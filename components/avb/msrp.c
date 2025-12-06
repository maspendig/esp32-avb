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

/* Default MRP timer values per IEEE 802.1Q */
#define MRP_JOIN_TIME_MS      200
#define MRP_LEAVE_TIME_MS     600
#define MRP_LEAVE_ALL_TIME_MS 10000

/* ============================================================================
 * Helper Functions
 * ============================================================================
 */

static const char* msrp_attribute_event_string(int s)
{
  switch (s)
  {
  case MSRP_ATTRIBUTE_EVENT_NEW:
    return "NEW";
  case MSRP_ATTRIBUTE_EVENT_JOININ:
    return "JOININ";
  case MSRP_ATTRIBUTE_EVENT_IN:
    return "IN";
  case MSRP_ATTRIBUTE_EVENT_JOINMT:
    return "JOINMT";
  case MSRP_ATTRIBUTE_EVENT_MT:
    return "MT";
  case MSRP_ATTRIBUTE_EVENT_LV:
    return "LV";
  default:
    return "??";
  }
}

static const char* msrp_listener_decl_string(int d)
{
  switch (d)
  {
  case MSRP_LISTENER_IGNORE:
    return "IGNORE";
  case MSRP_LISTENER_ASKING_FAILED:
    return "ASKING_FAILED";
  case MSRP_LISTENER_READY:
    return "READY";
  case MSRP_LISTENER_READY_FAILED:
    return "READY_FAILED";
  default:
    return "??";
  }
}

static const char* mrp_applicant_state_string(mrp_applicant_state_t s)
{
  switch (s)
  {
  case MRP_APPLICANT_VO: return "VO";
  case MRP_APPLICANT_VP: return "VP";
  case MRP_APPLICANT_VN: return "VN";
  case MRP_APPLICANT_AN: return "AN";
  case MRP_APPLICANT_AA: return "AA";
  case MRP_APPLICANT_QA: return "QA";
  case MRP_APPLICANT_LA: return "LA";
  case MRP_APPLICANT_AO: return "AO";
  case MRP_APPLICANT_QO: return "QO";
  case MRP_APPLICANT_AP: return "AP";
  case MRP_APPLICANT_QP: return "QP";
  case MRP_APPLICANT_LO: return "LO";
  default: return "??";
  }
}

static const char* mrp_registrar_state_string(mrp_registrar_state_t s)
{
  switch (s)
  {
  case MRP_REGISTRAR_MT: return "MT";
  case MRP_REGISTRAR_IN: return "IN";
  case MRP_REGISTRAR_LV: return "LV";
  default: return "??";
  }
}

/**
 * Decode three-packed event value to get individual event
 * Per IEEE 802.1Q, events are packed as: a*36 + b*6 + c
 */
static u8 decode_three_packed_event(u8 packed, int index)
{
  switch (index)
  {
  case 0: return packed / 36;
  case 1: return (packed % 36) / 6;
  case 2: return packed % 6;
  default: return 0;
  }
}

/**
 * Encode three-packed event value
 * Per IEEE 802.1Q, events are packed as: a*36 + b*6 + c
 */
static u8 encode_three_packed_event(u8 a, u8 b, u8 c)
{
  return a * 36 + b * 6 + c;
}

/**
 * Decode four-packed declaration type
 * Per IEEE 802.1Qat, declarations are packed as: a*64 + b*16 + c*4 + d
 */
static u8 decode_four_packed_decl(u8 packed, int index)
{
  switch (index)
  {
  case 0: return (packed >> 6) & 0x03;
  case 1: return (packed >> 4) & 0x03;
  case 2: return (packed >> 2) & 0x03;
  case 3: return packed & 0x03;
  default: return 0;
  }
}

/**
 * Encode four-packed declaration type
 */
static u8 encode_four_packed_decl(u8 a, u8 b, u8 c, u8 d)
{
  return (a << 6) | (b << 4) | (c << 2) | d;
}

/**
 * Get current time in milliseconds
 */
__attribute__((unused))
static u64 get_time_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/**
 * Check if timer has expired
 */
static bool timer_expired(const struct timespec* timer, u32 timeout_ms)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  u64 timer_ms = (u64)timer->tv_sec * 1000 + timer->tv_nsec / 1000000;
  u64 now_ms = (u64)now.tv_sec * 1000 + now.tv_nsec / 1000000;

  return (now_ms - timer_ms) >= timeout_ms;
}

/**
 * Reset timer to current time
 */
static void timer_reset(struct timespec* timer)
{
  clock_gettime(CLOCK_MONOTONIC, timer);
}

/* ============================================================================
 * MRP Applicant State Machine - IEEE 802.1Q-2022 Section 10.7.4
 * ============================================================================
 * Simplified implementation for listener participation
 */

/**
 * Process applicant state machine on Join! event (local application wants to join)
 */
static void applicant_join(msrp_listener_decl_t* listener)
{
  mrp_applicant_state_t old_state = listener->applicant_state;

  switch (listener->applicant_state)
  {
  case MRP_APPLICANT_VO:
  case MRP_APPLICANT_LO:
    listener->applicant_state = MRP_APPLICANT_VN;
    listener->tx_pending = true;
    break;
  case MRP_APPLICANT_LA:
    listener->applicant_state = MRP_APPLICANT_AA;
    break;
  case MRP_APPLICANT_AO:
    listener->applicant_state = MRP_APPLICANT_AP;
    break;
  case MRP_APPLICANT_QO:
    listener->applicant_state = MRP_APPLICANT_QP;
    break;
  default:
    /* Already in a joined state, no change */
    break;
  }

  if (old_state != listener->applicant_state)
  {
    ESP_LOGI(TAG, "Applicant Join!: %s -> %s",
             mrp_applicant_state_string(old_state),
             mrp_applicant_state_string(listener->applicant_state));
  }
}

