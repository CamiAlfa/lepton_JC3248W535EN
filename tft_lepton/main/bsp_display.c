#include "bsp_display.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "axs15231b_jc3248w535_init.h"

static const char *TAG = "bsp_display";

// ---------------------------------------------------------------------------
// JC3248W535C pin map  (AXS15231B, QSPI 4-data-line interface)
// ---------------------------------------------------------------------------
#define LCD_HOST            SPI2_HOST

#define PIN_NUM_LCD_CS      45
#define PIN_NUM_LCD_PCLK    47
#define PIN_NUM_LCD_DATA0   21
#define PIN_NUM_LCD_DATA1   48
#define PIN_NUM_LCD_DATA2   40
#define PIN_NUM_LCD_DATA3   39
#define PIN_NUM_LCD_RST     (-1)
#define PIN_NUM_LCD_BL      1
#define LCD_BL_ON_LEVEL     1

#define LCD_BITS_PER_PIXEL  16
#define LCD_MAX_TRANSFER_SZ (BSP_LCD_V_RES * 10 * sizeof(uint16_t))

static esp_lcd_panel_io_handle_t s_io_handle;
static esp_lcd_panel_handle_t    s_panel_handle;

static void backlight_init(void)
{
    // bsp_display_shutdown() latches this pin low through deep sleep via
    // gpio_hold_en() -- per that function's own doc comment, the hold
    // persists into the *next* boot too and silently overrides
    // gpio_set_level() below until explicitly released. Without this, the
    // backlight would stay stuck off after the first Shutdown+wake cycle.
    // Harmless no-op if the pin was never held (first boot ever, or a
    // reset that wasn't preceded by Shutdown).
    gpio_hold_dis(PIN_NUM_LCD_BL);

    const gpio_config_t bk_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_NUM_LCD_BL,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_cfg));
    // Starts off: the panel's GRAM still holds the previous image after a
    // deep-sleep wake, so the caller turns it on via bsp_display_backlight()
    // once the first frame has been rendered.
    gpio_set_level(PIN_NUM_LCD_BL, !LCD_BL_ON_LEVEL);
    ESP_LOGI(TAG, "Backlight GPIO%d initialised off", PIN_NUM_LCD_BL);
}

void bsp_display_backlight(bool on)
{
    gpio_set_level(PIN_NUM_LCD_BL, on ? LCD_BL_ON_LEVEL : !LCD_BL_ON_LEVEL);
}

lv_display_t *bsp_display_start(void)
{
    backlight_init();

    ESP_LOGI(TAG, "Initialize QSPI bus");
    const spi_bus_config_t buscfg = AXS15231B_PANEL_BUS_QSPI_CONFIG(
        PIN_NUM_LCD_PCLK,
        PIN_NUM_LCD_DATA0,
        PIN_NUM_LCD_DATA1,
        PIN_NUM_LCD_DATA2,
        PIN_NUM_LCD_DATA3,
        LCD_MAX_TRANSFER_SZ);
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config =
        AXS15231B_PANEL_IO_QSPI_CONFIG(PIN_NUM_LCD_CS, NULL, NULL);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &s_io_handle));

    ESP_LOGI(TAG, "Install AXS15231B panel driver (board-specific init)");
    const axs15231b_vendor_config_t vendor_config = {
        .init_cmds = axs15231b_jc3248w535_init,
        .init_cmds_size = sizeof(axs15231b_jc3248w535_init) /
                          sizeof(axs15231b_jc3248w535_init[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
        .vendor_config = (void *)&vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(
        s_io_handle, &panel_config, &s_panel_handle));

    // After a deep-sleep wake the panel is still in Sleep In (bsp_display_
    // shutdown() sent 0x10) -- RST is -1 so nothing hardware-resets it the way
    // an EN-button reset does. Take it out of sleep first (0x11, same QSPI
    // frame as shutdown's SLPIN) so the software reset + init below start from
    // a normal state. Harmless on a cold boot.
    const uint32_t qspi_slpout = (0x02u << 24) | ((0x11u & 0xFF) << 8);
    esp_lcd_panel_io_tx_param(s_io_handle, qspi_slpout, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));
    // NOTE: matches the known-working reference for this board. The init table
    // already does sleep-out (0x11); this panel/component wants `false` here.
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel_handle, false, false));

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_priority = 2;
    lvgl_cfg.timer_period_ms = 40;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io_handle,
        .panel_handle = s_panel_handle,
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES,
        .double_buffer = true,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .swap_bytes = true,
            .full_refresh = true,
            .sw_rotate = true,    // hardware swap_xy black-screens this panel
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);

    // Landscape. Must be done under the LVGL lock — the port task is already
    // running and an unlocked LVGL call here trips the task watchdog.
    lvgl_port_lock(0);
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Display ready (%dx%d landscape)",
             BSP_LCD_V_RES, BSP_LCD_H_RES);
    return disp;
}

