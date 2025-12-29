#include "maap.h"

#include <avtp.h>
#include <cc.h>
#include <config.h>
#include <esp_eth_spec.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <sys/types.h>
#include <sys/unistd.h>

#define TAG "maap"

void maap_state_machine(struct avtp_state_s* state, maap_event event, struct maap_pdu_s* msg);

void generate_address(u8* maap_mac)
{
  // random part
  u16 random_part = esp_random() % 0xFE00; // Limit to 0xFE00 to avoid reserved addresses

  maap_mac[0] = 0x91;
  maap_mac[1] = 0xE0;
  maap_mac[2] = 0xF0;
  maap_mac[3] = 0x00;
  maap_mac[4] = (random_part >> 8) & 0xFF;
  maap_mac[5] = random_part & 0xFF;
}

void create_maap_msg(struct avtp_state_s* state, struct maap_pdu_s* msg, u8 message_type)
{
  memcpy(msg->header.dst_mac, MAAP_MULTICAST_ADDR, ETH_ADDR_LEN);
  memcpy(msg->header.src_mac, state->intf_hw_addr, ETH_ADDR_LEN);
  msg->header.eth_type[0] = (ETH_TYPE_AVTP >> 8) & 0xFF;
  msg->header.eth_type[1] = ETH_TYPE_AVTP & 0xFF;
  msg->subtype = AVTP_SUBTYPE_MAAP;
  msg->message_type = message_type;
  msg->maap_version_and_control_data_length = htons(0x081c); // MAAP version 1, control data length 28 bytes
  msg->stream_id = htonll(0); // Stream ID is not used in MAAP, set to 0
}

void maap_send_announce(struct avtp_state_s* state, const u8* allocated_mac, const size_t allocated_count)
{
  struct maap_pdu_s msg = {0};

  create_maap_msg(state, &msg, MAAP_MSG_TYPE_ANNOUNCE);

  memcpy(msg.requested_start_address, state->maap_db.mac, ETH_ADDR_LEN);
  msg.requested_count = htons(CONFIG_TALKER_STREAM_SOURCES); // Announcing allocated MAC addresses

  ssize_t len = write(state->socket, &msg, sizeof(msg));
  if (len < 0)
  {
    ESP_LOGE(TAG, "Failed to send MAAP Announce: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Sent MAAP Announce for MAC %02X:%02X:%02X:%02X:%02X:%02X",
             state->maap_db.mac[0], state->maap_db.mac[1], state->maap_db.mac[2],
             state->maap_db.mac[3], state->maap_db.mac[4], state->maap_db.mac[5]);
  }
}

void maap_send_defend(struct avtp_state_s* state, const u8* requested_mac, const size_t requested_count)
{
  struct maap_pdu_s msg = {0};

  create_maap_msg(state, &msg, MAAP_MSG_TYPE_DEFEND);

  memcpy(msg.requested_start_address, requested_mac, ETH_ADDR_LEN);
  msg.requested_count = htons(requested_count); // Requesting 1 MAC address

  ssize_t len = write(state->socket, &msg, sizeof(msg));
  if (len < 0)
  {
    ESP_LOGE(TAG, "Failed to send MAAP Probe: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Sent MAAP Probe for MAC %02X:%02X:%02X:%02X:%02X:%02X",
             state->maap_db.mac[0], state->maap_db.mac[1], state->maap_db.mac[2],
             state->maap_db.mac[3], state->maap_db.mac[4], state->maap_db.mac[5]);
  }
}

void maap_send_probe(struct avtp_state_s* state)
{
  struct maap_pdu_s msg = {0};
  create_maap_msg(state, &msg, MAAP_MSG_TYPE_PROBE);

  memcpy(msg.requested_start_address, state->maap_db.mac, ETH_ADDR_LEN);
  msg.requested_count = htons(CONFIG_TALKER_STREAM_SOURCES); // Requesting 1 MAC address

  ssize_t len = write(state->socket, &msg, sizeof(msg));
  if (len < 0)
  {
    ESP_LOGE(TAG, "Failed to send MAAP Probe: %d", errno);
  }
  else
  {
    ESP_LOGI(TAG, "Sent MAAP Probe for MAC %02X:%02X:%02X:%02X:%02X:%02X",
             state->maap_db.mac[0], state->maap_db.mac[1], state->maap_db.mac[2],
             state->maap_db.mac[3], state->maap_db.mac[4], state->maap_db.mac[5]);
  }
}

