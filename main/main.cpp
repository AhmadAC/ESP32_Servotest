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
#include "driver/usb_serial_jtag.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "audio_player.h"
#include "ble_server.h"
#include "keypad.h"

static const char *TAG = "MAIN_APP";

// =========================================================================
// HARDWARE PIN CONFIGURATION
// =========================================================================
#define SERVO_PIN           GPIO_NUM_4
#define LEDC_TIMER          LEDC_TIMER_0
#define LEDC_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL        LEDC_CHANNEL_0
#define LEDC_DUTY_RES       LEDC_TIMER_14_BIT
#define LEDC_FREQUENCY      50

#define SERVO_MIN_US        500
#define SERVO_MAX_US        2500

static int current_servo_angle = 90;
static httpd_handle_t server = NULL;

// Forward Declarations
static void notify_all_status(void);
static void process_unified_command(const char *cmd_line, void (*print_fn)(const char *));
static void console_print(const char *str);

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

// Broadcasts status string to connected BLE clients: STATUS:angle=A,vol=V,bclk=B,lrc=L,din=D
static void notify_all_status(void) {
    if (!ble_server_is_connected()) return;

    int bclk = 10, lrc = 11, din = 12;
    audio_get_pins(&bclk, &lrc, &din);

    char status_str[128];
    snprintf(status_str, sizeof(status_str),
             "STATUS:angle=%d,vol=%d,bclk=%d,lrc=%d,din=%d\n",
             current_servo_angle, audio_get_volume(), bclk, lrc, din);
    ble_server_send_notify(status_str);
}