/**
 * Process applicant state machine on Leave! event (local application wants to leave)
 */
static void applicant_leave(msrp_listener_decl_t* listener)
{
  mrp_applicant_state_t old_state = listener->applicant_state;

  switch (listener->applicant_state)
  {
  case MRP_APPLICANT_VN:
  case MRP_APPLICANT_AN:
  case MRP_APPLICANT_AA:
  case MRP_APPLICANT_QA:
    listener->applicant_state = MRP_APPLICANT_LA;
    break;
  case MRP_APPLICANT_VP:
  case MRP_APPLICANT_AP:
  case MRP_APPLICANT_QP:
    listener->applicant_state = MRP_APPLICANT_VO;
    break;
  default:
    break;
  }

  if (old_state != listener->applicant_state)
  {
    ESP_LOGI(TAG, "Applicant Leave!: %s -> %s",
             mrp_applicant_state_string(old_state),
             mrp_applicant_state_string(listener->applicant_state));
  }
}

/**
 * Process applicant state machine when receiving rJoinIn event
 * (received JoinIn from another participant)
 */
static void applicant_rx_joinin(msrp_listener_decl_t* listener)
{
  mrp_applicant_state_t old_state = listener->applicant_state;

  switch (listener->applicant_state)
  {
  case MRP_APPLICANT_VO:
    listener->applicant_state = MRP_APPLICANT_AO;
    break;
  case MRP_APPLICANT_VP:
    listener->applicant_state = MRP_APPLICANT_AP;
    break;
  case MRP_APPLICANT_AA:
    listener->applicant_state = MRP_APPLICANT_QA;
    break;
  case MRP_APPLICANT_AO:
    listener->applicant_state = MRP_APPLICANT_QO;
    break;
  case MRP_APPLICANT_AP:
    listener->applicant_state = MRP_APPLICANT_QP;
    break;
  default:
    break;
  }

  if (old_state != listener->applicant_state)
  {
    ESP_LOGD(TAG, "Applicant rJoinIn: %s -> %s",
             mrp_applicant_state_string(old_state),
             mrp_applicant_state_string(listener->applicant_state));
  }
}

/**
 * Process applicant state machine when receiving rIn event
 */
static void applicant_rx_in(msrp_listener_decl_t* listener)
{
  mrp_applicant_state_t old_state = listener->applicant_state;

  switch (listener->applicant_state)
  {
  case MRP_APPLICANT_AA:
    listener->applicant_state = MRP_APPLICANT_QA;
    break;
  default:
    break;
  }

  if (old_state != listener->applicant_state)
  {
    ESP_LOGD(TAG, "Applicant rIn: %s -> %s",
             mrp_applicant_state_string(old_state),
             mrp_applicant_state_string(listener->applicant_state));
  }
}

/**
 * Process applicant state machine when receiving rJoinMt or rMt event
 */
static void applicant_rx_empty(msrp_listener_decl_t* listener)
{
  mrp_applicant_state_t old_state = listener->applicant_state;

  switch (listener->applicant_state)
  {
  case MRP_APPLICANT_QA:
    listener->applicant_state = MRP_APPLICANT_AA;
    listener->tx_pending = true;
    break;
  case MRP_APPLICANT_QO:
    listener->applicant_state = MRP_APPLICANT_AO;
    break;
  case MRP_APPLICANT_QP:
    listener->applicant_state = MRP_APPLICANT_AP;
    break;
  default:
    break;
  }

  if (old_state != listener->applicant_state)
  {
    ESP_LOGD(TAG, "Applicant rEmpty: %s -> %s",
             mrp_applicant_state_string(old_state),
             mrp_applicant_state_string(listener->applicant_state));
  }
}

/**
 * Process applicant state machine when receiving rLeave event
 */
static void applicant_rx_leave(msrp_listener_decl_t* listener)
{
  mrp_applicant_state_t old_state = listener->applicant_state;

  switch (listener->applicant_state)
  {
  case MRP_APPLICANT_VO:
  case MRP_APPLICANT_AO:
  case MRP_APPLICANT_QO:
    listener->applicant_state = MRP_APPLICANT_LO;
    break;
  case MRP_APPLICANT_VP:
  case MRP_APPLICANT_AP:
  case MRP_APPLICANT_QP:
    listener->applicant_state = MRP_APPLICANT_VO;
    break;
  default:
    break;
  }

  if (old_state != listener->applicant_state)
  {
    ESP_LOGD(TAG, "Applicant rLeave: %s -> %s",
             mrp_applicant_state_string(old_state),
             mrp_applicant_state_string(listener->applicant_state));
  }
}

/**
 * Process applicant state machine on tx opportunity
 * Returns the event to transmit, or -1 if no transmission needed
 * Per IEEE 802.1Q-2022 Table 10-3
 */
