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


#include <dvm/bf_drv_intf.h>
#include <dvm/dvm_intf.h>
#include <port_mgr/port_mgr_intf.h>
#include <port_mgr/bf_fsm_if.h>
#include <lld/bf_dev_if.h>
#include <bf_types/bf_types.h>
#include <port_mgr/bf_fsm_if.h>
#include "port_mgr.h"
#include "port_mgr_log.h"
#include "port_mgr_dev.h"
#include "port_mgr_ha.h"
#include "port_mgr_port.h"
#include "port_mgr_tof1/port_mgr_tof1.h"
#include "port_mgr_tof2/port_mgr_tof2.h"
#include "port_mgr_tof3/port_mgr_tof3.h"
#include <lld/lld_interrupt_if.h>

static bf_sys_sem_t port_mgr_port_intbh_sem;

static int port_mgr_submodule_debug = 0;

void port_mgr_submodule_debug_set(int submodule) {
  if (submodule >= PORT_MGR_SUBMODULE_MAX) {
    return;
  }
  port_mgr_submodule_debug = (port_mgr_submodule_debug | (1 << submodule));
  return;
}

int is_port_mgr_submodule_log_enable(int submodule_bitmap) {
  return ((port_mgr_submodule_debug & submodule_bitmap) ? 1 : 0);
}

bf_status_t port_mgr_intbh_init(void) {
  int ret = 0;

  lld_register_mac_int_bh_wakeup_cb(port_mgr_port_int_bh_wakeup);
  ret = bf_sys_sem_init(&port_mgr_port_intbh_sem, 0, 0);
  if (ret) {
    return BF_INIT_ERROR;
  }
  return BF_SUCCESS;
}

bf_status_t port_mgr_wakeup_intbh(void) {
  int rc;
  rc = bf_sys_sem_post(&port_mgr_port_intbh_sem);
  if (rc) {
    return BF_EAGAIN;
  }

  return BF_SUCCESS;
}

bf_status_t port_mgr_check_port_int(void) {
  int rc;
  rc = bf_sys_sem_wait(&port_mgr_port_intbh_sem);
  if (rc) {
    return BF_EAGAIN;
  }

  // Resetting the semaphore to 0
  while (!bf_sys_sem_trywait(&port_mgr_port_intbh_sem))
    ;
  return BF_SUCCESS;
}

// global context
static port_mgr_context_t g_ctx;
port_mgr_context_t *port_mgr_ctx = &g_ctx;

/********************************************************************
 ** \brief port_mgr_init:
 *
 * Initialize port_mgr module
 *******************************************************************/
void port_mgr_init(void) {
  /* Register for notificatons */
  bf_drv_client_handle_t bf_drv_hdl;
  bf_status_t r;

  // init the global context structure
  memset((char *)port_mgr_ctx, 0, sizeof(*port_mgr_ctx));

  // register callbacks with DVM
  r = bf_drv_register("port_mgr", &bf_drv_hdl);
  bf_sys_assert(r == BF_SUCCESS);
  bf_drv_client_callbacks_t callbacks = {0};
  callbacks.device_add = port_mgr_dev_add;
  callbacks.device_del = port_mgr_dev_remove;
  callbacks.port_add = port_mgr_port_add;
  callbacks.port_del = port_mgr_port_remove;
  callbacks.port_admin_state = port_mgr_port_enable;

  /* Fast Recfg callbacks */
  callbacks.lock = port_mgr_dev_lock;
  callbacks.unlock_reprogram_core = port_mgr_dev_unlock;
  callbacks.disable_input_pkts = port_mgr_ha_disable_traffic;
  callbacks.enable_input_pkts = port_mgr_ha_enable_input_packets;

  /* Hitless callbacks */
  callbacks.complete_hitless_hw_read = NULL;
  callbacks.compute_delta_changes = port_mgr_ha_compute_delta_changes;
  callbacks.push_delta_changes = port_mgr_ha_push_delta_changes;

  /* Warm init Port callbacks */
  callbacks.register_port_corr_action = port_mgr_ha_register_port_corr_action;
  callbacks.port_delta_push_done = port_mgr_ha_port_delta_push_done;
  callbacks.port_serdes_upgrade = port_mgr_ha_port_serdes_upgrade;

  bf_drv_client_register_callbacks(bf_drv_hdl, &callbacks, BF_CLIENT_PRIO_4);

  bf_drv_client_register_warm_init_flags(bf_drv_hdl,
                                         true /* override fast recfg flag */,
                                         true /* issue_fast_recfg_port_cb */);
}
