#ifndef ESP32_AVB_AVTP_H
#define ESP32_AVB_AVTP_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#define ETH_TYPE_AVTP 0x22F0
#define MAX_ADP_ENTITIES 32

/* Structure to hold discovered ADP entity information */
struct adp_entity_entry_s
{
  uint64_t entity_id;
  uint8_t mac[6];
  uint16_t talker_stream_sources;
  uint16_t talker_capabilities;
  uint16_t listener_stream_sinks;
  uint16_t listener_capabilities;
  uint32_t controller_capabilities;
  uint32_t available_index;
  time_t valid_until; // epoch seconds until this entry is valid
  bool in_use;
};

struct avtp_state_s
{
  bool stop;
  int socket;
  uint16_t acmp_sequence_id;
  uint8_t intf_hw_addr[6];
  uint64_t entity_id;
  uint64_t entity_model_id;
  struct timespec last_transmitted_adp;
  uint32_t adp_available_index; // renamed from adp_availabe_index[4] for easier increment
  struct adp_entity_entry_s adp_entities[MAX_ADP_ENTITIES];
  bool connected;
};

int start_avtp_listener(const char* interface);

#define AVTP_GET_STATUS(hdr) \
((ntohs((hdr)->control_data_len_status) >> 11) & 0x1F)

#define AVTP_GET_CTRL_DATA_LEN(hdr) \
(ntohs(hdr->control_data_len_status) & 0x7FF)

#define AVTP_SET_CTRL_DATA_STATUS(hdr, status, cdl) \
(hdr->control_data_len_status = htons(((status & 0x1F) << 11) | (cdl & 0x7FF)))


#endif //ESP32_AVB_AVTP_H
