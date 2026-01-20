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

/* ============================================================================
 * MVRP Message Structures
 * ============================================================================
 */

struct mvrp_msg_s
{
  struct header_s header;
  u8 protocol_version;
  u8 attribute_type;
  u8 attribute_length;
  u16 vector_header; /* LeaveAll event (3 bits) + NumberOfValues (13 bits) */
  u16 first_value; /* First VID in vector */
  u8 three_packed_event; /* Three-packed event encoding */
  u16 end_mark_list;
  u16 end_mark;
  u8 padding[38]; /* Padding to reach 64 bytes minimum */
} __attribute__((packed));

/* ============================================================================
 * Timer Helper Functions
 * ============================================================================
 */

static void mvrp_periodic_timer_cb(void* arg)
{
  mvrp_state_t* state = (mvrp_state_t*)arg;
  state->periodic_pending = true;
}

static void timer_reset(struct timespec* timer)
{
  clock_gettime(CLOCK_MONOTONIC, timer);
}

static bool timer_expired(const struct timespec* timer, u32 timeout_ms)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  u64 timer_ms = (u64)timer->tv_sec * 1000 + timer->tv_nsec / 1000000;
  u64 now_ms = (u64)now.tv_sec * 1000 + now.tv_nsec / 1000000;

  return (now_ms - timer_ms) >= timeout_ms;
}

/* ============================================================================
 * Debug Helper Functions
 * ============================================================================
 */

static const char* applicant_state_string(mvrp_applicant_state_t state)
{
  switch (state)
  {
  case MVRP_APPLICANT_VO: return "VO";
  case MVRP_APPLICANT_VP: return "VP";
  case MVRP_APPLICANT_VN: return "VN";
  case MVRP_APPLICANT_AN: return "AN";
  case MVRP_APPLICANT_AA: return "AA";
  case MVRP_APPLICANT_QA: return "QA";
  case MVRP_APPLICANT_LA: return "LA";
  case MVRP_APPLICANT_AO: return "AO";
  case MVRP_APPLICANT_QO: return "QO";
  case MVRP_APPLICANT_AP: return "AP";
  case MVRP_APPLICANT_QP: return "QP";
  case MVRP_APPLICANT_LO: return "LO";
  default: return "??";
  }
}

static const char* registrar_state_string(mvrp_registrar_state_t state)
{
  switch (state)
  {
  case MVRP_REGISTRAR_MT: return "MT";
  case MVRP_REGISTRAR_IN: return "IN";
  case MVRP_REGISTRAR_LV: return "LV";
  default: return "??";
  }
}

static const char* event_string(u8 event)
{
  switch (event)
  {
  case MRP_EVENT_NEW: return "New";
  case MRP_EVENT_R_JOIN_IN: return "JoinIn";
  case MRP_EVENT_R_IN: return "In";
  case MRP_EVENT_R_JOIN_MT: return "JoinMt";
  case MRP_EVENT_R_MT: return "Mt";
  case MRP_EVENT_LV: return "Leave";
  default: return "??";
  }
}

/* ============================================================================
 * MRP Applicant State Machine - IEEE 802.1Q-2022 Section 10.7.4
 * ============================================================================
 */

/**
 * Process applicant state machine on Join! event (local application wants to join)
 */
static void applicant_join(mvrp_vlan_decl_t* decl)
{
  mvrp_applicant_state_t old_state = decl->applicant_state;

  switch (decl->applicant_state)
  {
  case MVRP_APPLICANT_VO:
  case MVRP_APPLICANT_LO:
    decl->applicant_state = MVRP_APPLICANT_VN;
    decl->tx_pending = true;
    break;
  case MVRP_APPLICANT_LA:
    decl->applicant_state = MVRP_APPLICANT_AA;
    break;
  case MVRP_APPLICANT_AO:
    decl->applicant_state = MVRP_APPLICANT_AP;
    break;
  case MVRP_APPLICANT_QO:
    decl->applicant_state = MVRP_APPLICANT_QP;
    break;
  default:
    /* Already in a joined state, no change */
    break;
  }

  if (old_state != decl->applicant_state)
  {
    ESP_LOGI(TAG, "Applicant Join!: %s -> %s",
             applicant_state_string(old_state),
             applicant_state_string(decl->applicant_state));
  }
}

