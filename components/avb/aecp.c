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

  /* Set entity name */
  const char* entity_name = "ESP32-AVB Entity";
  strncpy((char*)response.descriptor.entity_name, entity_name, sizeof(response.descriptor.entity_name));

  response.descriptor.vendor_name_string = htons(0);
  response.descriptor.model_name_string = htons(0);

  /* Set firmware version */
  const char* fw_version = "0.0.1";
  strncpy((char*)response.descriptor.firmware_version, fw_version, sizeof(response.descriptor.firmware_version));

  /* Set group name */
  const char* group_name = "ESP32-AVB";
  strncpy((char*)response.descriptor.group_name, group_name, sizeof(response.descriptor.group_name));

  /* Set serial number */
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
  const uint16_t desc_data_len = sizeof(struct config_desc_s) + num_desc_types * sizeof(struct desc_count_s);
  ACMP_SET_CTRL_DATA_STATUS((&resp->aecp_header), 0, desc_data_len);

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
  resp->config_desc.localized_description = htons(2);
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

  auto status = 0; // Success
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

  ESP_LOG_BUFFER_HEX_LEVEL(TAG, resp, response_size, ESP_LOG_INFO);

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

  auto status = 0; // Success
  auto cdl = 36;
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

  ESP_LOG_BUFFER_HEX_LEVEL(TAG, &resp, sizeof(resp), ESP_LOG_INFO);

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

  auto status = 0; // Success
  auto cdl = 36;
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

  auto status = 0; // Success
  auto cdl = sizeof(struct acm_desc_stream_s) + 4;
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
  memset(resp->stream_output_desc.object_name, 0, sizeof(resp->stream_output_desc.object_name));
  resp->stream_output_desc.localized_description = htons(0);
  resp->stream_output_desc.clock_domain_index = htons(0);
  resp->stream_output_desc.stream_flags = htons(0x0001); // clock_sync_source
  resp->stream_output_desc.current_format = htonll(0x00A0020201000030); // 61883-6 48kHz 2ch 24bit
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

  auto status = 0; // Success
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
  memset(resp->stream_input_desc.object_name, 0, sizeof(resp->stream_input_desc.object_name));
  resp->stream_input_desc.localized_description = htons(0);
  resp->stream_input_desc.clock_domain_index = htons(0);
  resp->stream_input_desc.stream_flags = htons(0x0001); // clock_sync_source
  resp->stream_input_desc.current_format = htonll(stream_formats[0]);
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

  auto status = 0; // Success
  auto cdl = sizeof(struct aecp_audio_unit_s) + 4;
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
  resp.audio_unit_desc.localized_description = htons(1);
  resp.audio_unit_desc.number_of_stream_input_ports = htons(1);
  resp.audio_unit_desc.number_of_stream_output_ports = htons(1);
  resp.audio_unit_desc.number_of_external_input_ports = htons(CONFIG_LISTENER_STREAM_SINKS);
  resp.audio_unit_desc.number_of_external_input_ports = htons(CONFIG_TALKER_STREAM_SOURCES);
  resp.audio_unit_desc.current_sampling_rate = htonl(CONFIG_SAMPLING_RATE);
  resp.audio_unit_desc.sampling_rates_count = htons(1);
  resp.audio_unit_desc.sampling_rates_offset = htons(144);
  resp.audio_unit_desc.sampling_rates[0] = htonl(CONFIG_SAMPLING_RATE);

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

