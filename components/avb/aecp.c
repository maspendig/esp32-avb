#include "types.h"
#include "aecp.h"
#include "acmp.h"
#include "avtp.h"
#include "config.h"

#include <cc.h>
#include <esp_err.h>
#include <esp_eth_spec.h>
#include <esp_log.h>
#include <sys/unistd.h>

#define TAG "aecp"

static void handle_aem_read_desc_entity(struct avtp_state_s* s_state, struct aecp_data_unit_s* request_msg)
{
  ESP_LOGI(TAG, "Received ACM Read ENTITY Descriptor Request");
  if (s_state == NULL || s_state->socket < 0)
  {
    ESP_LOGE(TAG, "Socket not ready to send AECP response");
    return;
  }

  struct aecp_read_descriptor_response_s response = {0};

  /* Copy Ethernet header from request and swap MAC addresses */
  memcpy(response.aecp_header.header.dst_mac, request_msg->header.src_mac, ETH_ADDR_LEN);
  memcpy(response.aecp_header.header.src_mac, request_msg->header.dst_mac, ETH_ADDR_LEN);

  /* Ethernet type (big-endian) */
  response.aecp_header.header.eth_type[0] = (ETH_TYPE_AVTP >> 8) & 0xFF;
  response.aecp_header.header.eth_type[1] = ETH_TYPE_AVTP & 0xFF;

  /* AECP header fields */
  response.aecp_header.subtype = AVTP_SUBTYPE_AECP;
  response.aecp_header.message_type = AECP_MSG_TYPE_AEM_RESPONSE;
  response.aecp_header.version = 0;
  response.aecp_header.h = 0;

  ACMP_SET_CTRL_DATA_STATUS((&response.aecp_header), 0, 328);

  /* Swap entity IDs - we become the target, controller becomes the controller */
  response.aecp_header.target_entity_id = request_msg->target_entity_id;
  response.aecp_header.controller_entity_id = request_msg->controller_entity_id;
  response.aecp_header.sequence_id = request_msg->sequence_id; // Echo sequence ID
  response.aecp_header.command_type = htons(ACM_COMMAND_TYPE_READ_DESCRIPTOR);

  /* Response payload fields */
  response.configuration_index = 0;
  response.reserved = 0;

  /* Fill ENTITY descriptor */
  response.descriptor.descriptor_type = htons(AEM_DESC_TYPE_ENTITY); // ENTITY
  response.descriptor.descriptor_index = htons(0x0000);
  response.descriptor.entity_id = htonll(s_state->entity_id);
  response.descriptor.entity_model_id = htonll(s_state->entity_model_id);
  response.descriptor.entity_capabilities = htonl(CONFIG_ENTITY_CAPABILITIES); // Example capabilities
  response.descriptor.talker_stream_sources = htons(CONFIG_TALKER_STREAM_SOURCES);
  response.descriptor.talker_capabilities = htons(CONFIG_TALKER_CAPABILITIES);
  response.descriptor.listener_stream_sinks = htons(CONFIG_LISTENER_STREAM_SINKS);
  response.descriptor.listener_capabilities = htons(CONFIG_LISTENER_CAPABILITIES);
  response.descriptor.controller_capabilities = htonl(CONFIG_CONTROLLER_CAPABILITIES);
  response.descriptor.available_index = htonl(s_state->adp_available_index);
  response.descriptor.association_id = htonll(0);

  strncpy((char*)response.descriptor.entity_name, CONFIG_ENTITY_NAME, sizeof(response.descriptor.entity_name));

  response.descriptor.vendor_name_string = htons(0);
  response.descriptor.model_name_string = htons(1);

  strncpy((char*)response.descriptor.firmware_version, CONFIG_FW_VERSION, sizeof(response.descriptor.firmware_version));

  // TODO generate a unique serial number, refer to std
  char serial[65];
  snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
           s_state->intf_hw_addr[0], s_state->intf_hw_addr[1], s_state->intf_hw_addr[2],
           s_state->intf_hw_addr[3], s_state->intf_hw_addr[4], s_state->intf_hw_addr[5]);
  strncpy((char*)response.descriptor.serial_number, serial, sizeof(response.descriptor.serial_number));

  response.descriptor.configurations_count = htons(1);
  response.descriptor.current_configuration = htons(0);


  /* Send the response */
  ssize_t written = write(s_state->socket, &response, sizeof(response));
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send ENTITY descriptor response: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Sent AECP Entity Descriptor Response");
  }
}

void handle_aem_read_configuration(struct avtp_state_s* s_state, struct aecp_data_unit_s* request_msg)
{
  if (s_state == NULL || s_state->socket < 0)
  {
    ESP_LOGE(TAG, "Socket not ready to send AECP response");
    return;
  }

  struct config_response
  {
    struct aecp_data_unit_s aecp_header;
    uint16_t configuration_index;
    uint16_t reserved;
    struct config_desc_s config_desc;
  } __attribute__((packed));

  // Define the number of descriptor types
  const size_t num_desc_types = 8;

  // Calculate total response size
  const size_t response_size = sizeof(struct aecp_data_unit_s) +
    sizeof(uint16_t) * 2 + // configuration_index + reserved
    sizeof(struct config_desc_s) +
    num_desc_types * sizeof(struct desc_count_s);

  // Allocate response with space for the flexible array member
  struct config_response* resp = malloc(response_size);

  if (resp == NULL)
  {
    ESP_LOGE(TAG, "Failed to allocate memory for configuration response");
    return;
  }

  // Copy AECP header from request and swap MAC addresses
  memcpy(resp->aecp_header.header.dst_mac, request_msg->header.src_mac, ETH_ADDR_LEN);
  memcpy(resp->aecp_header.header.src_mac, request_msg->header.dst_mac, ETH_ADDR_LEN);

  // Copy Ethernet type
  resp->aecp_header.header.eth_type[0] = request_msg->header.eth_type[0];
  resp->aecp_header.header.eth_type[1] = request_msg->header.eth_type[1];

  // Copy and modify AECP fields
  resp->aecp_header.subtype = request_msg->subtype;
  resp->aecp_header.message_type = AECP_MSG_TYPE_AEM_RESPONSE; // Change to response
  resp->aecp_header.version = request_msg->version;
  resp->aecp_header.h = request_msg->h;
  // Set control data length and status
  resp->aecp_header.control_data_len_status = htons(0x007A);

  // Copy entity IDs and sequence ID
  resp->aecp_header.target_entity_id = request_msg->target_entity_id;
  resp->aecp_header.controller_entity_id = request_msg->controller_entity_id;
  resp->aecp_header.sequence_id = request_msg->sequence_id;
  resp->aecp_header.command_type = request_msg->command_type;

  // Set configuration index and reserved
  resp->configuration_index = htons(0);
  resp->reserved = htons(0);

  // Initialize the configuration descriptor fields
  resp->config_desc.descriptor_type = htons(AEM_DESC_TYPE_CONFIGURATION);

  resp->config_desc.descriptor_index = htons(0);
  memset(resp->config_desc.object_name, 0, sizeof(resp->config_desc.object_name));
  resp->config_desc.localized_description = htons(0xFFFF);
  resp->config_desc.descriptor_counts_count = htons(num_desc_types);
  resp->config_desc.descriptor_counts_offset = htons(74); // Offset to descriptor_counts array

  // Populate the descriptor_counts array
  resp->config_desc.descriptor_counts[0].descriptor_type = htons(AEM_DESC_TYPE_AUDIO_UNIT);
  resp->config_desc.descriptor_counts[0].count = htons(1);
  resp->config_desc.descriptor_counts[1].descriptor_type = htons(AEM_DESC_TYPE_STREAM_INPUT);
  resp->config_desc.descriptor_counts[1].count = htons(1);
  resp->config_desc.descriptor_counts[2].descriptor_type = htons(AEM_DESC_TYPE_STREAM_OUTPUT);
  resp->config_desc.descriptor_counts[2].count = htons(1);
  resp->config_desc.descriptor_counts[3].descriptor_type = htons(AEM_DESC_TYPE_AVB_INTERFACE);
  resp->config_desc.descriptor_counts[3].count = htons(1);
  resp->config_desc.descriptor_counts[4].descriptor_type = htons(AEM_DESC_TYPE_CLOCK_SOURCE);
  resp->config_desc.descriptor_counts[4].count = htons(1);
  resp->config_desc.descriptor_counts[5].descriptor_type = htons(AEM_DESC_TYPE_LOCALE);
  resp->config_desc.descriptor_counts[5].count = htons(1);
  resp->config_desc.descriptor_counts[6].descriptor_type = htons(AEM_DESC_TYPE_STRINGS);
  resp->config_desc.descriptor_counts[6].count = htons(1);
  resp->config_desc.descriptor_counts[7].descriptor_type = htons(AEM_DESC_TYPE_CLOCK_DOMAIN);
  resp->config_desc.descriptor_counts[7].count = htons(1);

  // Send configuration descriptor response
  ssize_t written = write(s_state->socket, resp, response_size);
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send CONFIGURATION descriptor response: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Sent AECP Configuration Descriptor Response (%zd bytes)", written);
  }

  // Free allocated memory
  free(resp);
}

