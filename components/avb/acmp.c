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
  const uint8_t acmp_multicast_mac[6] = {0x91, 0xE0, 0xF0, 0x01, 0x00, 0x00}; // ACMP multicast MAC
  memcpy(msg->header.dst_mac, acmp_multicast_mac, sizeof(msg->header.dst_mac));
  memcpy(msg->header.src_mac, state->intf_hw_addr, sizeof(msg->header.src_mac));

  /* Ethernet type (big-endian) */
  msg->header.eth_type[0] = (ETH_TYPE_AVTP >> 8) & 0xFF;
  msg->header.eth_type[1] = ETH_TYPE_AVTP & 0xFF;

  msg->header.subtype = AVTP_SUBTYPE_ACMP;
  msg->header.h = 0;
  msg->header.version = 0;
  msg->header.message_type = msg_type;
  ACMP_SET_CTRL_DATA_STATUS((&msg->header), status, length);
}

void acmp_set_common_du(struct avtp_state_s* state, struct acmp_du_s* msg)
{
  const uint64_t entity_id = htonll(state->entity_id);
  memcpy(msg->controller_entity_id, &entity_id, sizeof(msg->controller_entity_id));
  msg->talker_unique_id = htons(0);
  msg->listener_unique_id = htons(0);
  msg->connection_count = htons(0);
  msg->sequence_id = htons(state->acmp_sequence_id++);
  msg->flags = htons(0);
  msg->stream_vlan_id = htons(0);
  msg->reserved = htons(0);
  msg->stream_id[7] = 0x01;
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
  const uint64_t entity_id = htonll(state->entity_id);
  const uint64_t talker_entity_id = htonll(state->adp_entities[0].entity_id);
  memcpy(msg.talker_entity_id, &talker_entity_id, sizeof(msg.talker_entity_id));
  memcpy(msg.listener_entity_id, &entity_id, sizeof(msg.listener_entity_id));

  const uint8_t zero_mac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  memcpy(msg.stream_dest_mac, zero_mac, sizeof(msg.stream_dest_mac));

  /* Send the message */
  return send_msg(state->socket, &msg, sizeof(msg));
}

int send_acmp_connect_rx_command(struct avtp_state_s* state, uint8_t msg_type)
{
  struct acmp_du_s msg = {0};

  acmp_set_common_header(state, &msg, msg_type, 44, 0);
  acmp_set_common_du(state, &msg);
  auto talker = &(state->adp_entities[0]);

  /* ACMP payload - convert to network byte order */
  const uint64_t entity_id = htonll(state->entity_id);
  const uint64_t talker_entity_id = htonll(talker->entity_id);
  memcpy(msg.talker_entity_id, &entity_id, sizeof(msg.talker_entity_id));
  memcpy(msg.listener_entity_id, &talker_entity_id, sizeof(msg.listener_entity_id));

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

void handle_acmp_connect_tx_response(struct avtp_state_s* state, struct acmp_du_s* msg)
{
  const auto status = ACMP_GET_STATUS(&msg->header);
  /* Check status */
  if (status != 0)
  {
    ESP_LOGE(TAG, "ACMP Connect TX Response failed with status: %d", status);
    return;
  }

  /* Connection established successfully */
  ESP_LOGI(TAG, "ACMP Connect TX successful, connection established.");
  // send_acmp_message(state, ACMP_MSG_TYPE_CONNECT_RX_COMMAND);
}

int handle_acmp_connect_tx_command(struct avtp_state_s* state, struct acmp_du_s* msg)
{
  struct acmp_du_s resp = {0};

  uint64_t talker_entity_id = htonll(*(uint64_t *)msg->talker_entity_id);

  // TODO check if this condition is correct.
  // We are a listener receiving a connect tx command from a talker
  // So the check should be is listener_entity_id == our entity_id, right?

  // if (talker_entity_id != state->entity_id)
  // {
  //   ESP_LOGW(TAG, "Ignoring foreign ACMP Connect TX Command (target: 0x%016llX, our: 0x%016llX).",
  //            (unsigned long long)talker_entity_id, (unsigned long long)state->entity_id);
  //   return ESP_OK;
  // }

  acmp_set_common_header(state, &resp, ACMP_MSG_TYPE_CONNECT_TX_RESPONSE, 44, 0);
  acmp_set_common_du(state, &resp);

  resp.sequence_id = msg->sequence_id;

  /* ACMP payload - convert to network byte order */
  memcpy(resp.talker_entity_id, &msg->talker_entity_id, sizeof(resp.talker_entity_id));
  memcpy(resp.listener_entity_id, &msg->listener_entity_id, sizeof(resp.listener_entity_id));

  memcpy(resp.stream_dest_mac, msg->stream_dest_mac, sizeof(resp.stream_dest_mac));

  resp.connection_count = htons(1); // One connection established
  resp.stream_vlan_id = msg->stream_vlan_id; // Keep the same VLAN ID

  if (send_msg(state->socket, &resp, sizeof(resp)) != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to send ACMP Connect TX Response");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "Sent ACMP Connect TX Response to Talker 0x%016llX",
           (unsigned long long)talker_entity_id);
  return ESP_OK;
}

int handle_acmp_connect_rx_response(struct avtp_state_s* state, struct acmp_du_s* msg)
{
  const auto status = ACMP_GET_STATUS(&msg->header);
  /* Check status */
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

void acmp_net_rx(struct avtp_state_s* state, struct acmp_du_s* msg, ssize_t len)
{
  switch (msg->header.message_type)
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
  default:
    ESP_LOGW(TAG, "Received unimplemented ACMP message type: 0x%1X", msg->header.message_type);
  }
}
