#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"

#include "driver/i2s_std.h"
#include "driver/i2c.h"
#include "es8311.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_websocket_client.h"
#include "esp_wireguard.h"
#include "esp_netif_sntp.h"

#include "display.h"

static const char *TAG = "WAKE_WORD";

// =======================
// ES8311 CODEC - PIN (ESP32-P4-Module-DEV-KIT)
// Theo example_config.h của Espressif cho CONFIG_IDF_TARGET_ESP32P4
// =======================
#define I2C_NUM_CODEC   I2C_NUM_0
#define I2C_SCL_IO      GPIO_NUM_8
#define I2C_SDA_IO      GPIO_NUM_7

#define I2S_MCK_IO      GPIO_NUM_13
#define I2S_BCK_IO      GPIO_NUM_12
#define I2S_WS_IO       GPIO_NUM_10
#define I2S_DO_IO       GPIO_NUM_9   
#define I2S_DI_IO       GPIO_NUM_11

#define EXAMPLE_SAMPLE_RATE     16000
#define EXAMPLE_MCLK_MULTIPLE   384
#define EXAMPLE_MCLK_FREQ_HZ    (EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE)

// =======================
// WIFI CONFIG
// =======================
//công ty
#define WIFI_SSID       "DevBriX"
#define WIFI_PASSWORD   "@DevBriX2026$#"
// hoangwifi
// #define WIFI_SSID       "+++"
// #define WIFI_PASSWORD   "h0916325810"
// nhà
//#define WIFI_SSID       "AH2-1009"
//#define WIFI_PASSWORD   "nhincaichogi"
#define EXAMPLE_ESP_MAXIMUM_RETRY 5

// =======================
// WEBSOCKET CONFIG
// =======================
//công ty 
// #define WEBSOCKET_URI   "ws://192.168.2.214:8765" 
// nhà
#define WEBSOCKET_URI   "ws://10.10.10.1:8765"
#define WS_TIMEOUT_MS   5000
static EventGroupHandle_t sys_event_group = NULL;
typedef enum {
    STATE_IDLE,
    STATE_RECORDING,
    STATE_WAITING_RESULT,
} system_state_t;

volatile system_state_t sys_state = STATE_IDLE;

volatile bool force_record_flag = false;   
volatile bool audio_stream_ended       = false;  // server đã gửi AUDIO_END cho lượt TTS hiện tại
volatile bool resume_conversation_flag = false;  // báo detect_task tự động ghi âm tiếp, không cần wake word  

// =======================
// GLOBAL VARIABLES
// =======================
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

i2s_chan_handle_t rx_chan           = NULL;
i2s_chan_handle_t tx_chan = NULL;              // thêm dòng này, cạnh rx_chan
RingbufHandle_t   playback_ringbuf = NULL;     // buffer chứa PCM chờ phát ra loa
static bool       receiving_audio = false;     // đánh dấu đang nhận 1 frame binary audio
static volatile bool new_stream_pending = false;   // đánh dấu vừa bắt đầu 1 đoạn audio mới, cần đệm lại trước khi phát

#define PLAYBACK_PRIME_BYTES   12800   // ~400ms audio ở 16kHz, mono, 16-bit (16000 mau/giay * 2 byte/mau * 0.4 giay)
#define RESUME_DELAY_MS        2000
static es8311_handle_t es_handle    = NULL;
const esp_afe_sr_iface_t *afe_handle = NULL;
esp_afe_sr_data_t        *afe_data   = NULL;
RingbufHandle_t           audio_ringbuf = NULL;
esp_websocket_client_handle_t ws_client = NULL;

// =======================
// JSON PARSER ĐƠN GIẢN
// =======================
static int json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *pos = strstr(json, search);
    if (!pos) return 0;
    pos += strlen(search);
    while (*pos == ' ') pos++;
    if (*pos != '"') return 0;
    pos++;
    size_t i = 0;
    while (*pos && *pos != '"' && i < out_size - 1)
        out[i++] = *pos++;
    out[i] = '\0';
    return 1;
}

