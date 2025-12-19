//
// Created by max on 11/26/25.
//

#ifndef ETHERNET_PTP_MSRP_H
#define ETHERNET_PTP_MSRP_H

#include <stdbool.h>
#include <sys/time.h>

#include "types.h"

/* Forward declaration */
struct avtp_state_s;

#define MSRP_ATTRIBUTE_TYPE_TALKER_ADVERTISE 1
#define MSRP_ATTRIBUTE_TYPE_TALKER_FAILED 2
#define MSRP_ATTRIBUTE_TYPE_LISTENER 3
#define MSRP_ATTRIBUTE_TYPE_DOMAIN 4

/* MRP Attribute Events (Three-Packed Events) - IEEE 802.1Q Section 10.8 */
#define MSRP_ATTRIBUTE_EVENT_NEW    0
#define MSRP_ATTRIBUTE_EVENT_JOININ 1
#define MSRP_ATTRIBUTE_EVENT_IN     2
#define MSRP_ATTRIBUTE_EVENT_JOINMT 3
#define MSRP_ATTRIBUTE_EVENT_MT     4
#define MSRP_ATTRIBUTE_EVENT_LV     5

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

/* Class ID definitions */
#define MSRP_SR_CLASS_A      6
#define MSRP_SR_CLASS_B      5

/* Default values for class priorities */
#define MSRP_SR_CLASS_A_PRIO 3
#define MSRP_SR_CLASS_B_PRIO 2

#define MSRP_MULTICAST_MAC (u8[6]){0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E}

/* ============================================================================
 * MRP Applicant State Machine States - IEEE 802.1Q-2022 Section 10.7.4
 * ============================================================================
 * These states track our local declaration intent for an attribute.
 */
typedef enum
{
  MRP_APPLICANT_VO, /* Very anxious Observer - no local declaration */
  MRP_APPLICANT_VP, /* Very anxious Passive - pending declaration */
  MRP_APPLICANT_VN, /* Very anxious New - new declaration */
  MRP_APPLICANT_AN, /* Anxious New - new, timer running */
  MRP_APPLICANT_AA, /* Anxious Active - active, timer running */
  MRP_APPLICANT_QA, /* Quiet Active - active, waiting */
  MRP_APPLICANT_LA, /* Leaving Active - leaving, timer running */
  MRP_APPLICANT_AO, /* Anxious Observer - observed, timer running */
  MRP_APPLICANT_QO, /* Quiet Observer - observed, waiting */
  MRP_APPLICANT_AP, /* Anxious Passive - passive, timer running */
  MRP_APPLICANT_QP, /* Quiet Passive - passive, waiting */
  MRP_APPLICANT_LO, /* Leaving Observer - leaving observer */
} mrp_applicant_state_t;

/* ============================================================================
 * MRP Registrar State Machine States - IEEE 802.1Q-2022 Section 10.7.5
 * ============================================================================
 * These states track remote declarations we have received.
 */
typedef enum
{
  MRP_REGISTRAR_MT, /* Empty - no registration */
  MRP_REGISTRAR_IN, /* In - registered */
  MRP_REGISTRAR_LV, /* Leaving - leave timer running */
} mrp_registrar_state_t;

/* ============================================================================
 * MSRP Talker Stream Information (received via TalkerAdvertise/TalkerFailed)
 * ============================================================================
 */
typedef struct
{
  bool valid; /* Is this entry in use? */
  u64 stream_id; /* Stream ID from talker */
  u8 dest_addr[6]; /* Destination MAC for stream */
  u16 vlan_id; /* VLAN ID for stream */
  u16 max_frame_size; /* Maximum frame size */
  u16 max_frame_interval; /* Maximum frame interval */
  u8 priority; /* Stream priority (3 bits) */
  u8 rank; /* Stream rank (1 bit) */
  u32 accumulated_latency; /* Accumulated latency */
  bool failed; /* True if TalkerFailed received */
  u8 failure_code; /* Failure code if failed */
  u64 failure_bridge_id; /* Bridge ID that reported failure */

  /* Registrar state for this talker attribute */
  mrp_registrar_state_t registrar_state;
  struct timespec leave_timer; /* Leave timer for registrar state */
} msrp_talker_info_t;

