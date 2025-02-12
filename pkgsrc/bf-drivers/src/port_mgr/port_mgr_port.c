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
#include <dvm/dvm_intf.h>
#include <pipe_mgr/pktgen_intf.h>
#include <port_mgr/bf_port_if.h>
#include <port_mgr/bf_fsm_if.h>
#include <port_mgr/port_mgr_intf.h>
#include <port_mgr/port_mgr.h>
#include <port_mgr/port_mgr_port.h>
#include "bf_types/bf_types.h"
#include "port_mgr_dev.h"
#include "port_mgr_map.h"
#include "port_mgr_tof1/port_mgr_tof1_port.h"
#include "port_mgr_tof2/port_mgr_tof2_port.h"
#include "port_mgr_tof1/port_mgr_mac.h"
#include "port_mgr/port_mgr_tof2/port_mgr_tof2_umac3.h"
#include "port_mgr/port_mgr_tof2/port_mgr_tof2_umac4.h"
#include "port_mgr/port_mgr_tof3/port_mgr_tof3_port.h"
#include "port_mgr_log.h"
#include <time.h>

extern int64_t timeval_subtract(struct timeval *x, struct timeval *y);

/********************************************************************
** \brief port_mgr_port_add
*         Add a Tofino port to the system
*         note: dvm will call this API twice,
*               first for ingress, then for egress.
*
* [ PRE_ENABLE ]
*
* \param dev_id: system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
* \param port  : physical port # on dev_id (0..LLD_MAX_PORTS-1)
* \param port_attrib
* \param direction
*
* \return: BF_SUCCESS     : port added successfully
* \return: BF_INVALID_ARG : invalid dev_id
* \return: BF_INVALID_ARG : invalid dev_port
*******************************************************************/
bf_status_t port_mgr_port_add(bf_dev_id_t dev_id,
                              bf_dev_port_t dev_port,
                              bf_port_attributes_t *port_attrib,
                              bf_port_cb_direction_t direction) {
  // Ignore recirc port.
  bool recirc = false;
  bf_status_t rc = bf_recirculation_get(dev_id, dev_port, &recirc);
  if (rc == BF_SUCCESS && recirc) {
    return BF_SUCCESS;
  }

  port_mgr_port_t *port_p =
      port_mgr_map_dev_port_to_port_allow_unassigned(dev_id, dev_port);

  if (port_attrib == NULL) {
    port_mgr_log_error(
        "%s:%d Null pointer arguments passed", __func__, __LINE__);
    return BF_INVALID_ARG;
  }
  port_mgr_log("%d:%d: %d(speed): %d(fec): %d(n_lanes)",
               dev_id,
               dev_port,
               port_attrib->port_speeds,
               port_attrib->port_fec_types,
               port_attrib->n_lanes);

  if (port_p) port_p->sw.n_lanes = port_attrib->n_lanes;
  if (port_mgr_dev_is_tof1(dev_id)) {
    return port_mgr_tof1_port_add(dev_id, dev_port, port_attrib, direction);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    return port_mgr_tof2_port_add(dev_id, dev_port, port_attrib, direction);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    return port_mgr_tof3_port_add(dev_id, dev_port, port_attrib, direction);
  }
  return BF_SUCCESS;
}

/** \brief port_mgr_port_remove
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
bf_status_t port_mgr_port_remove(bf_dev_id_t dev_id,
                                 bf_dev_port_t dev_port,
                                 bf_port_cb_direction_t direction) {
  port_mgr_log("%s:%d:%d:%d:%d(direction)",
               __func__,
               __LINE__,
               dev_id,
               dev_port,
               direction);

  // Ignore recirc port.
  bool recirc = false;
  bf_status_t sts = bf_recirculation_get(dev_id, dev_port, &recirc);
  if (sts == BF_SUCCESS && recirc) {
    return BF_SUCCESS;
  }

  if (port_mgr_dev_is_tof1(dev_id)) {
    return port_mgr_tof1_port_remove(dev_id, dev_port, direction);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    return port_mgr_tof2_port_remove(dev_id, dev_port, direction);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    return port_mgr_tof3_port_remove(dev_id, dev_port, direction);
  }
  return BF_SUCCESS;
}

/** \brief port_mgr_port_enable
 *         Enable a Tofino port. This kicks off the port
 *         bring-up sequence.
 *
 * [ PRE_ENABLE ]
 *
 *
 * \param dev_id: int         : system-assigned identifier
 *(0..BF_MAX_DEV_COUNT-1)
 * \param port: int         : physical port # on dev_id (0..LLD_MAX_PORTS-1)
 *
 * \return: BF_SUCCESS     : port enabled successfully
 * \return: BF_INVALID_ARG : dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG : port > LLD_MAX_PORTS-1
 *
 */
