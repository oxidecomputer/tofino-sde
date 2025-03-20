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


/*
    bf_hw_pltfrm_porting.c
*/
/* Standard includes */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

/* <bf_syslib> includes */
#include <target-sys/bf_sal/bf_sys_intf.h>

/* <clish> includes  */
#include <target-utils/clish/thread.h>

/* <bf_driver> includes */
#include <bf_types/bf_types.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include <bf_pm/bf_pm_intf.h>
#include <dvm/bf_drv_intf.h>

/* Local includes */
#include "bf_hw_porting_config.h"
#include "bf_model_pltfm_porting.h"
#include "bf_switchd.h"
#include "switch_config.h"

#include <target-utils/map/map.h>
#include <target-utils/third-party/cJSON/cJSON.h>
#include <ctx_json/ctx_json_utils.h>

#define BF_BMAP_LOG_INFO(flag, format, ...)    \
  if (flag) {                                  \
    printf("BD_MAP : " format, ##__VA_ARGS__); \
  }

#define BF_BMAP_LOG_ERROR(...) printf(__VA_ARGS__)
#define BF_BMAP_LOG_DEBUG(...) printf(__VA_ARGS__)

// forward declares
static bf_status_t bf_hw_cfg_media_type_set(
    bf_pal_front_port_handle_t *port_hdl, bf_pal_front_port_cb_cfg_t *port_cfg);

static bf_board_map_t bd_to_asic_map;
static bf_map_t bf_board_map_db;

int bf_hw_cfg_bd_num_port_get(void) { return bd_to_asic_map.num_of_connectors; }

int bf_hw_cfg_bd_cfg_found_get(void) { return bd_to_asic_map.cfg_found; }

static void bf_hw_port_info_to_str(bf_pltfm_port_info_t *port_info,
                                   char *str_hdl,
                                   uint32_t str_len) {
  if (!str_hdl || !port_info) return;

  snprintf(str_hdl, str_len, "%d/%d", port_info->conn_id, port_info->chnl_id);

  return;
}

bf_conn_map_t *bf_pltfm_conn_map_find(uint32_t connector, uint32_t channel) {
  // if (connector >= (uint32_t)bf_hw_cfg_bd_num_port_get() ||
  //    (channel >= MAX_CHAN_PER_CONNECTORS)) {
  //  return NULL;
  //}

  bf_map_sts_t map_status;
  bf_conn_map_t *cmap = NULL;
  unsigned long cmap_key = BF_GET_CONN_MAP_KEY(connector, channel);

  map_status = bf_map_get(&bf_board_map_db, cmap_key, (void *)&cmap);
  if (map_status != BF_MAP_OK) {
    return NULL;
  }

  return cmap;
}

static void bf_pltfm_conn_map_remove(uint32_t connector, uint32_t channel) {
  unsigned long cmap_key = BF_GET_CONN_MAP_KEY(connector, channel);

  bf_map_rmv(&bf_board_map_db, cmap_key);
}

static bf_status_t bf_hw_cfg_get_port_is_an_eligible(
    bf_pal_front_port_handle_t *port_hdl, bool *is_an_eligible) {
  // Safety checks
  if ((!port_hdl) || (!is_an_eligible)) return BF_INVALID_ARG;

  bf_conn_map_t *cmap =
      bf_pltfm_conn_map_find(port_hdl->conn_id, port_hdl->chnl_id);

  if (NULL == cmap) {
    return BF_INVALID_ARG;
  }

  *is_an_eligible = cmap->enable_auto_neg;

  return BF_SUCCESS;
}

static bf_status_t bf_hw_cfg_get_port_ready_info(
    bf_pal_front_port_handle_t *port_hdl, bool *is_ready) {
  // Safety checks
  if ((!port_hdl) || (!is_ready)) return BF_INVALID_ARG;

  bf_conn_map_t *cmap =
      bf_pltfm_conn_map_find(port_hdl->conn_id, port_hdl->chnl_id);

  if (NULL == cmap) {
    return BF_INVALID_ARG;
  }

  *is_ready = cmap->mark_port_as_ready;

  return BF_SUCCESS;
}

// Get configured dev-id from cmap
static bf_status_t bf_hw_cfg_get_dev_id(bf_pal_front_port_handle_t *port_hdl,
                                        bf_dev_id_t *dev_id) {
  // Safety checks
  if ((!port_hdl) || (!dev_id)) return BF_INVALID_ARG;

  bf_conn_map_t *cmap =
      bf_pltfm_conn_map_find(port_hdl->conn_id, port_hdl->chnl_id);

  if (NULL == cmap) {
    return BF_INVALID_ARG;
  }

  *dev_id = cmap->device_id;

  return BF_SUCCESS;
}

static void bf_hw_send_asic_serdes_info(bf_pal_front_port_handle_t *port_hdl,
                                        bf_pal_front_port_cb_cfg_t *port_cfg) {
  uint32_t ch;
  bf_dev_id_t dev_id = 0;
  bf_pal_serdes_polarity_t asic_serdes_pol[MAX_CHAN_PER_CONNECTORS] = {{0}};
  bf_pal_serdes_tx_eq_params_t asic_serdes_tx_eq;

  if (!port_hdl || !port_cfg) return;

  bf_hw_cfg_get_dev_id(port_hdl, &dev_id);
  if (!bf_pm_intf_is_device_family_tofino2(dev_id)) return;

  // Note: SDK caches it and applies it only when the port-fsm is run
  for (ch = 0; ch < (uint32_t)port_cfg->num_lanes; ch++) {
    int cmap_lane = port_hdl->chnl_id + ch;
    bf_conn_map_t *cmap = bf_pltfm_conn_map_find(port_hdl->conn_id, cmap_lane);
    if (!cmap) {
      break;
    }
    serdes_lane_tx_eq_t *tx_eq =
        &cmap->lane[cmap_lane].tf2_tx_eq[port_cfg->enc_type];

    asic_serdes_pol[ch].rx_inv = (bool)cmap->lane[cmap_lane].rx_pn_swap;
    asic_serdes_pol[ch].tx_inv = (bool)cmap->lane[cmap_lane].tx_pn_swap;

    asic_serdes_tx_eq.tx_eq.tof2[ch].tx_pre1 = tx_eq->tx_pre1[0];
    asic_serdes_tx_eq.tx_eq.tof2[ch].tx_pre2 = tx_eq->tx_pre2[0];
    asic_serdes_tx_eq.tx_eq.tof2[ch].tx_main = tx_eq->tx_main[0];
    asic_serdes_tx_eq.tx_eq.tof2[ch].tx_post1 = tx_eq->tx_post1[0];
    asic_serdes_tx_eq.tx_eq.tof2[ch].tx_post2 = tx_eq->tx_post2[0];
  }

  bf_pm_port_serdes_polarity_set(
      dev_id, port_hdl, port_cfg->num_lanes, asic_serdes_pol);

  BF_BMAP_LOG_DEBUG(
      "Pushing the TX-EQ parameters dev %d : %d/%d encoding : %s \n",
      dev_id,
      port_hdl->conn_id,
      port_hdl->chnl_id,
      (port_cfg->enc_type == BF_ENCODING_PAM4) ? "PAM4" : "NRZ");

  bf_pm_port_serdes_tx_eq_params_set(
      dev_id, port_hdl, port_cfg->num_lanes, &asic_serdes_tx_eq);
}

static bf_status_t bf_hw_pre_port_enable_cfg_set(
    bf_pal_front_port_handle_t *port_hdl,
    bf_pal_front_port_cb_cfg_t *port_cfg) {
  bf_dev_id_t dev_id = 0;
  bool auto_neg = true;

  // Safety Checks
  if ((!port_hdl) || (!port_cfg)) return BF_INVALID_ARG;

  bf_hw_cfg_get_dev_id(port_hdl, &dev_id);
  if (bf_pm_intf_is_device_family_tofino(dev_id)) {
    return BF_SUCCESS;
  }

  // cross check with user configured
  if (port_cfg->is_an_on == PM_AN_DEFAULT) {
    bf_hw_cfg_get_port_is_an_eligible(port_hdl, &auto_neg);
  } else if (port_cfg->is_an_on == PM_AN_FORCE_ENABLE) {
    auto_neg = true;
  } else {
    auto_neg = false;
  }

  bf_hw_send_asic_serdes_info(port_hdl, port_cfg);
  bf_pm_pltfm_front_port_eligible_for_autoneg(dev_id, port_hdl, auto_neg);
  bf_pm_port_serdes_rx_ready_for_bringup_set(dev_id, port_hdl, true);
  bf_pm_pltfm_front_port_ready_for_bringup(dev_id, port_hdl, true);
  // valid only for tf3. For others, returns success
  bf_pm_port_multi_serdes_polarity_set(dev_id, port_hdl);
  return BF_SUCCESS;
}

// Use this to mark port as ready
static bf_status_t bf_hw_post_port_add_cfg_set(
    bf_pal_front_port_handle_t *port_hdl,
    bf_pal_front_port_cb_cfg_t *port_cfg) {
  bf_dev_id_t dev_id = 0;
  bf_status_t sts;

  // Safety Checks
  if ((!port_hdl) || (!port_cfg)) return BF_INVALID_ARG;

  sts = bf_hw_cfg_get_dev_id(port_hdl, &dev_id);
  if (sts != BF_SUCCESS) {
    BF_BMAP_LOG_ERROR("Unable to get dev_id for %d/%d (sts=%d)\n",
                      port_hdl->conn_id,
                      port_hdl->chnl_id,
                      sts);
    return sts;
  }

  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    return BF_SUCCESS;
  }

  // set configured media-type
  sts = bf_hw_cfg_media_type_set(port_hdl, port_cfg);
  if (sts != BF_SUCCESS) {
    BF_BMAP_LOG_ERROR(
        "Unable to set media-type for dev : %d : front port : "
        "%d/%d : %s (%d)\n",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        bf_err_str(sts),
        sts);

    return sts;
  }

  bool auto_neg;
  sts = bf_hw_cfg_get_port_is_an_eligible(port_hdl, &auto_neg);
  if (sts != BF_SUCCESS) {
    BF_BMAP_LOG_ERROR(
        "Unable to get auto-neg value for for dev : %d : front port : "
        "%d/%d : %s (%d)\n",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        bf_err_str(sts),
        sts);

    return sts;
  }

  sts = bf_pm_pltfm_front_port_eligible_for_autoneg(dev_id, port_hdl, auto_neg);
  if (sts != BF_SUCCESS) {
    BF_BMAP_LOG_ERROR(
        "Unable to set auto-neg for dev : %d : front port : "
        "%d/%d : %s (%d)\n",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        bf_err_str(sts),
        sts);

    return sts;
  }

  bool is_ready;
  sts = bf_hw_cfg_get_port_ready_info(port_hdl, &is_ready);
  if (sts != BF_SUCCESS) {
    BF_BMAP_LOG_ERROR(
        "Unable to get port ready for bringup for dev : %d : front port : "
        "%d/%d : %s (%d)\n",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        bf_err_str(sts),
        sts);

    return sts;
  }
  sts = bf_pm_pltfm_front_port_ready_for_bringup(dev_id, port_hdl, is_ready);
  if (sts != BF_SUCCESS) {
    BF_BMAP_LOG_ERROR(
        "Unable to mark port ready for bringup for dev : %d : front port : "
        "%d/%d : %s (%d)\n",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        bf_err_str(sts),
        sts);
    return sts;
  }

  // not used
  (void)port_cfg;
  return BF_SUCCESS;
}

