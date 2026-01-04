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


#endif //ETHERNET_PTP_MRP_H