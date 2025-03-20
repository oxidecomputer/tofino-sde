/*******************************************************************************
 * Copyright (c) 2015-2020 Barefoot Networks, Inc.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * $Id: $
 *
 ******************************************************************************/
#include <stdio.h>
#include <unistd.h>  //write
#define __USE_GNU    /* See feature_test_macros(7) */
#include <pthread.h>

#include <target-sys/bf_sal/bf_sys_intf.h>
#include <bf_pltfm_types/bf_pltfm_types.h>
#include <bf_switchd/bf_switchd.h>
#include <bf_bd_cfg/bf_bd_cfg_porting.h>
#include <bf_bd_cfg/bf_bd_cfg_intf.h>
#include "bf_pltfm.h"
#include <bf_port_mgmt/bf_port_mgmt_intf.h>
#include <bf_port_mgmt/bf_port_mgmt_porting.h>
#include <bf_qsfp/bf_qsfp.h>
#include <tofino/bf_pal/bf_pal_port_intf.h>
#include <tofino/bf_pal/bf_pal_pltfm_porting.h>

/* global */
static bool pltfm_mgr_init_done = false;

/*
 * 'TCL_ONLY' flag is to be used when you want to build the SDE to run on the
 * emulator. On the emulator we really only care about the tcl server and
 * thus build only the tcl server component of bf-platforms. The 'TCL_ONLY'
 * has not been propagated in the bf-drivers. Thus bf-switchd will still call
 * 'bf_pltfm_device_type_get' function which is actually defined in the chss-
 * mgmt module. However, since we don't compile that module in the 'TCL_ONLY'
 * option, we define a dummy version of the function under this. Also we
 * add placeholder functions for the thrift add and rmv services so that when
 * bf_switchd calls those, we don't do anything and simply return.
 */
#ifdef TCL_ONLY
bf_pltfm_status_t bf_pltfm_device_type_get(bf_dev_id_t dev_id,
                                           bool *is_sw_model) {
/* This func returns the device type based on compile time flags */
#ifndef DEVICE_IS_ASIC
  *is_sw_model = true;
#else
  *is_sw_model = false;
#endif
  return BF_PLTFM_SUCCESS;
}

int agent_rpc_server_thrift_service_add(void *processor) {
  bf_pltfm_agent_rpc_server_thrift_service_add(processor);
  return 0;
}

int agent_rpc_server_thrift_service_rmv(void *processor) {
  bf_pltfm_agent_rpc_server_thrift_service_rmv(processor);
  return 0;
}

/* Initialize and start pltfm_mgrs server */
bf_status_t pltfm_mgr_init(bf_switchd_context_t *switchd_ctx) {
  /* platform specific init here */
  if (bf_pltfm_platform_init(switchd_ctx) != BF_PLTFM_SUCCESS) {
    LOG_ERROR("pltfm_mgr: platform init failed\n");
    return -1;
  }

  pltfm_mgr_init_done = true;
  /* listen to client commands and execute them */
  while (1) {
    usleep(10000); /* TBD : probably not required */
  }

  return 0;
}

/* platform-mgr thread exit callback API */
static void pltfm_mgr_exit_cleanup(void *arg) {
  (void)arg;
  bf_pltfm_platform_exit(arg); /* platform specific cleanup */
}
/* Init function  */
void *bf_switchd_agent_init(void *arg) {
  if (!arg) return NULL;
  bf_switchd_context_t *switchd_ctx = (bf_switchd_context_t *)arg;
  LOG_DEBUG("Platform-mgr started \n");
  /* Register cleanup handler */
  pthread_cleanup_push(pltfm_mgr_exit_cleanup, "Platform-Mgr Cleanup handler");
  pltfm_mgr_init(switchd_ctx);
  pthread_cleanup_pop(0);
  return NULL;
}

#else  // TCL_ONLY
/* platform-mgr thread exit callback API */
static void pltfm_mgr_exit_cleanup(void *arg) {
  (void)arg;

  if (bf_pltfm_pm_deinit()) {
    LOG_ERROR("pltfm_mgr: Error while de-initializing pltfm mgr");
  }

  bf_pltfm_platform_exit(arg); /* platform specific cleanup */
}

// static void pltfm_mgr_exit() { bf_pltfm_platform_exit(NULL); }

