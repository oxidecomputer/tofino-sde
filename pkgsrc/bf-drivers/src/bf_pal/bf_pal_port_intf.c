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


#include <tofino/bf_pal/bf_pal_port_intf.h>
#include <tofino/bf_pal/bf_pal_types.h>
#include <dvm/bf_drv_intf.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include <bf_pm/bf_pm_intf.h>
#include <pipe_mgr/pipe_mgr_intf.h>
#include <port_mgr/bf_port_if.h>
#include <traffic_mgr/traffic_mgr_read_apis.h>
#include <traffic_mgr/traffic_mgr_port_intf.h>
#include <lld/lld_err.h>
#include <lld/lld_sku.h>

#include "bf_pal_log.h"

bf_status_t bf_pal_max_ports_get(bf_dev_id_t dev_id, uint32_t *ports) {
  bf_status_t sts;

  sts = bf_pm_num_max_ports_get(dev_id, ports);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR("Unable to get the max ports for dev : %d : %s (%d)",
                 dev_id,
                 bf_err_str(sts),
                 sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_num_front_ports_get(bf_dev_id_t dev_id, uint32_t *ports) {
  bf_status_t sts;

  sts = bf_pm_num_front_ports_get(dev_id, ports);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR("Unable to get the max ports for dev : %d : %s (%d)",
                 dev_id,
                 bf_err_str(sts),
                 sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_fp_idx_to_dev_port_map(bf_dev_id_t dev_id,
                                          uint32_t fp_idx,
                                          bf_dev_port_t *dev_port) {
  bf_status_t sts;

  sts = bf_pm_front_port_index_to_dev_port_get(dev_id, fp_idx, dev_port);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the dev port for dev : %d : front port index : %d : %s "
        "(%d)",
        dev_id,
        fp_idx,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_get_first(bf_dev_id_t dev_id, bf_dev_port_t *dev_port) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;
  bf_dev_id_t dev_id_of_port = 0;

  sts = bf_pm_port_front_panel_port_get_first(dev_id, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR("Unable to get the first port for dev : %d : %s (%d)",
                 dev_id,
                 bf_err_str(sts),
                 sts);
    return sts;
  }

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      &port_hdl, &dev_id_of_port, dev_port);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the dev port for dev : %d : front panel port : %d/%d : "
        "%s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        bf_err_str(sts),
        sts);
    return sts;
  }
  if (dev_id_of_port != dev_id) {
    BF_PAL_ERROR("Warning: %d/%d found on dev%d : exp dev%d",
                 port_hdl.conn_id,
                 port_hdl.chnl_id,
                 dev_id_of_port,
                 dev_id);
  }
  return BF_SUCCESS;
}

bf_status_t bf_pal_port_get_first_added(bf_dev_id_t dev_id,
                                        bf_dev_port_t *dev_port) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;
  bf_dev_id_t dev_id_of_port = 0;

  sts = bf_pm_port_front_panel_port_get_first_added(dev_id, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_DEBUG("Unable to get the first added port for dev : %d : %s (%d)",
                 dev_id,
                 bf_err_str(sts),
                 sts);
    return sts;
  }

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      &port_hdl, &dev_id_of_port, dev_port);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the dev port for dev : %d : front panel port : %d/%d : "
        "%s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        bf_err_str(sts),
        sts);
    return sts;
  }
  if (dev_id_of_port != dev_id) {
    BF_PAL_ERROR("Warning: %d/%d found on dev%d : exp dev%d",
                 port_hdl.conn_id,
                 port_hdl.chnl_id,
                 dev_id_of_port,
                 dev_id);
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_get_next(bf_dev_id_t dev_id,
                                 bf_dev_port_t curr_dev_port,
                                 bf_dev_port_t *next_dev_port) {
  bf_status_t sts;
  bf_pal_front_port_handle_t curr_port_hdl, next_port_hdl;
  bf_dev_id_t dev_id_of_port = 0;

  sts = bf_pm_port_dev_port_to_front_panel_port_get(
      dev_id, curr_dev_port, &curr_port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front panel port for dev : %d : dev port : %d : %s "
        "(%d)",
        dev_id,
        curr_dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_front_panel_port_get_next(
      dev_id, &curr_port_hdl, &next_port_hdl);
  if (sts == BF_OBJECT_NOT_FOUND) {
    // Indicates that this is the final port in the system
    if (!next_dev_port) {
      BF_PAL_ERROR("Please allocate memory for the out param");
      return BF_INVALID_ARG;
    }
    *next_dev_port = -1;
    return sts;
  }

  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the next front panel for dev : %d : front panel port : "
        "%d/%d (%d) : %s (%d)",
        dev_id,
        curr_port_hdl.conn_id,
        curr_port_hdl.chnl_id,
        curr_dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      &next_port_hdl, &dev_id_of_port, next_dev_port);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the dev port for dev : %d : front panel port : %d/%d : "
        "%s (%d)",
        dev_id,
        next_port_hdl.conn_id,
        next_port_hdl.chnl_id,
        bf_err_str(sts),
        sts);
    return sts;
  }
  if (dev_id_of_port != dev_id) {
    BF_PAL_ERROR("Warning: %d/%d found on dev%d : exp dev%d",
                 next_port_hdl.conn_id,
                 next_port_hdl.chnl_id,
                 dev_id_of_port,
                 dev_id);
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_get_next_added(bf_dev_id_t dev_id,
                                       bf_dev_port_t curr_dev_port,
                                       bf_dev_port_t *next_dev_port) {
  bf_status_t sts;
  bf_pal_front_port_handle_t curr_port_hdl, next_port_hdl;
  bf_dev_id_t dev_id_of_port = 0;

  sts = bf_pm_port_dev_port_to_front_panel_port_get(
      dev_id, curr_dev_port, &curr_port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_DEBUG(
        "Unable to get the front panel port for dev : %d : dev port : %d : %s "
        "(%d)",
        dev_id,
        curr_dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_front_panel_port_get_next_added(
      dev_id, &curr_port_hdl, &next_port_hdl);
  if (sts == BF_OBJECT_NOT_FOUND) {
    // Indicates that this is the final port in the system
    *next_dev_port = -1;
    return sts;
  }

  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the next added front panel for dev : %d : front panel "
        "port : "
        "%d/%d (%d) : %s (%d)",
        dev_id,
        curr_port_hdl.conn_id,
        curr_port_hdl.chnl_id,
        curr_dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      &next_port_hdl, &dev_id_of_port, next_dev_port);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the dev port for dev : %d : front panel port : %d/%d : "
        "%s (%d)",
        dev_id,
        next_port_hdl.conn_id,
        next_port_hdl.chnl_id,
        bf_err_str(sts),
        sts);
    return sts;
  }
  if (dev_id_of_port != dev_id) {
    BF_PAL_ERROR("Warning: %d/%d found on dev%d : exp dev%d",
                 next_port_hdl.conn_id,
                 next_port_hdl.chnl_id,
                 dev_id_of_port,
                 dev_id);
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_recirc_port_range_get(bf_dev_id_t dev_id,
                                         uint32_t *start_recirc_port,
                                         uint32_t *end_recirc_port) {
  bf_status_t sts;

  sts = bf_pm_recirc_port_range_get(dev_id, start_recirc_port, end_recirc_port);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR("Unable to get the recirc port range for dev : %d : %s (%d)",
                 dev_id,
                 bf_err_str(sts),
                 sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_recirc_port_to_dev_port_map(bf_dev_id_t dev_id,
                                               uint32_t recirc_port,
                                               bf_dev_port_t *dev_port) {
  bf_status_t sts;

  sts = bf_pm_recirc_port_to_dev_port_get(dev_id, recirc_port, dev_port);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the dev port for dev : %d : recirc port : %d : %s (%d)",
        dev_id,
        recirc_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_get_default_lane_numb(bf_dev_id_t dev_id,
                                              bf_port_speed_t speed,
                                              uint32_t *lane_numb) {
  return bf_port_get_default_lane_numb(dev_id, speed, lane_numb);
}

bf_status_t bf_pal_port_add(bf_dev_id_t dev_id,
                            bf_dev_port_t dev_port,
                            bf_port_speed_t speed,
                            bf_fec_type_t fec_type) {
  uint32_t lane_numb = 0;
  if (bf_port_get_default_lane_numb(dev_id, speed, &lane_numb) != BF_SUCCESS)
    return BF_INVALID_ARG;

  return bf_pal_port_add_with_lanes(
      dev_id, dev_port, speed, lane_numb, fec_type);
}

bf_status_t bf_pal_port_add_with_lanes(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port,
                                       bf_port_speed_t speed,
                                       uint32_t n_lanes,
                                       bf_fec_type_t fec_type) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_add_with_lanes(dev_id, &port_hdl, speed, n_lanes, fec_type);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to add port for dev : %d : front port : %d/%d (%d) : %s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_add_all(bf_dev_id_t dev_id,
                                bf_port_speed_t speed,
                                bf_fec_type_t fec_type) {
  return bf_pm_port_add_all(dev_id, speed, fec_type);
}

bf_status_t bf_pal_port_del(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_delete(dev_id, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to delete port for dev : %d : front port : %d/%d (%d) : %s "
        "(%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_del_with_lanes(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port,
                                       bf_port_speed_t speed,
                                       uint32_t n_lanes) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;
  uint32_t lane_numb = 0, lane = 0;
  bool is_added = false;

  if (!n_lanes) {
    if (bf_port_get_default_lane_numb(dev_id, speed, &lane_numb) != BF_SUCCESS)
      return BF_INVALID_ARG;
  } else
    lane_numb = n_lanes;

  for (lane = 1; lane < lane_numb;
       lane++) { /* 0th lane will be deleted at speed set */
    sts = bf_pm_port_dev_port_to_front_panel_port_get(
        dev_id, dev_port + lane, &port_hdl);
    if (sts != BF_SUCCESS) {
      BF_PAL_ERROR(
          "Unable to get the front port handle for dev : %d : dev_port : %d : "
          "%s "
          "(%d)",
          dev_id,
          dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }

    sts = bf_pm_is_port_added(dev_id, &port_hdl, &is_added);
    if (sts != BF_SUCCESS) {
      BF_PAL_ERROR(
          "Unable to check whether port is added for dev : %d : front port : "
          "%d/%d "
          "(%d) : %s "
          "(%d)",
          dev_id,
          port_hdl.conn_id,
          port_hdl.chnl_id,
          dev_port,
          bf_err_str(sts),
          sts);
      return sts;
    }
    if (is_added) {
      sts = bf_pm_port_delete(dev_id, &port_hdl);
      if (sts != BF_SUCCESS) {
        BF_PAL_ERROR(
            "Unable to delete port for dev : %d : front port : %d/%d (%d) : %s "
            "(%d)",
            dev_id,
            port_hdl.conn_id,
            port_hdl.chnl_id,
            dev_port,
            bf_err_str(sts),
            sts);
        return sts;
      }
    }
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_del_all(bf_dev_id_t dev_id) {
  return bf_pm_port_delete_all(dev_id);
}

bf_status_t bf_pal_port_enable(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_enable(dev_id, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to enable port for dev : %d : front port : %d/%d (%d) : %s "
        "(%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_enable_all(bf_dev_id_t dev_id) {
  return bf_pm_port_enable_all(dev_id);
}

bf_status_t bf_pal_port_disable(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_disable(dev_id, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to disable port for dev : %d : front port : %d/%d (%d) : %s "
        "(%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_all_stats_get(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      uint64_t stats[BF_NUM_RMON_COUNTERS]) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_all_stats_get(dev_id, &port_hdl, stats);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get port stats for dev : %d : front port : %d/%d (%d) : %s "
        "(%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_all_pure_stats_get_with_timestamp(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    uint64_t stats[BF_NUM_RMON_COUNTERS],
    int64_t *timestamp_s,
    int64_t *timestamp_ns) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_all_pure_stats_get_with_timestamp(
      dev_id, &port_hdl, stats, timestamp_s, timestamp_ns);
  return sts;
}

bf_status_t bf_pal_port_this_stat_get(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      bf_rmon_counter_t ctr_type,
                                      uint64_t *stat_val) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_this_stat_get(dev_id, &port_hdl, ctr_type, stat_val);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get port stat (%d) for dev : %d : front port : %d/%d (%d) : "
        "%s (%d)",
        ctr_type,
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_pkt_rate_get(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port,
                                     bf_pkt_rate_t *pkt_rate) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_pkt_rate_get(dev_id, &port_hdl, pkt_rate);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get port packet rate for dev : %d : front port : %d/%d (%d) "
        ": "
        "%s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_this_stat_get_with_timestamp(bf_dev_id_t dev_id,
                                                     bf_dev_port_t dev_port,
                                                     bf_rmon_counter_t ctr_type,
                                                     uint64_t *stat_val,
                                                     int64_t *timestamp_s,
                                                     int64_t *timestamp_ns) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_this_stat_get_with_timestamp(
      dev_id, &port_hdl, ctr_type, stat_val, timestamp_s, timestamp_ns);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get port stat (%d) for dev : %d : front port : %d/%d (%d) : "
        "%s (%d)",
        ctr_type,
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_this_stat_id_to_str(bf_rmon_counter_t ctr_type,
                                            char **str) {
  return bf_port_rmon_counter_to_str(ctr_type, str);
}

bf_status_t bf_pal_port_this_stat_clear(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        bf_rmon_counter_t ctr_type) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_this_stat_clear(dev_id, &port_hdl, ctr_type);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to clear port stat (%d) for dev : %d : front port : %d/%d (%d) "
        ": %s (%d)",
        ctr_type,
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_all_stats_clear(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_all_stats_clear(dev_id, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to clear all port stats for dev : %d : front port : %d/%d (%d) "
        ": %s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_stats_poll_intvl_set(bf_dev_id_t dev_id,
                                             uint32_t poll_intvl_ms) {
  bf_status_t sts;

  sts = bf_pm_port_stats_poll_period_update(dev_id, poll_intvl_ms);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR("Unable to update the port stats timer for dev : %d : %s (%d)",
                 dev_id,
                 bf_err_str(sts),
                 sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_stats_poll_stop(bf_dev_id_t dev_id) {
  bf_status_t sts;

  sts = bf_pm_port_stats_poll_stop(dev_id);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR("Unable to stop the port stats timer for dev : %d : %s (%d)",
                 dev_id,
                 bf_err_str(sts),
                 sts);
    return sts;
  }
  return BF_SUCCESS;
}

bf_status_t bf_pal_port_stats_poll_intvl_get(bf_dev_id_t dev_id,
                                             uint32_t *poll_intvl_ms) {
  bf_status_t sts;
  if (poll_intvl_ms == NULL) return BF_INVALID_ARG;
  sts = bf_pm_port_stats_poll_period_get(dev_id, poll_intvl_ms);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR("Unable to get the port stats timer for dev : %d : %s (%d)",
                 dev_id,
                 bf_err_str(sts),
                 sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_status_notif_reg(port_status_chg_cb cb_fn,
                                         void *cookie) {
  return bf_port_client_register_status_notif(cb_fn, cookie);
}

bf_status_t bf_pal_port_mode_change_notif_reg(
    bf_dev_id_t dev_id,
    port_mode_chg_cb mode_chg_cb_fn,
    void *mode_chg_cookie,
    port_mode_chg_cmplt_cb cmplt_cb_fn,
    void *mode_chg_cmplt_cookie) {
  return bf_port_client_register_mode_change_notif(dev_id,
                                                   mode_chg_cb_fn,
                                                   mode_chg_cookie,
                                                   cmplt_cb_fn,
                                                   mode_chg_cmplt_cookie);
}

bf_status_t bf_pal_port_loopback_mode_set(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port,
                                          bf_loopback_mode_e mode) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_loopback_mode_set(dev_id, &port_hdl, mode);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to set port in loopback mode (%d) for dev : %d : front port : "
        "%d/%d (%d) : %s (%d)",
        mode,
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_loopback_mode_get(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port,
                                          bf_loopback_mode_e *mode) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;
  if (mode == NULL) return BF_INVALID_ARG;
  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_loopback_mode_get(dev_id, &port_hdl, mode);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get port loopback mode for dev : %d : front port : "
        "%d/%d (%d) : %s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_autoneg_policy_get(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    bf_pm_port_autoneg_policy_e *an_policy) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  if (an_policy == NULL) return BF_INVALID_ARG;
  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_autoneg_get(dev_id, &port_hdl, an_policy);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get autoneg policy for dev : %d : front port : %d/%d "
        "(%d) : %s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_autoneg_policy_set(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           int an_policy) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_autoneg_set(dev_id, &port_hdl, an_policy);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to set autoneg policy (%d) for dev : %d : front port : %d/%d "
        "(%d) : %s (%d)",
        an_policy,
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_autoneg_policy_set_all(bf_dev_id_t dev_id,
                                               int an_policy) {
  return bf_pm_port_autoneg_set_all(dev_id, an_policy);
}

bf_status_t bf_pal_port_autoneg_adv_speed_set(bf_dev_id_t dev_id,
                                              bf_dev_port_t dev_port,
                                              bf_port_speed_t *adv_speed_arr,
                                              uint32_t adv_speed_cnt) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;
  if (adv_speed_arr == NULL) return BF_INVALID_ARG;
  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts =
      bf_pm_port_adv_speed_set(dev_id, &port_hdl, adv_speed_arr, adv_speed_cnt);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to set advertised speeds for dev : %d : front port : %d/%d "
        "(%d) : %s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_autoneg_adv_fec_set(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port,
                                            bf_fec_type_t *adv_fec_arr,
                                            uint32_t adv_fec_cnt) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;
  if (adv_fec_arr == NULL) return BF_INVALID_ARG;
  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_adv_fec_set(dev_id, &port_hdl, adv_fec_arr, adv_fec_cnt);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to set advertised fecs for dev : %d : front port : %d/%d "
        "(%d) : %s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_kr_mode_policy_set(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    bf_pm_port_kr_mode_policy_e kr_policy) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_kr_mode_set(dev_id, &port_hdl, kr_policy);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to set kr mode policy (%d) for dev : %d : front port : %d/%d "
        "(%d) : %s (%d)",
        kr_policy,
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_kr_mode_policy_set_all(
    bf_dev_id_t dev_id, bf_pm_port_kr_mode_policy_e kr_policy) {
  return bf_pm_port_kr_mode_set_all(dev_id, kr_policy);
}

bf_status_t bf_pal_port_kr_mode_policy_get(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    bf_pm_port_kr_mode_policy_e *kr_policy) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_kr_mode_get(dev_id, &port_hdl, kr_policy);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get kr mode policy for dev : %d : front port : %d/%d "
        "(%d) : %s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_autoneg_hcd_fec_get(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port,
                                            bf_port_speed_t *hcd,
                                            int *hcd_lanes,
                                            bf_fec_type_t *fec) {
  return bf_port_autoneg_hcd_fec_get_v2(dev_id, dev_port, hcd, hcd_lanes, fec);
}

bf_status_t bf_pal_port_all_stats_update(bf_dev_id_t dev_id,
                                         bf_dev_port_t dev_port) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_all_stats_update(dev_id, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to update stats for dev : %d : front port : %d/%d (%d) : %s "
        "(%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_num_lanes_get(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      int *num_lanes) {
  return bf_port_num_lanes_get(dev_id, dev_port, num_lanes);
}

bf_status_t bf_pal_port_mtu_set(bf_dev_id_t dev_id,
                                bf_dev_port_t dev_port,
                                uint32_t tx_mtu,
                                uint32_t rx_mtu) {
  return bf_port_mtu_set(dev_id, dev_port, tx_mtu, rx_mtu);
}

bf_status_t bf_pal_port_mtu_get(bf_dev_id_t dev_id,
                                bf_dev_port_t dev_port,
                                uint32_t *tx_mtu,
                                uint32_t *rx_mtu) {
  return bf_port_mtu_get(dev_id, dev_port, tx_mtu, rx_mtu);
}

bf_status_t bf_pal_port_max_mtu_get(bf_dev_id_t dev_id, uint32_t *max_mtu) {
  return bf_port_max_mtu_get(dev_id, max_mtu);
}

bf_status_t bf_pal_port_pll_ovrclk_set(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port,
                                       float pll_ovrclk) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_pll_ovrclk_set(dev_id, &port_hdl, pll_ovrclk);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to set PLL Overclock for dev : %d : front port : %d/%d (%d) : "
        "%s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_pll_ovrclk_get(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port,
                                       float *pll_ovrclk) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;
  if (pll_ovrclk == NULL) return BF_INVALID_ARG;
  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_pll_ovrclk_get(dev_id, &port_hdl, pll_ovrclk);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get PLL Overclock config for dev : %d : front port : %d/%d "
        "(%d) : %s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_flow_control_pfc_set(bf_dev_id_t dev_id,
                                             bf_dev_port_t dev_port,
                                             uint32_t tx_en_map,
                                             uint32_t rx_en_map) {
  return bf_port_flow_control_pfc_set(dev_id, dev_port, tx_en_map, rx_en_map);
}

bf_status_t bf_pal_port_flow_control_pfc_get(bf_dev_id_t dev_id,
                                             bf_dev_port_t dev_port,
                                             uint32_t *tx_en_map,
                                             uint32_t *rx_en_map) {
  return bf_port_flow_control_pfc_get(dev_id, dev_port, tx_en_map, rx_en_map);
}

bf_status_t bf_pal_port_flow_control_link_pause_set(bf_dev_id_t dev_id,
                                                    bf_dev_port_t dev_port,
                                                    bool tx_en,
                                                    bool rx_en) {
  return bf_port_flow_control_link_pause_set(dev_id, dev_port, tx_en, rx_en);
}

bf_status_t bf_pal_port_flow_control_link_pause_get(bf_dev_id_t dev_id,
                                                    bf_dev_port_t dev_port,
                                                    bool *tx_en,
                                                    bool *rx_en) {
  return bf_port_flow_control_link_pause_get(dev_id, dev_port, tx_en, rx_en);
}

bf_status_t bf_pal_port_oper_state_get(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port,
                                       bool *state) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }
  sts = bf_pm_port_oper_status_get(dev_id, &port_hdl, state);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the port status for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_is_valid(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  return bf_port_is_valid(dev_id, dev_port);
}

bf_status_t bf_pal_port_fec_set(bf_dev_id_t dev_id,
                                bf_dev_port_t dev_port,
                                bf_fec_type_t fec_type) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_fec_set(dev_id, &port_hdl, fec_type);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to set fec for dev : %d : front port : %d/%d (%d) : %s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_fec_get(bf_dev_id_t dev_id,
                                bf_dev_port_t dev_port,
                                bf_fec_type_t *fec_type) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;
  if (fec_type == NULL) return BF_INVALID_ARG;
  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_fec_get(dev_id, &port_hdl, fec_type);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get fec for dev : %d : front port : %d/%d (%d) : %s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_media_type_get(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port,
                                       bf_media_type_t *media_type) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_media_type_get(dev_id, &port_hdl, media_type);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the media type for dev : %d : front port : %d/%d (%d) : "
        "%s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_cut_through_enable(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port) {
  bf_status_t sts = BF_SUCCESS;
  bool ct_enabled;

  // Safety checks
  if (dev_id < 0 || dev_id >= BF_MAX_DEV_COUNT) {
    return BF_INVALID_ARG;
  }

  /*
   * Enabling cut-through involes two steps -
   *      - Enable CT in TM port speed register
   *      - Enable in ParDe EBUF channel mode register
   *
   * Since it is a two step process, if second step fails, first step needs to
   * be reverted to make sure both TM and ParDe EBUF registers are in
   * sync. For this purpose, first get the current status in TM SW cache.
   * HW read is avoided to handle warm init cases.
   */
  sts = bf_tm_port_cut_through_enable_status_get(
      dev_id, dev_port, &ct_enabled, NULL);  // Pass NULL for HW status
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Failed to get cut-through enable status for dev : %d : dev_port : %d "
        ": %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  /* If cut-through is already enabled, return */
  if (ct_enabled) {
    BF_PAL_DEBUG("Cut-through is already enabled dev : %d : dev_port : %d",
                 dev_id,
                 dev_port);

    return (BF_SUCCESS);
  }

  sts = bf_tm_port_cut_through_enable(dev_id, dev_port);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Enabling cut-through in TM failed for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pipe_mgr_port_ebuf_enable_cut_through(dev_id, dev_port);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Enabling cut-through in pipe-mgr EBUF  failed for dev : %d : dev_port "
        ": %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);

    /* Revert step #1, no need to error check */
    bf_tm_port_cut_through_disable(dev_id, dev_port);
    return sts;
  }

  return sts;
}

bf_status_t bf_pal_port_cut_through_disable(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port) {
  bf_status_t sts;
  bool ct_enabled;

  // Safety checks
  if (dev_id < 0 || dev_id >= BF_MAX_DEV_COUNT) {
    return BF_INVALID_ARG;
  }

  /*
   * Disabling cut-through involves two step process -
   *      - Disable CT in TM port speed register
   *      - Disable in ParDe EBUF channel mode register
   *
   * Since it is a two step process, if second step fails, first step needs to
   * be reverted to make sure both TM and ParDe EBUF registers are in
   * sync. For this purpose, first get the current status in TM SW cache.
   * HW read is avoided to handle warm init cases.
   */
  sts = bf_tm_port_cut_through_enable_status_get(
      dev_id, dev_port, &ct_enabled, NULL);  // Pass NULL for HW status
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Failed to get cut-through enable status for dev : %d : dev_port : %d "
        ": %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  /* If cut-through is already disabled, return */
  if (!ct_enabled) {
    BF_PAL_DEBUG("Cut-through is already disabled dev : %d : dev_port : %d",
                 dev_id,
                 dev_port);

    return (BF_SUCCESS);
  }

  sts = bf_tm_port_cut_through_disable(dev_id, dev_port);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Disabling cut-through in TM failed for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pipe_mgr_port_ebuf_disable_cut_through(dev_id, dev_port);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Disabling cut-through in pipe-mgr EBUF  failed for dev : %d : "
        "dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);

    /* Revert step #1, no need to error check */
    bf_tm_port_cut_through_enable(dev_id, dev_port);
    return sts;
  }

  return sts;
}

bf_status_t bf_pal_port_cut_through_enable_status_get(bf_dev_id_t dev_id,
                                                      bf_dev_port_t dev_port,
                                                      bool *ct_enabled) {
  bf_status_t sts = BF_SUCCESS;

  // Safety checks
  if (dev_id < 0 || dev_id >= BF_MAX_DEV_COUNT) {
    return BF_INVALID_ARG;
  }

  /*
   * Get the cut-through enable status from TM
   */
  sts = bf_tm_port_cut_through_enable_status_get(
      dev_id, dev_port, ct_enabled, ct_enabled);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Failed to get cut-through enable status for dev : %d : dev_port : %d "
        ": %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return (sts);
}

bf_status_t bf_pal_num_pipes_get(bf_dev_id_t dev_id, uint32_t *num_pipes) {
  return lld_sku_get_num_active_pipes(dev_id, num_pipes);
}

bf_status_t bf_pal_front_port_to_dev_port_get(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl,
    bf_dev_port_t *dev_port) {
  bf_status_t sts = BF_SUCCESS;
  bf_dev_id_t dev_id_of_port = 0;

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, dev_port);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the dev port for dev : %d : front panel port : %d/%d : "
        "%s (%d)",
        dev_id,
        port_hdl->conn_id,
        port_hdl->chnl_id,
        bf_err_str(sts),
        sts);
  }
  if (dev_id_of_port != dev_id) {
    BF_PAL_ERROR("Warning: %d/%d found on dev%d : exp dev%d",
                 port_hdl->conn_id,
                 port_hdl->chnl_id,
                 dev_id_of_port,
                 dev_id);
  }
  return sts;
}

bf_status_t bf_pal_dev_port_to_front_port_get(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    bf_pal_front_port_handle_t *port_hdl) {
  bf_status_t sts = BF_SUCCESS;

  sts = bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s"
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
  }
  return sts;
}