static int applicant_tx(msrp_listener_decl_t* listener)
{
  mrp_applicant_state_t old_state = listener->applicant_state;
  int tx_event = -1;

  switch (listener->applicant_state)
  {
  case MRP_APPLICANT_VN:
    /* VN + tx! -> AN, send sN (New) */
    listener->applicant_state = MRP_APPLICANT_AN;
    tx_event = MSRP_ATTRIBUTE_EVENT_NEW;
    break;
  case MRP_APPLICANT_AN:
    /* AN + tx! -> QA, send sN (New) */
    listener->applicant_state = MRP_APPLICANT_QA;
    tx_event = MSRP_ATTRIBUTE_EVENT_NEW;
    break;
  case MRP_APPLICANT_AA:
    /* AA + tx! -> QA, send sJ (JoinIn) */
    listener->applicant_state = MRP_APPLICANT_QA;
    tx_event = MSRP_ATTRIBUTE_EVENT_JOININ;
    break;
  case MRP_APPLICANT_QA:
    /* QA is quiet - no transmission on tx! */
    /* QA only transmits when stimulated to AA by rMt/rLv events */
    break;
  case MRP_APPLICANT_LA:
    /* LA + tx! -> VO, send sL (Leave) */
    listener->applicant_state = MRP_APPLICANT_VO;
    tx_event = MSRP_ATTRIBUTE_EVENT_LV;
    break;
  case MRP_APPLICANT_VP:
    /* VP + tx! -> AA, send sJ (JoinMt) */
    listener->applicant_state = MRP_APPLICANT_AA;
    tx_event = MSRP_ATTRIBUTE_EVENT_JOINMT;
    break;
  case MRP_APPLICANT_AP:
    /* AP + tx! -> QA, send sJ (JoinIn) */
    listener->applicant_state = MRP_APPLICANT_QA;
    tx_event = MSRP_ATTRIBUTE_EVENT_JOININ;
    break;
  case MRP_APPLICANT_VO:
  case MRP_APPLICANT_AO:
  case MRP_APPLICANT_QO:
  case MRP_APPLICANT_QP:
    /* These states don't transmit on tx! */
    break;
  case MRP_APPLICANT_LO:
    /* LO + tx! -> VO, no transmission */
    listener->applicant_state = MRP_APPLICANT_VO;
    break;
  default:
    break;
  }

  listener->tx_pending = false;

  if (old_state != listener->applicant_state)
  {
    ESP_LOGI(TAG, "Applicant TX: %s -> %s (event: %s)",
             mrp_applicant_state_string(old_state),
             mrp_applicant_state_string(listener->applicant_state),
             tx_event >= 0 ? msrp_attribute_event_string(tx_event) : "none");
  }

  return tx_event;
}

/* ============================================================================
 * MRP Registrar State Machine - IEEE 802.1Q-2022 Section 10.7.5
 * ============================================================================
 */

/**
 * Process registrar state machine on rNew, rJoinIn, or rJoinMt event
 */
static void registrar_rx_join(msrp_talker_info_t* talker)
{
  mrp_registrar_state_t old_state = talker->registrar_state;

  switch (talker->registrar_state)
  {
  case MRP_REGISTRAR_MT:
  case MRP_REGISTRAR_LV:
    talker->registrar_state = MRP_REGISTRAR_IN;
    ESP_LOGI(TAG, "Registrar: Stream 0x%016llX now IN (registered)",
             (unsigned long long)talker->stream_id);
    break;
  case MRP_REGISTRAR_IN:
    /* Already registered, refresh */
    break;
  }

  if (old_state != talker->registrar_state)
  {
    ESP_LOGD(TAG, "Registrar Join: %s -> %s",
             mrp_registrar_state_string(old_state),
             mrp_registrar_state_string(talker->registrar_state));
  }
}

/**
 * Process registrar state machine on rLeave event
 */
static void registrar_rx_leave(msrp_talker_info_t* talker)
{
  mrp_registrar_state_t old_state = talker->registrar_state;

  switch (talker->registrar_state)
  {
  case MRP_REGISTRAR_IN:
    talker->registrar_state = MRP_REGISTRAR_LV;
    timer_reset(&talker->leave_timer);
    break;
  default:
    break;
  }

  if (old_state != talker->registrar_state)
  {
    ESP_LOGD(TAG, "Registrar Leave: %s -> %s",
             mrp_registrar_state_string(old_state),
             mrp_registrar_state_string(talker->registrar_state));
  }
}

/**
 * Process registrar leave timer expiry
 */
static void registrar_leave_timer_expired(msrp_talker_info_t* talker)
{
  if (talker->registrar_state == MRP_REGISTRAR_LV)
  {
    talker->registrar_state = MRP_REGISTRAR_MT;
    talker->valid = false;
    ESP_LOGI(TAG, "Registrar: Stream 0x%016llX leave timer expired, now MT",
             (unsigned long long)talker->stream_id);
  }
}

/* ============================================================================
 * MSRP Message Handlers
 * ============================================================================
 */

