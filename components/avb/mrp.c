//
// Created by max on 1/4/26.
//

#include "mrp.h"

#include <cc.h>

#include "common.h"

#include <esp_err.h>
#include <esp_eth_spec.h>
#include <esp_log.h>
#include <msrp.h>
#include <mvrp.h>
#include <types.h>

#define TAG "mrp"

//region string conversion helpers

char* mrp_application_type_to_str(mrp_application_type_t type)
{
  switch (type)
  {
  case MSRP: return "MSRP";
  case MVRP: return "MVRP";
  default: return "UNKNOWN";
  }
}

char* mrp_state_to_str(mrp_state_t state)
{
  switch (state)
  {
  case MRP_VO_STATE: return "VO";
  case MRP_VP_STATE: return "VP";
  case MRP_VN_STATE: return "VN";
  case MRP_AN_STATE: return "AN";
  case MRP_AA_STATE: return "AA";
  case MRP_QA_STATE: return "QA";
  case MRP_LA_STATE: return "LA";
  case MRP_AO_STATE: return "AO";
  case MRP_QO_STATE: return "QO";
  case MRP_AP_STATE: return "AP";
  case MRP_QP_STATE: return "QP";
  case MRP_LO_STATE: return "LO";

  // Registrar states
  case MRP_IN_STATE: return "IN";
  case MRP_LV_STATE: return "LV";
  case MRP_MT_STATE: return "MT";

  default: return "UNKNOWN";
  }
}

/* resolve protocol event states to 802.1Q-2018 10.7.1 nomenclature */
char* mrp_event_to_str(mrp_event_t event)
{
  switch (event)
  {
  case MRP_EVENT_BEGIN: return "Begin!";
  case MRP_EVENT_NEW: return "New!";
  case MRP_EVENT_JOIN: return "Join!";
  case MRP_EVENT_LV: return "Lv!";
  case MRP_EVENT_TX: return "tx!";
  case MRP_EVENT_TXLA: return "txLA!";
  case MRP_EVENT_TXLAF: return "txLAF!";
  case MRP_EVENT_R_NEW: return "rNew!";
  case MRP_EVENT_R_JOIN_IN: return "rJoinIn!";
  case MRP_EVENT_R_IN: return "rIn!";
  case MRP_EVENT_R_JOIN_MT: return "rJoinMt!";
  case MRP_EVENT_R_MT: return "rMt!";
  case MRP_EVENT_R_LV: return "rLv!";
  case MRP_EVENT_R_LA: return "rLa!";
  case MRP_EVENT_FLUSH: return "Flush!";
  case MRP_EVENT_REDECLARE: return "Re-Declare!";
  case MRP_EVENT_PERIODIC: return "periodic!";
  case MRP_EVENT_LEAVETIMER: return "leavetimer!";
  case MRP_EVENT_LEAVEALLTIMER: return "leavealltimer!";
  case MRP_EVENT_PERIODICTIMER: return "periodictimer!";
  default: return "UNKNOWN";
  }
}

char* mrp_action_to_str(mrp_action_t action)
{
  switch (action)
  {
  case MRP_ACTION_NONE: return "None";
  case MRP_ACTION_NEW: return "New";
  case MRP_ACTION_JOIN: return "Join";
  case MRP_ACTION_LV: return "Lv";
  case MRP_ACTION_S_N: return "sN";
  case MRP_ACTION_S_J: return "sJ";
  case MRP_ACTION_S_L: return "sL";
  case MRP_ACTION_S: return "s";
  case MRP_ACTION_S_LA: return "sLA";
  case MRP_ACTION_PERIODIC: return "periodic";
  case MRP_ACTION_LEAVETIMER: return "leavetimer";
  case MRP_ACTION_LEAVEALLTIMER: return "leavealltimer";
  case MRP_ACTION_PERIODICTIMER: return "periodictimer";
  default: return "UNKNOWN";
  }
}

char* mrp_active_state_to_str(mrp_active_state_t state)
{
  switch (state)
  {
  case MRP_ACTIVE: return "Active";
  case MRP_PASSIVE: return "Passive";
  default: return "UNKNOWN";
  }
}

char* mrp_attribute_event_to_str(mrp_attribute_event_t event)
{
  switch (event)
  {
  case MRP_ATTRIBUTE_EVENT_NEW: return "New";
  case MRP_ATTRIBUTE_EVENT_JOIN_IN: return "JoinIn";
  case MRP_ATTRIBUTE_EVENT_IN: return "In";
  case MRP_ATTRIBUTE_EVENT_JOIN_MT: return "JoinMt";
  case MRP_ATTRIBUTE_EVENT_MT: return "Mt";
  case MRP_ATTRIBUTE_EVENT_LV: return "Lv";
  default: return "UNKNOWN";
  }
}

//endregion

void mrp_delete_attribute(struct mrp_attribute* attr)
{
  struct Node* node = &attr->list;

  esp_timer_delete(attr->app->leaveall.timer);
  node->prev->next = node->next;
  node->next->prev = node->prev;
  free(attr);
}