bool bf_pal_dev_port_speed_validate(bf_dev_id_t dev_id,
                                    bf_dev_port_t dev_port,
                                    bf_port_speed_t speed,
                                    uint32_t n_lanes,
                                    bf_fec_type_t fec) {
  bool sts = false;

  sts =
      bf_pm_port_valid_speed_and_channel(dev_id, dev_port, speed, n_lanes, fec);
  if (sts != true) {
    BF_PAL_ERROR(
        "Speed invalid for %d : dev_port : %d : %s"
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
  }
  return sts;
}

bf_status_t bf_pal_port_str_to_dev_port_map(bf_dev_id_t dev_id,
                                            char *port_str,
                                            bf_dev_port_t *dev_port) {
  bf_status_t sts = bf_pm_port_str_to_dev_port_get(dev_id, port_str, dev_port);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR("Unable to get dev_port for port %s, dev : %d : %s (%d)",
                 port_str,
                 dev_id,
                 bf_err_str(sts),
                 sts);
  }

  return sts;
}

bf_status_t bf_pal_dev_port_to_port_str_map(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port,
                                            char *port_str) {
  bf_status_t sts = bf_pm_dev_port_to_port_str_get(dev_id, dev_port, port_str);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR("Unable to get port name for dev_port %d, dev : %d : %s (%d)",
                 dev_port,
                 dev_id,
                 bf_err_str(sts),
                 sts);
  }

  return sts;
}

