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

void msrp_net_rx();
int msrp_init(const char* interface);
void read_msrp_net(const struct avtp_state_s* state);

#endif //ETHERNET_PTP_MSRP_H
