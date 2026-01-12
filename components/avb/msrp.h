//
// Created by max on 11/26/25.
//

#ifndef ETHERNET_PTP_MSRP_H
#define ETHERNET_PTP_MSRP_H

#include <mrp.h>
#include <stdbool.h>
#include <sys/time.h>

#include "types.h"

/* Forward declaration */
struct avtp_state_s;

#define MSRP_PROTOCOL_VERSION 0

typedef enum
{
  MSRP_TALKER_ADVERTISE = 1,
  MSRP_TALKER_FAILED = 2,
  MSRP_LISTENER = 3,
  MSRP_DOMAIN = 4,
} msrp_attribute_type_t;

#define MSRP_ATTRIBUTE_LENGTH_TALKER_ADVERTISE 0x19 // (25)
#define MSRP_ATTRIBUTE_LENGTH_TALKER_FAILED    0x22 // (34)
#define MSRP_ATTRIBUTE_LENGTH_LISTENER         0x08 // (8)
#define MSRP_ATTRIBUTE_LENGTH_DOMAIN           0x04 // (4)

/* Listener Declaration Types - IEEE 802.1Qat Table 35-3 */
#define MSRP_LISTENER_IGNORE       0
#define MSRP_LISTENER_ASKING_FAILED 1
#define MSRP_LISTENER_READY        2
#define MSRP_LISTENER_READY_FAILED 3

#define ETH_TYPE_MSRP 0x22EA

/* MSRP Failure Codes - IEEE 802.1Qat Table 35-6 */
#define MSRP_FAIL_BANDWIDTH     1
#define MSRP_FAIL_BRIDGE        2
#define MSRP_FAIL_TC_BANDWIDTH  3
#define MSRP_FAIL_ID_BUSY       4
#define MSRP_FAIL_DSTADDR_BUSY  5
#define MSRP_FAIL_PREEMPTED     6
#define MSRP_FAIL_LATENCY_CHNG  7
#define MSRP_FAIL_PORT_NOT_AVB  8
#define MSRP_FAIL_DSTADDR_FULL  9
#define MSRP_FAIL_MSRP_RESOURCE 10
#define MSRP_FAIL_MMRP_RESOURCE 11
#define MSRP_FAIL_DSTADDR_FAIL  12
#define MSRP_FAIL_PRIO_NOT_SR   13
#define MSRP_FAIL_FRAME_SIZE    14
#define MSRP_FAIL_FANIN_EXCEED  15
#define MSRP_FAIL_STREAM_CHANGE 16
#define MSRP_FAIL_VLAN_BLOCKED  17
#define MSRP_FAIL_VLAN_DISABLED 18
#define MSRP_FAIL_SR_PRIO_ERR   19

/* IEEE 802.1Q-2022 35.2.1.2 Direction */
#define MSRP_DIRECTION_TALKER   0
#define MSRP_DIRECTION_LISTENER 1

/* Class ID definitions */
#define MSRP_SR_CLASS_A      6
#define MSRP_SR_CLASS_B      5

/* Default values for class priorities */
#define MSRP_SR_CLASS_A_PRIO 3
#define MSRP_SR_CLASS_B_PRIO 2

#define MSRP_MULTICAST_MAC (u8[6]){0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E}


typedef struct msrp_state
{
  struct mrp_attribute mrp;
} msrp_state_t;

typedef struct msrp_attribute
{
  u8 attribute_type;
  /* indicates the length of the FirstValue field */
  u8 attribute_length;
  /* Indicates the length of the AttributeList field */
  u16 attribute_list_length;
} __attribute__((packed)) msrp_attribute_t;

typedef struct msrp_header
{
  struct header_s header;
  u8 protocol_version;
  msrp_attribute_t attribute[];
} __attribute__((packed)) msrp_header_t;

typedef struct msrpdu_talker_advertise
{
  u64 stream_id;
  u8 dest_mac[6];
  u16 vlan_id;
  u16 max_frame_size;
  u16 max_frame_interval;
  u8 priority_and_rank;
  u32 accumulated_latency;
} __attribute__((packed)) msrpdu_talker_advertise_t;

typedef struct msrpdu_listener
{
  u64 stream_id;
} msrpdu_listener_t;

typedef struct msrpdu_domain
{
  u8 sr_class_id;
  u8 sr_class_priority;
  u16 sr_class_vid;
} __attribute__((packed)) msrpdu_domain_t;

/**
 * Initialize the MSRP subsystem
 * @param interface Network interface name
 * @return Socket file descriptor or -1 on error
 */
int msrp_init_socket(const char* interface);

/**
 * Initialize MSRP state with default values
 * @param state Pointer to MSRP state structure
 */
void msrp_state_init(struct avtp_state_s* state);

/**
 * Process incoming MSRP packets
 * @param state Pointer to AVTP state
 */
void msrp_net_rx(struct avtp_state_s* state);
void msrp_register_attach_request(struct mrp_application* app, u64 stream_id);

#endif // ETHERNET_PTP_MSRP_H
