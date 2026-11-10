#include "console_serial.h"
#include "command_processor.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

void console_print(const char *str) {
    if (!str) return;
    size_t len = strlen(str);
    uart_write_bytes(UART_NUM_0, str, len);
    usb_serial_jtag_write_bytes(str, len, 0);
}

static void console_read_task(void *pvParameter) {
    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate = 115200;
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity    = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    uart_param_config(UART_NUM_0, &uart_cfg);
    uart_driver_install(UART_NUM_0, 512, 512, 0, NULL, 0);

    usb_serial_jtag_driver_config_t usbjtag_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&usbjtag_cfg);

    vTaskDelay(pdMS_TO_TICKS(400));

    const char *banner =
        "\n==================================================\n"
        "   YD-ESP32-S3 Interactive Console Ready\n"
        "   Control Modes: Wi-Fi, BLE, 4x4 Keypad & USB UART\n"
        "==================================================\n"
        "Commands: '0-180', 'nudge <+/-deg>', 'key <1-16> <cmd>',\n"
        "          'getkeys', 'bark', 'beep', 'vol <0-100>', 'help'\n"
        "==================================================\n"
        "REPL> ";
    console_print(banner);

    char cmd_buf[128];
    int cmd_idx = 0;

    while (1) {
        uint8_t c = 0;
        bool has_char = false;

        if (uart_read_bytes(UART_NUM_0, &c, 1, 0) > 0) {
            has_char = true;
        } else if (usb_serial_jtag_read_bytes(&c, 1, 0) > 0) {
            has_char = true;
        }

        if (has_char) {
            if (c == 0x03) {
                console_print("\n[Line Cleared]\nREPL> ");
                cmd_idx = 0;
                continue;
            }

            if (c == '\r' || c == '\n') {
                if (cmd_idx > 0) {
                    cmd_buf[cmd_idx] = '\0';
                    console_print("\n");
                    command_process(cmd_buf, console_print);
                    cmd_idx = 0;
                    console_print("REPL> ");
                } else {
                    console_print("REPL> ");
                }
            } else if (c == 0x08 || c == 0x7F) {
                if (cmd_idx > 0) {
                    cmd_idx--;
                    console_print("\b \b");
                }
            } else if (c >= 0x20 && c <= 0x7E) {
                if (cmd_idx < (int)sizeof(cmd_buf) - 1) {
                    cmd_buf[cmd_idx++] = (char)c;
                    char echo_str[2] = {(char)c, '\0'};
                    console_print(echo_str);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

void console_serial_init(void) {
    xTaskCreate(console_read_task, "console_read_task", 4096, NULL, 5, NULL);
}