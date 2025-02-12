/*******************************************************************************
 *  Copyright (C) 2024 Intel Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions
 *  and limitations under the License.
 *
 *
 *  SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/

#include <stdio.h>
#include <stdbool.h>

#include <bf_types/bf_types.h>
#include <dvm/bf_dma_types.h>
#include <dvm/bf_drv_intf.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include <port_mgr/bf_port_if.h>
#include "bf_pm_fsm_if.h"
#include "../pm_log.h"

/************************************************************************
 * Port Initialization FSMs
 *
 * This module defines generic Finite State Machine (FSM). FSMs are
 * used by the Port Manager Example code to bring up ports once they
 * have been enabled.
 *
 * The FSMs are useful for amortizing delays, required by certain
 * programming steps, across multiple ports, effectively "sharing" the
 * delays. For example, initializing each serdes slice can take up to
 * 40ms, waiting for the PLLs to stabilize. Executed serially over 256x
 * 25G ports would require a minimum of 256*40ms, or over 10 seconds.
 * However, If all ports are enabled at roughly the same time (for
 * instance on a cold boot) all slices can be programmed then share a
 * single 40ms wait for the PLLs.
 *
 * The states are defined in bf_pm_fsm_if.h and may be extended as necessary.
 * Each state descriptor describes a single state in the FSM.
 *
 * State descriptor variables include the,
 *    state identifier
 *    state handler fn
 *    next state identifier to execute if the handler returns BF_SUCCESS
 *    time before either retrying the current state or transitioning to
 *    the next state
 *    a "transition fn" to be called on transition to next state.
 *
 * Each state handler function has the same prototype,
 *   bf_status_t <handler_fn>( bf_dev_id_t, bf_dev_port_t )
 *
 * The state machine is controlled by the return value from the handlers.
 * If the handler returns,
 *
 *    BF_SUCCES,    transition to next state
 *    BF_NOT_READY, stay in current state
 *    otherwise,    terminate the state machine (some fatal error)
 *
 * There are two FSMs, one for ports requiring clause 73/72 autoneg and
 * link training (bf_pm_fsm_an_default) and one for those that don't
 * (bf_pm_fsm_default).
 ************************************************************************/

#if 0
/*
 * This definition is now in bf_pm_fsm_common.h.  The old cold has been left
 * here to simplify future merges with upstream code
 */

/************************************************************************
 * bf_pm_fsm_st_to_str
 *
 * enum-to-str fn for display purposes
 ************************************************************************/
char *bf_pm_fsm_st_to_str[] = {
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
};

/************************************************************************
 * bf_pm_fsm_state_to_str
 *
 ************************************************************************/
char *bf_pm_fsm_state_to_str(bf_pm_fsm_st st) {
  if (st < (sizeof(bf_pm_fsm_st_to_str) / sizeof(bf_pm_fsm_st_to_str[0]))) {
    return bf_pm_fsm_st_to_str[st];
  }
  return "invalid";
}
#endif

/************************************************************************
 * bf_pm_fsm_find_state
 *
 * Look up the state descriptor corresponding to the passed "st" in the
 * passed "fsm".
 *
 * Each fsm is terminated by a null entry with cur_st=BF_PM_FSM_ST_END.
 ************************************************************************/
bf_pm_fsm_state_desc_t *bf_pm_fsm_find_state(bf_pm_fsm_state_desc_t *fsm,
                                             bf_pm_fsm_st st) {
  bf_pm_fsm_state_desc_t *desc_p = &fsm[0];

  while (desc_p->state != st) {
    desc_p++;
    // dont run off the end
    if (desc_p->state == BF_PM_FSM_ST_END) break;

    bf_sys_assert(desc_p->state != BF_PM_FSM_ST_END);
  }
  return (desc_p);
}

/************************************************************************
 * bf_pm_fsm_bind_cb_to_state
 ************************************************************************/
bf_status_t bf_pm_fsm_bind_cb_to_state(bf_pm_fsm_state_desc_t *fsm,
                                       bf_pm_fsm_st st,
                                       bf_pm_fsm_transition_fn cb) {
  bf_pm_fsm_state_desc_t *desc_p = bf_pm_fsm_find_state(fsm, st);

  if (desc_p == NULL) return BF_INVALID_ARG;

  desc_p->transition_fn = cb;

  return BF_SUCCESS;
}

