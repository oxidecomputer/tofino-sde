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


// Used only for internal debug
/*
   port_mgr.c

   Port Manager example

   This file implements a simple port handling module. The module handles
   the following functionality:

   - port add, delete, enable, disable
   - running sample port bring-up FSMs, one for AN one for non-AN
   - collecting port status for enabled ports on a timer basis
   - collecting RMON stats for "up" ports on a timer basis
   - registering for port status change callbacks

   The module is composed of two parts:

   - A "Port Mgr Data Management" layer (pm_dm_ functions)
   - A "Port Mgr" layer (pm_ functions)

   The data management layer maintains a set of simple "port" data
   structures tracking a ports configuration and state. It provides
   the "port mgr" layer with APIs to get and set all properties of
   the port. It may be replaced by any implementation, as long as the
   pm_dm_get/set interface to pm_ is maintained.

*/
/* Standard includes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

/* <bf_syslib> includes */
#include <target-sys/bf_sal/bf_sys_intf.h>

/* <bf_driver> includes */
#include <dvm/bf_drv_intf.h>
#include <port_mgr/port_mgr_intf.h>
#include <lld/bf_dma_if.h>
#include <lld/bf_dev_if.h>
#include <lld/lld_err.h>
#include <lld/lld_sku.h>
#include <port_mgr/bf_port_if.h>
#include <port_mgr/bf_serdes_if.h>
#include <port_mgr/bf_tof2_serdes_if.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include <port_mgr/bf_fsm_if.h>
#include <dvm/bf_drv_intf.h>
#include <port_mgr/bf_fsm_hdlrs.h>
#include <port_mgr/port_mgr_port.h>
#include <port_mgr/port_mgr_tof2/umac4_ctrs.h>
#include <port_mgr/port_mgr_tof2/port_mgr_tof2_map.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include <bf_pm/bf_pm_intf.h>

/* local includes */
#include "pm.h"
#include "pm_log.h"
#include "pm_intf.h"
#include "pm_task.h"
#include "port_fsm/bf_pm_fsm_if.h"

#ifdef USE_BF_SYSLIB_TIMERS
static void pm_start_timer(pm_dm_handle_t handle, uint32_t ms);
#endif  // USE_BF_SYSLIB_TIMERS

/* Port Mgr Data management data structures

   The "ports" are maintained in a single structure, pm_dm_db.
   Any port can be located directly by decoding the bf_dev_id_t
   and bf_dev_port_t "keys" into chip #, pipe #, and port #
   components and indexing directly into the structure.

   pm_dm provides iteration functions to iterate over pm_dm_db
   and issue callbacks based on certain criteria (e.g. whether or not
   the port is "up").

   As a slight optimization, a variable identifying the highest
   bf_dev_id_t value used in the add API is maintained to limit
   the depth of the iteration to only those bf_dev_id_t's that
   have had ports added.
*/

typedef struct pm_dm_pipe_db_t {
  pm_port_t port_db[BF_PIPE_PORT_COUNT];
} pm_dm_pipe_db_t;

typedef struct pm_dm_chip_db_t {
  bool in_cfg_replay;
  bool interrupt_based_link_monitoring;
  pm_dm_pipe_db_t pipe_db[BF_PIPE_COUNT];
} pm_dm_chip_db_t;

typedef struct pm_dm_db_t {
  pm_dm_chip_db_t chip_db[BF_MAX_DEV_COUNT];
} pm_dm_db_t;

int g_pm_max_asic_seen = 0;  // limits what we bother iterating over

pm_dm_db_t pm_dm_db;

static bool skip_serdes_fsm[BF_PORT_COUNT];

static bool is_fsm_for_tx_mode[BF_PORT_COUNT];

static bf_pm_port_fsm_mode_t port_fsm_mode[BF_PORT_COUNT];
static bool port_fsm_stop[BF_MAX_DEV_COUNT][BF_PORT_COUNT];

// Port Mgr data management interface

/************************************************************************
 * pm_dm_find_dev
 *
 * Return the pm_dm_chip_db_tstructure identified by dev_id
 ************************************************************************/
static pm_dm_chip_db_t *pm_dm_find_dev(bf_dev_id_t dev_id) {
  return (&pm_dm_db.chip_db[dev_id]);
}

/************************************************************************
 * pm_dm_get_interrupt_based_link_monitoring
 *
 * Return the interrupt_based_link_monitoring flag
 ************************************************************************/
static int pm_dm_get_interrupt_based_link_monitoring(bf_dev_id_t dev_id,
                                                     bool *en) {
  pm_dm_chip_db_t *chip_db;

  if (dev_id >= BF_MAX_DEV_COUNT) return -1;
  if (en == NULL) return -2;

  chip_db = pm_dm_find_dev(dev_id);
  *en = chip_db->interrupt_based_link_monitoring;
  return 0;
}

/************************************************************************
 * pm_dm_set_interrupt_based_link_monitoring
 *
 * Set the interrupt_based_link_monitoring flag
 *
 * Note: This API is intended ONLY to be called once with en=true. We
 * do not support turning it off and on at run-time.
 ************************************************************************/
static int pm_dm_set_interrupt_based_link_monitoring(bf_dev_id_t dev_id,
                                                     bool en) {
  pm_dm_chip_db_t *chip_db;

  if (dev_id >= BF_MAX_DEV_COUNT) return -1;

  chip_db = pm_dm_find_dev(dev_id);
  if (en == false) {
    // allow setting to false if its not enabled
    if (!chip_db->interrupt_based_link_monitoring) {
      return 0;
    } else {
      // but error to try to turn it off after its been enabled
      return -1;
    }
  }
  chip_db->interrupt_based_link_monitoring = true;
  // disable all MAC ints. Caller will (soon) enable specific LF/RF bits
  bf_port_interrupt_based_link_monitoring_enable(dev_id);
  return 0;
}

/************************************************************************
 * pm_dm_get_in_cfg_replay
 *
 * Return the interrupt_based_link_monitoring flag
 ************************************************************************/
static int pm_dm_get_in_cfg_replay(bf_dev_id_t dev_id, bool *en) {
  pm_dm_chip_db_t *chip_db;

  if (dev_id >= BF_MAX_DEV_COUNT) return -1;
  if (en == NULL) return -2;

  chip_db = pm_dm_find_dev(dev_id);
  *en = chip_db->in_cfg_replay;
  return 0;
}

/************************************************************************
 * pm_dm_set_in_cfg_replay
 *
 * Set the interrupt_based_link_monitoring flag
 ************************************************************************/
static int pm_dm_set_in_cfg_replay(bf_dev_id_t dev_id, bool en) {
  pm_dm_chip_db_t *chip_db;

  if (dev_id >= BF_MAX_DEV_COUNT) return -1;

  chip_db = pm_dm_find_dev(dev_id);
  chip_db->in_cfg_replay = en;
  return 0;
}

/************************************************************************
 * pm_dm_find
 *
 * Return the pm_port_t structure identified by the tuple,
 *   { dev_id, dev_port }
 *
 * The correct structure is located by decomposing dev_port into
 * its pipe and port components and directly indexing pm_dm_db.
 ************************************************************************/
static pm_port_t *pm_dm_find(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  uint32_t pipe, port;

  if (!DEV_PORT_VALIDATE(dev_port)) return NULL;

  pipe = DEV_PORT_TO_PIPE(dev_port);
  port = DEV_PORT_TO_LOCAL_PORT(dev_port);

  return (&pm_dm_db.chip_db[dev_id].pipe_db[pipe].port_db[port]);
}

/************************************************************************
 * pm_dm_lock_init_this_port
 *
 * Initialize the lock for a particular port
 *   { dev_id, dev_port }
 *
 * The corresponding lock of a port is initialized so that it can guard
 * the critical sections
 ************************************************************************/
static void pm_dm_lock_init_this_port(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port) {
  pm_port_t *port = pm_dm_find(dev_id, dev_port);

  if (!port) return;
  bf_sys_mutex_init(&port->pm_mtx);
  port->pm_mtx_init = true;
}

/************************************************************************
 * pm_dm_lock_deinit_this_port
 *
 * Deletes the lock for a particular port
 *   { dev_id, dev_port }
 *
 ************************************************************************/
static void pm_dm_lock_deinit_this_port(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port) {
  pm_port_t *port = pm_dm_find(dev_id, dev_port);

  if (!port) return;

  if (port->pm_mtx_init) {
    bf_sys_mutex_del(&port->pm_mtx);
    port->pm_mtx_init = false;
  }
}

/************************************************************************
 * pm_dm_exclusive_access_start
 *
 * Gain exclusive access to the port structure
 *   { dev_id, dev_port }
 *
 * Exclusive access is obtained by holding the per port lock
 ************************************************************************/
static void pm_dm_exclusive_access_start(bf_dev_id_t dev_id,
                                         bf_dev_port_t dev_port) {
  pm_port_t *port;
  int rc;

  port = pm_dm_find(dev_id, dev_port);
  if (!port) return;

  if (port->pm_mtx_init) {
    rc = bf_sys_mutex_lock(&port->pm_mtx);
  } else {
    rc = -1;
  }

  if (rc) {
    PM_ERROR("Error getting pm lock : Error %d : dev_id=%d : dev_port=%xh",
             rc,
             dev_id,
             dev_port);
  }
}

