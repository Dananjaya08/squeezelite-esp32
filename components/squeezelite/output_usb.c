/* 
 *  Squeezelite for esp32 - USB Audio Class (UAC 1.0) Output Backend
 *
 *  (c) Sebastien 2019, Philippe G. 2019
 *  USB UAC Backend implementation for ESP32-S3
 *
 */

#include "squeezelite.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#include "equalizer.h"
#include "platform_config.h"
#include "services.h"
#include "led.h"

static const char *TAG = "output_usb";

extern struct outputstate output;
extern struct buffer *outputbuf;
extern struct buffer *streambuf;
extern u8_t *silencebuf;

#define LOCK   mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)

#define FRAME_BLOCK MAX_SILENCE_FRAMES

#define USB_HOST_TASK_PRIORITY  (CONFIG_ESP32_PTHREAD_TASK_PRIO_DEFAULT + 11)
#define USB_OUT_TASK_PRIORITY   (CONFIG_ESP32_PTHREAD_TASK_PRIO_DEFAULT + 10)
#define UAC_TASK_PRIORITY       (CONFIG_ESP32_PTHREAD_TASK_PRIO_DEFAULT + 11)

typedef enum {
    UAC_OUT_EVT_DRIVER,
    UAC_OUT_EVT_DEVICE,
} uac_out_evt_type_t;

typedef struct {
    uac_out_evt_type_t type;
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
} uac_out_event_t;

static log_level loglevel;
static volatile bool running = false;
static volatile bool ended = false;
static volatile bool s_dac_connected = false;
static volatile bool s_stream_started = false;

static u8_t *obuf = NULL;       // Internal DMA-capable RAM output buffer
static frames_t oframes = 0;
static uint32_t usb_idle_since = 0;

static QueueHandle_t s_uac_evt_queue = NULL;
static TaskHandle_t s_usb_host_task = NULL;
static TaskHandle_t s_output_usb_task = NULL;
static uac_host_device_handle_t s_spk_handle = NULL;
static uint8_t s_dac_addr = 0;
static uint8_t s_dac_iface = 0;

static int _usb_write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR, u8_t flags,
                             s32_t cross_gain_in, s32_t cross_gain_out, ISAMPLE_T **cross_ptr);
static void output_thread_usb(void *arg);
static esp_err_t start_dac_stream(uint8_t addr, uint8_t iface_num);
static void stop_dac_stream(void);

/****************************************************************************************
 * USB Idle sleeper callback
 */
static uint32_t usb_idle_callback(void) {
    return output.state <= OUTPUT_STOPPED ? pdTICKS_TO_MS(xTaskGetTickCount()) - usb_idle_since : 0;
}

/****************************************************************************************
 * UAC Event Callbacks
 */
static void uac_device_cb(uac_host_device_handle_t handle, const uac_host_device_event_t event, void *arg) {
    if (!s_uac_evt_queue) return;
    uac_out_event_t evt = {
        .type = UAC_OUT_EVT_DEVICE,
        .device_evt.handle = handle,
        .device_evt.event = event,
        .device_evt.arg = arg
    };
    xQueueSend(s_uac_evt_queue, &evt, 0);
}

static void uac_driver_cb(uint8_t addr, uint8_t iface_num, const uac_host_driver_event_t event, void *arg) {
    if (!s_uac_evt_queue) return;
    uac_out_event_t evt = {
        .type = UAC_OUT_EVT_DRIVER,
        .driver_evt.addr = addr,
        .driver_evt.iface_num = iface_num,
        .driver_evt.event = event,
        .driver_evt.arg = arg
    };
    xQueueSend(s_uac_evt_queue, &evt, 0);
}

/****************************************************************************************
 * USB Host Daemon Task
 */
static void usb_host_lib_task(void *arg) {
    TaskHandle_t calling_task = (TaskHandle_t)arg;
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };

    esp_err_t ret = usb_host_install(&host_config);
    if (ret != ESP_OK) {
        LOG_ERROR("usb_host_install failed: %s", esp_err_to_name(ret));
        xTaskNotifyGive(calling_task);
        vTaskDelete(NULL);
        return;
    }

    LOG_INFO("USB Host installed for Squeezelite UAC backend");
    xTaskNotifyGive(calling_task);

    while (running) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(100), &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
            break;
        }
    }

    LOG_INFO("Uninstalling USB Host Library");
    vTaskDelay(pdMS_TO_TICKS(50));
    usb_host_uninstall();
    s_usb_host_task = NULL;
    vTaskDelete(NULL);
}

/****************************************************************************************
 * Start DAC UAC Stream (44.1kHz / 16-bit / Stereo)
 */
