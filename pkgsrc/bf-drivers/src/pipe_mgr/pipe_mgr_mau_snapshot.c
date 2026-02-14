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


/*!
 * @file pipe_mgr_mau_snapshot.c
 * @date
 *
 * Implementation of MAU snapshot
 */

/* Local header files */
#include "pipe_mgr_int.h"
#include "pipe_mgr_sm.h"
#include "pipe_mgr_tbl.h"
#include "pipe_mgr_mau_snapshot.h"
#include "pipe_mgr_tof_mau_snapshot.h"
#include "pipe_mgr_tof2_mau_snapshot.h"
#include "pipe_mgr_tof3_mau_snapshot.h"
#include "pipe_mgr_drv_intf.h"
#include "pipe_mgr_entry_format_json.h"
#include <tofino/pdfixed/pd_common.h>

extern pipe_mgr_ctx_t *pipe_mgr_ctx;

/* Per handle info */
typedef struct pipe_snap_hdl_info {
  dev_stage_t oper_stage; /* Stage where match trig is actually installed */
  uint32_t num_trig_fields;
  pipe_snap_trig_field_info_t *trig_fields;
} pipe_snap_hdl_info_t;

/* Per stage snapshot info */
typedef struct pipe_snap_stage_info {
  bf_snapshot_state_t admin_state;
  bool timer_enabled;
  uint32_t timer_val;
  dev_stage_t end_stage;
  bool hdl_created;
  pipe_snapshot_hdl_t hdl;
  bool all_fields_dict_valid;
  uint32_t size_of_dict;       /* Size of allocated dictionary */
  uint32_t num_fields_in_dict; /* Num of fields actually in dict */
  pipe_snap_stage_field_info_t *all_fields_dict;  // all fields
  bf_snapshot_ig_mode_t trig_mode;
} pipe_snap_stage_info_t;

/* Per device snapshot info */
#define PIPE_MGR_SNAP_NUM_DIR 2
typedef struct bf_snapshot_dev_info {
  bf_map_t hdl_htbl;
  bf_snapshot_triggered_cb trig_cb;
  bool interrupt_mode;
  pipe_snap_stage_info_t **stage_info[PIPE_MGR_SNAP_NUM_DIR];
  uint32_t capture_total_phv_size[PIPE_MGR_SNAP_NUM_DIR];
  uint32_t capture_per_stage_phv_size[PIPE_MGR_SNAP_NUM_DIR];
} bf_snapshot_dev_info_t;

/* Snapshot DB */
typedef struct bf_snapshot_info {
  bf_snapshot_dev_info_t *dev_info[PIPE_MGR_NUM_DEVICES];
} bf_snapshot_info_t;
static bf_snapshot_info_t snap_info;

#define PIPE_MGR_SNAP_INFO (snap_info)

#define PIPE_MGR_SNAP_HDL_MAP(dev) (snap_info.dev_info[dev]->hdl_htbl)

#define PIPE_MGR_SNAP_DEV_INFO(dev) (snap_info.dev_info[dev])

#define PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, dir) \
  (snap_info.dev_info[dev]->stage_info[dir][pipe][stage])

#define PIPE_MGR_SNAP_INTERRUPT_STATE(dev) \
  (snap_info.dev_info[dev]->interrupt_mode)

#define PIPE_MGR_SNAP_CALLBACK(dev) (snap_info.dev_info[dev]->trig_cb)

#define PIPE_MGR_SNAP_DEV_VALID(dev) (dev < PIPE_MGR_NUM_DEVICES)

#define PIPE_MGR_SNAP_TIMER_VAL_VALID(_val) (_val != 0)

static inline bool snap_pipe_valid(rmt_dev_info_t *dev_info,
                                   bf_dev_pipe_t pipe) {
  return pipe == BF_DEV_PIPE_ALL || dev_info->num_active_pipes > pipe;
}

static inline bool snap_stage_valid(rmt_dev_info_t *dev_info,
                                    dev_stage_t stage) {
  return stage < dev_info->num_active_mau;
}

#define PIPE_MGR_SNAP_DIR_VALID(dir) (dir <= BF_SNAPSHOT_DIR_EGRESS)

#define PIPE_MGR_SNAP_DIR_STRING(dir) \
  ((dir == BF_SNAPSHOT_DIR_INGRESS) ? "ingress" : "egress")

#define PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx) (hdl_info->trig_fields[idx])

int pipe_mgr_mau_snapshot_debug = 0;

#define PIPE_SNAP_DBG \
  if (pipe_mgr_mau_snapshot_debug) printf

/* --- FUNCTION PROTOTYPE DEFINTIONS ---- */
static pipe_status_t pipe_mgr_snapshot_fsm_state_set(
    bf_dev_id_t dev,
    bf_dev_pipe_t pipe,
    dev_stage_t stage,
    bf_snapshot_dir_t dir,
    pipe_snapshot_fsm_state_t fsm_state);
pipe_status_t pipe_mgr_snapshot_capture_trigger_fields_validate(
    pipe_snapshot_hdl_t hdl, dev_stage_t stage);

static pipe_status_t pipe_mgr_snapshot_capture_trigger_fields_clr(
    pipe_snapshot_hdl_t hdl);
static bf_status_t snapshot_capture_phv_fields_dict_size(
    bf_dev_id_t dev, int dir, uint32_t *total_size, uint32_t *per_stage_size);

/* Get num of active pipes on device */
static inline uint32_t pipe_mgr_snapshot_num_active_pipes(bf_dev_id_t dev) {
  return pipe_mgr_get_num_active_pipes(dev);
}

/* Snapshot fsm state to string */
char *pipe_mgr_snapshot_fsm_state_to_str(pipe_snapshot_fsm_state_t state,
                                         char *buf) {
  switch (state) {
    case PIPE_SNAPSHOT_FSM_ST_PASSIVE:
      strcpy(buf, "Passive");
      break;
    case PIPE_SNAPSHOT_FSM_ST_ARMED:
      strcpy(buf, "Armed");
      break;
    case PIPE_SNAPSHOT_FSM_ST_TRIGGER_HAPPY:
      strcpy(buf, "Trigger Happy");
      break;
    case PIPE_SNAPSHOT_FSM_ST_FULL:
      strcpy(buf, "Full");
      break;
    default:
      sprintf(buf, "Invalid (%d)", state);
      break;
  }

  return buf;
}

/* Snapshot enable state to string */
char *pipe_mgr_snapshot_state_to_str(bf_snapshot_state_t state, char *buf) {
  switch (state) {
    case BF_SNAPSHOT_ST_DISABLED:
      strcpy(buf, "Disabled");
      break;
    case BF_SNAPSHOT_ST_ENABLED:
      strcpy(buf, "Enabled");
      break;
    default:
      sprintf(buf, "Invalid (%d)", state);
      break;
  }

  return buf;
}

/* Get snapshot hdl info */
pipe_snap_hdl_info_t *pipe_mgr_snap_hdl_info_get(bf_dev_id_t dev,
                                                 pipe_snapshot_hdl_t hdl) {
  pipe_snap_hdl_info_t *hdl_info = NULL;
  bf_map_sts_t map_sts = BF_MAP_OK;

  map_sts = bf_map_get(&(PIPE_MGR_SNAP_HDL_MAP(dev)), hdl, (void **)&hdl_info);
  if (map_sts != BF_MAP_OK) {
    return NULL;
  }

  return hdl_info;
}

/* Init snapshot */
pipe_status_t pipe_mgr_snapshot_init() {
  PIPE_MGR_MEMSET(&PIPE_MGR_SNAP_INFO, 0, sizeof(PIPE_MGR_SNAP_INFO));
  return PIPE_SUCCESS;
}

/* Handle add device */
pipe_status_t pipe_mgr_snapshot_add_device(bf_dev_id_t dev) {
  int dir = 0;
  bf_dev_pipe_t pipe = 0;
  dev_stage_t stage = 0;
  profile_id_t prof_id = 0;

  if (!PIPE_MGR_SNAP_DEV_VALID(dev)) {
    return PIPE_INVALID_ARG;
  }
  if (PIPE_MGR_SNAP_DEV_INFO(dev)) {
    return PIPE_SUCCESS;
  }
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return PIPE_INVALID_ARG;

  PIPE_MGR_SNAP_DEV_INFO(dev) = PIPE_MGR_MALLOC(sizeof(bf_snapshot_dev_info_t));
  PIPE_MGR_MEMSET(
      PIPE_MGR_SNAP_DEV_INFO(dev), 0, sizeof(bf_snapshot_dev_info_t));

  /* Get number of fields for each stage in each direction */
  if (dev_info->num_pipeline_profiles == 0) {
    return PIPE_SUCCESS;
  }
  unsigned int num_pipes = dev_info->num_active_pipes;
  /* Create the snapshot info for all stages in the chip, including bypass
   * stages and extra stage with deparser iPHV. */
  dev_stage_t num_stages = dev_info->num_active_mau + 1;
  for (dir = 0; dir < PIPE_MGR_SNAP_NUM_DIR; dir++) {
    PIPE_MGR_SNAP_DEV_INFO(dev)->stage_info[dir] = PIPE_MGR_CALLOC(
        num_pipes, sizeof(*PIPE_MGR_SNAP_DEV_INFO(dev)->stage_info[dir]));
    if (!PIPE_MGR_SNAP_DEV_INFO(dev)->stage_info[dir]) {
      return PIPE_NO_SYS_RESOURCES;
    }
    for (pipe = 0; pipe < num_pipes; pipe++) {
      PIPE_MGR_SNAP_DEV_INFO(dev)->stage_info[dir][pipe] = PIPE_MGR_CALLOC(
          num_stages,
          sizeof(*PIPE_MGR_SNAP_DEV_INFO(dev)->stage_info[dir][pipe]));
      if (!PIPE_MGR_SNAP_DEV_INFO(dev)->stage_info[dir][pipe]) {
        return PIPE_NO_SYS_RESOURCES;
      }
      if (pipe >= dev_info->num_active_pipes) {
        continue;
      }
      if (pipe_mgr_pipe_to_profile(
              dev_info, pipe, &prof_id, __func__, __LINE__) != PIPE_SUCCESS) {
        continue;
      }
      // For snapshot purpose there should be 1 extra stage PHV info published
      // which defines oPHV from last existing stage.
      uint8_t last_stage = dev_info->profile_info[prof_id]->num_stages;


      /* Get the size for each stage configured by the profile. */
      for (stage = 0; stage <= last_stage; stage++) {
        pipe_mgr_ctxjson_phv_fields_dict_size_get(
            dev,
            prof_id,
            stage,
            dir,
            &(PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, dir).size_of_dict));
        PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, dir).all_fields_dict_valid =
            false;
      }
      /* And then for any bypass stages, use the PHV configuration for the last
       * programmed stage since PHV allocation remains static after that. */
      dev_stage_t decode_stage = dev_info->profile_info[prof_id]->num_stages;

      for (; stage < num_stages; ++stage) {
        pipe_mgr_ctxjson_phv_fields_dict_size_get(
            dev,
            prof_id,
            decode_stage,
            dir,
            &(PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, dir).size_of_dict));
        PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, dir).all_fields_dict_valid =
            false;
      }
    }
  }

  for (dir = 0; dir < PIPE_MGR_SNAP_NUM_DIR; dir++) {
    snapshot_capture_phv_fields_dict_size(
        dev,
        dir,
        &(PIPE_MGR_SNAP_DEV_INFO(dev)->capture_total_phv_size[dir]),
        &(PIPE_MGR_SNAP_DEV_INFO(dev)->capture_per_stage_phv_size[dir]));
  }

  return PIPE_SUCCESS;
}

/* Handle remove device */
pipe_status_t pipe_mgr_snapshot_remove_device(bf_dev_id_t dev) {
  bf_map_sts_t map_sts = BF_MAP_OK;
  pipe_snap_hdl_info_t *hdl_info = NULL;
  unsigned long key = 0;
  bf_dev_pipe_t pipe = 0;
  dev_stage_t stage = 0;
  int dir = 0;

  if (!PIPE_MGR_SNAP_DEV_VALID(dev)) {
    return PIPE_INVALID_ARG;
  }
  if (!PIPE_MGR_SNAP_DEV_INFO(dev)) {
    return PIPE_SUCCESS;
  }

  map_sts = bf_map_get_first_rmv(
      &(PIPE_MGR_SNAP_HDL_MAP(dev)), &key, (void **)&hdl_info);

  while (map_sts == BF_MAP_OK) {
    PIPE_MGR_FREE(hdl_info);
    hdl_info = NULL;

    map_sts = bf_map_get_first_rmv(
        &(PIPE_MGR_SNAP_HDL_MAP(dev)), &key, (void **)&hdl_info);
  }
  bf_map_destroy(&PIPE_MGR_SNAP_HDL_MAP(dev));

  /* Free up the memory allocated for field dictionaries per stage,
   * including extra stage */
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return PIPE_INVALID_ARG;
  unsigned int num_pipes = dev_info->num_active_pipes;
  dev_stage_t num_stages = dev_info->num_active_mau + 1;
  for (dir = 0; dir < PIPE_MGR_SNAP_NUM_DIR; dir++) {
    if (PIPE_MGR_SNAP_DEV_INFO(dev)->stage_info[dir]) {
      for (pipe = 0; pipe < num_pipes; pipe++) {
        if (PIPE_MGR_SNAP_DEV_INFO(dev)->stage_info[dir][pipe]) {
          for (stage = 0; stage < num_stages; stage++) {
            if (PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, dir)
                    .all_fields_dict) {
              PIPE_MGR_FREE(PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, dir)
                                .all_fields_dict);
              PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, dir).all_fields_dict =
                  NULL;
            }
            PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, dir)
                .all_fields_dict_valid = false;
          }
          PIPE_MGR_FREE(PIPE_MGR_SNAP_DEV_INFO(dev)->stage_info[dir][pipe]);
        }
      }
      PIPE_MGR_FREE(PIPE_MGR_SNAP_DEV_INFO(dev)->stage_info[dir]);
    }
  }

  PIPE_MGR_FREE(PIPE_MGR_SNAP_DEV_INFO(dev));
  PIPE_MGR_SNAP_DEV_INFO(dev) = NULL;

  return PIPE_SUCCESS;
}

/*
   Set the snapshot monitoring mode
   Also, Register callback function for snapshot
*/
bf_status_t bf_snapshot_monitoring_mode(bf_dev_id_t dev,
                                        bool interrupt_or_polling,
                                        bf_snapshot_triggered_cb trig_cb) {
  if (!PIPE_MGR_SNAP_DEV_VALID(dev)) {
    return BF_INVALID_ARG;
  }
  if (!PIPE_MGR_SNAP_DEV_INFO(dev)) {
    return BF_INVALID_ARG;
  }
  PIPE_MGR_SNAP_INTERRUPT_STATE(dev) = interrupt_or_polling;
  PIPE_MGR_SNAP_CALLBACK(dev) = trig_cb;

  return BF_SUCCESS;
}

/* Convert device params to snapshot hdl  */
static inline bf_status_t pipe_mgr_snapshot_dev_params_to_hdl(
    bf_dev_id_t dev,
    bf_dev_pipe_t pipe,
    int s_stage,
    int e_stage,
    bf_snapshot_dir_t dir,
    pipe_snapshot_hdl_t *hdl) {
  /* unused (4 bits),  dev-id (8 bits), pipe (8 bits),
     end_stage (5 bits) start-stage (5 bits), dir (1 bit),
     unused (1 bit)
  */
  *hdl = ((dev & 0xFF) << 20) | ((pipe & 0xFF) << 12) |
         ((e_stage & 0x1F) << 7) | ((s_stage & 0x1F) << 2) |
         ((dir & 0x1) << 1) | 0x1;

  return BF_SUCCESS;
}

/* Convert snapshot hdl to device params */
static inline bf_status_t pipe_mgr_snapshot_hdl_to_dev_params(
    pipe_snapshot_hdl_t hdl,
    bf_dev_id_t *dev,
    bf_dev_pipe_t *pipe,
    dev_stage_t *s_stage,
    dev_stage_t *e_stage,
    int *dir) {
  if (dev) *dev = (hdl >> 20) & 0xFF;
  if (pipe) *pipe = (hdl >> 12) & 0xFF;
  if (e_stage) *e_stage = (hdl >> 7) & 0x1F;
  if (s_stage) *s_stage = (hdl >> 2) & 0x1F;
  if (dir) *dir = (hdl >> 1) & 0x1;

  /* We encoded only 8 bits of pipe, while decoding set correct value */
  if (pipe && (*pipe == 0xFF)) {
    *pipe = BF_DEV_PIPE_ALL;
  }

  return BF_SUCCESS;
}

/* Check if snapshot hdl is valid  */
static inline bool pipe_mgr_snapshot_hdl_valid(pipe_snapshot_hdl_t hdl) {
  bf_dev_id_t dev = 0;
  pipe_snap_hdl_info_t *hdl_info = NULL;

  pipe_mgr_snapshot_hdl_to_dev_params(hdl, &dev, NULL, NULL, NULL, NULL);
  if (!PIPE_MGR_SNAP_DEV_VALID(dev)) {
    LOG_ERROR("Handle 0x%x is invalid, invalid dev %d ", hdl, dev);
    return false;
  }
  if (!PIPE_MGR_SNAP_DEV_INFO(dev)) {
    return false;
  }
  /* Check if it exists in map */
  hdl_info = pipe_mgr_snap_hdl_info_get(dev, hdl);

  if (hdl_info) {
    return true;
  } else {
    LOG_ERROR("Handle 0x%x does not exist ", hdl);
    return false;
  }

  return false;
}

/* Get snapshot enabled stages */
pipe_status_t bf_snapshot_first_handle_get(bf_dev_id_t dev,
                                           bf_dev_pipe_t pipe,
                                           pipe_snapshot_hdl_t *entry_hdl) {
  pipe_snap_hdl_info_t *hdl_info = NULL;
  pipe_status_t status;
  bf_map_sts_t map_sts = BF_MAP_OK;
  unsigned long hdl;
  bf_dev_pipe_t hdl_pipe;
  bf_dev_id_t hdl_dev;

  map_sts =
      bf_map_get_first(&(PIPE_MGR_SNAP_HDL_MAP(dev)), &hdl, (void **)&hdl_info);
  while (map_sts == BF_MAP_OK) {
    status = pipe_mgr_snapshot_hdl_to_dev_params(
        hdl, &hdl_dev, &hdl_pipe, NULL, NULL, NULL);
    if (status == PIPE_SUCCESS &&
        (hdl_pipe == pipe || hdl_pipe == BF_DEV_PIPE_ALL)) {
      *entry_hdl = hdl;
      return PIPE_SUCCESS;
    }
    map_sts = bf_map_get_next(
        &(PIPE_MGR_SNAP_HDL_MAP(dev)), &hdl, (void **)&hdl_info);
  }

  return PIPE_OBJ_NOT_FOUND;
}

/* Get snapshot enabled stages for raw data fetching */
pipe_status_t bf_snapshot_stages_get(bf_dev_id_t dev,
                                     bf_dev_pipe_t pipe,
                                     uint32_t size,
                                     int *stages) {
  pipe_snap_hdl_info_t *hdl_info = NULL;
  pipe_status_t status;
  bf_map_sts_t map_sts = BF_MAP_OK;
  unsigned long hdl = 0;
  bf_dev_pipe_t hdl_pipe;
  bf_dev_id_t hdl_dev;
  dev_stage_t s_stage;
  dev_stage_t e_stage;
  uint32_t i = 0;
  int hdl_dir;

  PIPE_MGR_MEMSET(stages, -1, size * sizeof(stages[0]));

  map_sts =
      bf_map_get_first(&(PIPE_MGR_SNAP_HDL_MAP(dev)), &hdl, (void **)&hdl_info);
  while (map_sts == BF_MAP_OK) {
    status = pipe_mgr_snapshot_hdl_to_dev_params(
        hdl, &hdl_dev, &hdl_pipe, &s_stage, &e_stage, &hdl_dir);
    if (status == PIPE_SUCCESS &&
        (hdl_pipe == pipe || hdl_pipe == BF_DEV_PIPE_ALL)) {
      for (int j = s_stage; j <= e_stage; j++, i++) {
        if (size <= i) return PIPE_INVALID_ARG;
        stages[i] = j;
      }
    }
    map_sts = bf_map_get_next(
        &(PIPE_MGR_SNAP_HDL_MAP(dev)), &hdl, (void **)&hdl_info);
  }

  return PIPE_SUCCESS;
}

/* Get dev params for handle.  */
bf_status_t bf_snapshot_entry_params_get(pipe_snapshot_hdl_t hdl,
                                         bf_dev_id_t *dev,
                                         bf_dev_pipe_t *pipe,
                                         dev_stage_t *s_stage,
                                         dev_stage_t *e_stage,
                                         bf_snapshot_dir_t *dir) {
  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return BF_INVALID_ARG;
  }

  return pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, dev, pipe, s_stage, e_stage, (int *)dir);
}

