//
// Created by max on 11/20/25.
//

#ifndef ETHERNET_PTP_ADP_H
#define ETHERNET_PTP_ADP_H

#define ADP_MSG_TYPE_ENTITY_AVAILABLE 0x0
#define ADP_MSG_TYPE_ENTITY_DEPARTING 0x1
#define ADP_MSG_TYPE_ENTITY_DISCOVER  0x2
#include <sys/types.h>

struct avtp_state_s;
struct avtp_discovery_msg_s;

int adp_net_rx(struct avtp_state_s* state, struct avtp_discovery_msg_s* msg, ssize_t len);
void send_adp_entity_available(struct avtp_state_s* s_state);
#endif //ETHERNET_PTP_ADP_H
