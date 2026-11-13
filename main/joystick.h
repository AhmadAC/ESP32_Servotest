#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Hardware pin definitions for PS2 Joystick & 4-Button breakout
#define JOYSTICK_ADC_VRX_PIN    GPIO_NUM_1   // ADC1 Channel 0
#define JOYSTICK_ADC_VRY_PIN    GPIO_NUM_2   // ADC1 Channel 1
#define JOYSTICK_SW_PIN         GPIO_NUM_3   // Joystick Push Button (Active LOW)
#define JOYSTICK_BTN1_PIN       GPIO_NUM_5   // Button 1 (Red / Key 1)
#define JOYSTICK_BTN2_PIN       GPIO_NUM_6   // Button 2 (Green / Key 2)
#define JOYSTICK_BTN3_PIN       GPIO_NUM_7   // Button 3 (Blue / Key 3)
#define JOYSTICK_BTN4_PIN       GPIO_NUM_8   // Button 4 (Yellow / Key 4)

void joystick_init(void);

#ifdef __cplusplus
}
#endif