/************************************************************************
 * pm_dm_exclusive_access_end
 *
 * Give up exclusive access of the port structure
 *   { dev_id, dev_port }
 *
 * Exclusive access is surrendered by releasing the per port lock
 ************************************************************************/
static void pm_dm_exclusive_access_end(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port) {
  pm_port_t *port;
  int rc;

  port = pm_dm_find(dev_id, dev_port);
  if (!port) return;

  if (port->pm_mtx_init) {
    rc = bf_sys_mutex_unlock(&port->pm_mtx);
  } else {
    rc = -1;
  }
  if (rc) {
    PM_ERROR("Error releasing pm lock : Error %d : dev_id=%d : dev_port=%xh",
             rc,
             dev_id,
             dev_port);
  }
}

/************************************************************************
 * pm_dm_lock_init
 *
 * Initialize the lock for all the ports on a chip
 *   { dev_id, dev_port }
 *
 * The locks of all the ports on the chip are initialized
 ************************************************************************/
static void pm_dm_lock_init(bf_dev_id_t dev_id) {
  int pipe, port;
  uint32_t num_subdev = 0;

  lld_sku_get_num_subdev(dev_id, &num_subdev, NULL);
  for (pipe = 0; pipe < (int)(BF_SUBDEV_PIPE_COUNT * num_subdev); pipe++) {
    for (port = 0; port < BF_PIPE_PORT_COUNT; port++) {
      bf_dev_port_t dev_port = MAKE_DEV_PORT(pipe, port);
      pm_dm_lock_init_this_port(dev_id, dev_port);
    }
  }
}

/************************************************************************
 * pm_dm_lock_deinit
 *
 * Deletes the lock for all the ports on a chip
 *   { dev_id, dev_port }
 *
 * The locks of all the ports on the chip are deleted
 ************************************************************************/
static void pm_dm_lock_deinit(bf_dev_id_t dev_id) {
  int pipe, port;
  uint32_t num_subdev = 0;

  lld_sku_get_num_subdev(dev_id, &num_subdev, NULL);
  for (pipe = 0; pipe < (int)(BF_SUBDEV_PIPE_COUNT * num_subdev); pipe++) {
    for (port = 0; port < BF_PIPE_PORT_COUNT; port++) {
      bf_dev_port_t dev_port = MAKE_DEV_PORT(pipe, port);
      pm_dm_lock_deinit_this_port(dev_id, dev_port);
    }
  }
}

/************************************************************************
 * pm_dm_port_add
 *
 * Initialize the pm_port_t structure identified by the tuple,
 *   { dev_id, dev_port }
 *
 * All member variables are initialized to default values. The "an"
 * field is set to the value of the autoneg_enable argument. This will
 * be used (later) to determine which FSM to run when the port is enabled.
 *
 * The "st" member is moved from PM_PORT_ST_NOT_ADDED to
 * PM_PORT_ST_DON.
 *
 * The g_pm_max_asic_seen is updated for the iteration functions.
 ************************************************************************/
static int pm_dm_port_add(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  pm_port_t *pm_port_p = pm_dm_find(dev_id, dev_port);

  if (pm_port_p == NULL) return -1;
  if (pm_port_p->added) return -2;

  // init the port structure
  pm_port_p->dev_id = dev_id;
  pm_port_p->dev_port = dev_port;
  pm_port_p->an = false;
  pm_port_p->en = false;
  pm_port_p->up = false;
  pm_port_p->added = true;

  // update our highest known chip#, if necessary
  if (dev_id > g_pm_max_asic_seen) {
    g_pm_max_asic_seen = dev_id;
  }
  return 0;
}

/************************************************************************
 * pm_dm_port_rmv
 *
 * Re-initialize the pm_port_t structure identified by the tuple,
 *   { dev_id, dev_port }
 *
 * All member variables are initialized to default values.
 *
 * The "st" member is moved to PM_PORT_ST_NOT_ADDED.
 ************************************************************************/
static int pm_dm_port_rmv(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  pm_port_t *pm_port_p = pm_dm_find(dev_id, dev_port);

  if (pm_port_p == NULL) return -1;
  if (!pm_port_p->added) return -2;

  // de-init the port structure
  pm_port_p->dev_id = dev_id;
  pm_port_p->dev_port = dev_port;
  pm_port_p->an = false;
  pm_port_p->en = false;
  pm_port_p->up = false;
  pm_port_p->added = false;
  return 0;
}

/************************************************************************
 * pm_dm_handle_from_id
 *
 * Return a "handle" to the pm_port_t structure identified by the tuple,
 *   { dev_id, dev_port }
 *
 * The handle provides an abstraction to the pm_ layer so the actual
 * pm_dm_ layer data structures are not exposed.
 *
 * The handle is a void * pointer to the pm_port_t structure. It is
 * used by the tasklet scheduler to pass a "context" to the FSMs.
 ************************************************************************/
static int pm_dm_handle_from_id(bf_dev_id_t dev_id,
                                bf_dev_port_t dev_port,
                                pm_dm_handle_t *handle) {
  pm_port_t *pm_port_p = pm_dm_find(dev_id, dev_port);

  if (pm_port_p == NULL) return -1;
  if (!pm_port_p->added) return -2;

  *handle = (pm_dm_handle_t)pm_port_p;
  return 0;
}

/************************************************************************
 * pm_dm_id_from_handle
 *
 * Return the id, given by the tuple,
 *   { dev_id, dev_port }
 *
 * associated with the passed handle.
 *
 * The handle is used by the FSM to reconstruct the arguments required
 * by the FSM state handler functions.
 ************************************************************************/
static int pm_dm_id_from_handle(pm_dm_handle_t handle,
                                bf_dev_id_t *dev_id,
                                bf_dev_port_t *dev_port) {
  pm_port_t *pm_port_p = (pm_port_t *)handle;

  if (pm_port_p == NULL) return -1;
  if (!pm_port_p->added) return -2;

  *dev_id = pm_port_p->dev_id;
  *dev_port = pm_port_p->dev_port;
  return 0;
}

/************************************************************************
 * pm_dm_get_added
 *
 * Return whether or not the port has been added
 * (enabled=true, disabled=false) of the specified port.
 ************************************************************************/
static int pm_dm_get_added(bf_dev_id_t dev_id,
                           bf_dev_port_t dev_port,
                           bool *added) {
  pm_port_t *pm_port_p = pm_dm_find(dev_id, dev_port);

  if (pm_port_p == NULL) return -1;
  if (added == NULL) return -3;

  *added = pm_port_p->added;
  return 0;
}

/************************************************************************
 * pm_dm_get_en
 *
 * Return the configured administrative state
 * (enabled=true, disabled=false) of the specified port.
 ************************************************************************/
static int pm_dm_get_en(bf_dev_id_t dev_id, bf_dev_port_t dev_port, bool *en) {
  pm_port_t *pm_port_p = pm_dm_find(dev_id, dev_port);

  if (pm_port_p == NULL) return -1;
  if (!pm_port_p->added) return -2;
  if (en == NULL) return -3;

  *en = pm_port_p->en;
  return 0;
}

/************************************************************************
 * pm_dm_set_en
 *
 * Set the administrative state (enabled=true, disabled=false) of
 * the specified port.
 ************************************************************************/
static int pm_dm_set_en(bf_dev_id_t dev_id, bf_dev_port_t dev_port, bool en) {
  pm_port_t *pm_port_p = pm_dm_find(dev_id, dev_port);

  if (pm_port_p == NULL) return -1;
  if (!pm_port_p->added) return -2;

  pm_port_p->en = en;
  return 0;
}

/************************************************************************
 * pm_dm_set_an
 *
 * Set the desired autonegotiation state
 * (enabled=true, disabled=false) of the specified port.
 ************************************************************************/
static int pm_dm_set_an(bf_dev_id_t dev_id, bf_dev_port_t dev_port, bool an) {
  pm_port_t *pm_port_p = pm_dm_find(dev_id, dev_port);

  if (pm_port_p == NULL) return -1;
  if (!pm_port_p->added) return -2;

  pm_port_p->an = an;
  return 0;
}

/************************************************************************
 * pm_dm_get_an
 *
 * Return the configured autonegotiation state
 * (enabled=true, disabled=false) of the specified port.
 ************************************************************************/
static int pm_dm_get_an(bf_dev_id_t dev_id, bf_dev_port_t dev_port, bool *an) {
  pm_port_t *pm_port_p = pm_dm_find(dev_id, dev_port);

  if (pm_port_p == NULL) return -1;
  if (!pm_port_p->added) return -2;
  if (an == NULL) return -3;

  *an = pm_port_p->an;
  return 0;
}

/************************************************************************
 * pm_dm_set_up
 *
 * Set the operational state (enabled=true, disabled=false) of
 * the specified port.
 ************************************************************************/
static int pm_dm_set_up(bf_dev_id_t dev_id, bf_dev_port_t dev_port, bool up) {
  pm_port_t *pm_port_p = pm_dm_find(dev_id, dev_port);

  if (pm_port_p == NULL) return -1;
  if (!pm_port_p->added) return -2;

  pm_port_p->up = up;
  return 0;
}

#ifdef USE_BF_SYSLIB_TIMERS
/************************************************************************
 * pm_dm_get_fsm_tmr
 *
 * Return the FSM and current state of the specified port.
 ************************************************************************/