void handle_aem_read_desc_audio_map(struct avtp_state_s* s_state, struct aecp_data_unit_s* msg)
{
  ESP_LOGI(TAG, "Received ACM Read AUDIO_MAP Descriptor Request");

  const size_t num_mappings = 8;

  struct aecp_audio_map_response_s
  {
    struct aecp_data_unit_s aecp_header;
    uint16_t configuration_index;
    uint16_t reserved;
    struct aecp_audio_map_s audio_map_desc;
  };

  const size_t response_size = sizeof(struct aecp_audio_map_response_s) +
    num_mappings * sizeof(struct aecp_audio_mapping_s);

  struct aecp_audio_map_response_s* resp = malloc(response_size);
  if (resp == NULL)
  {
    ESP_LOGE(TAG, "Failed to allocate memory for audio map response");
    return;
  }
  memset(resp, 0, response_size);


  /* Copy Ethernet header from request and swap MAC addresses */
  memcpy(resp->aecp_header.header.dst_mac, msg->header.src_mac, ETH_ADDR_LEN);
  memcpy(resp->aecp_header.header.src_mac, msg->header.dst_mac, ETH_ADDR_LEN);
  /* Ethernet type (big-endian) */
  resp->aecp_header.header.eth_type[0] = (ETH_TYPE_AVTP >> 8) & 0xFF;
  resp->aecp_header.header.eth_type[1] = ETH_TYPE_AVTP & 0xFF;
  /* AECP header fields */
  resp->aecp_header.subtype = AVTP_SUBTYPE_AECP;
  resp->aecp_header.message_type = AECP_MSG_TYPE_AEM_RESPONSE;
  resp->aecp_header.version = 0;
  resp->aecp_header.h = 0;

  u8 status = 0; // Success
  const uint16_t desc_data_len = sizeof(struct aecp_audio_map_s) + num_mappings * sizeof(struct aecp_audio_mapping_s);
  AECP_SET_CTRL_DATA_STATUS((&resp->aecp_header), status, desc_data_len);
  resp->aecp_header.target_entity_id = msg->target_entity_id;
  resp->aecp_header.controller_entity_id = msg->controller_entity_id;

  resp->aecp_header.sequence_id = msg->sequence_id;
  resp->aecp_header.command_type = htons(ACM_COMMAND_TYPE_READ_DESCRIPTOR);
  /* Response payload fields */
  resp->configuration_index = 0;
  resp->reserved = 0;
  /* Fill AUDIO_MAP descriptor */
  resp->audio_map_desc.descriptor_type = htons(AEM_DESC_TYPE_AUDIO_MAP);
  resp->audio_map_desc.descriptor_index = 0;
  resp->audio_map_desc.mappings_offset = htons(8);
  resp->audio_map_desc.number_of_mappings = htons(num_mappings);

  for (size_t i = 0; i < num_mappings; i++)
  {
    resp->audio_map_desc.mappings[i].mapping_stream_index = htons(0);
    resp->audio_map_desc.mappings[i].mapping_stream_channel = htons(i);
    resp->audio_map_desc.mappings[i].mapping_cluster_offset = htons(i);
    resp->audio_map_desc.mappings[i].mapping_cluster_channel = htons(0);
  }


  /* Send the response */
  ssize_t written = write(s_state->socket, resp, response_size);
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send AUDIO_MAP descriptor response: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Sent AECP AUDIO_MAP Descriptor Response");
  }

  free(resp);
}

void handle_aem_read_desc_stream_port_input(struct avtp_state_s* s_state, struct aecp_data_unit_s* msg)
{
  ESP_LOGI(TAG, "Received ACM Read STREAM_PORT_INPUT Descriptor Request");

  struct aecp_stream_port_response_s
  {
    struct aecp_data_unit_s aecp_header;
    uint16_t configuration_index;
    uint16_t reserved;
    struct acm_desc_stream_port_s stream_port_desc;
  } __attribute__((packed));

  struct aecp_stream_port_response_s resp = {0};
  /* Copy Ethernet header from request and swap MAC addresses */
  memcpy(resp.aecp_header.header.dst_mac, msg->header.src_mac, ETH_ADDR_LEN);
  memcpy(resp.aecp_header.header.src_mac, msg->header.dst_mac, ETH_ADDR_LEN);
  /* Ethernet type (big-endian) */
  resp.aecp_header.header.eth_type[0] = (ETH_TYPE_AVTP >> 8) & 0xFF;
  resp.aecp_header.header.eth_type[1] = ETH_TYPE_AVTP & 0xFF;
  /* AECP header fields */
  resp.aecp_header.subtype = AVTP_SUBTYPE_AECP;
  resp.aecp_header.message_type = AECP_MSG_TYPE_AEM_RESPONSE;
  resp.aecp_header.version = 0;
  resp.aecp_header.h = 0;

  u8 status = 0; // Success
  u16 cdl = 36;
  (resp.aecp_header.control_data_len_status = htons(((status & 0x1F) << 11) | (cdl & 0x7FF)));
  resp.aecp_header.target_entity_id = msg->target_entity_id;
  resp.aecp_header.controller_entity_id = msg->controller_entity_id;

  resp.aecp_header.sequence_id = msg->sequence_id;
  resp.aecp_header.command_type = htons(ACM_COMMAND_TYPE_READ_DESCRIPTOR);
  /* Response payload fields */
  resp.configuration_index = 0;
  resp.reserved = 0;
  /* Fill STREAM_PORT_INPUT descriptor */
  resp.stream_port_desc.descriptor_type = htons(AEM_DESC_TYPE_STREAM_PORT_INPUT);
  resp.stream_port_desc.descriptor_index = 0;
  resp.stream_port_desc.clock_domain_index = htons(0);
  resp.stream_port_desc.port_flags = htons(0);
  resp.stream_port_desc.number_of_controls = htons(0);
  resp.stream_port_desc.base_control = htons(0);
  resp.stream_port_desc.number_of_clusters = htons(8);
  resp.stream_port_desc.base_cluster = htons(0);
  resp.stream_port_desc.number_of_maps = htons(1);
  resp.stream_port_desc.base_map = htons(0);


  /* Send the response */
  ssize_t written = write(s_state->socket, &resp, sizeof(resp));
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send STREAM_PORT_INPUT descriptor response: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Sent AECP STREAM_PORT_INPUT Descriptor Response");
  }
}

