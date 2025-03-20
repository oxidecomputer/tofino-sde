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


/**
 * @file bf_drv_ucli.c
 * @brief bf_drv uCli thread and commands
 *
 * The uCli object is invoked in its own thread.
 */

#include <stdlib.h>
#include <getopt.h>

#include <target-sys/bf_sal/bf_sys_intf.h>
#include <target-utils/uCli/ucli.h>
#include <target-utils/uCli/ucli_argparse.h>
#include <target-utils/uCli/ucli_node.h>
#include <target-utils/target_utils.h>
#include <bf_switchd/bf_switchd.h>
#include <bfutils/bf_utils.h>
#include <bf_types/bf_types.h>
#include <dvm/bf_drv_intf.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include <bf_pm/pm_intf.h>
#include <lld/lld.h>
#include <port_mgr/bf_fsm_if.h>
#include <port_mgr/port_mgr_port.h>
#include <pipe_mgr/pipe_mgr_stful_ucli.h>
#include <traffic_mgr/common/traffic_mgr_int.h>
#include <mc_mgr/mc_mgr_int.h>
#include <dvm/dvm.h>

static ucli_status_t bf_drv_ucli_ucli__version(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(
      uc, "ver", 0, "Display versions of all loaded/linked BF SDE components");

  aim_printf(&uc->pvs,
             "bf-syslibs: %s (Internal:%s)\n",
             target_syslib_get_version(),
             target_syslib_get_internal_version());

  aim_printf(&uc->pvs,
             "bf-utils: %s (Internal:%s)\n",
             bf_utils_get_version(),
             bf_utils_get_internal_version());

  aim_printf(&uc->pvs,
             "bf-drivers: %s (Internal:%s)\n",
             bf_drv_get_version(),
             bf_drv_get_internal_version());

  return 0;
}

ucli_status_t bf_drv_show_tech_ucli_sys__(ucli_context_t *uc) {
  aim_printf(&uc->pvs, "--------------------- SYSTEM --------------------\n");
  bf_drv_ucli_ucli__version(uc);
  bf_pm_ucli_ucli__sku__(uc);
  return 0;
}

static ucli_status_t bf_drv_ucli_ucli__dump_trace__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "dump-trace", 0, "Display the trace logs");

  size_t max_size = 1024 * 1024;
  unsigned char *t = bf_sys_malloc(max_size);
  if (!t) {
    aim_printf(&uc->pvs, "Failed to allocate buffer for trace read\n");
    return 0;
  }
  memset(t, 0, max_size);
  size_t act_size;
  int x = bf_sys_trace_get(t, max_size, &act_size);
  if (x)
    aim_printf(&uc->pvs, "Failed to retrieve trace buffer, status %d\n", x);
  else
    aim_printf(&uc->pvs, "Trace is %zu bytes:\n%s\n", act_size, t);
  bf_sys_free(t);
  return 0;
}

static ucli_status_t bf_drv_ucli_ucli__reset_trace__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "reset-trace", 0, "Clear the trace logs");

  int x = bf_sys_trace_reset();
  if (x) aim_printf(&uc->pvs, "Failed to reset trace buffer, status %d\n", x);
  return 0;
}

