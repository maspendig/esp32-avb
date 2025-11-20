#ifndef ESP32_AVB_AVTP_H
#define ESP32_AVB_AVTP_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#define ETH_TYPE_AVTP 0x22F0
#define MAX_ADP_ENTITIES 32


#define AVTP_SUBTYPE_ADP  0xFA
#define AVTP_SUBTYPE_AECP 0xFB
#define AVTP_SUBTYPE_MAAP 0xFE

/* IEEE 1722.1-2021 ENTITY Descriptor (7.2.1) */
struct atdecc_entity_descriptor_s
{
  uint16_t descriptor_type; // 0x0000 for ENTITY
  uint16_t descriptor_index; // 0x0000 for ENTITY (only one per entity)
  uint64_t entity_id; // Unique identifier for the ATDECC Entity
  uint64_t entity_model_id; // Unique identifier for the Entity model
  uint32_t entity_capabilities; // Entity capability flags
  uint16_t talker_stream_sources; // Number of talker stream sources
  uint16_t talker_capabilities; // Talker capability flags
  uint16_t listener_stream_sinks; // Number of listener stream sinks
  uint16_t listener_capabilities; // Listener capability flags
  uint32_t controller_capabilities; // Controller capability flags
  uint32_t available_index; // Incremented on ADP available
  uint64_t association_id; // Association ID for grouping entities
  uint8_t entity_name[64]; // UTF-8 entity name
  uint16_t vendor_name_string; // Localized string reference
  uint16_t model_name_string; // Localized string reference
  uint8_t firmware_version[64]; // UTF-8 firmware version string
  uint8_t group_name[64]; // UTF-8 group name string
  uint8_t serial_number[64]; // UTF-8 serial number string
  uint16_t configurations_count; // Number of configuration descriptors
  uint16_t current_configuration; // Index of current configuration
} __attribute__((packed));

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