void handle_aem_read_desc_stream_port_output(struct avtp_state_s* s_state, struct aecp_data_unit_s* msg)
{
  ESP_LOGI(TAG, "Received ACM Read STREAM_PORT_OUTPUT Descriptor Request");

  struct aecp_stream_port_response_s
  {
    struct aecp_data_unit_s aecp_header;
    uint16_t configuration_index;
    uint16_t reserved;
    struct acm_desc_stream_port_s stream_port_desc;
  } __attribute__((packed));

  struct aecp_stream_port_response_s resp = {0};
  /* Copy Ethernet header from request and swap MAC addresses */
  memcpy(resp.aecp_header.header.dst_mac, msg->header.src_mac, ETH_ADDR_LEN);
  memcpy(resp.aecp_header.header.src_mac, msg->header.dst_mac, ETH_ADDR_LEN);
  /* Ethernet type (big-endian) */
  resp.aecp_header.header.eth_type[0] = (ETH_TYPE_AVTP >> 8) & 0xFF;
  resp.aecp_header.header.eth_type[1] = ETH_TYPE_AVTP & 0xFF;
  /* AECP header fields */
  resp.aecp_header.subtype = AVTP_SUBTYPE_AECP;
  resp.aecp_header.message_type = AECP_MSG_TYPE_AEM_RESPONSE;
  resp.aecp_header.version = 0;
  resp.aecp_header.h = 0;

  u8 status = 0; // Success
  u16 cdl = 36;
  (resp.aecp_header.control_data_len_status = htons(((status & 0x1F) << 11) | (cdl & 0x7FF)));
  resp.aecp_header.target_entity_id = msg->target_entity_id;
  resp.aecp_header.controller_entity_id = msg->controller_entity_id;

  resp.aecp_header.sequence_id = msg->sequence_id;
  resp.aecp_header.command_type = htons(ACM_COMMAND_TYPE_READ_DESCRIPTOR);
  /* Response payload fields */
  resp.configuration_index = 0;
  resp.reserved = 0;
  /* Fill STREAM_PORT_OUTPUT descriptor */
  resp.stream_port_desc.descriptor_type = htons(AEM_DESC_TYPE_STREAM_PORT_OUTPUT);
  resp.stream_port_desc.descriptor_index = 0;
  resp.stream_port_desc.clock_domain_index = htons(0);
  resp.stream_port_desc.port_flags = htons(1);
  resp.stream_port_desc.number_of_controls = htons(0);
  resp.stream_port_desc.base_control = htons(0);
  resp.stream_port_desc.number_of_clusters = htons(8);
  resp.stream_port_desc.base_cluster = htons(0);
  resp.stream_port_desc.number_of_maps = htons(1);
  resp.stream_port_desc.base_map = htons(1);

  /* Send the response */
  ssize_t written = write(s_state->socket, &resp, sizeof(resp));
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send STREAM_PORT_OUTPUT descriptor response: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Sent AECP STREAM_PORT_OUTPUT Descriptor Response");
  }
}

void handle_aem_read_desc_stream_output(struct avtp_state_s* s_state, struct aecp_data_unit_s* msg)
{
  ESP_LOGI(TAG, "Received ACM Read STREAM_OUTPUT Descriptor Request");


  const size_t num_formats = sizeof(stream_formats) / sizeof(stream_formats[0]);

  struct aecp_stream_input_response_s
  {
    struct aecp_data_unit_s aecp_header;
    uint16_t configuration_index;
    uint16_t reserved;
    struct acm_desc_stream_s stream_output_desc;
  };

  const size_t response_size = sizeof(struct aecp_stream_input_response_s) + (num_formats * sizeof(uint64_t));
  struct aecp_stream_input_response_s* resp = malloc(response_size);
  if (resp == NULL)
  {
    ESP_LOGE(TAG, "Failed to allocate memory for stream input response");
    return;
  }
  memset(resp, 0, response_size);
  /* Copy Ethernet header from request and swap MAC addresses */
  memcpy(resp->aecp_header.header.dst_mac, msg->header.src_mac, ETH_ADDR_LEN);
  memcpy(resp->aecp_header.header.src_mac, msg->header.dst_mac, ETH_ADDR_LEN);

  resp->aecp_header.header.eth_type[0] = (ETH_TYPE_AVTP >> 8) & 0xFF;
  resp->aecp_header.header.eth_type[1] = ETH_TYPE_AVTP & 0xFF;
  resp->aecp_header.subtype = AVTP_SUBTYPE_AECP;
  resp->aecp_header.message_type = AECP_MSG_TYPE_AEM_RESPONSE;
  resp->aecp_header.version = 0;
  resp->aecp_header.h = 0;

  u8 status = 0; // Success
  u16 cdl = sizeof(struct acm_desc_stream_s) + 4;
  resp->aecp_header.control_data_len_status = htons(((status & 0x1F) << 11) | (cdl & 0x7FF));
  resp->aecp_header.target_entity_id = msg->target_entity_id;
  resp->aecp_header.controller_entity_id = msg->controller_entity_id;

  resp->aecp_header.sequence_id = msg->sequence_id;
  resp->aecp_header.command_type = htons(ACM_COMMAND_TYPE_READ_DESCRIPTOR);
  resp->configuration_index = 0;
  resp->reserved = 0;
  /* Fill STREAM_OUTPUT descriptor */
  resp->stream_output_desc.descriptor_type = htons(AEM_DESC_TYPE_STREAM_OUTPUT);
  resp->stream_output_desc.descriptor_index = 0;

  strncpy((char*)resp->stream_output_desc.object_name, "Stream 1", sizeof(resp->stream_output_desc.object_name));
  resp->stream_output_desc.localized_description = htons(0);
  resp->stream_output_desc.clock_domain_index = htons(0);
  resp->stream_output_desc.stream_flags = htons(0x0002); // clock_sync_source
  resp->stream_output_desc.current_format = htonll(0x00a0020840000800);
  resp->stream_output_desc.formats_offset = htons(0);
  resp->stream_output_desc.number_of_formats = htons(1);
  resp->stream_output_desc.backup_talker_entity_id_0 = htonll(0);
  resp->stream_output_desc.backup_talker_unique_id_0 = htons(0);
  resp->stream_output_desc.backup_talker_entity_id_1 = htonll(0);
  resp->stream_output_desc.backup_talker_unique_id_1 = htons(0);
  resp->stream_output_desc.backup_talker_entity_id_2 = htonll(0);
  resp->stream_output_desc.backup_talker_unique_id_2 = htons(0);
  resp->stream_output_desc.backedup_talker_entity_id = htonll(0);
  resp->stream_output_desc.backedup_talker_unique_id = htons(0);
  resp->stream_output_desc.avb_interface_index = htons(0);
  resp->stream_output_desc.buffer_length = htonl(8);

  for (size_t i = 0; i < num_formats; ++i)
  {
    resp->stream_output_desc.formats[i] = htonll(stream_formats[i]);
  }

  ssize_t written = write(s_state->socket, resp, response_size);
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send STREAM_OUTPUT descriptor response: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Sent AECP STREAM_OUTPUT Descriptor Response");
  }
  free(resp);
}

