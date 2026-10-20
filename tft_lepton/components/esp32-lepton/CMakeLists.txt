# Vendored copy of kampi/esp32-lepton v0.0.3, patched to build on modern
# ESP-IDF (v5/v6).
#
# Two real bugs in the upstream package, not a wiring/config issue:
#
# 1. Upstream's CMakeLists.txt uses the pre-v4 register_component()/
#    COMPONENT_SRCS/COMPONENT_PRIV_REQUIRES style. On ESP-IDF v6 that doesn't
#    actually propagate COMPONENT_PRIV_REQUIRES to the component's own
#    sources: compiling src/VoSPI/vospi.cpp failed on
#    "driver/gpio.h: No such file or directory" even though `driver` was
#    listed. Rewritten below with idf_component_register().
# 2. `driver` and `freertos` were declared as *private* requirements, but
#    the component's own PUBLIC headers (include/Definitions/lepton_defs.h,
#    include/Definitions/lepton_config.h) #include their headers directly.
#    A private requirement's include paths aren't propagated to consumers
#    (like our main component) -- they need to be public REQUIRES here.
#
# Both `driver` (the umbrella compat component) and the granular
# esp_driver_gpio/esp_driver_i2c/esp_driver_spi are listed: `driver` alone
# has worked fine for our other components in this project, but listing the
# granular ones directly is more precise/robust and costs nothing extra.
#
# Everything else (sources, include layout, version defines) is unchanged
# from upstream. Upstream: https://github.com/Kampi/ESP32-Lepton (GPL-3.0).
# esp-dsp is fetched as a managed dependency declared in main/idf_component.yml
# (upstream's own manifest doesn't declare it either, a separate packaging gap).

idf_component_register(
    SRCS
        "src/lepton.cpp"
        "src/lepton_cci.cpp"
        "src/lepton_capture.cpp"
        "src/VoSPI/vospi.cpp"
        "src/CCI/cci.cpp"
    INCLUDE_DIRS
        "include"
        "include/Definitions"
        "include/CCI"
        "include/VoSPI"
    PRIV_INCLUDE_DIRS
        "include/CCI"
        "include/VoSPI"
    REQUIRES
        driver
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_spi
        freertos
        log
    PRIV_REQUIRES
        esp_timer
        esp_system
        heap
        espressif__esp-dsp
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    ESP32_LEPTON_LIB_MAJOR=0
    ESP32_LEPTON_LIB_MINOR=0
    ESP32_LEPTON_LIB_BUILD=3
)
