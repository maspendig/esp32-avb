
#include "aecp.h"

#include <acmp.h>

#include "avtp.h"

#include <cc.h>
#include <esp_err.h>
#include <esp_eth_spec.h>
#include <esp_log.h>
#include <types.h>
#include <sys/unistd.h>

#define TAG "aecp"

static void send_entity_descriptor_response(struct avtp_state_s* s_state, struct aecp_data_unit_s* request_msg,
                                            uint16_t configuration_index)
{
  if (s_state == NULL || s_state->socket < 0)
  {
    ESP_LOGE(TAG, "Socket not ready to send AECP response");
    return;
  }

  struct aecp_read_descriptor_response_s response = {0};

  /* Copy Ethernet header from request and swap MAC addresses */
  memcpy(response.aecp_header.header.dst_mac, request_msg->header.src_mac, ETH_ADDR_LEN);
  memcpy(response.aecp_header.header.src_mac, s_state->intf_hw_addr, ETH_ADDR_LEN);

  /* Ethernet type (big-endian) */
  response.aecp_header.header.eth_type[0] = (ETH_TYPE_AVTP >> 8) & 0xFF;
  response.aecp_header.header.eth_type[1] = ETH_TYPE_AVTP & 0xFF;

  /* AECP header fields */
  response.aecp_header.subtype = AVTP_SUBTYPE_AECP;
  response.aecp_header.message_type = AECP_MSG_TYPE_AEM_RESPONSE;
  response.aecp_header.version = 0;
  response.aecp_header.h = 0;

  ACMP_SET_CTRL_DATA_STATUS((&response.aecp_header), 0, 44);

  /* Swap entity IDs - we become the target, controller becomes the controller */
  response.aecp_header.target_entity_id = request_msg->controller_entity_id;
  response.aecp_header.controller_entity_id = request_msg->target_entity_id;
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

  /* Check if the message is targeted to this entity */
  uint64_t target_entity_id = ntohll(msg->target_entity_id);
  if (target_entity_id != s_state->entity_id)
  {
    ESP_LOGV(TAG, "AECP message not for this entity (target: 0x%016llX, our: 0x%016llX)",
             (unsigned long long)target_entity_id, (unsigned long long)s_state->entity_id);
    return ESP_OK;
  }

  uint8_t* payload = (uint8_t*)msg + sizeof(struct aecp_data_unit_s);
  switch (command_type)
  {
  case ACM_COMMAND_TYPE_READ_DESCRIPTOR:
    {
      uint16_t configuration_index = ntohs(*(uint16_t*)(payload + 0));
      uint16_t descriptor_type = ntohs(*(uint16_t*)(payload + 4));

      switch (descriptor_type)
      {
      case 0x0000: // ENTITY Descriptor
        ESP_LOGI(TAG, "Received AECP ACM Read Entity Descriptor Request");
        send_entity_descriptor_response(s_state, msg, configuration_index);
        break;
      default:
        ESP_LOGW(TAG, "Unsupported ACM read descriptor type: 0x%04X", descriptor_type);
        break;
      }
    }
    break;
  case ACM_COMMAND_TYPE_REGISTER_UNSOLICITED_NOTIFICATION:
    {
      ESP_LOGI(TAG, "Received AECP ACM Register Unsolicited Notification Command");

      /* Check if message has payload (flags field) */
      ssize_t payload_offset = sizeof(struct aecp_data_unit_s);
      bool time_limited = 0;

      if (len > payload_offset)
      {
        /* Payload exists, read flags and extract time_limited bit */
        uint32_t flags = ntohl(*(uint32_t*)(payload + 0));
        time_limited = flags & 0x1; // Least significant bit
        ESP_LOGI(TAG, "  Flags: 0x%08X, Time Limited: %d", flags, time_limited);
      }
      else
      {
        /* No payload, time_limited defaults to 0 */
        ESP_LOGI(TAG, "  No payload, Time Limited: 0");
      }
    }
    break;
  case ACM_COMMAND_TYPE_UNREGISTER_UNSOLICITED_NOTIFICATION:
    ESP_LOGI(TAG, "Received AECP ACM Register Unsolicited Notification Command");
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
