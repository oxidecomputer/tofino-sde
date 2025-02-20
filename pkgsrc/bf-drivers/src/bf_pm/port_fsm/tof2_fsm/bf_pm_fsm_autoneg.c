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
#include <inttypes.h>

#include <bf_types/bf_types.h>
#include <dvm/bf_drv_intf.h>
#include <port_mgr/bf_port_if.h>
#include <port_mgr/bf_tof2_serdes_if.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include <bf_pm/bf_pm_intf.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include "../bf_pm_fsm_if.h"
#include "../../pm_log.h"

uint32_t port_mgr_tof2_serdes_tile_rd(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      uint32_t ofs);

bf_port_speeds_t pm_port_speed_to_an_speed_map(bf_dev_id_t dev_id,
                                               bf_port_speed_t p_speed,
                                               uint32_t n_lanes,
                                               bool adv_kr_mode,
                                               bool rsfec_support);

bf_fec_types_t pm_port_fec_to_an_fec_map(bf_fec_type_t p_fec,
                                         bf_port_speed_t p_speed);

void bf_pm_log_state_chg_info(uint32_t dev_id, uint32_t dev_port, bool st);

extern bool tof2_an_dbg;
extern bf_status_t port_mgr_tof2_serdes_an_lt_debug_get(bf_dev_id_t dev_id,
                                                        bf_dev_port_t dev_port,
                                                        uint32_t ln,
                                                        uint32_t *r_800c7,
                                                        uint32_t *r_800c8,
                                                        uint32_t *r_800c9,
                                                        uint32_t *r_800ca,
                                                        uint32_t *r_800cb);

#if 0
static bf_status_t bf_pm_fsm_an_status_get(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           uint32_t *an_status);
static bf_status_t bf_pm_fsm_an_hcd_get(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        uint32_t *hcd);
#endif

extern bf_status_t port_mgr_tof2_map_dev_port_to_all(bf_dev_id_t dev_id,
                                                     bf_dev_port_t dev_port,
                                                     uint32_t *pipe_id,
                                                     uint32_t *port_id,
                                                     uint32_t *umac,
                                                     uint32_t *ch,
                                                     bool *is_cpu_port);
#if 0
typedef struct {
  uint32_t lane_mode;
  uint32_t an_sts;
  uint32_t lt_status;
  uint32_t lane_speed;
  uint32_t fw_an_status;
  uint32_t fw_an_hcd;
  uint32_t readout_state;
  uint32_t frame_lock;
  uint32_t rx_trained;
  uint32_t readout_training_state;
  uint32_t training_failure;
  uint32_t tx_training_data_en;
  uint32_t sig_det;
  uint32_t readout_txstate;
  uint32_t adapt_done;
  uint32_t adapt_cnt;
  uint32_t readapt_cnt;
  uint32_t link_lost_cnt;
} lt_debug_state_t;

lt_debug_state_t lt_st[1024][8] = {{{0}}};
#endif

