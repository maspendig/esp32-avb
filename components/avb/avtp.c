#include "avtp.h"
#include "aecp.h"
#include "adp.h"
#include "maap.h"
#include "msrp.h"
#include "mvrp.h"
#include "media_queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_attr.h"
#include <errno.h>

#include "esp_vfs_l2tap.h"
#include "esp_eth_driver.h"
#include "acmp.h"

#include <fcntl.h>
#include <nvs_flash.h>

#include "esp_eth_spec.h"
#include "pthread.h"
#include "sys/ioctl.h"
#include <arpa/inet.h>
#include <time.h>
#include <sys/select.h>

#include "audio.h"

#define CONFIG_ADP_SEND_INTERVAL_MSEC 5800

/* Task configuration for dual-core processing */
#define STREAM_TASK_CORE    1       /* Core 1 for high-priority stream processing */
#define CONTROL_TASK_CORE   0       /* Core 0 for control protocols */
#define STREAM_TASK_PRIORITY  14    /* Higher priority for stream data */
#define CONTROL_TASK_PRIORITY 6    /* Lower priority for control protocols */

const char* TAG = "avtp";

static TaskHandle_t talker_stream_task_handle;
static TaskHandle_t listener_stream_task_handle;

/* Forward declarations */
static uint64_t mac_to_entity_id(uint64_t mac);

static struct avtp_state_s* s_state;