static ucli_status_t bf_drv_ucli_ucli__set_trace_level__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(
      uc, "set-trace-level", -1, "Set the trace level for a module");
  char help_str[] =
      "set-trace-level <CRIT|ERR|WARN|INFO|DBG> "
      "[SYS|UTIL|LLD|PIPE|TM|MC|PKT|DVM|PM|PORT|DRU|MAP|SWITCHAPI|SAI|"
      "SWITCHD]";

  if (2 != uc->pargs->count && 1 != uc->pargs->count) {
    aim_printf(&uc->pvs, "%s\n", help_str);
    return 0;
  }
  int level = 0;
  if (!strncmp("CRIT", uc->pargs->args[0], 4)) {
    level = BF_LOG_CRIT;
  } else if (!strncmp("ERR", uc->pargs->args[0], 3)) {
    level = BF_LOG_ERR;
  } else if (!strncmp("WARN", uc->pargs->args[0], 4)) {
    level = BF_LOG_WARN;
  } else if (!strncmp("INFO", uc->pargs->args[0], 4)) {
    level = BF_LOG_INFO;
  } else if (!strncmp("DBG", uc->pargs->args[0], 3)) {
    level = BF_LOG_DBG;
  } else {
    aim_printf(&uc->pvs, "%s\n", help_str);
    return 0;
  }

  int module = -1;
  if (2 == uc->pargs->count) {
    if (!strncmp("SYS", uc->pargs->args[1], 3)) {
      module = BF_MOD_SYS;
    } else if (!strncmp("UTIL", uc->pargs->args[1], 4)) {
      module = BF_MOD_UTIL;
    } else if (!strncmp("LLD", uc->pargs->args[1], 3)) {
      module = BF_MOD_LLD;
    } else if (!strncmp("PIPE", uc->pargs->args[1], 4)) {
      module = BF_MOD_PIPE;
    } else if (!strncmp("TM  ", uc->pargs->args[1], 2)) {
      module = BF_MOD_TM;
    } else if (!strncmp("MC  ", uc->pargs->args[1], 2)) {
      module = BF_MOD_MC;
    } else if (!strncmp("PKT ", uc->pargs->args[1], 3)) {
      module = BF_MOD_PKT;
    } else if (!strncmp("DVM ", uc->pargs->args[1], 3)) {
      module = BF_MOD_DVM;
    } else if (!strncmp("PM  ", uc->pargs->args[1], 2)) {
      module = BF_MOD_PM;
    } else if (!strncmp("PORT", uc->pargs->args[1], 4)) {
      module = BF_MOD_PORT;
    } else if (!strncmp("DRU ", uc->pargs->args[1], 3)) {
      module = BF_MOD_DRU;
    } else if (!strncmp("MAP ", uc->pargs->args[1], 3)) {
      module = BF_MOD_MAP;
    } else if (!strncmp("SWITCHAPI", uc->pargs->args[1], 9)) {
      module = BF_MOD_SWITCHAPI;
    } else if (!strncmp("SAI ", uc->pargs->args[1], 3)) {
      module = BF_MOD_SAI;
    } else if (!strncmp("BFRT", uc->pargs->args[1], 4)) {
      module = BF_MOD_BFRT;
    } else if (!strncmp("SWITCHD", uc->pargs->args[1], 7)) {
      module = BF_MOD_SWITCHD;
    } else {
      aim_printf(&uc->pvs, "%s\n", help_str);
      return 0;
    }
  }

  if (-1 == module)
    for (module = BF_MOD_START; module < BF_MOD_MAX; ++module)
      bf_sys_trace_level_set(module, level);
  else
    bf_sys_trace_level_set(module, level);
  return 0;
}

