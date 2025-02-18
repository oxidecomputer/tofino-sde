/*******************************************************************************
 * Copyright (c) 2015-2020 Barefoot Networks, Inc.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * $Id: $
 *
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>

#include <bf_bd_cfg/bf_bd_cfg_intf.h>
#include <bf_port_mgmt/bf_port_mgmt_intf.h>

#include <bf_types/bf_types.h>
#include <target-sys/bf_sal/bf_sys_sem.h>
#include <target-sys/bf_sal/bf_sys_timer.h>
#include <bf_switchd/bf_switchd.h>
#include <tofino/bf_pal/bf_pal_pltfm_porting.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include <bf_pm/bf_pm_intf.h>
#include <bf_pltfm.h>
#include <bf_pltfm_ext_phy.h>
#include <bf_pltfm_types/bf_pltfm_types.h>
#include <target-utils/map/map.h>
#include "bf_pm_priv.h"

#define PM_BIT_SET(val, bit_pos) ((val) |= (1 << (bit_pos)))
#define PM_BIT_CLR(val, bit_pos) ((val) &= ~(1 << (bit_pos)))
#define PM_BIT_GET(val, bit_pos) (((val) >> (bit_pos)) & 1)

#define QSFP_SCAN_TMR_PERIOD_MS 2000
bf_sys_timer_t qsfp_scan_timer;

#define QSFP_FSM_TMR_PERIOD_MS 100
bf_sys_timer_t qsfp_fsm_timer;

static bool pm_qsfp_quick_removed[BF_PLAT_MAX_QSFP + 1];

static bf_pm_qsfp_info_t pm_qsfp_info_arr[BF_PLAT_MAX_QSFP + 1];

static uint32_t
    qsfp_pres_mask[3];  // 0->lower mask, 1->upper mask 2->cpu port mask

static uint32_t qsfp_quick_rmv_pres_mask[3];  // 0->lower mask, 1->upper mask
                                              // 2->cpu port mask

static int num_ports = 0;

int bf_pm_num_ports_get(void) { return num_ports; }

static bool bf_pltfm_pm_ha_mode = false;

void bf_pm_qsfp_quick_removal_detected_set(uint32_t conn_id, bool flag) {
  int mask_id = 2;  // cpu port
  if (conn_id <= 32) {
    mask_id = 1;
  } else if (conn_id <= 64) {
    mask_id = 0;
  }

  if (flag) {
    PM_BIT_SET(qsfp_quick_rmv_pres_mask[mask_id], (conn_id % 32) - 1);
  } else {
    PM_BIT_CLR(qsfp_quick_rmv_pres_mask[mask_id], (conn_id % 32) - 1);
  }
  pm_qsfp_quick_removed[conn_id] = flag;
}

bool bf_pm_qsfp_quick_removal_detected_get(uint32_t conn_id) {
  return pm_qsfp_quick_removed[conn_id];
}

static void qsfp_info_clear(uint32_t conn_id) {
  pm_qsfp_info_arr[conn_id].is_present = false;
  pm_qsfp_info_arr[conn_id].qsfp_type = BF_PLTFM_QSFP_UNKNOWN;
  pm_qsfp_info_arr[conn_id].qsfpdd_type = BF_PLTFM_QSFPDD_UNKNOWN;
}

// populate qsfp_info_arr for CMIS modules
static void cmis_populate_qsfp_info_arr(int conn_id) {
  bf_cmis_type_get(conn_id, &pm_qsfp_info_arr[conn_id].qsfpdd_type);

  // Hw-init method. Now handled in the module fsm as host-init
  // bf_qsfp_set_transceiver_lpmode(conn_id, true);
  // bf_qsfp_reset(conn_id, true);
  // bf_qsfp_set_transceiver_lpmode(conn_id, false);
  // bf_qsfp_reset(conn_id, false);
}

// populate qsfp_info_arr for SFF-8636 modules
static void sff8636_populate_qsfp_info_arr(int conn_id) {
  if (bf_qsfp_type_get(conn_id, &pm_qsfp_info_arr[conn_id].qsfp_type) != 0) {
    LOG_ERROR("PM_INTF QSFP    %2d : error getting QSFP type\n", conn_id);
  }
}

static void qsfp_present_actions(int conn_id) {
  uint8_t module_type = 0;

  if (conn_id > bf_qsfp_get_max_qsfp_ports()) {
    LOG_ERROR("PM_INTF QSFP    %2d : Invalid. Max supported = %2d",
              conn_id,
              bf_qsfp_get_max_qsfp_ports());
    return;
  }

  bf_qsfp_get_module_type(conn_id, &module_type);
  if (module_type == 0) {
    LOG_ERROR("PM_INTF QSFP    %2d : Unknown module detected (id=%d)\n",
              conn_id,
              module_type);
    return;
  }

  if (bf_qsfp_is_cmis(conn_id)) {
    cmis_populate_qsfp_info_arr(conn_id);
  } else {
    sff8636_populate_qsfp_info_arr(conn_id);
  }
  pm_qsfp_info_arr[conn_id].is_optic = bf_qsfp_is_optical(conn_id);

  LOG_DEBUG("PM_INTF QSFP    %2d : %s module\n",
            conn_id,
            pm_qsfp_info_arr[conn_id].is_optic ? "Optical" : "Copper");

  // mark present
  pm_qsfp_info_arr[conn_id].is_present = true;
}

// this function is only used for Tofino1 systems
static void qsfp_all_info_read(int conn_id) {
  // Get the type of the QSFP connector
  if (bf_qsfp_type_get(conn_id, &pm_qsfp_info_arr[conn_id].qsfp_type) != 0) {
    LOG_ERROR("Port   %2d : error getting QSFP type\n", conn_id);
  }
}

static bool autoneg_to_apply(uint32_t conn, uint32_t chnl) {
  if ((pm_qsfp_info_arr[conn].qsfp_type == BF_PLTFM_QSFP_CU_0_5_M) ||
      (pm_qsfp_info_arr[conn].qsfp_type == BF_PLTFM_QSFP_CU_1_M) ||
      (pm_qsfp_info_arr[conn].qsfp_type == BF_PLTFM_QSFP_CU_2_M) ||
      (pm_qsfp_info_arr[conn].qsfp_type == BF_PLTFM_QSFP_CU_3_M)) {
    return true;
  }
  return false;
}

static void qsfp_detection_actions(bf_dev_id_t dev_id, int conn_id) {
  int chnl_id;
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;
  bool an_elig = false;

  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    return;
  }

  qsfp_all_info_read(conn_id);

  bf_pltfm_platform_ext_phy_config_set(
      dev_id, conn_id, pm_qsfp_info_arr[conn_id].qsfp_type);
  port_hdl.conn_id = conn_id;
  for (chnl_id = 0; chnl_id < QSFP_NUM_CHN; chnl_id++) {
    port_hdl.chnl_id = chnl_id;
    an_elig = autoneg_to_apply(conn_id, chnl_id);
    sts = bf_pal_pm_front_port_eligible_for_autoneg(dev_id, &port_hdl, an_elig);
    if (sts != BF_SUCCESS) {
      LOG_ERROR(
          "Unable to mark port eligible for AN for dev : %d : front port : "
          "%d/%d : %s (%d)",
          dev_id,
          port_hdl.conn_id,
          port_hdl.chnl_id,
          bf_err_str(sts),
          sts);
    }

    sts = bf_pal_pm_front_port_ready_for_bringup(dev_id, &port_hdl, true);
    if (sts != BF_SUCCESS) {
      LOG_ERROR(
          "Unable to mark port ready for bringup for dev : %d : front port : "
          "%d/%d : %s (%d)",
          dev_id,
          port_hdl.conn_id,
          port_hdl.chnl_id,
          bf_err_str(sts),
          sts);
    }
  }
}

static void qsfp_removal_actions(bf_dev_id_t dev_id, int conn_id) {
  int chnl_id;
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl;

  qsfp_info_clear(conn_id);

  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    return;
  }

  port_hdl.conn_id = conn_id;
  for (chnl_id = 0; chnl_id < QSFP_NUM_CHN; chnl_id++) {
    port_hdl.chnl_id = chnl_id;
    sts = bf_pal_pm_front_port_eligible_for_autoneg(dev_id, &port_hdl, false);
    if (sts != BF_SUCCESS) {
      LOG_ERROR(
          "Unable to mark port eligible for AN for dev : %d : front port : "
          "%d/%d : %s (%d)",
          dev_id,
          port_hdl.conn_id,
          port_hdl.chnl_id,
          bf_err_str(sts),
          sts);
    }
    qsfp_fsm_ch_notify_not_ready(dev_id, port_hdl.conn_id, port_hdl.chnl_id);
    sts = bf_pal_pm_front_port_ready_for_bringup(dev_id, &port_hdl, false);
    if (sts != BF_SUCCESS) {
      LOG_ERROR(
          "Unable to mark port ready for bringup for dev : %d : front port : "
          "%d/%d : %s (%d)",
          dev_id,
          port_hdl.conn_id,
          port_hdl.chnl_id,
          bf_err_str(sts),
          sts);
    }
  }

  (void)dev_id;
}

void qsfp_scan_removed(bf_dev_id_t dev_id, int conn_id) {
  pm_qsfp_info_arr[conn_id].is_present = false;
  qsfp_removal_actions(dev_id, conn_id);
  qsfp_fsm_removed(conn_id);
}

static bf_pltfm_status_t qsfp_scan_helper(bf_dev_id_t dev_id,
                                          int conn_id,
                                          uint32_t mask,
                                          int mask_id) {
  uint8_t res_bit;
  uint32_t res_mask;
  bool is_present = false;

  res_mask = qsfp_pres_mask[mask_id] ^ mask;

  while (res_mask != 0) {
    // Indicates that there is change in the number of the plugged in qsfps
    // Probe each bit to find the exact id of the qsfp
    res_bit = res_mask & 0x01;
    while (res_bit == 0) {
      res_mask = res_mask >> 1;
      res_bit = res_mask & 0x01;

      // mask = mask >> 1;
      conn_id++;
    }
    res_mask = res_mask >> 1;

    if (!bf_pltfm_pm_is_ha_mode()) {
      // Handle any previous removal found via fsm, if any.
      if (bf_pm_qsfp_quick_removal_detected_get(conn_id)) {
        goto handle_removal;
      }

      // Actual reset sequence etc per spec are handled via qsfp-fsm.
      // Just toggle reset so that we can read the module
      if (bf_qsfp_get_reset(conn_id)) {
        int rc;

        LOG_DEBUG("PM_INTF QSFP    %2d : RESETL = true", conn_id);
        // assert resetL
        rc = bf_qsfp_reset(conn_id, true);
        if (rc != 0) {
          LOG_ERROR(
              "PM_INTF QSFP    %2d : Error <%d> asserting resetL", conn_id, rc);
        }

        bf_sys_usleep(3);  // really 2 micro-seconds

        LOG_DEBUG("PM_INTF QSFP    %2d : RESETL = false", conn_id);
        // de-assert resetL
        rc = bf_qsfp_reset(conn_id, false);
        if (rc != 0) {
          LOG_ERROR("PM_INTF QSFP    %2d : Error <%d> de-asserting resetL",
                    conn_id,
                    rc);
        }
        // We need 2-seconds for module to be ready, hence we continue.
        // In case, module is not ready, bf_qsfp_detect_transceiver will
        // retry.

        conn_id++;
        continue;  // back to outer while loop
      }
    }

    bool qsfp_curr_st_abs = PM_BIT_GET(mask, (conn_id % 32) - 1);
    bool qsfp_prev_st_abs =
        PM_BIT_GET(qsfp_pres_mask[mask_id], (conn_id % 32) - 1);
    LOG_DEBUG("PM_INTF QSFP    %2d : curr-pres-st : %d prev-pres-st : %d",
              conn_id,
              qsfp_curr_st_abs,
              qsfp_prev_st_abs);
    if (qsfp_curr_st_abs) {
      if (!qsfp_prev_st_abs) {
        LOG_DEBUG("PM_INTF QSFP    %2d : unplugged (from plug st)\n", conn_id);
        is_present = false;
        // hack to clear the states.
        bf_qsfp_set_present(conn_id, is_present);
        goto handle_removal;
      }
      // we should never land here. But fall through and handle as done
      // previously
      LOG_DEBUG("PM_INTF QSFP    %2d : unplugged (from unplugged st)", conn_id);
    }

    int detect_st = bf_qsfp_detect_transceiver(conn_id, &is_present);
    LOG_DEBUG("PM_INTF QSFP    %2d : detect-st : %d is-present : %d",
              conn_id,
              detect_st,
              is_present);

    // Find if the said qsfp module was removed or added
    if (detect_st) {
      // hopefully, detect it in the next iteration
      conn_id++;
      continue;  // back to outer while loop
    } else {
    handle_removal:
      if (bf_pm_qsfp_quick_removal_detected_get(conn_id)) {
        // over-ride present bit so that we go through clean state in next cycle
        if (is_present) {
          LOG_DEBUG(
              "PM_INTF QSFP    %2d : Latched removal conditon detected. Doing "
              "removal "
              "actions.\n",
              conn_id);
          is_present = false;
        }
        bf_pm_qsfp_quick_removal_detected_set(conn_id, false);
      }
      // update qsfp_pres_mask after successfully detecting the module
      if (is_present) {
        PM_BIT_CLR(qsfp_pres_mask[mask_id], (conn_id % 32) - 1);
      } else {
        PM_BIT_SET(qsfp_pres_mask[mask_id], (conn_id % 32) - 1);
      }
    }
    if (is_present == true) {
      if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
        // determines qsfp type only
        qsfp_present_actions(conn_id);
      }
      // kick off the module FSM
      if (!bf_pltfm_pm_is_ha_mode()) {
        qsfp_fsm_inserted(conn_id);
      } else {
        qsfp_state_ha_config_set(dev_id, conn_id);
      }
    } else {
      // Update the cached status of the qsfp
      if (!bf_pltfm_pm_is_ha_mode()) {
        qsfp_scan_removed(dev_id, conn_id);
      } else {
        qsfp_state_ha_config_delete(conn_id);
      }
    }

    conn_id++;
  }  // Outside while
  return BF_PLTFM_SUCCESS;
}

bf_pltfm_status_t qsfp_scan(bf_dev_id_t dev_id) {
  uint32_t lower_mask, upper_mask, cpu_mask;
  uint32_t lower_mask_prev, upper_mask_prev, cpu_mask_prev;
  bf_pltfm_status_t sts;
  upper_mask_prev = upper_mask = qsfp_pres_mask[0];
  lower_mask_prev = lower_mask = qsfp_pres_mask[1];
  cpu_mask_prev = cpu_mask = qsfp_pres_mask[2];

  // Query the QSFP library for the qsfp presence mask
  if (bf_qsfp_get_transceiver_pres(&lower_mask, &upper_mask, &cpu_mask) != 0) {
    return BF_PLTFM_COMM_FAILED;
  }

  // Handle quick unplug and plug
  // Cross check with latched state and over-ride the mask
  if (lower_mask_prev != qsfp_quick_rmv_pres_mask[1]) {
    LOG_DEBUG("pres-lower-mask changed: 0x%0x -> 0x%0x",
              lower_mask_prev,
              qsfp_quick_rmv_pres_mask[1]);
    lower_mask = qsfp_quick_rmv_pres_mask[1];
  }

  if (upper_mask_prev != qsfp_quick_rmv_pres_mask[0]) {
    LOG_DEBUG("pres-upper-mask changed: 0x%0x -> 0x%0x",
              upper_mask_prev,
              qsfp_quick_rmv_pres_mask[0]);
    upper_mask = qsfp_quick_rmv_pres_mask[0];
  }

  if (cpu_mask_prev != qsfp_quick_rmv_pres_mask[2]) {
    LOG_DEBUG("pres-cpu-mask changed: 0x%0x -> 0x%0x",
              cpu_mask_prev,
              qsfp_quick_rmv_pres_mask[2]);
    cpu_mask = qsfp_quick_rmv_pres_mask[2];
  }

  if (bf_bd_cfg_bd_num_port_get() <= 33) {
    sts = qsfp_scan_helper(dev_id, 1, lower_mask, 1);
    if (sts != BF_PLTFM_SUCCESS) {
      LOG_ERROR("PM_INTF QSFP:%s in scanning the QSFPs (1-32) at %s:%d\n",
                bf_pltfm_err_str(sts),
                __func__,
                __LINE__);
    }

    sts = qsfp_scan_helper(dev_id, 33, cpu_mask, 2);
    if (sts != BF_PLTFM_SUCCESS) {
      LOG_ERROR("PM_INTF QSFP:%s in scanning the QSFP (33) at %s:%d\n",
                bf_pltfm_err_str(sts),
                __func__,
                __LINE__);
    }
  } else if (bf_bd_cfg_bd_num_port_get() <= 65) {
    sts = qsfp_scan_helper(dev_id, 1, lower_mask, 1);
    if (sts != BF_PLTFM_SUCCESS) {
      LOG_ERROR("PM_INTF QSFP:%s in scanning the QSFPs (1-32) at %s:%d\n",
                bf_pltfm_err_str(sts),
                __func__,
                __LINE__);
    }
    sts = qsfp_scan_helper(dev_id, 33, upper_mask, 0);
    if (sts != BF_PLTFM_SUCCESS) {
      LOG_ERROR("PM_INTF QSFP:%s in scanning the QSFPs (33-64)  at %s:%d\n",
                bf_pltfm_err_str(sts),
                __func__,
                __LINE__);
    }
    sts = qsfp_scan_helper(dev_id, 65, cpu_mask, 2);
    if (sts != BF_PLTFM_SUCCESS) {
      LOG_ERROR("PM_INTF QSFP:%s in scanning the QSFP (65) at %s:%d\n",
                bf_pltfm_err_str(sts),
                __func__,
                __LINE__);
    }
  }

  qsfp_quick_rmv_pres_mask[0] = qsfp_pres_mask[0];
  qsfp_quick_rmv_pres_mask[1] = qsfp_pres_mask[1];
  qsfp_quick_rmv_pres_mask[2] = qsfp_pres_mask[2];

  return BF_PLTFM_SUCCESS;
}

void qsfp_scan_timer_cb(struct bf_sys_timer_s *timer, void *data) {
  bf_pltfm_status_t sts;
  bf_dev_id_t dev_id = (bf_dev_id_t)(intptr_t)data;

  static int dumped = 0;
  if (!dumped) {
    LOG_DEBUG("PM_INTF QSFP Scan started..");
    dumped = 1;
  }
  // printf("PM_INTF QSFP timer off\n");
  // Scan the insertion and detection of QSFPs
  sts = qsfp_scan(dev_id);
  if (sts != BF_PLTFM_SUCCESS) {
    LOG_ERROR("PM_INTF QSFP Error %s: in scanning qsfps at %s:%d\n",
              bf_pltfm_err_str(sts),
              __func__,
              __LINE__);
  }
  (void)timer;
}

void qsfp_fsm_timer_cb(struct bf_sys_timer_s *timer, void *data) {
  bf_pltfm_status_t sts;
  bf_dev_id_t dev_id = (bf_dev_id_t)(intptr_t)data;

  if (bf_pm_intf_is_device_family_tofino(dev_id)) {
    // Process QSFP start-up actions (if necessary)
    sts = qsfp_fsm(dev_id);
    if (sts != BF_PLTFM_SUCCESS) {
      LOG_ERROR("PM_INTF QSFP %s: in qsfp fsm at %s:%d\n",
                bf_pltfm_err_str(sts),
                __func__,
                __LINE__);
    }
  } else {
    qsfp_fsm(dev_id);
    bf_pm_interface_fsm();
  }

  (void)timer;
}

bf_pltfm_status_t bf_pltfm_pm_port_qsfp_type_get(
    bf_pltfm_port_info_t *port_info, bf_pltfm_qsfp_type_t *qsfp_type) {
  if (!port_info || !qsfp_type) return BF_PLTFM_INVALID_ARG;
  if ((port_info->conn_id > (uint32_t)num_ports) ||
      (port_info->chnl_id >= QSFP_NUM_CHN)) {
    return BF_PLTFM_INVALID_ARG;
  }

  *qsfp_type = pm_qsfp_info_arr[port_info->conn_id].qsfp_type;

  return BF_PLTFM_SUCCESS;
}

bf_pltfm_status_t pltfm_pm_port_qsfp_is_present(bf_pltfm_port_info_t *port_info,
                                                bool *is_present) {
  if (!port_info) return BF_PLTFM_INVALID_ARG;
  if ((port_info->conn_id > (uint32_t)num_ports) ||
      (port_info->chnl_id >= QSFP_NUM_CHN)) {
    return BF_PLTFM_INVALID_ARG;
  }

  // use bf_qsfp_is_present for consistent view - TBD
  *is_present = pm_qsfp_info_arr[port_info->conn_id].is_present;

  return BF_PLTFM_SUCCESS;
}

bf_pltfm_status_t bf_pltfm_pm_qsfp_scan_poll_start() {
  bf_sys_timer_status_t rc;
  // Start the QSFP scan only if the board type is not Emulator/Model
  if (platform_is_hw()) {
    rc = bf_sys_timer_start(&qsfp_scan_timer);
    if (rc) {
      LOG_ERROR("PM_INTF QSFP    %2d: in starting qsfp-scan timer\n", rc);
      return BF_PLTFM_COMM_FAILED;
    }
    rc = bf_sys_timer_start(&qsfp_fsm_timer);
    if (rc) {
      LOG_ERROR("PM_INTF QSFP    %2d: in starting qsfp-fsm timer\n", rc);
      return BF_PLTFM_COMM_FAILED;
    }
  }
  return BF_PLTFM_SUCCESS;
}

bf_pltfm_status_t bf_pltfm_pm_qsfp_scan_poll_stop() {
  bf_sys_timer_status_t rc;
  rc = bf_sys_timer_stop(&qsfp_scan_timer);
  if (rc) {
    LOG_ERROR("PM_INTF QSFP    %2d: in stopping qsfp-scan timer\n", rc);
    return BF_PLTFM_COMM_FAILED;
  }
  rc = bf_sys_timer_stop(&qsfp_fsm_timer);
  if (rc) {
    LOG_ERROR("PM_INTF QSFP    %2d: in stopping qsfp-fsm timer\n", rc);
    return BF_PLTFM_COMM_FAILED;
  }
  return BF_PLTFM_SUCCESS;
}

bf_pltfm_status_t bf_pltfm_pm_init(bf_dev_init_mode_t init_mode) {
  bf_pltfm_status_t sts;
  bf_dev_id_t dev_id = 0;
  int conn_id;
  bf_sys_timer_status_t rc;

  num_ports = bf_qsfp_get_max_qsfp_ports();
  LOG_DEBUG("num of QSFP ports=%d", num_ports);

  for (conn_id = 0; conn_id < num_ports; conn_id++) {
    qsfp_info_clear(conn_id);
  }

  qsfp_pres_mask[0] = 0xffffffff;
  qsfp_pres_mask[1] = 0xffffffff;
  qsfp_pres_mask[2] = 0xffffffff;

  qsfp_quick_rmv_pres_mask[0] = 0xffffffff;
  qsfp_quick_rmv_pres_mask[1] = 0xffffffff;
  qsfp_quick_rmv_pres_mask[2] = 0xffffffff;

  if (platform_is_hw()) {
    rc = bf_sys_timer_create(&qsfp_scan_timer,
                             QSFP_SCAN_TMR_PERIOD_MS,
                             QSFP_SCAN_TMR_PERIOD_MS,
                             qsfp_scan_timer_cb,
                             (void *)(intptr_t)dev_id);
    if (rc) {
      LOG_ERROR("PM_INTF QSFP    %2d: in creating qsfp-scan timer\n", rc);
      return BF_PLTFM_COMM_FAILED;
    }
  }

  if (platform_is_hw()) {
    if ((init_mode != BF_DEV_WARM_INIT_FAST_RECFG) &&
        (init_mode != BF_DEV_WARM_INIT_HITLESS)) {
      qsfp_deassert_all_reset_pins();
    }
    rc = bf_sys_timer_create(&qsfp_fsm_timer,
                             QSFP_FSM_TMR_PERIOD_MS,
                             QSFP_FSM_TMR_PERIOD_MS,
                             qsfp_fsm_timer_cb,
                             (void *)(intptr_t)dev_id);
    if (rc) {
      LOG_ERROR("PM_INTF QSFP    %2d: in creating qsfp-scan timer\n", rc);
      return BF_PLTFM_COMM_FAILED;
    }
  }
  // Register the qsfp type get function with the bd_cfg lib
  sts = bf_bd_cfg_qsfp_type_fn_reg(bf_pltfm_pm_port_qsfp_type_get);
  if (sts != BF_PLTFM_SUCCESS) {
    LOG_ERROR(
        "Unable to register the qsfp type get function with bd_cfg lib : %s "
        "(%d)",
        bf_pltfm_err_str(sts),
        sts);
    return sts;
  }

  return BF_PLTFM_SUCCESS;
}

bf_pltfm_status_t bf_pltfm_pm_deinit() {
  bf_sys_timer_status_t rc;

  // Stop the timers only if the board type is not Emulator/Model
  if (platform_is_hw()) {
    rc = bf_sys_timer_stop(&qsfp_scan_timer);
    if (rc) {
      LOG_ERROR("PM_INTF QSFP    %2d: in stopping qsfp-scan timer\n", rc);
      return BF_HW_COMM_FAIL;
    }
    rc = bf_sys_timer_stop(&qsfp_fsm_timer);
    if (rc) {
      LOG_ERROR("PM_INTF QSFP    %2d: in stopping qsfp-fsm timer\n", rc);
      return BF_HW_COMM_FAIL;
    }

    rc = bf_sys_timer_del(&qsfp_scan_timer);
    if (rc) {
      LOG_ERROR("PM_INTF QSFP    %2d: in deleting qsfp-scan timer\n", rc);
      return BF_HW_COMM_FAIL;
    }

    rc = bf_sys_timer_del(&qsfp_fsm_timer);
    if (rc) {
      LOG_ERROR("PM_INTF QSFP    %2d: in deleting qsfp-fsm timer\n", rc);
      return BF_HW_COMM_FAIL;
    }
  }

  return BF_PLTFM_SUCCESS;
}

bf_status_t bf_pltfm_pm_media_type_get(bf_pltfm_port_info_t *port_info,
                                       bf_media_type_t *media_type) {
  if (!port_info || !media_type) return BF_PLTFM_INVALID_ARG;
  if (bf_bd_is_this_port_internal(port_info->conn_id, port_info->chnl_id)) {
    // Indicates that it is an internal port and hence media type is unknown
    *media_type = BF_MEDIA_TYPE_UNKNOWN;
    return BF_PLTFM_SUCCESS;
  }
  if ((int)port_info->conn_id > num_ports) {
    *media_type = BF_MEDIA_TYPE_UNKNOWN;
    return BF_PLTFM_INVALID_ARG;
  }
  if (!pm_qsfp_info_arr[port_info->conn_id].is_present) {
    /*Indicates that no QSFP is inserted in the port. Hence the media type is
      unknown*/
    *media_type = BF_MEDIA_TYPE_UNKNOWN;
    return BF_PLTFM_SUCCESS;
  }

  if (bf_qsfp_is_optical(port_info->conn_id)) {
    *media_type = BF_MEDIA_TYPE_OPTICAL;
  } else {
    *media_type = BF_MEDIA_TYPE_COPPER;
  }

  return BF_PLTFM_SUCCESS;
}

