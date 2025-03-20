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
    bf_switchd.c
*/

/* Standard includes */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <getopt.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>
#ifdef __sun
#include <endian.h>
#define ILLUMOS_UNUSED(expr) do { (void)(expr); } while (0)
#endif

/* <bf_syslib> includes */
#include <target-sys/bf_sal/bf_sys_intf.h>

/* <clish> includes  */
#include <target-utils/clish/thread.h>

/* <bf_driver> includes */
#include <dru_sim/dru_sim.h>
#include <dvm/bf_drv_intf.h>
#include <lld/bf_dma_if.h>
#include <lld/bf_dev_if.h>
#include <tofino/pdfixed/pd_conn_mgr.h>
#include <mc_mgr/mc_mgr_intf.h>
#include <pkt_mgr/pkt_mgr_intf.h>
#include <traffic_mgr/traffic_mgr.h>
#include <lld/lld_err.h>
#include <lld/lld_sku.h>
#include <lld/bf_int_if.h>
#include <port_mgr/bf_port_if.h>
#include <tofino/pdfixed/pd_conn_mgr.h>
#include <tofino/bf_pal/bf_pal_port_intf.h>
#include <tofino/bf_pal/dev_intf.h>
#include <tofino/bf_pal/pltfm_func_mgr.h>
#include <tofino/bf_pal/pltfm_intf.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include <bf_pm/bf_pm_intf.h>
#include <bf_rt/bf_rt_init.h>
/* Required for lld_sku API */
#include <lld/lld_dr_if.h>

#ifdef __sun
#include <illumos.h>
#else
#include <kdrv/bf_kdrv/bf_ioctl.h>
#endif

/* Required for python shell */
#include <lld/python_shell_mutex.h>

#ifdef THRIFT_ENABLED
#include <pdfixed_thrift/bfn_pd_rpc_server.h>
#endif
#ifdef P4RT_ENABLED
#include "p4_rt/server/server.h"
#endif
#ifdef TDI_ENABLED
#include <tdi_tofino/c_frontend/tdi_tofino_init.h>
#endif
#ifdef GRPC_ENABLED
#include <bf_rt/proto/bf_rt_server.h>
#endif

/* Local includes */
#include "switch_config.h"
#include "bf_model_pltfm_porting.h"
#include "bf_hw_porting_config.h"

#ifdef INCLUDE_SERDES_PKG
#include <port_mgr/bf_serdes_if.h>
#endif

/******************************************************************************
*******************************************************************************
                          BF_SWITCHD OPERATIONAL MODE SETTINGS
*******************************************************************************
******************************************************************************/

#ifdef FASTRECFG_TEST
#include <tofino/pdfixed/pd_devport_mgr.h>
#endif

#include "bf_switchd.h"
#ifdef REG_ACCESS_LOG
#include "bf_switchd_log.h"
#endif

#include <tofino_regs/tofino.h>
#include <tof2_regs/tof2_reg_drv.h>
#include <tof3_regs/tof3_reg_drv.h>

#define PCI_VENDOR_ID_TOF 0x1d1c
#define PCI_DEV_ID_TOF_A0 0x01
#define PCI_DEV_ID_TOF_B0 0x10
#define PCI_DEV_ID_TOF2_A0 0x0100
#define PCI_DEV_ID_TOF2_B0 0x0110
#define PCI_DEV_ID_TOF3_A0 0x0DA2

/* Multi subdevice case */
#define TOFINO3_MULTI_SUBDEV_MODE 1
/* default case is multi device  (with one subdevice each) */

#ifdef PERF_TEST
/* Increase the DMA memory size for perf-test. This is so we have enough
 * to fill and not having to ever wait for the completions from the model
 */
#define TOF_DMA_INST_LIST_SIZE (145 * 1024 * 1024)
#define TOF_DMA_INST_LIST_ADDITIONAL_3 (0)
#define TOF_DMA_INST_LIST_ADDITIONAL_4 (0)
#define TOF2_DMA_INST_LIST_SIZE (120 * 1024 * 1024)
#define TOF2_DMA_INST_LIST_ADDITIONAL_3 (0)
#define TOF2_DMA_INST_LIST_ADDITIONAL_4 (0)
#define TOF3_DMA_INST_LIST_SIZE (120 * 1024 * 1024)
#define TOF3_DMA_INST_LIST_ADDITIONAL_3 (0)
#define TOF3_DMA_INST_LIST_ADDITIONAL_4 (0)



#define DMA_INST_BUF_SIZE (32 * 1024)
#define DMA_PKT_TX_SIZE (64 * 1024)
#else
#define TOF_DMA_INST_LIST_SIZE (62 * 1024 * 1024)
#define TOF_DMA_INST_LIST_ADDITIONAL_3 (12 * 1024 * 1024)
#define TOF_DMA_INST_LIST_ADDITIONAL_4 (28 * 1024 * 1024)
#define TOF2_DMA_INST_LIST_SIZE (80 * 1024 * 1024)
#define TOF2_DMA_INST_LIST_ADDITIONAL_3 (40 * 1024 * 1024)
#define TOF2_DMA_INST_LIST_ADDITIONAL_4 (72 * 1024 * 1024)
#define TOF3_DMA_INST_LIST_SIZE (80 * 1024 * 1024)
#define TOF3_DMA_INST_LIST_ADDITIONAL_3 (40 * 1024 * 1024)
#define TOF3_DMA_INST_LIST_ADDITIONAL_4 (72 * 1024 * 1024)



#define DMA_INST_BUF_SIZE (16 * 1024)
#define DMA_PKT_TX_SIZE (2 * 1024 * 1024)
#endif

extern bf_pltfm_reg_dir_i2c_rd reg_dir_i2c_rd_func;
extern bf_pltfm_reg_dir_i2c_wr reg_dir_i2c_wr_func;
extern void diagnostic_python_interface(bool *local_only);

/* Local defines */
typedef void *(*start_routine)(void *);
typedef void *pvoid_dl_t __attribute__((__may_alias__));
static bf_switchd_internal_context_t *switchd_ctx = NULL;
static int interrupt_cnt[512] = {0};

/* Compile out the diagnostic python interface by default */
#undef DIAG_PYTHON_SUPPORT
/* un-comment to open diag serdes TCP port */
//#define DIAG_PYTHON_SUPPORT
#ifdef DIAG_PYTHON_SUPPORT
void start_credo_py_server(bool *local_only);
#endif  // DIAG_PYTHON_SUPPORT

void (*bf_fpga_reg_write32_fn)(int fd, uint32_t offset, uint32_t val);
int (*bf_fpga_reg_read32_fn)(int fd, uint32_t offset, uint32_t *val);

#ifndef __sun
static int bf_switchd_sysfs_set(char *file_node,
                                char *sysfs_fn,
                                char *val,
                                size_t val_len) {
  int fd, ret;
  char file_name[sizeof(((switchd_pcie_cfg_t *)0)->pci_sysfs_str) + 16];

  snprintf(file_name, sizeof(file_name), "%s/%s", file_node, sysfs_fn);

  fd = open(file_name, O_WRONLY);
  if (fd == -1) {
    return fd;
  }
  ret = write(fd, val, val_len);
  if (ret != (int)val_len) {
    ret = -1;
  } else {
    ret = 0;
  }
  close(fd);
  return ret;
}

static int bf_switchd_sysfs_get(char *file_node,
                                char *sysfs_fn,
                                uint8_t *val,
                                size_t val_len) {
  int fd, ret;
  char file_name[sizeof(((switchd_pcie_cfg_t *)0)->pci_sysfs_str) + 16];

  snprintf(file_name, sizeof(file_name), "%s/%s", file_node, sysfs_fn);

  fd = open(file_name, O_RDONLY);
  if (fd == -1) {
    return fd;
  }
  ret = read(fd, val, val_len);
  close(fd);
  return ret;
}
#else // __sun

static int
get_pci_dev_id(char *devname, uint8_t *buf, ssize_t len)
{
	int fd, rval;
	uint32_t devid;

	fd = open(devname, O_RDONLY);
	if (fd < 0) {
		printf("failed to open %s: %s\n", devname, strerror(errno));
		return (-1);
	}

	rval = ioctl(fd, BF_GET_PCI_DEVID, &devid);
	if (rval < 0) {
		printf("failed to get pci devid: %s\n", strerror(errno));
	} else {
		printf("ioctl() returned %d\n", devid);
		snprintf((char *)buf, len, "%d", devid);
	}
	close(fd);

	return (rval);
}

static char *
test_device_name(char *name)
{
	char *path = NULL;
	struct stat buf;

	if ((stat(name, &buf) == 0) && S_ISCHR(buf.st_mode)) {
		path = malloc(strlen(name) + 1);
		sprintf(path, name);
	}
	return (path);
}

/* Find a single tofino device */
static char *
find_tofino_device()
{
	char *path = NULL;
	DIR *dirp;
	struct dirent *dp;

	/*  On old kernels, the path is hardcoded to /dev/tofino */
	if ((path = test_device_name("/dev/tofino")) != NULL)
		return (path);

	/* New kernels support multiple tofino devices in /dev/tofino/ */
	if ((dirp = opendir("/dev/tofino")) == NULL)
		return (NULL);

	while ((dp = readdir(dirp)) != NULL) {
		char tmp_name[256];
		sprintf(tmp_name, "/dev/tofino/%s", dp->d_name);
		if ((path = test_device_name(tmp_name)) != NULL)
			break;
	}

	closedir(dirp);
	return (path);
}

#endif // __sun

static void bf_switchd_asic_set_pci_dev_id(switch_asic_t *bf_asic) {
  uint8_t buf[8];
  int res = -1;

#if __sun
  char *path = find_tofino_device();

  if (path == NULL) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "ERROR unable to find tofino device");
  } else {
   if ((res = get_pci_dev_id(path, buf, 8)) < 0)
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "ERROR getting ASIC pci device id for %s", path);
   free(path);
  }

  if (res < 0)
	  return;

#else
  res = bf_switchd_sysfs_get(
      bf_asic->pcie_cfg.pci_sysfs_str, "device", buf, sizeof(buf));
  if (res == -1) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "ERROR getting ASIC pci device id for %s",
                         bf_asic->pcie_cfg.pci_sysfs_str);
    return;
  }
#endif // __sun
  bf_asic->pci_dev_id = strtol((char *)buf, NULL, 0);
  bf_sys_log_and_trace(BF_MOD_SWITCHD,
                       BF_LOG_DBG,
                       "ASIC pci device id is %d (0x%04x)",
                       bf_asic->pci_dev_id,
                       bf_asic->pci_dev_id);
}

/* map dma address to bus address using bf_kdrv ioctl */
static int bf_switchd_dmamap(bf_dev_id_t dev_id,
                             uint32_t subdev_id,
                             void *phy_addr,
                             size_t size,
                             void **dma_addr) {
  bf_dma_bus_map_t dma_map;
  if (!bf_dev_id_validate(dev_id)) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "ERROR: invalid dev_id (%d) in %s",
                         dev_id,
                         __func__);
    return -1;
  }

  int fd = switchd_ctx->pcie_map[dev_id][0].dev_fd;
  (void)subdev_id;

  dma_map.phy_addr = (phys_addr_t)(uintptr_t)phy_addr;
  dma_map.size = size;
  dma_map.dma_addr = NULL;
  if (fd < 0) {
    return -1;
  }
  if (ioctl(fd, BF_IOCMAPDMAADDR, &dma_map)) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "error in dma map ioctl for phy addr %p",
                         (void *)(uintptr_t)(dma_map.phy_addr));
    return -1;
  }
  *dma_addr = (void *)dma_map.dma_addr;
  return 0;
}

/* unmap dma bus address using bf_kdrv ioctl */
static int bf_switchd_dmaunmap(bf_dev_id_t dev_id,
                               uint32_t subdev_id,
                               void *dma_addr,
                               size_t size) {
  bf_dma_bus_map_t dma_map;
  if (!bf_dev_id_validate(dev_id)) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "ERROR: invalid dev_id (%d) in %s",
                         dev_id,
                         __func__);
    return -1;
  }
  int fd = switchd_ctx->pcie_map[dev_id][0].dev_fd;
  (void)subdev_id;

  dma_map.dma_addr = (void *)(uintptr_t)dma_addr;
  dma_map.size = size;
  if (fd < 0) {
    return -1;
  }
  if (ioctl(fd, BF_IOCUNMAPDMAADDR, &dma_map)) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "error in dma unmap ioctl for dma addr %p",
                         dma_addr);
    return -1;
  }
  return 0;
}

/* set msix numbers for TBUS msix in the kernel module.
 * relevant with kernel mode pkt processing only.
 * relevant with type tofino-2 and later.
 * sets <cnt> number fromthe array <indices>
 */
static int bf_switchd_tbus_msix_index_set(int fd, int cnt, int *indices) {
  bf_tbus_msix_indices_t tbus_msix;
  int i;

  tbus_msix.cnt = cnt;
  for (i = 0; i < cnt; i++) {
    tbus_msix.indices[i] = indices[i];
  }
  return (ioctl(fd, BF_TBUS_MSIX_INDEX, &tbus_msix));
}

static int bf_switchd_int_mode_get(int fd, enum bf_intr_mode *intr_mode) {
  bf_intr_mode_t intr_mode_ioctl;

  if (ioctl(fd, BF_GET_INTR_MODE, &intr_mode_ioctl)) {
    *intr_mode = BF_INTR_MODE_NONE;
    return -1;
  } else {
    *intr_mode = intr_mode_ioctl.intr_mode;
    return 0;
  }
}

#ifndef STATIC_LINK_LIB
/* Helper function to locate a symbol in a dll */
static void bf_switchd_find_lib_fn(const char *fn_name,
                                   void *dl_handle,
                                   pvoid_dl_t *fn) {
  char *error;
  *fn = dlsym(dl_handle, fn_name);
  if ((error = dlerror()) != NULL) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "ERROR:%s: %d: looking up '%s' func, err=%s ",
                         __func__,
                         __LINE__,
                         fn_name,
                         error);
  }
}

/* Initialize P4 PD API library */
static void bf_switchd_pd_lib_init(bf_dev_id_t dev_id) {
  int j = 0;
  p4_devices_t *p4_device = &(switchd_ctx->p4_devices[dev_id]);
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);
  char pd_init_fn_name[250];
  void (*pd_init_fn)(void);
  int i = 0;

  if (!state || (!p4_device)) {
    return;
  }

  for (j = 0; j < p4_device->num_p4_programs; j++) {
    p4_programs_t *p4_programs = &(p4_device->p4_programs[j]);
    switchd_p4_program_state_t *p4_program_state =
        &(state->p4_programs_state[j]);

    for (i = 0; i < p4_programs->num_p4_pipelines; i++) {
      p4_pipeline_config_t *p4_pipeline = &(p4_programs->p4_pipelines[i]);
      switchd_p4_pipeline_state_t *pipeline_state =
          &(p4_program_state->p4_pipeline_state[i]);

      if (pipeline_state->pd_lib_handle == NULL) {
        continue;
      }

      const char *p4_prefix = p4_programs->program_name;
      if (strcmp(p4_prefix, "switch") == 0) {
        p4_prefix = "dc";
      }

      /* Initialize the PD API lib */
      sprintf(pd_init_fn_name, "p4_pd_%s_init", p4_prefix);
      bf_switchd_find_lib_fn(pd_init_fn_name,
                             pipeline_state->pd_lib_handle,
                             (pvoid_dl_t *)&pd_init_fn);
      bf_sys_assert(pd_init_fn != NULL);
      pd_init_fn();

      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_DBG,
          "bf_switchd: libpd initialized for p4 pipeline %s on dev %d",
          p4_pipeline->p4_pipeline_name,
          dev_id);
    }
  }
}

#ifdef THRIFT_ENABLED
/* Initialize P4 PD THRIFT API library */
static void bf_switchd_pd_thrift_lib_init(bf_dev_id_t dev_id) {
  int j = 0;
  p4_devices_t *p4_device = &(switchd_ctx->p4_devices[dev_id]);
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);
  int (*add_to_rpc_fn)(void *);
  char *error;
  int i = 0;

  if (!state || (!p4_device)) {
    return;
  }

  for (j = 0; j < p4_device->num_p4_programs; j++) {
    p4_programs_t *p4_programs = &(p4_device->p4_programs[j]);
    switchd_p4_program_state_t *p4_program_state =
        &(state->p4_programs_state[j]);

    for (i = 0; i < p4_programs->num_p4_pipelines; i++) {
      p4_pipeline_config_t *p4_pipeline = &(p4_programs->p4_pipelines[i]);
      switchd_p4_pipeline_state_t *pipeline_state =
          &(p4_program_state->p4_pipeline_state[i]);

      if (pipeline_state->pd_thrift_lib_handle == NULL) {
        continue;
      }

      /* Call add_to_rpc_server */
      *(pvoid_dl_t *)(&add_to_rpc_fn) =
          dlsym(pipeline_state->pd_thrift_lib_handle, "add_to_rpc_server");
      if ((error = dlerror()) != NULL) {
        bf_sys_log_and_trace(
            BF_MOD_SWITCHD,
            BF_LOG_ERR,
            "%s: %d: Error in looking up add_to_rpc func, err=%s ",
            __func__,
            __LINE__,
            error);
      }
      bf_sys_assert(add_to_rpc_fn != NULL);
      add_to_rpc_fn(switchd_ctx->rpc_server_cookie);
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_DBG,
          "bf_switchd: libpdthrift initialized for p4 pipeline %s on dev %d",
          p4_pipeline->p4_pipeline_name,
          dev_id);
    }
  }
}

static void bf_switchd_pd_thrift_lib_deinit(
    bf_dev_id_t dev_id,
    p4_pipeline_config_t *p4_pipeline,
    switchd_p4_pipeline_state_t *pipeline_state) {
  int (*rmv_from_rpc_fn)(void *);
  char *error;

  if (!p4_pipeline || (!pipeline_state)) {
    return;
  }

  if (pipeline_state->pd_thrift_lib_handle == NULL) {
    return;
  }

  /* Call from_from_rpc_server */
  *(pvoid_dl_t *)(&rmv_from_rpc_fn) =
      dlsym(pipeline_state->pd_thrift_lib_handle, "rmv_from_rpc_server");
  if ((error = dlerror()) != NULL) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD,
        BF_LOG_ERR,
        "%s: %d: Error in looking up rmv_from_rpc func, err=%s ",
        __func__,
        __LINE__,
        error);
  }
  bf_sys_assert(rmv_from_rpc_fn != NULL);
  rmv_from_rpc_fn(switchd_ctx->rpc_server_cookie);
  bf_sys_log_and_trace(
      BF_MOD_SWITCHD,
      BF_LOG_DBG,
      "bf_switchd: libpdthrift deinitialized for pipeline %s on dev %d",
      p4_pipeline->p4_pipeline_name,
      dev_id);
}
#endif  // THRIFT_ENABLED
#endif  // STATIC_LINK_LIB

#ifdef THRIFT_ENABLED
/* Initialize agents' thrift libraries */
static void bf_switchd_agent_thrift_lib_init(bf_dev_id_t dev_id) {
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);
#ifdef STATIC_LINK_LIB
  if (!state) {
    return;
  }

  if (switchd_ctx->agent_rpc_server_thrift_service_add_fn) {
    switchd_ctx->agent_rpc_server_thrift_service_add_fn(
        switchd_ctx->rpc_server_cookie);
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_DBG,
                         "bf_switchd: thrift initialized for agent0");
  }

#else   // STATIC_LINK_LIB
  int (*add_to_rpc_fn)(void *);
  char *error;
  char *add_to_rpc_fn_name = "agent_rpc_server_thrift_service_add";
  int agent_idx;

  for (agent_idx = 0; agent_idx < BF_SWITCHD_MAX_AGENTS; agent_idx++) {
    if (state->agent_lib_handle[agent_idx] == NULL) {
      continue;
    }

    *(pvoid_dl_t *)(&add_to_rpc_fn) =
        dlsym(state->agent_lib_handle[agent_idx], add_to_rpc_fn_name);
    if ((error = dlerror()) != NULL) {
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "%s: %d: Error in looking up %s func, err=%s ",
                           __func__,
                           __LINE__,
                           add_to_rpc_fn_name,
                           error);
    }
    bf_sys_assert(add_to_rpc_fn != NULL);
    add_to_rpc_fn(switchd_ctx->rpc_server_cookie);
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_DBG,
                         "bf_switchd: thrift initialized for agent : %d",
                         agent_idx);
  }
#endif  // STATIC_LINK_LIB
}
/* Deinitialize agents' thrift libraries */
static void bf_switchd_agent_thrift_lib_deinit(bf_dev_id_t dev_id) {
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);
#ifdef STATIC_LINK_LIB
  if (!state) {
    return;
  }

  if (switchd_ctx->agent_rpc_server_thrift_service_rmv_fn) {
    switchd_ctx->agent_rpc_server_thrift_service_rmv_fn(
        switchd_ctx->rpc_server_cookie);
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_DBG,
                         "bf_switchd: thrift desinitialized for agent0");
  }

#else   // STATIC_LINK_LIB
  int (*rmv_from_rpc_fn)(void *);
  char *error;
  char *rmv_from_rpc_fn_name = "agent_rpc_server_thrift_service_rmv";
  int agent_idx;

  for (agent_idx = 0; agent_idx < BF_SWITCHD_MAX_AGENTS; agent_idx++) {
    if (state->agent_lib_handle[agent_idx] == NULL) {
      continue;
    }

    *(pvoid_dl_t *)(&rmv_from_rpc_fn) =
        dlsym(state->agent_lib_handle[agent_idx], rmv_from_rpc_fn_name);
    if ((error = dlerror()) != NULL) {
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "%s: %d: Error in looking up %s func, err=%s ",
                           __func__,
                           __LINE__,
                           rmv_from_rpc_fn_name,
                           error);
    }
    bf_sys_assert(rmv_from_rpc_fn != NULL);
    rmv_from_rpc_fn(switchd_ctx->rpc_server_cookie);
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_DBG,
                         "bf_switchd: thrift deinitialized for agent : %d",
                         agent_idx);
  }
#endif  // STATIC_LINK_LIB
}
#endif  // THRIFT_ENABLED

#ifndef STATIC_LINK_LIB
/* Initialize switch.p4 SWITCHAPI library */
static void bf_switchd_switchapi_lib_init(bf_dev_id_t dev_id) {
  int j = 0;
  p4_devices_t *p4_device = &(switchd_ctx->p4_devices[dev_id]);
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);
  bf_dev_init_mode_t init_mode = switchd_ctx->args.init_mode;
  bool warm_boot = (init_mode != BF_DEV_INIT_COLD) ? true : false;

  int (*switchapi_init_fn)(int, int, char *, bool);
  int (*switchapi_start_srv_fn)(void);
  int (*switchapi_start_packet_driver_fn)();
  int (*bf_switch_init_fn)(uint16_t, char *, char *, bool, char *);

  /* Currently the switch libraries need to be initialized only once */
  if (dev_id != 0) {
    return;
  }

  for (j = 0; j < p4_device->num_p4_programs; j++) {
    p4_programs_t *p4_programs = &(p4_device->p4_programs[j]);
    switchd_p4_program_state_t *p4_program_state =
        &(state->p4_programs_state[j]);

    if (p4_program_state->switchapi_lib_handle == NULL) {
      continue;
    }

    /* Initialize the P4-16 switch API lib */
    *(pvoid_dl_t *)(&bf_switch_init_fn) =
        dlsym(p4_program_state->switchapi_lib_handle, "bf_switch_init");
    if (bf_switch_init_fn) {
      bf_switch_init_fn(dev_id,
                        switchd_ctx->args.conf_file,
                        switchd_ctx->args.install_dir,
                        warm_boot,
                        "/tmp/db.txt");
    }

    *(pvoid_dl_t *)(&switchapi_init_fn) =
        dlsym(p4_program_state->switchapi_lib_handle, "switch_api_init");
    if (switchapi_init_fn) {
      /* Initialize the P4-14 switch API library. */
      if (p4_programs->use_eth_cpu_port) {
        switchapi_init_fn(dev_id,
                          0,
                          p4_programs->eth_cpu_port_name,
                          p4_programs->add_ports_to_switchapi);
      } else {
        switchapi_init_fn(dev_id, 0, NULL, p4_programs->add_ports_to_switchapi);
      }

      /* Initialize the P4-14 switch API RPC server. */
      *(pvoid_dl_t *)(&switchapi_start_srv_fn) =
          dlsym(p4_program_state->switchapi_lib_handle,
                "start_switch_api_rpc_server");
      if (switchapi_start_srv_fn) {
        switchapi_start_srv_fn();
      }
    } else {
      /* Initialize the P4-16 switch API RPC server. */
      *(pvoid_dl_t *)(&switchapi_start_srv_fn) =
          dlsym(p4_program_state->switchapi_lib_handle,
                "start_bf_switcht_api_rpc_server");
      if (switchapi_start_srv_fn) {
        switchapi_start_srv_fn();
      }
    }
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD, BF_LOG_DBG, "bf_switchd: switchapi initialized");

    /* Initialize the P4-14 switch API packet driver. */
    *(pvoid_dl_t *)(&switchapi_start_packet_driver_fn) =
        dlsym(p4_program_state->switchapi_lib_handle,
              "start_switch_api_packet_driver");
    if (switchapi_start_packet_driver_fn) {
      switchapi_start_packet_driver_fn();
    }
    /* Load switchapi only once */
    return;
  }
}
#endif  // STATIC_LINK_LIB

#ifndef STATIC_LINK_LIB
/* Initialize switch.p4 SWITCHSAI library */
static void bf_switchd_switchsai_lib_init(bf_dev_id_t dev_id) {
  int j = 0;
  p4_devices_t *p4_device = &(switchd_ctx->p4_devices[dev_id]);
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);

  int (*switchsai_start_srv_fn)(char *);
  void (*switchsai_init_fn)();
  void (*smi_sai_init_fn)(uint16_t, char *, char *);

  /* Currently the switch libraries need to be initialized only once */
  if (dev_id != 0) {
    return;
  }

  for (j = 0; j < p4_device->num_p4_programs; j++) {
    p4_programs_t *p4_programs = &(p4_device->p4_programs[j]);
    switchd_p4_program_state_t *p4_program_state =
        &(state->p4_programs_state[j]);

    if (p4_program_state->switchsai_lib_handle == NULL) {
      continue;
    }

    if (!p4_programs->add_ports_to_switchapi) {
      continue;
    }

    /* P4-14 switch SAI library initialization. */
    *(pvoid_dl_t *)(&switchsai_init_fn) =
        dlsym(p4_program_state->switchsai_lib_handle, "switch_sai_init");
    if (switchsai_init_fn) {
      switchsai_init_fn();
    }

    /* P4-16 switch SAI library initialization. */
    *(pvoid_dl_t *)(&smi_sai_init_fn) =
        dlsym(p4_program_state->switchsai_lib_handle, "smi_sai_init");
    if (smi_sai_init_fn) {
      smi_sai_init_fn(
          dev_id, switchd_ctx->args.conf_file, switchd_ctx->args.install_dir);
    }

    /* P4-16 or P4-14 SAI RPC server initialization. */
    *(pvoid_dl_t *)(&switchsai_start_srv_fn) =
        dlsym(p4_program_state->switchsai_lib_handle,
              "start_p4_sai_thrift_rpc_server");
    if (switchsai_start_srv_fn) {
      switchsai_start_srv_fn("9092");
    }

    bf_sys_log_and_trace(
        BF_MOD_SWITCHD, BF_LOG_DBG, "bf_switchd: switchsai initialized");
    /* Load switchsai only once */
    return;
  }
}
#endif  // STATIC_LINK_LIB

/* Initialize agent library */
static void bf_switchd_agent_lib_init(bf_dev_id_t dev_id) {
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);
  char agent_thr_name[16]; /* 16 - MAX pthread name including NULL char */
  int agent_idx = 0, ret = 0;
#ifndef STATIC_LINK_LIB
  agent_init_fn_t agent_init_fn;
#endif  // STATIC_LINK_LIB

  if (!state) {
    return;
  }

  /* Currently the agent libraries need to be initialized only once */
  if (dev_id != 0) {
    return;
  }

#ifdef STATIC_LINK_LIB
  if (switchd_ctx->bf_switchd_agent_init_fn) {
    /* Start the agent thread */
    pthread_attr_t agent_t_attr;
    pthread_attr_init(&agent_t_attr);
    ret = pthread_create(&(switchd_ctx->args.agent_t_id[agent_idx]),
                         &agent_t_attr,
                         switchd_ctx->bf_switchd_agent_init_fn,
                         (void *)switchd_ctx);
    pthread_attr_destroy(&agent_t_attr);
    if (ret != 0) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "ERROR: thread creation failed for agent[%d] service: %d",
          agent_idx,
          ret);
      return;
    }
    snprintf(agent_thr_name, sizeof(agent_thr_name), "bf_agent%d", agent_idx);
    pthread_setname_np(switchd_ctx->args.agent_t_id[agent_idx], agent_thr_name);
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_DBG,
                         "bf_switchd: agent[%d] initialized",
                         agent_idx);
  } else {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD,
        BF_LOG_ERR,
        "ERROR: bf_switchd: agent0 init function not registered");
  }

#else   // STATIC_LINK_LIB
  for (agent_idx = 0; agent_idx < BF_SWITCHD_MAX_AGENTS; agent_idx++) {
    if (state->agent_lib_handle[agent_idx] == NULL) {
      continue;
    }

    /* Initialize the agent API lib */
    *(pvoid_dl_t *)(&agent_init_fn) =
        dlsym(state->agent_lib_handle[agent_idx], "bf_switchd_agent_init");
    if (agent_init_fn) {
      /* Start the agent thread */
      pthread_attr_t agent_t_attr;
      pthread_attr_init(&agent_t_attr);
      ret = pthread_create(&(switchd_ctx->args.agent_t_id[agent_idx]),
                           &agent_t_attr,
                           agent_init_fn,
                           (void *)switchd_ctx);
      pthread_attr_destroy(&agent_t_attr);
      if (ret != 0) {
        bf_sys_log_and_trace(
            BF_MOD_SWITCHD,
            BF_LOG_ERR,
            "ERROR: thread creation failed for agent[%d] service: %d",
            agent_idx,
            ret);
        return;
      }
      snprintf(agent_thr_name, sizeof(agent_thr_name), "bf_agent%d", agent_idx);
      pthread_setname_np(switchd_ctx->args.agent_t_id[agent_idx],
                         agent_thr_name);
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_DBG,
                           "bf_switchd: agent[%d] initialized",
                           agent_idx);
    } else {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "ERROR: bf_switchd: agent[%d] init function not found \n",
          agent_idx);
    }
  }
