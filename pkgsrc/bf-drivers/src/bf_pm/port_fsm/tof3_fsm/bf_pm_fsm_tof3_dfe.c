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

  if (!bf_port_is_rx_only(dev_id, dev_port)) {
    // indicate Tx serdes initialized
    pm_port_serdes_tx_ready_set(dev_id, dev_port, true);
  }

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

  // release MAC reset
  bf_port_tof3_tmac_reset_set(dev_id, dev_port, 0);

  for (int ln = 0; ln < num_lanes; ln++) {
    bf_tof3_serdes_config_mode(dev_id, dev_port, ln);
  }

  if (bf_port_is_rx_only(dev_id, dev_port)) {
    for (int ln = 0; ln < num_lanes; ln++) {
      bf_tof3_serdes_squelch_set(dev_id, dev_port, ln, 1);
    }
    bf_port_mac_tx_enable_set(dev_id, dev_port, false);
    bf_port_tx_drain_set(dev_id, dev_port, true);
  }

  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_wait_rx_ready
 *
 * Wait for platform indication that module RX is ready to sense
 */
static bf_status_t bf_pm_fsm_wait_rx_ready(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port) {
  bool do_dfe;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  // check if datapath is really ready
  pm_port_serdes_rx_ready_for_bringup_get(dev_id, dev_port, &do_dfe);
  if (!do_dfe) return BF_NOT_READY;

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

  if (bf_port_is_rx_only(dev_id, dev_port))
    pm_port_serdes_tx_ready_set(dev_id, dev_port, false);

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

  pm_port_serdes_rx_ready_set(dev_id, dev_port, true);

  /* archive HW stats as they will be cleared during rx_reset */
  bf_port_mac_stats_historical_update_set(dev_id, dev_port);

  // assert "force tx RF" to keep link-partner Dn
  // bf_port_force_remote_fault_set(dev_id, dev_port, true);

  // bring-up UMAC
  // bf_port_rx_reset_set(dev_id, dev_port);
  // bf_port_un_force_sig_ok_low_set(dev_id, dev_port);
  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_check_link_state
 *
 * Internal function to test operational (link) state and issue
 * state-change callbacks if required.
 */
static bf_status_t bf_pm_fsm_check_link_state(bf_dev_id_t dev_id,
                                              bf_dev_port_t dev_port,
                                              int exp_st) {
  int state = exp_st;
  bf_status_t rc;
  bool pending;

  rc = bf_port_oper_state_get(dev_id, dev_port, &state);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;
  if (state == exp_st) return BF_NOT_READY;

  rc = bf_port_is_oper_state_callback_pending(dev_id, dev_port, &pending);
  if ((rc == BF_SUCCESS) && pending) {
    // log state change information
    bf_pm_log_state_chg_info(dev_id, dev_port, state == 0 ? false : true);
  }
  return rc;
}

/*****************************************************************************
 * bf_pm_fsm_check_link_state_extended
 *
 * Internal function to test operational (link) state and issue
 * state-change callbacks if required.
 */
static bf_status_t bf_pm_fsm_check_link_state_extended(bf_dev_id_t dev_id,
                                                       bf_dev_port_t dev_port,
                                                       bool force_log,
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
  if (force_log) {
    PM_DEBUG("FSM :%d:%3d:-: state=%d PCS_st=%x l_fault=%d r_fault =%d",
             dev_id,
             dev_port,
             *state,
             *pcs_rdy,
             *l_fault,
             *r_fault);
  }
  /* if current state is the expected one, return here. Decisions about
   * state transitions ar deferred to the state handler.
   */
  if (*state == exp_st) return BF_SUCCESS;

  rc = bf_port_is_oper_state_callback_pending(dev_id, dev_port, &pending);
  if ((rc == BF_SUCCESS) && pending) {
    // log state change information
    bf_pm_log_state_chg_info(dev_id, dev_port, state == 0 ? false : true);
    if (!force_log) {
      PM_DEBUG("FSM :%d:%3d:-: state=%d PCS_st=%x l_fault=%d r_fault =%d",
               dev_id,
               dev_port,
               *state,
               *pcs_rdy,
               *l_fault,
               *r_fault);
    }
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
  int state;
  bf_status_t rc;
  bool pending;
  int exp_st = 0;

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
  rc = bf_pm_fsm_check_link_state_extended(dev_id,
                                           dev_port,
                                           BF_PM_FSM_LINK_STATUS_VERBOSE_MODE,
                                           0,
                                           &st,
                                           &pcs_rdy,
                                           &local_fault,
                                           &remote_fault);
  if (rc != BF_SUCCESS) return rc;

  if (st) {
    /* link up debouncing */
    if (bf_pm_port_debounce_adj_chk(dev_id, dev_port)) {
      /* link is UP, transition to BF_PM_FSM_ST_LINK_UP state */
      bf_port_link_fault_status_set(dev_id, dev_port, BF_PORT_LINK_FAULT_OK);
      bf_port_oper_state_set_pending_callbacks(dev_id, dev_port, st);
      return BF_SUCCESS;
    }
    return BF_NOT_READY;
  }

  bf_pm_port_debounce_restore(dev_id, dev_port);

  if (!pcs_rdy) {
    /* PCS still not ready: stay at LINK_DN state, with timeout */
    return BF_NOT_READY;
  } else if (local_fault) {
    /* receiving local_fault ordered set: stay at LINK_DN state, no timeout */
    return BF_ALT2_NEXT_ST;
  } else if (remote_fault) {
    /* receiving remote_fault ordered set: transition to REMOTE_FAULT state */
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
 * PCS is up but link-partner is indicating a fault on their side.
 */
static bf_status_t bf_pm_fsm_remote_fault(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port) {
  bool remote_fault;
  bool local_fault;
  bf_status_t rc;
  bool pcs_rdy;
  int st = 0;

  return 0;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  /* check link status, expected to be down (0) */
  rc = bf_pm_fsm_check_link_state_extended(dev_id,
                                           dev_port,
                                           BF_PM_FSM_LINK_STATUS_VERBOSE_MODE,
                                           0,
                                           &st,
                                           &pcs_rdy,
                                           &local_fault,
                                           &remote_fault);
  if (rc != BF_SUCCESS) return rc;

  if (st) {
    /* link up debouncing */
    if (bf_pm_port_debounce_adj_chk(dev_id, dev_port)) {
      /* link is UP, transition to BF_PM_FSM_ST_LINK_UP state */
      bf_port_link_fault_status_set(dev_id, dev_port, BF_PORT_LINK_FAULT_OK);
      bf_port_oper_state_set_pending_callbacks(dev_id, dev_port, st);
      return BF_SUCCESS;
    }
    return BF_NOT_READY;
  }

  bf_pm_port_debounce_restore(dev_id, dev_port);

  if (!pcs_rdy) {
    /* PCS not ready: transition to ABORT state */
    bf_port_link_fault_status_set(
        dev_id, dev_port, BF_PORT_LINK_FAULT_LOC_FAULT);
    return BF_INVALID_ARG;
  } else if (local_fault) {
    /* receiving local fault ordered set: transition to LINK_DN state */
    bf_port_link_fault_status_set(
        dev_id, dev_port, BF_PORT_LINK_FAULT_LOC_FAULT);
    return BF_ALT2_NEXT_ST;
  } else if (remote_fault) {
    /* still receiving remote_fault: stay at REMOTE_FAULT state */
    return BF_NOT_READY;
  }
  return BF_INVALID_ARG;
}

/*****************************************************************************
 * bf_pm_fsm_wait_link_down
 *
 * Wait for MAC link-down indication
 */
static bf_status_t bf_pm_fsm_wait_link_down(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port) {
#if 1
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

#else
  bool remote_fault;
  bool local_fault;
  bf_status_t rc;
  bool pcs_rdy;
  int st = 1;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  /* check link status, expected to be up (1) */
  rc = bf_pm_fsm_check_link_state_extended(
      dev_id, dev_port, false, 1, &st, &pcs_rdy, &local_fault, &remote_fault);
  if (rc != BF_SUCCESS) return rc;

  if (st) {
    /* link remains up, stay at LINK_UP state */
    rc = BF_NOT_READY;
  } else if (!pcs_rdy) {
    /* link down, transition to ABORT state */
    bf_port_link_fault_status_set(
        dev_id, dev_port, BF_PORT_LINK_FAULT_LOC_FAULT);
    rc = BF_INVALID_ARG;
  } else if (local_fault) {
    /* receiving local_fault ordered set, transition to LINK_DN state */
    bf_port_link_fault_status_set(
        dev_id, dev_port, BF_PORT_LINK_FAULT_LOC_FAULT);
    rc = BF_ALT2_NEXT_ST;
  } else if (remote_fault) {
    /* receiving remote_fault ordered set, transition to REMOTE_FAULT state */
    bf_port_link_fault_status_set(
        dev_id, dev_port, BF_PORT_LINK_FAULT_REM_FAULT);
    rc = BF_ALT3_NEXT_ST;
  } else {
    rc = BF_INVALID_ARG;
  }

  if (rc != BF_NOT_READY) {
    /* There is a state change, set pending callback flag */
    bf_port_oper_state_set_pending_callbacks(dev_id, dev_port, st);
  }

  return rc;
#endif
}

/*****************************************************************************
 * bf_pm_fsm_abort
 *
 * Restart FSM bring-up from wait_signal state
 */
static bf_status_t bf_pm_fsm_abort(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  int ln, num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  // disconnect UMAC from serdes Rx
  // bf_port_force_sig_ok_low_set(dev_id, dev_port);

  // assert "force tx RF" to keep link-partner Dn
  // bf_port_force_remote_fault_set(dev_id, dev_port, true);

  // disable tx-drain
  // bf_port_tx_drain_set(dev_id, dev_port, false);

  // clear rate chg requests in case we were waiting for done
  for (ln = 0; ln < num_lanes; ln++) {
    bf_tof3_serdes_tx_rate_change_done(dev_id, dev_port, ln);
    bf_tof3_serdes_rx_rate_change_done(dev_id, dev_port, ln);
  }

  // reset MAC
  bf_port_tof3_tmac_reset_set(dev_id, dev_port, 1);
  bf_port_tof3_tmac_reset_set(dev_id, dev_port, 0);

  pm_port_serdes_rx_ready_set(dev_id, dev_port, false);

  bf_port_tx_drain_set(dev_id, dev_port, false);
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

  // re-assert MAC reset?
  bf_port_tof3_tmac_reset_set(dev_id, dev_port, 1);

  bf_pm_fsm_check_link_state(dev_id, dev_port, 1 /*exp up*/);

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
  bf_port_tx_drain_set(dev_id, dev_port, false);

  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_ber_check_start
 *
 * if port uses PAM4 serdes, then it uses RS-FEC. Use the RS FEC symbol
 * error counters to estimate BER. This is the first step, clearing the
 * counters.
 * Counters will be read in bf_pm_fsm_ber_check_done to determine
 * whether or not to restart adaptation.
 */
static bf_status_t bf_pm_fsm_ber_check_start(bf_dev_id_t dev_id,
                                             bf_dev_port_t dev_port) {
  int /*ln,*/ num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);
  bf_status_t rc;
  uint32_t n_ctrs, first_ctr;
  (void)first_ctr;
  (void)n_ctrs;
  bf_serdes_encoding_mode_t enc_mode;
  bf_port_speed_t speed;
  uint32_t umac;
  uint32_t ch_ln;
  bool is_cpu_port;

  rc = port_mgr_tof3_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch_ln, &is_cpu_port);
  if (rc != BF_SUCCESS) {
    // de-assert "force tx RF" to let link-partner Up
    bf_port_force_remote_fault_set(dev_id, dev_port, false);
    return BF_SUCCESS;  // no-op for NRZ speeds
  }
  bf_port_speed_get(dev_id, dev_port, &speed);
  bf_serdes_encoding_mode_get(speed, num_lanes, &enc_mode);
  if (enc_mode != BF_SERDES_ENC_MODE_PAM4) {
    // de-assert "force tx RF" to let link-partner Up
    bf_port_force_remote_fault_set(dev_id, dev_port, false);

    return BF_SUCCESS;
  }

  if (speed == BF_SPEED_400G) {
    n_ctrs = 16;
    first_ctr = 0;
  } else if (speed == BF_SPEED_200G) {
    n_ctrs = 8;
    first_ctr = (dev_port & 0x7) * 2;
  } else if ((speed == BF_SPEED_100G) && (num_lanes == 2)) {
    n_ctrs = 4;
    first_ctr = (dev_port & 0x7) * 2;
  } else if ((speed == BF_SPEED_50G) && (num_lanes == 1)) {
    n_ctrs = 2;
    first_ctr = (dev_port & 0x7) * 2;
  } else {
    return BF_SUCCESS;
  }
  // read and discard counters (read-to-clear)
  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_ber_check_done
 *
 * if port uses PAM4 serdes, then it uses RS-FEC. We cleared the FEC
 * counters in bf_pm_fsm_ber_check_start. We are here after a fixed delay.
 * So we read the FEC symbol error counters and compute an estimated BER.
 * If the estimated BER is > 1e-05, restart adaptation.
 */
static bf_status_t bf_pm_fsm_ber_check_done(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port) {
  int /*ln,*/ num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);
  bf_status_t rc;
  uint32_t ctr, n_ctrs, first_ctr, end_ctr;
  (void)end_ctr;
  (void)ctr;
  bf_serdes_encoding_mode_t enc_mode;
  bf_port_speed_t speed;
  uint32_t tmac, ch_ln;
  bool is_cpu_port;
  uint64_t bps, gb;
  int integration_time = 100000;  // us? or ms?
  bool hi_ber = false;

  rc = port_mgr_tof3_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &tmac, &ch_ln, &is_cpu_port);
  if (rc != BF_SUCCESS) {
    return BF_SUCCESS;  // no-op for NRZ speeds
  }
  bf_port_speed_get(dev_id, dev_port, &speed);
  bf_serdes_encoding_mode_get(speed, num_lanes, &enc_mode);
  if (enc_mode != BF_SERDES_ENC_MODE_PAM4) {
    return BF_SUCCESS;
  }

  gb = 1000ul * 1000ul * 1000ul;

  if (speed == BF_SPEED_400G) {
    n_ctrs = 16;
    first_ctr = 0;
    bps = (8 * 53125ul * gb) / 1000ul;
  } else if (speed == BF_SPEED_200G) {
    n_ctrs = 8;
    /* for 200G: (dev_port & 0x7) -> 0, 4 */
    first_ctr = (dev_port & 0x4) * 2;
    bps = (4 * 53125ul * gb) / 1000ul;
  } else if ((speed == BF_SPEED_100G) && (num_lanes == 2)) {
    n_ctrs = 4;
    /* for 100G: (dev_port & 0x7) -> 0, 2, 4, 6 */
    first_ctr = (dev_port & 0x6) * 2;
    bps = (2 * 53125ul * gb) / 1000ul;
  } else if ((speed == BF_SPEED_50G) && (num_lanes == 1)) {
    n_ctrs = 2;
    /* for 100G: (dev_port & 0x7) -> 0, 1, 2, 3, 4, 5, 6, 7 */
    first_ctr = (dev_port & 0x7) * 2;
    bps = (1 * 53125ul * gb) / 1000ul;
  } else {
    return BF_SUCCESS;
  }
  end_ctr = (first_ctr + n_ctrs);
  bps = (bps * integration_time) / 1000000;

  // read FEC symbol error counters
  // compute BER
  if (hi_ber) {
    PM_DEBUG("   FSM :%d:%3d:-: HI_BER. Force re-adapt ..", dev_id, dev_port);
    //  return BF_INVALID_ARG;  // try again
  }
  // de-assert "force tx RF" to let link-partner Up
  // bf_port_force_remote_fault_set(dev_id, dev_port, false);

  return BF_SUCCESS;
}

/*
  BF_PM_FSM_ST_WAIT_TX_RATE_CHG_DONE,
  BF_PM_FSM_ST_WAIT_RX_RATE_CHG_DONE,
  BF_PM_FSM_ST_WAIT_CDR_LOCK,
*/
bf_pm_fsm_state_desc_t bf_pm_fsm_tof3_dfe[] = {
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

     .next_state = BF_PM_FSM_ST_WAIT_RX_READY,
     .next_state_wait_ms = 500,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_RX_READY,
     .handler = bf_pm_fsm_wait_rx_ready,
     .wait_ms = 100,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_WAIT_SIGNAL_OK,
     .next_state_wait_ms = 30,

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
     .next_state_wait_ms = 1000,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_DFE_DONE,
     .handler = bf_pm_fsm_wait_dfe_done,
     .wait_ms = 200,
     .tmout_cycles = 50,

     .next_state = BF_PM_FSM_ST_WAIT_CDR_LOCK,
     .next_state_wait_ms = 100,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_CDR_LOCK,
     .handler = bf_pm_fsm_wait_cdr_lock,
     .wait_ms = 200,
     .tmout_cycles = 40,

     .next_state = BF_PM_FSM_ST_BER_CHECK_START,
     .next_state_wait_ms = 100,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_BER_CHECK_START,
     .handler = bf_pm_fsm_ber_check_start,
     .wait_ms = 0,
     .tmout_cycles = 0,

     .next_state = BF_PM_FSM_ST_BER_CHECK_DONE,
     .next_state_wait_ms = 100,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_BER_CHECK_DONE,
     .handler = bf_pm_fsm_ber_check_done,
     .wait_ms = 0,
     .tmout_cycles = 0,

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
     .tmout_cycles = 100,

     .next_state = BF_PM_FSM_ST_LINK_UP,
     .next_state_wait_ms = 0,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_LINK_DN,
     .alt_next_state_2_wait_ms = 100,
     .alt_next_state_3 = BF_PM_FSM_ST_REMOTE_FAULT,
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
bf_pm_fsm_t bf_pm_get_fsm_for_tof3_dfe(void) { return bf_pm_fsm_tof3_dfe; }