/**
 * Process applicant state machine on Leave! event (local application wants to leave)
 */
static void applicant_leave(mvrp_vlan_decl_t* decl)
{
  mvrp_applicant_state_t old_state = decl->applicant_state;

  switch (decl->applicant_state)
  {
  case MVRP_APPLICANT_VN:
    decl->applicant_state = MVRP_APPLICANT_LA;
    break;
  case MVRP_APPLICANT_AN:
  case MVRP_APPLICANT_AA:
  case MVRP_APPLICANT_QA:
    decl->applicant_state = MVRP_APPLICANT_LA;
    break;
  case MVRP_APPLICANT_VP:
  case MVRP_APPLICANT_AP:
  case MVRP_APPLICANT_QP:
    decl->applicant_state = MVRP_APPLICANT_VO;
    break;
  default:
    break;
  }

  if (old_state != decl->applicant_state)
  {
    ESP_LOGI(TAG, "Applicant Leave!: %s -> %s",
             applicant_state_string(old_state),
             applicant_state_string(decl->applicant_state));
  }
}

/**
 * Process applicant state machine when receiving rJoinIn event
 */
static void applicant_rx_joinin(mvrp_vlan_decl_t* decl)
{
  mvrp_applicant_state_t old_state = decl->applicant_state;

  switch (decl->applicant_state)
  {
  case MVRP_APPLICANT_VO:
    decl->applicant_state = MVRP_APPLICANT_AO;
    break;
  case MVRP_APPLICANT_VP:
    decl->applicant_state = MVRP_APPLICANT_AP;
    break;
  case MVRP_APPLICANT_AA:
    decl->applicant_state = MVRP_APPLICANT_QA;
    break;
  case MVRP_APPLICANT_AO:
    decl->applicant_state = MVRP_APPLICANT_QO;
    break;
  case MVRP_APPLICANT_AP:
    decl->applicant_state = MVRP_APPLICANT_QP;
    break;
  default:
    break;
  }

  if (old_state != decl->applicant_state)
  {
    ESP_LOGD(TAG, "Applicant rJoinIn: %s -> %s",
             applicant_state_string(old_state),
             applicant_state_string(decl->applicant_state));
  }
}

/**
 * Process applicant state machine when receiving rIn event
 */
static void applicant_rx_in(mvrp_vlan_decl_t* decl)
{
  mvrp_applicant_state_t old_state = decl->applicant_state;

  switch (decl->applicant_state)
  {
  case MVRP_APPLICANT_AA:
    decl->applicant_state = MVRP_APPLICANT_QA;
    break;
  default:
    break;
  }

  if (old_state != decl->applicant_state)
  {
    ESP_LOGD(TAG, "Applicant rIn: %s -> %s",
             applicant_state_string(old_state),
             applicant_state_string(decl->applicant_state));
  }
}

/**
 * Process applicant state machine when receiving rJoinMt or rMt event
 */
static void applicant_rx_empty(mvrp_vlan_decl_t* decl)
{
  mvrp_applicant_state_t old_state = decl->applicant_state;

  switch (decl->applicant_state)
  {
  case MVRP_APPLICANT_QA:
    decl->applicant_state = MVRP_APPLICANT_AA;
    decl->tx_pending = true;
    break;
  case MVRP_APPLICANT_QO:
    decl->applicant_state = MVRP_APPLICANT_AO;
    break;
  case MVRP_APPLICANT_QP:
    decl->applicant_state = MVRP_APPLICANT_AP;
    break;
  default:
    break;
  }

  if (old_state != decl->applicant_state)
  {
    ESP_LOGD(TAG, "Applicant rEmpty: %s -> %s",
             applicant_state_string(old_state),
             applicant_state_string(decl->applicant_state));
  }
}

/**
 * Process applicant state machine when receiving rLeave event
 */