static void handle_msrp_talker_advertise(struct avtp_state_s* state, void* buf, size_t len)
{
  struct talker_advertise_data_s
  {
    u16 leave_all_event_and_number_of_values;
    u64 stream_id;
    u8 stream_da[6];
    u16 stream_vlan_id;
    u16 max_frame_size;
    u16 max_frame_interval;
    u8 priority_and_rank;
    u32 accumulated_latency;
    u8 attribute_event;
  } __attribute__((packed));

  struct talker_advertise_data_s* talker_adv = (struct talker_advertise_data_s*)buf;

  u16 leave_all = (ntohs(talker_adv->leave_all_event_and_number_of_values) >> 13) & 0x07;
  u16 number_of_values = ntohs(talker_adv->leave_all_event_and_number_of_values) & 0x1FFF;
  u8 event = decode_three_packed_event(talker_adv->attribute_event, 0);

  (void)leave_all; /* Used for debugging */
  (void)number_of_values; /* Used for debugging */

  u64 stream_id = ntohll(talker_adv->stream_id);

  ESP_LOGI(TAG, "  Talker Advertise: stream=0x%016llX event=%s",
           (unsigned long long)stream_id, msrp_attribute_event_string(event));
  ESP_LOGD(TAG, "    Leave All: %u, Number of Values: %u", leave_all, number_of_values);
  ESP_LOGD(TAG, "    Stream DA: %02X:%02X:%02X:%02X:%02X:%02X",
           talker_adv->stream_da[0], talker_adv->stream_da[1], talker_adv->stream_da[2],
           talker_adv->stream_da[3], talker_adv->stream_da[4], talker_adv->stream_da[5]);
  ESP_LOGD(TAG, "    VLAN: %u, MaxFrame: %u, MaxInterval: %u",
           ntohs(talker_adv->stream_vlan_id), ntohs(talker_adv->max_frame_size),
           ntohs(talker_adv->max_frame_interval));

  msrp_state_t* msrp = &state->msrp;
  msrp_talker_info_t* talker = &msrp->talker;

  /* Update talker information */
  if (!talker->valid || talker->stream_id == stream_id)
  {
    talker->valid = true;
    talker->stream_id = stream_id;
    memcpy(talker->dest_addr, talker_adv->stream_da, 6);
    talker->vlan_id = ntohs(talker_adv->stream_vlan_id);
    talker->max_frame_size = ntohs(talker_adv->max_frame_size);
    talker->max_frame_interval = ntohs(talker_adv->max_frame_interval);
    talker->priority = (talker_adv->priority_and_rank >> 5) & 0x07;
    talker->rank = (talker_adv->priority_and_rank >> 4) & 0x01;
    talker->accumulated_latency = ntohl(talker_adv->accumulated_latency);
    talker->failed = false;

    /* Process registrar state machine */
    switch (event)
    {
    case MSRP_ATTRIBUTE_EVENT_NEW:
    case MSRP_ATTRIBUTE_EVENT_JOININ:
    case MSRP_ATTRIBUTE_EVENT_JOINMT:
      registrar_rx_join(talker);
      break;
    case MSRP_ATTRIBUTE_EVENT_LV:
      registrar_rx_leave(talker);
      break;
    case MSRP_ATTRIBUTE_EVENT_IN:
    case MSRP_ATTRIBUTE_EVENT_MT:
    default:
      /* Just refresh, don't change state */
      break;
    }

    /* If we have an active listener for this stream, process applicant state */
    if (msrp->listener.active && msrp->listener.stream_id == stream_id)
    {
      /* Add MAC filter for the stream destination address so we can receive packets */
      esp_eth_handle_t eth_handle;
      if (ioctl(state->socket, L2TAP_G_DEVICE_DRV_HNDL, &eth_handle) == 0)
      {
        esp_err_t err = esp_eth_ioctl(eth_handle, ETH_CMD_ADD_MAC_FILTER, talker->dest_addr);
        if (err == ESP_OK)
        {
          ESP_LOGI(TAG, "Added MAC filter for stream dest: %02X:%02X:%02X:%02X:%02X:%02X",
                   talker->dest_addr[0], talker->dest_addr[1],
                   talker->dest_addr[2], talker->dest_addr[3],
                   talker->dest_addr[4], talker->dest_addr[5]);
        }
      }

      /* Upgrade from ASKING_FAILED to READY now that talker is available */
      if (msrp->listener.declaration_type == MSRP_LISTENER_ASKING_FAILED &&
        talker->registrar_state == MRP_REGISTRAR_IN)
      {
        msrp->listener.declaration_type = MSRP_LISTENER_READY;
        msrp->listener.tx_pending = true;
        ESP_LOGI(TAG, "Talker available for stream 0x%016llX, upgrading to READY",
                 (unsigned long long)stream_id);
      }

      switch (event)
      {
      case MSRP_ATTRIBUTE_EVENT_JOININ:
        applicant_rx_joinin(&msrp->listener);
        break;
      case MSRP_ATTRIBUTE_EVENT_IN:
        applicant_rx_in(&msrp->listener);
        break;
      case MSRP_ATTRIBUTE_EVENT_LV:
        applicant_rx_leave(&msrp->listener);
        break;
      case MSRP_ATTRIBUTE_EVENT_MT:
      case MSRP_ATTRIBUTE_EVENT_JOINMT:
        applicant_rx_empty(&msrp->listener);
        break;
      default:
        break;
      }
    }
  }
}

