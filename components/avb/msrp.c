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
#include <errno.h>

#define TAG "msrp"

char* msrp_attribute_type_to_str(msrp_attribute_type_t type)
{
  switch (type)
  {
  case MSRP_TALKER_ADVERTISE: return "Talker Advertise";
  case MSRP_TALKER_FAILED: return "Talker Failed";
  case MSRP_LISTENER: return "Listener";
  case MSRP_DOMAIN: return "Domain";
  default: return "UNKNOWN";
  }
}

bool validate_attribute_length(const msrp_attribute_t* attrib)
{
  switch (attrib->attribute_type)
  {
  case MSRP_TALKER_ADVERTISE: return attrib->attribute_length == MSRP_ATTRIBUTE_LENGTH_TALKER_ADVERTISE;
  case MSRP_TALKER_FAILED: return attrib->attribute_length == MSRP_ATTRIBUTE_LENGTH_TALKER_FAILED;
  case MSRP_LISTENER: return attrib->attribute_length == MSRP_ATTRIBUTE_LENGTH_LISTENER;
  case MSRP_DOMAIN: return attrib->attribute_length == MSRP_ATTRIBUTE_LENGTH_DOMAIN;
  default: return false;
  }
}

struct msrp_domain* msrp_find_domain(struct msrp_ctx* ctx, msrpdu_domain_t* domain)
{
  ESP_LOGI(TAG, "msrp_find_domain, searching for domain: { id: %d, prio: %d, vid: %d }",
           domain->sr_class_id,
           domain->sr_class_priority,
           ntohs(domain->sr_class_vid));
  struct Node* head = ctx->domains;
  struct Node* node = head;
  while (node->next != head)
  {
    struct msrp_domain* list_entry = (struct msrp_domain*)node->next;
    msrpdu_domain_t* existing_domain = &list_entry->domain;
    if (existing_domain->sr_class_id == domain->sr_class_id &&
      existing_domain->sr_class_priority == domain->sr_class_priority &&
      existing_domain->sr_class_vid == domain->sr_class_vid)
    {
      return list_entry;
    }
    node = node->next;
  }

  return NULL;
}

msrp_domain_t* msrp_create_domain(struct msrp_ctx* ctx, msrpdu_domain_t* domain)
{
  struct msrp_domain* new_domain = calloc(1, sizeof(msrp_domain_t));
  memcpy(&new_domain->domain, domain, sizeof(msrpdu_domain_t));
  list_append(ctx->domains, &new_domain->list);
  ctx->domain_count++;
  ESP_LOGI(TAG, "Created new MSRP domain: { id: %d, prio: %d, vid: %d }, total domains: %d",
           domain->sr_class_id,
           domain->sr_class_priority,
           ntohs(domain->sr_class_vid),
           ctx->domain_count);

  return new_domain;
}

void msrp_declare_domain(struct msrp_ctx* ctx, msrpdu_domain_t* domain, bool new)
{
  msrp_domain_t* existing_domain = msrp_find_domain(ctx, domain);

  if (existing_domain == NULL)
  {
    existing_domain = msrp_create_domain(ctx, domain);
  }

  ESP_LOGI(TAG, "Declaring MSRP domain: { id: %d, prio: %d, vid: %d }, %s",
           domain->sr_class_id,
           domain->sr_class_priority,
           ntohs(domain->sr_class_vid),
           new ? "new" : "join");
  mrp_mad_join_request(&ctx->app, MSRP_DOMAIN, (u8*)domain, new);
}

/**
 * IEEE 802.1Q-2022 - 35.2.3.1.2 REGISTER_STREAM.indication
 * On receipt of a MAD_Join.indication service primitive (10.2, 10.3)
 * with an attribute_type of Talker Advertise, Talker Failed, or Talker Enhanced (35.2.2.4),
 * the MSRP application shall issue a REGISTER_STREAM.indication to the Listener application entity.
 */