/* Get fsm states for all stages on a pipe  */
pipe_status_t pipe_mgr_snap_all_fsm_states_get(
    bf_dev_id_t dev,
    bf_dev_pipe_t pipe,
    int dir,
    pipe_snapshot_fsm_state_t *stage_fsm_st) {
  pipe_status_t status = PIPE_SUCCESS;
  int s_idx = 0, s_rng_idx = 0;
  pipe_snap_hdl_info_t *hdl_info = NULL;
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return PIPE_INVALID_ARG;
  dev_stage_t num_stages = dev_info->num_active_mau;
  bool local[num_stages];
  bool prev[num_stages];

  PIPE_MGR_MEMSET(local, 0, sizeof(local));
  PIPE_MGR_MEMSET(prev, 0, sizeof(prev));

  /* Go over all stages */
  for (s_idx = 0; s_idx < num_stages; s_idx++) {
    if (PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_idx, dir).admin_state &&
        PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_idx, dir).hdl_created) {
      hdl_info = pipe_mgr_snap_hdl_info_get(
          dev, PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_idx, dir).hdl);
      local[s_idx] = true;

      for (s_rng_idx = s_idx + 1;
           s_rng_idx <=
           PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_idx, dir).end_stage;
           s_rng_idx++) {
        /* If oper-stage is different than start stage, set local match
           on oper-stage
         */
        if (hdl_info && (hdl_info->oper_stage == s_rng_idx)) {
          local[s_rng_idx] = true;
        } else {
          prev[s_rng_idx] = true;
        }
      }
    }
  }

  /* Get the right fsm state */
  for (s_idx = 0; s_idx < num_stages; s_idx++) {
    if (local[s_idx] && prev[s_idx]) {
      stage_fsm_st[s_idx] = PIPE_SNAPSHOT_FSM_ST_TRIGGER_HAPPY;
    } else if (prev[s_idx]) {
      stage_fsm_st[s_idx] = PIPE_SNAPSHOT_FSM_ST_PASSIVE;
    } else if (local[s_idx]) {
      stage_fsm_st[s_idx] = PIPE_SNAPSHOT_FSM_ST_ARMED;
    } else {
      stage_fsm_st[s_idx] = PIPE_SNAPSHOT_FSM_ST_FULL;
    }
  }

  return status;
}

/* Reevaulate and reprogram the fsm states for affected stages */
pipe_status_t pipe_mgr_reevaluate_program_fsm_states(pipe_snapshot_hdl_t hdl) {
  pipe_status_t status = PIPE_SUCCESS;
  bf_dev_pipe_t pipe_idx = 0;
  bf_dev_id_t dev = 0;
  bf_dev_pipe_t pipe = 0;
  dev_stage_t s_stage = 0, e_stage = 0;
  bf_dev_pipe_t start_pipe = 0, pipe_count = 0;
  int dir = 0, s_idx = 0;

  status = pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, &pipe, &s_stage, &e_stage, &dir);
  if (status != PIPE_SUCCESS) {
    LOG_ERROR("Could not convert snapshot hdl 0x%x to dev params", hdl);
    return status;
  }

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return PIPE_INVALID_ARG;
  dev_stage_t num_stages = dev_info->num_active_mau;
  pipe_snapshot_fsm_state_t stage_fsm_st[num_stages];
  if (pipe == BF_DEV_PIPE_ALL) {
    start_pipe = 0;
    pipe_count = pipe_mgr_snapshot_num_active_pipes(dev);
  } else {
    start_pipe = pipe;
    pipe_count = 1;
  }

  pipe_sess_hdl_t shdl = pipe_mgr_ctx->int_ses_hndl;
  status = pipe_mgr_api_enter(shdl);
  if (status != PIPE_SUCCESS) return status;

  /* Reprogram for all pipes where hdl is */
  for (pipe_idx = start_pipe; pipe_idx < (start_pipe + pipe_count);
       pipe_idx++) {
    PIPE_MGR_MEMSET(stage_fsm_st, 0, sizeof(stage_fsm_st));
    /* Re-evaulate the fsm state for all stages */
    pipe_mgr_snap_all_fsm_states_get(dev, pipe_idx, dir, &stage_fsm_st[0]);

    /* Rewrite specified stages only */
    /* Write fsm states in reverse order so that all stages are captured
       if traffic is running
    */
    for (s_idx = e_stage; s_idx >= s_stage; s_idx--) {
      status = pipe_mgr_snapshot_fsm_state_set(
          dev, pipe_idx, s_idx, dir, stage_fsm_st[s_idx]);
      if (status != PIPE_SUCCESS) {
        goto done;
      }
    }
  }

done:
  pipe_mgr_api_exit(shdl);
  return status;
}

/* Check if snapshot onveralps with any existing snapshot */
bf_status_t pipe_mgr_snapshot_overlap_check(pipe_snapshot_hdl_t hdl,
                                            bf_dev_id_t dev,
                                            bf_dev_pipe_t pipe,
                                            dev_stage_t s_stage,
                                            dev_stage_t e_stage,
                                            int dir) {
  bf_status_t status = BF_SUCCESS;
  dev_stage_t stage = 0;
  int dir_idx = 0;

  (void)dir;

  if (pipe == BF_DEV_PIPE_ALL) {
    return BF_INVALID_ARG;
  }

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return PIPE_INVALID_ARG;
  dev_stage_t num_stages = dev_info->num_active_mau;
  for (stage = 0; stage < num_stages; stage++) {
    /* Snapshot cannot exist in ingress and egress direction as the same time
       as the snapshot registers are common.
       Check for overlap on ingress and egress
    */
    for (dir_idx = BF_SNAPSHOT_DIR_INGRESS; dir_idx <= BF_SNAPSHOT_DIR_EGRESS;
         dir_idx++) {
      if (PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, dir_idx).hdl_created) {
        /* overlap check -
          condition 1: Overlap check on right side of existing snapshot
          condition 2: Overlap check on left side of existing snapshot
          condition 3: Overlap check inside an existing snapshot
        */
        if (((PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, dir_idx).end_stage >=
              s_stage) &&
             (PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, dir_idx).end_stage <=
              e_stage)) ||
            ((stage >= s_stage) && (stage <= e_stage)) ||
            ((stage <= s_stage) &&
             (PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, dir_idx).end_stage >=
              e_stage))) {
          LOG_ERROR(
              "Snapshot config (hdl 0x%x) overlaps with an existing snapshot "
              "having handle 0x%x ",
              hdl,
              PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, dir_idx).hdl);

          return BF_NOT_SUPPORTED;
        }
      }
    }
  }

  return status;
}

/* Snapshot timer enable helper */
static pipe_status_t pipe_mgr_snapshot_cfg_set(rmt_dev_info_t *dev_info,
                                               bf_dev_pipe_t pipe,
                                               dev_stage_t stage,
                                               bf_snapshot_dir_t dir,
                                               bool timer_disable,
                                               bf_snapshot_ig_mode_t mode) {
  pipe_status_t status = PIPE_SUCCESS;
  bf_dev_id_t dev = dev_info->dev_id;
  bf_dev_pipe_t start_pipe = 0, pipe_count = 0, pipe_idx = 0;

  if (!PIPE_MGR_SNAP_DIR_VALID(dir)) {
    LOG_ERROR("Invalid direction %d ", dir);
    return PIPE_INVALID_ARG;
  }
  if (dev_info->dev_family == BF_DEV_FAMILY_TOFINO &&
      mode != BF_SNAPSHOT_IGM_INGRESS) {
    LOG_ERROR("Tofino supports only \"ingress\" trigger mode");
    return BF_INVALID_ARG;
  }

  if (pipe == BF_DEV_PIPE_ALL) {
    start_pipe = 0;
    pipe_count = dev_info->num_active_pipes;
  } else {
    start_pipe = pipe;
    pipe_count = 1;
  }

  /* Update DB */
  for (pipe_idx = start_pipe; pipe_idx < (start_pipe + pipe_count);
       pipe_idx++) {
    PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, stage, dir).timer_enabled =
        (!timer_disable);
    PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, stage, dir).trig_mode = mode;
  }
  /* Update hardware */
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO: {
      bool ing_enable = PIPE_MGR_SNAP_STAGE_INFO(
                            dev, start_pipe, stage, BF_SNAPSHOT_DIR_INGRESS)
                            .timer_enabled;
      bool egr_enable = PIPE_MGR_SNAP_STAGE_INFO(
                            dev, start_pipe, stage, BF_SNAPSHOT_DIR_EGRESS)
                            .timer_enabled;
      /* There is no configuration option for ingress mode on tofino */
      status = pipe_mgr_snapshot_timer_enable_tof(
          dev_info, pipe, stage, ing_enable, egr_enable);
      break;
    }
    case BF_DEV_FAMILY_TOFINO2: {
      bool ing_enable =
          PIPE_MGR_SNAP_STAGE_INFO(dev, start_pipe, stage, 0).timer_enabled;
      bool egr_enable =
          PIPE_MGR_SNAP_STAGE_INFO(dev, start_pipe, stage, 1).timer_enabled;
      status = pipe_mgr_snapshot_cfg_set_tof2(
          dev_info, pipe, stage, ing_enable, egr_enable, mode);
      break;
    }
    case BF_DEV_FAMILY_TOFINO3: {
      bool ing_enable =
          PIPE_MGR_SNAP_STAGE_INFO(dev, start_pipe, stage, 0).timer_enabled;
      bool egr_enable =
          PIPE_MGR_SNAP_STAGE_INFO(dev, start_pipe, stage, 1).timer_enabled;
      status = pipe_mgr_snapshot_cfg_set_tof3(
          dev_info, pipe, stage, ing_enable, egr_enable, mode);
      break;
    }
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }

  return status;
}

/* Snapshot cfg set */
bf_status_t bf_snapshot_cfg_set(pipe_snapshot_hdl_t hdl,
                                bool timer_disable,
                                bf_snapshot_ig_mode_t mode) {
  bf_status_t status = BF_SUCCESS;
  bf_dev_id_t dev = 0;
  bf_dev_pipe_t pipe = 0;
  dev_stage_t s_stage = 0, e_stage = 0;
  int dir = 0;

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return BF_INVALID_ARG;
  }
  status = pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, &pipe, &s_stage, &e_stage, &dir);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Could not convert snapshot hdl 0x%x to dev params", hdl);
    return status;
  }
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return BF_INVALID_ARG;

  pipe_sess_hdl_t shdl = pipe_mgr_ctx->int_ses_hndl;
  status = pipe_mgr_api_enter(shdl);
  if (status != PIPE_SUCCESS) return status;

  if (pipe_mgr_snapshot_cfg_set(
          dev_info, pipe, s_stage, dir, timer_disable, mode) != PIPE_SUCCESS) {
    status = BF_INVALID_ARG;
  }

  pipe_mgr_api_exit(shdl);
  return status;
}

/* Snapshot timer and ingress trigger values get. This function aggregates timer
 * enable and time thresholds. */
bf_status_t bf_snapshot_cfg_get(pipe_snapshot_hdl_t hdl,
                                bool *timer_enable,
                                uint32_t *usec,
                                bf_snapshot_ig_mode_t *mode) {
  bf_dev_pipe_t pipe = 0;
  dev_stage_t stage = 0;
  bf_dev_id_t dev = 0;
  int dir;

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) return BF_INVALID_ARG;

  pipe_mgr_snapshot_hdl_to_dev_params(hdl, &dev, &pipe, &stage, NULL, &dir);
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }

  /* If snapshot was created for all pipes - take the lowest one. */
  if (pipe == BF_DEV_PIPE_ALL) {
    pipe = 0;
  }

  pipe_sess_hdl_t shdl = pipe_mgr_ctx->int_ses_hndl;
  pipe_status_t status = pipe_mgr_api_enter(shdl);
  if (status != PIPE_SUCCESS) return status;

  /* Read mau snapshot config */
  bool ing_timer_en = false, egr_timer_en = false;
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      pipe_mgr_snapshot_timer_get_enable_tof(
          dev_info, pipe, stage, &ing_timer_en, &egr_timer_en);
      /* Ingress trigger mode supported on Tofino is ingress only. */
      *mode = BF_SNAPSHOT_IGM_INGRESS;
      break;
    case BF_DEV_FAMILY_TOFINO2:
      pipe_mgr_snapshot_cfg_get_tof2(
          dev_info, pipe, stage, &ing_timer_en, &egr_timer_en, mode);
      break;
    case BF_DEV_FAMILY_TOFINO3:
      pipe_mgr_snapshot_cfg_get_tof3(
          dev_info, pipe, stage, &ing_timer_en, &egr_timer_en, mode);
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      pipe_mgr_api_exit(shdl);
      return PIPE_UNEXPECTED;
  }
  *timer_enable =
      (dir == BF_SNAPSHOT_DIR_INGRESS) ? ing_timer_en : egr_timer_en;

  /* Read time counter */
  uint64_t clocks_now, clocks_trig;
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      pipe_mgr_snapshot_timer_get_tof(
          dev_info, pipe, stage, &clocks_now, &clocks_trig);
      break;
    case BF_DEV_FAMILY_TOFINO2:
      pipe_mgr_snapshot_timer_get_tof2(
          dev_info, pipe, stage, &clocks_now, &clocks_trig);
      break;
    case BF_DEV_FAMILY_TOFINO3:
      pipe_mgr_snapshot_timer_get_tof3(
          dev_info, pipe, stage, &clocks_now, &clocks_trig);
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      pipe_mgr_api_exit(shdl);
      return PIPE_UNEXPECTED;
  }
  /* In theory this can overflow usec type size, but in practice
   * configured value will not be bigger than uint32_t, because
   * that is what interface allows. */
  *usec = pipe_mgr_clock_to_usec(dev, clocks_trig);

  pipe_mgr_api_exit(shdl);
  return status;
}

/* Snapshot timer val set */
static pipe_status_t pipe_mgr_snapshot_timer_set(pipe_snapshot_hdl_t hdl,
                                                 rmt_dev_info_t *dev_info,
                                                 bf_dev_pipe_t pipe,
                                                 dev_stage_t stage,
                                                 uint32_t usec) {
  pipe_status_t status = PIPE_SUCCESS;
  uint64_t clocks = 0;
  bf_dev_id_t dev = dev_info->dev_id;
  (void)hdl;

  if (!PIPE_MGR_SNAP_DEV_VALID(dev)) {
    return PIPE_INVALID_ARG;
  }
  if (!PIPE_MGR_SNAP_DEV_INFO(dev)) {
    return PIPE_INVALID_ARG;
  }

  /* Convert time to clocks */
  clocks = pipe_mgr_usec_to_clock(dev, usec);

  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      status = pipe_mgr_snapshot_timer_set_tof(dev_info, pipe, stage, clocks);
      break;
    case BF_DEV_FAMILY_TOFINO2:
      status = pipe_mgr_snapshot_timer_set_tof2(dev_info, pipe, stage, clocks);
      break;
    case BF_DEV_FAMILY_TOFINO3:
      status = pipe_mgr_snapshot_timer_set_tof3(dev_info, pipe, stage, clocks);
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }
  return status;
}

/* Snapshot get captured threads */
bf_status_t bf_snapshot_capture_thread_get(pipe_snapshot_hdl_t hdl,
                                           uint32_t size,
                                           int *threads) {
  bf_dev_pipe_t start_pipe = 0, pipe_count = 0, pipe_idx = 0;
  dev_stage_t stage = 0;
  bf_dev_id_t dev = 0;
  int dir;

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) return BF_INVALID_ARG;
  if (!threads) return BF_INVALID_ARG;

  pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, &start_pipe, &stage, NULL, &dir);
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }

  /* If snapshot was created for all pipes - take the lowest one. */
  if (start_pipe == BF_DEV_PIPE_ALL) {
    start_pipe = 0;
    pipe_count = pipe_mgr_snapshot_num_active_pipes(dev);
  } else {
    pipe_count = 1;
  }
  if (size < pipe_count) {
    LOG_ERROR("Provided array size %d will not fit all the data %d",
              size,
              pipe_count);
    return BF_INVALID_ARG;
  }

  pipe_sess_hdl_t shdl = pipe_mgr_ctx->int_ses_hndl;
  pipe_status_t status = pipe_mgr_api_enter(shdl);
  if (status != PIPE_SUCCESS) return status;

  /* Check overlap on all pipes */
  for (pipe_idx = start_pipe; pipe_idx < (start_pipe + pipe_count);
       pipe_idx++) {
    /* Read snapshot captured threads */
    bool ing_cap = false, egr_cap = false, ghost_cap = false;
    switch (dev_info->dev_family) {
      case BF_DEV_FAMILY_TOFINO:
        pipe_mgr_snapshot_captured_thread_get_tof(
            dev_info, pipe_idx, stage, dir, &ing_cap, &egr_cap);
        break;
      case BF_DEV_FAMILY_TOFINO2:
        pipe_mgr_snapshot_captured_thread_get_tof2(
            dev_info, pipe_idx, stage, dir, &ing_cap, &egr_cap, &ghost_cap);
        break;
      case BF_DEV_FAMILY_TOFINO3:
        pipe_mgr_snapshot_captured_thread_get_tof3(
            dev_info, pipe_idx, stage, dir, &ing_cap, &egr_cap, &ghost_cap);
        break;
      default:
        PIPE_MGR_DBGCHK(0);
        pipe_mgr_api_exit(shdl);
        return PIPE_UNEXPECTED;
    }
    threads[pipe_idx] = 0;
    threads[pipe_idx] |= ing_cap << BF_SNAPSHOT_THREAD_INGRESS;
    threads[pipe_idx] |= egr_cap << BF_SNAPSHOT_THREAD_EGRESS;
    threads[pipe_idx] |= ghost_cap << BF_SNAPSHOT_THREAD_GHOST;
  }

  pipe_mgr_api_exit(shdl);
  return status;
}

/* Create dictionary of all fields in a stage */
pipe_status_t pipe_mgr_create_all_fields_dict(bf_dev_id_t dev,
                                              bf_dev_pipe_t pipe,
                                              dev_stage_t s_stage,
                                              bf_snapshot_dir_t dir) {
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  profile_id_t prof_id = 0;
  if (pipe == BF_DEV_PIPE_ALL) {
    return PIPE_INVALID_ARG;
  }

  if (PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_stage, dir).all_fields_dict_valid) {
    return PIPE_SUCCESS;
  }

  /* Free memory if it is still valid */
  if (PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_stage, dir).all_fields_dict) {
    PIPE_MGR_FREE(
        PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_stage, dir).all_fields_dict);
  }

  /* Allocate and rebuild database */
  PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_stage, dir).all_fields_dict =
      PIPE_MGR_CALLOC(
          PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_stage, dir).size_of_dict,
          sizeof(pipe_snap_stage_field_info_t));
  if (!(PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_stage, dir).all_fields_dict)) {
    return PIPE_NO_SYS_RESOURCES;
  }

  if (pipe_mgr_pipe_to_profile(dev_info, pipe, &prof_id, __func__, __LINE__) !=
      PIPE_SUCCESS) {
    return PIPE_INVALID_ARG;
  }

  dev_stage_t decode_stage = s_stage;
  if (decode_stage > dev_info->profile_info[prof_id]->num_stages) {
    decode_stage = dev_info->profile_info[prof_id]->num_stages;
  }
  pipe_mgr_ctxjson_phv_fields_dict_get(
      dev,
      prof_id,
      decode_stage,
      dir,
      PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_stage, dir).all_fields_dict,
      &(PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_stage, dir).num_fields_in_dict));
  /* Make sure allocated memory was enough */
  PIPE_MGR_DBGCHK(
      PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_stage, dir).size_of_dict >=
      PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_stage, dir).num_fields_in_dict);

  PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_stage, dir).all_fields_dict_valid =
      true;

  return PIPE_SUCCESS;
}

static void reset_dp_register(rmt_dev_info_t *dev_info,
                              bf_dev_pipe_t pipe_idx,
                              dev_stage_t stage,
                              bf_snapshot_dir_t dir) {
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      pipe_mgr_snapshot_dp_reset_tof(dev_info, pipe_idx, stage, dir);
      break;
    case BF_DEV_FAMILY_TOFINO2:
      pipe_mgr_snapshot_dp_reset_tof2(dev_info, pipe_idx, stage, dir);
      break;
    case BF_DEV_FAMILY_TOFINO3:
      pipe_mgr_snapshot_dp_reset_tof3(dev_info, pipe_idx, stage, dir);
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      break;
  }
}

