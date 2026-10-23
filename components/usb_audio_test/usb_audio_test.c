/*
 * USB Audio Class (UAC 1.0) Standalone Test Component for ESP32-S3
 *
 * Squeezelite-ESP32
 */

#include "usb_audio_test.h"
#include "esp_log.h"

#ifdef CONFIG_IDF_TARGET_ESP32S3

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_heap_caps.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"

static const char *TAG = "usb_audio_test";

#define USB_HOST_TASK_PRIORITY   5
#define UAC_TASK_PRIORITY        5
#define SINE_BUFFER_FRAMES       4410 // 100ms chunk at 44.1kHz

typedef enum {
    UAC_TEST_EVT_DRIVER,
    UAC_TEST_EVT_DEVICE,
} test_event_type_t;

typedef struct {
    test_event_type_t type;
    union {
        struct {
            uint8_t addr;
            uint8_t iface_num;
            uac_host_driver_event_t event;
            void *arg;
        } driver_evt;
        struct {
            uac_host_device_handle_t handle;
            uac_host_device_event_t event;
            void *arg;
        } device_evt;
    };
} test_event_t;

static QueueHandle_t s_test_evt_queue = NULL;
static TaskHandle_t s_usb_host_task_handle = NULL;
static volatile bool s_usb_host_running = false;
static uac_host_device_handle_t s_spk_handle = NULL;

static void uac_device_event_cb(uac_host_device_handle_t uac_device_handle,
                                const uac_host_device_event_t event, void *arg)
{
    if (!s_test_evt_queue) return;
    test_event_t evt = {
        .type = UAC_TEST_EVT_DEVICE,
        .device_evt.handle = uac_device_handle,
        .device_evt.event = event,
        .device_evt.arg = arg
    };
    xQueueSend(s_test_evt_queue, &evt, 0);
}

static void uac_driver_event_cb(uint8_t addr, uint8_t iface_num,
                                const uac_host_driver_event_t event, void *arg)
{
    if (!s_test_evt_queue) return;
    test_event_t evt = {
        .type = UAC_TEST_EVT_DRIVER,
        .driver_evt.addr = addr,
        .driver_evt.iface_num = iface_num,
        .driver_evt.event = event,
        .driver_evt.arg = arg
    };
    xQueueSend(s_test_evt_queue, &evt, 0);
}

static void usb_host_lib_task(void *arg)
{
    TaskHandle_t calling_task = (TaskHandle_t)arg;
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };

    esp_err_t ret = usb_host_install(&host_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(ret));
        s_usb_host_running = false;
        xTaskNotifyGive(calling_task);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "USB Host Library installed successfully");
    s_usb_host_running = true;
    xTaskNotifyGive(calling_task);

    while (s_usb_host_running) {
        uint32_t event_flags = 0;
        ret = usb_host_lib_handle_events(pdMS_TO_TICKS(100), &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGI(TAG, "USB Host: all clients deregistered");
            usb_host_device_free_all();
            break;
        }
    }

    ESP_LOGI(TAG, "Uninstalling USB Host Library");
    vTaskDelay(pdMS_TO_TICKS(20));
    usb_host_uninstall();
    s_usb_host_task_handle = NULL;
    vTaskDelete(NULL);
}

static void generate_sine_16bit_stereo(int16_t *buffer, size_t frames, uint32_t rate, float freq, float amplitude)
{
    float phase_step = (2.0f * (float)M_PI * freq) / (float)rate;
    float phase = 0.0f;

    for (size_t i = 0; i < frames; i++) {
        int16_t sample = (int16_t)(sinf(phase) * amplitude * 32767.0f);
        buffer[i * 2]     = sample; // Left
        buffer[i * 2 + 1] = sample; // Right
        phase += phase_step;
        if (phase >= 2.0f * (float)M_PI) {
            phase -= 2.0f * (float)M_PI;
        }
    }
}

