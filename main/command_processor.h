#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*command_print_fn_t)(const char *str);

void command_processor_init(void);
void command_process(const char *cmd_line, command_print_fn_t print_fn);
void command_notify_status(void);

void command_set_auto_retract(bool enable);
bool command_get_auto_retract(void);

// Callbacks for peripheral events
void command_on_ble_received(const char *cmd, void (*reply_fn)(const char *reply));
void command_on_keypad_event(int key_index, const char *cmd, bool is_pressed);

#ifdef __cplusplus
}
#endif