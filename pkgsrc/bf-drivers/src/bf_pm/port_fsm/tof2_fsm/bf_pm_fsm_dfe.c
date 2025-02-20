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
#include <port_mgr/bf_tof2_serdes_if.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include "../bf_pm_fsm_if.h"
#include "../../bf_pm.h"
#include "../../pm_log.h"
#include <port_mgr/port_mgr_tof2/umac4_ctrs.h>
#include <port_mgr/port_mgr_tof2/port_mgr_tof2_serdes.h>

// hack
uint32_t port_mgr_tof2_serdes_a0_upper_bits_get(bf_dev_id_t dev_id,
                                                bf_dev_port_t dev_port,
                                                uint32_t ln);
void port_mgr_tof2_serdes_a0_upper_bits_set(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port,
                                            uint32_t ln,
                                            uint32_t x3_bits);

void bf_pm_log_state_chg_info(uint32_t dev_id, uint32_t dev_port, bool st);
extern bf_status_t port_mgr_tof2_map_dev_port_to_all(bf_dev_id_t dev_id,
                                                     bf_dev_port_t dev_port,
                                                     uint32_t *pipe_id,
                                                     uint32_t *port_id,
                                                     uint32_t *umac,
                                                     uint32_t *ch,
                                                     bool *is_cpu_port);

void verify_a0_upper_bits_000(bf_dev_id_t dev_id,
                              bf_dev_port_t dev_port,
                              uint32_t n_lanes) {
  uint32_t ln;

  for (ln = 0; ln < n_lanes; ln++) {
    uint32_t bits =
        port_mgr_tof2_serdes_a0_upper_bits_get(dev_id, dev_port, ln);
    if (((bits >> 13) & 0x7) != 0) {
      PM_DEBUG("   FSM :%d:%3d:%d: WARNIN: A0[15:13] not 000b ! <%4x>",
               dev_id,
               dev_port,
               ln,
               bits);
      port_mgr_tof2_serdes_a0_upper_bits_set(dev_id, dev_port, ln, 0);
      PM_DEBUG("   FSM :%d:%3d:%d: WARNIN: A0[15:13] Fixed ..",
               dev_id,
               dev_port,
               ln);
    }
  }
}
void verify_a0_upper_bits_0X1(bf_dev_id_t dev_id,
                              bf_dev_port_t dev_port,
                              uint32_t n_lanes) {
  uint32_t ln;

  for (ln = 0; ln < n_lanes; ln++) {
    uint32_t bits =
        port_mgr_tof2_serdes_a0_upper_bits_get(dev_id, dev_port, ln);
    if (((bits >> 13) & 0x5) != 1) {
      PM_DEBUG("   FSM :%d:%3d:%d: WARNIN: A0[15:13] not 0X1b ! <%4x>",
               dev_id,
               dev_port,
               ln,
               bits);
    }
  }
}

static bf_status_t bf_fsm_dfe_config_tx_taps(bf_dev_id_t dev_id,
                                             bf_dev_port_t dev_port) {
  int32_t pre2;
  int32_t pre1;
  int32_t main_tap;  // compiler warns about "main" not being a fn
  int32_t post1;
  int32_t post2;
  int ln;
  int num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);

  for (ln = 0; ln < num_lanes; ln++) {
    // get configured values
    bf_tof2_serdes_tx_taps_get(
        dev_id, dev_port, ln, &pre2, &pre1, &main_tap, &post1, &post2);
    // unsquelch tx output
    bf_tof2_serdes_tx_taps_set(dev_id,
                               dev_port,
                               ln,
                               pre2,
                               pre1,
                               main_tap,
                               post1,
                               post2,
                               true /*apply to hw*/);
  }
  return BF_SUCCESS;
}

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

  // hack, verify 0xA0[15:13] == 011
  verify_a0_upper_bits_0X1(dev_id, dev_port, num_lanes);

  rc = bf_port_speed_get(dev_id, dev_port, &speed);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;

  bf_port_link_fault_status_set(dev_id, dev_port, BF_PORT_LINK_FAULT_LOC_FAULT);

  for (ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof2_serdes_config_ln(dev_id,
                                  dev_port,
                                  ln,
                                  speed,
                                  num_lanes,
                                  BF_PORT_PRBS_MODE_NONE,
                                  BF_PORT_PRBS_MODE_NONE,
                                  true);
    if (rc != BF_SUCCESS) {
      PM_ERROR("Error: %d : from bf_tof2_serdes_config_ln\n", rc);
      return BF_INVALID_ARG;
    }
  }

  bf_pm_port_debounce_restore(dev_id, dev_port);

  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_wait_pll_ready
 *
 * Wait for Tx PLL ready (for now it is just a fixed delay of 100ms)
 */
