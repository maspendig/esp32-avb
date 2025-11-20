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

#include "esp_eth_spec.h"
#include "pthread.h"
#include "sys/ioctl.h"
#include <arpa/inet.h>
#include <time.h>


#define CONFIG_ADP_SEND_INTERVAL_MSEC 5800

const char* TAG = "avtp";


struct avtp_discovery_msg_s
{
  struct header_s header;
  uint8_t subtype;
  /** AVTP control field containing
   * - Stream ID valid (bit 0)
   * - AVTP version (bits 1..4)
   * - Message type (bits 5..7)
   *
   * use AVTP_STREAMID_VALID_MASK, AVTP_VERSION_MASK, AVTP_MSGTYPE_MASK to extract values
   */
  uint8_t control;

  /** Control data length field containing valid_time (5 bits) and control_data_length (11 bits) */
  union
  {
    struct
    {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
      uint16_t control_data_length : 11; /* 11 bits for control data length */
      uint16_t valid_time : 5; /* 5 bits for valid time */
#else
      uint16_t valid_time : 5; /* 5 bits for valid time */
      uint16_t control_data_length : 11; /* 11 bits for control data length */
#endif
    } __attribute__((packed));

    uint8_t raw[2]; /* Raw bytes for network transmission */
    uint16_t raw_u16; /* Raw 16-bit value for easy manipulation */
  } control_data_length_field;

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
  struct header_s header;
  struct avtp_discovery_msg_s adp_msg;
  struct aecp_data_unit_s aecp_msg;
  struct acmp_du_s acmp_msg;
  uint8_t raw[128];
} avtp_msg_buffer;

/* Masks for avtp_ctl byte */
const uint8_t AVTP_STREAMID_VALID_MASK = 0x80; /* 8th bit */
const uint8_t AVTP_VERSION_MASK = 0x70; /* bits 7..5 */
const uint8_t AVTP_MSGTYPE_MASK = 0x0F; /* bits 4..0 */


/* Forward declarations */
static uint64_t mac_to_entity_id(uint64_t mac);
static void send_entity_descriptor_response(struct aecp_data_unit_s* request_msg, uint16_t configuration_index);
static void adp_upsert_entity(struct avtp_discovery_msg_s* msg);
static void adp_remove_entity(struct avtp_discovery_msg_s* msg);
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

int adp_net_rx(struct avtp_discovery_msg_s* msg, ssize_t len)
{
  /* Convert control_data_length_field from network byte order for parsing */
  msg->control_data_length_field.raw_u16 = ntohs(msg->control_data_length_field.raw_u16);

  uint8_t msg_type = msg->control & AVTP_MSGTYPE_MASK; /* lower 4 bits of 15th byte */
  switch (msg_type)
  {
  case ADP_MSG_TYPE_ENTITY_AVAILABLE:
    ESP_LOGI(TAG, "ADP Entity Available Message received");
    adp_upsert_entity(msg);
    break;
  case ADP_MSG_TYPE_ENTITY_DEPARTING:
    ESP_LOGI(TAG, "ADP Entity Departing Message received");
    adp_remove_entity(msg);
    break;
  case ADP_MSG_TYPE_ENTITY_DISCOVER:
    ESP_LOGI(TAG, "Entity Discover Message", msg_type);
    break;
  default:
    ESP_LOGW(TAG, "Unknown ADP message type: 0x%02X", msg_type);
    break;
  }

  return ESP_OK;
}


static uint64_t mac_to_entity_id(uint64_t mac)
{
  return ((mac & 0xffffff000000) << 16) | (0xfffe000000) | (mac & 0xffffff);
}