/* Snapshot create */
bf_status_t bf_snapshot_create(bf_dev_id_t dev,
                               bf_dev_pipe_t pipe,
                               dev_stage_t s_stage,
                               dev_stage_t e_stage,
                               bf_snapshot_dir_t dir,
                               pipe_snapshot_hdl_t *hdl) {
  bf_status_t status = BF_SUCCESS;
  bf_map_sts_t map_sts = BF_MAP_OK;
  pipe_snap_hdl_info_t *hdl_info = NULL;
  bf_dev_pipe_t start_pipe = 0, pipe_count = 0, pipe_idx = 0;
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return BF_INVALID_ARG;

  if (!PIPE_MGR_SNAP_DEV_INFO(dev)) {
    LOG_ERROR("Device %d not found ", dev);
    return BF_INVALID_ARG;
  }
  if (!snap_pipe_valid(dev_info, pipe)) {
    LOG_ERROR("Invalid pipe %d ", pipe);
    return BF_INVALID_ARG;
  }

  if (dev_info->num_pipeline_profiles > 1 && pipe == BF_DEV_PIPE_ALL) {
    LOG_ERROR(
        "Device %d is configured with multiple pipeline profiles, so single "
        "snapshot instance for all pipes(0x%x) is not supported. Please create "
        "snapshot per pipe",
        dev,
        pipe);
    return PIPE_INVALID_ARG;
  }

  if (!snap_stage_valid(dev_info, s_stage) ||
      !snap_stage_valid(dev_info, e_stage) || (e_stage < s_stage)) {
    LOG_ERROR("Invalid stage ");
    return BF_INVALID_ARG;
  }
  if (!PIPE_MGR_SNAP_DIR_VALID(dir)) {
    LOG_ERROR("Invalid direction %d ", dir);
    return BF_INVALID_ARG;
  }

  pipe_sess_hdl_t shdl = pipe_mgr_ctx->int_ses_hndl;
  status = pipe_mgr_api_enter(shdl);
  if (status != PIPE_SUCCESS) return status;

  pipe_mgr_snapshot_dev_params_to_hdl(dev, pipe, s_stage, e_stage, dir, hdl);

  /* If snapshot handle already exists, return the status */
  hdl_info = pipe_mgr_snap_hdl_info_get(dev, *hdl);
  if (hdl_info) {
    LOG_ERROR("Handle 0x%x exists ", *hdl);
    status = BF_ALREADY_EXISTS;
    goto done;
  }
  LOG_TRACE(
      "Allocated handle 0x%x for dev %d, pipe %d, stage (%d-%d),"
      "dir %d ",
      *hdl,
      dev,
      pipe,
      s_stage,
      e_stage,
      dir);

  if (pipe == BF_DEV_PIPE_ALL) {
    start_pipe = 0;
    pipe_count = pipe_mgr_snapshot_num_active_pipes(dev);
  } else {
    start_pipe = pipe;
    pipe_count = 1;
  }

  /* Check overlap on all pipes */
  for (pipe_idx = start_pipe; pipe_idx < (start_pipe + pipe_count);
       pipe_idx++) {
    status = pipe_mgr_snapshot_overlap_check(
        *hdl, dev, pipe_idx, s_stage, e_stage, dir);
    if (status != BF_SUCCESS) {
      goto done;
    }
  }

  LOG_TRACE(
      "Adding to map: handle 0x%x for dev %d, pipe %d, stage (%d-%d),"
      " dir %d ",
      *hdl,
      dev,
      pipe,
      s_stage,
      e_stage,
      dir);

  hdl_info =
      (pipe_snap_hdl_info_t *)PIPE_MGR_MALLOC(sizeof(pipe_snap_hdl_info_t));
  PIPE_MGR_MEMSET(hdl_info, 0, sizeof(pipe_snap_hdl_info_t));

  hdl_info->oper_stage = s_stage;
  map_sts = bf_map_add(&(PIPE_MGR_SNAP_HDL_MAP(dev)), *hdl, hdl_info);
  if (map_sts != BF_MAP_OK) {
    LOG_ERROR("Error when adding hdl %d to map", *hdl);
    PIPE_MGR_FREE(hdl_info);
    status = BF_INVALID_ARG;
    goto done;
  }

  hdl_info->num_trig_fields =
      PIPE_MGR_SNAP_STAGE_INFO(dev, start_pipe, s_stage, dir).size_of_dict;
  hdl_info->trig_fields = PIPE_MGR_CALLOC(hdl_info->num_trig_fields,
                                          sizeof(pipe_snap_trig_field_info_t));

  /* Malloc failed, delete the entry from DB */
  if (!hdl_info->trig_fields) {
    hdl_info->num_trig_fields = 0;
    bf_snapshot_delete(*hdl);
    status = BF_NO_SYS_RESOURCES;
    goto done;
  }

  /* Populate the snapshot DB */
  for (pipe_idx = start_pipe; pipe_idx < (start_pipe + pipe_count);
       pipe_idx++) {
    PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, s_stage, dir).hdl_created = true;
    PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, s_stage, dir).hdl = *hdl;
    PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, s_stage, dir).end_stage = e_stage;
    PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, s_stage, dir).admin_state =
        BF_SNAPSHOT_ST_DISABLED;

    /* Disable the timer state on snapshot creation */
    pipe_mgr_snapshot_cfg_set(
        dev_info, pipe_idx, s_stage, dir, true, BF_SNAPSHOT_IGM_INGRESS);
    PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, s_stage, dir).timer_enabled = false;
    /* Reset datapath register */
    reset_dp_register(dev_info, pipe_idx, s_stage, dir);

    if (!(PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, s_stage, dir)
              .all_fields_dict_valid)) {
      pipe_mgr_create_all_fields_dict(dev, pipe_idx, s_stage, dir);
    }
  }
  /* Set the default fsm states to disabled */
  pipe_mgr_reevaluate_program_fsm_states(*hdl);
  /* Clear any trigger fields and set phvs to match on anything */
  bf_snapshot_capture_trigger_fields_clr(*hdl);

done:
  pipe_mgr_api_exit(shdl);
  return status;
}

/* Snapshot Handle get */
bf_status_t bf_snapshot_handle_get(bf_dev_id_t dev,
                                   bf_dev_pipe_t pipe,
                                   dev_stage_t s_stage,
                                   dev_stage_t e_stage,
                                   bf_snapshot_dir_t *dir,
                                   pipe_snapshot_hdl_t *hdl) {
  pipe_snap_hdl_info_t *hdl_info = NULL;
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return BF_INVALID_ARG;

  if (!PIPE_MGR_SNAP_DEV_INFO(dev)) {
    LOG_ERROR("Device %d not found ", dev);
    return BF_INVALID_ARG;
  }
  if (!snap_pipe_valid(dev_info, pipe)) {
    LOG_ERROR("Invalid pipe %d ", pipe);
    return BF_INVALID_ARG;
  }
  if (!snap_stage_valid(dev_info, s_stage) ||
      !snap_stage_valid(dev_info, e_stage) || (e_stage < s_stage)) {
    LOG_ERROR("Invalid stage ");
    return BF_INVALID_ARG;
  }

  /* Try all both directions */
  for (*dir = BF_SNAPSHOT_DIR_INGRESS; *dir <= BF_SNAPSHOT_DIR_EGRESS;
       (*dir)++) {
    pipe_mgr_snapshot_dev_params_to_hdl(dev, pipe, s_stage, e_stage, *dir, hdl);
    /* If snapshot handle already exists, return the status */
    hdl_info = pipe_mgr_snap_hdl_info_get(dev, *hdl);
    if (hdl_info) {
      return BF_SUCCESS;
    }
  }
  return BF_INVALID_ARG;
}

/* Snapshot handles get */
bf_status_t bf_snapshot_next_handles_get(bf_dev_id_t dev,
                                         pipe_snapshot_hdl_t hdl,
                                         int n,
                                         int *next_handles) {
  pipe_snap_hdl_info_t *hdl_info = NULL;
  bf_dev_pipe_t pipe = 0, next_pipe = 0;
  bf_map_sts_t map_sts = BF_MAP_OK;
  unsigned long key = 0;
  int i = 0;

  if (next_handles == NULL) return BF_INVALID_ARG;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return BF_INVALID_ARG;

  // If hdl is 0, this function is supposed to return all handles.
  if (hdl == 0) {
    map_sts = bf_map_get_first(
        &(PIPE_MGR_SNAP_HDL_MAP(dev)), &key, (void **)&hdl_info);
    next_handles[i] = key;
    i++;
  } else {
    pipe_mgr_snapshot_hdl_to_dev_params(hdl, NULL, &pipe, NULL, NULL, NULL);
    hdl_info = pipe_mgr_snap_hdl_info_get(dev, hdl);
    if (!hdl_info) return BF_OBJECT_NOT_FOUND;
    key = hdl;
  }

  map_sts =
      bf_map_get_next(&(PIPE_MGR_SNAP_HDL_MAP(dev)), &key, (void **)&hdl_info);
  while (map_sts == BF_MAP_OK && i < n) {
    pipe_mgr_snapshot_hdl_to_dev_params(
        key, NULL, &next_pipe, NULL, NULL, NULL);
    if (next_pipe == pipe || hdl == 0) {
      next_handles[i] = key;
      i++;
    }
    /* Get next handle */
    map_sts = bf_map_get_next(
        &(PIPE_MGR_SNAP_HDL_MAP(dev)), &key, (void **)&hdl_info);
  }
  return BF_SUCCESS;
}

/* Snapshot usage get */
bf_status_t bf_snapshot_usage_get(bf_dev_id_t dev,
                                  bf_dev_pipe_t pipe,
                                  uint32_t *count) {
  pipe_snap_hdl_info_t *hdl_info = NULL;
  bf_dev_pipe_t ent_pipe = 0;
  bf_map_sts_t map_sts = BF_MAP_OK;
  unsigned long key = 0;
  uint32_t i = 0;

  if (!PIPE_MGR_SNAP_DEV_INFO(dev)) {
    LOG_ERROR("Device %d not found ", dev);
    return BF_INVALID_ARG;
  }

  map_sts =
      bf_map_get_first(&(PIPE_MGR_SNAP_HDL_MAP(dev)), &key, (void **)&hdl_info);
  while (map_sts == BF_MAP_OK) {
    pipe_mgr_snapshot_hdl_to_dev_params(key, NULL, &ent_pipe, NULL, NULL, NULL);
    if (ent_pipe == pipe || ent_pipe == BF_DEV_PIPE_ALL) {
      i++;
    }
    /* Get next handle */
    map_sts = bf_map_get_next(
        &(PIPE_MGR_SNAP_HDL_MAP(dev)), &key, (void **)&hdl_info);
  }
  *count = i;
  return BF_SUCCESS;
}

/* Delete snapshot entry internal function.
 * This function must be called after pipe_mgr_api_enter()
 * call was done. */
static pipe_status_t snapshot_del_entry(pipe_snapshot_hdl_t hdl,
                                        rmt_dev_info_t *dev_info) {
  bf_map_sts_t map_sts = BF_MAP_OK;
  bf_status_t status = BF_SUCCESS;
  bf_dev_id_t dev = dev_info->dev_id;
  bf_dev_pipe_t pipe = 0;
  dev_stage_t s_stage = 0, e_stage = 0;
  int dir = 0;
  bf_dev_pipe_t start_pipe = 0, pipe_count = 0, pipe_idx = 0;
  void *data = NULL;

  status = pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, &pipe, &s_stage, &e_stage, &dir);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Could not convert snapshot hdl 0x%x to dev params", hdl);
    return status;
  }

  if (pipe == BF_DEV_PIPE_ALL) {
    start_pipe = 0;
    pipe_count = pipe_mgr_snapshot_num_active_pipes(dev);
  } else {
    start_pipe = pipe;
    pipe_count = 1;
  }

  /* Update snapshot DB */
  for (pipe_idx = start_pipe; pipe_idx < (start_pipe + pipe_count);
       pipe_idx++) {
    PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, s_stage, dir).hdl_created = false;
    PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, s_stage, dir).hdl = 0;
    PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, s_stage, dir).end_stage = 0;
    PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, s_stage, dir).admin_state =
        BF_SNAPSHOT_ST_DISABLED;
    /* Disable the timer state */
    if (PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, s_stage, dir).timer_enabled) {
      pipe_mgr_snapshot_cfg_set(
          dev_info, pipe_idx, s_stage, dir, true, BF_SNAPSHOT_IGM_INGRESS);
    }
    PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, s_stage, dir).timer_enabled = false;
  }

  /* Re-evaulate and program the fsm state for affected stages */
  bf_snapshot_state_set(hdl, BF_SNAPSHOT_ST_DISABLED, 0);

  /* Clear any trigger fields and set phvs to not match */
  bf_snapshot_capture_trigger_fields_clr(hdl);

  LOG_TRACE("Deleting from map: handle 0x%x ", hdl);
  map_sts = bf_map_get_rmv(&(PIPE_MGR_SNAP_HDL_MAP(dev)), hdl, &data);
  if (map_sts != BF_MAP_OK) {
    LOG_ERROR("Error when removing hdl %d from map", hdl);
  } else {
    pipe_snap_hdl_info_t *hdl_info = (pipe_snap_hdl_info_t *)data;
    /* Free field memory */
    if (hdl_info->trig_fields) {
      PIPE_MGR_FREE(hdl_info->trig_fields);
    }
    PIPE_MGR_FREE(data);
  }
  data = NULL;

  return (map_sts == BF_MAP_OK) ? PIPE_SUCCESS : PIPE_INVALID_ARG;
}

/* Snapshot delete */
bf_status_t bf_snapshot_delete(pipe_snapshot_hdl_t hdl) {
  pipe_status_t status;
  bf_dev_id_t dev = 0;
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return BF_INVALID_ARG;

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return BF_INVALID_ARG;
  }

  pipe_sess_hdl_t shdl = pipe_mgr_ctx->int_ses_hndl;
  status = pipe_mgr_api_enter(shdl);
  if (status != PIPE_SUCCESS) return status;

  bf_status_t sts = snapshot_del_entry(hdl, dev_info);

  pipe_mgr_api_exit(shdl);
  return (sts == PIPE_SUCCESS) ? BF_SUCCESS : BF_INVALID_ARG;
}

/* Snapshot clear */
bf_status_t bf_snapshot_clear(bf_dev_id_t dev) {
  unsigned long key = 0;
  void *data = NULL;
  int num_trig = 0;
  bf_status_t sts = BF_SUCCESS;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return BF_INVALID_ARG;

  pipe_sess_hdl_t shdl = pipe_mgr_ctx->int_ses_hndl;
  pipe_status_t status = pipe_mgr_api_enter(shdl);
  if (status != PIPE_SUCCESS) return status;

  bf_map_sts_t map_sts =
      bf_map_get_first(&(PIPE_MGR_SNAP_HDL_MAP(dev)), &key, &data);

  while (map_sts == BF_MAP_OK) {
    pipe_snapshot_hdl_t hdl = key;

    bf_snapshot_num_trig_fields_get(hdl, &num_trig);
    if (num_trig != 0 || sts != BF_SUCCESS) {
      LOG_ERROR("Snapshot instance used 0x%x", hdl);
      pipe_mgr_api_exit(shdl);
      return PIPE_INVALID_ARG;
    }

    sts = snapshot_del_entry(hdl, dev_info);
    if (sts != BF_SUCCESS) {
      break;
    }

    /* bf_snapshot_delete will remove entry from the map */
    map_sts = bf_map_get_first(&(PIPE_MGR_SNAP_HDL_MAP(dev)), &key, &data);
  }

  pipe_mgr_api_exit(shdl);
  return sts;
}

/* Encode the fields into the phv spec */
static pipe_status_t snap_encode(pipe_snapshot_hdl_t hdl,
                                 rmt_dev_info_t *dev_info,
                                 bf_dev_pipe_t pipe,
                                 dev_stage_t stage,
                                 bf_snapshot_dir_t dir,
                                 pipe_mgr_phv_spec_t *phv_spec,
                                 pipe_mgr_phv_spec_t *phv_words_updated) {
  pipe_status_t status = PIPE_SUCCESS;
  bf_dev_id_t dev = dev_info->dev_id;
  uint32_t cntr = 0, cntr_idx = 0;
  uint32_t cntr_idx_max_32 = 0, cntr_idx_max_16 = 0, cntr_idx_max_8 = 0;
  uint32_t idx = 0;
  pipe_snap_hdl_info_t *hdl_info = NULL;
  pipe_snap_stage_field_info_t *field_details = NULL;
  pipe_snap_stage_info_t *stage_info = NULL;

  hdl_info = pipe_mgr_snap_hdl_info_get(dev, hdl);
  if (!hdl_info) {
    LOG_ERROR("Handle info is null for handle 0x%x", hdl);
    return PIPE_INVALID_ARG;
  }
  stage_info = &(PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, dir));

  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      cntr_idx_max_32 = PIPE_MGR_TOF_NUM_32BIT_PHV;
      cntr_idx_max_16 = PIPE_MGR_TOF_NUM_16BIT_PHV;
      cntr_idx_max_8 = PIPE_MGR_TOF_NUM_8BIT_PHV;
      break;
    case BF_DEV_FAMILY_TOFINO2:
      cntr_idx_max_32 = PIPE_MGR_TOF2_NUM_32BIT_CAP_PHV;
      cntr_idx_max_16 = PIPE_MGR_TOF2_NUM_16BIT_CAP_PHV;
      cntr_idx_max_8 = PIPE_MGR_TOF2_NUM_8BIT_CAP_PHV;
      break;
    case BF_DEV_FAMILY_TOFINO3:
      cntr_idx_max_32 = PIPE_MGR_TOF3_NUM_32BIT_CAP_PHV;
      cntr_idx_max_16 = PIPE_MGR_TOF3_NUM_16BIT_CAP_PHV;
      cntr_idx_max_8 = PIPE_MGR_TOF3_NUM_8BIT_CAP_PHV;
      break;
    default:
      LOG_ERROR("Invalid dev family %d", dev);
      return PIPE_UNEXPECTED;
  }

  /* Set all the phvs allocated in this direction */
  for (cntr = 0; cntr < stage_info->num_fields_in_dict; cntr++) {
    field_details = &(stage_info->all_fields_dict[cntr]);
    if (!field_details->valid) {
      continue;
    }
    if (field_details->container_type == PIPE_MGR_DARK_PHV) {  // dark
      continue;
    }
    if (field_details->container_width == 32) {
      cntr_idx = field_details->container_num - phv_spec->base_32;
      if (cntr_idx >= cntr_idx_max_32) {
        LOG_ERROR(
            "Invalid PHV 32 index %d, container number %d, max index can be "
            "%d, dev %d",
            cntr_idx,
            field_details->container_num,
            (cntr_idx_max_32 - 1),
            dev);
        PIPE_MGR_DBGCHK(cntr_idx < cntr_idx_max_32);
      }
      phv_words_updated->phvs32bit_lo[cntr_idx] = 1;
      phv_words_updated->phvs32bit_hi[cntr_idx] = 1;
    } else if (field_details->container_width == 16) {
      cntr_idx = field_details->container_num - phv_spec->base_16;
      if (cntr_idx >= cntr_idx_max_16) {
        LOG_ERROR(
            "Invalid PHV 16 index %d, container number %d, max index can be "
            "%d, dev %d",
            cntr_idx,
            field_details->container_num,
            (cntr_idx_max_16 - 1),
            dev);
        PIPE_MGR_DBGCHK(cntr_idx < cntr_idx_max_16);
      }
      phv_words_updated->phvs16bit[cntr_idx] = 1;
    } else if (field_details->container_width == 8) {
      cntr_idx = field_details->container_num - phv_spec->base_8;
      if (cntr_idx >= cntr_idx_max_8) {
        LOG_ERROR(
            "Invalid PHV 8 index %d, container number %d, max index can be %d, "
            "dev %d",
            cntr_idx,
            field_details->container_num,
            (cntr_idx_max_8 - 1),
            dev);
        PIPE_MGR_DBGCHK(cntr_idx < cntr_idx_max_8);
      }
      phv_words_updated->phvs8bit[cntr_idx] = 1;
    }
  }

  for (idx = 0; idx < hdl_info->num_trig_fields; idx++) {
    if (!PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).valid) {
      continue;
    }
    /* A field might be split in multiple containers */
    for (cntr = 0; cntr < stage_info->num_fields_in_dict; cntr++) {
      field_details = &(stage_info->all_fields_dict[cntr]);
      if (!field_details->valid) {
        continue;
      }
      if (field_details->container_type == 2) {  // dark
        continue;
      }
      if (strcmp(field_details->name,
                 PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).name) != 0) {
        continue;
      }

      PIPE_SNAP_DBG(
          "Field %s: f_lsb %d, f_msb %d, p_lsb %d, p_msb %d, cntr_num %d, "
          "cntr_width %d \n",
          field_details->name,
          field_details->field_lsb,
          field_details->field_msb,
          field_details->phv_lsb,
          field_details->phv_msb,
          field_details->container_num,
          field_details->container_width);
      status = pipe_mgr_ctxjson_snapshot_encode(
          dev_info->dev_family,
          stage,
          dir,
          field_details,
          &(PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx)),
          phv_spec,
          phv_words_updated);
    }
  }

  return status;
}

/* Get number of trigger fields in this handle */
pipe_status_t bf_snapshot_num_trig_fields_get(pipe_snapshot_hdl_t hdl,
                                              int *count) {
  int cnt = 0;
  uint32_t idx = 0;
  bf_dev_id_t dev = 0;
  pipe_snap_hdl_info_t *hdl_info = NULL;

  pipe_status_t status =
      pipe_mgr_snapshot_hdl_to_dev_params(hdl, &dev, NULL, NULL, NULL, NULL);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Could not convert snapshot hdl 0x%x to dev params", hdl);
    return status;
  }

  hdl_info = pipe_mgr_snap_hdl_info_get(dev, hdl);
  if (!hdl_info) {
    LOG_ERROR("Handle info is null for handle 0x%x", hdl);
    return PIPE_INVALID_ARG;
  }

  for (idx = 0; idx < hdl_info->num_trig_fields; idx++) {
    if (!PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).valid) {
      continue;
    }
    cnt++;
  }

  *count = cnt;
  return PIPE_SUCCESS;
}

