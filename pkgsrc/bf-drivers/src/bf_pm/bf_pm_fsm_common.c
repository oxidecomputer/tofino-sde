#include <stdio.h>

#include <tofino/bf_pal/bf_pal_types.h>
#include <bf_types/bf_types.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include <bf_pm/bf_pm_intf.h>
#include <bf_pm/pm_log.h>

char *bf_pm_fsm_fsms[BF_FSM_TYPE_MAX + 1] = {
	"PORT",
	"MEDIA",
	"QSFP",
	"QSFP_CHANNEL"
};

char *bf_pm_fsm_st_to_str[BF_FSM_PORT_MAX + 1] = {
    "BF_PM_FSM_ST_IDLE                 ",
    "BF_PM_FSM_ST_WAIT_PLL_READY       ",
    "BF_PM_FSM_ST_WAIT_SIGNAL_OK       ",
    "BF_PM_FSM_ST_WAIT_DFE_DONE        ",
    "BF_PM_FSM_ST_REMOTE_FAULT,        ",
    "BF_PM_FSM_ST_LINK_DN              ",
    "BF_PM_FSM_ST_LINK_UP              ",
    "BF_PM_FSM_ST_WAIT_TEST_DONE       ",
    "BF_PM_FSM_ST_BER_CHECK_START      ",
    "BF_PM_FSM_ST_BER_CHECK_DONE       ",

    "BF_PM_FSM_ST_END                  ",

    // AN fsm states
    "BF_PM_FSM_ST_WAIT_AN_DONE         ",
    "BF_PM_FSM_ST_WAIT_AN_LT_DONE      ",

    // PRBS-specific FSM states
    "BF_PM_FSM_ST_MONITOR_PRBS_ERRORS  ",

    // tof3 dfe FSM states
    "BF_PM_FSM_ST_WAIT_RX_READY        ",
    "BF_PM_FSM_ST_WAIT_TX_RATE_CHG_DONE",
    "BF_PM_FSM_ST_WAIT_RX_RATE_CHG_DONE",
    "BF_PM_FSM_ST_WAIT_CDR_LOCK        ",
    "BF_PM_FSM_ST_WAIT_BIST_LOCK       ",

    // tof3 ANLT extra FSM state
    "BF_PM_FSM_ST_WAIT_PACING_CTRL     ",
    "BF_PM_FSM_ST_AN_NP_1              ",
    "BF_PM_FSM_ST_AN_NP_2              ",
    "BF_PM_FSM_ST_AN_NP_3              ",
    "BF_PM_FSM_ST_WAIT_AN_BASE_PG_DONE ",
    "BF_PM_FSM_ST_SELECT_LT_CLAUSE     ",
    "BF_PM_FSM_ST_WAIT_AN_LT_DONE_CL72 ",
    "BF_PM_FSM_ST_WAIT_AN_LT_DONE_CL92 ",
    "BF_PM_FSM_ST_WAIT_AN_LT_DONE_CL136",
    "BF_PM_FSM_ST_WAIT_AN_LT_DONE_CL162",

    // Special "clean-up" states
    "BF_PM_FSM_ST_ABORT                ",
    "BF_PM_FSM_ST_DISABLED             ",
    "BF_PM_FSM_ST_CFG_TX_MODE          ",
};

char *pm_intf_fsm_st_to_str[BF_FSM_MEDIA_MAX + 1] = {
    "PM_INTF_FSM_DISABLED           ",
    "PM_INTF_FSM_INIT           ",
    "PM_INTF_FSM_MEDIA_DETECTED     ",
    "PM_INTF_FSM_WAIT_SERDES_TX_INIT",
    "PM_INTF_FSM_WAIT_MEDIA_INIT    ",
    "PM_INTF_FSM_WAIT_LINK_ST     ",
    "PM_INTF_FSM_INCOMPATIBLE_MEDIA ",
    "PM_INTF_FSM_HA_WAIT_FOR_LINK_ST",
    "PM_INTF_FSM_LINK_UP         ",
    "PM_INTF_FSM_LINK_DOWN",
    "PM_INTF_FSM_HA_LINK_UP         ",
    "PM_INTF_FSM_HA_LINK_DOWN",
    "PM_INTF_FSM_WAIT_MEDIA_RX_LOS  ",
    "PM_INTF_FSM_WAIT_MEDIA_RX_LOL  ",
    "PM_INTF_FSM_SET_RX_READY       ",
};