void bsp_display_lock(void)
{
    lvgl_port_lock(0);
}

void bsp_display_unlock(void)
{
    lvgl_port_unlock();
}

void bsp_display_shutdown(void)
{
    // Panel off before backlight so there's no half-lit flash on the way
    // down. esp_lcd_panel_disp_on_off() is the same API already used (with
    // `false`) during bring-up in bsp_display_start().
    esp_lcd_panel_disp_on_off(s_panel_handle, false);

    // Display-off (0x28, above) only blanks the output -- the controller's
    // own GRAM/oscillators stay powered, which is why the panel alone was
    // still drawing up to ~30mA with the backlight already off. Actually
    // dropping that needs the deeper "Sleep In" state (DCS 0x10), which the
    // esp_lcd_axs15231b component doesn't wire up as a panel_disp_sleep()
    // vfunc (esp_lcd_panel_disp_sleep() would just return
    // ESP_ERR_NOT_SUPPORTED here) -- sent directly instead, over the same
    // panel IO handle the component's own init table uses for every other
    // command. Confirmed this panel follows standard DCS command/timing
    // semantics from that same init table: axs15231b_jc3248w535_init.h
    // sends {0x11 (SLPOUT), ..., 120ms delay} during bring-up, so 0x10
    // (SLPIN) is its documented counterpart and the same 120ms settle time
    // applies symmetrically.
    //
    // A bare 0x10 here (what this looked like on the first attempt) had no
    // effect at all -- checked esp_lcd_axs15231b.c directly afterward and
    // found why: in QSPI mode, the component's own (private, not exposed)
    // tx_param() helper wraps every command as a 32-bit frame,
    // `(0x02 << 24) | ((cmd & 0xFF) << 8)` (0x02 = its LCD_OPCODE_WRITE_CMD),
    // before ever calling esp_lcd_panel_io_tx_param() -- that's what
    // `lcd_cmd_bits = 32` in AXS15231B_PANEL_IO_QSPI_CONFIG is actually for.
    // A bare 0x10 doesn't match that framing at all, so the panel had no
    // reason to treat it as SLPIN (or anything meaningful) and the previous
    // attempt changed nothing. Replicating that exact frame here since the
    // helper itself isn't public.
    const uint32_t kQspiWriteCmd = 0x02;
    const uint32_t qspi_slpin = (kQspiWriteCmd << 24) | ((0x10u & 0xFF) << 8);
    esp_lcd_panel_io_tx_param(s_io_handle, qspi_slpin, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    gpio_set_level(PIN_NUM_LCD_BL, !LCD_BL_ON_LEVEL);

    // Setting the level alone isn't enough for deep sleep specifically:
    // per ESP-IDF's own gpio_hold_en() docs, a GPIO's driven level is NOT
    // guaranteed to persist once the digital domain powers down for deep
    // sleep -- without an explicit hold, this pin could float (or revert to
    // some default state) for the whole sleep period regardless of what we
    // just set it to, silently leaving the backlight circuit undefined
    // (possibly still lit) for exactly the period this is meant to save
    // power during. gpio_hold_en() latches this specific pin's current
    // level; gpio_deep_sleep_hold_en() is the separate global switch that
    // makes any held pin's hold actually take effect across deep sleep.
    gpio_hold_en(PIN_NUM_LCD_BL);
    gpio_deep_sleep_hold_en();

    ESP_LOGI(TAG, "Display and backlight off (held through deep sleep)");
}
