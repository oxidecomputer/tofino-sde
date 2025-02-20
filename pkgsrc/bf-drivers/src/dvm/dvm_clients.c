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
#include <sched.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <target-sys/bf_sal/bf_sys_intf.h>
#include <dvm/bf_drv_intf.h>
#include "dvm.h"
#include <dvm/dvm_log.h>
#include <port_mgr/bf_fsm_if.h>
#include <pipe_mgr/pipe_mgr_intf.h>
#include <traffic_mgr/traffic_mgr_port_intf.h>
#include <traffic_mgr/traffic_mgr_miscapi.h>

static bool bf_driver_client_db_init_done = false;
static int bf_drv_num_clients = 0;
bf_drv_hdl_info_t bf_drv_hdl_info[BF_DRV_MAX_CLIENTS];

bf_drv_client_t bf_driver_client_db[BF_DRV_MAX_CLIENTS];
bf_drv_app_t bf_app_client_db;
static bool skip_port_delta_push_for_reg_clients[BF_MAX_DEV_COUNT];

/* Init the DB */
void bf_driver_client_db_init() {
  memset(&bf_drv_hdl_info, 0, sizeof(bf_drv_hdl_info));
  memset(&bf_driver_client_db, 0, sizeof(bf_driver_client_db));
  memset(&bf_app_client_db, 0, sizeof(bf_app_client_db));
}

/* allocate an id for the client */
bf_status_t bf_drv_register(const char *client_name,
                            bf_drv_client_handle_t *client_handle) {
  int id = 0;

  if (bf_driver_client_db_init_done == false) {
    bf_driver_client_db_init();
    bf_driver_client_db_init_done = true;
  }

  memset(&skip_port_delta_push_for_reg_clients[0],
         0,
         sizeof(skip_port_delta_push_for_reg_clients));

  /* Allocate id from 1 */
  for (id = 1; id < BF_DRV_MAX_CLIENTS; id++) {
    if (bf_drv_hdl_info[id].allocated == true) {
      continue;
    }

    bf_drv_hdl_info[id].allocated = true;
    strncpy(bf_drv_hdl_info[id].client_name,
            client_name,
            BF_DRV_CLIENT_NAME_LEN - 1);
    bf_drv_hdl_info[id].client_name[BF_DRV_CLIENT_NAME_LEN - 1] = '\0';
    break;
  }

  if (id >= BF_DRV_MAX_CLIENTS) {
    *client_handle = -1;
    return BF_MAX_SESSIONS_EXCEEDED;
  }

  bf_drv_num_clients++;
  *client_handle = id;

  return BF_SUCCESS;
}

/* De-register the client */
bf_status_t bf_drv_deregister(bf_drv_client_handle_t client_handle) {
  int id = 0, i = 0;
  bf_drv_client_t *db_ptr;

  bf_sys_assert(client_handle >= 0);
  bf_sys_assert(client_handle < BF_DRV_MAX_CLIENTS);

  memset(&bf_drv_hdl_info[client_handle],
         0,
         sizeof(bf_drv_hdl_info[client_handle]));

  bf_drv_num_clients--;

  for (id = 0; id < BF_DRV_MAX_CLIENTS; id++) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if (db_ptr->client_handle == client_handle) {
      /* Move all entries up */
      for (i = id; i < (BF_DRV_MAX_CLIENTS - 1); i++) {
        memcpy(&bf_driver_client_db[i],
               &bf_driver_client_db[i + 1],
               sizeof(bf_drv_client_t));
      }
      /* Zero out last entry */
      db_ptr = &bf_driver_client_db[BF_DRV_MAX_CLIENTS - 1];
      memset(db_ptr, 0, sizeof(bf_drv_client_t));
      break;
    }
  }

  return BF_SUCCESS;
}

/* Master function to register callbacks */
bf_status_t bf_drv_client_register_callbacks(
    bf_drv_client_handle_t client_handle,
    bf_drv_client_callbacks_t *callbacks,
    bf_drv_client_prio_t add_priority) {
  bf_status_t status = 0;
  int id = 0, i = 0;
  bool shift = false;

  if (!callbacks) return BF_INVALID_ARG;

  bf_sys_assert(client_handle >= 0);
  bf_sys_assert(client_handle < BF_DRV_MAX_CLIENTS);
  bf_sys_assert(bf_drv_hdl_info[client_handle].allocated == true);

  for (id = 0; id < BF_DRV_MAX_CLIENTS; id++) {
    if (bf_driver_client_db[id].valid == false) {
      break;
    }
    if (bf_driver_client_db[id].client_handle == client_handle) {
      return status;
    }
    if (bf_driver_client_db[id].priority < add_priority) {
      shift = true;
      break;
    }
  }

  /* Make sure an empty space was found */
  if (id > (BF_DRV_MAX_CLIENTS - 1 - 1)) {
    bf_sys_assert(0);
    return BF_NO_SYS_RESOURCES;
  }

  if (shift == true) {
    for (i = BF_DRV_MAX_CLIENTS - 1 - 1; i >= id; i--) {
      memcpy(&bf_driver_client_db[i + 1],
             &bf_driver_client_db[i],
             sizeof(bf_drv_client_t));
    }
  }

  bf_driver_client_db[id] = (bf_drv_client_t){
      .valid = true,
      .client_handle = client_handle,
      .priority = add_priority,
      .override_fast_recfg = false,
      .issue_fast_recfg_port_cb = false,
      .callbacks = *callbacks,
  };

  strncpy(bf_driver_client_db[id].client_name,
          bf_drv_hdl_info[client_handle].client_name,
          BF_DRV_CLIENT_NAME_LEN - 1);
  bf_driver_client_db[id].client_name[BF_DRV_CLIENT_NAME_LEN - 1] = '\0';

  return status;
}

/* Function to register warm init flags */
bf_status_t bf_drv_client_register_warm_init_flags(
    bf_drv_client_handle_t client_handle,
    bool override_fast_recfg,
    bool issue_fast_recfg_port_cb) {
  bf_drv_client_t *db_ptr;
  int id;
  bf_sys_assert(client_handle >= 0);
  bf_sys_assert(client_handle < BF_DRV_MAX_CLIENTS);
  bf_sys_assert(bf_drv_hdl_info[client_handle].allocated == true);

  db_ptr = &bf_driver_client_db[0];

  for (id = 0; id < BF_DRV_MAX_CLIENTS; id++) {
    if (db_ptr->valid == false) {
      break;
    }
    if (db_ptr->client_handle == client_handle) {
      db_ptr->override_fast_recfg = override_fast_recfg;
      db_ptr->issue_fast_recfg_port_cb = issue_fast_recfg_port_cb;
      return BF_SUCCESS;
    }
    db_ptr++;
  }

  return BF_SUCCESS;
}

bf_status_t bf_port_client_register_status_notif(
    bf_drv_port_status_cb port_status, void *cookie) {
  bf_app_client_db.port_status = port_status;
  bf_app_client_db.port_status_cookie = cookie;
  return BF_SUCCESS;
}

