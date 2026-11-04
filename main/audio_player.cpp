#include "audio_player.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include <string.h>
#include <math.h>

static const char *TAG = "AUDIO";

// Configurable hardware pin mappings (Default: YD-ESP32-S3 GPIO 10, 11, 12)
static int32_t current_bclk_pin = 10;
static int32_t current_lrc_pin  = 11;
static int32_t current_din_pin  = 12;

// Pinout mapping for optional external PDM microphone
#define I2S_MIC_CLK_PIN GPIO_NUM_42
#define I2S_MIC_DAT_PIN GPIO_NUM_41

static i2s_chan_handle_t tx_chan = NULL;
static i2s_chan_handle_t rx_chan = NULL;
static int32_t current_volume = 80;
static QueueHandle_t audio_queue;
static volatile bool audio_stop_requested = false;

// Access the embedded WAV file created by CMake EMBED_FILES
extern const uint8_t dog_barking_wav_start[] asm("_binary_dog_barking_wav_start");
extern const uint8_t dog_barking_wav_end[]   asm("_binary_dog_barking_wav_end");

static void load_audio_nvs(void) {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        nvs_get_i32(my_handle, "aud_vol", &current_volume);
        nvs_get_i32(my_handle, "aud_bclk", &current_bclk_pin);
        nvs_get_i32(my_handle, "aud_lrc",  &current_lrc_pin);
        nvs_get_i32(my_handle, "aud_din",  &current_din_pin);
        nvs_close(my_handle);
    }
    if (current_volume < 10 || current_volume > 100) current_volume = 80;
    if (current_bclk_pin <= 0) current_bclk_pin = 10;
    if (current_lrc_pin <= 0)  current_lrc_pin = 11;
    if (current_din_pin <= 0)  current_din_pin = 12;
}

static void silence_and_disable_speaker(void) {
    if (tx_chan == NULL) return;
    int16_t silence_buf[256] = {0};
    size_t bytes_written = 0;
    for (int i = 0; i < 4; i++) {
        i2s_channel_write(tx_chan, silence_buf, sizeof(silence_buf), &bytes_written, pdMS_TO_TICKS(10));
    }
    i2s_channel_disable(tx_chan);
}

static void play_synth_beep(void) {
    if (tx_chan == NULL) return;
    ESP_LOGI(TAG, "Playing diagnostic test chime on BCLK:%d, LRC:%d, DIN:%d...",
             (int)current_bclk_pin, (int)current_lrc_pin, (int)current_din_pin);

    uint32_t sample_rate = 22050;
    i2s_channel_disable(tx_chan);

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);

    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    i2s_channel_reconfig_std_slot(tx_chan, &slot_cfg);

    i2s_channel_enable(tx_chan);

    float vol = ((float)current_volume / 100.0f) * 0.80f;
    const size_t CHUNK = 256;
    int16_t out_buf[CHUNK];
    size_t bytes_written;

    // Beep 1: 880 Hz for 150 ms
    int samples1 = (sample_rate * 150) / 1000;
    for (int i = 0; i < samples1 && !audio_stop_requested; i += CHUNK) {
        int count = (samples1 - i < (int)CHUNK) ? (samples1 - i) : (int)CHUNK;
        for (int j = 0; j < count; j++) {
            float t = (float)(i + j) / (float)sample_rate;
            out_buf[j] = (int16_t)(sinf(2.0f * (float)M_PI * 880.0f * t) * 20000.0f * vol);
        }
        i2s_channel_write(tx_chan, out_buf, count * 2, &bytes_written, portMAX_DELAY);
    }

    vTaskDelay(pdMS_TO_TICKS(40));

    // Beep 2: 1760 Hz for 180 ms
    int samples2 = (sample_rate * 180) / 1000;
    for (int i = 0; i < samples2 && !audio_stop_requested; i += CHUNK) {
        int count = (samples2 - i < (int)CHUNK) ? (samples2 - i) : (int)CHUNK;
        for (int j = 0; j < count; j++) {
            float t = (float)(i + j) / (float)sample_rate;
            out_buf[j] = (int16_t)(sinf(2.0f * (float)M_PI * 1760.0f * t) * 22000.0f * vol);
        }
        i2s_channel_write(tx_chan, out_buf, count * 2, &bytes_written, portMAX_DELAY);
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    silence_and_disable_speaker();
}

