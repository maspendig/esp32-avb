//
// Created by max on 11/27/25.
//

#ifndef ETHERNET_PTP_MVRP_H
#define ETHERNET_PTP_MVRP_H

#include "avtp.h"

#define ETH_TYPE_MVRP 0x88F5

// MVRP Attribute Types (IEEE 802.1Q-2018)
#define MVRP_ATTRIBUTE_TYPE_VID 1

// MRP Attribute Events (IEEE 802.1Q-2018 Section 10.8)
#define MRP_EVENT_NEW         0  // New declaration
#define MRP_EVENT_JOININ      1  // JoinIn
#define MRP_EVENT_IN          2  // In
#define MRP_EVENT_JOINMT      3  // JoinMT (Join Empty)
#define MRP_EVENT_MT          4  // MT (Empty)
#define MRP_EVENT_LV          5  // Leave

// Three-packed event encoding for attribute vectors
#define MRP_VECATTR_EVENT_JOININ  0x24  // 00 10 01 00 (JoinIn, JoinIn, JoinIn)

int mvrp_init(const char* interface);
void read_mvrp_net(const struct avtp_state_s* state);
int mvrp_send_vlan_join(struct avtp_state_s* state, uint16_t vlan_id);

#endif //ETHERNET_PTP_MVRP_H
