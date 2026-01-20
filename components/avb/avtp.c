#include "avtp.h"
#include "aecp.h"
#include "adp.h"
#include "maap.h"
#include "msrp.h"
#include "mvrp.h"

#include "esp_log.h"
#include "esp_err.h"
#include <errno.h>

#include "esp_vfs_l2tap.h"
#include "esp_eth_driver.h"
#include "acmp.h"

#include <fcntl.h>

#include "esp_eth_spec.h"
#include "pthread.h"
#include "sys/ioctl.h"
#include <arpa/inet.h>
#include <time.h>
#include <sys/select.h>

#include "audio_output.h"

#define CONFIG_ADP_SEND_INTERVAL_MSEC 5800

/* Task configuration for dual-core processing */
#define STREAM_TASK_CORE    1       /* Core 1 for high-priority stream processing */
#define CONTROL_TASK_CORE   0       /* Core 0 for control protocols */
#define STREAM_TASK_PRIORITY  15    /* Higher priority for stream data */
#define CONTROL_TASK_PRIORITY 10    /* Lower priority for control protocols */

const char* TAG = "avtp";

/* Forward declarations */
static uint64_t mac_to_entity_id(uint64_t mac);
static void extract_am824_audio(const u8* packet, size_t packet_len, int16_t* output_buffer, size_t* output_samples);

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
  mvrp_init(state);
  state->entity_id = mac_to_entity_id(mac);
  state->entity_model_id = 0x0000000000000001ULL;

  ESP_LOGI(TAG, "Entity ID initialized: 0x%016llX, Model ID: 0x%016llX (from MAC: %02X:%02X:%02X:%02X:%02X:%02X)",
           (unsigned long long)state->entity_id,
           (unsigned long long)state->entity_model_id,
           state->intf_hw_addr[0], state->intf_hw_addr[1], state->intf_hw_addr[2],
           state->intf_hw_addr[3], state->intf_hw_addr[4], state->intf_hw_addr[5]);

  /* Also enable all multicast reception as fallback */
  bool enable_multicast = true;
  int err = esp_eth_ioctl(eth_handle, ETH_CMD_S_ALL_MULTICAST, &enable_multicast);
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

/**
 * @brief Extract AM824 audio samples from 61883-IIDC packet
 *
 * IEC 61883-6 AM824 format:
 * - Each audio sample is 32 bits (4 bytes)
 * - Label (8 bits) + PCM data (24 bits, MSB aligned)
 * - For 48kHz stereo, typically 6 samples per channel per packet
 *
 * Packet structure (with VLAN):
 * - Ethernet header (14 bytes)
 * - VLAN tag (4 bytes)
 * - AVTP header (~24 bytes for 61883-IIDC)
 * - CIP header (8 bytes)
 * - Audio data (AM824 format)
 *
 * @param packet Raw packet buffer starting from Ethernet header
 * @param packet_len Total packet length
 * @param output_buffer Output buffer for 16-bit stereo PCM samples
 * @param output_samples Number of stereo sample pairs extracted
 */
static void extract_am824_audio(const u8* packet, size_t packet_len, int16_t* output_buffer, size_t* output_samples)
{
  *output_samples = 0;

  /* Parse VLAN-tagged frame structure:
   * Ethernet (14) + VLAN (4) = 18 bytes
   * Then AVTP 61883-IIDC header starts at offset 18
   */
  const size_t vlan_header_size = 18; // Eth + VLAN

  if (packet_len < vlan_header_size + 32) // Minimum: headers + some audio
  {
    return;
  }

  /* 61883-IIDC AVTP header (24 bytes):
   * 0: subtype (0x00)
   * 1: sv, version, mr, _r, tv
   * 2-3: sequence_num
   * 4-7: stream_id (upper 32 bits)
   * 8-11: avtp_timestamp
   * 12: gateway_info
   * 13-15: stream_data_length (in bytes)
   * 16-17: tag (upper 8), channel, tcode, sy
   * 18-23: stream_id (lower 48 bits)
   */
  const u8* avtp_header = packet + vlan_header_size;

  // Extract stream_data_length (includes CIP header + audio data)
  u16 stream_data_length = (avtp_header[13] << 16) | (avtp_header[14] << 8) | avtp_header[15];
  stream_data_length &= 0xFFFF; // Only lower 16 bits

  /* CIP header starts after AVTP header (24 bytes) */
  const size_t cip_offset = vlan_header_size + 24;

  if (packet_len < cip_offset + 8) // Need at least CIP header
  {
    return;
  }

  /* CIP header (8 bytes):
   * 0-1: SID, DBS, FN, QPC, SPH, _r
   * 2-5: DBC, FMT, FDF, SYT
   * 6-7: Additional format info
   *
   * For AM824: DBS = number of data blocks
   * (DBS field available but not used for basic stereo extraction)
   */

  /* Audio data starts after CIP header */
  const size_t audio_offset = cip_offset + 8;

  if (packet_len < audio_offset + 4)
  {
    return;
  }

  /* Calculate number of audio samples
   * For stereo AM824: each sample is 4 bytes, 2 channels = 8 bytes per stereo pair
   * Typical: 6 stereo pairs = 48 bytes of audio data
   */
  size_t audio_data_len = stream_data_length - 8; // Subtract CIP header
  size_t num_quadlets = audio_data_len / 4; // Each quadlet is 4 bytes

  const u8* audio_data = packet + audio_offset;
  size_t out_idx = 0;

  /* Extract samples (assuming 2-channel stereo AM824) */
  for (size_t i = 0; i < num_quadlets && (out_idx < 12); i += 2) // Process stereo pairs (6 samples = 12 channels)
  {
    size_t offset = i * 4;

    if (audio_offset + offset + 8 > packet_len)
    {
      break;
    }

    /* Extract left channel (first quadlet):
     * Byte 0: Label (typically 0x40 for valid audio)
     * Bytes 1-3: 24-bit PCM data (MSB first)
     */
    u8 label_l = audio_data[offset];

    // Only process if label indicates valid audio (0x40)
    if ((label_l & 0x40) == 0)
    {
      continue; // Skip invalid samples
    }

    // Extract 24-bit PCM and convert to 16-bit
    int32_t sample_l = (audio_data[offset + 1] << 16) |
      (audio_data[offset + 2] << 8) |
      audio_data[offset + 3];

    // Sign extend 24-bit to 32-bit
    if (sample_l & 0x800000)
    {
      sample_l |= 0xFF000000;
    }

    // Convert to 16-bit (right shift 8 bits)
    output_buffer[out_idx++] = (int16_t)(sample_l >> 8);

    /* Extract right channel (second quadlet) */
    if (i + 1 < num_quadlets)
    {
      u8 label_r = audio_data[offset + 4];

      if ((label_r & 0x40) == 0)
      {
        output_buffer[out_idx++] = 0; // Silence if invalid
        continue;
      }

      int32_t sample_r = (audio_data[offset + 5] << 16) |
        (audio_data[offset + 6] << 8) |
        audio_data[offset + 7];

      // Sign extend
      if (sample_r & 0x800000)
      {
        sample_r |= 0xFF000000;
      }

      output_buffer[out_idx++] = (int16_t)(sample_r >> 8);
    }
  }

  *output_samples = out_idx / 2; // Return number of stereo pairs
}