void handle_aem_read_desc_audio_cluster(struct avtp_state_s* s_state, struct aecp_data_unit_s* msg)
{
  ESP_LOGI(TAG, "Received ACM Read AUDIO CLUSTER Descriptor Request");

  struct aecp_aem_read_desc_cmd* read_desc_cmd = (struct aecp_aem_read_desc_cmd*)(msg + 1);

  struct aecp_audio_cluster_response_s
  {
    struct aecp_data_unit_s aecp_header;
    uint16_t configuration_index;
    uint16_t reserved;
    struct aecp_audio_cluster_s audio_cluster_desc;
  } __attribute__((packed));

  struct aecp_audio_cluster_response_s resp = {0};
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

  auto status = 0; // Success
  auto cdl = sizeof(struct aecp_audio_cluster_s) + 4;
  (resp.aecp_header.control_data_len_status = htons(((status & 0x1F) << 11) | (cdl & 0x7FF)));
  resp.aecp_header.target_entity_id = msg->target_entity_id;
  resp.aecp_header.controller_entity_id = msg->controller_entity_id;

  resp.aecp_header.sequence_id = msg->sequence_id;
  resp.aecp_header.command_type = htons(ACM_COMMAND_TYPE_READ_DESCRIPTOR);
  /* Response payload fields */
  resp.configuration_index = 0;
  resp.reserved = 0;
  /* Fill AUDIO_CLUSTER descriptor */
  resp.audio_cluster_desc.descriptor_type = htons(AEM_DESC_TYPE_AUDIO_CLUSTER);
  resp.audio_cluster_desc.descriptor_index = read_desc_cmd->descriptor_index;
  memset(resp.audio_cluster_desc.object_name, 0, sizeof(resp.audio_cluster_desc.object_name));
  resp.audio_cluster_desc.localized_description = htons(0);
  resp.audio_cluster_desc.signal_type = htons(-1);
  resp.audio_cluster_desc.signal_index = htons(0);
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

void handle_aecp_aem_read_desc_cmd(struct avtp_state_s* s_state, struct aecp_data_unit_s* msg, ssize_t len)
{
  // TODO create a combined struct to pass down the handler
  struct aecp_aem_read_desc_cmd* read_desc_cmd = (struct aecp_aem_read_desc_cmd*)(msg + 1);
  auto desc_type = ntohs(read_desc_cmd->descriptor_type);
  switch (desc_type)
  {
  case AEM_DESC_TYPE_ENTITY: // ENTITY Descriptor
    ESP_LOGI(TAG, "Received ACM Read ENTITY Descriptor Request");
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
    handle_aem_read_desc_audio_cluster(s_state, msg);
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
    ESP_LOGI(TAG, "Received ACM Read AVB INTERFACE Descriptor Request");
    break;
  default:
    ESP_LOGW(TAG, "Unsupported ACM read descriptor type: 0x%04X", desc_type);
    break;
  }
}

void handle_aecp_acm_register_unsol_notification(struct avtp_state_s* s_state, struct aecp_data_unit_s* msg,
                                                 ssize_t len)
{
  struct aecp_data_unit_s response = {0};
  memcpy(&response, msg, sizeof(struct aecp_data_unit_s));

  response.message_type = AECP_MSG_TYPE_AEM_RESPONSE;

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

void handle_acm_get_sampling_rate(struct avtp_state_s* s_state, struct aecp_data_unit_s* msg)
{
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

  // ESP_LOG_BUFFER_HEX_LEVEL(TAG, (uint8_t*)&response, sizeof(response), ESP_LOG_INFO);

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

/* Handle AECP ATDECC Entity Model Command messages */
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
    ESP_LOGI(TAG, "Received AECP ACM register Unsolicited Notification Command");
    handle_aecp_acm_register_unsol_notification(s_state, msg, len);
    break;
  case ACM_COMMAND_TYPE_UNREGISTER_UNSOLICITED_NOTIFICATION:
    ESP_LOGI(TAG, "Received AECP ACM unregister Unsolicited Notification Command");
    break;
  case ACM_COMMAND_TYPE_GET_SAMPLING_RATE:
    ESP_LOGI(TAG, "Received AECP ACM GET_SAMPLING_RATE Command");
    handle_acm_get_sampling_rate(s_state, msg);
    break;
  default:
    ESP_LOGW(TAG, "Recieved unimplemented AECP ACM Command type: 0x%04X", command_type);
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
  default:
    ESP_LOGW(TAG, "Unknown AECP message type: 0x%X", msg->message_type);
    break;
  }

  return ESP_OK;
}
