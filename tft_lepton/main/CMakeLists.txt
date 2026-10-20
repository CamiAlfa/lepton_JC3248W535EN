# lvgl / esp_lvgl_port / esp_lcd_axs15231b / esp_lcd_touch come from
# main/idf_component.yml and are auto-added as requirements by the component
# manager -- only built-in IDF components (driver, esp_lcd) and the vendored
# local component need to be listed explicitly here.
idf_component_register(
    SRCS
        "main.cpp"
        "bsp_display.c"
        "bsp_touch.c"
        "sd_card.cpp"
        "wifi_setup.cpp"
        "web_server.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        driver
        esp_lcd
        esp32-lepton
        fatfs
        sdmmc
        esp_driver_sdmmc
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_http_server
)
