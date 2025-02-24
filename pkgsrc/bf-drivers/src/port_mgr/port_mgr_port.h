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


#ifndef PORT_MGR_PORT_H_INCLUDED
#define PORT_MGR_PORT_H_INCLUDED

/* Allow the use in C++ code.  */
#ifdef __cplusplus
extern "C" {
#endif

// Default max-frame-size
#define PORT_MGR_TOF_DEFLT_MAX_FRAME_SZ (16 * 1024 - 256)

bf_status_t port_mgr_port_add(bf_dev_id_t dev_id,
                              bf_dev_port_t dev_port,
                              bf_port_attributes_t *port_attrib,
                              bf_port_cb_direction_t direction);
bf_status_t port_mgr_port_remove(bf_dev_id_t dev_id,
                                 bf_dev_port_t dev_port,
                                 bf_port_cb_direction_t direction);
bf_status_t port_mgr_port_enable(bf_dev_id_t dev_id,
                                 bf_dev_port_t dev_port,
                                 bool enable);
void port_mgr_link_up_actions(bf_dev_id_t dev_id, bf_dev_port_t dev_port);
void port_mgr_link_dn_actions(bf_dev_id_t dev_id, bf_dev_port_t dev_port);
void port_mgr_fsm_actions(bf_dev_id_t dev_id,
			  bf_dev_port_t dev_port,
			  bf_fsm_st state);
void port_mgr_presence_actions(bf_dev_id_t dev_id,
			  bf_dev_port_t dev_port,
			  bool presence);
bf_status_t port_mgr_port_read_counter(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port,
                                       bf_rmon_counter_t ctr_id,
                                       uint64_t *ctr_value);
bf_status_t port_mgr_port_read_all_counters(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port);
int port_mgr_get_num_lanes(bf_dev_id_t dev_id, bf_dev_port_t dev_port);
bf_status_t port_mgr_port_bind_interrupt_callback(bf_dev_id_t dev_id,
                                                  bf_dev_port_t dev_port,
                                                  bf_port_int_callback_t fn,
                                                  void *userdata);
bf_status_t port_mgr_port_config_get(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port);
bf_status_t port_mgr_port_bring_up_time_get(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port,
                                            uint64_t *bring_up_time_us);
bf_status_t port_mgr_port_link_up_time_get(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           uint64_t *bring_up_time_us);
bf_status_t port_mgr_port_signal_detect_time_set(bf_dev_id_t dev_id,
                                                 bf_dev_port_t dev_port);
bf_status_t port_mgr_port_an_lt_start_time_set(bf_dev_id_t dev_id,
                                               bf_dev_port_t dev_port);
bf_status_t port_mgr_port_an_lt_dur_set(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port);
bf_status_t port_mgr_port_an_lt_stats_get(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port,
                                          uint64_t *an_lt_dur_us,
                                          uint32_t *an_try_cnt);
bf_status_t port_mgr_port_an_try_inc(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port);
bf_status_t port_mgr_port_an_lt_stats_init(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           bool init_all);
ucli_status_t bf_drv_show_tech_ucli_port__(ucli_context_t *uc);
void port_mgr_port_default_int_bh_wakeup_cb(bf_dev_id_t dev_id);
void port_mgr_handle_port_int_notif(bf_dev_id_t dev_id);
void port_mgr_tofino3_handle_port_int_notif(bf_dev_id_t dev_id);
void port_mgr_port_int_bh_wakeup(bf_dev_id_t dev_id);
uint port_mgr_max_frame_sz_get(bf_dev_id_t dev_id);
#ifdef __cplusplus
}
#endif /* C++ */

#endif  // PORT_MGR_PORT_H_INCLUDED