static inline pipe_status_t snap_phv_spec_populate(bf_dev_family_t family,
                                                   pipe_mgr_phv_spec_t *spec) {
  switch (family) {
    case BF_DEV_FAMILY_TOFINO:
      spec->phvs32bit_lo =
          PIPE_MGR_CALLOC(PIPE_MGR_TOF_NUM_32BIT_PHV, sizeof(uint32_t));
      spec->phvs32bit_hi =
          PIPE_MGR_CALLOC(PIPE_MGR_TOF_NUM_32BIT_PHV, sizeof(uint32_t));
      spec->phvs8bit =
          PIPE_MGR_CALLOC(PIPE_MGR_TOF_NUM_8BIT_PHV, sizeof(uint32_t));
      spec->phvs16bit =
          PIPE_MGR_CALLOC(PIPE_MGR_TOF_NUM_16BIT_PHV, sizeof(uint32_t));
      if (!spec->phvs32bit_lo || !spec->phvs32bit_hi || !spec->phvs8bit ||
          !spec->phvs16bit) {
        if (spec->phvs32bit_lo) PIPE_MGR_FREE(spec->phvs32bit_lo);
        if (spec->phvs32bit_hi) PIPE_MGR_FREE(spec->phvs32bit_hi);
        if (spec->phvs8bit) PIPE_MGR_FREE(spec->phvs8bit);
        if (spec->phvs16bit) PIPE_MGR_FREE(spec->phvs16bit);
        spec->phvs32bit_lo = NULL;
        spec->phvs32bit_hi = NULL;
        spec->phvs8bit = NULL;
        spec->phvs16bit = NULL;
        return PIPE_NO_SYS_RESOURCES;
      }
      spec->base_32 = 0;
      spec->base_8 = PIPE_MGR_TOF_NUM_32BIT_PHV;
      spec->base_16 = PIPE_MGR_TOF_NUM_32BIT_PHV + PIPE_MGR_TOF_NUM_8BIT_PHV;
      spec->phv_count = 224;
      return PIPE_SUCCESS;
    case BF_DEV_FAMILY_TOFINO2:
      spec->phvs32bit_lo =
          PIPE_MGR_CALLOC(PIPE_MGR_TOF2_NUM_32BIT_CAP_PHV, sizeof(uint32_t));
      spec->phvs32bit_hi =
          PIPE_MGR_CALLOC(PIPE_MGR_TOF2_NUM_32BIT_CAP_PHV, sizeof(uint32_t));
      spec->phvs8bit =
          PIPE_MGR_CALLOC(PIPE_MGR_TOF2_NUM_8BIT_CAP_PHV, sizeof(uint32_t));
      spec->phvs16bit =
          PIPE_MGR_CALLOC(PIPE_MGR_TOF2_NUM_16BIT_CAP_PHV, sizeof(uint32_t));
      if (!spec->phvs32bit_lo || !spec->phvs32bit_hi || !spec->phvs8bit ||
          !spec->phvs16bit) {
        if (spec->phvs32bit_lo) PIPE_MGR_FREE(spec->phvs32bit_lo);
        if (spec->phvs32bit_hi) PIPE_MGR_FREE(spec->phvs32bit_hi);
        if (spec->phvs8bit) PIPE_MGR_FREE(spec->phvs8bit);
        if (spec->phvs16bit) PIPE_MGR_FREE(spec->phvs16bit);
        spec->phvs32bit_lo = NULL;
        spec->phvs32bit_hi = NULL;
        spec->phvs8bit = NULL;
        spec->phvs16bit = NULL;
        return PIPE_NO_SYS_RESOURCES;
      }
      spec->base_32 = 0;
      spec->base_8 = PIPE_MGR_TOF2_NUM_32BIT_CAP_PHV;
      spec->base_16 =
          PIPE_MGR_TOF2_NUM_32BIT_CAP_PHV + PIPE_MGR_TOF2_NUM_8BIT_CAP_PHV;
      spec->phv_count = spec->base_16 + PIPE_MGR_TOF2_NUM_16BIT_CAP_PHV;
      return PIPE_SUCCESS;

    case BF_DEV_FAMILY_TOFINO3:
      spec->phvs32bit_lo =
          PIPE_MGR_CALLOC(PIPE_MGR_TOF3_NUM_32BIT_CAP_PHV, sizeof(uint32_t));
      spec->phvs32bit_hi =
          PIPE_MGR_CALLOC(PIPE_MGR_TOF3_NUM_32BIT_CAP_PHV, sizeof(uint32_t));
      spec->phvs8bit =
          PIPE_MGR_CALLOC(PIPE_MGR_TOF3_NUM_8BIT_CAP_PHV, sizeof(uint32_t));
      spec->phvs16bit =
          PIPE_MGR_CALLOC(PIPE_MGR_TOF3_NUM_16BIT_CAP_PHV, sizeof(uint32_t));
      if (!spec->phvs32bit_lo || !spec->phvs32bit_hi || !spec->phvs8bit ||
          !spec->phvs16bit) {
        if (spec->phvs32bit_lo) PIPE_MGR_FREE(spec->phvs32bit_lo);
        if (spec->phvs32bit_hi) PIPE_MGR_FREE(spec->phvs32bit_hi);
        if (spec->phvs8bit) PIPE_MGR_FREE(spec->phvs8bit);
        if (spec->phvs16bit) PIPE_MGR_FREE(spec->phvs16bit);
        spec->phvs32bit_lo = NULL;
        spec->phvs32bit_hi = NULL;
        spec->phvs8bit = NULL;
        spec->phvs16bit = NULL;
        return PIPE_NO_SYS_RESOURCES;
      }
      spec->base_32 = 0;
      spec->base_8 = PIPE_MGR_TOF3_NUM_32BIT_CAP_PHV;
      spec->base_16 =
          PIPE_MGR_TOF3_NUM_32BIT_CAP_PHV + PIPE_MGR_TOF3_NUM_8BIT_CAP_PHV;
      spec->phv_count = spec->base_16 + PIPE_MGR_TOF3_NUM_16BIT_CAP_PHV;
      return PIPE_SUCCESS;
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }
}

static inline void snap_phv_spec_clean(bf_dev_family_t family,
                                       pipe_mgr_phv_spec_t *spec) {
  (void)family;
  if (spec->phvs32bit_lo) PIPE_MGR_FREE(spec->phvs32bit_lo);
  if (spec->phvs32bit_hi) PIPE_MGR_FREE(spec->phvs32bit_hi);
  if (spec->phvs8bit) PIPE_MGR_FREE(spec->phvs8bit);
  if (spec->phvs16bit) PIPE_MGR_FREE(spec->phvs16bit);
  spec->phvs32bit_lo = NULL;
  spec->phvs32bit_hi = NULL;
  spec->phvs8bit = NULL;
  spec->phvs16bit = NULL;
  spec->base_32 = 0;
  spec->base_8 = 0;
  spec->base_16 = 0;
  spec->phv_count = 0;
}

/* Snapshot trigger set helper */
static pipe_status_t snapshot_capture_trigger_set(pipe_snapshot_hdl_t hdl,
                                                  bf_dev_id_t dev,
                                                  bf_dev_pipe_t pipe,
                                                  dev_stage_t stage,
                                                  bf_snapshot_dir_t dir) {
  pipe_status_t status = PIPE_SUCCESS;
  bf_dev_pipe_t start_pipe = 0, pipe_count = 0, pipe_idx = 0;
  int idx = 0, mask = 0;
  pipe_mgr_phv_spec_t phv_spec[2];
  pipe_mgr_phv_spec_t phv_words_updated;
  bool no_match = false;

  /* By default set all fields to match on anything (0/0) even if there are no
     valid fields specified by the user
   */
  no_match = false;
  /* If trigger is invalid (happens when oper-stage is different than
     start-stage) then set all fields to match on 0 (1/0)
   */
  if (pipe_mgr_snapshot_capture_trigger_fields_validate(hdl, stage) !=
      PIPE_SUCCESS) {
    no_match = true;
  }

  /* If timer is enabled then set all fields to match on 0 (1/0) */
  if (PIPE_MGR_SNAP_TIMER_VAL_VALID(
          PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, stage, dir).timer_val)) {
    no_match = true;
  }

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) {
    PIPE_MGR_DBGCHK(dev_info);
    return PIPE_INVALID_ARG;
  }

  if (pipe == BF_DEV_PIPE_ALL) {
    start_pipe = 0;
    pipe_count = dev_info->num_active_pipes;
  } else {
    start_pipe = pipe;
    pipe_count = 1;
  }

  snap_phv_spec_populate(dev_info->dev_family, &phv_spec[0]);
  snap_phv_spec_populate(dev_info->dev_family, &phv_spec[1]);
  snap_phv_spec_populate(dev_info->dev_family, &phv_words_updated);

  for (pipe_idx = start_pipe; pipe_idx < (start_pipe + pipe_count);
       pipe_idx++) {
    switch (dev_info->dev_family) {
      case BF_DEV_FAMILY_TOFINO:
        /* Init phv spec to all 1's if trigger is valid */
        for (mask = 0; mask < 2; mask++) {
          for (idx = 0; idx < PIPE_MGR_TOF_NUM_8BIT_PHV; idx++) {
            phv_spec[mask].phvs8bit[idx] = 0x1ff;
            if ((no_match) && (mask == 1)) {
              phv_spec[mask].phvs8bit[idx] = 0;
            }
          }
          for (idx = 0; idx < PIPE_MGR_TOF_NUM_16BIT_PHV; idx++) {
            phv_spec[mask].phvs16bit[idx] = 0x1ffff;
            if ((no_match) && (mask == 1)) {
              phv_spec[mask].phvs16bit[idx] = 0;
            }
          }
          for (idx = 0; idx < PIPE_MGR_TOF_NUM_32BIT_PHV; idx++) {
            phv_spec[mask].phvs32bit_lo[idx] = 0xffff;
            phv_spec[mask].phvs32bit_hi[idx] = 0x1ffff;
            if ((no_match) && (mask == 1)) {
              phv_spec[mask].phvs32bit_lo[idx] = 0;
              phv_spec[mask].phvs32bit_hi[idx] = 0;
            }
          }
        }

        /* Encode the fields into phvs */
        snap_encode(
            hdl, dev_info, pipe_idx, stage, dir, phv_spec, &phv_words_updated);

        /* Update hardware */
        status = pipe_mgr_snapshot_capture_trigger_set_tof(
            dev_info, pipe_idx, stage, phv_spec);
        break;
      case BF_DEV_FAMILY_TOFINO2:
        /* Init phv spec to all 1's if trigger is valid */
        for (mask = 0; mask < 2; mask++) {
          for (idx = 0; idx < PIPE_MGR_TOF2_NUM_8BIT_CAP_PHV; idx++) {
            phv_spec[mask].phvs8bit[idx] = 0x1ff;
            if ((no_match) && (mask == 1)) {
              phv_spec[mask].phvs8bit[idx] = 0;
            }
          }
          for (idx = 0; idx < PIPE_MGR_TOF2_NUM_16BIT_CAP_PHV; idx++) {
            phv_spec[mask].phvs16bit[idx] = 0x1ffff;
            if ((no_match) && (mask == 1)) {
              phv_spec[mask].phvs16bit[idx] = 0;
            }
          }
          for (idx = 0; idx < PIPE_MGR_TOF2_NUM_32BIT_CAP_PHV; idx++) {
            phv_spec[mask].phvs32bit_lo[idx] = 0xffff;
            phv_spec[mask].phvs32bit_hi[idx] = 0x1ffff;
            if ((no_match) && (mask == 1)) {
              phv_spec[mask].phvs32bit_lo[idx] = 0;
              phv_spec[mask].phvs32bit_hi[idx] = 0;
            }
          }
        }

        /* Encode the fields into phvs */
        snap_encode(
            hdl, dev_info, pipe_idx, stage, dir, phv_spec, &phv_words_updated);

        status = pipe_mgr_snapshot_capture_trigger_set_tof2(
            dev_info, pipe_idx, stage, phv_spec);
        break;
      case BF_DEV_FAMILY_TOFINO3:
        /* Init phv spec to all 1's if trigger is valid */
        for (mask = 0; mask < 2; mask++) {
          for (idx = 0; idx < PIPE_MGR_TOF3_NUM_8BIT_CAP_PHV; idx++) {
            phv_spec[mask].phvs8bit[idx] = 0x1ff;
            if ((no_match) && (mask == 1)) {
              phv_spec[mask].phvs8bit[idx] = 0;
            }
          }
          for (idx = 0; idx < PIPE_MGR_TOF3_NUM_16BIT_CAP_PHV; idx++) {
            phv_spec[mask].phvs16bit[idx] = 0x1ffff;
            if ((no_match) && (mask == 1)) {
              phv_spec[mask].phvs16bit[idx] = 0;
            }
          }
          for (idx = 0; idx < PIPE_MGR_TOF3_NUM_32BIT_CAP_PHV; idx++) {
            phv_spec[mask].phvs32bit_lo[idx] = 0xffff;
            phv_spec[mask].phvs32bit_hi[idx] = 0x1ffff;
            if ((no_match) && (mask == 1)) {
              phv_spec[mask].phvs32bit_lo[idx] = 0;
              phv_spec[mask].phvs32bit_hi[idx] = 0;
            }
          }
        }

        /* Encode the fields into phvs */
        snap_encode(
            hdl, dev_info, pipe_idx, stage, dir, phv_spec, &phv_words_updated);

        status = pipe_mgr_snapshot_capture_trigger_set_tof3(
            dev_info, pipe_idx, stage, phv_spec);
        break;

      default:
        PIPE_MGR_DBGCHK(0);
        return PIPE_UNEXPECTED;
    }
  }
  snap_phv_spec_clean(dev_info->dev_family, &phv_spec[0]);
  snap_phv_spec_clean(dev_info->dev_family, &phv_spec[1]);
  snap_phv_spec_clean(dev_info->dev_family, &phv_words_updated);

  return status;
}

/* Snapshot trigger set helper */
bf_status_t bf_snapshot_capture_trigger_set(pipe_snapshot_hdl_t hdl,
                                            void *trig_spec,
                                            void *trig_mask) {
  bf_status_t status = BF_SUCCESS;
  bf_dev_id_t dev = 0;
  bf_dev_pipe_t pipe = 0;
  dev_stage_t s_stage = 0, e_stage = 0, stage = 0;
  int dir = 0;
  pipe_snap_hdl_info_t *hdl_info = NULL;
  bool oper_stage_set = false;

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return BF_INVALID_ARG;
  }
  status = pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, &pipe, &s_stage, &e_stage, &dir);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Could not convert snapshot hdl 0x%x to dev params", hdl);
    return status;
  }

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  profile_id_t prof_id = 0;
  if (pipe_mgr_pipe_to_profile(dev_info, pipe, &prof_id, __func__, __LINE__) !=
      PIPE_SUCCESS) {
    return PIPE_INVALID_ARG;
  }

  pipe_sess_hdl_t shdl = pipe_mgr_ctx->int_ses_hndl;
  status = pipe_mgr_api_enter(shdl);
  if (status != PIPE_SUCCESS) return status;

  /* Clear any existing fields */
  pipe_mgr_snapshot_capture_trigger_fields_clr(hdl);
  hdl_info = pipe_mgr_snap_hdl_info_get(dev, hdl);
  if (!hdl_info) {
    LOG_ERROR("Handle info is null for handle 0x%x", hdl);
    return PIPE_INVALID_ARG;
  }

  /* Get the P4 field names and mask+value pairs */
  status = pipe_mgr_ctxjson_snapshot_trig_field_set_from_pd(
      hdl,
      dev,
      prof_id,
      dir,
      trig_spec,
      trig_mask,
      &PIPE_MGR_SNAP_HDL_FIELD(hdl_info, 0),
      hdl_info->num_trig_fields);
  if (status != PIPE_SUCCESS) {
    pipe_mgr_snapshot_capture_trigger_fields_clr(hdl);
    status = BF_OBJECT_NOT_FOUND;
    goto done;
  }

  /* Validate that the fields exists in that pipe, stage */
  for (stage = s_stage; stage <= e_stage; stage++) {
    status = pipe_mgr_snapshot_capture_trigger_fields_validate(hdl, stage);
    if (status == PIPE_SUCCESS) {
      /* Find the first stage this match spec is valid - set oper_stage */
      if (!oper_stage_set) {
        hdl_info->oper_stage = stage;
        oper_stage_set = true;
        break;
      }
    }
  }

  if (!oper_stage_set) {
    pipe_mgr_snapshot_capture_trigger_fields_clr(hdl);
    status = BF_INVALID_ARG;
    goto done;
  }

  /* Re-evaulate fsm state's for all stages as oper-stage might have changed */
  pipe_mgr_reevaluate_program_fsm_states(hdl);

  for (stage = s_stage; stage <= e_stage; stage++) {
    status |= snapshot_capture_trigger_set(hdl, dev, pipe, stage, dir);
  }

done:
  pipe_mgr_api_exit(shdl);
  return status;
}

/* Snapshot fsm set helper */
static pipe_status_t pipe_mgr_snapshot_fsm_state_set(
    bf_dev_id_t dev,
    bf_dev_pipe_t pipe,
    dev_stage_t stage,
    bf_snapshot_dir_t dir,
    pipe_snapshot_fsm_state_t fsm_state) {
  pipe_status_t status = PIPE_SUCCESS;
  bf_dev_pipe_t start_pipe = 0, pipe_count = 0, pipe_idx = 0;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) {
    PIPE_MGR_DBGCHK(dev_info);
    return PIPE_INVALID_ARG;
  }

  if (pipe == BF_DEV_PIPE_ALL) {
    start_pipe = 0;
    pipe_count = dev_info->num_active_pipes;
  } else {
    start_pipe = pipe;
    pipe_count = 1;
  }

  if (!PIPE_MGR_SNAP_DIR_VALID(dir)) {
    return PIPE_INVALID_ARG;
  }

  for (pipe_idx = start_pipe; pipe_idx < (start_pipe + pipe_count);
       pipe_idx++) {
    switch (dev_info->dev_family) {
      case BF_DEV_FAMILY_TOFINO:
        status = pipe_mgr_snapshot_fsm_state_set_tof(
            dev_info, pipe_idx, stage, dir, fsm_state);
        break;
      case BF_DEV_FAMILY_TOFINO2:
        status = pipe_mgr_snapshot_fsm_state_set_tof2(
            dev_info, pipe_idx, stage, dir, fsm_state);
        break;
      case BF_DEV_FAMILY_TOFINO3:
        status = pipe_mgr_snapshot_fsm_state_set_tof3(
            dev_info, pipe_idx, stage, dir, fsm_state);
        break;
      default:
        PIPE_MGR_DBGCHK(0);
        return PIPE_UNEXPECTED;
    }
  }

  return status;
}

/* Snapshot fsm state set */
bf_status_t bf_snapshot_state_set(pipe_snapshot_hdl_t hdl,
                                  bf_snapshot_state_t en_state,
                                  uint32_t usec) {
  bf_status_t status = BF_SUCCESS;
  bf_dev_id_t dev = 0;
  bf_dev_pipe_t pipe = 0;
  bf_dev_pipe_t start_pipe = 0, pipe_count = 0, pipe_idx = 0;
  dev_stage_t s_stage = 0, e_stage = 0;
  int dir = 0;

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return BF_INVALID_ARG;
  }
  status = pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, &pipe, &s_stage, &e_stage, &dir);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Could not convert snapshot hdl 0x%x to dev params", hdl);
    return status;
  }
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return PIPE_INVALID_ARG;
  if (pipe == BF_DEV_PIPE_ALL) {
    start_pipe = 0;
    pipe_count = pipe_mgr_snapshot_num_active_pipes(dev);
  } else {
    start_pipe = pipe;
    pipe_count = 1;
  }

  pipe_sess_hdl_t shdl = pipe_mgr_ctx->int_ses_hndl;
  status = pipe_mgr_api_enter(shdl);
  if (status != PIPE_SUCCESS) return status;

  for (pipe_idx = start_pipe; pipe_idx < (start_pipe + pipe_count);
       pipe_idx++) {
    PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, s_stage, dir).admin_state =
        en_state;
    PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, s_stage, dir).timer_val = usec;
  }

  /* Re-evaulate the fsm state for all stages */
  pipe_mgr_reevaluate_program_fsm_states(hdl);

  if (pipe_mgr_snapshot_timer_set(hdl, dev_info, pipe, s_stage, usec) !=
      PIPE_SUCCESS) {
    status = BF_INVALID_ARG;
  }

  /* Reset the cached timer value to zero once it has been set in asic */
  for (pipe_idx = start_pipe; pipe_idx < (start_pipe + pipe_count);
       pipe_idx++) {
    PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, s_stage, dir).timer_val = 0;
  }

  pipe_mgr_api_exit(shdl);
  return status;
}

