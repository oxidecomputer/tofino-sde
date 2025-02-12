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
#include <lld/lld_efuse.h>
#include <port_mgr/bf_port_if.h>
#include <port_mgr/port_mgr_tof3/aw_lane_cfg.h>
#include <port_mgr/bf_tof3_serdes_if.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include "../bf_pm_fsm_if.h"
#include "../../bf_pm.h"
#include "../../pm_log.h"

void bf_pm_log_state_chg_info(uint32_t dev_id, uint32_t dev_port, bool st);
extern bf_status_t port_mgr_tof3_map_dev_port_to_all(bf_dev_id_t dev_id,
                                                     bf_dev_port_t dev_port,
                                                     uint32_t *pipe_id,
                                                     uint32_t *port_id,
                                                     uint32_t *umac,
                                                     uint32_t *ch,
                                                     bool *is_cpu_port);

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

  for (ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof3_serdes_config_ln(
        dev_id, dev_port, ln, speed, num_lanes, prbs_mode, prbs_mode);
    if (rc != BF_SUCCESS) {
      PM_ERROR("Error: %d : from bf_tof3_serdes_config_ln\n", rc);
      return BF_INVALID_ARG;
    }
  }

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

  for (int ln = 0; ln < num_lanes; ln++) {
    bf_tof3_serdes_config_mode(dev_id, dev_port, ln);
  }
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
  bool sig_detect;
  (void)sig_detect;
  (void)ppm;
  (void)rc;
  int num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  // wait for signal-detect on all lanes
  for (ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof3_serdes_wait_sig_ok(dev_id, dev_port, ln);
    if (rc == BF_NOT_READY) {
      return rc;
    } else if (rc != BF_SUCCESS) {
      return rc;
    }
  }

  // check if datapath is really ready
  // pm_port_serdes_rx_ready_for_bringup_get(dev_id, dev_port, &do_dfe);
  // if (!do_dfe) return BF_NOT_READY;

  bf_port_signal_detect_time_set(dev_id, dev_port);

  for (ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof3_serdes_eq_start(dev_id, dev_port, ln);
    if (rc != BF_SUCCESS) {
      return rc;
    }
  }
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
  (void)rc;
  int ln;
  int num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  // wait for EQ done on all lanes
  for (ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof3_serdes_check_eq_done(dev_id, dev_port, ln);
    if (rc == BF_NOT_READY) {
      return rc;
    } else if (rc != BF_SUCCESS) {
      return rc;
    }
  }
  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_wait_cdr_lock
 *
 * Wait for Rx EQ (adapation) done indications from all lanes
 */
static bf_status_t bf_pm_fsm_wait_cdr_lock(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port) {
  bf_status_t rc;
  int ln;
  int num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  // wait for EQ done on all lanes
  for (ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof3_serdes_check_cdr_lock(dev_id, dev_port, ln);
    if (rc == BF_NOT_READY) {
      return rc;
    } else if (rc != BF_SUCCESS) {
      return rc;
    }
  }
  // start PRBS checker
  for (ln = 0; ln < num_lanes; ln++) {
    bf_tof3_serdes_check_bist_lock(dev_id, dev_port, ln);
  }

  pm_port_serdes_rx_ready_set(dev_id, dev_port, true);
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
  int ln, num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  // clear rate chg requests in case we were waiting for done
  for (ln = 0; ln < num_lanes; ln++) {
    bf_tof3_serdes_tx_rate_change_done(dev_id, dev_port, ln);
    bf_tof3_serdes_rx_rate_change_done(dev_id, dev_port, ln);
  }

  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_disable_port
 *
 * Port being disabled
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

/*
  BF_PM_FSM_ST_WAIT_TX_RATE_CHG_DONE,
  BF_PM_FSM_ST_WAIT_RX_RATE_CHG_DONE,
  BF_PM_FSM_ST_WAIT_CDR_LOCK,
*/
bf_pm_fsm_state_desc_t bf_pm_fsm_tof3_prbs[] = {
    {.state = BF_PM_FSM_ST_IDLE,
     .handler = bf_pm_fsm_init_serdes,
     .wait_ms = 0,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_WAIT_TX_RATE_CHG_DONE,
     .next_state_wait_ms = 50,

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

     .next_state = BF_PM_FSM_ST_WAIT_RX_RATE_CHG_DONE,
     .next_state_wait_ms = 50,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_RX_RATE_CHG_DONE,
     .handler = bf_pm_fsm_wait_rx_rate_chg_done,
     .wait_ms = 10,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_WAIT_SIGNAL_OK,
     .next_state_wait_ms = 0,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_SIGNAL_OK,
     .handler = bf_pm_fsm_wait_rx_signal,
     .wait_ms = 100,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_WAIT_DFE_DONE,
     .next_state_wait_ms = 3500,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_DFE_DONE,
     .handler = bf_pm_fsm_wait_dfe_done,
     .wait_ms = 1000,
     .tmout_cycles = 10,

     .next_state = BF_PM_FSM_ST_WAIT_CDR_LOCK,
     .next_state_wait_ms = 2000,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_CDR_LOCK,
     .handler = bf_pm_fsm_wait_cdr_lock,
     .wait_ms = 1000,
     .tmout_cycles = 10,

     .next_state = BF_PM_FSM_ST_MONITOR_PRBS_ERRORS,
     .next_state_wait_ms = 1000,

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
 * bf_pm_get_fsm_for_tof3_dfe
 ************************************************************************/
bf_pm_fsm_t bf_pm_get_fsm_for_tof3_prbs(void) { return bf_pm_fsm_tof3_prbs; }
