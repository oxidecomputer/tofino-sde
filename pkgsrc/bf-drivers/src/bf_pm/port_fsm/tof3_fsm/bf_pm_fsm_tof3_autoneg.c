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
#include <port_mgr/port_mgr_tof3/aw_lane_cfg.h>
#include <port_mgr/bf_tof3_serdes_if.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include <bf_pm/bf_pm_intf.h>
#include "../bf_pm_fsm_if.h"
#include "../../bf_pm.h"
#include "../../pm_log.h"

bf_port_speeds_t pm_port_speed_to_an_speed_map(bf_dev_id_t dev_id,
                                               bf_port_speed_t p_speed,
                                               uint32_t n_lanes,
                                               bool adv_kr_mode,
                                               bool rsfec_support);

bf_fec_types_t pm_port_fec_to_an_fec_map(bf_fec_type_t p_fec,
                                         bf_port_speed_t p_speed);

void bf_pm_log_state_chg_info(uint32_t dev_id, uint32_t dev_port, bool st);

extern bf_status_t port_mgr_tof3_map_dev_port_to_all(bf_dev_id_t dev_id,
                                                     bf_dev_port_t dev_port,
                                                     uint32_t *pipe_id,
                                                     uint32_t *port_id,
                                                     uint32_t *umac,
                                                     uint32_t *ch,
                                                     bool *is_cpu_port);
bf_status_t bf_tof3_serdes_config_ln_autoneg_all(bf_dev_id_t dev_id,
                                                 bf_dev_port_t dev_port,
                                                 uint32_t ln,
                                                 uint64_t basepage,
                                                 uint32_t consortium_np_47_16,
                                                 bool is_loop);
extern char *bf_tof3_serdes_tech_ability_to_str(uint32_t tech_ability);

extern bf_status_t bf_aw_pmd_anlt_auto_neg_page_rx_get(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    uint32_t ln,
    uint32_t *an_mr_page_rx,
    uint64_t *an_rx_link_code_word);
extern bf_status_t bf_aw_pmd_anlt_auto_neg_config_get(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    uint32_t ln,
    uint32_t *status_check_disable,
    uint32_t *next_page_en,
    uint32_t *an_no_nonce_check);
extern bf_status_t bf_aw_pmd_anlt_auto_neg_next_page_set(bf_dev_id_t dev_id,
                                                         bf_dev_port_t dev_port,
                                                         uint32_t ln,
                                                         uint64_t an_tx_np);

/*****************************************************************************
 * Pacing control
 */
int32_t max_active_lanes = 264;
int32_t active_d_p[1][1000] = {{0}};

static void bf_pm_pacing_ctrl_check(bf_dev_id_t dev_id,
                                    bf_dev_port_t dev_port,
                                    bool *queue_full) {
  int32_t active_lanes = 0;
  (void)dev_port;

  for (int d_p = 0; d_p < 1000; d_p++) {
    if (active_d_p[dev_id][d_p]) {
      int32_t n_lanes;
      bf_port_num_lanes_get(dev_id, d_p, &n_lanes);
      active_lanes += n_lanes;
    }
  }
  if (active_lanes > max_active_lanes) {
    *queue_full = true;
  } else {
    *queue_full = false;
  }
}

static void bf_pm_pacing_ctrl_enter(bf_dev_id_t dev_id,
                                    bf_dev_port_t dev_port) {
  active_d_p[dev_id][dev_port] = 1;
}

static void bf_pm_pacing_ctrl_exit(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  active_d_p[dev_id][dev_port] = 0;
}

/*****************************************************************************
 * bf_pm_fsm_idle
 *
 * Check pacer to see if we can start ANLT
 */
