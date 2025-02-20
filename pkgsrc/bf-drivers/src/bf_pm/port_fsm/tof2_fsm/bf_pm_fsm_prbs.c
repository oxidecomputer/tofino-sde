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
#include <port_mgr/bf_tof2_serdes_if.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include "../bf_pm_fsm_if.h"
#include "../../bf_pm.h"
#include "../../pm_log.h"

/*****************************************************************************
 * bf_pm_fsm_init_serdes
 *
 * Configure the serdes lanes implementing this dev_port.
 */
static bf_status_t bf_pm_fsm_init_serdes(bf_dev_id_t dev_id,
                                         bf_dev_port_t dev_port) {
  bf_status_t rc;
  int ln;
  bf_port_speed_t speed;
  bf_port_prbs_mode_t prbs_mode;
  int num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  rc = bf_port_speed_get(dev_id, dev_port, &speed);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;

  rc = bf_port_prbs_mode_get(dev_id, dev_port, &prbs_mode);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;

  // Make sure MAC doesnt see any of this
  bf_port_force_sig_ok_low_set(dev_id, dev_port);
  bf_port_link_fault_status_set(dev_id, dev_port, BF_PORT_LINK_FAULT_LOC_FAULT);

  for (ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof2_serdes_config_ln(
        dev_id, dev_port, ln, speed, num_lanes, prbs_mode, prbs_mode, false);
    if (rc != BF_SUCCESS) {
      PM_ERROR("Error: %d : from bf_tof2_serdes_config_ln\n", rc);
      return BF_INVALID_ARG;
    }
  }
  // indicate Tx serdes initialized
  pm_port_serdes_tx_ready_set(dev_id, dev_port, true);

  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_wait_rx_signal
 *
 * Wait for sig_detect and phy_ready indications from all lanes
 */
static bf_status_t bf_pm_fsm_wait_rx_signal(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port) {
  bf_status_t rc;
  int ln;
  int32_t ppm;
  bool sig_detect, phy_ready;
  int num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  for (ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof2_serdes_rx_sig_info_get(
        dev_id, dev_port, ln, &sig_detect, &phy_ready, &ppm);
    if (rc != BF_SUCCESS) return BF_INVALID_ARG;
    if (!sig_detect || !phy_ready) return BF_NOT_READY;
  }
#if 0
  // Lanes are ready for Rx EQ. issue a lane reset to get DFE (re-)started
  for (ln = 0; ln < num_lanes; ln++) {
    bf_tof2_serdes_lane_reset_set(dev_id, dev_port, ln);
  }
#endif
  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_wait_dfe_done
 *
 * Wait for Rx EQ (adapation) done indications from all lanes
 */
static bf_status_t bf_pm_fsm_wait_dfe_done(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port) {
  bf_status_t rc;
  int ln;
  uint32_t adapt_cnt, readapt_cnt, link_lost_cnt;
  bool adapt_done;
  int num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  for (ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof2_serdes_adapt_counts_get(dev_id,
                                         dev_port,
                                         ln,
                                         &adapt_done,
                                         &adapt_cnt,
                                         &readapt_cnt,
                                         &link_lost_cnt);
    if (rc != BF_SUCCESS) return BF_INVALID_ARG;
    if (!adapt_done) return BF_NOT_READY;
  }

  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_monitor_prbs_errors
 *
 * No actions in FSM here. This is the state in which the user can monitor
 * the PRBS error counts using CLI commands.
 *
 * Only exit is port disable
 */
static bf_status_t bf_pm_fsm_monitor_prbs_errors(bf_dev_id_t dev_id,
                                                 bf_dev_port_t dev_port) {
  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  return BF_NOT_READY;
}

/*****************************************************************************
 * bf_pm_fsm_abort
 *
 * Restart FSM bring-up from wait_signal state
 */
static bf_status_t bf_pm_fsm_abort(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  // disconnect UMAC from serdes Rx
  bf_port_force_sig_ok_low_set(dev_id, dev_port);

  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_disable_port
 *
 * Restart FSM bring-up from wait_signal state
 */
static bf_status_t bf_pm_fsm_disable_port(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port) {
  bf_status_t rc;
  int ln, num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  // disconnect UMAC from serdes Rx (just in case)
  bf_port_force_sig_ok_low_set(dev_id, dev_port);

  // un-configure all lanes. This will also squelch Tx output
  for (ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof2_serdes_config_ln(dev_id,
                                  dev_port,
                                  ln,
                                  BF_SPEED_NONE,
                                  num_lanes,
                                  BF_PORT_PRBS_MODE_31,
                                  BF_PORT_PRBS_MODE_31,
                                  true);
    if (rc != BF_SUCCESS) {
      PM_ERROR("Error: %d : from bf_tof2_serdes_config_ln\n", rc);
      return BF_INVALID_ARG;
    }
  }
  return BF_SUCCESS;
}

bf_pm_fsm_state_desc_t bf_pm_fsm_prbs[] = {
    {.state = BF_PM_FSM_ST_IDLE,
     .handler = bf_pm_fsm_init_serdes,
     .wait_ms = 0,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_WAIT_SIGNAL_OK,
     .next_state_wait_ms = 50,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_SIGNAL_OK,
     .handler = bf_pm_fsm_wait_rx_signal,
     .wait_ms = 500,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_WAIT_DFE_DONE,
     .next_state_wait_ms = 0,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_DFE_DONE,
     .handler = bf_pm_fsm_wait_dfe_done,
     .wait_ms = 500,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_MONITOR_PRBS_ERRORS,
     .next_state_wait_ms = 100,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_MONITOR_PRBS_ERRORS,
     .handler = bf_pm_fsm_monitor_prbs_errors,
     .wait_ms = 0,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_ABORT,
     .next_state_wait_ms = 0,

     .alt_next_state = BF_PM_FSM_ST_END,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_END,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_END,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_ABORT,
     .handler = bf_pm_fsm_abort,
     .wait_ms = 0,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_WAIT_SIGNAL_OK,
     .next_state_wait_ms = 0,

     .alt_next_state = BF_PM_FSM_ST_END,
     .alt_next_state_wait_ms = 0,
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
 * bf_pm_get_fsm_for_prbs
 ************************************************************************/
bf_pm_fsm_t bf_pm_get_fsm_for_prbs(void) { return bf_pm_fsm_prbs; }
