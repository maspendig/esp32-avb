/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the audio output system
 *
 * Initializes I2S driver and ES8311 codec for audio output.
 *
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t audio_output_init(void);

/**
 * @brief Start the audio output task
 *
 * Creates a task that generates and plays a musical scale.
 *
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t audio_output_start(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_OUTPUT_H