/* Initialize and start pltfm_mgrs server */
bf_status_t pltfm_mgr_init(bf_switchd_context_t *switchd_ctx) {
  bf_status_t sts;

  /* initialize qsfp data structures */
  if (bf_qsfp_init()) {
    LOG_ERROR("pltfm_mgr: qsfp soft init failed\n");
    return -1;
  }
  /* platform specific init here */
  if (bf_pltfm_platform_init(switchd_ctx) != BF_PLTFM_SUCCESS) {
    LOG_ERROR("pltfm_mgr: platform init failed\n");
    return -1;
  }

  /* init board config data structures */
  if (bf_bd_cfg_init() != BF_PLTFM_SUCCESS) {
    LOG_ERROR("pltfm_mgr: Board Config library initialization failed\n");
    return -1;
  }

  /* Register pltfm port manager */
  if (!switchd_ctx->skip_hld.port_mgr) {
    if (bf_pltfm_pm_init(switchd_ctx->init_mode) != BF_PLTFM_SUCCESS) {
      LOG_ERROR(
          "pltfm_mgr: Error while initializing the platforms port manager\n");
    }
  }

  if (platform_is_hw()) {
    // Register the interfaces for bf_pm
    bf_pal_pltfm_reg_interface_t bf_pm_interface = {0};
    bf_pm_interface.pltfm_safe_to_call_in_notify =
        &bf_pm_safe_to_call_in_notify;
    bf_pm_interface.pltfm_front_panel_port_get_first =
        &bf_bd_cfg_front_panel_port_get_first;
    bf_pm_interface.pltfm_front_panel_port_get_next =
        &bf_bd_cfg_front_panel_port_get_next;
    bf_pm_interface.pltfm_mac_to_serdes_map_get =
        &bf_bd_cfg_mac_to_serdes_map_get;
    bf_pm_interface.pltfm_serdes_info_get = &bf_bd_cfg_serdes_info_get;
    bf_pm_interface.pltfm_pre_port_add_cfg_set = &bf_pm_pre_port_add_cfg_set;
    bf_pm_interface.pltfm_post_port_add_cfg_set = &bf_pm_post_port_add_cfg_set;
    bf_pm_interface.pltfm_pre_port_delete_cfg_set =
        &bf_pm_pre_port_delete_cfg_set;
    bf_pm_interface.pltfm_post_port_delete_cfg_set =
        &bf_pm_post_port_delete_cfg_set;
    bf_pm_interface.pltfm_pre_port_enable_cfg_set =
        &bf_pm_pre_port_enable_cfg_set;
    bf_pm_interface.pltfm_post_port_enable_cfg_set =
        &bf_pm_post_port_enable_cfg_set;
    bf_pm_interface.pltfm_pre_port_disable_cfg_set =
        &bf_pm_pre_port_disable_cfg_set;
    bf_pm_interface.pltfm_post_port_disable_cfg_set =
        &bf_pm_post_port_disable_cfg_set;
    bf_pm_interface.pltfm_port_link_up_actions = &bf_pm_port_link_up_actions;
    bf_pm_interface.pltfm_port_link_down_actions =
        &bf_pm_port_link_down_actions;
    bf_pm_interface.pltfm_port_media_type_get =
        &bf_pm_pltfm_port_media_type_get;
    bf_pm_interface.pltfm_port_str_to_hdl_get = &bf_bd_cfg_port_str_to_handle;
    bf_pm_interface.pltfm_init = &bf_pm_cold_init;
    bf_pm_interface.pltfm_port_prbs_cfg_set = &bf_pm_prbs_cfg_set;
    bf_pm_interface.pltfm_ha_mode_set = &bf_pm_ha_mode_enable;
    bf_pm_interface.pltfm_ha_mode_clear = &bf_pm_ha_mode_disable;
    bf_pm_interface.pltfm_ha_link_state_notify = &bf_pm_ha_link_state_notify;
    bf_pm_interface.pltfm_deinit = 0;
    bf_pm_interface.pltfm_mac_to_multi_serdes_map_get =
        &bf_bd_cfg_mac_to_multi_serdes_map_get;
    bf_pm_interface.pltfm_port_presence_get = &bf_pm_pltfm_port_presence_get;

    // atexit(pltfm_mgr_exit); // not tested

    sts = bf_pal_pltfm_all_interface_set(&bf_pm_interface);
    if (sts != BF_SUCCESS) {
      LOG_ERROR("Unable to register pltfm interfaces for bf_pm");
    }
  }

  pltfm_mgr_init_done = true;
  /* listen to client commands and execute them */
  while (1) {
    usleep(10000); /* TBD : probably not required */
  }

  return 0;
}

/* Init function  */
void *bf_switchd_agent_init(void *arg) {
  if (!arg) return NULL;
  bf_switchd_context_t *switchd_ctx = (bf_switchd_context_t *)arg;
  LOG_DEBUG("Platform-mgr started \n");
  /* Register cleanup handler */
  pthread_cleanup_push(pltfm_mgr_exit_cleanup, "Platform-Mgr Cleanup handler");
  pltfm_mgr_init(switchd_ctx);
  pthread_cleanup_pop(0);
  return NULL;
}

void *bf_switchd_agent_init_done(void *arg) {
  if (!arg) return NULL;
  int *ret = (int *)arg;
  *ret = pltfm_mgr_init_done;
  return NULL;
}

#ifdef THRIFT_ENABLED
int agent_rpc_server_thrift_service_add(void *processor) {
  return bf_pltfm_agent_rpc_server_thrift_service_add(processor);
}

int agent_rpc_server_thrift_service_rmv(void *processor) {
  return bf_pltfm_agent_rpc_server_thrift_service_rmv(processor);
}
#endif  // THRIFT_ENABLED

#ifdef STATIC_LINK_LIB
int bf_pltfm_init_handlers_register(bf_switchd_context_t *ctx) {
  if (ctx == NULL) {
    LOG_ERROR("%s: invalid input", __func__);
  }

  ctx->bf_switchd_agent_init_fn = bf_switchd_agent_init;
  ctx->bf_switchd_agent_init_done_fn = bf_switchd_agent_init_done;
  ctx->bf_pltfm_device_type_get_fn = bf_pltfm_device_type_get;
#ifdef THRIFT_ENABLED
  ctx->agent_rpc_server_thrift_service_add_fn =
      agent_rpc_server_thrift_service_add;
  ctx->agent_rpc_server_thrift_service_rmv_fn =
      agent_rpc_server_thrift_service_rmv;
#endif  // THRIFT_ENABLED

  return 0;
}
#endif  // STATIC_LINK_LIB

#endif  // TCL_ONLY
