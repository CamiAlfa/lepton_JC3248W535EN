#include "bsp_touch.h"

#include "bsp_display.h"      // BSP_LCD_H_RES / BSP_LCD_V_RES (physical, portrait)
#include "driver/i2c_master.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "bsp_touch";

#define TOUCH_I2C_PORT   I2C_NUM_0
#define PIN_TOUCH_SDA    4
#define PIN_TOUCH_SCL    8
#define TOUCH_I2C_HZ     400000

lv_indev_t *bsp_touch_start(lv_display_t *disp)
{
    ESP_LOGI(TAG, "Init I2C%d (SDA=%d, SCL=%d)",
             TOUCH_I2C_PORT, PIN_TOUCH_SDA, PIN_TOUCH_SCL);

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = PIN_TOUCH_SDA,
        .scl_io_num = PIN_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    esp_lcd_panel_io_i2c_config_t tp_io_cfg =
        ESP_LCD_TOUCH_IO_I2C_AXS15231B_CONFIG();
    tp_io_cfg.scl_speed_hz = TOUCH_I2C_HZ;

    esp_lcd_panel_io_handle_t tp_io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus, &tp_io_cfg, &tp_io));

    // Coordinates come back in the panel's native portrait frame. esp_lvgl_port
    // applies the LVGL display rotation to them, so keep the physical maxima
    // here. If a touch lands rotated/mirrored, flip these flags instead.
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    esp_lcd_touch_handle_t tp = NULL;
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_axs15231b(tp_io, &tp_cfg, &tp));

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = tp,
    };
    lv_indev_t *indev = lvgl_port_add_touch(&touch_cfg);
    ESP_LOGI(TAG, "Touch ready");
    return indev;
}