static int64_t timespec_to_ms(const struct timespec* ts)
{
  return ts->tv_sec * 1000 + (ts->tv_nsec / 1000000ll);
}

/**
 * @brief High-priority stream data task - runs on Core 1
 *
 * This task exclusively handles VLAN-tagged AVB stream packets.
 * It runs on a dedicated core with high priority to minimize packet loss.
 */
static void avtp_stream_task(void* arg)
{
  struct avtp_state_s* state = (struct avtp_state_s*)arg;
  u8 raw_buf[1518]; /* Max Ethernet frame size */
  u8 seq_number = 0;

  ESP_LOGI(TAG, "Stream task started on core %d (priority %d)",
           xPortGetCoreID(), uxTaskPriorityGet(NULL));

  /* Set VLAN socket to non-blocking mode */
  int flags = fcntl(state->vlan_socket, F_GETFL, 0);
  if (flags >= 0)
  {
    fcntl(state->vlan_socket, F_SETFL, flags | O_NONBLOCK);
  }

  while (!state->stop)
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
      int packets_processed = 0;

      /* Drain all available packets (non-blocking) */
      while ((len = read(state->vlan_socket, raw_buf, sizeof(raw_buf))) > 0)
      {
        packets_processed++;

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
                if (raw_buf[20] != seq_number)
                {
                  ESP_LOGW(TAG, "sequence number mismatch: expected=%u received=%u", seq_number, raw_buf[20]);
                  seq_number = raw_buf[20];
                }

                int16_t audio_samples[12];
                size_t num_samples = 0;

                extract_am824_audio(raw_buf, len, audio_samples, &num_samples);

                if (num_samples > 0)
                {
                  size_t bytes_to_write = num_samples * 2 * sizeof(int16_t);
                  audio_output_write(audio_samples, bytes_to_write);
                }
                seq_number++;
              }
              break;
            default:
              break;
            }
          }
        }
      }

      if (packets_processed > 0)
      {
        ESP_LOGD(TAG, "Stream task processed %d packets", packets_processed);
      }
    }
  }

  ESP_LOGI(TAG, "Stream task exiting");
  vTaskDelete(NULL);
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

static void avtp_listener_task(void* arg)
{
  const char* interface = "ETH_0";

  struct avtp_state_s* state = calloc(1, sizeof(struct avtp_state_s));

  if (arg != NULL)
  {
    interface = arg;
  }

  if (avtp_init_state(state, interface) != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to initialize AVTP state, exiting\n");
    free(state);
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "AVTP listener initialized on interface: %s", interface);

  /* Create high-priority stream task on Core 1 */
  TaskHandle_t stream_task_handle;
  BaseType_t ret = xTaskCreatePinnedToCore(
    avtp_stream_task,
    "AVTP_Stream",
    8192,
    state,
    STREAM_TASK_PRIORITY,
    &stream_task_handle,
    STREAM_TASK_CORE
  );

  if (ret != pdPASS)
  {
    ESP_LOGE(TAG, "Failed to create stream task");
    free(state);
    vTaskDelete(NULL);
    return;
  }

  /* Create control task on Core 0 */
  TaskHandle_t control_task_handle;
  ret = xTaskCreatePinnedToCore(
    avtp_control_task,
    "AVTP_Ctrl",
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
    free(state);
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "AVTP dual-core architecture started: Stream on Core %d, Control on Core %d",
           STREAM_TASK_CORE, CONTROL_TASK_CORE);

  /* This task can exit now, the stream and control tasks will run independently */
  vTaskDelete(NULL);
}


int start_avtp_listener(const char* interface)
{
  if (s_state == NULL)
  {
    /* Use higher priority for real-time packet processing */
    xTaskCreate(avtp_listener_task, "AVTP", 8192,
                (void*)interface, 10, NULL);
    return ESP_OK;
  }
  ESP_LOGE(TAG, "Other instance of AVTP is already running");
  return ESP_FAIL;
}

int stop_avtp_listener(int pid)
{
  s_state->stop = true;
  return ESP_OK;
}