void handle_aem_read_desc_stream_input(struct avtp_state_s* s_state, struct aecp_data_unit_s* msg)
{
  ESP_LOGI(TAG, "Received ACM Read STREAM_INPUT Descriptor Request");

  const size_t num_formats = sizeof(stream_formats) / sizeof(stream_formats[0]);

  struct aecp_stream_input_response_s
  {
    struct aecp_data_unit_s aecp_header;
    uint16_t configuration_index;
    uint16_t reserved;
    struct acm_desc_stream_s stream_input_desc;
  };

  const size_t response_size = sizeof(struct aecp_stream_input_response_s) + (num_formats * sizeof(uint64_t));
  struct aecp_stream_input_response_s* resp = malloc(response_size);
  if (resp == NULL)
  {
    ESP_LOGE(TAG, "Failed to allocate memory for stream input response");
    return;
  }
  memset(resp, 0, response_size);

  /* Copy Ethernet header from request and swap MAC addresses */
  memcpy(resp->aecp_header.header.dst_mac, msg->header.src_mac, ETH_ADDR_LEN);
  memcpy(resp->aecp_header.header.src_mac, msg->header.dst_mac, ETH_ADDR_LEN);
  /* Ethernet type (big-endian) */
  resp->aecp_header.header.eth_type[0] = (ETH_TYPE_AVTP >> 8) & 0xFF;
  resp->aecp_header.header.eth_type[1] = ETH_TYPE_AVTP & 0xFF;
  /* AECP header fields */
  resp->aecp_header.subtype = AVTP_SUBTYPE_AECP;
  resp->aecp_header.message_type = AECP_MSG_TYPE_AEM_RESPONSE;
  resp->aecp_header.version = 0;
  resp->aecp_header.h = 0;

  u8 status = 0; // Success
  const uint16_t desc_data_len = sizeof(struct acm_desc_stream_s) + (num_formats * sizeof(uint64_t));
  AECP_SET_CTRL_DATA_STATUS((&resp->aecp_header), status, desc_data_len);
  resp->aecp_header.target_entity_id = msg->target_entity_id;
  resp->aecp_header.controller_entity_id = msg->controller_entity_id;

  resp->aecp_header.sequence_id = msg->sequence_id;
  resp->aecp_header.command_type = htons(ACM_COMMAND_TYPE_READ_DESCRIPTOR);
  /* Response payload fields */
  resp->configuration_index = 0;
  resp->reserved = 0;
  /* Fill STREAM_INPUT descriptor */
  resp->stream_input_desc.descriptor_type = htons(AEM_DESC_TYPE_STREAM_INPUT);
  resp->stream_input_desc.descriptor_index = 0;
  strncpy((char*)resp->stream_input_desc.object_name, "Stream 1", sizeof(resp->stream_input_desc.object_name));
  resp->stream_input_desc.localized_description = htons(0);
  resp->stream_input_desc.clock_domain_index = htons(0);
  resp->stream_input_desc.stream_flags = htons(0x0003); // clock_sync_source | clock a flag
  resp->stream_input_desc.current_format = htonll(0x00a0020840000800);
  resp->stream_input_desc.formats_offset = htons(offsetof(struct acm_desc_stream_s, formats));
  resp->stream_input_desc.number_of_formats = htons(num_formats);
  resp->stream_input_desc.backup_talker_entity_id_0 = htonll(0);
  resp->stream_input_desc.backup_talker_unique_id_0 = htons(0);
  resp->stream_input_desc.backup_talker_entity_id_1 = htonll(0);
  resp->stream_input_desc.backup_talker_unique_id_1 = htons(0);
  resp->stream_input_desc.backup_talker_entity_id_2 = htonll(0);
  resp->stream_input_desc.backup_talker_unique_id_2 = htons(0);
  resp->stream_input_desc.backedup_talker_entity_id = htonll(0);
  resp->stream_input_desc.backedup_talker_unique_id = htons(0);
  resp->stream_input_desc.avb_interface_index = htons(0);
  // FIXME The length in nanoseconds of the MAC’s ingress buffer as defined in IEEE Std 17222016 Figure 5.
  // For a STREAM_INPUT this is the MAC’s ingress buffer size
  // This is the length of the buffer between the IEEE Std 17222016 reference plane and the MAC.
  resp->stream_input_desc.buffer_length = htonl(8);

  for (size_t i = 0; i < num_formats; ++i)
  {
    resp->stream_input_desc.formats[i] = htonll(stream_formats[i]);
  }


  /* Send the response */
  ssize_t written = write(s_state->socket, resp, response_size);
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send STREAM_INPUT descriptor response: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Sent AECP STREAM_INPUT Descriptor Response");
  }
  free(resp);
}

void handle_aem_read_desc_audio_unit(struct avtp_state_s* s_state, struct aecp_data_unit_s* msg)
{
  ESP_LOGI(TAG, "Received ACM Read AUDIO UNIT Descriptor Request");

  struct aecp_audio_unit_response_s
  {
    struct aecp_data_unit_s aecp_header;
    uint16_t configuration_index;
    uint16_t reserved;
    struct aecp_audio_unit_s audio_unit_desc;
  } __attribute__((packed));


  // Allocate response with space for the flexible array member
  struct aecp_audio_unit_response_s resp = {0};
  /* Copy Ethernet header from request and swap MAC addresses */
  memcpy(resp.aecp_header.header.dst_mac, msg->header.src_mac, ETH_ADDR_LEN);
  memcpy(resp.aecp_header.header.src_mac, msg->header.dst_mac, ETH_ADDR_LEN);
  /* Ethernet type (big-endian) */
  resp.aecp_header.header.eth_type[0] = (ETH_TYPE_AVTP >> 8) & 0xFF;
  resp.aecp_header.header.eth_type[1] = ETH_TYPE_AVTP & 0xFF;
  /* AECP header fields */
  resp.aecp_header.subtype = AVTP_SUBTYPE_AECP;
  resp.aecp_header.message_type = AECP_MSG_TYPE_AEM_RESPONSE;
  resp.aecp_header.version = 0;
  resp.aecp_header.h = 0;

  u8 status = 0; // Success
  u16 cdl = 164;
  (resp.aecp_header.control_data_len_status = htons(((status & 0x1F) << 11) | (cdl & 0x7FF)));
  resp.aecp_header.target_entity_id = msg->target_entity_id;
  resp.aecp_header.controller_entity_id = msg->controller_entity_id;

  resp.aecp_header.sequence_id = msg->sequence_id;
  resp.aecp_header.command_type = htons(ACM_COMMAND_TYPE_READ_DESCRIPTOR);
  /* Response payload fields */
  resp.configuration_index = 0;
  resp.reserved = 0;
  /* Fill AUDIO UNIT descriptor */
  resp.audio_unit_desc.descriptor_type = htons(AEM_DESC_TYPE_AUDIO_UNIT);
  resp.audio_unit_desc.descriptor_index = 0;
  memset(resp.audio_unit_desc.object_name, 0, sizeof(resp.audio_unit_desc.object_name));
  resp.audio_unit_desc.localized_description = htons(0xFFFF);
  resp.audio_unit_desc.number_of_stream_input_ports = htons(1);
  resp.audio_unit_desc.number_of_stream_output_ports = htons(1);
  resp.audio_unit_desc.number_of_external_input_ports = htons(CONFIG_LISTENER_STREAM_SINKS);
  resp.audio_unit_desc.number_of_external_output_ports = htons(CONFIG_TALKER_STREAM_SOURCES);
  resp.audio_unit_desc.current_sampling_rate = htonl(CONFIG_SAMPLING_RATE);
  resp.audio_unit_desc.sampling_rates_count = htons(1);
  resp.audio_unit_desc.sampling_rates_offset = htons(144);
  resp.audio_unit_desc.sampling_rates[0] = htonl(48000);


  /* Send the response */
  ssize_t written = write(s_state->socket, &resp, sizeof(resp));
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send AUDIO UNIT descriptor response: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Sent AECP AUDIO UNIT Descriptor Response");
  }
}


void handle_aem_read_desc_audio_cluster(struct avtp_state_s* s_state, struct aecp_read_desc_request_s* msg)
{
  ESP_LOGI(TAG, "Received ACM Read AUDIO CLUSTER Descriptor Request");

  struct aecp_audio_cluster_response_s
  {
    struct aecp_data_unit_s aecp_header;
    uint16_t configuration_index;
    uint16_t reserved;
    struct aecp_audio_cluster_s audio_cluster_desc;
  } __attribute__((packed));

  struct aecp_audio_cluster_response_s resp = {0};
  memcpy(&resp, msg, sizeof(struct aecp_read_desc_request_s));
  /* Copy Ethernet header from request and swap MAC addresses */
  memcpy(resp.aecp_header.header.dst_mac, msg->aecp_header.header.src_mac, ETH_ADDR_LEN);
  memcpy(resp.aecp_header.header.src_mac, msg->aecp_header.header.dst_mac, ETH_ADDR_LEN);

  resp.aecp_header.message_type = AECP_MSG_TYPE_AEM_RESPONSE;

  u8 status = 0; // Success
  u16 cdl = sizeof(struct aecp_audio_cluster_s) + 4;
  (resp.aecp_header.control_data_len_status = htons(((status & 0x1F) << 11) | (cdl & 0x7FF)));

  /* Fill AUDIO_CLUSTER descriptor */
  resp.audio_cluster_desc.descriptor_type = htons(AEM_DESC_TYPE_AUDIO_CLUSTER);
  resp.audio_cluster_desc.descriptor_index = msg->descriptor_index;
  memset(resp.audio_cluster_desc.object_name, 0, sizeof(resp.audio_cluster_desc.object_name));
  resp.audio_cluster_desc.localized_description = htons(0xFFFF);
  resp.audio_cluster_desc.signal_type = htons(0xFFFF);
  resp.audio_cluster_desc.signal_index = htons(msg->descriptor_index % 8);
  resp.audio_cluster_desc.signal_output = htons(0);
  resp.audio_cluster_desc.path_latency = htonl(0);
  resp.audio_cluster_desc.block_latency = htonl(0);
  resp.audio_cluster_desc.channel_count = htons(1);
  resp.audio_cluster_desc.format = 0x40; // MBLA IEEE1722.1 p.86

  /* Send the response */
  ssize_t written = write(s_state->socket, &resp, sizeof(resp));
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send AUDIO_CLUSTER descriptor response: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Sent AECP AUDIO_CLUSTER Descriptor Response");
  }
}

