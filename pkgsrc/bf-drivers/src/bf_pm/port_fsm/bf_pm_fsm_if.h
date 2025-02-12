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

#ifndef BF_PM_FSM_H_INCLUDED
#define BF_PM_FSM_H_INCLUDED

/* Allow the use in C++ code.  */
#ifdef __cplusplus
extern "C" {
#endif

// FSM extended return codes
#define BF_ALT1_NEXT_ST BF_INVALID_ARG
#define BF_ALT2_NEXT_ST 0x20000
#define BF_ALT3_NEXT_ST 0x30000

/* FSM: extended link status: 0=normal mode; 1=verbose mode
 * Note that verbose mode is intended for debugging only and it may
 * significatively increase the volume of the log.
 */
#define BF_PM_FSM_LINK_STATUS_VERBOSE_MODE 0

#if 0
/*
 * This definition is now in bf_pm_fsm_common.h.  The old cold has been left
 * here to simplify future merges with upstream code
 */
typedef enum {

  // DFE fsm states
  BF_PM_FSM_ST_IDLE = 0,
  BF_PM_FSM_ST_WAIT_PLL_READY,
  BF_PM_FSM_ST_WAIT_SIGNAL_OK,
  BF_PM_FSM_ST_WAIT_DFE_DONE,
  BF_PM_FSM_ST_REMOTE_FAULT,
  BF_PM_FSM_ST_LINK_DN,
  BF_PM_FSM_ST_LINK_UP,
  BF_PM_FSM_ST_WAIT_TEST_DONE,
  // special check for poor BER
  BF_PM_FSM_ST_BER_CHECK_START,
  BF_PM_FSM_ST_BER_CHECK_DONE,
  BF_PM_FSM_ST_END,

  // AN fsm states
  BF_PM_FSM_ST_WAIT_AN_DONE,
  BF_PM_FSM_ST_WAIT_AN_LT_DONE,

  // PRBS-specific FSM states
  BF_PM_FSM_ST_MONITOR_PRBS_ERRORS,

  // tof3 DFE FSM states
  BF_PM_FSM_ST_WAIT_RX_READY,
  BF_PM_FSM_ST_WAIT_TX_RATE_CHG_DONE,
  BF_PM_FSM_ST_WAIT_RX_RATE_CHG_DONE,
  BF_PM_FSM_ST_WAIT_CDR_LOCK,
  BF_PM_FSM_ST_WAIT_BIST_LOCK,

  // tof3 ANLT extra FSM state
  BF_PM_FSM_ST_WAIT_PACING_CTRL,  // check pacer
  BF_PM_FSM_ST_AN_NP_1,           // 1st consortium NP
  BF_PM_FSM_ST_AN_NP_2,           // 2nd consortium NP
  BF_PM_FSM_ST_AN_NP_3,           // ack all NP with NULL pg
  BF_PM_FSM_ST_WAIT_AN_BASE_PG_DONE,
  BF_PM_FSM_ST_SELECT_LT_CLAUSE,
  BF_PM_FSM_ST_WAIT_AN_LT_DONE_CL72,   // 500ms
  BF_PM_FSM_ST_WAIT_AN_LT_DONE_CL92,   // ?
  BF_PM_FSM_ST_WAIT_AN_LT_DONE_CL136,  // 3.5 sec
  BF_PM_FSM_ST_WAIT_AN_LT_DONE_CL162,  // 15 sec

  // Special "clean-up" states
  BF_PM_FSM_ST_ABORT,
  BF_PM_FSM_ST_DISABLED,
  BF_PM_FSM_ST_CFG_TX_MODE,

} bf_pm_fsm_st;

#endif

/* Signature for a state handler function, executed
 *  by the FSM on each call */
typedef bf_status_t (*bf_pm_fsm_fn)(bf_dev_id_t dev_id, bf_dev_port_t dev_port);

/* Signature for a state-transition function, executed
 *  (optionally) on transition from the current state. This
 *  may be specified as NULL if no specific function is
 *  required to execute (once) when entering a new state.
 *  If specified, this function is passed pointers to the
 *  (expected) next state and wait times. These may be modified
 *  by the function (for example, to provide a multi-way branch
 *  from one state to one of several others).
 */
typedef bf_status_t (*bf_pm_fsm_transition_fn)(bf_dev_id_t dev_id,
                                               bf_dev_port_t dev_port,
                                               bf_pm_fsm_st *next_state,
                                               uint32_t *next_state_wait_ms);

// Abort indication if returned as "nxt_wait_ms"
#define BF_FSM_TERMINATE 0xffffffff

// No FSM state timeout
#define BF_FSM_NO_TIMEOUT 0xffffffff

/* Generic Finite State Machine (FSM) state descriptor.
 *  Each state in a state-machine is described by one
 *  bf_pm_fsm_state_desc_t structure.
 *  The FSM defines a set of states, next states, and the
 *  amount of time that should elapse between calls to
 *  bf_pm_fsm_run for a given state machine and state.
 *  The FSM provides a mechanism for two-way branching
 *  on transition from any state. This is envisioned
 *  to be used primarily as "good path"/'error path"
 *  branching, but can be used for any 2-way branching
 *  purpose.
 *  The FSM also provides a transition function to be
 *  specified to execute on transition FROM any state. This
 *  function is optional. If specified it can be used to
 *  set initial conditions upon entry to a state. It may
 *  also modify the next state and next state wait times,
 *  allowing m-way branching on transition.
 */
typedef struct bf_pm_fsm_state_desc_t {
  bf_pm_fsm_st state;     // current fsm state
  bf_pm_fsm_fn handler;   // function executed when in cur_st
  uint32_t wait_ms;       // wait time to next bf_pm_fsm_run invocation if
                          //   no transition (BF_NOT_READY)
  uint32_t tmout_cycles;  // max time in state (returning BF_NOT_READY)

  bf_pm_fsm_st next_state;  // next state to transition to if cur_fn succeeds
  uint32_t next_state_wait_ms;  // wait time to next bf_pm_fsm_run invocation if
                                //   transitioning (BF_SUCCESS)
  bf_pm_fsm_st
      alt_next_state;  // next state to transition to if cur_fn succeeds
  uint32_t
      alt_next_state_wait_ms;  // wait time to next bf_pm_fsm_run invocation if
                               //   anything other than BF_SUCCESS or
                               //   BF_NOT_READY is returned from handler
  bf_pm_fsm_st alt_next_state_2;  // next state to transition to if cur_fn
                                  //  succeeds with error code BF_ALT2_NEXT_ST.
  uint32_t
      alt_next_state_2_wait_ms;   // wait time to next bf_fsm_run invocation if
                                  //   transitioning (BF_ALT2_NEXT_ST)
  bf_pm_fsm_st alt_next_state_3;  // next state to transition to if cur_fn
                                  //  succeeds with error code BF_ALT3_NEXT_ST.
  uint32_t
      alt_next_state_3_wait_ms;  // wait time to next bf_fsm_run invocation if
                                 //   transitioning (BF_ALT3_NEXT_ST)
  bf_pm_fsm_transition_fn
      transition_fn;  // function to execute if transitioning to
} bf_pm_fsm_state_desc_t;

typedef bf_pm_fsm_state_desc_t *bf_pm_fsm_t;

bf_status_t bf_pm_fsm_bind_cb_to_state(bf_pm_fsm_state_desc_t *fsm,
                                       bf_pm_fsm_st st,
                                       bf_pm_fsm_transition_fn cb);
bf_status_t bf_pm_fsm_run(bf_dev_id_t dev_id,
                          bf_dev_port_t dev_port,
                          bf_pm_fsm_t fsm,
                          bf_pm_fsm_st fsm_st_current,
                          // returned values
                          bf_pm_fsm_st *fsm_st_next,
                          uint32_t *wait_ms);
bool bf_pm_fsm_port_is_valid(bf_dev_id_t dev_id, bf_dev_port_t dev_port);
bool bf_pm_fsm_port_is_enabled(bf_dev_id_t dev_id, bf_dev_port_t dev_port);
int bf_pm_fsm_num_lanes_get(bf_dev_id_t dev_id, bf_dev_port_t dev_port);

/************************************************************************
 * FSM locators
 ************************************************************************/

// tof2
bf_pm_fsm_t bf_pm_get_fsm_for_dfe(void);
bf_pm_fsm_t bf_pm_get_fsm_for_autoneg(void);
bf_pm_fsm_t bf_pm_get_fsm_for_prbs(void);
bf_pm_fsm_t bf_pm_get_fsm_for_mac_loopback(void);
bf_pm_fsm_t bf_pm_get_fsm_for_mac_far_loopback(void);
bf_pm_fsm_t bf_pm_get_fsm_for_pcs_loopback(void);
bf_pm_fsm_t bf_pm_get_fsm_for_sw_model(void);
bf_pm_fsm_t bf_pm_get_fsm_for_emulator(void);
bf_pm_fsm_t bf_pm_get_fsm_for_tx_mode(void);
bf_pm_fsm_t bf_pm_get_fsm_for_pipe_loopback(void);

// tof3
bf_pm_fsm_t bf_pm_get_fsm_for_tof3_dfe(void);
bf_pm_fsm_t bf_pm_get_fsm_for_tof3_autoneg(void);
bf_pm_fsm_t bf_pm_get_fsm_for_tof3_prbs(void);
bf_pm_fsm_t bf_pm_get_fsm_for_tof3_mac_loopback(void);
bf_pm_fsm_t bf_pm_get_fsm_for_tof3_mac_far_loopback(void);
bf_pm_fsm_t bf_pm_get_fsm_for_tof3_pcs_loopback(void);
bf_pm_fsm_t bf_pm_get_fsm_for_tof3_sw_model(void);
bf_pm_fsm_t bf_pm_get_fsm_for_tof3_emulator(void);
bf_pm_fsm_t bf_pm_get_fsm_for_tof3_tx_mode(void);
bf_pm_fsm_t bf_pm_get_fsm_for_tof3_pipe_loopback(void);

typedef enum {
  BF_PM_PORT_FSM_MODE_NONE = 0,
  BF_PM_PORT_FSM_MODE_DFE,
  BF_PM_PORT_FSM_MODE_AUTONEG,
  BF_PM_PORT_FSM_MODE_PRBS,
  BF_PM_PORT_FSM_MODE_PIPE_LOOPBCK,
  BF_PM_PORT_FSM_MODE_MAC_NEAR_LOOPBCK,
  BF_PM_PORT_FSM_MODE_MAC_FAR_LOOPBCK,
  BF_PM_PORT_FSM_MODE_PCS_LOOPBCK,
  BF_PM_PORT_FSM_MODE_SW_MODEL,
  BF_PM_PORT_FSM_MODE_TX_MODE,
  BF_PM_PORT_FSM_MODE_EMULATOR,
} bf_pm_port_fsm_mode_t;

bf_pm_fsm_t bf_pm_fsm_handle_get(bf_pm_port_fsm_mode_t fsm_mode);
bf_pm_fsm_t bf_pm_fsm_tof3_handle_get(bf_pm_port_fsm_mode_t fsm_mode);
void bf_pm_fsm_display_get(bf_pm_fsm_t fsm,
                           bf_pm_fsm_st st,
                           char **fsm_str,
                           char **fsm_st_str);
bf_status_t bf_pm_fsm_port_disable(bf_dev_id_t dev_id,
                                   bf_dev_port_t dev_port,
                                   bf_pm_fsm_t fsm);
#ifdef __cplusplus
}
#endif /* C++ */

#endif  // BF_PM_FSM_H_INCLUDED