// =========================================================================
// UNIFIED COMMAND PROCESSOR (SERIAL, BLE, KEYPAD & WEB SHARE THIS LOGIC)
// =========================================================================
static void process_unified_command(const char *cmd_line, void (*print_fn)(const char *)) {
    if (!cmd_line || !print_fn) return;

    const char *p = cmd_line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return;

    // Check if command is direct integer angle (e.g. "90")
    bool is_numeric = true;
    int p_len = strlen(p);
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
            print_fn(msg);
            notify_all_status();
        } else {
            print_fn(">> Error: Angle must be between 0 and 180 degrees.\n");
        }
        return;
    }

    // Relative claw nudging: "nudge <+/-deg>" or "+5" or "-10"
    if (strncmp(p, "nudge ", 6) == 0 ||
        ((p[0] == '+' || p[0] == '-') && isdigit((unsigned char)p[1]))) {
        int delta = (strncmp(p, "nudge ", 6) == 0) ? atoi(p + 6) : atoi(p);
        int target = current_servo_angle + delta;
        if (target < 0) target = 0;
        if (target > 180) target = 180;
        servo_set_angle(target);
        char msg[64];
        snprintf(msg, sizeof(msg), ">> Servo nudged by %+d -> %d deg\n", delta, target);
        print_fn(msg);
        notify_all_status();
        return;
    }

    if (strncmp(p, "angle ", 6) == 0) {
        int req_angle = atoi(p + 6);
        if (req_angle >= 0 && req_angle <= 180) {
            servo_set_angle(req_angle);
            char msg[64];
            snprintf(msg, sizeof(msg), ">> Servo moved to %d deg\n", req_angle);
            print_fn(msg);
            notify_all_status();
        } else {
            print_fn(">> Error: Angle must be between 0 and 180 degrees.\n");
        }
    } else if (strcmp(p, "bark") == 0 || strcmp(p, "play") == 0 || strcmp(p, "sound") == 0) {
        audio_play("dog_bark");
        char msg[64];
        snprintf(msg, sizeof(msg), ">> Playing dog bark on HT517 (Volume: %d%%)\n", audio_get_volume());
        print_fn(msg);
    } else if (strcmp(p, "beep") == 0 || strcmp(p, "test") == 0) {
        audio_play_test_beep();
        print_fn(">> Playing test beep chime on HT517 speaker...\n");
    } else if (strcmp(p, "stop") == 0) {
        audio_stop();
        print_fn(">> Audio playback stopped.\n");
    } else if (strncmp(p, "vol ", 4) == 0 || strncmp(p, "volume ", 7) == 0) {
        const char *v_str = (strncmp(p, "vol ", 4) == 0) ? (p + 4) : (p + 7);
        while (*v_str == ' ') v_str++;
        int target_vol = 0;
        if (v_str[0] == '+' || v_str[0] == '-') {
            target_vol = audio_get_volume() + atoi(v_str);
        } else {
            target_vol = atoi(v_str);
        }
        if (target_vol < 0) target_vol = 0;
        if (target_vol > 100) target_vol = 100;
        audio_set_volume(target_vol);
        char msg[64];
        snprintf(msg, sizeof(msg), ">> Speaker volume set to %d%%\n", target_vol);
        print_fn(msg);
        notify_all_status();
    } else if (strncmp(p, "pins ", 5) == 0) {
        int b = 0, l = 0, d = 0;
        if (sscanf(p + 5, "%d %d %d", &b, &l, &d) == 3) {
            audio_set_pins(b, l, d);
            char msg[64];
            snprintf(msg, sizeof(msg), ">> Audio pins updated: BCLK=%d, LRC=%d, DIN=%d\n", b, l, d);
            print_fn(msg);
            notify_all_status();
        } else {
            print_fn(">> Usage: pins <bclk> <lrc> <din> (e.g. 'pins 10 11 12')\n");
        }
    } else if (strcmp(p, "status") == 0) {
        int bclk = 10, lrc = 11, din = 12;
        audio_get_pins(&bclk, &lrc, &din);
        char msg[140];
        snprintf(msg, sizeof(msg),
                 ">> Servo: %d deg | Vol: %d%% | Pins: BCLK=%d LRC=%d DIN=%d | BLE: %s | Web: http://192.168.4.1/\n",
                 current_servo_angle, audio_get_volume(), bclk, lrc, din,
                 ble_server_is_connected() ? "Connected" : "Advertising");
        print_fn(msg);
        notify_all_status();
    } else if (strcmp(p, "help") == 0 || strcmp(p, "?") == 0) {
        print_fn("Available Commands:\n"
                 "  0 - 180          : Move servo angle directly\n"
                 "  angle <0-180>    : Move servo angle\n"
                 "  nudge <+/-deg>   : Nudge angle (e.g. 'nudge +10', '-5')\n"
                 "  bark / play      : Play dog bark sound\n"
                 "  beep / test      : Play diagnostic tone\n"
                 "  stop             : Stop speaker playback\n"
                 "  vol <0-100>      : Set volume (or 'vol +10', 'vol -10')\n"
                 "  pins 10 11 12    : Preset 1 (YD Default)\n"
                 "  pins 8 7 9       : Preset 2 (Seeed)\n"
                 "  pins 12 11 10    : Preset 3 (Swapped)\n"
                 "  status           : Show device status\n"
                 "  reboot / restart : Restart ESP32-S3\n");
    } else if (strcmp(p, "reboot") == 0 || strcmp(p, "restart") == 0) {
        print_fn("Rebooting ESP32-S3...\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else {
        char err_msg[64];
        snprintf(err_msg, sizeof(err_msg), ">> Unknown command: '%s'. Enter 'help'.\n", p);
        print_fn(err_msg);
    }
}

// Callback invoked when incoming data arrives via BLE RX characteristic
static void on_ble_command(const char *cmd, void (*reply_fn)(const char *reply)) {
    ESP_LOGI(TAG, "BLE Command received: '%s'", cmd);
    process_unified_command(cmd, reply_fn);
}

