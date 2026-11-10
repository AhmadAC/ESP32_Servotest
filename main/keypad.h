#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KEYPAD_NUM_KEYS 16
#define KEYPAD_CMD_MAX_LEN 48

typedef void (*keypad_event_callback_t)(int key_index, const char *cmd, bool is_pressed);

void keypad_init(keypad_event_callback_t callback);
bool keypad_set_key_command(int key_index, const char *command);
const char* keypad_get_key_command(int key_index);
void keypad_reset_defaults(void);

#ifdef __cplusplus
}
#endif