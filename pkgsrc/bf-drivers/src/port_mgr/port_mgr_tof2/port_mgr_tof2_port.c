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


#include <target-sys/bf_sal/bf_sys_intf.h>
#include <dvm/bf_drv_intf.h>
#include <pipe_mgr/pktgen_intf.h>
#include <dvm/dvm_intf.h>
#include <lld/lld_err.h>
#include <lld/lld_sku.h>
#include <port_mgr/bf_fsm_if.h>
#include <port_mgr/bf_port_if.h>
#include <port_mgr/bf_tof2_serdes_if.h>
#include <port_mgr/port_mgr_intf.h>
#include <port_mgr/port_mgr_ha.h>
#include <port_mgr/port_mgr.h>
#include <port_mgr/port_mgr_dev.h>
#include <port_mgr/port_mgr_map.h>
#include <port_mgr/port_mgr_log.h>
#include <port_mgr/port_mgr_mac_stats.h>
#include <port_mgr/port_mgr_port.h>
#include "bf_types/bf_types.h"
#include "port_mgr_tof2_map.h"
#include "port_mgr_tof2_port.h"
#include "port_mgr_tof2_umac.h"
#include "port_mgr_tof2_serdes.h"

#include "umac4_ctrs.h"
#include "port_mgr_tof2_umac4.h"
#include "../port_mgr_mac_stats.h"

static bf_status_t port_mgr_tof2_default_config(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    bf_port_attributes_t *port_attrib);
/** \brief port_mgr_tof2_port_add
 *         Add a Tofino2 port to the system
 *         note: dvm will call this API twice, first for ingress, then for
 *egress.
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: dev_port
 *
 * \return: BF_SUCCESS     : port added successfully
 * \return: BF_INVALID_ARG : invalid dev_id
 * \return: BF_INVALID_ARG : invalid_dev_port
 * \return: BF_INVALID_ARG : resources in-use
 *
 */
bf_status_t port_mgr_tof2_port_add(bf_dev_id_t dev_id,
                                   bf_dev_port_t dev_port,
                                   bf_port_attributes_t *port_attrib,
                                   bf_port_cb_direction_t direction) {
  port_mgr_err_t sts;
  int umac, ch, port_id;
  int is_cpu_port = false;
  port_mgr_port_t *port_p;
  bool is_valid;
  bf_status_t rc;

  if (port_attrib == NULL) return BF_INVALID_ARG;

  if ((direction != BF_PORT_CB_DIRECTION_INGRESS) &&
      (direction != BF_PORT_CB_DIRECTION_EGRESS))
    return BF_INVALID_ARG;

  // Check for ports port_mgr really handles. This callback gets called for
  // any port_add, including recirc, etc. Just ignore those.
  sts = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, &port_id, &umac, &ch, &is_cpu_port);
  if (sts != PORT_MGR_OK) {
    return BF_SUCCESS;
  }

  port_mgr_log("PRT :%d:%3d:-: Add <spd=%d : n_lanes=%d : dir=%d ha-stage=%d>",
               dev_id,
               dev_port,
               port_attrib->port_speeds,
               port_attrib->n_lanes,
               direction,
               port_mgr_dev_ha_stage_get(dev_id));

// Hack until CPU port is supported.
#if defined(DEVICE_IS_EMULATOR)  // Emulator
  if (is_cpu_port) return BF_SUCCESS;
#endif
  // ignore non-MAC ports
  if ((port_id < 8) && (!is_cpu_port)) return BF_SUCCESS;

  // validate speed/fec combination
  rc = bf_port_fec_type_validate(dev_id,
                                 dev_port,
                                 port_attrib->port_speeds,
                                 port_attrib->port_fec_types,
                                 &is_valid);
  if ((rc != BF_SUCCESS) || (!is_valid)) {
    return BF_INVALID_ARG;
  }

  port_p = port_mgr_map_dev_port_to_port_allow_unassigned(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;
  port_p->sw.speed = port_attrib->port_speeds;
  port_p->sw.fec = port_attrib->port_fec_types;

  switch (port_mgr_dev_ha_stage_get(dev_id)) {
    case PORT_MGR_HA_CFG_REPLAY:
      if (direction == BF_PORT_CB_DIRECTION_INGRESS) {
        /* During warm init, fill up the port_p->sw during EGRESS call only*/
        return BF_SUCCESS;
      }
      return port_mgr_tof2_default_config(dev_id, dev_port, port_attrib);
    case PORT_MGR_HA_DELTA_PUSH:
      /* dvm will call this API twice, first for egress, then for ingress.
       * The port will be added on the first call (egress) but the
       * force_pfc_flush
       * bits will be left on until the second call, for ingress */
      if (direction == BF_PORT_CB_DIRECTION_INGRESS) {
        /* undo force clear pause state */
        // port_mgr_mac_pgm_force_pfc_flush(dev_id, mac_block, ch);
        return BF_SUCCESS;
      }
      /* verify required resources are uncommitted only when we are not in delta
         push phase because this would have already been populated during cfg
         replay. Hence if we check again during push delta, then this check will
         surely fail. We can assume that right things already exist in
         mac_block_p->ch_in_use and simply proceed */
      break;
    case PORT_MGR_HA_NONE:
      /* dvm will call this API twice, first for egress, then for ingress.
       * The port will be added on the first call (egress) but the
       * force_pfc_flush
       * bits will be left on until the second call, for ingress */
      if (direction == BF_PORT_CB_DIRECTION_INGRESS) {
        /* undo force clear pause state */
        // port_mgr_mac_pgm_force_pfc_flush(dev_id, mac_block, ch);
        return BF_SUCCESS;
      }

      rc = port_mgr_tof2_default_config(dev_id, dev_port, port_attrib);
      if (rc != BF_SUCCESS) return rc;

      break;
    case PORT_MGR_HA_DELTA_COMPUTE:
    case PORT_MGR_HA_MAX:
    default:
      port_mgr_log("Error : Invalid HA stage %d while trying to add a port",
                   port_mgr_dev_ha_stage_get(dev_id));
      return BF_INVALID_ARG;
  }
  port_mgr_tof2_umac_config(dev_id,
                            umac,
                            ch,
                            port_attrib->port_speeds,
                            port_attrib->port_fec_types,
                            port_attrib->n_lanes);
  return BF_SUCCESS;
}

static bf_status_t port_mgr_port_remove_basic_setup(bf_dev_id_t dev_id,
                                                    bf_dev_port_t dev_port,
                                                    port_mgr_port_t *port_p) {
  int is_cpu_port = false;
  port_mgr_err_t sts;
  int umac, ch, port_id;
  uint32_t ch_needed = 0;
  if (port_p == NULL) {
    port_mgr_log("%s:%d Map dev %d port %d to port_p failed",
                 __func__,
                 __LINE__,
                 dev_id,
                 dev_port);
    return BF_INVALID_ARG;
  }
  ch_needed = ((1 << port_p->sw.n_lanes) - 1);

  // Check for ports port_mgr really handles. This callback gets called for
  // any port_add, including recirc, etc. Just ignore those.
  sts = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, &port_id, &umac, &ch, &is_cpu_port);
  if (sts != PORT_MGR_OK) {
    port_mgr_log("%s:%d Map dev %d port %d failed ",
                 __func__,
                 __LINE__,
                 dev_id,
                 dev_port);

    return BF_SUCCESS;
  }

  // verify resources are available
  if (!is_cpu_port) {
    port_mgr_umac4_t *umac4_p;

    umac4_p = port_mgr_tof2_map_dev_port_lane_to_umac4(dev_id, dev_port);
    if (umac4_p == NULL) {
      port_mgr_log("%s:%d Map dev %d port %d to umac4 failed",
                   __func__,
                   __LINE__,
                   dev_id,
                   dev_port);
      return BF_INVALID_ARG;
    }
    if ((umac4_p->ch_in_use & (ch_needed << ch)) != (ch_needed << ch)) {
      port_mgr_log(
          "dev %d port %d umac4 ch %d already in use", dev_id, dev_port, ch);
      return BF_INVALID_ARG;
    }
    umac4_p->ch_in_use &= ~(ch_needed << ch);
  } else {
    port_mgr_umac3_t *umac3_p;

    umac3_p = port_mgr_tof2_map_dev_port_lane_to_umac3(dev_id, dev_port);
    if (umac3_p == NULL) {
      port_mgr_log("%s:%d Map dev %d port %d to umac3 failed",
                   __func__,
                   __LINE__,
                   dev_id,
                   dev_port);
      return BF_INVALID_ARG;
    }
    if ((umac3_p->ch_in_use & (ch_needed << ch)) != (ch_needed << ch)) {
      port_mgr_log(
          "dev %d port %d umac3 ch %d already in use", dev_id, dev_port, ch);
      return BF_INVALID_ARG;
    }
    umac3_p->ch_in_use &= ~(ch_needed << ch);
  }

  // reset port speed and fec mode
  port_p->sw.speed = BF_SPEED_NONE;
  port_p->sw.fec = BF_FEC_TYP_NONE;

  port_p->sw.assigned = 0;
  return BF_SUCCESS;
}

/** \brief port_mgr_tof2_port_remove
 *         Remove a Tofino port from the system
 *         note: dvm will call this API twice, first for ingress, then for
 *egress.
 *
 * \param dev_id: int : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param port: int : physical port # on dev_id (0..LLD_MAX_PORTS-1)
 *
 * \return: BF_SUCCESS : port removed successfully
 *
 */