static int pm_dm_get_fsm_tmr(bf_dev_id_t dev_id,
                             bf_dev_port_t dev_port,
                             bf_sys_timer_t **tmr) {
  pm_port_t *pm_port_p = pm_dm_find(dev_id, dev_port);

  if (pm_port_p == NULL) return -1;
  if (!pm_port_p->added) return -2;
  if (tmr == NULL) return -3;

  *tmr = &pm_port_p->fsm_tmr;
  return 0;
}
#endif  // USE_BF_SYSLIB_TIMERS

/************************************************************************
 * pm_dm_get_fsm
 *
 * Return the FSM and current state of the specified port.
 ************************************************************************/
static int pm_dm_get_fsm(bf_dev_id_t dev_id,
                         bf_dev_port_t dev_port,
                         bf_fsm_t *fsm,
                         bf_fsm_st *fsm_cur_st) {
  pm_port_t *pm_port_p = pm_dm_find(dev_id, dev_port);

  if (pm_port_p == NULL) return -1;
  if (!pm_port_p->added) return -2;
  if (fsm == NULL) return -3;
  if (fsm_cur_st == NULL) return -3;

  *fsm = pm_port_p->fsm;
  *fsm_cur_st = pm_port_p->fsm_st;
  return 0;
}

/************************************************************************
 * pm_dm_set_fsm
 *
 * Set the FSM and state to be used to bring up the specified port.
 ************************************************************************/
static int pm_dm_set_fsm(bf_dev_id_t dev_id,
                         bf_dev_port_t dev_port,
                         bf_fsm_t fsm,
                         bf_fsm_st fsm_cur_st) {
  pm_port_t *pm_port_p = pm_dm_find(dev_id, dev_port);
  bf_fsm_st fsm_old_st;

  if (pm_port_p == NULL) return -1;
  if (!pm_port_p->added) return -2;

  fsm_old_st = pm_port_p->fsm_st;
  pm_port_p->fsm = fsm;
  pm_port_p->fsm_st = fsm_cur_st;

  if (fsm_old_st != fsm_cur_st) {
	  bf_fsm_port_t p;
	  p.asic_id = dev_port;
	  bf_pm_fsm_transition(BF_FSM_PORT, p, fsm_old_st, fsm_cur_st);
	  port_mgr_fsm_actions(dev_id, dev_port, fsm_cur_st);
  }

  return 0;
}

/************************************************************************
 * pm_dm_get_stats_curr_area
 *
 * Return a pointer to the bf_rmon_counter_array_t that is marked as
 * "current".
 ************************************************************************/
int pm_dm_get_stats_curr_area(bf_dev_id_t dev_id,
                              bf_dev_port_t dev_port,
                              bf_rmon_counter_array_t **ctr_array) {
  pm_port_t *pm_port_p = pm_dm_find(dev_id, dev_port);
  int cur_area;

  if (pm_port_p == NULL) return -1;
  if (!pm_port_p->added) return -2;
  if (ctr_array == NULL) return -3;

  cur_area = pm_port_p->stats.cur_area;
  *ctr_array = &pm_port_p->stats.ctr_array[cur_area];
  return 0;
}

/************************************************************************
 * pm_dm_get_stats_last_area
 *
 * Return a pointer to the bf_rmon_counter_array_t that is marked as
 * "not current".
 ************************************************************************/
static int pm_dm_get_stats_last_area(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port,
                                     bf_rmon_counter_array_t **ctr_array) {
  pm_port_t *pm_port_p = pm_dm_find(dev_id, dev_port);
  int last_area;

  if (pm_port_p == NULL) return -1;
  if (!pm_port_p->added) return -2;
  if (ctr_array == NULL) return -3;

  last_area = pm_port_p->stats.cur_area ? 0 : 1;
  *ctr_array = &pm_port_p->stats.ctr_array[last_area];
  return 0;
}

/************************************************************************
 * pm_dm_swap_stats_areas
 *
 * Swap the "current" and "last" stats areas.
 ************************************************************************/
static int pm_dm_swap_stats_areas(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  pm_port_t *pm_port_p = pm_dm_find(dev_id, dev_port);

  if (pm_port_p == NULL) return -1;
  if (!pm_port_p->added) return -2;

  pm_port_p->stats.cur_area = pm_port_p->stats.cur_area ? 0 : 1;
  return 0;
}

#if 0
/************************************************************************
* pm_dm_iter_over_up
*
* Iterate over the pm_dm_db and issue the callback "fn" for each
* pm_port_t structure for which up=true
************************************************************************/
static void pm_dm_iter_over_up(pm_dm_iter_cb fn) {
  bf_dev_id_t dev_id;
  int pipe, port;
  uint32_t num_subdev = 0;

  lld_sku_get_num_subdev(dev_id, &num_subdev, NULL);
  for (dev_id = 0; dev_id <= g_pm_max_asic_seen; dev_id++) {
    for (pipe = 0; pipe < (BF_SUBDEV_PIPE_COUNT *num_subdev); pipe++) {
      for (port = 0; port < BF_PIPE_PORT_COUNT; port++) {
        bf_dev_port_t dev_port = MAKE_DEV_PORT(pipe, port);
        pm_port_t *pm_port_p = pm_dm_find(dev_id, dev_port);
        if (pm_port_p == NULL) continue;  // should never continue
        if (!pm_port_p->added) continue;  // only added ports

        if (pm_port_p->up) {
          fn(dev_id, dev_port);
        }
      }
    }
  }
}
#endif

// Port Mgr application interface

/************************************************************************
 * pm_port_status_chg_cb
 *
 * Callback function that is called on a change in port status. This
 * function is registered at the time of pm_port_add.
 ************************************************************************/
bf_status_t pm_port_status_chg_cb(bf_dev_id_t dev_id,
                                  bf_dev_port_t dev_port,
                                  bool up,
                                  void *unused) {
  bool use_ints = false;
  bool ena_ints = false;
  bf_status_t sts;

  pm_dm_get_interrupt_based_link_monitoring(dev_id, &use_ints);
  sts = bf_port_mac_interrupt_ena_get(dev_id, dev_port, &ena_ints);
  if (sts != BF_SUCCESS) return sts;

  if (use_ints) {
    if (up && ena_ints) {
      // enable LF/RF ints only if NEAR_MAC/PIPE loopback is not enabled
      bf_port_local_fault_int_set(dev_id, dev_port, true);
      bf_port_remote_fault_int_set(dev_id, dev_port, true);
    } else {  // dn
      // disable LF/RF ints unconditionally
      bf_port_local_fault_int_set(dev_id, dev_port, false);
      bf_port_remote_fault_int_set(dev_id, dev_port, false);
    }
  }

  pm_dm_set_up(dev_id, dev_port, up);
  (void)unused;
  return BF_SUCCESS;
}

/************************************************************************
 * pm_port_add
 *
 * Initialize the specified port with the passed configuration to the
 * BF SDK.
 * Using BF_ APIs add the port and bind a status change callback fn.
 * Add the pm_dm_ structure.
 * Undo everything (above) on any failure
 ************************************************************************/
bf_status_t pm_port_add(bf_dev_id_t dev_id,
                        bf_dev_port_t dev_port,
                        bf_port_speeds_t port_speed,
                        uint32_t n_lanes,
                        bf_fec_types_t port_fec_type) {
  int err;
  bf_status_t bf_status;
  bool added, use_ints = false;
  int port_idx = DEV_PORT_TO_BIT_IDX(dev_port);

  if (port_idx >= BF_PORT_COUNT) return BF_INVALID_ARG;

  err = pm_dm_get_added(dev_id, dev_port, &added);
  if (err || added) return BF_INVALID_ARG;

  // Gain exclusive access to the port
  pm_dm_exclusive_access_start(dev_id, dev_port);

  // Add the port to the BF SDK
  bf_status = bf_port_add_with_lane_numb(
      dev_id, dev_port, port_speed, n_lanes, port_fec_type);
  if (bf_status != BF_SUCCESS) {
    PM_ERROR("bf_port_add : Error %xh : dev_id=%d : dev_port=%xh",
             bf_status,
             dev_id,
             dev_port);
    goto undo_add;
  }

  // add to data management layer structure
  err = pm_dm_port_add(dev_id, dev_port);
  if (err) {
    PM_ERROR("pm_dm_port_add: Error %xh : dev_id=%d : dev_port=%xh",
             err,
             dev_id,
             dev_port);
    goto undo_add;
  }

  port_fsm_mode[port_idx] = BF_PM_PORT_FSM_MODE_NONE;
  // By default use the usual serdes fsm
  skip_serdes_fsm[port_idx] = false;

  pm_dm_get_interrupt_based_link_monitoring(dev_id, &use_ints);
  if (use_ints) {
    // enable MAC ints (if desired)
    bf_port_mac_int_set(dev_id, dev_port, true);
  }
  /* set errored block threshold. Lower threshold
   * for FEC enabled ports */
  if (port_fec_type == BF_FEC_TYP_RS) {
    bf_port_errored_block_thresh_set(dev_id, dev_port, 5);
  } else {
    bf_port_errored_block_thresh_set(dev_id, dev_port, 50);
  }
  // Surrender exclusive access of the port
  pm_dm_exclusive_access_end(dev_id, dev_port);
  return BF_SUCCESS;

undo_add:
  pm_dm_port_rmv(dev_id, dev_port);  // ignore any errors
  // Surrender exclusive access of the port
  pm_dm_exclusive_access_end(dev_id, dev_port);
  // on any failure, roll back all of the above changes
  bf_port_bind_status_change_cb(dev_id, dev_port, NULL, NULL);
  bf_port_remove(dev_id, dev_port);  // ignore any errors

  return BF_INVALID_ARG;
}