static void applicant_rx_leave(mvrp_vlan_decl_t* decl)
{
  mvrp_applicant_state_t old_state = decl->applicant_state;

  switch (decl->applicant_state)
  {
  case MVRP_APPLICANT_VO:
  case MVRP_APPLICANT_AO:
  case MVRP_APPLICANT_QO:
    decl->applicant_state = MVRP_APPLICANT_LO;
    break;
  case MVRP_APPLICANT_VP:
  case MVRP_APPLICANT_AP:
  case MVRP_APPLICANT_QP:
    decl->applicant_state = MVRP_APPLICANT_VO;
    break;
  default:
    break;
  }

  if (old_state != decl->applicant_state)
  {
    ESP_LOGD(TAG, "Applicant rLeave: %s -> %s",
             applicant_state_string(old_state),
             applicant_state_string(decl->applicant_state));
  }
}

/**
 * Process applicant state machine on periodic transmission event
 */
static void applicant_periodic(mvrp_vlan_decl_t* decl)
{
  mvrp_applicant_state_t old_state = decl->applicant_state;

  switch (decl->applicant_state)
  {
  case MVRP_APPLICANT_QA:
    decl->applicant_state = MVRP_APPLICANT_AA;
    decl->tx_pending = true;
    break;
  case MVRP_APPLICANT_QP:
    decl->applicant_state = MVRP_APPLICANT_AP;
    decl->tx_pending = true;
    break;
  default:
    break;
  }

  if (old_state != decl->applicant_state)
  {
    ESP_LOGD(TAG, "Applicant Periodic: %s -> %s",
             applicant_state_string(old_state),
             applicant_state_string(decl->applicant_state));
  }
}

/**
 * Process applicant state machine on tx opportunity
 * Returns the event to transmit, or -1 if no transmission needed
 */
static int applicant_tx(mvrp_vlan_decl_t* decl)
{
  mvrp_applicant_state_t old_state = decl->applicant_state;
  int tx_event = -1;

  switch (decl->applicant_state)
  {
  case MVRP_APPLICANT_VN:
    /* VN + tx! -> AN, send New */
    decl->applicant_state = MVRP_APPLICANT_AN;
    tx_event = MRP_EVENT_NEW;
    break;
  case MVRP_APPLICANT_AN:
    /* AN + tx! -> QA, send New */
    decl->applicant_state = MVRP_APPLICANT_QA;
    tx_event = MRP_EVENT_NEW;
    break;
  case MVRP_APPLICANT_AA:
    /* AA + tx! -> QA, send JoinIn */
    decl->applicant_state = MVRP_APPLICANT_QA;
    tx_event = MRP_EVENT_R_JOIN_IN;
    break;
  case MVRP_APPLICANT_LA:
    /* LA + tx! -> VO, send Leave */
    decl->applicant_state = MVRP_APPLICANT_VO;
    tx_event = MRP_EVENT_LV;
    break;
  case MVRP_APPLICANT_VP:
    /* VP + tx! -> AA, send JoinIn */
    decl->applicant_state = MVRP_APPLICANT_AA;
    tx_event = MRP_EVENT_R_JOIN_IN;
    break;
  case MVRP_APPLICANT_AP:
    /* AP + tx! -> QA, send JoinIn */
    decl->applicant_state = MVRP_APPLICANT_QA;
    tx_event = MRP_EVENT_R_JOIN_IN;
    break;
  case MVRP_APPLICANT_AO:
    /* AO + tx! -> QO, send In */
    decl->applicant_state = MVRP_APPLICANT_QO;
    tx_event = MRP_EVENT_R_IN;
    break;
  case MVRP_APPLICANT_LO:
    /* LO + tx! -> VO, send empty (no tx) */
    decl->applicant_state = MVRP_APPLICANT_VO;
    tx_event = -1;
    break;
  default:
    /* No transmission needed for QA, QO, QP, VO states */
    break;
  }

  decl->tx_pending = false;

  if (old_state != decl->applicant_state)
  {
    ESP_LOGD(TAG, "Applicant tx!: %s -> %s, event=%s",
             applicant_state_string(old_state),
             applicant_state_string(decl->applicant_state),
             tx_event >= 0 ? event_string(tx_event) : "none");
  }

  return tx_event;
}

/**
 * Check if applicant is in an "anxious" state that requires periodic transmission
 */