bf_status_t port_mgr_tof2_port_remove(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      bf_port_cb_direction_t direction) {
  int umac, ch, is_cpu_port, port_id;
  port_mgr_port_t *port_p;
  port_mgr_dev_t *dev_p = port_mgr_map_dev_id_to_dev_p_allow_unassigned(dev_id);
  if (dev_p == NULL) return BF_INVALID_ARG;

  port_mgr_err_t err = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, &port_id, &umac, &ch, &is_cpu_port);
  if (err) {
    // ignore non-MAC ports
    return BF_SUCCESS;
  }

  if (port_mgr_dev_ha_stage_get(dev_id) == PORT_MGR_HA_DELTA_PUSH) {
    port_p = port_mgr_map_dev_port_to_port_allow_unassigned(dev_id, dev_port);
  } else {
    port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  }
  if (!port_p) return BF_INVALID_ARG;

  // make sure its a real mac
  if ((port_id < 8) && (!is_cpu_port)) {
    // ignore non-MAC ports
    return BF_SUCCESS;
  }
  switch (port_mgr_dev_ha_stage_get(dev_id)) {
    case PORT_MGR_HA_CFG_REPLAY:
      if (direction == BF_PORT_CB_DIRECTION_INGRESS) {
        /* During warm init, do nothing during the INGRESS call*/
        return BF_SUCCESS;
      }

      port_mgr_port_remove_basic_setup(dev_id, dev_port, port_p);
      return BF_SUCCESS;
    case PORT_MGR_HA_DELTA_PUSH:
      /* consider the case when a port is being deleted because it was already
         in the hw but was never replayed back. In this case the port_p->sw
         data structure would be invalid. Hence we need to go by the port_p->hw
         data strcuture */
      // if (!port_p->hw.assigned) {
      /* Indicates that the port was never added in the past as well. Not sure
         if this condition will ever occur */
      return BF_SUCCESS;
      //}
#if 0
      if (direction == BF_PORT_CB_DIRECTION_INGRESS) {
        /* If the port was enabled in hw, then disable it before removing */
        // if (port_p->hw.enabled) { /* we check hw instead of sw*/
        //  port_mgr_port_disable(dev_id, dev_port);
        //}
        // update txff_ctrl, set force flush mode
        // mac_block_p->txff_ctrl = port_mgr_construct_txff_ctrl(dev_id,
        // dev_port);
        // port_mgr_mac_pgm_txff_ctrl(dev_id, mac_block);
        return BF_SUCCESS;
      } else {
        // determine freed resources (mac/serdes)
        // ch_reqd = port_mgr_ch_reqd_by_speed(dev_id, dev_port,
        // port_p->hw.speed);
      }

      break;
#endif
    case PORT_MGR_HA_NONE:
      // PORT-SEQ If direction == BF_PORT_CB_DIRECTION_INGRESS port-disable,
      // enable flush in MAC
      // PORT-SEQ If direction == BF_PORT_CB_DIRECTION_EGRESS disable MAC
      // channel , Disable Flush
      if (direction == BF_PORT_CB_DIRECTION_INGRESS) {
        // update txff_ctrl, set force flush mode
        // mac_block_p->txff_ctrl = port_mgr_construct_txff_ctrl(dev_id,
        // dev_port);
        // port_mgr_mac_pgm_txff_ctrl(dev_id, mac_block);
        return BF_SUCCESS;
      } else {
#if 1
        // if enabled, disable before removing
        if (port_p->sw.enabled) {
          port_mgr_tof2_port_disable(dev_id, dev_port);
        }
#endif
        // determine freed resources (mac/serdes)
        port_mgr_port_remove_basic_setup(dev_id, dev_port, port_p);
      }
      break;
    case PORT_MGR_HA_DELTA_COMPUTE:
    case PORT_MGR_HA_MAX:
    default:
      port_mgr_log("Error : Invalid HA stage %d while trying to remove a port",
                   port_mgr_dev_ha_stage_get(dev_id));
      return BF_INVALID_ARG;
  }
  port_mgr_tof2_umac_de_config(dev_id, umac, ch);

  bool persist_enable = false;
  port_mgr_stats_persistent_get(dev_id, &persist_enable);
  if (persist_enable == false) {
    port_mgr_mac_stats_clear_sw_ctrs(&port_p->mac_stat_historical);
  }

  return BF_SUCCESS;
}

/** \brief port_mgr_tof2_port_enable
 *         Enable a Tofino port. This kicks off the port
 *         bring-up sequence.
 *
 * [ PRE_ENABLE ]
 *
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: dev_port
 * \param enable  : true= Enable, false= Disable
 *
 * \return: BF_SUCCESS     : port enabled successfully
 * \return: BF_INVALID_ARG : dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG : port > LLD_MAX_PORTS-1
 *
 */
bf_status_t port_mgr_tof2_port_enable(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      bool enable) {
  port_mgr_log(
      "%s:%d:%d:%d:%d(enb)", __func__, __LINE__, dev_id, dev_port, enable);
  port_mgr_tof2_pdev_t *dev_p = port_mgr_dev_physical_dev_tof2_get(dev_id);

  port_mgr_log(
      "PRT :%d:%3d:-: Raw %s", dev_id, dev_port, enable ? "Enable" : "Disable");

  if (!dev_p) return BF_INVALID_ARG;

  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (!port_p) return BF_INVALID_ARG;
  int ch, is_cpu_port, port_id;
  uint32_t umac;

  port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, &port_id, (int *)&umac, &ch, &is_cpu_port);

  // make sure its a real mac
  if ((port_id < 8) && (!is_cpu_port)) {
    // ignore non-MAC ports
    return BF_SUCCESS;
  }

  port_mgr_log("PRT :%d:%3d:-: %s ha-stage %d",
               dev_id,
               dev_port,
               enable ? "Enable" : "Disable",
               port_mgr_dev_ha_stage_get(dev_id));

  if (!enable) {
    return port_mgr_tof2_port_disable(dev_id, dev_port);
  }

  switch (port_mgr_dev_ha_stage_get(dev_id)) {
    case PORT_MGR_HA_CFG_REPLAY:
      port_p->sw.enabled = 1;
      return BF_SUCCESS;
    case PORT_MGR_HA_DELTA_PUSH:
      // We will not enter this state unless corrective action
      // is required. So program normally.
    case PORT_MGR_HA_NONE:
      port_p->sw.enabled = 1;
      port_mgr_tof2_umac_enable(dev_id, umac, ch);
      break;
    case PORT_MGR_HA_DELTA_COMPUTE:
    case PORT_MGR_HA_MAX:
    default:
      port_mgr_log("Error : Invalid HA stage %d while trying to enable a port",
                   port_mgr_dev_ha_stage_get(dev_id));
      return BF_INVALID_ARG;
  }
  return BF_SUCCESS;
}

/** \brief port_mgr_tof2_port_disable
 *         Disable a Tofino port. This brings the port down
 *         and disables the transmitter, bringing the link-
 *         partner down.
 *
 * \param dev_id: int         : system-assigned identifier
 *(0..BF_MAX_DEV_COUNT-1)
 * \param port: int         : physical port # on dev_id (0..LLD_MAX_PORTS-1)
 *
 * \return: BF_SUCCESS     : port disabled successfully
 * \return: BF_INVALID_ARG : dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG : port > LLD_MAX_PORTS-1
 *
 */
bf_status_t port_mgr_tof2_port_disable(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port) {
  int mac_block, ch, is_cpu_port, port_id;
  uint32_t umac4;
  port_mgr_port_t *port_p;

  port_mgr_dev_t *dev_p = port_mgr_map_dev_id_to_dev_p_allow_unassigned(dev_id);
  if (!dev_p) return BF_INVALID_ARG;

  port_p = port_mgr_map_dev_port_to_port_allow_unassigned(dev_id, dev_port);
  if (!port_p) return BF_INVALID_ARG;

  if (port_mgr_dev_ha_stage_get(dev_id) == PORT_MGR_HA_DELTA_PUSH) {
  } else {
    // port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  }

  port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, &port_id, &mac_block, &ch, &is_cpu_port);

  // make sure its a real mac
  if ((port_id < 8) && (!is_cpu_port)) {
    // ignore non-MAC ports
    return BF_SUCCESS;
  }

  switch (port_mgr_dev_ha_stage_get(dev_id)) {
    case PORT_MGR_HA_CFG_REPLAY:
      return BF_SUCCESS;
    case PORT_MGR_HA_DELTA_PUSH:
    case PORT_MGR_HA_NONE: {
      // de-assert "force tx RF" which is set by some FSMs during
      // intermediate states
      bf_port_force_remote_fault_set(dev_id, dev_port, false);
      if (port_p->sw.enabled) {
        // disconnect UMAC from serdes Rx
        bf_port_force_sig_ok_low_set(dev_id, dev_port);
      }
      /* archive HW stats as they will be cleared during rx_reset */
      bf_port_mac_stats_historical_update_set(dev_id, dev_port);

      umac4 = mac_block;
      port_mgr_tof2_umac_disable(dev_id, umac4, ch);

      if (port_p->sw.oper_state) {
        port_mgr_link_dn_actions(dev_id, dev_port);
      }
      port_p->sw.enabled = 0;
      port_p->sw.oper_state = 0;
    } break;
    case PORT_MGR_HA_DELTA_COMPUTE:
    case PORT_MGR_HA_MAX:
    default:
      port_mgr_log("Error : Invalid HA stage %d while trying to add a port",
                   port_mgr_dev_ha_stage_get(dev_id));
      return BF_INVALID_ARG;
  }
  return BF_SUCCESS;
}

/** \brief port_mgr_tof2_port_oper_state_get
 *         Get the operational stateof a Tof2 port.
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: physical port # on dev_id (0..LLD_MAX_PORTS-1)
 * \param up      : operational state, false=down, true=up
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG : dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG : port > LLD_MAX_PORTS-1
 *
 */
