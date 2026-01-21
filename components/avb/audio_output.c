/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include "audio_output.h"
#include "config.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "es8311.h"
#include "esp_check.h"
#include "esp_log.h"

static const char* TAG = "audio_output";

static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;


static esp_err_t i2s_driver_init(void)
{
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true; // Auto clear the legacy data in the DMA buffer
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));

  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_MCK_IO,
      .bclk = I2S_BCK_IO,
      .ws = I2S_WS_IO,
      .dout = I2S_DO_IO,
      .din = I2S_DI_IO,
      .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv = false,
      },
    },
  };
  std_cfg.clk_cfg.mclk_multiple = AUDIO_MCLK_MULTIPLE;

  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
  ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));

  ESP_LOGI(TAG, "I2S driver initialized successfully");
  return ESP_OK;
}

esp_err_t audio_output_init(void)
{
  ESP_LOGI(TAG, "Initializing audio output system");
  ESP_LOGI(TAG, "Sample rate: %d Hz", AUDIO_SAMPLE_RATE);
  ESP_LOGI(TAG, "MCLK multiple: %d", AUDIO_MCLK_MULTIPLE);

  /* Initialize I2S peripheral */
  if (i2s_driver_init() != ESP_OK)
  {
    ESP_LOGE(TAG, "I2S driver init failed");
    return ESP_FAIL;
  }

  /* Initialize I2C peripheral and config ES8311 codec by I2C */
  if (es8311_codec_init(&tx_handle, &rx_handle) != ESP_OK)
  {
    ESP_LOGE(TAG, "ES8311 codec init failed");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Audio output system initialized successfully");
  return ESP_OK;
}

esp_err_t audio_output_write(const void* data, size_t size)
{
  if (!tx_handle || !data || size == 0)
  {
    return ESP_FAIL;
  }

  size_t bytes_written = 0;
  esp_err_t ret = i2s_channel_write(tx_handle, data, size, &bytes_written, 0);
  if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT)
  {
    ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
    return ESP_FAIL;
  }

  return ESP_OK;
}

