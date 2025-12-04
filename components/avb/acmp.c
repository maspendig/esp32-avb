#include <esp_log.h>

#include "avtp.h"
#include "acmp.h"
#include "types.h"

#include <cc.h>
#include <esp_err.h>
#include <sys/errno.h>
#include <sys/unistd.h>
#include <string.h>
#include <assert.h>
#include <config.h>
#include <msrp.h>

#define TAG "acmp"

void eui48_from_uint64(uint8_t* mac[6], uint64_t other)
{
  *mac[0] = (uint8_t)((other >> (5 * 8)) & 0xff);
  *mac[1] = (uint8_t)((other >> (4 * 8)) & 0xff);
  *mac[2] = (uint8_t)((other >> (3 * 8)) & 0xff);
  *mac[3] = (uint8_t)((other >> (2 * 8)) & 0xff);
  *mac[4] = (uint8_t)((other >> (1 * 8)) & 0xff);
  *mac[5] = (uint8_t)((other >> (0 * 8)) & 0xff);
}

void acmp_set_common_header(struct avtp_state_s* state, struct acmp_du_s* msg, uint8_t msg_type, uint16_t length,
                            uint8_t status)
{
  /* Set Ethernet header */
  memcpy(msg->header.dst_mac, ACMP_MULTICAST_MAC, sizeof(msg->header.dst_mac));
  memcpy(msg->header.src_mac, state->intf_hw_addr, sizeof(msg->header.src_mac));

  /* Ethernet type (big-endian) */
  msg->header.eth_type[0] = (ETH_TYPE_AVTP >> 8) & 0xFF;
  msg->header.eth_type[1] = ETH_TYPE_AVTP & 0xFF;

  msg->subtype = AVTP_SUBTYPE_ACMP;
  msg->h = 0;
  msg->version = 0;
  msg->message_type = msg_type;
  ACMP_SET_CTRL_DATA_STATUS(msg, status, length);
}

void acmp_set_common_du(struct avtp_state_s* state, struct acmp_du_s* msg)
{
  msg->controller_entity_id = htonll(state->entity_id);
  msg->talker_unique_id = htons(0);
  msg->listener_unique_id = htons(0);
  msg->connection_count = htons(0);
  msg->sequence_id = htons(state->acmp_sequence_id++);
  msg->flags = htons(0);
  msg->stream_vlan_id = htons(0);
  msg->reserved = htons(0);
  msg->stream_id = htonll(1);
}

int send_msg(int socket, void* buffer, int buflen)
{
  const ssize_t written = write(socket, buffer, buflen);
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send ACMP Message: %d (errno: %d)", written, errno);
    return ESP_FAIL;
  }
  return ESP_OK;
}

int send_acmp_connect_tx_command(struct avtp_state_s* state, uint8_t msg_type)
{
  ESP_LOGI(TAG, "Sent ACMP Connect TX Command to %02X:%02X:%02X:%02X:%02X:%02X entity_id=0x%016llx",
           state->adp_entities[0].mac[0], state->adp_entities[0].mac[1],
           state->adp_entities[0].mac[2], state->adp_entities[0].mac[3],
           state->adp_entities[0].mac[4], state->adp_entities[0].mac[5],
           (unsigned long long)state->adp_entities[0].entity_id);
  struct acmp_du_s msg = {0};

  acmp_set_common_header(state, &msg, msg_type, 44, 0);
  acmp_set_common_du(state, &msg);

  /* ACMP payload - convert to network byte order */
  const uint64_t talker_entity_id = htonll(state->adp_entities[0].entity_id);
  msg.talker_entity_id = talker_entity_id;
  msg.listener_entity_id = htonll(state->entity_id);

  const uint8_t zero_mac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  memcpy(msg.stream_dest_mac, zero_mac, sizeof(msg.stream_dest_mac));

  /* Send the message */
  return send_msg(state->socket, &msg, sizeof(msg));
}

