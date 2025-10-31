#include "avtp.h"
#include "esp_log.h"
#include "esp_err.h"
#include <errno.h>

#include "esp_vfs_l2tap.h"
#include "esp_eth_driver.h"

#include <fcntl.h>

#include "esp_eth_spec.h"
#include "pthread.h"
#include "sys/ioctl.h"

const char *TAG = "avtp";

struct avtp_state_s
{
  bool stop;
  int socket;
};

static struct avtp_state_s *s_state;

static int avtp_init_state(struct avtp_state_s *state, const char *interface){

  s_state = state;
  return ESP_OK;
}

static void avtp_listener_task(void *arg)
{
  char *interface = "ETH_0";
  struct avtp_state_s* state = calloc(1, sizeof(struct avtp_state_s));

  // override interface if provided
  if (arg != NULL)
  {
    interface = arg;
  }

  if(avtp_init_state(state, interface) != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to initialize AVTP state, exiting\n");
    free(state);
  }

  ESP_LOGI(TAG, "AVTP listener started on interface: %s", interface);

  while(state->stop != true)
  {
    ESP_LOGI(TAG, "Listening for AVTP packets on %s...", interface);
  }
}

int start_avtp_listener(const char *interface)
{
  if (s_state == NULL)
  {
    xTaskCreate(avtp_listener_task, "AVTP", 4096,
      (void *)interface, tskIDLE_PRIORITY + 1, nullptr);
    return ESP_OK;
  }
  ESP_LOGE(TAG, "Other instance of AVTP is already running");
  return ESP_FAIL;
}

int stop_avtp_listener(int pid)
{
  s_state->stop = true;
  return ESP_OK;
}