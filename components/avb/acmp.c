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
#include <esp_eth_spec.h>
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

void acmp_set_common_header(struct avtp_state_s* state, struct acmp_common_s* msg, uint8_t msg_type, uint16_t length,
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

void acmp_set_common_du(struct avtp_state_s* state, struct acmp_common_s* msg)
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

/**
 * Final answer from the talker to listeners connection request
 * contains information about streamId and destination MAC
 * initiates the SRP listener registration
 */
void handle_acmp_connect_tx_response(struct avtp_state_s* state, struct acmp_common_s* msg)
{
  if (state->entity_id != htonll(msg->listener_entity_id))
  {
    ESP_LOGI(TAG, "Ignoring foreign ACMP Connect TX Resp (target: 0x%016llX, our: 0x%016llX).",
             htonll(msg->listener_entity_id),
             state->entity_id);
    return;
  }

  struct acmp_common_s resp = {0};
  acmp_set_common_header(state, &resp, ACMP_MSG_TYPE_CONNECT_RX_RESPONSE, 44, ACMP_STATUS_SUCCESS);

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
    // TODO check need! most of the fields are already been set on connect_rx_command...
    struct listener_stream_info_s* listenerInfo = &state->listener_stream_infos[listener_unique_id];
    listenerInfo->pending_connection = false;
    listenerInfo->connected = true;
    listenerInfo->stream_id = ntohll(msg->stream_id);
    memcpy(listenerInfo->stream_dest_mac, msg->stream_dest_mac, sizeof(listenerInfo->stream_dest_mac));
    listenerInfo->controller_entity_id = ntohll(msg->controller_entity_id);
    listenerInfo->flags = ntohs(msg->flags);
    // Apple always sends VLAN ID 0 in the Connect TX Response, but we use VLAN 2 always - like motu
    listenerInfo->stream_vlan_id = ntohs(2);
    listenerInfo->talker_unique_id = ntohs(msg->talker_unique_id);
    listenerInfo->talker_entity_id = ntohll(msg->talker_entity_id);

    resp.sequence_id = htons(listenerInfo->sequence_id);

    /* Join the MSRP stream reservation as a listener */
    // msrp_listener_join(state, listenerInfo->stream_id);

    ESP_LOGI(TAG, "Updated listener stream info [%u]: pending_connection=false (connection established)",
             listener_unique_id);
  }
  else
  {
    ESP_LOGW(TAG, "Listener unique ID %u exceeds MAX_LISTENER_STREAMS (%d), cannot update stream info",
             listener_unique_id, MAX_LISTENER_STREAMS);
  }


  // Copy relevant fields from the TX response
  resp.stream_id = msg->stream_id;
  resp.controller_entity_id = msg->controller_entity_id;
  resp.talker_entity_id = msg->talker_entity_id;
  resp.listener_entity_id = msg->listener_entity_id;
  resp.talker_unique_id = msg->talker_unique_id;
  resp.listener_unique_id = msg->listener_unique_id;
  memcpy(resp.stream_dest_mac, msg->stream_dest_mac, sizeof(resp.stream_dest_mac));
  resp.connection_count = msg->connection_count;
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

bool is_listener_connected(struct avtp_state_s* state, u64 listener_entity_id, u16 listener_unique_id)
{
  ESP_LOGI(TAG, "Checking if listener 0x%016llX (unique_id=%u) is already connected",
           (unsigned long long)listener_entity_id,
           listener_unique_id);
  struct listener_pair_s* conn_rx = state->talker_stream_info.connected_listeners;
  for (u16 i = 0; i < state->talker_stream_info.connection_count; i++)
  {
    if (conn_rx[i].listener_entity_id == listener_entity_id && conn_rx[i].listener_unique_id == listener_unique_id)
    {
      return true;
    }
  }
  return false;
}

void generate_stream_id(const u8 mac[6], const u8 stream_index, u64* stream_id)
{
  *stream_id = MAC_ARRAY_TO_U64(mac);
  // shift 16 bytes to left to make space for stream index
  *stream_id <<= 16;
  *stream_id += stream_index;
}

// Talker handling Connect TX Command
int handle_acmp_connect_tx_command(struct avtp_state_s* state, struct acmp_common_s* msg)
{
  struct acmp_common_s resp = {0};
  u16 status = ACMP_STATUS_SUCCESS;

  if (state->entity_id != htonll(msg->talker_entity_id))
  {
    ESP_LOGI(TAG, "Ignoring foreign ACMP Connect TX Command (target: 0x%016llX, our: 0x%016llX).",
             htonll(msg->talker_entity_id),
             state->entity_id);
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Received ACMP Connect TX Command");

  // TODO implement talkerIsAcquiredOrLockedByOther 1722.1-2021 p352
  // if (is_talker_unavailable(&msg))
  // {
  //   // Talker is unavailable, send response with appropriate status
  //   ACMP_SET_CTRL_DATA_STATUS(*resp, ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED, 44);
  //   status = ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED;
  // }

  acmp_set_common_header(state, &resp, ACMP_MSG_TYPE_CONNECT_TX_RESPONSE, 44, 0);

  // Save talker stream information
  u64 listener_entity_id = ntohll(msg->listener_entity_id);
  u16 listener_unique_id = ntohs(msg->listener_unique_id);

  // Initialize stream info on first connection
  if (state->talker_stream_info.connection_count == 0)
  {
    generate_stream_id(state->intf_hw_addr, 1, &state->talker_stream_info.stream_id);
    memcpy(state->talker_stream_info.stream_dest_mac, state->maap_db.start_mac, ETH_ADDR_LEN);
    state->talker_stream_info.stream_vlan_id = 2;

    /* Start MSRP Talker Advertisement with proper state machine integration
     * This will trigger the applicant state machine to send periodic advertisements */
    // TODO start msrp talker advertise properly
    // msrp_talker_advertise(state,
    //                       state->talker_stream_info.stream_id,
    //                       state->talker_stream_info.stream_dest_mac,
    //                       state->talker_stream_info.stream_vlan_id,
    //                       224, /* max_frame_size - typical for audio */
    //                       1, /* max_frame_interval */
    //                       MSRP_SR_CLASS_A_PRIO, /* priority - Class A */
    //                       1, /* rank - non-emergency */
    //                       100095); /* accumulated_latency in nanoseconds */
  }
  else if (is_listener_connected(state, listener_entity_id, listener_unique_id) == true)
  {
    ESP_LOGW(TAG, "Listener 0x%016llX (unique_id=%u) is already connected, ignoring duplicate connection",
             (unsigned long long)listener_entity_id,
             listener_unique_id);
    return ESP_OK;
  }

  // Add listener to connected_listeners array
  if (state->talker_stream_info.connection_count < MAX_CONNECTED_LISTENERS)
  {
    const u16 index = state->talker_stream_info.connection_count;
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
    status = ACMP_STATUS_TALKER_NO_BANDWIDTH;
  }

  resp.sequence_id = msg->sequence_id;

  /* ACMP payload - convert to network byte order */
  resp.talker_entity_id = msg->talker_entity_id;
  resp.controller_entity_id = msg->controller_entity_id;
  resp.listener_entity_id = msg->listener_entity_id;

  // wait for MAAP address to be acquired
  memcpy(resp.stream_dest_mac, state->maap_db.start_mac, sizeof(resp.stream_dest_mac));

  resp.connection_count = htons(state->talker_stream_info.connection_count);
  resp.stream_id = htonll(state->talker_stream_info.stream_id);
  resp.stream_vlan_id = htons(2); // Keep the same VLAN ID

  ACMP_SET_CTRL_DATA_STATUS((&resp), status, 44);

  if (send_msg(state->socket, &resp, sizeof(resp)) != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to send ACMP Connect TX Response");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Sent ACMP Connect TX Response to Listener 0x%016llX", htonll(msg->listener_entity_id));
  return ESP_OK;
}

int handle_acmp_connect_rx_command(struct avtp_state_s* state, struct acmp_common_s* msg)
{
  if (state->entity_id != htonll(msg->listener_entity_id))
  {
    ESP_LOGI(TAG, "Ignoring foreign ACMP Connect RX Command (target: 0x%016llX, our: 0x%016llX).",
             htonll(msg->listener_entity_id),
             state->entity_id);
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Received ACMP Connect RX Command");

  struct acmp_common_s resp = {0};

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
  const u16 seq = state->acmp_sequence_id++;
  resp.sequence_id = htons(seq);
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
      listenerInfo->sequence_id = ntohs(msg->sequence_id);

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

int handle_acmp_connect_rx_response(struct avtp_state_s* state, struct acmp_common_s* msg)
{
  if (state->entity_id != htonll(msg->talker_entity_id))
  {
    ESP_LOGI(TAG, "Ignoring foreign ACMP Connect RX Response (target: 0x%016llX, our: 0x%016llX).",
             htonll(msg->talker_entity_id),
             state->entity_id);
    return ESP_OK;
  }

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
void handle_acmp_get_rx_state_command(struct avtp_state_s* state, struct acmp_common_s* msg)
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

  struct acmp_common_s resp = {0};

  acmp_set_common_header(state, &resp, ACMP_MSG_TYPE_GET_RX_STATE_RESPONSE, 44, ACMP_STATUS_SUCCESS);
  acmp_set_common_du(state, &resp);

  u16 listener_unique_id = ntohs(msg->listener_unique_id);
  if (listener_unique_id < MAX_LISTENER_STREAMS)
  {
    struct listener_stream_info_s* info = &state->listener_stream_infos[listener_unique_id];

    resp.connection_count = htons(info->connected ? 1 : 0);
    resp.stream_id = ntohll(info->stream_id);
    // we always use VLAN ID 2, as the motu does
    resp.stream_vlan_id = ntohs(2);
    memcpy(resp.stream_dest_mac, info->stream_dest_mac, sizeof(resp.stream_dest_mac));

    resp.talker_entity_id = ntohll(info->talker_entity_id);
    resp.talker_unique_id = ntohs(info->talker_unique_id);
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

void handle_acmp_disconnect_rx_command(struct avtp_state_s* state, struct acmp_common_s* msg)
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

  struct acmp_common_s resp = {0};
  memcpy(&resp, msg, sizeof(struct acmp_common_s));
  acmp_set_common_header(state, &resp, ACMP_MSG_TYPE_DISCONNECT_TX_COMMAND, 44, ACMP_STATUS_SUCCESS);

  struct listener_stream_info_s* listener_info = &state->listener_stream_infos[msg->listener_unique_id];
  // msrp_listener_leave(state, listener_info->stream_id);

  *listener_info = (struct listener_stream_info_s){0};
  listener_info->sequence_id = ntohs(msg->sequence_id);
  resp.sequence_id = htons(state->acmp_sequence_id++);

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

void handle_acmp_disconnect_tx_response(struct avtp_state_s* state, struct acmp_common_s* msg)
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

  struct acmp_common_s resp = {0};
  memcpy(&resp, msg, sizeof(struct acmp_common_s));

  acmp_set_common_header(state, &resp, ACMP_MSG_TYPE_DISCONNECT_RX_RESPONSE, 44, ACMP_STATUS_SUCCESS);
  resp.sequence_id = htons(state->listener_stream_infos[msg->listener_unique_id].sequence_id);

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

void disconnectTalker(struct avtp_state_s* state, struct acmp_common_s* msg)
{
  u64 listener_entity_id = ntohll(msg->listener_entity_id);
  u16 listener_unique_id = ntohs(msg->listener_unique_id);

  // Find and remove listener from connected_listeners array
  struct listener_pair_s* conn_rx = state->talker_stream_info.connected_listeners;
  u16 found_index = MAX_CONNECTED_LISTENERS;
  for (u16 i = 0; i < state->talker_stream_info.connection_count; i++)
  {
    if (conn_rx[i].listener_entity_id == listener_entity_id && conn_rx[i].listener_unique_id == listener_unique_id)
    {
      found_index = i;
      break;
    }
  }
  if (found_index < MAX_CONNECTED_LISTENERS)
  {
    // Shift remaining listeners down
    for (u16 i = found_index; i < state->talker_stream_info.connection_count - 1; i++)
    {
      conn_rx[i] = conn_rx[i + 1];
    }
    // Clear the last entry
    conn_rx[state->talker_stream_info.connection_count - 1] = (struct listener_pair_s){0};
    state->talker_stream_info.connection_count--;

    ESP_LOGI(TAG, "Disconnected listener 0x%016llX (unique_id=%u), remaining connections=%u",
             (unsigned long long)listener_entity_id,
             listener_unique_id,
             state->talker_stream_info.connection_count);
  }
  else
  {
    ESP_LOGW(TAG, "Listener 0x%016llX (unique_id=%u) not found in connected listeners",
             (unsigned long long)listener_entity_id,
             listener_unique_id);
  }

  if (state->talker_stream_info.connection_count == 0)
  {
    /* Withdraw MSRP Talker Advertisement when no more listeners */
    ESP_LOGI(TAG, "No more connected listeners, withdrawing talker advertisement");
    // TODO re-enable when MSRP leave is implemented
    // msrp_talker_leave(state, state->talker_stream_info.stream_id);

    /* Deallocate stream multicast MAC address */
    maap_release(state);
  }
}

void handle_acmp_disconnect_tx_command(struct avtp_state_s* state, struct acmp_common_s* msg)
{
  // check if we are the intended talker
  if (state->entity_id != htonll(msg->talker_entity_id))
  {
    ESP_LOGW(TAG, "Ignoring foreign ACMP DISCONNECT TX Command (target: 0x%016llX, our: 0x%016llX).",
             htonll(msg->listener_entity_id),
             state->entity_id);
    return;
  }
  if (is_listener_connected(state, ntohll(msg->listener_entity_id), ntohs(msg->listener_unique_id)) == false)
  {
    ESP_LOGW(TAG, "ACMP DISCONNECT TX Command: Listener 0x%016llX (unique_id=%u) not connected",
             (unsigned long long)ntohll(msg->listener_entity_id),
             ntohs(msg->listener_unique_id));
    return;
  }


  ESP_LOGI(TAG, "Received ACMP Disconnect TX Command");

  disconnectTalker(state, msg);
  // send DISCONNECT RX RESPONSE

  struct acmp_common_s resp = {0};
  memcpy(&resp, msg, sizeof(struct acmp_common_s));
  acmp_set_common_header(state, &resp, ACMP_MSG_TYPE_DISCONNECT_TX_RESPONSE, 44, ACMP_STATUS_SUCCESS);

  //send
  int result = send_msg(state->socket, &resp, sizeof(resp));
  if (result == ESP_OK)
  {
    ESP_LOGI(TAG, "Sent ACMP DISCONNECT TX Response to Listener");
  }
  else
  {
    ESP_LOGE(TAG, "Failed to send ACMP DISCONNECT TX Response");
  }
}

void acmp_net_rx(struct avtp_state_s* state, struct acmp_common_s* msg, ssize_t len)
{
  switch (msg->message_type)
  {
  case ACMP_MSG_TYPE_CONNECT_TX_COMMAND:
    handle_acmp_connect_tx_command(state, msg);
    break;
  case ACMP_MSG_TYPE_CONNECT_TX_RESPONSE:
    handle_acmp_connect_tx_response(state, msg);
    break;
  case ACMP_MSG_TYPE_CONNECT_RX_COMMAND:
    handle_acmp_connect_rx_command(state, msg);
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
  case ACMP_MSG_TYPE_DISCONNECT_RX_COMMAND:
    handle_acmp_disconnect_rx_command(state, msg);
    break;
  case ACMP_MSG_TYPE_DISCONNECT_TX_RESPONSE:
    handle_acmp_disconnect_tx_response(state, msg);
    break;
  case ACMP_MSG_TYPE_DISCONNECT_TX_COMMAND:
    handle_acmp_disconnect_tx_command(state, msg);
    break;
  case ACMP_MSG_TYPE_DISCONNECT_RX_RESPONSE:
    ESP_LOGW(TAG, "Received ACMP Disconnect RX Response - NOT IMPLEMENTED");
    break;
  default:
    ESP_LOGW(TAG, "Received unimplemented ACMP message type: 0x%1X", msg->message_type);
  }
}