bf_status_t port_mgr_port_enable(bf_dev_id_t dev_id,
                                 bf_dev_port_t dev_port,
                                 bool enable) {
  port_mgr_log(
      "%s:%d:%d:%d:%d(enb)", __func__, __LINE__, dev_id, dev_port, enable);

  // Ignore recirc port.
  bool recirc = false;
  bf_status_t sts = bf_recirculation_get(dev_id, dev_port, &recirc);
  if (sts == BF_SUCCESS && recirc) {
    return BF_SUCCESS;
  }

  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  // save enable time. Allows determination of time-to-come-up
  if (port_p) {
    if (enable) {
      gettimeofday(&port_p->last_en_time, NULL);
    } else {
      port_p->last_en_time.tv_sec = 0;
      port_p->last_en_time.tv_usec = 0;
    }
  }

  if (port_mgr_dev_is_tof1(dev_id)) {
    return port_mgr_tof1_port_enable(dev_id, dev_port, enable);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    return port_mgr_tof2_port_enable(dev_id, dev_port, enable);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    return port_mgr_tof3_port_enable(dev_id, dev_port, enable);
  }

  return BF_SUCCESS;
}

/**************************************************************************
 * port_mgr_link_up_actions
 *
 * All actions to be effected upon determining a port has changed status
 * from Down to Up.
 **************************************************************************/
void port_mgr_link_up_actions(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  struct timeval last_en_time_cpy;
  port_mgr_port_callback_t fn;
  void *userdata;

  port_mgr_dev_port_cb_get(dev_id, &fn, &userdata);

  // notify other modules (internal cb)
  if (fn) {
    fn(dev_id, dev_port, PORT_MGR_PORT_EVT_UP, 0, userdata);
  }

  // notify user level (external cb)
  if (port_p) {
    gettimeofday(&port_p->last_up_time, NULL);
    if (port_p->sts_chg_cb) {
      /* Save current last_en_time value before executing the callback */
      last_en_time_cpy = port_p->last_en_time;
      port_p->sts_chg_cb(
          dev_id, dev_port, PORT_MGR_PORT_EVT_UP, port_p->sts_chg_userdata);

      /* Check if last_en_time was modified, which means that the callback
       * disabled and re-enabled the port.
       */
      if (last_en_time_cpy.tv_usec != port_p->last_en_time.tv_usec ||
          last_en_time_cpy.tv_sec != port_p->last_en_time.tv_sec) {
        /* Update last_up_time to avoid monotonicity issues dues to
         * gettimeofday(): set last_up_time == last_en_time, so link up time
         * will be zero.
         */
        port_p->last_up_time = port_p->last_en_time;
      }
    }
  }
}

/**************************************************************************
 * port_mgr_link_dn_actions
 *
 * All actions to be effected upon determining a port has changed status
 * from Up to Down.
 **************************************************************************/
void port_mgr_link_dn_actions(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  port_mgr_port_callback_t fn;
  void *userdata;

  port_mgr_dev_port_cb_get(dev_id, &fn, &userdata);

  // notify other modules (internal cb)
  if (fn) {
    fn(dev_id, dev_port, PORT_MGR_PORT_EVT_DOWN, 0, userdata);
  }

  if (port_p) {
    gettimeofday(&port_p->last_dn_time, NULL);
    if (port_p->sts_chg_cb) {
      port_p->sts_chg_cb(
          dev_id, dev_port, PORT_MGR_PORT_EVT_DOWN, port_p->sts_chg_userdata);
    }
  }
}

