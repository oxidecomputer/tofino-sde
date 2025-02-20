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


#ifndef DVM_H_INCLUDED
#define DVM_H_INCLUDED

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dvm/bf_drv_intf.h>

#define BF_DRV_MAX_CLIENTS 16
#define BF_DRV_CLIENT_NAME_LEN 50

typedef struct dvm_port_ {
  bf_dev_port_t port;
  int added;
  int enabled;
  bf_port_speed_t speed;
  uint32_t lane_numb;
} dvm_port_t;

typedef struct dvm_port_corr_action_ {
  /*
   * This holds the corrective actions for all the ports on this device as
   * deemed fit by each of the DVM clients.
   */
  bf_ha_port_reconcile_info_per_device_t
      port_reconcile_info[BF_DRV_MAX_CLIENTS];
  /*
   * This holds the final corrective action for all the ports on this
   * device as deemed fit by the DVM
   */
  bf_ha_port_reconcile_info_per_device_t final_port_reconcile_info;

  /*
   * The following arrays hold the ports categorized into different actions
   * by the DVM
   */
  bf_dev_port_t port_delete_bucket[BF_PORT_COUNT];
  bf_dev_port_t port_disable_bucket[BF_PORT_COUNT];
  bf_dev_port_t port_add_bucket[BF_PORT_COUNT];
  bf_dev_port_t port_enable_bucket[BF_PORT_COUNT];
  bf_dev_port_t port_bringup_bucket[BF_PORT_COUNT];
  bf_dev_port_t port_fsm_link_monitoring_bucket[BF_PORT_COUNT];
  bf_dev_port_t port_serdes_upgrade_bucket[BF_PORT_COUNT];
} dvm_port_corr_action_t;

typedef struct dvm_asic_ {
  bf_dev_id_t dev_id;
  bf_dev_family_t dev_family;
  int added;
  dvm_port_t port[BF_PIPE_COUNT][BF_PIPE_PORT_COUNT];
  // Pointer to error event notification callback function. May be null.
  bf_error_event_cb event_cb;
  // Cookie for error event callback function.
  void *event_cb_cookie;
  // Logical pipe count.
  uint32_t num_pipes;
  bf_dev_init_mode_t warm_init_mode;
  // Flag to indicate whether any warm init (fast reconfig,
  // fast reconfig quick and hitless HA) is in progress.
  bool warm_init_in_progress;
  bool pktmgr_dev_add_done;
  bool dev_add_done;
  bool is_sw_model;
  bool is_virtual;
  bool is_virtual_dev_slave;
  bool warm_init_error;
  bf_dev_serdes_upgrade_mode_t serdes_upgrade_mode;
  dvm_port_corr_action_t port_corr_action;

  // Port stuck detection
  bf_port_stuck_cb port_stuck_cb;
  void *port_stuck_cb_cookie;
} dvm_asic_t;

typedef struct bf_drv_hdl_info_ {
  bool allocated;
  char client_name[BF_DRV_CLIENT_NAME_LEN];
} bf_drv_hdl_info_t;

/* Internal driver modules registration DB */
typedef struct bf_drv_client_ {
  bool valid;
  bf_drv_client_handle_t client_handle;
  bf_drv_client_prio_t priority;
  char client_name[BF_DRV_CLIENT_NAME_LEN];
  bf_drv_client_callbacks_t callbacks;
  bool override_fast_recfg;      /* This flag indicates if this client wants the
       fast recfg seuquecne overriden by the hitless ha sequence */
  bool issue_fast_recfg_port_cb; /* This flag indicates if this client wants
  port callbacks to be issued even during fast recfg */
} bf_drv_client_t;

/* App registraton DB */
typedef struct bf_drv_app_ {
  bf_drv_port_status_cb port_status;
  bf_drv_port_speed_cb port_speed;
  bf_drv_port_mode_change_cb port_mode_change;
  bf_drv_port_mode_change_complete_cb port_mode_change_complete;
  void *port_status_cookie;
  void *port_speed_cookie;
  void *port_mode_change_cookie;
  void *port_mode_change_complete_cookie;
} bf_drv_app_t;

typedef enum bf_fast_reconfig_step_e {
  BF_RECONFIG_LOCK,
  BF_RECONFIG_UNLOCK,
  BF_RECONFIG_UNLOCK_QUICK,
} bf_fast_reconfig_step_t;

