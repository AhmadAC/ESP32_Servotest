// main/command_processor.cpp
#include "command_processor.h"
#include "servo_controller.h"
#include "audio_player.h"
#include "ble_server.h"
#include "keypad.h"
#include "console_serial.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "CMD_PROC";

static bool s_auto_retract_enabled = true;
static int s_active_open_key = -1;

void command_processor_init(void) {
    // Always boot with Auto-Retract enabled by default across power cycles
    s_auto_retract_enabled = true;

    nvs_handle_t handle;
    if (nvs_open("storage", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, "auto_retract", 1);
        nvs_commit(handle);
        nvs_close(handle);
    }

    ESP_LOGI(TAG, "Command Processor Initialized (Auto-Retract: %s by default, Claw Limit: %d-%d deg)",
             s_auto_retract_enabled ? "ON" : "OFF", SERVO_MIN_ANGLE, SERVO_MAX_ANGLE);
}

void command_set_auto_retract(bool enable) {
    s_auto_retract_enabled = enable;
    nvs_handle_t handle;
    if (nvs_open("storage", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, "auto_retract", enable ? 1 : 0);
        nvs_commit(handle);
        nvs_close(handle);
    }

    char ble_resp[32];
    snprintf(ble_resp, sizeof(ble_resp), "AUTORETRACT:%s\n", enable ? "ON" : "OFF");
    ble_server_send_notify(ble_resp);

    command_notify_status();
    ESP_LOGI(TAG, "Auto-Retract set to %s", enable ? "ON" : "OFF");
}

bool command_get_auto_retract(void) {
    return s_auto_retract_enabled;
}

void command_notify_status(void) {
    if (!ble_server_is_connected()) return;

    int bclk = 10, lrc = 11, din = 12;
    audio_get_pins(&bclk, &lrc, &din);

    char status_str[160];
    snprintf(status_str, sizeof(status_str),
             "STATUS:angle=%d,vol=%d,bclk=%d,lrc=%d,din=%d,retract=%d\n",
             servo_get_angle(), audio_get_volume(), bclk, lrc, din,
             s_auto_retract_enabled ? 1 : 0);
    ble_server_send_notify(status_str);
}

