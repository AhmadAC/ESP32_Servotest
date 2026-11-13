#include "joystick.h"
#include "servo_controller.h"
#include "command_processor.h"
#include "audio_player.h"
#include "console_serial.h"
#include "ble_server.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "JOYSTICK";

static adc_oneshot_unit_handle_t adc1_handle = NULL;

#define ADC_VRX_CHANNEL     ADC_CHANNEL_0  // GPIO 1
#define ADC_VRY_CHANNEL     ADC_CHANNEL_1  // GPIO 2

#define DEADZONE_MARGIN     350
#define ADC_MAX_VAL         4095

static const gpio_num_t BUTTON_PINS[] = {
    JOYSTICK_SW_PIN,
    JOYSTICK_BTN1_PIN,
    JOYSTICK_BTN2_PIN,
    JOYSTICK_BTN3_PIN,
    JOYSTICK_BTN4_PIN
};

#define NUM_BUTTONS (sizeof(BUTTON_PINS) / sizeof(BUTTON_PINS[0]))

static void joystick_poll_task(void *pvParameters) {
    // 1. Initial ADC Calibration / Baseline determination
    int center_x = 2048;
    int center_y = 2048;

    long accum_x = 0;
    long accum_y = 0;
    const int CAL_SAMPLES = 20;

    for (int i = 0; i < CAL_SAMPLES; i++) {
        int rx = 2048, ry = 2048;
        adc_oneshot_read(adc1_handle, ADC_VRX_CHANNEL, &rx);
        adc_oneshot_read(adc1_handle, ADC_VRY_CHANNEL, &ry);
        accum_x += rx;
        accum_y += ry;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    center_x = accum_x / CAL_SAMPLES;
    center_y = accum_y / CAL_SAMPLES;
    ESP_LOGI(TAG, "PS2 Joystick Calibrated Center -> X: %d, Y: %d", center_x, center_y);

    bool was_deflected = false;
    int last_applied_angle = servo_get_angle();

    // Button debounce tracking
    uint8_t press_count[NUM_BUTTONS] = {0};
    uint8_t release_count[NUM_BUTTONS] = {0};
    bool btn_state[NUM_BUTTONS] = {false};
    const uint8_t DEBOUNCE_THRESHOLD = 3;

    while (1) {
        // --- 1. Read Analog Joystick Axes ---
        int raw_x = 0;
        int raw_y = 0;
        adc_oneshot_read(adc1_handle, ADC_VRX_CHANNEL, &raw_x);
        adc_oneshot_read(adc1_handle, ADC_VRY_CHANNEL, &raw_y);

        int delta_x = raw_x - center_x;
        int delta_y = raw_y - center_y;

        bool active_x = abs(delta_x) > DEADZONE_MARGIN;
        bool active_y = abs(delta_y) > DEADZONE_MARGIN;

        if (active_x || active_y) {
            was_deflected = true;

            // Map deflection directly across 0 to 180 degrees
            int target_angle = 90;
            if (active_x) {
                if (delta_x > 0) {
                    float norm = (float)(delta_x - DEADZONE_MARGIN) / (float)(ADC_MAX_VAL - center_x - DEADZONE_MARGIN);
                    if (norm > 1.0f) norm = 1.0f;
                    target_angle = 90 + (int)(norm * 90.0f);
                } else {
                    float norm = (float)(-delta_x - DEADZONE_MARGIN) / (float)(center_x - DEADZONE_MARGIN);
                    if (norm > 1.0f) norm = 1.0f;
                    target_angle = 90 - (int)(norm * 90.0f);
                }
            } else if (active_y) {
                // Secondary axis: nudges or sets upper/lower range
                if (delta_y > 0) {
                    float norm = (float)(delta_y - DEADZONE_MARGIN) / (float)(ADC_MAX_VAL - center_y - DEADZONE_MARGIN);
                    target_angle = (int)(norm * 180.0f);
                } else {
                    target_angle = 0;
                }
            }

            if (target_angle < 0) target_angle = 0;
            if (target_angle > 180) target_angle = 180;

            if (abs(target_angle - last_applied_angle) >= 2) {
                last_applied_angle = target_angle;
                servo_set_angle(target_angle);
                command_notify_status();
            }
        } else {
            // Joystick is resting at Center Deadzone
            if (was_deflected) {
                was_deflected = false;
                if (command_get_auto_retract()) {
                    servo_set_angle(0);
                    last_applied_angle = 0;
                    command_notify_status();
                    console_print(">> PS2 Joystick Centered -> Auto-Retracted to 0 deg\n");
                    ble_server_send_notify("AUTORETRACT:RETRACTED\n");
                }
            }
        }

        // --- 2. Read SW and Push Buttons ---
        for (size_t b = 0; b < NUM_BUTTONS; b++) {
            int level = gpio_get_level(BUTTON_PINS[b]);
            bool is_down = (level == 0); // Active LOW

            if (is_down) {
                release_count[b] = 0;
                if (press_count[b] < DEBOUNCE_THRESHOLD) {
                    press_count[b]++;
                    if (press_count[b] >= DEBOUNCE_THRESHOLD && !btn_state[b]) {
                        btn_state[b] = true;

                        if (BUTTON_PINS[b] == JOYSTICK_SW_PIN) {
                            // SW Click: Toggle Auto-Retract mode
                            bool current_mode = command_get_auto_retract();
                            command_set_auto_retract(!current_mode);
                            char log_msg[64];
                            snprintf(log_msg, sizeof(log_msg), ">> [Joystick SW] Auto-Retract Toggled: %s\n",
                                     !current_mode ? "ON" : "OFF");
                            console_print(log_msg);
                        } else if (BUTTON_PINS[b] == JOYSTICK_BTN1_PIN) {
                            // K1 (Red): Open Claw completely (180 deg)
                            console_print(">> [Joystick Button 1 / RED] -> Open Claw (180 deg)\n");
                            command_on_keypad_event(4, "180", true);
                        } else if (BUTTON_PINS[b] == JOYSTICK_BTN2_PIN) {
                            // K2 (Green): Set Claw to Center (90 deg)
                            console_print(">> [Joystick Button 2 / GREEN] -> Center Claw (90 deg)\n");
                            command_on_keypad_event(3, "90", true);
                        } else if (BUTTON_PINS[b] == JOYSTICK_BTN3_PIN) {
                            // K3 (Blue): Play Dog Bark Sound
                            console_print(">> [Joystick Button 3 / BLUE] -> Bark Sound\n");
                            command_process("bark", console_print);
                        } else if (BUTTON_PINS[b] == JOYSTICK_BTN4_PIN) {
                            // K4 (Yellow): Play Diagnostic Beep
                            console_print(">> [Joystick Button 4 / YELLOW] -> Diagnostic Beep\n");
                            command_process("beep", console_print);
                        }
                    }
                }
            } else {
                press_count[b] = 0;
                if (btn_state[b]) {
                    if (release_count[b] < DEBOUNCE_THRESHOLD) {
                        release_count[b]++;
                        if (release_count[b] >= DEBOUNCE_THRESHOLD) {
                            btn_state[b] = false;
                            release_count[b] = 0;

                            // Edge detection release: trigger auto-retract if hold-to-open button was released
                            if (BUTTON_PINS[b] == JOYSTICK_BTN1_PIN) {
                                command_on_keypad_event(4, "180", false);
                            } else if (BUTTON_PINS[b] == JOYSTICK_BTN2_PIN) {
                                command_on_keypad_event(3, "90", false);
                            }
                        }
                    }
                } else {
                    release_count[b] = 0;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void joystick_init(void) {
    // 1. Configure ADC1 for VRX (GPIO 1) and VRY (GPIO 2)
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
#if defined(ADC_ATTEN_DB_12)
        .atten = ADC_ATTEN_DB_12,
#else
        .atten = ADC_ATTEN_DB_11,
#endif
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_VRX_CHANNEL, &config));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_VRY_CHANNEL, &config));

    // 2. Configure SW and Buttons GPIOs as Inputs with Internal Pull-Up
    gpio_config_t btn_cfg = {};
    btn_cfg.mode = GPIO_MODE_INPUT;
    btn_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    btn_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    btn_cfg.intr_type = GPIO_INTR_DISABLE;

    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        btn_cfg.pin_bit_mask |= (1ULL << BUTTON_PINS[i]);
    }
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));

    // 3. Launch background polling task
    xTaskCreatePinnedToCore(joystick_poll_task, "joystick_task", 4096, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "PS2 Joystick & Buttons Subsystem initialized (VRX: GP1, VRY: GP2, SW: GP3, K1..K4: GP5..8)");
}