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
#include <stdlib.h>
#include <getopt.h>
#include <inttypes.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <ctype.h>
#include <sys/time.h>
#include <unistd.h>

#include <bf_types/bf_types.h>
#include <lld/bf_lld_if.h>
#include <lld/lld_err.h>
#include <lld/lld_sku.h>
#include <port_mgr/bf_port_if.h>
#include <port_mgr/port_mgr_tof3/aw_lane_cfg.h>
#include <port_mgr/bf_tof3_serdes_if.h>
#include <port_mgr/port_mgr_ucli.h>

#include <tofino/bf_pal/bf_pal_port_intf.h>
#include <dvm/bf_drv_intf.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include <bf_pm/bf_pm_intf.h>
#include <dvm/bf_drv_intf.h>
#include <target-utils/uCli/ucli.h>
#include <target-utils/uCli/ucli_argparse.h>
#include <target-utils/uCli/ucli_handler_macros.h>
#include <tof3_regs/tof3_reg_drv.h>
#include <lld/lld_reg_if.h>

#include "bf_pm.h"
#include "bf_pm_tof3_ucli.h"
#include "pm_log.h"

extern int bf_tof3_serdes_read_status2(uint32_t dev_id,
                                       uint32_t dev_port,
                                       uint32_t ln,
                                       uint32_t br);
extern bf_status_t bf_map_logical_tmac_to_physical(bf_dev_id_t dev_id,
                                                   bf_subdev_id_t *subdev_id,
                                                   uint32_t logical_tmac,
                                                   uint32_t *physical_tmac);
extern char *bf_tof3_serdes_tech_ability_to_str(uint32_t tech_ability);

extern void port_mgr_dump_uptime(ucli_context_t *uc,
                                 bf_dev_id_t dev_id,
                                 bf_dev_port_t dev_port);
static void pm_ucli_pcs3_banner(ucli_context_t *uc) {
  char *lastup[] = {"Last UP Time               ",
                    "---------------------------"};
  char *lastdn[] = {"Last DWN Time               ",
                    "----------------------------"};
  char *uptime[] = {"Link uptime  ", "-------------"};
  aim_printf(&uc->pvs,
             "-----------------------------------------------------------------"
             "-------------------------------------+%s+%s+%s\n",
             lastup[1],
             lastdn[1],
             uptime[1]);
  aim_printf(&uc->pvs,
             "PORT |MAC   |D_P|P/PT |"
             "LNKUP|TxRDY|RxRDY|RxSTS|"
             "BlkLK|AMLK|AMOK|ALGN|"
             "LFLT|RFLT|HISER|HIBER|"
             "DGRRE|DGRLO |%s|%s|%s\n",
             lastup[0],
             lastdn[0],
             uptime[0]);
  aim_printf(&uc->pvs,
             "-----------------------------------------------------------------"
             "-------------------------------------+%s+%s+%s\n",
             lastup[1],
             lastdn[1],
             uptime[1]);
}

void pm_ucli_tof3_pcs_status(ucli_context_t *uc,
                             bf_dev_id_t dev_id,
                             int aflag,
                             bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  uint32_t phy_pipe, log_pipe;
  uint32_t print_count = 0;
  int ret;
  bf_dev_id_t dev_id_ret = 0;

  next_port_info_ptr = &next_port_info;

  pm_ucli_pcs3_banner(uc);

  ret = pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_ret);
  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_ret)) {
    // conn=-1 and/or chnl=-1 is a wild card show
    if (((int)port_hdl->conn_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.conn_id !=
          port_hdl->conn_id))) {
      continue;
    }
    if (((int)port_hdl->chnl_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id !=
          port_hdl->chnl_id))) {
      continue;
    }
    // conn=-1 and/or chnl=-1 is a wild card show
    if (((int)port_hdl->conn_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.conn_id !=
          port_hdl->conn_id))) {
      continue;
    }
    if (((int)port_hdl->chnl_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id !=
          port_hdl->chnl_id))) {
      continue;
    }

    log_pipe = DEV_PORT_TO_PIPE(next_port_info_ptr->dev_port);
    if (lld_sku_map_pipe_id_to_phy_pipe_id(dev_id, log_pipe, &phy_pipe) != 0) {
      PM_ERROR(
          "Unable to get phy pipe id from log pipe id for dev %d : log pipe id "
          ": %d",
          dev_id,
          log_pipe);
      return;
    }
    if (next_port_info_ptr->is_added) {
      bf_dev_port_t dev_port = next_port_info_ptr->dev_port;
      bf_tof3_pcs_status_t pcs;
      bf_status_t rc;

      rc = bf_port_tof3_pcs_status_get(dev_id, dev_port, &pcs);
      if (rc != BF_SUCCESS) {
        continue;
      }
      aim_printf(&uc->pvs,
                 "%-5s|%-2d/%-1d  |%-3d|%-1d/%-2d ",
                 next_port_info_ptr->pltfm_port_info.port_str,
                 (int)next_port_info_ptr->pltfm_port_info.log_mac_id,
                 (int)next_port_info_ptr->pltfm_port_info.log_mac_lane,
                 (int)next_port_info_ptr->dev_port,
                 (int)phy_pipe,
                 (int)DEV_PORT_TO_LOCAL_PORT(next_port_info_ptr->dev_port));
      aim_printf(&uc->pvs,
                 "|%-5d|%-5d|%-5d|%-5d"
                 "|%-5d|%-4d|%-4d|%-4d"
                 "|%-4d|%-4d|%-5d|%-5d"
                 "|%-5d|%-5d|",
                 pcs.up,
                 pcs.tx_rdy,
                 pcs.rx_rdy,
                 pcs.rx_status,
                 pcs.block_lock_all,
                 pcs.am_lock_all,
                 pcs.am_map_ok,
                 pcs.align_status,
                 pcs.rs_rx_fault_local,
                 pcs.rs_rx_fault_remote,
                 pcs.hi_ser,
                 pcs.hi_ber,
                 pcs.degraded_remote,
                 pcs.degraded_local);

      port_mgr_dump_uptime(uc, dev_id, dev_port);
      aim_printf(&uc->pvs, "\n");
      print_count++;
    } else {
    }
    // print the banner
    if (print_count >= 32) {
      pm_ucli_pcs3_banner(uc);
      print_count = 0;
    }
  }
  (void)aflag;
}