/**************************************************************************
 * port_mgr_fsm_actions
 *
 * All actions to be effected upon a transition in the the port's AN/LT
 * finite state machine.
 **************************************************************************/
void port_mgr_fsm_actions(bf_dev_id_t dev_id, bf_dev_port_t dev_port,
    bf_fsm_st state) {
  port_mgr_port_callback_t fn;
  void *userdata;

  port_mgr_dev_port_cb_get(dev_id, &fn, &userdata);

  // notify other modules (internal cb)
  if (fn) {
    fn(dev_id, dev_port, PORT_MGR_PORT_FSM_TRANSITION, (uint32_t) state,
	userdata);
  }
}

/**************************************************************************
 * port_mgr_presence_actions
 *
 * All actions to be effected upon an update to a port's presence-detect bit
 **************************************************************************/
void port_mgr_presence_actions(bf_dev_id_t dev_id, bf_dev_port_t dev_port,
    bool presence) {
  port_mgr_port_callback_t fn;
  void *userdata;

  port_mgr_dev_port_cb_get(dev_id, &fn, &userdata);

  if (fn) {
    fn(dev_id, dev_port, PORT_MGR_PORT_PRESENCE_UPDATE, (uint32_t) presence,
	userdata);
  }
}

/** \brief port_mgr_port_read_counter
 *         HW (sync) MAC counter read
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_por t: physical port # on dev_id (0..LLD_MAX_PORTS-1)
 * \param ctr_id   : RMON counter id
 * \param ctr_value: returned counter value
 *
 * \return: BF_SUCCESS : port removed successfully
 * \return: BF_EAGAIN  : MAC ctr read error, try again
 *
 */
bf_status_t port_mgr_port_read_counter(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port,
                                       bf_rmon_counter_t ctr_id,
                                       uint64_t *ctr_value) {
  int ctr_rd_fail = 0;

  if (port_mgr_dev_is_tof1(dev_id)) {
    ctr_rd_fail =
        port_mgr_mac_read_counter(dev_id, dev_port, ctr_id, ctr_value);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    ctr_rd_fail =
        port_mgr_tof2_port_read_counter(dev_id, dev_port, ctr_id, ctr_value);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    ctr_rd_fail =
        port_mgr_tof3_port_read_counter(dev_id, dev_port, ctr_id, ctr_value);
  }

  return (ctr_rd_fail ? BF_EAGAIN : BF_SUCCESS);
}

/** \brief port_mgr_port_read_all_counters into the MAC stat cache
 *         HW (sync) MAC counter read
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_por t: physical port # on dev_id (0..LLD_MAX_PORTS-1)
 *
 * \return: BF_SUCCESS : port removed successfully
 * \return: BF_EAGAIN  : MAC ctr read error, try again
 *
 */
bf_status_t port_mgr_port_read_all_counters(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port) {
  bf_rmon_counter_t ctr_id;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  // read counters into mac stat cache
  for (ctr_id = 0; ctr_id < BF_NUM_RMON_COUNTERS; ctr_id++) {
    int ctr_rd_failed = 1, tries = 0, max_attempts = 3;

    while (ctr_rd_failed && (tries++ < max_attempts)) {
      ctr_rd_failed = port_mgr_port_read_counter(
          dev_id,
          dev_port,
          ctr_id,
          &port_p->mac_stat_cache.format.ctr_array[ctr_id]);
    }
    if (ctr_rd_failed) {
      return BF_HW_COMM_FAIL;
    }
  }
  return BF_SUCCESS;
}

uint port_mgr_max_frame_sz_get(bf_dev_id_t dev_id) {
  if (port_mgr_dev_is_tof1(dev_id)) {
    // limit MTU on Tofino to avoid a TM issue
    return PORT_MGR_TOF_MAX_FRAME_SZ;
  }
  return PORT_MGR_TOF_DEFLT_MAX_FRAME_SZ;
}