static void handle_msrp_talker_failed(struct avtp_state_s* state, void* buf, size_t len)
{
  struct talker_failed_data_s
  {
    u16 leave_all_event_and_number_of_values;
    u64 stream_id;
    u8 stream_da[6];
    u16 stream_vlan_id;
    u16 max_frame_size;
    u16 max_frame_interval;
    u8 priority_and_rank;
    u32 accumulated_latency;
    u64 failed_bridge_id;
    u8 failure_code;
    u8 attribute_event;
  } __attribute__((packed));

  struct talker_failed_data_s* msg = (struct talker_failed_data_s*)buf;

  u16 leave_all = (ntohs(msg->leave_all_event_and_number_of_values) >> 13) & 0x07;
  u16 number_of_values = ntohs(msg->leave_all_event_and_number_of_values) & 0x1FFF;

  (void)leave_all; /* Used for debugging */
  (void)number_of_values; /* Used for debugging */

  u64 stream_id = ntohll(msg->stream_id);

  ESP_LOGI(TAG, "  Talker Failed: stream=0x%016llX failure_code=%u",
           (unsigned long long)stream_id, msg->failure_code);

  msrp_state_t* msrp = &state->msrp;
  msrp_talker_info_t* talker = &msrp->talker;

  /* Update talker information as failed */
  if (!talker->valid || talker->stream_id == stream_id)
  {
    talker->valid = true;
    talker->stream_id = stream_id;
    memcpy(talker->dest_addr, msg->stream_da, 6);
    talker->vlan_id = ntohs(msg->stream_vlan_id);
    talker->max_frame_size = ntohs(msg->max_frame_size);
    talker->max_frame_interval = ntohs(msg->max_frame_interval);
    talker->priority = (msg->priority_and_rank >> 5) & 0x07;
    talker->rank = (msg->priority_and_rank >> 4) & 0x01;
    talker->accumulated_latency = ntohl(msg->accumulated_latency);
    talker->failed = true;
    talker->failure_code = msg->failure_code;
    talker->failure_bridge_id = ntohll(msg->failed_bridge_id);

    registrar_rx_join(talker);

    /* If we're listening to this stream, update declaration type */
    if (msrp->listener.active && msrp->listener.stream_id == stream_id)
    {
      if (msrp->listener.declaration_type == MSRP_LISTENER_READY)
      {
        msrp->listener.declaration_type = MSRP_LISTENER_READY_FAILED;
        msrp->listener.tx_pending = true;
        ESP_LOGW(TAG, "Stream 0x%016llX failed, switching to READY_FAILED",
                 (unsigned long long)stream_id);
      }
    }
  }
}

static void handle_msrp_listener(struct avtp_state_s* state, void* buf, size_t len)
{
  struct listener_data_s
  {
    u16 leave_all_event_and_number_of_values;
    u64 stream_id;
    u8 attribute_event;
    u8 declaration_type;
  } __attribute__((packed));

  struct listener_data_s* msg = (struct listener_data_s*)buf;

  u16 number_of_values = ntohs(msg->leave_all_event_and_number_of_values) & 0x1FFF;
  (void)number_of_values; /* Used for debugging */

  u64 stream_id = ntohll(msg->stream_id);
  u8 event = decode_three_packed_event(msg->attribute_event, 0);
  u8 decl = decode_four_packed_decl(msg->declaration_type, 0);

  ESP_LOGI(TAG, "  Listener: stream=0x%016llX event=%s decl=%s",
           (unsigned long long)stream_id,
           msrp_attribute_event_string(event),
           msrp_listener_decl_string(decl));

  /* Process applicant state machine if this is for our stream */
  msrp_state_t* msrp = &state->msrp;
  if (msrp->listener.active && msrp->listener.stream_id == stream_id)
  {
    switch (event)
    {
    case MSRP_ATTRIBUTE_EVENT_JOININ:
      applicant_rx_joinin(&msrp->listener);
      break;
    case MSRP_ATTRIBUTE_EVENT_IN:
      applicant_rx_in(&msrp->listener);
      break;
    case MSRP_ATTRIBUTE_EVENT_LV:
      applicant_rx_leave(&msrp->listener);
      break;
    case MSRP_ATTRIBUTE_EVENT_MT:
    case MSRP_ATTRIBUTE_EVENT_JOINMT:
      applicant_rx_empty(&msrp->listener);
      break;
    default:
      break;
    }
  }
}

static void handle_msrp_domain(struct avtp_state_s* state, void* buf, size_t len)
{
  struct domain_data_s
  {
    u16 leave_all_event_and_number_of_values;
    u8 sr_class_id;
    u8 sr_class_priority;
    u16 sr_class_vid;
    u8 attribute_event;
  } __attribute__((packed));

  struct domain_data_s* msg = (struct domain_data_s*)buf;

  u8 event = decode_three_packed_event(msg->attribute_event, 0);

  ESP_LOGI(TAG, "  Domain: class=%u prio=%u vid=%u event=%s",
           msg->sr_class_id, msg->sr_class_priority,
           ntohs(msg->sr_class_vid), msrp_attribute_event_string(event));

  /* Update domain information */
  msrp_state_t* msrp = &state->msrp;
  int domain_idx = (msg->sr_class_id == MSRP_SR_CLASS_A) ? 0 : 1;

  if (domain_idx < 2)
  {
    msrp->domains[domain_idx].valid = true;
    msrp->domains[domain_idx].sr_class_id = msg->sr_class_id;
    msrp->domains[domain_idx].sr_class_priority = msg->sr_class_priority;
    msrp->domains[domain_idx].sr_class_vid = ntohs(msg->sr_class_vid);

    if (event == MSRP_ATTRIBUTE_EVENT_NEW ||
      event == MSRP_ATTRIBUTE_EVENT_JOININ ||
      event == MSRP_ATTRIBUTE_EVENT_JOINMT)
    {
      msrp->domains[domain_idx].registrar_state = MRP_REGISTRAR_IN;
    }
    else if (event == MSRP_ATTRIBUTE_EVENT_LV)
    {
      msrp->domains[domain_idx].registrar_state = MRP_REGISTRAR_LV;
    }
  }
}

