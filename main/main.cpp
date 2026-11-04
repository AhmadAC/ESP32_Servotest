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
#include "driver/uart.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "audio_player.h"

static const char *TAG = "MAIN_APP";

// =========================================================================
// HARDWARE PIN CONFIGURATION
// =========================================================================
#define SERVO_PIN           GPIO_NUM_4
#define LEDC_TIMER          LEDC_TIMER_0
#define LEDC_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL        LEDC_CHANNEL_0
#define LEDC_DUTY_RES       LEDC_TIMER_14_BIT // 16384 duty steps
#define LEDC_FREQUENCY      50                // 50 Hz standard servo PWM (20ms period)

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

    ESP_LOGI(TAG, "Servo initialized on GPIO %d (Angle: %d deg)", SERVO_PIN, current_servo_angle);
}

static void servo_set_angle(int angle) {
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;

    current_servo_angle = angle;
    uint32_t duty = angle_to_duty(angle);

    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));

    ESP_LOGI(TAG, "Servo moved to %d deg (duty: %lu)", angle, (unsigned long)duty);
}

// =========================================================================
// CAPTIVE PORTAL DNS SERVER (UDP PORT 53)
// =========================================================================
#pragma pack(push, 1)
typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;
#pragma pack(pop)

static void dns_server_task(void *pvParameters) {
    uint8_t rx_buffer[512];
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(53);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS socket bind failed: %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Captive DNS Server running on UDP port 53 -> Resolves all queries to 192.168.4.1");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&client_addr, &addr_len);
        if (len < (int)sizeof(dns_header_t)) {
            continue;
        }

        dns_header_t *dns_hdr = (dns_header_t *)rx_buffer;
        // Check if standard query (QR == 0)
        if ((ntohs(dns_hdr->flags) & 0x8000) != 0) {
            continue;
        }

        // Parse Question section to find end of QNAME
        int qname_pos = sizeof(dns_header_t);
        while (qname_pos < len && rx_buffer[qname_pos] != 0) {
            qname_pos += rx_buffer[qname_pos] + 1;
        }
        qname_pos++; // skip terminating 0 byte
        int qend_pos = qname_pos + 4; // skip QTYPE (2) + QCLASS (2)

        if (qend_pos > len || qend_pos + 16 > (int)sizeof(rx_buffer)) {
            continue;
        }

        // Formulate standard DNS Answer pointing to 192.168.4.1
        dns_hdr->flags    = htons(0x8180); // Standard response, recursion available, no error
        dns_hdr->qd_count = htons(1);
        dns_hdr->an_count = htons(1);
        dns_hdr->ns_count = 0;
        dns_hdr->ar_count = 0;

        uint8_t *ans = rx_buffer + qend_pos;
        // Compression pointer to Question Name at offset 12 (0xC00C)
        ans[0] = 0xC0;
        ans[1] = 0x0C;
        // TYPE: A (IPv4)
        ans[2] = 0x00;
        ans[3] = 0x01;
        // CLASS: IN
        ans[4] = 0x00;
        ans[5] = 0x01;
        // TTL: 60s
        ans[6] = 0x00;
        ans[7] = 0x00;
        ans[8] = 0x00;
        ans[9] = 0x3C;
        // RDLENGTH: 4 bytes
        ans[10] = 0x00;
        ans[11] = 0x04;
        // RDATA: 192.168.4.1
        ans[12] = 192;
        ans[13] = 168;
        ans[14] = 4;
        ans[15] = 1;

        int resp_len = qend_pos + 16;
        sendto(sock, rx_buffer, resp_len, 0, (struct sockaddr *)&client_addr, addr_len);
    }
}

// =========================================================================
// SERIAL CONSOLE TASK (USB SERIAL/JTAG + UART0 BIDIRECTIONAL REPL)
// =========================================================================
static void console_print(const char *str) {
    printf("%s", str);
    fflush(stdout);
    uart_write_bytes(UART_NUM_0, str, strlen(str));
}