// =======================
// WIFI 
// =======================
static int s_retry_num = 0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Thử kết nối lại AP...");
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"Kết nối AP thất bại");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void app_wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();

    // Lưu ý: esp_netif_init() và esp_event_loop_create_default() 
    // đã được gọi chung ở đây. Nếu app_main đã gọi rồi thì nên cẩn thận, 
    // nhưng theo cấu trúc file của bạn thì gọi ở đây là đúng.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_LOGI(TAG, "Chờ ESP32-C6 co-processor khởi động (hosted slave)...");
    vTaskDelay(pdMS_TO_TICKS(500));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init hoàn tất. Đang chờ kết nối...");

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Đã kết nối thành công tới AP");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Gặp lỗi, không thể kết nối tới AP");
    } else {
        ESP_LOGE(TAG, "Lỗi sự kiện không xác định");
    }
}

// =======================
// WEBSOCKET
// =======================
static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    if (event_id == WEBSOCKET_EVENT_DATA)
    {
        // --- Nhận audio PCM (binary) từ server để phát ra loa ---
            if (data->op_code == 0x02) {
            receiving_audio = true;
            new_stream_pending = true;   // báo cho play_task biết đây là đoạn audio mới, cần chờ đệm trước khi phát
            }
        if (receiving_audio && data->data_len > 0 &&
            (data->op_code == 0x02 || data->op_code == 0x00))
        {
                if (playback_ringbuf) {
                // --- Kiểm tra PCM nhận từ server có bị clip (chạm đỉnh full-scale) không ---
                int16_t *samples = (int16_t *)data->data_ptr;
                    int sample_count = data->data_len / sizeof(int16_t);
                    int clipped_count = 0;
                    int max_run_length = 0;      // chuỗi liên tiếp dài nhất các mẫu bị ghim ở đỉnh
                    int current_run_length = 0;  // chuỗi liên tiếp hiện tại đang đếm

                    for (int i = 0; i < sample_count; i++) {
                        bool is_clipped = (samples[i] >= 32760 || samples[i] <= -32760);
                        if (is_clipped) {
                            clipped_count++;
                            current_run_length++;
                            if (current_run_length > max_run_length) {
                                max_run_length = current_run_length;
                            }
                        } else {
                            current_run_length = 0;
                        }
                    }

                    // Chỉ cảnh báo khi có chuỗi liên tiếp >= 3 mẫu bị ghim cứng -> dấu hiệu clipping thật sự
                    // (1-2 mẫu chạm đỉnh đơn lẻ có thể chỉ là đỉnh sóng to bình thường, không phải lỗi)
                    if (max_run_length >= 3) {
                        float clipped_percent = 100.0f * clipped_count / sample_count;
                        ESP_LOGW(TAG, "PCM từ server: %d/%d mẫu cham full-scale (%.1f%%), chuoi lien tiep dai nhat = %d mau",
                                clipped_count, sample_count, clipped_percent, max_run_length);
                    }
                // --- Hết đoạn kiểm tra, phần đẩy vào ringbuf giữ nguyên như cũ ---

                BaseType_t ok = xRingbufferSend(playback_ringbuf, data->data_ptr,
                                                data->data_len, pdMS_TO_TICKS(10000));
                if (ok != pdTRUE) {
                    ESP_LOGE(TAG, "Playback ringbuf TIMEOUT - audio bị drop! (%d bytes)", data->data_len);
                }
            }

            if (data->payload_offset + data->data_len >= data->payload_len) {
                receiving_audio = false;
                ESP_LOGI(TAG, "Nhận xong audio PCM (%d bytes)", data->payload_len);
            }
            return;  // không đi tiếp xuống phần JSON
        }

        // --- Nhận text (JSON kết quả hoặc "AUDIO_END") ---
        if (data->data_len > 0 && data->op_code == 0x01)
        {
            char json_buf[512] = {0};
            int len = data->data_len < (int)(sizeof(json_buf) - 1)
                      ? data->data_len : (int)(sizeof(json_buf) - 1);
            memcpy(json_buf, data->data_ptr, len);

            if (strncmp(json_buf, "AUDIO_END", 9) == 0) {
                ESP_LOGI(TAG, "Server báo hiệu kết thúc audio.");
                audio_stream_ended = true;
                return;
            }

            if (sys_state == STATE_WAITING_RESULT)
            {
                ESP_LOGI(TAG, "JSON: %s", json_buf);

                char speech[256] = {0};
                char face[32]    = {0};
                char motor[32]   = {0};
                json_get_string(json_buf, "speech", speech, sizeof(speech));
                json_get_string(json_buf, "face",   face,   sizeof(face));
                json_get_string(json_buf, "motor",  motor,  sizeof(motor));

                ESP_LOGI(TAG, "speech='%s' face='%s' motor='%s'", speech, face, motor);
                display_update(face, speech);

                sys_state = STATE_IDLE;
            }
        }
    }
    else if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "WebSocket kết nối.");
    }
    else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "WebSocket mất kết nối.");
        if (sys_state != STATE_IDLE) sys_state = STATE_IDLE;
    }
}