static ucli_status_t bf_drv_ucli_ucli__set_log_level__(ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc, "set-log-level", -1, "Set the log level for a module");
  char help_str[] =
      "set-log-level <CRIT|ERR|WARN|INFO|DBG> <FILE|STDOUT> "
      "[SYS|UTIL|LLD|PIPE|TM|MC|PKT|DVM|PM|PORT|DRU|MAP|SWITCHAPI|SAI|"
      "BFRT|PLTFM|SWITCHD]";

  if (uc->pargs->count < 2) {
    aim_printf(&uc->pvs, "%s\n", help_str);
    return 0;
  }
  int level = 0;
  if (!strncmp("CRIT", uc->pargs->args[0], 4)) {
    level = BF_LOG_CRIT;
  } else if (!strncmp("ERR", uc->pargs->args[0], 3)) {
    level = BF_LOG_ERR;
  } else if (!strncmp("WARN", uc->pargs->args[0], 4)) {
    level = BF_LOG_WARN;
  } else if (!strncmp("INFO", uc->pargs->args[0], 4)) {
    level = BF_LOG_INFO;
  } else if (!strncmp("DBG", uc->pargs->args[0], 3)) {
    level = BF_LOG_DBG;
  } else {
    aim_printf(&uc->pvs, "%s\n", help_str);
    return 0;
  }

  int output = 0x4;
  if (!strncmp("FILE", uc->pargs->args[1], 4)) {
    output = BF_LOG_DEST_FILE;
  } else if (!strncmp("STDOUT", uc->pargs->args[1], 6)) {
    output = BF_LOG_DEST_STDOUT;
  } else {
    aim_printf(&uc->pvs, "%s\n", help_str);
    return 0;
  }

  int module = -1;
  if (3 == uc->pargs->count) {
    if (!strncmp("SYS", uc->pargs->args[2], 3)) {
      module = BF_MOD_SYS;
    } else if (!strncmp("UTIL", uc->pargs->args[2], 4)) {
      module = BF_MOD_UTIL;
    } else if (!strncmp("LLD", uc->pargs->args[2], 3)) {
      module = BF_MOD_LLD;
    } else if (!strncmp("PIPE", uc->pargs->args[2], 4)) {
      module = BF_MOD_PIPE;
    } else if (!strncmp("TM  ", uc->pargs->args[2], 2)) {
      module = BF_MOD_TM;
    } else if (!strncmp("MC  ", uc->pargs->args[2], 2)) {
      module = BF_MOD_MC;
    } else if (!strncmp("PKT ", uc->pargs->args[2], 3)) {
      module = BF_MOD_PKT;
    } else if (!strncmp("DVM ", uc->pargs->args[2], 3)) {
      module = BF_MOD_DVM;
    } else if (!strncmp("PM  ", uc->pargs->args[2], 2)) {
      module = BF_MOD_PM;
    } else if (!strncmp("PORT", uc->pargs->args[2], 4)) {
      module = BF_MOD_PORT;
    } else if (!strncmp("DRU ", uc->pargs->args[2], 3)) {
      module = BF_MOD_DRU;
    } else if (!strncmp("MAP ", uc->pargs->args[2], 3)) {
      module = BF_MOD_MAP;
    } else if (!strncmp("SWITCHAPI", uc->pargs->args[2], 9)) {
      module = BF_MOD_SWITCHAPI;
    } else if (!strncmp("SAI ", uc->pargs->args[2], 3)) {
      module = BF_MOD_SAI;
    } else if (!strncmp("BFRT", uc->pargs->args[2], 4)) {
      module = BF_MOD_BFRT;
    } else if (!strncmp("PLTFM", uc->pargs->args[2], 5)) {
      module = BF_MOD_PLTFM;
    } else if (!strncmp("SWITCHD", uc->pargs->args[2], 7)) {
      module = BF_MOD_SWITCHD;
    } else {
      aim_printf(&uc->pvs, "%s\n", help_str);
      return 0;
    }
  }

  if (-1 == module) {
    for (module = BF_MOD_START; module < BF_MOD_MAX; ++module) {
      bf_sys_log_level_set(module, output, level);
    }
  } else {
    bf_sys_log_level_set(module, output, level);
  }
  return 0;
}

static ucli_status_t bf_drv_ucli_ucli__trace_buff__(ucli_context_t *uc) {
  size_t size, len_written;
  uint8_t *buf;

  UCLI_COMMAND_INFO(uc, "get_trace", 1, "get_trace <size>");

  char help_str[] = "get_trace <size>";

  if (uc->pargs->count < 1) {
    aim_printf(&uc->pvs, "%s\n", help_str);
    return 0;
  }

  size = (size_t)atoi(uc->pargs->args[0]);
  /* allocate the buffer to read the trace buffer into */
  buf = (uint8_t *)bf_sys_malloc(size);
  if (!buf) {
    printf("Error malloc\n");
    return 0;
  }
  if (bf_sys_trace_get(buf, size, &len_written) == 0) {
    printf("%s\n", buf);
  } else {
    printf("Error reading Trace buffer\n");
  }
  bf_sys_free(buf);
  return 0;
}

