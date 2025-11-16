#ifndef ESP32_AVB_AVTP_H
#define ESP32_AVB_AVTP_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#define MAX_ADP_ENTITIES 32

/* Structure to hold discovered ADP entity information */
struct adp_entity_entry_s {
  uint64_t entity_id;
  uint8_t mac[6];
  uint16_t talker_stream_sources;
  uint16_t talker_capabilities;
  uint16_t listener_stream_sinks;
  uint16_t listener_capabilities;
  uint32_t controller_capabilities;
  uint32_t available_index;
  time_t valid_until;  // epoch seconds until this entry is valid
  bool in_use;
};

struct avtp_state_s
{
  bool stop;
  int socket;
  uint8_t intf_hw_addr[6];
  uint64_t entity_id;
  uint64_t entity_model_id;
  struct timespec last_transmitted_adp;
  uint32_t adp_available_index; // renamed from adp_availabe_index[4] for easier increment
  struct adp_entity_entry_s adp_entities[MAX_ADP_ENTITIES];
};

int start_avtp_listener(const char *interface);

#endif //ESP32_AVB_AVTP_H