/*************************************************************************
 * bf_pm_fsm_run
 *
 * Run the port thru its current state in the fsm. The state handling fn
 * returns one of,
 *
 * BF_SUCCESS, indicates the state completed successfully, transition to
 *             nxt_st
 * BF_NOT_READY, indicates the current state is incomplete and must be called
 *               again. No transition.
 * Anything else, indicates some error that will terminate fsm.
 *
 * This function returns the time to wait until the next state or the
 * next invocation of the current state (on BF_SUCCESS or BF_NOT_READY)
 * Otherwise, returns next wait time = BF_FSM_ABORT which will terminate
 * the tasklet.
 *************************************************************************/
bf_status_t bf_pm_fsm_run(bf_dev_id_t dev_id,
                          bf_dev_port_t dev_port,
                          bf_pm_fsm_t fsm,
                          bf_pm_fsm_st fsm_st_current,
                          // returned values
                          bf_pm_fsm_st *fsm_st_next,
                          uint32_t *wait_ms) {
  bf_pm_fsm_st expected_next_state = BF_PM_FSM_ST_END;
  bf_pm_fsm_state_desc_t *desc_p = NULL;
  uint32_t expected_wait_ms = 0;
  char *alt_str = "";
  bf_status_t st_rc;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;
  if (fsm == NULL) return BF_INVALID_ARG;
  if (wait_ms == NULL) return BF_INVALID_ARG;

  // find the fsm_st state descriptor from the fsm
  if (!bf_pm_fsm_port_is_enabled(dev_id, dev_port)) {
    desc_p = bf_pm_fsm_find_state(fsm, BF_PM_FSM_ST_DISABLED);
    if (desc_p) {
      // show which state we are abandoning
      PM_DEBUG("FSM :%d:%3d:-: %s --> %s",
               dev_id,
               dev_port,
               bf_pm_fsm_st_to_str[fsm_st_current],
               bf_pm_fsm_st_to_str[BF_PM_FSM_ST_DISABLED]);
    }
  } else {
    desc_p = bf_pm_fsm_find_state(fsm, fsm_st_current);
  }
  if (desc_p == NULL) {
    PM_ERROR("FSM :%d:%3d:-: state=%d : state not found?!",
             dev_id,
             dev_port,
             fsm_st_current);
    return BF_INVALID_ARG;
  }
  // make sure we dont jump off to 0 if user forgets to add a func ptr
  if (desc_p->handler != NULL) {
    st_rc = desc_p->handler(dev_id, dev_port);
  } else {
    st_rc = BF_INVALID_ARG;
  }

  if (st_rc == BF_NOT_READY) {
    uint32_t time_in_state;

    if (desc_p->tmout_cycles != BF_FSM_NO_TIMEOUT) {
      // check for timeout
      bf_port_time_in_state_get(dev_id, dev_port, &time_in_state);
      time_in_state++;
      bf_port_time_in_state_set(dev_id, dev_port, time_in_state);
      if (0) {
        PM_DEBUG("FSM :%d:%3d:-: %s <%d of %d>",
                 dev_id,
                 dev_port,
                 bf_pm_fsm_st_to_str[desc_p->state],
                 time_in_state,
                 desc_p->tmout_cycles);
      }
      if (time_in_state >= desc_p->tmout_cycles) {
        st_rc = BF_INVALID_ARG;  // cause abort to be next state
        PM_DEBUG("FSM :%d:%3d:-: %s <timeout>",
                 dev_id,
                 dev_port,
                 bf_pm_fsm_st_to_str[desc_p->state]);
      }
    }
    if (st_rc == BF_NOT_READY) {
      // no transition. stay in current state
      *fsm_st_next = fsm_st_current;
      *wait_ms = desc_p->wait_ms;
      return BF_SUCCESS;
    }
  }

  if (st_rc == BF_SUCCESS) {
    expected_next_state = desc_p->next_state;
    expected_wait_ms = desc_p->next_state_wait_ms;
  } else if (st_rc == BF_ALT2_NEXT_ST) {
    expected_next_state = desc_p->alt_next_state_2;
    expected_wait_ms = desc_p->alt_next_state_2_wait_ms;
    alt_str = "(alt 2)";
  } else if (st_rc == BF_ALT3_NEXT_ST) {
    expected_next_state = desc_p->alt_next_state_3;
    expected_wait_ms = desc_p->alt_next_state_3_wait_ms;
    alt_str = "(alt 3)";
  } else {
    expected_next_state = desc_p->alt_next_state;
    expected_wait_ms = desc_p->alt_next_state_wait_ms;
    alt_str = "(alt)";
  }

  if (expected_next_state != desc_p->state) {
    /* Add a log entry and execute transition function ONLY if next state
     * is different from the current one.
     * Note: transitions to the same state may be used to reinit the timeout.
     */
    PM_DEBUG("FSM :%d:%3d:-: %s --> %s %s",
             dev_id,
             dev_port,
             bf_pm_fsm_st_to_str[desc_p->state],
             bf_pm_fsm_st_to_str[expected_next_state],
             alt_str);

    // transition to next state
    if (desc_p->transition_fn) {
      desc_p->transition_fn(
          dev_id, dev_port, &expected_next_state, &expected_wait_ms);
    }
    bf_port_time_in_state_set(dev_id, dev_port, 0);
  }

  *fsm_st_next = expected_next_state;
  *wait_ms = expected_wait_ms;
  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_port_is_valid
 *
 * Internal function to determine whether port is still in-use (added).
 *****************************************************************************/
bool bf_pm_fsm_port_is_valid(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  bf_status_t rc = bf_port_is_valid(dev_id, dev_port);

  return (rc == BF_SUCCESS);
}

/*****************************************************************************
 * bf_pm_fsm_port_is_enabled
 *
 * Internal function to determine whether port is admin enabled
 *****************************************************************************/
bool bf_pm_fsm_port_is_enabled(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  bf_status_t rc = bf_port_is_enabled(dev_id, dev_port);

  return (rc == BF_SUCCESS);
}

/*****************************************************************************
 * bf_pm_fsm_num_lanes_get
 *
 * Internal function to return number of logical lanes a port uses
 *****************************************************************************/
int bf_pm_fsm_num_lanes_get(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  int num_lanes;
  bf_status_t bf_status;

  bf_status = bf_port_num_lanes_get(dev_id, dev_port, &num_lanes);
  if (bf_status != BF_SUCCESS) {
    PM_ERROR("%s : Failed to get num_lanes for dev %d, dev_port %d",
             __func__,
             dev_id,
             dev_port);
    return 0;
  }
  return num_lanes;
}

/*****************************************************************************
 * bf_pm_fsm_handle_get
 *
 * Returns fsm handler based on port-configuration
 *****************************************************************************/
bf_pm_fsm_t bf_pm_fsm_handle_get(bf_pm_port_fsm_mode_t fsm_mode) {
  switch (fsm_mode) {
    case BF_PM_PORT_FSM_MODE_DFE:
      return bf_pm_get_fsm_for_dfe();
      break;
    case BF_PM_PORT_FSM_MODE_AUTONEG:
      return bf_pm_get_fsm_for_autoneg();
      break;
    case BF_PM_PORT_FSM_MODE_PRBS:
      return bf_pm_get_fsm_for_prbs();
      break;
    case BF_PM_PORT_FSM_MODE_PIPE_LOOPBCK:
      return bf_pm_get_fsm_for_pipe_loopback();
      break;
    case BF_PM_PORT_FSM_MODE_MAC_NEAR_LOOPBCK:
      return bf_pm_get_fsm_for_mac_loopback();
      break;
    case BF_PM_PORT_FSM_MODE_MAC_FAR_LOOPBCK:
      return bf_pm_get_fsm_for_mac_far_loopback();
      break;
    case BF_PM_PORT_FSM_MODE_PCS_LOOPBCK:
      return bf_pm_get_fsm_for_pcs_loopback();
      break;
    case BF_PM_PORT_FSM_MODE_SW_MODEL:
      return bf_pm_get_fsm_for_sw_model();
      break;
    case BF_PM_PORT_FSM_MODE_TX_MODE:
      return bf_pm_get_fsm_for_tx_mode();
      break;
    case BF_PM_PORT_FSM_MODE_EMULATOR:
      return bf_pm_get_fsm_for_emulator();
      break;

    case BF_PM_PORT_FSM_MODE_NONE:
    default:
      break;
  }
  return NULL;
}

/*****************************************************************************
 * bf_pm_fsm_handle_tof3_get
 *
 * Returns fsm handler based on port-configuration
 *****************************************************************************/
bf_pm_fsm_t bf_pm_fsm_tof3_handle_get(bf_pm_port_fsm_mode_t fsm_mode) {
  switch (fsm_mode) {
    case BF_PM_PORT_FSM_MODE_DFE:
      return bf_pm_get_fsm_for_tof3_dfe();
      break;
    case BF_PM_PORT_FSM_MODE_AUTONEG:
      return bf_pm_get_fsm_for_tof3_autoneg();
      break;
    case BF_PM_PORT_FSM_MODE_PRBS:
      return bf_pm_get_fsm_for_tof3_prbs();
      break;
    case BF_PM_PORT_FSM_MODE_PIPE_LOOPBCK:
      return bf_pm_get_fsm_for_tof3_pipe_loopback();
      break;
    case BF_PM_PORT_FSM_MODE_MAC_NEAR_LOOPBCK:
      return bf_pm_get_fsm_for_tof3_mac_loopback();
      break;
    case BF_PM_PORT_FSM_MODE_MAC_FAR_LOOPBCK:
      return bf_pm_get_fsm_for_tof3_mac_far_loopback();
      break;
    case BF_PM_PORT_FSM_MODE_PCS_LOOPBCK:
      return bf_pm_get_fsm_for_tof3_pcs_loopback();
      break;
    case BF_PM_PORT_FSM_MODE_SW_MODEL:
      return bf_pm_get_fsm_for_tof3_sw_model();
      break;
    case BF_PM_PORT_FSM_MODE_TX_MODE:
      return bf_pm_get_fsm_for_tof3_tx_mode();
      break;
    case BF_PM_PORT_FSM_MODE_EMULATOR:
      return bf_pm_get_fsm_for_tof3_emulator();
      break;

    case BF_PM_PORT_FSM_MODE_NONE:
    default:
      break;
  }
  return NULL;
}

/*****************************************************************************
 * bf_pm_fsm_handle_get
 *
 * Returns fsm handler based on port-configuration
 *****************************************************************************/
char *bf_pm_fsm_to_str(bf_pm_fsm_t fsm) {
  if ((fsm == bf_pm_get_fsm_for_dfe()) ||
      (fsm == bf_pm_get_fsm_for_tof3_dfe())) {
    return "Non-AN FSM";
  } else if ((fsm == bf_pm_get_fsm_for_autoneg()) ||
             (fsm == bf_pm_get_fsm_for_tof3_autoneg())) {
    return "AN FSM";
  } else if ((fsm == bf_pm_get_fsm_for_prbs()) ||
             (fsm == bf_pm_get_fsm_for_tof3_prbs())) {
    return "PRBS FSM";
  } else if ((fsm == bf_pm_get_fsm_for_pipe_loopback()) ||
             (fsm == bf_pm_get_fsm_for_tof3_pipe_loopback())) {
    return "Pipe Loopback FSM";
  } else if ((fsm == bf_pm_get_fsm_for_mac_loopback()) ||
             (fsm == bf_pm_get_fsm_for_tof3_mac_loopback())) {
    return "MAC Near Loopback FSM";
  } else if ((fsm == bf_pm_get_fsm_for_mac_far_loopback()) ||
             (fsm == bf_pm_get_fsm_for_tof3_mac_far_loopback())) {
    return "MAC Far Loopback FSM";
  } else if ((fsm == bf_pm_get_fsm_for_pcs_loopback()) ||
             (fsm == bf_pm_get_fsm_for_tof3_pcs_loopback())) {
    return "PCS Loopback FSM";
  } else if ((fsm == bf_pm_get_fsm_for_sw_model()) ||
             (fsm == bf_pm_get_fsm_for_tof3_sw_model())) {
    return "SW Model FSM";
  } else if ((fsm == bf_pm_get_fsm_for_tx_mode()) ||
             (fsm == bf_pm_get_fsm_for_tof3_tx_mode())) {
    return "Tx-Only Mode FSM";
  } else if ((fsm == bf_pm_get_fsm_for_emulator()) ||
             (fsm == bf_pm_get_fsm_for_tof3_emulator())) {
    return "Emulator Mode FSM";
  }
  return "<invalid FSM>";
}

/*****************************************************************************
 * bf_pm_fsm_display_get
 *
 * Returns strings for FSM and FSM state for display
 *****************************************************************************/
void bf_pm_fsm_display_get(bf_pm_fsm_t fsm,
                           bf_pm_fsm_st st,
                           char **fsm_str,
                           char **fsm_st_str) {
  *fsm_str = bf_pm_fsm_to_str(fsm);
  *fsm_st_str = bf_pm_fsm_state_to_str(st);
}

bf_status_t bf_pm_fsm_port_disable(bf_dev_id_t dev_id,
                                   bf_dev_port_t dev_port,
                                   bf_pm_fsm_t fsm) {
  bf_pm_fsm_state_desc_t *desc_p = NULL;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;
  if (fsm == NULL) return BF_INVALID_ARG;

  // Get port-disable handler
  desc_p = bf_pm_fsm_find_state(fsm, BF_PM_FSM_ST_DISABLED);
  if (desc_p && desc_p->handler) {
    PM_DEBUG("FSM: port-disable :%d:%3d: %s",
             dev_id,
             dev_port,
             bf_pm_fsm_st_to_str[BF_PM_FSM_ST_DISABLED]);
    desc_p->handler(dev_id, dev_port);
  }

  return BF_SUCCESS;
}