void msrp_talker_advertise_join_indication(struct mrp_application* app, struct mrp_attribute* attribute, bool new)
{
  msrpdu_talker_advertise_t* attr_value = (msrpdu_talker_advertise_t*)attribute->value;
  ESP_LOGI(TAG, "MSRP Talker Advertise Join Indication:");
  ESP_LOGI(TAG, "  Stream ID: 0x%012llX", ntohll(attr_value->stream_id));
  ESP_LOGI(TAG, "  Dest MAC: %02X:%02X:%02X:%02X:%02X:%02X",
           attr_value->dest_mac[0], attr_value->dest_mac[1], attr_value->dest_mac[2],
           attr_value->dest_mac[3], attr_value->dest_mac[4], attr_value->dest_mac[5]);
  ESP_LOGI(TAG, "  VLAN ID: %d", ntohs(attr_value->vlan_id));
  ESP_LOGI(TAG, "  Max Frame Size: %d", ntohs(attr_value->max_frame_size));
  ESP_LOGI(TAG, "  Max Frame Interval: %d", ntohs(attr_value->max_frame_interval));
  ESP_LOGI(TAG, "  Priority and Rank: 0x%02X", attr_value->priority_and_rank);
  ESP_LOGI(TAG, "  Accumulated Latency: %d", ntohl(attr_value->accumulated_latency));
}

void msrp_talker_failed_join_indication(struct mrp_application* app, struct mrp_attribute* attribute, bool new)
{
  ESP_LOGW(TAG, "msrp_talker_failed_join_indication not implemented yet");
}

/**
 * IEEE 802.1Q-2022 35.1.2.1
 * On receipt of a MAD_Join.indication for a Listener Declaration,
 * the Talker first merges (35.2.4.4.3) the Listener Declarations that it has registered for the same Stream.
 * Then the Talker examines the StreamID (35.2.2.8.2) and Declaration Type (35.2.1.3) of the merged Listener Declaration.
 * If the merged Listener Declaration is associated with a Stream that the Talker can supply,
 * and the DeclarationType is either Ready or Ready Failed (i.e., one or more Listeners can receive the Stream),
 * the Talker can start the transmission for this Stream immediately.
 * If the merged Listener Declaration is an Asking Failed,
 * the Talker shall stop the transmission for the Stream, if it is transmitting.
 */
void msrp_listener_join_indication(struct mrp_application* app, struct mrp_attribute* attribute, bool new)
{
  ESP_LOGW(TAG, "msrp_listener_join_indication not implemented yet");
}

void msrp_domain_join_indication(struct mrp_application* app, struct mrp_attribute* attribute, bool new)
{
  msrpdu_domain_t* attr_value = (msrpdu_domain_t*)attribute->value;

  ESP_LOGI(TAG, "MSRP Domain Join Indication: { id: %d, prio: %d, vid: %d }, %s",
           attr_value->sr_class_id,
           attr_value->sr_class_priority,
           ntohs(attr_value->sr_class_vid),
           new ? "new" : "join");
  msrp_declare_domain((msrp_ctx_t*)app->ctx, attr_value, new);
}

void msrp_mad_join_indication(struct mrp_application* app, struct mrp_attribute* attribute, bool new)
{
  switch (attribute->type)
  {
  case MSRP_TALKER_ADVERTISE:
    msrp_talker_advertise_join_indication(app, attribute, new);
    break;
  case MSRP_TALKER_FAILED:
    msrp_talker_failed_join_indication(app, attribute, new);
    break;
  case MSRP_LISTENER:
    msrp_listener_join_indication(app, attribute, new);
    break;
  case MSRP_DOMAIN:
    msrp_domain_join_indication(app, attribute, new);
    break;
  default:
    ESP_LOGW(TAG, "MAD Join Indication: unknown attribute type %d", attribute->type);
    break;
  }
}