ucli_command_handler_f bf_drv_show_tech_ucli_handlers__[] = {
    bf_drv_show_tech_ucli_sys__,
    bf_drv_show_tech_ucli_dvm__,
    bf_drv_show_tech_ucli_lld__,
    bf_drv_show_tech_ucli_pm__,
    bf_drv_show_tech_ucli_port__,
    bf_drv_show_tech_ucli_pipe__,
    bf_drv_show_tech_ucli_tm__,
    bf_drv_show_tech_ucli_mc__,
    NULL};

static ucli_status_t bf_drv_ucli_ucli__show_tech_drivers___(
    ucli_context_t *uc) {
  UCLI_COMMAND_INFO(uc,
                    "show_tech_drivers",
                    -1,
                    " Display verbose system and module parameters. "
                    "Usage -d <dev_id> -m <module_name> "
                    "[-f <to enable file logging>]");

  /* Display existing foirmat for the command. */
  char help_str[] =
      "show_tech_drivers -d [dev_id] -m [SYS|LLD|DVM|PM|PORT|PIPE|TM|MC] "
      "-f [to log the contents in a file]";
  optind = 0; /* reset optind value */

  int argc = uc->pargs->count + 1;
  char *const *argv = (char *const *)&(uc->pargs->args__[0]);
  int c = 0;
  unsigned dev_id = 0, module_bmap = 0;
  bool enable_dev_id = false, partial_mod_dump = false;
  unsigned module_itr = 0;
  char ch = '\n';
  char *fname = "ShowTechLog";
  bool enable_file_log = false;

  /* Parse the incoming specifiers. */
  while ((c = getopt(argc, argv, "d:?m:f")) != -1) {
    switch (c) {
      case 'd':
        if (optarg) {
          dev_id = strtoul(optarg, NULL, 0);
          enable_dev_id = true;
        }
        break;
      case 'm':
        if (optarg) {
          if (strstr(optarg, "SYS")) {
            module_bmap |= 1 << BF_SHOW_TECH_DRV_SYS;
            partial_mod_dump = true;
          }
          if (strstr(optarg, "DVM")) {
            module_bmap |= 1 << BF_SHOW_TECH_DRV_DVM;
            partial_mod_dump = true;
          }
          if (strstr(optarg, "LLD")) {
            module_bmap |= 1 << BF_SHOW_TECH_DRV_LLD;
            partial_mod_dump = true;
          }
          if (strstr(optarg, "PM")) {
            module_bmap |= 1 << BF_SHOW_TECH_DRV_PM;
            partial_mod_dump = true;
          }
          if (strstr(optarg, "PORT")) {
            module_bmap |= 1 << BF_SHOW_TECH_DRV_PORT;
            partial_mod_dump = true;
          }
          if (strstr(optarg, "PIPE")) {
            module_bmap |= 1 << BF_SHOW_TECH_DRV_PIPE;
            partial_mod_dump = true;
          }
          if (strstr(optarg, "TM")) {
            module_bmap |= 1 << BF_SHOW_TECH_DRV_TM;
            partial_mod_dump = true;
          }
          if (strstr(optarg, "MC")) {
            module_bmap |= 1 << BF_SHOW_TECH_DRV_MC;
            partial_mod_dump = true;
          }
        } else {
          module_bmap = 0xff;
        }
        break;
      case 'f':
        enable_file_log = true;
        break;
      case '?':
        aim_printf(&uc->pvs, "%s \n", help_str);
        goto cleanup;
    }
  }
  if (!uc->pargs->count) {
    aim_printf(&uc->pvs, " No Device ID given \n");
    aim_printf(&uc->pvs, "%s \n", help_str);
    return 0;
  }
  /* Invalid argument check. */
  if (enable_dev_id && !is_dev_id_valid(dev_id)) {
    aim_printf(&uc->pvs, "Invalid Device ID : %d \n", dev_id);
    aim_printf(&uc->pvs, "%s\n", help_str);
    return 0;
  }
  /* File logging enabled and all modules */
  if (enable_dev_id && enable_file_log && !partial_mod_dump) {
    module_bmap = 0xff;
  }
  if (!module_bmap && (uc->pargs->count > 1)) {
    aim_printf(&uc->pvs, "Invalid Module specified \n");
    aim_printf(&uc->pvs, "%s\n", help_str);
    return 0;
  }
  /* For incorrect option ie. nothing specified. */
  if (!module_bmap) {
    module_bmap = 0xff;
  }
  /* When dev-id is not specified. */
  if (!enable_dev_id) {
    dev_id = 0;
  }
  if (!uc->pargs->args[0] || !enable_dev_id) {
    if (!uc->pargs->args[0]) {
      uc->pargs->args = (const char **)bf_sys_malloc(sizeof(const char *));
      if (uc->pargs->args == NULL) {
        aim_printf(&uc->pvs, "Failed to allocate buffer for arguments \n");
        return 0;
      }
    }
    if (!uc->pargs->args__[1] && !uc->pargs->count) {
      uc->pargs->args__[1] = (const char *)bf_sys_malloc(sizeof(const char *));
      if (uc->pargs->args__[1] == NULL) {
        aim_printf(&uc->pvs, "Failed to allocate buffer for arguments \n");
        if (uc->pargs->args) {
          bf_sys_free((void *)uc->pargs->args);
        }
        return 0;
      }
    }
    /* Initialize dev-id. */
    uc->pargs->args[0] = (const char *)&dev_id;
  }
  if (module_bmap == 0xff) {
    if (!uc->pargs->args[1]) {
      uc->pargs->args[1] = (const char *)bf_sys_malloc(sizeof(const char *));
      if (uc->pargs->args[1] == NULL) {
        aim_printf(&uc->pvs, "Failed to allocate buffer for arguments \n");
        if (uc->pargs->args) {
          bf_sys_free((void *)uc->pargs->args);
        }
        return 0;
      }
    }
    /*
     * Its going to run for all the modules.
     * Hence, confirmation from user is required.
     */
    aim_printf(
        &uc->pvs,
        "Do you wish to proceed to display contents of all modules [y/n] ");
    ch = getchar();
    if (ch == 'n') {
      goto cleanup;
    } else if (ch != 'y') {
      aim_printf(&uc->pvs, " Invalid option given \n ");
      goto cleanup;
    }
  }
  if (enable_file_log) {
    aim_printf(&uc->pvs,
               " File logging enabled. Contents will be dumped in a file named "
               "ShowTechLog\n");
    ucli_log_file_open(fname, uc);
    /*
     * If file enabled then reduce one argument
     * as its not required in below commands.
     */
    if (uc->pargs->count > 2) {
      uc->pargs->count--;
    }
  }
  while (module_bmap && (module_itr < BF_SHOW_TECH_DRV_MAX)) {
    /* Iterate through all the module specific Handlers. */
    if (module_bmap & 0x1) {
      if (bf_drv_show_tech_ucli_handlers__[module_itr](uc)) {
        bf_drv_show_tech_ucli_handlers__[module_itr](uc);
      }
    }
    module_bmap >>= 1;
    module_itr++;
  }
  /* Reset file logging. */
  if (enable_file_log) {
    ucli_log_file_close(uc);
    enable_file_log = false;
  }

cleanup:
  /* Free allocated memory. */
  if (!uc->pargs->count) {
    if (uc->pargs->args__[1]) {
      bf_sys_free((void *)uc->pargs->args__[1]);
    }
    if (uc->pargs->args[1]) {
      bf_sys_free((void *)uc->pargs->args[1]);
    }
    if (uc->pargs->args) {
      bf_sys_free((void *)uc->pargs->args);
    }
  }
  return 0;
}