bf_status_t bf_pltfm_pm_presence_get(bf_pltfm_port_info_t *port_info,
                                       bool *presence) {
  if (!port_info || !presence) return BF_PLTFM_INVALID_ARG;

  if (bf_bd_is_this_port_internal(port_info->conn_id, port_info->chnl_id)) {
    *presence = true;
    return BF_PLTFM_SUCCESS;
  }

  if ((int)port_info->conn_id > num_ports) {
    *presence = false;
    return BF_PLTFM_INVALID_ARG;
  }

  *presence = pm_qsfp_info_arr[port_info->conn_id].is_present;

  return BF_PLTFM_SUCCESS;
}

/*****************************************************************
 *
 *****************************************************************/
void qsfp_fsm_notify_bf_pltfm(bf_dev_id_t dev_id, int conn_id) {
  bf_dev_port_t dev_port = (bf_dev_port_t)0L;
  bf_pal_front_port_handle_t port_hdl;
  uint32_t chnl;
  bool is_luxtera = bf_qsfp_is_luxtera_optic(conn_id);
  bf_dev_id_t dev_id_of_port = 0;

  // notify bf_pltfm of new QSFP
  pm_qsfp_info_arr[conn_id].is_present = true;

  /* Luxtera modules require some special handling during bring-up.
   * They seem to prefer a higher loop bandwidth settingi.
   * This code takes care of the case where a port was defined on the
   * connector before a QSFP was detected (so we couldn't determine
   * the setting in the pre_port_enable callback). Now that the QSFP
   * has been detected we can set it properly.
   *
   * Note: Since we don't eally know what all ports are defined here
   *       we set the loop bandwidth for all four possible dev_ports
   *       specifying "lane 0" for each. The API dutifully converts
   *       this to the correct serdes structure for later application.
   *
   * drv-1347
   */
  port_hdl.conn_id = conn_id;
  for (chnl = 0; chnl < 4; chnl++) {
    port_hdl.chnl_id = chnl;
    bf_pm_port_front_panel_port_to_dev_port_get(
        &port_hdl, &dev_id_of_port, &dev_port);

    if (is_luxtera) {
      bf_serdes_tx_loop_bandwidth_set(
          dev_id, dev_port, 0, BF_SDS_TOF_TX_LOOP_BANDWIDTH_9MHZ);
    } else {
      bf_serdes_tx_loop_bandwidth_set(
          dev_id, dev_port, 0, BF_SDS_TOF_TX_LOOP_BANDWIDTH_DEFAULT);
    }
  }

  qsfp_detection_actions(dev_id, conn_id);
}

/*****************************************************************
 *
 *****************************************************************/
void bf_pltfm_pm_qsfp_simulate_all_removed(void) {
  int conn_id;

  for (conn_id = 1; conn_id < num_ports; conn_id++) {
    if (pm_qsfp_info_arr[conn_id].is_present) {
      qsfp_scan_removed(0, conn_id);
    }
  }
  qsfp_pres_mask[0] = 0xffffffff;
  qsfp_pres_mask[1] = 0xffffffff;
  qsfp_pres_mask[2] = 0xffffffff;
  bf_qsfp_debug_clear_all_presence_bits();
}

bf_pltfm_status_t bf_pltfm_pm_ha_mode_set() {
  bf_pltfm_pm_ha_mode = true;
  return BF_PLTFM_SUCCESS;
}

bf_pltfm_status_t bf_pltfm_pm_ha_mode_clear() {
  bf_pltfm_pm_ha_mode = false;
  return BF_PLTFM_SUCCESS;
}

bool bf_pltfm_pm_is_ha_mode() { return bf_pltfm_pm_ha_mode; }

bf_pm_qsfp_info_t *bf_pltfm_get_pm_qsfp_info_ptr(int connector) {
  return &pm_qsfp_info_arr[connector];
}