/* ============================================================================
 * MSRP Message Transmission
 * ============================================================================
 */

int msrp_send_listener_declaration(struct avtp_state_s* state, u64 stream_id,
                                   u8 declaration_type, u8 event)
{
  struct listener_msg_s
  {
    struct header_s header;
    u8 protocol_version;
    u8 attribute_type;
    u8 attribute_length;
    u16 attribute_list_length;
    u16 leave_all_event_and_number_of_values;
    u64 stream_id;
    u8 attribute_event;
    u8 declaration_type_packed;
    u16 end_mark_list;
    u16 end_mark;
  } __attribute__((packed));

  struct listener_msg_s msg = {0};

  /* Ethernet header */
  memcpy(msg.header.dst_mac, MSRP_MULTICAST_MAC, sizeof(msg.header.dst_mac));
  memcpy(msg.header.src_mac, state->intf_hw_addr, sizeof(msg.header.src_mac));
  msg.header.eth_type[0] = (ETH_TYPE_MSRP >> 8) & 0xFF;
  msg.header.eth_type[1] = ETH_TYPE_MSRP & 0xFF;

  /* MSRP header */
  msg.protocol_version = 0;
  msg.attribute_type = MSRP_ATTRIBUTE_TYPE_LISTENER;
  msg.attribute_length = 8; /* StreamID (8 bytes) */
  msg.attribute_list_length = htons(14); /* 2 + 8 + 1 + 1 + 2 = 14 bytes */

  /* Vector header: leave_all=0, number_of_values=1 */
  msg.leave_all_event_and_number_of_values = htons(1);

  /* Stream ID (first value) */
  msg.stream_id = htonll(stream_id);

  /* Three-packed event (one event, padded with zeros) */
  msg.attribute_event = encode_three_packed_event(event, 0, 0);

  /* Four-packed declaration type (one declaration, padded with zeros) */
  msg.declaration_type_packed = encode_four_packed_decl(declaration_type, 0, 0, 0);

  /* End marks */
  msg.end_mark_list = 0;
  msg.end_mark = 0;

  ESP_LOGI(TAG, "Sending Listener %s for stream 0x%016llX (event=%s)",
           msrp_listener_decl_string(declaration_type),
           (unsigned long long)stream_id,
           msrp_attribute_event_string(event));

  ssize_t written = write(state->msrp_socket, &msg, sizeof(msg));
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send MSRP Listener: %d (errno: %d)", (int)written, errno);
    return -1;
  }

  return 0;
}

int msrp_send_talker_advertise(struct avtp_state_s* state)
{
  struct talker_advertise_s msg = {0};

  memcpy(msg.header.dst_mac, MSRP_MULTICAST_MAC, sizeof(msg.header.dst_mac));
  memcpy(msg.header.src_mac, state->intf_hw_addr, sizeof(msg.header.src_mac));
  msg.header.eth_type[0] = (ETH_TYPE_MSRP >> 8) & 0xFF;
  msg.header.eth_type[1] = ETH_TYPE_MSRP & 0xFF;

  msg.attribute_type = MSRP_ATTRIBUTE_TYPE_TALKER_ADVERTISE;
  msg.attribute_length = 25;
  msg.attribute_list_length = htons(30);

  msg.number_of_values = htons(1);
  msg.stream_id = htonll(state->talker_stream_info.stream_id);
  memcpy(msg.stream_da, state->talker_stream_info.stream_dest_mac, sizeof(msg.stream_da));
  msg.stream_vlan_id = htons(state->talker_stream_info.stream_vlan_id);
  msg.max_frame_size = htons(224);
  msg.max_frame_interval = htons(1);
  msg.priority = MSRP_SR_CLASS_A_PRIO;
  msg.rank = 1;
  msg.accumulated_latency = htonl(100095);
  msg.attribute_event = encode_three_packed_event(MSRP_ATTRIBUTE_EVENT_JOININ, 0, 0);

  ESP_LOGI(TAG, "Sending Talker Advertise for stream 0x%016llX",
           (unsigned long long)state->talker_stream_info.stream_id);

  ssize_t written = write(state->msrp_socket, &msg, sizeof(msg));
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send MSRP Talker Advertise: %d (errno: %d)", (int)written, errno);
    return -1;
  }

  return 0;
}

/* ============================================================================
 * MSRP Packet Reception
 * ============================================================================
 */

