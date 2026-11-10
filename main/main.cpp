#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "servo_controller.h"
#include "audio_player.h"
#include "keypad.h"
#include "ble_server.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "console_serial.h"
#include "command_processor.h"

static const char *TAG = "APP_MAIN";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Booting YD-ESP32-S3 Claw Controller Subsystems...");

    // 1. Flash Storage (NVS)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Wi-Fi Soft-AP & Captive DNS (UDP 53)
    wifi_manager_init();

    // 3. Servo Actuator (LEDC PWM GPIO 4)
    servo_controller_init();

    // 4. Audio Engine (I2S Speaker & PDM Mic)
    audio_player_init();

    // 5. 16-Matrix Keyboard Scanner (Rows: 13,14,15,16 | Cols: 17,18,21,38)
    keypad_init(command_on_keypad_received);

    // 6. HTTP Web Server & Dashboard
    web_server_init();

    // 7. Bluetooth Low Energy NUS Engine
    ble_server_init(command_on_ble_received);

    // 8. Serial Console (UART0 & Native USB-Serial-JTAG REPL)
    console_serial_init();

    ESP_LOGI(TAG, "All subsystems running. System ready for commands.");
}