void port_mgr_set_default_port_state(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port) {
  port_mgr_port_t *port_p =
      port_mgr_map_dev_port_to_port_allow_unassigned(dev_id, dev_port);
  if (port_p == NULL) {
    port_mgr_log("%s:%d Map dev %d port %d to port_p failed",
                 __func__,
                 __LINE__,
                 dev_id,
                 dev_port);
    return;
  }

  port_p->sw.assigned = 0;
  port_p->sw.enabled = 0;
  port_p->sw.oper_state = 0;
  port_p->sw.speed = 0;
  port_p->sw.fec = 0;
  port_p->sw.prbs_mode = BF_PORT_PRBS_MODE_NONE;
  port_p->sw.fc_corr_en = true;  // for FC fec must be programmed in reset
  port_p->sw.fc_ind_en = true;   // ..
  port_p->sw.tx_mtu = PORT_MGR_TOF_MAX_FRAME_SZ;  // restrict to 10K on Tofino
  port_p->sw.rx_mtu = PORT_MGR_TOF_MAX_FRAME_SZ;  // restrict to 10K on Tofino
  port_p->sw.rx_max_jab_sz = PORT_MGR_TOF_DEFLT_MAX_JAB_SZ;
  port_p->sw.ifg = 12;
  port_p->sw.preamble_length = 8;
  port_p->sw.promiscuous_mode = 1;
  port_p->sw.mac_addr[0] = 0;
  port_p->sw.mac_addr[1] = 0;
  port_p->sw.mac_addr[2] = 0;
  port_p->sw.mac_addr[3] = 0;
  port_p->sw.mac_addr[4] = 0;
  port_p->sw.mac_addr[5] = 0;
  port_p->sw.link_pause_tx = 0;
  port_p->sw.link_pause_rx = 0;
  port_p->sw.pfc_pause_tx = 0;
  port_p->sw.pfc_pause_rx = 0;
  port_p->sw.loopback_enabled = 0;
  port_p->sw.dfe_type = 0;
  port_p->sw.xoff_pause_time = 0xffff;
  port_p->sw.xon_pause_time = 0;

  // flow control dst mac
  port_p->sw.fc_dst_mac_addr[0] = 1;
  port_p->sw.fc_dst_mac_addr[1] = 0x80;
  port_p->sw.fc_dst_mac_addr[2] = 0xc2;
  port_p->sw.fc_dst_mac_addr[3] = 0;
  port_p->sw.fc_dst_mac_addr[4] = 0;
  port_p->sw.fc_dst_mac_addr[5] = 1;
  // flow control src mac
  port_p->sw.fc_src_mac_addr[0] = 0;
  port_p->sw.fc_src_mac_addr[1] = 0;
  port_p->sw.fc_src_mac_addr[2] = 0;
  port_p->sw.fc_src_mac_addr[3] = 0;
  port_p->sw.fc_src_mac_addr[4] = 0;
  port_p->sw.fc_src_mac_addr[5] = 0;
  // txff trunc_ctrl
  port_p->sw.txff_trunc_ctrl_size = 0;
  port_p->sw.txff_trunc_ctrl_en = 0;
  // txff ctrl
  port_p->sw.txff_ctrl_crc_check_disable = 1;
  port_p->sw.txff_ctrl_crc_removal_disable = 0;
  port_p->sw.txff_ctrl_fcs_insert_disable = 0;
  port_p->sw.txff_ctrl_pad_disable = 0;

  // non-recoverable fields
  port_p->an_enabled = 0;
  port_p->last_up_time.tv_sec = 0;
  port_p->last_up_time.tv_usec = 0;
  port_p->last_dn_time.tv_sec = 0;
  port_p->last_dn_time.tv_usec = 0;

  // an stats
  port_p->an_stats.an_lt_dur_us_cache = 0;
  port_p->an_stats.an_lt_dur_us = 0;
  port_p->an_stats.an_lt_start_s = 0;
  port_p->an_stats.an_lt_start_ns = 0;
  port_p->an_stats.an_try_count = 0;

  // serdes upgrade flag
  port_p->serdes_upgrade_required = false;
  port_p->fec_reset_inp = false;

  // set pcs-near loopback override to false
  port_p->forced_pcs_loopback = false;

  // dont modify callback fields
}

