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


#ifndef __BF_SWITCHD_H__
#define __BF_SWITCHD_H__
/*-------------------- bf_switchd.h -----------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <bf_types/bf_types.h>
#include <mc_mgr/mc_mgr_types.h>
#include <dvm/bf_drv_intf.h>

#define BF_SWITCHD_MAX_FILE_NAME 512

/* must match with those defined in kernle module */
#define BF_SWITCHD_INT_MODE_NONE 0
#define BF_SWITCHD_INT_MODE_INTX 1
#define BF_SWITCHD_INT_MODE_MSI 2
#define BF_SWITCHD_INT_MODE_MSIX 3

#define BF_SWITCHD_MSIX_NONE 0
#define BF_SWITCHD_MSIX_HOSTIF 1
#define BF_SWITCHD_MSIX_TBUS 2
#define BF_SWITCHD_MSIX_CBUS 3
#define BF_SWITCHD_MSIX_MBUS 4
#define BF_SWITCHD_MSIX_PBUS 5

typedef struct switchd_pcie_cfg_s {
  /* The sysfs path for a given device, primarily used to communicate with the
   * kernel driver.  This will be set once and taken from the conf file. */
  char pci_sysfs_str[128];
  /* The interrupt mode the device will use, this will be set by querying the
   * kernel driver.  0:INTX, 1:MSI, 2:MSIX */
  uint8_t int_mode;
} switchd_pcie_cfg_t;

/* A helper to hold the path to the directory containing the serdes firmware
 * files.  This path will be prepended to the firmware filenames and copied into
 * the bf_device_profile_t used as part of the bf_device_add when a device is
 * added to the driver. */
typedef struct switchd_serdes_cfg_s {
  char *sds_fw_path;
} switchd_serdes_cfg_t;

typedef struct switch_asic_s {
  int pci_dev_id;
  int configured;
  bf_dev_family_t chip_family;
  bool is_virtual;
  bool is_sw_model;
  switchd_pcie_cfg_t pcie_cfg;
  switchd_serdes_cfg_t serdes_cfg;
  bf_sys_mutex_t switch_mutex; /* A mutex to protect device_ready. */
  bf_sys_mutex_t pktmgr_mutex;
  unsigned short subdev_pres_msk;  // subdev mask bit=1 means presence
} switch_asic_t;

#define BF_SWITCHD_MAX_AGENTS 5
#define BF_SWITCHD_MAX_PIPES BF_PIPE_COUNT
#define BF_SWITCHD_MAX_P4_PROGRAMS BF_SWITCHD_MAX_PIPES
#define BF_SWITCHD_MAX_P4_PIPELINES BF_SWITCHD_MAX_PIPES
typedef struct p4_pipeline_config_s {
  char *p4_pipeline_name;
  char pd[BF_SWITCHD_MAX_FILE_NAME];
  char pd_thrift[BF_SWITCHD_MAX_FILE_NAME];
  char table_config[BF_SWITCHD_MAX_FILE_NAME];
  char tofino_bin[BF_SWITCHD_MAX_FILE_NAME];
  char pi_native_config_path[BF_SWITCHD_MAX_FILE_NAME];
  int num_pipes_in_scope;
  int pipe_scope[BF_SWITCHD_MAX_PIPES];
} p4_pipeline_config_t;

typedef struct p4_programs_s {
  char *program_name;
  char diag[BF_SWITCHD_MAX_FILE_NAME];
  char accton_diag[BF_SWITCHD_MAX_FILE_NAME];
  char switchapi[BF_SWITCHD_MAX_FILE_NAME];
  char switchsai[BF_SWITCHD_MAX_FILE_NAME];
  bool add_ports_to_switchapi;
  bool use_eth_cpu_port;
  char eth_cpu_port_name[BF_SWITCHD_MAX_FILE_NAME];
  char bfrt_config[BF_SWITCHD_MAX_FILE_NAME];
  int num_p4_pipelines;
  p4_pipeline_config_t p4_pipelines[BF_SWITCHD_MAX_P4_PIPELINES];
} p4_programs_t;

typedef struct p4_devices_s {
  uint8_t num_p4_programs;
  p4_programs_t p4_programs[BF_SWITCHD_MAX_P4_PROGRAMS];
  char agent[BF_SWITCHD_MAX_AGENTS][BF_SWITCHD_MAX_FILE_NAME];
  int coal_mirror_enable;
  int coal_sessions_num;
  int coal_min;
} p4_devices_t;

typedef struct switchd_p4_pipeline_state_s {
  void *pd_lib_handle;
  void *pd_thrift_lib_handle;
} switchd_p4_pipeline_state_t;

