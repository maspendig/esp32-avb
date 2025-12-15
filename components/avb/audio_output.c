/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "audio_output.h"
#include "config.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_vol.h"
#include "esp_check.h"
#include "esp_log.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char* TAG = "audio_output";
static const char err_reason[][30] = {
  "input param is invalid",
  "operation timeout"
};

static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;

/* Audio configuration based on AVB config and ES8311 example */
#define AUDIO_SAMPLE_RATE       CONFIG_SAMPLING_RATE
#define AUDIO_MCLK_MULTIPLE     384  // For 24-bit compatibility
#define AUDIO_VOICE_VOLUME      60   // 0-100
#define AUDIO_RECV_BUF_SIZE     2400

/* I2C configuration for ESP32-P4-Function-EV-Board */
#define I2C_NUM                 0
#define I2C_SCL_IO              GPIO_NUM_8
#define I2C_SDA_IO              GPIO_NUM_7

/* I2S configuration for ESP32-P4-Function-EV-Board */
#define I2S_NUM                 0
#define I2S_MCK_IO              GPIO_NUM_13
#define I2S_BCK_IO              GPIO_NUM_12
#define I2S_WS_IO               GPIO_NUM_10
#define I2S_DO_IO               GPIO_NUM_9
#define I2S_DI_IO               GPIO_NUM_11

/* PA control GPIO */
#define PA_CTRL_IO              GPIO_NUM_53

/* Sine wave generation parameters */
#define BASE_FREQ_HZ            440.0f  // Base frequency (A4)
#define SINE_AMPLITUDE          16000   // Amplitude for 16-bit audio
#define BUFFER_SIZE             1024    // Buffer size in samples
#define NOTE_DURATION_MS        300     // Duration of each note in milliseconds

/* Musical scale frequencies (using equal temperament, 440Hz as A)
 * C, D, E, F, G, A, B, C then back down */
static const float scale_frequencies[] = {
  /* Ascending C4 to C5 */
  261.6256f, /* C4 */
  293.6648f, /* D4 */
  329.6276f, /* E4 */
  349.2282f, /* F4 */
  392.0000f, /* G4 */
  440.0000f, /* A4 */
  493.8833f, /* B4 */
  523.2511f, /* C5 */
  /* Descending back to C4 */
  493.8833f, /* B4 */
  440.0000f, /* A4 */
  392.0000f, /* G4 */
  349.2282f, /* F4 */
  329.6276f, /* E4 */
  293.6648f, /* D4 */
};
#define SCALE_LENGTH (sizeof(scale_frequencies) / sizeof(scale_frequencies[0]))

static esp_err_t es8311_codec_init(void)
{
  /* Initialize I2C peripheral */
  i2c_master_bus_handle_t i2c_bus_handle = NULL;
  i2c_master_bus_config_t i2c_mst_cfg = {
    .i2c_port = I2C_NUM,
    .sda_io_num = I2C_SDA_IO,
    .scl_io_num = I2C_SCL_IO,
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
    /* Pull-up internally for no external pull-up case.
        Suggest to use external pull-up to ensure a strong enough pull-up. */
    .flags.enable_internal_pullup = true,
  };
  ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_cfg, &i2c_bus_handle));

  /* Create control interface with I2C bus handle */
  audio_codec_i2c_cfg_t i2c_cfg = {
    .port = I2C_NUM,
    .addr = ES8311_CODEC_DEFAULT_ADDR,
    .bus_handle = i2c_bus_handle,
  };
  const audio_codec_ctrl_if_t* ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
  if (!ctrl_if)
  {
    ESP_LOGE(TAG, "Failed to create I2C control interface");
    return ESP_FAIL;
  }

  /* Create data interface with I2S bus handle */
  audio_codec_i2s_cfg_t i2s_cfg = {
    .port = I2S_NUM,
    .rx_handle = rx_handle,
    .tx_handle = tx_handle,
  };
  const audio_codec_data_if_t* data_if = audio_codec_new_i2s_data(&i2s_cfg);
  if (!data_if)
  {
    ESP_LOGE(TAG, "Failed to create I2S data interface");
    return ESP_FAIL;
  }

  /* Create ES8311 interface handle */
  const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();
  if (!gpio_if)
  {
    ESP_LOGE(TAG, "Failed to create GPIO interface");
    return ESP_FAIL;
  }

  es8311_codec_cfg_t es8311_cfg = {
    .ctrl_if = ctrl_if,
    .gpio_if = gpio_if,
    .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
    .master_mode = false,
    .use_mclk = I2S_MCK_IO >= 0,
    .pa_pin = PA_CTRL_IO,
    .pa_reverted = false,
    .hw_gain = {
      .pa_voltage = 5.0,
      .codec_dac_voltage = 3.3,
    },
    .mclk_div = AUDIO_MCLK_MULTIPLE,
  };
  const audio_codec_if_t* es8311_if = es8311_codec_new(&es8311_cfg);
  if (!es8311_if)
  {
    ESP_LOGE(TAG, "Failed to create ES8311 codec interface");
    return ESP_FAIL;
  }

  /* Create the top codec handle with ES8311 interface handle and data interface */
  esp_codec_dev_cfg_t dev_cfg = {
    .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
    .codec_if = es8311_if,
    .data_if = data_if,
  };
  esp_codec_dev_handle_t codec_handle = esp_codec_dev_new(&dev_cfg);
  if (!codec_handle)
  {
    ESP_LOGE(TAG, "Failed to create codec device");
    return ESP_FAIL;
  }

  /* Specify the sample configurations and open the device */
  esp_codec_dev_sample_info_t sample_cfg = {
    .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
    .channel = 2,
    .channel_mask = 0x03,
    .sample_rate = AUDIO_SAMPLE_RATE,
  };
  if (esp_codec_dev_open(codec_handle, &sample_cfg) != ESP_CODEC_DEV_OK)
  {
    ESP_LOGE(TAG, "Open codec device failed");
    return ESP_FAIL;
  }

  /* Set the initial volume */
  if (esp_codec_dev_set_out_vol(codec_handle, AUDIO_VOICE_VOLUME) != ESP_CODEC_DEV_OK)
  {
    ESP_LOGE(TAG, "Set output volume failed");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "ES8311 codec initialized successfully");
  return ESP_OK;
}

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