/*****************************************************************************
 * port_mgr_get_num_lanes
 *
 * Internal function to return the number of "logical" lanes on a port.
 * Intention is to be called to determine "for loop" limits.
 *
 *****************************************************************************/
int port_mgr_get_num_lanes(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  if (port_mgr_dev_is_tof1(dev_id)) {
    return port_mgr_tof1_get_num_lanes(dev_id, dev_port);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    return port_mgr_tof2_get_num_lanes(dev_id, dev_port);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    return port_mgr_tof3_get_num_lanes(dev_id, dev_port);
  }

  return 0;
}

/********************************************************************
 * port_mgr_port_bind_interrupt_callback
 *
 * Bind an interrupt callback to a Tofino port. The passed callback
 * will be issued for any interrupt the caller is interested
 * in (via port_mgr_mac_interrupt_registration). The callback will be
 * issued from within port_mgr_mac_interrupt_poll().
 *
 * The opaque "userdata" will be returned in the callback.
 *
 * The callback may be "un-bound" using this API with NULL for the
 * "fn" parameter.
 ********************************************************************/
bf_status_t port_mgr_port_bind_interrupt_callback(bf_dev_id_t dev_id,
                                                  bf_dev_port_t dev_port,
                                                  bf_port_int_callback_t fn,
                                                  void *userdata) {
  port_mgr_port_t *port_p;

  port_p = port_mgr_map_dev_port_to_port_allow_unassigned(dev_id, dev_port);
  if (port_p == NULL) {
    return BF_INVALID_ARG;
  }
  port_p->mac_int_cb = fn;
  port_p->mac_int_userdata = userdata;
  return BF_SUCCESS;
}

void port_mgr_handle_port_int_notif(bf_dev_id_t dev_id) {
  int mac_block, ch;
  port_mgr_port_t *port_p;
  bf_status_t bf_status;
  port_mgr_ldev_t *dev_p = port_mgr_dev_logical_dev_get(dev_id);

  if (port_mgr_dev_is_tof3(dev_id)) {
    port_mgr_tofino3_handle_port_int_notif(dev_id);
    return;
  }

  if (!bf_sys_compare_and_swap(&dev_p->port_int_flag, 1, 0)) {
    return;  // this device didnt generate any port interrupt
  }

  for (mac_block = 0; mac_block < lld_get_max_mac_blocks(dev_id); mac_block++) {
    bf_dev_port_t dev_port;
    for (ch = 0; ch < lld_get_chnls_per_mac(dev_id); ch++) {
      lld_err_t err =
          lld_sku_map_mac_ch_to_dev_port_id(dev_id, mac_block, ch, &dev_port);
      if (err != LLD_OK) continue;  // invalid mac-block for sku

      port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
      if (port_p == NULL) continue;  // assoc port is disabled or not added

      bf_status = bf_port_intr_oper_state_callbacks_issue(dev_id, dev_port);
      if (bf_status != BF_SUCCESS) {
        port_mgr_log("%d:%3d: Unable to issue port sts chg callback : %s (%d)",
                     dev_id,
                     dev_port,
                     bf_err_str(bf_status),
                     bf_status);
      }
    }
  }
}