// Note: Merge with common port-pcs command alongwith multi-dev chnages - TBD
static ucli_status_t bf_pm_tof3_ucli_port_pcs__(ucli_context_t *uc) {
  static char usage[] = "port-pcs-tof3 -a <conn_id/chnl>";
  UCLI_COMMAND_INFO(uc, "port-pcs-tof3", -1, "-a <port_str>");

  int aflag;
  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  bf_status_t sts;

  aflag = 0;
  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  if (!bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this device\n");
    return 0;
  }
  int argc = (uc->pargs->count + 1);
  if (argc > 1) {
    if (argv[1][0] == '-' && argv[1][1] == 'a') {
      aflag = 1;
    } else {
      sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], &port_hdl);
      if (sts != BF_SUCCESS) {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
    }
  }

  // conn==-1 and chnl==-1 indicates that scan through all the ports but only
  // print the ports that have been added.
  //-a option indicates that the scan and print all the ports (whether added or
  // not)

  pm_ucli_tof3_pcs_status(uc, dev_id, aflag, &port_hdl);
  return 0;
}

static void pm_ucli_fec3_banner(ucli_context_t *uc) {
  aim_printf(
      &uc->pvs,
      "-----+------+---+-----+-----+----------+---------+--------+-------+---"
      "---+------+------+------+------+------+------+------+------+-----"
      "-+------+------+\n");
  aim_printf(
      &uc->pvs,
      "PORT |MAC   |D_P|P/PT |uncor| 0        | 1       | 2      | 3     | 4  "
      "  | 5    | 6    | 7    | 8    | 9    | 10   | 11   | 12   | 13   "
      "| 14   | 15   |\n");
  aim_printf(
      &uc->pvs,
      "-----+------+---+-----+-----+----------+---------+--------+-------+---"
      "---+------+------+------+------+------+------+------+------+-----"
      "-+------+------+\n");
}

void pm_ucli_tof3_fec_status(ucli_context_t *uc,
                             bf_dev_id_t dev_id,
                             int aflag,
                             bf_pal_front_port_handle_t *port_hdl,
                             int int_time_ms) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  uint32_t phy_pipe, log_pipe;
  uint32_t print_count = 0;
  int ret;
  bf_dev_id_t dev_id_ret = 0;

  next_port_info_ptr = &next_port_info;

  aim_printf(&uc->pvs, "Integration Time: %dms\n", int_time_ms);

  pm_ucli_fec3_banner(uc);

  ret = pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_ret);
  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_ret)) {
    // conn=-1 and/or chnl=-1 is a wild card show
    if (((int)port_hdl->conn_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.conn_id !=
          port_hdl->conn_id))) {
      continue;
    }
    if (((int)port_hdl->chnl_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id !=
          port_hdl->chnl_id))) {
      continue;
    }
    // conn=-1 and/or chnl=-1 is a wild card show
    if (((int)port_hdl->conn_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.conn_id !=
          port_hdl->conn_id))) {
      continue;
    }
    if (((int)port_hdl->chnl_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id !=
          port_hdl->chnl_id))) {
      continue;
    }

    log_pipe = DEV_PORT_TO_PIPE(next_port_info_ptr->dev_port);
    if (lld_sku_map_pipe_id_to_phy_pipe_id(dev_id, log_pipe, &phy_pipe) != 0) {
      PM_ERROR(
          "Unable to get phy pipe id from log pipe id for dev %d : log pipe id "
          ": %d",
          dev_id,
          log_pipe);
      return;
    }
    if (next_port_info_ptr->is_added) {
      bf_dev_port_t dev_port = next_port_info_ptr->dev_port;
      bf_tof3_fec_status_t fec;
      bf_status_t rc;

      if (int_time_ms != 0) {
        // read first to clear counts
        bf_port_tof3_fec_status_get(dev_id, dev_port, &fec);
      }
      // delay fixed amount of time
      bf_sys_usleep(int_time_ms * 1000);
      // now read counts to display
      rc = bf_port_tof3_fec_status_get(dev_id, dev_port, &fec);
      if (rc != BF_SUCCESS) {
        continue;
      }
      bool hi_ser;
      bool fec_align_status;
      uint32_t fec_corr_cnt;
      uint32_t fec_uncorr_cnt;
      uint32_t fec_ser_lane_0;
      uint32_t fec_ser_lane_1;
      uint32_t fec_ser_lane_2;
      uint32_t fec_ser_lane_3;
      uint32_t fec_ser_lane_4;
      uint32_t fec_ser_lane_5;
      uint32_t fec_ser_lane_6;
      uint32_t fec_ser_lane_7;

      bf_port_rs_fec_status_and_counters_get(dev_id,
                                             dev_port,
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
      aim_printf(&uc->pvs,
                 "%-5s|%-2d/%-1d  |%-3d|%-1d/%-2d |",
                 next_port_info_ptr->pltfm_port_info.port_str,
                 (int)next_port_info_ptr->pltfm_port_info.log_mac_id,
                 (int)next_port_info_ptr->pltfm_port_info.log_mac_lane,
                 (int)next_port_info_ptr->dev_port,
                 (int)phy_pipe,
                 (int)DEV_PORT_TO_LOCAL_PORT(next_port_info_ptr->dev_port));
      aim_printf(&uc->pvs,
                 "%-5d|%-10d|%-9d|%-8d|%-7d|%-6d|%-6d|%-6d|%-6d|"
                 "%-6d|%-6d|%-6d|%-6d|%-6d|%-6d|%-6d|%-6d|\n",
                 fec_uncorr_cnt,
                 fec.cnt[0],
                 fec.cnt[1],
                 fec.cnt[2],
                 fec.cnt[3],
                 fec.cnt[4],
                 fec.cnt[5],
                 fec.cnt[6],
                 fec.cnt[7],
                 fec.cnt[8],
                 fec.cnt[9],
                 fec.cnt[10],
                 fec.cnt[11],
                 fec.cnt[12],
                 fec.cnt[13],
                 fec.cnt[14],
                 fec.cnt[15]);

      print_count++;
    } else {
    }
    // print the banner
    if (print_count >= 32) {
      pm_ucli_fec3_banner(uc);
      print_count = 0;
    }
  }
  (void)aflag;
}

