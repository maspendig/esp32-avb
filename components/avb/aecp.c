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

static void send_entity_descriptor_response(struct avtp_state_s* s_state, struct aecp_data_unit_s* request_msg)
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
  response.descriptor.descriptor_type = htons(0x0000); // ENTITY
  response.descriptor.descriptor_index = htons(0x0000);
  response.descriptor.entity_id = htonll(s_state->entity_id);
  response.descriptor.entity_model_id = htonll(s_state->entity_model_id);
  response.descriptor.entity_capabilities = htonl(0x0000C508); // Example capabilities
  response.descriptor.talker_stream_sources = htons(1);
  response.descriptor.talker_capabilities = htons(0x4001);
  response.descriptor.listener_stream_sinks = htons(1);
  response.descriptor.listener_capabilities = htons(0x4001);
  response.descriptor.controller_capabilities = htonl(0);
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

void send_configuration_response(struct avtp_state_s* s_state, struct aecp_data_unit_s* request_msg)
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
  ESP_LOG_BUFFER_HEX_LEVEL(TAG, &resp, sizeof(resp), ESP_LOG_INFO);

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

void handle_aecp_aem_read_desc_cmd(struct avtp_state_s* s_state, struct aecp_data_unit_s* msg, ssize_t len)
{
  struct aecp_aem_read_desc_cmd* read_desc_cmd = (struct aecp_aem_read_desc_cmd*)(msg + 1);
  auto desc_type = ntohs(read_desc_cmd->descriptor_type);
  switch (desc_type)
  {
  case AEM_DESC_TYPE_ENTITY: // ENTITY Descriptor
    ESP_LOGI(TAG, "Received ACM Read ENTITY Descriptor Request");
    send_entity_descriptor_response(s_state, msg);
    break;
  case AEM_DESC_TYPE_CONFIGURATION:
    ESP_LOGI(TAG, "Received ACM Read CONFIGURATION Descriptor Request");
    send_configuration_response(s_state, msg);
    break;
  case AEM_DESC_TYPE_AUDIO_UNIT:
    handle_aem_read_desc_audio_unit(s_state, msg);
    break;
  case AEM_DESC_TYPE_STREAM_INPUT:
    ESP_LOGI(TAG, "Received ACM Read STREAM_INPUT Descriptor Request");
    // TODO implement
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