bf_status_t bf_pal_is_port_internal(bf_dev_id_t dev_id,
                                    bf_dev_port_t dev_port,
                                    bool *is_internal) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  if (is_internal == NULL) {
    return BF_INVALID_ARG;
  }
  *is_internal = false;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_is_port_internal(dev_id, &port_hdl, is_internal);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to check for internal port for dev : %d : front port : %d/%d "
        "(%d) : %s "
        "(%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_is_port_added(bf_dev_id_t dev_id,
                                 bf_dev_port_t dev_port,
                                 bool *is_added) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  if (is_added == NULL) {
    return BF_INVALID_ARG;
  }
  *is_added = false;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_is_port_added(dev_id, &port_hdl, is_added);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to check whether port is added for dev : %d : front port : "
        "%d/%d "
        "(%d) : %s "
        "(%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_stat_direct_get(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        bf_rmon_counter_t *ctr_type_array,
                                        uint64_t *stat_val,
                                        uint32_t num_of_ctr) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_stat_direct_get(
      dev_id, &port_hdl, ctr_type_array, stat_val, num_of_ctr);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get port stat for dev : %d : front port : %d/%d (%d) : "
        "%s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_direction_set(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      bf_pm_port_dir_e port_dir) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_direction_set(dev_id, &port_hdl, port_dir);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to set port direction (%d) for dev : %d : front port : %d/%d "
        "(%d) : %s (%d)",
        port_dir,
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_direction_get(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      bf_pm_port_dir_e *port_dir) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }
  return bf_pm_port_direction_get(dev_id, &port_hdl, port_dir);
}
bf_status_t bf_pal_port_media_type_set(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port,
                                       bf_media_type_t media_type) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  // Safety checks
  if (dev_id < 0 || dev_id >= BF_MAX_DEV_COUNT) {
    return BF_INVALID_ARG;
  }

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_media_type_set(dev_id, &port_hdl, media_type);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the media type for dev : %d : front port : %d/%d (%d) : "
        "%s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_serdes_params_set(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    bf_pal_serdes_params_t *serdes_param) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  // Safety checks
  if ((dev_id < 0 || dev_id >= BF_MAX_DEV_COUNT) || (!serdes_param)) {
    return BF_INVALID_ARG;
  }

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_serdes_param_set(dev_id, &port_hdl, serdes_param);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the serdes params for dev : %d : front port : %d/%d "
        "(%d) : "
        "%s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_get_pipe_from_dev_port(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port,
                                          bf_dev_pipe_t *phy_pipe,
                                          bf_dev_pipe_t *log_pipe) {
  lld_err_t err;

  // Safety checks
  if ((dev_id < 0 || dev_id >= BF_MAX_DEV_COUNT) || (!phy_pipe) ||
      (!log_pipe)) {
    return BF_INVALID_ARG;
  }

  *log_pipe = DEV_PORT_TO_PIPE(dev_port);
  err = lld_sku_map_pipe_id_to_phy_pipe_id(dev_id, *log_pipe, phy_pipe);
  if (err != LLD_OK) {
    BF_PAL_ERROR(
        "Unable to get phy pipe from log pipe for dev %d : log pipe:%d  \n",
        dev_id,
        *log_pipe);
    return BF_INTERNAL_ERROR;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_speed_get(bf_dev_id_t dev_id,
                                  bf_dev_port_t dev_port,
                                  bf_port_speed_t *speed) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return bf_pm_port_speed_get(dev_id, &port_hdl, speed);
}

bf_status_t bf_pal_port_is_enabled(bf_dev_id_t dev_id,
                                   bf_dev_port_t dev_port,
                                   bool *is_enabled) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return bf_pm_port_is_enabled(dev_id, &port_hdl, is_enabled);
}

