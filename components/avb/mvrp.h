//
// Created by max on 11/27/25.
//
// MVRP - Multiple VLAN Registration Protocol
// IEEE 802.1Q-2022 Section 11.2
//

#ifndef ETHERNET_PTP_MVRP_H
#define ETHERNET_PTP_MVRP_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "types.h"

/* Forward declaration */
struct avtp_state_s;

/* ============================================================================
 * MVRP Constants - IEEE 802.1Q-2022
 * ============================================================================
 */

#define ETH_TYPE_MVRP 0x88F5

/* MVRP multicast destination address */
#define MVRP_MULTICAST_MAC (u8[6]){0x01, 0x80, 0xC2, 0x00, 0x00, 0x21}

/* MVRP Attribute Types (IEEE 802.1Q-2022 Section 11.2.3) */
#define MVRP_ATTRIBUTE_TYPE_VID 1
#define MVRP_ATTRIBUTE_LENGTH_VID 2

/* MRP Timer Values (IEEE 802.1Q-2022 Section 10.7) */
#define MVRP_JOIN_TIME_MS       200   /* JoinTime: 200ms default */
#define MVRP_LEAVE_TIME_MS      600   /* LeaveTime: 600ms default */
#define MVRP_LEAVE_ALL_TIME_MS  10000 /* LeaveAllTime: 10s default */

/* ============================================================================
 * MRP Applicant State Machine States - IEEE 802.1Q-2022 Section 10.7.4
 * ============================================================================
 * These states track our local declaration intent for a VLAN attribute.
 */
typedef enum
{
  MVRP_APPLICANT_VO, /* Very anxious Observer - no local declaration */
  MVRP_APPLICANT_VP, /* Very anxious Passive - pending declaration */
  MVRP_APPLICANT_VN, /* Very anxious New - new declaration */
  MVRP_APPLICANT_AN, /* Anxious New - new, timer running */
  MVRP_APPLICANT_AA, /* Anxious Active - active, timer running */
  MVRP_APPLICANT_QA, /* Quiet Active - active, waiting */
  MVRP_APPLICANT_LA, /* Leaving Active - leaving, timer running */
  MVRP_APPLICANT_AO, /* Anxious Observer - observed, timer running */
  MVRP_APPLICANT_QO, /* Quiet Observer - observed, waiting */
  MVRP_APPLICANT_AP, /* Anxious Passive - passive, timer running */
  MVRP_APPLICANT_QP, /* Quiet Passive - passive, waiting */
  MVRP_APPLICANT_LO, /* Leaving Observer - leaving observer */
} mvrp_applicant_state_t;

/* ============================================================================
 * MRP Registrar State Machine States - IEEE 802.1Q-2022 Section 10.7.5
 * ============================================================================
 * These states track remote VLAN declarations we have received.
 */
typedef enum
{
  MVRP_REGISTRAR_MT, /* Empty - no registration */
  MVRP_REGISTRAR_IN, /* In - registered */
  MVRP_REGISTRAR_LV, /* Leaving - leave timer running */
} mvrp_registrar_state_t;

/* ============================================================================
 * MVRP VID Declaration (our VLAN membership declaration)
 * ============================================================================
 */
typedef struct
{
  bool active; /* Is this declaration active? */
  u16 vlan_id; /* VLAN ID we are declaring */

  /* Applicant state machine */
  mvrp_applicant_state_t applicant_state; /* Current applicant state */
  struct timespec join_timer; /* Join timer for applicant state */

  /* Flags for transmission */
  bool tx_pending; /* Need to transmit declaration */
} mvrp_vlan_decl_t;

/* ============================================================================
 * MVRP VID Registration (received VLAN declaration from others)
 * ============================================================================
 */
typedef struct
{
  bool valid; /* Is this entry in use? */
  u16 vlan_id; /* VLAN ID registered */

  /* Registrar state machine */
  mvrp_registrar_state_t registrar_state; /* Current registrar state */
  struct timespec leave_timer; /* Leave timer for registrar state */
} mvrp_vlan_reg_t;

/* Maximum number of VLAN registrations to track */
#define MVRP_MAX_VLAN_REGISTRATIONS 4

/* ============================================================================
 * MVRP State (to be included in avtp_state_s)
 * ============================================================================
 */
typedef struct
{
  /* Our VLAN declaration - for AVB listener, typically just one VLAN */
  mvrp_vlan_decl_t vlan_decl;

  /* Received VLAN registrations from other participants */
  mvrp_vlan_reg_t vlan_registrations[MVRP_MAX_VLAN_REGISTRATIONS];

  /* Timing for periodic transmissions */
  struct timespec last_tx_time; /* Last transmission time */
  u32 join_timeout_ms; /* JoinTime in ms */
  u32 leave_timeout_ms; /* LeaveTime in ms */
  u32 leave_all_timeout_ms; /* LeaveAllTime in ms */
} mvrp_state_t;

/* ============================================================================
 * MVRP Initialization and Core Functions
 * ============================================================================
 */

/**
 * Initialize the MVRP socket
 * @param interface Network interface name
 * @return Socket file descriptor or -1 on error
 */
int mvrp_init(const char* interface);

/**
 * Initialize MVRP state with default values
 * @param state Pointer to MVRP state structure
 */
void mvrp_state_init(mvrp_state_t* state);

/**
 * Process incoming MVRP packets
 * @param state Pointer to AVTP state
 */
void mvrp_net_rx(struct avtp_state_s* state);

/**
 * Process MVRP timers and send pending messages
 * @param state Pointer to AVTP state
 */
void mvrp_periodic(struct avtp_state_s* state);

/* ============================================================================
 * MVRP VLAN Declaration Functions
 * ============================================================================
 */

/**
 * Join a VLAN (declare VLAN membership)
 * Triggers the applicant state machine to declare VID
 * @param state Pointer to AVTP state
 * @param vlan_id VLAN ID to join
 * @return 0 on success, -1 on error
 */
int mvrp_vlan_join(struct avtp_state_s* state, u16 vlan_id);

/**
 * Leave a VLAN (withdraw VLAN membership declaration)
 * Triggers the applicant state machine to withdraw declaration
 * @param state Pointer to AVTP state
 * @param vlan_id VLAN ID to leave
 * @return 0 on success, -1 on error
 */
int mvrp_vlan_leave(struct avtp_state_s* state, u16 vlan_id);

/**
 * Check if a VLAN is registered (received from other participants)
 * @param state Pointer to AVTP state
 * @param vlan_id VLAN ID to check
 * @return true if VLAN is registered, false otherwise
 */
bool mvrp_vlan_is_registered(const struct avtp_state_s* state, u16 vlan_id);

#endif //ETHERNET_PTP_MVRP_H
