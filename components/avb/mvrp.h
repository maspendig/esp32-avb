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
#include <esp_timer.h>

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

typedef struct mvrp_pdu_first_value
{
  u16 vlan_id;
} __attribute__((packed)) mvrp_pdu_first_value_t;

typedef mvrp_pdu_first_value_t mvrp_attr_value_t;

typedef struct mvrp_vlan
{
  struct Node list;
  struct mvrp_pdu_first_value vlan;
} mvrp_vlan_t;

/* Maximum number of VLAN registrations to track */
#define MVRP_MAX_VLAN_REGISTRATIONS 4

typedef struct mvrp_ctx
{
  struct mrp_application app;
  struct avtp_state_s* state;
  struct Node* vlans;
} mvrp_ctx_t;

/* ============================================================================
 * MVRP Initialization and Core Functions
 * ============================================================================
 */

/**
 * Initialize the MVRP socket
 * @param interface Network interface name
 * @return Socket file descriptor or -1 on error
 */
int mvrp_init_socket(const char* interface);

/**
 * Initialize MVRP state with default values
 * @param state Pointer to MVRP state structure
 */
void mvrp_state_init(struct avtp_state_s* state);

/**
 * Process incoming MVRP packets
 * @param state Pointer to AVTP state
 */
void mvrp_net_rx(struct avtp_state_s* state);

#endif //ETHERNET_PTP_MVRP_H
