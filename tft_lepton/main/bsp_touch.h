// Capacitive touch for the JC3248W535C. The AXS15231B is a combined
// display + touch chip; touch is a separate I2C interface (GPIO4/GPIO8).
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the I2C bus + AXS15231B touch driver and register it with
// esp_lvgl_port as a pointer input device for `disp`. Aborts on error.
lv_indev_t *bsp_touch_start(lv_display_t *disp);

#ifdef __cplusplus
}
#endif