static bf_status_t bf_pm_fsm_wait_pll_ready(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port) {
  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  if (!bf_port_is_rx_only(dev_id, dev_port)) {
    // disconnect UMAC from serdes Rx
    bf_port_force_sig_ok_low_set(dev_id, dev_port);

    if (!bf_port_is_decoupled_mode(dev_id, dev_port)) {
      // make sure that "tx_ignore_rx" is disabled
      bf_port_tx_ignore_rx_set(dev_id, dev_port, false);

      // assert "force tx RF" to keep link-partner Dn
      bf_port_force_remote_fault_set(dev_id, dev_port, true);
    } else {
      // Port is in decoupled mode: set "tx_ignore_rx"
      bf_port_tx_ignore_rx_set(dev_id, dev_port, true);
    }

    // Reset UMAC Tx once PCS Tx clk is stable
    bf_port_tx_reset_set(dev_id, dev_port);

    // un-squelch serdes output
    bf_fsm_dfe_config_tx_taps(dev_id, dev_port);

    // indicate Tx serdes initialized
    pm_port_serdes_tx_ready_set(dev_id, dev_port, true);
  } else {
    // RX-ONLY mode: set txdrain
    bf_port_tx_drain_set(dev_id, dev_port, true);
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
  bool sig_detect, phy_ready, do_dfe;
  int num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  // hack, verify 0xA0[15:13] == 000
  verify_a0_upper_bits_000(dev_id, dev_port, num_lanes);

  if (!bf_port_is_rx_only(dev_id, dev_port)) {
    // check if datapath is really ready
    pm_port_serdes_rx_ready_for_bringup_get(dev_id, dev_port, &do_dfe);
    if (!do_dfe) return BF_NOT_READY;
  }

  for (ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof2_serdes_rx_sig_info_get(
        dev_id, dev_port, ln, &sig_detect, &phy_ready, &ppm);
    if (rc != BF_SUCCESS) return BF_INVALID_ARG;
    if (!sig_detect || !phy_ready) return BF_NOT_READY;
  }

  /* reset serdes to force re-adapt */
  for (ln = 0; ln < num_lanes; ln++) {
    bf_tof2_serdes_lane_reset_set(dev_id, dev_port, ln);
  }

  bf_port_signal_detect_time_set(dev_id, dev_port);
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

  pm_port_serdes_rx_ready_set(dev_id, dev_port, true);

  /* archive HW stats as they will be cleared during rx_reset */
  bf_port_mac_stats_historical_update_set(dev_id, dev_port);

  // assert "force tx RF" to keep link-partner Dn
  bf_port_force_remote_fault_set(dev_id, dev_port, true);

  // bring-up UMAC
  bf_port_rx_reset_set(dev_id, dev_port);
  bf_port_un_force_sig_ok_low_set(dev_id, dev_port);
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
  bf_port_speed_t speed;

  rc = bf_port_oper_state_get(dev_id, dev_port, &state);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;

  /* If link state is up, then validate serdes status as well. Sometimes the PCS
   * status shows link is up eventhough the Serdes signal detect is failing.
   * Validating the same with serdes status prevents any false link up
   * scenarios. Limiting this extra validation to only 10G & 25G speed since the
   * issue have been observed in this speed config.
   */
  rc = bf_port_speed_get(dev_id, dev_port, &speed);
  if ((rc == BF_SUCCESS) && state &&
      ((speed == BF_SPEED_10G) || (speed == BF_SPEED_25G))) {
    bool srds_sts = false;
    rc = bf_port_serdes_sig_sts_get(dev_id, dev_port, &srds_sts);
    if ((rc == BF_SUCCESS) && !srds_sts) {
      state = false;
    }
  }

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
  bf_port_speed_t speed;

  rc = bf_port_oper_state_get_extended(
      dev_id, dev_port, state, pcs_rdy, l_fault, r_fault);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;
  if (!state) return BF_INVALID_ARG;
  if (force_log) {
    PM_DEBUG("FSM :%d:%3d:-: state=%d PCS_st=%x l_fault=%d r_fault =%d",
             dev_id,
             dev_port,
             *state,
             *pcs_rdy,
             *l_fault,
             *r_fault);
  }

  /* If link state is up, then validate serdes status as well. Sometimes the PCS
   * status shows link is up eventhough the Serdes signal detect is failing.
   * Validating the same with serdes status prevents any false link up
   * scenarios. Limiting this extra validation to only 10G & 25G speed since the
   * issue have been observed in this speed config.
   */
  rc = bf_port_speed_get(dev_id, dev_port, &speed);
  if ((rc == BF_SUCCESS) && *state &&
      ((speed == BF_SPEED_10G) || (speed == BF_SPEED_25G))) {
    bool srds_sts = false;
    rc = bf_port_serdes_sig_sts_get(dev_id, dev_port, &srds_sts);
    if ((rc == BF_SUCCESS) && !srds_sts) {
      *state = false;
    }
  }
  /* if current state is the expected one, return here. Decisions about
   * state transitions ar deferred to the state handler.
   */
  if (*state == exp_st) return BF_SUCCESS;

  rc = bf_port_is_oper_state_callback_pending(dev_id, dev_port, &pending);
  if ((rc == BF_SUCCESS) && pending) {
    // log state change information
    bf_pm_log_state_chg_info(dev_id, dev_port, *state == 0 ? false : true);
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
      /* restore the debounce counter in success return*/
      bf_pm_port_debounce_restore(dev_id, dev_port);
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
      /* restore the debounce counter in success return*/
      bf_pm_port_debounce_restore(dev_id, dev_port);
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
  bool remote_fault;
  bool local_fault;
  bf_status_t rc;
  bool pcs_rdy;
  int st = 1;
  bf_port_speed_t speed;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  if (!bf_port_is_decoupled_mode(dev_id, dev_port)) {
    /* Default direction mode: check link status, expected to be up (1) */
    rc = bf_pm_fsm_check_link_state_extended(
        dev_id, dev_port, false, 1, &st, &pcs_rdy, &local_fault, &remote_fault);
    if (rc != BF_SUCCESS) return rc;

    /* Handle mac/pcs stuck condition:
     * If link state is up, then validate serdes status as well. Sometimes the
     * PCS status shows link is up eventhough the Serdes signal detect is
     * failing. Validating the same with serdes status prevents any false link
     * up scenarios. Limiting this extra validation to only 10G & 25G speed
     * since the issue have been observed in this speed config.
     *
     * Note on Interrupt: If mac/pcs is in stuck condition, interrupt won't be
     * triggered and sw.per_state will remain 1. oper_state_get_extended api
     * will return st = sw.oper_state = 1. Hence below check is still valid for
     * interrupt case too.
     *
     * However pcs_rdy, local/remote are not valid in both the cases.
     */
    rc = bf_port_speed_get(dev_id, dev_port, &speed);
    if ((rc == BF_SUCCESS) && st &&
        ((speed == BF_SPEED_10G) || (speed == BF_SPEED_25G))) {
      bool srds_sts = false;
      rc = bf_port_serdes_sig_sts_get(dev_id, dev_port, &srds_sts);
      if ((rc == BF_SUCCESS) && !srds_sts) {
        st = 0;
        pcs_rdy = false;
        bf_port_link_fault_status_set(
            dev_id, dev_port, BF_PORT_LINK_FAULT_LOC_FAULT);
        bf_port_oper_state_set_force_callbacks(dev_id, dev_port, true);
        bf_port_oper_state_set_pending_callbacks(dev_id, dev_port, st);
        return BF_INVALID_ARG;
      }
    }
  } else {
    // Decoupled mode: assume the link remains UP after first link UP
    st = 1;
    pcs_rdy = true;
    remote_fault = false;
    local_fault = false;
  }

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
}

/*****************************************************************************
 * bf_pm_fsm_abort
 *
 * Restart FSM bring-up from wait_signal state
 */
static bf_status_t bf_pm_fsm_abort(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  int num_lanes;
  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  // disconnect UMAC from serdes Rx
  bf_port_force_sig_ok_low_set(dev_id, dev_port);

  // assert "force tx RF" to keep link-partner Dn
  bf_port_force_remote_fault_set(dev_id, dev_port, true);

  // disable tx-drain
  bf_port_tx_drain_set(dev_id, dev_port, false);

  // disable "tx_ignore_rx"
  bf_port_tx_ignore_rx_set(dev_id, dev_port, false);

  pm_port_serdes_rx_ready_set(dev_id, dev_port, false);

  /* reset serdes to force re-adapt */
  num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);
  for (int ln = 0; ln < num_lanes; ln++) {
    bf_tof2_serdes_lane_reset_set(dev_id, dev_port, ln);
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

  // hack for bring-up
  bf_port_un_force_sig_ok_high_set(dev_id, dev_port);

  // disconnect UMAC from serdes Rx
  bf_port_force_sig_ok_low_set(dev_id, dev_port);
  bf_pm_fsm_check_link_state(dev_id, dev_port, 1 /*exp up*/);

  pm_port_serdes_rx_ready_set(dev_id, dev_port, false);
  pm_port_serdes_tx_ready_set(dev_id, dev_port, false);

  // de-assert "force tx RF"
  bf_port_force_remote_fault_set(dev_id, dev_port, false);

  // disable tx-drain
  bf_port_tx_drain_set(dev_id, dev_port, false);

  // disable "tx_ignore_rx"
  bf_port_tx_ignore_rx_set(dev_id, dev_port, false);

  // un-configure all lanes. This will also squelch Tx output
  for (ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof2_serdes_config_ln(dev_id,
                                  dev_port,
                                  ln,
                                  BF_SPEED_NONE,
                                  num_lanes,
                                  BF_PORT_PRBS_MODE_NONE,
                                  BF_PORT_PRBS_MODE_NONE,
                                  true);
    if (rc != BF_SUCCESS) {
      PM_ERROR("Error: %d : from bf_tof2_serdes_config_ln\n", rc);
      return BF_INVALID_ARG;
    }
  }

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
  uint32_t /*ctr,*/ n_ctrs, first_ctr;  //, end_ctr;
  bf_serdes_encoding_mode_t enc_mode;
  bf_port_speed_t speed;
  umac4_rs_fec_ln_ctr_t fec_ln_ctrs;
  // uint64_t *ctrs = (uint64_t *)&fec_ln_ctrs;
  uint32_t umac;
  uint32_t ch_ln;
  bool is_cpu_port;

  rc = port_mgr_tof2_map_dev_port_to_all(
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
  umac4_ctrs_rs_fec_ln_range_get(dev_id, umac, first_ctr, n_ctrs, &fec_ln_ctrs);
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
  bf_serdes_encoding_mode_t enc_mode;
  bf_port_speed_t speed;
  umac4_rs_fec_ln_ctr_t fec_ln_ctrs;
  uint64_t *ctrs = (uint64_t *)&fec_ln_ctrs;
  uint32_t umac;
  uint32_t ch_ln;
  bool is_cpu_port;
  uint64_t bps, gb, s_errs;       //, tot_s_errs = 0ul;
  int integration_time = 100000;  // us? or ms?
  bool hi_ber = false;

  rc = port_mgr_tof2_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch_ln, &is_cpu_port);
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
  umac4_ctrs_rs_fec_ln_range_get(dev_id, umac, first_ctr, n_ctrs, &fec_ln_ctrs);

  for (ctr = first_ctr; ctr < end_ctr; ctr++) {
    float ber;
    s_errs = ctrs[ctr];
    ber = (float)(s_errs * 1) /
          (float)(bps / ((uint64_t)n_ctrs));  // 10b symbols, ea lane
                                              // carries 1/n_ctrs of the
                                              // bits
    PM_DEBUG("   FSM :%d:%3d:-: FEC ctr: %d : BER=%8.1e",
             dev_id,
             dev_port,
             ctr,
             ber);
    if (ber > 1.0e-05) {
      hi_ber = true;
    }
  }
  if (hi_ber) {
    PM_DEBUG("   FSM :%d:%3d:-: HI_BER. Force re-adapt ..", dev_id, dev_port);
    return BF_INVALID_ARG;  // try again
  }
  // de-assert "force tx RF" to let link-partner Up
  bf_port_force_remote_fault_set(dev_id, dev_port, false);

  return BF_SUCCESS;
}

bf_pm_fsm_state_desc_t bf_pm_fsm_dfe[] = {
    {.state = BF_PM_FSM_ST_IDLE,
     .handler = bf_pm_fsm_init_serdes,
     .wait_ms = 0,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_WAIT_PLL_READY,
     .next_state_wait_ms = 100,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_PLL_READY,
     .handler = bf_pm_fsm_wait_pll_ready,
     .wait_ms = 100,
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
     .tmout_cycles = 120,

     .next_state = BF_PM_FSM_ST_BER_CHECK_START,
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
     .tmout_cycles = 300,

     .next_state = BF_PM_FSM_ST_LINK_UP,
     .next_state_wait_ms = 0,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_LINK_DN,
     .alt_next_state_2_wait_ms = 100,
     .alt_next_state_3 = BF_PM_FSM_ST_REMOTE_FAULT,
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
 * bf_pm_get_fsm_for_dfe
 ************************************************************************/
bf_pm_fsm_t bf_pm_get_fsm_for_dfe(void) { return bf_pm_fsm_dfe; }