static ucli_status_t bf_pm_tof3_ucli_port_fec__(ucli_context_t *uc) {
  static char usage[] = "port-fec-tail -a <conn_id/chnl>";
  UCLI_COMMAND_INFO(uc, "port-fec-tail", -1, "-a <port_str>");

  int aflag, int_time_ms = 100;
  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  bf_status_t sts;

  aflag = 0;
  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof2(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this device\n");
    return 0;
  }
  int argc = (uc->pargs->count + 1);
  if (argc > 1) {
    if (argv[1][0] == '-' && argv[1][1] == 'a') {
      aflag = 1;
    } else {
      sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], &port_hdl);
      if (sts != BF_SUCCESS) {
        // see if its an integration time
        int_time_ms = strtoul(argv[1], NULL, 10);
        if (int_time_ms > 10 * 1000) {
          aim_printf(&uc->pvs, "Usage : %s\n", usage);
          return 0;
        } else {
          port_hdl.conn_id = -1;
          port_hdl.chnl_id = -1;
        }
      }
      if (argc > 2) {
        // check for integration time on the end of the cmd, e.g. '.. port-name
        // 1000'
        int_time_ms = strtoul(argv[2], NULL, 10);
        if (int_time_ms > 10 * 1000) {
          int_time_ms = 100;  // default
        }
      }
    }
  }

  // conn==-1 and chnl==-1 indicates that scan through all the ports but only
  // print the ports that have been added.
  //-a option indicates that the scan and print all the ports (whether added or
  // not)

  pm_ucli_tof3_fec_status(uc, dev_id, aflag, &port_hdl, int_time_ms);
  return 0;
}

static void pm_ucli_fec3_mon_banner(ucli_context_t *uc) {
  aim_printf(
      &uc->pvs,
      "-----+------+---+-----+---+----------+---------+--------+-------+---\n");
  aim_printf(&uc->pvs, "PORT |MAC   |D_P|P/PT |unc| corr\n");
  aim_printf(
      &uc->pvs,
      "-----+------+---+-----+---+----------+---------+--------+-------+---\n");
}

bool pm_ucli_tof3_fec_mon_status(ucli_context_t *uc,
                                 bf_dev_id_t dev_id,
                                 int aflag,
                                 bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  uint32_t phy_pipe, log_pipe;
  uint32_t print_count = 0;
  int ret;
  bf_dev_id_t dev_id_ret = 0;
  bool found_one = false;
  next_port_info_ptr = &next_port_info;

  ret = pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_ret);
  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_ret)) {
    // conn=-1 and/or chnl=-1 is a wild card show
    if (((int)port_hdl->conn_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.conn_id !=
          port_hdl->conn_id))) {
      continue;
    }
    if (((int)port_hdl->chnl_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id !=
          port_hdl->chnl_id))) {
      continue;
    }
    // conn=-1 and/or chnl=-1 is a wild card show
    if (((int)port_hdl->conn_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.conn_id !=
          port_hdl->conn_id))) {
      continue;
    }
    if (((int)port_hdl->chnl_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id !=
          port_hdl->chnl_id))) {
      continue;
    }

    log_pipe = DEV_PORT_TO_PIPE(next_port_info_ptr->dev_port);
    if (lld_sku_map_pipe_id_to_phy_pipe_id(dev_id, log_pipe, &phy_pipe) != 0) {
      PM_ERROR(
          "Unable to get phy pipe id from log pipe id for dev %d : log pipe id "
          ": %d",
          dev_id,
          log_pipe);
      return 0;
    }
    if (next_port_info_ptr->is_added) {
      bf_dev_port_t dev_port = next_port_info_ptr->dev_port;
      bool hi_ser;
      bool fec_align_status;
      uint32_t fec_corr_cnt;
      uint32_t fec_uncorr_cnt;
      uint32_t fec_ser_lane_0;
      uint32_t fec_ser_lane_1;
      uint32_t fec_ser_lane_2;
      uint32_t fec_ser_lane_3;
      uint32_t fec_ser_lane_4;
      uint32_t fec_ser_lane_5;
      uint32_t fec_ser_lane_6;
      uint32_t fec_ser_lane_7;

      bf_port_rs_fec_status_and_counters_get(dev_id,
                                             dev_port,
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
      // count is clear-on-read so non-0 means new one
      if (fec_uncorr_cnt) {
        found_one = true;
        aim_printf(&uc->pvs,
                   "%-5s|%-2d/%-1d  |%-3d|%-1d/%-2d |%3d|%d\n",
                   next_port_info_ptr->pltfm_port_info.port_str,
                   (int)next_port_info_ptr->pltfm_port_info.log_mac_id,
                   (int)next_port_info_ptr->pltfm_port_info.log_mac_lane,
                   (int)next_port_info_ptr->dev_port,
                   (int)phy_pipe,
                   (int)DEV_PORT_TO_LOCAL_PORT(next_port_info_ptr->dev_port),
                   fec_uncorr_cnt,
                   fec_corr_cnt);
      }
      print_count++;
    } else {
    }
    // print the banner
    if (print_count >= 32) {
      pm_ucli_fec3_mon_banner(uc);
      print_count = 0;
    }
  }
  (void)aflag;
  return found_one;
}

