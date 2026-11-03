#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "driver/ledc.h"
#include "audio_player.h"

static const char *TAG = "MAIN_APP";

// =========================================================================
// HARDWARE PIN CONFIGURATION
// =========================================================================
#define SERVO_PIN           GPIO_NUM_4
#define LEDC_TIMER          LEDC_TIMER_0
#define LEDC_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL        LEDC_CHANNEL_0
#define LEDC_DUTY_RES       LEDC_TIMER_14_BIT // 16384 total duty steps
#define LEDC_FREQUENCY      50                // 50 Hz standard servo PWM (20ms period)

// 500us (0 deg) to 2500us (180 deg) mapping for 14-bit at 50Hz (20000us period)
#define SERVO_MIN_US        500
#define SERVO_MAX_US        2500

static int current_servo_angle = 90;
static httpd_handle_t server = NULL;

// =========================================================================
// SERVO LEDC CONTROLLER FUNCTIONS
// =========================================================================
static uint32_t angle_to_duty(int angle) {
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;

    uint32_t pulse_us = SERVO_MIN_US + ((SERVO_MAX_US - SERVO_MIN_US) * angle) / 180;
    // Duty = (pulse_us / 20000us) * (2^14 - 1)
    uint32_t duty = (pulse_us * ((1 << 14) - 1)) / 20000;
    return duty;
}

static void servo_init(void) {
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode       = LEDC_MODE;
    ledc_timer.duty_resolution  = LEDC_DUTY_RES;
    ledc_timer.timer_num        = LEDC_TIMER;
    ledc_timer.freq_hz          = LEDC_FREQUENCY;
    ledc_timer.clk_cfg          = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel = {};
    ledc_channel.gpio_num       = SERVO_PIN;
    ledc_channel.speed_mode     = LEDC_MODE;
    ledc_channel.channel        = LEDC_CHANNEL;
    ledc_channel.intr_type      = LEDC_INTR_DISABLE;
    ledc_channel.timer_sel      = LEDC_TIMER;
    ledc_channel.duty           = angle_to_duty(current_servo_angle);
    ledc_channel.hpoint         = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    ESP_LOGI(TAG, "Servo hardware initialized on GPIO %d (default angle: %d deg)", SERVO_PIN, current_servo_angle);
}

static void servo_set_angle(int angle) {
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;

    current_servo_angle = angle;
    uint32_t duty = angle_to_duty(angle);

    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));

    ESP_LOGI(TAG, "Angle set to %d deg (Duty: %lu)", angle, (unsigned long)duty);
}

