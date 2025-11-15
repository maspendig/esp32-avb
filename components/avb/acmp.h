//
// Created by max on 11/15/25.
//

#ifndef ETHERNET_PTP_ACMP_H
#define ETHERNET_PTP_ACMP_H

#define AVTP_SUBTYPE_ACMP 0xFC

struct acmp_header_s {
  uint8_t subtype;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  uint8_t message_type : 4;
  uint8_t version : 3;
  uint8_t h : 1;
#else
  uint8_t h : 1;
  uint8_t version : 3;
  uint8_t message_type : 4;
#endif

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  uint16_t control_data_length : 11;
  uint16_t status : 5;
#else
  uint16_t status : 5;
  uint16_t control_data_length : 11;
#endif
};

/* ATDECC Connection Management Protocol Data Unit */
struct acmp_du_s {
  struct acmp_header_s header;
  uint64_t stream_id;
  uint64_t controller_entity_id;
  uint64_t talker_entity_id;
  uint64_t listener_entity_id;
  uint16_t talker_unique_id;
  uint16_t listener_unique_id;
  uint16_t stream_dest_mac[3];
  uint16_t connection_count;
  uint16_t sequence_id;
  uint16_t flags;
  uint16_t stream_vlan_id;
  uint16_t connected_listeners_entries;
  uint16_t ip_flags;
  uint16_t reserved;
  uint16_t source_port;
  uint16_t dest_port;
  uint64_t source_ip_address[2];
  uint64_t destination_ip_address[2];
};

void send_acmp_message();
#endif //ETHERNET_PTP_ACMP_H