void handle_maap_announce(struct avtp_state_s* state, struct maap_pdu_s* msg)
{
  u64 requested_mac;
  memcpy(&requested_mac, msg->requested_start_address, 6);

  ESP_LOGI(TAG, "Received MAAP Announce for MAC %02X:%02X:%02X:%02X:%02X:%02X, count %u",
           msg->requested_start_address[0], msg->requested_start_address[1], msg->requested_start_address[2],
           msg->requested_start_address[3], msg->requested_start_address[4], msg->requested_start_address[5],
           ntohs(msg->requested_count));

  // todo check for conflicts for all counts
  if (memcmp(&requested_mac, state->maap_db.mac, 6) == 0)
  {
    ESP_LOGW(TAG, "MAAP Conflict detected for our allocated MAC address!");
    // Handle conflict (e.g., reallocate MAC address)
  }
}


u32 random_in_range(const u32 min, const u32 max)
{
  u32 r = esp_random();
  u32 span = (max - min + 1);
  return (u32)(r % span) + min;
}

void dec_maap_probe_count(struct avtp_state_s* state)
{
  if (state->maap_db.probe_count > 1)
  {
    state->maap_db.probe_count--;
  }
  else
  {
    maap_state_machine(state, MAAP_EVENT_PROBE_COUNT, NULL);
  }
}

static void maap_handle_announce_timer(void* arg)
{
  struct avtp_state_s* state = (struct avtp_state_s*)arg;
  if (state->maap_db.state == MAAP_STATE_DEFEND)
  {
    const u32 announce_timer = random_in_range(MAAP_ANNOUNCE_MIN_INTERVAL_MS, MAAP_ANNOUNCE_MAX_INTERVAL_MS);
    esp_timer_start_once(state->maap_db.announce_timer_handle, announce_timer * 1000ULL);
    maap_send_announce(state, state->maap_db.mac, 1);
  }
}

static void maap_handle_probe_timer(void* arg)
{
  struct avtp_state_s* state = (struct avtp_state_s*)arg;
  const u32 probe_timer = random_in_range(MAAP_PROBE_MIN_INTERVAL_MS, MAAP_PROBE_MAX_INTERVAL_MS);
  esp_timer_start_once(state->maap_db.probe_timer_handle, probe_timer * 1000ULL);
  maap_send_probe(state);
  dec_maap_probe_count(state);
}

bool compare_mac(const u8* a, const u8 a_count, const u8* b, const u8 b_count)
{
  u64 a_start, b_start;
  memcpy(&a_start, a, 6);
  memcpy(&b_start, b, 6);

  u64 a_end = a_start + a_count - 1;
  u64 b_end = b_start + b_count - 1;

  if (a_start <= b_end && b_start <= a_end)
  {
    return true; // Ranges overlap
  }
  return false; // No overlap
}

