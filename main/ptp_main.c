/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include <audio.h>
#include <storage.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "ethernet_init.h"
#include "esp_vfs_l2tap.h"
#include "driver/gpio.h"
#include "ptpd.h"
#include "avtp.h"

#include "esp_eth_time.h"

static const char* TAG = "avb";

static struct timespec s_next_time;
static bool s_gpio_level;

void init_ethernet_and_netif(void)
{
  uint8_t eth_port_cnt;
  esp_eth_handle_t* eth_handles;

  ESP_ERROR_CHECK(esp_event_loop_create_default());

  ESP_ERROR_CHECK(ethernet_init_all(&eth_handles, &eth_port_cnt));

  ESP_ERROR_CHECK(esp_netif_init());

  ESP_ERROR_CHECK(esp_vfs_l2tap_intf_register(NULL));

  esp_netif_inherent_config_t esp_netif_base_config = ESP_NETIF_INHERENT_DEFAULT_ETH();
  esp_netif_config_t esp_netif_config = {
    .base = &esp_netif_base_config,
    .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
  };
  char if_key_str[10];
  char if_desc_str[10];
  char num_str[3];
  for (int i = 0; i < eth_port_cnt; i++)
  {
    itoa(i, num_str, 10);
    strcat(strcpy(if_key_str, "ETH_"), num_str);
    strcat(strcpy(if_desc_str, "eth"), num_str);
    esp_netif_base_config.if_key = if_key_str;
    esp_netif_base_config.if_desc = if_desc_str;
    esp_netif_base_config.route_prio -= i * 5;
    esp_netif_t* eth_netif = esp_netif_new(&esp_netif_config);

    // attach Ethernet driver to TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handles[i])));
  }

  for (int i = 0; i < eth_port_cnt; i++)
  {
    ESP_ERROR_CHECK(esp_eth_start(eth_handles[i]));
  }
}

IRAM_ATTR bool ts_callback(esp_eth_mediator_t* eth, void* user_args)
{
  gpio_set_level(CONFIG_EXAMPLE_PTP_PULSE_GPIO, s_gpio_level ^= 1);

  // Set the next target time
  struct timespec interval = {
    .tv_sec = 0,
    .tv_nsec = CONFIG_EXAMPLE_PTP_PULSE_WIDTH_NS
  };
  timespecadd(&s_next_time, &interval, &s_next_time);

  struct timespec curr_time;
  esp_eth_clock_gettime(CLOCK_PTP_SYSTEM, &curr_time);
  // check the next time is in the future
  if (timespeccmp(&s_next_time, &curr_time, >))
  {
    esp_eth_clock_set_target_time(CLOCK_PTP_SYSTEM, &s_next_time);
  }

  return false;
}

void app_main(void)
{
  init_ethernet_and_netif();

  int pid = ptpd_start("ETH_0");
  int avtp_pid = start_avtp_listener("ETH_0");

  init_audio_codec();
  esp_err_t err = init_nvs();
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(err));
    return;
  }
  /* Initialize audio output system for AVB stream playback */
  ESP_LOGI(TAG, "Initializing audio output for AVB stream");
}