/************************************************************************
 * pm_port_rmv
 *
 * Remove the specified port from the BF SDK
 * Using BF_ APIs remove the port and status change callback fn.
 * remove the pm_dm_ structure.
 ************************************************************************/
bf_status_t pm_port_rmv(bf_dev_id_t dev_id,
                        bf_dev_port_t dev_port,
                        bool switchd_remove_only) {
  bool oper_st_cb_pending = false;
  bf_status_t sts = BF_SUCCESS;
  int iter = 0;
  // Gain exclusive access to the port
  pm_dm_exclusive_access_start(dev_id, dev_port);

  bf_port_bind_status_change_cb(dev_id, dev_port, NULL, NULL);

  if (!switchd_remove_only) {
    // Wait for any oper state change callbacks to be issued before deleting
    // the port
    do {
      sts = bf_port_is_oper_state_callback_pending(
          dev_id, dev_port, &oper_st_cb_pending);
      if (sts != BF_SUCCESS) {
        PM_WARN(
            "bf_port_is_oper_state_callback_pending : Error %xh : dev_id=%d : "
            "dev_port=%xh",
            sts,
            dev_id,
            dev_port);
        break;
      }
      if (!oper_st_cb_pending) {
        break;
      }
      if (iter >= 30) {  // total max wait time is 300 msec
        PM_ERROR(
            "%s: timed out waiting for port status chg cbs to be issued for "
            "dev_id=%d : dev_port=%xh",
            __func__,
            dev_id,
            dev_port);
        break;
      }
      iter++;
      bf_sys_usleep(10000);  // Wait for 10 msec
    } while (oper_st_cb_pending);

    bf_port_remove(dev_id, dev_port);  // cant do much on errors ...
  }
  pm_dm_port_rmv(dev_id, dev_port);

  // Surrender exclusive access of the port
  pm_dm_exclusive_access_end(dev_id, dev_port);
  return sts;
}

/************************************************************************
 * pm_port_del
 *
 * Wrapper around the port_rmv function
 ************************************************************************/
bf_status_t pm_port_del(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  return pm_port_rmv(dev_id, dev_port, false);
}

/************************************************************************
 * pm_port_enable
 *
 * Enable the specified port in the BF SDK
 * Start the port bring-up FSM for the port.
 ************************************************************************/
bf_status_t pm_port_enable(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  bf_status_t bf_status;
  int err;
  bool added, enabled, in_cfg_replay = false;
  int qualifying_eye_ht_1e06 = 40;  // 140   // mV
  int qualifying_eye_ht_1e10 = 0;   // 80;   // mV
  int qualifying_eye_ht_1e12 = 0;   // 60;   // mV

  err = pm_dm_get_added(dev_id, dev_port, &added);
  if (err || !added) return BF_INVALID_ARG;

  err = pm_dm_get_en(dev_id, dev_port, &enabled);
  if (err != 0) return BF_INVALID_ARG;
  if (enabled) return BF_SUCCESS;

  // Gain exclusive access to the port
  pm_dm_exclusive_access_start(dev_id, dev_port);

  bf_port_eye_quality_set(dev_id,
                          dev_port,
                          qualifying_eye_ht_1e06,
                          qualifying_eye_ht_1e10,
                          qualifying_eye_ht_1e12);

  bf_status = bf_port_enable(dev_id, dev_port, true);
  if (bf_status == BF_SUCCESS) {
    pm_dm_get_in_cfg_replay(dev_id, &in_cfg_replay);
    if (!in_cfg_replay) {
      pm_dm_set_en(dev_id, dev_port, true);
      err = pm_port_fsm_init(dev_id, dev_port);  // start its bring-up fsm
      if (err) {
        bf_status = BF_INVALID_ARG;
        goto end;
      }
      bool use_ints = false;
      // Note: Interrupts are enabled after
      // link-up and disabled for link-down as part link up/down actions.
      pm_dm_get_interrupt_based_link_monitoring(dev_id, &use_ints);
      if (use_ints) {
        // explicit disable LF/RF ints
        bf_port_local_fault_int_set(dev_id, dev_port, false);
        bf_port_remote_fault_int_set(dev_id, dev_port, false);
      }
    }
    bf_status = BF_SUCCESS;
    goto end;
  }
  bf_status = BF_INVALID_ARG;
end:
  // Surrender exclusive access of the port
  pm_dm_exclusive_access_end(dev_id, dev_port);
  return bf_status;
}

/************************************************************************
 * pm_port_disable
 *
 * Disable the specified port in the BF SDK
 * Setting the administrative state ("en") to false will cause any
 * running FSM to terminate prior to running the next state.
 ************************************************************************/
bf_status_t pm_port_disable(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  bf_status_t bf_status = BF_SUCCESS;
  int err;
  bool added;
  bool in_cfg_replay = false;
  int port_idx = DEV_PORT_TO_BIT_IDX(dev_port);

  if (port_idx >= BF_PORT_COUNT) return BF_INVALID_ARG;

  err = pm_dm_get_added(dev_id, dev_port, &added);
  if (err || !added) return BF_INVALID_ARG;

  // Gain exclusive access to the port
  pm_dm_exclusive_access_start(dev_id, dev_port);
  bf_status = bf_port_enable(dev_id, dev_port, false);

  if (bf_status == BF_SUCCESS) {
    pm_dm_set_en(dev_id, dev_port, false);

    pm_dm_get_in_cfg_replay(dev_id, &in_cfg_replay);
    // Deinitialize the bring up FSM for this port only when not in cfg replay
    // We need this check else we might remove the port from the FSM and
    // the link status polling won't happen when warm init has ended if the
    // the port is actually up. consider the following scenario.
    // The user might have a port which is UP
    // Then during cfg replay, the user might replay port-add, port-enb,
    // port-dis, port-enb. Thus if this check is not there we would remove the
    // port from the FSM even though in this case the user wants the port to be
    // UP after warm init sequence (essentially no corrective action as the
    // final cfg that was replay matches the programmed cfg for the port)
    if (!in_cfg_replay) {
      // Deinitialize the bring-up FSM
      err = pm_port_fsm_deinit(dev_id, dev_port);
      if (err) {
        bf_status = BF_INVALID_ARG;
        goto end;
      }

      if (!bf_lld_dev_is_tof1(dev_id)) {
        bf_pm_fsm_t fsm;
        if (bf_lld_dev_is_tof2(dev_id)) {
          fsm = bf_pm_fsm_handle_get(port_fsm_mode[port_idx]);
        } else {
          fsm = bf_pm_fsm_tof3_handle_get(port_fsm_mode[port_idx]);
        }
        bf_pm_fsm_port_disable(dev_id, dev_port, fsm);
        // for fsm-display
        pm_dm_set_fsm(
            dev_id, dev_port, (bf_fsm_t)fsm, (bf_fsm_st)BF_PM_FSM_ST_DISABLED);
      }
      bool use_ints = false;

      pm_dm_get_interrupt_based_link_monitoring(dev_id, &use_ints);
      if (use_ints) {
        // Disable LF/RF ints
        bf_port_local_fault_int_set(dev_id, dev_port, false);
        bf_port_remote_fault_int_set(dev_id, dev_port, false);
      }
    }

    bf_status = BF_SUCCESS;
    goto end;
  }
  bf_status = BF_INVALID_ARG;
end:
  // Surrender exclusive access of the port
  pm_dm_exclusive_access_end(dev_id, dev_port);
  return bf_status;
}

bf_status_t pm_port_set_an(bf_dev_id_t dev_id,
                           bf_dev_port_t dev_port,
                           bool an) {
  bf_status_t bf_status;
  int err;
  bool added;

  err = pm_dm_get_added(dev_id, dev_port, &added);
  if (err || !added) return BF_INVALID_ARG;

  // Gain exclusive access to the port
  pm_dm_exclusive_access_start(dev_id, dev_port);

  err = pm_dm_set_an(dev_id, dev_port, an);
  if (err) {
    bf_status = BF_INVALID_ARG;
    goto end;
  }

  bf_status = BF_SUCCESS;

end:
  // Surrender exclusive access of the port
  pm_dm_exclusive_access_end(dev_id, dev_port);
  return bf_status;
}

/************************************************************************
 * pm_mac_stats_callback
 *
 * DMA callback function to process a mac stat DMA completion. This fn
 * is registered with the BF SDK in pm_port_cb_rmon_stats.
 *
 * "Current" and "previous" counter sets are maintained using a double-
 * buffering scheme (stats.cur_area). Any change in stats from the
 * previous counters are displayed.
 ************************************************************************/
