#ifndef ESP32_AVB_AVTP_H
#define ESP32_AVB_AVTP_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "types.h"

#include <aecp.h>
#include <acmp.h>
#include <maap.h>
#include <msrp.h>
#include <mvrp.h>
#include <nvs.h>
#include "media_queue.h"

#define ETH_TYPE_AVTP 0x22F0
#define ETH_TYPE_8021Q 0x8100  /* 802.1Q VLAN tag */
#define MAX_ADP_ENTITIES 32

/* AVTP Stream subtypes (IEEE 1722-2016) */
#define AVTP_SUBTYPE_61883_IIDC  0x00
#define AVTP_SUBTYPE_AAF         0x02  /* AVTP Audio Format */
#define AVTP_SUBTYPE_CVF         0x03  /* Compressed Video Format */
#define AVTP_SUBTYPE_CRF         0x04  /* Clock Reference Format */
#define AVTP_SUBTYPE_TSCF        0x05  /* Time-Synchronous Control Format */
#define AVTP_SUBTYPE_NTSCF       0x82  /* Non-Time-Synchronous Control Format */

/* AVTP Control subtypes (IEEE 1722-2016) */
#define AVTP_SUBTYPE_ADP  0xFA
#define AVTP_SUBTYPE_AECP 0xFB
#define AVTP_SUBTYPE_ACMP 0xFC
#define AVTP_SUBTYPE_MAAP 0xFE

/* Masks for avtp_ctl byte */
#define AVTP_STREAMID_VALID_MASK  0x80 /* 8th bit */
#define AVTP_VERSION_MASK  0x70 /* bits 7..5 */
#define AVTP_MSGTYPE_MASK  0x0F /* bits 4..0 */


/* IEC 61883-6 CIP header format */
typedef struct iec61883
{
  u8 subtype;
  u8 avtp_info; /* contains sv, version, media clock restart, gateway info valid, timestamp valid */
  u8 sequence_num;
  u8 reserved : 7;
  u8 time_uncertain : 1;
  u64 stream_id;
  u32 avtp_timestamp;
  u32 gateway_info;
  u16 stream_data_length;
} __attribute__((packed)) iec61883_t;

/* AM824 audio sample format */
typedef struct am824_sample
{
  u8 label;
  u8 sample[3];
} __attribute__((packed)) am824_sample_t;

/* IEEE 1722-2016 5.4.3 IEC 61883 CIP header encapsulation */
typedef struct iec61883_cip_header
{
  u8 sid : 6; /* source id */
  u8 qi_1 : 2; /* quadlet indicator */
  u8 dbs; /* data block size */
  u8 fn : 1; /* fraction number */
  u8 qpc : 3; /* quadlet per channel */
  u8 sph : 1; /* source packet header */
  u8 dbc; /* data block count */
  u8 fmt : 6; /* format */
  u8 qi_2 : 2; /* quadlet indicator */
  u8 cip_fmt_specific_data[3]; /* format specific data */
} __attribute__((packed)) iec61883_cip_header_t;

struct avtp_discovery_msg_s
{
  struct header_s header;
  uint8_t subtype;
  /** AVTP control field containing
   * - Stream ID valid (bit 0)
   * - AVTP version (bits 1..4)
   * - Message type (bits 5..7)
   *
   * use AVTP_STREAMID_VALID_MASK, AVTP_VERSION_MASK, AVTP_MSGTYPE_MASK to extract values
   */
  uint8_t control;

  /** Control data length field containing valid_time (5 bits) and control_data_length (11 bits) */
  union
  {
    struct
    {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
      u16 control_data_length : 11; /* 11 bits for control data length */
      u16 valid_time : 5; /* 5 bits for valid time */
#else
      uint16_t valid_time : 5; /* 5 bits for valid time */
      uint16_t control_data_length : 11; /* 11 bits for control data length */
#endif
    } __attribute__((packed));

    u8 raw[2]; /* Raw bytes for network transmission */
    u16 raw_u16; /* Raw 16-bit value for easy manipulation */
  } control_data_length_field;

