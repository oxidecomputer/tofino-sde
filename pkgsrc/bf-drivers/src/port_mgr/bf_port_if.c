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
#include <port_mgr/bf_tof2_serdes_if.h>
#include <port_mgr/bf_tof3_serdes_if.h>
#include <port_mgr/port_mgr_port_evt.h>
#include <port_mgr/port_mgr_ha.h>
#include <port_mgr/port_mgr.h>
#include <port_mgr/port_mgr_dev.h>
#include <port_mgr/port_mgr_map.h>
#include <port_mgr/port_mgr_port.h>
#include <port_mgr/port_mgr_log.h>
#include "port_mgr_mac_stats.h"
#include "port_mgr_tof1/port_mgr_mac.h"
#include "port_mgr_tof1/port_mgr_tof1_port.h"
#include "port_mgr_tof1/port_mgr_serdes.h"
#include "port_mgr_tof2/port_mgr_tof2_dev.h"
#include "port_mgr_tof2/port_mgr_tof2_port.h"
#include "port_mgr_tof2/port_mgr_tof2_umac.h"
#include "port_mgr_tof2/port_mgr_tof2_umac4.h"
#include "port_mgr_tof2/port_mgr_tof2_serdes.h"
#include "port_mgr_tof2/umac4_ctrs.h"
#include "port_mgr_tof3/port_mgr_tof3_port.h"
#include "port_mgr_tof3/port_mgr_tof3_tmac.h"
#include <bf_pm/bf_pm.h>
#include <tofino/bf_pal/bf_pal_port_intf.h>

/**
 * @file bf_port_if.c
 * \brief Details Port-level APIs.
 *
 */

//-----------------------------------------------------------------
// This file contains all APIs for operating the MAC and serdes
// hardware.
//
//   - MAC APIs take the combination of { dev_id, dev_port } to
//     identify the specific hardware to program.
//
//   - Serdes APIs take { dev_id, dev_port, lane } for the same.
//
// APIs fall in to three categories:
//
// "pre-enable API"  (PRE_ENABLE)
// "post-enable API" (POST_ENABLE)
// "unrestricted API"
//
// APIs expected to be used in a certain mode are labelled in the
// comment block immediately above them.
//
// No restriction is placed on calling the PRE_ENABLE or POST_ENABLE
// APIs but they may not function properly if called in the wrong state.
//
//
// "pre-enable API" (PRE_ENABLE)
//
//    These APIs modify configuration values that determine what happens
//    upon entering the "enabled" state (such as whether or not auto-
//    negotiation is run). These APIs will only function as expected if
//    the port is not already enabled. In other words, if such an API is
//    called while the port is already enabled it will NOT be disabled and
//    re-enabled automatically as this could result in a link-flap and it
//    is intended that any link-flap due to configuration changes should be
//    explicitly commanded by the user via port disable or
//    port remove.
//
// "post-enable API" (POST_ENABLE)
//
//    These APIs modify configuration values that require the port to be
//    enabled to function correctly (such as running DFE or link-training).
//
// "unrestricted API"
//    These APIs may be called in either enabled or disabled states.
//    Most of the unrestricted APIs are serdes diagnostic or debug related.
//
//--------------------------------------------------------------------------

/**
 * @addtogroup lld-port-api
 * @{
 * APIs for operating on logical ports
 */

/*****************************************************************************
 * bf_port_mgr_init
 *****************************************************************************/
bf_status_t bf_port_mgr_init(void) {
  port_mgr_init();
  return BF_SUCCESS;
}

bf_status_t bf_port_mgr_intbh_init(void) {
  bf_status_t st = BF_SUCCESS;

  st = port_mgr_intbh_init();
  if (st) return st;

  return BF_SUCCESS;
}

bf_status_t bf_port_check_port_int(void) { return port_mgr_check_port_int(); }

bf_status_t bf_port_handle_port_int_notif(bf_dev_id_t dev_id) {
  port_mgr_handle_port_int_notif(dev_id);
  return BF_SUCCESS;
}

static inline void bf_port_mac_stats_mtx_lock(bf_dev_id_t dev_id,
                                              bf_dev_port_t dev_port) {
  int mac_block, idx;
  port_mgr_mac_block_t *mac_block_p;

  idx = 0;  // irrespective of ch, lock at mac level.
  port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &mac_block, &idx, NULL);
  mac_block_p =
      port_mgr_map_idx_to_mac_block_allow_unassigned(dev_id, mac_block);
  if (mac_block_p != NULL) {
    bf_sys_mutex_lock(&mac_block_p->mac_stats_mtx);
  }
}

static inline void bf_port_mac_stats_mtx_unlock(bf_dev_id_t dev_id,
                                                bf_dev_port_t dev_port) {
  int mac_block, idx;
  port_mgr_mac_block_t *mac_block_p;

  idx = 0;  // irrespective of ch, unlock at mac level.
  port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &mac_block, &idx, NULL);
  mac_block_p =
      port_mgr_map_idx_to_mac_block_allow_unassigned(dev_id, mac_block);
  if (mac_block_p != NULL) {
    bf_sys_mutex_unlock(&mac_block_p->mac_stats_mtx);
  }
}

/** \brief Bind a callback function to be invoked on a port status change
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 * \param fn       : fn to call on port status chg (may be NULL)
 * \param userdata : user-defined cookie passed to fn (may be NULL)
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_bind_status_change_cb(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port,
                                          bf_port_callback_t fn,
                                          void *userdata) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  port_p->sts_chg_cb = fn;
  port_p->sts_chg_userdata = userdata;
  return BF_SUCCESS;
}

/** \brief Bind a callback function to be invoked when MAC
 *         interrupts are detected.
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 * \param fn       : fn to call if MAC interrupt asserts
 * \param userdata : user-defined cookie passed to fn (may be NULL)
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_bind_mac_interrupt_cb(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port,
                                          bf_port_int_callback_t fn,
                                          void *userdata) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  port_p->mac_int_cb = fn;
  port_p->mac_int_userdata = userdata;
  return BF_SUCCESS;
}

/** \brief Set the FEC mode to use on next port-enb
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param fec     : FEC mode
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: fec (return value pointer) NULL
 *
 */
bf_status_t bf_port_fec_mode_set(bf_dev_id_t dev_id,
                                 bf_dev_port_t dev_port,
                                 bf_fec_type_t fec) {
  bf_status_t bf_status = BF_SUCCESS;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  bool is_valid;

  if (port_p == NULL) return BF_INVALID_ARG;
  if (!port_mgr_dev_is_tof2(dev_id) && !port_mgr_dev_is_tof3(dev_id))
    return BF_INVALID_ARG;

  bf_status = bf_port_fec_type_validate(
      dev_id, dev_port, port_p->sw.speed, fec, &is_valid);
  if (bf_status != BF_SUCCESS) return bf_status;
  if (!is_valid) return BF_INVALID_ARG;

  port_p->sw.fec = fec;
  return BF_SUCCESS;
}

/** \brief Set MTU on a Tofino port
 *         Frames (sent or recieved) larger than these values will
 *         be truncated and flagged as errors.
 *         MTU must be specified as a multiple of the datapath width
 *         in bytes:
 *
 *             100G  : multiple of 16
 *             40/50G: multiple of 8
 *             10/25G: multiple of 4
 *             1G    : multiple of 1
 *
 * Note: On Tofino, MTU is restricted to  limit of 10KB
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 * \param tx_mtu   : Tx Maximum transfer unit length (0-65535)
 * \param rx_mtu   : Rx Maximum transfer unit length (0-65535)
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_mtu_set(bf_dev_id_t dev_id,
                            bf_dev_port_t dev_port,
                            uint32_t tx_mtu,
                            uint32_t rx_mtu) {
  uint32_t max_frm_sz = 0;
  uint32_t rx_max_jab_sz;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  max_frm_sz = port_mgr_max_frame_sz_get(dev_id);

  if (tx_mtu > max_frm_sz) return BF_INVALID_ARG;
  if (rx_mtu > max_frm_sz) return BF_INVALID_ARG;

  // jabber size must be a multiple of the mac internal datapath width
  // we'll just use the max width, 100g=16 and pad mtu to that size (see
  // jira swi-1198)
  //
  // jabber size must be larger than the max frame size. So, use the bigger of
  // both, PORT_MGR_TOF_MAX_JAB_SZ and the calculate value adding  4 to the MTU
  // then round UP to the next 16B boundary.
  rx_max_jab_sz = (rx_mtu + 4 + 15) & 0xfff0;
  if (rx_max_jab_sz < PORT_MGR_TOF_DEFLT_MAX_JAB_SZ) {
    rx_max_jab_sz = PORT_MGR_TOF_DEFLT_MAX_JAB_SZ;
  }

  port_p->sw.tx_mtu = tx_mtu;
  port_p->sw.rx_mtu = rx_mtu;
  port_p->sw.rx_max_jab_sz = rx_max_jab_sz;
  return BF_SUCCESS;
}

/** \brief Return the MTU on a port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param tx_mtu  : returned
 * \param rx_mtu  : returned
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: pause (return value pointer) NULL
 *
 */
bf_status_t bf_port_mtu_get(bf_dev_id_t dev_id,
                            bf_dev_port_t dev_port,
                            uint32_t *tx_mtu,
                            uint32_t *rx_mtu) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (tx_mtu == NULL) return BF_INVALID_ARG;
  if (rx_mtu == NULL) return BF_INVALID_ARG;

  *tx_mtu = port_p->sw.tx_mtu;
  *rx_mtu = port_p->sw.rx_mtu;
  return BF_SUCCESS;
}

/** \brief Return the max MTU value supported by device
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id      : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param max_frm_sz  : returned
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: max_mtu is NULL
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 *
 */
bf_status_t bf_port_max_mtu_get(bf_dev_id_t dev_id, uint32_t *max_mtu) {
  if (max_mtu == NULL) return BF_INVALID_ARG;
  if (port_mgr_map_dev_id_to_ldev_p(dev_id) == NULL) return BF_INVALID_ARG;
  *max_mtu = port_mgr_max_frame_sz_get(dev_id);
  return BF_SUCCESS;
}

/** \brief Set Inter-Frame gap on a Tofino port
 *         The IFG is (optionally) checked against
 *         IEEE defined ranges depending on port speed. if
 *         the caller wishes to set a custom (non-IEEE) value
 *         then the "ieee" parameter may be passed as 0.
 *            > 10G: 12-63
 *           == 10G: 8-63
 *            < 10G: 1-63
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 * \param ifg      : inter-frame gap (in bytes)
 * \param ieee     : check against IEEE 802.3 allowed ranges
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: invalid ifg for current speed
 *
 */
bf_status_t bf_port_ifg_set(bf_dev_id_t dev_id,
                            bf_dev_port_t dev_port,
                            uint32_t ifg,
                            bool ieee) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  if (ieee) {
    switch (port_p->sw.speed) {
      /* TBD: For now assume 200/400 are the same as 100g */
      case BF_SPEED_400G:
      case BF_SPEED_200G:

      case BF_SPEED_100G:
      case BF_SPEED_50G:
      case BF_SPEED_40G:
        if ((ifg < 12) || (ifg > 63)) {
          return BF_INVALID_ARG;
        }
        break;
      case BF_SPEED_25G:
      case BF_SPEED_10G:
        if ((ifg < 8) || (ifg > 63)) {
          return BF_INVALID_ARG;
        }
        break;
      case BF_SPEED_1G:
        if ((ifg < 1) || (ifg > 63)) {
          return BF_INVALID_ARG;
        }
        break;
      default:
        bf_sys_assert(0);
    }
  }
  port_p->sw.ifg = ifg;
  return BF_SUCCESS;
}

/** \brief Set non-standard preamble length on a Tofino port
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id         : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port       : encoded port identifier
 * \param preamble_length: preamble-length (in bytes)
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG : dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG : invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG : invalid ifg for current speed
 *
 */
bf_status_t bf_port_preamble_length_set(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        uint32_t preamble_length) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  switch (port_p->sw.speed) {
    /* TBD: For now assume 200/400 are the same as 100g */
    case BF_SPEED_400G:
    case BF_SPEED_200G:

    case BF_SPEED_100G:
    case BF_SPEED_50G:
    case BF_SPEED_40G:
    case BF_SPEED_25G:
    case BF_SPEED_10G:
      if ((preamble_length != 8) && (preamble_length != 4)) {
        return BF_INVALID_ARG;
      }
      break;
    case BF_SPEED_1G:
      if ((preamble_length < 1) || (preamble_length > 8)) {
        return BF_INVALID_ARG;
      }
      break;
    default:
      bf_sys_assert(0);
  }
  port_p->sw.preamble_length = preamble_length;
  return BF_SUCCESS;
}

/** \brief Set or clear promiscuous mode on a Tofino port.
 *         Default is "enabled".
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param en      : false=disabled, true=enabled
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_promiscuous_mode_set(bf_dev_id_t dev_id,
                                         bf_dev_port_t dev_port,
                                         bool en) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  port_p->sw.promiscuous_mode = en ? 1 : 0;
  return BF_SUCCESS;
}

/** \brief Set the MAC address on a Tofino port
 *         Used when not in promiscuous mode.
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param mac_addr: 6-byte mac address
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_mac_address_set(bf_dev_id_t dev_id,
                                    bf_dev_port_t dev_port,
                                    uint8_t *mac_addr) {
  int i;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (mac_addr == NULL) return BF_INVALID_ARG;

  for (i = 0; i < 6; i++) {
    port_p->sw.mac_addr[i] = mac_addr[i];
  }
  return BF_SUCCESS;
}

/** \brief Set the DFE type on a Tofino port
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param type    : BF_DFE_TYP_NONE
 *                : BF_DFE_TYP_COARSE
 *                : BF_DFE_TYP_FINE
 *                : BF_DFE_TYP_CONTINUOUS
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: lane > 3
 *
 */
bf_status_t bf_port_dfe_type_set(bf_dev_id_t dev_id,
                                 bf_dev_port_t dev_port,
                                 bf_dfe_type_e type) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  switch (type) {
    case BF_DFE_TYP_NONE:
    case BF_DFE_TYP_COARSE:
    case BF_DFE_TYP_FINE:
    case BF_DFE_TYP_CONTINUOUS:
      break;
    default:
      return BF_INVALID_ARG;
  }
  port_p->sw.dfe_type = type;
  return BF_SUCCESS;
}

/** \brief Return if loopback is enabled or not on a Tofino port
 *
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 *
 * \return enb    : true if any loopback is set for the port; false otherwise
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_is_loopback_enb(bf_dev_id_t dev_id,
                                    bf_dev_port_t dev_port,
                                    bool *enb) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  *enb = port_p->sw.loopback_enabled;

  return BF_SUCCESS;
}

/** \brief Set or clear loopback mode on a Tofino port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param mode    : loopback mode,
 *                : BF_LPBK_NONE, undo all loopbacks
 *                : BF_LPBK_MAC_NEAR, MAC Tx to Rx
 *                : BF_LPBK_MAC_FAR, MAC Rx to Tx
 *                : BF_LPBK_PCS_NEAR, PCS Tx to Rx
 *                : BF_LPBK_SERDES_NEAR, Serdes Tx to Rx
 *                : BF_LPBK_SERDES_FAR, serdes Rx to Tx
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: invalid loopback mode
 *
 */
bf_status_t bf_port_loopback_mode_set(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      bf_loopback_mode_e mode) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  bf_status_t rc = BF_SUCCESS;

  if (port_p == NULL) return BF_INVALID_ARG;

  switch (mode) {
    case BF_LPBK_NONE:
    case BF_LPBK_MAC_NEAR:
    case BF_LPBK_MAC_FAR:
    case BF_LPBK_PCS_NEAR:
    case BF_LPBK_SERDES_NEAR:
    case BF_LPBK_SERDES_FAR:
    case BF_LPBK_PIPE:
      break;
    default:
      return BF_INVALID_ARG;
  }
  port_p->sw.loopback_enabled = (mode == BF_LPBK_NONE) ? false : true;
  port_p->sw.lpbk_mode = mode;

  /* set (or unset) MAC loopbacks */
  switch (mode) {
    case BF_LPBK_NONE:
    case BF_LPBK_MAC_NEAR:
    case BF_LPBK_MAC_FAR:
    case BF_LPBK_PCS_NEAR:
    case BF_LPBK_PIPE:
      if (port_mgr_dev_is_tof2(dev_id)) {
        /* Loopback mode is usually passed to port_mgr from the FSM on TF2
         * however during fast reconfig bf_pm will pass the SW config to
         * port_mgr to save in SW state (port_p->sw.*) so in this case return
         * now since that is done before we continue and touch HW. */
        port_mgr_ldev_t *dev_p = port_mgr_dev_logical_dev_get(dev_id);
        if (dev_p && dev_p->ha_stage == PORT_MGR_HA_CFG_REPLAY)
          return BF_SUCCESS;

        rc = port_mgr_tof2_port_loopback_set(dev_id, dev_port, mode);
        return rc;
      } else if (port_mgr_dev_is_tof3(dev_id)) {
        rc = port_mgr_tof3_port_loopback_set(dev_id, dev_port, mode);
        return rc;
      }
      if (mode == BF_LPBK_PIPE) return BF_INVALID_ARG;  // tof2-only mode
      port_mgr_mac_set_loopback_mode(dev_id, dev_port, mode);
      break;
    default:
      break;
  }
  /* set (or unset) serdes loopbacks */
  switch (mode) {
    case BF_LPBK_NONE:
    case BF_LPBK_SERDES_NEAR:
    case BF_LPBK_SERDES_FAR: {
      int ln;
      int num_lanes = port_mgr_get_num_lanes(dev_id, dev_port);

#ifdef UTEST  // aapl would return an error if accessed
      return BF_SUCCESS;
#endif

      for (ln = 0; ln < num_lanes; ln++) {
        bf_status_t bf_status = BF_SUCCESS;
        bool is_sw_model = false;
        bf_sds_loopback_t loopback_mode;

        loopback_mode = (mode == BF_LPBK_SERDES_NEAR)
                            ? BF_SDS_LB_SER_TX_TO_RX
                            : (mode == BF_LPBK_SERDES_FAR)
                                  ? BF_SDS_LB_PAR_RX_TO_TX
                                  : BF_SDS_LB_OFF;

        bf_drv_device_type_get(dev_id, &is_sw_model);

        if (!is_sw_model) {
          // For serdes near loopback need to undo any polarity inversions
          // port will need to be disabled/re-enabled afterwards to recover
          // the inversion settings.
          if (mode == BF_LPBK_SERDES_NEAR) {
            bf_serdes_tx_drv_inv_set(dev_id, dev_port, ln, false);
            bf_serdes_rx_afe_inv_set(dev_id, dev_port, ln, false);
          }

          bf_status =
              bf_serdes_lane_loopback_set(dev_id, dev_port, ln, loopback_mode);
          if (bf_status != BF_SUCCESS) return BF_INVALID_ARG;
        }
      }
      break;
    }
    default:
      break;
  }
  return BF_SUCCESS;
}

/** \brief Return if interrupt should be enabled or not according loopback mode
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param ena_ints: enable interrupts if true, dont enable if false
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: invalid loopback mode
 *
 */
bf_status_t bf_port_mac_interrupt_ena_get(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port,
                                          bool *ena_ints) {
  port_mgr_port_t *port_p = NULL;
  port_mgr_ldev_t *dev_p = NULL;

  if (ena_ints == NULL) return BF_INVALID_ARG;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  dev_p = port_mgr_dev_logical_dev_get(dev_id);
  if (port_p == NULL || dev_p == NULL) {
    *ena_ints = false;
    return BF_INVALID_ARG;
  }

  // determine default interrupt enable option
  *ena_ints = dev_p->interrupt_based_link_monitoring;

  // some loopback modes do not use interrupts
  switch (port_p->sw.lpbk_mode) {
    case BF_LPBK_MAC_NEAR:
    case BF_LPBK_PIPE:
      // do not use interrupt for these modes
      *ena_ints = false;
      break;
    case BF_LPBK_NONE:
    case BF_LPBK_MAC_FAR:
    case BF_LPBK_PCS_NEAR:
    case BF_LPBK_SERDES_NEAR:
    case BF_LPBK_SERDES_FAR:
      break;
    default:
      *ena_ints = false;
      return BF_INVALID_ARG;
  }

  switch (port_p->sw.port_dir) {
    case BF_PORT_DIR_TX_ONLY:
      *ena_ints = false;
      break;
    case BF_PORT_DIR_DUPLEX:
    case BF_PORT_DIR_RX_ONLY:
    case BF_PORT_DIR_DECOUPLED:
      break;
    default:
      *ena_ints = false;
      return BF_INVALID_ARG;
  }

  return BF_SUCCESS;
}

/** \brief Set direction mode on a Tofino port
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param mode    : direction mode. See bf_port_dir_e for supported port
 *                  direction modes.
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: invalid direction mode mode
 *
 */
bf_status_t bf_port_direction_mode_set(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port,
                                       bf_port_dir_e mode) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  if (mode >= BF_PORT_DIR_MAX) return BF_INVALID_ARG;
  port_p->sw.port_dir = mode;

  return BF_SUCCESS;
}

/** \brief Configure Tx Fifo truncation settings on a tofino port
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param size    : size beyond which to truncate, in bytes
 * \param en      : false=disabled, true=enabled
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_txff_truncation_set(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        uint32_t size,
                                        bool en) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  port_p->sw.txff_trunc_ctrl_size = size;
  port_p->sw.txff_trunc_ctrl_en = en;
  return BF_SUCCESS;
}

/** \brief Configure Tx Fifo modes on a tofino port
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id             : system-assigned id (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port           : encoded port identifier
 * \param crc_check_disable  : disable CRC32 check from Deparser
 * \param crc_removal_disable: disable CRC32 removal before MAC
 * \param fcs_insert_disable : disable FCS insertion by MAC
 * \param pad_disable        : disable PAD insertion by MAC
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_txff_mode_set(bf_dev_id_t dev_id,
                                  bf_dev_port_t dev_port,
                                  bool crc_check_disable,
                                  bool crc_removal_disable,
                                  bool fcs_insert_disable,
                                  bool pad_disable) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  port_p->sw.txff_ctrl_crc_check_disable = crc_check_disable;
  port_p->sw.txff_ctrl_crc_removal_disable = crc_removal_disable;
  port_p->sw.txff_ctrl_fcs_insert_disable = fcs_insert_disable;
  port_p->sw.txff_ctrl_pad_disable = pad_disable;
  return BF_SUCCESS;
}

/** \brief Configure Tx Fifo preamble and IPG on a tofino port
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param preamble: ptr to array of 7 bytes containing the preamble
 * \param ipg     : IPG after each packet (0-255)
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_txff_preamble_ipg_set(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port,
                                          uint8_t *preamble,
                                          uint32_t ipg) {
  uint32_t i;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (preamble == NULL) return BF_INVALID_ARG;

  port_p->sw.ipg = ipg;
  for (i = 0; i < sizeof(port_p->sw.preamble); i++) {
    port_p->sw.preamble[i] = preamble[i];
  }
  return BF_SUCCESS;
}

/** \brief Enable or Disable link Tx and Rx link pause
 *         on a Tofino port
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param tx_en   : state to set (False=disable, True=enable)
 * \param rx_en   : state to set (False=disable, True=enable)
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_flow_control_link_pause_set(bf_dev_id_t dev_id,
                                                bf_dev_port_t dev_port,
                                                bool tx_en,
                                                bool rx_en) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  port_p->sw.link_pause_tx = tx_en ? 1 : 0;
  port_p->sw.link_pause_rx = rx_en ? 1 : 0;

  if (port_mgr_dev_is_tof2(dev_id)) {
    return port_mgr_tof2_port_flowcontrol_set(dev_id, dev_port);
  }

  port_mgr_mac_set_flow_control(dev_id, dev_port);

  return BF_SUCCESS;
}

bf_status_t bf_port_flow_control_link_pause_get(bf_dev_id_t dev_id,
                                                bf_dev_port_t dev_port,
                                                bool *tx_en,
                                                bool *rx_en) {
  if (tx_en == NULL) return BF_INVALID_ARG;
  if (rx_en == NULL) return BF_INVALID_ARG;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  *tx_en = (port_p->sw.link_pause_tx == 1 ? true : false);
  *rx_en = (port_p->sw.link_pause_rx == 1 ? true : false);

  return BF_SUCCESS;
}

/** \brief Enable or Disable Per-COS Tx/Rx pause on a Tofino port
 * Per COS bitmap is not supported for Tofino2
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id   : system-assigned identifier *(0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 * \param tx_en_map: per-COS bitmap of state to set Tx
 *                 : (0=disable, 1=enable)
 * \param rx_en_map: per-COS bitmap of state to set Rx
 *                 : (0=disable, 1=enable)
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_flow_control_pfc_set(bf_dev_id_t dev_id,
                                         bf_dev_port_t dev_port,
                                         uint32_t tx_en_map,
                                         uint32_t rx_en_map) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof2(dev_id) &&
      ((rx_en_map != 0xFF) && (rx_en_map != 0x0))) {
    port_mgr_log("Tof2 does not support per COS bitmap enable/disable %d %d",
                 dev_id,
                 dev_port);
    return BF_INVALID_ARG;
  }

  // save in port struct
  port_p->sw.pfc_pause_tx = tx_en_map;
  port_p->sw.pfc_pause_rx = rx_en_map;

  if (port_mgr_dev_is_tof2(dev_id)) {
    return port_mgr_tof2_port_flowcontrol_set(dev_id, dev_port);
  }

  port_mgr_mac_set_flow_control(dev_id, dev_port);

  return BF_SUCCESS;
}

bf_status_t bf_port_flow_control_pfc_get(bf_dev_id_t dev_id,
                                         bf_dev_port_t dev_port,
                                         uint32_t *tx_en_map,
                                         uint32_t *rx_en_map) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  *tx_en_map = port_p->sw.pfc_pause_tx;
  *rx_en_map = port_p->sw.pfc_pause_rx;
  return BF_SUCCESS;
}

/** \brief Set the source MAC address to be transmitted
 *         in flow control frames on a Tofino port
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param mac_addr: ptr to 6-byte mac address
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_flow_control_frame_src_mac_address_set(
    bf_dev_id_t dev_id, bf_dev_port_t dev_port, uint8_t *mac_addr) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (mac_addr == NULL) return BF_INVALID_ARG;

  port_p->sw.fc_src_mac_addr[0] = mac_addr[0];
  port_p->sw.fc_src_mac_addr[1] = mac_addr[1];
  port_p->sw.fc_src_mac_addr[2] = mac_addr[2];
  port_p->sw.fc_src_mac_addr[3] = mac_addr[3];
  port_p->sw.fc_src_mac_addr[4] = mac_addr[4];
  port_p->sw.fc_src_mac_addr[5] = mac_addr[5];
  return BF_SUCCESS;
}

/** \brief Set the destination MAC address to be transmitted
 *         in flow control frames on a Tofino port
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param mac_addr: ptr to 6-byte mac address
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_flow_control_frame_dest_mac_address_set(
    bf_dev_id_t dev_id, bf_dev_port_t dev_port, uint8_t *mac_addr) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (mac_addr == NULL) return BF_INVALID_ARG;

  port_p->sw.fc_dst_mac_addr[0] = mac_addr[0];
  port_p->sw.fc_dst_mac_addr[1] = mac_addr[1];
  port_p->sw.fc_dst_mac_addr[2] = mac_addr[2];
  port_p->sw.fc_dst_mac_addr[3] = mac_addr[3];
  port_p->sw.fc_dst_mac_addr[4] = mac_addr[4];
  port_p->sw.fc_dst_mac_addr[5] = mac_addr[5];
  return BF_SUCCESS;
}

/** \brief Set the XOFF pause time value to be transmitted
 *         in XOFF PAUSE flow control frames on a Tofino port
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id    : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port  : encoded port identifier
 * \param pause_time: pause time (0-0xffff)
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_xoff_pause_time_set(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        uint32_t pause_time) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  port_p->sw.xoff_pause_time = pause_time;
  return BF_SUCCESS;
}

/** \brief Set the XON pause time value to be transmitted
 *         in XON PAUSE flow control frames on a Tofino port
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id    : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port  : encoded port identifier
 * \param pause_time: pause time (0-0xffff)
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_xon_pause_time_set(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port,
                                       uint32_t pause_time) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  port_p->sw.xon_pause_time = pause_time;
  return BF_SUCCESS;
}

bf_port_speeds_t _bf_port_speed_to_an_speed_map(bf_dev_id_t dev_id,
                                                bf_port_speed_t p_speed,
                                                uint32_t n_lanes,
                                                bool adv_kr_mode) {
  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));

  switch (p_speed) {
    case BF_SPEED_1G:
      return BF_ADV_SPD_1000BASE_KX;
    case BF_SPEED_10G:
      return BF_ADV_SPD_10GBASE_KR;
    case BF_SPEED_25G:
      if (dev_family == BF_DEV_FAMILY_TOFINO) {
        return BF_ADV_SPD_25GBASE_KR_CR;
      } else {
        return (BF_ADV_SPD_25GBASE_KR_CR | BF_ADV_SPD_25GBASE_KR1_CONSORTIUM |
                BF_ADV_SPD_25GBASE_CR1_CONSORTIUM);
      }
    case BF_SPEED_50G:
      if (n_lanes == 2) {
        /* 50G (2x25G) IEEE : autoneg not supported */
        return 0;
      } else {
        return BF_ADV_SPD_50GBASE_KR1_CR1;
      }
    case BF_SPEED_50G_CONS:
      return BF_ADV_SPD_50GBASE_CR2_CONSORTIUM;
    case BF_SPEED_40G:
      if (adv_kr_mode) {
        return BF_ADV_SPD_40GBASE_KR4;
      } else {
        return BF_ADV_SPD_40GBASE_CR4;
      }
    case BF_SPEED_100G:
      if (n_lanes == 1) {
        return BF_ADV_SPD_100GBASE_KR1_CR1;
      } else if (n_lanes == 2) {
        return BF_ADV_SPD_100GBASE_KR2_CR2;
      } else {
        if (adv_kr_mode) {
          return BF_ADV_SPD_100GBASE_KR4;
        } else {
          return BF_ADV_SPD_100GBASE_CR4;
        }
      }
    case BF_SPEED_200G:
      if (n_lanes == 2) {
        return BF_ADV_SPD_200GBASE_KR2_CR2;
      } else if (n_lanes == 4) {
        return BF_ADV_SPD_200GBASE_KR4_CR4;
      } else {
        return 0;  // no AN for 200G-R8 (NRZ)
      }
    case BF_SPEED_400G:
      if (n_lanes == 4) {
        return BF_ADV_SPD_400GBASE_KR4_CR4;
      } else {
        return BF_ADV_SPD_400GBASE_CR8_CONSORTIUM;
      }
    default:
      return 0;
  }
  return 0;
}

