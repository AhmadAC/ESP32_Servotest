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

static const char *TAG = "AUDIO";

// HT517 I2S Amplifier Pinout Mapping:
// GPIO 10 -> HT517 pin D10 (BCLK / SCK)
// GPIO 11 -> HT517 pin D11 (LRC / WS)
// GPIO 12 -> HT517 pin D12 (DIN / SD)
#define I2S_BCLK_PIN GPIO_NUM_10
#define I2S_LRC_PIN  GPIO_NUM_11
#define I2S_DIN_PIN  GPIO_NUM_12

// Pinout mapping for optional external PDM microphone
#define I2S_MIC_CLK_PIN GPIO_NUM_42
#define I2S_MIC_DAT_PIN GPIO_NUM_41

static i2s_chan_handle_t tx_chan = NULL;
static i2s_chan_handle_t rx_chan = NULL;
static bool is_tx_enabled = false;
static uint32_t active_sample_rate = 44100;
static int32_t current_volume = 80; // 0-100 Default volume
static QueueHandle_t audio_queue;
static volatile bool audio_stop_requested = false;

// Access the embedded WAV file created by CMake EMBED_FILES
extern const uint8_t dog_barking_wav_start[] asm("_binary_dog_barking_wav_start");
extern const uint8_t dog_barking_wav_end[]   asm("_binary_dog_barking_wav_end");

static void load_audio_nvs(void) {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        nvs_get_i32(my_handle, "aud_vol", &current_volume);
        nvs_close(my_handle);
    }
    if (current_volume < 0) current_volume = 0;
    if (current_volume > 100) current_volume = 100;
}

static void ensure_speaker_clock(uint32_t sample_rate) {
    if (tx_chan == NULL) return;

    if (!is_tx_enabled) {
        i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
        i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);
        if (i2s_channel_enable(tx_chan) == ESP_OK) {
            is_tx_enabled = true;
            active_sample_rate = sample_rate;
        }
    } else if (active_sample_rate != sample_rate) {
        i2s_channel_disable(tx_chan);
        i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
        i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);
        i2s_channel_enable(tx_chan);
        active_sample_rate = sample_rate;
    }
}

static void silence_speaker(void) {
    if (tx_chan == NULL || !is_tx_enabled) return;

    // Send zero frames to keep the HT517 PLL locked and mute DC pops
    int16_t silence_buf[128] = {0};
    size_t bytes_written = 0;
    for (int i = 0; i < 4; i++) {
        i2s_channel_write(tx_chan, silence_buf, sizeof(silence_buf), &bytes_written, pdMS_TO_TICKS(20));
    }
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
        ESP_LOGE(TAG, "Unable to create TCP socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_sock, 3) != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Audio Raw TCP Stream Server listening on Port 82");

    while (1) {
        struct sockaddr_storage source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
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

        ESP_LOGI(TAG, "Audio Live Stream client connected to Port 82!");

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
                if (sent < 0) {
                    break;
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        ESP_LOGI(TAG, "Audio Live Stream client disconnected.");
        close(sock);
    }
}

static void audio_rx_stream_task(void *pvParameters) {
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(83);

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create RX TCP socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_sock, 3) != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Walkie-Talkie TCP Server listening on Port 83");

    while (1) {
        struct sockaddr_storage source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        ESP_LOGI(TAG, "Walkie-Talkie App connected! Reconfiguring I2S to 16kHz...");

        audio_stop();
        vTaskDelay(pdMS_TO_TICKS(50));

        ensure_speaker_clock(16000);

        uint8_t rx_buffer[512];
        int16_t stereo_out[512];

        while (1) {
            int len = recv(sock, rx_buffer, sizeof(rx_buffer), 0);
            if (len <= 0) {
                break;
            }

            int16_t *pcm = (int16_t *)rx_buffer;
            size_t mono_samples = len / 2;
            float vol_mult = ((float)current_volume / 100.0f);

            for (size_t i = 0; i < mono_samples; i++) {
                int32_t val = (int32_t)(pcm[i] * vol_mult);
                if (val > 32767) val = 32767;
                if (val < -32768) val = -32768;
                stereo_out[i * 2]     = (int16_t)val;
                stereo_out[i * 2 + 1] = (int16_t)val;
            }

            size_t bytes_written;
            if (is_tx_enabled) {
                i2s_channel_write(tx_chan, stereo_out, mono_samples * 4, &bytes_written, portMAX_DELAY);
            }
        }

        ESP_LOGI(TAG, "Walkie-Talkie stream disconnected.");
        close(sock);
        silence_speaker();
    }
}

