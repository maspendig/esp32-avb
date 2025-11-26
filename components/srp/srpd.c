//
// Created by max on 10/29/25.
//

#include "srpd.h"
#include "msrp.h"
#include <errno.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_l2tap.h"
#include "esp_eth_driver.h"

#include <fcntl.h>

#include "esp_eth_spec.h"
#include "pthread.h"
#include "sys/ioctl.h"
#include "sys/select.h"

static const char* TAG = "srpd";

struct srp_state_s
{
  bool stop;
  int msrp_socket;
  int mvrp_socket;
};

static struct srp_state_s* s_state;

static int srp_init_state(struct srp_state_s* state, const char* interface)
{
  // Initialize MSRP socket
  state->msrp_socket = open("/dev/net/tap", 0);
  if (state->msrp_socket < 0)
  {
    ESP_LOGE(TAG, "Failed to create MSRP socket");
    return ESP_FAIL;
  }

  int ioctl_err = ioctl(state->msrp_socket, L2TAP_S_INTF_DEVICE, interface);
  if (ioctl_err < 0)
  {
    ESP_LOGE(TAG, "failed to set network interface %s at MSRP socket: %d\n", interface, ioctl_err);
    close(state->msrp_socket);
    return ESP_FAIL;
  }

  uint16_t eth_type_filter = ETH_TYPE_MSRP;
  if (ioctl(state->msrp_socket, L2TAP_S_RCV_FILTER, &eth_type_filter) < 0)
  {
    ESP_LOGE(TAG, "failed to set MSRP Ethertype filter: %d\n", errno);
    close(state->msrp_socket);
    return ESP_FAIL;
  }

  // Initialize MVRP socket
  state->mvrp_socket = open("/dev/net/tap", 0);
  if (state->mvrp_socket < 0)
  {
    ESP_LOGE(TAG, "Failed to create MVRP socket");
    close(state->msrp_socket);
    return ESP_FAIL;
  }

  ioctl_err = ioctl(state->mvrp_socket, L2TAP_S_INTF_DEVICE, interface);
  if (ioctl_err < 0)
  {
    ESP_LOGE(TAG, "failed to set network interface %s at MVRP socket: %d\n", interface, ioctl_err);
    close(state->msrp_socket);
    close(state->mvrp_socket);
    return ESP_FAIL;
  }

  eth_type_filter = ETH_TYPE_MVRP;
  if (ioctl(state->mvrp_socket, L2TAP_S_RCV_FILTER, &eth_type_filter) < 0)
  {
    ESP_LOGE(TAG, "failed to set MVRP Ethertype filter: %d\n", errno);
    close(state->msrp_socket);
    close(state->mvrp_socket);
    return ESP_FAIL;
  }

  s_state = state;
  return ESP_OK;
}


static int srp_net_recv(int socket, void* msg, uint16_t ptp_msg_len)
{
  uint8_t eth_frame[ptp_msg_len + ETH_HEADER_LEN];

  l2tap_extended_buff_t mrp_msg_ext_buff;

  mrp_msg_ext_buff.buff = eth_frame;
  mrp_msg_ext_buff.buff_len = sizeof(eth_frame);

  int ret = read(socket, &mrp_msg_ext_buff, 0);

  memcpy(msg, &eth_frame[ETH_HEADER_LEN], ret);

  return ret;
}

static void srp_daemon(void* task_param)
{
  const char* interface = "ETH_0";
  uint8_t buf[1600];
  struct srp_state_s* state = calloc(1, sizeof(struct srp_state_s));

  // override interface if provided
  if (task_param != NULL)
  {
    interface = task_param;
  }

  if (srp_init_state(state, interface) != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to initialize SRP state, exiting\n");
    // mrp_destroy_state(state);
    free(state);
    // errorhandling
  }

  // Determine the maximum file descriptor for select()
  int max_fd = (state->msrp_socket > state->mvrp_socket) ? state->msrp_socket : state->mvrp_socket;

  while (!state->stop)
  {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(state->msrp_socket, &read_fds);
    FD_SET(state->mvrp_socket, &read_fds);

    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    int activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);

    if (activity < 0)
    {
      ESP_LOGE(TAG, "Select error: %s", strerror(errno));
      break;
    }
    else if (activity == 0)
    {
      // Timeout, continue loop
      continue;
    }

    // Check MSRP socket
    if (FD_ISSET(state->msrp_socket, &read_fds))
    {
      const ssize_t len = read(state->msrp_socket, buf, sizeof(buf));
      if (len > 0)
      {
        msrp_net_rx();
      }
      else if (len < 0)
      {
        ESP_LOGE(TAG, "MSRP read error: %s", strerror(errno));
      }
    }

    // Check MVRP socket
    if (FD_ISSET(state->mvrp_socket, &read_fds))
    {
      const ssize_t len = read(state->mvrp_socket, buf, sizeof(buf));
      if (len > 0)
      {
        ESP_LOGI(TAG, "MVRP package received (%zd bytes)", len);
      }
      else if (len < 0)
      {
        ESP_LOGE(TAG, "MVRP read error: %s", strerror(errno));
      }
    }
  }
  //srp_destroy_state(state);
  close(state->msrp_socket);
  close(state->mvrp_socket);
  free(state);
}

int srpd_start(const char* interface)
{
  if (s_state == NULL)
  {
    xTaskCreate(srp_daemon, "SRPD", 4096,
                (void*)interface, tskIDLE_PRIORITY + 1, nullptr);
    return ESP_OK;
  }
  ESP_LOGE(TAG, "Other instance of MRP is already running");
  return ESP_FAIL;
}

int srpd_stop(int pid)
{
  return ESP_OK;
}

int srpd_status(int pid)
{
  return ESP_OK;
}