static bool applicant_is_anxious(mvrp_applicant_state_t state)
{
  switch (state)
  {
  case MVRP_APPLICANT_VN:
  case MVRP_APPLICANT_AN:
  case MVRP_APPLICANT_AA:
  case MVRP_APPLICANT_LA:
  case MVRP_APPLICANT_VP:
  case MVRP_APPLICANT_AP:
  case MVRP_APPLICANT_AO:
  case MVRP_APPLICANT_LO:
    return true;
  default:
    return false;
  }
}

/* ============================================================================
 * MRP Registrar State Machine - IEEE 802.1Q-2022 Section 10.7.5
 * ============================================================================
 */

/**
 * Process registrar state machine on rNew, rJoinIn, or rJoinMt event
 */
static void registrar_rx_join(mvrp_vlan_reg_t* reg)
{
  mvrp_registrar_state_t old_state = reg->registrar_state;

  switch (reg->registrar_state)
  {
  case MVRP_REGISTRAR_MT:
  case MVRP_REGISTRAR_LV:
    reg->registrar_state = MVRP_REGISTRAR_IN;
    ESP_LOGI(TAG, "Registrar: VLAN %u now IN (registered)",
             reg->vlan_id);
    break;
  case MVRP_REGISTRAR_IN:
    /* Already registered, refresh */
    break;
  }

  if (old_state != reg->registrar_state)
  {
    ESP_LOGD(TAG, "Registrar Join: %s -> %s",
             registrar_state_string(old_state),
             registrar_state_string(reg->registrar_state));
  }
}

/**
 * Process registrar state machine on rLeave event
 */
static void registrar_rx_leave(mvrp_vlan_reg_t* reg)
{
  mvrp_registrar_state_t old_state = reg->registrar_state;

  switch (reg->registrar_state)
  {
  case MVRP_REGISTRAR_IN:
    reg->registrar_state = MVRP_REGISTRAR_LV;
    timer_reset(&reg->leave_timer);
    break;
  default:
    break;
  }

  if (old_state != reg->registrar_state)
  {
    ESP_LOGD(TAG, "Registrar Leave: %s -> %s",
             registrar_state_string(old_state),
             registrar_state_string(reg->registrar_state));
  }
}

/**
 * Process registrar leave timer expiry
 */
static void registrar_leave_timer_expired(mvrp_vlan_reg_t* reg)
{
  if (reg->registrar_state == MVRP_REGISTRAR_LV)
  {
    reg->registrar_state = MVRP_REGISTRAR_MT;
    reg->valid = false;
    ESP_LOGI(TAG, "Registrar: VLAN %u leave timer expired, now MT",
             reg->vlan_id);
  }
}

/* ============================================================================
 * MVRP Message Transmission
 * ============================================================================
 */

/**
 * Send an MVRP VID declaration message
 */
static int mvrp_send_vid_declaration(struct avtp_state_s* state, u16 vlan_id, u8 event)
{
  struct mvrp_msg_s msg = {0};

  /* MVRP multicast destination MAC */
  u8* mvrp_multicast_mac = MVRP_MULTICAST_MAC;
  memcpy(msg.header.dst_mac, mvrp_multicast_mac, sizeof(msg.header.dst_mac));
  memcpy(msg.header.src_mac, state->intf_hw_addr, sizeof(msg.header.src_mac));

  /* Ethernet type */
  msg.header.eth_type[0] = (ETH_TYPE_MVRP >> 8) & 0xFF;
  msg.header.eth_type[1] = ETH_TYPE_MVRP & 0xFF;

  /* MRP Protocol Version */
  msg.protocol_version = 0;

  /* MVRP VID Attribute Type */
  msg.attribute_type = MVRP_ATTRIBUTE_TYPE_VID;
  msg.attribute_length = MVRP_ATTRIBUTE_LENGTH_VID;

  /* Vector Header: LeaveAll (3 bits) + NumberOfValues (13 bits) */
  /* NumberOfValues = 1, LeaveAll = 0 */
  msg.vector_header = htons(1);

  /* First Value (VID) */
  msg.first_value = htons(vlan_id);

  /* Three-packed event encoding: one event uses 5 bits, encoded as 6^0 * event */
  /* For single event: event * 36 + 0 * 6 + 0 = event * 36 */
  msg.three_packed_event = event * 36;

  /* End marks */
  msg.end_mark_list = 0;
  msg.end_mark = 0;

  ESP_LOGI(TAG, "Sending MVRP VID %u with event %s", vlan_id, event_string(event));

  ssize_t written = write(state->mvrp_socket, &msg, sizeof(msg));
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send MVRP message: %d (errno: %d)", (int)written, errno);
    return -1;
  }

  ESP_LOGD(TAG, "MVRP message sent (%d bytes)", (int)written);
  return 0;
}

