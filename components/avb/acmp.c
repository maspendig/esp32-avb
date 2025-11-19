#include <esp_log.h>
#include "acmp.h"

#include <cc.h>
#include <esp_err.h>
#include <sys/errno.h>
#include <sys/unistd.h>
#include <string.h>
#include <assert.h>

#include "avtp.h"

#define TAG "acmp"

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


int send_acmp_message(const struct avtp_state_s *state)
{
  ESP_LOGI(TAG, "Sent ACMP Connect TX Command to %02X:%02X:%02X:%02X:%02X:%02X entity_id=0x%016llx",
            state->adp_entities[0].mac[0], state->adp_entities[0].mac[1],
            state->adp_entities[0].mac[2], state->adp_entities[0].mac[3],
            state->adp_entities[0].mac[4], state->adp_entities[0].mac[5],
            (unsigned long long)state->adp_entities[0].entity_id);
  struct acmp_du_s msg = {0};

  /* Set Ethernet header */
  const uint8_t acmp_multicast_mac[6] = {0x91, 0xE0, 0xF0, 0x01, 0x00, 0x00}; // ACMP multicast MAC
  memcpy(msg.header.dst_mac, acmp_multicast_mac, sizeof(msg.header.dst_mac));
  memcpy(msg.header.src_mac, state->intf_hw_addr, sizeof(msg.header.src_mac));

  /* Ethernet type (big-endian) */
  msg.header.eth_type[0] = (ETH_TYPE_AVTP >> 8) & 0xFF;
  msg.header.eth_type[1] = ETH_TYPE_AVTP & 0xFF;

  /* ACMP header fields */
  msg.header.subtype = AVTP_SUBTYPE_ACMP;
  msg.header.h = 0;
  msg.header.version = 0;
  msg.header.message_type = ACMP_MSG_TYPE_CONNECT_TX_COMMAND;

  /* Calculate control_data_length: everything after the AECP common header */
  msg.header.control_data_length = htons(4);
  msg.header.status = 0; // SUCCESS

  /* ACMP payload - convert to network byte order */
  msg.stream_id[7] = 0x01;
  const uint64_t entity_id = htonll(state->entity_id);
  memcpy(msg.controller_entity_id, &entity_id, sizeof(msg.controller_entity_id));
  const uint64_t talker_entity_id = htonll(state->adp_entities[0].entity_id);
  memcpy(msg.talker_entity_id, &talker_entity_id, sizeof(msg.talker_entity_id));
  memcpy(msg.listener_entity_id, &entity_id, sizeof(msg.listener_entity_id));
  msg.talker_unique_id = htons(0);
  msg.listener_unique_id = htons(0);
  memcpy(msg.stream_dest_mac, state->adp_entities[0].mac, sizeof(msg.stream_dest_mac));
  msg.connection_count = htons(0);
  msg.sequence_id = htons(1);
  msg.flags = htons(0);
  msg.stream_vlan_id = htons(0);
  msg.reserved = htons(0);

  /* Debug: dump controller_entity_id bytes (network order) */
  uint8_t *cid = (uint8_t *)&msg.controller_entity_id;
  ESP_LOGI(TAG, "controller_entity_id host=0x%016llx net_bytes=%02X %02X %02X %02X %02X %02X %02X %02X",
           (unsigned long long)state->entity_id,
           cid[0], cid[1], cid[2], cid[3], cid[4], cid[5], cid[6], cid[7]);

  /* Send the message */
  const ssize_t written = write(state->socket, &msg, sizeof(msg));
  if (written < 0) {
    ESP_LOGE(TAG, "Failed to send ACMP Message: %d (errno: %d)", written, errno);
    return ESP_FAIL;
  } else {
    ESP_LOGI(TAG, "Sent ACMP Connect TX Command", written);
    return ESP_OK;
  }
}
void acmp_net_rx(struct acmp_du_s *msg, ssize_t len)
{
    ESP_LOGI(TAG, "ATDECC Connection Management Protocol received");
}