/* Reconfig params */
typedef struct reconfig_params_ {
  bool lock;
  bool create_dma;
  bool disable_input_pkts;
  bool wait_for_flush;
  bool core_reset;
  bool unlock_reprogram_core;
  bool config_complete;
  bool enable_input_pkts;
  bool warm_init_quick;
  bool error_cleanup;
  bool wait_for_swcfg_replay_end;
} reconfig_params_s;

bf_status_t bf_drv_notify_clients_pktmgr_dev_add(
    bf_dev_id_t dev_id,
    bf_dev_family_t dev_family,
    bf_dma_info_t *dma_info,
    bf_dev_init_mode_t warm_init_mode);

bf_status_t bf_drv_notify_clients_dev_add(bf_dev_id_t dev_id,
                                          bf_dev_family_t dev_family,
                                          bf_device_profile_t *profile,
                                          bf_dma_info_t *dma_info,
                                          bf_dev_init_mode_t warm_init_mode);
bf_status_t bf_drv_notify_clients_virtual_dev_add(
    bf_dev_id_t dev_id,
    bf_dev_type_t dev_type,
    bf_device_profile_t *profile,
    bf_dev_init_mode_t warm_init_mode);

bf_status_t bf_drv_notify_clients_dev_type_virtual_dev_slave(
    bf_dev_id_t dev_id);

bf_status_t bf_drv_notify_clients_dev_del(bf_dev_id_t dev_id, bool log_err);
bf_status_t bf_drv_notify_clients_dev_log(bf_dev_id_t dev_id,
                                          const char *filepath);
bf_status_t bf_drv_notify_clients_dev_restore(bf_dev_id_t dev_id,
                                              const char *filepath);
bf_status_t bf_drv_notify_clients_port_add(bf_dev_id_t dev_id,
                                           bf_dev_port_t port_id,
                                           bf_port_speeds_t speed,
                                           uint32_t n_lanes,
                                           bf_fec_types_t port_fec_type);
bf_status_t bf_drv_notify_clients_port_del(bf_dev_id_t dev_id,
                                           bf_dev_port_t port_id,
                                           bool log_err);
void bf_drv_notify_clients_port_status_chg(bf_dev_id_t dev_id,
                                           bf_dev_port_t port_id,
                                           port_mgr_port_event_t event,
					   uint32_t eventdata,
                                           void *userdata);
bf_status_t bf_drv_notify_clients_port_serdes_upgrade(
    bf_dev_id_t dev_id,
    bf_dev_port_t port_id,
    uint32_t serdes_fw_version,
    char *serdes_fw_path);
bf_status_t bf_drv_notify_clients_port_admin_state(bf_dev_id_t dev_id,
                                                   bf_dev_port_t port_id,
                                                   bool enable);
bf_status_t bf_drv_skip_port_delta_push_set(bf_dev_id_t dev_id, bool val);
bf_status_t bf_drv_notify_clients_err_interrupt_handling_mode(
    bf_dev_id_t dev_id, bool enable);
bf_status_t bf_drv_apply_reconfig_step(bf_dev_id_t dev_id,
                                       bf_fast_reconfig_step_t step);

bf_status_t bf_drv_notify_clients_complete_hitless_hw_read(bf_dev_id_t dev_id);

bf_status_t bf_drv_notify_clients_hitless_warm_init_end(bf_dev_id_t dev_id);

bf_status_t bf_drv_get_port_delta_from_clients(
    bf_dev_id_t dev_id, dvm_port_corr_action_t *port_corr);

bf_status_t bf_drv_notify_clients_port_delta_push_done(bf_dev_id_t dev_id);

bf_status_t bf_drv_get_get_next_client_for_port_corrective_actions(
    bf_dev_id_t dev_id, int cur_client, int *nxt_client);
bool bf_drv_check_port_mode_transition_wa(bf_dev_id_t dev_id,
                                          bf_dev_port_t port_id,
                                          bf_port_speeds_t port_speed);

bool bf_drv_client_port_mode_change_callback_registered(bf_dev_id_t dev_id);

ucli_status_t bf_drv_show_tech_ucli_dvm__(ucli_context_t *uc);

#endif  // DVM_H_INCLUDED
