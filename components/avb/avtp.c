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
#include <mvrp.h>

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

  // Set the Ethertype filter for untagged AVTP frames
  uint16_t eth_type_filter = ETH_TYPE_AVTP;
  if (ioctl(state->socket, L2TAP_S_RCV_FILTER, &eth_type_filter) < 0)
  {
    ESP_LOGE(TAG, "failed to set Ethertype filter: %d\n", errno);
    return ESP_FAIL;
  }

  /* Create VLAN socket for receiving VLAN-tagged AVTP streams (802.1Q) */
  state->vlan_socket = open("/dev/net/tap", 0);
  if (state->vlan_socket < 0)
  {
    ESP_LOGE(TAG, "Failed to create VLAN socket");
    return ESP_FAIL;
  }

  ioctl_err = ioctl(state->vlan_socket, L2TAP_S_INTF_DEVICE, interface);
  if (ioctl_err < 0)
  {
    ESP_LOGE(TAG, "failed to set network interface %s at VLAN socket: %d\n", interface, ioctl_err);
    close(state->vlan_socket);
    return ESP_FAIL;
  }

  /* Filter for 802.1Q VLAN-tagged frames */
  eth_type_filter = ETH_TYPE_8021Q;
  if (ioctl(state->vlan_socket, L2TAP_S_RCV_FILTER, &eth_type_filter) < 0)
  {
    ESP_LOGE(TAG, "failed to set VLAN Ethertype filter: %d\n", errno);
    close(state->vlan_socket);
    return ESP_FAIL;
  }

  /* Set VLAN socket to non-blocking mode for efficient draining of stream packets */
  int flags = fcntl(state->vlan_socket, F_GETFL, 0);
  if (flags >= 0)
  {
    fcntl(state->vlan_socket, F_SETFL, flags | O_NONBLOCK);
  }
  ESP_LOGI(TAG, "VLAN socket initialized for 802.1Q tagged frames (non-blocking)");

  // Get the ethernet handle to configure multicast reception
  esp_eth_handle_t eth_handle;
  if (ioctl(state->socket, L2TAP_G_DEVICE_DRV_HNDL, &eth_handle) < 0)
  {
    ESP_LOGE(TAG, "failed to get ethernet handle: %d\n", errno);
    return ESP_FAIL;
  }

  state->acmp_sequence_id = 0;
  state->msrp_socket = msrp_init(interface);
  state->mvrp_socket = mvrp_init(interface);

  /* Initialize MSRP state machine */
  msrp_state_init(&state->msrp);

  /* Initialize MVRP state machine */
  mvrp_state_init(&state->mvrp);

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

  /* Also enable all multicast reception as fallback */
  bool enable_multicast = true;
  int err = esp_eth_ioctl(eth_handle, ETH_CMD_S_ALL_MULTICAST, &enable_multicast);
  if (err != ESP_OK)
  {
    ESP_LOGW(TAG, "failed to enable all multicast reception: %s", esp_err_to_name(err));
  }
  else
  {
    ESP_LOGI(TAG, "All multicast reception enabled");
  }

  /* Add specific multicast MAC filters for AVB:
   * - MAAP dynamic allocation range: 91:E0:F0:00:00:00 - 91:E0:F0:00:FD:FF
   * - AVDECC/ATDECC: 91:E0:F0:01:00:00
   */
  uint8_t maap_base_mac[6] = {0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x00}; /* Common MAAP address */
  err = esp_eth_ioctl(eth_handle, ETH_CMD_ADD_MAC_FILTER, maap_base_mac);
  if (err != ESP_OK)
  {
    ESP_LOGW(TAG, "failed to add MAAP MAC filter: %s", esp_err_to_name(err));
  }
  else
  {
    ESP_LOGI(TAG, "Added MAC filter for MAAP address %02X:%02X:%02X:%02X:%02X:%02X",
             maap_base_mac[0], maap_base_mac[1], maap_base_mac[2],
             maap_base_mac[3], maap_base_mac[4], maap_base_mac[5]);
  }

  /* Add AVDECC multicast address */
  uint8_t avdecc_mac[6] = {0x91, 0xE0, 0xF0, 0x01, 0x00, 0x00};
  err = esp_eth_ioctl(eth_handle, ETH_CMD_ADD_MAC_FILTER, avdecc_mac);
  if (err != ESP_OK)
  {
    ESP_LOGW(TAG, "failed to add AVDECC MAC filter: %s", esp_err_to_name(err));
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
  /* Buffer large enough for AVTP stream packets (AAF can be up to ~1500 bytes) */
  union
  {
    struct avtp_discovery_msg_s adp;
    struct aecp_data_unit_s aecp;
    struct acmp_du_s acmp;
    struct avtp_header_s header;
    u8 raw[1518]; /* Max Ethernet frame size */
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
  mvrp_vlan_join(state, 2);
  u8 seq_number = 0;
  // msrp_send_talker_advertise(state);
  while (!state->stop)
  {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(state->socket, &readfds);
    FD_SET(state->vlan_socket, &readfds);
    FD_SET(state->msrp_socket, &readfds);
    FD_SET(state->mvrp_socket, &readfds);

    int max_fd = state->socket;
    if (state->vlan_socket > max_fd) max_fd = state->vlan_socket;
    if (state->msrp_socket > max_fd) max_fd = state->msrp_socket;
    if (state->mvrp_socket > max_fd) max_fd = state->mvrp_socket;

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
        u8 subtype = buf.header.subtype;
        /* Check if this is a stream packet (subtypes 0x00-0x7F) or control (0x80+) */
        /* Control packet */
        switch (subtype)
        {
        case AVTP_SUBTYPE_ADP:
          adp_net_rx(state, &buf.adp, len);
          break;
        case AVTP_SUBTYPE_AECP:
          aecp_net_rx(state, &buf.aecp, len);
          break;
        case AVTP_SUBTYPE_ACMP:
          acmp_net_rx(state, &buf.acmp, len);
          break;
        case AVTP_SUBTYPE_MAAP:
          ESP_LOGI(TAG, "MAAP Announce received");
          break;
        default:
          ESP_LOGW(TAG, "Unknown AVTP control subtype received: 0x%02X", subtype);
          break;
        }
      }
    }

    /* Check if VLAN socket has VLAN-tagged frames (AVB streams) */
    /* Drain all available packets to prevent queue overflow */
    if (FD_ISSET(state->vlan_socket, &readfds))
    {
      ssize_t len;
      int packets_processed = 0;

      /* Read all available packets (socket is non-blocking) */
      while ((len = read(state->vlan_socket, &buf, sizeof(buf))) > 0)
      {
        packets_processed++;

        /* Parse 802.1Q VLAN header:
         * Offset 12-13: TPID (0x8100) - already filtered by socket
         * Offset 14-15: TCI (PCP:3, DEI:1, VID:12)
         * Offset 16-17: Inner Ethertype
         * Offset 18+: Payload
         */
        u16 tci = (buf.raw[14] << 8) | buf.raw[15];
        u16 vlan_id = tci & 0x0FFF;
        u8 pcp = (tci >> 13) & 0x07;
        u16 inner_ethertype = (buf.raw[16] << 8) | buf.raw[17];

        /* Check if inner Ethertype is AVTP */
        if (inner_ethertype == ETH_TYPE_AVTP)
        {
          /* AVTP subtype is at offset 18 (after VLAN header) */
          u8 subtype = buf.raw[18];

          if (subtype < 0x80)
          {
            /* Stream data packet */
            switch (subtype)
            {
            case AVTP_SUBTYPE_AAF:
              /* TODO: Process AAF audio data from offset 18 */
              break;
            case AVTP_SUBTYPE_61883_IIDC:
              if (buf.raw[20] != seq_number)
              {
                ESP_LOGW(TAG, "sequence number mismatch: expected=%u received=%u", seq_number, buf.raw[20]);
                seq_number = buf.raw[20];
              }

              seq_number++;
              break;
            case AVTP_SUBTYPE_CVF:
              /* TODO: Process CVF video stream */
              break;
            case AVTP_SUBTYPE_CRF:
              /* TODO: Process CRF clock reference */
              break;
            default:
              break;
            }
          }
        }
      }

      if (packets_processed > 0)
      {
        ESP_LOGD(TAG, "Processed %d VLAN packets in batch", packets_processed);
      }
    }

    if (FD_ISSET(state->mvrp_socket, &readfds))
    {
      mvrp_net_rx(state);
    }

    // Check if MSRP socket has data
    if (FD_ISSET(state->msrp_socket, &readfds))
    {
      msrp_net_rx(state);
    }

    /* Process MSRP periodic tasks (timers, pending transmissions) */
    msrp_periodic(state);

    /* Process MVRP periodic tasks (timers, pending transmissions) */
    mvrp_periodic(state);

    /* we use the MOTU as AVB controller to establish the input / output streams */
    /* Check connection status and attempt to connect to available talkers */
    // if (!state->connected)
    // {
    //   if (has_available_talker(state))
    //   {
    //     ESP_LOGI(TAG, "Not connected, sending ACMP connect message to available talker");
    //     // send_acmp_connect_tx_command(state, ACMP_MSG_TYPE_CONNECT_TX_COMMAND);
    //     if (send_acmp_connect_rx_command(state, ACMP_MSG_TYPE_CONNECT_RX_COMMAND) == ESP_OK)
    //     {
    //       // msrp_send_listener_join_request(state);
    //       state->connected = true;
    //     }
    //     msrp_send_talker_advertise(state);
    //   }
    // }

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
    /* Use higher priority for real-time packet processing */
    xTaskCreate(avtp_listener_task, "AVTP", 8192,
                (void*)interface, 10, NULL);
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
