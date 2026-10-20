// tft_lepton - live thermal video on the JC3248W535C via kampi/esp32-lepton
// (vendored + fixed, see components/esp32-lepton/CMakeLists.txt).
//
// Display (QSPI/AXS15231B + LVGL) and touch are back in the build --
// bsp_display.c/bsp_touch.c, previously excluded while the Lepton link was
// debugged in isolation. Captured frames are converted to RGB via the
// library's own Lepton_Raw14ToRGB() (palette LUT + auto min/max contrast,
// no need to reinvent that), upscaled 2x, and blitted into the LVGL canvas,
// flush to the top-left corner.
//
// This module is a confirmed non-radiometric Lepton 3.0 (part number
// 500-0726-01) -- Lepton_GetPixelTemperature() always returns
// LEPTON_ERR_NOT_SUPPORTED on it, so no temperature/touch-to-read feature.
//
// Right-side control strip (x=320..480, the empty margin next to the
// 320x240 image): Freeze/Resume, Save (writes a BMP to the on-board SD
// card), Gray/Color (toggles LEPTON_PALETTE_WHITEHOT vs LEPTON_PALETTE_IRON).
//
// The SD card now runs over native 4-bit SDMMC (CLK=12, CMD=11, D0=13,
// D1=41, D2=42, D3=10 -- see sd_card.cpp) on its own dedicated peripheral,
// after the board was rewired for true SDMMC lines. Earlier approaches
// (kept as history in the README's "SD card" section): first a hardware-
// SPI time-share with the Lepton's VoSPI (SPI3), since ESP32-S3 only has
// two general-purpose SPI controllers and both were already claimed
// (display QSPI on SPI2, Lepton VoSPI on SPI3) -- that paused the video
// feed ~2-3s per save; then bit-banged (plain GPIO) SPI once that pause
// became unacceptable, needing no SPI host at all but running deliberately
// slow. The card is mounted once at boot and stays mounted permanently
// either way: Save just writes a file, no Lepton pause required.

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/rtc_io.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lepton.h"

#include "app_state.h"
#include "bsp_display.h"
#include "bsp_touch.h"
#include "sd_card.h"
#include "web_server.h"
#include "wifi_setup.h"

static const char *TAG = "app";

// ---------------------------------------------------------------------------
// I2C HAL glue. The component doesn't own an I2C driver -- it calls back
// into these to talk to the CCI bus, so straight wrappers around ESP-IDF's
// i2c_master driver are all that's needed.
// ---------------------------------------------------------------------------

static int32_t I2CM_Init(i2c_master_bus_config_t *p_Config, i2c_master_bus_handle_t *p_Bus_Handle)
{
    esp_err_t err = i2c_new_master_bus(p_Config, p_Bus_Handle);
    ESP_LOGI(TAG, "I2CM_Init -> %s", esp_err_to_name(err));
    return err == ESP_OK ? 0 : -1;
}

static int32_t I2CM_Deinit(i2c_master_bus_handle_t Bus_Handle)
{
    esp_err_t err = i2c_del_master_bus(Bus_Handle);
    ESP_LOGI(TAG, "I2CM_Deinit -> %s", esp_err_to_name(err));
    return err == ESP_OK ? 0 : -1;
}

static int32_t I2CM_Write(i2c_master_dev_handle_t *p_Dev_Handle, const uint8_t *p_Buffer, uint32_t Length)
{
    esp_err_t err = i2c_master_transmit(*p_Dev_Handle, p_Buffer, Length, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2CM_Write(%" PRIu32 " bytes) -> %s", Length, esp_err_to_name(err));
    }
    return err == ESP_OK ? 0 : -1;
}

static int32_t I2CM_Read(i2c_master_dev_handle_t *p_Dev_Handle, uint8_t *p_Buffer, uint32_t Length)
{
    esp_err_t err = i2c_master_receive(*p_Dev_Handle, p_Buffer, Length, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2CM_Read(%" PRIu32 " bytes) -> %s", Length, esp_err_to_name(err));
    }
    return err == ESP_OK ? 0 : -1;
}

static int32_t I2CM_WriteRead(i2c_master_dev_handle_t *p_Dev_Handle,
                              const uint8_t *p_WriteData, uint32_t WriteLength,
                              uint8_t *p_ReadData, uint32_t ReadLength)
{
    esp_err_t err = i2c_master_transmit_receive(*p_Dev_Handle, p_WriteData, WriteLength,
                                                p_ReadData, ReadLength, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2CM_WriteRead(%" PRIu32 "->%" PRIu32 ") -> %s",
                 WriteLength, ReadLength, esp_err_to_name(err));
    }
    return err == ESP_OK ? 0 : -1;
}

// ---------------------------------------------------------------------------
// CCI I2C bus config. Separate from the display/touch I2C.
// ---------------------------------------------------------------------------
#define LEPTON_I2C_PORT  I2C_NUM_1
// i2c_master_bus_config_t.sda_io_num/scl_io_num are typed gpio_num_t, not
// int -- LEPTON_DEFAULT_I2C assigns these macros straight into those fields,
// and C++ (unlike C) won't implicitly convert a plain int literal to an enum.
#define PIN_LEPTON_SDA   GPIO_NUM_17
#define PIN_LEPTON_SCL   GPIO_NUM_18

// MOSFET load switch on the Lepton's own supply rail (hardware addition --
// this pin previously had no software meaning at all): 1 = powered,
// 0 = cut. Lets perform_shutdown() cut the sensor's power entirely instead
// of just telling it to standby over CCI (Lepton_PowerDown()) -- see the
// "Lepton power-down" README section for why that alone wasn't enough
// (the sensor has a fixed always-on rail there; this GPIO is what makes it
// switchable at all now).
#define PIN_LEPTON_POWER GPIO_NUM_6
#define LEPTON_POWER_ON_LEVEL  1

static i2c_master_bus_config_t s_i2c_bus_cfg = {};

