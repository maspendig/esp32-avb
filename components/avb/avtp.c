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
#include <arpa/inet.h>
#include <time.h>

#define ETH_TYPE_AVTP 0x22F0

#define AVTP_SUBTYPE_ADP  0xFA
#define AVTP_SUBTYPE_AECP 0xFB
#define AVTP_SUBTYPE_MAAP 0xFE

#define ADP_MSG_TYPE_ENTITY_AVAILABLE 0x0
#define ADP_MSG_TYPE_ENTITY_DEPARTING 0x1
#define ADP_MSG_TYPE_ENTITY_DISCOVER  0x2

#define AECP_MSG_TYPE_ACM_COMMAND   0x0
#define AECP_MSG_TYPE_ACM_RESPONSE  0x1

#define ACM_COMMAND_TYPE_READ_DESCRIPTOR 0x0004
#define ACM_COMMAND_TYPE_REGISTER_UNSOLICITED_NOTIFICATION 0x0024
#define ACM_COMMAND_TYPE_UNREGISTER_UNSOLICITED_NOTIFICATION 0x0025
#define ACM_COMMAND_TYPE_IDENTIFY_NOTIFICATION 0x0026


#define CONFIG_ADP_SEND_INTERVAL_MSEC 5800

const char *TAG = "avtp";

#define MAX_ADP_ENTITIES 32

/* Structure to hold discovered ADP entity information */
struct adp_entity_entry_s {
  uint64_t entity_id;
  uint8_t mac[6];
  uint16_t talker_stream_sources;
  uint16_t talker_capabilities;
  uint16_t listener_stream_sinks;
  uint16_t listener_capabilities;
  uint32_t controller_capabilities;
  uint32_t available_index;
  time_t valid_until;  // epoch seconds until this entry is valid
  bool in_use;
};

struct avtp_state_s
{
  bool stop;
  int socket;
  uint8_t intf_hw_addr[6];
  uint64_t entity_id;
  uint64_t entity_model_id;
  struct timespec last_transmitted_adp;
  uint32_t adp_available_index; // renamed from adp_availabe_index[4] for easier increment
  struct adp_entity_entry_s adp_entities[MAX_ADP_ENTITIES];
};

struct header_s
{
  uint8_t dst_mac[6];
  uint8_t src_mac[6];
  uint8_t eth_type[2];
};

struct avtp_discovery_msg_s{
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
  union {
    struct {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
      uint16_t control_data_length : 11;  /* 11 bits for control data length */
      uint16_t valid_time : 5;            /* 5 bits for valid time */
#else
      uint16_t valid_time : 5;            /* 5 bits for valid time */
      uint16_t control_data_length : 11;  /* 11 bits for control data length */
#endif
    } __attribute__((packed));
    uint8_t raw[2];                       /* Raw bytes for network transmission */
    uint16_t raw_u16;                     /* Raw 16-bit value for easy manipulation */
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

/* IEEE 1722.1-2021 ENTITY Descriptor (7.2.1) */
struct atdecc_entity_descriptor_s {
  uint16_t descriptor_type;              // 0x0000 for ENTITY
  uint16_t descriptor_index;             // 0x0000 for ENTITY (only one per entity)
  uint64_t entity_id;                    // Unique identifier for the AVDECC Entity
  uint64_t entity_model_id;              // Unique identifier for the Entity model
  uint32_t entity_capabilities;          // Entity capability flags
  uint16_t talker_stream_sources;        // Number of talker stream sources
  uint16_t talker_capabilities;          // Talker capability flags
  uint16_t listener_stream_sinks;        // Number of listener stream sinks
  uint16_t listener_capabilities;        // Listener capability flags
  uint32_t controller_capabilities;      // Controller capability flags
  uint32_t available_index;              // Incremented on ADP available
  uint64_t association_id;               // Association ID for grouping entities
  uint8_t entity_name[64];               // UTF-8 entity name
  uint16_t vendor_name_string;           // Localized string reference
  uint16_t model_name_string;            // Localized string reference
  uint8_t firmware_version[64];          // UTF-8 firmware version string
  uint8_t group_name[64];                // UTF-8 group name string
  uint8_t serial_number[64];             // UTF-8 serial number string
  uint16_t configurations_count;         // Number of configuration descriptors
  uint16_t current_configuration;        // Index of current configuration
} __attribute__((packed));

struct aecp_data_unit_s {
  struct header_s header;
  uint8_t subtype;                      // 1 octet

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  uint8_t message_type : 4;             // 4 bits
  uint8_t version : 3;                  // 3 bits
  uint8_t h : 1;                        // 1 bit (header specific)