bf_status_t bf_pal_port_serdes_tx_eq_pre_get(bf_dev_id_t dev_id,
                                             bf_dev_port_t dev_port,
                                             int *tx_pre) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return bf_pm_port_serdes_tx_eq_pre_get(dev_id, &port_hdl, tx_pre);
}

bf_status_t bf_pal_port_serdes_tx_eq_post2_get(bf_dev_id_t dev_id,
                                               bf_dev_port_t dev_port,
                                               int *tx_post2) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return bf_pm_port_serdes_tx_eq_post2_get(dev_id, &port_hdl, tx_post2);
}

bf_status_t bf_pal_port_serdes_tx_eq_pre2_get(bf_dev_id_t dev_id,
                                              bf_dev_port_t dev_port,
                                              int *tx_pre2) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return bf_pm_port_serdes_tx_eq_pre2_get(dev_id, &port_hdl, tx_pre2);
}

bf_status_t bf_pal_port_serdes_tx_eq_post_get(bf_dev_id_t dev_id,
                                              bf_dev_port_t dev_port,
                                              int *tx_post) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return bf_pm_port_serdes_tx_eq_post_get(dev_id, &port_hdl, tx_post);
}

bf_status_t bf_pal_port_serdes_tx_eq_attn_get(bf_dev_id_t dev_id,
                                              bf_dev_port_t dev_port,
                                              int *tx_attn) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return bf_pm_port_serdes_tx_eq_main_get(dev_id, &port_hdl, tx_attn);
}