// gpio_hold_en() + gpio_deep_sleep_hold_en() (perform_shutdown()) latch a
// pin's level THROUGH deep sleep -- but per gpio_hold_dis()'s own doc
// comment, that hold persists into the *next* boot too, silently
// overriding any gpio_set_level() call on that pin until gpio_hold_dis()
// is explicitly called first. Without this, the Lepton could never be
// powered back on after the first Shutdown (stuck latched at 0 forever).
// Configures the pin as output and drives `on` -- shared by the power-on
// path in app_main() and (driving off) perform_shutdown(), so both always
// release any stale hold before touching the level.
static void lepton_power_set(bool on)
{
    gpio_hold_dis(PIN_LEPTON_POWER);
    gpio_reset_pin(PIN_LEPTON_POWER);
    gpio_set_direction(PIN_LEPTON_POWER, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LEPTON_POWER, on ? LEPTON_POWER_ON_LEVEL : !LEPTON_POWER_ON_LEVEL);
}

// ---------------------------------------------------------------------------
// SD card. Pins (CLK=12, CMD=11, D0=13, D1=41, D2=42, D3=10) and the native
// SDMMC driver itself live in sd_card.cpp/h -- mounted once at boot, see
// app_main(). Its own dedicated peripheral, so no contention with the
// Lepton's VoSPI or the display's QSPI.
// ---------------------------------------------------------------------------
#define SD_MOUNT_POINT "/sdcard"

// Touch controller INT (active low); also the deep-sleep wake source.
#define PIN_TOUCH_INT GPIO_NUM_3

// ---------------------------------------------------------------------------
// Display state
// ---------------------------------------------------------------------------
#define BG_COLOR          0x003a57
#define THERMAL_SCALE     2   // 160x120 -> 320x240, anchored top-left

// Display name for each LEPTON_PALETTE_* value, in enum order (see
// lepton_palette.h) -- shown on the palette button/web control so cycling
// says what it just switched to, not just "next".
static const char *const kPaletteNames[LEPTON_PALETTE_COUNT] = {
    "Iron", "Rainbow", "Whitehot", "Blackhot", "Amber", "Arctic", "Lava",
};

// Right-side control strip: image is 320px wide, canvas is 480 -> x=320..480
// is empty regardless of frame content. 40/10 (not 48/12) so all 6 buttons
// fit the 320-tall panel: 12 + 6*(40+10) = 312, an 8px margin to the bottom
// edge.
#define BTN_X    328
#define BTN_W    140
#define BTN_H    40
#define BTN_GAP  10

static lv_obj_t *s_canvas;
static uint16_t *s_canvas_buf;
static int32_t s_canvas_w, s_canvas_h;
static lv_obj_t *s_status_label;
static uint8_t *s_rgb_buf;   // Lepton_Raw14ToRGB() scratch, PSRAM

static lv_obj_t *s_freeze_btn_label;
static lv_obj_t *s_palette_btn_label;
static volatile bool s_frozen = false;
// Whitehot (grayscale) is the default, per earlier request. Cycles through
// every LEPTON_PALETTE_* value on each button press, wrapping back to 0.
static volatile int s_palette_index = LEPTON_PALETTE_WHITEHOT;
// Set by save_btn_cb (LVGL/touch task), consumed by the main loop -- keeps
// SD writes off the LVGL/touch task, same as before, even though the save
// itself no longer touches the Lepton at all.
static volatile bool s_save_requested = false;
// Same request-flag pattern as s_save_requested, for the same reason: the
// CCI (I2C) interface isn't wrapped in its own mutex in the vendored
// component, so FFC requests (calibrate_btn_cb on the LVGL task,
// app_request_calibration() from the web server's task) are funneled
// through the one task that already owns `Device` instead of racing each
// other on the I2C bus directly.
static volatile bool s_calibrate_requested = false;
// Toggled from the web page only (no physical button) -- see app_state.h's
// app_toggle_streaming() comment for why the LCD and the web feed are
// mutually exclusive rather than both running at once.
static volatile bool s_web_streaming = false;
// Streaming screen-timeout state; see update_stream_screen_timeout().
static volatile bool s_screen_off = false;
static volatile bool s_touch_mask = false;
static volatile bool s_wake_touch = false;
static lv_indev_read_cb_t s_touch_orig_read_cb;
static void gated_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data);
static lv_obj_t *s_stop_stream_btn;
// Same request-flag pattern as s_save_requested/s_calibrate_requested; see
// perform_shutdown() for what it actually does. Set from a long-press on
// the physical button (not a plain tap -- see shutdown_btn_cb) or a
// confirm() dialog on the web page (app_request_shutdown()), since this is
// the one control on this device you can't undo without a physical reset.
static volatile bool s_shutdown_requested = false;

static void set_status(const char *text, uint32_t color);   // defined below, used by do_toggle_streaming

// Guards s_rgb_buf: it used to only ever be touched from this app's one
// task (Raw14ToRGB writes it, the very next lines blit-read it, all
// synchronous), but the web feed's app_build_frame_bmp() now reads it from
// the HTTP server's own task -- held for the whole render (convert+blit),
// not just the Raw14ToRGB call, so a concurrent BMP build can't observe a
// half-updated frame.
static SemaphoreHandle_t s_rgb_mutex;

// lvgl_port_lock() (bsp_display_lock()) is a recursive mutex, so it's safe
// to call unconditionally here regardless of caller: the LVGL touch
// callback (freeze_btn_cb) already runs under the port's own lock, the web
// server's task (app_toggle_freeze()) does not.
static void do_toggle_freeze(void)
{
    s_frozen = !s_frozen;
    bsp_display_lock();
    lv_label_set_text(s_freeze_btn_label, s_frozen ? "Resume" : "Freeze");
    bsp_display_unlock();
}

static void do_cycle_palette(void)
{
    s_palette_index = (s_palette_index + 1) % LEPTON_PALETTE_COUNT;
    bsp_display_lock();
    lv_label_set_text(s_palette_btn_label, kPaletteNames[s_palette_index]);
    bsp_display_unlock();
}

static void do_toggle_streaming(void)
{
    s_web_streaming = !s_web_streaming;
    // Otherwise the screen would just sit on whatever frame was last drawn
    // with no indication of why it stopped updating.
    set_status(s_web_streaming ? "Streaming to web..." : "", 0xffc400);
}

// app_state.h -- called from web_server.cpp (a different task) so the
// browser control page and the physical touchscreen drive the same state.
void app_toggle_freeze(void) { do_toggle_freeze(); }
void app_cycle_palette(void) { do_cycle_palette(); }
void app_toggle_streaming(void) { do_toggle_streaming(); }
bool app_is_streaming(void) { return s_web_streaming; }

