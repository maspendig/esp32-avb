#include "avtp.h"

#include <aecp.h>

#include "adp.h"
#include "esp_log.h"
#include "esp_err.h"
#include <errno.h>

#include "esp_vfs_l2tap.h"
#include "esp_eth_driver.h"
#include "acmp.h"

#include <fcntl.h>
#include <msrp.h>

#include "esp_eth_spec.h"
#include "pthread.h"
#include "sys/ioctl.h"
#include <arpa/inet.h>
#include <time.h>
#include <sys/select.h>

#define CONFIG_ADP_SEND_INTERVAL_MSEC 5800

const char* TAG = "avtp";

/* Forward declarations */
static uint64_t mac_to_entity_id(uint64_t mac);
static bool has_available_talker(struct avtp_state_s* state);

static struct avtp_state_s* s_state;

static int avtp_init_state(struct avtp_state_s* state, const char* interface)
{
  state->socket = open("/dev/net/tap", 0);
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

  state->msrp_socket = msrp_init(interface);

  // get HW address
  esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, &state->intf_hw_addr);

  // Generate entity_id from MAC address
  uint64_t mac = ((uint64_t)state->intf_hw_addr[0] << 40) |
    ((uint64_t)state->intf_hw_addr[1] << 32) |
    ((uint64_t)state->intf_hw_addr[2] << 24) |
    ((uint64_t)state->intf_hw_addr[3] << 16) |
    ((uint64_t)state->intf_hw_addr[4] << 8) |
    ((uint64_t)state->intf_hw_addr[5]);
  state->entity_id = mac_to_entity_id(mac);
  state->entity_model_id = 0x0000000000000001ULL;

  ESP_LOGI(TAG, "Entity ID initialized: 0x%016llX, Model ID: 0x%016llX (from MAC: %02X:%02X:%02X:%02X:%02X:%02X)",
           (unsigned long long)state->entity_id,
           (unsigned long long)state->entity_model_id,
           state->intf_hw_addr[0], state->intf_hw_addr[1], state->intf_hw_addr[2],
           state->intf_hw_addr[3], state->intf_hw_addr[4], state->intf_hw_addr[5]);

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


static uint64_t mac_to_entity_id(uint64_t mac)
{
  return ((mac & 0xffffff000000) << 16) | (0xfffe000000) | (mac & 0xffffff);
}


static int64_t timespec_to_ms(const struct timespec* ts)
{
  return ts->tv_sec * 1000 + (ts->tv_nsec / 1000000ll);
}

static bool has_available_talker(struct avtp_state_s* state)
{
  if (!state) return false;

  time_t now = time(NULL);
  for (int i = 0; i < MAX_ADP_ENTITIES; ++i)
  {
    if (state->adp_entities[i].in_use)
    {
      /* Check if entity is still valid */
      if (state->adp_entities[i].valid_until < now)
      {
        /* Entity expired, mark as not in use */
        state->adp_entities[i].in_use = false;
        ESP_LOGW(TAG, "ADP entity 0x%016llX expired",
                 (unsigned long long)state->adp_entities[i].entity_id);
        continue;
      }

      /* Check if entity has talker capabilities */
      if (state->adp_entities[i].talker_stream_sources > 0)
      {
        ESP_LOGI(TAG, "Found available talker: 0x%016llX with %u stream sources",
                 (unsigned long long)state->adp_entities[i].entity_id,
                 state->adp_entities[i].talker_stream_sources);
        return true;
      }
    }
  }
  return false;
}

static void avtp_listener_task(void* arg)
{
  const char* interface = "ETH_0";
  union
  {
    struct avtp_discovery_msg_s adp;
    struct aecp_data_unit_s aecp;
    struct acmp_du_s acmp;
    struct avtp_header_s header;
    u8 raw[100];
  } buf;

  struct avtp_state_s* state = calloc(1, sizeof(struct avtp_state_s));

  // override interface if provided
  if (arg != NULL)
  {
    interface = arg;
  }

  if (avtp_init_state(state, interface) != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to initialize AVTP state, exiting\n");
    free(state);
  }

  ESP_LOGI(TAG, "AVTP listener started on interface: %s", interface);

  msrp_send_domain_request(state);
  while (!state->stop)
  {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(state->socket, &readfds);
    FD_SET(state->msrp_socket, &readfds);

    int max_fd = (state->socket > state->msrp_socket) ? state->socket : state->msrp_socket;

    // Set timeout for select to allow periodic tasks (ADP sending, etc.)
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000; // 100ms timeout

    int ret = select(max_fd + 1, &readfds, NULL, NULL, &timeout);

    if (ret < 0)
    {
      ESP_LOGE(TAG, "select() error: %d (errno: %d)", ret, errno);
      continue;
    }

    // Check if AVTP socket has data
    if (FD_ISSET(state->socket, &readfds))
    {
      const ssize_t len = read(state->socket, &buf, sizeof(buf));
      if (len > 0)
      {
        // TODO implement discovery state machine like in IEEE 1722-2022 p. 60
        switch (buf.header.subtype)
        {
        case AVTP_SUBTYPE_ADP:
          adp_net_rx(state, &buf.adp, len);
          break;
        case AVTP_SUBTYPE_AECP:

          // ESP_LOG_BUFFER_HEX_LEVEL(TAG, (uint8_t*)&buf.raw + 15, 45, ESP_LOG_INFO);
          aecp_net_rx(state, &buf.aecp, len);
          break;
        case AVTP_SUBTYPE_ACMP:

          acmp_net_rx(state, &buf.acmp, len);
          break;
        case AVTP_SUBTYPE_MAAP:
          ESP_LOGI(TAG, "MAAP Announce received");
          break;
        default:
          ESP_LOGW(TAG, "Unknown AVTP subtype received: 0x%02X", buf.header.subtype);
          break;
        }
      }
    }

    // Check if MSRP socket has data
    if (FD_ISSET(state->msrp_socket, &readfds))
    {
      read_msrp_net(state);
    }

    /* Check connection status and attempt to connect to available talkers */
    if (!state->connected)
    {
      if (has_available_talker(state))
      {
        ESP_LOGI(TAG, "Not connected, sending ACMP connect message to available talker");
        send_acmp_connect_tx_command(state, ACMP_MSG_TYPE_CONNECT_TX_COMMAND);
        if (send_acmp_connect_rx_command(state, ACMP_MSG_TYPE_CONNECT_RX_COMMAND) == ESP_OK)
        {
          // msrp_send_listener_join_request(state);
          state->connected = true;
        }
        msrp_send_talker_advertise(state);
      }
    }

    struct timespec time_now;
    struct timespec delta;

    clock_gettime(CLOCK_MONOTONIC, &time_now);
    timespecsub(&time_now,
                &state->last_transmitted_adp, &delta);
    if (timespec_to_ms(&delta)
      > CONFIG_ADP_SEND_INTERVAL_MSEC)
    {
      // TODO refactor using randomDeviceDelay p 56. of IEEE 1722-2022
      state->last_transmitted_adp = time_now;
      send_adp_entity_available(state);
    }
  }
  free(state);
}


int start_avtp_listener(const char* interface)
{
  if (s_state == NULL)
  {
    xTaskCreate(avtp_listener_task, "AVTP", 4096,
                (void*)interface, tskIDLE_PRIORITY + 1, NULL);
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