bf_status_t bf_pal_fp_idx_get_first(bf_dev_id_t dev_id, uint32_t *fp_idx) {
  return bf_pm_front_port_index_get_first(dev_id, fp_idx);
}

bf_status_t bf_pal_fp_idx_get_next(bf_dev_id_t dev_id,
                                   uint32_t curr_idx,
                                   uint32_t *next_idx) {
  return bf_pm_front_port_index_get_next(dev_id, curr_idx, next_idx);
}

bf_status_t bf_pal_port_mac_stats_historical_get(
    bf_dev_id_t dev_id, bf_dev_port_t dev_port, bf_rmon_counter_array_t *data) {
  return bf_port_mac_stats_historical_get(dev_id, dev_port, data);
}

bf_status_t bf_pal_port_mac_stats_historical_set(
    bf_dev_id_t dev_id, bf_dev_port_t dev_port, bf_rmon_counter_array_t *data) {
  return bf_port_mac_stats_historical_set(dev_id, dev_port, data);
}

bf_status_t bf_pal_interrupt_based_link_monitoring_get(bf_dev_id_t dev_id,
                                                       bool *en) {
  return bf_pm_interrupt_based_link_monitoring_get(dev_id, en);
}

uint32_t bf_pal_recirc_devports_get(bf_dev_id_t dev_id,
                                    uint32_t *recirc_devport_list) {
  return bf_pm_recirc_devports_get(dev_id, recirc_devport_list);
}