void app_get_state(bool *frozen, const char **palette_name, bool *streaming)
{
    *frozen = s_frozen;
    *palette_name = kPaletteNames[s_palette_index];
    *streaming = s_web_streaming;
}

void app_request_calibration(void) { s_calibrate_requested = true; }

void app_request_shutdown(void) { s_shutdown_requested = true; }

// Full-screen mode: the image is rescaled to cover the whole canvas and
// every other UI element (buttons, status label) is hidden. Entered via the
// Full button, exited by a single tap anywhere on the canvas -- see
// canvas_click_cb(). All objects hidden/shown together via s_hideable_ui.
static volatile bool s_fullscreen = false;
#define NUM_HIDEABLE_UI 8
static lv_obj_t *s_hideable_ui[NUM_HIDEABLE_UI];

static lv_obj_t *s_wifi_label;   // bottom-right corner, shows the IP once connected (see wifi_setup.h)

static void freeze_btn_cb(lv_event_t *e)
{
    do_toggle_freeze();
}

static void save_btn_cb(lv_event_t *e)
{
    s_save_requested = true;
}

// FFC (Flat-Field Correction) -- the sensor recalibrates against its
// internal shutter, which is the standard fix for image retention/burn-in
// ("persistent image"). See s_calibrate_requested's comment for why this
// goes through a request flag rather than calling Lepton_RunCCI() directly.
static void calibrate_btn_cb(lv_event_t *e)
{
    s_calibrate_requested = true;
}

static void palette_btn_cb(lv_event_t *e)
{
    do_cycle_palette();
}

// Bound to LV_EVENT_LONG_PRESSED, not LV_EVENT_CLICKED (see make_button()'s
// call site) -- a stray tap here would deep-sleep the device (it wakes again
// on touch, see perform_shutdown()), so this needs deliberate intent, not a
// single touch, the same way the other buttons are fine to fire on.
static void shutdown_btn_cb(lv_event_t *e)
{
    s_shutdown_requested = true;
}

static void stop_stream_btn_cb(lv_event_t *e)
{
    if (s_web_streaming) {
        do_toggle_streaming();
    }
}

static void fullscreen_btn_cb(lv_event_t *e)
{
    s_fullscreen = true;
    for (int i = 0; i < NUM_HIDEABLE_UI; i++) {
        lv_obj_add_flag(s_hideable_ui[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// Hidden UI objects don't receive input, so this only fires from a tap that
// lands on the canvas itself -- correct in both modes: in normal mode the
// canvas only covers the image area (buttons/label are separate siblings
// drawn on top), in full-screen mode it's the only thing left visible.
static void canvas_click_cb(lv_event_t *e)
{
    if (!s_fullscreen) {
        return;
    }
    s_fullscreen = false;
    for (int i = 0; i < NUM_HIDEABLE_UI; i++) {
        lv_obj_remove_flag(s_hideable_ui[i], LV_OBJ_FLAG_HIDDEN);
    }
    // Normal-mode rendering only touches the top-left 320x240 -- clear the
    // full-screen image data left over everywhere else so it doesn't show
    // through gaps between the now-visible widgets until the next frame.
    if (s_canvas_buf) {
        bsp_display_lock();
        lv_canvas_fill_bg(s_canvas, lv_color_hex(BG_COLOR), LV_OPA_COVER);
        lv_obj_invalidate(s_canvas);
        bsp_display_unlock();
    }
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int32_t y, lv_event_cb_t cb,
                              lv_obj_t **out_label, lv_event_code_t event_code = LV_EVENT_CLICKED)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, BTN_W, BTN_H);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, BTN_X, y);
    lv_obj_add_event_cb(btn, cb, event_code, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (out_label) {
        *out_label = label;
    }
    return btn;   // returned (rather than the label) so callers can hide/show the whole button
}

static void start_display(void)
{
    lv_display_t *disp = bsp_display_start();
    lv_indev_t *touch_indev = bsp_touch_start(disp);
    s_touch_orig_read_cb = lv_indev_get_read_cb(touch_indev);
    lv_indev_set_read_cb(touch_indev, gated_touch_read_cb);

    bsp_display_lock();

    s_canvas_w = lv_display_get_horizontal_resolution(disp);
    s_canvas_h = lv_display_get_vertical_resolution(disp);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(BG_COLOR), LV_PART_MAIN);

    s_canvas_buf = static_cast<uint16_t *>(
        heap_caps_aligned_alloc(64, (size_t)s_canvas_w * s_canvas_h * 2, MALLOC_CAP_SPIRAM));
    if (!s_canvas_buf) {
        ESP_LOGE(TAG, "canvas buffer allocation failed");
        bsp_display_unlock();
        return;
    }
    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, s_canvas_w, s_canvas_h, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(s_canvas, lv_color_hex(BG_COLOR), LV_OPA_COVER);
    lv_obj_set_pos(s_canvas, 0, 0);
    // Clickable so canvas_click_cb() can catch a tap anywhere on it to exit
    // full-screen mode -- not clickable by default like lv_button_create().
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, canvas_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *freeze_btn = make_button(scr, "Freeze", 12, freeze_btn_cb, &s_freeze_btn_label);
    lv_obj_t *save_btn = make_button(scr, "Save", 12 + (BTN_H + BTN_GAP) * 1, save_btn_cb, nullptr);
    // Cycles through every LEPTON_PALETTE_* value (do_cycle_palette());
    // label shows the palette that's now active, not the next one, since
    // "the next of 7" doesn't read as clearly as Freeze/Resume's two-state
    // action-not-state convention did.
    lv_obj_t *palette_btn = make_button(scr, kPaletteNames[s_palette_index],
                                         12 + (BTN_H + BTN_GAP) * 2, palette_btn_cb,
                                         &s_palette_btn_label);
    lv_obj_t *full_btn = make_button(scr, "Full", 12 + (BTN_H + BTN_GAP) * 3, fullscreen_btn_cb,
                                      nullptr);
    lv_obj_t *calibrate_btn = make_button(scr, "Calib", 12 + (BTN_H + BTN_GAP) * 4, calibrate_btn_cb,
                                           nullptr);
    // Long-press, not a plain tap, since this is the one control here you
    // can't undo without a physical reset/power-cycle -- see
    // shutdown_btn_cb's comment.
    lv_obj_t *shutdown_btn = make_button(scr, "Shutdown", 12 + (BTN_H + BTN_GAP) * 5,
                                          shutdown_btn_cb, nullptr, LV_EVENT_LONG_PRESSED);

    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "Waiting for\nLepton...");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    // Left side of the screen, under the image (which only occupies
    // y=0..240 of the 320-tall canvas) -- not the right-side button strip.
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_LEFT, 12, -12);

    // Hidden until wifi_setup_get_ip() reports a connection (see app_main's
    // main loop) -- empty text collapses it to zero size, so an unused
    // label doesn't reserve visible space in the corner.
    s_wifi_label = lv_label_create(scr);
    lv_label_set_text(s_wifi_label, "");
    lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(s_wifi_label, LV_ALIGN_BOTTOM_RIGHT, -12, -12);

    // Only shown while streaming to the web (and not full-screen); visibility
    // is managed by update_stream_screen_timeout(), so it's deliberately not
    // in s_hideable_ui (whose restore path would unhide it unconditionally).
    s_stop_stream_btn = make_button(scr, "Stop stream", 0, stop_stream_btn_cb, nullptr);
    lv_obj_set_width(s_stop_stream_btn, 120);
    lv_obj_align(s_stop_stream_btn, LV_ALIGN_BOTTOM_LEFT, 196, -12);
    lv_obj_add_flag(s_stop_stream_btn, LV_OBJ_FLAG_HIDDEN);

    s_hideable_ui[0] = freeze_btn;
    s_hideable_ui[1] = save_btn;
    s_hideable_ui[2] = palette_btn;
    s_hideable_ui[3] = full_btn;
    s_hideable_ui[4] = calibrate_btn;
    s_hideable_ui[5] = shutdown_btn;
    s_hideable_ui[6] = s_status_label;
    s_hideable_ui[7] = s_wifi_label;

    bsp_display_unlock();

    // Backlight stays off (bsp_display_start()) until LVGL has flushed this
    // first frame over the panel's stale GRAM -- a few refresh periods.
    vTaskDelay(pdMS_TO_TICKS(400));
    bsp_display_backlight(true);
}