#endif  // STATIC_LINK_LIB
}

/* Wait for agent library initialization to complete*/
static void bf_switchd_agent_lib_wait_init_done(bf_dev_id_t dev_id) {
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);
  int ret = 0;
#ifndef STATIC_LINK_LIB
  char *fn_name = "bf_switchd_agent_init_done";
  agent_init_done_fn_t agent_init_done_fn;
  int agent_idx = 0;
#endif  // STATIC_LINK_LIB

  if (!state) {
    return;
  }

  /* Currently the agent libraries need to be initialized only once */
  if (dev_id != 0) {
    return;
  }

#ifdef STATIC_LINK_LIB
  if (switchd_ctx->bf_switchd_agent_init_done_fn) {
    ret = 0;
    // Wait till the agent has been initialized
    while (!ret) {
      switchd_ctx->bf_switchd_agent_init_done_fn((void *)&ret);
      // Sleep for sometime before retrying
      usleep(100);
    }
  }

#else   // STATIC_LINK_LIB
  for (agent_idx = 0; agent_idx < BF_SWITCHD_MAX_AGENTS; agent_idx++) {
    if (state->agent_lib_handle[agent_idx] == NULL) {
      continue;
    }

    /* Initialize the agent API lib */
    *(pvoid_dl_t *)(&agent_init_done_fn) =
        dlsym(state->agent_lib_handle[agent_idx], fn_name);
    if (agent_init_done_fn) {
      ret = 0;
      // Wait till the agent has been initialized
      while (!ret) {
        agent_init_done_fn((void *)&ret);
        // Sleep for sometime before retrying
        usleep(100);
      }
    }
  }
#endif  // STATIC_LINK_LIB
}

/* De-Initialize and unload agent library */
static void bf_switchd_agent_lib_deinit(bf_dev_id_t dev_id) {
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);
  int agent_idx = 0, ret = 0;

  if (!state) {
    return;
  }

  if (switchd_ctx->args.bf_switchd_agent_deinit_fn)
    switchd_ctx->args.bf_switchd_agent_deinit_fn();

#ifdef STATIC_LINK_LIB
  if (switchd_ctx->args.agent_t_id[agent_idx]) {
    ret = pthread_cancel(switchd_ctx->args.agent_t_id[agent_idx]);
    if (ret != 0) {
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "ERROR: thread cancel failed for agent[%d] : %d",
                           agent_idx,
                           ret);
    } else {
      ret = pthread_join(switchd_ctx->args.agent_t_id[agent_idx], NULL);
    }
    switchd_ctx->args.agent_t_id[agent_idx] = 0;
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_DBG,
                         "bf_switchd: agent[%d] deinit done for dev_id %d",
                         agent_idx,
                         dev_id);
  }

#else   // STATIC_LINK_LIB
  for (agent_idx = 0; agent_idx < BF_SWITCHD_MAX_AGENTS; agent_idx++) {
    if (state->agent_lib_handle[agent_idx] == NULL) {
      continue;
    }
    /* Cancel the thread before unloading the lib*/
    if (switchd_ctx->args.agent_t_id[agent_idx]) {
      ret = pthread_cancel(switchd_ctx->args.agent_t_id[agent_idx]);
      if (ret != 0) {
        bf_sys_log_and_trace(BF_MOD_SWITCHD,
                             BF_LOG_ERR,
                             "ERROR: thread cancel failed for agent[%d] : %d",
                             agent_idx,
                             ret);
      } else {
        ret = pthread_join(switchd_ctx->args.agent_t_id[agent_idx], NULL);
        if (ret != 0) {
          bf_sys_log_and_trace(BF_MOD_SWITCHD,
                               BF_LOG_ERR,
                               "ERROR: thread join failed for agent[%d] : %d",
                               agent_idx,
                               ret);
        }
      }
      switchd_ctx->args.agent_t_id[agent_idx] = 0;
    }
    dlclose(state->agent_lib_handle[agent_idx]);
    state->agent_lib_handle[agent_idx] = NULL;
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_DBG,
                         "bf_switchd: agent[%d] library unloaded for dev_id %d",
                         agent_idx,
                         dev_id);
  }
#endif  // STATIC_LINK_LIB
}

/* When running against the model all ports are added as 10g ports, this
 * includes CPU ports, recirculation ports, as well as normal MAC ports.  Note
 * that since the Ethernet MACs are not modeled port enable calls are not
 * required. */
static void bf_switchd_ports_add_to_model(bf_dev_id_t dev_id) {
  int max_port, port_step;
  uint32_t num_pipes;
  bf_status_t sts;
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);
  bool skip_mac_ports = false;
  int j = 0;
  p4_devices_t *p4_device = &(switchd_ctx->p4_devices[dev_id]);

  for (j = 0; j < p4_device->num_p4_programs; j++) {
    switchd_p4_program_state_t *p4_program_state =
        &(state->p4_programs_state[j]);
    /* Since switch API needs to manages eth ports by itself skip adding them */
    if (p4_program_state->switchapi_lib_handle ||
        p4_program_state->switchsai_lib_handle) {
      skip_mac_ports = true;
    }
  }

#ifdef STATIC_LINK_LIB
  skip_mac_ports = true;
#endif  // STATIC_LINK_LIB

  /* Setup any internal ports in loopback mode for eligible SKUs. */
  sts = bf_pm_internal_ports_init(dev_id);
  if (sts != BF_SUCCESS) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "ERROR: internal port add failed(%d) for dev_id=%d",
                         sts,
                         dev_id);
  }

  /* Figure out how many pipelines present on this device */
  lld_sku_get_num_active_pipes(dev_id, &num_pipes);

  if (switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO3) {
    max_port = lld_get_max_fp_port(dev_id);
    port_step = 2;
  } else {
    max_port = BF_PIPE_PORT_COUNT - 1;
    port_step = 1;
  }

  /* Add a all of device's ports across all pipelines present */
  for (uint32_t pipe = 0; pipe < num_pipes; pipe++) {
    for (int port = 0; port <= max_port; port += port_step) {
      bf_status_t bf_status;
      bool has_mac = false;
      bf_dev_port_t dev_port = MAKE_DEV_PORT(pipe, port);

      /* Check if this is port is associated with an Ethernet MAC, if so, it
       * should be added with bf_pal_port_add.  Otherwise, use bf_port_add. */
      bf_status = bf_port_has_mac(dev_id, dev_port, &has_mac);
      if (bf_status != BF_SUCCESS) {
        has_mac = false;
      }
      if (has_mac) {
        /* Check for internal ports and skip them, they've already been setup
         * by the call to bf_pm_internal_ports_init. */
        bool internal = false;
        sts = bf_pal_is_port_internal(dev_id, dev_port, &internal);
        if (sts == BF_SUCCESS && internal) continue;
        if (skip_mac_ports) continue;
        sts = bf_pal_port_add(dev_id, dev_port, BF_SPEED_10G, BF_FEC_TYP_NONE);
      } else {
        sts = bf_port_add(dev_id, dev_port, BF_SPEED_10G, BF_FEC_TYP_NONE);
      }
      if (sts != BF_SUCCESS) {
        bf_sys_log_and_trace(BF_MOD_SWITCHD,
                             BF_LOG_ERR,
                             "ERROR: port_add failed(%d) for dev_id=%d, "
                             "dev_port=%d (pipe=%d, port_id=%d)",
                             sts,
                             dev_id,
                             dev_port,
                             pipe,
                             port);
      }
    }
  }
}

#if defined(DEVICE_IS_EMULATOR)
// For the emulator
// Adds all ports at tof:100G, tof2:400G (the speed most commonly used for
// tests)
// Port enables are left to discretion of the user
static void bf_switchd_ports_add_to_emulator(bf_dev_id_t dev_id) {
  unsigned int pipe, port;
  uint32_t num_pipes;
  bf_status_t sts;

  lld_sku_get_num_active_pipes(dev_id, &num_pipes);
  for (pipe = 0; pipe < num_pipes; pipe++) {
    if ((switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO2) ||
        (switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO3)) {
      for (port = 8; port < 72;
           port +=
           8) {  // tof2: front panel port (8-71) has 8 channels per mac.
        /* For tofino3 add ports in 400G-R4 */
        sts = bf_pal_port_add_with_lanes(
            dev_id,
            MAKE_DEV_PORT(pipe, port),
            BF_SPEED_400G,
            (switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO3)
                ? 4
                : 8,
            BF_FEC_TYP_RS);  // default to 400G with 8 chnls configuration
        if (sts != BF_SUCCESS) {
          bf_sys_log_and_trace(
              BF_MOD_SWITCHD,
              BF_LOG_ERR,
              "ERROR: port_add failed(%d) for dev_id=%d, pipe=%d, port_id=%d",
              sts,
              dev_id,
              pipe,
              port);
        }
      }
      // and one loop for the internal ports (note pcie port can only run at
      // 25g)
      for (port = 0; port < 8;) {
        bf_dev_port_t dev_port = MAKE_DEV_PORT(pipe, port);
        bf_status_t bf_status;
        bool has_mac = false;

        /* Dev Port 0 is the PCIe port and must be 25g.
         * Dev Port 2-5 is the Eth CPU port and will be four channels of 100G.
         * All other dev ports will be two channel 100G ports. */
        bool pipe0_on_die = false;
        if ((pipe == 0) || (pipe == BF_SUBDEV_PIPE_COUNT)) {
          pipe0_on_die = true;
        }
        bf_port_speeds_t speed =
            ((port < 2) && pipe0_on_die) ? BF_SPEED_25G : BF_SPEED_100G;
        uint32_t lane_numb = ((port < 2) && pipe0_on_die)
                                 ? 1
                                 : ((port < 6) && pipe0_on_die) ? 4 : 2;
        int inc = 2;
        if ((port < 2) && pipe0_on_die) {
          /* Tofino3 supports only even ports */
          if (switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO3) {
            inc = 2;
          } else {
            inc = 1;
          }
        } else if ((port < 6) && pipe0_on_die)
          inc = 4;
        port += inc;

        /* If port is associated with a MAC then it needs the pm FSM to bring it
         * up.  Other ports, like PCIe port or recirc, don't. */
        bf_status = bf_port_has_mac(dev_id, dev_port, &has_mac);
        if (bf_status != BF_SUCCESS) {
          has_mac = false;
        }
        if (has_mac) {
          sts = bf_pal_port_add_with_lanes(
              dev_id, dev_port, speed, lane_numb, BF_FEC_TYP_NONE);
        } else {
          sts = bf_port_add_with_lane_numb(
              dev_id, dev_port, speed, lane_numb, BF_FEC_TYP_NONE);
        }
        if (sts != BF_SUCCESS) {
          bf_sys_log_and_trace(
              BF_MOD_SWITCHD,
              BF_LOG_ERR,
              "ERROR: port_add failed(%d) for dev_id=%d, dev_port %d",
              sts,
              dev_id,
              dev_port);
        }
      }
    } else {
      for (port = 0; port < 64; port += 4) {
        sts = bf_pal_port_add(
            dev_id, MAKE_DEV_PORT(pipe, port), BF_SPEED_100G, BF_FEC_TYP_NONE);
        if (sts != BF_SUCCESS) {
          bf_sys_log_and_trace(
              BF_MOD_SWITCHD,
              BF_LOG_ERR,
              "ERROR: port_add failed(%d) for dev_id=%d, pipe=%d, port_id=%d",
              sts,
              dev_id,
              pipe,
              port);
        }
      }
      // and one loop for the internal ports (note pcie port can only run at
      // 100g)
      for (port = 64; port < 72; port += 4) {
        bf_status_t bf_status;
        bool has_mac = false;

        /* If port is associated with a MAC then it needs the pm FSM to bring it
         * up.
         * Other ports, like PCIe port or recirc, don't. */
        bf_status =
            bf_port_has_mac(dev_id, MAKE_DEV_PORT(pipe, port), &has_mac);
        if (bf_status != BF_SUCCESS) {
          has_mac = false;
        }
        if (has_mac) {
          sts = bf_pal_port_add(dev_id,
                                MAKE_DEV_PORT(pipe, port),
                                BF_SPEED_100G,
                                BF_FEC_TYP_NONE);
        } else {
          sts = bf_port_add(dev_id,
                            MAKE_DEV_PORT(pipe, port),
                            BF_SPEED_100G,
                            BF_FEC_TYP_NONE);
        }
        if (sts != BF_SUCCESS) {
          bf_sys_log_and_trace(
              BF_MOD_SWITCHD,
              BF_LOG_ERR,
              "ERROR: port_add failed(%d) for dev_id=%d, pipe=%d, port_id=%d",
              sts,
              dev_id,
              pipe,
              port);
        }
      }
    }
  }
}
#endif

// For the ASIC add only non-MAC ports in 100G mode
static void bf_switchd_ports_add_to_asic(bf_dev_id_t dev_id) {
  bf_dev_port_t port;
  int inc;
  uint32_t num_pipes;
  bf_status_t sts;
  bf_dev_port_t pcie_cpu_port = bf_pcie_cpu_port_get(dev_id);
  bf_dev_port_t pcie_cpu_port2 = bf_pcie_cpu_port2_get(dev_id);
  bf_dev_port_t eth_cpu_port = bf_eth_cpu_port_get(dev_id);
  bf_dev_port_t eth_cpu_port2 = bf_eth_cpu_port2_get(dev_id);
  lld_sku_get_num_active_pipes(dev_id, &num_pipes);
  for (bf_dev_pipe_t pipe = 0; pipe < num_pipes; pipe++) {
    switch (switchd_ctx->asic[dev_id].chip_family) {
      case BF_DEV_FAMILY_TOFINO:
        /* The internal ports are 64-71 in each pipe.  Add the non-Ethernet MAC
         * ports here, these are the PCIe CPU port and the recirculation ports.
         */
        for (port = 64; port < 72; port += 4) {
          bf_dev_port_t dev_port = MAKE_DEV_PORT(pipe, port);

          /* The PCIe CPU port and recirculation ports can all be added at 100g
           * speed. */
          if (dev_port != eth_cpu_port) {
            sts = bf_port_add(dev_id, dev_port, BF_SPEED_100G, BF_FEC_TYP_NONE);
            if (sts != BF_SUCCESS) {
              bf_sys_log_and_trace(
                  BF_MOD_SWITCHD,
                  BF_LOG_ERR,
                  "ERROR: port_add failed(%d) for dev_id=%d, dev_port %d",
                  sts,
                  dev_id,
                  dev_port);
            } else {
              bf_sys_log_and_trace(
                  BF_MOD_SWITCHD,
                  BF_LOG_ERR,
		  "port_added for dev_id=%d, dev_port %d\n",
		  dev_id,
		  dev_port);
	    }
          }
        }
        break;
      case BF_DEV_FAMILY_TOFINO2:
        /* The internal ports are 0-7 in each pipe.  Add the non-Ethernet MAC
         * ports here, these are the PCIe CPU port and the recirculation ports.
         */
        for (inc = 0, port = 0; port < 8; port += inc) {
          bf_dev_port_t dev_port = MAKE_DEV_PORT(pipe, port);
          if (dev_port == pcie_cpu_port) {
            /* The PCIe CPU Port only supports 25g. */
            inc = 1;
            sts = bf_port_add(dev_id, dev_port, BF_SPEED_25G, BF_FEC_TYP_NONE);
            if (sts != BF_SUCCESS) {
              bf_sys_log_and_trace(
                  BF_MOD_SWITCHD,
                  BF_LOG_ERR,
                  "ERROR: port_add failed(%d) for dev_id=%d, dev_port %d",
                  sts,
                  dev_id,
                  dev_port);
            }
          } else if (dev_port == eth_cpu_port) {
            /* Do not add the Ethernet CPU port by default, let the application
             * take care of it. */
            inc = 4;
            sts = BF_SUCCESS;
          } else if (dev_port & 1) {
            /* This is NOT a CPU port so it must be a recirculation port.  Since
             * it is an odd number add it as single lane 50g. */
            inc = 1;
            sts = bf_port_add_with_lane_numb(
                dev_id, dev_port, BF_SPEED_50G, 1, BF_FEC_TYP_NONE);
          } else {
            /* This is NOT a CPU port so it must be a recirculation port.  Since
             * it is an even number add it as dual lane 100g. */
            inc = 2;
            sts = bf_port_add_with_lane_numb(
                dev_id, dev_port, BF_SPEED_100G, 2, BF_FEC_TYP_NONE);
          }
          if (sts != BF_SUCCESS) {
            bf_sys_log_and_trace(
                BF_MOD_SWITCHD,
                BF_LOG_ERR,
                "ERROR: port_add failed(%d) for dev_id=%d, dev_port %d",
                sts,
                dev_id,
                dev_port);
          }
        }
        break;
      case BF_DEV_FAMILY_TOFINO3:
        /* The internal ports are 0-7 in each pipe.  Add the non-Ethernet MAC
         * ports here, these are the PCIe CPU port and the recirculation ports.
         */
        for (inc = 0, port = 0; port < 8; port += inc) {
          bf_dev_port_t dev_port = MAKE_DEV_PORT(pipe, port);
          inc = 2;
          if ((dev_port == pcie_cpu_port) || (dev_port == pcie_cpu_port2)) {
            /* The PCIe CPU Port only supports 25g. */
            sts = bf_port_add(dev_id, dev_port, BF_SPEED_25G, BF_FEC_TYP_NONE);
            if (sts != BF_SUCCESS) {
              bf_sys_log_and_trace(
                  BF_MOD_SWITCHD,
                  BF_LOG_ERR,
                  "ERROR: port_add failed(%d) for dev_id=%d, dev_port %d",
                  sts,
                  dev_id,
                  dev_port);
            }
          } else if ((dev_port == eth_cpu_port) ||
                     (dev_port == eth_cpu_port2)) {
            /* Do not add the Ethernet CPU port by default, let the application
             * take care of it. */
            inc = 4;
            sts = BF_SUCCESS;
          } else {
            /* This is NOT a CPU port so it must be a recirculation port; all
             * recirculation ports will run at 100G with two lanes. */
            sts = bf_port_add_with_lane_numb(
                dev_id, dev_port, BF_SPEED_100G, 2, BF_FEC_TYP_NONE);
          }
          if (sts != BF_SUCCESS) {
            bf_sys_log_and_trace(
                BF_MOD_SWITCHD,
                BF_LOG_ERR,
                "ERROR: port_add failed(%d) for dev_id=%d, dev_port %d",
                sts,
                dev_id,
                dev_port);
          }
        }
        break;
      default:
        bf_sys_log_and_trace(BF_MOD_SWITCHD,
                             BF_LOG_ERR,
                             "ERROR: Invalid chip family %d for dev_id=%d",
                             switchd_ctx->asic[dev_id].chip_family,
                             dev_id);
        return;
    }
  }

  // Put the internal ports in MAC lpbk for eligible SKU parts
  sts = bf_pm_internal_ports_init(dev_id);
  if (sts != BF_SUCCESS) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "ERROR: internal port add failed(%d) for dev_id=%d",
                         sts,
                         dev_id);
  }
}

/* Routine to instantiate and initialize ports of a device */
static void bf_switchd_ports_add(bf_dev_id_t dev_id) {
  if (switchd_ctx->args.skip_port_add) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD, BF_LOG_DBG, "Skipping port add");
    return;
  }
  if (switchd_ctx->asic[dev_id].is_virtual) return;
#if defined(DEVICE_IS_EMULATOR)  // Emulator
  bf_switchd_ports_add_to_emulator(dev_id);
  return;
#endif
  if (switchd_ctx->asic[dev_id].is_sw_model) {
    bf_switchd_ports_add_to_model(dev_id);
  } else {
    bf_switchd_ports_add_to_asic(dev_id);
  }
}

static void bf_switchd_ports_del_from_model(bf_dev_id_t dev_id) {
  unsigned int pipe, port;
  uint32_t num_pipes;

  lld_sku_get_num_active_pipes(dev_id, &num_pipes);
  for (pipe = 0; pipe < num_pipes; pipe++) {
    for (port = 0; port < BF_PIPE_PORT_COUNT; port += 1) {
      bool has_mac = false;
      bf_status_t bf_status =
          bf_port_has_mac(dev_id, MAKE_DEV_PORT(pipe, port), &has_mac);
      if (bf_status != BF_SUCCESS) {
        has_mac = false;
      }
      if (!has_mac && !switchd_ctx->state[dev_id].device_locked) {
        bf_port_remove(dev_id, MAKE_DEV_PORT(pipe, port));
      }
    }
  }
}

#if defined(DEVICE_IS_EMULATOR)  // Emulator
static void bf_switchd_ports_del_from_emulator(bf_dev_id_t dev_id) {
  unsigned int pipe, port;
  uint32_t num_pipes;

  lld_sku_get_num_active_pipes(dev_id, &num_pipes);
  for (pipe = 0; pipe < num_pipes; pipe++) {
    for (port = 0; port < BF_PIPE_PORT_COUNT; port += 4) {
      bool has_mac = false;
      bf_status_t bf_status =
          bf_port_has_mac(dev_id, MAKE_DEV_PORT(pipe, port), &has_mac);
      if (bf_status != BF_SUCCESS) {
        has_mac = false;
      }
      if (!has_mac && !switchd_ctx->state[dev_id].device_locked) {
        bf_port_remove(dev_id, MAKE_DEV_PORT(pipe, port));
      }
    }
  }
}
#endif

static void bf_switchd_ports_del_from_asic(bf_dev_id_t dev_id) {
  unsigned int pipe, port;
  uint32_t num_pipes;

  lld_sku_get_num_active_pipes(dev_id, &num_pipes);
  for (pipe = 0; pipe < num_pipes; pipe++) {
    for (port = 0; port < BF_PIPE_PORT_COUNT; ++port) {
      bool has_mac = false;
      bf_status_t bf_status =
          bf_port_has_mac(dev_id, MAKE_DEV_PORT(pipe, port), &has_mac);
      if (bf_status != BF_SUCCESS) {
        has_mac = false;
      }
      if (!has_mac && !switchd_ctx->state[dev_id].device_locked) {
        bf_port_remove(dev_id, MAKE_DEV_PORT(pipe, port));
      }
    }
  }
}

/* Delete all ports of a device */
static void bf_switchd_ports_delete(bf_dev_id_t dev_id) {
  if (switchd_ctx->asic[dev_id].is_virtual) return;
#if defined(DEVICE_IS_EMULATOR)  // Emulator
  bf_switchd_ports_del_from_emulator(dev_id);
  return;
#endif
  if (switchd_ctx->asic[dev_id].is_sw_model) {
    bf_switchd_ports_del_from_model(dev_id);
  } else {
    bf_switchd_ports_del_from_asic(dev_id);
  }
}

static int sanitize_serdes_file_name(const char *path_prefix,
                                     const char *path,
                                     const char *f_name,
                                     char **dst) {
  char file_path[2 * BF_SWITCHD_MAX_FILE_NAME];
  int file_path_len = sizeof file_path;

  if (path[0] != '/') {
    int x = snprintf(
        file_path, file_path_len, "%s/%s/%s", path_prefix, path, f_name);
    if (x <= 0 || x >= file_path_len) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "Error resolving file path %s/%s/%s\nPath must be less than %d "
          "characters",
          path_prefix,
          path,
          f_name,
          BF_SWITCHD_MAX_FILE_NAME);
      return -1;
    }
  } else {
    int x = snprintf(file_path, file_path_len, "%s/%s", path, f_name);
    if (x <= 0 || x >= file_path_len) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "Error resolving file path %s/%s\nPath must be less than %d "
          "characters",
          path,
          f_name,
          BF_SWITCHD_MAX_FILE_NAME);
      return -1;
    }
  }

  char *real_path = realpath(file_path, NULL);
  if (!real_path) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD, BF_LOG_ERR, "Error resolving file path %s", file_path);
    return -1;
  }
  *dst = bf_sys_strdup(real_path);
  free(real_path);
  return 0;
}

/* Helper routine to initialize the bf_device_profile_t structure used to add a
 * device to the driver. */
static bf_status_t bf_switchd_init_device_profile(
    bf_dev_id_t dev_id, bool skip_p4, bf_device_profile_t *dev_profile_p) {
  if (!switchd_ctx) return BF_NOT_READY;

  p4_devices_t *p4_device = &(switchd_ctx->p4_devices[dev_id]);
  switchd_serdes_cfg_t *serdes_cfg = &(switchd_ctx->asic[dev_id].serdes_cfg);
  int pci_dev_id = switchd_ctx->asic[dev_id].pci_dev_id;
  int rc;

  /* First populate the fields related to serdes firmware, this is only required
   * for physical devicse. */
  if (!switchd_ctx->asic[dev_id].is_virtual &&
    !switchd_ctx->asic[dev_id].is_sw_model) {
    /* Set the Tofino-1 serdes firmware paths for the device. */
    if (switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO) {
      /* If the conf file did not specify a path to the serdes firmware files
       * use our default location in the install directory. */
      char *fw_path = serdes_cfg->sds_fw_path
                          ? serdes_cfg->sds_fw_path
                          : "share/tofino_sds_fw/avago/firmware/";
      rc = sanitize_serdes_file_name(switchd_ctx->args.install_dir,
                                     fw_path,
                                     "sbus_master.rom",
                                     &dev_profile_p->sds_prof.sbus_master_fw);
      if (rc) return BF_INVALID_ARG;
      rc = sanitize_serdes_file_name(switchd_ctx->args.install_dir,
                                     fw_path,
                                     "pcie_serdes.rom",
                                     &dev_profile_p->sds_prof.pcie_fw);
      if (rc) return BF_INVALID_ARG;
      if (pci_dev_id == PCI_DEV_ID_TOF_A0) {
        rc = sanitize_serdes_file_name(switchd_ctx->args.install_dir,
                                       fw_path,
                                       "serdes_A0.rom",
                                       &dev_profile_p->sds_prof.serdes_fw);
      } else {
        rc = sanitize_serdes_file_name(switchd_ctx->args.install_dir,
                                       fw_path,
                                       "serdes_B0.rom",
                                       &dev_profile_p->sds_prof.serdes_fw);
      }
    } else if (switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO2) {
      /* If the conf file did not specify a path to the serdes firmware files
       * use our default location in the install directory.  Or, since there are
       * some out dated conf files in existance which incorrectly point to the
       * TF-1 default serdes firmware location ignore that and use the default
       * TF-2 location.  Ideally these conf files will be cleaned up but this
       * workaround maintains compability with them for now. */
      char *fw_path = serdes_cfg->sds_fw_path
                          ? serdes_cfg->sds_fw_path
                          : "share/tofino_sds_fw/credo/firmware/";
      if (!strcmp(fw_path, "share/tofino_sds_fw/avago/firmware") ||
          !strcmp(fw_path, "share/tofino_sds_fw/avago/firmware/")) {
        fw_path = "share/tofino_sds_fw/credo/firmware/";
      }
      rc = sanitize_serdes_file_name(
          switchd_ctx->args.install_dir,
          fw_path,
          "tof2_A0_grp_0_7_serdes.bin",
          &dev_profile_p->sds_prof.tof2_serdes_grp_0_7_fw);
      if (rc) return BF_INVALID_ARG;
      rc = sanitize_serdes_file_name(
          switchd_ctx->args.install_dir,
          fw_path,
          "tof2_A0_grp_8_serdes.bin",
          &dev_profile_p->sds_prof.tof2_serdes_grp_8_fw);
      if (rc) return BF_INVALID_ARG;
    } else if (switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO3) {
      dev_profile_p->sds_prof.tof3_serdes_0_pcie_fw = NULL;
      dev_profile_p->sds_prof.tof3_serdes_0_fw = NULL;
      dev_profile_p->sds_prof.tof3_serdes_1_32_fw = NULL;

      /* If the conf file did not specify a path to the serdes firmware files
       * use our default location in the install directory.  Or, since there are
       * some out dated conf files in existance which incorrectly point to the
       * TF-1 default serdes firmware location ignore that and use the default
       * TF-3 location.  Ideally these conf files will be cleaned up but this
       * workaround maintains compability with them for now. */
      char *fw_path = serdes_cfg->sds_fw_path
                          ? serdes_cfg->sds_fw_path
                          : "share/tofino_sds_fw/alphawave/firmware/";
      if (!strcmp(fw_path, "share/tofino_sds_fw/avago/firmware") ||
          !strcmp(fw_path, "share/tofino_sds_fw/avago/firmware/")) {
        fw_path = "share/tofino_sds_fw/alphawave/firmware/";
      }

      rc = sanitize_serdes_file_name(
          switchd_ctx->args.install_dir,
          fw_path,
          "tof3_serdes_1_32_fw.hex",
          &dev_profile_p->sds_prof.tof3_serdes_1_32_fw);
      if (rc) return BF_INVALID_ARG;

      rc = sanitize_serdes_file_name(switchd_ctx->args.install_dir,
                                     fw_path,
                                     "tof3_serdes_0_fw.hex",
                                     &dev_profile_p->sds_prof.tof3_serdes_0_fw);
      if (rc) return BF_INVALID_ARG;

      rc = sanitize_serdes_file_name(
          switchd_ctx->args.install_dir,
          fw_path,
          "tof3_serdes_0_pcie_fw.hex",
          &dev_profile_p->sds_prof.tof3_serdes_0_pcie_fw);
      if (rc) return BF_INVALID_ARG;
    }
  }

#ifdef BFRT_ENABLED
  if (switchd_ctx->args.install_dir != NULL) {
    /* Set the path the the BF-RT Fixed JSON files based on the install
     * directory. */
    const char *fixed_json_path = "share/bf_rt_shared/";
    dev_profile_p->bfrt_non_p4_json_dir_path = bf_sys_malloc(
        strlen(fixed_json_path) + strlen(switchd_ctx->args.install_dir) +
        1 /* For / character */ + 1 /* For termination */);
    if (!dev_profile_p->bfrt_non_p4_json_dir_path) return BF_NO_SYS_RESOURCES;
    sprintf(dev_profile_p->bfrt_non_p4_json_dir_path,
            "%s/%s",
            switchd_ctx->args.install_dir,
            fixed_json_path);
  }
#endif

#ifdef TDI_ENABLED
  if (switchd_ctx->args.install_dir != NULL) {
    /* Set the path for the TDI Fixed JSON files based on the install
     * directory. */
    const char *fixed_json_path = "share/tdi_shared/";
    dev_profile_p->tdi_non_p4_json_dir_path = bf_sys_malloc(
        strlen(fixed_json_path) + strlen(switchd_ctx->args.install_dir) +
        1 /* For / character */ + 1 /* For termination */);
    if (!dev_profile_p->tdi_non_p4_json_dir_path) return BF_NO_SYS_RESOURCES;
    sprintf(dev_profile_p->tdi_non_p4_json_dir_path,
            "%s/%s",
            switchd_ctx->args.install_dir,
            fixed_json_path);
  }
#endif

  dev_profile_p->microp_prof.microp_fw = NULL;

  if (skip_p4) {
    return BF_SUCCESS;
  }

  // initialize the number of p4 programs for the device
  dev_profile_p->num_p4_programs = p4_device->num_p4_programs;
  for (int j = 0; j < p4_device->num_p4_programs; j++) {
    p4_programs_t *p4_programs = &(p4_device->p4_programs[j]);
    bf_p4_program_t *dev_profile_program = &(dev_profile_p->p4_programs[j]);

    /* Copy the program name */
    snprintf(dev_profile_program->prog_name,
             sizeof(dev_profile_program->prog_name),
             "%s",
             p4_programs->program_name);
    if (strlen(p4_programs->bfrt_config)) {
      dev_profile_program->bfrt_json_file =
          bf_sys_strdup(p4_programs->bfrt_config);
    } else {
      dev_profile_program->bfrt_json_file = NULL;
    }

    // initialize p4 profile(s) for the device
    dev_profile_program->num_p4_pipelines = p4_programs->num_p4_pipelines;
    for (int i = 0; i < p4_programs->num_p4_pipelines; i++) {
      p4_pipeline_config_t *p4_pipeline = &(p4_programs->p4_pipelines[i]);
      bf_p4_pipeline_t *dev_profile_pipeline =
          &(dev_profile_program->p4_pipelines[i]);

      /* Copy the pipeline name */
      snprintf(
          dev_profile_pipeline->p4_pipeline_name,
          sizeof(dev_profile_pipeline->p4_pipeline_name),
          "%s",
          p4_pipeline->p4_pipeline_name ? p4_pipeline->p4_pipeline_name : "");

      // configuration file
      dev_profile_pipeline->cfg_file = bf_sys_strdup(p4_pipeline->tofino_bin);
      // Json file
      dev_profile_pipeline->runtime_context_file =
          bf_sys_strdup(p4_pipeline->table_config);
      // PI configuration
      if (p4_pipeline->pi_native_config_path[0] != 0) {
        dev_profile_pipeline->pi_config_file =
            bf_sys_strdup(p4_pipeline->pi_native_config_path);
      }
      // Pipes in Scope
      dev_profile_pipeline->num_pipes_in_scope =
          p4_pipeline->num_pipes_in_scope;
      for (int s = 0; s < p4_pipeline->num_pipes_in_scope; s++) {
        dev_profile_pipeline->pipe_scope[s] = p4_pipeline->pipe_scope[s];
      }
    }
  }

  // coalescing mirror configuration
  dev_profile_p->coal_mirror_enable = p4_device->coal_mirror_enable;
  dev_profile_p->coal_sessions_num = p4_device->coal_sessions_num;
  dev_profile_p->coal_min = p4_device->coal_min;

  return BF_SUCCESS;
}

