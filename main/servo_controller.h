#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SERVO_PIN           GPIO_NUM_4
#define SERVO_MIN_US        500
#define SERVO_MAX_US        2500

void servo_controller_init(void);
void servo_set_angle(int angle);
int servo_get_angle(void);

#ifdef __cplusplus
}
#endif