static void set_status(const char *text, uint32_t color)
{
    bsp_display_lock();
    lv_label_set_text(s_status_label, text);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(color), LV_PART_MAIN);
    bsp_display_unlock();
}

// Converts the raw frame via the library's own palette+auto-contrast helper,
// then upscales/blits straight into the canvas's backing buffer.
static void render_thermal_frame(Lepton_t *device, const Lepton_FrameBuffer_t &frame)
{
    if (!s_rgb_buf || !s_canvas_buf) {
        return;
    }
    const int palette = s_palette_index;

    // Held for the whole conversion+blit -- see s_rgb_mutex's own comment.
    xSemaphoreTake(s_rgb_mutex, portMAX_DELAY);
    if (!Lepton_Raw14ToRGB(device, frame.ImageBuffer, s_rgb_buf, nullptr, nullptr,
                          frame.Width, frame.Height, Lepton_Palette_Table[palette])) {
        xSemaphoreGive(s_rgb_mutex);
        return;
    }

    // Skip the LCD entirely while streaming to the web -- see
    // app_toggle_streaming()'s comment in app_state.h for why. Raw14ToRGB
    // above still ran, so s_rgb_buf stays live for the web feed either way.
    if (!s_web_streaming) {
        // Top-left aligned in normal mode (320x240 at integer 2x, no
        // centering offset); full-screen mode instead covers the whole
        // canvas. 160x120's 4:3 aspect doesn't evenly divide into the
        // 480x320 (3:2) panel, so full-screen stretches rather than
        // letterboxing -- destination-pixel-driven nearest-neighbor
        // sampling (as opposed to the old source-pixel-driven replicate
        // loop) handles both the exact-2x normal case and the non-integer
        // full-screen scale with the same code.
        const int dst_w = s_fullscreen ? s_canvas_w : frame.Width * THERMAL_SCALE;
        const int dst_h = s_fullscreen ? s_canvas_h : frame.Height * THERMAL_SCALE;

        bsp_display_lock();

        for (int dy = 0; dy < dst_h; dy++) {
            const int sy = (dy * frame.Height) / dst_h;
            uint16_t *row = s_canvas_buf + dy * s_canvas_w;
            for (int dx = 0; dx < dst_w; dx++) {
                const int sx = (dx * frame.Width) / dst_w;
                const uint8_t *p = &s_rgb_buf[(sy * frame.Width + sx) * 3];
                row[dx] = lv_color_to_u16(lv_color_make(p[0], p[1], p[2]));
            }
        }
        lv_obj_invalidate(s_canvas);
        bsp_display_unlock();
    }

    xSemaphoreGive(s_rgb_mutex);
}

// While streaming to the web the LCD isn't updated (see render_thermal_frame),
// so after STREAM_SCREEN_OFF_MS without a touch the backlight is switched off.
// It comes back on when streaming stops or a touch is detected (a touch also
// restarts the countdown). Called from the main loop only.
#define STREAM_SCREEN_OFF_MS 60000

// The touch that wakes a dark screen must not also press a button: while
// s_touch_mask is set the real indev's reads are reported as RELEASED, and a
// press seen while the screen is off is recorded in s_wake_touch instead.
// The mask is dropped on the first release after the screen is back on.
static void gated_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    s_touch_orig_read_cb(indev, data);
    if (!s_touch_mask) {
        return;
    }
    if (data->state == LV_INDEV_STATE_PRESSED) {
        if (s_screen_off) {
            s_wake_touch = true;
        }
    } else if (!s_screen_off) {
        s_touch_mask = false;
    }
    data->state = LV_INDEV_STATE_RELEASED;
}