char *qsfp_fsm_st_to_str[BF_FSM_QSFP_MAX + 1] = {
    "QSFP_FSM_ST_IDLE            ",
    "QSFP_FSM_ST_REMOVED         ",
    "QSFP_FSM_ST_INSERTED        ",
    "QSFP_FSM_ST_WAIT_T_RESET    ",
    "QSFP_FSM_ST_WAIT_TON_TXDIS  ",
    "QSFP_FSM_ST_WAIT_LOWPWR     ",
    "QSFP_FSM_ST_DP_DEACTIVATE   ",
    "QSFP_FSM_ST_TOFF_LPMODE     ",
    "QSFP_FSM_ST_WAIT_TOFF_LPMODE",
    "QSFP_FSM_ST_DETECTED        ",
    "QSFP_FSM_ST_WAIT_TON_LPMODE ",
    "QSFP_FSM_ST_LPMODE          ",
    "QSFP_FSM_ST_UPDATE          ",
    "QSFP_FSM_ST_WAIT_UPDATE     ",
};

char *qsfp_fsm_ch_en_st_to_str[BF_FSM_QSFP_CHANNEL_MAX + 1] = {
    "QSFP_CH_FSM_ST_DISABLED",
    "QSFP_CH_FSM_ST_ENABLING",
    "QSFP_CH_FSM_ST_APPSEL",
    "QSFP_CH_FSM_ST_ENA_CDR",
    "QSFP_CH_FSM_ST_ENA_OPTICAL_TX",
    "QSFP_CH_FSM_ST_NOTIFY_ENABLED",
    "QSFP_CH_FSM_ST_ENABLED",
    "QSFP_CH_FSM_ST_DISABLING",
    "QSFP_CH_FSM_ST_REJECTED",
};

/*
 * Given a bf_pm_fsm_st, return a string containing the state's name.
 */
char *bf_pm_fsm_state_to_str(bf_pm_fsm_st st) {
  if (st < (sizeof(bf_pm_fsm_st_to_str) / sizeof(bf_pm_fsm_st_to_str[0]))) {
    return bf_pm_fsm_st_to_str[st];
  }
  return "invalid";
}

/*
 * Optional callback provided by the control plane binary linked with this
 * library.
 */
#pragma weak bf_pm_fsm_transition_callback

const char *INVALID_STATE = "INVALID_STATE";

/*
 * Log the FSM state transition being made.  If the new and old states are
 * different, and the control plane binary has provided a callback, notify the
 * controller of the transition.
 */
bf_status_t
bf_pm_fsm_transition(bf_fsm_type_t which, bf_fsm_port_t port,
    uint32_t from, uint32_t to) {
	uint32_t max_state;
	const char *from_name, *to_name;
	char **state_names;
	char port_name[32];
	uint32_t asic_id;

	switch (which) {
	case BF_FSM_PORT:
		max_state = BF_FSM_PORT_MAX;
		state_names = bf_pm_fsm_st_to_str;
		snprintf(port_name, sizeof (port_name), "ASIC_ID %d",
		    port.asic_id);
		break;
	case BF_FSM_MEDIA:
		max_state = BF_FSM_MEDIA_MAX;
		state_names = pm_intf_fsm_st_to_str;
		snprintf(port_name, sizeof (port_name), "PHYS %d:%d",
		    port.port_id.connector, port.port_id.channel);
		break;
	case BF_FSM_QSFP:
		max_state = BF_FSM_QSFP_MAX;
		state_names = qsfp_fsm_st_to_str;
		snprintf(port_name, sizeof (port_name), "PHYS %d:%d",
		    port.port_id.connector, port.port_id.channel);
		break;
	case BF_FSM_QSFP_CHANNEL:
		max_state = BF_FSM_QSFP_CHANNEL_MAX;
		state_names = qsfp_fsm_ch_en_st_to_str;
		snprintf(port_name, sizeof (port_name), "PHYS %d:%d",
		    port.port_id.connector, port.port_id.channel);
		break;
	default:
		return BF_INVALID_ARG;
	}

	from_name = (from <= max_state) ? state_names[from] : INVALID_STATE;
	to_name = (from <= max_state) ? state_names[to] : INVALID_STATE;

	PM_DEBUG("FSM transition.  port: %s  fsm: %s  from: %s to: %s",
	    port_name, bf_pm_fsm_fsms[which], from_name, to_name);

	if (to_name == INVALID_STATE || from_name == INVALID_STATE)
		return BF_INVALID_ARG;

	if (from == to)
		return BF_SUCCESS;

	if (bf_pm_fsm_transition_callback == NULL) {
		return BF_SUCCESS;
	}

	if (which == BF_FSM_PORT) {
		asic_id = port.asic_id;
	} else {
		bf_status_t rval;
		bf_dev_id_t dev_id = 0;
		bf_dev_port_t dev_port;
		bf_pal_front_port_handle_t hdl = {
			.conn_id = port.port_id.connector,
			.chnl_id = port.port_id.channel
		};

		rval = bf_pm_port_front_panel_port_to_dev_port_get(&hdl,
		    &dev_id, &dev_port);
		if (rval != BF_SUCCESS)
			return (rval);
		asic_id = dev_port;
	}

	bf_pm_fsm_transition_callback(which, asic_id, (uint32_t)from,
	    (uint32_t)to);

	return BF_SUCCESS;
}