static int avtp_init_state(struct avtp_state_s* state, const char* interface)
{
  state->socket = open("/dev/net/tap", 0);
  if (state->socket < 0)
  {
    ESP_LOGE(TAG, "Failed to create tx socket");
    return ESP_FAIL;
  }

  int ioctl_err = ioctl(state->socket, L2TAP_S_INTF_DEVICE, interface);

  if (ioctl_err < 0)
  {
    ESP_LOGE(TAG, "failed to set network interface %s at socket: %d\n", interface, ioctl_err);
    return ESP_FAIL;
  }

  // Set the Ethertype filter for untagged AVTP frames
  uint16_t eth_type_filter = ETH_TYPE_AVTP;
  if (ioctl(state->socket, L2TAP_S_RCV_FILTER, &eth_type_filter) < 0)
  {
    ESP_LOGE(TAG, "failed to set Ethertype filter: %d\n", errno);
    return ESP_FAIL;
  }

  /* Create VLAN socket for receiving VLAN-tagged AVTP streams (802.1Q) */
  state->vlan_socket = open("/dev/net/tap", 0);
  if (state->vlan_socket < 0)
  {
    ESP_LOGE(TAG, "Failed to create VLAN socket");
    return ESP_FAIL;
  }

  ioctl_err = ioctl(state->vlan_socket, L2TAP_S_INTF_DEVICE, interface);
  if (ioctl_err < 0)
  {
    ESP_LOGE(TAG, "failed to set network interface %s at VLAN socket: %d\n", interface, ioctl_err);
    close(state->vlan_socket);
    return ESP_FAIL;
  }

  /* Filter for 802.1Q VLAN-tagged frames */
  eth_type_filter = ETH_TYPE_8021Q;
  if (ioctl(state->vlan_socket, L2TAP_S_RCV_FILTER, &eth_type_filter) < 0)
  {
    ESP_LOGE(TAG, "failed to set VLAN Ethertype filter: %d\n", errno);
    close(state->vlan_socket);
    return ESP_FAIL;
  }

  /* Set VLAN socket to non-blocking mode for efficient draining of stream packets */
  int flags = fcntl(state->vlan_socket, F_GETFL, 0);
  if (flags >= 0)
  {
    fcntl(state->vlan_socket, F_SETFL, flags | O_NONBLOCK);
  }
  ESP_LOGI(TAG, "VLAN socket initialized for 802.1Q tagged frames (non-blocking)");
  // Get the ethernet handle to configure multicast reception
  esp_eth_handle_t eth_handle;
  if (ioctl(state->socket, L2TAP_G_DEVICE_DRV_HNDL, &eth_handle) < 0)
  {
    ESP_LOGE(TAG, "failed to get ethernet handle: %d\n", errno);
    return ESP_FAIL;
  }

  /* Initialize the media queue for audio sample buffering */
  esp_err_t err = media_queue_init(&state->media_queue);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to initialize media queue");
    return ESP_FAIL;
  }

  state->acmp_sequence_id = 0;
  state->msrp_socket = msrp_init_socket(interface);
  state->mvrp_socket = mvrp_init_socket(interface);


  // get HW address
  esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, &state->intf_hw_addr);

  // Generate entity_id from MAC address
  uint64_t mac = ((uint64_t)state->intf_hw_addr[0] << 40) |
    ((uint64_t)state->intf_hw_addr[1] << 32) |
    ((uint64_t)state->intf_hw_addr[2] << 24) |
    ((uint64_t)state->intf_hw_addr[3] << 16) |
    ((uint64_t)state->intf_hw_addr[4] << 8) |
    ((uint64_t)state->intf_hw_addr[5]);

  /* Initialize MSRP state machine */
  msrp_state_init(state);

  /* Initialize MVRP state machine */
  mvrp_state_init(state);
  state->entity_id = mac_to_entity_id(mac);
  state->entity_model_id = 0x0000000000000001ULL;

  ESP_LOGI(TAG, "Entity ID initialized: 0x%016llX, Model ID: 0x%016llX (from MAC: %02X:%02X:%02X:%02X:%02X:%02X)",
           (unsigned long long)state->entity_id,
           (unsigned long long)state->entity_model_id,
           state->intf_hw_addr[0], state->intf_hw_addr[1], state->intf_hw_addr[2],
           state->intf_hw_addr[3], state->intf_hw_addr[4], state->intf_hw_addr[5]);

  /* Also enable all multicast reception as fallback */
  bool enable_multicast = true;
  err = esp_eth_ioctl(eth_handle, ETH_CMD_S_ALL_MULTICAST, &enable_multicast);
  if (err != ESP_OK)
  {
    ESP_LOGW(TAG, "failed to enable all multicast reception: %s", esp_err_to_name(err));
  }
  else
  {
    ESP_LOGI(TAG, "All multicast reception enabled");
  }

  /* Add specific multicast MAC filters for AVB:
   * - MAAP dynamic allocation range: 91:E0:F0:00:00:00 - 91:E0:F0:00:FD:FF
   * - AVDECC/ATDECC: 91:E0:F0:01:00:00
   */
  uint8_t maap_base_mac[6] = {0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x00}; /* Common MAAP address */
  err = esp_eth_ioctl(eth_handle, ETH_CMD_ADD_MAC_FILTER, maap_base_mac);
  if (err != ESP_OK)
  {
    ESP_LOGW(TAG, "failed to add MAAP MAC filter: %s", esp_err_to_name(err));
  }
  else
  {
    ESP_LOGI(TAG, "Added MAC filter for MAAP address %02X:%02X:%02X:%02X:%02X:%02X",
             maap_base_mac[0], maap_base_mac[1], maap_base_mac[2],
             maap_base_mac[3], maap_base_mac[4], maap_base_mac[5]);
  }

  /* Add AVDECC multicast address */
  uint8_t avdecc_mac[6] = {0x91, 0xE0, 0xF0, 0x01, 0x00, 0x00};
  err = esp_eth_ioctl(eth_handle, ETH_CMD_ADD_MAC_FILTER, avdecc_mac);
  if (err != ESP_OK)
  {
    ESP_LOGW(TAG, "failed to add AVDECC MAC filter: %s", esp_err_to_name(err));
  }

  // Initialize MAAP
  maap_init(state);

  s_state = state;
  return ESP_OK;
}

static uint64_t mac_to_entity_id(uint64_t mac)
{
  return ((mac & 0xffffff000000) << 16) | (0xfffe000000) | (mac & 0xffffff);
}

static void extract_am824_audio_to_16(const u8* packet, size_t packet_len, s16* output_buffer, u8 channels,
                                      size_t* output_samples)
{
  iec61883_t* iec61883 = (iec61883_t*)&packet[18];

  iec61883_cip_header_t* cip = (iec61883_cip_header_t*)&packet[42];
  /* amount of channels per sample */
  u8 dbs = cip->dbs;

  u8* audio_data = (u8*)&packet[50];

  unsigned int sample_count = 0;
  // Calculate number of samples in the packet
  for (int i = 0; i < iec61883->stream_data_length && i < packet_len - 50; i += 4)
  {
    u8 channel_index = (i / 4) % dbs;
    if (channel_index < channels)
    {
      am824_sample_t* sample = (am824_sample_t*)&audio_data[i];

      s32 sample_value = sample->sample[0] << 16 | sample->sample[1] << 8 | sample->sample[2];
      if (sample_value & 0x800000)
      {
        sample_value |= 0xFF000000; // sign extend negative values
      }
      output_buffer[sample_count++] = (int16_t)(sample_value >> 8);
    }
  }
  *output_samples = sample_count / channels;
}