bf_status_t bf_port_client_register_speed_notif(bf_drv_port_speed_cb port_speed,
                                                void *cookie) {
  bf_app_client_db.port_speed = port_speed;
  bf_app_client_db.port_speed_cookie = cookie;
  return BF_SUCCESS;
}

bf_status_t bf_port_client_register_mode_change_notif(
    bf_dev_id_t dev_id,
    bf_drv_port_mode_change_cb port_mode_change,
    void *port_mode_change_cookie,
    bf_drv_port_mode_change_complete_cb port_mode_change_complete,
    void *port_mode_change_complete_cookie) {
  bf_dev_family_t dev_family;
  bf_status_t status;

  status = bf_device_family_get(dev_id, &dev_family);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Unable to get the device family for dev %d, sts %s (%d)",
              dev_id,
              bf_err_str(status),
              status);
    return status;
  }

  if (dev_family == BF_DEV_FAMILY_TOFINO) {
    bf_app_client_db.port_mode_change = port_mode_change;
    bf_app_client_db.port_mode_change_cookie = port_mode_change_cookie;
    bf_app_client_db.port_mode_change_complete = port_mode_change_complete;
    bf_app_client_db.port_mode_change_complete_cookie =
        port_mode_change_complete_cookie;
    return BF_SUCCESS;
  } else {
    return BF_INVALID_ARG;
  }
}

bool bf_drv_client_port_mode_change_callback_registered(bf_dev_id_t dev_id) {
  (void)dev_id;

  if (bf_app_client_db.port_mode_change) {
    return true;
  } else {
    return false;
  }
}

bf_status_t bf_drv_notify_clients_pktmgr_dev_add(
    bf_dev_id_t dev_id,
    bf_dev_family_t dev_family,
    bf_dma_info_t *dma_info,
    bf_dev_init_mode_t warm_init_mode) {
  int id = 0;
  bf_drv_client_t *db_ptr = NULL;
  bf_status_t status = BF_SUCCESS;

  for (id = 0; id < BF_DRV_MAX_CLIENTS; id++) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if (db_ptr->callbacks.pkt_mgr_dev_add) {
      status = db_ptr->callbacks.pkt_mgr_dev_add(
          dev_id, dev_family, dma_info, warm_init_mode);
      if (status != BF_SUCCESS) {
        LOG_ERROR(
            "Pktmgr device add handling failed for dev %d, sts %s (%d),"
            " Client %s ",
            dev_id,
            bf_err_str(status),
            status,
            db_ptr->client_name);
        return status;
      }
    }
  }

  return status;
}

bf_status_t bf_drv_notify_clients_dev_add(bf_dev_id_t dev_id,
                                          bf_dev_family_t dev_family,
                                          bf_device_profile_t *profile,
                                          bf_dma_info_t *dma_info,
                                          bf_dev_init_mode_t warm_init_mode) {
  int id = 0;
  bf_drv_client_t *db_ptr = NULL;
  bf_status_t status = BF_SUCCESS;

  for (id = 0; id < BF_DRV_MAX_CLIENTS; id++) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if (db_ptr->callbacks.device_add) {
      status = db_ptr->callbacks.device_add(
          dev_id, dev_family, profile, dma_info, warm_init_mode);
      if (status != BF_SUCCESS) {
        LOG_ERROR(
            "Device add handling failed for dev %d, sts %s (%d),"
            " Client %s ",
            dev_id,
            bf_err_str(status),
            status,
            db_ptr->client_name);
        return status;
      }
    }
  }

  return status;
}

bf_status_t bf_drv_notify_clients_virtual_dev_add(
    bf_dev_id_t dev_id,
    bf_dev_type_t dev_type,
    bf_device_profile_t *profile,
    bf_dev_init_mode_t warm_init_mode) {
  int id = 0;
  bf_drv_client_t *db_ptr = NULL;
  bf_status_t status = BF_SUCCESS;

  for (id = 0; id < BF_DRV_MAX_CLIENTS; id++) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if (db_ptr->callbacks.virtual_device_add) {
      status = db_ptr->callbacks.virtual_device_add(
          dev_id, dev_type, profile, warm_init_mode);
      if (status != BF_SUCCESS) {
        LOG_ERROR(
            "Virtual device add handling failed for dev %d, sts %s (%d),"
            " Client %s ",
            dev_id,
            bf_err_str(status),
            status,
            db_ptr->client_name);
        return status;
      }
    }
  }

  return status;
}

bf_status_t bf_drv_notify_clients_dev_type_virtual_dev_slave(
    bf_dev_id_t dev_id) {
  int id = 0;
  bf_drv_client_t *db_ptr = NULL;
  bf_status_t status = BF_SUCCESS;

  for (id = BF_DRV_MAX_CLIENTS - 1; id >= 0; id--) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if (db_ptr->callbacks.device_mode_virtual_dev_slave) {
      bf_status_t client_status =
          db_ptr->callbacks.device_mode_virtual_dev_slave(dev_id);
      if (client_status != BF_SUCCESS) {
        LOG_ERROR(
            "Device mode handling failed for dev %d, sts %s (%d), Client %s",
            dev_id,
            bf_err_str(client_status),
            client_status,
            db_ptr->client_name);
        if (status == BF_SUCCESS) {
          status = client_status;
        }
      }
    }
  }
  return status;
}

bf_status_t bf_drv_notify_clients_dev_del(bf_dev_id_t dev_id, bool log_err) {
  int id = 0;
  bf_drv_client_t *db_ptr = NULL;
  bf_status_t status = BF_SUCCESS;

  for (id = BF_DRV_MAX_CLIENTS - 1; id >= 0; id--) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if (db_ptr->callbacks.device_del) {
      bf_status_t client_status = db_ptr->callbacks.device_del(dev_id);
      if (client_status != BF_SUCCESS) {
        /* Don't log error if this is a cleanup being performed due
           to an earlier add failure
        */
        if (log_err) {
          LOG_ERROR(
              "Device del handling failed for dev %d,"
              "sts %s (%d), Client %s ",
              dev_id,
              bf_err_str(client_status),
              client_status,
              db_ptr->client_name);
        }
        if (status == BF_SUCCESS) {
          status = client_status;
        }
      }
    }
  }

  return status;
}

bf_status_t bf_drv_notify_clients_dev_log(bf_dev_id_t dev_id,
                                          const char *filepath) {
  int id = 0;
  bf_drv_client_t *db_ptr = NULL;
  bf_status_t status = BF_SUCCESS;
  cJSON *dev, *clients, *client;
  FILE *logfile;
  char *output_str;

  dev = cJSON_CreateObject();
  cJSON_AddItemToObject(dev, "clients", clients = cJSON_CreateArray());

  for (id = 0; id < BF_DRV_MAX_CLIENTS; id++) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if (db_ptr->callbacks.device_log) {
      cJSON_AddItemToArray(clients, client = cJSON_CreateObject());
      cJSON_AddNumberToObject(client, "client_id", id);
      cJSON_AddStringToObject(client, "name", db_ptr->client_name);
      bf_status_t client_status = db_ptr->callbacks.device_log(dev_id, client);
      if (client_status != BF_SUCCESS) {
        LOG_ERROR(
            "Device log handling failed for dev %d,"
            "sts %s (%d), Client %s ",
            dev_id,
            bf_err_str(client_status),
            client_status,
            db_ptr->client_name);
        if (status == BF_SUCCESS) {
          status = client_status;
        }
      }
    }
  }

  if (status == BF_SUCCESS) {
    logfile = fopen(filepath, "w");
    if (!logfile) {
      status = BF_OBJECT_NOT_FOUND;
    } else {
      output_str = cJSON_Print(dev);
      fputs(output_str, logfile);
      bf_sys_free(output_str);
      fclose(logfile);
    }
  }
  cJSON_Delete(dev);
  return status;
}