void msrp_mad_leave_indication(struct mrp_application* app, struct mrp_attribute* attr)
{
  switch (attr->type)
  {
  case MSRP_TALKER_ADVERTISE:
    ESP_LOGW(TAG, "msrp_talker_advertise_leave_indication not implemented yet");
    break;
  case MSRP_TALKER_FAILED:
    ESP_LOGW(TAG, "msrp_talker_failed_leave_indication not implemented yet");
    break;
  case MSRP_LISTENER:
    // On receipt of a MAD_Leave.indication for a Listener Declaration, if the StreamID of the Declaration matches a Stream that the Talker is transmitting, then the Talker shall stop the transmission for this Stream, if it is transmitting.
    ESP_LOGW(TAG, "msrp_listener_leave_indication not implemented yet");
    break;
  case MSRP_DOMAIN:
    ESP_LOGW(TAG, "msrp_domain_leave_indication not implemented yet");
    break;
  default:
    ESP_LOGW(TAG, "MAD Leave Indication: unknown attribute type %d", attr->type);
    break;
  }
}

u8 msrp_get_attribute_value_length(u8 attribute_type)
{
  switch (attribute_type)
  {
  case MSRP_TALKER_ADVERTISE:
    return MSRP_ATTRIBUTE_LENGTH_TALKER_ADVERTISE;
  case MSRP_TALKER_FAILED:
    return MSRP_ATTRIBUTE_LENGTH_TALKER_FAILED;
  case MSRP_LISTENER:
    return MSRP_ATTRIBUTE_LENGTH_LISTENER;
  case MSRP_DOMAIN:
    return MSRP_ATTRIBUTE_LENGTH_DOMAIN;
  default:
    ESP_LOGW(TAG, "Unknown MSRP attribute type %d for value length retrieval", attribute_type);
    return 0;
  }
}

void mrsp_tx_mrpdu(struct mrp_application* app, u8* buf, size_t len)
{
  ESP_LOGI(TAG, "Transmitting MSRP MRPDU");
  ESP_LOG_BUFFER_HEX_LEVEL(TAG, buf, len, ESP_LOG_INFO);
  msrp_ctx_t* ctx = app->ctx;
  struct avtp_state_s* avtp_state = ctx->state;

  int result = write(avtp_state->msrp_socket, buf, len);
  if (result < 0)
  {
    ESP_LOGE(TAG, "Failed to send MSRP MRPDU: %s (%d)", strerror(errno), errno);
  }
  else
  {
    ESP_LOGI(TAG, "MSRP MRPDU sent successfully, %d bytes", result);
  }
}