bf_status_t bf_hw_cfg_port_mac_to_serdes_map_get(
    bf_pal_front_port_handle_t *port_hdl,
    bf_pal_mac_to_serdes_lane_map_t *mac_blk_map) {
  // Safety checks
  if (!port_hdl) return -1;
  if (!mac_blk_map) return -1;

  bf_conn_map_t *cmap =
      bf_pltfm_conn_map_find(port_hdl->conn_id, port_hdl->chnl_id);

  if (NULL == cmap) {
    return -1;
  }

  lane_map_t *lane_map = &cmap->lane[port_hdl->chnl_id];

  mac_blk_map->tx_lane = lane_map->tx_lane;
  mac_blk_map->rx_lane = lane_map->rx_lane;

  return 0;
}

bf_status_t bf_hw_cfg_port_serdes_info_get(bf_pal_front_port_handle_t *port_hdl,
                                           bf_pal_serdes_info_t *serdes_info) {
  if (NULL == port_hdl || NULL == serdes_info) {
    return BF_INVALID_ARG;
  }

  bf_conn_map_t *cmap =
      bf_pltfm_conn_map_find(port_hdl->conn_id, port_hdl->chnl_id);

  if (NULL == cmap) {
    return BF_INVALID_ARG;
  }

  lane_map_t *lane_map = &cmap->lane[port_hdl->chnl_id];

  serdes_info->tx_inv = (lane_map->tx_pn_swap ? true : false);
  serdes_info->rx_inv = (lane_map->rx_pn_swap ? true : false);
  serdes_info->tx_attn = lane_map->tx_eq_attn;
  serdes_info->tx_pre = lane_map->tx_eq_pre;
  serdes_info->tx_post = lane_map->tx_eq_post;

  return 0;
}

bf_status_t bf_hw_cfg_port_mac_get(bf_pltfm_port_info_t *port_info,
                                   uint32_t *mac_id,
                                   uint32_t *mac_chnl_id) {
  if ((!port_info) || (!mac_id) || (!mac_chnl_id)) {
    return BF_INVALID_ARG;
  }

  bf_conn_map_t *cmap =
      bf_pltfm_conn_map_find(port_info->conn_id, port_info->chnl_id);

  if (NULL == cmap) {
    return BF_OBJECT_NOT_FOUND;
  }

  *mac_id = cmap->mac_block;

  lane_map_t *lane_map = &cmap->lane[port_info->chnl_id];
  *mac_chnl_id = lane_map->mac_ch;

  return BF_SUCCESS;
}

bf_status_t bf_hw_cfg_front_panel_port_get_first(
    bf_pal_front_port_info_t *first_port_info) {
  bf_status_t sts = 0;
  bf_map_sts_t map_sts;
  bf_pltfm_port_info_t port_info;
  bf_conn_map_t *cmap = NULL;
  unsigned long key;
  uint32_t log_mac_id, log_mac_lane;

  map_sts = bf_map_get_first(&bf_board_map_db, &key, (void **)&cmap);
  if (map_sts == BF_MAP_OK) {
    port_info.conn_id = BF_GET_CONN_FROM_KEY(key);
    port_info.chnl_id = BF_GET_CHNL_FROM_KEY(key);

    /* this is required to handle the case where the connector is not
     * a 4-channel (QSFP) module (e.g. SFP) because bf_pm is going to
     * use "log_mac_lane" to index into the lane map for a given
     * connector.
     */
    sts = bf_hw_cfg_port_mac_get(&port_info, &log_mac_id, &log_mac_lane);
    if (sts != BF_SUCCESS) {
      BF_BMAP_LOG_ERROR(
          "Unable to get the mac id and chnl for front port %d/%d : (%d)\n",
          port_info.conn_id,
          port_info.chnl_id,
          sts);
      return BF_OBJECT_NOT_FOUND;
    }

    first_port_info->dev_id = cmap->device_id;
    first_port_info->port_hdl.conn_id = port_info.conn_id;
    first_port_info->port_hdl.chnl_id = port_info.chnl_id;
    first_port_info->log_mac_id = log_mac_id;
    first_port_info->log_mac_lane = log_mac_lane;
    bf_hw_port_info_to_str(&port_info,
                           first_port_info->port_str,
                           sizeof(first_port_info->port_str));
    return BF_SUCCESS;
  }
  return BF_OBJECT_NOT_FOUND;
}

