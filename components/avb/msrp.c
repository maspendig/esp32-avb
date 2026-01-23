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

struct msrp_domain* msrp_find_domain(struct msrp_ctx* ctx, msrp_pdu_domain_first_value_t* domain)
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
    msrp_pdu_domain_first_value_t* existing_domain = &list_entry->domain;
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

msrp_domain_t* msrp_create_domain(struct msrp_ctx* ctx, msrp_pdu_domain_first_value_t* domain)
{
  struct msrp_domain* new_domain = calloc(1, sizeof(msrp_domain_t));
  memcpy(&new_domain->domain, domain, sizeof(msrp_pdu_domain_first_value_t));
  list_append(ctx->domains, &new_domain->list);
  ctx->domain_count++;
  ESP_LOGI(TAG, "Created new MSRP domain: { id: %d, prio: %d, vid: %d }, total domains: %d",
           domain->sr_class_id,
           domain->sr_class_priority,
           ntohs(domain->sr_class_vid),
           ctx->domain_count);

  return new_domain;
}

void msrp_declare_domain(struct msrp_ctx* ctx, msrp_pdu_domain_first_value_t* domain, bool new)
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

static msrp_map_listener_t* msrp_map_find_listener(msrp_ctx_t* ctx, u64 stream_id)
{
  struct Node* head = ctx->map.listeners;
  struct Node* node = head->next;
  while (node != head)
  {
    msrp_map_listener_t* listener = (msrp_map_listener_t*)node;
    if (listener->value.stream_id == stream_id)
    {
      return listener;
    }
    node = node->next;
  }
  return NULL;
}

static void msrp_map_add_listener(msrp_ctx_t* ctx, msrp_listener_attr_value_t* value)
{
  if (msrp_map_find_listener(ctx, value->stream_id)) return;

  msrp_map_listener_t* node = calloc(1, sizeof(msrp_map_listener_t));
  node->value = *value;
  list_append(ctx->map.listeners, &node->list);
  ESP_LOGI(TAG, "Added listener to map, StreamID: 0x%llx", value->stream_id);
}

static void msrp_map_remove_listener(msrp_ctx_t* ctx, u64 stream_id)
{
  msrp_map_listener_t* node = msrp_map_find_listener(ctx, stream_id);
  if (node)
  {
    list_remove(&node->list);
    free(node);
    ESP_LOGI(TAG, "Removed listener from map, StreamID: 0x%llx", stream_id);
  }
}

msrp_stream_t* msrp_find_stream(msrp_ctx_t* ctx, u64 stream_id)
{
  struct Node* head = ctx->map.streams;
  struct Node* node = head;
  while (node->next != head)
  {
    msrp_stream_t* stream = (msrp_stream_t*)node->next;
    if (stream->first_value.stream_id == stream_id)
    {
      return stream;
    }
    node = node->next;
  }
  ESP_LOGI(TAG, "Stream 0x%016llX not found in map", ntohll(stream_id));
  return NULL;
}

msrp_stream_t* msrp_create_stream(msrp_ctx_t* ctx, u64 stream_id)
{
  msrp_stream_t* stream = calloc(1, sizeof(msrp_stream_t));

  stream = msrp_find_stream(ctx, stream_id);
  if (stream != NULL)
  {
    ESP_LOGI(TAG, "Stream 0x%016llX already exists in map", ntohll(stream_id));
    return stream;
  }

  stream = calloc(1, sizeof(msrp_stream_t));
  stream->first_value.stream_id = stream_id;
  ESP_LOGI(TAG, "Creating new stream in map: StreamID 0x%016llX", ntohll(stream->first_value.stream_id));
  list_append(ctx->map.streams, &stream->list);

  return stream;
}

ssize_t msrp_set_attribute_event(struct mrp_application* app, struct mrp_attribute* attr, u8 event, u8* buf)
{
  msrp_attribute_type_t type = attr->type;

  switch (type)
  {
  case MSRP_TALKER_ADVERTISE:
  case MSRP_DOMAIN:
  case MSRP_TALKER_FAILED:
    // write three packed attribute event to buffer
    buf[0] = mrp_encode_three_packed_event(event, 0, 0);

    return sizeof(u8);
  case MSRP_LISTENER:

    struct msrp_listener_attr_value listener = *(struct msrp_listener_attr_value*)attr->value;
    // write three packed attribute event to buffer
    buf[0] = mrp_encode_three_packed_event(event, 0, 0);
    // write four packed declaration type to buffer
    buf[1] = mrp_encode_four_packed_event(listener.declaration_type, 0, 0, 0);
    return sizeof(u16);
    break;

  default:
    ESP_LOGW(TAG, "set_attribute_event: unknown attribute type %d", type);
    return 0;
  }
}