void mrp_set_packet_header(struct mrp_application* app, struct header_s* header)
{
  if (app->type == MSRP)
  {
    memcpy(header->dst_mac, MSRP_MULTICAST_MAC, 6);
  }
  else
  {
    memcpy(header->dst_mac, MVRP_MULTICAST_MAC, 6);
  }

  memcpy(header->src_mac, app->src_mac, ETH_ADDR_LEN);
  header->eth_type[0] = (app->eth_type >> 8) & 0xFF;
  header->eth_type[1] = app->eth_type & 0xFF;
}

/**
 * Adds MRPDU data to the transmission queue for the given application
 * @param app MRP application instance
 * @param attr MRP attribute to be queued for transmission
 */
void mrp_queue_mrpdu(struct mrp_application* app, struct mrp_attribute* attr)
{
  ESP_LOGI(TAG, "Queuing MRPDU for attribute type %d", attr->type);
}

void mrp_free_attribute_check(struct mrp_application* app, struct mrp_attribute* attr)
{
  /* 802.1Q-2011, Table 10-3, Note 11 */
  if (((attr->applicant.state == MRP_VO_STATE) ||
      (attr->applicant.state == MRP_AO_STATE) ||
      (attr->applicant.state == MRP_QO_STATE)) &&
    (!(app->participant_type & FULL) || (attr->registrar.state == MRP_MT_STATE)))
    mrp_delete_attribute(attr);
}

s8 mrp_action2event(struct mrp_application* app, struct mrp_attribute* attr)
{
  struct mrp_applicant* applicant = &attr->applicant;
  struct mrp_registrar* registrar = &attr->registrar;
  s8 event;

  switch (applicant->action)
  {
  case MRP_ACTION_S_N:
    event = MRP_ATTRIBUTE_EVENT_NEW;
    break;

  case MRP_ACTION_S_J:
    event = MRP_ATTRIBUTE_EVENT_JOIN_MT;

    /*
     * 802.1Q-2022 - 10.7.6.3 sJ
     * If the registrar state is IN, send JoinIn
     * else send JoinMt
     */
    ESP_LOGI(TAG, "mrp_action2event: Checking for JoinIn condition. Registrar state: %s",
             mrp_state_to_str(registrar->state));
    if ((app->participant_type == FULL || app->participant_type == FULL_P2P) && registrar->state == MRP_IN_STATE)
    {
      event = MRP_ATTRIBUTE_EVENT_JOIN_IN;
    }
    break;
  case MRP_ACTION_S_L:
    event = MRP_ATTRIBUTE_EVENT_LV;
    break;

  case MRP_ACTION_S:
    event = MRP_ATTRIBUTE_EVENT_MT;
    /*
     * 802.1Q-2022 - 10.7.6.5 s, [s]
     * If the Registrar state is IN, then the AttributeEvent is In
     * If the Registrar state is MT or LV, then the AttributeEvent value Mt is encoded
     */
    if ((app->participant_type == FULL || app->participant_type == FULL_P2P) && registrar->state == MRP_IN_STATE)
    {
      event = MRP_ATTRIBUTE_EVENT_IN;
    }
    break;
  default:
    event = -1;
    break;
  }

  return event;
}

// TODO implement aggregation of multiple attributes into one MRPDU packet
void mrp_transmit(struct mrp_application* app)
{
  ESP_LOGI(TAG, "MRP Transmission triggered. leaveall: %s", app->send_leave_all ? "true" : "false");
  struct mrpdu_packet_s
  {
    struct header_s header;
    u8 protocol_version;
    u8 data[1500];
  } __attribute__((packed));

  for (u8 type = app->min_attribute_type; type <= app->max_attribute_type; type++)
  {
    u8 attribute_length = app->get_attribute_length(type);
    u8 attribute_value_length = app->get_attribute_value_length(type);


    struct Node* head = app->attributes[type];
    struct Node* node = head;

    struct mrpdu_packet_s packet = {0};
    u16 length = sizeof(struct header_s) + 1;
    mrp_set_packet_header(app, &packet.header);
    packet.protocol_version = 0;

    u8 vector_count = 0;

    while (node->next != head || (vector_count == 0 && app->send_leave_all))
    {
      struct mrp_attribute* attribute = node->next != head ? (struct mrp_attribute*)node->next : NULL;

      // number of values * value length + vector header + end mark
      u8 attribute_list_length = attribute_length + sizeof(u16) + sizeof(u16);

      u8 value[attribute_length];
      s8 event = 0;

      if (attribute)
      {
        /* regular attribute sending */
        event = mrp_action2event(app, attribute);
        if (attribute->applicant.tx == false || event < 0)
        {
          goto next;
        }

        attribute->applicant.tx = false;
        memcpy(value, attribute->value, attribute_value_length);
      }
      else if (app->send_leave_all)
      {
        memset(value, 0, attribute_length);
        // send leave all even if no attributes are present
      }
      u16* vector_header;
      if (app->uses_attribute_list_length == true)
      {
        struct mrp_data_unit_header* mrp_du_header = (struct mrp_data_unit_header*)packet.data;
        mrp_du_header->attribute_type = type;
        mrp_du_header->attribute_length = attribute_length;
        mrp_du_header->attribute_list_length = htons(attribute_list_length);
        vector_header = (u16*)(packet.data + sizeof(struct mrp_data_unit_header));
        length += sizeof(struct mrp_data_unit_header);
      }
      else
      {
        struct mvrp_data_unit_header* mvrp_du_header = (struct mvrp_data_unit_header*)packet.data;
        mvrp_du_header->attribute_type = type;
        mvrp_du_header->attribute_length = attribute_length;
        vector_header = (u16*)(packet.data + sizeof(struct mvrp_data_unit_header));
        length += sizeof(struct mvrp_data_unit_header);
      }

      // vector header
      *vector_header = htons(attribute ? 1 : 0); // no leave all, 1 value
      if (vector_count == 0 && app->send_leave_all == true)
      {
        *vector_header |= htons(0x2000); // set leave all bit
      }

      u8* value_pointer = (u8*)vector_header + sizeof(u16);
      length += sizeof(u16);

      memcpy(value_pointer, value, attribute_length);
      length += attribute_length;

      u8* event_pointer = value_pointer + attribute_length;

      if (attribute)
      {
        // let the application handle the event and declaration encoding
        const u8 event_len = app->set_attribute_event(app, attribute, event, event_pointer);

        event_pointer += event_len;
        length += event_len;

        // update attribute_list_length in MRPDU header
        if (app->uses_attribute_list_length == true)
        {
          attribute_list_length += event_len;
          struct mrp_data_unit_header* mrp_du_header = (struct mrp_data_unit_header*)packet.data;
          mrp_du_header->attribute_list_length = htons(attribute_list_length);
        }
      }

      length += 2 * sizeof(u16); // end marks

      app->tx_mrpdu(app, (u8*)&packet, length);

      vector_count++;
    next:
      node = node->next;
    }
  }
  app->send_leave_all = false;
  ESP_LOGI(TAG, "MRP Transmission completed");
}