bf_status_t bf_port_speed_to_an_speed_map(bf_dev_id_t dev_id,
                                          bf_port_speed_t p_speed,
                                          uint32_t n_lanes,
                                          bool adv_kr_mode,
                                          bf_port_speeds_t *adv_speeds) {
  *adv_speeds =
      _bf_port_speed_to_an_speed_map(dev_id, p_speed, n_lanes, adv_kr_mode);
  return BF_SUCCESS;
}

bf_fec_types_t _bf_port_fec_to_an_fec_map(bf_fec_type_t p_fec,
                                          bf_port_speed_t p_speed) {
  if (p_speed == BF_SPEED_10G || p_speed == BF_SPEED_40G) {
    switch (p_fec) {
      case BF_FEC_TYP_FIRECODE:
        return BF_ADV_FEC_FC_10G_ABILITY_IEEE | BF_ADV_FEC_FC_10G_REQUEST_IEEE;
      default:
        return 0;
    }
  } else if (p_speed == BF_SPEED_25G || p_speed == BF_SPEED_100G ||
             p_speed == BF_SPEED_50G || p_speed == BF_SPEED_200G ||
             p_speed == BF_SPEED_400G || p_speed == BF_SPEED_50G_CONS) {
    // FIXME for 200G and 400G
    switch (p_fec) {
      case BF_FEC_TYP_FIRECODE:
        return BF_ADV_FEC_FC_25G_REQUEST_IEEE;
      case BF_FEC_TYP_REED_SOLOMON:
        return BF_ADV_FEC_RS_25G_REQUEST_IEEE;
      case BF_FEC_TYP_NONE:
        return 0;
      default:
        return 0;
    }
  } else if (p_speed == BF_SPEED_1G) {
    return 0;
  }
  return 0;
}

bf_status_t bf_port_fec_to_an_fec_map(bf_fec_type_t p_fec,
                                      bf_port_speed_t p_speed,
                                      bf_fec_types_t *fec_adv) {
  *fec_adv = _bf_port_fec_to_an_fec_map(p_fec, p_speed);
  return BF_SUCCESS;
}

/** \brief Get link-partners clause 73 Autonegotiation base and next page
 *         advertisements on a Tofino port.
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param base_pg : (out) lower 48 bits are base page advertisement
 * \param num_np  : (in/out)
 *                : (in) max number of next page values to retrieve
 *                : (out) number of next pages pointed to by next_pg
 * \param next_pg : (out) pointer to "*num_np" next page values.
 *                : lower 48 bits are next page advertisement(s)
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: advert NULL
 * \return: BF_INVALID_ARG: passed *num_np (on input) not sufficient to
 *                        : hold response on return *num_np is set to
 *                        : the number required
 *
 */
bf_status_t bf_port_autoneg_lp_advert_get(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port,
                                          uint64_t *base_pg,
                                          uint32_t *num_np,
                                          uint64_t *next_pg) {
  uint32_t i;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (base_pg == NULL) return BF_INVALID_ARG;
  if (num_np == NULL) return BF_INVALID_ARG;
  if ((*num_np != 0) && (next_pg == NULL)) return BF_INVALID_ARG;

  // make sure caller allocated enough space. If not, set num_np to the
  // number of next pages we have and return an error
  if (*num_np < port_p->an_lp_num_next_pages) {
    *num_np = port_p->an_lp_num_next_pages;
    return BF_INVALID_ARG;
  }
  *base_pg = port_p->an_lp_base_page;
  *num_np = port_p->an_lp_num_next_pages;
  for (i = 0; i < port_p->an_lp_num_next_pages; i++) {
    next_pg[i] = port_p->an_lp_next_page[i];
  }
  return BF_SUCCESS;
}

/** \brief Set Reed-Solomon FEC control values
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id     : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port   : encoded port identifier
 * \param byp_corr_en: bypass FEC correction enable
 * \param byp_ind_en : bypass FEC error indication (to PCS) enable
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_rs_fec_control_set(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port,
                                       bool byp_corr_en,
                                       bool byp_ind_en) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  port_p->sw.fc_corr_en = byp_corr_en ? 0 : 1;
  port_p->sw.fc_ind_en = byp_ind_en ? 0 : 1;

  // for tof2, these will be applied on the next port enable
  if (port_mgr_dev_is_tof2(dev_id)) {
    return BF_SUCCESS;
  }

  port_mgr_mac_set_rs_fec_control(dev_id, dev_port, byp_corr_en, byp_ind_en);
  return BF_SUCCESS;
}

/** \brief Get Reed-Solomon FEC status and counters
 *         NULL pointer values may beused to skip collection
 *         of individual counters/status values.
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id          : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port        : encoded port identifier
 * \param hi_ser          : 1=FEC symbol errors over 8192 codewords > thresh
 * \param fec_align_status: 1=all lanes are synchronized and aligned
 * \param fec_corr_cnt    : Corrected blocks counter
 * \param fec_uncorr_cnt  : Uncorrected blocks counter
 * \param fec_ser_lane_0  : Symbol error counter, lane 0
 * \param fec_ser_lane_1  : Symbol error counter, lane 1
 * \param fec_ser_lane_2  : Symbol error counter, lane 2
 * \param fec_ser_lane_3  : Symbol error counter, lane 3
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_rs_fec_status_and_counters_get(bf_dev_id_t dev_id,
                                                   bf_dev_port_t dev_port,
                                                   bool *hi_ser,
                                                   bool *fec_align_status,
                                                   uint32_t *fec_corr_cnt,
                                                   uint32_t *fec_uncorr_cnt,
                                                   uint32_t *fec_ser_lane_0,
                                                   uint32_t *fec_ser_lane_1,
                                                   uint32_t *fec_ser_lane_2,
                                                   uint32_t *fec_ser_lane_3,
                                                   uint32_t *fec_ser_lane_4,
                                                   uint32_t *fec_ser_lane_5,
                                                   uint32_t *fec_ser_lane_6,
                                                   uint32_t *fec_ser_lane_7) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof3(dev_id)) {
    uint32_t umac, ch;

    int lanes_per_ch = port_mgr_tof3_tmac_num_lanes_per_ch(dev_id, dev_port);
    port_mgr_map_dev_port_to_all(
        dev_id, dev_port, NULL, NULL, (int *)&umac, (int *)&ch, NULL);
    port_mgr_tof3_tmac_rs_fec_status_and_counters_get(dev_id,
                                                      umac,
                                                      ch,
                                                      lanes_per_ch,
                                                      hi_ser,
                                                      fec_align_status,
                                                      fec_corr_cnt,
                                                      fec_uncorr_cnt,
                                                      fec_ser_lane_0,
                                                      fec_ser_lane_1,
                                                      fec_ser_lane_2,
                                                      fec_ser_lane_3,
                                                      fec_ser_lane_4,
                                                      fec_ser_lane_5,
                                                      fec_ser_lane_6,
                                                      fec_ser_lane_7);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    uint32_t umac, ch;

    port_mgr_map_dev_port_to_all(
        dev_id, dev_port, NULL, NULL, (int *)&umac, (int *)&ch, NULL);
    port_mgr_tof2_umac_rs_fec_status_and_counters_get(dev_id,
                                                      umac,
                                                      ch,
                                                      hi_ser,
                                                      fec_align_status,
                                                      fec_corr_cnt,
                                                      fec_uncorr_cnt,
                                                      fec_ser_lane_0,
                                                      fec_ser_lane_1,
                                                      fec_ser_lane_2,
                                                      fec_ser_lane_3,
                                                      fec_ser_lane_4,
                                                      fec_ser_lane_5,
                                                      fec_ser_lane_6,
                                                      fec_ser_lane_7);
  } else {
    port_mgr_mac_get_rs_fec_status_and_counters(dev_id,
                                                dev_port,
                                                hi_ser,
                                                fec_align_status,
                                                fec_corr_cnt,
                                                fec_uncorr_cnt,
                                                fec_ser_lane_0,
                                                fec_ser_lane_1,
                                                fec_ser_lane_2,
                                                fec_ser_lane_3);
  }
  return BF_SUCCESS;
}

/** \brief Set Firecode FEC control values
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id     : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param port       : encoded port identifier
 * \param byp_corr_en: bypass FEC correction enable
 * \param byp_ind_en : bypass FEC error indication (to PCS) enable
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_fc_fec_control_set(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port,
                                       bool byp_corr_en,
                                       bool byp_ind_en) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  // reverse sense (this is to keep consistent with RS FEC API)
  port_p->sw.fc_corr_en = byp_corr_en ? false : true;
  port_p->sw.fc_ind_en = byp_ind_en ? false : true;
  return BF_SUCCESS;
}

/** \brief Get Firecode FEC status and counters
 *         NULL pointer values may beused to skip collection
 *         of individual counters/status values.
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id           : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port         : encoded port identifier
 * \param block_lock_status: true=all lanes are synchronized and aligned
 * \param fc_enabled       : true=Firecode FEC active on channel
 * \param fec_corr_cnt     : Corrected blocks counter
 * \param fec_uncorr_cnt   : Uncorrected blocks counter
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_fc_fec_status_and_counters_get(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    uint32_t vl,
    bool *block_lock_status,
    uint32_t *fec_corr_blk_cnt,
    uint32_t *fec_uncorr_blk_cnt) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (port_mgr_dev_is_tof1(dev_id)) {
    port_mgr_mac_get_fc_fec_status_and_counters_per_vl(dev_id,
                                                       dev_port,
                                                       vl,
                                                       block_lock_status,
                                                       fec_corr_blk_cnt,
                                                       fec_uncorr_blk_cnt);
  } else {
    // FC FEC counters and status get is not implemented for TF2
    return BF_UNEXPECTED;
  }

  return BF_SUCCESS;
}

/** \brief Set Tx enable mode on a Tofino port
 *         Note: disconnects the MAC from the PCS but the PCS
 *               still transmits idles (/I) so the link partner
 *               will see the link as "up".
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param en      : false=disabled, true=enabled
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_mac_tx_enable_set(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      bool en) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  // only used by the tof1 loopback modes. Should not be called on tof2
  if (port_mgr_dev_is_tof2(dev_id)) {
    return BF_INVALID_ARG;
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    uint32_t rc, tmac, ch;
    rc = port_mgr_map_dev_port_to_all(
        dev_id, dev_port, NULL, NULL, (int *)&tmac, (int *)&ch, NULL);
    if (rc != 0) return BF_INVALID_ARG;
    port_mgr_tof3_tmac_tx_enable(dev_id, tmac, ch, en);
    return BF_SUCCESS;
  }

  port_mgr_mac_set_tx_enable(dev_id, dev_port, en);
  return BF_SUCCESS;
}

/** \brief Set Rx enable mode on a Tofino port
 *         Note: disconnects the MAC from the PCS but the PCS
 *               still transmits idles (/I) so the link partner
 *               will see the link as "up". No incoming packets
 *               passed to MAC.
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param en      : false=disabled, true=enabled
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_mac_rx_enable_set(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      bool en) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  // only used by the tof1 loopback modes. Should not be called on tof2
  if (port_mgr_dev_is_tof2(dev_id)) {
    uint32_t rc, umac, ch;

    rc = port_mgr_map_dev_port_to_all(
        dev_id, dev_port, NULL, NULL, (int *)&umac, (int *)&ch, NULL);
    if (rc != 0) return BF_INVALID_ARG;
    port_mgr_tof2_umac_rx_enable(dev_id, umac, ch, en);
    return BF_SUCCESS;
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    uint32_t rc, umac, ch;

    rc = port_mgr_map_dev_port_to_all(
        dev_id, dev_port, NULL, NULL, (int *)&umac, (int *)&ch, NULL);
    if (rc != 0) return BF_INVALID_ARG;
    port_mgr_tof3_tmac_rx_enable(dev_id, umac, ch, en);
    return BF_SUCCESS;
  }

  port_mgr_mac_set_rx_enable(dev_id, dev_port, en);
  return BF_SUCCESS;
}

/** \brief Set Channel enable on a Tofino port, allowing it to become
 *         operational. And configure the MAC channel interrupts
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param en      : false=disabled, true=enabled
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_mac_ch_enable_set(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      bool en) {
  int mac_block, ch;
  // port_mgr_dev_t *dev_p = port_mgr_map_dev_id_to_dev_p(dev_id);
  port_mgr_ldev_t *dev_p = port_mgr_dev_logical_dev_get(dev_id);
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  // Should not be called on tof2
  if (port_mgr_dev_is_tof2(dev_id)) {
    return BF_INVALID_ARG;
  }

  port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &mac_block, &ch, NULL);

  if (en) {
    if (dev_p->interrupt_based_link_monitoring) {
      // leave ints disabled until port comes up, then only enable LF/RF
      // need to explicitly disable here?
    } else {
      // config and clear interrupts
      port_mgr_mac_basic_interrupt_setup(dev_id, dev_port);
    }
    // release the channel reset
    port_mgr_mac_enable_ch(dev_id, mac_block, ch);
    return BF_SUCCESS;  // only enable lowest numbered ch
  } else {
    // assert the channel reset
    port_mgr_mac_disable_ch(dev_id, mac_block, ch);
  }
  return BF_SUCCESS;
}

/** \brief Set Tx drain mode on a Tofino port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param drain   : false=normal, true=drain-mode
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_mac_tx_drain_set(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port,
                                     bool drain) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  // Should not be called on tof2
  if (port_mgr_dev_is_tof2(dev_id)) {
    return BF_INVALID_ARG;
  }

  port_mgr_mac_set_drain(dev_id, dev_port, drain);
  return BF_SUCCESS;
}

/** \brief Force PCS FAULT to be sent on a Tofino port by engaging rtestmode
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param force   : true=force send fault, false=dont-force
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_force_rtestmode_set(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        bool force) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  // Should not be called on tof2
  if (port_mgr_dev_is_tof2(dev_id)) {
    return BF_INVALID_ARG;
  }

  port_mgr_log("%d:%3d: RTESTMODE = %d", dev_id, dev_port, force ? 1 : 0);

  port_mgr_mac_set_rtestmode(dev_id, dev_port, force);
  return BF_SUCCESS;
}

/** \brief Force LOCAL-FAULT to be sent on a Tofino port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param force   : true=force send local-fault, false=dont-force
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_force_local_fault_set(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port,
                                          bool force) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof2(dev_id)) {
    uint32_t rc, umac, ch;

    rc = port_mgr_map_dev_port_to_all(
        dev_id, dev_port, NULL, NULL, (int *)&umac, (int *)&ch, NULL);
    if (rc != 0) return BF_INVALID_ARG;

    port_mgr_tof2_umac_tx_local_fault_set(dev_id, umac, ch, force);
  } else {
    /*
     * If LF is forced, then auto drain on fault would not
     * work causing TX packets not getting flushed in MAC and
     * in turn resulting in TM buffer build up for the egress ports
     * in admin enabled state but link being down. As per Comira,
     * we can use rtestmode instead of LF to signal fault and
     * this would not affect auto drain functionality.
     */
    return (bf_port_force_rtestmode_set(dev_id, dev_port, force));
  }
  return BF_SUCCESS;
}

/** \brief Force REMOTE-FAULT to be sent on a Tofino port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param force   : true=force send local-fault, false=dont-force
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_force_remote_fault_set(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           bool force) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof2(dev_id)) {
    uint32_t rc, umac, ch;

    rc = port_mgr_map_dev_port_to_all(
        dev_id, dev_port, NULL, NULL, (int *)&umac, (int *)&ch, NULL);
    if (rc != 0) return BF_INVALID_ARG;

    port_mgr_tof2_umac_tx_remote_fault_set(dev_id, umac, ch, force);
  } else {
    port_mgr_mac_set_force_remote_fault(dev_id, dev_port, force);
  }
  return BF_SUCCESS;
}

/** \brief Force IDLE to be sent on a Tofino port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param force   : true=force send local-fault, false=dont-force
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_force_idle_set(bf_dev_id_t dev_id,
                                   bf_dev_port_t dev_port,
                                   bool force) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof2(dev_id)) {
    uint32_t rc, umac, ch;

    rc = port_mgr_map_dev_port_to_all(
        dev_id, dev_port, NULL, NULL, (int *)&umac, (int *)&ch, NULL);
    if (rc != 0) return BF_INVALID_ARG;

    port_mgr_tof2_umac_tx_idle_set(dev_id, umac, ch, force);
  } else {
    port_mgr_mac_set_force_idle(dev_id, dev_port, force);
  }
  return BF_SUCCESS;
}

/** \brief Enable/Disable tx-drain on a Tofino port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param en   : true=enable tx-drain, false=disable tx-drain
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_tx_drain_set(bf_dev_id_t dev_id,
                                 bf_dev_port_t dev_port,
                                 bool en) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof2(dev_id)) {
    uint32_t rc, umac, ch;

    rc = port_mgr_map_dev_port_to_all(
        dev_id, dev_port, NULL, NULL, (int *)&umac, (int *)&ch, NULL);
    if (rc != 0) return BF_INVALID_ARG;

    port_mgr_tof2_umac_tx_drain_set(dev_id, umac, ch, en);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    uint32_t rc, tmac, ch;
    rc = port_mgr_map_dev_port_to_all(
        dev_id, dev_port, NULL, NULL, (int *)&tmac, (int *)&ch, NULL);
    if (rc != 0) return BF_INVALID_ARG;
    port_mgr_tof3_tmac_tx_drain_set(dev_id, tmac, ch, en);
  } else {
    port_mgr_mac_set_drain(dev_id, dev_port, en);
  }
  return BF_SUCCESS;
}

/** \brief Tell if the specified port is in RX_ONLY mode
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 *
 * \return: true if port is configured with RX_ONLY mode.
 * \return: false otherwise.
 *
 */
bf_status_t bf_port_is_rx_only(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return false;

  if (port_p->sw.port_dir == BF_PORT_DIR_RX_ONLY) return true;

  return false;
}

/** \brief Tell if the specified port is in DECOUPLED mode
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 *
 * \return: true if port is configured with DECOUPLED mode.
 * \return: false otherwise.
 *
 */
bf_status_t bf_port_is_decoupled_mode(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return false;

  if (port_p->sw.port_dir == BF_PORT_DIR_DECOUPLED) return true;

  return false;
}

/** \brief Initiate a DMA of MAC stats from hardware
 *
 * [ POST_ENABLE ]
 *
 * Initiates a DMA read of the MAC stats. If specified, the user_cb
 * function will be called upon receipt of the DMA completion. The
 * default action taken is to update the cached counters. Once this is done,
 * if the user specified a callback, the callback is issued.
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port encoded port identifier
 * \param user_cb  : May be NULL
 * \param user_data: cookie returned in callback
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG : dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG : invalid or missing bf_dev_port_t
 * \return: BF_NOT_READY   : MAC stat request outstanding (try again later)
 * \return: BF_HW_COMM_FAIL: DMA memory not allocated correctly
 */
bf_status_t bf_port_mac_stats_hw_async_get(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           bf_port_mac_stat_callback_t user_cb,
                                           void *user_data) {
  bf_status_t rc;
  int mac_block, ch;
  port_mgr_dev_t *dev_p = port_mgr_map_dev_id_to_dev_p_allow_unassigned(dev_id);
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  bf_dma_addr_t dma_addr;

  if (dev_p == NULL) return BF_INVALID_ARG;
  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof2(dev_id)) {
    // FIXME: add sppt
    // return BF_INVALID_ARG;
  }

  rc = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &mac_block, &ch, NULL);
  if (rc != 0) {
    return rc;
  }
  /* Map the virtual address of the buffer to the DMA address before
     it is pushed into the DR */
  if (bf_sys_dma_map(port_p->mac_stat_hndl,
                     port_p->mac_stat_dma_vaddr,
                     port_p->mac_stat_dma_paddr,
                     port_p->mac_stat_buf_size,
                     &dma_addr,
                     BF_DMA_TO_CPU) != 0) {
    return BF_HW_COMM_FAIL;
  }
  rc = lld_push_mac_stats_read(
      dev_id,
      mac_block,
      ch,
      dma_addr,
      port_mgr_mac_stat_dma_msg_id_encode(dev_id, dev_port));
  if (rc == LLD_ERR_DR_FULL) {
    lld_dr_start(dev_id, 0, lld_dr_tx_mac_stat);
    lld_dr_service(dev_id, 0, lld_dr_tx_mac_stat, 1);
    rc = lld_push_mac_stats_read(
        dev_id,
        mac_block,
        ch,
        dma_addr,
        port_mgr_mac_stat_dma_msg_id_encode(dev_id, dev_port));  // try again
  }
  if (rc != BF_SUCCESS) {
    /* Unmap the buffer */
    bf_sys_dma_unmap(port_p->mac_stat_hndl,
                     port_p->mac_stat_dma_vaddr,
                     port_p->mac_stat_buf_size,
                     BF_DMA_TO_CPU);
    // reset callback info
    port_p->mac_stat_user_cb = NULL;
    port_p->mac_stat_user_data = NULL;
    return BF_INVALID_ARG;
  }
  // save callback info
  port_p->mac_stat_user_cb = user_cb;
  port_p->mac_stat_user_data = user_data;

  // finally, initiate the DMA
  rc = lld_dr_start(dev_id, 0, lld_dr_tx_mac_stat);
  return rc;
}

/** \brief Return MAC historical stats
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param ctr_data: caller supplied buffer
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG : dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG : invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_mac_stats_historical_get(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    bf_rmon_counter_array_t *ctr_data) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (ctr_data == NULL) return BF_INVALID_ARG;

  // disallow simultaneous access as cache is being updated
  bf_port_mac_stats_mtx_lock(dev_id, dev_port);

  // copy historical counters (only) to user area
  memcpy((char *)ctr_data->format.ctr_array,
         (char *)port_p->mac_stat_historical.format.ctr_array,
         sizeof(ctr_data->format.ctr_array));

  // cache updated now, safe for other accesses
  bf_port_mac_stats_mtx_unlock(dev_id, dev_port);

  return BF_SUCCESS;
}

/** \brief Set MAC historical stats (used to restore ctrs after HA event)
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param ctr_data: caller supplied buffer
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG : dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG : invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_mac_stats_historical_set(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    bf_rmon_counter_array_t *ctr_data) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (ctr_data == NULL) return BF_INVALID_ARG;

  // disallow simultaneous access as cache is being updated
  bf_port_mac_stats_mtx_lock(dev_id, dev_port);

  // copy to historical counters
  memcpy((char *)port_p->mac_stat_historical.format.ctr_array,
         (char *)ctr_data->format.ctr_array,
         sizeof(ctr_data->format.ctr_array));

  // cache updated now, safe for other accesses
  bf_port_mac_stats_mtx_unlock(dev_id, dev_port);

  return BF_SUCCESS;
}

/** \brief Update historical counters in anticipation of HW stats being cleared
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG : dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG : invalid or missing bf_dev_port_t
 * \return: BF_NOT_READY   : MAC stat request outstanding (try again later)
 * \return: BF_HW_COMM_FAIL: DMA memory not allocated correctly
 *
 */
bf_status_t bf_port_mac_stats_historical_update_set(bf_dev_id_t dev_id,
                                                    bf_dev_port_t dev_port) {
  int ctr_rd_failed;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  // disallow simultaneous access as cache is being updated
  bf_port_mac_stats_mtx_lock(dev_id, dev_port);

  // read counters into mac stat cache
  ctr_rd_failed = port_mgr_port_read_all_counters(dev_id, dev_port);

  if (ctr_rd_failed) {
    bf_port_mac_stats_mtx_unlock(dev_id, dev_port);
    return BF_HW_COMM_FAIL;
  }
  port_mgr_mac_stats_copy(&port_p->mac_stat_cache,
                          &port_p->mac_stat_historical,
                          &port_p->mac_stat_historical);

  // now clear cached counters (last HW stats read or DMA'd) so they cant get
  // added again
  port_mgr_mac_stats_clear_sw_ctrs(&port_p->mac_stat_cache);

  // cache updated now, safe for other accesses
  bf_port_mac_stats_mtx_unlock(dev_id, dev_port);

  return BF_SUCCESS;
}

/** \brief Read MAC stats from hardware using the register interface
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param ctr_data: caller supplied buffer
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG : dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG : invalid or missing bf_dev_port_t
 * \return: BF_NOT_READY   : MAC stat request outstanding (try again later)
 * \return: BF_HW_COMM_FAIL: DMA memory not allocated correctly
 *
 */
bf_status_t bf_port_mac_stats_hw_sync_get(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port,
                                          bf_rmon_counter_array_t *ctr_data) {
  int ctr_rd_failed;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (ctr_data == NULL) return BF_INVALID_ARG;

  // disallow simultaneous access as cache is being updated
  bf_port_mac_stats_mtx_lock(dev_id, dev_port);

  // read counters into mac stat cache
  ctr_rd_failed = port_mgr_port_read_all_counters(dev_id, dev_port);

  if (ctr_rd_failed) {
    bf_port_mac_stats_mtx_unlock(dev_id, dev_port);
    return BF_HW_COMM_FAIL;
  }

  // copy cached counters to user area
  port_mgr_mac_stats_copy(
      &port_p->mac_stat_cache, &port_p->mac_stat_historical, ctr_data);

  // cache updated now, safe for other accesses
  bf_port_mac_stats_mtx_unlock(dev_id, dev_port);

  return BF_SUCCESS;
}

/** \brief Read MAC stats from hardware and dont add historical ctrs
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param ctr_data: caller supplied buffer
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG : dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG : invalid or missing bf_dev_port_t
 * \return: BF_NOT_READY   : MAC stat request outstanding (try again later)
 * \return: BF_HW_COMM_FAIL: DMA memory not allocated correctly
 *
 */