static ucli_status_t bf_pm_tof3_ucli_port_fec_mon__(ucli_context_t *uc) {
  static char usage[] = "port-fec-mon -a <conn_id/chnl>";
  UCLI_COMMAND_INFO(uc, "port-fec-mon", -1, "-a <port_str>");

  int aflag, int_time_ms = 100;
  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  bf_status_t sts;

  aflag = 0;
  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof2(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this device\n");
    return 0;
  }
  int argc = (uc->pargs->count + 1);
  if (argc > 1) {
    if (argv[1][0] == '-' && argv[1][1] == 'a') {
      aflag = 1;
    } else {
      sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], &port_hdl);
      if (sts != BF_SUCCESS) {
        // see if its an integration time
        int_time_ms = strtoul(argv[1], NULL, 10);
        if (int_time_ms > 10 * 1000000) {
          aim_printf(&uc->pvs, "Usage : %s\n", usage);
          return 0;
        } else {
          port_hdl.conn_id = -1;
          port_hdl.chnl_id = -1;
        }
      }
    }
  }

  // conn==-1 and chnl==-1 indicates that scan through all the ports but only
  // print the ports that have been added.
  //-a option indicates that the scan and print all the ports (whether added or
  // not)
  aim_printf(&uc->pvs, "Integration Time: %dms\n", int_time_ms);

  pm_ucli_fec3_mon_banner(uc);

  for (int n = 0; n < int_time_ms / 10; n++) {
    bool found_uncorr;

    found_uncorr = pm_ucli_tof3_fec_mon_status(uc, dev_id, aflag, &port_hdl);
    if (found_uncorr) {
      aim_printf(&uc->pvs,
                 "-----+------+---+-----+---+------- Time: %d.%d sec\n",
                 (n / 100),
                 (n % 100));
    }
    bf_sys_usleep(10000);
  }
  return 0;
}

static void pm_ucli_int3_banner(ucli_context_t *uc) {
  aim_printf(&uc->pvs,
             "rr dev_0-0 serdes serdes1 serdes_glue_regs anlt_intr stat\n");
  aim_printf(&uc->pvs,
             "rr dev_0-0 serdes serdes1 serdes_glue_regs sds_intr stat\n");
  aim_printf(&uc->pvs, "rr dev_0-0 eth400g[1] eth400g_mac mem_intr stat\n");
  aim_printf(&uc->pvs, "rr dev_0-0 eth400g[1] eth400g_mac link_intr stat\n");
  aim_printf(&uc->pvs, "rr dev_0-0 eth400g[1] eth400g_mac mem_intr stat\n");
  aim_printf(&uc->pvs, "rr dev_0-0 eth400g[1] eth400g_sys rx_intr stat\n");
  aim_printf(&uc->pvs, "rr dev_0-0 eth400g[1] eth400g_sys tx_intr stat\n");
  aim_printf(&uc->pvs, "rr dev_0-0 eth400g[1] eth400g_sys cts_intr stat\n");
  aim_printf(&uc->pvs, "rr dev_0-0 eth400g[1] eth400g_sys err_ptp_intr stat\n");
  aim_printf(&uc->pvs, "rr dev_0-0 eth400g[1] eth400g_app chnl_intr stat\n");
  aim_printf(&uc->pvs, "rr dev_0-0 eth400g[1] eth400g_app mem_intr\n");
  aim_printf(&uc->pvs,
             "rr dev_0-0 eth400g[1] eth400g_app err_icrc_intr stat\n");

  aim_printf(&uc->pvs, "\n");
  aim_printf(&uc->pvs,
             "                       Serdes glue            MAC              "
             "Sys             "
             "    App\n");
  aim_printf(&uc->pvs,
             "PORT |MAC   |D_P|P/PT |   ANLT     serdes     mem   link    rx   "
             "  tx    cts "
             "ptp   chnl  mem err_icrc\n");
  aim_printf(&uc->pvs,
             "-----+------+---+-----+----------+--------+-------+------+------+"
             "------+----+-"
             "---+------+----+-----+\n");
}