bf_status_t bf_drv_notify_clients_dev_restore(bf_dev_id_t dev_id,
                                              const char *filepath) {
  FILE *logfile;
  struct stat logfile_stat;
  char *file_str = NULL;
  cJSON *dev, *clients, *client;
  int num_clients = 0;
  int client_idx = 0;
  int id = 0;
  bf_drv_client_t *db_ptr = NULL;
  bf_status_t status = BF_SUCCESS;

  logfile = fopen(filepath, "r");
  if (!logfile) {
    LOG_ERROR("Failed to open logfile %s", filepath);
    return BF_OBJECT_NOT_FOUND;
  }

  if (stat(filepath, &logfile_stat) == -1) {
    LOG_ERROR("Failed to retrieve stats for logfile %s", filepath);
    status = BF_OBJECT_NOT_FOUND;
    goto cleanup;
  }

  file_str = bf_sys_calloc(logfile_stat.st_size + 1, 1);
  if (!file_str) {
    LOG_ERROR("Malloc failed for device %d restore", dev_id);
    status = BF_NO_SYS_RESOURCES;
    goto cleanup;
  }
  if (!fread(file_str, logfile_stat.st_size, 1, logfile)) {
    LOG_ERROR("Failed to read logfile %s", filepath);
    status = BF_UNEXPECTED;
    goto cleanup;
  }
  file_str[logfile_stat.st_size] = '\0';
  LOG_ERROR("reading file = %s", filepath);
  fclose(logfile);

  dev = cJSON_Parse(file_str);
  clients = cJSON_GetObjectItem(dev, "clients");
  num_clients = cJSON_GetArraySize(clients);
  for (client_idx = 0; client_idx < num_clients; client_idx++) {
    client = cJSON_GetArrayItem(clients, client_idx);
    for (id = 0; id < BF_DRV_MAX_CLIENTS; id++) {
      db_ptr = &bf_driver_client_db[id];
      if (strcmp(db_ptr->client_name,
                 cJSON_GetObjectItem(client, "name")->valuestring) == 0) {
        bf_sys_assert(id == cJSON_GetObjectItem(client, "client_id")->valueint);
        if (!db_ptr->callbacks.device_restore) {
          break;
        }
        bf_status_t client_status =
            db_ptr->callbacks.device_restore(dev_id, client);
        if (client_status != BF_SUCCESS) {
          LOG_ERROR(
              "Device restore handling failed for dev %d,"
              "sts %s (%d), Client %s ",
              dev_id,
              bf_err_str(client_status),
              client_status,
              db_ptr->client_name);
          if (status == BF_SUCCESS) {
            status = client_status;
          }
        }
      }
    }
  }

  cJSON_Delete(dev);
  bf_sys_free(file_str);
  return status;

cleanup:
  if (file_str) bf_sys_free(file_str);
  fclose(logfile);
  return status;
}

bf_status_t bf_drv_notify_clients_port_add(bf_dev_id_t dev_id,
                                           bf_dev_port_t port_id,
                                           bf_port_speeds_t port_speed,
                                           uint32_t n_lanes,
                                           bf_fec_types_t port_fec_type) {
  int id = 0;
  bf_drv_client_t *db_ptr = NULL;
  bf_status_t status = BF_SUCCESS;
  bf_port_attributes_t port_attrib;
  bf_dev_init_mode_t warm_init_mode;

  status = bf_device_init_mode_get(dev_id, &warm_init_mode);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Unable to get the device mode for dev %d, sts %s (%d)",
              dev_id,
              bf_err_str(status),
              status);
    LOG_ERROR(
        "Considering the device mode as COLD START for port add for dev %d "
        "port %d",
        dev_id,
        port_id);
    warm_init_mode = BF_DEV_INIT_COLD;
  }

  /*
   *     Port Addition Sequence
   *    _________________________
   *
   *  Egress Configuration order is
   *     1. Disable flush in MAC, Enable MAC channel, ..  --> Notify LLD first
   *     2. Enable Egress Parser Buffer    --> Notify pipe-mgr module
   *     3. Enable TX queue in TM ---> Notify TM module next
   *
   *  Ingress Configuration order is
   *     1. Enable Parser Arbiter, Enable Ingress Parser Buffer --> Notify
   *pipe-mgr module
   *     2. Undo force clear pause state --> Notify LLD
   *
   *
   *  Since the callback list are maintained such that highest prio is first in
   *the list,
   *  Egress pipe is setup/configured first according to the registered
   *priority.
   *  However ingress pipe is setup/configured in reverse order of priority.
   */

  // Notify module by invoking callback.
  // Egress pipe setup
  for (id = 0; id < BF_DRV_MAX_CLIENTS; id++) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if ((warm_init_mode == BF_DEV_WARM_INIT_FAST_RECFG) ||
        (warm_init_mode == BF_DEV_WARM_INIT_FAST_RECFG_QUICK)) {
      if (skip_port_delta_push_for_reg_clients[dev_id]) {
        if (!db_ptr->issue_fast_recfg_port_cb) {
          continue;
        }
      }
    }
    if (db_ptr->callbacks.port_add) {
      port_attrib.port_speeds = port_speed;
      port_attrib.n_lanes = n_lanes;
      port_attrib.port_fec_types = port_fec_type;
      status = db_ptr->callbacks.port_add(
          dev_id, port_id, &port_attrib, BF_PORT_CB_DIRECTION_EGRESS);
      if (status != BF_SUCCESS) {
        LOG_ERROR(
            "Port add (eg) failed for dev %d, port %d, sts %s (%d), "
            "Client %s ",
            dev_id,
            port_id,
            bf_err_str(status),
            status,
            db_ptr->client_name);
        return status;
      }
      bf_sys_usleep(100);
    }
  }
  // Ingress pipe setup
  for (id = BF_DRV_MAX_CLIENTS - 1; id >= 0; id--) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if ((warm_init_mode == BF_DEV_WARM_INIT_FAST_RECFG) ||
        (warm_init_mode == BF_DEV_WARM_INIT_FAST_RECFG_QUICK)) {
      if (skip_port_delta_push_for_reg_clients[dev_id]) {
        if (!db_ptr->issue_fast_recfg_port_cb) {
          continue;
        }
      }
    }
    if (db_ptr->callbacks.port_add) {
      port_attrib.port_speeds = port_speed;
      port_attrib.n_lanes = n_lanes;
      port_attrib.port_fec_types = port_fec_type;
      status = db_ptr->callbacks.port_add(
          dev_id, port_id, &port_attrib, BF_PORT_CB_DIRECTION_INGRESS);
      if (status != BF_SUCCESS) {
        LOG_ERROR(
            "Port add(ig) handling failed for dev %d, port %d,"
            " sts %s (%d), Client %s ",
            dev_id,
            port_id,
            bf_err_str(status),
            status,
            db_ptr->client_name);
        return status;
      }
      bf_sys_usleep(100);
    }
  }

  return status;
}