static ucli_command_handler_f bf_drv_ucli_handlers__[] = {
    bf_drv_ucli_ucli__dump_trace__,
    bf_drv_ucli_ucli__reset_trace__,
    bf_drv_ucli_ucli__set_trace_level__,
    bf_drv_ucli_ucli__set_log_level__,
    bf_drv_ucli_ucli__trace_buff__,
    bf_drv_ucli_ucli__version,
    bf_drv_ucli_ucli__show_tech_drivers___,
    NULL};

static ucli_module_t bf_drv_ucli_mod = {"bf-sde", NULL, bf_drv_ucli_handlers__};

typedef struct bf_drv_ctl_s {
  bf_sys_thread_t thread_id;
  ucli_t *uc;
} bf_drv_ctl_t;

static bf_drv_ctl_t _ctl = {0, NULL};

static void *ucli_thread(void *arg) {
  bf_drv_ctl_t *ctl = (bf_drv_ctl_t *)arg;

  (void)ucli_run(ctl->uc, "bf-sde", stdin, stdout, stderr);

  return NULL;
}

void bf_drv_ucli_run(FILE *fin, FILE *fout) {
  if (_ctl.uc == NULL) {
    printf("UCLI init not done..Please wait \n");
    return;
  }
  printf("Starting UCLI from bf-shell \n");
  (void)ucli_run(_ctl.uc, "bf-sde", fin, fout, fout);
}