bf_status_t bf_pal_port_1588_timestamp_delta_tx_set(bf_dev_id_t dev_id,
                                                    bf_dev_port_t dev_port,
                                                    uint16_t delta) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_1588_timestamp_delta_tx_set(dev_id, &port_hdl, delta);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to set 1588 tx delta for dev : %d : front port : %d/%d (%d) : "
        "%s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_1588_timestamp_delta_tx_get(bf_dev_id_t dev_id,
                                                    bf_dev_port_t dev_port,
                                                    uint16_t *delta) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_1588_timestamp_delta_tx_get(dev_id, &port_hdl, delta);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get 1588 tx delta for dev : %d : front port : %d/%d (%d) : "
        "%s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_1588_timestamp_delta_rx_set(bf_dev_id_t dev_id,
                                                    bf_dev_port_t dev_port,
                                                    uint16_t delta) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_1588_timestamp_delta_rx_set(dev_id, &port_hdl, delta);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to set 1588 rx delta for dev : %d : front port : %d/%d (%d) : "
        "%s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_1588_timestamp_delta_rx_get(bf_dev_id_t dev_id,
                                                    bf_dev_port_t dev_port,
                                                    uint16_t *delta) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_1588_timestamp_delta_rx_get(dev_id, &port_hdl, delta);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get 1588 rx delta for dev : %d : front port : %d/%d (%d) : "
        "%s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_1588_timestamp_get(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           uint64_t *ts,
                                           bool *ts_valid,
                                           int *ts_id) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  sts = bf_pm_port_1588_timestamp_get(dev_id, &port_hdl, ts, ts_valid, ts_id);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get 1588 timestamp for dev : %d : front port : %d/%d (%d) : "
        "%s (%d)",
        dev_id,
        port_hdl.conn_id,
        port_hdl.chnl_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pal_port_speed_set(bf_dev_id_t dev_id,
                                  bf_dev_port_t dev_port,
                                  bf_port_speed_t speed) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return bf_pm_port_speed_set(dev_id, &port_hdl, speed);
}