//region join timer
/* Join timer per application */

int mrp_start_join_timer(const struct mrp_application* app)
{
  if (esp_timer_is_active(app->join_timer))
    return ESP_OK;

  ESP_LOGI(TAG, "Starting MRP Join Timer for application %s", mrp_application_type_to_str(app->type));
  // in order to fit the state machine definitions, we only use once timer here
  // the timer is restarted by the state machine
  return esp_timer_start_once(app->join_timer, MRP_JOIN_TIME_MS * 1000);
}

/*
 Request transmission of MRPDU
 alias of the mrp_start_join_timer
 */
int mrp_request_transmission(const struct mrp_application* app)
{
  return mrp_start_join_timer(app);
}

int mrp_stop_join_timer(const struct mrp_attribute* attr)
{
  return esp_timer_stop(attr->app->join_timer);
}


/**
 * Callback for join timer expiration
 * Offers a transmission opportunity to the applicant state machine
 */
void mrp_join_timer_callback(void* arg)
{
  ESP_LOGI(TAG, "MRP Join Timer expired, processing tx event");
  struct mrp_application* app = (struct mrp_application*)arg;
  mrp_event_t event = MRP_EVENT_TX;

  /* IEEE802.1q-2022 10.7.5.7
   * The tx! event is modified by the behavior of the LeaveAll state machine.
   * If the LeaveAll state machine has signaled LeaveAll, then tx! is modified to txLA!
   */
  mrp_leaveall_state_machine(app, MRP_EVENT_TX);
  if (app->leaveall.action == MRP_ACTION_S_LA)
  {
    event = MRP_EVENT_TXLA;
  }

  // loop through all attributes of all types
  for (u8 type = app->min_attribute_type; type <= app->max_attribute_type; type++)
  {
    struct Node* head = app->attributes[type];
    struct Node* node = head;
    while (node->next != head)
    {
      struct mrp_attribute* attribute = (struct mrp_attribute*)node->next;
      ESP_LOGI(TAG, "Processing MRP Join Timer tx event [loop: %d] [attribute type: %d]", type, attribute->type);
      mrp_applicant_state_machine(attribute, event);
      // mrp_delete_attribute(attribute);
      node = node->next;
    }
  }

  mrp_transmit(app);
}

int mrp_init_join_timer(const struct mrp_application* app)
{
  const esp_timer_create_args_t timer_args = {
    .callback = &mrp_join_timer_callback,
    .arg = (void*)app,
    .name = "mrp_join_timer"
  };
  return esp_timer_create(&timer_args, &app->join_timer);
}

//endregion

//region leaveall timer
/* IEEE802.1Q-2022 10.7.4.3 leavealltimer */
int mrp_start_leaveall_timer(const struct mrp_application* app)
{
  // LeaveAllTime < T < 1.5 x LeaveAllTime
  u32 interval = random_in_range(MRP_LEAVEALL_TIME_MS, MRP_LEAVEALL_TIME_MS * 1.5) * 1000;
  // in order to fit the state machine definitions, we only use once timer here
  // the timer is restarted by the state machine
  return esp_timer_start_once(app->leaveall.timer, interval);
}

