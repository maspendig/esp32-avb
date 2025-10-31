//
// Created by max on 10/29/25.
//

#include "mrpd.h"

#include <errno.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_l2tap.h"
#include "esp_eth_driver.h"

#include <fcntl.h>

#include "esp_eth_spec.h"
#include "pthread.h"
#include "sys/ioctl.h"

#define ETH_TYPE_MSRP 0x22EA
#define ETH_TYPE_MVRP 0x88F5

static const char *TAG = "mrpd";

struct mrp_state_s
{
  bool stop;
  int socket;
};

static struct mrp_state_s *s_state;

static int mrp_init_state(struct mrp_state_s *state, const char *interface)
{
  state->socket = open("/dev/net/tap", 0);
  if (state->socket < 0)
  {
    ESP_LOGE(TAG, "Failed to create tx socket");
    return ESP_FAIL;
  }


  int ioctl_err = ioctl(state->socket, L2TAP_S_INTF_DEVICE, interface);
  // Set Ethernet interface on which to get raw frames
  if (ioctl_err < 0)
  {
    ESP_LOGE(TAG,"failed to set network interface %s at socket: %d\n", interface, ioctl_err);
    return ESP_FAIL;
  }

  // Set the Ethertype filter (frames with this type will be available through the state->tx_socket)
  uint16_t eth_type_filter = ETH_TYPE_MSRP;
  if (ioctl(state->socket, L2TAP_S_RCV_FILTER, &eth_type_filter) < 0)
  {
    ESP_LOGE(TAG,"failed to set Ethertype filter: %d\n", errno);
    return ESP_FAIL;
  }

  s_state = state;
  return ESP_OK;
}


static int mrp_net_recv(const struct mrp_state_s *state, void *mrp_msg, uint16_t ptp_msg_len)
{
  uint8_t eth_frame[ptp_msg_len + ETH_HEADER_LEN];

  l2tap_extended_buff_t mrp_msg_ext_buff;

  mrp_msg_ext_buff.buff = eth_frame;
  mrp_msg_ext_buff.buff_len = sizeof(eth_frame);

  int ret = read(state->socket, &mrp_msg_ext_buff, 0);

  memcpy(mrp_msg, &eth_frame[ETH_HEADER_LEN], ret);

  return ret;
}

static void mrp_daemon(void *task_param)
{
  const char *interface = "ETH_0";
  int ret;
  uint8_t buf[1600];
  struct mrp_state_s* state = calloc(1, sizeof(struct mrp_state_s));

  // override interface if provided
  if (task_param != NULL)
  {
    interface = task_param;
  }

  if (mrp_init_state(state, interface) != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to initialize MRP state, exiting\n");
    // mrp_destroy_state(state);
    free(state);
    // errorhandling
  }
  while (!state->stop)
  {
    // ret = mrp_net_recv(state, &buf, sizeof(buf));
    // if (ret > 0)
    // {
    //   ESP_LOGI(TAG, "received mrp packet");
    // }
    const ssize_t len = read(state->socket, buf, sizeof(buf));
    if (len > 0) {
      // EtherType is at offset 12 in Ethernet header
      uint16_t ethertype = (buf[12] << 8) | buf[13];

      switch (ethertype)
      {
        case ETH_TYPE_MSRP:
          ESP_LOGI(TAG, "MSRP Package received");
          uint8_t proto_version = buf[14];
          ESP_LOGI(TAG, "Protocol version: %d", proto_version);
          uint16_t read_marker = 15;
          while (read_marker <= len)
          {
            if (buf[read_marker] == 0 && buf[read_marker + 1] == 0)
            {
              break;
            }
            uint8_t attribute_type = buf[read_marker];
            uint8_t attribute_len = buf[read_marker + 1];
            uint8_t attribute_list_len = (uint16_t)((buf[read_marker + 2] << 8) | buf[read_marker + 3]);

            if (attribute_type == 4)
            {
              for (int domain_marker = 0; domain_marker < attribute_list_len-2; domain_marker+=7)
              {
                int pos = read_marker + 6 + domain_marker;
                int sr_class_id = buf[pos];
                int prio = buf[pos + 1];
                int vlan_id = (buf[pos + 2] << 8) | buf[pos + 3];
                if (sr_class_id == 6)
                {
                  ESP_LOGI(TAG, "detected domain Class A PRIO=%d, VID=%d", prio, vlan_id);
                }
              }
            }

            ESP_LOGI(TAG, "Attribute type: %d, read_marker: %d", attribute_type, read_marker);
            read_marker += (4 + attribute_list_len);
          }
          break;
        case ETH_TYPE_MVRP:
          ESP_LOGI(TAG, "MVRP Package received");
          break;
        default:
          ESP_LOGW(TAG, "Unknown EtherType 0x%04X received", ethertype);
          break;
      }

    } else {
      ESP_LOGE(TAG, "Read error: %s", strerror(errno));
      break;
    }
  }
  //mrp_destroy_state(state);
  free(state);
}

int mrpd_start(const char *interface)
{
  if (s_state == NULL)
  {
    xTaskCreate(mrp_daemon, "MRPD", 4096,
        (void *)interface, tskIDLE_PRIORITY + 1, nullptr);
    return ESP_OK;
  }
  ESP_LOGE(TAG, "Other instance of MRP is already running");
  return ESP_FAIL;

}

int mrpd_stop(int pid)
{
  return ESP_OK;
}

int mrpd_status(int pid)
{
    return ESP_OK;
}
