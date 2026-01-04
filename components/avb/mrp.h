//
// Created by max on 1/4/26.
//

#ifndef ETHERNET_PTP_MRP_H
#define ETHERNET_PTP_MRP_H

typedef enum
{
  /* MRP Applicant States - IEEE 802.1Q-2022 Section 10.7.1 */
  MRP_VO_STATE = 0,	/* Very Anxious Observer */
  MRP_VP_STATE, /* Very Anxious Passive */
  MRP_VN_STATE, /* Very Anxious New */
  MRP_AN_STATE, /* Anxious New */
  MRP_AA_STATE, /* Anxious Active */
  MRP_QA_STATE, /* Quiet Active */
  MRP_LA_STATE, /* Leaving Active */
  MRP_AO_STATE, /* Anxious Observer State */
  MRP_QO_STATE, /* Quiet Observer State */
  MRP_AP_STATE, /* Anxious Passive State */
  MRP_QP_STATE,	/* Quiet Passive State */
  MRP_LO_STATE,	/* Leaving Observer State */

  /* MRP Registrar States - IEEE 802.1Q-2022 Section 10.7.1 */
  MRP_IN_STATE, /* when Registrar state is IN */
  MRP_LV_STATE, /* registrar state - leaving */
  MRP_MT_STATE  /* when Registrar state is empty */
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

typedef enum
{
  MRP_ACTION_NONE = 0,
  MRP_ACTION_NEW,		/** send a New indication to MAP and the MRP application */
  MRP_ACTION_JOIN,	/* send a Join indication to MAP and the MRP application */
  MRP_ACTION_LV,		/* send a Lv indication to MAP and the MRP application */
  MRP_ACTION_S_N,		/* send a New message */
  MRP_ACTION_S_J,		/* send a JoinIn or JoinMt message */
  MRP_ACTION_S_L,		/* send a Lv message */
  MRP_ACTION_S,		/**< send a In or Empty message */
  MRP_ACTION_S_LA,		/**< send a Leave All message */
  MRP_ACTION_PERIODIC,	/**< Periodic transmission event */
  MRP_ACTION_LEAVETIMER,	/**< Leave All period timer */
  MRP_ACTION_LEAVEALLTIMER,	/**< Leave All timer */
  MRP_ACTION_PERIODICTIMER	/**< Periodic transmission timer */
} mrp_action_t;

#endif //ETHERNET_PTP_MRP_H