void websocket_init(void)
{
    esp_websocket_client_config_t ws_cfg = {
        .uri                  = WEBSOCKET_URI,
        .reconnect_timeout_ms = WS_TIMEOUT_MS,
        .buffer_size          = 4096,
    };
    ws_client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY,
                                  websocket_event_handler, (void *)ws_client);
    esp_websocket_client_start(ws_client);
}

// =======================
// I2C (điều khiển ES8311)
// =======================
static void i2c_init(void)
{
    i2c_config_t i2c_cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_SDA_IO,
        .scl_io_num       = I2C_SCL_IO,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_CODEC, &i2c_cfg));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_CODEC, I2C_MODE_MASTER, 0, 0, 0));
}

#define GPIO_OUTPUT_PA  GPIO_NUM_53

static void pa_enable(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_OUTPUT_PA),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(GPIO_OUTPUT_PA, 1);
    ESP_LOGI(TAG, "PA (ampli loa) đã bật");
}
// =======================
// I2S + ES8311 (mic onboard + loa)
// =======================
void i2s_init(void)
{   
    pa_enable();
    i2c_init();

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 16;
    chan_cfg.dma_frame_num = 1024;
    chan_cfg.auto_clear    = true;  
    // Tạo cả tx_chan và rx_chan cùng lúc (full-duplex, chung 1 controller)
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(EXAMPLE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCK_IO,
            .bclk = I2S_BCK_IO,
            .ws   = I2S_WS_IO,
            .dout = I2S_DO_IO,   
            .din  = I2S_DI_IO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false }
        }
    };
    std_cfg.clk_cfg.mclk_multiple = EXAMPLE_MCLK_MULTIPLE;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));

    // ES8311 codec
    es_handle = es8311_create(I2C_NUM_CODEC, ES8311_ADDRRES_0);
    if (!es_handle) {
        ESP_LOGE(TAG, "es8311_create thất bại");
        return;
    }
    const es8311_clock_config_t es_clk = {
        .mclk_inverted      = false,
        .sclk_inverted      = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency     = EXAMPLE_MCLK_FREQ_HZ,
        .sample_frequency   = EXAMPLE_SAMPLE_RATE
    };
    ESP_ERROR_CHECK(es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
    ESP_ERROR_CHECK(es8311_sample_frequency_config(es_handle, EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE, EXAMPLE_SAMPLE_RATE));
    ESP_ERROR_CHECK(es8311_microphone_config(es_handle, false));
    // sau dòng es8311_microphone_config(es_handle, false);
    ESP_ERROR_CHECK(es8311_microphone_gain_set(es_handle, ES8311_MIC_GAIN_30DB));
    ESP_ERROR_CHECK(es8311_voice_volume_set(es_handle,80, NULL));   

    ESP_LOGI(TAG, "I2S full-duplex + ES8311 OK");
}

