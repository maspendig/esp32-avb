//
// Created by max on 1/20/26.
//

#include "es8311.h"

#include <../avb/config.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/i2c_types.h>
#include <driver/i2s_common.h>
#include <driver/i2s_std.h>
#include <driver/i2s_types.h>
#include <hal/i2s_types.h>

#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_vol.h"

#define TAG "ES8311"

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

/**
 * @brief Initialize ES8311 codec with I2C and I2S interfaces
 *
 * This function initializes the ES8311 audio codec using the specified I2C and I2S channel handles.
 * It sets up the codec for both input and output, configures sample rates, and sets the initial volume.
 *
 * @param tx_handle Pointer to the I2S transmit channel handle
 * @param rx_handle Pointer to the I2S receive channel handle
 * @return ESP_OK on success, ESP_FAIL on error
 */
static esp_err_t es8311_codec_init()
{
  /* Initialize I2C peripheral */
  i2c_master_bus_handle_t i2c_bus_handle = NULL;
  i2c_master_bus_config_t i2c_mst_cfg = {
    .i2c_port = I2C_NUM,
    .sda_io_num = I2C_SDA_IO,
    .scl_io_num = I2C_SCL_IO,
    .clk_source = I2S_CLK_SRC_APLL,
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
      .pa_voltage = 5.0f,
      .codec_dac_voltage = 3.3f,
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

void i2s_write(const void* data, size_t size, size_t* bytes_written)
{
  ESP_ERROR_CHECK(i2s_channel_write(tx_handle, data, size, bytes_written, 0));
}

void es8311_init()
{
  /* Initialize I2S peripheral */
  if (i2s_driver_init() != ESP_OK)
  {
    ESP_LOGE(TAG, "I2S driver init failed");
  }

  /* Initialize I2C peripheral and config ES8311 codec by I2C */
  if (es8311_codec_init(&tx_handle, &rx_handle) != ESP_OK)
  {
    ESP_LOGE(TAG, "ES8311 codec init failed");
  }
  ESP_LOGI(TAG, "ES8311 codec initialized successfully");
}