bf_status_t bf_drv_shell_init(void) {
  ucli_t *uc;

  /* Return if shell init has already been done */
  if (_ctl.uc != NULL) {
    return BF_SUCCESS;
  }

  ucli_init();

  ucli_module_init(&bf_drv_ucli_mod);
  _ctl.uc = uc = ucli_create("bf-sde", &bf_drv_ucli_mod, NULL);

  ucli_node_t *x = NULL;

  extern ucli_node_t *dvm_ucli_node_create(void);
  x = dvm_ucli_node_create();
  ucli_node_add(uc, x);

  extern ucli_node_t *lldlib_ucli_node_create(void);
  x = lldlib_ucli_node_create();
  ucli_node_add(uc, x);

  extern ucli_node_t *devdiag_ucli_node_create(void);
  x = devdiag_ucli_node_create();
  ucli_node_add(uc, x);

  extern ucli_node_t *port_mgr_ucli_node_create(void);
  x = port_mgr_ucli_node_create();
  ucli_node_add(uc, x);

  extern ucli_node_t *mc_mgr_ucli_node_create(void);
  x = mc_mgr_ucli_node_create();
  ucli_node_add(uc, x);

  extern ucli_node_t *pipe_mgr_ucli_node_create(void);
  x = pipe_mgr_ucli_node_create();
  ucli_node_add(uc, x);

  extern ucli_node_t *traffic_mgr_ucli_node_create(void);
  x = traffic_mgr_ucli_node_create();
  ucli_node_add(uc, x);

  extern ucli_node_t *bf_pm_ucli_node_create(void);
  x = bf_pm_ucli_node_create();
  ucli_node_add(uc, x);

  extern ucli_node_t *pkt_mgr_ucli_node_create(void);
  x = pkt_mgr_ucli_node_create();
  ucli_node_add(uc, x);

  extern ucli_node_t *perf_ucli_node_create(void);
  x = perf_ucli_node_create();
  ucli_node_add(uc, x);

  return BF_SUCCESS;
}

bf_status_t bf_drv_shell_start(void) {
  int rv = 0;

  rv = bf_sys_thread_create(&_ctl.thread_id, ucli_thread, &_ctl, 0);
  if (rv < 0) {
    return BF_NO_SYS_RESOURCES;
  }
  bf_sys_thread_set_name(_ctl.thread_id, "bf_ucli_shell");

  return BF_SUCCESS;
}

bf_status_t bf_drv_shell_stop(void) {
  bf_sys_thread_cancel(_ctl.thread_id);
  bf_sys_thread_join(_ctl.thread_id, NULL);
  _ctl.thread_id = 0;

  return BF_SUCCESS;
}

bf_status_t bf_drv_shell_register_ucli(ucli_node_t *ucli_node) {
  ucli_node_add(_ctl.uc, ucli_node);
  return BF_SUCCESS;
}

bf_status_t bf_drv_shell_unregister_ucli(ucli_node_t *ucli_node) {
  ucli_node_remove(_ctl.uc, ucli_node);
  return BF_SUCCESS;
}
