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

// HT517 I2S Amplifier Pinout Mapping:
// GPIO 10 -> HT517 pin DIO / DI0 (BCLK / SCK)
// GPIO 11 -> HT517 pin DI1 (LRC / WS)
// GPIO 12 -> HT517 pin DI2 (DIN / SD)
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
    if (current_volume < 10) current_volume = 80;
    if (current_volume > 100) current_volume = 100;
}

static void ensure_speaker_clock(uint32_t sample_rate) {
    if (tx_chan == NULL) return;
    if (sample_rate < 8000 || sample_rate > 96000) {
        sample_rate = 44100;
    }

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

    int16_t silence_buf[256] = {0};
    size_t bytes_written = 0;
    for (int i = 0; i < 4; i++) {
        i2s_channel_write(tx_chan, silence_buf, sizeof(silence_buf), &bytes_written, pdMS_TO_TICKS(20));
    }
}

// Synthesizes a clean two-tone test chirp (880Hz / 1760Hz) to test the HT517 speaker output
static void play_synth_beep(void) {
    ESP_LOGI(TAG, "Playing diagnostic test tone on HT517 speaker...");
    uint32_t srate = 22050;
    ensure_speaker_clock(srate);
    silence_speaker();

    float vol = (float)current_volume / 100.0f;
    int16_t out_buf[256 * 2];
    size_t bytes_written;

    // Beep tone 1: 880 Hz for 150 ms
    int samples1 = (srate * 150) / 1000;
    for (int i = 0; i < samples1 && !audio_stop_requested; i += 256) {
        int chunk = (samples1 - i < 256) ? (samples1 - i) : 256;
        for (int j = 0; j < chunk; j++) {
            float t = (float)(i + j) / (float)srate;
            int16_t val = (int16_t)(sinf(2.0f * (float)M_PI * 880.0f * t) * 20000.0f * vol);
            out_buf[j * 2]     = val;
            out_buf[j * 2 + 1] = val;
        }
        i2s_channel_write(tx_chan, out_buf, chunk * 4, &bytes_written, portMAX_DELAY);
    }

    vTaskDelay(pdMS_TO_TICKS(40));

    // Beep tone 2: 1760 Hz for 180 ms
    int samples2 = (srate * 180) / 1000;
    for (int i = 0; i < samples2 && !audio_stop_requested; i += 256) {
        int chunk = (samples2 - i < 256) ? (samples2 - i) : 256;
        for (int j = 0; j < chunk; j++) {
            float t = (float)(i + j) / (float)srate;
            int16_t val = (int16_t)(sinf(2.0f * (float)M_PI * 1760.0f * t) * 22000.0f * vol);
            out_buf[j * 2]     = val;
            out_buf[j * 2 + 1] = val;
        }
        i2s_channel_write(tx_chan, out_buf, chunk * 4, &bytes_written, portMAX_DELAY);
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    silence_speaker();
}

// Synthesizes a realistic double-bark canine sound in software
static void play_synth_bark(void) {
    ESP_LOGI(TAG, "Playing synthesized dog bark waveform...");
    uint32_t srate = 22050;
    ensure_speaker_clock(srate);
    silence_speaker();

    float vol = (float)current_volume / 100.0f;
    int16_t out_buf[256 * 2];
    size_t bytes_written;

    // Two consecutive bark impulses
    for (int bark_num = 0; bark_num < 2 && !audio_stop_requested; bark_num++) {
        int bark_samples = (srate * 160) / 1000; // 160ms burst
        for (int i = 0; i < bark_samples && !audio_stop_requested; i += 256) {
            int chunk = (bark_samples - i < 256) ? (bark_samples - i) : 256;
            for (int j = 0; j < chunk; j++) {
                float progress = (float)(i + j) / (float)bark_samples;
                // Pitch falls from 750 Hz down to 220 Hz
                float freq = 750.0f - (530.0f * progress);
                float envelope = (progress < 0.15f) ? (progress / 0.15f) : (1.0f - progress);
                float t = (float)(i + j) / (float)srate;
                float sample_val = (sinf(2.0f * (float)M_PI * freq * t) + 0.4f * sinf(4.0f * (float)M_PI * freq * t)) * envelope;
                int16_t val = (int16_t)(sample_val * 24000.0f * vol);
                out_buf[j * 2]     = val;
                out_buf[j * 2 + 1] = val;
            }
            i2s_channel_write(tx_chan, out_buf, chunk * 4, &bytes_written, portMAX_DELAY);
        }
        vTaskDelay(pdMS_TO_TICKS(80)); // Pause between barks
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    silence_speaker();
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
            ESP_LOGI(TAG, "Audio task processing command: '%s' (Volume: %ld%%)", sound_req, (long)current_volume);

            if (strcmp(sound_req, "test_beep") == 0 || strcmp(sound_req, "beep") == 0) {
                audio_stop_requested = false;
                play_synth_beep();
                continue;
            }

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
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                    free(record_buffer);
                }
                silence_speaker();
                continue;
            }

            // -------------------------------------------------------------
            // PLAY STATIC WAV FILE THROUGH HT517 AMPLIFIER (GPIO 10, 11, 12)
            // -------------------------------------------------------------
            const uint8_t *wav_start = dog_barking_wav_start;
            const uint8_t *wav_end   = dog_barking_wav_end;
            size_t wav_len = (wav_end > wav_start) ? (size_t)(wav_end - wav_start) : 0;

            ESP_LOGI(TAG, "WAV resource address: start=%p, end=%p, size=%lu bytes", wav_start, wav_end, (unsigned long)wav_len);

            if (wav_len < 44) {
                ESP_LOGW(TAG, "WAV resource too small or empty. Falling back to synthesized bark.");
                audio_stop_requested = false;
                play_synth_bark();
                continue;
            }

            uint32_t sample_rate = 44100;
            uint16_t channels = 1;
            uint16_t bit_depth = 16;
            const uint8_t *audio_data = NULL;
            uint32_t data_size = 0;

            // Safe parsing of RIFF WAVE structure using memcpy to prevent unaligned load exceptions
            if (memcmp(wav_start, "RIFF", 4) == 0 && memcmp(wav_start + 8, "WAVE", 4) == 0) {
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
                        ESP_LOGI(TAG, "WAV 'fmt ' parsed: %u Hz, %u ch, %u bits", (unsigned int)sample_rate, channels, bit_depth);
                    } else if (strcmp(chunk_id, "data") == 0) {
                        audio_data = wav_start + offset;
                        data_size = chunk_size;
                        ESP_LOGI(TAG, "WAV 'data' chunk found: %lu bytes", (unsigned long)data_size);
                        break;
                    }
                    offset += (chunk_size + 1) & ~1;
                }
            }

            // Fallback: locate "data" signature if chunk traversal missed it
            if (audio_data == NULL) {
                for (size_t i = 12; i + 8 <= wav_len; i++) {
                    if (memcmp(wav_start + i, "data", 4) == 0) {
                        memcpy(&data_size, wav_start + i + 4, 4);
                        audio_data = wav_start + i + 8;
                        if (audio_data + data_size > wav_end) {
                            data_size = wav_end - audio_data;
                        }
                        ESP_LOGI(TAG, "WAV 'data' found via scan at offset %lu, size %lu bytes", (unsigned long)i, (unsigned long)data_size);
                        break;
                    }
                }
            }

            // Fallback: default to standard 44-byte WAV header offset
            if (audio_data == NULL && wav_len > 44) {
                audio_data = wav_start + 44;
                data_size = wav_len - 44;
                sample_rate = 44100;
                channels = 1;
                bit_depth = 16;
                ESP_LOGI(TAG, "Using default 44-byte WAV header fallback: %lu bytes", (unsigned long)data_size);
            }

            if (audio_data != NULL && data_size > 0) {
                ensure_speaker_clock(sample_rate);
                silence_speaker();

                float vol_mult = ((float)current_volume / 100.0f);
                audio_stop_requested = false;
                const size_t CHUNK_FRAMES = 256;
                int16_t stereo_buffer[CHUNK_FRAMES * 2];
                size_t bytes_written;

                if (bit_depth == 16) {
                    size_t total_samples = data_size / sizeof(int16_t);
                    if (channels == 1) {
                        for (size_t i = 0; i < total_samples && !audio_stop_requested; i += CHUNK_FRAMES) {
                            size_t frames = (total_samples - i < CHUNK_FRAMES) ? (total_samples - i) : CHUNK_FRAMES;
                            for (size_t j = 0; j < frames; j++) {
                                int16_t raw_val = 0;
                                memcpy(&raw_val, audio_data + (i + j) * 2, 2);
                                int32_t val = (int32_t)(raw_val * vol_mult);
                                if (val > 32767) val = 32767;
                                if (val < -32768) val = -32768;
                                stereo_buffer[j * 2]     = (int16_t)val;
                                stereo_buffer[j * 2 + 1] = (int16_t)val;
                            }
                            i2s_channel_write(tx_chan, stereo_buffer, frames * 4, &bytes_written, portMAX_DELAY);
                        }
                    } else {
                        size_t total_frames = total_samples / 2;
                        for (size_t i = 0; i < total_frames && !audio_stop_requested; i += CHUNK_FRAMES) {
                            size_t frames = (total_frames - i < CHUNK_FRAMES) ? (total_frames - i) : CHUNK_FRAMES;
                            for (size_t j = 0; j < frames; j++) {
                                int16_t raw_l = 0, raw_r = 0;
                                memcpy(&raw_l, audio_data + (i + j) * 4, 2);
                                memcpy(&raw_r, audio_data + (i + j) * 4 + 2, 2);
                                int32_t val_l = (int32_t)(raw_l * vol_mult);
                                int32_t val_r = (int32_t)(raw_r * vol_mult);
                                if (val_l > 32767) val_l = 32767;
                                if (val_l < -32768) val_l = -32768;
                                if (val_r > 32767) val_r = 32767;
                                if (val_r < -32768) val_r = -32768;
                                stereo_buffer[j * 2]     = (int16_t)val_l;
                                stereo_buffer[j * 2 + 1] = (int16_t)val_r;
                            }
                            i2s_channel_write(tx_chan, stereo_buffer, frames * 4, &bytes_written, portMAX_DELAY);
                        }
                    }
                } else if (bit_depth == 8) {
                    size_t total_samples = data_size;
                    for (size_t i = 0; i < total_samples && !audio_stop_requested; i += CHUNK_FRAMES) {
                        size_t frames = (total_samples - i < CHUNK_FRAMES) ? (total_samples - i) : CHUNK_FRAMES;
                        for (size_t j = 0; j < frames; j++) {
                            uint8_t uval = audio_data[i + j];
                            int32_t val = (int32_t)(((int)uval - 128) << 8);
                            val = (int32_t)(val * vol_mult);
                            if (val > 32767) val = 32767;
                            if (val < -32768) val = -32768;
                            stereo_buffer[j * 2]     = (int16_t)val;
                            stereo_buffer[j * 2 + 1] = (int16_t)val;
                        }
                        i2s_channel_write(tx_chan, stereo_buffer, frames * 4, &bytes_written, portMAX_DELAY);
                    }
                }

                vTaskDelay(pdMS_TO_TICKS(150));
                if (audio_stop_requested) {
                    ESP_LOGI(TAG, "Audio playback interrupted by STOP request.");
                } else {
                    ESP_LOGI(TAG, "HT517 Playback finished successfully.");
                }
            } else {
                ESP_LOGW(TAG, "Failed to parse WAV structure. Using synthesized bark.");
                audio_stop_requested = false;
                play_synth_bark();
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
    chan_cfg.auto_clear = true; // Automatically send zero frames during underflows to avoid clicks
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN, // GPIO 10 -> HT517 DIO
            .ws   = I2S_LRC_PIN,  // GPIO 11 -> HT517 DI1
            .dout = I2S_DIN_PIN,  // GPIO 12 -> HT517 DI2
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false, .bclk_inv = false, .ws_inv = false
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));

    // Enable I2S channel continuously on boot so HT517 internal PLL locks and unmutes cleanly
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
        } else {
            i2s_del_channel(rx_chan);
            rx_chan = NULL;
        }
    }

    xTaskCreatePinnedToCore(audio_task, "audio_task", 4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(audio_stream_task, "audio_stream_task", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(audio_rx_stream_task, "audio_rx_stream_task", 4096, NULL, 3, NULL, 1);

    ESP_LOGI(TAG, "HT517 I2S Audio Initialized (DIO=GPIO10, DI1=GPIO11, DI2=GPIO12)");
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
    silence_speaker();
}