// TODO msrp send talker advertise after connect rx command sent
int send_acmp_connect_rx_command(struct avtp_state_s* state, uint8_t msg_type)
{
  struct acmp_du_s msg = {0};

  acmp_set_common_header(state, &msg, msg_type, 44, 0);
  acmp_set_common_du(state, &msg);
  struct adp_entity_entry_s* talker = &(state->adp_entities[0]);

  msg.talker_entity_id = htonll(state->entity_id);
  msg.listener_entity_id = htonll(talker->entity_id);

  // FIXME - sure??!!
  const uint8_t zero_mac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  memcpy(msg.stream_dest_mac, zero_mac, sizeof(msg.stream_dest_mac));

  if (send_msg(state->socket, &msg, sizeof(msg)) != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to send ACMP Connect RX Command");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "Sent ACMP Connect RX Command to Talker 0x%016llX", (unsigned long long)talker->entity_id);
  return ESP_OK;
}

/**
 * Final answer from the talker to listeners connection request
 * contains information about streamId and destination MAC
 * initiates the SRP listener registration
 */
void handle_acmp_connect_tx_response(struct avtp_state_s* state, struct acmp_du_s* msg)
{
  const int status = ACMP_GET_STATUS(msg);
  /* Check status */
  if (status != 0)
  {
    ESP_LOGE(TAG, "ACMP Connect TX Response failed with status: %d", status);
    return;
  }

  /* Connection established successfully */
  ESP_LOGI(TAG, "ACMP Connect TX successful, connection established.");

  // Update listener stream info - set pending_connection to false
  u16 listener_unique_id = ntohs(msg->listener_unique_id);

  if (listener_unique_id < MAX_LISTENER_STREAMS)
  {
    struct listener_stream_info_s* listenerInfo = &state->listener_stream_infos[listener_unique_id];
    listenerInfo->pending_connection = false;
    listenerInfo->connected = true;
    listenerInfo->stream_id = ntohll(msg->stream_id);
    memcpy(listenerInfo->stream_dest_mac, msg->stream_dest_mac, sizeof(listenerInfo->stream_dest_mac));
    listenerInfo->controller_entity_id = ntohll(msg->controller_entity_id);
    listenerInfo->flags = ntohs(msg->flags);
    listenerInfo->stream_vlan_id = ntohs(msg->stream_vlan_id);
    listenerInfo->talker_unique_id = ntohs(msg->talker_unique_id);
    listenerInfo->talker_entity_id = ntohll(msg->talker_entity_id);

    msrp_send_listener_join_request(state, listenerInfo->stream_id);

    ESP_LOGI(TAG, "Updated listener stream info [%u]: pending_connection=false (connection established)",
             listener_unique_id);
  }
  else
  {
    ESP_LOGW(TAG, "Listener unique ID %u exceeds MAX_LISTENER_STREAMS (%d), cannot update stream info",
             listener_unique_id, MAX_LISTENER_STREAMS);
  }

  // Send CONNECT_RX_RESPONSE message
  struct acmp_du_s resp = {0};

  acmp_set_common_header(state, &resp, ACMP_MSG_TYPE_CONNECT_RX_RESPONSE, 44, ACMP_STATUS_SUCCESS);
  // Copy relevant fields from the TX response
  resp.stream_id = msg->stream_id;
  resp.controller_entity_id = msg->controller_entity_id;
  resp.talker_entity_id = msg->talker_entity_id;
  resp.listener_entity_id = msg->listener_entity_id;
  resp.talker_unique_id = msg->talker_unique_id;
  resp.listener_unique_id = msg->listener_unique_id;
  memcpy(resp.stream_dest_mac, msg->stream_dest_mac, sizeof(resp.stream_dest_mac));
  resp.connection_count = msg->connection_count;
  resp.sequence_id = msg->sequence_id;
  resp.flags = msg->flags;
  resp.stream_vlan_id = msg->stream_vlan_id;

  int result = send_msg(state->socket, &resp, sizeof(resp));

  if (result == ESP_OK)
  {
    ESP_LOGI(TAG, "Sent ACMP Connect RX Response (SUCCESS) to controller");
  }
  else
  {
    ESP_LOGE(TAG, "Failed to send ACMP Connect RX Response");
  }
}

int handle_acmp_connect_tx_command(struct avtp_state_s* state, struct acmp_du_s* msg)
{
  struct acmp_du_s resp = {0};

  if (state->entity_id != htonll(msg->talker_entity_id))
  {
    ESP_LOGI(TAG, "Ignoring foreign ACMP Connect TX Command (target: 0x%016llX, our: 0x%016llX).",
             htonll(msg->talker_entity_id),
             state->entity_id);
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Received ACMP Connect TX Command");

  acmp_set_common_header(state, &resp, ACMP_MSG_TYPE_CONNECT_TX_RESPONSE, 44, 0);
  acmp_set_common_du(state, &resp);

  resp.sequence_id = msg->sequence_id;

  /* ACMP payload - convert to network byte order */
  resp.talker_entity_id = msg->talker_entity_id;
  resp.listener_entity_id = msg->listener_entity_id;

  const u8 maap_mac[6] = {0x91, 0xe0, 0xf0, 0x00, 0xfe, 0x00}; // Example MAAP MAC
  memcpy(resp.stream_dest_mac, maap_mac, sizeof(resp.stream_dest_mac));

  resp.connection_count = htons(1); // One connection established
  resp.stream_vlan_id = msg->stream_vlan_id; // Keep the same VLAN ID

  if (send_msg(state->socket, &resp, sizeof(resp)) != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to send ACMP Connect TX Response");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Sent ACMP Connect TX Response to Listener 0x%016llX", htonll(msg->listener_entity_id));

  // Save talker stream information
  u64 listener_entity_id = ntohll(msg->listener_entity_id);
  u16 listener_unique_id = ntohs(msg->listener_unique_id);

  // Initialize stream info on first connection
  if (state->talker_stream_info.connection_count == 0)
  {
    state->talker_stream_info.stream_id = ntohll(resp.stream_id);
    memcpy(state->talker_stream_info.stream_dest_mac, resp.stream_dest_mac,
           sizeof(state->talker_stream_info.stream_dest_mac));
    state->talker_stream_info.stream_vlan_id = ntohs(resp.stream_vlan_id);
    state->talker_stream_info.connection_count = 0;
  }
  // Add listener to connected_listeners array
  if (state->talker_stream_info.connection_count < MAX_CONNECTED_LISTENERS)
  {
    u16 index = state->talker_stream_info.connection_count;
    state->talker_stream_info.connected_listeners[index].listener_entity_id = listener_entity_id;
    state->talker_stream_info.connected_listeners[index].listener_unique_id = listener_unique_id;
    state->talker_stream_info.connection_count++;

    ESP_LOGI(TAG, "Saved talker stream info: listener[%u]=0x%016llX (unique_id=%u), total_connections=%u",
             index,
             (unsigned long long)listener_entity_id,
             listener_unique_id,
             state->talker_stream_info.connection_count);
  }
  else
  {
    ESP_LOGW(TAG, "Maximum connected listeners (%d) reached, cannot add listener 0x%016llX",
             MAX_CONNECTED_LISTENERS,
             (unsigned long long)listener_entity_id);
  }

  return ESP_OK;
}

int handle_acmp_connect_rx_command(struct avtp_state_s* state, struct acmp_du_s* msg)
{
  ESP_LOGI(TAG, "Received ACMP Connect RX Command");

  if (state->entity_id != htonll(msg->listener_entity_id))
  {
    ESP_LOGW(TAG, "Ignoring foreign ACMP Connect RX Command (target: 0x%016llX, our: 0x%016llX).",
             htonll(msg->listener_entity_id),
             state->entity_id);
    return ESP_OK;
  }

  struct acmp_du_s resp = {0};

  acmp_set_common_header(state, &resp, ACMP_MSG_TYPE_CONNECT_TX_COMMAND, 44, 0);

  resp.stream_id = htonll(0);
  resp.controller_entity_id = msg->controller_entity_id;
  resp.talker_entity_id = msg->talker_entity_id;
  resp.listener_entity_id = msg->listener_entity_id;
  // TODO check if this is really simply returned!
  resp.talker_unique_id = msg->talker_unique_id;
  resp.listener_unique_id = msg->listener_unique_id;
  memcpy(resp.stream_dest_mac, msg->stream_dest_mac, sizeof(resp.stream_dest_mac));
  resp.connection_count = msg->connection_count;
  const u64 seq = state->acmp_sequence_id++;
  resp.sequence_id = htonll(seq);
  resp.flags = msg->flags;
  resp.stream_vlan_id = htons(0x0002);

  int result = send_msg(state->socket, &resp, sizeof(resp));

  if (result == ESP_OK)
  {
    // Save listener stream information
    u16 listener_unique_id = ntohs(resp.listener_unique_id);
    if (listener_unique_id < MAX_LISTENER_STREAMS)
    {
      struct listener_stream_info_s* listenerInfo = &state->listener_stream_infos[listener_unique_id];
      listenerInfo->talker_entity_id = ntohll(resp.talker_entity_id);
      listenerInfo->talker_unique_id = ntohs(resp.talker_unique_id);
      listenerInfo->pending_connection = true;

      ESP_LOGI(TAG, "Saved listener stream info [%u]: talker_entity_id=0x%016llX, talker_unique_id=%u, pending=true",
               listener_unique_id,
               (unsigned long long)listenerInfo->talker_entity_id,
               listenerInfo->talker_unique_id);
    }
    else
    {
      ESP_LOGW(TAG, "Listener unique ID %u exceeds MAX_LISTENER_STREAMS (%d), cannot save stream info",
               listener_unique_id, MAX_LISTENER_STREAMS);
    }
  }

  return result;
}

int handle_acmp_connect_rx_response(struct avtp_state_s* state, struct acmp_du_s* msg)
{
  u8 status = ACMP_GET_STATUS(msg);
  switch (status)
  {
  case ACMP_STATUS_SUCCESS:
    ESP_LOGI(TAG, "ACMP Connect RX successful, connection established.");
    break;
  case ACMP_STATUS_LISTENER_TALKER_TIMEOUT:
    ESP_LOGW(TAG, "ACMP Connect RX Response: Listener-Talker Timeout");
    return ESP_FAIL;
  default:
    ESP_LOGE(TAG, "ACMP Connect RX Response failed with status: %d", status);
    return ESP_FAIL;
  }

  /* Connection established successfully */
  return ESP_OK;
}

/* We as a listener are asked by the talker about the listening state */
void handle_acmp_get_rx_state_command(struct avtp_state_s* state, struct acmp_du_s* msg)
{
  // check if we are the intended listener
  if (state->entity_id != htonll(msg->listener_entity_id))
  {
    ESP_LOGW(TAG, "Ignoring foreign ACMP Get RX State Command (target: 0x%016llX, our: 0x%016llX).",
             htonll(msg->listener_entity_id),
             state->entity_id);
    return;
  }

  ESP_LOGI(TAG, "Received ACMP Get RX State Command");

  struct acmp_du_s resp = {0};

  acmp_set_common_header(state, &resp, ACMP_MSG_TYPE_GET_RX_STATE_RESPONSE, 44, ACMP_STATUS_SUCCESS);
  acmp_set_common_du(state, &resp);

  u16 listener_unique_id = ntohs(msg->listener_unique_id);
  if (listener_unique_id < MAX_LISTENER_STREAMS)
  {
    struct listener_stream_info_s* info = &state->listener_stream_infos[listener_unique_id];
    /* If an entry exists and is not pending, connection_count = 1, otherwise 0 */
    resp.connection_count = htons(info->pending_connection ? 0 : 1);
    resp.stream_id = ntohs(info->stream_id);
    resp.stream_vlan_id = ntohs(resp.stream_vlan_id);
    memcpy(resp.stream_dest_mac, info->stream_dest_mac, sizeof(resp.stream_dest_mac));

    resp.talker_entity_id = info->talker_entity_id;
    resp.talker_unique_id = info->talker_unique_id;
  }
  else
  {
    /* Out of range -> no connection */
    resp.connection_count = htons(0);
  }

  resp.controller_entity_id = msg->controller_entity_id;
  resp.listener_entity_id = msg->listener_entity_id;
  resp.listener_unique_id = msg->listener_unique_id;

  resp.sequence_id = msg->sequence_id;
  resp.flags = htons(0x0000); // Connected
  // send
  int result = send_msg(state->socket, &resp, sizeof(resp));
  if (result == ESP_OK)
  {
    ESP_LOGI(TAG, "Sent ACMP Get RX State Response to controller");
  }
  else
  {
    ESP_LOGE(TAG, "Failed to send ACMP Get RX State Response");
  }
}

void handle_acmp_disconnect_rx_command(struct avtp_state_s* state, struct acmp_du_s* msg)
{
  // remove listener stream info. if found and ok, return DISCONNECT_TX_COMMAND
  if (state->entity_id != htonll(msg->listener_entity_id))
  {
    ESP_LOGW(TAG, "Ignoring foreign ACMP DISCONNECT RX Command (target: 0x%016llX, our: 0x%016llX).",
             htonll(msg->listener_entity_id),
             state->entity_id);
    return;
  }

  ESP_LOGI(TAG, "Received ACMP Disconnect RX Command");
  state->listener_stream_infos[msg->listener_unique_id] = (struct listener_stream_info_s){0};
  struct acmp_du_s resp = {0};
  memcpy(&resp, msg, sizeof(struct acmp_du_s));
  resp.message_type = ACMP_MSG_TYPE_DISCONNECT_TX_COMMAND;

  memcpy(resp.header.dst_mac, msg->header.src_mac, sizeof(resp.stream_dest_mac));
  memcpy(resp.header.src_mac, msg->header.dst_mac, sizeof(resp.stream_dest_mac));

  //send
  int result = send_msg(state->socket, &resp, sizeof(resp));
  if (result == ESP_OK)
  {
    ESP_LOGI(TAG, "Sent ACMP DISCONNECT TX Command to Talker");
  }
  else
  {
    ESP_LOGE(TAG, "Failed to send ACMP DISCONNECT TX Command");
  }
}

void handle_acmp_disconnect_tx_response(struct avtp_state_s* state, struct acmp_du_s* msg)
{
  // remove listener stream info. if found and ok, return DISCONNECT_TX_COMMAND
  if (state->entity_id != htonll(msg->listener_entity_id))
  {
    ESP_LOGW(TAG, "Ignoring foreign ACMP DISCONNECT TX Response (target: 0x%016llX, our: 0x%016llX).",
             htonll(msg->listener_entity_id),
             state->entity_id);
    return;
  }

  ESP_LOGI(TAG, "Received ACMP Disconnect TX RESPONSE");
  struct acmp_du_s resp = {0};
  memcpy(&resp, msg, sizeof(struct acmp_du_s));
  resp.message_type = ACMP_MSG_TYPE_DISCONNECT_RX_RESPONSE;

  memcpy(resp.header.dst_mac, msg->header.src_mac, sizeof(resp.stream_dest_mac));
  memcpy(resp.header.src_mac, msg->header.dst_mac, sizeof(resp.stream_dest_mac));

  //send
  int result = send_msg(state->socket, &resp, sizeof(resp));
  if (result == ESP_OK)
  {
    ESP_LOGI(TAG, "Sent ACMP DISCONNECT RX Response to Talker");
  }
  else
  {
    ESP_LOGE(TAG, "Failed to send ACMP DISCONNECT RX Response");
  }
}

void acmp_net_rx(struct avtp_state_s* state, struct acmp_du_s* msg, ssize_t len)
{
  switch (msg->message_type)
  {
  case ACMP_MSG_TYPE_CONNECT_TX_COMMAND:
    handle_acmp_connect_tx_command(state, msg);
    break;
  case ACMP_MSG_TYPE_CONNECT_TX_RESPONSE:
    handle_acmp_connect_tx_response(state, msg);
    break;
  case ACMP_MSG_TYPE_CONNECT_RX_RESPONSE:
    handle_acmp_connect_rx_response(state, msg);
    break;
  case ACMP_MSG_TYPE_GET_RX_STATE_COMMAND:
    handle_acmp_get_rx_state_command(state, msg);
    break;
  case ACMP_MSG_TYPE_GET_TX_STATE_RESPONSE:
    ESP_LOGW(TAG, "Received ACMP Get TX State Response - NOT IMPLEMENTED");
    break;
  case ACMP_MSG_TYPE_CONNECT_RX_COMMAND:
    handle_acmp_connect_rx_command(state, msg);
    break;
  case ACMP_MSG_TYPE_DISCONNECT_RX_COMMAND:
    handle_acmp_disconnect_rx_command(state, msg);
    break;
  case ACMP_MSG_TYPE_DISCONNECT_TX_RESPONSE:
    handle_acmp_disconnect_tx_response(state, msg);
    break;
  default:
    ESP_LOGW(TAG, "Received unimplemented ACMP message type: 0x%1X", msg->message_type);
  }
}