void handle_aem_read_desc_clock_source(struct avtp_state_s* state, struct aecp_data_unit_s* msg)
{
  ESP_LOGI(TAG, "Received ACM Read CLOCK SOURCE Descriptor Request");

  struct aecp_clock_source_response_s
  {
    struct aecp_data_unit_s aecp_header;
    u16 configuration_index;
    u16 reserved;
    u16 descriptor_type;
    u16 descriptor_index;
    u8 object_name[64];
    u16 localized_description;
    u16 clock_source_flags;
    u16 clock_source_type;
    u64 clock_source_id;
    u16 clock_source_location_type;
    u16 clock_source_location_id;
  } __attribute__((packed));

  struct aecp_clock_source_response_s resp = {0};
  /* Copy Ethernet header from request and swap MAC addresses */
  memcpy(resp.aecp_header.header.dst_mac, msg->header.src_mac, ETH_ADDR_LEN);
  memcpy(resp.aecp_header.header.src_mac, msg->header.dst_mac, ETH_ADDR_LEN);
  memcpy(resp.aecp_header.header.eth_type, msg->header.eth_type, 2);
  /* AECP header fields */
  resp.aecp_header.subtype = AVTP_SUBTYPE_AECP;
  resp.aecp_header.message_type = AECP_MSG_TYPE_AEM_RESPONSE;
  resp.aecp_header.version = 0;
  resp.aecp_header.h = 0;
  resp.aecp_header.control_data_len_status = htons(102);
  resp.aecp_header.target_entity_id = msg->target_entity_id;
  resp.aecp_header.controller_entity_id = msg->controller_entity_id;
  memset(&resp.object_name, 0, sizeof(resp.object_name));
  resp.aecp_header.sequence_id = msg->sequence_id;
  resp.aecp_header.command_type = htons(ACM_COMMAND_TYPE_READ_DESCRIPTOR);
  /* Response payload fields */
  resp.configuration_index = 0;
  resp.reserved = 0;
  /* Fill CLOCK SOURCE descriptor */
  resp.descriptor_type = htons(AEM_DESC_TYPE_CLOCK_SOURCE);
  resp.descriptor_index = 0;
  resp.localized_description = htons(2);
  resp.clock_source_flags = 0;
  resp.clock_source_type = htons(0); // INTERNAL
  resp.clock_source_id = htonll(state->entity_id);
  resp.clock_source_location_type = htons(0x0002);
  resp.clock_source_location_id = htons(0);

  //send
  ssize_t written = write(state->socket, &resp, sizeof(resp));
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send CLOCK SOURCE descriptor response: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Sent AECP CLOCK SOURCE Descriptor Response");
  }
}

void handle_aem_read_desc_avb_interface(struct avtp_state_s* s_state, struct aecp_data_unit_s* msg)
{
  ESP_LOGI(TAG, "Received ACM Read AVB INTERFACE Descriptor Request");

  struct aecp_avb_interface_response_s
  {
    struct aecp_data_unit_s aecp_header;
    uint16_t configuration_index;
    uint16_t reserved;
    struct aecp_avb_interface_s avb_interface_desc;
  } __attribute__((packed));

  struct aecp_avb_interface_response_s resp = {0};
  /* Copy Ethernet header from request and swap MAC addresses */
  memcpy(resp.aecp_header.header.dst_mac, msg->header.src_mac, ETH_ADDR_LEN);
  memcpy(resp.aecp_header.header.src_mac, msg->header.dst_mac, ETH_ADDR_LEN);
  /* Ethernet type (big-endian) */
  resp.aecp_header.header.eth_type[0] = (ETH_TYPE_AVTP >> 8) & 0xFF;
  resp.aecp_header.header.eth_type[1] = ETH_TYPE_AVTP & 0xFF;
  /* AECP header fields */
  resp.aecp_header.subtype = AVTP_SUBTYPE_AECP;
  resp.aecp_header.message_type = AECP_MSG_TYPE_AEM_RESPONSE;
  resp.aecp_header.version = 0;
  resp.aecp_header.h = 0;

  u8 status = 0; // Success
  u16 cdl = sizeof(struct aecp_avb_interface_s) + 4;
  (resp.aecp_header.control_data_len_status = htons(((status & 0x1F) << 11) | (cdl & 0x7FF)));
  resp.aecp_header.target_entity_id = msg->target_entity_id;
  resp.aecp_header.controller_entity_id = msg->controller_entity_id;

  resp.aecp_header.sequence_id = msg->sequence_id;
  resp.aecp_header.command_type = htons(ACM_COMMAND_TYPE_READ_DESCRIPTOR);
  /* Response payload fields */
  resp.configuration_index = 0;
  resp.reserved = 0;
  /* Fill AVB_INTERFACE descriptor */
  resp.avb_interface_desc.descriptor_type = htons(AEM_DESC_TYPE_AVB_INTERFACE);
  resp.avb_interface_desc.descriptor_index = 0;

  resp.avb_interface_desc.localized_description = htons(0xFFFF);
  memcpy(&resp.avb_interface_desc.mac_address, s_state->intf_hw_addr, ETH_ADDR_LEN);
  resp.avb_interface_desc.interface_flags = htons(7);
  resp.avb_interface_desc.clock_identity = htonll(s_state->entity_id);
  resp.avb_interface_desc.priority1 = 248;
  resp.avb_interface_desc.clock_class = 248;
  resp.avb_interface_desc.offset_scaled_log_variance = htons(17258);
  resp.avb_interface_desc.clock_accuracy = 248;
  resp.avb_interface_desc.priority2 = 238;
  resp.avb_interface_desc.domain_number = 0;
  resp.avb_interface_desc.log_sync_interval = -3;
  resp.avb_interface_desc.log_announce_interval = 0;
  resp.avb_interface_desc.log_pdelay_interval = 0;
  resp.avb_interface_desc.port_number = htons(0);


  /* Send the response */
  ssize_t written = write(s_state->socket, &resp, sizeof(resp));
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send AVB_INTERFACE descriptor response: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Sent AECP AVB_INTERFACE Descriptor Response");
  }
}

void hande_aem_read_desc_locale(struct avtp_state_s* state, struct aecp_data_unit_s* msg)
{
  ESP_LOGI(TAG, "Received ACM Read Locale Descriptor Request");

  if (state == NULL || state->socket < 0)
  {
    ESP_LOGE(TAG, "Socket not ready to send AECP response");
    return;
  }

  // LOCALE descriptor structure according to IEEE 1722.1
  struct aem_desc_locale_s
  {
    u16 descriptor_type; // 0x000C
    u16 descriptor_index; // Index of this descriptor
    u8 locale_identifier[64]; // UTF-8 locale string (e.g., "en-US")
    u16 number_of_strings; // Number of STRINGS descriptors
    u16 base_strings; // Base index for STRINGS descriptors
  } __attribute__((packed));

  struct aecp_locale_response_s
  {
    struct header_s header;
    struct subtype_data_s subtype_data;
    struct aecp_common_data_s common_data;
    u16 configuration_index;
    u16 reserved;
    struct aem_desc_locale_s descriptor;
  } __attribute__((packed));

  struct aecp_locale_response_s response = {0};

  /* Copy Ethernet header from request and swap MAC addresses */
  memcpy(response.header.dst_mac, msg->header.src_mac, ETH_ADDR_LEN);
  memcpy(response.header.src_mac, msg->header.dst_mac, ETH_ADDR_LEN);

  /* Ethernet type (big-endian) */
  response.header.eth_type[0] = (ETH_TYPE_AVTP >> 8) & 0xFF;
  response.header.eth_type[1] = ETH_TYPE_AVTP & 0xFF;

  /* AECP subtype data */
  response.subtype_data.subtype = AVTP_SUBTYPE_AECP;
  response.subtype_data.message_type = AECP_MSG_TYPE_AEM_RESPONSE;
  response.subtype_data.version = 0;
  response.subtype_data.h = 0;

  // Control data length: 14 bytes (descriptor) + 4 bytes (config_index + reserved) = 18 bytes
  response.subtype_data.control_data_len_status = htons(88);

  /* AECP common data */
  response.common_data.target_entity_id = msg->target_entity_id;
  response.common_data.controller_entity_id = msg->controller_entity_id;
  response.common_data.sequence_id = msg->sequence_id;
  response.common_data.command_type = htons(ACM_COMMAND_TYPE_READ_DESCRIPTOR);

  /* Configuration index */
  response.configuration_index = 0;
  response.reserved = 0;

  /* Fill LOCALE descriptor */
  response.descriptor.descriptor_type = htons(AEM_DESC_TYPE_LOCALE);
  response.descriptor.descriptor_index = htons(0x0000);
  // Set locale identifier to "en-US" (null-terminated, rest filled with zeros)
  strncpy((char*)response.descriptor.locale_identifier, CONFIG_LOCALE_IDENTIFIER,
          sizeof(response.descriptor.locale_identifier));

  // Number of STRINGS descriptors (3 as requested)
  response.descriptor.number_of_strings = htons(3);

  // Base STRINGS descriptor index (typically 0)
  response.descriptor.base_strings = htons(0);

  /* Send the response */
  ssize_t written = write(state->socket, &response, sizeof(response));
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send LOCALE descriptor response: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Sent AECP LOCALE Descriptor Response (%zd bytes)", written);
  }
}