/* Snapshot fsm state get helper */
pipe_status_t pipe_mgr_snapshot_fsm_state_get(
    bf_dev_id_t dev,
    bf_dev_pipe_t pipe,
    dev_stage_t stage,
    bf_snapshot_dir_t dir,
    pipe_snapshot_fsm_state_t *fsm_state,
    int *en_state) {
  pipe_status_t status = PIPE_SUCCESS;

  if (pipe == BF_DEV_PIPE_ALL) {
    return PIPE_INVALID_ARG;
  }

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) {
    PIPE_MGR_DBGCHK(dev_info);
    return PIPE_INVALID_ARG;
  }

  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      status = pipe_mgr_snapshot_fsm_state_get_tof(
          dev_info, pipe, stage, dir, fsm_state);
      break;
    case BF_DEV_FAMILY_TOFINO2:
      status = pipe_mgr_snapshot_fsm_state_get_tof2(
          dev_info, pipe, stage, dir, fsm_state);
      break;
    case BF_DEV_FAMILY_TOFINO3:
      status = pipe_mgr_snapshot_fsm_state_get_tof3(
          dev_info, pipe, stage, dir, fsm_state);
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }

  if (*fsm_state == PIPE_SNAPSHOT_FSM_ST_FULL) {
    *en_state = BF_SNAPSHOT_ST_DISABLED;
  } else {
    *en_state = BF_SNAPSHOT_ST_ENABLED;
  }

  return status;
}

/* Get snapshot state per pipe function wrapper */
bf_status_t bf_snapshot_pd_state_get(pipe_snapshot_hdl_t hdl,
                                     bf_dev_pipe_t pipe_input,
                                     int *en_state) {
  bf_status_t status = BF_SUCCESS;
  bf_dev_id_t dev = 0;
  bf_dev_pipe_t pipe = 0;
  dev_stage_t stage = 0;
  int dir = 0;
  pipe_snapshot_fsm_state_t fsm_state = 0;
  pipe_snap_hdl_info_t *hdl_info = NULL;

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return BF_INVALID_ARG;
  }
  status = pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, &pipe, &stage, &stage, &dir);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Could not convert snapshot hdl 0x%x to dev params", hdl);
    return status;
  }
  if (pipe == BF_DEV_PIPE_ALL) {
    pipe = pipe_input;
  }
  hdl_info = pipe_mgr_snap_hdl_info_get(dev, hdl);
  if (!hdl_info) {
    LOG_ERROR("Handle info is null for handle 0x%x", hdl);
    return PIPE_INVALID_ARG;
  }
  if (pipe_mgr_snapshot_fsm_state_get(
          dev, pipe, hdl_info->oper_stage, dir, &fsm_state, en_state) !=
      PIPE_SUCCESS) {
    return BF_INVALID_ARG;
  }
  return status;
}

/* Snapshot state get BRI */
bf_status_t bf_snapshot_state_get(pipe_snapshot_hdl_t hdl,
                                  uint32_t size,
                                  pipe_snapshot_fsm_state_t *fsm_state,
                                  bool *en_state) {
  bf_status_t status = BF_SUCCESS;
  bf_dev_id_t dev = 0;
  bf_dev_pipe_t pipe = 0, pipe_idx = 0;
  uint32_t pipe_count = 0;
  dev_stage_t stage = 0;
  int dir = 0;
  pipe_snap_hdl_info_t *hdl_info = NULL;

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return BF_INVALID_ARG;
  }
  status =
      pipe_mgr_snapshot_hdl_to_dev_params(hdl, &dev, &pipe, &stage, NULL, &dir);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Could not convert snapshot hdl 0x%x to dev params", hdl);
    return status;
  }

  hdl_info = pipe_mgr_snap_hdl_info_get(dev, hdl);
  if (!hdl_info) {
    LOG_ERROR("Handle info is null for handle 0x%x", hdl);
    return PIPE_INVALID_ARG;
  }

  /* If all pipes, go over all */
  if (pipe == BF_DEV_PIPE_ALL) {
    pipe = 0;
    pipe_count = pipe_mgr_snapshot_num_active_pipes(dev);
  } else {
    pipe_count = 1;
  }

  if (size < pipe_count) {
    LOG_ERROR("Provided array size %d will not fit all the data %d",
              size,
              pipe_count);
    return BF_INVALID_ARG;
  }
  /* Clear the input array */
  for (pipe_idx = 0; pipe_idx < size; pipe_idx++) {
    fsm_state[pipe_idx] = PIPE_SNAPSHOT_FSM_ST_MAX;
  }

  int en_state_hw;
  for (pipe_idx = pipe; pipe_idx < (pipe + pipe_count); pipe_idx++) {
    status = pipe_mgr_snapshot_fsm_state_get(
        dev, pipe_idx, hdl_info->oper_stage, dir, fsm_state++, &en_state_hw);
    if (status != BF_SUCCESS) {
      LOG_ERROR(
          "Could fetch snapshot fsm state for hdl %d pipe %d", hdl, pipe_idx);
      return status;
    }
  }

  /* Fetching admin state from lowest pipe */
  if (PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, dir).admin_state) {
    *en_state = true;
  } else {
    *en_state = false;
  }

  return status;
}

/* Snapshot captured data get from ASIC */
static pipe_status_t snapshot_stage_capture_get_from_hw(
    rmt_dev_info_t *dev_info,
    bf_dev_pipe_t pipe,
    dev_stage_t stage,
    bf_snapshot_dir_t dir,
    pipe_mgr_snapshot_capture_data_t *capture) {
  pipe_status_t status = PIPE_SUCCESS;
  pipe_mgr_phv_spec_t *phv_spec = NULL;

  if (pipe == BF_DEV_PIPE_ALL) {
    LOG_ERROR("All pipes is invalid in snapshot get");
    return PIPE_INVALID_ARG;
  }
  if (!PIPE_MGR_SNAP_DIR_VALID(dir)) {
    LOG_ERROR("Invalid dir %d ", dir);
    return PIPE_INVALID_ARG;
  }

  phv_spec = &(capture->phv_spec);

  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      status = pipe_mgr_get_snapshot_captured_data_tof(
          dev_info, pipe, stage, dir, phv_spec, capture);
      break;
    case BF_DEV_FAMILY_TOFINO2:
      status = pipe_mgr_get_snapshot_captured_data_tof2(
          dev_info, pipe, stage, dir, phv_spec, capture);
      break;
    case BF_DEV_FAMILY_TOFINO3:
      status = pipe_mgr_get_snapshot_captured_data_tof3(
          dev_info, pipe, stage, dir, phv_spec, capture);
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }
  return status;
}

/* Get total number of PHV containers */
pipe_status_t bf_snapshot_total_phv_count_get(bf_dev_id_t dev_id,
                                              uint32_t *count) {
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  if (!dev_info) {
    LOG_ERROR("Invalid device %d", dev_id);
    return BF_INVALID_ARG;
  }
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      *count = PIPE_MGR_TOF_NUM_TOTAL_PHV;
      break;
    case BF_DEV_FAMILY_TOFINO2:
      *count = PIPE_MGR_TOF2_NUM_32BIT_CAP_PHV +
               PIPE_MGR_TOF2_NUM_8BIT_CAP_PHV + PIPE_MGR_TOF2_NUM_16BIT_CAP_PHV;
      break;
    case BF_DEV_FAMILY_TOFINO3:
      *count = PIPE_MGR_TOF3_NUM_32BIT_CAP_PHV +
               PIPE_MGR_TOF3_NUM_8BIT_CAP_PHV + PIPE_MGR_TOF3_NUM_16BIT_CAP_PHV;
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }
  return PIPE_SUCCESS;
}

/* Get the snapshot capture data for one stage. */
pipe_status_t bf_snapshot_raw_capture_get(bf_dev_id_t dev_id,
                                          bf_dev_pipe_t pipe,
                                          dev_stage_t stage,
                                          uint32_t size,
                                          uint32_t *phvs,
                                          bool *phvs_v) {
  pipe_mgr_snapshot_capture_data_t pipe_capture;
  pipe_mgr_phv_spec_t *phv_spec = &pipe_capture.phv_spec;
  pipe_status_t status = PIPE_SUCCESS;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  if (!dev_info) {
    LOG_ERROR("Invalid device %d", dev_id);
    return BF_INVALID_ARG;
  }

  if (pipe == BF_DEV_PIPE_ALL) {
    LOG_ERROR("All pipes is invalid in raw snapshot get");
    return PIPE_INVALID_ARG;
  }

  /* Set up the PHV spec. */
  status = snap_phv_spec_populate(dev_info->dev_family, phv_spec);
  if (status) return status;

  if (size < phv_spec->phv_count) {
    LOG_ERROR("Provided array is not big enough to fit all the PHVs.");
    snap_phv_spec_clean(dev_info->dev_family, phv_spec);
    return PIPE_INVALID_ARG;
  }

  /* Get the captured snapshot data from hardware.
   * For fetching PHV values, direction doesn't matter.*/
  status = snapshot_stage_capture_get_from_hw(
      dev_info, pipe, stage, BF_SNAPSHOT_DIR_INGRESS, &pipe_capture);
  if (status) {
    LOG_ERROR(
        "Error while getting snapshot values from hw dev_id: %d, pipe: %d, "
        "stage %d",
        dev_id,
        pipe,
        stage);
    snap_phv_spec_clean(dev_info->dev_family, phv_spec);
    return status;
  }

  int j = phv_spec->base_32;
  for (int i = 0; i < phv_spec->base_8; ++i, ++j) {
    phvs[j] = (phv_spec->phvs32bit_hi[i] << 16) |
              (phv_spec->phvs32bit_lo[i] & 0xFFFF);
    phvs_v[j] = (dev_info->dev_family == BF_DEV_FAMILY_TOFINO)
                    ? (phv_spec->phvs32bit_hi[i] & 0x10000)
                    : true;
  }
  int num_8b = phv_spec->base_16 - phv_spec->base_8;
  for (int i = 0; i < num_8b; ++i, ++j) {
    phvs[j] = phv_spec->phvs8bit[i] & 0xFF;
    phvs_v[j] = (dev_info->dev_family == BF_DEV_FAMILY_TOFINO)
                    ? phv_spec->phvs8bit[i] & 0x100
                    : true;
  }
  int num_16b = phv_spec->phv_count - phv_spec->base_16;
  for (int i = 0; i < num_16b; ++i, ++j) {
    phvs[j] = phv_spec->phvs16bit[i] & 0xFFFF;
    phvs_v[j] = (dev_info->dev_family == BF_DEV_FAMILY_TOFINO)
                    ? phv_spec->phvs16bit[i] & 0x10000
                    : true;
  }

  snap_phv_spec_clean(dev_info->dev_family, &pipe_capture.phv_spec);

  return PIPE_SUCCESS;
}

/* Get the snapshot capture data for one stage. */
static pipe_status_t snapshot_stage_capture_get(
    rmt_dev_info_t *dev_info,
    bf_dev_pipe_t pipe,
    dev_stage_t stage,
    bf_snapshot_dir_t dir,
    pipe_mgr_snapshot_long_branch_t *long_branch,
    uint8_t *data,
    uint8_t *data_v,
    bf_snapshot_capture_ctrl_info_t *ctrl) {
  pipe_status_t status = PIPE_SUCCESS;
  pipe_mgr_snapshot_capture_data_t pipe_capture;
  bf_dev_id_t dev = dev_info->dev_id;
  profile_id_t prof_id = 0;

  PIPE_MGR_MEMSET(&pipe_capture, 0, sizeof(pipe_capture));
  pipe_capture.long_branch = *long_branch;

  if (pipe == BF_DEV_PIPE_ALL) {
    LOG_ERROR("All pipes is invalid in snapshot get");
    return PIPE_INVALID_ARG;
  }

  if (pipe_mgr_pipe_to_profile(dev_info, pipe, &prof_id, __func__, __LINE__) !=
      PIPE_SUCCESS) {
    return PIPE_INVALID_ARG;
  }
  /* Set up the PHV spec. */
  snap_phv_spec_populate(dev_info->dev_family, &pipe_capture.phv_spec);

  /* Get the captured snapshot data from hardware. */
  snapshot_stage_capture_get_from_hw(dev_info, pipe, stage, dir, &pipe_capture);
  /* Copy back to long_branch because it could be initalized in the above
   * function. */
  *long_branch = pipe_capture.long_branch;

  /* Decode the data */
  ctrl->stage_id = stage;
  ctrl->valid = true;

  /* If there is a stage extension in use we could have captures more stage than
   * the context.json has data for.  If that is the case use the PHV decode from
   * the last stage the context.json has information on since the PHV allocation
   * cannot change in the bypass stages. */
  dev_stage_t phv_decode_stage = stage;
  if (stage >= dev_info->profile_info[prof_id]->num_stages) {
    phv_decode_stage = dev_info->profile_info[prof_id]->num_stages - 1;
  }

  status = pipe_mgr_ctxjson_snapshot_decode(dev_info,
                                            prof_id,
                                            pipe,
                                            phv_decode_stage,
                                            dir,
                                            &pipe_capture,
                                            ctrl,
                                            data,
                                            data_v);

  /* Clean up the PHV spec. */
  snap_phv_spec_clean(dev_info->dev_family, &pipe_capture.phv_spec);

  /* No further table decode is needed for bypass stages. */
  if (stage >= dev_info->profile_info[prof_id]->num_stages) {
    return status;
  }

  /* Now that we have decoded the raw snapshot data we can use the decoded per
   * table hit address to find a corresponding entry handle. */

  /* For each logical table in the stage... */
  int log_tbl_id;
  for (log_tbl_id = 0; log_tbl_id < BF_MAX_LOG_TBLS; ++log_tbl_id) {
    bf_snapshot_tables_info_t *ti = &ctrl->tables_info[log_tbl_id];
    /* Initialize the entry handle to a known value. */
    ti->hit_entry_handle = 0;
    /* We only care about hits. */
    if (!ti->table_hit) continue;
    /* We only care about proper match tables. */
    if (PIPE_HDL_TYPE_MAT_TBL != PIPE_GET_HDL_TYPE(ti->table_handle)) continue;
    pipe_mat_ent_hdl_t hit_entry_hdl;
    status = pipe_mgr_tbl_stage_idx_to_hdl(dev,
                                           ti->table_handle,
                                           pipe,
                                           stage,
                                           log_tbl_id,
                                           ti->match_hit_address,
                                           &hit_entry_hdl);
    if (PIPE_SUCCESS != status) {
      LOG_TRACE(
          "Failed to map dev %d tbl 0x%x pipe %d stage %d lt %d hit-addr "
          "0x%x to entry handle, %s",
          dev,
          ti->table_handle,
          pipe,
          stage,
          log_tbl_id,
          ti->match_hit_address,
          pipe_str_err(status));
    } else {
      ti->hit_entry_handle = hit_entry_hdl;
    }
  }
  return status;
}

/* Get the size of the snapshot capture data that needs to be allocated */
static bf_status_t snapshot_capture_phv_fields_dict_size(
    bf_dev_id_t dev, int dir, uint32_t *total_size, uint32_t *per_stage_size) {
  uint32_t curr_size = 0, max_size = 0;
  uint8_t p = 0;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) {
    LOG_ERROR("%s: Invalid device %d", __func__, dev);
    PIPE_MGR_DBGCHK(0);
    return BF_INVALID_ARG;
  }

  /* Get the max size in all pipeline profiles */
  for (p = 0; p < dev_info->num_pipeline_profiles; p++) {
    rmt_dev_profile_info_t *dev_profile_info = dev_info->profile_info[p];

    curr_size = pipe_mgr_ctxjson_phv_fields_dict_size(
        dev, dev_profile_info->profile_id, dir);
    if (curr_size > max_size) {
      max_size = curr_size;
    }
  }

  *per_stage_size = max_size * 2;  // Allocate enough space
  *total_size =
      (*per_stage_size) * dev_info->num_active_mau;  // Allocate for all stages
  /* Add validity bits for PHV for TOFINO. */
  if (dev_info->dev_family == BF_DEV_FAMILY_TOFINO) {
    *total_size += PIPE_MGR_TOF_NUM_TOTAL_PHV / 8;
  }

  return BF_SUCCESS;
}

/* Get the size of the snapshot capture data that needs to be allocated */
bf_status_t bf_snapshot_capture_phv_fields_dict_size(pipe_snapshot_hdl_t hdl,
                                                     uint32_t *total_size,
                                                     uint32_t *per_stage_size) {
  bf_status_t status = BF_SUCCESS;
  bf_dev_id_t dev = 0;
  int dir = 0;

  status =
      pipe_mgr_snapshot_hdl_to_dev_params(hdl, &dev, NULL, NULL, NULL, &dir);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Could not convert snapshot hdl 0x%x to dev params", hdl);
    return status;
  }

  *total_size = PIPE_MGR_SNAP_DEV_INFO(dev)->capture_total_phv_size[dir];
  *per_stage_size =
      PIPE_MGR_SNAP_DEV_INFO(dev)->capture_per_stage_phv_size[dir];

  return BF_SUCCESS;
}

/* Get the snapshot data */
bf_status_t bf_snapshot_capture_get(
    pipe_snapshot_hdl_t hdl,
    bf_dev_pipe_t pipe_input,
    uint8_t *capture,
    bf_snapshot_capture_ctrl_info_arr_t *capture_ctrl_arr,
    int *num_captures) {
  bf_status_t status = BF_SUCCESS;
  bf_dev_id_t dev = 0;
  bf_dev_pipe_t pipe = 0;
  dev_stage_t s_stage = 0, e_stage = 0;
  int dir = 0, s_idx = 0;
  int capture_count = 0;
  uint32_t total_size = 0, stage_size = 0;
  pipe_mgr_snapshot_long_branch_t long_branch;
  long_branch.data = NULL;

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return BF_INVALID_ARG;
  }
  status = pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, &pipe, &s_stage, &e_stage, &dir);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Could not convert snapshot hdl 0x%x to dev params", hdl);
    return status;
  }
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) {
    LOG_ERROR("Invalid device %d for snapshot 0x%x", dev, hdl);
    PIPE_MGR_DBGCHK(0);
    return BF_INVALID_ARG;
  }
  if (pipe == BF_DEV_PIPE_ALL) {
    pipe = pipe_input;
  }

  if (pipe >= dev_info->dev_cfg.num_pipelines) {
    LOG_ERROR("Invalid pipe %d for device %d", pipe, dev);
    return BF_INVALID_ARG;
  }

  bf_snapshot_capture_phv_fields_dict_size(hdl, &total_size, &stage_size);

  for (s_idx = s_stage; s_idx <= e_stage; s_idx++) {
    uint8_t *stage_capture = (uint8_t *)capture + (stage_size * capture_count);
    uint8_t *capture_v = NULL;
    if (dev_info->dev_family == BF_DEV_FAMILY_TOFINO) {
      capture_v =
          (uint8_t *)capture + total_size - PIPE_MGR_TOF_NUM_TOTAL_PHV / 8;
    }

    snapshot_stage_capture_get(dev_info,
                               pipe,
                               s_idx,
                               dir,
                               &long_branch,
                               stage_capture,
                               capture_v,
                               &capture_ctrl_arr->ctrl[capture_count]);
    capture_count++;
  }
  if (long_branch.data) PIPE_MGR_FREE(long_branch.data);
  *num_captures = capture_count;

  return status;
}

/* Get the field value in the snapshot data */
bf_status_t bf_snapshot_capture_decode_field_value(pipe_snapshot_hdl_t hdl,
                                                   bf_dev_pipe_t pipe,
                                                   dev_stage_t stage,
                                                   uint8_t *capture,
                                                   int num_captures,
                                                   char *field_name,
                                                   uint64_t *field_value,
                                                   bool *field_valid) {
  int capture_idx = 0;
  bf_dev_id_t dev = 0;
  dev_stage_t s_stage = 0, e_stage = 0;
  int dir = 0, s_idx = 0;
  profile_id_t prof_id = 0;
  uint32_t total_size = 0, stage_size = 0;
  bf_status_t status = BF_SUCCESS;

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return BF_INVALID_ARG;
  }
  status = pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, NULL, &s_stage, &e_stage, &dir);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Could not convert snapshot hdl 0x%x to dev params", hdl);
    return status;
  }
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) {
    LOG_ERROR("Invalid device %d for snapshot 0x%x", dev, hdl);
    PIPE_MGR_DBGCHK(0);
    return BF_INVALID_ARG;
  }
  if (pipe_mgr_pipe_to_profile(dev_info, pipe, &prof_id, __func__, __LINE__) !=
      PIPE_SUCCESS) {
    return PIPE_INVALID_ARG;
  }
  bf_snapshot_capture_phv_fields_dict_size(hdl, &total_size, &stage_size);

  capture_idx = 0;
  for (s_idx = s_stage; (s_idx <= e_stage) && (capture_idx < num_captures);
       s_idx++, capture_idx++) {
    if (stage != s_idx) {
      continue;
    }

    uint8_t *data = (uint8_t *)capture + (capture_idx * stage_size);
    uint8_t *data_v = NULL;
    if (dev_info->dev_family == BF_DEV_FAMILY_TOFINO) {
      data_v = (uint8_t *)capture + total_size - PIPE_MGR_TOF_NUM_TOTAL_PHV / 8;
    }
    // Stage passed to value get should be increased by 1 for proper decoding
    status = pipe_mgr_ctxjson_tof_snapshot_capture_field_value_get(dev,
                                                                   prof_id,
                                                                   s_idx + 1,
                                                                   dir,
                                                                   data,
                                                                   data_v,
                                                                   field_name,
                                                                   field_value,
                                                                   field_valid);

    return status;
  }

  return BF_INVALID_ARG;
}

/* Decode a named field from a snapshot capture into a byte buffer.
 * Works for fields of any width (including >64-bit fields like IPv6). */