static IRAM_ATTR void extract_am824_audio_to_32(const u8* packet, size_t packet_len, s32* output_buffer, u8 channels,
                                                size_t* output_samples)
{
  iec61883_t* iec61883 = (iec61883_t*)&packet[18];

  iec61883_cip_header_t* cip = (iec61883_cip_header_t*)&packet[42];
  /* amount of channels per sample */
  u8 dbs = cip->dbs;

  u8* audio_data = (u8*)&packet[50];

  unsigned int sample_count = 0;
  // Calculate number of samples in the packet
  for (int i = 0; i < iec61883->stream_data_length && i < packet_len - 50; i += 4)
  {
    u8 channel_index = (i / 4) % dbs;
    if (channel_index < channels)
    {
      am824_sample_t* sample = (am824_sample_t*)&audio_data[i];
      output_buffer[sample_count++] = 0x00 | sample->sample[2] << 8 | sample->sample[1] << 16 | sample->sample[0] << 24;
    }
  }
  *output_samples = sample_count / channels;
}


static int64_t timespec_to_ms(const struct timespec* ts)
{
  return ts->tv_sec * 1000 + (ts->tv_nsec / 1000000ll);
}

/**
 * This task exclusively handles VLAN-tagged AVB stream packets.
 * It runs on a dedicated core with high priority to minimize packet loss.
 */
static void avtp_listener_stream_task(void* arg)
{
  struct avtp_state_s* state = (struct avtp_state_s*)arg;
  u8 raw_buf[1518]; /* Max Ethernet frame size */
  u8 seq_number = 0;


  ESP_LOGI(TAG, "Stream task started on core %d (priority %d)",
           xPortGetCoreID(), uxTaskPriorityGet(NULL));

  empty_audio_buffer();

  while (!state->listener_stop)
  {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(state->vlan_socket, &readfds);

    /* Short timeout to ensure responsive shutdown */
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 10000}; /* 10ms */

    int ret = select(state->vlan_socket + 1, &readfds, NULL, NULL, &timeout);

    if (ret > 0 && FD_ISSET(state->vlan_socket, &readfds))
    {
      ssize_t len;
      bool first_packet = true;

      /* Drain all available packets (non-blocking) */
      while ((len = read(state->vlan_socket, raw_buf, sizeof(raw_buf))) > 0)
      {
        /* Parse 802.1Q VLAN header */
        u16 tci = (raw_buf[14] << 8) | raw_buf[15];
        u16 vlan_id = tci & 0x0FFF;
        u8 pcp = (tci >> 13) & 0x07;
        u16 inner_ethertype = (raw_buf[16] << 8) | raw_buf[17];

        /* Check if inner Ethertype is AVTP */
        if (inner_ethertype == ETH_TYPE_AVTP)
        {
          u8 subtype = raw_buf[18];

          if (subtype < 0x80)
          {
            switch (subtype)
            {
            case AVTP_SUBTYPE_AAF:
              ESP_LOGI(TAG, "Received AAF stream packet: VLAN ID=%u, PCP=%u, Length=%zd bytes",
                       vlan_id, pcp, len);
              break;
            case AVTP_SUBTYPE_61883_IIDC:
              {
                if (first_packet)
                {
                  seq_number = raw_buf[20];
                  first_packet = false;
                }

                if (raw_buf[20] != seq_number)
                {
                  seq_number = raw_buf[20] + 1;
                  continue;
                }

                /* Parse AVTP header for timestamp info */
                iec61883_t* iec61883 = (iec61883_t*)&raw_buf[18];
                u32 avtp_timestamp = ntohl(iec61883->avtp_timestamp);
                bool timestamp_valid = iec61883->avtp_info & 0x01;
                bool timestamp_uncertain = iec61883->time_uncertain;

                u8 channels = OUTPUT_CHANNELS;
                size_t num_samples = 0;

                /* Prepare media queue entry */
                media_queue_entry_t entry = {
                  .avtp_timestamp = avtp_timestamp,
                  .sequence_number = raw_buf[20],
                  .timestamp_valid = timestamp_valid,
                  .timestamp_uncertain = timestamp_uncertain,
                };

#if SAMPLE_BIT_RATE == 16
                extract_am824_audio_to_16(raw_buf, len, entry.samples, channels, &num_samples);
#else
                extract_am824_audio_to_32(raw_buf, len, entry.samples, channels, &num_samples);
#endif

                entry.sample_count = num_samples;

                if (num_samples > 0)
                {
                  /* Push to media queue - consumer task will write to codec */
                  media_queue_push(&state->media_queue, &entry);
                }
                seq_number++;
              }
              break;
            default:
              ESP_LOGW(TAG, "Received unknown AVTP subtype: %d", subtype);
              break;
            }
          }
        }
      }
    }
  }

  empty_audio_buffer();
  ESP_LOGI(TAG, "Listener stream task exiting");
  vTaskDelete(listener_stream_task_handle);
}