  uint16_t control_data_length : 11;    // 11 bits
  uint16_t status : 5;                  // 5 bits
#else
  uint8_t h : 1;                        // 1 bit (header specific)
  uint8_t version : 3;                  // 3 bits
  uint8_t message_type : 4;             // 4 bits

  uint16_t status : 5;                  // 5 bits
  uint16_t control_data_length : 11;    // 11 bits
#endif

  uint64_t target_entity_id;            // 64 bits
  uint64_t controller_entity_id;        // 64 bits
  uint16_t sequence_id;                 // 16 bits
  uint16_t command_type;                // 16 bits (ACM command type)
} __attribute__((packed));

/* AECP READ_DESCRIPTOR Response structure */
struct aecp_read_descriptor_response_s {
  struct aecp_data_unit_s aecp_header;
  uint16_t configuration_index;         // 16 bits
  uint16_t reserved;                    // 16 bits
  struct atdecc_entity_descriptor_s descriptor;
} __attribute__((packed));



typedef union
{
  struct header_s                header;
  struct avtp_discovery_msg_s        adp_msg;
  struct aecp_data_unit_s        aecp_msg;
  uint8_t                            raw[128];
} avtp_msg_buffer;

/* Masks for avtp_ctl byte */
const uint8_t AVTP_STREAMID_VALID_MASK = 0x80; /* 8th bit */
const uint8_t AVTP_VERSION_MASK        = 0x70; /* bits 7..5 */
const uint8_t AVTP_MSGTYPE_MASK        = 0x0F; /* bits 4..0 */

/* Define ntohll and htonll if not already defined */
#ifndef ntohll
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define ntohll(x) ((uint64_t)( \
    (((uint64_t)(x) & 0x00000000000000ffULL) << 56) | \
    (((uint64_t)(x) & 0x000000000000ff00ULL) << 40) | \
    (((uint64_t)(x) & 0x0000000000ff0000ULL) << 24) | \
    (((uint64_t)(x) & 0x00000000ff000000ULL) << 8)  | \
    (((uint64_t)(x) & 0x000000ff00000000ULL) >> 8)  | \
    (((uint64_t)(x) & 0x0000ff0000000000ULL) >> 24) | \
    (((uint64_t)(x) & 0x00ff000000000000ULL) >> 40) | \
    (((uint64_t)(x) & 0xff00000000000000ULL) >> 56) ))
#define htonll(x) ntohll(x)
#else
#define ntohll(x) ((uint64_t)(x))
#define htonll(x) ((uint64_t)(x))
#endif
#endif

/* Forward declarations */
static uint64_t mac_to_entity_id(uint64_t mac);
static void send_entity_descriptor_response(struct aecp_data_unit_s *request_msg, uint16_t configuration_index);
static void adp_upsert_entity(struct avtp_discovery_msg_s *msg);
static void adp_remove_entity(struct avtp_discovery_msg_s *msg);

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

  // get HW address
  esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, &state->intf_hw_addr);