void msrp_net_rx(struct avtp_state_s* state)
{
  union
  {
    struct msrp_header_s header;
    u8 raw[256];
  } buf;

  ssize_t len = read(state->msrp_socket, &buf, sizeof(buf));
  if (len <= 0)
  {
    if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
    {
      ESP_LOGE(TAG, "Failed to read MSRP message: %d (errno: %d)", (int)len, errno);
    }
    return;
  }

  ESP_LOGD(TAG, "MSRP packet received (%d bytes)", (int)len);

  /* Parse MSRP message */
  size_t offset = sizeof(struct header_s) + 1; /* header + protocol_version */

  while (offset + 4 < (size_t)len)
  {
    u8 attribute_type = buf.raw[offset];
    u8 attribute_length = buf.raw[offset + 1];
    u16 attribute_list_length = ntohs(*(u16*)&buf.raw[offset + 2]);

    if (attribute_list_length == 0 || offset + 4 + attribute_list_length > (size_t)len)
    {
      break;
    }

    void* attr_data = &buf.raw[offset + 4];
    size_t attr_len = attribute_list_length;

    switch (attribute_type)
    {
    case MSRP_ATTRIBUTE_TYPE_TALKER_ADVERTISE:
      if (attribute_length == 25)
      {
        handle_msrp_talker_advertise(state, attr_data, attr_len);
      }
      break;

    case MSRP_ATTRIBUTE_TYPE_TALKER_FAILED:
      if (attribute_length == 34)
      {
        handle_msrp_talker_failed(state, attr_data, attr_len);
      }
      break;

    case MSRP_ATTRIBUTE_TYPE_LISTENER:
      if (attribute_length == 8)
      {
        handle_msrp_listener(state, attr_data, attr_len);
      }
      break;

    case MSRP_ATTRIBUTE_TYPE_DOMAIN:
      if (attribute_length == 4)
      {
        handle_msrp_domain(state, attr_data, attr_len);
      }
      break;

    default:
      ESP_LOGW(TAG, "Unknown MSRP attribute type: %u", attribute_type);
      break;
    }

    offset += 4 + attribute_list_length;

    /* Check for end mark */
    if (offset + 2 <= (size_t)len)
    {
      u16 end_mark = ntohs(*(u16*)&buf.raw[offset]);
      if (end_mark == 0x0000)
      {
        break;
      }
    }
  }
}

/* ============================================================================
 * MSRP Periodic Processing
 * ============================================================================
 */

/**
 * Check if applicant is in an "anxious" state that requires periodic transmission
 */
static bool applicant_is_anxious(mrp_applicant_state_t state)
{
  switch (state)
  {
  case MRP_APPLICANT_VN:
  case MRP_APPLICANT_AN:
  case MRP_APPLICANT_AA:
  case MRP_APPLICANT_LA:
  case MRP_APPLICANT_VP:
  case MRP_APPLICANT_AP:
  case MRP_APPLICANT_AO:
  case MRP_APPLICANT_LO:
    return true;
  case MRP_APPLICANT_QA:
  case MRP_APPLICANT_QO:
  case MRP_APPLICANT_QP:
  case MRP_APPLICANT_VO:
  default:
    return false;
  }
}

void msrp_periodic(struct avtp_state_s* state)
{
  msrp_state_t* msrp = &state->msrp;

  /* Check registrar leave timer */
  if (msrp->talker.valid && msrp->talker.registrar_state == MRP_REGISTRAR_LV)
  {
    if (timer_expired(&msrp->talker.leave_timer, msrp->leave_timeout_ms))
    {
      registrar_leave_timer_expired(&msrp->talker);
    }
  }

  /* Check if we need to transmit listener declaration */
  if (msrp->listener.active)
  {
    bool should_tx = false;

    /* Only transmit on timer if in an anxious state or tx_pending is set */
    if (timer_expired(&msrp->last_tx_time, msrp->join_timeout_ms))
    {
      /* In anxious states, transmit on timer expiry */
      if (applicant_is_anxious(msrp->listener.applicant_state))
      {
        should_tx = true;
      }
      /* Reset timer even if not transmitting to avoid immediate trigger next time */
      timer_reset(&msrp->last_tx_time);
    }

    /* Always transmit if tx_pending is explicitly set */
    if (msrp->listener.tx_pending)
    {
      should_tx = true;
    }

    if (should_tx)
    {
      int tx_event = applicant_tx(&msrp->listener);
      if (tx_event >= 0)
      {
        msrp_send_listener_declaration(state,
                                       msrp->listener.stream_id,
                                       msrp->listener.declaration_type,
                                       tx_event);
        timer_reset(&msrp->listener.join_timer);
      }
    }
  }
}

/* ============================================================================
 * MSRP Listener Join/Leave API
 * ============================================================================
 */

int msrp_listener_join(struct avtp_state_s* state, u64 stream_id)
{
  msrp_state_t* msrp = &state->msrp;

  /* For now, only support single stream */
  if (msrp->listener.active && msrp->listener.stream_id != stream_id)
  {
    ESP_LOGE(TAG, "Already listening to stream 0x%016llX, cannot join 0x%016llX",
             (unsigned long long)msrp->listener.stream_id,
             (unsigned long long)stream_id);
    return -1;
  }

  ESP_LOGI(TAG, "Joining stream 0x%016llX as listener", (unsigned long long)stream_id);

  msrp->listener.active = true;
  msrp->listener.stream_id = stream_id;

  /* If we know the talker's stream destination MAC, add it to the MAC filter
   * so we can receive the multicast stream packets */
  if (msrp->talker.valid && msrp->talker.stream_id == stream_id)
  {
    /* Get ethernet handle to add MAC filter */
    esp_eth_handle_t eth_handle;
    if (ioctl(state->socket, L2TAP_G_DEVICE_DRV_HNDL, &eth_handle) == 0)
    {
      esp_err_t err = esp_eth_ioctl(eth_handle, ETH_CMD_ADD_MAC_FILTER, msrp->talker.dest_addr);
      if (err == ESP_OK)
      {
        ESP_LOGI(TAG, "Added MAC filter for stream dest: %02X:%02X:%02X:%02X:%02X:%02X",
                 msrp->talker.dest_addr[0], msrp->talker.dest_addr[1],
                 msrp->talker.dest_addr[2], msrp->talker.dest_addr[3],
                 msrp->talker.dest_addr[4], msrp->talker.dest_addr[5]);
      }
      else
      {
        ESP_LOGW(TAG, "Failed to add MAC filter for stream: %s", esp_err_to_name(err));
      }
    }
  }

  /* Determine declaration type based on talker state */
  if (msrp->talker.valid && msrp->talker.stream_id == stream_id)
  {
    if (msrp->talker.failed)
    {
      msrp->listener.declaration_type = MSRP_LISTENER_READY_FAILED;
    }
    else if (msrp->talker.registrar_state == MRP_REGISTRAR_IN)
    {
      msrp->listener.declaration_type = MSRP_LISTENER_READY;
    }
    else
    {
      msrp->listener.declaration_type = MSRP_LISTENER_ASKING_FAILED;
    }
  }
  else
  {
    /* Talker not yet known, start with ASKING_FAILED */
    msrp->listener.declaration_type = MSRP_LISTENER_ASKING_FAILED;
  }

  /* Trigger Join! event on applicant state machine */
  applicant_join(&msrp->listener);

  return 0;
}

