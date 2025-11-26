//
// Created by max on 10/29/25.
//

#ifndef ETHERNET_PTP_MRPD_H
#define ETHERNET_PTP_MRPD_H

#ifndef FAR
#define FAR
#endif

#define ETH_TYPE_MSRP 0x22EA
#define ETH_TYPE_MVRP 0x88F5

int srpd_start(const char* interface);
#endif //ETHERNET_PTP_MRPD_H