/* ============================================================================
 * MVRP Message Reception and Processing
 * ============================================================================
 */

/**
 * Find or create a VLAN registration entry
 */
static mvrp_vlan_reg_t* find_or_create_vlan_reg(mvrp_state_t* mvrp, u16 vlan_id)
{
  /* First, try to find existing entry */
  for (int i = 0; i < MVRP_MAX_VLAN_REGISTRATIONS; i++)
  {
    if (mvrp->vlan_registrations[i].valid && mvrp->vlan_registrations[i].vlan_id == vlan_id)
    {
      return &mvrp->vlan_registrations[i];
    }
  }

  /* Not found, create new entry */
  for (int i = 0; i < MVRP_MAX_VLAN_REGISTRATIONS; i++)
  {
    if (!mvrp->vlan_registrations[i].valid)
    {
      mvrp->vlan_registrations[i].valid = true;
      mvrp->vlan_registrations[i].vlan_id = vlan_id;
      mvrp->vlan_registrations[i].registrar_state = MVRP_REGISTRAR_MT;
      return &mvrp->vlan_registrations[i];
    }
  }

  ESP_LOGW(TAG, "No space for new VLAN registration");
  return NULL;
}

/**
 * Handle received MVRP VID message
 */
static void handle_mvrp_vid_message(struct avtp_state_s* state, const u8* buf, size_t len)
{
  mvrp_state_t* mvrp = &state->mvrp_state;

  /* Parse the message - skip header (14) + version (1) + type (1) + length (1) */
  if (len < 22)
  {
    ESP_LOGW(TAG, "MVRP message too short: %zu bytes", len);
    return;
  }

  const u8* attr_data = buf + 17; /* After header + version + type + length */

  /* Parse vector header */
  u16 vector_header = ntohs(*(u16*)attr_data);
  u8 leave_all = (vector_header >> 13) & 0x07;
  u16 num_values = vector_header & 0x1FFF;

  attr_data += 2;

  if (leave_all)
  {
    ESP_LOGI(TAG, "Received MVRP LeaveAll");
    /* On LeaveAll, reset all registrations to LV state */
    for (int i = 0; i < MVRP_MAX_VLAN_REGISTRATIONS; i++)
    {
      if (mvrp->vlan_registrations[i].valid &&
        mvrp->vlan_registrations[i].registrar_state == MVRP_REGISTRAR_IN)
      {
        mvrp->vlan_registrations[i].registrar_state = MVRP_REGISTRAR_LV;
        timer_reset(&mvrp->vlan_registrations[i].leave_timer);
      }
    }
  }

  if (num_values == 0)
  {
    return;
  }

  /* Parse first value (VID) */
  u16 first_vid = ntohs(*(u16*)attr_data);
  attr_data += 2;

  /* Parse three-packed events */
  u8 packed_events = *attr_data;

  for (u16 i = 0; i < num_values && i < 3; i++)
  {
    u16 vid = first_vid + i;
    u8 event;

    /* Decode three-packed event */
    switch (i)
    {
    case 0:
      event = packed_events / 36;
      break;
    case 1:
      event = (packed_events / 6) % 6;
      break;
    case 2:
      event = packed_events % 6;
      break;
    default:
      continue;
    }

    ESP_LOGI(TAG, "Received MVRP VID %u, event %s", vid, event_string(event));

    /* Process registrar state machine for received VLAN */
    mvrp_vlan_reg_t* reg = find_or_create_vlan_reg(mvrp, vid);
    if (reg)
    {
      switch (event)
      {
      case MRP_EVENT_NEW:
      case MRP_EVENT_R_JOIN_IN:
      case MRP_EVENT_R_JOIN_MT:
        registrar_rx_join(reg);
        break;
      case MRP_EVENT_LV:
        registrar_rx_leave(reg);
        break;
      default:
        /* In, Mt - just refresh */
        break;
      }
    }

    /* Process applicant state machine if this is our declared VLAN */
    if (mvrp->vlan_decl.active && mvrp->vlan_decl.vlan_id == vid)
    {
      switch (event)
      {
      case MRP_EVENT_R_JOIN_IN:
        applicant_rx_joinin(&mvrp->vlan_decl);
        break;
      case MRP_EVENT_R_IN:
        applicant_rx_in(&mvrp->vlan_decl);
        break;
      case MRP_EVENT_R_JOIN_MT:
      case MRP_EVENT_R_MT:
        applicant_rx_empty(&mvrp->vlan_decl);
        break;
      case MRP_EVENT_LV:
        applicant_rx_leave(&mvrp->vlan_decl);
        break;
      default:
        break;
      }
    }
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

void mvrp_state_init(struct avtp_state_s* avtp_state)
{
  mvrp_state_t* state = &avtp_state->mvrp_state;
  memset(state, 0, sizeof(*state));

  mvrp_ctx_t* mvrp = &avtp_state->mvrp;

  /* Initialize the MRP attribute structure */
  memset(mvrp, 0, sizeof(mvrp_ctx_t));

  mrp_init(&mvrp->app, MVRP);
  mvrp->app.mad_join_indication = &mvrp_mad_join_indication;
  mvrp->app.mad_leave_indication = &mvrp_mad_leave_indication;
  mvrp->app.get_attribute_value_length = &mvrp_get_attribute_value_length;
  mvrp->app.get_attribute_length = &mvrp_get_attribute_length;
  mvrp->app.set_attribute_event = &mvrp_set_attribute_event;
  mvrp->app.uses_attribute_list_length = true;
  mvrp->app.tx_mrpdu = &mvrp_tx_mrpdu;
  mvrp->app.participant_type = FULL_P2P;
  mvrp->app.ctx = mvrp;
  memcpy(mvrp->app.src_mac, avtp_state->intf_hw_addr, ETH_ADDR_LEN);

  struct Node* head = calloc(1, sizeof(struct Node));
  head->next = head;
  head->prev = head;
  mvrp->vlans = head;

  /* Initialize timer values */
  state->join_timeout_ms = MVRP_JOIN_TIME_MS;
  state->leave_timeout_ms = MVRP_LEAVE_TIME_MS;
  state->leave_all_timeout_ms = MVRP_LEAVE_ALL_TIME_MS;

  /* Initialize applicant state to VO (observer) */
  state->vlan_decl.applicant_state = MVRP_APPLICANT_VO;

  /* Trigger Join! for default VLAN 2 to ensure we start sending declarations */
  // Fix for "no package send out"
  state->vlan_decl.active = true;
  state->vlan_decl.vlan_id = 2; // Default VLAN
  // applicant_join(&state->vlan_decl); // Cannot call static function easily if not forward declared, but it is above.
  // Actually recursive calls or static ordering matters. applicant_join is defined as static above.
  // But I am in mvrp_state_init which is below applicant_state_machine defs?
  // Let's check... mvrp_state_init is at line 688, applicant_join is static.
  // To avoid ordering issues, I'll essentially inline minimal join logic or just set state.
  state->vlan_decl.applicant_state = MVRP_APPLICANT_VN;
  state->vlan_decl.tx_pending = true;

  timer_reset(&state->last_tx_time);

  const esp_timer_create_args_t periodic_timer_args = {
    .callback = &mvrp_periodic_timer_cb,
    .arg = state,
    .name = "mvrp_periodic"
  };

  esp_timer_create(&periodic_timer_args, &state->periodic_timer);
  esp_timer_start_periodic(state->periodic_timer, MRP_PERIODIC_TIME_MS * 1000); // us
}

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
    handle_mvrp_vid_message(state, buf, len);
  }
  else
  {
    ESP_LOGW(TAG, "Unknown MVRP attribute type: %u", attr_type);
  }
}

