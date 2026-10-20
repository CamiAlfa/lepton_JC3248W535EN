#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reads json_path (e.g. "/sdcard/wifi.json",
// {"ssid": "...", "password": "..."}) and, if found and valid, starts
// connecting to that network in the background (WiFi station mode). Safe to
// call even if the file doesn't exist, is malformed, or the SD card isn't
// mounted -- does nothing in that case, silently. Call once, after the SD
// card is mounted.
void wifi_setup_start(const char *json_path);

// Fills ip_str (at least 16 bytes, "a.b.c.d") and returns true once
// connected and an IP has been assigned; returns false otherwise (ip_str
// left untouched) -- not yet connected, connection failed/dropped, or
// wifi_setup_start() was never called / found no usable wifi.json.
bool wifi_setup_get_ip(char *ip_str, size_t ip_str_len);

// Stops the radio (esp_wifi_stop()) for a clean shutdown before deep sleep
// -- safe to call even if wifi_setup_start() was never called or never got
// as far as esp_wifi_start() (no wifi.json found, SD not mounted, etc.);
// does nothing in that case rather than erroring.
void wifi_setup_stop(void);

#ifdef __cplusplus
}
#endif
