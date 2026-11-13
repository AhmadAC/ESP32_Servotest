// main/servo_controller.cpp
#include "servo_controller.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "SERVO";

#define LEDC_TIMER          LEDC_TIMER_0
#define LEDC_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL        LEDC_CHANNEL_0
#define LEDC_DUTY_RES       LEDC_TIMER_14_BIT
#define LEDC_FREQUENCY      50

static int current_servo_angle = SERVO_MIN_ANGLE;

static uint32_t angle_to_duty(int angle) {
    if (angle < SERVO_MIN_ANGLE) angle = SERVO_MIN_ANGLE;
    if (angle > SERVO_MAX_ANGLE) angle = SERVO_MAX_ANGLE;

    uint32_t pulse_us = SERVO_MIN_US + ((SERVO_MAX_US - SERVO_MIN_US) * angle) / 180;
    uint32_t duty = (pulse_us * ((1 << 14) - 1)) / 20000;
    return duty;
}

void servo_controller_init(void) {
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

    ESP_LOGI(TAG, "Servo hardware PWM ready on GPIO %d (Angle: %d deg, Limit: %d-%d deg)",
             SERVO_PIN, current_servo_angle, SERVO_MIN_ANGLE, SERVO_MAX_ANGLE);
}

void servo_set_angle(int angle) {
    if (angle < SERVO_MIN_ANGLE) angle = SERVO_MIN_ANGLE;
    if (angle > SERVO_MAX_ANGLE) angle = SERVO_MAX_ANGLE;

    current_servo_angle = angle;
    uint32_t duty = angle_to_duty(angle);

    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));

    ESP_LOGI(TAG, "Servo moved to %d deg (duty: %lu)", angle, (unsigned long)duty);
}

int servo_get_angle(void) {
    return current_servo_angle;
}