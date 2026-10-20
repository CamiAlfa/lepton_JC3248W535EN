// HTTP control/streaming server for the browser-side "remote control":
// live video feed, Freeze/Palette/Calibrate toggles mirroring the
// touchscreen buttons, and a Save-to-PC download.
//
// Two separate httpd_start() instances, not one: esp_http_server processes
// one request at a time on a single select()-loop task per instance
// (confirmed against esp_http_server/src/httpd_main.c -- a single
// FreeRTOS task, no per-connection worker threads). /stream's handler is
// intentionally a "never returns while the client is connected" loop, so on
// a single shared server it would block every other request -- /status,
// /freeze, /palette, /save, even reloading "/" -- for as long as a browser
// tab keeps the feed open. Splitting the stream onto its own server+port (:81)
// is the same fix the official ESP32 CameraWebServer example uses, and
// keeps the control page responsive while a feed is open. The page's <img>
// src is pointed at that port via a one-line script, since a relative URL
// can't cross ports.

#include "web_server.h"

#include <stdlib.h>
#include <string.h>

#include "app_state.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web_server";

#define STREAM_PORT       81
#define STREAM_CTRL_PORT  32769   // must differ from the main server's (default 32768)

static const char s_index_html[] = R"HTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>tft_lepton</title>
<style>
body{background:#111;color:#eee;font-family:sans-serif;text-align:center;margin:0;padding:16px}
img{max-width:100%;height:auto;border:1px solid #444;image-rendering:pixelated;background:#000}
.controls{margin-top:12px}
button,a.btn{display:inline-block;margin:4px;padding:10px 20px;font-size:16px;
  background:#2a2a2a;color:#eee;border:1px solid #555;border-radius:6px;
  text-decoration:none;cursor:pointer}
button:active,a.btn:active{background:#444}
#shutdownBtn{background:#4a1f1f;border-color:#7a2c2c}
#shutdownBtn:active{background:#5c2626}
</style>
</head>
<body>
<h2>Lepton Thermal Feed</h2>
<img id="feed" alt="live feed">
<div class="controls">
<button id="freezeBtn" onclick="toggle('/freeze')">Freeze</button>
<button id="paletteBtn" onclick="toggle('/palette')">Palette</button>
<button id="transmitBtn" onclick="toggle('/transmit')">Start Transmission</button>
<button onclick="toggle('/calibrate')">Calibrate</button>
<a class="btn" href="/save" download="lepton.bmp">Save to PC</a>
<button id="shutdownBtn" onclick="shutdown()">Shutdown</button>
</div>
<p id="hint" style="opacity:.7;font-size:13px">Starting transmission stops the image on the device's own screen.
Calibrate briefly pauses the image while the sensor recalibrates (fixes a
"stuck"/persistent image) -- result shows on the device's own screen.
Shutdown deep-sleeps the device to save battery -- touching the screen
wakes it up again (as do the reset button or a power cycle).</p>
<script>
document.getElementById('feed').src = 'http://' + location.hostname + ':81/stream';
function applyState(s) {
  document.getElementById('freezeBtn').textContent = s.frozen ? 'Resume' : 'Freeze';
  document.getElementById('paletteBtn').textContent = s.palette;
  document.getElementById('transmitBtn').textContent = s.streaming ? 'Stop Transmission' : 'Start Transmission';
}
function toggle(url) {
  fetch(url, {method:'POST'}).then(function(r){ return r.json(); }).then(applyState);
}
function shutdown() {
  if (!confirm('Shut down and deep-sleep the device?\n\nTouch the screen to wake it up -- there is no remote way to turn it back on.')) {
    return;
  }
  fetch('/shutdown', {method:'POST'});
  document.getElementById('shutdownBtn').textContent = 'Shutting down...';
  document.getElementById('shutdownBtn').disabled = true;
}
function refreshStatus() {
  fetch('/status').then(function(r){ return r.json(); }).then(applyState);
}
refreshStatus();
setInterval(refreshStatus, 2000);
</script>
</body>
</html>
)HTML";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, s_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    bool frozen = false;
    const char *palette_name = "";
    bool streaming = false;
    app_get_state(&frozen, &palette_name, &streaming);

    char json[96];
    const int len = snprintf(json, sizeof(json),
                              "{\"frozen\":%s,\"palette\":\"%s\",\"streaming\":%s}",
                              frozen ? "true" : "false", palette_name,
                              streaming ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, len);
}

static esp_err_t freeze_post_handler(httpd_req_t *req)
{
    app_toggle_freeze();
    return status_get_handler(req);   // responds with the new state
}

static esp_err_t palette_post_handler(httpd_req_t *req)
{
    app_cycle_palette();
    return status_get_handler(req);
}

static esp_err_t transmit_post_handler(httpd_req_t *req)
{
    app_toggle_streaming();
    return status_get_handler(req);
}

static esp_err_t calibrate_post_handler(httpd_req_t *req)
{
    // Fire-and-forget: the actual FFC runs on app_main's task (see
    // app_request_calibration()'s comment) and its result shows on the
    // device's own screen, not here -- the status JSON returned is the same
    // shape as every other action endpoint for consistency, even though
    // calibration doesn't change any of frozen/palette/streaming.
    app_request_calibration();
    return status_get_handler(req);
}

static esp_err_t shutdown_post_handler(httpd_req_t *req)
{
    // Fire-and-forget, same as /calibrate -- the actual shutdown (stop the
    // Lepton, blank the panel/backlight, deep sleep) runs on app_main's
    // task after this handler returns, so the browser still gets a normal
    // response before the device goes dark. No /status-shaped JSON here
    // like the other action endpoints, since there's nothing meaningful
    // left to report -- the device is about to stop responding entirely.
    app_request_shutdown();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_get_handler(httpd_req_t *req)
{
    size_t bmp_len = 0;
    uint8_t *bmp = app_build_frame_bmp(&bmp_len);
    if (!bmp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No frame yet");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"lepton.bmp\"");
    const esp_err_t err = httpd_resp_send(req, reinterpret_cast<const char *>(bmp), bmp_len);
    free(bmp);
    return err;
}

static esp_err_t stream_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    // One reusable buffer for the whole connection instead of malloc/free
    // every frame -- avoids repeated PSRAM allocator churn competing with
    // the Lepton capture task for the bus while transmission is on.
    const size_t buf_cap = app_frame_bmp_size();
    uint8_t *bmp = static_cast<uint8_t *>(malloc(buf_cap));
    if (!bmp) {
        ESP_LOGE(TAG, "stream: buffer allocation failed");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    while (err == ESP_OK) {
        // Full resolution, but only actually encoded/sent while the web
        // page's Start Transmission button has enabled it (app_state.h) --
        // the connection itself stays open (so a later "Start" takes effect
        // immediately) but idles here, sending nothing, while disabled.
        if (!app_is_streaming()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        size_t bmp_len = 0;
        if (!app_build_frame_bmp_into(bmp, buf_cap, &bmp_len)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        char part_hdr[96];
        const int hdr_len = snprintf(part_hdr, sizeof(part_hdr),
                                      "--frame\r\nContent-Type: image/bmp\r\nContent-Length: %u\r\n\r\n",
                                      (unsigned)bmp_len);
        err = httpd_resp_send_chunk(req, part_hdr, hdr_len);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(bmp), bmp_len);
        }
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, "\r\n", 2);
        }

        if (err == ESP_OK) {
            // ~4fps -- deliberately well under the Lepton's own ~8.7fps.
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
    free(bmp);
    ESP_LOGI(TAG, "Stream client disconnected");
    return ESP_OK;   // the error above is why the loop exited, not a handler failure
}

esp_err_t web_server_start(void)
{
    // Pinned to core 0, same as WiFi's own task and app_main's -- the
    // Lepton capture task is deliberately pinned to core 1
    // (CONFIG_LEPTON_CAPTURE_TASK_CORE) and is timing-sensitive (VoSPI has
    // no flow control), so keeping every WiFi-adjacent task off that core
    // avoids adding direct scheduling contention on top of it.
    httpd_handle_t main_server = nullptr;
    httpd_config_t main_cfg = HTTPD_DEFAULT_CONFIG();
    main_cfg.core_id = 0;
    // Default is 8, and this server now registers exactly 8 handlers
    // (root/status/freeze/palette/calibrate/save/transmit/shutdown) --
    // bumped for headroom rather than sitting right at the ceiling.
    main_cfg.max_uri_handlers = 12;
    esp_err_t err = httpd_start(&main_server, &main_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start(main) failed: %s", esp_err_to_name(err));
        return err;
    }

    // Zero-init + assign individually, not a partial designated-init list --
    // same reasoning as elsewhere in this project: httpd_uri_t gains a
    // websocket field when CONFIG_HTTPD_WS_SUPPORT is on (off in this
    // build, but this stays correct if that ever changes), and
    // -Wmissing-field-initializers is an error here for partial lists.
    httpd_uri_t root_uri = {};
    root_uri.uri = "/";
    root_uri.method = HTTP_GET;
    root_uri.handler = root_get_handler;
    httpd_register_uri_handler(main_server, &root_uri);

    httpd_uri_t status_uri = {};
    status_uri.uri = "/status";
    status_uri.method = HTTP_GET;
    status_uri.handler = status_get_handler;
    httpd_register_uri_handler(main_server, &status_uri);

    httpd_uri_t freeze_uri = {};
    freeze_uri.uri = "/freeze";
    freeze_uri.method = HTTP_POST;
    freeze_uri.handler = freeze_post_handler;
    httpd_register_uri_handler(main_server, &freeze_uri);

    httpd_uri_t palette_uri = {};
    palette_uri.uri = "/palette";
    palette_uri.method = HTTP_POST;
    palette_uri.handler = palette_post_handler;
    httpd_register_uri_handler(main_server, &palette_uri);

    httpd_uri_t calibrate_uri = {};
    calibrate_uri.uri = "/calibrate";
    calibrate_uri.method = HTTP_POST;
    calibrate_uri.handler = calibrate_post_handler;
    httpd_register_uri_handler(main_server, &calibrate_uri);

    httpd_uri_t save_uri = {};
    save_uri.uri = "/save";
    save_uri.method = HTTP_GET;
    save_uri.handler = save_get_handler;
    httpd_register_uri_handler(main_server, &save_uri);

    httpd_uri_t transmit_uri = {};
    transmit_uri.uri = "/transmit";
    transmit_uri.method = HTTP_POST;
    transmit_uri.handler = transmit_post_handler;
    httpd_register_uri_handler(main_server, &transmit_uri);

    httpd_uri_t shutdown_uri = {};
    shutdown_uri.uri = "/shutdown";
    shutdown_uri.method = HTTP_POST;
    shutdown_uri.handler = shutdown_post_handler;
    httpd_register_uri_handler(main_server, &shutdown_uri);

    httpd_handle_t stream_server = nullptr;
    httpd_config_t stream_cfg = HTTPD_DEFAULT_CONFIG();
    stream_cfg.server_port = STREAM_PORT;
    stream_cfg.ctrl_port = STREAM_CTRL_PORT;
    stream_cfg.core_id = 0;   // see main_cfg.core_id's comment above
    err = httpd_start(&stream_server, &stream_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start(stream) failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t stream_uri = {};
    stream_uri.uri = "/stream";
    stream_uri.method = HTTP_GET;
    stream_uri.handler = stream_get_handler;
    httpd_register_uri_handler(stream_server, &stream_uri);

    ESP_LOGI(TAG, "Web server started (control on :80, stream on :%d)", STREAM_PORT);
    return ESP_OK;
}