static void bf_switchd_free_device_profile(bf_device_profile_t *dev_profile) {
  if (!dev_profile) return;

  if (dev_profile->sds_prof.sbus_master_fw)
    bf_sys_free(dev_profile->sds_prof.sbus_master_fw);
  if (dev_profile->sds_prof.pcie_fw) bf_sys_free(dev_profile->sds_prof.pcie_fw);
  if (dev_profile->sds_prof.serdes_fw)
    bf_sys_free(dev_profile->sds_prof.serdes_fw);
  if (dev_profile->microp_prof.microp_fw)
    bf_sys_free(dev_profile->microp_prof.microp_fw);
  if (dev_profile->bfrt_non_p4_json_dir_path)
    bf_sys_free(dev_profile->bfrt_non_p4_json_dir_path);
  if (dev_profile->tdi_non_p4_json_dir_path)
    bf_sys_free(dev_profile->tdi_non_p4_json_dir_path);

  for (unsigned i = 0; i < dev_profile->num_p4_programs; ++i) {
    bf_p4_program_t *p = dev_profile->p4_programs + i;
    if (p->bfrt_json_file) bf_sys_free(p->bfrt_json_file);
    for (unsigned j = 0; j < p->num_p4_pipelines; ++j) {
      if (p->p4_pipelines[j].cfg_file) bf_sys_free(p->p4_pipelines[j].cfg_file);
      if (p->p4_pipelines[j].runtime_context_file)
        bf_sys_free(p->p4_pipelines[j].runtime_context_file);
      if (p->p4_pipelines[j].pi_config_file)
        bf_sys_free(p->p4_pipelines[j].pi_config_file);
    }
  }
}

struct bf_switchd_dma_cfg {
  bool is_shared;
  union {
    struct dma_mem_params {
      bool shared_across_chips;
      int sz;
      int cnt;
    } p;
    bf_dma_type_t shares_with;
  } c;
};
struct bf_switchd_dma_cfg tof_dma[BF_DMA_TYPE_MAX];
struct bf_switchd_dma_cfg tof2_dma[BF_DMA_TYPE_MAX];
struct bf_switchd_dma_cfg tof3_dma[BF_DMA_TYPE_MAX];


static void bf_switchd_init_dma_template(bool kernel_pkt_proc) {
  /* Create the DMA configuration per chip type. */
  memset(tof_dma, 0, sizeof tof_dma);
  memset(tof2_dma, 0, sizeof tof2_dma);
  memset(tof3_dma, 0, sizeof tof3_dma);

  for (bf_dma_type_t t = BF_DMA_TYPE_FIRST; t < BF_DMA_TYPE_MAX; ++t) {
    switch (t) {
      case BF_DMA_PIPE_INSTRUCTION_LIST:
        tof_dma[t].c.p.shared_across_chips = true;
        tof_dma[t].c.p.sz = DMA_INST_BUF_SIZE;
        tof_dma[t].c.p.cnt = TOF_DMA_INST_LIST_SIZE / tof_dma[t].c.p.sz;
        tof2_dma[t].c.p.shared_across_chips = true;
        tof2_dma[t].c.p.sz = DMA_INST_BUF_SIZE;
        tof2_dma[t].c.p.cnt = TOF2_DMA_INST_LIST_SIZE / tof2_dma[t].c.p.sz;
        tof3_dma[t].c.p.shared_across_chips = true;
        tof3_dma[t].c.p.sz = DMA_INST_BUF_SIZE;
        tof3_dma[t].c.p.cnt = TOF3_DMA_INST_LIST_SIZE / tof3_dma[t].c.p.sz;





        break;
      case BF_DMA_PIPE_LEARN_NOTIFY:
        tof_dma[t].c.p.sz = 32 * 1024;
        tof_dma[t].c.p.cnt = 24;
        tof2_dma[t].c.p.sz = 32 * 1024;
        tof2_dma[t].c.p.cnt = 48;
        tof3_dma[t] = tof2_dma[t];

        break;
      case BF_DMA_PIPE_STAT_NOTIFY:
        tof_dma[t].c.p.sz = 512;
        tof_dma[t].c.p.cnt = (3 * 1024 * 1024) / tof_dma[t].c.p.sz;
        tof2_dma[t] = tof_dma[t];
        tof3_dma[t] = tof_dma[t];

        break;
      case BF_DMA_PIPE_IDLE_STATE_NOTIFY:
        tof_dma[t].c.p.sz = 32 * 1024;
        tof_dma[t].c.p.cnt = (3 * 1024 * 1024) / tof_dma[t].c.p.sz;
        tof2_dma[t] = tof_dma[t];
        tof3_dma[t] = tof_dma[t];

        break;
      case BF_DMA_PIPE_BLOCK_WRITE:
        tof_dma[t].is_shared = true;
        tof_dma[t].c.shares_with = BF_DMA_PIPE_INSTRUCTION_LIST;
        tof2_dma[t] = tof_dma[t];
        tof3_dma[t] = tof_dma[t];

        break;
      case BF_DMA_PIPE_BLOCK_READ:
        tof_dma[t].is_shared = true;
        tof_dma[t].c.shares_with = BF_DMA_PIPE_INSTRUCTION_LIST;
        tof2_dma[t] = tof_dma[t];
        tof3_dma[t] = tof_dma[t];

        break;
      case BF_DMA_TM_WRITE_LIST:
        tof_dma[t].c.p.shared_across_chips = true;
        tof_dma[t].c.p.sz = 4 * 1024;
        tof_dma[t].c.p.cnt = (16 * 1024 * 1024) / tof_dma[t].c.p.sz;
        tof2_dma[t].c.p.shared_across_chips = true;
        tof2_dma[t].c.p.sz = 4 * 1024;
        tof2_dma[t].c.p.cnt = (8 * 1024 * 1024) / tof2_dma[t].c.p.sz;
        tof3_dma[t] = tof2_dma[t];

        break;
      case BF_DMA_DIAG_ERR_NOTIFY:
        tof_dma[t].c.p.sz = 256;
        tof_dma[t].c.p.cnt = 8 * 1024;
        tof2_dma[t] = tof_dma[t];
        tof3_dma[t] = tof_dma[t];

        break;
      case BF_DMA_MAC_STAT_RECEIVE:
        tof_dma[t].c.p.sz = 8 * 128;
        tof_dma[t].c.p.cnt = 256 + 4;
        tof2_dma[t] = tof_dma[t];
        tof3_dma[t] = tof_dma[t];

        break;
      case BF_DMA_CPU_PKT_RECEIVE_0:
      case BF_DMA_CPU_PKT_RECEIVE_1:
      case BF_DMA_CPU_PKT_RECEIVE_2:
      case BF_DMA_CPU_PKT_RECEIVE_3:
      case BF_DMA_CPU_PKT_RECEIVE_4:
      case BF_DMA_CPU_PKT_RECEIVE_5:
      case BF_DMA_CPU_PKT_RECEIVE_6:
      case BF_DMA_CPU_PKT_RECEIVE_7:
        if (!kernel_pkt_proc) {
          tof_dma[t].c.p.sz = 2 * 1024;
          tof_dma[t].c.p.cnt = (2 * 1024 * 1024) / tof_dma[t].c.p.sz - 16;
        } else {
          memset(&tof_dma[t], 0, sizeof tof_dma[t]);
        }
        tof2_dma[t] = tof_dma[t];
        tof3_dma[t] = tof_dma[t];

        break;
      case BF_DMA_CPU_PKT_TRANSMIT_0:
      case BF_DMA_CPU_PKT_TRANSMIT_1:
      case BF_DMA_CPU_PKT_TRANSMIT_2:
      case BF_DMA_CPU_PKT_TRANSMIT_3:
        if (!kernel_pkt_proc) {
          tof_dma[t].c.p.shared_across_chips = true;
          tof_dma[t].c.p.sz = 2 * 1024;
          tof_dma[t].c.p.cnt = DMA_PKT_TX_SIZE / tof_dma[t].c.p.sz;
        } else {
          memset(&tof_dma[t], 0, sizeof tof_dma[t]);
        }
        tof2_dma[t] = tof_dma[t];
        tof3_dma[t] = tof_dma[t];

        break;
      case BF_DMA_MAC_BLOCK_WRITE:
        memset(&tof_dma[t], 0, sizeof tof_dma[t]);
        tof2_dma[t] = tof_dma[t];
        tof3_dma[t] = tof_dma[t];

        break;
      case BF_DMA_TM_WRITE_LIST_1:
        memset(&tof_dma[t], 0, sizeof tof_dma[t]);
        tof2_dma[t].c.p.shared_across_chips = true;
        tof2_dma[t].c.p.sz = 4 * 1024;
        tof2_dma[t].c.p.cnt = (10 * 1024 * 1024) / tof2_dma[t].c.p.sz;
        tof3_dma[t] = tof2_dma[t];

        break;
      case BF_DMA_TM_BLOCK_READ_0:
        memset(&tof_dma[t], 0, sizeof tof_dma[t]);
        tof2_dma[t].is_shared = true;
        tof2_dma[t].c.shares_with = BF_DMA_TM_WRITE_LIST;
        tof3_dma[t] = tof2_dma[t];

        break;
      case BF_DMA_TM_BLOCK_READ_1:
        memset(&tof_dma[t], 0, sizeof tof_dma[t]);
        tof2_dma[t].is_shared = true;
        tof2_dma[t].c.shares_with = BF_DMA_TM_WRITE_LIST_1;
        tof3_dma[t] = tof2_dma[t];

        break;
      default:
        bf_sys_assert(0);
    }
  }
}

/* Routine to release DMA memory assigned for a device */
static void bf_switchd_release_dma_mem(bf_dev_id_t dev_id,
                                       bf_subdev_id_t subdev_id) {
  if (!bf_dev_id_validate(dev_id) || subdev_id < 0 ||
      subdev_id >= BF_MAX_SUBDEV_COUNT) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "ERROR: invalid dev_id (%d) subdev_id (%d) in %s",
                         dev_id,
                         subdev_id,
                         __func__);
    return;
  }

  bool is_tof = switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO;
  bool is_tof2 = switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO2;
  bool is_tof3 = switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO3;



  bf_dma_info_t *dma_info = &(switchd_ctx->dma_info[dev_id][subdev_id]);

  if (!is_tof3 && subdev_id != 0) {

      return;

  }

  /* Clean up the DR pool if it was allocated. */
  if (dma_info->dma_dr_pool_handle) {
    bf_sys_dma_pool_destroy(dma_info->dma_dr_pool_handle);
  }
  /* Clean up the rest of the DMA pools if they were allocated.
   * Note if the pool is shared with another DMA type it can be skipped.
   * Note if the pool is shared across devices it should be left if another
   * device is using it. */
  for (bf_dma_type_t t = BF_DMA_TYPE_FIRST; t < BF_DMA_TYPE_MAX; ++t) {
    struct bf_switchd_dma_cfg *cfg = NULL;
    if (is_tof) {
      cfg = &tof_dma[t];
    } else if (is_tof2) {
      cfg = &tof2_dma[t];
    } else if (is_tof3) {
      cfg = &tof3_dma[t];


    }
    if (!dma_info->dma_buff_info[t].dma_buf_pool_handle) continue;
    if (!cfg || cfg->is_shared) continue;
    if (cfg->c.p.shared_across_chips) {
      bool in_use = false;
      for (int d = 0; d < BF_MAX_DEV_COUNT; ++d) {
        /* Don't count ourselves. */
        if (d == dev_id) continue;
        /* Don't count other chip types. */
        if (switchd_ctx->asic[dev_id].chip_family !=
            switchd_ctx->asic[d].chip_family) {
          continue;
        }
        if (switchd_ctx->dma_info[d][subdev_id]
                .dma_buff_info[t]
                .dma_buf_pool_handle) {
          in_use = true;
          break;
        }
      }
      if (in_use) continue;
    }
    bf_sys_dma_pool_destroy(dma_info->dma_buff_info[t].dma_buf_pool_handle);
  }
  memset(dma_info, 0, sizeof(bf_dma_info_t));

  return;
}

static int get_extra_dma_buffers(bf_dev_id_t dev_id, int num_p4_pipelines) {
  /* If we are running more than one P4 pipeline on the device increase the
   * amount of PBus DMA memory available based on the number of P4 pipelines
   * and Tofino family. */
  int extra_buffers = 0;

  if (num_p4_pipelines > 2) {
    int additional_sz = 0;
    int buf_sz = 1;
    switch (switchd_ctx->asic[dev_id].chip_family) {
      case BF_DEV_FAMILY_TOFINO:
        /* No need for extra buffer for 2 programs */
        if (num_p4_pipelines == 3)
          additional_sz = TOF_DMA_INST_LIST_ADDITIONAL_3;
        else
          additional_sz = TOF_DMA_INST_LIST_ADDITIONAL_4;
        buf_sz = tof_dma[BF_DMA_PIPE_INSTRUCTION_LIST].c.p.sz;
        break;
      case BF_DEV_FAMILY_TOFINO2:
        /* No need for extra buffer for 2 programs */
        if (num_p4_pipelines == 3)
          additional_sz = TOF2_DMA_INST_LIST_ADDITIONAL_3;
        else
          additional_sz = TOF2_DMA_INST_LIST_ADDITIONAL_4;
        buf_sz = tof2_dma[BF_DMA_PIPE_INSTRUCTION_LIST].c.p.sz;
        break;
      case BF_DEV_FAMILY_TOFINO3:
        if (num_p4_pipelines == 3)
          additional_sz = TOF3_DMA_INST_LIST_ADDITIONAL_3;
        else
          additional_sz = TOF3_DMA_INST_LIST_ADDITIONAL_4;
        buf_sz = tof3_dma[BF_DMA_PIPE_INSTRUCTION_LIST].c.p.sz;
        break;









      default:
        break;
    }
    extra_buffers = additional_sz / buf_sz;
  }

  return extra_buffers;
}

/* Setup and assign DMA memory for a device */
static int bf_switchd_setup_dma_mem(bf_dev_id_t dev_id,
                                    bf_subdev_id_t subdev_id,
                                    int num_p4_pipelines) {
  int ret;

  if (!bf_dev_id_validate(dev_id) || subdev_id < 0 ||
      subdev_id >= BF_MAX_SUBDEV_COUNT) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "ERROR: invalid dev_id (%d) subdev_id (%d)",
                         dev_id,
                         subdev_id);
    goto no_cleanup_exit_1;
  }

  /* enforce sudo privileges */
  if (geteuid() != 0) {
    printf("ERROR: must run sudo for DMA allocation\n");
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD, BF_LOG_ERR, "ERROR: must run sudo for DMA allocation");
    goto no_cleanup_exit_1;
  }
  ret = bf_sys_dma_lib_init(NULL, NULL, NULL);
  if (ret) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "ERROR: bf_sys_dma_lib_init failed(%d)",
                         ret);
    goto no_cleanup_exit_1;
  }
  if (!switchd_ctx->asic[dev_id].is_sw_model && !reg_dir_i2c_rd_func &&
      !reg_dir_i2c_wr_func) {
    /* register with bf_syslib the DMA bus mapping APIs
     */
    bf_sys_dma_map_fn_register(bf_switchd_dmamap, bf_switchd_dmaunmap);
  }

  bool is_tof = switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO;
  bool is_tof2 = switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO2;
  bool is_tof3 = switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO3;




  switch (switchd_ctx->asic[dev_id].chip_family) {
    case BF_DEV_FAMILY_TOFINO:
    case BF_DEV_FAMILY_TOFINO2:

      if (subdev_id) {
        bf_sys_log_and_trace(BF_MOD_SWITCHD,
                             BF_LOG_ERR,
                             "ERROR: Unexpected subdev_id %d for dev %d",
                             subdev_id,
                             dev_id);
        goto no_cleanup_exit_1;
      }
      break;
    case BF_DEV_FAMILY_TOFINO3:
      if (subdev_id != 0 && subdev_id != 1) {
        bf_sys_log_and_trace(BF_MOD_SWITCHD,
                             BF_LOG_ERR,
                             "ERROR: Unexpected subdev_id %d for dev %d",
                             subdev_id,
                             dev_id);
        goto no_cleanup_exit_1;
      }
      break;
    default:
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "ERROR: Unexpected chip family %d for dev %d",
                           switchd_ctx->asic[dev_id].chip_family,
                           dev_id);
      goto no_cleanup_exit_1;
  }

  bf_dma_info_t *dma_info = &(switchd_ctx->dma_info[dev_id][subdev_id]);
  memset(dma_info, 0, sizeof *dma_info);

  dma_info->dev_id = dev_id;
  dma_info->subdev_id = subdev_id;

  /* If we are running more than one P4 pipeline on the device increase the
   * amount of PBus DMA memory available based on the number of P4 pipelines. */
  int extra_pbus_buffers = 0;
  if (num_p4_pipelines > 2)
    extra_pbus_buffers = get_extra_dma_buffers(dev_id, num_p4_pipelines);

  /* Assign the buffer size and counts. */
  for (bf_dma_type_t t = BF_DMA_TYPE_FIRST; t < BF_DMA_TYPE_MAX; ++t) {
    bf_dma_type_t T = BF_DMA_TYPE_MAX;
    if (is_tof) {
      T = tof_dma[t].is_shared ? tof_dma[t].c.shares_with : t;
      dma_info->dma_buff_info[t].dma_buf_size = tof_dma[T].c.p.sz;
      dma_info->dma_buff_info[t].dma_buf_cnt = tof_dma[T].c.p.cnt;
    } else if (is_tof2) {
      T = tof2_dma[t].is_shared ? tof2_dma[t].c.shares_with : t;
      dma_info->dma_buff_info[t].dma_buf_size = tof2_dma[T].c.p.sz;
      dma_info->dma_buff_info[t].dma_buf_cnt = tof2_dma[T].c.p.cnt;
    } else if (is_tof3) {
      T = tof3_dma[t].is_shared ? tof3_dma[t].c.shares_with : t;
      dma_info->dma_buff_info[t].dma_buf_size = tof3_dma[T].c.p.sz;
      dma_info->dma_buff_info[t].dma_buf_cnt = tof3_dma[T].c.p.cnt;






    }
    if (T == BF_DMA_PIPE_INSTRUCTION_LIST)
      dma_info->dma_buff_info[t].dma_buf_cnt += extra_pbus_buffers;
  }

  /* Configure the DR memory using a single pool. */
  int total_dr_size = 0;
  for (bf_dma_type_t t = BF_DMA_TYPE_FIRST; t < BF_DMA_TYPE_MAX; ++t) {
    unsigned int buf_cnt = dma_info->dma_buff_info[t].dma_buf_cnt;
    unsigned int max_tx, max_rx;
    bf_dma_dr_get_max_depth(dev_id, t, &max_tx, &max_rx);
    bf_sys_assert(max_tx >= buf_cnt);
    bf_sys_assert(max_rx >= buf_cnt);

    /* Note that free memory DRs have a lower priority than their
     * corresponding completion DR.  Therefore, if completions are being
     * generated at a high rate the free memory DR pointer may not be
     * updated by the hardware (since it is lower priority).  This can
     * result in the CPU seeing a completion while the free memory DR is
     * still full!  Add some guard space to the size of those free memory
     * DRs so that we don't run into this. */
    unsigned int guard = 0;
    if (t == BF_DMA_PIPE_LEARN_NOTIFY || t == BF_DMA_PIPE_STAT_NOTIFY ||
        t == BF_DMA_PIPE_IDLE_STATE_NOTIFY || t == BF_DMA_DIAG_ERR_NOTIFY ||
        (t >= BF_DMA_CPU_PKT_RECEIVE_0 && t <= BF_DMA_CPU_PKT_RECEIVE_7)) {
      guard = buf_cnt < 16 ? buf_cnt : 16;
    }
    dma_info->dma_dr_info[t].dma_dr_entry_count[BF_DMA_DR_DIR_CPU2DEV] =
        buf_cnt + guard;
    dma_info->dma_dr_info[t].dma_dr_entry_count[BF_DMA_DR_DIR_DEV2CPU] =
        buf_cnt;

    int x = bf_dma_dr_get_mem_requirement(
        switchd_ctx->asic[dev_id].chip_family,
        t,
        dma_info->dma_dr_info[t].dma_dr_entry_count[BF_DMA_DR_DIR_CPU2DEV],
        dma_info->dma_dr_info[t].dma_dr_entry_count[BF_DMA_DR_DIR_DEV2CPU]);
    if (x > 0) {
      total_dr_size += x;
    }
  }
  int dr_pool_buf_sz = BF_HUGE_PAGE_SIZE;
  char dma_dr_pool_name[32] = {0};
  snprintf(dma_dr_pool_name,
           sizeof(dma_dr_pool_name) - 1,
           "%d_%d_DR_Pool",
           dev_id,
           subdev_id);
  dma_info->dma_dr_pool_handle = 0;
  dma_info->dma_dr_buf_size = dr_pool_buf_sz;
  int dr_pool_buf_cnt =
      1 + (total_dr_size + dr_pool_buf_sz - 1) / dr_pool_buf_sz;
  ret = bf_sys_dma_pool_create(dma_dr_pool_name,
                               &dma_info->dma_dr_pool_handle,
                               dev_id,
                               subdev_id,
                               dr_pool_buf_sz,
                               dr_pool_buf_cnt,
                               64);
  if (ret) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD,
        BF_LOG_ERR,
        "ERROR: DR pool create failed; dev_id %d subdev_id %d size %d "
        "failed(%d)",
        dev_id,
        subdev_id,
        total_dr_size,
        ret);
    goto cleanup_exit_1;
  }

  /* Setup the DMA buffer pools for each type of DMA except the shared pools. */
  for (bf_dma_type_t t = BF_DMA_TYPE_FIRST; t < BF_DMA_TYPE_MAX; ++t) {
    struct bf_switchd_dma_cfg *cfg = NULL;

    if (is_tof) {
      cfg = &tof_dma[t];
    } else if (is_tof2) {
      cfg = &tof2_dma[t];
    } else if (is_tof3) {
      cfg = &tof3_dma[t];


    }
    if (!cfg) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "ERROR: Could not get dma config; dev_id %d dma type %d",
          dev_id,
          t);
      goto cleanup_exit_1;
    }

    if (cfg->is_shared) continue;
    if (cfg->c.p.shared_across_chips) {
      /* This pool is shared across devices, check all other devices to see if
       * it has already been allocated.  Only share with devices of the same
       * type. */
      for (int d = 0; d < BF_MAX_DEV_COUNT; ++d) {
        if (switchd_ctx->asic[dev_id].chip_family !=
            switchd_ctx->asic[d].chip_family) {
          continue;
        }
        if (switchd_ctx->dma_info[d][subdev_id]
                .dma_buff_info[t]
                .dma_buf_pool_handle) {
          dma_info->dma_buff_info[t].dma_buf_pool_handle =
              switchd_ctx->dma_info[d][subdev_id]
                  .dma_buff_info[t]
                  .dma_buf_pool_handle;
          break;
        }
      }
    }
    if (!dma_info->dma_buff_info[t].dma_buf_pool_handle) {
      /* This DMA pool needs to be allocated. */
      char pool_name[64];
      if (cfg->c.p.shared_across_chips) {
        sprintf(pool_name, "%s_Pool", bf_dma_type_str(t));
      } else {
        sprintf(pool_name,
                "%s_dev_%d_%d_Pool",
                bf_dma_type_str(t),
                dev_id,
                subdev_id);
      }
      ret = bf_sys_dma_pool_create(
          pool_name,
          &dma_info->dma_buff_info[t].dma_buf_pool_handle,
          dev_id,
          subdev_id,
          dma_info->dma_buff_info[t].dma_buf_size,
          dma_info->dma_buff_info[t].dma_buf_cnt,
          256);
      if (ret) {
        bf_sys_log_and_trace(
            BF_MOD_SWITCHD,
            BF_LOG_ERR,
            "ERROR: bf_sys_dma_pool_create failed(%d) for dev_id %d subdev_id "
            "%d pool %s",
            ret,
            dev_id,
            subdev_id,
            pool_name);
        goto cleanup_exit_1;
      }
    }
  }
  /* Assign the pool handles for the shared pools. */
  for (bf_dma_type_t t = BF_DMA_TYPE_FIRST; t < BF_DMA_TYPE_MAX; ++t) {
    struct bf_switchd_dma_cfg *cfg = NULL;
    if (is_tof) {
      cfg = &tof_dma[t];
    } else if (is_tof2) {
      cfg = &tof2_dma[t];
    } else if (is_tof3) {
      cfg = &tof3_dma[t];


    }

    if (!cfg || !cfg->is_shared) continue;
    dma_info->dma_buff_info[t].dma_buf_pool_handle =
        dma_info->dma_buff_info[cfg->c.shares_with].dma_buf_pool_handle;
  }

  return 0;

cleanup_exit_1:
  for (int i = 0; i < BF_MAX_SUBDEV_COUNT; i++) {
    if (switchd_ctx->pcie_map[dev_id][i].configured) {
      bf_switchd_release_dma_mem(dev_id, i);
    }
  }
no_cleanup_exit_1:
 return -1;
}

#ifdef TOFINO3_MULTI_SUBDEV_MODE
static bf_status_t bf_unmap_dev_tof3(int dev_id) {
  switchd_pcie_map_t *pcie_map;
  int cnt;

  for (cnt = 0; cnt < BF_MAX_SUBDEV_COUNT; cnt++) {
    pcie_map = &(switchd_ctx->pcie_map[dev_id][cnt]);

    if (pcie_map->dev_fd >= 0) {
      close(pcie_map->dev_fd);
    }

    if (pcie_map->dev_base_addr && pcie_map->dev_base_size) {
      munmap((void *)pcie_map->dev_base_addr, pcie_map->dev_base_size);
    }
    pcie_map->dev_fd = -1;
  }
  return BF_SUCCESS;
}

static bf_status_t bf_mmap_dev_tof3(int dev_id) {
  switchd_pcie_map_t *pcie_map;
  enum bf_intr_mode intr_mode;
  bf_status_t status = BF_OBJECT_NOT_FOUND; /* initialize */
  int cnt;

  for (cnt = 0; cnt < BF_MAX_SUBDEV_COUNT; cnt++) {
    pcie_map = &(switchd_ctx->pcie_map[dev_id][cnt]);
    pcie_map->dev_base_size = 128 * 1024 * 1024;
    snprintf(pcie_map->cdev_name,
             sizeof(pcie_map->cdev_name),
             "/dev/bf%ds%1d",
             dev_id,
             cnt);
    pcie_map->dev_fd = open(pcie_map->cdev_name, O_RDWR);
    if (pcie_map->dev_fd < 0) {
      pcie_map->dev_fd = -1; /* invalidate */
      pcie_map->cdev_name[0] = '\0';
      /* subdevice 0 must exist */
      /* subdevice 1 must exist only for certain parts, but the part type
         is not known at this point. So, we log  result only but continue */
      if (cnt == 0) {
        bf_sys_log_and_trace(BF_MOD_SWITCHD,
                             BF_LOG_ERR,
                             "error opening %s",
                             pcie_map->cdev_name);
        return BF_OBJECT_NOT_FOUND;
      } else {
        bf_sys_log_and_trace(
            BF_MOD_SWITCHD,
            BF_LOG_WARN,
            "cannot open %s, an error only for two subdevice part",
            pcie_map->cdev_name);
        continue;
      }
    }
    pcie_map->dev_base_addr = (uint8_t *)mmap(NULL,
                                   pcie_map->dev_base_size,
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED,
                                   pcie_map->dev_fd,
                                   0);
    if (pcie_map->dev_base_addr == (uint8_t *)-1) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD, BF_LOG_ERR, "error mmaping %s", pcie_map->cdev_name);
      close(pcie_map->dev_fd);
      return BF_OBJECT_NOT_FOUND;
    } else {
      status = BF_SUCCESS;
    }
  }
  if (status != BF_SUCCESS) {
    return status;
  }
  /* set interrupt mode by querying the kernel module */
  pcie_map = &(switchd_ctx->pcie_map[dev_id][0]);
  bf_switchd_int_mode_get(pcie_map->dev_fd, &intr_mode);
  switch (intr_mode) {
    case BF_INTR_MODE_MSIX:
      switchd_ctx->asic[dev_id].pcie_cfg.int_mode = BF_SWITCHD_INT_MODE_MSIX;
      break;
    case BF_INTR_MODE_MSI:
      switchd_ctx->asic[dev_id].pcie_cfg.int_mode = BF_SWITCHD_INT_MODE_MSI;
      break;
    default:
      /* treat intx and no-interrupt in the same way  */
      switchd_ctx->asic[dev_id].pcie_cfg.int_mode = BF_SWITCHD_INT_MODE_INTX;
      break;
  }
  return status;
}
#endif  // TOFINO3_MULTI_SUBDEV_MODE

