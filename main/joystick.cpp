// main/joystick.cpp
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
#include <stdio.h>

static const char *TAG = "JOYSTICK";

static adc_oneshot_unit_handle_t adc1_handle = NULL;

#define ADC_VRX_CHANNEL     ADC_CHANNEL_0  // GPIO 1
#define ADC_VRY_CHANNEL     ADC_CHANNEL_1  // GPIO 2

#define ADC_EFF_MIN         100.0f         // Reliable low-end pot rail
#define ADC_EFF_MAX         3995.0f        // Reliable high-end pot rail (below 4095 saturation)
#define ADC_MIN_SPAN        200.0f         // Minimum span guard against division by zero
#define DEADZONE_NORM       0.14f          // ~14% radial center deadzone
#define EMA_ALPHA           0.45f          // Smoothing factor for jitter-free 1-deg precision
#define RESPONSE_GAMMA      1.35f          // Gentle curve for fine 1-deg control near small angles
#define ADC_AVG_SAMPLES     8              // Multi-sample read per poll tick

static const gpio_num_t BUTTON_PINS[] = {
    JOYSTICK_SW_PIN,
    JOYSTICK_BTN1_PIN,
    JOYSTICK_BTN2_PIN,
    JOYSTICK_BTN3_PIN,
    JOYSTICK_BTN4_PIN
};

#define NUM_BUTTONS (sizeof(BUTTON_PINS) / sizeof(BUTTON_PINS[0]))

static float read_adc_avg(adc_channel_t channel, int samples) {
    long sum = 0;
    int valid = 0;
    for (int i = 0; i < samples; i++) {
        int val = 0;
        if (adc_oneshot_read(adc1_handle, channel, &val) == ESP_OK) {
            sum += val;
            valid++;
        }
    }
    if (valid == 0) return 2048.0f;
    return (float)sum / (float)valid;
}

static float normalize_axis(float val, float center) {
    float delta = val - center;
    if (delta >= 0.0f) {
        float span = ADC_EFF_MAX - center;
        if (span < ADC_MIN_SPAN) span = ADC_MIN_SPAN;
        float n = delta / span;
        return (n > 1.0f) ? 1.0f : n;
    } else {
        float span = center - ADC_EFF_MIN;
        if (span < ADC_MIN_SPAN) span = ADC_MIN_SPAN;
        float n = delta / span;
        return (n < -1.0f) ? -1.0f : n;
    }
}