bf_status_t bf_port_mac_stats_hw_only_sync_get(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    bf_rmon_counter_array_t *ctr_data) {
  int ctr_rd_failed;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (ctr_data == NULL) return BF_INVALID_ARG;

  // disallow simultaneous access as cache is being updated
  bf_port_mac_stats_mtx_lock(dev_id, dev_port);

  // read counters into mac stat cache
  ctr_rd_failed = port_mgr_port_read_all_counters(dev_id, dev_port);

  if (ctr_rd_failed) {
    bf_port_mac_stats_mtx_unlock(dev_id, dev_port);
    return BF_HW_COMM_FAIL;
  }

  // copy cached counters to user area. Do NOT add historical data
  memcpy((char *)ctr_data->format.ctr_array,
         (char *)port_p->mac_stat_cache.format.ctr_array,
         sizeof(ctr_data->format.ctr_array));

  // cache updated now, safe for other accesses
  bf_port_mac_stats_mtx_unlock(dev_id, dev_port);

  return BF_SUCCESS;
}

/** \brief Read MAC stats from cached since the last sync or async update
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param ctr_data: caller supplied buffer
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_NOT_READY   : MAC stat request outstanding (try again later)
 *
 */
bf_status_t bf_port_mac_stats_cached_get(bf_dev_id_t dev_id,
                                         bf_dev_port_t dev_port,
                                         bf_rmon_counter_array_t *ctr_data) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (ctr_data == NULL) return BF_INVALID_ARG;

  // disallow simultaneous access as cache is being updated
  bf_sys_mutex_lock(&port_p->port_mtx);

  // copy cached counters to user_ctrs
  port_mgr_mac_stats_copy(
      &port_p->mac_stat_cache, &port_p->mac_stat_historical, ctr_data);

  // cache copied now, safe for other accesses
  bf_sys_mutex_unlock(&port_p->port_mtx);

  return BF_SUCCESS;
}

/** \brief Read specific MAC stat counter from the cache.
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param ctr_id  : counter-id to read
 * \param ctr_val : ptr to counter value (returned)
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_NOT_READY   : MAC stat request outstanding (try again later)
 *
 */
bf_status_t bf_port_mac_stats_ctr_cached_get(bf_dev_id_t dev_id,
                                             bf_dev_port_t dev_port,
                                             bf_rmon_counter_t ctr_id,
                                             uint64_t *ctr_data) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (ctr_id >= BF_NUM_RMON_COUNTERS) return BF_INVALID_ARG;
  if (ctr_data == NULL) return BF_INVALID_ARG;

  // disallow simultaneous access as cache is being updated
  bf_sys_mutex_lock(&port_p->port_mtx);

  *ctr_data = port_p->mac_stat_cache.format.ctr_array[ctr_id] +
              port_p->mac_stat_historical.format.ctr_array[ctr_id];
  ;

  // cache copied now, safe for other accesses
  bf_sys_mutex_unlock(&port_p->port_mtx);
  return BF_SUCCESS;
}

/** \brief Clear MAC stats for the given port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_NOT_READY   : MAC stat request outstanding (try again later)
 *
 */
bf_status_t bf_port_mac_stats_clear(bf_dev_id_t dev_id,
                                    bf_dev_port_t dev_port) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  // disallow simultaneous access as cache is being updated
  bf_sys_mutex_lock(&port_p->port_mtx);

  // clear cached counters (last HW stats read or DMA'd)
  port_mgr_mac_stats_clear_sw_ctrs(&port_p->mac_stat_cache);

  // clear historical values
  port_mgr_mac_stats_clear_sw_ctrs(&port_p->mac_stat_historical);

  // clear HW counters
  if (port_mgr_dev_is_tof3(dev_id)) {
    port_mgr_tof3_port_mac_stats_clear(dev_id, dev_port);
  } else {
    // clear HW counters
    port_mgr_mac_stats_clear(dev_id, dev_port);
  }

  // cache copied now, safe for other accesses
  bf_sys_mutex_unlock(&port_p->port_mtx);
  return BF_SUCCESS;
}

/**************************************************************************
 * bf_rmon_counter_str
 *
 * Array of strings corresponding to the enumerated type bf_rmon_counter_t
 *
 * Both the string array and the enumerated type are generated from a
 * metadata file, bf_rmon.h, containing macro definitions for each counter.
 *
 * The macro is re-defined here to generate a character string.
 **************************************************************************/
#undef MAC_RMON_CTR
#define MAC_RMON_CTR(idx, id) #id,

static char *bf_rmon_counter_str[] = {
#include <port_mgr/bf_rmon.h>
};

#undef MAC_RMON_CTR

/** \brief Return string corresponding to an RMON counter enum value
 *
 * [ POST_ENABLE ]
 *
 * \param ctr enum value
 * \param str : corresponding ascii string
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: ctr < 0 or ctr >= BF_NUM_RMON_COUNTERS
 * \return: BF_INVALID_ARG: str == NULL
 *
 */
bf_status_t bf_port_rmon_counter_to_str(bf_rmon_counter_t ctr, char **str) {
  if (str == NULL) return BF_INVALID_ARG;
  if (((int)ctr < 0) || (ctr >= BF_NUM_RMON_COUNTERS)) {
    *str = "enum_out_of_range";
    return BF_INVALID_ARG;
  }
  *str = bf_rmon_counter_str[ctr];
  return BF_SUCCESS;
}

/** \brief Get PCS status
 *         NULL pointer values may be used to skip collection
 *         of individual counters/status values.
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id        : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port      : encoded port identifier
 * \param pcs_status    : 1=PCS fully operational
 * \param block_lock_per_pcs_lane : 1 bit per pcs lane (0-19)
 * \param alignment_marker_lock_per_pcs_lane: 1 bit per pcs lane (0-19)
 * \param hi_ber        : 1=high bit error rate detected
 * \param block_lock_all: 1=block lock on all lanes
 * \param alignment_marker_lock_all : 1=alignment marker lock on all lanes
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_pcs_status_get(bf_dev_id_t dev_id,
                                   bf_dev_port_t dev_port,
                                   bool *pcs_status,
                                   uint32_t *block_lock_per_pcs_lane,
                                   uint32_t *alignment_marker_lock_per_pcs_lane,
                                   bool *hi_ber,
                                   bool *block_lock_all,
                                   bool *alignment_marker_lock_all) {
  int mac_block, ch;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &mac_block, &ch, NULL);

  if (port_mgr_dev_is_tof1(dev_id)) {
    port_mgr_mac_get_pcs_status(dev_id,
                                mac_block,
                                ch,
                                pcs_status,
                                block_lock_per_pcs_lane,
                                alignment_marker_lock_per_pcs_lane,
                                hi_ber,
                                block_lock_all,
                                alignment_marker_lock_all);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    port_mgr_tof2_umac_get_pcs_status(dev_id,
                                      mac_block,
                                      ch,
                                      pcs_status,
                                      block_lock_per_pcs_lane,
                                      alignment_marker_lock_per_pcs_lane,
                                      hi_ber,
                                      block_lock_all,
                                      alignment_marker_lock_all);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    bf_tof3_pcs_status_t pcs;
    port_mgr_tof3_tmac_pcs_status_get(dev_id, mac_block, ch, &pcs);
    *pcs_status = pcs.up;
    *alignment_marker_lock_all = pcs.am_lock_all;
    *hi_ber = pcs.hi_ber;
    *block_lock_all = pcs.block_lock_all;
    // block lock per lane and alignment per lane not supported
    *block_lock_per_pcs_lane = 0;
    *alignment_marker_lock_per_pcs_lane = 0;
  } else {
    return BF_INVALID_ARG;
  }
  return BF_SUCCESS;
}

bf_status_t bf_port_pcs_status_get_v2(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      bool *pcs_status,
                                      bool *hi_ber,
                                      bool *block_lock_all) {
  int mac_block, ch;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof2(dev_id)) {
    // FIXME: add sppt
    return BF_INVALID_ARG;
  }

  port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &mac_block, &ch, NULL);

  port_mgr_mac_get_pcs_status_v2(
      dev_id, mac_block, ch, pcs_status, hi_ber, block_lock_all);

  return BF_SUCCESS;
}

/** \brief Get PCS counters
 *         NULL pointer values may be used to skip collection
 *         of individual counters/status values.
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id           : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port         : encoded port identifier
 * \param ber_cnt          : count of bad sync headers
 * \param errored_blk_cnt  : count of errored blocks
 * \param sync_loss        : count of sync loss
 * \param block_lock_loss  : count of block-lock loss
 * \param hi_ber_cnt       : count of hi_ber events
 * \param valid_error_cnt  : count of valid error events
 * \param unknown_error_cnt: count of unknown error events
 * \param invalid_error_cnt: count of invalid error events
 * \param bip_errors_per_pcs_lane: Bit Inteleaved Parity errors (per PCS lane)
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_pcs_counters_get(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port,
                                     uint32_t *ber_cnt,
                                     uint32_t *errored_blk_cnt,
                                     uint32_t *sync_loss,
                                     uint32_t *block_lock_loss,
                                     uint32_t *hi_ber_cnt,
                                     uint32_t *valid_error_cnt,
                                     uint32_t *unknown_error_cnt,
                                     uint32_t *invalid_error_cnt,
                                     uint32_t *bip_errors_per_pcs_lane) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof2(dev_id)) {
    umac4_pcs_ctr_t ctrs;
    uint32_t umac, ch;

    port_mgr_map_dev_port_to_all(
        dev_id, dev_port, NULL, NULL, (int *)&umac, (int *)&ch, NULL);
    umac4_ctrs_pcs_get(dev_id, umac, ch, &ctrs);
    if (ber_cnt) *ber_cnt = (uint32_t)ctrs.HSMC_PCS_BER_Error_Counter;
    if (errored_blk_cnt)
      *errored_blk_cnt = (uint32_t)ctrs.HSMC_PCS_Errored_Blocks_Counter;
    if (sync_loss) *sync_loss = (uint32_t)ctrs.LSMC_PCS_Sync_Loss_Counter;
    if (block_lock_loss)
      *block_lock_loss = (uint32_t)ctrs.HSMC_PCS_Block_Lock_Loss_Counter;
    if (hi_ber_cnt) *hi_ber_cnt = (uint32_t)ctrs.HSMC_PCS_HiBER_Counter;
    if (valid_error_cnt)
      *valid_error_cnt = (uint32_t)ctrs.HSMC_PCS_Valid_Errored_Block_Counter_;
    if (unknown_error_cnt)
      *unknown_error_cnt =
          (uint32_t)ctrs.HSMC_PCS_Unknown_Errored_Block_Counter;
    if (invalid_error_cnt)
      *invalid_error_cnt = (uint32_t)ctrs.LSMC_PCS_Invalid_Code_Counter;
    //
    if (bip_errors_per_pcs_lane) {
      umac4_pcs_vl_ctr_t vl;
      umac4_ctrs_pcs_vl_get(dev_id, umac, &vl);
      bip_errors_per_pcs_lane[0] =
          (uint32_t)vl.HSMC_PCS_Lane_Chx_BIP_0_Errors_Counter;
      bip_errors_per_pcs_lane[1] =
          (uint32_t)vl.HSMC_PCS_Lane_Chx_BIP_1_Errors_Counter;
      bip_errors_per_pcs_lane[2] =
          (uint32_t)vl.HSMC_PCS_Lane_Chx_BIP_2_Errors_Counter;
      bip_errors_per_pcs_lane[3] =
          (uint32_t)vl.HSMC_PCS_Lane_Chx_BIP_3_Errors_Counter;
      bip_errors_per_pcs_lane[4] =
          (uint32_t)vl.HSMC_PCS_Lane_Chx_BIP_4_Errors_Counter;
      bip_errors_per_pcs_lane[5] =
          (uint32_t)vl.HSMC_PCS_Lane_Chx_BIP_5_Errors_Counter;
      bip_errors_per_pcs_lane[6] =
          (uint32_t)vl.HSMC_PCS_Lane_Chx_BIP_6_Errors_Counter;
      bip_errors_per_pcs_lane[7] =
          (uint32_t)vl.HSMC_PCS_Lane_Chx_BIP_7_Errors_Counter;
      bip_errors_per_pcs_lane[8] =
          (uint32_t)vl.HSMC_PCS_Lane_Chx_BIP_8_Errors_Counter;
      bip_errors_per_pcs_lane[9] =
          (uint32_t)vl.HSMC_PCS_Lane_Chx_BIP_9_Errors_Counter;
      bip_errors_per_pcs_lane[10] =
          (uint32_t)vl.HSMC_PCS_Lane_Chx_BIP_10_Errors_Counter;
      bip_errors_per_pcs_lane[11] =
          (uint32_t)vl.HSMC_PCS_Lane_Chx_BIP_11_Errors_Counter;
      bip_errors_per_pcs_lane[12] =
          (uint32_t)vl.HSMC_PCS_Lane_Chx_BIP_12_Errors_Counter;
      bip_errors_per_pcs_lane[13] =
          (uint32_t)vl.HSMC_PCS_Lane_Chx_BIP_13_Errors_Counter;
      bip_errors_per_pcs_lane[14] =
          (uint32_t)vl.HSMC_PCS_Lane_Chx_BIP_14_Errors_Counter;
      bip_errors_per_pcs_lane[15] =
          (uint32_t)vl.HSMC_PCS_Lane_Chx_BIP_15_Errors_Counter;
      bip_errors_per_pcs_lane[16] =
          (uint32_t)vl.HSMC_PCS_Lane_Chx_BIP_16_Errors_Counter;
      bip_errors_per_pcs_lane[17] =
          (uint32_t)vl.HSMC_PCS_Lane_Chx_BIP_17_Errors_Counter;
      bip_errors_per_pcs_lane[18] =
          (uint32_t)vl.HSMC_PCS_Lane_Chx_BIP_18_Errors_Counter;
      bip_errors_per_pcs_lane[19] =
          (uint32_t)vl.HSMC_PCS_Lane_Chx_BIP_19_Errors_Counter;
    }
    return BF_SUCCESS;
  }

  port_mgr_mac_get_pcs_counters(dev_id,
                                dev_port,
                                ber_cnt,
                                errored_blk_cnt,
                                sync_loss,
                                block_lock_loss,
                                hi_ber_cnt,
                                valid_error_cnt,
                                unknown_error_cnt,
                                invalid_error_cnt,
                                bip_errors_per_pcs_lane);
  return BF_SUCCESS;
}

/** \brief Get PCS cumulative counters
 *         NULL pointer values may be used to skip collection
 *         of individual counters/status values.
 *
 * \param dev_id           : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port         : encoded port identifier
 * \param ber_cnt          : count of bad sync headers
 * \param errored_blk_cnt  : count of errored blocks
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_pcs_cumulative_counters_get(bf_dev_id_t dev_id,
                                                bf_dev_port_t dev_port,
                                                uint32_t *ber_cnt,
                                                uint32_t *errored_blk_cnt) {
  if (port_mgr_dev_is_tof2(dev_id)) {
    port_mgr_log("Error : Get cumulative pcs_counters not supported for Tof2");
    return BF_INVALID_ARG;
  }

  port_mgr_mac_get_pcs_cumulative_counters(
      dev_id, dev_port, ber_cnt, errored_blk_cnt);
  return BF_SUCCESS;
}

/** \brief Clear PCS cumulative counters.
 *
 * \param dev_id           : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port         : encoded port identifier
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_pcs_cumulative_counters_clr(bf_dev_id_t dev_id,
                                                bf_dev_port_t dev_port) {
  if (port_mgr_dev_is_tof2(dev_id)) {
    bf_status_t st;
    port_mgr_port_t *port_p;
    uint32_t dummy_errored_blk_cnt;
    uint32_t dummy_ber_cnt;

    port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
    if (!port_p) return BF_INVALID_ARG;

    if (port_p->sw.oper_state) {
      st = bf_port_pcs_counters_get(dev_id,
                                    dev_port,
                                    &dummy_ber_cnt,
                                    &dummy_errored_blk_cnt,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL);
      return st;
    }
    return BF_SUCCESS;
  }

  port_mgr_mac_clr_pcs_cumulative_counters(dev_id, dev_port);

  return BF_SUCCESS;
}

/** \brief Start (or restart) autonegotiation on a Tofino port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_autoneg_restart_set(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        bool disable_nonce_match,
                                        bool disable_link_inhibit_timer) {
  int rc;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  if (!port_mgr_dev_is_tof1(dev_id)) {
    // FIXME: add sppt
    return BF_INVALID_ARG;
  }

  rc = bf_serdes_autoneg_start(dev_id,
                               dev_port,
                               port_p->an_base_page,
                               port_p->an_num_next_pages,
                               disable_nonce_match,
                               disable_link_inhibit_timer);
  if (rc != BF_SUCCESS) {
    port_mgr_log("%d : %x : AN set error=%xh\n", dev_id, dev_port, rc);
    return BF_INVALID_ARG;
  }
  return BF_SUCCESS;
}

/** \brief Config port to perform autonegotiation on a Tofino 2 port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: Not supported on device
 *
 */
bf_status_t bf_port_autoneg_config_set(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port) {
  bf_status_t bf_status = BF_SUCCESS;
  port_mgr_port_t *port_p = NULL;
  bool is_loop = true;
  int rc;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof2(dev_id)) {
    port_mgr_log(
        "FSM :%d:%3d:0: AN : bp=%04x_%04x_%04x : consort=%04x_%04x_%04X",
        dev_id,
        dev_port,
        (uint32_t)((port_p->an_base_page >> 32ull) & 0xffff),
        (uint32_t)((port_p->an_base_page >> 16ull) & 0xffff),
        (uint32_t)((port_p->an_base_page >> 0ull) & 0xffff),
        (uint32_t)((port_p->an_next_page[1] >> 32ull) & 0xffff),
        (uint32_t)((port_p->an_next_page[1] >> 16ull) & 0xffff),
        (uint32_t)((port_p->an_next_page[1] >> 0ull) & 0xffff));

    rc = bf_tof2_serdes_config_ln_autoneg(
        dev_id,
        dev_port,
        0,
        port_p->an_base_page,
        (port_p->an_next_page[1] >> 16ull) & 0xffffffff,
        is_loop);
    if (rc) {
      port_mgr_log(
          "FSM :%d:%3d:0: AN : ERROR configuring serdes", dev_id, dev_port);
      bf_status = BF_INVALID_ARG;
    }
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    port_mgr_log(
        "FSM :%d:%3d:0: AN : bp=%04x_%04x_%04x : consort=%04x_%04x_%04X",
        dev_id,
        dev_port,
        (uint32_t)((port_p->an_base_page >> 32ull) & 0xffff),
        (uint32_t)((port_p->an_base_page >> 16ull) & 0xffff),
        (uint32_t)((port_p->an_base_page >> 0ull) & 0xffff),
        (uint32_t)((port_p->an_next_page[1] >> 32ull) & 0xffff),
        (uint32_t)((port_p->an_next_page[1] >> 16ull) & 0xffff),
        (uint32_t)((port_p->an_next_page[1] >> 0ull) & 0xffff));
  } else {
    // Not supported by Tof 1 and Tof 3 yet
    bf_status = BF_INVALID_ARG;
  }

  return bf_status;
}

/** \brief Control AN FSM logging
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param an_log_ena: enable/disable autoneg extended logging
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_autoneg_ext_log_ena(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        bool an_log_ena) {
  port_mgr_port_t *port_p = NULL;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  port_p->an_log_ena = an_log_ena;

  return BF_SUCCESS;
}

/** \brief Return the state of autonegotiation on a Tofino port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param an_st   : autonegotiation state
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: an_st (return value pointer) NULL
 *
 */
bf_status_t bf_port_autoneg_state_get(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      bf_an_state_e *an_st) {
  bf_status_t bf_status = BF_SUCCESS;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (an_st == NULL) return BF_INVALID_ARG;

  *an_st = BF_AN_ST_NONE;

  if (!port_mgr_dev_is_tof1(dev_id)) {
    // FIXME: add sppt
    return BF_INVALID_ARG;
  }

  bf_status = bf_serdes_autoneg_st_get(dev_id, dev_port, an_st);

  port_mgr_log("bf_serdes_autoneg_st_get: an_st=%d\n", *an_st);
  return bf_status;
}

/** \brief Resolve and return negotiated HCD speed, lanes and fec mode
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param tx_base_page: Transmitted base page
 * \param rx_base_page: Received base page
 * \param hcd_speed   : Return speed of negotiated mode
 * \param hcd_lanes   : Return number of lanes of negotiated mode
 * \param fec         : FEC for negotiated mode
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: invalid pointer
 *
 */
bf_status_t bf_port_autoneg_hcd_fec_resolve(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port,
                                            uint64_t tx_base_page,
                                            uint64_t rx_base_page,
                                            bf_port_speed_t *hcd_speed,
                                            int *hcd_lanes,
                                            bf_fec_type_t *fec) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  static int an_73_priority[] = {0, 3, 4, 7,  8,  10, 11, 12, 13, 5, 6,
                                 1, 2, 9, 14, 15, 0,  0,  0,  0,  0, 0};
  char tx_advtech_str[24] = {0};
  char rx_advtech_str[24] = {0};
  bool mode_25g_s = false;
  uint32_t tx_advtech;
  uint32_t rx_advtech;
  int cur_priority;
  uint32_t tx_fec;
  uint32_t rx_fec;
  int tech_index;
  int i;

  if (!hcd_speed || !hcd_lanes || !fec) return BF_INVALID_ARG;
  if (port_p == NULL) return BF_INVALID_ARG;

  *hcd_speed = BF_SPEED_NONE;
  *hcd_lanes = 0;
  *fec = BF_FEC_TYP_NONE;

  if (!tx_base_page || !rx_base_page) return BF_SUCCESS;

  tx_advtech = (tx_base_page >> 21ull) & 0x3FFFFFull;
  tx_fec = (tx_base_page >> 44ull) & 0x000Full;
  rx_advtech = (rx_base_page >> 21ull) & 0x3FFFFFull;
  rx_fec = (rx_base_page >> 44ull) & 0x000Full;

  tech_index = -1;
  cur_priority = -1;
  for (i = 0; i <= 22; i++) {
    tx_advtech_str[i] = (tx_advtech & (1ull << i)) ? '1' : '0';
    rx_advtech_str[i] = (rx_advtech & (1ull << i)) ? '1' : '0';
    /* tx_advtech and rx_advtech can have bit set between 0 to 21 so below
       condition won't be true for i greater than 21. Hence an_73_priority[i]
       does not exceed boundry */
    if ((tx_advtech & rx_advtech) & (1ull << i)) {
      if (an_73_priority[i] > cur_priority) {
        tech_index = i;
        cur_priority = an_73_priority[i];
      }
    }
  }
  port_p->hcd_ndx = tech_index;

  if (tech_index >= 0) {
    // map tech_index to supported speed modes
    switch (tech_index) {
      case 0:
        *hcd_speed = BF_SPEED_1G;
        *hcd_lanes = 1;
        break;
      case 2:
        *hcd_speed = BF_SPEED_10G;
        *hcd_lanes = 1;
        break;
      case 3:
      case 4:
        *hcd_speed = BF_SPEED_40G;
        *hcd_lanes = 4;
        break;
      case 7:
      case 8:
        *hcd_speed = BF_SPEED_100G;
        *hcd_lanes = 4;
        break;
      case 9:
        mode_25g_s = true;
        *hcd_speed = BF_SPEED_25G;
        *hcd_lanes = 1;
        break;
      case 10:
        *hcd_speed = BF_SPEED_25G;
        *hcd_lanes = 1;
        break;
      case 13:
        *hcd_speed = BF_SPEED_50G;
        *hcd_lanes = 1;
        break;
      case 14:
        *hcd_speed = BF_SPEED_100G;
        *hcd_lanes = 2;
        break;
      case 15:
        *hcd_speed = BF_SPEED_200G;
        *hcd_lanes = 4;
        break;
    }
  }

  if (port_p->an_log_ena) {
    port_mgr_log("%d:%3.3d:0: AN Tech Field Tx: A[0] %s A[22] (hcd_index=%d)",
                 dev_id,
                 dev_port,
                 tx_advtech_str,
                 tech_index);
    port_mgr_log("%d:%3.3d:0: AN Tech Field Rc: A[0] %s A[22] (hcd_index=%d)",
                 dev_id,
                 dev_port,
                 rx_advtech_str,
                 tech_index);
  }

  /* Determine negotiated FEC (only applies to 10GBase-KR and 25GBASE-R modes)
   * FEC is negotiated using 4 bits:
   * FO (D46): 10Gb/s per lane FEC ability
   * F1 (D47): 10Gb/s per lane FEC requested
   * F2 (D44): 25G RS-FEC requested
   * F3 (D45): 25G BASE-R FEC requested
   */

  if (*hcd_speed == BF_SPEED_10G || *hcd_speed == BF_SPEED_40G) {
    if (((rx_fec & tx_fec) & 0x04) && ((rx_fec | tx_fec) & 0x08)) {
      /* select Fire Code FEC if:
       *  both PHYs set F0 bit (F0 -> D46 in basepage)
       *  .AND.
       * either PHY set F1 bit (F1 -> D47 in basepage)
       */
      *fec = BF_FEC_TYP_FIRECODE;
    }
  } else if (*hcd_speed == BF_SPEED_25G) {
    if (((rx_fec & tx_fec) & 0x01) ||
        (((rx_fec | tx_fec) & 0x01) && !mode_25g_s)) {
      /* select Reed Solomon FEC if any of these:
       *  both PHYs set F2 bit (F2 -> D44 in basepage)
       *  .OR.
       * either PHY set F2 and HCD is not 25GBase-CR/KR-S mode
       */
      *fec = BF_FEC_TYP_REED_SOLOMON;
    } else if (((rx_fec & tx_fec) & 0x02) ||
               (((rx_fec | tx_fec) & 0x02) && !mode_25g_s)) {
      /* select Fire Code FEC if any of these:
       *  both PHYs set F3 bit (F3 -> D45 in basepage)
       *  .OR.
       * either PHY set F2 and HCD is not 25GBase-CR/KR-S mode
       */
      *fec = BF_FEC_TYP_FIRECODE;
    }
  }
  if (port_p->an_log_ena) {
    port_mgr_log("%d:%3.3d:0: AN FEC Tx F0=%d F1=%d F2=%d F3=%d",
                 dev_id,
                 dev_port,
                 (tx_fec & 0x4) ? 1 : 0,
                 (tx_fec & 0x8) ? 1 : 0,
                 (tx_fec & 0x1) ? 1 : 0,
                 (tx_fec & 0x2) ? 1 : 0);
    port_mgr_log("%d:%3.3d:0: AN FEC Rx F0=%d F1=%d F2=%d F3=%d",
                 dev_id,
                 dev_port,
                 (rx_fec & 0x4) ? 1 : 0,
                 (rx_fec & 0x8) ? 1 : 0,
                 (rx_fec & 0x1) ? 1 : 0,
                 (rx_fec & 0x2) ? 1 : 0);
  }

  return BF_SUCCESS;
}

/** \brief Resolve negotiated HCD speed lanes and fec mode using next pages
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id      : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port    : encoded port identifier
 * \param tx_base_page: Transmitted base page (just used for FEC resolution)
 * \param rx_base_page: Received base page (just used for FEC resolution)
 * \param tx_nxt_pg2  : Transmitted next page #2
 * \param rx_nxt_pg2  : Received next page #2
 * \param hcd_speed   : Return speed of negotiated mode
 * \param hcd_lanes   : Return number of lanes of negotiated mode
 * \param fec         : FEC for negotiated mode
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: invalid pointer
 *
 */