static void update_stream_screen_timeout(void)
{
    static bool was_streaming = false;
    static TickType_t idle_since = 0;

    const TickType_t now = xTaskGetTickCount();

    static bool stop_btn_shown = false;
    const bool want_stop_btn = s_web_streaming && !s_fullscreen;
    if (s_stop_stream_btn && want_stop_btn != stop_btn_shown) {
        stop_btn_shown = want_stop_btn;
        bsp_display_lock();
        if (want_stop_btn) {
            lv_obj_remove_flag(s_stop_stream_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_stop_stream_btn, LV_OBJ_FLAG_HIDDEN);
        }
        bsp_display_unlock();
    }

    if (!s_web_streaming) {
        if (s_screen_off) {
            s_screen_off = false;
            s_wake_touch = false;
            bsp_display_backlight(true);   // mask drops on the next release
        }
        was_streaming = false;
        return;
    }

    if (!was_streaming) {
        was_streaming = true;
        idle_since = now;
    }

    if (s_screen_off) {
        if (s_wake_touch) {
            s_wake_touch = false;
            s_screen_off = false;
            idle_since = now;
            bsp_display_backlight(true);
        }
        return;
    }

    // LVGL resets the display's inactivity timer on every (unmasked) touch.
    bsp_display_lock();
    const uint32_t inactive_ms = lv_display_get_inactive_time(NULL);
    bsp_display_unlock();
    const uint32_t since_idle_ms = (now - idle_since) * portTICK_PERIOD_MS;

    if (inactive_ms + 100 < since_idle_ms) {
        idle_since = now - pdMS_TO_TICKS(inactive_ms);   // touched since last look
    } else if (since_idle_ms >= STREAM_SCREEN_OFF_MS) {
        s_touch_mask = true;
        s_screen_off = true;
        bsp_display_backlight(false);
    }
}

// Same header/pixel layout as write_bmp() below (bottom-up rows, BGR order)
// but into memory instead of a file, for the web server (app_state.h).
// Shared core for app_build_frame_bmp() (mallocs, for one-off use like the
// /save download) and app_build_frame_bmp_into() (caller-supplied buffer,
// for streaming).
static bool encode_frame_bmp(uint8_t *out, size_t out_cap, size_t *out_len, int dst_w, int dst_h)
{
    if (!s_rgb_buf) {
        return false;
    }
    const int src_w = LEPTON_IMAGE_WIDTH;
    const int src_h = LEPTON_IMAGE_HEIGHT;
    const uint32_t row_bytes = (uint32_t)dst_w * 3;
    const uint32_t image_size = row_bytes * (uint32_t)dst_h;
    const uint32_t file_size = 14 + 40 + image_size;
    if (out_cap < file_size) {
        return false;
    }

    uint8_t *p = out;
    p[0] = 'B'; p[1] = 'M';
    memcpy(&p[2], &file_size, 4);
    memset(&p[6], 0, 4);
    const uint32_t data_offset = 14 + 40;
    memcpy(&p[10], &data_offset, 4);
    p += 14;

    memset(p, 0, 40);
    p[0] = 40;
    memcpy(&p[4], &dst_w, 4);
    memcpy(&p[8], &dst_h, 4);
    p[12] = 1;
    p[14] = 24;
    memcpy(&p[20], &image_size, 4);
    p += 40;

    xSemaphoreTake(s_rgb_mutex, portMAX_DELAY);
    for (int dy = dst_h - 1; dy >= 0; dy--) {   // BMP rows are bottom-up
        const int sy = (dy * src_h) / dst_h;
        const uint8_t *src_row = &s_rgb_buf[(size_t)sy * src_w * 3];
        for (int dx = 0; dx < dst_w; dx++) {
            const int sx = (dx * src_w) / dst_w;
            const uint8_t *sp = &src_row[sx * 3];
            p[dx * 3 + 0] = sp[2];   // B
            p[dx * 3 + 1] = sp[1];   // G
            p[dx * 3 + 2] = sp[0];   // R
        }
        p += row_bytes;
    }
    xSemaphoreGive(s_rgb_mutex);

    *out_len = file_size;
    return true;
}

size_t app_frame_bmp_size(void)
{
    return 14 + 40 + (size_t)LEPTON_IMAGE_WIDTH * LEPTON_IMAGE_HEIGHT * 3;
}

uint8_t *app_build_frame_bmp(size_t *out_len)
{
    const size_t cap = app_frame_bmp_size();
    uint8_t *out = static_cast<uint8_t *>(malloc(cap));
    if (!out) {
        return nullptr;
    }
    if (!encode_frame_bmp(out, cap, out_len, LEPTON_IMAGE_WIDTH, LEPTON_IMAGE_HEIGHT)) {
        free(out);
        return nullptr;
    }
    return out;
}

bool app_build_frame_bmp_into(uint8_t *buf, size_t buf_cap, size_t *out_len)
{
    return encode_frame_bmp(buf, buf_cap, out_len, LEPTON_IMAGE_WIDTH, LEPTON_IMAGE_HEIGHT);
}

// ---------------------------------------------------------------------------
// SD card save
// ---------------------------------------------------------------------------

// Builds the whole BMP (header + BGR bottom-up pixels, via encode_frame_bmp()
// -- same routine the web server's /save download uses) into one PSRAM
// buffer, then writes it with a single fwrite(). Replaced an earlier
// per-row version (120 separate ~480-byte fwrite calls) that turned out to
// be the actual bottleneck behind a ~3s save over SDMMC -- one write of the
// whole ~57KB file lets the FatFS/SDMMC stack move it as a handful of large
// sector-sized transfers instead of 120 small ones, each of which was
// apparently paying fixed per-call overhead unrelated to the bus clock
// (removing the earlier SDMMC_FREQ_PROBING clock cap, the first thing
// tried, made no difference -- see README's SD card section).
static esp_err_t write_bmp(const char *path)
{
    size_t len = 0;
    uint8_t *buf = app_build_frame_bmp(&len);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed", path);
        free(buf);
        return ESP_FAIL;
    }

    const esp_err_t err = (fwrite(buf, len, 1, f) == 1) ? ESP_OK : ESP_FAIL;
    fclose(f);
    free(buf);
    return err;
}

static uint32_t s_save_counter = 0;

// Scans for existing lepton_NNNN.bmp files so numbering continues where it
// left off after a reboot instead of restarting at 1 and overwriting
// whatever's already on the card. Called once, right after mount.
static void init_save_counter(void)
{
    uint32_t max_index = 0;
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) {
        ESP_LOGW(TAG, "init_save_counter: opendir(%s) failed", SD_MOUNT_POINT);
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        uint32_t index;
        if (sscanf(entry->d_name, "lepton_%" SCNu32 ".bmp", &index) == 1) {
            if (index > max_index) {
                max_index = index;
            }
        }
    }
    closedir(dir);
    s_save_counter = max_index;
    ESP_LOGI(TAG, "init_save_counter: next save will be lepton_%04" PRIu32 ".bmp",
             s_save_counter + 1);
}