  // Generate entity_id from MAC address
  uint64_t mac = ((uint64_t)state->intf_hw_addr[0] << 40) |
                 ((uint64_t)state->intf_hw_addr[1] << 32) |
                 ((uint64_t)state->intf_hw_addr[2] << 24) |
                 ((uint64_t)state->intf_hw_addr[3] << 16) |
                 ((uint64_t)state->intf_hw_addr[4] << 8)  |
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

int adp_net_rx(struct avtp_discovery_msg_s *msg, ssize_t len)
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

static void send_entity_descriptor_response(struct aecp_data_unit_s *request_msg, uint16_t configuration_index)
{
  if (s_state == NULL || s_state->socket < 0) {
    ESP_LOGE(TAG, "Socket not ready to send AECP response");
    return;
  }

  struct aecp_read_descriptor_response_s response = {0};

  /* Copy Ethernet header from request and swap MAC addresses */
  memcpy(response.aecp_header.header.dst_mac, request_msg->header.src_mac, ETH_ADDR_LEN);
  memcpy(response.aecp_header.header.src_mac, s_state->intf_hw_addr, ETH_ADDR_LEN);

  /* Ethernet type (big-endian) */
  response.aecp_header.header.eth_type[0] = (ETH_TYPE_AVTP >> 8) & 0xFF;
  response.aecp_header.header.eth_type[1] = ETH_TYPE_AVTP & 0xFF;

  /* AECP header fields */
  response.aecp_header.subtype = AVTP_SUBTYPE_AECP;
  response.aecp_header.message_type = AECP_MSG_TYPE_ACM_RESPONSE;
  response.aecp_header.version = 0;
  response.aecp_header.h = 0;
  response.aecp_header.status = 0; // SUCCESS

  /* Calculate control_data_length: everything after the AECP common header */
  uint16_t control_data_length = sizeof(response) - sizeof(struct header_s) - 2; // header + subtype + control fields
  response.aecp_header.control_data_length = control_data_length;

  /* Swap entity IDs - we become the target, controller becomes the controller */
  response.aecp_header.target_entity_id = request_msg->controller_entity_id;
  response.aecp_header.controller_entity_id = request_msg->target_entity_id;
  response.aecp_header.sequence_id = request_msg->sequence_id; // Echo sequence ID
  response.aecp_header.command_type = htons(ACM_COMMAND_TYPE_READ_DESCRIPTOR);

  /* Response payload fields */
  response.configuration_index = 0;
  response.reserved = 0;

  /* Fill ENTITY descriptor */
  response.descriptor.descriptor_type = htons(0x0000); // ENTITY
  response.descriptor.descriptor_index = htons(0x0000);
  response.descriptor.entity_id = htonll(s_state->entity_id);
  response.descriptor.entity_model_id = htonll(s_state->entity_model_id);
  response.descriptor.entity_capabilities = htonl(0x0000C508); // Example capabilities
  response.descriptor.talker_stream_sources = htons(0);
  response.descriptor.talker_capabilities = htons(0);
  response.descriptor.listener_stream_sinks = htons(4);
  response.descriptor.listener_capabilities = htons(0x4001);
  response.descriptor.controller_capabilities = htonl(0);
  response.descriptor.available_index = htonl(s_state->adp_available_index);
  response.descriptor.association_id = htonll(0);

  /* Set entity name */
  const char *entity_name = "ESP32-AVB Entity";
  strncpy((char *)response.descriptor.entity_name, entity_name, sizeof(response.descriptor.entity_name));

  response.descriptor.vendor_name_string = htons(0);
  response.descriptor.model_name_string = htons(0);

  /* Set firmware version */
  const char *fw_version = "0.0.1";
  strncpy((char *)response.descriptor.firmware_version, fw_version, sizeof(response.descriptor.firmware_version));

  /* Set group name */
  const char *group_name = "ESP32-AVB";
  strncpy((char *)response.descriptor.group_name, group_name, sizeof(response.descriptor.group_name));

  /* Set serial number */
  char serial[65];
  snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
           s_state->intf_hw_addr[0], s_state->intf_hw_addr[1], s_state->intf_hw_addr[2],
           s_state->intf_hw_addr[3], s_state->intf_hw_addr[4], s_state->intf_hw_addr[5]);
  strncpy((char *)response.descriptor.serial_number, serial, sizeof(response.descriptor.serial_number));

  response.descriptor.configurations_count = htons(1);
  response.descriptor.current_configuration = htons(0);

  /* Send the response */
  ssize_t written = write(s_state->socket, &response, sizeof(response));
  if (written < 0) {
    ESP_LOGE(TAG, "Failed to send ENTITY descriptor response: %d", errno);
  } else {
    ESP_LOGI(TAG, "Sent ENTITY descriptor response (%zd bytes)", written);
  }
}