bf_status_t bf_port_autoneg_hcd_fec_resolve_nxt_pg(bf_dev_id_t dev_id,
                                                   bf_dev_port_t dev_port,
                                                   uint64_t tx_base_page,
                                                   uint64_t rx_base_page,
                                                   uint64_t tx_nxt_pg2,
                                                   uint64_t rx_nxt_pg2,
                                                   bf_port_speed_t *hcd_speed,
                                                   int *hcd_lanes,
                                                   bf_fec_type_t *fec) {
  bool ld_f1, ld_f2, ld_f3, ld_f4;
  bool rd_f1, rd_f2, rd_f3, rd_f4;
  uint32_t tx_bp_fec;
  uint32_t rx_bp_fec;

  if (!hcd_speed || !hcd_lanes || !fec) return BF_INVALID_ARG;

  *hcd_speed = BF_SPEED_NONE;
  *hcd_lanes = 0;
  *fec = BF_FEC_TYP_NONE;

  if (!tx_nxt_pg2 || !rx_nxt_pg2) return BF_SUCCESS;

  if ((tx_nxt_pg2 & rx_nxt_pg2) & 0x400000000) {
    *hcd_speed = BF_SPEED_400G;
    *hcd_lanes = 8;
  } else if ((tx_nxt_pg2 & rx_nxt_pg2) & 0x3000000) {
    *hcd_speed = BF_SPEED_50G_CONS;
    *hcd_lanes = 2;
  } else if ((tx_nxt_pg2 & rx_nxt_pg2) & 0x300000) {
    *hcd_speed = BF_SPEED_25G;
    *hcd_lanes = 1;
  }

  // Fec resolution
  if (*hcd_speed != BF_SPEED_NONE) {
    // f1: advertises Clause 91 FEC ability
    // f2: advertises Clause 74 FEC ability
    // f3: request Clause 91 FEC
    // f3: request Clause 74 FEC
    // ld: local device
    // rd: remote device
    ld_f1 = tx_nxt_pg2 & 0x10000000000;
    ld_f2 = tx_nxt_pg2 & 0x20000000000;
    ld_f3 = tx_nxt_pg2 & 0x40000000000;
    ld_f4 = tx_nxt_pg2 & 0x80000000000;

    rd_f1 = rx_nxt_pg2 & 0x10000000000;
    rd_f2 = rx_nxt_pg2 & 0x20000000000;
    rd_f3 = rx_nxt_pg2 & 0x40000000000;
    rd_f4 = rx_nxt_pg2 & 0x80000000000;

    tx_bp_fec = (tx_base_page >> 46ull) & 0x0003ull;
    rx_bp_fec = (rx_base_page >> 46ull) & 0x0003ull;

    // See FEC resolution algorithm in section 3.2.5.2 "FEC Control" of the
    // Consortium 25G/50G specification
    if ((ld_f3 || rd_f3) && ld_f1 && rd_f1) {
      // if either link partner requests Clause 91 FEC and BOTH support it:
      *fec = BF_FEC_TYP_REED_SOLOMON;
    } else if ((ld_f4 || rd_f4) && ld_f2 && rd_f2) {
      // if either link partner requests Clause 74 FEC and BOTH support it:
      *fec = BF_FEC_TYP_FIRECODE;
    } else if ((tx_bp_fec & rx_bp_fec & 0x1) &&
               ((tx_bp_fec | rx_bp_fec) & 0x2)) {
      // Use Clause 73 10G Base page FEC control bits to determine if whether
      // Clause 74 FEC is used
      *fec = BF_FEC_TYP_FIRECODE;
    } else {
      // NO FEC
      *fec = BF_FEC_TYP_NONE;
    }
  }

  return BF_SUCCESS;
}

/** \brief Return the HCD speed and fec mode negotiated on a Tofino port
 *
 * [ POST_ENABLE ]
 * Note: this function is deprecated, use bf_port_autoneg_hcd_fec_get_v2()
 * instead.
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param hcd     : Highest Common Denominator speed negotiated
 * \param fec     : FEC mode negotiated
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: hcd (return value pointer) NULL
 * \return: BF_INVALID_ARG: fec (return value pointer) NULL
 *
 */
bf_status_t bf_port_autoneg_hcd_fec_get(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        bf_port_speed_t *hcd,
                                        bf_fec_type_t *fec) {
  uint32_t av_hcd;
  bf_status_t bf_status = BF_SUCCESS;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (hcd == NULL) return BF_INVALID_ARG;
  if (fec == NULL) return BF_INVALID_ARG;

  if (!port_mgr_dev_is_tof1(dev_id)) {
    // FIXME: add sppt
    return BF_INVALID_ARG;
  }

  bf_status =
      bf_serdes_autoneg_hcd_fec_get(dev_id, dev_port, hcd, fec, &av_hcd);
  port_p->av_hcd = av_hcd;
  return bf_status;
}

/** \brief Return the HCD speed and fec negotiated on a Tof 1 or Tof 2  port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 * \param hcd_speed: Highest Common Denominator speed of negotiated mode
 * \param hcd_lanes: Highest Common Denominator lane number of negotiated mode
 *                   It can be NULL.
 * \param fec      : FEC mode negotiated. Note that this is only applicable to
 *                   10G, 25G (IEEE/Consortium) and 50G-R2 (Consortium) modes,
 *                   and it should be ignored for all other modes.
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: hcd (return value pointer) NULL
 * \return: BF_INVALID_ARG: fec (return value pointer) NULL
 *
 */
bf_status_t bf_port_autoneg_hcd_fec_get_v2(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           bf_port_speed_t *hcd_speed,
                                           int *hcd_lanes,
                                           bf_fec_type_t *fec) {
  port_mgr_port_t *port_p = NULL;
  bf_port_speed_t hcd_speed_tmp;
  bf_status_t rc = BF_SUCCESS;
  bf_fec_type_t fec_tmp;
  int hcd_lanes_tmp;
  uint64_t txbp;
  uint64_t rxbp;

  if (hcd_speed == NULL) return BF_INVALID_ARG;
  if (fec == NULL) return BF_INVALID_ARG;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof1(dev_id)) {
    rc = bf_port_autoneg_hcd_fec_get(dev_id, dev_port, hcd_speed, fec);
    if (rc != BF_SUCCESS) return rc;

    switch (*hcd_speed) {
      case BF_SPEED_1G:
      case BF_SPEED_10G:
      case BF_SPEED_25G:
        hcd_lanes_tmp = 1;
        break;
      case BF_SPEED_40G:
      case BF_SPEED_100G:
        hcd_lanes_tmp = 4;
        break;
      default:
        /* Invalid Tof 1 speed */
        hcd_lanes_tmp = 0;
    }
  }

  txbp = port_p->an_base_page;
  rxbp = port_p->an_lp_base_page;

  // Try to resolve HCD using base page
  rc = bf_port_autoneg_hcd_fec_resolve(
      dev_id, dev_port, txbp, rxbp, &hcd_speed_tmp, &hcd_lanes_tmp, &fec_tmp);

  if (rc == BF_SUCCESS) {
    port_mgr_log(
        "%d:%3.3d:0: AN Base Page Tx: %04x_%04x_%04x   "
        "Rx: %04x_%04x_%04x hcd(speed)=%2.2d hcd(lanes)=%d fec=%s",
        dev_id,
        dev_port,
        (uint32_t)((txbp >> 32ull) & 0xFFFFull),
        (uint32_t)((txbp >> 16ull) & 0xFFFFull),
        (uint32_t)(txbp & 0xFFFFull),
        (uint32_t)((rxbp >> 32ull) & 0xFFFFull),
        (uint32_t)((rxbp >> 16ull) & 0xFFFFull),
        (uint32_t)(rxbp & 0xFFFFull),
        hcd_speed_tmp,
        hcd_lanes_tmp,
        (fec_tmp == 2) ? "RS" : (fec_tmp == 1) ? "FC" : "NONE");
  }

  // if no HCD in base page, try to resolve using next pages
  if (hcd_speed_tmp == BF_SPEED_NONE && port_p->an_num_next_pages >= 2 &&
      port_p->an_lp_num_next_pages >= 2) {
    rc = bf_port_autoneg_hcd_fec_resolve_nxt_pg(dev_id,
                                                dev_port,
                                                txbp,
                                                rxbp,
                                                port_p->an_next_page[1],
                                                port_p->an_lp_next_page[1],
                                                &hcd_speed_tmp,
                                                &hcd_lanes_tmp,
                                                &fec_tmp);
    if (rc == BF_SUCCESS && port_p->an_log_ena) {
      port_mgr_log(
          "%d:%3.3d:0: AN Next Page #2 Tx: %04x_%04x_%04x   "
          "Rx: %04x_%04x_%04x hcd(speed)=%2.2d hcd(lanes)=%d fec=%s",
          dev_id,
          dev_port,
          (uint32_t)((port_p->an_next_page[1] >> 32ull) & 0xFFFFull),
          (uint32_t)((port_p->an_next_page[1] >> 16ull) & 0xFFFFull),
          (uint32_t)(port_p->an_next_page[1] & 0xFFFFull),
          (uint32_t)((port_p->an_lp_next_page[1] >> 32ull) & 0xFFFFull),
          (uint32_t)((port_p->an_lp_next_page[1] >> 16ull) & 0xFFFFull),
          (uint32_t)(port_p->an_lp_next_page[1] & 0xFFFFull),
          hcd_speed_tmp,
          hcd_lanes_tmp,
          (fec_tmp == 2) ? "RS" : (fec_tmp == 1) ? "FC" : "NONE");
    }
  }

  if (hcd_lanes) *hcd_lanes = hcd_lanes_tmp;
  *hcd_speed = hcd_speed_tmp;
  *fec = fec_tmp;
  bf_port_autoneg_ext_log_ena(dev_id, dev_port, false);

  return rc;
}

/** \brief Return the PAUSE resolutuion negotiated on a (1G) Tofino port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param tx_pause: true=enabled, false=disabled
 * \param rx_pause: true=enabled, false=disabled
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: tx_pause (return value pointer) NULL
 * \return: BF_INVALID_ARG: rx_pause (return value pointer) NULL
 *
 */
bf_status_t bf_port_autoneg_pause_resolution_get(bf_dev_id_t dev_id,
                                                 bf_dev_port_t dev_port,
                                                 bool *tx_pause,
                                                 bool *rx_pause) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (tx_pause == NULL) return BF_INVALID_ARG;
  if (rx_pause == NULL) return BF_INVALID_ARG;

  if (!port_mgr_dev_is_tof1(dev_id)) {
    // FIXME: add sppt
    return BF_INVALID_ARG;
  }

  // tbd
  return BF_SUCCESS;
}

/** \brief Return the state of clause 73/72 AN and Link-Training on a Tofino
 *port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param an_cmplt: AN+Link training state
 *                : true=done,
 *                : false=failed or in-progress
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: an_cmplt (return value pointer) NULL
 *
 */
bf_status_t bf_port_autoneg_complete_get(bf_dev_id_t dev_id,
                                         bf_dev_port_t dev_port,
                                         bool *an_cmplt) {
  bf_an_state_e an_st = BF_AN_ST_NONE;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (an_cmplt == NULL) return BF_INVALID_ARG;

  if (!port_mgr_dev_is_tof1(dev_id)) {
    // FIXME: add sppt
    return BF_INVALID_ARG;
  }

  bf_port_autoneg_state_get(dev_id, dev_port, &an_st);

  port_mgr_log("bf_port_autoneg_state_get: an_st=%d\n", an_st);

  *an_cmplt = (an_st == BF_AN_ST_COMPLETE) ? true : false;
  return BF_SUCCESS;
}

/** \brief Set 1588 Tx delta on a tofino port
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param delta   : time delta to set (0-65535)
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_1588_timestamp_delta_tx_set(bf_dev_id_t dev_id,
                                                bf_dev_port_t dev_port,
                                                uint16_t delta) {
  if (port_mgr_map_dev_port_to_port(dev_id, dev_port) == NULL) {
    return BF_INVALID_ARG;
  }
  if (port_mgr_dev_is_tof1(dev_id)) {
    return port_mgr_mac_set_1588_timestamp_delta_tx(dev_id, dev_port, delta);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    return port_mgr_tof2_umac_set_1588_timestamp_delta_tx(
        dev_id, dev_port, delta);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    return port_mgr_tof3_tmac_1588_timestamp_delta_tx_set(
        dev_id, dev_port, delta);
  } else {
    return BF_INVALID_ARG;
  }
}

/** \brief Get 1588 Tx delta on a tofino port
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param delta   : returned time delta
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_1588_timestamp_delta_tx_get(bf_dev_id_t dev_id,
                                                bf_dev_port_t dev_port,
                                                uint16_t *delta) {
  if (port_mgr_map_dev_port_to_port(dev_id, dev_port) == NULL) {
    return BF_INVALID_ARG;
  }

  if (port_mgr_dev_is_tof1(dev_id)) {
    return port_mgr_mac_get_1588_timestamp_delta_tx(dev_id, dev_port, delta);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    return port_mgr_tof2_umac_get_1588_timestamp_delta_tx(
        dev_id, dev_port, delta);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    return port_mgr_tof3_tmac_1588_timestamp_delta_tx_get(
        dev_id, dev_port, delta);
  } else {
    return BF_INVALID_ARG;
  }
}

/** \brief Set 1588 Rx delta on a tofino port
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param delta   : time delta to set (0-65535)
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_1588_timestamp_delta_rx_set(bf_dev_id_t dev_id,
                                                bf_dev_port_t dev_port,
                                                uint16_t delta) {
  if (port_mgr_map_dev_port_to_port(dev_id, dev_port) == NULL) {
    return BF_INVALID_ARG;
  }

  if (port_mgr_dev_is_tof1(dev_id)) {
    return port_mgr_mac_set_1588_timestamp_delta_rx(dev_id, dev_port, delta);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    return port_mgr_tof2_umac_set_1588_timestamp_delta_rx(
        dev_id, dev_port, delta);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    return port_mgr_tof3_tmac_1588_timestamp_delta_rx_set(
        dev_id, dev_port, delta);
  } else {
    return BF_INVALID_ARG;
  }
}

/** \brief Get 1588 Rx delta on a tofino port
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param delta   : returned time delta
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_1588_timestamp_delta_rx_get(bf_dev_id_t dev_id,
                                                bf_dev_port_t dev_port,
                                                uint16_t *delta) {
  if (port_mgr_map_dev_port_to_port(dev_id, dev_port) == NULL) {
    return BF_INVALID_ARG;
  }

  if (port_mgr_dev_is_tof1(dev_id)) {
    return port_mgr_mac_get_1588_timestamp_delta_rx(dev_id, dev_port, delta);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    return port_mgr_tof2_umac_get_1588_timestamp_delta_rx(
        dev_id, dev_port, delta);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    return port_mgr_tof3_tmac_1588_timestamp_delta_rx_get(
        dev_id, dev_port, delta);
  } else {
    return BF_INVALID_ARG;
  }
}

/** \brief Get 1588 Tx timestamp on a tofino port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param ts      : returned timestamp
 * \param ts_valid: returned timestamp valid indication
 * \param ts_id   : returned timestamp id (0-3)
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: ts == NULL
 * \return: BF_INVALID_ARG: ts_valid == NULL
 * \return: BF_INVALID_ARG: ts_id == NULL
 *
 */
bf_status_t bf_port_1588_timestamp_tx_get(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port,
                                          uint64_t *ts,
                                          bool *ts_valid,
                                          int *ts_id) {
  if (port_mgr_map_dev_port_to_port(dev_id, dev_port) == NULL) {
    return BF_INVALID_ARG;
  }
  if (ts == NULL) return BF_INVALID_ARG;
  if (ts_valid == NULL) return BF_INVALID_ARG;
  if (ts_id == NULL) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof1(dev_id)) {
    return port_mgr_mac_get_1588_timestamp_tx(
        dev_id, dev_port, ts, ts_valid, ts_id);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    return port_mgr_tof2_umac_get_1588_timestamp_tx(
        dev_id, dev_port, ts, ts_valid, ts_id);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    return port_mgr_tof3_tmac_1588_timestamp_tx_get(
        dev_id, dev_port, ts, ts_valid, ts_id);
  } else {
    return BF_INVALID_ARG;
  }
}

/** \brief Return the operational state (up/down) of a Tofino2 port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param state   : 0=down, 1=up
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: state (returned value) NULL
 */
static bf_status_t bf_port_tof2_oper_state_get_internal(bf_dev_id_t dev_id,
                                                        bf_dev_port_t dev_port,
                                                        int *state) {
  bf_status_t rc = BF_SUCCESS;
  bool up = false;
  bool is_sw_model = false;

  bool is_emulator = false;
  bool sig_detect = false, phy_ready = false;
  int num_lanes;

  bf_drv_device_type_get(dev_id, &is_sw_model);
#if defined(DEVICE_IS_EMULATOR)  // Emulator
  is_emulator = true;
#endif
  if (is_sw_model && !is_emulator) {
    *state = 1;  // just call it up on the model
  } else {
    rc = port_mgr_tof2_port_oper_state_get(dev_id, dev_port, &up);
    *state = up ? 1 : 0;
  }
    if (rc != BF_SUCCESS) return rc;

    if (!up) {
      return rc;
    }

    // If link state is up, then validate serdes status as well
    rc = bf_port_num_lanes_get(dev_id, dev_port, &num_lanes);
    for (int ln = 0; ln < num_lanes; ln++) {
      rc = port_mgr_tof2_serdes_sig_detect_get(
          dev_id, dev_port, ln, &sig_detect, &phy_ready);
      if (rc != BF_SUCCESS) continue;
      if (!sig_detect || !phy_ready) {
        *state = 0;
        return rc;
      }
    }
  return rc;
}

/** \brief Return the operational state (up/down) of a Tofino3 port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param state   : 0=down, 1=up
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: state (returned value) NULL
 */
static bf_status_t bf_port_tof3_oper_state_get_internal(bf_dev_id_t dev_id,
                                                        bf_dev_port_t dev_port,
                                                        int *state) {
  bf_status_t rc = BF_SUCCESS;
  bool up = false;
  bool is_sw_model = false;

  bf_drv_device_type_get(dev_id, &is_sw_model);
  if (!is_sw_model) {
    rc = port_mgr_tof3_port_oper_state_get(dev_id, dev_port, &up);
    *state = up ? 1 : 0;
  } else {
    *state = 1;  // just fake it
  }
  return rc;
}

/** \brief Return the extended operational state (up/down) of a Tofino2 port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 * \param state    : 0=down, 1=up
 * \param pcs_ready: true=pcs ready (block & alignment lock), false= not ready
 * \param l_fault  : pcs is receiving local_fault ordered set
 * \param l_fault  : pcs is receiving remote ordered set
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: state (returned value) NULL
 */
static bf_status_t bf_port_tof2_oper_state_get_extended_internal(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    int *state,
    bool *pcs_ready,
    bool *l_fault,
    bool *r_fault) {
  bf_status_t rc = BF_SUCCESS;
  bool is_sw_model = false;
  port_mgr_port_t *port_p;
  bool up = false;
  bool sig_detect = false, phy_ready = false;
  int num_lanes;

  if (!state || !pcs_ready) return BF_INVALID_ARG;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  if ((port_p->sw.assigned == 0) || (port_p->sw.enabled == 0)) {
    *state = 0;
    *pcs_ready = false;
    if (l_fault) *l_fault = false;
    if (r_fault) *r_fault = false;
  } else {
    bool is_emulator = false;

    bf_drv_device_type_get(dev_id, &is_sw_model);
#if defined(DEVICE_IS_EMULATOR)  // Emulator
    is_emulator = true;
#endif
    if (is_sw_model && !is_emulator) {
      *state = 1;  // just call it up on the model
      *pcs_ready = true;
      if (l_fault) *l_fault = false;
      if (r_fault) *r_fault = false;
    } else {
      *state = 0;
      *pcs_ready = false;
      if (l_fault) *l_fault = false;
      if (r_fault) *r_fault = false;
      rc = port_mgr_tof2_port_oper_state_extended_get(
          dev_id, dev_port, &up, pcs_ready, l_fault, r_fault);
      if (rc != BF_SUCCESS) return rc;

      *state = up ? 1 : 0;
      if (!up) {
        return rc;
      }

      // If link state is up, then validate serdes status as well
      rc = bf_port_num_lanes_get(dev_id, dev_port, &num_lanes);
      for (int ln = 0; ln < num_lanes; ln++) {
        rc = port_mgr_tof2_serdes_sig_detect_get(
            dev_id, dev_port, ln, &sig_detect, &phy_ready);
        if (rc != BF_SUCCESS) continue;
        if (!sig_detect || !phy_ready) {
          *state = 0;
          return rc;
        }
      }
    }
  }
  return rc;
}

/** \brief
 *
 */
bf_status_t bf_port_oper_state_set_pending_callbacks(bf_dev_id_t dev_id,
                                                     bf_dev_port_t dev_port,
                                                     int st) {
  port_mgr_port_t *port_p = NULL;
  port_mgr_dev_t *dev_p = port_mgr_map_dev_id_to_dev_p(dev_id);
  if (dev_p == NULL) return BF_INVALID_ARG;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  if (st != port_p->sw.oper_state) {
    port_p->issue_callbacks = true;
  }
  port_p->sw.oper_state = st;
  return BF_SUCCESS;
}

/** \brief
 *
 */
bf_status_t bf_port_oper_state_set_and_issue_callbacks(bf_dev_id_t dev_id,
                                                       bf_dev_port_t dev_port,
                                                       int st) {
  port_mgr_port_t *port_p = NULL;
  port_mgr_dev_t *dev_p = port_mgr_map_dev_id_to_dev_p(dev_id);
  if (dev_p == NULL) return BF_INVALID_ARG;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  if (st != port_p->sw.oper_state) {
    if (st) {
      port_mgr_log("%d:%3d: --- Up", dev_id, dev_port);
      port_mgr_link_up_actions(dev_id, dev_port);
    } else {
      port_mgr_log("%d:%3d: --- Dn", dev_id, dev_port);
      port_mgr_link_dn_actions(dev_id, dev_port);
    }
  }
  port_p->sw.oper_state = st;
  return BF_SUCCESS;
}

/** \brief Return the operational state (up/down) of a Tofino port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param state   : 0=down, 1=up
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: state (returned value) NULL
 */
static bf_status_t bf_port_oper_state_get_internal(bf_dev_id_t dev_id,
                                                   bf_dev_port_t dev_port,
                                                   int *state) {
  bf_status_t rc;
  int st, mac_block, ch;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  bool is_sw_model = false;

  if (port_p == NULL) return BF_INVALID_ARG;
  if (state == NULL) return BF_INVALID_ARG;

  bf_drv_device_type_get(dev_id, &is_sw_model);
  if (is_sw_model) {
    *state = 1;
    return BF_SUCCESS;
  }

  if ((port_p->sw.assigned == 0) || (port_p->sw.enabled == 0)) {
    *state = 0;
    return BF_SUCCESS;

  } else {
    // Fake it for mac-near;
    if (port_p->sw.lpbk_mode == BF_LPBK_MAC_NEAR) {
      *state = 1;
      return BF_SUCCESS;
    }
  }

  if (port_mgr_dev_is_tof2(dev_id)) {
    return bf_port_tof2_oper_state_get_internal(dev_id, dev_port, state);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    return bf_port_tof3_oper_state_get_internal(dev_id, dev_port, state);
  } else {
    port_mgr_map_dev_port_to_all(
        dev_id, dev_port, NULL, NULL, &mac_block, &ch, NULL);

    st = port_mgr_mac_link_state(dev_id, mac_block, ch, port_p->sw.oper_state);
    if (port_p->sw.fec == BF_FEC_TYP_REED_SOLOMON) {
      *state = st;
      return BF_SUCCESS;
    }
    /* If port is Down, continue to hold it down until BER improves
     * If the port is up though, don't take it down just for BER issues*/
    if (port_p->sw.oper_state == 0) {
      uint32_t ber_cnt;
      uint32_t errored_blk_cnt;
      uint32_t sync_loss;
      uint32_t block_lock_loss;
      uint32_t hi_ber_cnt;
      uint32_t valid_error_cnt;
      uint32_t unknown_error_cnt;
      uint32_t invalid_error_cnt;
      uint32_t bip_errors_per_pcs_lane[20];

      // Hold port "Dn" if too many PCS errors
      rc = bf_port_pcs_counters_get(dev_id,
                                    dev_port,
                                    &ber_cnt,
                                    &errored_blk_cnt,
                                    &sync_loss,
                                    &block_lock_loss,
                                    &hi_ber_cnt,
                                    NULL,   //&valid_error_cnt,
                                    NULL,   //&unknown_error_cnt,
                                    NULL,   //&invalid_error_cnt,
                                    NULL);  // bip_errors_per_pcs_lane
      if (rc != BF_SUCCESS) {
        return rc;
      }
      if ((errored_blk_cnt > 20) || ber_cnt || sync_loss || block_lock_loss ||
          hi_ber_cnt) {
        port_mgr_log(
            "%d:%3d: --- [%s] <ber=%d : errblk=%d : synlst=%d : "
            "blklklst=%d : hi_ber=%d>",
            dev_id,
            dev_port,
            st ? "Up" : "Dn",
            ber_cnt,
            errored_blk_cnt,
            sync_loss,
            block_lock_loss,
            hi_ber_cnt);
        // just log the PCS errors, dont act on them
        (void)valid_error_cnt;
        (void)unknown_error_cnt;
        (void)invalid_error_cnt;
        (void)bip_errors_per_pcs_lane;
      }
    } else if ((port_p->sw.single_lane_error_thresh > 0) &&
               (port_p->sw.speed == BF_SPEED_10G) &&
               (port_p->sw.fec == BF_FEC_TYP_NONE)) {
      uint32_t ber_cnt;
      uint32_t errored_blk_cnt;
      uint32_t sync_loss;
      uint32_t block_lock_loss;
      uint32_t hi_ber_cnt;
      uint32_t valid_error_cnt;
      uint32_t unknown_error_cnt;
      uint32_t invalid_error_cnt;
      uint32_t bip_errors_per_pcs_lane[20];

      // Hold port "Dn" if too many PCS errors
      rc = bf_port_pcs_counters_get(dev_id,
                                    dev_port,
                                    &ber_cnt,
                                    &errored_blk_cnt,
                                    &sync_loss,
                                    &block_lock_loss,
                                    &hi_ber_cnt,
                                    NULL,   //&valid_error_cnt,
                                    NULL,   //&unknown_error_cnt,
                                    NULL,   //&invalid_error_cnt,
                                    NULL);  // bip_errors_per_pcs_lane
      if (rc != BF_SUCCESS) {
        return BF_INVALID_ARG;
      }
      if ((errored_blk_cnt > port_p->sw.single_lane_error_thresh) || ber_cnt ||
          sync_loss || block_lock_loss || hi_ber_cnt) {
        if (st) {
          port_mgr_log(
              "%d:%3d: --- [Up-dn] Up to down 10g non-FEC port showing errors",
              dev_id,
              dev_port);
          st = 0;
        }
        port_mgr_log(
            "%d:%3d: --- [%s] <ber=%d : errblk=%d : synlst=%d : "
            "blklklst=%d : hi_ber=%d>",
            dev_id,
            dev_port,
            st ? "Up" : "Dn",
            ber_cnt,
            errored_blk_cnt,
            sync_loss,
            block_lock_loss,
            hi_ber_cnt);
        // just log the PCS errors, dont act on them
        (void)valid_error_cnt;
        (void)unknown_error_cnt;
        (void)invalid_error_cnt;
        (void)bip_errors_per_pcs_lane;
      }
    }
    *state = st;
    return BF_SUCCESS;
  }
}

/** \brief Return the serdes signal state (true/false) of a
 *         Tofino2 port.
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param srds_sts: serdes signal state, true=signl detect, false=no signal
 * detect
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG  : for any errors related to passed parameter
 */
static bf_status_t bf_port_tof2_serdes_sig_sts_get(bf_dev_id_t dev_id,
                                                   bf_dev_port_t dev_port,
                                                   bool *srds_sts) {
  int num_lanes;
  bool sig_detect = false, phy_ready = false;
  bf_status_t rc;

  rc = bf_port_num_lanes_get(dev_id, dev_port, &num_lanes);
  if (rc != BF_SUCCESS) {
    return rc;
  }

  *srds_sts = true;
  for (int ln = 0; ln < num_lanes; ln++) {
    rc = port_mgr_tof2_serdes_sig_detect_get(
        dev_id, dev_port, ln, &sig_detect, &phy_ready);
    if (rc != BF_SUCCESS) {
      return rc;
    }
    if (!sig_detect || !phy_ready) {
      *srds_sts = 0;
      return BF_SUCCESS;
    }
  }

  return BF_SUCCESS;
}

/** \brief Return the serdes signal state (true/false) of a
 *         Tofino port.
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param srds_sts: serdes signal state, true=signl detect, false=no signal
 * detect
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG  : srds_sts pointer is invalid
 * \return: BF_NOT_SUPPORTED: Signal status fetch not supported on this device
 */