static esp_err_t save_current_frame_to_sd(void)
{
    if (!s_rgb_buf) {
        return ESP_ERR_INVALID_STATE;
    }

    char path[64];
    snprintf(path, sizeof(path), SD_MOUNT_POINT "/lepton_%04" PRIu32 ".bmp", ++s_save_counter);

    esp_err_t err = write_bmp(path);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved %s", path);
        char msg[32];
        snprintf(msg, sizeof(msg), "Saved lepton_%04" PRIu32 ".bmp", s_save_counter);
        set_status(msg, 0x4caf50);
    } else {
        ESP_LOGE(TAG, "write_bmp(%s) failed: %s", path, esp_err_to_name(err));
    }
    return err;
}

// The SD card is mounted permanently (native SDMMC, its own dedicated
// peripheral -- see sd_card.cpp), so a save is just a file write; the
// Lepton is never touched.
static void perform_save(void)
{
    set_status("Saving...", 0xffc400);
    esp_err_t err = save_current_frame_to_sd();
    if (err != ESP_OK) {
        char msg[48];
        snprintf(msg, sizeof(msg), "SD save failed: %s", esp_err_to_name(err));
        set_status(msg, 0xff5252);
    }
}

// FFC (Flat-Field Correction): the sensor closes its internal shutter and
// recalibrates against it, the standard fix for image retention/burn-in.
// Pure CCI (I2C) traffic -- doesn't touch VoSPI/SPI3, so it doesn't need
// the pause-and-restart dance perform_save() used to need for the SD card.
static void perform_calibration(Lepton_t *device)
{
    set_status("Calibrating...", 0xffc400);
    Lepton_Error_t err = Lepton_RunCCI(device);
    if (err == LEPTON_ERR_OK) {
        set_status("Calibration done", 0x4caf50);
    } else {
        char msg[48];
        snprintf(msg, sizeof(msg), "Calibration failed: 0x%" PRIX32, (uint32_t)err);
        set_status(msg, 0xff5252);
    }
}

