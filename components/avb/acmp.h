//
// Created by max on 11/15/25.
//


#ifndef ETHERNET_PTP_ACMP_H
#define ETHERNET_PTP_ACMP_H

#include "avtp.h"

#define AVTP_SUBTYPE_ACMP 0xFC

#define ACMP_MSG_TYPE_CONNECT_TX_COMMAND   0x0
#define ACMP_MSG_TYPE_CONNECT_TX_RESPONSE  0x1
#define ACMP_MSG_TYPE_DISCONNECT_TX_COMMAND  0x2
#define ACMP_MSG_TYPE_DISCONNECT_TX_RESPONSE  0x3
#define ACMP_MSG_TYPE_GET_TX_STATE_COMMAND  0x4
#define ACMP_MSG_TYPE_GET_TX_STATE_RESPONSE  0x5
#define ACMP_MSG_TYPE_CONNECT_RX_COMMAND   0x6
#define ACMP_MSG_TYPE_CONNECT_RX_RESPONSE  0x7
#define ACMP_MSG_TYPE_DISCONNECT_RX_COMMAND  0x8
#define ACMP_MSG_TYPE_DISCONNECT_RX_RESPONSE  0x9
#define ACMP_MSG_TYPE_GET_RX_STATE_COMMAND  0xa
#define ACMP_MSG_TYPE_GET_RX_STATE_RESPONSE  0xb
#define ACMP_MSG_TYPE_GET_TX_CONNECTION_COMMAND  0xc
#define ACMP_MSG_TYPE_GET_TX_CONNECTION_RESPONSE  0xd

#define ACMP_STATUS_SUCCESS 0x00
#define ACMP_STATUS_LISTENER_TALKER_TIMEOUT 0x07

#define MULTICAST_ACMP_MAC ( 0x91e0f0010000ULL )

struct acmp_header_s
{
  uint8_t dst_mac[6];
  uint8_t src_mac[6];
  uint8_t eth_type[2];
  uint8_t subtype;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  uint8_t message_type : 4; // 4 bits
  uint8_t version : 3; // 3 bits
  uint8_t h : 1; // 1 bit (header specific)
#else
  uint8_t h : 1; // 1 bit (header specific)
  uint8_t version : 3; // 3 bits
  uint8_t message_type : 4; // 4 bits
#endif
  uint16_t control_data_len_status;
};

/* ATDECC Connection Management Protocol Data Unit */
struct acmp_du_s
{
  struct acmp_header_s header;
  uint8_t stream_id[8];
  uint8_t controller_entity_id[8];
  uint8_t talker_entity_id[8];
  uint8_t listener_entity_id[8];
  uint16_t talker_unique_id;
  uint16_t listener_unique_id;
  uint8_t stream_dest_mac[6];
  uint16_t connection_count;
  uint16_t sequence_id;
  uint16_t flags;
  uint16_t stream_vlan_id;
  uint16_t connected_listeners_entries;
  uint16_t ip_flags;
  uint16_t reserved;
  uint16_t source_port;
  uint16_t dest_port;
  uint64_t source_ip_address[2];
  uint64_t destination_ip_address[2];
};

#define ACMP_GET_STATUS(hdr) \
  ((ntohs((hdr)->control_data_len_status) >> 11) & 0x1F)

#define ACMP_GET_CTRL_DATA_LEN(hdr) \
  (ntohs(hdr->control_data_len_status) & 0x7FF)

#define ACMP_SET_CTRL_DATA_STATUS(hdr, status, cdl) \
  (hdr->control_data_len_status = htons(((status & 0x1F) << 11) | (cdl & 0x7FF)))

int send_acmp_connect_rx_command(struct avtp_state_s* state, uint8_t msg_type);
int send_acmp_connect_tx_command(struct avtp_state_s* state, uint8_t msg_type);
void acmp_net_rx(struct avtp_state_s* state, struct acmp_du_s* msg, ssize_t len);
#endif //ETHERNET_PTP_ACMP_H