bf_status_t bf_port_serdes_sig_sts_get(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port,
                                       bool *srds_sts) {
  bf_status_t rc = BF_SUCCESS;

  if (!srds_sts) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof2(dev_id)) {
    rc = bf_port_tof2_serdes_sig_sts_get(dev_id, dev_port, srds_sts);
  } else {
    rc = BF_NOT_SUPPORTED;
  }

  return rc;
}

/** \brief Return the extended operational state (up/down) of a
 *         Tofino port.
 *
 *  Note that references to local/remote faults may be NULL. If both pointers
 *  are NULL, the link interrupt register is not read. This is mostly used when
 *  link interrupts are enabled.
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param state   : link state, 0=down, 1=up
 * \param pcs_rdy : pcs ready, false=not ready, true=ready
 * \param l_fault : local fault event, false=no, true=yes. May be NULL.
 * \param r_fault : remote fault event, false=no, true=yes May be NULL.
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: state, pcs_rdy, l_fault, r_faul NULL
 */
bf_status_t bf_port_oper_state_get_extended(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port,
                                            int *state,
                                            bool *pcs_rdy,
                                            bool *l_fault,
                                            bool *r_fault) {
  port_mgr_dev_t *dev_p = NULL;
  bool is_sw_model = false;
  port_mgr_port_t *port_p;
  int remote_fault = 0;
  int local_fault = 0;
  bool block_lock_all;
  bf_status_t rc = BF_SUCCESS;
  int mac_block;
  bool hi_ber;
  int st = 0;
  int ch, is_emu = 0;

  if (!state || !pcs_rdy) {
    return BF_INVALID_ARG;
  }

  dev_p = port_mgr_map_dev_id_to_dev_p(dev_id);
  if (dev_p == NULL) return BF_INVALID_ARG;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) {
    return BF_INVALID_ARG;
  }

  bf_drv_device_type_get(dev_id, &is_sw_model);
  if (is_sw_model) {
    *state = 1;
    *pcs_rdy = true;
    if (l_fault) *l_fault = false;
    if (r_fault) *r_fault = false;
    return BF_SUCCESS;
  }

  if (port_mgr_dev_is_tof2(dev_id)) {
    rc = bf_port_tof2_oper_state_get_extended_internal(
        dev_id, dev_port, &st, pcs_rdy, l_fault, r_fault);
    if (rc != BF_SUCCESS) return rc;

    if (dev_p->ldev.interrupt_based_link_monitoring && *state != 0) {
      /*  if we want to monitor link status over interrupt,
       *  override state with current value and return here
       */
      *state = port_p->sw.oper_state;
      return BF_SUCCESS;
    }
    // Set the flag to indicate that we need to issue callbacks
    if (port_p->sw.oper_state != st && port_p->sw.loopback_enabled) {
      port_p->issue_callbacks = true;
      port_p->sw.oper_state = st;
    }
    *state = st;
    return BF_SUCCESS;
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    // TBD for TF3
    return BF_SUCCESS;
  }

  port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &mac_block, &ch, NULL);

  if (port_p->sw.assigned && port_p->sw.enabled) {
    // Fake return values for mac-near or for tx-only mode.
    // Note that forced_pcs_loopback == true indicates tx-only mode enabled.
    if (port_p->sw.lpbk_mode == BF_LPBK_MAC_NEAR ||
        port_p->forced_pcs_loopback) {
      *state = 1;
      *pcs_rdy = true;
      if (l_fault) *l_fault = false;
      if (r_fault) *r_fault = false;
      return BF_SUCCESS;
    }

#if defined(DEVICE_IS_EMULATOR)  // Emulator
    is_emu = 1;
#endif
    // Read PCS ready status
    rc = bf_port_pcs_status_get_v2(
        dev_id, dev_port, pcs_rdy, &hi_ber, &block_lock_all);
    if (rc != BF_SUCCESS) {
      return rc;
    }

    port_p->lstate.pcs_rdy = *pcs_rdy;
    port_p->lstate.hi_ber = hi_ber;
    port_p->lstate.blocklockall = block_lock_all;

    if (is_emu) {
      st = port_mgr_mac_live_link_state(dev_id, mac_block, ch);
      *state = st;
      if (l_fault) *l_fault = false;
      if (r_fault) *r_fault = false;
      return BF_SUCCESS;
    }

    if ((l_fault || r_fault) &&
        (!dev_p->ldev.interrupt_based_link_monitoring || (*state == 0))) {
      // Read link state (up/down) and link fault interrupt register.
      rc = port_mgr_mac_link_state_v2(
          dev_id, mac_block, ch, &st, &local_fault, &remote_fault);
      if (rc != BF_SUCCESS) {
        return rc;
      }
    } else {
      /* Whether both pointers, l_fault and r_fault, are NULL, or if interrupt
       * based link monitoring is enabled: just read the live link status.
       */
      st = port_mgr_mac_live_link_state(dev_id, mac_block, ch);
    }
  }
  // rc is always BF_SUCCESS. So commening out below line as suggested by
  // Coverity.
  // if (rc != BF_SUCCESS) return rc;

  // Force link down for 25g && forced fec reset
  if ((port_p->sw.speed == BF_SPEED_25G) && (port_p->fec_reset_inp)) {
    st = 0;
  }

  // For 1G only: verride pcs_ready with the status of the link
  if (port_p->sw.speed == BF_SPEED_1G) {
    *pcs_rdy = (st != 0);
    port_p->lstate.pcs_rdy = *pcs_rdy;
  }

  *state = st;
  if (l_fault) {
    *l_fault = port_p->lstate.local_fault ? true : false;
  }

  if (r_fault) {
    *r_fault = port_p->lstate.remote_fault ? true : false;
  }

  return BF_SUCCESS;
}

/** \brief Update operational state (up/down) of a Tofino port.
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param state   : link state, 0=down, 1=up
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: state, pcs_rdy, l_fault, r_faul NULL
 */
bf_status_t bf_port_oper_state_update(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      int state) {
  port_mgr_port_t *port_p;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) {
    return BF_INVALID_ARG;
  }

  if (port_p->sw.oper_state != state) {
    port_p->issue_callbacks = true;
    port_p->sw.oper_state = state;
  }
  return BF_SUCCESS;
}

/** \brief Return the operational state (up/down) of a Tofino port and
 *  issue callbacks to the registered modules later. This is called
 *  in the interrupt context.
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param state   : 0=down, 1=up
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: state (returned value) NULL
 */
bf_status_t bf_port_oper_state_get_and_issue_callbacks(bf_dev_id_t dev_id,
                                                       bf_dev_port_t dev_port,
                                                       int *state) {
  int st;
  bf_status_t sts;
  port_mgr_dev_t *dev_p = NULL;
  port_mgr_port_t *port_p = NULL;
  dev_p = port_mgr_map_dev_id_to_dev_p(dev_id);
  if (dev_p == NULL) return BF_INVALID_ARG;

  if (!dev_p->ldev.interrupt_based_link_monitoring) {
    /* This function is called by the interrupt handler. Thus if we don't want
       to monitor link status over interrupt, just return here */
    return BF_SUCCESS;
  }

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;
  if (state == NULL) return BF_INVALID_ARG;

  sts = bf_port_oper_state_get_internal(dev_id, dev_port, &st);
  if (sts != BF_SUCCESS) {
    port_mgr_log("%d:%3d: Unable to get the oper state for port : %s (%d)",
                 dev_id,
                 dev_port,
                 bf_err_str(sts),
                 sts);
    return sts;
  }

  if (st != port_p->sw.oper_state) {
    if (st) {
      port_mgr_log("%d:%3d: --- Up", dev_id, dev_port);
      port_mgr_link_up_actions(dev_id, dev_port);
    } else {
      port_mgr_log("%d:%3d: --- Dn", dev_id, dev_port);
      port_mgr_link_dn_actions(dev_id, dev_port);
    }
  }
  port_p->sw.oper_state = st;
  *state = st;
  return BF_SUCCESS;
}

/** \brief Return the operational state (up/down) of a Tofino port and
 *  indicate if we need to issue callbacks to the registered modules later
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param state   : 0=down, 1=up
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: state (returned value) NULL
 */
bf_status_t bf_port_oper_state_get(bf_dev_id_t dev_id,
                                   bf_dev_port_t dev_port,
                                   int *state) {
  int st;
  bf_status_t sts;
  port_mgr_dev_t *dev_p = NULL;
  port_mgr_port_t *port_p = NULL;

  dev_p = port_mgr_map_dev_id_to_dev_p(dev_id);
  if (dev_p == NULL) return BF_INVALID_ARG;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;
  if (state == NULL) return BF_INVALID_ARG;

  if (dev_p->ldev.interrupt_based_link_monitoring && *state) {
    /*  if we want to monitor link
     *  status over interrupt, just return here
     */
    *state = port_p->sw.oper_state;  // leave st unchanged
    return BF_SUCCESS;
  }

  sts = bf_port_oper_state_get_internal(dev_id, dev_port, &st);
  if (sts != BF_SUCCESS) {
    port_mgr_log("%d:%3d: Unable to get the oper state for port : %s (%d)",
                 dev_id,
                 dev_port,
                 bf_err_str(sts),
                 sts);
    return sts;
  }

  if ((port_p->sw.speed == BF_SPEED_25G || port_p->sw.speed == BF_SPEED_50G) &&
      (port_p->fec_reset_inp)) {
    port_mgr_log("%d:%3d: FEC reset in-progress. Hold dn", dev_id, dev_port);
    st = 0;  // keep link-down
  }

  // Set the flag to indicate that we need to issue callbacks
  if (port_p->sw.oper_state != st) {
    port_p->issue_callbacks = true;
  }
  port_p->sw.oper_state = st;
  *state = st;
  return BF_SUCCESS;
}

/** \brief Issue callbacks to the registered modules if needed
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 */
bf_status_t bf_port_oper_state_callbacks_issue(bf_dev_id_t dev_id,
                                               bf_dev_port_t dev_port) {
  port_mgr_dev_t *dev_p = NULL;
  port_mgr_port_t *port_p = NULL;

  dev_p = port_mgr_map_dev_id_to_dev_p(dev_id);
  if (dev_p == NULL) return BF_INVALID_ARG;

  if (dev_p->ldev.interrupt_based_link_monitoring) {
    /*  if we want to monitor link
     *  status over interrupt, just return here
     */
    return BF_SUCCESS;
  }

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_p->issue_callbacks) {
    if (port_p->sw.oper_state) {
      port_mgr_log("%d:%3d: --- Up", dev_id, dev_port);
      port_mgr_link_up_actions(dev_id, dev_port);
    } else {
      port_mgr_log("%d:%3d: --- Dn", dev_id, dev_port);
      port_mgr_link_dn_actions(dev_id, dev_port);
    }
    port_p->issue_callbacks = false;
  }
  return BF_SUCCESS;
}

/** \brief Return if oper state callbacks are pending for this port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param pending : true if callbacks pending; false otherwise
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: pending (returned value) NULL
 */
bf_status_t bf_port_is_oper_state_callback_pending(bf_dev_id_t dev_id,
                                                   bf_dev_port_t dev_port,
                                                   bool *pending) {
  port_mgr_port_t *port_p = NULL;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;
  if (pending == NULL) return BF_INVALID_ARG;

  *pending = port_p->issue_callbacks;
  return BF_SUCCESS;
}

/** \brief Return the operational state (up/down) of a Tofino port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param state   : 0=down, 1=up
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: state (returned value) NULL
 */
bf_status_t bf_port_oper_state_get_no_side_effect(bf_dev_id_t dev_id,
                                                  bf_dev_port_t dev_port,
                                                  int *state) {
  bf_status_t sts;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (state == NULL) return BF_INVALID_ARG;

  sts = bf_port_oper_state_get_internal(dev_id, dev_port, state);
  if (sts != BF_SUCCESS) {
    port_mgr_log("%d:%3d: Unable to get the oper state for port : %s (%d)",
                 dev_id,
                 dev_port,
                 bf_err_str(sts),
                 sts);
    return sts;
  }

  return BF_SUCCESS;
}

/** \brief Return true if dev_port is associated with a MAC
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BFN_MAX_ASICS-1)
 * \param dev_port: encoded port identifier
 * \param has_mac : returned
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BFN_MAX_ASICS-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: return value ptr (has_mac) NULL
 *
 */
bf_status_t bf_port_has_mac(bf_dev_id_t dev_id,
                            bf_dev_port_t dev_port,
                            bool *has_mac) {
  int mac_block, err;

  if (has_mac == NULL) return BF_INVALID_ARG;

  err = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &mac_block, NULL, NULL);
  if (err == 0) {
    *has_mac = true;
    return BF_SUCCESS;
  }
  *has_mac = false;
  return BF_SUCCESS;
}

/** \brief Directly read a Tofino MAC register
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param reg     : MAC register to read
 * \param val     : returned, value read
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 */
bf_status_t bf_port_mac_rd(bf_dev_id_t dev_id,
                           bf_dev_port_t dev_port,
                           uint32_t reg,
                           uint32_t *val) {
  int mac_block;

  port_mgr_port_t *port_p =
      port_mgr_map_dev_port_to_port_allow_unassigned(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (val == NULL) return BF_INVALID_ARG;

  if (!port_mgr_dev_is_tof1(dev_id)) {
    // FIXME: add sppt
    return BF_INVALID_ARG;
  }

  port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &mac_block, NULL, NULL);

  *val = mac_read(dev_id, mac_block, reg);
  return BF_SUCCESS;
}

/** \brief Directly write a Tofino MAC register
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param reg     : MAC register to modify
 * \param val     : value to write
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 */
bf_status_t bf_port_mac_wr(bf_dev_id_t dev_id,
                           bf_dev_port_t dev_port,
                           uint32_t reg,
                           uint32_t val) {
  int mac_block;
  port_mgr_port_t *port_p =
      port_mgr_map_dev_port_to_port_allow_unassigned(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  if (!port_mgr_dev_is_tof1(dev_id)) {
    // FIXME: add sppt
    return BF_INVALID_ARG;
  }

  port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &mac_block, NULL, NULL);

  mac_write(dev_id, mac_block, reg, val);
  return BF_SUCCESS;
}

/** \brief Given a bf_dev_port_t, return the associated mac_block and channel
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 * \param mac_block: (returned) MAC register to modify
 * \param ch       : (returned) MAC channel
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 */
bf_status_t bf_port_map_dev_port_to_mac(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        bf_mac_block_id_t *mac_block,
                                        int *ch) {
  port_mgr_err_t err;

  err = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, (int *)mac_block, ch, NULL);
  if (err != 0) return BF_INVALID_ARG;

  return BF_SUCCESS;
}

/** \brief Given a bf_mac_block_id_t, return the associated mac_block and
 *channel
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param mac_block: logical mac-block
 * \param ch       : MAC channel
 * \param dev_port : (returned) encoded port identifier
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 */
bf_status_t bf_port_map_mac_to_dev_port(bf_dev_id_t dev_id,
                                        bf_mac_block_id_t mac_block,
                                        int ch,
                                        bf_dev_port_t *dev_port) {
  lld_err_t err;
  uint32_t phy_mac_block;

  // get phy mac_block from logical
  err = lld_sku_map_log2phy_mac_block(dev_id, mac_block, &phy_mac_block);
  if (err != LLD_OK) return BF_INVALID_ARG;

  err = lld_sku_map_mac_ch_to_dev_port_id(
      dev_id, phy_mac_block, (uint32_t)ch, dev_port);
  if (err != LLD_OK) return BF_INVALID_ARG;

  return BF_SUCCESS;
}

/** \brief Return the number of lanes used by the specified port
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 * \param num_lanes: (returned) number of lanes used
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: return value ptr NULL
 * \return: BF_INVALID_ARG: invalid speed (or not assigned) for port
 */
bf_status_t bf_port_num_lanes_get(bf_dev_id_t dev_id,
                                  bf_dev_port_t dev_port,
                                  int *num_lanes) {
  if (num_lanes == NULL) return BF_INVALID_ARG;

  *num_lanes = port_mgr_get_num_lanes(dev_id, dev_port);
  if (*num_lanes == 0) return BF_INVALID_ARG;

  return BF_SUCCESS;
}

/*****************************************************************
* bf_an_advertisement_set
*
* Construct the base page and (if necessary) next-page code words
* based on the specified ability/request bits.
*
* The 48b codewords are returned in the lower 48 bits of a
* user-specified array of uint64_t entries. The size of this
* array should be passed as the value of max_pgs
*
* Currently, AN for consortium requires a base page and 2 next
* pages, so the "pgs" array can be 3x64b entries and max_pgs
* set to "3".
*
* If no Consortium advertisements are requiested then num_pgs
* will be set to "1" and pgs[0] will contain the base page. No
* next pages need to be sent (NP=0)
*
* If any Consortium advertisements are requested then num_pgs
* will be set to "3". pgs[0] will contain the base page. NP=1.
* pgs[1] will contain an OUI tagged formatted next pagei with
* message code="5". OUI is set to "oui" (which, currently,
* should be the Ethernet Consortium CID=0x6A737D). NP=1

* pgs[2] will contain an OUI tagged unformatted next page with
* the specified consortium advertisements. OUI is set to "oui"
* (which, currently, should be the Ethernet Consortium CID
* (0x6A737D). NP=0.
*/
bf_status_t bf_an_advertisement_set(bf_an_adv_speeds_t speed_adv,
                                    bf_an_pause_t pause_adv,
                                    bf_an_fec_t fec_adv,
                                    uint32_t oui,
                                    uint32_t max_pgs,
                                    uint32_t *num_pgs,
                                    uint64_t *pgs) {
  bool consortium_mode = false;
  uint32_t sel_fld = 1, np = 0, tech_abl = 0, c0 = 0, c1 = 0;
  uint32_t f0 = 0, f1 = 0, f2 = 0, f3 = 0, f4 = 0;
  uint64_t codeword = 0ull, oui_upper = 0ull, oui_lower = 0ull;
  uint64_t ext_tech_abl = 0ull;

  if (num_pgs == NULL) return BF_INVALID_ARG;
  if (pgs == NULL) return BF_INVALID_ARG;

  // sanity check, cant request what we dont support
  if ((fec_adv & BF_ADV_FEC_FC_10G_REQUEST_IEEE) &&
      !(fec_adv & BF_ADV_FEC_FC_10G_ABILITY_IEEE)) {
    return BF_INVALID_ARG;
  }
  if ((fec_adv & BF_ADV_FEC_RS_REQUEST_CONSORTIUM) &&
      !(fec_adv & BF_ADV_FEC_RS_ABILITY_CONSORTIUM)) {
    return BF_INVALID_ARG;
  }
  if ((fec_adv & BF_ADV_FEC_FC_REQUEST_CONSORTIUM) &&
      !(fec_adv & BF_ADV_FEC_FC_ABILITY_CONSORTIUM)) {
    return BF_INVALID_ARG;
  }

  /* 73.6.4
   * For 25 Gb/s operation the same bits are used to advertise backplane and
   * copper cable assembly operation. For other speeds, a PHY for operation
   * over an electrical backplane (e.g., 1000BASE-KX, 10GBASE-KX4, 10GBASE-KR,
   * 40GBASE-KR4, 100GBASE-KP4, 100GBASE-KR4) shall not be advertised
   * simultaneously with a PHY for operation over a copper cable assembly
   * (e.g., 40GBASE-CR4, 100GBASE-CR10, 100GBASE-CR4)
   */
  if ((speed_adv & (BF_ADV_SPD_1000BASE_KX | BF_ADV_SPD_10GBASE_KR |
                    BF_ADV_SPD_40GBASE_KR4 | BF_ADV_SPD_100GBASE_KR4)) &&
      (speed_adv & (BF_ADV_SPD_40GBASE_CR4 | BF_ADV_SPD_100GBASE_CR4))) {
    return BF_INVALID_ARG;
  }

  /* 73.6.4
   * 25GBASE-KR-S abilities are a subset of 25GBASE-KR abilities, and likewise
   * 25GBASE-CR-S abilities are a subset of 25GBASE-CR abilities. To allow
   * interoperation between 25GBASE-KR-S and 25GBASE-KR PHY types, and between
   * 25GBASE-CR-S and 25GBASE-CR PHY types, a device that supports 25GBASE-KR
   * or 25GBASE-CR should advertise both A9 and A10 ability bits during
   * auto-negotiation.
   */
  if (speed_adv & BF_ADV_SPD_25GBASE_KR_CR) {
    speed_adv |= BF_ADV_SPD_25GBASE_KRS_CRS;
  }

  // if any consortium advertisements, need next pages
  if (speed_adv &
      ((BF_ADV_SPD_25GBASE_KR1_CONSORTIUM | BF_ADV_SPD_25GBASE_CR1_CONSORTIUM |
        BF_ADV_SPD_50GBASE_KR2_CONSORTIUM | BF_ADV_SPD_50GBASE_CR2_CONSORTIUM) |
       (BF_ADV_SPD_400GBASE_CR8_CONSORTIUM))) {
    consortium_mode = true;
  } else if (fec_adv & (BF_ADV_FEC_RS_ABILITY_CONSORTIUM |
                        BF_ADV_FEC_FC_ABILITY_CONSORTIUM)) {
    consortium_mode = true;
  }

  if (consortium_mode) {
    if (max_pgs < 3) {
      return BF_INVALID_ARG;  // need 3 pages
    } else {
      *num_pgs = 3;
      np = 1;
    }
  } else {
    np = 0;
    *num_pgs = 1;
  }

  // construct pause bits c0 and c1
  if (pause_adv & BF_ADV_PAUSE_RX) c0 = 1;
  if (pause_adv & BF_ADV_PAUSE_TX) c1 = 1;

  // construct base-pg technology ability field
  // Note: CONSORTIUM flags indicators that are coded in bits [24..30] should
  // be masked
  tech_abl |= speed_adv & 0x7fffff;

  // construct fec bits f0 and f1
  if (fec_adv & BF_ADV_FEC_FC_10G_ABILITY_IEEE) f0 = 1;
  if (fec_adv & BF_ADV_FEC_FC_10G_REQUEST_IEEE) f1 = 1;
  if (fec_adv & BF_ADV_FEC_RS_25G_REQUEST_IEEE) f2 = 1;
  if (fec_adv & BF_ADV_FEC_FC_25G_REQUEST_IEEE) f3 = 1;

#if 0
  // test for IXIA. IXIA seems to advertise 10G FEC modes for 100G
  if (f2) {
    f0 = 1;
    f1 = 1;
    f2 = 1;
  }
#endif

  codeword = ((uint64_t)sel_fld << 0ull);
  codeword |= ((uint64_t)c0 << 10ull);
  codeword |= ((uint64_t)c1 << 11ull);
  codeword |= ((uint64_t)np << 15ull);
  codeword |= ((uint64_t)tech_abl << 21ull);
  codeword |= ((uint64_t)f2 << 44ull);  // 25g FEC
  codeword |= ((uint64_t)f3 << 45ull);
  codeword |= ((uint64_t)f0 << 46ull);  // 10g FEC (legacy)
  codeword |= ((uint64_t)f1 << 47ull);

  pgs[0] = codeword;

  if (!consortium_mode) return BF_SUCCESS;

  // Construct the OUI tagged formatted next page (MP5)
  oui_upper = (oui >> 13ull) & 0x7FFull;  // upper 11b [23:13]
  oui_lower = (oui >> 2ull) & 0x7FFull;   // middle 11b [12:2]

  codeword = 0xA005ull | (oui_upper << 16ull) | (oui_lower << 32ull);
  pgs[1] = codeword;

  // Construct the OUI tagged unformatted next page (UP-1)
  // construct extended technology ability field
  ext_tech_abl = 0ull;
  if (speed_adv & BF_ADV_SPD_25GBASE_KR1_CONSORTIUM)
    ext_tech_abl |= (1 << 4ull);
  if (speed_adv & BF_ADV_SPD_25GBASE_CR1_CONSORTIUM)
    ext_tech_abl |= (1 << 5ull);
  if (speed_adv & BF_ADV_SPD_50GBASE_KR2_CONSORTIUM)
    ext_tech_abl |= (1 << 8ull);
  if (speed_adv & BF_ADV_SPD_50GBASE_CR2_CONSORTIUM)
    ext_tech_abl |= (1 << 9ull);
  if (speed_adv & BF_ADV_SPD_400GBASE_CR8_CONSORTIUM)
    ext_tech_abl |= (1 << 18ull);

  f1 = (fec_adv & BF_ADV_FEC_RS_ABILITY_CONSORTIUM) ? 1 : 0;
  f2 = (fec_adv & BF_ADV_FEC_FC_ABILITY_CONSORTIUM) ? 1 : 0;
  f3 = (fec_adv & BF_ADV_FEC_RS_REQUEST_CONSORTIUM) ? 1 : 0;
  f4 = (fec_adv & BF_ADV_FEC_FC_REQUEST_CONSORTIUM) ? 1 : 0;

  codeword = 0x203ull;
  codeword |= (oui & 3) << 9ull;
  codeword |= (ext_tech_abl << 16ull);
  codeword |= ((uint64_t)f1 << 40ull);
  codeword |= ((uint64_t)f2 << 41ull);
  codeword |= ((uint64_t)f3 << 42ull);
  codeword |= ((uint64_t)f4 << 43ull);

  pgs[2] = codeword;

  return BF_SUCCESS;
}

bf_status_t bf_an_advertisement_get(uint32_t num_pgs,
                                    uint64_t *pgs,
                                    bf_an_adv_speeds_t *speed_adv,
                                    bf_an_pause_t *pause_adv,
                                    bf_an_fec_t *fec_adv,
                                    uint32_t *oui) {
  uint64_t codeword = 0ull, oui_upper = 0ull, oui_lower = 0ull;
  uint64_t base_pg;

  if (speed_adv == NULL) return BF_INVALID_ARG;
  if (pause_adv == NULL) return BF_INVALID_ARG;
  if (fec_adv == NULL) return BF_INVALID_ARG;
  if (oui == NULL) return BF_INVALID_ARG;
  if (pgs == NULL) return BF_INVALID_ARG;
  if (num_pgs == 0) return BF_INVALID_ARG;

  *speed_adv = 0;
  *pause_adv = 0;
  *fec_adv = 0;

  // decode base pg
  base_pg = pgs[0];

  // reconstruct pause advert
  if (base_pg & (1 << 10ull)) *pause_adv |= BF_ADV_PAUSE_RX;
  if (base_pg & (1 << 11ull)) *pause_adv |= BF_ADV_PAUSE_TX;

  // reconstruct speed advert
  if (base_pg & (1 << 21ull)) *speed_adv |= BF_ADV_SPD_1000BASE_KX;
  if (base_pg & (1 << 22ull))
    *speed_adv |= BF_ADV_SPD_10GBASE_KX4;  // not supported
  if (base_pg & (1 << 23ull)) *speed_adv |= BF_ADV_SPD_10GBASE_KR;
  if (base_pg & (1 << 24ull)) *speed_adv |= BF_ADV_SPD_40GBASE_KR4;
  if (base_pg & (1 << 25ull)) *speed_adv |= BF_ADV_SPD_40GBASE_CR4;
  if (base_pg & (1 << 26ull))
    *speed_adv |= BF_ADV_SPD_100GBASE_CR10;  // not supported
  if (base_pg & (1 << 27ull))
    *speed_adv |= BF_ADV_SPD_40GBASE_KP4;  // not supported
  if (base_pg & (1 << 28ull)) *speed_adv |= BF_ADV_SPD_100GBASE_KR4;
  if (base_pg & (1 << 29ull)) *speed_adv |= BF_ADV_SPD_100GBASE_CR4;
  if (base_pg & (1 << 30ull)) *speed_adv |= BF_ADV_SPD_25GBASE_KRS_CRS;
  if (base_pg & (1 << 31ull)) *speed_adv |= BF_ADV_SPD_25GBASE_KR_CR;

  // reconstruct fec advert
  if (base_pg & (1 < 44ull)) *fec_adv |= BF_ADV_FEC_RS_25G_REQUEST_IEEE;
  if (base_pg & (1 < 45ull)) *fec_adv |= BF_ADV_FEC_FC_25G_REQUEST_IEEE;
  if (base_pg & (1 < 46ull)) *fec_adv |= BF_ADV_FEC_FC_10G_ABILITY_IEEE;
  if (base_pg & (1 < 47ull)) *fec_adv |= BF_ADV_FEC_FC_10G_REQUEST_IEEE;

  if (num_pgs == 1) return BF_SUCCESS;

  // reconstruct (part of) OUI
  codeword = pgs[1];
  oui_upper = (codeword >> 16ull) & 0x7FFull;
  oui_lower = (codeword >> 32ull) & 0x7FFull;
  *oui = ((oui_upper << 11ull) | oui_lower) << 2ull;

  if (num_pgs == 2) return BF_SUCCESS;  // even though only partially decoded

  codeword = pgs[2];

  // complete oui now
  *oui |= (codeword >> 9) & 0x3ull;

  // complete consortium speed and fec adverts
  if (codeword & (1 << 20ull)) *speed_adv |= BF_ADV_SPD_25GBASE_KR1_CONSORTIUM;
  if (codeword & (1 << 21ull)) *speed_adv |= BF_ADV_SPD_25GBASE_CR1_CONSORTIUM;
  if (codeword & (1 << 24ull)) *speed_adv |= BF_ADV_SPD_50GBASE_KR2_CONSORTIUM;
  if (codeword & (1 << 25ull)) *speed_adv |= BF_ADV_SPD_50GBASE_CR2_CONSORTIUM;

  if (codeword & (1ull << 40ull)) *fec_adv |= BF_ADV_FEC_RS_ABILITY_CONSORTIUM;
  if (codeword & (1ull << 41ull)) *fec_adv |= BF_ADV_FEC_FC_ABILITY_CONSORTIUM;
  if (codeword & (1ull << 42ull)) *fec_adv |= BF_ADV_FEC_RS_REQUEST_CONSORTIUM;
  if (codeword & (1ull << 43ull)) *fec_adv |= BF_ADV_FEC_FC_REQUEST_CONSORTIUM;

  return BF_SUCCESS;
}

