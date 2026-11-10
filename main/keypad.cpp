#include "keypad.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_rom_sys.h"

static const char *TAG = "KEYPAD";

// Row Pins (Outputs, Driven LOW sequentially)
static const gpio_num_t ROW_PINS[4] = {
    GPIO_NUM_13,
    GPIO_NUM_14,
    GPIO_NUM_15,
    GPIO_NUM_16
};

// Column Pins (Inputs, Internal Pull-Ups enabled)
static const gpio_num_t COL_PINS[4] = {
    GPIO_NUM_17,
    GPIO_NUM_18,
    GPIO_NUM_21,
    GPIO_NUM_38
};

// Default commands for Key 1 through Key 16 (C1R1 .. C4R4)
static const char *DEFAULT_COMMANDS[KEYPAD_NUM_KEYS] = {
    "0",          // Key 1  (R1C1): Full Close
    "45",         // Key 2  (R1C2): 45 degrees
    "90",         // Key 3  (R1C3): Half Open (Neutral)
    "180",        // Key 4  (R1C4): Full Open
    "nudge -10",  // Key 5  (R2C1): Nudge Close 10 deg
    "135",        // Key 6  (R2C2): 135 degrees
    "nudge +10",  // Key 7  (R2C3): Nudge Open 10 deg
    "bark",       // Key 8  (R2C4): Bark Sound
    "nudge -2",   // Key 9  (R3C1): Fine Nudge Close 2 deg
    "beep",       // Key 10 (R3C2): Diagnostic Tone
    "nudge +2",   // Key 11 (R3C3): Fine Nudge Open 2 deg
    "stop",       // Key 12 (R3C4): Stop Audio
    "vol -10",    // Key 13 (R4C1): Volume -10%
    "vol 50",     // Key 14 (R4C2): Volume 50%
    "vol +10",    // Key 15 (R4C3): Volume +10%
    "status"      // Key 16 (R4C4): Print Status
};

static char s_commands[KEYPAD_NUM_KEYS][KEYPAD_CMD_MAX_LEN];
static keypad_event_callback_t s_event_callback = NULL;

static void keypad_load_nvs(void) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("keypad_cfg", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed (%s), loading in-memory defaults.", esp_err_to_name(err));
        for (int i = 0; i < KEYPAD_NUM_KEYS; i++) {
            strncpy(s_commands[i], DEFAULT_COMMANDS[i], KEYPAD_CMD_MAX_LEN - 1);
            s_commands[i][KEYPAD_CMD_MAX_LEN - 1] = '\0';
        }
        return;
    }

    bool needs_commit = false;
    for (int i = 0; i < KEYPAD_NUM_KEYS; i++) {
        char nvs_key[8];
        snprintf(nvs_key, sizeof(nvs_key), "k%d", i + 1);

        size_t required_size = 0;
        err = nvs_get_str(my_handle, nvs_key, NULL, &required_size);
        if (err == ESP_OK && required_size > 0 && required_size <= KEYPAD_CMD_MAX_LEN) {
            nvs_get_str(my_handle, nvs_key, s_commands[i], &required_size);
        } else {
            strncpy(s_commands[i], DEFAULT_COMMANDS[i], KEYPAD_CMD_MAX_LEN - 1);
            s_commands[i][KEYPAD_CMD_MAX_LEN - 1] = '\0';
            nvs_set_str(my_handle, nvs_key, s_commands[i]);
            needs_commit = true;
        }
    }

    if (needs_commit) {
        nvs_commit(my_handle);
    }
    nvs_close(my_handle);
}

bool keypad_set_key_command(int key_index, const char *command) {
    if (key_index < 1 || key_index > KEYPAD_NUM_KEYS || command == NULL) {
        return false;
    }

    int idx = key_index - 1;
    strncpy(s_commands[idx], command, KEYPAD_CMD_MAX_LEN - 1);
    s_commands[idx][KEYPAD_CMD_MAX_LEN - 1] = '\0';

    nvs_handle_t my_handle;
    if (nvs_open("keypad_cfg", NVS_READWRITE, &my_handle) == ESP_OK) {
        char nvs_key[8];
        snprintf(nvs_key, sizeof(nvs_key), "k%d", key_index);
        nvs_set_str(my_handle, nvs_key, s_commands[idx]);
        nvs_commit(my_handle);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "Key %d mapped to '%s' (Saved to NVS)", key_index, s_commands[idx]);
        return true;
    }
    return false;
}

const char* keypad_get_key_command(int key_index) {
    if (key_index < 1 || key_index > KEYPAD_NUM_KEYS) {
        return "";
    }
    return s_commands[key_index - 1];
}