// =======================
// FEED TASK
// =======================
void feed_task(void *arg)
{
    int chunk_size = afe_handle->get_feed_chunksize(afe_data);

    // ES8311: I2S stereo 16-bit -> mỗi frame = 2 x int16_t (L, R)
    int16_t *i2s_buffer = malloc(chunk_size * 2 * sizeof(int16_t));
    int16_t *pcm_buffer = malloc(chunk_size * sizeof(int16_t)); // cho AFE
    int16_t *raw_buffer = malloc(chunk_size * sizeof(int16_t)); // cho server

    if (!i2s_buffer || !pcm_buffer || !raw_buffer) {
        ESP_LOGE(TAG, "malloc thất bại");
        vTaskDelete(NULL);
        return;
    }

    size_t  bytes_read;
    int32_t dc_offset = 0;

    while (1)
    {
        esp_err_t ret = i2s_channel_read(rx_chan, i2s_buffer,
                                          chunk_size * 2 * sizeof(int16_t),
                                          &bytes_read, portMAX_DELAY);
        if (ret != ESP_OK || bytes_read == 0) continue;

        int samples_read = bytes_read / (2 * sizeof(int16_t)); // số frame stereo đọc được

        for (int i = 0; i < samples_read; i++) {
            // TODO xác nhận bằng test_mic_serial: mic ES8311 có thể nằm ở kênh trái
            // hoặc lặp trên cả 2 kênh tuỳ cấu hình codec. Đang lấy kênh trái (index 0).
            int32_t raw_sample = i2s_buffer[i * 2];

            // DC offset removal
            dc_offset = (dc_offset * 63 + raw_sample) / 64;
            int32_t clean = raw_sample - dc_offset;

            // Tăng gain nhẹ để âm thanh nghe rõ hơn nhưng vẫn giữ được sự ổn định.
            // Nếu tăng quá mức, tín hiệu sẽ bị clipping và chất lượng âm thanh sẽ giảm.
            
            int32_t r = clean * 12;
            if (r >  32767) r =  32767;
            if (r < -32768) r = -32768;
            raw_buffer[i] = (int16_t)r;

            int32_t p = clean * 4;
            if (p >  32767) p =  32767;
            if (p < -32768) p = -32768;
            pcm_buffer[i] = (int16_t)p;
        }

        // Luồng 1: AFE detect
        afe_handle->feed(afe_data, pcm_buffer);

        // Luồng 2: raw audio vào ringbuf (không qua AFE processing)
       if (sys_state == STATE_RECORDING && audio_ringbuf != NULL) {
            // Kiểm tra còn chỗ trước khi push
            UBaseType_t free_size = xRingbufferGetCurFreeSize(audio_ringbuf);
            if (free_size >= chunk_size * sizeof(int16_t)) {
                BaseType_t ok = xRingbufferSend(audio_ringbuf, raw_buffer,
                                                 chunk_size * sizeof(int16_t), 0);
                if (ok != pdTRUE) {
                    ESP_LOGW(TAG, "Ringbuf FULL - frame bị drop!");
                }
            }
            // Nếu không đủ chỗ thì bỏ qua frame này, không log gì cả
        }
    }
}

void play_task(void *arg)
{
    const int chunk_samples = 512;
    int16_t *stereo_buf = malloc(chunk_samples * 2 * sizeof(int16_t));
    if (!stereo_buf) {
        ESP_LOGE(TAG, "play_task malloc thất bại");
        vTaskDelete(NULL);
        return;
    }

    bool priming = false;   // trạng thái đang chờ đệm đủ dữ liệu trước khi phát

    while (1) {
        // --- Phát hiện đoạn audio mới -> chuyển sang trạng thái chờ đệm ---
        if (new_stream_pending) {
            new_stream_pending = false;
            priming = true;
            ESP_LOGI(TAG, "Bat dau doan audio moi - dang dem du lieu truoc khi phat...");
        }

        // --- Trạng thái đệm: chờ ringbuf tích đủ PLAYBACK_PRIME_BYTES rồi mới phát ---
        if (priming) {
            UBaseType_t free_size = xRingbufferGetCurFreeSize(playback_ringbuf);
            UBaseType_t used_size = (128 * 1024) - free_size;   // 128*1024 = kich thuoc playback_ringbuf, khai bao trong app_main

            if (used_size >= PLAYBACK_PRIME_BYTES) {
                priming = false;
                ESP_LOGI(TAG, "Da dem du (%u byte) - bat dau phat.", (unsigned int)used_size);
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;   // chưa đủ đệm, chưa lấy dữ liệu ra phát
            }
        }

        size_t item_size;
        int16_t *mono = (int16_t *)xRingbufferReceiveUpTo(
            playback_ringbuf, &item_size, pdMS_TO_TICKS(200),
            chunk_samples * sizeof(int16_t));

        if (mono == NULL) {
                if (audio_stream_ended) {
                    audio_stream_ended = false;
                    ESP_LOGI(TAG, "Phat xong TTS. Cho %d ms roi quay lai ghi am...", RESUME_DELAY_MS);
                    vTaskDelay(pdMS_TO_TICKS(RESUME_DELAY_MS));
                    resume_conversation_flag = true;
                }
                continue;  // chưa có audio, chờ tiếp
            }
        int samples = item_size / sizeof(int16_t);
        for (int i = 0; i < samples; i++) {
            stereo_buf[i * 2]     = mono[i];  // loa mono -> lặp ra cả 2 kênh
            stereo_buf[i * 2 + 1] = mono[i];
        }

        size_t bytes_written;
        i2s_channel_write(tx_chan, stereo_buf, samples * 2 * sizeof(int16_t),
                           &bytes_written, portMAX_DELAY);

        vRingbufferReturnItem(playback_ringbuf, mono);
    }
}
// =======================
// DETECT TASK
// =======================
#define EVENT_START_SEND  BIT0