/**
 * IEEE 802.1Q-2022 - 35.2.3.1.2 REGISTER_STREAM.indication
 * On receipt of a MAD_Join.indication service primitive (10.2, 10.3)
 * with an attribute_type of Talker Advertise, Talker Failed, or Talker Enhanced (35.2.2.4),
 * the MSRP application shall issue a REGISTER_STREAM.indication to the Listener application entity.
 */
void msrp_talker_advertise_join_indication(struct mrp_application* app, struct mrp_attribute* attribute, bool new)
{
  msrp_ctx_t* ctx = (msrp_ctx_t*)app->ctx;
  msrp_talker_advertise_attr_value_t* attr_value = (msrp_talker_advertise_attr_value_t*)attribute->value;
  ESP_LOGW(TAG, "MSRP Talker Advertise Join Indication: StreamID 0x%016llX, New: %d",
           ntohll(attr_value->stream_id),
           new);
  msrp_stream_t* stream = msrp_create_stream(ctx, attr_value->stream_id);
}

void msrp_talker_failed_join_indication(struct mrp_application* app, struct mrp_attribute* attribute, bool new)
{
  ESP_LOGW(TAG, "Talker Failed Join Indication");
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
  msrp_ctx_t* msrp = (msrp_ctx_t*)app->ctx;
  msrp_listener_attr_value_t* value = (msrp_listener_attr_value_t*)attribute->value;

  ESP_LOGI(TAG, "MSRP Listener Join Indication: StreamID 0x%llx, DeclType %d, New: %d",
           value->stream_id, value->declaration_type, new);

  msrp_map_add_listener(msrp, value);

  mrp_mad_join_request(app, MSRP_LISTENER, (u8*)value, new);
}

void msrp_domain_join_indication(struct mrp_application* app, struct mrp_attribute* attribute, bool new)
{
  msrp_pdu_domain_first_value_t* attr_value = (msrp_pdu_domain_first_value_t*)attribute->value;

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
    {
      msrp_listener_attr_value_t* val = (msrp_listener_attr_value_t*)attr->value;
      ESP_LOGI(TAG, "MSRP Listener Leave Indication: StreamID 0x%llx", val->stream_id);
      msrp_map_remove_listener((msrp_ctx_t*)app->ctx, val->stream_id);
      break;
    }
  case MSRP_DOMAIN:
    ESP_LOGW(TAG, "msrp_domain_leave_indication not implemented yet");
    break;
  default:
    ESP_LOGW(TAG, "MAD Leave Indication: unknown attribute type %d", attr->type);
    break;
  }
}

/**
 * Get the length of the MSRP attribute value based on the attribute type
 * differes from the attribute length field in the attribute header
 * as some attributes carries additional fields (e.g., declaration type for listener attribute)
 */
u8 msrp_get_attribute_length(u8 attribute_type)
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

/**
 * Get the length of the MSRP attribute value based on the attribute type
 * differes from the attribute length field in the attribute header
 * as some attributes carries additional fields (e.g., declaration type for listener attribute)
 */
u8 msrp_get_attribute_value_length(u8 attribute_type)
{
  switch (attribute_type)
  {
  case MSRP_TALKER_ADVERTISE:
    return sizeof(msrp_talker_advertise_attr_value_t);
  case MSRP_TALKER_FAILED:
    return sizeof(msrp_talker_failed_attr_value_t);
  case MSRP_LISTENER:
    return sizeof(msrp_listener_attr_value_t);
  case MSRP_DOMAIN:
    return sizeof(msrp_domain_attr_value_t);
  default:
    ESP_LOGW(TAG, "Unknown MSRP attribute type %d for value length retrieval", attribute_type);
    return 0;
  }
}

