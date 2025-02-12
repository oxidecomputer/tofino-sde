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


#include <inttypes.h>
#include <stdlib.h>
#include <target-utils/uCli/ucli.h>
#include <target-utils/uCli/ucli_argparse.h>
#include <target-utils/uCli/ucli_handler_macros.h>

#include <target-sys/bf_sal/bf_sys_intf.h>

#include <dvm/bf_drv_intf.h>
#include <port_mgr/bf_port_if.h>
#include "bf_switchd_log.h"
#include "bf_switchd.h"
#include "bf_hw_porting_config.h"

extern void *bf_dma2virt_dbg(bf_dma_addr_t dma_addr);

static ucli_status_t switchd_ucli_ucli__sys_dma2virt__(ucli_context_t *uc) {
  bf_dma_addr_t dma_addr;
  void *vptr;

  UCLI_COMMAND_INFO(
      uc, "sys_dma2virt", 1, "Convert dma address to virtual address");

  dma_addr = strtoull(uc->pargs->args[0], NULL, 16);
  vptr = bf_dma2virt_dbg(dma_addr);
  aim_printf(&uc->pvs,
             "dma address 0x%08" PRIx64 " has virtual adress %p\n",
             (uint64_t)dma_addr,
             vptr);
  return 0;
}

static ucli_status_t switchd_ucli_ucli__sys_virt2dma__(ucli_context_t *uc) {
  bf_dma_addr_t dma_addr;
  void *vptr;

  UCLI_COMMAND_INFO(
      uc, "sys_virt2dma", 1, "Convert virtual address to dma address");

  vptr = (void *)(uintptr_t)strtoull(uc->pargs->args[0], NULL, 16);
  dma_addr = bf_mem_virt2dma(vptr);
  aim_printf(&uc->pvs,
             "virtual address %p has dma adress 0x%08" PRIx64 "\n",
             vptr,
             (uint64_t)dma_addr);
  return 0;
}

static ucli_status_t switchd_ucli_ucli__pcie_log_clear__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "pcie_log_clear", 0, "Dump register access log");

  bf_switchd_log_init(false);
  return 0;
}

static ucli_status_t switchd_ucli_ucli__pcie_log__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "pcie_log", 0, "Dump register access log");

  bf_switchd_log_dump_access_log();
  return 0;
}

static ucli_status_t switchd_ucli_ucli__pcie_logx__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "pcie_logx", 1, "Filtered dump of register access log");

  bf_switchd_log_dump_access_log_w_filter((char *)uc->pargs->args[0]);
  return 0;
}

static ucli_status_t switchd_ucli_ucli__pcie_log_last__(ucli_context_t *uc) {
  int num_logs;

  UCLI_COMMAND_INFO(
      uc, "pcie_log_last", 1, "Dump last 'n' entries of register access log");

  num_logs = atoi(uc->pargs->args[0]);
  bf_switchd_log_dump_access_log_last_n(num_logs);
  return 0;
}

static ucli_status_t switchd_ucli_ucli__terminate__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(
      uc, "terminate", 1, "Exit switchd with status: terminate <exit status>");
  int status = atoi(uc->pargs->args[0]);

  for (int i = 0; i < BF_MAX_DEV_COUNT; ++i) bf_switchd_device_remove(i);

  exit(status);
  return 0;
}

static ucli_status_t switchd_ucli_ucli__warm_init_end__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc,
                    "warm-init-end",
                    1,
                    "End warm init for a device: warm-init-end <device>");
  bf_dev_id_t dev_id = atoi(uc->pargs->args[0]);

  bf_status_t x = bf_switchd_warm_init_end(dev_id);
  aim_printf(
      &uc->pvs, "Warm init end for device %d returned status %d\n", dev_id, x);
  return 0;
}

static ucli_status_t switchd_ucli_ucli__rmv_dev__(ucli_context_t *uc) {
  bf_dev_id_t dev_id;

  UCLI_COMMAND_INFO(uc, "rmv_dev", 1, "Remove device <device>");

  dev_id = atoi(uc->pargs->args[0]);

  if (dev_id >= BF_MAX_DEV_COUNT) {
    aim_printf(
        &uc->pvs, "ERROR: Max device id supported is %d\n", BF_MAX_DEV_COUNT);
    return 0;
  }

  aim_printf(&uc->pvs, "Removing device %d \n", dev_id);
  bf_status_t sts = bf_switchd_device_remove(dev_id);
  aim_printf(&uc->pvs, "Remove device %d returned status %d\n", dev_id, sts);
  return 0;
}