/*
 BlueJay FW 1.2.X LT (Exit codes?)
#define STATE_LT_WAIT_NRZ_FRAME_LOCK        0xD100
#define STATE_LT_WAIT_REMOTE_LOCK           0xD180
#define STATE_LT_WAIT_NRZ_INITIAL_CMD       0xD200
#define STATE_LT_TX_BLIND_ADJUST            0xD300
#define STATE_LT_NRZ_CNTR_TARGET            0xD400
#define STATE_LT_CTLE_FINE                  0xD500
#define STATE_LT_TX_NRZ_ADJUST              0xD600
#define STATE_LT_FFE_ON                     0xD640
#define STATE_LT_F0_SEARCH                  0xD680
#define STATE_LT_NRZ_COLLECT                0xD6C0
#define STATE_PAM4_WAIT_SD2                 0xD700
#define STATE_PAM4_EQ2_WAIT_LINK            0xD720
#define STATE_LT_WAIT_PAM_FRAME_LOCK        0xD740
#define STATE_LT_WAIT_PAM_FRAME             0xD780
#define STATE_LT_TX_PAM4_ADJUST             0xD800

 --- NRZ states ---
#define STATE_NRZ_WAIT_RESET                0x9001
#define STATE_NRZ_DC_SEARCH                 0x9002
#define STATE_NRZ_DC_SEARCH_RESET           0x9902
#define STATE_NRZ_WAIT_SD                   0x9003
#define STATE_NRZ_EQ1                       0x9004
#define STATE_NRZ_WAIT_24                   0x9005
#define STATE_NRZ_STABLIZE                  0x9015
#define STATE_NRZ_CH                        0x9006
#define STATE_NRZ_CNTR_TARGET               0x9007
#define STATE_NRZ_EQ2                       0x9008
#define STATE_NRZ_EQ2_LD                    0x9009
#define STATE_NRZ_EQ2_LINK_CHECK            0x9108
#define STATE_NRZ_GAIN_TRACKING_BASE        0x900B
#define STATE_NRZ_GAIN_TRACKING             0x9900

 --- NRZ Lane is up here ---
#define STATE_NRZ_STATISTICS                0x9A00
#define STATE_NRZ_FREEZE                    0x9EEE
#define STATE_NRZ_DONE                      0xEEEE
#define STATE_NRZ_EXIT                      0xEEEF
#define STATE_NRZ_RESET_FROM_TOP_MODE       0xEE00

 --- PAM4 states ---
#define STATE_PAM4_WAIT_RESET               0x8000
#define STATE_PAM4_WAIT_SD                  0x8100
#define STATE_PAM4_DC_GAIN_SEARCH1          0x8200
#define STATE_PAM4_CHANNEL_ANALYZER         0x8300
#define STATE_PAM4_DC_GAIN_SEARCH2          0x8400
#define STATE_PAM4_EYE_CHECK                0x8500
#define STATE_PAM4_CTLE_SEARCH              0x8600
#define STATE_PAM4_DELTA_SEARCH             0x8700
#define STATE_PAM4_EYE_CHECK2               0x8800
#define STATE_PAM4_CNT_TARGET               0x8840

 --- PAM4 Lane is up here ---
#define STATE_PAM4_LANE_UP                  0x8900

#define STATE_PAM4_FFE_ADAPT                0x8900
#define STATE_PAM4_SKEF_SEARCH              0x8980
#define STATE_PAM4_FFE_TRACK                0x8A00
#define STATE_PAM4_ISI_COLLECT              0x8B00

#define STATE_PAM4_FREEZE                   0x8EEE
#define STATE_PPAM4_DONE                    0xEEEE
#define STATE_PAM4_EXIT                     0xEEEF
#define STATE_PAM4_RESET_FINAL_CHECK        0xEE10
#define STATE_PAM4_RESET_BGND_EYE_CHECK     0xEE20
#define STATE_PAM4_RESET_FROM_TOP_MODE      0xEE00
*/

/*****************************************************************************
 * bf_pm_fsm_idle
 *
 * Start AN/LT
 */
static bf_status_t bf_pm_fsm_idle(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  int32_t n_lanes;
  bf_status_t rc;
  int ln;

  bf_port_num_lanes_get(dev_id, dev_port, &n_lanes);

  bf_port_link_fault_status_set(dev_id, dev_port, BF_PORT_LINK_FAULT_LOC_FAULT);

  if (1) {
    bool an_done;

    // make sure LT done flag has been cleared
    for (ln = 0; ln < (int)n_lanes; ln++) {
      bf_tof2_serdes_an_done_get(dev_id, dev_port, ln, &an_done);
      if (!an_done) continue;
      {
        uint32_t a0s[8] = {0};
        for (int ln_ = 0; ln_ < 8; ln_++) {
          a0s[ln_] = port_mgr_tof2_serdes_tile_rd(
              dev_id, dev_port, 0xA0 + (0x10000 * ln_));
        }
        PM_DEBUG("FSM :%d:%3d:%d: *** AN Done already set ***",
                 dev_id,
                 dev_port,
                 ln);
        PM_DEBUG(
            "FSM :%d:%3d:%d: *** %04x : %04x : %04x : %04x : %04x : %04x : "
            "%04x : %04x",
            dev_id,
            dev_port,
            ln,
            a0s[0],
            a0s[1],
            a0s[2],
            a0s[3],
            a0s[4],
            a0s[5],
            a0s[6],
            a0s[7]);
      }
    }
  }
  for (ln = 0; ln < n_lanes; ln++) {
    PM_DEBUG("FSM :%d:%3d:%d: AN : Init lane", dev_id, dev_port, ln);
    if (0) bf_tof2_serdes_init_ln(dev_id, dev_port, ln);
    bf_tof2_serdes_pre_config_ln(dev_id, dev_port, ln);
  }

  /* Configure port to perform AN. Basepage and next-pages, if applicable, were
   * already built and available in port_mgr structure.
   */
  rc = bf_port_autoneg_config_set(dev_id, dev_port);
  bf_port_an_lt_stats_init(dev_id, dev_port, false);
  return rc;
}