void detect_task(void *arg)
{
    int  silence_count       = 0;
    int  recording_frames    = 0;
    int  speech_run_count    = 0;
    bool any_speech_detected = false;
    const int MIN_SPEECH_RUN_FRAMES = 10;    // ~180-200ms non-silence liên tiếp mới tính là thực sự nói
    const int MAX_SILENCE_FRAMES    = 60;
    const int MAX_RECORDING_FRAMES  = 450;

    // Cửa sổ trượt để quyết định 1 "nhịp" là im lặng hay không (đa số phiếu, tránh tạp âm 1 frame lẻ)
    #define SILENCE_WINDOW_SIZE      4
    #define SILENCE_WINDOW_THRESHOLD 1   // >=2/4 frame là silence thì tính nhịp này là im lặng
    bool silence_window[SILENCE_WINDOW_SIZE] = { true, true, true, true };
    int  silence_window_idx = 0;

    while (1)
    {
        afe_fetch_result_t *res = afe_handle->fetch(afe_data);
        if (res == NULL) continue;

        if (sys_state == STATE_IDLE)
        {
            if (res->wakeup_state == WAKENET_DETECTED || force_record_flag || resume_conversation_flag)
            {
                if (force_record_flag) {
                    ESP_LOGI(TAG, "Test mode: Bỏ qua wake word! Bắt đầu ghi âm...");
                    force_record_flag = false;   // reset cờ ngay sau khi dùng
                } else if (resume_conversation_flag) {
                    ESP_LOGI(TAG, "Tiep tuc hoi thoai! Bat dau ghi am...");
                    resume_conversation_flag = false;
                } else {
                    ESP_LOGI(TAG, "Wake word! Bắt đầu ghi âm...");
                }

                silence_count       = 0;
                recording_frames    = 0;
                speech_run_count    = 0;
                any_speech_detected = false;
                for (int i = 0; i < SILENCE_WINDOW_SIZE; i++) silence_window[i] = true;
                silence_window_idx = 0;
                size_t item_size;
                void  *old_data;
                while ((old_data = xRingbufferReceive(audio_ringbuf, &item_size, 0)) != NULL)
                    vRingbufferReturnItem(audio_ringbuf, old_data);

                display_set_listening();
                sys_state = STATE_RECORDING;
            }
        }
        else if (sys_state == STATE_RECORDING)
        {
            recording_frames++;

            // Cập nhật cửa sổ trượt
            silence_window[silence_window_idx] = (res->vad_state == VAD_SILENCE);
            silence_window_idx = (silence_window_idx + 1) % SILENCE_WINDOW_SIZE;

            int silent_in_window = 0;
            for (int i = 0; i < SILENCE_WINDOW_SIZE; i++) {
                if (silence_window[i]) silent_in_window++;
            }
            bool window_is_silence = (silent_in_window >= SILENCE_WINDOW_THRESHOLD);

            if (window_is_silence) {
                silence_count++;
                speech_run_count = 0;
            } else {
                silence_count = 0;
                speech_run_count++;
                if (speech_run_count >= MIN_SPEECH_RUN_FRAMES) {
                    any_speech_detected = true;
                }
            }
            // Log để theo dõi như trước
            if (recording_frames % 10 == 0)
                ESP_LOGI(TAG, "Đang ghi âm... frame=%d silence=%d", recording_frames, silence_count);

            bool end_by_silence = (silence_count    >= MAX_SILENCE_FRAMES);
            bool end_by_timeout = (recording_frames >= MAX_RECORDING_FRAMES);

            if (end_by_silence || end_by_timeout)
            {
                if (!any_speech_detected)
                {
                    // Không nói gì cả (vd. sau khi robot vừa trả lời xong) -> không gửi server,
                    // quay thẳng về chờ wake word
                    ESP_LOGI(TAG, "Khong phat hien giong noi. Quay ve cho wake word.");
                    display_reset_to_idle();
                    sys_state = STATE_IDLE;
                }
                else
                {
                    if (end_by_timeout)
                        ESP_LOGW(TAG, "Timeout! Kết thúc ghi âm.");
                    else
                        ESP_LOGI(TAG, "Im lặng. Kết thúc ghi âm.");

                    display_set_thinking();
                    sys_state = STATE_WAITING_RESULT;
                    // QUAN TRỌNG: KHÔNG đổi state ở đây
                    // feed_task vẫn push vào ringbuf trong khi send_task drain
                    // send_task sẽ tự đổi state sau khi gửi END
                    xEventGroupSetBits(sys_event_group, EVENT_START_SEND);
                }
            }
        }
    }
}