void bf_an_base_page_log(bf_dev_id_t dev_id,
                         int ring,
                         int sd,
                         uint64_t base_pg) {
  port_mgr_log("%d:%d:%3d:            raw: %04x_%04x_%04x",
               dev_id,
               ring,
               sd,
               (unsigned int)((base_pg >> 32ull) & 0xffff),
               (unsigned int)((base_pg >> 16ull) & 0xffff),
               (unsigned int)((base_pg >> 0ull) & 0xffff));

  if (base_pg & (0x7FFull << 21ull)) {
    port_mgr_log("%d:%d:%3d:       AN speed: %s%s%s%s%s%s%s%s%s%s%s",
                 dev_id,
                 ring,
                 sd,
                 (base_pg & (1ull << 21ull)) ? "1000_KX " : "",
                 (base_pg & (1ull << 22ull)) ? "10G_KX4 " : "",
                 (base_pg & (1ull << 23ull)) ? "10G_KR " : "",
                 (base_pg & (1ull << 24ull)) ? "40G_KR4 " : "",
                 (base_pg & (1ull << 25ull)) ? "40G_CR4 " : "",
                 (base_pg & (1ull << 26ull)) ? "100G_CR10 " : "",
                 (base_pg & (1ull << 27ull)) ? "40G_KP4 " : "",
                 (base_pg & (1ull << 28ull)) ? "100G_KR4 " : "",
                 (base_pg & (1ull << 29ull)) ? "100G_CR4 " : "",
                 (base_pg & (1ull << 30ull)) ? "25G_KRS_CRS " : "",
                 (base_pg & (1ull << 31ull)) ? "25G_KR_CR " : "");
  }

  if (base_pg & (0xFull << 44ull)) {
    port_mgr_log("%d:%d:%3d:       AN   fec: %s%s%s%s",
                 dev_id,
                 ring,
                 sd,
                 (base_pg & (1ull << 44ull)) ? "f2 (25G-RS FEC REQUEST) " : "",
                 (base_pg & (1ull << 45ull)) ? "f3 (25G-FC FEC REQUEST) " : "",
                 (base_pg & (1ull << 46ull)) ? "f0 (10G-FC FEC ABILITY) " : "",
                 (base_pg & (1ull << 47ull)) ? "f1 (10G-FC FEC REQUEST) " : "");
  }
  if (base_pg & (3ull << 10ull)) {
    port_mgr_log("%d:%d:%3d:       AN  pause: %s%s",
                 dev_id,
                 ring,
                 sd,
                 (base_pg & (1ull << 10ull)) ? "c0 (RX) " : "",
                 (base_pg & (1ull << 11ull)) ? "c1 (TX) " : "");
  }
}

/** \brief Get the clause 73 Autonegotiation base and next page
 *         advertisements on a Tofino port.
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param num_pgs : number of uint64_t entries pointed to by pgs
 * \param pgs     : pointer to "num_pgs" next page values.
 *                : lower 48 bits are base/next page advertisement(s)
 *                : pgs[0] contains the base pg advertisement
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: advert NULL
 * \return: BF_INVALID_ARG: too many next pages for port_mgr_port_t to
 *                        : hold
 *
 */
bf_status_t bf_port_autoneg_advert_get(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port,
                                       uint32_t *num_pgs,
                                       uint64_t *pgs) {
  uint32_t i;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (pgs == NULL) return BF_INVALID_ARG;

  *num_pgs = port_p->an_num_next_pages + 1;  // always +1 for the base pg
  pgs[0] = port_p->an_base_page;
  for (i = 0; i < port_p->an_num_next_pages; i++) {
    pgs[i + 1] = port_p->an_next_page[i];
  }
  return BF_SUCCESS;
}

/** \brief Set the clause 73 Autonegotiation base and next page
 *         advertisements on a Tofino port.
 *
 * [ PRE_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param num_pgs : number of uint64_t entries pointed to by pgs
 * \param pgs     : pointer to "num_pgs" next page values.
 *                : lower 48 bits are base/next page advertisement(s)
 *                : pgs[0] contains the base pg advertisement
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: advert NULL
 * \return: BF_INVALID_ARG: too many next pages for port_mgr_port_t to
 *                        : hold
 *
 */
bf_status_t bf_port_autoneg_advert_set(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port,
                                       uint32_t num_pgs,
                                       uint64_t *pgs) {
  uint32_t i;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (pgs == NULL) return BF_INVALID_ARG;
  if (num_pgs > BF_MAX_AN_NEXT_PAGES) return BF_INVALID_ARG;

  port_p->an_base_page = pgs[0];
  port_p->an_num_next_pages = (num_pgs > 1) ? (num_pgs - 1) : 0;
  // clear any previous value in next pages
  memset(port_p->an_next_page, 0, sizeof(port_p->an_next_page));
  for (i = 0; i < port_p->an_num_next_pages; i++) {
    port_p->an_next_page[i] = pgs[i + 1];
  }
  return BF_SUCCESS;
}

/** \brief Returns BF_SUCCESS if port is still defined (added)
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 *
 * \return: BF_SUCCESS,          port is valid and in-use
 * \return: BF_INVALID_ARG:      port is not-valid
 * \return: BF_OBJECT_NOT_FOUND: port is out-of-range
 */
bf_status_t bf_port_is_valid(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  if (!DEV_PORT_VALIDATE(dev_port)) return BF_INVALID_ARG;

  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_OBJECT_NOT_FOUND;
  return BF_SUCCESS;
}

/** \brief Returns BF_SUCCESS if port is admin enabled
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 *
 * \return: BF_SUCCESS,     port is admin enabled
 * \return: BF_INVALID_ARG: port out-of-range or not-valid or disabled
 */
bf_status_t bf_port_is_enabled(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  if (!DEV_PORT_VALIDATE(dev_port)) return BF_INVALID_ARG;

  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (!port_p->sw.enabled) return BF_INVALID_ARG;

  return BF_SUCCESS;
}

/** \brief Set reset mode of RS FEC
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param en: bool value indicating reset of RS FEC
 *
 * \return: BF_SUCCESS,     Set enable mode successfully
 * \return: BF_INVALID_ARG: Unable to set mode
 */
bf_status_t bf_port_rs_fec_reset_set(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port,
                                     bool assert_reset) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  bf_status_t bf_status;
  bf_mac_block_id_t mac_block;
  int ch;

  if (port_p == NULL) return BF_INVALID_ARG;

  // should not be called on tof2
  if (!port_mgr_dev_is_tof1(dev_id)) {
    return BF_INVALID_ARG;
  }

  bf_status = bf_port_map_dev_port_to_mac(dev_id, dev_port, &mac_block, &ch);
  if (bf_status == BF_SUCCESS) {
    port_mgr_mac_rs_fec_reset_set(dev_id, mac_block, ch, assert_reset);
  }
  return BF_SUCCESS;
}

/** \brief Set enable mode of RS FEC Scrambler
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param en: bool value indicating enable mode of RS FEC Scrambler
 *
 * \return: BF_SUCCESS,     Set enable mode successfully
 * \return: BF_INVALID_ARG: Unable to set mode
 */
bf_status_t bf_port_rs_fec_scrambler_en_set(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port,
                                            bool en) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  bf_status_t bf_status;
  bf_mac_block_id_t mac_block;
  int ch;

  if (port_p == NULL) return BF_INVALID_ARG;

  // should not be called on tof2
  if (!port_mgr_dev_is_tof1(dev_id)) {
    return BF_INVALID_ARG;
  }

  bf_status = bf_port_map_dev_port_to_mac(dev_id, dev_port, &mac_block, &ch);
  if (bf_status == BF_SUCCESS) {
    port_mgr_mac_rs_fec_scrambler_en_set(dev_id, mac_block, ch, (en ? 1 : 0));
  }
  return BF_SUCCESS;
}

/** \brief Returns BF_SUCCESS if able to get the speed of the port
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 *
 * \return speed  : speed of the valid port
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_speed_get(bf_dev_id_t dev_id,
                              bf_dev_port_t dev_port,
                              bf_port_speed_t *speed) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (speed == NULL) return BF_INVALID_ARG;

  *speed = port_p->sw.speed;
  return BF_SUCCESS;
}

/** \brief Return configured FEC mode
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \return fec    : speed of the valid port
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_fec_get(bf_dev_id_t dev_id,
                            bf_dev_port_t dev_port,
                            bf_fec_type_t *fec) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (fec == NULL) return BF_INVALID_ARG;

  *fec = port_p->sw.fec;
  return BF_SUCCESS;
}

/** \brief Cached the PRBS mode for use when port is enabled
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param prbs_mode:
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_prbs_mode_set(bf_dev_id_t dev_id,
                                  bf_dev_port_t dev_port,
                                  bf_port_prbs_mode_t prbs_mode) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  port_p->sw.prbs_mode = prbs_mode;
  return BF_SUCCESS;
}

/** \brief return the cached the PRBS mode
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param prbs_mode:
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_prbs_mode_get(bf_dev_id_t dev_id,
                                  bf_dev_port_t dev_port,
                                  bf_port_prbs_mode_t *prbs_mode) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  *prbs_mode = port_p->sw.prbs_mode;
  return BF_SUCCESS;
}

/** \brief Setup PRBS test for a port
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param prbs_speed Speed in which PRBS test is to be set (25G or 10G)
 * \param prbs_mode Mode of the PRBS test (31, 23, 15, 13, 11, 9, 7)
 *
 * \return Status of the API call
 */
bf_status_t bf_port_prbs_init(bf_dev_id_t dev_id,
                              bf_dev_port_t dev_port,
                              bf_port_prbs_speed_t prbs_speed,
                              bf_port_prbs_mode_t prbs_mode) {
  int num_lanes = lld_get_chnls_dev_port(dev_id, dev_port);
  int ln = 0;
  bf_status_t sts = BF_SUCCESS;

  if (prbs_speed >= BF_PORT_PRBS_SPEED_MAX) return BF_INVALID_ARG;
  if (prbs_mode >= BF_PORT_PRBS_MODE_MAX) return BF_INVALID_ARG;

  // should not be called on tof2
  if (!port_mgr_dev_is_tof1(dev_id)) {
    return BF_INVALID_ARG;
  }

  sts = bf_serdes_prbs_mode_set(dev_id, prbs_mode);
  if (sts != BF_SUCCESS) {
    return sts;
  }

  for (ln = 0; ln < num_lanes; ln++) {
    sts = bf_serdes_prbs_init(dev_id, dev_port, ln, prbs_speed);
    if (sts != BF_SUCCESS) {
      return sts;
    }
  }
  return BF_SUCCESS;
}

/** \brief Setup Rx Equalization for the prbs test
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 *
 * \return Status of the API call
 */
bf_status_t bf_port_prbs_rx_eq_run(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  int num_lanes = lld_get_chnls_dev_port(dev_id, dev_port);
  bf_status_t sts = BF_SUCCESS;
  int ln = 0;

  // should not be called on tof2
  if (!port_mgr_dev_is_tof1(dev_id)) {
    return BF_INVALID_ARG;
  }

  // Start the ICAL
  for (ln = 0; ln < num_lanes; ln++) {
    sts = bf_serdes_start_dfe_ical_allow_unassigned(dev_id, dev_port, ln);
    if (sts != BF_SUCCESS) {
      return sts;
    }
  }
  return BF_SUCCESS;
}

/** \brief Setup compare mode for prbs
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 *
 * \return Status of the API call
 */
bf_status_t bf_port_prbs_cmp_mode_set(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port) {
  int num_lanes = lld_get_chnls_dev_port(dev_id, dev_port);
  int ln = 0;
  bf_status_t sts = BF_SUCCESS;

  // should not be called on tof2
  if (!port_mgr_dev_is_tof1(dev_id)) {
    return BF_INVALID_ARG;
  }

  for (ln = 0; ln < num_lanes; ln++) {
    sts = bf_serdes_prbs_cmp_mode_set(dev_id, dev_port, ln);
    if (sts != BF_SUCCESS) {
      return sts;
    }
  }

  return BF_SUCCESS;
}

/** \brief Cleanup the setup done for prbs
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 *
 * \return Status of the API call
 */
bf_status_t bf_port_prbs_cleanup(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  int num_lanes = lld_get_chnls_dev_port(dev_id, dev_port);
  int ln = 0;
  bf_status_t sts = BF_SUCCESS;

  // should not be called on tof2
  if (!port_mgr_dev_is_tof1(dev_id)) {
    return BF_INVALID_ARG;
  }

  for (ln = 0; ln < num_lanes; ln++) {
    sts = bf_serdes_prbs_diag_off(dev_id, dev_port, ln);
    if (sts != BF_SUCCESS) {
      return sts;
    }
  }
  return BF_SUCCESS;
}

/** \brief Sets user-specified minimum eye heights at 1e06, 1e10, and 1e12
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 *
 * \return: BF_SUCCESS,     port is valid and in-use
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_eye_quality_set(bf_dev_id_t dev_id,
                                    bf_dev_port_t dev_port,
                                    int qualifying_eye_ht_1e06,
                                    int qualifying_eye_ht_1e10,
                                    int qualifying_eye_ht_1e12) {
  int ln;
  int num_lanes = port_mgr_get_num_lanes(dev_id, dev_port);
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  // should not be called on tof2
  if (!port_mgr_dev_is_tof1(dev_id)) {
    return BF_INVALID_ARG;
  }

  for (ln = 0; ln < num_lanes; ln++) {
    bf_status_t rc;

    rc = bf_serdes_eye_quality_configured_set(dev_id,
                                              dev_port,
                                              ln,
                                              qualifying_eye_ht_1e06,
                                              qualifying_eye_ht_1e10,
                                              qualifying_eye_ht_1e12);
    if (rc != BF_SUCCESS) return BF_INVALID_ARG;
  }
  return BF_SUCCESS;
}

/** \brief Sets previously user-specified minimum eye heights at 1e06, 1e10, and
 *1e12
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 *
 * \return: BF_SUCCESS,     port is valid and in-use
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_eye_quality_reset(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port) {
  int ln;
  int num_lanes = port_mgr_get_num_lanes(dev_id, dev_port);
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  // should not be called on tof2
  if (!port_mgr_dev_is_tof1(dev_id)) {
    return BF_INVALID_ARG;
  }

  for (ln = 0; ln < num_lanes; ln++) {
    bf_status_t rc;

    rc = bf_serdes_eye_quality_reset(dev_id, dev_port, ln);
    if (rc != BF_SUCCESS) return BF_INVALID_ARG;
  }
  return BF_SUCCESS;
}

/** \brief Check if the fec type is valid for a particular speed
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param speed : Speed of the port
 * \param fec   : FEC type to validate
 *
 * \return is_valid : Indicates if the FEC is valid for that speed
 * \return BF_SUCCESS : Arguments are valid by themselves
 * \return BF_INVALID_ARG : Arguments are invalid by themselves
 */
bf_status_t bf_port_fec_type_validate(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      bf_port_speeds_t speed,
                                      bf_fec_type_t fec,
                                      bool *is_valid) {
  int is_cpu_port, err;

  *is_valid = true;
  // validate fec type for speed
  switch (speed) {
    case BF_SPEED_400G:
    case BF_SPEED_200G:
      if (fec != BF_FEC_TYP_REED_SOLOMON) {
        *is_valid = false;
      }
      break;
    case BF_SPEED_100G:
      if ((fec != BF_FEC_TYP_NONE) && (fec != BF_FEC_TYP_REED_SOLOMON)) {
        *is_valid = false;
      }
      break;
    case BF_SPEED_40G:
      if ((fec != BF_FEC_TYP_NONE) && (fec != BF_FEC_TYP_FIRECODE)) {
        *is_valid = false;
      }
      break;
    case BF_SPEED_10G:
      if ((fec != BF_FEC_TYP_NONE) && (fec != BF_FEC_TYP_FIRECODE)) {
        *is_valid = false;
      }
      break;
    case BF_SPEED_25G:
    case BF_SPEED_50G:
    case BF_SPEED_50G_CONS:
    case BF_SPEED_40G_R2:
      break;  // all fec types supported
    case BF_SPEED_1G:
      if (fec != BF_FEC_TYP_NONE) {
        *is_valid = false;
      }
      break;
    default:
      *is_valid = false;
      return BF_INVALID_ARG;  // invalid speed or fec mode
  }
  if (port_mgr_dev_is_tof1(dev_id)) {
    // special check for RS-FEC not supported on TOF1 CPU port
    err = port_mgr_map_dev_port_to_all(
        dev_id, dev_port, NULL, NULL, NULL, NULL, &is_cpu_port);
    if (err != 0) return BF_INVALID_ARG;
    if (is_cpu_port) {
      if ((fec != BF_FEC_TYP_NONE) && (fec != BF_FEC_TYP_FIRECODE)) {
        *is_valid = false;
      }
    }
  }

  return BF_SUCCESS;
}

/** \brief Get the port serdes stats like errors, eye, tx attn etc.
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param q_stats : Structure to hold the stats for all the channels of a MAC
 *blk
 *
 * \return Status of the API call
 */
bf_status_t bf_port_prbs_debug_stats_get(bf_dev_id_t dev_id,
                                         bf_dev_port_t dev_port,
                                         bf_sds_debug_stats_t *stats) {
  bf_status_t sts = BF_SUCCESS;
  uint32_t errors, eye;
  int attn, pre, post;

  if (!stats) return BF_INVALID_ARG;

  // should not be called on tof2
  if (!port_mgr_dev_is_tof1(dev_id)) {
    return BF_INVALID_ARG;
  }

  sts = bf_serdes_tx_drv_attn_get_allow_unassigned(
      dev_id, dev_port, 0, &attn, &post, &pre);
  if (sts != BF_SUCCESS) {
    return sts;
  }
  sts = bf_serdes_debug_stats_get(dev_id, dev_port, 0, &errors, &eye);
  if (sts != BF_SUCCESS) {
    return sts;
  }
  stats->attn_main = attn;
  stats->attn_post = post;
  stats->attn_pre = pre;
  stats->errors = errors;
  stats->eye_metric = eye;
  return BF_SUCCESS;
}

/** \brief Get the port serdes stats like errors, eye, tx attn etc. per quad
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param q_stats : Structure to hold the stats for all the channels of a MAC
 *blk
 *
 * \return Status of the API call
 */
bf_status_t bf_port_prbs_debug_stats_per_quad_get(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    bf_sds_debug_stats_per_quad_t *q_stats) {
  int num_lanes = lld_get_chnls_dev_port(dev_id, dev_port);
  int ln = 0;
  bf_status_t sts = BF_SUCCESS;
  uint32_t errors, eye;
  int attn, pre, post;

  if (!q_stats) return BF_INVALID_ARG;

  // should not be called on tof2
  if (!port_mgr_dev_is_tof1(dev_id)) {
    return BF_INVALID_ARG;
  }

  for (ln = 0; ln < num_lanes; ln++) {
    sts = bf_serdes_tx_drv_attn_get_allow_unassigned(
        dev_id, dev_port, ln, &attn, &post, &pre);
    if (sts != BF_SUCCESS) {
      return sts;
    }
    sts = bf_serdes_debug_stats_get(dev_id, dev_port, ln, &errors, &eye);
    if (sts != BF_SUCCESS) {
      return sts;
    }
    q_stats->chnl_stats[ln].attn_main = attn;
    q_stats->chnl_stats[ln].attn_post = post;
    q_stats->chnl_stats[ln].attn_pre = pre;
    q_stats->chnl_stats[ln].errors = errors;
    q_stats->chnl_stats[ln].eye_metric = eye;
  }
  return BF_SUCCESS;
}

/**\get the sum of ipg and preamble
 *
 */
int bf_port_overhead_len_get(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  port_mgr_port_t *port_p;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return 0;

  return (port_p->sw.preamble_length +
          port_p->sw.ifg);  // should be ipg here, but no one init sw.ipg
}

#ifdef PORT_MGR_HA_UNIT_TESTING
/** \brief Get the corrective action for a port as deemed fit by the port mgr
 *         based on the MAC config
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param corr    : corrective action for the port
 *
 * \return: BF_SUCCESS,     Device is valid
 * \return: BF_INVALID_ARG: Device is invalid
 */
bf_status_t bf_port_mac_corr_action_get(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        bf_ha_corrective_action_t *corr) {
  // port_mgr_dev_t *dev_p =
  // port_mgr_map_dev_id_to_dev_p_allow_unassigned(dev_id);
  port_mgr_ldev_t *dev_p = port_mgr_dev_logical_dev_get(dev_id);
  if (dev_p == NULL) return BF_INVALID_ARG;
  if (corr == NULL) return BF_INVALID_ARG;
  int dev_port_idx = DEV_PORT_TO_BIT_IDX(dev_port);
  if (!BIT_IDX_VALIDATE(dev_port_idx)) return BF_INVALID_ARG;

  bf_ha_port_reconcile_info_per_device_t *mac_recon_info =
      &dev_p->port_mgr_mac_recon_info;

  *corr = mac_recon_info->recon_info_per_port[dev_port_idx].ca;

  return BF_SUCCESS;
}

/** \brief Get the corrective action for a port as deemed fit by the port mgr
 *         based on the Serdes config
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param corr    : corrective action for the port
 *
 * \return: BF_SUCCESS,     Device is valid
 * \return: BF_INVALID_ARG: Device is invalid
 */
bf_status_t bf_port_serdes_corr_action_get(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           bf_ha_corrective_action_t *corr) {
  // port_mgr_dev_t *dev_p =
  // port_mgr_map_dev_id_to_dev_p_allow_unassigned(dev_id);
  port_mgr_ldev_t *dev_p = port_mgr_dev_logical_dev_get(dev_id);
  if (dev_p == NULL) return BF_INVALID_ARG;
  if (corr == NULL) return BF_INVALID_ARG;
  int dev_port_idx = DEV_PORT_TO_BIT_IDX(dev_port);
  if (!BIT_IDX_VALIDATE(dev_port_idx)) return BF_INVALID_ARG;

  bf_ha_port_reconcile_info_per_device_t *serdes_recon_info =
      &dev_p->port_mgr_serdes_recon_info;
  *corr = serdes_recon_info->recon_info_per_port[dev_port_idx].ca;

  return BF_SUCCESS;
}
#endif