// Stops the Lepton, blanks the panel/backlight, then deep-sleeps. Never
// returns -- esp_deep_sleep_start() halts the chip; on wake it's a fresh
// boot (app_main from the top), not a resume of this call. Wake source: a
// touch, via the controller's INT pin on GPIO3 (ext1, active low); the
// EN/reset button or a power cycle also still work.
//
// The sensor originally had a fixed, always-on power supply (see the wiring
// notes above), so freeing CCI/VoSPI's GPIOs on the ESP32 side alone
// (Lepton_Deinit(), below) never reduced its own current draw --
// Lepton_PowerDown() (CCI_CMD_OEM_RUN_POWER_DOWN) at least tells the sensor
// itself to go into its low-power state before we let go of it. A MOSFET
// load switch was later added on that supply rail (PIN_LEPTON_POWER), so
// this now also cuts the sensor's power outright -- Lepton_PowerDown() is
// still sent first regardless, so a graceful CCI shutdown happens before
// the rail actually drops, not instead of it. Order matters: StopCapture()
// first so the capture task isn't still trying to read VoSPI frames right
// as the camera goes silent (harmless, but would spam a burst of sync-error
// warnings for no reason); PowerDown() before Deinit() since it needs the
// CCI (I2C) bus Deinit() frees; Deinit() before the actual power cut so
// nothing's mid-transaction when the rail drops; the power cut itself last
// of the three regardless of whether PowerDown() succeeded, so a failed CCI
// command never leaves the sensor powered for no reason.
static void perform_shutdown(Lepton_t *device)
{
    // Black screen for 2s so the finger from the long-press is lifted (INT
    // would otherwise still be low and wake the chip at once).
    bsp_display_backlight(false);
    vTaskDelay(pdMS_TO_TICKS(2000));

    Lepton_StopCapture(device);

    Lepton_Error_t pd_err = Lepton_PowerDown(device);
    if (pd_err != LEPTON_ERR_OK) {
        ESP_LOGW(TAG, "Lepton_PowerDown failed: 0x%" PRIX32 " (continuing shutdown anyway)",
                 (uint32_t)pd_err);
    }

    Lepton_Deinit(device);

    lepton_power_set(false);
    // Otherwise the level set above wouldn't survive actual deep sleep --
    // see gpio_hold_en()'s own doc comment (ESP32-S3 specifically resets
    // digital GPIOs to their default state on deep sleep unless held) and
    // lepton_power_set()'s comment on why the *next* boot also has to
    // release this hold before it can drive the pin high again.
    gpio_hold_en(PIN_LEPTON_POWER);

    // Cleanly stops the radio rather than just relying on deep sleep's hard
    // power cutoff -- a no-op if wifi.json was never found/valid.
    wifi_setup_stop();

    bsp_display_lock();
    bsp_display_shutdown();   // also holds the backlight pin low through sleep
    bsp_display_unlock();

    // i2c_master's own bus teardown (Lepton_Deinit() above, and whatever
    // bsp_touch.c set up) disables the SDA/SCL pins' output/input signal
    // routing but -- verified against esp_driver_i2c/i2c_common.c's
    // i2c_common_deinit_pins() -- never touches their pull configuration,
    // so an internal pull-up enabled at bus-creation time (CCI here, touch
    // in bsp_touch.c, both created with enable_internal_pullup=true) stays
    // configured and keeps drawing a small leakage current through deep
    // sleep. rtc_gpio_isolate() disconnects a pin's input/output buffers
    // and pull resistors entirely -- the ESP-IDF-documented fix for
    // exactly this ("to reduce ... current consumption"). Safe regardless
    // of whether each pin's own driver was cleanly deinitialized first:
    // every GPIO gets freshly reconfigured on the next boot anyway, since
    // waking from this deep sleep is a full reset, not a resume.
    //
    // The SD card's pins (sd_card.cpp) don't need this: that driver never
    // enables any ESP32-internal pull-up (the microSD breakout board has
    // its own), and an external pull-up's own current draw isn't affected
    // by whether the ESP32 side isolates its pad or not -- there's nothing
    // here for rtc_gpio_isolate() to actually save. (The bit-banged SPI
    // driver this replaced did enable one, on its MISO pin -- this is why
    // an isolate call for that pin used to live here.)
    rtc_gpio_isolate(PIN_LEPTON_SDA);   // CCI I2C (this file)
    rtc_gpio_isolate(PIN_LEPTON_SCL);
    rtc_gpio_isolate(GPIO_NUM_4);       // touch I2C SDA (bsp_touch.c)
    rtc_gpio_isolate(GPIO_NUM_8);       // touch I2C SCL

    // Wake on touch: the controller's INT line (GPIO3, RTC-capable) goes low
    // on touch. GPIO3 is a strapping pin read only at reset, so it's fine as
    // a wake source. Wait for it to be released first -- the Shutdown
    // long-press is itself a touch, and sleeping while INT is still low would
    // wake the chip immediately. The RTC pull-up keeps the line high when the
    // controller isn't driving it.
    gpio_set_direction(PIN_TOUCH_INT, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_TOUCH_INT, GPIO_PULLUP_ONLY);
    for (int i = 0; i < 100 && gpio_get_level(PIN_TOUCH_INT) == 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    rtc_gpio_init(PIN_TOUCH_INT);
    rtc_gpio_set_direction(PIN_TOUCH_INT, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(PIN_TOUCH_INT);
    rtc_gpio_pulldown_dis(PIN_TOUCH_INT);
    esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_TOUCH_INT, ESP_EXT1_WAKEUP_ANY_LOW);

    esp_task_wdt_delete(NULL);
    esp_deep_sleep_start();
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "==== tft_lepton ====");

    // As early as possible so the sensor gets the most of the "wait for
    // Lepton to boot" delay below as actual power-on time, not time added
    // on top of it.
    lepton_power_set(true);

    // rtc_gpio_isolate() (perform_shutdown()) disconnects a pin from the
    // digital IOMUX entirely at the RTC/LP_IO mux level -- undoing that
    // specifically needs rtc_gpio_deinit(), which plain gpio_config()/
    // i2c_new_master_bus() calls do NOT do on their own. Checked directly:
    // esp_driver_i2c's standard-I2C pin setup (s_hp_i2c_pins_config(),
    // i2c_common.c) never calls gpio_reset_pin() or rtc_gpio_deinit(), so
    // without this, touch (bsp_touch.c, below) and CCI I2C (this file,
    // further down) would stay silently broken on the very first boot
    // after a Shutdown. gpio_reset_pin() calls rtc_gpio_deinit()
    // internally for any RTC-capable pin (confirmed against
    // esp_driver_gpio/src/gpio.c) -- harmless no-op if a pin was never
    // isolated to begin with. The SD card's pins don't need this here --
    // sd_card.cpp's driver was never isolated in the first place (see
    // perform_shutdown()'s comment on why).
    //
    // rtc_gpio_isolate() also latches the pad's hold, and rtc_gpio_deinit()/
    // gpio_reset_pin() don't release it -- a held pad ignores all later
    // configuration. Before touch-wake this was masked because the only way
    // back was EN, which clears holds; a deep-sleep wake keeps them, leaving
    // the touch and CCI I2C pins dead (Lepton "Failed to access Status
    // register"). Release them explicitly, plus the global deep-sleep hold
    // and the wake pin itself.
    gpio_deep_sleep_hold_dis();
    for (gpio_num_t p : {GPIO_NUM_4, GPIO_NUM_8, PIN_LEPTON_SDA, PIN_LEPTON_SCL, PIN_TOUCH_INT}) {
        rtc_gpio_hold_dis(p);
    }
    gpio_reset_pin(PIN_TOUCH_INT);
    gpio_reset_pin(GPIO_NUM_4);     // touch I2C SDA (bsp_touch.c)
    gpio_reset_pin(GPIO_NUM_8);     // touch I2C SCL
    gpio_reset_pin(PIN_LEPTON_SDA); // CCI I2C SDA
    gpio_reset_pin(PIN_LEPTON_SCL); // CCI I2C SCL

    s_rgb_mutex = xSemaphoreCreateMutex();

    // Lepton_Raw14ToRGB() (called below from this task, not the library's own
    // capture task) calls esp_task_wdt_reset() internally during its min/max
    // scan and row loop, assuming the caller is watchdog-registered like the
    // capture task is. Without this, every one of those resets fails with
    // "task not found" -- dozens of times per frame. Registering here also
    // gives real hang protection for this task, not just quiet logs.
    esp_task_wdt_add(NULL);

    start_display();
    esp_task_wdt_reset();

    esp_err_t sd_err = sd_card_mount(SD_MOUNT_POINT);
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed: %s (Save will fail until this is fixed)",
                 esp_err_to_name(sd_err));
    } else {
        init_save_counter();
        wifi_setup_start(SD_MOUNT_POINT "/wifi.json");
        // Harmless to start even if wifi.json wasn't found/valid -- the
        // server just sits unreachable until (if ever) an IP is assigned.
        esp_err_t web_err = web_server_start();
        if (web_err != ESP_OK) {
            ESP_LOGW(TAG, "web_server_start failed: %s", esp_err_to_name(web_err));
        }
    }
    esp_task_wdt_reset();

    // Bit-banged SD init/mount plus this delay were together enough to trip
    // the 5s task watchdog (registered above) before either existed on its
    // own -- split into chunks with resets in between rather than one flat
    // blocking delay, so startup timing changes can't silently reintroduce
    // the same trip.
    ESP_LOGI(TAG, "Waiting for Lepton to boot...");
    for (int i = 0; i < 4; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_task_wdt_reset();
    }

    Lepton_Conf_t Config = LEPTON_DEFAULT_CONF;
    LEPTON_ASSIGN_I2C_FUNC(Config, I2CM_Init, I2CM_Deinit, I2CM_Write, I2CM_Read, I2CM_WriteRead);
    Config.CCI.I2C_Bus_Config = &s_i2c_bus_cfg;
    LEPTON_DEFAULT_I2C(Config, LEPTON_I2C_PORT, PIN_LEPTON_SDA, PIN_LEPTON_SCL, true, false);

    ESP_LOGI(TAG, "CCI I2C: port=%d SDA=%d SCL=%d", LEPTON_I2C_PORT, PIN_LEPTON_SDA, PIN_LEPTON_SCL);
#if defined(CONFIG_LEPTON_VOSPI_SPI3_HOST)
    ESP_LOGI(TAG, "VoSPI: host=SPI3 CS=%d SCK=%d MISO=%d",
             CONFIG_LEPTON_VOSPI_CS, CONFIG_LEPTON_VOSPI_SCK, CONFIG_LEPTON_VOSPI_MISO);