void maap_state_machine(struct avtp_state_s* state, maap_event event, struct maap_pdu_s* msg)
{
  switch (event)
  {
  case MAAP_EVENT_BEGIN:
  case MAAP_EVENT_RESTART:
    ESP_LOGI(TAG, "MAAP Event: BEGIN");
    if (state->maap_db.state == MAAP_STATE_INITIAL)
    {
      generate_address(state->maap_db.mac);
      return maap_state_machine(state, MAAP_EVENT_RESERVE_ADDRESS, NULL);
    }
    break;
  case MAAP_EVENT_RESERVE_ADDRESS:
    ESP_LOGI(TAG, "MAAP Event: RESERVE_ADDRESS");
    if (state->maap_db.state == MAAP_STATE_INITIAL)
    {
      state->maap_db.probe_count = MAAP_PROBE_RETRANSMITS;
      const u32 probe_timer = random_in_range(MAAP_PROBE_MIN_INTERVAL_MS, MAAP_PROBE_MAX_INTERVAL_MS);
      maap_send_probe(state);
      // Create and start timer
      esp_timer_create_args_t timer_args = {
        .callback = maap_handle_probe_timer,
        .arg = state,
        .name = "maap_probe_timer"
      };
      esp_timer_create(&timer_args, &state->maap_db.probe_timer_handle);
      esp_timer_start_once(state->maap_db.probe_timer_handle, probe_timer * 1000ULL);
      state->maap_db.state = MAAP_STATE_PROBE;
    }
    break;
  case MAAP_EVENT_PROBE_COUNT:
    // stop timer
    esp_timer_stop(state->maap_db.probe_timer_handle);
    // start announce timer
    {
      const u32 announce_timer = random_in_range(MAAP_ANNOUNCE_MIN_INTERVAL_MS, MAAP_ANNOUNCE_MAX_INTERVAL_MS);
      esp_timer_create_args_t timer_args = {
        .callback = maap_handle_announce_timer,
        .arg = state,
        .name = "maap_announce_timer"
      };
      esp_timer_create(&timer_args, &state->maap_db.announce_timer_handle);
      esp_timer_start_once(state->maap_db.announce_timer_handle, announce_timer * 1000ULL);
    }
    // send announce
    maap_send_announce(state, state->maap_db.mac, 1);
    state->maap_db.state = MAAP_STATE_DEFEND;
    break;

  case MAAP_EVENT_RANNOUNCE:

    if (state->maap_db.state == MAAP_STATE_INITIAL)
    {
      // Ignore announce in INITIAL state
      break;
    }

    const bool conflict = compare_mac(msg->requested_start_address, ntohs(msg->requested_count),
                                      state->maap_db.mac, CONFIG_TALKER_STREAM_SOURCES);
    if (conflict)
    {
      ESP_LOGW(TAG, "MAAP Conflict detected from Announce message!");
      // Stop announce timer
      switch (state->maap_db.state)
      {
      case MAAP_STATE_PROBE:
        esp_timer_stop(state->maap_db.probe_timer_handle);
        esp_timer_delete(state->maap_db.probe_timer_handle);
        break;
      case MAAP_STATE_DEFEND:
        esp_timer_stop(state->maap_db.announce_timer_handle);
        esp_timer_delete(state->maap_db.announce_timer_handle);
        break;
      default:
        break;
      }
      state->maap_db.state = MAAP_STATE_INITIAL;
      // Restart allocation process
      return maap_state_machine(state, MAAP_EVENT_RESTART, NULL);
    }
    break;

  case MAAP_EVENT_RPROBE:
    if (state->maap_db.state != MAAP_STATE_INITIAL)
    {
      break;
    }
    const bool p_conflict = compare_mac(msg->requested_start_address, ntohs(msg->requested_count),
                                        state->maap_db.mac, CONFIG_TALKER_STREAM_SOURCES);
    if (p_conflict)
    {
      ESP_LOGW(TAG, "MAAP Conflict detected from Probe message!");
      switch (state->maap_db.state)
      {
      case MAAP_STATE_PROBE:
        esp_timer_stop(state->maap_db.probe_timer_handle);
        esp_timer_delete(state->maap_db.probe_timer_handle);
        maap_state_machine(state, MAAP_EVENT_RESTART, NULL);
        break;
      case MAAP_STATE_DEFEND:
        maap_send_defend(state, msg->requested_start_address, ntohs(msg->requested_count));
        break;
      default:
        break;
      }
    }
    break;
  case MAAP_EVENT_RDEFEND:
    if (state->maap_db.state != MAAP_STATE_INITIAL)
    {
      break;
    }

    const bool d_conflict = compare_mac(msg->requested_start_address, ntohs(msg->requested_count),
                                        state->maap_db.mac, CONFIG_TALKER_STREAM_SOURCES);
    if (d_conflict)
    {
      switch (state->maap_db.state)
      {
      case MAAP_STATE_PROBE:
        esp_timer_stop(state->maap_db.probe_timer_handle);
        esp_timer_delete(state->maap_db.probe_timer_handle);
        break;
      case MAAP_STATE_DEFEND:
        esp_timer_stop(state->maap_db.announce_timer_handle);
        esp_timer_delete(state->maap_db.announce_timer_handle);
        break;
      default:
        break;
      }
      maap_state_machine(state, MAAP_EVENT_RESTART, NULL);
    }
    break;
  case MAAP_EVENT_RELEASE:
    switch (state->maap_db.state)
    {
    case MAAP_STATE_PROBE:
      esp_timer_stop(state->maap_db.probe_timer_handle);
      esp_timer_delete(state->maap_db.probe_timer_handle);
      state->maap_db.state = MAAP_STATE_INITIAL;
      break;
    case MAAP_STATE_DEFEND:
      esp_timer_stop(state->maap_db.announce_timer_handle);
      esp_timer_delete(state->maap_db.announce_timer_handle);
      state->maap_db.state = MAAP_STATE_INITIAL;
      break;
    default:
      // no action on INITIAL state
      break;
    }
  default:
    ESP_LOGW(TAG, "MAAP Event: Unhandled event %d", event);
    break;
  }
}

void maap_init(struct avtp_state_s* state)
{
  maap_state_machine(state, MAAP_EVENT_BEGIN, NULL);
}

void maap_net_rx(struct avtp_state_s* state, struct maap_pdu_s* msg, ssize_t len)
{
  switch (msg->message_type)
  {
  case MAAP_MSG_TYPE_ANNOUNCE:
    maap_state_machine(state, MAAP_EVENT_RANNOUNCE, msg);
    break;
  case MAAP_MSG_TYPE_PROBE:
    maap_state_machine(state, MAAP_EVENT_RPROBE, msg);
    break;
  case MAAP_MSG_TYPE_DEFEND:
    maap_state_machine(state, MAAP_EVENT_RDEFEND, msg);
    break;
  default:
    ESP_LOGW(TAG, "Unknown MAAP message type received: 0x%02X", msg->message_type);
    break;
  }
}