void pm_ucli_tof3_int_status(ucli_context_t *uc,
                             bf_dev_id_t dev_id,
                             int aflag,
                             bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  uint32_t phy_pipe, log_pipe;
  uint32_t print_count = 0;
  int ret;
  bf_dev_id_t dev_id_ret = 0;

  next_port_info_ptr = &next_port_info;

  ret = pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_ret);
  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_ret)) {
    // conn=-1 and/or chnl=-1 is a wild card show
    if (((int)port_hdl->conn_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.conn_id !=
          port_hdl->conn_id))) {
      continue;
    }
    if (((int)port_hdl->chnl_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id !=
          port_hdl->chnl_id))) {
      continue;
    }
    // conn=-1 and/or chnl=-1 is a wild card show
    if (((int)port_hdl->conn_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.conn_id !=
          port_hdl->conn_id))) {
      continue;
    }
    if (((int)port_hdl->chnl_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id !=
          port_hdl->chnl_id))) {
      continue;
    }

    log_pipe = DEV_PORT_TO_PIPE(next_port_info_ptr->dev_port);
    if (lld_sku_map_pipe_id_to_phy_pipe_id(dev_id, log_pipe, &phy_pipe) != 0) {
      PM_ERROR(
          "Unable to get phy pipe id from log pipe id for dev %d : log pipe id "
          ": %d",
          dev_id,
          log_pipe);
      return;
    }
    if (next_port_info_ptr->is_added) {
      bf_status_t rc;
      bf_subdev_id_t subdev_id;
      uint32_t tmac, mac = next_port_info_ptr->pltfm_port_info.log_mac_id;

      rc = bf_map_logical_tmac_to_physical(dev_id, &subdev_id, mac, &tmac);
      if (rc != BF_SUCCESS) return;
      if (tmac >= 33) return;

      // read MAC and serdes interrupt regs
      uint32_t addr, data;
      uint32_t chnl, err_icrc, app_mem, cts, err_ptp, rx, tx, link, mac_mem,
          sds, anlt;

      // app intrs
      addr = offsetof(tof3_reg, eth400g[tmac].eth400g_app.chnl_intr.stat);
      lld_read_register(dev_id, addr, &data);
      chnl = data;

      addr = offsetof(tof3_reg, eth400g[tmac].eth400g_app.err_icrc_intr.stat);
      lld_read_register(dev_id, addr, &data);
      err_icrc = data;

      addr = offsetof(tof3_reg, eth400g[tmac].eth400g_app.mem_intr.stat);
      lld_read_register(dev_id, addr, &data);
      app_mem = data;

      // addr = offsetof(tof3_reg, eth400g[tmac].eth400g_app.uctrl_intr.stat);
      // lld_read_register(dev_id, addr, &data);

      // sys intrs
      addr = offsetof(tof3_reg, eth400g[tmac].eth400g_sys.cts_intr.stat);
      lld_read_register(dev_id, addr, &data);
      cts = data;

      addr = offsetof(tof3_reg, eth400g[tmac].eth400g_sys.err_ptp_intr.stat);
      lld_read_register(dev_id, addr, &data);
      err_ptp = data;

      addr = offsetof(tof3_reg, eth400g[tmac].eth400g_sys.rx_intr.stat);
      lld_read_register(dev_id, addr, &data);
      rx = data;

      addr = offsetof(tof3_reg, eth400g[tmac].eth400g_sys.tx_intr.stat);
      lld_read_register(dev_id, addr, &data);
      tx = data;

      // mac intrs
      addr = offsetof(tof3_reg, eth400g[tmac].eth400g_mac.link_intr.stat);
      lld_read_register(dev_id, addr, &data);
      link = data;

      addr = offsetof(tof3_reg, eth400g[tmac].eth400g_mac.mem_intr.stat);
      lld_read_register(dev_id, addr, &data);
      mac_mem = data;

      if (tmac == 0) {  // cpu port
        // serdes glue
        addr = offsetof(tof3_reg,
                        serdes.serdes0.serdes4ln_glue_regs.sds_intr.stat);
        lld_read_register(dev_id, addr, &data);
        sds = data;

        addr = offsetof(tof3_reg,
                        serdes.serdes0.serdes4ln_glue_regs.anlt_intr.stat);
        lld_read_register(dev_id, addr, &data);
        anlt = data;
      } else {
        uint32_t stride = offsetof(tof3_reg, serdes.serdes2) -
                          offsetof(tof3_reg, serdes.serdes1);

        // serdes glue
        addr =
            offsetof(tof3_reg, serdes.serdes1.serdes_glue_regs.sds_intr.stat);
        addr += (stride * (tmac - 1));
        lld_read_register(dev_id, addr, &data);
        sds = data;

        addr =
            offsetof(tof3_reg, serdes.serdes1.serdes_glue_regs.anlt_intr.stat);
        addr += (stride * (tmac - 1));
        lld_read_register(dev_id, addr, &data);
        anlt = data;
      }
      // print the banner
      if ((print_count % 33) == 0) {
        pm_ucli_int3_banner(uc);
        print_count = 1;
      }
      print_count++;

      // uint32_t chnl-16, err_icrc-12, app_mem-8, cts-8, err_ptp-4, rx-16,
      // tx-16, link-16, mac_mem-14, sds-18;
      aim_printf(&uc->pvs,
                 "%-5s|%-2d/%-1d  |%-3d|%-1d/%-2d |",
                 next_port_info_ptr->pltfm_port_info.port_str,
                 (int)next_port_info_ptr->pltfm_port_info.log_mac_id,
                 (int)next_port_info_ptr->pltfm_port_info.log_mac_lane,
                 (int)next_port_info_ptr->dev_port,
                 (int)phy_pipe,
                 (int)DEV_PORT_TO_LOCAL_PORT(next_port_info_ptr->dev_port));

      //
      aim_printf(&uc->pvs,
                 " %08x |  %5x | %4x  | %4x | %4x | %4x | %2x | %2x | %4x | "
                 "%2x | %3x |\n",
                 anlt,
                 sds,
                 mac_mem,
                 link,
                 rx,
                 tx,
                 cts,
                 err_ptp,
                 chnl,
                 app_mem,
                 err_icrc);

    } else {
    }
  }
  (void)aflag;
}

static ucli_status_t bf_pm_tof3_ucli_port_int__(ucli_context_t *uc) {
  static char usage[] = "port-int-tof3 -a <conn_id/chnl>";
  UCLI_COMMAND_INFO(uc, "port-int-tof3", -1, "-a <port_str>");

  int aflag;
  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  bf_status_t sts;

  aflag = 0;
  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof2(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this device\n");
    return 0;
  }
  int argc = (uc->pargs->count + 1);
  if (argc > 1) {
    if (argv[1][0] == '-' && argv[1][1] == 'a') {
      aflag = 1;
    } else {
      sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], &port_hdl);
      if (sts != BF_SUCCESS) {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
    }
  }

  // conn==-1 and chnl==-1 indicates that scan through all the ports but only
  // print the ports that have been added.
  //-a option indicates that the scan and print all the ports (whether added or
  // not)

  pm_ucli_tof3_int_status(uc, dev_id, aflag, &port_hdl);
  return 0;
}

void pm_ucli_anlt_banner(ucli_context_t *uc) {
  aim_printf(&uc->pvs,
             "-----+------+---+-----+---+---+---+---+---+---+---+---+");
  aim_printf(&uc->pvs,
             "---+----+-----------------+----------------+--------------\n");
  aim_printf(&uc->pvs,
             "PORT |MAC   |D_P|P/PT |    AN  LT  sig lck run flt rdy|");
  aim_printf(&uc->pvs, "frm| fsm|  Local Base pg | LP Base pg\n");
  aim_printf(&uc->pvs,
             "-----+------+---+-----+---+---+---+---+---+---+---+---+");
  aim_printf(&uc->pvs,
             "---+----+-----------------+----------------+--------------\n");
}