#define PORT_MODE_TRANSITION_WA_NUM_PKTS 6
#define PORT_MODE_TRANSITION_WA_PKT_SIZE 24
bf_status_t bf_drv_complete_port_mode_transition_wa(
    bf_dev_id_t dev_id, bf_dev_port_t port_id, bf_port_speeds_t port_speed) {
  bf_status_t sts = BF_SUCCESS, rc = BF_SUCCESS;

  /* Check if port mode transition workaround needs to be applied */
  if (bf_drv_check_port_mode_transition_wa(dev_id, port_id, port_speed)) {
    bool is_sw_model = true;
    bf_drv_device_type_get(dev_id, &is_sw_model);

    /* Make sure all completion callbacks are received for pipe mgr
     * internal session */
    sts = pipe_mgr_complete_operations(0);
    if (rc == BF_SUCCESS) rc = sts;

    /* Enable qac_rx to send the packets out of TM */
    sts = bf_tm_port_set_qac_rx(dev_id, port_id, true);
    if (rc == BF_SUCCESS) rc = sts;
    sts = bf_tm_complete_operations(dev_id);
    if (rc == BF_SUCCESS) rc = sts;

    /* Get the count of packets to that port before the client injects the
     * workaround packets. */
    uint64_t start_pkt_count = 0;
    sts = bf_pipe_mgr_port_ebuf_counter_get(dev_id, port_id, &start_pkt_count);
    if (rc == BF_SUCCESS) rc = sts;

    /* Invoke the aplication registered callback to send workaround packets */
    LOG_DBG(
        "%s: Calling port mode transition callback for dev %d, port %d, port "
        "speed %d",
        __func__,
        dev_id,
        port_id,
        port_speed);
    sts = bf_app_client_db.port_mode_change(
        dev_id,
        port_id,
        PORT_MODE_TRANSITION_WA_NUM_PKTS,
        PORT_MODE_TRANSITION_WA_PKT_SIZE,
        bf_app_client_db.port_mode_change_cookie);
    if (rc == BF_SUCCESS) rc = sts;

    /* Wait for the packets sent by the client to forward to the port.  We'll
     * poll an EBuf counter for this port to wait for the six packets injected
     * by the client to forward to the port.  We add a timeout so this doesn't
     * become an infinite loop in case of error.  On HW a short timeout of one
     * second is enough while the model may run much slower so we wait 10
     * seconds. */
    uint64_t pkt_count = 0;
    uint64_t required_count =
        PORT_MODE_TRANSITION_WA_NUM_PKTS + start_pkt_count;
    int I = is_sw_model ? 10000 : 1000;
    for (int i = 0; i < I; ++i) {
      sts = bf_pipe_mgr_port_ebuf_counter_get(dev_id, port_id, &pkt_count);
      if (sts != BF_SUCCESS) {
        if (rc == BF_SUCCESS) rc = sts;
        break;
      }
      if (pkt_count >= required_count) break;
      /* Wait 1000 usec or 1ms.  Since the loop will run 7000 times max this
       * gives a 7 second timeout for the packets which should be enough even
       * on the model. */
      bf_sys_usleep(1000);
    }
    if (pkt_count < required_count) {
      sts = PIPE_UNEXPECTED;
      if (rc == BF_SUCCESS) rc = sts;
    }

    /* Revert the qac_rx state to disable */
    sts = bf_tm_port_set_qac_rx(dev_id, port_id, false);
    if (rc == BF_SUCCESS) rc = sts;
    sts = bf_tm_complete_operations(dev_id);
    if (rc == BF_SUCCESS) rc = sts;

    /* Complete parde configs */
    sts = pipe_mgr_complete_port_mode_transition_wa(dev_id, port_id);
    if (rc == BF_SUCCESS) rc = sts;

    /* Make sure all completion callbacks are received for pipe mgr
     * internal session */
    sts = pipe_mgr_complete_operations(0);
    if (rc == BF_SUCCESS) rc = sts;

    if (bf_app_client_db.port_mode_change_complete) {
      bf_app_client_db.port_mode_change_complete(
          dev_id, port_id, bf_app_client_db.port_mode_change_complete_cookie);
    }
  }

  return rc;
}