void send_task(void *arg)
{
    while (1)
    {
        xEventGroupWaitBits(sys_event_group, EVENT_START_SEND,
                            pdTRUE, pdFALSE, portMAX_DELAY);

        // Đổi state ở đây — feed_task ngừng push từ lúc này
        

        if (!esp_websocket_client_is_connected(ws_client)) {
            sys_state = STATE_IDLE;
            continue;
        }

        // Chờ thêm 1 chút để feed_task kịp push frame cuối vào ringbuf
        vTaskDelay(pdMS_TO_TICKS(50));

        size_t item_size;
        void  *audio_data;
        int    chunks = 0;

        while ((audio_data = xRingbufferReceiveUpTo(
                    audio_ringbuf, &item_size,
                    pdMS_TO_TICKS(200), 8192)) != NULL)
        {
            esp_websocket_client_send_bin(ws_client,
                (const char *)audio_data, item_size, portMAX_DELAY);
            vRingbufferReturnItem(audio_ringbuf, audio_data);
            chunks++;
        }

        ESP_LOGI(TAG, "Gửi xong %d chunks → END", chunks);
        esp_websocket_client_send_text(ws_client, "END", 3, portMAX_DELAY);
    }
}

void keyboard_task(void *arg)
{
    ESP_LOGI(TAG, "Test mode: Nhấn phím 's' (và Enter) trên Serial Monitor để giả lập wake word.");
    while (1) {
        int c = fgetc(stdin);
        if (c == 's' || c == 'S') {
            if (sys_state == STATE_IDLE) {
                force_record_flag = true;
            } else {
                ESP_LOGW(TAG, "Hệ thống đang bận (%d), không thể trigger lúc này.", sys_state);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static wireguard_ctx_t wg_ctx = {0};

void wireguard_start(void)
{
    ESP_LOGI("WG", "Đang đồng bộ thời gian NTP...");
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_config);
    
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) != ESP_OK) {
        ESP_LOGE("WG", "Lỗi đồng bộ NTP. Kết nối WireGuard có thể bị Server từ chối.");
    }

    ESP_LOGI("WG", "Khởi tạo WireGuard...");
    wireguard_config_t wg_config = ESP_WIREGUARD_CONFIG_DEFAULT();
    
    // Config Interface (ESP32)
    wg_config.private_key = "YH0uRMEK7pmNL6kby11H4tVZyjD2QCNR5Aj2xWLv1kg=";
    wg_config.listen_port = 51824; 
    wg_config.allowed_ip = "10.10.10.3";
    wg_config.allowed_ip_mask = "255.255.255.0";
    
    // Config Peer (Server)
    wg_config.public_key = "mW7pVXjgvjlBRcceeX/R/fOFFSDNKk6h4O+Lq6TVtgQ=";
    wg_config.port = 51824;
    wg_config.persistent_keepalive = 25;

    // QUAN TRỌNG CHO VIỆC TEST:
    // - Nếu ESP32 dùng 4G: Giữ nguyên "171.244.185.125"
    // - Nếu ESP32 dùng chung WiFi công ty với Server: Đổi thành IP LAN (192.168.x.x) của Server.
    wg_config.endpoint = "171.244.185.125"; 

    if (esp_wireguard_init(&wg_config, &wg_ctx) != ESP_OK) return;
    if (esp_wireguard_connect(&wg_ctx) != ESP_OK) return;

    ESP_LOGI("WG", "Chờ WireGuard hoàn tất Handshake...");
    while (esp_wireguardif_peer_is_up(&wg_ctx) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI("WG", "WireGuard link UP! Định tuyến hoàn tất.");
}

// =======================
// MAIN
// =======================
void app_main(void)
{
    ESP_LOGI(TAG, "Khởi tạo hệ thống...");

    // Ring Buffer 64KB trên PSRAM
    // audio_ringbuf (mic -> wakenet): giữ nguyên PSRAM, không nằm trong đường I2S output nên không cần internal
size_t             ringbuf_size    = 128 * 1024;
StaticRingbuffer_t *ringbuf_struct  = heap_caps_malloc(sizeof(StaticRingbuffer_t), MALLOC_CAP_SPIRAM);
uint8_t            *ringbuf_storage = heap_caps_malloc(ringbuf_size, MALLOC_CAP_SPIRAM);
if (!ringbuf_struct || !ringbuf_storage) {
    ESP_LOGE(TAG, "Cap phat audio_ringbuf that bai (PSRAM con trong: %u byte)",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
audio_ringbuf = xRingbufferCreateStatic(ringbuf_size, RINGBUF_TYPE_BYTEBUF,
                                         ringbuf_storage, ringbuf_struct);

// playback_ringbuf (I2S output): chuyen sang internal RAM de tranh PSRAM contention voi DSI display
// giam kich thuoc xuong 48KB (~1.5 giay audio o 16kHz mono 16-bit), du cho buffer priming 400ms + du phong
size_t playback_buf_size = 48 * 1024;
StaticRingbuffer_t *playback_struct  = heap_caps_malloc(sizeof(StaticRingbuffer_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
uint8_t            *playback_storage = heap_caps_malloc(playback_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
if (!playback_struct || !playback_storage) {
    ESP_LOGE(TAG, "Cap phat playback_ringbuf that bai (Internal RAM con trong: %u byte) - AUDIO SE KHONG PHAT DUOC",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
playback_ringbuf = xRingbufferCreateStatic(playback_buf_size, RINGBUF_TYPE_BYTEBUF,
                                            playback_storage, playback_struct);

    // NVS
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    app_wifi_init();
    wireguard_start();
    websocket_init();
    display_init();
    i2s_init();

    // Model + AFE
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) { ESP_LOGE(TAG, "Không tìm thấy partition model."); return; }
    ESP_LOGI(TAG, "Model: %s", models->model_name[0]);

    afe_config_t *afe_config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!afe_config) { ESP_LOGE(TAG, "Lỗi cấu hình AFE."); return; }
    afe_config->vad_mode = VAD_MODE_4;
    afe_config->aec_init = false;

    afe_handle = esp_afe_handle_from_config(afe_config);
    if (!afe_handle) { ESP_LOGE(TAG, "Lỗi AFE handle."); return; }

    afe_data = afe_handle->create_from_config(afe_config);
    if (!afe_data) { ESP_LOGE(TAG, "Lỗi AFE data."); return; }

    afe_config_free(afe_config);

    // feed + detect cùng core 0 để AFE pipeline không bị trễ
    // detect priority cao hơn feed để fetch kịp sau mỗi lần feed
    sys_event_group = xEventGroupCreate();
    xTaskCreatePinnedToCore(feed_task,   "feed_task",   4096*4, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(detect_task, "detect_task", 4096*4, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(send_task,   "send",   4096*3, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(play_task, "play_task", 4096*3, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(keyboard_task, "keyboard_task", 4096, NULL, 3, NULL, 1);

    ESP_LOGI(TAG, "Hệ thống sẵn sàng. Chờ wake word...");
}