bf_status_t port_mgr_tof2_port_oper_state_get(bf_dev_id_t dev_id,
                                              bf_dev_port_t dev_port,
                                              bool *up) {
  int is_cpu_port, port_id;
  int umac, ch;
  port_mgr_port_t *port_p;
  bf_status_t rc;

  // Ignore recirc port.
  bool recirc = false;
  rc = bf_recirculation_get(dev_id, dev_port, &recirc);
  if (rc == BF_SUCCESS && recirc) {
    return BF_SUCCESS;
  }

  port_p = port_mgr_map_dev_port_to_port_allow_unassigned(dev_id, dev_port);
  // if no port defined, its an error
  if (!port_p) {
    *up = false;
    return BF_INVALID_ARG;
  }
  // if port defined but not enabled, just call it down
  if (!port_p->sw.enabled) {
    *up = false;
    return BF_SUCCESS;
  }

  rc = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, &port_id, &umac, &ch, &is_cpu_port);
  if (rc != BF_SUCCESS) {
    *up = false;
    return BF_INVALID_ARG;
  }

  // make sure its a real mac
  if ((port_id < 8) && (!is_cpu_port)) {
    // ignore non-MAC ports
    return BF_SUCCESS;
  }
  port_mgr_tof2_umac_link_state_get(dev_id, umac, ch, up);
  return BF_SUCCESS;
}

/** \brief port_mgr_tof2_port_oper_state_extended_get
 *         Get the extended operational state of a Tof2 port.
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: physical port # on dev_id (0..LLD_MAX_PORTS-1)
 * \param up      : operational state, false=down, true=up
 * \param pcs_rdy : true if pcs has am lock
 * \param l_fault : true if pcs is receiving local fault ordered set.
 * \param r_fault : true if pcs is receiving remote fault ordered set.
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG : dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG : port > LLD_MAX_PORTS-1
 *
 */
bf_status_t port_mgr_tof2_port_oper_state_extended_get(bf_dev_id_t dev_id,
                                                       bf_dev_port_t dev_port,
                                                       bool *up,
                                                       bool *pcs_ready,
                                                       bool *l_fault,
                                                       bool *r_fault) {
  port_mgr_port_t *port_p;
  bool remote_fault;
  bool local_fault;
  int is_cpu_port;
  bf_status_t rc;
  int port_id;
  int umac;
  int ch;

  if (!up || !pcs_ready) return BF_INVALID_ARG;

  /* set default values */
  *up = false;
  *pcs_ready = false;
  if (l_fault) *l_fault = false;
  if (r_fault) *r_fault = false;

  // Ignore recirc port.
  bool recirc = false;
  rc = bf_recirculation_get(dev_id, dev_port, &recirc);
  if (rc == BF_SUCCESS && recirc) {
    return BF_SUCCESS;
  }

  port_p = port_mgr_map_dev_port_to_port_allow_unassigned(dev_id, dev_port);
  // if no port defined, its an error
  if (!port_p) return BF_INVALID_ARG;

  // if port defined but not enabled, just call it down
  if (!port_p->sw.enabled) return BF_SUCCESS;

  rc = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, &port_id, &umac, &ch, &is_cpu_port);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;

  // make sure its a real mac
  if ((port_id < 8) && (!is_cpu_port)) return BF_SUCCESS;

  port_mgr_tof2_umac_link_state_get(dev_id, umac, ch, up);

  port_mgr_tof2_umac_link_fault_get(
      dev_id, umac, ch, pcs_ready, &local_fault, &remote_fault);

  if (l_fault) *l_fault = local_fault;
  if (local_fault) remote_fault = 0;
  if (r_fault) *r_fault = remote_fault;

  return BF_SUCCESS;
}

/**************************************************************************
 * port_mgr_tof2_port_loopback_set
 **************************************************************************/
bf_status_t port_mgr_tof2_port_loopback_set(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port,
                                            bf_loopback_mode_e mode) {
  int umac, ch;
  bf_status_t rc;

  // Ignore recirc port.
  bool recirc = false;
  rc = bf_recirculation_get(dev_id, dev_port, &recirc);
  if (rc == BF_SUCCESS && recirc) {
    return BF_SUCCESS;
  }

  switch (mode) {
    case BF_LPBK_NONE:
    case BF_LPBK_MAC_NEAR:
    case BF_LPBK_MAC_FAR:
    case BF_LPBK_PCS_NEAR:
    case BF_LPBK_PIPE:
      break;
    default:
      return BF_INVALID_ARG;
  }
  rc = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch, NULL);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;

  port_mgr_tof2_umac_loopback_set(dev_id, umac, ch, mode);
  return BF_SUCCESS;
}

/**************************************************************************
 * port_mgr_tof2_port_tx_ignore_rx_set
 **************************************************************************/
bf_status_t port_mgr_tof2_port_tx_ignore_rx_set(bf_dev_id_t dev_id,
                                                bf_dev_port_t dev_port,
                                                bool en) {
  int umac, ch;
  bf_status_t rc;

  rc = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch, NULL);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;

  port_mgr_tof2_umac_tx_ignore_rx_set(dev_id, umac, ch, en);
  return BF_SUCCESS;
}

/**************************************************************************
 * port_mgr_tof2_set_default_port_state_itr
 **************************************************************************/
static bf_status_t port_mgr_tof2_set_default_port_state_itr(
    bf_dev_id_t dev_id, bf_dev_port_t dev_port, void *unused) {
  port_mgr_port_t *port_p =
      port_mgr_map_dev_port_to_port_allow_unassigned(dev_id, dev_port);

  if (port_p) port_mgr_set_default_port_state(dev_id, dev_port);

  (void)unused;
  return BF_SUCCESS;
}

/**************************************************************************
 * port_mgr_tof2_default_config
 *
 * Set default config for port-add
 **************************************************************************/
static bf_status_t port_mgr_tof2_default_config(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    bf_port_attributes_t *port_attrib) {
  int is_cpu_port = false;
  port_mgr_err_t sts;
  int umac, ch, port_id;
  port_mgr_port_t *port_p =
      port_mgr_map_dev_port_to_port_allow_unassigned(dev_id, dev_port);

  if (port_p == NULL) {
    port_mgr_log("%s:%d Map dev %d port %d to port_p failed",
                 __func__,
                 __LINE__,
                 dev_id,
                 dev_port);
    return BF_INVALID_ARG;
  }

  // Check for ports port_mgr really handles. This callback gets called for
  // any port_add, including recirc, etc. Just ignore those.
  sts = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, &port_id, &umac, &ch, &is_cpu_port);
  if (sts != PORT_MGR_OK) {
    port_mgr_log("%s:%d Map dev %d port %d failed ",
                 __func__,
                 __LINE__,
                 dev_id,
                 dev_port);

    return BF_SUCCESS;
  }

  // init all sw config to defaults
  port_mgr_set_default_port_state(dev_id, dev_port);
  // verify resources are available
  if (!is_cpu_port) {
    port_mgr_umac4_t *umac4_p;
    uint32_t ch_needed = ((1 << port_attrib->n_lanes) - 1);

    umac4_p = port_mgr_tof2_map_dev_port_lane_to_umac4(dev_id, dev_port);
    if (umac4_p == NULL) {
      port_mgr_log("%s:%d Map dev %d port %d to umac4 failed",
                   __func__,
                   __LINE__,
                   dev_id,
                   dev_port);
      return BF_INVALID_ARG;
    }
    if ((umac4_p->ch_in_use & (ch_needed << ch)) != 0) {
      port_mgr_log(
          "dev %d port %d umac4 ch %d already in use", dev_id, dev_port, ch);
      return BF_INVALID_ARG;
    }

    umac4_p->ch_in_use |= (ch_needed << ch);
  } else {
    port_mgr_umac3_t *umac3_p;
    uint32_t ch_needed = ((1 << port_attrib->n_lanes) - 1);

    umac3_p = port_mgr_tof2_map_dev_port_lane_to_umac3(dev_id, dev_port);
    if (umac3_p == NULL) {
      port_mgr_log("%s:%d Map dev %d port %d to umac3 failed",
                   __func__,
                   __LINE__,
                   dev_id,
                   dev_port);
      return BF_INVALID_ARG;
    }
    if ((umac3_p->ch_in_use & (ch_needed << ch)) != 0) {
      port_mgr_log(
          "dev %d port %d umac3 ch %d already in use", dev_id, dev_port, ch);
      return BF_INVALID_ARG;
    }
    umac3_p->ch_in_use |= (ch_needed << ch);
  }

  // configuration is valid
  port_p->sw.assigned = 1;
  // speed must be set in port for the txff_ctrl calculation (below)
  port_p->sw.speed = port_attrib->port_speeds;
  port_p->sw.fec = port_attrib->port_fec_types;

  // clear historical counter stats
  // port_mgr_tof2_mac_stats_clear_historical_ctrs(&port_p->mac_stat_historical);
  return BF_SUCCESS;
}

/*****************************************************************************
 * port_mgr_tof2_port_read_counter
 *
 * Sync read of UMAC RMON counter
 ****************************************************************************/
bf_status_t port_mgr_tof2_port_read_counter(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port,
                                            bf_rmon_counter_t ctr_id,
                                            uint64_t *ctr_value) {
  int umac, ch;
  int is_cpu_port = false;

  port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch, &is_cpu_port);

  return port_mgr_tof2_umac_read_counter(dev_id, umac, ch, ctr_id, ctr_value);
}

/*****************************************************************************
 * port_mgr_tof2_port_mac_stats_clear
 *
 * Clear HW stats
 ****************************************************************************/
bf_status_t port_mgr_tof2_port_mac_stats_clear(bf_dev_id_t dev_id,
                                               bf_dev_port_t dev_port) {
  int umac, ch;
  int is_cpu_port = false;

  port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch, &is_cpu_port);

  return port_mgr_tof2_umac_clear_counter(dev_id, umac, ch);
}
/**************************************************************************
 * port_mgr_tof2_port_iter
 *
 * iterate over all logical ports calling the passed fn for each
 **************************************************************************/
