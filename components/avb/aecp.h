//
// Created by max on 11/20/25.
//

#ifndef ETHERNET_PTP_AECP_H
#define ETHERNET_PTP_AECP_H

#include "avtp.h"
#include <stdint.h>
#include <types.h>
#include <sys/types.h>

/* ATDECC Entity Model Command */
#define AECP_MSG_TYPE_AEM_COMMAND   0x0
/* ATDECC Entity Model Command response */
#define AECP_MSG_TYPE_AEM_RESPONSE  0x1

#define ACM_COMMAND_TYPE_READ_DESCRIPTOR 0x0004
#define ACM_COMMAND_TYPE_REGISTER_UNSOLICITED_NOTIFICATION 0x0024
#define ACM_COMMAND_TYPE_UNREGISTER_UNSOLICITED_NOTIFICATION 0x0025
#define ACM_COMMAND_TYPE_IDENTIFY_NOTIFICATION 0x0026

struct aecp_data_unit_s
{
  struct header_s header;
  u8 subtype; // 1 octet

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  u8 message_type : 4; // 4 bits
  u8 version : 3; // 3 bits
  u8 h : 1; // 1 bit (header specific)
#else
  uint8_t h : 1; // 1 bit (header specific)
  uint8_t version : 3; // 3 bits
  uint8_t message_type : 4; // 4 bits
#endif

  u16 control_data_len_status; // 16 bits
  u64 target_entity_id; // 64 bits
  u64 controller_entity_id; // 64 bits
  u16 sequence_id; // 16 bits
  u16 command_type; // 16 bits (ACM command type)
} __attribute__((packed));

/* AECP READ_DESCRIPTOR Response structure */
struct aecp_read_descriptor_response_s
{
  struct aecp_data_unit_s aecp_header;
  uint16_t configuration_index; // 16 bits
  uint16_t reserved; // 16 bits
  struct atdecc_entity_descriptor_s descriptor;
} __attribute__((packed));

int aecp_net_rx(struct avtp_state_s* state, struct aecp_data_unit_s* msg, ssize_t len);
#endif //ETHERNET_PTP_AECP_H