bf_status_t bf_drv_notify_clients_port_del(bf_dev_id_t dev_id,
                                           bf_dev_port_t port_id,
                                           bool log_err) {
  int id = 0;
  bf_drv_client_t *db_ptr = NULL;
  bf_status_t status = BF_SUCCESS;
  bf_dev_init_mode_t warm_init_mode;

  status = bf_device_init_mode_get(dev_id, &warm_init_mode);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Unable to get the device mode for dev %d, sts %s (%d)",
              dev_id,
              bf_err_str(status),
              status);
    LOG_ERROR(
        "Considering the device mode as COLD START for port del for dev %d "
        "port %d",
        dev_id,
        port_id);
    warm_init_mode = BF_DEV_INIT_COLD;
    status = BF_SUCCESS;
  }
  /*
   *     Port Deletion Sequence
   *    _________________________
   *
   *  Ingress Configuration order is
   *     1. Execute port disable sequence, Enable MAC flush -- > Notify LLD
   *     2. Disable Ingress Buffer, Disable parser arb -- > Notify pipe-mgr
   *     3. Check for outstanding packets in TM --> Notify TM
   *
   *  Egress Configuration order is
   *     1. Disable packet queueing to port --> Notify TM
   *     2. Disable Egress Parser Buffer --> Notify Pipe-mgr
   *     3. Disable MAC -- > Notify LLD
   *
   *
   *  Since the callback list are maintained such that highest prio is first in
   *the list,
   *  Inress pipe is setup/configured first according to the registered
   *priority.
   *  However Egress pipe is setup/configured in reverse order of priority.
   *
   *  Current Order:
   *  Ingress
   *   - LLD
   *   - port_mgr
   *   - pipe_mgr
   *   - pkt_mgr
   *   - pd
   *   - traffic_mgr
   *   - bf-pm
   *   - mc_mgr
   *  Egress
   *   - mc_mgr
   *   - bf-pm
   *   - traffic_mgr
   *   - pd
   *   - pkt_mgr
   *   - pipe_mgr
   *   - port_mgr
   *   - LLD
   */
  for (id = 0; id < BF_DRV_MAX_CLIENTS; id++) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if ((warm_init_mode == BF_DEV_WARM_INIT_FAST_RECFG) ||
        (warm_init_mode == BF_DEV_WARM_INIT_FAST_RECFG_QUICK)) {
      if (skip_port_delta_push_for_reg_clients[dev_id]) {
        if (!db_ptr->issue_fast_recfg_port_cb) {
          continue;
        }
      }
    }
    if (db_ptr->callbacks.port_del) {
      bf_status_t client_status = db_ptr->callbacks.port_del(
          dev_id, port_id, BF_PORT_CB_DIRECTION_INGRESS);
      if (client_status != BF_SUCCESS) {
        /* Don't log error if this is a cleanup being performed due
           to an earlier add failure
        */
        if (log_err) {
          LOG_ERROR(
              "Port del(ig) handling failed for dev %d,"
              " port %d, sts %s (%d), Client %s ",
              dev_id,
              port_id,
              bf_err_str(client_status),
              client_status,
              db_ptr->client_name);
        }
        if (status == BF_SUCCESS) {
          status = client_status;
        }
      }
      bf_sys_usleep(100);
    }
  }

  for (id = BF_DRV_MAX_CLIENTS - 1; id >= 0; id--) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if ((warm_init_mode == BF_DEV_WARM_INIT_FAST_RECFG) ||
        (warm_init_mode == BF_DEV_WARM_INIT_FAST_RECFG_QUICK)) {
      if (skip_port_delta_push_for_reg_clients[dev_id]) {
        if (!db_ptr->issue_fast_recfg_port_cb) {
          continue;
        }
      }
    }
    if (db_ptr->callbacks.port_del) {
      bf_status_t client_status = db_ptr->callbacks.port_del(
          dev_id, port_id, BF_PORT_CB_DIRECTION_EGRESS);
      if (client_status != BF_SUCCESS) {
        /* Don't log error if this is a cleanup being performed due
           to an earlier add failure
        */
        if (log_err) {
          LOG_ERROR(
              "Port del(eg) handling failed for dev %d, port %d,"
              " sts %s (%d),  Client %s ",
              dev_id,
              port_id,
              bf_err_str(client_status),
              client_status,
              db_ptr->client_name);
        }
        if (status == BF_SUCCESS) {
          status = client_status;
        }
      }
      bf_sys_usleep(100);
    }
  }

  return status;
}

void bf_drv_notify_clients_port_status_chg(bf_dev_id_t dev_id,
                                           bf_dev_port_t port_id,
                                           port_mgr_port_event_t event,
                                           uint32_t event_data,
                                           void *userdata) {
  int id = 0;
  bf_drv_client_t *db_ptr = NULL;
  bf_status_t client_status = BF_SUCCESS;
  bool status_event = false, speed_event = false, fsm_event = false;
  bool port_up = false;
  bf_port_speed_t speed = 0;
  bf_fsm_st fsm_state;

  (void)userdata;
  if (event == PORT_MGR_PORT_EVT_UP) {
    status_event = true;
    port_up = true;
  } else if (event == PORT_MGR_PORT_EVT_DOWN) {
    status_event = true;
    port_up = false;
  } else if (event == PORT_MGR_PORT_EVT_SPEED_SET) {
    speed_event = true;
    /* Get the speed from LLD */
    speed = 0;
  } else if (event == PORT_MGR_PORT_FSM_TRANSITION) {
    fsm_event = true;
    fsm_state = (bf_fsm_st) event_data;
  }

  for (id = BF_DRV_MAX_CLIENTS - 1; id >= 0; id--) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if ((status_event) && (db_ptr->callbacks.port_status)) {
      client_status = db_ptr->callbacks.port_status(dev_id, port_id, port_up);
      if (client_status != BF_SUCCESS) {
        LOG_ERROR(
            "Port status handing failed for dev %d, port %d,"
            " sts %s (%d), Client %s ",
            dev_id,
            port_id,
            bf_err_str(client_status),
            client_status,
            db_ptr->client_name);
      }
    }
    if ((speed_event) && (db_ptr->callbacks.port_speed)) {
      client_status = db_ptr->callbacks.port_speed(dev_id, port_id);
      if (client_status != BF_SUCCESS) {
        LOG_ERROR(
            "Port speed handing failed for dev %d, port %d,"
            " sts %s (%d), Client %s ",
            dev_id,
            port_id,
            bf_err_str(client_status),
            client_status,
            db_ptr->client_name);
      }
    } else if ((fsm_event) && (db_ptr->callbacks.port_fsm)) {
      client_status = db_ptr->callbacks.port_fsm(dev_id, port_id, fsm_state);
      if (client_status != BF_SUCCESS) {
        LOG_ERROR(
            "Port FSM handling failed for dev %d, port %d,"
            " sts %s (%d), Client %s ",
            dev_id,
            port_id,
            bf_err_str(client_status),
            client_status,
            db_ptr->client_name);
      }
    }
  }

  /* Notify app also */
  if ((status_event) && (bf_app_client_db.port_status)) {
    bf_app_client_db.port_status(
        dev_id, port_id, port_up, bf_app_client_db.port_status_cookie);
  }
  if ((speed_event) && (bf_app_client_db.port_speed)) {
    bf_app_client_db.port_speed(
        dev_id, port_id, speed, bf_app_client_db.port_speed_cookie);
  }

  return;
}

bf_status_t bf_drv_notify_clients_port_serdes_upgrade(
    bf_dev_id_t dev_id,
    bf_dev_port_t port_id,
    uint32_t serdes_fw_version,
    char *serdes_fw_path) {
  int id = 0;
  bf_drv_client_t *db_ptr = NULL;
  bf_status_t status = BF_SUCCESS;
  bf_dev_init_mode_t warm_init_mode;

  status = bf_device_init_mode_get(dev_id, &warm_init_mode);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Unable to get the device mode for dev %d, sts %s (%d)",
              dev_id,
              bf_err_str(status),
              status);
    LOG_ERROR(
        "Considering the device mode as COLD START for port admin state for "
        "dev %d port %d",
        dev_id,
        port_id);
    warm_init_mode = BF_DEV_INIT_COLD;
  }

  for (id = 0; id < BF_DRV_MAX_CLIENTS; id++) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if ((warm_init_mode == BF_DEV_WARM_INIT_FAST_RECFG) ||
        (warm_init_mode == BF_DEV_WARM_INIT_FAST_RECFG_QUICK)) {
      if (!db_ptr->issue_fast_recfg_port_cb) {
        continue;
      }
    }
    if (db_ptr->callbacks.port_serdes_upgrade) {
      status = db_ptr->callbacks.port_serdes_upgrade(
          dev_id, port_id, serdes_fw_version, serdes_fw_path);
      if (status != BF_SUCCESS) {
        LOG_ERROR(
            "Port serdes upgrade notify failed for dev %d, port %d, sts %s "
            "(%d), "
            "Client %s ",
            dev_id,
            port_id,
            bf_err_str(status),
            status,
            db_ptr->client_name);
        return status;
      }
      bf_sys_usleep(100);
    }
  }

  return status;
}