static void audio_task(void *pvParameter) {
    char sound_req[32];

    while (1) {
        if (xQueueReceive(audio_queue, &sound_req, portMAX_DELAY)) {

            // -------------------------------------------------------------
            // RECORD 3 SECONDS FROM PDM MIC & PLAY IT BACK
            // -------------------------------------------------------------
            if (strcmp(sound_req, "record_3s") == 0) {
                ESP_LOGI(TAG, "Starting 3-second recording from PDM mic...");
                uint32_t sample_rate = 16000;
                size_t data_size = sample_rate * sizeof(int16_t) * 3;

                uint8_t *record_buffer = NULL;
#ifdef CONFIG_SPIRAM
                record_buffer = (uint8_t *)heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
                if (!record_buffer) {
                    record_buffer = (uint8_t *)malloc(data_size);
                }

                if (record_buffer && rx_chan) {
                    audio_stop_requested = false;
                    size_t chunk = sample_rate * sizeof(int16_t) / 10;
                    size_t total_read = 0;

                    while (total_read < data_size && !audio_stop_requested) {
                        size_t to_read = (data_size - total_read > chunk) ? chunk : (data_size - total_read);
                        size_t bytes_read = 0;
                        i2s_channel_read(rx_chan, record_buffer + total_read, to_read, &bytes_read, portMAX_DELAY);
                        total_read += bytes_read;
                    }

                    if (audio_stop_requested) {
                        ESP_LOGI(TAG, "Recording interrupted by STOP request.");
                    } else if (total_read > 0) {
                        ESP_LOGI(TAG, "Recording complete (%lu bytes). Playing back through HT517...", (unsigned long)total_read);

                        ensure_speaker_clock(sample_rate);

                        int16_t *pcm_samples = (int16_t *)record_buffer;
                        size_t num_samples = total_read / 2;

                        const float MIC_GAIN = 4.0f;
                        float vol_mult = ((float)current_volume / 100.0f) * MIC_GAIN;

                        const size_t CHUNK_SAMPLES = 256;
                        int16_t stereo_out[CHUNK_SAMPLES * 2];
                        size_t bytes_written;

                        for (size_t i = 0; i < num_samples; i += CHUNK_SAMPLES) {
                            if (audio_stop_requested || !is_tx_enabled) break;
                            size_t chunk_size = (num_samples - i < CHUNK_SAMPLES) ? (num_samples - i) : CHUNK_SAMPLES;

                            for (size_t j = 0; j < chunk_size; j++) {
                                int32_t val = (int32_t)(pcm_samples[i + j] * vol_mult);
                                if (val > 32767) val = 32767;
                                if (val < -32768) val = -32768;
                                stereo_out[j * 2]     = (int16_t)val;
                                stereo_out[j * 2 + 1] = (int16_t)val;
                            }
                            i2s_channel_write(tx_chan, stereo_out, chunk_size * 4, &bytes_written, portMAX_DELAY);
                        }
                        vTaskDelay(pdMS_TO_TICKS(100)); // Allow DMA buffer to drain
                    }
                    free(record_buffer);
                }
                silence_speaker();
                continue;
            }

            // -------------------------------------------------------------
            // PLAY STATIC WAV FILE THROUGH HT517 AMPLIFIER (GPIO 10, 11, 12)
            // -------------------------------------------------------------
            const uint8_t *wav_start = NULL;
            const uint8_t *wav_end = NULL;

            if (strcmp(sound_req, "dog_bark") == 0 || strcmp(sound_req, "bark") == 0) {
                wav_start = dog_barking_wav_start;
                wav_end = dog_barking_wav_end;
            }

            if (wav_start != NULL && wav_end > wav_start) {
                uint32_t sample_rate = 44100;
                uint16_t channels = 2;
                uint16_t bit_depth = 16;
                const uint8_t *audio_data = NULL;
                uint32_t data_size = 0;

                if (strncmp((const char *)wav_start, "RIFF", 4) == 0) {
                    size_t offset = 12;
                    while (offset < (size_t)(wav_end - wav_start) - 8) {
                        char chunk_id[5] = {0};
                        memcpy(chunk_id, wav_start + offset, 4);
                        uint32_t chunk_size = *(uint32_t *)(wav_start + offset + 4);
                        offset += 8;

                        if (strcmp(chunk_id, "fmt ") == 0) {
                            channels = *(uint16_t *)(wav_start + offset + 2);
                            sample_rate = *(uint32_t *)(wav_start + offset + 4);
                            bit_depth = *(uint16_t *)(wav_start + offset + 14);
                        } else if (strcmp(chunk_id, "data") == 0) {
                            audio_data = wav_start + offset;
                            data_size = chunk_size;
                            break;
                        }
                        offset += chunk_size;
                    }
                }

                if (audio_data != NULL && data_size > 0 && bit_depth == 16) {
                    ensure_speaker_clock(sample_rate);

                    // Flush silence so the HT517 PLL locks and unmutes cleanly
                    silence_speaker();

                    if (audio_data + data_size > wav_end) {
                        data_size = wav_end - audio_data;
                    }

                    int16_t *pcm_samples = (int16_t *)audio_data;
                    size_t total_samples = data_size / 2;
                    float vol_mult = ((float)current_volume / 100.0f);

                    audio_stop_requested = false;
                    const size_t CHUNK_SIZE = 256;
                    int16_t stereo_buffer[CHUNK_SIZE * 2];
                    size_t bytes_written;

                    if (channels == 1) {
                        for (size_t i = 0; i < total_samples; i += CHUNK_SIZE) {
                            if (audio_stop_requested || !is_tx_enabled) break;
                            size_t cur_chunk = (total_samples - i < CHUNK_SIZE) ? (total_samples - i) : CHUNK_SIZE;
                            for (size_t j = 0; j < cur_chunk; j++) {
                                int32_t val = (int32_t)(pcm_samples[i + j] * vol_mult);
                                if (val > 32767) val = 32767;
                                if (val < -32768) val = -32768;
                                stereo_buffer[j * 2]     = (int16_t)val;
                                stereo_buffer[j * 2 + 1] = (int16_t)val;
                            }
                            i2s_channel_write(tx_chan, stereo_buffer, cur_chunk * 4, &bytes_written, portMAX_DELAY);
                        }
                    } else {
                        size_t stereo_frames = total_samples / 2;
                        for (size_t i = 0; i < stereo_frames; i += CHUNK_SIZE) {
                            if (audio_stop_requested || !is_tx_enabled) break;
                            size_t cur_chunk = (stereo_frames - i < CHUNK_SIZE) ? (stereo_frames - i) : CHUNK_SIZE;
                            for (size_t j = 0; j < cur_chunk; j++) {
                                int32_t val_l = (int32_t)(pcm_samples[(i + j) * 2]     * vol_mult);
                                int32_t val_r = (int32_t)(pcm_samples[(i + j) * 2 + 1] * vol_mult);
                                if (val_l > 32767) val_l = 32767;
                                if (val_l < -32768) val_l = -32768;
                                if (val_r > 32767) val_r = 32767;
                                if (val_r < -32768) val_r = -32768;
                                stereo_buffer[j * 2]     = (int16_t)val_l;
                                stereo_buffer[j * 2 + 1] = (int16_t)val_r;
                            }
                            i2s_channel_write(tx_chan, stereo_buffer, cur_chunk * 4, &bytes_written, portMAX_DELAY);
                        }
                    }

                    // Allow the trailing audio data to physically exit the DMA FIFO
                    vTaskDelay(pdMS_TO_TICKS(100));

                    if (audio_stop_requested) {
                        ESP_LOGI(TAG, "Audio playback interrupted by STOP request.");
                    } else {
                        ESP_LOGI(TAG, "HT517 Playback finished.");
                    }
                } else {
                    ESP_LOGE(TAG, "Invalid or unsupported WAV structure.");
                }
            }
            silence_speaker();
        }
    }
}