int msrp_listener_leave(struct avtp_state_s* state, u64 stream_id)
{
  msrp_state_t* msrp = &state->msrp;

  if (!msrp->listener.active || msrp->listener.stream_id != stream_id)
  {
    ESP_LOGW(TAG, "Not listening to stream 0x%016llX", (unsigned long long)stream_id);
    return -1;
  }

  ESP_LOGI(TAG, "Leaving stream 0x%016llX as listener", (unsigned long long)stream_id);

  /* Trigger Leave! event on applicant state machine */
  applicant_leave(&msrp->listener);

  /* Send immediate Leave message */
  msrp_send_listener_declaration(state, stream_id,
                                 msrp->listener.declaration_type,
                                 MSRP_ATTRIBUTE_EVENT_LV);

  /* Mark as inactive after sending leave */
  msrp->listener.active = false;

  return 0;
}

/* ============================================================================
 * MSRP Initialization
 * ============================================================================
 */

void msrp_state_init(msrp_state_t* state)
{
  memset(state, 0, sizeof(*state));

  /* Initialize timer values */
  state->join_timeout_ms = MRP_JOIN_TIME_MS;
  state->leave_timeout_ms = MRP_LEAVE_TIME_MS;
  state->leave_all_timeout_ms = MRP_LEAVE_ALL_TIME_MS;

  /* Initialize applicant state to VO (observer) */
  state->listener.applicant_state = MRP_APPLICANT_VO;

  /* Initialize registrar state to MT (empty) */
  state->talker.registrar_state = MRP_REGISTRAR_MT;
  state->domains[0].registrar_state = MRP_REGISTRAR_MT;
  state->domains[1].registrar_state = MRP_REGISTRAR_MT;

  timer_reset(&state->last_tx_time);
}

void msrp_send_domain_request(const struct avtp_state_s* state)
{
  struct domain_msg_s
  {
    struct header_s header;
    u8 protocol_version;
    u8 attribute_type;
    u8 attribute_length;
    u16 attribute_list_length;
    u16 leave_all_event_and_number_of_values;
    u8 sr_class_id;
    u8 sr_class_priority;
    u16 sr_class_vid;
    u8 attribute_event;
    u16 end_mark_list;
    u16 end_mark;
  } __attribute__((packed));

  struct domain_msg_s msg = {0};

  /* Ethernet header */
  memcpy(msg.header.dst_mac, MSRP_MULTICAST_MAC, sizeof(msg.header.dst_mac));
  memcpy(msg.header.src_mac, state->intf_hw_addr, sizeof(msg.header.src_mac));
  msg.header.eth_type[0] = (ETH_TYPE_MSRP >> 8) & 0xFF;
  msg.header.eth_type[1] = ETH_TYPE_MSRP & 0xFF;

  /* MSRP header */
  msg.protocol_version = 0;
  msg.attribute_type = MSRP_ATTRIBUTE_TYPE_DOMAIN;
  msg.attribute_length = 4; /* SRclassID(1) + SRclassPriority(1) + SRclassVID(2) */
  msg.attribute_list_length = htons(9); /* 2 + 4 + 1 + 2 = 9 bytes */

  /* Vector header: leave_all=0, number_of_values=1 */
  msg.leave_all_event_and_number_of_values = htons(1);

  /* Domain first value - Class A */
  msg.sr_class_id = MSRP_SR_CLASS_A;
  msg.sr_class_priority = MSRP_SR_CLASS_A_PRIO;
  msg.sr_class_vid = htons(2); /* Default VLAN */

  /* Three-packed event (JoinMt to discover domain) */
  msg.attribute_event = encode_three_packed_event(MSRP_ATTRIBUTE_EVENT_JOINMT, 0, 0);

  /* End marks */
  msg.end_mark_list = 0;
  msg.end_mark = 0;

  ESP_LOGI(TAG, "Sending Domain Discovery for Class A");

  ssize_t written = write(state->msrp_socket, &msg, sizeof(msg));
  if (written < 0)
  {
    ESP_LOGE(TAG, "Failed to send MSRP Domain Request: %d (errno: %d)", (int)written, errno);
  }
}

int msrp_init(const char* interface)
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

