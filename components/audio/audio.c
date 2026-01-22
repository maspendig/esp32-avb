/**
 * This file provides interfaces to interact with audio codecs and handle audio data.
 * The used codec is defined via CONFIG_AUDIO_CODEC in the project configuration.
 * Currently supported codecs are:
 * - ES8311 (default - onboard ESP32-P4-Function-EV-Board codec)
 * - TLV320AIC32X4 (audio cape required)
 * - AK4619 (TDM mode - audio cape required)
 */

#include "audio.h"
#include <esp_ldo_regulator.h>
#include <esp_log.h>
#include <driver/gpio.h>

#define TAG "audio"

void init_audio_codec()
{
#ifndef CONFIG_CODEC_ES8311

  // Configure LDO 4 to 3.3V
  ESP_LOGI(TAG, "Configuring LDO 4 to 3.3V");
  esp_ldo_channel_handle_t ldo4_chan = NULL;
  esp_ldo_channel_config_t ldo_config = {
    .chan_id = 4, // LDO channel 4
    .voltage_mv = 3300, // 3.3V in millivolts
    .flags = {
      .adjustable = false, // Not adjustable after acquisition
    }
  };
  esp_err_t ret = esp_ldo_acquire_channel(&ldo_config, &ldo4_chan);
  if (ret == ESP_OK)
  {
    ESP_LOGI(TAG, "LDO 4 configured to 3.3V successfully");
  }
  else
  {
    ESP_LOGE(TAG, "Failed to configure LDO 4: %s", esp_err_to_name(ret));
  }
#endif // CONFIG_AUDIO_CODEC_ES8311

  CODEC_INIT();

#ifdef CONFIG_CODEC_TLV320AIC3254
  CODEC_SET_OUTPUT_LEVELS(75, 75); // Set output levels to 75%
#endif
}

#define TAG "audio"
