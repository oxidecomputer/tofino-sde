#ifndef BF_PM_FSM_COMMON_H_INCLUDED
#define BF_PM_FSM_COMMON_H_INCLUDED

/* Allow the use in C++ code.  */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Each FSM manages the lifecycle of a different type of component.  While there
 * is a strong association between an asic ID and the physical port it connects
 * with, that association isn't generally relevant or known within the FSM.
 * This structure lets each FSM identify the object under management in its
 * natural namespace.
 */
typedef union {
	/* The asic ID is used by the port-level FSM. */
	uint32_t asic_id;
	/* The port_id is used by the FSMs that manage the physical media */
	struct {
		uint32_t connector;
		uint32_t channel;
	} port_id;
} bf_fsm_port_t;

/*
 * Finite state machine types
 */
typedef enum {
	BF_FSM_PORT,
	BF_FSM_MEDIA,
	BF_FSM_QSFP,
	BF_FSM_QSFP_CHANNEL
} bf_fsm_type_t;

#define BF_FSM_TYPE_MAX BF_FSM_QSFP_CHANNEL

/*
 * per-link state machine, including autonegotiation and link-training
 */
typedef enum {
  // DFE fsm states
  BF_PM_FSM_ST_IDLE = 0,
  BF_PM_FSM_ST_WAIT_PLL_READY,
  BF_PM_FSM_ST_WAIT_SIGNAL_OK,
  BF_PM_FSM_ST_WAIT_DFE_DONE,
  BF_PM_FSM_ST_REMOTE_FAULT,
  BF_PM_FSM_ST_LINK_DN,
  BF_PM_FSM_ST_LINK_UP,
  BF_PM_FSM_ST_WAIT_TEST_DONE,
  // special check for poor BER
  BF_PM_FSM_ST_BER_CHECK_START,
  BF_PM_FSM_ST_BER_CHECK_DONE,
  BF_PM_FSM_ST_END,

  // AN fsm states
  BF_PM_FSM_ST_WAIT_AN_DONE,
  BF_PM_FSM_ST_WAIT_AN_LT_DONE,

  // PRBS-specific FSM states
  BF_PM_FSM_ST_MONITOR_PRBS_ERRORS,

  // tof3 DFE FSM states
  BF_PM_FSM_ST_WAIT_RX_READY,
  BF_PM_FSM_ST_WAIT_TX_RATE_CHG_DONE,
  BF_PM_FSM_ST_WAIT_RX_RATE_CHG_DONE,
  BF_PM_FSM_ST_WAIT_CDR_LOCK,
  BF_PM_FSM_ST_WAIT_BIST_LOCK,

  // tof3 ANLT extra FSM state
  BF_PM_FSM_ST_WAIT_PACING_CTRL,  // check pacer
  BF_PM_FSM_ST_AN_NP_1,           // 1st consortium NP
  BF_PM_FSM_ST_AN_NP_2,           // 2nd consortium NP
  BF_PM_FSM_ST_AN_NP_3,           // ack all NP with NULL pg
  BF_PM_FSM_ST_WAIT_AN_BASE_PG_DONE,
  BF_PM_FSM_ST_SELECT_LT_CLAUSE,
  BF_PM_FSM_ST_WAIT_AN_LT_DONE_CL72,   // 500ms
  BF_PM_FSM_ST_WAIT_AN_LT_DONE_CL92,   // ?
  BF_PM_FSM_ST_WAIT_AN_LT_DONE_CL136,  // 3.5 sec
  BF_PM_FSM_ST_WAIT_AN_LT_DONE_CL162,  // 15 sec

  // Special "clean-up" states
  BF_PM_FSM_ST_ABORT,
  BF_PM_FSM_ST_DISABLED,
} bf_pm_fsm_st;

#define BF_FSM_PORT_MAX BF_PM_FSM_ST_DISABLED

extern char *bf_pm_fsm_st_to_str[];
extern char *bf_pm_fsm_state_to_str(bf_pm_fsm_st st);

/*
 * Higher-level media state machine
 */
