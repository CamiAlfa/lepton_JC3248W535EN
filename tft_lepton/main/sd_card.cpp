// Native 4-bit SDMMC, replacing the earlier bit-banged-SPI driver (sd_bitbang.cpp/h, removed
// outright -- see the README's "SD card" section for the full history,
// kept there as context even though the code itself is gone) now that
// the board has real SDMMC wiring: D0-D3, CMD, CLK instead of a
// single-bit SPI link. That bit-banged driver existed in the first place
// only because this board originally had no free hardware SPI host for
// the SD card (SPI2 on the display, SPI3 on the Lepton's VoSPI) and no
// SDMMC wiring either -- with the new wiring, the SD card gets its own
// dedicated peripheral (SDMMC host) instead of sharing anything.
//
// ESP32-S3's SDMMC peripheral routes CLK/CMD/D0-D3 through the GPIO matrix
// -- unlike the classic ESP32, which has two fixed-pin slots (HS1_*/HS2_*
// in the IOMUX). Confirmed against driver/sdmmc_default_configs.h directly
// rather than assumed: its own ESP32-S3 SDMMC_SLOT_CONFIG_DEFAULT() uses
// non-contiguous pins (14, 15, 2, 4, 12, 13), which only makes sense if
// arbitrary GPIOs are supported -- so this board's custom pinout needs
// nothing beyond setting the slot_config fields below.

#include "sd_card.h"

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd_card";

// Board's own wiring (see README's SD card section). D1/D2 are swapped
// relative to the originally-reported wiring (D1=42, D2=41, not 41/42) --
// a physical wiring mistake on those two lines, found and fixed on the
// board itself, was the actual root cause of the 4-bit mount failures
// below, not a frequency/margin issue.
#define PIN_SD_CLK  GPIO_NUM_12
#define PIN_SD_CMD  GPIO_NUM_11
#define PIN_SD_D0   GPIO_NUM_13
#define PIN_SD_D1   GPIO_NUM_42
#define PIN_SD_D2   GPIO_NUM_41
#define PIN_SD_D3   GPIO_NUM_10

esp_err_t sd_card_mount(const char *base_path)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // host.max_freq_khz is left at SDMMC_HOST_DEFAULT()'s own value
    // (SDMMC_FREQ_DEFAULT, 20MHz) on purpose -- an earlier version of
    // this file capped it down to SDMMC_FREQ_PROBING (400kHz), on the
    // theory that ESP32-S3's GPIO-matrix-routed SDMMC pins (see this
    // file's header comment) added enough skew at 20MHz to cause the
    // mount failures being debugged at the time. That theory turned out
    // to be wrong (the mount still failed identically even at the
    // 400kHz floor) -- the real cause was D1/D2 being physically swapped
    // on the board (see slot_config.width below), now fixed in hardware.
    // With that fixed, there's no longer a reason to run 50x below the
    // default: the only cost of the old cap was turning a sub-second BMP
    // save into a ~3s one for no benefit.

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = PIN_SD_CLK;
    slot_config.cmd = PIN_SD_CMD;
    slot_config.d0 = PIN_SD_D0;
    slot_config.d1 = PIN_SD_D1;
    slot_config.d2 = PIN_SD_D2;
    slot_config.d3 = PIN_SD_D3;
    // 4-bit mount was failing with "corrupted data after increasing clock
    // frequency" even with host.max_freq_khz capped at SDMMC_FREQ_PROBING
    // (above) -- checked sdmmc_init_host_frequency() directly and
    // confirmed that cap already made a clock raise impossible, so the
    // error message's framing was misleading: sdmmc_check_scr() blames a
    // frequency change as the *usual* cause of a register-read mismatch,
    // but here even low-speed reads were inconsistent, pointing at a
    // wiring fault on one of the data lines instead of clock margin.
    // Dropping to 1-bit (removing D1/D2/D3 from the transaction) mounted
    // reliably, isolating the fault to D1/D2/D3 -- and it turned out to
    // be exactly that: D1 and D2 were physically swapped on the board
    // (see the PIN_SD_D1/D2 comment above). Back to 4-bit now that it's
    // fixed on the hardware side.
    slot_config.width = 4;
    // No SDMMC_SLOT_FLAG_INTERNAL_PULLUP: a microSD breakout board already
    // has its own pull-ups on CMD/D0-D3 (standard for virtually every such
    // module), and enabling the ESP32's internal ones too would just be
    // redundant leakage current with no benefit -- the same lesson the
    // bit-banged driver's own MISO pull-up taught during the deep-sleep
    // power work (see README), just avoided here by not opting in at all.

    // Zero-init + assign individually, not a partial designated-init list
    // -- same reasoning as elsewhere in this project:
    // esp_vfs_fat_mount_config_t has more fields than listed here
    // (disk_status_check_enable, use_one_fat, ...) depending on IDF
    // version, and this project's build treats
    // -Wmissing-field-initializers as an error for partial designated
    // inits.
    esp_vfs_fat_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 2;
    mount_cfg.allocation_unit_size = 16 * 1024;

    sdmmc_card_t *card = nullptr;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(base_path, &host, &slot_config, &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_fat_sdmmc_mount failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SD card mounted at %s (4-bit SDMMC)", base_path);
    return ESP_OK;
}