int aecp_acm_command_handle(struct aecp_data_unit_s *msg, ssize_t len)
{
  if (msg == NULL || len < sizeof(struct aecp_data_unit_s))
  {
    ESP_LOGE(TAG, "Invalid AECP ACM command message or length");
    return ESP_FAIL;
  }

  /* Convert command_type from network byte order to host byte order */
  uint16_t command_type = ntohs(msg->command_type);

  /* Check if the message is targeted to this entity */
  uint64_t target_entity_id = ntohll(msg->target_entity_id);
  if (target_entity_id != s_state->entity_id)
  {
    ESP_LOGW(TAG, "AECP message not for this entity (target: 0x%016llX, our: 0x%016llX)",
             (unsigned long long)target_entity_id, (unsigned long long)s_state->entity_id);
    return ESP_OK;
  }

  uint8_t *payload = (uint8_t *)msg + sizeof(struct aecp_data_unit_s);
  switch (command_type)
  {
  case ACM_COMMAND_TYPE_READ_DESCRIPTOR:
    {
      uint16_t configuration_index = ntohs(*(uint16_t *)(payload + 0));
      // uint16_t reserved = ntohs(*(uint16_t *)(payload + 2));
      uint16_t descriptor_type = ntohs(*(uint16_t *)(payload + 4));
      uint16_t descriptor_index = ntohs(*(uint16_t *)(payload + 6));

      switch (descriptor_type)
      {
      case 0x0000: // ENTITY Descriptor
        ESP_LOGI(TAG, "AECP Read ENTITY Descriptor Request from (Config Index: %d, Descriptor Index: %d)",
                 configuration_index, descriptor_index);
        send_entity_descriptor_response(msg, configuration_index);
        break;
      default:
        ESP_LOGW(TAG, "Unsupported ACM read descriptor type: 0x%04X", descriptor_type);
        break;
      }
    }
    break;
    case ACM_COMMAND_TYPE_REGISTER_UNSOLICITED_NOTIFICATION:
    {
      ESP_LOGI(TAG, "AECP Register Unsolicited Notification Command");

      /* Check if message has payload (flags field) */
      ssize_t payload_offset = sizeof(struct aecp_data_unit_s);
      bool time_limited = 0;

      if (len > payload_offset) {
        /* Payload exists, read flags and extract time_limited bit */
        uint32_t flags = ntohl(*(uint32_t *)(payload + 0));
        time_limited = flags & 0x1; // Least significant bit
        ESP_LOGI(TAG, "  Flags: 0x%08X, Time Limited: %d", flags, time_limited);
      } else {
        /* No payload, time_limited defaults to 0 */
        ESP_LOGI(TAG, "  No payload, Time Limited: 0");
      }
    }
    break;
    case ACM_COMMAND_TYPE_UNREGISTER_UNSOLICITED_NOTIFICATION:
      ESP_LOGI(TAG, "AECP Register Unsolicited Notification Command");
    break;
  default:
    ESP_LOGW(TAG, "Unhandled AECP ACM command type: 0x%04X", command_type);
  }

  return ESP_OK;
}

int aecp_net_rx(struct aecp_data_unit_s *msg, ssize_t len)
{
  if (msg == NULL || len < sizeof(struct aecp_data_unit_s))
  {
    ESP_LOGE(TAG, "Invalid AECP message or length");
    return ESP_FAIL;
  }

  switch (msg->message_type)
  {
    case AECP_MSG_TYPE_ACM_COMMAND:
      ESP_LOGI(TAG, "AECP ACM Command Message Received");
      aecp_acm_command_handle(msg, len);
      break;
    case AECP_MSG_TYPE_ACM_RESPONSE:
      ESP_LOGI(TAG, "AECP ACM Response Message Received");
      break;
    default:
      ESP_LOGW(TAG, "Unknown AECP message type: 0x%X", msg->message_type);
      break;
  }

  return ESP_OK;
}

static uint64_t mac_to_entity_id(uint64_t mac)
{
  return ((mac & 0xffffff000000) << 16) | (0xfffe000000) | (mac & 0xffffff);
}