bf_status_t bf_snapshot_capture_decode_field_value_bytes(
    pipe_snapshot_hdl_t hdl,
    bf_dev_pipe_t pipe,
    dev_stage_t stage,
    uint8_t *capture,
    int num_captures,
    char *field_name,
    uint8_t *out_value,
    int out_len,
    int *out_width,
    bool *field_valid) {
  int capture_idx = 0;
  bf_dev_id_t dev = 0;
  dev_stage_t s_stage = 0, e_stage = 0;
  int dir = 0, s_idx = 0;
  profile_id_t prof_id = 0;
  uint32_t total_size = 0, stage_size = 0;
  bf_status_t status = BF_SUCCESS;

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return BF_INVALID_ARG;
  }
  status = pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, NULL, &s_stage, &e_stage, &dir);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Could not convert snapshot hdl 0x%x to dev params", hdl);
    return status;
  }
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) {
    LOG_ERROR("Invalid device %d for snapshot 0x%x", dev, hdl);
    PIPE_MGR_DBGCHK(0);
    return BF_INVALID_ARG;
  }
  if (pipe_mgr_pipe_to_profile(dev_info, pipe, &prof_id, __func__, __LINE__) !=
      PIPE_SUCCESS) {
    return PIPE_INVALID_ARG;
  }
  bf_snapshot_capture_phv_fields_dict_size(hdl, &total_size, &stage_size);

  capture_idx = 0;
  for (s_idx = s_stage; (s_idx <= e_stage) && (capture_idx < num_captures);
       s_idx++, capture_idx++) {
    if (stage != s_idx) {
      continue;
    }

    uint8_t *data = (uint8_t *)capture + (capture_idx * stage_size);
    uint8_t *data_v = NULL;
    if (dev_info->dev_family == BF_DEV_FAMILY_TOFINO) {
      data_v =
          (uint8_t *)capture + total_size - PIPE_MGR_TOF_NUM_TOTAL_PHV / 8;
    }
    // Stage passed to value get should be increased by 1 for proper decoding
    status = pipe_mgr_ctxjson_tof_snapshot_capture_field_value_get_bytes(
        dev,
        prof_id,
        s_idx + 1,
        dir,
        data,
        data_v,
        field_name,
        out_value,
        out_len,
        out_width,
        field_valid);

    return status;
  }

  return BF_INVALID_ARG;
}

/* Snapshot interrupt clear helper */
pipe_status_t pipe_mgr_snapshot_interrupt_clear(bf_dev_id_t dev,
                                                bf_dev_pipe_t pipe,
                                                dev_stage_t stage,
                                                bf_snapshot_dir_t dir) {
  pipe_status_t status = PIPE_SUCCESS;

  if (pipe == BF_DEV_PIPE_ALL) {
    LOG_ERROR("Invalid pipe %d ", pipe);
    return PIPE_INVALID_ARG;
  }
  if (!PIPE_MGR_SNAP_DIR_VALID(dir)) {
    LOG_ERROR("Invalid dir %d ", dir);
    return PIPE_INVALID_ARG;
  }

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) {
    PIPE_MGR_DBGCHK(dev_info);
    return PIPE_INVALID_ARG;
  }

  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      status =
          pipe_mgr_snapshot_interrupt_clear_tof(dev_info, pipe, stage, dir);
      break;
    case BF_DEV_FAMILY_TOFINO2:
      status =
          pipe_mgr_snapshot_interrupt_clear_tof2(dev_info, pipe, stage, dir);
      break;
    case BF_DEV_FAMILY_TOFINO3:
      status =
          pipe_mgr_snapshot_interrupt_clear_tof3(dev_info, pipe, stage, dir);
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }

  return status;
}

/* Snapshot interrupt clear */
pipe_status_t bf_snapshot_interrupt_clear(pipe_snapshot_hdl_t hdl,
                                          bf_dev_pipe_t pipe_input,
                                          dev_stage_t stage) {
  bf_dev_id_t dev = 0;
  bf_dev_pipe_t pipe = 0;
  int dir = 0, s_idx = 0;
  dev_stage_t s_stage = 0, e_stage = 0;
  pipe_status_t status = PIPE_SUCCESS;

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return PIPE_INVALID_ARG;
  }
  pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, &pipe, &s_stage, &e_stage, &dir);

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return PIPE_INVALID_ARG;

  if (pipe == BF_DEV_PIPE_ALL) {
    if (pipe_input == BF_DEV_PIPE_ALL) {
      LOG_ERROR(
          "Snapshot hdl %d created for all pipes, please specify pipe"
          " to display \n",
          hdl);
      return PIPE_INVALID_ARG;
    } else {
      if (!snap_pipe_valid(dev_info, pipe_input)) {
        LOG_ERROR("Invalid pipe %d ", pipe_input);
        return PIPE_INVALID_ARG;
      }
      pipe = pipe_input;
    }
  }
  /* Validate stage give by user */
  if ((stage != PIPE_MGR_SNAP_STAGE_INVALID) &&
      ((stage < s_stage) || (stage > e_stage))) {
    LOG_ERROR("Invalid stage %d, not part of snapshot \n", stage);
    return PIPE_INVALID_ARG;
  }

  pipe_sess_hdl_t shdl = pipe_mgr_ctx->int_ses_hndl;
  status = pipe_mgr_api_enter(shdl);
  if (status != PIPE_SUCCESS) return status;

  for (s_idx = s_stage; s_idx <= e_stage; s_idx++) {
    if ((stage != PIPE_MGR_SNAP_STAGE_INVALID) && (stage != s_idx)) {
      continue;
    }

    status |= pipe_mgr_snapshot_interrupt_clear(dev, pipe, s_idx, dir);
  }

  pipe_mgr_api_exit(shdl);
  return status;
}

/* Get the trigger type that caused the snapshot */
pipe_status_t pipe_mgr_snapshot_captured_trigger_type_get(
    bf_dev_id_t dev,
    bf_dev_pipe_t pipe,
    dev_stage_t stage,
    int dir,
    bool *prev_stage_trig,
    bool *local_stage_trig,
    bool *timer_trig) {
  pipe_status_t status = PIPE_SUCCESS;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) {
    PIPE_MGR_DBGCHK(dev_info);
    return PIPE_INVALID_ARG;
  }

  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      status = pipe_mgr_snapshot_captured_trigger_type_get_tof(dev_info,
                                                               pipe,
                                                               stage,
                                                               dir,
                                                               prev_stage_trig,
                                                               local_stage_trig,
                                                               timer_trig);
      break;
    case BF_DEV_FAMILY_TOFINO2:
      status =
          pipe_mgr_snapshot_captured_trigger_type_get_tof2(dev_info,
                                                           pipe,
                                                           stage,
                                                           dir,
                                                           prev_stage_trig,
                                                           local_stage_trig,
                                                           timer_trig);
      break;
    case BF_DEV_FAMILY_TOFINO3:
      status =
          pipe_mgr_snapshot_captured_trigger_type_get_tof3(dev_info,
                                                           pipe,
                                                           stage,
                                                           dir,
                                                           prev_stage_trig,
                                                           local_stage_trig,
                                                           timer_trig);
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }

  return status;
}

/* Poll the snapshot states and give callback on triggerd ones */
bf_status_t bf_snapshot_do_polling(bf_dev_id_t dev) {
  pipe_status_t status = PIPE_SUCCESS;
  bf_map_sts_t map_sts = BF_MAP_OK;
  unsigned long key = 0;
  pipe_snapshot_hdl_t hdl = 0;
  bf_dev_pipe_t pipe = 0, start_pipe = 0, pipe_idx = 0;
  pipe_snap_hdl_info_t *hdl_info = NULL;
  int en_state = 0, pipe_count = 0;
  int dir = 0;
  dev_stage_t s_stage = 0;
  bool prev_stage_trig = false, local_stage_trig = false;
  bool timer_trig = false;

  if (!PIPE_MGR_SNAP_DEV_VALID(dev)) {
    return BF_INVALID_ARG;
  }
  if (!PIPE_MGR_SNAP_DEV_INFO(dev)) {
    return BF_SUCCESS;
  }
  /* Make sure we are not in interrupt mode */
  if (PIPE_MGR_SNAP_INTERRUPT_STATE(dev)) {
    return BF_SUCCESS;
  }
  if (!PIPE_MGR_SNAP_CALLBACK(dev)) {
    return BF_SUCCESS;
  }

  /* Get first handle */
  map_sts =
      bf_map_get_first(&(PIPE_MGR_SNAP_HDL_MAP(dev)), &key, (void **)&hdl_info);

  while (map_sts == BF_MAP_OK) {
    hdl = key;
    pipe_mgr_snapshot_hdl_to_dev_params(hdl, &dev, &pipe, &s_stage, NULL, &dir);

    /* If all pipes, go over all */
    if (pipe == BF_DEV_PIPE_ALL) {
      start_pipe = 0;
      pipe_count = pipe_mgr_snapshot_num_active_pipes(dev);
    } else {
      start_pipe = pipe;
      pipe_count = 1;
    }

    for (pipe_idx = start_pipe; pipe_idx < (start_pipe + pipe_count);
         pipe_idx++) {
      /* Make sure snapshot has been enabled */
      if (!PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, s_stage, dir).admin_state ||
          !PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, s_stage, dir).hdl_created) {
        continue;
      }
      status = bf_snapshot_pd_state_get(hdl, pipe_idx, &en_state);
      if (status != PIPE_SUCCESS) {
        continue;
      }
      /* After snapshot trig hit, state goes to disabled */
      if (en_state != BF_SNAPSHOT_ST_DISABLED) {
        continue;
      }
      /* Read snapshot datapath register to determine the trigger */
      status = pipe_mgr_snapshot_captured_trigger_type_get(dev,
                                                           pipe_idx,
                                                           hdl_info->oper_stage,
                                                           dir,
                                                           &prev_stage_trig,
                                                           &local_stage_trig,
                                                           &timer_trig);
      if (status != PIPE_SUCCESS) {
        continue;
      }

      /* local trigger only, no callbacks for prev stage triggers */
      if ((!local_stage_trig) && (!timer_trig)) {
        continue;
      }
      if (PIPE_MGR_SNAP_CALLBACK(dev)) {
        PIPE_MGR_SNAP_CALLBACK(dev)(dev, pipe_idx, hdl);
      }
    }

    /* Get next handle */
    map_sts = bf_map_get_next(
        &(PIPE_MGR_SNAP_HDL_MAP(dev)), &key, (void **)&hdl_info);
  }

  return BF_SUCCESS;
}

/* Handle interrupt */
pipe_status_t pipe_mgr_snapshot_handle_interrupt(bf_dev_id_t dev,
                                                 bf_dev_pipe_t phy_pipe,
                                                 dev_stage_t stage,
                                                 bf_snapshot_dir_t dir) {
  pipe_status_t status = PIPE_SUCCESS;
  pipe_snapshot_hdl_t hdl = 0;
  bf_dev_pipe_t pipe = 0;
  bool prev_stage_trig = false, local_stage_trig = false;
  bool timer_trig = false;
  int en_state = 0;

  if (!PIPE_MGR_SNAP_DEV_VALID(dev)) {
    return PIPE_INVALID_ARG;
  }
  if (!PIPE_MGR_SNAP_DEV_INFO(dev)) {
    return PIPE_INVALID_ARG;
  }
  if (!PIPE_MGR_SNAP_DIR_VALID(dir)) {
    return PIPE_INVALID_ARG;
  }

  /* Get logical pipe from physical pipe */
  pipe_mgr_map_phy_pipe_id_to_log_pipe_id(dev, phy_pipe, &pipe);

  pipe_sess_hdl_t shdl = pipe_mgr_ctx->int_ses_hndl;
  status = pipe_mgr_api_enter(shdl);
  if (status != PIPE_SUCCESS) return status;

  /* Clear interrupt */
  pipe_mgr_snapshot_interrupt_clear(dev, pipe, stage, dir);

  /* Make sure we are in interrupt mode */
  if (!PIPE_MGR_SNAP_INTERRUPT_STATE(dev)) {
    goto done;
  }
  if (!PIPE_MGR_SNAP_CALLBACK(dev)) {
    goto done;
  }

  status = bf_snapshot_pd_state_get(hdl, pipe, &en_state);
  if (status != PIPE_SUCCESS) {
    LOG_ERROR("Snapshot state get error ");
    goto done;
  }
  /* After snapshot trig hit, state goes to disabled */
  if (en_state != BF_SNAPSHOT_ST_DISABLED) {
    LOG_ERROR("Snapshot is not in disabled state, dev %d hdl 0x%x", dev, hdl);
    goto done;
  }

  /* Read snapshot datapath register to determine the trigger */
  pipe_mgr_snapshot_captured_trigger_type_get(
      dev, pipe, stage, dir, &prev_stage_trig, &local_stage_trig, &timer_trig);

  /* Get the right snapshot hdl */
  if (local_stage_trig || timer_trig) {  // local trigger
    if (PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, dir).admin_state &&
        PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, dir).hdl_created) {
      hdl = PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, dir).hdl;
    }
  } else if (prev_stage_trig) {  // prev stage trigger
    /* No callbacks for prev stage trigger */
    goto done;
  }

  /* Give callback */
  if (PIPE_MGR_SNAP_CALLBACK(dev)) {
    PIPE_MGR_SNAP_CALLBACK(dev)(dev, pipe, hdl);
  }

done:
  pipe_mgr_api_exit(shdl);
  return status;
}

/* Dump PHV allocation */
pipe_status_t pipe_mgr_phv_allocation_dump(bf_dev_id_t dev,
                                           bf_dev_pipe_t log_pipe,
                                           dev_stage_t stage,
                                           bf_snapshot_dir_t dir,
                                           char *str,
                                           int max_len) {
  int c_len = 0;
  uint32_t idx = 0;
  pipe_snap_stage_info_t *stage_info = NULL;
  bool snapshot_matchable = false, stage_depen;

  if (!PIPE_MGR_SNAP_DEV_VALID(dev)) {
    return PIPE_INVALID_ARG;
  }
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return PIPE_INVALID_ARG;

  if (!PIPE_MGR_SNAP_DEV_INFO(dev)) {
    /*c_len += */ snprintf(str + c_len,
                           (c_len < max_len) ? (max_len - c_len - 1) : 0,
                           "Device %d not found \n",
                           dev);
    return PIPE_INVALID_ARG;
  }
  if (!snap_pipe_valid(dev_info, log_pipe)) {
    LOG_ERROR("Invalid pipe %d ", log_pipe);
    return PIPE_INVALID_ARG;
  }
  if (!snap_stage_valid(dev_info, stage)) {
    LOG_ERROR("Invalid stage %d on dev %d", stage, dev);
    return PIPE_INVALID_ARG;
  }
  if (!PIPE_MGR_SNAP_DIR_VALID(dir)) {
    LOG_ERROR("Invalid direction %d ", dir);
    return PIPE_INVALID_ARG;
  }
  profile_id_t prof_id = 0;
  if (pipe_mgr_pipe_to_profile(
          dev_info, log_pipe, &prof_id, __func__, __LINE__) != PIPE_SUCCESS) {
    return PIPE_INVALID_ARG;
  }

  stage_info = &(PIPE_MGR_SNAP_STAGE_INFO(dev, log_pipe, stage, dir));
  if (!(stage_info->all_fields_dict_valid)) {
    pipe_mgr_create_all_fields_dict(dev, log_pipe, stage, dir);
  }

  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "Pipe Stage SnapS Contr[Len] Field[S:E] PHV[S:E] Name\n");
  stage_depen = pipe_mgr_stage_match_dependent_get(
      dev, prof_id, stage, dir);  // stage dependent
  for (idx = 0; idx < stage_info->num_fields_in_dict; idx++) {
    if (!stage_info->all_fields_dict[idx].valid) {
      continue;
    }
    switch (stage_info->all_fields_dict[idx].container_type) {
      case 0:
        snapshot_matchable = true;
        break;
      case 1:
        snapshot_matchable = stage_depen;
        break;
      case 2:
        snapshot_matchable = false;
        break;
      default:
        LOG_ERROR("Invalid container_type %d, idx %d ",
                  stage_info->all_fields_dict[idx].container_type,
                  idx);
        return PIPE_INVALID_ARG;
    }
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "%4d %5d   %s   %6d[%2d] %7d:%2d %5d:%2d %s\n",
                      log_pipe,
                      stage,
                      snapshot_matchable ? "Y" : "N",
                      stage_info->all_fields_dict[idx].container_num,
                      stage_info->all_fields_dict[idx].container_width,
                      stage_info->all_fields_dict[idx].field_lsb,
                      stage_info->all_fields_dict[idx].field_msb,
                      stage_info->all_fields_dict[idx].phv_lsb,
                      stage_info->all_fields_dict[idx].phv_msb,
                      stage_info->all_fields_dict[idx].name);
  }

  return PIPE_SUCCESS;
}

/* Show Snapshot state */
pipe_status_t pipe_mgr_snapshot_state_show(pipe_snapshot_hdl_t hdl,
                                           bf_dev_pipe_t pipe_input,
                                           dev_stage_t stage,
                                           char *str,
                                           int max_len) {
  pipe_status_t status = PIPE_SUCCESS;
  int c_len = 0;
  pipe_snapshot_fsm_state_t fsm_state = 0;
  bf_dev_id_t dev = 0;
  bf_dev_pipe_t pipe = 0, phy_pipe = 0;
  dev_stage_t s_stage = 0, e_stage = 0;
  int dir = 0, en_state = 0, s_idx = 0;
  char buf[PIPE_SNAP_TRIG_FIELD_NAME_LEN];

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return PIPE_INVALID_ARG;
  }
  status = pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, &pipe, &s_stage, &e_stage, &dir);

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) {
    PIPE_MGR_DBGCHK(dev_info);
    return PIPE_INVALID_ARG;
  }

  if (pipe == BF_DEV_PIPE_ALL) {
    if (pipe_input == BF_DEV_PIPE_ALL) {
      /* c_len += */
      snprintf(str + c_len,
               (c_len < max_len) ? (max_len - c_len - 1) : 0,
               "Snapshot hdl %d created for all pipes, please specify pipe"
               " to display \n",
               hdl);
      return PIPE_SUCCESS;
    } else {
      if (!snap_pipe_valid(dev_info, pipe_input)) {
        LOG_ERROR("Invalid pipe %d ", pipe_input);
        return PIPE_INVALID_ARG;
      }
      pipe = pipe_input;
    }
  }
  /* Validate stage give by user */
  if ((stage != PIPE_MGR_SNAP_STAGE_INVALID) &&
      ((stage < s_stage) || (stage > e_stage))) {
    /* c_len += */ snprintf(str + c_len,
                            (c_len < max_len) ? (max_len - c_len - 1) : 0,
                            "Invalid stage %d, not part of snapshot \n",
                            stage);
    return PIPE_INVALID_ARG;
  }
  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "Snapshot state dump for handle 0x%x \n",
                    hdl);

  c_len +=
      snprintf(str + c_len,
               (c_len < max_len) ? (max_len - c_len - 1) : 0,
               "Dumping snapshot state for dev %d, pipe %d, start-stage %d, "
               "end-stage %d, dir %s \n",
               dev,
               pipe,
               s_stage,
               e_stage,
               PIPE_MGR_SNAP_DIR_STRING(dir));

  /* Get physical pipe from logical */
  pipe_mgr_map_pipe_id_log_to_phy(dev_info, pipe, &phy_pipe);

  /* Dump for all stages in this snapshot */
  for (s_idx = s_stage; s_idx <= e_stage; s_idx++) {
    if ((stage != PIPE_MGR_SNAP_STAGE_INVALID) && (stage != s_idx)) {
      continue;
    }

    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      " --- Dumping snapshot state for stage %d --- \n",
                      s_idx);

    bool is_set = false;
    switch (dev_info->dev_family) {
      case BF_DEV_FAMILY_TOFINO:
        pipe_mgr_snapshot_interrupt_get_tof(
            dev_info, pipe, s_idx, dir, &is_set);
        break;
      case BF_DEV_FAMILY_TOFINO2:
        pipe_mgr_snapshot_interrupt_get_tof2(
            dev_info, pipe, s_idx, dir, &is_set);
        break;
      case BF_DEV_FAMILY_TOFINO3:
        pipe_mgr_snapshot_interrupt_get_tof3(
            dev_info, pipe, s_idx, dir, &is_set);
        break;
      default:
        PIPE_MGR_DBGCHK(0);
        return PIPE_UNEXPECTED;
    }
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "Interrupt state : %s \n",
                      is_set ? "Set" : "Clear");

    /* Read fsm state */
    pipe_mgr_snapshot_fsm_state_get(
        dev, pipe, s_idx, dir, &fsm_state, &en_state);
    c_len += snprintf(
        str + c_len,
        (c_len < max_len) ? (max_len - c_len - 1) : 0,
        "Admin State     : %s \n",
        pipe_mgr_snapshot_state_to_str((bf_snapshot_state_t)en_state, buf));
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "FSM state       : %s \n",
                      pipe_mgr_snapshot_fsm_state_to_str(fsm_state, buf));

    /* Read mau snapshot config */
    bool ing_timer_en = false, egr_timer_en = false;
    bf_snapshot_ig_mode_t mode = BF_SNAPSHOT_IGM_INGRESS;
    switch (dev_info->dev_family) {
      case BF_DEV_FAMILY_TOFINO:
        pipe_mgr_snapshot_timer_get_enable_tof(
            dev_info, pipe, s_idx, &ing_timer_en, &egr_timer_en);
        break;
      case BF_DEV_FAMILY_TOFINO2:
        pipe_mgr_snapshot_cfg_get_tof2(
            dev_info, pipe, s_idx, &ing_timer_en, &egr_timer_en, &mode);
        break;
      case BF_DEV_FAMILY_TOFINO3:
        pipe_mgr_snapshot_timer_get_enable_tof3(
            dev_info, pipe, s_idx, &ing_timer_en, &egr_timer_en);
        break;
      default:
        PIPE_MGR_DBGCHK(0);
        return PIPE_UNEXPECTED;
    }
    bool timer_en =
        dir == BF_SNAPSHOT_DIR_INGRESS ? ing_timer_en : egr_timer_en;
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "Timer state     : %s \n",
                      timer_en ? "Enabled" : "Disabled");
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "Ingress trigger mode   : 0x%x \n",
                      mode);

    /* Read time counter */
    uint64_t clocks_now, clocks_trig;
    switch (dev_info->dev_family) {
      case BF_DEV_FAMILY_TOFINO:
        pipe_mgr_snapshot_timer_get_tof(
            dev_info, pipe, s_idx, &clocks_now, &clocks_trig);
        break;
      case BF_DEV_FAMILY_TOFINO2:
        pipe_mgr_snapshot_timer_get_tof2(
            dev_info, pipe, s_idx, &clocks_now, &clocks_trig);
        break;
      case BF_DEV_FAMILY_TOFINO3:
        pipe_mgr_snapshot_timer_get_tof3(
            dev_info, pipe, s_idx, &clocks_now, &clocks_trig);
        break;
      default:
        PIPE_MGR_DBGCHK(0);
        return PIPE_UNEXPECTED;
    }
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "Current time    : 0x%" PRIx64 " clocks\n",
                      clocks_now);
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "Trigger time    : 0x%" PRIx64 " clocks \n",
                      clocks_trig);
  }

  return status;
}