/* Unmaps uio device and closes associated file handle
 */
static bf_status_t bf_unmap_dev(bf_dev_id_t dev_id) {
  switchd_pcie_map_t *pcie_map = &(switchd_ctx->pcie_map[dev_id][0]);

#ifdef TOFINO3_MULTI_SUBDEV_MODE
  if (switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO3) {
    return (bf_unmap_dev_tof3(dev_id));
  }
#endif
  if (pcie_map->dev_fd >= 0) {
    close(pcie_map->dev_fd);
  }

  if (pcie_map->dev_base_addr && pcie_map->dev_base_size) {
    munmap((void *)pcie_map->dev_base_addr, pcie_map->dev_base_size);
  }
  pcie_map->dev_fd = -1;
  return BF_SUCCESS;
}

/* Locates bf device, mmaps it, and opens a file handle
 */
static bf_status_t bf_mmap_dev(bf_dev_id_t dev_id) {
  char bf_name[256];
  switchd_pcie_map_t *pcie_map = &(switchd_ctx->pcie_map[dev_id][0]);
  enum bf_intr_mode intr_mode;

  switch (switchd_ctx->asic[dev_id].chip_family) {
    case BF_DEV_FAMILY_TOFINO:
      pcie_map->dev_base_size = 64 * 1024 * 1024;
      break;
    case BF_DEV_FAMILY_TOFINO2:
      pcie_map->dev_base_size = 128 * 1024 * 1024;
      break;
    case BF_DEV_FAMILY_TOFINO3:
#ifdef TOFINO3_MULTI_SUBDEV_MODE
      return (bf_mmap_dev_tof3(dev_id));
#endif
      pcie_map->dev_base_size = 128 * 1024 * 1024;
      break;



    default:
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "Unexpected chip type, device %d type %d",
                           dev_id,
                           switchd_ctx->asic[dev_id].chip_family);
      return BF_INVALID_ARG;
  }

#ifdef __sun
  char *tmp_name;
  if ((tmp_name = find_tofino_device()) == NULL)
	  return (BF_INVALID_ARG);

  snprintf(bf_name, sizeof(bf_name), tmp_name);
  free(tmp_name);
#else
  if (switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO3) {
#ifndef TOFINO3_MULTI_SUBDEV_MODE
    snprintf(bf_name, sizeof(bf_name), "%s%ds0", "/dev/bf", dev_id);
#endif
  } else {
    snprintf(bf_name, sizeof(bf_name), "%s%d", "/dev/bf", dev_id);
  }
#endif
  pcie_map->dev_fd = open(bf_name, O_RDWR);
  if (pcie_map->dev_fd < 0) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD, BF_LOG_ERR, "error opening %s", bf_name);
    return BF_OBJECT_NOT_FOUND;
  }

  /* Set the interrupt mode by querying the kernel module. */
  bf_switchd_int_mode_get(pcie_map->dev_fd, &intr_mode);
  switch (intr_mode) {
    case BF_INTR_MODE_MSIX:
      switchd_ctx->asic[dev_id].pcie_cfg.int_mode = BF_SWITCHD_INT_MODE_MSIX;
      break;
    case BF_INTR_MODE_MSI:
      switchd_ctx->asic[dev_id].pcie_cfg.int_mode = BF_SWITCHD_INT_MODE_MSI;
      break;
    default:
      /* treat intx and no-interrupt in the same way  */
      switchd_ctx->asic[dev_id].pcie_cfg.int_mode = BF_SWITCHD_INT_MODE_INTX;
      break;
  }

  pcie_map->dev_base_addr = (uint8_t *)mmap(NULL,
                                 pcie_map->dev_base_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED,
                                 pcie_map->dev_fd,
                                 0);
  if (pcie_map->dev_base_addr == (uint8_t *)-1) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD, BF_LOG_ERR, "error mmaping %s", bf_name);
    close(pcie_map->dev_fd);
    return BF_OBJECT_NOT_FOUND;
  }
  return BF_SUCCESS;
}

static void setup_async_notification(int fd) {
#ifdef __sun
  (void) fd;
#else
  int flags;

  fcntl(fd, F_SETOWN, getpid());
  flags = fcntl(fd, F_GETFL);
  fcntl(fd, F_SETFL, flags | FASYNC);
#endif
}

static bf_status_t bf_switchd_msix_map_init(bf_dev_id_t dev_id) {
  int irq, irq_base, pipe;
  bf_status_t status;

  /* setup msix:
     1: host
     2: tbus
     3: cbus
     4: mbus
     5: pbus
     all other: unmapped
  */
  for (irq = bf_tof2_int_host_lo; irq <= bf_tof2_int_host_hi;
       irq++) {  // hostif MSIX
    if ((status = bf_msix_map_view_init(dev_id, irq, BF_SWITCHD_MSIX_HOSTIF)) !=
        BF_SUCCESS) {
      goto msix_map_init_error;
    }
  }
  for (irq = bf_tof2_int_tbus_lo; irq < (bf_tof2_int_tbus_lo + 3);
       irq++) {  // TBUS MSIX
    if ((status = bf_msix_map_view_init(dev_id, irq, BF_SWITCHD_MSIX_TBUS)) !=
        BF_SUCCESS) {
      goto msix_map_init_error;
    }
  }
  for (irq = bf_tof2_int_cbus_lo; irq < (bf_tof2_int_cbus_lo + 32);
       irq++) {  // CBUS MSIX
    if ((status = bf_msix_map_view_init(dev_id, irq, BF_SWITCHD_MSIX_CBUS)) !=
        BF_SUCCESS) {
      goto msix_map_init_error;
    }
  }
  for (irq = bf_tof2_int_mbus_lo; irq <= bf_tof2_int_mbus_hi;
       irq++) {  // MBUS MSIX
    if ((status = bf_msix_map_view_init(dev_id, irq, BF_SWITCHD_MSIX_MBUS)) !=
        BF_SUCCESS) {
      goto msix_map_init_error;
    }
  }
  // PBUS
  irq_base = bf_tof2_int_pbus_lo;  // pipe 0 base
  for (pipe = 0; pipe < 4; pipe++) {
    for (irq = irq_base; irq < (irq_base + 44); irq++) {
      if ((status = bf_msix_map_view_init(dev_id, irq, BF_SWITCHD_MSIX_PBUS)) !=
          BF_SUCCESS) {
        goto msix_map_init_error;
      }
    }
    irq_base += 64;  // move to next pipe index
  }
  bf_sys_log_and_trace(BF_MOD_SWITCHD,
                       BF_LOG_DBG,
                       "bf_switchd msix_map set for dev_id %d",
                       dev_id);
  return BF_SUCCESS;

msix_map_init_error:
  bf_sys_log_and_trace(
      BF_MOD_SWITCHD,
      BF_LOG_ERR,
      "ERROR: bf_switchd_msix_map_init failed %d for dev_id %d irq %d",
      status,
      dev_id,
      irq);
  return status;
}

/* Routine to instantiate and initialize a device with the driver */
bf_status_t bf_switchd_device_add(bf_dev_id_t dev_id, bool setup_dma) {
  bf_status_t status = BF_SUCCESS;
  bf_device_profile_t dev_profile;
  bf_dma_info_t *dma_info = NULL;
  bool is_sw_model;
  bf_dev_flags_t flags = 0;

  if (!bf_dev_id_validate(dev_id)) {
    return BF_INVALID_ARG;
  }

  dma_info = &(switchd_ctx->dma_info[dev_id][0]);

  /* Initialize P4 profile for the device */
  memset(&dev_profile, 0, sizeof dev_profile);
  if (switchd_ctx->asic[dev_id].configured) {
    status = bf_switchd_init_device_profile(
        dev_id, switchd_ctx->args.skip_p4, &dev_profile);
    if (status != BF_SUCCESS) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "ERROR: bf_switchd_init_device_profile failed(%d) for dev_id %d",
          status,
          dev_id);
      return status;
    }
  }
  if (switchd_ctx->args.skip_p4) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "Skipping P4 program load for dev_id %d",
                         dev_id);
  }
  if (switchd_ctx->asic[dev_id].is_virtual) {
    bf_dev_type_t vir_dev_type = BF_DEV_UNKNOWN;
    switch (switchd_ctx->asic[dev_id].chip_family) {
      case BF_DEV_FAMILY_TOFINO:
        vir_dev_type = BF_DEV_BFNT10064Q;
        break;
      case BF_DEV_FAMILY_TOFINO2:
        vir_dev_type = BF_DEV_BFNT20128Q;
        break;
      case BF_DEV_FAMILY_TOFINO3:
        vir_dev_type = BF_DEV_BFNT31_12Q;
        break;



      case BF_DEV_FAMILY_UNKNOWN:
        vir_dev_type = BF_DEV_UNKNOWN;
        break;
    }
    status = bf_virtual_device_add(dev_id, &dev_profile, vir_dev_type);
    if (status != BF_SUCCESS) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "ERROR: bf_virtual_device_add failed(%d) for dev_id %d",
          status,
          dev_id);
      bf_switchd_free_device_profile(&dev_profile);
      return status;
    }
  } else {
    is_sw_model = switchd_ctx->asic[dev_id].is_sw_model;
    if (is_sw_model == false && !reg_dir_i2c_rd_func && !reg_dir_i2c_wr_func) {
      /* Map the device to user space */
      status = bf_mmap_dev(dev_id);
      if (status != BF_SUCCESS) {
        bf_sys_log_and_trace(BF_MOD_SWITCHD,
                             BF_LOG_ERR,
                             "ERROR: Device mmap failed for dev_id %d",
                             dev_id);
        bf_sys_log_and_trace(
            BF_MOD_SWITCHD,
            BF_LOG_ERR,
            "Please load driver with bf_kdrv_mod_load script. Exiting.. \n");
        bf_switchd_free_device_profile(&dev_profile);
        return BF_NO_SYS_RESOURCES;  // return error instead of exiting
      }
#ifdef __sun
	if (switchd_ctx->args.kernel_pkt) {
		int resetting = 1;
		printf("trigger tfpkt reset\n");
		int dev_fd = (&(switchd_ctx->pcie_map[dev_id][0]))->dev_fd;
		if (ioctl(dev_fd, BF_PKT_INIT, &resetting) != 0) {
			printf("failed to trigger tfpkt reset: %s\n",
			    strerror(errno));
		}
	}
#endif
      /* add pci error signal handler */
      if (switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO3) {
        int dev_fd, cnt;
        for (cnt = 0; cnt < BF_MAX_SUBDEV_COUNT; cnt++) {
          dev_fd = (&(switchd_ctx->pcie_map[dev_id][cnt]))->dev_fd;
          if (dev_fd >= 0) {
            setup_async_notification(dev_fd);
          }
        }
      } else {
        setup_async_notification((&(switchd_ctx->pcie_map[dev_id][0]))->dev_fd);
      }
      /*  initialize msix map */
      if (switchd_ctx->asic[dev_id].pcie_cfg.int_mode ==
              BF_SWITCHD_INT_MODE_MSIX &&
          ((switchd_ctx->asic[dev_id].chip_family != BF_DEV_FAMILY_UNKNOWN) &&
           (switchd_ctx->asic[dev_id].chip_family != BF_DEV_FAMILY_TOFINO))) {
        int msix_indices[3] = {
            BF_SWITCHD_MSIX_TBUS, BF_SWITCHD_MSIX_TBUS, BF_SWITCHD_MSIX_TBUS};
        status = bf_switchd_msix_map_init(dev_id);
        if (status != BF_SUCCESS) {
          bf_sys_log_and_trace(BF_MOD_SWITCHD,
                               BF_LOG_CRIT,
                               "ERROR: msix_map init failed for dev_id %d",
                               dev_id);
          /* Exit thread as this is critical */
          exit(-1);
        }
        if (switchd_ctx->args.kernel_pkt) {
          if (bf_switchd_tbus_msix_index_set(
                  switchd_ctx->pcie_map[dev_id][0].dev_fd, 3, msix_indices)) {
            bf_sys_log_and_trace(
                BF_MOD_SWITCHD,
                BF_LOG_CRIT,
                "ERROR: tbus_msix_index_set failed for dev_id %d",
                dev_id);
            /* Exit thread as this is critical */
            exit(-1);
          }
        }
      }
    }
    if (setup_dma) {
      /* Setup DMA memory for the device */
      int num_unique_pipelines = 0;
      for (int i = 0; i < dev_profile.num_p4_programs; ++i)
        num_unique_pipelines += dev_profile.p4_programs[i].num_p4_pipelines;
      for (int i = 0; i < BF_MAX_SUBDEV_COUNT; i++) {
        if (!switchd_ctx->pcie_map[dev_id][i].configured) continue;
        if (bf_switchd_setup_dma_mem(dev_id, i, num_unique_pipelines)) {
          bf_sys_log_and_trace(
              BF_MOD_SWITCHD,
              BF_LOG_ERR,
              "ERROR: dma init failed for dev_id %d subdev_id %d",
              dev_id,
              i);
          bf_unmap_dev(dev_id);
          bf_switchd_free_device_profile(&dev_profile);
          return BF_NO_SYS_RESOURCES;
        }
      }
    }

    /* Set the is_sw_model value in drv flags to pass it to bf-drivers */
    BF_DEV_IS_SW_MODEL_SET(flags, is_sw_model);
    status = bf_pktmgr_device_add(
        switchd_ctx->asic[dev_id].chip_family, dev_id, dma_info, flags);
    if (status != BF_SUCCESS) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "ERROR: bf_pktmgr_device_add failed(%d) for dev_id %d",
          status,
          dev_id);
      for (int i = 0; i < BF_MAX_SUBDEV_COUNT; i++) {
        if (switchd_ctx->pcie_map[dev_id][i].configured) {
          bf_switchd_release_dma_mem(dev_id, i);
        }
      }
      if (is_sw_model == false) {
        bf_unmap_dev(dev_id);
      }

      bf_switchd_free_device_profile(&dev_profile);
      return status;
    }

    bf_sys_mutex_lock(&switchd_ctx->asic[dev_id].pktmgr_mutex);
    switchd_ctx->state[dev_id].device_pktmgr_ready = true;
    bf_sys_mutex_unlock(&switchd_ctx->asic[dev_id].pktmgr_mutex);

    /* device add only if  using PCIe access */
    /* *** might want to disable bf_device_add for i2c_only_image */
    /* Invoke the driver API to add the device */
    status = bf_device_add(switchd_ctx->asic[dev_id].chip_family,
                           dev_id,
                           &dev_profile,
                           dma_info,
                           flags);
    if (status != BF_SUCCESS) {
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "ERROR: bf_device_add failed(%d) for dev_id %d",
                           status,
                           dev_id);
      for (int i = 0; i < BF_MAX_SUBDEV_COUNT; i++) {
        if (switchd_ctx->pcie_map[dev_id][i].configured) {
          bf_switchd_release_dma_mem(dev_id, i);
        }
      }
      if (is_sw_model == false) {
        bf_unmap_dev(dev_id);
      }

      bf_switchd_free_device_profile(&dev_profile);
      return status;
    }
  }
  bf_switchd_free_device_profile(&dev_profile);

#ifdef LINKDOWN_INTERRUPT_ENABLED
  // un-comment to detect port-dn via interrupts
  //
  bf_pm_interrupt_based_link_monitoring_set(dev_id, true);
#endif

  // un-comment to enable packet DR interrupts
  //
  // pkt_mgr_dr_int_en(dev_id, true);

  switchd_ctx->state[dev_id].device_ready = true;

  return status;
}

/* Routine to remove a device. */
bf_status_t bf_switchd_device_remove(bf_dev_id_t dev_id) {
  if (!bf_dev_id_validate(dev_id)) return BF_INVALID_ARG;
  if (!switchd_ctx->asic[dev_id].configured) return BF_OBJECT_NOT_FOUND;

  bf_sys_mutex_lock(&switchd_ctx->asic[dev_id].switch_mutex);
  if (!switchd_ctx->state[dev_id].device_ready) {
    bf_sys_mutex_unlock(&switchd_ctx->asic[dev_id].switch_mutex);
    return BF_OBJECT_NOT_FOUND;
  }
  switchd_ctx->state[dev_id].device_ready = false;
  bf_sys_mutex_unlock(&switchd_ctx->asic[dev_id].switch_mutex);
  bf_status_t sts = bf_device_remove(dev_id);
  if (sts != BF_SUCCESS) return sts;
  bf_switchd_release_dma_mem(dev_id, 0);
  return BF_SUCCESS;
}

static uint32_t bf_switchd_get_max_subdev_id_cnt() {
  bf_dev_id_t dev_id = 0;
  uint32_t max_subdev_id_cnt = 1;
  if (lld_sku_get_num_subdev(dev_id, &max_subdev_id_cnt, NULL) != LLD_OK) {
    max_subdev_id_cnt = 1;
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "ERROR: lld_sku_get_num_subdev failed for dev_id %d",
                         dev_id);
  }
  return max_subdev_id_cnt;
}

static int bf_switchd_check_for_interrupts_or_timeout(
    fd_set *dev_fd_set_p, uint32_t timeout_value_us) {
  int err;
  struct timeval tv;
  static struct timeval warned_at = {0, 0};
  int maxfd = 0;
  bf_dev_id_t dev_id = 0;
  switchd_state_t *dev_state;
  static uint32_t max_subdev_id_cnt = 0;

  if (max_subdev_id_cnt == 0) {
    max_subdev_id_cnt = bf_switchd_get_max_subdev_id_cnt();
  }

  /* just sleep  and return if using i2c-only access */
  if (reg_dir_i2c_rd_func && reg_dir_i2c_wr_func) {
    bf_sys_usleep(timeout_value_us);
    return 0;
  }

  FD_ZERO(dev_fd_set_p);

  /* Initialize the device fd set to be used with select() */
  switchd_pcie_map_t *pcie_map;

  for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    if (switchd_ctx->asic[dev_id].is_sw_model) {
      continue;
    }

    if (switchd_ctx->asic[dev_id].is_virtual) {
      continue;
    }

    dev_state = &(switchd_ctx->state[dev_id]);
    if (dev_state->device_locked || !dev_state->device_ready) {
      continue;
    }
    for (uint32_t cnt = 0; cnt < max_subdev_id_cnt; cnt++) {
      pcie_map = &(switchd_ctx->pcie_map[dev_id][cnt]);

      if (pcie_map->dev_fd == -1) {
        /* Skip the device if it's not mmap-ed */
        continue;
      }

      FD_SET(pcie_map->dev_fd, dev_fd_set_p);
      maxfd = (maxfd > pcie_map->dev_fd) ? maxfd : pcie_map->dev_fd;
    }
  }

  /* Run select on all device FDs with a timrout of 1us */
  tv.tv_sec = 0;
  tv.tv_usec = timeout_value_us;
  err = select(maxfd + 1, dev_fd_set_p, NULL, NULL, &tv);
  if (err < 0) {
	  gettimeofday(&tv, NULL);
	  if (tv.tv_sec - warned_at.tv_sec >= 300) {
		  warned_at.tv_sec = tv.tv_sec;
		  bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "ERROR: select() failed with ret_code %d",
                         err);
	  }
  }
  return 0;
}

static bf_status_t bf_switchd_check_for_port_interrupts(void) {
  return bf_port_check_port_int();
}

static int bf_switchd_service_interrupts(bf_dev_id_t dev_id,
                                         fd_set *dev_fd_set_p) {
  switchd_state_t *dev_state;
  int int_enable = 0;
  static uint32_t max_subdev_id_cnt = 0;

  if (max_subdev_id_cnt == 0) {
    max_subdev_id_cnt = bf_switchd_get_max_subdev_id_cnt();
  }

  if (switchd_ctx->asic[dev_id].is_sw_model) {
    return 0;
  }

  /* return if using i2c-only access */
  if (reg_dir_i2c_rd_func && reg_dir_i2c_wr_func) {
    return 0;
  }

  dev_state = &(switchd_ctx->state[dev_id]);
  if (dev_state->device_locked || !dev_state->device_ready) {
    return 0;
  }

  switchd_pcie_map_t *pcie_map;

  /* Handle interrupts for devices with ready FDs */
  switchd_pcie_cfg_t *pcie_cfg = &(switchd_ctx->asic[dev_id].pcie_cfg);

  for (uint32_t subdev_id = 0; subdev_id < max_subdev_id_cnt; subdev_id++) {
    pcie_map = &(switchd_ctx->pcie_map[dev_id][subdev_id]);

    if (FD_ISSET(pcie_map->dev_fd, dev_fd_set_p)) {
      size_t mode_cnt;
      ssize_t status_cnt;
      if (pcie_cfg->int_mode ==
          BF_SWITCHD_INT_MODE_MSIX) { /* msix mode, read 512*4 bytes */
        mode_cnt = 512 * sizeof(uint32_t);
      } else if (pcie_cfg->int_mode ==
                 BF_SWITCHD_INT_MODE_MSI) { /* msi mode, read 8 bytes */
        mode_cnt = 2 * sizeof(uint32_t);
      } else {
        mode_cnt = sizeof(uint32_t);
      }
      status_cnt = read(pcie_map->dev_fd, &interrupt_cnt, mode_cnt);
      if (status_cnt <= 0) {
        return 0;
      }
      status_cnt /= 4; /* number of "u32s" read */

      int_enable = 1; /* has to be 32bit */
      if (pcie_cfg->int_mode == BF_SWITCHD_INT_MODE_MSI) {
        /* MSI-1 is owned by kernel if using kernel packet processing */
        if (!switchd_ctx->args.kernel_pkt) {
          for (int i = 0; i < 2; i++) {
            if (interrupt_cnt[i] != 0) {
              bf_int_msi_svc(dev_id, subdev_id, i, false);
            }
          }
        } else {
          if (interrupt_cnt[bf_tof_msi_int_0_switch] != 0) {
            bf_int_msi_svc(dev_id, subdev_id, bf_tof_msi_int_0_switch, false);
          }
        }
      } else if (pcie_cfg->int_mode == BF_SWITCHD_INT_MODE_MSIX) {
        for (ssize_t i = 0; i < status_cnt; i++) {
          if (interrupt_cnt[i] != 0) {
            bf_int_msi_x_svc(dev_id, subdev_id, i, false);
          }
        }
      } else {
        bf_int_int_x_svc(dev_id, subdev_id, false);
      }

      /* next, reenable the interrupt */
      ssize_t ws = write(pcie_map->dev_fd, &int_enable, sizeof(int_enable));
      (void)ws;
    }
  }

  return 0;
}

#ifndef STATIC_LINK_LIB
/* Helper routine to load a library */
static void *bf_switchd_lib_load(char *lib) {
  void *handle = NULL;
  char *error;

  if (strlen(lib) == 0) {
    return NULL;
  }

  handle = dlopen(lib, RTLD_LAZY | RTLD_GLOBAL);
  if (handle == NULL && (error = dlerror()) != NULL) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "ERROR:%s:%d: dlopen failed for %s, err=%s",
                         __func__,
                         __LINE__,
                         lib,
                         error);
    return NULL;
  }
  bf_sys_log_and_trace(
      BF_MOD_SWITCHD, BF_LOG_DBG, "bf_switchd: library %s loaded", lib);
  return handle;
}

/* Load all agent libs */
static int bf_switchd_agent_lib_load(bf_dev_id_t dev_id) {
  p4_devices_t *p4_device = &(switchd_ctx->p4_devices[dev_id]);
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);
  int agent_idx = 0;

  if (!p4_device || !state) {
    return 0;
  }
  for (agent_idx = 0; agent_idx < BF_SWITCHD_MAX_AGENTS; agent_idx++) {
    if (strlen(p4_device->agent[agent_idx])) {
      if (access(p4_device->agent[agent_idx], F_OK) == -1) {
        continue;
      }
      state->agent_lib_handle[agent_idx] =
          bf_switchd_lib_load(p4_device->agent[agent_idx]);
      if (state->agent_lib_handle[agent_idx] == NULL) {
        bf_sys_log_and_trace(BF_MOD_SWITCHD,
                             BF_LOG_ERR,
                             "ERROR: agent[%d] lib loading failure",
                             agent_idx);
        return -1;
      }
    }
  }
  return 0;
}

/* Load mav diag lib */
static int bf_switchd_accton_diag_lib_load(bf_dev_id_t dev_id) {
  int j = 0;
  p4_devices_t *p4_device = &(switchd_ctx->p4_devices[dev_id]);
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);

  if (!p4_device || !state || switchd_ctx->args.skip_p4) {
    return 0;
  }
  for (j = 0; j < p4_device->num_p4_programs; j++) {
    p4_programs_t *p4_programs = &(p4_device->p4_programs[j]);
    switchd_p4_program_state_t *p4_program_state =
        &(state->p4_programs_state[j]);

    if (strlen(p4_programs->accton_diag)) {
      if (access(p4_programs->accton_diag, F_OK) == -1) {
        return -1;
      }
      p4_program_state->accton_diag_lib_handle =
          bf_switchd_lib_load(p4_programs->accton_diag);
      if (p4_program_state->accton_diag_lib_handle == NULL) {
        bf_sys_log_and_trace(BF_MOD_SWITCHD,
                             BF_LOG_ERR,
                             "ERROR: accton_diag lib loading failure");
        return -1;
      }

      /* Load accton_diag library only once per device */
      return 0;
    }
  }
  return 0;
}

/* Load P4 specific libraries */
static int bf_switchd_p4_pd_lib_load(bf_dev_id_t dev_id) {
  int i = 0, j = 0;
  p4_devices_t *p4_device = &(switchd_ctx->p4_devices[dev_id]);
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);

  if (!p4_device || !state || switchd_ctx->args.skip_p4) {
    return 0;
  }

  for (j = 0; j < p4_device->num_p4_programs; j++) {
    p4_programs_t *p4_programs = &(p4_device->p4_programs[j]);
    switchd_p4_program_state_t *p4_program_state =
        &(state->p4_programs_state[j]);

    for (i = 0; i < p4_programs->num_p4_pipelines; i++) {
      p4_pipeline_config_t *p4_pipeline = &(p4_programs->p4_pipelines[i]);
      switchd_p4_pipeline_state_t *pipeline_state =
          &(p4_program_state->p4_pipeline_state[i]);
      /* Load the PD API lib */
      if (p4_pipeline && strlen(p4_pipeline->pd)) {
        pipeline_state->pd_lib_handle = bf_switchd_lib_load(p4_pipeline->pd);
        if (pipeline_state->pd_lib_handle == NULL) {
          return -1;
        }
      }

#ifdef THRIFT_ENABLED
      /* Load the PD Thrift API lib */
      if (p4_pipeline && strlen(p4_pipeline->pd_thrift)) {
        pipeline_state->pd_thrift_lib_handle =
            bf_switchd_lib_load(p4_pipeline->pd_thrift);
        if (pipeline_state->pd_thrift_lib_handle == NULL) {
          return -1;
        }
      }
#endif
    }
  }
  return 0;
}

/* Unload mav diag lib */
static void bf_switchd_accton_diag_lib_unload(bf_dev_id_t dev_id) {
  int j = 0;
  p4_devices_t *p4_device = &(switchd_ctx->p4_devices[dev_id]);
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);
  int ret = 0;

  if (!p4_device || !state) {
    return;
  }

  for (j = 0; j < p4_device->num_p4_programs; j++) {
    p4_programs_t *p4_programs = &(p4_device->p4_programs[j]);
    switchd_p4_program_state_t *p4_program_state =
        &(state->p4_programs_state[j]);

    if (p4_program_state->accton_diag_lib_handle) {
      /* Cancel the thread before unloading lib */
      if (switchd_ctx->args.accton_diag_t_id) {
        ret = pthread_cancel(switchd_ctx->args.accton_diag_t_id);
        if (ret != 0) {
          bf_sys_log_and_trace(
              BF_MOD_SWITCHD,
              BF_LOG_ERR,
              "ERROR: thread cancel failed for accton_diag : %d",
              ret);
        } else {
          pthread_join(switchd_ctx->args.accton_diag_t_id, NULL);
        }
        switchd_ctx->args.accton_diag_t_id = 0;
      }
      dlclose(p4_program_state->accton_diag_lib_handle);
      p4_program_state->accton_diag_lib_handle = NULL;
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_DBG,
          "bf_switchd: Mav diag library %s unloaded for dev_id %d",
          p4_programs->accton_diag,
          dev_id);
    }
  }
}

/* De-Initialize diag library */
static void bf_switchd_diag_lib_deinit(bf_dev_id_t dev_id) {
  int j = 0;
  p4_devices_t *p4_device = &(switchd_ctx->p4_devices[dev_id]);
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);
  char *fn_name = "bf_diag_deinit";
  int (*diag_deinit_fn)();

  if (switchd_ctx->args.skip_p4) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD, BF_LOG_DBG, "Skip diag lib deinit");
    return;
  }

  if (!state) {
    return;
  }

  for (j = 0; j < p4_device->num_p4_programs; j++) {
    switchd_p4_program_state_t *p4_program_state =
        &(state->p4_programs_state[j]);

    if (p4_program_state->diag_lib_handle == NULL) {
      continue;
    }

    /* Deinitialize the diag library */
    *(pvoid_dl_t *)(&diag_deinit_fn) =
        dlsym(p4_program_state->diag_lib_handle, fn_name);
    if (diag_deinit_fn) {
      diag_deinit_fn(dev_id);
    }
    /* Currently the diag libraries are initialized only once */
    return;
  }
}

