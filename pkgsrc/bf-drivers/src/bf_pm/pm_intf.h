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


#ifndef pm_intf_h_included
#define pm_intf_h_included

#include <port_mgr/bf_port_if.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include "port_fsm/bf_pm_fsm_if.h"

// Port Mgr application interface

void pm_init(bf_dev_id_t dev_id);
void pm_terminate(bf_dev_id_t dev_id);

// Device-specific operations
void pm_dev_add_serdes_init(bf_dev_id_t dev_id);

// Port-specific operations
bf_status_t pm_port_add(bf_dev_id_t dev_id,
                        bf_dev_port_t dev_port,
                        bf_port_speeds_t port_speed,
                        uint32_t n_lanes,
                        bf_fec_types_t port_fec_type);
bf_status_t pm_port_rmv(bf_dev_id_t dev_id,
                        bf_dev_port_t dev_port,
                        bool soft_remove_only);
bf_status_t pm_port_del(bf_dev_id_t dev_id, bf_dev_port_t dev_port);
bf_status_t pm_port_enable(bf_dev_id_t dev_id, bf_dev_port_t dev_port);
bf_status_t pm_port_disable(bf_dev_id_t dev_id, bf_dev_port_t dev_port);
int pm_dm_get_stats_curr_area(bf_dev_id_t dev_id,
                              bf_dev_port_t dev_port,
                              bf_rmon_counter_array_t **ctr_array);
bf_status_t pm_port_set_an(bf_dev_id_t dev_id, bf_dev_port_t dev_port, bool an);
void pm_port_set_autoneg_parms(bf_dev_id_t dev_id,
                               bf_dev_port_t dev_port,
                               bf_an_adv_speeds_t p_speed,
                               bf_an_fec_t p_fec,
                               bf_pause_cfg_t tx_pause,
                               bf_pause_cfg_t rx_pause);
bf_status_t pm_max_ports_get(bf_dev_id_t dev_id, uint32_t *num_ports);
bf_status_t pm_fp_idx_to_dev_port_map(bf_dev_id_t dev_id,
                                      uint32_t fp_idx,
                                      bf_dev_port_t *dev_port);
bf_status_t pm_recirc_port_range_get(bf_dev_id_t dev_id,
                                     uint32_t *start_recirc_port,
                                     uint32_t *end_recirc_port);
bf_status_t pm_recirc_port_to_dev_port_map(bf_dev_id_t dev_id,
                                           uint32_t recirc_port,
                                           bf_dev_port_t *dev_port);
bf_status_t pm_port_status_chg_cb(bf_dev_id_t dev_id,
                                  bf_dev_port_t dev_port,
                                  bool up,
                                  void *unused);
bf_status_t pm_port_skip_serdes_fsm_set(bf_dev_port_t dev_port, bool val);
bf_status_t pm_port_cfg_replay_flag_set(bf_dev_id_t dev_id, bool val);
int pm_port_fsm_init(bf_dev_id_t dev_id, bf_dev_port_t dev_port);
int pm_port_fsm_deinit(bf_dev_id_t dev_id, bf_dev_port_t dev_port);
bf_status_t pm_port_interrupt_based_link_monitoring_set(bf_dev_id_t dev_id,
                                                        bool val);
bf_status_t pm_port_interrupt_based_link_monitoring_get(bf_dev_id_t dev_id,
                                                        bool *val);
bf_status_t pm_port_fsm_for_tx_mode_set(bf_dev_port_t dev_port, bool val);
int pm_port_fsm_set_down_event_state(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port);
int pm_port_fsm_init_in_up_state(bf_dev_id_t dev_id, bf_dev_port_t dev_port);

bf_status_t pm_port_fsm_mode_set(bf_dev_port_t dev_port,
                                 bf_pm_port_fsm_mode_t fsm_mode);
bf_pm_port_fsm_mode_t pm_port_fsm_mode_get(bf_dev_port_t dev_port);
bf_status_t pm_port_tof2_prbs_stats_get(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        bf_port_sds_prbs_stats_t *stats);
bf_status_t pm_port_tof2_eye_val_get(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port,
                                     bf_port_eye_val_t *eye);
bf_status_t pm_port_tof2_ber_get(bf_dev_id_t dev_id,
                                 bf_dev_port_t dev_port,
                                 bf_port_ber_t *ber);

ucli_status_t bf_drv_show_tech_ucli_pm__(ucli_context_t *uc);
ucli_status_t bf_pm_ucli_ucli__sku__(ucli_context_t *uc);
#endif  // pm_intf_h_included