static void audio_stream_task(void *pvParameters) {
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(82);

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_sock, 3) != 0) {
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        struct sockaddr_storage source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        struct timeval timeout;
        timeout.tv_sec = 1; timeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        char rx_buffer[128];
        recv(sock, rx_buffer, sizeof(rx_buffer), 0);

        timeout.tv_sec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        const char *HEADER = "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
        send(sock, HEADER, strlen(HEADER), 0);

        uint32_t sample_rate = 16000;
        uint16_t bit_depth = 16;
        uint16_t channels = 1;
        uint32_t byte_rate = sample_rate * channels * (bit_depth / 8);
        uint16_t block_align = channels * (bit_depth / 8);

        uint8_t wav_header[44] = {
            'R', 'I', 'F', 'F',
            0xFF, 0xFF, 0xFF, 0x7F,
            'W', 'A', 'V', 'E',
            'f', 'm', 't', ' ',
            16, 0, 0, 0,
            1, 0,
            (uint8_t)channels, 0,
            (uint8_t)(sample_rate & 0xFF), (uint8_t)((sample_rate >> 8) & 0xFF), (uint8_t)((sample_rate >> 16) & 0xFF), (uint8_t)((sample_rate >> 24) & 0xFF),
            (uint8_t)(byte_rate & 0xFF), (uint8_t)((byte_rate >> 8) & 0xFF), (uint8_t)((byte_rate >> 16) & 0xFF), (uint8_t)((byte_rate >> 24) & 0xFF),
            (uint8_t)block_align, 0,
            (uint8_t)bit_depth, 0,
            'd', 'a', 't', 'a',
            0xFF, 0xFF, 0xFF, 0x7F
        };

        send(sock, wav_header, 44, 0);

        uint8_t buf[1024];
        while (1) {
            size_t bytes_read = 0;
            esp_err_t err = rx_chan ? i2s_channel_read(rx_chan, buf, sizeof(buf), &bytes_read, pdMS_TO_TICKS(100)) : ESP_FAIL;

            if (err == ESP_OK && bytes_read > 0) {
                int16_t *pcm = (int16_t *)buf;
                size_t samples = bytes_read / 2;
                const float STREAM_GAIN = 10.0f;

                for (size_t i = 0; i < samples; i++) {
                    int32_t val = (int32_t)(pcm[i] * STREAM_GAIN);
                    if (val > 32767) val = 32767;
                    if (val < -32768) val = -32768;
                    pcm[i] = (int16_t)val;
                }

                int sent = send(sock, buf, bytes_read, 0);
                if (sent < 0) break;
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        close(sock);
    }
}

static void audio_task(void *pvParameter) {
    char sound_req[32];

    while (1) {
        if (xQueueReceive(audio_queue, &sound_req, portMAX_DELAY)) {
            ESP_LOGI(TAG, "Audio task command: '%s' (Volume: %ld%%)", sound_req, (long)current_volume);

            if (strcmp(sound_req, "test_beep") == 0 || strcmp(sound_req, "beep") == 0) {
                audio_stop_requested = false;
                play_synth_beep();
                continue;
            }

            // -------------------------------------------------------------
            // PLAY STATIC WAV FILE
            // -------------------------------------------------------------
            const uint8_t *wav_start = dog_barking_wav_start;
            const uint8_t *wav_end   = dog_barking_wav_end;
            size_t wav_len = (wav_end > wav_start) ? (size_t)(wav_end - wav_start) : 0;

            if (wav_len < 44) {
                ESP_LOGW(TAG, "Embedded WAV empty. Playing diagnostic chime fallback.");
                audio_stop_requested = false;
                play_synth_beep();
                continue;
            }

            uint32_t sample_rate = 44100;
            uint16_t channels = 2;
            uint16_t bit_depth = 16;
            const uint8_t *audio_data = NULL;
            uint32_t data_size = 0;

            if (strncmp((const char *)wav_start, "RIFF", 4) == 0) {
                size_t offset = 12;
                while (offset + 8 <= wav_len) {
                    char chunk_id[5] = {0};
                    memcpy(chunk_id, wav_start + offset, 4);
                    uint32_t chunk_size = 0;
                    memcpy(&chunk_size, wav_start + offset + 4, 4);
                    offset += 8;

                    if (offset + chunk_size > wav_len) {
                        chunk_size = wav_len - offset;
                    }

                    if (strcmp(chunk_id, "fmt ") == 0 && chunk_size >= 16) {
                        memcpy(&channels,    wav_start + offset + 2, 2);
                        memcpy(&sample_rate, wav_start + offset + 4, 4);
                        memcpy(&bit_depth,   wav_start + offset + 14, 2);
                    } else if (strcmp(chunk_id, "data") == 0) {
                        audio_data = wav_start + offset;
                        data_size = chunk_size;
                        break;
                    }
                    offset += (chunk_size + 1) & ~1;
                }
            }

            if (audio_data == NULL) {
                for (size_t i = 12; i + 8 <= wav_len; i++) {
                    if (memcmp(wav_start + i, "data", 4) == 0) {
                        memcpy(&data_size, wav_start + i + 4, 4);
                        audio_data = wav_start + i + 8;
                        if (audio_data + data_size > wav_end) {
                            data_size = wav_end - audio_data;
                        }
                        break;
                    }
                }
            }

            if (audio_data == NULL) {
                audio_data = wav_start + 44;
                data_size = wav_len - 44;
                sample_rate = 44100;
                channels = 2;
                bit_depth = 16;
            }

            ESP_LOGI(TAG, "Playing WAV on BCLK:%d, LRC:%d, DIN:%d (%lu Hz, %u ch, %u bits, %lu bytes)",
                     (int)current_bclk_pin, (int)current_lrc_pin, (int)current_din_pin,
                     (unsigned long)sample_rate, (unsigned int)channels, (unsigned int)bit_depth, (unsigned long)data_size);

            i2s_channel_disable(tx_chan);

            i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
            i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);

            i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_16BIT,
                channels == 2 ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO
            );
            i2s_channel_reconfig_std_slot(tx_chan, &slot_cfg);

            i2s_channel_enable(tx_chan);

            const float HARDWARE_POWER_LIMIT = 0.80f;
            float vol_mult = ((float)current_volume / 100.0f) * HARDWARE_POWER_LIMIT;

            const size_t CHUNK_SAMPLES = 512;
            int16_t out_buffer[CHUNK_SAMPLES];
            size_t bytes_written;
            size_t num_samples = data_size / 2;

            audio_stop_requested = false;
            for (size_t i = 0; i < num_samples; i += CHUNK_SAMPLES) {
                if (audio_stop_requested) break;
                size_t chunk_size = (num_samples - i < CHUNK_SAMPLES) ? (num_samples - i) : CHUNK_SAMPLES;
                for (size_t j = 0; j < chunk_size; j++) {
                    int16_t raw_val = 0;
                    memcpy(&raw_val, audio_data + (i + j) * 2, 2);
                    out_buffer[j] = (int16_t)(raw_val * vol_mult);
                }
                esp_err_t err = i2s_channel_write(tx_chan, out_buffer, chunk_size * 2, &bytes_written, portMAX_DELAY);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "I2S Write Failed: %s", esp_err_to_name(err));
                    break;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(100));
            silence_and_disable_speaker();
        }
    }
}