void pm_mac_stats_callback(bf_dev_id_t dev_id,
                           bf_dev_port_t dev_port,
                           bf_rmon_counter_array_t *ctrs,
                           void *userdata) {
  bf_rmon_counter_t i;
  int hdr_shown = 0;
  bf_rmon_counter_array_t *curr_array;
  bf_rmon_counter_array_t *last_array;
  uint64_t *area, *cur_area;
  int err;
  (void)userdata;

  // get the "current" stats area in the pm_port struct
  err = pm_dm_get_stats_curr_area(dev_id, dev_port, &curr_array);
  if (err) {
    PM_ERROR("PM : Error : %d : getting curr stats area", err);
    return;
  }

  area = &ctrs->format.ctr_array[0];

  // copy stats to pm_port "current area"
  cur_area = (uint64_t *)curr_array;
  for (i = 0; i < BF_NUM_RMON_COUNTERS; i++) {
    cur_area[i] = area[i];
  }

  // get the previous stats area for comparison
  err = pm_dm_get_stats_last_area(dev_id, dev_port, &last_array);
  if (err) {
    PM_ERROR("PM : Error : %d : getting last stats area", err);
    return;
  }

  // iterate thru the RMON counters and display any changes
  // since the last pass.
  for (i = 0; i < BF_NUM_RMON_COUNTERS; i++) {
    uint64_t *cur = (uint64_t *)curr_array;
    uint64_t *last = (uint64_t *)last_array;

    if (cur[i] != last[i]) {
      char *stat_name;
      bf_status_t bf_status;

      bf_status = bf_port_rmon_counter_to_str(i, &stat_name);
      if (bf_status != BF_SUCCESS) {
        PM_ERROR("Error converting counter # %d to string", i);
      }

      if (!hdr_shown) {
        hdr_shown = 1;
        PM_DEBUG("PM : Rmon stats : dev_id=%d : dev_port=%x", dev_id, dev_port);
      }

      if (cur[i] < last[i]) {
        PM_DEBUG(
            "    : %16" PRIu64 " : *** stats reset *** %s", cur[i], stat_name);
      } else {
        PM_DEBUG("    : %16" PRIu64 " : %s", cur[i] - last[i], stat_name);
      }
    }
    last[i] = cur[i];
  }
  // swap stats areas
  pm_dm_swap_stats_areas(dev_id, dev_port);
}

/************************************************************************
 * bf-sys-timer
 *
 * bf-sys-timer is used to run two threads for port_mgr,
 *    a link state polling thread
 *    a mac stat polling thread
 *
 * Thread polling frequency is specified using the defines below
 ************************************************************************/
#define USE_BF_SYS_TIMER

/************************************************************************
 * pm_init
 *
 * Called when a device is added. This fn init mutexes and other device
 * specific elements.
 ************************************************************************/
void pm_init(bf_dev_id_t dev_id) {
  // init mutexes and device-specific info
  pm_dm_set_interrupt_based_link_monitoring(dev_id, false);
  //  pm_dm_set_in_cfg_replay(dev_id, false);
  pm_dm_lock_init(dev_id);
}

/************************************************************************
 * pm_terminate
 *
 * Called when a device is deleted. This fn removes locks and allocated
 * mem, if any.
 *
 ************************************************************************/
void pm_terminate(bf_dev_id_t dev_id) {
  // remove locks allocated for this device
  pm_dm_lock_deinit(dev_id);
}

int pm_port_fsm_stop_set(bf_dev_id_t dev_id,
                         bf_dev_port_t dev_port,
                         bool stop) {
  int port_idx;

  if (!DEV_PORT_VALIDATE(dev_port)) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;

  port_idx = DEV_PORT_TO_BIT_IDX(dev_port);

  if (port_idx >= BF_PORT_COUNT) {
    return -1;
  }

  port_fsm_stop[dev_id][port_idx] = stop;

  if (!stop) {
    // stop and go, start with init-state
    pm_port_fsm_init(dev_id, dev_port);
  }

  return 0;
}

/*************************************************************************
 * pm_port_fsm_run
 *
 * Tasklet scheduler callback to run one of the two port fsm's on a port.
 *************************************************************************/
uint32_t pm_port_fsm_run(void *context) {
  uint32_t nxt_wait_ms;
  int err;
  bool en = false;
  bf_dev_id_t dev_id;
  bf_dev_port_t dev_port;
  bf_status_t bf_status;
  bf_fsm_t fsm;
  bf_fsm_st fsm_st_current;
  bf_fsm_st fsm_st_next;
  pm_dm_handle_t handle = (pm_dm_handle_t)context;
  uint32_t ret;
  int port_idx;

  err = pm_dm_id_from_handle(handle, &dev_id, &dev_port);
  if (err) {
    ret = 0xffffffff;
    PM_ERROR("Unable to find PM handle %x", ret);
    return ret;
  }

  port_idx = DEV_PORT_TO_BIT_IDX(dev_port);
  if (port_idx >= BF_PORT_COUNT) {
    ret = 0xffffffff;
    PM_ERROR("%d:%3d: Invalid Port id", dev_id, dev_port);
    return ret;
  }

  if (port_fsm_stop[dev_id][port_idx]) {
    return 0;
  }

  // Gain exclusive access to the port structure
  pm_dm_exclusive_access_start(dev_id, dev_port);

  err = pm_dm_get_en(dev_id, dev_port, &en);
  if (err || !en) {
    ret = BF_FSM_TERMINATE;
    goto end;
  }
  /* This check to prevent invoking fsm handler through stale TCB
     for port which is removed and added by the user again with different config
   */
  if (pm_is_current_tasklet_valid(context) == false) {
    ret = BF_FSM_TERMINATE;
    goto end;
  }
  err = pm_dm_get_fsm(dev_id, dev_port, &fsm, &fsm_st_current);
  if (err) {
    ret = BF_FSM_TERMINATE;
    PM_ERROR("%d:%3d: Unable to get fsm state err:%d", dev_id, dev_port, err);
    goto end;
  }

  if (!bf_lld_dev_is_tof1(dev_id)) {  // FIXME for tof2
    bf_status = bf_pm_fsm_run(dev_id,
                              dev_port,
                              (bf_pm_fsm_t)fsm,
                              (bf_pm_fsm_st)fsm_st_current,
                              // returned values
                              (bf_pm_fsm_st *)&fsm_st_next,
                              &nxt_wait_ms);
  } else {
    bf_status = bf_fsm_run(dev_id,
                           dev_port,
                           fsm,
                           fsm_st_current,
                           // returned values
                           &fsm_st_next,
                           &nxt_wait_ms);
  }
  if (bf_status != BF_SUCCESS) {
    ret = BF_FSM_TERMINATE;
    PM_ERROR("%d:%3d: FSM run failed", dev_id, dev_port);
    goto end;
  } else if (nxt_wait_ms == BF_FSM_TERMINATE) {
    ret = BF_FSM_TERMINATE;
    PM_ERROR("%d:%3d: FSM nxt st run failed", dev_id, dev_port);
    goto end;
  } else {
    pm_dm_set_fsm(dev_id, dev_port, fsm, fsm_st_next);
  }
#ifdef USE_BF_SYSLIB_TIMERS
  ret = nxt_wait_ms;
#else
  ret = nxt_wait_ms * 1000;  // tasklet sched time is in us
#endif  // USE_BF_SYSLIB_TIMERS

end:
  // Surrender exclusive access of the port structure
  pm_dm_exclusive_access_end(dev_id, dev_port);

  // Check if we need to issue any oper state chg callbacks and issue them
  if (ret != BF_FSM_TERMINATE) {
    bf_status =
        bf_port_oper_state_callbacks_issue_with_intr_check(dev_id, dev_port);
    if (bf_status != BF_SUCCESS) {
      PM_ERROR("%d:%3d: Unable to issue port sts chg callback : %s (%d)",
               dev_id,
               dev_port,
               bf_err_str(bf_status),
               bf_status);
    }
  }

  return ret;
}

/**************************************************************************
 * pm_port_fsm_init
 *
 * Add a port FSM as a scheduled tasklet
 **************************************************************************/
int pm_port_fsm_init(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  bool an;
  int err;
  bf_fsm_t fsm;
  pm_dm_handle_t handle;
  int port_idx = DEV_PORT_TO_BIT_IDX(dev_port);

  if (port_idx >= BF_PORT_COUNT) return -1;

  if (!bf_lld_dev_is_tof1(dev_id)) {
    bf_pm_fsm_t bf_pm_fsm;
    if (bf_lld_dev_is_tof2(dev_id)) {
      bf_pm_fsm = bf_pm_fsm_handle_get(port_fsm_mode[port_idx]);
    } else {
      bf_pm_fsm = bf_pm_fsm_tof3_handle_get(port_fsm_mode[port_idx]);
    }
    fsm = (bf_fsm_t)bf_pm_fsm;
    pm_dm_set_fsm(dev_id, dev_port, fsm, BF_FSM_ST_IDLE);
    err = pm_dm_handle_from_id(dev_id, dev_port, &handle);
    if (err) return -1;
    pm_tasklet_new(pm_port_fsm_run, (void *)handle, HI_PRI);
    (void)bf_pm_fsm;
    return 0;
  }

  err = pm_dm_get_an(dev_id, dev_port, &an);
  if (err) return -1;

  if (skip_serdes_fsm[port_idx]) {
    fsm = bf_get_non_serdes_fsm();
  } else {
    fsm = bf_get_default_fsm(dev_id, an);
  }

  // Overwrite default ones.
  if (is_fsm_for_tx_mode[port_idx]) {
    fsm = bf_get_fsm_for_tx_mode();
  }

  if (fsm == NULL) return -2;

  // set the fsm and state for bf_fsm_run calls
  pm_dm_set_fsm(dev_id, dev_port, fsm, BF_FSM_ST_IDLE);

  // now add the scheduler (or timer)
  err = pm_dm_handle_from_id(dev_id, dev_port, &handle);
  if (err) return -1;

  pm_tasklet_new(pm_port_fsm_run, (void *)handle, HI_PRI);
  return 0;
}