/* Show Snapshot config */
pipe_status_t pipe_mgr_snapshot_config_dump(pipe_snapshot_hdl_t hdl,
                                            bf_dev_pipe_t pipe_input,
                                            dev_stage_t stage,
                                            char *str,
                                            int max_len) {
  pipe_status_t status = PIPE_SUCCESS;
  int c_len = 0;
  bf_dev_id_t dev = 0;
  bf_dev_pipe_t pipe = 0;
  dev_stage_t s_stage = 0, e_stage = 0;
  int dir = 0, s_idx = 0, i = 0;
  uint32_t idx = 0;
  pipe_snap_hdl_info_t *hdl_info = NULL;

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return PIPE_INVALID_ARG;
  }

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return PIPE_INVALID_ARG;

  status = pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, &pipe, &s_stage, &e_stage, &dir);

  if (pipe == BF_DEV_PIPE_ALL) {
    if (pipe_input == BF_DEV_PIPE_ALL) {
      /* c_len += */
      snprintf(str + c_len,
               (c_len < max_len) ? (max_len - c_len - 1) : 0,
               "Snapshot hdl %d created for all pipes, please specify pipe"
               " to display \n",
               hdl);
      return PIPE_SUCCESS;
    } else {
      if (!snap_pipe_valid(dev_info, pipe_input)) {
        LOG_ERROR("Invalid pipe %d ", pipe_input);
        return PIPE_INVALID_ARG;
      }
      pipe = pipe_input;
    }
  }
  /* Validate stage give by user */
  if ((stage != PIPE_MGR_SNAP_STAGE_INVALID) &&
      ((stage < s_stage) || (stage > e_stage))) {
    /* c_len += */ snprintf(str + c_len,
                            (c_len < max_len) ? (max_len - c_len - 1) : 0,
                            "Invalid stage %d, not part of snapshot \n",
                            stage);
    return PIPE_INVALID_ARG;
  }
  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "Snapshot config dump for handle 0x%x \n",
                    hdl);

  c_len +=
      snprintf(str + c_len,
               (c_len < max_len) ? (max_len - c_len - 1) : 0,
               "Dumping snapshot config for dev %d, pipe %d, start-stage %d, "
               "end-stage %d, dir %s \n",
               dev,
               pipe,
               s_stage,
               e_stage,
               PIPE_MGR_SNAP_DIR_STRING(dir));

  /* Dump handle info */
  hdl_info = pipe_mgr_snap_hdl_info_get(dev, hdl);
  if (hdl_info) {
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      " -- Oper-start-stage %d \n",
                      hdl_info->oper_stage);
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      " -- Trigger info for handle 0x%x -- \n",
                      hdl);
    for (idx = 0; idx < hdl_info->num_trig_fields; idx++) {
      if (!PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).valid) {
        continue;
      }

      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        "     Trigger: %s Value: ",
                        PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).name);

      for (i = 0; i < (int)PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).width; i++) {
        c_len += snprintf(str + c_len,
                          (c_len < max_len) ? (max_len - c_len - 1) : 0,
                          "%02x",
                          PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).value[i]);
      }
      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        " Mask: ");
      for (i = 0; i < (int)PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).width; i++) {
        c_len += snprintf(str + c_len,
                          (c_len < max_len) ? (max_len - c_len - 1) : 0,
                          "%02x",
                          PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).mask[i]);
      }
      c_len += snprintf(
          str + c_len, (c_len < max_len) ? (max_len - c_len - 1) : 0, "\n");
    }
  } else {
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      " Handle info not found for hdl 0x%x \n",
                      hdl);
  }

  c_len += snprintf(
      str + c_len, (c_len < max_len) ? (max_len - c_len - 1) : 0, "\n");

  /* Dump for all stages in this snapshot */
  for (s_idx = s_stage; s_idx <= e_stage; s_idx++) {
    if ((stage != PIPE_MGR_SNAP_STAGE_INVALID) && (stage != s_idx)) {
      continue;
    }

    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      " --- Dumping snapshot config for stage %d --- \n",
                      s_idx);

    c_len +=
        snprintf(str + c_len,
                 (c_len < max_len) ? (max_len - c_len - 1) : 0,
                 "Admin State    : %s \n",
                 PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_idx, dir).admin_state
                     ? "Enabled"
                     : "Disabled");

    c_len +=
        snprintf(str + c_len,
                 (c_len < max_len) ? (max_len - c_len - 1) : 0,
                 "Timer state    : %s \n",
                 PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_idx, dir).timer_enabled
                     ? "Enabled"
                     : "Disabled");

    c_len +=
        snprintf(str + c_len,
                 (c_len < max_len) ? (max_len - c_len - 1) : 0,
                 "End stage      : %d \n",
                 PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_idx, dir).end_stage);

    c_len += snprintf(
        str + c_len,
        (c_len < max_len) ? (max_len - c_len - 1) : 0,
        "Handle created : %s \n",
        PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_idx, dir).hdl_created ? "Yes"
                                                                    : "No");

    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "Handle value   : 0x%x \n",
                      PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_idx, dir).hdl);

    c_len +=
        snprintf(str + c_len,
                 (c_len < max_len) ? (max_len - c_len - 1) : 0,
                 "Ingress trigger mode   : 0x%x \n",
                 PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, s_idx, dir).trig_mode);
  }

  return status;
}

/* Show Snapshot capture */
pipe_status_t pipe_mgr_snapshot_capture_show(pipe_snapshot_hdl_t hdl,
                                             bf_dev_pipe_t pipe_input,
                                             dev_stage_t stage,
                                             char *str,
                                             int max_len) {
  pipe_status_t status = PIPE_SUCCESS;
  int c_len = 0;
  void *capture = NULL;
  bf_dev_id_t dev = 0;
  bf_dev_pipe_t pipe = 0;
  dev_stage_t s_stage = 0, e_stage = 0;
  int dir = 0, capture_idx = 0, num_captures = 0;
  profile_id_t prof_id = 0;
  bool valid_stage = false;

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return PIPE_INVALID_ARG;
  }

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return PIPE_INVALID_ARG;

  status = pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, &pipe, &s_stage, &e_stage, &dir);

  if (pipe == BF_DEV_PIPE_ALL) {
    if (pipe_input == BF_DEV_PIPE_ALL) {
      c_len +=
          snprintf(str + c_len,
                   (c_len < max_len) ? (max_len - c_len - 1) : 0,
                   "Snapshot hdl %d created for all pipes, please specify pipe"
                   " to display \n",
                   hdl);
      return PIPE_SUCCESS;
    } else {
      if (!snap_pipe_valid(dev_info, pipe_input)) {
        LOG_ERROR("Invalid pipe %d ", pipe_input);
        return PIPE_INVALID_ARG;
      }
      pipe = pipe_input;
    }
  }
  /* Validate stage give by user */
  if ((stage != PIPE_MGR_SNAP_STAGE_INVALID) &&
      ((stage < s_stage) || (stage > e_stage))) {
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "Invalid stage %d, not part of snapshot \n",
                      stage);
    return PIPE_INVALID_ARG;
  }
  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "Snapshot capture for handle 0x%x \n",
                    hdl);

  c_len +=
      snprintf(str + c_len,
               (c_len < max_len) ? (max_len - c_len - 1) : 0,
               "Dumping snapshot capture for dev %d, pipe %d, start-stage %d, "
               "end-stage %d, dir %s \n",
               dev,
               pipe,
               s_stage,
               e_stage,
               PIPE_MGR_SNAP_DIR_STRING(dir));
  if (pipe_mgr_pipe_to_profile(dev_info, pipe, &prof_id, __func__, __LINE__) !=
      PIPE_SUCCESS) {
    return PIPE_INVALID_ARG;
  }

  /* Allocate memory for p4 fields capture for all stages */
  /* Return PHV dict size of a stage that has most number of
   * alive PHVs or maximum PHV dict size among all stages
   */
  uint32_t data_size = 0, stage_size = 0;
  bf_snapshot_capture_phv_fields_dict_size(hdl, &data_size, &stage_size);
  capture = PIPE_MGR_CALLOC(1, data_size);
  if (!capture) {
    LOG_ERROR("Unable to allocate memory ");
    return PIPE_NO_SYS_RESOURCES;
  }

  bf_snapshot_capture_ctrl_info_arr_t ctrl_info_arr;
  memset(&ctrl_info_arr, 0, sizeof(ctrl_info_arr));
  bf_snapshot_capture_get(
      hdl, pipe_input, capture, &ctrl_info_arr, &num_captures);

  /* Dump for all stages in this snapshot */
  for (capture_idx = 0; capture_idx < num_captures; capture_idx++) {
    /* Print capture of P4 fields */
    bf_snapshot_capture_ctrl_info_t *ctrl = &ctrl_info_arr.ctrl[capture_idx];
    uint8_t *data = (uint8_t *)capture + (capture_idx * stage_size);
    uint8_t *data_v = NULL;
    if (dev_info->dev_family == BF_DEV_FAMILY_TOFINO) {
      /* For tofino data_v must be pointing to end part of capture buffer,
       * where validity bits are stored. */
      data_v = (uint8_t *)capture + data_size - PIPE_MGR_TOF_NUM_TOTAL_PHV / 8;
    }
    if (valid_stage == false) {
      /* First valid stage should be either locally triggered or timer
       * triggered. */
      if (ctrl->local_stage_trigger || ctrl->timer_trigger)
        valid_stage = true;
      else
        continue;
    }
    status = pipe_mgr_ctxjson_tof_snapshot_capture_print(
        dev, prof_id, dir, ctrl, data, data_v, str, &c_len, max_len, stage);
  }

  PIPE_MGR_FREE(capture);
  capture = NULL;

  return status;
}

/* Print handle info */
pipe_status_t pipe_mgr_snapshot_hdl_info_print(pipe_snapshot_hdl_t hdl,
                                               pipe_snap_hdl_info_t *hdl_info,
                                               char *str,
                                               int *c_str_len,
                                               int max_len) {
  int c_len = *c_str_len;
  bf_dev_id_t dev = 0;
  bf_dev_pipe_t pipe = 0;
  dev_stage_t s_stage = 0, e_stage = 0;
  int dir = 0;

  if (!hdl_info) {
    return PIPE_INVALID_ARG;
  }

  pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, &pipe, &s_stage, &e_stage, &dir);

  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "Hdl 0x%x --> dev %d, pipe %d, start_stage %d, "
                    "oper-start-stage %d, end_stage %d, dir %s \n",
                    hdl,
                    dev,
                    pipe,
                    s_stage,
                    hdl_info->oper_stage,
                    e_stage,
                    PIPE_MGR_SNAP_DIR_STRING(dir));

  *c_str_len = c_len;

  return PIPE_SUCCESS;
}

/* Dump Snapshot handles */
pipe_status_t pipe_mgr_snapshot_hdls_dump(bf_dev_id_t dev,
                                          pipe_snapshot_hdl_t hdl_input,
                                          char *str,
                                          int max_len) {
  pipe_status_t status = PIPE_SUCCESS;
  bf_map_sts_t map_sts = BF_MAP_OK;
  pipe_snapshot_hdl_t hdl = 0;
  int c_len = 0;
  unsigned long key = 0;
  pipe_snap_hdl_info_t *hdl_info = NULL;

  if (!PIPE_MGR_SNAP_DEV_VALID(dev)) {
    return PIPE_INVALID_ARG;
  }
  if (!PIPE_MGR_SNAP_DEV_INFO(dev)) {
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      " Dev %d does not exist \n",
                      dev);
    return PIPE_INVALID_ARG;
  }
  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    " --- Dumping snapshot hdls for dev %d ---  \n",
                    dev);
  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "Num of Handles on dev %d is %d \n",
                    dev,
                    bf_map_count(&PIPE_MGR_SNAP_HDL_MAP(dev)));

  if (hdl_input != 0) {
    hdl_info = pipe_mgr_snap_hdl_info_get(dev, hdl_input);
    if (!hdl_info) {
      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        "Handle 0x%x does not exist on dev %d \n",
                        hdl_input,
                        dev);
    } else {
      /* Print handle info */
      pipe_mgr_snapshot_hdl_info_print(
          hdl_input, hdl_info, str, &c_len, max_len);
    }
  } else {
    /* Get first handle */
    map_sts = bf_map_get_first(
        &(PIPE_MGR_SNAP_HDL_MAP(dev)), &key, (void **)&hdl_info);

    while (map_sts == BF_MAP_OK) {
      hdl = key;

      /* Print handle info */
      pipe_mgr_snapshot_hdl_info_print(hdl, hdl_info, str, &c_len, max_len);

      /* Get next handle */
      map_sts = bf_map_get_next(
          &(PIPE_MGR_SNAP_HDL_MAP(dev)), &key, (void **)&hdl_info);
    }
  }

  return status;
}

/* Validate one p4 field */
static pipe_status_t pipe_mgr_snapshot_field_validate(bf_dev_id_t dev,
                                                      bf_dev_pipe_t pipe,
                                                      dev_stage_t stage,
                                                      int dir,
                                                      char *name,
                                                      bool trig_field,
                                                      bool *field_found,
                                                      int *width) {
  pipe_snap_stage_info_t *stage_info = NULL;
  pipe_snap_stage_field_info_t *field_details = NULL;
  int bit_width = 0;
  bf_dev_pipe_t start_pipe = 0, pipe_count = 0;
  bf_dev_pipe_t pipe_idx = 0;
  uint32_t cntr = 0, field_found_cnt = 0;
  bool found_in_pipe = false;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return PIPE_INVALID_ARG;

  *field_found = false;
  *width = 0;
  if (pipe == BF_DEV_PIPE_ALL) {
    start_pipe = 0;
    pipe_count = pipe_mgr_snapshot_num_active_pipes(dev);
  } else {
    start_pipe = pipe;
    pipe_count = 1;
  }

  /* Reprogram for all pipes where hdl is */
  for (pipe_idx = start_pipe; pipe_idx < (start_pipe + pipe_count);
       pipe_idx++) {
    found_in_pipe = false;
    stage_info = &(PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, stage, dir));

    if (!(PIPE_MGR_SNAP_STAGE_INFO(dev, pipe_idx, stage, dir)
              .all_fields_dict_valid)) {
      pipe_mgr_create_all_fields_dict(dev, pipe_idx, stage, dir);
    }

    profile_id_t prof_id = 0;
    if (pipe_mgr_pipe_to_profile(
            dev_info, pipe_idx, &prof_id, __func__, __LINE__) != PIPE_SUCCESS) {
      continue;
    }

    for (cntr = 0; cntr < stage_info->num_fields_in_dict; cntr++) {
      field_details = &(stage_info->all_fields_dict[cntr]);
      if (!field_details->valid) {
        continue;
      }
      /* Look over all instances of this field to get max width */
      if (strcmp(field_details->name, name) == 0) {
        /* Check validation of contain type */
        switch (field_details->container_type) {
          case 0:  // normal phv
            break;
          case 1:  // mocha phv
            // If stage is match dependent then mocha can be used as normal phv
            // for triggering, otherwise skip this container.
            if (!trig_field) break;
            if (pipe_mgr_stage_match_dependent_get(dev, prof_id, stage, dir))
              break;
            else
              continue;  // not matchable; keep scanning for normal containers
          case 2:  // dark phv
            if (!trig_field) break;
            // Dark PHV cannot be used as a trigger -- skip this container
            // but keep scanning for matchable (normal) containers.
            // The field dictionary is global (same for every stage), so a
            // field that lives in dark PHV at some stages may still have
            // normal containers that are perfectly usable for triggering.
            continue;
          default:  // unknown phv
            PIPE_MGR_DBGCHK(0);
        }
        found_in_pipe = true;
        if ((field_details->field_msb + 1) > (bit_width)) {
          bit_width = field_details->field_msb + 1;
        }
      }
    }
    /* Inc count if field exists on this pipe */
    if (found_in_pipe == true) {
      field_found_cnt++;
    }
  }
  /* Check if field was found on all pipes */
  if (field_found_cnt == pipe_count) {
    *field_found = true;
    *width = (bit_width + 7) / 8;
  }

  return PIPE_SUCCESS;
}

/* Validate one trigger p4 field */
static pipe_status_t pipe_mgr_snapshot_trig_field_validate(bf_dev_id_t dev,
                                                           bf_dev_pipe_t pipe,
                                                           dev_stage_t stage,
                                                           int dir,
                                                           char *name,
                                                           bool *field_found,
                                                           int *width) {
  return pipe_mgr_snapshot_field_validate(
      dev, pipe, stage, dir, name, true, field_found, width);
}

/* Validate all fields in the handle */
pipe_status_t pipe_mgr_snapshot_capture_trigger_fields_validate(
    pipe_snapshot_hdl_t hdl, dev_stage_t stage) {
  pipe_status_t ret_status = PIPE_SUCCESS;
  pipe_snap_hdl_info_t *hdl_info = NULL;
  bf_dev_id_t dev = 0;
  bf_dev_pipe_t pipe = 0;
  dev_stage_t s_stage = 0, e_stage = 0;
  int dir = 0, width = 0;
  uint32_t idx = 0;
  bool field_found = false;

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return PIPE_INVALID_ARG;
  }
  pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, &pipe, &s_stage, &e_stage, &dir);

  hdl_info = pipe_mgr_snap_hdl_info_get(dev, hdl);
  if (!hdl_info) {
    LOG_ERROR("Handle info is null for handle 0x%x", hdl);
    return PIPE_INVALID_ARG;
  }

  /* Walk all p4 fields */
  for (idx = 0; idx < hdl_info->num_trig_fields; idx++) {
    if (PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).valid) {
      field_found = false;
      ret_status |= pipe_mgr_snapshot_trig_field_validate(
          dev,
          pipe,
          stage,
          dir,
          PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).name,
          &field_found,
          &width);
      if (ret_status != PIPE_SUCCESS) ret_status |= PIPE_INVALID_ARG;
      /* If field name not found, set error status */
      if (!field_found) {
        LOG_TRACE(
            "Field name %s does not exist in trigger spec in"
            " pipe %d, stage %d, dev %d dir %s",
            PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).name,
            pipe,
            stage,
            dev,
            PIPE_MGR_SNAP_DIR_STRING(dir));
        ret_status |= PIPE_INVALID_ARG;
      }
    }
  }

  return ret_status;
}

