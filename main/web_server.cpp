#include "web_server.h"
#include "servo_controller.h"
#include "audio_player.h"
#include "keypad.h"
#include "command_processor.h"
#include "console_serial.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "WEB_SERVER";
static httpd_handle_t server = NULL;

// Lightweight minimal diagnostic page (WebLauncher app runs independently)
static const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32-S3 Claw Controller API</title>
    <style>
        body { background: #020617; color: #f8fafc; font-family: -apple-system, sans-serif; display: flex; justify-content: center; align-items: center; min-height: 100vh; margin: 0; padding: 16px; }
        .card { background: #0f172a; border: 1px solid #334155; border-radius: 16px; padding: 24px; max-width: 400px; width: 100%; text-align: center; }
        h1 { font-size: 1.2rem; color: #38bdf8; margin-bottom: 8px; }
        p { font-size: 0.85rem; color: #94a3b8; line-height: 1.4; margin-bottom: 16px; }
        .badge { display: inline-block; padding: 4px 10px; border-radius: 9999px; background: #10b98122; color: #34d399; font-size: 0.75rem; font-weight: 600; }
    </style>
</head>
<body>
    <div class="card">
        <h1>ESP32-S3 Claw Firmware Ready</h1>
        <p>All REST API and BLE NUS services are online. Control the device using your Apple WebLauncher app.</p>
        <span class="badge">API &amp; BLE Active</span>
    </div>
</body>
</html>
)rawliteral";

static void url_decode(char *dst, const char *src, size_t dst_len) {
    size_t d = 0;
    while (*src && d + 1 < dst_len) {
        if (*src == '%' && isxdigit((unsigned char)*(src + 1)) && isxdigit((unsigned char)*(src + 2))) {
            char hex[3] = { src[1], src[2], '\0' };
            dst[d++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[d++] = ' ';
            src++;
        } else {
            dst[d++] = *src++;
        }
    }
    dst[d] = '\0';
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t captive_redirect_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t set_angle_handler(httpd_req_t *req) {
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[16] = {0};
        if (httpd_query_key_value(query, "angle", param, sizeof(param)) == ESP_OK) {
            int requested_angle = atoi(param);
            servo_set_angle(requested_angle);
            command_notify_status();

            char resp[32];
            snprintf(resp, sizeof(resp), "OK %d", servo_get_angle());
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'angle' param");
    return ESP_FAIL;
}

static esp_err_t get_status_handler(httpd_req_t *req) {
    int bclk = 10, lrc = 11, din = 12;
    audio_get_pins(&bclk, &lrc, &din);
    char json_resp[160];
    snprintf(json_resp, sizeof(json_resp),
             "{\"angle\":%d,\"volume\":%d,\"bclk\":%d,\"lrc\":%d,\"din\":%d,\"autoretract\":%d}",
             servo_get_angle(), audio_get_volume(), bclk, lrc, din,
             command_get_auto_retract() ? 1 : 0);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t autoretract_handler(httpd_req_t *req) {
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16] = {0};
        if (httpd_query_key_value(query, "val", val, sizeof(val)) == ESP_OK ||
            httpd_query_key_value(query, "enable", val, sizeof(val)) == ESP_OK) {
            bool en = (atoi(val) != 0 || strcmp(val, "on") == 0 || strcmp(val, "true") == 0);
            command_set_auto_retract(en);
            char resp[32];
            snprintf(resp, sizeof(resp), "OK %d", en ? 1 : 0);
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    }
    char resp[32];
    snprintf(resp, sizeof(resp), "{\"autoretract\":%d}", command_get_auto_retract() ? 1 : 0);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t play_sound_handler(httpd_req_t *req) {
    char query[64] = {0};
    char sound[32] = "dog_bark";

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[32] = {0};
        if (httpd_query_key_value(query, "sound", param, sizeof(param)) == ESP_OK) {
            strncpy(sound, param, sizeof(sound) - 1);
        }
    }

    audio_play(sound);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "PLAYING", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t play_beep_handler(httpd_req_t *req) {
    audio_play_test_beep();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "BEEPING", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t stop_sound_handler(httpd_req_t *req) {
    audio_stop();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "STOPPED", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t set_volume_handler(httpd_req_t *req) {
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[16] = {0};
        if (httpd_query_key_value(query, "val", param, sizeof(param)) == ESP_OK ||
            httpd_query_key_value(query, "volume", param, sizeof(param)) == ESP_OK) {
            int vol = atoi(param);
            audio_set_volume(vol);
            command_notify_status();

            char resp[32];
            snprintf(resp, sizeof(resp), "OK %d", audio_get_volume());
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'val' param");
    return ESP_FAIL;
}

static esp_err_t set_pins_handler(httpd_req_t *req) {
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char p_bclk[16] = {0}, p_lrc[16] = {0}, p_din[16] = {0};
        httpd_query_key_value(query, "bclk", p_bclk, sizeof(p_bclk));
        httpd_query_key_value(query, "lrc",  p_lrc,  sizeof(p_lrc));
        httpd_query_key_value(query, "din",  p_din,  sizeof(p_din));
        if (p_bclk[0] && p_lrc[0] && p_din[0]) {
            audio_set_pins(atoi(p_bclk), atoi(p_lrc), atoi(p_din));
            command_notify_status();
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "PINS_UPDATED", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing pin params");
    return ESP_FAIL;
}

static esp_err_t keypad_get_handler(httpd_req_t *req) {
    char json[1024];
    int offset = snprintf(json, sizeof(json), "{");
    for (int i = 1; i <= KEYPAD_NUM_KEYS; i++) {
        const char *cmd = keypad_get_key_command(i);
        offset += snprintf(json + offset, sizeof(json) - offset,
                           "\"%d\":\"%s\"%s", i, cmd, (i < KEYPAD_NUM_KEYS) ? "," : "");
    }
    snprintf(json + offset, sizeof(json) - offset, "}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t keypad_set_handler(httpd_req_t *req) {
    char query[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char key_str[16] = {0};
        char cmd_raw[KEYPAD_CMD_MAX_LEN * 3] = {0};
        char cmd_decoded[KEYPAD_CMD_MAX_LEN] = {0};

        httpd_query_key_value(query, "key", key_str, sizeof(key_str));
        httpd_query_key_value(query, "cmd", cmd_raw, sizeof(cmd_raw));

        if (key_str[0] != '\0') {
            url_decode(cmd_decoded, cmd_raw, sizeof(cmd_decoded));
            int key_idx = atoi(key_str);
            if (keypad_set_key_command(key_idx, cmd_decoded)) {
                httpd_resp_set_type(req, "text/plain");
                httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid key or command");
    return ESP_FAIL;
}

static esp_err_t keypad_reset_handler(httpd_req_t *req) {
    keypad_reset_defaults();
    return keypad_get_handler(req);
}

void web_server_init(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 26;
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri = {};

        uri.uri = "/";
        uri.method = HTTP_GET;
        uri.handler = root_get_handler;
        uri.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/set";
        uri.method = HTTP_GET;
        uri.handler = set_angle_handler;
        uri.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/get";
        uri.method = HTTP_GET;
        uri.handler = get_status_handler;
        uri.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/autoretract";
        uri.method = HTTP_GET;
        uri.handler = autoretract_handler;
        uri.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/play";
        uri.method = HTTP_GET;
        uri.handler = play_sound_handler;
        uri.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/beep";
        uri.method = HTTP_GET;
        uri.handler = play_beep_handler;
        uri.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/stop";
        uri.method = HTTP_GET;
        uri.handler = stop_sound_handler;
        uri.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/volume";
        uri.method = HTTP_GET;
        uri.handler = set_volume_handler;
        uri.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/set_pins";
        uri.method = HTTP_GET;
        uri.handler = set_pins_handler;
        uri.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/keypad/get";
        uri.method = HTTP_GET;
        uri.handler = keypad_get_handler;
        uri.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/keypad/set";
        uri.method = HTTP_GET;
        uri.handler = keypad_set_handler;
        uri.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/keypad/reset";
        uri.method = HTTP_GET;
        uri.handler = keypad_reset_handler;
        uri.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri);

        uri.method = HTTP_GET;
        uri.handler = captive_redirect_handler;
        uri.user_ctx = NULL;

        uri.uri = "/hotspot-detect.html";
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/canonical.html";
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/library/test/success.html";
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/generate_204";
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/gen_204";
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/ncsi.txt";
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/connecttest.txt";
        httpd_register_uri_handler(server, &uri);

        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);

        ESP_LOGI(TAG, "HTTP Server active on port 80 (API endpoints ready)");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server!");
    }
}