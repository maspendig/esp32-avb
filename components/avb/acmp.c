#include <esp_log.h>
#include "acmp.h"

#include <sys/errno.h>
#include <sys/unistd.h>

#include "avtp.h"

#define TAG "acmp"

void send_acmp_message(const struct avtp_state_s *state)
{
  ESP_LOGI(TAG, "Sending ACMP message");
  struct acmp_du_s msg = {0};
  msg.header.subtype = AVTP_SUBTYPE_ACMP;
  msg.header.h = 0;
  msg.header.version = 0;
  msg.header.message_type = ACMP_MSG_TYPE_CONNECT_TX_COMMAND;
  msg.header.status = 0; // SUCCESS
  msg.header.control_data_length = 44; // Size of ACMP payload after header
  msg.stream_id = 0x123456789ABCDEF0ULL;
  msg.controller_entity_id = state->entity_id;
  msg.talker_entity_id = state->adp_entities[0].entity_id;
  msg.listener_entity_id = state->entity_id;
  msg.talker_unique_id = 0;
  msg.listener_unique_id = 0;
  memcpy(msg.stream_dest_mac, state->adp_entities[0].mac, sizeof(msg.stream_dest_mac));
  msg.connection_count = 0;
  msg.sequence_id = 1;
  msg.flags = 0;
  msg.stream_vlan_id= 0;
  msg.reserved = 0;

  /* Send the response */
  ssize_t written = write(state->socket, &msg, sizeof(msg));
  if (written < 0) {
    ESP_LOGE(TAG, "Failed to send ACMP Message: %d", errno);
  } else {
    ESP_LOGI(TAG, "Sent  (%zd bytes)", written);
  }
}