void handle_aem_read_desc_clock_domain(struct avtp_state_s* state, struct aecp_data_unit_s* msg)
{
  ESP_LOGI(TAG, "Received ACM Read CLOCK DOMAIN Descriptor Request");

  if (state == NULL || state->socket < 0)
  {
    ESP_LOGE(TAG, "Socket not ready to send AECP response");
    return;
  }

  // CLOCK_DOMAIN descriptor structure according to IEEE 1722.1
  struct aem_desc_clock_domain_s
  {
    u16 descriptor_type; // 0x0024 (AEM_DESC_TYPE_CLOCK_DOMAIN)
    u16 descriptor_index; // Index of this descriptor
    u8 object_name[64]; // Name of the clock domain
    u16 localized_description; // Index to localized description
    u16 clock_source_index; // Current clock source index
    u16 clock_sources_offset; // Offset to clock_sources array
    u16 clock_sources_count; // Number of clock sources
    u16 clock_sources[1]; // Array of clock source indices (1 element)
  } __attribute__((packed));

  struct aecp_clock_domain_response_s
  {
    struct aecp_data_unit_s aecp_header;
    u16 configuration_index;
    u16 reserved;
    struct aem_desc_clock_domain_s descriptor;
  } __attribute__((packed));

  struct aecp_clock_domain_response_s resp = {0};

  /* Copy Ethernet header from request and swap MAC addresses */
  memcpy(resp.aecp_header.header.dst_mac, msg->header.src_mac, ETH_ADDR_LEN);
  memcpy(resp.aecp_header.header.src_mac, msg->header.dst_mac, ETH_ADDR_LEN);

  /* Ethernet type (big-endian) */
  resp.aecp_header.header.eth_type[0] = (ETH_TYPE_AVTP >> 8) & 0xFF;
  resp.aecp_header.header.eth_type[1] = ETH_TYPE_AVTP & 0xFF;

  /* AECP header fields */
  resp.aecp_header.subtype = AVTP_SUBTYPE_AECP;
  resp.aecp_header.message_type = AECP_MSG_TYPE_AEM_RESPONSE;
  resp.aecp_header.version = 0;
  resp.aecp_header.h = 0;

  // Control data length: descriptor size (76 bytes) + config_index + reserved (4 bytes) = 80 bytes
  u8 status = 0; // Success
  u16 cdl = sizeof(struct aem_desc_clock_domain_s) + 4;
  resp.aecp_header.control_data_len_status = htons(((status & 0x1F) << 11) | (cdl & 0x7FF));

  resp.aecp_header.target_entity_id = msg->target_entity_id;
  resp.aecp_header.controller_entity_id = msg->controller_entity_id;
  resp.aecp_header.sequence_id = msg->sequence_id;
  resp.aecp_header.command_type = htons(ACM_COMMAND_TYPE_READ_DESCRIPTOR);

  /* Configuration index */
  resp.configuration_index = 0;
  resp.reserved = 0;

  /* Fill CLOCK_DOMAIN descriptor */
  resp.descriptor.descriptor_type = htons(AEM_DESC_TYPE_CLOCK_DOMAIN);
  resp.descriptor.descriptor_index = htons(0x0000);

  // Set object name
  strncpy((char*)resp.descriptor.object_name, CONFIG_CLOCK_DOMAIN_NAME, sizeof(resp.descriptor.object_name));

  resp.descriptor.localized_description = htons(0x2);

  // Current clock source index (0 = internal clock source)
  resp.descriptor.clock_source_index = htons(0);

  // Clock sources offset (offset from start of descriptor to clock_sources array)
  // descriptor_type(2) + descriptor_index(2) + object_name(64) + localized_description(2) +
  // clock_source_index(2) + clock_sources_offset(2) + clock_sources_count(2) = 76 bytes
  resp.descriptor.clock_sources_offset = htons(76);

  // Number of available clock sources (1 = only internal)
  resp.descriptor.clock_sources_count = htons(1);

  // Clock sources array - contains index 0 (internal clock source)
  resp.descriptor.clock_sources[0] = htons(0);

  ESP_LOGI(TAG, "Sending CLOCK_DOMAIN descriptor: index=0, clock_source=0, count=1");

  /* Send the response */
  ssize_t written = write(state->socket, &resp, sizeof(resp));
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send CLOCK DOMAIN descriptor response: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Sent AECP CLOCK DOMAIN Descriptor Response (%zd bytes)", written);
  }
}