bf_status_t bf_drv_notify_clients_port_admin_state(bf_dev_id_t dev_id,
                                                   bf_dev_port_t port_id,
                                                   bool enable) {
  int id = 0;
  bf_drv_client_t *db_ptr = NULL;
  bf_status_t client_status = BF_SUCCESS, status = BF_SUCCESS;
  bf_dev_init_mode_t warm_init_mode;

  status = bf_device_init_mode_get(dev_id, &warm_init_mode);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Unable to get the device mode for dev %d, sts %s (%d)",
              dev_id,
              bf_err_str(status),
              status);
    LOG_ERROR(
        "Considering the device mode as COLD START for port admin state for "
        "dev %d port %d",
        dev_id,
        port_id);
    warm_init_mode = BF_DEV_INIT_COLD;
    status = BF_SUCCESS;
  }

  if (enable) {
    for (id = 0; id < BF_DRV_MAX_CLIENTS; id++) {
      db_ptr = &bf_driver_client_db[id];
      if (db_ptr->valid == false) {
        continue;
      }
      if ((warm_init_mode == BF_DEV_WARM_INIT_FAST_RECFG) ||
          (warm_init_mode == BF_DEV_WARM_INIT_FAST_RECFG_QUICK)) {
        if (skip_port_delta_push_for_reg_clients[dev_id]) {
          if (!db_ptr->issue_fast_recfg_port_cb) {
            continue;
          }
        }
      }
      if (db_ptr->callbacks.port_admin_state) {
        client_status =
            db_ptr->callbacks.port_admin_state(dev_id, port_id, enable);
        if (client_status != BF_SUCCESS) {
          LOG_ERROR(
              "Port admin chg handing failed for dev %d,"
              " port %d, sts %s (%d), Client %s ",
              dev_id,
              port_id,
              bf_err_str(client_status),
              client_status,
              db_ptr->client_name);
          if (status == BF_SUCCESS) {
            status = client_status;
          }
        }
      }
    }
  } else {
    for (id = BF_DRV_MAX_CLIENTS - 1; id >= 0; id--) {
      db_ptr = &bf_driver_client_db[id];
      if (db_ptr->valid == false) {
        continue;
      }
      if ((warm_init_mode == BF_DEV_WARM_INIT_FAST_RECFG) ||
          (warm_init_mode == BF_DEV_WARM_INIT_FAST_RECFG_QUICK)) {
        if (skip_port_delta_push_for_reg_clients[dev_id]) {
          if (!db_ptr->issue_fast_recfg_port_cb) {
            continue;
          }
        }
      }
      if (db_ptr->callbacks.port_admin_state) {
        client_status =
            db_ptr->callbacks.port_admin_state(dev_id, port_id, enable);
        if (client_status != BF_SUCCESS) {
          LOG_ERROR(
              "Port admin chg handing failed for dev %d,"
              " port %d, sts %s (%d), Client %s ",
              dev_id,
              port_id,
              bf_err_str(client_status),
              client_status,
              db_ptr->client_name);
          if (status == BF_SUCCESS) {
            status = client_status;
          }
        }
      }
    }
  }

  return status;
}

bf_status_t bf_drv_skip_port_delta_push_set(bf_dev_id_t dev_id, bool val) {
  if (dev_id >= BF_MAX_DEV_COUNT) return BF_INVALID_ARG;
  skip_port_delta_push_for_reg_clients[dev_id] = val;
  return BF_SUCCESS;
}