static bf_status_t bf_pm_fsm_idle(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  (void)dev_id;
  (void)dev_port;
  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_wait_pacing_ctrl
 *
 * Start AN/LT
 */
static bf_status_t bf_pm_fsm_wait_pacing_ctrl(bf_dev_id_t dev_id,
                                              bf_dev_port_t dev_port) {
  bf_status_t rc;
  int32_t n_lanes;
  uint64_t basepage;
  uint64_t cons_np2;
  bool is_loop = true;
  bool queue_full;

  bf_pm_pacing_ctrl_check(dev_id, dev_port, &queue_full);
  if (queue_full) {
    return BF_NOT_READY;
  }
  bf_pm_pacing_ctrl_enter(dev_id, dev_port);

  // confgure the mux first
  bf_port_num_lanes_get(dev_id, dev_port, &n_lanes);
  rc = bf_tof3_serdes_anlt_mux_set(dev_id, dev_port, 0, n_lanes);
  if (rc != BF_SUCCESS) {
    return rc;
  }
  // clear PCS UP indication (sticky)
  rc = bf_tof3_serdes_anlt_link_up_set(dev_id, dev_port, 0);

  bf_port_autoneg_config_set(dev_id, dev_port);

  bf_serdes_an_loc_pages_get(dev_id, dev_port, BF_AN_PAGE_BASE, &basepage);
  bf_serdes_an_loc_pages_get(dev_id, dev_port, BF_AN_PAGE_NEXT_2, &cons_np2);

  rc = bf_tof3_serdes_config_ln_autoneg(
      dev_id, dev_port, 0, basepage, (uint32_t)(cons_np2 >> 16ull), is_loop);

  // clear any saved AN page(s) and init AN stats
  bf_serdes_an_lp_pages_save(dev_id, dev_port, BF_AN_PAGE_BASE, 0ull);
  bf_port_an_lt_stats_init(dev_id, dev_port, false);

  return rc;
}

/*****************************************************************************
 * bf_pm_fsm_an_done
 *
 * AN has completed
 */
static bf_status_t bf_pm_fsm_an_done(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port) {
  uint32_t an_result, an_mr_page_rx;
  uint64_t an_rx_link_code_word;
  bf_port_speed_t hcd_speed;
  bf_fec_type_t fec;
  int hcd_lanes;
  bf_status_t rc;

  PM_DEBUG("FSM :%d:%3d:-: AN Done", dev_id, dev_port);

  // get and log HCD and LP base pg
  rc = bf_tof3_serdes_an_result_get(
      dev_id, dev_port, &an_mr_page_rx, &an_result, &an_rx_link_code_word);
  if (rc != 0) {
    PM_DEBUG(
        "FSM :%d:%3d:-: AN result invalid : %08x", dev_id, dev_port, an_result);
    return BF_INVALID_ARG;
  }
  PM_DEBUG("FSM :%d:%3d:-: AN Tech ability : %d : %s",
           dev_id,
           dev_port,
           an_result,
           bf_tof3_serdes_tech_ability_to_str(an_result));

  // process AN information for upper layers
  bf_serdes_an_lp_pages_get(dev_id, dev_port, NULL, NULL, NULL);

  bf_port_autoneg_hcd_fec_get_v2(
      dev_id, dev_port, &hcd_speed, &hcd_lanes, &fec);

  bf_port_signal_detect_time_set(dev_id, dev_port);

  /* archive HW stats as they will be cleared during rx_reset */
  bf_port_mac_stats_historical_update_set(dev_id, dev_port);

  // release MAC reset
  bf_port_tof3_tmac_reset_set(dev_id, dev_port, 0);
  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_wait_base_pg_xchg_done
 *
 * Wait for FW to indicate link-training is done by asserting adapt_done
 */
static bf_status_t bf_pm_fsm_wait_base_pg_xchg_done(bf_dev_id_t dev_id,
                                                    bf_dev_port_t dev_port) {
  bool an_done = false;
  bf_port_speed_t speed;
  int32_t n_lanes;
  uint32_t an_mr_page_rx;
  uint64_t an_rx_link_code_word;
  uint32_t status_check_disable, next_page_en, an_no_nonce_check;
  uint64_t an_tx_np = 0x00000001ul;  // null page;

  bf_port_num_lanes_get(dev_id, dev_port, &n_lanes);
  bf_port_speed_get(dev_id, dev_port, &speed);

  // check for page received
  bf_aw_pmd_anlt_auto_neg_page_rx_get(
      dev_id, dev_port, 0, &an_mr_page_rx, &an_rx_link_code_word);
  if (!an_mr_page_rx) return BF_NOT_READY;

  // save partner base page
  bf_serdes_an_lp_pages_save(
      dev_id, dev_port, BF_AN_PAGE_BASE, an_rx_link_code_word);

  // see if we requested next-pages
  bf_aw_pmd_anlt_auto_neg_config_get(dev_id,
                                     dev_port,
                                     0,
                                     &status_check_disable,
                                     &next_page_en,
                                     &an_no_nonce_check);

  // check for required NP exchange
  if (!next_page_en && ((an_rx_link_code_word & 0x00008000ul) == 0)) {
    // get AN_done status
    bf_tof3_serdes_an_done_get(dev_id, dev_port, 0, &an_done);
    if (!an_done) return BF_NOT_READY;

    bf_pm_fsm_an_done(dev_id, dev_port);
    return BF_SUCCESS;

  } else {
    // NP exchange required
    // See if we have NP to send, otherwise send a NULL page
    if (next_page_en) {
      // After the exchange of the base page, the link partners exchange an OUI
      // tagged formatted Next Page (using message code #5).
      bf_serdes_an_loc_pages_get(
          dev_id, dev_port, BF_AN_PAGE_NEXT_1, &an_tx_np);
    }

    PM_DEBUG("FSM :%d:%3d:-: AN NP (1): %04x_%04x_%04x",
             dev_id,
             dev_port,
             (uint32_t)(an_tx_np >> 32ull) & 0xffff,
             (uint32_t)(an_tx_np >> 16ull) & 0xffff,
             (uint32_t)(an_tx_np >> 0ull) & 0xffff);

    bf_aw_pmd_anlt_auto_neg_next_page_set(dev_id, dev_port, 0, an_tx_np);
    // wait for an_mr_page_rx to be de-asserted
    // ...with a timeout
    uint32_t tout = 100;
    do {
      bf_aw_pmd_anlt_auto_neg_page_rx_get(
          dev_id, dev_port, 0, &an_mr_page_rx, &an_rx_link_code_word);
    } while (--tout && an_mr_page_rx);
    if (an_mr_page_rx) {
      PM_DEBUG("FSM :%d:%3d:-: NP (warning: an_mr_page_rx never de-asserted)",
               dev_id,
               dev_port);
    }
    return BF_ALT1_NEXT_ST;
  }
  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_wait_np1_done
 *
 * Wait for 1st Next-page exchange to be acked
 */
static bf_status_t bf_pm_fsm_wait_np1_done(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port) {
  bf_port_speed_t speed;
  int32_t n_lanes;
  uint32_t an_mr_page_rx;
  uint64_t an_rx_link_code_word;
  uint64_t cons_np;

  bf_port_num_lanes_get(dev_id, dev_port, &n_lanes);
  bf_port_speed_get(dev_id, dev_port, &speed);

  // check for page received
  bf_aw_pmd_anlt_auto_neg_page_rx_get(
      dev_id, dev_port, 0, &an_mr_page_rx, &an_rx_link_code_word);
  if (!an_mr_page_rx) return BF_NOT_READY;

  // save partner next page 1
  bf_serdes_an_lp_pages_save(
      dev_id, dev_port, BF_AN_PAGE_NEXT_1, an_rx_link_code_word);

  // set next page 2 to be transmitted
  bf_serdes_an_loc_pages_get(dev_id, dev_port, BF_AN_PAGE_NEXT_2, &cons_np);

  // add in ACK
  cons_np |= (1ul << 14ul);

  PM_DEBUG("FSM :%d:%3d:-: TX AN NP (2): %04x_%04x_%04x Consortium advert",
           dev_id,
           dev_port,
           (uint32_t)(cons_np >> 32ull) & 0xffff,
           (uint32_t)(cons_np >> 16ull) & 0xffff,
           (uint32_t)(cons_np >> 0ull) & 0xffff);

  bf_aw_pmd_anlt_auto_neg_next_page_set(dev_id, dev_port, 0, cons_np);
  // wait for an_mr_page_rx to be de-asserted
  // ...with a timeout
  uint32_t tout = 100;
  do {
    bf_aw_pmd_anlt_auto_neg_page_rx_get(
        dev_id, dev_port, 0, &an_mr_page_rx, &an_rx_link_code_word);
  } while (--tout && an_mr_page_rx);
  if (an_mr_page_rx) {
    PM_DEBUG("FSM :%d:%3d:-: NP (warning: an_mr_page_rx never de-asserted)",
             dev_id,
             dev_port);
  }
  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_wait_np2_done
 *
 * Wait for 1st Next-page exchange to be acked
 */
static bf_status_t bf_pm_fsm_wait_np2_done(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port) {
  bf_port_speed_t speed;
  int32_t n_lanes;
  uint32_t an_mr_page_rx;
  uint64_t an_rx_link_code_word;
  uint64_t an_tx_np;

  bf_port_num_lanes_get(dev_id, dev_port, &n_lanes);
  bf_port_speed_get(dev_id, dev_port, &speed);

  // check for page received
  bf_aw_pmd_anlt_auto_neg_page_rx_get(
      dev_id, dev_port, 0, &an_mr_page_rx, &an_rx_link_code_word);
  if (!an_mr_page_rx) return BF_NOT_READY;

  // save partner next page 2
  bf_serdes_an_lp_pages_save(
      dev_id, dev_port, BF_AN_PAGE_NEXT_2, an_rx_link_code_word);

  if ((an_rx_link_code_word & 0x00008000ul)) {  // LP has more NP to send us
    an_tx_np = 0x00000001ul;                    // null page
    PM_DEBUG("FSM :%d:%3d:-: AN NP (3): %016" PRIx64 " NULL page",
             dev_id,
             dev_port,
             an_tx_np);
    bf_aw_pmd_anlt_auto_neg_next_page_set(dev_id, dev_port, 0, an_tx_np);
    // wait for an_mr_page_rx to be de-asserted
    // ...with a timeout
    uint32_t tout = 100;
    do {
      bf_aw_pmd_anlt_auto_neg_page_rx_get(
          dev_id, dev_port, 0, &an_mr_page_rx, &an_rx_link_code_word);
    } while (--tout && an_mr_page_rx);
    if (an_mr_page_rx) {
      PM_DEBUG("FSM :%d:%3d:-: NP (warning: an_mr_page_rx never de-asserted)",
               dev_id,
               dev_port);
    }
    return BF_NOT_READY;
  } else {
    bool an_done;
    // get AN_done status
    bf_tof3_serdes_an_done_get(dev_id, dev_port, 0, &an_done);
    if (!an_done) return BF_NOT_READY;

    bf_pm_fsm_an_done(dev_id, dev_port);
    return BF_SUCCESS;
  }
  return BF_NOT_READY;
}

/*****************************************************************************
 * bf_pm_fsm_select_lt_clause
 *
 * Wait for 1st Next-page exchange to be acked
 */
static bf_status_t bf_pm_fsm_select_lt_clause(bf_dev_id_t dev_id,
                                              bf_dev_port_t dev_port) {
  bf_port_speed_t speed;
  int32_t n_lanes;
  uint32_t serdes_gb;
  bool is_pam4;

  bf_port_num_lanes_get(dev_id, dev_port, &n_lanes);
  bf_port_speed_get(dev_id, dev_port, &speed);
  bf_tof3_serdes_port_speed_to_serdes_speed(
      dev_id, dev_port, 0, speed, n_lanes, &serdes_gb, &is_pam4);
  bf_port_an_lt_start_time_set(dev_id, dev_port);
  bf_port_an_try_inc(dev_id, dev_port);

  if (serdes_gb == 10) {
    return BF_SUCCESS;
  } else if (serdes_gb == 25) {
    return BF_ALT1_NEXT_ST;
  } else if (serdes_gb == 50) {
    return BF_ALT2_NEXT_ST;
  } else if (serdes_gb == 100) {
    return BF_ALT3_NEXT_ST;
  } else {
    return BF_INVALID_ARG;
  }
  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_wait_an_lt_done
 *
 * Wait for FW to indicate link-training is done by asserting lt_done
 */
static bf_status_t bf_pm_fsm_wait_an_lt_done(bf_dev_id_t dev_id,
                                             bf_dev_port_t dev_port) {
  bf_status_t rc = 0;
  uint32_t ln;
  bf_port_speed_t speed;
  uint32_t n_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);
  bool lt_done = false;

  bf_port_speed_get(dev_id, dev_port, &speed);

  // get LT state
  for (ln = 0; ln < n_lanes; ln++) {
    rc = bf_tof3_serdes_lt_done_get(dev_id, dev_port, ln, &lt_done);
    if (!lt_done) {
      return BF_NOT_READY;
    }
  }

  bf_pm_pacing_ctrl_exit(dev_id, dev_port);

  // check for signal-detect and cdr-lock on all lanes
  for (ln = 0; ln < n_lanes; ln++) {
    uint32_t pmd_rx_lock = 0, signal_detect = 0;

    rc = bf_tof3_serdes_anlt_lane_status_get(
        dev_id, dev_port, ln, &signal_detect, &pmd_rx_lock);
    if (rc != BF_SUCCESS) return BF_INVALID_ARG;
    /*
     * If we dont have signal detect then it may mean we lost
     * frame-lock and we should restart AN.
     * If we have signal detect but not CDR lock, then we may
     * just need to wait a little longer
     */
    if (!signal_detect) return BF_INVALID_ARG;  // test
    // if (!signal_detect) return BF_NOT_READY;
    if (!pmd_rx_lock) return BF_NOT_READY;
  }

  bf_port_an_lt_dur_set(dev_id, dev_port);
  return rc;
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

  bf_port_speed_get(dev_id, dev_port, &speed);
  bf_serdes_encoding_mode_get(speed, n_lanes, &enc_mode);

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  rc = bf_pm_fsm_check_link_state(dev_id, dev_port, 0 /*exp down*/);

  if (rc == BF_SUCCESS) {
    // indicate PCS UP to AN block, signifying completion of ANLT
    rc = bf_tof3_serdes_anlt_link_up_set(dev_id, dev_port, 1);
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
  bf_status_t rc;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  rc = bf_pm_fsm_check_link_state(dev_id, dev_port, 1 /*exp up*/);
  if (rc == BF_NOT_READY) {
    return rc;
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
  bf_port_speed_t hcd_speed;
  bf_fec_type_t fec;
  int hcd_lanes;
  bf_status_t rc = 0;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  // process any available AN information for upper layers, even if AN failed
  bf_serdes_an_lp_pages_get(dev_id, dev_port, NULL, NULL, NULL);

  bf_port_autoneg_hcd_fec_get_v2(
      dev_id, dev_port, &hcd_speed, &hcd_lanes, &fec);

  bf_pm_pacing_ctrl_exit(dev_id, dev_port);

  // un-configure all lanes. This will also squelch Tx output
  for (ln = 0; ln < num_lanes; ln++) {
    // power-dn all lanes
    rc = bf_tof3_serdes_cleanup(dev_id, dev_port, ln);
    if (rc != BF_SUCCESS) {
      PM_ERROR("Error: %d : from bf_tof3_serdes_config_ln\n", rc);
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
  bf_status_t rc = 0;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  bf_pm_pacing_ctrl_exit(dev_id, dev_port);

  // release MAC reset
  bf_port_tof3_tmac_reset_set(dev_id, dev_port, 1);

  bf_pm_fsm_check_link_state(dev_id, dev_port, 1 /*exp up*/);

  pm_port_serdes_rx_ready_set(dev_id, dev_port, false);
  pm_port_serdes_tx_ready_set(dev_id, dev_port, false);

  // reset the ANLT muxes
  bf_tof3_serdes_anlt_mux_reset(dev_id, dev_port, 0, num_lanes);

  // power-down all lanes
  for (ln = 0; ln < num_lanes; ln++) {
    rc = bf_tof3_serdes_cleanup(dev_id, dev_port, ln);
    // rc = bf_tof3_serdes_config_ln(dev_id,
    //                              dev_port,
    //                              ln,
    //                              BF_SPEED_NONE,
    //                              num_lanes,
    //                              BF_PORT_PRBS_MODE_NONE,
    //                              BF_PORT_PRBS_MODE_NONE);
    if (rc != BF_SUCCESS) {
      PM_ERROR("Error: %d : from bf_tof3_serdes_config_ln\n", rc);
      return BF_INVALID_ARG;
    }
  }
  return BF_SUCCESS;
}

bf_pm_fsm_state_desc_t bf_pm_fsm_tof3_autoneg[] = {
    {.state = BF_PM_FSM_ST_IDLE,
     .handler = bf_pm_fsm_idle,
     .wait_ms = 1000,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_WAIT_PACING_CTRL,
     .next_state_wait_ms = 10,

     .alt_next_state = BF_PM_FSM_ST_END,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_END,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_END,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_PACING_CTRL,
     .handler = bf_pm_fsm_wait_pacing_ctrl,
     .wait_ms = 1000,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_WAIT_AN_BASE_PG_DONE,
     .next_state_wait_ms = 10,

     .alt_next_state = BF_PM_FSM_ST_END,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_END,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_END,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_AN_BASE_PG_DONE,
     .handler = bf_pm_fsm_wait_base_pg_xchg_done,
     .wait_ms = 10,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_SELECT_LT_CLAUSE,
     .next_state_wait_ms = 20,

     .alt_next_state = BF_PM_FSM_ST_AN_NP_1,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_AN_NP_1,
     .handler = bf_pm_fsm_wait_np1_done,
     .wait_ms = 10,
     .tmout_cycles = 30,

     .next_state = BF_PM_FSM_ST_AN_NP_2,
     .next_state_wait_ms = 20,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_AN_NP_2,
     .handler = bf_pm_fsm_wait_np2_done,
     .wait_ms = 10,
     .tmout_cycles = 30,

     .next_state = BF_PM_FSM_ST_SELECT_LT_CLAUSE,
     .next_state_wait_ms = 20,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_SELECT_LT_CLAUSE,
     .handler = bf_pm_fsm_select_lt_clause,
     .wait_ms = 10,
     .tmout_cycles = BF_FSM_NO_TIMEOUT,

     .next_state = BF_PM_FSM_ST_WAIT_AN_LT_DONE_CL72,
     .next_state_wait_ms = 50,

     .alt_next_state = BF_PM_FSM_ST_WAIT_AN_LT_DONE_CL92,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_WAIT_AN_LT_DONE_CL136,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_WAIT_AN_LT_DONE_CL162,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_AN_LT_DONE_CL72,
     .handler = bf_pm_fsm_wait_an_lt_done,
     .wait_ms = 10,
     .tmout_cycles = 52,  // 510ms,

     .next_state = BF_PM_FSM_ST_LINK_DN,
     .next_state_wait_ms = 10,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_AN_LT_DONE_CL92,
     .handler = bf_pm_fsm_wait_an_lt_done,
     .wait_ms = 10,
     .tmout_cycles = 52,  // 510ms,

     .next_state = BF_PM_FSM_ST_LINK_DN,
     .next_state_wait_ms = 10,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_AN_LT_DONE_CL136,
     .handler = bf_pm_fsm_wait_an_lt_done,
     .wait_ms = 10,
     .tmout_cycles = 2 * 350,  // 3.5 sec,

     .next_state = BF_PM_FSM_ST_LINK_DN,
     .next_state_wait_ms = 10,

     .alt_next_state = BF_PM_FSM_ST_ABORT,
     .alt_next_state_wait_ms = 0,
     .alt_next_state_2 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_2_wait_ms = 0,
     .alt_next_state_3 = BF_PM_FSM_ST_ABORT,
     .alt_next_state_3_wait_ms = 0,
     .transition_fn = NULL},

    {.state = BF_PM_FSM_ST_WAIT_AN_LT_DONE_CL162,
     .handler = bf_pm_fsm_wait_an_lt_done,
     .wait_ms = 10,
     .tmout_cycles = 1500,  // 15 sec,

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
     .wait_ms = 10,
     .tmout_cycles = 500,  // BF_FSM_NO_TIMEOUT,  // 400,

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
     .wait_ms = 1000,  // 10000000, //100,
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
 * bf_pm_get_fsm_for_tof3_autoneg
 ************************************************************************/
bf_pm_fsm_t bf_pm_get_fsm_for_tof3_autoneg(void) {
  return bf_pm_fsm_tof3_autoneg;
}