esp_err_t run_usb_audio_test(uint32_t duration_sec, float freq_hz)
{
    if (duration_sec == 0) duration_sec = 10;
    if (freq_hz <= 0.0f) freq_hz = 440.0f;

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "Starting USB Audio Class 1.0 Standalone Test");
    ESP_LOGI(TAG, "Parameters: Duration = %u s, Frequency = %.1f Hz", duration_sec, freq_hz);
    ESP_LOGI(TAG, "Target format: 44.1kHz, 16-bit signed PCM, Stereo");
    ESP_LOGI(TAG, "==================================================");

    s_test_evt_queue = xQueueCreate(10, sizeof(test_event_t));
    if (!s_test_evt_queue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_ERR_NO_MEM;
    }

    // 1. Install USB Host Library in dedicated task
    xTaskCreatePinnedToCore(usb_host_lib_task, "usb_lib", 4096,
                            (void *)xTaskGetCurrentTaskHandle(),
                            USB_HOST_TASK_PRIORITY, &s_usb_host_task_handle, 0);

    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!s_usb_host_running) {
        ESP_LOGE(TAG, "USB Host task failed to start");
        vQueueDelete(s_test_evt_queue);
        s_test_evt_queue = NULL;
        return ESP_FAIL;
    }

    // 2. Install UAC Host driver
    uac_host_driver_config_t uac_driver_config = {
        .create_background_task = true,
        .task_priority = UAC_TASK_PRIORITY,
        .stack_size = 4096,
        .core_id = 0,
        .callback = uac_driver_event_cb,
        .callback_arg = NULL
    };

    esp_err_t ret = uac_host_install(&uac_driver_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_install failed: %s", esp_err_to_name(ret));
        s_usb_host_running = false;
        vQueueDelete(s_test_evt_queue);
        s_test_evt_queue = NULL;
        return ret;
    }
    ESP_LOGI(TAG, "UAC Host driver installed. Waiting for DAC connection...");

    // 3. Wait for DAC connection event (up to 10 seconds)
    test_event_t evt;
    bool connected = false;
    uint8_t dac_addr = 0;
    uint8_t dac_iface = 0;

    TickType_t start_tick = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start_tick) < pdMS_TO_TICKS(10000)) {
        if (xQueueReceive(s_test_evt_queue, &evt, pdMS_TO_TICKS(500))) {
            if (evt.type == UAC_TEST_EVT_DRIVER &&
                evt.driver_evt.event == UAC_HOST_DRIVER_EVENT_TX_CONNECTED) {
                dac_addr = evt.driver_evt.addr;
                dac_iface = evt.driver_evt.iface_num;
                connected = true;
                ESP_LOGI(TAG, "Found UAC Speaker/DAC: Addr = %u, Interface = %u", dac_addr, dac_iface);
                break;
            }
        }
    }

    if (!connected) {
        ESP_LOGE(TAG, "Timeout: No USB Audio (TX) device detected on USB OTG port!");
        ESP_LOGE(TAG, "Please check connection: ESP32-S3 USB OTG -> Hub -> DAC");
        uac_host_uninstall();
        s_usb_host_running = false;
        vQueueDelete(s_test_evt_queue);
        s_test_evt_queue = NULL;
        return ESP_ERR_TIMEOUT;
    }

    // 4. Open UAC device interface
    const uac_host_device_config_t dev_config = {
        .addr = dac_addr,
        .iface_num = dac_iface,
        .buffer_size = 16000,
        .buffer_threshold = 4000,
        .callback = uac_device_event_cb,
        .callback_arg = NULL,
    };

    ret = uac_host_device_open(&dev_config, &s_spk_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_device_open failed: %s", esp_err_to_name(ret));
        uac_host_uninstall();
        s_usb_host_running = false;
        vQueueDelete(s_test_evt_queue);
        s_test_evt_queue = NULL;
        return ret;
    }

    // 5. Query and log device information
    uac_host_dev_info_t dev_info;
    if (uac_host_get_device_info(s_spk_handle, &dev_info) == ESP_OK) {
        ESP_LOGI(TAG, "--- USB DAC Device Information ---");
        ESP_LOGI(TAG, "Interface number: %u, Alternate settings: %u", dev_info.iface_num, dev_info.iface_alt_num);
        uac_host_printf_device_param(s_spk_handle);
    }

    // 6. Inspect alternate settings and match 44.1kHz
    uac_host_dev_alt_param_t chosen_alt;
    memset(&chosen_alt, 0, sizeof(chosen_alt));
    bool found_44k = false;

    for (uint8_t alt = 1; alt <= dev_info.iface_alt_num; alt++) {
        uac_host_dev_alt_param_t alt_param;
        if (uac_host_get_device_alt_param(s_spk_handle, alt, &alt_param) == ESP_OK) {
            ESP_LOGI(TAG, "Alt %u: channels=%u, bits=%u, subframe=%u, freq_type=%u",
                     alt, alt_param.channels, alt_param.bit_resolution, alt_param.subframe_size, alt_param.sample_freq_type);

            if (alt_param.sample_freq_type > 0) {
                for (int i = 0; i < alt_param.sample_freq_type; i++) {
                    ESP_LOGI(TAG, "  Supported rate: %u Hz", alt_param.sample_freq[i]);
                    if (alt_param.sample_freq[i] == 44100 && alt_param.bit_resolution == 16 && alt_param.channels == 2) {
                        chosen_alt = alt_param;
                        found_44k = true;
                    }
                }
            } else {
                ESP_LOGI(TAG, "  Continuous rate: %u - %u Hz", alt_param.sample_freq_lower, alt_param.sample_freq_upper);
                if (44100 >= alt_param.sample_freq_lower && 44100 <= alt_param.sample_freq_upper) {
                    chosen_alt = alt_param;
                    found_44k = true;
                }
            }
        }
    }

    if (!found_44k) {
        ESP_LOGW(TAG, "44.1kHz/16-bit/stereo alt setting not explicitly matched, querying alt 1 default");
        uac_host_get_device_alt_param(s_spk_handle, 1, &chosen_alt);
    }

    // 7. Start stream with 44.1kHz, 16-bit, stereo
    uac_host_stream_config_t stm_config = {
        .channels = 2,
        .bit_resolution = 16,
        .sample_freq = 44100,
        .flags = 0,
    };

    ESP_LOGI(TAG, "Starting audio stream: rate=%" PRIu32 " Hz, channels=%u, bits=%u",
             stm_config.sample_freq, stm_config.channels, stm_config.bit_resolution);

    ret = uac_host_device_start(s_spk_handle, &stm_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_device_start failed: %s", esp_err_to_name(ret));
        uac_host_device_close(s_spk_handle);
        s_spk_handle = NULL;
        uac_host_uninstall();
        s_usb_host_running = false;
        vQueueDelete(s_test_evt_queue);
        s_test_evt_queue = NULL;
        return ret;
    }

    // Unmute and set volume
    uac_host_device_set_mute(s_spk_handle, false);
    uac_host_device_set_volume(s_spk_handle, 70);

    // 8. Allocate internal DMA-capable buffer for 100ms of stereo audio
    // 4410 frames * 2 channels * 2 bytes = 17640 bytes
    size_t chunk_bytes = SINE_BUFFER_FRAMES * 2 * sizeof(int16_t);
    int16_t *sine_buf = (int16_t *)heap_caps_malloc(chunk_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!sine_buf) {
        ESP_LOGE(TAG, "Failed to allocate internal DMA buffer of %u bytes", chunk_bytes);
        uac_host_device_stop(s_spk_handle);
        uac_host_device_close(s_spk_handle);
        s_spk_handle = NULL;
        uac_host_uninstall();
        s_usb_host_running = false;
        vQueueDelete(s_test_evt_queue);
        s_test_evt_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    generate_sine_16bit_stereo(sine_buf, SINE_BUFFER_FRAMES, 44100, freq_hz, 0.5f);
    ESP_LOGI(TAG, "Generated pure sine tone (%.1f Hz, 50%% volume). Streaming for %u seconds...", freq_hz, duration_sec);

    uint32_t total_chunks = (duration_sec * 44100) / SINE_BUFFER_FRAMES;
    uint32_t streamed_chunks = 0;
    TickType_t stream_start = xTaskGetTickCount();

    for (uint32_t c = 0; c < total_chunks; c++) {
        ret = uac_host_device_write(s_spk_handle, (uint8_t *)sine_buf, chunk_bytes, 1000);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "uac_host_device_write failed at chunk %u: %s", c, esp_err_to_name(ret));
            break;
        }
        streamed_chunks++;

        // Process any async device events
        if (xQueueReceive(s_test_evt_queue, &evt, 0)) {
            if (evt.type == UAC_TEST_EVT_DEVICE &&
                evt.device_evt.event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
                ESP_LOGW(TAG, "USB DAC disconnected during playback!");
                break;
            }
        }
    }

    uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - stream_start);
    ESP_LOGI(TAG, "Streaming completed: %u chunks written (~%u ms playback)", streamed_chunks, elapsed_ms);

    // 9. Teardown
    free(sine_buf);
    ESP_LOGI(TAG, "Stopping UAC device...");
    uac_host_device_stop(s_spk_handle);
    uac_host_device_close(s_spk_handle);
    s_spk_handle = NULL;

    ESP_LOGI(TAG, "Uninstalling UAC host driver...");
    uac_host_uninstall();

    s_usb_host_running = false;
    vTaskDelay(pdMS_TO_TICKS(50));

    vQueueDelete(s_test_evt_queue);
    s_test_evt_queue = NULL;

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "USB Audio Standalone Test Finished Successfully!");
    ESP_LOGI(TAG, "==================================================");

    return ESP_OK;
}