typedef enum {
  PM_INTF_FSM_DISABLED,
  PM_INTF_FSM_INIT,
  PM_INTF_FSM_MEDIA_DETECTED,
  PM_INTF_FSM_WAIT_SERDES_TX_INIT,
  PM_INTF_FSM_WAIT_MEDIA_INIT,
  PM_INTF_FSM_WAIT_LINK_ST,
  PM_INTF_FSM_INCOMPATIBLE_MEDIA,
  PM_INTF_FSM_HA_WAIT_LINK_ST,
  PM_INTF_FSM_LINK_UP,
  PM_INTF_FSM_LINK_DOWN,
  PM_INTF_FSM_HA_LINK_UP,
  PM_INTF_FSM_HA_LINK_DOWN,
  PM_INTF_FSM_WAIT_MEDIA_RX_LOS,
  PM_INTF_FSM_WAIT_MEDIA_RX_LOL,
  PM_INTF_FSM_SET_RX_READY,
} pm_intf_fsm_states_t;

#define BF_FSM_MEDIA_MAX PM_INTF_FSM_SET_RX_READY

extern char *pm_intf_fsm_st_to_str[];

/*
 * QSFP transceiver state machine
 */
typedef enum {
  // QSFP unknown or unused on board
  QSFP_FSM_ST_IDLE = 0,
  // pseudo-states, assigned by qsfp scan timer to communicate
  // with qsfp fsm
  QSFP_FSM_ST_REMOVED,
  QSFP_FSM_ST_INSERTED,
  // states requiring fixed delays
  QSFP_FSM_ST_WAIT_T_RESET,    // 2000ms
  QSFP_FSM_ST_WAIT_TON_TXDIS,  //  100ms
  QSFP_FSM_ST_WAIT_LOWPWR,
  QSFP_FSM_ST_DP_DEACTIVATE,
  QSFP_FSM_ST_TOFF_LPMODE,       //  300ms
  QSFP_FSM_ST_WAIT_TOFF_LPMODE,  //  300ms
  QSFP_FSM_ST_DETECTED,
  QSFP_FSM_ST_WAIT_TON_LPMODE,
  QSFP_FSM_ST_LPMODE,
  QSFP_FSM_ST_UPDATE,
  QSFP_FSM_ST_WAIT_UPDATE,  // update-specific delay
} qsfp_fsm_state_t;

#define BF_FSM_QSFP_MAX QSFP_FSM_ST_WAIT_UPDATE
extern char *qsfp_fsm_st_to_str[];

/*
 * QSFP per-channel state machine
 */
typedef enum {
  QSFP_CH_FSM_ST_DISABLED = 0,
  QSFP_CH_FSM_ST_ENABLING,  // kick off enable sequence
  QSFP_CH_FSM_ST_APPSEL,
  QSFP_CH_FSM_ST_ENA_CDR,         // 300ms (Luxtera PSM4)
  QSFP_CH_FSM_ST_ENA_OPTICAL_TX,  // 400ms, CMISand8636_TMR_toff_txdis
  QSFP_CH_FSM_ST_NOTIFY_ENABLED,
  QSFP_CH_FSM_ST_ENABLED,
  QSFP_CH_FSM_ST_DISABLING,  // assert TX_DISABLE
  QSFP_CH_FSM_ST_REJECTED,
} qsfp_fsm_ch_en_state_t;
#define BF_FSM_QSFP_CHANNEL_MAX QSFP_CH_FSM_ST_REJECTED

extern char *qsfp_fsm_ch_en_st_to_str[];

/* Internal function called by the state machines during transitions */
bf_status_t bf_pm_fsm_transition(bf_fsm_type_t which, bf_fsm_port_t port,
    uint32_t from, uint32_t to);

/* Optional external function called by bf_pm_fsm_transition() */
void bf_pm_fsm_transition_callback(bf_fsm_type_t which, uint32_t asic_id,
    uint32_t from, uint32_t to);

#ifdef __cplusplus
}
#endif /* C++ */

#endif  // BF_PM_FSM_COMMON_H_INCLUDED
