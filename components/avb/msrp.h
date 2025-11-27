//
// Created by max on 11/26/25.
//

#ifndef ETHERNET_PTP_MSRP_H
#define ETHERNET_PTP_MSRP_H

#include "avtp.h"

#define MSRP_ATTRIBUTE_TYPE_TALKER_ADVERTISE 1
#define MSRP_ATTRIBUTE_TYPE_TALKER_FAILED 2
#define MSRP_ATTRIBUTE_TYPE_LISTENER 3
#define MSRP_ATTRIBUTE_TYPE_DOMAIN 4

#define ETH_TYPE_MSRP 0x22EA

struct msrp_header_s
{
  struct header_s header;
  u8 protocol_version;
  u8 attribute_type;
  u8 attribute_length;
  u16 attribute_list_length;
};

struct talker_advertise_s
{
  struct header_s header;
  u8 protocol_version;
  u8 attribute_type;
  u8 attribute_length;
  u16 attribute_list_length;
  u16 number_of_values;
  u64 stream_id;
  u8 stream_da[6];
  u16 stream_vlan_id;
  u16 max_frame_size;
  u16 max_frame_interval;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  u8 reserved : 4;
  u8 rank : 1;
  u8 priority : 3;
#else
  u8 priority : 3;
  u8 rank : 1;
  u8 reserved : 4;
#endif
  u32 accumulated_latency;
  u8 attribute_event;
  u16 end_mark_attribute_list;
  u16 end_mark;
} __attribute__((packed));

int msrp_init(const char* interface);
void read_msrp_net(const struct avtp_state_s* state);
int msrp_send_listener_join_request(struct avtp_state_s* state);
int msrp_send_talker_advertise(struct avtp_state_s* state);

#endif //ETHERNET_PTP_MSRP_H