int mrp_restart_leaveall_timer(const struct mrp_application* app)
{
  // LeaveAllTime < T < 1.5 x LeaveAllTime
  u32 interval = random_in_range(MRP_LEAVEALL_TIME_MS, MRP_LEAVEALL_TIME_MS * 1.5) * 1000;
  return esp_timer_restart(app->leaveall.timer, interval);
}

int mrp_stop_leaveall_timer(const struct mrp_attribute* attr)
{
  const struct mrp_application* app = attr->app;
  return esp_timer_stop(app->leaveall.timer);
}

void mrp_leaveall_timer_callback(void* arg)
{
  const struct mrp_application* app = (struct mrp_application*)arg;
  ESP_LOGI(TAG, "[app: %s] LeaveAll Timer expired, processing leavealltimer event",
           mrp_application_type_to_str(app->type));
  mrp_leaveall_state_machine(app, MRP_EVENT_LEAVEALLTIMER);
}

int mrp_init_leaveall_timer(const struct mrp_application* app)
{
  const esp_timer_create_args_t timer_args = {
    .callback = &mrp_leaveall_timer_callback,
    .arg = (void*)app,
    .name = "mrp_leaveall_timer"
  };
  return esp_timer_create(&timer_args, &app->leaveall.timer);
}

//endregion

//region leave timer
void mrp_leave_timer_callback(void* arg)
{
  struct mrp_attribute* attr = (struct mrp_attribute*)arg;
  mrp_registrar_state_machine(attr, MRP_EVENT_LEAVETIMER);
}

int mrp_init_leave_timer(struct mrp_attribute* attr)
{
  esp_timer_create_args_t timer_args = {
    .callback = &mrp_leave_timer_callback,
    .arg = (void*)attr,
    .name = "mrp_leave_timer"
  };
  struct mrp_registrar* registrar = &attr->registrar;
  return esp_timer_create(&timer_args, &registrar->leave_timer);
}

int mrp_start_leave_timer(const struct mrp_attribute* attr)
{
  const struct mrp_registrar* registrar = &attr->registrar;
  // in order to fit the state machine definitions, we only use once timer here
  // the timer is restarted by the state machine
  return esp_timer_start_once(registrar->leave_timer, MRP_LEAVE_TIME_MS * 1000);
}

int mrp_stop_leave_timer(const struct mrp_attribute* attr)
{
  const struct mrp_registrar* registrar = &attr->registrar;
  return esp_timer_stop(registrar->leave_timer);
}

//endregion

//region periodic timer
void mrp_periodic_timer_callback(void* arg)
{
  ESP_LOGI(TAG, "[app: %s] Periodic Timer expired, processing periodictimer event",
           mrp_application_type_to_str(((struct mrp_application*)arg)->type));
  const struct mrp_application* app = (struct mrp_application*)arg;
  for (u8 type = app->min_attribute_type; type <= app->max_attribute_type; type++)
  {
    struct Node* head = app->attributes[type];
    struct Node* node = head;
    while (node->next != head)
    {
      struct mrp_attribute* attribute = (struct mrp_attribute*)node->next;
      mrp_applicant_state_machine(attribute, MRP_EVENT_PERIODICTIMER);
      node = node->next;
    }
  }
}

int mrp_init_periodic_timer(const struct mrp_application* app)
{
  const esp_timer_create_args_t timer_args = {
    .callback = &mrp_periodic_timer_callback,
    .arg = (void*)app,
    .name = "mrp_periodic_timer"
  };
  return esp_timer_create(&timer_args, &app->periodic_timer);
}

void mrp_stop_periodic_timer(const struct mrp_application* app)
{
  esp_timer_stop(app->periodic_timer);
}

/* Restart or start periodic timer */
void mrp_start_periodic_timer(const struct mrp_application* app)
{
  if (esp_timer_is_active(app->periodic_timer))
  {
    esp_timer_restart(app->periodic_timer, MRP_PERIODIC_TIME_MS * 1000); // us
  }
  else
  {
    esp_timer_start_periodic(app->periodic_timer, MRP_PERIODIC_TIME_MS * 1000); // us
  }
}

bool mrp_is_periodic_timer_active(const struct mrp_application* app)
{
  return esp_timer_is_active(app->periodic_timer);
}

//endregion

int mrp_init_timers(struct mrp_application* app)
{
  int ret = ESP_OK;
  ret += mrp_init_leaveall_timer(app);
  ret += mrp_init_join_timer(app);

  if (app->type != MSRP)
  {
    ret += mrp_init_periodic_timer(app);
  }
  return ret;
}

void mrp_stop_timers(const struct mrp_application* app)
{
  esp_timer_stop(app->leaveall.timer);
  esp_timer_stop(app->join_timer);
  esp_timer_stop(app->periodic_timer);
}

void mrp_delete_timers(const struct mrp_application* app)
{
  esp_timer_delete(app->leaveall.timer);
  esp_timer_delete(app->join_timer);
  esp_timer_delete(app->periodic_timer);
}

void mrp_parse_vector_header(u16 vector_header, bool* leave_all_event, u16* number_of_values)
{
  *leave_all_event = (vector_header >> 13) & 0x1;
  *number_of_values = vector_header & 0x1FFF;
}