static void joystick_poll_task(void *pvParameters) {
    (void)pvParameters;

    // 1. Initial ADC Calibration / Center Baseline determination
    float accum_x = 0.0f;
    float accum_y = 0.0f;
    const int CAL_SAMPLES = 32;

    for (int i = 0; i < CAL_SAMPLES; i++) {
        accum_x += read_adc_avg(ADC_VRX_CHANNEL, 4);
        accum_y += read_adc_avg(ADC_VRY_CHANNEL, 4);
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    float center_x = accum_x / (float)CAL_SAMPLES;
    float center_y = accum_y / (float)CAL_SAMPLES;
    float filt_x = center_x;
    float filt_y = center_y;

    ESP_LOGI(TAG, "PS2 Joystick Calibrated Center -> X: %.1f, Y: %.1f (360-deg radial -> %d-%d deg)",
             center_x, center_y, SERVO_MIN_ANGLE, SERVO_MAX_ANGLE);

    bool was_deflected = false;
    bool prev_auto_retract = command_get_auto_retract();
    int last_applied_angle = servo_get_angle();
    bool pending_notify = false;
    TickType_t last_notify_tick = 0;

    // Button debounce tracking
    uint8_t press_count[NUM_BUTTONS] = {0};
    uint8_t release_count[NUM_BUTTONS] = {0};
    bool btn_state[NUM_BUTTONS] = {false};
    const uint8_t DEBOUNCE_THRESHOLD = 3;

    while (1) {
        // --- 1. Read & Filter Analog Joystick Axes (360-Degree Radial Control) ---
        float raw_x = read_adc_avg(ADC_VRX_CHANNEL, ADC_AVG_SAMPLES);
        float raw_y = read_adc_avg(ADC_VRY_CHANNEL, ADC_AVG_SAMPLES);

        filt_x += EMA_ALPHA * (raw_x - filt_x);
        filt_y += EMA_ALPHA * (raw_y - filt_y);

        float nx = normalize_axis(filt_x, center_x);
        float ny = normalize_axis(filt_y, center_y);

        // Radial magnitude in all 360 degrees around center
        float mag = sqrtf((nx * nx) + (ny * ny));
        if (mag > 1.0f) mag = 1.0f;

        bool cur_auto_retract = command_get_auto_retract();
        bool in_deflection = (mag > DEADZONE_NORM);

        // If Auto-Retract was just switched ON while joystick rests at center, retract immediately
        if (!prev_auto_retract && cur_auto_retract) {
            if (!in_deflection && servo_get_angle() > SERVO_MIN_ANGLE) {
                servo_set_angle(SERVO_MIN_ANGLE);
                last_applied_angle = SERVO_MIN_ANGLE;
                pending_notify = false;
                command_notify_status();
                last_notify_tick = xTaskGetTickCount();
                console_print(">> Auto-Retract Enabled -> Claw Retracted to 0 deg\n");
                ble_server_send_notify("AUTORETRACT:RETRACTED\n");
            }
        }
        prev_auto_retract = cur_auto_retract;

        if (in_deflection) {
            if (!was_deflected) {
                was_deflected = true;
                last_applied_angle = servo_get_angle();
            }

            float effective = (mag - DEADZONE_NORM) / (1.0f - DEADZONE_NORM);
            if (effective < 0.0f) effective = 0.0f;
            if (effective > 1.0f) effective = 1.0f;

            // Gentle curve provides fine 1-degree thumb control at small/mid openings
            float curved = powf(effective, RESPONSE_GAMMA);
            int target_angle = (int)lroundf(curved * (float)(SERVO_MAX_ANGLE - SERVO_MIN_ANGLE)) + SERVO_MIN_ANGLE;

            if (target_angle < SERVO_MIN_ANGLE) target_angle = SERVO_MIN_ANGLE;
            if (target_angle > SERVO_MAX_ANGLE) target_angle = SERVO_MAX_ANGLE;

            if (target_angle != last_applied_angle) {
                last_applied_angle = target_angle;
                servo_set_angle(target_angle);
                pending_notify = true;
            }

            if (pending_notify && (xTaskGetTickCount() - last_notify_tick) >= pdMS_TO_TICKS(60)) {
                command_notify_status();
                last_notify_tick = xTaskGetTickCount();
                pending_notify = false;
            }
        } else {
            // Joystick is resting inside Center Deadzone
            if (was_deflected) {
                was_deflected = false;
                if (cur_auto_retract) {
                    servo_set_angle(SERVO_MIN_ANGLE);
                    last_applied_angle = SERVO_MIN_ANGLE;
                    pending_notify = false;
                    command_notify_status();
                    last_notify_tick = xTaskGetTickCount();
                    console_print(">> PS2 Joystick Centered -> Auto-Retracted to 0 deg\n");
                    ble_server_send_notify("AUTORETRACT:RETRACTED\n");
                } else if (pending_notify) {
                    command_notify_status();
                    last_notify_tick = xTaskGetTickCount();
                    pending_notify = false;
                }
            } else if (pending_notify) {
                command_notify_status();
                last_notify_tick = xTaskGetTickCount();
                pending_notify = false;
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
                            // K1 (Red): Open Claw to hard limit (135 deg)
                            console_print(">> [Joystick Button 1 / RED] -> Open Claw (135 deg limit)\n");
                            command_on_keypad_event(4, "135", true);
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
                                command_on_keypad_event(4, "135", false);
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
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
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