void command_process(const char *cmd_line, command_print_fn_t print_fn) {
    if (!cmd_line || !print_fn) return;

    const char *p = cmd_line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return;

    // Direct integer angle (e.g. "90")
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
            int actual = servo_get_angle();
            char msg[64];
            snprintf(msg, sizeof(msg), ">> Servo moved to %d deg\n", actual);
            print_fn(msg);
            command_notify_status();
        } else {
            char msg[80];
            snprintf(msg, sizeof(msg), ">> Error: Angle must be between %d and %d degrees.\n",
                     SERVO_MIN_ANGLE, SERVO_MAX_ANGLE);
            print_fn(msg);
        }
        return;
    }

    // Relative claw nudging: "nudge <+/-deg>" or "+5" or "-10"
    if (strncmp(p, "nudge ", 6) == 0 ||
        ((p[0] == '+' || p[0] == '-') && isdigit((unsigned char)p[1]))) {
        int delta = (strncmp(p, "nudge ", 6) == 0) ? atoi(p + 6) : atoi(p);
        int target = servo_get_angle() + delta;
        if (target < SERVO_MIN_ANGLE) target = SERVO_MIN_ANGLE;
        if (target > SERVO_MAX_ANGLE) target = SERVO_MAX_ANGLE;
        servo_set_angle(target);
        char msg[64];
        snprintf(msg, sizeof(msg), ">> Servo nudged by %+d -> %d deg\n", delta, servo_get_angle());
        print_fn(msg);
        command_notify_status();
        return;
    }

    // Auto-Retract Toggle Commands: "autoretract on", "autoretract off", "autoretract toggle"
    if (strncmp(p, "autoretract", 11) == 0) {
        const char *arg = p + 11;
        while (*arg == ' ') arg++;
        if (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0 || strcmp(arg, "true") == 0) {
            command_set_auto_retract(true);
            print_fn(">> Auto-Retract: ON\n");
            return;
        } else if (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0 || strcmp(arg, "false") == 0) {
            command_set_auto_retract(false);
            print_fn(">> Auto-Retract: OFF\n");
            return;
        } else if (strcmp(arg, "toggle") == 0) {
            command_set_auto_retract(!s_auto_retract_enabled);
            char msg[64];
            snprintf(msg, sizeof(msg), ">> Auto-Retract toggled to: %s\n",
                     s_auto_retract_enabled ? "ON" : "OFF");
            print_fn(msg);
            return;
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), ">> Auto-Retract currently: %s\n",
                     s_auto_retract_enabled ? "ON" : "OFF");
            print_fn(msg);
            return;
        }
    }

    // Reprogramming 16 Matrix Keyboard: "key <1-16> <command...>"
    if (strncmp(p, "key ", 4) == 0 || strncmp(p, "setkey ", 7) == 0) {
        const char *arg = (strncmp(p, "key ", 4) == 0) ? (p + 4) : (p + 7);
        while (*arg == ' ') arg++;
        char *endptr = NULL;
        long key_num = strtol(arg, &endptr, 10);
        if (key_num >= 1 && key_num <= KEYPAD_NUM_KEYS && endptr != NULL && *endptr != '\0') {
            while (*endptr == ' ' || *endptr == '\t') endptr++;
            if (*endptr != '\0') {
                keypad_set_key_command((int)key_num, endptr);

                char confirm[96];
                snprintf(confirm, sizeof(confirm), "KEY:%ld=%s\n", key_num, endptr);
                ble_server_send_notify(confirm);

                char msg[96];
                snprintf(msg, sizeof(msg), ">> Key %ld mapped to '%s' (NVS Flash)\n", key_num, endptr);
                print_fn(msg);
                return;
            }
        }
        print_fn(">> Usage: key <1-16> <command> (e.g. 'key 1 0' or 'key 8 bark')\n");
        return;
    }

    // Query all keys over BLE / Serial: "getkeys"
    if (strcmp(p, "getkeys") == 0 || strcmp(p, "keys") == 0) {
        for (int i = 1; i <= KEYPAD_NUM_KEYS; i++) {
            const char *kcmd = keypad_get_key_command(i);
            char line[96];
            snprintf(line, sizeof(line), "KEY:%d=%s\n", i, kcmd);
            ble_server_send_notify(line);
            print_fn(line);
            vTaskDelay(pdMS_TO_TICKS(15));
        }
        return;
    }

    // Reset keypad to factory defaults: "keyreset"
    if (strcmp(p, "keyreset") == 0) {
        keypad_reset_defaults();
        for (int i = 1; i <= KEYPAD_NUM_KEYS; i++) {
            const char *kcmd = keypad_get_key_command(i);
            char line[96];
            snprintf(line, sizeof(line), "KEY:%d=%s\n", i, kcmd);
            ble_server_send_notify(line);
            print_fn(line);
            vTaskDelay(pdMS_TO_TICKS(15));
        }
        print_fn(">> Keypad reset to defaults.\n");
        return;
    }

    if (strncmp(p, "angle ", 6) == 0) {
        int req_angle = atoi(p + 6);
        if (req_angle >= 0 && req_angle <= 180) {
            servo_set_angle(req_angle);
            char msg[64];
            snprintf(msg, sizeof(msg), ">> Servo moved to %d deg\n", servo_get_angle());
            print_fn(msg);
            command_notify_status();
        } else {
            char msg[80];
            snprintf(msg, sizeof(msg), ">> Error: Angle must be between %d and %d degrees.\n",
                     SERVO_MIN_ANGLE, SERVO_MAX_ANGLE);
            print_fn(msg);
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
        command_notify_status();
    } else if (strncmp(p, "pins ", 5) == 0) {
        int b = 0, l = 0, d = 0;
        if (sscanf(p + 5, "%d %d %d", &b, &l, &d) == 3) {
            audio_set_pins(b, l, d);
            char msg[64];
            snprintf(msg, sizeof(msg), ">> Audio pins updated: BCLK=%d, LRC=%d, DIN=%d\n", b, l, d);
            print_fn(msg);
            command_notify_status();
        } else {
            print_fn(">> Usage: pins <bclk> <lrc> <din> (e.g. 'pins 10 11 12')\n");
        }
    } else if (strcmp(p, "status") == 0) {
        int bclk = 10, lrc = 11, din = 12;
        audio_get_pins(&bclk, &lrc, &din);
        char msg[160];
        snprintf(msg, sizeof(msg),
                 ">> Servo: %d deg | Vol: %d%% | Auto-Retract: %s | Pins: %d,%d,%d | BLE: %s\n",
                 servo_get_angle(), audio_get_volume(),
                 s_auto_retract_enabled ? "ON" : "OFF",
                 bclk, lrc, din,
                 ble_server_is_connected() ? "Connected" : "Advertising");
        print_fn(msg);
        command_notify_status();
    } else if (strcmp(p, "help") == 0 || strcmp(p, "?") == 0) {
        print_fn("Available Commands:\n"
                 "  0 - 135          : Move servo angle directly (0-135 hard limit)\n"
                 "  angle <0-135>    : Move servo angle\n"
                 "  nudge <+/-deg>   : Nudge angle (e.g. 'nudge +10', '-5')\n"
                 "  autoretract <on/off/toggle> : Hold-to-open retract mode\n"
                 "  key <1-16> <cmd> : Reprogram key and save to NVS flash\n"
                 "  getkeys          : Read all 16 key actions over BLE\n"
                 "  keyreset         : Restore factory defaults on keypad\n"
                 "  bark / play      : Play dog bark sound\n"
                 "  beep / test      : Play diagnostic tone\n"
                 "  stop             : Stop speaker playback\n"
                 "  vol <0-100>      : Set volume (or 'vol +10', 'vol -10')\n"
                 "  pins 10 11 12    : Preset 1 (YD Default)\n"
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

void command_on_ble_received(const char *cmd, void (*reply_fn)(const char *reply)) {
    ESP_LOGI(TAG, "BLE Command: '%s'", cmd);
    command_process(cmd, reply_fn);
}

// Handles physical press and release events from the 16-key matrix keyboard
void command_on_keypad_event(int key_index, const char *cmd, bool is_pressed) {
    if (is_pressed) {
        char log_buf[80];
        snprintf(log_buf, sizeof(log_buf), "\n[Matrix Keypad Action] -> Pressed Key %d ('%s')\n", key_index, cmd);
        console_print(log_buf);

        // Check if this command opens the claw
        int target_angle = -1;
        const char *p = cmd;
        while (*p == ' ') p++;

        bool is_num = true;
        for (int i = 0; p[i] != '\0'; i++) {
            if (!isdigit((unsigned char)p[i])) { is_num = false; break; }
        }

        if (is_num) {
            target_angle = atoi(p);
        } else if (strncmp(p, "angle ", 6) == 0) {
            target_angle = atoi(p + 6);
        } else if (strncmp(p, "nudge +", 7) == 0 || (p[0] == '+' && isdigit((unsigned char)p[1]))) {
            target_angle = servo_get_angle() + atoi(p[0] == '+' ? p : p + 6);
        }

        if (target_angle > SERVO_MIN_ANGLE) {
            s_active_open_key = key_index;
        } else if (target_angle == SERVO_MIN_ANGLE) {
            s_active_open_key = -1;
        }

        // Execute the command: claw moves to commanded angle and holds as long as pressed
        command_process(cmd, console_print);

    } else {
        // BUTTON RELEASED:
        char log_buf[64];
        snprintf(log_buf, sizeof(log_buf), "\n[Matrix Keypad Action] -> Released Key %d\n", key_index);
        console_print(log_buf);

        if (s_auto_retract_enabled) {
            // If the key that opened/nudged the claw was let go, immediately snap back to 0°
            if (s_active_open_key == key_index || (s_active_open_key == -1 && servo_get_angle() > SERVO_MIN_ANGLE)) {
                s_active_open_key = -1;
                servo_set_angle(SERVO_MIN_ANGLE);
                command_notify_status();
                console_print(">> Auto-Retract triggered on release: Claw snapped to 0 deg\n");
                ble_server_send_notify("AUTORETRACT:RETRACTED\n");
            }
        }
    }
}