/* Add a field to the list of triggers helper */
static pipe_status_t snapshot_capture_trigger_field_add(pipe_snapshot_hdl_t hdl,
                                                        char *field_name,
                                                        uint64_t value,
                                                        uint64_t mask) {
  pipe_status_t status = PIPE_SUCCESS;
  pipe_snap_hdl_info_t *hdl_info = NULL;
  bf_dev_id_t dev = 0;
  bf_dev_pipe_t pipe = 0;
  int dir = 0, idx = 0, i = 0, shift_cntr = 0;
  bool field_inserted = false, field_found = false, oper_stage_set = false;
  dev_stage_t s_stage = 0, e_stage = 0, stage = 0;
  int width = 0, n_width = 0, n_start = 0;

  PIPE_SNAP_DBG("Dbg Trigger: Adding Field %s, value 0x%" PRIx64
                ", mask 0x%" PRIx64 " \n",
                field_name,
                value,
                mask);

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return PIPE_INVALID_ARG;
  }

  pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, &pipe, &s_stage, &e_stage, &dir);

  hdl_info = pipe_mgr_snap_hdl_info_get(dev, hdl);
  if (!hdl_info) {
    LOG_ERROR("Handle info is null for handle 0x%x", hdl);
    return PIPE_INVALID_ARG;
  }

  /* Validate the field name */
  for (stage = s_stage; stage <= e_stage; stage++) {
    status = pipe_mgr_snapshot_trig_field_validate(
        dev, pipe, stage, dir, field_name, &field_found, &width);
    if (status != PIPE_SUCCESS) { /* dark and some mocha PHV do not matchable */
      PIPE_SNAP_DBG(
          "Field name %s can not be added due to container_type in stage %d \n",
          field_name,
          stage);
      LOG_ERROR(
          "Field name %s can not be added due to container_type in dev_id %d, "
          "stage %d, direction %d\n",
          field_name,
          dev,
          stage,
          dir);
      continue;
    }
    if (!field_found) {
      PIPE_SNAP_DBG(
          "Field name %s does not exist in stage %d \n", field_name, stage);
    } else {
      /* Field valid in this stage */
      /* Current oper-stage has the new field name, we are good */
      if (stage == hdl_info->oper_stage) {
        oper_stage_set = true;
        break;
      } else {
        /* need to check if oper-stage can be changed for all
           existing match params to this stage
        */
        status = pipe_mgr_snapshot_capture_trigger_fields_validate(hdl, stage);
        if (status == PIPE_SUCCESS) {
          if (!oper_stage_set) {
            hdl_info->oper_stage = stage;
            oper_stage_set = true;
            PIPE_SNAP_DBG("Snapshot 0x%x oper-stage changed to %d \n",
                          hdl,
                          hdl_info->oper_stage);
            break;
          }
        }
      }
    }  // else if field_found
  }

  if (!oper_stage_set) {
    LOG_ERROR(
        "Field name %s does not exist or not all trigger fields exist in a "
        "stage \n",
        field_name);
    return PIPE_INVALID_ARG;
  }

  /* For fields greater than 8 bytes, use only lower 8 bytes DRV-998 */
  if (width > (int)sizeof(value)) {
    n_width = sizeof(value);
    n_start =
        sizeof(PIPE_MGR_SNAP_HDL_FIELD(hdl_info, 0).value) - sizeof(value);
    LOG_WARN(
        "Snapshot-trig-add: Width of field %s is %d bytes which is greater "
        "than the max supported, using only lower %d bytes",
        field_name,
        width,
        n_width);
  } else {
    n_width = width;
    n_start = 0;
  }

  for (idx = 0; idx < (int)hdl_info->num_trig_fields; idx++) {
    /* Modifying an existing trigger field */
    if (PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).valid) {
      if (strcmp(field_name, PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).name) ==
          0) {
        PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).width = width;
        /* If field width is greater than 8 bytes, then copy the value/mask in
           the lower 8 bytes of the field
        */
        for (i = 0, shift_cntr = 0; i < width; i++) {
          if (i < n_start) {
            PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).value[i] = 0;
            PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).mask[i] = 0;
          } else {
            PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).value[i] =
                (value >> (8 * (n_width - shift_cntr - 1))) & 0xff;
            PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).mask[i] =
                (mask >> (8 * (n_width - shift_cntr - 1))) & 0xff;
            shift_cntr++;
          }
        }
        field_inserted = true;
        break;
      } else {
        continue;
      }
    }
    /* Adding a new trigger field */
    PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).valid = true;
    strncpy(PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).name,
            field_name,
            PIPE_SNAP_TRIG_FIELD_NAME_LEN - 1);
    PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).width = width;
    /* If field width is greater than 8 bytes, then copy the value/mask in
       the lower 8 bytes of the field
    */
    for (i = 0, shift_cntr = 0; i < width; i++) {
      if (i < n_start) {
        PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).value[i] = 0;
        PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).mask[i] = 0;
      } else {
        PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).value[i] =
            (uint8_t)(value >> (8 * (n_width - shift_cntr - 1))) & 0xff;
        PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).mask[i] =
            (mask >> (8 * (n_width - shift_cntr - 1))) & 0xff;
        shift_cntr++;
      }
    }
    field_inserted = true;
    break;
  }

  if (!field_inserted) {
    LOG_ERROR("Dbg: Out of space in trigger spec \n");
    return PIPE_NO_SYS_RESOURCES;
  }

  return status;
}

/* Add a field to the list of triggers (byte-array variant for wide fields) */
static pipe_status_t snapshot_capture_trigger_field_add_bytes(
    pipe_snapshot_hdl_t hdl,
    char *field_name,
    uint8_t *value,
    uint8_t *mask,
    int len) {
  pipe_status_t status = PIPE_SUCCESS;
  pipe_snap_hdl_info_t *hdl_info = NULL;
  bf_dev_id_t dev = 0;
  bf_dev_pipe_t pipe = 0;
  int dir = 0, idx = 0;
  bool field_inserted = false, field_found = false, oper_stage_set = false;
  dev_stage_t s_stage = 0, e_stage = 0, stage = 0;
  int width = 0;

  PIPE_SNAP_DBG("Dbg Trigger: Adding Field %s (bytes, len=%d)\n",
                field_name,
                len);

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return PIPE_INVALID_ARG;
  }

  pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, &pipe, &s_stage, &e_stage, &dir);

  hdl_info = pipe_mgr_snap_hdl_info_get(dev, hdl);
  if (!hdl_info) {
    LOG_ERROR("Handle info is null for handle 0x%x", hdl);
    return PIPE_INVALID_ARG;
  }

  /* Validate the field name */
  for (stage = s_stage; stage <= e_stage; stage++) {
    status = pipe_mgr_snapshot_trig_field_validate(
        dev, pipe, stage, dir, field_name, &field_found, &width);
    if (status != PIPE_SUCCESS) {
      PIPE_SNAP_DBG(
          "Field name %s can not be added due to container_type in stage %d \n",
          field_name,
          stage);
      LOG_ERROR(
          "Field name %s can not be added due to container_type in dev_id %d, "
          "stage %d, direction %d\n",
          field_name,
          dev,
          stage,
          dir);
      continue;
    }
    if (!field_found) {
      PIPE_SNAP_DBG(
          "Field name %s does not exist in stage %d \n", field_name, stage);
    } else {
      if (stage == hdl_info->oper_stage) {
        oper_stage_set = true;
        break;
      } else {
        status = pipe_mgr_snapshot_capture_trigger_fields_validate(hdl, stage);
        if (status == PIPE_SUCCESS) {
          if (!oper_stage_set) {
            hdl_info->oper_stage = stage;
            oper_stage_set = true;
            PIPE_SNAP_DBG("Snapshot 0x%x oper-stage changed to %d \n",
                          hdl,
                          hdl_info->oper_stage);
            break;
          }
        }
      }
    }
  }

  if (!oper_stage_set) {
    LOG_ERROR(
        "Field name %s does not exist or not all trigger fields exist in a "
        "stage \n",
        field_name);
    return PIPE_INVALID_ARG;
  }

  /* Clamp caller length to the field width from the SDE and to the storage
   * capacity of the trigger field info structure (16 bytes). */
  int copy_len = len < width ? len : width;
  if (copy_len > (int)sizeof(PIPE_MGR_SNAP_HDL_FIELD(hdl_info, 0).value)) {
    copy_len = sizeof(PIPE_MGR_SNAP_HDL_FIELD(hdl_info, 0).value);
  }

  /* The value/mask byte arrays are big-endian: byte[0] is the MSB.
   * If the caller supplies fewer bytes than the field width, right-align
   * (pad leading bytes with zero so the supplied bytes correspond to
   * the least-significant portion of the field).  If the caller supplies
   * more bytes, only the last 'width' bytes are used. */
  int field_start = width - copy_len;  /* leading zeros in field */
  int src_start = len > copy_len ? len - copy_len : 0;

  for (idx = 0; idx < (int)hdl_info->num_trig_fields; idx++) {
    /* Modifying an existing trigger field */
    if (PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).valid) {
      if (strcmp(field_name, PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).name) ==
          0) {
        PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).width = width;
        PIPE_MGR_MEMSET(PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).value, 0, 16);
        PIPE_MGR_MEMSET(PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).mask, 0, 16);
        PIPE_MGR_MEMCPY(
            &PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).value[field_start],
            &value[src_start],
            copy_len);
        PIPE_MGR_MEMCPY(
            &PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).mask[field_start],
            &mask[src_start],
            copy_len);
        field_inserted = true;
        break;
      } else {
        continue;
      }
    }
    /* Adding a new trigger field */
    PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).valid = true;
    strncpy(PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).name,
            field_name,
            PIPE_SNAP_TRIG_FIELD_NAME_LEN - 1);
    PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).width = width;
    PIPE_MGR_MEMSET(PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).value, 0, 16);
    PIPE_MGR_MEMSET(PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).mask, 0, 16);
    PIPE_MGR_MEMCPY(
        &PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).value[field_start],
        &value[src_start],
        copy_len);
    PIPE_MGR_MEMCPY(
        &PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).mask[field_start],
        &mask[src_start],
        copy_len);
    field_inserted = true;
    break;
  }

  if (!field_inserted) {
    LOG_ERROR("Dbg: Out of space in trigger spec \n");
    return PIPE_NO_SYS_RESOURCES;
  }

  return status;
}

/* Add a field to the list of triggers */
bf_status_t bf_snapshot_capture_trigger_field_add(pipe_snapshot_hdl_t hdl,
                                                  char *field_name,
                                                  uint64_t value,
                                                  uint64_t mask) {
  bf_status_t bf_status = BF_SUCCESS;
  pipe_status_t status = PIPE_SUCCESS;
  bf_dev_id_t dev = 0;
  bf_dev_pipe_t pipe = 0;
  dev_stage_t stage = 0, s_stage = 0, e_stage = 0;
  int dir = 0;

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return BF_INVALID_ARG;
  }
  pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, &pipe, &s_stage, &e_stage, &dir);

  pipe_sess_hdl_t shdl = pipe_mgr_ctx->int_ses_hndl;
  status = pipe_mgr_api_enter(shdl);
  if (status != PIPE_SUCCESS) return status;

  /* Add field to DB */
  status = snapshot_capture_trigger_field_add(hdl, field_name, value, mask);
  if (status != PIPE_SUCCESS) {
    bf_status = BF_INVALID_ARG;
    goto done;
  }
  LOG_TRACE("Dev %d snapshot 0x%x Added trigger for %s 0x%" PRIx64
            "/0x%" PRIx64,
            dev,
            hdl,
            field_name,
            value,
            mask);

  /* Re-evaulate fsm state's for all stages as oper-stage might have changed */
  pipe_mgr_reevaluate_program_fsm_states(hdl);

  /* program new match field only in oper_stage */
  for (stage = s_stage; stage <= e_stage; stage++) {
    status = snapshot_capture_trigger_set(hdl, dev, pipe, stage, dir);
    if (status != PIPE_SUCCESS) {
      bf_status |= BF_INVALID_ARG;
    }
  }

done:
  pipe_mgr_api_exit(shdl);
  return bf_status;
}

/* Add a field to the list of triggers (by-value struct for wide fields) */
bf_status_t bf_snapshot_capture_trigger_field_add_bytes(
    pipe_snapshot_hdl_t hdl,
    bf_snapshot_trigger_field_t field) {
  bf_status_t bf_status = BF_SUCCESS;
  pipe_status_t status = PIPE_SUCCESS;
  bf_dev_id_t dev = 0;
  bf_dev_pipe_t pipe = 0;
  dev_stage_t stage = 0, s_stage = 0, e_stage = 0;
  int dir = 0;

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return BF_INVALID_ARG;
  }
  pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, &pipe, &s_stage, &e_stage, &dir);

  pipe_sess_hdl_t shdl = pipe_mgr_ctx->int_ses_hndl;
  status = pipe_mgr_api_enter(shdl);
  if (status != PIPE_SUCCESS) return status;

  /* Add field to DB */
  status = snapshot_capture_trigger_field_add_bytes(
      hdl, field.name, field.value, field.mask, sizeof(field.value));
  if (status != PIPE_SUCCESS) {
    bf_status = BF_INVALID_ARG;
    goto done_bytes;
  }
  LOG_TRACE("Dev %d snapshot 0x%x Added trigger (bytes) for %s",
            dev,
            hdl,
            field.name);

  /* Re-evaulate fsm state's for all stages as oper-stage might have changed */
  pipe_mgr_reevaluate_program_fsm_states(hdl);

  /* program new match field only in oper_stage */
  for (stage = s_stage; stage <= e_stage; stage++) {
    status = snapshot_capture_trigger_set(hdl, dev, pipe, stage, dir);
    if (status != PIPE_SUCCESS) {
      bf_status |= BF_INVALID_ARG;
    }
  }

done_bytes:
  pipe_mgr_api_exit(shdl);
  return bf_status;
}

/* Get trigger field value and mask*/
pipe_status_t bf_snapshot_capture_trigger_field_get(pipe_snapshot_hdl_t hdl,
                                                    char *field_name,
                                                    uint64_t *value,
                                                    uint64_t *mask) {
  pipe_status_t status = PIPE_SUCCESS;
  pipe_snap_hdl_info_t *hdl_info = NULL;
  bf_dev_id_t dev = 0;
  bf_dev_pipe_t pipe = 0;
  int dir = 0, idx = 0, i = 0, shift_cntr = 0;
  bool field_got = false;
  int width = 0, n_width = 0, n_start = 0;

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return PIPE_INVALID_ARG;
  }

  pipe_mgr_snapshot_hdl_to_dev_params(hdl, &dev, &pipe, NULL, NULL, &dir);

  hdl_info = pipe_mgr_snap_hdl_info_get(dev, hdl);
  if (!hdl_info) {
    LOG_ERROR("Handle info is null for handle 0x%x", hdl);
    return PIPE_INVALID_ARG;
  }

  pipe_sess_hdl_t shdl = pipe_mgr_ctx->int_ses_hndl;
  status = pipe_mgr_api_enter(shdl);
  if (status != PIPE_SUCCESS) return status;

  for (idx = 0; idx < (int)hdl_info->num_trig_fields; idx++) {
    /* Find the field an existing trigger field */
    if (PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).valid) {
      if (strcmp(field_name, PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).name) ==
          0) {
        /* For fields greater than 8 bytes, use only lower 8 bytes DRV-998 */
        width = PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).width;
        if (width > (int)sizeof(value)) {
          n_width = sizeof(value);
          n_start = sizeof(PIPE_MGR_SNAP_HDL_FIELD(hdl_info, 0).value) -
                    sizeof(value);
          LOG_WARN(
              "Snapshot width of field %s is %d bytes which is greater "
              "than the max supported, using only lower %d bytes",
              field_name,
              width,
              n_width);
        } else {
          n_width = width;
          n_start = 0;
        }

        *value = 0;
        *mask = 0;
        for (i = 0, shift_cntr = 0; i < width; i++) {
          if (i < n_start) continue;
          *value |= (uint64_t)PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).value[i]
                    << (8 * (n_width - shift_cntr - 1));
          *mask |= (uint64_t)PIPE_MGR_SNAP_HDL_FIELD(hdl_info, idx).mask[i]
                   << (8 * (n_width - shift_cntr - 1));
          shift_cntr++;
        }
        field_got = true;
        break;
      }  // name check
    }    // field valid
  }

  if (!field_got) {
    status = PIPE_OBJ_NOT_FOUND;
  }

  pipe_mgr_api_exit(shdl);
  return status;
}

/* Remove all trgger fields helper */
static pipe_status_t pipe_mgr_snapshot_capture_trigger_fields_clr(
    pipe_snapshot_hdl_t hdl) {
  pipe_snap_hdl_info_t *hdl_info = NULL;
  dev_stage_t s_stage = 0;
  bf_dev_id_t dev = 0;

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return PIPE_INVALID_ARG;
  }
  pipe_mgr_snapshot_hdl_to_dev_params(hdl, &dev, NULL, &s_stage, NULL, NULL);

  hdl_info = pipe_mgr_snap_hdl_info_get(dev, hdl);
  if (!hdl_info) {
    LOG_ERROR("Handle info is null for handle 0x%x", hdl);
    return PIPE_INVALID_ARG;
  }
  PIPE_MGR_MEMSET(
      &PIPE_MGR_SNAP_HDL_FIELD(hdl_info, 0),
      0,
      sizeof(pipe_snap_trig_field_info_t) * hdl_info->num_trig_fields);
  /* Reset the oper stage to the start stage */
  hdl_info->oper_stage = s_stage;

  return PIPE_SUCCESS;
}

/* Remove all trgger fields */
bf_status_t bf_snapshot_capture_trigger_fields_clr(pipe_snapshot_hdl_t hdl) {
  bf_status_t bf_status = BF_SUCCESS;
  pipe_status_t status = PIPE_SUCCESS;
  bf_dev_id_t dev = 0;
  bf_dev_pipe_t pipe = 0;
  int dir = 0;
  dev_stage_t stage = 0, s_stage = 0, e_stage = 0;

  if (!pipe_mgr_snapshot_hdl_valid(hdl)) {
    return BF_INVALID_ARG;
  }
  pipe_mgr_snapshot_hdl_to_dev_params(
      hdl, &dev, &pipe, &s_stage, &e_stage, &dir);

  pipe_sess_hdl_t shdl = pipe_mgr_ctx->int_ses_hndl;
  status = pipe_mgr_api_enter(shdl);
  if (status != PIPE_SUCCESS) return status;

  /* Clear the fields */
  pipe_mgr_snapshot_capture_trigger_fields_clr(hdl);

  /* Reprogram */
  for (stage = s_stage; stage <= e_stage; stage++) {
    status = snapshot_capture_trigger_set(hdl, dev, pipe, stage, dir);
    if (status != PIPE_SUCCESS) {
      bf_status |= BF_INVALID_ARG;
    }
  }

  pipe_mgr_api_exit(shdl);
  return bf_status;
}

/* Check if field exists in scope in stage */
bf_status_t bf_snapshot_field_in_scope(bf_dev_id_t dev,
                                       bf_dev_pipe_t pipe,
                                       dev_stage_t stage,
                                       bf_snapshot_dir_t dir,
                                       char *field_name,
                                       bool *exists) {
  int width = 0;

  *exists = false;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return BF_INVALID_ARG;

  if (!PIPE_MGR_SNAP_DEV_VALID(dev)) {
    return BF_INVALID_ARG;
  }
  if (!PIPE_MGR_SNAP_DEV_INFO(dev)) {
    return BF_INVALID_ARG;
  }
  if (!snap_pipe_valid(dev_info, pipe)) {
    return BF_INVALID_ARG;
  }
  if (!snap_stage_valid(dev_info, stage)) {
    return BF_INVALID_ARG;
  }
  if (!PIPE_MGR_SNAP_DIR_VALID(dir)) {
    return BF_INVALID_ARG;
  }
  PIPE_SNAP_DBG("%s: Find field %s in dev %d, pipe %d, stage %d \n",
                __func__,
                field_name,
                dev,
                pipe,
                stage);
  // This function is used for data, data is defined as oPHV for current stage,
  // that means iPHV for the next stage. Context.json describes iPHV.
  if (pipe_mgr_snapshot_field_validate(
          dev, pipe, stage + 1, dir, field_name, false, exists, &width) !=
      PIPE_SUCCESS) {
    return BF_INVALID_ARG;
  }
  return BF_SUCCESS;
}

/* Check if trigger field exists in scope in stage */
bf_status_t bf_snapshot_trigger_field_in_scope(bf_dev_id_t dev,
                                               bf_dev_pipe_t pipe,
                                               dev_stage_t stage,
                                               bf_snapshot_dir_t dir,
                                               char *field_name,
                                               bool *exists) {
  int width = 0;

  *exists = false;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return BF_INVALID_ARG;

  if (!PIPE_MGR_SNAP_DEV_VALID(dev)) {
    return BF_INVALID_ARG;
  }
  if (!PIPE_MGR_SNAP_DEV_INFO(dev)) {
    return BF_INVALID_ARG;
  }
  if (!snap_pipe_valid(dev_info, pipe)) {
    return BF_INVALID_ARG;
  }
  if (!snap_stage_valid(dev_info, stage)) {
    return BF_INVALID_ARG;
  }
  if (!PIPE_MGR_SNAP_DIR_VALID(dir)) {
    return BF_INVALID_ARG;
  }
  PIPE_SNAP_DBG("%s: Find field %s in dev %d, pipe %d, stage %d \n",
                __func__,
                field_name,
                dev,
                pipe,
                stage);
  if (pipe_mgr_snapshot_trig_field_validate(
          dev, pipe, stage, dir, field_name, exists, &width) != PIPE_SUCCESS) {
    return BF_INVALID_ARG;
  }
  return BF_SUCCESS;
}

pipe_snap_stage_field_info_t *pipe_mgr_get_snap_stage_field_info_dict(
    bf_dev_id_t dev,
    bf_dev_pipe_t pipe,
    int stage,
    int direction,
    int *fields_in_dict) {
  *fields_in_dict =
      PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, direction).num_fields_in_dict;
  return (&(PIPE_MGR_SNAP_STAGE_INFO(dev, pipe, stage, direction)
                .all_fields_dict[0]));
}