/*****************************************************************************
 * bf_pm_fsm_wait_an_done
 *
 * Wait for FW to indicate link-training is done by asserting adapt_done
 */
static bf_status_t bf_pm_fsm_wait_an_done(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port) {
  bool an_done;
  uint32_t rc, result;
  bf_serdes_encoding_mode_t enc_mode;
  bf_port_speed_t speed;
  int32_t n_lanes;
  bf_port_speed_t hcd_speed;
  bf_fec_type_t fec;
  int hcd_lanes;

  bf_port_num_lanes_get(dev_id, dev_port, &n_lanes);

  bf_port_speed_get(dev_id, dev_port, &speed);
  bf_serdes_encoding_mode_get(speed, n_lanes, &enc_mode);
  if (enc_mode == BF_SERDES_ENC_MODE_PAM4) {
    rc = bf_tof2_serdes_fw_debug_cmd(
        dev_id, dev_port, 0, 0 /*mode*/, 99, &result);
    if (rc != BF_SUCCESS) {
      PM_DEBUG("FSM :%d:%3d:%d: AN ST: Err", dev_id, dev_port, 0);
    } else {
      PM_DEBUG("FSM :%d:%3d:%d: AN ST: %04x", dev_id, dev_port, 0, result);
    }
  }

  // get LT status from all lanes
  bf_tof2_serdes_an_done_get(dev_id, dev_port, 0, &an_done);
  if (!an_done) return BF_NOT_READY;

  PM_DEBUG("FSM :%d:%3d:-: AN Done", dev_id, dev_port);

  bf_serdes_an_lp_pages_get(dev_id, dev_port, NULL, NULL, NULL);
  bf_port_autoneg_hcd_fec_get_v2(
      dev_id, dev_port, &hcd_speed, &hcd_lanes, &fec);

  bf_port_signal_detect_time_set(dev_id, dev_port);

  /* archive HW stats as they will be cleared during rx_reset */
  bf_port_mac_stats_historical_update_set(dev_id, dev_port);

  // bring-up UMAC
  bf_port_tx_reset_set(dev_id, dev_port);
  bf_port_rx_reset_set(dev_id, dev_port);
  bf_port_un_force_sig_ok_low_set(dev_id, dev_port);

  bf_port_an_lt_start_time_set(dev_id, dev_port);
  bf_port_an_try_inc(dev_id, dev_port);

  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_wait_an_lt_done
 *
 * Wait for FW to indicate link-training is done by asserting adapt_done
 */
static bf_status_t bf_pm_fsm_wait_an_lt_done(bf_dev_id_t dev_id,
                                             bf_dev_port_t dev_port) {
  bool adapt_done;
  uint32_t adapt_cnt;
  uint32_t readapt_cnt;
  uint32_t link_lost_cnt;
  bf_status_t rc;
  int ln;
  bf_serdes_encoding_mode_t enc_mode;
  bf_port_speed_t speed;
  uint32_t result;
  uint32_t n_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);

  bf_port_speed_get(dev_id, dev_port, &speed);
  bf_serdes_encoding_mode_get(speed, n_lanes, &enc_mode);

  if (tof2_an_dbg) {  // && enc_mode == BF_SERDES_ENC_MODE_PAM4) {
    uint32_t ltst[8] = {0};
    for (ln = 0; ln < (int)n_lanes; ln++) {
      rc = bf_tof2_serdes_fw_debug_cmd(
          dev_id, dev_port, ln, 0 /*mode*/, 99, &result);
      if (rc == BF_SUCCESS) {
        ltst[ln] = result;
      } else {
        ltst[ln] = 0xeeee;
      }
    }
    PM_DEBUG("FSM :%d:%3d:-: LT ST: %04x %04x %04x %04x %04x %04x %04x %04x",
             dev_id,
             dev_port,
             ltst[0],
             ltst[1],
             ltst[2],
             ltst[3],
             ltst[4],
             ltst[5],
             ltst[6],
             ltst[7]);
  }

  // get LT status from all lanes
  bool lt_done = true;
  for (ln = 0; ln < (int)n_lanes; ln++) {
    adapt_done = false;

    rc = bf_tof2_serdes_adapt_counts_get(dev_id,
                                         dev_port,
                                         ln,
                                         &adapt_done,
                                         &adapt_cnt,
                                         &readapt_cnt,
                                         &link_lost_cnt);
    if (rc != BF_SUCCESS) return BF_INVALID_ARG;

    if (!adapt_done) lt_done = false;
  }
  if (!lt_done) return BF_NOT_READY;

  bf_port_an_lt_dur_set(dev_id, dev_port);

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
  bf_status_t rc;
  int state = exp_st;
  bool pending;

  rc = bf_port_oper_state_get(dev_id, dev_port, &state);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;

  if (state == exp_st) return BF_NOT_READY;

  rc = bf_port_is_oper_state_callback_pending(dev_id, dev_port, &pending);
  if ((rc == BF_SUCCESS) && pending) {
    // log state change information
    bf_pm_log_state_chg_info(dev_id, dev_port, state == 0 ? false : true);
    return rc;
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
  bf_status_t rc;
  uint32_t n_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);
  bf_serdes_encoding_mode_t enc_mode;
  bf_port_speed_t speed;
  uint32_t result;

  bf_port_speed_get(dev_id, dev_port, &speed);
  bf_serdes_encoding_mode_get(speed, n_lanes, &enc_mode);

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  // if (enc_mode == BF_SERDES_ENC_MODE_PAM4) {
  if (tof2_an_dbg) {
    uint32_t ltst[8] = {0};
    int ln;
    for (ln = 0; ln < (int)n_lanes; ln++) {
      rc = bf_tof2_serdes_fw_debug_cmd(
          dev_id, dev_port, ln, 0 /*mode*/, 99, &result);
      if (rc == BF_SUCCESS) {
        ltst[ln] = result;
      } else {
        ltst[ln] = 0xeeee;
      }
    }
    PM_DEBUG(
        "   FSM :%d:%3d:%d:PCS DN: %04x %04x %04x %04x %04x %04x %04x %04x",
        dev_id,
        dev_port,
        ln,
        ltst[0],
        ltst[1],
        ltst[2],
        ltst[3],
        ltst[4],
        ltst[5],
        ltst[6],
        ltst[7]);
  }

  rc = bf_pm_fsm_check_link_state(dev_id, dev_port, 0 /*exp down*/);

  // hack to "fix" PCS lock up on CLK change issue
  if (rc == BF_NOT_READY) {
    bf_port_rx_reset_set(dev_id, dev_port);
  } else if (rc == BF_SUCCESS) {
    bf_port_link_fault_status_set(dev_id, dev_port, BF_PORT_LINK_FAULT_OK);
  }
  return rc;
}