// Callback invoked when a 16-key matrix keyboard button is pressed
static void on_keypad_command(const char *cmd) {
    char log_buf[64];
    snprintf(log_buf, sizeof(log_buf), "\n[Matrix Keypad Action] -> '%s'\n", cmd);
    console_print(log_buf);
    process_unified_command(cmd, console_print);
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
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Captive DNS Server running on UDP port 53");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&client_addr, &addr_len);
        if (len < (int)sizeof(dns_header_t)) {
            continue;
        }

        dns_header_t *dns_hdr = (dns_header_t *)rx_buffer;
        if ((ntohs(dns_hdr->flags) & 0x8000) != 0) {
            continue;
        }

        int qname_pos = sizeof(dns_header_t);
        while (qname_pos < len && rx_buffer[qname_pos] != 0) {
            qname_pos += rx_buffer[qname_pos] + 1;
        }
        qname_pos++;
        int qend_pos = qname_pos + 4;

        if (qend_pos > len || qend_pos + 16 > (int)sizeof(rx_buffer)) {
            continue;
        }

        dns_hdr->flags    = htons(0x8180);
        dns_hdr->qd_count = htons(1);
        dns_hdr->an_count = htons(1);
        dns_hdr->ns_count = 0;
        dns_hdr->ar_count = 0;

        uint8_t *ans = rx_buffer + qend_pos;
        ans[0] = 0xC0;
        ans[1] = 0x0C;
        ans[2] = 0x00;
        ans[3] = 0x01;
        ans[4] = 0x00;
        ans[5] = 0x01;
        ans[6] = 0x00;
        ans[7] = 0x00;
        ans[8] = 0x00;
        ans[9] = 0x3C;
        ans[10] = 0x00;
        ans[11] = 0x04;
        ans[12] = 192;
        ans[13] = 168;
        ans[14] = 4;
        ans[15] = 1;

        int resp_len = qend_pos + 16;
        sendto(sock, rx_buffer, resp_len, 0, (struct sockaddr *)&client_addr, addr_len);
    }
}

// =========================================================================
// SERIAL CONSOLE TASK (UART0 & USB SERIAL/JTAG)
// =========================================================================
static void console_print(const char *str) {
    size_t len = strlen(str);
    uart_write_bytes(UART_NUM_0, str, len);
    usb_serial_jtag_write_bytes(str, len, 0);
}

