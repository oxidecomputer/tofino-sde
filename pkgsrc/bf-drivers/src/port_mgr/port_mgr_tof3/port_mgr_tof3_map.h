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



#ifndef port_mgr_tof3_map_h
#define port_mgr_tof3_map_h

/* Allow the use in C++ code.  */
#ifdef __cplusplus
extern "C" {
#endif

#include "port_mgr_tof3_physical_dev.h"

port_mgr_err_t port_mgr_tof3_map_dev_port_to_all(bf_dev_id_t dev_id,
                                                 bf_dev_port_t dev_port,
                                                 uint32_t *pipe_id,
                                                 uint32_t *port_id,
                                                 uint32_t *tmac,
                                                 uint32_t *ch,
                                                 bool *is_cpu_port);
uint32_t port_mgr_tof3_map_dev_port_to_port_index(bf_dev_id_t dev_id,
                                                  bf_dev_port_t dev_port);
port_mgr_tof3_serdes_t *port_mgr_tof3_map_tmac_ch_to_serdes(bf_dev_id_t dev_id,
                                                            uint32_t tmac,
                                                            uint32_t ch);
port_mgr_tmac_t *port_mgr_tof3_map_dev_port_lane_to_tmac(
    bf_dev_id_t dev_id, bf_dev_port_t dev_port);

bf_status_t bf_map_logical_tmac_to_physical(bf_dev_id_t dev_id,
                                            bf_subdev_id_t *subdev_id,
                                            uint32_t logical_tmac,
                                            uint32_t *physical_tmac);

bf_status_t bf_map_physical_tmac_to_logical(bf_dev_id_t dev_id,
                                            bf_subdev_id_t subdev_id,
                                            uint32_t physical_tmac,
                                            uint32_t *logical_tmac);

bool port_mgr_tof3_dev_port_is_cpu_port(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port);
#ifdef __cplusplus
}
#endif /* C++ */

#endif
