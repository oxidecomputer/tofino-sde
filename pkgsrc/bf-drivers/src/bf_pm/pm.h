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


/* pm.h
 *
 * Private PM header file
 */
#ifndef pm_h_included
#define pm_h_included

typedef enum {

  // non-AN fsm states
  PM_FSM_ST_IDLE = 0,
  PM_FSM_ST_WAIT_PLL,
  PM_FSM_ST_WAIT_SIGNAL_OK,
  PM_FSM_ST_START_DFE,
  PM_FSM_ST_WAIT_DFE_DONE,
  PM_FSM_ST_ENA_MAC,
  PM_FSM_ST_WAIT_PCS_UP,
  PM_FSM_ST_WAIT_PCS_DN,
  PM_FSM_ST_END,

  // AN fsm states
  PM_AN_FSM_ST_IDLE,
  PM_AN_FSM_ST_WAIT_PLL1,
  PM_AN_FSM_ST_START_AN,
  PM_AN_FSM_WAIT_AN_DONE,
  PM_AN_FSM_ST_PGM_HCD,
  PM_AN_FSM_ST_WAIT_PLL2,
  PM_AN_FSM_ST_START_LT,
  PM_AN_FSM_ST_WAIT_LT_DONE,
  PM_AN_FSM_ST_WAIT_AN_CMPLT,
  PM_AN_FSM_ST_WAIT_PCS_UP,
  PM_AN_FSM_ST_WAIT_PCS_DN,
  PM_AN_FSM_ST_END,
} pm_fsm_st;

typedef void *pm_dm_handle_t;

typedef bf_status_t (*pm_fsm_fn)(bf_dev_id_t dev_id, bf_dev_port_t dev_port);

typedef struct pm_fsm_state_desc_t {
  pm_fsm_st cur_st;      // current fsm state
  pm_fsm_fn cur_fn;      // function implementing current fsm state
  pm_fsm_st nxt_st;      // next state to transition to if cur_fn succeeds
  uint32_t cur_wait_ms;  // time to wait if no transition
  uint32_t nxt_wait_ms;  // time to wait if transitioning to nxt_st
  pm_fsm_fn trans_fn;  // function to execute on transition to nxt_st (optional)
} pm_fsm_state_desc_t;

// Port Mgr internal data structures
typedef struct pm_port_rmon_stats_t {
  int cur_area;
  bf_rmon_counter_array_t ctr_array[2];  // double-buffered
} pm_port_rmon_stats_t;

typedef struct pm_port_t {
  bf_dev_id_t dev_id;
  bf_dev_port_t dev_port;
  bool added;
  bool an;                // AN disabled=false, AN enabled=true
  bool en;                // disabled=false, enabled=true
  bool up;                // down=false, up=true
  bool presence;	  // presence-detect bit
  bf_sys_mutex_t pm_mtx;  // Lock to protect critical sections
  bool pm_mtx_init;       // indicates pm_mtx was initialized
#ifdef USE_BF_SYSLIB_TIMERS
  bf_sys_timer_t fsm_tmr;
#endif  // USE_BF_SYSLIB_TIMERS

  bf_fsm_t fsm;
  bf_fsm_st fsm_st;
  pm_port_rmon_stats_t stats;
} pm_port_t;

typedef void (*pm_dm_iter_cb)(bf_dev_id_t dev_id, bf_dev_port_t dev_port);

// Port Mgr data management interface

static int pm_dm_port_add(bf_dev_id_t dev_id, bf_dev_port_t dev_port);
static int pm_dm_port_rmv(bf_dev_id_t dev_id, bf_dev_port_t dev_port);

// "set" operations
static int pm_dm_set_en(bf_dev_id_t dev_id, bf_dev_port_t dev_port, bool en);
static int pm_dm_set_up(bf_dev_id_t dev_id, bf_dev_port_t dev_port, bool up);

// "get" operations
static int pm_dm_get_added(bf_dev_id_t dev_id,
                           bf_dev_port_t dev_port,
                           bool *added);
static int pm_dm_get_en(bf_dev_id_t dev_id, bf_dev_port_t dev_port, bool *en);
static int pm_dm_get_an(bf_dev_id_t dev_id, bf_dev_port_t dev_port, bool *an);

static int pm_dm_get_stats_last_area(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port,
                                     bf_rmon_counter_array_t **ctr_array);
static int pm_dm_swap_stats_areas(bf_dev_id_t dev_id, bf_dev_port_t dev_port);

#ifdef USE_BF_SYSLIB_TIMERS
static int pm_dm_get_fsm_tmr(bf_dev_id_t dev_id,
                             bf_dev_port_t dev_port,
                             bf_sys_timer_t **tmr);
#endif  // USE_BF_SYSLIB_TIMERS

static int pm_dm_handle_from_id(bf_dev_id_t dev_id,
                                bf_dev_port_t dev_port,
                                pm_dm_handle_t *handle);
static int pm_dm_id_from_handle(pm_dm_handle_t handle,
                                bf_dev_id_t *dev_id,
                                bf_dev_port_t *dev_port);
void pm_port_fsm_display_info_get(bf_dev_id_t dev_id,
                                  bf_dev_port_t dev_port,
                                  char **fsm_str,
                                  char **fsm_st_str);

#endif  // pm_h_included