void mvrp_periodic(struct avtp_state_s* state)
{
  mvrp_state_t* mvrp = &state->mvrp_state;

  /* Check registrar leave timers */
  for (int i = 0; i < MVRP_MAX_VLAN_REGISTRATIONS; i++)
  {
    if (mvrp->vlan_registrations[i].valid &&
      mvrp->vlan_registrations[i].registrar_state == MVRP_REGISTRAR_LV)
    {
      if (timer_expired(&mvrp->vlan_registrations[i].leave_timer, mvrp->leave_timeout_ms))
      {
        registrar_leave_timer_expired(&mvrp->vlan_registrations[i]);
      }
    }
  }

  /* Process periodic timer event */
  if (mvrp->periodic_pending)
  {
    mvrp->periodic_pending = false;
    if (mvrp->vlan_decl.active)
    {
      applicant_periodic(&mvrp->vlan_decl);
    }
  }

  /* Check if we need to transmit VLAN declaration */
  if (mvrp->vlan_decl.active)
  {
    bool should_tx = false;

    /* Transmit on timer if in anxious state */
    if (timer_expired(&mvrp->last_tx_time, mvrp->join_timeout_ms))
    {
      if (applicant_is_anxious(mvrp->vlan_decl.applicant_state))
      {
        should_tx = true;
      }
      timer_reset(&mvrp->last_tx_time);
    }

    /* Always transmit if tx_pending is set */
    if (mvrp->vlan_decl.tx_pending)
    {
      should_tx = true;
    }

    if (should_tx)
    {
      int tx_event = applicant_tx(&mvrp->vlan_decl);
      if (tx_event >= 0)
      {
        mvrp_send_vid_declaration(state, mvrp->vlan_decl.vlan_id, tx_event);
      }

      /* Check if we left the VLAN */
      if (mvrp->vlan_decl.applicant_state == MVRP_APPLICANT_VO &&
        !mvrp->vlan_decl.tx_pending)
      {
        ESP_LOGI(TAG, "VLAN %u declaration completed (left)", mvrp->vlan_decl.vlan_id);
        mvrp->vlan_decl.active = false;
      }
    }
  }
}