/**
 * @brief Control protocol task - runs on Core 0
 *
 * Handles AVTP control messages (ADP, AECP, ACMP, MAAP) and
 * reservation protocols (MSRP, MVRP).
 */
static void avtp_control_task(void* arg)
{
  struct avtp_state_s* state = (struct avtp_state_s*)arg;

  /* Buffer for control messages */
  union
  {
    struct avtp_discovery_msg_s adp;
    struct aecp_data_unit_s aecp;
    struct acmp_common_s acmp;
    struct maap_pdu_s maap;
    struct avtp_header_s header;
    u8 raw[1518];
  } buf;

  ESP_LOGI(TAG, "Control task started on core %d (priority %d)",
           xPortGetCoreID(), uxTaskPriorityGet(NULL));

  while (!state->stop)
  {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(state->socket, &readfds);
    FD_SET(state->msrp_socket, &readfds);
    FD_SET(state->mvrp_socket, &readfds);

    int max_fd = state->socket;
    if (state->msrp_socket > max_fd) max_fd = state->msrp_socket;
    if (state->mvrp_socket > max_fd) max_fd = state->mvrp_socket;

    struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000}; /* 100ms */

    int ret = select(max_fd + 1, &readfds, NULL, NULL, &timeout);

    if (ret < 0)
    {
      ESP_LOGE(TAG, "select() error: %d (errno: %d)", ret, errno);
      continue;
    }

    /* Process AVTP control socket */
    if (FD_ISSET(state->socket, &readfds))
    {
      const ssize_t len = read(state->socket, &buf, sizeof(buf));
      if (len > 0)
      {
        u8 subtype = buf.header.subtype;
        switch (subtype)
        {
        case AVTP_SUBTYPE_ADP:
          adp_net_rx(state, &buf.adp, len);
          break;
        case AVTP_SUBTYPE_AECP:
          aecp_net_rx(state, &buf.aecp, len);
          break;
        case AVTP_SUBTYPE_ACMP:
          acmp_net_rx(state, &buf.acmp, len);
          break;
        case AVTP_SUBTYPE_MAAP:
          maap_net_rx(state, &buf.maap, len);
          break;
        default:
          ESP_LOGW(TAG, "Unknown AVTP control subtype received: 0x%02X", subtype);
          break;
        }
      }
    }

    /* Process MVRP socket */
    if (FD_ISSET(state->mvrp_socket, &readfds))
    {
      mvrp_net_rx(state);
    }

    /* Process MSRP socket */
    if (FD_ISSET(state->msrp_socket, &readfds))
    {
      msrp_net_rx(state);
    }

    /* Send periodic ADP announcements */
    struct timespec time_now;
    struct timespec delta;

    clock_gettime(CLOCK_MONOTONIC, &time_now);
    timespecsub(&time_now, &state->last_transmitted_adp, &delta);
    if (timespec_to_ms(&delta) > CONFIG_ADP_SEND_INTERVAL_MSEC)
    {
      state->last_transmitted_adp = time_now;
      send_adp_entity_available(state);
    }
  }

  ESP_LOGI(TAG, "Control task exiting");
  vTaskDelete(NULL);
}