void audio_set_pins(int bclk, int lrc, int din) {
    current_bclk_pin = (int32_t)bclk;
    current_lrc_pin  = (int32_t)lrc;
    current_din_pin  = (int32_t)din;

    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_i32(my_handle, "aud_bclk", current_bclk_pin);
        nvs_set_i32(my_handle, "aud_lrc",  current_lrc_pin);
        nvs_set_i32(my_handle, "aud_din",  current_din_pin);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }

    if (tx_chan != NULL) {
        i2s_channel_disable(tx_chan);
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)current_bclk_pin,
            .ws   = (gpio_num_t)current_lrc_pin,
            .dout = (gpio_num_t)current_din_pin,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false, .bclk_inv = false, .ws_inv = false
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    i2s_channel_disable(tx_chan);

    ESP_LOGI(TAG, "Audio pins updated: BCLK=%d, LRC=%d, DIN=%d", (int)bclk, (int)lrc, (int)din);
}

void audio_get_pins(int *bclk, int *lrc, int *din) {
    if (bclk) *bclk = (int)current_bclk_pin;
    if (lrc)  *lrc  = (int)current_lrc_pin;
    if (din)  *din  = (int)current_din_pin;
}

void audio_player_init(void) {
    load_audio_nvs();
    audio_queue = xQueueCreate(5, sizeof(char) * 32);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)current_bclk_pin,
            .ws   = (gpio_num_t)current_lrc_pin,
            .dout = (gpio_num_t)current_din_pin,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false, .bclk_inv = false, .ws_inv = false
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    i2s_channel_disable(tx_chan);

    // Optional PDM RX channel for microphone on Controller 1
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    if (i2s_new_channel(&rx_chan_cfg, NULL, &rx_chan) == ESP_OK) {
        i2s_pdm_rx_config_t pdm_rx_cfg = {
            .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(16000),
            .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .clk = I2S_MIC_CLK_PIN,
                .din = I2S_MIC_DAT_PIN,
                .invert_flags = {
                    .clk_inv = false,
                },
            },
        };
        pdm_rx_cfg.slot_cfg.slot_mask = I2S_PDM_SLOT_LEFT;
        if (i2s_channel_init_pdm_rx_mode(rx_chan, &pdm_rx_cfg) == ESP_OK) {
            i2s_channel_enable(rx_chan);
        } else {
            i2s_del_channel(rx_chan);
            rx_chan = NULL;
        }
    }

    xTaskCreatePinnedToCore(audio_task, "audio_task", 4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(audio_stream_task, "audio_stream_task", 4096, NULL, 3, NULL, 1);

    ESP_LOGI(TAG, "Audio Driver Initialized (BCLK=%d, LRC=%d, DIN=%d)",
             (int)current_bclk_pin, (int)current_lrc_pin, (int)current_din_pin);
}

void audio_set_volume(int volume_pct) {
    if (volume_pct < 0) volume_pct = 0;
    if (volume_pct > 100) volume_pct = 100;
    current_volume = (int32_t)volume_pct;

    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_i32(my_handle, "aud_vol", current_volume);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

int audio_get_volume(void) {
    return (int)current_volume;
}

void audio_play(const char *sound_name) {
    char req[32];
    strncpy(req, sound_name, sizeof(req) - 1);
    req[sizeof(req) - 1] = '\0';
    xQueueSend(audio_queue, &req, 0);
}

void audio_play_test_beep(void) {
    audio_play("test_beep");
}

void audio_stop(void) {
    audio_stop_requested = true;
    xQueueReset(audio_queue);
    silence_and_disable_speaker();
}