//
// Created by max on 11/27/25.
//
// MVRP - Multiple VLAN Registration Protocol Implementation
// IEEE 802.1Q-2022 Section 11.2
//

#include "mrp.h"
#include "mvrp.h"
#include "avtp.h"
#include "types.h"

#include "esp_vfs_l2tap.h"
#include "esp_eth_driver.h"

#include <esp_err.h>
#include <esp_log.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <esp_timer.h>

#define TAG "mvrp"

void mvrp_net_rx(struct avtp_state_s* state)
{
  u8 buf[128];

  ssize_t len = read(state->mvrp_socket, buf, sizeof(buf));
  if (len <= 0)
  {
    return;
  }

  ESP_LOGD(TAG, "MVRP received %d bytes", (int)len);

  /* Minimum MVRP message: header(14) + version(1) + type(1) + length(1) + vector(5) + endmarks(4) = 26 */
  if (len < 26)
  {
    ESP_LOGW(TAG, "MVRP message too short");
    return;
  }

  /* Check attribute type */
  u8 attr_type = buf[15];
  if (attr_type == MVRP_ATTRIBUTE_TYPE_VID)
  {
  }
  else
  {
    ESP_LOGW(TAG, "Unknown attribute type: %u", attr_type);
  }
}

void mvrp_mad_join_indication(struct mrp_application* app, struct mrp_attribute* attr, bool new)
{
  ESP_LOGW(TAG, "MVRP MAD Join Indication received. (Not implemented)");
}

void mvrp_mad_leave_indication(struct mrp_application* app, struct mrp_attribute* attr)
{
  ESP_LOGW(TAG, "MVRP MAD Leave Indication received. (Not implemented)");
}

void mvrp_tx_mrpdu(struct mrp_application* app, u8* buf, size_t len)
{
  mvrp_ctx_t* ctx = app->ctx;
  struct avtp_state_s* avtp_state = ctx->state;

  if (len < 64)
  {
    // MSRP frames must be at least 64 bytes (including FCS)
    size_t padding = 64 - len;
    memset(buf + len, 0, padding);
    len += padding;
  }

  int result = write(avtp_state->mvrp_socket, buf, len);
  if (result < 0)
  {
    ESP_LOGE(TAG, "Failed to send MRPDU: %s (%d)", strerror(errno), errno);
  }
  else
  {
    ESP_LOGI(TAG, "MRPDU sent successfully, %d bytes", result);
  }
}

ssize_t mvrp_set_attribute_event(struct mrp_application* app,
                                 struct mrp_attribute* attr,
                                 u8 event,
                                 u8* buf)
{
  // write three packed attribute event to buffer
  buf[0] = mrp_encode_three_packed_event(event, 0, 0);

  return sizeof(u8);
}


u8 mvrp_get_attribute_length(u8 attribute_type)
{
  return MVRP_ATTRIBUTE_LENGTH_VID;
}

u8 mvrp_get_attribute_value_length(u8 attribute_type)
{
  return sizeof(mvrp_attr_value_t);
}

/* ============================================================================
 * MVRP Public API
 * ============================================================================
 */

void mvrp_init(struct avtp_state_s* avtp_state)
{
  mvrp_ctx_t* mvrp = &avtp_state->mvrp;

  /* Initialize the MRP attribute structure */
  memset(mvrp, 0, sizeof(mvrp_ctx_t));

  mvrp->app.min_attribute_type = MVRP_ATTRIBUTE_TYPE_VID;
  mvrp->app.max_attribute_type = MVRP_ATTRIBUTE_TYPE_VID;
  mrp_init(&mvrp->app, MVRP);
  mvrp->app.mad_join_indication = &mvrp_mad_join_indication;
  mvrp->app.mad_leave_indication = &mvrp_mad_leave_indication;
  mvrp->app.get_attribute_value_length = &mvrp_get_attribute_value_length;
  mvrp->app.get_attribute_length = &mvrp_get_attribute_length;
  mvrp->app.set_attribute_event = &mvrp_set_attribute_event;
  mvrp->app.uses_attribute_list_length = false;
  mvrp->app.eth_type = ETH_TYPE_MVRP;
  mvrp->app.tx_mrpdu = &mvrp_tx_mrpdu;
  mvrp->app.participant_type = FULL_P2P;
  mvrp->app.ctx = mvrp;
  memcpy(mvrp->app.src_mac, avtp_state->intf_hw_addr, ETH_ADDR_LEN);

  struct Node* head = calloc(1, sizeof(struct Node));
  head->next = head;
  head->prev = head;
  mvrp->vlans = head;
}


int mvrp_init_socket(const char* interface)
{
  /* Initialize MVRP socket */
  int sock = open("/dev/net/tap", 0);
  if (sock < 0)
  {
    ESP_LOGE(TAG, "Failed to create MVRP socket");
    return -1;
  }

  int ioctl_err = ioctl(sock, L2TAP_S_INTF_DEVICE, interface);
  if (ioctl_err < 0)
  {
    ESP_LOGE(TAG, "Failed to set network interface %s at MVRP socket: %d", interface, ioctl_err);
    close(sock);
    return -1;
  }

  u16 eth_type_filter = ETH_TYPE_MVRP;
  if (ioctl(sock, L2TAP_S_RCV_FILTER, &eth_type_filter) < 0)
  {
    ESP_LOGE(TAG, "Failed to set MVRP Ethertype filter: %d", errno);
    close(sock);
    return -1;
  }

  ESP_LOGI(TAG, "MVRP socket initialized on %s", interface);
  return sock;
}
