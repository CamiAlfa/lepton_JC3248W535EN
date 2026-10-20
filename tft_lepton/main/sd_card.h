#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Mounts the SD card over native 4-bit SDMMC (not the bit-banged SPI driver
// this project used before the board was rewired for true SDMMC lines --
// see sd_card.cpp's header comment and the README's "SD card" section for
// the history, kept there since this file replaced it outright). Pins are
// fixed in sd_card.cpp to match this board's wiring.
esp_err_t sd_card_mount(const char *base_path);

#ifdef __cplusplus
}
#endif
