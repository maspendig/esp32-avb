//
// Created by max on 11/26/25.
//

#include "msrp.h"

#include <string.h>
#include <cc.h>
#include <config.h>

#include "avtp.h"
#include "types.h"
#include "esp_vfs_l2tap.h"
#include "esp_eth_driver.h"

#include <fcntl.h>
#include <esp_err.h>
#include <esp_log.h>
#include <sys/ioctl.h>
#include <sys/unistd.h>
#include <arpa/inet.h>

#define TAG "msrp"

char* msrp_attribute_type_to_str(msrp_attribute_type_t type)
{
  switch (type)
  {
  case MSRP_TALKER_ADVERTISE: return "Talker Advertise";
  case MSRP_TALKER_FAILED: return "Talker Failed";
  case MSRP_LISTENER: return "Listener";
  case MSRP_DOMAIN: return "Domain";
  default: return "UNKNOWN";
  }
}

bool validate_attribute_length(const msrp_attribute_t* attrib)
{
  switch (attrib->attribute_type)
  {
  case MSRP_TALKER_ADVERTISE: return attrib->attribute_length == MSRP_ATTRIBUTE_LENGTH_TALKER_ADVERTISE;
  case MSRP_TALKER_FAILED: return attrib->attribute_length == MSRP_ATTRIBUTE_LENGTH_TALKER_FAILED;
  case MSRP_LISTENER: return attrib->attribute_length == MSRP_ATTRIBUTE_LENGTH_LISTENER;
  case MSRP_DOMAIN: return attrib->attribute_length == MSRP_ATTRIBUTE_LENGTH_DOMAIN;
  default: return false;
  }
}

void msrp_process_rx(struct avtp_state_s* state, const u8* buf, size_t len)
{
  struct msrp_packet_s
  {
    struct header_s header;
    u8 protocol_version;
  } __attribute__((packed));


  u16 attribute_pointer = sizeof(struct msrp_packet_s); // header + protocol_version
  while (len > attribute_pointer)
  {
    // check for end mark 0x0000
    if (buf[attribute_pointer] == 0x00 && buf[attribute_pointer + 1] == 0x00)
    {
      break;
    }

    const msrp_attribute_t* attrib = (msrp_attribute_t*)(buf + attribute_pointer);
    if (validate_attribute_length(attrib) == false)
    {
      ESP_LOGW(TAG, "Invalid MSRP attribute length: type %s, length %d",
               msrp_attribute_type_to_str((msrp_attribute_type_t)attrib->attribute_type),
               attrib->attribute_length);
      break;
    }

    ESP_LOGI(TAG, "Received MSRP attribute type: %s",
             msrp_attribute_type_to_str((msrp_attribute_type_t)attrib->attribute_type));

    attribute_pointer += sizeof(msrp_attribute_t) + ntohs(attrib->attribute_list_length);
  }
}

void msrp_net_rx(struct avtp_state_s* state)
{
  u8 buf[128];

  ssize_t len = read(state->msrp_socket, buf, sizeof(buf));
  if (len <= 0)
  {
    return;
  }
  msrp_process_rx(state, buf, len);
}

void msrp_state_init(msrp_state_t* state)
{
  if (state == NULL)
  {
    ESP_LOGE(TAG, "MSRP state pointer is NULL");
    return;
  }

  /* Initialize the MRP attribute structure */
  memset(state, 0, sizeof(msrp_state_t));
  struct mrp_application* msrp = malloc(sizeof(struct mrp_application));

  msrp->type = MSRP;

  state->mrp.app = msrp;

  ESP_LOGI(TAG, "MSRP state initialized");
}

int msrp_init_socket(const char* interface)
{
  /* Initialize MSRP socket */
  int socket = open("/dev/net/tap", 0);
  if (socket < 0)
  {
    ESP_LOGE(TAG, "Failed to create MSRP socket");
    return -1;
  }

  int ioctl_err = ioctl(socket, L2TAP_S_INTF_DEVICE, interface);
  if (ioctl_err < 0)
  {
    ESP_LOGE(TAG, "Failed to set network interface %s at MSRP socket: %d", interface, ioctl_err);
    close(socket);
    return -1;
  }

  uint16_t eth_type_filter = ETH_TYPE_MSRP;
  if (ioctl(socket, L2TAP_S_RCV_FILTER, &eth_type_filter) < 0)
  {
    ESP_LOGE(TAG, "Failed to set MSRP Ethertype filter: %d", errno);
    close(socket);
    return -1;
  }

  ESP_LOGI(TAG, "MSRP socket initialized on interface %s", interface);

  return socket;
}