void msrp_tx_mrpdu(struct mrp_application* app, u8* buf, size_t len)
{
  msrp_ctx_t* ctx = app->ctx;
  struct avtp_state_s* avtp_state = ctx->state;

  if (len < 64)
  {
    // MSRP frames must be at least 64 bytes (including FCS)
    size_t padding = 64 - len;
    memset(buf + len, 0, padding);
    len += padding;
  }

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
  /* TODO check if receiving packages are of interest for this application */
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
          mrp_process_attribute(&state->msrp.app, list_entry, MRP_ATTRIBUTE_EVENT_LV);
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
        {
          msrp_pdu_talker_advertise_first_value_t* talker_adv = (msrp_pdu_talker_advertise_first_value_t*)(buf +
            first_value_pointer);
          mrp_decode_three_packed_event(buf[vector_end], three_packed);
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

          struct Node* head = state->msrp.app.attributes[MSRP_LISTENER];
          struct Node* node = head;
          while (node->next != head)
          {
            struct mrp_attribute* list_entry = (struct mrp_attribute*)node->next;
            msrp_listener_attr_value_t* listener = (msrp_listener_attr_value_t*)list_entry->value;
            if (listener->stream_id == talker_adv->stream_id)
            {
              ESP_LOGI(TAG, "  Found Listener for StreamID 0x%016llX in map, sending REGISTER_STREAM.indication",
                       ntohll(listener->stream_id));
              mrp_process_attribute(&state->msrp.app, list_entry, three_packed[0]);
            }
            node = node->next;
          }

          break;
        }
      case MSRP_TALKER_FAILED:
        // TODO implement processing of Talker Failed attribute
        break;
      case MSRP_DOMAIN:
        {
          struct msrp_pdu_domain_first_value* domain = (struct msrp_pdu_domain_first_value*)(buf + first_value_pointer);
          mrp_decode_three_packed_event(buf[vector_end], three_packed);
          //TODO use all three packed event values

          ESP_LOGI(TAG, " SR Class: {id: %d, prio: %d, vid: %d}, event: %s",
                   domain->sr_class_id,
                   domain->sr_class_priority,
                   ntohs(domain->sr_class_vid),
                   mrp_attribute_event_to_str(three_packed[0]));
          value = (u8*)domain;

          break;
        }
      case MSRP_LISTENER:
        {
          vector_length = attrib->attribute_length + sizeof(struct mrp_vector_header) + 2;
          msrp_listener_attr_value_t* listener = (msrp_listener_attr_value_t*)(buf + first_value_pointer);
          vector_end = vector_pointer + vector_length - 1;

          u8 declaration_type[4]; // four_packed
          mrp_decode_three_packed_event(buf[vector_end - 1], three_packed);
          mrp_decode_four_packed_event(buf[vector_end], declaration_type);
          ESP_LOGI(TAG, "  Listener Stream ID: 0x%016llX", ntohll(listener->stream_id));
          ESP_LOGI(TAG, "  Event: %s", mrp_attribute_event_to_str(three_packed[0]));
          ESP_LOGI(TAG, "  Declaration Type: %d", declaration_type[0]);
          value = (u8*)listener;
          break;
        }
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

      mrp_find_and_process_attribute(&state->msrp.app, attrib->attribute_type,
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

  msrp_listener_attr_value_t listener_attr = {
    .stream_id = htonll(stream_id),
    .declaration_type = MSRP_LISTENER_DECLARATION_READY
  };
  mrp_mad_join_request(app, MSRP_LISTENER, (u8*)&listener_attr, true);
}

/*
 * 32.2.3.1.1 REGISTER_STREAM.request
 * A Talker application entity shall issue a REGISTER_STREAM.request
 * to the MSRP Participant to initiate the advertisement of an available Stream.
 */
void msrp_register_stream_request(struct mrp_application* app, struct talker_stream_info_s* stream_info)
{
  msrp_talker_advertise_attr_value_t talker_adv_attr = {
    .stream_id = htonll(stream_info->stream_id),
    .vlan_id = htons(stream_info->stream_vlan_id),
    // TODO make configurable
    .max_frame_size = htons(224),
    .max_frame_interval = htons(1),
    .priority_and_rank = 0x70,
    .accumulated_latency = htonl(95)
  };
  memcpy(talker_adv_attr.dest_mac, stream_info->stream_dest_mac, ETH_ADDR_LEN);
  mrp_mad_join_request(app, MSRP_TALKER_ADVERTISE, (u8*)&talker_adv_attr, true);
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

  msrp->app.min_attribute_type = MSRP_TALKER_ADVERTISE;
  msrp->app.max_attribute_type = MSRP_DOMAIN;
  mrp_init(&msrp->app, MSRP);
  msrp->app.mad_join_indication = &msrp_mad_join_indication;
  msrp->app.mad_leave_indication = &msrp_mad_leave_indication;
  msrp->app.get_attribute_value_length = &msrp_get_attribute_value_length;
  msrp->app.get_attribute_length = &msrp_get_attribute_length;
  msrp->app.set_attribute_event = &msrp_set_attribute_event;
  msrp->app.uses_attribute_list_length = true;
  msrp->app.eth_type = ETH_TYPE_MSRP;
  msrp->app.tx_mrpdu = &msrp_tx_mrpdu;
  msrp->app.participant_type = FULL_P2P;
  msrp->app.ctx = msrp;
  memcpy(msrp->app.src_mac, state->intf_hw_addr, ETH_ADDR_LEN);

  struct Node* head = calloc(1, sizeof(struct Node));
  head->next = head;
  head->prev = head;
  msrp->domains = head;

  struct Node* map_head = calloc(1, sizeof(struct Node));
  map_head->next = map_head;
  map_head->prev = map_head;
  msrp->map.listeners = map_head;

  struct Node* stream_head = calloc(1, sizeof(struct Node));
  stream_head->next = stream_head;
  stream_head->prev = stream_head;
  msrp->map.streams = stream_head;

  msrp->domain_count = 0;
  msrp->state = state;

  // register domains

  msrp_pdu_domain_first_value_t default_domain = {
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