/* ============================================================================
 * MSRP Listener Applicant State (our listener declaration for a stream)
 * ============================================================================
 */
typedef struct
{
  bool active; /* Is this listener declaration active? */
  bool leaving; /* Is this listener in the process of leaving? */
  u64 stream_id; /* Stream ID we want to listen to */
  u8 declaration_type; /* READY, READY_FAILED, ASKING_FAILED */

  /* Applicant state machine */
  mrp_applicant_state_t applicant_state;
  struct timespec join_timer; /* Join timer for applicant state */

  /* Flags for transmission */
  bool tx_pending; /* Need to transmit declaration */
} msrp_listener_decl_t;

/* ============================================================================
 * MSRP Domain Information
 * ============================================================================
 */
typedef struct
{
  bool valid;
  u8 sr_class_id;
  u8 sr_class_priority;
  u16 sr_class_vid;
  mrp_registrar_state_t registrar_state;
} msrp_domain_info_t;

/* ============================================================================
 * MSRP State (to be included in avtp_state_s)
 * ============================================================================
 */
typedef struct
{
  /* Domain information - support 2 domains (Class A and Class B) */
  msrp_domain_info_t domains[2];

  /* Received talker stream info - for now only support single stream */
  msrp_talker_info_t talker;

  /* Our listener declaration - for now only support single stream */
  msrp_listener_decl_t listener;

  /* Timing for periodic transmissions */
  struct timespec last_tx_time;
  u32 join_timeout_ms; /* JoinTime in ms (typically 200ms) */
  u32 leave_timeout_ms; /* LeaveTime in ms (typically 600ms) */
  u32 leave_all_timeout_ms; /* LeaveAllTime in ms (typically 10000ms) */
} msrp_state_t;

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

/* ============================================================================
 * MSRP Initialization and Core Functions
 * ============================================================================
 */

/**
 * Initialize the MSRP subsystem
 * @param interface Network interface name
 * @return Socket file descriptor or -1 on error
 */
int msrp_init(const char* interface);

/**
 * Initialize MSRP state with default values
 * @param state Pointer to MSRP state structure
 */
void msrp_state_init(msrp_state_t* state);

/**
 * Process incoming MSRP packets
 * @param state Pointer to AVTP state
 */
void msrp_net_rx(struct avtp_state_s* state);

/**
 * Process MSRP timers and send pending messages
 * @param state Pointer to AVTP state
 */
void msrp_periodic(struct avtp_state_s* state);

/* ============================================================================
 * MSRP Listener Applicant Functions
 * ============================================================================
 */

/**
 * Start listening to a stream (Join as listener)
 * This triggers the applicant state machine to declare Listener Ready
 * @param state Pointer to AVTP state
 * @param stream_id Stream ID to listen to
 * @return 0 on success, -1 on error
 */
int msrp_listener_join(struct avtp_state_s* state, u64 stream_id);

/**
 * Stop listening to a stream (Leave as listener)
 * This triggers the applicant state machine to withdraw declaration
 * @param state Pointer to AVTP state
 * @param stream_id Stream ID to stop listening to
 * @return 0 on success, -1 on error
 */
int msrp_listener_leave(struct avtp_state_s* state, u64 stream_id);

/**
 * Send a listener declaration message
 * @param state Pointer to AVTP state
 * @param stream_id Stream ID
 * @param declaration_type MSRP_LISTENER_READY, MSRP_LISTENER_READY_FAILED, etc.
 * @param event MRP attribute event (NEW, JOININ, etc.)
 * @return 0 on success, -1 on error
 */
int msrp_send_listener_declaration(struct avtp_state_s* state, u64 stream_id,
                                   u8 declaration_type, u8 event);

/* ============================================================================
 * MSRP Talker Functions (for future use)
 * ============================================================================
 */

/**
 * Send a talker advertise message
 * @param state Pointer to AVTP state
 * @return 0 on success, -1 on error
 */
int msrp_send_talker_advertise(struct avtp_state_s* state);

/**
 * Send a domain discovery request
 * @param state Pointer to AVTP state
 */
void msrp_send_domain_request(const struct avtp_state_s* state);

#endif // ETHERNET_PTP_MSRP_H
