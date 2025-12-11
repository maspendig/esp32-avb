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
#define AECP_MSG_TYPE_VENDOR_UNIQUE_COMMAND 0x6

#define ACM_COMMAND_TYPE_ACQUIRE_ENTITY 0x0000
#define ACM_COMMAND_TYPE_READ_DESCRIPTOR 0x0004
#define ACM_COMMAND_TYPE_GET_STREAM_FORMAT 0x0009
#define ACM_COMMAND_TYPE_GET_SAMPLING_RATE 0x0015
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
#define AEM_DESC_TYPE_STREAM_PORT_INPUT 0x000E
#define AEM_DESC_TYPE_STREAM_PORT_OUTPUT 0x000F
#define AEM_DESC_TYPE_CONTROL 0x001A
#define AEM_DESC_TYPE_AUDIO_CLUSTER 0x0014
#define AEM_DESC_TYPE_AUDIO_MAP 0x0017
#define AEM_DESC_TYPE_CLOCK_DOMAIN 0x0024

struct aecp_avb_interface_s
{
  u16 descriptor_type;
  u16 descriptor_index;
  u8 object_name[64];
  u16 localized_description;
  u64 mac_address;
  u16 interface_flags;
  u64 clock_identity;
  u8 priority1;
  u8 clock_class;
  u16 offset_scaled_log_variance;
  u8 clock_accuracy;
  u8 priority2;
  u8 domain_number;
  s8 log_sync_interval;
  s8 log_announce_interval;
  s8 log_pdelay_interval;
  u16 port_number;
} __attribute__((packed));

struct desc_count_s
{
  u16 descriptor_type;
  u16 count;
} __attribute__((packed));

struct subtype_data_s
{
  u8 subtype;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  u8 message_type : 4; // 4 bits
  u8 version : 3; // 3 bits
  u8 h : 1; // 1 bit (header specific)
#else
  u8 h : 1; // 1 bit (header specific)
  u8 version : 3; // 3 bits
  u8 message_type : 4; // 4 bits
#endif
  u16 control_data_len_status;
} __attribute__((packed));

struct aecp_common_data_s
{
  u64 target_entity_id;
  u64 controller_entity_id;
  u16 sequence_id;
  u16 command_type;
} __attribute__((packed));

struct aecp_get_stream_format_s
{
  u16 descriptor_type;
  u16 descriptor_index;
  u64 stream_format;
} __attribute__((packed));

struct aecp_sampling_rate_s
{
  u16 descriptor_type;
  u16 descriptor_index;
  u32 sampling_rate;
} __attribute__((packed));

struct aecp_audio_unit_s
{
  u16 descriptor_type;
  u16 descriptor_index;
  u8 object_name[64];
  u16 localized_description;
  u16 clock_domain_index;
  u16 number_of_stream_input_ports;
  u16 base_stream_input_port;
  u16 number_of_stream_output_ports;
  u16 base_stream_output_port;
  u16 number_of_external_input_ports;
  u16 base_external_input_port;
  u16 number_of_external_output_ports;
  u16 base_external_output_port;
  u16 number_of_internal_input_ports;
  u16 base_internal_input_port;
  u16 number_of_internal_output_ports;
  u16 base_internal_output_port;
  u16 number_of_control;
  u16 base_control;
  u16 number_of_signal_selectors;
  u16 base_signal_selector;
  u16 number_of_mixers;
  u16 base_mixer;
  u16 number_of_matrices;
  u16 base_matrix;
  u16 number_of_splitters;
  u16 base_splitter;
  u16 number_of_combiners;
  u16 base_combiner;
  u16 number_of_demultiplexers;
  u16 base_demultiplexer;
  u16 number_of_multiplexers;
  u16 base_multiplexer;
  u16 number_of_tanscoders;
  u16 base_transcoder;
  u16 number_of_control_blocks;
  u16 base_control_block;
  u32 current_sampling_rate;
  u16 sampling_rates_offset;
  u16 sampling_rates_count;
  // FIXME make this a flexible array member
  u32 sampling_rates[1];
} __attribute__((packed));

struct acm_desc_stream_s
{
  u16 descriptor_type;
  u16 descriptor_index;
  u8 object_name[64];
  u16 localized_description;
  u16 clock_domain_index;
  u16 stream_flags;
  u64 current_format;
  u16 formats_offset;
  u16 number_of_formats;
  u64 backup_talker_entity_id_0;
  u16 backup_talker_unique_id_0;
  u64 backup_talker_entity_id_1;
  u16 backup_talker_unique_id_1;
  u64 backup_talker_entity_id_2;
  u16 backup_talker_unique_id_2;
  u64 backedup_talker_entity_id;
  u16 backedup_talker_unique_id;
  u16 avb_interface_index;
  u32 buffer_length;
  u64 formats[];
} __attribute__((packed));

struct acm_desc_stream_port_s
{
  u16 descriptor_type;
  u16 descriptor_index;
  u16 clock_domain_index;
  u16 port_flags;
  u16 number_of_controls;
  u16 base_control;
  u16 number_of_clusters;
  u16 base_cluster;
  u16 number_of_maps;
  u16 base_map;
} __attribute__((packed));

struct aecp_audio_cluster_s
{
  u16 descriptor_type;
  u16 descriptor_index;
  u8 object_name[64];
  u16 localized_description;
  u16 signal_type;
  u16 signal_index;
  u16 signal_output;
  u32 path_latency;
  u32 block_latency;
  u16 channel_count;
  u8 format;
} __attribute__((packed));

struct aecp_audio_mapping_s
{
  u16 mapping_stream_index;
  u16 mapping_stream_channel;
  u16 mapping_cluster_offset;
  u16 mapping_cluster_channel;
} __attribute__((packed));

struct aecp_audio_map_s
{
  u16 descriptor_type;
  u16 descriptor_index;
  u16 mappings_offset;
  u16 number_of_mappings;
  struct aecp_audio_mapping_s mappings[];
} __attribute__((packed));

struct config_desc_s
{
  u16 descriptor_type;
  u16 descriptor_index;
  u8 object_name[64];
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
  u16 vendor_name_string; // Localized string reference
  u16 model_name_string; // Localized string reference
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

#define AECP_SET_CTRL_DATA_STATUS(hdr, status, cdl) \
(hdr->control_data_len_status = htons(((status & 0x1F) << 11) | (cdl & 0x7FF)))

struct avtp_state_s;
int aecp_net_rx(struct avtp_state_s* state, struct aecp_data_unit_s* msg, ssize_t len);
#endif //ETHERNET_PTP_AECP_H