/* Unload P4 specific libraries */
static void bf_switchd_p4_pd_lib_unload(bf_dev_id_t dev_id) {
  int j = 0;
  p4_devices_t *p4_device = &(switchd_ctx->p4_devices[dev_id]);
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);
  char pd_cleanup_fn_name[250];
  char pd_device_cleanup_fn_name[250];
  void (*pd_cleanup_fn)(void);
  void (*pd_device_state_cleanup_fn)(bf_dev_id_t);
  int i = 0;

  if (!p4_device || !state) {
    return;
  }

  int rc;
  for (j = 0; j < p4_device->num_p4_programs; j++) {
    p4_programs_t *p4_programs = &(p4_device->p4_programs[j]);
    switchd_p4_program_state_t *p4_program_state =
        &(state->p4_programs_state[j]);

    for (i = 0; i < p4_programs->num_p4_pipelines; i++) {
      switchd_p4_pipeline_state_t *pipeline_state =
          &(p4_program_state->p4_pipeline_state[i]);
      p4_pipeline_config_t *p4_pipeline = &(p4_programs->p4_pipelines[i]);
      /* Unload the PD API lib */
      if (pipeline_state->pd_lib_handle) {
#ifdef THRIFT_ENABLED
        bf_switchd_pd_thrift_lib_deinit(dev_id, p4_pipeline, pipeline_state);
#endif
        const char *p4_prefix = p4_programs->program_name;
        if (strcmp(p4_programs->program_name, "switch") == 0) {
          p4_prefix = "dc";
        }

        sprintf(pd_cleanup_fn_name, "p4_pd_%s_cleanup", p4_prefix);
        bf_switchd_find_lib_fn(pd_cleanup_fn_name,
                               pipeline_state->pd_lib_handle,
                               (pvoid_dl_t *)&pd_cleanup_fn);
        bf_sys_assert(pd_cleanup_fn != NULL);

        sprintf(pd_device_cleanup_fn_name,
                "p4_pd_%s_device_state_cleanup",
                p4_prefix);
        bf_switchd_find_lib_fn(pd_cleanup_fn_name,
                               pipeline_state->pd_lib_handle,
                               (pvoid_dl_t *)&pd_device_state_cleanup_fn);
        bf_sys_assert(pd_device_state_cleanup_fn != NULL);

        pd_device_state_cleanup_fn(dev_id);
        pd_cleanup_fn();
        rc = dlclose(pipeline_state->pd_lib_handle);
        bf_sys_assert(rc == 0);
        pipeline_state->pd_lib_handle = NULL;
        bf_sys_log_and_trace(BF_MOD_SWITCHD,
                             BF_LOG_DBG,
                             "bf_switchd: library %s unloaded for dev_id %d",
                             p4_pipeline->pd,
                             dev_id);
      }

#ifdef THRIFT_ENABLED
      /* Unload the PD Thrift API lib */
      if (pipeline_state->pd_thrift_lib_handle) {
        rc = dlclose(pipeline_state->pd_thrift_lib_handle);
        bf_sys_assert(rc == 0);
        pipeline_state->pd_thrift_lib_handle = NULL;
        bf_sys_log_and_trace(BF_MOD_SWITCHD,
                             BF_LOG_DBG,
                             "bf_switchd: library %s unloaded for dev_id %d",
                             p4_pipeline->pd_thrift,
                             dev_id);
      }
#endif
    }
  }
}

/* Unload switch.p4 specific libraries */
static void bf_switchd_p4_switch_lib_unload(bf_dev_id_t dev_id) {
  int i = 0, j = 0;
  p4_devices_t *p4_device = &(switchd_ctx->p4_devices[dev_id]);
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);
  bf_dev_init_mode_t init_mode = switchd_ctx->args.init_mode;
  bool warm_boot = (init_mode != BF_DEV_INIT_COLD) ? true : false;
  int (*switchapi_stop_srv_fn)(void) = NULL;
  int (*switchapi_stop_packet_driver_fn)() = NULL;
  int (*switchsai_stop_srv_fn)(void) = NULL;
  int (*switchapi_deinit_fn)(int, bool, char *) = NULL;
  if (!p4_device || !state) {
    return;
  }

  for (j = 0; j < p4_device->num_p4_programs; j++) {
    p4_programs_t *p4_programs = &(p4_device->p4_programs[j]);
    switchd_p4_program_state_t *p4_program_state =
        &(state->p4_programs_state[j]);
    /* Stop the switch api rpc server */
    if (p4_program_state->switchapi_lib_handle) {
      *(pvoid_dl_t *)(&switchapi_stop_srv_fn) = dlsym(
          p4_program_state->switchapi_lib_handle, "stop_switch_api_rpc_server");
      if (switchapi_stop_srv_fn) {
        switchapi_stop_srv_fn();
      }

      /* Stop the switch api packet driver */
      *(pvoid_dl_t *)(&switchapi_stop_packet_driver_fn) =
          dlsym(p4_program_state->switchapi_lib_handle,
                "stop_switch_api_packet_driver");
      if (switchapi_stop_packet_driver_fn) {
        switchapi_stop_packet_driver_fn();
      }
    }
    /* Stop the switch SAI thrift server */
    if (p4_program_state->switchsai_lib_handle) {
      *(pvoid_dl_t *)(&switchsai_stop_srv_fn) =
          dlsym(p4_program_state->switchsai_lib_handle,
                "stop_p4_sai_thrift_rpc_server");
      if (switchsai_stop_srv_fn) {
        switchsai_stop_srv_fn();
      }
    }
    /* Deinit Switch APi */
    if (p4_program_state->switchapi_lib_handle) {
      *(pvoid_dl_t *)(&switchapi_deinit_fn) =
          dlsym(p4_program_state->switchapi_lib_handle, "switch_api_free");
    }

    if (switchapi_deinit_fn) {
      switchapi_deinit_fn(dev_id, false, NULL);
    } else {
      if (p4_program_state->switchapi_lib_handle) {
        *(pvoid_dl_t *)(&switchapi_deinit_fn) =
            dlsym(p4_program_state->switchapi_lib_handle, "bf_switch_clean");
      }

      if (switchapi_deinit_fn) {
        switchapi_deinit_fn(dev_id, warm_boot, "/tmp/db.txt");
      }
    }

    /* Unload the switch API lib */
    if (p4_program_state->switchapi_lib_handle) {
      i = dlclose(p4_program_state->switchapi_lib_handle);
      char *err = dlerror();
      if (i != 0)
        bf_sys_log_and_trace(BF_MOD_SWITCHD,
                             BF_LOG_ERR,
                             "bf_switchd: library %s unload error %d/%s",
                             p4_programs->switchapi,
                             i,
                             (err) ? err : "NONE");
      p4_program_state->switchapi_lib_handle = NULL;
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_DBG,
                           "bf_switchd: library %s unloaded for dev_id %d",
                           p4_programs->switchapi,
                           dev_id);
    }

    /* Unload the switch SAI lib */
    if (p4_program_state->switchsai_lib_handle) {
      dlclose(p4_program_state->switchsai_lib_handle);
      p4_program_state->switchsai_lib_handle = NULL;
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_DBG,
                           "bf_switchd: library %s unloaded for dev_id %d",
                           p4_programs->switchsai,
                           dev_id);
    }
  }
}

/* Load switch.p4 specific libraries */
static int bf_switchd_p4_switch_lib_load(bf_dev_id_t dev_id) {
  int j = 0;
  p4_devices_t *p4_device = &(switchd_ctx->p4_devices[dev_id]);
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);

  if (!p4_device || !state || switchd_ctx->args.skip_p4) {
    return 0;
  }

  for (j = 0; j < p4_device->num_p4_programs; j++) {
    p4_programs_t *p4_programs = &(p4_device->p4_programs[j]);
    switchd_p4_program_state_t *p4_program_state =
        &(state->p4_programs_state[j]);

    /* Load the switch API lib */
    if (p4_programs && strlen(p4_programs->switchapi)) {
      p4_program_state->switchapi_lib_handle =
          bf_switchd_lib_load(p4_programs->switchapi);
      if (p4_program_state->switchapi_lib_handle == NULL) {
        return -1;
      }
    }

    /* Load the switch SAI lib */
    if (p4_programs && strlen(p4_programs->switchsai)) {
      p4_program_state->switchsai_lib_handle =
          bf_switchd_lib_load(p4_programs->switchsai);
    }
  }

  return 0;
}

/* Initialize diag library */
static void bf_switchd_diag_lib_init(bf_dev_id_t dev_id) {
  int j = 0;
  p4_devices_t *p4_device = &(switchd_ctx->p4_devices[dev_id]);
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);
  char *fn_name = "bf_diag_init";
  int (*diag_init_fn)();

  if (switchd_ctx->args.skip_p4) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD, BF_LOG_DBG, "Skip diag lib init");
    return;
  }

  if (!state) {
    return;
  }

  for (j = 0; j < p4_device->num_p4_programs; j++) {
    switchd_p4_program_state_t *p4_program_state =
        &(state->p4_programs_state[j]);
    p4_programs_t *p4_programs = &(p4_device->p4_programs[j]);

    if (p4_program_state->diag_lib_handle == NULL) {
      continue;
    }

    /* Initialize the diag library */
    *(pvoid_dl_t *)(&diag_init_fn) =
        dlsym(p4_program_state->diag_lib_handle, fn_name);
    if (diag_init_fn) {
      const char *cpu_port = NULL;

      if (p4_programs->use_eth_cpu_port) {
        cpu_port = p4_programs->eth_cpu_port_name;
      }
      diag_init_fn(dev_id, cpu_port);
      /* Currently the diag libraries need to be initialized only once */
      return;
    }
  }
}

/* Initialize accton_diag library */
static void bf_switchd_accton_diag_lib_init(bf_dev_id_t dev_id) {
  int j = 0;
  p4_devices_t *p4_device = &(switchd_ctx->p4_devices[dev_id]);
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);
  agent_init_fn_t agent_init_fn;
  int ret = 0;

  if (switchd_ctx->args.skip_p4) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD, BF_LOG_DBG, "Skip mav diag lib init");
    return;
  }

  if (!state) {
    return;
  }
  if (dev_id != 0) {
    return;
  }

  for (j = 0; j < p4_device->num_p4_programs; j++) {
    switchd_p4_program_state_t *p4_program_state =
        &(state->p4_programs_state[j]);

    if (p4_program_state->accton_diag_lib_handle == NULL) {
      continue;
    }

    /* Initialize the mav diag library */
    *(pvoid_dl_t *)(&agent_init_fn) = dlsym(
        p4_program_state->accton_diag_lib_handle, "bf_switchd_agent_init");
    if (agent_init_fn) {
      /* Start the agent thread */
      pthread_attr_t agent_t_attr;
      pthread_attr_init(&agent_t_attr);
      ret = pthread_create(&(switchd_ctx->args.accton_diag_t_id),
                           &agent_t_attr,
                           agent_init_fn,
                           NULL);
      pthread_attr_destroy(&agent_t_attr);
      if (ret != 0) {
        bf_sys_log_and_trace(
            BF_MOD_SWITCHD,
            BF_LOG_ERR,
            "ERROR: thread creation failed for accton_diag service: %d",
            ret);
        return;
      }
      pthread_setname_np(switchd_ctx->args.accton_diag_t_id, "bf_mdiag_srv");
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD, BF_LOG_DBG, "bf_switchd: accton_diag initialized");

      /* Currently the mav diag libraries need to be initialized only once */
      return;
    } else {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "ERROR: bf_switchd: accton_diag init function not found");
    }
  }
}
#endif  // STATIC_LINK_LIB

/* Initialize P4 specific libraries */
static void bf_switchd_p4_lib_init(bf_dev_id_t dev_id) {
  if (switchd_ctx->args.skip_p4) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD, BF_LOG_DBG, "Skip p4 lib init");
    return;
  }

#ifndef STATIC_LINK_LIB
  /* Initialize the PD API lib */
  bf_switchd_pd_lib_init(dev_id);

#ifdef THRIFT_ENABLED
  /* Initialize the PD Thrift API lib */
  bf_switchd_pd_thrift_lib_init(dev_id);
#endif  // THRIFT_ENABLED

  /* Initialize the switch API lib (only expected for switch.p4) */
  bf_switchd_switchapi_lib_init(dev_id);

  /* Initialize the switch SAI lib (only expected for switch.p4) */
  bf_switchd_switchsai_lib_init(dev_id);

  if (!switchd_ctx->args.skip_hld.pipe_mgr) {
    /* Initialize the diag lib */
    bf_switchd_diag_lib_init(dev_id);
  } else {
    printf("Skipped diag init as pipe-mgr was skipped\n");
  }
#endif  // STATIC_LINK_LIB
}

#ifndef STATIC_LINK_LIB
/* Load diag specific libraries */
static int bf_switchd_diag_lib_load(bf_dev_id_t dev_id) {
  int j = 0;
  p4_devices_t *p4_device = &(switchd_ctx->p4_devices[dev_id]);
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);

  if (!p4_device || !state) {
    return 0;
  }

  for (j = 0; j < p4_device->num_p4_programs; j++) {
    p4_programs_t *p4_program = &(p4_device->p4_programs[j]);
    switchd_p4_program_state_t *p4_program_state =
        &(state->p4_programs_state[j]);

    if (p4_program && strlen(p4_program->diag)) {
      p4_program_state->diag_lib_handle = bf_switchd_lib_load(p4_program->diag);
      if (p4_program_state->diag_lib_handle == NULL) {
        return -1;
      }

      /* Load diag library only once per device */
      return 0;
    }
  }
  return 0;
}

/* Unload diag specific libraries */
static void bf_switchd_diag_lib_unload(bf_dev_id_t dev_id) {
  int j = 0;
  p4_devices_t *p4_device = &(switchd_ctx->p4_devices[dev_id]);
  switchd_state_t *state = &(switchd_ctx->state[dev_id]);

  bf_switchd_diag_lib_deinit(dev_id);

  if (!p4_device || !state) {
    return;
  }

  for (j = 0; j < p4_device->num_p4_programs; j++) {
    p4_programs_t *p4_programs = &(p4_device->p4_programs[j]);
    switchd_p4_program_state_t *p4_program_state =
        &(state->p4_programs_state[j]);

    if (p4_program_state->diag_lib_handle) {
      dlclose(p4_program_state->diag_lib_handle);
      p4_program_state->diag_lib_handle = NULL;
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "bf_switchd: library %s unloaded for dev_id %d",
                           p4_programs->diag,
                           dev_id);
    }
  }
}
#endif  // STATIC_LINK_LIB

static bf_status_t bf_switchd_warm_init_begin(
    bf_dev_id_t dev_id,
    bf_dev_init_mode_t warm_init_mode,
    bf_dev_serdes_upgrade_mode_t serdes_upgrade_mode,
    bool upgrade_agents) {
  if (!bf_dev_id_validate(dev_id)) {
    return BF_INVALID_ARG;
  }

  if (upgrade_agents && (warm_init_mode == BF_DEV_WARM_INIT_FAST_RECFG_QUICK)) {
    upgrade_agents = false;
  }

  /* During cold init the library must be initilalised */
  if (!upgrade_agents && (warm_init_mode == BF_DEV_INIT_COLD)) {
    upgrade_agents = true;
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD,
        BF_LOG_WARN,
        "Forcing upgrade_agents to True during cold-init for dev_id %d",
        dev_id);
  }

  /* Only if device is ready do fast-reconfig */
  bf_sys_mutex_lock(&switchd_ctx->asic[dev_id].switch_mutex);
  if (switchd_ctx->state[dev_id].device_ready == false) {
    bf_sys_mutex_unlock(&switchd_ctx->asic[dev_id].switch_mutex);
    return BF_NOT_READY;
  }

  /* Set the switchd_ctx->init_mode to the appropriate mode. So that the
   * agent libraries know of the init mode via the passed in switchd_ctx
   */
  switchd_ctx->args.init_mode = warm_init_mode;

  bf_sys_log_and_trace(
      BF_MOD_SWITCHD,
      BF_LOG_DBG,
      "bf_switchd: starting warm init%s for dev_id %d mode %d serdes_upgrade "
      "%d",
      (warm_init_mode == BF_DEV_WARM_INIT_FAST_RECFG_QUICK) ? " quick" : "",
      dev_id,
      warm_init_mode,
      serdes_upgrade_mode);

  bf_sys_mutex_lock(&switchd_ctx->asic[dev_id].pktmgr_mutex);
  switchd_ctx->state[dev_id].device_locked = true;
  switchd_ctx->state[dev_id].device_pktmgr_ready = false;
  bf_sys_mutex_unlock(&switchd_ctx->asic[dev_id].pktmgr_mutex);
  bf_sys_mutex_unlock(&switchd_ctx->asic[dev_id].switch_mutex);

  if (upgrade_agents) {
#ifdef THRIFT_ENABLED
    /* Deinit the agent thrift service */
    bf_switchd_agent_thrift_lib_deinit(dev_id);
#endif
    /* Unload and reload the agents */
    bf_switchd_agent_lib_deinit(dev_id);
#ifndef STATIC_LINK_LIB
    bf_switchd_agent_lib_load(dev_id);

    /* Reset platform function pointers after warm init */
    bf_pm_update_pltfm_interface();
#endif  // STATIC_LINK_LIB
    bf_switchd_agent_lib_init(dev_id);
    bf_switchd_agent_lib_wait_init_done(dev_id);
#ifdef THRIFT_ENABLED
    /* Init the agent thrift service */
    bf_switchd_agent_thrift_lib_init(dev_id);
#endif
  }

#ifndef STATIC_LINK_LIB
  /* Unload P4 Libs */
  /* TODO: STATIC_LINK_LIB_TODO - Fix this for STATIC_LINK_LIB case */
  if (warm_init_mode != BF_DEV_WARM_INIT_FAST_RECFG_QUICK) {
    bf_switchd_p4_switch_lib_unload(dev_id);
    bf_switchd_diag_lib_unload(dev_id);

    /* Unload the mav diag library. */
    bf_switchd_accton_diag_lib_unload(dev_id);
  }
#endif  // STATIC_LINK_LIB

  bf_device_warm_init_begin(dev_id, warm_init_mode, serdes_upgrade_mode);

  if (warm_init_mode != BF_DEV_WARM_INIT_FAST_RECFG_QUICK) {
#ifndef STATIC_LINK_LIB
    /* Unload the PD library after the device is removed, so that the
     * device remove callback gets executed
     */
    /* TODO: STATIC_LINK_LIB_TODO - Fix this for STATIC_LINK_LIB case */
    bf_switchd_p4_pd_lib_unload(dev_id);
#endif  // STATIC_LINK_LIB

    /* Unmap the associated UIO device if the device is ASIC */
    if (switchd_ctx->asic[dev_id].is_sw_model == false) {
      bf_unmap_dev(dev_id);
    }
  }

  return BF_SUCCESS;
}

static bf_status_t bf_switchd_device_add_with_p4(
    bf_dev_id_t dev_id, bf_device_profile_t *device_profile) {
  if (!bf_dev_id_validate(dev_id)) {
    return BF_INVALID_ARG;
  }

  if (device_profile) {
    switchd_ctx->args.skip_p4 = (device_profile->num_p4_programs == 0);

    /* Update the switchd context with the information from the new profile
     * since bf_switchd_device_add will construct a new bf_device_profile_t from
     * the switchd context. */
    bf_p4_pipeline_t *pipeline_profile =
        &(device_profile->p4_programs[0].p4_pipelines[0]);
    struct stat stat_buf;
    bool absolute_paths = (stat(pipeline_profile->cfg_file, &stat_buf) == 0);
    switch_p4_pipeline_config_each_program_update(
        &switchd_ctx->p4_devices[dev_id],
        device_profile,
        switchd_ctx->args.install_dir,
        absolute_paths);
  }

#ifndef STATIC_LINK_LIB

  /* Reload P4 specific libraries with the device */
  if (switchd_ctx->asic[dev_id].configured) {
    int ret = 0;
    /* Load P4 PD libraries */
    ret = bf_switchd_p4_pd_lib_load(dev_id);
    if (ret != 0) {
      return BF_OBJECT_NOT_FOUND;
    }
    /* Load Diag */
    ret = bf_switchd_diag_lib_load(dev_id);
    if (ret != 0) {
      return ret;
    }
    /* Load the mav diag libs */
    ret = bf_switchd_accton_diag_lib_load(dev_id);
    if (ret != 0) {
      return ret;
    }
    /* Load the switch libs */
    ret = bf_switchd_p4_switch_lib_load(dev_id);
    if (ret != 0) {
      return ret;
    }
  }
#endif  // STATIC_LINK_LIB

  bf_status_t status = BF_SUCCESS;
  /* Add the device */
  status = bf_switchd_device_add(dev_id, false);
  if (status != BF_SUCCESS) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD,
        BF_LOG_ERR,
        "ERROR: bf_switchd_device_add failed(%d) for dev_id %d",
        status,
        dev_id);
    return status;
  }

  /* Add the ports for the device */
  bf_switchd_ports_add(dev_id);

  /* Initialize P4 specific libraries for the device */
  if (switchd_ctx->asic[dev_id].configured) {
    bf_switchd_p4_lib_init(dev_id);
#ifndef STATIC_LINK_LIB
    bf_switchd_accton_diag_lib_init(dev_id);
#endif  // STATIC_LINK_LIB
  }

  return status;
}

static bf_status_t bf_switchd_warm_init_port_bring_up(bf_dev_id_t dev_id) {
  unsigned int pipe, port;
  bf_dev_port_t dev_port;
  bool has_mac;
  bool link_down;
  uint32_t num_pipes;
  bf_status_t status;
  bf_pal_front_port_handle_t port_hdl;

  /* Figure out how many pipelines present on this device */
  lld_sku_get_num_active_pipes(dev_id, &num_pipes);

  bf_pal_pltfm_reg_interface_t bf_pm_interface = {0};
  bf_pal_pltfm_all_interface_get(&bf_pm_interface);

  for (pipe = 0; pipe < num_pipes; pipe++) {
    for (port = 0; port < BF_PIPE_PORT_COUNT; port += 1) {
      has_mac = false;
      dev_port = MAKE_DEV_PORT(pipe, port);
      /* If port is associated with a MAC then it needs the pm FSM to bring it
       * up.
       * Other ports, like PCIe port or recirc, don't. */
      status = bf_port_has_mac(dev_id, dev_port, &has_mac);
      if (status != BF_SUCCESS) {
        has_mac = false;
      }

      // skip ports which are not added
      if ((bf_port_is_valid(dev_id, dev_port) != BF_SUCCESS) || (!has_mac)) {
        continue;
      }

      // Note: For internal ports which are in mac-loopback,
      // link states are not monitored. Link state read would return down.
      // So skip doing fsm-init to avoid traffic disruption.
      lld_err_t ret;
      bool is_internal = false;
      ret = lld_sku_is_dev_port_internal(dev_id, dev_port, &is_internal);
      if (ret != LLD_OK) {
        bf_sys_log_and_trace(
            BF_MOD_SWITCHD,
            BF_LOG_ERR,
            "Unable to determine if port is internal for dev : %d : dev port : "
            "%d : lld_err : %d",
            dev_id,
            dev_port,
            ret);
        continue;  // should never happen
      }

      //  Handle FSM states:
      //
      //  a) port-enabled -
      //     i) Link-Down : Move FSM to init state
      //     ii) Link-UP   : Move FSM to Link-down monitoring state
      //
      //  b) port-disabled: Skip FSM handling. Below API will return
      //  OBJ_NOT_FOUND
      //
      status = bf_device_warm_init_port_bring_up(dev_id, dev_port, &link_down);
      if (status == BF_OBJECT_NOT_FOUND) {
        continue;
      }
      status = bf_pm_port_dev_port_to_front_panel_port_get(
          dev_id, dev_port, &port_hdl);
      if (status != BF_SUCCESS) {
        bf_sys_log_and_trace(
            BF_MOD_SWITCHD,
            BF_LOG_ERR,
            "Unable to get the front port handle for dev : %d : dev_port : "
            "%d : %s (%d)",
            dev_id,
            dev_port,
            bf_err_str(status),
            status);
        continue;
      }

      if (link_down) {
        status = bf_pm_port_fsm_init_in_down_state(dev_id, &port_hdl);
        if (status != BF_SUCCESS) {
          bf_sys_log_and_trace(
              BF_MOD_SWITCHD,
              BF_LOG_ERR,
              "Unable to start FSM during warm init end for dev : %d front "
              "panel port : %d/%d (%d) : %s (%d)",
              dev_id,
              port_hdl.conn_id,
              port_hdl.chnl_id,
              dev_port,
              bf_err_str(status),
              status);
          // continue with next port
        }
      } else {
        status = bf_pm_port_fsm_init_in_up_state(dev_id, &port_hdl);
        if (status != BF_SUCCESS) {
          bf_sys_log_and_trace(
              BF_MOD_SWITCHD,
              BF_LOG_ERR,
              "Unable to start FSM during warm init end for dev : %d front "
              "panel port : %d/%d (%d) : %s (%d)",
              dev_id,
              port_hdl.conn_id,
              port_hdl.chnl_id,
              dev_port,
              bf_err_str(status),
              status);
        }
      }

      if ((!is_internal) && (bf_pm_interface.pltfm_ha_link_state_notify)) {
        status = bf_pm_interface.pltfm_ha_link_state_notify(
            dev_id, &port_hdl, !link_down);
        if (status != BF_SUCCESS) {
          bf_sys_log_and_trace(
              BF_MOD_SWITCHD,
              BF_LOG_ERR,
              "Platform return error for link-state notification during warm "
              "init end for dev : %d front "
              "panel port : %d/%d (%d) : %s (%d)",
              dev_id,
              port_hdl.conn_id,
              port_hdl.chnl_id,
              dev_port,
              bf_err_str(status),
              status);
        }
      }
    }
  }
  return BF_SUCCESS;
}

bf_status_t bf_switchd_warm_init_end(bf_dev_id_t dev_id) {
  bf_status_t status = BF_SUCCESS;

  if (!bf_dev_id_validate(dev_id)) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD, BF_LOG_ERR, "Invalid device id: %d", dev_id);

    return BF_INVALID_ARG;
  }

  if (switchd_ctx->state[dev_id].device_locked) {
    status = bf_device_warm_init_end(dev_id);
    if (status != BF_SUCCESS) {
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "Error encountered: Force cold-reset for dev_id %d",
                           dev_id);
      return status;
    }

    if (!switchd_ctx->asic[dev_id].is_virtual &&
        !switchd_ctx->asic[dev_id].is_sw_model) {
      status = bf_switchd_warm_init_port_bring_up(dev_id);
      if (status != BF_SUCCESS) {
        bf_sys_log_and_trace(
            BF_MOD_SWITCHD,
            BF_LOG_ERR,
            "Error encountered while bringing up ports in warm init end: Force "
            "cold-reset for dev_id %d",
            dev_id);
        return status;
      }
    }
    switchd_ctx->state[dev_id].device_locked = false;
  }

  /* Warm init done.  Reset init_mode to INIT_COLD
   */
  if (switchd_ctx->args.init_mode == BF_DEV_WARM_INIT_FAST_RECFG ||
      switchd_ctx->args.init_mode == BF_DEV_WARM_INIT_FAST_RECFG_QUICK ||
      switchd_ctx->args.init_mode == BF_DEV_WARM_INIT_HITLESS) {
    switchd_ctx->args.init_mode = BF_DEV_INIT_COLD;
  }

  return status;
}

#ifdef FASTRECFG_TEST
static bf_status_t bf_switchd_warm_init_begin_test(
    bf_dev_id_t dev_id,
    p4_devport_mgr_warm_init_mode_e warm_init_mode,
    p4_devport_mgr_serdes_upgrade_mode_e serdes_upgrade_mode,
    bool upgrade_agents) {
  bf_status_t sts;

  sts = bf_switchd_warm_init_begin(
      dev_id,
      (bf_dev_init_mode_t)warm_init_mode,
      (bf_dev_serdes_upgrade_mode_t)serdes_upgrade_mode,
      upgrade_agents);
  if (sts != BF_SUCCESS) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "Dev %d warm init begin failed %d",
                         dev_id,
                         sts);
    return sts;
  }

  if ((bf_dev_init_mode_t)warm_init_mode != BF_DEV_WARM_INIT_FAST_RECFG_QUICK) {
    sts = bf_switchd_device_add_with_p4(dev_id, NULL);
    if (sts != BF_SUCCESS) {
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "Dev %d bf_switchd_device_add_with_p4 failed %d",
                           dev_id,
                           sts);
    }
  }

  return sts;
}
#endif /* FAST_RECFG_TEST */

/**
 * Initialize the driver CLI shell and the switchd uCLI node. This does not
 * actually start the shell; call bf_switchd_drv_shell_start() afterwards
 * to begin processing interactive input.
 */
static bf_status_t bf_switchd_drv_shell_init() {
  /* Initialize uCLI and the driver shell */
  bf_status_t ret = bf_drv_shell_init();
  if (ret != BF_SUCCESS) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "ERROR: bf-driver shell initialization failed : %d",
                         ret);
    return ret;
  }

  /* Register switchd uCLI node */
  switchd_register_ucli_node();

  return ret;
}

/**
 * Function that creates copies of the p4 program names and stores
 * them in a char* array. The caller of this function is responsible
 * for freeing the memory associated with the strings and the array.
 * The array of char* is itself null-ptr terminated.
 */
static char **bf_switchd_cpy_p4_names(p4_devices_t *p4_devices) {
  int num_cfg = 0;
  for (bf_dev_id_t dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    num_cfg += p4_devices[dev_id].num_p4_programs;
  }

  /* Add one additional for a NULL termination and one more in case switchapi is
   * required. */
  num_cfg += 2;

  char **p4_names = bf_sys_calloc(num_cfg, sizeof(char *));
  if (!p4_names) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "bf_switchd: Failed to allocate memory for p4_names");
    return NULL;
  }

  int cur_p4 = 0;
  bool switchapi_added = false;
  for (bf_dev_id_t dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    for (int p4 = 0; p4 < p4_devices[dev_id].num_p4_programs; ++p4) {
      p4_names[cur_p4++] =
          bf_sys_strdup(p4_devices[dev_id].p4_programs[p4].program_name);
      // We add a value for switchapi only if its library has been loaded
      /* We must be careful when free-ing this array, as this value is not
         stored in the heap. */
      if (!switchapi_added &&
          strcmp(p4_devices[dev_id].p4_programs[p4].program_name, "switch") ==
              0) {
        p4_names[cur_p4++] = "bf_switchapi~";
        switchapi_added = true;
      }
    }
  }
  return p4_names;
}