void pm_ucli_tof3_anlt_status(ucli_context_t *uc,
                              bf_dev_id_t dev_id,
                              int aflag,
                              bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  uint32_t phy_pipe, log_pipe;
  uint32_t print_count = 0;
  int ret;
  bf_dev_id_t dev_id_ret = 0;

  next_port_info_ptr = &next_port_info;

  ret = pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_ret);
  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_ret)) {
    // conn=-1 and/or chnl=-1 is a wild card show
    if (((int)port_hdl->conn_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.conn_id !=
          port_hdl->conn_id))) {
      continue;
    }
    if (((int)port_hdl->chnl_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id !=
          port_hdl->chnl_id))) {
      continue;
    }
    // conn=-1 and/or chnl=-1 is a wild card show
    if (((int)port_hdl->conn_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.conn_id !=
          port_hdl->conn_id))) {
      continue;
    }
    if (((int)port_hdl->chnl_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id !=
          port_hdl->chnl_id))) {
      continue;
    }

    log_pipe = DEV_PORT_TO_PIPE(next_port_info_ptr->dev_port);
    if (lld_sku_map_pipe_id_to_phy_pipe_id(dev_id, log_pipe, &phy_pipe) != 0) {
      PM_ERROR(
          "Unable to get phy pipe id from log pipe id for dev %d : log pipe id "
          ": %d",
          dev_id,
          log_pipe);
      return;
    }
    if (next_port_info_ptr->is_added) {
      uint32_t n_lanes;
      bool an_done;
      uint32_t pmd_rx_lock[8] = {0};
      uint32_t signal_detect[8] = {0};
      uint32_t lt_running[8] = {0};
      uint32_t lt_done[8] = {0};
      uint32_t lt_training_failure[8] = {0};
      uint32_t lt_rx_ready[8] = {0};
      uint32_t lt_fsm_st[8] = {0};
      uint32_t frame_lock[8] = {0};
      uint32_t an_mr_page_rx, an_result;
      uint64_t base_pg, lp_base_pg, cons_np;
      bf_dev_port_t dev_port = next_port_info_ptr->dev_port;

      bf_port_num_lanes_get(dev_id, dev_port, (int *)&n_lanes);
      bf_tof3_serdes_an_done_get(dev_id, dev_port, 0, &an_done);
      bf_tof3_serdes_an_result_get(
          dev_id, dev_port, &an_mr_page_rx, &an_result, &lp_base_pg);

      bf_serdes_an_loc_pages_get(dev_id, dev_port, BF_AN_PAGE_BASE, &base_pg);
      bf_serdes_an_loc_pages_get(dev_id, dev_port, BF_AN_PAGE_NEXT_2, &cons_np);

      uint32_t bp_15_0 = (uint32_t)((base_pg >> 0ull) & 0xffff);
      uint32_t bp_31_16 = (uint32_t)((base_pg >> 16ull) & 0xffff);
      uint32_t bp_47_32 = (uint32_t)((base_pg >> 32ull) & 0xffff);

      uint32_t lp_bp_15_0 = (uint32_t)((lp_base_pg >> 0ull) & 0xffff);
      uint32_t lp_bp_31_16 = (uint32_t)((lp_base_pg >> 16ull) & 0xffff);
      uint32_t lp_bp_47_32 = (uint32_t)((lp_base_pg >> 32ull) & 0xffff);

      for (uint32_t ln = 0; ln < n_lanes; ln++) {
        // bf_tof3_serdes_lt_done_get(dev_id, dev_port, ln, &lt_done[ln]);
        bf_tof3_serdes_anlt_lane_status_get(
            dev_id, dev_port, ln, &signal_detect[ln], &pmd_rx_lock[ln]);
        bf_tof3_serdes_anlt_link_training_status_get(dev_id,
                                                     dev_port,
                                                     ln,
                                                     &lt_running[ln],
                                                     &lt_done[ln],
                                                     &lt_training_failure[ln],
                                                     &lt_rx_ready[ln]);
        bf_tof3_serdes_lt_info_get(
            dev_id, dev_port, ln, &lt_fsm_st[ln], &frame_lock[ln]);
      }
      // print the banner
      if ((print_count % 9) == 0) {
        pm_ucli_anlt_banner(uc);
        print_count = 1;
      }
      print_count++;

      aim_printf(&uc->pvs,
                 "%-5s|%-2d/%-1d  |%-3d|%-1d/%-2d |",
                 next_port_info_ptr->pltfm_port_info.port_str,
                 (int)next_port_info_ptr->pltfm_port_info.log_mac_id,
                 (int)next_port_info_ptr->pltfm_port_info.log_mac_lane,
                 (int)next_port_info_ptr->dev_port,
                 (int)phy_pipe,
                 (int)DEV_PORT_TO_LOCAL_PORT(next_port_info_ptr->dev_port));

      //
      for (uint32_t ln = 0; ln < n_lanes; ln++) {
        if (ln == 0) {
          aim_printf(&uc->pvs,
                     "ln%d| %1d | %1d | %1d | %1d | %1d | %1d | %1d | %1d | "
                     "%2d |%04x_%04x_%04x | %04x_%04x_%04x | %s\n",
                     ln,
                     an_done ? 1 : 0,
                     lt_done[ln] ? 1 : 0,
                     signal_detect[ln],
                     pmd_rx_lock[ln],
                     lt_running[ln],
                     lt_training_failure[ln],
                     lt_rx_ready[ln],
                     frame_lock[ln],
                     lt_fsm_st[ln],
                     bp_47_32,
                     bp_31_16,
                     bp_15_0,
                     lp_bp_47_32,
                     lp_bp_31_16,
                     lp_bp_15_0,
                     bf_tof3_serdes_tech_ability_to_str(an_result));
        } else {
          aim_printf(&uc->pvs,
                     "                      |ln%d| - | %1d | %1d | %1d | %1d | "
                     "%1d | %1d | %1d | %2d |\n",
                     ln,
                     lt_done[ln] ? 1 : 0,
                     signal_detect[ln],
                     pmd_rx_lock[ln],
                     lt_running[ln],
                     lt_training_failure[ln],
                     lt_rx_ready[ln],
                     frame_lock[ln],
                     lt_fsm_st[ln]);
        }
      }
    }
  }
  (void)aflag;
}