static void console_read_task(void *pvParameter) {
    setvbuf(stdout, NULL, _IONBF, 0);

    // Initialize UART0 driver on GPIO 43/44 so secondary UART COM port also works
    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate = 115200;
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity    = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    uart_param_config(UART_NUM_0, &uart_cfg);
    uart_driver_install(UART_NUM_0, 512, 512, 0, NULL, 0);

    // Set standard input to non-blocking
    int fd = fileno(stdin);
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    vTaskDelay(pdMS_TO_TICKS(400));

    const char *banner =
        "\n==================================================\n"
        "   YD-ESP32-S3 Interactive Console Ready\n"
        "==================================================\n"
        "Servo Commands:\n"
        "  0 - 180          : Move servo angle directly (e.g. '90')\n"
        "  angle <N>        : Move servo angle (e.g. 'angle 45')\n"
        "Speaker Audio Commands:\n"
        "  bark / play      : Play dog bark sound on HT517\n"
        "  beep / test      : Play diagnostic test beep on HT517\n"
        "  stop             : Stop speaker playback\n"
        "  vol <0-100>      : Set volume percentage (e.g. 'vol 85')\n"
        "General Commands:\n"
        "  status           : Show servo angle, volume & Wi-Fi IP\n"
        "  reboot / restart : Restart ESP32-S3 board\n"
        "  help             : Show command list\n"
        "==================================================\n"
        "REPL> ";
    console_print(banner);

    char cmd_buf[128];
    int cmd_idx = 0;

    while (1) {
        uint8_t c = 0;
        bool has_char = false;

        // 1. Read from STDIN (USB Serial/JTAG primary console)
        if (read(fd, &c, 1) > 0) {
            has_char = true;
        } else {
            // 2. Read from UART0 (CH340/CH343 COM port)
            if (uart_read_bytes(UART_NUM_0, &c, 1, 0) > 0) {
                has_char = true;
            }
        }

        if (has_char) {
            // Handle Ctrl+C (Clear current line)
            if (c == 0x03) {
                console_print("\n[Line Cleared]\nREPL> ");
                cmd_idx = 0;
                continue;
            }

            // Handle Enter (Newline)
            if (c == '\r' || c == '\n') {
                if (cmd_idx > 0) {
                    cmd_buf[cmd_idx] = '\0';
                    console_print("\n");

                    char *p = cmd_buf;
                    while (*p == ' ' || *p == '\t') p++;

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
                            char msg[64];
                            snprintf(msg, sizeof(msg), ">> Servo moved to %d deg\n", req_angle);
                            console_print(msg);
                        } else {
                            console_print(">> Error: Angle must be between 0 and 180 degrees.\n");
                        }
                    } else if (strncmp(p, "angle ", 6) == 0) {
                        int req_angle = atoi(p + 6);
                        if (req_angle >= 0 && req_angle <= 180) {
                            servo_set_angle(req_angle);
                            char msg[64];
                            snprintf(msg, sizeof(msg), ">> Servo moved to %d deg\n", req_angle);
                            console_print(msg);
                        } else {
                            console_print(">> Error: Angle must be between 0 and 180 degrees.\n");
                        }
                    } else if (strcmp(p, "bark") == 0 || strcmp(p, "play") == 0 || strcmp(p, "sound") == 0) {
                        audio_play("dog_bark");
                        char msg[64];
                        snprintf(msg, sizeof(msg), ">> Playing dog bark on HT517 (Volume: %d%%)\n", audio_get_volume());
                        console_print(msg);
                    } else if (strcmp(p, "beep") == 0 || strcmp(p, "test") == 0) {
                        audio_play_test_beep();
                        console_print(">> Playing test beep chime on HT517 speaker...\n");
                    } else if (strcmp(p, "stop") == 0) {
                        audio_stop();
                        console_print(">> Audio playback stopped.\n");
                    } else if (strncmp(p, "vol ", 4) == 0 || strncmp(p, "volume ", 7) == 0) {
                        int req_vol = (strncmp(p, "vol ", 4) == 0) ? atoi(p + 4) : atoi(p + 7);
                        if (req_vol >= 0 && req_vol <= 100) {
                            audio_set_volume(req_vol);
                            char msg[64];
                            snprintf(msg, sizeof(msg), ">> Speaker volume set to %d%%\n", req_vol);
                            console_print(msg);
                        } else {
                            console_print(">> Error: Volume must be between 0 and 100.\n");
                        }
                    } else if (strcmp(p, "status") == 0) {
                        char msg[128];
                        snprintf(msg, sizeof(msg), ">> Servo: %d deg | Speaker Vol: %d%% | Web: http://192.168.4.1/\n",
                                 current_servo_angle, audio_get_volume());
                        console_print(msg);
                    } else if (strcmp(p, "help") == 0 || strcmp(p, "?") == 0) {
                        console_print("Available Commands:\n"
                                      "  0 - 180          : Move servo angle\n"
                                      "  bark / play      : Play dog bark sound\n"
                                      "  beep / test      : Play diagnostic beep\n"
                                      "  stop             : Stop speaker playback\n"
                                      "  vol <0-100>      : Set volume\n"
                                      "  status           : Show device status\n"
                                      "  reboot / restart : Restart ESP32-S3\n");
                    } else if (strcmp(p, "reboot") == 0 || strcmp(p, "restart") == 0) {
                        console_print("Rebooting ESP32-S3...\n");
                        vTaskDelay(pdMS_TO_TICKS(100));
                        esp_restart();
                    } else {
                        console_print(">> Unknown command: '");
                        console_print(p);
                        console_print("'. Enter a number (0-180), 'bark', 'beep', or 'help'.\n");
                    }

                    cmd_idx = 0;
                    console_print("REPL> ");
                } else {
                    console_print("REPL> ");
                }
            } else if (c == 0x08 || c == 0x7F) { // Backspace
                if (cmd_idx > 0) {
                    cmd_idx--;
                    console_print("\b \b");
                }
            } else if (c >= 0x20 && c <= 0x7E) {
                if (cmd_idx < (int)sizeof(cmd_buf) - 1) {
                    cmd_buf[cmd_idx++] = (char)c;
                    char echo_str[2] = {(char)c, '\0'};
                    console_print(echo_str);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// =========================================================================
// HTML / WEB INTERFACE (RESPONSIVE FOR IPHONE CNA & DESKTOP BROWSERS)
// =========================================================================
static const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <meta name="apple-mobile-web-app-capable" content="yes">
    <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
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
        * { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
            background-color: var(--bg-color);
            color: var(--text-color);
            display: flex;
            align-items: center;
            justify-content: center;
            min-height: 100vh;
            padding: 16px;
        }
        .container {
            background-color: var(--card-color);
            width: 100%;
            max-width: 440px;
            border-radius: 20px;
            padding: 24px 20px;
            box-shadow: 0 10px 25px rgba(0, 0, 0, 0.6);
            border: 1px solid var(--card-border);
            text-align: center;
        }
        h1 { font-size: 1.4rem; margin-bottom: 4px; color: var(--primary); }
        .sub-header { font-size: 0.78rem; color: var(--muted-text); margin-bottom: 18px; }
        .section-box {
            background: #0f172a;
            border: 1px solid var(--card-border);
            border-radius: 14px;
            padding: 16px;
            margin-bottom: 16px;
        }
        .section-title {
            font-size: 0.75rem;
            font-weight: 700;
            letter-spacing: 0.08em;
            text-transform: uppercase;
            color: var(--muted-text);
            margin-bottom: 10px;
            display: flex;
            justify-content: space-between;
        }
        .readout { font-size: 3rem; font-weight: 700; color: var(--text-color); line-height: 1.1; }
        .readout-unit { font-size: 1.3rem; color: var(--primary); }
        .slider-wrap { margin: 14px 0 8px 0; }
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
            font-size: 0.72rem;
            color: var(--muted-text);
            margin-top: 6px;
        }
        .btn-grid {
            display: grid;
            grid-template-columns: repeat(5, 1fr);
            gap: 6px;
            margin-top: 12px;
        }
        .audio-btn-grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 10px;
            margin-top: 12px;
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
        button:active { transform: scale(0.96); }
        .btn-bark { background: var(--accent); padding: 13px 8px; font-size: 0.92rem; }
        .btn-beep { background: #0284c7; padding: 13px 8px; font-size: 0.92rem; }
        .btn-stop { background: #475569; padding: 13px 8px; font-size: 0.92rem; grid-column: span 2; }
        .btn-stop:active { background: var(--danger-hover); }
        .status-dot {
            display: inline-block;
            width: 8px;
            height: 8px;
            background-color: #22c55e;
            border-radius: 50%;
            margin-right: 6px;
        }
        .status-bar { margin-top: 12px; font-size: 0.78rem; color: var(--muted-text); }
    </style>
</head>
<body>
    <div class="container">
        <h1>YD-ESP32-S3 Dashboard</h1>
        <div class="sub-header">Servo (GPIO 4) &amp; HT517 Speaker (GPIO 10,11,12)</div>

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
                    <span>0&deg;</span><span>45&deg;</span><span>90&deg;</span><span>135&deg;</span><span>180&deg;</span>
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

        <!-- HT517 SPEAKER CONTROL SECTION -->
        <div class="section-box">
            <div class="section-title">
                <span>HT517 Speaker Audio</span>
                <span style="color: var(--accent);" id="volLabel">Vol: 80%</span>
            </div>
            <div class="slider-wrap">
                <input type="range" id="volSlider" min="0" max="100" value="80" oninput="onVolSliderMove(this.value)" onchange="sendVolume(this.value)">
                <div class="range-labels">
                    <span>Mute</span><span>25%</span><span>50%</span><span>75%</span><span>100%</span>
                </div>
            </div>
            <div class="audio-btn-grid">
                <button class="btn-bark" onclick="playBark()">&#128054; Dog Bark</button>
                <button class="btn-beep" onclick="playBeep()">&#128276; Test Tone</button>
                <button class="btn-stop" onclick="stopAudio()">&#9209; Stop Audio</button>
            </div>
        </div>

        <div class="status-bar">
            <span class="status-dot"></span><span id="connStatus">Portal Connected (192.168.4.1)</span>
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
            document.getElementById("connStatus").innerText = "Setting angle: " + val + "°";
            fetch('/set?angle=' + val)
                .then(r => r.text())
                .then(t => { document.getElementById("connStatus").innerText = "Servo Angle: " + val + "°"; })
                .catch(e => { document.getElementById("connStatus").innerText = "Servo command error"; });
        }

        function updateVolDisplay(val) {
            document.getElementById("volLabel").innerText = "Vol: " + val + "%";
            document.getElementById("volSlider").value = val;
        }

        function onVolSliderMove(val) {
            document.getElementById("volLabel").innerText = "Vol: " + val + "%";
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
            fetch('/volume?val=' + val)
                .then(r => r.text())
                .then(t => { document.getElementById("connStatus").innerText = "Speaker Volume: " + val + "%"; })
                .catch(e => { document.getElementById("connStatus").innerText = "Volume error"; });
        }

        function playBark() {
            document.getElementById("connStatus").innerText = "Playing dog bark audio...";
            fetch('/play?sound=dog_bark')
                .then(r => r.text())
                .then(t => { document.getElementById("connStatus").innerText = "Dog bark playing on HT517"; })
                .catch(e => { document.getElementById("connStatus").innerText = "Play command error"; });
        }

        function playBeep() {
            document.getElementById("connStatus").innerText = "Playing diagnostic test tone...";
            fetch('/beep')
                .then(r => r.text())
                .then(t => { document.getElementById("connStatus").innerText = "Test tone playing on HT517"; })
                .catch(e => { document.getElementById("connStatus").innerText = "Beep command error"; });
        }

        function stopAudio() {
            document.getElementById("connStatus").innerText = "Stopping speaker audio...";
            fetch('/stop')
                .then(r => r.text())
                .then(t => { document.getElementById("connStatus").innerText = "Speaker playback stopped"; })
                .catch(e => { document.getElementById("connStatus").innerText = "Stop command error"; });
        }

        window.onload = function() {
            fetch('/get')
                .then(r => r.json())
                .then(data => {
                    if (data.angle !== undefined) updateAngleDisplay(data.angle);
                    if (data.volume !== undefined) updateVolDisplay(data.volume);
                })
                .catch(err => { console.log("Init status error", err); });
        };
    </script>
</body>
</html>
)rawliteral";

// =========================================================================
// HTTP SERVER HANDLERS & CAPTIVE REDIRECTORS
// =========================================================================
static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Intercepts Apple CNA & Android probes and sends HTTP 302 Found to trigger auto-open
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

            char resp[32];
            snprintf(resp, sizeof(resp), "OK %d", audio_get_volume());
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
    config.max_uri_handlers = 20; // Support app routes + captive portal routes
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri = {};

        // Core application endpoints
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

        // Apple iOS Captive Network Assistant detection endpoints
        uri.method = HTTP_GET;
        uri.handler = captive_redirect_handler;
        uri.user_ctx = NULL;

        uri.uri = "/hotspot-detect.html";
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/canonical.html";
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/library/test/success.html";
        httpd_register_uri_handler(server, &uri);

        // Android / Windows / Chrome captive endpoints
        uri.uri = "/generate_204";
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/gen_204";
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/ncsi.txt";
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/connecttest.txt";
        httpd_register_uri_handler(server, &uri);

        // Catch-all 404 handler redirecting all external domain requests to http://192.168.4.1/
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);

        ESP_LOGI(TAG, "HTTP Server active on port 80 with Captive Portal redirection");
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
    wifi_config.ap.beacon_interval = 100;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Disable Wi-Fi power save to prevent iPhone disconnections or packet latency
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "Wi-Fi AP active. SSID: 'ESP32S3-Servo'");
    ESP_LOGI(TAG, "Web Portal: http://192.168.4.1/");
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

    // 1. Initialize Wi-Fi stack & TCP/IP
    wifi_init_softap();

    // 2. Start Captive DNS Server (UDP port 53)
    xTaskCreatePinnedToCore(dns_server_task, "dns_server_task", 4096, NULL, 5, NULL, 0);

    // 3. Initialize hardware PWM on GPIO 4 for servo control
    servo_init();

    // 4. Initialize HT517 I2S audio driver & background tasks
    audio_player_init();

    // 5. Start HTTP web server with Captive Portal routes
    start_webserver();

    // 6. Start bidirectional console task (USB Serial/JTAG + UART0)
    xTaskCreate(console_read_task, "console_read_task", 4096, NULL, 5, NULL);
}