/**
 * Function to start the driver CLI shell. You must call
 * bf_switchd_drv_shell_init() before calling this function.
 */
static bf_status_t bf_switchd_drv_shell_start(char *install_dir,
                                              bool run_background,
                                              bool run_ucli,
                                              bool local_only,
                                              p4_devices_t *p4_devices) {
  bf_status_t ret = BF_SUCCESS;

  /* Note that the CLI thread is responsible for free-ing this data */
  char **p4_names = bf_switchd_cpy_p4_names(p4_devices);
  char *install_dir_path = calloc(1024, sizeof(char));
  if (!install_dir_path) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD,
        BF_LOG_ERR,
        "bf_switchd: Failed to allocate memory for install_dir_path");
    return BF_INVALID_ARG;
  }

  /* Start the CLI server thread */
  bf_sys_log_and_trace(
      BF_MOD_SWITCHD, BF_LOG_DBG, "bf_switchd: spawning cli server thread");
  snprintf(install_dir_path, 1023, "%s/", install_dir);
  cli_thread_main(install_dir_path, p4_names, local_only);

  if (!run_background) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD, BF_LOG_DBG, "bf_switchd: spawning driver shell");
    /* Start driver shell */
    if (run_ucli) {
      ret = bf_drv_shell_start();
      if (ret != BF_SUCCESS) {
        bf_sys_log_and_trace(BF_MOD_SWITCHD,
                             BF_LOG_ERR,
                             "ERROR: uCLI instantiation failed : %d",
                             ret);
        return ret;
      }
    } else {
      cli_run_bfshell();
    }
  } else {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD,
        BF_LOG_DBG,
        "bf_switchd: running in background; driver shell is disabled");
  }
  bf_sys_log_and_trace(BF_MOD_SWITCHD,
                       BF_LOG_DBG,
                       "bf_switchd: server started - listening on port 9999");

  return ret;
}

#ifdef DEVICE_IS_EMULATOR

bool bf_switchd_reg_is_not_simulated_hw(bf_dev_id_t dev_id, uint32_t addr) {
  if (switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO) {
    if ((addr >= tofino_serdes_address) &&
        (addr < (tofino_serdes_address + tofino_serdes_array_element_size))) {
      return true;
    }
    if (addr == offsetof(Tofino, device_select.misc_regs.sbm_ind_rslt))
      return true;
    if (addr == offsetof(Tofino, device_select.misc_regs.sbm_ind_rdata))
      return true;
    if (addr == offsetof(Tofino, device_select.misc_regs.sbm_ind_wdata))
      return true;
    if (addr == offsetof(Tofino, device_select.misc_regs.sbm_ind_ctrl))
      return true;
  } else if (switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO2) {
    if ((addr >= 0x3000000) && (addr < 0x4000000)) {
      return true;
    } else if (addr >= offsetof(tof2_reg, eth100g_regs) &&
               addr < offsetof(tof2_reg, eth400g_p1)) { /* eth100g */
      return true;
    } else if (addr >= offsetof(tof2_reg, ethgpiobr) &&
               addr < offsetof(tof2_reg, eth100g_regs_rot)) { /* GPIOs */
      return true;
    } else if (addr >= offsetof(tof2_reg, eth100g_regs_rot) &&
               addr < offsetof(tof2_reg, pipes)) { /* eth100g_rot and serdes */
      return true;
      /* Hole */
    } else if (addr > offsetof(tof2_reg,
                               device_select.tm_top.tm_psc_top.psc_common
                                   .underflow_err_log) &&
               addr < offsetof(tof2_reg, eth100g_regs)) {
      return true;
    }
  } else if (switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO3) {
    if ((addr >= 0x3000000) && (addr < 0x4000000)) {
      return true;
    } else if (addr > offsetof(tof3_reg,
                               device_select.tm_top.tm_ddr_top.tm_fab_shim
                                   .intr_afifo_overflow.freeze_enable) &&
               addr < offsetof(tof3_reg, eth400g[0])) {
      /* Hole */
      return true;
#if defined(EMU_SKIP_BLOCKS_OPT)
    } else if (addr >= offsetof(tof3_reg,
                                pipes[0].pardereg.pgstnreg.ipbprsr4reg[0]) &&
               (addr < offsetof(tof3_reg,
                                pipes[0].pardereg.pgstnreg.ipbprsr4reg[1]))) {
      /* Hole */
      return true;
    } else if (addr >= offsetof(tof3_reg,
                                pipes[1].pardereg.pgstnreg.ipbprsr4reg[0]) &&
               (addr < offsetof(tof3_reg,
                                pipes[1].pardereg.pgstnreg.ipbprsr4reg[1]))) {
      /* Hole */
      return true;
    } else if (addr >= offsetof(tof3_reg,
                                pipes[2].pardereg.pgstnreg.ipbprsr4reg[0]) &&
               (addr < offsetof(tof3_reg,
                                pipes[2].pardereg.pgstnreg.ipbprsr4reg[1]))) {
      /* Hole */
      return true;
    } else if (addr >= offsetof(tof3_reg,
                                pipes[3].pardereg.pgstnreg.ipbprsr4reg[0]) &&
               (addr < offsetof(tof3_reg,
                                pipes[3].pardereg.pgstnreg.ipbprsr4reg[1]))) {
      /* Hole */
      return true;
    } else if (addr >= offsetof(tof3_reg,
                                pipes[0].pardereg.pgstnreg.epbprsr4reg[0]) &&
               (addr < offsetof(tof3_reg,
                                pipes[0].pardereg.pgstnreg.epbprsr4reg[1]))) {
      /* Hole */
      return true;
    } else if (addr >= offsetof(tof3_reg,
                                pipes[1].pardereg.pgstnreg.epbprsr4reg[0]) &&
               (addr < offsetof(tof3_reg,
                                pipes[1].pardereg.pgstnreg.epbprsr4reg[1]))) {
      /* Hole */
      return true;
    } else if (addr >= offsetof(tof3_reg,
                                pipes[2].pardereg.pgstnreg.epbprsr4reg[0]) &&
               (addr < offsetof(tof3_reg,
                                pipes[2].pardereg.pgstnreg.epbprsr4reg[1]))) {
      /* Hole */
      return true;
    } else if (addr >= offsetof(tof3_reg,
                                pipes[3].pardereg.pgstnreg.epbprsr4reg[0]) &&
               (addr < offsetof(tof3_reg,
                                pipes[3].pardereg.pgstnreg.epbprsr4reg[1]))) {
      /* Hole */
      return true;
#endif
    }
  }
  return false;
}
#endif  // DEVICE_IS_EMULATOR

bool bf_switchd_reg_is_tof2_serdes_addr(bf_dev_id_t dev_id, uint32_t addr) {
  if (switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO) {
    return false;
  } else if (switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO2) {
    if ((addr >= 0x3000000) && (addr < 0x4000000)) {
      return true;
    }
  }
  return false;
}

void fpga_read_register(bf_dev_id_t dev_id,
                        uint32_t bfn_addr,
                        uint32_t *reg_val);
void fpga_write_register(bf_dev_id_t dev_id,
                         uint32_t bfn_addr,
                         uint32_t reg_val);

/* Function to peform PIO write access to the device */
static bf_status_t bf_switchd_reg_wr_fn(bf_dev_id_t dev_id,
                                        bf_subdev_id_t subdev_id,
                                        uint32_t addr,
                                        uint32_t data) {
  int ret;
  switchd_pcie_map_t *pcie_map;

#ifdef REG_ACCESS_LOG
  bf_switchd_log_access(BF_SWITCHD_LOG_WRITE,
                        dev_id,
                        switchd_ctx->asic[dev_id].chip_family,
                        addr,
                        data);
#endif  // REG_ACCESS_LOG

#ifdef DEVICE_IS_EMULATOR
  if (bf_switchd_reg_is_not_simulated_hw(dev_id, addr)) {
    return BF_SUCCESS;
  }
#endif

  if (switchd_ctx->asic[dev_id].is_sw_model) {
    bool is_tof3 =
        switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO3;
    uint32_t pkg_size = 1;
    if (is_tof3) {
      // If both subdev_0 and sub_dev_1 are set then package size is 2
      // otherwise its 1.
      // TODO assuming two subdevices only [0-1] revisist this later for
      // more flexibility.
      if ((switchd_ctx->asic[dev_id].subdev_pres_msk & 0x3) == 0x3) {
        pkg_size = 2;
      }
    }

    /* Perform dru_sim access */
    switchd_ctx->dru_sim_cpu_to_pcie_wr_fn(
        is_tof3 ? (uint32_t)dev_id * pkg_size + subdev_id : (uint32_t)dev_id,
        addr,
        data);
    return BF_SUCCESS;
  }

  // ASIC case
  pcie_map = &(switchd_ctx->pcie_map[dev_id][subdev_id]);
  // If pltfm mgr has registered the i2c slave function, use that
  if (reg_dir_i2c_wr_func != NULL) {
    ret = reg_dir_i2c_wr_func(dev_id, subdev_id, addr, data);
    if (ret != 0) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD, BF_LOG_ERR, "Error in writing to reg via I2C");
    }
  } else {
    /* Perform PCIE access */
    if (!switchd_ctx->state[dev_id].device_pci_err) {
      data = htole32(data);
      *(volatile uint32_t *)(pcie_map->dev_base_addr + addr) = data;
    }
  }

  return BF_SUCCESS;
}

switchd_pcie_map_t *dev_0_pcie_map = NULL;

uintptr_t bf_switchd_get_dev_base(bf_dev_id_t dev_id) {
  if (dev_0_pcie_map == NULL) {  // one time init
    dev_0_pcie_map = &(switchd_ctx->pcie_map[dev_id][0]);
  }
  return (uintptr_t)dev_0_pcie_map->dev_base_addr;
}

/* Function to peform PIO read access to the device */
static bf_status_t bf_switchd_reg_rd_fn(bf_dev_id_t dev_id,
                                        bf_subdev_id_t subdev_id,
                                        uint32_t addr,
                                        uint32_t *data) {
  int ret;
  switchd_pcie_map_t *pcie_map;

#ifdef REG_ACCESS_LOG
  bf_switchd_log_access(BF_SWITCHD_LOG_READ_BEFORE,
                        dev_id,
                        switchd_ctx->asic[dev_id].chip_family,
                        addr,
                        0);
#endif  // REG_ACCESS_LOG

#ifdef DEVICE_IS_EMULATOR
  if (bf_switchd_reg_is_not_simulated_hw(dev_id, addr)) {
    *data = 0x0BAD0BAD;
    return BF_SUCCESS;
  }
#endif

  if (switchd_ctx->asic[dev_id].is_sw_model) {
    bool is_tof3 =
        switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO3;
    uint32_t pkg_size = 1;
    if (is_tof3) {
      // If both subdev_0 and sub_dev_1 are set then package size is 2
      // otherwise its 1.
      // TODO assuming two subdevices only [0-1] revisist this later for
      // more flexibility.
      if ((switchd_ctx->asic[dev_id].subdev_pres_msk & 0x3) == 0x3) {
        pkg_size = 2;
      }
    }

    /* Perform dru_sim access */
    *data = switchd_ctx->dru_sim_cpu_to_pcie_rd_fn(
        is_tof3 ? (uint32_t)dev_id * pkg_size + subdev_id : (uint32_t)dev_id,
        addr);
#ifdef REG_ACCESS_LOG
    bf_switchd_log_access(BF_SWITCHD_LOG_READ_AFTER,
                          dev_id,
                          switchd_ctx->asic[dev_id].chip_family,
                          addr,
                          *data);
#endif  // REG_ACCESS_LOG

    return BF_SUCCESS;
  }

  // ASIC case
  pcie_map = &(switchd_ctx->pcie_map[dev_id][subdev_id]);
  /* Perform PCIE access */
  // If the pltfm mgr has registered the i2c slave function, use that
  if (reg_dir_i2c_rd_func != NULL) {
    ret = reg_dir_i2c_rd_func(dev_id, subdev_id, addr, data);
    if (ret != 0) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD, BF_LOG_ERR, "Error in reading from reg via I2C");
    }
  } else {
    if (!switchd_ctx->state[dev_id].device_pci_err) {
      /* Perform PCIE access */
      *data = *(volatile uint32_t *)(pcie_map->dev_base_addr + addr);
      *data = le32toh(*data);
      /* This is the best effort to catch PCIE read timeout before AER
       * signal can trigger SIGIO action and user space application can
       * receive it */
      if (*data == 0xFFFFFFFFUL) {
        /* ensure that it is not pcie time out */
        uint32_t fixed_offset, temp;
        if (switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO) {
          fixed_offset = 0x40000UL;
        } else {
          fixed_offset = 0x80000UL;
        }
        temp = *(volatile uint32_t *)(pcie_map->dev_base_addr + fixed_offset);
        if (temp == 0xFFFFFFFFUL) {
#ifdef REG_ACCESS_LOG
          bf_switchd_log_dump_access_log_last_n(100);
#endif
          bf_sys_log_and_trace(BF_MOD_SWITCHD,
                               BF_LOG_ERR,
                               "Tofino PCIE access failure at offset 0x%08x...",
                               addr);
          printf("Tofino PCIE access failure at offset 0x%08x...\n", addr);
          fflush(stdout);
          exit(-1);
        }
      } else if (*data == 0x0ECC0ECC) {
        static bool log_tlp_signature = true; /* should it be configurable? */
        if (log_tlp_signature) {
          bf_sys_log_and_trace(
              BF_MOD_SWITCHD,
              BF_LOG_ERR,
              "Tofino PCIE access read 0x0ecc0ecc at offset 0x%08x...",
              addr);
        }
      }
    }
  }

#ifdef REG_ACCESS_LOG
  bf_switchd_log_access(BF_SWITCHD_LOG_READ_AFTER,
                        dev_id,
                        switchd_ctx->asic[dev_id].chip_family,
                        addr,
                        *data);
#endif  // REG_ACCESS_LOG

  return BF_SUCCESS;
}

/* Routine to process async updates from HW (DMA resp) */
static void *bf_switchd_process_async_dma_notifs(void *arg) {
  (void)arg;
  bf_dev_id_t dev_id = 0;
  switchd_state_t *dev_state;
  bf_mc_session_hdl_t mc_session_handle;

  /* Get a handle for multicast node garbage collection. */
  if (!switchd_ctx->args.skip_hld.mc_mgr) {
    bf_status_t sts = bf_mc_create_session(&mc_session_handle);
    bf_sys_assert(BF_SUCCESS == sts);
  }

  /* Device update processing loop
   *  a) Process DMA responses
   *  b) Reclaim freed up MC nodes
   */
  while (1) {
    /* The number of DRs processed per second decreases above this value.
     * There may be additional tuning required for different CPUs.
     * Around 100K DRs/sec observed with this interval on the ref platform
     */
    usleep(300);
    if (reg_dir_i2c_rd_func && reg_dir_i2c_wr_func) {
      continue;
    }

    for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
      dev_state = &(switchd_ctx->state[dev_id]);

      if (!switchd_ctx->asic[dev_id].configured) continue;

      bf_sys_mutex_lock(&switchd_ctx->asic[dev_id].switch_mutex);

      if (!dev_state->device_ready) {
        bf_sys_mutex_unlock(&switchd_ctx->asic[dev_id].switch_mutex);
        continue;
      }

      if (switchd_ctx->asic[dev_id].is_virtual) {
        bf_sys_mutex_unlock(&switchd_ctx->asic[dev_id].switch_mutex);
        continue;
      }

      if (dev_state->device_locked) {
        bf_sys_mutex_unlock(&switchd_ctx->asic[dev_id].switch_mutex);
        continue;
      }

      /* Perform per-device periodic processing tasks */
      if (!switchd_ctx->args.skip_hld.pipe_mgr) {
        bf_dma_service_pipe_learning(dev_id);
        bf_dma_service_pipe_idle_time(dev_id);
        bf_dma_service_pipe_stats(dev_id);
        bf_dma_service_pipe_read_block_completion(dev_id);
        bf_dma_service_pipe_write_block_completion(dev_id);
        bf_dma_service_pipe_ilist_completion(dev_id);
      }
      bf_dma_service_write_list_completion(dev_id);
      bf_dma_service_mac_stats(dev_id, 256);
      if (switchd_ctx->asic[dev_id].chip_family != BF_DEV_FAMILY_TOFINO) {
        bf_dma_service_read_block0_completion(dev_id);
        bf_dma_service_read_block1_completion(dev_id);
        bf_dma_service_write_list1_completion(dev_id);
      }
      bf_sys_mutex_unlock(&switchd_ctx->asic[dev_id].switch_mutex);
    }

    /* Perform multicast driver background node garbage collection */
    if (!switchd_ctx->args.skip_hld.mc_mgr) {
      bf_mc_do_node_garbage_collection(mc_session_handle);
    }
  }
  return NULL;
}

/* Routine to process async updates from HW (Interrupts)
 * Note that if and when interrupts are enabled for certain DRs,
 * the above routine has to stop processing those DRs and the relevant
 * handlers must be created in the interrupt handler callback.
 */
static void *bf_switchd_process_async_int_notifs(void *arg) {
  (void)arg;
  bf_dev_id_t dev_id = 0;
  fd_set dev_fd_set;
  switchd_state_t *dev_state;

  /* Device interrupt processing loop */
  while (1) {
    bf_switchd_check_for_interrupts_or_timeout(&dev_fd_set, 100);

    for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
      dev_state = &(switchd_ctx->state[dev_id]);

      if (!dev_state->device_ready) {
        continue;
      }

      if (switchd_ctx->asic[dev_id].is_virtual) {
        continue;
      }

      if (dev_state->device_locked) {
        continue;
      }

      bf_switchd_service_interrupts(dev_id, &dev_fd_set);
    }
  }
  return NULL;
}

void *bf_switchd_process_async_port_int_notifs(void *arg) {
  (void)arg;
  bf_dev_id_t dev_id = 0;
  switchd_state_t *dev_state;
  bf_status_t st;

  /* Port interrupt notification loop */
  while (1) {
    st = bf_switchd_check_for_port_interrupts();
    if (st != BF_SUCCESS) continue;

    for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
      dev_state = &(switchd_ctx->state[dev_id]);

      if (!dev_state->device_ready) {
        continue;
      }

      if (switchd_ctx->asic[dev_id].is_virtual) {
        continue;
      }

      if (dev_state->device_locked) {
        continue;
      }

      if (switchd_ctx->asic[dev_id].is_sw_model == false) {
        bf_port_handle_port_int_notif(dev_id);
      }
    }
  }
  return NULL;
}

/* Routine to process async updates from HW (Packets) */
static void *bf_switchd_process_async_pkt_notifs(void *arg) {
  (void)arg;
  bf_dev_id_t dev_id = 0;
  switchd_state_t *dev_state;

  /* Device packet processing loop */
  while (1) {
    usleep(300);

    for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
      dev_state = &(switchd_ctx->state[dev_id]);

      if (!dev_state->device_ready) {
        continue;
      }

      if (switchd_ctx->asic[dev_id].is_virtual) {
        continue;
      }

      bf_sys_mutex_lock(&switchd_ctx->asic[dev_id].pktmgr_mutex);
      if (dev_state->device_pktmgr_ready) {
        bf_dma_service_pkt(dev_id);
      }
      bf_sys_mutex_unlock(&switchd_ctx->asic[dev_id].pktmgr_mutex);
    }
  }
  return NULL;
}

/* Routine to process async updates from HW (port fsm) */
static void *bf_switchd_process_async_port_notifs(void *arg) {
  switchd_state_t *dev_state;
  (void)arg;

  while (1) {
    usleep(300);
    dev_state = &(switchd_ctx->state[0]);

    if (/*(!switchd_ctx->asic[0].is_sw_model) &&*/
        (!dev_state->device_locked && dev_state->device_ready)) {
      bf_pm_tasklet_scheduler();
    }
  }
  return NULL;
}

static void bf_switchd_default_device_type_set(void) {
  bf_dev_id_t dev_id;

  bf_sys_log_and_trace(BF_MOD_SWITCHD,
                       BF_LOG_DBG,
                       "bf_switch: Operational mode set to default: MODEL");

  /* Set the device type to MODEL for all devices */
  for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    if (switchd_ctx->asic[dev_id].configured == 0) {
      continue;
    }

    switchd_ctx->asic[dev_id].is_sw_model = true;
  }

  return;
}

int bf_switchd_device_type_update(bf_dev_id_t dev_id, bool is_sw_model) {
  /*
   * Update the global context flags if this is the first device. For
   * subsequent devices, check if mixed set of devices exist.
   */
  bf_sys_log_and_trace(BF_MOD_SWITCHD,
                       BF_LOG_DBG,
                       "Device %d: Operational mode set to %s",
                       dev_id,
                       is_sw_model ? "MODEL" : "ASIC");

  /* Update the type of device in per asic struct */
  switchd_ctx->asic[dev_id].is_sw_model = is_sw_model;

  return 0;
}

/* Attempt to use PAL to query if each configured device is a model or an asic.
 * If a mix of model and asic devices is detected an error is returned.
 * Return values:
 *   0: successly queried PAL or no devices are configured
 *   1: PAL was not registered
 *  -1: error
 */
static int bf_switchd_pal_device_type_get(void) {
  bf_dev_id_t dev_id;
  int ret;
  bool is_sw_model = true;
  /*
   * Check for PAL handler registration to get device type and
   * if registered, update the device type for all devices.
   */
  for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    if (switchd_ctx->asic[dev_id].configured == 0) {
      continue;
    }

    bf_status_t sts;

    sts = bf_pal_pltfm_type_get(dev_id, &is_sw_model);
    /* If not registered, return 1 as this is not error  */
    if (sts == BF_NOT_IMPLEMENTED) {
      return 1;
    }

    if (sts != BF_SUCCESS) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "ERROR: PAL handler to get device type failed for device %d",
          dev_id);
      return -1;
    }

    /* Update the device type. If mixed set of devices found, return error */
    ret = bf_switchd_device_type_update(dev_id, is_sw_model);
    if (ret != 0) {
      return -1;
    }
  }

  /* If there is a mix of device types (model and asic) return an error. */
  bool using_model = false, using_asic = false;
  for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    if (switchd_ctx->asic[dev_id].configured == 0 ||
        switchd_ctx->asic[dev_id].is_virtual) {
      continue;
    }
    if (switchd_ctx->asic[dev_id].is_sw_model)
      using_model = true;
    else
      using_asic = true;
  }
  if (using_model && using_asic) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD,
        BF_LOG_ERR,
        "ERROR: Mix of model and asic devices is not suported.");
    return -1;
  }

  return 0;
}

static int bf_switchd_pltfm_device_type_get(void) {
  bf_dev_id_t dev_id;
  bf_status_t sts;
  int ret = 0;
  bool is_sw_model;
#ifndef STATIC_LINK_LIB
  char *error;
  switchd_state_t *state;
  bf_pal_pltfm_type_get_fn pltfm_device_type_get_fn = NULL;
  int agent_idx = 0;
#endif  // STATIC_LINK_LIB

  for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    if (switchd_ctx->asic[dev_id].configured == 0) {
      continue;
    }

#ifdef STATIC_LINK_LIB
    if (switchd_ctx->bf_pltfm_device_type_get_fn == NULL) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "Platforms infra API to get device type is not registered");
      /* Not an error, return 0 */
      return 1;
    }

    sts = switchd_ctx->bf_pltfm_device_type_get_fn(dev_id, &is_sw_model);
    if (sts != BF_SUCCESS) {
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "ERROR: Getting device type failed in agent lib "
                           " for device %d",
                           dev_id);
      return -1;
    }
#else  // STATIC_LINK_LIB
    /* Check if device type get API exists in any of the agent libs */
    for (agent_idx = 0; agent_idx < BF_SWITCHD_MAX_AGENTS; agent_idx++) {
      state = &(switchd_ctx->state[dev_id]);
      if (!state) {
        return -1;
      }

      if (state->agent_lib_handle[agent_idx] == NULL) {
        continue;
      }

      dlerror();
      *(pvoid_dl_t *)(&pltfm_device_type_get_fn) =
          dlsym(state->agent_lib_handle[agent_idx], "bf_pltfm_device_type_get");
      if (((error = dlerror()) == NULL) && (pltfm_device_type_get_fn != NULL)) {
        break;
      }
    }

    /*
     * If the device type get API doesn't exist in any of the
     * agent libs, return 0 as this is not error.
     */
    if (pltfm_device_type_get_fn == NULL) {
      return 1;
    }

    /* Get the device type using device type get API in pltfm infra */
    sts = pltfm_device_type_get_fn(dev_id, &is_sw_model);
    if (sts != BF_SUCCESS) {
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "ERROR: Getting device type failed in agent lib "
                           " for device %d",
                           dev_id);
      return -1;
    }

#endif  // STATIC_LINK_LIB

    /* Update the device type. If mixed set of devices found, return error. */
    ret = bf_switchd_device_type_update(dev_id, is_sw_model);
    if (ret != 0) {
      return -1;
    }
  }

  return 0;
}

static int bf_switchd_device_type_get(void) {
  int ret = 0;

  /* Check the PAL handler registration to get device type */
  ret = bf_switchd_pal_device_type_get();
  if (ret < 0) {
    return -1;
  }

  /* Return if device types are identified in PAL handler */
  if (ret == 0) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD,
        BF_LOG_DBG,
        "Initialized the device types using PAL handler registration");
    return 0;
  }

  /* Check pltfm infra agent library to get device type */
  ret = bf_switchd_pltfm_device_type_get();
  if (ret < 0) {
    return -1;
  }

  /* Return if device types are indentified using pltfm infra agent lib */
  if (ret == 0) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD,
        BF_LOG_DBG,
        "Initialized the device types using platforms infra API");
    return 0;
  }

  /* Derives the Board map and sets the model/hardware context from json conf
   * file
   */
  int bd_conf_present = 0;
  ret = bf_hw_porting_handle_board_config_map(&bd_conf_present, switchd_ctx);
  if (ret != 0) {
    return -1;
  }
  if (bd_conf_present) {
    return 0;
  }

  /* If device types are not identified still, set it to model by default */
  bf_switchd_default_device_type_set();

  return 0;
}

/* Initialize bf-driver libraries */
static int bf_switchd_driver_init(bool kernel_pkt_proc) {
  int ret = 0;
  int is_master;

#if __sun
  is_master = 1;
#else
  is_master = !kernel_pkt_proc;
#endif

  /* Initialize the base driver */
  ret = bf_drv_init();
  if (ret != 0) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD, BF_LOG_ERR, "ERROR: bf_drv_init failed : %d", ret);
    return ret;
  }

  /* Initialize the Low Level Driver (LLD) */
  /* "master_mode" bf_lld_init is done by kernel packet processing module
   * if enabled. A command line option to switchd indicates that switchd must
   * only do "non-master-mode" bf_lld_init in that case.
   */
  ret =
      bf_lld_init(is_master, bf_switchd_reg_wr_fn, bf_switchd_reg_rd_fn);
  if (ret != 0) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD, BF_LOG_ERR, "ERROR: bf_lld_init failed : %d", ret);
    return ret;
  }

  /* Initialize the Port Mgmt Driver (port_mgr) */
  if (!switchd_ctx->args.skip_hld.port_mgr) {
    ret = bf_port_mgr_init();
    if (ret != 0) {
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "ERROR: bf_port_mgr_init failed : %d",
                           ret);
      return ret;
    }
    ret = bf_pm_init();
    if (ret != 0) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD, BF_LOG_ERR, "ERROR: bf_pm_init failed : %d", ret);
      return ret;
    }
#ifdef LINKDOWN_INTERRUPT_ENABLED
    ret = bf_port_mgr_intbh_init();
    if (ret != 0) {
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "ERROR: bf_port_mgr_intbh_init failed : %d",
                           ret);
      return ret;
    }
    /* Start a thread to handle link interrupt notification. */
    if ((ret = bf_sys_thread_create(&switchd_ctx->args.port_int_t_id,
                                    bf_switchd_process_async_port_int_notifs,
                                    NULL,
                                    0)) != 0) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "ERROR: thread creation failed for port int notification service: %d",
          ret);
      return ret;
    }
    bf_sys_thread_set_name(switchd_ctx->args.port_int_t_id, "bf_port_intrrpt");
#endif
  } else {
    bf_sys_log_and_trace(BF_MOD_SWITCHD, BF_LOG_DBG, "Skipped port-mgr init");
  }

  /* Initialize the P4 driver (PD) and pipe_mgr */
  if (!switchd_ctx->args.skip_hld.pipe_mgr) {
    ret = p4_pd_init();
    if (ret != 0) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD, BF_LOG_ERR, "ERROR: p4_pd_init failed : %d", ret);
      return ret;
    }
  } else {
    bf_sys_log_and_trace(BF_MOD_SWITCHD, BF_LOG_DBG, "Skipped pipe-mgr init");
  }

  /* Initialize the Multicast Mgmt Driver (mc_mgr) */
  if (!switchd_ctx->args.skip_hld.mc_mgr) {
    ret = bf_mc_init();
    if (ret != 0) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD, BF_LOG_ERR, "ERROR: bf_mc_init failed : %d", ret);
      return ret;
    }
  } else {
    bf_sys_log_and_trace(BF_MOD_SWITCHD, BF_LOG_DBG, "Skipped mc-mgr init");
  }

  /* Initialize the TM Mgmt Driver (traffic_mgr) */
  if (!switchd_ctx->args.skip_hld.traffic_mgr) {
    ret = bf_tm_init();
    if (ret != 0) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD, BF_LOG_ERR, "ERROR: bf_tm_init failed : %d", ret);
      return ret;
    }
  } else {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD, BF_LOG_DBG, "Skipped traffic-mgr init");
  }

  /* Initialize the PCIe Packet Driver (pkt_mgr) */
  if (!kernel_pkt_proc && !switchd_ctx->args.skip_hld.pkt_mgr) {
    ret = bf_pkt_init();
    if (ret != 0) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD, BF_LOG_ERR, "ERROR: bf_pkt_init failed : %d", ret);
      return ret;
    }
  } else {
    bf_sys_log_and_trace(BF_MOD_SWITCHD, BF_LOG_DBG, "Skipped pkt-mgr init");
  }

  /* Initialize the BF_RT module.  This is done regardless of the type of P4
   * program(s) (P4-14 vs P4-16) as BF-RT may be used for fixed components as
   * well as P4 based objects. */
  if (!switchd_ctx->args.skip_hld.pipe_mgr) {
#ifdef BFRT_ENABLED
    ret = bf_rt_module_init(switchd_ctx->args.skip_hld.pkt_mgr,
                            switchd_ctx->args.skip_hld.mc_mgr,
                            switchd_ctx->args.skip_hld.port_mgr,
                            switchd_ctx->args.skip_hld.traffic_mgr);
    if (ret != 0) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD, BF_LOG_ERR, "ERROR: bf_rt_init failed : %d", ret);
      return ret;
    }