/* IEEE 802.1Q-2022 Table 10-3 Note 6:
 * Request opportunity to transmit on entry to VN, AN, AA, LA, VP, AP, and LO states
 */
bool is_state_requesting_transmit(const mrp_state_t state)
{
  switch (state)
  {
  case MRP_VN_STATE:
  case MRP_AN_STATE:
  case MRP_AA_STATE:
  case MRP_LA_STATE:
  case MRP_VP_STATE:
  case MRP_AP_STATE:
  case MRP_LO_STATE:
    return true;
  default:
    return false;
  }
}

//region state machines

void mrp_applicant_state_machine(struct mrp_attribute* attr, mrp_event_t event)
{
  mrp_action_t action = MRP_ACTION_NONE;
  struct mrp_applicant* applicant = &attr->applicant;
  mrp_state_t state = applicant->state;

  switch (event)
  {
  case MRP_EVENT_BEGIN:
    // Initial state
    state = MRP_VO_STATE;
    action = MRP_ACTION_NONE;
    break;
  case MRP_EVENT_NEW:
    switch (state)
    {
    case MRP_VN_STATE:
    case MRP_AN_STATE:
      break;
    default:
      state = MRP_VN_STATE;
      break;
    }
    break;
  case MRP_EVENT_JOIN:
    switch (state)
    {
    case MRP_VO_STATE:
    case MRP_LO_STATE:
      state = MRP_VP_STATE;
      break;
    case MRP_LA_STATE:
      state = MRP_AA_STATE;
      break;
    case MRP_AO_STATE:
      state = MRP_AP_STATE;
      break;
    case MRP_QO_STATE:
      state = MRP_QP_STATE;
      break;
    default:
      break;
    }
    break;
  case MRP_EVENT_LV:
    switch (state)
    {
    case MRP_VP_STATE:
      state = MRP_VO_STATE;
      break;
    case MRP_VN_STATE:
    case MRP_AN_STATE:
    case MRP_AA_STATE:
    case MRP_QA_STATE:
      state = MRP_LA_STATE;
      break;
    case MRP_AP_STATE:
      state = MRP_AO_STATE;
      break;
    case MRP_QP_STATE:
      state = MRP_QO_STATE;
      break;
    default: break;
    }
    break;
  case MRP_EVENT_R_NEW:
    break;
  case MRP_EVENT_R_JOIN_IN:
    switch (state)
    {
    case MRP_VO_STATE:
      /* IEEE 802.1Q-2022 10-3 Note 4 Ignored if point-to-point subset */
      if (!(attr->app->participant_type & FULL_P2P ||
        attr->app->participant_type & APPLICANT_ONLY_P2P))
      {
        state = MRP_AO_STATE;
      }
      break;
    case MRP_VP_STATE:
      /* IEEE 802.1Q-2022 10-3 Note 4 Ignored if point-to-point subset */
      if (!(attr->app->participant_type & FULL_P2P ||
        attr->app->participant_type & APPLICANT_ONLY_P2P))
      {
        state = MRP_AP_STATE;
      }
      break;
    case MRP_AA_STATE:
      state = MRP_QA_STATE;
      break;
    case MRP_AO_STATE:
      state = MRP_QO_STATE;
      break;
    case MRP_AP_STATE:
      state = MRP_QP_STATE;
      break;
    default: break;
    }
    break;
  case MRP_EVENT_R_IN:
    switch (state)
    {
    case MRP_AA_STATE:
      state = MRP_QA_STATE;
      break;
    default: break;
    }
    break;
  case MRP_EVENT_R_JOIN_MT:
  case MRP_EVENT_R_MT:
    switch (state)
    {
    case MRP_QA_STATE:
      state = MRP_AA_STATE;
      break;
    case MRP_QO_STATE:
      state = MRP_AO_STATE;
      break;
    case MRP_QP_STATE:
      state = MRP_AP_STATE;
      break;
    case MRP_LO_STATE:
      state = MRP_VO_STATE;
      break;
    default: break;
    }
    break;
  case MRP_EVENT_R_LV:
  case MRP_EVENT_R_LA:
  case MRP_EVENT_REDECLARE:
    switch (state)
    {
    case MRP_VO_STATE:
    case MRP_AO_STATE:
    case MRP_QO_STATE:
      state = MRP_LO_STATE;
      break;
    case MRP_AN_STATE:
      state = MRP_VN_STATE;
      break;
    case MRP_AA_STATE:
    case MRP_QA_STATE:
    case MRP_AP_STATE:
    case MRP_QP_STATE:
      state = MRP_VP_STATE;
      break;
    default: break;
    }
    break;
  case MRP_EVENT_PERIODIC:
    switch (state)
    {
    case MRP_QA_STATE:
      state = MRP_AA_STATE;
      break;
    case MRP_QP_STATE:
      state = MRP_AP_STATE;
      break;
    default: break;
    }
    break;
  case MRP_EVENT_TX:
    switch (state)
    {
    case MRP_VP_STATE:
      state = MRP_AA_STATE;
      action = MRP_ACTION_S_J;
      break;
    case MRP_VN_STATE:
      state = MRP_AN_STATE;
      action = MRP_ACTION_S_N;
      break;
    case MRP_AN_STATE:
      state = MRP_QA_STATE;
      action = MRP_ACTION_S_N;
      break;
    case MRP_AA_STATE:
    case MRP_AP_STATE:
      state = MRP_QA_STATE;
      action = MRP_ACTION_S_J;
      break;
    case MRP_LA_STATE:
      state = MRP_VO_STATE;
      action = MRP_ACTION_S_L;
      break;
    case MRP_LO_STATE:
      state = MRP_VO_STATE;
      action = MRP_ACTION_S;
    default: break;
    }
    break;
  case MRP_EVENT_TXLA:
    switch (state)
    {
    case MRP_VO_STATE:
    case MRP_AO_STATE:
    case MRP_QO_STATE:
      // action = [s];
      break;
    case MRP_VP_STATE:
      state = MRP_AA_STATE;
      action = MRP_ACTION_S_J;
      break;
    case MRP_VN_STATE:
      state = MRP_AN_STATE;
      action = MRP_ACTION_S_N;
      break;
    case MRP_AN_STATE:
      state = MRP_QA_STATE;
      action = MRP_ACTION_S_N;
      break;
    case MRP_AA_STATE:
      state = MRP_QA_STATE;
      action = MRP_ACTION_S_J;
      break;
    case MRP_QA_STATE:
      // action = [sJ];
      break;
    case MRP_LA_STATE:
      state = MRP_VO_STATE;
      action = MRP_ACTION_S_L;
      break;
    case MRP_AP_STATE:
      state = MRP_QA_STATE;
      action = MRP_ACTION_S_J;
      break;
    case MRP_LO_STATE:
      state = MRP_VO_STATE;
      action = MRP_ACTION_S;
      break;
    default: break;
    }
    break;
  case MRP_EVENT_TXLAF:
    switch (state)
    {
    case MRP_VO_STATE:
    case MRP_LA_STATE:
    case MRP_AO_STATE:
    case MRP_QO_STATE:
      state = MRP_LO_STATE;
      break;
    case MRP_VP_STATE:
    case MRP_AA_STATE:
    case MRP_QA_STATE:
    case MRP_AP_STATE:
    case MRP_QP_STATE:
      state = MRP_VP_STATE;
      break;
    case MRP_VN_STATE:
    case MRP_AN_STATE:
      state = MRP_VN_STATE;
      break;
    default: break;
    }
    break;
  default:
    ESP_LOGI(TAG, "applicant sm: Unknown event %d", event);
  }

  /* 802.1Q-2022 10-3 Note
   * Point-to-point subset participants do not transition to AO, QO, AP, QP states
   */
  if ((attr->app->participant_type == FULL_P2P ||
      attr->app->participant_type == APPLICANT_ONLY_P2P
    )
    &&
    (state == MRP_AO_STATE ||
      state == MRP_QO_STATE ||
      state == MRP_AP_STATE ||
      state == MRP_QP_STATE)
  )
  {
    return;
  }


  ESP_LOGI(TAG, "Applicant SM - event: %s, state [%s] => [%s], action %s",
           mrp_event_to_str(event),
           mrp_state_to_str(applicant->state),
           mrp_state_to_str(state),
           mrp_action_to_str(action));

  /* IEEE 802.1Q-2022 Table 10-3 Note 6:
   * Request opportunity to transmit on entry to VN, AN, AA, LA, VP, AP, and LO states
   */
  if (state != applicant->state && is_state_requesting_transmit(state))
  {
    /* Whenever a state machine transitions to a state that requires transmission of a message,
       a transmit opportunity is requested if one is not already pending */
    mrp_request_transmission(attr->app);
  }

  applicant->state = state;
  applicant->action = action;
  applicant->tx = action != MRP_ACTION_NONE;
}