/** \brief Returns state of LF and RF interrupt bits
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param lf      : returned: LF state (may be NULL)
 * \param rf      : returned: RF state (may be NULL)
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_lf_rf_get(bf_dev_id_t dev_id,
                              bf_dev_port_t dev_port,
                              bool *latched_lf,
                              bool *latched_rf) {
  bool latched_lf_st = false;
  bool latched_rf_st = false;

  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  // FIXME add sppt
  if (!port_mgr_dev_is_tof1(dev_id)) {
    return BF_INVALID_ARG;
  }

  port_mgr_mac_get_lf_rf_interrupts(
      dev_id, dev_port, &latched_lf_st, &latched_rf_st);
  if (latched_lf) *latched_lf = latched_lf_st;
  if (latched_rf) *latched_rf = latched_rf_st;

  return BF_SUCCESS;
}

/** \brief Disable link-training on a port
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param disable : true iff you want to DISABLE link-training (non-default)
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_lt_disable_set(bf_dev_id_t dev_id,
                                   bf_dev_port_t dev_port,
                                   bool disable) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  port_p->lt_disabled = disable;

  return BF_SUCCESS;
}

/** \brief Enable or disable LOCAL FAULT interrupts on a port
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param en      : true=enable LF interrupts
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_local_fault_int_set(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        bool en) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof1(dev_id)) {
    port_mgr_mac_local_fault_int_set(dev_id, dev_port, en);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    port_mgr_tof2_port_local_fault_int_en_set(dev_id, dev_port, en);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    return BF_INVALID_ARG;
  } else {
    return BF_INVALID_ARG;
  }
  return BF_SUCCESS;
}

/** \brief Enable or disable REMOTE FAULT interrupts on a port
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param en      : true=enable RF interrupts
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_remote_fault_int_set(bf_dev_id_t dev_id,
                                         bf_dev_port_t dev_port,
                                         bool en) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof1(dev_id)) {
    port_mgr_mac_remote_fault_int_set(dev_id, dev_port, en);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    port_mgr_tof2_port_remote_fault_int_en_set(dev_id, dev_port, en);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    port_mgr_tof3_port_remote_fault_int_en_set(dev_id, dev_port, en);
  } else {
    return BF_INVALID_ARG;
  }
  return BF_SUCCESS;
}

/** \brief Enable or disable Link gainT interrupts on a port
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param en      : true=enable link gain interrupts
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_link_gain_int_set(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      bool en) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof2(dev_id)) {
    port_mgr_tof2_port_link_gain_int_en_set(dev_id, dev_port, en);
  } else {
    return BF_NOT_SUPPORTED;
  }
  return BF_SUCCESS;
}

/** \brief Enable or disable Comira MAC interrupts
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param en      : true=enable Comira MAC interrupts in eth_regs level
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_mac_int_set(bf_dev_id_t dev_id,
                                bf_dev_port_t dev_port,
                                bool en) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof1(dev_id)) {
    port_mgr_mac_int_en_set(dev_id, dev_port, en);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    port_mgr_tof2_port_mac_int_en_set(dev_id, dev_port, en);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    port_mgr_tof3_port_mac_int_en_set(dev_id, dev_port, en);
  } else {
    return BF_INVALID_ARG;
  }

  return BF_SUCCESS;
}

/** \brief Enable interrupt handling policy for the device. This is only
 *         intended to be set ON (en=true) and only once.
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_interrupt_based_link_monitoring_enable(bf_dev_id_t dev_id) {
  port_mgr_ldev_t *dev_p = port_mgr_dev_logical_dev_get(dev_id);
  dev_p->interrupt_based_link_monitoring = true;
  return BF_SUCCESS;
}

/** \brief Disable all Comira MAC interrupts on all mac blocks
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_mac_int_all_disable(bf_dev_id_t dev_id) {
  if (port_mgr_dev_is_tof1(dev_id)) {
    port_mgr_mac_disable_mac_ints(dev_id);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    port_mgr_tof2_umac_dis_all_set(dev_id);
  } else {
    return BF_INVALID_ARG;
  }

  return BF_SUCCESS;
}

/** \brief Return state of link-training disable setting on a port
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param disable : state of "lt disable" setting for port
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_lt_disable_get(bf_dev_id_t dev_id,
                                   bf_dev_port_t dev_port,
                                   bool *disable) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (disable == NULL) return BF_INVALID_ARG;

  *disable = port_p->lt_disabled;

  return BF_SUCCESS;
}

/** \brief Inhibit starting adaptive PCAL on port coming up
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param val     : true=on, false=off
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_no_auto_adaptive_tuning_set(bf_dev_id_t dev_id, bool val) {
  port_mgr_ldev_t *ldev_p = port_mgr_dev_logical_dev_get(dev_id);

  if (ldev_p == NULL) return BF_INVALID_ARG;

  ldev_p->no_auto_adaptive_tuning = val;
  return BF_SUCCESS;
}

/** \brief Return the state of the no_auto_adaptive_tuning flag
 *         for the device
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param val     : ptr to bool (returned state)
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_no_auto_adaptive_tuning_get(bf_dev_id_t dev_id, bool *val) {
  port_mgr_ldev_t *ldev_p = port_mgr_dev_logical_dev_get(dev_id);

  if (ldev_p == NULL) return BF_INVALID_ARG;
  if (val == NULL) return BF_INVALID_ARG;

  *val = ldev_p->no_auto_adaptive_tuning;
  return BF_SUCCESS;
}

/** \brief Indicate this port is connected via an optical transceiver
 *
 * \param dev_id    : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port  : encoded port identifier
 * \param is_optical:
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_is_optical_xcvr_set(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        bool is_optical) {
  port_mgr_port_t *port_p =
      port_mgr_map_dev_port_to_port_allow_unassigned(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  port_p->is_optical = is_optical;

  return BF_SUCCESS;
}

/** \brief Indicate this port is connected via an optical transceiver
 *
 * \param dev_id    : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port  : encoded port identifier
 * \param is_optical: ptr to bool (returned state)
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_is_optical_xcvr_get(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        bool *is_optical) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (is_optical == NULL) return BF_INVALID_ARG;

  *is_optical = port_p->is_optical;

  return BF_SUCCESS;
}

/** \brief Indicate optical LOS detected on this port
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 * \param los      : LOS from QSFP FSM
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_optical_los_set(bf_dev_id_t dev_id,
                                    bf_dev_port_t dev_port,
                                    bool los) {
  port_mgr_port_t *port_p =
      port_mgr_map_dev_port_to_port_allow_unassigned(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  port_p->loss_of_optical_signal = los;

  return BF_SUCCESS;
}

/** \brief Return state of optical LOS on this port
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 * \param los      : LOS (returned)
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_optical_los_get(bf_dev_id_t dev_id,
                                    bf_dev_port_t dev_port,
                                    bool *los) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (los == NULL) return BF_INVALID_ARG;

  *los = port_p->loss_of_optical_signal;

  return BF_SUCCESS;
}

/** \brief Indicate this port is connected via an optical transceiver
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 * \param ready    : ready indication from QSFP FSM
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_optical_xcvr_ready_set(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           bool ready) {
  port_mgr_port_t *port_p =
      port_mgr_map_dev_port_to_port_allow_unassigned(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  port_p->optical_xcvr_ready = ready;

  return BF_SUCCESS;
}

/** \brief Indicate this port is connected via an optical transceiver
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 * \param ready    : ready (returned)
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_optical_xcvr_ready_get(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           bool *ready) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (ready == NULL) return BF_INVALID_ARG;

  *ready = port_p->optical_xcvr_ready;

  return BF_SUCCESS;
}

/** \brief Set time_in_state to aid in FSM timeout handling
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 * \param cycles   : # passes in a given state
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_time_in_state_set(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      uint32_t cycles) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  port_p->time_in_state = cycles;
  return BF_SUCCESS;
}

/** \brief Get # cycles spent in same FSM state
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 * \param cycles   : # passes in a given state (returned)
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_time_in_state_get(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      uint32_t *cycles) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;
  if (cycles == NULL) return BF_INVALID_ARG;

  *cycles = port_p->time_in_state;
  return BF_SUCCESS;
}

/** \brief Force serdes sig_ok low
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_force_sig_ok_low_set(bf_dev_id_t dev_id,
                                         bf_dev_port_t dev_port) {
  bool is_sw_model = false;
  int num_lanes = port_mgr_get_num_lanes(dev_id, dev_port);
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  bf_drv_device_type_get(dev_id, &is_sw_model);
  if (is_sw_model) {
    /* this API depends on real data returned from the MAC
     * which is not simlated in the sw_model
     */
    return BF_SUCCESS;
  }

  if (port_mgr_dev_is_tof1(dev_id)) {
    port_mgr_mac_sigovrd_set(dev_id, dev_port, num_lanes, BF_SIGOVRD_FORCE_LO);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    port_mgr_tof2_port_sigovrd_set(
        dev_id, dev_port, num_lanes, BF_SIGOVRD_FORCE_LO);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    return BF_INVALID_ARG;
  } else {
    return BF_INVALID_ARG;
  }
  return BF_SUCCESS;
}

/** \brief Allow actual serdes sig_ok indications thru to MAC
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_un_force_sig_ok_low_set(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port) {
  bool is_sw_model = false;
  int num_lanes = port_mgr_get_num_lanes(dev_id, dev_port);
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  bf_drv_device_type_get(dev_id, &is_sw_model);
  if (is_sw_model) {
    /* this API depends on real data returned from the MAC
     * which is not simlated in the sw_model
     */
    return BF_SUCCESS;
  }
  if (port_mgr_dev_is_tof1(dev_id)) {
    port_mgr_mac_sigovrd_set(dev_id, dev_port, num_lanes, BF_SIGOVRD_PASS_THRU);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    port_mgr_tof2_port_sigovrd_set(
        dev_id, dev_port, num_lanes, BF_SIGOVRD_PASS_THRU);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    return BF_INVALID_ARG;
  } else {
    return BF_INVALID_ARG;
  }
  return BF_SUCCESS;
}

/** \brief Force serdes sig_ok high
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_force_sig_ok_high_set(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port) {
  bool is_sw_model = false;
  int num_lanes = port_mgr_get_num_lanes(dev_id, dev_port);
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  bf_drv_device_type_get(dev_id, &is_sw_model);
  if (is_sw_model) {
    /* this API depends on real data returned from the MAC
     * which is not simlated in the sw_model
     */
    return BF_SUCCESS;
  }

  if (port_mgr_dev_is_tof1(dev_id)) {
    port_mgr_mac_sigovrd_set(dev_id, dev_port, num_lanes, BF_SIGOVRD_FORCE_HI);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    port_mgr_tof2_port_sigovrd_set(
        dev_id, dev_port, num_lanes, BF_SIGOVRD_FORCE_HI);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    return BF_INVALID_ARG;
  } else {
    return BF_INVALID_ARG;
  }
  return BF_SUCCESS;
}

/** \brief Allow actual serdes sig_ok indications thru to MAC
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_un_force_sig_ok_high_set(bf_dev_id_t dev_id,
                                             bf_dev_port_t dev_port) {
  bool is_sw_model = false;
  int num_lanes = port_mgr_get_num_lanes(dev_id, dev_port);
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  bf_drv_device_type_get(dev_id, &is_sw_model);
  if (is_sw_model) {
    /* this API depends on real data returned from the MAC
     * which is not simlated in the sw_model
     */
    return BF_SUCCESS;
  }

  if (port_mgr_dev_is_tof1(dev_id)) {
    port_mgr_mac_sigovrd_set(dev_id, dev_port, num_lanes, BF_SIGOVRD_PASS_THRU);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    port_mgr_tof2_port_sigovrd_set(
        dev_id, dev_port, num_lanes, BF_SIGOVRD_PASS_THRU);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    return BF_INVALID_ARG;
  } else {
    return BF_INVALID_ARG;
  }
  return BF_SUCCESS;
}

/** \brief Set MAC to ignore link faults from the RS. This is a debug mode
 *
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 * \param en       : set (1) or clear (0)
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_tx_ignore_rx_set(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port,
                                     bool en) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof1(dev_id)) {
    port_mgr_port_tx_ignore_rx_set(dev_id, dev_port, en);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    port_mgr_tof2_port_tx_ignore_rx_set(dev_id, dev_port, en);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    return BF_INVALID_ARG;
  } else {
    return BF_INVALID_ARG;
  }
  return BF_SUCCESS;
}

/** \brief Reset Rx path of a port (PCS/FEC)
 *
 * \param dev_id   : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port : encoded port identifier
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_rx_path_reset(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  bf_port_force_sig_ok_low_set(dev_id, dev_port);
  bf_port_un_force_sig_ok_low_set(dev_id, dev_port);

  return BF_SUCCESS;
}

/** \brief Set maximum number of PCS errored blocks allowed on a 10/25G non-FEC
 *         port w/out taking it down. 0=no limit
 *
 * \param dev_id     : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param max_errors : max number of errors.
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_errored_block_thresh_set(bf_dev_id_t dev_id,
                                             bf_dev_port_t dev_port,
                                             uint32_t max_errors) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;
  port_p->sw.single_lane_error_thresh = max_errors;
  return BF_SUCCESS;
}

/** \brief Get maximum number of PCS errors allowed on a 10G non-FEC port w/out
 *         taking it down. 0=no limit
 *
 * \param dev_id     : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param max_errors : pointer to a calloer allocated variable where this
 *                     function will return the max number of errors.
 *
 * \return: BF_SUCCESS,     port is valid
 * \return: BF_INVALID_ARG: port not valid
 */
bf_status_t bf_port_errored_block_thresh_get(bf_dev_id_t dev_id,
                                             bf_dev_port_t dev_port,
                                             uint32_t *max_errors) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;
  if (max_errors == NULL) return BF_INVALID_ARG;

  *max_errors = port_p->sw.single_lane_error_thresh;
  return BF_SUCCESS;
}

/** \brief Read MAC stats from hardware registers directly
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param ctr_data: caller supplied buffer
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG : dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG : invalid or missing bf_dev_port_t
 * \return: BF_NOT_READY   : MAC stat request outstanding (try again later)
 * \return: BF_HW_COMM_FAIL: Read failed
 *
 */
bf_status_t bf_port_mac_stats_hw_direct_get(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port,
                                            bf_rmon_counter_t *ctr_type_array,
                                            uint64_t *ctr_data,
                                            uint32_t num_of_ctr) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  uint32_t ctr_id;
  uint64_t data64;

  if (port_p == NULL) return BF_INVALID_ARG;
  if (ctr_data == NULL) return BF_INVALID_ARG;
  if (ctr_type_array == NULL) return BF_INVALID_ARG;
  if (num_of_ctr >= BF_NUM_RMON_COUNTERS) return BF_INVALID_ARG;

  // disallow simultaneous access
  bf_port_mac_stats_mtx_lock(dev_id, dev_port);

  // read counters into user space
  for (ctr_id = 0; ctr_id <= num_of_ctr; ctr_id++) {
    int ctr_rd_failed = 1, tries = 0, max_attempts = 3;

    while (ctr_rd_failed && (tries++ < max_attempts)) {
      ctr_rd_failed = port_mgr_port_read_counter(
          dev_id, dev_port, ctr_type_array[ctr_id], &data64);
    }
    if (ctr_rd_failed) {
      bf_port_mac_stats_mtx_unlock(dev_id, dev_port);
      return BF_HW_COMM_FAIL;
    }
    ctr_data[ctr_id] = data64;
  }

  bf_port_mac_stats_mtx_unlock(dev_id, dev_port);

  return BF_SUCCESS;
}

/** \brief Force MAC link-up in rx direction on a Tofino port
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param enable  : true=forces link-up in rx-direction, false=dont-force
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 *
 */
bf_status_t bf_port_mac_set_tx_mode(bf_dev_id_t dev_id,
                                    bf_dev_port_t dev_port,
                                    bool enable) {
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  int is_cpu_port = 0;

  if (port_p == NULL) return BF_INVALID_ARG;

  bf_sys_mutex_lock(&port_p->port_mtx);

  if (port_mgr_dev_is_tof2(dev_id)) {
    port_mgr_map_dev_port_to_all(
        dev_id, dev_port, NULL, NULL, NULL, NULL, &is_cpu_port);
    if (enable) {
      port_mgr_tof2_port_tx_ignore_rx_set(dev_id, dev_port, true);
    } else {
      port_mgr_tof2_port_tx_ignore_rx_set(dev_id, dev_port, false);
    }
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    port_mgr_tof3_ignore_fault(dev_id, dev_port, enable);
    return BF_SUCCESS;
  } else {
    if (enable) {
      // PCS loopback must be already disabled before the call to enable it:
      // this allows recovery from situations where the software flag and the HW
      // control bit are out of sync.
      port_mgr_mac_force_pcs_near_loopback(dev_id, dev_port, false);
    }

    port_mgr_mac_force_pcs_near_loopback(dev_id, dev_port, enable);
  }

  bf_sys_mutex_unlock(&port_p->port_mtx);
  return BF_SUCCESS;
}

/** \brief Set Transmit and Receive lane mapping in a port
 *
 * Configures the port (quad lane) based lane mapper to map physical lanes
 * to logical lanes.
 *
 * \param[in]  dev_id            : System-assigned identifier
 *(0..BFN_MAX_ASICS-1)
 * \param[in]  mac_id            : MAC_ID
 * \param[in]  tx_lane[4 or 8]   : TX physical lane # (0-3/7) for logical lane
 *0-3/7
 * \param[in]  rx_lane[4 or 8]   : RX physical lane # (0-3/7) for logical lane
 *0-3/7
 *
 */
bf_status_t bf_port_lane_map_set(bf_dev_id_t dev_id,
                                 bf_mac_block_id_t mac_id,
                                 bf_mac_block_lane_map_t *lane_map) {
  uint32_t ch;

  // add front-port info to all port structs that belong to the same MAC
  // 4 channels for tof1, 8 channels for tof2
  if (port_mgr_dev_is_tof1(dev_id)) {
    for (ch = 0; ch < BF_TOF_MAC_BLOCK_CHANNELS; ch++) {
      port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port_allow_unassigned(
          dev_id, lane_map->dev_port + ch);
      if (port_p == NULL) continue;

      port_p->fp_conn_id = lane_map->fp_conn_id;
      port_p->fp_chnl_id = lane_map->fp_chnl_id;
    }
  } else {
    for (ch = 0; ch < BF_TOF2_MAC_BLOCK_CHANNELS; ch++) {
      port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port_allow_unassigned(
          dev_id, lane_map->dev_port + ch);
      if (port_p == NULL) continue;  // only 4 channels on cpu port

      port_p->fp_conn_id = lane_map->fp_conn_id;
      port_p->fp_chnl_id = lane_map->fp_chnl_id;
    }
  }

  if (port_mgr_dev_is_tof1(dev_id)) {
    return port_mgr_tof1_serdes_lane_map_set(dev_id, mac_id, lane_map);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    return port_mgr_tof2_port_lane_map_set(
        dev_id, lane_map->dev_port, lane_map->tx_lane, lane_map->rx_lane);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    return port_mgr_tof3_port_lane_map_set(
        dev_id, lane_map->dev_port, lane_map->tx_lane, lane_map->rx_lane);
  }
  return BF_INVALID_ARG;
}

/** \brief Get Transmit and Receive lane mapping in a port
 *
 * Gets the port (quad lane) based lane mapper config.
 *
 * \param[in]  dev_id       : System-assigned identifier (0..BFN_MAX_ASICS-1)
 * \param[in]  port         : Physical port # on dev_id (0..LLD_MAX_PORTS-1)
 * \param[out] tx_lane[4]   : TX physical lane # (0-3) for logical lane 0-3
 * \param[out] rx_lane[4]   : RX physical lane # (0-3) for logical lane 0-3
 *
 */
bf_status_t bf_port_lane_map_get(bf_dev_id_t dev_id,
                                 bf_mac_block_id_t mac_id,
                                 bf_mac_block_lane_map_t *lane_map) {
  if (port_mgr_dev_is_tof1(dev_id)) {
    return port_mgr_tof1_serdes_lane_map_get(dev_id, mac_id, lane_map);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    return port_mgr_tof2_serdes_lane_map_get(
        dev_id, lane_map->dev_port, lane_map->tx_lane, lane_map->rx_lane);
  } else {
    return port_mgr_tof3_port_lane_map_get(
        dev_id, lane_map->dev_port, lane_map->tx_lane, lane_map->rx_lane);
  }
  return BF_SUCCESS;
}

/** \brief Return string representing passed bf_port_speed_t
 *
 * [ POST_ENABLE ]
 *
 * \param speed  : to convert
 *
 */
bf_status_t bf_port_speed_to_str(bf_port_speed_t speed, char **speed_str) {
  if (speed_str == NULL) return BF_INVALID_ARG;

  switch (speed) {
    case BF_SPEED_400G:
      *speed_str = "400g";
      break;
    case BF_SPEED_200G:
      *speed_str = "200g";
      break;
    case BF_SPEED_100G:
      *speed_str = "100g";
      break;
    case BF_SPEED_50G:
    case BF_SPEED_50G_CONS:
      *speed_str = "50g";
      break;
    case BF_SPEED_40G:
      *speed_str = "40g";
      break;
    case BF_SPEED_40G_R2:
      *speed_str = "40g-r2";
      break;
    case BF_SPEED_25G:
      *speed_str = "25g";
      break;
    case BF_SPEED_10G:
      *speed_str = "10g";
      break;
    case BF_SPEED_1G:
      *speed_str = "1g";
      break;
    default:
      *speed_str = "invalid";
      return BF_INVALID_ARG;
  }
  return BF_SUCCESS;
}

/** \brief Return the operational state (up/down) of a Tofino port and
 *  indicate if we need to issue callbacks to the registered modules later
 *  Interrupt monitoring check is skipped here.
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param state   : 0=down, 1=up
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: state (returned value) NULL
 */
bf_status_t bf_port_oper_state_get_skip_intr_check(bf_dev_id_t dev_id,
                                                   bf_dev_port_t dev_port,
                                                   int *state) {
  int st;
  bf_status_t sts;
  port_mgr_dev_t *dev_p = NULL;
  port_mgr_port_t *port_p = NULL;

  dev_p = port_mgr_map_dev_id_to_dev_p(dev_id);
  if (dev_p == NULL) return BF_INVALID_ARG;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;
  if (state == NULL) return BF_INVALID_ARG;

  sts = bf_port_oper_state_get_internal(dev_id, dev_port, &st);
  if (sts != BF_SUCCESS) {
    port_mgr_log("%d:%3d: Unable to get the oper state for port : %s (%d)",
                 dev_id,
                 dev_port,
                 bf_err_str(sts),
                 sts);
    return sts;
  }

  if ((port_p->sw.speed == BF_SPEED_25G || port_p->sw.speed == BF_SPEED_50G) &&
      (port_p->fec_reset_inp)) {
    port_mgr_log(
        "%d:%3d: FEC reset in-progress (2). Hold dn", dev_id, dev_port);
    st = 0;  // keep link-down
  }

  // Set the flag to indicate that we need to issue callbacks
  if (port_p->sw.oper_state != st) {
    port_p->issue_callbacks = true;
  }
  port_p->sw.oper_state = st;
  *state = st;
  return BF_SUCCESS;
}

/** \brief Issue callbacks to the registered modules if needed
 *   If interrupt is enabled, only link-up action is handled.
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 */
bf_status_t bf_port_oper_state_callbacks_issue_with_intr_check(
    bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  port_mgr_dev_t *dev_p = NULL;
  port_mgr_port_t *port_p = NULL;

  dev_p = port_mgr_map_dev_id_to_dev_p(dev_id);
  if (dev_p == NULL) return BF_INVALID_ARG;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_p->issue_callbacks) {
    if (port_p->sw.oper_state) {
      port_mgr_log("%d:%3d: --- Up", dev_id, dev_port);
      port_mgr_link_up_actions(dev_id, dev_port);
    } else {
      if (dev_p->ldev.interrupt_based_link_monitoring) {
        if (!port_p->force_link_down_cb) {
          /*  if we want to monitor linkdown
           *  status over interrupt, just return here
           */
          return BF_SUCCESS;
        }
        port_mgr_log("%d:%3d: --- down in interrupt-mode", dev_id, dev_port);
      }
      port_mgr_log("%d:%3d: --- Dn", dev_id, dev_port);
      port_mgr_link_dn_actions(dev_id, dev_port);
    }
    port_p->issue_callbacks = false;
  }
  return BF_SUCCESS;
}

bf_status_t bf_port_intr_oper_state_callbacks_issue(bf_dev_id_t dev_id,
                                                    bf_dev_port_t dev_port) {
  port_mgr_dev_t *dev_p = NULL;
  port_mgr_port_t *port_p = NULL;

  dev_p = port_mgr_map_dev_id_to_dev_p(dev_id);
  if (dev_p == NULL) return BF_INVALID_ARG;

  if (!dev_p->ldev.interrupt_based_link_monitoring) {
    return BF_SUCCESS;
  }

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_p->issue_callbacks) {
    if (!port_p->sw.oper_state) {
      port_mgr_log("%d:%3d: --- Dn", dev_id, dev_port);
      port_mgr_link_dn_actions(dev_id, dev_port);
      port_p->issue_callbacks = false;
    }
  }
  return BF_SUCCESS;
}

/** \brief Return the operational state (up/down) of a serdes los
 *   Serdes los is a sticky bit, in order to clear previous state, this
 *   function should be called twice and ignore the value returned by
 *   the first call.
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param state   : 0=down, 1=up
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG: invalid or missing bf_dev_port_t
 * \return: BF_INVALID_ARG: state (returned value) NULL
 */
bf_status_t bf_port_serdes_los_get(bf_dev_id_t dev_id,
                                   bf_dev_port_t dev_port,
                                   int *state) {
  int st = 0;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  bool is_sw_model = false;

  if (port_p == NULL) return BF_INVALID_ARG;
  if (state == NULL) return BF_INVALID_ARG;

  bf_drv_device_type_get(dev_id, &is_sw_model);
  if (is_sw_model) {
    *state = st;
    return BF_SUCCESS;
  }

  // FIXME add sppt
  if (port_mgr_dev_is_tof2(dev_id)) {
    return BF_INVALID_ARG;
  }

  int ln, num_lanes = port_mgr_get_num_lanes(dev_id, dev_port);
  bool rx_los = false;

  for (ln = 0; ln < num_lanes; ln++) {
    bf_status_t rc = bf_serdes_rx_afe_los_get(dev_id, dev_port, ln, &rx_los);
    if (rc == BF_SUCCESS) {
      if (rx_los) {
        port_mgr_log("%d:%3d: ln:%d LOS=1 Port Dn", dev_id, dev_port, ln);
        st = 0;
        break;
      } else {
        st = 1;
      }
    }
  }
  *state = st;
  return BF_SUCCESS;
}

const char *lb_mode_str[] = {"  NONE  ",
                             "Mac-near",
                             "Mac-far ",
                             "Pcs-near",
                             "Ser-near",
                             "Ser-far ",
                             "Pipe    "};

const char *get_loopback_mode_str(bf_loopback_mode_e mode) {
  return lb_mode_str[mode];
}

/** \brief configure clkobs pad on the ASIC
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param pad: BF_CLKOBS_PAD_0 or BF_CLKOBS_PAD_1
 * \param clk_src: BF_SDS_NONE_CLK or BF_SDS_RX_RECOVEREDCLK or BF_SDS_TX_CLK
 * \param divider: 0,1,2, 3 for div by 1,2,3,8, respectively
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 * \return: BF_HW_UPDATE_FAILED: failure to apply config
 */
bf_status_t bf_port_clkobs_set(bf_dev_id_t dev_id,
                               bf_dev_port_t dev_port,
                               bf_clkobs_pad_t pad,
                               bf_sds_clkobs_clksel_t clk_src,
                               int divider) {
  int daisy_sel, lane;
  bf_mac_block_id_t mac_block;

  if (dev_id >= BF_MAX_DEV_COUNT) {
    return BF_INVALID_ARG;
  }
  if (divider < 0 || divider > 3) {
    return BF_INVALID_ARG;
  }
  if (bf_port_map_dev_port_to_mac(dev_id, dev_port, &mac_block, &lane) !=
      BF_SUCCESS) {
    return BF_INVALID_ARG;
  }
  if (port_mgr_dev_is_tof1(dev_id)) {
    if (mac_block >= 8 && mac_block <= 39) {
      daisy_sel = 1;
    } else {
      daisy_sel = 0;
    }
    /* ** TBD ** unselect other MACs from driving the selected daisy chain */
    if (bf_serdes_clkobs_clksel_set(dev_id, dev_port, pad, clk_src) !=
        BF_SUCCESS) {
      return BF_HW_UPDATE_FAILED;
    }
    if (bf_serdes_clkobs_div_set(dev_id, pad, divider, daisy_sel) !=
        BF_SUCCESS) {
      return BF_HW_UPDATE_FAILED;
    }
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    if (mac_block >= 9 && mac_block <= 24) {
      daisy_sel = 1;
    } else {
      daisy_sel = 0;
    }
    if (bf_tof2_serdes_clkobs_set(
            dev_id, dev_port, pad, clk_src, divider, daisy_sel) != BF_SUCCESS) {
      return BF_HW_UPDATE_FAILED;
    }
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    if (bf_tof3_serdes_clkobs_set(dev_id, dev_port, pad, clk_src, divider) !=
        BF_SUCCESS) {
      return BF_HW_UPDATE_FAILED;
    }
  }

  return BF_SUCCESS;
}