#endif
#ifdef TDI_ENABLED
    ret = tdi_module_init(NULL);
    if (ret != 0) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD, BF_LOG_ERR, "ERROR: tdi_init failed : %d", ret);
      return ret;
    }
#endif
  }

  /* Start a thread to handle asynchronous DMA notifications.  Note this doesn't
   * include DMA notifications for packet I/O over DMA. */
  if (!switchd_ctx->args.skip_dma_thread) {
    pthread_attr_t dma_t_attr;
    pthread_attr_init(&dma_t_attr);
    if ((ret = pthread_create(&switchd_ctx->args.dma_t_id,
                              &dma_t_attr,
                              bf_switchd_process_async_dma_notifs,
                              NULL)) != 0) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "ERROR: thread creation failed for dma handling service: %d",
          ret);
      return ret;
    }
    pthread_setname_np(switchd_ctx->args.dma_t_id, "bf_dma");
  }

  /* Start a thread to handle interrupts from all devices. */
  pthread_attr_t int_t_attr;
  pthread_attr_init(&int_t_attr);
  bf_dev_id_t dev_id = 0;
  if (switchd_ctx->asic[dev_id].is_sw_model == false) {
    if ((ret = pthread_create(&switchd_ctx->args.int_t_id,
                              &int_t_attr,
                              bf_switchd_process_async_int_notifs,
                              NULL)) != 0) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "ERROR: thread creation failed for int handling service: %d",
          ret);
      return ret;
    }
    pthread_setname_np(switchd_ctx->args.int_t_id, "bf_interrupt");
  }

  /* Start a thread to handle asynchronous DMA notifications for packet I/O over
   * DMA.  This is not done if packet processing is done in the kernel.  If the
   * packet manager is skipped for debug purposes this thread is also not
   * required. */
  if (!kernel_pkt_proc && !switchd_ctx->args.skip_hld.pkt_mgr) {
    pthread_attr_t pkt_t_attr;
    pthread_attr_init(&pkt_t_attr);
    if ((ret = pthread_create(&switchd_ctx->args.pkt_t_id,
                              &pkt_t_attr,
                              bf_switchd_process_async_pkt_notifs,
                              NULL)) != 0) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "ERROR: thread creation failed for pkt handling service: %d",
          ret);
      return ret;
    }
    pthread_setname_np(switchd_ctx->args.pkt_t_id, "bf_packet");
  }

  /* Start a thread to handle the port FSM. */
  pthread_attr_t port_t_attr;
  pthread_attr_init(&port_t_attr);
  if ((ret = pthread_create(&switchd_ctx->args.port_fsm_t_id,
                            &port_t_attr,
                            bf_switchd_process_async_port_notifs,
                            NULL)) != 0) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD,
        BF_LOG_ERR,
        "ERROR: thread creation failed for port fsm service: %d",
        ret);
    return ret;
  }
  pthread_setname_np(switchd_ctx->args.port_fsm_t_id, "bf_port_fsm");

#ifdef FASTRECFG_TEST
  /* Register FAST_RECFG test functions that need thrift service.
   * Need to move to switchd's own thrift service
   */
  p4_devport_mgr_register_test_handlers(bf_switchd_warm_init_begin_test,
                                        bf_switchd_warm_init_end);
#endif /* FAST_RECFG_TEST */

/* This is a hack for now. Enabling this in static builds means
 * compiling and linking thrift lib statically
 */
#ifndef STATIC_LINK_LIB
#ifdef THRIFT_ENABLED
  start_bfn_pd_rpc_server(&switchd_ctx->rpc_server_cookie,
                          switchd_ctx->args.server_listen_local_only);
#endif
#endif

  return 0;
}

/* Process bf_switchd conf-file */
static int bf_switchd_load_switch_conf_file() {
  if (!switchd_ctx->args.conf_file) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "ERROR: invalid switchd configuration file");
    return -1;
  }

  bf_sys_log_and_trace(BF_MOD_SWITCHD,
                       BF_LOG_DBG,
                       "bf_switchd: loading conf_file %s...",
                       switchd_ctx->args.conf_file);

  return switch_dev_config_init(
      switchd_ctx->args.install_dir, switchd_ctx->args.conf_file, switchd_ctx);
}

void *bf_dma2virt_dbg(bf_dma_addr_t dma_addr) {
  /* Iterate through all the dma memory pools created to get the virtual
     address */
  int i;
  bf_dma_info_t *dma_info;
  bf_sys_dma_pool_handle_t hndl;
  void *virtaddr = NULL;
  bf_dev_id_t dev_id;
  bf_subdev_id_t subdev_id;

  for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    if ((switchd_ctx->asic[dev_id].configured == 0) ||
        (switchd_ctx->asic[dev_id].is_virtual)) {
      continue;
    }
    for (subdev_id = 0; subdev_id < BF_MAX_SUBDEV_COUNT; subdev_id++) {
      if (!switchd_ctx->pcie_map[dev_id][subdev_id].configured) {
        continue;
      }
      /* First check if the dma address belongs to the memory pool
       associated with the DR_Pool */
      dma_info = &(switchd_ctx->dma_info[dev_id][subdev_id]);
      hndl = dma_info->dma_dr_pool_handle;
      virtaddr = bf_mem_dma2virt(hndl, dma_addr);
      if (virtaddr != NULL) {
        /* found the mapping from the dma address to the virtual address */
        return virtaddr;
      }
      for (i = 0; i < BF_DMA_TYPE_MAX; i++) {
        hndl = dma_info->dma_buff_info[i].dma_buf_pool_handle;
        virtaddr = bf_mem_dma2virt(hndl, dma_addr);
        if (virtaddr != NULL) {
          /* found the mapping from the dma address to the virtual address */
          return virtaddr;
        }
      }
    }
  }

  /* Indicates that the mapping between the dma and the virtual address was
     not found. Hence return NULL */
  bf_sys_log_and_trace(BF_MOD_SWITCHD,
                       BF_LOG_ERR,
                       "ERROR: Invalid DMA bus address: %012llx",
                       (long long unsigned)dma_addr);
  return NULL;
}

static int bf_switchd_find_dru_sim_fn(void *dru_sim_handle) {
#ifdef STATIC_LINK_LIB
  (void)dru_sim_handle;
  switchd_ctx->dru_sim_cpu_to_pcie_wr_fn = dru_sim_cpu_to_pcie_wr;
  switchd_ctx->dru_sim_cpu_to_pcie_rd_fn = dru_sim_cpu_to_pcie_rd;

  return 0;
#else   // STATIC_LINK_LIB

  /* Get dru sim reg write function */
  bf_switchd_find_lib_fn("dru_sim_cpu_to_pcie_wr",
                         dru_sim_handle,
                         (pvoid_dl_t *)&switchd_ctx->dru_sim_cpu_to_pcie_wr_fn);
  if (switchd_ctx->dru_sim_cpu_to_pcie_wr_fn == NULL) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "ERROR: dru sim reg write function lookup failed");
    return -1;
  }

  /* Get dru sim reg read function */
  bf_switchd_find_lib_fn("dru_sim_cpu_to_pcie_rd",
                         dru_sim_handle,
                         (pvoid_dl_t *)&switchd_ctx->dru_sim_cpu_to_pcie_rd_fn);
  if (switchd_ctx->dru_sim_cpu_to_pcie_rd_fn == NULL) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "ERROR: dru sim reg read function lookup failed");
    return -1;
  }

  return 0;
#endif  // STATIC_LINK_LIB
}

/* Initialize DMA simulation service */
static int bf_switchd_dru_sim_init(void) {
  int ret = 0;
  void *dru_sim_handle = NULL;

#ifdef STATIC_LINK_LIB
  /* Set dru sim reg read/write functions */
  ret = bf_switchd_find_dru_sim_fn(dru_sim_handle);
  if (ret != 0) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD, BF_LOG_ERR, "ERROR: dru sim functions lookup failed");
  }

  /* Initialize the dru_sim library */
  ret = dru_sim_init(switchd_ctx->args.model_ip, switchd_ctx->args.tcp_port_base,
    bf_dma2virt_dbg);
  if (ret != 0) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD, BF_LOG_ERR, "ERROR: DRU sim initialization failed");
    return -1;
  }

  /* Start the simulated DMA servicing thread */
  static pthread_attr_t drusim_t_attr;
  pthread_attr_init(&drusim_t_attr);
  if ((ret = pthread_create(&switchd_ctx->args.drusim_t_id,
                            &drusim_t_attr,
                            dru_pcie_dma_service_thread_entry,
                            NULL)) != 0) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD,
        BF_LOG_ERR,
        "ERROR: thread creation failed for simulated DMA service: %d",
        ret);
    return ret;
  }
  pthread_setname_np(switchd_ctx->args.drusim_t_id, "bf_pcie_dma_svc");

  return 0;

#else   // STATIC_LINK_LIB
  int (*dru_sim_init_fn)(char *, int, dru_sim_dma2virt_dbg_callback_fn);
  void *(*dru_pcie_dma_service_thread_entry_fn)(void *);

  /* Load dru sim library */
  dru_sim_handle = bf_switchd_lib_load("libdru_sim.so");
  if (dru_sim_handle == NULL) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD, BF_LOG_ERR, "ERROR: Unable to load dru sim library");
    return -1;
  }

  /* Get the dru sim init function */
  bf_switchd_find_lib_fn(
      "dru_sim_init", dru_sim_handle, (pvoid_dl_t *)&dru_sim_init_fn);
  if (dru_sim_init_fn == NULL) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "ERROR: dru sim init function lookup failed");
    return -1;
  }

  /* Get the dru sim service thread function */
  bf_switchd_find_lib_fn("dru_pcie_dma_service_thread_entry",
                         dru_sim_handle,
                         (pvoid_dl_t *)&dru_pcie_dma_service_thread_entry_fn);
  if (dru_pcie_dma_service_thread_entry_fn == NULL) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD,
        BF_LOG_ERR,
        "ERROR: dru sim service thread function lookup failed");
    return -1;
  }

  /* Get dru sim reg read/write functions */
  ret = bf_switchd_find_dru_sim_fn(dru_sim_handle);
  if (ret != 0) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD, BF_LOG_ERR, "ERROR: dru sim functions lookup failed");
  }

  /* Initialize the dru_sim library */
  ret = dru_sim_init_fn(switchd_ctx->args.model_ip,
    switchd_ctx->args.tcp_port_base, bf_dma2virt_dbg);
  if (ret != 0) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD, BF_LOG_ERR, "ERROR: DRU sim initialization failed");
    return -1;
  }

  /* Start the simulated DMA servicing thread */
  static pthread_attr_t drusim_t_attr;
  pthread_attr_init(&drusim_t_attr);
  if ((ret = pthread_create(&switchd_ctx->args.drusim_t_id,
                            &drusim_t_attr,
                            dru_pcie_dma_service_thread_entry_fn,
                            NULL)) != 0) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD,
        BF_LOG_ERR,
        "ERROR: thread creation failed for simulated DMA service: %d",
        ret);
    return ret;
  }
  pthread_setname_np(switchd_ctx->args.drusim_t_id, "bf_pcie_dma_svc");

  return 0;
#endif  // STATIC_LINK_LIB
}

static void *bf_sys_timer_init_wrapper(void *arg) {
  (void)arg;
  bf_sys_timer_init();
  return NULL;
}
/* Initialize system services expected by drivers */
static int bf_switchd_sys_init(void) {
  struct stat buf;
  int ret = 0;
  int len;

  /* Initialize system timer service */
  static pthread_attr_t tmr_t_attr;
  pthread_attr_init(&tmr_t_attr);
  if ((ret = pthread_create(&switchd_ctx->args.tmr_t_id,
                            &tmr_t_attr,
                            bf_sys_timer_init_wrapper,
                            NULL)) != 0) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD,
        BF_LOG_ERR,
        "ERROR: thread creation failed for system timer service: %d",
        ret);
    return ret;
  }
  pthread_setname_np(switchd_ctx->args.tmr_t_id, "bf_timer_src");

  /* Note that although we are using BF_SWITCHD_MAX_FILE_NAME (512) here, later
   * on in the init sequence a few config/log files length used are being
   * limited by the PIPE_MGR_CFG_FILE_LEN (250). */
  char bf_sys_log_cfg_file[BF_SWITCHD_MAX_FILE_NAME] = {0};
  if (stat("./zlog-cfg", &buf) == 0) {
    len = snprintf(bf_sys_log_cfg_file, sizeof(bf_sys_log_cfg_file),
		"./zlog-cfg");
  } else {
    len = snprintf(bf_sys_log_cfg_file,
                 sizeof(bf_sys_log_cfg_file),
                 "%s/share/bf_switchd/zlog-cfg",
                 switchd_ctx->args.install_dir);
  }

  bf_sys_log_and_trace(BF_MOD_SWITCHD,
                       BF_LOG_DBG,
                       "bf_switchd: Install dir: %s",
                       switchd_ctx->args.install_dir);

  if (len < 0 || len >= BF_SWITCHD_MAX_FILE_NAME) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD,
        BF_LOG_ERR,
        "ERROR: Problem with resolving path for zlog-cfg file."
        "       Install dir name too long: %zu, should be %zu at most.",
        strlen(switchd_ctx->args.install_dir),
        BF_SWITCHD_MAX_FILE_NAME - strlen("/share/bf_switchd/zlog-cfg") - 1);
    return -1;
  }

  /* Initialize the logging service */
  if ((ret = bf_sys_log_init(
           bf_sys_log_cfg_file, (void *)BF_LOG_DBG, (void *)(32 * 1024))) !=
      0) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "ERROR: failed to initialize logging service!");
    return ret;
  }

/* Don't override logging levels if flag is set. */
#if 0
#ifndef DRV_DEV_LOGS
  /* For performance set defaut log level to error for pipe and pkt modules */
  bf_sys_log_level_set(BF_MOD_PIPE, BF_LOG_DEST_FILE, BF_LOG_ERR);
  bf_sys_trace_level_set(BF_MOD_PIPE, BF_LOG_ERR);

  bf_sys_log_level_set(BF_MOD_PKT, BF_LOG_DEST_FILE, BF_LOG_ERR);
  bf_sys_trace_level_set(BF_MOD_PKT, BF_LOG_ERR);

#ifdef BFRT_ENABLED
  bf_sys_log_level_set(BF_MOD_BFRT, BF_LOG_DEST_FILE, BF_LOG_ERR);
  bf_sys_trace_level_set(BF_MOD_BFRT, BF_LOG_ERR);
#endif
#endif
#endif

  return 0;
}

void bf_switchd_exit_sighandler(int signum) {
  bf_dev_id_t dev_id;
  bf_subdev_id_t subdev_id;

  if (signum == SIGKILL || signum == SIGTERM || signum == SIGQUIT ||
      signum == SIGIO) {
    for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
      switchd_ctx->state[dev_id].device_pktmgr_ready = false;
      switchd_ctx->state[dev_id].device_locked = true;
      for (subdev_id = 0; subdev_id < BF_MAX_SUBDEV_COUNT; subdev_id++) {
        bf_switchd_release_dma_mem(dev_id, subdev_id);
      }
      bf_unmap_dev(dev_id);
    }
    /* reset bf_syslib to iommu-diabled state */
    bf_sys_dma_map_fn_register(NULL, NULL);
  }
}

static void pci_err_handler(int signum, siginfo_t *siginfo, void *arg) {
  bf_dev_id_t dev_id;

  bf_sys_log_and_trace(BF_MOD_SWITCHD,
                       BF_LOG_CRIT,
                       "bf_switchd:received SIGIO %d sicode %x si_band %lx",
                       signum,
                       siginfo->si_code,
                       siginfo->si_band);
  (void)arg;

#ifdef __sun
  int from_kernel = SI_FROMKERNEL(siginfo);
#else
  int from_kernel = (siginfo->si_code == SI_KERNEL);
#endif

  if (from_kernel && siginfo->si_band == 0) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_CRIT,
                         "bf_switchd:received pci error signal %d",
                         signum);
    for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
      switchd_ctx->state[dev_id].device_pci_err = true;
      switchd_ctx->state[dev_id].device_ready = false;
    }
#ifdef REG_ACCESS_LOG
    bf_switchd_log_dump_access_log_last_n(100);
#endif
#ifdef COVERAGE_ENABLED
    extern void __gcov_flush(void);
    /* coverage signal handler to allow flush of coverage data*/
    __gcov_flush(); /* dump coverage data on receiving SIGUSR1 */
#endif
    bf_switchd_exit_sighandler(signum);
    exit(-1);
  }
}

static void *bf_switchd_device_status_srv(void *arg) {
  bf_switchd_context_t *ctx = (bf_switchd_context_t *)arg;
  int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in switchd_addr;
  memset(&switchd_addr, 0, sizeof switchd_addr);
  switchd_addr.sin_family = AF_INET;
  switchd_addr.sin_addr.s_addr = ctx->server_listen_local_only
                                     ? htonl(INADDR_LOOPBACK)
                                     : htonl(INADDR_ANY);
  switchd_addr.sin_port = htons(ctx->dev_sts_port);
  int optval = 1;
  setsockopt(
      sock_fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(int));

  bind(sock_fd, (struct sockaddr *)&switchd_addr, sizeof(switchd_addr));
  listen(sock_fd, 1);
  while (1) {
    int fd = accept(sock_fd, (struct sockaddr *)NULL, NULL);
    char x[2] = {0};
    ssize_t rs = read(fd, x, 1);
    if (-1 == rs) {
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "Read failure for device status request: %s",
                           strerror(errno));
    }
    bf_dev_id_t dev_id = atoi(x);
    if (!bf_dev_id_validate(dev_id)) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD, BF_LOG_ERR, "Invalid device id: %d\n", dev_id);
      x[0] = '0';
    } else {
      bool init_done;
      bf_sys_mutex_lock(&switchd_ctx->init_done_mutex);
      init_done = switchd_ctx->init_done;
      bf_sys_mutex_unlock(&switchd_ctx->init_done_mutex);
      x[0] = init_done && switchd_ctx->state[dev_id].device_ready ? '1' : '0';
    }
    ssize_t ws = write(fd, x, 1);
    if (-1 == ws) {
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "Write failure for device status request: %s\n",
                           strerror(errno));
    }
    close(fd);
  }
  return NULL;
}

static void setup_pci_err_handler() {
  struct sigaction pci_err_action;

  /* setup signal hander for pci error */
  pci_err_action.sa_sigaction = pci_err_handler;
  sigemptyset(&pci_err_action.sa_mask);
  pci_err_action.sa_flags = SA_SIGINFO;
  sigaction(SIGIO, &pci_err_action, NULL);
}

#ifndef __sun
static bf_status_t bf_switchd_sysfs_cpuif_name_get(char *file_node,
                                                   char *cpuif_netdev_name,
                                                   size_t cpuif_name_size) {
  DIR *d;
  struct dirent *dir;
  char dir_name[sizeof(((switchd_pcie_cfg_t *)0)->pci_sysfs_str) + 16] = "";

  snprintf(dir_name, sizeof(dir_name), "%s/%s", file_node, "net");
  d = opendir(dir_name);
  if (d) {
    while ((dir = readdir(d)) != NULL) {
      if (!strcmp(dir->d_name, ".") || !strcmp(dir->d_name, "..")) {
        continue;
      } else {
        strncpy(cpuif_netdev_name, dir->d_name, cpuif_name_size);
        closedir(d);
        return BF_SUCCESS;
      }
    }
  }
  return BF_OBJECT_NOT_FOUND;
}
#endif // __sun

/* While running on asic the netdev name is derived from PCIe sysfs str
corresponing to the bf_kpkt driver. Otherwise if running as a model the
default interface used is veth127(assuming veth127 is mapped to pcie port)
except when switch.p4 used. In that case we use switch.conf to see if it
specifies cpu-eth veth mapping and use that port*/
bf_status_t bf_switchd_cpuif_netdev_name_get(bf_dev_id_t dev_id,
                                             char *cpuif_netdev_name,
                                             size_t cpuif_name_size) {
  p4_devices_t *p4_devices = &(switchd_ctx->p4_devices[dev_id]);

  if (!switchd_ctx->asic[dev_id].is_sw_model) {
#if __sun
	  printf("ILLUMOS: ignoring call to %s\n", __func__);
	  return BF_OBJECT_NOT_FOUND;
#else
    return bf_switchd_sysfs_cpuif_name_get(
        switchd_ctx->asic[dev_id].pcie_cfg.pci_sysfs_str,
        cpuif_netdev_name,
        cpuif_name_size);
#endif
  } else if (p4_devices) {
    uint32_t i = 0;
    for (i = 0; i < p4_devices->num_p4_programs; i++) {
      p4_programs_t *p4_programs = &(p4_devices->p4_programs[i]);
      if (strlen(p4_programs->switchapi) && p4_programs->use_eth_cpu_port) {
        memcpy(
            cpuif_netdev_name, p4_programs->eth_cpu_port_name, cpuif_name_size);
        return BF_SUCCESS;
      }
    }
  }

  memcpy(cpuif_netdev_name, "veth127", cpuif_name_size);

  return BF_SUCCESS;
}

/* This returns the 10G interface name on the host CPU
 * example 10g pci location pci_bus_dev = "0000:04:00" */
bf_status_t bf_switchd_cpuif_10g_netdev_name_get(bf_dev_id_t dev_id,
                                                 char *pci_bus_dev,
                                                 int instance,
                                                 char *cpuif_netdev_name,
                                                 size_t cpuif_name_size) {
#if __sun
	ILLUMOS_UNUSED(dev_id);
	ILLUMOS_UNUSED(pci_bus_dev);
	ILLUMOS_UNUSED(instance);
	ILLUMOS_UNUSED(cpuif_netdev_name);
	ILLUMOS_UNUSED(cpuif_name_size);
	  printf("ILLUMOS: ignoring call to %s\n", __func__);
	  return BF_OBJECT_NOT_FOUND;
#else
  char sysfs_path_10g[128];

  (void)dev_id;
  snprintf(
      sysfs_path_10g, sizeof(sysfs_path_10g), "%s.%1d", pci_bus_dev, instance);
  return bf_switchd_sysfs_cpuif_name_get(
      sysfs_path_10g, cpuif_netdev_name, cpuif_name_size);
#endif
}

bf_status_t bf_switchd_pltfm_type_get(bf_dev_id_t dev_id, bool *is_sw_model) {
  if (!bf_dev_id_validate(dev_id)) {
    return BF_INVALID_ARG;
  }

  *is_sw_model = switchd_ctx->asic[dev_id].is_sw_model;
  return BF_SUCCESS;
}

bf_status_t bf_switchd_reset_config(bf_dev_id_t dev_id) {
  if (!bf_dev_id_validate(dev_id)) return BF_INVALID_ARG;
  bf_device_profile_t dev_profile;
  memset(&dev_profile, 0, sizeof dev_profile);
  bf_status_t sts = bf_switchd_init_device_profile(dev_id, true, &dev_profile);
  if (sts != BF_SUCCESS) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD,
        BF_LOG_ERR,
        "ERROR: bf_switchd_init_device_profile failed(%d) for dev_id %d",
        sts,
        dev_id);
    return sts;
  }

  sts = bf_switchd_warm_init_begin(
      dev_id, BF_DEV_INIT_COLD, BF_DEV_SERDES_UPD_NONE, false);
  if (sts != BF_SUCCESS) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "Dev %d warm init begin failed %d",
                         dev_id,
                         sts);
    return sts;
  }
  sts = bf_switchd_device_add_with_p4(dev_id, &dev_profile);
  if (sts != BF_SUCCESS) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "Dev %d bf_switchd_device_add_with_p4 failed %d",
                         dev_id,
                         sts);
  }
  bf_switchd_warm_init_end(dev_id);
  return sts;
}

static bf_status_t bf_switchd_warm_init_error_set(bf_dev_id_t dev_id,
                                                  bool state) {
  if (!bf_dev_id_validate(dev_id)) {
    return BF_INVALID_ARG;
  }
  bf_warm_init_error_set(dev_id, state);
  return BF_SUCCESS;
}

static bf_status_t bf_switchd_warm_init_error_get(bf_dev_id_t dev_id,
                                                  bool *state) {
  if (!bf_dev_id_validate(dev_id)) {
    return BF_INVALID_ARG;
  }
  if (state == NULL) {
    return BF_INVALID_ARG;
  }
  *state = bf_warm_init_error_get(dev_id);
  return BF_SUCCESS;
}

static int bf_switchd_pal_handler_reg() {
  bf_pal_dev_callbacks_t dev_callbacks;
  dev_callbacks.warm_init_begin = bf_switchd_warm_init_begin;
  dev_callbacks.device_add = bf_switchd_device_add_with_p4;
  dev_callbacks.warm_init_end = bf_switchd_warm_init_end;
  dev_callbacks.cpuif_netdev_name_get = bf_switchd_cpuif_netdev_name_get;
  dev_callbacks.cpuif_10g_netdev_name_get =
      bf_switchd_cpuif_10g_netdev_name_get;
  dev_callbacks.pltfm_type_get = bf_switchd_pltfm_type_get;
  dev_callbacks.reset_config = bf_switchd_reset_config;
  dev_callbacks.warm_init_error_set = bf_switchd_warm_init_error_set;
  dev_callbacks.warm_init_error_get = bf_switchd_warm_init_error_get;
  bf_pal_device_callbacks_register(&dev_callbacks);
  return 0;
}

bool bf_switchd_is_kernel_pkt_proc(bf_dev_id_t dev_id) {
  (void)dev_id;
  return (switchd_ctx->args.kernel_pkt);
}

bf_status_t bf_switchd_kpkt_enable_traffic(bf_dev_id_t dev_id) {
  if (switchd_ctx->args.kernel_pkt) {
#if __sun
	(void)dev_id;
#else
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD,
        BF_LOG_DBG,
        "Perform warm reboot and device_add in kernel module pkt path");
    /* inform bf_kpkt kernel module to warm init the device and perform
     * pkt_mgr device_add subsequently. Expect CPU path packet loss during
     * the period when core_reset is issued (before invoking this) and until
     * the pkt_mgr device_add happens in the kernel */
    /* the driver sysfs file, "m_init", is written with the parameter '2' to
     * indicate that it is a warm initialization */
    if (bf_switchd_sysfs_set(switchd_ctx->asic[dev_id].pcie_cfg.pci_sysfs_str,
                             "m_init",
                             "2",
                             1)) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "ERROR: kernel packet module master initialization failed during "
          "warm_init_end");
      return -1;
    }
    if (bf_switchd_sysfs_set(switchd_ctx->asic[dev_id].pcie_cfg.pci_sysfs_str,
                             "dev_add",
                             "1",
                             1)) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "ERROR: kernel packet module device add failed during "
          "warm_init_end");
      return -1;
    }
#endif
  }

  return BF_SUCCESS;
}

static void bf_switchd_set_dflt_skip_options() {
  for (int i = 0; i < BF_MAX_DEV_COUNT; ++i) {
    if (!switchd_ctx->asic[i].configured) continue;
    switch (switchd_ctx->asic[i].chip_family) {
      case BF_DEV_FAMILY_TOFINO:
        /* Nothing skipped by default. */
        break;
      case BF_DEV_FAMILY_TOFINO2:
        /* Nothing skipped by default. */
        break;
      case BF_DEV_FAMILY_TOFINO3:
        /* Nothing skipped by default. */
        break;





      default:
        break;
    }
  }
}

bool bf_switchd_is_kernel_pkt_module_present() {
  char bf_sysfs_fname[128];
  FILE *fd;

  /* determine if kernel mode packet driver is loaded */
  /* configuration is not parsed at this point, so try for all
   * device famity types */
  switch_pci_sysfs_str_get(bf_sysfs_fname,
                           sizeof(bf_sysfs_fname) - sizeof("/dev_add"),
                           BF_DEV_FAMILY_TOFINO);
  strncat(bf_sysfs_fname,
          "/dev_add",
          sizeof(bf_sysfs_fname) - 1 - strlen(bf_sysfs_fname));
  fd = fopen(bf_sysfs_fname, "r");
  if (fd != NULL) {
    fclose(fd);
    return true;
  }

  /* try Tofino3 sysfs files */
  switch_pci_sysfs_str_get(bf_sysfs_fname,
                           sizeof(bf_sysfs_fname) - sizeof("/dev_add"),
                           BF_DEV_FAMILY_TOFINO3);
  strncat(bf_sysfs_fname,
          "/dev_add",
          sizeof(bf_sysfs_fname) - 1 - strlen(bf_sysfs_fname));
  fd = fopen(bf_sysfs_fname, "r");
  if (fd != NULL) {
    fclose(fd);
    return true;
  }
  return false;
}

/* bf_switchd main */
int bf_switchd_lib_init(bf_switchd_context_t *ctx) {
  int ret = 0;
  bf_dev_id_t dev_id = 0;
  bf_subdev_id_t subdev_id = 0;
  int agent_idx = 0;
  bf_status_t sts;
  int num_active_devices = 0;

  if (!ctx) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD, BF_LOG_ERR, "Null switchd context provided");
    return BF_INVALID_ARG;
  }

  if ((switchd_ctx = calloc(1, sizeof *switchd_ctx)) == NULL) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "Failed to allocate memory for switchd context");
    return BF_NO_SYS_RESOURCES;
  }
  switchd_ctx->args = *ctx;
  bf_sys_mutex_init(&switchd_ctx->init_done_mutex);

#ifndef __sun
  // On helios we have no support for user-space packet processing, so
  // we don't bother checking for the kernel module.
  switchd_ctx->args.kernel_pkt = bf_switchd_is_kernel_pkt_module_present();