void mrp_registrar_exec_action(struct mrp_attribute* attr)
{
  switch (attr->registrar.action)
  {
  case MRP_ACTION_NEW:
    /* IEEE 802.1Q-2022 10.7.6.12 New */
    attr->app->mad_join_indication(attr->app, attr, true);
    break;
  case MRP_ACTION_JOIN:
    /* IEEE 802.1Q-2022 10.7.6.13 Join */
    attr->app->mad_join_indication(attr->app, attr, false);
    break;
  case MRP_ACTION_LV:
    /* IEEE 802.1Q-2022 Lv */
    attr->app->mad_leave_indication(attr->app, attr);
    break;
  default:
    return;
  }

  ESP_LOGI(TAG, "Registrar action %s for attribute type %d",
           mrp_action_to_str(attr->registrar.action),
           attr->type);
}

void mrp_registrar_state_machine(struct mrp_attribute* attr, mrp_event_t event)
{
  struct mrp_registrar* registrar = &attr->registrar;
  mrp_state_t state = registrar->state;
  mrp_action_t action = MRP_ACTION_NONE;
  switch (event)
  {
  case MRP_EVENT_BEGIN:
    state = MRP_MT_STATE;
    break;
  case MRP_EVENT_R_NEW:
    if (state == MRP_LV_STATE)
    {
      mrp_stop_leave_timer(attr);
    }
    state = MRP_IN_STATE;
    action = MRP_ACTION_NEW;
    break;
  case MRP_EVENT_R_JOIN_IN:
  case MRP_EVENT_R_JOIN_MT:
    if (state == MRP_LV_STATE)
    {
      mrp_stop_leave_timer(attr);
    }
    else if (state == MRP_MT_STATE)
    {
      action = MRP_ACTION_JOIN;
    }
    state = MRP_IN_STATE;
    break;
  case MRP_EVENT_R_LV:
  case MRP_EVENT_R_LA:
  case MRP_EVENT_TXLA:
  case MRP_EVENT_REDECLARE:
    if (state == MRP_IN_STATE)
    {
      mrp_start_leave_timer(attr);
      state = MRP_LV_STATE;
    }
    break;
  case MRP_EVENT_FLUSH:
    if (state == MRP_IN_STATE || state == MRP_LV_STATE)
    {
      action = MRP_ACTION_LV;
    }
    state = MRP_MT_STATE;
    break;
  case MRP_EVENT_LEAVETIMER:
    // TODO investigate operPointToPointMAC condition
    if (state == MRP_LV_STATE)
    {
      state = MRP_MT_STATE;
      action = MRP_ACTION_LV;
    }
    else if (state == MRP_MT_STATE)
    {
      state = MRP_MT_STATE;
    }
    break;
  default:
    ESP_LOGW(TAG, "registrar sm: Unknown event %d", event);
    break;
  }

  if (attr->type == MSRP_LISTENER)
    ESP_LOGI(TAG, "Registrar SM - event: %s, state [%s] => [%s], action %s",
           mrp_event_to_str(event),
           mrp_state_to_str(registrar->state),
           mrp_state_to_str(state),
           mrp_action_to_str(action));

  registrar->state = state;
  registrar->action = action;

  mrp_registrar_exec_action(attr);
}

