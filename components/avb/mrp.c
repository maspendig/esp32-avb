//
// Created by max on 1/4/26.
//

#include "mrp.h"

#include <esp_log.h>

#include "../../../../../esp-idf/components/fatfs/src/ff.h"

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

void mrp_applicant_state_machine(mrp_event_t event, mrp_state_t state)
{
  mrp_action_t action = MRP_ACTION_NONE;

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

  ESP_LOGI(TAG, "state %s => action %s", mrp_state_to_str(state), mrp_action_to_str(*action));
}

void mrp_state_init()
{
}