static void adp_upsert_entity(struct avtp_discovery_msg_s* msg)
{
  if (!s_state) return;

  /* Extract entity_id (network -> host) */
  uint64_t entity_id_net;
  memcpy(&entity_id_net, msg->entity_id, sizeof(entity_id_net));
  uint64_t entity_id = ntohll(entity_id_net);

  /* Extract capabilities & counts (big-endian byte arrays) */
  uint16_t talker_stream_sources = ((uint16_t)msg->talker_stream_sources[0] << 8) | msg->talker_stream_sources[1];
  uint16_t talker_capabilities = ((uint16_t)msg->talker_capabilities[0] << 8) | msg->talker_capabilities[1];
  uint16_t listener_stream_sinks = ((uint16_t)msg->listener_stream_sinks[0] << 8) | msg->listener_stream_sinks[1];
  uint16_t listener_capabilities = ((uint16_t)msg->listener_capabilities[0] << 8) | msg->listener_capabilities[1];
  uint32_t controller_capabilities = ((uint32_t)msg->controller_capabilities[0] << 24) |
    ((uint32_t)msg->controller_capabilities[1] << 16) |
    ((uint32_t)msg->controller_capabilities[2] << 8) |
    ((uint32_t)msg->controller_capabilities[3]);
  uint32_t available_index = ((uint32_t)msg->available_index[0] << 24) |
    ((uint32_t)msg->available_index[1] << 16) |
    ((uint32_t)msg->available_index[2] << 8) |
    ((uint32_t)msg->available_index[3]);

  uint8_t* src_mac = msg->header.src_mac;

  /* Valid time (5 bits) doubled in seconds */
  uint8_t valid_time = msg->control_data_length_field.valid_time & 0x1F;
  time_t now = time(NULL);
  time_t valid_until = now + (valid_time * 2);

  /* Search for existing entry or free slot */
  int free_index = -1;
  for (int i = 0; i < MAX_ADP_ENTITIES; ++i)
  {
    if (s_state->adp_entities[i].in_use)
    {
      if (s_state->adp_entities[i].entity_id == entity_id)
      {
        /* Update existing entry */
        s_state->adp_entities[i].talker_stream_sources = talker_stream_sources;
        s_state->adp_entities[i].talker_capabilities = talker_capabilities;
        s_state->adp_entities[i].listener_stream_sinks = listener_stream_sinks;
        s_state->adp_entities[i].listener_capabilities = listener_capabilities;
        s_state->adp_entities[i].controller_capabilities = controller_capabilities;
        s_state->adp_entities[i].available_index = available_index;
        s_state->adp_entities[i].valid_until = valid_until;
        memcpy(s_state->adp_entities[i].mac, src_mac, 6);
        ESP_LOGI(TAG, "Updated ADP entity 0x%016llX (valid %us)",
                 (unsigned long long)entity_id, (unsigned)(valid_time * 2));
        return;
      }
    }
    else if (free_index < 0)
    {
      free_index = i; /* remember first free slot */
    }
  }

  if (free_index < 0)
  {
    ESP_LOGW(TAG, "ADP entity list full; cannot add 0x%016llX", (unsigned long long)entity_id);
    return;
  }

  /* Add new entry */
  struct adp_entity_entry_s* entry = &s_state->adp_entities[free_index];
  entry->entity_id = entity_id;
  memcpy(entry->mac, src_mac, 6);
  entry->talker_stream_sources = talker_stream_sources;
  entry->talker_capabilities = talker_capabilities;
  entry->listener_stream_sinks = listener_stream_sinks;
  entry->listener_capabilities = listener_capabilities;
  entry->controller_capabilities = controller_capabilities;
  entry->available_index = available_index;
  entry->valid_until = valid_until;
  entry->in_use = true;

  ESP_LOGI(
    TAG, "Added ADP entity 0x%016llX (MAC: %02X:%02X:%02X:%02X:%02X:%02X, TalkerSrc=%u, ListenerSinks=%u, valid %us)",
    (unsigned long long)entity_id,
    src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5],
    talker_stream_sources,
    listener_stream_sinks,
    (unsigned)(valid_time * 2));
}

static void adp_remove_entity(struct avtp_discovery_msg_s* msg)
{
  if (!s_state) return;

  /* Extract entity_id (network -> host) */
  uint64_t entity_id_net;
  memcpy(&entity_id_net, msg->entity_id, sizeof(entity_id_net));
  uint64_t entity_id = ntohll(entity_id_net);

  /* Search for entity and mark as not in use */
  for (int i = 0; i < MAX_ADP_ENTITIES; ++i)
  {
    if (s_state->adp_entities[i].in_use && s_state->adp_entities[i].entity_id == entity_id)
    {
      s_state->adp_entities[i].in_use = false;
      ESP_LOGI(TAG, "Removed ADP entity 0x%016llX (MAC: %02X:%02X:%02X:%02X:%02X:%02X)",
               (unsigned long long)entity_id,
               s_state->adp_entities[i].mac[0], s_state->adp_entities[i].mac[1],
               s_state->adp_entities[i].mac[2], s_state->adp_entities[i].mac[3],
               s_state->adp_entities[i].mac[4], s_state->adp_entities[i].mac[5]);
      return;
    }
  }

  ESP_LOGW(TAG, "ADP entity departing not found: 0x%016llX", (unsigned long long)entity_id);
}