static ucli_status_t bf_pm_tof3_ucli_port_anlt__(ucli_context_t *uc) {
  static char usage[] = "port-anlt-tof3 -a <conn_id/chnl>";
  UCLI_COMMAND_INFO(uc, "port-anlt-tof3", -1, "-a <port_str>");

  int aflag;
  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  bf_status_t sts;

  aflag = 0;
  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof2(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this device\n");
    return 0;
  }
  int argc = (uc->pargs->count + 1);
  if (argc > 1) {
    if (argv[1][0] == '-' && argv[1][1] == 'a') {
      aflag = 1;
    } else {
      sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], &port_hdl);
      if (sts != BF_SUCCESS) {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
    }
  }

  // conn==-1 and chnl==-1 indicates that scan through all the ports but only
  // print the ports that have been added.
  //-a option indicates that the scan and print all the ports (whether added or
  // not)

  pm_ucli_tof3_anlt_status(uc, dev_id, aflag, &port_hdl);
  return 0;
}

/*

                                    &rx_sts__rx_valid[ln],
                                    &anlt_ctrl__link_status[ln],
                                    &anlt_mux__group_master[ln],
                                    &anlt_mux__group_sel[ln],
                                    &anlt_mux__rxsel[ln],
                                    &anlt_stat__an_done[ln],
                                    &anlt_stat__an_fec_ena[ln],
                                    &anlt_stat__an_link_good[ln],
                                    &anlt_stat__an_new_page[ln],
                                    &anlt_stat__an_rsfec_ena[ln],
                                    &anlt_stat__an_tr_disable[ln],
                                    &anlt_stat__an_link_control[ln]);
*/
void pm_ucli_glue_banner(ucli_context_t *uc) {
  aim_printf(&uc->pvs,
             "-----+------+---+-----+---+---+---+---+---+---+---+---+---+---+--"
             "-+---+\n");
  aim_printf(&uc->pvs,
             "PORT |MAC   |D_P|P/PT |rx |lnk|grp|grp|rx |an |fec|lnk|new|rs "
             "|trn|   |\n");
  aim_printf(&uc->pvs,
             "     |      |   |     |vld|sts|mst|sel|sel|dn |ena|gd |pg "
             "|fec|dis|hcd|\n");
  aim_printf(&uc->pvs,
             "-----+------+---+-----+---+---+---+---+---+---+---+---+---+---+--"
             "-+---+\n");
}

void pm_ucli_tof3_glue(ucli_context_t *uc,
                       bf_dev_id_t dev_id,
                       int aflag,
                       bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  uint32_t phy_pipe, log_pipe;
  uint32_t print_count = 0;
  int ret;
  bf_dev_id_t dev_id_ret = 0;

  next_port_info_ptr = &next_port_info;

  ret = pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_ret);
  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_ret)) {
    // conn=-1 and/or chnl=-1 is a wild card show
    if (((int)port_hdl->conn_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.conn_id !=
          port_hdl->conn_id))) {
      continue;
    }
    if (((int)port_hdl->chnl_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id !=
          port_hdl->chnl_id))) {
      continue;
    }
    // conn=-1 and/or chnl=-1 is a wild card show
    if (((int)port_hdl->conn_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.conn_id !=
          port_hdl->conn_id))) {
      continue;
    }
    if (((int)port_hdl->chnl_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id !=
          port_hdl->chnl_id))) {
      continue;
    }

    log_pipe = DEV_PORT_TO_PIPE(next_port_info_ptr->dev_port);
    if (lld_sku_map_pipe_id_to_phy_pipe_id(dev_id, log_pipe, &phy_pipe) != 0) {
      PM_ERROR(
          "Unable to get phy pipe id from log pipe id for dev %d : log pipe id "
          ": %d",
          dev_id,
          log_pipe);
      return;
    }
    if (next_port_info_ptr->is_added) {
      uint32_t rx_sts__rx_valid[8];
      uint32_t anlt_ctrl__link_status[8];
      uint32_t anlt_mux__group_master[8];
      uint32_t anlt_mux__group_sel[8];
      uint32_t anlt_mux__rxsel[8];
      uint32_t anlt_stat__an_done[8];
      uint32_t anlt_stat__an_fec_ena[8];
      uint32_t anlt_stat__an_link_good[8];
      uint32_t anlt_stat__an_new_page[8];
      uint32_t anlt_stat__an_rsfec_ena[8];
      uint32_t anlt_stat__an_tr_disable[8];
      uint32_t anlt_stat__an_link_control[8];
      uint32_t n_lanes;
      bf_dev_port_t dev_port = next_port_info_ptr->dev_port;

      bf_port_num_lanes_get(dev_id, dev_port, (int *)&n_lanes);

      for (uint32_t ln = 0; ln < n_lanes; ln++) {
        bf_tof3_serdes_glue_get(dev_id,
                                dev_port,
                                ln,
                                &rx_sts__rx_valid[ln],
                                &anlt_ctrl__link_status[ln],
                                &anlt_mux__group_master[ln],
                                &anlt_mux__group_sel[ln],
                                &anlt_mux__rxsel[ln],
                                &anlt_stat__an_done[ln],
                                &anlt_stat__an_fec_ena[ln],
                                &anlt_stat__an_link_good[ln],
                                &anlt_stat__an_new_page[ln],
                                &anlt_stat__an_rsfec_ena[ln],
                                &anlt_stat__an_tr_disable[ln],
                                &anlt_stat__an_link_control[ln]);
      }
      // print the banner
      if ((print_count % 9) == 0) {
        pm_ucli_glue_banner(uc);
        print_count = 1;
      }
      print_count++;

      aim_printf(&uc->pvs,
                 "%-5s|%-2d/%-1d  |%-3d|%-1d/%-2d |",
                 next_port_info_ptr->pltfm_port_info.port_str,
                 (int)next_port_info_ptr->pltfm_port_info.log_mac_id,
                 (int)next_port_info_ptr->pltfm_port_info.log_mac_lane,
                 (int)next_port_info_ptr->dev_port,
                 (int)phy_pipe,
                 (int)DEV_PORT_TO_LOCAL_PORT(next_port_info_ptr->dev_port));

      //
      for (uint32_t ln = 0; ln < n_lanes; ln++) {
        if (ln != 0) {
          aim_printf(&uc->pvs, "                      |");
        }
        aim_printf(&uc->pvs,
                   " %1u | %1u | %1u | %1u | %1u | %1u | %1u | %1u | %1u | %1u "
                   "| %1u | %2u |\n",
                   rx_sts__rx_valid[ln],
                   anlt_ctrl__link_status[ln],
                   anlt_mux__group_master[ln],
                   anlt_mux__group_sel[ln],
                   anlt_mux__rxsel[ln],
                   anlt_stat__an_done[ln],
                   anlt_stat__an_fec_ena[ln],
                   anlt_stat__an_link_good[ln],
                   anlt_stat__an_new_page[ln],
                   anlt_stat__an_rsfec_ena[ln],
                   anlt_stat__an_tr_disable[ln],
                   anlt_stat__an_link_control[ln]);
      }
    }
  }
  (void)aflag;
}

