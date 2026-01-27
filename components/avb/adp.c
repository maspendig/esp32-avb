//
// Created by max on 11/20/25.
//

#include "adp.h"

#include <avtp.h>
#include <cc.h>
#include <config.h>
#include <esp_eth_spec.h>
#include <esp_log.h>
#include <sys/types.h>
#include <sys/unistd.h>

#define TAG "adp"

void set_entity_values(struct adp_entity_entry_s* entity, struct avtp_discovery_msg_s* msg)
{
  /* Valid time (5 bits) doubled in seconds */
  uint8_t valid_time = msg->control_data_length_field.valid_time & 0x1F;
  time_t now = time(NULL);
  time_t valid_until = now + (valid_time * 2);
  /* Update existing entry */
  entity->talker_stream_sources = ntohs(msg->talker_stream_sources);
  entity->talker_capabilities = ntohs(msg->talker_capabilities);
  entity->listener_stream_sinks = ntohs(msg->listener_stream_sinks);
  entity->listener_capabilities = ntohs(msg->listener_capabilities);
  entity->controller_capabilities = ntohs(msg->controller_capabilities);
  entity->available_index = ntohl(msg->available_index);
  memcpy(entity->mac, msg->header.src_mac, sizeof(entity->mac));
  entity->valid_until = valid_until;
}

static void adp_upsert_entity(struct avtp_state_s* s_state, struct avtp_discovery_msg_s* msg)
{
  uint64_t entity_id = ntohll(msg->entity_id);

  /* Search for existing entry or free slot */
  int free_index = -1;
  for (int i = 0; i < MAX_ADP_ENTITIES; ++i)
  {
    if (s_state->adp_entities[i].in_use)
    {
      if (s_state->adp_entities[i].entity_id == entity_id)
      {
        set_entity_values(&s_state->adp_entities[i], msg);
        ESP_LOGI(TAG, "Updated ADP entity 0x%016llX (valid %us)",
                 (unsigned long long)entity_id, (unsigned)(s_state->adp_entities[i].valid_until * 2));
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
  set_entity_values(entry, msg);
  entry->in_use = true;

  ESP_LOGI(
    TAG, "Added ADP entity 0x%016llX (MAC: %02X:%02X:%02X:%02X:%02X:%02X, TalkerSrc=%u, ListenerSinks=%u)",
    (unsigned long long)entity_id,
    entry->mac[0], entry->mac[1], entry->mac[2], entry->mac[3], entry->mac[4], entry->mac[5],
    entry->talker_stream_sources,
    entry->listener_stream_sinks);
}

static void adp_remove_entity(struct avtp_state_s* s_state, struct avtp_discovery_msg_s* msg)
{
  if (!s_state) return;

  uint64_t entity_id = ntohll(msg->entity_id);

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

  msg.entity_id = htonll(s_state->entity_id);
  msg.entity_model_id = htonll(s_state->entity_model_id);

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

  msg.entity_capabilities = htonl(CONFIG_ENTITY_CAPABILITIES);
  msg.talker_capabilities = htons(CONFIG_TALKER_CAPABILITIES);
  msg.talker_stream_sources = htons(CONFIG_TALKER_STREAM_SOURCES);
  msg.listener_stream_sinks = htons(CONFIG_LISTENER_STREAM_SINKS);
  msg.listener_capabilities = htons(CONFIG_LISTENER_CAPABILITIES);
  msg.available_index = htonl(s_state->adp_available_index++);
  msg.association_id = htonll(0);

  // TODO get grandmaster from PTP module
  memcpy(msg.gptp_grandmaster_id, (uint8_t[]){0x00, 0x01, 0xf2, 0xff, 0xfe, 0x00, 0xae, 0x35}, 8);

  ssize_t written = write(s_state->socket, &msg, 82);
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send ADP entity available: %d", errno);
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
