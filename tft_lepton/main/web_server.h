#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the HTTP control/streaming server (esp_http_server). Safe to call
// regardless of WiFi state -- it just won't be reachable until an IP is
// assigned (see wifi_setup.h). Serves:
//   GET  /          control page: live feed + Freeze/Palette/Transmit/Calibrate/Save/Shutdown
//   GET  /stream    live video feed (multipart/x-mixed-replace of BMP frames,
//                   same mechanism as MJPEG streaming, just BMP parts instead
//                   of JPEG -- avoids needing a JPEG encoder on-device); sends
//                   nothing while transmission is off, see /transmit
//   GET  /save      current frame as a BMP download (Content-Disposition:
//                   attachment) -- the "save to PC" button
//   GET  /status    {"frozen":bool,"palette":string,"streaming":bool}
//   POST /freeze    toggles freeze, responds with the same JSON as /status
//   POST /palette   advances to the next LEPTON_PALETTE_* value (wraps),
//                   responds with the same JSON as /status
//   POST /transmit  toggles the web feed; while on, the device's own LCD
//                   stops updating (app_state.h) -- responds with /status JSON
//   POST /calibrate requests a Flat-Field Correction (fixes image
//                   retention/burn-in); result shows on the device's own
//                   screen, not here -- responds with /status JSON regardless
//   POST /shutdown  stops the Lepton, blanks the panel/backlight, deep
//                   sleeps the device (app_state.h) -- {"ok":true}; only a
//                   touch (or reset/power-cycle) brings it back
esp_err_t web_server_start(void);
esp_err_t web_server_start(void);

#ifdef __cplusplus
}
#endif