  u64 entity_id;
  u64 entity_model_id;
  u32 entity_capabilities;
  u16 talker_stream_sources;
  u16 talker_capabilities;
  u16 listener_stream_sinks;
  u16 listener_capabilities;
  u32 controller_capabilities;
  u32 available_index;
  u8 gptp_grandmaster_id[8];
  u64 association_id;
} __attribute__((packed));

struct avtp_header_s
{
  struct header_s header;
  u8 subtype;
};

/* Structure to hold discovered ADP entity information */
struct adp_entity_entry_s
{
  uint64_t entity_id;
  uint8_t mac[6];
  uint16_t talker_stream_sources;
  uint16_t talker_capabilities;
  uint16_t listener_stream_sinks;
  uint16_t listener_capabilities;
  uint32_t controller_capabilities;
  uint32_t available_index;
  time_t valid_until; // epoch seconds until this entry is valid
  bool in_use;
};

/* Structure to hold listener stream information for ACMP connections */
struct listener_stream_info_s
{
  u64 talker_entity_id;
  u64 talker_unique_id;
  bool connected;
  u64 stream_id;
  u16 sequence_id;
  u8 stream_dest_mac[6];
  u64 controller_entity_id;
  u16 flags;
  u16 stream_vlan_id;
  bool pending_connection;
};

#define MAX_LISTENER_STREAMS 16

/* Structure to hold a connected listener's information */
struct listener_pair_s
{
  u64 listener_entity_id;
  u16 listener_unique_id;
};

#define MAX_CONNECTED_LISTENERS 16

/* Structure to hold talker stream information for ACMP connections */
struct talker_stream_info_s
{
  u64 stream_id;
  u8 stream_dest_mac[6];
  u16 connection_count;
  struct listener_pair_s connected_listeners[MAX_CONNECTED_LISTENERS];
  u16 stream_vlan_id;
  u8 sequence_number;
  u8 cip_data_block_continuity;
};

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

struct avtp_state_s
{
  bool talker_stop;
  bool listener_stop;
  bool stop;
  int socket; /* Untagged AVTP frames (0x22F0) */
  int vlan_socket; /* VLAN-tagged frames (0x8100) for AVB streams */
  int msrp_socket;
  int mvrp_socket;

  nvs_handle_t nvs_handle;

  struct maap_db_s maap_db;

  uint16_t acmp_sequence_id;
  u8 intf_hw_addr[6];
  uint64_t entity_id;
  uint64_t entity_model_id;
  struct timespec last_transmitted_adp;
  uint32_t adp_available_index; // renamed from adp_availabe_index[4] for easier increment
  struct adp_entity_entry_s adp_entities[MAX_ADP_ENTITIES];
  bool connected;

  /* Listener stream information for ACMP connections */
  struct listener_stream_info_s listener_stream_infos[MAX_LISTENER_STREAMS];
  /* Talker stream information for ACMP connections (supporting one stream) */
  struct talker_stream_info_s talker_stream_info;

  msrp_ctx_t msrp;
  mvrp_ctx_t mvrp;

  /* Media queue for audio sample buffering and synchronized playback */
  media_queue_t media_queue;

  /* AECP entity acquisition state (IEEE 1722.1-2021, 7.4.1) */
  uint64_t acquired_by_controller_id; /* Entity ID of controller that acquired us (0 = not acquired) */
  bool acquire_persistent; /* Whether the acquisition is persistent */
};

// TODO move this content to the main
int start_avtp_listener(const char* interface);

int avtp_talker_start(struct avtp_state_s* state);
void avtp_talker_stop(struct avtp_state_s* state);

int avtp_listener_start(struct avtp_state_s* state);
void avtp_listener_stop(struct avtp_state_s* state);

#define AVTP_GET_STATUS(hdr) \
((ntohs((hdr)->control_data_len_status) >> 11) & 0x1F)

#define AVTP_GET_CTRL_DATA_LEN(hdr) \
(ntohs(hdr->control_data_len_status) & 0x7FF)

#define AVTP_SET_CTRL_DATA_STATUS(hdr, status, cdl) \
(hdr->control_data_len_status = htons(((status & 0x1F) << 11) | (cdl & 0x7FF)))


#endif //ESP32_AVB_AVTP_H
