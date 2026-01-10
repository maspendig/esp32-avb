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

struct mrp_attribute;

struct mrp_application
{
  mrp_application_type_t type;

  void (*mad_join_indication)(struct mrp_application* app, struct mrp_attribute* attr, bool new);
  void (*mad_leave_indication)(struct mrp_application* app, struct mrp_attribute* attr);
  u8 (*get_attribute_value_length)(u8 attribute_type);

  struct mrp_leaveall leaveall;
  esp_timer_handle_t join_timer;
};

struct mrp_attribute
{
  struct mrp_application* app;
  struct mrp_applicant applicant;
  struct mrp_registrar registrar;
  /* Attribute type identifier */
  u8 type;
  /* Attribute value(s) */
  u8 value[0];
};

/* IEEE 802.1Q-2022 10.8.2.5 Encoding of AttributeEvent */
typedef enum
{
  MRP_ATTRIBUTE_EVENT_NEW = 0,
  MRP_ATTRIBUTE_EVENT_JOIN_IN,
  MRP_ATTRIBUTE_EVENT_IN,
  MRP_ATTRIBUTE_EVENT_JOIN_MT,
  MRP_ATTRIBUTE_EVENT_MT,
  MRP_ATTRIBUTE_EVENT_LV,
} mrp_attribute_event_t;

// mapping of attribute event to state machine events
static const mrp_event_t mrp_attribute_event_to_event_map[] = {
  [MRP_ATTRIBUTE_EVENT_NEW] = MRP_EVENT_R_NEW,
  [MRP_ATTRIBUTE_EVENT_JOIN_IN] = MRP_EVENT_R_JOIN_IN,
  [MRP_ATTRIBUTE_EVENT_IN] = MRP_EVENT_R_IN,
  [MRP_ATTRIBUTE_EVENT_JOIN_MT] = MRP_EVENT_R_JOIN_MT,
  [MRP_ATTRIBUTE_EVENT_MT] = MRP_EVENT_R_MT,
  [MRP_ATTRIBUTE_EVENT_LV] = MRP_EVENT_R_LV
};


typedef struct mrp_vector_header
{
  u16 leave_all_event : 3;
  u16 number_of_values : 13;
} __attribute__((packed)) mrp_vector_header_t;

typedef struct mrp_data_unit_header
{
  u8 attribute_type;
  u8 attribute_length;
  u16 attribute_list_length;
} __attribute__((packed)) mrp_data_unit_header_t;

/**
 * IEEE 802.1Q-2022 10.8.2.10.1 Encoding of Vector ThreePackedEvents
 *
 * (((((firstAttributeEvent) × 6) + secondAttributeEvent) × 6) + thirdAttributeEvent)
 */
inline void mrp_decode_three_packed_event(u8 packed_event, u8* event)
{
  event[0] = packed_event / 36;
  event[1] = (packed_event % 36) / 6;
  event[2] = packed_event % 6;
}

/**
 * IEEE 802.1Q-2022 10.8.2.10.2 Encoding of Vector FourPackedEvents
 *
 * ((firstFourPackedType × 64) + (secondFourPackedType × 16) + (thirdFourPackedType × 4) + (fourthFourPackedType))
 */
inline void mrp_decode_four_packed_event(u8 packed_event, u8* event)
{
  event[0] = packed_event / 64;
  event[1] = (packed_event % 64) / 16;
  event[2] = (packed_event % 16) / 4;
  event[3] = packed_event % 4;
}

// Function declarations
void mrp_mad_join_request(struct mrp_application* app, u8 attribute_type, u8* value, bool new);
char* mrp_attribute_event_to_str(mrp_attribute_event_t event);
void mrp_applicant_state_machine(struct mrp_attribute* attr, mrp_event_t event);
void mrp_leaveall_state_machine(const struct mrp_attribute* attr, mrp_event_t event);
void mrp_registrar_state_machine(struct mrp_attribute* attr, mrp_event_t event);
int mrp_init_timers(struct mrp_attribute* attr);
void mrp_delete_timers(const struct mrp_attribute* attr);
void mrp_parse_vector_header(u16 vector_header, bool* leave_all_event, u16* number_of_values);
int mrp_init(struct mrp_attribute* attr, mrp_application_type_t type);

#endif //ETHERNET_PTP_MRP_H
