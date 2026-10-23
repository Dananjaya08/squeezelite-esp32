#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run standalone USB Audio Class 1.0 test.
 *
 * Enumerates USB Host, waits for UAC DAC (through hub or direct),
 * logs descriptor and alternate setting details, opens 44.1kHz/16-bit stereo stream,
 * generates a 440Hz sine wave tone, streams for the requested duration,
 * tests volume if supported, and tears down cleanly.
 *
 * @param duration_sec Test duration in seconds (default: 10)
 * @param freq_hz Sine wave frequency in Hz (default: 440)
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t run_usb_audio_test(uint32_t duration_sec, float freq_hz);

/**
 * @brief Register the 'usbtest' console command.
 */
void register_usb_test_cmd(void);

#ifdef __cplusplus
}
#endif