void msrp_process_rx(struct avtp_state_s* state, const u8* buf, size_t len)
{
  struct msrp_packet_s
  {
    struct header_s header;
    u8 protocol_version;
  } __attribute__((packed));
  bool leave_all_seen[5] = {false, false, false, false, false};
  u16 attribute_pointer = sizeof(struct msrp_packet_s); // header + protocol_version
  u16 vector_length = 0, number_of_values;
  u8 three_packed[3];
  while (len > attribute_pointer + 1)
  {
    // check for end mark 0x0000
    if (buf[attribute_pointer] == 0x00 && buf[attribute_pointer + 1] == 0x00)
    {
      break;
    }

    const msrp_attribute_t* attrib = (msrp_attribute_t*)(buf + attribute_pointer);
    if (validate_attribute_length(attrib) == false)
    {
      ESP_LOGW(TAG, "Invalid MSRP attribute length: type %s[%d], length %d",
               msrp_attribute_type_to_str((msrp_attribute_type_t)attrib->attribute_type),
               attrib->attribute_type,
               attrib->attribute_length);
      break;
    }

    ESP_LOGI(TAG, "rx -> Attribute Type: %s, Attribute Length: %d, List Length: %d",
             msrp_attribute_type_to_str((msrp_attribute_type_t)attrib->attribute_type),
             attrib->attribute_length,
             ntohs(attrib->attribute_list_length));
    bool next_vector = true;
    u16 vector_pointer = attribute_pointer + sizeof(msrp_attribute_t);
    do
    {
      u8* value = 0;
      bool leave_all_event = false;
      u16* vector_header = (u16*)(buf + vector_pointer);
      mrp_parse_vector_header(ntohs(*vector_header), &leave_all_event, &number_of_values);

      if (leave_all_event == true && leave_all_seen[attrib->attribute_type] == false)
      {
        leave_all_seen[attrib->attribute_type] = true;
        ESP_LOGI(TAG, "MSRP LeaveAll Event received for attribute type %s",
                 msrp_attribute_type_to_str((msrp_attribute_type_t)attrib->attribute_type));

        struct Node* head = state->msrp.app.attributes[attrib->attribute_type];
        struct Node* node = head;
        while (node->next != head)
        {
          struct mrp_attribute* list_entry = (struct mrp_attribute*)node->next;
          // TODO add a function that takes the attribute directly, as we already have it here
          mrp_process_attribute_event(&state->msrp.app, attrib->attribute_type,
                                      list_entry->value,
                                      MRP_ATTRIBUTE_EVENT_LV);
          node = node->next;
        }
        mrp_leaveall_state_machine(&state->msrp.app, MRP_EVENT_R_LA);
      }

      if (number_of_values == 0)
      {
        // no values to process
        break;
      }

      vector_length = attrib->attribute_length + sizeof(struct mrp_vector_header) + 1;
      u16 first_value_pointer = vector_pointer + sizeof(mrp_vector_header_t);
      u16 vector_end = vector_pointer + vector_length - 1;
      switch (attrib->attribute_type)
      {
      case MSRP_TALKER_ADVERTISE:
        msrpdu_talker_advertise_t* talker_adv = (msrpdu_talker_advertise_t*)(buf + first_value_pointer);
        mrp_decode_four_packed_event(buf[vector_end], three_packed);
        ESP_LOGI(TAG, "  Talker Stream ID: 0x%016llX", ntohll(talker_adv->stream_id));
        ESP_LOGI(TAG, "  Dest MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 talker_adv->dest_mac[0], talker_adv->dest_mac[1], talker_adv->dest_mac[2],
                 talker_adv->dest_mac[3], talker_adv->dest_mac[4], talker_adv->dest_mac[5]);
        ESP_LOGI(TAG, "  VLAN ID: %d", ntohs(talker_adv->vlan_id));
        ESP_LOGI(TAG, "  Max Frame Size: %d", ntohs(talker_adv->max_frame_size));
        ESP_LOGI(TAG, "  Max Frame Interval: %d", ntohs(talker_adv->max_frame_interval));
        ESP_LOGI(TAG, "  Priority and Rank: 0x%02X", talker_adv->priority_and_rank);
        ESP_LOGI(TAG, "  Accumulated Latency: %lu", ntohl(talker_adv->accumulated_latency));
        ESP_LOGI(TAG, "  Event: %s",
                 mrp_attribute_event_to_str(three_packed[0]));
        value = (u8*)talker_adv;

        break;
      case MSRP_TALKER_FAILED:
        break;
      case MSRP_DOMAIN:
        struct msrpdu_domain* domain = (struct msrpdu_domain*)(buf + first_value_pointer);
        mrp_decode_three_packed_event(buf[vector_end], three_packed);
        //TODO use all three packed event values

        ESP_LOGI(TAG, " SR Class: {id: %d, prio: %d, vid: %d}, event: %s",
                 domain->sr_class_id,
                 domain->sr_class_priority,
                 ntohs(domain->sr_class_vid),
                 mrp_attribute_event_to_str(three_packed[0]));
        value = (u8*)domain;

        break;
      case MSRP_LISTENER:
        vector_length = attrib->attribute_length + sizeof(struct mrp_vector_header) + 2;
        msrpdu_listener_t* listener = (msrpdu_listener_t*)(buf + first_value_pointer);
        vector_end = vector_pointer + vector_length - 1;

        u8 declaration_type[4]; // four_packed
        mrp_decode_three_packed_event(buf[vector_end - 1], three_packed);
        mrp_decode_four_packed_event(buf[vector_end], declaration_type);
        ESP_LOGI(TAG, "  Listener Stream ID: 0x%016llX", ntohll(listener->stream_id));
        ESP_LOGI(TAG, "  Event: %s", mrp_attribute_event_to_str(three_packed[0]));
        ESP_LOGI(TAG, "  Declaration Type: %d", declaration_type[0]);
        value = (u8*)listener;
        break;
      default:
        ESP_LOGW(TAG, "Unknown MSRP attribute type: %d", attrib->attribute_type);
        break;
      }

      // check attribute list end mark
      if (buf[vector_end + 1] == 0x00 && buf[vector_end + 2] == 0x00)
      {
        next_vector = false;
      }

      // safety check
      if (len < vector_end + 1)
      {
        next_vector = false;
      }

      vector_pointer = vector_end + 1;

      ESP_LOGI(TAG, "Processing MSRP attribute event: type %s, event %s[%d],",
               msrp_attribute_type_to_str((msrp_attribute_type_t)attrib->attribute_type),
               mrp_attribute_event_to_str(three_packed[0])
      );

      mrp_process_attribute_event(&state->msrp.app, attrib->attribute_type,
                                  value,
                                  three_packed[0]);
    }
    while (next_vector);

    attribute_pointer += sizeof(msrp_attribute_t) + ntohs(attrib->attribute_list_length);
  }
}

