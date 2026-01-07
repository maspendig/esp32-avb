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

void mrp_applicant_state_machine(struct mrp_attribute* attr, mrp_event_t event);
void mrp_leaveall_state_machine(const struct mrp_attribute* attr, mrp_event_t event);
void mrp_registrar_state_machine(struct mrp_attribute* attr, mrp_event_t event);

//region join timer
int mrp_start_join_timer(const struct mrp_attribute* attr)
{
  // in order to fit the state machine definitions, we only use once timer here
  // the timer is restarted by the state machine
  return esp_timer_start_once(attr->app->join_timer, MRP_JOIN_TIME_MS);
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
  struct mrp_attribute* attr = (struct mrp_attribute*)arg;
  struct mrp_application* app = attr->app;
  mrp_event_t event = MRP_EVENT_TX;

  /* IEEE802.1q-2022 10.7.5.7
   * The tx! event is modified by the behaviour of the LeaveAll state machine.
   * If the LeaveAll state machine has signaled LeaveAll, then tx! is modified to txLA!
   */
  mrp_leaveall_state_machine(attr, MRP_EVENT_TX);
  if (app->leaveall.action == MRP_ACTION_S_LA)
  {
    event = MRP_EVENT_TXLA;
  }

  mrp_applicant_state_machine(attr, event);
}

int mrp_init_join_timer(const struct mrp_attribute* attr)
{
  const esp_timer_create_args_t timer_args = {
    .callback = &mrp_join_timer_callback,
    .arg = (void*)attr,
    .name = "mrp_join_timer"
  };
  struct mrp_application* app = attr->app;
  return esp_timer_create(&timer_args, &app->join_timer);
}

//endregion

//region leaveall timer
/* IEEE802.1Q-2022 10.7.4.3 leavealltimer */
int mrp_start_leaveall_timer(const struct mrp_attribute* attr)
{
  const struct mrp_application* app = attr->app;
  // LeaveAllTime < T < 1.5 x LeaveAllTime
  u32 interval = random_in_range(MRP_LEAVEALL_TIME_MS, MRP_LEAVEALL_TIME_MS * 1.5);
  // in order to fit the state machine definitions, we only use once timer here
  // the timer is restarted by the state machine
  return esp_timer_start_once(app->leaveall.timer, interval);
}

int mrp_restart_leaveall_timer(const struct mrp_attribute* attr)
{
  const struct mrp_application* app = attr->app;
  // LeaveAllTime < T < 1.5 x LeaveAllTime
  u32 interval = random_in_range(MRP_LEAVEALL_TIME_MS, MRP_LEAVEALL_TIME_MS * 1.5);
  return esp_timer_restart(app->leaveall.timer, interval);
}

int mrp_stop_leaveall_timer(const struct mrp_attribute* attr)
{
  const struct mrp_application* app = attr->app;
  return esp_timer_stop(app->leaveall.timer);
}

void mrp_leaveall_timer_callback(void* arg)
{
  const struct mrp_attribute* attr = (struct mrp_attribute*)arg;
  mrp_leaveall_state_machine(attr, MRP_EVENT_LEAVEALLTIMER);
}

int mrp_init_leaveall_timer(const struct mrp_attribute* attr)
{
  const esp_timer_create_args_t timer_args = {
    .callback = &mrp_leaveall_timer_callback,
    .arg = (void*)attr,
    .name = "mrp_leaveall_timer"
  };
  struct mrp_application* app = attr->app;
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
  return esp_timer_start_once(registrar->leave_timer, MRP_LEAVE_TIME_MS);
}

int mrp_stop_leave_timer(const struct mrp_attribute* attr)
{
  const struct mrp_registrar* registrar = &attr->registrar;
  return esp_timer_stop(registrar->leave_timer);
}

//endregion

void init_timers(struct mrp_attribute* attr)
{
  mrp_init_leaveall_timer(attr);
  mrp_init_leave_timer(attr);
  mrp_init_join_timer(attr);
}

void delete_timers(const struct mrp_attribute* attr)
{
  esp_timer_delete(attr->app->leaveall.timer);
  esp_timer_delete(attr->registrar.leave_timer);
  esp_timer_delete(attr->app->join_timer);
}

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
    ESP_LOGI(TAG, "Unknown event %s", mrp_event_to_str(event));
  }

  applicant->state = state;
  applicant->action = action;
  ESP_LOGI(TAG, "state %s => action %s", mrp_state_to_str(state), mrp_action_to_str(action));
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
    ESP_LOGW(TAG, "Unknown event %s", mrp_event_to_str(event));
    break;
  }

  registrar->state = state;
  registrar->action = action;
  ESP_LOGI(TAG, "state %s => action %s", mrp_state_to_str(state), mrp_action_to_str(action));
}

void mrp_leaveall_state_machine(const struct mrp_attribute* attr, mrp_event_t event)
{
  struct mrp_application* app = attr->app;
  struct mrp_leaveall* leaveall = &app->leaveall;
  mrp_active_state_t state = leaveall->state;
  mrp_action_t action = MRP_ACTION_NONE;
  switch (event)
  {
  case MRP_EVENT_BEGIN:
    state = MRP_PASSIVE;
    mrp_start_leaveall_timer(attr);
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
    mrp_restart_leaveall_timer(attr);
    break;
  case MRP_EVENT_LEAVEALLTIMER:
    state = MRP_ACTIVE;
    mrp_start_leaveall_timer(attr);
    break;
  default:
    ESP_LOGW(TAG, "Unknown event %s for leave all state machine!", mrp_event_to_str(event));
  }
  leaveall->state = state;
  leaveall->action = action;
}

void mrp_state_init()
{
}