bf_status_t port_mgr_tof2_port_iter(bf_dev_id_t dev_id,
                                    port_mgr_tof2_iter_cb fn,
                                    void *userdata) {
  int rc, port;
  uint32_t pipe, num_pipes;
  bf_dev_port_t dev_port;

  rc = lld_sku_get_num_active_pipes(dev_id, &num_pipes);
  if (rc != 0) return BF_INVALID_ARG;

  for (pipe = 0; pipe < num_pipes; pipe++) {
    for (port = 0; port < 72; port++) {
      dev_port = MAKE_DEV_PORT(pipe, port);
      fn(dev_id, dev_port, userdata);
    }
  }
  return BF_SUCCESS;
}

/*****************************************************************************
 * port_mgr_tof2_port_lane_map_set
 *
 * Save and program the lane map in both UMAC and serdes. The UMAC need only
 * be programmed once, at init. The serdes lane map would be lost if a group
 * reset is issued (which is required for new FW dnld). So we also save the
 * lane map here so the serdes can retreive it if needed.
 ****************************************************************************/
bf_status_t port_mgr_tof2_port_lane_map_set(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port,
                                            uint32_t phys_tx_ln[8],
                                            uint32_t phys_rx_ln[8]) {
  int umac, ch, port_id, ln;
  int is_cpu_port, n_lanes;
  bf_status_t rc;
  port_mgr_tof2_pdev_t *dev_p = port_mgr_dev_physical_dev_tof2_get(dev_id);
  uint32_t tile_tx_ln[8], tile_rx_ln[8];

  // Check for ports port_mgr really handles. This callback gets called for
  // any port_add, including recirc, etc. Just ignore those.
  rc = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, &port_id, &umac, &ch, &is_cpu_port);
  if (rc != PORT_MGR_OK) {
    return BF_SUCCESS;
  }

// Hack until CPU port is supported.
#if defined(DEVICE_IS_EMULATOR)  // Emulator
  if (is_cpu_port) return BF_SUCCESS;
#endif

  // ignore non-MAC ports
  if ((port_id < 8) && (!is_cpu_port)) return BF_SUCCESS;

  if (is_cpu_port) {
    n_lanes = 4;
    for (ln = 0; ln < n_lanes; ln++) {
      dev_p->umac3[0].phys_tx_ln[ln] = phys_tx_ln[ln];
      dev_p->umac3[0].phys_rx_ln[ln] = phys_rx_ln[ln];
      tile_tx_ln[ln] = 3 - phys_tx_ln[ln];
      tile_rx_ln[ln] = 3 - phys_rx_ln[ln];
    }
  } else {
    n_lanes = 8;
    for (ln = 0; ln < n_lanes; ln++) {
      dev_p->umac4[umac - 1].phys_tx_ln[ln] = phys_tx_ln[ln];
      dev_p->umac4[umac - 1].phys_rx_ln[ln] = phys_rx_ln[ln];
      if (((umac <= 8) && (umac >= 1)) || ((umac <= 24) && (umac >= 17))) {
        tile_tx_ln[ln] = 7 - phys_tx_ln[ln];
        tile_rx_ln[ln] = 7 - phys_rx_ln[ln];
      } else {
        tile_tx_ln[ln] = phys_tx_ln[ln];
        tile_rx_ln[ln] = phys_rx_ln[ln];
      }
    }
  }
  port_mgr_log("%d:%3d: umac:%0d: Serdes(Tile) and Physical Lane Map:",
               dev_id,
               dev_port,
               umac);
  if (is_cpu_port) {
    port_mgr_log("       -|-|-|-|3|2|1|0|");
    port_mgr_log("       -|-|-|-|%d|%d|%d|%d| Tx(tile)",
                 tile_tx_ln[3],
                 tile_tx_ln[2],
                 tile_tx_ln[1],
                 tile_tx_ln[0]);
    port_mgr_log("       -|-|-|-|%d|%d|%d|%d| Rx(tile)",
                 tile_rx_ln[3],
                 tile_rx_ln[2],
                 tile_rx_ln[1],
                 tile_rx_ln[0]);

    port_mgr_log("       -|-|-|-|%d|%d|%d|%d| Tx(phy)",
                 phys_tx_ln[3],
                 phys_tx_ln[2],
                 phys_tx_ln[1],
                 phys_tx_ln[0]);
    port_mgr_log("       -|-|-|-|%d|%d|%d|%d| Rx(phy)",
                 phys_rx_ln[3],
                 phys_rx_ln[2],
                 phys_rx_ln[1],
                 phys_rx_ln[0]);
  } else {
    port_mgr_log("       7|6|5|4|3|2|1|0|");
    port_mgr_log("       %d|%d|%d|%d|%d|%d|%d|%d| Tx(tile)",
                 tile_tx_ln[7],
                 tile_tx_ln[6],
                 tile_tx_ln[5],
                 tile_tx_ln[4],
                 tile_tx_ln[3],
                 tile_tx_ln[2],
                 tile_tx_ln[1],
                 tile_tx_ln[0]);
    port_mgr_log("       %d|%d|%d|%d|%d|%d|%d|%d| Rx(tile)",
                 tile_rx_ln[7],
                 tile_rx_ln[6],
                 tile_rx_ln[5],
                 tile_rx_ln[4],
                 tile_rx_ln[3],
                 tile_rx_ln[2],
                 tile_rx_ln[1],
                 tile_rx_ln[0]);
    port_mgr_log("       %d|%d|%d|%d|%d|%d|%d|%d| Tx(phy)",
                 phys_tx_ln[7],
                 phys_tx_ln[6],
                 phys_tx_ln[5],
                 phys_tx_ln[4],
                 phys_tx_ln[3],
                 phys_tx_ln[2],
                 phys_tx_ln[1],
                 phys_tx_ln[0]);
    port_mgr_log("       %d|%d|%d|%d|%d|%d|%d|%d| Rx(phy)",
                 phys_rx_ln[7],
                 phys_rx_ln[6],
                 phys_rx_ln[5],
                 phys_rx_ln[4],
                 phys_rx_ln[3],
                 phys_rx_ln[2],
                 phys_rx_ln[1],
                 phys_rx_ln[0]);
  }

  /* Don't touch hardware during cfg replay */
  if (port_mgr_dev_ha_stage_get(dev_id) == PORT_MGR_HA_CFG_REPLAY) {
    return BF_SUCCESS;
  }

  // set phys lane map in UMAC
  port_mgr_tof2_umac_lane_map_set(dev_id, umac, phys_tx_ln, phys_rx_ln);

  // set tile lane map in serdes
  rc = port_mgr_tof2_serdes_lane_map_set(
      dev_id, dev_port, tile_tx_ln, tile_rx_ln);
  return rc;
}

/*****************************************************************************
 * port_mgr_tof2_port_lane_map_get
 *
 * Retreive the lane map previously set (it is saved in the physical_device
 * struct with each umac.
 ****************************************************************************/
bf_status_t port_mgr_tof2_port_lane_map_get(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port,
                                            uint32_t phys_tx_ln[8],
                                            uint32_t phys_rx_ln[8]) {
  int umac, port_id, ln;
  int is_cpu_port;
  bf_status_t rc;
  port_mgr_tof2_pdev_t *dev_p = port_mgr_dev_physical_dev_tof2_get(dev_id);

  // Check for ports port_mgr really handles. This callback gets called for
  // any port_add, including recirc, etc. Just ignore those.
  rc = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, &port_id, &umac, NULL, &is_cpu_port);
  if (rc != PORT_MGR_OK) {
    return BF_SUCCESS;
  }

// Hack until CPU port is supported.
#if defined(DEVICE_IS_EMULATOR)  // Emulator
  if (is_cpu_port) return BF_SUCCESS;
#endif

  // ignore non-MAC ports
  if ((port_id < 8) && (!is_cpu_port)) return BF_SUCCESS;

  if (is_cpu_port) {
    for (ln = 0; ln < 4; ln++) {
      phys_tx_ln[ln] = dev_p->umac3[umac].phys_tx_ln[ln];
      phys_rx_ln[ln] = dev_p->umac3[umac].phys_rx_ln[ln];
    }
    return BF_SUCCESS;
  }
  for (ln = 0; ln < 8; ln++) {
    phys_tx_ln[ln] = dev_p->umac4[umac - 1].phys_tx_ln[ln];
    phys_rx_ln[ln] = dev_p->umac4[umac - 1].phys_rx_ln[ln];
  }
  return BF_SUCCESS;
}

/*****************************************************************************
 * port_mgr_tof2_get_num_lanes
 *
 * Internal function to return the number of "logical" lanes on a port.
 * Intention is to be called to determine "for loop" limits.
 *
 *****************************************************************************/
int port_mgr_tof2_get_num_lanes(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (!port_p) {
    return 0;
  } else {
    return port_p->sw.n_lanes;
  }
}

/*****************************************************************************
 * port_mgr_tof2_port_sigovrd_set
 *
 *****************************************************************************/
bf_status_t port_mgr_tof2_port_sigovrd_set(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           uint32_t num_lanes,
                                           bf_sigovrd_fld_t ovrd_val) {
  bf_status_t rc;
  int umac, ch;

  rc = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch, NULL);
  if (rc != PORT_MGR_OK) {
    return BF_SUCCESS;
  }
  port_mgr_tof2_umac_sigovrd_set(dev_id, umac, ch, num_lanes, ovrd_val);
  return BF_SUCCESS;
}

/*****************************************************************************
 * port_mgr_tof2_port_tx_reset_set
 *
 *****************************************************************************/
bf_status_t port_mgr_tof2_port_tx_reset_set(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port) {
  bf_status_t rc;
  int umac, ch;

  rc = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch, NULL);
  if (rc != PORT_MGR_OK) {
    return BF_SUCCESS;
  }
  port_mgr_tof2_umac_tx_reset_set(dev_id, umac, ch);
  return BF_SUCCESS;
}

/*****************************************************************************
 * port_mgr_tof2_port_rx_reset_set
 *
 *****************************************************************************/
