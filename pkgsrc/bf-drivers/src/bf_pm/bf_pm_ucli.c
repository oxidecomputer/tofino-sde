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
#include <math.h>
#include <sys/time.h>
#include <unistd.h>

#include <bf_types/bf_types.h>
#include <lld/bf_lld_if.h>
#include <lld/lld_err.h>
#include <lld/lld_sku.h>
#include <port_mgr/bf_port_if.h>
#include <port_mgr/bf_serdes_if.h>
#include <port_mgr/port_mgr_tof3/aw_lane_cfg.h>
#include <port_mgr/bf_tof3_serdes_if.h>
#include <port_mgr/port_mgr_ucli.h>

#include <tofino/bf_pal/bf_pal_port_intf.h>
#include <dvm/bf_drv_intf.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include <bf_pm/bf_pm_intf.h>
#include <target-utils/uCli/ucli.h>
#include <target-utils/uCli/ucli_argparse.h>
#include <target-utils/uCli/ucli_handler_macros.h>
#include <port_mgr/port_mgr_tof2/umac4_ctrs.h>
#include <lld/lld_dev.h>

#include "bf_pm.h"
#include "pm_log.h"
#include "bf_pm_tof3_ucli.h"

bf_status_t port_mgr_tof2_map_dev_port_to_all(bf_dev_id_t dev_id,
                                              bf_dev_port_t dev_port,
                                              uint32_t *pipe_id,
                                              uint32_t *port_id,
                                              uint32_t *umac,
                                              uint32_t *ch,
                                              bool *is_cpu_port);
bf_status_t port_mgr_tof3_map_dev_port_to_all(bf_dev_id_t dev_id,
                                              bf_dev_port_t dev_port,
                                              uint32_t *pipe_id,
                                              uint32_t *port_id,
                                              uint32_t *tmac,
                                              uint32_t *ch,
                                              bool *is_cpu_port);
extern void umac4_ctrs_rs_fec_ln_get(bf_dev_id_t dev_id,
                                     uint32_t umac,
                                     umac4_rs_fec_ln_ctr_t *ctrs);
extern uint32_t umac4_ctrs_rs_fec_ctr_address_get(bf_dev_id_t dev_id,
                                                  uint32_t umac,
                                                  uint32_t ch,
                                                  umac4_rs_fec_ln_ctr_e ctr);
extern uint32_t umac4_ctrs_rs_fec_ln_ctr_address_get(bf_dev_id_t dev_id,
                                                     uint32_t umac,
                                                     umac4_rs_fec_ln_ctr_e ctr);

extern bf_pm_port_info_t *pm_port_info_get(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port);
extern void pm_tasklet_free_run_set(bool st);
extern void pm_tasklet_single_step_set(void);

extern void port_mgr_dump_uptime(ucli_context_t *uc,
                                 bf_dev_id_t dev_id,
                                 bf_dev_port_t dev_port);

static void pm_ucli_speed_to_display_get(bf_port_speed_t speed,
                                         char *str,
                                         int len,
                                         uint32_t n_lanes) {
  switch (speed) {
    case BF_SPEED_NONE:
      strcpy(str, "NONE   ");
      break;
    case BF_SPEED_1G:
      strcpy(str, "1G     ");
      break;
    case BF_SPEED_10G:
      strcpy(str, "10G    ");
      break;
    case BF_SPEED_25G:
      strcpy(str, "25G    ");
      break;
    case BF_SPEED_50G:
      if (n_lanes == 1) {
        strcpy(str, "50G-R1 ");
      } else if (n_lanes == 2) {
        strcpy(str, "50G-R2 ");
      } else {
        strcpy(str, "50G    ");
      }
      break;
    case BF_SPEED_50G_CONS:
      strcpy(str, "50G-CNS");
      break;
    case BF_SPEED_100G:
      if (n_lanes == 1) {
        strcpy(str, "100G-R1");
      } else if (n_lanes == 2) {
        strcpy(str, "100G-R2");
      } else {
        strcpy(str, "100G   ");
      }
      break;
    case BF_SPEED_200G:
      if (n_lanes == 2) {
        strcpy(str, "200G-R2");
      } else if (n_lanes == 4) {
        strcpy(str, "200G-R4");
      } else if (n_lanes == 8) {
        strcpy(str, "200G-R8");
      } else {
        strcpy(str, "200G   ");
      }
      break;
    case BF_SPEED_400G:
      if (n_lanes == 4) {
        strcpy(str, "400G-R4");
      } else {
        strcpy(str, "400G   ");
      }
      break;
    case BF_SPEED_40G:
      strcpy(str, "40G    ");
      break;
    case BF_SPEED_40G_R2:
      strcpy(str, "40G-R2 ");
      break;
    default:
      strcpy(str, "-------");
  }
  str[len - 1] = '\0';
}

static uint32_t pm_util_get_port_rate(bf_port_speed_t speed) {
  switch (speed) {
    case BF_SPEED_NONE:
      return 0;
    case BF_SPEED_1G:
      return 1;
    case BF_SPEED_10G:
      return 10;
    case BF_SPEED_25G:
      return 25;
    case BF_SPEED_50G:
    case BF_SPEED_50G_CONS:
      return 50;
    case BF_SPEED_100G:
      return 100;
    case BF_SPEED_200G:
      return 200;
    case BF_SPEED_400G:
      return 400;
    case BF_SPEED_40G:
    case BF_SPEED_40G_R2:
      return 40;
    default:
      return 0;
  }
}

static uint64_t pm_util_aggregate_bit_rate_get(bf_port_speed_t speed,
                                               uint32_t num_lanes) {
  uint64_t gb, bps;
  uint64_t serdes_clk_nrz_1g = 1031250;
  uint64_t serdes_clk_nrz_10g = 10312500;
  uint64_t serdes_clk_nrz = 25781250;
  uint64_t serdes_clk_pam4 = 53125000;
  uint64_t serdes_clk_pam4_112g = 2 * 53125000;
  uint64_t clk = serdes_clk_pam4;  // give it a default;

  gb = 1000ul * 1000ul * 1000ul;
  if (speed == BF_SPEED_400G) {
    if (num_lanes == 4) {  // 400G-R4
      clk = serdes_clk_pam4_112g;
    } else {  // 400G-R8
      clk = serdes_clk_pam4;
    }
  } else if (speed == BF_SPEED_200G) {
    if (num_lanes == 2) {
      clk = serdes_clk_pam4_112g;
    } else if (num_lanes == 4) {
      clk = serdes_clk_pam4;
    } else if (num_lanes == 8) {
      clk = serdes_clk_nrz;
    }
  } else if (speed == BF_SPEED_100G) {
    if (num_lanes == 1) {
      clk = serdes_clk_pam4_112g;
    } else if (num_lanes == 2) {
      clk = serdes_clk_pam4;
    } else if (num_lanes == 4) {
      clk = serdes_clk_nrz;
    }
  } else if (speed == BF_SPEED_50G) {
    if (num_lanes == 1) {
      clk = serdes_clk_pam4;
    } else if (num_lanes == 2) {
      clk = serdes_clk_nrz;
    }
  } else if (speed == BF_SPEED_25G) {
    clk = serdes_clk_nrz;
  } else if (speed == BF_SPEED_40G) {
    clk = serdes_clk_nrz_10g;
  } else if (speed == BF_SPEED_10G) {
    clk = serdes_clk_nrz_10g;
  } else if (speed == BF_SPEED_1G) {
    clk = serdes_clk_nrz_1g;
  } else {
    clk = serdes_clk_nrz;
  }
  bps = (num_lanes * clk * gb) / 1000000ul;
  return bps;
}

static void pm_ucli_fec_to_display_get(bf_fec_type_t fec, char *str, int len) {
  switch (fec) {
    case BF_FEC_TYP_NONE:
      strcpy(str, "NONE");
      break;
    case BF_FEC_TYP_FIRECODE:
      strcpy(str, " FC ");
      break;
    case BF_FEC_TYP_REED_SOLOMON:
      strcpy(str, " RS ");
      break;
    default:
      strcpy(str, "----");
  }
  str[len - 1] = '\0';
}

static void pm_ucli_rdy_to_display_get(bf_pm_port_info_t *port_info,
                                       bf_dev_id_t dev_id,
                                       char *str,
                                       int len) {
  if (!port_info || !str) {
    return;
  }

  bf_dev_family_t dev_family =
      bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));

  strcpy(str, "NO ");
  if (dev_family == BF_DEV_FAMILY_TOFINO) {
    if (port_info->is_ready_for_bringup) {
      strcpy(str, "YES");
    }
  } else {  // all other TF family
    if ((port_info->admin_state == PM_PORT_DISABLED) ||
        (!port_info->is_added)) {
      strcpy(str, "---");
    } else if (port_info->lpbk_mode != BF_LPBK_NONE ||
               port_info->is_ready_for_bringup) {
      strcpy(str, "YES");
    } else {
      strcpy(str, "NO ");
    }
  }
  str[len - 1] = '\0';
}

static void pm_ucli_tolower(const char *str, char *str_new) {
  int i = 0, i_new = 0;
  int diff = 'A' - 'a';
  if ((str == NULL) || (str_new == NULL)) return;
  while (str[i] != '\0') {
    if ((str[i] == 'G') || (str[i] == 'R') || (str[i] == 'N') ||
        (str[i] == 'B') || (str[i] == 'C')) {
      str_new[i_new++] = str[i++] - diff;
    } else {
      str_new[i_new++] = str[i++];
    }
  }
  str_new[i_new] = '\0';
  return;
}

/**
 * Given a possibly wildcarded port string, construct an array of all individual
 * port_hdls represented by the wildcard.
 */
static bf_status_t bf_pm_ucli_port_list_get(char *in_portstr,
                                            uint32_t *list_sz,
                                            bf_pal_front_port_handle_t **list) {
  bf_status_t sts;
  bf_dev_id_t dev_id;
  bf_dev_id_t dev_id_of_port = 0;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr, *next_port_hdl_ptr;
  int db_size, i = 0, list_size;
  bf_pal_front_port_handle_t *port_hdl_list;
  bf_dev_port_t dev_port;
  char port_str[10] = {0};

  port_hdl_ptr = &port_hdl;
  next_port_hdl_ptr = &next_port_hdl;

  if (!in_portstr) return BF_INVALID_ARG;
  if (!list_sz) return BF_INVALID_ARG;
  if (!list) return BF_INVALID_ARG;

  strncpy(port_str, in_portstr, sizeof(port_str) - 1);
  *list_sz = 0;
  *list = NULL;

  dev_id = 0;

  sts = bf_pm_port_str_to_hdl_get(dev_id, port_str, port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    return BF_INVALID_ARG;
  }
  // printf("port_hdl_ptr = conn_id=%d chnl=%d\n",
  //       port_hdl_ptr->conn_id,
  //       port_hdl_ptr->chnl_id);
  if ((port_str[1] == '-') || ((port_str[1] != '/') && (port_str[2] == '-'))) {
    port_hdl_ptr->conn_id = 99;
    port_hdl_ptr->chnl_id = 99;
  }

  uint32_t nlanes_per_ch = 0, nserdes_per_mac = 0;
  if (port_hdl_ptr->conn_id == (uint32_t)-1) {
    db_size = pm_num_fp_all_get() + pm_num_of_internal_ports_all_get();
    list_size = db_size;
    port_hdl_list = (bf_pal_front_port_handle_t *)bf_sys_malloc(
        db_size * sizeof(bf_pal_front_port_handle_t));
    if (!port_hdl_list) return BF_INVALID_ARG;

    // Get the first port in the system
    sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
    if (sts != BF_SUCCESS || !port_hdl_ptr) {
      bf_sys_free((void *)port_hdl_list);
      return BF_INVALID_ARG;
    }
    memcpy(&port_hdl_list[0], port_hdl_ptr, sizeof(port_hdl_list[0]));
    i = 1;
    while (sts == BF_SUCCESS) {
      // Get the next port in the system
      sts = bf_pm_port_front_panel_port_get_next(
          dev_id, port_hdl_ptr, next_port_hdl_ptr);
      if (sts == BF_OBJECT_NOT_FOUND) break;
      if (sts != BF_SUCCESS) {
        bf_sys_free((void *)port_hdl_list);
        return BF_INVALID_ARG;
      }
      memcpy(&port_hdl_list[i], next_port_hdl_ptr, sizeof(port_hdl_list[i]));
      i++;
      // Make the curr port hdl equal to the next port hdl
      port_hdl_ptr = next_port_hdl_ptr;
    }
    bf_sys_assert(i == db_size);
  } else {
    if (port_hdl_ptr->chnl_id == (uint32_t)-1) {
      port_hdl_ptr->chnl_id = 0;
      if (BF_SUCCESS == bf_pm_port_front_panel_port_to_dev_port_get(
                            port_hdl_ptr, &dev_id_of_port, &dev_port)) {
        dev_id = dev_id_of_port;
        list_size = lld_get_chnls_dev_port(dev_id, dev_port);
        if (list_size == -1) return BF_INVALID_ARG;
        nserdes_per_mac = lld_get_num_serdes_per_mac(dev_id, dev_port);
      } else {
        return BF_INVALID_ARG;
      }
      port_hdl_list = (bf_pal_front_port_handle_t *)bf_sys_malloc(
          list_size * sizeof(bf_pal_front_port_handle_t));

      if (bf_lld_dev_is_tof3(dev_id)) {
        if ((dev_port >= lld_get_min_cpu_port(dev_id)) &&
            (dev_port <= lld_get_max_cpu_port(dev_id))) {
          nlanes_per_ch = 2;
        } else {
          nlanes_per_ch = (nserdes_per_mac == 8) ? 2 : 1;
        }
        for (i = 0; i < list_size; i++) {
          port_hdl_list[i].conn_id = port_hdl_ptr->conn_id;
          port_hdl_list[i].chnl_id = i * nlanes_per_ch;
        }
      } else {
        for (i = 0; i < list_size; i++) {
          port_hdl_list[i].conn_id = port_hdl_ptr->conn_id;
          port_hdl_list[i].chnl_id = i;
        }
      }
    } else {
      /* check for port range. It must either,
       *
       * X-Y/chnl
       * X-YY/chnl
       * XX-YY/chnl
       *
       * note: XX-Y is not possible as it would mean the range
       *       was going backwards
       */
      int conn_lo, conn_hi, chan;
#define is_dig(x) (((x) >= '0') && (((x) <= '9')))
#define ord(x) ((x) - '0')
      if ((port_str[1] == '-') || (port_str[2] == '-')) {
        if (port_str[1] == '-') {
          // X-
          conn_lo = ord(port_str[0]);
          if (is_dig(port_str[3])) {
            conn_hi = (ord(port_str[2]) * 10) + ord(port_str[3]);
            // X-YY
            if (port_str[5] == '-') {
              chan = -1;
            } else {
              chan = ord(port_str[5]);
            }
          } else {
            // X-Y
            conn_hi = ord(port_str[2]);
            if (port_str[4] == '-') {
              chan = -1;
            } else {
              chan = ord(port_str[4]);
            }
          }
        } else {
          // XX-YY
          conn_lo = (ord(port_str[0]) * 10) + ord(port_str[1]);
          // conn_hi must also be 2 digit
          conn_hi = (ord(port_str[3]) * 10) + ord(port_str[4]);
          if (port_str[6] == '-') {
            chan = -1;
          } else {
            chan = ord(port_str[6]);
          }
        }
        if ((conn_lo <= 0) || (conn_lo >= 66)) {
          // invalid range
          return BF_INVALID_ARG;
        }
        if (chan > 7) {
          // invalid range
          return BF_INVALID_ARG;
        }
        if ((conn_hi <= 0) || (conn_hi >= 66) || (conn_hi <= conn_lo)) {
          // invalid range
          return BF_INVALID_ARG;
        }
        int next_list_ent = 0, conn;
        int list_len = 0;
        // allocate space for 8 chnls per conn_id (max)
        port_hdl_list = (bf_pal_front_port_handle_t *)bf_sys_malloc(
            (((conn_hi - conn_lo + 1) * 8) *
             sizeof(bf_pal_front_port_handle_t)));

        for (conn = conn_lo; conn <= conn_hi; conn++) {
          port_hdl_ptr->conn_id = conn;
          port_hdl_ptr->chnl_id = 0;
          if (BF_SUCCESS == bf_pm_port_front_panel_port_to_dev_port_get(
                                port_hdl_ptr, &dev_id_of_port, &dev_port)) {
            dev_id = dev_id_of_port;
            list_len = lld_get_chnls_dev_port(dev_id, dev_port);
            nserdes_per_mac = lld_get_num_serdes_per_mac(dev_id, dev_port);
          } else {
            return BF_INVALID_ARG;
          }

          if (bf_lld_dev_is_tof3(dev_id)) {
            if ((dev_port >= lld_get_min_cpu_port(dev_id)) &&
                (dev_port <= lld_get_max_cpu_port(dev_id))) {
              nlanes_per_ch = 2;
            } else {
              nlanes_per_ch = (nserdes_per_mac == 8) ? 2 : 1;
            }
            if (chan == -1) {  // wild-carded chnl
              for (i = 0; i < list_len; i++) {
                port_hdl_list[next_list_ent].conn_id = port_hdl_ptr->conn_id;
                port_hdl_list[next_list_ent].chnl_id = i * nlanes_per_ch;
                next_list_ent++;
              }
            } else {
              port_hdl_list[next_list_ent].conn_id = port_hdl_ptr->conn_id;
              port_hdl_list[next_list_ent].chnl_id = chan;
              next_list_ent++;
            }
          } else {
            if (chan == -1) {  // wild-carded chnl
              for (i = 0; i < list_len; i++) {
                port_hdl_list[next_list_ent].conn_id = port_hdl_ptr->conn_id;
                port_hdl_list[next_list_ent].chnl_id = i;
                next_list_ent++;
              }
            } else {
              port_hdl_list[next_list_ent].conn_id = port_hdl_ptr->conn_id;
              port_hdl_list[next_list_ent].chnl_id = chan;
              next_list_ent++;
            }
          }
        }
        list_size = next_list_ent;
      } else {
        list_size = 1;
        port_hdl_list = (bf_pal_front_port_handle_t *)bf_sys_malloc(
            list_size * sizeof(bf_pal_front_port_handle_t));
        port_hdl_list[i].conn_id = port_hdl_ptr->conn_id;
        port_hdl_list[i].chnl_id = port_hdl_ptr->chnl_id;
      }
    }
  }
  *list_sz = list_size;
  *list = port_hdl_list;
  return BF_SUCCESS;
}

bf_fec_type_t an_fec_remapp(bf_port_speed_t speed, bf_fec_type_t fec) {
  // Note that, for several auto-negotiated modes, FEC is not negotiated and
  // assumed to be set to RS. Some examples are 50G-R1, 100G-Rx, 200G-Rx,
  // 400G, etc).
  // This function performs the remapping to display the actual FEC value
  // configured on HW.
  switch (speed) {
    case BF_SPEED_NONE:
    case BF_SPEED_1G:
    case BF_SPEED_10G:
    case BF_SPEED_25G:
    case BF_SPEED_50G_CONS:
      return fec;
    default:
      break;
  }
  return BF_FEC_TYP_REED_SOLOMON;
}

void dump_port_an_info_help(ucli_context_t *uc) {
  aim_printf(&uc->pvs, "Field description:\n");
  aim_printf(
      &uc->pvs,
      " DI:   dev_id                             FEC:  Current FEC config\n");
  aim_printf(&uc->pvs,
             " PORT: conn/channel                       Basepage: txtted(L:) "
             "or rxved(P:) basepage\n");
  aim_printf(&uc->pvs,
             " MAC:  mac/lane                           Nextpage #x: "
             "txtted(L:) or rxved(P:) next pages\n");
  aim_printf(&uc->pvs,
             " D_P:  dev_port                           Ability field: "
             "basepage's ability field detail\n");
  aim_printf(&uc->pvs,
             " OPR:  Operational State                  HCD raw: raw HCD index "
             "(see below for encoding)\n");
  aim_printf(
      &uc->pvs,
      " HCD:  Highest Common Denominator Speed   Flags F1,F0,F3,F2: FEC flags "
      "(see description below)\n");
  aim_printf(&uc->pvs,
             " LT_dur: Link Training duration [ms]      AN_try_cnt: "
             "number of AN retries\n\n");

  aim_printf(&uc->pvs, "Technology Ability Field encoding:\n");
  aim_printf(&uc->pvs,
             " A0:  1000BASE-KX                         A11: 2.5GBASE-KX\n");
  aim_printf(&uc->pvs,
             " A1:  10GBASE-KX4                         A12: 5GBASE-KR\n");
  aim_printf(&uc->pvs,
             " A2:  10GBASE-KR                          A13: 50GBASE-KR or "
             "50GBASE-CR\n");
  aim_printf(&uc->pvs,
             " A3:  40GBASE-KR4                         A14: 100GBASE-KR2 or "
             "100GBASE-CR2\n");
  aim_printf(&uc->pvs,
             " A4:  40GBASE-CR4                         A15: 200GBASE-KR4 or "
             "200GBASE-CR4\n");
  aim_printf(&uc->pvs,
             " A5:  100GBASE-CR10                       A16: 100GBASE-KR1 or "
             "200GBASE-CR1\n");
  aim_printf(&uc->pvs,
             " A6:  100GBASE-KP4                        A17: 200GBASE-KR2 or "
             "200GBASE-CR2\n");
  aim_printf(&uc->pvs,
             " A7:  100GBASE-KR4                        A18: 400GBASE-KR4 or "
             "400GBASE-CR4\n");
  aim_printf(&uc->pvs,
             " A8:  100GBASE-CR4                        A19: Reserved for "
             "future technology\n");
  aim_printf(&uc->pvs,
             " A9:  25GBASE-KR-S or 25GBASE-CR-S        A20: Reserved for "
             "future technology\n");
  aim_printf(&uc->pvs,
             " A10: 25GBASE-KR or 25GBASE-CR            A21: Reserved for "
             "future technology\n\n");
  aim_printf(&uc->pvs, "FEC capability:\n");
  aim_printf(&uc->pvs,
             " F0(D46): 10 Gb/s per lane FEC ability    F1(D47): 10 Gb/s per "
             "lane FEC requested\n");
  aim_printf(&uc->pvs,
             " F2(D44): 25G RS-FEC requested            F3(D45): 25G BASE-R "
             "FEC requested\n\n");
}

void dump_port_an_info_banner(ucli_context_t *uc) {
  aim_printf(
      &uc->pvs,
      "+--+-----+----+---+---+-------+----+----------------+--------------+----"
      "----------+-----------------------+---+-------+------+----+\n");

  aim_printf(
      &uc->pvs,
      "|DI|PORT |MAC |D_P|OPR|HCD    |FEC |Basepage        |Next Page #1  "
      "|Next Page #2  |Ability field [0..22]  |HCD| Flags | LT   |AN  |\n");

  aim_printf(
      &uc->pvs,
      "|  |     |    |   |   |       |    |                |              |    "
      "          |00000000001111111111222|   |F F F F| dur  |try |\n");

  aim_printf(
      &uc->pvs,
      "|  |     |    |   |   |speed  |    |                |              |    "
      "          |01234567890123456789012|raw|1 0 3 2| [ms] |cnt |\n");

  aim_printf(
      &uc->pvs,
      "+--+-----+----+---+---+-------+----+----------------+--------------+----"
      "----------+-----------------------+---+-------+------+----+\n");
}

void dump_port_an_info_dump_line_1(ucli_context_t *uc,
                                   bf_dev_id_t dev_id,
                                   bf_pm_port_info_t *port_info_ptr) {
  char advtech_str[24] = {0};
  char fec_flg_str[9] = {0};
  uint64_t loc_basepage = 0;
  uint64_t loc_nxtpage1 = 0;
  uint64_t loc_nxtpage2 = 0;
  bf_port_speed_t hcd_speed;
  bf_dev_port_t dev_port;
  bf_fec_type_t hcd_fec;
  uint64_t an_lt_dur_us;
  char hcd_speed_str[9];
  char hcd_fec_str[5];
  uint32_t an_tries;
  bf_status_t sts;
  int hcd_lanes;
  int hcd_index;
  int i;

  // gather needed info and prepare it to be displayed
  dev_port = port_info_ptr->dev_port;
  bf_serdes_an_loc_pages_get(dev_id, dev_port, BF_AN_PAGE_BASE, &loc_basepage);
  bf_serdes_an_loc_pages_get(
      dev_id, dev_port, BF_AN_PAGE_NEXT_1, &loc_nxtpage1);
  bf_serdes_an_loc_pages_get(
      dev_id, dev_port, BF_AN_PAGE_NEXT_2, &loc_nxtpage2);
  sts = bf_port_autoneg_hcd_fec_get_v2(
      dev_id, dev_port, &hcd_speed, &hcd_lanes, &hcd_fec);
  if (sts != BF_SUCCESS) return;

  pm_ucli_speed_to_display_get(
      hcd_speed, hcd_speed_str, sizeof(hcd_speed_str), hcd_lanes);
  pm_ucli_fec_to_display_get(
      an_fec_remapp(hcd_speed, hcd_fec), hcd_fec_str, sizeof(hcd_fec_str));
  bf_serdes_an_hcd_index_get(dev_id, dev_port, &hcd_index);

  // build tech ability field
  for (i = 0; i <= 22; i++) {
    advtech_str[i] = (loc_basepage & (1ull << (i + 21))) ? '1' : '0';
  }

  // get fec flags field (F1, F0, F3, F2)
  for (i = 0; i < 4; i++) {
    fec_flg_str[2 * i] = (loc_basepage & (1ull << (47 - i))) ? '1' : '0';
    fec_flg_str[2 * i + 1] = ' ';
  }
  fec_flg_str[7] = '\0';

  // dev_id
  aim_printf(&uc->pvs, "|%2" PRIu64, (uint64_t)dev_id);
  // port/channel
  aim_printf(&uc->pvs,
             "|%3" PRIu64 "/%" PRIu64,
             (uint64_t)port_info_ptr->pltfm_port_info.port_hdl.conn_id,
             (uint64_t)port_info_ptr->pltfm_port_info.port_hdl.chnl_id);
  // mac/lane
  aim_printf(&uc->pvs,
             "|%2" PRIu64 "/%" PRIu64,
             (uint64_t)port_info_ptr->pltfm_port_info.log_mac_id,
             (uint64_t)port_info_ptr->pltfm_port_info.log_mac_lane);
  // dev_port
  aim_printf(&uc->pvs, "|%3" PRIu64, (uint64_t)dev_port);
  // operational state
  aim_printf(&uc->pvs, "|%s", port_info_ptr->oper_status ? " UP" : "DWN");
  // hcd speed
  aim_printf(&uc->pvs, "|%s", hcd_speed_str);
  // hcd FEC
  aim_printf(&uc->pvs, "|%s", hcd_fec_str);
  // transmitted base_page
  aim_printf(&uc->pvs,
             "|L:%04x_%04x_%04x",
             (uint32_t)(loc_basepage >> 32ull) & 0xffff,
             (uint32_t)(loc_basepage >> 16ull) & 0xffff,
             (uint32_t)(loc_basepage >> 0ull) & 0xffff);

  // transmitted next pages (only for 25G)
  if (loc_nxtpage1) {
    aim_printf(&uc->pvs,
               "|%04x_%04x_%04x",
               (uint32_t)(loc_nxtpage1 >> 32ull) & 0xffff,
               (uint32_t)(loc_nxtpage1 >> 16ull) & 0xffff,
               (uint32_t)(loc_nxtpage1 >> 0ull) & 0xffff);
    aim_printf(&uc->pvs,
               "|%04x_%04x_%04x",
               (uint32_t)(loc_nxtpage2 >> 32ull) & 0xffff,
               (uint32_t)(loc_nxtpage2 >> 16ull) & 0xffff,
               (uint32_t)(loc_nxtpage2 >> 0ull) & 0xffff);
  } else {
    aim_printf(&uc->pvs, "|---- ---- ----");
    aim_printf(&uc->pvs, "|---- ---- ----");
  }

  // Ability field
  aim_printf(&uc->pvs, "|%s", advtech_str);

  // HCD RAW (index 0..22)
  aim_printf(&uc->pvs, "| %2d", hcd_index);

  // FEC
  aim_printf(&uc->pvs, "|%s", fec_flg_str);

  // Link training duration (ms)
  bf_port_an_lt_stats_get(dev_id, dev_port, &an_lt_dur_us, &an_tries);
  if (an_lt_dur_us >= 99 * 1000000) {
    aim_printf(&uc->pvs, "| 99999");
  } else {
    aim_printf(&uc->pvs, "| %5u", (uint32_t)an_lt_dur_us / 1000);
  }

  // AN try counter
  if (an_tries >= 999) {
    aim_printf(&uc->pvs, "| 999|\n");
  } else {
    aim_printf(&uc->pvs, "| %3u|\n", an_tries);
  }
}

void dump_port_an_info_dump_line_2(ucli_context_t *uc,
                                   bf_dev_id_t dev_id,
                                   bf_pm_port_info_t *port_info_ptr) {
  char advtech_str[24] = {0};
  char fec_flg_str[9] = {0};
  uint64_t lp_basepage = 0;
  uint64_t lp_nxtpage1 = 0;
  uint64_t lp_nxtpage2 = 0;
  bf_dev_port_t dev_port;
  int hcd_index;
  int i;

  dev_port = port_info_ptr->dev_port;
  bf_serdes_an_rcv_pages_get(dev_id, dev_port, BF_AN_PAGE_BASE, &lp_basepage);
  bf_serdes_an_rcv_pages_get(dev_id, dev_port, BF_AN_PAGE_NEXT_1, &lp_nxtpage1);
  bf_serdes_an_rcv_pages_get(dev_id, dev_port, BF_AN_PAGE_NEXT_2, &lp_nxtpage2);
  bf_serdes_an_hcd_index_get(dev_id, dev_port, &hcd_index);

  // tech ability field
  for (i = 0; i <= 22; i++) {
    advtech_str[i] = (lp_basepage & (1ull << (i + 21))) ? '1' : '0';
  }

  // fec flags F1, F0, F3, F2
  for (i = 0; i < 4; i++) {
    fec_flg_str[2 * i] = (lp_basepage & (1ull << (47 - i))) ? '1' : '0';
    fec_flg_str[2 * i + 1] = ' ';
  }
  fec_flg_str[7] = '\0';

  // 2nd line: skip some fields
  aim_printf(&uc->pvs, "|  |     |    |   |   |       |    ");

  // transmitted base_page
  aim_printf(&uc->pvs,
             "|P:%04x_%04x_%04x",
             (uint32_t)(lp_basepage >> 32ull) & 0xffff,
             (uint32_t)(lp_basepage >> 16ull) & 0xffff,
             (uint32_t)(lp_basepage >> 0ull) & 0xffff);

  // transmitted next pages (only for 25G)
  if (lp_nxtpage1) {
    aim_printf(&uc->pvs,
               "|%04x_%04x_%04x",
               (uint32_t)(lp_nxtpage1 >> 32ull) & 0xffff,
               (uint32_t)(lp_nxtpage1 >> 16ull) & 0xffff,
               (uint32_t)(lp_nxtpage1 >> 0ull) & 0xffff);
    aim_printf(&uc->pvs,
               "|%04x_%04x_%04x",
               (uint32_t)(lp_nxtpage2 >> 32ull) & 0xffff,
               (uint32_t)(lp_nxtpage2 >> 16ull) & 0xffff,
               (uint32_t)(lp_nxtpage2 >> 0ull) & 0xffff);
  } else {
    aim_printf(&uc->pvs, "|---- ---- ----");
    aim_printf(&uc->pvs, "|---- ---- ----");
  }

  // Ability field
  aim_printf(&uc->pvs, "|%s", advtech_str);

  // HCD index
  aim_printf(&uc->pvs, "| %2d", hcd_index);

  // FEC
  aim_printf(&uc->pvs, "|%s|      |    |\n", fec_flg_str);

  aim_printf(
      &uc->pvs,
      "+--+-----+----+---+---+-------+----+----------------+--------------+----"
      "----------+-----------------------+---+-------+------+----+\n");
}

static int pm_ucli_port_speed_get(const char *str,
                                  bf_port_speed_t *ps,
                                  uint32_t *n_lanes) {
  uint32_t max_len = strlen("40G_NON_BREAKABLE");
  char str_new[max_len + 1];
  if (strlen(str) > max_len) return -1;
  pm_ucli_tolower(str, str_new);

  if ((strcmp(str_new, "1g") == 0) || (strcmp(str_new, "1") == 0)) {
    *ps = BF_SPEED_1G;
    *n_lanes = 1;
    return 0;
  } else if ((strcmp(str_new, "10g") == 0) || (strcmp(str_new, "10") == 0)) {
    *ps = BF_SPEED_10G;
    *n_lanes = 1;
    return 0;
  } else if ((strcmp(str_new, "25g") == 0) || (strcmp(str_new, "25") == 0)) {
    *ps = BF_SPEED_25G;
    *n_lanes = 1;
    return 0;
  } else if ((strcmp(str_new, "40g") == 0) || ((strcmp(str_new, "40") == 0))) {
    *ps = BF_SPEED_40G;
    *n_lanes = 4;
    return 0;
  } else if ((strcmp(str_new, "50g") == 0) ||
             (strcmp(str_new, "50g-r2") == 0)) {
    *ps = BF_SPEED_50G;
    *n_lanes = 2;
    return 0;
  } else if ((strcmp(str_new, "50g-r2") == 0) ||
             (strcmp(str_new, "50-r2") == 0)) {
    *ps = BF_SPEED_50G;
    *n_lanes = 2;
    return 0;
  } else if ((strcmp(str_new, "50g-r2-c") == 0) ||
             (strcmp(str_new, "50-r2-c") == 0)) {
    *ps = BF_SPEED_50G_CONS;
    *n_lanes = 2;
    return 0;
  } else if ((strcmp(str_new, "50g-r1") == 0) ||
             (strcmp(str_new, "50-r1") == 0)) {
    *ps = BF_SPEED_50G;
    *n_lanes = 1;
    return 0;
  } else if ((strcmp(str_new, "100g") == 0) || (strcmp(str_new, "100") == 0)) {
    *ps = BF_SPEED_100G;
    *n_lanes = 4;
    return 0;
  } else if ((strcmp(str_new, "100g-r4") == 0) ||
             (strcmp(str_new, "100-r4") == 0)) {
    *ps = BF_SPEED_100G;
    *n_lanes = 4;
    return 0;
  } else if ((strcmp(str_new, "100g-r2") == 0) ||
             (strcmp(str_new, "100-r2") == 0)) {
    *ps = BF_SPEED_100G;
    *n_lanes = 2;
    return 0;
  } else if ((strcmp(str_new, "100g-r1") == 0) ||
             (strcmp(str_new, "100-r1") == 0)) {
    *ps = BF_SPEED_100G;
    *n_lanes = 1;
    return 0;
  } else if ((strcmp(str_new, "200g") == 0) || (strcmp(str_new, "200") == 0)) {
    *ps = BF_SPEED_200G;
    *n_lanes = 4;
    return 0;
  } else if ((strcmp(str_new, "200g-r2") == 0) ||
             (strcmp(str_new, "200-r2") == 0)) {
    *ps = BF_SPEED_200G;
    *n_lanes = 2;
    return 0;
  } else if ((strcmp(str_new, "200g-r4") == 0) ||
             (strcmp(str_new, "200-r4") == 0)) {
    *ps = BF_SPEED_200G;
    *n_lanes = 4;
    return 0;
  } else if ((strcmp(str_new, "200g-r8") == 0) ||
             (strcmp(str_new, "200-r8") == 0)) {
    *ps = BF_SPEED_200G;
    *n_lanes = 8;
    return 0;
  } else if ((strcmp(str_new, "400g") == 0) || (strcmp(str_new, "400") == 0) ||
             (strcmp(str_new, "400g-r8") == 0) ||
             (strcmp(str_new, "400-r8") == 0)) {
    *ps = BF_SPEED_400G;
    *n_lanes = 8;
    return 0;
  } else if ((strcmp(str_new, "400g-r4") == 0) ||
             (strcmp(str_new, "400-r4") == 0)) {
    *ps = BF_SPEED_400G;
    *n_lanes = 4;
    return 0;
  } else if ((strcmp(str_new, "40g-r2") == 0) ||
             (strcmp(str_new, "40-r2") == 0)) {
    *ps = BF_SPEED_40G_R2;
    *n_lanes = 2;
    return 0;
  } else {
    return -1;
  }
}

static int pm_ucli_fec_type_get(const char *str, bf_fec_type_t *fec_type) {
  if ((strcmp(str, "NONE") == 0) || (strcmp(str, "none") == 0)) {
    *fec_type = BF_FEC_TYP_NONE;
    return 0;
  } else if ((strcmp(str, "FC") == 0) || (strcmp(str, "fc") == 0)) {
    *fec_type = BF_FEC_TYP_FIRECODE;
    return 0;
  } else if ((strcmp(str, "RS") == 0) || (strcmp(str, "rs") == 0)) {
    *fec_type = BF_FEC_TYP_REED_SOLOMON;
    return 0;
  } else {
    return -1;
  }
}

// Some simple checks to avoid errors while ucli config
static bool pm_ucli_port_add_tof3_validate(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           bf_port_speed_t speed,
                                           uint32_t n_lanes) {
  bool sds_is_56g_mode =
      lld_get_num_serdes_per_mac(dev_id, dev_port) == 8 ? true : false;

  if ((speed == BF_SPEED_400G) && (n_lanes == 4) && (sds_is_56g_mode))
    return false;

  if ((speed == BF_SPEED_400G) && (n_lanes == 8) && (!sds_is_56g_mode))
    return false;

  if ((speed == BF_SPEED_200G) && (n_lanes == 2) && (sds_is_56g_mode))
    return false;

  if ((speed == BF_SPEED_200G) && (n_lanes == 8) && (!sds_is_56g_mode))
    return false;

  if ((speed == BF_SPEED_100G) && (n_lanes == 1) && (sds_is_56g_mode))
    return false;

  return true;
}

static bf_status_t pm_ucli_port_add_tof3(bf_dev_id_t dev_id,
                                         bf_pal_front_port_handle_t *port_hdl,
                                         bf_port_speed_t speed,
                                         uint32_t n_lanes,
                                         bf_fec_type_t fec_type,
                                         ucli_context_t *uc) {
  bf_dev_port_t dev_port;
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl_ptr;
  uint32_t i, ch_used_mask = 0, ch_reqd_mask, max_mac_ch;
  uint32_t num_lanes;
  bf_dev_id_t dev_id_of_port = 0;

  if (!port_hdl || !uc) return BF_INVALID_ARG;

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) {
    return sts;
  }
  if (dev_id != dev_id_of_port) {
    return BF_INVALID_ARG;
  }

  if (bf_port_is_valid(dev_id, dev_port) == BF_SUCCESS)
    return BF_ALREADY_EXISTS;  // port exist

  port_hdl_ptr.conn_id = port_hdl->conn_id;
  port_hdl_ptr.chnl_id = port_hdl->chnl_id;

  // printf("%2d/%d : Add : Speed=%d : lanes=%d\n",
  //       port_hdl->conn_id,
  //       port_hdl->chnl_id,
  //       speed,
  //       n_lanes);

  // Screen
  // a) if a particular port is being created with different speed
  // b) if port is added with /- notation
  // Note:
  //  56g ports have 4 channels and 8 serdes lanes
  // 112g ports have 4 channels and 4 serdes lanes
  //
  // A port requires at least one channel and one lane
  //
  // max_mac_ch = 4, except for CPU ports=2 (for some reason)
  //
  max_mac_ch = (uint32_t)lld_get_chnls_dev_port(dev_id, dev_port);
  //
  // nserdes_per_mac = 8 for 56g ports, 4 for 112g ports
  //
  int nserdes_per_mac = lld_get_num_serdes_per_mac(dev_id, dev_port);
  //
  // ln_per_chnl is 2 for 56g ports, 1 for 112g ports
  //
  int ln_per_chnl = nserdes_per_mac / max_mac_ch;

  // printf("dev_port=%d : max_mac_ch=%d : nserdes_per_mac=%d :
  // ln_per_chnl=%d\n",
  //       dev_port,
  //       max_mac_ch,
  //       nserdes_per_mac,
  //       ln_per_chnl);
  if (ln_per_chnl == 0) return BF_NOT_SUPPORTED;

  //
  // ch_needed_for_this_port
  //
  int ch_needed_for_this_port =
      (ln_per_chnl == 1) ? n_lanes : (n_lanes + 1) / ln_per_chnl;

  // printf("ch_needed_for_this_port=%d\n", ch_needed_for_this_port);

  if (ln_per_chnl == 2) {
    //
    // Only even channels are used on TF3 so "chnl_id" is really 2x what you
    // expect
    //
    ch_reqd_mask = ((1U << ch_needed_for_this_port) - 1)
                   << (port_hdl->chnl_id / 2);
  } else {  // OSFP ports are numbered sequentially, not by 2's
    ch_reqd_mask = ((1U << ch_needed_for_this_port) - 1) << (port_hdl->chnl_id);
  }
  // printf("ch_reqd_mask=%x\n", ch_reqd_mask);

  if (!pm_ucli_port_add_tof3_validate(dev_id, dev_port, speed, n_lanes))
    return BF_NOT_SUPPORTED;

  //
  // Determine how many channels (not lanes) are already in use
  for (i = 0; i < max_mac_ch; i++) {
    int used_channels = 0;

    if (ln_per_chnl == 2) {
      port_hdl_ptr.chnl_id = 2 * i;
    } else {
      port_hdl_ptr.chnl_id = i;
    }

    // On a non-existing port, num-of-lanes is zero.
    num_lanes = bf_pm_port_get_num_of_lanes(dev_id, &port_hdl_ptr);

    if (num_lanes) {
      used_channels =
          (ln_per_chnl == 1) ? num_lanes : (num_lanes + 1) / ln_per_chnl;

      // printf("ch%d : used_channels=%d\n", i, used_channels);

      ch_used_mask |= ((1U << used_channels) - 1) << i;

      // printf("ch%d : ch_used_mask =%x\n", i, ch_used_mask);
    }
  }
  if (ch_used_mask & ch_reqd_mask) {
    return BF_ALREADY_EXISTS;
  }

  // Any error will be logged within port-add
  return bf_pm_port_add_with_lanes(dev_id, port_hdl, speed, n_lanes, fec_type);
}

/**
 * Utility fn for port-add ucli
 */
static bf_status_t pm_ucli_port_add(bf_dev_id_t dev_id,
                                    bf_pal_front_port_handle_t *port_hdl,
                                    bf_port_speed_t speed,
                                    uint32_t n_lanes,
                                    bf_fec_type_t fec_type,
                                    ucli_context_t *uc) {
  bf_dev_port_t dev_port;
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl_ptr;
  uint32_t i, ch_used_mask = 0, ch_reqd_mask, max_ch;
  uint32_t nlanes_used = 0, num_lanes;
  bf_dev_id_t dev_id_of_port = 0;

  if (!port_hdl || !uc) return BF_INVALID_ARG;

  // printf("pm_ucli_port_add: dev=%d : is_tof3=%d\n",
  //       dev_id,
  //       bf_pm_intf_is_device_family_tofino3(dev_id));

  if (bf_pm_intf_is_device_family_tofino3(dev_id)) {
    return pm_ucli_port_add_tof3(
        dev_id, port_hdl, speed, n_lanes, fec_type, uc);
  }

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) {
    return sts;
  }
  if (dev_id != dev_id_of_port) {
    return BF_INVALID_ARG;
  }

  port_hdl_ptr.conn_id = port_hdl->conn_id;
  port_hdl_ptr.chnl_id = port_hdl->chnl_id;

  // Screen
  // a) if a particular port is being created with different speed
  // b) if port is added with /- notation

  if (lld_get_chnls_dev_port(dev_id, dev_port) == -1) return BF_INVALID_ARG;

  max_ch = (uint32_t)lld_get_chnls_dev_port(dev_id, dev_port);
  ch_reqd_mask = ((1U << n_lanes) - 1) << port_hdl->chnl_id;

  // Unsupported config e.g., 400G on cpu-port
  if (n_lanes > max_ch) {
    return BF_NOT_SUPPORTED;
  }

  for (i = 0; i < max_ch; i++) {
    port_hdl_ptr.chnl_id = i;
    // On a non-existence port, num-of-lanes is zero.
    num_lanes = bf_pm_port_get_num_of_lanes(dev_id, &port_hdl_ptr);
    if (num_lanes) {
      ch_used_mask |= ((1U << num_lanes) - 1) << i;
    }
    nlanes_used += num_lanes;
  }

  // no free channels
  if ((nlanes_used + n_lanes) > max_ch) {
    return BF_NO_SPACE;
  }

  if (ch_used_mask & ch_reqd_mask) {
    return BF_ALREADY_EXISTS;
  }

  // Any error will be logged within port-add
  return bf_pm_port_add_with_lanes(dev_id, port_hdl, speed, n_lanes, fec_type);
}

static ucli_status_t bf_pm_ucli_ucli__port_add__(ucli_context_t *uc) {
  static char usage[] =
      "port-add <port_str> <speed (1G, 10G, 25G, 40G, 40G-R2, "
      "50G(50G/50G-R2, 50G-R2-C, 50G-R1), 100G(100G/100G-R4, "
      "100G-R2, 100G-R1), 200G(200G/200G-R4, 200G-R2, 200G-R8), "
      "400G(400G/400G-R8, 400G-R4))> <fec (NONE, FC, RS)>";
  UCLI_COMMAND_INFO(uc,
                    "port-add",
                    3,
                    "<port_str> <speed (1G, 10G, 25G, 40G, 40G-R2, "
                    "50G(50G/50G-R2, 50G-R2-C, 50G-R1), 100G(100G/100G-R4, "
                    "100G-R2, 100G-R1), 200G(200G/200G-R4, 200G-R2, 200G-R8), "
                    "400G(400G/400G-R8, 400G-R4))> <fec (NONE, FC, RS)>");

  bf_port_speed_t speed;
  bf_fec_type_t fec_type;
  uint32_t n_lanes;
  bf_status_t sts;
  int ret;
  uint32_t list_sz;
  bf_pal_front_port_handle_t *list;
  bool all_add_failed = true;

  // get speed mode
  ret = pm_ucli_port_speed_get(uc->pargs->args[1], &speed, &n_lanes);
  if (ret != 0) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return UCLI_STATUS_E_ARG;
  }

  // get FEC mode
  ret = pm_ucli_fec_type_get(uc->pargs->args[2], &fec_type);
  if (ret != 0) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return UCLI_STATUS_E_ARG;
  }

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return UCLI_STATUS_E_ARG;
  }
  // During wildcard move to next port
  uint32_t idx = n_lanes;
  if (bf_pm_intf_is_device_family_tofino3(0)) {
    idx = 1;
  } else {
    idx = n_lanes;
  }
  for (uint32_t i = 0; i < list_sz; i = i + idx) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    bf_pal_front_port_handle_t *port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) {
      idx = n_lanes;
      continue;
    }

    if (bf_lld_dev_is_tof1(dev_id) && (speed == BF_SPEED_25G) &&
        (fec_type == BF_FEC_TYP_REED_SOLOMON) &&
        ((dev_port >= lld_get_min_cpu_port(dev_id)) &&
         (dev_port <= lld_get_max_cpu_port(dev_id)))) {
      aim_printf(&uc->pvs, "RS-FEC is not supported for 25G CPU ports\n");
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return UCLI_STATUS_E_ARG;
    }

    sts = pm_ucli_port_add(dev_id, port_hdl_ptr, speed, n_lanes, fec_type, uc);
    all_add_failed &= (sts != BF_SUCCESS);
  }

  bf_sys_free((void *)list);

  // print and return an error only if ALL add failed
  if (all_add_failed) {
    aim_printf(&uc->pvs, "Add failed %s (%d)\n", bf_err_str(sts), sts);
    // Don't return errors to bfshell
  }

  return UCLI_STATUS_OK;
}

/**
 * Utility fn for port-del ucli
 */
static bf_status_t pm_ucli_port_delete(ucli_context_t *uc,
                                       bf_dev_id_t dev_id,
                                       bf_pal_front_port_handle_t *port_hdl) {
  bf_dev_port_t dev_port;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;

  if (!uc) return BF_INVALID_ARG;

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) {
    return sts;
  }
  dev_id = dev_id_of_port;

  if (bf_port_is_valid(dev_id, dev_port) != BF_SUCCESS)
    return BF_SUCCESS;  // port-does not exist
  sts = bf_pm_port_delete(dev_id, port_hdl);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs,
               "Port delete error: port: %d/%d\n",
               port_hdl->conn_id,
               port_hdl->chnl_id);
    return sts;
  }

  return BF_SUCCESS;
}

static ucli_status_t bf_pm_ucli_ucli__port_delete__(ucli_context_t *uc) {
  static char usage[] = "port-del <port_str>";
  UCLI_COMMAND_INFO(uc, "port-del", 1, "<port_str>");
  bf_status_t sts;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    (void)dev_port;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;

    pm_ucli_port_delete(uc, dev_id, port_hdl_ptr);
  }
  bf_sys_free((void *)list);
  return 0;
}

/**
 * Utility fn for port-enb ucli
 */
static void pm_ucli_port_enable(bf_dev_id_t dev_id,
                                bf_pal_front_port_handle_t *port_hdl) {
  bf_dev_port_t dev_port;
  bf_status_t sts;
  bf_pm_port_info_t port_info;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) {
    return;
  }
  dev_id = dev_id_of_port;

  ret = pm_port_info_get_copy(dev_id, dev_port, &port_info);
  if (ret != 0) {
    return;
  }

  if (!port_info.is_added) {
    return;
  }

  bf_pm_port_enable(dev_id, port_hdl);
}

static ucli_status_t bf_pm_ucli_ucli__port_enable__(ucli_context_t *uc) {
  static char usage[] = "port-enb <port_str>";
  UCLI_COMMAND_INFO(uc, "port-enb", 1, "<port_str>");
  bf_status_t sts;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    (void)dev_port;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;

    pm_ucli_port_enable(dev_id, port_hdl_ptr);
  }
  bf_sys_free((void *)list);
  return 0;
}

/**
 * Utility fn for port-dis ucli
 */
static void pm_ucli_port_disable(bf_dev_id_t dev_id,
                                 bf_pal_front_port_handle_t *port_hdl) {
  bf_dev_port_t dev_port;
  bf_status_t sts;
  bf_pm_port_info_t port_info;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) {
    return;
  }
  dev_id = dev_id_of_port;

  ret = pm_port_info_get_copy(dev_id, dev_port, &port_info);
  if (ret != 0) {
    return;
  }

  if (!port_info.is_added) {
    return;
  }

  bf_pm_port_disable(dev_id, port_hdl);
}

static ucli_status_t bf_pm_ucli_ucli__port_disable__(ucli_context_t *uc) {
  static char usage[] = "port-dis <port_str>";
  UCLI_COMMAND_INFO(uc, "port-dis", 1, "<port_str>");
  bf_status_t sts;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    (void)dev_port;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;

    pm_ucli_port_disable(dev_id, port_hdl_ptr);
  }
  bf_sys_free((void *)list);
  return 0;
}

static void pm_ucli_display_port_fc_fec_counters(
    ucli_context_t *uc,
    bf_dev_id_t dev_id,
    int aflag,
    bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  uint32_t print_count = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  char *port_d[] = {"PORT ", "-----"};
  char *mac_d[] = {"MAC ", "----"};
  char *dev_port_d[] = {"D_P", "---"};
  char *speed_d[] = {"SPEED  ", "-------"};
  char *fec_d[] = {"FEC ", "----"};
  char *oper_d[] = {"OPR", "---"};

  char *vl_lane_d[] = {"vl lane", "-------"};
  char *block_lck_sts_d[] = {"BLK LOCK", "--------"};
  char *uncorr_fec_d[] = {"FEC UNCORR", "-----------"};
  char *corr_fec_d[] = {"FEC CORR", "--------"};

  char display_speed[9];
  char display_fec[5];

  aim_printf(&uc->pvs,
             "\n%s+%s+%s+%s+%s+%s+%s+%s+%s+%s\n",
             port_d[1],
             mac_d[1],
             dev_port_d[1],
             speed_d[1],
             fec_d[1],
             oper_d[1],
             vl_lane_d[1],
             block_lck_sts_d[1],
             uncorr_fec_d[1],
             corr_fec_d[1]);

  aim_printf(&uc->pvs,
             "%s|%s|%s|%s|%s|%s|%s|%s|%s|%s\n",
             port_d[0],
             mac_d[0],
             dev_port_d[0],
             speed_d[0],
             fec_d[0],
             oper_d[0],
             vl_lane_d[0],
             block_lck_sts_d[0],
             uncorr_fec_d[0],
             corr_fec_d[0]);

  aim_printf(&uc->pvs,
             "%s+%s+%s+%s+%s+%s+%s+%s+%s+%s\n",
             port_d[1],
             mac_d[1],
             dev_port_d[1],
             speed_d[1],
             fec_d[1],
             oper_d[1],
             vl_lane_d[1],
             block_lck_sts_d[1],
             uncorr_fec_d[1],
             corr_fec_d[1]);

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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

    if (!next_port_info_ptr->is_added) {
      if (aflag) {
        // Indicates that this port has not been added yet
        aim_printf(&uc->pvs,
                   "%-5s|%2" PRIu64 "/%1" PRIu64 "|%3" PRIu64
                   ""
                   "|%s|%s|%s|%s|%s|%s|%s|\n",
                   next_port_info_ptr->pltfm_port_info.port_str,
                   (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_id,
                   (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_lane,
                   (uint64_t)next_port_info_ptr->dev_port,
                   speed_d[1],
                   fec_d[1],
                   oper_d[1],
                   vl_lane_d[1],
                   block_lck_sts_d[1],
                   uncorr_fec_d[1],
                   corr_fec_d[1]);

        print_count++;
      }
    } else if (next_port_info_ptr->fec_type == BF_FEC_TYP_FIRECODE) {
      print_count++;
      pm_ucli_speed_to_display_get(next_port_info_ptr->speed,
                                   display_speed,
                                   sizeof(display_speed),
                                   next_port_info_ptr->n_lanes);
      pm_ucli_fec_to_display_get(
          next_port_info_ptr->fec_type, display_fec, sizeof(display_fec));

      aim_printf(&uc->pvs,
                 "%-5s|%2" PRIu64 "/%1" PRIu64 "|%3" PRIu64
                 ""
                 "|%s|%s|%s",
                 next_port_info_ptr->pltfm_port_info.port_str,
                 (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_id,
                 (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_lane,
                 (uint64_t)next_port_info_ptr->dev_port,
                 display_speed,
                 display_fec,
                 next_port_info_ptr->oper_status ? "UP " : "DWN");

      uint32_t vl, num_vl;

      if ((next_port_info_ptr->speed == BF_SPEED_10G) ||
          (next_port_info_ptr->speed == BF_SPEED_25G)) {
        num_vl = 1;
      } else if ((next_port_info_ptr->speed == BF_SPEED_40G) ||
                 (next_port_info_ptr->speed == BF_SPEED_40G_R2) ||
                 (next_port_info_ptr->speed == BF_SPEED_50G) ||
                 (next_port_info_ptr->speed == BF_SPEED_50G_CONS)) {
        num_vl = 4;
      } else {
        num_vl = 0; /* fec invalid for other speeds */
      }
      for (vl = 0; vl < num_vl; vl++) {
        if (vl > 0) {
          aim_printf(&uc->pvs, "                               ");
        }
        aim_printf(&uc->pvs,
                   "|%2d     |%-8s|%-11u|%-11u\n",
                   vl,
                   next_port_info_ptr->fec_info.fc_block_lock_status[vl] ? "yes"
                                                                         : "no",
                   next_port_info_ptr->fec_info.fc_fec_corr_blk_cnt[vl],
                   next_port_info_ptr->fec_info.fc_fec_uncorr_blk_cnt[vl]);
      }
    }

    // print the banner
    if (print_count >= 40) {
      aim_printf(&uc->pvs,
                 "%s+%s+%s+%s+%s+%s+%s+%s+%s+%s\n",
                 port_d[1],
                 mac_d[1],
                 dev_port_d[1],
                 speed_d[1],
                 fec_d[1],
                 oper_d[1],
                 vl_lane_d[1],
                 block_lck_sts_d[1],
                 uncorr_fec_d[1],
                 corr_fec_d[1]);

      aim_printf(&uc->pvs,
                 "%s|%s|%s|%s|%s|%s|%s|%s|%s|%s\n",
                 port_d[0],
                 mac_d[1],
                 dev_port_d[0],
                 speed_d[0],
                 fec_d[0],
                 oper_d[0],
                 vl_lane_d[0],
                 block_lck_sts_d[0],
                 uncorr_fec_d[0],
                 corr_fec_d[0]);

      aim_printf(&uc->pvs,
                 "%s+%s+%s+%s+%s+%s+%s+%s+%s+%s\n",
                 port_d[1],
                 mac_d[1],
                 dev_port_d[1],
                 speed_d[1],
                 fec_d[1],
                 oper_d[1],
                 vl_lane_d[1],
                 block_lck_sts_d[1],
                 uncorr_fec_d[1],
                 corr_fec_d[1]);

      print_count = 0;
    }
  }
}

static void pm_ucli_port_clear_fec_counters(
    bf_dev_id_t dev_id, int aflag, bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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

    if (!next_port_info_ptr->is_added) {
      if (aflag) {
        // Indicates that this port has not been added yet
      }
    } else { /* clear FC/RS fec counters */
      bf_pm_port_clear_the_fec_counters(dev_id, next_port_info_ptr->dev_port);
    }
  }
}

static void pm_ucli_display_port_rs_fec_counters(
    ucli_context_t *uc,
    bf_dev_id_t dev_id,
    int aflag,
    bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  uint32_t print_count = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  char *port_d[] = {"PORT ", "-----"};
  char *mac_d[] = {"MAC ", "----"};
  char *dev_port_d[] = {"D_P", "---"};
  char *speed_d[] = {"SPEED  ", "-------"};
  char *fec_d[] = {"FEC ", "----"};
  char *oper_d[] = {"OPR", "---"};
  char *hiser_d[] = {"HISER", "-----"};
  char *algnsts_d[] = {"FEC ALGN", "--------"};
  char *uncorr_fec_d[] = {"FEC UNCORR", "----------"};
  char *corr_fec_d[] = {"FEC CORR", "--------"};
  char *ser_ln0_d[] = {"ser ln 0", "--------"};
  char *ser_ln1_d[] = {"ser ln 1", "--------"};
  char *ser_ln2_d[] = {"ser ln 2", "--------"};
  char *ser_ln3_d[] = {"ser ln 3", "--------"};
  char *ser_ln4_d[] = {"ser ln 4", "--------"};
  char *ser_ln5_d[] = {"ser ln 5", "--------"};
  char *ser_ln6_d[] = {"ser ln 6", "--------"};
  char *ser_ln7_d[] = {"ser ln 7", "--------"};

  char display_speed[9];
  char display_fec[5];

  aim_printf(&uc->pvs,
             "%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s",
             port_d[1],
             mac_d[1],
             dev_port_d[1],
             speed_d[1],
             fec_d[1],
             oper_d[1],
             hiser_d[1],
             algnsts_d[1],
             uncorr_fec_d[1],
             corr_fec_d[1],
             ser_ln0_d[1],
             ser_ln1_d[1],
             ser_ln2_d[1],
             ser_ln3_d[1]);
  if (bf_lld_dev_is_tof2(dev_id)) {
    aim_printf(&uc->pvs,
               "+%s+%s+%s+%s",
               ser_ln4_d[1],
               ser_ln5_d[1],
               ser_ln6_d[1],
               ser_ln7_d[1]);
  }
  aim_printf(&uc->pvs, "\n");

  aim_printf(&uc->pvs,
             "%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s",
             port_d[0],
             mac_d[0],
             dev_port_d[0],
             speed_d[0],
             fec_d[0],
             oper_d[0],
             hiser_d[0],
             algnsts_d[0],
             uncorr_fec_d[0],
             corr_fec_d[0],
             ser_ln0_d[0],
             ser_ln1_d[0],
             ser_ln2_d[0],
             ser_ln3_d[0]);
  if (bf_lld_dev_is_tof2(dev_id)) {
    aim_printf(&uc->pvs,
               "|%s|%s|%s|%s",
               ser_ln4_d[0],
               ser_ln5_d[0],
               ser_ln6_d[0],
               ser_ln7_d[0]);
  }
  aim_printf(&uc->pvs, "\n");

  aim_printf(&uc->pvs,
             "%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s",
             port_d[1],
             mac_d[1],
             dev_port_d[1],
             speed_d[1],
             fec_d[1],
             oper_d[1],
             hiser_d[1],
             algnsts_d[1],
             uncorr_fec_d[1],
             corr_fec_d[1],
             ser_ln0_d[1],
             ser_ln1_d[1],
             ser_ln2_d[1],
             ser_ln3_d[1]);
  if (bf_lld_dev_is_tof2(dev_id)) {
    aim_printf(&uc->pvs,
               "+%s+%s+%s+%s",
               ser_ln4_d[1],
               ser_ln5_d[1],
               ser_ln6_d[1],
               ser_ln7_d[1]);
  }
  aim_printf(&uc->pvs, "\n");

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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

    if (!next_port_info_ptr->is_added) {
      if (aflag) {
        // Indicates that this port has not been added yet
        aim_printf(&uc->pvs,
                   "%-5s|%2" PRIu64 "/%1" PRIu64 "|%3" PRIu64
                   ""
                   "|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|",
                   next_port_info_ptr->pltfm_port_info.port_str,
                   (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_id,
                   (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_lane,
                   (uint64_t)next_port_info_ptr->dev_port,
                   speed_d[1],
                   fec_d[1],
                   oper_d[1],
                   hiser_d[1],
                   algnsts_d[1],
                   uncorr_fec_d[1],
                   corr_fec_d[1],
                   ser_ln0_d[1],
                   ser_ln1_d[1],
                   ser_ln2_d[1],
                   ser_ln3_d[1]);
        if (bf_lld_dev_is_tof2(dev_id)) {
          aim_printf(&uc->pvs,
                     "%s|%s|%s|%s|",
                     ser_ln4_d[1],
                     ser_ln5_d[1],
                     ser_ln6_d[1],
                     ser_ln7_d[1]);
        }
        aim_printf(&uc->pvs, "\n");
        print_count++;
      }
    } else if (next_port_info_ptr->fec_type == BF_FEC_TYP_REED_SOLOMON) {
      print_count++;
      pm_ucli_speed_to_display_get(next_port_info_ptr->speed,
                                   display_speed,
                                   sizeof(display_speed),
                                   next_port_info_ptr->n_lanes);
      pm_ucli_fec_to_display_get(
          next_port_info_ptr->fec_type, display_fec, sizeof(display_fec));

      aim_printf(&uc->pvs,
                 "%-5s|%2" PRIu64 "/%1" PRIu64 "|%3" PRIu64
                 ""
                 "|%s|%s|%s|%-5d|%-8d|%-10" PRIu64 "|%-8" PRIu64
                 ""
                 "|%-8" PRIu64 "|%-8" PRIu64 "|%-8" PRIu64 "|%-8" PRIu64,
                 next_port_info_ptr->pltfm_port_info.port_str,
                 (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_id,
                 (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_lane,
                 (uint64_t)next_port_info_ptr->dev_port,
                 display_speed,
                 display_fec,
                 next_port_info_ptr->oper_status ? "UP " : "DWN",
                 next_port_info_ptr->fec_info.hi_ser,
                 next_port_info_ptr->fec_info.fec_align_status,
                 (uint64_t)next_port_info_ptr->fec_info.fec_uncorr_cnt,
                 (uint64_t)next_port_info_ptr->fec_info.fec_corr_cnt,
                 (uint64_t)next_port_info_ptr->fec_info.fec_ser_lane_0,
                 (uint64_t)next_port_info_ptr->fec_info.fec_ser_lane_1,
                 (uint64_t)next_port_info_ptr->fec_info.fec_ser_lane_2,
                 (uint64_t)next_port_info_ptr->fec_info.fec_ser_lane_3);
      if (bf_lld_dev_is_tof2(dev_id)) {
        aim_printf(&uc->pvs,
                   "|%-8" PRIu64 "|%-8" PRIu64 "|%-8" PRIu64 "|%-8" PRIu64,
                   (uint64_t)next_port_info_ptr->fec_info.fec_ser_lane_4,
                   (uint64_t)next_port_info_ptr->fec_info.fec_ser_lane_5,
                   (uint64_t)next_port_info_ptr->fec_info.fec_ser_lane_6,
                   (uint64_t)next_port_info_ptr->fec_info.fec_ser_lane_7);
      }
      aim_printf(&uc->pvs, "\n");
    }
    // print the banner
    if (print_count >= 40) {
      aim_printf(&uc->pvs,
                 "%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s\n",
                 port_d[1],
                 mac_d[1],
                 dev_port_d[1],
                 speed_d[1],
                 fec_d[1],
                 oper_d[1],
                 hiser_d[1],
                 algnsts_d[1],
                 uncorr_fec_d[1],
                 corr_fec_d[1],
                 ser_ln0_d[1],
                 ser_ln1_d[1],
                 ser_ln2_d[1],
                 ser_ln3_d[1]);

      aim_printf(&uc->pvs,
                 "%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s\n",
                 port_d[0],
                 mac_d[0],
                 dev_port_d[0],
                 speed_d[0],
                 fec_d[0],
                 oper_d[0],
                 hiser_d[0],
                 algnsts_d[0],
                 uncorr_fec_d[0],
                 corr_fec_d[0],
                 ser_ln0_d[0],
                 ser_ln1_d[0],
                 ser_ln2_d[0],
                 ser_ln3_d[0]);

      aim_printf(&uc->pvs,
                 "%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s\n",
                 port_d[1],
                 mac_d[1],
                 dev_port_d[1],
                 speed_d[1],
                 fec_d[1],
                 oper_d[1],
                 hiser_d[1],
                 algnsts_d[1],
                 uncorr_fec_d[1],
                 corr_fec_d[1],
                 ser_ln0_d[1],
                 ser_ln1_d[1],
                 ser_ln2_d[1],
                 ser_ln3_d[1]);

      print_count = 0;
    }
  }
}

static ucli_status_t bf_pm_ucli_ucli__port_error_clear__(ucli_context_t *uc) {
  static char usage[] = "port-error-clear -a -p <conn_id/chnl>";
  UCLI_COMMAND_INFO(uc, "port-error-clear", -1, " -a -p <port_str>");

  extern char *optarg;
  extern int optind;
  int c, pflag, aflag, dflag;
  char *p_arg;
  int argc;
  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  bf_status_t sts;

  pflag = aflag = dflag = 0;
  p_arg = NULL;
  optind = 0;  // reset optind value
  argc = (uc->pargs->count + 1);
  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  if (!bf_lld_dev_is_tof1(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }
  while ((c = getopt(argc, argv, "p:a")) != -1) {
    switch (c) {
      case 'p':
        pflag = 1;
        p_arg = optarg;
        if (!p_arg) {
          aim_printf(&uc->pvs, "Usage : %s\n", usage);
          return UCLI_STATUS_OK;
        }
        sts = bf_pm_port_str_to_hdl_get(dev_id, p_arg, &port_hdl);
        if (sts != BF_SUCCESS) {
          aim_printf(&uc->pvs, "Usage : %s\n", usage);
          return 0;
        }
        break;
      case 'a':
        aflag = 1;
        break;
    }
  }

  /* Clear FEC related error counters */
  pm_ucli_port_clear_fec_counters(dev_id, aflag, &port_hdl);

  (void)pflag;
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_error_show__(ucli_context_t *uc) {
  static char usage[] = "port-error-show -a -p <conn_id/chnl>";
  UCLI_COMMAND_INFO(uc, "port-error-show", -1, "-a -p <port_str>");

  extern char *optarg;
  extern int optind;
  int c, pflag, aflag, dflag;
  char *p_arg;
  int argc;
  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  bf_status_t sts;

  pflag = aflag = dflag = 0;
  p_arg = NULL;
  optind = 0;  // reset optind value
  argc = (uc->pargs->count + 1);
  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  if (!(bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof2(dev_id))) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }
  while ((c = getopt(argc, argv, "p:a")) != -1) {
    switch (c) {
      case 'p':
        pflag = 1;
        p_arg = optarg;
        if (!p_arg) {
          aim_printf(&uc->pvs, "Usage : %s\n", usage);
          return UCLI_STATUS_OK;
        }
        sts = bf_pm_port_str_to_hdl_get(dev_id, p_arg, &port_hdl);
        if (sts != BF_SUCCESS) {
          aim_printf(&uc->pvs, "Usage : %s\n", usage);
          return 0;
        }
        break;
      case 'a':
        aflag = 1;
        break;
    }
  }

  /* Display corresponding errors */
  pm_ucli_display_port_rs_fec_counters(uc, dev_id, aflag, &port_hdl);
  pm_ucli_display_port_fc_fec_counters(uc, dev_id, aflag, &port_hdl);

  (void)pflag;
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_stats_clear__(ucli_context_t *uc) {
  static char usage[] = "port-stats-clr <port_str>";
  UCLI_COMMAND_INFO(uc, "port-stats-clr", 1, "<port_str>");
  bf_status_t sts;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    (void)dev_port;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;

    bf_pm_port_all_stats_clear(dev_id, port_hdl_ptr);
  }
  bf_sys_free((void *)list);
  return 0;
}

/**
 * Utility fn for port-error-get ucli (log rs fec)
 */
static void pm_ucli_port_error_get_dump_rs(ucli_context_t *uc,
                                           bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port) {
  bf_pm_port_info_t *port_info;
  uint32_t ser_lane_0 = 0;
  uint32_t ser_lane_1 = 0;
  uint32_t ser_lane_2 = 0;
  uint32_t ser_lane_3 = 0;
  uint32_t ser_lane_4 = 0;
  uint32_t ser_lane_5 = 0;
  uint32_t ser_lane_6 = 0;
  uint32_t ser_lane_7 = 0;
  uint32_t uncorr_cnt;
  uint32_t *ser_lane;
  uint32_t corr_cnt;
  bool align_status;
  bf_status_t sts;
  uint32_t lane;
  bool hi_ser;

  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) return;

  sts = bf_pm_port_get_rs_fec_counters(
      dev_id, dev_port, &hi_ser, &align_status, &corr_cnt, &uncorr_cnt);
  if (sts != BF_SUCCESS) return;

  for (lane = 0; lane < port_info->n_lanes; lane++) {
    switch (lane) {
      case 0:
        ser_lane = &ser_lane_0;
        break;
      case 1:
        ser_lane = &ser_lane_1;
        break;
      case 2:
        ser_lane = &ser_lane_2;
        break;
      case 3:
        ser_lane = &ser_lane_3;
        break;
      case 4:
        ser_lane = &ser_lane_4;
        break;
      case 5:
        ser_lane = &ser_lane_5;
        break;
      case 6:
        ser_lane = &ser_lane_6;
        break;
      case 7:
        ser_lane = &ser_lane_7;
        break;
      default:
        return;
    }
    sts = bf_pm_port_get_rs_fec_ser_lane_cnt(dev_id, dev_port, lane, ser_lane);
    if (sts != BF_SUCCESS) return;
  }

  aim_printf(&uc->pvs, " dev_port:%3d\n", dev_port);
  aim_printf(&uc->pvs,
             "  fec_type:RS, aligned: %c, hi_ser: %c\n",
             align_status ? 'Y' : 'N',
             hi_ser ? 'Y' : 'N');
  aim_printf(&uc->pvs,
             "  corrected blocks:%-6d  uncorrected blocks:%-6d\n",
             corr_cnt,
             uncorr_cnt);
  aim_printf(&uc->pvs, "  block symb errors per lane 0:%-6d", ser_lane_0);

  if (port_info->n_lanes > 1) {
    aim_printf(&uc->pvs, "  1:%-6d", ser_lane_1);

    if (port_info->n_lanes > 2) {
      aim_printf(&uc->pvs, "  2:%-6d  3:%-6d", ser_lane_2, ser_lane_3);
    }
    if (port_info->n_lanes > 4) {
      aim_printf(&uc->pvs,
                 "  4:%-6d  5:%-6d 6:%-6d 7:%-6d",
                 ser_lane_4,
                 ser_lane_5,
                 ser_lane_6,
                 ser_lane_7);
    }
  }
  aim_printf(&uc->pvs, "\n\n");
}

/**
 * Utility fn for port-error-get ucli (log fc fec)
 */
static void pm_ucli_port_error_get_dump_fc(ucli_context_t *uc,
                                           bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port) {
  uint32_t uncorr_blk_cnt;
  uint32_t corr_blk_cnt;
  bool block_lock;
  bf_status_t sts;

  sts = bf_pm_port_get_fc_fec_counters(
      dev_id, dev_port, &block_lock, &corr_blk_cnt, &uncorr_blk_cnt);
  if (sts != BF_SUCCESS) return;

  aim_printf(&uc->pvs, " dev_port:%3d\n", dev_port);
  aim_printf(
      &uc->pvs, "  fec_type:FC, block lock: %c\n", block_lock ? 'Y' : 'N');
  aim_printf(&uc->pvs,
             "  corrected blocks:%-6d  uncorrected blocks:%-6d\n\n",
             corr_blk_cnt,
             uncorr_blk_cnt);
}

/**
 * Utility fn for port-error-get ucli (log pcs errors)
 */
static void pm_ucli_port_error_get_dump_pcs(ucli_context_t *uc,
                                            bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port) {
  uint32_t errored_blk_cnt;
  uint32_t ber_cnt;
  bf_status_t sts;

  sts =
      bf_pm_port_get_pcs_counters(dev_id, dev_port, &ber_cnt, &errored_blk_cnt);

  if (sts != BF_SUCCESS) return;

  aim_printf(&uc->pvs, " dev_port:%2d\n", dev_port);
  aim_printf(&uc->pvs, "  fec_type:NO\n");
  aim_printf(&uc->pvs,
             "  bad sync errors:%-6d   errored blocks:%-6d\n\n",
             ber_cnt,
             errored_blk_cnt);
}

static ucli_status_t bf_pm_ucli_ucli__port_error_get__(ucli_context_t *uc) {
  static char usage[] = "port-error-get <port_str>";
  UCLI_COMMAND_INFO(uc, "port-error-get", 1, "<port_str>");

  bf_pal_front_port_handle_t *port_hdl_ptr;
  bf_pal_front_port_handle_t *list = NULL;
  bf_pm_port_info_t *port_info;
  bf_dev_port_t dev_port;
  bf_dev_id_t dev_id = 0;
  uint32_t list_sz = 0;
  bf_status_t sts;

  if (!(bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof2(dev_id))) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }

  /* For the time being, this command is just for internal use, the output
   * format may be changed.
   */
  aim_printf(&uc->pvs, "This command is for internal use only\n");

  /* get list of port_hdls */
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    (void)dev_port;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;

    port_info = pm_port_info_get(dev_id, dev_port);
    if (!port_info) continue;
    if (!port_info->is_added) continue;
    if (port_info->admin_state == PM_PORT_DISABLED) continue;

    if (port_info->fec_type == BF_FEC_TYP_REED_SOLOMON) {
      pm_ucli_port_error_get_dump_rs(uc, dev_id, dev_port);
    } else if (port_info->fec_type == BF_FEC_TYP_FIRECODE) {
      pm_ucli_port_error_get_dump_fc(uc, dev_id, dev_port);
    } else {
      pm_ucli_port_error_get_dump_pcs(uc, dev_id, dev_port);
    }
  }

  bf_sys_free((void *)list);
  return 0;
}

static bf_port_prbs_speed_t pm_ucli_prbs_speed_get(const char *str) {
  if (strcmp(str, "10G") == 0) {
    return BF_PORT_PRBS_SPEED_10G;
  } else if (strcmp(str, "25G") == 0) {
    return BF_PORT_PRBS_SPEED_25G;
  }
  return BF_PORT_PRBS_SPEED_MAX;
}

static bf_port_prbs_mode_t pm_ucli_prbs_mode_get(int mode) {
  switch (mode) {
    case 31:
      return BF_PORT_PRBS_MODE_31;
      break;
    case 23:
      return BF_PORT_PRBS_MODE_23;
      break;
    case 15:
      return BF_PORT_PRBS_MODE_15;
      break;
    case 13:
      return BF_PORT_PRBS_MODE_13;
      break;
    case 11:
      return BF_PORT_PRBS_MODE_11;
      break;
    case 9:
      return BF_PORT_PRBS_MODE_9;
      break;
    case 7:
      return BF_PORT_PRBS_MODE_7;
      break;
    default:
      return BF_PORT_PRBS_MODE_MAX;
  }
  return BF_PORT_PRBS_MODE_MAX;
}

static char *pm_ucli_prbs_mode_str_get(bf_port_prbs_mode_t prbs_mode) {
  switch (prbs_mode) {
    case BF_PORT_PRBS_MODE_31:
      return "PRBS31";
      break;
    case BF_PORT_PRBS_MODE_23:
      return "PRBS23";
      break;
    case BF_PORT_PRBS_MODE_15:
      return "PRBS15";
      break;
    case BF_PORT_PRBS_MODE_13:
      return "PRBS13";
      break;
    case BF_PORT_PRBS_MODE_11:
      return "PRBS11";
      break;
    case BF_PORT_PRBS_MODE_9:
      return "PRBS9";
      break;
    case 7:
      return "PRBS7";
      break;
    default:
      return "";
  }
  return "";
}

static ucli_status_t bf_pm_ucli_ucli__port_prbs_set__(ucli_context_t *uc) {
  static char usage[] =
      "prbs-set <port_str> <prbs_speed: 10G, 25G> <prbs_mode: "
      "31, "
      "23, 15, 13, 11, 9, 7>";
  UCLI_COMMAND_INFO(uc,
                    "prbs-set",
                    3,
                    "<port_str> <prbs_speed: 10G, 25G> "
                    "<prbs_mode: 31, 23, 15, 13, 11, 9, 7>");
  bf_status_t sts;
  int prbs_speed;
  bf_port_prbs_mode_t prbs_mode;
  int in_prbs_mode;
  bf_dev_id_t dev_id;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  dev_id = 0;
  if (!bf_lld_dev_is_tof1(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }

  prbs_speed = pm_ucli_prbs_speed_get(uc->pargs->args[1]);
  in_prbs_mode = strtoul(uc->pargs->args[2], NULL, 10);
  prbs_mode = pm_ucli_prbs_mode_get(in_prbs_mode);

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }

  sts = bf_pm_port_prbs_set(dev_id, list, list_sz, prbs_speed, prbs_mode);

  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
  }
  bf_sys_free(list);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_prbs_cleanup__(ucli_context_t *uc) {
  static char usage[] = "prbs-clnup <port_str>";
  UCLI_COMMAND_INFO(uc, "prbs-clnup", 1, "<port_str>");
  bf_status_t sts;
  bf_dev_id_t dev_id = 0;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  if (!bf_lld_dev_is_tof1(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }

  sts = bf_pm_port_prbs_cleanup(dev_id, list, list_sz);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
  }
  bf_sys_free(list);
  return 0;
}

extern void bf_pm_ucli_tof2_serdes_state_display(uint32_t fp,
                                                 uint32_t chnl,
                                                 bf_dev_port_t dev_port,
                                                 ucli_context_t *uc);
extern void bf_pm_ucli_tof3_serdes_state_display(uint32_t fp,
                                                 uint32_t chnl,
                                                 bf_dev_port_t dev_port,
                                                 ucli_context_t *uc);

static bf_status_t bf_pm_ucli_print_banner(uint32_t conn_id,
                                           ucli_context_t *uc,
                                           bool *print_banner) {
  (void)conn_id;
  if (*print_banner == false) {
    bf_status_t sts;
    /*aim_printf(&uc->pvs,
               "         ============================== Front panel "
               "Connector : %d =============================\n",
               conn_id);*/
    sts = bf_serdes_prbs_stats_banner_display(uc);
    if (sts != BF_SUCCESS) {
      return sts;
    }
    *print_banner = true;
  }
  return BF_SUCCESS;
}

static bf_status_t bf_pm_ucli_tof2_chnl_sd_show(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl, void *uc) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      if (bf_lld_dev_is_tof3(dev_id)) {
        bf_pm_ucli_tof3_serdes_state_display(  // port_hdl->conn_id,
            next_port_info_ptr->pltfm_port_info.port_hdl.conn_id,
            next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id,
            next_port_info_ptr->dev_port,
            uc);
      } else {
        bf_pm_ucli_tof2_serdes_state_display(port_hdl->conn_id,
                                             port_hdl->chnl_id,
                                             next_port_info_ptr->dev_port,
                                             uc);
      }
    } else {
      continue;
    }
  }
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_sd_show__(ucli_context_t *uc) {
  static char usage[] = "port-sd-show <port_str>";
  UCLI_COMMAND_INFO(uc, "port-sd-show", 1, "<port_str>");
  bf_status_t sts;
  bf_dev_id_t dev_id = 0;
  bf_pal_front_port_handle_t port_hdl, *port_hdl_ptr;
  bool print_banner = false;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;
  bool is_sw_model = true;

  sts = bf_drv_device_type_get(dev_id, &is_sw_model);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs,
               "Unable to get the device type for dev : %d : %s (%d)\n",
               dev_id,
               bf_err_str(sts),
               sts);
  }
  if (is_sw_model) {
    aim_printf(&uc->pvs, "Command is disabled on model, dev %d\n", dev_id);
    return 0;
  }

  port_hdl_ptr = &port_hdl;

  sts = bf_pm_port_str_to_hdl_get(dev_id, uc->pargs->args[0], port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }

  // if (bf_lld_dev_is_tof3(dev_id)) {
  //  aim_printf(&uc->pvs, "Command not supported on this chip\n");
  //  return 0;
  //}

  if ((bf_lld_dev_is_tof2(dev_id)) || (bf_lld_dev_is_tof3(dev_id))) {
    bf_pm_ucli_tof2_chnl_sd_show(dev_id, port_hdl_ptr, uc);
    return 0;
  }

  sts = bf_pm_ucli_print_banner(port_hdl_ptr->conn_id, uc, &print_banner);
  if (sts != BF_SUCCESS) {
    return sts;
  }

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    return -1;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_port_t dev_port;
    bool is_port_internal = false;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;

    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      if (port_hdl_ptr->chnl_id == 0) {
        aim_printf(&uc->pvs,
                   "\nPort %d/%d\n",
                   port_hdl_ptr->conn_id,
                   port_hdl_ptr->chnl_id);
        print_banner = false;  // messed up parm def
        bf_pm_ucli_print_banner(port_hdl_ptr->conn_id, uc, &print_banner);
      }
      port_diag_prbs_stats_display(dev_id, dev_port, uc);
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_sd_cfg_set__(ucli_context_t *uc) {
  static char usage[] = "port-sds-cfg <port_str>";
  UCLI_COMMAND_INFO(uc, "port-sds-cfg", 1, "<port_str>");
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl, *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  port_hdl_ptr = &port_hdl;

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return -1;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    bool is_port_internal = false;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;
    if (!bf_lld_dev_is_tof1(dev_id)) continue;  // cmd only spptd on tof1

    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      bf_pm_port_serdes_cfg_set(dev_id, port_hdl_ptr);
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_sd_perf__(ucli_context_t *uc) {
  static char usage[] = "port-sd-perf <port_str>";
  UCLI_COMMAND_INFO(uc, "port-sd-perf", 1, "<port_str>");
  bf_status_t sts;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return -1;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    bool is_port_internal = false;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;
    if (!bf_lld_dev_is_tof1(dev_id)) continue;  // cmd only spptd on tof1

    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      port_diag_perf_display(
          dev_id, dev_port, port_hdl_ptr->conn_id, port_hdl_ptr->chnl_id, uc);
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_sd_plot_eye__(ucli_context_t *uc) {
  static char usage[] = "port-sd-plot-eye <port_str>";
  UCLI_COMMAND_INFO(uc, "port-sd-plot-eye", 1, "<port_str>");
  bf_status_t sts;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return UCLI_STATUS_E_ARG;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    bool is_port_internal = false;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;
    if (!bf_lld_dev_is_tof1(dev_id)) continue;  // cmd only spptd on tof1

    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      port_diag_plot_eye(dev_id, dev_port, uc);
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

static ucli_status_t bf_pm_ucli_print_notif_sd_dfe_set(ucli_context_t *uc) {
  static char usage[] =
      "port-sd-dfe-set <port_str> <lane/all> <dfe-ctrl> <hf_val> <lf_val> "
      "<dc_val>";
  aim_printf(&uc->pvs, "Usage : %s\n", usage);
  aim_printf(&uc->pvs, "dfe_ctrl is OR of:\n");
  aim_printf(&uc->pvs, "  0  = DEFAULT\n");
  aim_printf(&uc->pvs, "  0  = ICAL (note: default)\n");
  aim_printf(&uc->pvs, "  1  = PCAL\n");
  aim_printf(&uc->pvs, "  2  = SEEDED_HF\n");
  aim_printf(&uc->pvs, "  4  = SEEDED_LF\n");
  aim_printf(&uc->pvs, "  8  = SEEDED_DC\n");
  aim_printf(&uc->pvs, "  16 = FIXED_HF\n");
  aim_printf(&uc->pvs, "  32 = FIXED_LF\n");
  aim_printf(&uc->pvs, "  64 = FIXED_DC\n");
  aim_printf(&uc->pvs,
             "       port-sd-dfe-set <port_str> all 0 0 0 0(DEFAULT)\n");
  aim_printf(&uc->pvs,
             "       port-sd-dfe-set <port_str> all 16 15 0 0(FIXED_HF=15)\n");
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_sd_dfe_set__(ucli_context_t *uc) {
  // static char usage[] = "port-sd-dfe-set <port_str> <lane/all> <dfe-ctrl>
  // <hf_val> <lf_val> <dc_val>";
  UCLI_COMMAND_INFO(
      uc,
      "port-sd-dfe-set",
      6,
      "<port_str> <lane/all> <dfe-ctrl><hf_val> <lf_val> <dc_val>");
  bf_status_t sts;
  uint32_t dfe_ctrl, hf_val, lf_val, dc_val;
  uint32_t lane_no = 0;  // only configure which lane
  bool set_all_lane = false;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  if (uc->pargs->count != 6) {
    bf_pm_ucli_print_notif_sd_dfe_set(uc);
    return 0;
  }
  if (strcmp(uc->pargs->args[1], "all") == 0) {
    set_all_lane = true;
  } else {
    lane_no = atoi(uc->pargs->args[1]);
    if (lane_no > 3) {  // roughly check para 1
      bf_pm_ucli_print_notif_sd_dfe_set(uc);
      return 0;
    }
  }
  dfe_ctrl = atoi(uc->pargs->args[2]);
  hf_val = atoi(uc->pargs->args[3]);
  lf_val = atoi(uc->pargs->args[4]);
  dc_val = atoi(uc->pargs->args[5]);

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    bf_pm_ucli_print_notif_sd_dfe_set(uc);
    return -1;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    bool is_port_internal = false;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;
    if (!bf_lld_dev_is_tof1(dev_id)) continue;  // cmd only spptd on tof1

    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      port_diag_dfe_set(dev_id,
                        dev_port,
                        lane_no,
                        set_all_lane,
                        dfe_ctrl,
                        hf_val,
                        lf_val,
                        dc_val,
                        uc);
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

static bf_status_t bf_pm_ucli_chnl_sd_ical_set(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl_ptr,
    uint32_t lane_no,
    bool set_all_lane,
    void *uc) {
  bf_status_t sts;
  uint32_t chnl_begin, chnl_end;
  uint32_t j;
  bf_dev_port_t dev_port;
  bool is_port_internal = false;
  bf_dev_id_t dev_id_of_port = 0;

  if (port_hdl_ptr->chnl_id == (uint32_t)-1) {
    chnl_begin = 0;
    port_hdl_ptr->chnl_id = 0;
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id_of_port, &dev_port);
    if (sts == BF_SUCCESS) {
      dev_id = dev_id_of_port;
      chnl_end = lld_get_chnls_dev_port(dev_id, dev_port) - 1;
    } else {
      return sts;
    }
    for (j = chnl_begin; j <= chnl_end; j++) {
      port_hdl_ptr->chnl_id = j;
      bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
      if (!is_port_internal) {
        sts = bf_pm_port_front_panel_port_to_dev_port_get(
            port_hdl_ptr, &dev_id_of_port, &dev_port);
        if (sts != BF_SUCCESS) {
          continue;
        }
        dev_id = dev_id_of_port;

        port_diag_dfe_ical_set(dev_id, dev_port, lane_no, set_all_lane, uc);
      }
    }
  } else {
    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      sts = bf_pm_port_front_panel_port_to_dev_port_get(
          port_hdl_ptr, &dev_id_of_port, &dev_port);
      if (sts != BF_SUCCESS) {
        return sts;
      }
      dev_id = dev_id_of_port;

      port_diag_dfe_ical_set(dev_id, dev_port, lane_no, set_all_lane, uc);
    }
  }
  return BF_SUCCESS;
}
static ucli_status_t bf_pm_ucli_ucli__port_sd_dfe_ical__(ucli_context_t *uc) {
  static char usage[] = "port-sd-dfe-ical <port_str> <lane/all>";
  UCLI_COMMAND_INFO(uc, "port-sd-dfe-ical", 2, "<port_str> <lane/all>");
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr, *next_port_hdl_ptr;
  bf_dev_id_t dev_id;
  uint32_t lane_no = 0;  // only configure which lane
  bool set_all_lane = false;

  port_hdl_ptr = &port_hdl;
  next_port_hdl_ptr = &next_port_hdl;

  dev_id = 0;
  if (!bf_lld_dev_is_tof1(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }
  if (uc->pargs->count != 2) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }
  if (strcmp(uc->pargs->args[1], "all") == 0) {
    set_all_lane = true;
  } else {
    lane_no = atoi(uc->pargs->args[1]);
    if (lane_no > 3) {  // roughly check para 1
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
  }
  sts = bf_pm_port_str_to_hdl_get(dev_id, uc->pargs->args[0], port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }

  if (port_hdl_ptr->conn_id == (uint32_t)-1) {
    // all ports
    // Get the first port in the system
    sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
    if (sts != BF_SUCCESS || !port_hdl_ptr) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
    sts = bf_pm_ucli_chnl_sd_ical_set(
        dev_id, port_hdl_ptr, lane_no, set_all_lane, uc);
    if (sts != BF_SUCCESS) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
    while (sts == BF_SUCCESS) {
      // Get the next port in the system
      sts = bf_pm_port_front_panel_port_get_next(
          dev_id, port_hdl_ptr, next_port_hdl_ptr);
      if (sts == BF_OBJECT_NOT_FOUND) break;
      if (sts != BF_SUCCESS) {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
      sts = bf_pm_ucli_chnl_sd_ical_set(
          dev_id, next_port_hdl_ptr, lane_no, set_all_lane, uc);
      if (sts != BF_SUCCESS) {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
      // Make the curr port hdl equal to the next port hdl
      port_hdl_ptr = next_port_hdl_ptr;
    }
  } else {
    sts = bf_pm_ucli_chnl_sd_ical_set(
        dev_id, port_hdl_ptr, lane_no, set_all_lane, uc);
    if (sts != BF_SUCCESS) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
  }
  return 0;
}

static bf_status_t bf_pm_ucli_chnl_sd_pcal_set(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl_ptr,
    uint32_t lane_no,
    bool set_all_lane,
    void *uc) {
  bf_status_t sts;
  uint32_t chnl_begin, chnl_end;
  uint32_t j;
  bf_dev_port_t dev_port;
  bool is_port_internal = false;
  bf_dev_id_t dev_id_of_port = 0;

  if (port_hdl_ptr->chnl_id == (uint32_t)-1) {
    chnl_begin = 0;
    port_hdl_ptr->chnl_id = 0;
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id_of_port, &dev_port);
    if (sts == BF_SUCCESS) {
      dev_id = dev_id_of_port;
      chnl_end = lld_get_chnls_dev_port(dev_id, dev_port) - 1;
    } else {
      return sts;
    }
    for (j = chnl_begin; j <= chnl_end; j++) {
      port_hdl_ptr->chnl_id = j;
      bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
      if (!is_port_internal) {
        sts = bf_pm_port_front_panel_port_to_dev_port_get(
            port_hdl_ptr, &dev_id_of_port, &dev_port);
        if (sts != BF_SUCCESS) {
          continue;
        }
        dev_id = dev_id_of_port;

        port_diag_dfe_pcal_set(dev_id, dev_port, lane_no, set_all_lane, uc);
      }
    }
  } else {
    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      sts = bf_pm_port_front_panel_port_to_dev_port_get(
          port_hdl_ptr, &dev_id_of_port, &dev_port);
      if (sts != BF_SUCCESS) {
        return sts;
      }
      dev_id = dev_id_of_port;

      port_diag_dfe_pcal_set(dev_id, dev_port, lane_no, set_all_lane, uc);
    }
  }
  return BF_SUCCESS;
}

static ucli_status_t bf_pm_ucli_ucli__port_sd_dfe_pcal__(ucli_context_t *uc) {
  static char usage[] = "port-sd-dfe-pcal <port_str> <lane/all>";
  UCLI_COMMAND_INFO(uc, "port-sd-dfe-pcal", 2, "<port_str> <lane/all>");
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr, *next_port_hdl_ptr;
  bf_dev_id_t dev_id;
  uint32_t lane_no = 0;  // only configure which lane
  bool set_all_lane = false;
  port_hdl_ptr = &port_hdl;
  next_port_hdl_ptr = &next_port_hdl;

  dev_id = 0;
  if (!bf_lld_dev_is_tof1(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }

  if (uc->pargs->count != 2) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }
  if (strcmp(uc->pargs->args[1], "all") == 0) {
    set_all_lane = true;
  } else {
    lane_no = atoi(uc->pargs->args[1]);
    if (lane_no > 3) {  // roughly check para 1
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
  }
  sts = bf_pm_port_str_to_hdl_get(dev_id, uc->pargs->args[0], port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }

  if (port_hdl_ptr->conn_id == (uint32_t)-1) {
    // all ports
    // Get the first port in the system
    sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
    if (sts != BF_SUCCESS || !port_hdl_ptr) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
    sts = bf_pm_ucli_chnl_sd_pcal_set(
        dev_id, port_hdl_ptr, lane_no, set_all_lane, uc);
    if (sts != BF_SUCCESS) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
    while (sts == BF_SUCCESS) {
      // Get the next port in the system
      sts = bf_pm_port_front_panel_port_get_next(
          dev_id, port_hdl_ptr, next_port_hdl_ptr);
      if (sts == BF_OBJECT_NOT_FOUND) break;
      if (sts != BF_SUCCESS) {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
      sts = bf_pm_ucli_chnl_sd_pcal_set(
          dev_id, next_port_hdl_ptr, lane_no, set_all_lane, uc);
      if (sts != BF_SUCCESS) {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
      // Make the curr port hdl equal to the next port hdl
      port_hdl_ptr = next_port_hdl_ptr;
    }
  } else {
    sts = bf_pm_ucli_chnl_sd_pcal_set(
        dev_id, port_hdl_ptr, lane_no, set_all_lane, uc);
    if (sts != BF_SUCCESS) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
  }
  return 0;
}

static bf_status_t bf_pm_ucli_chnl_sd_chg_to_prbs(
    bf_dev_id_t dev_id, bf_pal_front_port_handle_t *port_hdl_ptr, void *uc) {
  bf_status_t sts;
  uint32_t chnl_begin, chnl_end;
  uint32_t j;
  bf_dev_port_t dev_port;
  bool is_port_internal = false;
  bf_dev_id_t dev_id_of_port = 0;

  if (port_hdl_ptr->chnl_id == (uint32_t)-1) {
    chnl_begin = 0;
    port_hdl_ptr->chnl_id = 0;
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id_of_port, &dev_port);
    if (sts == BF_SUCCESS) {
      dev_id = dev_id_of_port;
      chnl_end = lld_get_chnls_dev_port(dev_id, dev_port) - 1;
    } else {
      return sts;
    }
    for (j = chnl_begin; j <= chnl_end; j++) {
      port_hdl_ptr->chnl_id = j;
      bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
      if (!is_port_internal) {
        sts = bf_pm_port_front_panel_port_to_dev_port_get(
            port_hdl_ptr, &dev_id_of_port, &dev_port);
        if (sts != BF_SUCCESS) {
          continue;
        }
        dev_id = dev_id_of_port;

        port_diag_chg_to_prbs(dev_id, dev_port, uc);
      }
    }
  } else {
    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      sts = bf_pm_port_front_panel_port_to_dev_port_get(
          port_hdl_ptr, &dev_id_of_port, &dev_port);
      if (sts != BF_SUCCESS) {
        return sts;
      }
      dev_id = dev_id_of_port;

      port_diag_chg_to_prbs(dev_id, dev_port, uc);
    }
  }
  return BF_SUCCESS;
}
static ucli_status_t bf_pm_ucli_ucli__port_sd_chg_to_prbs__(
    ucli_context_t *uc) {
  static char usage[] = "port-sd-chg-to-prbs <port_str>";
  UCLI_COMMAND_INFO(uc, "port-sd-chg-to-prbs", 1, "<port_str>");
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr, *next_port_hdl_ptr;
  bf_dev_id_t dev_id;

  port_hdl_ptr = &port_hdl;
  next_port_hdl_ptr = &next_port_hdl;
  dev_id = 0;
  if (!bf_lld_dev_is_tof1(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }

  sts = bf_pm_port_str_to_hdl_get(dev_id, uc->pargs->args[0], port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }

  if (port_hdl_ptr->conn_id == (uint32_t)-1) {
    // all ports
    // Get the first port in the system
    sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
    if (sts != BF_SUCCESS || !port_hdl_ptr) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
    sts = bf_pm_ucli_chnl_sd_chg_to_prbs(dev_id, port_hdl_ptr, uc);
    if (sts != BF_SUCCESS) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
    while (sts == BF_SUCCESS) {
      // Get the next port in the system
      sts = bf_pm_port_front_panel_port_get_next(
          dev_id, port_hdl_ptr, next_port_hdl_ptr);
      if (sts == BF_OBJECT_NOT_FOUND) break;
      if (sts != BF_SUCCESS) {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
      sts = bf_pm_ucli_chnl_sd_chg_to_prbs(dev_id, next_port_hdl_ptr, uc);
      if (sts != BF_SUCCESS) {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
      // Make the curr port hdl equal to the next port hdl
      port_hdl_ptr = next_port_hdl_ptr;
    }
  } else {
    sts = bf_pm_ucli_chnl_sd_chg_to_prbs(dev_id, port_hdl_ptr, uc);
    if (sts != BF_SUCCESS) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
  }
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_auto_neg_set__(ucli_context_t *uc) {
  static char usage[] = "an-set <port_str> <0->auto,1->enable,2->disable>";
  UCLI_COMMAND_INFO(
      uc, "an-set", 2, "<port_str> <0->auto,1->enable,2->disable>");
  bf_status_t sts;
  uint32_t an;
  bf_pm_port_autoneg_policy_e an_policy;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  if ((uc->pargs->args[1][0] == 'e') || (uc->pargs->args[1][0] == 'E')) {
    an_policy = PM_AN_FORCE_ENABLE;
  } else if ((uc->pargs->args[1][0] == 'd') || (uc->pargs->args[1][0] == 'D')) {
    an_policy = PM_AN_FORCE_DISABLE;
  } else if ((uc->pargs->args[1][0] == 'a') || (uc->pargs->args[1][0] == 'A')) {
    an_policy = PM_AN_DEFAULT;
  } else {
    an = strtoul(uc->pargs->args[1], NULL, 10);
    an_policy = an == 0 ? PM_AN_DEFAULT
                        : an == 1 ? PM_AN_FORCE_ENABLE : PM_AN_FORCE_DISABLE;
  }

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return -1;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    bool is_port_internal = false;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;

    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      bf_pm_port_autoneg_set(dev_id, port_hdl_ptr, an_policy);
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_kr_mode_set__(ucli_context_t *uc) {
  static char usage[] = "kr-set <port_str> <0->auto,1->enable,2->disable>";
  UCLI_COMMAND_INFO(
      uc, "kr-set", 2, "<port_str> <0->auto,1->enable,2->disable>");
  bf_status_t sts;
  uint32_t kr;
  bf_pm_port_kr_mode_policy_e kr_policy;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  if ((uc->pargs->args[1][0] == 'e') || (uc->pargs->args[1][0] == 'E')) {
    kr_policy = PM_KR_FORCE_ENABLE;
  } else if ((uc->pargs->args[1][0] == 'd') || (uc->pargs->args[1][0] == 'D')) {
    kr_policy = PM_KR_FORCE_DISABLE;
  } else if ((uc->pargs->args[1][0] == 'a') || (uc->pargs->args[1][0] == 'A')) {
    kr_policy = PM_KR_DEFAULT;
  } else {
    kr = strtoul(uc->pargs->args[1], NULL, 10);
    kr_policy = kr == 0 ? PM_AN_DEFAULT
                        : kr == 1 ? PM_KR_FORCE_ENABLE : PM_KR_FORCE_DISABLE;
  }
  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return -1;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    bool is_port_internal = false;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;

    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      bf_pm_port_kr_mode_set(dev_id, port_hdl_ptr, kr_policy);
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_linkup_max_errors_set__(
    ucli_context_t *uc) {
  static char usage[] = "port-max-err-set <port_str> <max-errors>";
  UCLI_COMMAND_INFO(uc, "port-max-err-set", 2, "<port_str> <max-errors>");
  bf_status_t sts;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;
  bf_dev_port_t dev_port;
  bool is_port_internal;
  uint32_t max_errors;
  bf_dev_id_t dev_id;

  dev_id = 0;
  if (!bf_lld_dev_is_tof1(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }

  max_errors = strtol(uc->pargs->args[1], NULL, 10);
  if (max_errors > 10000) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return UCLI_STATUS_E_ARG;
  }

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return UCLI_STATUS_E_ARG;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;

    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      bf_pm_port_link_up_max_err_set(dev_id, port_hdl_ptr, max_errors);
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_linkup_max_errors_get__(
    ucli_context_t *uc) {
  static char usage[] = "port-max-err-get <port_str>";
  UCLI_COMMAND_INFO(uc, "port-max-err-get", 1, "<port_str>");
  bf_status_t sts;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;
  bool is_port_internal = false;
  uint32_t max_errors = -1;
  bf_dev_port_t dev_port;
  bf_dev_id_t dev_id;

  dev_id = 0;
  if (!bf_lld_dev_is_tof1(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS || list_sz != 1) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return UCLI_STATUS_E_ARG;
  }

  port_hdl_ptr = &list[0];
  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl_ptr, &dev_id, &dev_port);
  if (sts == BF_SUCCESS) {
    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      bf_pm_port_link_up_max_err_get(dev_id, port_hdl_ptr, &max_errors);
      aim_printf(&uc->pvs, "max_errors = %d\n", max_errors);
    }
  }

  bf_sys_free((void *)list);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_linkup_debounce_set__(
    ucli_context_t *uc) {
  static char usage[] = "port-dbnc-set <port_str> <value>";
  UCLI_COMMAND_INFO(uc, "port-dbnc-set", 2, "<port_str> <value>");
  bf_status_t sts;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;
  bf_dev_port_t dev_port;
  bool is_port_internal;
  uint32_t debounce_val;
  bf_dev_id_t dev_id;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }

  debounce_val = strtol(uc->pargs->args[1], NULL, 10);

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return UCLI_STATUS_E_ARG;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;

    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      sts = bf_pm_port_debounce_thresh_set(dev_id, port_hdl_ptr, debounce_val);
      if (sts != BF_SUCCESS) {
        aim_printf(&uc->pvs,
                   "ERROR cannot set link updebounce value for port %d/%d",
                   port_hdl_ptr->conn_id,
                   port_hdl_ptr->chnl_id);
      }
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_linkup_debounce_get__(
    ucli_context_t *uc) {
  static char usage[] = "port-dbnc-get <port_str>";
  UCLI_COMMAND_INFO(uc, "port-dbnc-get", 1, "<port_str>");
  bf_status_t sts;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;
  bool is_port_internal = false;
  bf_dev_port_t dev_port;
  uint32_t debounce_val;
  bf_dev_id_t dev_id;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS || list_sz != 1) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return UCLI_STATUS_E_ARG;
  }

  port_hdl_ptr = &list[0];
  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl_ptr, &dev_id, &dev_port);
  if (sts == BF_SUCCESS) {
    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      sts = bf_pm_port_debounce_thresh_get(dev_id, port_hdl_ptr, &debounce_val);
      if (sts == BF_SUCCESS) {
        aim_printf(&uc->pvs, "debounce_value = %d\n", debounce_val);
      } else {
        aim_printf(&uc->pvs,
                   "ERROR cannot get linkup debounce value for port %d/%d",
                   port_hdl_ptr->conn_id,
                   port_hdl_ptr->chnl_id);
      }
    }
  }

  bf_sys_free((void *)list);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__tof1_port_term_mode_set__(
    ucli_context_t *uc) {
  static char usage[] =
      "port-term-set-tof1 <port_str> <lane-str> <0->DEFAULT, 1->GND, 2->AVDD, "
      "3->FLOAT>";
  UCLI_COMMAND_INFO(
      uc,
      "port-term-set-tof1",
      3,
      "<port_str> <lane-str> <0->DEFAULT, 1->GND, 2->AVDD, 3->FLOAT>");

  bf_status_t sts;
  uint32_t term;
  char lane_str;
  int ln;
  bf_pm_port_term_mode_e term_mode;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  lane_str = uc->pargs->args[1][0];
  if (lane_str == '-') {
    ln = -1;
  } else {
    ln = atoi(uc->pargs->args[1]);
  }
  term = strtoul(uc->pargs->args[2], NULL, 10);
  switch (term) {
    case 0:
      term_mode = PM_TERM_MODE_DEFAULT;
      break;
    case 1:
      term_mode = PM_TERM_MODE_GND;
      break;
    case 2:
      term_mode = PM_TERM_MODE_AVDD;
      break;
    case 3:
      term_mode = PM_TERM_MODE_FLOAT;
      break;
    default:
      aim_printf(&uc->pvs, "Invalid termination mode\n");
      return 0;
  }

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return -1;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    bool is_port_internal = false;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;
    if (!bf_lld_dev_is_tof1(dev_id)) {
      aim_printf(&uc->pvs, "Command not supported on this chip\n");
      break;
    }

    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      bf_pm_port_term_mode_set(dev_id, port_hdl_ptr, ln, term_mode);
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__dfe_retry_timer_set__(
    ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "dfe-retry-tmr-set", 1, "<time in milliseconds>");

  uint32_t dfe_retry_time;
  bf_dev_id_t dev_id = 0;

  if (!(bf_lld_dev_is_tof1(dev_id))) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }

  dfe_retry_time = atoi(uc->pargs->args[0]);
  if (dfe_retry_time > 10000) {
    aim_printf(&uc->pvs, "Maximum allowed dfe retry time is 10 seconds\n");
    return 0;
  }

  bf_pm_set_dfe_retry_time(dfe_retry_time);

  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__dfe_retry_timer_get__(
    ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "dfe-retry-tmr-get", 0, "");

  uint32_t dfe_retry_time;
  bf_dev_id_t dev_id = 0;

  if (!(bf_lld_dev_is_tof1(dev_id))) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }

  dfe_retry_time = bf_pm_get_dfe_retry_time();

  aim_printf(&uc->pvs, "dfe-retry-time = %d milliseconds\n", dfe_retry_time);

  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__tof2_port_term_mode_set__(
    ucli_context_t *uc) {
  static char usage[] =
      "port-term-set-tof2 <port_str> <lane-str> <0->default, 1->AC coupled, "
      "2->DC "
      "coupled>";
  UCLI_COMMAND_INFO(
      uc,
      "port-term-set-tof2",
      3,
      "<port_str> <lane-str> <0->default, 1->AC coupled, 2->DC coupled>");

  bf_status_t sts;
  uint32_t term;
  char lane_str;
  int ln;
  bf_pm_port_term_mode_e term_mode;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  lane_str = uc->pargs->args[1][0];
  if (lane_str == '-') {
    ln = -1;
  } else {
    ln = atoi(uc->pargs->args[1]);
  }
  term = strtoul(uc->pargs->args[2], NULL, 10);
  term_mode = (term == 0) ? PM_TERM_MODE_DEFAULT
                          : (term == 1) ? PM_TERM_MODE_AC : PM_TERM_MODE_DC;

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return -1;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    bool is_port_internal = false;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;
    if (!bf_lld_dev_is_tof2(dev_id)) {
      aim_printf(&uc->pvs, "Command not supported on this chip\n");
      break;
    }

    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      bf_pm_port_term_mode_set(dev_id, port_hdl_ptr, ln, term_mode);
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

// Note: same as tof2 for now, need to add other tof3 termination options later
static ucli_status_t bf_pm_ucli_ucli__tof3_port_term_mode_set__(
    ucli_context_t *uc) {
  static char usage[] =
      "port-term-set-tof3 <port_str> <lane-str> <0->default, 1->AC coupled, "
      "2->DC "
      "coupled>";
  UCLI_COMMAND_INFO(
      uc,
      "port-term-set-tof3",
      3,
      "<port_str> <lane-str> <0->default, 1->AC coupled, 2->DC coupled>");

  bf_status_t sts;
  uint32_t term;
  char lane_str;
  int ln;
  bf_pm_port_term_mode_e term_mode;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  lane_str = uc->pargs->args[1][0];
  if (lane_str == '-') {
    ln = -1;
  } else {
    ln = atoi(uc->pargs->args[1]);
  }
  term = strtoul(uc->pargs->args[2], NULL, 10);
  term_mode = (term == 0) ? PM_TERM_MODE_DEFAULT
                          : (term == 1) ? PM_TERM_MODE_AC : PM_TERM_MODE_DC;

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return -1;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    bool is_port_internal = false;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;
    if (!bf_lld_dev_is_tof3(dev_id)) {
      aim_printf(&uc->pvs, "Command not supported on this chip\n");
      break;
    }

    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      bf_pm_port_term_mode_set(dev_id, port_hdl_ptr, ln, term_mode);
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_precoding_rx_clear__(
    ucli_context_t *uc) {
  static char usage[] = "pc-rx-clear <port_str>";
  UCLI_COMMAND_INFO(uc, "pc-rx-clear", 1, "<port_str>");
  bf_status_t sts;
  bf_dev_id_t dev_id;
  bf_dev_id_t dev_id_of_port;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr, *next_port_hdl_ptr;
  int db_size, i = 0, list_size;
  bf_pal_front_port_handle_t *port_hdl_list;
  bf_dev_port_t dev_port;

  port_hdl_ptr = &port_hdl;
  next_port_hdl_ptr = &next_port_hdl;

  dev_id = 0;
  if (!(bf_lld_dev_is_tof2(dev_id) || (bf_lld_dev_is_tof3(dev_id)))) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }

  sts = bf_pm_port_str_to_hdl_get(dev_id, uc->pargs->args[0], port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }

  if (port_hdl_ptr->conn_id == (uint32_t)-1) {
    db_size = pm_port_info_db_size_get(dev_id);
    list_size = db_size;
    port_hdl_list = (bf_pal_front_port_handle_t *)bf_sys_malloc(
        db_size * sizeof(bf_pal_front_port_handle_t));
    if (!port_hdl_list) return 0;

    // Get the first port in the system
    sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
    if (sts != BF_SUCCESS || !port_hdl_ptr) {
      aim_printf(&uc->pvs, "%s\n", usage);
      return 0;
    }
    i = 0;
    memcpy(&port_hdl_list[i], port_hdl_ptr, sizeof(port_hdl_list[i]));
    while (sts == BF_SUCCESS) {
      i++;
      // Get the next port in the system
      sts = bf_pm_port_front_panel_port_get_next(
          dev_id, port_hdl_ptr, next_port_hdl_ptr);
      if (sts == BF_OBJECT_NOT_FOUND) break;
      if (sts != BF_SUCCESS) {
        goto end;
      }

      memcpy(&port_hdl_list[i], next_port_hdl_ptr, sizeof(port_hdl_list[i]));
      // Make the curr port hdl equal to the next port hdl
      port_hdl_ptr = next_port_hdl_ptr;
    }
    bf_sys_assert(i == db_size);
  } else {
    if (port_hdl_ptr->chnl_id == (uint32_t)-1) {
      port_hdl_ptr->chnl_id = 0;
      if (BF_SUCCESS == bf_pm_port_front_panel_port_to_dev_port_get(
                            port_hdl_ptr, &dev_id_of_port, &dev_port)) {
        dev_id = dev_id_of_port;
        list_size = lld_get_chnls_dev_port(dev_id, dev_port);
        if (list_size == -1) {
          aim_printf(&uc->pvs, "Invalid Channel list.\n");
          return 0;
        }
      } else {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
      port_hdl_list = (bf_pal_front_port_handle_t *)bf_sys_malloc(
          list_size * sizeof(bf_pal_front_port_handle_t));
      for (i = 0; i < list_size; i++) {
        port_hdl_list[i].conn_id = port_hdl_ptr->conn_id;
        port_hdl_list[i].chnl_id = i;
      }
    } else {
      list_size = 1;
      port_hdl_list = (bf_pal_front_port_handle_t *)bf_sys_malloc(
          list_size * sizeof(bf_pal_front_port_handle_t));
      port_hdl_list[i].conn_id = port_hdl_ptr->conn_id;
      port_hdl_list[i].chnl_id = port_hdl_ptr->chnl_id;
    }
  }

  sts = bf_pm_port_precoding_rx_clear(dev_id, port_hdl_list, list_size);
  if (sts != BF_SUCCESS) {
    goto end;
  }

  bf_sys_free(port_hdl_list);
  return 0;
end:
  aim_printf(&uc->pvs, "Usage : %s\n", usage);
  bf_sys_free(port_hdl_list);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_precoding_tx_clear__(
    ucli_context_t *uc) {
  static char usage[] = "pc-tx-clear <port_str>";
  UCLI_COMMAND_INFO(uc, "pc-tx-clear", 1, "<port_str>");
  bf_status_t sts;
  bf_dev_id_t dev_id;
  bf_dev_id_t dev_id_of_port;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr, *next_port_hdl_ptr;
  int db_size, i = 0, list_size;
  bf_pal_front_port_handle_t *port_hdl_list;
  bf_dev_port_t dev_port;

  port_hdl_ptr = &port_hdl;
  next_port_hdl_ptr = &next_port_hdl;

  dev_id = 0;
  if (!(bf_lld_dev_is_tof2(dev_id) || (bf_lld_dev_is_tof3(dev_id)))) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }

  sts = bf_pm_port_str_to_hdl_get(dev_id, uc->pargs->args[0], port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }

  if (port_hdl_ptr->conn_id == (uint32_t)-1) {
    db_size = pm_port_info_db_size_get(dev_id);
    list_size = db_size;
    port_hdl_list = (bf_pal_front_port_handle_t *)bf_sys_malloc(
        db_size * sizeof(bf_pal_front_port_handle_t));
    if (!port_hdl_list) return 0;

    // Get the first port in the system
    sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
    if (sts != BF_SUCCESS || !port_hdl_ptr) {
      aim_printf(&uc->pvs, "%s\n", usage);
      return 0;
    }
    i = 0;
    memcpy(&port_hdl_list[i], port_hdl_ptr, sizeof(port_hdl_list[i]));
    while (sts == BF_SUCCESS) {
      i++;
      // Get the next port in the system
      sts = bf_pm_port_front_panel_port_get_next(
          dev_id, port_hdl_ptr, next_port_hdl_ptr);
      if (sts == BF_OBJECT_NOT_FOUND) break;
      if (sts != BF_SUCCESS) {
        goto end;
      }

      memcpy(&port_hdl_list[i], next_port_hdl_ptr, sizeof(port_hdl_list[i]));
      // Make the curr port hdl equal to the next port hdl
      port_hdl_ptr = next_port_hdl_ptr;
    }
    bf_sys_assert(i == db_size);
  } else {
    if (port_hdl_ptr->chnl_id == (uint32_t)-1) {
      port_hdl_ptr->chnl_id = 0;
      if (BF_SUCCESS == bf_pm_port_front_panel_port_to_dev_port_get(
                            port_hdl_ptr, &dev_id_of_port, &dev_port)) {
        dev_id = dev_id_of_port;
        list_size = lld_get_chnls_dev_port(dev_id, dev_port);
        if (list_size == -1) {
          aim_printf(&uc->pvs, "Invalid Channel size.\n");
          return 0;
        }
      } else {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
      port_hdl_list = (bf_pal_front_port_handle_t *)bf_sys_malloc(
          list_size * sizeof(bf_pal_front_port_handle_t));
      for (i = 0; i < list_size; i++) {
        port_hdl_list[i].conn_id = port_hdl_ptr->conn_id;
        port_hdl_list[i].chnl_id = i;
      }
    } else {
      list_size = 1;
      port_hdl_list = (bf_pal_front_port_handle_t *)bf_sys_malloc(
          list_size * sizeof(bf_pal_front_port_handle_t));
      port_hdl_list[i].conn_id = port_hdl_ptr->conn_id;
      port_hdl_list[i].chnl_id = port_hdl_ptr->chnl_id;
    }
  }

  sts = bf_pm_port_precoding_tx_clear(dev_id, port_hdl_list, list_size);
  if (sts != BF_SUCCESS) {
    goto end;
  }

  bf_sys_free(port_hdl_list);
  return 0;
end:
  aim_printf(&uc->pvs, "Usage : %s\n", usage);
  bf_sys_free(port_hdl_list);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_precoding_tx_set__(
    ucli_context_t *uc) {
  static char usage[] =
      "pc-tx-set <port_str> <lane | - > < e[nable] | d[isable] >";
  UCLI_COMMAND_INFO(
      uc, "pc-tx-set", 3, "<port_str> <lane | - > < e[nable] | d[isable] >");
  bf_status_t sts;
  bf_pm_port_precoding_policy_e pc_policy;
  bool all_lanes = false;
  int ln;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  if (uc->pargs->args[1][0] == '-') {
    ln = 0;
    all_lanes = true;
  } else {
    ln = strtoul(uc->pargs->args[1], NULL, 10);
    all_lanes = false;
  }

  if ((uc->pargs->args[2][0] == 'e') || (uc->pargs->args[2][0] == 'E')) {
    pc_policy = PM_PRECODING_FORCE_ENABLE;
  } else if ((uc->pargs->args[2][0] == 'd') || (uc->pargs->args[2][0] == 'D')) {
    pc_policy = PM_PRECODING_FORCE_DISABLE;
  } else {
    aim_printf(&uc->pvs, "Please specify enable or disable\n");
    return 0;
  }
  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return -1;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    bool is_port_internal = false;
    int first_ln, last_ln, num_lanes;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;
    if (bf_lld_dev_is_tof1(dev_id)) {
      aim_printf(&uc->pvs, "Command not supported on this chip\n");
      return 0;
    }

    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      bf_port_num_lanes_get(dev_id, dev_port, &num_lanes);

      if (all_lanes) {
        first_ln = 0;
        last_ln = num_lanes - 1;
      } else {
        first_ln = ln;
        last_ln = ln;
      }
      for (ln = first_ln; ln <= last_ln; ln++) {
        bf_pm_port_precoding_tx_set(dev_id, port_hdl_ptr, ln, pc_policy);
      }
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_precoding_rx_set__(
    ucli_context_t *uc) {
  static char usage[] =
      "pc-rx-set <port_str> <lane | - > < e[nable] | d[isable] >";
  UCLI_COMMAND_INFO(
      uc, "pc-rx-set", 3, "<port_str> <lane | - > < e[nable] | d[isable] >");
  bf_status_t sts;
  bf_pm_port_precoding_policy_e pc_policy;
  bool all_lanes = false;
  int ln;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  if (uc->pargs->args[1][0] == '-') {
    ln = 0;
    all_lanes = true;
  } else {
    ln = strtoul(uc->pargs->args[1], NULL, 10);
    all_lanes = false;
  }

  if ((uc->pargs->args[2][0] == 'e') || (uc->pargs->args[2][0] == 'E')) {
    pc_policy = PM_PRECODING_FORCE_ENABLE;
  } else if ((uc->pargs->args[2][0] == 'd') || (uc->pargs->args[2][0] == 'D')) {
    pc_policy = PM_PRECODING_FORCE_DISABLE;
  } else {
    aim_printf(&uc->pvs, "Please specify enable or disable\n");
    return 0;
  }
  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return -1;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    bool is_port_internal = false;
    int first_ln, last_ln, num_lanes;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;
    if (bf_lld_dev_is_tof1(dev_id)) {
      aim_printf(&uc->pvs, "Command not supported on this chip\n");
      return 0;
    }

    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      bf_port_num_lanes_get(dev_id, dev_port, &num_lanes);

      if (all_lanes) {
        first_ln = 0;
        last_ln = num_lanes - 1;
      } else {
        first_ln = ln;
        last_ln = ln;
      }
      for (ln = first_ln; ln <= last_ln; ln++) {
        bf_pm_port_precoding_rx_set(dev_id, port_hdl_ptr, ln, pc_policy);
      }
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_direction_set__(ucli_context_t *uc) {
  static char usage[] =
      "port-dir-set <port_str> <0->Default,1->TX Only,2->RX Only>";
  UCLI_COMMAND_INFO(uc,
                    "port-dir-set",
                    2,
                    "<port_str> <0->Default,1->TX Only,2->RX Only,"
                    "3->Decoupled>");
  bf_status_t sts;
  uint32_t pdir_in;
  bf_pm_port_dir_e port_dir;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  pdir_in = strtoul(uc->pargs->args[1], NULL, 10);

  switch (pdir_in) {
    case 0:
      port_dir = PM_PORT_DIR_DEFAULT;
      break;
    case 1:
      port_dir = PM_PORT_DIR_TX_ONLY;
      break;
    case 2:
      port_dir = PM_PORT_DIR_RX_ONLY;
      break;
    case 3:
      port_dir = PM_PORT_DIR_DECOUPLED;
      break;
    default:
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return -1;
  }

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return -1;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    bool is_port_internal = false;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;

    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      bf_pm_port_direction_set(dev_id, port_hdl_ptr, port_dir);
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

static bf_status_t bf_pm_ucli_port_loopback_mode_get(
    const char *str1, bf_loopback_mode_e *lb_mode) {
  int i = 0;
  char str[25];

  if ((!lb_mode) || (!str1)) return BF_INVALID_ARG;

  memset(str, '\0', sizeof(str));

  while ((str1[i]) && i < 25) {
    str[i] = tolower(str1[i]);
    i++;
  }

  if (strcmp(str, "clear") == 0) {
    *lb_mode = BF_LPBK_NONE;
  } else if (strcmp(str, "mac-near") == 0) {
    *lb_mode = BF_LPBK_MAC_NEAR;
  } else if (strcmp(str, "mac-far") == 0) {
    *lb_mode = BF_LPBK_MAC_FAR;
  } else if (strcmp(str, "pcs-near") == 0) {
    *lb_mode = BF_LPBK_PCS_NEAR;
  } else if (strcmp(str, "serdes-near") == 0) {
    *lb_mode = BF_LPBK_SERDES_NEAR;
  } else if (strcmp(str, "serdes-far") == 0) {
    *lb_mode = BF_LPBK_SERDES_FAR;
  } else if (strcmp(str, "pipe-loopback") == 0) {
    *lb_mode = BF_LPBK_PIPE;
  } else {
    return BF_INVALID_ARG;
  }

  return BF_SUCCESS;
}

static void bf_pm_ucli_ucli__port_loopback_set__usage(ucli_context_t *uc) {
  static char usage[] =
      "port-loopback "
      "<port_str> <clear, mac-near, pcs-near, serdes-near, "
      "pipe-loopback>";
  static char desc[] =
      "This command is intended to be used when the target port(s) are\n"
      "disbled. It modifies an internal configuration setting that will\n"
      "be applied the next time the port is enabled (via the port-enb "
      "command).\n"
      "\n"
      "Notes:\n"
      "serdes-near loopback is not supported on tofino2\n"
      "pipe-loopback is not supported on tofino1. Also, MAC stats will not\n"
      "              increment in this mode. Pipe statistics must be used.\n"
      ""
      "";
  aim_printf(&uc->pvs, "\nusage:\n%s\n", usage);
  aim_printf(&uc->pvs, "\ndescription:\n%s\n", desc);
}

static ucli_status_t bf_pm_ucli_ucli__port_loopback_set__(ucli_context_t *uc) {
  bf_status_t sts;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  bf_loopback_mode_e lb_mode;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  UCLI_COMMAND_INFO(uc,
                    "port-loopback",
                    2,
                    "<port_str> <clear, mac-near, pcs-near, serdes-near, "
                    "pipe-loopback, mac-far>");

  sts = bf_pm_ucli_port_loopback_mode_get(uc->pargs->args[1], &lb_mode);
  if (sts != 0) {
    bf_pm_ucli_ucli__port_loopback_set__usage(uc);
    return 0;
  };
  if (lb_mode == BF_LPBK_SERDES_FAR) {
    aim_printf(&uc->pvs, "Serdes Far loopback mode not supported\n");
    return 0;
  }

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs,
               "Usage : <port_str> <clear, mac-near, pcs-near, serdes-near, "
               "pipe-loopback, mac-far>");
    return 0;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    bool is_port_internal = false;
    bf_dev_family_t dev_family;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;

    dev_family = bf_dev_type_to_family(lld_sku_get_dev_type(dev_id));
    if (dev_family == BF_DEV_FAMILY_TOFINO2 ||
        dev_family == BF_DEV_FAMILY_TOFINO3) {
      if (lb_mode == BF_LPBK_MAC_FAR) {
        aim_printf(&uc->pvs,
                   "MAC Far loopback mode not supported on this device\n");
        bf_sys_free((void *)list);
        return 0;
      }
    }
    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      bf_pm_port_loopback_mode_set(dev_id, port_hdl_ptr, lb_mode);
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

static void pm_ucli_port_cut_through_set(bf_dev_id_t dev_id,
                                         bf_pal_front_port_handle_t *port_hdl,
                                         bool ct_enabled) {
  bf_dev_port_t dev_port;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) {
    return;
  }
  dev_id = dev_id_of_port;

  if (ct_enabled) {
    bf_pal_port_cut_through_enable(dev_id, dev_port);
  } else {
    bf_pal_port_cut_through_disable(dev_id, dev_port);
  }

  return;
}

static ucli_status_t bf_pm_ucli_ucli__port_ct_set__(ucli_context_t *uc) {
  static char usage[] = "port-ct-set <port_str> <0->disable,1->enable>";
  UCLI_COMMAND_INFO(uc, "port-ct-set", 2, "<port_str> <0->disable,1->enable>");
  bf_status_t sts;
  uint32_t chnl_begin, chnl_end;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr, *next_port_hdl_ptr;
  bf_dev_id_t dev_id;
  bf_dev_id_t dev_id_of_port = 0;
  uint32_t j, ct;
  bool ct_enabled;
  bf_dev_port_t dev_port;

  port_hdl_ptr = &port_hdl;
  next_port_hdl_ptr = &next_port_hdl;

  dev_id = 0;

  sts = bf_pm_port_str_to_hdl_get(dev_id, uc->pargs->args[0], port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    goto end;
  }

  ct = strtoul(uc->pargs->args[1], NULL, 10);
  if (ct != 0 && ct != 1) {
    goto end;
  }
  ct_enabled = ct == 1 ? true : false;

  if (port_hdl_ptr->conn_id == (uint32_t)-1) {
    // Get the first port in the system
    sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
    if (sts != BF_SUCCESS || !port_hdl_ptr) {
      goto end;
    }
    pm_ucli_port_cut_through_set(dev_id, port_hdl_ptr, ct_enabled);

    while (sts == BF_SUCCESS) {
      // Get the next port in the system
      sts = bf_pm_port_front_panel_port_get_next(
          dev_id, port_hdl_ptr, next_port_hdl_ptr);
      if (sts == BF_OBJECT_NOT_FOUND) break;
      if (sts != BF_SUCCESS) {
        goto end;
      }
      pm_ucli_port_cut_through_set(dev_id, next_port_hdl_ptr, ct_enabled);
      // Make the curr port hdl equal to the next port hdl
      port_hdl_ptr = next_port_hdl_ptr;
    }
  } else {
    if (port_hdl_ptr->chnl_id == (uint32_t)-1) {
      chnl_begin = 0;
      port_hdl_ptr->chnl_id = 0;
      if (BF_SUCCESS == bf_pm_port_front_panel_port_to_dev_port_get(
                            port_hdl_ptr, &dev_id_of_port, &dev_port)) {
        dev_id = dev_id_of_port;
        chnl_end = lld_get_chnls_dev_port(dev_id, dev_port) - 1;
      } else {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
    } else {
      chnl_begin = port_hdl_ptr->chnl_id;
      chnl_end = chnl_begin;
    }
    for (j = chnl_begin; j <= chnl_end; j++) {
      port_hdl_ptr->chnl_id = j;
      pm_ucli_port_cut_through_set(dev_id, port_hdl_ptr, ct_enabled);
    }
  }
  return 0;
end:
  aim_printf(&uc->pvs, "Usage %s\n", usage);
  return 0;
}

static void pm_ucli_port_fsm_stop(bf_dev_id_t dev_id,
                                  bf_pal_front_port_handle_t *port_hdl,
                                  bool stop) {
  bf_dev_port_t dev_port;
  bf_status_t sts;
  bf_dev_id_t dev_id_of_port = 0;

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) {
    return;
  }

  pm_port_fsm_stop_set(dev_id, dev_port, stop);
}

static ucli_status_t bf_pm_ucli_ucli__port_fsm__(ucli_context_t *uc) {
  static char usage[] = "port-fsm <port_str> <1->stop,0->go>";
  UCLI_COMMAND_INFO(uc, "port-fsm", 2, "<port_str> <1->stop,0->go>");
  bf_status_t sts;
  uint32_t chnl_begin, chnl_end;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr, *next_port_hdl_ptr;
  bf_dev_id_t dev_id;
  bf_dev_id_t dev_id_of_port;
  uint32_t j, ct;
  bool disabled;
  bf_dev_port_t dev_port;

  port_hdl_ptr = &port_hdl;
  next_port_hdl_ptr = &next_port_hdl;

  dev_id = 0;

  sts = bf_pm_port_str_to_hdl_get(dev_id, uc->pargs->args[0], port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    goto end;
  }
  ct = strtoul(uc->pargs->args[1], NULL, 10);
  if (ct != 0 && ct != 1) {
    goto end;
  }
  disabled = ct == 1 ? true : false;

  if (port_hdl_ptr->conn_id == (uint32_t)-1) {
    // Get the first port in the system
    sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
    if (sts != BF_SUCCESS || !port_hdl_ptr) {
      goto end;
    }
    pm_ucli_port_fsm_stop(dev_id, port_hdl_ptr, disabled);

    while (sts == BF_SUCCESS) {
      // Get the next port in the system
      sts = bf_pm_port_front_panel_port_get_next(
          dev_id, port_hdl_ptr, next_port_hdl_ptr);
      if (sts == BF_OBJECT_NOT_FOUND) break;
      if (sts != BF_SUCCESS) {
        goto end;
      }
      pm_ucli_port_fsm_stop(dev_id, next_port_hdl_ptr, disabled);
      // Make the curr port hdl equal to the next port hdl
      port_hdl_ptr = next_port_hdl_ptr;
    }
  } else {
    if (port_hdl_ptr->chnl_id == (uint32_t)-1) {
      chnl_begin = 0;
      port_hdl_ptr->chnl_id = 0;
      if (BF_SUCCESS == bf_pm_port_front_panel_port_to_dev_port_get(
                            port_hdl_ptr, &dev_id_of_port, &dev_port)) {
        dev_id = dev_id_of_port;
        chnl_end = lld_get_chnls_dev_port(dev_id, dev_port) - 1;
      } else {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
    } else {
      chnl_begin = port_hdl_ptr->chnl_id;
      chnl_end = chnl_begin;
    }
    for (j = chnl_begin; j <= chnl_end; j++) {
      port_hdl_ptr->chnl_id = j;
      pm_ucli_port_fsm_stop(dev_id, port_hdl_ptr, disabled);
    }
  }
  return 0;
end:
  aim_printf(&uc->pvs, "Usage %s\n", usage);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_stats_timer__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "port-stats-timer", 1, "<0->stop,1->start>");
  uint32_t timer_flag;
  bf_dev_id_t dev_id = 0;
  bool is_sw_model = true;
  bf_status_t sts = bf_drv_device_type_get(dev_id, &is_sw_model);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs,
               "Unable to get the device type for dev : %d : %s (%d)",
               dev_id,
               bf_err_str(sts),
               sts);
  }
  if (is_sw_model) {
    aim_printf(&uc->pvs,
               "MAC stats polling timer is disabled on model, dev %d",
               dev_id);
    return 0;
  }
  timer_flag = strtoul(uc->pargs->args[0], NULL, 10);
  if (timer_flag == 0) {
    // Stop port stats polling
    pm_port_stats_poll_stop(dev_id);
  } else {
    // Start port stats polling
    pm_port_stats_poll_start(dev_id);
  }
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__fsm_stop__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "fsm-stop", 0, "");

  pm_tasklet_free_run_set(false);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__fsm_go__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "fsm-go", 0, "");

  pm_tasklet_free_run_set(true);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__fsm_step__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "fsm-step", 0, "");

  pm_tasklet_single_step_set();
  return 0;
}

static void pm_ucli_update_stats_all_ports(bf_dev_id_t dev_id) {
  bf_status_t sts;
  bf_pal_front_port_handle_t iter_port_hdl, next_iter_port_hdl;
  bf_pal_front_port_handle_t *iter_port_hdl_ptr, *next_iter_port_hdl_ptr;

  iter_port_hdl_ptr = &iter_port_hdl;
  next_iter_port_hdl_ptr = &next_iter_port_hdl;

  // Update the stats for the valid added ports in the system
  sts = bf_pm_port_front_panel_port_get_first(dev_id, iter_port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    return;
  }
  bf_pm_port_all_stats_update_sync(dev_id, iter_port_hdl_ptr);
  while (sts == BF_SUCCESS) {
    // Get the next port in the system
    sts = bf_pm_port_front_panel_port_get_next(
        dev_id, iter_port_hdl_ptr, next_iter_port_hdl_ptr);
    if (sts == BF_OBJECT_NOT_FOUND) break;
    if (sts != BF_SUCCESS) {
      return;
    }
    // Update the stats for this port
    bf_pm_port_all_stats_update_sync(dev_id, next_iter_port_hdl_ptr);
    // Make the curr port hdl equal to the next port hdl
    iter_port_hdl_ptr = next_iter_port_hdl_ptr;
  }
}

static void display_pm_show_banner(ucli_context_t *uc, int tflag) {
  char *port_d[] = {"PORT ", "-----"};
  char *speed_d[] = {"SPEED  ", "-------"};
  char *fec_d[] = {"FEC ", "----"};
  char *an[] = {"AN", "--"};
  char *kr[] = {"KR", "--"};
  char *REDY_d[] = {"RDY", "---"};
  char *admin_d[] = {"ADM", "---"};
  char *oper_d[] = {"OPR", "---"};
  char *lb_d[] = {"LPBK    ", "--------"};
  char *frames_rx_d[] = {"FRAMES RX       ", "----------------"};
  char *frames_tx_d[] = {"FRAMES TX       ", "----------------"};
  char *mac_d[] = {"MAC ", "----"};
  char *dev_port_d[] = {"D_P", "---"};
  char *pipe_port_d[] = {"P/PT", "----"};
  char *error_d[] = {"E", "-"};
  char *lastup[] = {"Last UP Time               ",
                    "---------------------------"};
  char *lastdn[] = {"Last DWN Time               ",
                    "----------------------------"};
  char *uptime[] = {"Link uptime  ", "-------------"};

  if (!uc) return;

  aim_printf(&uc->pvs,
             "%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s",
             port_d[1],
             mac_d[1],
             dev_port_d[1],
             pipe_port_d[1],
             speed_d[1],
             fec_d[1],
             an[1],
             kr[1],
             REDY_d[1],
             admin_d[1],
             oper_d[1],
             lb_d[1],
             frames_rx_d[1],
             frames_tx_d[1],
             error_d[1]);
  if (tflag) {
    aim_printf(&uc->pvs, "+%s+%s+%s", lastup[1], lastdn[1], uptime[1]);
  }
  aim_printf(&uc->pvs,
             "\n%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s",
             port_d[0],
             mac_d[0],
             dev_port_d[0],
             pipe_port_d[0],
             speed_d[0],
             fec_d[0],
             an[0],
             kr[0],
             REDY_d[0],
             admin_d[0],
             oper_d[0],
             lb_d[0],
             frames_rx_d[0],
             frames_tx_d[0],
             error_d[0]);
  if (tflag) {
    aim_printf(&uc->pvs, "|%s|%s|%s", lastup[0], lastdn[0], uptime[0]);
  }
  aim_printf(&uc->pvs,
             "\n%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s",
             port_d[1],
             mac_d[1],
             dev_port_d[1],
             pipe_port_d[1],
             speed_d[1],
             fec_d[1],
             an[1],
             kr[1],
             REDY_d[1],
             admin_d[1],
             oper_d[1],
             lb_d[1],
             frames_rx_d[1],
             frames_tx_d[1],
             error_d[1]);
  if (tflag) {
    aim_printf(&uc->pvs, "+%s+%s+%s", lastup[1], lastdn[1], uptime[1]);
  }
  aim_printf(&uc->pvs, "\n");
}

static void pm_ucli_display_info_summ(ucli_context_t *uc,
                                      bf_dev_id_t dev_id,
                                      int aflag,
                                      int tflag,
                                      bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  struct ctr_ids *stats_ptr, *old_stats_ptr;
  uint32_t print_count = 0;
  uint64_t rx_tmp, tx_tmp;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  // used by aflag
  char *speed_d[] = {"SPEED  ", "-------"};
  char *fec_d[] = {"FEC ", "----"};
  char *an[] = {"AN", "--"};
  char *kr[] = {"KR", "--"};
  char *admin_d[] = {"ADM", "---"};
  char *oper_d[] = {"OPR", "---"};
  char *lb_d[] = {"LPBK    ", "--------"};
  char *frames_rx_d[] = {"FRAMES RX       ", "----------------"};
  char *frames_tx_d[] = {"FRAMES TX       ", "----------------"};
  char *error_d[] = {"E", "-"};
  char *lastup[] = {"Last UP Time               ",
                    "---------------------------"};
  char *lastdn[] = {"Last DWN Time               ",
                    "----------------------------"};
  char *uptime[] = {"Link uptime  ", "-------------"};

  char display_speed[9];
  char display_fec[5];
  char *display_an;
  char *display_kr;
  char display_rdy[4];

  display_pm_show_banner(uc, tflag);

  for (ret = pm_port_info_get_first_copy(
           dev_id, next_port_info_ptr, &dev_id_of_port);
       ret == 0;
       port_info_ptr = next_port_info_ptr,
      ret = pm_port_info_get_next_copy(
          dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
    pm_ucli_rdy_to_display_get(
        next_port_info_ptr, dev_id, display_rdy, sizeof(display_rdy));
    if (!next_port_info_ptr->is_added) {
      if (aflag) {
        // Indicates that this port has not been added yet
        aim_printf(
            &uc->pvs,
            "%-5s|%2" PRIu64 "/%1" PRIu64 "|%3" PRIu64 "|%1" PRIu64 "/%2" PRIu64
            "|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s",
            next_port_info_ptr->pltfm_port_info.port_str,
            (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_id,
            (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_lane,
            (uint64_t)next_port_info_ptr->dev_port,
            (uint64_t)phy_pipe,
            (uint64_t)DEV_PORT_TO_LOCAL_PORT(next_port_info_ptr->dev_port),
            speed_d[1],
            fec_d[1],
            an[1],
            kr[1],
            display_rdy,
            admin_d[1],
            oper_d[1],
            lb_d[1],
            frames_rx_d[1],
            frames_tx_d[1],
            error_d[1]);
        if (tflag) {
          aim_printf(&uc->pvs, "|%s|%s|%s", lastup[1], lastdn[1], uptime[1]);
        }
        aim_printf(&uc->pvs, "\n");
        print_count++;
      }
    } else {
      print_count++;

      /* Update stats on the port before displaying it.  Grab a fresh copy of
       * the port_info after syncing stats since the port_info used as the loop
       * iterator is also a copy and will not be updated by the stats sync. */
      bf_pm_port_all_stats_update_sync(
          dev_id, &next_port_info_ptr->pltfm_port_info.port_hdl);
      bf_pm_port_info_t port_info = {0};
      if (pm_port_info_get_copy(
              dev_id, next_port_info_ptr->dev_port, &port_info))
        continue;
      stats_ptr = (struct ctr_ids *)&port_info.curr_stats;
      old_stats_ptr = (struct ctr_ids *)&port_info.old_stats;

      pm_ucli_speed_to_display_get(next_port_info_ptr->speed,
                                   display_speed,
                                   sizeof(display_speed),
                                   next_port_info_ptr->n_lanes);
      pm_ucli_fec_to_display_get(
          next_port_info_ptr->fec_type, display_fec, sizeof(display_fec));
      bf_pm_port_autoneg_policy_e an_policy;
      bf_pm_port_kr_mode_policy_e kr_policy;
      bf_pm_port_autoneg_get(
          dev_id, &next_port_info_ptr->pltfm_port_info.port_hdl, &an_policy);
      bf_pm_port_kr_mode_get(
          dev_id, &next_port_info_ptr->pltfm_port_info.port_hdl, &kr_policy);
      display_an = an_policy == PM_AN_DEFAULT
                       ? "Au"
                       : an_policy == PM_AN_FORCE_ENABLE
                             ? "En"
                             : an_policy == PM_AN_FORCE_DISABLE ? "Ds" : "??";
      display_kr = kr_policy == PM_KR_DEFAULT
                       ? "Au"
                       : kr_policy == PM_KR_FORCE_ENABLE
                             ? "En"
                             : kr_policy == PM_KR_FORCE_DISABLE ? "Ds" : "??";
      rx_tmp =
          ((stats_ptr->FramesReceivedAll >= old_stats_ptr->FramesReceivedAll)
               ? (stats_ptr->FramesReceivedAll -
                  old_stats_ptr->FramesReceivedAll)
               : ((uint64_t)(-1) - old_stats_ptr->FramesReceivedAll +
                  stats_ptr->FramesReceivedAll));
      tx_tmp = ((stats_ptr->FramesTransmittedAll >=
                 old_stats_ptr->FramesTransmittedAll)
                    ? (stats_ptr->FramesTransmittedAll -
                       old_stats_ptr->FramesTransmittedAll)
                    : ((uint64_t)(-1) - old_stats_ptr->FramesTransmittedAll +
                       stats_ptr->FramesTransmittedAll));
      // This port has been added. Hence display the info
      aim_printf(&uc->pvs,
                 "%-5s|%2" PRIu64 "/%1" PRIu64 "|%3" PRIu64 "|%1" PRIu64
                 "/%2" PRIu64 "|%s|%s|%s|%s|%s|%s|%s|%-8s|%16" PRIu64
                 "|%16" PRIu64 "|%s",
                 next_port_info_ptr->pltfm_port_info.port_str,
                 (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_id,
                 (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_lane,
                 (uint64_t)next_port_info_ptr->dev_port,
                 (uint64_t)phy_pipe,
                 (uint64_t)DEV_PORT_TO_LOCAL_PORT(next_port_info_ptr->dev_port),
                 display_speed,
                 display_fec,
                 display_an,
                 display_kr,
                 display_rdy,
                 next_port_info_ptr->admin_state ? "ENB" : "DIS",
                 next_port_info_ptr->oper_status ? "UP " : "DWN",
                 get_loopback_mode_str(next_port_info_ptr->lpbk_mode),
                 rx_tmp,
                 tx_tmp,
                 ((stats_ptr->FrameswithanyError !=
                   old_stats_ptr->FrameswithanyError) ||
                  (stats_ptr->FramesTransmittedwithError !=
                   old_stats_ptr->FramesTransmittedwithError))
                     ? "*"
                     : " ");
      if (tflag) {
        aim_printf(&uc->pvs, "|");
        port_mgr_dump_uptime(uc, dev_id, next_port_info_ptr->dev_port);
      }
      aim_printf(&uc->pvs, "\n");
    }
    // print the banner
    if (print_count >= 40) {
      display_pm_show_banner(uc, tflag);
      print_count = 0;
    }
  }
}

static void pm_ucli_display_all_info(ucli_context_t *uc,
                                     bf_dev_id_t dev_id,
                                     bf_pm_port_info_t *port_info) {
  int stat_id;
  char *stat_name;
  uint64_t *stats_ptr, *old_stats_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  char display_speed[9];
  char display_fec[5];
  bool is_port_internal;
  uint64_t stats_tmp;
  char display_rdy[4];

  // Update the stats for this port
  bf_pm_is_port_internal(
      dev_id, &port_info->pltfm_port_info.port_hdl, &is_port_internal);
  bf_pm_port_all_stats_update_sync(dev_id,
                                   &port_info->pltfm_port_info.port_hdl);
  stats_ptr = (uint64_t *)&port_info->curr_stats;
  old_stats_ptr = (uint64_t *)&port_info->old_stats;
  log_pipe = DEV_PORT_TO_PIPE(port_info->dev_port);
  if (lld_sku_map_pipe_id_to_phy_pipe_id(dev_id, log_pipe, &phy_pipe) != 0) {
    PM_ERROR(
        "Unable to get phy pipe id from log pipe id for dev %d : log pipe id : "
        "%d",
        dev_id,
        log_pipe);
    return;
  }
  pm_ucli_speed_to_display_get(port_info->speed,
                               display_speed,
                               sizeof(display_speed),
                               port_info->n_lanes);
  pm_ucli_fec_to_display_get(
      port_info->fec_type, display_fec, sizeof(display_fec));
  pm_ucli_rdy_to_display_get(
      port_info, dev_id, display_rdy, sizeof(display_rdy));

  aim_printf(&uc->pvs,
             "%16s : Port Identifier\n",
             port_info->pltfm_port_info.port_str);
  aim_printf(
      &uc->pvs, "%16s : is port internal\n", is_port_internal ? "YES" : "NO"),
      aim_printf(&uc->pvs,
                 "%14" PRIu64 "/%1" PRIu64 " : MAC\n",
                 (uint64_t)port_info->pltfm_port_info.log_mac_id,
                 (uint64_t)port_info->pltfm_port_info.log_mac_lane);
  aim_printf(
      &uc->pvs, "%16" PRIu64 " : Dev Port\n", (uint64_t)port_info->dev_port);
  aim_printf(&uc->pvs,
             "%13" PRIu64 "/%2" PRIu64 " : Pipe/Port \n",
             (uint64_t)phy_pipe,
             (uint64_t)DEV_PORT_TO_LOCAL_PORT(port_info->dev_port));
  aim_printf(&uc->pvs, "%16s : Speed\n", display_speed);
  aim_printf(&uc->pvs, "%16s : FEC\n", display_fec);
  aim_printf(&uc->pvs, "%16s : Ready for Bring Up\n", display_rdy);
  aim_printf(&uc->pvs,
             "%16s : Autoneg eligibility\n",
             port_info->is_an_eligible ? "YES" : "NO");
  aim_printf(&uc->pvs,
             "%16s : AN policy set\n",
             port_info->an_policy == PM_AN_DEFAULT
                 ? "DEFAULT"
                 : port_info->an_policy == PM_AN_FORCE_ENABLE
                       ? "FORCE_ENABLE"
                       : "FORCE_DISABLE");
  aim_printf(
      &uc->pvs, "%16s : Admin State\n", port_info->admin_state ? "ENB" : "DIS");
  aim_printf(&uc->pvs,
             "%16s : Operational Status\n",
             port_info->oper_status ? "UP" : "DOWN");

  for (stat_id = 0; stat_id < BF_NUM_RMON_COUNTERS; stat_id++) {
    if (bf_port_rmon_counter_to_str(stat_id, &stat_name) != 0) {
      return;
    }
    stats_tmp =
        ((stats_ptr[stat_id] >= old_stats_ptr[stat_id])
             ? (stats_ptr[stat_id] - old_stats_ptr[stat_id])
             : ((uint64_t)(-1) - old_stats_ptr[stat_id] + stats_ptr[stat_id]));
    aim_printf(&uc->pvs, "%16" PRIu64 " : %s\n", stats_tmp, stat_name);
  }
  aim_printf(
      &uc->pvs,
      "=================================================================="
      "==============\n");
}

static void pm_ucli_display_info_detailed(
    ucli_context_t *uc,
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t port_info, next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  uint32_t chnl_begin, chnl_end, j;
  int ret;
  bf_status_t sts;
  bf_dev_port_t dev_port;
  bf_dev_id_t dev_id_of_port = 0;

  port_info_ptr = &port_info;
  next_port_info_ptr = &next_port_info;

  aim_printf(
      &uc->pvs,
      "========================================================================"
      "========\n");

  pm_ucli_update_stats_all_ports(dev_id);

  if (port_hdl->conn_id == (uint32_t)-1) {
    ret = pm_port_info_get_first_copy(dev_id, port_info_ptr, &dev_id_of_port);
    dev_id = dev_id_of_port;

    for (; ret == 0;
         ret = pm_port_info_get_next_copy(
             dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port),
         port_info_ptr = next_port_info_ptr) {
      dev_id = dev_id_of_port;

      // This port has been added. Hence display the info
      pm_ucli_display_all_info(uc, dev_id, port_info_ptr);
    }
  } else {
    if (port_hdl->chnl_id == (uint32_t)-1) {
      chnl_begin = 0;
      port_hdl->chnl_id = 0;
      if (BF_SUCCESS == bf_pm_port_front_panel_port_to_dev_port_get(
                            port_hdl, &dev_id_of_port, &dev_port)) {
        dev_id = dev_id_of_port;
        chnl_end = lld_get_chnls_dev_port(dev_id, dev_port) - 1;
      } else {
        return;
      }
    } else {
      chnl_begin = port_hdl->chnl_id;
      chnl_end = chnl_begin;
    }
    for (j = chnl_begin; j <= chnl_end; j++) {
      port_hdl->chnl_id = j;
      // Get the dev port for the port hdl
      sts = bf_pm_port_front_panel_port_to_dev_port_get(
          port_hdl, &dev_id_of_port, &dev_port);
      if (sts != BF_SUCCESS) {
        return;
      }
      dev_id = dev_id_of_port;

      // Get the pm port data structure from the dev port
      ret = pm_port_info_get_copy(dev_id, dev_port, port_info_ptr);
      if (ret != 0) {
        return;
      }
      // Display the info of the port
      pm_ucli_display_all_info(uc, dev_id, port_info_ptr);
    }
  }
}

static void pm_ucli_dump_info(ucli_context_t *uc,
                              bf_dev_id_t dev_id,
                              bf_pal_front_port_handle_t *port_hdl,
                              int dflag,
                              int aflag,
                              int tflag) {
  if (dflag == 1) {
    // Print in detail
    pm_ucli_display_info_detailed(uc, dev_id, port_hdl);
  } else {
    // Print summary
    pm_ucli_display_info_summ(uc, dev_id, aflag, tflag, port_hdl);
  }
}

static ucli_status_t bf_pm_ucli_ucli__port_show__(ucli_context_t *uc) {
  static char usage[] = "show -a -p <conn_id/chnl> [-d] [-t]";
  UCLI_COMMAND_INFO(uc, "show", -1, "-a -p <port_str> [-d] [-t]");

  extern char *optarg;
  extern int optind;
  int pflag, aflag, dflag, tflag;
  int argc;
  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  bf_status_t sts;

  pflag = aflag = dflag = tflag = 0;
  optind = 0;  // reset optind value
  argc = (uc->pargs->count + 1);
  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  if ((port_hdl.conn_id = -1) && (port_hdl.chnl_id = -1)) {
    // check for specific port argument
    int a;
    bf_pal_front_port_handle_t new_port_hdl;

    for (a = 0; a <= argc; a++) {
      if (argv[a] == NULL) break;
      if (strncmp(argv[a], "pm", 2) == 0) continue;
      if (strncmp(argv[a], "show", 4) == 0) continue;
      if (strncmp(argv[a], "-a", 2) == 0) {
        aflag = true;
        continue;
      }
      if (strncmp(argv[a], "-d", 2) == 0) {
        dflag = true;
        continue;
      }
      if (strncmp(argv[a], "-t", 2) == 0) {
        tflag = true;
        continue;
      }
      if (strncmp(argv[a], "-p", 2) == 0) {
        a++;
        // see if its a port name
        sts = bf_pm_port_str_to_hdl_get(dev_id, argv[a], &new_port_hdl);
        if (sts != BF_SUCCESS) {
          aim_printf(&uc->pvs, "Usage : %s\n", usage);
          return 0;
        }
        port_hdl.conn_id = new_port_hdl.conn_id;
        port_hdl.chnl_id = new_port_hdl.chnl_id;
        continue;
      }
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
  }
  // conn==-1 and chnl==-1 indicates that scan through all the ports but only
  // print the ports that have been added.
  //-a option indicates that the scan and print all the ports (whether added or
  // not)

  pm_ucli_dump_info(uc, dev_id, &port_hdl, dflag, aflag, tflag);
  (void)pflag;
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_rate_set__(ucli_context_t *uc) {
  static char usage[] =
      "rate-period <duration> Start a periodic timer with an interval of "
      "\"duration\" seconds. Each time the timer fires, the average rate over "
      "the last \"duration\" seconds is computed and can be displayed with the "
      "rate-show command. Use 0, the default, to disable the timer.";
  UCLI_COMMAND_INFO(uc,
                    "rate-period",
                    1,
                    "<duration> Start a periodic timer with an interval of "
                    "\"duration\" seconds. Each time the timer fires, the "
                    "average rate over the last \"duration\" seconds is "
                    "computed and can be displayed with the rate-show command. "
                    "Use 0, the default, to disable the timer.");
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr, *next_port_hdl_ptr;
  bf_dev_id_t dev_id;
  bf_dev_id_t dev_id_of_port = 0;
  bf_dev_port_t dev_port;

  uint32_t period_msecs;
  long period_sec_tmp;

  port_hdl_ptr = &port_hdl;
  next_port_hdl_ptr = &next_port_hdl;

  dev_id = 0;
  if (uc->pargs->count == 1) {
    period_sec_tmp = strtoul(uc->pargs->args[0], NULL, 10);  // seconds
    if ((period_sec_tmp < 0) || (period_sec_tmp > 4294967)) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      aim_printf(&uc->pvs, "        Please input valid time period.\n");
      return 0;
    }
    period_msecs = (uint32_t)period_sec_tmp * 1000;  // msec
  } else {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }
  sts = bf_pm_rate_timer_check_del(dev_id);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Reset rate timer fails.\n");
    return 0;
  }
  if (period_msecs == 0) {
    // finished
    aim_printf(&uc->pvs, "Deleted the last setting successfully.\n");
    return 0;
  }

  // set timer
  sts = bf_pm_rate_timer_creat(dev_id, period_msecs);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Creat timer fails.\n");
    return 0;
  }
  // record current counter of each enable port
  // Get the first port in the system
  sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
  if (sts != BF_SUCCESS || !port_hdl_ptr) {
    aim_printf(&uc->pvs, "No port has been added.\n");
    return 0;
  }
  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl_ptr, &dev_id_of_port, &dev_port);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }
  dev_id = dev_id_of_port;

  // update counter cache, time stamp, init rate
  sts = bf_pm_init_rate(dev_id, dev_port);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Unable to update stats\n");
    return 0;
  }
  while (sts == BF_SUCCESS) {
    // Get the next port in the system
    sts = bf_pm_port_front_panel_port_get_next(
        dev_id, port_hdl_ptr, next_port_hdl_ptr);
    if (sts == BF_OBJECT_NOT_FOUND) break;
    if (sts != BF_SUCCESS) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        next_port_hdl_ptr, &dev_id_of_port, &dev_port);
    if (sts != BF_SUCCESS) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
    dev_id = dev_id_of_port;

    sts = bf_pm_init_rate(dev_id, dev_port);
    if (sts != BF_SUCCESS) {
      aim_printf(&uc->pvs, "Unable to update stats\n");
      return 0;
    }
    // Make the curr port hdl equal to the next port hdl
    port_hdl_ptr = next_port_hdl_ptr;
  }

  // start timer
  sts = bf_pm_rate_timer_start(dev_id);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "start timer fails.\n");
    return 0;
  }
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_rate_show__(ucli_context_t *uc) {
  // only read from port_info->rx_rate tx_rate
  UCLI_COMMAND_INFO(uc,
                    "rate-show",
                    0,
                    "Print rate, valid after setting rate-period with non-zero "
                    "\"duration\" seconds");
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_id_t dev_id = 0;
  bf_dev_id_t dev_id_of_port = 0;
  long rx_rate, tx_rate;
  uint32_t rx_pps, tx_pps;
  uint32_t rx_ppl_rate[BF_PIPE_COUNT] = {0, 0, 0, 0, 0, 0, 0, 0};
  uint32_t tx_ppl_rate[BF_PIPE_COUNT] = {0, 0, 0, 0, 0, 0, 0, 0};
  uint32_t rx_ppl_pps[BF_PIPE_COUNT] = {0, 0, 0, 0, 0, 0, 0, 0};
  uint32_t tx_ppl_pps[BF_PIPE_COUNT] = {0, 0, 0, 0, 0, 0, 0, 0};
  uint32_t port_rate;
  bf_dev_pipe_t log_pipe = 0;
  bf_dev_pipe_t phy_pipe = 0;
  int ret;
  uint32_t print_count = 0;
  char *port_d[] = {"PORT ", "-----"};
  char *mac_d[] = {"MAC ", "----"};
  char *dev_port_d[] = {"D_P", "---"};
  char *speed_d[] = {"SPEED  ", "-------"};
  char *REDY_d[] = {"RDY", "---"};
  char *frames_rx_d[] = {"RX Mpps", "-------"};
  char *frames_tx_d[] = {"TX Mpps", "-------"};
  char *rate_rx_d[] = {"RX Mbps  ", "---------"};
  char *rate_tx_d[] = {"TX Mbps  ", "---------"};
  char *percent_rx_d[] = {"RX %", "----"};
  char *percent_tx_d[] = {"TX %", "----"};
  char *pipe_port_d[] = {"P/PT", "----"};

  char display_speed[9];

  aim_printf(&uc->pvs,
             "%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s\n",
             port_d[1],
             mac_d[1],
             dev_port_d[1],
             pipe_port_d[1],
             speed_d[1],
             REDY_d[1],
             frames_rx_d[1],
             frames_tx_d[1],
             rate_rx_d[1],
             rate_tx_d[1],
             percent_rx_d[1],
             percent_tx_d[1]);
  aim_printf(&uc->pvs,
             "%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s\n",
             port_d[0],
             mac_d[0],
             dev_port_d[0],
             pipe_port_d[0],
             speed_d[0],
             REDY_d[0],
             frames_rx_d[0],
             frames_tx_d[0],
             rate_rx_d[0],
             rate_tx_d[0],
             percent_rx_d[0],
             percent_tx_d[0]);
  aim_printf(&uc->pvs,
             "%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s\n",
             port_d[1],
             mac_d[1],
             dev_port_d[1],
             pipe_port_d[1],
             speed_d[1],
             REDY_d[1],
             frames_rx_d[1],
             frames_tx_d[1],
             rate_rx_d[1],
             rate_tx_d[1],
             percent_rx_d[1],
             percent_tx_d[1]);

  next_port_info_ptr = &next_port_info;

  // Get the first port in the system
  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

    log_pipe = DEV_PORT_TO_PIPE(next_port_info_ptr->dev_port);
    if ((lld_sku_map_pipe_id_to_phy_pipe_id(dev_id, log_pipe, &phy_pipe) !=
         0) ||
        (phy_pipe >= BF_PIPE_COUNT)) {
      aim_printf(&uc->pvs, "Unable to get pipe id");
      return 0;
    }
    if (next_port_info_ptr->is_added) {
      // Indicates that this port has not been added yet
      rx_rate = next_port_info_ptr->rx_rate;
      tx_rate = next_port_info_ptr->tx_rate;
      rx_pps = next_port_info_ptr->rx_pps;
      tx_pps = next_port_info_ptr->tx_pps;
      pm_ucli_speed_to_display_get(next_port_info_ptr->speed,
                                   display_speed,
                                   sizeof(display_speed),
                                   next_port_info_ptr->n_lanes);
      port_rate = pm_util_get_port_rate(next_port_info_ptr->speed);
      if (port_rate == 0) {
        aim_printf(&uc->pvs, "Unable to get port rate");
        return 0;
      }
      aim_printf(
          &uc->pvs,
          "%-5s|%2" PRIu64 "/%1" PRIu64 "|%3" PRIu64 "|%1" PRIu64 "/%2" PRIu64
          "|%s|%s| %3u.%02u| %3u.%02u"
          "|%6" PRIu64 ".%02" PRIu64 "|%6" PRIu64 ".%02" PRIu64 "|%3" PRIu64
          "%%|%3" PRIu64 "%%\n",
          next_port_info_ptr->pltfm_port_info.port_str,
          (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_id,
          (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_lane,
          (uint64_t)next_port_info_ptr->dev_port,
          (uint64_t)phy_pipe,
          (uint64_t)DEV_PORT_TO_LOCAL_PORT(next_port_info_ptr->dev_port),
          display_speed,
          next_port_info_ptr->oper_status ? "UP " : "DWN",
          (uint32_t)(rx_pps / 1000000),
          (uint32_t)((rx_pps - (uint32_t)(rx_pps / 1000000) * 1000000) / 10000),
          (uint32_t)(tx_pps / 1000000),
          (uint32_t)((tx_pps - (uint32_t)(tx_pps / 1000000) * 1000000) / 10000),
          (uint64_t)(rx_rate / 1000000),
          (uint64_t)((rx_rate - (uint64_t)(rx_rate / 1000000) * 1000000) /
                     10000),
          (uint64_t)(tx_rate / 1000000),
          (uint64_t)((tx_rate - (uint64_t)(tx_rate / 1000000) * 1000000) /
                     10000),
          (uint64_t)(rx_rate * 100 / port_rate / 1000000000),
          (uint64_t)(tx_rate * 100 / port_rate / 1000000000));
      print_count++;
      // accumulate pipeline rate
      rx_ppl_rate[phy_pipe] += (rx_rate / 10000);  // 0.01Mbps
      tx_ppl_rate[phy_pipe] += (tx_rate / 10000);
      rx_ppl_pps[phy_pipe] += (rx_pps / 10000);  // 0.01Mpps
      tx_ppl_pps[phy_pipe] += (tx_pps / 10000);
    }

    if (print_count >= 40) {
      aim_printf(&uc->pvs,
                 "%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s\n",
                 port_d[1],
                 mac_d[1],
                 dev_port_d[1],
                 pipe_port_d[1],
                 speed_d[1],
                 REDY_d[1],
                 frames_rx_d[1],
                 frames_tx_d[1],
                 rate_rx_d[1],
                 rate_tx_d[1],
                 percent_rx_d[1],
                 percent_tx_d[1]);
      aim_printf(&uc->pvs,
                 "%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s\n",
                 port_d[0],
                 mac_d[0],
                 dev_port_d[0],
                 pipe_port_d[0],
                 speed_d[0],
                 REDY_d[0],
                 frames_rx_d[0],
                 frames_tx_d[0],
                 rate_rx_d[0],
                 rate_tx_d[0],
                 percent_rx_d[0],
                 percent_tx_d[0]);
      aim_printf(&uc->pvs,
                 "%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s\n",
                 port_d[1],
                 mac_d[1],
                 dev_port_d[1],
                 pipe_port_d[1],
                 speed_d[1],
                 REDY_d[1],
                 frames_rx_d[1],
                 frames_tx_d[1],
                 rate_rx_d[1],
                 rate_tx_d[1],
                 percent_rx_d[1],
                 percent_tx_d[1]);
      print_count = 0;
    }
  }
  aim_printf(&uc->pvs,
             "%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s+%s\n",
             port_d[1],
             mac_d[1],
             dev_port_d[1],
             pipe_port_d[1],
             speed_d[1],
             REDY_d[1],
             frames_rx_d[1],
             frames_tx_d[1],
             rate_rx_d[1],
             rate_tx_d[1],
             percent_rx_d[1],
             percent_tx_d[1]);
  for (ret = 0; ret < BF_PIPE_COUNT; ret++) {
    if ((rx_ppl_rate[ret] == 0) && (tx_ppl_rate[ret] == 0)) continue;
    aim_printf(
        &uc->pvs,
        "Pipe%1u| RX RATE %4u.%02uMpps %7uMbps| TX RATE %4u.%02uMpps %7uMbps\n",
        ret,
        (uint32_t)(rx_ppl_pps[ret] / 100),
        (uint32_t)(rx_ppl_pps[ret] - 100 * ((uint32_t)(rx_ppl_pps[ret] / 100))),
        (uint32_t)(rx_ppl_rate[ret] / 100),
        (uint32_t)(tx_ppl_pps[ret] / 100),
        (uint32_t)(tx_ppl_pps[ret] - 100 * ((uint32_t)(tx_ppl_pps[ret] / 100))),
        (uint32_t)(tx_ppl_rate[ret] / 100));
  }
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_stats_period_set__(
    ucli_context_t *uc) {
  static char usage[] =
      "port-stats-period-set <period in millisec> "
      "min:500";
  UCLI_COMMAND_INFO(
      uc, "port-stats-period-set", 1, " <period in millisec> min:500");

  uint32_t period_msecs;

  if (uc->pargs->count == 1) {
    period_msecs = strtol(uc->pargs->args[0], NULL, 10);
    if (period_msecs < PORT_STATS_POLL_MIN_TMR_PERIOD_MS) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      aim_printf(&uc->pvs, "        Please input valid period.\n");
      return 0;
    }
  } else {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }

  bf_pm_port_stats_poll_period_update(0, period_msecs);

  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_stats_persistent__(
    ucli_context_t *uc) {
  static char usage[] =
      "port-stats-persistent <dev_id> [0->disable, 1->enable]";
  UCLI_COMMAND_INFO(
      uc, "port-stats-persistent", -1, "<dev_id> [0->disable, 1->enable]");

  bool enable = false;
  bf_dev_id_t dev_id;
  bf_status_t sts;
  uint32_t value;

  dev_id = 0;
  if (uc->pargs->count >= 1) {
    dev_id = strtoul(uc->pargs->args[0], NULL, 10);
  } else {
    goto end;
  }

  if (uc->pargs->count == 2) {
    value = strtoul(uc->pargs->args[1], NULL, 10);
    if (value > 1) goto end;
    enable = value ? true : false;
    sts = bf_pm_port_stats_persistent_set(dev_id, enable);
    if (sts != BF_SUCCESS) {
      aim_printf(&uc->pvs,
                 "Error setting port stats persistent control flag.\n");
    }
    return 0;
  } else if (uc->pargs->count == 1) {
    sts = bf_pm_port_stats_persistent_get(dev_id, &enable);
    if (sts != BF_SUCCESS) {
      aim_printf(&uc->pvs, "Error getting port stats persistent flag.\n");
    } else {
      aim_printf(&uc->pvs,
                 "Persistent port stats : %s\n",
                 enable ? "enabled" : "disabled");
    }
    return 0;
  }

end:
  aim_printf(&uc->pvs, "Usage : %s\n", usage);
  return 0;
}

// tof2======================================================================
ucli_context_t *serdes_uc;

char *pm_ber_graph(float ber);

#include <port_mgr/bf_tof2_serdes_if.h>

int glbl_csv_sample = 0;

/*****************************************************************************
 */
void tof2_serdes_info(uint32_t fp,
                      uint32_t chnl,
                      uint32_t dev_port,
                      uint32_t integration_ms) {
  uint32_t tile, grp;
  uint32_t fw_len;
  uint32_t fw_hash_code;
  uint32_t fw_crc;
  uint32_t running_hash_code;
  uint32_t running_crc;
  uint32_t running_ver;
  bf_status_t rc;
  char *fw_grp_0_7;
  char *fw_grp_8;
  uint32_t phys_tx_ln[8], phys_rx_ln[8];
  uint32_t dev_id = 0;
  uint32_t mac, ch;
  char *mode_str[] = {"NONE", "NRZ ", "PAM4"};
  int num_lanes;
  uint64_t chip_id;
  uint64_t val, flipped = 0ull;
  uint64_t fab, lot, lotnum0, lotnum1, lotnum2, lotnum3, wafer, x, y;
  int i;
  char wafer_pos[32];
  float temperature_tile[4];
  int temp_tries = 3;

  i = 0;
  do {
    rc = bf_tof2_serdes_fw_temperature_get(
        dev_id, dev_port, &temperature_tile[i]);
  } while ((temp_tries-- > 0) && (temperature_tile[i] == 0));

  ucli_context_t *uc = serdes_uc;

  rc = bf_port_fw_get(dev_id, &fw_grp_0_7, &fw_grp_8);
  if (rc != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Warning: Cant identify FW file paths");
  }
  lld_sku_get_chip_id(dev_id, &chip_id);

  // first, flip the 64 bits end-to-end
  for (i = 0; i < 64; i++) {
    flipped |= ((chip_id >> i) & 1) << (64 - i - 1);
  }
  // then decode
  val = flipped;
  fab = val >> 57ull;
  lot = (val >> 50ull) & 0x7full;
  lotnum0 = (val >> 43ull) & 0x7full;
  lotnum1 = (val >> 36ull) & 0x7full;
  lotnum2 = (val >> 29ull) & 0x7full;
  lotnum3 = (val >> 22ull) & 0x7full;
  wafer = (val >> 17ull) & 0x1full;
  x = (val >> 9ull) & 0xffull;
  y = (val >> 1ull) & 0xffull;
  // make sure fields are printable
  if (!isprint(fab)) fab = '?';
  if (!isprint(lot)) lot = '?';
  if (!isprint(lotnum0)) lotnum0 = '?';
  if (!isprint(lotnum1)) lotnum1 = '?';
  if (!isprint(lotnum2)) lotnum2 = '?';
  if (!isprint(lotnum3)) lotnum3 = '?';
  snprintf(wafer_pos,
           sizeof(wafer_pos) - 1,
           "Wafer-id: %c%c%c%c%c%c-W%" PRIu64 "-X%" PRIu64 "-Y%" PRIu64 "",
           (char)fab,
           (char)lot,
           (char)lotnum0,
           (char)lotnum1,
           (char)lotnum2,
           (char)lotnum3,
           wafer,
           x,
           y);

  lld_sku_map_dev_port_id_to_mac_ch(dev_id, dev_port, &mac, &ch);
  lld_sku_map_mac_stn_id_to_tile_and_group(dev_id, mac, &tile, &grp);

  bf_tof2_serdes_fw_ver_get(dev_id,
                            dev_port,
                            fw_grp_0_7,
                            &fw_len,
                            &fw_hash_code,
                            &fw_crc,
                            &running_hash_code,
                            &running_crc,
                            &running_ver);
  aim_printf(&uc->pvs,
             "Sample,Time,Wafer,Temperature,FP,"
             "devid,D_P,Mac,Ch,PipeId,PortId,Tile,Grp,log_ln,phy_tx,phy_rx,");
  aim_printf(&uc->pvs,
             "file_len,file hash,file crc,running_"
             "hash,running_crc,running_ver,");
  aim_printf(&uc->pvs,
             "enc,speed"
             ",pre2,hw_pre2,pre1,hw_pre1,main,hw_main,post1,hw_post1,"
             "post2,hw_post2,tx_pol,hw_tx_pol,rx"
             "pol,hw_rx_pol,tx_bg,hw_tx_bg,rx_bg,hw_rx_bg,");

  aim_printf(&uc->pvs,
             "Mode,Speed,Adp,"
             "ReAdp,LLos,SigDet,PhyRdy,AdpDone,PPM,Ch_Est,OF,HF,"
             "Ctle_OW_val,Ctle_1,Ctle_2,Ctle_G1,Ctle_G2,SK,DAC,"
             "eye1,eye2,eye3,Dfe_F0,Dfe_F1,Dfe_F1_F0,Dfe_F13,"
             "Time_Del,Time_Edge,Ffe_K1_val,Ffe_K2_val,Ffe_K3_val,Ffe_K4_val,"
             "Ffe_S1_val,Ffe_S2_val,Ffe_K1,Ffe_K2,Ffe_K3,"
             "Ffe_K4,isi_pre2,isi_pre1,isi_post1,f3,f4,f5,f6,f7,f8,"
             "f9,f10,f11,f12,f13,f14,f15,"
             "IntegrationMs,PRBSErrs,BER,BER_graph,FEC_ln_N_SER,"
             "FEC_ln_N_SymErrs,FEC_ln_Np1_SER,FEC_ln_Np1_SymErrs,"
             "Serdes_ln_SER,Serdes_ln_SymErrs,Bring_up_time\n");

  bf_tof2_serdes_lane_map_get(dev_id, dev_port, phys_tx_ln, phys_rx_ln);
  bf_port_num_lanes_get(dev_id, dev_port, &num_lanes);

  for (int ln = 0; ln < num_lanes; ln++) {
    bf_serdes_encoding_mode_t enc_mode;
    int32_t pre2, pre1, _main, post1, post2;
    int32_t hw_pre2, hw_pre1, hw_main, hw_post1, hw_post2;
    bool tx_inv, rx_inv, hw_rx_inv, hw_tx_inv;
    uint32_t tx_bg, hw_tx_bg, rx_bg, hw_rx_bg;
    uint32_t G, adapt, readapt, link_lost, of, hf;
    uint32_t skef_val, dac_val;
    uint32_t ctle_over_val, ctle_map_0, ctle_map_1, ctle_1, ctle_2, dc_gain_1,
        dc_gain_2;
    uint32_t delta, edge1, edge2, edge3, edge4;
    uint32_t tap1, tap2, tap3, f13_val;
    int signed_ppm, signed7_delta;
    bool sig_detect, phy_ready, adapt_done;
    int32_t ppm;
    float chan_est, eye_1, eye_2, eye_3;
    float f0, f1, ratio;
    struct timeval tv;
    struct tm *loctime;
    char ubuf[132], tbuf[132];

    aim_printf(&uc->pvs, "%d,", glbl_csv_sample);
    glbl_csv_sample++;

    gettimeofday(&tv, NULL);
    loctime = localtime(&tv.tv_sec);
    if (loctime == NULL) {
      aim_printf(&uc->pvs, "localtime returned null \n");
      return;
    }

    strftime(tbuf, sizeof(tbuf), "%a %b %d", loctime);
    aim_printf(&uc->pvs, "%s ", tbuf);
    strftime(ubuf, sizeof(ubuf), "%T\n", loctime);
    ubuf[strlen(ubuf) - 1] = 0;  // remove CR
    aim_printf(&uc->pvs, "%s.%06d,", ubuf, (int)tv.tv_usec);

    aim_printf(&uc->pvs, "%s, ", wafer_pos);
    aim_printf(&uc->pvs, "%2.1f,", temperature_tile[0]);

    aim_printf(&uc->pvs,
               "%d/%d,%3d,%3d,%2d,%d,%d,%2d,%2d,%d,%d,",
               fp,
               chnl,
               dev_id,
               dev_port,
               mac,
               ch,
               DEV_PORT_TO_PIPE(dev_port),
               DEV_PORT_TO_LOCAL_PORT(dev_port),
               tile,
               grp,
               ch + ln);
    aim_printf(&uc->pvs, "%d,%d,", phys_tx_ln[ch + ln], phys_rx_ln[ch + ln]);

    aim_printf(&uc->pvs,
               "%7d,%06x,%04x,%06x,%04x,%d.%d.%d,",
               fw_len,
               fw_hash_code,
               fw_crc,
               running_hash_code,
               running_crc,
               (running_ver >> 16) & 0xff,
               (running_ver >> 8) & 0xff,
               (running_ver >> 0) & 0xff);

    bf_tof2_serdes_fw_lane_speed_get(dev_id, dev_port, ln, &G, &enc_mode);
    aim_printf(&uc->pvs, "%5s,%3dG ,", mode_str[enc_mode], G);

    bf_tof2_serdes_tx_taps_get(
        dev_id, dev_port, ln, &pre2, &pre1, &_main, &post1, &post2);
    bf_tof2_serdes_tx_taps_hw_get(dev_id,
                                  dev_port,
                                  ln,
                                  &hw_pre2,
                                  &hw_pre1,
                                  &hw_main,
                                  &hw_post1,
                                  &hw_post2);
    aim_printf(&uc->pvs,
               "%3d,%3d,%3d,%3d,%3d,%3d,%3d,%3d,%3d,%3d,",
               pre2,
               hw_pre2,
               pre1,
               hw_pre1,
               _main,
               hw_main,
               post1,
               hw_post1,
               post2,
               hw_post2);
    bf_tof2_serdes_tx_polarity_get(dev_id, dev_port, ln, &tx_inv);
    bf_tof2_serdes_rx_polarity_get(dev_id, dev_port, ln, &rx_inv);
    bf_tof2_serdes_tx_polarity_hw_get(dev_id, dev_port, ln, &hw_tx_inv);
    bf_tof2_serdes_rx_polarity_hw_get(dev_id, dev_port, ln, &hw_rx_inv);
    aim_printf(&uc->pvs, "%d,%d,%d,%d,", tx_inv, hw_tx_inv, rx_inv, hw_rx_inv);
    bf_tof2_serdes_tx_bandgap_get(dev_id, dev_port, ln, &tx_bg);
    bf_tof2_serdes_rx_bandgap_get(dev_id, dev_port, ln, &rx_bg);
    bf_tof2_serdes_tx_bandgap_hw_get(dev_id, dev_port, ln, &hw_tx_bg);
    bf_tof2_serdes_rx_bandgap_hw_get(dev_id, dev_port, ln, &hw_rx_bg);
    aim_printf(&uc->pvs, "%d,%d,%d,%d,", tx_bg, hw_tx_bg, rx_bg, hw_rx_bg);

    rc = bf_tof2_serdes_fw_lane_speed_get(dev_id, dev_port, ln, &G, &enc_mode);
    if (rc) {
      G = 0;
    }
    if (G == 0) {  // OFF
      aim_printf(&uc->pvs, "OFF\n");
      continue;
    }
    rc = bf_tof2_serdes_rx_sig_info_get(
        dev_id, dev_port, ln, &sig_detect, &phy_ready, &ppm);
    if (rc != BF_SUCCESS) continue;

    rc = bf_tof2_serdes_adapt_counts_get(
        dev_id, dev_port, ln, &adapt_done, &adapt, &readapt, &link_lost);
    signed_ppm = ppm;
    if (rc != BF_SUCCESS) continue;
    aim_printf(&uc->pvs,
               "%5s,%3dG,%5d,%5d,%4d, "
               "%d,%d,%d,%4d,",
               mode_str[enc_mode],
               G,
               adapt,
               readapt,
               link_lost,
               sig_detect,
               phy_ready,
               adapt_done,
               signed_ppm);

    rc = bf_tof2_serdes_of_get(dev_id, dev_port, ln, &of, &hf);
    if (rc != BF_SUCCESS) continue;
    chan_est = (hf ? (float)of / (float)hf : NAN);
    aim_printf(&uc->pvs, "%4.2f,%2d,%2d,", chan_est, of, hf);

    bf_tof2_serdes_ctle_over_val_get(dev_id, dev_port, ln, &ctle_over_val);
    bf_tof2_serdes_ctle_val_get(
        dev_id, dev_port, ln, ctle_over_val, &ctle_map_0, &ctle_map_1);
    ctle_1 = ctle_map_0;
    ctle_2 = ctle_map_1;

    if (enc_mode == 2) {  // PAM4
      uint32_t ctle_map_7_0, ctle_map_7_1;
      // if (ctle_1_bit4 == 1 or ctle_2_bit4 == 1 or
      // chip.PAM50[ln].ctle_map_pam4(7)[0] == 7): ctle_val += 8
      bf_tof2_serdes_ctle_val_get(
          dev_id, dev_port, ln, 7, &ctle_map_7_0, &ctle_map_7_1);
      if ((ctle_1 & 0x8) || (ctle_2 & 0x8) || (ctle_map_7_0 == 7)) {
        ctle_over_val += 8;
      }
    }

    bf_tof2_serdes_ctle_gain_get(dev_id, dev_port, ln, &dc_gain_1, &dc_gain_2);
    aim_printf(&uc->pvs,
               "%2d,%d,%d,%3d,%2d,",
               ctle_over_val,
               ctle_1,
               ctle_2,
               dc_gain_1,
               dc_gain_2);

    rc = bf_tof2_serdes_skef_val_get(dev_id, dev_port, ln, &skef_val);
    if (rc != BF_SUCCESS) continue;
    rc = bf_tof2_serdes_dac_val_get(dev_id, dev_port, ln, &dac_val);
    if (rc != BF_SUCCESS) continue;

    aim_printf(&uc->pvs, "%d,%2d,", skef_val, dac_val);

    rc = bf_tof2_serdes_eye_get(dev_id, dev_port, ln, &eye_1, &eye_2, &eye_3);
    if (enc_mode == 1) {  // NRZ
      aim_printf(&uc->pvs, "%4.0f,0,0,", eye_1);
    } else {
      aim_printf(&uc->pvs, "%4.0f,%3.0f,%3.0f,", eye_1, eye_2, eye_3);
    }
    bf_tof2_serdes_delta_get(dev_id, dev_port, ln, &delta);
    if (delta & (1 << 6)) {
      signed7_delta = 0 - (128 - delta);
    } else {
      signed7_delta = delta;
    }
    bf_tof2_serdes_edge_get(
        dev_id, dev_port, ln, &edge1, &edge2, &edge3, &edge4);
    if (enc_mode == 1) {  // NRZ
      bf_tof2_serdes_dfe_nrz_get(dev_id, dev_port, ln, &tap1, &tap2, &tap3);
      aim_printf(&uc->pvs, "%4d,%4d,%4d,", tap1, tap2, tap3);
    } else {
      bf_tof2_serdes_dfe_pam4_get(dev_id, dev_port, ln, &f0, &f1, &ratio);
      bf_tof2_serdes_f13_val_pam4_get(dev_id, dev_port, ln, &f13_val);
      aim_printf(&uc->pvs, "%4.2f,%4.2f,%5.2f,%2d,", f0, f1, ratio, f13_val);
    }
    aim_printf(
        &uc->pvs, "%3d,%X%X%X%X,", signed7_delta, edge1, edge2, edge3, edge4);

    if (enc_mode == 2) {  // PAM4
      int32_t k1, k2, k3, k4, s1, s2;
      bf_tof2_serdes_ffe_taps_pam4_get(
          dev_id, dev_port, ln, &k1, &k2, &k3, &k4, &s1, &s2);
      aim_printf(
          &uc->pvs, "%4d,%4d,%4d,%4d,%02X,%02X,", k1, k2, k3, k4, s1, s2);
      aim_printf(&uc->pvs,
                 "%4.2f,%6.3f,%6.4f,%6.4f,",
                 (float)k1 * 0.01,
                 (float)k2 * 0.008 * ((float)s1 / 4),
                 (float)k3 * 0.0067 * ((float)s2 / 4) * ((float)s1 / 4),
                 (float)k4 * 0.0053 * ((float)s2 / 4) * ((float)s1 / 4));
    }

    uint32_t isi_vals[16];

    bf_tof2_serdes_isi_get(dev_id, dev_port, ln, isi_vals);
    for (i = 0; i < 16; i++) {
      aim_printf(&uc->pvs, "%4d,", isi_vals[i]);
    }
    uint32_t err_cnt, err_sum = 0;
    bf_port_prbs_mode_t prbs_mode;

    bf_port_prbs_mode_get(dev_id, dev_port, &prbs_mode);
    if (prbs_mode == BF_PORT_PRBS_MODE_NONE) {
      aim_printf(&uc->pvs, "0,0,");
    } else {
      if (ln >= num_lanes) {
        aim_printf(&uc->pvs, "0,");
      } else {
        if (integration_ms != 0) {
          bf_tof2_serdes_prbs_rst_set(dev_id, dev_port, ln);
        }
        bf_sys_usleep(integration_ms * 1000);

        bf_tof2_serdes_rx_prbs_err_get(0, dev_port, ln, &err_cnt);
        aim_printf(&uc->pvs, "%5d,%10d,", integration_ms, err_cnt);
        err_sum += err_cnt;

        if (integration_ms == 0) {
          bf_tof2_serdes_prbs_rst_set(dev_id, dev_port, ln);
        }
      }
    }
    float ber;
    if (prbs_mode == BF_PORT_PRBS_MODE_NONE) {
      uint64_t bps;
      bf_port_speed_t speed;

      bf_port_speed_get(dev_id, dev_port, &speed);

      /* get ports aggregate bit-rate */
      bps = pm_util_aggregate_bit_rate_get(speed, num_lanes);

      /* get per-lane bit-rate */
      bps = bps / num_lanes;
      bps = (bps * integration_ms / 1000);
      ber = (float)((float)err_sum / (float)bps);
      aim_printf(&uc->pvs, "%8.1e,%s,", ber, pm_ber_graph(ber));

      bf_fec_type_t fec;
      umac4_rs_fec_ln_ctr_t fec_ln_ctrs;
      uint32_t umac;
      uint32_t ch_ln;
      bool is_cpu_port;
      uint32_t ctr, n_ctrs, first_ctr;
      uint64_t s_errs;
      uint64_t *ctrs = (uint64_t *)&fec_ln_ctrs;

      bf_port_fec_get(dev_id, dev_port, &fec);
      bf_port_speed_get(dev_id, dev_port, &speed);
      bf_port_num_lanes_get(0, dev_port, &num_lanes);
      bf_serdes_encoding_mode_get(speed, num_lanes, &enc_mode);

      rc = port_mgr_tof2_map_dev_port_to_all(
          dev_id, dev_port, NULL, NULL, &umac, &ch_ln, &is_cpu_port);
      if (rc != BF_SUCCESS) {
        aim_printf(&uc->pvs, "unable to map dev_port (%d) to umac\n", dev_port);
        return;
      }
      if (fec != BF_FEC_TYP_RS) {
        aim_printf(&uc->pvs, "\n");
        continue;
      }
      /* get ports aggregate bit-rate */
      bps = pm_util_aggregate_bit_rate_get(speed, num_lanes);

      if (speed == BF_SPEED_400G) {
        n_ctrs = 16;
        first_ctr = 0;
      } else if (speed == BF_SPEED_200G) {
        n_ctrs = 8;
        first_ctr = (dev_port & 0x7) * 2;
      } else if (speed == BF_SPEED_100G) {
        n_ctrs = 4;
        first_ctr = (dev_port & 0x7) * 2;
      } else if (speed == BF_SPEED_50G) {
        n_ctrs = 2;
        first_ctr = (dev_port & 0x7) * 2;
      } else {
        aim_printf(&uc->pvs, "Not supported for this speed (yet)\n");
        return;
      }
      n_ctrs = 2;  // 2 FEC counters, one for each PCS lane (=2 per serdes lane)
      first_ctr += (ln * 2);

      bps = (bps * integration_ms) / 1000;

      // timed collections of counters
      umac4_ctrs_rs_fec_ln_get(
          dev_id, umac, &fec_ln_ctrs);  // read first to clear
      bf_sys_usleep(integration_ms * 1000);
      umac4_ctrs_rs_fec_ln_get(dev_id, umac, &fec_ln_ctrs);  // now for real

      uint32_t serdes_ln_ctrs = 0;
      for (ctr = first_ctr; ctr < (first_ctr + n_ctrs); ctr++) {
        s_errs = ctrs[ctr];
        serdes_ln_ctrs += s_errs;
        ber = (float)(s_errs * 1) /
              ((float)(bps / ((uint64_t)n_ctrs)));  // 10b symbols, ea lane
                                                    // carries 1/n_ctrs of the
                                                    // bits
        aim_printf(&uc->pvs, "%8.1e,%-9d,", ber, (uint32_t)ctrs[ctr]);
      }
      ber = ((float)(serdes_ln_ctrs * 1) / (float)(bps / ((uint64_t)n_ctrs))) /
            2.0;  // 2 ctrs per serdes lane
      aim_printf(&uc->pvs, "%8.1e,%-9d", ber, (uint32_t)serdes_ln_ctrs);
    } else {
      ber = 0.0;
      aim_printf(&uc->pvs, "%8.1e,%s,", ber, pm_ber_graph(ber));
      for (i = 0; i < 2; i++) {
        aim_printf(&uc->pvs, "%8.1e,%-9d,", ber, 0);
      }
      aim_printf(&uc->pvs, "%8.1e,%-9d", ber, 0);
    }
    uint64_t t_us;
    bf_port_bring_up_time_get(dev_id, dev_port, &t_us);
    aim_printf(&uc->pvs, "%u", (uint32_t)t_us);
    aim_printf(&uc->pvs, "\n");
  }
}

int bf_tof3_serdes_read_status2(uint32_t dev_id,
                                uint32_t dev_port,
                                uint32_t ln,
                                uint32_t br);

static bf_status_t bf_pm_ucli_serdes_csv_dump(
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl,
    uint32_t integration_ms) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      if (bf_lld_dev_is_tof2(dev_id)) {
        tof2_serdes_info(next_port_info_ptr->pltfm_port_info.port_hdl.conn_id,
                         next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id,
                         next_port_info_ptr->dev_port,
                         integration_ms);
      } else if (bf_lld_dev_is_tof3(dev_id)) {
        int num_lanes, ln;
        bf_port_num_lanes_get(dev_id, next_port_info_ptr->dev_port, &num_lanes);
        for (ln = 0; ln < num_lanes; ln++) {
          bf_tof3_serdes_read_status2(
              dev_id, next_port_info_ptr->dev_port, ln, 0 /*branch*/);
        }
      }
    } else {
      continue;
    }
  }
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_csv_dump__(ucli_context_t *uc) {
  static char usage[] = "port-csv <port_str>";
  UCLI_COMMAND_INFO(uc, "port-csv", -1, "<port_str>");
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr, *next_port_hdl_ptr;
  bf_dev_id_t dev_id;
  uint32_t integration_ms = 1000;

  port_hdl_ptr = &port_hdl;
  next_port_hdl_ptr = &next_port_hdl;

  serdes_uc = uc;
  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }

  if (uc->pargs->count == 0) {  // all ports
    port_hdl_ptr->conn_id = (uint32_t)-1;
    port_hdl_ptr->chnl_id = (uint32_t)-1;
  }
  if (uc->pargs->count >= 1) {
    sts = bf_pm_port_str_to_hdl_get(dev_id, uc->pargs->args[0], port_hdl_ptr);
    if (sts != BF_SUCCESS) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
  }
  if (uc->pargs->count == 2) {
    integration_ms = atoi(uc->pargs->args[1]);
    if (integration_ms > 100000) goto csv_arg_error;
  }
  if (uc->pargs->count > 2) {
    aim_printf(&uc->pvs,
               "usage: port-csv [[port-str] [integration_time_ms]]\n");
    return 0;
  }

  if (port_hdl_ptr->conn_id == (uint32_t)-1) {
    // Get the first port in the system
    sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
    if (sts != BF_SUCCESS || !port_hdl_ptr) {
      goto csv_arg_error;
    }
    sts = bf_pm_ucli_serdes_csv_dump(dev_id, port_hdl_ptr, integration_ms);
    if (sts != BF_SUCCESS) {
      goto csv_arg_error;
    }
    while (sts == BF_SUCCESS) {
      // Get the next port in the system
      sts = bf_pm_port_front_panel_port_get_next(
          dev_id, port_hdl_ptr, next_port_hdl_ptr);
      if (sts == BF_OBJECT_NOT_FOUND) break;
      if (sts != BF_SUCCESS) {
        goto csv_arg_error;
      }
      sts =
          bf_pm_ucli_serdes_csv_dump(dev_id, next_port_hdl_ptr, integration_ms);
      if (sts != BF_SUCCESS) {
        goto csv_arg_error;
      }
      // Make the curr port hdl equal to the next port hdl
      port_hdl_ptr = next_port_hdl_ptr;
    }
  } else {
    sts = bf_pm_ucli_serdes_csv_dump(dev_id, port_hdl_ptr, integration_ms);
    if (sts != BF_SUCCESS) {
      goto csv_arg_error;
    }
  }
  return 0;

csv_arg_error:
  aim_printf(&uc->pvs, "Usage : %s\n", usage);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_csv_dump_multiple__(
    ucli_context_t *uc) {
  static char usage[] = "port-csv-m <port_str> <N>";
  UCLI_COMMAND_INFO(uc, "port-csv-m", 2, "<port_str> <N>");
  bf_status_t sts;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr, *next_port_hdl_ptr;
  bf_dev_id_t dev_id;
  int N;
  uint32_t integration_ms = 1000;

  port_hdl_ptr = &port_hdl;
  next_port_hdl_ptr = &next_port_hdl;

  serdes_uc = uc;
  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }

  sts = bf_pm_port_str_to_hdl_get(dev_id, uc->pargs->args[0], port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }
  N = atoi(uc->pargs->args[1]);

  for (int i = 0; i < N; i++) {
    if (port_hdl_ptr->conn_id == (uint32_t)-1) {
      // Get the first port in the system
      sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
      if (sts != BF_SUCCESS || !port_hdl_ptr) {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
      if (sts != BF_SUCCESS) {
        return sts;
      }
      sts = bf_pm_ucli_serdes_csv_dump(dev_id, port_hdl_ptr, integration_ms);
      if (sts != BF_SUCCESS) {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
      while (sts == BF_SUCCESS) {
        // Get the next port in the system
        sts = bf_pm_port_front_panel_port_get_next(
            dev_id, port_hdl_ptr, next_port_hdl_ptr);
        if (sts == BF_OBJECT_NOT_FOUND) break;
        if (sts != BF_SUCCESS) {
          aim_printf(&uc->pvs, "Usage : %s\n", usage);
          return 0;
        }
        sts = bf_pm_ucli_serdes_csv_dump(
            dev_id, next_port_hdl_ptr, integration_ms);
        if (sts != BF_SUCCESS) {
          aim_printf(&uc->pvs, "Usage : %s\n", usage);
          return 0;
        }
        // Make the curr port hdl equal to the next port hdl
        port_hdl_ptr = next_port_hdl_ptr;
      }
    } else {
      sts = bf_pm_ucli_serdes_csv_dump(dev_id, port_hdl_ptr, integration_ms);
      if (sts != BF_SUCCESS) {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
    }
  }
  return 0;
}

void tof2_serdes_fw_info(uint32_t fp, uint32_t dev_port) {
  uint32_t dev_id = 0;
  uint32_t tile, grp;
  uint32_t fw_len;
  uint32_t fw_hash_code;
  uint32_t fw_crc;
  uint32_t running_hash_code;
  uint32_t running_crc;
  uint32_t running_ver;
  uint32_t mac, ch;
  ucli_context_t *uc = serdes_uc;
  bf_status_t rc;
  char *fw_grp_0_7;
  char *fw_grp_8;

  rc = bf_port_fw_get(dev_id, &fw_grp_0_7, &fw_grp_8);
  if (rc != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Warning: Cant identify FW file paths");
  }
  aim_printf(&uc->pvs,
             "---+-----+--------+----+---+--------+---------+--------+---------"
             "---+-----------+-----------+\n");
  aim_printf(&uc->pvs,
             "FP |devid|dev port|MAC|Tile|Grp|file len|file hash|file "
             "crc|running hash|running crc|running ver|\n");
  aim_printf(&uc->pvs,
             "---+-----+--------+----+---+--------+---------+--------+---------"
             "---+-----------+-----------+\n");

  lld_sku_map_dev_port_id_to_mac_ch(dev_id, dev_port, &mac, &ch);
  lld_sku_map_mac_stn_id_to_tile_and_group(dev_id, mac, &tile, &grp);

  bf_tof2_serdes_fw_ver_get(dev_id,
                            dev_port,
                            fw_grp_0_7,
                            &fw_len,
                            &fw_hash_code,
                            &fw_crc,
                            &running_hash_code,
                            &running_crc,
                            &running_ver);
  aim_printf(&uc->pvs,
             "%2d | %3d |   %3d  | %2d| %2d | %d |",
             fp,
             dev_id,
             dev_port,
             mac,
             tile,
             grp);
  aim_printf(&uc->pvs,
             "%7d | %06x  |  %04x  |   %06x   |   %04x    |  %d.%d.%d   |\n",
             fw_len,
             fw_hash_code,
             fw_crc,
             running_hash_code,
             running_crc,
             (running_ver >> 16) & 0xff,
             (running_ver >> 8) & 0xff,
             (running_ver >> 0) & 0xff);
  aim_printf(&uc->pvs, "\n");
}

void tof2_serdes_config(uint32_t fp, uint32_t dev_port) {
  uint32_t tile, grp;
  uint32_t phys_tx_ln[8], phys_rx_ln[8];
  uint32_t dev_id = 0;
  uint32_t mac, ch;
  char *mode_str[] = {"NONE", "NRZ ", "PAM4"};
  int num_lanes;
  ucli_context_t *uc = serdes_uc;

  aim_printf(&uc->pvs,
             "---+-----+--------+----+---+------+------+------+-----+-------+--"
             "-------+---------+---------+---------+---------+-------+-------+"
             "-------+-------+-------+-----+-----+-----+-----+-----+-----+\n");
  aim_printf(
      &uc->pvs,
      "FP |devid|dev port|Tile|Grp|log ln|phy tx|phy rx| enc | speed "
      "|pre2 (hw)|pre1 (hw)|main (hw)|post1(hw)|post2(hw)| tx pol| rx "
      "pol| tx bg | rx bg |Precode|ph tx|ph rx|an tx|an rx|hw tx|hw rx\n");
  aim_printf(&uc->pvs,
             "---+-----+--------+----+---+------+------+------+-----+-------+--"
             "-------+---------+---------+---------+---------+-------+-------+"
             "-------+-------+-------+-----+-----+-----+-----+-----+-----+\n");

  lld_sku_map_dev_port_id_to_mac_ch(dev_id, dev_port, &mac, &ch);
  lld_sku_map_mac_stn_id_to_tile_and_group(dev_id, mac, &tile, &grp);
  bf_tof2_serdes_lane_map_get(dev_id, dev_port, phys_tx_ln, phys_rx_ln);

  bf_port_num_lanes_get(dev_id, dev_port, &num_lanes);

  for (int ln = 0; ln < num_lanes; ln++) {
    bf_serdes_encoding_mode_t enc_mode;
    uint32_t G;
    int32_t pre2, pre1, _main, post1, post2;
    int32_t hw_pre2, hw_pre1, hw_main, hw_post1, hw_post2;
    bool tx_inv, rx_inv, hw_rx_inv, hw_tx_inv;
    uint32_t tx_bg, hw_tx_bg, rx_bg, hw_rx_bg;

    aim_printf(&uc->pvs,
               "%2d | %3d |   %3d  | %2d | %d |   %d  |",
               fp,
               dev_id,
               dev_port,
               tile,
               grp,
               ch + ln);
    aim_printf(
        &uc->pvs, "   %d  |   %d  |", phys_tx_ln[ch + ln], phys_rx_ln[ch + ln]);

    bf_tof2_serdes_fw_lane_speed_get(dev_id, dev_port, ln, &G, &enc_mode);
    aim_printf(&uc->pvs, "%5s|  %3dG |", mode_str[enc_mode], G);

    bf_tof2_serdes_tx_taps_get(
        dev_id, dev_port, ln, &pre2, &pre1, &_main, &post1, &post2);
    bf_tof2_serdes_tx_taps_hw_get(dev_id,
                                  dev_port,
                                  ln,
                                  &hw_pre2,
                                  &hw_pre1,
                                  &hw_main,
                                  &hw_post1,
                                  &hw_post2);
    aim_printf(&uc->pvs,
               "%3d (%3d)|%3d (%3d)|%3d (%3d)|%3d (%3d)|%3d (%3d)|",
               pre2,
               hw_pre2,
               pre1,
               hw_pre1,
               _main,
               hw_main,
               post1,
               hw_post1,
               post2,
               hw_post2);
    bf_tof2_serdes_tx_polarity_get(dev_id, dev_port, ln, &tx_inv);
    bf_tof2_serdes_rx_polarity_get(dev_id, dev_port, ln, &rx_inv);
    bf_tof2_serdes_tx_polarity_hw_get(dev_id, dev_port, ln, &hw_tx_inv);
    bf_tof2_serdes_rx_polarity_hw_get(dev_id, dev_port, ln, &hw_rx_inv);
    aim_printf(
        &uc->pvs, "  %d (%d)|  %d (%d)|", tx_inv, hw_tx_inv, rx_inv, hw_rx_inv);
    bf_tof2_serdes_tx_bandgap_get(dev_id, dev_port, ln, &tx_bg);
    bf_tof2_serdes_rx_bandgap_get(dev_id, dev_port, ln, &rx_bg);
    bf_tof2_serdes_tx_bandgap_hw_get(dev_id, dev_port, ln, &hw_tx_bg);
    bf_tof2_serdes_rx_bandgap_hw_get(dev_id, dev_port, ln, &hw_rx_bg);
    aim_printf(
        &uc->pvs, "  %d (%d)|  %d (%d)|", tx_bg, hw_tx_bg, rx_bg, hw_rx_bg);

    bool phy_mode_tx_en;
    bool phy_mode_rx_en;
    bool anlt_mode_tx_en;
    bool anlt_mode_rx_en;
    bool hw_rx_en, hw_tx_en;

    bf_tof2_serdes_precode_get(dev_id, dev_port, ln, &hw_tx_en, &hw_rx_en);
    bf_tof2_serdes_fw_precode_get(dev_id,
                                  dev_port,
                                  ln,
                                  &phy_mode_tx_en,
                                  &phy_mode_rx_en,
                                  &anlt_mode_tx_en,
                                  &anlt_mode_rx_en);
    aim_printf(&uc->pvs,
               "       |  %d  |  %d  |  %d  |  %d  |  %d  |  %d  |\n",
               phy_mode_tx_en,
               phy_mode_rx_en,
               anlt_mode_tx_en,
               anlt_mode_rx_en,
               hw_tx_en,
               hw_rx_en);
  }

  aim_printf(&uc->pvs, "\n");
  aim_printf(&uc->pvs, "+---+-----+-------+----+---------+----------+\n");
  aim_printf(&uc->pvs, "| FP|devid|devport|Lane|Term mode|PRBS TxCfg|\n");
  aim_printf(&uc->pvs, "+---+-----+-------+----+---------+----------+\n");
  for (int ln = 0; ln < num_lanes; ln++) {
    bool ac_coupled;
    uint32_t tx_cfg = 0;
    bf_tof2_serdes_term_mode_get(dev_id, dev_port, ln, &ac_coupled);
    bf_tof2_serdes_tx_prbs_cfg_get(dev_id, dev_port, ln, &tx_cfg);
    aim_printf(&uc->pvs,
               "|%3d|%5d|%7d|%4d| %-7s |%#10x|\n",
               fp,
               dev_id,
               dev_port,
               ch + ln,
               (ac_coupled == true) ? "AC" : "DC",
               tx_cfg);
  }
}

void fw_serdes_params_separator(ucli_context_t *uc) {
  char *line_separator =
      "#+----------------------------------------------------------------------"
      "------------------------------------------------------------------------"
      "----------------------------------------------+";
  aim_printf(&uc->pvs, "%s\n", line_separator);
}

void fw_serdes_params_banner(ucli_context_t *uc) {
  fw_serdes_params_separator(uc);
  aim_printf(&uc->pvs,
             "\n#|   |     |        |    |   |    |     |     |    COUNTERS    "
             " |SD,Rdy,| FRQ |  CHANNEL   |      CTLE     |   |   | EYE MARGIN "
             " |         DFE       | TIMING  |           FFE Taps           |");
  aim_printf(&uc->pvs,
             "\n#|FP |devid|dev port|Tile|Grp|Lane| Mode|Speed| Adp "
             ",ReAdp,LLost|AdpDone| PPM | Est ,OF,HF |Peaking, G1,G2 |SK |DAC| "
             " 1 , 2 , 3  | F0 , F1 ,F1/F0,F13|Del,Edge | K1 ,   K2  ,   K3  , "
             "  K4    |");
  fw_serdes_params_separator(uc);
}

void fw_serdes_params(uint32_t fp, uint32_t dev_port) {
  uint32_t tile, grp, ln;
  uint32_t mac, ch;
  bf_status_t rc;
  bool loaded;
  bf_dev_id_t dev_id;
  char *line_separator;
  (void)fp;
  int num_lanes;
  ucli_context_t *uc = serdes_uc;

  dev_id = 0;
  lld_sku_map_dev_port_id_to_mac_ch(dev_id, dev_port, &mac, &ch);
  lld_sku_map_mac_stn_id_to_tile_and_group(dev_id, mac, &tile, &grp);

  rc = bf_tof2_serdes_fw_loaded_get(dev_id, dev_port, &loaded);
  if (rc || !loaded) {
    aim_printf(&uc->pvs, "\n######## FW is Not Loaded ! #########\n");
    return;
  }

  line_separator =
      "\n#+--------------------------------------------------------------------"
      "------------------------------------------------------------------------"
      "------------------------------------------------+";
  aim_printf(&uc->pvs, "%s", line_separator);
  aim_printf(&uc->pvs,
             "\n#|   |     |        |    |   |    |     |     |    COUNTERS    "
             " |SD,Rdy,| FRQ |  CHANNEL   |      CTLE     |   |   | EYE MARGIN "
             " |         DFE       | TIMING  |           FFE Taps           |");
  aim_printf(
      &uc->pvs,
      "\n#|FP |devid|dev port|Tile|Grp|Lane| Mode|Speed| Adp "
      ",ReAdp,LLost|AdpDone| PPM | Est ,OF,HF |Peaking, G1 ,G2  |SK |DAC| "
      " 1 , 2 , 3  | F0 , F1 ,F1/F0,F13|Del,Edge | K1 ,   K2  ,   K3  , "
      "  K4    |");
  aim_printf(&uc->pvs, "%s", line_separator);

  bf_port_num_lanes_get(dev_id, dev_port, &num_lanes);

  for (ln = 0; ln < (uint32_t)num_lanes; ln++) {
    uint32_t G, adapt, readapt, link_lost, of, hf;
    uint32_t skef_val, dac_val;
    uint32_t ctle_over_val, ctle_map_0, ctle_map_1, ctle_1, ctle_2, dc_gain_1,
        dc_gain_2;
    char dc_gain_1_over_limit = ' ';
    char dc_gain_2_over_limit = ' ';
    uint32_t delta, edge1, edge2, edge3, edge4;
    uint32_t tap1, tap2, tap3, f13_val;
    int signed_ppm, signed7_delta;
    bf_serdes_encoding_mode_t enc_mode;
    bool sig_detect, phy_ready, adapt_done;
    int32_t ppm;
    char *mode_str[] = {"NONE", "NRZ ", "PAM4"};
    float chan_est, eye_1, eye_2, eye_3;
    float f0, f1, ratio;
    char *sd_flag;

    lld_sku_map_dev_port_id_to_mac_ch(dev_id, dev_port, &mac, &ch);

    lld_sku_map_mac_stn_id_to_tile_and_group(dev_id, mac, &tile, &grp);

    rc = bf_tof2_serdes_fw_lane_speed_get(dev_id, dev_port, ln, &G, &enc_mode);
    if (rc) {
      aim_printf(&uc->pvs, "bf_tof2_serdes_fw_lane_speed_get: rtnd: %d\n", rc);
      return;
    }
    if (G == 0) {  // OFF
      aim_printf(&uc->pvs,
                 "\n#|%2d | %3d |   %3d  | %d | %d |  %d |%5s| %3dG|",
                 fp,
                 dev_id,
                 dev_port,
                 tile,
                 grp,
                 ch + ln,
                 mode_str[enc_mode],
                 G);
      continue;
    }
    rc = bf_tof2_serdes_rx_sig_info_get(
        dev_id, dev_port, ln, &sig_detect, &phy_ready, &ppm);
    if (rc != BF_SUCCESS) continue;
    sd_flag = sig_detect ? " " : "*";

    rc = bf_tof2_serdes_adapt_counts_get(
        dev_id, dev_port, ln, &adapt_done, &adapt, &readapt, &link_lost);
    signed_ppm = ppm;
    if (rc != BF_SUCCESS) continue;
    aim_printf(&uc->pvs,
               "\n#|%2d | %3d |   %3d  |  %d | %d |  %d |%5s| %3dG|%5d,%5d,%4d "
               "|%s%d,%d,%d%s|%4d |",
               fp,
               dev_id,
               dev_port,
               tile,
               grp,
               ch + ln,
               mode_str[enc_mode],
               G,
               adapt,
               readapt,
               link_lost,
               sd_flag,
               sig_detect,
               phy_ready,
               adapt_done,
               phy_ready ? " " : "*",
               signed_ppm);

    rc = bf_tof2_serdes_of_get(dev_id, dev_port, ln, &of, &hf);
    if (rc != BF_SUCCESS) continue;
    chan_est = (hf ? (float)of / (float)hf : NAN);
    aim_printf(&uc->pvs, " %4.2f,%2d,%2d ", chan_est, of, hf);

    bf_tof2_serdes_ctle_over_val_get(dev_id, dev_port, ln, &ctle_over_val);
    bf_tof2_serdes_ctle_val_get(
        dev_id, dev_port, ln, ctle_over_val, &ctle_map_0, &ctle_map_1);
    ctle_1 = ctle_map_0;
    ctle_2 = ctle_map_1;

    if (enc_mode == 2) {  // PAM4
      uint32_t ctle_map_7_0, ctle_map_7_1;
      // if (ctle_1_bit4 == 1 or ctle_2_bit4 == 1 or
      // chip.PAM50[ln].ctle_map_pam4(7)[0] == 7): ctle_val += 8
      bf_tof2_serdes_ctle_val_get(
          dev_id, dev_port, ln, 7, &ctle_map_7_0, &ctle_map_7_1);
      if ((ctle_1 & 0x8) || (ctle_2 & 0x8) || (ctle_map_7_0 == 7)) {
        ctle_over_val += 8;
      }
    }

    bf_tof2_serdes_ctle_gain_get(dev_id, dev_port, ln, &dc_gain_1, &dc_gain_2);
    if (dc_gain_1 >= 63) dc_gain_1_over_limit = '*';
    if (dc_gain_2 >= 63) dc_gain_2_over_limit = '*';
    aim_printf(&uc->pvs,
               "| %2d(%d,%d),%3d%c,%2d%c",
               ctle_over_val,
               ctle_1,
               ctle_2,
               dc_gain_1,
               dc_gain_1_over_limit,
               dc_gain_2,
               dc_gain_2_over_limit);

    rc = bf_tof2_serdes_skef_val_get(dev_id, dev_port, ln, &skef_val);
    if (rc != BF_SUCCESS) continue;
    rc = bf_tof2_serdes_dac_val_get(dev_id, dev_port, ln, &dac_val);
    if (rc != BF_SUCCESS) continue;

    aim_printf(&uc->pvs, "| %d | %2d", skef_val, dac_val);

    rc = bf_tof2_serdes_eye_get(dev_id, dev_port, ln, &eye_1, &eye_2, &eye_3);
    if (enc_mode == 1) {  // NRZ
      aim_printf(&uc->pvs, "|    %4.0f     ", eye_1);
    } else {
      aim_printf(&uc->pvs, "|%4.0f,%3.0f,%3.0f ", eye_1, eye_2, eye_3);
    }
    bf_tof2_serdes_delta_get(dev_id, dev_port, ln, &delta);
    if (delta & (1 << 6)) {
      signed7_delta = 0 - (128 - delta);
    } else {
      signed7_delta = delta;
    }
    bf_tof2_serdes_edge_get(
        dev_id, dev_port, ln, &edge1, &edge2, &edge3, &edge4);
    if (enc_mode == 1) {  // NRZ
      bf_tof2_serdes_dfe_nrz_get(dev_id, dev_port, ln, &tap1, &tap2, &tap3);
      if (tap1 > 63) tap1 = 0 - (128 - tap1);
      if (tap2 > 63) tap2 = 0 - (128 - tap2);
      if (tap3 > 63) tap3 = 0 - (128 - tap3);
      aim_printf(&uc->pvs, "| %4d,%4d,%4d    ", tap1, tap2, tap3);
    } else {
      bf_tof2_serdes_dfe_pam4_get(dev_id, dev_port, ln, &f0, &f1, &ratio);
      bf_tof2_serdes_f13_val_pam4_get(dev_id, dev_port, ln, &f13_val);
      aim_printf(&uc->pvs, "|%4.2f,%4.2f,%5.2f,%2d ", f0, f1, ratio, f13_val);
    }
    aim_printf(
        &uc->pvs, "|%3d,%X%X%X%X |", signed7_delta, edge1, edge2, edge3, edge4);

    if (enc_mode == 2) {  // PAM4
      int32_t k1, k2, k3, k4, s1, s2;
      bf_tof2_serdes_ffe_taps_pam4_get(
          dev_id, dev_port, ln, &k1, &k2, &k3, &k4, &s1, &s2);
      aim_printf(&uc->pvs,
                 "%4.2f, %6.3f, %6.4f, %6.4f, ",
                 (float)k1 * 0.01,
                 (float)k2 * 0.008 * ((float)s1 / 4),
                 (float)k3 * 0.0067 * ((float)s2 / 4) * ((float)s1 / 4),
                 (float)k4 * 0.0053 * ((float)s2 / 4) * ((float)s1 / 4));
    }
  }
  aim_printf(&uc->pvs, "\n");
}

void fw_serdes_info(uint32_t dev_id, uint32_t dev_port, info_t *info) {
  uint32_t ln;
  int num_lanes;

  bf_port_num_lanes_get(dev_id, dev_port, &num_lanes);

  for (ln = 0; ln < (uint32_t)num_lanes; ln++) {
    uint32_t G, adapt, readapt, link_lost, of, hf;
    uint32_t skef_val, dac_val;
    uint32_t ctle_over_val, ctle_map_0, ctle_map_1, ctle_1, ctle_2, dc_gain_1,
        dc_gain_2;
    uint32_t delta, edge1, edge2, edge3, edge4;
    int signed_ppm, signed7_delta;
    bf_serdes_encoding_mode_t enc_mode;
    bool sig_detect, phy_ready, adapt_done;
    int32_t ppm;
    float chan_est, eye_1, eye_2, eye_3;
    float f0 = 0, f1 = 0, ratio = 0;
    int32_t k1 = 0, k2 = 0, k3 = 0, k4 = 0, s1 = 0, s2 = 0;
    uint32_t tap1 = 0, tap2 = 0, tap3 = 0, f13_val = 0;

    bf_tof2_serdes_fw_lane_speed_get(dev_id, dev_port, ln, &G, &enc_mode);
    bf_tof2_serdes_rx_sig_info_get(
        dev_id, dev_port, ln, &sig_detect, &phy_ready, &ppm);
    bf_tof2_serdes_adapt_counts_get(
        dev_id, dev_port, ln, &adapt_done, &adapt, &readapt, &link_lost);
    if (ppm & (1 << 10)) {
      signed_ppm = (int)ppm - 2048;
    } else {
      signed_ppm = ppm;
    }
    bf_tof2_serdes_of_get(dev_id, dev_port, ln, &of, &hf);
    chan_est = (hf ? (float)of / (float)hf : NAN);

    bf_tof2_serdes_ctle_over_val_get(dev_id, dev_port, ln, &ctle_over_val);
    bf_tof2_serdes_ctle_val_get(
        dev_id, dev_port, ln, ctle_over_val, &ctle_map_0, &ctle_map_1);
    ctle_1 = ctle_map_0;
    ctle_2 = ctle_map_1;

    if (enc_mode == 2) {  // PAM4
      uint32_t ctle_map_7_0, ctle_map_7_1;
      // if (ctle_1_bit4 == 1 or ctle_2_bit4 == 1 or
      // chip.PAM50[ln].ctle_map_pam4(7)[0] == 7): ctle_val += 8
      bf_tof2_serdes_ctle_val_get(
          dev_id, dev_port, ln, 7, &ctle_map_7_0, &ctle_map_7_1);
      if ((ctle_1 & 0x8) || (ctle_2 & 0x8) || (ctle_map_7_0 == 7)) {
        ctle_over_val += 8;
      }
    }
    bf_tof2_serdes_ctle_gain_get(dev_id, dev_port, ln, &dc_gain_1, &dc_gain_2);
    bf_tof2_serdes_skef_val_get(dev_id, dev_port, ln, &skef_val);
    bf_tof2_serdes_dac_val_get(dev_id, dev_port, ln, &dac_val);
    bf_tof2_serdes_eye_get(dev_id, dev_port, ln, &eye_1, &eye_2, &eye_3);
    bf_tof2_serdes_delta_get(dev_id, dev_port, ln, &delta);
    if (delta & (1 << 6)) {
      signed7_delta = 0 - (128 - delta);
    } else {
      signed7_delta = delta;
    }
    bf_tof2_serdes_edge_get(
        dev_id, dev_port, ln, &edge1, &edge2, &edge3, &edge4);
    if (enc_mode == 1) {  // NRZ
      bf_tof2_serdes_dfe_nrz_get(dev_id, dev_port, ln, &tap1, &tap2, &tap3);
    } else {
      bf_tof2_serdes_dfe_pam4_get(dev_id, dev_port, ln, &f0, &f1, &ratio);
      bf_tof2_serdes_f13_val_pam4_get(dev_id, dev_port, ln, &f13_val);
    }
    if (enc_mode == 2) {  // PAM4
      bf_tof2_serdes_ffe_taps_pam4_get(
          dev_id, dev_port, ln, &k1, &k2, &k3, &k4, &s1, &s2);
    }
    info->serdes[ln].G = G;
    info->serdes[ln].adapt = adapt;
    info->serdes[ln].readapt = readapt;
    info->serdes[ln].link_lost = link_lost;
    info->serdes[ln].of = of;
    info->serdes[ln].hf = hf;
    info->serdes[ln].skef_val = skef_val;
    info->serdes[ln].dac_val = dac_val;
    info->serdes[ln].ctle_over_val = ctle_over_val;
    info->serdes[ln].ctle_map_0 = ctle_map_0;
    info->serdes[ln].ctle_map_1 = ctle_map_1;
    info->serdes[ln].ctle_1 = ctle_1;
    info->serdes[ln].ctle_2 = ctle_2;
    info->serdes[ln].dc_gain_1 = dc_gain_1;
    info->serdes[ln].dc_gain_2 = dc_gain_2;
    info->serdes[ln].delta = delta;
    info->serdes[ln].edge1 = edge1;
    info->serdes[ln].edge2 = edge2;
    info->serdes[ln].edge3 = edge3;
    info->serdes[ln].edge4 = edge4;
    info->serdes[ln].tap1 = tap1;
    info->serdes[ln].tap2 = tap2;
    info->serdes[ln].tap3 = tap3;
    info->serdes[ln].f13_val = f13_val;
    info->serdes[ln].signed_ppm = signed_ppm;
    info->serdes[ln].signed7_delta = signed7_delta;
    info->serdes[ln].enc_mode = enc_mode;
    info->serdes[ln].sig_detect = sig_detect;
    info->serdes[ln].phy_ready = phy_ready;
    info->serdes[ln].adapt_done = adapt_done;
    info->serdes[ln].ppm = ppm;
    info->serdes[ln].chan_est = chan_est;
    info->serdes[ln].eye_1 = eye_1;
    info->serdes[ln].eye_2 = eye_2;
    info->serdes[ln].eye_3 = eye_3;
    info->serdes[ln].f0 = f0;
    info->serdes[ln].f1 = f1;
    info->serdes[ln].ratio = ratio;
    info->serdes[ln].k1 = k1;
    info->serdes[ln].k2 = k2;
    info->serdes[ln].k3 = k3;
    info->serdes[ln].k4 = k4;
    info->serdes[ln].s1 = s1;
    info->serdes[ln].s2 = s2;
  }
}

void bf_pm_log_state_chg_info(uint32_t dev_id, uint32_t dev_port, bool st) {
  bf_pm_port_info_t *port_info = NULL;
  uint64_t reg64;

  return;

  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) return;

  gettimeofday(&port_info->history[port_info->history_next].tv, NULL);
  port_info->history[port_info->history_next].st = st;
  bf_port_umac4_interrupt_get(
      dev_id, dev_port, &port_info->history[port_info->history_next].int_reg);
  bf_port_umac4_status_get(
      dev_id,
      dev_port,
      &reg64,
      &port_info->history[port_info->history_next].pcs.txclkpresentall,
      &port_info->history[port_info->history_next].pcs.rxclkpresentall,
      &port_info->history[port_info->history_next].pcs.rxsigokall,
      &port_info->history[port_info->history_next].pcs.blocklockall,
      &port_info->history[port_info->history_next].pcs.amlockall,
      &port_info->history[port_info->history_next].pcs.aligned,
      &port_info->history[port_info->history_next].pcs.nohiber,
      &port_info->history[port_info->history_next].pcs.nolocalfault,
      &port_info->history[port_info->history_next].pcs.noremotefault,
      &port_info->history[port_info->history_next].pcs.linkup,
      &port_info->history[port_info->history_next].pcs.hiser,
      &port_info->history[port_info->history_next].pcs.fecdegser,
      &port_info->history[port_info->history_next].pcs.rxamsf);
  fw_serdes_info(
      dev_id, dev_port, &port_info->history[port_info->history_next]);
  port_info->history_next = (port_info->history_next + 1) % 8;
}

void fw_serdes_info_display(ucli_context_t *uc,
                            uint32_t dev_id,
                            uint32_t dev_port,
                            uint32_t ln,
                            info_t *info) {
  bf_pm_port_info_t *port_info = NULL;
  bf_pal_front_port_handle_t *port_hdl = NULL;
  char *mode_str[] = {"NONE", "NRZ ", "PAM4"};
  uint32_t tile, grp;
  uint32_t mac, ch;
  char *sd_flag;

  lld_sku_map_dev_port_id_to_mac_ch(dev_id, dev_port, &mac, &ch);

  lld_sku_map_mac_stn_id_to_tile_and_group(dev_id, mac, &tile, &grp);

  port_info = pm_port_info_get(dev_id, dev_port);
  if (!port_info) return;
  port_hdl = &port_info->pltfm_port_info.port_hdl;

  if (info->serdes[ln].G == 0) {  // OFF
    aim_printf(&uc->pvs,
               "#|%2d | %3d |   %3d  | %d | %d |  %d |%5s| %3dG|",
               port_hdl->conn_id,
               dev_id,
               dev_port,
               tile,
               grp,
               port_hdl->chnl_id + ln,
               mode_str[info->serdes[ln].enc_mode],
               info->serdes[ln].G);
    return;
    ;
  }
  sd_flag = info->serdes[ln].sig_detect ? " " : "*";

  aim_printf(&uc->pvs,
             "#|%2d | %3d |   %3d  |  %d | %d |  %d |%5s| %3dG|%5d,%5d,%4d "
             "|%s%d,%d,%d%s|%4d |",
             port_hdl->conn_id,
             dev_id,
             dev_port,
             tile,
             grp,
             port_hdl->chnl_id + ln,
             mode_str[info->serdes[ln].enc_mode],
             info->serdes[ln].G,
             info->serdes[ln].adapt,
             info->serdes[ln].readapt,
             info->serdes[ln].link_lost,
             sd_flag,
             info->serdes[ln].sig_detect,
             info->serdes[ln].phy_ready,
             info->serdes[ln].adapt_done,
             info->serdes[ln].phy_ready ? " " : "*",
             info->serdes[ln].signed_ppm);

  aim_printf(&uc->pvs,
             " %4.2f,%2d,%2d ",
             info->serdes[ln].chan_est,
             info->serdes[ln].of,
             info->serdes[ln].hf);

  aim_printf(&uc->pvs,
             "| %2d(%d,%d),%3d,%2d",
             info->serdes[ln].ctle_over_val,
             info->serdes[ln].ctle_1,
             info->serdes[ln].ctle_2,
             info->serdes[ln].dc_gain_1,
             info->serdes[ln].dc_gain_2);

  aim_printf(&uc->pvs,
             "| %d | %2d",
             info->serdes[ln].skef_val,
             info->serdes[ln].dac_val);

  if (info->serdes[ln].enc_mode == 1) {  // NRZ
    aim_printf(&uc->pvs, "|    %4.0f     ", info->serdes[ln].eye_1);
  } else {
    aim_printf(&uc->pvs,
               "|%4.0f,%3.0f,%3.0f ",
               info->serdes[ln].eye_1,
               info->serdes[ln].eye_2,
               info->serdes[ln].eye_3);
  }
  if (info->serdes[ln].enc_mode == 1) {  // NRZ
    aim_printf(&uc->pvs,
               "| %4d,%4d,%4d    ",
               info->serdes[ln].tap1,
               info->serdes[ln].tap2,
               info->serdes[ln].tap3);
  } else {
    aim_printf(&uc->pvs,
               "|%4.2f,%4.2f,%5.2f,%2d ",
               info->serdes[ln].f0,
               info->serdes[ln].f1,
               info->serdes[ln].ratio,
               info->serdes[ln].f13_val);
  }
  aim_printf(&uc->pvs,
             "|%3d,%X%X%X%X |",
             info->serdes[ln].signed7_delta,
             info->serdes[ln].edge1,
             info->serdes[ln].edge2,
             info->serdes[ln].edge3,
             info->serdes[ln].edge4);

  if (info->serdes[ln].enc_mode == 2) {  // PAM4
    aim_printf(&uc->pvs,
               "%4d,%4d,%4d,%4d,%02X,%02X| ",
               info->serdes[ln].k1,
               info->serdes[ln].k2,
               info->serdes[ln].k3,
               info->serdes[ln].k4,
               info->serdes[ln].s1,
               info->serdes[ln].s2);
  }
  aim_printf(&uc->pvs, "\n");
}

typedef struct rx_mon_st {
  uint32_t cnt;
  uint32_t cnt_n1;
  uint32_t cnt_n2;
  struct timeval prbs_cnt_time;
  struct timeval prbs_reset_time;
  uint32_t cnt_time;
  float eyes_0;
  float eyes_1;
  float eyes_2;
  uint32_t link_status;
  uint32_t tei;
  uint32_t teo;
} rx_mon_st;

rx_mon_st rx_mon[33][8] = {{{0}}};  // indexed by {front_port, lane}

static uint32_t fec_analyzer_tei(uint32_t dev_port, uint32_t ln) {
  uint32_t dev_id, tei;

  dev_id = 0;

  bf_tof2_serdes_fec_analyzer_tei_get(dev_id, dev_port, ln, &tei);
  return tei;
}

static uint32_t fec_analyzer_teo(uint32_t dev_port, uint32_t ln) {
  uint32_t dev_id, teo;

  dev_id = 0;

  bf_tof2_serdes_fec_analyzer_teo_get(dev_id, dev_port, ln, &teo);
  return teo;
}

static void rx_monitor_clear(uint32_t fp,
                             uint32_t chnl,
                             bf_dev_port_t dev_port,
                             uint32_t ln) {
  bf_serdes_encoding_mode_t enc_mode;
  uint32_t G;
  uint32_t dev_id;

  dev_id = 0;

  memset((char *)&rx_mon[fp][chnl], 0, sizeof(rx_mon[fp][chnl]));

  bf_tof2_serdes_fw_lane_speed_get(dev_id, dev_port, ln, &G, &enc_mode);
  if (enc_mode == 2) {  // PAM4
    bf_tof2_serdes_fec_analyzer_init_set(
        dev_id, dev_port, ln, 0, 15 /*T*/, 10 /*M*/, 5440 /*N*/);
  } else {
    bf_tof2_serdes_fec_analyzer_init_set(
        dev_id, dev_port, ln, 0, 7 /*T*/, 10 /*M*/, 5280 /*N*/);
  }
  bf_sys_usleep(100000);
  bf_tof2_serdes_prbs_rst_set(dev_id, dev_port, ln);
  gettimeofday(&rx_mon[fp][chnl].prbs_reset_time, NULL);
}

static void rx_monitor_capture(uint32_t fp,
                               uint32_t chnl,
                               bf_dev_port_t dev_port,
                               uint32_t ln) {
  uint32_t dev_id;
  uint32_t cnt;
  bool sd, phy_rdy, rdy;
  int32_t ppm;

  dev_id = 0;

  //###### 1. Capture FEC Analyzer Data for this lane
  rx_mon[fp][chnl].tei = fec_analyzer_tei(dev_port, ln);
  rx_mon[fp][chnl].teo = fec_analyzer_teo(dev_port, ln);
  //###### 2. Capture PRBS Counter for this lane
  bf_tof2_serdes_rx_prbs_err_get(dev_id, dev_port, ln, &cnt);
  // cnt1 = cnt; // can we assume this?
  gettimeofday(&rx_mon[fp][chnl].prbs_cnt_time, NULL);
  bf_tof2_serdes_rx_sig_info_get(dev_id, dev_port, ln, &sd, &phy_rdy, &ppm);
  rdy = sd && phy_rdy;
  rx_mon[fp][chnl].link_status = rdy;
  if ((rx_mon[fp][chnl].prbs_reset_time.tv_sec == 0) &&
      (rx_mon[fp][chnl].prbs_reset_time.tv_usec == 0)) {
    rx_mon[fp][chnl].prbs_reset_time = rx_mon[fp][chnl].prbs_cnt_time;
  }
  if (!rdy) {
    rx_mon[fp][chnl].tei = 0xEEEEEEEE;
    rx_mon[fp][chnl].teo = 0xEEEEEEEE;
    rx_mon[fp][chnl].cnt =
        0xEEEEEEEE;  //  # return an artificially large number if RDY=0
    rx_mon[fp][chnl].cnt_n1 = 0;  //  # PrevPrbsCount-1
    rx_mon[fp][chnl].cnt_n2 = 0;  //  # PrevPrbsCount-2
    rx_mon[fp][chnl].eyes_0 = -1;
    rx_mon[fp][chnl].eyes_1 = -1;
    rx_mon[fp][chnl].eyes_2 = -1;
  } else {
    bf_tof2_serdes_eye_get(dev_id,
                           dev_port,
                           ln,
                           &rx_mon[fp][chnl].eyes_0,
                           &rx_mon[fp][chnl].eyes_1,
                           &rx_mon[fp][chnl].eyes_2);
  }
  rx_mon[fp][chnl].cnt_n2 = rx_mon[fp][chnl].cnt_n1;
  rx_mon[fp][chnl].cnt_n1 = rx_mon[fp][chnl].cnt;
  rx_mon[fp][chnl].cnt = cnt;
}

static void rx_monitor_print_prbs31_stats(uint32_t fp,
                                          bf_dev_id_t dev_id,
                                          bf_dev_port_t dev_port,
                                          uint32_t chnl) {
  uint32_t num_lanes;
  ucli_context_t *uc = serdes_uc;
  uint32_t ln, G;
  bf_serdes_encoding_mode_t enc_mode;

  bf_port_num_lanes_get(dev_id, dev_port, (int *)&num_lanes);
  aim_printf(&uc->pvs, "\n Elapsed Time");
  for (ln = 0; ln < num_lanes; ln++) {
    if (rx_mon[fp][chnl + ln].link_status) {
      struct timeval res;
      uint64_t elapsed_time;
      float f_elapsed_time;

      timersub(&rx_mon[fp][chnl + ln].prbs_cnt_time,
               &rx_mon[fp][chnl + ln].prbs_reset_time,
               &res);
      elapsed_time = res.tv_sec * 1000000 + res.tv_usec;
      f_elapsed_time = (float)elapsed_time / 1000000;
      aim_printf(&uc->pvs, "%6.1f s", f_elapsed_time);
    } else {
      aim_printf(&uc->pvs, "       -");
    }
  }
  aim_printf(&uc->pvs, "\n     PRBS Cnt");
  for (ln = 0; ln < num_lanes; ln++) {
    if (rx_mon[fp][chnl + ln].link_status) {
      aim_printf(&uc->pvs, "%8X", rx_mon[fp][chnl + ln].cnt);
    } else {
      aim_printf(&uc->pvs, "       -");
    }
  }
  aim_printf(&uc->pvs, "\n     PRBS BER");
  for (ln = 0; ln < num_lanes; ln++) {
    if (rx_mon[fp][chnl + ln].link_status) {
      struct timeval res;
      uint64_t elapsed_time, bits_transferred;
      float f_elapsed_time, data_rate;

      timersub(&rx_mon[fp][chnl + ln].prbs_cnt_time,
               &rx_mon[fp][chnl + ln].prbs_reset_time,
               &res);
      elapsed_time = res.tv_sec * 1000000 + res.tv_usec;
      f_elapsed_time = (float)elapsed_time / 1000000;
      bf_tof2_serdes_fw_lane_speed_get(dev_id, dev_port, ln, &G, &enc_mode);
      if (G == 53) {
        data_rate = 53.125;
      } else if (G == 25) {
        data_rate = 25.78125;
      } else if (G == 10) {
        data_rate = 10.3125;
      } else if (G == 1) {
        data_rate = 1.25;
      } else {
        data_rate = 0.0;
      }
      bits_transferred =
          (float)((f_elapsed_time * data_rate * (float)1000000000.0));
      if ((rx_mon[fp][chnl + ln].cnt == 0) || (bits_transferred == 0)) {
        aim_printf(&uc->pvs, "%8.1e", 0.0);
      } else {
        aim_printf(&uc->pvs,
                   "%8.1e",
                   (float)rx_mon[fp][chnl + ln].cnt / (float)bits_transferred);
      }
    } else {
      aim_printf(&uc->pvs, "       -");
    }
  }
  aim_printf(&uc->pvs, "\n-------------");
  for (ln = 0; ln < num_lanes; ln++) {
    aim_printf(&uc->pvs, "--------");
  }
  aim_printf(&uc->pvs, "\n  pre-FEC Cnt");
  for (ln = 0; ln < num_lanes; ln++) {
    if (rx_mon[fp][chnl + ln].link_status) {
      aim_printf(&uc->pvs, "%8X", rx_mon[fp][chnl + ln].tei);
    } else {
      aim_printf(&uc->pvs, "       -");
    }
  }
  aim_printf(&uc->pvs, "\n Post-FEC Cnt");
  for (ln = 0; ln < num_lanes; ln++) {
    if (rx_mon[fp][chnl + ln].link_status) {
      aim_printf(&uc->pvs, "%8X", rx_mon[fp][chnl + ln].teo);
    } else {
      aim_printf(&uc->pvs, "       -");
    }
  }
  aim_printf(&uc->pvs, "\n  pre-FEC BER");
  for (ln = 0; ln < num_lanes; ln++) {
    if (rx_mon[fp][chnl + ln].link_status) {
      struct timeval res;
      uint64_t elapsed_time, bits_transferred;
      float f_elapsed_time, data_rate;

      timersub(&rx_mon[fp][chnl + ln].prbs_cnt_time,
               &rx_mon[fp][chnl + ln].prbs_reset_time,
               &res);
      elapsed_time = res.tv_sec * 1000000 + res.tv_usec;
      f_elapsed_time = (float)elapsed_time / 1000000;
      bf_tof2_serdes_fw_lane_speed_get(dev_id, dev_port, ln, &G, &enc_mode);
      if (G == 53) {
        data_rate = 53.125;
      } else if (G == 25) {
        data_rate = 25.78125;
      } else if (G == 10) {
        data_rate = 10.3125;
      } else if (G == 1) {
        data_rate = 1.25;
      } else {
        data_rate = 0.0;
      }
      bits_transferred =
          (float)((f_elapsed_time * data_rate * (float)1000000000.0));
      if ((rx_mon[fp][chnl + ln].tei == 0) || (bits_transferred == 0)) {
        aim_printf(&uc->pvs, "%8.1e", 0.0);
      } else {
        aim_printf(&uc->pvs,
                   "%8.1e",
                   (float)rx_mon[fp][chnl + ln].tei / (float)bits_transferred);
      }
    } else {
      aim_printf(&uc->pvs, "       -");
    }
  }
  aim_printf(&uc->pvs, "\n Post-FEC BER");
  for (ln = 0; ln < num_lanes; ln++) {
    if (rx_mon[fp][chnl + ln].link_status) {
      struct timeval res;
      uint64_t elapsed_time, bits_transferred;
      float f_elapsed_time, data_rate;

      timersub(&rx_mon[fp][chnl + ln].prbs_cnt_time,
               &rx_mon[fp][chnl + ln].prbs_reset_time,
               &res);
      elapsed_time = res.tv_sec * 1000000 + res.tv_usec;
      f_elapsed_time = (float)elapsed_time / 1000000;
      bf_tof2_serdes_fw_lane_speed_get(dev_id, dev_port, ln, &G, &enc_mode);
      if (G == 53) {
        data_rate = 53.125;
      } else if (G == 25) {
        data_rate = 25.78125;
      } else if (G == 10) {
        data_rate = 10.3125;
      } else if (G == 1) {
        data_rate = 1.25;
      } else {
        data_rate = 0.0;
      }
      bits_transferred =
          (float)((f_elapsed_time * data_rate * (float)1000000000.0));
      if ((rx_mon[fp][chnl + ln].teo == 0) || (bits_transferred == 0)) {
        aim_printf(&uc->pvs, "%8d", 0);
      } else {
        aim_printf(&uc->pvs,
                   "%8.1e",
                   (float)rx_mon[fp][chnl + ln].teo / (float)bits_transferred);
      }
    } else {
      aim_printf(&uc->pvs, "       -");
    }
  }
  aim_printf(&uc->pvs, "\n-------------");
  for (ln = 0; ln < num_lanes; ln++) {
    aim_printf(&uc->pvs, "--------");
  }
  aim_printf(&uc->pvs, "\n");
}

static void rx_monitor_print(uint32_t fp,
                             uint32_t chnl,
                             bf_dev_port_t dev_port,
                             uint32_t ln) {
  uint32_t tile, grp, G;
  bf_serdes_encoding_mode_t enc_mode;
  bf_dev_id_t dev_id = 0;
  char *lane_name_list[] = {"A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7"};
  char *mode_str[] = {"NONE", "NRZ ", "PAM4"};
  uint32_t num_lanes;
  uint32_t mac, ch;
  ucli_context_t *uc = serdes_uc;
  bf_port_prbs_mode_t prbs_mode;

  bf_port_num_lanes_get(dev_id, dev_port, (int *)&num_lanes);

  lld_sku_map_dev_port_id_to_mac_ch(dev_id, dev_port, &mac, &ch);

  lld_sku_map_mac_stn_id_to_tile_and_group(dev_id, mac, &tile, &grp);

  aim_printf(&uc->pvs, "\n-------------");
  for (ln = 0; ln < num_lanes; ln++) {
    aim_printf(&uc->pvs, "--------");
  }

  aim_printf(&uc->pvs, "\n Tile,Grp,Ln ");
  for (ln = 0; ln < num_lanes; ln++) {
    aim_printf(&uc->pvs, "   %d,%d,%d", tile, grp, chnl + ln);
  }
  aim_printf(&uc->pvs, "\n         Lane");
  for (ln = 0; ln < num_lanes; ln++) {
    aim_printf(&uc->pvs, "%8s", lane_name_list[chnl + ln]);
  }
  aim_printf(&uc->pvs, "\n     Encoding");
  for (ln = 0; ln < num_lanes; ln++) {
    bf_tof2_serdes_fw_lane_speed_get(dev_id, dev_port, ln, &G, &enc_mode);
    aim_printf(&uc->pvs, "%8s", mode_str[enc_mode]);
  }
  aim_printf(&uc->pvs, "\nDataRate Gbps");
  for (ln = 0; ln < num_lanes; ln++) {
    float data_rate;

    bf_tof2_serdes_fw_lane_speed_get(dev_id, dev_port, ln, &G, &enc_mode);
    if (G == 53) {
      data_rate = 53.125;
    } else if (G == 25) {
      data_rate = 25.78125;
    } else if (G == 10) {
      data_rate = 10.3125;
    } else if (G == 1) {
      data_rate = 1.25;
    } else {
      data_rate = 0.0;
    }
    aim_printf(&uc->pvs, "%8.4f", data_rate);
  }
  aim_printf(&uc->pvs, "\n-------------");
  for (ln = 0; ln < num_lanes; ln++) {
    aim_printf(&uc->pvs, "--------");
  }
  aim_printf(&uc->pvs, "\n  Link Status");
  for (ln = 0; ln < num_lanes; ln++) {
    aim_printf(
        &uc->pvs, "%8s", rx_mon[fp][chnl + ln].link_status ? "RDY" : "NOT RDY");
  }
  aim_printf(&uc->pvs, "\n    Eye1 (mV)");
  for (ln = 0; ln < num_lanes; ln++) {
    if (rx_mon[fp][chnl + ln].link_status) {
      aim_printf(&uc->pvs, "%8.0f", rx_mon[fp][chnl + ln].eyes_0);
    } else {
      aim_printf(&uc->pvs, "       -");
    }
  }
  aim_printf(&uc->pvs, "\n    Eye2 (mV)");
  for (ln = 0; ln < num_lanes; ln++) {
    if ((enc_mode == 2) && rx_mon[fp][chnl + ln].link_status) {
      aim_printf(&uc->pvs, "%8.0f", rx_mon[fp][chnl + ln].eyes_1);
    } else {
      aim_printf(&uc->pvs, "       -");
    }
  }
  aim_printf(&uc->pvs, "\n    Eye3 (mV)");
  for (ln = 0; ln < num_lanes; ln++) {
    if ((enc_mode == 2) && rx_mon[fp][chnl + ln].link_status) {
      aim_printf(&uc->pvs, "%8.0f", rx_mon[fp][chnl + ln].eyes_2);
    } else {
      aim_printf(&uc->pvs, "       -");
    }
  }
  aim_printf(&uc->pvs, "\n-------------");
  for (ln = 0; ln < num_lanes; ln++) {
    aim_printf(&uc->pvs, "--------");
  }
  aim_printf(&uc->pvs, "\n");

  bf_port_prbs_mode_get(dev_id, dev_port, &prbs_mode);
  if (prbs_mode == BF_PORT_PRBS_MODE_31)
    rx_monitor_print_prbs31_stats(fp, dev_id, dev_port, chnl);
}

void rx_monitor(uint32_t fp,
                uint32_t chnl,
                bf_dev_port_t dev_port,
                uint32_t ln,
                bool rst,
                bool print_en,
                bool capture_en) {
  uint32_t dev_id;

  if (fp >= (sizeof(rx_mon) / sizeof(rx_mon[0])) ||
      chnl >= (sizeof(rx_mon[0]) / sizeof(rx_mon[0][0]))) {
    return;
  }

  dev_id = 0;
  if (rst) {
    rx_monitor_clear(fp, chnl, dev_port, ln);
  } else if ((rx_mon[fp][chnl].prbs_reset_time.tv_sec == 0) &&
             (rx_mon[fp][chnl].prbs_reset_time.tv_usec == 0)) {
    rx_monitor_clear(fp, chnl, dev_port, ln);
  }
  if (capture_en) {
    rx_monitor_capture(fp, chnl, dev_port, ln);
  }
  if (print_en) {
    rx_monitor_print(fp, chnl, dev_port, ln);
  }
  (void)dev_id;
}

void pll_info_banner(void) {
  char *line_separator =
      "\n#+--------------------------------------------------------------------"
      "------------------------------------------------------------------------"
      "---------------------------------------------+";
  aim_printf(&serdes_uc->pvs, "%s", line_separator);
  aim_printf(&serdes_uc->pvs,
             "\n#|     |     |                                       TX PLL    "
             "                                 |                               "
             "       RX PLL                                   |");
  aim_printf(&serdes_uc->pvs,
             "\n#| FP  |devid|dev port|lane|  DataRate  | FVCO,  PllCal | N | "
             "DIV4 | DIV2 |  REF_CLK  | FRAC_EN |  FRAC-N  |  DataRate  | "
             "FVCO,  PllCal | N | DIV4 | DIV2 |  REF_CLK  | FRAC_EN |  FRAC-N  "
             "|");
  aim_printf(&serdes_uc->pvs, "%s", line_separator);
}

void tof2_pll_display(int fp, bf_dev_port_t dev_port) {
  int ln, num_lanes;
  bf_dev_id_t dev_id = 0;

  pll_info_banner();
  bf_port_num_lanes_get(0, dev_port, (int *)&num_lanes);
  for (ln = 0; ln < num_lanes; ln++) {
    pll_info_t tx_pll_info;
    pll_info_t rx_pll_info;

    bf_tof2_serdes_pll_info_get(
        dev_id, dev_port, ln, &tx_pll_info, &rx_pll_info);
    aim_printf(&serdes_uc->pvs,
               "\n#| %2d  |  %d  |   %3d  | %d  |  %f | %f, %d |%d |   %d  |   "
               "%d  |%f |    %d    | %08xL|  %f | %f, %d |%d |   %d  |   %d  "
               "|%f |    %d    | %08xL|",
               fp,
               dev_id,
               dev_port,
               ln,
               tx_pll_info.data_rate,
               tx_pll_info.fvco,
               tx_pll_info.pll_cap,
               (int)tx_pll_info.pll_n_float,
               tx_pll_info.div4_en,
               tx_pll_info.div2_bypass,
               tx_pll_info.ref_clk,
               tx_pll_info.pll_frac_en,
               tx_pll_info.pll_frac_n,
               rx_pll_info.data_rate,
               rx_pll_info.fvco,
               rx_pll_info.pll_cap,
               (int)rx_pll_info.pll_n_float,
               rx_pll_info.div4_en,
               rx_pll_info.div2_bypass,
               rx_pll_info.ref_clk,
               rx_pll_info.pll_frac_en,
               rx_pll_info.pll_frac_n);
  }
  aim_printf(&serdes_uc->pvs, "\n");
}

/*typedef struct pll_info_t {
  float data_rate;
  float fvco;
  uint32_t pll_cap;
  float pll_n_float;
  uint32_t div4_en;
  uint32_t div2_bypass;
  float ref_clk;
  uint32_t pll_frac_en;
  uint32_t pll_frac_n;
} pll_info_t;
*/

// tof3 serdes display
extern void bf_pm_ucli_tof3_serdes_state_banner(ucli_context_t *uc);
extern void tof3_ucli_serdes_state_display(uint32_t fp,
                                           uint32_t chnl,
                                           bf_dev_port_t dev_port,
                                           uint32_t ln,
                                           ucli_context_t *uc);

void bf_pm_ucli_tof3_serdes_state_display(uint32_t fp,
                                          uint32_t chnl,
                                          bf_dev_port_t dev_port,
                                          ucli_context_t *uc) {
  uint32_t num_lanes;
  uint32_t ln;

  if (chnl == 0xffffffff) {
    chnl = 0;
  }
  bf_port_num_lanes_get(0, dev_port, (int *)&num_lanes);

  bf_pm_ucli_tof3_serdes_state_banner(uc);
  for (ln = 0; ln < num_lanes; ln++) {
    tof3_ucli_serdes_state_display(fp, chnl, dev_port, ln, uc);
  }
  aim_printf(&uc->pvs, "\n");
}

// tof2 serdes display
void bf_pm_ucli_tof2_serdes_state_display(uint32_t fp,
                                          uint32_t chnl,
                                          bf_dev_port_t dev_port,
                                          ucli_context_t *uc) {
  uint32_t num_lanes;
  uint32_t ln;

  if (chnl == 0xffffffff) {
    chnl = 0;
  }
  serdes_uc = uc;

  tof2_serdes_fw_info(fp, dev_port);
  tof2_serdes_config(fp, dev_port);
  fw_serdes_params(fp, dev_port);
  tof2_pll_display(fp, dev_port);

  bf_port_num_lanes_get(0, dev_port, (int *)&num_lanes);

  for (ln = 0; ln < num_lanes; ln++) {
    rx_monitor(fp, chnl + ln, dev_port, ln, true, false, false);  // rst
  }
  bf_sys_usleep(100000);
  for (ln = 0; ln < num_lanes; ln++) {
    rx_monitor(fp, chnl + ln, dev_port, ln, false, false, true);  // capture
  }
  // dumps all logical lanes in port
  rx_monitor(fp, chnl, dev_port, 0, false, true, false);  // print
  (void)dev_port;
}

static void pm_ucli_tof2_pcs_banner(ucli_context_t *uc) {
  aim_printf(&uc->pvs,
             "-----------------------------------------------------------------"
             "----------------------------+\n");

  aim_printf(&uc->pvs,
             "PORT |MAC "
             "|D_P|P/PT"
             "|Tx |Rx |Sig|Blk|AM |AL |HI |LCL|RMT|LNK|HI |FEC|Rx | Last "
             "up/down time\n");
  aim_printf(&uc->pvs,
             "     |    |   |    |"
             "Clk|Clk|Ok |Lk |Lk |GN |BER|FLT|FLT|   |SER|DEG|AMF| \n");

  aim_printf(&uc->pvs,
             "-----+----+---+----+---+---+---+---+---+---+---+---+"
             "---+---+---+---+---+\n");
}

extern void port_mgr_tof2_umac4_pcslcfg_get(bf_dev_id_t dev_id,
                                            uint32_t umac4,
                                            uint32_t vl,
                                            uint64_t *amlock,
                                            uint64_t *blocklock,
                                            uint64_t *mapping,
                                            uint64_t *amperiod);
#if 1
void get_vl_info(bf_dev_id_t dev_id,
                 bf_dev_port_t dev_port,
                 uint32_t ch,
                 int *vl_start,
                 int *vl_end,
                 int *vl_consecutive,
                 int *vl_stride) {
  bf_fec_type_t fec;
  bf_port_speed_t speed;
  int num_lanes;

  bf_port_speed_get(dev_id, dev_port, &speed);
  bf_port_fec_get(dev_id, dev_port, &fec);
  bf_port_num_lanes_get(0, dev_port, (int *)&num_lanes);

  if ((speed == BF_SPEED_100G) && (fec == BF_FEC_TYP_RS)) {
    *vl_start = (ch * 10);
    *vl_end = (ch * 10) + (20);
    *vl_stride = 5;
    *vl_consecutive = 1;
  } else if ((speed == BF_SPEED_100G) && (fec == BF_FEC_TYP_NONE)) {
    *vl_start = (ch * 10);
    *vl_end = (ch * 10) + (20);
    *vl_stride = 1;
    *vl_consecutive = 1;
  }
  switch (speed) {
    case BF_SPEED_10G:
    case BF_SPEED_25G:
      *vl_start = (ch * 10);
      *vl_end = (ch * 10) + (1);
      *vl_stride = 1;
      *vl_consecutive = 1;
      break;
    case BF_SPEED_40G_R2:
    case BF_SPEED_50G:
    case BF_SPEED_50G_CONS:
      if (num_lanes == 2) {
        if ((fec == BF_FEC_TYP_NONE) || (fec == BF_FEC_TYP_FC)) {
          *vl_start = (ch * 10);
          *vl_end = (ch * 10) + (2 * 5);
          *vl_stride = 5;
          *vl_consecutive = 2;
        } else if (fec == BF_FEC_TYP_RS) {
          *vl_start = (ch * 10);
          *vl_end = (ch * 10) + (2 * 5);
          *vl_stride = 5;
          *vl_consecutive = 1;
        }
      } else {
        *vl_start = (ch * 10);
        *vl_end = (ch * 10) + (2 * 5);
        *vl_stride = 5;
        *vl_consecutive = 1;
      }
      break;
    case BF_SPEED_100G:
      if (num_lanes == 4) {
        if (fec == BF_FEC_TYP_NONE) {
          *vl_start = (ch * 10);
          *vl_end = (ch * 10) + (4 * 5);
          *vl_stride = 20;
          *vl_consecutive = 20;
        } else if (fec == BF_FEC_TYP_RS) {
          *vl_start = (ch * 10);
          *vl_end = (ch * 10) + (4 * 5);
          *vl_stride = 5;
          *vl_consecutive = 1;
        }
      } else {
        *vl_start = (ch * 10);
        *vl_end = (ch * 10) + (4 * 5);
        *vl_stride = 5;
        *vl_consecutive = 1;
      }
      break;
    case BF_SPEED_200G:
      if (num_lanes == 4) {
        *vl_start = (ch * 10);
        *vl_end = (ch * 10) + (8 * 5);
        *vl_stride = 5;
        *vl_consecutive = 1;
      } else if (num_lanes == 8) {
        *vl_start = (ch * 10);
        *vl_end = (ch * 10) + (8 * 5);
        *vl_stride = 5;
        *vl_consecutive = 1;
      }
      break;
    case BF_SPEED_400G:
      *vl_start = (ch * 10);
      *vl_end = (ch * 10) + (16 * 5);
      *vl_stride = 5;
      *vl_consecutive = 1;
      break;
    default:
      break;
  }
}
#endif

static ucli_status_t bf_pm_ucli_ucli__port_vl__(ucli_context_t *uc) {
  static char usage[] = "port-vl <conn_id/chnl>";
  UCLI_COMMAND_INFO(uc, "port-vl", -1, "<port_str>");

  bf_status_t sts;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;
  char *port_str = NULL;

  if (bf_lld_dev_is_tof1(0) || bf_lld_dev_is_tof3(0)) {
    aim_printf(&uc->pvs, "Command not supported on tofino1\n");
    return 0;
  }

  if (uc->pargs->count == 0) {
    port_str = "-/-";
  } else {
    port_str = (char *)uc->pargs->args[0];
  }
  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get(port_str, &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return -1;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    uint32_t umac, ch;
    bool is_port_internal = false, is_cpu_port = false;
    bf_pm_port_info_t *port_info = NULL;

    port_hdl_ptr = &list[i];

    // once to get dev_id of port
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    /* need the port_info, so get all */
    sts = bf_pm_port_info_all_get(
        __func__, dev_id, port_hdl_ptr, &port_info, &dev_id, &dev_port);

    if (!port_info || !port_info->is_added) continue;

    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      int num_lanes = 0;
      int vl_start, vl_end, vl_consecutive, vl_stride;

      sts = port_mgr_tof2_map_dev_port_to_all(
          dev_id, dev_port, NULL, NULL, &umac, &ch, &is_cpu_port);
      if (sts != BF_SUCCESS) {
        aim_printf(&uc->pvs, "unable to map dev_port (%d) to umac\n", dev_port);
        return 0;
      }
      if (umac == 0) {  // cpu port is umac3
        aim_printf(&uc->pvs, "Not supported by umac3\n");
        return 0;
      }
      bf_port_num_lanes_get(0, dev_port, (int *)&num_lanes);

      aim_printf(&uc->pvs, "\n");
      aim_printf(&uc->pvs,
                 " Port |dev| d_p |mac | vl | amlock | blocklock | mapping | "
                 "period \n");
      aim_printf(&uc->pvs,
                 "------+---+-----+----+----+--------+-----------+---------+---"
                 "-----\n");

      get_vl_info(dev_id,
                  dev_port,
                  ch,
                  &vl_start,
                  &vl_end,
                  &vl_consecutive,
                  &vl_stride);

      for (int vl = vl_start; vl < vl_end; vl += vl_stride) {
        // for (uint32_t vl = 0; vl < 80; vl ++) {
        uint64_t amlock;
        uint64_t blocklock;
        uint64_t mapping;
        uint64_t amperiod;

        for (int c = 0; c < vl_consecutive; c++) {
          port_mgr_tof2_umac4_pcslcfg_get(
              dev_id, umac, vl + c, &amlock, &blocklock, &mapping, &amperiod);
          aim_printf(&uc->pvs,
                     "%3d/%d | %d | %3d | %2d | %2d |    %1" PRIu64
                     "   |     %1" PRIu64 "     |   %2" PRIu64 "    | %" PRIu64
                     "\n",
                     port_hdl_ptr->conn_id,
                     port_hdl_ptr->chnl_id,
                     dev_id,
                     dev_port,
                     umac,
                     vl + c,
                     amlock,
                     blocklock,
                     mapping,
                     amperiod);
        }
      }
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

extern int port_mgr_dump_this_pcs_counters(ucli_context_t *uc,
                                           bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port);
extern void print_pcs_ctrs_banner(ucli_context_t *uc);
int port_mgr_dump_this_pcs_status(ucli_context_t *uc,
                                  bf_dev_id_t dev_id,
                                  bf_dev_port_t dev_port);
void dump_oper_banner(ucli_context_t *uc);

static ucli_status_t bf_pm_ucli_ucli__port_pcs__(ucli_context_t *uc) {
  static char usage[] = "port-pcs <conn_id/chnl>";
  UCLI_COMMAND_INFO(uc, "port-pcs", -1, "<port_str>");

  bf_status_t sts;
  uint32_t print_count = 0;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;
  char *port_str = NULL;

  if (uc->pargs->count == 0) {
    port_str = "-/-";
  } else {
    port_str = (char *)uc->pargs->args[0];
  }
  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get(port_str, &list_sz, &list);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return -1;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    bool is_port_internal = false;
    bf_pm_port_info_t *port_info = NULL;

    port_hdl_ptr = &list[i];

    // once to get dev_id of port
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    /* need the port_info, so get all */
    sts = bf_pm_port_info_all_get(
        __func__, dev_id, port_hdl_ptr, &port_info, &dev_id, &dev_port);

    if (!port_info || !port_info->is_added) continue;

    if (bf_lld_dev_is_tof3(dev_id)) {
      aim_printf(&uc->pvs,
                 "Command not supported on this device %d port %d:%d\n",
                 dev_id,
                 port_hdl_ptr->conn_id,
                 port_hdl_ptr->chnl_id);
      continue;
    }
    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      // print the banner if necessary
      if (print_count == 0) {
        if (bf_lld_dev_is_tof1(dev_id)) {
          dump_oper_banner(uc);
        } else {
          pm_ucli_tof2_pcs_banner(uc);
        }
      }

      if (bf_lld_dev_is_tof1(dev_id)) {
        int line_output = port_mgr_dump_this_pcs_status(uc, dev_id, dev_port);
        if (line_output) {
          print_count++;
          if (print_count >= 32) {
            print_count = 0;
          }
        } else if (print_count == 0) {
          print_count = 1;
        }
        continue;
      }
      uint64_t reg64;
      uint64_t txclkpresentall;
      uint64_t rxclkpresentall;
      uint64_t rxsigokall;
      uint64_t blocklockall;
      uint64_t amlockall;
      uint64_t aligned;
      uint64_t nohiber;
      uint64_t nolocalfault;
      uint64_t noremotefault;
      uint64_t linkup;
      uint64_t hiser;
      uint64_t fecdegser;
      uint64_t rxamsf;
      bf_status_t rc;

      log_pipe = DEV_PORT_TO_PIPE(dev_port);
      if (lld_sku_map_pipe_id_to_phy_pipe_id(dev_id, log_pipe, &phy_pipe) !=
          0) {
        PM_ERROR(
            "Unable to get phy pipe id from log pipe id for dev %d : log pipe "
            "id "
            ": %d",
            dev_id,
            log_pipe);
        continue;
      }

      rc = bf_port_umac4_status_get(dev_id,
                                    dev_port,
                                    &reg64,
                                    &txclkpresentall,
                                    &rxclkpresentall,
                                    &rxsigokall,
                                    &blocklockall,
                                    &amlockall,
                                    &aligned,
                                    &nohiber,
                                    &nolocalfault,
                                    &noremotefault,
                                    &linkup,
                                    &hiser,
                                    &fecdegser,
                                    &rxamsf);
      if (rc != BF_SUCCESS) {
        continue;
      }
      aim_printf(&uc->pvs,
                 "%-5s|%2" PRIu64 "/%1" PRIu64 "|%3" PRIu64 "|%1" PRIu64
                 "/%2" PRIu64,
                 port_info->pltfm_port_info.port_str,
                 (uint64_t)port_info->pltfm_port_info.log_mac_id,
                 (uint64_t)port_info->pltfm_port_info.log_mac_lane,
                 (uint64_t)dev_port,
                 (uint64_t)phy_pipe,
                 (uint64_t)DEV_PORT_TO_LOCAL_PORT(port_info->dev_port));
      aim_printf(&uc->pvs,
                 "|%2" PRIu64 " |%2" PRIu64 " |%2" PRIu64 " |%2" PRIu64
                 " |%2" PRIu64 " |%2" PRIu64 " |%2d |%2d |%2d |%2" PRIu64
                 " |%2" PRIu64 " |%2" PRIu64 " |%2" PRIu64 " | ",
                 txclkpresentall,
                 rxclkpresentall,
                 rxsigokall,
                 blocklockall,
                 amlockall,
                 aligned,
                 nohiber ? 0 : 1,
                 nolocalfault ? 0 : 1,
                 noremotefault ? 0 : 1,
                 linkup,
                 hiser,
                 fecdegser,
                 rxamsf);
      port_mgr_dump_uptime(uc, dev_id, dev_port);
      aim_printf(&uc->pvs, "\n");

      print_count++;
      if (print_count >= 32) {
        print_count = 0;
      }
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

static void pm_ucli_umac4_int_banner(ucli_context_t *uc) {
  aim_printf(&uc->pvs,
             "-----------------------------------------------------------------"
             "--------------------------------------+\n");
  aim_printf(&uc->pvs,
             "PORT |MAC "
             "|D_P|P/"
             "PT|TFIFO|PROTE|TXJAB|SFIFO|GBOX|DSKW|HIBER|PCSE|RFLT|DROP|FCSE|"
             "RJAB|RUNT|RFIFO|DOWN|UP|\n");
  aim_printf(&uc->pvs,
             "-----+----+---+----+-----+-----+-----+-----+----+----+-----+----+"
             "----+----+----+----+----+-----+----+--+\n");
}

static void pm_ucli_tof2_umac4_int(ucli_context_t *uc,
                                   bf_dev_id_t dev_id,
                                   int aflag,
                                   bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  uint32_t print_count = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  pm_ucli_umac4_int_banner(uc);

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
    if (next_port_info_ptr->is_added || aflag) {
      uint64_t reg64;
      bf_status_t rc;

      rc = bf_port_umac4_interrupt_get(
          dev_id, next_port_info_ptr->dev_port, &reg64);
      if (rc != BF_SUCCESS) {
        continue;
      }
      aim_printf(
          &uc->pvs,
          "%-5s|%2" PRIu64 "/%1" PRIu64 "|%3" PRIu64 "|%1" PRIu64 "/%2" PRIu64,
          next_port_info_ptr->pltfm_port_info.port_str,
          (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_id,
          (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_lane,
          (uint64_t)next_port_info_ptr->dev_port,
          (uint64_t)phy_pipe,
          (uint64_t)DEV_PORT_TO_LOCAL_PORT(next_port_info_ptr->dev_port));
      aim_printf(&uc->pvs,
                 "| %4" PRIu64 "| %4" PRIu64 "| %4" PRIu64 "| %4" PRIu64
                 "| %3" PRIu64 "| %3" PRIu64 "| %4" PRIu64 "| %3" PRIu64
                 "| %3" PRIu64 "| %3" PRIu64 "| %3" PRIu64 "| %3" PRIu64
                 "| %3" PRIu64 "| %4" PRIu64 "| %3" PRIu64 "| %1" PRIu64 "\n",
                 (reg64 >> 0ull) & 1,
                 (reg64 >> 1ull) & 1,
                 (reg64 >> 2ull) & 1,
                 (reg64 >> 3ull) & 1,
                 (reg64 >> 4ull) & 1,
                 (reg64 >> 5ull) & 1,
                 (reg64 >> 6ull) & 1,
                 (reg64 >> 7ull) & 1,
                 (reg64 >> 8ull) & 1,
                 (reg64 >> 9ull) & 1,
                 (reg64 >> 10ull) & 1,
                 (reg64 >> 11ull) & 1,
                 (reg64 >> 12ull) & 1,
                 (reg64 >> 13ull) & 1,
                 (reg64 >> 14ull) & 1,
                 (reg64 >> 15ull) & 1);
      print_count++;
    } else {
    }
    // print the banner
    if (print_count >= 32) {
      pm_ucli_umac4_int_banner(uc);
      print_count = 0;
    }
  }
}

static ucli_status_t bf_pm_ucli_ucli__port_int__(ucli_context_t *uc) {
  static char usage[] = "port-int -a <conn_id/chnl>";
  UCLI_COMMAND_INFO(uc, "port-int", -1, "-a <port_str>");

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
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
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

  pm_ucli_tof2_umac4_int(uc, dev_id, aflag, &port_hdl);
  return 0;
}

static void pm_ucli_tof2_port_history(ucli_context_t *uc,
                                      bf_dev_id_t dev_id,
                                      int aflag,
                                      bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  uint32_t print_count = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      int num_lanes;
      int snapshot;
      uint32_t history_next;

      bf_port_num_lanes_get(0, next_port_info_ptr->dev_port, (int *)&num_lanes);
      history_next = next_port_info_ptr->history_next;

      for (snapshot = 0; snapshot < 8; snapshot++) {
        info_t *info;
        int ln;
        uint64_t reg64;
        struct timeval *tm;
        char tbuf[256] = {0};
        char ubuf[256] = {0};
        struct tm *loctime;

        history_next = (history_next + 1) % 8;

        info = &next_port_info_ptr->history[history_next];
        if ((info->tv.tv_sec == 0) && (info->tv.tv_usec == 0)) continue;
        tm = &info->tv;

        loctime = localtime(&tm->tv_sec);
        if (loctime == NULL) {
          aim_printf(&uc->pvs, "localtime returned null \n");
          continue;
        }
        strftime(tbuf, sizeof(tbuf), "%a %b %d", loctime);
        aim_printf(&uc->pvs, "\n%s ", tbuf);

        strftime(ubuf, sizeof(ubuf), "%T\n", loctime);
        ubuf[strlen(ubuf) - 1] = 0;  // remove CR
        aim_printf(&uc->pvs, "\n%s.%06d : ", ubuf, (int)tm->tv_usec);

        aim_printf(&uc->pvs,
                   "%s : %s\n",
                   next_port_info_ptr->pltfm_port_info.port_str,
                   info->st ? "UP" : "DN");

        pm_ucli_umac4_int_banner(uc);
        reg64 = info->int_reg;
        aim_printf(
            &uc->pvs,
            "%-5s|%2" PRIu64 "/%1" PRIu64 "|%3" PRIu64 "|%1" PRIu64
            "/%2" PRIu64,
            next_port_info_ptr->pltfm_port_info.port_str,
            (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_id,
            (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_lane,
            (uint64_t)next_port_info_ptr->dev_port,
            (uint64_t)phy_pipe,
            (uint64_t)DEV_PORT_TO_LOCAL_PORT(next_port_info_ptr->dev_port));
        aim_printf(&uc->pvs,
                   "| %4" PRIu64 "| %4" PRIu64 "| %4" PRIu64 "| %4" PRIu64
                   "| %3" PRIu64 "| %3" PRIu64 "| %4" PRIu64 "| %3" PRIu64
                   "| %3" PRIu64 "| %3" PRIu64 "| %3" PRIu64 "| %3" PRIu64
                   "| %3" PRIu64 "| %4" PRIu64 "| %3" PRIu64 "| %1" PRIu64 "\n",
                   (reg64 >> 0ull) & 1,
                   (reg64 >> 1ull) & 1,
                   (reg64 >> 2ull) & 1,
                   (reg64 >> 3ull) & 1,
                   (reg64 >> 4ull) & 1,
                   (reg64 >> 5ull) & 1,
                   (reg64 >> 6ull) & 1,
                   (reg64 >> 7ull) & 1,
                   (reg64 >> 8ull) & 1,
                   (reg64 >> 9ull) & 1,
                   (reg64 >> 10ull) & 1,
                   (reg64 >> 11ull) & 1,
                   (reg64 >> 12ull) & 1,
                   (reg64 >> 13ull) & 1,
                   (reg64 >> 14ull) & 1,
                   (reg64 >> 15ull) & 1);

        pm_ucli_tof2_pcs_banner(uc);
        aim_printf(
            &uc->pvs,
            "%-5s|%2" PRIu64 "/%1" PRIu64 "|%3" PRIu64 "|%1" PRIu64
            "/%2" PRIu64,
            next_port_info_ptr->pltfm_port_info.port_str,
            (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_id,
            (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_lane,
            (uint64_t)next_port_info_ptr->dev_port,
            (uint64_t)phy_pipe,
            (uint64_t)DEV_PORT_TO_LOCAL_PORT(next_port_info_ptr->dev_port));
        aim_printf(&uc->pvs,
                   "| %4" PRIu64 "| %4" PRIu64 "| %4" PRIu64 "| %4" PRIu64
                   "| %3" PRIu64 "| %3" PRIu64 "| %4" PRIu64 "| %3" PRIu64
                   "| %3" PRIu64 "| %3" PRIu64 "| %4" PRIu64 "| %4" PRIu64
                   "| %5" PRIu64 "\n",
                   info->pcs.txclkpresentall,
                   info->pcs.rxclkpresentall,
                   info->pcs.rxsigokall,
                   info->pcs.blocklockall,
                   info->pcs.amlockall,
                   info->pcs.aligned,
                   info->pcs.nohiber,
                   info->pcs.nolocalfault,
                   info->pcs.noremotefault,
                   info->pcs.linkup,
                   info->pcs.hiser,
                   info->pcs.fecdegser,
                   info->pcs.rxamsf);

        if (info->st == 0) {  // no serdes info on down
          continue;
        }

        fw_serdes_params_banner(uc);

        for (ln = 0; ln < num_lanes; ln++) {
          fw_serdes_info_display(
              uc, dev_id, next_port_info_ptr->dev_port, ln, info);
        }
        fw_serdes_params_separator(uc);
      }
      print_count++;
    } else {
    }
    // print the banner
    if (print_count >= 32) {
      print_count = 0;
    }
  }
  (void)aflag;
}

static ucli_status_t bf_pm_ucli_ucli__port_hist__(ucli_context_t *uc) {
  static char usage[] = "port-hist -a <conn_id/chnl>";
  UCLI_COMMAND_INFO(uc, "port-hist", 1, "-a <port_str>");

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
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }
  if (argv[1][0] == '-' && argv[1][1] == 'a') {
    aflag = 1;
  } else {
    sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], &port_hdl);
    if (sts != BF_SUCCESS) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
  }

  // conn==-1 and chnl==-1 indicates that scan through all the ports but only
  // print the ports that have been added.
  //-a option indicates that the scan and print all the ports (whether added or
  // not)

  pm_ucli_tof2_port_history(uc, dev_id, aflag, &port_hdl);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_fec_set__(ucli_context_t *uc) {
  static char usage[] = "port-fec-set <port_str> <NONE | FC | RS>";
  UCLI_COMMAND_INFO(uc, "port-fec-set", 2, "<port_str> <NONE | FC | RS>");
  bf_status_t sts;
  uint32_t chnl_begin, chnl_end;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr, *next_port_hdl_ptr;
  bf_dev_id_t dev_id;
  bf_dev_id_t dev_id_of_port = 0;
  uint32_t j;
  bf_dev_port_t dev_port;
  char lower_case_fec[5];
  bf_fec_type_t fec;
  int len;
  bf_pm_port_info_t *port_info;

  port_hdl_ptr = &port_hdl;
  next_port_hdl_ptr = &next_port_hdl;

  dev_id = 0;

  sts = bf_pm_port_str_to_hdl_get(dev_id, uc->pargs->args[0], port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    goto end;
  }

  // convert fec arg to lower-case
  len = strlen(uc->pargs->args[1]);
  if (len > 4) {
    sts = BF_INVALID_ARG;
    goto end;
  }
  for (j = 0; j < (uint32_t)len; j++) {
    lower_case_fec[j] = tolower(uc->pargs->args[1][j]);
  }
  lower_case_fec[len] = 0;  // terminate

  if (strncmp(lower_case_fec, "rs", len) == 0) {
    fec = BF_FEC_TYP_REED_SOLOMON;
  } else if (strncmp(lower_case_fec, "fc", len) == 0) {
    fec = BF_FEC_TYP_FIRECODE;
  } else if (strncmp(lower_case_fec, "none", len) == 0) {
    fec = BF_FEC_TYP_NONE;
  } else {
    sts = BF_INVALID_ARG;
    goto end;
  }

  if (port_hdl_ptr->conn_id == (uint32_t)-1) {
    // Get the first port in the system
    sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
    if (sts != BF_SUCCESS || !port_hdl_ptr) {
      goto end;
    }

    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id_of_port, &dev_port);
    if (sts != BF_SUCCESS) {
      sts = BF_INVALID_ARG;
      goto end;
    }
    dev_id = dev_id_of_port;

    port_info = pm_port_info_get(dev_id, dev_port);
    if (!port_info) {
      sts = BF_INVALID_ARG;
      goto end;
    }

    if (port_info->admin_state == PM_PORT_ENABLED) {
      aim_printf(&uc->pvs,
                 "Port must be disabled before changing configuration\n");
      sts = BF_INVALID_ARG;
      goto end;
    }

    sts = bf_pm_port_fec_set(dev_id, port_hdl_ptr, fec);
    if (sts != BF_SUCCESS) {
      sts = BF_INVALID_ARG;
      goto end;
    }

    while (sts == BF_SUCCESS) {
      // Get the next port in the system
      sts = bf_pm_port_front_panel_port_get_next(
          dev_id, port_hdl_ptr, next_port_hdl_ptr);
      if (sts == BF_OBJECT_NOT_FOUND) break;
      if (sts != BF_SUCCESS) {
        goto end;
      }

      sts = bf_pm_port_front_panel_port_to_dev_port_get(
          port_hdl_ptr, &dev_id_of_port, &dev_port);
      if (sts != BF_SUCCESS) {
        sts = BF_INVALID_ARG;
        goto end;
      }
      dev_id = dev_id_of_port;
      port_info = pm_port_info_get(dev_id, dev_port);
      if (!port_info) {
        sts = BF_INVALID_ARG;
        goto end;
      }

      if (port_info->admin_state == PM_PORT_ENABLED) {
        aim_printf(&uc->pvs,
                   "Port must be disabled before changing configuration\n");
        sts = BF_INVALID_ARG;
        goto end;
      }

      sts = bf_pm_port_fec_set(dev_id, port_hdl_ptr, fec);
      if (sts != BF_SUCCESS) {
        sts = BF_INVALID_ARG;
        goto end;
      }
      // Make the curr port hdl equal to the next port hdl
      port_hdl_ptr = next_port_hdl_ptr;
    }
  } else {
    if (port_hdl_ptr->chnl_id == (uint32_t)-1) {
      chnl_begin = 0;
      port_hdl_ptr->chnl_id = 0;
      if (BF_SUCCESS == bf_pm_port_front_panel_port_to_dev_port_get(
                            port_hdl_ptr, &dev_id_of_port, &dev_port)) {
        dev_id = dev_id_of_port;
        chnl_end = lld_get_chnls_dev_port(dev_id, dev_port) - 1;
      } else {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
    } else {
      chnl_begin = port_hdl_ptr->chnl_id;
      chnl_end = chnl_begin;
    }
    for (j = chnl_begin; j <= chnl_end; j++) {
      port_hdl_ptr->chnl_id = j;
      sts = bf_pm_port_front_panel_port_to_dev_port_get(
          port_hdl_ptr, &dev_id_of_port, &dev_port);
      if (sts != BF_SUCCESS) {
        sts = BF_INVALID_ARG;
        goto end;
      }
      dev_id = dev_id_of_port;

      port_info = pm_port_info_get(dev_id, dev_port);
      if (!port_info) {
        sts = BF_INVALID_ARG;
        goto end;
      }

      if (port_info->admin_state == PM_PORT_ENABLED) {
        aim_printf(&uc->pvs,
                   "Port must be disabled before changing configuration\n");
        sts = BF_INVALID_ARG;
        goto end;
      }

      sts = bf_pm_port_fec_set(dev_id, port_hdl_ptr, fec);
      if (sts != BF_SUCCESS) {
        sts = BF_INVALID_ARG;
        goto end;
      }
    }
  }
  return 0;
end:
  aim_printf(&uc->pvs, "Usage %s\n", usage);
  return 0;
}

extern void pm_port_fsm_display_info_get(bf_dev_id_t dev_id,
                                         bf_dev_port_t dev_port,
                                         char **fsm_str,
                                         char **fsm_st_str);

static void pm_ucli_fsm_info_banner(ucli_context_t *uc) {
  aim_printf(&uc->pvs,
             "-----------------------------------------------------------------"
             "--------------+---------------------+----------------------------"
             "+\n");
  aim_printf(&uc->pvs,
             "PORT |MAC |D_P|P/PT|RXRDY|TXRDY|DFERDY|PRBS  |LPBK MODE|ANPOL|AN "
             "EL|BRNGUP|DIR |         FSM         | FSM state                  "
             "|\n");
  aim_printf(&uc->pvs,
             "-----+----+---+----+-----+-----+------+------+---------+-----+---"
             "--+------+----+---------------------+----------------------------"
             "+\n");
}

static void pm_ucli_tof2_fsm_info(ucli_context_t *uc,
                                  bf_dev_id_t dev_id,
                                  int aflag,
                                  bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  uint32_t print_count = 0;
  int ret;
  char display_rdy[4];
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  pm_ucli_fsm_info_banner(uc);

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      // if (next_port_info_ptr->pltfm_port_info.log_mac_id == 0) {
      //  continue;
      //}
      char *lpbk_str = "invalid", *prbs_str = "invld";
      char *fsm_str, *fsm_st_str;

      bf_port_loopback_mode_to_str(next_port_info_ptr->lpbk_mode, &lpbk_str);
      bf_port_prbs_mode_to_str(next_port_info_ptr->prbs_mode, &prbs_str);
      pm_port_fsm_display_info_get(
          dev_id, next_port_info_ptr->dev_port, &fsm_str, &fsm_st_str);
      pm_ucli_rdy_to_display_get(
          next_port_info_ptr, dev_id, display_rdy, sizeof(display_rdy));
      aim_printf(
          &uc->pvs,
          "%-5s|%2" PRIu64 "/%1" PRIu64 "|%3" PRIu64 "|%1" PRIu64 "/%2" PRIu64,
          next_port_info_ptr->pltfm_port_info.port_str,
          (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_id,
          (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_lane,
          (uint64_t)next_port_info_ptr->dev_port,
          (uint64_t)phy_pipe,
          (uint64_t)DEV_PORT_TO_LOCAL_PORT(next_port_info_ptr->dev_port));
      aim_printf(
          &uc->pvs,
          "| %3s | %3s | %4s |%6s|%9s| %3s | %3s | %4s | %2d |%-21s| %s\n",
          next_port_info_ptr->serdes_rx_ready ? "Y" : "N",
          next_port_info_ptr->serdes_tx_ready ? "Y" : "N",
          next_port_info_ptr->serdes_rx_ready_for_dfe ? "Y" : "N",
          prbs_str,
          lpbk_str,
          next_port_info_ptr->an_policy == 0
              ? "DEF"
              : next_port_info_ptr->an_policy == 1
                    ? "ON "
                    : next_port_info_ptr->an_policy == 2 ? "OFF" : "BUG",
          next_port_info_ptr->is_an_eligible ? "Y" : "N",
          display_rdy,
          next_port_info_ptr->port_dir,
          fsm_str,
          fsm_st_str);

      print_count++;
    } else {
    }
    // print the banner
    if (print_count >= 32) {
      pm_ucli_fsm_info_banner(uc);
      print_count = 0;
    }
  }
  (void)aflag;
}

static ucli_status_t bf_pm_ucli_ucli__fsm_info__(ucli_context_t *uc) {
  static char usage[] = "fsm-info -a <conn_id/chnl>";
  UCLI_COMMAND_INFO(uc, "fsm-info", -1, "-a <port_str>");

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
  if (bf_lld_dev_is_tof1(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
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

  pm_ucli_tof2_fsm_info(uc, dev_id, aflag, &port_hdl);
  return 0;
}

char ber_graph[140];
char *pm_ber_graph(float ber) {
  int lines, line_len;
  char line_end[3] = {0};

  if (ber >= 0.0001) {
    ber_graph[0] = 0;
    return ber_graph;
  }
  if (ber >= 0.00001) {  // > 1e-04
    line_len = (uint32_t)(ber * 100000);
    lines = 0;
  } else if (ber >= 0.000001) {  // > 1e-05
    line_len = (uint32_t)(ber * 1000000);
    lines = 1;
  } else if (ber >= 0.0000001) {  // > 1e-06
    line_len = (uint32_t)(ber * 10000000);
    lines = 2;
  } else if (ber >= 0.00000001) {  // > 1e-07
    line_len = (uint32_t)(ber * 100000000);
    lines = 3;
  } else if (ber >= 0.000000001) {  // > 1e-08
    line_len = (uint32_t)(ber * 1000000000);
    lines = 4;
  } else if (ber >= 0.0000000001) {  // > 1e-09
    line_len = (uint32_t)(ber * 10000000000);
    lines = 5;
  } else if (ber >= 0.00000000001) {  // > 1e-10
    line_len = (uint32_t)(ber * 100000000000);
    lines = 6;
    //} else if (ber >= 0.000000000001) { // > 1e-11
    //  line_len = (uint32_t)(ber * 1000000000000);
    //  lines = 7;
    //} else if (ber >= 0.0000000000001) { // > 1e-12
    //  line_len = (uint32_t)(ber * 10000000000000);
    //  lines = 8;
  } else {
    lines = 7;
    line_len = 9;
    line_end[0] = '.';
    line_end[1] = '.';
    line_end[2] = 0;
  }
  int g = 0;
  for (int i = 0; i < lines; i++) {
    for (int j = 0; j < 9; j++) {
      ber_graph[g++] = '-';
    }
    ber_graph[g++] = '+';
  }
  for (int k = 0; k < (10 - line_len); k++) {
    ber_graph[g++] = '-';
  }
  ber_graph[g++] = line_end[0];
  ber_graph[g++] = line_end[1];
  ber_graph[g++] = line_end[2];
  return ber_graph;
}

static void pm_ucli_prbs_show_banner(ucli_context_t *uc) {
  aim_printf(
      &uc->pvs,
      "-----------------------------------------------------------------"
      "------------------------------------------+--------------+----------"
      "+"
      " ---------+---------+---------+---------+---------+---------+---------+-"
      "..\n");
  aim_printf(
      &uc->pvs,
      "PORT |MAC |D_P|P/PT|PRBS  |    0     |    1     |    2     |    3     |"
      "    4     |    5     |    6     |    7     | Total Errors |   BER    |"
      "        1e-05     1e-06     1e-07     1e-08     1e-09     1e-10      "
      "1e-11"
      "\n");
  aim_printf(
      &uc->pvs,
      "-----+----+---+----+----------+----------+----------+----------+"
      "----------+----------+----------+----------+--------------+----------"
      "+"
      " ---------+---------+---------+---------+---------+---------+---------+-"
      "..\n");
}

static void pm_ucli_tof2_prbs_show(ucli_context_t *uc,
                                   bf_dev_id_t dev_id,
                                   uint32_t integration_ms,
                                   bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  uint32_t print_count = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  aim_printf(&uc->pvs, "Integration time: %dms\n", integration_ms);

  pm_ucli_prbs_show_banner(uc);

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      // if (next_port_info_ptr->pltfm_port_info.log_mac_id == 0) {
      //  continue;
      //}
      uint32_t err_cnt = 0, err_sum = 0;
      int num_lanes, ln;

      bf_port_num_lanes_get(0, next_port_info_ptr->dev_port, &num_lanes);
      aim_printf(&uc->pvs,
                 "%-5s|%2" PRIu64 "/%1" PRIu64 "|%3" PRIu64 "|%1" PRIu64
                 "/%2" PRIu64 "|%-5s",
                 next_port_info_ptr->pltfm_port_info.port_str,
                 (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_id,
                 (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_lane,
                 (uint64_t)next_port_info_ptr->dev_port,
                 (uint64_t)phy_pipe,
                 (uint64_t)DEV_PORT_TO_LOCAL_PORT(next_port_info_ptr->dev_port),
                 pm_ucli_prbs_mode_str_get(next_port_info_ptr->prbs_mode));
      // for (ln = 7; ln >= 0; ln--) {
      for (ln = 0; ln < 8; ln++) {
        if (ln >= num_lanes) {
          aim_printf(&uc->pvs, "|          ");
        } else {
          if (integration_ms != 0) {
            if (bf_lld_dev_is_tof2(dev_id)) {
              bf_tof2_serdes_prbs_rst_set(
                  dev_id, next_port_info_ptr->dev_port, ln);
            } else if (bf_lld_dev_is_tof3(dev_id)) {
              bf_tof3_serdes_prbs_rst_set(
                  dev_id, next_port_info_ptr->dev_port, ln);
            }
          }
          bf_sys_usleep(integration_ms * 1000);

          if (bf_lld_dev_is_tof2(dev_id)) {
            bf_tof2_serdes_rx_prbs_err_get(
                0, next_port_info_ptr->dev_port, ln, &err_cnt);
          } else if (bf_lld_dev_is_tof3(dev_id)) {
            bf_tof3_serdes_rx_prbs_err_get(
                0, next_port_info_ptr->dev_port, ln, &err_cnt);
          }
          if (bf_lld_dev_is_tof3(dev_id)) {
            uint32_t cdr_lock;
            bf_tof3_serdes_cdr_lock_get(
                dev_id, next_port_info_ptr->dev_port, ln, &cdr_lock);
            if (!cdr_lock) {
              aim_printf(&uc->pvs, "|      ----");
              err_cnt = 0xffffffff;
            } else {
              aim_printf(&uc->pvs, "|%10d", err_cnt);
            }
          } else {
            aim_printf(&uc->pvs, "|%10d", err_cnt);
          }
          err_sum += err_cnt;

          if (integration_ms == 0) {
            if (bf_lld_dev_is_tof2(dev_id)) {
              bf_tof2_serdes_prbs_rst_set(
                  dev_id, next_port_info_ptr->dev_port, ln);
            } else if (bf_lld_dev_is_tof3(dev_id)) {
              bf_tof3_serdes_prbs_rst_set(
                  dev_id, next_port_info_ptr->dev_port, ln);
            }
          }
        }
      }
      uint64_t bps;
      float ber;
      bf_port_speed_t speed;

      bf_port_speed_get(dev_id, next_port_info_ptr->dev_port, &speed);

      /* get ports aggregate bit-rate */
      bps = pm_util_aggregate_bit_rate_get(speed, num_lanes);

      bps = (bps * integration_ms / 1000);
      ber = (float)((float)err_sum / (float)bps);
      aim_printf(
          &uc->pvs, "| %12d | %8.1e | %s\n", err_sum, ber, pm_ber_graph(ber));
      print_count++;
    }
    // print the banner
    if (print_count >= 32) {
      pm_ucli_prbs_show_banner(uc);
      print_count = 0;
    }
  }
}

static ucli_status_t bf_pm_ucli_ucli__prbs_show__(ucli_context_t *uc) {
  static char usage[] = "prbs-show <conn_id/chnl> [integration-time-ms]";
  UCLI_COMMAND_INFO(uc, "prbs-show", -1, "<port_str> [n_ms]");

  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  bf_status_t sts;
  uint32_t integration_ms = 10;  // default 10ms

  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }
  int argc = (uc->pargs->count + 1);
  if (argc > 1) {
    if ((argv[1][1] == '/') ||
        (argv[1][2] == '/')) {  // must be 1/ or 10/ to be a port
      sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], &port_hdl);
      if (sts != BF_SUCCESS) {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
    } else {
      integration_ms = strtoul(argv[1], NULL, 10);  // seconds
      if (integration_ms > 60000) {
        integration_ms = 10;  // default to 10ms
      }
    }
  }
  if (argc > 2) {
    integration_ms = strtoul(argv[2], NULL, 10);  // seconds
    if (integration_ms > 60000) {
      integration_ms = 10;  // default to 10ms
    }
  }
  // conn==-1 and chnl==-1 indicates that scan through all the ports but only
  // print the ports that have been added.
  //-a option indicates that the scan and print all the ports (whether added or
  // not)

  pm_ucli_tof2_prbs_show(uc, dev_id, integration_ms, &port_hdl);
  return 0;
}

static void pm_ucli_tof2_prbs_fix(ucli_context_t *uc,
                                  bf_dev_id_t dev_id,
                                  int aflag,
                                  bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  uint32_t print_count = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  pm_ucli_prbs_show_banner(uc);

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      if (next_port_info_ptr->pltfm_port_info.log_mac_id == 0) {
        continue;
      }
      uint32_t err_cnt;
      int num_lanes, ln;

      bf_port_num_lanes_get(0, next_port_info_ptr->dev_port, &num_lanes);
      aim_printf(&uc->pvs,
                 "%-5s|%2" PRIu64 "/%1" PRIu64 "|%3" PRIu64 "|%1" PRIu64
                 "/%2" PRIu64 "|%-5s",
                 next_port_info_ptr->pltfm_port_info.port_str,
                 (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_id,
                 (uint64_t)next_port_info_ptr->pltfm_port_info.log_mac_lane,
                 (uint64_t)next_port_info_ptr->dev_port,
                 (uint64_t)phy_pipe,
                 (uint64_t)DEV_PORT_TO_LOCAL_PORT(next_port_info_ptr->dev_port),
                 pm_ucli_prbs_mode_str_get(next_port_info_ptr->prbs_mode));
      for (ln = 7; ln >= 0; ln--) {
        if (ln >= num_lanes) {
          aim_printf(&uc->pvs, "         |");
        } else {
          bf_tof2_serdes_prbs_rst_set(dev_id, next_port_info_ptr->dev_port, ln);
          bf_sys_usleep(10000);  // 10ms data collection per lane
          bf_tof2_serdes_rx_prbs_err_get(
              0, next_port_info_ptr->dev_port, ln, &err_cnt);
          aim_printf(&uc->pvs, "|%10d", err_cnt);
          // if err ct too high, try inverting rx
          if (err_cnt > 1000000) {
            bool inv;
            bf_tof2_serdes_rx_polarity_hw_get(
                dev_id, next_port_info_ptr->dev_port, ln, &inv);
            bf_tof2_serdes_rx_polarity_set(
                dev_id, next_port_info_ptr->dev_port, ln, inv ? 0 : 1, true);
            PM_DEBUG("%d:%3d:%d: invert lane Rx polarity due to errors",
                     dev_id,
                     next_port_info_ptr->dev_port,
                     ln);
          }
        }
      }
      aim_printf(&uc->pvs, "\n");
      print_count++;
    }
    // print the banner
    if (print_count >= 32) {
      pm_ucli_prbs_show_banner(uc);
      print_count = 0;
    }
  }
  (void)aflag;
}

static ucli_status_t bf_pm_ucli_ucli__prbs_fix__(ucli_context_t *uc) {
  static char usage[] = "prbs-fix -a <conn_id/chnl>";
  UCLI_COMMAND_INFO(uc, "prbs-fix", -1, "-a <port_str>");

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
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
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

  pm_ucli_tof2_prbs_fix(uc, dev_id, aflag, &port_hdl);
  return 0;
}

static void pm_ucli_tof2_prbs_reset(ucli_context_t *uc,
                                    bf_dev_id_t dev_id,
                                    int aflag,
                                    bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      if (next_port_info_ptr->pltfm_port_info.log_mac_id == 0) {
        continue;
      }
      int num_lanes, ln;

      bf_port_num_lanes_get(0, next_port_info_ptr->dev_port, &num_lanes);
      for (ln = 0; ln < num_lanes; ln++) {
        bf_tof2_serdes_lane_reset_set(dev_id, next_port_info_ptr->dev_port, ln);
      }
    }
  }
  (void)aflag;
  (void)uc;
}

static ucli_status_t bf_pm_ucli_ucli__prbs_reset__(ucli_context_t *uc) {
  static char usage[] = "prbs-reset-a <conn_id/chnl>";
  UCLI_COMMAND_INFO(uc, "prbs-reset", -1, "-a <port_str>");

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
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
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

  pm_ucli_tof2_prbs_reset(uc, dev_id, aflag, &port_hdl);
  return 0;
}

static void pm_ucli_tof3_port_ber(ucli_context_t *uc,
                                  bf_dev_id_t dev_id,
                                  int integration_time,
                                  bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      // if (next_port_info_ptr->pltfm_port_info.log_mac_id == 0) {
      //  continue;
      //}
      int num_lanes;
      bf_status_t rc;
      bf_serdes_encoding_mode_t enc_mode;
      bf_port_speed_t speed;
      bf_fec_type_t fec;
      uint64_t symb_err[16];
      uint32_t tmac;
      uint32_t ch_ln;
      bool is_cpu_port;
      uint32_t ctr, n_ctrs, first_ctr, end_ctr;
      uint64_t bps, s_errs, tot_s_errs = 0ul;
      uint64_t *ctrs = (uint64_t *)&symb_err;
      size_t ctrs_sz = sizeof(symb_err) / sizeof(uint64_t);

      bf_port_fec_get(dev_id, next_port_info_ptr->dev_port, &fec);
      bf_port_speed_get(dev_id, next_port_info_ptr->dev_port, &speed);
      bf_port_num_lanes_get(0, next_port_info_ptr->dev_port, &num_lanes);
      bf_serdes_encoding_mode_get(speed, num_lanes, &enc_mode);

      rc = port_mgr_tof3_map_dev_port_to_all(dev_id,
                                             next_port_info_ptr->dev_port,
                                             NULL,
                                             NULL,
                                             &tmac,
                                             &ch_ln,
                                             &is_cpu_port);
      if (rc != BF_SUCCESS) {
        aim_printf(&uc->pvs,
                   "unable to map dev_port (%d) to tmac\n",
                   next_port_info_ptr->dev_port);
        return;
      }

      if (speed == BF_SPEED_400G) {
        n_ctrs = 16;
        first_ctr = 0;
      } else if ((speed == BF_SPEED_200G) && (num_lanes == 2)) {
        n_ctrs = 8;
        first_ctr = (next_port_info_ptr->dev_port & 0x7) * 4;
      } else if ((speed == BF_SPEED_200G) && (num_lanes == 4)) {
        n_ctrs = 8;
        first_ctr = (next_port_info_ptr->dev_port & 0x7) * 2;
      } else if ((speed == BF_SPEED_100G) && (num_lanes == 1)) {
        n_ctrs = 4;
        first_ctr = (next_port_info_ptr->dev_port & 0x7) * 2;
      } else if ((speed == BF_SPEED_100G) && (num_lanes == 2)) {
        n_ctrs = 4;
        first_ctr = (next_port_info_ptr->dev_port & 0x7) * 2;
      } else if ((speed == BF_SPEED_50G) && (num_lanes == 1)) {
        n_ctrs = 2;
        first_ctr = (next_port_info_ptr->dev_port & 0x7) * 2;
      } else {
        aim_printf(&uc->pvs,
                   "%2d/%d | %3d |  %3d  | not      |\n",
                   next_port_info_ptr->pltfm_port_info.port_hdl.conn_id,
                   next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id,
                   next_port_info_ptr->dev_port,
                   tmac);
        aim_printf(&uc->pvs, "                   | supported|\n");
        aim_printf(&uc->pvs, "------------------------------+\n");
        continue;
      }
      if (next_port_info_ptr->oper_status != PM_PORT_UP) {
        continue;
      }
      /* get ports aggregate bit-rate */
      bps = pm_util_aggregate_bit_rate_get(speed, num_lanes);
      bps = (bps * integration_time) / 1000000;

      end_ctr = (first_ctr + n_ctrs);
      if (end_ctr > ctrs_sz) {
        aim_printf(&uc->pvs,
                   "Attempt to access array of size %zu with index %d\n",
                   ctrs_sz,
                   (int)end_ctr);
        return;
      }

      // timed collections of counters
      rc = bf_port_tof3_fec_lane_symb_err_counter_get(
          dev_id, next_port_info_ptr->dev_port, n_ctrs, symb_err);
      bf_sys_usleep(integration_time);
      rc = bf_port_tof3_fec_lane_symb_err_counter_get(
          dev_id, next_port_info_ptr->dev_port, n_ctrs, symb_err);

      for (ctr = first_ctr; ctr < end_ctr; ctr++) {
        // tot_s_errs += ctrs[ctr];
        tot_s_errs += ctrs[ctr - first_ctr];
      }
      if (next_port_info_ptr->oper_status == PM_PORT_UP) {
        aim_printf(&uc->pvs,
                   "%2d/%d | %3d |  %3d  | %8.1e |",
                   next_port_info_ptr->pltfm_port_info.port_hdl.conn_id,
                   next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id,
                   next_port_info_ptr->dev_port,
                   tmac,
                   (float)(tot_s_errs * 1) / (float)(bps));
      } else {
        aim_printf(&uc->pvs,
                   "%2d/%d | %3d |  %3d  |    Dn    |",
                   next_port_info_ptr->pltfm_port_info.port_hdl.conn_id,
                   next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id,
                   next_port_info_ptr->dev_port,
                   tmac);
      }

      for (ctr = first_ctr; ctr < end_ctr; ctr++) {
        float ber;
        // s_errs = ctrs[ctr];
        s_errs = ctrs[ctr - first_ctr];
        ber = (float)(s_errs * 1) /
              ((float)(bps / ((uint64_t)n_ctrs)));  // 10b symbols, ea lane
                                                    // carries 1/n_ctrs of the
                                                    // bits
        if (next_port_info_ptr->oper_status == PM_PORT_UP) {
          aim_printf(&uc->pvs, "%8.1e |", ber);
        } else {
          aim_printf(&uc->pvs, "   ---   |");
        }
      }
      aim_printf(&uc->pvs, "\n");
      aim_printf(&uc->pvs, "                              |");
      for (ctr = first_ctr; ctr < end_ctr; ctr++) {
        if (next_port_info_ptr->oper_status == PM_PORT_UP) {
          // aim_printf(&uc->pvs, "%8d |", (uint32_t)ctrs[ctr]);
          aim_printf(&uc->pvs, "%8d |", (uint32_t)ctrs[ctr - first_ctr]);
        } else {
          aim_printf(&uc->pvs, "         |");
        }
      }
      aim_printf(&uc->pvs, "\n");
#undef DISPLAY_PCSL_RATIO
#if DISPLAY_PCSL_RATIO
      // test. display PCSL n/n+1 ratio
      aim_printf(&uc->pvs, "                              |");
      for (ctr = first_ctr; ctr < end_ctr; ctr += 2) {
        if (next_port_info_ptr->oper_status == PM_PORT_UP) {
          aim_printf(&uc->pvs,
                     "%8.1f           |",
                     (float)((float)ctrs[ctr - first_ctr] /
                             (float)ctrs[ctr - first_ctr + 1]));
        } else {
          aim_printf(&uc->pvs, "                   |");
        }
      }
      aim_printf(&uc->pvs, "\n");
#endif
      aim_printf(&uc->pvs,
                 "------------------------------+---------+---------+---------+"
                 "---------+---------+---------+---------+---------+---------+-"
                 "--------+---------+---------+---------+---------+---------+--"
                 "-------+\n");
    }
  }
}

static void pm_ucli_tof2_port_ber(ucli_context_t *uc,
                                  bf_dev_id_t dev_id,
                                  int integration_time,
                                  bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      // if (next_port_info_ptr->pltfm_port_info.log_mac_id == 0) {
      //  continue;
      //}
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
      uint64_t bps, s_errs, tot_s_errs = 0ul;
      uint64_t *ctrs = (uint64_t *)&fec_ln_ctrs;
      size_t ctrs_sz = sizeof(fec_ln_ctrs) / sizeof(uint64_t);

      bf_port_fec_get(dev_id, next_port_info_ptr->dev_port, &fec);
      bf_port_speed_get(dev_id, next_port_info_ptr->dev_port, &speed);
      bf_port_num_lanes_get(0, next_port_info_ptr->dev_port, &num_lanes);
      bf_serdes_encoding_mode_get(speed, num_lanes, &enc_mode);

      rc = port_mgr_tof2_map_dev_port_to_all(dev_id,
                                             next_port_info_ptr->dev_port,
                                             NULL,
                                             NULL,
                                             &umac,
                                             &ch_ln,
                                             &is_cpu_port);
      if (rc != BF_SUCCESS) {
        aim_printf(&uc->pvs,
                   "unable to map dev_port (%d) to umac\n",
                   next_port_info_ptr->dev_port);
        return;
      }

      if (speed == BF_SPEED_400G) {
        n_ctrs = 16;
        first_ctr = 0;
      } else if (speed == BF_SPEED_200G) {
        n_ctrs = 8;
        first_ctr = (next_port_info_ptr->dev_port & 0x7) * 2;
      } else if ((speed == BF_SPEED_100G) && (num_lanes == 2)) {
        n_ctrs = 4;
        first_ctr = (next_port_info_ptr->dev_port & 0x7) * 2;
      } else if ((speed == BF_SPEED_50G) && (num_lanes == 1)) {
        n_ctrs = 2;
        first_ctr = (next_port_info_ptr->dev_port & 0x7) * 2;
      } else {
        aim_printf(&uc->pvs,
                   "%2d/%d | %3d |  %3d  | not      |\n",
                   next_port_info_ptr->pltfm_port_info.port_hdl.conn_id,
                   next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id,
                   next_port_info_ptr->dev_port,
                   umac);
        aim_printf(&uc->pvs, "                   | supported|\n");
        aim_printf(&uc->pvs, "------------------------------+\n");
        continue;
      }
      /* get ports aggregate bit-rate */
      bps = pm_util_aggregate_bit_rate_get(speed, num_lanes);
      bps = (bps * integration_time) / 1000000;

      end_ctr = (first_ctr + n_ctrs);
      if (end_ctr > ctrs_sz) {
        aim_printf(&uc->pvs,
                   "Attempt to access array of size %zu with index %d\n",
                   ctrs_sz,
                   (int)end_ctr);
        return;
      }

      // timed collections of counters
      umac4_ctrs_rs_fec_ln_get(
          dev_id, umac, &fec_ln_ctrs);  // read first to clear
      bf_sys_usleep(integration_time);
      umac4_ctrs_rs_fec_ln_get(dev_id, umac, &fec_ln_ctrs);  // now for real

      for (ctr = first_ctr; ctr < end_ctr; ctr++) {
        tot_s_errs += ctrs[ctr];
      }
      if (next_port_info_ptr->oper_status == PM_PORT_UP) {
        aim_printf(&uc->pvs,
                   "%2d/%d | %3d |  %3d  | %8.1e |",
                   next_port_info_ptr->pltfm_port_info.port_hdl.conn_id,
                   next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id,
                   next_port_info_ptr->dev_port,
                   umac,
                   (float)(tot_s_errs * 1) / (float)(bps));
      } else {
        aim_printf(&uc->pvs,
                   "%2d/%d | %3d |  %3d  |    Dn    |",
                   next_port_info_ptr->pltfm_port_info.port_hdl.conn_id,
                   next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id,
                   next_port_info_ptr->dev_port,
                   umac);
      }

      for (ctr = first_ctr; ctr < end_ctr; ctr++) {
        float ber;
        s_errs = ctrs[ctr];
        ber = (float)(s_errs * 1) /
              ((float)(bps / ((uint64_t)n_ctrs)));  // 10b symbols, ea lane
                                                    // carries 1/n_ctrs of the
                                                    // bits
        if (next_port_info_ptr->oper_status == PM_PORT_UP) {
          aim_printf(&uc->pvs, "%8.1e |", ber);
        } else {
          aim_printf(&uc->pvs, "   ---   |");
        }
      }
      aim_printf(&uc->pvs, "\n");
      aim_printf(&uc->pvs, "                              |");
      for (ctr = first_ctr; ctr < end_ctr; ctr++) {
        if (next_port_info_ptr->oper_status == PM_PORT_UP) {
          aim_printf(&uc->pvs, "%8d |", (uint32_t)ctrs[ctr]);
        } else {
          aim_printf(&uc->pvs, "         |");
        }
      }
      aim_printf(&uc->pvs, "\n");
      aim_printf(&uc->pvs,
                 "------------------------------+---------+---------+---------+"
                 "---------+---------+---------+---------+---------+---------+-"
                 "--------+---------+---------+---------+---------+---------+--"
                 "-------+\n");
    }
  }
}

static ucli_status_t bf_pm_ucli_ucli__port_ber__(ucli_context_t *uc) {
  static char usage[] = "port-ber <conn_id/chnl> [integration_time_usec]";
  UCLI_COMMAND_INFO(uc, "port-ber", -1, "<port_str> [integration_time_usec]");
  int integration_time = 1000000;
  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  bf_status_t sts;

  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }
  int argc = (uc->pargs->count + 1);
  if (argc > 1) {
    sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], &port_hdl);
    if (sts != BF_SUCCESS) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
    if (argc > 2) {
      int t = strtoul(argv[2], NULL, 10);  // atoi(argv[2]);
      if (t >= 60000000) {                 // 60 sec max
        aim_printf(&uc->pvs,
                   "60 sec max integration time. Setting default=1sec\n");
        t = 1000000;
      }
      integration_time = t;
    } else {
      integration_time = 1000000;  // 1 sec default
    }
  }
  aim_printf(&uc->pvs,
             "Integration time: %3.1f\n",
             (float)integration_time / 1000000.0);

  aim_printf(&uc->pvs,
             "                              "
             "+----------------------------------------------------------------"
             "-----------------------------------------------------------------"
             "------------------------------+\n");
  aim_printf(&uc->pvs,
             "                              |                                  "
             "                                          FEC lane               "
             "                                                             \n");
  aim_printf(&uc->pvs,
             "-----+------+------+----------+---------+---------+---------+----"
             "-----+---------+---------+---------+---------+---------+---------"
             "+---------+---------+---------+---------+---------+---------+\n");
  aim_printf(&uc->pvs,
             "Port | D_P  | umac |   BER    |    0    |    1    |    2    |    "
             "3    |    4    |    5    |    6    |    7    |    8    |    9    "
             "|   10    |   11    |   12    |   13    |   14    |   15    |\n");
  aim_printf(&uc->pvs,
             "-----+------+------+----------+---------+---------+---------+----"
             "-----+---------+---------+---------+---------+---------+---------"
             "+---------+---------+---------+---------+---------+---------+\n");

  // conn==-1 and chnl==-1 indicates that scan through all the ports but only
  // print the ports that have been added.
  //-a option indicates that the scan and print all the ports (whether added or
  // not)

  if (bf_lld_dev_is_tof2(dev_id)) {
    pm_ucli_tof2_port_ber(uc, dev_id, integration_time, &port_hdl);
  } else if (bf_lld_dev_is_tof3(dev_id)) {
    pm_ucli_tof3_port_ber(uc, dev_id, integration_time, &port_hdl);
  }
  return 0;
}

static void pm_ucli_tof2_port_isi(ucli_context_t *uc,
                                  bf_dev_id_t dev_id,
                                  int aflag,
                                  bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      if (next_port_info_ptr->pltfm_port_info.log_mac_id == 0) {
        continue;
      }
      int num_lanes, ln;
      // bf_status_t rc;
      bf_serdes_encoding_mode_t enc_mode;
      bf_port_speed_t speed;

      bf_port_speed_get(dev_id, next_port_info_ptr->dev_port, &speed);
      bf_port_num_lanes_get(0, next_port_info_ptr->dev_port, &num_lanes);
      bf_serdes_encoding_mode_get(speed, num_lanes, &enc_mode);
      if (enc_mode == BF_SERDES_ENC_MODE_PAM4) {
        aim_printf(&uc->pvs,
                   "-----+------+------+------+------+------+------+------+----"
                   "--+------+------+------+------+------+------+------+------+"
                   "------+\n");
        aim_printf(&uc->pvs,
                   "Lane | pre2 | pre1 |  k1  |  k2  |  k3  |  k4  |  f6  ||  "
                   "f7  |  f8  |  f9  |  f10 |  f11 |  f12 |  f13 |  f14 |  "
                   "f15 | 8-15 |\n");
        aim_printf(&uc->pvs,
                   "-----+------+------+------+------+------+------+------+----"
                   "--+------+------+------+------+------+------+------+------+"
                   "------+\n");
      } else {
        aim_printf(&uc->pvs,
                   "-----+------+------+------+------+------+------+------+----"
                   "--+------+------+------+------+------+------+------+------+"
                   "------+\n");
        aim_printf(&uc->pvs,
                   "Lane |  f-1 |  f2  |  f3  |  f4  |  f5  |  f6  |  f7  |  "
                   "f8  |  f9  |  f10 |  f11 |  f12 |  f13 |  f14 |  f15 |  "
                   "f16 | 8-15 |\n");
        aim_printf(&uc->pvs,
                   "-----+------+------+------+------+------+------+------+----"
                   "--+------+------+------+------+------+------+------+------+"
                   "------+\n");
      }

      for (ln = 0; ln < num_lanes; ln++) {
        uint32_t isi_vals[16], sum_8_15 = 0;

        bf_tof2_serdes_isi_get(
            dev_id, next_port_info_ptr->dev_port, ln, isi_vals);
        aim_printf(&uc->pvs, "  %d  |", ln);
        for (int j = 0; j < 16; j++) {
          aim_printf(&uc->pvs, " %4d |", isi_vals[j]);
          if (j >= 9) {
            sum_8_15 += isi_vals[j];
          }
        }
        aim_printf(&uc->pvs, " %4d |\n", sum_8_15);
      }
      if (enc_mode == BF_SERDES_ENC_MODE_PAM4) {
        aim_printf(&uc->pvs,
                   "-----+------+------+------+------+------+------+------+----"
                   "--+------+------+------+------+------+------+------+------+"
                   "------+\n");
      } else {
        aim_printf(&uc->pvs,
                   "-----+------+------+------+------+------+------+------+----"
                   "--+------+------+------+------+------+------+------+------+"
                   "------+\n");
      }
    }
  }
  (void)aflag;
  (void)uc;
}

static ucli_status_t bf_pm_ucli_ucli__port_isi__(ucli_context_t *uc) {
  static char usage[] = "port-isi -a <conn_id/chnl>";
  UCLI_COMMAND_INFO(uc, "port-isi", -1, "-a <port_str>");

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
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
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

  pm_ucli_tof2_port_isi(uc, dev_id, aflag, &port_hdl);
  return 0;
}

ucli_context_t *serdes_uc = NULL;

typedef struct pm_ucli_tune_data_t {
  int32_t tap_val[5];  // {pre2, pre1, main, post1, post2}
  int32_t isi_val[5];  // {pre2, pre1, main, post1, post2}
  uint32_t selected_tap;
  int32_t last_tap_chg_dir[5];     // -1, 0, +1
  int32_t last_tap_chg_effect[5];  // -1 (worse), 0 (no change), 1 (better)
  uint32_t prbs_errs[5];           // 5x PRBS error count samples

  bool first_pass_done;
  int32_t best_tap_val[5];
  int32_t best_isi_val[5];
  uint32_t best_baseline;

  uint32_t baseline;  // avg of above count samples
  // same as above but after a tap change
  uint32_t delta_prbs_errs[5];  // 5x PRBS error count samples
  uint32_t delta_baseline;      // avg of above count samples

} pm_ucli_tune_data_t;

uint32_t num_taps_changed = 0;

#define NO_TAP_SEL 999

// indexed by [conn_id][chnl_id], first entry (0) unused
pm_ucli_tune_data_t tune_data[34][8] = {{{{0}}}};

// assumes top to bottom cabling (default)
uint32_t link_partner[34] = {0,  2,  1,  4,  3,  6,  5,  8,  7,  10, 9,  12,
                             11, 14, 13, 16, 15, 18, 17, 20, 19, 22, 21, 24,
                             23, 26, 25, 28, 27, 30, 29, 32, 31, 33};

/********************************************************************
 *
 ********************************************************************/
static void pm_ucli_tune_change_tap(uint32_t conn_id,
                                    uint32_t chnl_id,
                                    bf_dev_port_t p,
                                    uint32_t ln) {
  uint32_t lp_fp = link_partner[conn_id];
  bf_pal_front_port_handle_t port_hdl = {lp_fp, 0};
  bf_dev_id_t dev_id = 0;
  bf_dev_id_t dev_id_of_port = 0;
  bf_dev_port_t lp_dp;
  bf_status_t sts;
  uint32_t tap = tune_data[conn_id][chnl_id].selected_tap;
  (void)p;
  uint32_t real_tap[] = {0, 1, 4, -1, -1};
  uint32_t tx_tap_to_modify = real_tap[tap];

  if (tap == NO_TAP_SEL) {  // no tap selected
    return;
  }

  num_taps_changed++;

  sts = bf_pm_port_front_panel_port_to_dev_port_get(
      &port_hdl, &dev_id_of_port, &lp_dp);
  if (sts != BF_SUCCESS) {
    return;
  }

  dev_id = dev_id_of_port;

  bf_tof2_serdes_tx_taps_hw_get(dev_id,
                                lp_dp,
                                ln,
                                &tune_data[conn_id][chnl_id].tap_val[0],
                                &tune_data[conn_id][chnl_id].tap_val[1],
                                &tune_data[conn_id][chnl_id].tap_val[2],
                                &tune_data[conn_id][chnl_id].tap_val[3],
                                &tune_data[conn_id][chnl_id].tap_val[4]);
  tune_data[conn_id][chnl_id].tap_val[tx_tap_to_modify] +=
      tune_data[conn_id][chnl_id].last_tap_chg_dir[tap];
  bf_tof2_serdes_tx_taps_set(dev_id,
                             lp_dp,
                             ln,
                             tune_data[conn_id][chnl_id].tap_val[0],
                             tune_data[conn_id][chnl_id].tap_val[1],
                             tune_data[conn_id][chnl_id].tap_val[2],
                             tune_data[conn_id][chnl_id].tap_val[3],
                             tune_data[conn_id][chnl_id].tap_val[4],
                             true);
  aim_printf(
      &serdes_uc->pvs,
      "%2d/%d : d_p=%d : ln=%d : LP Tx Taps: %3d : %3d : %3d %3d : %3d (%s "
      "%s)\n",
      lp_fp,
      chnl_id,
      lp_dp,
      ln,
      tune_data[conn_id][chnl_id].tap_val[0],
      tune_data[conn_id][chnl_id].tap_val[1],
      tune_data[conn_id][chnl_id].tap_val[2],
      tune_data[conn_id][chnl_id].tap_val[3],
      tune_data[conn_id][chnl_id].tap_val[4],
      (tap == 0)
          ? "Pre2"
          : (tap == 1) ? "Pre1"
                       : (tap == 2) ? "Main"
                                    : (tap == 3) ? "Post1"
                                                 : (tap == 4) ? "Post2" : "??",
      tune_data[conn_id][chnl_id].last_tap_chg_dir[tap] < 0 ? "-1" : "+1");
}

/********************************************************************
 *
 ********************************************************************/
static void pm_ucli_tune_evaluate_tap_change(uint32_t conn_id,
                                             uint32_t chnl_id,
                                             bf_dev_port_t p,
                                             uint32_t ln) {
  uint32_t delta, margin_of_error = 100;
  uint32_t tap = tune_data[conn_id][chnl_id].selected_tap;
  (void)p;
  (void)ln;

  delta = abs(tune_data[conn_id][chnl_id].delta_baseline -
              tune_data[conn_id][chnl_id].baseline);

#if 0
  aim_printf(&serdes_uc->pvs,
             "%2d/%d : d_p=%d : ln=%d : base=%d : delta_base=%d : delta=%d\n",
             conn_id,
             chnl_id,
             p,
             ln,
             tune_data[conn_id][chnl_id].baseline,
             tune_data[conn_id][chnl_id].delta_baseline,
             delta);
#endif
  if (delta < margin_of_error) {
    tune_data[conn_id][chnl_id].last_tap_chg_effect[tap] = 0;
  } else if (delta >= margin_of_error) {
    if (tune_data[conn_id][chnl_id].delta_baseline >
        tune_data[conn_id][chnl_id].baseline) {
      int32_t saved;

      // got worse
      tune_data[conn_id][chnl_id].last_tap_chg_effect[tap] = -1;
      // save chg dir
      saved = tune_data[conn_id][chnl_id].last_tap_chg_dir[tap];
      tune_data[conn_id][chnl_id].last_tap_chg_dir[tap] = 0 - saved;
      aim_printf(&serdes_uc->pvs,
                 "%2d/%d : d_p=%d : ln=%d : LP Tx Taps : Revert\n",
                 conn_id,
                 chnl_id,
                 p,
                 ln);
      pm_ucli_tune_change_tap(conn_id, chnl_id, p, ln);  // change setting back
      tune_data[conn_id][chnl_id].last_tap_chg_dir[tap] = saved;
    } else {
      // got better
      tune_data[conn_id][chnl_id].last_tap_chg_effect[tap] = 1;
      if (!tune_data[conn_id][chnl_id].first_pass_done) {
        tune_data[conn_id][chnl_id].best_baseline = 0xffffffff;
        tune_data[conn_id][chnl_id].best_isi_val[0] =
            tune_data[conn_id][chnl_id].isi_val[0];
        tune_data[conn_id][chnl_id].best_isi_val[1] =
            tune_data[conn_id][chnl_id].isi_val[1];
        tune_data[conn_id][chnl_id].best_isi_val[2] =
            tune_data[conn_id][chnl_id].isi_val[2];
        tune_data[conn_id][chnl_id].best_isi_val[3] =
            tune_data[conn_id][chnl_id].isi_val[3];
        tune_data[conn_id][chnl_id].best_isi_val[4] =
            tune_data[conn_id][chnl_id].isi_val[4];
        tune_data[conn_id][chnl_id].first_pass_done = true;
      }
      if (tune_data[conn_id][chnl_id].delta_baseline <
          tune_data[conn_id][chnl_id].best_baseline) {
        tune_data[conn_id][chnl_id].best_baseline =
            tune_data[conn_id][chnl_id].delta_baseline;
        tune_data[conn_id][chnl_id].best_tap_val[0] =
            tune_data[conn_id][chnl_id].tap_val[0];
        tune_data[conn_id][chnl_id].best_tap_val[1] =
            tune_data[conn_id][chnl_id].tap_val[1];
        tune_data[conn_id][chnl_id].best_tap_val[2] =
            tune_data[conn_id][chnl_id].tap_val[2];
        tune_data[conn_id][chnl_id].best_tap_val[3] =
            tune_data[conn_id][chnl_id].tap_val[3];
        tune_data[conn_id][chnl_id].best_tap_val[4] =
            tune_data[conn_id][chnl_id].tap_val[4];
        tune_data[conn_id][chnl_id].best_isi_val[0] =
            tune_data[conn_id][chnl_id].isi_val[0];
        tune_data[conn_id][chnl_id].best_isi_val[1] =
            tune_data[conn_id][chnl_id].isi_val[1];
        tune_data[conn_id][chnl_id].best_isi_val[2] =
            tune_data[conn_id][chnl_id].isi_val[2];
        tune_data[conn_id][chnl_id].best_isi_val[3] =
            tune_data[conn_id][chnl_id].isi_val[3];
        tune_data[conn_id][chnl_id].best_isi_val[4] =
            tune_data[conn_id][chnl_id].isi_val[4];
      }
    }
  }
}

/********************************************************************
 *
 ********************************************************************/
static void pm_ucli_tune_select_tap(uint32_t conn_id,
                                    uint32_t chnl_id,
                                    bf_dev_port_t p,
                                    uint32_t ln) {
  int i;
  bf_dev_id_t dev_id = 0;
  int32_t max_dist = 0, max_dist_tap = NO_TAP_SEL, isi_vals[16] = {0};
  int32_t goal_distance = 28;
  int32_t isi_sample, isi_integration_cnt = 5, isi_sample_vals[16] = {0};
  uint32_t tap_opt_order[] = {0, 1, 2, 3, 4};

  // ISI value seems to move a lot. Try to get an avg
  for (isi_sample = 0; isi_sample < isi_integration_cnt; isi_sample++) {
    bf_tof2_serdes_isi_get(dev_id, p, ln, (uint32_t *)isi_sample_vals);
    for (i = 0; i < 5; i++) {
      isi_vals[i] += isi_sample_vals[i];
    }
  }
  for (i = 0; i < 5; i++) {
    isi_vals[i] /= isi_integration_cnt;  // keep an avg
  }

  for (i = 0; i < 5; i++) {
    tune_data[conn_id][chnl_id].isi_val[i] = isi_vals[i];  // save first 5
  }

  for (i = 0; i < 5; i++) {
    uint32_t tap = tap_opt_order[i];

    if (abs(isi_vals[tap]) > goal_distance) {
      max_dist = isi_vals[tap];
      max_dist_tap = tap;
      break;  // select first out-of-range tap (from left to right)
    }
    if (abs(isi_vals[tap]) > (int)abs(max_dist)) {
      max_dist = isi_vals[tap];
      max_dist_tap = tap;
    }
    if (abs(isi_vals[tap]) > (int)max_dist) {
      max_dist = abs(isi_vals[tap]);
      max_dist_tap = tap;
    }
  }
  if (max_dist <= goal_distance) {
    max_dist_tap = NO_TAP_SEL;
  } else if (max_dist_tap > 2) {  // only pre2, pre1, post2 are valid
    max_dist_tap = NO_TAP_SEL;
  }
  if (abs(max_dist) <= goal_distance) {
    max_dist_tap = NO_TAP_SEL;
  } else if (max_dist_tap > 2) {  // only pre2, pre1, post2 are valid
    max_dist_tap = NO_TAP_SEL;
  }
  if (max_dist_tap != NO_TAP_SEL) {
#if 0
    aim_printf(&serdes_uc->pvs,
               "%2d/%d : d_p=%d : ln=%d : ISI %d %d %d %d %d : max_dist_tap=%d : max_dist=%d : "
               "last_dir=%d : last_effect=%d\n",
               conn_id,
               chnl_id,
               p,
               ln,
               isi_vals[0], isi_vals[1],isi_vals[2],isi_vals[3],isi_vals[4],
               max_dist_tap,
               max_dist,
               tune_data[conn_id][chnl_id].last_tap_chg_dir[max_dist_tap],
               tune_data[conn_id][chnl_id].last_tap_chg_effect[max_dist_tap]);
#endif
  }

  tune_data[conn_id][chnl_id].selected_tap =
      max_dist_tap;  // == NO_TAP_SEL means no tap selected
  if (tune_data[conn_id][chnl_id].selected_tap != NO_TAP_SEL) {
    uint32_t tap = tune_data[conn_id][chnl_id].selected_tap;

    if (tune_data[conn_id][chnl_id].last_tap_chg_effect[tap] <= 0) {
      if (tune_data[conn_id][chnl_id].last_tap_chg_dir[tap] == 0) {  // first
                                                                     // chg
        if (tap == 0) {                                              // pre2
          tune_data[conn_id][chnl_id].last_tap_chg_dir[tap] = 1;  // go up first
        } else if (tap == 1) {                                    // pre1
          tune_data[conn_id][chnl_id].last_tap_chg_dir[tap] =
              -1;                                                 // go dn first
        } else {                                                  // post1
          tune_data[conn_id][chnl_id].last_tap_chg_dir[tap] = 1;  // go up first
        }
      } else {
        if (tap == 0) {        // pre2
          if (max_dist < 0) {  // move opposite dir
            tune_data[conn_id][chnl_id].last_tap_chg_dir[tap] = 1;  // go up
          } else {
            tune_data[conn_id][chnl_id].last_tap_chg_dir[tap] = -1;  // go dn
          }
        } else if (tap == 1) {  // pre1
          if (max_dist < 0) {   // move opposite dir
            tune_data[conn_id][chnl_id].last_tap_chg_dir[tap] = 1;  // go up
          } else {
            tune_data[conn_id][chnl_id].last_tap_chg_dir[tap] = -1;  // go dn
          }
        } else {               // post1
          if (max_dist < 0) {  // move opposite dir
            tune_data[conn_id][chnl_id].last_tap_chg_dir[tap] = 1;  // go up
          } else {
            tune_data[conn_id][chnl_id].last_tap_chg_dir[tap] = -1;  // go dn
          }
        }
        // tune_data[conn_id][chnl_id].last_tap_chg_dir[tap] =
        //    0 -
        //    tune_data[conn_id][chnl_id].last_tap_chg_dir[tap];  // opposite
        //    dir
      }
    } else {
      // keep same change direction
    }
#if 0
    aim_printf(&serdes_uc->pvs,
               "%2d/%d : d_p=%d : ln=%d : max_dist_tap=%d : max_dist=%d : "
               "this_dir=%d\n",
               conn_id,
               chnl_id,
               p,
               ln,
               max_dist_tap,
               max_dist,
               tune_data[conn_id][chnl_id].last_tap_chg_dir[max_dist_tap]);
#endif
  }
}

/********************************************************************
 *
 ********************************************************************/
static void pm_ucli_tune_get_baseline(uint32_t conn_id,
                                      uint32_t chnl_id,
                                      bf_dev_port_t p,
                                      uint32_t ln) {
  int i;
  bf_dev_id_t dev_id = 0;
  uint64_t total_errs = 0ul;

  for (i = 0; i < 5; i++) {
    bf_tof2_serdes_prbs_rst_set(dev_id, p, ln);
    bf_sys_usleep(200 * 1000);  // 10ms data collection per lane
    bf_tof2_serdes_rx_prbs_err_get(
        dev_id, p, ln, &tune_data[conn_id][chnl_id].prbs_errs[i]);
  }
  total_errs = 0;
  for (i = 0; i < 5; i++) {
    total_errs += (uint64_t)tune_data[conn_id][chnl_id].prbs_errs[i];
  }
  total_errs = total_errs / 5ul;
  tune_data[conn_id][chnl_id].baseline = (uint32_t)total_errs;
}

/********************************************************************
 *
 ********************************************************************/
static void pm_ucli_tune_get_new_baseline(uint32_t conn_id,
                                          uint32_t chnl_id,
                                          bf_dev_port_t p,
                                          uint32_t ln) {
  int i;
  bf_dev_id_t dev_id = 0;
  uint64_t total_errs = 0ul;

  for (i = 0; i < 5; i++) {
    bf_tof2_serdes_prbs_rst_set(dev_id, p, ln);
    bf_sys_usleep(200 * 1000);  // 10ms data collection per lane
    bf_tof2_serdes_rx_prbs_err_get(
        dev_id, p, ln, &tune_data[conn_id][chnl_id].delta_prbs_errs[i]);
  }
  total_errs = 0;
  for (i = 0; i < 5; i++) {
    total_errs += (uint64_t)tune_data[conn_id][chnl_id].delta_prbs_errs[i];
  }
  total_errs = total_errs / 5ul;
  tune_data[conn_id][chnl_id].delta_baseline = (uint32_t)total_errs;
}

/********************************************************************
 *
 ********************************************************************/
static void pm_ucli_tune_tap(uint32_t conn_id,
                             uint32_t chnl_id,
                             bf_dev_port_t p,
                             uint32_t ln) {
  pm_ucli_tune_get_baseline(conn_id, chnl_id, p, ln);
  pm_ucli_tune_select_tap(conn_id, chnl_id, p, ln);
  pm_ucli_tune_change_tap(conn_id, chnl_id, p, ln);
}

/********************************************************************
 *
 ********************************************************************/
static void pm_ucli_tune_evaluate_change(uint32_t conn_id,
                                         uint32_t chnl_id,
                                         bf_dev_port_t p,
                                         uint32_t ln) {
  pm_ucli_tune_get_new_baseline(conn_id, chnl_id, p, ln);
  pm_ucli_tune_evaluate_tap_change(conn_id, chnl_id, p, ln);
}

/********************************************************************
 *
 ********************************************************************/
// step 1: change tap
// step 2: re-adapt
// step 3: evaluate change
static void pm_ucli_tof2_fp_tune(bf_dev_id_t dev_id,
                                 bf_pal_front_port_handle_t *port_hdl,
                                 uint32_t step) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      if (next_port_info_ptr->pltfm_port_info.log_mac_id == 0) {
        continue;
      }
      int num_lanes, ln;
      bf_port_num_lanes_get(0, next_port_info_ptr->dev_port, &num_lanes);

      if (step == 1) {
        uint32_t lp_fp =
            link_partner[next_port_info_ptr->pltfm_port_info.port_hdl.conn_id];
        bf_pal_front_port_handle_t lp_port_hdl = {lp_fp, 0};
        bf_dev_port_t lp_dp;
        bf_pm_port_front_panel_port_to_dev_port_get(
            &lp_port_hdl, &dev_id_of_port, &lp_dp);
        dev_id = dev_id_of_port;

        // dump LP Tx parms
        tof2_serdes_config(lp_fp, lp_dp);
        // and DUT Rx parms
        pm_ucli_tof2_port_isi(
            serdes_uc, 0, 0, &next_port_info_ptr->pltfm_port_info.port_hdl);
      }
      for (ln = 0; ln < num_lanes; ln++) {
        if (step == 1) {
          pm_ucli_tune_tap(
              next_port_info_ptr->pltfm_port_info.port_hdl.conn_id,
              next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id + ln,
              next_port_info_ptr->dev_port,
              ln);

        } else if (step == 3) {
          pm_ucli_tune_evaluate_change(
              next_port_info_ptr->pltfm_port_info.port_hdl.conn_id,
              next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id + ln,
              next_port_info_ptr->dev_port,
              ln);
        }
      }
      if ((step == 3) || (step == 4)) {
        pm_ucli_tof2_prbs_show(
            serdes_uc, 0, 200, &next_port_info_ptr->pltfm_port_info.port_hdl);
        if (step == 4) {
          for (ln = 0; ln < num_lanes; ln++) {
            uint32_t conn_id, chnl_id;
            conn_id = next_port_info_ptr->pltfm_port_info.port_hdl.conn_id;
            chnl_id = next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id + ln;
            aim_printf(&serdes_uc->pvs,
                       "%2d/%d : d_p=%d : ln=%d : Base=%6d : Taps : %3d : %3d "
                       ": %3d : %3d : %3d : ISI : %4d : %4d : %4d : %4d : "
                       "%4d\n",
                       next_port_info_ptr->pltfm_port_info.port_hdl.conn_id,
                       next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id,
                       next_port_info_ptr->dev_port,
                       ln,
                       tune_data[conn_id][chnl_id].best_baseline,
                       tune_data[conn_id][chnl_id].best_tap_val[0],
                       tune_data[conn_id][chnl_id].best_tap_val[1],
                       tune_data[conn_id][chnl_id].best_tap_val[2],
                       tune_data[conn_id][chnl_id].best_tap_val[3],
                       tune_data[conn_id][chnl_id].best_tap_val[4],
                       tune_data[conn_id][chnl_id].best_isi_val[0],
                       tune_data[conn_id][chnl_id].best_isi_val[1],
                       tune_data[conn_id][chnl_id].best_isi_val[2],
                       tune_data[conn_id][chnl_id].best_isi_val[3],
                       tune_data[conn_id][chnl_id].best_isi_val[4]);
          }
        }
      }
    }
  }
}

void pm_ucli_re_adapt(bf_dev_id_t dev_id,
                      bf_pal_front_port_handle_t *port_hdl) {
  int num_lanes, ln;
  bool adapt_done;
  uint32_t adapt, readapt, link_lost;
  uint32_t wait, max_wait = 20;
  bf_dev_id_t dev_id_of_port = 0;
  bf_dev_port_t dev_port;

  bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);
  dev_id = dev_id_of_port;
  bf_port_num_lanes_get(dev_id, dev_port, &num_lanes);

  // test : see what happens if we dont re-run adaptation
  bf_sys_usleep(10000);
  {
    bool re_adapted = false;

    for (ln = 0; ln < num_lanes; ln++) {
      bf_tof2_serdes_adapt_counts_get(
          dev_id, dev_port, ln, &adapt_done, &adapt, &readapt, &link_lost);
      if (link_lost) {
        aim_printf(&serdes_uc->pvs,
                   "%d/%d : %3d : %d : Lost link ..\n",
                   port_hdl->conn_id,
                   port_hdl->chnl_id,
                   dev_port,
                   ln);
        re_adapted = true;
      }
    }
    if (re_adapted) {
      aim_printf(&serdes_uc->pvs, "Wait 20s for re-adapt ..\n");
      bf_sys_usleep(20000000);  // give it 20s
    }
  }
  return;

  for (ln = 0; ln < num_lanes; ln++) {
    bf_tof2_serdes_lane_reset_set(dev_id, dev_port, ln);
  }
  bf_sys_usleep(1000000);

  for (wait = 0; wait < max_wait; wait++) {
    for (ln = 0; ln < num_lanes; ln++) {
      bf_tof2_serdes_adapt_counts_get(
          dev_id, dev_port, ln, &adapt_done, &adapt, &readapt, &link_lost);
      if (!adapt_done) {
        break;
      }
    }
    if (adapt_done) break;
    bf_sys_usleep(1000000);
  }
  bf_sys_usleep(60 * 1000000);  // + 10sec settling time ..
}

int pm_ucli_num_taps_changed(bf_dev_id_t dev_id,
                             bf_pal_front_port_handle_t *port_hdl) {
  int num_lanes, ln;
  bf_dev_id_t dev_id_of_port = 0;
  bf_dev_port_t dev_port;
  int n_taps_changed = 0;

  bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);
  dev_id = dev_id_of_port;
  bf_port_num_lanes_get(dev_id, dev_port, &num_lanes);

  for (ln = 0; ln < num_lanes; ln++) {
    if (tune_data[port_hdl->conn_id][port_hdl->chnl_id].selected_tap !=
        NO_TAP_SEL)
      n_taps_changed++;
  }
  return n_taps_changed;
}

static ucli_status_t bf_pm_ucli_ucli__port_tune__(ucli_context_t *uc) {
  static char usage[] = "port-tune <conn_id/chnl> <LP conn_id/chnl>";
  UCLI_COMMAND_INFO(uc, "port-tune", -1, "<port_str> <LP port_str>");

  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl, lp_port_hdl;
  bf_status_t sts;

  serdes_uc = uc;

  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }
  int argc = (uc->pargs->count + 1);
  if (argc < 2) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }
  sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], &port_hdl);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }
  if (argc > 2) {
    // modify default LP mapping
    sts = bf_pm_port_str_to_hdl_get(dev_id, argv[2], &lp_port_hdl);
    if (sts != BF_SUCCESS) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
    link_partner[port_hdl.conn_id] = lp_port_hdl.conn_id;
    link_partner[lp_port_hdl.conn_id] = port_hdl.conn_id;
  }

  // conn==-1 and chnl==-1 indicates that scan through all the ports but only
  // print the ports that have been added.
  //-a option indicates that the scan and print all the ports (whether added or
  // not)

  int32_t max_passes = 7;
  do {
    num_taps_changed = 0;
    pm_ucli_tof2_fp_tune(dev_id, &port_hdl, 1);
    pm_ucli_re_adapt(dev_id, &port_hdl);
    pm_ucli_tof2_fp_tune(dev_id, &port_hdl, 3);
    // 2nd pass of adaptation required in case any settings were reverted
    pm_ucli_re_adapt(dev_id, &port_hdl);
    aim_printf(&uc->pvs, "Pass %d of %d\n", (7 - max_passes), 7);
  } while ((num_taps_changed > 0) && (--max_passes > 0));

  // show best
  pm_ucli_tof2_fp_tune(dev_id, &port_hdl, 4);

  return 0;
}

static void pm_ucli_tof2_port_fw_dbg_cmd(ucli_context_t *uc,
                                         bf_dev_id_t dev_id,
                                         bf_pal_front_port_handle_t *port_hdl,
                                         uint32_t section,
                                         uint32_t index) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      // if (next_port_info_ptr->pltfm_port_info.log_mac_id == 0) {
      //  continue;
      //}
      int num_lanes, ln;
      bf_port_num_lanes_get(0, next_port_info_ptr->dev_port, &num_lanes);

      for (ln = 0; ln < num_lanes; ln++) {
        uint32_t result;

        bf_tof2_serdes_fw_debug_cmd(
            dev_id, next_port_info_ptr->dev_port, ln, section, index, &result);
        aim_printf(&uc->pvs,
                   "%2d/%d.%d: FW dbg cmd: section=%04x : index=%04x : %4x "
                   "(dec. %d)\n",
                   port_hdl->conn_id,
                   port_hdl->chnl_id,
                   ln,
                   section,
                   index,
                   result,
                   result);
      }
    }
  }
}

static ucli_status_t bf_pm_ucli_ucli__port_fw_dbg_cmd__(ucli_context_t *uc) {
  static char usage[] = "port-fw-dbg-cmd <conn_id/chnl> <section> <index>";
  UCLI_COMMAND_INFO(uc, "port-fw-dbg-cmd", -1, "<port_str> <section> <index>");

  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  bf_status_t sts;
  uint32_t section, index;

  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }
  int argc = (uc->pargs->count + 1);
  if (argc < 4) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }
  sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], &port_hdl);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }
  section = strtoul(uc->pargs->args[1], NULL, 16);
  index = strtoul(uc->pargs->args[2], NULL, 16);

  // conn==-1 and chnl==-1 indicates that scan through all the ports but only
  // print the ports that have been added.
  //-a option indicates that the scan and print all the ports (whether added or
  // not)

  pm_ucli_tof2_port_fw_dbg_cmd(uc, dev_id, &port_hdl, section, index);
  return 0;
}

static void pm_ucli_tof2_port_fw_cmd(ucli_context_t *uc,
                                     bf_dev_id_t dev_id,
                                     bf_pal_front_port_handle_t *port_hdl,
                                     uint32_t cmd,
                                     uint32_t detail) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      if (next_port_info_ptr->pltfm_port_info.log_mac_id == 0) {
        continue;
      }
      int num_lanes, ln;
      bf_port_num_lanes_get(0, next_port_info_ptr->dev_port, &num_lanes);

      aim_printf(&uc->pvs,
                 "%2d/%d: FW cmd: cmd=%04x : detail=%04x : \n   : ",
                 port_hdl->conn_id,
                 port_hdl->chnl_id,
                 cmd,
                 detail);
      for (ln = 0; ln < num_lanes; ln++) {
        aim_printf(&uc->pvs, " %4x :", ln);
      }
      aim_printf(
          &uc->pvs,
          "\n------+------+------+------+------+------+------+------+\n");
      aim_printf(&uc->pvs, "\n");
      for (int n_items = 0; n_items < 50; n_items++) {
        aim_printf(&uc->pvs, "%2x | ", n_items);
        for (ln = 0; ln < num_lanes; ln++) {
          uint32_t result;

          bf_tof2_serdes_fw_cmd(dev_id,
                                next_port_info_ptr->dev_port,
                                ln,
                                cmd + ln,
                                detail + n_items,
                                &result);
          aim_printf(&uc->pvs, " %4x |", result);
        }
        aim_printf(&uc->pvs, "\n");
      }
    }
  }
}

static ucli_status_t bf_pm_ucli_ucli__port_fw_cmd__(ucli_context_t *uc) {
  static char usage[] = "port-fw-cmd <conn_id/chnl> <cmd> <detail>";
  UCLI_COMMAND_INFO(uc, "port-fw-cmd", -1, "<port_str> <cmd> <detail>");

  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  bf_status_t sts;
  uint32_t cmd, detail;

  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }
  int argc = (uc->pargs->count + 1);
  if (argc < 4) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }
  sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], &port_hdl);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }
  cmd = strtoul(uc->pargs->args[1], NULL, 16);
  detail = strtoul(uc->pargs->args[2], NULL, 16);

  // conn==-1 and chnl==-1 indicates that scan through all the ports but only
  // print the ports that have been added.
  //-a option indicates that the scan and print all the ports (whether added or
  // not)

  pm_ucli_tof2_port_fw_cmd(uc, dev_id, &port_hdl, cmd, detail);
  return 0;
}

static void pm_ucli_tof2_port_fw_reg_rd(ucli_context_t *uc,
                                        bf_dev_id_t dev_id,
                                        bf_pal_front_port_handle_t *port_hdl,
                                        uint32_t fw_reg_addr,
                                        uint32_t *data) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      // if (next_port_info_ptr->pltfm_port_info.log_mac_id == 0) {
      //  continue;
      //}
      {
        bf_status_t sts;

        sts = bf_tof2_serdes_fw_reg_section_rd(
            dev_id, next_port_info_ptr->dev_port, 0, fw_reg_addr, data, 0);
        aim_printf(&uc->pvs,
                   "%2d/%d: FW reg RD [%04xh <%d>] : %08x <%d> : (sts=%d)\n",
                   port_hdl->conn_id,
                   port_hdl->chnl_id,
                   fw_reg_addr,
                   fw_reg_addr,
                   *data,
                   *data,
                   sts);
      }
    }
  }
}

static ucli_status_t bf_pm_ucli_ucli__port_fw_reg_rd__(ucli_context_t *uc) {
  static char usage[] = "port-fw-reg-rd <conn_id/chnl> <fw-reg-decimal>";
  UCLI_COMMAND_INFO(uc, "port-fw-reg-rd", 2, "<port_str> <fw-reg-decimal>");

  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  bf_status_t sts;
  uint32_t fw_reg_addr, data;

  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }
  sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], &port_hdl);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }
  fw_reg_addr = strtoul(uc->pargs->args[1], NULL, 10);

  // conn==-1 and chnl==-1 indicates that scan through all the ports but only
  // print the ports that have been added.
  //-a option indicates that the scan and print all the ports (whether added or
  // not)
  pm_ucli_tof2_port_fw_reg_rd(uc, dev_id, &port_hdl, fw_reg_addr, &data);
  return 0;
}

static void pm_ucli_tof2_port_fw_reg_wr(ucli_context_t *uc,
                                        bf_dev_id_t dev_id,
                                        bf_pal_front_port_handle_t *port_hdl,
                                        uint32_t fw_reg_addr,
                                        uint32_t data) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      // if (next_port_info_ptr->pltfm_port_info.log_mac_id == 0) {
      //  continue;
      //}
      {
        bf_status_t sts;

        sts = bf_tof2_serdes_fw_reg_section_wr(
            dev_id, next_port_info_ptr->dev_port, 0, fw_reg_addr, data, 0);
        aim_printf(&uc->pvs,
                   "%2d/%d: FW reg WR [%04xh <%d>] : %08x <%d> : (sts=%d)\n",
                   port_hdl->conn_id,
                   port_hdl->chnl_id,
                   fw_reg_addr,
                   fw_reg_addr,
                   data,
                   data,
                   sts);
      }
    }
  }
}

static ucli_status_t bf_pm_ucli_ucli__port_fw_reg_wr__(ucli_context_t *uc) {
  static char usage[] =
      "port-fw-reg-wr <conn_id/chnl> <fw-reg-decimal> <data-hex>";
  UCLI_COMMAND_INFO(
      uc, "port-fw-reg-wr", 3, "<port_str> <fw-reg-decimal> <data-hex>");

  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  bf_status_t sts;
  uint32_t fw_reg_addr, data;

  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }
  sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], &port_hdl);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }
  fw_reg_addr = strtoul(uc->pargs->args[1], NULL, 10);
  data = strtoul(uc->pargs->args[2], NULL, 16);

  // conn==-1 and chnl==-1 indicates that scan through all the ports but only
  // print the ports that have been added.
  //-a option indicates that the scan and print all the ports (whether added or
  // not)
  pm_ucli_tof2_port_fw_reg_wr(uc, dev_id, &port_hdl, fw_reg_addr, data);
  return 0;
}

static void pm_ucli_tof2_port_exit_codes_cmd(
    ucli_context_t *uc,
    bf_dev_id_t dev_id,
    bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      // if (next_port_info_ptr->pltfm_port_info.log_mac_id == 0) {
      //  continue;
      //}
      int num_lanes, ln;
      bf_port_num_lanes_get(0, next_port_info_ptr->dev_port, &num_lanes);

      for (ln = 0; ln < num_lanes; ln++) {
        uint32_t result;
        uint32_t section = 0;
        uint32_t index;

        aim_printf(&uc->pvs,
                   "%2d/%d.%d: Exit codes: ",
                   port_hdl->conn_id,
                   port_hdl->chnl_id,
                   ln);
        for (index = 0x63; index < 0x63 + 16 + 1; index++) {
          bf_tof2_serdes_fw_debug_cmd(dev_id,
                                      next_port_info_ptr->dev_port,
                                      ln,
                                      section,
                                      index,
                                      &result);
          if (result != 0) {
            aim_printf(&uc->pvs, "%4x : ", result);
          }
        }
        aim_printf(&uc->pvs, "\n");
      }
    }
  }
}

static ucli_status_t bf_pm_ucli_ucli__port_exit_codes__(ucli_context_t *uc) {
  static char usage[] = "port-exit-codes <conn_id/chnl>";
  UCLI_COMMAND_INFO(uc, "port-exit-codes", 1, "<port_str>");

  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  bf_status_t sts;

  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }
  int argc = (uc->pargs->count + 1);
  if (argc < 2) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }
  sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], &port_hdl);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }

  // conn==-1 and chnl==-1 indicates that scan through all the ports but only
  // print the ports that have been added.
  //-a option indicates that the scan and print all the ports (whether added or
  // not)

  pm_ucli_tof2_port_exit_codes_cmd(uc, dev_id, &port_hdl);
  return 0;
}

extern bf_status_t bf_tof2_serdes_tx_taps_set(bf_dev_id_t dev_id,
                                              bf_dev_port_t dev_port,
                                              uint32_t ln,
                                              int32_t pre2,
                                              int32_t pre1,
                                              int32_t main,
                                              int32_t post1,
                                              int32_t post2,
                                              bool apply);
bf_status_t bf_tof3_serdes_txfir_config_set(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port,
                                            uint32_t ln,
                                            uint32_t cm3,
                                            uint32_t cm2,
                                            uint32_t cm1,
                                            uint32_t c0,
                                            uint32_t c1);
bf_status_t bf_tof3_serdes_txfir_config_get(bf_dev_id_t dev_id,
                                            bf_dev_port_t dev_port,
                                            uint32_t ln,
                                            uint32_t *cm3,
                                            uint32_t *cm2,
                                            uint32_t *cm1,
                                            uint32_t *c0,
                                            uint32_t *c1);

static void pm_ucli_tx_eq_set(ucli_context_t *uc,
                              int show_only,
                              int use_default,
                              char *port_str,
                              int32_t ln,  // -1 means all lanes
                              int32_t pre3,
                              int32_t pre2,
                              int32_t pre1,
                              int32_t main_tap,
                              int32_t post1,
                              int32_t post2) {
  bf_status_t sts;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pm_port_info_t *port_info;
  bf_pal_front_port_handle_t *list = NULL;

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get(port_str, &list_sz, &list);
  if (sts != BF_SUCCESS) {
    return;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    bool is_port_internal = false;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;

    port_info = pm_port_info_get(dev_id, dev_port);
    if (port_info == NULL) {
      continue;
    }
    if (!port_info->is_added) {
      continue;
    }
    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      int num_lanes, first_ln, last_ln;

      bf_port_num_lanes_get(dev_id, dev_port, &num_lanes);
      if (num_lanes < 1) {
        PM_ERROR("num_lanes (%d) invalid for %d/%d",
                 num_lanes,
                 port_hdl_ptr->conn_id,
                 port_hdl_ptr->chnl_id);
        return;
      }
      if (ln == -1) {  // all lanes
        first_ln = 0;
        last_ln = num_lanes - 1;
      } else {
        if ((ln < 0) || (ln >= num_lanes)) {
          aim_printf(&uc->pvs,
                     "%2d/%d : valid lanes are 0 - %d\n",
                     port_hdl_ptr->conn_id,
                     port_hdl_ptr->chnl_id,
                     num_lanes - 1);
          return;
        }
        first_ln = ln;
        last_ln = ln;
      }
      if ((bf_lld_dev_is_tof2(dev_id)) || (bf_lld_dev_is_tof3(dev_id))) {
        if (show_only) {
          port_info = pm_port_info_get(dev_id, dev_port);
          if (port_info == NULL) {
            PM_ERROR("Error: Unable to get port info object (d_p: %d:%3d)\n",
                     dev_id,
                     dev_port);
          } else {
            if (bf_lld_dev_is_tof2(dev_id)) {
              tof2_serdes_config(port_hdl_ptr->conn_id, dev_port);
            } else if (bf_lld_dev_is_tof3(dev_id)) {
              for (int __ln = 0; __ln < num_lanes; __ln++) {
                uint32_t cm3, cm2, cm1, c0, c1;
                uint32_t r_cm3, r_cm2, r_cm1, r_c0, r_c1;
                bf_tof3_serdes_txfir_range_get(dev_id,
                                               dev_port,
                                               __ln,
                                               &r_cm3,
                                               &r_cm2,
                                               &r_cm1,
                                               &r_c0,
                                               &r_c1);
                bf_tof3_serdes_txfir_config_get(
                    dev_id, dev_port, __ln, &cm3, &cm2, &cm1, &c0, &c1);
                aim_printf(&uc->pvs,
                           "%2d/%d | ln%d | %2d | %2d | %2d | %2d | %2d | %2d "
                           "| %2d | %2d | %2d | %2d \n",
                           port_hdl_ptr->conn_id,
                           port_hdl_ptr->chnl_id,
                           __ln,
                           cm3,
                           cm2,
                           cm1,
                           c0,
                           c1,
                           r_cm3,
                           r_cm2,
                           r_cm1,
                           r_c0,
                           r_c1);
              }
              aim_printf(&uc->pvs,
                         "-----+-----+----+----+----+----+----+----+----+----+-"
                         "---+----\n");
            }
            if (port_info->serdes_tx_eq_override) {
              aim_printf(&uc->pvs,
                         "%2d/%d : Tx Eq Override set\n",
                         port_hdl_ptr->conn_id,
                         port_hdl_ptr->chnl_id);
            } else {
              bool override_set = false;
              for (int __ln = 0; __ln < num_lanes; __ln++) {
                if (port_info->serdes_lane_tx_eq_override[__ln]) {
                  override_set = true;
                  break;
                }
              }
              if (override_set) {
                aim_printf(&uc->pvs,
                           "%2d/%d : Tx Eq Override set on lanes: ",
                           port_hdl_ptr->conn_id,
                           port_hdl_ptr->chnl_id);
                for (int __ln = 0; __ln < num_lanes; __ln++) {
                  if (port_info->serdes_lane_tx_eq_override[__ln]) {
                    aim_printf(&uc->pvs, "%d ", __ln);
                  }
                }
                aim_printf(&uc->pvs, "\n");
              }
            }
          }
        } else if (use_default) {
          // now some lane is using defaults so clear port-level override
          //
          // next_port_info_ptr is only a copy of the bf_pm port_info struct.
          // Need to use API
          // to set the override in the "real" port_info struct
          bf_pm_serdes_tx_eq_override_set(dev_id, port_hdl_ptr, false);

          // clear per-lane override(s)
          for (int __ln = first_ln; __ln <= last_ln; __ln++) {
            bf_pm_serdes_lane_tx_eq_override_set(
                dev_id, port_hdl_ptr, __ln, false);
          }
        } else {
          if (ln == -1) {  // all lanes
            // next_port_info_ptr is only a copy of the bf_pm port_info struct.
            // Need to use API
            // to set the override in the "real" port_info struct
            bf_pm_serdes_tx_eq_override_set(dev_id, port_hdl_ptr, true);
          } else {
            bf_pm_serdes_tx_eq_override_set(dev_id, port_hdl_ptr, false);
          }
          for (int __ln = first_ln; __ln <= last_ln; __ln++) {
            // set per-lane override as well
            bf_pm_serdes_lane_tx_eq_override_set(
                dev_id, port_hdl_ptr, __ln, true);
            if (bf_lld_dev_is_tof3(dev_id)) {
              sts = bf_tof3_serdes_txfir_config_set(
                  dev_id, dev_port, __ln, pre3, pre2, pre1, main_tap, post1);
            } else {
              sts = bf_tof2_serdes_tx_taps_set(dev_id,
                                               dev_port,
                                               __ln,
                                               pre2,
                                               pre1,
                                               main_tap,
                                               post1,
                                               post2,
                                               true);
            }
            if (sts != BF_SUCCESS) {
              PM_ERROR(
                  "Error %d: Unable to set Tx taps for %d/%d ln%d (d_p: "
                  "%d:%3d)\n",
                  sts,
                  port_hdl_ptr->conn_id,
                  port_hdl_ptr->chnl_id,
                  __ln,
                  dev_id,
                  dev_port);
            }
          }
        }
      } else {
        if (show_only) {
          bool print_banner = false;

          bf_pm_ucli_print_banner(port_hdl_ptr->conn_id, uc, &print_banner);
          port_diag_prbs_stats_display(dev_id, dev_port, uc);

        } else {
          for (int __ln = first_ln; __ln <= last_ln; __ln++) {
            sts = bf_serdes_tx_drv_attn_set(
                dev_id, dev_port, __ln, main_tap, post1, pre1);
            if (sts != BF_SUCCESS) {
              PM_ERROR(
                  "Error %d: Unable to set Tx taps for %d/%d ln%d (d_p: "
                  "%d:%3d)\n",
                  sts,
                  port_hdl_ptr->conn_id,
                  port_hdl_ptr->chnl_id,
                  __ln,
                  dev_id,
                  dev_port);
            }
          }
        }
      }
    }
  }
  bf_sys_free((void *)list);
  return;
}

static ucli_status_t bf_pm_ucli_ucli__tof3_port_tx_eq_set__(
    ucli_context_t *uc) {
  static char usage[] =
      "port-tx-eq3 [-a <conn_id/chnl>] <pre3 pre2 pre1 main post1>";
  UCLI_COMMAND_INFO(
      uc, "port-tx-eq3", -1, "<port_str> <pre3 pre2 pre1 main post1>");

  int show_only = false;
  int use_default = false;
  char *const *argv;
  int pre3 = -1, pre2 = -1, pre1 = -1, main_tap = -1, post1 = -1;

  serdes_uc = uc;

  show_only = 0;
  argv = (char *const *)&(uc->pargs->args__);

  int argc = (uc->pargs->count + 1);
  if (argc > 1) {
    if (argc > 6) {
      pre3 = atoi(argv[2]);
      pre2 = atoi(argv[3]);
      pre1 = atoi(argv[4]);
      main_tap = atoi(argv[5]);
      post1 = atoi(argv[6]);
    } else if (argc == 2) {
      show_only = true;
    } else if (argc == 3) {
      if (argv[2][0] == 'd') {
        use_default = true;
      }
    } else {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
  } else {
    show_only = true;
  }

  if (show_only) {
    aim_printf(
        &uc->pvs,
        "              Configured            | Maximum (sum <= 60)\n"
        "              p    p    p    m    p |  p    p    p    m    p\n"
        "              r    r    r    a    o |  r    r    r    a    o\n"
        "              e    e    e    i    s |  e    e    e    i    s\n"
        "Port |  ln |  3    2    1    n    t |  3    2    1    n    t\n"
        "-----+-----+----+----+----+----+----+----+----+----+----+----\n");
  }

  pm_ucli_tx_eq_set(uc,
                    show_only,
                    use_default,
                    argv[1],
                    -1 /*all lanes*/,
                    pre3,
                    pre2,
                    pre1,
                    main_tap,
                    post1,
                    0);  // no post2 in tf3
  if (use_default) {
    aim_printf(&uc->pvs, "Default settngs will be applied on next port-enb\n");
  }
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_tx_eq_set__(ucli_context_t *uc) {
  static char usage[] =
      "port-tx-eq [-a <conn_id/chnl>] <pre2 pre1 main post1 post2>";
  UCLI_COMMAND_INFO(
      uc, "port-tx-eq", -1, "<port_str> <pre2 pre1 main post1 post2>");

  int show_only = false;
  int use_default = false;
  char *const *argv;
  int pre2 = -1, pre1 = -1, main_tap = -1, post1 = -1, post2 = -1;

  serdes_uc = uc;

  show_only = 0;
  argv = (char *const *)&(uc->pargs->args__);

  int argc = (uc->pargs->count + 1);
  if (argc > 1) {
    if (argc > 6) {
      pre2 = atoi(argv[2]);
      pre1 = atoi(argv[3]);
      main_tap = atoi(argv[4]);
      post1 = atoi(argv[5]);
      post2 = atoi(argv[6]);
    } else if (argc == 2) {
      show_only = true;
    } else if (argc == 3) {
      if (argv[2][0] == 'd') {
        use_default = true;
      }
    } else {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
  } else {
    show_only = true;
  }

  pm_ucli_tx_eq_set(uc,
                    show_only,
                    use_default,
                    argv[1],
                    -1 /*all lanes*/,
                    0,  // no pre3 in tof2/1
                    pre2,
                    pre1,
                    main_tap,
                    post1,
                    post2);
  if (use_default) {
    aim_printf(&uc->pvs, "Default settngs will be applied on next port-enb\n");
  }
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__ln_tx_eq_set__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(
      uc, "lane-tx-eq", -1, "<port_str> ln <pre2 pre1 main post1 post2>");
  char *usage =
      "Usage : lane-tx-eq <port_str> ln [default | <pre2 pre1 main post1 "
      "post2>]\n";

  char *const *argv;
  int pre2 = 0, pre1 = 0, main_tap = 0, post1 = 0, post2 = 0;
  uint32_t ln;
  int argc = (uc->pargs->count + 1);
  bool show_only = false;
  bool use_default = false;

  argv = (char *const *)&(uc->pargs->args__);

  if (argc == 8) {
    if (argv[2][0] == '-') {
      ln = -1;
    } else {
      ln = atoi(argv[2]);
    }
    pre2 = atoi(argv[3]);
    pre1 = atoi(argv[4]);
    main_tap = atoi(argv[5]);
    post1 = atoi(argv[6]);
    post2 = atoi(argv[7]);
  } else if (argc == 3) {
    if (argv[2][0] == '-') {
      ln = -1;
    } else {
      ln = atoi(argv[2]);
    }
    show_only = true;
  } else if (argc == 4) {
    if (argv[2][0] == '-') {
      ln = -1;
    } else {
      ln = atoi(argv[2]);
    }
    if ((argv[3][0] == 'd') || (argv[3][0] == 'D')) {
      use_default = true;
    } else {
      aim_printf(&uc->pvs, "%s", usage);
      return 0;
    }
  } else {
    aim_printf(&uc->pvs, "%s", usage);
    return 0;
  }

  pm_ucli_tx_eq_set(uc,
                    show_only,
                    use_default,
                    argv[1],
                    ln,
                    0,  // no pre3 in tf2/1
                    pre2,
                    pre1,
                    main_tap,
                    post1,
                    post2);
  if (use_default) {
    aim_printf(&uc->pvs, "Default settngs will be applied on next port-enb\n");
  }
  return 0;
}

static void pm_ucli_tof2_err_inject_set(ucli_context_t *uc,
                                        bf_dev_id_t dev_id,
                                        bf_pal_front_port_handle_t *port_hdl,
                                        uint32_t ln,
                                        uint32_t n_errs) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      if (next_port_info_ptr->pltfm_port_info.log_mac_id == 0) {
        continue;
      }
      int num_lanes;

      bf_port_num_lanes_get(0, next_port_info_ptr->dev_port, &num_lanes);
      if ((int)ln < num_lanes) {
        bf_tof2_serdes_error_inject_set(
            dev_id, next_port_info_ptr->dev_port, ln, n_errs);
      }
    }
  }
  (void)uc;
}

static ucli_status_t bf_pm_ucli_ucli__tx_err_set__(ucli_context_t *uc) {
  static char usage[] = "port-tx-err [<conn_id/chnl>] <lane> <n_errs>";
  UCLI_COMMAND_INFO(uc, "port-tx-err", 3, "<lane> <n_errs>>");

  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  bf_status_t sts;
  uint32_t ln = 0, n_errs = 1;

  serdes_uc = uc;

  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  int argc = (uc->pargs->count + 1);
  if (argc > 1) {
    sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], &port_hdl);
    if (sts != BF_SUCCESS) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
    if (argc > 2) {
      ln = atoi(argv[2]);
    }
    if (argc > 3) {
      n_errs = atoi(argv[3]);
      if ((n_errs < 1) || (n_errs > 1000)) {
        aim_printf(&uc->pvs, "n_errs must be between 1-1000\n");
        return 0;
      }
    } else {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
  }

  // conn==-1 and chnl==-1 indicates that scan through all the ports but only
  // print the ports that have been added.
  //-a option indicates that the scan and print all the ports (whether added or
  // not)

  pm_ucli_tof2_err_inject_set(uc, dev_id, &port_hdl, ln, n_errs);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__ln_tx_polarity_set__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(
      uc, "lane-tx-pol", 3, "<port_str> ln <0=not inverted, 1=inverted>");
  char *const *argv;
  bf_status_t sts;
  uint32_t ln, inv;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  argv = (char *const *)&(uc->pargs->args__);

  ln = atoi(argv[2]);
  inv = atoi(argv[3]);

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get(argv[1], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    return 0;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    bool is_port_internal = false;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;

    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      int num_lanes;

      bf_port_num_lanes_get(dev_id, dev_port, &num_lanes);
      if ((int)ln >= num_lanes) {
        aim_printf(&uc->pvs, "Valid lanes are: 0-%d\n", num_lanes);
        return 0;
      }
      if (bf_lld_dev_is_tof1(dev_id)) {
        sts = bf_serdes_tx_drv_inv_set(dev_id, dev_port, ln, inv);
      } else if (bf_lld_dev_is_tof2(dev_id)) {
        sts = bf_tof2_serdes_tx_polarity_set(dev_id, dev_port, ln, inv, true);
      } else if (bf_lld_dev_is_tof3(dev_id)) {
        sts = bf_tof3_serdes_tx_polarity_set(dev_id, dev_port, ln, inv, true);
      }
      if (sts != BF_SUCCESS) {
        aim_printf(&uc->pvs,
                   "Error <%d> setting tx polarity <%s>",
                   sts,
                   inv ? "inverted" : "non-inverted");
      }
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__ln_rx_polarity_set__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(
      uc, "lane-rx-pol", 3, "<port_str> ln <0=not inverted, 1=inverted>");

  char *const *argv;
  bf_status_t sts;
  uint32_t ln, inv;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  argv = (char *const *)&(uc->pargs->args__);

  ln = atoi(argv[2]);
  inv = atoi(argv[3]);

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get(argv[1], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    return 0;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    bool is_port_internal = false;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;

    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      int num_lanes;

      bf_port_num_lanes_get(dev_id, dev_port, &num_lanes);
      if ((int)ln >= num_lanes) {
        aim_printf(&uc->pvs, "Valid lanes are: 0-%d\n", num_lanes);
        return 0;
      }
      if (bf_lld_dev_is_tof1(dev_id)) {
        sts = bf_serdes_rx_afe_inv_set(dev_id, dev_port, ln, inv);
      } else if (bf_lld_dev_is_tof2(dev_id)) {
        sts = bf_tof2_serdes_rx_polarity_set(dev_id, dev_port, ln, inv, true);
      } else if (bf_lld_dev_is_tof3(dev_id)) {
        sts = bf_tof3_serdes_rx_polarity_set(dev_id, dev_port, ln, inv, true);
      }
      if (sts != BF_SUCCESS) {
        aim_printf(&uc->pvs,
                   "Error <%d> setting rx polarity <%s>",
                   sts,
                   inv ? "inverted" : "non-inverted");
      }
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

extern void umac4_ctrs_rmon_dump(bf_dev_id_t dev_id,
                                 uint32_t umac,
                                 uint32_t ch);
extern void umac4_ctrs_pcs_dump(bf_dev_id_t dev_id, uint32_t umac, uint32_t ch);
extern void umac4_ctrs_rs_fec_dump(bf_dev_id_t dev_id,
                                   uint32_t umac,
                                   uint32_t ch);
extern void umac4_ctrs_pcs_vl_dump(bf_dev_id_t dev_id, uint32_t umac);
extern void umac4_ctrs_rs_fec_ln_dump(bf_dev_id_t dev_id, uint32_t umac);
extern void umac4_ctrs_fc_fec_ln_dump(bf_dev_id_t dev_id, uint32_t umac);

extern ucli_context_t *cur_uc;
void ucli_r_rmon_hdlr(ucli_context_t *uc,
                      bf_dev_id_t dev_id,
                      bf_dev_port_t dev_port);
int port_mgr_dump_tof1_pcs_ctrs(ucli_context_t *uc,
                                bf_dev_id_t dev_id,
                                bf_dev_port_t dev_port);
void ucli_fec_hdlr(ucli_context_t *uc,
                   bf_dev_id_t dev_id,
                   bf_dev_port_t dev_port);
void ucli_fec_hdlr_init(void);
void print_pcs_ctrs_banner(ucli_context_t *uc);

static ucli_status_t bf_pm_ucli_ucli__port_ctrs__(ucli_context_t *uc) {
  static char usage[] = "port-ctrs <conn_id/chnl> [pcs fec rmon]";
  UCLI_COMMAND_INFO(uc, "port-ctrs", 2, "<port_str> [pcs fec rmon]");

  char *const *argv;
  bf_status_t sts;
  char *ctr_typ = NULL;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;
  bool all = false;
  bool tof1_banner_init_needed = true;
  bool tof1_pcs_banner_needed = true;

  cur_uc = uc;

  argv = (char *const *)&(uc->pargs->args__);
  if ((argv[2][0] == 'r') || (argv[2][0] == 'R')) {
    ctr_typ = "rmon";
  } else if ((argv[2][0] == 'p') || (argv[2][0] == 'P')) {
    ctr_typ = "pcs";
  } else if ((argv[2][0] == 'f') || (argv[2][0] == 'F')) {
    ctr_typ = "fec";
  } else if ((argv[2][0] == 'a') || (argv[2][0] == 'A')) {
    ctr_typ = "all";
    all = true;
  } else {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get(argv[1], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    return 0;
  }

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    bool is_port_internal = false;
    bool is_port_added = false;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;

    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    bf_pm_is_port_added(dev_id, port_hdl_ptr, &is_port_added);
    if (!is_port_internal && is_port_added) {
      if (bf_lld_dev_is_tof1(dev_id)) {
        if (all || (strcmp("rmon", ctr_typ) == 0)) {
          aim_printf(&uc->pvs,
                     "\n\n%2d/%d : RMON Counters:\n",
                     port_hdl_ptr->conn_id,
                     port_hdl_ptr->chnl_id);
          ucli_r_rmon_hdlr(uc, dev_id, dev_port);
        }
        if (all || (strcmp("pcs", ctr_typ) == 0)) {
          // aim_printf(&uc->pvs,
          //         "\n\n%2d/%d : PCS  Counters:\n",
          //         port_hdl_ptr->conn_id,
          //         port_hdl_ptr->chnl_id);
          if (tof1_pcs_banner_needed) {
            print_pcs_ctrs_banner(uc);
            tof1_pcs_banner_needed = false;
          }
          port_mgr_dump_tof1_pcs_ctrs(uc, dev_id, dev_port);
        }
        if (all || (strcmp("fec", ctr_typ) == 0)) {
          // aim_printf(&uc->pvs,
          //         "\n\n%2d/%d : FEC  Counters:\n",
          //         port_hdl_ptr->conn_id,
          //         port_hdl_ptr->chnl_id);
          if (tof1_banner_init_needed) {
            ucli_fec_hdlr_init();
            tof1_banner_init_needed = false;
          }
          ucli_fec_hdlr(uc, dev_id, dev_port);
        }
      } else if (bf_lld_dev_is_tof2(dev_id)) {
        bf_status_t rc;
        uint32_t umac;
        uint32_t ch_ln;
        bool is_cpu_port;

        rc = port_mgr_tof2_map_dev_port_to_all(
            dev_id, dev_port, NULL, NULL, &umac, &ch_ln, &is_cpu_port);
        if (rc != BF_SUCCESS) continue;
        if (umac == 0) continue;
        if (!ctr_typ) continue;

        if (all || (strcmp("rmon", ctr_typ) == 0)) {
          aim_printf(&uc->pvs,
                     "\n\n%2d/%d : RMON Counters:\n",
                     port_hdl_ptr->conn_id,
                     port_hdl_ptr->chnl_id);
          umac4_ctrs_rmon_dump(0, umac, ch_ln);
        }
        if (all || (strcmp("pcs", ctr_typ) == 0)) {
          aim_printf(&uc->pvs,
                     "\n\n%2d/%d : PCS  Counters:\n",
                     port_hdl_ptr->conn_id,
                     port_hdl_ptr->chnl_id);
          umac4_ctrs_pcs_dump(0, umac, ch_ln);
          aim_printf(&uc->pvs,
                     "\n\n%2d/%d : PCS Virtual Lane Counters:\n",
                     port_hdl_ptr->conn_id,
                     port_hdl_ptr->chnl_id);
          umac4_ctrs_pcs_vl_dump(0, umac);
        }
        if (all || (strcmp("fec", ctr_typ) == 0)) {
          aim_printf(&uc->pvs,
                     "\n\n%2d/%d : RS FEC  Counters:\n",
                     port_hdl_ptr->conn_id,
                     port_hdl_ptr->chnl_id);
          umac4_ctrs_rs_fec_dump(0, umac, ch_ln);
          aim_printf(&uc->pvs,
                     "\n\n%2d/%d : RS FEC Per Lane Counters:\n",
                     port_hdl_ptr->conn_id,
                     port_hdl_ptr->chnl_id);
          umac4_ctrs_rs_fec_ln_dump(0, umac);
          aim_printf(&uc->pvs,
                     "\n\n%2d/%d : FC FEC Per Lane Counters:\n",
                     port_hdl_ptr->conn_id,
                     port_hdl_ptr->chnl_id);
          umac4_ctrs_fc_fec_ln_dump(0, umac);
        }
      }
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

extern void port_mgr_ucli_dump_port_config(ucli_context_t *uc,
                                           uint32_t conn_id,
                                           uint32_t chnl_id,
                                           bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port);

static ucli_status_t bf_pm_ucli_ucli__port_cfg__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "port-cfg", 1, "<port_str>");

  char *const *argv;
  bf_status_t sts;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  uint32_t list_sz = 0;
  bf_pal_front_port_handle_t *list = NULL;

  cur_uc = uc;

  argv = (char *const *)&(uc->pargs->args__);

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get(argv[1], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    return 0;
  }

  aim_printf(&uc->pvs,
             "-------+---+-----+-------+-------+---------+---------+-----------"
             "----+---------------+---------+--------+\n");
  aim_printf(&uc->pvs,
             "       |   |     | Tx    | Rx    | Tx      | Rx      | Tx        "
             "    | Rx            |         |          |\n");
  aim_printf(&uc->pvs,
             " Port  |dev| D_P | Pause | Pause | PFC     | PFC     | MTU       "
             "    | MTU           | IFG     |Preamble|\n");
  aim_printf(&uc->pvs,
             "-------+---+-----+-------+-------+---------+---------+-----------"
             "----+---------------+---------+--------+\n");

  for (uint32_t i = 0; i < list_sz; i++) {
    bf_dev_id_t dev_id;
    bf_dev_port_t dev_port;
    bool is_port_internal = false;
    bool is_port_added = false;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;

    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    bf_pm_is_port_added(dev_id, port_hdl_ptr, &is_port_added);
    if (is_port_added) {
      port_mgr_ucli_dump_port_config(
          uc, port_hdl_ptr->conn_id, port_hdl_ptr->chnl_id, dev_id, dev_port);
    }
  }
  bf_sys_free((void *)list);
  return 0;
}

struct timeval g_tm;

struct timeval *print_ts(ucli_context_t *uc, bool do_print) {
  struct timeval *tm = &g_tm;
  char tbuf[256] = {0};
  char ubuf[256] = {0};
  struct tm *loctime;

  gettimeofday(&g_tm, NULL);
  loctime = localtime(&tm->tv_sec);

  if (loctime == NULL) {
    return tm;
  }

  if (do_print) {
    strftime(tbuf, sizeof(tbuf), "%a %b %d", loctime);
    aim_printf(&uc->pvs, "\n%s ", tbuf);

    strftime(ubuf, sizeof(ubuf), "%T\n", loctime);
    ubuf[strlen(ubuf) - 1] = 0;  // remove CR
    aim_printf(&uc->pvs, "\n%s.%06d : \n", ubuf, (int)tm->tv_usec);
  }

  return tm;
}

//#define ERROR_PROFILING
#undef ERROR_PROFILING
#ifdef ERROR_PROFILING
extern void umac4_ctrs_rs_fec_ln_get(bf_dev_id_t dev_id,
                                     uint32_t umac,
                                     umac4_rs_fec_ln_ctr_t *ctrs);

#define MAX_SAMPLES 10000
#define MAX_D_P 450
umac4_rs_fec_ln_ctr_t profile_ctrs[MAX_SAMPLES][MAX_D_P];
uint32_t sample_time[MAX_SAMPLES][MAX_D_P];
uint32_t d_p_list[MAX_D_P] = {0};
uint32_t f_p_list[MAX_D_P] = {0};
int next_d_p = 0;

static void pm_ucli_tof2_port_err_profile(ucli_context_t *uc,
                                          bf_dev_id_t dev_id,
                                          bf_pal_front_port_handle_t *port_hdl,
                                          int sample,
                                          int n_samples,
                                          struct timeval *tm,
                                          bool prbs_mode) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      if (next_port_info_ptr->pltfm_port_info.log_mac_id == 0) {
        continue;
      }
      bf_status_t rc;
      uint32_t umac;
      uint32_t ch_ln, time_of_sample;
      bool is_cpu_port;
      struct timeval tm2, elapsed;

      rc = port_mgr_tof2_map_dev_port_to_all(dev_id,
                                             next_port_info_ptr->dev_port,
                                             NULL,
                                             NULL,
                                             &umac,
                                             &ch_ln,
                                             &is_cpu_port);
      if (rc != BF_SUCCESS) continue;

      if (sample == 0) {
        d_p_list[next_d_p] = next_port_info_ptr->dev_port;
        ;
        f_p_list[next_d_p++] =
            (next_port_info_ptr->pltfm_port_info.port_hdl.conn_id << 16) |
            next_port_info_ptr->pltfm_port_info.port_hdl.chnl_id;

        // pause serdes FW
        bf_tof2_serdes_fw_reg_section_wr(
            dev_id, next_port_info_ptr->dev_port, ch_ln, 128, 0, 0);
        // read once to clear
        umac4_ctrs_rs_fec_ln_get(
            0, umac, &profile_ctrs[0][next_port_info_ptr->dev_port]);
        if (prbs_mode) {
          int num_lanes;

          bf_port_num_lanes_get(0, next_port_info_ptr->dev_port, &num_lanes);
          for (int ln = 0; ln < num_lanes; ln++) {
            bf_tof2_serdes_prbs_rst_set(
                dev_id, next_port_info_ptr->dev_port, ln);
          }
        }
      }

      if (prbs_mode) {
        int num_lanes;

        bf_port_num_lanes_get(0, next_port_info_ptr->dev_port, &num_lanes);
        for (int ln = 0; ln < num_lanes; ln++) {
          uint32_t err_cnt;

          bf_tof2_serdes_rx_prbs_err_get(
              dev_id, next_port_info_ptr->dev_port, ln, &err_cnt);
          bf_tof2_serdes_prbs_rst_set(dev_id, next_port_info_ptr->dev_port, ln);
          uint32_t *ctr =
              (uint32_t *)&profile_ctrs[sample][next_port_info_ptr->dev_port];
          ctr[ln * 2] = err_cnt;
        }
      } else {
        umac4_ctrs_rs_fec_ln_get(
            0, umac, &profile_ctrs[sample][next_port_info_ptr->dev_port]);
      }

      tm2 = *(print_ts(uc, false));
      timersub(&tm2, tm, &elapsed);
      time_of_sample = elapsed.tv_usec;
      if (elapsed.tv_sec > 0) {
        time_of_sample += (elapsed.tv_sec * 1000000);
      }
      sample_time[sample][next_port_info_ptr->dev_port] = time_of_sample;

      if (sample == (n_samples - 1)) {
        // un-pause serdes FW
        bf_tof2_serdes_fw_reg_section_wr(
            dev_id, next_port_info_ptr->dev_port, ch_ln, 128, 0xffff, 0);
      }
    }
  }
  (void)uc;
}

extern ucli_context_t *cur_uc;

static ucli_status_t bf_pm_ucli_ucli__port_errs__(ucli_context_t *uc) {
  static char usage[] = "port-errs <conn_id/chnl> [<cnt> [prbs]]";
  UCLI_COMMAND_INFO(uc, "port-errs", -1, "<port_str> [<cnt> [prbs]]");

  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  bf_status_t sts;
  int n_samples;
  struct timeval tm1;
  bool prbs_mode;

  cur_uc = uc;  // save for callback

  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }
  n_samples = MAX_SAMPLES;
  prbs_mode = false;
  int argc = (uc->pargs->count + 1);
  if (argc > 1) {
    sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], &port_hdl);
    if (sts != BF_SUCCESS) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
    if (argc > 2) {
      n_samples = atoi(argv[2]);
      if (n_samples > MAX_SAMPLES) {
        n_samples = MAX_SAMPLES;
      }
    }
    if (argc > 3) {
      prbs_mode = true;
    }
  } else {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }

  // conn==-1 and chnl==-1 indicates that scan through all the ports but only
  // print the ports that have been added.
  //-a option indicates that the scan and print all the ports (whether added or
  // not)

  float temperature;
  int temp_tries = 3;

  do {
    bf_tof2_serdes_fw_temperature_get(dev_id, 136, &temperature);
  } while ((temp_tries-- > 0) &&
           ((temperature == 0.0) || (temperature == -0.0)));

  // init the D_P List
  memset((char *)d_p_list, 0, sizeof(d_p_list));
  memset((char *)f_p_list, 0, sizeof(f_p_list));
  next_d_p = 0;

  aim_printf(&uc->pvs,
             "sample,FP,D_P,microsec,Temp_C,l0,l1,l2,l3,l4,l5,l6,l7,l8,l9,l10,"
             "l11,l12,l13,l14,l15,\n");
  tm1 = *(print_ts(uc, false));
  for (int s = 0; s < n_samples; s++) {
    pm_ucli_tof2_port_err_profile(
        uc, dev_id, &port_hdl, s, n_samples, &tm1, prbs_mode);
  }

  for (int d_p_idx = 0; d_p_idx < next_d_p; d_p_idx++) {
    bf_dev_port_t d_p = d_p_list[d_p_idx];
    uint32_t conn_id = f_p_list[d_p_idx] >> 16;
    uint32_t chnl_id = f_p_list[d_p_idx] & 0xf;

    for (int s = 0; s < n_samples; s++) {
      uint64_t *ctr_p = (uint64_t *)&profile_ctrs[s][d_p];
      aim_printf(&uc->pvs,
                 "%d,%d/%d,%d,%d,%f,",
                 s,
                 conn_id,
                 chnl_id,
                 d_p,
                 sample_time[s][d_p],
                 temperature);
      for (int c = 0; c < 16; c++) {
        aim_printf(&uc->pvs, "%" PRIu64 ",", *ctr_p);
        ctr_p++;
      }
      aim_printf(&uc->pvs, "\n");
    }
  }
  return 0;
}

typedef struct port_mon_t {
  // RS
  int live_link_state;
  int remote_fault;
  int local_fault;
  // PCS
  uint32_t ber_cnt;
  uint32_t errored_blk_cnt;
  uint32_t sync_loss;
  uint32_t block_lock_loss;
  uint32_t hi_ber_cnt;
  uint32_t valid_error_cnt;
  uint32_t unknown_error_cnt;
  uint32_t invalid_error_cnt;
  uint32_t bip_errors_per_pcs_lane[20];
  // FEC
  uint32_t hi_ser, fec_align_status, fec_corr_cnt, fec_uncorr_cnt,
      fec_ser_lane_0, fec_ser_lane_1, fec_ser_lane_2, fec_ser_lane_3,
      fec_ser_lane_4, fec_ser_lane_5, fec_ser_lane_6, fec_ser_lane_7;
  // serdes
  struct {
    bool los, ei, flock, failed;
  } ln[8];
} port_mon_t;

/*
bf_serdes_get_los(dev_id, dev_port, ln, &los);
bf_serdes_elec_idle_get(dev_id, dev_port, ln, &ei);
bf_serdes_frequency_lock_get(dev_id, dev_port, ln, &flock);
bf_serdes_calibration_status_get(dev_id, dev_port, ln, &failed);
*/

port_mon_t port_mon[BF_PORT_COUNT] = {{0}};

extern void port_mgr_mac_int_fault_get(bf_dev_id_t dev_id,
                                       int mac_blk,
                                       int ch,
                                       int *remote_fault,
                                       int *local_fault);
extern int port_mgr_mac_live_link_state(bf_dev_id_t dev_id,
                                        int mac_block,
                                        int channel);

void monitor_port(ucli_context_t *uc,
                  bf_dev_id_t dev_id,
                  bf_dev_port_t dev_port) {
  uint32_t ber_cnt;
  uint32_t errored_blk_cnt;
  uint32_t sync_loss;
  uint32_t block_lock_loss;
  uint32_t hi_ber_cnt;
  uint32_t valid_error_cnt;
  uint32_t unknown_error_cnt;
  uint32_t invalid_error_cnt;
  uint32_t bip_errors_per_pcs_lane[20];
  bf_status_t rc;
  port_mon_t *pm = (port_mon_t *)&port_mon[dev_port];
  bool update = false;

  // Hold port "Dn" if too many PCS errors
  rc = bf_port_pcs_counters_get(dev_id,
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
  if (rc != BF_SUCCESS) {
    return;
  }
  if ((0 != ber_cnt) || (0 != errored_blk_cnt) || (0 != sync_loss) ||
      (0 != block_lock_loss) || (0 != hi_ber_cnt) || (0 != valid_error_cnt) ||
      (0 != unknown_error_cnt) || (0 != invalid_error_cnt)) {
    update = true;
  }
  uint32_t fec_corr_cnt, fec_uncorr_cnt, fec_ser_lane_0, fec_ser_lane_1,
      fec_ser_lane_2, fec_ser_lane_3, fec_ser_lane_4, fec_ser_lane_5,
      fec_ser_lane_6, fec_ser_lane_7;
  bool hi_ser, fec_align_status;

  /* down collect during down state */
  rc = bf_port_rs_fec_status_and_counters_get(dev_id,
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
  if (rc != BF_SUCCESS) {
    return;
  }

  if (bf_lld_dev_is_tof1(dev_id)) {
    if ((pm->hi_ser != hi_ser) || (pm->fec_align_status != fec_align_status) ||
        (0 != fec_corr_cnt) || (0 != fec_uncorr_cnt) || (0 != fec_ser_lane_0) ||
        (0 != fec_ser_lane_1) || (0 != fec_ser_lane_2) ||
        (0 != fec_ser_lane_3)) {
      update = true;
    }
  } else if (bf_lld_dev_is_tof2(dev_id)) {
    if ((pm->hi_ser != hi_ser) || (pm->fec_align_status != fec_align_status) ||
        (0 != fec_corr_cnt) || (0 != fec_uncorr_cnt) || (0 != fec_ser_lane_0) ||
        (0 != fec_ser_lane_1) || (0 != fec_ser_lane_2) ||
        (0 != fec_ser_lane_3) || (0 != fec_ser_lane_4) ||
        (0 != fec_ser_lane_5) || (0 != fec_ser_lane_6) ||
        (0 != fec_ser_lane_7)) {
      update = true;
    }
  }

  uint32_t umac, ch;
  rc = port_mgr_tof2_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch, NULL);
  if (rc != BF_SUCCESS) {
    return;
  }
  int remote_fault;
  int local_fault;
  int live_link_state;
  port_mgr_mac_int_fault_get(dev_id, umac, ch, &local_fault, &remote_fault);
  live_link_state = port_mgr_mac_live_link_state(dev_id, umac, ch);

  if ((pm->live_link_state != live_link_state) ||
      (pm->local_fault != local_fault) || (pm->remote_fault != remote_fault)) {
    update = true;
  }

  int ln, num_lanes;
  bf_port_num_lanes_get(dev_id, dev_port, &num_lanes);
  for (ln = 0; ln < num_lanes; ln++) {
    bool los, ei, flock, failed;

    bf_serdes_get_los(dev_id, dev_port, ln, &los);
    bf_serdes_elec_idle_get(dev_id, dev_port, ln, &ei);
    bf_serdes_frequency_lock_get(dev_id, dev_port, ln, &flock);
    bf_serdes_calibration_status_get(dev_id, dev_port, ln, &failed);
    if ((los != pm->ln[ln].los) || (ei != pm->ln[ln].ei) ||
        /*(flock != pm->ln[ln].flock) || too noisy */
        (failed != pm->ln[ln].failed)) {
      update = true;
      pm->ln[ln].los = los;
      pm->ln[ln].ei = ei;
      pm->ln[ln].flock = flock;
      pm->ln[ln].failed = failed;
    }
  }

  if (update) {
    pm->ber_cnt = ber_cnt;
    pm->errored_blk_cnt = errored_blk_cnt;
    pm->sync_loss = sync_loss;
    pm->block_lock_loss = block_lock_loss;
    pm->hi_ber_cnt = hi_ber_cnt;
    pm->valid_error_cnt = valid_error_cnt;
    pm->unknown_error_cnt = unknown_error_cnt;
    pm->invalid_error_cnt = invalid_error_cnt;

    pm->hi_ser = hi_ser;
    pm->fec_align_status = fec_align_status;
    pm->fec_corr_cnt = fec_corr_cnt;
    pm->fec_uncorr_cnt = fec_uncorr_cnt;
    pm->fec_ser_lane_0 = fec_ser_lane_0;
    pm->fec_ser_lane_1 = fec_ser_lane_1;
    pm->fec_ser_lane_2 = fec_ser_lane_2;
    pm->fec_ser_lane_3 = fec_ser_lane_3;

    if (bf_lld_dev_is_tof2(dev_id)) {
      pm->fec_ser_lane_4 = fec_ser_lane_4;
      pm->fec_ser_lane_5 = fec_ser_lane_5;
      pm->fec_ser_lane_6 = fec_ser_lane_6;
      pm->fec_ser_lane_7 = fec_ser_lane_7;
    }
    pm->live_link_state = live_link_state;
    pm->local_fault = local_fault;
    pm->remote_fault = remote_fault;
    {
      struct timeval tv;
      struct tm *loctime;
      char ubuf[132], tbuf[132];

      gettimeofday(&tv, NULL);
      loctime = localtime(&tv.tv_sec);
      strftime(tbuf, sizeof(tbuf), "%a %b %d", loctime);
      aim_printf(&uc->pvs, "%s ", tbuf);
      strftime(ubuf, sizeof(ubuf), "%T\n", loctime);
      ubuf[strlen(ubuf) - 1] = 0;  // remove CR
      aim_printf(&uc->pvs, "%s.%06d ::\n", ubuf, (int)tv.tv_usec);
    }
    aim_printf(&uc->pvs,
               "MON: %d:%3d: RS : st=%d lf=%d rf=%d\n",
               dev_id,
               dev_port,
               pm->live_link_state,
               pm->local_fault,
               pm->remote_fault);
    aim_printf(&uc->pvs,
               "MON: %d:%3d: PCS: ber=%d eblk=%d sync=%d blklk=%d hiber=%d "
               "vld=%d unk=%d inv=%d\n",
               dev_id,
               dev_port,
               pm->ber_cnt,
               pm->errored_blk_cnt,
               pm->sync_loss,
               pm->block_lock_loss,
               pm->hi_ber_cnt,
               pm->valid_error_cnt,
               pm->unknown_error_cnt,
               pm->invalid_error_cnt);
    if (bf_lld_dev_is_tof1(dev_id)) {
      aim_printf(&uc->pvs,
                 "MON: %d:%3d: FEC: algn=%d hiser=%d uncorr=%d corr=%d ser0=%d "
                 "ser1=%d ser2=%d ser3=%d\n",
                 dev_id,
                 dev_port,
                 pm->fec_align_status,
                 pm->hi_ser,
                 pm->fec_uncorr_cnt,
                 pm->fec_corr_cnt,
                 pm->fec_ser_lane_0,
                 pm->fec_ser_lane_1,
                 pm->fec_ser_lane_2,
                 pm->fec_ser_lane_3);
    } else if (bf_lld_dev_is_tof1(dev_id)) {
      aim_printf(&uc->pvs,
                 "MON: %d:%3d: FEC: algn=%d hiser=%d uncorr=%d corr=%d ser0=%d "
                 "ser1=%d ser2=%d ser3=%d ser4=%d ser5=%d ser6=%d ser7=%d \n",
                 dev_id,
                 dev_port,
                 pm->fec_align_status,
                 pm->hi_ser,
                 pm->fec_uncorr_cnt,
                 pm->fec_corr_cnt,
                 pm->fec_ser_lane_0,
                 pm->fec_ser_lane_1,
                 pm->fec_ser_lane_2,
                 pm->fec_ser_lane_3,
                 pm->fec_ser_lane_4 pm->fec_ser_lane_5 pm->fec_ser_lane_6 pm
                     ->fec_ser_lane_7);
    }
    for (ln = 0; ln < num_lanes; ln++) {
      aim_printf(&uc->pvs,
                 "MON: %d:%3d: SDS: ln[%d] : los=%d ei=%d flock=%d failed=%d\n",
                 dev_id,
                 dev_port,
                 ln,
                 pm->ln[ln].los,
                 pm->ln[ln].ei,
                 pm->ln[ln].flock,
                 pm->ln[ln].failed);
    }
  }
}

static void pm_ucli_port_mon_fec_counters(
    ucli_context_t *uc,
    bf_dev_id_t dev_id,
    int aflag,
    bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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

    if (!next_port_info_ptr->is_added) {
      if (aflag) {
        // Indicates that this port has not been added yet
      }
    } else { /* clear FC/RS fec counters */
      bf_dev_port_t d_p = next_port_info_ptr->dev_port;
      monitor_port(uc, dev_id, d_p);
    }
  }
}

static ucli_status_t bf_pm_ucli_ucli__port_error_mon__(ucli_context_t *uc) {
  static char usage[] = "port-error-mon -a -p <conn_id/chnl>";
  UCLI_COMMAND_INFO(uc, "port-error-mon", -1, "-a -p <port_str>");

  extern char *optarg;
  extern int optind;
  int c, pflag, aflag, dflag;
  char *p_arg;
  int argc;
  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  bf_status_t sts;

  pflag = aflag = dflag = 0;
  p_arg = NULL;
  optind = 0;  // reset optind value
  argc = (uc->pargs->count + 1);
  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  dev_id = 0;
  if (!(bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof2(dev_id))) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }
  while ((c = getopt(argc, argv, "p:a")) != -1) {
    switch (c) {
      case 'p':
        pflag = 1;
        p_arg = optarg;
        if (!p_arg) {
          aim_printf(&uc->pvs, "Usage : %s\n", usage);
          return UCLI_STATUS_OK;
        }
        sts = bf_pm_port_str_to_hdl_get(dev_id, p_arg, &port_hdl);
        if (sts != BF_SUCCESS) {
          aim_printf(&uc->pvs, "Usage : %s\n", usage);
          return 0;
        }
        break;
      case 'a':
        aflag = 1;
        break;
    }
  }

  /* disable port FSM */
  pm_tasklet_free_run_set(false);
  /* disable bkg stats polling */
  pm_port_stats_poll_stop(dev_id);

  for (;;) {
    /* monitor error counters/status */
    pm_ucli_port_mon_fec_counters(uc, dev_id, aflag, &port_hdl);
    bf_sys_usleep(10 * 1000);
  }

  (void)pflag;
  return 0;
}

uint32_t uncorr_ctr_ofs = 0;
uint32_t fec_ln_ctr_ofs = 0;
uint64_t uncorr = 0;
struct timeval burst_start, burst_end, elapsed, t0, t1;
uint32_t burst_len = 0;
uint64_t burst[32768] = {0ul};
uint64_t burst2[32768] = {0ul};

#if DEBUG_BURST_ERRORS
extern uintptr_t bf_switchd_get_dev_base(bf_dev_id_t dev_id);
#endif

uintptr_t dev_base = 0;

inline static void bf_switchd_reg_rd_FAST(uint32_t addr, uint32_t *data) {
  *data = *(volatile uint32_t *)(dev_base + addr);
}

#if 1
static ucli_status_t bf_pm_ucli_ucli__port_burst__(ucli_context_t *uc) {
  static char usage[] = "port-burst <dev_port>";
  UCLI_COMMAND_INFO(uc, "port-burst", 1, "<dev_port>");

  char *const *argv;
  bf_dev_id_t dev_id;
  bf_dev_port_t dev_port;
  uint32_t umac, ch;

  cur_uc = uc;  // save for callback

  argv = (char *const *)&(uc->pargs->args__);

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }
  int argc = (uc->pargs->count + 1);
  if (argc > 1) {
    dev_port = atoi(argv[1]);
  } else {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }
  int rc = port_mgr_tof2_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch, NULL);
  if (rc != 0) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }
  uint64_t cnt, cnt2;
  uint32_t lo, hi;
  uint64_t cur_uncorr;

  float temperature;
  int temp_tries = 3;

  do {
    bf_tof2_serdes_temperature_get(dev_id, dev_port, &temperature);
  } while ((temp_tries-- > 0) &&
           ((temperature == 0.0) || (temperature == -0.0)));

#if DEBUG_BURST_ERRORS
  dev_base = bf_switchd_get_dev_base(dev_id);
#else
  if (true) {
    int you_need_to_compile_with_DEBUG_BURST_ERRORS = 0;
    bf_sys_assert(you_need_to_compile_with_DEBUG_BURST_ERRORS);
    (void)dev_id;
    return 0;
  }
#endif
  uncorr_ctr_ofs = umac4_ctrs_rs_fec_ctr_address_get(0, umac, ch, 1);
  fec_ln_ctr_ofs = umac4_ctrs_rs_fec_ln_ctr_address_get(0, umac, ch * 2);
  // get starting uncorrectable error count
  bf_switchd_reg_rd_FAST(uncorr_ctr_ofs, &lo);
  uncorr = (uint64_t)lo;
  cur_uncorr = uncorr;
  burst_len = 0;
  bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs, &lo);
  bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs + 4, &hi);
  burst[burst_len] = ((uint64_t)hi << 32ul) | (uint64_t)lo;
  cnt = ((uint64_t)hi << 32ul) | (uint64_t)lo;

  bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs + 8, &lo);
  bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs + 12, &hi);
  burst2[burst_len] = ((uint64_t)hi << 32ul) | (uint64_t)lo;
  cnt2 = ((uint64_t)hi << 32ul) | (uint64_t)lo;

  aim_printf(&uc->pvs,
             "Start unCorr=%" PRIu64 ": Corr=%" PRIu64 " : %" PRIu64 "\n",
             uncorr,
             cnt,
             cnt2);

  // debug
  uint64_t pass = 0ul;
  uint32_t last_entry = 0;
  uint32_t post_trigr_cnt = 0;

  while ((cur_uncorr == uncorr) || (post_trigr_cnt < 16384)) {
    bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs, &lo);
    if (lo < (uint32_t)(burst[last_entry] & 0xffffffff)) {
      bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs + 4, &hi);
    } else {
      hi = (uint32_t)(burst[last_entry] >> 32ul);
    }
    cnt = ((uint64_t)hi << 32ul) | (uint64_t)lo;

    bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs + 8, &lo);
    if (lo < (uint32_t)(burst2[last_entry] & 0xffffffff)) {
      bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs + 12, &hi);
    } else {
      hi = (uint32_t)(burst2[last_entry] >> 32ul);
    }
    cnt2 = ((uint64_t)hi << 32ul) | (uint64_t)lo;
    if ((cnt - burst[last_entry] == 0ul) &&
        (cnt2 - burst2[last_entry] == 0ul)) {  // no new errs
    } else if (((cnt - burst[last_entry]) + (cnt2 - burst2[last_entry])) > 15) {
      bf_switchd_reg_rd_FAST(uncorr_ctr_ofs, &lo);  // check again
      cur_uncorr = (uint64_t)lo;
      post_trigr_cnt = 1;
    }
    last_entry = (last_entry + 1) & 0x7ffful;  //% 16384
    burst[last_entry] = cnt;
    burst2[last_entry] = cnt2;
    if (post_trigr_cnt) post_trigr_cnt++;
    if (++pass > 10002ul)
      continue;  // skip timer calibration
    else {
      if (pass == 1ul) {
        gettimeofday(&t0, NULL);
      } else if (pass == 10001ul) {
        gettimeofday(&t1, NULL);
      }
    }
  }
  bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs, &lo);
  bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs + 4, &hi);
  cnt = ((uint64_t)hi << 32ul) | (uint64_t)lo;
  bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs + 8, &lo);
  bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs + 12, &hi);
  cnt2 = ((uint64_t)hi << 32ul) | (uint64_t)lo;

  timersub(&t1, &t0, &elapsed);
  uint64_t elapsed_usec = (elapsed.tv_sec * 1000000) + elapsed.tv_usec;
  uint64_t usec_per_row = elapsed_usec / 10000;
  float t_per_row = (float)elapsed_usec / 10000.0;

  aim_printf(&uc->pvs,
             "Start unCorr=%" PRIu64 " : Current unCorr=%" PRIu64
             " : freq=%" PRIu64 " us : exact freq: %f\n",
             uncorr,
             cur_uncorr,
             usec_per_row,
             t_per_row);

  aim_printf(&uc->pvs,
             "sample,FP,D_P,microsec,Temp_C,l0,l1,l2,l3,l4,l5,l6,l7,l8,l9,l10,"
             "l11,l12,l13,l14,l15,\n");
  uint32_t i;
  for (i = 1; i < 32768; i++) {
    int prev_entry = (last_entry + i) & 0x7ffful;
    ;
    int entry = (last_entry + 1 + i) & 0x7ffful;
    aim_printf(&uc->pvs,
               "%d,30/4,%d,%" PRIu64 ",%f,%" PRIu64 ",%" PRIu64 ",%" PRIu64
               ",0,0,0,0,0,0,0,0,0,0,0,0,0\n",
               i,
               dev_port,
               (i * usec_per_row),
               temperature,
               burst[entry] - burst[prev_entry],
               burst2[entry] - burst2[prev_entry],
               (burst[entry] - burst[prev_entry]) +
                   (burst2[entry] - burst2[prev_entry]));
  }
  aim_printf(&uc->pvs,
             "last,%" PRIu64 ",%" PRIu64 "\n",
             cnt - burst[last_entry],
             cnt2 - burst2[last_entry]);

  return 0;
}

#else

static ucli_status_t bf_pm_ucli_ucli__port_burst__(ucli_context_t *uc) {
  static char usage[] = "port-burst <dev_port>";
  UCLI_COMMAND_INFO(uc, "port-burst", 1, "<dev_port>");

  char *const *argv;
  bf_dev_id_t dev_id;
  bf_dev_port_t dev_port;
  uint32_t umac, ch;

  cur_uc = uc;  // save for callback

  argv = (char *const *)&(uc->pargs->args__);

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }
  int argc = (uc->pargs->count + 1);
  if (argc > 1) {
    dev_port = atoi(argv[1]);
  } else {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }
  int rc = port_mgr_tof2_map_dev_port_to_all(
      dev_id, dev_port, NULL, NULL, &umac, &ch, NULL);
  if (rc != 0) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }
  uint64_t cnt, cnt2;
  uint32_t lo, hi;
  uint64_t cur_uncorr;

#if DEBUG_BURST_ERRORS
  dev_base = bf_switchd_get_dev_base(dev_id);
#else
  if (true) {
    int you_need_to_compile_with_DEBUG_BURST_ERRORS = 0;
    bf_sys_assert(you_need_to_compile_with_DEBUG_BURST_ERRORS);
    (void)dev_id;
    return 0;
  }
#endif
  uncorr_ctr_ofs = umac4_ctrs_rs_fec_ctr_address_get(0, umac, ch, 1);
  fec_ln_ctr_ofs = umac4_ctrs_rs_fec_ln_ctr_address_get(0, umac, ch * 2);
  // get starting uncorrectable error count
  bf_switchd_reg_rd_FAST(uncorr_ctr_ofs, &lo);
  uncorr = (uint64_t)lo;
  cur_uncorr = uncorr;
  burst_len = 0;
  bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs, &lo);
  bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs + 4, &hi);
  burst[burst_len] = ((uint64_t)hi << 32ul) | (uint64_t)lo;
  bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs + 8, &lo);
  bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs + 12, &hi);
  burst2[burst_len] = ((uint64_t)hi << 32ul) | (uint64_t)lo;

  aim_printf(
      &uc->pvs,
      "Start unCorr=%ld : %ld : %ld : fec_ln_ofs : %08x uncorr-ofs : %08x\n",
      uncorr,
      burst[burst_len],
      burst2[burst_len],
      fec_ln_ctr_ofs,
      uncorr_ctr_ofs);

  // debug
  uint32_t max_polls = 100000000;
  uint64_t prior_0s = 0ul;

  while (cur_uncorr == uncorr) {
    bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs, &lo);
    if (lo < (uint32_t)(burst[burst_len] & 0xffffffff)) {
      bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs + 4, &hi);
    } else {
      hi = (uint32_t)(burst[burst_len] >> 32ul);
    }
    cnt = ((uint64_t)hi << 32ul) | (uint64_t)lo;

    bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs + 8, &lo);
    if (lo < (uint32_t)(burst2[burst_len] & 0xffffffff)) {
      bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs + 12, &hi);
    } else {
      hi = (uint32_t)(burst2[burst_len] >> 32ul);
    }
    cnt2 = ((uint64_t)hi << 32ul) | (uint64_t)lo;
    if ((cnt - burst[burst_len] == 0ul) &&
        (cnt2 - burst2[burst_len] == 0ul)) {  // no new errs
      if (burst_len > 0) {
        burst_len = 0;  // burst ended
        prior_0s = 0ul;
      }
      prior_0s++;
    } else {
      if (burst_len == 0) {
        // get burst start time
        gettimeofday(&burst_start, NULL);
      }
      burst[burst_len + 1] = cnt;
      burst2[burst_len + 1] = cnt2;
      burst_len++;
      bf_switchd_reg_rd_FAST(uncorr_ctr_ofs, &lo);  // check again
      cur_uncorr = (uint64_t)lo;
    }
    // debug
    if (cur_uncorr != uncorr) break;
    if (--max_polls == 0) {
      aim_printf(&uc->pvs, "Didn't happen. try again.. \n");
      return 0;
    }
  }
  // get uncorr time
  gettimeofday(&burst_end, NULL);
  timersub(&burst_end, &burst_start, &elapsed);
  // time_of_sample = elapsed.tv_usec;
  // if (elapsed.tv_sec > 0) {
  //  time_of_sample += (elapsed.tv_sec * 1000000);
  //}
  uint64_t elapsed_usec = (elapsed.tv_sec * 1000000) + elapsed.tv_usec;
  if (burst_len == 0) {
    aim_printf(&uc->pvs, "burst len = 0?\n");
    return 0;
  }
  bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs, &lo);
  bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs + 4, &hi);
  cnt = ((uint64_t)hi << 32ul) | (uint64_t)lo;
  bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs + 8, &lo);
  bf_switchd_reg_rd_FAST(fec_ln_ctr_ofs + 12, &hi);
  cnt2 = ((uint64_t)hi << 32ul) | (uint64_t)lo;

  uint64_t usec_per_row = elapsed_usec / burst_len;
  aim_printf(&uc->pvs,
             "Start unCorr=%ld : Current unCorr=%ld : burst len=%d : freq=%ld "
             "us : prior zeros: %ld\n",
             uncorr,
             cur_uncorr,
             burst_len,
             usec_per_row,
             prior_0s);

  aim_printf(&uc->pvs, "\ntime,ser_ln0,ser_ln1,\n");
  uint32_t i;
  for (i = 1; i < burst_len; i++) {
    aim_printf(&uc->pvs,
               "%ld,%ld,%ld\n",
               (i * usec_per_row),
               burst[i] - burst[i - 1],
               burst2[i] - burst2[i - 1]);
  }
  aim_printf(
      &uc->pvs, "last,%ld,%ld\n", cnt - burst[i - 1], cnt2 - burst2[i - 1]);

  return 0;
}
#endif

#endif  // ERROR_PROFILING

static void pm_ucli_tof2_fw_serdes_params_all(
    ucli_context_t *uc,
    bf_dev_id_t dev_id,
    int aflag,
    bf_pal_front_port_handle_t *port_hdl) {
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  int ret;
  bf_dev_id_t dev_id_of_port = 0;

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      if (next_port_info_ptr->pltfm_port_info.log_mac_id == 0) {
        continue;
      }
      fw_serdes_params(next_port_info_ptr->pltfm_port_info.port_hdl.conn_id,
                       next_port_info_ptr->dev_port);
    }
  }
  (void)aflag;
  (void)uc;
}

static ucli_status_t bf_pm_ucli_ucli__port_rx_eq__(ucli_context_t *uc) {
  static char usage[] = "port-rx-eq <port-str>";
  UCLI_COMMAND_INFO(uc, "port-rx-eq", -1, "");
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t port_hdl;
  char *const *argv;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }
  serdes_uc = uc;

  argv = (char *const *)&(uc->pargs->args__);
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  int argc = (uc->pargs->count + 1);
  if (argc > 1) {
    bf_status_t sts;

    sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], &port_hdl);
    if (sts != BF_SUCCESS) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
  }

  // conn==-1 and chnl==-1 indicates that scan through all the ports but only
  // print the ports that have been added.
  //-a option indicates that the scan and print all the ports (whether added or
  // not)

  pm_ucli_tof2_fw_serdes_params_all(uc, dev_id, 0, &port_hdl);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_prbs_mode_set__(ucli_context_t *uc) {
  static char usage[] =
      "prbs-mode-set <port_str> <prbs_mode: "
      "31, "
      "15, 13, 9>";
  UCLI_COMMAND_INFO(uc,
                    "prbs-mode-set",
                    2,
                    "<port_str> "
                    "<prbs_mode: 9/13/15/31");
  bf_status_t sts;
  bf_port_prbs_mode_t prbs_mode;
  int in_prbs_mode;
  bf_dev_id_t dev_id;
  bf_dev_id_t dev_id_of_port = 0;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr, *next_port_hdl_ptr;
  int db_size, i = 0, list_size;
  bf_pal_front_port_handle_t *port_hdl_list;
  bf_dev_port_t dev_port;
  port_hdl_ptr = &port_hdl;
  next_port_hdl_ptr = &next_port_hdl;

  dev_id = 0;
  // if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
  if (bf_lld_dev_is_tof1(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }

  sts = bf_pm_port_str_to_hdl_get(dev_id, uc->pargs->args[0], port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }

  in_prbs_mode = strtoul(uc->pargs->args[1], NULL, 10);
  // make sure its a supported PRBS pattern. We define more patterns
  // than are supported by tof2
  if ((in_prbs_mode != 31) && (in_prbs_mode != 15) && (in_prbs_mode != 13) &&
      (in_prbs_mode != 9)) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }

  prbs_mode = pm_ucli_prbs_mode_get(in_prbs_mode);

  if (port_hdl_ptr->conn_id == (uint32_t)-1) {
    db_size = pm_port_info_db_size_get(dev_id);
    list_size = db_size;
    port_hdl_list = (bf_pal_front_port_handle_t *)bf_sys_malloc(
        db_size * sizeof(bf_pal_front_port_handle_t));
    if (!port_hdl_list) return 0;
    i = 0;
    // Get the first port in the system
    sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
    if (sts != BF_SUCCESS || !port_hdl_ptr) {
      goto end;
    }
    memcpy(&port_hdl_list[i], port_hdl_ptr, sizeof(port_hdl_list[i]));

    while (sts == BF_SUCCESS) {
      i++;
      // Get the next port in the system
      sts = bf_pm_port_front_panel_port_get_next(
          dev_id, port_hdl_ptr, next_port_hdl_ptr);
      if (sts == BF_OBJECT_NOT_FOUND) break;
      if (sts != BF_SUCCESS) {
        goto end;
      }

      memcpy(&port_hdl_list[i], next_port_hdl_ptr, sizeof(port_hdl_list[i]));
      // Make the curr port hdl equal to the next port hdl
      port_hdl_ptr = next_port_hdl_ptr;
    }
    bf_sys_assert(i == db_size);
  } else {
    if (port_hdl_ptr->chnl_id == (uint32_t)-1) {
      port_hdl_ptr->chnl_id = 0;
      if (BF_SUCCESS == bf_pm_port_front_panel_port_to_dev_port_get(
                            port_hdl_ptr, &dev_id_of_port, &dev_port)) {
        dev_id = dev_id_of_port;
        list_size = lld_get_chnls_dev_port(dev_id, dev_port);
        if (list_size == -1) {
          aim_printf(&uc->pvs, "Invalid channel  \n");
          return 0;
        }
      } else {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
      port_hdl_list = (bf_pal_front_port_handle_t *)bf_sys_malloc(
          list_size * sizeof(bf_pal_front_port_handle_t));
      for (i = 0; i < list_size; i++) {
        port_hdl_list[i].conn_id = port_hdl_ptr->conn_id;
        port_hdl_list[i].chnl_id = i;
      }
    } else {
      list_size = 1;
      port_hdl_list = (bf_pal_front_port_handle_t *)bf_sys_malloc(
          list_size * sizeof(bf_pal_front_port_handle_t));
      port_hdl_list[i].conn_id = port_hdl_ptr->conn_id;
      port_hdl_list[i].chnl_id = port_hdl_ptr->chnl_id;
    }
  }

  sts = bf_pm_port_prbs_mode_set(dev_id, port_hdl_list, list_size, prbs_mode);
  if (sts != BF_SUCCESS) {
    goto end;
  }

  return 0;
end:
  aim_printf(&uc->pvs, "Usage : %s\n", usage);
  bf_sys_free(port_hdl_list);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_prbs_mode_clear__(
    ucli_context_t *uc) {
  static char usage[] = "prbs-mode-clear <port_str>";
  UCLI_COMMAND_INFO(uc, "prbs-mode-clear", 1, "<port_str>");
  bf_status_t sts;
  bf_dev_id_t dev_id;
  bf_dev_id_t dev_id_of_port;
  bf_pal_front_port_handle_t port_hdl, next_port_hdl;
  bf_pal_front_port_handle_t *port_hdl_ptr, *next_port_hdl_ptr;
  int db_size, i = 0, list_size;
  bf_pal_front_port_handle_t *port_hdl_list;
  bf_dev_port_t dev_port;

  port_hdl_ptr = &port_hdl;
  next_port_hdl_ptr = &next_port_hdl;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }

  sts = bf_pm_port_str_to_hdl_get(dev_id, uc->pargs->args[0], port_hdl_ptr);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }

  if (port_hdl_ptr->conn_id == (uint32_t)-1) {
    db_size = pm_port_info_db_size_get(dev_id);
    list_size = db_size;
    port_hdl_list = (bf_pal_front_port_handle_t *)bf_sys_malloc(
        db_size * sizeof(bf_pal_front_port_handle_t));
    if (!port_hdl_list) return 0;

    // Get the first port in the system
    sts = bf_pm_port_front_panel_port_get_first(dev_id, port_hdl_ptr);
    if (sts != BF_SUCCESS || !port_hdl_ptr) {
      aim_printf(&uc->pvs, "%s\n", usage);
      return 0;
    }
    i = 0;
    memcpy(&port_hdl_list[i], port_hdl_ptr, sizeof(port_hdl_list[i]));
    while (sts == BF_SUCCESS) {
      i++;
      // Get the next port in the system
      sts = bf_pm_port_front_panel_port_get_next(
          dev_id, port_hdl_ptr, next_port_hdl_ptr);
      if (sts == BF_OBJECT_NOT_FOUND) break;
      if (sts != BF_SUCCESS) {
        goto end;
      }

      memcpy(&port_hdl_list[i], next_port_hdl_ptr, sizeof(port_hdl_list[i]));
      // Make the curr port hdl equal to the next port hdl
      port_hdl_ptr = next_port_hdl_ptr;
    }
    bf_sys_assert(i == db_size);
  } else {
    if (port_hdl_ptr->chnl_id == (uint32_t)-1) {
      port_hdl_ptr->chnl_id = 0;
      if (BF_SUCCESS == bf_pm_port_front_panel_port_to_dev_port_get(
                            port_hdl_ptr, &dev_id_of_port, &dev_port)) {
        dev_id = dev_id_of_port;
        list_size = lld_get_chnls_dev_port(dev_id, dev_port);
        if (list_size == -1) {
          aim_printf(&uc->pvs, "Invalid Channel list.\n");
          return 0;
        }
      } else {
        aim_printf(&uc->pvs, "Usage : %s\n", usage);
        return 0;
      }
      port_hdl_list = (bf_pal_front_port_handle_t *)bf_sys_malloc(
          list_size * sizeof(bf_pal_front_port_handle_t));
      for (i = 0; i < list_size; i++) {
        port_hdl_list[i].conn_id = port_hdl_ptr->conn_id;
        port_hdl_list[i].chnl_id = i;
      }
    } else {
      list_size = 1;
      port_hdl_list = (bf_pal_front_port_handle_t *)bf_sys_malloc(
          list_size * sizeof(bf_pal_front_port_handle_t));
      port_hdl_list[i].conn_id = port_hdl_ptr->conn_id;
      port_hdl_list[i].chnl_id = port_hdl_ptr->chnl_id;
    }
  }

  sts = bf_pm_port_prbs_mode_clear(dev_id, port_hdl_list, list_size);
  if (sts != BF_SUCCESS) {
    goto end;
  }

  bf_sys_free(port_hdl_list);
  return 0;
end:
  aim_printf(&uc->pvs, "Usage : %s\n", usage);
  bf_sys_free(port_hdl_list);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__plot__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "plot", 2, "<port_str> <lane_0_7>");

  char *const *argv;
  bf_dev_id_t dev_id;
  bf_dev_id_t dev_id_of_port = 0;
  bf_pal_front_port_handle_t _s_port_hdl, *port_hdl = &_s_port_hdl;
  bf_status_t sts;
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  int ret;
  uint32_t ln = 0;

  cur_uc = uc;  // save for callback

  argv = (char *const *)&(uc->pargs->args__);
  port_hdl->conn_id = -1;
  port_hdl->chnl_id = -1;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }
  sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], port_hdl);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : plot <port_str> <lane_0_7>");
    return 0;
  }
  ln = atoi(argv[2]);

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      uint8_t *plot_data = NULL;
      bf_tof2_serdes_fw_eye_plot_get(
          dev_id, next_port_info_ptr->dev_port, ln, &plot_data);
      if (plot_data) {
        aim_printf(&uc->pvs, "%s\n", plot_data);
      }
    }
  }
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__eyes__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "eyes", 2, "<port_str> <count>");

  char *const *argv;
  bf_dev_id_t dev_id;
  bf_pal_front_port_handle_t _s_port_hdl, *port_hdl = &_s_port_hdl;
  bf_status_t sts;
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  int ret;
  uint32_t pass, num_passes;
  bf_dev_id_t dev_id_of_port = 0;

  cur_uc = uc;  // save for callback

  argv = (char *const *)&(uc->pargs->args__);
  port_hdl->conn_id = -1;
  port_hdl->chnl_id = -1;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }
  sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], port_hdl);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : plot <port_str> <lane_0_7>");
    return 0;
  }
  num_passes = atoi(argv[2]);

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      bf_status_t rc;
      float eye_1;
      float eye_2;
      float eye_3;
      int ln, num_lanes;
      bf_port_num_lanes_get(0, next_port_info_ptr->dev_port, &num_lanes);

      for (pass = 0; pass < num_passes; pass++) {
        for (ln = 0; ln < num_lanes; ln++) {
          rc = bf_tof2_serdes_eye_get(
              dev_id, next_port_info_ptr->dev_port, ln, &eye_1, &eye_2, &eye_3);
          if (rc != 0) {
            aim_printf(&uc->pvs,
                       "Error %d: d_p=%d : ln=%d : pass=%d\n",
                       rc,
                       next_port_info_ptr->dev_port,
                       ln,
                       pass);
            return 0;
          }
          if ((eye_1 < 50.0) || (eye_2 < 50.0) || (eye_3 < 50.0)) {
            aim_printf(
                &uc->pvs,
                "small-eye: d_p=%d : ln=%d : pass=%d : %4.0f  %4.0f %4.0f\n",
                next_port_info_ptr->dev_port,
                ln,
                pass,
                eye_1,
                eye_2,
                eye_3);
            return 0;
          }
        }
      }
    }
  }
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__sleep__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "sleep", 1, "none");

  int time_in_sec = atoi(uc->pargs->args[0]);
  if (time_in_sec < 24 * 60 * 60) {
    sleep(time_in_sec);
  } else {
    aim_printf(&uc->pvs, "Sleep time must be less than 24 hours.\n");
    aim_printf(&uc->pvs, "For longer time, use hibernate <seconds>\n");
  }
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__hibernate__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "hibernate", 1, "none");

  int time_in_sec = atoi(uc->pargs->args[0]);
  sleep(time_in_sec);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_pll_ovrclk_set__(
    ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "pll-ovrclk-set", 2, "<port_str> <percent>");
  bf_status_t sts;
  bf_pal_front_port_handle_t *port_hdl_ptr;
  bf_dev_id_t dev_id;
  bf_dev_port_t dev_port;
  uint32_t list_sz = 0;
  bf_pm_port_info_t *port_info;
  bf_pal_front_port_handle_t *list = NULL;
  float pll_ovrclk;

  // get list of port_hdls
  sts = bf_pm_ucli_port_list_get((char *)uc->pargs->args[0], &list_sz, &list);
  if (sts != BF_SUCCESS) {
    return 0;
  }

  pll_ovrclk = atof(uc->pargs->args[1]);

  for (uint32_t i = 0; i < list_sz; i++) {
    bool is_port_internal = false;

    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;

    if (!bf_lld_dev_is_tof1(dev_id)) {
      aim_printf(&uc->pvs, "PLL Overclocking not supported on dev:%d", dev_id);
      return 0;
    }
    port_info = pm_port_info_get(dev_id, dev_port);
    if (port_info == NULL) {
      continue;
    }
    if (!port_info->is_added) {
      aim_printf(&uc->pvs,
                 "Port %d/%d need to be added before PLL overclocking\n",
                 port_hdl_ptr->conn_id,
                 port_hdl_ptr->chnl_id);
      continue;
    }
    bf_pm_is_port_internal(dev_id, port_hdl_ptr, &is_port_internal);
    if (!is_port_internal) {
      sts = bf_pm_port_pll_ovrclk_set(dev_id, port_hdl_ptr, pll_ovrclk);
      if (sts != BF_SUCCESS) {
        aim_printf(&uc->pvs,
                   "Error on setting PLL overclocking config on port %d/%d\n",
                   port_hdl_ptr->conn_id,
                   port_hdl_ptr->chnl_id);
      }
    }
  }

  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__port_an_info__(ucli_context_t *uc) {
  static char usage[] = "port-an-info [<conn_id/chnl>] [-h]";
  UCLI_COMMAND_INFO(uc, "port-an-info", -1, "[-a] [-p <port_str>]");

  bf_pal_front_port_handle_t *port_hdl_ptr;
  bf_pal_front_port_handle_t *list = NULL;
  bf_pm_port_info_t *port_info;
  bool flag_show_help = false;
  bf_dev_port_t dev_port;
  bf_dev_id_t dev_id = 0;
  uint32_t lst_sz = 0;
  char *const *argv;
  bf_status_t sts;
  char *scanp;
  char *strp;
  int argc;
  int i;

  argc = (uc->pargs->count + 1);
  argv = (char *const *)&(uc->pargs->args__);

  // scan command line for options
  for (i = 0; i < argc; i++) {
    if (argv[i] == NULL) break;
    if (strncmp(argv[i], "-h", 2) == 0) {
      flag_show_help = true;
    }
    if (strncmp(argv[i], "?", 2) == 0) {
      aim_printf(&uc->pvs, "Usage : %s\n", usage);
      return 0;
    }
  }

  // get list of port_hdls: by default, pass though all ports
  strp = "-/-";

  if (argc >= 2) {
    scanp = *(argv + 1);
    if ((*scanp == '-' && *(scanp + 1) == '/') ||
        (*scanp >= '0' && *scanp <= '9')) {
      strp = scanp;
    }
  }

  // prepare the list of ports to show
  sts = bf_pm_ucli_port_list_get(strp, &lst_sz, &list);
  if (sts != BF_SUCCESS) return 0;

  if (flag_show_help) {
    dump_port_an_info_help(uc);
  }

  dump_port_an_info_banner(uc);

  for (i = 0; i < (int)lst_sz; i++) {
    port_hdl_ptr = &list[i];
    sts = bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl_ptr, &dev_id, &dev_port);
    if (sts != BF_SUCCESS) continue;

    port_info = pm_port_info_get(dev_id, dev_port);
    if (port_info == NULL) {
      continue;
    }
    if (!port_info->is_added) continue;
    if (port_info->is_internal) continue;
    if (!port_info->admin_state) continue;
    if (port_info->an_policy == 2) continue;

    // print an info for each port:
    dump_port_an_info_dump_line_1(uc, dev_id, port_info);
    dump_port_an_info_dump_line_2(uc, dev_id, port_info);
  }
  aim_printf(&uc->pvs, "\n");
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__comment__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "comment", 1, "your comment enclosed in quotes");

  PM_DEBUG("%s", uc->pargs->args[0]);
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__time__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "time", 0, "");
  struct timeval tv;
  struct tm *loctime;
  char ubuf[132], tbuf[132];

  gettimeofday(&tv, NULL);
  loctime = localtime(&tv.tv_sec);
  if (!loctime) return UCLI_STATUS_E_ERROR;
  strftime(tbuf, sizeof(tbuf), "%a %b %d", loctime);
  aim_printf(&uc->pvs, "%s ", tbuf);
  strftime(ubuf, sizeof(ubuf), "%T\n", loctime);
  ubuf[strlen(ubuf) - 1] = 0;  // remove CR
  aim_printf(&uc->pvs, "%s.%06d\n", ubuf, (int)tv.tv_usec);

  return 0;
}

bool tof2_an_dbg = false;
static ucli_status_t bf_pm_ucli_ucli__tof2_an_dbg__(ucli_context_t *uc) {
  int en;

  UCLI_COMMAND_INFO(
      uc, "tof2-an-dbg", 1, "Turn TOF2 AN/LT debug logs on(1)/off(0)");

  en = atoi(uc->pargs->args[0]);
  tof2_an_dbg = en == 0 ? false : true;
  aim_printf(
      &uc->pvs, "AN/LT debug logs: %s\n", tof2_an_dbg ? "enabled" : "disabled");

  return 0;
}

static int32_t ucli_twos_comp(uint32_t val, uint32_t bits) {
  if ((val & (1 << (bits - 1))) !=
      0) {                    //# if sign bit is set e.g., 8bit: 128-255
    val = val - (1 << bits);  //           # compute negative value
  }
  return val;
}

static char *ANLT_state_trans(uint32_t code) {
  char *state = "undef";

  switch (code) {
    case 0xa000:
      state = "STATE_AN_WAIT_RESET";
      break;
    case 0xa001:
      state = "STATE_AN_QUIET";
      break;
    case 0xa003:
      state = "STATE_AN_WAIT_SD";
      break;
    case 0xa007:
      state = "STATE_AN_RECV_START";
      break;
    case 0xa00f:
      state = "STATE_AN_BP_RECEIVED";
      break;
    case 0xa01f:
      state = "STATE_AN_HCD_RESOLVED";
      break;
    case 0xb000:
      state = "STATE_PAM4_LT_PAM2_START";
      break;
    case 0xb001:
      state = "STATE_PAM4_LT_PAM2_WAIT_FRAME_LOCK";
      break;
    case 0xb002:
      state = "STATE_PAM4_LT_PAM2_FRAME_LOCK";
      break;
    case 0xb003:
      state = "STATE_PAM4_LT_PAM2_TX_ADJUST";
      break;
    case 0xb004:
      state = "STATE_PAM4_LT_PAM2_FINISH";
      break;
    case 0xc000:
      state = "STATE_PAM4_LT_PAM4_START";
      break;
    case 0xc001:
      state = "STATE_PAM4_LT_PAM4_WAIT_PHY";
      break;
    case 0xc002:
      state = "STATE_PAM4_LT_PAM4_TX_ADJUST";
      break;
    case 0xc003:
      state = "STATE_PAM4_LT_PAM4_FINISH";
      break;
    default:
      state = "0x0";
  }
  return state;
}

static char *TX_adjust_req_trans(uint32_t code) {
  char *local_req;

  switch (code) {
    case 1:
      local_req = "CM2_DEC";
      break;
    case 2:
      local_req = "CM2_INC";
      break;
    case 4:
      local_req = "CM1_DEC";
      break;
    case 5:
      local_req = "CM1_INC";
      break;
    case 10:
      local_req = "C0_DEC";
      break;
    case -1:
      local_req = "NO_REQ";
      break;
    default:
      local_req = "None";
      break;
  }
  return local_req;
}

/*
  CUR_ANLT_STAT = [ANLT_state_trans(hex(i)) for i in fw_debugs(ln, 0, [99])]
  ANLT_EXIT_CODE = [ANLT_state_trans(hex(i)) for i in fw_debugs(ln, 0,
  range(100,116))]
  FM1 = [twos_comp(i,16)/5 for i in fw_debugs(ln, 2, range(700,719))]
  FM2 = [twos_comp(i,16)/5 for i in fw_debugs(ln, 2, range(750,769))]
  LOCAL_REQ = [TX_adjust_req_trans(twos_comp(i,16)) for i in fw_debugs(ln, 2,
  range(800,819))]
  PAM2_TX = [twos_comp(i,6) for i in fw_debugs(ln, 2, range(850,853))]
  INIT_AGCGAIN = fw_debugs(ln, 2, range(860,862))
  TX_AJUST_ITERATION = fw_debugs(ln, 2, [870])
  print '\nCurrent_ANLT_status  ', CUR_ANLT_STAT,
  print '\nANLT_exit_code       ', ANLT_EXIT_CODE,
  print '\nFM1 during TX adjust ', FM1,
  print '\nFM2 during TX adjust ', FM2,
  print '\nlocal request command', LOCAL_REQ,
  print '\nPAM2 TX setting      ', PAM2_TX,
  print '\nPAM2 agcgain         ', INIT_AGCGAIN,
  print '\nTX adjust iteration  ', TX_AJUST_ITERATION,
*/

void ucli_dump_lt_debug_info(bf_dev_id_t dev_id,
                             bf_dev_port_t dev_port,
                             int ln) {
  bf_status_t rc;
  uint32_t result = 0;
  uint32_t i;
  char *CUR_ANLT_STAT, *ANLT_EXIT_CODE[16];
  int32_t FM1[719 - 700];
  int32_t FM2[769 - 750];
  char *LOCAL_REQ[819 - 800];
  int32_t PAM2_TX[853 - 850];
  int32_t INIT_AGCGAIN[862 - 860];
  uint32_t TX_AJUST_ITERATION;
  (void)rc;

  rc = bf_tof2_serdes_fw_debug_cmd(
      dev_id, dev_port, ln, 0 /*mode*/, 99, &result);
  CUR_ANLT_STAT = ANLT_state_trans(result);
  for (i = 100; i < 116; i++) {
    rc = bf_tof2_serdes_fw_debug_cmd(
        dev_id, dev_port, ln, 0 /*mode*/, i, &result);
    ANLT_EXIT_CODE[i - 100] = ANLT_state_trans(result);
  }
  for (i = 700; i < 719; i++) {
    rc = bf_tof2_serdes_fw_debug_cmd(
        dev_id, dev_port, ln, 2 /*mode*/, i, &result);
    FM1[i - 700] = ucli_twos_comp(result, 16) / 5;
  }
  for (i = 750; i < 769; i++) {
    rc = bf_tof2_serdes_fw_debug_cmd(
        dev_id, dev_port, ln, 2 /*mode*/, i, &result);
    FM2[i - 750] = ucli_twos_comp(result, 16) / 5;
  }
  for (i = 800; i < 819; i++) {
    rc = bf_tof2_serdes_fw_debug_cmd(
        dev_id, dev_port, ln, 2 /*mode*/, i, &result);
    LOCAL_REQ[i - 800] = TX_adjust_req_trans(ucli_twos_comp(result, 16));
  }
  for (i = 850; i < 853; i++) {
    rc = bf_tof2_serdes_fw_debug_cmd(
        dev_id, dev_port, ln, 2 /*mode*/, i, &result);
    PAM2_TX[i - 850] = ucli_twos_comp(result, 6);
  }
  for (i = 860; i < 862; i++) {
    rc = bf_tof2_serdes_fw_debug_cmd(
        dev_id, dev_port, ln, 2 /*mode*/, i, &result);
    INIT_AGCGAIN[i - 860] = result;
  }
  rc = bf_tof2_serdes_fw_debug_cmd(
      dev_id, dev_port, ln, 2 /*mode*/, 870, &result);
  TX_AJUST_ITERATION = result;

  aim_printf(&cur_uc->pvs, "\nCurrent_ANLT_status  : %s", CUR_ANLT_STAT);
  aim_printf(&cur_uc->pvs, "\nANLT_exit_code       : ");
  for (i = 100; i < 116; i++) {
    aim_printf(&cur_uc->pvs, "%s ", ANLT_EXIT_CODE[i - 100]);
  }
  aim_printf(&cur_uc->pvs, "\nFM1 during TX adjust :");
  for (i = 700; i < 719; i++) {
    aim_printf(&cur_uc->pvs, "%d ", FM1[i - 700]);
  }
  aim_printf(&cur_uc->pvs, "\nFM2 during TX adjust :");
  for (i = 750; i < 769; i++) {
    aim_printf(&cur_uc->pvs, "%d ", FM2[i - 750]);
  }
  aim_printf(&cur_uc->pvs, "\nlocal request command:");
  for (i = 800; i < 819; i++) {
    aim_printf(&cur_uc->pvs, "%s ", LOCAL_REQ[i - 800]);
  }
  aim_printf(&cur_uc->pvs, "\nPAM2 TX setting      :");
  for (i = 850; i < 853; i++) {
    aim_printf(&cur_uc->pvs, "%d ", PAM2_TX[i - 850]);
  }
  aim_printf(&cur_uc->pvs, "\nPAM2 agcgain         :");
  for (i = 860; i < 862; i++) {
    aim_printf(&cur_uc->pvs, "%d ", INIT_AGCGAIN[i - 860]);
  }
  aim_printf(&cur_uc->pvs, "\nTX adjust iteration  : %u\n", TX_AJUST_ITERATION);

  aim_printf(&cur_uc->pvs, "Current St: ");
  rc = bf_tof2_serdes_fw_debug_cmd(
      dev_id, dev_port, ln, 0 /*mode*/, 99, &result);
  if (rc != BF_SUCCESS) {
    aim_printf(&cur_uc->pvs, " Err, ");
  } else {
    aim_printf(&cur_uc->pvs, "%04x, ", result);
  }
  aim_printf(&cur_uc->pvs, "Exit codes: ");
  for (i = 100; i < 116; i++) {
    rc = bf_tof2_serdes_fw_debug_cmd(
        dev_id, dev_port, ln, 0 /*mode*/, i, &result);
    if (rc != BF_SUCCESS) {
      aim_printf(&cur_uc->pvs, " Err, ");
    } else {
      aim_printf(&cur_uc->pvs, "%04x, ", result);
    }
  }
  aim_printf(&cur_uc->pvs, "\n");
}

/*
def dump_CL136(lane):
    agcgain = fw_debugs(lane, 1, range(112, 115))
    pam2_fm1 = fw_debugs(lane, 5, range(200, 220))
    pam2_action = fw_debugs(lane, 5, range(250, 270))
    pam4_fm1 = fw_debugs(lane, 5, range(300, 320))
    pam4_fm2 = fw_debugs(lane, 5, range(350, 370))
    pam4_action = fw_debugs(lane, 5, range(400, 420))
    ctle_dfe_log = fw_debugs(lane, 1, range(180, 186))
    ctle_log = fw_debugs(lane, 1, range(190, 196))
    print ("AGCGain: %04x %04x %04x" % (agcgain[0], agcgain[1], agcgain[2]))
    print ("PAM2 f(-1) - action:")
    for i in range(20):
        print ("  %5d - %04x" % (pam2_fm1[i], pam2_action[i]))
    print ("PAM4 f(-1), f(-2) - action")
    for i in range(20):
        print ("   %5d, %5d - %04x" % (pam4_fm1[i], pam4_fm2[i],
pam4_action[i]))
    print ("CTLE DFE - new CTLE")
    for i in range(6):
        print ("   %04x - %d" % (ctle_dfe_log[i], ctle_log[i]))

For the action list, see below (only applies to CL136):

PAM2 actions:

0xA2                     C(-2) dec, if fail: C(0) dec
0xA4                     C(-1) dec, if fail: C(0) dec
0xB5                     C(-1) inc

PAM4 actions:
0x04                     C(-1) dec
0x05                     C(-1) inc
0x01                     C(-2) dec
0x02                     C(-2) inc
0xFF                      No action for this round
0xFE                      Done
*/
typedef struct cl136_xlat_t {
  int32_t action_code;
  char *action_string;
} cl136_xlat_t;

char *cl136_action(int32_t action) {
  cl136_xlat_t action_to_str[] = {{0xA2, "C(-2) dec, if fail: C(0) dec"},
                                  {0xA4, "C(-1) dec, if fail: C(0) dec"},
                                  {0xB5, "C(-1) inc"},
                                  {0x04, "C(-1) dec"},
                                  {0x05, "C(-1) inc"},
                                  {0x01, "C(-2) dec"},
                                  {0x02, "C(-2) inc"},
                                  {0xFF, "      No action for this round"},
                                  {0x2a, "C(-2) saturated, C(0) dec"},
                                  {0x4a, "C(-1) saturated, C(0) dec"},
                                  {0xFE, "      Done"}};
  int i;

  for (i = 0; i < (int)(sizeof(action_to_str) / sizeof(action_to_str[0]));
       i++) {
    if (action_to_str[i].action_code == action) {
      return action_to_str[i].action_string;
    }
  }
  return "";
}

static void ucli_dump_lt_debug_info_new(bf_dev_id_t dev_id,
                                        bf_dev_port_t dev_port,
                                        int ln) {
  bf_status_t rc;
  uint32_t result = 0;
  uint32_t i;
  int32_t AGCGAIN[115 - 112];
  int32_t PAM2_FM1[220 - 200];
  int32_t PAM2_ACTION[270 - 250];
  int32_t PAM4_FM1[320 - 300];
  int32_t PAM4_FM2[370 - 350];
  int32_t PAM4_ACTION[420 - 400];
  int32_t CTLE_DFE_LOG[186 - 180];
  int32_t CTLE_LOG[196 - 190];
  (void)rc;

  aim_printf(&cur_uc->pvs, "--------------------------\n");
  aim_printf(&cur_uc->pvs, "dev=%d : d_p=%3d : ln%d :\n", dev_id, dev_port, ln);

  for (i = 112; i < 115; i++) {
    rc = bf_tof2_serdes_fw_debug_cmd(
        dev_id, dev_port, ln, 1 /*mode*/, i, &result);
    AGCGAIN[i - 112] = result;
  }

  for (i = 200; i < 220; i++) {
    rc = bf_tof2_serdes_fw_debug_cmd(
        dev_id, dev_port, ln, 5 /*mode*/, i, &result);
    PAM2_FM1[i - 200] = ucli_twos_comp(result, 16);  // result;
  }

  for (i = 250; i < 270; i++) {
    rc = bf_tof2_serdes_fw_debug_cmd(
        dev_id, dev_port, ln, 5 /*mode*/, i, &result);
    PAM2_ACTION[i - 250] = result;
  }

  for (i = 300; i < 320; i++) {
    rc = bf_tof2_serdes_fw_debug_cmd(
        dev_id, dev_port, ln, 5 /*mode*/, i, &result);
    PAM4_FM1[i - 300] = ucli_twos_comp(result, 16);  // result;
  }

  for (i = 350; i < 370; i++) {
    rc = bf_tof2_serdes_fw_debug_cmd(
        dev_id, dev_port, ln, 5 /*mode*/, i, &result);
    PAM4_FM2[i - 350] = ucli_twos_comp(result, 16);  // result;
  }

  for (i = 400; i < 420; i++) {
    rc = bf_tof2_serdes_fw_debug_cmd(
        dev_id, dev_port, ln, 5 /*mode*/, i, &result);
    PAM4_ACTION[i - 400] = result;
  }

  for (i = 180; i < 186; i++) {
    rc = bf_tof2_serdes_fw_debug_cmd(
        dev_id, dev_port, ln, 1 /*mode*/, i, &result);
    CTLE_DFE_LOG[i - 180] = result;
  }

  for (i = 190; i < 196; i++) {
    rc = bf_tof2_serdes_fw_debug_cmd(
        dev_id, dev_port, ln, 1 /*mode*/, i, &result);
    CTLE_LOG[i - 190] = result;
  }

  aim_printf(&cur_uc->pvs, "CTLE DFE - new CTLE:\n");
  for (i = 0; i < 6; i++) {
    aim_printf(&cur_uc->pvs,
               "   (%04x) %2d,%2d - %d\n",
               CTLE_DFE_LOG[i],
               ucli_twos_comp((CTLE_DFE_LOG[i] >> 8), 8),
               ucli_twos_comp((CTLE_DFE_LOG[i] & 0xFF), 8),
               CTLE_LOG[i]);
    if ((CTLE_DFE_LOG[i] == 0) && (CTLE_LOG[i] == 0)) {
      break;
    }
  }

  aim_printf(&cur_uc->pvs,
             "AGCGain: %04x %04x %04x\n",
             AGCGAIN[0],
             AGCGAIN[1],
             AGCGAIN[2]);
  aim_printf(&cur_uc->pvs, "PAM2 f(-1) - action:\n");
  for (i = 0; i < (220 - 200); i++) {
    aim_printf(&cur_uc->pvs,
               "  %5d - %04x : %s\n",
               PAM2_FM1[i],
               PAM2_ACTION[i],
               cl136_action(PAM2_ACTION[i]));
    if ((PAM2_FM1[i] == 0) && (PAM2_ACTION[i] == 0)) {
      break;
    }
  }

  aim_printf(&cur_uc->pvs, "PAM4 f(-1), f(-2) - action:\n");
  for (i = 0; i < (320 - 300); i++) {
    aim_printf(&cur_uc->pvs,
               "   %5d, %5d - %04x : %s\n",
               PAM4_FM1[i],
               PAM4_FM2[i],
               PAM4_ACTION[i],
               cl136_action(PAM4_ACTION[i]));
    if ((PAM4_FM1[i] == 0) && (PAM4_FM2[i] == 0) && (PAM4_ACTION[i] == 0)) {
      break;
    }
  }
  aim_printf(&cur_uc->pvs, "\n");
  // also log the exit codes
  uint32_t res[16];
  for (i = 100; i < 116; i++) {
    rc = bf_tof2_serdes_fw_debug_cmd(
        dev_id, dev_port, ln, 0 /*mode*/, i, &res[i - 100]);
  }
  aim_printf(&cur_uc->pvs,
             "Exit code: %04x %04x %04x %04x %04x %04x %04x %04x "
             "%04x %04x %04x %04x %04x %04x %04x %04x\n",
             res[0],
             res[1],
             res[2],
             res[3],
             res[4],
             res[5],
             res[6],
             res[7],
             res[8],
             res[9],
             res[10],
             res[11],
             res[12],
             res[13],
             res[14],
             res[15]);
}

static ucli_status_t bf_pm_ucli_ucli__tof2_lt_dbg__(ucli_context_t *uc) {
  char *const *argv;
  bf_dev_id_t dev_id;
  bf_dev_id_t dev_id_of_port = 0;
  bf_pal_front_port_handle_t _s_port_hdl, *port_hdl = &_s_port_hdl;
  bf_status_t sts;
  bf_pm_port_info_t next_port_info;
  bf_pm_port_info_t *port_info_ptr, *next_port_info_ptr;
  bf_dev_pipe_t phy_pipe = 0;
  bf_dev_pipe_t log_pipe = 0;
  int ret;
  uint32_t ln = 0;

  UCLI_COMMAND_INFO(uc, "tof2-lt-dbg", 1, "TOF2 LT debug logs");

  cur_uc = uc;  // save for callback

  argv = (char *const *)&(uc->pargs->args__);
  port_hdl->conn_id = -1;
  port_hdl->chnl_id = -1;

  dev_id = 0;
  if (bf_lld_dev_is_tof1(dev_id) || bf_lld_dev_is_tof3(dev_id)) {
    aim_printf(&uc->pvs, "Command not supported on this chip\n");
    return 0;
  }
  sts = bf_pm_port_str_to_hdl_get(dev_id, argv[1], port_hdl);
  if (sts != BF_SUCCESS) {
    aim_printf(&uc->pvs, "Usage : plot <port_str> <lane_0_7>");
    return 0;
  }

  next_port_info_ptr = &next_port_info;

  ret =
      pm_port_info_get_first_copy(dev_id, next_port_info_ptr, &dev_id_of_port);
  dev_id = dev_id_of_port;

  for (; ret == 0;
       port_info_ptr = next_port_info_ptr,
       ret = pm_port_info_get_next_copy(
           dev_id, port_info_ptr, next_port_info_ptr, &dev_id_of_port)) {
    dev_id = dev_id_of_port;

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
      int n_lanes = next_port_info_ptr->n_lanes;

      for (ln = 0; ln < (uint32_t)n_lanes; ln++) {
        ucli_dump_lt_debug_info_new(dev_id, next_port_info_ptr->dev_port, ln);
      }
    }
  }
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__common_command_start__(
    ucli_context_t *uc) {
  UCLI_COMMAND_INFO(
      uc,
      "\ncommon_command_start",
      0,
      "---------- Start of common (tof1 or tof2) ucli commands -------\n");
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__tof1_command_start__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(
      uc,
      "\ncommon_tof1_start",
      0,
      "---------- Start of tof1-specific ucli commands -------\n");
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__tof2_command_start__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(
      uc,
      "\ncommon_tof2_start",
      0,
      "---------- Start of tof2-specific ucli commands -------\n");
  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__mac_reset__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "mac-reset", -1, "mac-reset <dev_port> [tx] [rx]");
  bool tx = false, rx = false, dev_id = 0;
  bf_dev_port_t dev_port;
  int argc = uc->pargs->count;
  bf_status_t rc;

  if (argc > 1) {
    dev_port = atoi(uc->pargs->args[0]);
  } else {
    aim_printf(&uc->pvs, "usage: mac-reset <dev_port> [tx] [rx]\n");
    return 0;
  }
  if (argc > 2) {
    if ((uc->pargs->args[1][0] == 't') || (uc->pargs->args[1][0] == 'T')) {
      tx = true;
    } else if ((uc->pargs->args[1][0] == 'r') ||
               (uc->pargs->args[1][0] == 'R')) {
      rx = true;
    }
    if (argc > 3) {
      if ((uc->pargs->args[2][0] == 't') || (uc->pargs->args[2][0] == 'T')) {
        tx = true;
      } else if ((uc->pargs->args[2][0] == 'r') ||
                 (uc->pargs->args[2][0] == 'R')) {
        rx = true;
      }
    }
  }
  if (tx) {
    rc = bf_port_tx_reset_set(dev_id, dev_port);
    if (rc) {
      aim_printf(&uc->pvs, "Error: %d : from bf_port_tx_reset_set", rc);
    }
  }
  if (rx) {
    rc = bf_port_rx_reset_set(dev_id, dev_port);
    if (rc) {
      aim_printf(&uc->pvs, "Error: %d : from bf_port_rx_reset_set", rc);
    }
  }
  if (!tx && !rx) {
    aim_printf(&uc->pvs, "usage: mac-reset <dev_port> [tx] [rx]\n");
  }
  return 0;
}

extern void efuse_display(ucli_context_t *uc, bf_dev_id_t dev_id);

ucli_status_t bf_pm_ucli_ucli__sku__(ucli_context_t *uc) {
  bf_dev_id_t dev_id = 0;
  bool detail = false;
  lld_err_t rc = 0;
  bf_sku_chip_part_rev_t rev_no;
  char *fam_str = "Tofino1";

  UCLI_COMMAND_INFO(uc, "sku", -1, "<dev-id> [-d]");

  if (uc->pargs->count == 0) {
    dev_id = 0;
    detail = false;
  } else if (uc->pargs->count == 1) {
    if ((uc->pargs->args[0][0] == '-') && (uc->pargs->args[0][1] == 'd')) {
      dev_id = 0;
      detail = true;
    } else {
      dev_id = atoi(uc->pargs->args[0]);
      detail = false;
    }
  } else if (uc->pargs->count == 2) {
    if ((uc->pargs->args[0][0] == '-') && (uc->pargs->args[0][1] == 'd')) {
      dev_id = atoi(uc->pargs->args[1]);
      detail = true;
    } else if ((uc->pargs->args[1][0] == '-') &&
               (uc->pargs->args[1][1] == 'd')) {
      dev_id = atoi(uc->pargs->args[0]);
      detail = true;
    }
  }

  if (!lld_dev_ready(dev_id, 0)) {
    aim_printf(&uc->pvs, "Invalid dev_id: %d\n", dev_id);
    return 0;
  }

  if (bf_lld_dev_is_tof2(dev_id)) {
    fam_str = "Tofino2";
  } else if (bf_lld_dev_is_tof3(dev_id)) {
    fam_str = "Tofino3";
  }
  rc = lld_sku_get_chip_part_revision_number(dev_id, &rev_no);
  if (rc == 0) {
    char *sku_nm = lld_sku_get_sku_name(dev_id);

    aim_printf(&uc->pvs,
               "SKU : %s %s (%s)\n",
               fam_str,
               sku_nm,
               bf_sku_chip_part_rev_str(rev_no));
  } else {
    aim_printf(&uc->pvs, "SKU : <unknown>\n");
  }
  if (detail) {
    efuse_display(uc, dev_id);
  }
  return 0;
}

ucli_status_t bf_drv_show_tech_ucli_pm__(ucli_context_t *uc) {
  aim_printf(&uc->pvs, "-------------------- PM --------------------\n");
  bf_dev_id_t dev_id;
  int aflag = 0, dflag = 0, tflag = 0;
  bf_pal_front_port_handle_t port_hdl;

  dev_id = 0;
  port_hdl.conn_id = -1;
  port_hdl.chnl_id = -1;

  pm_ucli_dump_info(uc, dev_id, &port_hdl, dflag, aflag, tflag);
  bf_pm_ucli_ucli__port_sd_show__(uc);
  bf_pm_ucli_ucli__port_pcs__(uc);
  bf_pm_ucli_ucli__fsm_info__(uc);
  bf_pm_ucli_ucli__port_ctrs__(uc);
  bf_pm_ucli_ucli__port_an_info__(uc);
  bf_pm_ucli_ucli__fsm_info__(uc);

  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__recirc__ports_show(ucli_context_t *uc) {
  bf_dev_id_t dev_id = 0;
  uint32_t recirc_devport_list[30] = {0xff};
  uint32_t max_recirc_port, i;

  UCLI_COMMAND_INFO(uc, "recirc-ports-show", 1, "<dev-id>");

  if (uc->pargs->count == 1) {
    dev_id = atoi(uc->pargs->args[0]);
  }

  if (!lld_dev_ready(dev_id, 0)) {
    aim_printf(&uc->pvs, "Invalid dev_id: %d\n", dev_id);
    return 0;
  }

  bf_pm_ucli_ucli__sku__(uc);

  max_recirc_port = bf_pm_recirc_devports_get(dev_id, &recirc_devport_list[0]);
  aim_printf(&uc->pvs, "-------------------------------------\n");
  aim_printf(&uc->pvs, "Cnt | Pipe | Recirc-port | Dev-port  \n");
  aim_printf(&uc->pvs, "-------------------------------------\n");
  for (i = 0; i < max_recirc_port; i++) {
    aim_printf(&uc->pvs,
               "%-4d| %-4d | %-11d | %-20d \n",
               i,
               DEV_PORT_TO_PIPE(recirc_devport_list[i]),
               DEV_PORT_TO_LOCAL_PORT(recirc_devport_list[i]),
               recirc_devport_list[i]);
  }

  return 0;
}

static ucli_status_t bf_pm_ucli_ucli__sonic_lane_idx_to_port__(
    ucli_context_t *uc) {
  static char usage[] = "lane-idx-sonic  <idx-1 idx-2 etc>";
  UCLI_COMMAND_INFO(uc, "lane-idx-sonic", -1, "<idx-1 idx-2 etc>");

  char *const *argv;
  uint32_t fp_idx[100];
  int i;
  bf_dev_id_t dev_id = 0;
  bf_dev_port_t dp = 0;
  char port_str[100];

  argv = (char *const *)&(uc->pargs->args__);
  int argc = (uc->pargs->count);
  if (argc >= 1) {
    for (i = 0; i < argc; i++) {
      fp_idx[i] = (int)strtol(argv[i + 1], NULL, 10);
    }
  } else {
    aim_printf(&uc->pvs, "Usage : %s\n", usage);
    return 0;
  }
  for (i = 0; i < argc; i++) {
    if (bf_pm_front_port_index_to_dev_port_get(dev_id, fp_idx[i], &dp) ==
        BF_SUCCESS) {
      bf_pm_dev_port_to_port_str_get(dev_id, dp, port_str);
      aim_printf(&uc->pvs,
                 "lane-idx : %4d dp : %4d port %s\n",
                 fp_idx[i],
                 dp,
                 port_str);
    } else {
      aim_printf(&uc->pvs,
                 "Error Dev-port not found for lane-idx : %2d \n",
                 fp_idx[i]);
    }
  }
  return 0;
}

static ucli_command_handler_f bf_pm_ucli_ucli_handlers__[] = {
    /* Common commands */
    bf_pm_ucli_ucli__common_command_start__,
    bf_pm_ucli_ucli__port_add__,
    bf_pm_ucli_ucli__port_delete__,
    bf_pm_ucli_ucli__port_enable__,
    bf_pm_ucli_ucli__port_disable__,
    bf_pm_ucli_ucli__port_show__,
    bf_pm_ucli_ucli__port_cfg__,
    bf_pm_ucli_ucli__port_stats_clear__,
    bf_pm_ucli_ucli__port_stats_timer__,
    bf_pm_ucli_ucli__port_stats_period_set__,
    bf_pm_ucli_ucli__port_rate_set__,
    bf_pm_ucli_ucli__port_rate_show__,
    bf_pm_ucli_ucli__port_auto_neg_set__,
    bf_pm_ucli_ucli__port_kr_mode_set__,
    bf_pm_ucli_ucli__port_direction_set__,
    bf_pm_ucli_ucli__port_loopback_set__,
    bf_pm_ucli_ucli__port_pcs__,
    bf_pm_ucli_ucli__port_vl__,
    bf_pm_ucli_ucli__port_ctrs__,
    bf_pm_ucli_ucli__port_sd_show__,
    bf_pm_ucli_ucli__port_fec_set__,
    bf_pm_ucli_ucli__port_ct_set__,
    bf_pm_ucli_ucli__port_tx_eq_set__,
    bf_pm_ucli_ucli__ln_tx_eq_set__,
    bf_pm_ucli_ucli__ln_tx_polarity_set__,
    bf_pm_ucli_ucli__ln_rx_polarity_set__,
    bf_pm_ucli_ucli__sonic_lane_idx_to_port__,
    bf_pm_ucli_ucli__fsm_stop__,
    bf_pm_ucli_ucli__fsm_go__,
    bf_pm_ucli_ucli__fsm_step__,
    bf_pm_ucli_ucli__port_fsm__,
    bf_pm_ucli_ucli__sku__,
    bf_pm_ucli_ucli__recirc__ports_show,
    bf_pm_ucli_ucli__comment__,
    bf_pm_ucli_ucli__time__,
    bf_pm_ucli_ucli__sleep__,
    bf_pm_ucli_ucli__hibernate__,
    bf_pm_ucli_ucli__port_pll_ovrclk_set__,
    bf_pm_ucli_ucli__port_an_info__,
    /* Tof1 (only) Commands */
    bf_pm_ucli_ucli__tof1_command_start__,
    bf_pm_ucli_ucli__port_prbs_set__,
    bf_pm_ucli_ucli__port_prbs_cleanup__,
    bf_pm_ucli_ucli__port_sd_cfg_set__,
    bf_pm_ucli_ucli__port_sd_perf__,
    bf_pm_ucli_ucli__port_sd_plot_eye__,
    bf_pm_ucli_ucli__port_sd_dfe_set__,
    bf_pm_ucli_ucli__port_sd_dfe_ical__,
    bf_pm_ucli_ucli__port_sd_dfe_pcal__,
    bf_pm_ucli_ucli__port_sd_chg_to_prbs__,
    bf_pm_ucli_ucli__port_error_show__,
    bf_pm_ucli_ucli__port_error_get__,
    bf_pm_ucli_ucli__port_error_clear__,
    bf_pm_ucli_ucli__port_linkup_max_errors_set__,
    bf_pm_ucli_ucli__port_linkup_max_errors_get__,
    bf_pm_ucli_ucli__port_stats_persistent__,
    bf_pm_ucli_ucli__tof1_port_term_mode_set__,
    bf_pm_ucli_ucli__dfe_retry_timer_set__,
    bf_pm_ucli_ucli__dfe_retry_timer_get__,
    /* Tof2 (only) commands */
    bf_pm_ucli_ucli__tof2_command_start__,
    bf_pm_ucli_ucli__port_int__,
    bf_pm_ucli_ucli__port_hist__,
    bf_pm_ucli_ucli__fsm_info__,
    bf_pm_ucli_ucli__port_prbs_mode_set__,
    bf_pm_ucli_ucli__port_prbs_mode_clear__,
    bf_pm_ucli_ucli__prbs_show__,
    bf_pm_ucli_ucli__prbs_fix__,
    bf_pm_ucli_ucli__prbs_reset__,
    bf_pm_ucli_ucli__port_isi__,
    bf_pm_ucli_ucli__port_ber__,
    bf_pm_ucli_ucli__port_precoding_tx_set__,
    bf_pm_ucli_ucli__port_precoding_rx_set__,
    bf_pm_ucli_ucli__port_precoding_tx_clear__,
    bf_pm_ucli_ucli__port_precoding_rx_clear__,
    bf_pm_ucli_ucli__port_rx_eq__,
    bf_pm_ucli_ucli__tx_err_set__,
    bf_pm_ucli_ucli__port_tune__,
    bf_pm_ucli_ucli__port_fw_cmd__,
    bf_pm_ucli_ucli__port_fw_dbg_cmd__,
    bf_pm_ucli_ucli__port_fw_reg_rd__,
    bf_pm_ucli_ucli__port_fw_reg_wr__,
    bf_pm_ucli_ucli__port_exit_codes__,
    bf_pm_ucli_ucli__port_csv_dump__,
    bf_pm_ucli_ucli__port_csv_dump_multiple__,
    bf_pm_ucli_ucli__tof2_an_dbg__,
    bf_pm_ucli_ucli__tof2_lt_dbg__,
    bf_pm_ucli_ucli__mac_reset__,
    bf_pm_ucli_ucli__plot__,
    bf_pm_ucli_ucli__eyes__,
    bf_pm_ucli_ucli__port_linkup_debounce_get__,
    bf_pm_ucli_ucli__port_linkup_debounce_set__,
    bf_pm_ucli_ucli__tof2_port_term_mode_set__,
#ifdef ERROR_PROFILING
    bf_pm_ucli_ucli__port_error_mon__,
    bf_pm_ucli_ucli__port_errs__,
    bf_pm_ucli_ucli__port_burst__,
#endif  // ERROR_PROFILING

    bf_pm_ucli_ucli__tof3_command_start__,
    bf_pm_ucli_ucli__tof3_port_tx_eq_set__,
    bf_pm_ucli_ucli__tof3_port_term_mode_set__,
    bf_pm_ucli_ucli__tof3_port_pcs__,
    bf_pm_ucli_ucli__tof3_port_fec__,
    bf_pm_ucli_ucli__tof3_port_anlt__,
    bf_pm_ucli_ucli__tof3_port_int__,
    bf_pm_ucli_ucli__tof3_port_glue__,
    bf_pm_ucli_ucli__tof3_port_serdes_debug__,
    bf_pm_ucli_ucli__tof3_port_fec_mon__,
    NULL};

static ucli_module_t bf_pm_ucli_module__ = {
    "bf_pm_ucli",
    NULL,
    bf_pm_ucli_ucli_handlers__,
    NULL,
    NULL,
};

ucli_node_t *bf_pm_ucli_node_create(void) {
  ucli_node_t *n;
  ucli_module_init(&bf_pm_ucli_module__);
  n = ucli_node_create("pm", NULL, &bf_pm_ucli_module__);
  ucli_node_subnode_add(n, ucli_module_log_node_create("pm"));
  return n;
}