void port_mgr_tofino3_handle_port_int_notif(bf_dev_id_t dev_id) {
  int mac_block, ch;
  port_mgr_port_t *port_p;
  bf_status_t bf_status;
  port_mgr_ldev_t *dev_p = port_mgr_dev_logical_dev_get(dev_id);

  if (!bf_sys_compare_and_swap(&dev_p->port_int_flag, 1, 0)) {
    return;  // this device didnt generate any port interrupt
  }

  for (mac_block = 0; mac_block < lld_get_max_mac_blocks(dev_id); mac_block++) {
    bf_dev_port_t dev_port;
    int max_ch = 2;
    if (mac_block != 0) {
      max_ch = lld_get_chnls_per_mac(dev_id);
    } else {
      max_ch = 2;
    }

    for (ch = 0; ch < max_ch; ch++) {
      lld_err_t err =
          lld_sku_map_mac_ch_to_dev_port_id(dev_id, mac_block, ch, &dev_port);
      if (err != LLD_OK) continue;  // invalid mac-block for sku

      port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
      if (port_p == NULL) continue;  // assoc port is disabled or not added

      bf_status = bf_port_intr_oper_state_callbacks_issue(dev_id, dev_port);
      if (bf_status != BF_SUCCESS) {
        port_mgr_log("%d:%3d: Unable to issue port sts chg callback : %s (%d)",
                     dev_id,
                     dev_port,
                     bf_err_str(bf_status),
                     bf_status);
      }
    }
  }
}

void port_mgr_port_default_int_bh_wakeup_cb(bf_dev_id_t dev_id) {
  port_mgr_ldev_t *dev_p = port_mgr_dev_logical_dev_get(dev_id);
  bf_sys_compare_and_swap(&dev_p->port_int_flag, 0, 1);
  port_mgr_wakeup_intbh();
}

bf_status_t port_mgr_port_bind_int_bh_wakeup_callback(
    bf_dev_id_t dev_id, bf_port_intbh_wakeup_callback_t fn) {
  port_mgr_ldev_t *dev_p = port_mgr_dev_logical_dev_get(dev_id);

  if (dev_p) {
    dev_p->port_intbh_wakeup_cb = fn;
    return BF_SUCCESS;
  }
  bf_sys_assert(0);  // hear about it

  return BF_INVALID_ARG;
}

void port_mgr_port_int_bh_wakeup(bf_dev_id_t dev_id) {
  port_mgr_ldev_t *dev_p = port_mgr_dev_logical_dev_get(dev_id);

  if (dev_p && dev_p->port_intbh_wakeup_cb) {
    /* Verify whether Top half of mac interrupt processing has enabled bottom
     * half */
    if (dev_p->port_intr_bhalf_valid == true) {
      dev_p->port_intbh_wakeup_cb(dev_id);
      dev_p->port_intr_bhalf_valid = false;
    }
  }
}

/********************************************************************
 * port_mgr_port_config_get
 *
 ********************************************************************/
bf_status_t port_mgr_port_config_get(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port) {
  if (port_mgr_dev_is_tof1(dev_id)) {
    return BF_INVALID_ARG;
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    return port_mgr_tof2_port_config_get(dev_id, dev_port);
  } else {
    return BF_INVALID_ARG;
  }
}

/********************************************************************
 * port_mgr_port_bring_up_time_get
 *
 * if port not up, returns 0
 ********************************************************************/
bf_status_t port_mgr_port_bring_up_time_get(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port,
                                            uint64_t *bring_up_time_us) {
  port_mgr_port_t *port_p;
  uint64_t t_us = 0ull;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;
  if (bring_up_time_us == NULL) return BF_INVALID_ARG;

  t_us = timeval_subtract(&port_p->last_up_time, &port_p->last_en_time);
  if (t_us & (1ull << 63ull)) {
    t_us = 0ull;  // up < enable, port not up
  }
  if (port_p->sw.oper_state == 0) {  // Dn
    t_us = 0ull;
  }
  *bring_up_time_us = t_us;
  return BF_SUCCESS;
}
// last_sig_detect_time

/********************************************************************
 * port_mgr_port_link_up_time_get
 *
 * if port not up, returns 0
 ********************************************************************/
bf_status_t port_mgr_port_link_up_time_get(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           uint64_t *bring_up_time_us) {
  port_mgr_port_t *port_p;
  uint64_t t_us = 0ull;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;
  if (bring_up_time_us == NULL) return BF_INVALID_ARG;

  t_us = timeval_subtract(&port_p->last_up_time, &port_p->last_sig_detect_time);
  if (t_us & (1ull << 63ull)) {
    t_us = 0ull;  // up < enable, port not up
  }
  if (port_p->sw.oper_state == 0) {  // Dn
    t_us = 0ull;
  }
  *bring_up_time_us = t_us;
  return BF_SUCCESS;
}

