//
// Created by max on 11/27/25.
//

#include "mvrp.h"
#include "msrp.h"
#include <cc.h>

#include "avtp.h"
#include "types.h"
#include "esp_vfs_l2tap.h"
#include "esp_eth_driver.h"

#include <esp_err.h>
#include <esp_log.h>
#include <sys/ioctl.h>
#include <sys/unistd.h>
#include <arpa/inet.h>

#define TAG "mvrp"

int mvrp_send_vlan_join(struct avtp_state_s* state, uint16_t vlan_id)
{
  // MVRP VID Attribute Vector structure
  struct attribute_vector_s
  {
    u16 leave_all_event : 3;
    u16 number_of_values : 13;
    u16 first_value; // First VID in vector
    u8 attribute_events; // Three-packed events (3 events in 1 byte)
  } __attribute__((packed));

  struct mvrp_vlan_join_msg
  {
    struct header_s header;
    u8 protocol_version;
    u8 attribute_type;
    u8 attribute_length;
    struct attribute_vector_s attribute_list[1];
    u16 end_mark_list;
    u16 end_mark;
    u8 padding[38]; // Padding to reach 64 bytes
  } __attribute__((packed));

  struct mvrp_vlan_join_msg msg = {0};

  // MVRP multicast MAC address (IEEE 802.1Q-2018)
  const u8 mvrp_multicast_mac[6] = {0x01, 0x80, 0xC2, 0x00, 0x00, 0x21};
  memcpy(msg.header.dst_mac, mvrp_multicast_mac, sizeof(msg.header.dst_mac));
  memcpy(msg.header.src_mac, state->intf_hw_addr, sizeof(msg.header.src_mac));

  // Ethernet type (big-endian)
  msg.header.eth_type[0] = (ETH_TYPE_MVRP >> 8) & 0xFF;
  msg.header.eth_type[1] = ETH_TYPE_MVRP & 0xFF;

  // MRP Protocol Version (IEEE 802.1Q-2018: Protocol Version 0)
  msg.protocol_version = 0;

  // MVRP VID Attribute Type
  msg.attribute_type = MVRP_ATTRIBUTE_TYPE_VID;
  msg.attribute_length = 2; // VID is 2 bytes (IEEE 802.1Q-2018 Table 10-8)

  // Attribute list length: leave_all(2) + first_value(2) + events(1) = 5 bytes

  // Attribute Vector
  msg.attribute_list[0].leave_all_event = 0; // No LeaveAll event
  msg.attribute_list[0].number_of_values = htons(1); // One VID in this vector
  msg.attribute_list[0].first_value = htons(vlan_id);
  msg.attribute_list[0].attribute_events = 0x6c; // JoinIn event

  // End marks (IEEE 802.1Q-2018 Section 10.8.2.9)
  msg.end_mark_list = 0;
  msg.end_mark = 0;

  ESP_LOGI(TAG, "Sending MVRP VLAN Join for VID %u", vlan_id);
  ESP_LOG_BUFFER_HEX_LEVEL(TAG, (uint8_t*)&msg, sizeof(msg), ESP_LOG_INFO);

  const ssize_t written = write(state->mvrp_socket, &msg, sizeof(msg));
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send MVRP VLAN Join: %d (errno: %d)", written, errno);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "MVRP VLAN Join sent successfully (%d bytes)", written);
  return ESP_OK;
}

void read_mvrp_net(const struct avtp_state_s* state)
{
  union
  {
    struct msrp_header_s header;
    u8 raw[129];
  } buf;

  const ssize_t len = read(state->mvrp_socket, &buf, sizeof(buf));
  if (len > 0)
  {
    ESP_LOGI(TAG, "MVRP read %d bytes", len);
  }
}

int mvrp_init(const char* interface)
{
  // Initialize MVRP socket
  int socket = open("/dev/net/tap", 0);
  if (socket < 0)
  {
    ESP_LOGE(TAG, "Failed to create MVRP socket");
    return ESP_FAIL;
  }

  const int ioctl_err = ioctl(socket, L2TAP_S_INTF_DEVICE, interface);
  if (ioctl_err < 0)
  {
    ESP_LOGE(TAG, "failed to set network interface %s at MVRP socket: %d\n", interface, ioctl_err);
    close(socket);
    return ESP_FAIL;
  }

  uint16_t eth_type_filter = ETH_TYPE_MVRP;
  if (ioctl(socket, L2TAP_S_RCV_FILTER, &eth_type_filter) < 0)
  {
    ESP_LOGE(TAG, "failed to set MVRP Ethertype filter: %d\n", errno);
    close(socket);
    return ESP_FAIL;
  }

  return socket;
}