#endif

  bf_switchd_init_dma_template(switchd_ctx->args.kernel_pkt);

  /* invalidate bf file handle until it is mmaped */
  for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    for (subdev_id = 0; subdev_id < BF_MAX_SUBDEV_COUNT; subdev_id++) {
      switchd_ctx->pcie_map[dev_id][subdev_id].dev_fd = -1;
    }
  }

  if (switchd_ctx->args.dev_sts_thread) {
    pthread_attr_t a;
    pthread_attr_init(&a);

    pthread_create(&switchd_ctx->args.dev_sts_t_id,
                   &a,
                   bf_switchd_device_status_srv,
                   switchd_ctx);

    pthread_attr_destroy(&a);
    pthread_setname_np(switchd_ctx->args.dev_sts_t_id, "bf_device_sts");
  }

  /* Always register the pltfm porting interfaces, the model or emulator is just
   * another platform.  Later if a pltfm agent is actually loaded, it will
   * overwrite functions. */
  bf_pal_pltfm_reg_interface_t bf_pm_interface = {0};
  bf_pm_interface.pltfm_safe_to_call_in_notify =
      &bf_model_safe_to_call_in_notify;
  bf_pm_interface.pltfm_front_panel_port_get_first =
      &bf_model_front_panel_port_get_first;
  bf_pm_interface.pltfm_front_panel_port_get_next =
      &bf_model_front_panel_port_get_next;
  bf_pm_interface.pltfm_mac_to_serdes_map_get = &bf_model_mac_to_serdes_map_get;
  bf_pm_interface.pltfm_serdes_info_get = &bf_model_serdes_info_get;
  bf_pm_interface.pltfm_port_media_type_get = &bf_model_media_type_get;
  bf_pm_interface.pltfm_port_str_to_hdl_get = &bf_model_port_str_to_hdl_get;
  bf_pm_interface.pltfm_init = &bf_model_init;
  bf_pm_interface.pltfm_mac_to_multi_serdes_map_get =
      &bf_model_mac_to_multi_serdes_map_get;
  bf_pm_interface.pltfm_pre_port_enable_cfg_set =
      &bf_model_pre_port_enable_cfg_set;
  sts = bf_pal_pltfm_all_interface_set(&bf_pm_interface);
  if (sts != BF_SUCCESS) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "Unable to register model pltfm interfaces for bf_pm");
  }

  if (switchd_ctx->args.kernel_pkt) {
    bf_drv_client_handle_t bf_kpkt_drv_hdl;

    // Register callbacks for bf_kppt for init and enable DR with the DVM
    sts = bf_drv_register("bf_kpkt", &bf_kpkt_drv_hdl);
    if (sts != BF_SUCCESS) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "Unable to register bf_kpkt module with DVM : %s (%d)",
          bf_err_str(sts),
          sts);
      return sts;
    }
    bf_drv_client_callbacks_t callbacks_kpkt = {0};
    callbacks_kpkt.enable_input_pkts = bf_switchd_kpkt_enable_traffic;
    sts = bf_drv_client_register_callbacks(
        bf_kpkt_drv_hdl, &callbacks_kpkt, BF_CLIENT_PRIO_4);
    if (sts != BF_SUCCESS) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "Unable to register bf_kpkt callbacks with DVM : %s (%d)",
          bf_err_str(sts),
          sts);
      return sts;
    }
  }

  /* Initialize system services */
  ret = bf_switchd_sys_init();
  if (ret != 0) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "System services initialization failed :%d",
                         ret);
    if (switchd_ctx) free(switchd_ctx);
    return ret;
  } else {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD, BF_LOG_DBG, "bf_switchd: system services initialized");
  }

  /* Load switchd configuration file */
  ret = bf_switchd_load_switch_conf_file();
  if (ret != 0) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD, BF_LOG_ERR, "Loading conf_file failed :%d", ret);
    if (switchd_ctx) free(switchd_ctx);
    return ret;
  }

  bf_switchd_set_dflt_skip_options();

  /* Load P4 specific libraries associated with each device */
  for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    if (!switchd_ctx->asic[dev_id].configured) continue;
    if (!switchd_ctx->asic[dev_id].is_virtual) {
      /* Increment the count of the number of active devices */
      num_active_devices++;
    }

    ret = bf_sys_mutex_init(&switchd_ctx->asic[dev_id].switch_mutex);
    if (ret != BF_SUCCESS) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "bf_sys_mutex_init failed (%d), switch_mutex, dev_id %d",
          ret,
          dev_id);
    }
    ret = bf_sys_mutex_init(&switchd_ctx->asic[dev_id].pktmgr_mutex);
    if (ret != BF_SUCCESS) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "bf_sys_mutex_init failed (%d), pktmgr_mutex, dev_id %d",
          ret,
          dev_id);
    }

#ifndef STATIC_LINK_LIB
    /* Load P4 PD libraries */
    ret = bf_switchd_p4_pd_lib_load(dev_id);
    if (ret != 0) {
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "bf_switchd_p4_pd_lib_load failed (%d), dev_id %d",
                           ret,
                           dev_id);
      if (switchd_ctx) free(switchd_ctx);
      return ret;
    }
    /* Load semantic libraries associated with switch.p4 (optional) */
    ret = bf_switchd_p4_switch_lib_load(dev_id);
    if (ret != 0) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "bf_switchd_p4_switch_lib_load failed (%d), dev_id %d",
          ret,
          dev_id);
      if (switchd_ctx) free(switchd_ctx);
      return ret;
    }
    /* Load Diag */
    ret = bf_switchd_diag_lib_load(dev_id);
    if (ret != 0) {
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "bf_switchd_diag_lib_load failed (%d), dev_id %d",
                           ret,
                           dev_id);
      if (switchd_ctx) free(switchd_ctx);
      return ret;
    }
    /* Load the agent libs */
    ret = bf_switchd_agent_lib_load(dev_id);
    if (ret != 0) {
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "bf_switchd_agent_lib_load failed (%d), dev_id %d",
                           ret,
                           dev_id);
      if (switchd_ctx) free(switchd_ctx);
      return ret;
    }
    /* Load Mav Diag */
    ret = bf_switchd_accton_diag_lib_load(dev_id);
    if (ret != 0) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "bf_switchd_accton_diag_lib_load failed (%d), dev_id %d",
          ret,
          dev_id);
      if (switchd_ctx) free(switchd_ctx);
      return ret;
    }
#endif  // STATIC_LINK_LIB
  }

  /* Initialize the driver shell. We need to do this even if we're running in
   * the background, because the shell is accessible via telnet using clish. */
  sts = bf_switchd_drv_shell_init();
  if (sts != 0) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "bf-driver shell initialization failed : %s",
                         bf_err_str(sts));
    if (switchd_ctx) free(switchd_ctx);
    return ret;
  }
  /* Initialize all agents. Init after shell start so that agents can
     register their clis
  */
  for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    if (!switchd_ctx->asic[dev_id].configured) continue;
    bf_switchd_agent_lib_init(dev_id);
    bf_switchd_agent_lib_wait_init_done(dev_id);
  }

  ret = bf_switchd_device_type_get();
  if (ret != 0) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "Error getting device type (model or asic) failed: %d",
                         ret);
    return ret;
  }

  /* determine number of subdevices in case of Tofino-3 based on the
   * the conf file
   */
  /* derive Asic pci sysfs string by going thru sysfs files in run time
   * instead of using the configured name in case of single device
   * program
   */
  for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    switchd_ctx->asic[dev_id].subdev_pres_msk = 0; /* init */
    if (switchd_ctx->asic[dev_id].configured &&
        switchd_ctx->asic[dev_id].chip_family == BF_DEV_FAMILY_TOFINO3) {
      for (subdev_id = 0; subdev_id < BF_MAX_SUBDEV_COUNT; subdev_id++) {
        if (switchd_ctx->pcie_map[dev_id][subdev_id].configured) {
          switchd_ctx->asic[dev_id].subdev_pres_msk |= (1 << subdev_id);
        }
      }
    }
  }

  bool single_device = false;
  for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    if (switchd_ctx->asic[dev_id].configured == 0 ||
        switchd_ctx->asic[dev_id].is_virtual ||
        switchd_ctx->asic[dev_id].is_sw_model) {
      continue;
    } else {
      if (!single_device) {
        single_device = true;
      } else {
        single_device = false;
        break;
      }
    }
  }
  if (single_device) {
#if __sun
      printf("ILLUMOS: ignoring attempt to get sysfs path\n");
#else
    int res;
    res = switch_pci_sysfs_str_get(
        switchd_ctx->asic[0].pcie_cfg.pci_sysfs_str,
        sizeof(switchd_ctx->asic[0].pcie_cfg.pci_sysfs_str),
        switchd_ctx->asic[0].chip_family);
    if (res) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD, BF_LOG_CRIT, "Error getting Asic sysfs filename");
      exit(-1);
    } else {
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_DBG,
                           "ASIC detected at PCI %s",
                           switchd_ctx->asic[0].pcie_cfg.pci_sysfs_str);
    }
#endif /* __sun */
  }

  /* get asic pci device id */
  for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    if (switchd_ctx->asic[dev_id].configured == 0 ||
        switchd_ctx->asic[dev_id].is_virtual ||
        switchd_ctx->asic[dev_id].is_sw_model) {
      continue;
    }
    bf_switchd_asic_set_pci_dev_id(&switchd_ctx->asic[dev_id]);
  }

#if defined(DEVICE_IS_EMULATOR)
  bf_model_num_devices_set(num_active_devices);
#endif

  dev_id = 0;
  for (bool model_done = false, asic_done = false;
       dev_id < BF_MAX_DEV_COUNT && !model_done && !asic_done;
       dev_id++) {
    if (switchd_ctx->asic[dev_id].configured == 0) continue;
    if (switchd_ctx->asic[dev_id].is_virtual) continue;
    if (!model_done && switchd_ctx->asic[dev_id].is_sw_model) {
      model_done = true;
      /* Register the number of devices with the model pltfm porting module */
      sts = bf_model_num_devices_set(num_active_devices);
      if (sts != BF_SUCCESS) {
        bf_sys_log_and_trace(
            BF_MOD_SWITCHD,
            BF_LOG_ERR,
            "Failed to set the num of devices with the model pltfm "
            "porting module");
      }
    }
    if (!asic_done && !switchd_ctx->asic[dev_id].is_sw_model) {
      asic_done = true;
      /* Setup pci error handler */
      setup_pci_err_handler();
    }
  }

  /* sleep for 3 seconds to allow other slow i2c operations to be over if
   * ASIC is accessed in i2c-only mode
   */
  if (reg_dir_i2c_rd_func && reg_dir_i2c_wr_func) {
    bf_sys_sleep(3);
  }

#if __sun
    printf("ILLUMOS: skipping sysfs write to cold reset the kpkt device\n");
#else
  if (switchd_ctx->args.kernel_pkt &&
      switchd_ctx->args.init_mode == BF_DEV_INIT_COLD) {
    /* cold initialzation. the driver sysfs file, "m_init" is written the
     * parameter '1' to indicate that it is a cold initialization */
    if (bf_switchd_sysfs_set(
            switchd_ctx->asic[0].pcie_cfg.pci_sysfs_str, "m_init", "1", 1)) {
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "Kernel packet module master initialization failed");
      return -1;
    }
  }
#endif

  /* Initialize driver libraries and start PD API RPC thrift server */
  ret = bf_switchd_driver_init(switchd_ctx->args.kernel_pkt);
  if (ret != 0) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_ERR,
                         "bf-driver initialization failed : %d",
                         ret);
    if (switchd_ctx) free(switchd_ctx);
    return ret;
  } else {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD, BF_LOG_DBG, "bf_switchd: drivers initialized");
  }

  /* Initialize DRU simulation service if operating mode is model */
  for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    if (switchd_ctx->asic[dev_id].configured == 0) continue;
    if (switchd_ctx->asic[dev_id].is_virtual) continue;
    if (switchd_ctx->asic[dev_id].is_sw_model) {
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_DBG,
                           "bf_switchd: initializing dru_sim service");
      ret = bf_switchd_dru_sim_init();
      if (ret != 0) {
        bf_sys_log_and_trace(BF_MOD_SWITCHD,
                             BF_LOG_ERR,
                             "dru_sim initialization failed : %d",
                             ret);
        return ret;
      }
      break;
    }
  }

  if (switchd_ctx->args.shell_before_dev_add) {
    /* Start the driver shell before devices are added. */
    sts = bf_switchd_drv_shell_start(switchd_ctx->args.install_dir,
                                     switchd_ctx->args.running_in_background,
                                     switchd_ctx->args.shell_set_ucli,
                                     switchd_ctx->args.server_listen_local_only,
                                     switchd_ctx->p4_devices);
    if (sts != BF_SUCCESS) {
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "driver shell start failed : %s",
                           bf_err_str(sts));
      return sts;
    }
  }

  bf_pm_port_mac_map_mtx_init();

  /* Initialize all devices and their ports */
  int num_devices_initialized = 0;
  for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    if (switchd_ctx->asic[dev_id].configured == 0) continue;

    /* Issue a warm-init-begin if switchd is starting in "HA mode". */
    if (switchd_ctx->args.init_mode == BF_DEV_WARM_INIT_FAST_RECFG ||
        switchd_ctx->args.init_mode == BF_DEV_WARM_INIT_HITLESS) {
      switchd_ctx->state[dev_id].device_locked = true;
      sts = bf_device_warm_init_begin(
          dev_id, switchd_ctx->args.init_mode, BF_DEV_SERDES_UPD_NONE);
      if (BF_SUCCESS != sts) {
        bf_sys_log_and_trace(
            BF_MOD_SWITCHD,
            BF_LOG_ERR,
            "ERROR: bf_device_warm_init_begin returned %s for dev %d",
            bf_err_str(sts),
            dev_id);
      }
    }
    /* Add device */
    sts = bf_switchd_device_add(dev_id, true);
    if (sts != BF_SUCCESS) {
      bf_sys_log_and_trace(
          BF_MOD_SWITCHD,
          BF_LOG_ERR,
          "ERROR: bf_switchd_device_add failed(%s) for dev_id %d",
          bf_err_str(sts),
          dev_id);
      continue;
    }
    if (switchd_ctx->args.kernel_pkt &&
        switchd_ctx->args.init_mode == BF_DEV_INIT_COLD) {
#ifndef __sun
      if (bf_switchd_sysfs_set(switchd_ctx->asic[dev_id].pcie_cfg.pci_sysfs_str,
                               "dev_add",
                               "1",
                               1)) {
        bf_sys_log_and_trace(BF_MOD_SWITCHD,
                             BF_LOG_ERR,
                             "ERROR: kernel packet module device add failed");
        continue;  // allow the remaining devices to come up
      }
#else
	if (switchd_ctx->args.kernel_pkt) {
		int resetting = 0;
		printf("activating tfpkt\n");
		int dev_fd = (&(switchd_ctx->pcie_map[dev_id][0]))->dev_fd;
		if (ioctl(dev_fd, BF_PKT_INIT, &resetting) != 0) {
			printf("failed to enable tbus access: %s\n",
			    strerror(errno));
		}
	}
#endif
    }

    num_devices_initialized++;

    /* Add ports for the device */
    bf_switchd_ports_add(dev_id);

    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_DBG,
                         "bf_switchd: dev_id %d initialized",
                         dev_id);
  }

  if (switchd_ctx->args.init_mode == BF_DEV_WARM_INIT_FAST_RECFG ||
      switchd_ctx->args.init_mode == BF_DEV_WARM_INIT_HITLESS) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD,
        BF_LOG_DBG,
        "Resetting init mode to cold-boot since all devices have completed "
        "warm-init");
    switchd_ctx->args.init_mode = BF_DEV_INIT_COLD;
  }
  bf_sys_log_and_trace(BF_MOD_SWITCHD,
                       BF_LOG_DBG,
                       "bf_switchd: initialized %d devices",
                       num_devices_initialized);

  // Register the functions against the interfaces in bf_pal layer
  if (bf_switchd_pal_handler_reg() != 0) {
    bf_sys_log_and_trace(
        BF_MOD_SWITCHD,
        BF_LOG_ERR,
        "Unable to register the functions to be used by switchapi");
  }

  /* (optionally) Create the diagnostic python interface for external
     python modules. Defines in diag_py.c determine what, if any,
     diagnostic interfaces are created. */
  diagnostic_python_interface(&switchd_ctx->args.server_listen_local_only);

  /* Initialize P4 specific libraries for all devices */
  for (dev_id = 0; dev_id < BF_MAX_DEV_COUNT; dev_id++) {
    if (switchd_ctx->asic[dev_id].configured == 0) continue;
    bf_switchd_p4_lib_init(dev_id);
#ifndef STATIC_LINK_LIB
    bf_switchd_accton_diag_lib_init(dev_id);
#endif  // STATIC_LINK_LIB

#ifdef THRIFT_ENABLED
    /* Initialize the agent thrift libraries. This MUST happen only AFTER
       the thrift server has been created */
    bf_switchd_agent_thrift_lib_init(dev_id);
#endif  // THRIFT_ENABLED
  }

  if (!switchd_ctx->args.shell_before_dev_add) {
    /* Start the driver shell now that devices are added. */
    sts = bf_switchd_drv_shell_start(switchd_ctx->args.install_dir,
                                     switchd_ctx->args.running_in_background,
                                     switchd_ctx->args.shell_set_ucli,
                                     switchd_ctx->args.server_listen_local_only,
                                     switchd_ctx->p4_devices);
    if (sts != BF_SUCCESS) {
      bf_sys_log_and_trace(BF_MOD_SWITCHD,
                           BF_LOG_ERR,
                           "ERROR: driver shell start failed : %s",
                           bf_err_str(sts));
      return sts;
    }
  }

  /* Return the created threads to the caller. */
  ctx->tmr_t_id = switchd_ctx->args.tmr_t_id;
  ctx->dma_t_id = switchd_ctx->args.dma_t_id;
  ctx->int_t_id = switchd_ctx->args.int_t_id;
  ctx->pkt_t_id = switchd_ctx->args.pkt_t_id;
  ctx->port_fsm_t_id = switchd_ctx->args.port_fsm_t_id;
  ctx->drusim_t_id = switchd_ctx->args.drusim_t_id;
  ctx->accton_diag_t_id = switchd_ctx->args.accton_diag_t_id;
  for (agent_idx = 0; agent_idx < BF_SWITCHD_MAX_AGENTS; agent_idx++) {
    if (switchd_ctx->args.agent_t_id[agent_idx] != 0) {
      ctx->agent_t_id[agent_idx] = switchd_ctx->args.agent_t_id[agent_idx];
    }
  }
  for (bf_dev_id_t device = 0; device < BF_MAX_DEV_COUNT; device++) {
    if (!switchd_ctx->asic[device].configured) continue;
    for (int program = 0;
         program < switchd_ctx->p4_devices[device].num_p4_programs;
         program++) {
      if (switchd_ctx->p4_devices[device]
              .p4_programs[program]
              .use_eth_cpu_port) {
        ctx->eth_cpu_port_name[device] = strdup(switchd_ctx->p4_devices[device]
                                                    .p4_programs[program]
                                                    .eth_cpu_port_name);
        break;
      }
    }
  }

// Starting GRPC servers only after device add is done
#ifdef GRPC_ENABLED
#ifdef BFRT_ENABLED
  sts = bf_rt_grpc_server_run(
      switchd_ctx->p4_devices[0].p4_programs[0].program_name,
      switchd_ctx->args.server_listen_local_only);
  if (sts != BF_SUCCESS) {
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_DBG,
                         "ERROR: Failed to start bfrt grpc server: %s",
                         bf_err_str(sts));
    return BF_EAGAIN;
  }
#endif
#endif

#ifdef P4RT_ENABLED
  if (switchd_ctx->args.p4rt_server) {
    BfP4RtServerRunAddr(switchd_ctx->args.p4rt_server);
    bf_sys_log_and_trace(BF_MOD_SWITCHD,
                         BF_LOG_DBG,
                         "P4Runtime GRPC server started on %s",
                         switchd_ctx->args.p4rt_server);
  }
#endif

  if (0) bf_switchd_ports_delete(0);  // Avoid the compiler warning

  bf_sys_mutex_lock(&switchd_ctx->init_done_mutex);
  switchd_ctx->init_done = true;
  bf_sys_mutex_unlock(&switchd_ctx->init_done_mutex);
  return ret;
}

//============================================================================
// FPGA tile driver
//
#include <time.h>
#include <sys/time.h>
int detailed_dbg_print = 0;
int dbg_print = 0;
int log_timestamps = 0;

void log_time(char *why) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  printf("%lld.%.9ld : %s", (long long)ts.tv_sec, ts.tv_nsec, why);
}

void log_timeval(struct timeval *tm) {
  char tbuf[256] = {0};
  char ubuf[256] = {0};
  struct tm *loctime;

  loctime = localtime(&tm->tv_sec);

  strftime(tbuf, sizeof(tbuf), "%a %b %d", loctime);
  printf("%s ", tbuf);

  strftime(ubuf, sizeof(ubuf), "%T\n", loctime);
  ubuf[strlen(ubuf) - 1] = 0;  // remove CR
  printf("%s.%06d : ", ubuf, (int)tm->tv_usec);
}

#define FPGA_SIGNATURE 0x0008

#define FPGA_TILE_0 0x22000
#define FPGA_TILE_1 0x23000
#define FPGA_TILE_2 0x24000
#define FPGA_TILE_3 0x25000

uint32_t tile_offset[4] = {FPGA_TILE_0, FPGA_TILE_1, FPGA_TILE_2, FPGA_TILE_3};
int active_tile = 0;
int active_group = 0;

typedef enum {
  FPGA_CMD_WRITE = 1,
  FPGA_CMD_READ = 2,
  FPGA_CMD_RST = 3,
  FPGA_CMD_TCK = 4,  // update jtag clock (speed)
} fpga_ctrlr_cmd_e;

#define FPGA_STATUS_DONE (1 << 16)
#define FPGA_STATUS_ERROR (2 << 16)

typedef enum {
  R0_CTRL_STATUS = 0,
  R1_GROUP = 4,
  R2_ADDR = 8,
  R3_DATA = 12,
} fpga_regs_e;

void fpga_active_tile_set(int tile) {
  if (tile < 4) {
    active_tile = tile;
  } else {
    active_tile = 0;
  }
}

int fpga_wait_done(int max_tmout) {
  int tmout = max_tmout;
  uint32_t sts;
  uint32_t raw_sts;

  do {
    // raw_sts = (volatile uint32_t)fpga_ctrlr->r0_ctrl_status;
    bf_fpga_reg_read32_fn(
        0, tile_offset[active_tile] + R0_CTRL_STATUS, &raw_sts);

    sts = raw_sts & 0xFFFF0000;
  } while ((sts == 0) && (tmout-- > 0));

  // printf("fpga_ctrlr=%p : status=0x%08x\n", fpga_ctrlr, raw_sts);

  if (sts == FPGA_STATUS_DONE) {
    return 0;
  } else if (sts == FPGA_STATUS_ERROR) {
    printf("..error.. ");
    return -1;
  } else if (tmout <= 0) {
    printf("..timeout..[%d] ", max_tmout);
    return -2;
  }
  printf("..ERROR..[sts=%08x, tmout=%d] ", raw_sts, tmout);
  exit(1);
  // return -3;
}

#define MAX_TILE 4
void fpga_ctrlr_reset(void) {
  int tile;

  for (tile = 0; tile < MAX_TILE; tile++) {
    fpga_active_tile_set(tile);
    // fpga_ctrlr->r0_ctrl_status = FPGA_CMD_RST;
    bf_fpga_reg_write32_fn(0, tile_offset[tile] + R0_CTRL_STATUS, FPGA_CMD_RST);
  }
  sleep(1);
  printf("Set MDIO speed to 2Mhz (divider=30)\n");
  for (tile = 0; tile < MAX_TILE; tile++) {
    fpga_active_tile_set(tile);
    // fpga_ctrlr->r3_data = 30; // 2Mhz (62.5Mhz/(div+2)
    bf_fpga_reg_write32_fn(0, tile_offset[tile] + R3_DATA, 30);
    // fpga_ctrlr->r0_ctrl_status = FPGA_CMD_TCK;
    bf_fpga_reg_write32_fn(0, tile_offset[tile] + R0_CTRL_STATUS, FPGA_CMD_TCK);
  }
  sleep(1);
}

void fpga_ctrlr_rd(uint32_t addr, uint32_t *data) {
  if (fpga_wait_done(1000) != 0) {
    printf("Previous operation not done (before read)\n");
    *data = 0x00000bad;
  }
  if (log_timestamps) log_time("Read start: \n");
  // fpga_ctrlr->r1_group = active_group;
  // fpga_ctrlr->r2_addr  = addr;
  // fpga_ctrlr->r0_ctrl_status = FPGA_CMD_READ;
  bf_fpga_reg_write32_fn(0, tile_offset[active_tile] + R1_GROUP, active_group);
  bf_fpga_reg_write32_fn(0, tile_offset[active_tile] + R2_ADDR, addr);
  bf_fpga_reg_write32_fn(
      0, tile_offset[active_tile] + R0_CTRL_STATUS, FPGA_CMD_READ);

  if (fpga_wait_done(1000) != 0) {
    printf("Read failed\n");
    *data = 0x00001bad;
    return;
  }
  //*data = fpga_ctrlr->r3_data;
  bf_fpga_reg_read32_fn(0, tile_offset[active_tile] + R3_DATA, data);

  if (log_timestamps) log_time("Read done :\n");
}

void fpga_ctrlr_wr(uint32_t addr, uint32_t data) {
  if (fpga_wait_done(1000) != 0) {
    printf("Previous operation not done (before write)\n");
  }
  if (log_timestamps) log_time("Write start:\n");
  // fpga_ctrlr->r1_group = active_group;
  // fpga_ctrlr->r2_addr  = addr;
  // fpga_ctrlr->r3_data  = data;
  // fpga_ctrlr->r0_ctrl_status = FPGA_CMD_WRITE;
  bf_fpga_reg_write32_fn(0, tile_offset[active_tile] + R1_GROUP, active_group);
  bf_fpga_reg_write32_fn(0, tile_offset[active_tile] + R2_ADDR, addr);
  bf_fpga_reg_write32_fn(0, tile_offset[active_tile] + R3_DATA, data);
  bf_fpga_reg_write32_fn(
      0, tile_offset[active_tile] + R0_CTRL_STATUS, FPGA_CMD_WRITE);

  if (fpga_wait_done(1000) != 0) {
    printf("Write failed\n");
    return;
  }
  if (log_timestamps) log_time("Write done :\n");
}

bool fpga_lock_initd = false;
bf_sys_mutex_t mtx;

void fpga_lock_init(void) {
  bf_sys_mutex_init(&mtx);
  fpga_lock_initd = true;
}
void fpga_lock_p(void) {
  if (!fpga_lock_initd) {
    fpga_lock_init();
  }
  bf_sys_mutex_lock(&mtx);
}
void fpga_lock_v(void) {
  if (!fpga_lock_initd) {
    fpga_lock_init();
  }
  bf_sys_mutex_unlock(&mtx);
}

void tile_sim_rd(uint32_t mac_stn_id, uint32_t addr, uint32_t *data) {
  uint32_t tile, grp;

  fpga_lock_p();

  lld_sku_map_mac_stn_id_to_tile_and_group(0, mac_stn_id, &tile, &grp);
  fpga_active_tile_set(tile);
  active_group = grp;

  // special check for group8 FW load. Must set group=9
  if ((addr >= 0x5000) && (addr <= 0x500F) && (grp == 8)) {
    active_group = 9;
  } else if (((addr >> 11) & 0xF) == 9) {
    active_group = 9;
  }

  if (0 && dbg_print)
    printf("Rd(1): mac_stn_id=%d : addr=%04x : data=na : tile=%d : grp=%d\n",
           mac_stn_id,
           addr,
           tile,
           grp);
  fpga_ctrlr_rd(addr, data);
  if (0 && dbg_print)
    printf("Rd(2): mac_stn_id=%d : addr=%04x : data=%04x : tile=%d : grp=%d\n",
           mac_stn_id,
           addr,
           *data,
           tile,
           grp);
  {
    if (detailed_dbg_print && dbg_print) {
      //_log_time();
      printf("Tile: %d : Group: %d : %s : Addr: %04x : Data : %04x\n",
             active_tile,
             active_group,
             "Read ",
             addr,
             *data);
    }
    fflush(stdout);
  }

  fpga_lock_v();
}

void tile_sim_wr(uint32_t mac_stn_id, uint32_t addr, uint32_t data) {
  uint32_t tile, grp;

  fpga_lock_p();

  lld_sku_map_mac_stn_id_to_tile_and_group(0, mac_stn_id, &tile, &grp);
  if (0 && dbg_print)
    printf("Wr   : mac_stn_id=%d : addr=%04x : data=%04x : tile=%d : grp=%d\n",
           mac_stn_id,
           addr,
           data,
           tile,
           grp);
  fpga_active_tile_set(tile);
  active_group = grp;

  // special check for group8 FW load. Must set group=9
  if ((addr >= 0x5000) && (addr <= 0x500F) && (grp == 8)) {
    active_group = 9;
  } else if (((addr >> 11) & 0xF) == 9) {
    active_group = 9;
  }

  fpga_ctrlr_wr(addr, data);
  {
    if (detailed_dbg_print && dbg_print) {
      //_log_time();
      printf("Tile: %d : Group: %d : %s : Addr: %04x : Data : %04x\n",
             active_tile,
             active_group,
             "Write",
             addr,
             data);
    }
    fflush(stdout);
  }

  fpga_lock_v();
}

void fpga_read_register(bf_dev_id_t dev_id,
                        uint32_t bfn_addr,
                        uint32_t *reg_val) {
  // bfn_addr = (3 << 24) | (mac_stn_id << 18) | bfn_ofs;
  // printf("lld_read_register: dev_id=%d : bfn_addr = %08x\n", dev_id,
  // bfn_addr);
  uint32_t mac = (bfn_addr >> 18) & 0x3f;
  uint32_t addr = (bfn_addr >> 2) & 0xffff;
  tile_sim_rd(mac, addr, reg_val);
  (void)dev_id;
}

void fpga_write_register(bf_dev_id_t dev_id,
                         uint32_t bfn_addr,
                         uint32_t reg_val) {
  // printf("lld_write_register: dev_id=%d : bfn_addr = %08x : reg_val =
  // %08x\n", dev_id, bfn_addr, reg_val);
  uint32_t mac = (bfn_addr >> 18) & 0x3f;
  uint32_t addr = (bfn_addr >> 2) & 0xffff;
  tile_sim_wr(mac, addr, reg_val);
  (void)dev_id;
}