static bf_status_t bf_drv_reconfg_callbacks(bf_dev_id_t dev_id,
                                            reconfig_params_s *params) {
  int id = 0;
  bf_drv_client_t *db_ptr = NULL;
  bf_status_t status = BF_SUCCESS, full_status = BF_SUCCESS;

  for (id = 0; id < BF_DRV_MAX_CLIENTS; id++) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    // RECONFIG_LOCK
    if (params->lock && db_ptr->callbacks.lock) {
      LOG_TRACE("Reconfig step LOCK,       start, dev %d client %s",
                dev_id,
                db_ptr->client_name);
      status = db_ptr->callbacks.lock(dev_id);
      LOG_TRACE("Reconfig step LOCK,       done,  dev %d client %s",
                dev_id,
                db_ptr->client_name);
    }
    // WARM_INIT_QUICK
    if (params->warm_init_quick && db_ptr->callbacks.warm_init_quick) {
      LOG_TRACE("Reconfig step WARM_INIT_QUICK, start, dev %d client %s",
                dev_id,
                db_ptr->client_name);
      status = db_ptr->callbacks.warm_init_quick(dev_id);
      LOG_TRACE("Reconfig step WARM_INIT_QUICK, done,  dev %d client %s",
                dev_id,
                db_ptr->client_name);
    }
    // RECONFIG_UNLOCK
    if (params->wait_for_swcfg_replay_end) {
      if (db_ptr->callbacks.wait_for_swcfg_replay_end) {
        LOG_TRACE(
            "Reconfig step WAIT_FOR_SWCFG_REPLAY_END, start, dev %d client %s",
            dev_id,
            db_ptr->client_name);
        status = db_ptr->callbacks.wait_for_swcfg_replay_end(dev_id);
        LOG_TRACE(
            "Reconfig step WAIT_FOR_SWCFG_REPLAY_END, done,  dev %d client %s",
            dev_id,
            db_ptr->client_name);
      }
    }
    if (params->create_dma) {
      if (db_ptr->override_fast_recfg &&
          db_ptr->callbacks.complete_hitless_hw_read) {
        LOG_TRACE(
            "Mapped Reconfig step CREATE_DMA to Hitless HA step HW Read step, "
            "start, dev %d client %s",
            dev_id,
            db_ptr->client_name);
        status = db_ptr->callbacks.complete_hitless_hw_read(dev_id);
        LOG_TRACE(
            "Mapped Reconfig step CREATE_DMA to Hitless HA step HW Read step, "
            "done, dev %d client %s",
            dev_id,
            db_ptr->client_name);
      } else if (db_ptr->callbacks.create_dma) {
        LOG_TRACE("Reconfig step CREATE_DMA, start, dev %d client %s",
                  dev_id,
                  db_ptr->client_name);
        status = db_ptr->callbacks.create_dma(dev_id);
        LOG_TRACE("Reconfig step CREATE_DMA, done,  dev %d client %s",
                  dev_id,
                  db_ptr->client_name);
      }
    }
    if (params->disable_input_pkts) {
      if (db_ptr->override_fast_recfg &&
          db_ptr->callbacks.compute_delta_changes) {
        LOG_TRACE(
            "Mapped Reconfig step PAUSE_PKTS to Hitless HA step Compute Delta, "
            "start, dev %d client %s",
            dev_id,
            db_ptr->client_name);
        status = db_ptr->callbacks.compute_delta_changes(dev_id, true);
        LOG_TRACE(
            "Mapped Reconfig step PAUSE_PKTS to Hitless HA step Compute Delta, "
            "done, dev %d client %s",
            dev_id,
            db_ptr->client_name);
      } else if (db_ptr->callbacks.disable_input_pkts) {
        LOG_TRACE("Reconfig step PAUSE_PKTS, start, dev %d client %s",
                  dev_id,
                  db_ptr->client_name);
        status = db_ptr->callbacks.disable_input_pkts(dev_id);
        LOG_TRACE("Reconfig step PAUSE_PKTS, done,  dev %d client %s",
                  dev_id,
                  db_ptr->client_name);
      }
    }
    if (params->wait_for_flush) {
      if (db_ptr->override_fast_recfg && db_ptr->callbacks.push_delta_changes) {
        LOG_TRACE(
            "Mapped Reconfig step FLUSH to Hitless HA step Push Delta, start, "
            "dev %d client %s",
            dev_id,
            db_ptr->client_name);
        status = db_ptr->callbacks.push_delta_changes(dev_id);
        LOG_TRACE(
            "Mapped Reconfig step FLUSH to Hitless HA step Push Delta, done, "
            "dev %d client %s",
            dev_id,
            db_ptr->client_name);
      } else if (db_ptr->callbacks.wait_for_flush) {
        LOG_TRACE("Reconfig step FLUSH,      start, dev %d client %s",
                  dev_id,
                  db_ptr->client_name);
        status = db_ptr->callbacks.wait_for_flush(dev_id);
        LOG_TRACE("Reconfig step FLUSH,      done,  dev %d client %s",
                  dev_id,
                  db_ptr->client_name);
      }
    }
    if (params->core_reset && db_ptr->callbacks.core_reset) {
      LOG_TRACE("Reconfig step CORE_RESET, start, dev %d client %s",
                dev_id,
                db_ptr->client_name);
      status = db_ptr->callbacks.core_reset(dev_id);
      LOG_TRACE("Reconfig step CORE_RESET, done,  dev %d client %s",
                dev_id,
                db_ptr->client_name);
    }
    if (params->unlock_reprogram_core &&
        db_ptr->callbacks.unlock_reprogram_core) {
      LOG_TRACE("Reconfig step UNLOCK,     start, dev %d client %s",
                dev_id,
                db_ptr->client_name);
      status = db_ptr->callbacks.unlock_reprogram_core(dev_id);
      LOG_TRACE("Reconfig step UNLOCK,     done,  dev %d client %s",
                dev_id,
                db_ptr->client_name);
    }
    if (params->config_complete && db_ptr->callbacks.config_complete) {
      LOG_TRACE("Reconfig step COMPLETE,   start, dev %d client %s",
                dev_id,
                db_ptr->client_name);
      status = db_ptr->callbacks.config_complete(dev_id);
      LOG_TRACE("Reconfig step COMPLETE,   done,  dev %d client %s",
                dev_id,
                db_ptr->client_name);
    }
    if (params->enable_input_pkts && db_ptr->callbacks.enable_input_pkts) {
      LOG_TRACE("Reconfig step RESUME,     start, dev %d client %s",
                dev_id,
                db_ptr->client_name);
      status = db_ptr->callbacks.enable_input_pkts(dev_id);
      LOG_TRACE("Reconfig step RESUME,     done,  dev %d client %s",
                dev_id,
                db_ptr->client_name);
    }
    // CLEANUP
    if (params->error_cleanup && db_ptr->callbacks.error_cleanup) {
      LOG_TRACE("Reconfig step ERROR CLEANUP,     start, dev %d client %s",
                dev_id,
                db_ptr->client_name);
      status = db_ptr->callbacks.error_cleanup(dev_id);
      LOG_TRACE("Reconfig step ERROR CLEANUP,     done,  dev %d client %s",
                dev_id,
                db_ptr->client_name);
    }
    if (status != BF_SUCCESS) {
      LOG_ERROR(
          "Reconfig callback failed for dev %d,"
          " sts %s (%d), Client %s ",
          dev_id,
          bf_err_str(status),
          status,
          db_ptr->client_name);
      if (full_status == BF_SUCCESS) {
        full_status = status;
      }
    }
  }

  return full_status;
}

bf_status_t bf_drv_reconfig_lock_device(bf_dev_id_t dev_id) {
  bf_status_t sts = bf_drv_apply_reconfig_step(dev_id, BF_RECONFIG_LOCK);
  if (BF_SUCCESS != sts) {
    LOG_ERROR("Dev %d reconfig lock failed status %s", dev_id, bf_err_str(sts));
  }
  return sts;
}

#ifdef BF_PROFILE_FAST_RECONFIG
#include "gperftools/profiler.h"
#endif
bf_status_t bf_drv_apply_reconfig_step(bf_dev_id_t dev_id,
                                       bf_fast_reconfig_step_t step) {
  bf_status_t status = BF_SUCCESS;
  reconfig_params_s params;

  if (step == BF_RECONFIG_LOCK) {
    // lock
    memset(&params, 0, sizeof(params));
    params.lock = true;
    status = bf_drv_reconfg_callbacks(dev_id, &params);  // TBD TF3-fix
    if (status != BF_SUCCESS) {
      return status;
    }
  } else if (step == BF_RECONFIG_UNLOCK || step == BF_RECONFIG_UNLOCK_QUICK) {
    if (step == BF_RECONFIG_UNLOCK_QUICK) {
      // device init state (add device will not be done)
      memset(&params, 0, sizeof(params));
      params.warm_init_quick = true;
      status = bf_drv_reconfg_callbacks(dev_id, &params);  // TBD TF3-fix
      if (status != BF_SUCCESS) {
        return status;
      }
    }

    // wait for swcfg replay to finish
    memset(&params, 0, sizeof(params));
    params.wait_for_swcfg_replay_end = true;
    status = bf_drv_reconfg_callbacks(dev_id, &params);
    if (status != BF_SUCCESS) {
      goto err_cleanup;
    }

    // create-dma
    memset(&params, 0, sizeof(params));
    params.create_dma = true;
    status = bf_drv_reconfg_callbacks(dev_id, &params);
    if (status != BF_SUCCESS) {
      goto err_cleanup;
    }

// disable input pkts
#ifdef BF_PROFILE_FAST_RECONFIG
    ProfilerStart("./perf_data.txt");
#endif
    memset(&params, 0, sizeof(params));
    params.disable_input_pkts = true;
    status = bf_drv_reconfg_callbacks(dev_id, &params);
    if (status != BF_SUCCESS) {
      goto err_cleanup;
    }

    // wait for flush
    memset(&params, 0, sizeof(params));
    params.wait_for_flush = true;
    status = bf_drv_reconfg_callbacks(dev_id, &params);
    if (status != BF_SUCCESS) {
      goto err_cleanup;
    }

    // Reset core
    memset(&params, 0, sizeof(params));
    params.core_reset = true;
    status = bf_drv_reconfg_callbacks(dev_id, &params);
    if (status != BF_SUCCESS) {
      goto err_cleanup;
    }

    // Unlock and reprogram core
    memset(&params, 0, sizeof(params));
    params.unlock_reprogram_core = true;
    status = bf_drv_reconfg_callbacks(dev_id, &params);
    if (status != BF_SUCCESS) {
      goto err_cleanup;
    }

    // Config complete
    memset(&params, 0, sizeof(params));
    params.config_complete = true;
    status = bf_drv_reconfg_callbacks(dev_id, &params);
    if (status != BF_SUCCESS) {
      goto err_cleanup;
    }

    // enable input pkts
    memset(&params, 0, sizeof(params));
    params.enable_input_pkts = true;
    status = bf_drv_reconfg_callbacks(dev_id, &params);
#ifdef BF_PROFILE_FAST_RECONFIG
    ProfilerFlush();
    ProfilerStop();
#endif
    if (status != BF_SUCCESS) {
      goto err_cleanup;
    }
  }

  return status;

err_cleanup:
  memset(&params, 0, sizeof(params));
  params.error_cleanup = true;
  bf_drv_reconfg_callbacks(dev_id, &params);
  return status;
}

