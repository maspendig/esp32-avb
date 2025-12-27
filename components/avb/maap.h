//
// Created by max on 12/27/25.
//

#ifndef ETHERNET_PTP_MAAP_H
#define ETHERNET_PTP_MAAP_H
#include "types.h"
#include <sys/types.h>

#define MAAP_PROBE_RETRANSMITS 3
#define MAAP_PROBE_INTERVAL_BASE_MS 500
#define MAAP_PROBE_INTERVAL_VARIATION_MS 100
#define MAAP_ANNOUNCE_INTERVAL_BASE_MS 30_000
#define MAAP_ANNOUNCE_INTERVAL_VARIATION_MS 2_000

#define MAAP_MSG_TYPE_PROBE        0x1
#define MAAP_MSG_TYPE_DEFEND       0x2
#define MAAP_MSG_TYPE_ANNOUNCE     0x3

/**
 * MAAP Reserved MAC addresses from IEEE Std 1722-2016, Table B.9 and B.10
 */
#define MAAP_MULTICAST_ADDR (u8[6]){0x91, 0xE0, 0xF0, 0x00, 0xFF, 0x00}
#define MAAP_DYNAMIC_POOL_MIN {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00}
#define MAAP_DYNAMIC_POOL_MAX {0x91, 0xE0, 0xF0, 0x00, 0xFD, 0xFF}
#define MAAP_LOCAL_POOL_MIN {0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x00}
#define MAAP_LOCAL_POOL_MAX {0x91, 0xE0, 0xF0, 0x00, 0xFE, 0xFF}
#define MAAP_RESERVED_POOL_MIN {0x91, 0xE0, 0xF0, 0x00, 0xFF, 0x01}
#define MAAP_RESERVED_POOL_MAX {0x91, 0xE0, 0xF0, 0x00, 0xFF, 0xFF}


struct maap_state_s
{
  u8 mac_address[6];
};

struct maap_pdu_s
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
};
#endif

  union
  {
    u16 maap_version_and_control_data_length;

    struct
    {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
      u16 control_data_length : 11; // 12 bits
      u16 maap_version : 5; // 4 bits
#else
      u16 maap_version : 5; // 4 bits
      u16 control_data_length : 11; // 12 bits
#endif
    };
  };

  u64 stream_id; // not used in MAAP, set to 0
  u8 requested_start_address[6];
  u16 requested_count;
  u8 conflicted_start_address[6];
  u16 conflicted_count;
} __attribute__((packed));

struct avtp_state_s;

void maap_net_rx(struct avtp_state_s* state, struct maap_pdu_s* msg, ssize_t len);
#endif //ETHERNET_PTP_MAAP_H
