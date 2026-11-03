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

static const char *TAG = "SERVO_WEB";

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
// SERIAL REPL CONSOLE TASK (INTERACTIVE REPL & CTRL+C / CTRL+D)
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
    printf("Commands:\n");
    printf("  0 - 180   : Set servo angle directly (e.g. '90')\n");
    printf("  angle <N> : Set angle (e.g. 'angle 45')\n");
    printf("  status    : Show current servo angle & IP\n");
    printf("  help      : Show this help guide\n");
    printf("  Ctrl+C    : Interrupt / Clear input\n");
    printf("  Ctrl+D    : Soft Reboot (Restart board)\n");
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

                // --- 1. Handle Ctrl+C (Interrupt) ---
                if (c == 0x03) {
                    printf("\n[Sent Ctrl+C - Interrupt]\nREPL> ");
                    fflush(stdout);
                    cmd_idx = 0;
                    continue;
                }

                // --- 2. Handle Ctrl+D (Soft Reboot) ---
                if (c == 0x04) {
                    printf("\n[Sent Ctrl+D - Soft Reboot]\n");
                    printf("Rebooting ESP32-S3 now...\n");
                    fflush(stdout);
                    vTaskDelay(pdMS_TO_TICKS(150));
                    esp_restart();
                }

                // --- 3. Handle Enter / Newline (Execute Command) ---
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
                        } else if (strcmp(p, "status") == 0) {
                            printf(">> Current Servo Angle: %d deg\n", current_servo_angle);
                            printf(">> Web Dashboard: http://192.168.4.1/\n");
                        } else if (strcmp(p, "help") == 0 || strcmp(p, "?") == 0) {
                            printf("Commands: 0-180, 'angle <N>', 'status', 'help', Ctrl+C, Ctrl+D\n");
                        } else if (strcmp(p, "reboot") == 0 || strcmp(p, "restart") == 0) {
                            printf("Rebooting...\n");
                            vTaskDelay(pdMS_TO_TICKS(100));
                            esp_restart();
                        } else {
                            printf(">> Unknown command: '%s'. Enter a number (0-180) or 'help'.\n", p);
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
                } else {
                    // Store character and echo back
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
    <title>YD-ESP32-S3 Servo Dashboard</title>
    <style>
        :root {
            --bg-color: #0f172a;
            --card-color: #1e293b;
            --primary: #38bdf8;
            --accent: #0284c7;
            --text-color: #f8fafc;
            --muted-text: #94a3b8;
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
            padding: 16px;
            box-sizing: border-box;
        }
        .container {
            background-color: var(--card-color);
            width: 100%;
            max-width: 440px;
            border-radius: 20px;
            padding: 30px;
            box-shadow: 0 10px 25px -5px rgba(0, 0, 0, 0.5);
            border: 1px solid #334155;
            text-align: center;
        }
        h1 {
            font-size: 1.4rem;
            margin-bottom: 6px;
            color: var(--primary);
        }
        .sub-header {
            font-size: 0.85rem;
            color: var(--muted-text);
            margin-bottom: 24px;
        }
        .dial-container {
            margin: 20px 0;
        }
        .angle-readout {
            font-size: 3.8rem;
            font-weight: 700;
            color: var(--text-color);
            font-variant-numeric: tabular-nums;
        }
        .angle-unit {
            font-size: 1.5rem;
            color: var(--primary);
        }
        .slider-container {
            margin: 25px 0 15px 0;
        }
        input[type=range] {
            -webkit-appearance: none;
            width: 100%;
            height: 12px;
            border-radius: 6px;
            background: #334155;
            outline: none;
        }
        input[type=range]::-webkit-slider-thumb {
            -webkit-appearance: none;
            width: 32px;
            height: 32px;
            border-radius: 50%;
            background: var(--primary);
            cursor: pointer;
            box-shadow: 0 0 10px rgba(56, 189, 248, 0.5);
            border: 2px solid #ffffff;
        }
        .range-labels {
            display: flex;
            justify-content: space-between;
            font-size: 0.85rem;
            color: var(--muted-text);
            margin-top: 8px;
        }
        .btn-grid {
            display: grid;
            grid-template-columns: repeat(5, 1fr);
            gap: 8px;
            margin-top: 24px;
        }
        button {
            background: #334155;
            border: none;
            border-radius: 10px;
            color: var(--text-color);
            padding: 12px 4px;
            font-weight: 600;
            font-size: 0.85rem;
            cursor: pointer;
            transition: all 0.15s ease;
        }
        button:hover {
            background: var(--accent);
            color: #ffffff;
        }
        button:active {
            transform: scale(0.96);
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
            margin-top: 20px;
            font-size: 0.8rem;
            color: var(--muted-text);
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>YD-ESP32-S3 Servo</h1>
        <div class="sub-header">LEDC 50Hz PWM on GPIO 4</div>

        <div class="dial-container">
            <span class="angle-readout" id="angleVal">90</span><span class="angle-unit">&deg;</span>
        </div>

        <div class="slider-container">
            <input type="range" id="angleSlider" min="0" max="180" value="90" oninput="onSliderMove(this.value)" onchange="sendAngle(this.value)">
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

        <div class="status-bar">
            <span class="status-dot"></span><span id="connStatus">Ready</span>
        </div>
    </div>

    <script>
        let pendingAngle = null;
        let isThrottled = false;

        function updateDisplay(val) {
            document.getElementById("angleVal").innerText = val;
            document.getElementById("angleSlider").value = val;
        }

        function onSliderMove(val) {
            document.getElementById("angleVal").innerText = val;
            throttleSend(val);
        }

        function setAngle(val) {
            updateDisplay(val);
            sendAngle(val);
        }

        function throttleSend(val) {
            if (!isThrottled) {
                sendAngle(val);
                isThrottled = true;
                setTimeout(() => {
                    isThrottled = false;
                    if (pendingAngle !== null) {
                        sendAngle(pendingAngle);
                        pendingAngle = null;
                    }
                }, 60);
            } else {
                pendingAngle = val;
            }
        }

        function sendAngle(val) {
            document.getElementById("connStatus").innerText = "Updating...";
            fetch(`/set?angle=${val}`)
                .then(res => res.text())
                .then(text => {
                    document.getElementById("connStatus").innerText = "Angle: " + val + "°";
                })
                .catch(err => {
                    document.getElementById("connStatus").innerText = "Communication error";
                });
        }

        window.onload = function() {
            fetch('/get')
                .then(res => res.json())
                .then(data => {
                    if (data.angle !== undefined) {
                        updateDisplay(data.angle);
                    }
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

static esp_err_t get_angle_handler(httpd_req_t *req) {
    char json_resp[48];
    snprintf(json_resp, sizeof(json_resp), "{\"angle\":%d}", current_servo_angle);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8;
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
        uri_get.handler  = get_angle_handler;
        uri_get.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri_get);

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

    // 1. Initialize hardware PWM on GPIO 4
    servo_init();

    // 2. Start Soft-AP Wi-Fi network
    wifi_init_softap();

    // 3. Start web server
    start_webserver();

    // 4. Start serial input task for interactive REPL control
    xTaskCreate(console_read_task, "console_read_task", 4096, NULL, 5, NULL);
}