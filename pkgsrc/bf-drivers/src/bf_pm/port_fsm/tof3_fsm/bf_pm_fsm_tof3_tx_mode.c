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
#include "../../bf_pm.h"
#include "../../pm_log.h"

extern void bf_pm_log_state_chg_info(uint32_t dev_id,
                                     uint32_t dev_port,
                                     bool st);

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

  // bf_pm_port_debounce_restore(dev_id, dev_port);

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
  // clear rate chg requests only after all requests are complete
  for (int ln = 0; ln < num_lanes; ln++) {
    bf_tof3_serdes_tx_rate_change_done(dev_id, dev_port, ln);
  }

  // indicate Tx serdes initialized
  pm_port_serdes_tx_ready_set(dev_id, dev_port, true);

  // request Rx rate chg
  for (int ln = 0; ln < num_lanes; ln++) {
    bf_tof3_serdes_apply_rx_rate(dev_id, dev_port, ln);
  }
  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_wait_rx_rate_chg_done
 *
 * Wait for Rx state change ack
 */
static bf_status_t bf_pm_fsm_wait_rx_rate_chg_done(bf_dev_id_t dev_id,
                                                   bf_dev_port_t dev_port) {
  bf_status_t rc;
  int num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);
  // bf_port_speed_t speed;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  for (int ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof3_serdes_wait_rx_rate_change_done(dev_id, dev_port, ln);
    if (rc == BF_NOT_READY) {
      return BF_NOT_READY;
    } else if (rc != BF_SUCCESS) {
      PM_ERROR("Error: %d : from bf_tof3_serdes_wait_rx_rate_change_done\n",
               rc);
      return BF_INVALID_ARG;
    }
  }
  // clear rate chg requests only after all requests are complete
  for (int ln = 0; ln < num_lanes; ln++) {
    bf_tof3_serdes_rx_rate_change_done(dev_id, dev_port, ln);
  }

  // release MAC reset
  bf_port_tof3_tmac_reset_set(dev_id, dev_port, 0);

  for (int ln = 0; ln < num_lanes; ln++) {
    bf_tof3_serdes_config_mode(dev_id, dev_port, ln);
  }

  for (int ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof3_serdes_eq_start(dev_id, dev_port, ln);
    if (rc != BF_SUCCESS) {
      return rc;
    }
  }

  return BF_SUCCESS;
}
/*****************************************************************************
 * bf_pm_fsm_cfg_tx_mode
 *
 * config tx mode for port
 */
static bf_status_t bf_pm_fsm_cfg_tx_mode(bf_dev_id_t dev_id,
                                         bf_dev_port_t dev_port) {
  bf_status_t rc;
  int ln;
  int num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);
  // bf_port_speed_t speed;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  bf_port_signal_detect_time_set(dev_id, dev_port);

  for (ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof3_serdes_eq_start(dev_id, dev_port, ln);
    if (rc != BF_SUCCESS) {
      return rc;
    }
  }

  bf_port_mac_rx_enable_set(dev_id, dev_port, false);
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

  // clear rate chg requests in case we were waiting for done
  for (ln = 0; ln < num_lanes; ln++) {
    bf_tof3_serdes_tx_rate_change_done(dev_id, dev_port, ln);
    bf_tof3_serdes_rx_rate_change_done(dev_id, dev_port, ln);
  }

  // re-assert MAC reset?
  bf_port_tof3_tmac_reset_set(dev_id, dev_port, 1);

  // bf_pm_fsm_check_link_state(dev_id, dev_port, 1 /*exp up*/);
  bf_port_oper_state_set_pending_callbacks(dev_id, dev_port, 0);
  bf_pm_log_state_chg_info(dev_id, dev_port, true);

  pm_port_serdes_rx_ready_set(dev_id, dev_port, false);
  pm_port_serdes_tx_ready_set(dev_id, dev_port, false);

  // power-down all lanes
  for (ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof3_serdes_config_ln(dev_id,
                                  dev_port,
                                  ln,
                                  BF_SPEED_NONE,
                                  num_lanes,
                                  BF_PORT_PRBS_MODE_NONE,
                                  BF_PORT_PRBS_MODE_NONE);
    if (rc != BF_SUCCESS) {
      PM_ERROR("Error: %d : from bf_tof3_serdes_config_ln\n", rc);
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
  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;
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

bf_pm_fsm_state_desc_t bf_pm_fsm_tof3_tx_mode[] = {
    {.state = BF_PM_FSM_ST_IDLE,
     .handler = bf_pm_fsm_init_serdes,
     .wait_ms = 0,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_WAIT_TX_RATE_CHG_DONE,
     .next_state_wait_ms = 30,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_TX_RATE_CHG_DONE,
     .handler = bf_pm_fsm_wait_tx_rate_chg_done,
     .wait_ms = 30,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_WAIT_RX_RATE_CHG_DONE,
     .next_state_wait_ms = 30,  // 50, // needs a little more time?

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_RX_RATE_CHG_DONE,
     .handler = bf_pm_fsm_wait_rx_rate_chg_done,
     .wait_ms = 30,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_CFG_TX_MODE,
     //.next_state = BF_PM_FSM_ST_WAIT_SIGNAL_OK,
     .next_state_wait_ms = 5000,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},
    {.state = BF_PM_FSM_ST_CFG_TX_MODE,
     .handler = bf_pm_fsm_cfg_tx_mode,
     .wait_ms = 100,
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
 * bf_pm_get_fsm_for_tof3_tx_mode
 ************************************************************************/
bf_pm_fsm_t bf_pm_get_fsm_for_tof3_tx_mode(void) {
  return bf_pm_fsm_tof3_tx_mode;
}