static ucli_status_t bf_pm_tof3_ucli_port_glue__(ucli_context_t *uc) {
  static char usage[] = "port-glue-tof3 -a <conn_id/chnl>";
  UCLI_COMMAND_INFO(uc, "port-glue-tof3", -1, "-a <port_str>");

  int aflag;
  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  bf_status_t sts;

  aflag = 0;
  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof2(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this device\n");
    return 0;
  }
  int argc = (uc->pargs->count + 1);
  if (argc > 1) {
    if (argv[1][0] == '-' && argv[1][1] == 'a') {
      aflag = 1;
    } else {
      sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], &port_hdl);
      if (sts != BF_SUCCESS) {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
    }
  }

  // conn==-1 and chnl==-1 indicates that scan through all the ports but only
  // print the ports that have been added.
  //-a option indicates that the scan and print all the ports (whether added or
  // not)

  pm_ucli_tof3_glue(uc, dev_id, aflag, &port_hdl);
  return 0;
}

void pm_ucli_tof3_serdes_debug(ucli_context_t *uc,
                               bf_dev_id_t dev_id,
                               int aflag,
                               bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  uint32_t phy_pipe, log_pipe;
  int ret;
  bf_dev_id_t dev_id_ret = 0;
  (void)uc;

  next_port_info_ptr = &next_port_info;

  ret = pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_ret);
  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_ret)) {
    // conn=-1 and/or chnl=-1 is a wild card show
    if (((int)port_hdl->conn_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.conn_id !=
          port_hdl->conn_id))) {
      continue;
    }
    if (((int)port_hdl->chnl_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id !=
          port_hdl->chnl_id))) {
      continue;
    }
    // conn=-1 and/or chnl=-1 is a wild card show
    if (((int)port_hdl->conn_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.conn_id !=
          port_hdl->conn_id))) {
      continue;
    }
    if (((int)port_hdl->chnl_id != -1) &&
        ((next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id !=
          port_hdl->chnl_id))) {
      continue;
    }

    log_pipe = DEV_PORT_TO_PIPE(next_port_info_ptr->dev_port);
    if (lld_sku_map_pipe_id_to_phy_pipe_id(dev_id, log_pipe, &phy_pipe) != 0) {
      PM_ERROR(
          "Unable to get phy pipe id from log pipe id for dev %d : log pipe id "
          ": %d",
          dev_id,
          log_pipe);
      return;
    }
    if (next_port_info_ptr->is_added) {
      uint32_t n_lanes;
      bf_dev_port_t dev_port = next_port_info_ptr->dev_port;

      bf_port_num_lanes_get(dev_id, dev_port, (int *)&n_lanes);

      for (uint32_t ln = 0; ln < n_lanes; ln++) {
        bf_tof3_serdes_read_status2(dev_id, dev_port, ln, 0);
      }
    }
  }
  (void)aflag;
}

static ucli_status_t bf_pm_tof3_ucli_port_serdes_debug__(ucli_context_t *uc) {
  static char usage[] = "port-sd-dbg-tof3 -a <conn_id/chnl>";
  UCLI_COMMAND_INFO(uc, "port-sd-dbg-tof3", -1, "-a <port_str>");

  int aflag;
  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  bf_status_t sts;

  aflag = 0;
  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof2(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this device\n");
    return 0;
  }
  int argc = (uc->pargs->count + 1);
  if (argc > 1) {
    if (argv[1][0] == '-' && argv[1][1] == 'a') {
      aflag = 1;
    } else {
      sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], &port_hdl);
      if (sts != BF_SUCCESS) {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
    }
  }

  // conn==-1 and chnl==-1 indicates that scan through all the ports but only
  // print the ports that have been added.
  //-a option indicates that the scan and print all the ports (whether added or
  // not)

  pm_ucli_tof3_serdes_debug(uc, dev_id, aflag, &port_hdl);
  return 0;
}

ucli_status_t bf_pm_ucli_ucli__tof3_port_pcs__(ucli_context_t *uc) {
  return bf_pm_tof3_ucli_port_pcs__(uc);
}

ucli_status_t bf_pm_ucli_ucli__tof3_port_fec__(ucli_context_t *uc) {
  return bf_pm_tof3_ucli_port_fec__(uc);
}

ucli_status_t bf_pm_ucli_ucli__tof3_port_fec_mon__(ucli_context_t *uc) {
  return bf_pm_tof3_ucli_port_fec_mon__(uc);
}

ucli_status_t bf_pm_ucli_ucli__tof3_port_int__(ucli_context_t *uc) {
  return bf_pm_tof3_ucli_port_int__(uc);
}

ucli_status_t bf_pm_ucli_ucli__tof3_port_anlt__(ucli_context_t *uc) {
  return bf_pm_tof3_ucli_port_anlt__(uc);
}

ucli_status_t bf_pm_ucli_ucli__tof3_port_glue__(ucli_context_t *uc) {
  return bf_pm_tof3_ucli_port_glue__(uc);
}

ucli_status_t bf_pm_ucli_ucli__tof3_port_serdes_debug__(ucli_context_t *uc) {
  return bf_pm_tof3_ucli_port_serdes_debug__(uc);
}

ucli_status_t bf_pm_ucli_ucli__tof3_command_start__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(
      uc,
      "\ncommon_tof3_start",
      0,
      "---------- Start of tof3-specific ucli commands -------\n");
  return 0;
}
