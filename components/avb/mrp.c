//
// Created by max on 1/4/26.
//

#include "mrp.h"
#include "common.h"

#include <esp_err.h>
#include <esp_log.h>
#include <msrp.h>
#include <types.h>


#define TAG "MRP"

//region string conversion helpers
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
  node->prev->next = node->next;
  node->next->prev = node->prev;
  free(attr);
}

//region join timer
/* Join timer per application */
int mrp_start_join_timer(const struct mrp_application* app)
{
  if (esp_timer_is_active(app->join_timer))
    return ESP_OK;

  // in order to fit the state machine definitions, we only use once timer here
  // the timer is restarted by the state machine
  return esp_timer_start_once(app->join_timer, MRP_JOIN_TIME_MS * 1000);
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
  for (u8 type = 0; type < MRP_MAX_ATTRIBUTE_TYPES; type++)
  {
    struct Node* head = app->attributes[type];
    struct Node* node = head;
    while (node->next != head)
    {
      struct mrp_attribute* attribute = (struct mrp_attribute*)node->next;
      ESP_LOGI(TAG, "Processing MRP Join Timer tx event [loop: %d] [attribute type: %d]", type, attribute->type);
      mrp_applicant_state_machine(attribute, event);
      mrp_delete_attribute(attribute);
      node = node->next;
    }
  }
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

int mrp_init_timers(struct mrp_application* app)
{
  int ret = ESP_OK;
  ret += mrp_init_leaveall_timer(app);
  ret += mrp_init_join_timer(app);
  return ret;
}

void mrp_delete_timers(const struct mrp_application* app)
{
  esp_timer_delete(app->leaveall.timer);
  esp_timer_delete(app->join_timer);
}

void mrp_parse_vector_header(u16 vector_header, bool* leave_all_event, u16* number_of_values)
{
  *leave_all_event = (vector_header >> 13) & 0x1;
  *number_of_values = vector_header & 0x1FFF;
}

/* IEEE 802.1Q-2022 Table 10-3 Note 6:
 * Request opportunity to transmit on entry to VN, AN, AA, LA, VP, AP, and LO states
 */
bool mrp_request_state_transmit(const mrp_state_t state)
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
      state = MRP_AO_STATE;
      break;
    case MRP_VP_STATE:
      state = MRP_AP_STATE;
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
    ESP_LOGI(TAG, "Unknown event %d", event);
  }

  ESP_LOGI(TAG, "Applicant SM - event: %s, state [%s] => [%s], action %s",
           mrp_event_to_str(event),
           mrp_state_to_str(applicant->state),
           mrp_state_to_str(state),
           mrp_action_to_str(action));

  /* IEEE 802.1Q-2022 Table 10-3 Note 6:
   * Request opportunity to transmit on entry to VN, AN, AA, LA, VP, AP, and LO states
   */
  if (mrp_request_state_transmit(state))
  {
    mrp_start_join_timer(attr->app);
  }

  applicant->state = state;
  applicant->action = action;
  if (action != MRP_ACTION_NONE)
    attr->app->mrp_send_action(attr->app, attr);
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
    break;
  }
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
    ESP_LOGW(TAG, "Unknown event %d", event);
    break;
  }

  ESP_LOGI(TAG, "Registrar SM - event: %s, state [%s] => [%s], action %s",
           mrp_event_to_str(event),
           mrp_state_to_str(registrar->state),
           mrp_state_to_str(state),
           mrp_action_to_str(action));

  registrar->state = state;
  registrar->action = action;

  mrp_registrar_exec_action(attr);
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
   * Request opportunity to trasmit on entry to the active state
   */
  if (leaveall->state == MRP_PASSIVE && state == MRP_ACTIVE)
  {
    mrp_start_join_timer(app);
  }

  leaveall->state = state;
  leaveall->action = action;
}

//endregion