/********************************************************************
 * port_mgr_port_signal_detect_time_set
 *
 * if port not up, returns 0
 ********************************************************************/
bf_status_t port_mgr_port_signal_detect_time_set(bf_dev_id_t dev_id,
                                                 bf_dev_port_t dev_port) {
  port_mgr_port_t *port_p;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  gettimeofday(&port_p->last_sig_detect_time, NULL);
  return BF_SUCCESS;
}

/********************************************************************
 * port_mgr_port_an_lt_start_time_set
 *
 * if port not up, returns 0
 ********************************************************************/
bf_status_t port_mgr_port_an_lt_start_time_set(bf_dev_id_t dev_id,
                                               bf_dev_port_t dev_port) {
  port_mgr_port_t *port_p;
  struct timespec ts;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  clock_gettime(CLOCK_REALTIME, &ts);
  port_p->an_stats.an_lt_start_ns = ts.tv_nsec;
  port_p->an_stats.an_lt_start_s = ts.tv_sec;

  return BF_SUCCESS;
}

/********************************************************************
 * port_mgr_port_an_lt_dur_set
 *
 * if port not up, returns 0
 ********************************************************************/
bf_status_t port_mgr_port_an_lt_dur_set(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port) {
  port_mgr_port_t *port_p;
  struct timespec ts;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  clock_gettime(CLOCK_REALTIME, &ts);

  port_p->an_stats.an_lt_dur_us = (ts.tv_sec * 1000000 + ts.tv_nsec / 1000) -
                                  (port_p->an_stats.an_lt_start_s * 1000000 +
                                   port_p->an_stats.an_lt_start_ns / 1000);

  return BF_SUCCESS;
}

/********************************************************************
 * port_mgr_port_an_lt_stats_get
 *
 * if port not up, returns 0
 ********************************************************************/
bf_status_t port_mgr_port_an_lt_stats_get(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port,
                                          uint64_t *an_lt_dur_us,
                                          uint32_t *an_try_cnt) {
  port_mgr_port_t *port_p;

  if (!an_lt_dur_us) return BF_INVALID_ARG;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  if (an_lt_dur_us) {
    if (port_p->an_stats.an_lt_dur_us) {
      *an_lt_dur_us = port_p->an_stats.an_lt_dur_us;
    } else {
      *an_lt_dur_us = port_p->an_stats.an_lt_dur_us_cache;
    }
  }

  if (an_try_cnt) {
    *an_try_cnt = port_p->an_stats.an_try_count;
  }

  return BF_SUCCESS;
}

/********************************************************************
 * port_mgr_port_an_try_inc
 *
 * if port not up, returns 0
 ********************************************************************/
bf_status_t port_mgr_port_an_try_inc(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port) {
  port_mgr_port_t *port_p;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  // saturate auto-neg try counter at 0xffffffff
  if (port_p->an_stats.an_try_count < 0xFFFFFFFF) {
    port_p->an_stats.an_try_count++;
  }

  return BF_SUCCESS;
}

/********************************************************************
 * port_mgr_port_an_lt_stats_init
 *
 * if port not up, returns 0
 ********************************************************************/
bf_status_t port_mgr_port_an_lt_stats_init(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           bool init_all) {
  port_mgr_port_t *port_p;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  if (init_all) {
    port_p->an_stats.an_lt_dur_us_cache = 0;
    port_p->an_stats.an_try_count = 0;
  } else {
    port_p->an_stats.an_lt_dur_us_cache = port_p->an_stats.an_lt_dur_us;
  }

  port_p->an_stats.an_lt_dur_us = 0;
  port_p->an_stats.an_lt_start_s = 0;
  port_p->an_stats.an_lt_start_ns = 0;

  return BF_SUCCESS;
}