/*****************************************************************************
 * bf_pm_fsm_wait_link_down
 *
 * Wait for MAC link-down indication
 */
static bf_status_t bf_pm_fsm_wait_link_down(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port) {
  bf_status_t rc, rsts;
  int state = 0;
  bf_port_speed_t speed;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  rc = bf_pm_fsm_check_link_state(dev_id, dev_port, 1 /*exp up*/);
  if (rc == BF_NOT_READY) {
    state = 1;
  }

  /* Handle mac/pcs stuck condition:
   * If link state is up, then validate serdes status as well. Sometimes the PCS
   * status shows link is up eventhough the Serdes signal detect is failing.
   * Validating the same with serdes status prevents any false link up
   * scenarios. Limiting this extra validation to only 10G & 25G speed since the
   * issue have been observed in this speed config.
   *
   * Note on Interrupt: If mac/pcs is in stuck condition, interrupt won't be
   * triggered and sw.per_state will remain 1. oper_state_get_extended api
   * will return st = sw.oper_state = 1. Hence below check is still valid for
   * interrupt case too.
   *
   * However pcs_rdy, local/remote are not valid in both the cases.
   */
  rsts = bf_port_speed_get(dev_id, dev_port, &speed);
  if ((rsts == BF_SUCCESS) && state &&
      ((speed == BF_SPEED_10G) || (speed == BF_SPEED_25G))) {
    bool srds_sts = false;
    rsts = bf_port_serdes_sig_sts_get(dev_id, dev_port, &srds_sts);
    if ((rsts == BF_SUCCESS) && !srds_sts) {
      state = false;
      bf_port_link_fault_status_set(
          dev_id, dev_port, BF_PORT_LINK_FAULT_LOC_FAULT);
      bf_port_oper_state_set_force_callbacks(dev_id, dev_port, true);
      bf_port_oper_state_set_pending_callbacks(dev_id, dev_port, state);
      return BF_INVALID_ARG;
    }
  }
  if (rc == BF_NOT_READY) return rc;

  bf_port_link_fault_status_set(dev_id, dev_port, BF_PORT_LINK_FAULT_LOC_FAULT);
  bf_port_an_lt_stats_init(dev_id, dev_port, true);
  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_abort
 *
 * Restart FSM bring-up from wait_signal state
 */
static bf_status_t bf_pm_fsm_abort(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  int ln, num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);
  bf_status_t rc;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  // disconnect UMAC from serdes Rx
  bf_port_force_sig_ok_low_set(dev_id, dev_port);

  {
    uint32_t i, result[16];
    for (ln = 0; ln < num_lanes; ln++) {
      for (i = 100; i < 116; i++) {
        rc = bf_tof2_serdes_fw_debug_cmd(
            dev_id, dev_port, ln, 0 /*mode*/, i, &result[i - 100]);
      }
      PM_DEBUG(
          "FSM :%d:%3d:%d: Exit code: %04x %04x %04x %04x %04x %04x %04x %04x "
          "%04x %04x %04x %04x %04x %04x %04x %04x",
          dev_id,
          dev_port,
          ln,
          result[0],
          result[1],
          result[2],
          result[3],
          result[4],
          result[5],
          result[6],
          result[7],
          result[8],
          result[9],
          result[10],
          result[11],
          result[12],
          result[13],
          result[14],
          result[15]);
    }
  }
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
 * bf_pm_fsm_disable_port
 *
 * Restart FSM bring-up from wait_signal state
 */
static bf_status_t bf_pm_fsm_disable_port(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port) {
  int ln, num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);
  bf_status_t rc;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  // pm_port_serdes_rx_ready_set(dev_id, dev_port, false);
  // pm_port_serdes_tx_ready_set(dev_id, dev_port, false);

  // disconnect UMAC from serdes Rx
  bf_port_force_sig_ok_low_set(dev_id, dev_port);

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

#if 0
// return AN status, 0xA01F= HCD_RESOLVED
static bf_status_t bf_pm_fsm_an_status_get(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           uint32_t *an_status) {
  uint32_t ln = 0, section = 3, index = 0;

  bf_tof2_serdes_fw_debug_cmd(dev_id, dev_port, ln, section, index, an_status);
  return BF_SUCCESS;
}

// return AN HCD
static bf_status_t bf_pm_fsm_an_hcd_get(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        uint32_t *hcd) {
  uint32_t ln = 0, section = 3, index = 10;

  bf_tof2_serdes_fw_debug_cmd(dev_id, dev_port, ln, section, index, hcd);
  return BF_SUCCESS;
}
#endif

bf_pm_fsm_state_desc_t bf_pm_fsm_autoneg[] = {
    {.state = BF_PM_FSM_ST_IDLE,
     .handler = bf_pm_fsm_idle,
     .wait_ms = 0,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_WAIT_AN_DONE,
     .next_state_wait_ms = 10,

     .alt_next_state = BF_PM_FSM_ST_END,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_END,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_END,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_AN_DONE,
     .handler = bf_pm_fsm_wait_an_done,
     .wait_ms = 10,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_WAIT_AN_LT_DONE,
     .next_state_wait_ms = 20,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_AN_LT_DONE,
     .handler = bf_pm_fsm_wait_an_lt_done,
     .wait_ms = 100,
     .tmout_cycles = 50,

     .next_state = BF_PM_FSM_ST_LINK_DN,
     .next_state_wait_ms = 10,

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
     .tmout_cycles = 40,

     .next_state = BF_PM_FSM_ST_LINK_UP,
     .next_state_wait_ms = 100,

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

     .next_state = BF_PM_FSM_ST_IDLE,
     //.next_state_wait_ms = 1000000,  // for manual AN restarts
     .next_state_wait_ms = 1000,  // give de-config time to complete

     .alt_next_state = BF_PM_FSM_ST_END,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_DISABLED,
     .handler = bf_pm_fsm_disable_port,
     .wait_ms = 0,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_IDLE,
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
 * bf_pm_get_fsm_for_autoneg
 ************************************************************************/
bf_pm_fsm_t bf_pm_get_fsm_for_autoneg(void) { return bf_pm_fsm_autoneg; }
