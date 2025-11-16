#include <esp_log.h>
#include "acmp.h"

#include <cc.h>
#include <esp_err.h>
#include <sys/errno.h>
#include <sys/unistd.h>
#include <string.h>

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
  ESP_LOGI(TAG, "Sending ACMP message");
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
  msg.header.status = 0; // SUCCESS
  msg.header.control_data_length = 44; // Size of ACMP payload after AVTP header

  /* ACMP payload - convert to network byte order */
  msg.stream_id = htonll(0x123456789ABCDEF0ULL);
  msg.controller_entity_id = htonll(state->entity_id);
  msg.talker_entity_id = htonll(state->adp_entities[0].entity_id);
  msg.listener_entity_id = htonll(state->entity_id);
  msg.talker_unique_id = htons(0);
  msg.listener_unique_id = htons(0);
  memcpy(msg.stream_dest_mac, state->adp_entities[0].mac, sizeof(msg.stream_dest_mac));
  msg.connection_count = htons(0);
  msg.sequence_id = htons(1);
  msg.flags = htons(0);
  msg.stream_vlan_id = htons(0);
  msg.reserved = htons(0);

  /* Send the message */
  const ssize_t written = write(state->socket, &msg, sizeof(msg));
  if (written < 0) {
    ESP_LOGE(TAG, "Failed to send ACMP Message: %d (errno: %d)", written, errno);
    return ESP_FAIL;
  } else {
    ESP_LOGI(TAG, "Sent ACMP Connect RX Command (%zd bytes)", written);
    return ESP_OK;
  }
}