int start_avtp_listener(const char* interface)
{
  if (s_state != NULL)
  {
    ESP_LOGE(TAG, "Other instance of AVTP is already running");
    return ESP_FAIL;
  }

  if (interface == NULL)
  {
    interface = "ETH_0";
  }

  struct avtp_state_s* state = calloc(1, sizeof(struct avtp_state_s));
  if (!state)
  {
    ESP_LOGE(TAG, "Failed to allocate memory for AVTP state");
    return ESP_ERR_NO_MEM;
  }

  if (avtp_init_state(state, interface) != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to initialize AVTP state");
    free(state);
    return ESP_FAIL;
  }


  /* Create control task on Core 0 */
  TaskHandle_t control_task_handle;
  esp_err_t ret = xTaskCreatePinnedToCore(
    avtp_control_task,
    "avtp_ctrl",
    8192,
    state,
    CONTROL_TASK_PRIORITY,
    &control_task_handle,
    CONTROL_TASK_CORE
  );

  if (ret != pdPASS)
  {
    ESP_LOGE(TAG, "Failed to create control task");
    state->stop = true;
    vTaskDelay(pdMS_TO_TICKS(100)); /* Give stream task time to exit */
    s_state = NULL;
    free(state);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "AVTP dual-core architecture started: Stream on Core %d, Control on Core %d",
           STREAM_TASK_CORE, CONTROL_TASK_CORE);

  return ESP_OK;
}

typedef struct iec61883_am824_packet
{
  struct header_s header;

  struct
  {
    u16 tci;
    u16 vlan_eth_type;
  } vlan_tag;

  iec61883_t iec61883;

  union
  {
    struct
    {
      u8 format_tag : 2;
      u8 channel : 6;
      u8 tcode : 4;
      u8 app_specific_control : 4;
    } packet_info;

    u8 packet_info_raw[2];
  } packet_info_u;

  iec61883_cip_header_t cip;
  am824_sample_t audio_data[6 * 8];
} iec61883_am824_packet_t;

void avtp_set_common_header(struct avtp_state_s* state, struct iec61883_am824_packet* msg)
{
  /* Set Ethernet header */
  memcpy(msg->header.dst_mac, state->talker_stream_info.stream_dest_mac, sizeof(msg->header.dst_mac));
  memcpy(msg->header.src_mac, state->intf_hw_addr, sizeof(msg->header.src_mac));

  /* Ethernet type (big-endian) */
  msg->header.eth_type[0] = (ETH_TYPE_8021Q >> 8) & 0xFF;
  msg->header.eth_type[1] = ETH_TYPE_8021Q & 0xFF;

  u16 priority = 3;
  u16 ineligible = 0;
  u16 vlan_id = state->talker_stream_info.stream_vlan_id;
  msg->vlan_tag.tci = htons((priority << 13) | (ineligible << 12) | vlan_id);
  msg->vlan_tag.vlan_eth_type = htons(ETH_TYPE_AVTP);
}

void avtp_send_am824_stream(struct avtp_state_s* state, s32* audio_samples, u32 u32)
{
  iec61883_am824_packet_t p = {0};
  avtp_set_common_header(state, &p);
  p.iec61883.subtype = AVTP_SUBTYPE_61883_IIDC;

  bool stream_id_valid = 1;
  u8 version = 0;
  bool media_clock_restart = 0;
  bool gateway_info_valid = 0;
  bool timestamp_valid = 0;
  p.iec61883.avtp_info = (stream_id_valid << 7) |
    (version << 4) |
    (media_clock_restart << 3) |
    (gateway_info_valid << 2) |
    (timestamp_valid << 1);
  p.iec61883.sequence_num = state->talker_stream_info.sequence_number++;
  p.iec61883.stream_id = htonll(state->talker_stream_info.stream_id);
  p.iec61883.avtp_timestamp = htonl(0); // TODO proper timestamp
  p.iec61883.gateway_info = htonl(0);
  p.iec61883.stream_data_length = htons(200); // TODO proper length
  u8 format_tag = 0x1; // CIP header included
  u8 channel = 31; // Originating source is on AVTP network
  u8 tcode = 0xa; // Data packet
  u8 app_specific_control = 0;
  p.packet_info_u.packet_info_raw[0] = (format_tag << 6) | (channel & 0x3F);
  p.packet_info_u.packet_info_raw[1] = (tcode << 4) | (app_specific_control & 0x0F);
  p.cip.qi_1 = 0x0;
  p.cip.sid = 63; // Origination source is on AVTP network
  p.cip.dbs = 0x8;
  p.cip.fn = 0;
  p.cip.sph = 0;
  p.cip.dbc = state->talker_stream_info.cip_data_block_continuity += 6; // 6 data blocks
  p.cip.qi_2 = 0x2;
  p.cip.fmt = 0x10; // AM824 format
  p.cip.cip_fmt_specific_data[0] = 0x02;
  p.cip.cip_fmt_specific_data[1] = 0x00;
  p.cip.cip_fmt_specific_data[2] = 0x08; //

  // Fill audio data
  u8 audio_samples_idx = 0;
  u8 avtp_samples_idx = 0;
  for (int sample_idx = 0; sample_idx < 6; sample_idx++)
  {
    for (int ch = 0; ch < 8; ch++)
    {
      s32 sample_value = 0;
      if (ch < OUTPUT_CHANNELS)
      {
        sample_value = audio_samples[audio_samples_idx++];
      }
      p.audio_data[avtp_samples_idx].label = 0x40; // Audio data label
      p.audio_data[avtp_samples_idx].sample[0] = (sample_value >> 24) & 0xFF;
      p.audio_data[avtp_samples_idx].sample[1] = (sample_value >> 16) & 0xFF;
      p.audio_data[avtp_samples_idx].sample[2] = (sample_value >> 8) & 0xFF;
      avtp_samples_idx++;
    }
  }
  ssize_t sent_bytes = write(state->vlan_socket, &p, sizeof(p));
  if (sent_bytes < 0)
  {
    ESP_LOGE(TAG, "Failed to send AVTP stream packet: %d", errno);
  }
}