void handle_aem_read_desc_strings(struct avtp_state_s* state, struct aecp_data_unit_s* msg)
{
  // print descriptor Index
  struct aecp_aem_read_desc_cmd* read_desc_cmd = (struct aecp_aem_read_desc_cmd*)(msg + 1);
  ESP_LOGI(TAG, "Received ACM Read STRINGS Descriptor Request, Index: %d",
           ntohs(read_desc_cmd->descriptor_index));

  if (state == NULL || state->socket < 0)
  {
    ESP_LOGE(TAG, "Socket not ready to send AECP response");
    return;
  }

  // STRINGS descriptor structure according to IEEE 1722.1
  // Contains up to 7 strings (string_0 through string_6), each 64 bytes
  struct aem_desc_strings_s
  {
    u16 descriptor_type; // 0x000D (AEM_DESC_TYPE_STRINGS)
    u16 descriptor_index; // Index of this descriptor
    u8 string_0[64]; // First string (vendor name from CONFIG_VENDOR_NAME)
    u8 string_1[64]; // Second string (model name from CONFIG_MODEL_NAME)
    u8 string_2[64]; // Third string (clock source name from CONFIG_CLOCK_SOURCE_NAME)
    u8 string_3[64]; // Fourth string (empty)
    u8 string_4[64]; // Fifth string (empty)
    u8 string_5[64]; // Sixth string (empty)
    u8 string_6[64]; // Seventh string (empty)
  } __attribute__((packed));

  struct aecp_strings_response_s
  {
    struct aecp_data_unit_s aecp_header;
    u16 configuration_index;
    u16 reserved;
    struct aem_desc_strings_s descriptor;
  } __attribute__((packed));

  struct aecp_strings_response_s resp = {0};

  /* Copy Ethernet header from request and swap MAC addresses */
  memcpy(resp.aecp_header.header.dst_mac, msg->header.src_mac, ETH_ADDR_LEN);
  memcpy(resp.aecp_header.header.src_mac, msg->header.dst_mac, ETH_ADDR_LEN);

  /* Ethernet type (big-endian) */
  resp.aecp_header.header.eth_type[0] = (ETH_TYPE_AVTP >> 8) & 0xFF;
  resp.aecp_header.header.eth_type[1] = ETH_TYPE_AVTP & 0xFF;

  /* AECP header fields */
  resp.aecp_header.subtype = AVTP_SUBTYPE_AECP;
  resp.aecp_header.message_type = AECP_MSG_TYPE_AEM_RESPONSE;
  resp.aecp_header.version = 0;
  resp.aecp_header.h = 0;

  // Control data length: descriptor size (452 bytes) + config_index + reserved (4 bytes) = 456 bytes
  u8 status = 0; // Success
  u16 cdl = sizeof(struct aem_desc_strings_s) + 4;
  resp.aecp_header.control_data_len_status = htons(((status & 0x1F) << 11) | (cdl & 0x7FF));

  resp.aecp_header.target_entity_id = msg->target_entity_id;
  resp.aecp_header.controller_entity_id = msg->controller_entity_id;
  resp.aecp_header.sequence_id = msg->sequence_id;
  resp.aecp_header.command_type = htons(ACM_COMMAND_TYPE_READ_DESCRIPTOR);

  /* Configuration index */
  resp.configuration_index = 0;
  resp.reserved = 0;

  /* Fill STRINGS descriptor */
  resp.descriptor.descriptor_type = htons(AEM_DESC_TYPE_STRINGS);
  resp.descriptor.descriptor_index = read_desc_cmd->descriptor_index; // Echo the requested index

  // Set string_0 to vendor name
  strncpy((char*)resp.descriptor.string_0, CONFIG_VENDOR_NAME, sizeof(resp.descriptor.string_0));

  // Set string_1 to model name
  strncpy((char*)resp.descriptor.string_1, CONFIG_MODEL_NAME, sizeof(resp.descriptor.string_1));

  // Set string_2 to clock source name
  strncpy((char*)resp.descriptor.string_2, CONFIG_CLOCK_SOURCE_NAME, sizeof(resp.descriptor.string_2));
  // string_3 through string_6 remain zero-filled (empty strings)

  ESP_LOGI(TAG, "Sending STRINGS descriptor [%d]: string_0='%s', string_1='%s'",
           ntohs(read_desc_cmd->descriptor_index), CONFIG_VENDOR_NAME, CONFIG_MODEL_NAME);

  /* Send the response */
  ssize_t written = write(state->socket, &resp, sizeof(resp));
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send STRINGS descriptor response: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Sent AECP STRINGS Descriptor Response (%zd bytes)", written);
  }
}

void handle_aem_read_desc_external_port(struct avtp_state_s* state, struct aecp_read_desc_request_s* msg, u8 desc_type)
{
  struct external_port_response_s
  {
    struct aecp_data_unit_s aecp_header;
    u16 configuration_index;
    u16 reserved;
    u16 descriptor_type;
    u16 descriptor_index;
    u16 clock_domain_id;
    u16 port_flags;
    u16 number_of_controls;
    u16 base_control;
    u16 signal_type;
    u16 signal_index;
    u16 signal_output;
    u32 block_latency;
    u16 jack_id;
  } __attribute__((packed));

  ESP_LOGI(TAG, "Received ACM Read EXTERNAL PORT %s Descriptor Request",
           (desc_type == AEM_DESC_TYPE_EXTERNAL_PORT_INPUT) ? "INPUT" : "OUTPUT");

  struct external_port_response_s resp = {0};
  // Copy request to response as base
  memcpy(&resp, msg, sizeof(struct aecp_read_desc_request_s));
  /* Copy Ethernet header from request and swap MAC addresses */
  memcpy(resp.aecp_header.header.dst_mac, msg->aecp_header.header.src_mac, ETH_ADDR_LEN);
  memcpy(resp.aecp_header.header.src_mac, msg->aecp_header.header.dst_mac, ETH_ADDR_LEN);
  resp.aecp_header.control_data_len_status = htons(40);
  resp.aecp_header.message_type = AECP_MSG_TYPE_AEM_RESPONSE;
  resp.signal_type = htons(AEM_DESC_TYPE_AUDIO_CLUSTER); // Audio Cluster
  resp.signal_index = msg->descriptor_index;
  resp.descriptor_index = msg->descriptor_index;

  /* Send the response */
  ssize_t written = write(state->socket, &resp, sizeof(resp));
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send EXTERNAL PORT descriptor response: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Sent AECP EXTERNAL PORT Descriptor Response");
  }
}

void handle_aecp_aem_read_desc_cmd(struct avtp_state_s* s_state, struct aecp_data_unit_s* msg, ssize_t len)
{
  // TODO create a combined struct to pass down the handler
  struct aecp_aem_read_desc_cmd* read_desc_cmd = (struct aecp_aem_read_desc_cmd*)(msg + 1);
  u16 desc_type = ntohs(read_desc_cmd->descriptor_type);
  switch (desc_type)
  {
  case AEM_DESC_TYPE_ENTITY: // ENTITY Descriptor
    handle_aem_read_desc_entity(s_state, msg);
    break;
  case AEM_DESC_TYPE_CONFIGURATION:
    ESP_LOGI(TAG, "Received ACM Read CONFIGURATION Descriptor Request");
    handle_aem_read_configuration(s_state, msg);
    break;
  case AEM_DESC_TYPE_AUDIO_UNIT:
    handle_aem_read_desc_audio_unit(s_state, msg);
    break;
  case AEM_DESC_TYPE_STREAM_PORT_INPUT:
    handle_aem_read_desc_stream_port_input(s_state, msg);
    break;
  case AEM_DESC_TYPE_STREAM_PORT_OUTPUT:
    handle_aem_read_desc_stream_port_output(s_state, msg);
    break;
  case AEM_DESC_TYPE_AUDIO_CLUSTER:
    handle_aem_read_desc_audio_cluster(s_state, (struct aecp_read_desc_request_s*)msg);
    break;
  case AEM_DESC_TYPE_AUDIO_MAP:
    handle_aem_read_desc_audio_map(s_state, msg);
    break;
  case AEM_DESC_TYPE_STREAM_INPUT:
    handle_aem_read_desc_stream_input(s_state, msg);
    break;
  case AEM_DESC_TYPE_STREAM_OUTPUT:
    handle_aem_read_desc_stream_output(s_state, msg);
    break;
  case AEM_DESC_TYPE_AVB_INTERFACE:
    handle_aem_read_desc_avb_interface(s_state, msg);
    break;
  case AEM_DESC_TYPE_CLOCK_SOURCE:
    handle_aem_read_desc_clock_source(s_state, msg);
    break;
  case AEM_DESC_TYPE_LOCALE:
    hande_aem_read_desc_locale(s_state, msg);
    break;
  case AEM_DESC_TYPE_CLOCK_DOMAIN:
    handle_aem_read_desc_clock_domain(s_state, msg);
    break;
  case AEM_DESC_TYPE_STRINGS:
    handle_aem_read_desc_strings(s_state, msg);
    break;
  case AEM_DESC_TYPE_EXTERNAL_PORT_INPUT:
  case AEM_DESC_TYPE_EXTERNAL_PORT_OUTPUT:
    handle_aem_read_desc_external_port(s_state, (struct aecp_read_desc_request_s*)msg, desc_type);
    break;
  default:
    ESP_LOGW(TAG, "Unsupported ACM read descriptor type: 0x%04X", desc_type);
    break;
  }
}

void handle_aecp_acm_register_unsol_notification(struct avtp_state_s* s_state, struct aecp_data_unit_s* msg,
                                                 ssize_t len)
{
  char* desc_type_str = msg->command_type == htons(ACM_COMMAND_TYPE_REGISTER_UNSOLICITED_NOTIFICATION)
                          ? "REGISTER UNSOL NOTIFICATION"
                          : "UNREGISTER UNSOL NOTIFICATION";
  ESP_LOGI(TAG, "Received ACM %s command", desc_type_str);
  struct aecp_data_unit_s response = {0};
  memcpy(&response, msg, sizeof(struct aecp_data_unit_s));

  memcpy(response.header.dst_mac, msg->header.src_mac, ETH_ADDR_LEN);
  memcpy(response.header.src_mac, s_state->intf_hw_addr, ETH_ADDR_LEN);

  response.message_type = AECP_MSG_TYPE_AEM_RESPONSE;

  /* Send the response */
  ssize_t written = write(s_state->socket, &response, sizeof(response));
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send %s response", desc_type_str);
  }
  else
  {
    ESP_LOGI(TAG, "Sent %s response", desc_type_str);
  }
}