bf_status_t bf_pal_port_speed_with_lanes_set(bf_dev_id_t dev_id,
                                             bf_dev_port_t dev_port,
                                             bf_port_speed_t speed,
                                             uint32_t n_lanes) {
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  sts =
      bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (sts != BF_SUCCESS) {
    BF_PAL_ERROR(
        "Unable to get the front port handle for dev : %d : dev_port : %d : %s "
        "(%d)",
        dev_id,
        dev_port,
        bf_err_str(sts),
        sts);
    return sts;
  }

  return bf_pm_port_speed_set_with_lanes(dev_id, &port_hdl, speed, n_lanes);
}

bf_status_t bf_pal_get_serdes_mode(bf_dev_id_t dev_id,
                                   bf_dev_port_t dev_port,
                                   bf_port_serdes_mode_t *mode) {
  if (mode == NULL) {
    return BF_INVALID_ARG;
  }

  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));
  if (dev_family == BF_DEV_FAMILY_TOFINO3) {
    int nserdes_per_mac = lld_get_num_serdes_per_mac(dev_id, dev_port);
    switch (nserdes_per_mac) {
      case 4:
        *mode = BF_SERDES_MODE_112G;
        break;
      case 8:
        *mode = BF_SERDES_MODE_56G;
        break;
      default:
        return BF_INTERNAL_ERROR;
    }
    return BF_SUCCESS;
  } else {
    *mode = BF_SERDES_MODE_56G;
    return BF_SUCCESS;
  }
}

