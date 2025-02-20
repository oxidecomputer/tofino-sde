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


#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sys/time.h>

#include <bf_types/bf_types.h>
#include <dvm/bf_drv_intf.h>
#include <lld/bf_dma_if.h>
#include <lld/lld_dr_if.h>
#include <lld/lld_err.h>
#include <lld/lld_sku.h>
#include <lld/lld_reg_if.h>
#include <port_mgr/port_mgr_intf.h>
#include <port_mgr/bf_fsm_if.h>
#include <port_mgr/bf_port_if.h>
#include <port_mgr/bf_serdes_if.h>
#include <port_mgr/port_mgr_port_evt.h>
#include <port_mgr/port_mgr_ha.h>
#include <port_mgr/port_mgr.h>
#include <port_mgr/port_mgr_map.h>
#include <port_mgr/port_mgr_port.h>
#include <port_mgr/port_mgr_log.h>
#include "port_mgr_mac_stats.h"
#include "port_mgr_tof1/port_mgr_mac.h"
#include "port_mgr_tof1/port_mgr_tof1_port.h"

/**
 * @file bf_map.c
 * \brief Details Public mapping APIs.
 *
 */

/**
 * @addtogroup port-mgr-map
 * @{
 * APIs for mapping various port mgr elements
 */

/** \brief Map logical umac3 ID to physical umac3 ID
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id        : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param logical_umac  : logical umac
 * \param physical_umac : ptr to returned physical umac
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid umac id
 *
 */
bf_status_t bf_map_logical_umac3_to_physical(bf_dev_id_t dev_id,
                                             uint32_t logical_umac,
                                             uint32_t *physical_umac) {
  // for now, identity mapping
  *physical_umac = logical_umac;
  (void)dev_id;
  return BF_SUCCESS;
}

/** \brief Map logical umac4 ID to physical umac4 ID
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id        : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param logical_umac  : logical umac
 * \param physical_umac : ptr to returned physical umac
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid umac id
 *
 */
bf_status_t bf_map_logical_umac4_to_physical(bf_dev_id_t dev_id,
                                             uint32_t logical_umac,
                                             uint32_t *physical_umac) {
  // for now, identity mapping
  *physical_umac = logical_umac;
  (void)dev_id;
  return BF_SUCCESS;
}