bf_status_t port_mgr_tof2_port_rx_reset_set(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port) {
  bf_status_t rc;
  int umac, ch;

  rc = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch, NULL);
  if (rc != PORT_MGR_OK) {
    return BF_SUCCESS;
  }
  port_mgr_tof2_umac_rx_reset_set(dev_id, umac, ch);
  return BF_SUCCESS;
}

/*****************************************************************************
 * port_mgr_tof2_set_default_all_ports_state
 *
 *****************************************************************************/
bf_status_t port_mgr_tof2_set_default_all_ports_state(bf_dev_id_t dev_id) {
  bf_status_t rc = port_mgr_tof2_port_iter(
      dev_id, port_mgr_tof2_set_default_port_state_itr, NULL);
  if (BF_SUCCESS != rc) return rc;

  port_mgr_tof2_pdev_t *dev_p = port_mgr_dev_physical_dev_tof2_get(dev_id);
  if (!dev_p) return BF_INVALID_ARG;
  for (int i = 0; i < TOF2_NUM_UMAC3; ++i) dev_p->umac3[i].ch_in_use = 0;
  for (int i = 0; i < TOF2_NUM_UMAC4; ++i) dev_p->umac4[i].ch_in_use = 0;
  return BF_SUCCESS;
}

/**************************************************************************
 * port_mgr_tof2_port_flowcontrol_set
 *
 **************************************************************************/
bf_status_t port_mgr_tof2_port_flowcontrol_set(bf_dev_id_t dev_id,
                                               bf_dev_port_t dev_port) {
  int umac, ch;
  bf_status_t rc;

  rc = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch, NULL);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;

  port_mgr_tof2_umac_flowcontrol_set(dev_id, umac, ch);

  return BF_SUCCESS;
}

/**************************************************************************
 * port_mgr_tof2_port_mac_int_en_set
 *
 * Warning:
 * enable bit controls ALL channels on UMAC. Must use Comira
 * reg to enable/disable specific channels.
 **************************************************************************/
bf_status_t port_mgr_tof2_port_mac_int_en_set(bf_dev_id_t dev_id,
                                              bf_dev_port_t dev_port,
                                              bool on) {
  int umac, ch;
  bf_status_t rc;

  rc = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch, NULL);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;

  port_mgr_tof2_umac_int_en_set(dev_id, umac, on);

  return BF_SUCCESS;
}

/**************************************************************************
 * port_mgr_tof2_port_local_fault_int_en_set
 *
 **************************************************************************/
bf_status_t port_mgr_tof2_port_local_fault_int_en_set(bf_dev_id_t dev_id,
                                                      bf_dev_port_t dev_port,
                                                      bool en) {
  int umac, ch;
  bf_status_t rc;

  rc = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch, NULL);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;

  port_mgr_tof2_umac_local_fault_int_en_set(dev_id, umac, ch, en);

  return BF_SUCCESS;
}

/**************************************************************************
 * port_mgr_tof2_port_remote_fault_int_en_set
 *
 **************************************************************************/
bf_status_t port_mgr_tof2_port_remote_fault_int_en_set(bf_dev_id_t dev_id,
                                                       bf_dev_port_t dev_port,
                                                       bool en) {
  int umac, ch;
  bf_status_t rc;

  rc = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch, NULL);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;

  port_mgr_tof2_umac_remote_fault_int_en_set(dev_id, umac, ch, en);

  return BF_SUCCESS;
}
/**************************************************************************
 * port_mgr_tof2_port_link_gain_int_en_set
 *
 **************************************************************************/
bf_status_t port_mgr_tof2_port_link_gain_int_en_set(bf_dev_id_t dev_id,
                                                    bf_dev_port_t dev_port,
                                                    bool en) {
  int umac, ch;
  bf_status_t rc;

  rc = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch, NULL);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;

  port_mgr_tof2_umac_link_gain_int_en_set(dev_id, umac, ch, en);

  return BF_SUCCESS;
}

/**************************************************************************
 * port_mgr_tof2_port_default_handler_for_mac_ints
 *
 **************************************************************************/
void port_mgr_tof2_port_default_handler_for_mac_ints(
    bf_dev_id_t dev_id,
    int ch,
    uint32_t reg,
    uint32_t set_int_bits_unused,
    void *userdata) {
  bool possible_state_chg = false;
  bf_dev_port_t dev_port;
  uint32_t mac_stn_id;
  lld_err_t err;

  /* Must compute dev_port based on the MAC stn_id in the register and
   * the associated channel number from polling each channels interrupt
   * registers */
  mac_stn_id = (reg >> 18) & 0x3F;              // ~0xff03ffff)
  if ((mac_stn_id > 0) && (mac_stn_id < 33)) {  // umac4
    uint32_t umac4 = mac_stn_id;

    err = lld_sku_map_mac_ch_to_dev_port_id(dev_id, umac4, ch, &dev_port);
    if (err) {
      port_mgr_log("%d: Error %d: converting umac%d ch%d to dev_port",
                   dev_id,
                   err,
                   umac4,
                   ch);
      return;
    }

    port_mgr_tof2_umac_handle_interrupts(
        dev_id, umac4, ch, &possible_state_chg);
    if (possible_state_chg) {
      /* determine whether or not the real port state has changed, and
       * if so, execute any necessary actions ("link-up-actions" or
       * "link-dn-actions") */
      bf_port_oper_state_set_pending_callbacks(dev_id, dev_port, false);
      port_mgr_ldev_t *dev_p = port_mgr_dev_logical_dev_get(dev_id);
      if (dev_p != NULL) {
        dev_p->port_intr_bhalf_valid = true;
      }
    }
  } else {  // UMAC3
    /* This is a CPU port. Note that link fault notifications are sent from
     * the umac3 interrupt handle. */
    uint32_t umac3 = mac_stn_id;

    err = port_mgr_tof2_umac_handle_interrupts(
        dev_id, umac3, ch, &possible_state_chg);
  }

  (void)reg;
  (void)set_int_bits_unused;
  (void)userdata;
}

/**************************************************************************
 * port_mgr_tof2_port_register_default_handler_for_mac_ints
 *
 **************************************************************************/
static bf_status_t port_mgr_tof2_set_default_int_hdlr_itr(
    bf_dev_id_t dev_id, bf_dev_port_t dev_port, void *unused) {
  bf_status_t rc;

  rc = port_mgr_port_bind_interrupt_callback(
      dev_id, dev_port, port_mgr_tof2_port_default_handler_for_mac_ints, NULL);
  (void)unused;
  return rc;
}

/**************************************************************************
 * port_mgr_tof2_port_register_default_handler_for_mac_ints
 *
 **************************************************************************/
bf_status_t port_mgr_tof2_port_register_default_handler_for_mac_ints(
    bf_dev_id_t dev_id) {
  bf_status_t rc;

  rc = port_mgr_tof2_port_iter(
      dev_id, port_mgr_tof2_set_default_int_hdlr_itr, NULL);
  if (rc != BF_SUCCESS) {
    return rc;
  }

  port_mgr_port_bind_int_bh_wakeup_callback(
      dev_id, port_mgr_port_default_int_bh_wakeup_cb);

  return BF_SUCCESS;
}

/**************************************************************************
 * port_mgr_tof2_umac_config_get
 *
 **************************************************************************/
bf_status_t port_mgr_tof2_port_config_get(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port) {
  int umac, ch;
  bf_status_t rc;

  rc = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch, NULL);
  if (rc != BF_SUCCESS) return BF_INVALID_ARG;

  return port_mgr_tof2_umac_config_get(dev_id, umac, ch);
}

/**************************************************************************
 * port_mgr_tof2_link_fault_status_get
 *
 **************************************************************************/
bf_status_t port_mgr_tof2_link_fault_status_get(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    bf_port_link_fault_st_t *link_fault_st) {
  int is_cpu_port = 0;
  bool remote_fault;
  bool local_fault;
  bf_status_t rc;
  bool pcs_rdy;
  bool up;

  if (!link_fault_st) return BF_INVALID_ARG;

  rc = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, NULL, NULL, &is_cpu_port);
  if (rc != BF_SUCCESS) return rc;
  /* This function is not supported on CPU port for Tof2 */
  if (is_cpu_port) {
    port_mgr_log("Error : Get link fault status not supported on CPU port");
    return BF_INVALID_ARG;
  }

  rc = port_mgr_tof2_port_oper_state_extended_get(
      dev_id, dev_port, &up, &pcs_rdy, &local_fault, &remote_fault);
  if (rc == BF_SUCCESS) {
    if (up) {
      *link_fault_st = BF_PORT_LINK_FAULT_OK;
    } else if (!pcs_rdy || local_fault) {
      *link_fault_st = BF_PORT_LINK_FAULT_LOC_FAULT;
    } else if (remote_fault) {
      *link_fault_st = BF_PORT_LINK_FAULT_REM_FAULT;
    }
  }
  return rc;
}

