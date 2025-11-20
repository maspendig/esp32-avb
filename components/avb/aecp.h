//
// Created by max on 11/20/25.
//

#ifndef ETHERNET_PTP_AECP_H
#define ETHERNET_PTP_AECP_H

#include <stdint.h>
#include <types.h>
#include <sys/types.h>

/* ATDECC Entity Model Command */
#define AECP_MSG_TYPE_AEM_COMMAND   0x0
/* ATDECC Entity Model Command response */
#define AECP_MSG_TYPE_AEM_RESPONSE  0x1

#define ACM_COMMAND_TYPE_ACQUIRE_ENTITY 0x0000
#define ACM_COMMAND_TYPE_READ_DESCRIPTOR 0x0004
#define ACM_COMMAND_TYPE_REGISTER_UNSOLICITED_NOTIFICATION 0x0024
#define ACM_COMMAND_TYPE_UNREGISTER_UNSOLICITED_NOTIFICATION 0x0025
#define ACM_COMMAND_TYPE_IDENTIFY_NOTIFICATION 0x0026

#define AEM_DESC_TYPE_ENTITY 0x0000
#define AEM_DESC_TYPE_CONFIGURATION 0x0001
#define AEM_DESC_TYPE_AUDIO_UNIT 0x0002
#define AEM_DESC_TYPE_STREAM_INPUT 0x0005
#define AEM_DESC_TYPE_STREAM_OUTPUT 0x0006
#define AEM_DESC_TYPE_AVB_INTERFACE 0x0009
#define AEM_DESC_TYPE_CLOCK_SOURCE 0x000A
#define AEM_DESC_TYPE_LOCALE 0x000C
#define AEM_DESC_TYPE_STRINGS 0x000D
#define AEM_DESC_TYPE_CLOCK_DOMAIN 0x0024

struct desc_count_s
{
  u16 descriptor_type;
  u16 count;
} __attribute__((packed));

struct config_desc_s
{
  u16 descriptor_type;
  u8 descriptor_index[64];
  u16 localized_description;
  u16 descriptor_counts_count;
  u16 descriptor_counts_offset;
  struct desc_count_s descriptor_counts[];
} __attribute__((packed));

struct aecp_aem_read_desc_cmd
{
  u16 configuration;
  u16 reserved;
  u16 descriptor_type;
  u16 descriptor_index;
} __attribute__((packed));

struct aecp_data_unit_s
{
  struct header_s header;
  u8 subtype; // 1 octet

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  u8 message_type : 4; // 4 bits
  u8 version : 3; // 3 bits
  u8 h : 1; // 1 bit (header specific)
#else
  u8 h : 1; // 1 bit (header specific)
  u8 version : 3; // 3 bits
  u8 message_type : 4; // 4 bits
#endif

  u16 control_data_len_status; // 16 bits
  u64 target_entity_id; // 64 bits
  u64 controller_entity_id; // 64 bits
  u16 sequence_id; // 16 bits
  u16 command_type; // 16 bits (ACM command type)
} __attribute__((packed));


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

/* AECP READ_DESCRIPTOR Response structure */
struct aecp_read_descriptor_response_s
{
  struct aecp_data_unit_s aecp_header;
  uint16_t configuration_index; // 16 bits
  uint16_t reserved; // 16 bits
  struct atdecc_entity_descriptor_s descriptor;
} __attribute__((packed));

struct avtp_state_s;
int aecp_net_rx(struct avtp_state_s* state, struct aecp_data_unit_s* msg, ssize_t len);
#endif //ETHERNET_PTP_AECP_H