/**************************************************************************
 * pm_port_fsm_deinit
 *
 * Remove a port FSM tasklet from the timer wheel
 **************************************************************************/
int pm_port_fsm_deinit(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  int err;
  pm_dm_handle_t handle;

  err = pm_dm_handle_from_id(dev_id, dev_port, &handle);
  if (err) return -1;

  // remove the port tasklet from the scheduler (or timer)
  pm_tasklet_rmv((void *)handle);
  return 0;
}

#ifdef USE_BF_SYSLIB_TIMERS

/**************************************************************************
 * pm_port_fsm_tmr_cb
 *
 * Callback for bf-syslib timer to run the port fsm. "context" is the
 * port handle.
 **************************************************************************/
void pm_port_fsm_tmr_cb(struct bf_sys_timer_s *timer, void *context) {
  bf_dev_id_t dev_id;
  bf_dev_port_t dev_port;
  bf_sys_timer_t *tmr;
  int err;
  pm_dm_handle_t handle = (pm_dm_handle_t)context;
  uint32_t next_time = pm_port_fsm_run(context);

  // get dev_id and dev_port so we can get the fsm timer
  err = pm_dm_id_from_handle(handle, &dev_id, &dev_port);
  if (err) {
    PM_ERROR("Error: %x : getting id from handle <%p>", err, handle);
    return;
  }

  // get ptr to port fsm timer
  err = pm_dm_get_fsm_tmr(dev_id, dev_port, &tmr);
  if (err) {
    PM_ERROR("Error: %x : getting fsm from %d : %x", err, dev_id, dev_port);
    return;
  }

  // stop and delete any existing timer
  bf_sys_timer_del(tmr);  // ignore errors

  if (next_time != 0xffffffff) {
    // start next timer
    pm_start_timer(context, next_time / 1000);
  }
  (void)timer;
}

/**************************************************************************
 * pm_start_timer
 *
 * Start a port FSM timer for a port
 **************************************************************************/
static void pm_start_timer(pm_dm_handle_t handle, uint32_t ms) {
  bf_dev_id_t dev_id;
  bf_dev_port_t dev_port;
  bf_sys_timer_t *tmr;
  int err;

  // get dev_id and dev_port so we can get the fsm timer
  err = pm_dm_id_from_handle(handle, &dev_id, &dev_port);
  if (err) {
    PM_ERROR("Error: %x : getting id from handle <%p>", err, handle);
    return;
  }
  // get ptr to port fsm timer
  err = pm_dm_get_fsm_tmr(dev_id, dev_port, &tmr);
  if (err) {
    PM_ERROR("Error: %x : getting fsm from %d : %x", err, dev_id, dev_port);
    return;
  }

  // err = bf_sys_timer_create( tmr, ms, ms, pm_port_fsm_tmr_cb, handle );
  // err = bf_sys_timer_create( tmr, 0, ms, pm_port_fsm_tmr_cb, handle );
  err = bf_sys_timer_create(tmr, 0, 0, pm_port_fsm_tmr_cb, handle);
  if (err) {
    PM_ERROR("Error: %xh : creating fsm timer", err);
  }
  err = bf_sys_timer_start(tmr);
  if (err) {
    PM_ERROR("Error: %xh : starting link-poll timer", err);
  }
  PM_DEBUG("TMR: start : %d : %x : %dms", dev_id, dev_port, ms);
}
#endif  // USE_BF_SYSLIB_TIMERS

/* poll control
 *
 * These functions return the state of global variables that enable
 * or disable polling. The variables can be set/cleared via pm ucli
 * debug commands.
 */

/*********************************************************************
 * Sample auto-negotiation set-up
 *
 * Currently only used on the eval board
 *********************************************************************/
void pm_port_set_autoneg_parms(bf_dev_id_t dev_id,
                               bf_dev_port_t dev_port,
                               bf_an_adv_speeds_t p_speed,
                               bf_an_fec_t p_fec,
                               bf_pause_cfg_t tx_pause,
                               bf_pause_cfg_t rx_pause) {
  uint32_t num_pgs = 1;  // always a base pg
  uint64_t pgs[3] = {0};
  bf_status_t bf_status;
  (void)tx_pause;
  (void)rx_pause;
  bf_status = bf_an_advertisement_set(p_speed,
                                      0 /*(BF_ADV_PAUSE_RX | BF_ADV_PAUSE_TX)*/,
                                      p_fec,
                                      ETHERNET_CONSORTIUM_CID,
                                      sizeof(pgs) / sizeof(pgs[0]),
                                      &num_pgs,
                                      pgs);
  if (bf_status != BF_SUCCESS) {
    PM_ERROR("PM: Error: %d : from bf_an_advertisement_set", bf_status);
  }
  bf_port_autoneg_advert_set(dev_id, dev_port, num_pgs, pgs);
}

/***************************************************************************
 * pm_dev_add_serdes_init
 *
 * Serdes initializations to perform (one-time) when a device is added.
 ***************************************************************************/
void pm_dev_add_serdes_init(bf_dev_id_t dev_id) {
#ifdef INCLUDE_SERDES_PKG
  bf_status_t bf_status;
  uint32_t pipe, port, num_pipes;

  /* Figure out how many pipelines present on this device */
  lld_sku_get_num_active_pipes(dev_id, &num_pipes);

  if (!bf_lld_dev_is_tof1(dev_id)) {  // FIXME for tof2
    /* Only init TOF1 serdes here (for now) */
    return;
  }
  /* Set refclk to ETH_REFCLK for all of device's ports across all pipelines
     present */
  for (pipe = 0; pipe < num_pipes; pipe++) {
    for (port = 0; port < BF_PIPE_PORT_COUNT; port += 1) {
      bool has_mac = false;
      bf_dev_port_t dev_port = MAKE_DEV_PORT(pipe, port);

      bf_status = bf_port_has_mac(dev_id, dev_port, &has_mac);

      /* If port is associated with a MAC then it needs refclk set.
       * Default is to use BF_SDS_TX_PLL_ETH_REFCLK
       */
      if ((bf_status == BF_SUCCESS) && has_mac) {
        bf_status = bf_serdes_tx_pll_clksel_set(
            dev_id, dev_port, 0 /*lane*/, BF_SDS_TX_PLL_ETH_REFCLK);
        if (bf_status != BF_SUCCESS) {
          // PM_DEBUG("WARN: %d : %xh : Failed to set Tx Pll clksel: rc=%d",
          //       dev_id,
          //       dev_port,
          //       bf_status);
        }
      }
    }
  }

#else
  (void)dev_id;
#endif  // INCLUDE_SERDES_PKG
}

/***************************************************************************
 * pm_max_ports_get
 *
 * Get the maximum ports on the target
 ***************************************************************************/
bf_status_t pm_max_ports_get(bf_dev_id_t dev_id, uint32_t *num_ports) {
  uint32_t num_pipes;
  int max_fp, min_fp, max_cpu, min_cpu;
  if (num_ports == NULL) {
    return BF_INVALID_ARG;
  }
  max_fp = lld_get_max_fp_port(dev_id);
  min_fp = lld_get_min_fp_port(dev_id);
  max_cpu = lld_get_max_cpu_port(dev_id);
  min_cpu = lld_get_min_cpu_port(dev_id);
  if ((max_fp == -1) || (min_fp == -1) || (max_cpu == -1) || (min_cpu == -1)) {
    return BF_INVALID_ARG;
  }
  lld_sku_get_num_active_pipes(dev_id, &num_pipes);
  *num_ports = ((num_pipes * (max_fp - min_fp + 1)) + (max_cpu - min_cpu + 1));
  return BF_SUCCESS;
}

/***************************************************************************
 * pm_fp_idx_to_dev_port_map
 *
 * Get the dev port corresponding to the given front panel port
 ***************************************************************************/
bf_status_t pm_fp_idx_to_dev_port_map(bf_dev_id_t dev_id,
                                      uint32_t fp_idx,
                                      bf_dev_port_t *dev_port) {
  uint32_t pipe, port;
  uint32_t num_ports = 0;
  int max_cpu, min_cpu, max_fp, min_fp;
  if (pm_max_ports_get(dev_id, &num_ports) != BF_SUCCESS) {
    return BF_INVALID_ARG;
  }
  if (fp_idx >= num_ports) {
    return BF_INVALID_ARG;
  }

  if (dev_port == NULL) {
    return BF_INVALID_ARG;
  }
  max_fp = lld_get_max_fp_port(dev_id);
  min_fp = lld_get_min_fp_port(dev_id);
  max_cpu = lld_get_max_cpu_port(dev_id);
  min_cpu = lld_get_min_cpu_port(dev_id);
  if ((max_fp == -1) || (min_fp == -1) || (max_cpu == -1) || (min_cpu == -1)) {
    return BF_INVALID_ARG;
  }
  if (fp_idx >= (num_ports - (max_cpu - min_cpu + 1))) {
    // The last 4 front panel ports are the CPU ports
    pipe = 0;
    port = min_cpu + (fp_idx % min_cpu);
  } else {
    pipe = fp_idx / (max_fp - min_fp + 1);
    port = fp_idx % (max_fp - min_fp + 1) + min_fp;
  }

  *dev_port = MAKE_DEV_PORT(pipe, port);

  return BF_SUCCESS;
}