static void i2s_sinewave_task(void* args)
{
  esp_err_t ret = ESP_OK;
  size_t bytes_write = 0;

  // Allocate buffer for stereo 16-bit samples
  int16_t* audio_buffer = (int16_t*)malloc(BUFFER_SIZE * 2 * sizeof(int16_t));
  if (!audio_buffer)
  {
    ESP_LOGE(TAG, "[sinewave] No memory for audio buffer");
    vTaskDelete(NULL);
    return;
  }

  float phase = 0.0f;
  uint32_t current_note_index = 0;
  float current_freq = scale_frequencies[0];
  float phase_increment = 2.0f * M_PI * current_freq / AUDIO_SAMPLE_RATE;

  // Calculate how many samples needed for the note duration
  const uint32_t samples_per_note = (AUDIO_SAMPLE_RATE * NOTE_DURATION_MS) / 1000;
  uint32_t samples_played = 0;

  ESP_LOGI(TAG, "[sinewave] Starting musical scale playback");
  ESP_LOGI(TAG, "[sinewave] Base frequency: %.2f Hz (A note)", BASE_FREQ_HZ);
  ESP_LOGI(TAG, "[sinewave] Note duration: %d ms", NOTE_DURATION_MS);
  ESP_LOGI(TAG, "[sinewave] Scale length: %d notes", SCALE_LENGTH);
  ESP_LOGI(TAG, "[sinewave] Sample rate: %d Hz", AUDIO_SAMPLE_RATE);

  while (1)
  {
    // Generate sine wave samples
    for (int i = 0; i < BUFFER_SIZE; i++)
    {
      // Check if we need to switch to the next note
      if (samples_played >= samples_per_note)
      {
        samples_played = 0;
        current_note_index = (current_note_index + 1) % SCALE_LENGTH;
        current_freq = scale_frequencies[current_note_index];
        phase_increment = 2.0f * M_PI * current_freq / AUDIO_SAMPLE_RATE;

        ESP_LOGI(TAG, "[sinewave] Playing note %d: %.2f Hz",
                 current_note_index, current_freq);
      }

      float sample_value = sinf(phase) * SINE_AMPLITUDE;
      int16_t sample = (int16_t)sample_value;

      // Stereo output: same sample for both left and right channels
      audio_buffer[i * 2] = sample; // Left channel
      audio_buffer[i * 2 + 1] = sample; // Right channel

      // Increment phase and wrap to avoid floating point drift
      phase += phase_increment;
      if (phase >= 2.0f * M_PI)
      {
        phase -= 2.0f * M_PI;
      }

      samples_played++;
    }

    // Write sine wave to I2S
    ret = i2s_channel_write(tx_handle, audio_buffer, BUFFER_SIZE * 2 * sizeof(int16_t),
                            &bytes_write, portMAX_DELAY);
    if (ret != ESP_OK)
    {
      ESP_LOGE(TAG, "[sinewave] i2s write failed, %s", err_reason[ret == ESP_ERR_TIMEOUT]);
    }
  }

  free(audio_buffer);
  vTaskDelete(NULL);
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
  if (es8311_codec_init() != ESP_OK)
  {
    ESP_LOGE(TAG, "ES8311 codec init failed");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Audio output system initialized successfully");
  return ESP_OK;
}

esp_err_t audio_output_start(void)
{
  ESP_LOGI(TAG, "Starting audio output task");
  ESP_LOGI(TAG, "Playing musical scale: A-B-C-D-E-F-G-A-G-F-E-D-C-B-A");
  ESP_LOGI(TAG, "Note duration: %d ms", NOTE_DURATION_MS);

  /* Create task to generate and play musical scale */
  BaseType_t ret = xTaskCreate(i2s_sinewave_task, "audio_output", 8192, NULL, 10, NULL);
  if (ret != pdPASS)
  {
    ESP_LOGE(TAG, "Failed to create audio output task");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Audio output task started successfully");
  return ESP_OK;
}