struct mrp_attribute* mrp_get_attribute(struct mrp_application* app, u8 attribute_type, u8* value)
{
  const u8 length = app->get_attribute_value_length(attribute_type);
  const struct mrp_attribute* attribute;
  struct Node* head = app->attributes[attribute_type];
  struct Node* temp = head;
  // traverse the linked list, if next match the current node we reached the end
  while (temp->next != head)
  {
    attribute = (struct mrp_attribute*)temp;
    if (memcmp(attribute->value, value, length) == 0)
    {
      return (struct mrp_attribute*)temp;
    }
    temp = temp->next;
  }

  return NULL;
}


void mrp_append_attributes_list(struct mrp_application* app, struct mrp_attribute* attr)
{
  struct Node* head = app->attributes[attr->type];
  struct Node* entry = &attr->list;

  ESP_LOGI(TAG, "Appending attribute type %d to application's attribute list", attr->type);

  struct Node* last_entry = head->prev;

  // | head | <-> | ... | <-> | last_entry | <-> | entry |
  entry->prev = last_entry;
  last_entry->next = entry;

  head->prev = entry;
  entry->next = head;
}

struct mrp_attribute* mrp_create_attribute(struct mrp_application* app, u8 attribute_type, u8* value)
{
  ESP_LOGI(TAG, "Creating new attribute for type %d", attribute_type);
  const u8 attribute_value_length = app->get_attribute_value_length(attribute_type);
  struct mrp_attribute* attr = calloc(1, sizeof(struct mrp_attribute) + attribute_value_length);
  attr->app = app;
  attr->type = attribute_type;
  memcpy(attr->value, value, attribute_value_length);

  mrp_append_attributes_list(app, attr);


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
    attr = mrp_create_attribute(app, attribute_type, value);
  }
  return attr;
}

void mrp_mad_join_request(struct mrp_application* app, u8 attribute_type, u8* value, bool new)
{
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

void mrp_process_attribute_event(struct mrp_application* app, u8 type, u8* value, mrp_attribute_event_t event)
{
  struct mrp_attribute* attr;
  attr = mrp_get_or_create_attribute(app, type, value);

  const mrp_event_t _event = mrp_attribute_event_to_event_map[event];
  ESP_LOGI(TAG, "Mapped attribute event %s[%d] to protocol event %s[%d]",
           mrp_attribute_event_to_str(event),
           event,
           mrp_event_to_str(_event),
           _event);
  mrp_registrar_state_machine(attr, mrp_attribute_event_to_event_map[event]);
  mrp_applicant_state_machine(attr, mrp_attribute_event_to_event_map[event]);
}

void mrp_begin(const struct mrp_attribute* attr)
{
  // TODO check participant type
  // if(attr->app->participant_type == MRP_PARTICIPANT_FULL)
  {
    mrp_leaveall_state_machine(attr->app, MRP_EVENT_BEGIN);
  }
  if (attr->app->type != MSRP)
  {
    // TODO implement periodic
    // mrp_periodic_state_machine(attr, MRP_EVENT_BEGIN);
  }
}

// TODO only app is to be initialized here, nothing about attributes
int mrp_init(struct mrp_attribute* attr, mrp_application_type_t type)
{
  struct mrp_application* app = calloc(1, sizeof(struct mrp_application));
  struct mrp_registrar* reg = calloc(1, sizeof(struct mrp_registrar));
  struct mrp_applicant* applicant = calloc(1, sizeof(struct mrp_applicant));
  attr->app = app;
  attr->applicant = *applicant;
  attr->registrar = *reg;
  attr->app->type = type;
  if (mrp_init_timers(app) > ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to initialize MRP timers");
    return ESP_FAIL;
  }

  for (int i = 0; i < MRP_MAX_ATTRIBUTE_TYPES; i++)
  {
    struct Node* head = calloc(1, sizeof(struct Node));
    head->next = head;
    head->prev = head;
    app->attributes[i] = head;
  }

  mrp_begin(attr);

  return ESP_OK;
}

int mrp_exit(const struct mrp_application* app)
{
  mrp_delete_timers(app);
  return ESP_OK;
}