static void avtp_talker_stream_task(void* arg)
{
  struct avtp_state_s* state = arg;
  ESP_LOGI(TAG, "Talker stream task started on core %d (priority %d)",
           xPortGetCoreID(), uxTaskPriorityGet(NULL));
  while (!state->talker_stop)
  {
    u32 bytes_read = 0;
#if SAMPLE_BIT_RATE == 16
    s16 audio_samples[INPUT_CHANNELS * 6];
    CODEC_I2S_READ(audio_samples, sizeof(audio_samples), &bytes_read);
#else
    s32 audio_samples[INPUT_CHANNELS * 6];
    CODEC_I2S_READ(audio_samples, sizeof(audio_samples), &bytes_read);
    // last byte is always 0x00 due to 24-bit audio in 32-bit container
    avtp_send_am824_stream(state, audio_samples, bytes_read);

#endif
  }
  ESP_LOGI(TAG, "Talker stream task exiting");
  vTaskDelete(talker_stream_task_handle);
}

void avtp_talker_stop(struct avtp_state_s* state)
{
  state->talker_stop = true;
}

int avtp_talker_start(struct avtp_state_s* state)
{
  state->talker_stop = false;
  /* Create high-priority stream task on Core 1 */
  BaseType_t ret = xTaskCreatePinnedToCore(
    avtp_talker_stream_task,
    "avtp_talker",
    8192,
    state,
    STREAM_TASK_PRIORITY,
    &talker_stream_task_handle,
    STREAM_TASK_CORE
  );

  // TODO errorhandling!
  if (ret != pdPASS)
  {
    ESP_LOGE(TAG, "Failed to create stream task");
    s_state = NULL;
    free(state);
    return ESP_FAIL;
  }
  return ret;
}

int avtp_listener_start(struct avtp_state_s* state)
{
  state->listener_stop = false;


  /* Create high-priority stream task on Core 1 */
  BaseType_t ret = xTaskCreatePinnedToCore(
    avtp_listener_stream_task,
    "avtp_listener",
    8192,
    state,
    STREAM_TASK_PRIORITY,
    &listener_stream_task_handle,
    STREAM_TASK_CORE
  );

  if (ret != pdPASS)
  {
    ESP_LOGE(TAG, "Failed to create listener stream task");
    s_state = NULL;
    free(state);
    return ESP_FAIL;
  }

  /* Start the media queue consumer task */
  esp_err_t err = media_queue_start(&state->media_queue);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to start media queue consumer");
    media_queue_deinit(&state->media_queue);
    return ESP_FAIL;
  }
  return ret;
}

void avtp_listener_stop(struct avtp_state_s* state)
{
  state->listener_stop = true;

  /* Give listener task time to exit */
  vTaskDelay(pdMS_TO_TICKS(50));

  /* Stop and cleanup the media queue */
  media_queue_stop(&state->media_queue);
}