bf_status_t bf_hw_cfg_front_panel_port_get_next(
    bf_pal_front_port_info_t *curr_port_info,
    bf_pal_front_port_info_t *next_port_info) {
  bf_status_t sts = 0;
  bf_map_sts_t map_sts;
  bf_pltfm_port_info_t port_info;
  bf_conn_map_t *cmap = NULL;
  uint32_t log_mac_id, log_mac_lane;
  unsigned long key = BF_GET_CONN_MAP_KEY(curr_port_info->port_hdl.conn_id,
                                          curr_port_info->port_hdl.chnl_id);

  // start from given dev/port
  map_sts = bf_map_get_next(&bf_board_map_db, &key, (void **)&cmap);
  if (map_sts == BF_MAP_OK) {
    port_info.conn_id = BF_GET_CONN_FROM_KEY(key);
    port_info.chnl_id = BF_GET_CHNL_FROM_KEY(key);

    /* this is required to handle the case where the connector is not
     * a 4-channel (QSFP) module (e.g. SFP) because bf_pm is going to
     * use "log_mac_lane" to index into the lane map for a given
     * connector.
     */
    sts = bf_hw_cfg_port_mac_get(&port_info, &log_mac_id, &log_mac_lane);
    if (sts != BF_SUCCESS) {
      BF_BMAP_LOG_ERROR(
          "Unable to get the mac id and chnl for front port %d/%d : (%d)\n",
          port_info.conn_id,
          port_info.chnl_id,
          sts);
      return BF_OBJECT_NOT_FOUND;
    }

    next_port_info->dev_id = cmap->device_id;
    next_port_info->port_hdl.conn_id = port_info.conn_id;
    next_port_info->port_hdl.chnl_id = port_info.chnl_id;
    next_port_info->log_mac_id = log_mac_id;
    next_port_info->log_mac_lane = log_mac_lane;
    bf_hw_port_info_to_str(
        &port_info, next_port_info->port_str, sizeof(next_port_info->port_str));
    return BF_SUCCESS;
  }
  return BF_OBJECT_NOT_FOUND;
}

static bf_status_t bf_hw_media_type_get(bf_pal_front_port_handle_t *port_hdl,
                                        bf_media_type_t *media_type) {
  // Safety checks
  if (!port_hdl) return BF_INVALID_ARG;
  if (!media_type) return BF_INVALID_ARG;

  bf_conn_map_t *cmap =
      bf_pltfm_conn_map_find(port_hdl->conn_id, port_hdl->chnl_id);

  if (NULL == cmap) {
    return BF_INVALID_ARG;
  }

  *media_type = cmap->media_type;

  return BF_SUCCESS;
}

static bf_status_t bf_hw_cfg_media_type_set(
    bf_pal_front_port_handle_t *port_hdl,
    bf_pal_front_port_cb_cfg_t *port_cfg) {
  bf_status_t sts = BF_SUCCESS;
  bf_pltfm_port_info_t port_info;
  bf_dev_id_t dev_id = 0;
  bf_dev_id_t dev_id_of_port = 0;
  bf_dev_port_t dev_port;
  bf_media_type_t media_type = BF_MEDIA_TYPE_UNKNOWN;

  // Safety Checks
  if (!port_hdl) return BF_INVALID_ARG;

  port_info.conn_id = port_hdl->conn_id;
  port_info.chnl_id = port_hdl->chnl_id;

  sts = bf_hw_cfg_get_dev_id(port_hdl, &dev_id);
  if (sts != BF_SUCCESS) {
    BF_BMAP_LOG_ERROR(
        "%s: Unable to get the dev-id for front port : %d/%d : %s\n",
        __func__,
        port_info.conn_id,
        port_info.chnl_id,
        bf_err_str(sts));
    return sts;
  }

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);
  dev_id = dev_id_of_port;

  if (sts != BF_SUCCESS) {
    BF_BMAP_LOG_ERROR(
        "%s: Unable to get the dev port for dev : %d : front port : %d/%d : "
        "%s\n",
        __func__,
        dev_id,
        port_info.conn_id,
        port_info.chnl_id,
        bf_err_str(sts));
    return sts;
  }

  media_type = bf_hw_media_type_get(port_hdl, &media_type);

  // User has not defined it
  if (media_type == BF_MEDIA_TYPE_UNKNOWN) {
    return BF_SUCCESS;
  }

  if (media_type == BF_MEDIA_TYPE_OPTICAL) {
    sts = bf_port_is_optical_xcvr_set(dev_id, dev_port, true);
  } else {
    sts = bf_port_is_optical_xcvr_set(dev_id, dev_port, false);
  }

  (void)port_cfg;
  return sts;
}

static bf_status_t bf_hw_cfg_port_nlanes_per_ch_get(
    bf_pal_front_port_handle_t *port_hdl, uint32_t *num_lanes) {
  if ((NULL == port_hdl) || (!num_lanes)) {
    return BF_INVALID_ARG;
  }

  bf_conn_map_t *cmap =
      bf_pltfm_conn_map_find(port_hdl->conn_id, port_hdl->chnl_id);

  if (NULL == cmap) {
    return BF_INVALID_ARG;
  }

  if (cmap->nlanes_per_ch > 1) {
    *num_lanes = cmap->nlanes_per_ch;
  } else {
    *num_lanes = 1;  // default
  }

  return BF_SUCCESS;
}

bf_status_t bf_hw_cfg_port_phy_multilane_get(
    bf_pal_front_port_handle_t *port_hdl,
    bf_pal_mac_to_multi_serdes_lane_map_t *mac_blk_map) {
  // Safety checks
  if (!port_hdl) return -1;
  if (!mac_blk_map) return -1;

  bf_conn_map_t *cmap =
      bf_pltfm_conn_map_find(port_hdl->conn_id, port_hdl->chnl_id);
  if (NULL == cmap) {
    return -1;
  }

  lane_map_t *lane_map;
  uint32_t ch = port_hdl->chnl_id;

  if (cmap->nlanes_per_ch > 1) {
    for (uint idx = 0; idx < cmap->nlanes_per_ch; idx++) {
      lane_map = &cmap->lane[idx + ch];
      mac_blk_map->tx_lane[idx] = lane_map->tx_lane;
      mac_blk_map->rx_lane[idx] = lane_map->rx_lane;
    }
  } else {
    lane_map = &cmap->lane[port_hdl->chnl_id];
    mac_blk_map->tx_lane[0] = lane_map->tx_lane;
    mac_blk_map->rx_lane[0] = lane_map->rx_lane;
  }

  return 0;
}

bf_status_t bf_hw_port_serdes_polarity_get(
    bf_pal_front_port_handle_t *port_hdl,
    bf_pal_mac_to_multi_serdes_lane_map_t *mac_blk_map) {
  // Safety checks
  if (!port_hdl) return -1;
  if (!mac_blk_map) return -1;
  bf_conn_map_t *cmap =
      bf_pltfm_conn_map_find(port_hdl->conn_id, port_hdl->chnl_id);
  if (NULL == cmap) {
    return -1;
  }

  lane_map_t *lane_map;
  uint32_t ch = port_hdl->chnl_id;
  if (cmap->nlanes_per_ch > 1) {
    for (uint idx = 0; idx < cmap->nlanes_per_ch; idx++) {
      lane_map = &cmap->lane[idx + ch];
      mac_blk_map->tx_inv[idx] = lane_map->tx_pn_swap;
      mac_blk_map->rx_inv[idx] = lane_map->rx_pn_swap;
    }
  } else {
    lane_map = &cmap->lane[port_hdl->chnl_id];
    mac_blk_map->tx_inv[0] = lane_map->tx_pn_swap;
    mac_blk_map->rx_inv[0] = lane_map->rx_pn_swap;
  }

  return 0;
}