static bool tof2_port_misc_params_change_detected(port_cfg_settings_t *hw,
                                                  port_cfg_settings_t *sw,
                                                  bf_dev_id_t dev_id,
                                                  bf_dev_port_t dev_port) {
  uint32_t i = 0;

  if (!sw || !hw) return true;

  if (sw->fc_corr_en != hw->fc_corr_en) {
    port_mgr_log("%s:%d: Change detected in fc_corr_en : %d(SW) : %d(HW)",
                 __func__,
                 __LINE__,
                 sw->fc_corr_en,
                 hw->fc_corr_en);
    return true;
  }
  if (sw->fc_ind_en != hw->fc_ind_en) {
    port_mgr_log("%s:%d: Change detected in fc_ind_en : %d(SW) : %d(HW)",
                 __func__,
                 __LINE__,
                 sw->fc_ind_en,
                 hw->fc_ind_en);
    return true;
  }
  if (sw->tx_mtu != hw->tx_mtu) {
    port_mgr_log("%s:%d: Change detected in tx_mtu : %d(SW) : %d(HW)",
                 __func__,
                 __LINE__,
                 sw->tx_mtu,
                 hw->tx_mtu);
    return true;
  }
  if (sw->rx_mtu != hw->rx_mtu) {
    port_mgr_log("%s:%d: Change detected in rx_mtu : %d(SW) : %d(HW)",
                 __func__,
                 __LINE__,
                 sw->rx_mtu,
                 hw->rx_mtu);
    return true;
  }
  if (sw->ifg != hw->ifg) {
    port_mgr_log("%s:%d: Change detected in ifg : %d(SW) : %d(HW)",
                 __func__,
                 __LINE__,
                 sw->ifg,
                 hw->ifg);
    return true;
  }
  if (sw->ipg != hw->ipg) {
    port_mgr_log("%s:%d: Change detected in ipg : %d(SW) : %d(HW)",
                 __func__,
                 __LINE__,
                 sw->ipg,
                 hw->ipg);
    return true;
  }
  if (memcmp(sw->preamble, hw->preamble, sizeof(sw->preamble)) != 0) {
    for (i = 0; i < sizeof(sw->preamble); i++) {
      port_mgr_log("%s:%d: Change detected in preamble : %d(SW) : %d(HW)",
                   __func__,
                   __LINE__,
                   sw->preamble[i],
                   hw->preamble[i]);
    }
    return true;
  }
  if (sw->preamble_length != hw->preamble_length) {
    port_mgr_log("%s:%d: Change detected in preamble_length : %d(SW) : %d(HW)",
                 __func__,
                 __LINE__,
                 sw->preamble_length,
                 hw->preamble_length);
    return true;
  }
  if (sw->promiscuous_mode != hw->promiscuous_mode) {
    port_mgr_log("%s:%d: Change detected in promiscuous_mode : %d(SW) : %d(HW)",
                 __func__,
                 __LINE__,
                 sw->promiscuous_mode,
                 hw->promiscuous_mode);
    return true;
  }
  if (memcmp(sw->mac_addr, hw->mac_addr, sizeof(sw->mac_addr)) != 0) {
    for (i = 0; i < sizeof(sw->mac_addr); i++) {
      port_mgr_log("%s:%d: Change detected in mac_addr : %d(SW) : %d(HW)",
                   __func__,
                   __LINE__,
                   sw->mac_addr[i],
                   hw->mac_addr[i]);
    }
    return true;
  }
  if (sw->loopback_enabled != hw->loopback_enabled) {
    port_mgr_log("%s:%d: Change detected in loopback_enabled : %d(SW) : %d(HW)",
                 __func__,
                 __LINE__,
                 sw->loopback_enabled,
                 hw->loopback_enabled);
    return true;
  }
  if (sw->dfe_type != hw->dfe_type) {
    port_mgr_log("%s:%d: Change detected in dfe_type : %d(SW) : %d(HW)",
                 __func__,
                 __LINE__,
                 sw->dfe_type,
                 hw->dfe_type);
    return true;
  }

  if (sw->link_pause_tx != hw->link_pause_tx) {
    port_mgr_log("%s:%d: Change detected in link_pause_tx : %d(SW) : %d(HW)",
                 __func__,
                 __LINE__,
                 sw->link_pause_tx,
                 hw->link_pause_tx);
    return true;
  }

  /*
   * For PFC TX enable, all priorities get enabled in HW by default
   * So, instead of checking for whole PFC bitmap
   * between HW & SW config, just check if it's enabled or disabled.
   */
  if ((sw->pfc_pause_tx && !hw->pfc_pause_tx) ||
      (!sw->pfc_pause_tx && hw->pfc_pause_tx)) {
    port_mgr_log("%s:%d: Change detected in pfc_pause_tx", __func__, __LINE__);
    port_mgr_log("%s:%d: pfc_pause_tx : %d(SW) : %d(HW)",
                 __func__,
                 __LINE__,
                 sw->pfc_pause_tx,
                 hw->pfc_pause_tx);
    return true;
  }

  /*
   * For PFC RX and link pause RX config, same register is used in HW and
   * also there is no support for per priority PFC RX config. So, if RX flow
   * control is enabled in HW, it's fine if either of PFC RX & link pause RX
   * is enabled in SW. If it's disabled in HW, both PFC RX & link pause RX
   * should be disabled in SW.
   */
  if ((hw->pfc_pause_rx || hw->link_pause_rx) &&
      (!sw->pfc_pause_rx && !sw->link_pause_rx)) {
    port_mgr_log(
        "%s:%d: Change detected : Atleast one of PFC and LINK pause_rx enabled "
        "in hw but neither enabled in sw",
        __func__,
        __LINE__);
    port_mgr_log("%s:%d: pfc_pause_rx : %d(SW) : %d(HW)",
                 __func__,
                 __LINE__,
                 sw->pfc_pause_rx,
                 hw->pfc_pause_rx);
    port_mgr_log("%s:%d: link_pause_rx : %d(SW) : %d(HW)",
                 __func__,
                 __LINE__,
                 sw->link_pause_rx,
                 hw->link_pause_rx);
    return true;
  }
  if ((!hw->pfc_pause_rx && !hw->link_pause_rx) &&
      (sw->pfc_pause_rx || sw->link_pause_rx)) {
    port_mgr_log(
        "%s:%d: Change detected : PFC and LINK pause_rx both disabled in hw "
        "but atleast one enabled in sw",
        __func__,
        __LINE__);
    port_mgr_log("%s:%d: pfc_pause_rx : %d(SW) : %d(HW)",
                 __func__,
                 __LINE__,
                 sw->pfc_pause_rx,
                 hw->pfc_pause_rx);
    port_mgr_log("%s:%d: link_pause_rx : %d(SW) : %d(HW)",
                 __func__,
                 __LINE__,
                 sw->link_pause_rx,
                 hw->link_pause_rx);
    return true;
  }

  if (sw->xoff_pause_time != hw->xoff_pause_time) {
    port_mgr_log("%s:%d: Change detected in xoff_pause_time : %d(SW) : %d(HW)",
                 __func__,
                 __LINE__,
                 sw->xoff_pause_time,
                 hw->xoff_pause_time);
    return true;
  }
  if (sw->xon_pause_time != hw->xon_pause_time) {
    port_mgr_log("%s:%d: Change detected in xon_pause_time : %d(SW) : %d(HW)",
                 __func__,
                 __LINE__,
                 sw->xon_pause_time,
                 hw->xon_pause_time);
    return true;
  }
  if (memcmp(sw->fc_src_mac_addr,
             hw->fc_src_mac_addr,
             sizeof(sw->fc_src_mac_addr)) != 0) {
    for (i = 0; i < sizeof(sw->fc_src_mac_addr); i++) {
      port_mgr_log(
          "%s:%d: Change detected in fc_src_mac_addr : %d(SW) : %d(HW)",
          __func__,
          __LINE__,
          sw->fc_src_mac_addr[i],
          hw->fc_src_mac_addr[i]);
    }
    return true;
  }
  if (memcmp(sw->fc_dst_mac_addr,
             hw->fc_dst_mac_addr,
             sizeof(sw->fc_dst_mac_addr)) != 0) {
    for (i = 0; i < sizeof(sw->fc_dst_mac_addr); i++) {
      port_mgr_log(
          "%s:%d: Change detected in fc_dst_mac_addr : %d(SW) : %d(HW)",
          __func__,
          __LINE__,
          sw->fc_dst_mac_addr[i],
          hw->fc_dst_mac_addr[i]);
    }
    return true;
  }
  if (sw->txff_trunc_ctrl_size != hw->txff_trunc_ctrl_size) {
    port_mgr_log(
        "%s:%d: Change detected in txff_trunc_ctrl_size : %d(SW) : %d(HW)",
        __func__,
        __LINE__,
        sw->txff_trunc_ctrl_size,
        hw->txff_trunc_ctrl_size);
    return true;
  }
  if (sw->txff_trunc_ctrl_en != hw->txff_trunc_ctrl_en) {
    port_mgr_log(
        "%s:%d: Change detected in txff_trunc_ctrl_en : %d(SW) : %d(HW)",
        __func__,
        __LINE__,
        sw->txff_trunc_ctrl_en,
        hw->txff_trunc_ctrl_en);
    return true;
  }
  if (sw->txff_ctrl_crc_check_disable != hw->txff_ctrl_crc_check_disable) {
    port_mgr_log(
        "%s:%d: Change detected in txff_ctrl_crc_check_disable : %d(SW) : "
        "%d(HW)",
        __func__,
        __LINE__,
        sw->txff_ctrl_crc_check_disable,
        hw->txff_ctrl_crc_check_disable);
    return true;
  }
  if (sw->txff_ctrl_crc_removal_disable != hw->txff_ctrl_crc_removal_disable) {
    port_mgr_log(
        "%s:%d: Change detected in txff_ctrl_crc_removal_disable : %d(SW) : "
        "%d(HW)",
        __func__,
        __LINE__,
        sw->txff_ctrl_crc_removal_disable,
        hw->txff_ctrl_crc_removal_disable);
    return true;
  }
  if (sw->txff_ctrl_fcs_insert_disable != hw->txff_ctrl_fcs_insert_disable) {
    port_mgr_log(
        "%s:%d: Change detected in txff_ctrl_fcs_insert_disable : %d(SW) : "
        "%d(HW)",
        __func__,
        __LINE__,
        sw->txff_ctrl_fcs_insert_disable,
        hw->txff_ctrl_fcs_insert_disable);
    return true;
  }
  if (sw->txff_ctrl_pad_disable != hw->txff_ctrl_pad_disable) {
    port_mgr_log(
        "%s:%d: Change detected in txff_ctrl_pad_disable : %d(SW) : %d(HW)",
        __func__,
        __LINE__,
        sw->txff_ctrl_pad_disable,
        hw->txff_ctrl_pad_disable);
    return true;
  }

  port_mgr_log(
      "%s:%d No changes detected in misc port params for dev %d port %d",
      __func__,
      __LINE__,
      dev_id,
      dev_port);
  return false;
}