static esp_err_t start_dac_stream(uint8_t addr, uint8_t iface_num) {
    if (s_spk_handle != NULL) {
        stop_dac_stream();
    }

    const uac_host_device_config_t dev_config = {
        .addr = addr,
        .iface_num = iface_num,
        .buffer_size = 16000,
        .buffer_threshold = 4000,
        .callback = uac_device_cb,
        .callback_arg = NULL,
    };

    esp_err_t ret = uac_host_device_open(&dev_config, &s_spk_handle);
    if (ret != ESP_OK) {
        LOG_ERROR("uac_host_device_open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uac_host_dev_info_t dev_info;
    if (uac_host_get_device_info(s_spk_handle, &dev_info) == ESP_OK) {
        LOG_INFO("UAC DAC connected: iface=%u, alts=%u", dev_info.iface_num, dev_info.iface_alt_num);
        uac_host_printf_device_param(s_spk_handle);
    }

    uac_host_stream_config_t stm_config = {
        .channels = 2,
        .bit_resolution = 16,
        .sample_freq = 44100,
        .flags = 0,
    };

    ret = uac_host_device_start(s_spk_handle, &stm_config);
    if (ret != ESP_OK) {
        LOG_ERROR("uac_host_device_start failed: %s", esp_err_to_name(ret));
        uac_host_device_close(s_spk_handle);
        s_spk_handle = NULL;
        return ret;
    }

    uac_host_device_set_mute(s_spk_handle, false);
    s_stream_started = true;
    s_dac_connected = true;
    LOG_INFO("UAC audio stream started at 44.1kHz, 16-bit stereo");

    return ESP_OK;
}

static void stop_dac_stream(void) {
    s_stream_started = false;
    s_dac_connected = false;
    if (s_spk_handle) {
        LOG_INFO("Stopping UAC stream");
        uac_host_device_stop(s_spk_handle);
        uac_host_device_close(s_spk_handle);
        s_spk_handle = NULL;
    }
}

/****************************************************************************************
 * Init USB Output Backend
 */
void output_init_usb(log_level level, char *device, unsigned output_buf_size, char *params, 
                     unsigned rates[], unsigned rate_delay, unsigned idle) {
    loglevel = level;
    LOG_INFO("Initializing USB Audio Class backend for device '%s'", device);

    usb_idle_since = pdTICKS_TO_MS(xTaskGetTickCount());
    services_sleep_setsleeper(usb_idle_callback);

    led_blink(LED_GREEN, 200, 1000);

    // Target ONLY 44.1kHz / 16-bit stereo for initial version
    rates[0] = 44100;
    rates[1] = 0;
    output.current_sample_rate = 44100;

    // Allocate internal DMA-capable RAM output staging buffer
    // Squeezelite uses FRAME_BLOCK frames
    size_t obuf_size = FRAME_BLOCK * BYTES_PER_FRAME;
    obuf = (u8_t *)heap_caps_malloc(obuf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!obuf) {
        LOG_ERROR("Failed to allocate %u bytes internal DMA RAM for USB obuf!", obuf_size);
        return;
    }
    memset(obuf, 0, obuf_size);

    running = true;
    ended = false;
    output.write_cb = &_usb_write_frames;

    s_uac_evt_queue = xQueueCreate(16, sizeof(uac_out_event_t));

    // 1. Start USB host library task
    xTaskCreatePinnedToCore(usb_host_lib_task, "usb_lib", 4096,
                            (void *)xTaskGetCurrentTaskHandle(),
                            USB_HOST_TASK_PRIORITY, &s_usb_host_task, 0);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // 2. Install UAC Host Class Driver
    uac_host_driver_config_t uac_config = {
        .create_background_task = true,
        .task_priority = UAC_TASK_PRIORITY,
        .stack_size = 4096,
        .core_id = 0,
        .callback = uac_driver_cb,
        .callback_arg = NULL
    };
    esp_err_t ret = uac_host_install(&uac_config);
    if (ret != ESP_OK) {
        LOG_ERROR("uac_host_install failed: %s", esp_err_to_name(ret));
    } else {
        LOG_INFO("UAC host class driver installed");
    }

    equalizer_set_samplerate(output.current_sample_rate);

    // 3. Start dedicated output thread
    xTaskCreatePinnedToCore(output_thread_usb, "output_usb", 4096,
                            NULL, USB_OUT_TASK_PRIORITY, &s_output_usb_task, 0);

    LOG_INFO("USB Audio Class output backend initialized.");
}

/****************************************************************************************
 * Close USB Output Backend
 */
void output_close_usb(void) {
    LOG_INFO("Closing USB output backend");
    LOCK;
    running = false;
    UNLOCK;

    while (!ended) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    stop_dac_stream();
    uac_host_uninstall();

    if (s_uac_evt_queue) {
        vQueueDelete(s_uac_evt_queue);
        s_uac_evt_queue = NULL;
    }

    if (obuf) {
        free(obuf);
        obuf = NULL;
    }

    equalizer_close();
    LOG_INFO("USB output backend closed.");
}

/****************************************************************************************
 * Volume Control
 */
bool output_volume_usb(unsigned left, unsigned right) {
    if (!s_spk_handle || !s_stream_started) {
        return false; // Fall back to Squeezelite software volume
    }

    // Map 16-bit volume (0..65536) to 0..100%
    uint32_t avg = (left + right) / 2;
    uint32_t vol_pct = (avg * 100) / 65536;
    if (vol_pct > 100) vol_pct = 100;

    esp_err_t err = uac_host_device_set_volume(s_spk_handle, (uint8_t)vol_pct);
    if (err == ESP_OK) {
        LOG_DEBUG("Set UAC hardware volume to %u%%", vol_pct);
        return true;
    }

    return false; // Fall back to software volume if Feature Unit volume unsupported
}

/****************************************************************************************
 * Write Frames Callback (called from _output_frames with outputbuf locked)
 */
static int _usb_write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR, u8_t flags,
                             s32_t cross_gain_in, s32_t cross_gain_out, ISAMPLE_T **cross_ptr) {
    if (!silence) {
        if (output.fade == FADE_ACTIVE && output.fade_dir == FADE_CROSS && *cross_ptr) {
            _apply_cross(outputbuf, out_frames, cross_gain_in, cross_gain_out, cross_ptr);
        }

        _apply_gain(outputbuf, out_frames, gainL, gainR, flags);

#if BYTES_PER_FRAME == 4
        // 16-bit stereo native
        memcpy(obuf + oframes * BYTES_PER_FRAME, outputbuf->readp, out_frames * BYTES_PER_FRAME);
#else
        // 32-bit internal format -> downsample to 16-bit for DAC
        frames_t count = out_frames;
        s32_t *_iptr = (s32_t *)outputbuf->readp;
        s16_t *_optr = (s16_t *)(obuf + oframes * 4);
        while (count--) {
            *_optr++ = (s16_t)(*_iptr++ >> 16);
            *_optr++ = (s16_t)(*_iptr++ >> 16);
        }
#endif
    } else {
        memcpy(obuf + oframes * BYTES_PER_FRAME, silencebuf, out_frames * BYTES_PER_FRAME);
    }

    if (silence || _buf_used(outputbuf) > BYTES_PER_FRAME * output.current_sample_rate / 2) {
        output_visu_export(obuf + oframes * BYTES_PER_FRAME, out_frames, output.current_sample_rate, silence, (gainL + gainR) / 2);
    }

    oframes += out_frames;
    return out_frames;
}

/****************************************************************************************
 * Main USB Output Thread
 */
static void output_thread_usb(void *arg) {
    frames_t iframes = FRAME_BLOCK;
    output_state state = OUTPUT_OFF - 1;
    uac_out_event_t evt;

    LOG_INFO("USB output thread started");

    while (running) {
        // 1. Process asynchronous UAC driver / device events
        while (s_uac_evt_queue && xQueueReceive(s_uac_evt_queue, &evt, 0)) {
            if (evt.type == UAC_OUT_EVT_DRIVER) {
                if (evt.driver_evt.event == UAC_HOST_DRIVER_EVENT_TX_CONNECTED) {
                    s_dac_addr = evt.driver_evt.addr;
                    s_dac_iface = evt.driver_evt.iface_num;
                    LOG_INFO("UAC DAC connected on addr %u, iface %u", s_dac_addr, s_dac_iface);
                    start_dac_stream(s_dac_addr, s_dac_iface);
                }
            } else if (evt.type == UAC_OUT_EVT_DEVICE) {
                if (evt.device_evt.event == UAC_HOST_DRIVER_EVENT_DISCONNECTED ||
                    evt.device_evt.event == UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) {
                    LOG_WARN("UAC DAC disconnected / transfer error");
                    stop_dac_stream();
                }
            }
        }

        LOCK;

        // Manage state changes & LEDs
        if (state != output.state) {
            LOG_INFO("USB output state transition to %d", output.state);
            if (output.state == OUTPUT_OFF) {
                led_blink(LED_GREEN, 100, 2500);
            } else if (output.state == OUTPUT_STOPPED) {
                usb_idle_since = pdTICKS_TO_MS(xTaskGetTickCount());
                led_blink(LED_GREEN, 200, 1000);
            } else if (output.state == OUTPUT_RUNNING) {
                led_on(LED_GREEN);
            }
        }
        state = output.state;

        if (output.state == OUTPUT_OFF) {
            UNLOCK;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        oframes = 0;
        output.updated = gettime_ms();
        output.frames_played_dmp = output.frames_played;
        output.device_frames = 0; // UAC driver internally manages ring buffer

        // Produce frames from Squeezelite ring buffer
        _output_frames(iframes);
        output.frames_in_process = oframes;

        UNLOCK;

        // Run software equalizer
        if (oframes > 0) {
            equalizer_process(obuf, oframes * BYTES_PER_FRAME);

            // Stream to USB DAC if connected
            if (s_dac_connected && s_stream_started && s_spk_handle) {
                // 16-bit stereo = 4 bytes per frame sent over USB
                size_t bytes_to_write = oframes * 4;
                esp_err_t err = uac_host_device_write(s_spk_handle, obuf, bytes_to_write, pdMS_TO_TICKS(500));
                if (err != ESP_OK) {
                    LOG_WARN("uac_host_device_write failed (%s), frames dropped", esp_err_to_name(err));
                }
            } else {
                // DAC not connected yet: simulate playback delay so we don't spin CPU
                vTaskDelay(pdMS_TO_TICKS((oframes * 1000) / 44100));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    ended = true;
    vTaskDelete(NULL);
}

/****************************************************************************************
 * Standalone USB Audio Test Command
 */
#include <math.h>
#include "esp_console.h"
#include "argtable3/argtable3.h"

static struct {
    struct arg_int *duration;
    struct arg_int *freq;
    struct arg_end *end;
} usb_test_args;

static int do_usb_test_cmd(int argc, char **argv) {
    int nerrors = arg_parse(argc, argv, (void **)&usb_test_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, usb_test_args.end, argv[0]);
        return 1;
    }

    uint32_t duration_sec = usb_test_args.duration->count > 0 ? usb_test_args.duration->ival[0] : 10;
    float freq_hz = usb_test_args.freq->count > 0 ? (float)usb_test_args.freq->ival[0] : 440.0f;
    if (duration_sec == 0) duration_sec = 10;
    if (freq_hz <= 0.0f) freq_hz = 440.0f;

    LOG_INFO("==================================================");
    LOG_INFO("USB Audio Test: %.1f Hz sine wave, %u seconds", freq_hz, duration_sec);
    LOG_INFO("Target format: 44.1kHz, 16-bit signed PCM, Stereo");
    LOG_INFO("==================================================");

    if (!s_dac_connected || !s_spk_handle || !s_stream_started) {
        LOG_WARN("DAC stream is not currently active. Ensure DAC is connected via USB Host port.");
        return 1;
    }

    // Allocate 100ms test buffer in internal DMA RAM: 4410 frames * 4 bytes = 17640 bytes
    const size_t test_frames = 4410;
    const size_t buf_bytes = test_frames * 4;
    int16_t *test_buf = (int16_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!test_buf) {
        LOG_ERROR("Failed to allocate test buffer in internal DMA RAM");
        return 1;
    }

    float phase_step = (2.0f * (float)M_PI * freq_hz) / 44100.0f;
    float phase = 0.0f;
    for (size_t i = 0; i < test_frames; i++) {
        int16_t sample = (int16_t)(sinf(phase) * 0.5f * 32767.0f);
        test_buf[i * 2]     = sample;
        test_buf[i * 2 + 1] = sample;
        phase += phase_step;
        if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
    }

    uint32_t total_chunks = (duration_sec * 44100) / test_frames;
    for (uint32_t c = 0; c < total_chunks; c++) {
        esp_err_t err = uac_host_device_write(s_spk_handle, (uint8_t *)test_buf, buf_bytes, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            LOG_ERROR("uac_host_device_write failed (%s)", esp_err_to_name(err));
            break;
        }
    }

    free(test_buf);
    LOG_INFO("USB Audio Test Finished.");
    return 0;
}

void register_usb_test_cmd(void) {
    usb_test_args.duration = arg_int0("d", "duration", "<sec>", "Test duration in seconds (default: 10)");
    usb_test_args.freq     = arg_int0("f", "freq", "<Hz>", "Sine frequency in Hz (default: 440)");
    usb_test_args.end      = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "usbtest",
        .help = "Stream 44.1kHz 16-bit sine wave to USB DAC",
        .hint = NULL,
        .func = &do_usb_test_cmd,
        .argtable = &usb_test_args
    };
    esp_console_cmd_register(&cmd);
}

#else // !CONFIG_IDF_TARGET_ESP32S3

void register_usb_test_cmd(void) {
    // Stub on non-ESP32S3 targets
}

#endif // CONFIG_IDF_TARGET_ESP32S3