#else
    ESP_LOGI(TAG, "VoSPI: host=SPI2 CS=%d SCK=%d MISO=%d",
             CONFIG_LEPTON_VOSPI_CS, CONFIG_LEPTON_VOSPI_SCK, CONFIG_LEPTON_VOSPI_MISO);
#endif

    // static, not a plain local: Lepton_Init() never touches
    // Internal.CapHandle, so an uninitialized stack Lepton_t leaves it as
    // garbage -- if that garbage happens to be non-NULL, Lepton_StartCapture()
    // sees "already capturing" and fails with LEPTON_ERR_BUSY (0xB008) on the
    // very first call. static gives C++ zero-init, matching the component's
    // own examples (which also use a static/global Lepton_t).
    static Lepton_t Device;
    Lepton_Error_t Error = Lepton_Init(&Device, &Config);
    if (Error != LEPTON_ERR_OK) {
        ESP_LOGE(TAG, "Lepton_Init failed: 0x%" PRIX32, (uint32_t)Error);
        ESP_LOGE(TAG, "Check wiring/power and the VoSPI pin settings in sdkconfig "
                       "(menuconfig -> Component config -> ESP32-Lepton).");
        char msg[48];
        snprintf(msg, sizeof(msg), "Lepton_Init failed: 0x%" PRIX32, (uint32_t)Error);
        set_status(msg, 0xff5252);
        esp_task_wdt_delete(NULL);   // stopping here; don't leave this task subscribed
        return;
    }
    ESP_LOGI(TAG, "Lepton_Init ok");

    char SerialStr[24];
    snprintf(SerialStr, sizeof(SerialStr), "%02X%02X-%02X%02X-%02X%02X-%02X%02X",
             Device.SerialNumber[0], Device.SerialNumber[1],
             Device.SerialNumber[2], Device.SerialNumber[3],
             Device.SerialNumber[4], Device.SerialNumber[5],
             Device.SerialNumber[6], Device.SerialNumber[7]);
    ESP_LOGI(TAG, "  Part number:   %s", Device.PartNumber);
    ESP_LOGI(TAG, "  Serial number: %s", SerialStr);

    s_rgb_buf = static_cast<uint8_t *>(
        heap_caps_malloc(LEPTON_IMAGE_WIDTH * LEPTON_IMAGE_HEIGHT * 3, MALLOC_CAP_SPIRAM));
    if (!s_rgb_buf) {
        ESP_LOGE(TAG, "RGB scratch buffer allocation failed");
        set_status("Out of memory (RGB buffer)", 0xff5252);
        esp_task_wdt_delete(NULL);
        return;
    }

    // Must be length 1: Lepton_CaptureTask() pushes frames with
    // xQueueOverwrite(), which FreeRTOS only allows on length-1 queues
    // (asserts otherwise) -- it's overwrite-latest semantics, not a real
    // queue, so "2 for double buffering" doesn't apply here.
    QueueHandle_t RawFrameQueue = xQueueCreate(1, sizeof(Lepton_FrameBuffer_t));
    if (!RawFrameQueue) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        set_status("Out of memory (frame queue)", 0xff5252);
        esp_task_wdt_delete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Starting capture...");
    Error = Lepton_StartCapture(&Device, RawFrameQueue);
    if (Error != LEPTON_ERR_OK) {
        ESP_LOGE(TAG, "Lepton_StartCapture failed: 0x%" PRIX32, (uint32_t)Error);
        char msg[48];
        snprintf(msg, sizeof(msg), "Lepton_StartCapture failed: 0x%" PRIX32, (uint32_t)Error);
        set_status(msg, 0xff5252);
        esp_task_wdt_delete(NULL);
        return;
    }
    set_status("", 0xffffff);   // clear "Waiting for Lepton..." now that it's live

    uint32_t frames = 0;
    uint32_t timeouts = 0;
    TickType_t last_report = xTaskGetTickCount();
    Lepton_FrameBuffer_t RawFrame;
    bool wifi_ip_shown = false;

    for (;;) {
        // Pet our own registration every iteration (1s cadence, well under
        // the watchdog timeout) so a stretch of receive timeouts -- no
        // Lepton_Raw14ToRGB() calls, hence no resets from there -- can't trip
        // a real watchdog panic on top of a merely-slow resync.
        esp_task_wdt_reset();

        if (s_shutdown_requested) {
            perform_shutdown(&Device);   // never returns
        }

        if (s_save_requested) {
            s_save_requested = false;
            perform_save();
            continue;   // skip this iteration's receive; frame queue kept filling meanwhile
        }

        if (s_calibrate_requested) {
            s_calibrate_requested = false;
            perform_calibration(&Device);
            continue;
        }

        update_stream_screen_timeout();

        if (xQueueReceive(RawFrameQueue, &RawFrame, pdMS_TO_TICKS(1000)) == pdTRUE) {
            frames++;
            // Still drain the queue while frozen (keeps frame/timeout counts
            // and the watchdog honest) -- just skip updating the canvas, so
            // it keeps showing whatever was last rendered.
            if (!s_frozen) {
                render_thermal_frame(&Device, RawFrame);
            }
        } else {
            timeouts++;
            ESP_LOGW(TAG, "No frame received in 1s (timeouts=%" PRIu32 ")", timeouts);
        }

        if (xTaskGetTickCount() - last_report >= pdMS_TO_TICKS(3000)) {
            last_report = xTaskGetTickCount();
            const int32_t sync_errors = Lepton_GetSyncErrors(&Device);
            ESP_LOGI(TAG, "frames=%" PRIu32 " timeouts=%" PRIu32
                     " sync_errors=%" PRId32 " driver_frame_counter=%" PRId32,
                     frames, timeouts, sync_errors, Lepton_GetFrameCounter(&Device));

            if (!wifi_ip_shown) {
                char ip_str[16];
                if (wifi_setup_get_ip(ip_str, sizeof(ip_str))) {
                    wifi_ip_shown = true;
                    bsp_display_lock();
                    lv_label_set_text(s_wifi_label, ip_str);
                    lv_obj_align(s_wifi_label, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
                    bsp_display_unlock();
                }
            }
        }
    }
}