static void port_mgr_port_attrib_get(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port,
                                     bf_port_attributes_t *port_attrib) {
  port_mgr_port_t *port_p;
  port_p = port_mgr_map_dev_port_to_port_allow_unassigned(dev_id, dev_port);
  if (port_p == NULL) return;

  port_mgr_dev_t *dev_p = port_mgr_map_dev_id_to_dev_p_allow_unassigned(dev_id);
  if (dev_p == NULL) return;

  port_attrib->autoneg_enable = port_p->an_enabled;
  port_attrib->port_speeds = port_p->sw.speed;
  port_attrib->port_fec_types = port_p->sw.fec;
  port_attrib->n_lanes = port_p->sw.n_lanes;
}

static void port_dump_all_config(port_cfg_settings_t *sw,
                                 port_cfg_settings_t *hw) {
  port_mgr_log(
      "assigned                        %d : %d", sw->assigned, hw->assigned);
  port_mgr_log(
      "enabled                         %d : %d", sw->enabled, hw->enabled);
  port_mgr_log("oper_state                      %d : %d",
               sw->oper_state,
               hw->oper_state);
  port_mgr_log("speed                       %5d : %d", sw->speed, hw->speed);
  port_mgr_log("fec                             %d : %d", sw->fec, hw->fec);
  port_mgr_log("fc_corr_en                      %d : %d",
               sw->fc_corr_en,
               hw->fc_corr_en);
  port_mgr_log(
      "fc_ind_en                       %d : %d", sw->fc_ind_en, hw->fc_ind_en);
  port_mgr_log(
      "tx_mtu                      %5d : %-5d", sw->tx_mtu, hw->tx_mtu);
  port_mgr_log(
      "rx_mtu                      %5d : %-5d", sw->rx_mtu, hw->rx_mtu);
  port_mgr_log("ifg                         %5d : %d", sw->ifg, hw->ifg);
  port_mgr_log("preamble_length                %2d : %-2d",
               sw->preamble_length,
               hw->preamble_length);
  port_mgr_log("promiscuous_mode                %d : %d",
               sw->promiscuous_mode,
               hw->promiscuous_mode);
  port_mgr_log("link_pause_tx                   %d : %d",
               sw->link_pause_tx,
               hw->link_pause_tx);
  port_mgr_log("link_pause_rx                   %d : %d",
               sw->link_pause_rx,
               hw->link_pause_rx);
  port_mgr_log("pfc_pause_tx                    %d : %d",
               sw->pfc_pause_tx,
               hw->pfc_pause_tx);
  port_mgr_log("pfc_pause_rx                    %d : %d",
               sw->pfc_pause_rx,
               hw->pfc_pause_rx);
  port_mgr_log("loopback_enabled                %d : %d",
               sw->loopback_enabled,
               hw->loopback_enabled);
  port_mgr_log(
      "dfe_type                        %d : %d", sw->dfe_type, hw->dfe_type);
  port_mgr_log("xoff_pause_time             %5d : %-5d",
               sw->xoff_pause_time,
               hw->xoff_pause_time);
  port_mgr_log("xon_pause_time              %5d : %-5d",
               sw->xon_pause_time,
               hw->xon_pause_time);
  port_mgr_log("txff_trunc_ctrl_size            %d : %d",
               sw->txff_trunc_ctrl_size,
               hw->txff_trunc_ctrl_size);
  port_mgr_log("txff_trunc_ctrl_en              %d : %d",
               sw->txff_trunc_ctrl_en,
               hw->txff_trunc_ctrl_en);
  port_mgr_log("txff_ctrl_crc_check_disable     %d : %d",
               sw->txff_ctrl_crc_check_disable,
               hw->txff_ctrl_crc_check_disable);
  port_mgr_log("txff_ctrl_crc_removal_disable   %d : %d",
               sw->txff_ctrl_crc_removal_disable,
               hw->txff_ctrl_crc_removal_disable);
  port_mgr_log("txff_ctrl_fcs_insert_disable    %d : %d",
               sw->txff_ctrl_fcs_insert_disable,
               hw->txff_ctrl_fcs_insert_disable);
  port_mgr_log("txff_ctrl_pad_disable           %d : %d",
               sw->txff_ctrl_pad_disable,
               hw->txff_ctrl_pad_disable);
  port_mgr_log(
      "mac_addr        %02x:%02x:%02x:%02x:%02x:%02x :  "
      "%02x:%02x:%02x:%02x:%02x:%02x\n",
      sw->mac_addr[0],
      sw->mac_addr[1],
      sw->mac_addr[2],
      sw->mac_addr[3],
      sw->mac_addr[4],
      sw->mac_addr[5],
      hw->mac_addr[0],
      hw->mac_addr[1],
      hw->mac_addr[2],
      hw->mac_addr[3],
      hw->mac_addr[4],
      hw->mac_addr[5]);
  port_mgr_log(
      "fc_dst_mac_addr %02x:%02x:%02x:%02x:%02x:%02x :  "
      "%02x:%02x:%02x:%02x:%02x:%02x\n",
      sw->fc_dst_mac_addr[0],
      sw->fc_dst_mac_addr[1],
      sw->fc_dst_mac_addr[2],
      sw->fc_dst_mac_addr[3],
      sw->fc_dst_mac_addr[4],
      sw->fc_dst_mac_addr[5],
      hw->fc_dst_mac_addr[0],
      hw->fc_dst_mac_addr[1],
      hw->fc_dst_mac_addr[2],
      hw->fc_dst_mac_addr[3],
      hw->fc_dst_mac_addr[4],
      hw->fc_dst_mac_addr[5]);
  port_mgr_log(
      "fc_src_mac_addr %02x:%02x:%02x:%02x:%02x:%02x :  "
      "%02x:%02x:%02x:%02x:%02x:%02x\n",
      sw->fc_src_mac_addr[0],
      sw->fc_src_mac_addr[1],
      sw->fc_src_mac_addr[2],
      sw->fc_src_mac_addr[3],
      sw->fc_src_mac_addr[4],
      sw->fc_src_mac_addr[5],
      hw->fc_src_mac_addr[0],
      hw->fc_src_mac_addr[1],
      hw->fc_src_mac_addr[2],
      hw->fc_src_mac_addr[3],
      hw->fc_src_mac_addr[4],
      hw->fc_src_mac_addr[5]);
}

