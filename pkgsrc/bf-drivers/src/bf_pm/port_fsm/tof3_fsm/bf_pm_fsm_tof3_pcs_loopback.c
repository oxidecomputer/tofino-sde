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


#include <stdint.h>
#include <stdbool.h>

#include <bf_types/bf_types.h>
#include <dvm/bf_drv_intf.h>
#include <port_mgr/bf_port_if.h>
#include <port_mgr/port_mgr_tof3/aw_lane_cfg.h>
#include <port_mgr/bf_tof3_serdes_if.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include "../bf_pm_fsm_if.h"
#include "../../pm_log.h"

extern void bf_pm_log_state_chg_info(uint32_t dev_id,
                                     uint32_t dev_port,
                                     bool st);
/*****************************************************************************
 * bf_pm_fsm_cfg_pcs_loopback
 *
 * Configure the serdes lanes implementing this dev_port.
 */
static bf_status_t bf_pm_fsm_cfg_pcs_loopback(bf_dev_id_t dev_id,
                                              bf_dev_port_t dev_port) {
  bf_status_t rc;
  int ln;
  bf_port_speed_t speed;
  int num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  rc = bf_port_speed_get(dev_id, dev_port, &speed);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;

  for (ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof3_serdes_config_ln(dev_id,
                                  dev_port,
                                  ln,
                                  speed,
                                  num_lanes,
                                  BF_PORT_PRBS_MODE_NONE,
                                  BF_PORT_PRBS_MODE_NONE);
    if (rc != BF_SUCCESS) {
      PM_ERROR("Error: %d : from bf_tof3_serdes_config_ln\n", rc);
      return BF_INVALID_ARG;
    }
  }
  bf_port_loopback_mode_set(dev_id, dev_port, BF_LPBK_PCS_NEAR);

  // request Tx rate chg
  for (ln = 0; ln < num_lanes; ln++) {
    bf_tof3_serdes_apply_tx_rate(dev_id, dev_port, ln);
  }

  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_wait_tx_rate_chg_done
 *
 * Wait for Tx state change ack
 */
static bf_status_t bf_pm_fsm_wait_tx_rate_chg_done(bf_dev_id_t dev_id,
                                                   bf_dev_port_t dev_port) {
  bf_status_t rc;
  int num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  for (int ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof3_serdes_wait_tx_rate_change_done(dev_id, dev_port, ln);
    if (rc == BF_NOT_READY) {
      return BF_NOT_READY;
    } else if (rc != BF_SUCCESS) {
      PM_ERROR("Error: %d : from bf_tof3_serdes_wait_tx_rate_change_done\n",
               rc);
      return BF_INVALID_ARG;
    }
  }

  // release MAC reset
  bf_port_tof3_tmac_reset_set(dev_id, dev_port, 0);

  // request Rx rate chg
  // for (int ln = 0; ln < num_lanes; ln++) {
  //  bf_tof3_serdes_apply_rx_rate(dev_id, dev_port, ln);
  //}
  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_abort
 *
 * Restart FSM bring-up from wait_signal state
 */
static bf_status_t bf_pm_fsm_abort(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  // disconnect UMAC from serdes Rx
  //  bf_port_force_sig_ok_low_set(dev_id, dev_port);

  // clear all loopbakcs
  //  bf_port_loopback_mode_set(dev_id, dev_port, BF_LPBK_NONE);

  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_disable_port
 *
 * Restart FSM bring-up from wait_signal state
 */
static bf_status_t bf_pm_fsm_disable_port(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port) {
  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  // disconnect UMAC from serdes Rx
  bf_port_force_sig_ok_low_set(dev_id, dev_port);

  // clear all loopbakcs
  bf_port_loopback_mode_set(dev_id, dev_port, BF_LPBK_NONE);

  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_check_link_state_extended
 *
 * Internal function to test operational (link) state and issue
 * state-change callbacks if required.
 */
static bf_status_t bf_pm_fsm_check_link_state_extended(bf_dev_id_t dev_id,
                                                       bf_dev_port_t dev_port,
                                                       int exp_st,
                                                       int *state,
                                                       bool *pcs_rdy,
                                                       bool *l_fault,
                                                       bool *r_fault) {
  bf_status_t rc;
  bool pending;

  if (!state) return BF_INVALID_ARG;

  rc = bf_port_oper_state_get_extended(
      dev_id, dev_port, state, pcs_rdy, l_fault, r_fault);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;
  if (*state == exp_st) return BF_SUCCESS;

  rc = bf_port_is_oper_state_callback_pending(dev_id, dev_port, &pending);
  if ((rc == BF_SUCCESS) && pending) {
    // log state change information
    bf_pm_log_state_chg_info(dev_id, dev_port, state == 0 ? false : true);
  }
  return rc;
}

/*****************************************************************************
 * bf_pm_fsm_wait_link_up
 *
 * Wait for MAC link-up indication
 */
static bf_status_t bf_pm_fsm_wait_link_up(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port) {
  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  bf_status_t rc;
  bool pending;
  int exp_st = 0;
  int state = exp_st;

  rc = bf_port_oper_state_get(dev_id, dev_port, &state);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;
  if (state == exp_st) return BF_NOT_READY;

  rc = bf_port_is_oper_state_callback_pending(dev_id, dev_port, &pending);
  if ((rc == BF_SUCCESS) && pending) {
    // log state change information
    bf_pm_log_state_chg_info(dev_id, dev_port, state == 0 ? false : true);

    rc = bf_port_oper_state_callbacks_issue(dev_id, dev_port);
  }

  return rc;

#if 0
  bool remote_fault;
  bool local_fault;
  bf_status_t rc;
  bool pcs_rdy;
  int st = 0;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  /* check link status, expected to be down (0) */
  rc = bf_pm_fsm_check_link_state_extended(
      dev_id, dev_port, 0, &st, &pcs_rdy, &local_fault, &remote_fault);
  if (rc != BF_SUCCESS) return rc;

  if (st) {
    /* link is UP, transition to BF_PM_FSM_ST_LINK_UP state */
    bf_port_link_fault_status_set(dev_id, dev_port, BF_PORT_LINK_FAULT_OK);
    return BF_SUCCESS;
  } else if (!pcs_rdy) {
    /* PCS still not ready: stay at LINK_DN state, with timeout */
    return BF_NOT_READY;
  } else if (local_fault) {
    /* receiving local_fault ordered set: stay at LINK_DN state, no timeout.
     * Note: when near PCS loopback is enabled, this only may happen if the MAC
     * is forced to transmit local_fault ordered sets.
     */
    return BF_ALT2_NEXT_ST;
  } else if (remote_fault) {
    /* receiving remote_fault ordered set: transition to REMOTE_FAULT state
     * Note: when near PCS loopback is enabled, this only may happen if the MAC
     * is forced to transmit remote_fault ordered sets.
     */
    bf_port_link_fault_status_set(
        dev_id, dev_port, BF_PORT_LINK_FAULT_REM_FAULT);
    return BF_ALT3_NEXT_ST;
  }
  return BF_NOT_READY;
#endif
}

/*****************************************************************************
 * bf_pm_fsm_remote_fault
 *
 * Remote fault, wait for MAC link-up indication
 */
static bf_status_t bf_pm_fsm_remote_fault(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port) {
  bool remote_fault;
  bool local_fault;
  bf_status_t rc;
  bool pcs_rdy;
  int st = 0;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  return BF_SUCCESS;

#if 1
  /* check link status, expected to be down (0) */
  rc = bf_pm_fsm_check_link_state_extended(
      dev_id, dev_port, 0, &st, &pcs_rdy, &local_fault, &remote_fault);
  if (rc != BF_SUCCESS) return rc;

  if (st) {
    /* link is UP, transition to BF_PM_FSM_ST_LINK_UP state */
    bf_port_link_fault_status_set(dev_id, dev_port, BF_PORT_LINK_FAULT_OK);
    return BF_SUCCESS;
  } else if (!pcs_rdy) {
    /* transition to BF_PM_FSM_ST_LINK_DN state */
    bf_port_link_fault_status_set(
        dev_id, dev_port, BF_PORT_LINK_FAULT_LOC_FAULT);
    return BF_INVALID_ARG;
  } else if (local_fault) {
    /* receiving local fault ordered sets: transition to LINK_DN state */
    bf_port_link_fault_status_set(
        dev_id, dev_port, BF_PORT_LINK_FAULT_LOC_FAULT);
    return BF_ALT2_NEXT_ST;
  } else if (remote_fault) {
    /* still receiving remote_fault ordered sets: stay at REMOTE_FAULT state */
    return BF_NOT_READY;
  }
  return BF_INVALID_ARG;
#endif
}

/*****************************************************************************
 * bf_pm_fsm_wait_link_down
 *
 * Wait for MAC link-down indication
 */
static bf_status_t bf_pm_fsm_wait_link_down(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port) {
  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  bf_status_t rc;
  bool pending;
  int exp_st = 1;
  int state = exp_st;

  rc = bf_port_oper_state_get(dev_id, dev_port, &state);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;
  if (state == exp_st) return BF_NOT_READY;

  rc = bf_port_is_oper_state_callback_pending(dev_id, dev_port, &pending);
  if ((rc == BF_SUCCESS) && pending) {
    // log state change information
    bf_pm_log_state_chg_info(dev_id, dev_port, state == 0 ? false : true);

    rc = bf_port_oper_state_callbacks_issue(dev_id, dev_port);
  }

  return rc;

#if 0

  bool remote_fault;
  bool local_fault;
  bf_status_t rc;
  bool pcs_rdy;
  int st = 1;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  /* check link status, expected to be up (1) */
  rc = bf_pm_fsm_check_link_state_extended(
      dev_id, dev_port, 1, &st, &pcs_rdy, &local_fault, &remote_fault);
  if (rc != BF_SUCCESS) return rc;

  if (st) {
    /* link remains up, stay at LINK_UP state */
    return BF_NOT_READY;
  } else if (!pcs_rdy) {
    /* link down, transition to ABORT state */
    bf_port_link_fault_status_set(
        dev_id, dev_port, BF_PORT_LINK_FAULT_LOC_FAULT);
    return BF_INVALID_ARG;
  } else if (local_fault) {
    /* receiving local_fault ordered sets, transition to LINK_DN state */
    bf_port_link_fault_status_set(
        dev_id, dev_port, BF_PORT_LINK_FAULT_LOC_FAULT);
    return BF_ALT2_NEXT_ST;
  } else if (remote_fault) {
    /* receiving remote_fault ordered sets, transition to REMOTE_FAULT state */
    bf_port_link_fault_status_set(
        dev_id, dev_port, BF_PORT_LINK_FAULT_REM_FAULT);
    return BF_ALT3_NEXT_ST;
  }
  return BF_INVALID_ARG;
#endif
}

bf_pm_fsm_state_desc_t bf_pm_fsm_tof3_pcs_loopback[] = {
    {.state = BF_PM_FSM_ST_IDLE,
     .handler = bf_pm_fsm_cfg_pcs_loopback,
     .wait_ms = 0,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_WAIT_TX_RATE_CHG_DONE,
     .next_state_wait_ms = 20,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_TX_RATE_CHG_DONE,
     .handler = bf_pm_fsm_wait_tx_rate_chg_done,
     .wait_ms = 10,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_LINK_DN,
     .next_state_wait_ms = 0,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_LINK_DN,
     .handler = bf_pm_fsm_wait_link_up,
     .wait_ms = 100,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_LINK_UP,
     .next_state_wait_ms = 0,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_LINK_DN,
     .alt_next_state_2_wait_ms = 100,
     .alt_next_state_3 = BF_PM_FSM_ST_REMOTE_FAULT,
     .alt_next_state_3_wait_ms = 100,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_REMOTE_FAULT,
     .handler = bf_pm_fsm_remote_fault,
     .wait_ms = 100,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_LINK_UP,
     .next_state_wait_ms = 0,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_LINK_DN,
     .alt_next_state_2_wait_ms = 100,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_LINK_UP,
     .handler = bf_pm_fsm_wait_link_down,
     .wait_ms = 100,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_ABORT,
     .next_state_wait_ms = 0,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_LINK_DN,
     .alt_next_state_2_wait_ms = 100,
     .alt_next_state_3 = BF_PM_FSM_ST_REMOTE_FAULT,
     .alt_next_state_3_wait_ms = 100,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_ABORT,
     .handler = bf_pm_fsm_abort,
     .wait_ms = 0,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_IDLE,
     .next_state_wait_ms = 100,

     .alt_next_state = BF_PM_FSM_ST_IDLE,
     .alt_next_state_wait_ms = 100,
     .alt_next_state_2 = BF_PM_FSM_ST_END,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_END,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_DISABLED,
     .handler = bf_pm_fsm_disable_port,
     .wait_ms = 0,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_END,
     .next_state_wait_ms = 0,

     .alt_next_state = BF_PM_FSM_ST_END,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_END,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_END,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_END,
     .handler = NULL,
     .wait_ms = 0,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_END,
     .next_state_wait_ms = 0,

     .alt_next_state = BF_PM_FSM_ST_END,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_END,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_END,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},
};

/************************************************************************
 * bf_pm_get_fsm_for_tof3_pcs_loopback
 ************************************************************************/
bf_pm_fsm_t bf_pm_get_fsm_for_tof3_pcs_loopback(void) {
  return bf_pm_fsm_tof3_pcs_loopback;
}