/** \brief clkobs drive strength config (tofino2 only)
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param drive_strength: Clock observation pad drive strength. (0 ~ 15)
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_port_clkobs_drive_strength_set(bf_dev_id_t dev_id,
                                              int drive_strength) {
  return bf_tof2_clkobs_drive_strength_set(dev_id, drive_strength);
}

/** \brief Determine serdes encoding mode based on speed and number of lanes
 *
 * \param speed
 * \param n_lanes
 * \param enc_mode
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_serdes_encoding_mode_get(bf_port_speed_t speed,
                                        uint32_t n_lanes,
                                        bf_serdes_encoding_mode_t *enc_mode) {
  *enc_mode = BF_SERDES_ENC_MODE_NONE;

  switch (speed) {
    case BF_SPEED_800G:
    case BF_SPEED_400G:
      *enc_mode = BF_SERDES_ENC_MODE_PAM4;
      return BF_SUCCESS;
      break;
    case BF_SPEED_200G:
      if (n_lanes == 8) {
        *enc_mode = BF_SERDES_ENC_MODE_NRZ;
        return BF_SUCCESS;
      } else if (n_lanes == 4) {
        *enc_mode = BF_SERDES_ENC_MODE_PAM4;
        return BF_SUCCESS;
      }
      break;
    case BF_SPEED_100G:
      if (n_lanes == 2) {
        *enc_mode = BF_SERDES_ENC_MODE_PAM4;
        return BF_SUCCESS;
      } else if (n_lanes == 4) {
        *enc_mode = BF_SERDES_ENC_MODE_NRZ;
        return BF_SUCCESS;
      }
      break;
    case BF_SPEED_50G:
      if (n_lanes == 1) {
        *enc_mode = BF_SERDES_ENC_MODE_PAM4;
        return BF_SUCCESS;
      } else if (n_lanes == 2) {
        *enc_mode = BF_SERDES_ENC_MODE_NRZ;
        return BF_SUCCESS;
      }
      break;
    case BF_SPEED_50G_CONS:
    case BF_SPEED_40G_R2:
    case BF_SPEED_40G:
    case BF_SPEED_25G:
    case BF_SPEED_10G:
    case BF_SPEED_1G:
      *enc_mode = BF_SERDES_ENC_MODE_NRZ;
      return BF_SUCCESS;
    case BF_SPEED_NONE:
      return BF_SUCCESS;
  }
  return BF_INVALID_ARG;
}

/** \brief Get and save autoneg base and next pages
 *
 * \param dev_id
 * \param dev_port
 * \param enc_mode
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_serdes_an_lp_base_page_get(bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port,
                                          uint64_t *lp_base_page) {
  port_mgr_port_t *port_p = NULL;
  bf_status_t rc = BF_SUCCESS;
  uint64_t lp_base_pg = 0;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof1(dev_id)) {
    rc = bf_serdes_autoneg_lp_base_pg_get(dev_id, dev_port, 0, &lp_base_pg);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    rc = bf_tof2_serdes_an_lp_base_page_get(dev_id, dev_port, &lp_base_pg);
  } else {
    // not supported for Tof3 yet
    return BF_INVALID_ARG;
  }
  if (rc == BF_SUCCESS) {
    port_p->an_lp_base_page = lp_base_pg;

    if (lp_base_page) {
      *lp_base_page = lp_base_pg;
    }

    port_mgr_log("%d:%d:0: AN LP BASE PAGE: %04x_%04x_%04x",
                 dev_id,
                 dev_port,
                 (uint32_t)((lp_base_pg >> 32ull) & 0xFFFFull),
                 (uint32_t)((lp_base_pg >> 16ull) & 0xFFFFull),
                 (uint32_t)(lp_base_pg & 0xFFFFull));
  } else {
    port_mgr_log("%d:%d:0: AN Error getting LP BASE PAGE", dev_id, dev_port);
  }
  return rc;
}

/** \brief Get and save autoneg base and next pages
 *
 * \param dev_id
 * \param dev_port
 * \param lp_base_pag
 * \param lp_nxt_page1
 * \param lp_nxt_page2
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_serdes_an_lp_pages_get(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      uint64_t *lp_base_page,
                                      uint64_t *lp_nxt_page1,
                                      uint64_t *lp_nxt_page2) {
  port_mgr_port_t *port_p = NULL;
  bf_status_t rc = BF_SUCCESS;
  uint64_t lp_base_pg = 0;
  uint64_t lp_nxt_pg1 = 0;
  uint64_t lp_nxt_pg2 = 0;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof2(dev_id)) {
    bf_port_autoneg_ext_log_ena(dev_id, dev_port, true);
    rc = bf_tof2_serdes_an_lp_pages_get(
        dev_id, dev_port, &lp_base_pg, &lp_nxt_pg1, &lp_nxt_pg2);
  } else if (port_mgr_dev_is_tof3(dev_id)) {
    if (port_p->an_lp_tmp_base_page) {
      bf_port_autoneg_ext_log_ena(dev_id, dev_port, true);
      lp_base_pg = port_p->an_lp_tmp_base_page;
      lp_nxt_pg1 = port_p->an_lp_tmp_next_page[0];
      lp_nxt_pg2 = port_p->an_lp_tmp_next_page[1];
      port_p->an_lp_tmp_base_page = 0;
    }
  } else {
    // not supported for Tof 1 yet
    return BF_INVALID_ARG;
  }
  if (rc == BF_SUCCESS) {
    port_p->an_lp_base_page = lp_base_pg;
    port_p->an_lp_next_page[0] = lp_nxt_pg1;
    port_p->an_lp_next_page[1] = lp_nxt_pg2;
    port_p->an_lp_num_next_pages = !lp_nxt_pg1 ? 0 : !lp_nxt_pg2 ? 1 : 2;

    port_p->an_lp_next_page[0] = lp_nxt_pg1;

    if (lp_base_page) *lp_base_page = lp_base_pg;
    if (lp_nxt_page1) *lp_nxt_page1 = lp_nxt_pg1;
    if (lp_nxt_page2) *lp_nxt_page2 = lp_nxt_pg2;

    port_mgr_log("%d:%d:0: LP AN BASE PAGE: %04x_%04x_%04x",
                 dev_id,
                 dev_port,
                 (uint32_t)((lp_base_pg >> 32ull) & 0xFFFFull),
                 (uint32_t)((lp_base_pg >> 16ull) & 0xFFFFull),
                 (uint32_t)(lp_base_pg & 0xFFFFull));
    if (lp_nxt_pg1) {
      port_mgr_log("%d:%d:0: LP AN NXT PAGES: %04x_%04x_%04x  %04x_%04x_%04x",
                   dev_id,
                   dev_port,
                   (uint32_t)((lp_nxt_pg1 >> 32ull) & 0xFFFFull),
                   (uint32_t)((lp_nxt_pg1 >> 16ull) & 0xFFFFull),
                   (uint32_t)(lp_nxt_pg1 & 0xFFFFull),
                   (uint32_t)((lp_nxt_pg2 >> 32ull) & 0xFFFFull),
                   (uint32_t)((lp_nxt_pg2 >> 16ull) & 0xFFFFull),
                   (uint32_t)(lp_nxt_pg2 & 0xFFFFull));
    }
  } else {
    port_mgr_log("%d:%d:0: AN Error getting LP AN pages", dev_id, dev_port);
  }
  return rc;
}

/** \brief Get received (from partner) auto-negotiated pages
 *
 * \param dev_id
 * \param dev_port
 * \param page_selector
 * \param link_codeword_p
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_serdes_an_rcv_pages_get(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port,
                                       bf_an_page_sel_t page_selector,
                                       uint64_t *link_codeword_p) {
  port_mgr_port_t *port_p = NULL;
  bf_status_t rc = BF_SUCCESS;

  if (link_codeword_p == NULL) return BF_INVALID_ARG;
  *link_codeword_p = 0;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  switch (page_selector) {
    case BF_AN_PAGE_BASE:
      *link_codeword_p = port_p->an_lp_base_page;
      break;
    case BF_AN_PAGE_NEXT_1:
      *link_codeword_p = port_p->an_lp_next_page[0];
      break;
    case BF_AN_PAGE_NEXT_2:
      *link_codeword_p = port_p->an_lp_next_page[1];
      break;
    default:
      rc = BF_INVALID_ARG;
  }

  return rc;
}

/** \brief Get received (from partner) auto-negotiated pages
 *
 * \param dev_id
 * \param dev_port
 * \param hcd_ndx
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_serdes_an_hcd_index_get(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port,
                                       int *hcd_ndx) {
  port_mgr_port_t *port_p = NULL;

  if (hcd_ndx == NULL) return BF_INVALID_ARG;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  if (!port_p->an_lp_base_page) {
    *hcd_ndx = -1;
  } else {
    *hcd_ndx = port_p->hcd_ndx;
  }

  return BF_SUCCESS;
}

/** \brief Get locally generated pages for auto-negotiation
 *
 * \param dev_id
 * \param dev_port
 * \param page_selector
 * \param link_codeword_p
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_serdes_an_loc_pages_get(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port,
                                       bf_an_page_sel_t page_selector,
                                       uint64_t *link_codeword_p) {
  port_mgr_port_t *port_p = NULL;
  bf_status_t rc = BF_SUCCESS;

  if (link_codeword_p == NULL) return BF_INVALID_ARG;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  switch (page_selector) {
    case BF_AN_PAGE_BASE:
      *link_codeword_p = port_p->an_base_page;
      break;
    case BF_AN_PAGE_NEXT_1:
      *link_codeword_p = port_p->an_next_page[0];
      break;
    case BF_AN_PAGE_NEXT_2:
      *link_codeword_p = port_p->an_next_page[1];
      break;
    default:
      rc = BF_INVALID_ARG;
  }

  return rc;
}

/** \brief Save autoneg received pages for an ongoing negotiation
 *
 * \param dev_id
 * \param dev_port
 * \param page_selector
 * \param lp_link_codeword
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_serdes_an_lp_pages_save(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port,
                                       bf_an_page_sel_t page_selector,
                                       uint64_t lp_link_codeword) {
  port_mgr_port_t *port_p = NULL;
  bf_status_t rc = BF_SUCCESS;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;

  switch (page_selector) {
    case BF_AN_PAGE_BASE:
      // save base page and clear next pages.
      port_p->an_lp_tmp_base_page = lp_link_codeword;
      port_p->an_lp_tmp_num_next_pages = 0;
      port_p->an_lp_tmp_next_page[0] = 0ull;
      port_p->an_lp_tmp_next_page[1] = 0ull;
      if (lp_link_codeword) {
        port_mgr_log("FSM :%d:%3d:-: AN LP Base page  : %04x_%04x_%04x",
                     dev_id,
                     dev_port,
                     (uint32_t)(lp_link_codeword >> 32ull) & 0xffff,
                     (uint32_t)(lp_link_codeword >> 16ull) & 0xffff,
                     (uint32_t)(lp_link_codeword >> 0ull) & 0xffff);
      } else {
        port_mgr_log(
            "FSM :%d:%3d:-: AN LP Base page  : cleared", dev_id, dev_port);
      }

      break;
    case BF_AN_PAGE_NEXT_1:
      if (port_p->an_lp_tmp_num_next_pages == 0) {
        port_p->an_lp_tmp_num_next_pages = 1;
        port_p->an_lp_tmp_next_page[0] = lp_link_codeword;
        port_mgr_log(
            "FSM :%d:%3d:-: AN LP Next page 1: %04x_%04x_%04x "
            "Consortium OUI msg code #5",
            dev_id,
            dev_port,
            (uint32_t)(lp_link_codeword >> 32ull) & 0xffff,
            (uint32_t)(lp_link_codeword >> 16ull) & 0xffff,
            (uint32_t)(lp_link_codeword >> 0ull) & 0xffff);
      } else {
        // Next page out of sequence
        rc = BF_INVALID_ARG;
      }
      break;
    case BF_AN_PAGE_NEXT_2:
      if (port_p->an_lp_tmp_num_next_pages == 1) {
        port_p->an_lp_tmp_num_next_pages = 2;
        port_p->an_lp_tmp_next_page[1] = lp_link_codeword;
        port_mgr_log(
            "FSM :%d:%3d:-: AN LP Next page 2: %04x_%04x_%04x "
            "Consortium 25G/50G advert",
            dev_id,
            dev_port,
            (uint32_t)(lp_link_codeword >> 32ull) & 0xffff,
            (uint32_t)(lp_link_codeword >> 16ull) & 0xffff,
            (uint32_t)(lp_link_codeword >> 0ull) & 0xffff);
      } else {
        // invalid page selector
        rc = BF_INVALID_ARG;
      }
      break;
    default:
      rc = BF_INVALID_ARG;
  }

  return rc;
}

/** \brief Determine serdes encoding mode of a port
 *
 * \param dev_id
 * \param dev_port
 * \param enc_mode
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_port_encoding_mode_get(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      bf_serdes_encoding_mode_t *enc_mode) {
  bf_status_t rc;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  rc = bf_serdes_encoding_mode_get(
      port_p->sw.speed, port_p->sw.n_lanes, enc_mode);
  return rc;
}

/** \brief Return UMAC4 PCS status indicators
 *
 * \param dev_id
 * \param dev_port
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_port_umac4_status_get(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port,
                                     uint64_t *reg64,
                                     uint64_t *txclkpresentall,
                                     uint64_t *rxclkpresentall,
                                     uint64_t *rxsigokall,
                                     uint64_t *blocklockall,
                                     uint64_t *amlockall,
                                     uint64_t *aligned,
                                     uint64_t *nohiber,
                                     uint64_t *nolocalfault,
                                     uint64_t *noremotefault,
                                     uint64_t *linkup,
                                     uint64_t *hiser,
                                     uint64_t *fecdegser,
                                     uint64_t *rxamsf) {
  int umac, ch, err;
  int is_cpu_port;

  err = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch, &is_cpu_port);
  if (err != 0) return BF_INVALID_ARG;
  if (is_cpu_port) return BF_INVALID_ARG;

  return port_mgr_tof2_umac4_status_get(dev_id,
                                        umac,
                                        ch,
                                        reg64,
                                        txclkpresentall,
                                        rxclkpresentall,
                                        rxsigokall,
                                        blocklockall,
                                        amlockall,
                                        aligned,
                                        nohiber,
                                        nolocalfault,
                                        noremotefault,
                                        linkup,
                                        hiser,
                                        fecdegser,
                                        rxamsf);
}

/** \brief Return UMAC4 interrupt bits
 *
 * \param dev_id
 * \param dev_port
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_port_umac4_interrupt_get(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        uint64_t *reg64) {
  int umac, ch, err;
  int is_cpu_port;

  err = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch, &is_cpu_port);
  if (err != 0) return BF_INVALID_ARG;
  if (is_cpu_port) return BF_INVALID_ARG;

  return port_mgr_tof2_umac4_interrupt_get(dev_id, umac, ch, reg64);
}

/** \brief Return UMAC4 interrupt DN/UP bits
 *
 * \param dev_id
 * \param dev_port
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_port_umac4_interrupt_dn_up_get(bf_dev_id_t dev_id,
                                              bf_dev_port_t dev_port,
                                              uint64_t *dn,
                                              uint64_t *up) {
  int umac, ch, err;
  int is_cpu_port;

  err = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch, &is_cpu_port);
  if (err != 0) return BF_INVALID_ARG;
  if (is_cpu_port) return BF_INVALID_ARG;

  return port_mgr_tof2_umac4_interrupt_dn_up_get(dev_id, umac, ch, dn, up);
}

/** \brief Return UMAC3/4 RSigOk force bits
 * (tof2 only)
 *
 * \param dev_id
 * \param dev_port
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_port_forced_sigok_get(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port,
                                     uint32_t *force_hi_raw_val,
                                     uint32_t *force_lo_raw_val,
                                     uint32_t *force_hi,
                                     uint32_t *force_lo) {
  uint32_t n_lanes;
  int umac, ch, err;

  err = port_mgr_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch, NULL);
  if (err != 0) return BF_INVALID_ARG;

  n_lanes = port_mgr_get_num_lanes(dev_id, dev_port);

  port_mgr_tof2_umac_forced_sigok_get(dev_id,
                                      umac,
                                      ch,
                                      n_lanes,
                                      force_hi_raw_val,
                                      force_lo_raw_val,
                                      force_hi,
                                      force_lo);
  return BF_SUCCESS;
}

/** \brief Return link fault status on a Tofino port.
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param link_fault_st: link fault status.
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: invalid dev_id
 * \return: BF_INVALID_ARG: invalid dev_port
 *
 */
bf_status_t bf_port_link_fault_status_get(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    bf_port_link_fault_st_t *link_fault_st) {
  port_mgr_port_t *port_p = NULL;
  port_mgr_dev_t *dev_p = NULL;
  bf_status_t rc = BF_SUCCESS;
  int is_cpu_port = 0;

  if (!link_fault_st) {
    return BF_INVALID_ARG;
  }

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  dev_p = port_mgr_map_dev_id_to_dev_p(dev_id);
  if (port_p == NULL || dev_p == NULL) return BF_INVALID_ARG;

  if (!port_p->sw.enabled) return BF_INVALID_ARG;

  if (port_mgr_dev_is_tof2(dev_id)) {
    rc = port_mgr_map_dev_port_to_all(
        dev_id, dev_port, NULL, NULL, NULL, NULL, &is_cpu_port);
    if (rc != BF_SUCCESS) return rc;
    if (is_cpu_port) {
      port_mgr_log(
          "Error : Get link fault status not supported on CPU port for Tof2");
      return BF_INVALID_ARG;
    }
  }

  *link_fault_st = port_p->fsm_ext.link_fault_status;

  // If interrupt based link monitoring is enabled and the link is UP (according
  // the FSM) then returns the link fault state keep in alt_link_fault_st,
  // that reflects the actual value, updated by the ISR.
  if (*link_fault_st == BF_PORT_LINK_FAULT_OK &&
      dev_p->ldev.interrupt_based_link_monitoring) {
    if (port_mgr_dev_is_tof2(dev_id)) {
      rc = port_mgr_tof2_link_fault_status_get(dev_id, dev_port, link_fault_st);
    } else {
      *link_fault_st = port_p->fsm_ext.alt_link_fault_st;
    }
  }

  return rc;
}

/** \brief Set link fault status for a Tofino port.
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 * \param link_fault_st: link fault status.
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG: invalid dev_id
 * \return: BF_INVALID_ARG: invalid dev_port
 *
 */
bf_status_t bf_port_link_fault_status_set(
    bf_dev_id_t dev_id,
    bf_dev_port_t dev_port,
    bf_port_link_fault_st_t link_fault_st) {
  port_mgr_port_t *port_p = NULL;
  port_mgr_dev_t *dev_p = NULL;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  dev_p = port_mgr_map_dev_id_to_dev_p(dev_id);
  if (port_p == NULL || dev_p == NULL) return BF_INVALID_ARG;

  if (!port_p->sw.enabled) return BF_INVALID_ARG;

  port_p->fsm_ext.link_fault_status = link_fault_st;

  return BF_SUCCESS;
}

static char *lpbk_str[] = {
    "NONE     ",  // BF_LPBK_NONE = 0,
    "MAC NEAR ",  // BF_LPBK_MAC_NEAR,
    "MAC FAR  ",  // BF_LPBK_MAC_FAR,
    "PCS      ",  // BF_LPBK_PCS_NEAR,
    "SERD NEAR",  // BF_LPBK_SERDES_NEAR,
    "SERD FAR ",  // BF_LPBK_SERDES_FAR,
    "PIPE     ",  // BF_LPBK_PIPE,
};

/** \brief Map loopback mode to display string
 *
 * \param lpbk_mode
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_port_loopback_mode_to_str(bf_loopback_mode_e lpbk_mode,
                                         char **str) {
  if (str == NULL) return BF_INVALID_ARG;

  if (lpbk_mode < sizeof(lpbk_str) / sizeof(lpbk_str[0])) {
    *str = lpbk_str[lpbk_mode];
  }
  return BF_INVALID_ARG;
}
static char *prbs_str[] = {
    "PRBS31",  // BF_PORT_PRBS_MODE_31 = 0,
    "PRBS23",  // BF_PORT_PRBS_MODE_23,
    "PRBS15",  // BF_PORT_PRBS_MODE_15,
    "PRBS13",  // BF_PORT_PRBS_MODE_13,
    "PRBS11",  // BF_PORT_PRBS_MODE_11,
    "PRBS9 ",  // BF_PORT_PRBS_MODE_9,
    "PRBS7 ",  // BF_PORT_PRBS_MODE_7,
    "NONE  ",  // BF_PORT_PRBS_MODE_NONE,
};

/** \brief Map PRBS mode to display string
 * (tof2 only)
 *
 * \param dev_id
 * \param dev_port
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_port_prbs_mode_to_str(bf_port_prbs_mode_t prbs_mode,
                                     char **str) {
  if (str == NULL) return BF_INVALID_ARG;

  if (prbs_mode < sizeof(prbs_str) / sizeof(prbs_str[0])) {
    *str = prbs_str[prbs_mode];
  }
  return BF_INVALID_ARG;
}

/** \brief Retreive the full path to the serdes FW files
 * (tof2 only)
 *
 * \param dev_id
 * \param dev_port
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_port_fw_get(bf_dev_id_t dev_id,
                           char **fw_grp_0_7,
                           char **fw_grp_8) {
  bf_status_t rc;

  if (fw_grp_0_7 == NULL) return BF_INVALID_ARG;
  if (fw_grp_8 == NULL) return BF_INVALID_ARG;

  rc = port_mgr_tof2_dev_fw_get(dev_id, fw_grp_0_7, fw_grp_8);
  return rc;
}

/** \brief Reset (toggle) the UMAC Tx path
 * (tof2 only)
 *
 * \param dev_id
 * \param dev_port
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_port_tx_reset_set(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  bf_status_t rc;

  rc = port_mgr_tof2_port_tx_reset_set(dev_id, dev_port);
  return rc;
}

/** \brief Reset (toggle) the UMAC Rx path
 * (tof2 only)
 *
 * \param dev_id
 * \param dev_port
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_port_rx_reset_set(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  bf_status_t rc;

  rc = port_mgr_tof2_port_rx_reset_set(dev_id, dev_port);
  return rc;
}

bf_status_t bf_port_bind_interrupt_callback(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port,
                                            bf_port_int_callback_t fn,
                                            void *userdata) {
  bf_status_t rc;

  rc = port_mgr_port_bind_interrupt_callback(dev_id, dev_port, fn, userdata);
  return rc;
}

bf_status_t bf_port_tof3_pcs_status_get(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        bf_tof3_pcs_status_t *pcs) {
  return port_mgr_tof3_pcs_status_get(dev_id, dev_port, pcs);
}

bf_status_t bf_port_tof3_fec_status_get(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        bf_tof3_fec_status_t *fec) {
  return port_mgr_tof3_fec_status_get(dev_id, dev_port, fec);
}

bf_status_t bf_port_bring_up_time_get(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      uint64_t *but_us) {
  bf_status_t rc;

  if (but_us == NULL) return BF_INVALID_ARG;

  rc = port_mgr_port_bring_up_time_get(dev_id, dev_port, but_us);
  return rc;
}

/** \brief Get the time between signal-detect and port Up (in usec)
 *
 * \param dev_id
 * \param dev_port
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_port_link_up_time_get(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port,
                                     uint64_t *but_us) {
  bf_status_t rc;

  if (but_us == NULL) return BF_INVALID_ARG;

  rc = port_mgr_port_link_up_time_get(dev_id, dev_port, but_us);
  return rc;
}

/** \brief Set last time signal was detected on this port
 *
 * \param dev_id
 * \param dev_port
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_port_signal_detect_time_set(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port) {
  bf_status_t rc;

  rc = port_mgr_port_signal_detect_time_set(dev_id, dev_port);
  return rc;
}

/** \brief Set last time signal was detected on this port
 *
 * \param dev_id
 * \param dev_port
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_port_tof3_tmac_reset_set(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        uint32_t val) {
  bf_status_t rc;

  rc = port_mgr_tof3_port_tmac_reset_set(dev_id, dev_port, val);
  return rc;
}

/** \brief Set link training start reference time.
 *
 * \param dev_id
 * \param dev_port
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_port_an_lt_start_time_set(bf_dev_id_t dev_id,
                                         bf_dev_port_t dev_port) {
  bf_status_t rc;

  rc = port_mgr_port_an_lt_start_time_set(dev_id, dev_port);
  return rc;
}

/** \brief Compute and save link training duration in us
 *
 * \param dev_id
 * \param dev_port
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_port_an_lt_dur_set(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  bf_status_t rc;

  rc = port_mgr_port_an_lt_dur_set(dev_id, dev_port);
  return rc;
}

/** \brief Get link auto-neg/link training stats
 *
 * \param dev_id
 * \param dev_port
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_port_an_lt_stats_get(bf_dev_id_t dev_id,
                                    bf_dev_port_t dev_port,
                                    uint64_t *an_lt_dur_us,
                                    uint32_t *an_try_cnt) {
  bf_status_t rc;

  rc =
      port_mgr_port_an_lt_stats_get(dev_id, dev_port, an_lt_dur_us, an_try_cnt);

  return rc;
}

/** \brief Increment AN try counter
 *
 * \param dev_id
 * \param dev_port
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_port_an_try_inc(bf_dev_id_t dev_id, bf_dev_port_t dev_port) {
  bf_status_t rc;

  rc = port_mgr_port_an_try_inc(dev_id, dev_port);

  return rc;
}

/** \brief Init link training duration.
 *
 * \param dev_id
 * \param dev_port
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG:
 */
bf_status_t bf_port_an_lt_stats_init(bf_dev_id_t dev_id,
                                     bf_dev_port_t dev_port,
                                     bool init_all) {
  bf_status_t rc;

  rc = port_mgr_port_an_lt_stats_init(dev_id, dev_port, init_all);

  return rc;
}

bf_status_t bf_port_tof3_fec_lane_symb_err_counter_get(bf_dev_id_t dev_id,
                                                       bf_dev_port_t dev_port,
                                                       uint32_t n_ctrs,
                                                       uint64_t symb_err[16]) {
  bf_status_t rc;

  rc = port_mgr_tof3_port_fec_lane_symb_err_counter_get(
      dev_id, dev_port, n_ctrs, symb_err);
  return rc;
}

bf_status_t bf_port_tof_serdes_temperature_get(bf_dev_id_t dev_id,
                                               uint8_t arr_size,
                                               float *arr_temp,
                                               uint8_t *num_temp) {
  bf_status_t status = BF_SUCCESS;
  bf_dev_family_t dev_family = BF_DEV_FAMILY_UNKNOWN;
  uint32_t num_pipes = 0;
  uint32_t pipe = 0;

  if (arr_temp == NULL || num_temp == NULL) {
    port_mgr_log_error("Invalid argument specified");
    return BF_INVALID_ARG;
  }

  if (arr_size == 0) {
    port_mgr_log_error("Invalid array size specified");
    return BF_INVALID_ARG;
  }

  status = bf_device_family_get(dev_id, &dev_family);
  if (status != BF_SUCCESS) {
    port_mgr_log_error("Error of receiving the device family");
    return BF_HW_COMM_FAIL;
  }

  *num_temp = 0u;
  if (dev_family != BF_DEV_FAMILY_TOFINO2) {
    port_mgr_log_warn("Not supported for %s device family",
                      bf_dev_family_str(dev_family));
    return BF_NOT_SUPPORTED;
  }

  status = bf_pal_num_pipes_get(dev_id, &num_pipes);
  if (status != BF_SUCCESS) {
    port_mgr_log_error("Failed to retrieve the number of pipes");
    return BF_HW_COMM_FAIL;
  }

  *num_temp = num_pipes;
  if (num_pipes > arr_size) {
    // Return required array size in @num_temp
    return BF_NO_SPACE;
  }

  for (pipe = 0; pipe < num_pipes; pipe++) {
    float temp = 0.0;
    int temp_tries = 3;
    bf_dev_port_t dev_port = MAKE_DEV_PORT(pipe, 8);
    do {
      status = bf_tof2_serdes_fw_temperature_get(dev_id, dev_port, &temp);
      if (status != BF_SUCCESS) {
        port_mgr_log_error("Failed to retrieve serdes temp on pipe %u", pipe);
        return BF_HW_COMM_FAIL;
      }
    } while ((temp_tries-- > 0) && ((temp == 0.0) || (temp == -0.0)));
    arr_temp[pipe] = temp;
  }
  return status;
}

/** \brief Update historical counters in anticipation of HW stats being cleared
 *
 * [ POST_ENABLE ]
 *
 * \param dev_id  : system-assigned identifier (0..BF_MAX_DEV_COUNT-1)
 * \param dev_port: encoded port identifier
 *
 * \return: BF_SUCCESS
 * \return: BF_INVALID_ARG : dev_id never added or dev_id > BF_MAX_DEV_COUNT-1
 * \return: BF_INVALID_ARG : invalid or missing bf_dev_port_t
 * \return: BF_NOT_READY   : MAC stat request outstanding (try again later)
 * \return: BF_HW_COMM_FAIL: DMA memory not allocated correctly
 *
 */
bf_status_t bf_port_hw_mac_stats_clear(bf_dev_id_t dev_id,
                                       bf_dev_port_t dev_port) {
  int ctr_rd_failed;
  port_mgr_port_t *port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);

  if (port_p == NULL) return BF_INVALID_ARG;

  // disallow simultaneous access as cache is being updated
  bf_port_mac_stats_mtx_lock(dev_id, dev_port);

  // read counters into mac stat cache
  ctr_rd_failed = port_mgr_port_read_all_counters(dev_id, dev_port);

  if (ctr_rd_failed) {
    bf_port_mac_stats_mtx_unlock(dev_id, dev_port);
    return BF_HW_COMM_FAIL;
  }
  port_mgr_mac_stats_copy(&port_p->mac_stat_cache,
                          &port_p->mac_stat_historical,
                          &port_p->mac_stat_historical);

  // now clear cached counters (last HW stats read or DMA'd) so they cant get
  // added again
  port_mgr_mac_stats_clear_sw_ctrs(&port_p->mac_stat_cache);

  // clear HW counters
  if (port_mgr_dev_is_tof3(dev_id)) {
    port_mgr_tof3_port_mac_stats_clear(dev_id, dev_port);
  } else if (port_mgr_dev_is_tof2(dev_id)) {
    port_mgr_tof2_port_mac_stats_clear(dev_id, dev_port);
  } else {
    // clear HW counters
    port_mgr_mac_stats_clear(dev_id, dev_port);
  }

  // cache updated now, safe for other accesses
  bf_port_mac_stats_mtx_unlock(dev_id, dev_port);

  return BF_SUCCESS;
}

/** \brief
 *
 */
bf_status_t bf_port_oper_state_set_force_callbacks(bf_dev_id_t dev_id,
                                                   bf_dev_port_t dev_port,
                                                   int en) {
  port_mgr_port_t *port_p = NULL;
  port_mgr_dev_t *dev_p = port_mgr_map_dev_id_to_dev_p(dev_id);
  if (dev_p == NULL) return BF_INVALID_ARG;

  port_p = port_mgr_map_dev_port_to_port(dev_id, dev_port);
  if (port_p == NULL) return BF_INVALID_ARG;
  port_p->force_link_down_cb = en;
  return BF_SUCCESS;
}