void msrp_net_rx(struct avtp_state_s* state)
{
  u8 buf[128];

  ssize_t len = read(state->msrp_socket, buf, sizeof(buf));
  if (len <= 0)
  {
    return;
  }
  msrp_process_rx(state, buf, len);
}

/*
  35.2.3.1.5 REGISTER_ATTACH.request
  A Listener application entity shall issue a REGISTER_ATTACH.request
  to the MSRP Participant to request attachment to the referenced Stream.
 */
void msrp_register_attach_request(struct mrp_application* app, u64 stream_id)
{
  /*
   * On receipt of a REGISTER_ATTACH.request the MSRP Participant shall issue a MAD_Join.request service primitive (10.2, 10.3).
   * The attribute_type parameter of the request shall carry the appropriate Listener Attribute Type (35.2.2.4),
   * depending on neighborProtocolVersion. The attribute_value shall contain the StreamID and the Declaration Type.
   */

  mrp_mad_join_request(app, MSRP_LISTENER, (u8*)&stream_id, true);
}


void msrp_state_init(struct avtp_state_s* state)
{
  msrp_ctx_t* msrp = &state->msrp;
  if (msrp == NULL)
  {
    ESP_LOGE(TAG, "MSRP state pointer is NULL");
    return;
  }

  /* Initialize the MRP attribute structure */
  memset(msrp, 0, sizeof(msrp_ctx_t));

  mrp_init(&msrp->app, MSRP);
  msrp->app.mad_join_indication = &msrp_mad_join_indication;
  msrp->app.mad_leave_indication = &msrp_mad_leave_indication;
  msrp->app.get_attribute_value_length = &msrp_get_attribute_value_length;
  msrp->app.tx_mrpdu = &mrsp_tx_mrpdu;
  msrp->app.ctx = msrp;
  memcpy(msrp->app.src_mac, state->intf_hw_addr, ETH_ADDR_LEN);

  struct Node* head = calloc(1, sizeof(struct Node));
  head->next = head;
  head->prev = head;
  msrp->domains = head;
  msrp->domain_count = 0;
  msrp->state = state;

  // register domains

  msrpdu_domain_t default_domain = {
    .sr_class_id = MSRP_SR_CLASS_A,
    .sr_class_priority = MSRP_SR_CLASS_A_PRIO,
    // TODO move to config
    .sr_class_vid = htons(2)
  };

  msrp_declare_domain(msrp, &default_domain, true);

  ESP_LOGI(TAG, "MSRP state initialized");
}

int msrp_init_socket(const char* interface)
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

