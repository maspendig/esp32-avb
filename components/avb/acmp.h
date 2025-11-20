//
// Created by max on 11/15/25.
//


#ifndef ETHERNET_PTP_ACMP_H
#define ETHERNET_PTP_ACMP_H

#include <sys/types.h>

#include <avtp.h>
#include "types.h"

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

/* ATDECC Connection Management Protocol Data Unit */
struct acmp_du_s
{
  struct header_s header;
  u8 subtype;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  u8 message_type : 4; // 4 bits
  u8 version : 3; // 3 bits
  u8 h : 1; // 1 bit (header specific)
#else
  u8 h : 1; // 1 bit (header specific)
  u8 version : 3; // 3 bits
  u8 message_type : 4; // 4 bits
#endif
  u16 control_data_len_status;
  u8 stream_id[8];
  u8 controller_entity_id[8];
  u8 talker_entity_id[8];
  u8 listener_entity_id[8];
  u16 talker_unique_id;
  u16 listener_unique_id;
  u8 stream_dest_mac[6];
  u16 connection_count;
  u16 sequence_id;
  u16 flags;
  u16 stream_vlan_id;
  u16 connected_listeners_entries;
  u16 ip_flags;
  u16 reserved;
  u16 source_port;
  u16 dest_port;
  u64 source_ip_address[2];
  u64 destination_ip_address[2];
};

#define ACMP_GET_STATUS(hdr) \
  ((ntohs((hdr)->control_data_len_status) >> 11) & 0x1F)

#define ACMP_GET_CTRL_DATA_LEN(hdr) \
  (ntohs(hdr->control_data_len_status) & 0x7FF)

#define ACMP_SET_CTRL_DATA_STATUS(hdr, status, cdl) \
  (hdr->control_data_len_status = htons(((status & 0x1F) << 11) | (cdl & 0x7FF)))

struct avtp_state_s;

int send_acmp_connect_rx_command(struct avtp_state_s* state, u8 msg_type);
int send_acmp_connect_tx_command(struct avtp_state_s* state, u8 msg_type);
void acmp_net_rx(struct avtp_state_s* state, struct acmp_du_s* msg, ssize_t len);
#endif //ETHERNET_PTP_ACMP_H
