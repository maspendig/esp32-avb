//
// Created by max on 1/4/26.
//

#ifndef ETHERNET_PTP_MRP_H
#define ETHERNET_PTP_MRP_H
#include <esp_timer.h>
#include <types.h>

/* MRP timer parameter values defined in IEEE 802.1Q-2022 Table 10-7 */
#define MRP_JOIN_TIME_MS     200
#define MRP_LEAVE_TIME_MS    1000
#define MRP_LEAVEALL_TIME_MS 10000

typedef enum
{
  /* MRP Applicant States - IEEE 802.1Q-2022 Section 10.7.1 */
  MRP_VO_STATE = 0, /* Very Anxious Observer */
  MRP_VP_STATE, /* Very Anxious Passive */
  MRP_VN_STATE, /* Very Anxious New */
  MRP_AN_STATE, /* Anxious New */
  MRP_AA_STATE, /* Anxious Active */
  MRP_QA_STATE, /* Quiet Active */
  MRP_LA_STATE, /* Leaving Active */
  MRP_AO_STATE, /* Anxious Observer State */
  MRP_QO_STATE, /* Quiet Observer State */
  MRP_AP_STATE, /* Anxious Passive State */
  MRP_QP_STATE, /* Quiet Passive State */
  MRP_LO_STATE, /* Leaving Observer State */

  /* MRP Registrar States - IEEE 802.1Q-2022 Section 10.7.1 */
  MRP_IN_STATE, /* when Registrar state is IN */
  MRP_LV_STATE, /* registrar state - leaving */
  MRP_MT_STATE /* when Registrar state is empty */
} mrp_state_t;

typedef enum
{
  MRP_EVENT_BEGIN = 1,
  MRP_EVENT_NEW,
  MRP_EVENT_JOIN,
  MRP_EVENT_LV,
  MRP_EVENT_TX,
  MRP_EVENT_TXLA,
  MRP_EVENT_TXLAF,
  MRP_EVENT_R_NEW,
  MRP_EVENT_R_JOIN_IN,
  MRP_EVENT_R_IN,
  MRP_EVENT_R_JOIN_MT,
  MRP_EVENT_R_MT,
  MRP_EVENT_R_LV,
  MRP_EVENT_R_LA, /* Leave All */
  MRP_EVENT_FLUSH,
  MRP_EVENT_REDECLARE,
  MRP_EVENT_PERIODIC,
  MRP_EVENT_LEAVETIMER,
  MRP_EVENT_LEAVEALLTIMER,
  MRP_EVENT_PERIODICTIMER,
} mrp_event_t;

/** MRP Actions
 *  ignored encoding optimated actions as not needed for this implementation
 */
typedef enum
{
  MRP_ACTION_NONE = 0,
  MRP_ACTION_NEW, /** send a New indication to MAP and the MRP application */
  MRP_ACTION_JOIN, /* send a Join indication to MAP and the MRP application */
  MRP_ACTION_LV, /* send a Lv indication to MAP and the MRP application */
  MRP_ACTION_S_N, /* send a New message */
  MRP_ACTION_S_J, /* send a JoinIn or JoinMt message */
  MRP_ACTION_S_L, /* send a Lv message */
  MRP_ACTION_S, /**< send a In or Empty message */
  MRP_ACTION_S_LA, /**< send a Leave All message */
  MRP_ACTION_PERIODIC, /**< Periodic transmission event */
  MRP_ACTION_LEAVETIMER, /**< Leave All period timer */
  MRP_ACTION_LEAVEALLTIMER, /**< Leave All timer */
  MRP_ACTION_PERIODICTIMER /**< Periodic transmission timer */
} mrp_action_t;

typedef enum
{
  MSRP = 0,
  MVRP,
  MMRP,
} mrp_application_type_t;

typedef enum
{
  MRP_ACTIVE = 0,
  MRP_PASSIVE,
} mrp_active_state_t;

struct mrp_applicant
{
  mrp_state_t state;
  mrp_action_t action;
};

struct mrp_registrar
{
  mrp_state_t state;
  mrp_action_t action;
  esp_timer_handle_t leave_timer;
};

struct mrp_leaveall
{
  mrp_active_state_t state;
  mrp_action_t action;
  esp_timer_handle_t timer;
};

struct mrp_application
{
  mrp_application_type_t type;
  struct mrp_leaveall leaveall;
  esp_timer_handle_t join_timer;
};

struct mrp_attribute
{
  struct mrp_application* app;
  struct mrp_applicant applicant;
  struct mrp_registrar registrar;
};

typedef struct mrp_data_unit_header
{
  u8 attribute_type;
  u8 attribute_length;
  u16 attribute_list_length;
} __attribute__((packed)) mrp_data_unit_header_t;
#endif //ETHERNET_PTP_MRP_H
