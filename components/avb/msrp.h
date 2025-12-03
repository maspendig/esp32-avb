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

#define MSRP_FAIL_BANDWIDTH	1
#define MSRP_FAIL_BRIDGE	2
#define MSRP_FAIL_TC_BANDWIDTH	3
#define MSRP_FAIL_ID_BUSY	4
#define MSRP_FAIL_DSTADDR_BUSY	5
#define MSRP_FAIL_PREEMPTED	6
#define MSRP_FAIL_LATENCY_CHNG	7
#define MSRP_FAIL_PORT_NOT_AVB	8
#define MSRP_FAIL_DSTADDR_FULL	9
#define MSRP_FAIL_MSRP_RESOURCE	10
#define MSRP_FAIL_MMRP_RESOURCE	11
#define MSRP_FAIL_DSTADDR_FAIL	12
#define MSRP_FAIL_PRIO_NOT_SR	13
#define MSRP_FAIL_FRAME_SIZE	14
#define MSRP_FAIL_FANIN_EXCEED	15
#define MSRP_FAIL_STREAM_CHANGE	16
#define MSRP_FAIL_VLAN_BLOCKED	17
#define MSRP_FAIL_VLAN_DISABLED	18
#define MSRP_FAIL_SR_PRIO_ERR	19

/* Class ID defitions */
#define MSRP_SR_CLASS_A	6
#define MSRP_SR_CLASS_B	5

/* default values for class priorities */
#define MSRP_SR_CLASS_A_PRIO	3
#define MSRP_SR_CLASS_B_PRIO	2

#define MSRP_MULTICAST_MAC (u8[6]){0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E}; // MSRP multicast MAC

struct msrp_header_s
{
  struct header_s header;
  u8 protocol_version;
  u8 attribute_type;
  u8 attribute_length;
  u16 attribute_list_length;
};

typedef struct msrpdu_listen
{
  u64 stream_id;
} msrpdu_listen_t;

typedef struct msrpdu_talker_fail
{
  u64 stream_id;

  struct
  {
    u8 dest_addr[6];
    u16 vlan_id;
  } data_frame_params;

  struct
  {
    u16 max_frame_size;
    u16 max_interval_frames;
  } t_spec;

  u8 priority_and_rank;
  u32 accumulated_latency;

  struct
  {
    u64 bridge_id;
    u8 code;
  } failure;
} msrpdu_talker_fail_t;

/* Domain Discovery FirstValue definition */
typedef struct msrpdu_domain
{
  u8 sr_class_id;
  u8 sr_class_priority;
  u16 sr_class_vid;
} msrpdu_domain_t;

struct msrp_attribute
{
  struct msrp_attribute* prev;
  struct msrp_attribute* next;
  u32 type;

  union
  {
    msrpdu_talker_fail_t talk_listen;
    msrpdu_domain_t domain;
  } attribute;

  u32 substate; /*for listener events */
  u32 operation; /* DECLARE or REGISTER */
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
int msrp_send_listener_join_request(struct avtp_state_s* state, u64 stream_id);
int msrp_send_talker_advertise(struct avtp_state_s* state);
void msrp_send_domain_request(const struct avtp_state_s* state);

#endif //ETHERNET_PTP_MSRP_H