/* Notify error interrupt handling mode */
bf_status_t bf_drv_notify_clients_err_interrupt_handling_mode(
    bf_dev_id_t dev_id, bool enable) {
  int id = 0;
  bf_drv_client_t *db_ptr = NULL;
  bf_status_t client_status = BF_SUCCESS, status = BF_SUCCESS;

  for (id = 0; id < BF_DRV_MAX_CLIENTS; id++) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if (db_ptr->callbacks.err_intr_mode) {
      client_status = db_ptr->callbacks.err_intr_mode(dev_id, enable);
      if (client_status != BF_SUCCESS) {
        LOG_ERROR(
            "Interrupt mode handing failed for dev %d,"
            " sts %s (%d), Client %s ",
            dev_id,
            bf_err_str(client_status),
            client_status,
            db_ptr->client_name);
        if (status == BF_SUCCESS) {
          status = client_status;
        }
      }
    }
  }

  return status;
}

bf_status_t bf_drv_notify_clients_complete_hitless_hw_read(bf_dev_id_t dev_id) {
  int id = 0;
  bf_drv_client_t *db_ptr = NULL;
  bf_status_t status = BF_SUCCESS;

  for (id = 0; id < BF_DRV_MAX_CLIENTS; id++) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if (db_ptr->callbacks.complete_hitless_hw_read) {
      status = db_ptr->callbacks.complete_hitless_hw_read(dev_id);
      if (status != BF_SUCCESS) {
        LOG_ERROR(
            "Complete HW read handling failed for dev %d, sts %s (%d),"
            " Client %s ",
            dev_id,
            bf_err_str(status),
            status,
            db_ptr->client_name);
        return status;
      }
    }
  }
  return status;
}

bf_status_t bf_drv_notify_clients_hitless_warm_init_end(bf_dev_id_t dev_id) {
  int id = 0;
  bf_drv_client_t *db_ptr = NULL;
  bf_status_t status = BF_SUCCESS;

  for (id = 0; id < BF_DRV_MAX_CLIENTS; id++) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if (db_ptr->callbacks.complete_hitless_swcfg_replay) {
      status = db_ptr->callbacks.complete_hitless_swcfg_replay(dev_id);
      if (status != BF_SUCCESS) {
        LOG_ERROR(
            "Complete swcfg replay failed for dev %d, sts %s "
            "(%d),"
            " Client %s ",
            dev_id,
            bf_err_str(status),
            status,
            db_ptr->client_name);
        return status;
      }
    }
  }

  for (id = 0; id < BF_DRV_MAX_CLIENTS; id++) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if (db_ptr->callbacks.compute_delta_changes) {
      status = db_ptr->callbacks.compute_delta_changes(dev_id, false);
      if (status != BF_SUCCESS) {
        LOG_ERROR(
            "Warm init end compute delta handling failed for dev %d, sts %s "
            "(%d),"
            " Client %s ",
            dev_id,
            bf_err_str(status),
            status,
            db_ptr->client_name);
        return status;
      }
    }
  }

  for (id = 0; id < BF_DRV_MAX_CLIENTS; id++) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if (db_ptr->callbacks.push_delta_changes) {
      status = db_ptr->callbacks.push_delta_changes(dev_id);
      if (status != BF_SUCCESS) {
        LOG_ERROR(
            "Warm init end push delta handling failed for dev %d, sts %s (%d),"
            " Client %s ",
            dev_id,
            bf_err_str(status),
            status,
            db_ptr->client_name);
        return status;
      }
    }
  }

  return status;
}

bf_status_t bf_drv_get_port_delta_from_clients(
    bf_dev_id_t dev_id, dvm_port_corr_action_t *port_corr) {
  int id = 0;
  bf_drv_client_t *db_ptr = NULL;
  bf_status_t status = BF_SUCCESS;
  if (!port_corr) return BF_INVALID_ARG;

  for (id = 0; id < BF_DRV_MAX_CLIENTS; id++) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if (db_ptr->callbacks.register_port_corr_action) {
      status = db_ptr->callbacks.register_port_corr_action(
          dev_id, &port_corr->port_reconcile_info[id]);
      if (status != BF_SUCCESS) {
        LOG_ERROR(
            "Port corr action registration failed for dev %d, sts %s (%d),"
            " Client %s ",
            dev_id,
            bf_err_str(status),
            status,
            db_ptr->client_name);
        return status;
      }
    }
  }

  return status;
}

bf_status_t bf_drv_notify_clients_port_delta_push_done(bf_dev_id_t dev_id) {
  int id = 0;
  bf_drv_client_t *db_ptr = NULL;
  bf_status_t status = BF_SUCCESS;

  for (id = 0; id < BF_DRV_MAX_CLIENTS; id++) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if (db_ptr->callbacks.port_delta_push_done) {
      status = db_ptr->callbacks.port_delta_push_done(dev_id);
      if (status != BF_SUCCESS) {
        LOG_ERROR(
            "Port delta push done notification handling failed for dev %d, sts "
            "%s "
            "(%d),"
            " Client %s ",
            dev_id,
            bf_err_str(status),
            status,
            db_ptr->client_name);
        return status;
      }
    }
  }

  return status;
}

bf_status_t bf_drv_get_get_next_client_for_port_corrective_actions(
    bf_dev_id_t dev_id, int cur_client, int *nxt_client) {
  int id = 0;
  bf_drv_client_t *db_ptr = NULL;
  bf_status_t status = BF_INVALID_ARG;

  (void)dev_id;

  if (!nxt_client) return status;
  *nxt_client = -1;

  if (bf_driver_client_db_init_done == false) return status;

  if (cur_client >= BF_DRV_MAX_CLIENTS || cur_client < -1) return status;

  id = (cur_client == -1) ? 0 : cur_client++;

  for (; id < BF_DRV_MAX_CLIENTS; id++) {
    db_ptr = &bf_driver_client_db[id];
    if (db_ptr->valid == false) {
      continue;
    }
    if (db_ptr->callbacks.register_port_corr_action) {
      *nxt_client = id;
      status = BF_SUCCESS;
      break;
    }
  }

  return status;
}