static void adp_upsert_entity(struct avtp_discovery_msg_s *msg)
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
                                     ((uint32_t)msg->controller_capabilities[2] << 8)  |
                                     ((uint32_t)msg->controller_capabilities[3]);
  uint32_t available_index = ((uint32_t)msg->available_index[0] << 24) |
                             ((uint32_t)msg->available_index[1] << 16) |
                             ((uint32_t)msg->available_index[2] << 8)  |
                             ((uint32_t)msg->available_index[3]);

  uint8_t *src_mac = msg->header.src_mac;

  /* Valid time (5 bits) doubled in seconds */
  uint8_t valid_time = msg->control_data_length_field.valid_time & 0x1F;
  time_t now = time(NULL);
  time_t valid_until = now + (valid_time * 2);

  /* Search for existing entry or free slot */
  int free_index = -1;
  for (int i = 0; i < MAX_ADP_ENTITIES; ++i) {
    if (s_state->adp_entities[i].in_use) {
      if (s_state->adp_entities[i].entity_id == entity_id) {
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
    } else if (free_index < 0) {
      free_index = i; /* remember first free slot */
    }
  }

  if (free_index < 0) {
    ESP_LOGW(TAG, "ADP entity list full; cannot add 0x%016llX", (unsigned long long)entity_id);
    return;
  }

  /* Add new entry */
  struct adp_entity_entry_s *entry = &s_state->adp_entities[free_index];
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

  ESP_LOGI(TAG, "Added ADP entity 0x%016llX (MAC: %02X:%02X:%02X:%02X:%02X:%02X, TalkerSrc=%u, ListenerSinks=%u, valid %us)",
           (unsigned long long)entity_id,
           src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5],
           talker_stream_sources,
           listener_stream_sinks,
           (unsigned)(valid_time * 2));
}

static void adp_remove_entity(struct avtp_discovery_msg_s *msg)
{
  if (!s_state) return;

  /* Extract entity_id (network -> host) */
  uint64_t entity_id_net;
  memcpy(&entity_id_net, msg->entity_id, sizeof(entity_id_net));
  uint64_t entity_id = ntohll(entity_id_net);

  /* Search for entity and mark as not in use */
  for (int i = 0; i < MAX_ADP_ENTITIES; ++i) {
    if (s_state->adp_entities[i].in_use && s_state->adp_entities[i].entity_id == entity_id) {
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
  if (s_state == NULL || s_state->socket < 0) {
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
  msg.control_data_length_field.valid_time = 10;  /* Set valid_time as needed */
  msg.control_data_length_field.raw_u16 = htons(msg.control_data_length_field.raw_u16);

  memcpy(msg.entity_capabilities, (uint8_t[]){0x00, 0x00, 0xC5, 0x08}, 4); // Example capabilities

  /* Set 4 listener stream sinks (big-endian 0x0004) */
  msg.listener_stream_sinks[0] = 0x00;
  msg.listener_stream_sinks[1] = 0x04;
  msg.listener_capabilities[0] = 0x40;
  msg.listener_capabilities[1] = 0x01;

  /* Use incremented available_index from state (big-endian) */
  msg.available_index[0] = (s_state->adp_available_index >> 24) & 0xFF;
  msg.available_index[1] = (s_state->adp_available_index >> 16) & 0xFF;
  msg.available_index[2] = (s_state->adp_available_index >> 8) & 0xFF;
  msg.available_index[3] = s_state->adp_available_index++ & 0xFF;

  memset(msg.association_id, 0x00, sizeof(msg.association_id));
  memcpy(msg.gptp_grandmaster_id, (uint8_t[]){0x00,0x01,0xf2,0xff,0xfe, 0x00, 0xae, 0x35}, 8); // Example grandmaster ID

  ssize_t written = write(s_state->socket, &msg, 82);
  if (written < 0) {
    ESP_LOGE(TAG, "Failed to send ADP entity available: %d", errno);
  } else {
    ESP_LOGI(TAG, "Sent ADP Entity Available (%zd bytes)", written);
  }
}

static int64_t timespec_to_ms(const struct timespec *ts)
{
  return ts->tv_sec * 1000  + (ts->tv_nsec / 1000000ll);
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
      // TODO implement discovery state machine like in IEEE 1722-2022 p. 60
      switch (buf.adp_msg.subtype)
      {
      case AVTP_SUBTYPE_ADP:
        ESP_LOGI(TAG, "AVDECC Discovery Protocol received");
        adp_net_rx(&buf.adp_msg, len);
        break;
      case AVTP_SUBTYPE_AECP:
        ESP_LOGI(TAG, "AVDECC Enumeration an Control Protocol received");
        aecp_net_rx(&buf.aecp_msg, len);
        break;
      case AVTP_SUBTYPE_MAAP:
        ESP_LOGI(TAG, "MAAP Announce received");
        break;
      default:
        ESP_LOGW(TAG, "Unknown AVTP subtype received: 0x%02X", buf.adp_msg.subtype);
        break;
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