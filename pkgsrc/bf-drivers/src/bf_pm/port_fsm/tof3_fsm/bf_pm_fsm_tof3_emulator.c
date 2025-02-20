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
#include <bf_pm/bf_pm_fsm_common.h>
#include "../bf_pm_fsm_if.h"
#include "../../bf_pm.h"
#include "../../pm_log.h"

void bf_pm_log_state_chg_info(uint32_t dev_id, uint32_t dev_port, bool st);

/*****************************************************************************
 * bf_pm_fsm_init
 *
 * Emulator can be used in several MAC loopback modes. Check for current
 * loopback setting and configure it if appropriate for the emulator.
 *
 * Then just move to wait for MAC link-up indication state
 */
static bf_status_t bf_pm_fsm_init(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  bf_status_t rc;
  bf_loopback_mode_e mode;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  rc = bf_pm_port_loopback_mode_from_dev_port_get(dev_id, dev_port, &mode);
  if ((rc == BF_SUCCESS) &&
      ((mode == BF_LPBK_MAC_NEAR) || (mode == BF_LPBK_PCS_NEAR) ||
       (mode == BF_LPBK_PIPE))) {
    bf_port_loopback_mode_set(dev_id, dev_port, mode);
  }
  bf_port_tof3_tmac_reset_set(dev_id, dev_port, 0);
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

    rc = bf_port_oper_state_callbacks_issue(dev_id, dev_port);
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

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  rc = bf_pm_fsm_check_link_state(dev_id, dev_port, 0 /*exp down*/);
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
  if (rc == BF_NOT_READY) return rc;

  return BF_SUCCESS;
}

/*****************************************************************************
 * bf_pm_fsm_abort
 *
 * Restart FSM bring-up from wait_signal state
 */
static bf_status_t bf_pm_fsm_abort(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  bf_status_t rc = BF_SUCCESS;
  bf_loopback_mode_e mode = BF_LPBK_NONE;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  rc = bf_pm_port_loopback_mode_from_dev_port_get(dev_id, dev_port, &mode);
  if (rc != BF_SUCCESS) {
    PM_ERROR("Error: cannot get loopback mode for dev_port=%d\n", dev_port);
  } else if (mode != BF_LPBK_NONE) {
    bf_port_loopback_mode_set(dev_id, dev_port, BF_LPBK_NONE);
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
  bf_status_t rc = BF_SUCCESS;
  bf_loopback_mode_e mode = BF_LPBK_NONE;

  if (!bf_pm_fsm_port_is_valid(dev_id, dev_port)) return BF_INVALID_ARG;

  rc = bf_pm_port_loopback_mode_from_dev_port_get(dev_id, dev_port, &mode);
  if (rc != BF_SUCCESS) {
    PM_ERROR("Error: cannot get loopback mode for dev_port=%d\n", dev_port);
  } else if (mode != BF_LPBK_NONE) {
    bf_port_loopback_mode_set(dev_id, dev_port, BF_LPBK_NONE);
  }

  return BF_SUCCESS;
}

bf_pm_fsm_state_desc_t bf_pm_fsm_tof3_emulator[] = {
    {.state = BF_PM_FSM_ST_IDLE,
     .handler = bf_pm_fsm_init,
     .wait_ms = 0,
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
     .wait_ms = 10 * 1000,  // really slow for emulator
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
     .wait_ms = 10 * 1000,
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

    {.state = BF_PM_FSM_ST_ABORT,
     .handler = bf_pm_fsm_abort,
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
 * bf_pm_get_fsm_for_tof3_emulator
 ************************************************************************/
bf_pm_fsm_t bf_pm_get_fsm_for_tof3_emulator(void) {
  return bf_pm_fsm_tof3_emulator;
}