/***************************************************************************
 * pm_recirc_port_range_get
 *
 * Get the range of the recirculation ports on the target
 ***************************************************************************/
bf_status_t pm_recirc_port_range_get(bf_dev_id_t dev_id,
                                     uint32_t *start_recirc_port,
                                     uint32_t *end_recirc_port) {
  uint32_t num_pipes;
  bf_status_t sts;

  if (!start_recirc_port || !end_recirc_port) {
    return BF_INVALID_ARG;
  }

  sts = pm_max_ports_get(dev_id, start_recirc_port);
  if (sts != BF_SUCCESS) {
    return sts;
  }

  /* TOF: The last 8 ports (64-71) in all the pipes support recirculation.
     However,
     we enable/disbale recirc on a port group as a whole and since port group
     16 has a CPU port we only expose the ports in port group 17.
     TOF2: first 8 ports in every pipe support recirculation. Here expose all of
     them.
     note here, port 2-5 can be configured to cpu ports.*/
  lld_sku_get_num_active_pipes(dev_id, &num_pipes);

  *end_recirc_port =
      (*start_recirc_port) + (num_pipes * lld_get_chnls_per_mac(dev_id)) - 1;

  return BF_SUCCESS;
}

/***************************************************************************
 * pm_recirc_port_to_dev_port_map
 *
 * Get the dev port corresponding to the given recirculation port
 ***************************************************************************/
bf_status_t pm_recirc_port_to_dev_port_map(bf_dev_id_t dev_id,
                                           uint32_t recirc_port,
                                           bf_dev_port_t *dev_port) {
  bf_status_t sts;
  uint32_t start_recirc_port, end_recirc_port;
  uint32_t pipe, port;
  uint8_t port_group = lld_get_chnls_per_mac(dev_id);

  if (!dev_port) {
    return BF_INVALID_ARG;
  }

  sts = pm_recirc_port_range_get(dev_id, &start_recirc_port, &end_recirc_port);
  if (sts != BF_SUCCESS) {
    return sts;
  }

  if (recirc_port < start_recirc_port || recirc_port > end_recirc_port) {
    return BF_INVALID_ARG;
  }

  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));
  switch (dev_family) {
    case BF_DEV_FAMILY_TOFINO2:
    case BF_DEV_FAMILY_TOFINO3:
      /* Tof2 uses ports 0-7 in each pipe as recirc, we expose 0-3. */
      pipe = (recirc_port - start_recirc_port) / port_group;
      port = (recirc_port - start_recirc_port) % port_group;
      *dev_port = MAKE_DEV_PORT(pipe, port);
      break;
    case BF_DEV_FAMILY_TOFINO:
      pipe = (recirc_port - start_recirc_port) / port_group;
      port = (lld_get_max_fp_port(dev_id) + 1 + port_group) +
             ((recirc_port - start_recirc_port) % port_group);
      *dev_port = MAKE_DEV_PORT(pipe, port);
      break;
    default:
      return BF_INVALID_ARG;
  }
  return BF_SUCCESS;
}

/***************************************************************************
 * pm_port_skip_serdes_fsm_set
 *
 * Set a flag to indicate which FSM is to be used to bring up the port
 ***************************************************************************/
bf_status_t pm_port_skip_serdes_fsm_set(bf_dev_port_t dev_port, bool val) {
  int port_idx;

  if (!DEV_PORT_VALIDATE(dev_port)) return BF_INVALID_ARG;

  port_idx = DEV_PORT_TO_BIT_IDX(dev_port);

  if (port_idx >= BF_PORT_COUNT) return BF_INVALID_ARG;

  skip_serdes_fsm[port_idx] = val;

  return BF_SUCCESS;
}

/***************************************************************************
 * pm_port_fsm_for_tx_mode_set
 *
 * Set a flag to indicate which FSM is to be used to bring up the port
 ***************************************************************************/
bf_status_t pm_port_fsm_for_tx_mode_set(bf_dev_port_t dev_port, bool val) {
  int port_idx;

  if (!DEV_PORT_VALIDATE(dev_port)) return BF_INVALID_ARG;

  port_idx = DEV_PORT_TO_BIT_IDX(dev_port);

  if (port_idx >= BF_PORT_COUNT) return BF_INVALID_ARG;

  is_fsm_for_tx_mode[port_idx] = val;

  return BF_SUCCESS;
}

/***************************************************************************
 * pm_port_cfg_replay_flag_set
 *
 * Set a flag to indicate if the port enable call is during cfg replay or
 * not. We don't want to start the FSM if we are in config replay
 ***************************************************************************/
bf_status_t pm_port_cfg_replay_flag_set(bf_dev_id_t dev_id, bool val) {
  pm_dm_set_in_cfg_replay(dev_id, val);
  return BF_SUCCESS;
}

/***************************************************************************
 * pm_port_interrupt_based_link_monitoring_set
 *
 * Set device-wide flag indicating whether or not interrupts should be
 * used for link status monitoring.
 *
 * Note: This API is intended to be called only once at initialization of a
 * device. It is not intended to switch back and forth between modes.
 *
 ***************************************************************************/
bf_status_t pm_port_interrupt_based_link_monitoring_set(bf_dev_id_t dev_id,
                                                        bool val) {
  pm_dm_set_interrupt_based_link_monitoring(dev_id, val);
  return BF_SUCCESS;
}

/***************************************************************************
 * pm_port_interrupt_based_link_monitoring_get
 *
 * Get device-wide flag indicating whether or not interrupts are used for
 * link status monitoring.
 *
 ***************************************************************************/
bf_status_t pm_port_interrupt_based_link_monitoring_get(bf_dev_id_t dev_id,
                                                        bool *val) {
  return pm_dm_get_interrupt_based_link_monitoring(dev_id, val);
}

/**************************************************************************
 * pm_port_fsm_set_down_event_state
 *
 * Add a port FSM as a scheduled tasklet
 **************************************************************************/
int pm_port_fsm_set_down_event_state(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port) {
  bool an = 0;
  int err;
  bf_fsm_t fsm;
  pm_dm_handle_t handle;
  int port_idx = DEV_PORT_TO_BIT_IDX(dev_port);

  if (port_idx >= BF_PORT_COUNT) return -1;

  // Mainly for loopback. Today we donot poll link-status
  // when in loopback-mode, hence simply return from here.
  if (skip_serdes_fsm[port_idx]) {
    // fsm = bf_get_non_serdes_fsm(); // for later
    return 0;
  }

  if (is_fsm_for_tx_mode[port_idx]) {
    fsm = bf_get_fsm_for_tx_mode();
    pm_dm_set_fsm(dev_id, dev_port, fsm, BF_FSM_ST_WAIT_DWN_EVNT);
  } else {
    err = pm_dm_get_an(dev_id, dev_port, &an);
    if (err) return -1;
    fsm = bf_get_default_fsm(dev_id, an);
    // set the fsm and state for bf_fsm_run calls
    if (an) {
      pm_dm_set_fsm(dev_id, dev_port, fsm, BF_AN_FSM_ST_WAIT_DWN_EVNT);
    } else {
      pm_dm_set_fsm(dev_id, dev_port, fsm, BF_FSM_ST_WAIT_DWN_EVNT);
    }
  }

  // now add the scheduler (or timer)
  err = pm_dm_handle_from_id(dev_id, dev_port, &handle);
  if (err) return -1;

  pm_tasklet_new(pm_port_fsm_run, (void *)handle, HI_PRI);
  return 0;
}

/**************************************************************************
 * pm_port_fsm_init_in_up_state
 *
 * Add a port FSM as a scheduled tasklet and initialize the FSM in UP state
 *
 * Note: FSM state associated to link UP is BF_FSM_ST_WAIT_DWN_EVNT or
 * BF_AN_FSM_ST_WAIT_DWN_EVNT, for Tofino 1 and BF_PM_FSM_ST_LINK_UP for
 * Tofino 2.
 **************************************************************************/
int pm_port_fsm_init_in_up_state(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  int err;
  bf_fsm_t fsm;
  pm_dm_handle_t handle;
  int port_idx = DEV_PORT_TO_BIT_IDX(dev_port);

  if (port_idx >= BF_PORT_COUNT) return -1;

  if (!bf_lld_dev_is_tof1(dev_id)) {
    // Device is Tofino 2: initialize the FSM in BF_PM_FSM_ST_LINK_UP state
    bf_pm_fsm_t bf_pm_fsm;
    if (bf_lld_dev_is_tof2(dev_id)) {
      bf_pm_fsm = bf_pm_fsm_handle_get(port_fsm_mode[port_idx]);
    } else {
      bf_pm_fsm = bf_pm_fsm_tof3_handle_get(port_fsm_mode[port_idx]);
    }
    fsm = (bf_fsm_t)bf_pm_fsm;
    pm_dm_set_fsm(dev_id, dev_port, fsm, (bf_fsm_st)BF_PM_FSM_ST_LINK_UP);
    err = pm_dm_handle_from_id(dev_id, dev_port, &handle);
    if (err) return -1;
    pm_tasklet_new(pm_port_fsm_run, (void *)handle, HI_PRI);
    (void)bf_pm_fsm;
    return 0;
  }

  // Device is Tofino 1, initialize the FSM in BF_FSM_ST_WAIT_DWN_EVNT or
  // BF_AN_FSM_ST_WAIT_DWN_EVNT state
  return pm_port_fsm_set_down_event_state(dev_id, dev_port);
}

