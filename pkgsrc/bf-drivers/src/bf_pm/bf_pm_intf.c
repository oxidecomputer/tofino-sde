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


// Module includes
#include <tofino/bf_pal/bf_pal_types.h>
#include <tofino/bf_pal/bf_pal_pltfm_porting.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include <bf_pm/bf_pm_intf.h>
#include <bf_types/bf_types.h>
#include <target-sys/bf_sal/bf_sys_sem.h>
#include <target-sys/bf_sal/bf_sys_timer.h>
#include <port_mgr/bf_serdes_if.h>
#include <port_mgr/bf_port_if.h>
#include <port_mgr/bf_fsm_hdlrs.h>
#include <port_mgr/port_mgr_intf.h>
#include <port_mgr/bf_tof2_serdes_if.h>
#include <port_mgr/port_mgr_tof3/aw_lane_cfg.h>
#include <port_mgr/bf_tof3_serdes_if.h>
#include <port_mgr/bf_fsm_if.h>
#include <dvm/bf_drv_profile.h>
#include <dvm/bf_drv_intf.h>
#include <dvm/dvm_intf.h>
#include <lld/bf_lld_if.h>
#include <lld/lld_err.h>
#include <lld/lld_sku.h>
#include <lld/bf_dma_if.h>
#include <lld/lld_dr_if.h>
#include <target-utils/map/map.h>
#include <sys/time.h>
#include <time.h>

// Local includes
#include "bf_pm.h"
#include "pm_intf.h"
#include "pm_log.h"
#include "pm_task.h"

// Defined Macros

#define PORT_DISABLE_TIME_DURATION_SHORT 100
#define PORT_DISABLE_TIME_DURATION_LONG 10000
#define TIME_TO_WAIT_FOR_ENABLE 2000

#define INTERNAL_PORT_CONN_ID_START 600

typedef struct bf_pm_mac_multi_lane_ {
  bf_pal_mac_to_multi_serdes_lane_map_t *mac_chnl;
} bf_pm_mac_multi_lane_t;

typedef struct bf_pm_mac_blk {
  bf_pal_mac_to_serdes_lane_map_t *mac_chnl;
} bf_pm_mac_blk;

static bf_drv_client_handle_t bf_pm_drv_hdl;
static bf_pal_pltfm_reg_interface_t pltfm_interface = {0};
static bool bf_pm_dev_locked[BF_MAX_DEV_COUNT] = {0};

// MAP
static bool bf_pm_glb_db_initd = false;
static bf_map_t bf_pm_port_map_db[BF_MAX_DEV_COUNT];
static bf_map_t bf_pm_mac_map_db[BF_MAX_DEV_COUNT];

static bf_sys_rmutex_t
    port_map_mtx[BF_MAX_DEV_COUNT];  // Port recursive mutex for table map
static bf_sys_rmutex_t
    mac_map_mtx[BF_MAX_DEV_COUNT];  // Mac recursive mutex for table map
static bf_sys_timer_t port_stats_timer[BF_MAX_DEV_COUNT];
static bf_sys_timer_t rate_timer[BF_MAX_DEV_COUNT];
static bool rate_timer_on[BF_MAX_DEV_COUNT];

static bool port_stats_timer_started[BF_MAX_DEV_COUNT];
static uint32_t port_stats_timer_intvl[BF_MAX_DEV_COUNT];
static void pm_port_stats_timer_cb(struct bf_sys_timer_s *timer, void *data);
static bf_status_t pm_port_select_port_fsm_mode(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl);
static void pm_port_precoding_apply(bf_dev_id_t dev_id,
                                    bf_pal_front_port_handle_t *port_hdl);
static bf_status_t pm_dev_unlock_cb(bf_dev_id_t dev_id);
bf_status_t bf_pm_port_term_mode_get(bf_dev_id_t dev_id,
                                     bf_pal_front_port_handle_t *port_hdl,
                                     int ln,
                                     bf_pm_port_term_mode_e *term_mode);
static void pm_port_term_mode_apply(bf_dev_id_t dev_id,
                                    bf_pal_front_port_handle_t *port_hdl);
static bf_sds_line_rate_mode_t pm_port_speed_to_sds_line_rate_map(
    bf_port_speed_t ps);
static uint32_t mac_rx_pol[66][8];
static uint32_t mac_tx_pol[66][8];

bf_status_t bf_pm_update_pltfm_interface(void) {
  bf_status_t sts;
  // Get all the interfaces defined by the platforms module
  sts = bf_pal_pltfm_all_interface_get(&pltfm_interface);
  if (sts != BF_SUCCESS) {
    PM_ERROR("Unable to get the interfaces defined by platforms : %s (%d)",
             bf_err_str(sts),
             sts);
  }
  return sts;
}

/**
 * Start the port stats poll timer
 */
void pm_port_stats_poll_start(bf_dev_id_t dev_id) {
  bool is_sw_model = true;
  bf_status_t sts = bf_drv_device_type_get(dev_id, &is_sw_model);
  if (sts != BF_SUCCESS) {
    PM_ERROR("Unable to get the device type for dev : %d : %s (%d)",
             dev_id,
             bf_err_str(sts),
             sts);
  }
  if (!is_sw_model) {
    bf_sys_timer_status_t rc;
    if (port_stats_timer_started[dev_id]) return;
    rc = bf_sys_timer_create(&port_stats_timer[dev_id],
                             port_stats_timer_intvl[dev_id],
                             port_stats_timer_intvl[dev_id],
                             pm_port_stats_timer_cb,
                             (void *)(intptr_t)dev_id);
    if (rc) {
      PM_ERROR("Unable to create port stats timer : %d, dev %d", rc, dev_id);
      return;
    }
    rc = bf_sys_timer_start(&port_stats_timer[dev_id]);
    if (rc) {
      PM_ERROR("Unable to start port stats timer : %d, dev %d", rc, dev_id);
    }
    port_stats_timer_started[dev_id] = true;
  }
}

/**
 * Stop the port stats poll timer
 */
void pm_port_stats_poll_stop(bf_dev_id_t dev_id) {
  bf_sys_timer_status_t rc;
  if (port_stats_timer_started[dev_id] == false) return;
  rc = bf_sys_timer_stop(&port_stats_timer[dev_id]);
  if (rc) {
    PM_ERROR("Unable to stop port stats timer : %d", rc);
  }
  port_stats_timer_started[dev_id] = false;
}

/**
 * Given a port hdl return the key to be used to index the port database map
 */
static inline unsigned long pm_port_info_map_key_get(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl) {
  return ((port_hdl->conn_id << lld_get_chnls_per_mac(dev_id)) |
          (port_hdl->chnl_id));
}

/**
 * pm_port_info_get_from_port_hdl
 *
 * Look up the given conn/chnl (port_hdl) in the DB, across all devices
 */
static bf_pm_port_info_t *pm_port_info_get_from_port_hdl(
    bf_pal_front_port_handle_t *port_hdl, bf_dev_id_t *rtn_dev_id) {
  bf_map_sts_t map_sts;
  bf_pm_port_info_t *port_info = NULL;
  unsigned long port_key;

  for (int d = 0; d < BF_MAX_DEV_COUNT; d++) {
    port_key = pm_port_info_map_key_get(d, port_hdl);
    PORT_MGR_LOCK(&port_map_mtx[d]);
    map_sts = bf_map_get(&bf_pm_port_map_db[d], port_key, (void **)&port_info);
    PORT_MGR_UNLOCK(&port_map_mtx[d]);
    if (map_sts == BF_MAP_OK) {
      *rtn_dev_id = d;
      return port_info;
    }
  }
  PM_DEBUG("pm_port_info_get_from_port_hdl: Unable to get port info: %d/%d ",
           port_hdl->conn_id,
           port_hdl->chnl_id);
  return NULL;
}

/**
 * Given a dev port, return a copy of the corresponding bf_pm_port_info_t data
 * structure from the global database
 */
bf_pm_port_info_t *pm_port_info_get(bf_dev_id_t dev_id,
                                    bf_dev_port_t dev_port) {
  bf_map_sts_t map_sts;
  bf_pm_port_info_t *port_info;
  unsigned long port_key;

  PORT_MGR_LOCK(&port_map_mtx[dev_id]);
  map_sts = bf_map_get_first(
      &bf_pm_port_map_db[dev_id], &port_key, (void **)&port_info);
  PORT_MGR_UNLOCK(&port_map_mtx[dev_id]);
  while (map_sts == BF_MAP_OK) {
    if (port_info->dev_port == dev_port) {
      return port_info;
    }
    PORT_MGR_LOCK(&port_map_mtx[dev_id]);
    map_sts = bf_map_get_next(
        &bf_pm_port_map_db[dev_id], &port_key, (void **)&port_info);
    PORT_MGR_UNLOCK(&port_map_mtx[dev_id]);
  }

  return NULL;
}

/**
 * Given a dev port, return a copy of the corresponding bf_pm_port_info_t data
 * structure from the global database
 */
int pm_port_info_get_copy(bf_dev_id_t dev_id,
                          bf_dev_port_t dev_port,
                          bf_pm_port_info_t *port_info) {
  bf_pm_port_info_t *t_port_info;

  if (!port_info) return -1;

  t_port_info = pm_port_info_get(dev_id, dev_port);
  if (!t_port_info) return -1;

  memcpy(port_info, t_port_info, sizeof(*port_info));
  return 0;
}

/**
 * Utility to make a copy of a bf_pm_port_info_t, since a lot of ucli
 * commands seem to do that (not sure why as I don't see them modifying
 * the returned data
 */
void bf_pm_port_info_copy(bf_pm_port_info_t *from, bf_pm_port_info_t *to) {
  memcpy((void *)to, (void *)from, sizeof(bf_pm_port_info_t));
}

/**
 * return the first bf_pm_port_info_t data structure from
 * the given dev_id map. This restricts the search to a
 * specific dev_id
 */
static bf_pm_port_info_t *pm_port_info_get_first_on_this(bf_dev_id_t dev_id) {
  bf_map_sts_t map_sts;
  bf_pm_port_info_t *port_info;
  unsigned long port_key;

  PORT_MGR_LOCK(&port_map_mtx[dev_id]);
  map_sts = bf_map_get_first(
      &bf_pm_port_map_db[dev_id], &port_key, (void **)&port_info);
  PORT_MGR_UNLOCK(&port_map_mtx[dev_id]);
  if (map_sts == BF_MAP_OK) {
    return port_info;
  }
  return NULL;
}

/**
 * return the first bf_pm_port_info_t data structure from
 * the global database. This requires going thru the maps
 * associated with each dev_id until an entry is found.
 */
static bf_pm_port_info_t *pm_port_info_get_first(bf_dev_id_t *dev_id) {
  bf_map_sts_t map_sts;
  bf_pm_port_info_t *port_info;
  unsigned long port_key;

  for (int d = 0; d < BF_MAX_DEV_COUNT; d++) {
    PORT_MGR_LOCK(&port_map_mtx[d]);
    map_sts =
        bf_map_get_first(&bf_pm_port_map_db[d], &port_key, (void **)&port_info);
    PORT_MGR_UNLOCK(&port_map_mtx[d]);
    if (map_sts == BF_MAP_OK) {
      *dev_id = d;
      return port_info;
    }
  }
  return NULL;
}

/**
 * Given a dev_id and port_info, return the next port info data structure from
 * the global database
 */
static bf_pm_port_info_t *pm_port_info_get_next_on_this(
    bf_dev_id_t dev_id, bf_pm_port_info_t *port_info) {
  bf_map_sts_t map_sts;
  bf_pm_port_info_t *next_port_info;
  unsigned long port_key = 0;

  // start from given dev/port
  port_key =
      pm_port_info_map_key_get(dev_id, &port_info->pltfm_port_info.port_hdl);
  PORT_MGR_LOCK(&port_map_mtx[dev_id]);
  map_sts = bf_map_get_next(
      &bf_pm_port_map_db[dev_id], &port_key, (void **)&next_port_info);
  PORT_MGR_UNLOCK(&port_map_mtx[dev_id]);
  if (map_sts == BF_MAP_OK) {
    return next_port_info;
  }
  return NULL;
}

/**
 * Given a dev_id and port_info, return the next port info data structure from
 * the global database
 */
static bf_pm_port_info_t *pm_port_info_get_next(bf_dev_id_t dev_id,
                                                bf_pm_port_info_t *port_info,
                                                bf_dev_id_t *dev_id_of_port) {
  bf_map_sts_t map_sts;
  bf_pm_port_info_t *next_port_info;
  unsigned long port_key = 0;

  // start from given dev/port
  port_key =
      pm_port_info_map_key_get(dev_id, &port_info->pltfm_port_info.port_hdl);
  PORT_MGR_LOCK(&port_map_mtx[dev_id]);
  map_sts = bf_map_get_next(
      &bf_pm_port_map_db[dev_id], &port_key, (void **)&next_port_info);
  PORT_MGR_UNLOCK(&port_map_mtx[dev_id]);
  if (map_sts == BF_MAP_OK) {
    *dev_id_of_port = dev_id;
    return next_port_info;
  }
  // no next in the "current map", check any subsequent ones
  for (int d = dev_id + 1; d < BF_MAX_DEV_COUNT; d++) {
    PORT_MGR_LOCK(&port_map_mtx[d]);
    map_sts = bf_map_get_first(
        &bf_pm_port_map_db[d], &port_key, (void **)&next_port_info);
    PORT_MGR_UNLOCK(&port_map_mtx[d]);
    if (map_sts == BF_MAP_OK) {
      *dev_id_of_port = d;
      return next_port_info;
    }
  }
  return NULL;
}

/**
 * Given a dev_id, return a copy of the first bf_pm_port_info_t data structure
 * from the global database
 */
int pm_port_info_get_first_copy(bf_dev_id_t dev_id,
                                bf_pm_port_info_t *port_info,
                                bf_dev_id_t *dev_id_of_port) {
  bf_pm_port_info_t *t_port_info;
  (void)dev_id;

  if (!port_info) return -1;

  t_port_info = pm_port_info_get_first(dev_id_of_port);
  if (!t_port_info) return -1;

  memcpy(port_info, t_port_info, sizeof(*port_info));
  return 0;
}

/**
 * Given a dev_id and port_info, return a copy of the next port info data
 * structure from the global database
 */
int pm_port_info_get_next_copy(bf_dev_id_t dev_id,
                               bf_pm_port_info_t *port_info,
                               bf_pm_port_info_t *next_port_info,
                               bf_dev_id_t *dev_id_of_port) {
  bf_pm_port_info_t *t_port_info;

  if (!next_port_info) return -1;

  if (!port_info) {
    return pm_port_info_get_first_copy(dev_id, next_port_info, dev_id_of_port);
  }
  t_port_info = pm_port_info_get_next(dev_id, port_info, dev_id_of_port);
  if (!t_port_info) return -1;

  memcpy(next_port_info, t_port_info, sizeof(*next_port_info));
  return 0;
}

static bool pm_port_is_cpu_port(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  if ((dev_port >= lld_get_min_cpu_port(dev_id)) &&
      (dev_port <= lld_get_max_cpu_port(dev_id))) {
    return true;
  }
  return false;
}

static bool pm_port_validate_cpu_port_tof3(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           bf_port_speed_t speed,
                                           uint32_t n_lanes,
                                           bf_fec_type_t fec,
                                           char *err_str) {
  bf_pm_port_info_t *port_info = pm_port_info_get(dev_id, dev_port);
  uint32_t ch;

  if (!port_info) return false;

  ch = port_info->pltfm_port_info.port_hdl.chnl_id;

  switch (speed) {
    case BF_SPEED_NONE:
      strcpy(err_str, "Invalid speed");
      break;
    case BF_SPEED_100G:  // 4*25
      if ((n_lanes == 4) && (ch == 0)) {
        if ((fec == BF_FEC_TYP_NONE) || (fec == BF_FEC_TYP_RS)) {
          return true;
        }
      }
      strcpy(err_str, "Only 4*25g on ch0 and fec-type: none or RS supported");
      break;
    case BF_SPEED_40G:  // 4*10
      if ((n_lanes == 4) && (ch == 0)) {
        if ((fec == BF_FEC_TYP_NONE) || (fec == BF_FEC_TYP_FC)) {
          return true;
        }
      }
      strcpy(err_str, "Only 4*10g on ch0 and fec-type: none or FC supported");
      break;
    case BF_SPEED_50G:  // 2*25 of 1x50
      if ((n_lanes == 2) && ((ch == 0) || (ch == 2))) {
        if ((fec == BF_FEC_TYP_RS) || (fec == BF_FEC_TYP_NONE)) {
          return true;
        }
      }
      strcpy(err_str, "Only 2x25g on ch0/2 and fec-type: none or RS supported");
      break;
    case BF_SPEED_50G_CONS:
      strcpy(err_str, "Mode not supported");
      break;
    case BF_SPEED_40G_R2:
      strcpy(err_str, "Mode not supported");
      break;
    case BF_SPEED_25G:  // 1*25
    case BF_SPEED_10G:  // 1*10
    case BF_SPEED_1G:   // 1*1
      if (fec == BF_FEC_TYP_RS && speed == BF_SPEED_10G) {
        strcpy(err_str, "Invalid RS FEC. FC is valid");
        goto err_hndlr;
      }
      if (fec != BF_FEC_TYP_NONE && speed == BF_SPEED_1G) {
        strcpy(err_str, "Invalid FEC. FEC not supported");
        goto err_hndlr;
      }
      if ((n_lanes == 1) && ((ch == 0) || (ch == 2))) return true;
      strcpy(err_str, "only ch0/2 are valid");
      break;
    default:
      strcpy(err_str, "Invalid Speed");
      break;
  }

err_hndlr:
  return false;
}

static bool pm_port_validate_cpu_port(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      bf_port_speed_t speed,
                                      uint32_t n_lanes,
                                      bf_fec_type_t fec,
                                      char *err_str) {
  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));

  if (dev_family == BF_DEV_FAMILY_TOFINO3) {
    if (pm_port_validate_cpu_port_tof3(
            dev_id, dev_port, speed, n_lanes, fec, err_str)) {
      return true;
    }
    return false;
  }

  // check for cpu port
  switch (speed) {
    case BF_SPEED_NONE:
      strcpy(err_str, "Invalid speed");
      goto err_hndlr;
      break;
    case BF_SPEED_100G:  // 4*25
      if (dev_family == BF_DEV_FAMILY_TOFINO) {
        if ((dev_port == lld_get_min_cpu_port(dev_id)) && (n_lanes == 4) &&
            (fec == BF_FEC_TYP_NONE)) {
          return true;
        } else if (fec == BF_FEC_TYP_RS) {
          strcpy(err_str, "RS FEC not supported. Only NONE is valid");
          goto err_hndlr;
        } else if (fec == BF_FEC_TYP_FC) {
          strcpy(err_str, "FC FEC not supported. Only NONE is valid");
          goto err_hndlr;
        } else {
          strcpy(err_str, "Only 4x25g on ch0 supported");
          goto err_hndlr;
        }
      } else if (dev_family == BF_DEV_FAMILY_TOFINO2) {
        if ((dev_port == lld_get_min_cpu_port(dev_id)) && (n_lanes == 4) &&
            ((fec == BF_FEC_TYP_NONE) || (fec == BF_FEC_TYP_RS))) {
          return true;
        } else if (fec == BF_FEC_TYP_FC) {
          strcpy(err_str, "FC FEC not supported. Only NONE or RS FEC is valid");
          goto err_hndlr;
        } else {
          strcpy(err_str, "Only 4x25g on ch0 supported");
          goto err_hndlr;
        }
      } else if (dev_family == BF_DEV_FAMILY_TOFINO3) {
        if ((dev_port == lld_get_min_cpu_port(dev_id)) && (n_lanes == 4)) {
          if ((fec == BF_FEC_TYP_NONE) || (fec == BF_FEC_TYP_RS)) {
            return true;
          } else if (fec == BF_FEC_TYP_FC) {
            strcpy(err_str,
                   "FC FEC not supported. Only NONE or RS FEC is valid");
            goto err_hndlr;
          }
        } else if ((dev_port == lld_get_min_cpu_port(dev_id)) &&
                   (n_lanes == 1)) {
          if (fec == BF_FEC_TYP_RS) {
            return true;
          } else {
            strcpy(err_str, "Configure RS FEC for 100G-R1");
            goto err_hndlr;
          }
        } else {
          strcpy(err_str, "Only 4x25g on ch0 supported");
          goto err_hndlr;
        }
      }
      break;
    case BF_SPEED_40G:  // 4*10
      if ((dev_port == lld_get_min_cpu_port(dev_id)) && (n_lanes == 4) &&
          ((fec == BF_FEC_TYP_NONE) || (fec == BF_FEC_TYP_FIRECODE))) {
        return true;
      } else if (fec == BF_FEC_TYP_RS) {
        strcpy(err_str, "RS FEC not supported. Only NONE or FC FEC is valid");
        goto err_hndlr;
      } else {
        strcpy(err_str, "Only 4x25g on ch0 supported");
        goto err_hndlr;
      }
      break;
    case BF_SPEED_50G:  // 2*25 of 1x50
      if (dev_family == BF_DEV_FAMILY_TOFINO) {
        if ((dev_port == 64 || dev_port == 66) && (n_lanes == 2)) {
          return true;
        }
        strcpy(err_str, "Only 2x25g on ch0/2 supported");
        goto err_hndlr;
      } else if (dev_family == BF_DEV_FAMILY_TOFINO2) {
        if (n_lanes == 1) {
          strcpy(err_str, "Only 2x25g on ch0/2 supported");
          goto err_hndlr;
        }
        if (((dev_port == lld_get_min_cpu_port(dev_id)) ||
             (dev_port == lld_get_min_cpu_port(dev_id) + 2)) &&
            (n_lanes == 2)) {
          return true;
        }
        strcpy(err_str, "Only 2x25g on ch0/2 supported");
        goto err_hndlr;
      } else if (dev_family == BF_DEV_FAMILY_TOFINO3) {
        if (n_lanes == 1) {
          if (fec == BF_FEC_TYP_RS) {
            return true;
          } else {
            strcpy(err_str, "Configure RS FEC for 100G-R1");
            goto err_hndlr;
          }
        }
        if (((dev_port == lld_get_min_cpu_port(dev_id)) ||
             (dev_port == lld_get_min_cpu_port(dev_id) + 2)) &&
            (n_lanes == 2)) {
          return true;
        }
        strcpy(err_str, "Only 2x25g on ch0/2 supported");
        goto err_hndlr;
      }

      break;
    case BF_SPEED_50G_CONS:
      strcpy(err_str, "Mode not supported");
      break;
    case BF_SPEED_40G_R2:
      strcpy(err_str, "Mode not supported");
      break;
    case BF_SPEED_25G:  // 1*25
    case BF_SPEED_10G:  // 1*10
    case BF_SPEED_1G:   // 1*1
      if (fec == BF_FEC_TYP_RS && speed == BF_SPEED_10G) {
        strcpy(err_str, "Invalid RS FEC. FC is valid");
        goto err_hndlr;
      }
      if (fec != BF_FEC_TYP_NONE && speed == BF_SPEED_1G) {
        strcpy(err_str, "Invalid FEC. FEC not supported");
        goto err_hndlr;
      }
      if (n_lanes == 1) return true;
      break;
    default:
      strcpy(err_str, "Invalid Speed");
      goto err_hndlr;
      break;
  }

err_hndlr:
  return false;
}

/**
 * Given a port handle return if a port can be added with that speed on
 * that MAC chnl
 */
static bool pm_port_valid_speed_and_channel_internal(bf_dev_id_t dev_id,
                                                     bf_dev_port_t dev_port,
                                                     bf_port_speed_t speed,
                                                     uint32_t n_lanes,
                                                     bf_fec_type_t fec,
                                                     bool no_error) {
  bf_pm_port_info_t *port_info;
  uint32_t ch;
  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) return false;
  char *speed_str = "";
  char err_str[250] = "Unknown";
  bool is_56g_mode = false, is_112g_mode = false;

  // ch = port_info->pltfm_port_info.log_mac_lane;
  ch = port_info->pltfm_port_info.port_hdl.chnl_id;
  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));
  bf_port_speed_to_str(speed, &speed_str);
  if (!speed) {
    snprintf(err_str, sizeof(err_str), "%s", "Speed cannot be zero");
    goto err_hndlr;
  }
  if (!n_lanes) {
    snprintf(err_str, sizeof(err_str), "%s", "Num of lanes cannot be zero");
    goto err_hndlr;
  }

  if (dev_family == BF_DEV_FAMILY_TOFINO3) {
    is_56g_mode =
        lld_get_num_serdes_per_mac(dev_id, dev_port) == 8 ? true : false;
    is_112g_mode = !is_56g_mode;
    if ((dev_port == 0) || (dev_port == 6)) {
      snprintf(err_str,
               sizeof(err_str),
               "%s",
               "Dev-ports 0 or 6 cannot be configured");
      goto err_hndlr;
    }
  }

  switch (dev_family) {
    case BF_DEV_FAMILY_TOFINO2:
    case BF_DEV_FAMILY_TOFINO3:  // Separate the case - TBD
      if ((dev_port >= lld_get_min_cpu_port(dev_id)) &&
          (dev_port <= lld_get_max_cpu_port(dev_id))) {
        if (pm_port_validate_cpu_port(
                dev_id, dev_port, speed, n_lanes, fec, err_str)) {
          return true;
        }
        goto err_hndlr;
      } else {
        // check for fp ports
        switch (speed) {
          case BF_SPEED_400G:  // 8*50
            if (fec != BF_FEC_TYP_RS) {
              snprintf(err_str, sizeof(err_str), "%s", "RS FEC not Configured");
              goto err_hndlr;
            }

            if (dev_family == BF_DEV_FAMILY_TOFINO3) {
              if ((ch == 0) && (n_lanes == 8) && (is_56g_mode)) return true;
              if ((ch == 0) && (n_lanes == 4) && (is_112g_mode)) return true;
            } else if (dev_family == BF_DEV_FAMILY_TOFINO2) {
              if ((ch == 0) && (n_lanes == 8)) return true;
            }
            snprintf(err_str,
                     sizeof(err_str),
                     "%s",
                     "Channel not valid for this speed");
            break;
          case BF_SPEED_200G:  // 4*50 or 8*25
            if (fec != BF_FEC_TYP_RS) {
              snprintf(err_str, sizeof(err_str), "%s", "RS FEC not Configured");
              goto err_hndlr;
            }

            if (dev_family == BF_DEV_FAMILY_TOFINO3) {
              // R2 - 112g
              if (((ch == 0) || (ch == 2)) && (n_lanes == 2) && (is_112g_mode))
                return true;
              // R4 - 0 and 2 for 56g , 0 only for 112g
              if (((ch == 0) || (ch == 4)) && (n_lanes == 4) && (is_56g_mode))
                return true;
              if (((ch == 0)) && (n_lanes == 4) && (is_112g_mode)) return true;
              // R8 - 56g
              if (((ch == 0)) && (n_lanes == 8) && (is_56g_mode)) return true;
            }

            if (dev_family == BF_DEV_FAMILY_TOFINO2) {
              if (((ch == 0) || (ch == 4)) && (n_lanes == 4)) return true;
              if ((ch == 0) && (n_lanes == 8)) return true;
            }
            snprintf(err_str,
                     sizeof(err_str),
                     "%s",
                     "Channel not valid for this speed");
            break;
          case BF_SPEED_40G:  // 4*10
            if (fec == BF_FEC_TYP_RS) {
              snprintf(err_str,
                       sizeof(err_str),
                       "%s",
                       "RS FEC not valid for this speed");
            } else {
              if (dev_family == BF_DEV_FAMILY_TOFINO3) {
                if (((ch == 0) || (ch == 4)) && (n_lanes == 4) && (is_56g_mode))
                  return true;
                if (((ch == 0)) && (n_lanes == 4) && (is_112g_mode))
                  return true;
                snprintf(err_str,
                         sizeof(err_str),
                         "%s",
                         "Channel not valid for this speed");
                goto err_hndlr;
              }

              if (((ch == 0) || (ch == 4)) && (n_lanes == 4)) return true;
              snprintf(err_str,
                       sizeof(err_str),
                       "%s",
                       "Channel not valid for this speed");
            }
            break;
          case BF_SPEED_100G:  // 2*50 or 4*25

            if (dev_family == BF_DEV_FAMILY_TOFINO3) {
              // R1 - 112g
              if (((ch == 0) || (ch == 1) || (ch == 2) || (ch == 3)) &&
                  (n_lanes == 1) && (is_112g_mode)) {
                if (fec == BF_FEC_TYP_RS) return true;
                snprintf(err_str,
                         sizeof(err_str),
                         "%s",
                         "Only RS FEC supported for this speed");
                goto err_hndlr;
              }

              // R2 - 0 and 2 for 112g ; 0, 2, 4, 6 for 56g
              if (((ch == 0) || (ch == 2)) && (n_lanes == 2) &&
                  (is_112g_mode)) {
                if (fec == BF_FEC_TYP_RS) return true;
                snprintf(err_str,
                         sizeof(err_str),
                         "%s",
                         "Only RS FEC supported for this speed");
                goto err_hndlr;
              }

              if (((ch == 0) || (ch == 2) || (ch == 4) || (ch == 6)) &&
                  (n_lanes == 2) && (is_56g_mode)) {
                if (fec == BF_FEC_TYP_RS) return true;
                snprintf(err_str,
                         sizeof(err_str),
                         "%s",
                         "Only RS FEC supported for this speed");
                goto err_hndlr;
              }

              // R4 - 0 for 112g ; 0 and 4 for 56g mode
              if (((ch == 0)) && (n_lanes == 4) && (is_112g_mode)) {
                if ((fec == BF_FEC_TYP_RS) || (fec == BF_FEC_TYP_NONE))
                  return true;
                snprintf(err_str,
                         sizeof(err_str),
                         "%s",
                         "Only RS FEC or no-fec supported for this speed");
                goto err_hndlr;
              }

              if (((ch == 0) || (ch == 4)) && (n_lanes == 4) && (is_56g_mode)) {
                if ((fec == BF_FEC_TYP_RS) || (fec == BF_FEC_TYP_NONE))
                  return true;
                snprintf(err_str,
                         sizeof(err_str),
                         "%s",
                         "Only RS FEC or no-fec supported for this speed");
                goto err_hndlr;
              }

              snprintf(err_str,
                       sizeof(err_str),
                       "%s",
                       "Channel not valid for this speed");
              goto err_hndlr;
            }

            if (n_lanes == 4) {
              if ((ch != 0) && (ch != 4)) {
                snprintf(err_str,
                         sizeof(err_str),
                         "%s",
                         "Channel not valid for this speed");
                goto err_hndlr;
              } else if (fec == BF_FEC_TYP_FC) {
                snprintf(err_str,
                         sizeof(err_str),
                         "%s",
                         "RS FEC (or NONE) type required for this speed");
                goto err_hndlr;
              } else {
                return true;
              }
            } else if (n_lanes == 2) {
              if ((ch != 0) && (ch != 2) && (ch != 4) && (ch != 6)) {
                snprintf(err_str,
                         sizeof(err_str),
                         "%s",
                         "Channel not valid for this speed");
                goto err_hndlr;
              } else if (fec != BF_FEC_TYP_RS) {
                snprintf(err_str,
                         sizeof(err_str),
                         "%s",
                         "RS FEC required for this speed");
                goto err_hndlr;
              } else {
                return true;
              }
            }
            snprintf(
                err_str, sizeof(err_str), "%s", "Invalid configuration for ch");
            break;
          case BF_SPEED_50G:  // 1*50 or 2*25
          case BF_SPEED_50G_CONS:
            if (dev_family == BF_DEV_FAMILY_TOFINO3) {
              if (speed == BF_SPEED_50G_CONS) {
                snprintf(err_str,
                         sizeof(err_str),
                         "%s",
                         "Unsupported speed configuration");
                break;
              }

              // R1 - 0, 1, 2, 3
              if (((ch == 0) || (ch == 1) || (ch == 2) || (ch == 3)) &&
                  (n_lanes == 1) && (is_112g_mode)) {
                if (fec == BF_FEC_TYP_RS) return true;
                snprintf(err_str,
                         sizeof(err_str),
                         "%s",
                         "Only RS FEC is supported for this speed");
              }
              // R1 - 0, 2, 4, 6
              if (((ch == 0) || (ch == 2) || (ch == 4) || (ch == 6)) &&
                  (n_lanes == 1) && (is_56g_mode)) {
                if (fec == BF_FEC_TYP_RS) return true;
                snprintf(err_str,
                         sizeof(err_str),
                         "%s",
                         "Only RS FEC is supported for this speed");
              }

              // 50G-R2 0, 2, 4, 6 for 56g
              if (((ch == 0) || (ch == 2) || (ch == 4) || (ch == 6)) &&
                  (n_lanes == 2) && (is_56g_mode))
                return true;

              // 50g-R2
              if (((ch == 0) || (ch == 2)) && (n_lanes == 2) && (is_112g_mode))
                return true;
              snprintf(err_str,
                       sizeof(err_str),
                       "%s",
                       "Invalid configuration for this speed");
              goto err_hndlr;
            }

            if (n_lanes == 1) {
              if (fec == BF_FEC_TYP_RS) return true;
              snprintf(err_str,
                       sizeof(err_str),
                       "%s",
                       "Only RS FEC is supported for this speed");
              break;
            }
            if (((ch == 0) || (ch == 2) || (ch == 4) || (ch == 6)) &&
                (n_lanes == 2)) {
              return true;
            }
            snprintf(err_str,
                     sizeof(err_str),
                     "%s",
                     "Channel not valid for this speed");
            break;
          case BF_SPEED_25G:  // 1*25
          case BF_SPEED_10G:  // 1*10
            if (fec == BF_FEC_TYP_RS && speed == BF_SPEED_10G) {
              snprintf(err_str,
                       sizeof(err_str),
                       "%s",
                       "RS FEC Invalid. NONE Or FC is valid");
              goto err_hndlr;
            }
            if (n_lanes == 1) return true;
            break;
          case BF_SPEED_1G:
            if ((n_lanes == 1) && (fec == BF_FEC_TYP_NONE) &&
                ((ch == 0) || (ch == 2) || (ch == 4) || (ch == 6))) {
              return true;
            }
            break;
          case BF_SPEED_40G_R2:  // 2*20
            if (dev_family == BF_DEV_FAMILY_TOFINO3) {
              snprintf(err_str,
                       sizeof(err_str),
                       "%s",
                       "Channel not valid for this speed");
              goto err_hndlr;
            }
            if ((n_lanes == 2) &&
                ((ch == 0) || (ch == 2) || (ch == 4) || (ch == 6))) {
              return true;
            } else {
              snprintf(err_str,
                       sizeof(err_str),
                       "%s",
                       "Channel not valid for this speed");
            }
            break;
          default:
            snprintf(err_str, sizeof(err_str), "%s", "Invalid Speed");
            goto err_hndlr;
            break;
        }
      }
      break;
    case BF_DEV_FAMILY_TOFINO:
      if ((dev_port >= lld_get_min_cpu_port(dev_id)) &&
          (dev_port <= lld_get_max_cpu_port(dev_id))) {
        if (pm_port_validate_cpu_port(
                dev_id, dev_port, speed, n_lanes, fec, err_str)) {
          return true;
        }
        goto err_hndlr;
      }

      // check start lane
      switch (speed) {
        case BF_SPEED_100G:  // 4*25
          if (fec == BF_FEC_TYP_FC) {
            snprintf(err_str,
                     sizeof(err_str),
                     "%s",
                     "FC FEC not valid for this speed");
          } else {
            if ((ch == 0) && (n_lanes == 4))
              return true;
            else
              goto invalid_speed_lane;
          }
          break;
        case BF_SPEED_40G:  // 4*10
          if (fec == BF_FEC_TYP_RS) {
            snprintf(err_str,
                     sizeof(err_str),
                     "%s",
                     "RS FEC not valid for this speed");
          } else {
            if ((ch == 0) && (n_lanes == 4)) return true;
          }
          break;
        case BF_SPEED_50G:  // 2*25
          if ((ch == 0 || ch == 2) && (n_lanes == 2))
            return true;
          else
            goto invalid_speed_lane;
          break;
        case BF_SPEED_25G:  // 1*25
        case BF_SPEED_10G:  // 1*10
          if (fec == BF_FEC_TYP_RS && speed == BF_SPEED_10G) {
            snprintf(
                err_str, sizeof(err_str), "%s", "Invalid RS FEC. FC is valid");
            goto err_hndlr;
          }
          if (n_lanes == 1) return true;
          break;
        case BF_SPEED_1G:  // 1*1
          snprintf(
              err_str, sizeof(err_str), "%s", "1G not supported on this  port");
          goto err_hndlr;
        default:
          snprintf(err_str, sizeof(err_str), "%s", "Invalid Speed");
          goto err_hndlr;
          break;
      }
    invalid_speed_lane:
      snprintf(
          err_str, sizeof(err_str), "%s", "Speed and number of lanes mismatch");
      break;
    default:
      break;
  }

err_hndlr:
  if (no_error) return false;
  PM_ERROR(
      "Port validation failed for Dev-family: %s dev : %d d_p : %d : %d/%d "
      "speed : %s num-lanes: %d error-msg: %s",
      bf_dev_family_str(dev_family),
      dev_id,
      dev_port,
      port_info->pltfm_port_info.port_hdl.conn_id,
      port_info->pltfm_port_info.port_hdl.chnl_id,
      speed_str,
      n_lanes,
      err_str);

  return false;
}
bool pm_port_valid_speed_and_channel(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port,
                                     bf_port_speed_t speed,
                                     uint32_t n_lanes,
                                     bf_fec_type_t fec) {
  return pm_port_valid_speed_and_channel_internal(
      dev_id, dev_port, speed, n_lanes, fec, false);
}

bool bf_pm_port_valid_speed_and_channel(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        bf_port_speed_t speed,
                                        uint32_t n_lanes,
                                        bf_fec_type_t fec) {
  return pm_port_valid_speed_and_channel_internal(
      dev_id, dev_port, speed, n_lanes, fec, false);
}

/**
 * Given a device id, return if the device is locked or not
 */
static bool pm_dev_is_locked(bf_dev_id_t dev_id) {
  return bf_pm_dev_locked[dev_id];
}

/**
 * Get exlusive access for the corresponding
 * bf_pm_port_info_t data structure
 */
static inline void pm_port_exclusive_access_start(
    bf_dev_id_t dev_id, bf_pm_port_info_t *port_info) {
  int rc;

  rc = bf_sys_rmutex_lock(&port_info->port_mtx);
  if (rc) {
    PM_ERROR(
        "Error getting pm-port lock for dev : %d : port : %d/%d (%d) : err %d",
        dev_id,
        port_info->pltfm_port_info.port_hdl.conn_id,
        port_info->pltfm_port_info.port_hdl.chnl_id,
        port_info->dev_port,
        rc);
  }
}

/**
 * Release exlusive access for the corresponding
 * bf_pm_port_info_t data structure
 */
static inline void pm_port_exclusive_access_end(bf_dev_id_t dev_id,
                                                bf_pm_port_info_t *port_info) {
  int rc;

  rc = bf_sys_rmutex_unlock(&port_info->port_mtx);
  if (rc) {
    PM_ERROR(
        "Error releasing pm-port lock for dev : %d : port : %d/%d (%d) : err "
        "%d",
        dev_id,
        port_info->pltfm_port_info.port_hdl.conn_id,
        port_info->pltfm_port_info.port_hdl.chnl_id,
        port_info->dev_port,
        rc);
  }
}

/**
 * Given the front panel port info, return the corresponding dev port
 */
static bf_status_t pm_pltfm_port_info_to_dev_port(
    bf_dev_id_t dev_id,
    bf_pal_front_port_info_t *port_info,
    bf_dev_port_t *dev_port) {
  bf_status_t sts;

  if (!port_info || !dev_port) {
    PM_ERROR("Invalid port_info or dev_port, dev %d", dev_id);
    return BF_INVALID_ARG;
  }

  sts = bf_port_map_mac_to_dev_port(
      dev_id, port_info->log_mac_id, port_info->log_mac_lane, dev_port);
  if (sts != BF_SUCCESS) {
    return sts;
  }

  return BF_SUCCESS;
}

/**
 * Get information of all the ports from the platforms module
 */
static bf_status_t pm_pltfm_port_info_get(bf_dev_id_t dev_id) {
  bf_pal_front_port_info_t curr_port_info, next_port_info;
  bf_pal_front_port_info_t *curr_port_info_ptr = &curr_port_info;
  bf_pal_front_port_info_t *next_port_info_ptr = &next_port_info;
  bf_pm_port_info_t *port_info;
  bf_status_t sts;
  bf_dev_port_t dev_port;
  bf_map_sts_t map_sts;
  unsigned long port_key = 0;
  uint32_t internal_pipes;

  if (!pltfm_interface.pltfm_front_panel_port_get_next ||
      !pltfm_interface.pltfm_front_panel_port_get_first) {
    return BF_NOT_IMPLEMENTED;
  }

  sts = pltfm_interface.pltfm_front_panel_port_get_first(curr_port_info_ptr);
  if (sts != BF_SUCCESS) {
    PM_ERROR("Unable to get the first front panel port in the system : %s (%d)",
             bf_err_str(sts),
             sts);
    bf_sys_dbgchk(0);
    return sts;
  }
  if (curr_port_info_ptr->dev_id == dev_id) {
    internal_pipes = lld_get_internal_pipe_numb(curr_port_info_ptr->dev_id);
    if ((internal_pipes == 2) &&
        (bf_is_dev_type_family_tofino(
            lld_sku_get_dev_type(curr_port_info_ptr->dev_id)))) {
      // For Tofino 32Q only, Platform would give log_mac_id 0-31, while in the
      // real hw, the right log_mac_id are the even numbers in 0-63
      curr_port_info_ptr->log_mac_id = 2 * curr_port_info_ptr->log_mac_id;
    }
    // Get the corresponding dev port for the front panel port
    sts = pm_pltfm_port_info_to_dev_port(
        curr_port_info_ptr->dev_id, curr_port_info_ptr, &dev_port);
    if (sts != BF_SUCCESS) {
      // Not all front panel ports will have corresponding dev ports on SKUs.
      // Just continue on to the next front panel port.
    }

    if (sts == BF_SUCCESS) {
      port_info =
          (bf_pm_port_info_t *)bf_sys_calloc(1, 1 * sizeof(bf_pm_port_info_t));
      if (!port_info) {
        PM_ERROR(
            "Unable to allocate memory for port info in database for dev : %d "
            ": "
            "port : %d/%d (%d)",
            curr_port_info_ptr->dev_id,
            curr_port_info_ptr->port_hdl.conn_id,
            curr_port_info_ptr->port_hdl.chnl_id,
            dev_port);
        return BF_NO_SYS_RESOURCES;
      }
      // Cache the front panel port info in the local database
      memcpy(&port_info->pltfm_port_info,
             curr_port_info_ptr,
             sizeof(port_info->pltfm_port_info));
      port_info->dev_port = dev_port;
      port_info->is_internal = false;
      port_key = pm_port_info_map_key_get(curr_port_info_ptr->dev_id,
                                          &curr_port_info_ptr->port_hdl);
      PORT_MGR_LOCK(&port_map_mtx[curr_port_info_ptr->dev_id]);
      map_sts = bf_map_add(&bf_pm_port_map_db[curr_port_info_ptr->dev_id],
                           port_key,
                           (void *)port_info);
      PORT_MGR_UNLOCK(&port_map_mtx[curr_port_info_ptr->dev_id]);
      if (map_sts != BF_MAP_OK) {
        PM_ERROR(
            "Unable to add port in database for for dev : %d : port : %d/%d "
            "(%d) "
            ": "
            "map_err : %d",
            curr_port_info_ptr->dev_id,
            curr_port_info_ptr->port_hdl.conn_id,
            curr_port_info_ptr->port_hdl.chnl_id,
            dev_port,
            map_sts);
        return map_sts;
      } else {
        PM_DEBUG("dev : %d : port : %d/%d (d_p=%d) added to bf_pm_port_map_db",
                 curr_port_info_ptr->dev_id,
                 curr_port_info_ptr->port_hdl.conn_id,
                 curr_port_info_ptr->port_hdl.chnl_id,
                 dev_port);
      }
    } else {
      // This indicates that we were unable to get the dev port for the given
      // front panel port. This might be possible error on the platforms side.
      // So just reset the status and move on to process the rest of the front
      // panel ports if any
      sts = BF_SUCCESS;
    }
  }
  while (sts == BF_SUCCESS) {
    PM_DEBUG("pltfm_interface.pltfm_front_panel_port_get_next: %d/%d",
             curr_port_info_ptr->port_hdl.conn_id,
             curr_port_info_ptr->port_hdl.chnl_id);

    // Get the next front panel port in the system
    sts = pltfm_interface.pltfm_front_panel_port_get_next(curr_port_info_ptr,
                                                          next_port_info_ptr);
    if (sts == BF_OBJECT_NOT_FOUND) break;
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to get the front panel port after front port %d/%d in the "
          "system : %s (%d)",
          curr_port_info_ptr->port_hdl.conn_id,
          curr_port_info_ptr->port_hdl.chnl_id,
          bf_err_str(sts),
          sts);
      return sts;
    }
    if (next_port_info_ptr->dev_id != dev_id) {
      curr_port_info_ptr = next_port_info_ptr;
      continue;
    }
    internal_pipes = lld_get_internal_pipe_numb(next_port_info_ptr->dev_id);
    if ((internal_pipes == 2) &&
        (bf_is_dev_type_family_tofino(
            lld_sku_get_dev_type(curr_port_info_ptr->dev_id)))) {
      next_port_info_ptr->log_mac_id = 2 * next_port_info_ptr->log_mac_id;
    }
    // Get the corresponding dev port for the front panel port
    sts = pm_pltfm_port_info_to_dev_port(
        next_port_info_ptr->dev_id, next_port_info_ptr, &dev_port);

    // fall thru intentionally on error
    if (sts == BF_SUCCESS) {
      port_info =
          (bf_pm_port_info_t *)bf_sys_calloc(1, 1 * sizeof(bf_pm_port_info_t));
      if (!port_info) {
        PM_ERROR(
            "Unable to allocate memory for port info in database for dev : %d "
            ": "
            "port : %d/%d (%d)",
            next_port_info_ptr->dev_id,
            next_port_info_ptr->port_hdl.conn_id,
            next_port_info_ptr->port_hdl.chnl_id,
            dev_port);
        return BF_NO_SYS_RESOURCES;
      }

      // Cache the front panel port info in the local database
      memcpy(&port_info->pltfm_port_info,
             next_port_info_ptr,
             sizeof(port_info->pltfm_port_info));
      port_info->dev_port = dev_port;
      port_info->is_internal = false;

      port_key = pm_port_info_map_key_get(next_port_info_ptr->dev_id,
                                          &next_port_info_ptr->port_hdl);
      PORT_MGR_LOCK(&port_map_mtx[next_port_info_ptr->dev_id]);
      map_sts = bf_map_add(&bf_pm_port_map_db[next_port_info_ptr->dev_id],
                           port_key,
                           (void *)port_info);
      PORT_MGR_UNLOCK(&port_map_mtx[next_port_info_ptr->dev_id]);
      if (map_sts != BF_MAP_OK) {
        PM_ERROR(
            "Unable to add port in database for for dev : %d : port : %d/%d "
            "(%d) "
            ": map_err : %d",
            next_port_info_ptr->dev_id,
            next_port_info_ptr->port_hdl.conn_id,
            next_port_info_ptr->port_hdl.chnl_id,
            dev_port,
            map_sts);
        return map_sts;
      } else {
        PM_DEBUG("dev : %d : port : %d/%d (d_p=%d) added to bf_pm_port_map_db",
                 curr_port_info_ptr->dev_id,
                 curr_port_info_ptr->port_hdl.conn_id,
                 curr_port_info_ptr->port_hdl.chnl_id,
                 dev_port);
      }
    } else {
      // This indicates that we were unable to get the dev port for the given
      // front panel port. This might be possible error on the platforms side.
      // So just reset the status and move on to process the rest of the front
      // panel ports if any
      sts = BF_SUCCESS;
    }

    // Make the current port equal to the next
    curr_port_info_ptr = next_port_info_ptr;
  }

  return BF_SUCCESS;
}

/**
 * Given a front panel port handle, iterate through the global database and
 * get the corresponding dev port
 */
static bf_status_t pm_front_port_handle_to_dev_port(
    bf_pal_front_port_handle_t *port_hdl,
    bf_dev_id_t *dev_id,
    bf_dev_port_t *dev_port) {
  bf_pm_port_info_t *port_info = NULL;
  bf_dev_id_t dev_id_of_port = 0;

  if (!port_hdl || !dev_port || !dev_id) {
    return BF_INVALID_ARG;
  }
  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);

  if (!port_info) {
    return BF_OBJECT_NOT_FOUND;
  }

  *dev_port = port_info->dev_port;
  *dev_id = dev_id_of_port;
  return BF_SUCCESS;
}

bf_status_t pm_mac_to_multi_serdes_lane_map_set(bf_dev_id_t dev_id);
// static bf_status_t pm_mac_to_multi_serdes_lane_map_set(bf_dev_id_t dev_id) {
bf_status_t pm_mac_to_multi_serdes_lane_map_set(bf_dev_id_t dev_id) {
  uint32_t log_mac_id, log_mac_lane, conn_id, chnl_id;
  bf_pal_front_port_handle_t *t_port_hdl;
  bf_pal_mac_to_multi_serdes_lane_map_t mac_block;
  bf_status_t sts;
  bf_mac_block_lane_map_t mac_blk_lanes_info;
  bf_pm_port_info_t *port_info;
  bf_map_sts_t map_sts;
  bf_pm_mac_multi_lane_t *mac_multi_map;
  uint32_t n_serdes_per_lane = 0;
  uint32_t i;

  if (!pltfm_interface.pltfm_mac_to_multi_serdes_map_get) {
    return BF_NOT_IMPLEMENTED;
  }

  // Iterate through all the ports and fill in the MAC map
  port_info = pm_port_info_get_first_on_this(dev_id);
  for (; port_info != NULL;
       port_info = pm_port_info_get_next_on_this(dev_id, port_info)) {
    t_port_hdl = &port_info->pltfm_port_info.port_hdl;
    log_mac_id = port_info->pltfm_port_info.log_mac_id;
    log_mac_lane = port_info->pltfm_port_info.log_mac_lane;
    conn_id = t_port_hdl->conn_id;
    chnl_id = t_port_hdl->chnl_id;

    // Get the TX and RX serdes connected to this logical MAC block and channel
    mac_block.log_mac_lane = log_mac_lane;
    sts = pltfm_interface.pltfm_mac_to_multi_serdes_map_get(t_port_hdl,
                                                            &mac_block);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to get the MAC to Serdes lane mapping for dev : %d : Front "
          "Port : %d/%d (%d) : %s (%d)",
          dev_id,
          conn_id,
          chnl_id,
          port_info->dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }
    n_serdes_per_lane = mac_block.num_serdes_per_lane;

    PORT_MGR_LOCK(&mac_map_mtx[dev_id]);
    map_sts = bf_map_get(
        &bf_pm_mac_map_db[dev_id], log_mac_id, (void *)&mac_multi_map);
    PORT_MGR_UNLOCK(&mac_map_mtx[dev_id]);
    if (map_sts == BF_MAP_NO_KEY) {
      mac_multi_map = (bf_pm_mac_multi_lane_t *)bf_sys_calloc(
          1, 1 * sizeof(bf_pm_mac_multi_lane_t));
      if (!mac_multi_map) {
        PM_ERROR(
            "Unable to allocate space for mac blk for dev : %d : port : %d/%d "
            "(%d) : MAC : %d ",
            dev_id,
            conn_id,
            chnl_id,
            port_info->dev_port,
            log_mac_id);
        return BF_NO_SYS_RESOURCES;
      }
      mac_multi_map->mac_chnl =
          (bf_pal_mac_to_multi_serdes_lane_map_t *)bf_sys_calloc(
              lld_get_chnls_dev_port(dev_id, port_info->dev_port),
              sizeof(bf_pal_mac_to_multi_serdes_lane_map_t));
      if (!mac_multi_map->mac_chnl) {
        PM_ERROR(
            "Unable to allocate space for mac chnls for dev : %d : port : "
            "%d/%d "
            "(%d) : MAC : %d : chnl : %d",
            dev_id,
            conn_id,
            chnl_id,
            port_info->dev_port,
            log_mac_id,
            lld_get_chnls_dev_port(dev_id, port_info->dev_port));
        bf_sys_free(mac_multi_map);
        return BF_NO_SYS_RESOURCES;
      }

      mac_multi_map->mac_chnl[log_mac_lane].num_serdes_per_lane =
          n_serdes_per_lane;
      for (i = 0; i < n_serdes_per_lane; i++) {
        mac_multi_map->mac_chnl[log_mac_lane].tx_lane[i] = mac_block.tx_lane[i];
        mac_multi_map->mac_chnl[log_mac_lane].rx_lane[i] = mac_block.rx_lane[i];
        mac_multi_map->mac_chnl[log_mac_lane].tx_inv[i] = mac_block.tx_inv[i];
        mac_multi_map->mac_chnl[log_mac_lane].rx_inv[i] = mac_block.rx_inv[i];
        mac_rx_pol[log_mac_id][chnl_id + i] = mac_block.rx_inv[i];
        mac_tx_pol[log_mac_id][chnl_id + i] = mac_block.tx_inv[i];
      }

      PORT_MGR_LOCK(&mac_map_mtx[dev_id]);
      map_sts = bf_map_add(
          &bf_pm_mac_map_db[dev_id], log_mac_id, (void *)mac_multi_map);
      PORT_MGR_UNLOCK(&mac_map_mtx[dev_id]);
      if (map_sts != BF_MAP_OK) {
        PM_ERROR(
            "Unable to add mac block to map for dev : %d : port : %d/%d (%d) : "
            "MAC : %d : map_err : %d",
            dev_id,
            conn_id,
            chnl_id,
            port_info->dev_port,
            log_mac_id,
            map_sts);
        return map_sts;
      }
    } else if (map_sts == BF_MAP_OK) {
      mac_multi_map->mac_chnl[log_mac_lane].num_serdes_per_lane =
          n_serdes_per_lane;
      for (i = 0; i < n_serdes_per_lane; i++) {
        mac_multi_map->mac_chnl[log_mac_lane].tx_lane[i] = mac_block.tx_lane[i];
        mac_multi_map->mac_chnl[log_mac_lane].rx_lane[i] = mac_block.rx_lane[i];
        mac_multi_map->mac_chnl[log_mac_lane].tx_inv[i] = mac_block.tx_inv[i];
        mac_multi_map->mac_chnl[log_mac_lane].rx_inv[i] = mac_block.rx_inv[i];
        mac_rx_pol[log_mac_id][chnl_id + i] = mac_block.rx_inv[i];
        mac_tx_pol[log_mac_id][chnl_id + i] = mac_block.tx_inv[i];
      }
    } else {
      PM_ERROR(
          "Unable to get mac block from map for dev : %d : port : %d/%d (%d) : "
          "MAC : %d : map_err : %d",
          dev_id,
          conn_id,
          chnl_id,
          port_info->dev_port,
          log_mac_id,
          map_sts);
      return map_sts;
    }

#if 0
    n_serdes_per_lane =
        mac_multi_map->mac_chnl[log_mac_lane].num_serdes_per_lane;
    for (i = 0; i < n_serdes_per_lane; i++) {
      PM_DEBUG(
          "Dev : %d port/chnl : %d/%d  mac_id/chnl : %d/%d dev-port : %d : lane : "
          "%d tx_lane %d tx_inv %d rx_lane %d rx_inv %d",
          dev_id,
          conn_id,
          chnl_id,
          log_mac_id,
          log_mac_lane,
          port_info->dev_port,
          i,
          mac_multi_map->mac_chnl[log_mac_lane].tx_lane[i],
          mac_multi_map->mac_chnl[log_mac_lane].tx_inv[i],
          mac_multi_map->mac_chnl[log_mac_lane].rx_lane[i],
          mac_multi_map->mac_chnl[log_mac_lane].rx_inv[i]);
    }
#endif
  }

  // Iterate through the MAC map
  unsigned long mac_id;
  PORT_MGR_LOCK(&mac_map_mtx[dev_id]);
  map_sts = bf_map_get_first(
      &bf_pm_mac_map_db[dev_id], &mac_id, (void **)&mac_multi_map);
  PORT_MGR_UNLOCK(&mac_map_mtx[dev_id]);
  if (map_sts != BF_MAP_OK) {
    PM_ERROR("Unable to get first mac from map for dev : %d : map_err : %d",
             dev_id,
             map_sts);
    return map_sts;
  }
  while (map_sts == BF_MAP_OK) {
    sts = bf_port_map_mac_to_dev_port(
        dev_id, mac_id, 0 /*MAC chnl*/, &mac_blk_lanes_info.dev_port);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to get the dev port for dev: %d : logical MAC_id/logical "
          "MAC_chnl %lu/%d : %s (%d)",
          dev_id,
          mac_id,
          0,
          bf_err_str(sts),
          sts);
      return sts;
    }
    uint32_t idx;
    for (int mac_chnl = 0;
         mac_chnl < lld_get_chnls_dev_port(dev_id, mac_blk_lanes_info.dev_port);
         mac_chnl++) {
      n_serdes_per_lane = mac_multi_map->mac_chnl[mac_chnl].num_serdes_per_lane;
      (n_serdes_per_lane > 1) ? (idx = mac_chnl << 1) : (idx = mac_chnl);
      for (i = 0; i < n_serdes_per_lane; i++) {
        mac_blk_lanes_info.rx_lane[idx + i] =
            mac_multi_map->mac_chnl[mac_chnl].rx_lane[i];
        mac_blk_lanes_info.tx_lane[idx + i] =
            mac_multi_map->mac_chnl[mac_chnl].tx_lane[i];
#if 1
        PM_DEBUG(
            "dev: %d : dev port : %d "
            "mac_id/chnl %lu/%d : rx_lane:%d tx-lane:%d mac-rx-lane %d "
            "mac-tx-lane %d",
            dev_id,
            mac_blk_lanes_info.dev_port,
            mac_id,
            mac_chnl,
            mac_blk_lanes_info.rx_lane[idx + i],
            mac_blk_lanes_info.tx_lane[idx + i],
            mac_multi_map->mac_chnl[mac_chnl].rx_lane[i],
            mac_multi_map->mac_chnl[mac_chnl].tx_lane[i]);
#endif
      }
    }

    port_info = pm_port_info_get(dev_id, mac_blk_lanes_info.dev_port);
    if (!port_info) {
      PM_ERROR("Unable to get the dev port info for dev(%d) dev_port(%d)",
               dev_id,
               mac_blk_lanes_info.dev_port);
      return BF_UNEXPECTED;
    }

    mac_blk_lanes_info.fp_conn_id = port_info->pltfm_port_info.port_hdl.conn_id;
    mac_blk_lanes_info.fp_chnl_id = port_info->pltfm_port_info.port_hdl.chnl_id;
    sts = bf_port_lane_map_set(dev_id, mac_id, &mac_blk_lanes_info);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to set the MAC to Serdes lane mapping for dev : %d : Logical "
          "MAC : %lu : %s (%d)",
          dev_id,
          mac_id,
          bf_err_str(sts),
          sts);
#if !defined(DEVICE_IS_EMULATOR)  // Emulator
      return sts;
#endif
    }
    PORT_MGR_LOCK(&mac_map_mtx[dev_id]);
    map_sts = bf_map_get_next(
        &bf_pm_mac_map_db[dev_id], &mac_id, (void **)&mac_multi_map);
    PORT_MGR_UNLOCK(&mac_map_mtx[dev_id]);
  }

  return BF_SUCCESS;
}

/**
 * This initializes the swizzled mac to serdes lane mappings. Needs to be
 * done one time during init
 */
static bf_status_t pm_mac_to_serdes_lane_map_set(bf_dev_id_t dev_id) {
  int mac_chnl = 0;
  unsigned long mac_id;
  bf_pal_front_port_handle_t *t_port_hdl;
  bf_pal_mac_to_serdes_lane_map_t mac_block;
  bf_status_t sts;
  bf_mac_block_lane_map_t mac_blk_lanes_info;
  bf_pm_port_info_t *port_info;
  bf_map_sts_t map_sts;
  bf_pm_mac_blk *mac_map;
  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));

  if (!pltfm_interface.pltfm_mac_to_serdes_map_get) {
    return BF_NOT_IMPLEMENTED;
  }

  if ((dev_family == BF_DEV_FAMILY_TOFINO3)) {
    return pm_mac_to_multi_serdes_lane_map_set(dev_id);
  }

  // Iterate through all the ports and fill in the MAC map
  port_info = pm_port_info_get_first_on_this(dev_id);
  for (; port_info != NULL;
       port_info = pm_port_info_get_next_on_this(dev_id, port_info)) {
    t_port_hdl = &port_info->pltfm_port_info.port_hdl;

    // Get the TX and RX serdes connected to this logical MAC block and channel
    sts = pltfm_interface.pltfm_mac_to_serdes_map_get(t_port_hdl, &mac_block);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to get the MAC to Serdes lane mapping for dev : %d : Front "
          "Port : %d/%d (%d) : %s (%d)",
          dev_id,
          t_port_hdl->conn_id,
          t_port_hdl->chnl_id,
          port_info->dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }

    PORT_MGR_LOCK(&mac_map_mtx[dev_id]);
    map_sts = bf_map_get(&bf_pm_mac_map_db[dev_id],
                         port_info->pltfm_port_info.log_mac_id,
                         (void *)&mac_map);
    PORT_MGR_UNLOCK(&mac_map_mtx[dev_id]);
    if (map_sts == BF_MAP_NO_KEY) {
      mac_map = (bf_pm_mac_blk *)bf_sys_calloc(1, 1 * sizeof(bf_pm_mac_blk));
      if (!mac_map) {
        PM_ERROR(
            "Unable to allocate space for mac blk for dev : %d : port : %d/%d "
            "(%d) : MAC : %d ",
            dev_id,
            port_info->pltfm_port_info.port_hdl.conn_id,
            port_info->pltfm_port_info.port_hdl.chnl_id,
            port_info->dev_port,
            port_info->pltfm_port_info.log_mac_id);
        return BF_NO_SYS_RESOURCES;
      }
      mac_map->mac_chnl = (bf_pal_mac_to_serdes_lane_map_t *)bf_sys_calloc(
          lld_get_chnls_dev_port(dev_id, port_info->dev_port),
          sizeof(bf_pal_mac_to_serdes_lane_map_t));
      if (!mac_map->mac_chnl) {
        PM_ERROR(
            "Unable to allocate space for mac chnls for dev : %d : port : "
            "%d/%d "
            "(%d) : MAC : %d : chnl : %d",
            dev_id,
            port_info->pltfm_port_info.port_hdl.conn_id,
            port_info->pltfm_port_info.port_hdl.chnl_id,
            port_info->dev_port,
            port_info->pltfm_port_info.log_mac_id,
            lld_get_chnls_dev_port(dev_id, port_info->dev_port));
        bf_sys_free(mac_map);
        return BF_NO_SYS_RESOURCES;
      }
      mac_map->mac_chnl[port_info->pltfm_port_info.log_mac_lane].tx_lane =
          mac_block.tx_lane;
      mac_map->mac_chnl[port_info->pltfm_port_info.log_mac_lane].rx_lane =
          mac_block.rx_lane;
      PORT_MGR_LOCK(&mac_map_mtx[dev_id]);
      map_sts = bf_map_add(&bf_pm_mac_map_db[dev_id],
                           port_info->pltfm_port_info.log_mac_id,
                           (void *)mac_map);
      PORT_MGR_UNLOCK(&mac_map_mtx[dev_id]);
      if (map_sts != BF_MAP_OK) {
        PM_ERROR(
            "Unable to add mac block to map for dev : %d : port : %d/%d (%d) : "
            "MAC : %d : map_err : %d",
            dev_id,
            port_info->pltfm_port_info.port_hdl.conn_id,
            port_info->pltfm_port_info.port_hdl.chnl_id,
            port_info->dev_port,
            port_info->pltfm_port_info.log_mac_id,
            map_sts);
        return map_sts;
      }
    } else if (map_sts == BF_MAP_OK) {
      mac_map->mac_chnl[port_info->pltfm_port_info.log_mac_lane].tx_lane =
          mac_block.tx_lane;
      mac_map->mac_chnl[port_info->pltfm_port_info.log_mac_lane].rx_lane =
          mac_block.rx_lane;
    } else {
      PM_ERROR(
          "Unable to get mac block from map for dev : %d : port : %d/%d (%d) : "
          "MAC : %d : map_err : %d",
          dev_id,
          port_info->pltfm_port_info.port_hdl.conn_id,
          port_info->pltfm_port_info.port_hdl.chnl_id,
          port_info->dev_port,
          port_info->pltfm_port_info.log_mac_id,
          map_sts);
      return map_sts;
    }
  }

  // Iterate through the MAC map
  PORT_MGR_LOCK(&mac_map_mtx[dev_id]);
  map_sts =
      bf_map_get_first(&bf_pm_mac_map_db[dev_id], &mac_id, (void **)&mac_map);
  PORT_MGR_UNLOCK(&mac_map_mtx[dev_id]);
  if (map_sts != BF_MAP_OK) {
    PM_ERROR("Unable to get first mac from map for dev : %d : map_err : %d",
             dev_id,
             map_sts);
    return map_sts;
  }
  while (map_sts == BF_MAP_OK) {
    sts = bf_port_map_mac_to_dev_port(
        dev_id, mac_id, 0 /*MAC chnl*/, &mac_blk_lanes_info.dev_port);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to get the dev port for dev: %d : logical MAC_id/logical "
          "MAC_chnl %lu/%d : %s (%d)",
          dev_id,
          mac_id,
          mac_chnl,
          bf_err_str(sts),
          sts);
      return sts;
    }
    for (mac_chnl = 0;
         mac_chnl < lld_get_chnls_dev_port(dev_id, mac_blk_lanes_info.dev_port);
         mac_chnl++) {
      mac_blk_lanes_info.rx_lane[mac_chnl] =
          mac_map->mac_chnl[mac_chnl].rx_lane;
      mac_blk_lanes_info.tx_lane[mac_chnl] =
          mac_map->mac_chnl[mac_chnl].tx_lane;
    }
    port_info = pm_port_info_get(dev_id, mac_blk_lanes_info.dev_port);
    if (!port_info) {
      PM_ERROR("Unable to get the dev port info for dev(%d) dev_port(%d)",
               dev_id,
               mac_blk_lanes_info.dev_port);
      return BF_UNEXPECTED;
    }
    mac_blk_lanes_info.fp_conn_id = port_info->pltfm_port_info.port_hdl.conn_id;
    mac_blk_lanes_info.fp_chnl_id = port_info->pltfm_port_info.port_hdl.chnl_id;
    sts = bf_port_lane_map_set(dev_id, mac_id, &mac_blk_lanes_info);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to set the MAC to Serdes lane mapping for dev : %d : Logical "
          "MAC : %lu : %s (%d)",
          dev_id,
          mac_id,
          bf_err_str(sts),
          sts);
#if !defined(DEVICE_IS_EMULATOR)  // Emulator
      return sts;
#endif
    }
    PORT_MGR_LOCK(&mac_map_mtx[dev_id]);
    map_sts =
        bf_map_get_next(&bf_pm_mac_map_db[dev_id], &mac_id, (void **)&mac_map);
    PORT_MGR_UNLOCK(&mac_map_mtx[dev_id]);
  }

  return BF_SUCCESS;
}

/**
 * Initialize a port to default values
 */
static void pm_port_info_init_this_port(bf_dev_id_t dev_id,
                                        bf_pm_port_info_t *port_info) {
  port_info->is_added = false;
  port_info->admin_state = PM_PORT_DISABLED;
  port_info->oper_status = PM_PORT_DOWN;
  port_info->speed = BF_SPEED_NONE;
  port_info->fec_type = BF_FEC_TYP_NONE;
  port_info->kr_policy = PM_KR_DEFAULT;
  port_info->an_policy = PM_AN_DEFAULT;

  memset(&port_info->curr_stats, 0, sizeof(port_info->curr_stats));

  // DRV-8354 old stats can not be cleared if persistent enabled.
  bool persistent_enable = false;
  bf_pm_port_stats_persistent_get(dev_id, &persistent_enable);
  if (!persistent_enable) {
    memset(&port_info->old_stats, 0, sizeof(port_info->old_stats));
  }

  memset(&port_info->fec_info, 0, sizeof(port_info->fec_info));
  port_info->last_dwn_time = -1;
  port_info->enb_req = false;
  port_info->lpbk_mode = BF_LPBK_NONE;
  port_info->disable_time = PORT_DISABLE_TIME_DURATION_SHORT;
  port_info->async_stats_update_request_issued = false;
  /* Reset stats timestamp to zero on port add.
     Same is verified during stats collection
     to help syncing to system time */
  port_info->stats_timestamp_sec = 0;        // timestamp sec for curr_stats
  port_info->stats_timestamp_nsec = 0;       // timestamp nsec for curr_stats
  port_info->prev_stats_timestamp_sec = 0;   // timestamp sec for prev_stats
  port_info->prev_stats_timestamp_nsec = 0;  // timestamp nsec for prev_stats
  port_info->dma_timestamp_nsec = 0;         // timestamp nsec for dma

  // for rate function
  port_info->rate_timestamp_sec = 0;
  port_info->rate_timestamp_nsec = 0;
  port_info->rx_octets_good =
      port_info->curr_stats.format.ctr_ids.OctetsReceivedinGoodFrames;
  port_info->tx_octets_good =
      port_info->curr_stats.format.ctr_ids.OctetsTransmittedwithouterror;
  port_info->rx_frame_good =
      port_info->curr_stats.format.ctr_ids.FramesReceivedOK;
  port_info->tx_frame_good =
      port_info->curr_stats.format.ctr_ids.FramesTransmittedOK;
  port_info->tx_rate = 0;
  port_info->rx_rate = 0;
  port_info->rx_pps = 0;
  port_info->tx_pps = 0;
  port_info->serdes_tx_ready = 0;
  port_info->serdes_rx_ready = 0;
  port_info->serdes_rx_ready_for_dfe = 0;
  port_info->module_ready_for_link = 0;
  port_info->prbs_mode = BF_PORT_PRBS_MODE_NONE;
  port_info->serdes_tx_eq_override = false;
  port_info->fsm_debounce_cnt = 0;
  port_info->fsm_debounce_thr = BF_PM_FSM_LINK_UP_THRSHLD_DEFAULT;

  for (int ln = 0; ln < MAX_LANES_PER_PORT; ln++) {
    port_info->pc_tx_policy[ln] = PM_PRECODING_INVALID;
    port_info->pc_rx_policy[ln] = PM_PRECODING_INVALID;
    port_info->term_mode[ln] = PM_TERM_MODE_DEFAULT;
    port_info->serdes_lane_tx_eq_override[ln] = false;
  }

  port_info->n_lanes = 0;
  if (!bf_pm_intf_is_device_family_tofino(port_info->pltfm_port_info.dev_id)) {
    // For accton based tofino systems, platforms
    // would send the port-ready before port gets
    // added. So we should not clear it.
    // But this can also mean, if port gets del/dis and
    // add/enb back, we could be running with stale
    // port-ready info w/o platform knowledge.
    // For now, retain the old behavior.
    port_info->is_ready_for_bringup = 0;
  }
}

/**
 * Initialize the global database of the ports for either
 * 1) a given device
 * 2) all devices (first tme init)
 * to default values
 */
static void pm_port_info_init(bf_dev_id_t dev_id) {
  bf_pm_port_info_t *port_info;

  if (!bf_pm_glb_db_initd) {
    bf_pm_glb_db_initd = true;
    // if never been initd, do all maps associated with all dev_ids
    port_info = pm_port_info_get_first(&dev_id);
    if (!port_info) return;
    for (; port_info != NULL;
         port_info = pm_port_info_get_next(dev_id, port_info, &dev_id)) {
      pm_port_info_init_this_port(dev_id, port_info);
    }
  } else {
    // just re-init the map of a single dev_id
    port_info = pm_port_info_get_first_on_this(dev_id);
    if (!port_info) return;
    for (; port_info != NULL;
         port_info = pm_port_info_get_next_on_this(dev_id, port_info)) {
      pm_port_info_init_this_port(dev_id, port_info);
    }
  }
}

/**
 * Remove the ports from the FSM pm
 */
static void pm_port_rmv_internal(bf_dev_id_t dev_id) {
  bf_pm_port_info_t *port_info;

  port_info = pm_port_info_get_first_on_this(dev_id);
  for (; port_info != NULL;
       port_info = pm_port_info_get_next_on_this(dev_id, port_info)) {
    pm_port_rmv(dev_id, port_info->dev_port, pm_dev_is_locked(dev_id));
  }
}

/**
 * Initialize the per port mutex for all the ports
 */
static void pm_port_info_mtx_init(bf_dev_id_t dev_id) {
  bf_pm_port_info_t *port_info;

  port_info = pm_port_info_get_first_on_this(dev_id);
  for (; port_info != NULL;
       port_info = pm_port_info_get_next_on_this(dev_id, port_info)) {
    bf_sys_rmutex_init(&port_info->port_mtx);
  }
}

/**
 * Deinitialize the per port mutex for all the ports
 */
static void pm_port_info_mtx_deinit(bf_dev_id_t dev_id) {
  bf_pm_port_info_t *port_info;

  port_info = pm_port_info_get_first_on_this(dev_id);
  for (; port_info != NULL;
       port_info = pm_port_info_get_next_on_this(dev_id, port_info)) {
    bf_sys_rmutex_del(&port_info->port_mtx);
  }
}

void bf_pm_fsm_queues_init(void) { pm_fsm_queues_init(); }

void bf_pm_set_dfe_retry_time(uint32_t wait_ms) {
  bf_fsm_set_dfe_retry_time(wait_ms);
}

uint32_t bf_pm_get_dfe_retry_time(void) { return bf_fsm_get_dfe_retry_time(); }

/**
 * Given a conn/chnl (port_hdl) return the associated:
 *  dev_id
 *  dev_port (on the above dev_id)
 *  port_info
 */
bf_status_t bf_pm_port_info_all_get(const char *caller,
                                    bf_dev_id_t exp_dev_id,
                                    bf_pal_front_port_handle_t *port_hdl,
                                    bf_pm_port_info_t **port_info,
                                    bf_dev_id_t *dev_id_of_port,
                                    bf_dev_port_t *dev_port) {
  bf_status_t sts;

  if (!port_hdl) return BF_INVALID_ARG;
  if (!port_info) return BF_INVALID_ARG;
  if (!dev_id_of_port) return BF_INVALID_ARG;
  if (!dev_port) return BF_INVALID_ARG;
  *port_info = NULL;
  *dev_id_of_port = 0xffffffff;  // make sure its invalid
  *dev_port = 0xffffffff;        // make sure its invalid

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, dev_id_of_port, dev_port);
  if (sts != BF_SUCCESS) {
    PM_ERROR("%s : failed to find %d/%d (for dev_port)",
             caller,
             port_hdl->conn_id,
             port_hdl->chnl_id);
    return sts;
  }
  if (exp_dev_id != *dev_id_of_port) {
    PM_DEBUG("%s: Warning : %d/%d found on dev%d : exp dev%d",
             caller,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             *dev_id_of_port,
             exp_dev_id);
    exp_dev_id = *dev_id_of_port;
  }
  *port_info = pm_port_info_get(exp_dev_id, *dev_port);
  if (*port_info == NULL) {
    PM_ERROR("failed to find %d/%d (for port_info, dev_id=%d, d_p=%d (%xh))",
             port_hdl->conn_id,
             port_hdl->chnl_id,
             exp_dev_id,
             *dev_port,
             *dev_port);
    return BF_NOT_IMPLEMENTED;
  }
  return BF_SUCCESS;
}

/**
 * Configure the serdes inv params for a port
 */
static bf_status_t pm_serdes_inv_param_set(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl) {
  int log_lane, num_lanes;
  bf_dev_port_t dev_port;
  bf_pal_serdes_info_t serdes_info;
  bf_pal_front_port_handle_t t_port_hdl;
  bf_pm_port_info_t *port_info;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;

  if (!pltfm_interface.pltfm_serdes_info_get) {
    return BF_NOT_IMPLEMENTED;
  }
  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  if (port_info->is_added) {
    num_lanes = port_info->n_lanes;
  } else {
    PM_ERROR("Port is not added dev : %d : front port : %d/%d (%d)",
             dev_id,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_port);
    return BF_NOT_IMPLEMENTED;
  }
  t_port_hdl.conn_id = port_hdl->conn_id;
  for (log_lane = 0; log_lane < num_lanes; log_lane++) {
    t_port_hdl.chnl_id = port_hdl->chnl_id + log_lane;

    sts = pltfm_interface.pltfm_serdes_info_get(&t_port_hdl, &serdes_info);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to get serdes info from platforms module for dev : %d : "
          "front port : %d/%d (%d) : %s (%d)",
          dev_id,
          t_port_hdl.conn_id,
          t_port_hdl.chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }

    sts = bf_serdes_rx_afe_inv_set(
        dev_id, dev_port, log_lane, serdes_info.rx_inv);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to set the serdes rx inv for dev : %d : front port : %d/%d "
          "(%d) : %s (%d)",
          dev_id,
          t_port_hdl.conn_id,
          t_port_hdl.chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }
    sts = bf_serdes_tx_drv_inv_set(
        dev_id, dev_port, log_lane, serdes_info.tx_inv);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to set the serdes tx inv for dev : %d : front port : %d/%d "
          "(%d) : %s (%d)",
          dev_id,
          t_port_hdl.conn_id,
          t_port_hdl.chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }
  }

  return BF_SUCCESS;
}

/**
 * Configure the serdes Tx Equalization Params
 */
static bf_status_t pm_serdes_tx_eq_param_set(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl) {
  int log_lane, num_lanes;
  bf_dev_port_t dev_port;
  bf_pal_serdes_info_t serdes_info;
  bf_pal_front_port_handle_t t_port_hdl;
  bf_pm_port_info_t *port_info;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;

  if (!pltfm_interface.pltfm_serdes_info_get) {
    return BF_NOT_IMPLEMENTED;
  }
  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  if (port_info->is_added) {
    num_lanes = port_info->n_lanes;
  } else {
    PM_ERROR("Port is not added dev : %d : front port : %d/%d (%d)",
             dev_id,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_port);
    return BF_NOT_IMPLEMENTED;
  }

  t_port_hdl.conn_id = port_hdl->conn_id;
  for (log_lane = 0; log_lane < num_lanes; log_lane++) {
    t_port_hdl.chnl_id = port_hdl->chnl_id + log_lane;

    sts = pltfm_interface.pltfm_serdes_info_get(&t_port_hdl, &serdes_info);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to get serdes info from platforms module for dev : %d : "
          "front port : %d/%d (%d) : %s (%d)",
          dev_id,
          t_port_hdl.conn_id,
          t_port_hdl.chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }

    if (bf_dev_type_to_family(lld_sku_get_dev_type(dev_id)) ==
        BF_DEV_FAMILY_TOFINO) {
      sts = bf_serdes_tx_drv_attn_set(dev_id,
                                      dev_port,
                                      log_lane,
                                      serdes_info.tx_attn,
                                      serdes_info.tx_post,
                                      serdes_info.tx_pre);
      if (sts != BF_SUCCESS) {
        PM_ERROR(
            "Unable to set the serdes Tx Eq params for dev : %d : front port : "
            "%d/%d (%d) : %s (%d)",
            dev_id,
            t_port_hdl.conn_id,
            t_port_hdl.chnl_id,
            dev_port,
            bf_err_str(sts),
            sts);
        return sts;
      }
    }
    // FIXME for Tofino2
  }

  return BF_SUCCESS;
}

/**
 * Configure the serdes Tx Eq override (custom settings)
 */
bf_status_t bf_pm_serdes_tx_eq_override_set(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl, bool override) {
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0xffffffff;  // make sure its invalid

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);

  if (sts != BF_SUCCESS) {
    PM_ERROR("%d/%d : (dev_id=%d) : Unable to get dev_port",
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id);
    return sts;
  }

  port_info = pm_port_info_get(dev_id, dev_port);
  if (port_info == NULL) {
    PM_ERROR("%d/%d : (dev_id=%d d_p=%d) : Unable to get port info",
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id,
             dev_port);
    return BF_INVALID_ARG;
  }

  if (port_info->is_added) {
    port_info->serdes_tx_eq_override = override;
  } else {
    PM_ERROR("%d/%d : (dev_id=%d d_p=%d) : not added",
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id,
             dev_port);
    return BF_INVALID_ARG;
  }

  return BF_SUCCESS;
}

/**
 * Configure the serdes lane Tx Eq override (per-lane custom settings)
 */
bf_status_t bf_pm_serdes_lane_tx_eq_override_set(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl,
    uint32_t ln,
    bool override) {
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0xffffffff;  // make sure its invalid

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);

  if (sts != BF_SUCCESS) {
    PM_ERROR("%d/%d : (dev_id=%d) : Unable to get dev_port",
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id);
    return sts;
  }

  port_info = pm_port_info_get(dev_id, dev_port);
  if (port_info == NULL) {
    PM_ERROR("%d/%d : (dev_id=%d d_p=%d) : Unable to get port info",
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id,
             dev_port);
    return BF_INVALID_ARG;
  }

  if (port_info->is_added) {
    if (ln < port_info->n_lanes) {
      port_info->serdes_lane_tx_eq_override[ln] = override;
    } else {
      PM_ERROR("%d/%d : (dev_id=%d d_p=%d) : valid lanes %d-%d",
               port_hdl->conn_id,
               port_hdl->chnl_id,
               dev_id,
               dev_port,
               0,
               port_info->n_lanes - 1);
      return BF_INVALID_ARG;
    }
  } else {
    PM_ERROR("%d/%d : (dev_id=%d d_p=%d) : not added",
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id,
             dev_port);
    return BF_INVALID_ARG;
  }

  return BF_SUCCESS;
}

/**
 * Return the state of the serdes/lane Tx Eq override (per-lane custom settings)
 */
bf_status_t bf_pm_serdes_lane_tx_eq_override_get(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl,
    uint32_t ln,
    bool *override) {
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0xffffffff;  // make sure its invalid

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);

  if (sts != BF_SUCCESS) {
    PM_ERROR("%d/%d : (dev_id=%d) : Unable to get dev_port",
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id);
    return sts;
  }

  port_info = pm_port_info_get(dev_id, dev_port);
  if (port_info == NULL) {
    PM_ERROR("%d/%d : (dev_id=%d d_p=%d) : Unable to get port info",
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id,
             dev_port);
    return BF_INVALID_ARG;
  }

  if (port_info->is_added) {
    *override = false;
    if (port_info->serdes_tx_eq_override) {
      *override = true;
    }
    if (ln < port_info->n_lanes) {
      if (port_info->serdes_lane_tx_eq_override[ln]) {
        *override = true;
      }
    } else {
      PM_ERROR("%d/%d : (dev_id=%d d_p=%d) : valid lanes %d-%d",
               port_hdl->conn_id,
               port_hdl->chnl_id,
               dev_id,
               dev_port,
               0,
               port_info->n_lanes - 1);
      return BF_INVALID_ARG;
    }
  } else {
    PM_ERROR("%d/%d : (dev_id=%d d_p=%d) : not added",
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id,
             dev_port);
    return BF_INVALID_ARG;
  }

  return BF_SUCCESS;
}

/**
 * Configure the serdes termination mode
 */
static bf_status_t pm_serdes_term_mode_set(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl) {
  int log_lane, num_lanes;
  bf_dev_port_t dev_port;
  bf_pal_front_port_handle_t t_port_hdl;
  bf_pm_port_info_t *port_info;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;
  bf_sds_rx_term_t tof1_term;
  bool tof2_term;
  bool tof3_term;
  char *term_str[] = {"DFLT", "GND", "AVDD", "FLOAT", "AC", "DC"};

  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  if (port_info->is_added) {
    num_lanes = port_info->n_lanes;
  } else {
    PM_ERROR("Port is not added dev : %d : front port : %d/%d (%d)",
             dev_id,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_port);
    return BF_NOT_IMPLEMENTED;
  }

  t_port_hdl.conn_id = port_hdl->conn_id;
  for (log_lane = 0; log_lane < num_lanes; log_lane++) {
    t_port_hdl.chnl_id = port_hdl->chnl_id;  // + log_lane;
    bf_pm_port_term_mode_e term_mode;

    sts = bf_pm_port_term_mode_get(dev_id, &t_port_hdl, log_lane, &term_mode);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to get serdes termination mode for dev : %d : "
          "front port : %d/%d (%d) : %s (%d)",
          dev_id,
          t_port_hdl.conn_id,
          t_port_hdl.chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }

    if (bf_dev_type_to_family(lld_sku_get_dev_type(dev_id)) ==
        BF_DEV_FAMILY_TOFINO) {
      switch (term_mode) {
        case PM_TERM_MODE_GND:
          tof1_term = BF_SDS_RX_TERM_GND;
          break;
        case PM_TERM_MODE_FLOAT:
          tof1_term = BF_SDS_RX_TERM_FLOAT;
          break;
        case PM_TERM_MODE_AVDD:
        case PM_TERM_MODE_DEFAULT:
        default:
          term_mode = PM_TERM_MODE_AVDD;
          tof1_term = BF_SDS_RX_TERM_AVDD;
          break;
      }
      sts = bf_serdes_rx_afe_term_set(dev_id, dev_port, log_lane, tof1_term);
    } else if (bf_dev_type_to_family(lld_sku_get_dev_type(dev_id)) ==
               BF_DEV_FAMILY_TOFINO2) {
      switch (term_mode) {
        case PM_TERM_MODE_DC:
          tof2_term = false;
          break;
        case PM_TERM_MODE_AC:
        case PM_TERM_MODE_DEFAULT:
        default:
          term_mode = PM_TERM_MODE_AC;
          tof2_term = true;
          break;
      }
      sts = bf_tof2_serdes_term_mode_set(dev_id, dev_port, log_lane, tof2_term);
    } else if (bf_dev_type_to_family(lld_sku_get_dev_type(dev_id)) ==
               BF_DEV_FAMILY_TOFINO3) {
      switch (term_mode) {
        case PM_TERM_MODE_DC:
          tof3_term = false;
          break;
        case PM_TERM_MODE_AC:
        case PM_TERM_MODE_DEFAULT:
        default:
          term_mode = PM_TERM_MODE_AC;
          tof3_term = true;
          break;
      }
      sts = bf_tof3_serdes_term_mode_set(dev_id, dev_port, log_lane, tof3_term);
    }
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to set the serdes termination mode for dev : %d : front "
          "port : "
          "%d/%d (%d) : %s (%d)",
          dev_id,
          t_port_hdl.conn_id,
          t_port_hdl.chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    } else {
      PM_DEBUG("Port termination setting for %d/%d.%d: %s",
               port_hdl->conn_id,
               port_hdl->chnl_id,
               log_lane,
               term_str[term_mode]);
    }
  }

  return BF_SUCCESS;
}

/**
 * Configure the serdes for a particular port
 */
static bf_status_t pm_serdes_config_set(bf_dev_id_t dev_id,
                                        bf_pal_front_port_handle_t *port_hdl) {
  bf_status_t sts;

  sts = pm_serdes_inv_param_set(dev_id, port_hdl);
  if (sts != BF_SUCCESS) {
    return sts;
  }

  sts = pm_serdes_tx_eq_param_set(dev_id, port_hdl);
  if (sts != BF_SUCCESS) {
    return sts;
  }

  sts = pm_serdes_term_mode_set(dev_id, port_hdl);
  if (sts != BF_SUCCESS) {
    return sts;
  }

  return BF_SUCCESS;
}

/**
 * Given a port speed, return the standard AN speeed which needs to be
 * advertised

  IEEE Tech Ability Field options

  BF_ADV_SPD_1000BASE_KX = (1 << 0),
  BF_ADV_SPD_10GBASE_KX4 = (1 << 1),  // not supported
  BF_ADV_SPD_10GBASE_KR = (1 << 2),
  BF_ADV_SPD_40GBASE_KR4 = (1 << 3),
  BF_ADV_SPD_40GBASE_CR4 = (1 << 4),
  BF_ADV_SPD_100GBASE_CR10 = (1 << 5),  // not supported
  BF_ADV_SPD_40GBASE_KP4 = (1 << 6),    // not supported
  BF_ADV_SPD_100GBASE_KR4 = (1 << 7),
  BF_ADV_SPD_100GBASE_CR4 = (1 << 8),
  BF_ADV_SPD_25GBASE_KRS_CRS = (1 << 9),  // note: no RS-FEC capability
  BF_ADV_SPD_25GBASE_KR_CR = (1 << 10),
  BF_ADV_SPD_2_5GBASE_KR = (1 << 11),
  BF_ADV_SPD_5GBASE_KR = (1 << 12),
  BF_ADV_SPD_50GBASE_KR1_CR1 = (1 << 13),
  BF_ADV_SPD_100GBASE_KR2_CR2 = (1 << 14),
  BF_ADV_SPD_200GBASE_KR4_CR4 = (1 << 15),
  BF_ADV_SPD_100GBASE_KR1_CR1 = (1 << 16),
  BF_ADV_SPD_200GBASE_KR2_CR2 = (1 << 17),
  BF_ADV_SPD_400GBASE_KR4_CR4 = (1 << 18),
 */
bf_an_adv_speeds_t pm_port_speed_to_an_speed_map(bf_dev_id_t dev_id,
                                                 bf_port_speeds_t p_speed,
                                                 uint32_t n_lanes,
                                                 bool adv_kr_mode,
                                                 bool rsfec_support) {
  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));
  bf_an_adv_speeds_t result = 0;
  /* Every port with more number of lanes should support speeds for advertising
   * with less number of lanes*/
  switch (n_lanes) {
    case 8:
      // no AN for 200G-R8 (NRZ)
      if (p_speed & BF_SPEED_400G) result |= BF_ADV_SPD_400GBASE_CR8_CONSORTIUM;
      break;
    case 4:
      if (p_speed & BF_SPEED_40G)
        result |= adv_kr_mode ? BF_ADV_SPD_40GBASE_KR4 : BF_ADV_SPD_40GBASE_CR4;
      if (p_speed & BF_SPEED_100G)
        result |=
            adv_kr_mode ? BF_ADV_SPD_100GBASE_KR4 : BF_ADV_SPD_100GBASE_CR4;
      if (p_speed & BF_SPEED_200G) result |= BF_ADV_SPD_200GBASE_KR4_CR4;
      if (p_speed & BF_SPEED_400G) result |= BF_ADV_SPD_400GBASE_KR4_CR4;
      break;
    case 2:
      /* 50G (2x25G) IEEE : autoneg not supported */
      if (p_speed & BF_SPEED_50G_CONS)
        result |= BF_ADV_SPD_50GBASE_CR2_CONSORTIUM |
                  BF_ADV_SPD_50GBASE_KR2_CONSORTIUM;
      if (p_speed & BF_SPEED_100G) result |= BF_ADV_SPD_100GBASE_KR2_CR2;
      if (p_speed & BF_SPEED_200G) result |= BF_ADV_SPD_200GBASE_KR2_CR2;
      break;
    case 1:
      if (p_speed & BF_SPEED_1G) result |= BF_ADV_SPD_1000BASE_KX;
      if (p_speed & BF_SPEED_10G) result |= BF_ADV_SPD_10GBASE_KR;
      if (p_speed & BF_SPEED_25G) {
        // by default 25GBASE_KRS_CRS is always advertised. However
        // 25GBASE_KR_CR is advertised ONLY if port supports RS FEC
        result |= BF_ADV_SPD_25GBASE_KRS_CRS;
        if (rsfec_support) result |= BF_ADV_SPD_25GBASE_KR_CR;

        if (dev_family != BF_DEV_FAMILY_TOFINO) {
          // Tofino 1 does not support 25G/50G Consortium
          result |= BF_ADV_SPD_25GBASE_KR1_CONSORTIUM |
                    BF_ADV_SPD_25GBASE_CR1_CONSORTIUM;
        }
      }
      if (p_speed & BF_SPEED_50G) result |= BF_ADV_SPD_50GBASE_KR1_CR1;
      if (p_speed & BF_SPEED_100G) result |= BF_ADV_SPD_100GBASE_KR1_CR1;
      break;
    default:
      PM_ERROR("Invalid number of lanes to parce advertised speeds %d",
               n_lanes);
  }

  return result;
}

bf_an_adv_speeds_t bf_pm_get_an_adv_speed(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port,
                                          bf_port_speed_t speed,
                                          uint32_t n_lanes) {
  bf_pm_port_info_t *port_info = NULL;
  bf_pm_port_kr_mode_policy_e kr_policy = PM_KR_DEFAULT;
  bool is_cpu_port = false, adv_kr_mode = false;
  bool rsfec_support = true;

  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));
  is_cpu_port = pm_port_is_cpu_port(dev_id, dev_port);
  port_info = pm_port_info_get(dev_id, dev_port);
  if (port_info == NULL) {
    PM_ERROR("Unable to get port info for dev : %d : dev-port : %d",
             dev_id,
             dev_port);
    return 0;
  }

  kr_policy = port_info->kr_policy;
  if (kr_policy == PM_KR_FORCE_ENABLE) {
    adv_kr_mode = true;
  } else if (kr_policy == PM_KR_FORCE_DISABLE) {
    adv_kr_mode = false;
  } else if (is_cpu_port) {
    adv_kr_mode = true;
  } else {
    adv_kr_mode = false;
  }

  if ((dev_family == BF_DEV_FAMILY_TOFINO) && (speed == BF_SPEED_25G) &&
      is_cpu_port)
    rsfec_support = false;

  return pm_port_speed_to_an_speed_map(
      dev_id, speed, n_lanes, adv_kr_mode, rsfec_support);
}

/**
 * Given a port fec, return the standard AN fec which needs to be
 * advertised
 */
bf_an_fecs_t pm_port_fec_to_an_fec_map(bf_fec_types_t p_fec,
                                       bf_port_speeds_t p_speed) {
  bf_an_fecs_t result = 0;
  if (p_speed & BF_SPEED_10G || p_speed & BF_SPEED_40G) {
    if (p_fec & BF_FEC_TYP_FIRECODE)
      result |= BF_ADV_FEC_FC_10G_ABILITY_IEEE | BF_ADV_FEC_FC_10G_REQUEST_IEEE;
  }
  if (p_speed & BF_SPEED_25G) {
    if (p_fec & BF_FEC_TYP_FIRECODE)
      result |= BF_ADV_FEC_FC_25G_REQUEST_IEEE |
                BF_ADV_FEC_FC_ABILITY_CONSORTIUM |
                BF_ADV_FEC_FC_REQUEST_CONSORTIUM |
                BF_ADV_FEC_FC_10G_ABILITY_IEEE | BF_ADV_FEC_FC_10G_REQUEST_IEEE;
    else if (p_fec & BF_FEC_TYP_REED_SOLOMON)
      result |= BF_ADV_FEC_RS_25G_REQUEST_IEEE |
                BF_ADV_FEC_RS_ABILITY_CONSORTIUM |
                BF_ADV_FEC_RS_REQUEST_CONSORTIUM;
  }
  if (p_speed & BF_SPEED_50G_CONS) {
    if (p_fec & BF_FEC_TYP_FIRECODE)
      result |=
          BF_ADV_FEC_FC_ABILITY_CONSORTIUM | BF_ADV_FEC_FC_REQUEST_CONSORTIUM;
    else if (p_fec & BF_FEC_TYP_REED_SOLOMON)
      result |=
          BF_ADV_FEC_RS_ABILITY_CONSORTIUM | BF_ADV_FEC_RS_REQUEST_CONSORTIUM;
  }
  return result;
}

/**
 * Returns if the port should Auto-Negotiate or not
 */
static bool pm_port_should_autoneg(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  bf_pm_port_info_t *port_info;

  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) {
    PM_ERROR("%s: Warning: : invalid input dev-port %d/%d ",
             __func__,
             dev_id,
             dev_port);
    return false;
  }

  if (port_info->an_policy == PM_AN_DEFAULT) {
    return port_info->is_an_eligible;
  } else if (port_info->an_policy == PM_AN_FORCE_ENABLE) {
    return true;
  } else {
    return false;
  }

  return false;
}

/**
 * Enable Autonegotation for a port if desired by the user
 */
static void pm_port_an_apply(bf_dev_id_t dev_id,
                             bf_pal_front_port_handle_t *port_hdl) {
  bf_status_t sts;
  bf_pm_port_info_t *port_info;
  bool do_an = false;
  bf_pause_cfg_t tx_pause = BF_PAUSE_CFG_DISABLE;
  bf_pause_cfg_t rx_pause = BF_PAUSE_CFG_DISABLE;
  bf_dev_id_t dev_id_of_port = 0;
  bf_an_adv_speeds_t adv_speed_map = 0;
  bf_an_fecs_t adv_fec_map = 0;
  bool rsfec_support = true;

  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));
  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  do_an = pm_port_should_autoneg(dev_id, port_info->dev_port);

  if (do_an) {
    bool is_cpu_port = pm_port_is_cpu_port(dev_id, port_info->dev_port);
    bf_pm_port_kr_mode_policy_e kr_policy;
    bool adv_kr_mode = false;

    sts = bf_pm_port_kr_mode_get(dev_id, port_hdl, &kr_policy);
    if (sts) {
      PM_ERROR(
          "Unable to get KR Mode for dev : %d : front port : %d/%d : assume "
          "default",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id);
      kr_policy = PM_KR_DEFAULT;
    }
    if (kr_policy == PM_KR_FORCE_ENABLE) {
      adv_kr_mode = true;
    } else if (kr_policy == PM_KR_FORCE_DISABLE) {
      adv_kr_mode = false;
    } else if (is_cpu_port) {
      adv_kr_mode = true;
    } else {
      adv_kr_mode = false;
    }

    if ((dev_family == BF_DEV_FAMILY_TOFINO) &&
        (port_info->speed == BF_SPEED_25G) && is_cpu_port)
      rsfec_support = false;

    // If the advertised speed(s) and fec(s) was set by the user, use it for
    // advertising Otherwise, use the port speed and fec for advertising
    adv_speed_map = pm_port_speed_to_an_speed_map(
        dev_id,
        port_info->adv_speed_map ? port_info->adv_speed_map : port_info->speed,
        port_info->n_lanes,
        adv_kr_mode,
        rsfec_support);

    adv_fec_map = pm_port_fec_to_an_fec_map(
        port_info->adv_fec_map ? port_info->adv_fec_map : port_info->fec_type,
        port_info->adv_speed_map ? port_info->adv_speed_map : port_info->speed);
    // In case FEC type is not applicable for a particular speed, the
    // function returns 0. As a result, the bitmask will not be affected.

    pm_port_set_autoneg_parms(dev_id,
                              port_info->dev_port,
                              adv_speed_map,
                              adv_fec_map,
                              tx_pause,
                              rx_pause);

    // Start auto negotiating
    if ((sts = pm_port_set_an(dev_id, port_info->dev_port, true)) !=
        BF_SUCCESS) {
      PM_ERROR(
          "Unable to start auto-negotiation for dev : %d : front port : %d/%d "
          ": %s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          bf_err_str(sts),
          sts);
    }
  } else {
    // Stop auto negotiating
    if ((sts = pm_port_set_an(dev_id, port_info->dev_port, false)) !=
        BF_SUCCESS) {
      PM_ERROR(
          "Unable to stop auto-negotiation for dev : %d : front port : %d/%d : "
          "%s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          bf_err_str(sts),
          sts);
    }
  }
}

static bf_status_t bf_pm_get_encoding_type(bf_port_speed_t speed,
                                           uint32_t n_lanes,
                                           bf_pm_encoding_type_t *enc_type) {
  if (!enc_type) return BF_INVALID_ARG;

  *enc_type = BF_ENCODING_NRZ;

  switch (speed) {
    case BF_SPEED_400G:
      *enc_type = BF_ENCODING_PAM4;
      return BF_SUCCESS;
      break;
    case BF_SPEED_200G:
      if (n_lanes == 8) {
        *enc_type = BF_ENCODING_NRZ;
        return BF_SUCCESS;
      } else if (n_lanes == 4) {
        *enc_type = BF_ENCODING_PAM4;
        return BF_SUCCESS;
      }
      break;
    case BF_SPEED_100G:
      if (n_lanes == 2) {
        *enc_type = BF_ENCODING_PAM4;
        return BF_SUCCESS;
      } else if (n_lanes == 4) {
        *enc_type = BF_ENCODING_NRZ;
        return BF_SUCCESS;
      }
      break;
    case BF_SPEED_50G:
      if (n_lanes == 1) {
        *enc_type = BF_ENCODING_PAM4;
        return BF_SUCCESS;
      } else if (n_lanes == 2) {
        *enc_type = BF_ENCODING_NRZ;
        return BF_SUCCESS;
      }
      break;
    case BF_SPEED_50G_CONS:
    case BF_SPEED_40G_R2:
    case BF_SPEED_40G:
    case BF_SPEED_25G:
    case BF_SPEED_10G:
    case BF_SPEED_1G:
      *enc_type = BF_ENCODING_NRZ;
      return BF_SUCCESS;
    case BF_SPEED_NONE:
      return BF_SUCCESS;
    default:
      break;
  }
  return BF_INVALID_ARG;
}

/**
 * Initialize the port cfg to be sent to the platforms module
 */
static void pm_port_cfg_set(bf_dev_id_t dev_id,
                            bf_dev_port_t dev_port,
                            bf_pal_front_port_cb_cfg_t *port_cfg) {
  bf_pm_port_info_t *port_info;

  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) {
    memset(port_cfg, 0, sizeof(*port_cfg));
    return;
  }

  port_cfg->speed_cfg = port_info->speed;
  port_cfg->num_lanes = port_info->n_lanes;
  // AN and port-dir are NOT part of port-add and will not take affect
  // during pre/port-add cb to platforms
  port_cfg->is_an_on = pm_port_should_autoneg(dev_id, dev_port);
  port_cfg->port_dir = port_info->port_dir;
  bf_pm_get_encoding_type(
      port_info->speed, port_info->n_lanes, &port_cfg->enc_type);
}

/**
 * Bring up a port by starting the FSM
 */
static bf_status_t pm_port_bring_up(bf_dev_id_t dev_id,
                                    bf_pal_front_port_handle_t *port_hdl) {
  bf_status_t sts = BF_SUCCESS;
  bf_pm_port_info_t *port_info;
  bf_pal_front_port_cb_cfg_t port_cfg = {0};
  bf_dev_id_t dev_id_of_port = 0;
  bool is_sw_model;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  sts = bf_drv_device_type_get(dev_id, &is_sw_model);
  if (sts != BF_SUCCESS) {
    PM_ERROR("Unable to get the device type for dev : %d : %s (%d)",
             dev_id,
             bf_err_str(sts),
             sts);
  }

  // Initialize the port cfg to be passed to the platform module
  pm_port_cfg_set(dev_id, port_info->dev_port, &port_cfg);

  // This function gets called via bf_pm_pltfm_front_port_ready_for_bringup
  // and port-enable. Pltfm vectors needs to be called only during admin-up
  // control.
  // For tof2 - pltfm vector gets called during admin-control - skip here.
  if (bf_pm_intf_is_device_family_tofino(dev_id)) {
    // Call the pre-port-enable platform cfg set interface defined by the
    // platforms module
    if (pltfm_interface.pltfm_pre_port_enable_cfg_set &&
        !port_info->is_internal) {
      sts = pltfm_interface.pltfm_pre_port_enable_cfg_set(port_hdl, &port_cfg);
      if (sts != BF_SUCCESS) {
        PM_ERROR(
            "Error in setting pltfm pre-port-enable cfg for dev : %d : front "
            "port : %d/%d (%d) : %s (%d)",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            port_info->dev_port,
            bf_err_str(sts),
            sts);
      }
    }

    if ((port_info->lpbk_mode != BF_LPBK_NONE) &&
        (port_info->speed != BF_SPEED_100G)) {
      PM_TRACE(
          "WARNING: %d : %d/%d All channels/ports in the quad must be enabled",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id);
    }
  }

  if ((!port_info->is_ready_for_bringup) &&
      (port_info->lpbk_mode == BF_LPBK_NONE)) {
    // Indicates that the port is not ready to be brought up just yet
    return BF_NOT_READY;
  }

  if (!is_sw_model) {
    if ((bf_pm_intf_is_device_family_tofino2(dev_id)) ||
        (bf_pm_intf_is_device_family_tofino3(dev_id))) {
      // Apply precoding settings
      pm_port_precoding_apply(dev_id, port_hdl);
      // Apply termination settings
      pm_port_term_mode_apply(dev_id, port_hdl);
    }
    // Apply AN if desired
    pm_port_an_apply(dev_id, port_hdl);
  }

  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    sts = pm_port_select_port_fsm_mode(dev_id, port_hdl);
    if (sts != BF_SUCCESS) {
      return sts;
    }
  }
  sts = pm_port_enable(dev_id, port_info->dev_port);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Error in SDE port enable for dev : %d : front port : %d/%d (%d) : %s "
        "(%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        port_info->dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  if (bf_pm_intf_is_device_family_tofino(dev_id)) {
    // Call the post-port-enable platform cfg set interface defined by the
    // platforms module
    if (pltfm_interface.pltfm_post_port_enable_cfg_set &&
        !port_info->is_internal) {
      sts = pltfm_interface.pltfm_post_port_enable_cfg_set(port_hdl, &port_cfg);
      if (sts != BF_SUCCESS) {
        PM_ERROR(
            "Error in setting pltfm post-port-enable cfg for dev : %d : front "
            "port : %d/%d (%d) : %s (%d)",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            port_info->dev_port,
            bf_err_str(sts),
            sts);
      }
    }

    if (!port_info->is_internal && (port_info->lpbk_mode == BF_LPBK_MAC_NEAR)) {
      for (uint log_lane = 0; log_lane < port_info->n_lanes; log_lane++) {
        sts = bf_serdes_lane_init_run(
            dev_id,
            port_info->dev_port,
            log_lane,
            pm_port_speed_to_sds_line_rate_map(port_info->speed),
            false,
            true,  // tx-only
            false,
            false);
        if (sts != BF_SUCCESS) {
          PM_ERROR(
              "Unable to init serdes to default values for dev : %d : front "
              "port "
              ": %d/%d (%d) : %s (%d)",
              dev_id,
              port_hdl->conn_id,
              port_hdl->chnl_id,
              port_info->dev_port,
              bf_err_str(sts),
              sts);
          return sts;
        }
      }
    }
  }

  return BF_SUCCESS;
}

/**
 * Bring down the port by disabling it
 */
static bf_status_t pm_port_bring_down(bf_dev_id_t dev_id,
                                      bf_pal_front_port_handle_t *port_hdl) {
  bf_status_t sts = BF_SUCCESS;
  bf_pm_port_info_t *port_info;
  bf_pal_front_port_cb_cfg_t port_cfg = {0};
  bf_dev_id_t dev_id_of_port = 0;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  // This function gets called via bf_pm_pltfm_front_port_ready_for_bringup
  // and port-disable. Pltfm vectors needs to be called only during admin-down
  // control.
  // For tof2, pltfm disable is called via admin-disable control
  if (bf_pm_intf_is_device_family_tofino(dev_id)) {
    // Initialize the port cfg to be passed to the platform module
    pm_port_cfg_set(dev_id, port_info->dev_port, &port_cfg);

    if (pltfm_interface.pltfm_pre_port_disable_cfg_set &&
        !port_info->is_internal) {
      sts = pltfm_interface.pltfm_pre_port_disable_cfg_set(port_hdl, &port_cfg);
      if (sts != BF_SUCCESS) {
        PM_ERROR(
            "Error in setting pltfm pre-port-disable cfg for dev : %d : front "
            "port : %d/%d (%d) : %s (%d)",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            port_info->dev_port,
            bf_err_str(sts),
            sts);
      }
    }
    if ((port_info->lpbk_mode != BF_LPBK_NONE) &&
        (port_info->speed != BF_SPEED_100G)) {
      PM_TRACE(
          "WARNING: %d : %d/%d All channels/ports in the quad must be disabled",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id);
    }
  }

  // Call the SDE port disable function
  sts = pm_port_disable(dev_id, port_info->dev_port);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Error in SDE port disable for dev : %d : front port : %d/%d (%d) : %s "
        "(%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        port_info->dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  // Call the post-port-disable platform cfg set interface defined by the
  // platforms module
  if (bf_pm_intf_is_device_family_tofino(dev_id)) {
    if (pltfm_interface.pltfm_post_port_disable_cfg_set &&
        !port_info->is_internal) {
      sts =
          pltfm_interface.pltfm_post_port_disable_cfg_set(port_hdl, &port_cfg);
      if (sts != BF_SUCCESS) {
        PM_ERROR(
            "Error in setting pltfm post-port-disable cfg for dev : %d : front "
            "port : %d/%d (%d) : %s (%d)",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            port_info->dev_port,
            bf_err_str(sts),
            sts);
      }
    }
  }

  return BF_SUCCESS;
}

/* Sequence to place an existing port into MAC-Near loopback. */
static bf_status_t pm_internal_port_lb_cfg_tof1_seq(
    bf_dev_id_t dev_id, bf_pm_port_info_t *port_info) {
  bf_status_t sts = BF_SUCCESS;

  /* Port mode transition SW workaround */
  sts = bf_drv_complete_port_mode_transition_wa(
      dev_id, port_info->dev_port, port_info->speed);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to complete the sw workaround for internal port in loopback "
        "for dev : %d : front port : "
        "%d/%d (%d) : MAC : %d/%d : %s (%d)",
        dev_id,
        port_info->pltfm_port_info.port_hdl.conn_id,
        port_info->pltfm_port_info.port_hdl.chnl_id,
        port_info->dev_port,
        port_info->pltfm_port_info.log_mac_id,
        port_info->pltfm_port_info.log_mac_lane,
        bf_err_str(sts),
        sts);
    return sts;
  }

  // Enable the port
  sts = bf_pm_port_enable(dev_id, &port_info->pltfm_port_info.port_hdl);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to enable internal port for dev : %d : front port : %d/%d (%d) "
        ": MAC : %d/%d : %s (%d)",
        dev_id,
        port_info->pltfm_port_info.port_hdl.conn_id,
        port_info->pltfm_port_info.port_hdl.chnl_id,
        port_info->dev_port,
        port_info->pltfm_port_info.log_mac_id,
        port_info->pltfm_port_info.log_mac_lane,
        bf_err_str(sts),
        sts);
    return sts;
  }

  // Clear loopback
  sts = bf_pm_port_loopback_mode_set(
      dev_id, &port_info->pltfm_port_info.port_hdl, BF_LPBK_NONE);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to clear loopback for internal port for dev : %d : front port "
        ": "
        "%d/%d (%d) : MAC : %d/%d : %s (%d)",
        dev_id,
        port_info->pltfm_port_info.port_hdl.conn_id,
        port_info->pltfm_port_info.port_hdl.chnl_id,
        port_info->dev_port,
        port_info->pltfm_port_info.log_mac_id,
        port_info->pltfm_port_info.log_mac_lane,
        bf_err_str(sts),
        sts);
    return sts;
  }

  // Put the port in loopback
  sts = bf_pm_port_loopback_mode_set(
      dev_id, &port_info->pltfm_port_info.port_hdl, BF_LPBK_MAC_NEAR);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to put internal port in loopback for dev : %d : front port : "
        "%d/%d (%d) : MAC : %d/%d : %s (%d)",
        dev_id,
        port_info->pltfm_port_info.port_hdl.conn_id,
        port_info->pltfm_port_info.port_hdl.chnl_id,
        port_info->dev_port,
        port_info->pltfm_port_info.log_mac_id,
        port_info->pltfm_port_info.log_mac_lane,
        bf_err_str(sts),
        sts);
    return sts;
  }
  port_info->lpbk_mode = BF_LPBK_MAC_NEAR;

  PM_DEBUG(
      "Internal port MAC loopback set success for dev : %d : front port : "
      "%d/%d (%d) : MAC : %d/%d",
      dev_id,
      port_info->pltfm_port_info.port_hdl.conn_id,
      port_info->pltfm_port_info.port_hdl.chnl_id,
      port_info->dev_port,
      port_info->pltfm_port_info.log_mac_id,
      port_info->pltfm_port_info.log_mac_lane);
  return sts;
}

/* Sequence to place an existing port into Pipe loopback on Tofino-2 (and
 * later). */
static bf_status_t pm_internal_port_lb_cfg_tof2_seq(
    bf_dev_id_t dev_id, bf_pm_port_info_t *port_info) {
  bf_status_t sts = BF_SUCCESS;
  // Clear loopback
  sts = bf_pm_port_loopback_mode_set(
      dev_id, &port_info->pltfm_port_info.port_hdl, BF_LPBK_NONE);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to clear loopback for internal port for dev : %d : front port "
        ": %d/%d (%d) : MAC : %d/%d : %s (%d)",
        dev_id,
        port_info->pltfm_port_info.port_hdl.conn_id,
        port_info->pltfm_port_info.port_hdl.chnl_id,
        port_info->dev_port,
        port_info->pltfm_port_info.log_mac_id,
        port_info->pltfm_port_info.log_mac_lane,
        bf_err_str(sts),
        sts);
    return sts;
  }

  // Put the port in loopback, use pipe level loopback by default.
  bf_loopback_mode_e lpbk_mode = BF_LPBK_PIPE;
  sts = bf_pm_port_loopback_mode_set(
      dev_id, &port_info->pltfm_port_info.port_hdl, lpbk_mode);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to put internal port in loopback for dev : %d : front port : "
        "%d/%d (%d) : MAC : %d/%d : %s (%d)",
        dev_id,
        port_info->pltfm_port_info.port_hdl.conn_id,
        port_info->pltfm_port_info.port_hdl.chnl_id,
        port_info->dev_port,
        port_info->pltfm_port_info.log_mac_id,
        port_info->pltfm_port_info.log_mac_lane,
        bf_err_str(sts),
        sts);
    return sts;
  }
  port_info->lpbk_mode = lpbk_mode;

  // Enable the port
  sts = bf_pm_port_enable(dev_id, &port_info->pltfm_port_info.port_hdl);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to enable internal port for dev : %d : front port : %d/%d (%d) "
        ": MAC : %d/%d : %s (%d)",
        dev_id,
        port_info->pltfm_port_info.port_hdl.conn_id,
        port_info->pltfm_port_info.port_hdl.chnl_id,
        port_info->dev_port,
        port_info->pltfm_port_info.log_mac_id,
        port_info->pltfm_port_info.log_mac_lane,
        bf_err_str(sts),
        sts);
    return sts;
  }

  PM_DEBUG(
      "Internal port PIPE loopback set success for dev : %d : front port : "
      "%d/%d (%d) : MAC : %d/%d",
      dev_id,
      port_info->pltfm_port_info.port_hdl.conn_id,
      port_info->pltfm_port_info.port_hdl.chnl_id,
      port_info->dev_port,
      port_info->pltfm_port_info.log_mac_id,
      port_info->pltfm_port_info.log_mac_lane);
  return sts;
}
/**
 * Put an internal port (which doesn't have serdes) in MAC/Pipe lpbk
 */
static bf_status_t pm_add_internal_port_in_lb_mode(
    bf_dev_id_t dev_id,
    bf_pm_port_info_t *port_info,
    bf_port_speed_t internal_port_speed,
    uint32_t internal_port_n_lanes,
    bf_fec_type_t fec,
    bool errors_expected) {
  bf_status_t sts;
  bool is_valid_port;

  if (!port_info) return BF_INVALID_ARG;
  if (!port_info->is_internal) return BF_SUCCESS;
  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));

  pm_port_info_init_this_port(dev_id, port_info);

  is_valid_port =
      pm_port_valid_speed_and_channel_internal(dev_id,
                                               port_info->dev_port,
                                               internal_port_speed,
                                               internal_port_n_lanes,
                                               fec,
                                               errors_expected);
  if (!is_valid_port) return BF_SUCCESS;

  PM_DEBUG(
      "Port for dev : %d : front port : %d/%d (%d) : "
      "MAC : %d/%d : is_internal : %s",
      dev_id,
      port_info->pltfm_port_info.port_hdl.conn_id,
      port_info->pltfm_port_info.port_hdl.chnl_id,
      port_info->dev_port,
      port_info->pltfm_port_info.log_mac_id,
      port_info->pltfm_port_info.log_mac_lane,
      port_info->is_internal ? "YES" : "NO");

  // Call the SDE port add function
  pm_port_exclusive_access_start(dev_id, port_info);
  port_info->speed = internal_port_speed;
  port_info->n_lanes = internal_port_n_lanes;
  sts = pm_port_add(dev_id,
                    port_info->dev_port,
                    internal_port_speed,
                    internal_port_n_lanes,
                    fec);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to add internal port for dev : %d : front port : %d/%d "
        "(%d) : %s (%d)",
        dev_id,
        port_info->pltfm_port_info.port_hdl.conn_id,
        port_info->pltfm_port_info.port_hdl.chnl_id,
        port_info->dev_port,
        bf_err_str(sts),
        sts);
    goto end;
  }
  port_info->is_added = true;

  // Mark this port so that the non-serdes FSM is used to bring it up
  // Internal ports don't have serdes and hence we never touch them
  sts = pm_port_skip_serdes_fsm_set(port_info->dev_port, true);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to skip serdes fsm for dev : %d : front port : %d/%d (%d) : %s "
        "(%d)",
        dev_id,
        port_info->pltfm_port_info.port_hdl.conn_id,
        port_info->pltfm_port_info.port_hdl.chnl_id,
        port_info->dev_port,
        bf_err_str(sts),
        sts);
    goto end;
  }

  if (dev_family == BF_DEV_FAMILY_TOFINO) {
    sts = pm_internal_port_lb_cfg_tof1_seq(dev_id, port_info);
  } else {
    sts = pm_internal_port_lb_cfg_tof2_seq(dev_id, port_info);
  }

end:
  pm_port_exclusive_access_end(dev_id, port_info);
  return sts;
}

/**
 * If the chip has internal pipes(eg. 32Q), add all the internal ports (which
 * don't have serdes) in MAC lpbk
 */
static bf_status_t pm_internal_port_lpbk_set(bf_dev_id_t dev_id) {
  uint32_t internal_pipes;
  bf_status_t sts;
  bf_pm_port_info_t *port_info;

  // If the internal ports are tracking the front panel ports then do not add
  // them in loopback here, they will be created as front panel ports are added.
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;

  internal_pipes = lld_get_internal_pipe_numb(dev_id);
  if (internal_pipes == 0) {
    // No internal pipe
    return BF_SUCCESS;
  }

  bf_port_speed_t internal_port_speed;
  uint32_t internal_port_n_lanes;
  bf_fec_type_t fec;

  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));
  switch (dev_family) {
    case BF_DEV_FAMILY_TOFINO2:
      internal_port_speed = BF_SPEED_400G;
      internal_port_n_lanes = 8;
      fec = BF_FEC_TYP_REED_SOLOMON;
      break;
    case BF_DEV_FAMILY_TOFINO3:
      internal_port_speed = BF_SPEED_400G;
      internal_port_n_lanes = 4;
      fec = BF_FEC_TYP_REED_SOLOMON;
      break;
    case BF_DEV_FAMILY_TOFINO:
      internal_port_speed = BF_SPEED_100G;
      internal_port_n_lanes = 4;
      fec = BF_FEC_TYP_NONE;
      break;
    default:
      PM_ERROR("Error dev family for dev %d", dev_id);
      return BF_INVALID_ARG;
  }

  port_info = pm_port_info_get_first_on_this(dev_id);
  for (; port_info != NULL;
       port_info = pm_port_info_get_next_on_this(dev_id, port_info)) {
    sts = pm_add_internal_port_in_lb_mode(dev_id,
                                          port_info,
                                          internal_port_speed,
                                          internal_port_n_lanes,
                                          fec,
                                          true);
    if (sts != BF_SUCCESS) {
      return sts;
    }
  }
  return BF_SUCCESS;
}

/**
 * Callback triggered by the port stats poll timer
 */
static void pm_port_stats_timer_cb(struct bf_sys_timer_s *timer, void *data) {
  bf_dev_id_t dev_id;
  bf_pm_port_info_t *port_info;

  dev_id = (bf_dev_id_t)(intptr_t)data;
  port_info = pm_port_info_get_first_on_this(dev_id);
  for (; port_info != NULL;
       port_info = pm_port_info_get_next_on_this(dev_id, port_info)) {
    bf_pm_port_all_stats_update(dev_id, &port_info->pltfm_port_info.port_hdl);
  }
  (void)timer;
}

/**
 * Update the port stats poll timer
 */
bf_status_t bf_pm_port_stats_poll_period_update(bf_dev_id_t dev_id,
                                                uint32_t poll_intv_ms) {
  bf_sys_timer_status_t rc;
  bf_status_t sts = ~BF_SUCCESS;
  bool is_sw_model = true;
  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (poll_intv_ms < PORT_STATS_POLL_MIN_TMR_PERIOD_MS) {
    return BF_INVALID_ARG;
  }
  sts = bf_drv_device_type_get(dev_id, &is_sw_model);
  if (sts != BF_SUCCESS) {
    PM_ERROR("Unable to get the device type for dev : %d : %s (%d)",
             dev_id,
             bf_err_str(sts),
             sts);
  }
  if (is_sw_model) {
    PM_ERROR("MAC stats polling timer is disabled on model, dev %d", dev_id);
    return BF_SUCCESS;
  }
  if (bf_pm_dev_locked[dev_id]) {
    // only update timer intvl shadow
    port_stats_timer_intvl[dev_id] = poll_intv_ms;
    return BF_SUCCESS;
  }
  if (port_stats_timer[dev_id].cb_fn) {
    if (port_stats_timer_started[dev_id] &&
        (port_stats_timer_intvl[dev_id] == poll_intv_ms)) {
      return BF_SUCCESS;
    }
    rc = bf_sys_timer_del(&port_stats_timer[dev_id]);
    if (rc) {
      PM_ERROR("Unable to del port stats timer : %d, dev %d", rc, dev_id);
      return sts;
    }
  }

  port_stats_timer_started[dev_id] = false;
  rc = bf_sys_timer_create(&port_stats_timer[dev_id],
                           poll_intv_ms,
                           poll_intv_ms,
                           pm_port_stats_timer_cb,
                           (void *)(intptr_t)dev_id);
  if (rc) {
    PM_ERROR("Unable to create port stats timer : %d, dev %d", rc, dev_id);
    return sts;
  }

  rc = bf_sys_timer_start(&port_stats_timer[dev_id]);
  if (rc) {
    PM_ERROR("Unable to start port stats timer : %d, dev %d", rc, dev_id);
    return sts;
  }
  port_stats_timer_started[dev_id] = true;
  port_stats_timer_intvl[dev_id] = poll_intv_ms;
  return BF_SUCCESS;
}

bf_status_t bf_pm_port_stats_poll_period_get(bf_dev_id_t dev_id,
                                             uint32_t *poll_intv_ms) {
  if (poll_intv_ms == NULL) return BF_INVALID_ARG;
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  *poll_intv_ms = port_stats_timer_intvl[dev_id];
  return BF_SUCCESS;
}

static void bf_pm_update_rate(bf_dev_id_t dev_id,
                              bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t *port_info;
  uint64_t rx_cnt, tx_cnt;
  uint64_t rx_f_cnt, tx_f_cnt;
  struct timespec ts;
  int64_t delta_us;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  dev_id = dev_id_of_port;

  if (!port_info || (!port_info->is_added)) {
    return;
  }

  pm_port_exclusive_access_start(dev_id, port_info);
  clock_gettime(CLOCK_REALTIME, &ts);

  // update counter cache, time stamp, init rate
  sts = bf_port_mac_stats_hw_sync_get(
      dev_id, port_info->dev_port, &port_info->curr_stats);
  if (sts != BF_SUCCESS) {
    pm_port_exclusive_access_end(dev_id, port_info);
    return;
  }

  if ((port_info->curr_stats.format.ctr_ids.OctetsReceivedinGoodFrames <
       port_info->rx_octets_good) ||
      (port_info->curr_stats.format.ctr_ids.OctetsTransmittedwithouterror <
       port_info->tx_octets_good) ||
      (port_info->curr_stats.format.ctr_ids.FramesTransmittedOK <
       port_info->tx_frame_good) ||
      (port_info->curr_stats.format.ctr_ids.FramesReceivedOK <
       port_info->rx_frame_good) ||
      (port_info->rate_timestamp_sec > ts.tv_sec) ||
      (port_info->rate_timestamp_sec == 0)) {
    goto end;
  }

  rx_f_cnt = port_info->curr_stats.format.ctr_ids.FramesReceivedOK -
             port_info->rx_frame_good;
  tx_f_cnt = port_info->curr_stats.format.ctr_ids.FramesTransmittedOK -
             port_info->tx_frame_good;

  rx_cnt = (port_info->curr_stats.format.ctr_ids.OctetsReceivedinGoodFrames -
            port_info->rx_octets_good);
  tx_cnt = (port_info->curr_stats.format.ctr_ids.OctetsTransmittedwithouterror -
            port_info->tx_octets_good);

  // overhead = bf_port_overhead_len_get(dev_id, dev_port);
  rx_cnt = rx_cnt * 8 + rx_f_cnt * 20 * 8;
  tx_cnt = tx_cnt * 8 + tx_f_cnt * 20 * 8;

  /* Determine delta time [us] between the current and the previous update */
  delta_us = (ts.tv_sec - port_info->rate_timestamp_sec) * 1000000 +
             (ts.tv_nsec - port_info->rate_timestamp_nsec) / 1000;
  if (!delta_us) goto end;

  /* Compute rate and pps values keeping delta in us to reduce the error */
  port_info->rx_rate = (rx_cnt * 1000000) / delta_us;  // bps
  port_info->tx_rate = (tx_cnt * 1000000) / delta_us;
  port_info->rx_pps = (rx_f_cnt * 1000000) / delta_us;  // pps
  port_info->tx_pps = (tx_f_cnt * 1000000) / delta_us;

end:
  port_info->rate_timestamp_sec = ts.tv_sec;
  port_info->rate_timestamp_nsec = ts.tv_nsec;
  port_info->rx_octets_good =
      port_info->curr_stats.format.ctr_ids.OctetsReceivedinGoodFrames;
  port_info->tx_octets_good =
      port_info->curr_stats.format.ctr_ids.OctetsTransmittedwithouterror;
  port_info->rx_frame_good =
      port_info->curr_stats.format.ctr_ids.FramesReceivedOK;
  port_info->tx_frame_good =
      port_info->curr_stats.format.ctr_ids.FramesTransmittedOK;
  pm_port_exclusive_access_end(dev_id, port_info);
  return;
}

static void bf_pm_rate_calc(bf_pm_port_info_t *port_info) {
  uint64_t rx_cnt, tx_cnt;
  uint64_t rx_f_cnt, tx_f_cnt;
  uint64_t delta_us;
  uint64_t prev_us, current_us;

  if (port_info == NULL) {
    goto end;
  }

  if ((port_info->curr_stats.format.ctr_ids.OctetsReceivedinGoodFrames <
       port_info->rx_octets_good) ||
      (port_info->curr_stats.format.ctr_ids.OctetsTransmittedwithouterror <
       port_info->tx_octets_good) ||
      (port_info->curr_stats.format.ctr_ids.FramesTransmittedOK <
       port_info->tx_frame_good) ||
      (port_info->curr_stats.format.ctr_ids.FramesReceivedOK <
       port_info->rx_frame_good)) {
    goto end;
  }

  rx_f_cnt = port_info->curr_stats.format.ctr_ids.FramesReceivedOK -
             port_info->rx_frame_good;
  tx_f_cnt = port_info->curr_stats.format.ctr_ids.FramesTransmittedOK -
             port_info->tx_frame_good;

  rx_cnt = (port_info->curr_stats.format.ctr_ids.OctetsReceivedinGoodFrames -
            port_info->rx_octets_good);
  tx_cnt = (port_info->curr_stats.format.ctr_ids.OctetsTransmittedwithouterror -
            port_info->tx_octets_good);

  // overhead = bf_port_overhead_len_get(dev_id, dev_port);
  rx_cnt = rx_cnt * 8 + rx_f_cnt * 20 * 8;
  tx_cnt = tx_cnt * 8 + tx_f_cnt * 20 * 8;

  /* Determine delta time [us] between the current and the previous update */
  current_us = (port_info->stats_timestamp_sec) * 1000000 +
               (port_info->stats_timestamp_nsec) / 1000;
  prev_us = (port_info->prev_stats_timestamp_sec) * 1000000 +
            (port_info->prev_stats_timestamp_nsec) / 1000;
  delta_us = current_us - prev_us;
  if (!delta_us) goto end;

  /* Compute rate and pps values keeping delta in us to reduce the error */
  port_info->rx_rate = (rx_cnt * 1000000) / delta_us;  // bps
  port_info->tx_rate = (tx_cnt * 1000000) / delta_us;
  port_info->rx_pps = (rx_f_cnt * 1000000) / delta_us;  // pps
  port_info->tx_pps = (tx_f_cnt * 1000000) / delta_us;

end:
  return;
}

/**
 * Callback triggered by the rate timer
 */
static void pm_port_update_rate_cb(struct bf_sys_timer_s *timer, void *data) {
  // calculate rate here, wait to be displayed
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr, *next_port_hdl_ptr;
  bf_dev_id_t dev_id;

  dev_id = (bf_dev_id_t)(intptr_t)data;
  port_hdl_ptr = &port_hdl;
  next_port_hdl_ptr = &next_port_hdl;
  // Get the first port in the system
  sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
  if (sts != BF_SUCCESS || !port_hdl_ptr) {
    return;
  }
  bf_pm_update_rate(dev_id, port_hdl_ptr);
  while (sts == BF_SUCCESS) {
    // Get the next port in the system
    sts = bf_pm_port_front_panel_port_get_next(
        dev_id, port_hdl_ptr, next_port_hdl_ptr);
    if (sts == BF_OBJECT_NOT_FOUND) break;
    if (sts != BF_SUCCESS) {
      return;
    }
    bf_pm_update_rate(dev_id, next_port_hdl_ptr);
    // Make the curr port hdl equal to the next port hdl
    port_hdl_ptr = next_port_hdl_ptr;
  }
  return;
  (void)timer;
}

bf_status_t bf_pm_rate_timer_start(bf_dev_id_t dev_id) {
  bf_sys_timer_status_t rc;
  dev_id = 0;  // no reason to support per-device timers

  if (rate_timer_on[dev_id] == true) return BF_SUCCESS;
  rc = bf_sys_timer_start(&rate_timer[dev_id]);
  if (rc) {
    return BF_OBJECT_NOT_FOUND;
  }
  rate_timer_on[dev_id] = true;
  return BF_SUCCESS;
}

bf_status_t bf_pm_rate_timer_check_del(bf_dev_id_t dev_id) {
  bf_sys_timer_status_t rc;
  dev_id = 0;  // no reason to support per-device timers

  if (rate_timer_on[dev_id] == true) {
    // del timer
    rc = bf_sys_timer_del(&rate_timer[dev_id]);
    if (rc) {
      return BF_OBJECT_NOT_FOUND;
    }
    rate_timer_on[dev_id] = false;
  }
  return BF_SUCCESS;
}

bf_status_t bf_pm_rate_timer_creat(bf_dev_id_t dev_id, uint32_t period_msecs) {
  bf_sys_timer_status_t rc;
  dev_id = 0;  // no reason to support per-device timers

  // set timer
  rc = bf_sys_timer_create(&rate_timer[dev_id],
                           period_msecs,
                           period_msecs,
                           pm_port_update_rate_cb,
                           (void *)(intptr_t)dev_id);
  if (rc) {
    return BF_OBJECT_NOT_FOUND;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_stats_persistent_set(bf_dev_id_t dev_id, bool enable) {
  return port_mgr_stats_persistent_set(dev_id, enable);
}

bf_status_t bf_pm_port_stats_persistent_get(bf_dev_id_t dev_id, bool *enable) {
  return port_mgr_stats_persistent_get(dev_id, enable);
}

/**
 * Form the port str for the internal ports
 */
static void pm_internal_port_info_to_str(uint32_t conn,
                                         uint32_t chnl,
                                         char *str_hdl,
                                         uint32_t str_len) {
  if (!str_hdl) return;

  snprintf(str_hdl, str_len, "%d/%d", conn, chnl);

  return;
}

/**
 * Mark a particular port as internal
 */
static void pm_mark_internal_this_port(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port) {
  bf_pm_port_info_t *port_info, *dbg_port_info;
  lld_err_t ret;
  bool is_internal = false;
  bf_pal_front_port_info_t front_port_info = {0};
  uint32_t log_mac, phy_mac, mac_chnl;
  unsigned long port_key;
  bf_map_sts_t map_sts;

  ret = lld_sku_is_dev_port_internal(dev_id, dev_port, &is_internal);
  if (ret != LLD_OK) {
    PM_ERROR(
        "Unable to determine if port is internal for dev : %d : dev port : "
        "%d : lld_err : %d",
        dev_id,
        dev_port,
        ret);
  }
  if (!is_internal) return;

  dbg_port_info = pm_port_info_get(dev_id, dev_port);
  if (dbg_port_info) {
    // Remove the port from the database
    port_key = pm_port_info_map_key_get(
        dev_id, &dbg_port_info->pltfm_port_info.port_hdl);
    PORT_MGR_LOCK(&port_map_mtx[dev_id]);
    map_sts = bf_map_rmv(&bf_pm_port_map_db[dev_id], port_key);
    PORT_MGR_UNLOCK(&port_map_mtx[dev_id]);
    if (map_sts != BF_MAP_OK) {
      PM_ERROR(
          "Unable to remove internal passed by the pltfm module for dev: %d : "
          "front_port : "
          "%d/%d (%d)",
          dev_id,
          dbg_port_info->pltfm_port_info.port_hdl.conn_id,
          dbg_port_info->pltfm_port_info.port_hdl.chnl_id,
          dbg_port_info->dev_port);
    }
  }

  // Fake some front port conn_id and chnl_id and insert the internal
  // port in the port database
  front_port_info.dev_id = dev_id;
  ret =
      lld_sku_map_dev_port_id_to_mac_ch(dev_id, dev_port, &phy_mac, &mac_chnl);
  if (ret != LLD_OK) {
    PM_ERROR(
        "Unable to get the mac for internal port for dev : %d : dev port: %d : "
        "lld_err : %d",
        dev_id,
        dev_port,
        ret);
    return;
  }
  ret = lld_sku_map_phy2log_mac_block(dev_id, phy_mac, &log_mac);
  if (ret != LLD_OK) {
    PM_ERROR(
        "Unable to get the logical mac from physical mac for dev : %d : dev "
        "port : %d : phy MAC : %d : lld_err : %d",
        dev_id,
        dev_port,
        phy_mac,
        ret);
    return;
  }
  // For internal ports, the front panel conn_id are = (INITIAL OFFSET + LOG_MAC
  // connected to that port)
  front_port_info.port_hdl.conn_id = INTERNAL_PORT_CONN_ID_START + log_mac;
  front_port_info.port_hdl.chnl_id = mac_chnl;
  front_port_info.log_mac_id = log_mac;
  front_port_info.log_mac_lane = mac_chnl;
  pm_internal_port_info_to_str(front_port_info.port_hdl.conn_id,
                               front_port_info.port_hdl.chnl_id,
                               front_port_info.port_str,
                               sizeof(front_port_info.port_str));

  port_info =
      (bf_pm_port_info_t *)bf_sys_calloc(1, 1 * sizeof(bf_pm_port_info_t));
  if (!port_info) {
    PM_ERROR(
        "Unable to allocate memory for port info in database for dev : %d : "
        "port : %d/%d (%d)",
        front_port_info.dev_id,
        front_port_info.port_hdl.conn_id,
        front_port_info.port_hdl.chnl_id,
        dev_port);
    return;
  }
  // Cache the front panel port info in the local database
  memcpy(&port_info->pltfm_port_info,
         &front_port_info,
         sizeof(port_info->pltfm_port_info));
  port_info->dev_port = dev_port;
  port_info->is_internal = true;
  port_key = pm_port_info_map_key_get(dev_id, &front_port_info.port_hdl);
  PORT_MGR_LOCK(&port_map_mtx[dev_id]);
  map_sts = bf_map_add(&bf_pm_port_map_db[dev_id], port_key, (void *)port_info);
  PORT_MGR_UNLOCK(&port_map_mtx[dev_id]);
  if (map_sts != BF_MAP_OK) {
    PM_ERROR(
        "Unable to add port in database for for dev : %d : port : %d/%d (%d) : "
        "map_err : %d",
        front_port_info.dev_id,
        front_port_info.port_hdl.conn_id,
        front_port_info.port_hdl.chnl_id,
        dev_port,
        map_sts);
    return;
  }
  PM_DEBUG("Dev : %d : Marked Internal Port : %d/%d (%d)",
           front_port_info.dev_id,
           front_port_info.port_hdl.conn_id,
           front_port_info.port_hdl.chnl_id,
           dev_port);
}

/**
 * Iterate over all the ports and mark the internal ports
 */
static void pm_mark_internal_ports(bf_dev_id_t dev_id) {
  uint32_t internal_pipes;
  uint32_t num_pipes, i, j;
  bf_dev_port_t dev_port;
  lld_err_t ret;

  internal_pipes = lld_get_internal_pipe_numb(dev_id);
  if (internal_pipes == 0) {
    return;
  }

  ret = lld_sku_get_num_active_pipes(dev_id, &num_pipes);
  if (ret != LLD_OK) {
    PM_ERROR("Unable to get the number of pipes for dev : %d : lld_err : %d",
             dev_id,
             ret);
    bf_sys_assert(0);
  }

  for (i = 0; i < num_pipes; i++) {
    for (j = (uint32_t)lld_get_min_fp_port(dev_id);
         j <= (uint32_t)lld_get_max_fp_port(dev_id);
         j++) {
      dev_port = MAKE_DEV_PORT(i, j);
      pm_mark_internal_this_port(dev_id, dev_port);
    }
  }

  for (j = (uint32_t)lld_get_min_cpu_port(dev_id);
       j <= (uint32_t)lld_get_max_cpu_port(dev_id);
       j++) {
    dev_port = MAKE_DEV_PORT(0, j);
    pm_mark_internal_this_port(dev_id, dev_port);
  }
}

void bf_pm_port_mac_map_mtx_init() {
  for (int dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    bf_sys_rmutex_init(&port_map_mtx[dev_id]);
    bf_sys_rmutex_init(&mac_map_mtx[dev_id]);
  }
}

void bf_pm_port_mac_map_mtx_del() {
  for (int dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    bf_sys_rmutex_del(&port_map_mtx[dev_id]);
    bf_sys_rmutex_del(&mac_map_mtx[dev_id]);
  }
}

/**
 * Remove the non-functional ports - can be debug ports/unrouted/temporary
 * non-operational ports. Called at the end of device-add
 */
static void pm_rmv_non_func_ports_from_db(bf_dev_id_t dev_id) {
  bf_pm_port_info_t *port_info;
  bf_pal_front_port_handle_t *port_hdl = NULL;
  unsigned long port_key;
  bf_map_sts_t map_sts;

  port_info = pm_port_info_get_first_on_this(dev_id);
  for (; port_info != NULL;
       port_info = pm_port_info_get_next_on_this(dev_id, port_info)) {
    // Current known case
    if (!pltfm_interface.pltfm_is_port_non_func) continue;

    port_hdl = &port_info->pltfm_port_info.port_hdl;
    if (!port_hdl) continue;

    if (!pltfm_interface.pltfm_is_port_non_func(dev_id, port_hdl)) continue;

    port_key = pm_port_info_map_key_get(dev_id, port_hdl);
    PORT_MGR_LOCK(&port_map_mtx[dev_id]);
    map_sts = bf_map_rmv(&bf_pm_port_map_db[dev_id], port_key);
    PORT_MGR_UNLOCK(&port_map_mtx[dev_id]);
    if (map_sts != BF_MAP_OK) {
      PM_ERROR(
          "Unable to remove non-fucntional port : "
          "%d : %d/%d ",
          dev_id,
          port_info->pltfm_port_info.port_hdl.conn_id,
          port_info->pltfm_port_info.port_hdl.chnl_id);
    } else {
      PM_DEBUG(
          "Removed non-fucntional port : "
          "%d : %d/%d ",
          dev_id,
          port_info->pltfm_port_info.port_hdl.conn_id,
          port_info->pltfm_port_info.port_hdl.chnl_id);
    }
  }
}

/**
 * Callback issued by the DVM when a device is added
 */
static bf_status_t pm_dev_add_cb(bf_dev_id_t dev_id,
                                 bf_dev_family_t dev_family,
                                 bf_device_profile_t *dont_care,
                                 bf_dma_info_t *dma_info,
                                 bf_dev_init_mode_t warm_init_mode) {
  bf_status_t sts;
  bool is_sw_model = false;
  int x;

  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;

  // initial mac aync update mutex
  x = lld_dr_lock_required(dev_id, lld_dr_tx_mac_stat);
  if (x != LLD_OK) return BF_UNEXPECTED;
  x = lld_dr_lock_required(dev_id, lld_dr_cmp_mac_stat);
  if (x != LLD_OK) return BF_UNEXPECTED;

  // Nothing to do if it is a virtual device
  if (bf_drv_is_device_virtual(dev_id)) return BF_SUCCESS;

  // initial mac aync update mutex
  x = lld_dr_lock_required(dev_id, lld_dr_tx_mac_stat);
  if (x != LLD_OK) return BF_UNEXPECTED;
  x = lld_dr_lock_required(dev_id, lld_dr_cmp_mac_stat);
  if (x != LLD_OK) return BF_UNEXPECTED;

  // Cannot get the info of all the ports from the platforms module at once.
  // Only pull the info of current dev_id's.
  // Since LLD module for any other dev_ids' has not been initialled.
  sts = pm_pltfm_port_info_get(dev_id);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to get the front panel port info from the platforms module : "
        "%s (%d)",
        bf_err_str(sts),
        sts);
    return sts;
  }

  // Initialize the global database
  pm_port_info_init(dev_id);

  sts = bf_drv_device_type_get(dev_id, &is_sw_model);
  if (sts != BF_SUCCESS) {
    PM_ERROR("Unable to get the device type for dev : %d : %s (%d)",
             dev_id,
             bf_err_str(sts),
             sts);
  }

  // Set the swizzled MAC to Serdes lane mappings
  sts = pm_mac_to_serdes_lane_map_set(dev_id);
  if (sts != BF_SUCCESS) {
    PM_ERROR("Unable to set MAC to Serdes lane mappings for dev : %d : %s (%d)",
             dev_id,
             bf_err_str(sts),
             sts);
    return sts;
  }

  if (!is_sw_model) {
    if (bf_lld_dev_is_tof2(dev_id)) {
      // run bandgap calibration after lane maps are set
      if (!bf_pm_dev_locked[dev_id]) {
        bf_tof2_serdes_bandgap_init(dev_id);
      }
    }
#ifndef DEVICE_IS_EMULATOR
    if (bf_lld_dev_is_tof1(dev_id)) {
      // Initialize the Serdes
      pm_dev_add_serdes_init(dev_id);
    } else if (bf_lld_dev_is_tof3(dev_id)) {
      if (bf_tof3_serdes_sppt(dev_id)) {
        sts = bf_tof3_serdes_init(dev_id, true, false, true);
        if (sts != BF_SUCCESS) {
          return BF_HW_COMM_FAIL;
        }
      }
    }
#endif
  }

  pm_rmv_non_func_ports_from_db(dev_id);

  pm_mark_internal_ports(dev_id);

  // Initialize the per-port mutex for all the ports
  pm_port_info_mtx_init(dev_id);

  // Initialize the FSM pm
  pm_init(dev_id);

#ifndef DEVICE_IS_EMULATOR
  if (!bf_pm_dev_locked[dev_id]) {
    // Start the stats polling timer only when not on the model
    pm_port_stats_poll_start(dev_id);
  }
#endif

  if (pm_dev_is_locked(dev_id)) {
    // If the device is being added while it is locked it means that we are in
    // warm init. Thus we need to drain/reinitialize the port fsm so that
    // the run queue is empty
    pm_fsm_queues_init();
  }

  bf_status_t pltfm_init_sts = bf_pm_init_platform(warm_init_mode);
  if (pltfm_init_sts != BF_SUCCESS) {
    printf("ERROR: %d : from bf_pm_init_platform\n", pltfm_init_sts);
    return -1;
  }

  (void)dev_family;
  (void)dont_care;
  (void)dma_info;

  return BF_SUCCESS;
}

/** bf_pm_init_platform
 *
 * Once all platform devices and ports are added, finish
 * platform init.
 */
bf_status_t bf_pm_init_platform(bf_dev_init_mode_t warm_init_mode) {
  bf_status_t sts;

  // Call the pltfm init function
  if (!pltfm_interface.pltfm_init) bf_sys_assert(0);

  sts = pltfm_interface.pltfm_init(warm_init_mode);
  if (sts != BF_SUCCESS) {
    PM_ERROR("bf_pm_init_platform: Unable to init pltfm : %s (%d)",
             bf_err_str(sts),
             sts);
    return sts;
  }

  // Notify the platforms module that it is safe for it to call into bf_pm
  if (!pltfm_interface.pltfm_safe_to_call_in_notify) bf_sys_assert(0);

  sts = pltfm_interface.pltfm_safe_to_call_in_notify();
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "bf_pm_init_platform: Unable to notify the pltfm that it is safe to "
        "call into bf_pm"
        ": %s (%d)",
        bf_err_str(sts),
        sts);
  }
  return sts;
}

/**
 * Callback issued by the DVM when a device is deleted
 */
static bf_status_t pm_dev_del_cb(bf_dev_id_t dev_id) {
  bf_map_sts_t map_sts;
  unsigned long key;
  bf_pm_port_info_t *port_info;
  bf_pm_mac_blk *mac_map;
  bf_status_t sts;

  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  // Nothing to do if it is a virtual device
  if (bf_drv_is_device_virtual(dev_id)) return BF_SUCCESS;

  pm_port_stats_poll_stop(dev_id);

  // Remove the ports from the FSM pm
  pm_port_rmv_internal(dev_id);

  if (pltfm_interface.pltfm_deinit) {
    sts = pltfm_interface.pltfm_deinit(dev_id);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to deinit pltfm during bf_pm dev del for dev : %d : %s (%d)",
          dev_id,
          bf_err_str(sts),
          sts);
    }
    // fall through
  }

  // Deinitialize the per-port mutex for all the ports
  pm_port_info_mtx_deinit(dev_id);

  PORT_MGR_LOCK(&port_map_mtx[dev_id]);
  // Delete the port info map for this device
  while ((map_sts = bf_map_get_first_rmv(
              &bf_pm_port_map_db[dev_id], &key, (void **)&port_info)) ==
         BF_MAP_OK) {
    bf_sys_free(port_info);
  }
  bf_map_destroy(&bf_pm_port_map_db[dev_id]);
  PORT_MGR_UNLOCK(&port_map_mtx[dev_id]);

  PORT_MGR_LOCK(&mac_map_mtx[dev_id]);
  // Delete the mac block map for this device
  while ((map_sts = bf_map_get_first_rmv(
              &bf_pm_mac_map_db[dev_id], &key, (void **)&mac_map)) ==
         BF_MAP_OK) {
    bf_sys_free(mac_map->mac_chnl);
    bf_sys_free(mac_map);
  }
  bf_map_destroy(&bf_pm_mac_map_db[dev_id]);
  PORT_MGR_UNLOCK(&mac_map_mtx[dev_id]);

  /* Execute pm terminate to dealloc mutexes and other elements allocated
   * during the initialization.
   */
  pm_terminate(dev_id);

  (void)map_sts;
  return BF_SUCCESS;
}

/**
 * Callback issued by the DVM when a device is locked
 */
static bf_status_t pm_dev_lock_cb(bf_dev_id_t dev_id) {
  bf_status_t sts;

  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;

  bf_pm_dev_locked[dev_id] = true;

  // Set the cfg replay flag in pm so that the fsm is not started
  // during port enable during cfg replay
  pm_port_cfg_replay_flag_set(dev_id, true);

  if (pltfm_interface.pltfm_ha_mode_set) {
    sts = pltfm_interface.pltfm_ha_mode_set();
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to set HA mode: %d : %s (%d)", dev_id, bf_err_str(sts), sts);
    }
  }
  return BF_SUCCESS;
}

/**
 * Callback issued by the DVM to compute delta changes
 */
static bf_status_t pm_dev_delta_push_done(bf_dev_id_t dev_id) {
  bf_status_t sts;

  /* Clear ha mode flag */
  if (pltfm_interface.pltfm_ha_mode_clear) {
    sts = pltfm_interface.pltfm_ha_mode_clear();
    if (sts != BF_SUCCESS) {
      PM_ERROR("Unable to clear HA mode: %d : %s (%d)",
               dev_id,
               bf_err_str(sts),
               sts);
    }
  }

  pm_dev_unlock_cb(dev_id);

  return BF_SUCCESS;
}

static bf_status_t pm_dev_wait_port_cfg_done(bf_dev_id_t dev_id) {
  bf_status_t sts;

  /* Wait for port HA configuration to be complete */
  if (pltfm_interface.pltfm_ha_wait_port_cfg_done) {
    sts = pltfm_interface.pltfm_ha_wait_port_cfg_done(dev_id);
    if (sts != BF_SUCCESS) {
      PM_ERROR("Unable for some ports to complete HA config %d : %s (%d)",
               dev_id,
               bf_err_str(sts),
               sts);
    }
  }
  return BF_SUCCESS;
}

/**
 * Callback issued by the DVM when a device is unlocked
 */
static bf_status_t pm_dev_unlock_cb(bf_dev_id_t dev_id) {
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;

  bf_pm_dev_locked[dev_id] = false;

#ifndef DEVICE_IS_EMULATOR
  // Start the stats polling timer only when not on the model
  pm_port_stats_poll_start(dev_id);
#endif
  // Reset the cfg replay flag in pm
  pm_port_cfg_replay_flag_set(dev_id, false);

  return BF_SUCCESS;
}

/*
 * Clears the accumulated FEC counters
 */
bf_status_t bf_pm_port_clear_the_fec_counters(bf_dev_id_t dev_id,
                                              bf_dev_port_t dev_port) {
  bf_pm_port_info_t *port_info = NULL;
  bf_status_t sts;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;

  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) return BF_INVALID_ARG;

  if (!port_info->is_added) {
    return BF_INVALID_ARG;
  }

  memset(&port_info->fec_info, 0, sizeof(port_info->fec_info));

  sts = bf_port_pcs_cumulative_counters_clr(dev_id, dev_port);

  return sts;
}

/*
 * Gets cumulative RS fec corrected and uncorrected block counters
 */
bf_status_t bf_pm_port_get_rs_fec_counters(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           bool *hi_ser,
                                           bool *align_status,
                                           uint32_t *fec_corr_cnt,
                                           uint32_t *fec_uncorr_cnt) {
  bf_pm_port_info_t *port_info = NULL;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;

  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) return BF_INVALID_ARG;

  if (!port_info->is_added) return BF_INVALID_ARG;

  if (port_info->fec_type != BF_FEC_TYP_REED_SOLOMON) return BF_INVALID_ARG;

  if (hi_ser) *hi_ser = port_info->fec_info.hi_ser;
  if (align_status) *align_status = port_info->fec_info.fec_align_status;
  if (fec_corr_cnt) *fec_corr_cnt = port_info->fec_info.fec_corr_cnt;
  if (fec_uncorr_cnt) *fec_uncorr_cnt = port_info->fec_info.fec_uncorr_cnt;

  return BF_SUCCESS;
}

/*
 * Gets cumulative RS fec error counters per lane for Tofino 1 only
 */
bf_status_t bf_pm_port_get_rs_fec_ser_lane_cnt(bf_dev_id_t dev_id,
                                               bf_dev_port_t dev_port,
                                               uint32_t lane_id,
                                               uint32_t *fec_ser_lane) {
  bf_pm_port_info_t *port_info = NULL;
  bf_dev_family_t dev_family;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;

  dev_family = bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));
  if ((dev_family != BF_DEV_FAMILY_TOFINO) &&
      (dev_family != BF_DEV_FAMILY_TOFINO2)) {
    PM_ERROR("This API supports only TOFINO 1 and TOFINO 2 for dev;%d", dev_id);
    return BF_INVALID_ARG;
  }

  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) return BF_INVALID_ARG;

  if (!port_info->is_added) return BF_INVALID_ARG;

  if (port_info->fec_type != BF_FEC_TYP_REED_SOLOMON) return BF_INVALID_ARG;

  /* Check that lane is in the valid range according the Ethernet mode */
  if (lane_id >= port_info->n_lanes) return BF_INVALID_ARG;

  switch (lane_id) {
    case 0:
      *fec_ser_lane = port_info->fec_info.fec_ser_lane_0;
      break;
    case 1:
      *fec_ser_lane = port_info->fec_info.fec_ser_lane_1;
      break;
    case 2:
      *fec_ser_lane = port_info->fec_info.fec_ser_lane_2;
      break;
    case 3:
      *fec_ser_lane = port_info->fec_info.fec_ser_lane_3;
      break;
    case 4:
      *fec_ser_lane = port_info->fec_info.fec_ser_lane_4;
      break;
    case 5:
      *fec_ser_lane = port_info->fec_info.fec_ser_lane_5;
      break;
    case 6:
      *fec_ser_lane = port_info->fec_info.fec_ser_lane_6;
      break;
    case 7:
      *fec_ser_lane = port_info->fec_info.fec_ser_lane_7;
      break;
    default:
      /* This case should not be possible at least port_info->n_lanes is wrong
       */
      return BF_INVALID_ARG;
  }

  return BF_SUCCESS;
}

/*
 * Gets cumulative FC fec counters
 */
bf_status_t bf_pm_port_get_fc_fec_counters(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           bool *fc_block_lock_status,
                                           uint32_t *fc_fec_corr_blk_cnt,
                                           uint32_t *fc_fec_uncorr_blk_cnt) {
  bf_pm_port_info_t *port_info = NULL;
  uint32_t num_vl;
  uint32_t vl;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;

  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) return BF_INVALID_ARG;

  if (!port_info->is_added) return BF_INVALID_ARG;

  if (port_info->fec_type != BF_FEC_TYP_FIRECODE) return BF_INVALID_ARG;

  /* Determine number of virtual lanes */
  if ((port_info->speed == BF_SPEED_10G) ||
      (port_info->speed == BF_SPEED_25G)) {
    num_vl = 1;
  } else if ((port_info->speed == BF_SPEED_40G) ||
             (port_info->speed == BF_SPEED_40G_R2) ||
             (port_info->speed == BF_SPEED_50G) ||
             (port_info->speed == BF_SPEED_50G_CONS)) {
    num_vl = 4;
  } else {
    PM_ERROR("FC fec not supported at configured speed for dev %d :dev_port %d",
             dev_id,
             port_info->dev_port);
    return BF_INVALID_ARG;
  }

  if (fc_block_lock_status) *fc_block_lock_status = true;
  if (fc_fec_corr_blk_cnt) *fc_fec_corr_blk_cnt = 0;
  if (fc_fec_uncorr_blk_cnt) *fc_fec_uncorr_blk_cnt = 0;

  for (vl = 0; vl < num_vl; vl++) {
    if (fc_block_lock_status) {
      *fc_block_lock_status &= port_info->fec_info.fc_block_lock_status[vl];
    }
    if (fc_fec_corr_blk_cnt) {
      *fc_fec_corr_blk_cnt += port_info->fec_info.fc_fec_corr_blk_cnt[vl];
    }
    if (fc_fec_uncorr_blk_cnt) {
      *fc_fec_uncorr_blk_cnt += port_info->fec_info.fc_fec_uncorr_blk_cnt[vl];
    }
  }

  return BF_SUCCESS;
}

/*
 * Shows the cached entries
 */
static bf_status_t bf_pm_port_log_fec_counters(bf_dev_id_t dev_id,
                                               bf_dev_port_t dev_port) {
  bf_pm_port_info_t *port_info = NULL;
  bf_pal_front_port_handle_t *port_hdl = NULL;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;

  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) return BF_INVALID_ARG;

  port_hdl = &port_info->pltfm_port_info.port_hdl;
  if (!port_hdl) return BF_INVALID_ARG;

  if (!port_info->is_added) {
    return BF_INVALID_ARG;
  }

  if (port_info->fec_type == BF_FEC_TYP_REED_SOLOMON) {
    PM_DEBUG(
        "RS-FEC: Dev : %d : front port "
        ": %d/%d (%d) "
        "hiser:%d "
        "align: %d "
        "corr:%d "
        "uncorr:%d "
        "ser0:%d "
        "ser1:%d "
        "ser2:%d "
        "ser3:%d ",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        port_info->fec_info.hi_ser,
        port_info->fec_info.fec_align_status,
        port_info->fec_info.fec_corr_cnt,
        port_info->fec_info.fec_uncorr_cnt,
        port_info->fec_info.fec_ser_lane_0,
        port_info->fec_info.fec_ser_lane_1,
        port_info->fec_info.fec_ser_lane_2,
        port_info->fec_info.fec_ser_lane_3);

    bf_dev_family_t dev_family =
        bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));
    if (dev_family == BF_DEV_FAMILY_TOFINO2) {
      PM_DEBUG(
          "ser4:%d "
          "ser5:%d "
          "ser6:%d "
          "ser7:%d ",
          port_info->fec_info.fec_ser_lane_4,
          port_info->fec_info.fec_ser_lane_5,
          port_info->fec_info.fec_ser_lane_6,
          port_info->fec_info.fec_ser_lane_7);
    }

  } else if (port_info->fec_type == BF_FEC_TYP_FIRECODE) {
    uint32_t vl, num_vl;

    if ((port_info->speed == BF_SPEED_10G) ||
        (port_info->speed == BF_SPEED_25G)) {
      num_vl = 1;
    } else if ((port_info->speed == BF_SPEED_40G) ||
               (port_info->speed == BF_SPEED_40G_R2) ||
               (port_info->speed == BF_SPEED_50G) ||
               (port_info->speed == BF_SPEED_50G_CONS)) {
      num_vl = 4;
    } else {
      num_vl = 0; /* fec invalid for other speeds */
    }
    for (vl = 0; vl < num_vl; vl++) {
      PM_DEBUG(
          "FC-FEC: Dev : %d : front port "
          ": %d/%d (%d) "
          "VL: %d "
          "block-locked: %d "
          "fec-corr-blk-cnt: %d "
          "fec-uncorr-blk-cnt: %d \n",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          port_info->dev_port,
          vl,
          port_info->fec_info.fc_block_lock_status[vl],
          port_info->fec_info.fc_fec_corr_blk_cnt[vl],
          port_info->fec_info.fc_fec_uncorr_blk_cnt[vl]);
    }
  }

  return BF_SUCCESS;
}

/*
 * Get FEC counters
 */
static bf_status_t bf_pm_port_collect_fec_counters(bf_dev_id_t dev_id,
                                                   bf_dev_port_t dev_port) {
  bf_status_t sts = BF_SUCCESS;
  bf_pm_port_info_t *port_info = NULL;
  bf_pal_front_port_handle_t *port_hdl = NULL;
  bool hi_ser, fec_align_status;
  uint32_t fec_corr_cnt = 0, fec_uncorr_cnt = 0;
  uint32_t fec_ser_lane_0 = 0, fec_ser_lane_1 = 0;
  uint32_t fec_ser_lane_2 = 0, fec_ser_lane_3 = 0;
  uint32_t fec_ser_lane_4 = 0, fec_ser_lane_5 = 0;
  uint32_t fec_ser_lane_6 = 0, fec_ser_lane_7 = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;

  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) return BF_INVALID_ARG;

  port_hdl = &port_info->pltfm_port_info.port_hdl;
  if (!port_hdl) return BF_INVALID_ARG;

  if (!port_info->is_added) {
    return BF_INVALID_ARG;
  }

  if (port_info->fec_type == BF_FEC_TYP_REED_SOLOMON) {
    /* down collect during down state */
    sts = bf_port_rs_fec_status_and_counters_get(dev_id,
                                                 port_info->dev_port,
                                                 &hi_ser,
                                                 &fec_align_status,
                                                 &fec_corr_cnt,
                                                 &fec_uncorr_cnt,
                                                 &fec_ser_lane_0,
                                                 &fec_ser_lane_1,
                                                 &fec_ser_lane_2,
                                                 &fec_ser_lane_3,
                                                 &fec_ser_lane_4,
                                                 &fec_ser_lane_5,
                                                 &fec_ser_lane_6,
                                                 &fec_ser_lane_7);
    if (sts != BF_SUCCESS) {
      PM_DEBUG(
          "RS-FEC: Unable to update the counters"
          " for Dev : %d : front port "
          ": %d/%d (%d) : "
          "%s (%d) : port_added : %s ",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          port_info->dev_port,
          bf_err_str(sts),
          sts,
          port_info->is_added ? "true" : "false");
      return sts;
    }

    port_info->fec_info.hi_ser = hi_ser;
    port_info->fec_info.fec_align_status = fec_align_status;

    port_info->fec_info.fec_corr_cnt += fec_corr_cnt;
    port_info->fec_info.fec_uncorr_cnt += fec_uncorr_cnt;
    port_info->fec_info.fec_ser_lane_0 += fec_ser_lane_0;
    port_info->fec_info.fec_ser_lane_1 += fec_ser_lane_1;
    port_info->fec_info.fec_ser_lane_2 += fec_ser_lane_2;
    port_info->fec_info.fec_ser_lane_3 += fec_ser_lane_3;
    port_info->fec_info.fec_ser_lane_4 += fec_ser_lane_4;
    port_info->fec_info.fec_ser_lane_5 += fec_ser_lane_5;
    port_info->fec_info.fec_ser_lane_6 += fec_ser_lane_6;
    port_info->fec_info.fec_ser_lane_7 += fec_ser_lane_7;

  } else if (port_info->fec_type == BF_FEC_TYP_FIRECODE) {
    uint32_t vl, num_vl, fec_corr_blk_cnt, fec_uncorr_blk_cnt;
    bool block_lock_status;

    if ((port_info->speed == BF_SPEED_10G) ||
        (port_info->speed == BF_SPEED_25G)) {
      num_vl = 1;
    } else if ((port_info->speed == BF_SPEED_40G) ||
               (port_info->speed == BF_SPEED_40G_R2) ||
               (port_info->speed == BF_SPEED_50G) ||
               (port_info->speed == BF_SPEED_50G_CONS)) {
      num_vl = 4;
    } else {
      num_vl = 0; /* fec invalid for other speeds */
    }

    for (vl = 0; vl < num_vl; vl++) {
      block_lock_status = 0;
      fec_corr_blk_cnt = 0;
      fec_uncorr_blk_cnt = 0;
      sts = bf_port_fc_fec_status_and_counters_get(dev_id,
                                                   port_info->dev_port,
                                                   vl,
                                                   &block_lock_status,
                                                   &fec_corr_blk_cnt,
                                                   &fec_uncorr_blk_cnt);
      if (sts != BF_SUCCESS) {
        PM_DEBUG(
            "FC-FEC: Unable to update the counters"
            " for Dev : %d : front port "
            ": %d/%d (%d) : vl: %0d"
            "%s (%d) : port_added : %s ",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            port_info->dev_port,
            vl,
            bf_err_str(sts),
            sts,
            port_info->is_added ? "true" : "false");

        return sts;
      }

      port_info->fec_info.fc_block_lock_status[vl] = block_lock_status;
      port_info->fec_info.fc_fec_corr_blk_cnt[vl] += fec_corr_blk_cnt;
      port_info->fec_info.fc_fec_uncorr_blk_cnt[vl] += fec_uncorr_blk_cnt;
    }
  }

  return BF_SUCCESS;
}

/*
 * Gets cumulative pcs counters
 */
bf_status_t bf_pm_port_get_pcs_counters(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        uint32_t *ber_cnt,
                                        uint32_t *errored_blk_cnt) {
  bf_pm_port_info_t *port_info = NULL;
  bf_status_t sts;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;

  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) return BF_INVALID_ARG;

  if (!port_info->is_added) return BF_INVALID_ARG;

  if (port_info->fec_type != BF_FEC_TYP_NONE) return BF_INVALID_ARG;

  sts = bf_port_pcs_cumulative_counters_get(
      dev_id, dev_port, ber_cnt, errored_blk_cnt);

  return sts;
}

static bf_status_t bf_pm_port_log_pcs_counters(bf_dev_id_t dev_id,
                                               bf_dev_port_t dev_port) {
  bf_pm_port_info_t *port_info = NULL;
  bf_pal_front_port_handle_t *port_hdl = NULL;

  bool pcs_status = false;
  uint32_t block_lock_per_pcs_lane = 0;
  uint32_t alignment_marker_lock_per_pcs_lane = 0;
  bool hi_ber = false;
  bool block_lock_all = false;
  bool alignment_marker_lock_all = false;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;

  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) return BF_INVALID_ARG;

  port_hdl = &port_info->pltfm_port_info.port_hdl;
  if (!port_hdl) return BF_INVALID_ARG;

  if (!port_info->is_added) {
    return BF_INVALID_ARG;
  }

  /* check PCS status */
  bf_port_pcs_status_get(dev_id,
                         dev_port,
                         &pcs_status,
                         &block_lock_per_pcs_lane,
                         &alignment_marker_lock_per_pcs_lane,
                         &hi_ber,
                         &block_lock_all,
                         &alignment_marker_lock_all);
  PM_DEBUG(
      "PCS: Dev : %d : "
      "front port : %d/%d (%d) : "
      "pcs-sts=%d blk-lk=%x blk-lk-all=%d hi-ber=%x : "
      "algn-lk-all=%d algn-lk=%x",
      dev_id,
      port_hdl->conn_id,
      port_hdl->chnl_id,
      port_info->dev_port,
      pcs_status ? 1 : 0,
      block_lock_per_pcs_lane,
      block_lock_all ? 1 : 0,
      hi_ber ? 1 : 0,
      alignment_marker_lock_all ? 1 : 0,
      alignment_marker_lock_per_pcs_lane);

  if (pcs_status && block_lock_all) {
    // if PCS is up, check for bit errors in case link toggles
    if (hi_ber == 0) {
      uint32_t ber_cnt;
      uint32_t errored_blk_cnt;
      uint32_t sync_loss;
      uint32_t block_lock_loss;
      uint32_t hi_ber_cnt;
      uint32_t valid_error_cnt;
      uint32_t unknown_error_cnt;
      uint32_t invalid_error_cnt;
      uint32_t bip_errors_per_pcs_lane[20];
      uint32_t bip_err_summary = 0;
      uint32_t ln;

      bf_port_pcs_counters_get(dev_id,
                               dev_port,
                               &ber_cnt,
                               &errored_blk_cnt,
                               &sync_loss,
                               &block_lock_loss,
                               &hi_ber_cnt,
                               &valid_error_cnt,
                               &unknown_error_cnt,
                               &invalid_error_cnt,
                               bip_errors_per_pcs_lane);

      if (port_info->speed == BF_SPEED_100G) {
        for (ln = 0; ln < 20; ln++) {
          bip_err_summary += bip_errors_per_pcs_lane[ln];
        }
      }

      PM_DEBUG(
          "PCS: Dev : %d : "
          "front port : %d/%d (%d) : "
          "err-blk=%d : syn-lst=%d : bk-lk-lst=%d : "
          "hi_ber=%d : val-err=%d : unk-err=%d : inv-err=%d : bip=%d",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          port_info->dev_port,
          errored_blk_cnt,
          sync_loss,
          block_lock_loss,
          hi_ber_cnt,
          valid_error_cnt,
          unknown_error_cnt,
          invalid_error_cnt,
          bip_err_summary);
    }
  }

  return BF_SUCCESS;
}

/*
 * Called peridocally to collect the error counters.
 */
static bf_status_t bf_pm_collect_port_error_counters(bf_dev_id_t dev_id,
                                                     bf_dev_port_t dev_port) {
  bf_pm_port_info_t *port_info = NULL;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;

  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) return BF_INVALID_ARG;

  if (!port_info->is_added) {
    return BF_SUCCESS;
  }

  if ((port_info->oper_status != PM_PORT_UP) &&
      (port_info->admin_state != PM_PORT_ENABLED)) {
    return BF_SUCCESS;
  }
  if (bf_pm_intf_is_device_family_tofino(dev_id)) {
    if (port_info->fec_type == BF_FEC_TYP_NONE) {
      /* force a read of PCS counters */
      bf_port_pcs_cumulative_counters_get(dev_id, dev_port, NULL, NULL);
    } else {
      bf_pm_port_collect_fec_counters(dev_id, dev_port);
    }
  }
  return BF_SUCCESS;
}

/**
 * Callback issued by the DVM whenever a port's link status changes
 */
static bf_status_t pm_port_link_status_chg_cb(bf_dev_id_t dev_id,
                                              bf_dev_port_t dev_port,
                                              bool oper_state) {
  bf_status_t sts = BF_SUCCESS;
  bf_pm_port_info_t *port_info = NULL;
  bf_pal_front_port_handle_t *port_hdl = NULL;
  bf_pal_front_port_cb_cfg_t port_cfg = {0};

  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) return BF_INVALID_ARG;
  port_hdl = &port_info->pltfm_port_info.port_hdl;

  if (!port_info->is_added) {
    /* During warm init, callbacks are issued based on the hardware oper
     * state for all the possible ports. It is up to us to ignore the
     * callbacks for the ports which do not exist
     */
    return BF_SUCCESS;
  }
  /* The mutex is recursive mutex, hence we are free to take the mutex multiple
   * time */
  pm_port_exclusive_access_start(dev_id, port_info);

  sts = pm_port_status_chg_cb(dev_id, dev_port, oper_state, NULL);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Error in notifying switchd pm oper state change for dev : %d : front "
        "port : %d/%d (%d) : %s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
  }

  PM_DEBUG("PM: Dev %d : front port %d/%d (%d) : %s",
           dev_id,
           port_hdl->conn_id,
           port_hdl->chnl_id,
           port_info->dev_port,
           oper_state ? "UP" : "DOWN");

  /*
   * link flap from Up to down. Collect error counters and display.
   * Note: If flaps are happening too fast, we may have to reduce the
   * log frequency.
   */
  if (!oper_state && port_info->admin_state == PM_PORT_ENABLED) {
    if (bf_pm_intf_is_device_family_tofino(dev_id) ||
        bf_pm_intf_is_device_family_tofino2(dev_id)) {
      /* Display whatever is cached until now during port-up */
      bf_pm_port_log_fec_counters(dev_id, dev_port);
      bf_pm_port_clear_the_fec_counters(dev_id, dev_port);
      /*
       * Get current FEC counters and log it. Mainly to
       * catch FEC errors that causes port-down
       */
      bf_pm_port_collect_fec_counters(dev_id, dev_port);
      bf_pm_port_log_fec_counters(dev_id, dev_port);
    }
    /* Log PCS errors, if any */
    bf_pm_port_log_pcs_counters(dev_id, dev_port);
  }

  /* clear on UP */
  if (oper_state && port_info->admin_state == PM_PORT_ENABLED) {
    if (bf_pm_intf_is_device_family_tofino(dev_id)) {
      bf_pm_port_clear_the_fec_counters(dev_id, dev_port);
    }
  }

  // Initialize the port cfg to be passed to the platform module
  pm_port_cfg_set(dev_id, port_info->dev_port, &port_cfg);

  if (!port_info->is_internal) {
    pm_port_exclusive_access_end(dev_id, port_info);
    if (oper_state) {
      if (pltfm_interface.pltfm_port_link_up_actions) {
        sts = pltfm_interface.pltfm_port_link_up_actions(port_hdl, &port_cfg);
        if (sts != BF_SUCCESS) {
          PM_ERROR(
              "Error in pltfm port link up actions for dev : %d : front port "
              ": %d/%d (%d) : %s (%d)",
              dev_id,
              port_hdl->conn_id,
              port_hdl->chnl_id,
              port_info->dev_port,
              bf_err_str(sts),
              sts);
        }
      }
    } else {
      if (pltfm_interface.pltfm_port_link_down_actions) {
        sts = pltfm_interface.pltfm_port_link_down_actions(port_hdl, &port_cfg);
        if (sts != BF_SUCCESS) {
          PM_ERROR(
              "Error in pltfm port link down actions for dev : %d : front "
              "port : %d/%d (%d) : %s (%d)",
              dev_id,
              port_hdl->conn_id,
              port_hdl->chnl_id,
              port_info->dev_port,
              bf_err_str(sts),
              sts);
        }
      }
    }
    pm_port_exclusive_access_start(dev_id, port_info);
  }
  port_info->oper_status = oper_state ? PM_PORT_UP : PM_PORT_DOWN;
  pm_port_exclusive_access_end(dev_id, port_info);
  return BF_SUCCESS;
}

static bf_sds_line_rate_mode_t pm_port_speed_to_sds_line_rate_map(
    bf_port_speed_t ps) {
  // FIXME TBD for TOF2
  switch (ps) {
    case BF_SPEED_1G:
      return BF_SDS_LINE_RATE_1p25G;
      break;
    case BF_SPEED_10G:
      return BF_SDS_LINE_RATE_10G;
      break;
    case BF_SPEED_25G:
      return BF_SDS_LINE_RATE_25G;
      break;
    case BF_SPEED_40G:
      return BF_SDS_LINE_RATE_10G;
      break;
    case BF_SPEED_50G:
      return BF_SDS_LINE_RATE_25G;
      break;
    case BF_SPEED_100G:
      return BF_SDS_LINE_RATE_25G;
      break;
    default:
      return BF_SDS_LINE_RATE_10G;
      break;
  }
}

bf_status_t bf_pm_init(void) {
  bf_status_t sts;
  bf_dev_id_t dev_id;
  bf_map_sts_t map_sts;

  for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    map_sts = bf_map_init(&bf_pm_port_map_db[dev_id]);
    if (map_sts != BF_MAP_OK) {
      PM_ERROR(
          "Unable to initialize the port db in bf_pm for dev : %d : err : %d",
          dev_id,
          map_sts);
      return BF_NO_SYS_RESOURCES;
    }
    map_sts = bf_map_init(&bf_pm_mac_map_db[dev_id]);
    if (map_sts != BF_MAP_OK) {
      PM_ERROR(
          "Unable to initialize the mac db in bf_pm for dev : %d : err : %d",
          dev_id,
          map_sts);
      return BF_NO_SYS_RESOURCES;
    }
    port_stats_timer_started[dev_id] = false;
    rate_timer_on[dev_id] = false;
    port_stats_timer_intvl[dev_id] = PORT_STATS_POLL_TMR_PERIOD_MS;
    port_mgr_stats_persistent_set(dev_id, false);
  }

  // Get all the interfaces defined by the platforms module
  sts = bf_pal_pltfm_all_interface_get(&pltfm_interface);
  if (sts != BF_SUCCESS) {
    PM_ERROR("Unable to get the interfaces defined by platforms : %s (%d)",
             bf_err_str(sts),
             sts);
    return sts;
  }

  // Register callbacks with the DVM
  sts = bf_drv_register("bf_pm", &bf_pm_drv_hdl);
  if (sts != BF_SUCCESS) {
    PM_ERROR("Unable to register bf_pm module with DVM : %s (%d)",
             bf_err_str(sts),
             sts);
    return sts;
  }
  bf_drv_client_callbacks_t callbacks = {0};
  callbacks.device_add = pm_dev_add_cb;
  callbacks.device_del = pm_dev_del_cb;
  callbacks.port_status = pm_port_link_status_chg_cb;
  callbacks.lock = pm_dev_lock_cb;
  callbacks.wait_for_swcfg_replay_end = pm_dev_wait_port_cfg_done;
  // Note: Handle all unlock_reprogram_core cases inside
  // port_delta_push_done, which is common to both
  // hitless and fast-reconfigure.
  callbacks.complete_hitless_swcfg_replay = pm_dev_wait_port_cfg_done;
  callbacks.port_delta_push_done = pm_dev_delta_push_done;
  sts = bf_drv_client_register_callbacks(
      bf_pm_drv_hdl, &callbacks, BF_CLIENT_PRIO_0);
  if (sts != BF_SUCCESS) {
    PM_ERROR("Unable to register bf_pm callbacks with DVM : %s (%d)",
             bf_err_str(sts),
             sts);
    return sts;
  }

  /* Initalize the PM queues */
  bf_pm_fsm_queues_init();

  return BF_SUCCESS;
}

/**
 * Given a dev_id, return the size of the corresponding port database
 */
uint32_t pm_port_info_db_size_get(bf_dev_id_t dev_id) {
  bf_pm_port_info_t *port_info = NULL;
  uint32_t num_ports = 0;

  for (port_info = pm_port_info_get_first_on_this(dev_id); port_info != NULL;
       port_info = pm_port_info_get_next_on_this(dev_id, port_info)) {
    num_ports++;
  }

  return num_ports;
}

/**
 * Given a dev_id, return the num of front-ports
 */
uint32_t pm_num_of_front_ports_get(bf_dev_id_t dev_id) {
  bf_pm_port_info_t *port_info = NULL;
  uint32_t num_ports = 0;

  for (port_info = pm_port_info_get_first_on_this(dev_id); port_info != NULL;
       port_info = pm_port_info_get_next_on_this(dev_id, port_info)) {
    if (!port_info->is_internal) {
      num_ports++;
    }
  }

  return num_ports;
}

/**
 * return the total num of front-ports
 */
uint32_t pm_num_fp_all_get(void) {
  uint32_t cnt_all_fp = 0;

  for (bf_dev_id_t dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    cnt_all_fp += pm_num_of_front_ports_get(dev_id);
  }
  return cnt_all_fp;
}

/**
 * Given a dev_id, return the num of internal-ports
 */
uint32_t pm_num_of_internal_ports_get(bf_dev_id_t dev_id) {
  bf_pm_port_info_t *port_info = NULL;
  uint32_t num_ports = 0;

  for (port_info = pm_port_info_get_first_on_this(dev_id); port_info != NULL;
       port_info = pm_port_info_get_next_on_this(dev_id, port_info)) {
    if (port_info->is_internal) {
      num_ports++;
    }
  }

  return num_ports;
}

/**
 * Return total num of internal-ports
 */
uint32_t pm_num_of_internal_ports_all_get(void) {
  uint32_t cnt_all_ip = 0;

  for (bf_dev_id_t dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    cnt_all_ip += pm_num_of_internal_ports_get(dev_id);
  }
  return cnt_all_ip;
}

// Public Functions

bf_status_t bf_pm_pltfm_front_port_ready_for_bringup(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl, bool is_ready) {
  bf_status_t sts = BF_SUCCESS;
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d:%d(ready)",
           dev_id,
           port_hdl->conn_id,
           port_hdl->chnl_id,
           is_ready);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (!port_info) {
    PM_ERROR("%s: No port_info for dev %d port %d/%d",
             __func__,
             dev_id,
             port_hdl->conn_id,
             port_hdl->chnl_id);
    return BF_INVALID_ARG;
  }
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  pm_port_exclusive_access_start(dev_id, port_info);

  // Update the port info in the database
  port_info->is_ready_for_bringup = is_ready;
  if (!is_ready) {
    port_info->is_an_eligible = false;
  }

  // Do nothing if the port is in loopback
  if (port_info->lpbk_mode != BF_LPBK_NONE) {
    sts = BF_SUCCESS;
    goto end;
  }

  // For Tof2, Pltfm will push it
  if (bf_pm_intf_is_device_family_tofino(dev_id)) {
    if (port_info->is_added && is_ready) {
      sts = pm_serdes_tx_eq_param_set(dev_id, port_hdl);
      if (sts != BF_SUCCESS) {
        PM_ERROR(
            "Unable to set the tx eq settings for dev : %d : front port : "
            "%d/%d "
            "(%d) : %s "
            "(%d)",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            port_info->dev_port,
            bf_err_str(sts),
            sts);
        goto end;
      }
    }
  }

  if (port_info->is_added && port_info->admin_state == PM_PORT_ENABLED &&
      is_ready) {
    // valid only for tf3. For others, returns success
    bf_pm_port_multi_serdes_polarity_set(dev_id, port_hdl);

    // Bring up the port
    sts = pm_port_bring_up(dev_id, port_hdl);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Error while port bringup for dev : %d : front port : %d/%d (%d) : "
          "%s "
          "(%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          port_info->dev_port,
          bf_err_str(sts),
          sts);
      goto end;
    }
  }
  if (port_info->is_added && port_info->admin_state == PM_PORT_ENABLED &&
      !is_ready) {
    // Bring down the port
    sts = pm_port_bring_down(dev_id, port_hdl);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Error while port bringdown for dev : %d : front port : %d/%d (%d) : "
          "%s "
          "(%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          port_info->dev_port,
          bf_err_str(sts),
          sts);
      goto end;
    }
  }

end:
  pm_port_exclusive_access_end(dev_id, port_info);
  return sts;
}

bf_status_t bf_pm_pltfm_front_port_eligible_for_autoneg(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl,
    bool is_eligible) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d:%d(an_eligibility)",
           dev_id,
           port_hdl->conn_id,
           port_hdl->chnl_id,
           is_eligible);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  pm_port_exclusive_access_start(dev_id, port_info);
  // Update the port info in the database
  port_info->is_an_eligible = is_eligible;
  pm_port_exclusive_access_end(dev_id, port_info);

  return BF_SUCCESS;
}

bf_status_t bf_pm_pltfm_front_port_num_lanes_get(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl, int *num_lanes) {
  bf_status_t sts;
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info = NULL;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;

  PM_TRACE("%d:%d/%d", dev_id, port_hdl->conn_id, port_hdl->chnl_id);

  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  if (port_info->is_added) {
    *num_lanes = port_info->n_lanes;
  } else {
    PM_ERROR("Port is not added dev : %d : front port : %d/%d (%d)",
             dev_id,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_port);
    return BF_NOT_IMPLEMENTED;
  }
  return BF_SUCCESS;
}

bf_status_t bf_pm_port_dev_port_to_front_panel_port_get(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t *port_hdl_tmp = NULL;
  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;

  port_hdl_tmp = pm_port_info_get(dev_id, dev_port);
  if (port_hdl_tmp == NULL) return BF_INVALID_ARG;
  // Copy the port handle from the central database
  memcpy(
      port_hdl, &(port_hdl_tmp->pltfm_port_info.port_hdl), sizeof(*port_hdl));

  return BF_SUCCESS;
}

/**
 */
bf_status_t bf_pm_port_front_panel_port_to_dev_port_get(
    bf_pal_front_port_handle_t *port_hdl,
    bf_dev_id_t *dev_id,
    bf_dev_port_t *dev_port) {
  bf_status_t sts;

  // Safety checks
  if (!port_hdl) return BF_INVALID_ARG;
  if (!dev_id) return BF_INVALID_ARG;
  if (!dev_port) return BF_INVALID_ARG;

  sts = pm_front_port_handle_to_dev_port(port_hdl, dev_id, dev_port);
  if (sts != BF_SUCCESS) {
    PM_ERROR("%s: front port %d/%d : not found",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_front_port_index_get_first(bf_dev_id_t dev_id,
                                             uint32_t *fp_idx) {
  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!fp_idx) return BF_INVALID_ARG;

  *fp_idx = 0;
  return BF_SUCCESS;
}

bf_status_t bf_pm_front_port_index_get_next(bf_dev_id_t dev_id,
                                            uint32_t curr_fp_idx,
                                            uint32_t *next_fp_idx) {
  bf_status_t status = BF_SUCCESS;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!next_fp_idx) return BF_INVALID_ARG;

  uint32_t total_ports = 0;
  status = bf_pm_num_front_ports_get(dev_id, &total_ports);
  if (status != BF_SUCCESS) {
    PM_ERROR("Unable to get number of front ports for dev %d : err %d",
             dev_id,
             status);
    return status;
  }

  if (curr_fp_idx == (total_ports - 1)) {
    return BF_OBJECT_NOT_FOUND;
  }

  *next_fp_idx = curr_fp_idx + 1;
  return BF_SUCCESS;
}

// note: passed dev_id not used (except for some worthless checks)
// fix later
bf_status_t bf_pm_port_fp_get_first(
    bf_dev_id_t dev_id,
    bool is_added,
    bf_pal_front_port_handle_t *first_port_hdl) {
  bf_status_t sts = BF_SUCCESS;
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!first_port_hdl) return BF_INVALID_ARG;

  port_info = pm_port_info_get_first(&dev_id_of_port);
  if (!port_info) {
    sts = BF_OBJECT_NOT_FOUND;
    PM_DEBUG("%s", "bf_pm_port_fp_get_first: No ports found");
    return sts;
  }
  dev_id = dev_id_of_port;

  if (is_added) {
    // Iterate over all the ports until we find a port that is added
    while (!port_info->is_added) {
      port_info = pm_port_info_get_next(dev_id, port_info, &dev_id_of_port);
      if (!port_info) {
        // This indicates that we have iterated over all the ports but were not
        // able to find a single port that was added
        sts = BF_OBJECT_NOT_FOUND;
        PM_DEBUG("%s", "bf_pm_port_fp_get_first: No added ports found");
        return sts;
      } else {
        dev_id = dev_id_of_port;
      }
    }
  }
  first_port_hdl->conn_id = port_info->pltfm_port_info.port_hdl.conn_id;
  first_port_hdl->chnl_id = port_info->pltfm_port_info.port_hdl.chnl_id;
  return BF_SUCCESS;
}

bf_status_t bf_pm_port_front_panel_port_get_first(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *first_port_hdl) {
  return bf_pm_port_fp_get_first(dev_id, false, first_port_hdl);
}

bf_status_t bf_pm_port_front_panel_port_get_first_added(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *first_port_hdl) {
  return bf_pm_port_fp_get_first(dev_id, true, first_port_hdl);
}

bf_status_t bf_pm_port_fp_get_next(bf_dev_id_t dev_id,
                                   bool is_added,
                                   bf_pal_front_port_handle_t *curr_port_hdl,
                                   bf_pal_front_port_handle_t *next_port_hdl) {
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!next_port_hdl) return BF_INVALID_ARG;

  if (!curr_port_hdl) {
    // Since the curr_port_hdl is NULL, the user wants the first port in the
    // system
    return bf_pm_port_fp_get_first(dev_id, is_added, next_port_hdl);
  }

  sts = bf_pm_port_info_all_get(
      __func__, dev_id, curr_port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  dev_id = dev_id_of_port;

  port_info = pm_port_info_get_next(dev_id, port_info, &dev_id_of_port);
  if (!port_info) {
    return BF_OBJECT_NOT_FOUND;
  }
  dev_id = dev_id_of_port;

  if (is_added) {
    // Iterate over the remaining ports until we find a port that's added
    while (!port_info->is_added) {
      port_info = pm_port_info_get_next(dev_id, port_info, &dev_id_of_port);
      if (!port_info) {
        return BF_OBJECT_NOT_FOUND;
      }
      dev_id = dev_id_of_port;
    }
  }
  next_port_hdl->conn_id = port_info->pltfm_port_info.port_hdl.conn_id;
  next_port_hdl->chnl_id = port_info->pltfm_port_info.port_hdl.chnl_id;
  return BF_SUCCESS;
}

bf_status_t bf_pm_port_front_panel_port_get_next(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *curr_port_hdl,
    bf_pal_front_port_handle_t *next_port_hdl) {
  return bf_pm_port_fp_get_next(dev_id, false, curr_port_hdl, next_port_hdl);
}

bf_status_t bf_pm_port_front_panel_port_get_next_added(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *curr_port_hdl,
    bf_pal_front_port_handle_t *next_port_hdl) {
  return bf_pm_port_fp_get_next(dev_id, true, curr_port_hdl, next_port_hdl);
}

bf_status_t bf_pm_port_str_to_hdl_get(bf_dev_id_t dev_id,
                                      const char *port_str,
                                      bf_pal_front_port_handle_t *port_hdl) {
  bf_status_t sts = BF_SUCCESS;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!port_str) return BF_INVALID_ARG;

  if (!pltfm_interface.pltfm_port_str_to_hdl_get) {
    sts = BF_NOT_IMPLEMENTED;
    goto end;
  }

  sts = pltfm_interface.pltfm_port_str_to_hdl_get(port_str, port_hdl);
  if (sts != BF_SUCCESS) {
    goto end;
  }

  return BF_SUCCESS;
end:
  PM_ERROR("Unable to get the port hdl for dev : %d : port str : %s : %s (%d)",
           dev_id,
           port_str,
           bf_err_str(sts),
           sts);
  return sts;
}

static bf_status_t bf_pm_port_direction_clear(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl) {
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d", dev_id, port_hdl->conn_id, port_hdl->chnl_id);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  dev_port = port_info->dev_port;

  port_info->port_dir = PM_PORT_DIR_DEFAULT;

  // setting tx-mode to false, allows to run the default fsm.
  // Applies to PM_PORT_DIR_TX_ONLY --> PM_PORT_DIR_DEFAULT mode change
  sts = pm_port_fsm_for_tx_mode_set(dev_port, false);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to clear the tx-mode-fsm for dev : %d : front port : %d/%d "
        "(%d) : %s "
        "(%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
  }

  bf_port_direction_mode_set(dev_id, dev_port, BF_PORT_DIR_DUPLEX);

  return sts;
}

static bf_status_t bf_pm_port_direction_configure(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl,
    bf_pm_port_dir_e port_dir) {
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;
  bf_port_dir_e port_dir_mode;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d", dev_id, port_hdl->conn_id, port_hdl->chnl_id);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  dev_port = port_info->dev_port;

  if (!port_info->is_added) return BF_INVALID_ARG;

  port_info->port_dir = port_dir;

  if (port_dir == PM_PORT_DIR_TX_ONLY) {
    port_dir_mode = BF_PORT_DIR_TX_ONLY;
  } else if (port_dir == PM_PORT_DIR_RX_ONLY) {
    port_dir_mode = BF_PORT_DIR_RX_ONLY;
  } else if (port_dir == PM_PORT_DIR_DECOUPLED) {
    port_dir_mode = BF_PORT_DIR_DECOUPLED;
  } else {
    port_dir_mode = BF_PORT_DIR_DUPLEX;
  }
  bf_port_direction_mode_set(dev_id, dev_port, port_dir_mode);

  // stop processing here if not a Tofino 1 device.
  // Tofino 2 and later devices require a port-disable/reenable sequence
  // executed at higher level to select the proper state machine.
  if (!bf_pm_intf_is_device_family_tofino(dev_id)) return BF_SUCCESS;

  sts = pm_port_disable(dev_id, dev_port);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Error in SDE port disable for dev : %d : front port : %d/%d (%d) : "
        "%s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    goto end;
  }

  // setting tx-mode to false, allows to run the default fsm.
  // Applies to PM_PORT_DIR_TX_ONLY --> PM_PORT_DIR_DEFAULT mode change
  sts = pm_port_fsm_for_tx_mode_set(dev_port, false);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to clear the tx-mode-fsm for dev : %d : front port : %d/%d "
        "(%d) : %s "
        "(%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    goto end;
  }

  // Handle only TX mode.
  if (port_dir == PM_PORT_DIR_TX_ONLY) {
    if (pm_port_should_autoneg(dev_id, dev_port)) {
      // Stop auto negotiating
      if ((sts = pm_port_set_an(dev_id, dev_port, false)) != BF_SUCCESS) {
        PM_ERROR(
            "Unable to stop auto-negotiation for dev : %d : front port : %d/%d "
            ": "
            "%s (%d)",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            bf_err_str(sts),
            sts);
        goto end;
      }
    }

    // Enable tx-mode-fsm
    sts = pm_port_fsm_for_tx_mode_set(dev_port, true);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to set tx-mode-fsm for dev : %d : front port : %d/%d (%d) : "
          "%s "
          "(%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      goto end;
    }
  }

  /* Re-enable the port */
  if (port_info->admin_state == PM_PORT_ENABLED) {
    sts = pm_port_enable(dev_id, dev_port);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Error in SDE port enable for dev : %d : front port : %d/%d (%d) : "
          "%s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      goto end;
    }
  }

end:

  return sts;
}

bf_status_t bf_pm_port_direction_set(bf_dev_id_t dev_id,
                                     bf_pal_front_port_handle_t *port_hdl,
                                     bf_pm_port_dir_e port_dir) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;
  bf_dev_family_t dev_family;
  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d:%d(port_dir)",
           dev_id,
           port_hdl->conn_id,
           port_hdl->chnl_id,
           port_dir);
  if ((port_dir < PM_PORT_DIR_DEFAULT) || (port_dir >= PM_PORT_DIR_MAX)) {
    return BF_INVALID_ARG;
  }

  dev_family = bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));

  // Decoupled mode is currently supported by Tofino 2 only
  if (port_dir == PM_PORT_DIR_DECOUPLED &&
      dev_family != BF_DEV_FAMILY_TOFINO2) {
    PM_ERROR("%s: Error: : %d/%d Decoupled mode is only supported by Tofino 2",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id);

    return BF_INVALID_ARG;
  }

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if ((!port_info->is_added) || (port_info->is_internal)) {
    return BF_INVALID_ARG;
  }

  if (port_dir != PM_PORT_DIR_DEFAULT && port_info->lpbk_mode != BF_LPBK_NONE) {
    PM_ERROR(
        "%s: Error: : %d/%d %d:%3d:- Cannot set rx-only/tx-only mode if a "
        "loopback is enabled",
        __func__,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_id,
        dev_id_of_port);
    return BF_INVALID_ARG;
  }

  pm_port_exclusive_access_start(dev_id, port_info);
  bf_pm_port_direction_configure(dev_id, port_hdl, port_dir);
  pm_port_exclusive_access_end(dev_id, port_info);

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_direction_get(bf_dev_id_t dev_id,
                                     bf_pal_front_port_handle_t *port_hdl,
                                     bf_pm_port_dir_e *port_dir) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;
  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!port_dir) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) return BF_INVALID_ARG;

  *port_dir = port_info->port_dir;

  return BF_SUCCESS;
}

static bf_status_t bf_pm_internal_port_delete(bf_dev_id_t dev_id,
                                              bf_dev_port_t dev_port) {
  bf_status_t sts = BF_SUCCESS;
  bf_pm_port_info_t *port_info;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;

  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) {
    PM_ERROR("Unable to get internal port info for dev : %d : dev-port : %d ",
             dev_id,
             dev_port);
    return BF_INVALID_ARG;
  }
  dev_port = port_info->dev_port;
  if (!port_info->is_added) {
    // Exit cleanly
    return BF_SUCCESS;
  }

  sts = bf_pm_port_disable(dev_id, &port_info->pltfm_port_info.port_hdl);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to disable internal port for dev : %d : front port : %d/%d "
        "(%d) : %s "
        "(%d)",
        dev_id,
        port_info->pltfm_port_info.port_hdl.conn_id,
        port_info->pltfm_port_info.port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  pm_port_exclusive_access_start(dev_id, port_info);

  // Call the SDE port delete function
  sts = pm_port_rmv(dev_id, dev_port, false);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Error in internal port delete for dev : %d : front port : %d/%d (%d) "
        ": %s "
        "(%d)",
        dev_id,
        port_info->pltfm_port_info.port_hdl.conn_id,
        port_info->pltfm_port_info.port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    goto end;
  }

  // Update the port info in the database
  pm_port_info_init_this_port(dev_id, port_info);

end:
  pm_port_exclusive_access_end(dev_id, port_info);
  return sts;
}

// Handle internal port-add or delete
static bf_status_t bf_pm_handle_internal_port(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl,
    bool is_add,
    bf_port_speed_t speed,
    uint32_t n_lanes,
    bf_fec_type_t fec) {
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;

  // Only support folded pipeline SKUs, Tofino-1 32Q or Tofino-2 64Q.
  uint32_t internal_pipes = lld_get_internal_pipe_numb(dev_id);
  if (internal_pipes == 0) {
    // No internal pipe
    return BF_SUCCESS;
  }

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  dev_port = port_info->dev_port;

  // Handle manual add/del case
  if ((port_info->is_internal) &&
      (port_hdl->conn_id >= INTERNAL_PORT_CONN_ID_START)) {
    if ((speed != BF_SPEED_100G) && bf_lld_dev_is_tof1(dev_id)) {
      PM_TRACE(
          "WARNING: %d : %d/%d All channels/ports in the quad must have same "
          "speed",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id);
    }

    if (!is_add) {
      sts = bf_pm_internal_port_delete(dev_id, dev_port);
      port_info->is_iport_user_created = false;
    } else {
      /* normal port may get deleted and readded for simple change like FEC
       * but speed may remain same. In this case donot act on internal-port
       */
      if (port_info->speed != speed) {
        sts = pm_add_internal_port_in_lb_mode(
            dev_id, port_info, speed, n_lanes, fec, true);
      }
      port_info->is_iport_user_created = true;
    }
  }
  return sts;
}

bf_status_t bf_pm_port_add(bf_dev_id_t dev_id,
                           bf_pal_front_port_handle_t *port_hdl,
                           bf_port_speed_t speed,
                           bf_fec_type_t fec) {
  if (port_hdl) {
    PM_TRACE("%d:%d/%d:%d(speed):%d(fec)",
             dev_id,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             speed,
             fec);
  }
  uint32_t n_lanes = 0;
  if (bf_port_get_default_lane_numb(dev_id, speed, &n_lanes) != BF_SUCCESS)
    return BF_INVALID_ARG;
  return bf_pm_port_add_with_lanes(dev_id, port_hdl, speed, n_lanes, fec);
}

bf_status_t bf_pm_port_add_with_lanes(bf_dev_id_t dev_id,
                                      bf_pal_front_port_handle_t *port_hdl,
                                      bf_port_speed_t speed,
                                      uint32_t n_lanes,
                                      bf_fec_type_t fec) {
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_pal_front_port_cb_cfg_t port_cfg = {0};
  bool port_add_failed = false;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d:%d(speed):%d(fec):%d(n_lanes)",
           dev_id,
           port_hdl->conn_id,
           port_hdl->chnl_id,
           speed,
           fec,
           n_lanes);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  dev_port = port_info->dev_port;

  // Lock early to eliminate race conditions on add
  pm_port_exclusive_access_start(dev_id, port_info);

  if (port_info->is_added) {
    PM_DEBUG("port already exists for dev : %d : front port : %d/%d ",
             dev_id,
             port_hdl->conn_id,
             port_hdl->chnl_id);
    sts = BF_SUCCESS;
    goto end;
  }

  if ((port_info->is_internal) &&
      (port_hdl->conn_id >= INTERNAL_PORT_CONN_ID_START)) {
    sts =
        bf_pm_handle_internal_port(dev_id, port_hdl, true, speed, n_lanes, fec);
    goto end;
  }

  // Validate port attributes
  if (!pm_port_valid_speed_and_channel(dev_id, dev_port, speed, n_lanes, fec)) {
    sts = BF_INVALID_ARG;
    goto end;
  }

  // init to default values
  pm_port_info_init_this_port(dev_id, port_info);

  port_info->speed = speed;
  port_info->n_lanes = n_lanes;
  port_info->kr_policy = PM_KR_DEFAULT;
  port_info->an_policy = PM_AN_DEFAULT;

  // Initialize the port cfg to be passed to the platform module
  pm_port_cfg_set(dev_id, dev_port, &port_cfg);

  // Call the pre-port-add platform cfg set interface defined by the platforms
  // module
  if (pltfm_interface.pltfm_pre_port_add_cfg_set && !port_info->is_internal) {
    sts = pltfm_interface.pltfm_pre_port_add_cfg_set(port_hdl, &port_cfg);
    if (sts != BF_SUCCESS) {
      char *speed_str = "";
      bf_port_speed_to_str(speed, &speed_str);
      PM_ERROR(
          "Error in setting pltfm pre-port-add cfg for dev : %d : front port : "
          "%d/%d (%d) :  cfg-speed:%s n-lanes:%d err: %s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          dev_port,
          speed_str,
          n_lanes,
          bf_err_str(sts),
          sts);
      goto end;
    }
  }

  // Call the SDE port add function
  sts = pm_port_add(dev_id, dev_port, speed, n_lanes, fec);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Error in SDE port add for dev : %d : front port : %d/%d (%d) : %s "
        "(%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    port_add_failed = true;
    goto end;
  }

  // clear the port-direction for default fsm
  bf_pm_port_direction_clear(dev_id, port_hdl);

  // Call the post-port-add platform cfg set interface defined by the platforms
  // module
  if (pltfm_interface.pltfm_post_port_add_cfg_set && !port_info->is_internal) {
    sts = pltfm_interface.pltfm_post_port_add_cfg_set(port_hdl, &port_cfg);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Error in setting pltfm post-port-add cfg for dev : %d : front port "
          ": %d/%d (%d) : %s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
    }
  }

  port_info->is_added = true;
  port_info->fec_type = fec;

  // Configure the serdes for this port
  if (!port_info->is_internal) {
    // FIXME: moved up from below
    bool is_sw_model = false;
    sts = bf_drv_device_type_get(dev_id, &is_sw_model);
    if (sts != BF_SUCCESS) {
      PM_ERROR("Unable to get the device type for dev : %d : %s (%d)",
               dev_id,
               bf_err_str(sts),
               sts);
    }

    if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
      // Serdes TX-config will happen post port-enable
      // after module is detected
      goto end;
    }

    if (!is_sw_model) {
      sts = pm_serdes_config_set(dev_id, port_hdl);
      if (sts != BF_SUCCESS) {
        PM_ERROR(
            "Unable to configure the serdes for dev : %d : front port : %d/%d "
            "(%d) : %s (%d)",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            dev_port,
            bf_err_str(sts),
            sts);
        goto end;
      }
    }

    /* Port mode transition SW workaround */
    sts = bf_drv_complete_port_mode_transition_wa(dev_id, dev_port, speed);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to complete the SW workaround for dev : %d : front port : "
          "%d/%d "
          "(%d) "
          ": %s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      goto end;
    }
  }

end:
  pm_port_exclusive_access_end(dev_id, port_info);

  if (port_add_failed) {
    // Call the pre-port-delete platform to cleanup itself
    if (pltfm_interface.pltfm_pre_port_delete_cfg_set &&
        !port_info->is_internal) {
      sts = pltfm_interface.pltfm_pre_port_delete_cfg_set(port_hdl, &port_cfg);
      if (sts != BF_SUCCESS) {
        PM_ERROR(
            "Error in setting pltfm pre-port-delete cfg for dev : %d : front "
            "port : %d/%d (%d) : %s (%d)",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            dev_port,
            bf_err_str(sts),
            sts);
      }
    }
    pm_port_info_init_this_port(dev_id, port_info);
  }

  return sts;
}

bf_status_t bf_pm_port_add_all(bf_dev_id_t dev_id,
                               bf_port_speed_t speed,
                               bf_fec_type_t fec) {
  PM_TRACE("%d:%d(speed):%d(fec)", dev_id, speed, fec);
  uint32_t n_lanes = 0;
  if (bf_port_get_default_lane_numb(dev_id, speed, &n_lanes) != BF_SUCCESS)
    return BF_INVALID_ARG;
  return bf_pm_port_add_all_with_lanes(dev_id, speed, n_lanes, fec);
}

/** WIP
 */
bf_status_t bf_pm_port_add_all_with_lanes(bf_dev_id_t dev_id,
                                          bf_port_speed_t speed,
                                          uint32_t n_lanes,
                                          bf_fec_type_t fec) {
  bf_status_t sts = BF_SUCCESS;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_dev_port_t dev_port = 0;
  bf_dev_id_t dev_id_of_port = 0;  // for checking

  PM_TRACE("%d:%d(speed):%d(fec):%d(n_lanes)", dev_id, speed, fec, n_lanes);

  // Get the first port in the system
  sts = bf_pm_port_front_panel_port_get_first(dev_id, &port_hdl);
  if (sts != BF_SUCCESS) {
    return BF_INVALID_ARG;
  }

  while (sts == BF_SUCCESS) {
    sts =
        pm_front_port_handle_to_dev_port(&port_hdl, &dev_id_of_port, &dev_port);
    if (sts != BF_SUCCESS) {
      return sts;
    }
    if (dev_id_of_port != dev_id) {
      PM_DEBUG("%s : Warning : %d/%d found on dev%d : exp dev%d",
               __func__,
               port_hdl.conn_id,
               port_hdl.chnl_id,
               dev_id_of_port,
               dev_id);
      return BF_INVALID_ARG;
    }
    /* Add the port */
    if (pm_port_valid_speed_and_channel(
            dev_id, dev_port, speed, n_lanes, fec)) {
      bf_pm_port_add_with_lanes(dev_id, &port_hdl, speed, n_lanes, fec);
    }
    // Get the next port in the system
    sts =
        bf_pm_port_front_panel_port_get_next(dev_id, &port_hdl, &next_port_hdl);
    if (sts == BF_OBJECT_NOT_FOUND) break;
    if (sts != BF_SUCCESS) {
      return sts;
    }
    port_hdl = next_port_hdl;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_enable(bf_dev_id_t dev_id,
                              bf_pal_front_port_handle_t *port_hdl) {
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;
  bool is_sw_model = false;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d", dev_id, port_hdl->conn_id, port_hdl->chnl_id);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_DEBUG("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  dev_port = port_info->dev_port;

  if (!port_info->is_added) {
    return BF_INVALID_ARG;
  }

  /* Check if port is already enabled */
  if (port_info->admin_state == PM_PORT_ENABLED) {
    return BF_SUCCESS;
  }

  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    bf_pal_front_port_cb_cfg_t port_cfg = {0};
    // Initialize the port cfg to be passed to the platform module
    pm_port_cfg_set(dev_id, port_info->dev_port, &port_cfg);

    // Call the pre-port-enable platform cfg set interface defined by the
    // platforms module
    if (pltfm_interface.pltfm_pre_port_enable_cfg_set &&
        !port_info->is_internal) {
      sts = pltfm_interface.pltfm_pre_port_enable_cfg_set(port_hdl, &port_cfg);
      if (sts != BF_SUCCESS) {
        PM_ERROR(
            "Error in setting pltfm pre-port-enable cfg for dev : %d : front "
            "port : %d/%d (%d) : %s (%d)",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            port_info->dev_port,
            bf_err_str(sts),
            sts);
        goto end;
      }
    }
  }

  pm_port_exclusive_access_start(dev_id, port_info);

  sts = bf_drv_device_type_get(dev_id, &is_sw_model);  // Emulator or model
  if (is_sw_model) {
    // blindly set on emulator(model) since it doesn't run with platform code
    if (!bf_pm_intf_is_device_family_tofino3(dev_id)) {
      // FIXME: AddressSanitizer error on tofino3
      port_info->is_ready_for_bringup = true;
      port_info->serdes_rx_ready_for_dfe = true;
      port_info->module_ready_for_link = false;
    }
  }

  if (!port_info->is_internal || (port_info->lpbk_mode != BF_LPBK_NONE)) {
    sts = pm_port_bring_up(dev_id, port_hdl);
    if ((sts != BF_SUCCESS) && (sts != BF_NOT_READY)) {
      PM_ERROR(
          "Unable to bring up the port for dev : %d : front port : %d/%d (%d) "
          ": %s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      goto end;
    }
    sts = BF_SUCCESS;
  }

  // Update the port info in the database
  port_info->admin_state = PM_PORT_ENABLED;

end:
  pm_port_exclusive_access_end(dev_id, port_info);
  return sts;
}

bf_status_t bf_pm_port_enable_all(bf_dev_id_t dev_id) {
  PM_TRACE("%d", dev_id);
  bf_status_t sts = BF_SUCCESS;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr = &port_hdl;
  bf_pal_front_port_handle_t *next_port_hdl_ptr = &next_port_hdl;

  // Get the first port in the system
  sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    return BF_INVALID_ARG;
  }
  bf_pm_port_enable(dev_id, port_hdl_ptr);

  while (sts == BF_SUCCESS) {
    // Get the next port in the system
    sts = bf_pm_port_front_panel_port_get_next(
        dev_id, port_hdl_ptr, next_port_hdl_ptr);
    if (sts == BF_OBJECT_NOT_FOUND) break;
    if (sts != BF_SUCCESS) {
      return sts;
    }
    /* Delete the port */
    bf_pm_port_enable(dev_id, next_port_hdl_ptr);

    // Make the curr port hdl equal to the next port hdl
    port_hdl_ptr = next_port_hdl_ptr;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_disable(bf_dev_id_t dev_id,
                               bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_admin_state_e cur_admin_state;
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d", dev_id, port_hdl->conn_id, port_hdl->chnl_id);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  dev_port = port_info->dev_port;
  if (!port_info->is_added) {
    return BF_INVALID_ARG;
  }

  /* Check if port is already disabled */
  if (port_info->admin_state == PM_PORT_DISABLED) {
    return BF_SUCCESS;
  }

  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    bf_pal_front_port_cb_cfg_t port_cfg = {0};
    // Initialize the port cfg to be passed to the platform module
    pm_port_cfg_set(dev_id, port_info->dev_port, &port_cfg);

    // Call the pre-port-enable platform cfg set interface defined by the
    // platforms module
    if (pltfm_interface.pltfm_pre_port_disable_cfg_set &&
        !port_info->is_internal) {
      sts = pltfm_interface.pltfm_pre_port_disable_cfg_set(port_hdl, &port_cfg);
      if (sts != BF_SUCCESS) {
        PM_ERROR(
            "Error in setting pltfm pre-port-enable cfg for dev : %d : front "
            "port : %d/%d (%d) : %s (%d)",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            port_info->dev_port,
            bf_err_str(sts),
            sts);
        goto end;
      }
    }
  }

  pm_port_exclusive_access_start(dev_id, port_info);
  cur_admin_state = port_info->admin_state;
  port_info->admin_state = PM_PORT_DISABLED;
  sts = pm_port_bring_down(dev_id, port_hdl);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to bring down the port for dev : %d : front port : %d/%d (%d) "
        ": %s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    port_info->admin_state = cur_admin_state;
    goto end;
  }
  // Update the port info in the database
  port_info->last_dwn_time = -1;

  pm_port_exclusive_access_end(dev_id, port_info);
  // Call the post-port-disable platform cfg set interface defined by the
  // platforms module
  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    if (pltfm_interface.pltfm_post_port_disable_cfg_set &&
        !port_info->is_internal) {
      bf_pal_front_port_cb_cfg_t port_cfg = {0};
      // Initialize the port cfg to be passed to the platform module
      pm_port_cfg_set(dev_id, port_info->dev_port, &port_cfg);

      sts =
          pltfm_interface.pltfm_post_port_disable_cfg_set(port_hdl, &port_cfg);
      if (sts != BF_SUCCESS) {
        PM_ERROR(
            "Error in setting pltfm post-port-disable cfg for dev : %d : front "
            "port : %d/%d (%d) : %s (%d)",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            port_info->dev_port,
            bf_err_str(sts),
            sts);
      }
    }
  }

  return sts;
end:
  pm_port_exclusive_access_end(dev_id, port_info);
  return sts;
}

bf_status_t bf_pm_port_delete(bf_dev_id_t dev_id,
                              bf_pal_front_port_handle_t *port_hdl) {
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_pal_front_port_cb_cfg_t port_cfg = {0};
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d", dev_id, port_hdl->conn_id, port_hdl->chnl_id);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  dev_port = port_info->dev_port;
  if (!port_info->is_added) {
    PM_DEBUG("Port delete dev : %d port: %d/%d does not exist\n",
             dev_id,
             port_hdl->conn_id,
             port_hdl->chnl_id);
    // Exit cleanly
    return BF_SUCCESS;
  }

  if ((port_info->is_internal) &&
      (port_hdl->conn_id >= INTERNAL_PORT_CONN_ID_START)) {
    sts = bf_pm_handle_internal_port(dev_id,
                                     port_hdl,
                                     false,
                                     port_info->speed,
                                     port_info->n_lanes,
                                     BF_FEC_TYP_NONE);
    return sts;
  }

  // Clear any residual
  if (port_info->lpbk_mode != BF_LPBK_NONE) {
    bf_pm_port_loopback_mode_set(dev_id, port_hdl, BF_LPBK_NONE);
  }

  sts = bf_pm_port_disable(dev_id, port_hdl);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to disable port for dev : %d : front port : %d/%d (%d) : %s "
        "(%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  // Initialize the port cfg to be passed to the platform module
  pm_port_cfg_set(dev_id, dev_port, &port_cfg);

  // Call the pre-port-delete platform cfg set interface defined by the
  // platforms module
  if (pltfm_interface.pltfm_pre_port_delete_cfg_set &&
      !port_info->is_internal) {
    sts = pltfm_interface.pltfm_pre_port_delete_cfg_set(port_hdl, &port_cfg);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Error in setting pltfm pre-port-delete cfg for dev : %d : front "
          "port : %d/%d (%d) : %s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
    }
  }

  pm_port_exclusive_access_start(dev_id, port_info);

  // Call the SDE port delete function
  sts = pm_port_rmv(dev_id, dev_port, false);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Error in SDE port delete for dev : %d : front port : %d/%d (%d) : %s "
        "(%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    goto end;
  }

  // Call the post-port-delete platform cfg set interface defined by the
  // platforms module
  if (pltfm_interface.pltfm_post_port_delete_cfg_set &&
      !port_info->is_internal) {
    sts = pltfm_interface.pltfm_post_port_delete_cfg_set(port_hdl, &port_cfg);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Error in setting pltfm post-port-delete cfg for dev : %d : front "
          "port : %d/%d (%d) : %s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
    }
  }

  // Update the port info in the database
  pm_port_info_init_this_port(dev_id, port_info);

end:
  pm_port_exclusive_access_end(dev_id, port_info);
  return sts;
}

bf_status_t bf_pm_port_delete_all(bf_dev_id_t dev_id) {
  PM_TRACE("%d", dev_id);
  bf_status_t sts = BF_SUCCESS;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr = &port_hdl;
  bf_pal_front_port_handle_t *next_port_hdl_ptr = &next_port_hdl;

  // Get the first port in the system
  sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    return BF_INVALID_ARG;
  }
  bf_pm_port_delete(dev_id, port_hdl_ptr);

  while (sts == BF_SUCCESS) {
    // Get the next port in the system
    sts = bf_pm_port_front_panel_port_get_next(
        dev_id, port_hdl_ptr, next_port_hdl_ptr);
    if (sts == BF_OBJECT_NOT_FOUND) break;
    if (sts != BF_SUCCESS) {
      return sts;
    }
    /* Delete the port */
    bf_pm_port_delete(dev_id, next_port_hdl_ptr);

    // Make the curr port hdl equal to the next port hdl
    port_hdl_ptr = next_port_hdl_ptr;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_autoneg_get(bf_dev_id_t dev_id,
                                   bf_pal_front_port_handle_t *port_hdl,
                                   bf_pm_port_autoneg_policy_e *an_policy) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (an_policy == NULL) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) {
    return BF_INVALID_ARG;
  }
  *an_policy = port_info->an_policy;

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_autoneg_set(bf_dev_id_t dev_id,
                                   bf_pal_front_port_handle_t *port_hdl,
                                   bf_pm_port_autoneg_policy_e an_policy) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d:%d(an_policy)",
           dev_id,
           port_hdl->conn_id,
           port_hdl->chnl_id,
           an_policy);
  if ((an_policy < PM_AN_DEFAULT) || (an_policy >= PM_AN_MAX))
    return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) {
    return BF_INVALID_ARG;
  }
  pm_port_exclusive_access_start(dev_id, port_info);
  port_info->an_policy = an_policy;
  pm_port_exclusive_access_end(dev_id, port_info);

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_autoneg_set_all(bf_dev_id_t dev_id,
                                       bf_pm_port_autoneg_policy_e an_policy) {
  PM_TRACE("%d:%d(an_policy)", dev_id, an_policy);
  bf_status_t sts = BF_SUCCESS;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr = &port_hdl;
  bf_pal_front_port_handle_t *next_port_hdl_ptr = &next_port_hdl;

  // Get the first port in the system
  sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    return BF_INVALID_ARG;
  }
  bf_pm_port_autoneg_set(dev_id, port_hdl_ptr, an_policy);

  while (sts == BF_SUCCESS) {
    // Get the next port in the system
    sts = bf_pm_port_front_panel_port_get_next(
        dev_id, port_hdl_ptr, next_port_hdl_ptr);
    if (sts == BF_OBJECT_NOT_FOUND) break;
    if (sts != BF_SUCCESS) {
      return sts;
    }
    bf_pm_port_autoneg_set(dev_id, next_port_hdl_ptr, an_policy);

    // Make the curr port hdl equal to the next port hdl
    port_hdl_ptr = next_port_hdl_ptr;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_adv_speed_set(const bf_dev_id_t dev_id,
                                     bf_pal_front_port_handle_t *port_hdl,
                                     const bf_port_speed_t *adv_speed_arr,
                                     const uint32_t adv_speed_cnt) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if ((!adv_speed_arr) || (adv_speed_cnt == 0)) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d:%d(adv_speed_arr[0])",
           dev_id,
           port_hdl->conn_id,
           port_hdl->chnl_id,
           adv_speed_arr[0]);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (port_info == NULL) {
    PM_ERROR("%s: Failed to get port info on dev %d: %d/%d",
             __func__,
             dev_id,
             port_hdl->conn_id,
             port_hdl->chnl_id);
    return BF_INVALID_ARG;
  }
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) {
    return BF_INVALID_ARG;
  }

  pm_port_exclusive_access_start(dev_id, port_info);
  port_info->adv_speed_map = 0;
  for (uint32_t i = 0; i < adv_speed_cnt; ++i) {
    port_info->adv_speed_map |= adv_speed_arr[i];
  }
  pm_port_exclusive_access_end(dev_id, port_info);

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_adv_fec_set(const bf_dev_id_t dev_id,
                                   bf_pal_front_port_handle_t *port_hdl,
                                   const bf_fec_type_t *adv_fec_arr,
                                   const uint32_t adv_fec_cnt) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if ((!adv_fec_arr) || (adv_fec_cnt == 0)) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d:%d(adv_fec_arr[0])",
           dev_id,
           port_hdl->conn_id,
           port_hdl->chnl_id,
           adv_fec_arr[0]);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (port_info == NULL) {
    PM_ERROR("%s: Failed to get port info on dev %d: %d/%d",
             __func__,
             dev_id,
             port_hdl->conn_id,
             port_hdl->chnl_id);
    return BF_INVALID_ARG;
  }
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) {
    return BF_INVALID_ARG;
  }

  pm_port_exclusive_access_start(dev_id, port_info);
  port_info->adv_fec_map = 0;
  for (uint32_t i = 0; i < adv_fec_cnt; ++i) {
    port_info->adv_fec_map |= adv_fec_arr[i];
  }
  pm_port_exclusive_access_end(dev_id, port_info);

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_kr_mode_get(bf_dev_id_t dev_id,
                                   bf_pal_front_port_handle_t *port_hdl,
                                   bf_pm_port_kr_mode_policy_e *kr_policy) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (kr_policy == NULL) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) {
    return BF_INVALID_ARG;
  }
  *kr_policy = port_info->kr_policy;

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_kr_mode_set(bf_dev_id_t dev_id,
                                   bf_pal_front_port_handle_t *port_hdl,
                                   bf_pm_port_kr_mode_policy_e kr_policy) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d:%d(kr_policy)",
           dev_id,
           port_hdl->conn_id,
           port_hdl->chnl_id,
           kr_policy);
  if ((kr_policy < PM_KR_DEFAULT) || (kr_policy >= PM_KR_MAX))
    return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) {
    return BF_INVALID_ARG;
  }
  pm_port_exclusive_access_start(dev_id, port_info);
  port_info->kr_policy = kr_policy;
  pm_port_exclusive_access_end(dev_id, port_info);

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_kr_mode_set_all(bf_dev_id_t dev_id,
                                       bf_pm_port_kr_mode_policy_e kr_policy) {
  PM_TRACE("%d:%d(kr_policy)", dev_id, kr_policy);
  bf_status_t sts = BF_SUCCESS;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr = &port_hdl;
  bf_pal_front_port_handle_t *next_port_hdl_ptr = &next_port_hdl;

  // Get the first port in the system
  sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    return BF_INVALID_ARG;
  }
  bf_pm_port_kr_mode_set(dev_id, port_hdl_ptr, kr_policy);

  while (sts == BF_SUCCESS) {
    // Get the next port in the system
    sts = bf_pm_port_front_panel_port_get_next(
        dev_id, port_hdl_ptr, next_port_hdl_ptr);
    if (sts == BF_OBJECT_NOT_FOUND) break;
    if (sts != BF_SUCCESS) {
      return sts;
    }
    bf_pm_port_kr_mode_set(dev_id, next_port_hdl_ptr, kr_policy);

    // Make the curr port hdl equal to the next port hdl
    port_hdl_ptr = next_port_hdl_ptr;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_term_mode_get(bf_dev_id_t dev_id,
                                     bf_pal_front_port_handle_t *port_hdl,
                                     int ln,
                                     bf_pm_port_term_mode_e *term_mode) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (term_mode == NULL) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) {
    return BF_INVALID_ARG;
  }
  if ((ln >= (int)port_info->n_lanes) || (ln >= MAX_LANES_PER_PORT)) {
    PM_ERROR(
        "Invalid logical lane (%d) for dev : %d : front port : %d/%d "
        "(n_lanes=%d)",
        ln,
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        port_info->n_lanes);
    return BF_INVALID_ARG;
  }
  *term_mode = port_info->term_mode[ln];

  return BF_SUCCESS;
}

// note: ln=-1 means "all lanes"
bf_status_t bf_pm_port_term_mode_set(bf_dev_id_t dev_id,
                                     bf_pal_front_port_handle_t *port_hdl,
                                     int ln,
                                     bf_pm_port_term_mode_e term_mode) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d:%d(term_mode)",
           dev_id,
           port_hdl->conn_id,
           port_hdl->chnl_id,
           term_mode);
  if ((term_mode < PM_TERM_MODE_AC) || (term_mode > PM_TERM_MODE_MAX))
    return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) {
    return BF_INVALID_ARG;
  }
  if ((ln >= (int)port_info->n_lanes) || (ln >= MAX_LANES_PER_PORT)) {
    PM_ERROR(
        "Invalid logical lane (%d) for dev : %d : front port : %d/%d "
        "(n_lanes=%d)",
        ln,
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        port_info->n_lanes);
    return BF_INVALID_ARG;
  }
  pm_port_exclusive_access_start(dev_id, port_info);
  {
    int first_ln = (ln == -1) ? 0 : ln;
    int last_ln = (ln == -1) ? (int)port_info->n_lanes - 1 : ln;
    for (int l = first_ln; l <= last_ln; l++) {
      port_info->term_mode[l] = term_mode;
    }
  }
  pm_port_exclusive_access_end(dev_id, port_info);

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_term_mode_set_all(bf_dev_id_t dev_id,
                                         bf_pm_port_term_mode_e term_mode) {
  PM_TRACE("%d:%d(term_mode)", dev_id, term_mode);
  bf_status_t sts = BF_SUCCESS;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr = &port_hdl;
  bf_pal_front_port_handle_t *next_port_hdl_ptr = &next_port_hdl;

  // Get the first port in the system
  sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    return BF_INVALID_ARG;
  }
  bf_pm_port_term_mode_set(dev_id, port_hdl_ptr, -1, term_mode);
  while (sts == BF_SUCCESS) {
    // Get the next port in the system
    sts = bf_pm_port_front_panel_port_get_next(
        dev_id, port_hdl_ptr, next_port_hdl_ptr);
    if (sts == BF_OBJECT_NOT_FOUND) break;
    if (sts != BF_SUCCESS) {
      return sts;
    }
    bf_pm_port_term_mode_set(dev_id, next_port_hdl_ptr, -1, term_mode);

    // Make the curr port hdl equal to the next port hdl
    port_hdl_ptr = next_port_hdl_ptr;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_precoding_tx_get(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl,
    uint32_t ln,
    bf_pm_port_precoding_policy_e *pc_policy) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (pc_policy == NULL) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) {
    return BF_INVALID_ARG;
  }
  if ((ln >= port_info->n_lanes) || (ln >= MAX_LANES_PER_PORT)) {
    PM_ERROR(
        "Invalid logical lane (%d) for dev : %d : front port : %d/%d "
        "(n_lanes=%d)",
        ln,
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        port_info->n_lanes);
    return BF_INVALID_ARG;
  }
  *pc_policy = port_info->pc_tx_policy[ln];

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_precoding_rx_get(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl,
    uint32_t ln,
    bf_pm_port_precoding_policy_e *pc_policy) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (pc_policy == NULL) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) {
    return BF_INVALID_ARG;
  }
  if ((ln >= port_info->n_lanes) || (ln >= MAX_LANES_PER_PORT)) {
    PM_ERROR(
        "Invalid logical lane (%d) for dev : %d : front port : %d/%d "
        "(n_lanes=%d)",
        ln,
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        port_info->n_lanes);
    return BF_INVALID_ARG;
  }
  *pc_policy = port_info->pc_rx_policy[ln];

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_precoding_tx_clear(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl_list,
    uint32_t len) {
  bf_pm_port_info_t *port_info;
  uint32_t i, ln;
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl_list) return BF_INVALID_ARG;

  for (i = 0; i < len; i++) {
    if (port_hdl_list[i].chnl_id != 0) {
      continue;
    }
    sts = bf_pm_port_info_all_get(__func__,
                                  dev_id,
                                  &port_hdl_list[i],
                                  &port_info,
                                  &dev_id_of_port,
                                  &dev_port);
    if (sts != BF_SUCCESS) return sts;

    if (!bf_pm_intf_is_device_family_tofino2(
            port_info->pltfm_port_info.dev_id)) {
      continue;
    }
    if (port_info->is_added) {
      pm_port_exclusive_access_start(dev_id, port_info);
      for (ln = 0; ln < MAX_LANES_PER_PORT; ln++) {
        port_info->pc_tx_policy[ln] = PM_PRECODING_INVALID;
      }
      pm_port_exclusive_access_end(dev_id, port_info);
    }
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_precoding_tx_set(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl,
    uint32_t ln,
    bf_pm_port_precoding_policy_e pc_policy) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) {
    return BF_INVALID_ARG;
  }
  if ((ln >= port_info->n_lanes) || (ln >= MAX_LANES_PER_PORT)) {
    PM_ERROR(
        "Invalid logical lane (%d) for dev : %d : front port : %d/%d "
        "(n_lanes=%d)",
        ln,
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        port_info->n_lanes);
    return BF_INVALID_ARG;
  }
  pm_port_exclusive_access_start(dev_id, port_info);
  if ((port_info->pc_tx_policy[ln] == PM_PRECODING_FORCE_DISABLE) ||
      (port_info->pc_tx_policy[ln] == PM_PRECODING_FORCE_ENABLE)) {
    if ((pc_policy == PM_PRECODING_ENABLE) ||
        (pc_policy == PM_PRECODING_DISABLE)) {
      PM_TRACE(
          "%d:%d/%d.%d : %d(Using forced Tx pc_policy: %s). Ignore "
          "application.",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          ln,
          port_info->pc_tx_policy[ln],
          port_info->pc_tx_policy[ln] == PM_PRECODING_FORCE_ENABLE
              ? "Forced precoding Enabled"
              : "Forced precoding Disabled");
    } else {
      if ((port_info->pc_tx_policy[ln] == PM_PRECODING_FORCE_DISABLE) &&
          (pc_policy == PM_PRECODING_FORCE_ENABLE)) {
        // change forced policy
        port_info->pc_tx_policy[ln] = pc_policy;
      } else if ((port_info->pc_tx_policy[ln] == PM_PRECODING_FORCE_ENABLE) &&
                 (pc_policy == PM_PRECODING_FORCE_DISABLE)) {
        // change forced policy
        port_info->pc_tx_policy[ln] = pc_policy;
      }
    }
  } else {
    port_info->pc_tx_policy[ln] = pc_policy;
  }
  pm_port_exclusive_access_end(dev_id, port_info);

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_precoding_rx_clear(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl_list,
    uint32_t len) {
  bf_pm_port_info_t *port_info;
  uint32_t i, ln;
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl_list) return BF_INVALID_ARG;

  for (i = 0; i < len; i++) {
    if (port_hdl_list[i].chnl_id != 0) {
      continue;
    }
    sts = bf_pm_port_info_all_get(__func__,
                                  dev_id,
                                  &port_hdl_list[i],
                                  &port_info,
                                  &dev_id_of_port,
                                  &dev_port);
    if (sts != BF_SUCCESS) return sts;

    if (!bf_pm_intf_is_device_family_tofino2(
            port_info->pltfm_port_info.dev_id)) {
      continue;
    }
    if (port_info->is_added) {
      pm_port_exclusive_access_start(dev_id, port_info);
      for (ln = 0; ln < MAX_LANES_PER_PORT; ln++) {
        port_info->pc_rx_policy[ln] = PM_PRECODING_INVALID;
      }
      pm_port_exclusive_access_end(dev_id, port_info);
    }
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_precoding_rx_set(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl,
    uint32_t ln,
    bf_pm_port_precoding_policy_e pc_policy) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) {
    return BF_INVALID_ARG;
  }
  if ((ln >= port_info->n_lanes) || (ln >= MAX_LANES_PER_PORT)) {
    PM_ERROR(
        "Invalid logical lane (%d) for dev : %d : front port : %d/%d "
        "(n_lanes=%d)",
        ln,
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        port_info->n_lanes);
    return BF_INVALID_ARG;
  }
  pm_port_exclusive_access_start(dev_id, port_info);
  if ((port_info->pc_rx_policy[ln] == PM_PRECODING_FORCE_DISABLE) ||
      (port_info->pc_rx_policy[ln] == PM_PRECODING_FORCE_ENABLE)) {
    if ((pc_policy == PM_PRECODING_ENABLE) ||
        (pc_policy == PM_PRECODING_DISABLE)) {
      PM_TRACE(
          "%d:%d/%d.%d : %d(Using forced Rx pc_policy: %s). Ignore "
          "application.",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          ln,
          port_info->pc_rx_policy[ln],
          port_info->pc_rx_policy[ln] == PM_PRECODING_FORCE_ENABLE
              ? "Forced precoding Enabled"
              : "Forced precoding Disabled");
    } else {
      if ((port_info->pc_rx_policy[ln] == PM_PRECODING_FORCE_DISABLE) &&
          (pc_policy == PM_PRECODING_FORCE_ENABLE)) {
        // change forced policy
        port_info->pc_rx_policy[ln] = pc_policy;
      } else if ((port_info->pc_rx_policy[ln] == PM_PRECODING_FORCE_ENABLE) &&
                 (pc_policy == PM_PRECODING_FORCE_DISABLE)) {
        // change forced policy
        port_info->pc_rx_policy[ln] = pc_policy;
      }
    }
  } else {
    port_info->pc_rx_policy[ln] = pc_policy;
  }
  pm_port_exclusive_access_end(dev_id, port_info);

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_precodoing_set_all(
    bf_dev_id_t dev_id, bf_pm_port_precoding_policy_e pc_policy) {
  int ln, num_lanes;

  PM_TRACE("%d:%d(pc_policy)", dev_id, pc_policy);
  bf_status_t sts = BF_SUCCESS;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr = &port_hdl;
  bf_pal_front_port_handle_t *next_port_hdl_ptr = &next_port_hdl;

  // Get the first port in the system
  sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    return BF_INVALID_ARG;
  }
  bf_pm_pltfm_front_port_num_lanes_get(dev_id, port_hdl_ptr, &num_lanes);

  for (ln = 0; ln < num_lanes; ln++) {
    bf_pm_port_precoding_tx_set(dev_id, port_hdl_ptr, ln, pc_policy);
    bf_pm_port_precoding_rx_set(dev_id, port_hdl_ptr, ln, pc_policy);
  }
  while (sts == BF_SUCCESS) {
    // Get the next port in the system
    sts = bf_pm_port_front_panel_port_get_next(
        dev_id, port_hdl_ptr, next_port_hdl_ptr);
    if (sts == BF_OBJECT_NOT_FOUND) break;
    if (sts != BF_SUCCESS) {
      return sts;
    }
    for (ln = 0; ln < num_lanes; ln++) {
      bf_pm_port_precoding_tx_set(dev_id, port_hdl_ptr, ln, pc_policy);
      bf_pm_port_precoding_rx_set(dev_id, port_hdl_ptr, ln, pc_policy);
    }

    // Make the curr port hdl equal to the next port hdl
    port_hdl_ptr = next_port_hdl_ptr;
  }

  return BF_SUCCESS;
}

static void pm_port_term_mode_apply(bf_dev_id_t dev_id,
                                    bf_pal_front_port_handle_t *port_hdl) {
  pm_serdes_term_mode_set(dev_id, port_hdl);
}

/**
 * Configure precoding settings for a port (when enabled)
 */
static void pm_port_precoding_apply(bf_dev_id_t dev_id,
                                    bf_pal_front_port_handle_t *port_hdl) {
  bf_status_t sts;
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;
  int ln, num_lanes;
  bf_pm_pltfm_front_port_num_lanes_get(dev_id, port_hdl, &num_lanes);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  for (ln = 0; ln < num_lanes; ln++) {
    bf_pm_port_precoding_policy_e tx_pc_policy, rx_pc_policy;

    sts = bf_pm_port_precoding_rx_get(dev_id, port_hdl, ln, &rx_pc_policy);
    if (sts != BF_SUCCESS) {
      PM_ERROR("Unable to get Rx port precoding settings for %d/%d.%d <err=%d>",
               port_hdl->conn_id,
               port_hdl->chnl_id,
               ln,
               sts);
      rx_pc_policy = PM_PRECODING_DISABLE;
    }
    sts = bf_pm_port_precoding_tx_get(dev_id, port_hdl, ln, &tx_pc_policy);
    if (sts != BF_SUCCESS) {
      PM_ERROR("Unable to get Tx port precoding settings for %d/%d.%d <err=%d>",
               port_hdl->conn_id,
               port_hdl->chnl_id,
               ln,
               sts);
      tx_pc_policy = PM_PRECODING_DISABLE;
    }
    // convert ucli values
    if (rx_pc_policy == PM_PRECODING_FORCE_DISABLE) {
      rx_pc_policy = PM_PRECODING_DISABLE;
    } else if (rx_pc_policy == PM_PRECODING_FORCE_ENABLE) {
      rx_pc_policy = PM_PRECODING_ENABLE;
    }
    if (tx_pc_policy == PM_PRECODING_FORCE_DISABLE) {
      tx_pc_policy = PM_PRECODING_DISABLE;
    } else if (tx_pc_policy == PM_PRECODING_FORCE_ENABLE) {
      tx_pc_policy = PM_PRECODING_ENABLE;
    }
    bf_dev_family_t dev_family =
        bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));
    switch (dev_family) {
      case BF_DEV_FAMILY_TOFINO2:
        bf_tof2_serdes_fw_precode_set(dev_id,
                                      port_info->dev_port,
                                      ln,
                                      (tx_pc_policy == PM_PRECODING_ENABLE),
                                      (rx_pc_policy == PM_PRECODING_ENABLE),
                                      (tx_pc_policy == PM_PRECODING_ENABLE),
                                      (rx_pc_policy == PM_PRECODING_ENABLE));
        break;
      case BF_DEV_FAMILY_TOFINO3:
        bf_tof3_serdes_precode_set(dev_id,
                                   port_info->dev_port,
                                   ln,
                                   (tx_pc_policy == PM_PRECODING_ENABLE),
                                   (rx_pc_policy == PM_PRECODING_ENABLE));
        break;
      default:
        PM_ERROR("Unknown device familly: %d\n", dev_family);
        return;
    }
    PM_DEBUG("Port precoding settings for %d/%d.%d: Tx=%s : Rx=%s",
             port_hdl->conn_id,
             port_hdl->chnl_id,
             ln,
             (tx_pc_policy == PM_PRECODING_ENABLE) ? "Enbl" : "Dsbl",
             (rx_pc_policy == PM_PRECODING_ENABLE) ? "Enbl" : "Dsbl");
  }
}

bf_status_t bf_pm_port_prbs_set(bf_dev_id_t dev_id,
                                bf_pal_front_port_handle_t *port_hdl_list,
                                uint32_t len,
                                bf_port_prbs_speed_t prbs_speed,
                                bf_port_prbs_mode_t prbs_mode) {
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_pal_front_port_handle_t port_hdl;
  uint32_t i, j;
  bf_pal_front_port_cb_cfg_t port_cfg = {0};
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl_list) return BF_INVALID_ARG;
  if ((prbs_speed < BF_PORT_PRBS_SPEED_10G) ||
      (prbs_speed >= BF_PORT_PRBS_SPEED_MAX)) {
    return BF_INVALID_ARG;
  }
  if ((prbs_mode < BF_PORT_PRBS_MODE_31) ||
      (prbs_mode >= BF_PORT_PRBS_MODE_MAX)) {
    return BF_INVALID_ARG;
  }

  for (i = 0; i < len; i++) {
    if (port_hdl_list[i].chnl_id != 0) {
      continue;
    }
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        &port_hdl_list[i], &dev_id_of_port, &dev_port);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to get the dev port for dev : %d : front port : %d/%d : %s "
          "(%d)",
          dev_id,
          port_hdl_list[i].conn_id,
          port_hdl_list[i].chnl_id,
          bf_err_str(sts),
          sts);
      return sts;
    }

    // Initialize the port cfg to be passed to the platform module
    port_cfg.speed_cfg =
        (prbs_speed == BF_PORT_PRBS_SPEED_10G) ? BF_SPEED_10G : BF_SPEED_25G;
    // For the time being, prbs can only be configured on all mac lanes at once
    port_cfg.num_lanes = lld_get_chnls_dev_port(dev_id, dev_port);
    port_cfg.is_an_on = pm_port_should_autoneg(dev_id, dev_port);

    if (pltfm_interface.pltfm_port_prbs_cfg_set) {
      sts =
          pltfm_interface.pltfm_port_prbs_cfg_set(&port_hdl_list[i], &port_cfg);
      if (sts != BF_SUCCESS) {
        PM_ERROR(
            "Error in initializing prbs cfg for dev : %d : front "
            "port : %d/%d (%d) : %s (%d)",
            dev_id,
            port_hdl_list[i].conn_id,
            port_hdl_list[i].chnl_id,
            dev_port,
            bf_err_str(sts),
            sts);
      }
    }

    sts = bf_port_prbs_init(dev_id, dev_port, prbs_speed, prbs_mode);
    if (sts != BF_SUCCESS) {
      goto end;
    }

    port_hdl.conn_id = port_hdl_list[i].conn_id;
    for (j = 0; j < (uint32_t)port_cfg.num_lanes; j++) {
      port_hdl.chnl_id = j;

      sts = bf_pm_port_serdes_cfg_set(dev_id, &port_hdl);
      if (sts != BF_SUCCESS) {
        PM_ERROR(
            "Unable to set the serdes cfg for dev : %d : front port : %d/%d "
            "(%d) : %s "
            "(%d)",
            dev_id,
            port_hdl.conn_id,
            port_hdl.chnl_id,
            dev_port,
            bf_err_str(sts),
            sts);
        goto end;
      }
    }

    sts = bf_port_prbs_rx_eq_run(dev_id, dev_port);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to start ICAL for dev : %d : front port : %d/%d : %s (%d)",
          dev_id,
          port_hdl_list[i].conn_id,
          port_hdl_list[i].chnl_id,
          bf_err_str(sts),
          sts);
      goto end;
    }
  }

  // Wait for ICAL to complete
  bf_sys_sleep(30);

  for (i = 0; i < len; i++) {
    if (port_hdl_list[i].chnl_id != 0) {
      continue;
    }
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        &port_hdl_list[i], &dev_id_of_port, &dev_port);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to get the dev port for dev : %d : front port : %d/%d : %s "
          "(%d)",
          dev_id,
          port_hdl_list[i].conn_id,
          port_hdl_list[i].chnl_id,
          bf_err_str(sts),
          sts);
      return sts;
    }

    sts = bf_port_prbs_cmp_mode_set(dev_id, dev_port);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to set the prbs compare mode for dev : %d : front port : "
          "%d/%d : %s (%d)",
          dev_id,
          port_hdl_list[i].conn_id,
          port_hdl_list[i].chnl_id,
          bf_err_str(sts),
          sts);
      goto end;
    }
  }

  return BF_SUCCESS;
end:
  PM_ERROR("Unable to setup PRBS for dev : %d : front port : %d/%d : %s (%d)",
           dev_id,
           port_hdl_list[i].conn_id,
           port_hdl_list[i].chnl_id,
           bf_err_str(sts),
           sts);
  return sts;
}

bf_status_t bf_pm_port_prbs_cleanup(bf_dev_id_t dev_id,
                                    bf_pal_front_port_handle_t *port_hdl_list,
                                    uint32_t len) {
  uint32_t i;
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl_list) return BF_INVALID_ARG;

  for (i = 0; i < len; i++) {
    if (port_hdl_list[i].chnl_id != 0) {
      continue;
    }
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        &port_hdl_list[i], &dev_id_of_port, &dev_port);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to get the dev port for dev : %d : front port : %d/%d : %s "
          "(%d)",
          dev_id,
          port_hdl_list[i].conn_id,
          port_hdl_list[i].chnl_id,
          bf_err_str(sts),
          sts);
      return sts;
    }

    sts = bf_port_prbs_cleanup(dev_id, dev_port);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to cleanup PRBS for dev : %d : front port : %d/%d : %s (%d)",
          dev_id,
          port_hdl_list[i].conn_id,
          port_hdl_list[i].chnl_id,
          bf_err_str(sts),
          sts);
      return sts;
    }
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_prbs_stats_get(bf_dev_id_t dev_id,
                                      bf_pal_front_port_handle_t *port_hdl,
                                      bf_sds_debug_stats_t *stats) {
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!stats) return BF_INVALID_ARG;

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to get the dev port for dev : %d : front port : %d/%d : %s "
        "(%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_port_prbs_debug_stats_get(dev_id, dev_port, stats);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to get prbs stats for dev : %d : front port : %d/%d : %s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_oper_status_get(bf_dev_id_t dev_id,
                                       bf_pal_front_port_handle_t *port_hdl,
                                       bool *oper_status) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!oper_status) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) return BF_INVALID_ARG;

  *oper_status = port_info->oper_status == PM_PORT_UP ? true : false;

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_speed_get(bf_dev_id_t dev_id,
                                 bf_pal_front_port_handle_t *port_hdl,
                                 bf_port_speed_t *speed) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!speed) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) return BF_INVALID_ARG;

  *speed = port_info->speed;

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_speed_set(bf_dev_id_t dev_id,
                                 bf_pal_front_port_handle_t *port_hdl,
                                 bf_port_speed_t speed) {
  if (port_hdl) {
    PM_TRACE("%d:%d/%d:%d(speed)",
             dev_id,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             speed);
  }
  uint32_t n_lanes = 0;
  if (bf_port_get_default_lane_numb(dev_id, speed, &n_lanes) != BF_SUCCESS)
    return BF_INVALID_ARG;
  return bf_pm_port_speed_set_with_lanes(dev_id, port_hdl, speed, n_lanes);
}
bf_status_t bf_pm_port_speed_set_with_lanes(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl,
    bf_port_speed_t speed,
    uint32_t n_lanes) {
  bf_status_t sts = BF_SUCCESS;
  bf_pm_port_info_t *port_info;
  bool is_fec_valid;
  bf_pm_port_admin_state_e port_admin_state;
  bf_fec_type_t port_fec;
  bf_dev_port_t dev_port;
  bf_dev_id_t dev_id_of_port = 0;
  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  dev_port = port_info->dev_port;
  if (!port_info->is_added) return BF_INVALID_ARG;

  if ((port_info->speed == speed) && (port_info->n_lanes == n_lanes))
    return BF_SUCCESS;

  // Cache the fec and the admin state before we delete the port
  port_fec = port_info->fec_type;
  port_admin_state = port_info->admin_state;

  if (bf_port_fec_type_validate(
          dev_id, dev_port, speed, port_fec, &is_fec_valid) != BF_SUCCESS) {
    return BF_INVALID_ARG;
  }
  if (!is_fec_valid) {
    PM_ERROR(
        "New port speed is incompatible with dev : %d : front port : %d/%d : "
        "fec : %s",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        port_fec == BF_FEC_TYP_NONE
            ? "FEC_NONE"
            : port_fec == BF_FEC_TYP_REED_SOLOMON ? "FEC_RS" : "FEC_FC");
    return BF_INVALID_ARG;
  }

  sts = bf_pm_port_delete(dev_id, port_hdl);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to delete port for dev : %d : front port : %d/%d (%d) : %s "
        "(%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_add_with_lanes(dev_id, port_hdl, speed, n_lanes, port_fec);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to add port for dev : %d : front port : %d/%d (%d) : %s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  if (port_admin_state == PM_PORT_DISABLED) {
    return BF_SUCCESS;
  }

  // Enable the port after re-adding only if it was enabled before
  sts = bf_pm_port_enable(dev_id, port_hdl);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to enable port for dev : %d : front port : %d/%d (%d) : %s "
        "(%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_fec_get(bf_dev_id_t dev_id,
                               bf_pal_front_port_handle_t *port_hdl,
                               bf_fec_type_t *fec_type) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!fec_type) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) return BF_INVALID_ARG;

  *fec_type = port_info->fec_type;

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_fec_set(bf_dev_id_t dev_id,
                               bf_pal_front_port_handle_t *port_hdl,
                               bf_fec_type_t fec_type) {
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bool is_fec_valid;
  bf_pm_port_admin_state_e port_admin_state;
  bf_port_speed_t port_speed;
  uint32_t n_lanes = 0;
  bf_dev_id_t dev_id_of_port = 0;
  bool is_port_internal;
  bf_pm_port_dir_e port_direction;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d:%d(fec)",
           dev_id,
           port_hdl->conn_id,
           port_hdl->chnl_id,
           fec_type);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  dev_port = port_info->dev_port;
  if (!port_info->is_added) return BF_INVALID_ARG;

  if (port_info->fec_type == fec_type) return BF_SUCCESS;

  if (bf_port_fec_type_validate(
          dev_id, dev_port, port_info->speed, fec_type, &is_fec_valid) !=
      BF_SUCCESS) {
    return BF_INVALID_ARG;
  }
  if (!is_fec_valid) return BF_INVALID_ARG;

  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    sts = bf_port_fec_mode_set(dev_id, dev_port, fec_type);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to set FEC for port for dev : %d : front port : %d/%d (%d) : "
          "%s "
          "(%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }
    port_info->fec_type = fec_type;
    return BF_SUCCESS;
  }

  // Cache the speed and the admin state before we delete the port
  port_speed = port_info->speed;
  port_admin_state = port_info->admin_state;
  n_lanes = port_info->n_lanes;
  port_direction = port_info->port_dir;

  sts = bf_pm_port_delete(dev_id, port_hdl);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to delete port for dev : %d : front port : %d/%d (%d) : %s "
        "(%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_add_with_lanes(
      dev_id, port_hdl, port_speed, n_lanes, fec_type);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to add port for dev : %d : front port : %d/%d (%d) : %s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  bf_pm_is_port_internal(dev_id, port_hdl, &is_port_internal);
  if (!is_port_internal) {
    bf_pm_port_direction_set(dev_id, port_hdl, port_direction);
  }

  if (port_admin_state == PM_PORT_DISABLED) {
    return BF_SUCCESS;
  }

  // Enable the port after re-adding only if it was enabled before
  sts = bf_pm_port_enable(dev_id, port_hdl);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to enable port for dev : %d : front port : %d/%d (%d) : %s "
        "(%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_pll_ovrclk_get(bf_dev_id_t dev_id,
                                      bf_pal_front_port_handle_t *port_hdl,
                                      float *pll_ovrclk) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;
  bf_dev_port_t dev_port;
  bf_status_t sts;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!pll_ovrclk) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
    return BF_INVALID_ARG;
  }

  if (!bf_lld_dev_is_tof1(dev_id)) {
    PM_DEBUG("PLL Overclocking not supported on dev:%d", dev_id);
    return BF_SUCCESS;
  }

  if (!port_info->is_added) {
    PM_ERROR("%s: Warning: : %d/%d not added",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id);
    return BF_INVALID_ARG;
  }

  dev_port = port_info->dev_port;
  // For multichannel Port-modes, fetch the parameter from the first lane.
  sts = bf_serdes_pll_ovrclk_get(dev_id, dev_port, 0, pll_ovrclk);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to get PLL Overclock for port for dev : %d : front port : "
        "%d/%d (%d) : %s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_pll_ovrclk_set(bf_dev_id_t dev_id,
                                      bf_pal_front_port_handle_t *port_hdl,
                                      float pll_ovrclk) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;
  bf_dev_port_t dev_port;
  int num_lanes = 1, log_lane;
  bf_status_t sts;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
    return BF_INVALID_ARG;
  }

  if (!bf_lld_dev_is_tof1(dev_id)) {
    PM_ERROR("PLL Overclocking not supported on dev:%d", dev_id);
    return BF_NOT_IMPLEMENTED;
  }

  if (!port_info->is_added) {
    PM_ERROR("%s: Warning: : %d/%d not added",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id);
    return BF_INVALID_ARG;
  }

  PM_TRACE("Set PLL overclock(%f) for Dev:%d Port:%d/%d",
           pll_ovrclk,
           dev_id,
           port_hdl->conn_id,
           port_hdl->chnl_id);

  dev_port = port_info->dev_port;
  num_lanes = port_info->n_lanes;

  for (log_lane = 0; log_lane < num_lanes; log_lane++) {
    sts = bf_serdes_pll_ovrclk_set(dev_id, dev_port, log_lane, pll_ovrclk);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to set PLL Overclock for port for dev : %d : front port : "
          "%d/%d (%d) : %s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_loopback_mode_from_dev_port_get(
    bf_dev_id_t dev_id, bf_dev_port_t dev_port, bf_loopback_mode_e *mode) {
  bf_pm_port_info_t *port_info;

  port_info = pm_port_info_get(dev_id, dev_port);
  if (port_info == NULL) {
    PM_ERROR("Unable to get port info for dev : %d : dev-port : %d",
             dev_id,
             dev_port);
    return BF_INVALID_ARG;
  }

  *mode = port_info->lpbk_mode;
  return BF_SUCCESS;
}

bf_status_t bf_pm_port_loopback_mode_get(bf_dev_id_t dev_id,
                                         bf_pal_front_port_handle_t *port_hdl,
                                         bf_loopback_mode_e *mode) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) {
    return BF_INVALID_ARG;
  }

  if (!port_info->is_added) return BF_INVALID_ARG;

  *mode = port_info->lpbk_mode;

  return BF_SUCCESS;
}

static bf_status_t pm_port_set_mac_lb_mode(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           bf_pm_port_info_t *port_info) {
  bf_status_t sts;

  if (!port_info) return BF_INVALID_ARG;

  // Put MAC in reset state
  // We expect port to be in enabled state befor this step
  pm_port_exclusive_access_start(dev_id, port_info);
  sts = pm_port_enable(dev_id, port_info->dev_port);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to enable port for MAC loopback for dev : %d : front port : "
        "%d/%d (%d) : MAC : %d/%d : %s (%d)",
        dev_id,
        port_info->pltfm_port_info.port_hdl.conn_id,
        port_info->pltfm_port_info.port_hdl.chnl_id,
        port_info->dev_port,
        port_info->pltfm_port_info.log_mac_id,
        port_info->pltfm_port_info.log_mac_lane,
        bf_err_str(sts),
        sts);
    goto end;
  }
  sts = pm_port_disable(dev_id, port_info->dev_port);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to disable port for MAC loopback for dev : %d : front port : "
        "%d/%d (%d) : MAC : %d/%d : %s (%d)",
        dev_id,
        port_info->pltfm_port_info.port_hdl.conn_id,
        port_info->pltfm_port_info.port_hdl.chnl_id,
        port_info->dev_port,
        port_info->pltfm_port_info.log_mac_id,
        port_info->pltfm_port_info.log_mac_lane,
        bf_err_str(sts),
        sts);
    goto end;
  }

  sts = bf_port_loopback_mode_set(dev_id, dev_port, BF_LPBK_NONE);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to clear MAC loopback for dev : %d : front port : "
        "%d/%d (%d) : MAC : %d/%d : %s (%d)",
        dev_id,
        port_info->pltfm_port_info.port_hdl.conn_id,
        port_info->pltfm_port_info.port_hdl.chnl_id,
        port_info->dev_port,
        port_info->pltfm_port_info.log_mac_id,
        port_info->pltfm_port_info.log_mac_lane,
        bf_err_str(sts),
        sts);
    goto end;
  }

  sts = bf_port_loopback_mode_set(dev_id, dev_port, BF_LPBK_MAC_NEAR);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to set MAC loopback for dev : %d : front port : "
        "%d/%d (%d) : MAC : %d/%d : %s (%d)",
        dev_id,
        port_info->pltfm_port_info.port_hdl.conn_id,
        port_info->pltfm_port_info.port_hdl.chnl_id,
        port_info->dev_port,
        port_info->pltfm_port_info.log_mac_id,
        port_info->pltfm_port_info.log_mac_lane,
        bf_err_str(sts),
        sts);
    goto end;
  }
  port_info->lpbk_mode = BF_LPBK_MAC_NEAR;

  PM_DEBUG(
      "MAC loopback success for dev : %d : front port : "
      "%d/%d (%d) : MAC : %d/%d",
      dev_id,
      port_info->pltfm_port_info.port_hdl.conn_id,
      port_info->pltfm_port_info.port_hdl.chnl_id,
      port_info->dev_port,
      port_info->pltfm_port_info.log_mac_id,
      port_info->pltfm_port_info.log_mac_lane);

end:
  pm_port_exclusive_access_end(dev_id, port_info);
  return sts;
}

static bf_status_t bf_pm_validate_loopback_mode(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl,
    bf_pm_port_info_t *port_info,
    bf_loopback_mode_e mode) {
  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;

  bool is_sw_model = false;
  bf_status_t sts = bf_drv_device_type_get(dev_id, &is_sw_model);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to get device type for dev : %d : front port : %d/%d err %s "
        "(%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        bf_err_str(sts),
        sts);
    return BF_INVALID_ARG;
  }

  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));
  switch (dev_family) {
    case BF_DEV_FAMILY_TOFINO3:
      if (is_sw_model) {
        if (mode == BF_LPBK_NONE) return BF_SUCCESS;
        PM_ERROR("TOF3-Model Unsupported loopback mode %s on dev:%d:%d/%d",
                 get_loopback_mode_str(mode),
                 dev_id,
                 port_hdl->conn_id,
                 port_hdl->chnl_id);
        return BF_NOT_IMPLEMENTED;
      }
      switch (mode) {
        case BF_LPBK_NONE:
        case BF_LPBK_PCS_NEAR:
        case BF_LPBK_PIPE:
          return BF_SUCCESS;
        default:

          PM_ERROR("TOF3 Unsupported loopback mode %s on dev:%d:%d/%d",
                   get_loopback_mode_str(mode),
                   dev_id,
                   port_hdl->conn_id,
                   port_hdl->chnl_id);

          return BF_NOT_IMPLEMENTED;
      }
      break;
    case BF_DEV_FAMILY_TOFINO2:
      switch (mode) {
        case BF_LPBK_NONE:
        case BF_LPBK_MAC_NEAR:
        case BF_LPBK_PIPE:
          return BF_SUCCESS;
        case BF_LPBK_PCS_NEAR:
          /* PCS_NEAR in 10G Mode is not supported on Tofino 2 due a HW issue.
           * See Tofino 2 Errata document.
           */
          if (!port_info) return BF_INVALID_ARG;
          if (port_info->speed == BF_SPEED_10G &&
              !pm_port_is_cpu_port(dev_id, port_info->dev_port)) {
            PM_ERROR(
                "Unsupported loopback mode %s on ports configured with 10G"
                " Ethernet modes on dev:%d:%d/%d",
                get_loopback_mode_str(mode),
                dev_id,
                port_hdl->conn_id,
                port_hdl->chnl_id);
            return BF_NOT_IMPLEMENTED;
          }
          return BF_SUCCESS;
        default:

          PM_ERROR("Unsupported loopback mode %s on dev:%d:%d/%d",
                   get_loopback_mode_str(mode),
                   dev_id,
                   port_hdl->conn_id,
                   port_hdl->chnl_id);

          return BF_NOT_IMPLEMENTED;
      }

      break;
    case BF_DEV_FAMILY_TOFINO:
      switch (mode) {
        case BF_LPBK_NONE:
        case BF_LPBK_MAC_NEAR:
        case BF_LPBK_MAC_FAR:
        case BF_LPBK_PCS_NEAR:
          return BF_SUCCESS;
        case BF_LPBK_PIPE:
        case BF_LPBK_SERDES_NEAR:
        case BF_LPBK_SERDES_FAR:
          PM_ERROR("Unsupported loopback mode %s on dev:%d:%d/%d",
                   get_loopback_mode_str(mode),
                   dev_id,
                   port_hdl->conn_id,
                   port_hdl->chnl_id);

          return BF_NOT_IMPLEMENTED;
        default:
          PM_ERROR("Invalid loopback mode on dev:%d:%d/%d",
                   dev_id,
                   port_hdl->conn_id,
                   port_hdl->chnl_id);
          return BF_NOT_IMPLEMENTED;
      }
    default:
      return BF_INVALID_ARG;
  }

  return BF_SUCCESS;
}

/**
 * Set loopback mode.
 * Internal function: safety checks must be performed by caller, also any
 * required lock must be taken by the caller.
 */
static bf_status_t bf_pm_port_loopback_mode_set_internal(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl,
    bf_pm_port_info_t *port_info,
    bf_dev_port_t dev_port,
    bf_loopback_mode_e mode) {
  bf_status_t sts = BF_SUCCESS;
  int log_lane, num_lanes;
  bool skip_serdes_fsm_flag = false;

  port_info->lpbk_mode = mode;

  if (port_info->admin_state == PM_PORT_ENABLED) {
    sts = pm_port_disable(dev_id, dev_port);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Error in SDE port disable for dev : %d : front port : %d/%d (%d) : "
          "%s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }
  }

  /* Introduce a delay between flapping a port */
  bf_sys_usleep(200000);

  if ((mode != BF_LPBK_NONE) && (mode != BF_LPBK_MAC_FAR)) {
    // Stop auto negotiating
    if ((sts = pm_port_set_an(dev_id, dev_port, false)) != BF_SUCCESS) {
      PM_ERROR(
          "Unable to stop auto-negotiation for dev : %d : front port : %d/%d "
          "(%d) : %s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
    }
  } else {
    // Apply AN if desired
    pm_port_an_apply(dev_id, port_hdl);
    // Apply termination settings
    pm_port_term_mode_apply(dev_id, port_hdl);
  }

  skip_serdes_fsm_flag = true;
  if ((mode == BF_LPBK_NONE) || (mode == BF_LPBK_MAC_FAR)) {
    // If the mode is NONE, clear the skip serdes FSM flag only if the port is
    // an external port.
    // For internal ports, we should never enable the serdes
    if (!port_info->is_internal) {
      skip_serdes_fsm_flag = false;
    }
  }

  // Mark this port so that the non-serdes FSM is used to bring it up
  sts = pm_port_skip_serdes_fsm_set(dev_port, skip_serdes_fsm_flag);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to skip serdes fsm for dev : %d : front port : %d/%d (%d) : %s "
        "(%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  num_lanes = port_info->n_lanes;
  if (!port_info->is_internal) {
    if ((mode == BF_LPBK_NONE) || (mode == BF_LPBK_MAC_FAR)) {
      // Configure the serdes for this port
      sts = pm_serdes_config_set(dev_id, port_hdl);
      if (sts != BF_SUCCESS) {
        PM_ERROR(
            "Unable to configure the serdes for dev : %d : front port : %d/%d "
            "(%d) : %s (%d)",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            dev_port,
            bf_err_str(sts),
            sts);
        return sts;
      }
    } else {
      for (log_lane = 0; log_lane < num_lanes; log_lane++) {
        // Configure the serdes to default values so that the port comes up in
        // loopback
        /* reset the serdes lanes first to make lpbk phy work */
        sts = bf_serdes_reset(
            dev_id, dev_port, log_lane, BF_SDS_RESET_MICROPROCESSOR);
        if (sts != BF_SUCCESS) {
          PM_ERROR(
              "Unable to reset serdes microprocessor to default values for dev "
              ": %d : front "
              "port "
              ": %d/%d (%d) : %s (%d)",
              dev_id,
              port_hdl->conn_id,
              port_hdl->chnl_id,
              dev_port,
              bf_err_str(sts),
              sts);
          return sts;
        }
        sts = bf_serdes_lane_init_run(
            dev_id,
            dev_port,
            log_lane,
            pm_port_speed_to_sds_line_rate_map(port_info->speed),
            true,
            true,
            false,
            false);
        if (sts != BF_SUCCESS) {
          PM_ERROR(
              "Unable to init serdes to default values for dev : %d : front "
              "port "
              ": %d/%d (%d) : %s (%d)",
              dev_id,
              port_hdl->conn_id,
              port_hdl->chnl_id,
              dev_port,
              bf_err_str(sts),
              sts);
          return sts;
        }
      }
    }
  }

  // Since mac-loopback enable requires special sequence
  if (BF_LPBK_MAC_NEAR == mode) {
    sts = pm_port_set_mac_lb_mode(dev_id, dev_port, port_info);
  } else {
    /* Set the loopback mode for the port */
    sts = bf_port_loopback_mode_set(dev_id, dev_port, mode);
  }

  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Error in SDE loopback mode %d set for dev : %d : front port : %d/%d "
        "(%d) "
        ": %s (%d)",
        mode,
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  /* Reset the force remote fault bit for the port */
  sts = bf_port_force_remote_fault_set(dev_id, dev_port, false);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to undo forcing remote fault for dev : %d : front port : %d/%d "
        "(%d) : %s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  /* Reset the force local fault bit for the port */
  sts = bf_port_force_local_fault_set(dev_id, dev_port, false);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to undo forcing local fault for dev : %d : front port : %d/%d "
        "(%d) : %s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  /* Re-enable the port */
  if (port_info->admin_state == PM_PORT_ENABLED) {
    // test
    // if clearing loopback and port is not ready for bring-up, skip enable
    if ((mode == BF_LPBK_NONE) && (!port_info->is_ready_for_bringup)) {
    } else {
      sts = pm_port_enable(dev_id, dev_port);
      if (sts != BF_SUCCESS) {
        PM_ERROR(
            "Error in SDE port enable for dev : %d : front port : %d/%d (%d) : "
            "%s (%d)",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            dev_port,
            bf_err_str(sts),
            sts);
        return sts;
      }
    }
  }
  // Note: Mac-loopback remaps the serdes-lanes to 0.
  // We need to init the tx for that serdes.
  if (port_info->is_internal) {
    if (mode != BF_LPBK_NONE) {
      /* configure logical lane 0 */
      sts = bf_serdes_lane_init_run(dev_id,
                                    dev_port,
                                    0,
                                    BF_SDS_LINE_RATE_25G,
                                    false,
                                    true,  // tx only
                                    false,
                                    false);
      if (sts != BF_SUCCESS) {
        PM_ERROR(
            "Error in serdes init for logical lane 0 for dev : %d : front port "
            ": %d/%d "
            "(%d) "
            ": %s (%d)",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            dev_port,
            bf_err_str(sts),
            sts);
        return sts;
      }
    }
  } else if (mode == BF_LPBK_MAC_NEAR) {
    num_lanes = port_info->n_lanes;
    for (log_lane = 0; log_lane < num_lanes; log_lane++) {
      sts = bf_serdes_lane_init_run(
          dev_id,
          dev_port,
          log_lane,
          pm_port_speed_to_sds_line_rate_map(port_info->speed),
          false,
          true,  // tx-only
          false,
          false);
      if (sts != BF_SUCCESS) {
        PM_ERROR(
            "Unable to init serdes to default values for dev : %d : front "
            "port "
            ": %d/%d (%d) : %s (%d)",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            dev_port,
            bf_err_str(sts),
            sts);
        return sts;
      }
    }
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_loopback_mode_set(bf_dev_id_t dev_id,
                                         bf_pal_front_port_handle_t *port_hdl,
                                         bf_loopback_mode_e mode) {
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d:%d(loopback_mode)",
           dev_id,
           port_hdl->conn_id,
           port_hdl->chnl_id,
           mode);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  dev_port = port_info->dev_port;

  if (!port_info->is_added) return BF_INVALID_ARG;

  if (mode != BF_LPBK_NONE && port_info->port_dir != PM_PORT_DIR_DEFAULT) {
    PM_ERROR(
        "%s: Error: : %d/%d %d:%3d:- Cannot enable loopback on a "
        "rx-only/tx-only port",
        __func__,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_id,
        dev_id_of_port);
    return BF_INVALID_ARG;
  }

  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    // Gets applied during port-enable
    sts = bf_pm_validate_loopback_mode(dev_id, port_hdl, port_info, mode);
    if (sts != BF_SUCCESS) {
      port_info->lpbk_mode = BF_LPBK_NONE;
    } else {
      port_info->lpbk_mode = mode;
    }
    if (pm_dev_is_locked(dev_id)) {
      return bf_port_loopback_mode_set(dev_id, dev_port, mode);
    }
    return sts;
  }

  sts = bf_pm_validate_loopback_mode(dev_id, port_hdl, port_info, mode);
  if (sts != BF_SUCCESS) return sts;

  if ((port_info->speed != BF_SPEED_100G) && bf_lld_dev_is_tof1(dev_id)) {
    PM_TRACE(
        "WARNING: %d : %d/%d All channels/ports in the quad must have same "
        "speed, loopback modes and enabled or disabled",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id);
  }

  if (port_info->lpbk_mode == mode) return BF_SUCCESS;

  pm_port_exclusive_access_start(dev_id, port_info);

  if (port_info->lpbk_mode != BF_LPBK_NONE && mode != BF_LPBK_NONE) {
    sts = bf_pm_port_loopback_mode_set_internal(
        dev_id, port_hdl, port_info, dev_port, BF_LPBK_NONE);
    if (sts != BF_SUCCESS) goto end;
  }
  sts = bf_pm_port_loopback_mode_set_internal(
      dev_id, port_hdl, port_info, dev_port, mode);
  if (sts != BF_SUCCESS) goto end;

  pm_port_exclusive_access_end(dev_id, port_info);
  return BF_SUCCESS;

end:
  pm_port_exclusive_access_end(dev_id, port_info);
  PM_ERROR(
      "Unable to setup a port in loopback for dev : %d : front port : %d/%d : "
      "%s (%d)",
      dev_id,
      port_hdl->conn_id,
      port_hdl->chnl_id,
      bf_err_str(sts),
      sts);
  return sts;
}

bf_status_t bf_pm_port_stats_poll_start(bf_dev_id_t dev_id,
                                        uint32_t poll_intv_ms) {
  bf_sys_timer_status_t rc;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;

  if (port_stats_timer_started[dev_id] &&
      (poll_intv_ms == port_stats_timer_intvl[dev_id])) {
    // Indicates that the timer has already been started
    return BF_SUCCESS;
  }
  rc = bf_pm_port_stats_poll_period_update(dev_id, poll_intv_ms);
  if (rc) {
    PM_ERROR("Unable to update port stats timer : %d, dev %d, poll_intv_ms %d",
             rc,
             dev_id,
             poll_intv_ms);
  }

  return rc;
}

bf_status_t bf_pm_port_stats_poll_stop(bf_dev_id_t dev_id) {
  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;

  pm_port_stats_poll_stop(dev_id);
  return BF_SUCCESS;
}

bf_status_t bf_pm_port_this_stat_get(bf_dev_id_t dev_id,
                                     bf_pal_front_port_handle_t *port_hdl,
                                     bf_rmon_counter_t ctr_type,
                                     uint64_t *stat_val) {
  bf_pm_port_info_t *port_info;
  uint64_t *curr_stats_ptr, *old_stats_ptr;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!stat_val) return BF_INVALID_ARG;
  if (ctr_type >= BF_NUM_RMON_COUNTERS) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) return BF_INVALID_ARG;

  pm_port_exclusive_access_start(dev_id, port_info);
  curr_stats_ptr = (uint64_t *)&port_info->curr_stats;
  old_stats_ptr = (uint64_t *)&port_info->old_stats;

  *stat_val = ((curr_stats_ptr[ctr_type] >= old_stats_ptr[ctr_type])
                   ? (curr_stats_ptr[ctr_type] - old_stats_ptr[ctr_type])
                   : ((uint64_t)(-1) - old_stats_ptr[ctr_type] +
                      curr_stats_ptr[ctr_type]));
  pm_port_exclusive_access_end(dev_id, port_info);

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_this_stat_get_with_timestamp(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl,
    bf_rmon_counter_t ctr_type,
    uint64_t *stat_val,
    int64_t *timestamp_s,
    int64_t *timestamp_ns) {
  bf_pm_port_info_t *port_info;
  uint64_t *curr_stats_ptr, *old_stats_ptr;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!stat_val) return BF_INVALID_ARG;
  if (ctr_type >= BF_NUM_RMON_COUNTERS) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) return BF_INVALID_ARG;

  pm_port_exclusive_access_start(dev_id, port_info);
  curr_stats_ptr = (uint64_t *)&port_info->curr_stats;
  old_stats_ptr = (uint64_t *)&port_info->old_stats;

  *stat_val = ((curr_stats_ptr[ctr_type] >= old_stats_ptr[ctr_type])
                   ? (curr_stats_ptr[ctr_type] - old_stats_ptr[ctr_type])
                   : ((uint64_t)(-1) - old_stats_ptr[ctr_type] +
                      curr_stats_ptr[ctr_type]));
  *timestamp_s = port_info->stats_timestamp_sec;
  *timestamp_ns = port_info->stats_timestamp_nsec;
  pm_port_exclusive_access_end(dev_id, port_info);

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_all_stats_get(bf_dev_id_t dev_id,
                                     bf_pal_front_port_handle_t *port_hdl,
                                     uint64_t stats[BF_NUM_RMON_COUNTERS]) {
  bf_pm_port_info_t *port_info;
  uint64_t *curr_stats_ptr, *old_stats_ptr;
  uint32_t stat_id;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!stats) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) return BF_INVALID_ARG;

  pm_port_exclusive_access_start(dev_id, port_info);
  curr_stats_ptr = (uint64_t *)&port_info->curr_stats;
  old_stats_ptr = (uint64_t *)&port_info->old_stats;
  for (stat_id = 0; stat_id < BF_NUM_RMON_COUNTERS; stat_id++) {
    stats[stat_id] = ((curr_stats_ptr[stat_id] >= old_stats_ptr[stat_id])
                          ? (curr_stats_ptr[stat_id] - old_stats_ptr[stat_id])
                          : ((uint64_t)(-1) - old_stats_ptr[stat_id] +
                             curr_stats_ptr[stat_id]));
  }
  pm_port_exclusive_access_end(dev_id, port_info);

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_this_stat_clear(bf_dev_id_t dev_id,
                                       bf_pal_front_port_handle_t *port_hdl,
                                       bf_rmon_counter_t ctr_type) {
  bf_pm_port_info_t *port_info;
  uint64_t *curr_stats_ptr, *old_stats_ptr;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (ctr_type >= BF_NUM_RMON_COUNTERS) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) return BF_INVALID_ARG;

  pm_port_exclusive_access_start(dev_id, port_info);
  curr_stats_ptr = (uint64_t *)&port_info->curr_stats;
  old_stats_ptr = (uint64_t *)&port_info->old_stats;

  old_stats_ptr[ctr_type] = curr_stats_ptr[ctr_type];
  pm_port_exclusive_access_end(dev_id, port_info);

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_all_stats_clear(bf_dev_id_t dev_id,
                                       bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;
  bf_status_t sts = BF_SUCCESS;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) return BF_INVALID_ARG;

  pm_port_exclusive_access_start(dev_id, port_info);
  // Cache a copy of the current stats
  sts = bf_port_hw_mac_stats_clear(dev_id, port_info->dev_port);
  if (sts != BF_SUCCESS) {
    PM_ERROR("%s: Stats historical update failed on %d/%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id);
    pm_port_exclusive_access_end(dev_id, port_info);
    return BF_INVALID_ARG;
  }
  memcpy((char *)&port_info->old_stats,
         (char *)&port_info->curr_stats,
         sizeof(port_info->old_stats));
  pm_port_exclusive_access_end(dev_id, port_info);

  return BF_SUCCESS;
}

void bf_pm_port_all_stats_update_async_cb(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port,
                                          bf_rmon_counter_array_t *ctrs,
                                          uint64_t dma_timestamp_nsec,
                                          void *userdata) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  uint64_t dma_elapsed_nsec = 0;
  bf_pm_port_info_t *port_info = (bf_pm_port_info_t *)userdata;
  if (!port_info) {
    PM_ERROR(
        "Stats update aysnc callback received for a non existing port for dev "
        ": %d",
        dev_id);
    return;
  }

  if (dev_port != port_info->dev_port) {
    PM_ERROR(
        "Dev port embedded in the user data (%d) does not match with the dev "
        "port in the params (%d) for dev : %d",
        port_info->dev_port,
        dev_port,
        dev_id);
    return;
  }

  pm_port_exclusive_access_start(dev_id, port_info);
  if ((port_info->curr_stats.format.ctr_ids.FramesReceivedAll !=
       ctrs->format.ctr_ids.FramesReceivedAll) ||
      (port_info->curr_stats.format.ctr_ids.FramesTransmittedAll !=
       ctrs->format.ctr_ids.FramesTransmittedAll)) {
    port_info->there_is_traffic = 1;
  } else {
    port_info->there_is_traffic = 0;
  }

  /* Save the previous counters */
  port_info->prev_stats_timestamp_sec = port_info->stats_timestamp_sec;
  port_info->prev_stats_timestamp_nsec = port_info->stats_timestamp_nsec;
  port_info->rx_octets_good =
      port_info->curr_stats.format.ctr_ids.OctetsReceivedinGoodFrames;
  port_info->tx_octets_good =
      port_info->curr_stats.format.ctr_ids.OctetsTransmittedwithouterror;
  port_info->rx_frame_good =
      port_info->curr_stats.format.ctr_ids.FramesReceivedOK;
  port_info->tx_frame_good =
      port_info->curr_stats.format.ctr_ids.FramesTransmittedOK;

  // Update the counters in our data structure
  memcpy((char *)&port_info->curr_stats,
         (char *)ctrs,
         sizeof(port_info->curr_stats));
  if (bf_lld_dev_is_tof1(dev_id)) {
    port_info->stats_timestamp_sec = (uint64_t)ts.tv_sec;
    port_info->stats_timestamp_nsec = (uint64_t)ts.tv_nsec;
  } else {
    /* The first iteration of stats collection for a port
       that got added by user, needs initialisation of timestamp
       to system time. Later iteration will compare between
       timstamp from dma and port_info->dma_timestamp_nsec. */
    if (port_info->stats_timestamp_sec == 0) {
      port_info->stats_timestamp_sec = (uint64_t)ts.tv_sec;
      port_info->stats_timestamp_nsec = (uint64_t)ts.tv_nsec;
      port_info->dma_timestamp_nsec = dma_timestamp_nsec;
    } else {
      if (dma_timestamp_nsec < port_info->dma_timestamp_nsec) {
        dma_elapsed_nsec =
            (1ull << 40) + dma_timestamp_nsec - port_info->dma_timestamp_nsec;
      } else {
        dma_elapsed_nsec = dma_timestamp_nsec - port_info->dma_timestamp_nsec;
      }
      port_info->dma_timestamp_nsec = dma_timestamp_nsec;
      port_info->stats_timestamp_nsec =
          (port_info->prev_stats_timestamp_nsec + dma_elapsed_nsec) %
          1000000000;
      port_info->stats_timestamp_sec =
          (port_info->prev_stats_timestamp_sec) +
          ((port_info->prev_stats_timestamp_nsec + dma_elapsed_nsec) /
           1000000000);
    }
  }
  port_info->async_stats_update_request_issued = false;
  bf_pm_rate_calc(port_info);
  pm_port_exclusive_access_end(dev_id, port_info);
  return;
}

bf_status_t bf_pm_port_all_pure_stats_get_with_timestamp(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl,
    uint64_t stats[BF_NUM_RMON_COUNTERS],
    int64_t *timestamp_s,
    int64_t *timestamp_ns) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  if ((dev_id < 0) || (dev_id >= BF_MAX_DEV_COUNT)) return BF_INVALID_ARG;
  if (!stats) return BF_INVALID_ARG;
  if (!timestamp_s || !timestamp_ns) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) return BF_INVALID_ARG;

  pm_port_exclusive_access_start(dev_id, port_info);
  memcpy((char *)stats,
         (char *)&port_info->curr_stats,
         sizeof(int64_t) * BF_NUM_RMON_COUNTERS);
  *timestamp_s = port_info->stats_timestamp_sec;
  *timestamp_ns = port_info->stats_timestamp_nsec;
  pm_port_exclusive_access_end(dev_id, port_info);

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_all_stats_update(bf_dev_id_t dev_id,
                                        bf_pal_front_port_handle_t *port_hdl) {
  bf_status_t sts = BF_SUCCESS;
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) {
    return BF_INVALID_ARG;
  }
  if (port_info->admin_state != PM_PORT_ENABLED) {
    return BF_INVALID_ARG;
  }

  pm_port_exclusive_access_start(dev_id, port_info);
  if (port_info->async_stats_update_request_issued) {
    pm_port_exclusive_access_end(dev_id, port_info);
    return BF_SUCCESS;
  }
  port_info->async_stats_update_request_issued = true;
  pm_port_exclusive_access_end(dev_id, port_info);
  sts = bf_port_mac_stats_hw_async_get(dev_id,
                                       port_info->dev_port,
                                       bf_pm_port_all_stats_update_async_cb,
                                       (void *)port_info);
  if (sts != BF_SUCCESS) {
    PM_DEBUG(
        "Unable to update the stats for dev : %d : front port : %d/%d (%d) : "
        "%s (%d) : port_added : %s (Maybe the port got deleted by the time the "
        "async stats "
        "update went through)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        port_info->dev_port,
        bf_err_str(sts),
        sts,
        port_info->is_added ? "true" : "false");
    pm_port_exclusive_access_start(dev_id, port_info);
    port_info->async_stats_update_request_issued = false;
    pm_port_exclusive_access_end(dev_id, port_info);
    return sts;
  }

  sts = bf_pm_collect_port_error_counters(dev_id, port_info->dev_port);
  if (sts != BF_SUCCESS) {
    PM_DEBUG(
        "Unable to update the port-error-counters for dev : %d : "
        "front port : %d/%d (%d) : "
        "%s (%d) : port_added : %s "
        "(Maybe the port got deleted by the time the "
        "async stats "
        "update went through)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        port_info->dev_port,
        bf_err_str(sts),
        sts,
        port_info->is_added ? "true" : "false");
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_all_stats_update_sync(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl) {
  bf_status_t sts;
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (!port_info) {
    PM_ERROR("%s: Warning: : %d/%d invalid port",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id);
    return BF_INVALID_ARG;
  }

  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) return BF_INVALID_ARG;

  pm_port_exclusive_access_start(dev_id, port_info);
  // Read the stats from the hardware
  sts = bf_port_mac_stats_hw_sync_get(
      dev_id, port_info->dev_port, &port_info->curr_stats);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to update the stats for dev : %d : front port : %d/%d (%d) : "
        "%s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        port_info->dev_port,
        bf_err_str(sts),
        sts);
    pm_port_exclusive_access_end(dev_id, port_info);
    return sts;
  }
  pm_port_exclusive_access_end(dev_id, port_info);

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_media_type_get(bf_dev_id_t dev_id,
                                      bf_pal_front_port_handle_t *port_hdl,
                                      bf_media_type_t *media_type) {
  bf_pm_port_info_t *port_info;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!media_type) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) return BF_INVALID_ARG;

  if (!pltfm_interface.pltfm_port_media_type_get) {
    *media_type = BF_MEDIA_TYPE_UNKNOWN;
    return BF_SUCCESS;
  }

  sts = pltfm_interface.pltfm_port_media_type_get(port_hdl, media_type);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to get the media type for dev : %d : front port : %d/%d (%d): "
        "%s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        port_info->dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

/*
 * Call into the platform code to get the presence-detect bit for the specified
 * port.
 */
bf_status_t bf_pm_port_presence_get(bf_dev_id_t dev_id,
                                      bf_pal_front_port_handle_t *port_hdl,
                                      bool *presence) {
  bf_pm_port_info_t *port_info;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!presence) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s Warning: : %d/%d found on dev %d : exp dev%d", __func__,
             port_hdl->conn_id, port_hdl->chnl_id,
             dev_id_of_port, dev_id);
  }

  if (!pltfm_interface.pltfm_port_presence_get) {
	  *presence = true;
	  return BF_SUCCESS;
  }

  sts = pltfm_interface.pltfm_port_presence_get(port_hdl, presence);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to get the presence bit for front port : %d/%d (%d): %s (%d)",
        port_hdl->conn_id, port_hdl->chnl_id, port_info->dev_port,
        bf_err_str(sts), sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_num_max_ports_get(bf_dev_id_t dev_id, uint32_t *num_ports) {
  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!num_ports) return BF_INVALID_ARG;

  *num_ports = pm_port_info_db_size_get(dev_id);

  return BF_SUCCESS;
}

bf_status_t bf_pm_num_front_ports_get(bf_dev_id_t dev_id, uint32_t *num_ports) {
  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!num_ports) return BF_INVALID_ARG;

  *num_ports = pm_num_of_front_ports_get(dev_id);

  return BF_SUCCESS;
}

bf_status_t bf_pm_num_internal_ports_get(bf_dev_id_t dev_id,
                                         uint32_t *num_ports) {
  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!num_ports) return BF_INVALID_ARG;

  *num_ports = pm_num_of_internal_ports_get(dev_id);

  return BF_SUCCESS;
}

bf_status_t bf_pm_front_port_index_to_dev_port_get(bf_dev_id_t dev_id,
                                                   uint32_t fp_idx,
                                                   bf_dev_port_t *dev_port) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr = &port_hdl;
  bf_pal_front_port_handle_t *next_port_hdl_ptr = &next_port_hdl;
  uint32_t iter_fp_index = 1;
  bf_dev_id_t dev_id_of_port = 0;
  uint32_t idx = fp_idx;
  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!dev_port) return BF_INVALID_ARG;

  sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    goto end;
  }
  if (!port_hdl_ptr) {
    bf_sys_dbgchk(0);
    sts = BF_INVALID_ARG;
    goto end;
  }

  if (dev_family == BF_DEV_FAMILY_TOFINO3) {
    fp_idx = fp_idx / 2;
  }

  if (!fp_idx) {
    // Indicates this is the first port in the system, hence return
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id_of_port, dev_port);
    if (sts != BF_SUCCESS) {
      goto end;
    }
    return BF_SUCCESS;
  }

  // Iterate through all the ports
  for (iter_fp_index = 1; iter_fp_index <= fp_idx; iter_fp_index++) {
    sts = bf_pm_port_front_panel_port_get_next(
        dev_id, port_hdl_ptr, next_port_hdl_ptr);
    if (sts != BF_SUCCESS) {
      goto end;
    }
    if (!next_port_hdl_ptr) {
      bf_sys_dbgchk(0);
      sts = BF_OBJECT_NOT_FOUND;
      goto end;
    }

    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        next_port_hdl_ptr, &dev_id_of_port, dev_port);
    if (sts != BF_SUCCESS) {
      goto end;
    }

    port_hdl_ptr = next_port_hdl_ptr;
  }

  return BF_SUCCESS;

end:
  fp_idx = idx;
  PM_ERROR(
      "Unable to get the dev port for dev : %d : front port idx : %d : %s (%d)",
      dev_id,
      fp_idx,
      bf_err_str(sts),
      sts);
  return sts;
}

bf_status_t bf_pm_recirc_port_range_get(bf_dev_id_t dev_id,
                                        uint32_t *start_recirc_port,
                                        uint32_t *end_recirc_port) {
  bf_status_t sts;
  uint32_t num_fp_ports, num_pipes;
  uint8_t port_group = lld_get_chnls_per_mac(dev_id);

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!start_recirc_port || !end_recirc_port) return BF_INVALID_ARG;

  sts = bf_pm_num_max_ports_get(dev_id, &num_fp_ports);
  if (sts != BF_SUCCESS) {
    PM_ERROR("Unable to get the recirc port range for dev : %d : %s (%d)",
             dev_id,
             bf_err_str(sts),
             sts);
    return sts;
  }

  /* The last 8 ports (64-71) in all the pipes support recirculation. However,
     we enable/disbale recirc on a port group as a whole and since port group
     16 has a CPU port we only expose the ports in port group 17*/
  sts = lld_sku_get_num_active_pipes(dev_id, &num_pipes);
  if (sts != BF_SUCCESS) {
    PM_ERROR("Unable to get the active pipes for dev : %d : %s (%d)",
             dev_id,
             bf_err_str(sts),
             sts);
    return sts;
  }

  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));
  if (dev_family == BF_DEV_FAMILY_TOFINO3) {
    // This is needed for iterating process, since in tofino3
    // two lanes can be used only as one channel
    num_fp_ports *= 2;
    port_group *= 2;
  }

  *start_recirc_port = num_fp_ports;
  *end_recirc_port = (*start_recirc_port) + (num_pipes * port_group) - 1;

  return BF_SUCCESS;
}

bf_status_t bf_pm_recirc_port_to_dev_port_get(bf_dev_id_t dev_id,
                                              uint32_t recirc_port,
                                              bf_dev_port_t *dev_port) {
  bf_status_t sts;
  uint32_t start_recirc_port, end_recirc_port;
  uint32_t pipe, port;
  uint8_t port_group = lld_get_chnls_per_mac(dev_id);

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!dev_port) return BF_INVALID_ARG;

  sts =
      bf_pm_recirc_port_range_get(dev_id, &start_recirc_port, &end_recirc_port);
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
      /* Tof2 uses ports 0-7 in each pipe as recirc, we expose 0-7. */
      pipe = (recirc_port - start_recirc_port) / port_group;
      port = (recirc_port - start_recirc_port) % port_group;
      *dev_port = MAKE_DEV_PORT(pipe, port);
      break;
    case BF_DEV_FAMILY_TOFINO:
      /* Tof uses ports 64-71 in each pipe as recirc, we expose 68-71. */
      pipe = (recirc_port - start_recirc_port) / port_group;
      port = (lld_get_max_fp_port(dev_id) + 1 + port_group) +
             ((recirc_port - start_recirc_port) % port_group);
      *dev_port = MAKE_DEV_PORT(pipe, port);
      break;
    case BF_DEV_FAMILY_TOFINO3:
      /* please use bf_pm_recirc_devports_get */
      return BF_NOT_IMPLEMENTED;
      break;
    default:
      return BF_INVALID_ARG;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_serdes_tx_eq_pre_set(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl, int tx_pre) {
  bf_status_t sts;
  bf_pm_port_info_t *port_info;
  bf_dev_port_t dev_port;
  int num_lanes = 1, log_lane;
  int attn, post, pre;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d:%d(tx_pre)",
           dev_id,
           port_hdl->conn_id,
           port_hdl->chnl_id,
           tx_pre);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  dev_port = port_info->dev_port;

  if (port_info->is_added) {
    num_lanes = port_info->n_lanes;
  } else {
    num_lanes = 1;
  }

  for (log_lane = 0; log_lane < num_lanes; log_lane++) {
    sts = bf_serdes_tx_drv_attn_get_allow_unassigned(
        dev_id, dev_port, log_lane, &attn, &post, &pre);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to get the serdes tx attn params for for dev : %d : front "
          "port : %d/%d (%d) : %s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }

    sts = bf_serdes_tx_drv_attn_set_allow_unassigned(
        dev_id, dev_port, log_lane, attn, post, tx_pre);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to set the serdes tx attn params for for dev : %d : front "
          "port : %d/%d (%d) : %s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_serdes_tx_eq_pre_get(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl, int *tx_pre) {
  bf_status_t sts;
  bf_pm_port_info_t *port_info;
  bf_dev_port_t dev_port;
  int attn, post;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl || !tx_pre) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d", dev_id, port_hdl->conn_id, port_hdl->chnl_id);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  dev_port = port_info->dev_port;

  /* Tof2 supports PAM4 serdes which adds two extra Tx taps that are not
     defined for NRZ modes, PRE2 and POST2. */
  if (bf_pm_intf_is_device_family_tofino(dev_id)) {
    sts = bf_serdes_tx_drv_attn_get_allow_unassigned(
        dev_id, dev_port, 0, &attn, &post, tx_pre);
  } else {
    sts = bf_tof2_serdes_tx_taps_get(
        dev_id, dev_port, 0, NULL, tx_pre, NULL, NULL, NULL);
  }
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to get the serdes tx pre param for for dev : %d : front "
        "port : %d/%d (%d) : %s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }
  return BF_SUCCESS;
}

bf_status_t bf_pm_port_serdes_tx_eq_pre2_get(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl, int *tx_pre2) {
  bf_status_t sts;
  bf_pm_port_info_t *port_info;
  bf_dev_port_t dev_port;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl || !tx_pre2) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d", dev_id, port_hdl->conn_id, port_hdl->chnl_id);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  dev_port = port_info->dev_port;

  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    sts = bf_tof2_serdes_tx_taps_get(
        dev_id, dev_port, 0, tx_pre2, NULL, NULL, NULL, NULL);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to get the serdes tx post param for for dev : %d : front "
          "port : %d/%d (%d) : %s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }
  } else {
    // Not supported but return 0 to supress errors. Should be read-only.
    *tx_pre2 = 0;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_serdes_tx_eq_post_set(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl, int tx_post) {
  bf_status_t sts;
  bf_pm_port_info_t *port_info;
  bf_dev_port_t dev_port;
  int num_lanes = 1, log_lane;
  int attn, post, pre;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d:%d(tx_post)",
           dev_id,
           port_hdl->conn_id,
           port_hdl->chnl_id,
           tx_post);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  dev_port = port_info->dev_port;

  if (port_info->is_added) {
    num_lanes = port_info->n_lanes;
  } else {
    num_lanes = 1;
  }

  for (log_lane = 0; log_lane < num_lanes; log_lane++) {
    sts = bf_serdes_tx_drv_attn_get_allow_unassigned(
        dev_id, dev_port, log_lane, &attn, &post, &pre);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to get the serdes tx attn params for for dev : %d : front "
          "port : %d/%d (%d) : %s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }

    sts = bf_serdes_tx_drv_attn_set_allow_unassigned(
        dev_id, dev_port, log_lane, attn, tx_post, pre);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to set the serdes tx attn params for for dev : %d : front "
          "port : %d/%d (%d) : %s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_serdes_tx_eq_post_get(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl, int *tx_post) {
  bf_status_t sts;
  bf_pm_port_info_t *port_info;
  bf_dev_port_t dev_port;
  int attn, pre;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl || !tx_post) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d", dev_id, port_hdl->conn_id, port_hdl->chnl_id);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  dev_port = port_info->dev_port;

  /* Tof2 supports PAM4 serdes which adds two extra Tx taps that are not
     defined for NRZ modes, PRE2 and POST2. */
  if (bf_pm_intf_is_device_family_tofino(dev_id)) {
    sts = bf_serdes_tx_drv_attn_get_allow_unassigned(
        dev_id, dev_port, 0, &attn, tx_post, &pre);
  } else {
    sts = bf_tof2_serdes_tx_taps_get(
        dev_id, dev_port, 0, NULL, NULL, NULL, tx_post, NULL);
  }
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to get the serdes tx post param for for dev : %d : front "
        "port : %d/%d (%d) : %s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_serdes_tx_eq_post2_get(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl, int *tx_post2) {
  bf_status_t sts;
  bf_pm_port_info_t *port_info;
  bf_dev_port_t dev_port;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl || !tx_post2) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d", dev_id, port_hdl->conn_id, port_hdl->chnl_id);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  dev_port = port_info->dev_port;

  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    sts = bf_tof2_serdes_tx_taps_get(
        dev_id, dev_port, 0, NULL, NULL, NULL, NULL, tx_post2);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to get the serdes tx post param for for dev : %d : front "
          "port : %d/%d (%d) : %s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }
  } else {
    // Not supported but return 0 to supress errors. Should be read-only.
    *tx_post2 = 0;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_serdes_tx_eq_main_set(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl, int tx_attn) {
  bf_status_t sts;
  bf_pm_port_info_t *port_info;
  bf_dev_port_t dev_port;
  int num_lanes = 1, log_lane;
  int attn, post, pre;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d:%d(tx_attn)",
           dev_id,
           port_hdl->conn_id,
           port_hdl->chnl_id,
           tx_attn);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  dev_port = port_info->dev_port;

  if (port_info->is_added) {
    num_lanes = port_info->n_lanes;
  } else {
    num_lanes = 1;
  }

  for (log_lane = 0; log_lane < num_lanes; log_lane++) {
    sts = bf_serdes_tx_drv_attn_get_allow_unassigned(
        dev_id, dev_port, log_lane, &attn, &post, &pre);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to get the serdes tx attn params for for dev : %d : front "
          "port : %d/%d (%d) : %s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }

    sts = bf_serdes_tx_drv_attn_set_allow_unassigned(
        dev_id, dev_port, log_lane, tx_attn, post, pre);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to set the serdes tx attn params for for dev : %d : front "
          "port : %d/%d (%d) : %s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_serdes_tx_eq_main_get(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl, int *tx_attn) {
  bf_status_t sts;
  bf_pm_port_info_t *port_info;
  bf_dev_port_t dev_port;
  int post, pre;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl || !tx_attn) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d", dev_id, port_hdl->conn_id, port_hdl->chnl_id);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  dev_port = port_info->dev_port;

  if (bf_pm_intf_is_device_family_tofino(dev_id)) {
    sts = bf_serdes_tx_drv_attn_get_allow_unassigned(
        dev_id, dev_port, 0, tx_attn, &post, &pre);
  } else {
    sts = bf_tof2_serdes_tx_taps_get(
        dev_id, dev_port, 0, NULL, NULL, tx_attn, NULL, NULL);
  }
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to get the serdes tx attn param for for dev : %d : front "
        "port : %d/%d (%d) : %s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }
  return BF_SUCCESS;
}

bf_status_t bf_pm_port_fsm_init_in_down_state(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl) {
  int err;
  bf_status_t sts;
  bf_dev_port_t dev_port;
  bf_dev_id_t dev_id_of_port = 0;
  bf_pm_port_info_t *port_info = NULL;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d", dev_id, port_hdl->conn_id, port_hdl->chnl_id);

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to get the dev port for dev : %d : front port : %d/%d : %s "
        "(%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        bf_err_str(sts),
        sts);
    return sts;
  }

  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) return BF_INVALID_ARG;
  if (port_info->lpbk_mode == BF_LPBK_NONE) {
    if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
      // When ports are down, moving to IDLE state
      // will kick in the link bringup process.
      // This is undesirable for front-panel ports.
      // So, wait for port rdy or bringup indication.

      PM_TRACE(
          "Wait for RDY indication from platform for dev : %d : Port : %d "
          "front "
          "port : %d/%d ",
          dev_id,
          dev_port,
          port_hdl->conn_id,
          port_hdl->chnl_id);

      return BF_SUCCESS;
    } else {
      if (!port_info->is_ready_for_bringup) {
        PM_TRACE(
            "Tofino : Wait for RDY indication from platform for dev : %d : "
            "Port "
            ": %d "
            "front "
            "port : %d/%d ",
            dev_id,
            dev_port,
            port_hdl->conn_id,
            port_hdl->chnl_id);

        return BF_SUCCESS;
      }
    }
  }

  if (port_info->admin_state == PM_PORT_ENABLED) {
    sts = pm_port_enable(dev_id, dev_port);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to enable the dev port %d error:: %s "
          "(%d)",
          dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }
  }
  PM_TRACE(
      "Initializing FSM in IDLE state for device %d port %d enabled %d lpbck "
      "%d",
      dev_id,
      dev_port,
      port_info->admin_state,
      port_info->lpbk_mode);

  err = pm_port_fsm_init(dev_id, dev_port);
  if (err != 0) {
    PM_ERROR(
        "Unable to init fsm for dev : %d : front port : %d/%d (%d) : %s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return err;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_fsm_init_in_up_state(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl) {
  int err;
  bf_status_t sts;
  bf_dev_port_t dev_port;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d", dev_id, port_hdl->conn_id, port_hdl->chnl_id);

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to get the dev port for dev : %d : front port : %d/%d : %s "
        "(%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        bf_err_str(sts),
        sts);
    return sts;
  }

  err = pm_port_fsm_init_in_up_state(dev_id, dev_port);
  if (err != 0) {
    PM_ERROR(
        "Unable to init fsm (UP state) for dev : %d : front port : %d/%d (%d) "
        ": %s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return err;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_fsm_set_down_event_state(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl) {
  int err;
  bf_status_t sts;
  bf_dev_port_t dev_port;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;

  PM_TRACE("%d:%d/%d", dev_id, port_hdl->conn_id, port_hdl->chnl_id);

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to get the dev port for dev : %d : front port : %d/%d : %s "
        "(%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        bf_err_str(sts),
        sts);
    return sts;
  }

  PM_TRACE("Initializing FSM in WAIT_DWN_EVENT state for device %d port %d",
           dev_id,
           dev_port);

  err = pm_port_fsm_set_down_event_state(dev_id, dev_port);
  if (err != 0) {
    PM_ERROR(
        "Unable to init fsm for dev : %d : front port : %d/%d (%d) : %s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return err;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_tasklet_scheduler() {
  pm_tasklet_scheduler();

  return BF_SUCCESS;
}

bf_status_t bf_pm_interrupt_based_link_monitoring_set(bf_dev_id_t dev_id,
                                                      bool en) {
  PM_TRACE("%d:%d(en)", dev_id, en);
  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;

  pm_port_interrupt_based_link_monitoring_set(dev_id, en);
  return BF_SUCCESS;
}

bf_status_t bf_pm_interrupt_based_link_monitoring_get(bf_dev_id_t dev_id,
                                                      bool *en) {
  PM_TRACE("%d:%d(en)", dev_id, *en);
  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;

  return pm_port_interrupt_based_link_monitoring_get(dev_id, en);
}

bf_status_t bf_pm_internal_ports_init(bf_dev_id_t dev_id) {
  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;

  /* Do some special handling if the chip's SKU is 32Q */
  pm_internal_port_lpbk_set(dev_id);

  return BF_SUCCESS;
}

bf_status_t bf_pm_is_port_added(bf_dev_id_t dev_id,
                                bf_pal_front_port_handle_t *port_hdl,
                                bool *is_added) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  *is_added = port_info->is_added;

  return BF_SUCCESS;
}
bf_status_t bf_pm_is_port_internal(bf_dev_id_t dev_id,
                                   bf_pal_front_port_handle_t *port_hdl,
                                   bool *is_internal) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  *is_internal = port_info->is_internal;

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_serdes_cfg_set(bf_dev_id_t dev_id,
                                      bf_pal_front_port_handle_t *port_hdl) {
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_pal_mac_to_serdes_lane_map_t mts_lane_map;
  bf_pal_serdes_info_t serdes_info;
  bf_sds_lane_info_t sds_lane_info;
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d", dev_id, port_hdl->conn_id, port_hdl->chnl_id);

  if (!pltfm_interface.pltfm_mac_to_serdes_map_get ||
      !pltfm_interface.pltfm_serdes_info_get) {
    return BF_NOT_IMPLEMENTED;
  }

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  dev_port = port_info->dev_port;

  if (port_info->is_internal) {
    return BF_SUCCESS;
  }

  sts = pltfm_interface.pltfm_mac_to_serdes_map_get(port_hdl, &mts_lane_map);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to get the mac to serdes lane map for dev : %d : front "
        "port : %d/%d (%d) : %s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = pltfm_interface.pltfm_serdes_info_get(port_hdl, &serdes_info);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to get serdes lane info for dev : %d : front port : %d/%d (%d)"
        " : %s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sds_lane_info.tx_phy_lane_id = mts_lane_map.tx_lane;
  sds_lane_info.rx_phy_lane_id = mts_lane_map.rx_lane;
  sds_lane_info.tx_phy_pn_swap = serdes_info.tx_inv;
  sds_lane_info.rx_phy_pn_swap = serdes_info.rx_inv;
  sds_lane_info.tx_attn = serdes_info.tx_attn;
  sds_lane_info.tx_pre = serdes_info.tx_pre;
  sds_lane_info.tx_post = serdes_info.tx_post;

  sts = bf_serdes_lane_info_set(dev_id,
                                port_info->pltfm_port_info.log_mac_id,
                                sds_lane_info.tx_phy_lane_id,
                                sds_lane_info.rx_phy_lane_id,
                                &sds_lane_info);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to set the serdes lane info for dev : %d : front port : "
        "%d/%d (%d) : %s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_traffic_status_get(bf_dev_id_t dev_id,
                                          bf_pal_front_port_handle_t *port_hdl,
                                          bool *there_is_traffic) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (port_stats_timer_started[dev_id] != true) return BF_EAGAIN;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!there_is_traffic) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) return BF_INVALID_ARG;

  *there_is_traffic = port_info->there_is_traffic;

  return BF_SUCCESS;
}

bf_status_t bf_pm_init_rate(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  bf_pm_port_info_t *port_info;
  struct timespec ts;
  bf_status_t sts;
  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) {
    return BF_NOT_IMPLEMENTED;
  }
  if (!port_info->is_added) {
    return BF_SUCCESS;
  }
  // update counter cache, time stamp, init rate
  sts = bf_port_mac_stats_hw_sync_get(dev_id, dev_port, &port_info->curr_stats);
  if (sts != BF_SUCCESS) {
    return BF_NOT_IMPLEMENTED;
  }
  pm_port_exclusive_access_start(dev_id, port_info);
  clock_gettime(CLOCK_REALTIME, &ts);
  port_info->rate_timestamp_sec = ts.tv_sec;
  port_info->rate_timestamp_nsec = ts.tv_nsec;
  port_info->rx_octets_good =
      port_info->curr_stats.format.ctr_ids.OctetsReceivedinGoodFrames;
  port_info->tx_octets_good =
      port_info->curr_stats.format.ctr_ids.OctetsTransmittedwithouterror;
  port_info->rx_frame_good =
      port_info->curr_stats.format.ctr_ids.FramesReceivedOK;
  port_info->tx_frame_good =
      port_info->curr_stats.format.ctr_ids.FramesTransmittedOK;
  port_info->rx_rate = 0;
  port_info->tx_rate = 0;
  port_info->rx_pps = 0;
  port_info->tx_pps = 0;
  pm_port_exclusive_access_end(dev_id, port_info);
  return BF_SUCCESS;
}

bf_status_t bf_pm_port_stat_direct_get(bf_dev_id_t dev_id,
                                       bf_pal_front_port_handle_t *port_hdl,
                                       bf_rmon_counter_t *ctr_type_array,
                                       uint64_t *ctr_data,
                                       uint32_t num_of_ctr) {
  bf_pm_port_info_t *port_info;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!ctr_data) return BF_INVALID_ARG;
  if (!ctr_type_array) return BF_INVALID_ARG;
  if (num_of_ctr > BF_NUM_RMON_COUNTERS) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) return BF_INVALID_ARG;

  pm_port_exclusive_access_start(dev_id, port_info);

  sts = bf_port_mac_stats_hw_direct_get(
      dev_id, port_info->dev_port, ctr_type_array, ctr_data, num_of_ctr);

  pm_port_exclusive_access_end(dev_id, port_info);

  return sts;
}

bf_status_t bf_pm_port_media_type_set(bf_dev_id_t dev_id,
                                      bf_pal_front_port_handle_t *port_hdl,
                                      bf_media_type_t media_type) {
  bf_pm_port_info_t *port_info;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d:%d(media_type)",
           dev_id,
           port_hdl->conn_id,
           port_hdl->chnl_id,
           media_type);

  if ((media_type < BF_MEDIA_TYPE_COPPER) ||
      (media_type > BF_MEDIA_TYPE_UNKNOWN)) {
    return BF_INVALID_ARG;
  }

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) return BF_INVALID_ARG;

  if (media_type == BF_MEDIA_TYPE_OPTICAL) {
    sts = bf_port_is_optical_xcvr_set(dev_id, port_info->dev_port, true);
  } else {
    sts = bf_port_is_optical_xcvr_set(dev_id, port_info->dev_port, false);
  }

  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to set the media type for dev : %d : front port : %d/%d (%d): "
        "%s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        port_info->dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_serdes_param_set(bf_dev_id_t dev_id,
                                        bf_pal_front_port_handle_t *port_hdl,
                                        bf_pal_serdes_params_t *serdes_param) {
  bf_pm_port_info_t *port_info;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;
  bf_pal_serdes_tx_eq_params_t tx_eq;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!serdes_param) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d", dev_id, port_hdl->conn_id, port_hdl->chnl_id);

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) return BF_INVALID_ARG;

  if (bf_pm_intf_is_device_family_tofino(dev_id)) {
    sts =
        bf_pm_port_serdes_tx_eq_pre_set(dev_id, port_hdl, serdes_param->tx_pre);
    sts |= bf_pm_port_serdes_tx_eq_post_set(
        dev_id, port_hdl, serdes_param->tx_post);
    sts |= bf_pm_port_serdes_tx_eq_main_set(
        dev_id, port_hdl, serdes_param->tx_attn);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to set the Serdes tx param for dev : %d : front port : %d/%d "
          "(%d): "
          "%s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          port_info->dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }
  } else {
    sts = bf_pm_port_serdes_tx_eq_params_get(
        dev_id, port_hdl, port_info->n_lanes, &tx_eq);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to set the Serdes tx param for dev : %d : front port : %d/%d "
          "(%d): "
          "%s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          port_info->dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }
    uint32_t log_lane = 0;
    for (log_lane = 0; log_lane < port_info->n_lanes; log_lane++) {
      tx_eq.tx_eq.tof2[log_lane].tx_pre1 = serdes_param->tx_pre;
      tx_eq.tx_eq.tof2[log_lane].tx_main = serdes_param->tx_attn;
      tx_eq.tx_eq.tof2[log_lane].tx_post1 = serdes_param->tx_post;
    }
    sts = bf_pm_port_serdes_tx_eq_params_set(
        dev_id, port_hdl, port_info->n_lanes, &tx_eq);
    if (sts != BF_SUCCESS) {
      PM_ERROR(
          "Unable to set the Serdes tx param for dev : %d : front port : %d/%d "
          "(%d): "
          "%s (%d)",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          port_info->dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_str_to_dev_port_get(bf_dev_id_t dev_id,
                                           char *port_str,
                                           bf_dev_port_t *dev_port) {
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;
  bf_pal_front_port_handle_t port_hdl;

  if ((port_str == NULL) || (dev_port == NULL)) return BF_INVALID_ARG;
  sts = bf_pm_port_str_to_hdl_get(dev_id, port_str, &port_hdl);
  if (sts != BF_SUCCESS) {
    PM_ERROR("Unable to find the port_hdl related to %s", port_str);
    return BF_INVALID_ARG;
  }

  sts = pm_front_port_handle_to_dev_port(&port_hdl, &dev_id_of_port, dev_port);
  if (sts != BF_SUCCESS) {
    PM_ERROR("Unable to find the dev_port related to %s, on dev_id %d",
             port_str,
             dev_id);
    return BF_INVALID_ARG;
  }
  // check
  if (dev_id != dev_id_of_port) {
    PM_DEBUG("Warning: %s found on dev%d : exp dev%d",
             port_str,
             dev_id,
             dev_id_of_port);
  }
  return BF_SUCCESS;
}

bf_status_t bf_pm_dev_port_to_port_str_get(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           char *port_str) {
  bf_pm_port_info_t port_info;
  bf_status_t sts = pm_port_info_get_copy(dev_id, dev_port, &port_info);
  if (sts != 0) return sts;
  memcpy(port_str, port_info.pltfm_port_info.port_str, MAX_PORT_HDL_STRING_LEN);
  return BF_SUCCESS;
}

bool bf_pm_intf_is_device_family_tofino2(bf_dev_id_t dev_id) {
  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));
  if (dev_family == BF_DEV_FAMILY_TOFINO2) {
    return true;
  }

  return false;
}

bool bf_pm_intf_is_device_family_tofino3(bf_dev_id_t dev_id) {
  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));
  if (dev_family == BF_DEV_FAMILY_TOFINO3) {
    return true;
  }

  return false;
}

bf_status_t bf_pm_port_serdes_polarity_set(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl,
    uint32_t num_entries,
    bf_pal_serdes_polarity_t *serdes_pol) {
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;
  bool rx_inv, tx_inv;
  uint32_t log_lane;

  if ((!port_hdl) || (!serdes_pol)) {
    return BF_INVALID_ARG;
  }

  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  if (port_info->is_added) {
    // n_lanes exceeding the max supported by the device is taken care during
    // port-add. Hence not checked here.
    if (num_entries != port_info->n_lanes) {
      PM_ERROR("Port lanes invalid for dev : %d : front port : %d/%d (%d)",
               dev_id,
               port_hdl->conn_id,
               port_hdl->chnl_id,
               dev_port);
      return BF_INVALID_ARG;
    }
  } else {
    PM_ERROR("Port is not added dev : %d : front port : %d/%d (%d)",
             dev_id,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_port);
    return BF_NOT_IMPLEMENTED;
  }

  for (log_lane = 0; log_lane < num_entries; log_lane++) {
    rx_inv = serdes_pol[log_lane].rx_inv;
    tx_inv = serdes_pol[log_lane].tx_inv;

    PM_DEBUG("Dev : %d : Port : %d/%d (%d) lane:%d rx_inv:%d tx_inv:%d",
             dev_id,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_port,
             log_lane,
             rx_inv,
             tx_inv);

    bf_dev_family_t dev_family =
        bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));
    switch (dev_family) {
      case BF_DEV_FAMILY_TOFINO2:
        sts = bf_tof2_serdes_rx_polarity_set(
            dev_id, dev_port, log_lane, rx_inv, false);
        if (sts != BF_SUCCESS) {
          PM_ERROR(
              "Unable to set the serdes rx inv for dev : %d : front port : "
              "%d/%d "
              "(%d) : lane: %d %s (%d)",
              dev_id,
              port_hdl->conn_id,
              port_hdl->chnl_id,
              dev_port,
              log_lane,
              bf_err_str(sts),
              sts);
          return sts;
        }
        sts = bf_tof2_serdes_tx_polarity_set(
            dev_id, dev_port, log_lane, tx_inv, false);
        if (sts != BF_SUCCESS) {
          PM_ERROR(
              "Unable to set the serdes tx inv for dev : %d : front port : "
              "%d/%d "
              "(%d) : lane: %d %s (%d)",
              dev_id,
              port_hdl->conn_id,
              port_hdl->chnl_id,
              dev_port,
              log_lane,
              bf_err_str(sts),
              sts);
          return sts;
        }
        break;
      case BF_DEV_FAMILY_TOFINO3:
        sts = bf_tof3_serdes_rx_polarity_set(
            dev_id, dev_port, log_lane, rx_inv, false);
        if (sts != BF_SUCCESS) {
          PM_ERROR(
              "Unable to set the serdes rx inv for dev : %d : front port : "
              "%d/%d "
              "(%d) : lane: %d %s (%d)",
              dev_id,
              port_hdl->conn_id,
              port_hdl->chnl_id,
              dev_port,
              log_lane,
              bf_err_str(sts),
              sts);
          return sts;
        }
        sts = bf_tof3_serdes_tx_polarity_set(
            dev_id, dev_port, log_lane, tx_inv, false);
        if (sts != BF_SUCCESS) {
          PM_ERROR(
              "Unable to set the serdes tx inv for dev : %d : front port : "
              "%d/%d "
              "(%d) : lane: %d %s (%d)",
              dev_id,
              port_hdl->conn_id,
              port_hdl->chnl_id,
              dev_port,
              log_lane,
              bf_err_str(sts),
              sts);
          return sts;
        }
        break;
      default:
        return BF_NOT_IMPLEMENTED;
        break;
    }
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_serdes_tx_eq_params_set(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl,
    uint32_t num_entries,
    bf_pal_serdes_tx_eq_params_t *tx_eq) {
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;

  if ((!port_hdl) || (!tx_eq)) {
    return BF_INVALID_ARG;
  }

  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  if (port_info->is_added) {
    // n_lanes exceeding the max supported by the device is taken care during
    // port-add. Hence not checked here.
    if (num_entries != port_info->n_lanes) {
      PM_ERROR("Port lanes invalid for dev : %d : front port : %d/%d (%d)",
               dev_id,
               port_hdl->conn_id,
               port_hdl->chnl_id,
               dev_port);
      return BF_INVALID_ARG;
    }
  } else {
    PM_ERROR("Port is not added dev : %d : front port : %d/%d (%d)",
             dev_id,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_port);
    return BF_NOT_IMPLEMENTED;
  }

  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));

  uint32_t log_lane;
  switch (dev_family) {
    case BF_DEV_FAMILY_TOFINO2:
      // If user has applied custom Tx Eq settings, override defaults
      if (port_info->serdes_tx_eq_override) {
        break;
      }
      for (log_lane = 0; log_lane < num_entries; log_lane++) {
        // If user has applied custom lane Tx Eq settings, don't modify HW
        if (port_info->serdes_lane_tx_eq_override[log_lane]) {
          PM_DEBUG(
              "%d/%d : (dev_id=%d d_p=%d) : lane=%d : Tx Eq override set, "
              "skip..",
              port_hdl->conn_id,
              port_hdl->chnl_id,
              dev_id,
              dev_port,
              log_lane);
          continue;
        }
        PM_DEBUG(
            "Dev : %d : Port : %d/%d (%d) lane:%d pre2:%d pre1:%d main:%d "
            "post1:%d "
            "post2:%d",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            dev_port,
            log_lane,
            tx_eq->tx_eq.tof2[log_lane].tx_pre2,
            tx_eq->tx_eq.tof2[log_lane].tx_pre1,
            tx_eq->tx_eq.tof2[log_lane].tx_main,
            tx_eq->tx_eq.tof2[log_lane].tx_post1,
            tx_eq->tx_eq.tof2[log_lane].tx_post2);

        sts = bf_tof2_serdes_tx_taps_set(dev_id,
                                         dev_port,
                                         log_lane,
                                         tx_eq->tx_eq.tof2[log_lane].tx_pre2,
                                         tx_eq->tx_eq.tof2[log_lane].tx_pre1,
                                         tx_eq->tx_eq.tof2[log_lane].tx_main,
                                         tx_eq->tx_eq.tof2[log_lane].tx_post1,
                                         tx_eq->tx_eq.tof2[log_lane].tx_post2,
                                         false);

        if (sts != BF_SUCCESS) {
          PM_ERROR(
              "Unable to set the serdes tx FIR taps for dev : %d : front port "
              ": "
              "%d/%d "
              "(%d) : lane: %d %s (%d)",
              dev_id,
              port_hdl->conn_id,
              port_hdl->chnl_id,
              dev_port,
              log_lane,
              bf_err_str(sts),
              sts);
          return sts;
        }
      }
      break;
    case BF_DEV_FAMILY_TOFINO3:
      // If user has applied custom Tx Eq settings, override defaults
      if (port_info->serdes_tx_eq_override) {
        break;
      }
      for (log_lane = 0; log_lane < num_entries; log_lane++) {
        // If user has applied custom lane Tx Eq settings, don't modify HW
        if (port_info->serdes_lane_tx_eq_override[log_lane]) {
          PM_DEBUG(
              "%d/%d : (dev_id=%d d_p=%d) : lane=%d : Tx Eq override set, "
              "skip..",
              port_hdl->conn_id,
              port_hdl->chnl_id,
              dev_id,
              dev_port,
              log_lane);
          continue;
        }
        PM_DEBUG(
            "Dev : %d : Port : %d/%d (%d) lane:%d pre2:%d pre1:%d main:%d "
            "post1:%d "
            "post2:%d",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            dev_port,
            log_lane,
            tx_eq->tx_eq.tof2[log_lane].tx_pre2,
            tx_eq->tx_eq.tof2[log_lane].tx_pre1,
            tx_eq->tx_eq.tof2[log_lane].tx_main,
            tx_eq->tx_eq.tof2[log_lane].tx_post1,
            tx_eq->tx_eq.tof2[log_lane].tx_post2);
        sts = bf_tof3_serdes_txfir_config_set(
            dev_id,
            dev_port,
            log_lane,
            0 /*cm3*/,
            tx_eq->tx_eq.tof2[log_lane].tx_pre2,
            tx_eq->tx_eq.tof2[log_lane].tx_pre1,
            tx_eq->tx_eq.tof2[log_lane].tx_main,
            tx_eq->tx_eq.tof2[log_lane].tx_post1);

        if (sts != BF_SUCCESS) {
          PM_ERROR(
              "Unable to set the serdes tx FIR taps for dev : %d : front port "
              ": "
              "%d/%d "
              "(%d) : lane: %d %s (%d)",
              dev_id,
              port_hdl->conn_id,
              port_hdl->chnl_id,
              dev_port,
              log_lane,
              bf_err_str(sts),
              sts);
          return sts;
        }
      }
      break;
    default:
      return BF_NOT_IMPLEMENTED;
      break;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_serdes_tx_eq_params_get(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl,
    uint32_t num_entries,
    bf_pal_serdes_tx_eq_params_t *tx_eq) {
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;

  if ((!port_hdl) || (!tx_eq)) {
    return BF_INVALID_ARG;
  }

  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  if (!port_info->is_added) {
    PM_ERROR("Port is not added dev : %d : front port : %d/%d (%d)",
             dev_id,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_port);
    return BF_NOT_IMPLEMENTED;
  }

  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));

  uint32_t log_lane;
  switch (dev_family) {
    case BF_DEV_FAMILY_TOFINO2:
      for (log_lane = 0; log_lane < num_entries; log_lane++) {
        sts = bf_tof2_serdes_tx_taps_get(dev_id,
                                         dev_port,
                                         log_lane,
                                         &tx_eq->tx_eq.tof2[log_lane].tx_pre2,
                                         &tx_eq->tx_eq.tof2[log_lane].tx_pre1,
                                         &tx_eq->tx_eq.tof2[log_lane].tx_main,
                                         &tx_eq->tx_eq.tof2[log_lane].tx_post1,
                                         &tx_eq->tx_eq.tof2[log_lane].tx_post2);
        if (sts != BF_SUCCESS) {
          PM_ERROR(
              "Unable to get the serdes tx FIR taps for dev : %d : front port "
              ": "
              "%d/%d "
              "(%d) : lane: %d %s (%d)",
              dev_id,
              port_hdl->conn_id,
              port_hdl->chnl_id,
              dev_port,
              log_lane,
              bf_err_str(sts),
              sts);
          return sts;
        }
      }

      break;
    default:
      return BF_NOT_IMPLEMENTED;
      break;
  }
  return BF_SUCCESS;
}

bool bf_pm_intf_is_device_family_tofino(bf_dev_id_t dev_id) {
  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));
  if (dev_family == BF_DEV_FAMILY_TOFINO) {
    return true;
  }

  return false;
}

// Returns cumulative serdes tx-ready state per port
bf_status_t bf_pm_port_serdes_tx_ready_get(bf_dev_id_t dev_id,
                                           bf_pal_front_port_handle_t *port_hdl,
                                           bool *tx_ready) {
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;

  if ((!port_hdl) || (!tx_ready)) {
    return BF_INVALID_ARG;
  }

  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  if (port_info->admin_state != PM_PORT_ENABLED) {
    *tx_ready = false;
  } else {
    *tx_ready = port_info->serdes_tx_ready;
  }

  return BF_SUCCESS;
}

// Returns cumulative serdes rx-ready state per port
bf_status_t bf_pm_port_serdes_rx_ready_get(bf_dev_id_t dev_id,
                                           bf_pal_front_port_handle_t *port_hdl,
                                           bool *rx_ready) {
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;

  if ((!port_hdl) || (!rx_ready)) {
    return BF_INVALID_ARG;
  }
  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  if (port_info->admin_state != PM_PORT_ENABLED) {
    *rx_ready = false;
  } else {
    *rx_ready = port_info->serdes_rx_ready;
  }

  return BF_SUCCESS;
}

// This func is valid only when AN=OFF.
// port-fsm will check for this flag to proceed
// with DFE tuning.
bf_status_t bf_pm_port_serdes_rx_ready_for_bringup_set(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl, bool enable) {
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;

  if (!port_hdl) {
    return BF_INVALID_ARG;
  }
  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  port_info->serdes_rx_ready_for_dfe = enable;

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_module_ready_for_link_handling_set(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl, bool enable) {
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;

  if (!port_hdl) {
    return BF_INVALID_ARG;
  }
  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  port_info->module_ready_for_link = enable;

  return BF_SUCCESS;
}

// Gets serdes rx bringup state per port
// Called by port-fsm to do DFE
bf_status_t pm_port_serdes_rx_ready_for_bringup_get(bf_dev_id_t dev_id,
                                                    bf_dev_port_t dev_port,
                                                    bool *do_dfe) {
  bf_pm_port_info_t *port_info;

  port_info = pm_port_info_get(dev_id, dev_port);
  if (port_info == NULL) {
    PM_ERROR("Unable to get port info for dev : %d : dev-port : %d",
             dev_id,
             dev_port);
    return BF_INVALID_ARG;
  }

  if (!do_dfe) return BF_INVALID_ARG;

  *do_dfe = port_info->serdes_rx_ready_for_dfe;

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_module_ready_for_link_handling_get(
    bf_dev_id_t dev_id, bf_dev_port_t dev_port, bool *update_link) {
  bf_pm_port_info_t *port_info;

  port_info = pm_port_info_get(dev_id, dev_port);
  if (port_info == NULL) {
    PM_ERROR("Unable to get port info for dev : %d : dev-port : %d",
             dev_id,
             dev_port);
    return BF_INVALID_ARG;
  }

  if (!update_link) return BF_INVALID_ARG;

  *update_link = port_info->module_ready_for_link;

  return BF_SUCCESS;
}

// sets cumulative serdes rx-ready state per port
// Called by port-fsm.
bf_status_t pm_port_serdes_rx_ready_set(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        bool rx_ready) {
  bf_pm_port_info_t *port_info;

  port_info = pm_port_info_get(dev_id, dev_port);
  if (port_info == NULL) {
    PM_ERROR("Unable to get port info for dev : %d : dev-port : %d",
             dev_id,
             dev_port);
    return BF_INVALID_ARG;
  }

  port_info->serdes_rx_ready = rx_ready;
  return BF_SUCCESS;
}

// sets cumulative serdes tx-ready state per port
// Called by port-fsm.
bf_status_t pm_port_serdes_tx_ready_set(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        bool tx_ready) {
  bf_pm_port_info_t *port_info;

  port_info = pm_port_info_get(dev_id, dev_port);
  if (port_info == NULL) {
    PM_ERROR("Unable to get port info for dev : %d : dev-port : %d",
             dev_id,
             dev_port);
    return BF_INVALID_ARG;
  }

  port_info->serdes_tx_ready = tx_ready;
  return BF_SUCCESS;
}

/**
 * Select port-fsm
 */
static bf_status_t pm_port_select_port_fsm_mode(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl) {
  bf_status_t sts = BF_SUCCESS;
  bf_pm_port_info_t *port_info;
  bool do_an = false;
  bf_pm_port_fsm_mode_t fsm_mode;
  bf_dev_id_t dev_id_of_port = 0;

  if (!port_hdl) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  bool is_sw_model = true;
  sts = bf_drv_device_type_get(dev_id, &is_sw_model);
  if (sts != BF_SUCCESS) {
    PM_ERROR(
        "Unable to get device type for dev : %d : front port : %d/%d err %s "
        "(%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        bf_err_str(sts),
        sts);
    return BF_INVALID_ARG;
  }

#if defined(DEVICE_IS_EMULATOR)  // Emulator
  // Note the pcs or mac loopback are selected within
  // emulator-fsm
  fsm_mode = BF_PM_PORT_FSM_MODE_EMULATOR;
  pm_port_fsm_mode_set(port_info->dev_port, fsm_mode);
  //  PM_ERROR("Selected eumlator fsm for dev : %d : front port : %d/%d",
  //         dev_id,
  //       port_hdl->conn_id,
  //     port_hdl->chnl_id);

  return BF_SUCCESS;
#endif

  // Handle all special case first
  if (is_sw_model) {
    fsm_mode = BF_PM_PORT_FSM_MODE_SW_MODEL;
  } else if (port_info->prbs_mode != BF_PORT_PRBS_MODE_NONE) {
    fsm_mode = BF_PM_PORT_FSM_MODE_PRBS;
  } else if (port_info->lpbk_mode != BF_LPBK_NONE) {
    if (port_info->lpbk_mode == BF_LPBK_PIPE) {
      fsm_mode = BF_PM_PORT_FSM_MODE_PIPE_LOOPBCK;
    } else if (port_info->lpbk_mode == BF_LPBK_MAC_NEAR) {
      fsm_mode = BF_PM_PORT_FSM_MODE_MAC_NEAR_LOOPBCK;
    } else if (port_info->lpbk_mode == BF_LPBK_MAC_FAR) {
      fsm_mode = BF_PM_PORT_FSM_MODE_MAC_FAR_LOOPBCK;
    } else if (port_info->lpbk_mode == BF_LPBK_PCS_NEAR) {
      fsm_mode = BF_PM_PORT_FSM_MODE_PCS_LOOPBCK;
    } else {
      PM_ERROR(
          "Unsupported loopback-mode %d for dev : %d : front port : %d/%d ",
          dev_id,
          port_hdl->conn_id,
          port_hdl->chnl_id,
          port_info->lpbk_mode);
      sts = BF_NOT_IMPLEMENTED;
      return sts;
    }
  } else if (port_info->port_dir == PM_PORT_DIR_TX_ONLY) {
    fsm_mode = BF_PM_PORT_FSM_MODE_TX_MODE;
  } else {  // normal mode
    do_an = pm_port_should_autoneg(dev_id, port_info->dev_port);
    // AN mode can be selected only if direction mode is FULL DUPLEX (default
    // mode)
    if (do_an && port_info->port_dir == PM_PORT_DIR_DEFAULT) {
      fsm_mode = BF_PM_PORT_FSM_MODE_AUTONEG;
    } else {
      // Tx only mode (processed earlier by this function) uses a special FSM
      fsm_mode = BF_PM_PORT_FSM_MODE_DFE;  // autoneg-off
    }
  }
  pm_port_fsm_mode_set(port_info->dev_port, fsm_mode);
  return sts;
}

bf_status_t bf_pm_port_prbs_mode_set(bf_dev_id_t dev_id,
                                     bf_pal_front_port_handle_t *port_hdl_list,
                                     uint32_t len,
                                     bf_port_prbs_mode_t prbs_mode) {
  bf_pm_port_info_t *port_info;
  uint32_t i;
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_dev_id_t dev_id_of_port = 0;
  bf_serdes_encoding_mode_t enc_mode;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl_list) return BF_INVALID_ARG;

  for (i = 0; i < len; i++) {
    sts = bf_pm_port_info_all_get(__func__,
                                  dev_id,
                                  &port_hdl_list[i],
                                  &port_info,
                                  &dev_id_of_port,
                                  &dev_port);
    if (sts != BF_SUCCESS) return sts;

    if (bf_pm_intf_is_device_family_tofino(port_info->pltfm_port_info.dev_id)) {
      PM_ERROR(
          "PRBS mode set API not support on Tofino for dev : %d : dev-port : "
          "%d",
          dev_id,
          dev_port);
      continue;
    }

    bf_dev_family_t dev_family =
        bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));

    // tof3 does support these modes
    if (dev_family == BF_DEV_FAMILY_TOFINO2) {
      bf_serdes_encoding_mode_get(
          port_info->speed, port_info->n_lanes, &enc_mode);
      if ((enc_mode == BF_SERDES_ENC_MODE_NRZ) &&
          (prbs_mode == BF_PORT_PRBS_MODE_13)) {
        PM_ERROR(
            "PRBS 13 is not supported for NRZ supported speeds dev_port : %d\n",
            dev_port);
        continue;
      }
      if ((enc_mode == BF_SERDES_ENC_MODE_PAM4) &&
          (prbs_mode == BF_PORT_PRBS_MODE_23)) {
        PM_ERROR(
            "PRBS 23 is not supported for PAM4 supported speeds dev_port : "
            "%d\n",
            dev_port);
        continue;
      }
    }

    if (port_info->is_added) {
      /* configure prbs mode in port_mgr because that is where the mode to
       * program is retreived from */
      sts = bf_port_prbs_mode_set(dev_id_of_port, dev_port, prbs_mode);
      if (sts != BF_SUCCESS) return BF_INVALID_ARG;

      port_info->prbs_mode = prbs_mode;
    }
  }
  return BF_SUCCESS;
}

bf_status_t bf_pm_port_prbs_mode_clear(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl_list,
    uint32_t len) {
  bf_pm_port_info_t *port_info;
  uint32_t i;
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl_list) return BF_INVALID_ARG;

  for (i = 0; i < len; i++) {
    if (port_hdl_list[i].chnl_id != 0) {
      continue;
    }
    sts = bf_pm_port_info_all_get(__func__,
                                  dev_id,
                                  &port_hdl_list[i],
                                  &port_info,
                                  &dev_id_of_port,
                                  &dev_port);
    if (sts != BF_SUCCESS) return sts;

    if (bf_pm_intf_is_device_family_tofino(port_info->pltfm_port_info.dev_id)) {
      PM_ERROR(
          "PRBS API clear not support on Tofino for dev : %d : dev-port : %d",
          dev_id,
          dev_port);
      continue;
    }
    if (port_info->is_added) {
      /* configure prbs mode in port_mgr because that is where the mode to
       * program is retreived from */
      sts = bf_port_prbs_mode_set(
          dev_id_of_port, dev_port, BF_PORT_PRBS_MODE_NONE);
      if (sts != BF_SUCCESS) return BF_INVALID_ARG;

      port_info->prbs_mode = BF_PORT_PRBS_MODE_NONE;
    }
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_actv_chnl_mask_get(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port,
                                     uint32_t *msk) {
  bf_pm_port_info_t *port_info;
  port_info = pm_port_info_get(dev_id, dev_port);
  if (port_info == NULL) {
    *msk = 0;
    return BF_INVALID_ARG;
  }
  *msk = port_info->actv_chnl_mask;

  return BF_SUCCESS;
}

bf_status_t bf_pm_actv_chnl_mask_set(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port,
                                     uint32_t msk) {
  bf_pm_port_info_t *port_info;
  port_info = pm_port_info_get(dev_id, dev_port);
  if (port_info == NULL) {
    return BF_INVALID_ARG;
  }
  port_info->actv_chnl_mask = msk;
  return BF_SUCCESS;
}

bf_status_t bf_pm_port_prbs_mode_stats_get(bf_dev_id_t dev_id,
                                           bf_pal_front_port_handle_t *port_hdl,
                                           bf_port_sds_prbs_stats_t *stats) {
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!stats) return BF_INVALID_ARG;

  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));

  pm_port_exclusive_access_start(dev_id, port_info);
  switch (dev_family) {
    case BF_DEV_FAMILY_TOFINO2:
      sts = pm_port_tof2_prbs_stats_get(dev_id, dev_port, stats);
      if (sts != BF_SUCCESS) {
        PM_ERROR(
            "Unable to get prbs stats for dev : %d : front port : %d/%d : %s "
            "(%d)",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            bf_err_str(sts),
            sts);
        pm_port_exclusive_access_end(dev_id, port_info);
        return sts;
      }
      break;
    default:
      pm_port_exclusive_access_end(dev_id, port_info);
      return BF_NOT_IMPLEMENTED;
      break;
  }

  pm_port_exclusive_access_end(dev_id, port_info);
  return BF_SUCCESS;
}

bf_status_t bf_pm_port_eye_val_get(bf_dev_id_t dev_id,
                                   bf_pal_front_port_handle_t *port_hdl,
                                   bf_port_eye_val_t *eye) {
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!eye) return BF_INVALID_ARG;

  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));

  pm_port_exclusive_access_start(dev_id, port_info);
  switch (dev_family) {
    case BF_DEV_FAMILY_TOFINO2:
      sts = pm_port_tof2_eye_val_get(dev_id, dev_port, eye);
      if (sts != BF_SUCCESS) {
        PM_ERROR(
            "Unable to get prbs stats for dev : %d : front port : %d/%d : %s "
            "(%d)",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            bf_err_str(sts),
            sts);
        pm_port_exclusive_access_end(dev_id, port_info);
        return sts;
      }
      break;
    default:
      pm_port_exclusive_access_end(dev_id, port_info);
      return BF_NOT_IMPLEMENTED;
      break;
  }

  pm_port_exclusive_access_end(dev_id, port_info);
  return BF_SUCCESS;
}

bf_status_t bf_pm_port_ber_get(bf_dev_id_t dev_id,
                               bf_pal_front_port_handle_t *port_hdl,
                               bf_port_ber_t *ber) {
  bf_status_t sts = BF_SUCCESS;
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!ber) return BF_INVALID_ARG;

  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));

  pm_port_exclusive_access_start(dev_id, port_info);
  switch (dev_family) {
    case BF_DEV_FAMILY_TOFINO2:
      sts = pm_port_tof2_ber_get(dev_id, dev_port, ber);
      if (sts != BF_SUCCESS) {
        PM_ERROR(
            "Unable to get prbs stats for dev : %d : front port : %d/%d : %s "
            "(%d)",
            dev_id,
            port_hdl->conn_id,
            port_hdl->chnl_id,
            bf_err_str(sts),
            sts);
        pm_port_exclusive_access_end(dev_id, port_info);
        return sts;
      }
      break;
    default:
      pm_port_exclusive_access_end(dev_id, port_info);
      return BF_NOT_IMPLEMENTED;
      break;
  }

  pm_port_exclusive_access_end(dev_id, port_info);
  return BF_SUCCESS;
}

bf_status_t bf_pm_port_is_enabled(bf_dev_id_t dev_id,
                                  bf_pal_front_port_handle_t *port_hdl,
                                  bool *is_enabled) {
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;

  if (!port_hdl || !is_enabled) {
    return BF_INVALID_ARG;
  }
  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  *is_enabled = port_info->admin_state == PM_PORT_ENABLED;
  return BF_SUCCESS;
}

uint32_t bf_pm_port_get_num_of_lanes(bf_dev_id_t dev_id,
                                     bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  if (!port_hdl) return 0;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info) return 0;

  return port_info->n_lanes;
}

/**
 * @brief Gets the recirc dev-ports available on the device.
 *
 *  Given an pointer to an array[30] and dev-id, returns recirc dev-ports
 *  and maximum of recirc-port on that dev-id. Note max dev-ports returned
 *  will be 27 - upto tofino2.
 *
 * @see include/bf_pm/bf_pm_intf.h for detail description.
 */
uint32_t bf_pm_recirc_devports_get(bf_dev_id_t dev_id,
                                   uint32_t *recirc_devport_list) {
  uint32_t num_pipes, i, p, recirc_idx;
  bf_status_t sts;

  // Safety checks
  if (dev_id < 0) return 0;
  if (dev_id >= BF_MAX_DEV_COUNT) return 0;
  if (!recirc_devport_list) return 0;

  sts = lld_sku_get_num_active_pipes(dev_id, &num_pipes);
  if (sts != BF_SUCCESS) {
    PM_ERROR("Unable to get the active pipes for dev : %d : %s (%d)",
             dev_id,
             bf_err_str(sts),
             sts);
    return 0;
  }

  i = 0;
  recirc_idx = 0;
  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));
  switch (dev_family) {
    case BF_DEV_FAMILY_TOFINO3:
      /*
         Pipe == 0:
           port 0 - pcie port
           port 2, 4 - ETH ports
           port 6 is recirc port
         Pipe != 0:
           0, 2, 4 and 6 are recirc ports
      */
      recirc_devport_list[0] = MAKE_DEV_PORT(0, 6);
      recirc_idx = 1;
      for (p = 1; p < num_pipes; p++) {
        recirc_devport_list[recirc_idx++] = MAKE_DEV_PORT(p, 0);
        recirc_devport_list[recirc_idx++] = MAKE_DEV_PORT(p, 2);
        recirc_devport_list[recirc_idx++] = MAKE_DEV_PORT(p, 4);
        recirc_devport_list[recirc_idx++] = MAKE_DEV_PORT(p, 6);
      }
      return recirc_idx;

    case BF_DEV_FAMILY_TOFINO2:
      /*
         Pipe == 0:
           Chan 0 - 25g pcie path
           Chan 1 - 50g recirc
           Chan 2-5: Eth CPU
           Chan 6-7: 100g recirc
         Pipe != 0:
           0-7 are recirc ports
      */
      recirc_devport_list[i++] = MAKE_DEV_PORT(0, 1);
      recirc_devport_list[i++] = MAKE_DEV_PORT(0, 6);
      recirc_devport_list[i++] = MAKE_DEV_PORT(0, 7);
      recirc_idx = 3;
      for (p = 1; p < num_pipes; p++) {
        for (i = 0; i <= 7; i++, recirc_idx++) {
          recirc_devport_list[recirc_idx] = MAKE_DEV_PORT(p, i);
        }
      }
      return recirc_idx;
    case BF_DEV_FAMILY_TOFINO:
      if (num_pipes == 4) {
        // 68, 192, 196, 324, 448, 452
        for (i = 0; i < 4; i++, recirc_idx++) {
          recirc_devport_list[recirc_idx] = MAKE_DEV_PORT(0, (68 + i));
        }

        for (i = 0; i < 8; i++, recirc_idx++) {
          recirc_devport_list[recirc_idx] = MAKE_DEV_PORT(1, (64 + i));
        }

        for (i = 0; i < 4; i++, recirc_idx++) {
          recirc_devport_list[recirc_idx] = MAKE_DEV_PORT(2, (68 + i));
        }
        for (i = 0; i < 8; i++, recirc_idx++) {
          recirc_devport_list[recirc_idx] = MAKE_DEV_PORT(3, (64 + i));
        }

        return recirc_idx;
      }
      // 2-pipe systems.  68, 196.
      for (i = 0; i < 4; i++, recirc_idx++) {
        recirc_devport_list[recirc_idx] = MAKE_DEV_PORT(0, (68 + i));
      }
      for (i = 0; i < 4; i++, recirc_idx++) {
        recirc_devport_list[recirc_idx] = MAKE_DEV_PORT(1, (68 + i));
      }
      return recirc_idx;
    default:
      return 0;
  }

  return 0;
}

bf_status_t bf_pm_port_link_up_max_err_set(bf_dev_id_t dev_id,
                                           bf_pal_front_port_handle_t *port_hdl,
                                           uint32_t max_errors) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port;
  bf_dev_port_t dev_port;
  bf_status_t sts;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  PM_TRACE("%d:%d/%d: link-up max-errors-set=%d",
           dev_id,
           port_hdl->conn_id,
           port_hdl->chnl_id,
           max_errors);

  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) return BF_INVALID_ARG;

  pm_port_exclusive_access_start(dev_id, port_info);
  sts = bf_port_errored_block_thresh_set(dev_id, dev_port, max_errors);
  pm_port_exclusive_access_end(dev_id, port_info);

  return sts;
}

bf_status_t bf_pm_port_link_up_max_err_get(bf_dev_id_t dev_id,
                                           bf_pal_front_port_handle_t *port_hdl,
                                           uint32_t *max_errors) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port;
  bf_dev_port_t dev_port;
  bf_status_t sts;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl || !max_errors) return BF_INVALID_ARG;

  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) return BF_INVALID_ARG;

  pm_port_exclusive_access_start(dev_id, port_info);
  sts = bf_port_errored_block_thresh_get(dev_id, dev_port, max_errors);
  pm_port_exclusive_access_end(dev_id, port_info);

  return sts;
}

bf_status_t bf_pm_port_debounce_thresh_set(bf_dev_id_t dev_id,
                                           bf_pal_front_port_handle_t *port_hdl,
                                           uint32_t debounce_value) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port;

  // Safety checks
  if (dev_id < 0 || dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (!port_info) return BF_OBJECT_NOT_FOUND;

  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (debounce_value > BF_PM_FSM_LINK_UP_THRSHLD_MAX) {
    PM_ERROR(
        "Debounce threshold out of range for dev : %d : front port : %d/%d"
        " (%d) value=%d > MAX (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        port_info->dev_port,
        debounce_value,
        BF_PM_FSM_LINK_UP_THRSHLD_MAX);
    return BF_INVALID_ARG;
  }

  port_info->fsm_debounce_thr = debounce_value;
  PM_TRACE("%d:%d/%d: link-up debounce set=%d",
           dev_id,
           port_hdl->conn_id,
           port_hdl->chnl_id,
           debounce_value);

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_debounce_thresh_get(bf_dev_id_t dev_id,
                                           bf_pal_front_port_handle_t *port_hdl,
                                           uint32_t *debounce_value) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port;

  // Safety checks
  if (dev_id < 0 || dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl || !debounce_value) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (!port_info) return BF_OBJECT_NOT_FOUND;
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  *debounce_value = port_info->fsm_debounce_thr;

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_debounce_restore(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port) {
  bf_pm_port_info_t *port_info;

  port_info = pm_port_info_get(dev_id, dev_port);

  if (!port_info) return BF_OBJECT_NOT_FOUND;
  if (!port_info->is_added) return BF_INVALID_ARG;

  port_info->fsm_debounce_cnt = 0;
  return BF_SUCCESS;
}

bool bf_pm_port_debounce_adj_chk(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  bf_pm_port_info_t *port_info;

  port_info = pm_port_info_get(dev_id, dev_port);

  if (!port_info || !port_info->is_added) return false;

  /* verify the debounce counter is in the allowed range */
  if (port_info->fsm_debounce_cnt > BF_PM_FSM_LINK_UP_THRSHLD_MAX) {
    port_info->fsm_debounce_cnt = BF_PM_FSM_LINK_UP_THRSHLD_MAX;
    return true;
  }

  if (port_info->fsm_debounce_cnt <= port_info->fsm_debounce_thr) {
    port_info->fsm_debounce_cnt++;
  }
  return (port_info->fsm_debounce_cnt > port_info->fsm_debounce_thr);
}

/**
 * returns number fo serdes lanes per lane
 */
uint32_t bf_pm_port_num_serdes_per_lane_get(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl) {
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;
  bf_map_sts_t map_sts;
  bf_pm_mac_multi_lane_t *mac_multi_map;
  bf_status_t sts;

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) {
    return 0;
  }
  dev_id = dev_id_of_port;

  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) return 0;

  if (bf_pm_intf_is_device_family_tofino3(dev_id)) {
    PORT_MGR_LOCK(&mac_map_mtx[dev_id]);
    map_sts = bf_map_get(&bf_pm_mac_map_db[dev_id],
                         port_info->pltfm_port_info.log_mac_id,
                         (void *)&mac_multi_map);
    PORT_MGR_UNLOCK(&mac_map_mtx[dev_id]);
    if (map_sts == BF_MAP_OK) {
      return mac_multi_map->mac_chnl[port_info->pltfm_port_info.log_mac_lane]
          .num_serdes_per_lane;
    }
    PM_ERROR(
        "Unable to get mac from map for dev : %d port : %d/%d : mac : %d "
        "map_err : %d",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        port_info->pltfm_port_info.log_mac_id,
        map_sts);
    return 0;
  }

  return 1;
}

/**
 * Set polarity received from platform
 */
bf_status_t bf_pm_port_multi_serdes_polarity_set(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl) {
  bf_dev_port_t dev_port;
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;
  bf_status_t sts;

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) {
    return BF_INVALID_ARG;
  }
  dev_id = dev_id_of_port;

  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) return BF_INVALID_ARG;

  if (!bf_pm_intf_is_device_family_tofino3(dev_id)) {
    return BF_SUCCESS;
  }

  uint32_t mac_id = port_info->pltfm_port_info.log_mac_id;
  int nlanes = bf_pm_port_get_num_of_lanes(dev_id, port_hdl);
  //  int nserdes = lld_get_num_serdes_per_mac(dev_id, dev_port) == 8 ? 2 : 1;
  for (int lane = 0; lane < nlanes; lane++) {
    uint32_t idx = port_hdl->chnl_id + lane;
    PM_DEBUG(
        " Rx and Tx pol for dev : %d port : %d/%d : mac : %d "
        "lane %d tx-pol %d rx-pol %d",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        mac_id,
        lane,
        mac_tx_pol[mac_id][idx],
        mac_rx_pol[mac_id][idx]);
    sts = bf_tof3_serdes_tx_polarity_set(dev_id,
                                         dev_port,
                                         lane,
                                         mac_tx_pol[mac_id][idx],
                                         true /*not sure about this*/);
    if (sts != BF_SUCCESS) {
      PM_DEBUG("Error setting tx polarity on %d/%d, lane %d (inv=%d)\n",
               port_hdl->conn_id,
               port_hdl->chnl_id,
               lane,
               mac_tx_pol[mac_id][idx]);
    }
    sts = bf_tof3_serdes_rx_polarity_set(dev_id,
                                         dev_port,
                                         lane,
                                         mac_rx_pol[mac_id][idx],
                                         true /*not sure about this*/);
    if (sts != BF_SUCCESS) {
      PM_DEBUG("Error setting rx polarity on %d/%d, lane %d (inv=%d)\n",
               port_hdl->conn_id,
               port_hdl->chnl_id,
               lane,
               mac_rx_pol[mac_id][idx]);
    }
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_port_1588_timestamp_delta_tx_set(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl, uint16_t delta) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port;
  bf_dev_port_t dev_port;
  bf_status_t sts;

  // Safety checks
  if (dev_id < 0 || dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;

  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  if (!port_info) return BF_OBJECT_NOT_FOUND;
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  return bf_port_1588_timestamp_delta_tx_set(dev_id, dev_port, delta);
}

bf_status_t bf_pm_port_1588_timestamp_delta_tx_get(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl, uint16_t *delta) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port;
  bf_dev_port_t dev_port;
  bf_status_t sts;

  // Safety checks
  if (dev_id < 0 || dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!delta) return BF_INVALID_ARG;

  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  if (!port_info) return BF_OBJECT_NOT_FOUND;
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  return bf_port_1588_timestamp_delta_tx_get(dev_id, dev_port, delta);
}

bf_status_t bf_pm_port_1588_timestamp_delta_rx_set(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl, uint16_t delta) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port;
  bf_dev_port_t dev_port;
  bf_status_t sts;

  // Safety checks
  if (dev_id < 0 || dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;

  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  if (!port_info) return BF_OBJECT_NOT_FOUND;
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  return bf_port_1588_timestamp_delta_rx_set(dev_id, dev_port, delta);
}

bf_status_t bf_pm_port_1588_timestamp_delta_rx_get(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl, uint16_t *delta) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port;
  bf_dev_port_t dev_port;
  bf_status_t sts;

  // Safety checks
  if (dev_id < 0 || dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!delta) return BF_INVALID_ARG;

  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  if (!port_info) return BF_OBJECT_NOT_FOUND;
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  return bf_port_1588_timestamp_delta_rx_get(dev_id, dev_port, delta);
}

bf_status_t bf_pm_port_1588_timestamp_get(bf_dev_id_t dev_id,
                                          bf_pal_front_port_handle_t *port_hdl,
                                          uint64_t *ts,
                                          bool *ts_valid,
                                          int *ts_id) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port;
  bf_dev_port_t dev_port;
  bf_status_t sts;

  // Safety checks
  if (dev_id < 0 || dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!ts) return BF_INVALID_ARG;
  if (!ts_valid) return BF_INVALID_ARG;
  if (!ts_id) return BF_INVALID_ARG;

  sts = bf_pm_port_info_all_get(
      __func__, dev_id, port_hdl, &port_info, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) return sts;

  if (!port_info) return BF_OBJECT_NOT_FOUND;
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }
  return bf_port_1588_timestamp_tx_get(dev_id, dev_port, ts, ts_valid, ts_id);
}

bf_status_t bf_pm_port_pkt_rate_get(bf_dev_id_t dev_id,
                                    bf_pal_front_port_handle_t *port_hdl,
                                    bf_pkt_rate_t *pkt_rate) {
  bf_pm_port_info_t *port_info;
  bf_dev_id_t dev_id_of_port = 0;

  // Safety checks
  if (dev_id < 0) return BF_INVALID_ARG;
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  if (!port_hdl) return BF_INVALID_ARG;
  if (!pkt_rate) return BF_INVALID_ARG;

  port_info = pm_port_info_get_from_port_hdl(port_hdl, &dev_id_of_port);
  if (dev_id_of_port != dev_id) {
    PM_ERROR("%s: Warning: : %d/%d found on dev%d : exp dev%d",
             __func__,
             port_hdl->conn_id,
             port_hdl->chnl_id,
             dev_id_of_port,
             dev_id);
  }

  if (!port_info->is_added) return BF_INVALID_ARG;

  pm_port_exclusive_access_start(dev_id, port_info);
  pkt_rate->tx_pps = port_info->tx_pps;
  pkt_rate->rx_pps = port_info->rx_pps;
  pkt_rate->tx_rate = port_info->tx_rate;
  pkt_rate->rx_rate = port_info->rx_rate;

  pm_port_exclusive_access_end(dev_id, port_info);

  return BF_SUCCESS;
}
