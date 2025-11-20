//
// Created by max on 11/20/25.
//

#include "adp.h"

#include <avtp.h>
#include <cc.h>
#include <esp_eth_spec.h>
#include <esp_log.h>
#include <sys/types.h>
#include <sys/unistd.h>

#define TAG "adp"

static void adp_upsert_entity(struct avtp_state_s* s_state, struct avtp_discovery_msg_s* msg)
{
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

static void adp_remove_entity(struct avtp_state_s* s_state, struct avtp_discovery_msg_s* msg)
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

void send_adp_entity_available(struct avtp_state_s* s_state)
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

int adp_net_rx(struct avtp_state_s* state, struct avtp_discovery_msg_s* msg, ssize_t len)
{
  /* Convert control_data_length_field from network byte order for parsing */
  msg->control_data_length_field.raw_u16 = ntohs(msg->control_data_length_field.raw_u16);

  uint8_t msg_type = msg->control & AVTP_MSGTYPE_MASK; /* lower 4 bits of 15th byte */
  switch (msg_type)
  {
  case ADP_MSG_TYPE_ENTITY_AVAILABLE:
    ESP_LOGI(TAG, "ADP Entity Available Message received");
    adp_upsert_entity(state, msg);
    break;
  case ADP_MSG_TYPE_ENTITY_DEPARTING:
    ESP_LOGI(TAG, "ADP Entity Departing Message received");
    adp_remove_entity(state, msg);
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

