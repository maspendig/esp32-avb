//
// Created by max on 12/27/25.
//

#ifndef ETHERNET_PTP_MAAP_H
#define ETHERNET_PTP_MAAP_H
#include "types.h"
#include <sys/types.h>
#include <esp_timer.h>

#define MAAP_PROBE_RETRANSMITS 3
#define MAAP_PROBE_INTERVAL_BASE_MS 500
#define MAAP_PROBE_INTERVAL_VARIATION_MS 100
#define MAAP_ANNOUNCE_INTERVAL_BASE_MS  30000
#define MAAP_ANNOUNCE_INTERVAL_VARIATION_MS 2000

#define MAAP_PROBE_MIN_INTERVAL_MS MAAP_PROBE_INTERVAL_BASE_MS
#define MAAP_PROBE_MAX_INTERVAL_MS (MAAP_PROBE_INTERVAL_BASE_MS + MAAP_PROBE_INTERVAL_VARIATION_MS)
#define MAAP_ANNOUNCE_MIN_INTERVAL_MS MAAP_ANNOUNCE_INTERVAL_BASE_MS
#define MAAP_ANNOUNCE_MAX_INTERVAL_MS (MAAP_ANNOUNCE_INTERVAL_BASE_MS + MAAP_ANNOUNCE_INTERVAL_VARIATION_MS)

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

typedef enum
{
  MAAP_STATE_INITIAL = 0,
  MAAP_STATE_PROBE,
  MAAP_STATE_DEFEND,
} maap_state;

typedef enum
{
  MAAP_EVENT_BEGIN = 0,
  MAAP_EVENT_RELEASE,
  MAAP_EVENT_RESTART,
  MAAP_EVENT_RESERVE_ADDRESS,
  MAAP_EVENT_RPROBE,
  MAAP_EVENT_RDEFEND,
  MAAP_EVENT_RANNOUNCE,
  MAAP_EVENT_PROBE_COUNT,
  MAAP_EVENT_ANNOUNCE_TIMER,
  MAAP_EVENT_PROBE_TIMER,
  MAAP_EVENT_PORT_OPERATIONAL,
} maap_event;

struct maap_db_s
{
  u8 mac[6];
  u8 state;
  u8 event;
  u8 probe_count;
  esp_timer_handle_t probe_timer_handle;
  esp_timer_handle_t announce_timer_handle;
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
void maap_init(struct avtp_state_s* state);
#endif //ETHERNET_PTP_MAAP_H