void mrp_leaveall_exec_action(struct mrp_application* app)
{
  /* IEEE 802.1Q-2022 - 10.7.9 LeaveAll state machine
   * When the LeaveAll state machine signals LeaveAll (action S_LA),
   * all applicant and registrar state machines of that participant are notified of the LeaveAll event.
   */
  if (app->leaveall.action == MRP_ACTION_S_LA)
  {
    ESP_LOGI(TAG, "[app: %s] LeaveAll action S_LA executed, notifying all applicant and registrar state machines",
             mrp_application_type_to_str(app->type));
    // loop through all attributes of all types
    for (u8 type = app->min_attribute_type; type <= app->max_attribute_type; type++)
    {
      struct Node* head = app->attributes[type];
      struct Node* node = head;
      while (node->next != head)
      {
        struct mrp_attribute* attribute = (struct mrp_attribute*)node->next;
        ESP_LOGI(TAG, "Notifying applicant and registrar SMs of LeaveAll event [loop: %d] [attribute type: %d]", type,
                 attribute->type);
        mrp_applicant_state_machine(attribute, MRP_EVENT_R_LA);
        mrp_registrar_state_machine(attribute, MRP_EVENT_R_LA);
        node = node->next;
      }
    }
    app->send_leave_all = true;
  }
}

void mrp_leaveall_state_machine(const struct mrp_application* app, mrp_event_t event)
{
  struct mrp_leaveall* leaveall = &app->leaveall;
  mrp_active_state_t state = leaveall->state;
  mrp_action_t action = MRP_ACTION_NONE;
  switch (event)
  {
  case MRP_EVENT_BEGIN:
    state = MRP_PASSIVE;
    mrp_start_leaveall_timer(app);
    break;
  case MRP_EVENT_TX:
    if (state == MRP_ACTIVE)
    {
      state = MRP_PASSIVE;
      action = MRP_ACTION_S_LA;
    }
    break;
  case MRP_EVENT_R_LA:
    state = MRP_PASSIVE;
    // IEEE 802.1Q-2022 10.6: on LeaveAll message from another Participant, reset timer to minimize network traffic
    mrp_restart_leaveall_timer(app);
    break;
  case MRP_EVENT_LEAVEALLTIMER:
    state = MRP_ACTIVE;
    mrp_start_leaveall_timer(app);

    break;
  default:
    ESP_LOGW(TAG, "Unknown event %s for leave all state machine!", mrp_event_to_str(event));
  }
  ESP_LOGI(TAG, "LeaveAll SM - event: %s, state: [%s] => [%s], action: %s",
           mrp_event_to_str(event),
           mrp_active_state_to_str(leaveall->state),
           mrp_active_state_to_str(state),
           mrp_action_to_str(action));

  /* IEEE 802.1Q-2022 Table 10-5 Note a:
   * Request opportunity to transmit on entry to the active state
   */
  if (leaveall->state == MRP_PASSIVE && state == MRP_ACTIVE)
  {
    mrp_request_transmission(app);
    ESP_LOGI(TAG, "LeaveAll SM requesting transmission opportunity");
  }

  leaveall->state = state;
  leaveall->action = action;

  mrp_leaveall_exec_action(app);
}

//endregion

