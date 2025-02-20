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

extern void bf_pm_log_state_chg_info(uint32_t dev_id,
                                     uint32_t dev_port,
                                     bool st);
/*****************************************************************************
 * bf_pm_fsm_cfg_tx_mode
 *
 */
static bf_status_t bf_pm_fsm_cfg_tx_mode(bf_dev_id_t dev_id,
                                         bf_dev_port_t dev_port) {
  bf_status_t rc;
  int ln;
  bf_port_speed_t speed;
  int num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  rc = bf_port_speed_get(dev_id, dev_port, &speed);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;

  bf_port_mac_rx_enable_set(dev_id, dev_port, false);

  for (ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof2_serdes_config_ln(dev_id,
                                  dev_port,
                                  ln,
                                  speed,
                                  num_lanes,
                                  BF_PORT_PRBS_MODE_NONE,
                                  BF_PORT_PRBS_MODE_NONE,
                                  false);
    if (rc != BF_SUCCESS) {
      PM_ERROR("Error: %d : from bf_tof2_serdes_config_ln\n", rc);
      return BF_INVALID_ARG;
    }
  }

  // MAC interrutps must be disabled
  bf_port_local_fault_int_set(dev_id, dev_port, false);
  bf_port_remote_fault_int_set(dev_id, dev_port, false);

  // disconnect UMAC from serdes Rx
  bf_port_force_sig_ok_low_set(dev_id, dev_port);

  // indicate Tx serdes initialized
  pm_port_serdes_tx_ready_set(dev_id, dev_port, true);

  pm_port_serdes_rx_ready_set(dev_id, dev_port, false);

  bf_port_mac_set_tx_mode(dev_id, dev_port, true);

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
  bf_port_force_sig_ok_low_set(dev_id, dev_port);

  // clear tx-only mode config
  bf_port_mac_set_tx_mode(dev_id, dev_port, false);

  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_disable_port
 *
 * Restart FSM bring-up from wait_signal state
 */
static bf_status_t bf_pm_fsm_disable_port(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port) {
  int ln, num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);
  bf_status_t rc;
  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  // disconnect UMAC from serdes Rx
  bf_port_force_sig_ok_low_set(dev_id, dev_port);

  // clear tx-only mode config
  bf_port_mac_set_tx_mode(dev_id, dev_port, false);

  // update port state to DOWN
  bf_port_oper_state_set_pending_callbacks(dev_id, dev_port, 0);
  bf_pm_log_state_chg_info(dev_id, dev_port, false);

  pm_port_serdes_rx_ready_set(dev_id, dev_port, false);
  pm_port_serdes_tx_ready_set(dev_id, dev_port, false);

  // un-configure all lanes. This will also squelch Tx output
  for (ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof2_serdes_config_ln(dev_id,
                                  dev_port,
                                  ln,
                                  BF_SPEED_NONE,
                                  num_lanes,
                                  BF_PORT_PRBS_MODE_NONE,
                                  BF_PORT_PRBS_MODE_NONE,
                                  false);
    if (rc != BF_SUCCESS) {
      PM_ERROR("Error: %d : from bf_tof2_serdes_config_ln\n", rc);
      return BF_INVALID_ARG;
    }
  }

  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_wait_link_up
 *
 * Wait for MAC link-up indication
 */
static bf_status_t bf_pm_fsm_wait_link_up(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port) {
  bool update_link = false;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  bf_pm_port_module_ready_for_link_handling_get(dev_id, dev_port, &update_link);
  if (update_link == false) return BF_NOT_READY;

  // update port state to UP
  bf_port_oper_state_set_pending_callbacks(dev_id, dev_port, 1);
  bf_pm_log_state_chg_info(dev_id, dev_port, true);

  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_wait_link_down
 *
 * Wait for MAC link-down indication
 */
static bf_status_t bf_pm_fsm_wait_link_down(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port) {
  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  return BF_NOT_READY;
}

bf_pm_fsm_state_desc_t bf_pm_fsm_tx_mode[] = {
    {.state = BF_PM_FSM_ST_IDLE,
     .handler = bf_pm_fsm_cfg_tx_mode,
     .wait_ms = 500,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_LINK_DN,
     .next_state_wait_ms = 1000,

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
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
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
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_ABORT,
     .handler = bf_pm_fsm_abort,
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
 * bf_pm_get_fsm_for_tx_mode
 ************************************************************************/
bf_pm_fsm_t bf_pm_get_fsm_for_tx_mode(void) { return bf_pm_fsm_tx_mode; }