static bf_status_t bf_hw_cfg_mac_to_multi_serdes_map_get(
    bf_pal_front_port_handle_t *port_hdl,
    bf_pal_mac_to_multi_serdes_lane_map_t *mac_blk_map) {
  bf_status_t sts;

  // Safety checks
  if (!port_hdl) return BF_INVALID_ARG;
  if (!mac_blk_map) return BF_INVALID_ARG;
  if (port_hdl->chnl_id > 7) {
    BF_BMAP_LOG_ERROR(
        "Error getting serdes lane map: Invalid channel for front port %d/%d",
        port_hdl->conn_id,
        port_hdl->chnl_id);
    return BF_INVALID_ARG;
  }

  sts = bf_hw_cfg_port_nlanes_per_ch_get(port_hdl,
                                         &mac_blk_map->num_serdes_per_lane);
  if (sts != BF_SUCCESS) {
    BF_BMAP_LOG_ERROR(
        "Unable to get the num-serdes lane connected to front port %d/%d : "
        "Error(%d)",
        port_hdl->conn_id,
        port_hdl->chnl_id,
        sts);
    return sts;
  }

  sts = bf_hw_cfg_port_phy_multilane_get(port_hdl, mac_blk_map);
  if (sts != BF_SUCCESS) {
    BF_BMAP_LOG_ERROR(
        "Unable to get the serdes Tx lane connected to front port %d/%d : "
        "Error(%d)",
        port_hdl->conn_id,
        port_hdl->chnl_id,
        sts);
    return sts;
  }

  sts = bf_hw_port_serdes_polarity_get(port_hdl, mac_blk_map);
  if (sts != BF_SUCCESS) {
    BF_BMAP_LOG_ERROR(
        "Unable to get the serdes Rx pol connected to front port %d/%d : "
        "Error(%d)",
        port_hdl->conn_id,
        port_hdl->chnl_id,
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

static bf_status_t bf_hw_init(bf_dev_init_mode_t warm_init_mode) {
  (void)warm_init_mode;

  return BF_SUCCESS;
}

static bf_status_t bf_hw_safe_to_call_in_notify(void) { return BF_SUCCESS; }

static bf_status_t bf_hw_porting_register_pltm_vectors(bool is_hardware) {
  bf_pal_pltfm_reg_interface_t bf_pm_interface = {0};
  bf_status_t sts;

  bf_pm_interface.pltfm_safe_to_call_in_notify = &bf_hw_safe_to_call_in_notify;
  bf_pm_interface.pltfm_front_panel_port_get_first =
      &bf_hw_cfg_front_panel_port_get_first;
  bf_pm_interface.pltfm_front_panel_port_get_next =
      &bf_hw_cfg_front_panel_port_get_next;

  bf_pm_interface.pltfm_mac_to_serdes_map_get =
      &bf_hw_cfg_port_mac_to_serdes_map_get;
  bf_pm_interface.pltfm_serdes_info_get = &bf_hw_cfg_port_serdes_info_get;
  bf_pm_interface.pltfm_post_port_add_cfg_set = &bf_hw_post_port_add_cfg_set;
  bf_pm_interface.pltfm_pre_port_enable_cfg_set =
      &bf_hw_pre_port_enable_cfg_set;
  bf_pm_interface.pltfm_init = &bf_hw_init;
  bf_pm_interface.pltfm_deinit = 0;
  bf_pm_interface.pltfm_mac_to_multi_serdes_map_get =
      bf_hw_cfg_mac_to_multi_serdes_map_get;

  if (!is_hardware) {
    bf_pm_interface.pltfm_port_media_type_get = &bf_model_media_type_get;
  } else {
    bf_pm_interface.pltfm_port_media_type_get = &bf_hw_media_type_get;
  }
  bf_pm_interface.pltfm_port_str_to_hdl_get = &bf_model_port_str_to_hdl_get;

  sts = bf_pal_pltfm_all_interface_set(&bf_pm_interface);
  if (sts != BF_SUCCESS) {
    BF_BMAP_LOG_ERROR("%s Unable to register pltfm interfaces for bf_pm\n",
                      __func__);
  }

  return sts;
}

/* clear bd_map */
void bf_hw_clear_bd_map(void) {
  bf_conn_map_t *cmap;

  cmap = bd_to_asic_map.cmap;
  if (cmap) {
    free(cmap);
  }
  bd_to_asic_map.cmap = NULL;
  bd_to_asic_map.num_of_connectors = 0;
}

bf_status_t bf_hw_cfg_get_qsfp_ch(bf_pal_front_port_handle_t *port_hdl,
                                  uint *qsfp_ch) {
  if (NULL == port_hdl || NULL == qsfp_ch) {
    return BF_INVALID_ARG;
  }

  bf_conn_map_t *cmap =
      bf_pltfm_conn_map_find(port_hdl->conn_id, port_hdl->chnl_id);

  if (NULL == cmap) {
    return BF_INVALID_ARG;
  }

  lane_map_t *lmap = &cmap->lane[port_hdl->chnl_id];
  *qsfp_ch = lmap->channel;
  return BF_SUCCESS;
}

bf_status_t bf_hw_cfg_get_pin_name(bf_pal_front_port_handle_t *port_hdl,
                                   char *pin_name) {
  if (NULL == port_hdl || NULL == pin_name) {
    return BF_INVALID_ARG;
  }

  bf_conn_map_t *cmap =
      bf_pltfm_conn_map_find(port_hdl->conn_id, port_hdl->chnl_id);

  if (NULL == cmap) {
    return BF_INVALID_ARG;
  }

  strcpy(pin_name, cmap->pack_pin_name);
  return BF_SUCCESS;
}

static void compress_tf3_board_map(uint8_t *ports_per_macblk) {
  uint32_t c, i;

  if (!ports_per_macblk) return;

  for (i = 1; i < bd_to_asic_map.num_of_connectors + 1; i++) {
    for (c = 0; c < 8; c++) {
      bf_conn_map_t *src = bf_pltfm_conn_map_find(i, c);
      if (src == NULL) continue;
      if (src->mac_block == 0) {
        // For CPU port, special handling
        src->nlanes_per_ch = 2;
      } else if (ports_per_macblk[src->mac_block] == 4) {
        src->nlanes_per_ch = 1;
      } else {
        src->nlanes_per_ch = 2;
      }

      if (src->nlanes_per_ch == 2) {
        uint8_t valid_chnl[8] = {1, 0, 1, 0, 1, 0, 1, 0};
        uint8_t other_chnl[8] = {1, 0, 3, 2, 5, 4, 7, 6};
        int mac_ch_idx[8] = {0, 0, 1, 1, 2, 2, 3, 3};

        if (!valid_chnl[c]) continue;

        lane_map_t *src_lmap_1 = &src->lane[c + 0];
        if (src_lmap_1) src_lmap_1->mac_ch = mac_ch_idx[c + 0];

        lane_map_t *src_lmap_2 = &src->lane[c + 1];
        if (src_lmap_2) src_lmap_2->mac_ch = mac_ch_idx[c + 1];

        src->lane_arr[0] = c;
        src->lane_arr[1] = other_chnl[c];
        bf_pltfm_conn_map_remove(i, other_chnl[c]);
      }
    }
  }
}

static int bf_hw_parse_tf3_conf_file(int *bd_config_found,
                                     bool *is_hardware,
                                     char *config_buffer) {
  int rows;
  cJSON *root = NULL, *bd_map_ent_all = NULL, *bd_map_ent;
  bf_conn_map_t *cmap = NULL;
  int err = 0;
  char *param = "Unknown";
  uint8_t ports_per_macblk[67] = {0};

  assert(NULL != config_buffer);

  root = cJSON_Parse(config_buffer);
  if (root == NULL) {
    BF_BMAP_LOG_ERROR("cJSON parsing error before %s\n", cJSON_GetErrorPtr());
    goto handle_error;
  }

  *bd_config_found = 1;
  bd_to_asic_map.cfg_found = 1;
  *is_hardware = 1;

  int bd_id = 0xFF;
  err = bf_cjson_get_string(root, "board_id", &param);
  if (!err) {
    bd_id = atoi(param);
    BF_BMAP_LOG_DEBUG("Board-id 0x%0x\n", bd_id);
  }

  int en_log = 0;  // default disable
  err = bf_cjson_get_string(root, "enable_debug_log", &param);
  if (!err) {
    en_log = atoi(param);
    BF_BMAP_LOG_DEBUG("enable-log %d \n", en_log);
  }

  bd_map_ent_all = cJSON_GetObjectItem(root, "board_lane_map_entry");
  if (bd_map_ent_all == NULL) {
    BF_BMAP_LOG_ERROR("cJSON parsing error locating board_lane_map_entry\n");
    goto handle_error;
  }
  rows = cJSON_GetArraySize(bd_map_ent_all);
  if (rows == 0) {
    BF_BMAP_LOG_ERROR("cJSON parsing error empty board_lane_map_entry\n");
    goto handle_error;
  }
  /* allocate space to hold all bd_map_entry(s) */
  cmap = calloc(sizeof(bf_conn_map_t) * rows, 1);
  if (cmap == NULL) {
    BF_BMAP_LOG_ERROR("Could not allocate memory for bd_map_ent\n");
    goto handle_error;
  }
  bd_to_asic_map.cmap = cmap;

  bf_map_sts_t map_status = bf_map_init(&bf_board_map_db);
  if (map_status != BF_MAP_OK) {
    BF_BMAP_LOG_ERROR("%s error init the map\n", __func__);
    goto handle_error;
  }

  /* parse the json object and populate bd_map_ent */
  int row_number = 0;
  int nconnectors = 1;
  int prev_conn = 0;
  int mac_block = 0;
  int logical_ch = 0;
  cJSON_ArrayForEach(bd_map_ent, bd_map_ent_all) {
    int conn, channel, tx_lane, rx_lane;
    bool rx_pn_swap, tx_pn_swap;
    char *pin_name = "";
    int device_id = 0;
    int is_internal_port;

    err = tx_lane = rx_lane = is_internal_port = 0;

    cJSON *devidobj = cJSON_GetObjectItem(bd_map_ent, "device_id");
    if (devidobj) {
      err = bf_cjson_get_int(bd_map_ent, "device_id", &device_id);
    }
    BF_BMAP_LOG_INFO(en_log, "device_id %d \n", device_id);
    if (device_id >= BF_MAX_DEV_COUNT) {
      BF_BMAP_LOG_ERROR("Error %s Device-id %d exceeds max count allowed\n",
                        __func__,
                        device_id);
      err = -1;
      goto handle_error;
    }
    bd_to_asic_map.device_id_cfged[device_id] = 1;

    err = bf_cjson_get_string(bd_map_ent, "Package_Pin_Name", &pin_name);
    if (!strcmp(pin_name, "")) {
      continue;
    }

    err |= bf_cjson_get_string(bd_map_ent, "QSFPDD_Port", &param);
    conn = atoi(param);
    BF_BMAP_LOG_INFO(en_log, "connector %d \n", conn);
    if (conn != prev_conn) {
      nconnectors++;
      logical_ch = 0;
      cmap++;
    }
    prev_conn = conn;

    err = bf_cjson_get_string(bd_map_ent, "Internal_port", &param);
    if (!strcmp(param, "Yes")) {
      is_internal_port = 1;
    }
    BF_BMAP_LOG_INFO(
        en_log, "Internal_port:%s val:%d \n", param, is_internal_port);

    err |= bf_cjson_get_string(bd_map_ent, "QSFPDD_Lane", &param);
    channel = atoi(param);
    BF_BMAP_LOG_INFO(en_log, "channel:%d \n", channel);
    if (channel > MAX_CHAN_PER_CONNECTORS || channel < 0) {
      err = -1;
      goto handle_error;
    }

    rx_pn_swap = 0;
    err = bf_cjson_get_string(bd_map_ent, "RX_PN_Swap", &param);
    if (!strcmp(param, "Y")) {
      rx_pn_swap = 1;
    }
    BF_BMAP_LOG_INFO(en_log, "RX_PN_Swap:%s swap:%d \n", param, rx_pn_swap);

    tx_pn_swap = 0;
    err = bf_cjson_get_string(bd_map_ent, "TX_PN_Swap", &param);
    if (!strcmp(param, "Y")) {
      tx_pn_swap = 1;
    }
    BF_BMAP_LOG_INFO(en_log, "TX_PN_Swap:%s swap:%d \n", param, tx_pn_swap);

    err |= bf_cjson_get_string(bd_map_ent, "PCB_RX_Lane", &param);
    rx_lane = atoi(param);
    BF_BMAP_LOG_INFO(en_log, "SD_RX_Lane:%d \n", rx_lane);

    err |= bf_cjson_get_string(bd_map_ent, "PCB_TX_Lane", &param);
    tx_lane = atoi(param);
    BF_BMAP_LOG_INFO(en_log, "SD_TX_Lane:%d \n", tx_lane);
    // Do some sanity
    if ((tx_lane > 7) || (rx_lane > 7)) {
      BF_BMAP_LOG_ERROR("Rx:%d or tx:%d lane  not valid. Must be 0 to 7 \n",
                        rx_lane,
                        tx_lane);
      assert(0);
    }

    err |= bf_cjson_get_string(bd_map_ent, "Mac_number", &param);
    mac_block = atoi(param);
    BF_BMAP_LOG_INFO(en_log, "mac_block:%d \n", mac_block);

    if (err) {
      goto handle_error;
    }

    if (mac_block != 0) {
      channel--;
    }

    // Populate
    strcpy(cmap->pack_pin_name, pin_name);
    cmap->connector = conn;
    cmap->chip_type = BF_CHIP_TYPE_TF3;
    cmap->device_id = device_id;
    cmap->mac_block = mac_block;
    ports_per_macblk[mac_block]++;
    cmap->is_internal_port = is_internal_port;
    if (is_internal_port) {
      cmap->enable_auto_neg = 1;
    }

    lane_map_t *lmap = &cmap->lane[logical_ch];
    lmap->channel = channel;
    lmap->mac_ch = logical_ch;
    lmap->rx_lane = rx_lane;
    lmap->rx_pn_swap = rx_pn_swap;
    lmap->tx_lane = tx_lane;
    lmap->tx_pn_swap = tx_pn_swap;

    unsigned long cmap_key = BF_GET_CONN_MAP_KEY(conn, logical_ch);
    BF_BMAP_LOG_INFO(en_log,
                     "map-add dev/port/key : %d : %d/%d : %08x\n",
                     device_id,
                     conn,
                     logical_ch,
                     (unsigned int)cmap_key);

    map_status = bf_map_add(&bf_board_map_db, cmap_key, (void *)cmap);
    if (map_status != BF_MAP_OK) {
      BF_BMAP_LOG_INFO(
          1,
          "Error: map-add dev/port : %d : %d/%d : map-error : %d\n",
          device_id,
          conn,
          channel,
          map_status);
      err = -1;
      goto handle_error;
    }

    row_number++;
    logical_ch++;
  }
  nconnectors--;  // Index 1 to last port :33
  BF_BMAP_LOG_DEBUG("Num of connectors : %d\n", nconnectors);
  bd_to_asic_map.num_of_connectors = nconnectors;
  bd_to_asic_map.cfg_parsed_ok = 1;
  err = 0;
  compress_tf3_board_map(ports_per_macblk);

handle_error:
  if (root) {
    cJSON_Delete(root);
  }
  if (err) {
    BF_BMAP_LOG_ERROR("parsing error\n");
    bf_hw_clear_bd_map();
  }

  return err;
}

static int bf_hw_parse_tf2_conf_file(int *bd_config_found,
                                     bool *is_hardware,
                                     char *config_buffer) {
  int rows;
  cJSON *root = NULL, *bd_map_ent_all = NULL, *bd_map_ent;
  bf_conn_map_t *cmap = NULL;
  int err = 0;
  char *param = "Unknown";

  assert(NULL != config_buffer);

  root = cJSON_Parse(config_buffer);
  if (root == NULL) {
    BF_BMAP_LOG_ERROR("cJSON parsing error before %s\n", cJSON_GetErrorPtr());
    goto handle_error;
  }

  *bd_config_found = 1;
  bd_to_asic_map.cfg_found = 1;
  *is_hardware = 1;

  int bd_id = 0xFF;
  err = bf_cjson_get_string(root, "board_id", &param);
  if (!err) {
    bd_id = atoi(param);
    BF_BMAP_LOG_DEBUG("Board-id 0x%0x\n", bd_id);
  }

  int en_log = 0;  // default disable
  err = bf_cjson_get_string(root, "enable_debug_log", &param);
  if (!err) {
    en_log = atoi(param);
    BF_BMAP_LOG_DEBUG("enable-log %d \n", en_log);
  }

  bd_map_ent_all = cJSON_GetObjectItem(root, "board_lane_map_entry");
  if (bd_map_ent_all == NULL) {
    BF_BMAP_LOG_ERROR("cJSON parsing error locating board_lane_map_entry\n");
    goto handle_error;
  }
  rows = cJSON_GetArraySize(bd_map_ent_all);
  if (rows == 0) {
    BF_BMAP_LOG_ERROR("cJSON parsing error empty board_lane_map_entry\n");
    goto handle_error;
  }
  /* allocate space to hold all bd_map_entry(s) */
  cmap = calloc(sizeof(bf_conn_map_t) * rows, 1);
  if (cmap == NULL) {
    BF_BMAP_LOG_ERROR("Could not allocate memory for bd_map_ent\n");
    goto handle_error;
  }
  bd_to_asic_map.cmap = cmap;

  bf_map_sts_t map_status = bf_map_init(&bf_board_map_db);
  if (map_status != BF_MAP_OK) {
    BF_BMAP_LOG_ERROR("%s error init the map\n", __func__);
    goto handle_error;
  }

  /* parse the json object and populate bd_map_ent */
  int row_number = 0;
  char *tx_dict[] = {"TX_MAIN", "TX_POST1", "TX_POST2", "TX_Pre1", "TX_pre2"};
  char *module[] = {"_Cu_0.5m"};
  int qsfp_dd_idx[] = {0};

  char *encoding[] = {"_NRZ", "_PAM4"};
  char tx_string[80];
  int nconnectors = 1;
  int prev_conn = 0;
  int mac_block = 0;
  cJSON_ArrayForEach(bd_map_ent, bd_map_ent_all) {
    int conn, channel, tx_lane, rx_lane;
    bool rx_pn_swap, tx_pn_swap;
    bf_dev_id_t device_id = 0;
    int is_internal_port;

    err = tx_lane = rx_lane = is_internal_port = 0;

    // Optional
    device_id = 0;
    cJSON *devidobj = cJSON_GetObjectItem(bd_map_ent, "device_id");
    if (devidobj) {
      err = bf_cjson_get_int(bd_map_ent, "device_id", &device_id);
    }
    BF_BMAP_LOG_INFO(en_log, "device_id %d \n", device_id);
    if (device_id >= BF_MAX_DEV_COUNT) {
      BF_BMAP_LOG_ERROR("Error %s Device-id %d exceeds max count allowed\n",
                        __func__,
                        device_id);
      err = -1;
      goto handle_error;
    }
    bd_to_asic_map.device_id_cfged[device_id] = 1;

    err |= bf_cjson_get_string(bd_map_ent, "QSFPDD_Port", &param);
    conn = atoi(param);
    BF_BMAP_LOG_INFO(en_log, "connector %d \n", conn);
    if (conn != prev_conn) {
      nconnectors++;
    }
    prev_conn = conn;

    err = bf_cjson_get_string(bd_map_ent, "Internal_port", &param);
    if (!strcmp(param, "Yes")) {
      is_internal_port = 1;
    }
    BF_BMAP_LOG_INFO(
        en_log, "Internal_port:%s val:%d \n", param, is_internal_port);

    err = bf_cjson_get_string(bd_map_ent, "JBAY Package Net Name", &param);
    mac_block = atoi(param);
    BF_BMAP_LOG_INFO(en_log, "mac_block:%d \n", mac_block);

    err |= bf_cjson_get_string(bd_map_ent, "QSFPDD_Lane", &param);
    channel = atoi(param);
    BF_BMAP_LOG_INFO(en_log, "channel:%d \n", channel);
    if (channel >= MAX_CHAN_PER_CONNECTORS || channel < 0) {
      err = -1;
      goto handle_error;
    }

    rx_pn_swap = 0;
    err = bf_cjson_get_string(bd_map_ent, "RX_PN_Swap", &param);
    if (!strcmp(param, "Y")) {
      rx_pn_swap = 1;
    }
    BF_BMAP_LOG_INFO(en_log, "RX_PN_Swap:%s swap:%d \n", param, rx_pn_swap);

    tx_pn_swap = 0;
    err = bf_cjson_get_string(bd_map_ent, "TX_PN_Swap", &param);
    if (!strcmp(param, "Y")) {
      tx_pn_swap = 1;
    }
    BF_BMAP_LOG_INFO(en_log, "TX_PN_Swap:%s swap:%d \n", param, tx_pn_swap);

    err |= bf_cjson_get_string(bd_map_ent, "PCB_RX_Lane", &param);
    rx_lane = atoi(param);
    BF_BMAP_LOG_INFO(en_log, "SD_RX_Lane:%d \n", rx_lane);

    err |= bf_cjson_get_string(bd_map_ent, "PCB_TX_Lane", &param);
    tx_lane = atoi(param);
    BF_BMAP_LOG_INFO(en_log, "SD_TX_Lane:%d \n", tx_lane);
    // Do some sanity
    if ((tx_lane > 7) || (rx_lane > 7)) {
      BF_BMAP_LOG_ERROR("Rx:%d or tx:%d lane  not valid. Must be 0 to 7 \n",
                        rx_lane,
                        tx_lane);
      assert(0);
    }

    if (err) {
      goto handle_error;
    }
    // Populate
    cmap->connector = conn;
    cmap->chip_type = BF_CHIP_TYPE_TF2;
    cmap->device_id = device_id;
    cmap->mac_block = mac_block;
    cmap->nlanes_per_ch = 1;
    cmap->is_internal_port = is_internal_port;
    if (is_internal_port) {
      cmap->enable_auto_neg = 1;
    }

    lane_map_t *lmap = &cmap->lane[channel];
    lmap->channel = channel;
    lmap->mac_ch = channel;
    lmap->rx_lane = rx_lane;
    lmap->rx_pn_swap = rx_pn_swap;
    lmap->tx_lane = tx_lane;
    lmap->tx_pn_swap = tx_pn_swap;

    int enc = 0;
    int i = 0;
    int tx_main, tx_post1, tx_post2, tx_pre1, tx_pre2;
    for (enc = 0; enc < 2; enc++) {
      serdes_lane_tx_eq_t *tx_ptr = &cmap->lane[channel].tf2_tx_eq[enc];
      for (i = 0; i < 1; i++) {
        int k = 0;
        int qidx = 0;
        qidx = qsfp_dd_idx[i];
        // BF_BMAP_LOG_INFO(en_log, "module: %s qidx: %d\n", module[i], qidx);
        memset(tx_string, 0, sizeof(tx_string));
        snprintf(tx_string,
                 sizeof(tx_string),
                 "%s%s%s",
                 tx_dict[k++],
                 module[i],
                 encoding[enc]);
        err |= bf_cjson_get_string(bd_map_ent, tx_string, &param);
        tx_main = atoi(param);
        BF_BMAP_LOG_INFO(en_log, "%s val: %d\n", tx_string, tx_main);
        tx_ptr->tx_main[qidx] = tx_main;

        memset(tx_string, 0, sizeof(tx_string));
        snprintf(tx_string,
                 sizeof(tx_string),
                 "%s%s%s",
                 tx_dict[k++],
                 module[i],
                 encoding[enc]);
        err |= bf_cjson_get_string(bd_map_ent, tx_string, &param);
        tx_post1 = atoi(param);
        BF_BMAP_LOG_INFO(en_log, "%s val: %d\n", tx_string, tx_post1);
        tx_ptr->tx_post1[qidx] = tx_post1;

        memset(tx_string, 0, sizeof(tx_string));
        snprintf(tx_string,
                 sizeof(tx_string),
                 "%s%s%s",
                 tx_dict[k++],
                 module[i],
                 encoding[enc]);
        err |= bf_cjson_get_string(bd_map_ent, tx_string, &param);
        tx_post2 = atoi(param);
        BF_BMAP_LOG_INFO(en_log, "%s val: %d\n", tx_string, tx_post2);
        tx_ptr->tx_post2[qidx] = tx_post2;

        memset(tx_string, 0, sizeof(tx_string));
        snprintf(tx_string,
                 sizeof(tx_string),
                 "%s%s%s",
                 tx_dict[k++],
                 module[i],
                 encoding[enc]);
        err |= bf_cjson_get_string(bd_map_ent, tx_string, &param);
        tx_pre1 = atoi(param);
        BF_BMAP_LOG_INFO(en_log, "%s val: %d\n", tx_string, tx_pre1);
        tx_ptr->tx_pre1[qidx] = tx_pre1;

        memset(tx_string, 0, sizeof(tx_string));
        snprintf(tx_string,
                 sizeof(tx_string),
                 "%s%s%s",
                 tx_dict[k++],
                 module[i],
                 encoding[enc]);
        err |= bf_cjson_get_string(bd_map_ent, tx_string, &param);
        tx_pre2 = atoi(param);
        BF_BMAP_LOG_INFO(en_log, "%s val: %d\n", tx_string, tx_pre2);
        tx_ptr->tx_pre2[qidx] = tx_pre2;

        if (err) {
          goto handle_error;
        }
      }
    }
    unsigned long cmap_key = BF_GET_CONN_MAP_KEY(conn, channel);
    BF_BMAP_LOG_INFO(en_log,
                     "map-add dev/port/key : %d : %d/%d : %08x\n",
                     device_id,
                     conn,
                     channel,
                     (unsigned int)cmap_key);

    map_status = bf_map_add(&bf_board_map_db, cmap_key, (void *)cmap);
    if (map_status != BF_MAP_OK) {
      BF_BMAP_LOG_INFO(
          1,
          "Error: map-add dev/port : %d : %d/%d : map-error : %d\n",
          device_id,
          conn,
          channel,
          map_status);
      err = -1;
      goto handle_error;
    }

    row_number++;
    cmap++;
  }
  nconnectors--;  // Index 1 to last port :33
  BF_BMAP_LOG_DEBUG("Num of connectors : %d\n", nconnectors);
  bd_to_asic_map.num_of_connectors = nconnectors;
  bd_to_asic_map.cfg_parsed_ok = 1;
  err = 0;

handle_error:
  if (root) {
    cJSON_Delete(root);
  }
  if (err) {
    BF_BMAP_LOG_ERROR("parsing error\n");
    bf_hw_clear_bd_map();
  }

  return err;
}

/*
 * Parses the given jason for board-lane-mapping
 * Returns 0, if jason-file is not found or
 * Return  0, if no parsing errors are found.
 * Return  -1, if error.
 */
static int bf_derive_board_map_using_json_file(
    bf_dev_id_t dev_id,
    int *bd_config_found,
    bool *is_hardware,
    bf_switchd_internal_context_t *switchd_ctx) {
  FILE *fd = NULL;
  char *buffer = NULL;
  char bd_map_file[BF_SWITCHD_MAX_FILE_NAME] = {0};
  int rv = -1;
  cJSON *json = NULL;

  if ((!bd_config_found) || (!is_hardware) || (!switchd_ctx)) {
    BF_BMAP_LOG_ERROR("%s NULL param\n", __func__);
    return -1;
  }

  if (dev_id != 0) {
    BF_BMAP_LOG_DEBUG(
        "%s Board-map not Supported or already parsed on device %d\n",
        __func__,
        dev_id);
    *bd_config_found = 0;
    *is_hardware = false;
    return 0;
  }

  // In case of multiple devices, conf parsing happens only once
  // Match the configured-device-id with requested one and return.
  *bd_config_found = 0;
  if (bd_to_asic_map.cmap && bd_to_asic_map.num_of_connectors &&
      bd_to_asic_map.cfg_parsed_ok) {
    // device_id_cfged derived from connector map
    if (bd_to_asic_map.device_id_cfged[dev_id]) {
      *bd_config_found = (int)bd_to_asic_map.cfg_found;
      *is_hardware = bd_to_asic_map.is_hardware;
    }
    return 0;
  }

  bd_to_asic_map.cfg_found = 0;
  strncpy(bd_map_file,
          BOARD_LANE_MAP_CONF_FILE_DEFAULT_PATH,
          BF_SWITCHD_MAX_FILE_NAME - 1);
  fd = fopen(bd_map_file, "r");

  // Check for user defined path
  if (!fd) {
    if (strlen(switchd_ctx->board_port_map_conf_file)) {
      fd = fopen(switchd_ctx->board_port_map_conf_file, "r");
      if (!fd) {
        BF_BMAP_LOG_ERROR("board-map file %s not found\n",
                          switchd_ctx->board_port_map_conf_file);
        return 0;  // return no-error
      }
      snprintf(bd_map_file,
               sizeof(bd_map_file),
               "%s",
               switchd_ctx->board_port_map_conf_file);
    }
  }

  if (fd) {
    BF_BMAP_LOG_DEBUG("loading board-map %s...\n", bd_map_file);
    fseek(fd, 0, SEEK_END);
    int length = ftell(fd);
    fseek(fd, 0, SEEK_SET);
    buffer = (char *)malloc(length);
    assert(NULL != buffer);
    if (fread(buffer, 1, length, fd) == 0) {
      BF_BMAP_LOG_ERROR("fread returned error\n");
      fclose(fd);
      goto err_hndlr;
    }
    fclose(fd);
  } else {
    // not an error
    return 0;
  }

  assert(NULL != buffer);

  if (switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO2) {
    BF_BMAP_LOG_DEBUG("Parsing TF2 board-map\n");
    rv = bf_hw_parse_tf2_conf_file(bd_config_found, is_hardware, buffer);
    free(buffer);
    return rv;
  } else if (switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO3) {
    BF_BMAP_LOG_DEBUG("Parsing TF3 board-map\n");
    rv = bf_hw_parse_tf3_conf_file(bd_config_found, is_hardware, buffer);
    free(buffer);
    return rv;
  }

  // TF1 conf file

  *bd_config_found = 1;
  bd_to_asic_map.cfg_found = 1;

  // Now parse the board-mapping
  json = cJSON_Parse(buffer);
  assert(NULL != json);

  // donot throw error if "is_hardware" is not found; default to hardware.
  int err = 0, is_hw = 1;
  cJSON *jobj = cJSON_GetObjectItem(json, "is_hardware");
  if (jobj) {
    err = bf_cjson_get_int(json, "is_hardware", &is_hw);
    if (err) {
      BF_BMAP_LOG_ERROR("%s Could not find is_hardware value\n", __func__);
      goto err_hndlr;
    }
  }
  *is_hardware = is_hw;

  int en_log = 0;
  jobj = cJSON_GetObjectItem(json, "enable_debug_log");
  if (jobj) {
    err = bf_cjson_get_int(json, "enable_debug_log", &en_log);
    if (err) {
      // error logged @ bf_cjson*, simply return.
      goto err_hndlr;
    }
  }

  jobj = cJSON_GetObjectItem(json, "board_lane_map_entry");
  assert(NULL != jobj);

  int i, connector, lane, mac_block, tx_lane, rx_lane, tx_pn_swap, rx_pn_swap,
      mac_ch;
  int rows;
  bf_conn_map_t *cmap = NULL;

  rows = cJSON_GetArraySize(jobj);
  if (rows == 0) {
    BF_BMAP_LOG_ERROR("Parsing error empty board_lane_map_entry\n");
    goto err_hndlr;
  }

  BF_BMAP_LOG_INFO(en_log, "Parsing Board-lane-entries %d found\n", rows);

  /* allocate space to hold all cmap_entry */
  cmap = calloc(rows, sizeof(bf_conn_map_t));
  if (cmap == NULL) {
    BF_BMAP_LOG_ERROR("Could not allocate memory for cmap_ent\n");
    goto err_hndlr;
  }

  bd_to_asic_map.enable_debug_log = en_log;
  bd_to_asic_map.num_of_connectors = rows + 1;
  bd_to_asic_map.cmap = cmap;
  bd_to_asic_map.is_hardware = is_hw;
  BF_BMAP_LOG_INFO(en_log, "num-of-connectors:%d\n", rows);

  bf_map_sts_t map_status = bf_map_init(&bf_board_map_db);
  if (map_status != BF_MAP_OK) {
    BF_BMAP_LOG_ERROR("%s error init the map\n", __func__);
    goto err_hndlr;
  }

  bf_dev_id_t device_id = 0;
  bd_to_asic_map.cfg_parsed_ok = 0;
  for (i = 0; i < rows; i++, cmap++) {
    if (en_log) {
      BF_BMAP_LOG_DEBUG("\n");
    }
    cJSON *item = cJSON_GetArrayItem(jobj, i);
    assert(NULL != item);

    err |= bf_cjson_get_int(item, "connector", &connector);
    err |= bf_cjson_get_int(item, "mac_block", &mac_block);

    // Optional device_id
    device_id = 0;
    cJSON *devidobj = cJSON_GetObjectItem(item, "device_id");
    if (devidobj) {
      err |= bf_cjson_get_int(item, "device_id", &device_id);
    }

    if (device_id >= BF_MAX_DEV_COUNT) {
      BF_BMAP_LOG_ERROR("Error %s Device-id %d exceeds max count allowed\n",
                        __func__,
                        device_id);
      goto err_hndlr;
    }

    // mark configured devices
    bd_to_asic_map.device_id_cfged[device_id] = 1;

    // Optional media-type
    char *media_name = "Unknown";
    bf_media_type_t media_type = BF_MEDIA_TYPE_UNKNOWN;
    int enable_auto_neg = 0;
    cJSON *media = cJSON_GetObjectItem(item, "media_type");
    if (media) {
      err |= bf_cjson_get_string(item, "media_type", &media_name);
      if (!strcmp(media_name, "copper")) {
        media_type = BF_MEDIA_TYPE_COPPER;
        enable_auto_neg = 1;
      }
      if (!strcmp(media_name, "optical")) {
        media_type = BF_MEDIA_TYPE_OPTICAL;
        enable_auto_neg = 0;
      }
    }
    if (err) {
      // error logged @ bf_cjson*, simply return.
      goto err_hndlr;
    }

    BF_BMAP_LOG_INFO(en_log,
                     "Connector->%d device_id:%d mac_block:%d media:%s "
                     "enable-auto-neg:%d\n",
                     connector,
                     device_id,
                     mac_block,
                     media_name,
                     enable_auto_neg);

    // No parsing error per connector, do bd-entry.
    cmap->connector = connector;
    cmap->device_id = device_id;
    cmap->mac_block = mac_block;
    cmap->media_type = media_type;
    cmap->enable_auto_neg = enable_auto_neg;
    cmap->mark_port_as_ready = true;  // in bsp-less mode
    cmap->nlanes_per_ch = 1;

    char buf[6] = {0};
    const char *laneptr = buf;
    cJSON *subitem = cJSON_GetObjectItem(item, "lane0");
    assert(NULL != subitem);

    for (lane = 0; lane < 4; lane++) {
      cmap->lane_present[lane] = false;

      snprintf(buf, sizeof(buf), "lane%d", lane);
      subitem = cJSON_GetObjectItem(item, laneptr);

      // A connector should have atleast one lane.
      if (lane == 0) {
        assert(NULL != subitem);
      }

      if ((lane > 0) && (!subitem)) {
        continue;
      }
      err |= bf_cjson_get_int(subitem, "mac_ch", &mac_ch);
      err |= bf_cjson_get_int(subitem, "tx_lane", &tx_lane);
      err |= bf_cjson_get_int(subitem, "rx_lane", &rx_lane);
      err |= bf_cjson_get_int(subitem, "tx_pn_swap", &tx_pn_swap);
      err |= bf_cjson_get_int(subitem, "rx_pn_swap", &rx_pn_swap);
      if (err) {
        // error logged @ bf_cjson*, simply return.
        goto err_hndlr;
      }

      BF_BMAP_LOG_INFO(en_log,
                       "%s mac_ch:%d tx_lane:%d, rx_lane:%d, tx_pn_swap:%d "
                       "rx_pn_swap:%d\n",
                       laneptr,
                       mac_ch,
                       tx_lane,
                       rx_lane,
                       tx_pn_swap,
                       rx_pn_swap);

      /* Optional param */
      int tx_eq_pre = 0, tx_eq_post = 0, tx_eq_attn = 0;
      cJSON *sp = cJSON_GetObjectItem(subitem, "serdes_params");
      if (sp) {
        err |= bf_cjson_get_int(sp, "tx_eq_pre", &tx_eq_pre);
        err |= bf_cjson_get_int(sp, "tx_eq_post", &tx_eq_post);
        err |= bf_cjson_get_int(sp, "tx_eq_attn", &tx_eq_attn);
        if (err) {
          // error logged @ bf_cjson*, simply return.
          goto err_hndlr;
        }
        BF_BMAP_LOG_INFO(en_log,
                         "tx_eq_pre:%d tx_eq_post:%d tx_eq_attn:%d\n",
                         tx_eq_pre,
                         tx_eq_post,
                         tx_eq_attn);
      }
      cmap->lane_present[lane] = true;

      lane_map_t *lmap = &cmap->lane[lane];
      lmap->mac_ch = mac_ch;
      lmap->channel = mac_ch;
      lmap->rx_lane = rx_lane;
      lmap->rx_pn_swap = rx_pn_swap;
      lmap->tx_lane = tx_lane;
      lmap->tx_pn_swap = tx_pn_swap;
      lmap->tx_eq_pre = tx_eq_pre;
      lmap->tx_eq_post = tx_eq_post;
      lmap->tx_eq_attn = tx_eq_attn;

      unsigned long cmap_key = BF_GET_CONN_MAP_KEY(connector, lane);

      BF_BMAP_LOG_INFO(en_log,
                       "map-add dev:%d: :%d/%d : %08x\n",
                       dev_id,
                       connector,
                       mac_ch,
                       (unsigned int)cmap_key);

      map_status = bf_map_add(&bf_board_map_db, cmap_key, (void *)cmap);
      if (map_status != BF_MAP_OK) {
        BF_BMAP_LOG_ERROR(
            "Map-add failed for board map for dev:%d:connector:%d/%d : "
            "map-error : %d\n",
            dev_id,
            connector,
            mac_ch,
            map_status);
        rv = -1;
        goto err_hndlr;
      }
    }
  }
  rv = 0;
  bd_to_asic_map.cfg_parsed_ok = 1;

err_hndlr:
  if (json) {
    cJSON_Delete(json);
  }

  if (buffer) {
    free(buffer);
  }

  if (rv != 0) {
    cmap = bd_to_asic_map.cmap;
    if (cmap) {
      free(cmap);
    }
    bd_to_asic_map.cmap = NULL;
  }
  return rv;
}

/*
 * Based on the config-file, sets the switch context to hardware or model
 * Returns 0 if no-config file is found
 * Returns 0 if config file is found
 * Returns -1 if there is any error in parsing config file.
 */
int bf_hw_porting_handle_board_config_map(
    int *board_conf_present, bf_switchd_internal_context_t *switchd_ctx) {
  int bd_conf_present = 0;
  bool is_hardware = 0, is_sw_model;
  bf_dev_id_t dev_id = 0;

  if ((!switchd_ctx) || (!board_conf_present)) {
    return -1;
  }

  for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    bd_conf_present = is_hardware = 0;
    if (switchd_ctx->asic[dev_id].configured == 0) {
      continue;
    }

    if (bf_derive_board_map_using_json_file(
            dev_id, &bd_conf_present, &is_hardware, switchd_ctx) != 0) {
      return -1;
    }

    if (bd_conf_present) {
      is_sw_model = !is_hardware;
      if (bf_switchd_device_type_update(dev_id, is_sw_model) != 0) {
        return -1;
      }
      bf_hw_porting_register_pltm_vectors(is_hardware);

      // represents all dev-ids
      *board_conf_present = bd_conf_present;
    }
  }
  return 0;
}
