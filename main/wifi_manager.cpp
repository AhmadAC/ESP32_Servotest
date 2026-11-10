#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "WIFI_MGR";

#pragma pack(push, 1)
typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;
#pragma pack(pop)

static void dns_server_task(void *pvParameters) {
    uint8_t rx_buffer[512];
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(53);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Captive DNS Server running on UDP port 53");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&client_addr, &addr_len);
        if (len < (int)sizeof(dns_header_t)) {
            continue;
        }

        dns_header_t *dns_hdr = (dns_header_t *)rx_buffer;
        if ((ntohs(dns_hdr->flags) & 0x8000) != 0) {
            continue;
        }

        int qname_pos = sizeof(dns_header_t);
        while (qname_pos < len && rx_buffer[qname_pos] != 0) {
            qname_pos += rx_buffer[qname_pos] + 1;
        }
        qname_pos++;
        int qend_pos = qname_pos + 4;

        if (qend_pos > len || qend_pos + 16 > (int)sizeof(rx_buffer)) {
            continue;
        }

        dns_hdr->flags    = htons(0x8180);
        dns_hdr->qd_count = htons(1);
        dns_hdr->an_count = htons(1);
        dns_hdr->ns_count = 0;
        dns_hdr->ar_count = 0;

        uint8_t *ans = rx_buffer + qend_pos;
        ans[0] = 0xC0;
        ans[1] = 0x0C;
        ans[2] = 0x00;
        ans[3] = 0x01;
        ans[4] = 0x00;
        ans[5] = 0x01;
        ans[6] = 0x00;
        ans[7] = 0x00;
        ans[8] = 0x00;
        ans[9] = 0x3C;
        ans[10] = 0x00;
        ans[11] = 0x04;
        ans[12] = 192;
        ans[13] = 168;
        ans[14] = 4;
        ans[15] = 1;

        int resp_len = qend_pos + 16;
        sendto(sock, rx_buffer, resp_len, 0, (struct sockaddr *)&client_addr, addr_len);
    }
}

void wifi_manager_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.ap.ssid, "ESP32S3-Servo");
    wifi_config.ap.ssid_len = strlen("ESP32S3-Servo");
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    wifi_config.ap.beacon_interval = 100;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    xTaskCreatePinnedToCore(dns_server_task, "dns_server_task", 4096, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "Wi-Fi SoftAP active. SSID: 'ESP32S3-Servo' (192.168.4.1)");
}