static struct {
    struct arg_int *duration;
    struct arg_int *freq;
    struct arg_end *end;
} usb_test_args;

static int do_usb_test_cmd(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&usb_test_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, usb_test_args.end, argv[0]);
        return 1;
    }

    uint32_t duration = usb_test_args.duration->count > 0 ? usb_test_args.duration->ival[0] : 10;
    float freq = usb_test_args.freq->count > 0 ? (float)usb_test_args.freq->ival[0] : 440.0f;

    esp_err_t err = run_usb_audio_test(duration, freq);
    return (err == ESP_OK) ? 0 : 1;
}

void register_usb_test_cmd(void)
{
    usb_test_args.duration = arg_int0("d", "duration", "<seconds>", "Test duration in seconds (default: 10)");
    usb_test_args.freq     = arg_int0("f", "freq", "<Hz>", "Sine tone frequency in Hz (default: 440)");
    usb_test_args.end      = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "usbtest",
        .help = "Test USB DAC dongle with a 44.1kHz 16-bit stereo sine wave",
        .hint = NULL,
        .func = &do_usb_test_cmd,
        .argtable = &usb_test_args
    };
    esp_console_cmd_register(&cmd);
}

#else // Non-ESP32S3 stubs

esp_err_t run_usb_audio_test(uint32_t duration_sec, float freq_hz)
{
    ESP_LOGW(TAG, "USB Audio Class host is only supported on ESP32-S3");
    return ESP_ERR_NOT_SUPPORTED;
}

void register_usb_test_cmd(void)
{
    // No-op on classic ESP32
}

#endif
