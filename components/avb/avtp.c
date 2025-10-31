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

#define ETH_TYPE_AVTP 0x22F0

const char *TAG = "avtp";

struct avtp_state_s
{
  bool stop;
  int socket;
};

static struct avtp_state_s *s_state;

static int avtp_init_state(struct avtp_state_s *state, const char *interface)
{
  state ->socket = open("/dev/net/tap", 0);
  if (state->socket < 0)
  {
    ESP_LOGE(TAG, "Failed to create tx socket");
    return ESP_FAIL;
  }

  int ioctl_err = ioctl(state->socket, L2TAP_S_INTF_DEVICE, interface);

  if (ioctl_err < 0)
  {
    ESP_LOGE(TAG, "failed to set network interface %s at socket: %d\n", interface, ioctl_err);
    return ESP_FAIL;
  }

  // Set the Ethertype filter (frames with this type will be available through the state->tx_socket)
  uint16_t eth_type_filter = ETH_TYPE_AVTP; // Example Ethertype
  if (ioctl(state->socket, L2TAP_S_RCV_FILTER, &eth_type_filter) < 0)
  {
    ESP_LOGE(TAG, "failed to set Ethertype filter: %d\n", errno);
    return ESP_FAIL;
  }

  // Get the ethernet handle to configure multicast reception
  esp_eth_handle_t eth_handle;
  if (ioctl(state->socket, L2TAP_G_DEVICE_DRV_HNDL, &eth_handle) < 0)
  {
    ESP_LOGE(TAG, "failed to get ethernet handle: %d\n", errno);
    return ESP_FAIL;
  }

  //  // Enable reception of all multicast packets
  //  bool enable_multicast = true;
  //  esp_err_t err = esp_eth_ioctl(eth_handle, ETH_CMD_S_ALL_MULTICAST, &enable_multicast);
  //  if (err != ESP_OK)
  //  {
  //    ESP_LOGE(TAG, "failed to enable multicast reception: %s", esp_err_to_name(err));
  //    return ESP_FAIL;
  //  }

  // Enable reception of specific multicast MAC address used by AVTP
  uint8_t multicast_mac[6] = {0x91, 0xe0, 0xf0, 0x01, 0x00, 0x00};
  esp_err_t err = esp_eth_ioctl(eth_handle, ETH_CMD_ADD_MAC_FILTER, multicast_mac);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "failed to add multicast MAC filter: %s", esp_err_to_name(err));
    return ESP_FAIL;
  }

  s_state = state;
  return ESP_OK;
}

static void avtp_listener_task(void *arg)
{
  const char *interface = "ETH_0";
  uint8_t buf[1600];

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

  while(!state->stop)
  {
    const ssize_t len = read(state->socket, buf, sizeof(buf));
    if (len > 0)
    {
      uint16_t ethertype = (buf[12] << 8) | buf[13];
      // Print ethertype as hex
      ESP_LOGI(TAG, "AVTP Packet received, Ethertype: 0x%04X, Length: %d bytes", ethertype, len);
    }
  }
  free(state);
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