void handle_acm_get_sampling_rate(struct avtp_state_s* s_state, struct aecp_data_unit_s* msg)
{
  ESP_LOGI(TAG, "Received AECP ACM GET_SAMPLING_RATE Command");
  struct aecp_sampling_rate_response_s
  {
    struct header_s header;
    struct subtype_data_s subtype_data;
    struct aecp_common_data_s common_data;
    struct aecp_sampling_rate_s data;
    u8 padding[18]; // Padding to make total size 64 bytes
  } __attribute__((packed));

  struct aecp_sampling_rate_response_s response = {0};
  memcpy(response.header.dst_mac, msg->header.src_mac, ETH_ADDR_LEN);
  memcpy(response.header.src_mac, msg->header.dst_mac, ETH_ADDR_LEN);
  memcpy(response.header.eth_type, msg->header.eth_type, sizeof(msg->header.eth_type));

  response.subtype_data.message_type = AECP_MSG_TYPE_AEM_RESPONSE;
  response.subtype_data.version = msg->version;
  response.subtype_data.h = msg->h;
  response.subtype_data.subtype = msg->subtype;
  AECP_SET_CTRL_DATA_STATUS((&response.subtype_data), 0, 20);
  response.common_data.target_entity_id = msg->target_entity_id;
  response.common_data.controller_entity_id = msg->controller_entity_id;
  response.common_data.sequence_id = msg->sequence_id;
  response.common_data.command_type = htons(ACM_COMMAND_TYPE_GET_SAMPLING_RATE);

  response.data.descriptor_type = htons(AEM_DESC_TYPE_AUDIO_UNIT);
  response.data.descriptor_index = 0;

  response.data.sampling_rate = htonl(CONFIG_SAMPLING_RATE);

  // Padding is already filled with 0x00 due to struct initialization with {0}
  memset(response.padding, 0x00, sizeof(response.padding));


  /* Send the response */
  ssize_t written = write(s_state->socket, &response, sizeof(response));
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send ENTITY descriptor response: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Respond to ACM GET_SAMPLE_RATE request (64 bytes)");
  }
}

struct aecp_get_stream_format_request_s
{
  struct aecp_data_unit_s aecp_header;
  uint16_t descriptor_type;
  uint16_t descriptor_index;
} __attribute__((packed));

void handle_acm_get_stream_format(struct avtp_state_s* s_state, struct aecp_get_stream_format_request_s* msg)
{
  char* desc_type_str = msg->descriptor_type == htons(AEM_DESC_TYPE_STREAM_INPUT)
                          ? "STREAM_INPUT"
                          : msg->descriptor_type == htons(AEM_DESC_TYPE_STREAM_OUTPUT)
                          ? "STREAM_OUTPUT"
                          : "UNKNOWN";
  ESP_LOGI(TAG, "Received ACM GET_STREAM_FORMAT Command for %s Descriptor", desc_type_str);

  struct aecp_get_stream_format_response_s
  {
    struct aecp_data_unit_s aecp_header;
    u16 descriptor_type;
    u16 descriptor_index;
    u64 stream_format;
    u8 padding[14];
  } __attribute__((packed));

  struct aecp_get_stream_format_response_s resp = {0};
  /* Copy Ethernet header from request and swap MAC addresses */
  memcpy(resp.aecp_header.header.dst_mac, msg->aecp_header.header.src_mac, ETH_ADDR_LEN);
  memcpy(resp.aecp_header.header.src_mac, msg->aecp_header.header.dst_mac, ETH_ADDR_LEN);
  /* Ethernet type (big-endian) */
  resp.aecp_header.header.eth_type[0] = (ETH_TYPE_AVTP >> 8) & 0xFF;
  resp.aecp_header.header.eth_type[1] = ETH_TYPE_AVTP & 0xFF;
  /* AECP header fields */
  resp.aecp_header.subtype = AVTP_SUBTYPE_AECP;
  resp.aecp_header.message_type = AECP_MSG_TYPE_AEM_RESPONSE;
  resp.aecp_header.version = 0;
  resp.aecp_header.h = 0;

  u8 status = 0; // Success
  u16 cdl = 24;
  (resp.aecp_header.control_data_len_status = htons(((status & 0x1F) << 11) | (cdl & 0x7FF)));
  resp.aecp_header.target_entity_id = msg->aecp_header.target_entity_id;
  resp.aecp_header.controller_entity_id = msg->aecp_header.controller_entity_id;

  resp.aecp_header.sequence_id = msg->aecp_header.sequence_id;
  resp.aecp_header.command_type = htons(ACM_COMMAND_TYPE_GET_STREAM_FORMAT);
  /* Fill GET_STREAM_FORMAT descriptor */
  resp.descriptor_type = msg->descriptor_type;
  resp.descriptor_index = msg->descriptor_index;
  resp.stream_format = htonll(0x00a0020804000800); // 61883-6 48kHz 2ch 24bit


  /* Send the response */
  ssize_t written = write(s_state->socket, &resp, sizeof(resp));
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send GET_STREAM_FORMAT response: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Sent AECP GET_STREAM_FORMAT Response");
  }
}

int aecp_aem_command_handle(struct avtp_state_s* s_state, struct aecp_data_unit_s* msg, ssize_t len)
{
  if (msg == NULL || len < sizeof(struct aecp_data_unit_s))
  {
    ESP_LOGE(TAG, "Invalid AECP ACM command message or length");
    return ESP_FAIL;
  }

  /* Convert command_type from network byte order to host byte order */
  uint16_t command_type = ntohs(msg->command_type);

  uint64_t target_entity_id = ntohll(msg->target_entity_id);
  if (target_entity_id != s_state->entity_id)
  {
    // TODO change to debug level once verified
    ESP_LOGW(TAG, "AECP message not for this entity (target: 0x%016llX, our: 0x%016llX)",
             target_entity_id, s_state->entity_id);
    return ESP_OK;
  }

  switch (command_type)
  {
  case ACM_COMMAND_TYPE_ACQUIRE_ENTITY:
    ESP_LOGI(TAG, "Received AECP ACM Acquire Entity Command");
    break;
  case ACM_COMMAND_TYPE_READ_DESCRIPTOR:
    handle_aecp_aem_read_desc_cmd(s_state, msg, len);
    break;
  case ACM_COMMAND_TYPE_REGISTER_UNSOLICITED_NOTIFICATION:
  case ACM_COMMAND_TYPE_UNREGISTER_UNSOLICITED_NOTIFICATION:
    handle_aecp_acm_register_unsol_notification(s_state, msg, len);
    break;
  case ACM_COMMAND_TYPE_GET_SAMPLING_RATE:
    ESP_LOGI(TAG, "Received AECP ACM GET_SAMPLING_RATE Command");
    handle_acm_get_sampling_rate(s_state, msg);
    break;
  case ACM_COMMAND_TYPE_GET_STREAM_FORMAT:
    handle_acm_get_stream_format(s_state, (void*)msg);
    break;
  default:
    ESP_LOGW(TAG, "Received unimplemented AECP ACM Command type: 0x%04X", command_type);
  }
  return ESP_OK;
}

int aecp_net_rx(struct avtp_state_s* state, struct aecp_data_unit_s* msg, ssize_t len)
{
  if (msg == NULL || len < sizeof(struct aecp_data_unit_s))
  {
    ESP_LOGE(TAG, "Invalid AECP message or length");
    return ESP_FAIL;
  }

  switch (msg->message_type)
  {
  case AECP_MSG_TYPE_AEM_COMMAND:
    aecp_aem_command_handle(state, msg, len);
    break;
  case AECP_MSG_TYPE_AEM_RESPONSE:
    ESP_LOGI(TAG, "AECP ACM Response Message Received");
    break;
  case AECP_MSG_TYPE_VENDOR_UNIQUE_COMMAND:
    ESP_LOGW(TAG, "AECP Vendor Unique Command Message Received - unimplemented");
    break;
  default:
    ESP_LOGW(TAG, "Unknown AECP message type: 0x%X", msg->message_type);
    break;
  }

  return ESP_OK;
}