typedef struct switchd_p4_program_state_s {
  void *switchapi_lib_handle;
  void *switchsai_lib_handle;
  void *diag_lib_handle;
  void *accton_diag_lib_handle;
  switchd_p4_pipeline_state_t p4_pipeline_state[BF_SWITCHD_MAX_P4_PIPELINES];
} switchd_p4_program_state_t;

typedef struct switchd_state_s {
  switchd_p4_program_state_t p4_programs_state[BF_SWITCHD_MAX_P4_PROGRAMS];
  void *agent_lib_handle[BF_SWITCHD_MAX_AGENTS];
  bool device_locked;
  bool device_ready;
  bool device_pktmgr_ready;
  bool device_pci_err;
} switchd_state_t;

typedef struct switchd_pcie_map_s {
  bool configured;
  uint8_t *dev_base_addr; /* BAR0 base addr */
  size_t dev_base_size;   /* BAR0 mem size */
  int dev_fd;             /* file handle */
  char cdev_name[32];     /* device node name, e.g., /dev/bf0 */
} switchd_pcie_map_t;

typedef struct switchd_skip_hld_s {
  bool pipe_mgr;
  bool mc_mgr;
  bool traffic_mgr;
  bool pkt_mgr;
  bool port_mgr;
} switchd_skip_hld_t;

#ifdef STATIC_LINK_LIB
typedef void *(*bf_switchd_agent_init_fn_t)(void *arg);
typedef void *(*bf_switchd_agent_init_done_fn_t)(void *arg);
typedef int (*bf_pltfm_device_type_get_fn_t)(bf_dev_id_t dev_id,
                                             bool *is_sw_model);
#ifdef THRIFT_ENABLED
typedef int (*agent_rpc_server_thrift_service_add_fn_t)(void *processor);
typedef int (*agent_rpc_server_thrift_service_rmv_fn_t)(void *processor);
#endif  // THRIFT_ENABLED

#endif  // STATIC_LINK_LIB

typedef struct bf_switchd_context_s {
  /* Path to the install directory of the BF-SDE build. */
  char *install_dir;

  /* Path and file name of the bf_switchd conf file to load. */
  char *conf_file;

  /* When true, no P4 program will be loaded on the device.  This may be used
   * when a remote controller will later push a P4 configuration to the asic. */
  bool skip_p4;

  /* Set to true when using a kernel module to process packets sent and received
   * over PCIe, for example kpkt. */
  bool kernel_pkt;

  /* Specifies the behavior of the initial add-device.  If BF_DEV_INIT_COLD
   * (zero) a normal device add sequence is performed.  If
   * BF_DEV_WARM_INIT_FAST_RECFG or BF_DEV_WARM_INIT_HITLESS a warm-init-begin
   * call is made before the device add.  This may be used to perform HA events
   * where there is a process restart involved. */
  bf_dev_init_mode_t init_mode;

  /* When true only allow connections from local host on:
   *   gRPC server
   *   thrift server
   *   status server
   *   bfshell */
  bool server_listen_local_only;

  /* A string in the format of address:port specifying the address of the
   * P4Runtime gRPC server.  Can be NULL if P4Runtime is not enabled. */
  char *p4rt_server;

  /* When true do not start any interactive CLI session, remote a connection can
   * still be used to access a CLI session. */
  bool running_in_background;

  char *model_ip;

  /* Base TCP port number used for communication with the asic model via the
   * dru_sim library. */
  int tcp_port_base;

  /* When true start a thread which will listen on dev_sts_port and report the
   * status of a device (whether or not is has been added and is ready). */
  bool dev_sts_thread;
  uint16_t dev_sts_port;

  // define bfrt_grpc_port_status
  uint16_t bf_rt_grpc_port;

  /* Start uCLI as the default CLI shell instead of BFShell. */
  bool shell_set_ucli;

  /* Out parameters. */
  pthread_t dev_sts_t_id;
  pthread_t tmr_t_id;
  pthread_t drusim_t_id;
  pthread_t dma_t_id;
  pthread_t int_t_id;
  bf_sys_thread_t port_int_t_id;
  pthread_t pkt_t_id;
  pthread_t port_fsm_t_id;
  pthread_t agent_t_id[BF_SWITCHD_MAX_AGENTS];
  pthread_t accton_diag_t_id;

  char *eth_cpu_port_name[BF_MAX_DEV_COUNT];

  /*
   * Debug options
   */

  /* When true no ports will be added by default.  When false and running on the
   * model all ports will be added at a single channel speed such as 10g.  When
   * false and running against the asic the non-MAC ports (PCIe CPU and
   * recirculation ports) will be added at their highest supported speed. */
  bool skip_port_add;

  /* When true start the CLI before the first device-add is performed. */
  bool shell_before_dev_add;

  /* When true the thread which polls for DMA completions will not be started.
   */
  bool skip_dma_thread;

  /* For low level debug only!  Bypass the initialization of various low level
   * SDE driver components, not to be used during normal switch operation. */
  switchd_skip_hld_t skip_hld;
  void (*bf_switchd_agent_deinit_fn)(void);

  /*
   * The revision of the Sidecar main board against which we're running.
   *
   * This is used to load the correct version of the board map, describing
   * filtering parameters, connector-to-MAC-channel mappings, lane inversions,
   * etc.
   */
  char *sidecar_revision;

} bf_switchd_context_t;

