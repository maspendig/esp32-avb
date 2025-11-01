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

#define AVTP_SUBTYPE_ADP 0xFA
#define AVTP_SUBTYPE_MAAP 0xFE

const char *TAG = "avtp";

struct avtp_state_s
{
  bool stop;
  int socket;
};

struct avtp_header_s
{
  uint8_t dst_mac[6];
  uint8_t src_mac[6];
  uint8_t eth_type[2];
};

struct avtp_discovery_msg_s{
  struct avtp_header_s header;
  uint8_t subtype;
  /** AVTP control field containing
   * - Stream ID valid (bit 0)
   * - AVTP version (bits 1..4)
   * - Message type (bits 5..7)
   *
   * use AVTP_STREAMID_VALID_MASK, AVTP_VERSION_MASK, AVTP_MSGTYPE_MASK to extract values
   */
  uint8_t control;
  uint8_t control_data_length[2];
  uint8_t entity_id[8];
  uint8_t entity_model_id[8];
  uint8_t entity_capabilities[4];
  uint8_t talker_stream_sources[2];
  uint8_t talker_capabilities[2];
  uint8_t listener_stream_sinks[2];
  uint8_t listener_capabilities[2];
  uint8_t controller_capabilities[4];
  uint8_t available_index[4];
  uint8_t gptp_grandmaster_id[8];
  uint8_t association_id[8];
};

typedef union
{
  struct avtp_header_s                header;
  struct avtp_discovery_msg_s        adp_msg;
  uint8_t                            raw[128];
} avtp_msg_buffer;

/* Masks for avtp_ctl byte */
const uint8_t AVTP_STREAMID_VALID_MASK = 0x80; /* 8th bit */
const uint8_t AVTP_VERSION_MASK        = 0x70; /* bits 7..5 */
const uint8_t AVTP_MSGTYPE_MASK        = 0x0F; /* bits 4..0 */

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

  // Enable reception of all multicast packets
  bool enable_multicast = true;
  esp_err_t err = esp_eth_ioctl(eth_handle, ETH_CMD_S_ALL_MULTICAST, &enable_multicast);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "failed to enable multicast reception: %s", esp_err_to_name(err));
    return ESP_FAIL;
  }

  s_state = state;
  return ESP_OK;
}

int adp_net_rx(struct avtp_discovery_msg_s *msg, ssize_t len)
{
  uint8_t msg_type = msg->control & AVTP_MSGTYPE_MASK; /* lower 4 bits of 15th byte */
  switch (msg_type)
  {
    case 0x0:
      ESP_LOGI(TAG, "Entity Available Message", msg_type);
      ESP_LOGI(TAG, "Talker Stream Sources: %02X%02X", msg->talker_stream_sources[0], msg->talker_stream_sources[1]);
      break;
    default:
      ESP_LOGW(TAG, "Unknown ADP message type: 0x%02X", msg_type);
      break;
  }

  return ESP_OK;
}


static void avtp_listener_task(void *arg)
{
  const char *interface = "ETH_0";
  avtp_msg_buffer buf;

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
    const ssize_t len = read(state->socket, &buf, sizeof(buf));
    if (len > 0)
    {

      switch (buf.adp_msg.subtype)
      {
      case AVTP_SUBTYPE_ADP:
        ESP_LOGI(TAG, "AVDECC Discovery Protocol received");
        adp_net_rx(&buf.adp_msg, len);
        break;
      case AVTP_SUBTYPE_MAAP:
        ESP_LOGI(TAG, "MAAP Announce received");
        break;
      default:
        ESP_LOGW(TAG, "Unknown AVTP subtype received: 0x%02X", buf.adp_msg.subtype);
        break;
      }
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