struct mrp_attribute* mrp_get_attribute(struct mrp_application* app, u8 attribute_type, u8* value)
{
  ESP_LOGI(TAG, "[app: %s] Searching for attribute type %d",
           mrp_application_type_to_str(app->type),
           attribute_type);
  const u8 length = app->get_attribute_value_length(attribute_type);
  struct Node* head = app->attributes[attribute_type];
  struct Node* temp = head;
  if (temp->next == head)
  {
    ESP_LOGI(TAG, "Attribute list for type %d is empty", attribute_type);
    return NULL;
  }
  // traverse the linked list, if next match the current node we reached the end
  while (temp->next != head)
  {
    const struct mrp_attribute* attribute = (struct mrp_attribute*)temp->next;
    if (memcmp(attribute->value, value, length) == 0)
    {
      return (struct mrp_attribute*)temp->next;
    }
    temp = temp->next;
  }

  ESP_LOGD(TAG, "Attribute type %d with given value not found", attribute_type);

  return NULL;
}


struct mrp_attribute* mrp_create_attribute(struct mrp_application* app, u8 attribute_type, u8* value)
{
  ESP_LOGI(TAG, "Creating new attribute for type %d", attribute_type);
  const u8 attribute_value_length = app->get_attribute_value_length(attribute_type);
  struct mrp_attribute* attr = calloc(1, sizeof(struct mrp_attribute) + attribute_value_length);
  attr->app = app;
  attr->type = attribute_type;
  memcpy(attr->value, value, attribute_value_length);

  list_append(app->attributes[attribute_type], &attr->list);

  // initialize leave timer
  mrp_init_leave_timer(attr);

  mrp_applicant_state_machine(attr, MRP_EVENT_BEGIN);
  mrp_registrar_state_machine(attr, MRP_EVENT_BEGIN);

  return attr;
}

struct mrp_attribute* mrp_get_or_create_attribute(struct mrp_application* app, u8 attribute_type, u8* value)
{
  struct mrp_attribute* attr = mrp_get_attribute(app, attribute_type, value);
  if (attr == NULL)
  {
    ESP_LOGI(TAG, "Attribute type %d not found, creating new one", attribute_type);
    attr = mrp_create_attribute(app, attribute_type, value);
  }
  else
  {
    ESP_LOGI(TAG, "Attribute type %d found", attr->type);
  }
  return attr;
}

void mrp_mad_join_request(struct mrp_application* app, u8 attribute_type, u8* value, bool new)
{
  ESP_LOGI(TAG, "[app: %s] MAD Join Request for attribute type %d, new: %d",
           mrp_application_type_to_str(app->type),
           attribute_type,
           new);
  struct mrp_attribute* attribute;

  attribute = mrp_get_or_create_attribute(app, attribute_type, value);
  if (new)
  {
    mrp_applicant_state_machine(attribute, MRP_EVENT_NEW);
  }
  else
  {
    mrp_applicant_state_machine(attribute, MRP_EVENT_JOIN);
  }
}

void mrp_process_attribute(struct mrp_application* app, struct mrp_attribute* attr, mrp_attribute_event_t event)
{
  mrp_registrar_state_machine(attr, mrp_attribute_event_to_event_map[event]);
  mrp_applicant_state_machine(attr, mrp_attribute_event_to_event_map[event]);

  /* IEEE 802.1Q-2022 -
    * Table 10-3 Note 11:
    * An attribute is deleted when both the applicant and registrar state machines
    * reach one of the states VO, AO, or QO, and either the participant is not a Full Participant
    * or the registrar state machine is in state MT.
    */
  mrp_free_attribute_check(app, attr);
}

void mrp_find_and_process_attribute(struct mrp_application* app, u8 type, u8* value, mrp_attribute_event_t event)
{
  struct mrp_attribute* attr;
  attr = mrp_get_or_create_attribute(app, type, value);

  mrp_process_attribute(app, attr, event);
}

void mrp_enable(struct mrp_application* app)
{
  // TODO check participant type
  // if(attr->app->participant_type == MRP_PARTICIPANT_FULL)
  {
    mrp_leaveall_state_machine(app, MRP_EVENT_BEGIN);
  }
  if (app->type != MSRP)
  {
    mrp_start_periodic_timer(app);
  }
  app->enabled = true;
}

int mrp_init(struct mrp_application* app, mrp_application_type_t type)
{
  ESP_LOGI(TAG, "Initializing MRP-%s...", mrp_application_type_to_str(type));

  app->enabled = false;
  app->type = type;
  memset(app->attributes, 0, sizeof(app->attributes));

  if (mrp_init_timers(app) > ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to initialize MRP timers");
    return ESP_FAIL;
  }

  for (int i = 0; i <= app->max_attribute_type; i++)
  {
    struct Node* head = calloc(1, sizeof(struct Node));
    head->next = head;
    head->prev = head;
    app->attributes[i] = head;
  }

  mrp_enable(app);

  ESP_LOGI(TAG, "Initialization of MRP-%s completed.", mrp_application_type_to_str(type));
  return ESP_OK;
}

int mrp_exit(const struct mrp_application* app)
{
  mrp_stop_timers(app);
  mrp_delete_timers(app);
  return ESP_OK;
}
