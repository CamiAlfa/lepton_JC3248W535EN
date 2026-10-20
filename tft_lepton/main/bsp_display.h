// Minimal board-support layer for the Guition JC3248W535C:
//   ESP32-S3-WROOM-1 N16R8 + 3.5" 320x480 IPS, AXS15231B controller on QSPI.
//
// This module brings up the QSPI bus, the AXS15231B panel and esp_lvgl_port,
// then hands back an LVGL display you can draw on from app code.
#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Physical panel geometry (portrait scan). bsp_display_start() applies a 90-deg
// software rotation, so the LVGL display app code sees is 480x320 landscape.
// Keep these as the physical values.
#define BSP_LCD_H_RES 320
#define BSP_LCD_V_RES 480

// Bring up backlight, QSPI bus, AXS15231B panel and LVGL. Aborts on any
// hardware error (ESP_ERROR_CHECK). Returns the registered LVGL display.
lv_display_t *bsp_display_start(void);

// LVGL is driven from its own task inside esp_lvgl_port. Any LVGL call made
// from another task (e.g. app_main) must be wrapped in this lock/unlock pair.
void bsp_display_lock(void);
void bsp_display_unlock(void);

// Backlight only. bsp_display_start() leaves it off; app code turns it on once
// the first frame is drawn.
void bsp_display_backlight(bool on);

// Turns the panel and backlight off (for the Shutdown button, right before
// deep sleep) -- not a full teardown, just puts the hardware in its lowest-
// power state. Call under bsp_display_lock()/unlock() since it touches the
// same panel handle the LVGL port task flushes to.
void bsp_display_shutdown(void);

#ifdef __cplusplus
}
#endif