int mvrp_vlan_join(struct avtp_state_s* state, u16 vlan_id)
{
  mvrp_state_t* mvrp = &state->mvrp_state;

  ESP_LOGI(TAG, "Joining VLAN %u", vlan_id);

  /* Check if already active for this or another VLAN */
  if (mvrp->vlan_decl.active)
  {
    if (mvrp->vlan_decl.vlan_id == vlan_id)
    {
      ESP_LOGW(TAG, "Already declared VLAN %u", vlan_id);
      return 0;
    }
    else
    {
      ESP_LOGW(TAG, "Already have active VLAN %u, leaving first",
               mvrp->vlan_decl.vlan_id);
      mvrp_vlan_leave(state, mvrp->vlan_decl.vlan_id);
    }
  }

  /* Set up new declaration */
  mvrp->vlan_decl.active = true;
  mvrp->vlan_decl.vlan_id = vlan_id;

  /* Trigger Join! event on applicant state machine */
  applicant_join(&mvrp->vlan_decl);

  return 0;
}

int mvrp_vlan_leave(struct avtp_state_s* state, u16 vlan_id)
{
  mvrp_state_t* mvrp = &state->mvrp_state;

  if (!mvrp->vlan_decl.active || mvrp->vlan_decl.vlan_id != vlan_id)
  {
    ESP_LOGW(TAG, "Not declared for VLAN %u", vlan_id);
    return -1;
  }

  ESP_LOGI(TAG, "Leaving VLAN %u", vlan_id);

  /* Trigger Leave! event on applicant state machine */
  applicant_leave(&mvrp->vlan_decl);

  /* Send immediate Leave message */
  mvrp_send_vid_declaration(state, vlan_id, MRP_EVENT_LV);

  /* Mark as inactive after sending leave */
  mvrp->vlan_decl.active = false;

  return 0;
}

bool mvrp_vlan_is_registered(const struct avtp_state_s* state, u16 vlan_id)
{
  const mvrp_state_t* mvrp = &state->mvrp_state;

  for (int i = 0; i < MVRP_MAX_VLAN_REGISTRATIONS; i++)
  {
    if (mvrp->vlan_registrations[i].valid &&
      mvrp->vlan_registrations[i].vlan_id == vlan_id &&
      mvrp->vlan_registrations[i].registrar_state == MVRP_REGISTRAR_IN)
    {
      return true;
    }
  }

  return false;
}

int mvrp_init(const char* interface)
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
