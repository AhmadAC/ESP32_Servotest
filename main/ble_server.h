#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ble_cmd_callback_t)(const char *cmd, void (*reply_fn)(const char *reply));

void ble_server_init(ble_cmd_callback_t cmd_handler);
bool ble_server_is_connected(void);
void ble_server_send_notify(const char *message);

#ifdef __cplusplus
}
#endif