bf_status_t bf_pal_get_serdes_lane_count_per_mac(bf_dev_id_t dev_id,
                                                 bf_dev_port_t dev_port,
                                                 uint32_t *serdes_count) {
  if (serdes_count == NULL) return BF_INVALID_ARG;
  *serdes_count = lld_get_num_serdes_per_mac(dev_id, dev_port);
  return BF_SUCCESS;
}

bf_status_t bf_pal_is_special_port(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  uint32_t num_pipes;

  lld_sku_get_num_active_pipes(dev_id, &num_pipes);
  uint32_t start_pipe = DEV_PORT_TO_PIPE(dev_port);
  uint32_t local_port = DEV_PORT_TO_LOCAL_PORT(dev_port);

  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));
  if (dev_family == BF_DEV_FAMILY_TOFINO) {
    if (start_pipe < num_pipes) {
      if (local_port >= 64 && local_port < 72) {
        return BF_SUCCESS;
      }
    }
  }
  if (dev_family == BF_DEV_FAMILY_TOFINO2) {
    if (start_pipe < num_pipes) {
      if (local_port < 8) {
        return BF_SUCCESS;
      }
    }
  }
  if (dev_family == BF_DEV_FAMILY_TOFINO3) {
    if (start_pipe < num_pipes) {
      if (local_port == 0 || local_port == 2 || local_port == 4 ||
          local_port == 6) {
        return BF_SUCCESS;
      }
    }
  }
  return BF_INVALID_ARG;
}