void keypad_reset_defaults(void) {
    nvs_handle_t my_handle;
    bool nvs_ok = (nvs_open("keypad_cfg", NVS_READWRITE, &my_handle) == ESP_OK);

    for (int i = 0; i < KEYPAD_NUM_KEYS; i++) {
        strncpy(s_commands[i], DEFAULT_COMMANDS[i], KEYPAD_CMD_MAX_LEN - 1);
        s_commands[i][KEYPAD_CMD_MAX_LEN - 1] = '\0';
        if (nvs_ok) {
            char nvs_key[8];
            snprintf(nvs_key, sizeof(nvs_key), "k%d", i + 1);
            nvs_set_str(my_handle, nvs_key, s_commands[i]);
        }
    }

    if (nvs_ok) {
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
    ESP_LOGI(TAG, "Keypad restored to factory default actions.");
}

static void keypad_scan_task(void *pvParameters) {
    uint8_t press_count[KEYPAD_NUM_KEYS] = {0};
    uint8_t release_count[KEYPAD_NUM_KEYS] = {0};
    bool key_active[KEYPAD_NUM_KEYS] = {false};
    const uint8_t DEBOUNCE_THRESHOLD = 2; // ~30ms debounce

    while (1) {
        for (int r = 0; r < 4; r++) {
            // Drive current row LOW
            gpio_set_level(ROW_PINS[r], 0);
            esp_rom_delay_us(15);

            for (int c = 0; c < 4; c++) {
                int key_idx = (r * 4) + c;
                int level = gpio_get_level(COL_PINS[c]);
                bool is_down = (level == 0); // Active LOW due to internal pull-up

                if (is_down) {
                    release_count[key_idx] = 0;
                    if (press_count[key_idx] < DEBOUNCE_THRESHOLD) {
                        press_count[key_idx]++;
                        if (press_count[key_idx] >= DEBOUNCE_THRESHOLD && !key_active[key_idx]) {
                            key_active[key_idx] = true;
                            ESP_LOGI(TAG, "Key %d Pressed -> '%s'", key_idx + 1, s_commands[key_idx]);
                            if (s_event_callback != NULL && strlen(s_commands[key_idx]) > 0) {
                                s_event_callback(key_idx + 1, s_commands[key_idx], true);
                            }
                        }
                    }
                } else {
                    press_count[key_idx] = 0;
                    if (key_active[key_idx]) {
                        if (release_count[key_idx] < DEBOUNCE_THRESHOLD) {
                            release_count[key_idx]++;
                            if (release_count[key_idx] >= DEBOUNCE_THRESHOLD) {
                                key_active[key_idx] = false;
                                release_count[key_idx] = 0;
                                ESP_LOGI(TAG, "Key %d Released -> '%s'", key_idx + 1, s_commands[key_idx]);
                                if (s_event_callback != NULL && strlen(s_commands[key_idx]) > 0) {
                                    s_event_callback(key_idx + 1, s_commands[key_idx], false);
                                }
                            }
                        }
                    } else {
                        release_count[key_idx] = 0;
                    }
                }
            }

            // Restore row HIGH
            gpio_set_level(ROW_PINS[r], 1);
        }

        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

void keypad_init(keypad_event_callback_t callback) {
    s_event_callback = callback;
    keypad_load_nvs();

    // 1. Configure Row pins as Outputs
    gpio_config_t row_cfg = {};
    row_cfg.mode = GPIO_MODE_OUTPUT;
    row_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    row_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    row_cfg.intr_type = GPIO_INTR_DISABLE;
    for (int i = 0; i < 4; i++) {
        row_cfg.pin_bit_mask |= (1ULL << ROW_PINS[i]);
    }
    ESP_ERROR_CHECK(gpio_config(&row_cfg));

    for (int i = 0; i < 4; i++) {
        gpio_set_level(ROW_PINS[i], 1);
    }

    // 2. Configure Column pins as Inputs with Pull-Up
    gpio_config_t col_cfg = {};
    col_cfg.mode = GPIO_MODE_INPUT;
    col_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    col_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    col_cfg.intr_type = GPIO_INTR_DISABLE;
    for (int i = 0; i < 4; i++) {
        col_cfg.pin_bit_mask |= (1ULL << COL_PINS[i]);
    }
    ESP_ERROR_CHECK(gpio_config(&col_cfg));

    // 3. Launch background matrix scanner task
    xTaskCreatePinnedToCore(keypad_scan_task, "keypad_scan_task", 3072, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "16-Matrix Keypad Driver Initialized (Rows: 13,14,15,16 | Cols: 17,18,21,38)");
}