bf_status_t port_mgr_tof2_loopbackport_delta_compute(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    bf_ha_port_reconcile_info_t *recon_info) {
  port_mgr_port_t *port_p;
  bool existing_port_deleted = false;
  bool new_port_detected = false;
  bool same_port_with_diff_cfg_detected = false;
  bool existing_port_admin_state_change_detected = false;
  bool port_bringup_required = false;
  bool port_fsm_link_monitoring_required = false;

  port_mgr_log("%s:%d Entering Computing MAC delta changes for dev %d port %d",
               __func__,
               __LINE__,
               dev_id,
               dev_port);
  port_p = port_mgr_map_dev_port_to_port_allow_unassigned(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  port_mgr_dev_t *dev_p = port_mgr_map_dev_id_to_dev_p_allow_unassigned(dev_id);
  if (dev_p == NULL) {
    return BF_INVALID_ARG;
  }

  port_mgr_log(
      "Dumping Config for Dev : %d : loopback Port : %d : ( SW VAL : HW VAL )",
      dev_id,
      dev_port);
  port_dump_all_config(&port_p->sw, &port_p->hw);

  existing_port_deleted =
      ((port_p->hw.assigned == true) && (port_p->sw.assigned == false));

  new_port_detected =
      ((port_p->hw.assigned == false) && (port_p->sw.assigned == true));

  if ((port_p->hw.assigned == true) && (port_p->sw.assigned == true)) {
    if ((port_p->hw.speed != port_p->sw.speed) ||
        (port_p->sw.loopback_enabled != port_p->hw.loopback_enabled)) {
      same_port_with_diff_cfg_detected = true;
    } else if ((port_p->sw.loopback_enabled) &&
               (port_p->sw.lpbk_mode != port_p->hw.lpbk_mode)) {
      same_port_with_diff_cfg_detected = true;
    }
  }

  existing_port_admin_state_change_detected =
      ((port_p->hw.assigned == true) && (port_p->sw.assigned == true) &&
       (port_p->hw.enabled != port_p->sw.enabled));

  port_fsm_link_monitoring_required =
      ((port_p->hw.assigned == true) && (port_p->sw.assigned == true) &&
       (port_p->sw.enabled == true) && (port_p->hw.enabled == true));

  /* Indicates that an existing port has been deleted */
  if (existing_port_deleted) {
    /* Set the corrective action as DEL for the port */
    recon_info->ca = BF_HA_CA_PORT_DELETE;
    port_mgr_log("%s:%d:%d:%d Existing port deleted",
                 __func__,
                 __LINE__,
                 dev_id,
                 dev_port);
    return BF_SUCCESS;
  }
  /* Indicates that a new port is being added */
  else if (new_port_detected) {
    /* Get the port attributes of the port which will be used when the port is
     * added */
    port_mgr_port_attrib_get(dev_id, dev_port, &recon_info->port_attrib);
    /* Set the corrective action as ADD for the port under consideration */
    recon_info->ca = BF_HA_CA_PORT_ADD;
    if (port_p->sw.enabled) {
      recon_info->ca = BF_HA_CA_PORT_ADD_THEN_ENABLE;
    }
    port_mgr_log(
        "%s:%d:%d:%d New port detected", __func__, __LINE__, dev_id, dev_port);
    return BF_SUCCESS;
  }
  /* Indicates that the same port is being added with a diff config*/
  else if (same_port_with_diff_cfg_detected) {
    /* Get the port attributes of the port which will be used when the port is
     * added */
    port_mgr_port_attrib_get(dev_id, dev_port, &recon_info->port_attrib);
    /* Set the corrective action as DEL_THEN_ADD for the port under
     * consideration */
    recon_info->ca = BF_HA_CA_PORT_DELETE_THEN_ADD;
    if (port_p->sw.enabled) {
      recon_info->ca = BF_HA_CA_PORT_DELETE_THEN_ADD_THEN_ENABLE;
    }
    port_mgr_log("%s:%d:%d:%d Same port with different config detected",
                 __func__,
                 __LINE__,
                 dev_id,
                 dev_port);
    return BF_SUCCESS;
  }
  /* Indicates that a change in the admin state of the port is detected */
  else if (existing_port_admin_state_change_detected) {
    recon_info->ca =
        port_p->sw.enabled ? BF_HA_CA_PORT_ENABLE : BF_HA_CA_PORT_DISABLE;
    port_mgr_log("%s:%d:%d:%d Port admin state change detected",
                 __func__,
                 __LINE__,
                 dev_id,
                 dev_port);
    return BF_SUCCESS;
  } else if (port_bringup_required) {
    /* Indicates that the port was and is enabled after warm boot but is
     * is not UP. Hence FLAP the port in order to bring it up */
    recon_info->ca = BF_HA_CA_PORT_FLAP;
    port_mgr_log("%s:%d:%d:%d Port needs to be brought up",
                 __func__,
                 __LINE__,
                 dev_id,
                 dev_port);
    return BF_SUCCESS;
  }
  /* Indicates that the port was replayed with the exact same config */
  else if (port_fsm_link_monitoring_required) {
    /* This indicates that the port was added and enabled and up and was
     * replayed
     * with the exact same configuration. As a result we want initialize the
     * fsm in link monitoring state.
     */
    recon_info->ca = BF_HA_CA_PORT_FSM_LINK_MONITORING;
    port_mgr_log(
        "%s:%d:%d:%d Port FSM needs to be initialized in WAIT_FOR_DOWN_EVENT "
        "state",
        __func__,
        __LINE__,
        dev_id,
        dev_port);
    return BF_SUCCESS;
  }

  port_mgr_log("%s:%d:%d:%d No MAC delta detected for port",
               __func__,
               __LINE__,
               dev_id,
               dev_port);
  return BF_SUCCESS;
}

bf_status_t port_mgr_tof2_port_delta_compute(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    bf_ha_port_reconcile_info_t *recon_info) {
  port_mgr_port_t *port_p;
  bool existing_port_deleted = false;
  bool new_port_detected = false;
  bool same_port_with_diff_cfg_detected = false;
  bool existing_port_admin_state_change_detected = false;
  bool existing_port_misc_params_change_detected = false;
  bool port_bringup_required = false;
  bool port_fsm_link_monitoring_required = false;

  port_mgr_log("%s:%d Entering Computing MAC delta changes for dev %d port %d",
               __func__,
               __LINE__,
               dev_id,
               dev_port);
  port_p = port_mgr_map_dev_port_to_port_allow_unassigned(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  port_mgr_dev_t *dev_p = port_mgr_map_dev_id_to_dev_p_allow_unassigned(dev_id);
  if (dev_p == NULL) {
    return BF_INVALID_ARG;
  }

  port_mgr_log("Dumping Config for Dev : %d : Port : %d : ( SW VAL : HW VAL )",
               dev_id,
               dev_port);
  port_dump_all_config(&port_p->sw, &port_p->hw);

  existing_port_deleted =
      ((port_p->hw.assigned == true) && (port_p->sw.assigned == false));
  new_port_detected =
      ((port_p->hw.assigned == false) && (port_p->sw.assigned == true));
  same_port_with_diff_cfg_detected =
      ((port_p->hw.assigned == true) && (port_p->sw.assigned == true) &&
       ((port_p->hw.speed != port_p->sw.speed) ||
        (port_p->hw.fec != port_p->sw.fec)));
  existing_port_admin_state_change_detected =
      ((port_p->hw.assigned == true) && (port_p->sw.assigned == true) &&
       (port_p->hw.enabled != port_p->sw.enabled));
  /* We care about changes in misc params only if the port was added in hw
   * before cfg replay and was added and enabled during cfg replay. We need
   * to care about the port being enabled during cfg replay because of the
   * following scenario
   * Before cfg replay, if we only added and set the src mac address but
   * never enabled the port, the src address will never be flushed to the
   * hardware. Then during cfg replay, we would again add the port and replay
   * the src mac address. Thus while computing the delta for the port, we
   * would think that there is a mismatch between the replayed software src
   * mac addr and the already programmed hardware src mac addr. And thus
   * port mgr would flap the port. The correct thing to do here would be
   * to do nothing
   */
  existing_port_misc_params_change_detected =
      ((port_p->hw.assigned == true) && (port_p->sw.assigned == true) &&
       (port_p->sw.enabled == true) &&
       (tof2_port_misc_params_change_detected(
           &port_p->hw, &port_p->sw, dev_id, dev_port)));
  /* oper_state is ignored if port is in MAC_NEAR loopback because for that
   * mode the hw oper_state is forced to 1 by sw.
   */
  port_bringup_required =
      ((port_p->hw.assigned == true) && (port_p->sw.assigned == true) &&
       (port_p->sw.enabled == true) && (port_p->hw.enabled == true) &&
       (port_p->hw.lpbk_mode != BF_LPBK_MAC_NEAR) &&
       (port_p->hw.lpbk_mode != BF_LPBK_PIPE) && (!port_p->hw.oper_state));
  port_fsm_link_monitoring_required =
      ((port_p->hw.assigned == true) && (port_p->sw.assigned == true) &&
       (port_p->sw.enabled == true) && (port_p->hw.enabled == true) &&
       ((port_p->hw.oper_state) || (port_p->hw.lpbk_mode == BF_LPBK_MAC_NEAR)));

  /* Indicates that an existing port has been deleted */
  if (existing_port_deleted) {
    /* Set the corrective action as DEL for the port */
    recon_info->ca = BF_HA_CA_PORT_DELETE;
    port_mgr_log("%s:%d:%d:%d Existing port deleted",
                 __func__,
                 __LINE__,
                 dev_id,
                 dev_port);
    return BF_SUCCESS;
  }
  /* Indicates that a new port is being added */
  else if (new_port_detected) {
    /* Get the port attributes of the port which will be used when the port is
     * added */
    port_mgr_port_attrib_get(dev_id, dev_port, &recon_info->port_attrib);
    /* Set the corrective action as ADD for the port under consideration */
    recon_info->ca = BF_HA_CA_PORT_ADD;
    if (port_p->sw.enabled) {
      recon_info->ca = BF_HA_CA_PORT_ADD_THEN_ENABLE;
    }
    port_mgr_log(
        "%s:%d:%d:%d New port detected", __func__, __LINE__, dev_id, dev_port);
    return BF_SUCCESS;
  }
  /* Indicates that the same port is being added with a diff config*/
  else if (same_port_with_diff_cfg_detected) {
    /* Get the port attributes of the port which will be used when the port is
     * added */
    port_mgr_port_attrib_get(dev_id, dev_port, &recon_info->port_attrib);
    /* Set the corrective action as DEL_THEN_ADD for the port under
     * consideration */
    recon_info->ca = BF_HA_CA_PORT_DELETE_THEN_ADD;
    if (port_p->sw.enabled) {
      recon_info->ca = BF_HA_CA_PORT_DELETE_THEN_ADD_THEN_ENABLE;
    }
    port_mgr_log("%s:%d:%d:%d Same port with different config detected",
                 __func__,
                 __LINE__,
                 dev_id,
                 dev_port);
    return BF_SUCCESS;
  }
  /* Indicates that a change in the admin state of the port is detected */
  else if (existing_port_admin_state_change_detected) {
    recon_info->ca =
        port_p->sw.enabled ? BF_HA_CA_PORT_ENABLE : BF_HA_CA_PORT_DISABLE;
    port_mgr_log("%s:%d:%d:%d Port admin state change detected",
                 __func__,
                 __LINE__,
                 dev_id,
                 dev_port);
    return BF_SUCCESS;
  } else if (existing_port_misc_params_change_detected) {
    recon_info->ca = BF_HA_CA_PORT_FLAP;
    port_mgr_log("%s:%d:%d:%d Existing port misc params change detected",
                 __func__,
                 __LINE__,
                 dev_id,
                 dev_port);
    return BF_SUCCESS;
  }
  /* Indicates that the port was and is enabled after warm boot but is
   * is not UP. Hence FLAP the port in order to bring it up */
  else if (port_bringup_required) {
    recon_info->ca = BF_HA_CA_PORT_FLAP;
    port_mgr_log("%s:%d:%d:%d Port needs to be brought up",
                 __func__,
                 __LINE__,
                 dev_id,
                 dev_port);
    return BF_SUCCESS;
  }
  /* Indicates that the port was replayed with the exact same config */
  else if (port_fsm_link_monitoring_required) {
    /* This indicates that the port was added and enabled and up and was
     * replayed
     * with the exact same configuration. As a result we want initialize the
     * fsm in link monitoring state.
     */
    recon_info->ca = BF_HA_CA_PORT_FSM_LINK_MONITORING;
    port_mgr_log(
        "%s:%d:%d:%d Port FSM needs to be initialized in WAIT_FOR_DOWN_EVENT "
        "state",
        __func__,
        __LINE__,
        dev_id,
        dev_port);
    return BF_SUCCESS;
  }

  port_mgr_log("%s:%d:%d:%d No MAC delta detected for port",
               __func__,
               __LINE__,
               dev_id,
               dev_port);
  return BF_SUCCESS;
}