/***************************************************************************
 * pm_port_fsm_mode_set
 *
 * Set a flag to indicate which FSM is to be used to bring up the port
 ***************************************************************************/
bf_status_t pm_port_fsm_mode_set(bf_dev_port_t dev_port,
                                 bf_pm_port_fsm_mode_t fsm_mode) {
  int port_idx;

  if (!DEV_PORT_VALIDATE(dev_port)) return BF_INVALID_ARG;

  port_idx = DEV_PORT_TO_BIT_IDX(dev_port);

  if (port_idx >= BF_PORT_COUNT) return BF_INVALID_ARG;

  port_fsm_mode[port_idx] = fsm_mode;

  return BF_SUCCESS;
}

/***************************************************************************
 * pm_port_fsm_mode_get
 *
 * Get which FSM used to bring up the port
 ***************************************************************************/
bf_pm_port_fsm_mode_t pm_port_fsm_mode_get(bf_dev_port_t dev_port) {
  int port_idx = DEV_PORT_TO_BIT_IDX(dev_port);

  return port_idx < BF_PORT_COUNT ? port_fsm_mode[port_idx]
                                  : BF_PM_PORT_FSM_MODE_NONE;
}

/***************************************************************************
 * pm_port_display_info_get
 *
 ***************************************************************************/
void pm_port_fsm_display_info_get(bf_dev_id_t dev_id,
                                  bf_dev_port_t dev_port,
                                  char **fsm_str,
                                  char **fsm_st_str) {
  bf_pm_fsm_t fsm;
  bf_pm_fsm_st st;
  int err;

  err = pm_dm_get_fsm(
      dev_id, dev_port, (bf_fsm_state_desc_t **)&fsm, (bf_fsm_st *)&st);
  if (err) {
    *fsm_str = "<error getting fsm>";
    *fsm_st_str = "<error getting fsm st>";
    return;
  }
  bf_pm_fsm_display_get(fsm, st, fsm_str, fsm_st_str);
}

bf_status_t pm_port_tof2_prbs_stats_get(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        bf_port_sds_prbs_stats_t *stats) {
  uint32_t integration_ms = 10;  // default 10ms
  int ln, num_lanes = bf_pm_fsm_num_lanes_get(dev_id, dev_port);
  uint32_t err_cnt = 0;
  bool added = false;
  int err;

  err = pm_dm_get_added(dev_id, dev_port, &added);
  if (err || !added || !stats) return BF_INVALID_ARG;

  // Gain exclusive access to the port
  pm_dm_exclusive_access_start(dev_id, dev_port);

  for (ln = 0; ln < num_lanes; ln++) {
    if (integration_ms != 0) {
      bf_tof2_serdes_prbs_rst_set(dev_id, dev_port, ln);
    }
    bf_sys_usleep(integration_ms * 1000);
    bf_tof2_serdes_rx_prbs_err_get(0, dev_port, ln, &err_cnt);
    stats->prbs_stats.tof2_channel[ln].errors = err_cnt;

    if (integration_ms == 0) {
      bf_tof2_serdes_prbs_rst_set(dev_id, dev_port, ln);
    }
  }

  pm_dm_exclusive_access_end(dev_id, dev_port);
  return BF_SUCCESS;
}

bf_status_t pm_port_tof2_eye_val_get(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port,
                                     bf_port_eye_val_t *eye) {
  bool added = false;
  int err;
  int num_lanes, ln;

  err = pm_dm_get_added(dev_id, dev_port, &added);
  if (err || !added || !eye) return BF_INVALID_ARG;

  // Gain exclusive access to the port
  pm_dm_exclusive_access_start(dev_id, dev_port);
  bf_port_num_lanes_get(0, dev_port, &num_lanes);

  for (ln = 0; ln < num_lanes; ln++) {
    bf_tof2_serdes_eye_get(dev_id,
                           dev_port,
                           ln,
                           &eye->eye_val.tof2_channel[ln].eyes_0,
                           &eye->eye_val.tof2_channel[ln].eyes_1,
                           &eye->eye_val.tof2_channel[ln].eyes_2);
  }

  pm_dm_exclusive_access_end(dev_id, dev_port);
  return BF_SUCCESS;
}

bf_status_t pm_port_tof2_ber_get(bf_dev_id_t dev_id,
                                 bf_dev_port_t dev_port,
                                 bf_port_ber_t *ber) {
  int integration_time = 1000;
  bool added = false;
  int err;
  int num_lanes;
  bf_status_t rc;
  bf_serdes_encoding_mode_t enc_mode;
  bf_port_speed_t speed;
  bf_fec_type_t fec;
  umac4_rs_fec_ln_ctr_t fec_ln_ctrs;
  uint32_t umac;
  uint32_t ch_ln;
  bool is_cpu_port;
  uint32_t ctr, n_ctrs, first_ctr, end_ctr;
  uint64_t *ctrs = (uint64_t *)&fec_ln_ctrs;
  size_t ctrs_sz = sizeof(fec_ln_ctrs) / sizeof(uint64_t);
  uint64_t bps;
  uint32_t integration_ms = 1000;

  err = pm_dm_get_added(dev_id, dev_port, &added);
  if (err || !added || !ber) return BF_INVALID_ARG;

  // Gain exclusive access to the port
  pm_dm_exclusive_access_start(dev_id, dev_port);

  bf_port_fec_get(dev_id, dev_port, &fec);
  if (fec != BF_FEC_TYP_RS) {
    PM_ERROR("BER supported only with RS-FEC %d:%d\n", dev_id, dev_port);
    rc = BF_INVALID_ARG;
    goto err_exit;
  }

  bf_port_speed_get(dev_id, dev_port, &speed);
  bf_port_num_lanes_get(0, dev_port, &num_lanes);
  bf_serdes_encoding_mode_get(speed, num_lanes, &enc_mode);

  rc = port_mgr_tof2_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch_ln, &is_cpu_port);
  if (rc != BF_SUCCESS) {
    PM_ERROR("unable to map dev_port (%d) to umac\n", dev_port);
    goto err_exit;
  }

  if (speed == BF_SPEED_400G) {
    n_ctrs = 16;
    first_ctr = 0;
    bps = 400ul * 1000ul * 1000ul * 1000ul;
  } else if (speed == BF_SPEED_200G) {
    n_ctrs = 8;
    first_ctr = (dev_port & 0x7) * 2;
    bps = 200ul * 1000ul * 1000ul * 1000ul;
  } else if (speed == BF_SPEED_100G) {
    n_ctrs = 4;
    first_ctr = (dev_port & 0x7) * 2;
    bps = 100ul * 1000ul * 1000ul * 1000ul;
  } else if (speed == BF_SPEED_50G) {
    n_ctrs = 2;
    first_ctr = (dev_port & 0x7) * 2;
    bps = 50ul * 1000ul * 1000ul * 1000ul;
  } else {
    PM_ERROR("BER Not supported for this speed yet %d\n", dev_port);
    rc = BF_INVALID_ARG;
    goto err_exit;
  }
  //  n_ctrs = 2;  // 2 FEC counters, one for each PCS lane (=2 per serdes lane)
  //  first_ctr += (ln * 2);

  bps = (bps * integration_ms) / 1000;

  end_ctr = (first_ctr + n_ctrs);
  if (end_ctr > ctrs_sz) {
    PM_ERROR("Attempt to access array of size %zu with index %d\n",
             ctrs_sz,
             (int)end_ctr);
    rc = BF_UNEXPECTED;
    goto err_exit;
  }

  for (ctr = 0; ctr < 16; ctr++) {
    ber->ber.tof2.sym_err_ctrs[ctr] = ctrs[ctr] = 0;
  }

  // timed collections of counters
  umac4_ctrs_rs_fec_ln_get(dev_id, umac, &fec_ln_ctrs);  // read first to clear
  bf_sys_usleep(integration_time * 1000);
  umac4_ctrs_rs_fec_ln_get(dev_id, umac, &fec_ln_ctrs);  // now for real

  uint64_t tot_s_errs = 0ul;
  for (ctr = first_ctr; ctr < end_ctr; ctr++) {
    ber->ber.tof2.sym_err_ctrs[ctr] = ctrs[ctr];
    tot_s_errs += ctrs[ctr];
  }
  ber->ber.tof2.ber_aggr = (float)(tot_s_errs * 1) / (float)(bps);

  uint64_t s_errs;
  for (ctr = first_ctr; ctr < end_ctr; ctr++) {
    s_errs = ctrs[ctr];
    ber->ber.tof2.ber_per_lane[ctr] =
        (float)(s_errs * 1) /
        (float)(bps / ((uint64_t)n_ctrs));  // 10b symbols, ea lane
    // carries 1/n_ctrs of the
    // bits
  }

err_exit:
  pm_dm_exclusive_access_end(dev_id, dev_port);
  return rc;
}
