#pragma once

// Cross-task accessors into state main.cpp owns (s_frozen, s_palette_index,
// s_rgb_buf) -- used by web_server.cpp so the browser control page and the
// physical touchscreen UI drive the same state without touching each
// other's internals directly, and so the web server's task can safely read
// the frame buffer while the render loop (a different task) may be
// concurrently updating it.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void app_toggle_freeze(void);

// Advances to the next LEPTON_PALETTE_* value, wrapping back to 0 after the
// last one (see kPaletteNames in main.cpp for the full cycle order).
void app_cycle_palette(void);

// Toggles whether frames are pushed to the web feed. While enabled, the LCD
// stops updating (render_thermal_frame() still runs Raw14ToRGB so the web
// feed stays live, it just skips the canvas blit) -- trades the physical
// display for the browser view rather than trying to drive both at once,
// since driving the LCD (canvas blit + panel flush) turned out to be one
// more source of CPU0/bus load competing with the Lepton's capture task
// alongside the stream itself.
void app_toggle_streaming(void);
bool app_is_streaming(void);

// *palette_name is set to a pointer to a static string (e.g. "Iron") --
// valid for the life of the program, never freed, safe to use directly in
// a response without copying.
void app_get_state(bool *frozen, const char **palette_name, bool *streaming);

// Requests a Flat-Field Correction (FFC) -- the sensor recalibrates against
// its internal shutter, the standard fix for image retention/burn-in.
// Actioned from app_main's own task (see s_calibrate_requested's comment in
// main.cpp for why), so this just sets a flag and returns immediately; the
// on-screen status label reports "Calibrating..." then success/failure.
void app_request_calibration(void);

// Requests a full shutdown: stops the Lepton, blanks the panel/backlight,
// then deep-sleeps the ESP32 -- see perform_shutdown() in main.cpp. A touch
// (INT on GPIO3) wakes it again, as do the board's EN/reset button or a
// power cycle; this just sets a flag and returns immediately (the
// browser gets a normal HTTP response before the device actually goes
// dark), the same fire-and-forget shape as app_request_calibration().
void app_request_shutdown(void);

// Builds a BMP of the most recently rendered frame into a malloc'd buffer
// (caller must free()). Returns NULL if no frame has been rendered yet or
// on allocation failure. Fine for one-off use (e.g. the /save download);
// for anything repeated (streaming), preallocate once with
// app_frame_bmp_size() and reuse it via app_build_frame_bmp_into() instead
// -- avoids per-frame malloc/free churn competing with the Lepton capture
// task for the PSRAM bus (see web_server.cpp's stream handler).
uint8_t *app_build_frame_bmp(size_t *out_len);

// The BMP size for the current frame dimensions -- fixed for the life of
// the program (always LEPTON_IMAGE_WIDTH x LEPTON_IMAGE_HEIGHT), so this is
// safe to call once to size a reusable buffer.
size_t app_frame_bmp_size(void);

// Same encoding as app_build_frame_bmp(), but into a caller-supplied buffer
// (at least app_frame_bmp_size() bytes) instead of allocating one. Returns
// false (buf untouched) if buf_cap is too small or no frame has been
// rendered yet.
bool app_build_frame_bmp_into(uint8_t *buf, size_t buf_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