void audio_player_init(void) {
    load_audio_nvs();
    audio_queue = xQueueCreate(5, sizeof(char) * 32);

    // Initialize HT517 I2S TX channel dedicated to Controller 0
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN, // GPIO 10 -> HT517 D10
            .ws   = I2S_LRC_PIN,  // GPIO 11 -> HT517 D11
            .dout = I2S_DIN_PIN,  // GPIO 12 -> HT517 D12
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false, .bclk_inv = false, .ws_inv = false
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));

    // Enable I2S channel continuously on boot so HT517 PLL locks and unmutes
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
    is_tx_enabled = true;
    active_sample_rate = 44100;
    silence_speaker();

    // Initialize optional PDM RX channel on Controller 1
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
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
        }
    }

    xTaskCreatePinnedToCore(audio_task, "audio_task", 4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(audio_stream_task, "audio_stream_task", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(audio_rx_stream_task, "audio_rx_stream_task", 4096, NULL, 3, NULL, 1);

    ESP_LOGI(TAG, "HT517 I2S Audio Initialized (BCLK=GPIO10, LRC=GPIO11, DIN=GPIO12)");
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
    if (current_volume == 0) return;

    char req[32];
    strncpy(req, sound_name, sizeof(req) - 1);
    req[sizeof(req) - 1] = '\0';

    xQueueSend(audio_queue, &req, 0);
}

void audio_stop(void) {
    audio_stop_requested = true;
    xQueueReset(audio_queue);
    silence_speaker();
}