// =========================================================================
// SERIAL REPL CONSOLE TASK (INTERACTIVE REPL)
// =========================================================================
static void console_read_task(void *pvParameter) {
    // Unbuffer stdout to guarantee immediate console response
    setvbuf(stdout, NULL, _IONBF, 0);

    // Set standard input to non-blocking mode
    int fd = fileno(stdin);
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    vTaskDelay(pdMS_TO_TICKS(500));

    printf("\n==================================================\n");
    printf("   YD-ESP32-S3 Serial REPL Console Ready\n");
    printf("==================================================\n");
    printf("Servo Commands:\n");
    printf("  0 - 180          : Set servo angle directly (e.g. '90')\n");
    printf("  angle <N>        : Set servo angle (e.g. 'angle 45')\n");
    printf("Speaker Audio Commands:\n");
    printf("  bark / play      : Play dog bark sound on HT517 speaker\n");
    printf("  stop             : Stop audio playback\n");
    printf("  vol <0-100>      : Set speaker volume (e.g. 'vol 85')\n");
    printf("General Commands:\n");
    printf("  status           : Show servo angle, volume & IP\n");
    printf("  reboot / restart : Restart ESP32-S3 board\n");
    printf("  help             : Show this help guide\n");
    printf("  Ctrl+C           : Clear current input line\n");
    printf("==================================================\n\n");
    printf("REPL> ");
    fflush(stdout);

    uint8_t rx_buf[64];
    char cmd_buf[128];
    int cmd_idx = 0;

    while (1) {
        int len = read(fd, rx_buf, sizeof(rx_buf));
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                uint8_t c = rx_buf[i];

                // Handle Ctrl+C (Interrupt/Clear line)
                if (c == 0x03) {
                    printf("\n[Sent Ctrl+C - Cleared]\nREPL> ");
                    fflush(stdout);
                    cmd_idx = 0;
                    continue;
                }

                // Handle Enter / Newline (Execute Command)
                if (c == '\r' || c == '\n') {
                    if (cmd_idx > 0) {
                        cmd_buf[cmd_idx] = '\0';
                        printf("\n");

                        // Strip leading whitespace
                        char *p = cmd_buf;
                        while (*p == ' ' || *p == '\t') p++;

                        // Check if input is a pure number (0 - 180)
                        bool is_numeric = true;
                        int p_len = strlen(p);
                        if (p_len == 0) is_numeric = false;
                        for (int j = 0; j < p_len; j++) {
                            if (!isdigit((unsigned char)p[j])) {
                                is_numeric = false;
                                break;
                            }
                        }

                        if (is_numeric) {
                            int req_angle = atoi(p);
                            if (req_angle >= 0 && req_angle <= 180) {
                                servo_set_angle(req_angle);
                                printf(">> Servo moved to %d deg\n", req_angle);
                            } else {
                                printf(">> Error: Angle must be between 0 and 180 degrees.\n");
                            }
                        } else if (strncmp(p, "angle ", 6) == 0) {
                            int req_angle = atoi(p + 6);
                            if (req_angle >= 0 && req_angle <= 180) {
                                servo_set_angle(req_angle);
                                printf(">> Servo moved to %d deg\n", req_angle);
                            } else {
                                printf(">> Error: Angle must be between 0 and 180 degrees.\n");
                            }
                        } else if (strcmp(p, "bark") == 0 || strcmp(p, "play") == 0 || strcmp(p, "sound") == 0) {
                            audio_play("dog_bark");
                            printf(">> Playing dog bark on HT517 speaker (Volume: %d%%)\n", audio_get_volume());
                        } else if (strcmp(p, "stop") == 0) {
                            audio_stop();
                            printf(">> Audio playback stopped.\n");
                        } else if (strncmp(p, "vol ", 4) == 0 || strncmp(p, "volume ", 7) == 0) {
                            int req_vol = (strncmp(p, "vol ", 4) == 0) ? atoi(p + 4) : atoi(p + 7);
                            if (req_vol >= 0 && req_vol <= 100) {
                                audio_set_volume(req_vol);
                                printf(">> Speaker volume set to %d%%\n", req_vol);
                            } else {
                                printf(">> Error: Volume must be between 0 and 100.\n");
                            }
                        } else if (strcmp(p, "status") == 0) {
                            printf(">> Current Servo Angle : %d deg\n", current_servo_angle);
                            printf(">> Current Speaker Vol : %d%%\n", audio_get_volume());
                            printf(">> Web Dashboard       : http://192.168.4.1/\n");
                        } else if (strcmp(p, "help") == 0 || strcmp(p, "?") == 0) {
                            printf("Available Commands:\n");
                            printf("  0 - 180          : Direct servo angle\n");
                            printf("  angle <N>        : Set servo angle\n");
                            printf("  bark / play      : Play dog bark sound\n");
                            printf("  stop             : Stop speaker playback\n");
                            printf("  vol <0-100>      : Set volume percentage\n");
                            printf("  status           : Show device status\n");
                            printf("  reboot / restart : Restart ESP32-S3\n");
                        } else if (strcmp(p, "reboot") == 0 || strcmp(p, "restart") == 0) {
                            printf("Rebooting ESP32-S3...\n");
                            vTaskDelay(pdMS_TO_TICKS(100));
                            esp_restart();
                        } else {
                            printf(">> Unknown command: '%s'. Enter a number (0-180), 'bark', 'vol 80', or 'help'.\n", p);
                        }

                        cmd_idx = 0;
                        printf("REPL> ");
                        fflush(stdout);
                    }
                } else if (c == 0x08 || c == 0x7F) { // Backspace
                    if (cmd_idx > 0) {
                        cmd_idx--;
                        printf("\b \b");
                        fflush(stdout);
                    }
                } else if (c >= 0x20 && c <= 0x7E) {
                    // Store printable ASCII characters and echo back
                    if (cmd_idx < (int)sizeof(cmd_buf) - 1) {
                        cmd_buf[cmd_idx++] = (char)c;
                        putchar((char)c);
                        fflush(stdout);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// =========================================================================
// HTML / WEB INTERFACE
// =========================================================================
static const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>YD-ESP32-S3 Controller</title>
    <style>
        :root {
            --bg-color: #0b1120;
            --card-color: #1e293b;
            --card-border: #334155;
            --primary: #38bdf8;
            --primary-hover: #0284c7;
            --accent: #10b981;
            --accent-hover: #059669;
            --danger: #ef4444;
            --danger-hover: #dc2626;
            --text-color: #f8fafc;
            --muted-text: #94a3b8;
        }
        * {
            box-sizing: border-box;
        }
        body {
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
            background-color: var(--bg-color);
            color: var(--text-color);
            margin: 0;
            display: flex;
            align-items: center;
            justify-content: center;
            min-height: 100vh;
            padding: 20px 16px;
        }
        .container {
            background-color: var(--card-color);
            width: 100%;
            max-width: 460px;
            border-radius: 20px;
            padding: 26px 24px;
            box-shadow: 0 10px 25px -5px rgba(0, 0, 0, 0.6);
            border: 1px solid var(--card-border);
            text-align: center;
        }
        h1 {
            font-size: 1.45rem;
            margin: 0 0 4px 0;
            color: var(--primary);
        }
        .sub-header {
            font-size: 0.8rem;
            color: var(--muted-text);
            margin-bottom: 20px;
        }
        .section-box {
            background: #0f172a;
            border: 1px solid var(--card-border);
            border-radius: 14px;
            padding: 16px;
            margin-bottom: 18px;
        }
        .section-title {
            font-size: 0.8rem;
            font-weight: 700;
            letter-spacing: 0.08em;
            text-transform: uppercase;
            color: var(--muted-text);
            margin-bottom: 12px;
            display: flex;
            justify-content: space-between;
            align-items: center;
        }
        .readout {
            font-size: 3.2rem;
            font-weight: 700;
            color: var(--text-color);
            font-variant-numeric: tabular-nums;
            line-height: 1.1;
        }
        .readout-unit {
            font-size: 1.4rem;
            color: var(--primary);
        }
        .slider-wrap {
            margin: 16px 0 8px 0;
        }
        input[type=range] {
            -webkit-appearance: none;
            width: 100%;
            height: 10px;
            border-radius: 6px;
            background: #334155;
            outline: none;
        }
        input[type=range]::-webkit-slider-thumb {
            -webkit-appearance: none;
            width: 28px;
            height: 28px;
            border-radius: 50%;
            background: var(--primary);
            cursor: pointer;
            box-shadow: 0 0 10px rgba(56, 189, 248, 0.5);
            border: 2px solid #ffffff;
        }
        .range-labels {
            display: flex;
            justify-content: space-between;
            font-size: 0.75rem;
            color: var(--muted-text);
            margin-top: 6px;
        }
        .btn-grid {
            display: grid;
            grid-template-columns: repeat(5, 1fr);
            gap: 6px;
            margin-top: 14px;
        }
        .audio-btn-grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 10px;
            margin-top: 14px;
        }
        button {
            background: #334155;
            border: none;
            border-radius: 10px;
            color: var(--text-color);
            padding: 10px 4px;
            font-weight: 600;
            font-size: 0.85rem;
            cursor: pointer;
            transition: all 0.15s ease;
        }
        button:hover {
            background: var(--primary-hover);
            color: #ffffff;
        }
        button:active {
            transform: scale(0.96);
        }
        .btn-bark {
            background: var(--accent);
            padding: 14px 10px;
            font-size: 0.95rem;
        }
        .btn-bark:hover {
            background: var(--accent-hover);
        }
        .btn-stop {
            background: #475569;
            padding: 14px 10px;
            font-size: 0.95rem;
        }
        .btn-stop:hover {
            background: var(--danger-hover);
        }
        .status-dot {
            display: inline-block;
            width: 8px;
            height: 8px;
            background-color: #22c55e;
            border-radius: 50%;
            margin-right: 6px;
        }
        .status-bar {
            margin-top: 14px;
            font-size: 0.8rem;
            color: var(--muted-text);
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>YD-ESP32-S3 Dashboard</h1>
        <div class="sub-header">Servo (GPIO 4) &amp; HT517 I2S Speaker (GPIO 10,11,12)</div>

        <!-- SERVO MOTOR CONTROL SECTION -->
        <div class="section-box">
            <div class="section-title">
                <span>Servo Position</span>
                <span style="color: var(--primary);">LEDC 50Hz</span>
            </div>
            <div>
                <span class="readout" id="angleVal">90</span><span class="readout-unit">&deg;</span>
            </div>

            <div class="slider-wrap">
                <input type="range" id="angleSlider" min="0" max="180" value="90" oninput="onAngleSliderMove(this.value)" onchange="sendAngle(this.value)">
                <div class="range-labels">
                    <span>0&deg;</span>
                    <span>45&deg;</span>
                    <span>90&deg;</span>
                    <span>135&deg;</span>
                    <span>180&deg;</span>
                </div>
            </div>

            <div class="btn-grid">
                <button onclick="setAngle(0)">0&deg;</button>
                <button onclick="setAngle(45)">45&deg;</button>
                <button onclick="setAngle(90)">90&deg;</button>
                <button onclick="setAngle(135)">135&deg;</button>
                <button onclick="setAngle(180)">180&deg;</button>
            </div>
        </div>

        <!-- HT517 I2S SPEAKER AUDIO CONTROL SECTION -->
        <div class="section-box">
            <div class="section-title">
                <span>Speaker Audio</span>
                <span style="color: var(--accent);">HT517 Amp</span>
            </div>

            <div style="margin-bottom: 8px;">
                <span style="font-size: 1.1rem; font-weight: 600; color: var(--muted-text);">Volume: </span>
                <span style="font-size: 1.4rem; font-weight: 700; color: var(--text-color);" id="volVal">80</span>
                <span style="font-size: 1.1rem; color: var(--primary);">%</span>
            </div>

            <div class="slider-wrap">
                <input type="range" id="volSlider" min="0" max="100" value="80" oninput="onVolSliderMove(this.value)" onchange="sendVolume(this.value)">
                <div class="range-labels">
                    <span>Mute</span>
                    <span>25%</span>
                    <span>50%</span>
                    <span>75%</span>
                    <span>100%</span>
                </div>
            </div>

            <div class="audio-btn-grid">
                <button class="btn-bark" onclick="playBark()">&#128054; Play Bark</button>
                <button class="btn-stop" onclick="stopAudio()">&#9209; Stop Audio</button>
            </div>
        </div>

        <div class="status-bar">
            <span class="status-dot"></span><span id="connStatus">System Ready</span>
        </div>
    </div>

    <script>
        let pendingAngle = null;
        let isAngleThrottled = false;

        let pendingVol = null;
        let isVolThrottled = false;

        function updateAngleDisplay(val) {
            document.getElementById("angleVal").innerText = val;
            document.getElementById("angleSlider").value = val;
        }

        function onAngleSliderMove(val) {
            document.getElementById("angleVal").innerText = val;
            if (!isAngleThrottled) {
                sendAngle(val);
                isAngleThrottled = true;
                setTimeout(() => {
                    isAngleThrottled = false;
                    if (pendingAngle !== null) {
                        sendAngle(pendingAngle);
                        pendingAngle = null;
                    }
                }, 60);
            } else {
                pendingAngle = val;
            }
        }

        function setAngle(val) {
            updateAngleDisplay(val);
            sendAngle(val);
        }

        function sendAngle(val) {
            document.getElementById("connStatus").innerText = "Updating servo...";
            fetch(`/set?angle=${val}`)
                .then(res => res.text())
                .then(text => {
                    document.getElementById("connStatus").innerText = "Servo Angle: " + val + "°";
                })
                .catch(err => {
                    document.getElementById("connStatus").innerText = "Servo communication error";
                });
        }

        function updateVolDisplay(val) {
            document.getElementById("volVal").innerText = val;
            document.getElementById("volSlider").value = val;
        }

        function onVolSliderMove(val) {
            document.getElementById("volVal").innerText = val;
            if (!isVolThrottled) {
                sendVolume(val);
                isVolThrottled = true;
                setTimeout(() => {
                    isVolThrottled = false;
                    if (pendingVol !== null) {
                        sendVolume(pendingVol);
                        pendingVol = null;
                    }
                }, 80);
            } else {
                pendingVol = val;
            }
        }

        function sendVolume(val) {
            document.getElementById("connStatus").innerText = "Setting volume...";
            fetch(`/volume?val=${val}`)
                .then(res => res.text())
                .then(text => {
                    document.getElementById("connStatus").innerText = "Volume: " + val + "%";
                })
                .catch(err => {
                    document.getElementById("connStatus").innerText = "Volume error";
                });
        }

        function playBark() {
            document.getElementById("connStatus").innerText = "Playing dog bark...";
            fetch('/play?sound=dog_bark')
                .then(res => res.text())
                .then(text => {
                    document.getElementById("connStatus").innerText = "Playing dog bark audio";
                })
                .catch(err => {
                    document.getElementById("connStatus").innerText = "Audio playback request failed";
                });
        }

        function stopAudio() {
            document.getElementById("connStatus").innerText = "Stopping audio...";
            fetch('/stop')
                .then(res => res.text())
                .then(text => {
                    document.getElementById("connStatus").innerText = "Audio stopped";
                })
                .catch(err => {
                    document.getElementById("connStatus").innerText = "Stop command failed";
                });
        }

        window.onload = function() {
            fetch('/get')
                .then(res => res.json())
                .then(data => {
                    if (data.angle !== undefined) {
                        updateAngleDisplay(data.angle);
                    }
                    if (data.volume !== undefined) {
                        updateVolDisplay(data.volume);
                    }
                })
                .catch(err => {
                    console.log("Failed to load initial status", err);
                });
        };
    </script>
</body>
</html>
)rawliteral";

// =========================================================================
// HTTP SERVER HANDLERS
// =========================================================================
static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t set_angle_handler(httpd_req_t *req) {
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[16] = {0};
        if (httpd_query_key_value(query, "angle", param, sizeof(param)) == ESP_OK) {
            int requested_angle = atoi(param);
            servo_set_angle(requested_angle);

            char resp[32];
            snprintf(resp, sizeof(resp), "OK %d", current_servo_angle);
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    }

    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'angle' query param");
    return ESP_FAIL;
}

static esp_err_t get_status_handler(httpd_req_t *req) {
    char json_resp[64];
    snprintf(json_resp, sizeof(json_resp), "{\"angle\":%d,\"volume\":%d}", current_servo_angle, audio_get_volume());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_resp, HTTPD_RESP_USE_STRLEN);
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
            if (vol < 0) vol = 0;
            if (vol > 100) vol = 100;
            audio_set_volume(vol);

            char resp[32];
            snprintf(resp, sizeof(resp), "OK %d", vol);
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    }

    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'val' query param");
    return ESP_FAIL;
}

static void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 10;
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_root = {};
        uri_root.uri      = "/";
        uri_root.method   = HTTP_GET;
        uri_root.handler  = root_get_handler;
        uri_root.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri_root);

        httpd_uri_t uri_set = {};
        uri_set.uri      = "/set";
        uri_set.method   = HTTP_GET;
        uri_set.handler  = set_angle_handler;
        uri_set.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri_set);

        httpd_uri_t uri_get = {};
        uri_get.uri      = "/get";
        uri_get.method   = HTTP_GET;
        uri_get.handler  = get_status_handler;
        uri_get.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri_get);

        httpd_uri_t uri_play = {};
        uri_play.uri      = "/play";
        uri_play.method   = HTTP_GET;
        uri_play.handler  = play_sound_handler;
        uri_play.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri_play);

        httpd_uri_t uri_stop = {};
        uri_stop.uri      = "/stop";
        uri_stop.method   = HTTP_GET;
        uri_stop.handler  = stop_sound_handler;
        uri_stop.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri_stop);

        httpd_uri_t uri_volume = {};
        uri_volume.uri      = "/volume";
        uri_volume.method   = HTTP_GET;
        uri_volume.handler  = set_volume_handler;
        uri_volume.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri_volume);

        ESP_LOGI(TAG, "HTTP Server active on port %d", config.server_port);
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server!");
    }
}

// =========================================================================
// SOFT-AP WI-FI INITIALIZATION
// =========================================================================
static void wifi_init_softap(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.ap.ssid, "ESP32S3-Servo");
    wifi_config.ap.ssid_len = strlen("ESP32S3-Servo");
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "Wi-Fi AP started. SSID: ESP32S3-Servo");
    ESP_LOGI(TAG, "Connect to Wi-Fi and open http://192.168.4.1");
    ESP_LOGI(TAG, "==================================================");
}

// =========================================================================
// APPLICATION ENTRY POINT
// =========================================================================
extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 1. Initialize Wi-Fi stack & TCP/IP FIRST so lwIP sockets are ready
    wifi_init_softap();

    // 2. Initialize hardware PWM on GPIO 4 for servo control
    servo_init();

    // 3. Initialize HT517 I2S audio driver & background tasks
    audio_player_init();

    // 4. Start web server
    start_webserver();

    // 5. Start serial input task for interactive REPL control
    xTaskCreate(console_read_task, "console_read_task", 4096, NULL, 5, NULL);
}