static ucli_status_t switchd_ucli_ucli__board_port_map__(ucli_context_t *uc) {
  bf_dev_id_t dev_id;
  int num_ports = 0, num_ch = 0;
  bf_conn_map_t *cmap = NULL;
  char port_str[MAX_PORT_HDL_STRING_LEN];

  UCLI_COMMAND_INFO(
      uc, "board-port-map", 1, "Displays the board port map for <device>");

  dev_id = atoi(uc->pargs->args[0]);

  if (dev_id >= BF_MAX_DEV_COUNT) {
    aim_printf(&uc->pvs,
               "Only %d chips defined. Correct command or update "
               "lldif/inc/lldif/sdk_bsp.h\n",
               BF_MAX_DEV_COUNT);
    return 0;
  }

  if (!bf_hw_cfg_bd_cfg_found_get()) {
    aim_printf(&uc->pvs, "Available only for bspless mode\n");
    return 0;
  }

  num_ports = bf_hw_cfg_bd_num_port_get();
  num_ch = MAX_CHAN_PER_CONNECTORS;

  aim_printf(&uc->pvs,
             "-----------------------------------------------------------------"
             "---------------------------------------------\n");
  aim_printf(
      &uc->pvs,
      "| %10s | %9s | %8s | %3s | %6s | %6s | %6s | %10s | %10s | %11s |\n",
      "Front Port",
      "QSFPDD Ch",
      "Internal",
      "Mac",
      "Mac ch",
      "PCB RX",
      "PCB TX",
      "RX PN Swap",
      "TX PN Swap",
      "Package Pin");
  aim_printf(&uc->pvs,
             "-----------------------------------------------------------------"
             "---------------------------------------------\n");
  for (int conn = 1; conn <= num_ports; conn++) {
    for (int ch = 0; ch < num_ch; ch++) {
      cmap = bf_pltfm_conn_map_find(conn, ch);
      if (cmap != NULL) {
        snprintf(port_str, sizeof(port_str), "%d/%d", conn, ch);
        for (uint32_t itr = 0; itr < cmap->nlanes_per_ch; itr++) {
          lane_map_t *lmap = &cmap->lane[ch + itr];
          if (lmap == NULL) continue;

          aim_printf(
              &uc->pvs,
              "| %10s | %9d | %8d | %3d | %6d | %6d | %6d | %10d | %10d | "
              "%11s |\n",
              port_str,
              lmap->channel,
              cmap->is_internal_port,
              cmap->mac_block,
              lmap->mac_ch,
              lmap->rx_lane,
              lmap->tx_lane,
              lmap->rx_pn_swap,
              lmap->tx_pn_swap,
              // NOTE: This cannot be NULL, it's declared at
              // /home/bnaecker/bf_sde/pkgsrc/bf-drivers/bf_switchd/bf_hw_porting_config.h:101.
              cmap->pack_pin_name);
        }
      }
    }
  }
  return 0;
}

static ucli_status_t switchd_ucli_ucli__bye__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "bye", -1, "Stop switchd process");
  char help_str[] =
      "Are you sure to Stop the switchd process? "
      "Usage: bye -y";
  char key_str[] = "-y";
  if (!uc->pargs->count) {
    aim_printf(&uc->pvs, "%s\n", help_str);
    return 0;
  }
  if (!strncmp(key_str, uc->pargs->args[0], strlen(key_str))) {
    exit(0);
  }
  aim_printf(&uc->pvs, "%s\n", help_str);
  return 0;
}

static ucli_status_t switchd_ucli_ucli__add_dev__(ucli_context_t *uc) {
  bf_dev_id_t dev_id;

  UCLI_COMMAND_INFO(uc, "add_dev", 1, "add_dev <asic>");

  dev_id = atoi(uc->pargs->args[0]);

  if (dev_id >= BF_MAX_DEV_COUNT) {
    aim_printf(
        &uc->pvs, "ERROR: Max device id supported is %d\n", BF_MAX_DEV_COUNT);
    return 0;
  }

  aim_printf(&uc->pvs, "Adding device %d \n", dev_id);

  bf_status_t sts = bf_switchd_device_add(dev_id, true /* setup_dma */);
  aim_printf(&uc->pvs, "Add device %d returned status %d\n", dev_id, sts);
  return 0;
}

/* <auto.ucli.handlers.start> */
/* <auto.ucli.handlers.end> */
static ucli_command_handler_f switchd_ucli_ucli_handlers__[] = {
    switchd_ucli_ucli__terminate__,
    switchd_ucli_ucli__bye__,
    switchd_ucli_ucli__warm_init_end__,
    switchd_ucli_ucli__sys_dma2virt__,
    switchd_ucli_ucli__sys_virt2dma__,
    switchd_ucli_ucli__pcie_log__,
    switchd_ucli_ucli__pcie_logx__,
    switchd_ucli_ucli__pcie_log_clear__,
    switchd_ucli_ucli__pcie_log_last__,
    switchd_ucli_ucli__rmv_dev__,
    switchd_ucli_ucli__add_dev__,
    switchd_ucli_ucli__board_port_map__,
    NULL};

static ucli_module_t switchd_ucli_module__ = {
    "switchd_ucli",
    NULL,
    switchd_ucli_ucli_handlers__,
    NULL,
    NULL,
};

static ucli_node_t *switchd_ucli_node_create(void) {
  ucli_node_t *n;
  ucli_module_init(&switchd_ucli_module__);
  n = ucli_node_create("switchd", NULL, &switchd_ucli_module__);
  ucli_node_subnode_add(n, ucli_module_log_node_create("switchd"));
  return n;
}

void switchd_register_ucli_node() {
  ucli_node_t *ucli_node = switchd_ucli_node_create();
  bf_drv_shell_register_ucli(ucli_node);
}