static void console_read_task(void *pvParameter) {
    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate = 115200;
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity    = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    uart_param_config(UART_NUM_0, &uart_cfg);
    uart_driver_install(UART_NUM_0, 512, 512, 0, NULL, 0);

    usb_serial_jtag_driver_config_t usbjtag_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&usbjtag_cfg);

    vTaskDelay(pdMS_TO_TICKS(400));

    const char *banner =
        "\n==================================================\n"
        "   YD-ESP32-S3 Interactive Console Ready\n"
        "   Control Modes: Wi-Fi, BLE, 4x4 Keypad & USB UART\n"
        "==================================================\n"
        "Commands: '0-180', 'nudge <+/-deg>', 'bark', 'beep',\n"
        "          'vol <0-100>', 'pins 10 11 12', 'status', 'help'\n"
        "==================================================\n"
        "REPL> ";
    console_print(banner);

    char cmd_buf[128];
    int cmd_idx = 0;

    while (1) {
        uint8_t c = 0;
        bool has_char = false;

        if (uart_read_bytes(UART_NUM_0, &c, 1, 0) > 0) {
            has_char = true;
        } else if (usb_serial_jtag_read_bytes(&c, 1, 0) > 0) {
            has_char = true;
        }

        if (has_char) {
            if (c == 0x03) {
                console_print("\n[Line Cleared]\nREPL> ");
                cmd_idx = 0;
                continue;
            }

            if (c == '\r' || c == '\n') {
                if (cmd_idx > 0) {
                    cmd_buf[cmd_idx] = '\0';
                    console_print("\n");
                    process_unified_command(cmd_buf, console_print);
                    cmd_idx = 0;
                    console_print("REPL> ");
                } else {
                    console_print("REPL> ");
                }
            } else if (c == 0x08 || c == 0x7F) {
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
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

// =========================================================================
// HTML / WEB PORTAL INTERFACE
// =========================================================================
static const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <meta name="apple-mobile-web-app-capable" content="yes">
    <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
    <title>YD-ESP32-S3 Claw Controller</title>
    <style>
        :root {
            --bg-color: #0b1120;
            --card-color: #1e293b;
            --card-border: #334155;
            --primary: #38bdf8;
            --primary-hover: #0284c7;
            --accent: #10b981;
            --accent-hover: #059669;
            --warning: #f59e0b;
            --danger: #ef4444;
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
            max-width: 480px;
            border-radius: 20px;
            padding: 24px 20px;
            box-shadow: 0 10px 25px rgba(0, 0, 0, 0.6);
            border: 1px solid var(--card-border);
            text-align: center;
        }
        h1 { font-size: 1.35rem; margin-bottom: 4px; color: var(--primary); }
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
        .btn-pin { background: #1e293b; border: 1px solid #475569; padding: 8px 6px; font-size: 0.72rem; color: #cbd5e1; border-radius: 8px; }
        .btn-pin.active { background: #0284c7; border-color: #38bdf8; color: #fff; font-weight: bold; }
        
        /* 16 Matrix Keypad Grid Styling */
        .keypad-matrix {
            display: grid;
            grid-template-columns: repeat(4, 1fr);
            gap: 6px;
            margin-top: 10px;
        }
        .keypad-btn {
            background: #1e293b;
            border: 1px solid var(--card-border);
            border-radius: 10px;
            padding: 8px 4px;
            display: flex;
            flex-direction: column;
            align-items: center;
            justify-content: center;
            min-height: 52px;
            cursor: pointer;
            transition: all 0.15s ease;
        }
        .keypad-btn:hover { border-color: var(--primary); background: #24334a; }
        .keypad-btn.active-edit { border-color: #38bdf8; background: #0369a1; box-shadow: 0 0 8px rgba(56, 189, 248, 0.4); }
        .key-num { font-size: 0.65rem; color: var(--primary); font-weight: 700; text-transform: uppercase; }
        .key-coord { font-size: 0.58rem; color: var(--muted-text); }
        .key-cmd { font-size: 0.74rem; font-weight: 600; color: #f8fafc; margin-top: 2px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 90px; }
        
        .editor-box {
            background: #111827;
            border: 1px solid #3b82f6;
            border-radius: 12px;
            padding: 12px;
            margin-top: 12px;
            text-align: left;
        }
        .editor-header { display: flex; justify-content: space-between; align-items: center; font-size: 0.78rem; font-weight: 700; color: #93c5fd; margin-bottom: 8px; }
        .editor-select, .editor-input {
            width: 100%;
            background: #1e293b;
            border: 1px solid #475569;
            color: #fff;
            padding: 8px 10px;
            border-radius: 8px;
            font-size: 0.8rem;
            margin-bottom: 8px;
            outline: none;
        }
        .editor-actions { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin-top: 4px; }
        .btn-save-key { background: #10b981; color: white; padding: 8px; font-size: 0.8rem; }
        .btn-test-key { background: #0284c7; color: white; padding: 8px; font-size: 0.8rem; }

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
        <h1>YD-ESP32-S3 Claw Controller</h1>
        <div class="sub-header">Wi-Fi &bull; BLE &bull; 16-Key Matrix Keyboard Engine</div>

        <!-- SERVO MOTOR / CLAW CONTROL -->
        <div class="section-box">
            <div class="section-title">
                <span>Claw Servo Position</span>
                <span style="color: var(--primary);">GPIO 4</span>
            </div>
            <div>
                <span class="readout" id="angleVal">90</span><span class="readout-unit">&deg;</span>
            </div>
            <div class="slider-wrap">
                <input type="range" id="angleSlider" min="0" max="180" value="90" oninput="onAngleSliderMove(this.value)" onchange="sendAngle(this.value)">
                <div class="range-labels">
                    <span>Closed (0&deg;)</span><span>45&deg;</span><span>90&deg;</span><span>135&deg;</span><span>Open (180&deg;)</span>
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

        <!-- 16-KEY MATRIX KEYBOARD CONFIGURATION -->
        <div class="section-box">
            <div class="section-title">
                <span>16 Matrix Keyboard (Claw Keypad)</span>
                <span style="color: #38bdf8;" id="keypadStatusBadge">Persistent (NVS)</span>
            </div>
            <div style="font-size: 0.72rem; color: var(--muted-text); text-align: left; margin-bottom: 6px;">
                Click any key below to reprogram its persistent flash action:
            </div>

            <div class="keypad-matrix" id="keypadGrid">
                <!-- 16 Keys will be dynamically rendered or bound -->
            </div>

            <!-- Key Editor Overlay -->
            <div class="editor-box" id="editorBox" style="display: none;">
                <div class="editor-header">
                    <span id="editorTitle">Edit Key 1 (R1C1)</span>
                    <span style="font-size: 0.7rem; color: #94a3b8; cursor: pointer;" onclick="closeEditor()">&times; Close</span>
                </div>

                <label style="font-size: 0.7rem; color: #94a3b8;">Preset Actions:</label>
                <select class="editor-select" id="presetSelect" onchange="onPresetSelected(this.value)">
                    <option value="">-- Choose a Preset Action --</option>
                    <option value="0">Claw: Close Completely (0&deg;)</option>
                    <option value="45">Claw: Quarter Open (45&deg;)</option>
                    <option value="90">Claw: Half Open (90&deg;)</option>
                    <option value="135">Claw: 3/4 Open (135&deg;)</option>
                    <option value="180">Claw: Open Completely (180&deg;)</option>
                    <option value="nudge -10">Claw: Nudge Close (-10&deg;)</option>
                    <option value="nudge +10">Claw: Nudge Open (+10&deg;)</option>
                    <option value="nudge -2">Claw: Fine Nudge Close (-2&deg;)</option>
                    <option value="nudge +2">Claw: Fine Nudge Open (+2&deg;)</option>
                    <option value="bark">Audio: Play Dog Bark</option>
                    <option value="beep">Audio: Play Diagnostic Tone</option>
                    <option value="stop">Audio: Stop Sound</option>
                    <option value="vol -10">Volume: Decrease (-10%)</option>
                    <option value="vol +10">Volume: Increase (+10%)</option>
                    <option value="vol 50">Volume: Set to 50%</option>
                    <option value="vol 80">Volume: Set to 80%</option>
                    <option value="vol 100">Volume: Maximum 100%</option>
                    <option value="status">System: Print Status</option>
                    <option value="reboot">System: Reboot Board</option>
                </select>

                <label style="font-size: 0.7rem; color: #94a3b8;">Assigned Command Text:</label>
                <input type="text" class="editor-input" id="customCmdInput" placeholder="e.g. angle 90, bark, nudge +5">

                <div class="editor-actions">
                    <button class="btn-save-key" onclick="saveActiveKey()">Save to Flash</button>
                    <button class="btn-test-key" onclick="testActiveKey()">Test Key Now</button>
                </div>
            </div>

            <div style="display: flex; justify-content: space-between; align-items: center; margin-top: 10px;">
                <span style="font-size: 0.68rem; color: #64748b;">Rows: GPIO 13,14,15,16 &bull; Cols: GPIO 17,18,21,38</span>
                <button style="background: none; border: 1px solid #475569; font-size: 0.68rem; padding: 4px 8px; border-radius: 6px; color: #cbd5e1;" onclick="resetKeypadDefaults()">Reset All</button>
            </div>
        </div>

        <!-- HT517 SPEAKER AUDIO CONTROL -->
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

            <div style="margin: 10px 0 6px 0; text-align: left; font-size: 0.75rem; color: var(--muted-text);">
                Hardware Pin Selection:
            </div>
            <div style="display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 6px; margin-bottom: 12px;">
                <button id="btnPinYD" class="btn-pin active" onclick="applyPins(10, 11, 12)">1: 10,11,12</button>
                <button id="btnPinSeeed" class="btn-pin" onclick="applyPins(8, 7, 9)">2: 8,7,9</button>
                <button id="btnPinSwap" class="btn-pin" onclick="applyPins(12, 11, 10)">3: 12,11,10</button>
            </div>

            <div class="audio-btn-grid">
                <button class="btn-bark" onclick="playBark()">&#128054; Dog Bark</button>
                <button class="btn-beep" onclick="playBeep()">&#128276; Test Tone</button>
                <button class="btn-stop" onclick="stopAudio()">&#9209; Stop Audio</button>
            </div>
        </div>

        <div class="status-bar">
            <span class="status-dot"></span><span id="connStatus">Ready &bull; 192.168.4.1</span>
        </div>
    </div>

    <script>
        let isAngleThrottled = false;
        let pendingAngle = null;
        let isVolThrottled = false;
        let pendingVol = null;
        let activeEditKey = null;
        let keypadMap = {};

        const KEY_COORDS = [
            "R1C1", "R1C2", "R1C3", "R1C4",
            "R2C1", "R2C2", "R2C3", "R2C4",
            "R3C1", "R3C2", "R3C3", "R3C4",
            "R4C1", "R4C2", "R4C3", "R4C4"
        ];

        function renderKeypadGrid() {
            const grid = document.getElementById("keypadGrid");
            grid.innerHTML = "";
            for (let i = 1; i <= 16; i++) {
                const coord = KEY_COORDS[i - 1];
                const cmd = keypadMap[i] || "";
                const btn = document.createElement("div");
                btn.className = "keypad-btn" + (activeEditKey === i ? " active-edit" : "");
                btn.id = "kbtn-" + i;
                btn.onclick = () => openKeyEditor(i);
                btn.innerHTML = `
                    <div class="key-num">Key ${i}</div>
                    <div class="key-coord">${coord}</div>
                    <div class="key-cmd" title="${cmd}">${cmd || "(empty)"}</div>
                `;
                grid.appendChild(btn);
            }
        }

        function openKeyEditor(keyNum) {
            activeEditKey = keyNum;
            renderKeypadGrid();
            const coord = KEY_COORDS[keyNum - 1];
            const currentCmd = keypadMap[keyNum] || "";
            document.getElementById("editorTitle").innerText = `Edit Key ${keyNum} (${coord})`;
            document.getElementById("customCmdInput").value = currentCmd;
            document.getElementById("presetSelect").value = "";
            document.getElementById("editorBox").style.display = "block";
        }

        function closeEditor() {
            activeEditKey = null;
            document.getElementById("editorBox").style.display = "none";
            renderKeypadGrid();
        }

        function onPresetSelected(val) {
            if (val) {
                document.getElementById("customCmdInput").value = val;
            }
        }

        function saveActiveKey() {
            if (!activeEditKey) return;
            const keyNum = activeEditKey;
            const newCmd = document.getElementById("customCmdInput").value.trim();

            fetch(`/keypad/set?key=${keyNum}&cmd=${encodeURIComponent(newCmd)}`)
                .then(r => r.text())
                .then(res => {
                    keypadMap[keyNum] = newCmd;
                    document.getElementById("connStatus").innerText = `Key ${keyNum} saved to flash: '${newCmd}'`;
                    closeEditor();
                })
                .catch(err => {
                    alert("Error saving key: " + err);
                });
        }

        function testActiveKey() {
            const cmd = document.getElementById("customCmdInput").value.trim();
            if (!cmd) return;
            fetch(`/keypad/test?cmd=${encodeURIComponent(cmd)}`);
        }

        function resetKeypadDefaults() {
            if (!confirm("Reset all 16 keys to factory default actions?")) return;
            fetch('/keypad/reset')
                .then(r => r.json())
                .then(data => {
                    keypadMap = data;
                    closeEditor();
                    document.getElementById("connStatus").innerText = "Keypad restored to defaults.";
                });
        }

        function loadKeypadConfig() {
            fetch('/keypad/get')
                .then(r => r.json())
                .then(data => {
                    keypadMap = data;
                    renderKeypadGrid();
                })
                .catch(e => {});
        }

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
            fetch('/set?angle=' + val)
                .then(r => r.text())
                .then(t => { document.getElementById("connStatus").innerText = "Servo Angle: " + val + "°"; })
                .catch(e => {});
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
                .catch(e => {});
        }

        function playBark() {
            fetch('/play?sound=dog_bark');
        }

        function playBeep() {
            fetch('/beep');
        }

        function stopAudio() {
            fetch('/stop');
        }

        function applyPins(b, l, d) {
            fetch(`/set_pins?bclk=${b}&lrc=${l}&din=${d}`)
                .then(r => r.text())
                .then(t => {
                    updatePinButtons(b);
                    setTimeout(playBeep, 150);
                });
        }

        function updatePinButtons(bclk) {
            document.getElementById("btnPinYD").classList.remove("active");
            document.getElementById("btnPinSeeed").classList.remove("active");
            document.getElementById("btnPinSwap").classList.remove("active");

            if (bclk === 8) {
                document.getElementById("btnPinSeeed").classList.add("active");
            } else if (bclk === 12) {
                document.getElementById("btnPinSwap").classList.add("active");
            } else {
                document.getElementById("btnPinYD").classList.add("active");
            }
        }

        window.onload = function() {
            fetch('/get')
                .then(r => r.json())
                .then(data => {
                    if (data.angle !== undefined) updateAngleDisplay(data.angle);
                    if (data.volume !== undefined) updateVolDisplay(data.volume);
                    if (data.bclk !== undefined) updatePinButtons(data.bclk);
                })
                .catch(err => {});
            loadKeypadConfig();
        };
    </script>
</body>
</html>
)rawliteral";

// =========================================================================
// URL DECODE HELPER
// =========================================================================
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

// =========================================================================
// HTTP SERVER HANDLERS & CAPTIVE REDIRECTORS
// =========================================================================
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
            notify_all_status();

            char resp[32];
            snprintf(resp, sizeof(resp), "OK %d", current_servo_angle);
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
    char json_resp[128];
    snprintf(json_resp, sizeof(json_resp),
             "{\"angle\":%d,\"volume\":%d,\"bclk\":%d,\"lrc\":%d,\"din\":%d}",
             current_servo_angle, audio_get_volume(), bclk, lrc, din);
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
            notify_all_status();

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
            notify_all_status();
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "PINS_UPDATED", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing pin params");
    return ESP_FAIL;
}

// Keypad API Handlers
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

static esp_err_t keypad_test_handler(httpd_req_t *req) {
    char query[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char cmd_raw[KEYPAD_CMD_MAX_LEN * 3] = {0};
        char cmd_decoded[KEYPAD_CMD_MAX_LEN] = {0};
        if (httpd_query_key_value(query, "cmd", cmd_raw, sizeof(cmd_raw)) == ESP_OK) {
            url_decode(cmd_decoded, cmd_raw, sizeof(cmd_decoded));
            process_unified_command(cmd_decoded, console_print);
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "TRIGGERED", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing cmd");
    return ESP_FAIL;
}

static esp_err_t keypad_reset_handler(httpd_req_t *req) {
    keypad_reset_defaults();
    return keypad_get_handler(req);
}

static void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 25;
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

        // Keypad APIs
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

        uri.uri = "/keypad/test";
        uri.method = HTTP_GET;
        uri.handler = keypad_test_handler;
        uri.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri);

        uri.uri = "/keypad/reset";
        uri.method = HTTP_GET;
        uri.handler = keypad_reset_handler;
        uri.user_ctx = NULL;
        httpd_register_uri_handler(server, &uri);

        // Captive Portal Redirectors
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

        ESP_LOGI(TAG, "HTTP Server active on port 80 with Captive Portal & Keypad Reprogrammer");
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

    // 1. Initialize Wi-Fi stack & Soft-AP
    wifi_init_softap();

    // 2. Start Captive DNS Server (UDP port 53)
    xTaskCreatePinnedToCore(dns_server_task, "dns_server_task", 4096, NULL, 5, NULL, 0);

    // 3. Initialize hardware PWM on GPIO 4 for servo
    servo_init();

    // 4. Initialize HT517 I2S audio driver & background tasks
    audio_player_init();

    // 5. Initialize 16-Key Matrix Keyboard (Rows: 13,14,15,16 | Cols: 17,18,21,38)
    keypad_init(on_keypad_command);

    // 6. Start HTTP web server with Captive Portal & Keypad Reconfiguration APIs
    start_webserver();

    // 7. Initialize Bluetooth Low Energy Nordic UART Service
    ble_server_init(on_ble_command);

    // 8. Start interactive serial console (USB Serial/JTAG + UART0)
    xTaskCreate(console_read_task, "console_read_task", 4096, NULL, 5, NULL);
}