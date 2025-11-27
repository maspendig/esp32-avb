//
// Created by max on 11/26/25.
//

#include "msrp.h"

#include <cc.h>

#include "avtp.h"
#include "types.h"
#include "esp_vfs_l2tap.h"
#include "esp_eth_driver.h"

#include <fcntl.h>
#include <esp_err.h>
#include <esp_log.h>
#include <sys/ioctl.h>
#include <sys/unistd.h>

#define TAG "msrp"

int msrp_send_talker_advertise(struct avtp_state_s* state)
{
  struct talker_advertise_s msg = {0};

  const u8 acmp_multicast_mac[6] = {0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E}; // MSRP multicast MAC
  memcpy(msg.header.dst_mac, acmp_multicast_mac, sizeof(msg.header.dst_mac));
  memcpy(msg.header.src_mac, state->intf_hw_addr, sizeof(msg.header.src_mac));
  msg.header.eth_type[0] = (ETH_TYPE_MSRP >> 8) & 0xFF;
  msg.header.eth_type[1] = ETH_TYPE_MSRP & 0xFF;

  msg.attribute_type = MSRP_ATTRIBUTE_TYPE_TALKER_ADVERTISE;
  msg.attribute_length = 25; // IEEE 802.1Qat-2010 P.57 Table 35-2
  msg.attribute_list_length = htons(30);

  msg.number_of_values = htons(1);
  msg.stream_id = htonll(0x0000000000000001); //
  const u8 maap_mac[6] = {0x91, 0xe0, 0xf0, 0x00, 0xfe, 0x00}; // Example MAAP MAC
  memcpy(msg.stream_da, maap_mac, sizeof(msg.stream_da));
  msg.stream_vlan_id = htons(2);
  msg.max_frame_size = htons(224);
  msg.max_frame_interval = htons(1);
  msg.priority = 3;
  msg.rank = 1;
  msg.accumulated_latency = htonl(100095);
  msg.attribute_event = 0x6c; // Talker Advertise

  ESP_LOG_BUFFER_HEX_LEVEL(TAG, (uint8_t*)&msg, sizeof(msg), ESP_LOG_INFO);

  // send via msrp_socket
  const ssize_t written = write(state->msrp_socket, &msg, sizeof(msg));
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send MSRP Talker Advertise: %d (errno: %d)", written, errno);
    return ESP_FAIL;
  }

  return ESP_OK;
}

int msrp_send_listener_join_request(struct avtp_state_s* state)
{
  struct attribute_vector_s
  {
    u16 leave_all_event : 3;
    u16 number_of_values : 13;
    u64 stream_id;
    u8 attribute_event;
    u8 declaration_type;
  } __attribute__((packed));

  struct msrp_listener_join_request
  {
    struct header_s header;
    u8 protocol_version;
    u8 attribute_type;
    u8 attribute_length;
    u16 attribute_list_length;
    struct attribute_vector_s attribute_list[1];
    u16 end_mark_list;
    u16 end_mark;
  } __attribute__((packed));
  struct msrp_listener_join_request msg;

  const u8 acmp_multicast_mac[6] = {0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E}; // MSRP multicast MAC
  memcpy(msg.header.dst_mac, acmp_multicast_mac, sizeof(msg.header.dst_mac));
  memcpy(msg.header.src_mac, state->intf_hw_addr, sizeof(msg.header.src_mac));

  // Ethernet type (big-endian)
  msg.header.eth_type[0] = (ETH_TYPE_MSRP >> 8) & 0xFF;
  msg.header.eth_type[1] = ETH_TYPE_MSRP & 0xFF;

  msg.attribute_type = MSRP_ATTRIBUTE_TYPE_LISTENER;
  msg.attribute_length = 8; // IEEE 802.1Qat-2010 P.57 Table 35-2
  msg.attribute_list_length = htons(14);
  msg.attribute_list[0].stream_id = htonll(0x0000000000000000); // Example stream ID
  msg.attribute_list[0].leave_all_event = 0;
  msg.attribute_list[0].number_of_values = htons(1);
  msg.attribute_list[0].attribute_event = 0x24;
  msg.attribute_list[0].declaration_type = 0x80;
  msg.end_mark_list = 0;
  msg.end_mark = 0;

  ESP_LOG_BUFFER_HEX_LEVEL(TAG, (uint8_t*)&msg, sizeof(msg), ESP_LOG_INFO);

  const ssize_t written = write(state->msrp_socket, &msg, 64);
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send ACMP Message: %d (errno: %d)", written, errno);
    return ESP_FAIL;
  }
  return ESP_OK;
}

void read_msrp_net(const struct avtp_state_s* state)
{
  union
  {
    struct msrp_header_s header;
    u8 raw[100];
  } buf;
  const ssize_t len = read(state->msrp_socket, &buf, sizeof(buf));
  if (len > 0)
  {
    switch (buf.header.attribute_type)
    {
    case MSRP_ATTRIBUTE_TYPE_TALKER_ADVERTISE:
      ESP_LOGI(TAG, "Received MSRP Talker Advertise message");
      break;
    case MSRP_ATTRIBUTE_TYPE_TALKER_FAILED:
      ESP_LOGI(TAG, "Received MSRP Talker Failed message");
      break;
    case MSRP_ATTRIBUTE_TYPE_LISTENER:
      ESP_LOGI(TAG, "Received MSRP Listener message");
      break;
    case MSRP_ATTRIBUTE_TYPE_DOMAIN:
      ESP_LOGI(TAG, "Received MSRP Domain message");
      break;
    default:
      ESP_LOGW(TAG, "Unknown MSRP attribute type received: %d", buf.header.attribute_type);
      break;
    }
  }
  else if (len < 0)
  {
    ESP_LOGE(TAG, "Failed to read MSRP message: %d (errno: %d)", len, errno);
  }
}

int msrp_init(const char* interface)
{
  // Initialize MSRP socket
  int socket = open("/dev/net/tap", 0);
  if (socket < 0)
  {
    ESP_LOGE(TAG, "Failed to create MSRP socket");
    return ESP_FAIL;
  }

  const int ioctl_err = ioctl(socket, L2TAP_S_INTF_DEVICE, interface);
  if (ioctl_err < 0)
  {
    ESP_LOGE(TAG, "failed to set network interface %s at MSRP socket: %d\n", interface, ioctl_err);
    close(socket);
    return ESP_FAIL;
  }

  uint16_t eth_type_filter = ETH_TYPE_MSRP;
  if (ioctl(socket, L2TAP_S_RCV_FILTER, &eth_type_filter) < 0)
  {
    ESP_LOGE(TAG, "failed to set MSRP Ethertype filter: %d\n", errno);
    close(socket);
    return ESP_FAIL;
  }

  return socket;
}