typedef struct bf_switchd_internal_context_t {
  /* Various knobs passed in by the application. */
  bf_switchd_context_t args;
  /* Per device context */
  switch_asic_t asic[BF_MAX_DEV_COUNT];
  p4_devices_t p4_devices[BF_MAX_DEV_COUNT];
  switchd_state_t state[BF_MAX_DEV_COUNT];
  switchd_pcie_map_t pcie_map[BF_MAX_DEV_COUNT][BF_MAX_SUBDEV_COUNT];
  bf_sys_mutex_t init_done_mutex;
  bool init_done;
  /* MUST keep dma_info of all subdevices within a device contiguous!! */
  bf_dma_info_t dma_info[BF_MAX_DEV_COUNT][BF_MAX_SUBDEV_COUNT];
  char board_port_map_conf_file[BF_SWITCHD_MAX_FILE_NAME];
  void *rpc_server_cookie;
  uint32_t (*dru_sim_cpu_to_pcie_rd_fn)(bf_dev_id_t asic, uint32_t addr);
  void (*dru_sim_cpu_to_pcie_wr_fn)(bf_dev_id_t asic,
                                    uint32_t addr,
                                    uint32_t value);

#ifdef STATIC_LINK_LIB
  bf_switchd_agent_init_fn_t bf_switchd_agent_init_fn;
  bf_switchd_agent_init_done_fn_t bf_switchd_agent_init_done_fn;
  bf_pltfm_device_type_get_fn_t bf_pltfm_device_type_get_fn;
#ifdef THRIFT_ENABLED
  agent_rpc_server_thrift_service_add_fn_t
      agent_rpc_server_thrift_service_add_fn;
  agent_rpc_server_thrift_service_rmv_fn_t
      agent_rpc_server_thrift_service_rmv_fn;
#endif  // THRIFT_ENABLED
#endif  // STATIC_LINK_LIB
} bf_switchd_internal_context_t;

typedef void *(*agent_init_fn_t)(void *);
typedef void *(*agent_init_done_fn_t)(void *);

typedef int (*bf_pltfm_reg_dir_i2c_rd)(bf_dev_id_t dev_id,
                                       bf_subdev_id_t subdev_id,
                                       uint32_t addr,
                                       uint32_t *data);
typedef int (*bf_pltfm_reg_dir_i2c_wr)(bf_dev_id_t dev_id,
                                       bf_subdev_id_t subdev_id,
                                       uint32_t addr,
                                       uint32_t data);

void bf_switchd_i2c_fn_reg(bf_pltfm_reg_dir_i2c_rd rd_fn,
                           bf_pltfm_reg_dir_i2c_wr wr_fn);

void switchd_register_ucli_node(void);

int bf_switchd_lib_init(bf_switchd_context_t *ctx);

bool bf_switchd_is_kernel_pkt_module_present();
bool bf_switchd_is_kernel_pkt_proc(bf_dev_id_t dev_id);

void bf_switchd_exit_sighandler(int signum);

int bf_switchd_device_type_update(bf_dev_id_t dev_id, bool is_sw_model);

bf_status_t bf_switchd_warm_init_end(bf_dev_id_t dev_id);

bf_status_t bf_switchd_kpkt_enable_traffic(bf_dev_id_t dev_id);

bf_status_t bf_switchd_device_remove(bf_dev_id_t dev_id);
bf_status_t bf_switchd_device_add(bf_dev_id_t dev_id, bool setup_dma);

/******************************************************************************
*******************************************************************************
                          BF_SWITCHD OPERATIONAL MODE SETTINGS
*******************************************************************************
******************************************************************************/

/* Define for exercising FASTRECFG test */
#define FASTRECFG_TEST

/* Define for enabling register access log */
#define REG_ACCESS_LOG

#endif /* __BF_SWITCHD_H__ */