void send_adp_entity_available()
{
  if (s_state == NULL || s_state->socket < 0)
  {
    ESP_LOGE(TAG, "Socket not ready to send ADP");
    return;
  }

  struct avtp_discovery_msg_s msg = {0};

  memcpy(&msg.header.src_mac, s_state->intf_hw_addr, ETH_ADDR_LEN);

  uint8_t dst_mac[6] = {0x91, 0xE0, 0xF0, 0x01, 0x00, 0x00}; // ADP multicast MAC
  memcpy(msg.header.dst_mac, dst_mac, sizeof(dst_mac));

  /* Use entity_id from state and convert to network byte order */
  uint64_t entity_id_net = htonll(s_state->entity_id);
  memcpy(msg.entity_id, &entity_id_net, sizeof(msg.entity_id));

  /* Use entity_model_id from state and convert to network byte order */
  uint64_t entity_model_id_net = htonll(s_state->entity_model_id);
  memcpy(msg.entity_model_id, &entity_model_id_net, sizeof(msg.entity_model_id));

  /* Ethernet type (big-endian) */
  msg.header.eth_type[0] = (ETH_TYPE_AVTP >> 8) & 0xFF;
  msg.header.eth_type[1] = ETH_TYPE_AVTP & 0xFF;

  msg.subtype = AVTP_SUBTYPE_ADP;
  /* Control: set ADP Entity Available message type (lower 4 bits) */
  msg.control = (ADP_MSG_TYPE_ENTITY_AVAILABLE & AVTP_MSGTYPE_MASK);

  /* control_data_length: length of ADP payload after header (network byte order) */
  uint16_t payload_len = sizeof(msg) - sizeof(msg.header);
  msg.control_data_length_field.control_data_length = payload_len;
  msg.control_data_length_field.valid_time = 10; /* Set valid_time as needed */
  msg.control_data_length_field.raw_u16 = htons(msg.control_data_length_field.raw_u16);

  memcpy(msg.entity_capabilities, (uint8_t[]){0x00, 0x00, 0xC5, 0x08}, 4); // Example capabilities

  msg.talker_capabilities[0] = 0x40;
  msg.talker_capabilities[1] = 0x01;
  msg.talker_stream_sources[0] = 0x00;
  msg.talker_stream_sources[1] = 0x01;
  /* Set 4 listener stream sinks (big-endian 0x0004) */
  msg.listener_stream_sinks[0] = 0x00;
  msg.listener_stream_sinks[1] = 0x01;
  msg.listener_capabilities[0] = 0x40;
  msg.listener_capabilities[1] = 0x01;

  /* Use incremented available_index from state (big-endian) */
  msg.available_index[0] = (s_state->adp_available_index >> 24) & 0xFF;
  msg.available_index[1] = (s_state->adp_available_index >> 16) & 0xFF;
  msg.available_index[2] = (s_state->adp_available_index >> 8) & 0xFF;
  msg.available_index[3] = s_state->adp_available_index++ & 0xFF;

  memset(msg.association_id, 0x00, sizeof(msg.association_id));
  memcpy(msg.gptp_grandmaster_id, (uint8_t[]){0x00, 0x01, 0xf2, 0xff, 0xfe, 0x00, 0xae, 0x35}, 8);
  // Example grandmaster ID

  ssize_t written = write(s_state->socket, &msg, 82);
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send ADP entity available: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Sent ADP Entity Available");
  }
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
  avtp_msg_buffer buf;

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

  while (!state->stop)
  {
    const ssize_t len = read(state->socket, &buf, sizeof(buf));
    if (len > 0)
    {
      // TODO implement discovery state machine like in IEEE 1722-2022 p. 60
      switch (buf.adp_msg.subtype)
      {
      case AVTP_SUBTYPE_ADP:
        adp_net_rx(&buf.adp_msg, len);
        break;
      case AVTP_SUBTYPE_AECP:
        aecp_net_rx(state, &buf.aecp_msg, len);
        break;
      case AVTP_SUBTYPE_ACMP:
        acmp_net_rx(state, &buf.acmp_msg, len);
        break;
      case AVTP_SUBTYPE_MAAP:
        ESP_LOGI(TAG, "MAAP Announce received");
        break;
      default:
        ESP_LOGW(TAG, "Unknown AVTP subtype received: 0x%02X", buf.adp_msg.subtype);
        break;
      }
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
          state->connected = true;
        }
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
      send_adp_entity_available();
    }
  }
  free(state);
}


int start_avtp_listener(const char* interface)
{
  if (s_state == NULL)
  {
    xTaskCreate(avtp_listener_task, "AVTP", 4096,
                (void*)interface, tskIDLE_PRIORITY + 1, nullptr);
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
