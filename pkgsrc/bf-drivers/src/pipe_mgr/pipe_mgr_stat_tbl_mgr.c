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
 * @file pipe_mgr_stats_tbl_mgr.c
 * @date
 *
 *
 * Contains implementation of the APIs for statistics table management
 */

/* Global header includes */
#include <math.h>

/* Module header includes */
#include <target-utils/id/id.h>
#include <target-utils/map/map.h>
#include "pipe_mgr/pipe_mgr_intf.h"
#include "pipe_mgr/pipe_mgr_config.h"
#include "pipe_mgr/pipe_mgr_porting.h"

/* Local header includes */
#include "pipe_mgr_int.h"
#include "pipe_mgr_stat_mgr_int.h"
#include "pipe_mgr_stat_lrt.h"
#include "pipe_mgr_stat_tbl_init.h"
#include "pipe_mgr_log.h"
#include "pipe_mgr_stat_drv_workflows.h"
#include "pipe_mgr_stat_trace.h"

bool stat_mgr_enable_detail_trace;

static pipe_status_t pipe_mgr_stat_mgr_add_ent_hdl_loc(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    bf_dev_pipe_t pipe_id,
    dev_stage_t stage_id,
    pipe_stat_stage_ent_idx_t stage_ent_idx,
    bool pending,
    pipe_stat_data_t *user_count);
static pipe_status_t pipe_mgr_stat_mgr_update_ent_hdl_loc(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    bf_dev_pipe_t pipe_id,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    dev_stage_t src_stage_id,
    pipe_stat_stage_ent_idx_t src_stage_idx,
    dev_stage_t dst_stage_id,
    pipe_stat_stage_ent_idx_t dst_stage_idx);
static void pipe_mgr_stat_mgr_mark_entry_del_in_progress(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    bf_dev_pipe_t pipe_id,
    dev_stage_t stage_id,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    pipe_stat_stage_ent_idx_t stage_ent_idx);
static pipe_status_t pipe_mgr_stat_mgr_del_ent_hdl_loc(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    bf_dev_pipe_t pipe_id,
    dev_stage_t stage_id,
    pipe_stat_stage_ent_idx_t stage_ent_idx,
    bool inline_processing);
static pipe_status_t pipe_mgr_stat_mgr_clear_entry_pending(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    bf_dev_pipe_t pipe_id,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    dev_stage_t stage_id,
    pipe_stat_stage_ent_idx_t stage_ent_idx);
static pipe_mgr_stat_entry_info_t *pipe_mgr_stat_mgr_get_ent_idx_info(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    bf_dev_pipe_t pipe_id,
    dev_stage_t stage_id,
    pipe_stat_stage_ent_idx_t stage_ent_idx);
static pipe_status_t pipe_mgr_stat_mgr_copy_idx_count(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    bf_dev_pipe_t pipe_id,
    dev_stage_t src_stage_id,
    dev_stage_t dst_stage_id,
    pipe_stat_stage_ent_idx_t src_stat_ent_idx,
    pipe_stat_stage_ent_idx_t dst_stat_ent_idx);
static pipe_status_t pipe_mgr_stat_mgr_get_ent_hdl_count(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    pipe_mat_ent_hdl_t ent_hdl,
    pipe_stat_data_t *stat_data);
static pipe_status_t pipe_mgr_stat_mgr_update_ent_hdl_count(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    pipe_mat_ent_hdl_t ent_hdl,
    pipe_stat_data_t *stat_data,
    bool set_in_prog);
static pipe_status_t pipe_mgr_stat_mgr_stat_ent_move(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    bf_dev_pipe_t pipe_id,
    dev_stage_t src_stage_id,
    dev_stage_t dst_stage_id,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    pipe_stat_stage_ent_idx_t src_stat_ent_idx,
    pipe_stat_stage_ent_idx_t dst_stat_ent_idx,
    bool inline_processing);
static pipe_status_t pipe_mgr_stat_mgr_commit_ent_hdl_loc(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    bf_dev_pipe_t pipe_id,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    dev_stage_t src_stage_id,
    pipe_stat_stage_ent_idx_t src_stage_idx,
    dev_stage_t dst_stage_id,
    pipe_stat_stage_ent_idx_t dst_stage_idx,
    bool inline_processing);

static pipe_status_t pipe_mgr_stat_mgr_lock_stage_helper(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    dev_stage_t stage_id,
    lock_id_t lock_id,
    bool is_lock) {
  pipe_bitmap_t *pipe_bmp = &stat_tbl_instance->pipe_bmp;

  /* If the device is locked, which implies that we are in fast-reconfig
   * mode, nothing to do for the lock, since the lock instruction will
   * not be issued, and we should not be storing any state in anticipation
   * of the response.
   */
  if (pipe_mgr_fast_recfg_warm_init_in_progress(stat_tbl->device_id)) {
    return PIPE_SUCCESS;
  }

  /* Allocate lock state which is same as barrier state */
  pipe_mgr_stat_barrier_state_t *lock_state;
  lock_state =
      pipe_mgr_stat_mgr_alloc_barrier_state(pipe_bmp, stage_id, lock_id);
  if (lock_state == NULL) {
    LOG_ERROR("%s/%d : Malloc failure", __func__, __LINE__);
    return PIPE_NO_SYS_RESOURCES;
  }
  lock_state->operation =
      is_lock ? PIPE_MGR_STAT_LOCK_OP : PIPE_MGR_STAT_UNLOCK_OP;

  pipe_mgr_stat_mgr_add_barrier_state(
      stat_tbl, stat_tbl_instance, pipe_bmp, lock_state);

  return PIPE_SUCCESS;
}
static pipe_status_t pipe_mgr_stat_mgr_lock_stage(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    dev_stage_t stage_id,
    lock_id_t lock_id) {
  return pipe_mgr_stat_mgr_lock_stage_helper(
      stat_tbl, stat_tbl_instance, stage_id, lock_id, true);
}
static pipe_status_t pipe_mgr_stat_mgr_unlock_stage(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    dev_stage_t stage_id,
    lock_id_t lock_id) {
  return pipe_mgr_stat_mgr_lock_stage_helper(
      stat_tbl, stat_tbl_instance, stage_id, lock_id, false);
}

static pipe_status_t pipe_mgr_stat_mgr_process_entry_add(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    dev_stage_t stage_id,
    pipe_stat_stage_ent_idx_t stage_ent_idx,
    pipe_stat_data_t *sw_count) {
  pipe_status_t status = PIPE_SUCCESS;
  uint8_t pipe_iter = 0;

  /* Allocate memory first so that we don't need to clean up the task_list if
   * the the alloc fails on the last pipe. */
  pipe_mgr_stat_mgr_task_node_t
      *task_nodes[stat_tbl->dev_info->num_active_pipes];
  for (pipe_iter = 0; pipe_iter < stat_tbl->dev_info->num_active_pipes;
       ++pipe_iter) {
    task_nodes[pipe_iter] = NULL;
  }
  PIPE_BITMAP_ITER(&stat_tbl_instance->pipe_bmp, pipe_iter) {
    task_nodes[pipe_iter] = PIPE_MGR_CALLOC(1, sizeof *task_nodes[pipe_iter]);
    if (task_nodes[pipe_iter] == NULL) {
      LOG_ERROR("%s:%d Malloc failure", __func__, __LINE__);
      PIPE_BITMAP_ITER(&stat_tbl_instance->pipe_bmp, pipe_iter) {
        if (task_nodes[pipe_iter]) {
          PIPE_MGR_FREE(task_nodes[pipe_iter]);
        }
      }
      return PIPE_NO_SYS_RESOURCES;
    }
    task_nodes[pipe_iter]->type = PIPE_MGR_STAT_TASK_ENTRY_ADD;
    pipe_mgr_stat_mgr_task_type_ent_add_t *add_node =
        &task_nodes[pipe_iter]->u.ent_add_node;
    add_node->stage_ent_idx = stage_ent_idx;
    add_node->mat_ent_hdl = mat_ent_hdl;
    add_node->stage_id = stage_id;
  }

  uint32_t deferred = 0;
  for (pipe_iter = 0; pipe_iter < stat_tbl->dev_info->num_active_pipes;
       ++pipe_iter) {
    if (!task_nodes[pipe_iter]) continue;
    /* If there are pending operations to the table, defer the add until those
     * operations have completed. */
    bool enqueued = true;

    PIPE_MGR_LOCK(&stat_tbl_instance->barrier_data_mtx);
    pipe_mgr_stat_barrier_list_node_t *tail;
    BF_LIST_DLL_LAST(
        stat_tbl_instance->barrier_list[pipe_iter], tail, next, prev);
    if (tail) {
      BF_LIST_DLL_AP(tail->task_list, task_nodes[pipe_iter], next, prev);
    } else {
      enqueued = false;
    }

    /* We must update the ent_hdl_loc while holding the lock to protect the
     * task_list.  There is a chance this operation may have been able to go
     * through immediately (non-LR(t) table and entry initialized w/ move-reg
     * Pop) meaning the instruction generating the barrier response is NOT
     * sitting in our pending ilist but has already been sent to HW.  In such a
     * case another thread may service the task list and execute this operation
     * as soon as we release the lock. */
    if (enqueued) {
      status = pipe_mgr_stat_mgr_add_ent_hdl_loc(stat_tbl,
                                                 stat_tbl_instance,
                                                 mat_ent_hdl,
                                                 pipe_iter,
                                                 stage_id,
                                                 stage_ent_idx,
                                                 true,
                                                 sw_count);
      if (status != PIPE_SUCCESS) {
        PIPE_MGR_DBGCHK(0);
        PIPE_MGR_UNLOCK(&stat_tbl_instance->barrier_data_mtx);
        return status;
      }
    }

    PIPE_MGR_UNLOCK(&stat_tbl_instance->barrier_data_mtx);

    if (!enqueued) {
      status = pipe_mgr_stat_mgr_add_ent_hdl_loc(stat_tbl,
                                                 stat_tbl_instance,
                                                 mat_ent_hdl,
                                                 pipe_iter,
                                                 stage_id,
                                                 stage_ent_idx,
                                                 false,
                                                 sw_count);
      if (status != PIPE_SUCCESS) {
        PIPE_MGR_DBGCHK(0);
        return status;
      }
      /* Set our shadow of the HW counter to zero since the add is being
       * processed immediately (not deferred). */
      pipe_stat_data_t hw_count = {0};
      pipe_mgr_stat_mgr_set_ent_idx_count(stat_tbl,
                                          stat_tbl_instance,
                                          false,
                                          pipe_iter,
                                          stage_id,
                                          stage_ent_idx,
                                          &hw_count);
      /* Note that LR(t) cases can never come here, there may be unprocessed
       * evections on this location which were for an entry which is now
       * deleted.  We do not want those to be counted against this entry. */
      if (stat_tbl->lrt_enabled &&
          !pipe_mgr_is_device_locked(stat_tbl->device_id)) {
        /* Coming into this if case is an error, the DBGCHK condition will cause
         * us to report this event. */
        PIPE_MGR_DBGCHK(stat_tbl->lrt_enabled == false);
      }

      /* Free the task node since we've processed it. */
      PIPE_MGR_FREE(task_nodes[pipe_iter]);
    } else {
      deferred |= 1u << pipe_iter;
    }
  }
  pipe_mgr_stat_mgr_trace_add(stat_tbl,
                              stat_tbl_instance,
                              mat_ent_hdl,
                              stage_id,
                              stage_ent_idx,
                              deferred);

  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_stat_mgr_process_entry_del(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    dev_stage_t stage_id,
    pipe_stat_stage_ent_idx_t stage_ent_idx) {
  pipe_status_t status = PIPE_SUCCESS;
  bf_dev_pipe_t pipe_iter;

  /* Allocate memory first so that we don't need to clean up the task_list if
   * the the alloc fails on the last pipe. */
  pipe_mgr_stat_mgr_task_node_t
      *task_nodes[stat_tbl->dev_info->num_active_pipes];
  for (pipe_iter = 0; pipe_iter < stat_tbl->dev_info->num_active_pipes;
       ++pipe_iter) {
    task_nodes[pipe_iter] = NULL;
  }
  PIPE_BITMAP_ITER(&stat_tbl_instance->pipe_bmp, pipe_iter) {
    task_nodes[pipe_iter] = PIPE_MGR_CALLOC(1, sizeof *task_nodes[pipe_iter]);
    if (task_nodes[pipe_iter] == NULL) {
      LOG_ERROR("%s:%d Malloc failure", __func__, __LINE__);
      PIPE_BITMAP_ITER(&stat_tbl_instance->pipe_bmp, pipe_iter) {
        if (task_nodes[pipe_iter]) {
          PIPE_MGR_FREE(task_nodes[pipe_iter]);
        }
      }
      return PIPE_NO_SYS_RESOURCES;
    }
    task_nodes[pipe_iter]->type = PIPE_MGR_STAT_TASK_ENTRY_DEL;
    pipe_mgr_stat_mgr_task_type_ent_del_t *del_node =
        &task_nodes[pipe_iter]->u.ent_del_node;
    del_node->ent_hdl = mat_ent_hdl;
    del_node->stage_ent_idx = stage_ent_idx;
    del_node->stage_id = stage_id;
  }

  uint32_t deferred = 0;
  for (pipe_iter = 0; pipe_iter < stat_tbl->dev_info->num_active_pipes;
       ++pipe_iter) {
    if (!task_nodes[pipe_iter]) continue;
    bool enqueued = true;

    PIPE_MGR_LOCK(&stat_tbl_instance->barrier_data_mtx);
    pipe_mgr_stat_barrier_list_node_t *tail;
    BF_LIST_DLL_LAST(
        stat_tbl_instance->barrier_list[pipe_iter], tail, next, prev);
    if (tail) {
      BF_LIST_DLL_AP(tail->task_list, task_nodes[pipe_iter], next, prev);
    } else {
      enqueued = false;
    }

    if (enqueued) {
      pipe_mgr_stat_mgr_mark_entry_del_in_progress(stat_tbl,
                                                   stat_tbl_instance,
                                                   pipe_iter,
                                                   stage_id,
                                                   mat_ent_hdl,
                                                   stage_ent_idx);
    }

    PIPE_MGR_UNLOCK(&stat_tbl_instance->barrier_data_mtx);

    if (!enqueued) {
      status = pipe_mgr_stat_mgr_del_ent_hdl_loc(stat_tbl,
                                                 stat_tbl_instance,
                                                 mat_ent_hdl,
                                                 pipe_iter,
                                                 stage_id,
                                                 stage_ent_idx,
                                                 true);
      if (status != PIPE_SUCCESS) {
        LOG_ERROR(
            "%s:%d Dev %d tbl %s 0x%x, error deleting entry handle location "
            "for hdl 0x%x, pipe id %d, stage id %d, idx %d err %s",
            __func__,
            __LINE__,
            stat_tbl->device_id,
            stat_tbl->name,
            stat_tbl->stat_tbl_hdl,
            mat_ent_hdl,
            pipe_iter,
            stage_id,
            stage_ent_idx,
            pipe_str_err(status));
        return status;
      }
      PIPE_MGR_FREE(task_nodes[pipe_iter]);
    } else {
      deferred |= 1u << pipe_iter;
    }
  }
  pipe_mgr_stat_mgr_trace_del(stat_tbl,
                              stat_tbl_instance,
                              mat_ent_hdl,
                              stage_id,
                              stage_ent_idx,
                              deferred);
  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_stat_mgr_process_move(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    dev_stage_t src_stage_id,
    dev_stage_t dst_stage_id,
    pipe_stat_stage_ent_idx_t src_stat_ent_idx,
    pipe_stat_stage_ent_idx_t dst_stat_ent_idx) {
  bf_dev_pipe_t pipe_iter;
  /* Allocate memory first so that we don't need to clean up the task_list if
   * the the alloc fails on the last pipe. */
  pipe_mgr_stat_mgr_task_node_t
      *task_nodes[stat_tbl->dev_info->num_active_pipes];
  for (unsigned i = 0; i < stat_tbl->dev_info->num_active_pipes; ++i)
    task_nodes[i] = NULL;
  PIPE_BITMAP_ITER(&stat_tbl_instance->pipe_bmp, pipe_iter) {
    task_nodes[pipe_iter] = PIPE_MGR_CALLOC(1, sizeof *task_nodes[pipe_iter]);
    if (task_nodes[pipe_iter] == NULL) {
      LOG_ERROR("%s:%d Malloc failure", __func__, __LINE__);
      PIPE_BITMAP_ITER(&stat_tbl_instance->pipe_bmp, pipe_iter) {
        if (task_nodes[pipe_iter]) {
          PIPE_MGR_FREE(task_nodes[pipe_iter]);
        }
      }
      return PIPE_NO_SYS_RESOURCES;
    }
    task_nodes[pipe_iter]->type = PIPE_MGR_STAT_TASK_MOVE;
    pipe_mgr_stat_mgr_task_type_move_t *mov_node =
        &task_nodes[pipe_iter]->u.move_node;
    mov_node->mat_ent_hdl = mat_ent_hdl;
    mov_node->src_stage_id = src_stage_id;
    mov_node->dst_stage_id = dst_stage_id;
    mov_node->src_ent_idx = src_stat_ent_idx;
    mov_node->dst_ent_idx = dst_stat_ent_idx;
  }

  uint32_t deferred = 0;
  for (pipe_iter = 0; pipe_iter < stat_tbl->dev_info->num_active_pipes;
       ++pipe_iter) {
    if (!task_nodes[pipe_iter]) continue;
    /* If there are pending operations to the table, defer the move until those
     * operations have completed. */
    bool enqueued = true;

    PIPE_MGR_LOCK(&stat_tbl_instance->barrier_data_mtx);
    pipe_mgr_stat_barrier_list_node_t *tail;
    BF_LIST_DLL_LAST(
        stat_tbl_instance->barrier_list[pipe_iter], tail, next, prev);
    if (tail) {
      BF_LIST_DLL_AP(tail->task_list, task_nodes[pipe_iter], next, prev);
    } else {
      enqueued = false;
    }

    if (enqueued) {
      /* Since the move is deferred, update the entry's current location from
       * src_stage_id/src_stat_ent_idx to dst, but leave the entry's deferred
       * location unchanged.  As barrier responses arrive the deferred location
       * will catch up to the current location. */
      pipe_mgr_stat_mgr_update_ent_hdl_loc(stat_tbl,
                                           stat_tbl_instance,
                                           pipe_iter,
                                           mat_ent_hdl,
                                           src_stage_id,
                                           src_stat_ent_idx,
                                           dst_stage_id,
                                           dst_stat_ent_idx);
    }

    PIPE_MGR_UNLOCK(&stat_tbl_instance->barrier_data_mtx);

    if (!enqueued) {
      /* Since the move was not enqueued move both the current and deferred
       * locations on the entry. */
      pipe_mgr_stat_mgr_stat_ent_move(stat_tbl,
                                      stat_tbl_instance,
                                      pipe_iter,
                                      src_stage_id,
                                      dst_stage_id,
                                      mat_ent_hdl,
                                      src_stat_ent_idx,
                                      dst_stat_ent_idx,
                                      true);
      PIPE_MGR_FREE(task_nodes[pipe_iter]);
    } else {
      deferred |= 1u << pipe_iter;
    }
  }
  pipe_mgr_stat_mgr_trace_mov(stat_tbl,
                              stat_tbl_instance,
                              mat_ent_hdl,
                              dst_stage_id,
                              dst_stat_ent_idx,
                              src_stage_id,
                              src_stat_ent_idx,
                              deferred);

  return PIPE_SUCCESS;
}

uint8_t pipe_mgr_stat_mgr_ent_get_stage(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    pipe_stat_ent_idx_t stat_ent_idx,
    dev_stage_t stage_id,
    pipe_stat_stage_ent_idx_t *stage_ent_idx) {
  uint8_t stage_idx = 0;
  uint32_t accum = 0;

  pipe_mgr_stat_tbl_stage_info_t *stat_tbl_stage_info = NULL;

  /* If the stat_ent_idx is greater than the number of entries in any one of the
   * stages, this implies that the compiler allocated the stat table in all
   * the stages where a match entry was referring to it. Else the entry index
   * is just translated from the global entry index to a stage specific entry
   * index.
   */
  stat_tbl_stage_info = stat_tbl_instance->stat_tbl_stage_info;

  if (stat_tbl->over_allocated == true) {
    /* The table is over allocated, so the passed in stat_ent_idx is present
     * in all of the stages in which the table is present.
     */
    if (stage_id == 0xff) {
      /* Return the first stage in which the table is present */
      *stage_ent_idx = stat_ent_idx;
      return stat_tbl_stage_info->stage_id;
    } else {
      for (stage_idx = 0; stage_idx < stat_tbl_instance->num_stages;
           stage_idx++) {
        stat_tbl_stage_info =
            &stat_tbl_instance->stat_tbl_stage_info[stage_idx];

        if (stage_id == stat_tbl_stage_info->stage_id) {
          /* Return the next stage id if there is one */
          if (stage_idx == stat_tbl_instance->num_stages - 1) {
            return 0xff;
          }
          *stage_ent_idx = stat_ent_idx;
          return stat_tbl_instance->stat_tbl_stage_info[stage_idx + 1].stage_id;
        }
      }
    }
  } else {
    /* Need to translate the global stat entry index to the stage specific
     * entry index.
     */
    for (stage_idx = 0; stage_idx < stat_tbl_instance->num_stages;
         stage_idx++) {
      stat_tbl_stage_info = &stat_tbl_instance->stat_tbl_stage_info[stage_idx];
      if (stat_ent_idx < (stat_tbl_stage_info->ent_idx_offset +
                          stat_tbl_stage_info->num_entries)) {
        if (stat_tbl_stage_info->stage_id != stage_id) {
          *stage_ent_idx = stat_ent_idx - accum;
          return stat_tbl_stage_info->stage_id;
        } else {
          return 0xff;
        }
      }
      accum += stat_tbl_stage_info->num_entries;
    }
  }

  return 0xff;
}

pipe_status_t pipe_mgr_stat_mgr_verify_idx(bf_dev_id_t device_id,
                                           bf_dev_pipe_t pipe_id,
                                           pipe_stat_tbl_hdl_t stat_tbl_hdl,
                                           pipe_stat_ent_idx_t stat_ent_idx) {
  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance = NULL;

  stat_tbl = pipe_mgr_stat_tbl_get(device_id, stat_tbl_hdl);
  if (stat_tbl == NULL) {
    LOG_ERROR("%s : No information found for stat table with handle %d",
              __func__,
              stat_tbl_hdl);
    return PIPE_OBJ_NOT_FOUND;
  }

  stat_tbl_instance = pipe_mgr_stat_tbl_get_instance(stat_tbl, pipe_id);
  if (stat_tbl_instance == NULL) {
    LOG_ERROR("%s:%d Stat table instance for pipe id %d, tbl 0x%x not found",
              __func__,
              __LINE__,
              pipe_id,
              stat_tbl_hdl);
    return PIPE_OBJ_NOT_FOUND;
  }

  /* Check if the passed in entry index is a valid one */
  if (stat_ent_idx >= stat_tbl_instance->num_entries) {
    LOG_ERROR(
        "%s: Passed in stat entry index exceeds the capacity of the stats"
        " table, handle 0x%x, device id %d",
        __func__,
        stat_tbl_hdl,
        device_id);
    return PIPE_INVALID_ARG;
  }

  return PIPE_SUCCESS;
}

static inline pipe_stat_stage_ent_idx_t pipe_mgr_stat_mgr_compute_stage_ent_idx(
    pipe_mgr_stat_tbl_t *stat_tbl,
    bf_dev_pipe_t pipe_id,
    uint8_t stage_id,
    pipe_stat_ent_idx_t stat_ent_idx) {
  pipe_mgr_stat_tbl_stage_info_t *stat_tbl_stage_info = NULL;

  if (stat_tbl->over_allocated == true) {
    return stat_ent_idx;
  } else {
    stat_tbl_stage_info =
        pipe_mgr_stat_mgr_get_stage_info(stat_tbl, pipe_id, stage_id);
    if (!stat_tbl_stage_info) {
      PIPE_MGR_DBGCHK(0);
      return ~0;
    }
    return stat_ent_idx - stat_tbl_stage_info->ent_idx_offset;
  }
}

pipe_status_t rmt_stat_mgr_stat_ent_attach(
    uint8_t device_id,
    bf_dev_pipe_t pipe_id,
    uint8_t stage_id,
    pipe_stat_tbl_hdl_t stat_tbl_hdl,
    pipe_stat_ent_idx_t stat_ent_idx,
    rmt_virt_addr_t *stat_ent_virt_addr) {
  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance = NULL;
  pipe_mgr_stat_tbl_stage_info_t *stat_tbl_stage_info = NULL;
  pipe_stat_stage_ent_idx_t stage_ent_idx = PIPE_STAT_ENT_INVALID_ENTRY_INDEX;

  stat_tbl = pipe_mgr_stat_tbl_get(device_id, stat_tbl_hdl);

  if (stat_tbl == NULL) {
    LOG_ERROR("%s : No information found for stat table with handle %d",
              __func__,
              stat_tbl_hdl);
    return PIPE_OBJ_NOT_FOUND;
  }

  stat_tbl_instance = pipe_mgr_stat_tbl_get_instance(stat_tbl, pipe_id);

  if (stat_tbl_instance == NULL) {
    LOG_ERROR("%s:%d Stat table instance for pipe id %d, tbl %d not found",
              __func__,
              __LINE__,
              pipe_id,
              stat_tbl_hdl);
    return PIPE_OBJ_NOT_FOUND;
  }

  /* First check if the passed in entry index is a valid one */
  if (stat_ent_idx >= stat_tbl_instance->num_entries) {
    LOG_ERROR(
        "%s: Passed in entry index exceeds the capacity of the stats"
        " table, handle %d, device id %d",
        __func__,
        stat_tbl_hdl,
        device_id);
    return PIPE_INVALID_ARG;
  }

  stat_tbl_stage_info =
      pipe_mgr_stat_mgr_get_stage_info(stat_tbl, pipe_id, stage_id);

  if (stat_tbl_stage_info == NULL) {
    LOG_ERROR(
        "%s : Stat table stage info not found for stat table with handle "
        "%d, pipe id %d, stage id %d",
        __func__,
        stat_tbl_hdl,
        pipe_id,
        stage_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  stage_ent_idx = pipe_mgr_stat_mgr_compute_stage_ent_idx(
      stat_tbl, pipe_id, stage_id, stat_ent_idx);

  /* Form this entry's virtual address */
  *stat_ent_virt_addr = pipe_mgr_stat_mgr_compute_ent_virt_addr(
      stat_tbl_stage_info, stage_ent_idx);

  PIPE_MGR_STAT_TRACE(device_id,
                      stat_tbl_hdl,
                      stat_tbl->name,
                      stat_ent_idx,
                      stage_ent_idx,
                      pipe_id,
                      stage_id,
                      "Attached stat entry with addr 0x%x",
                      *stat_ent_virt_addr);

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stat_mgr_ent_query(pipe_sess_hdl_t sess_hdl,
                                          dev_target_t dev_tgt,
                                          pipe_stat_tbl_hdl_t stat_tbl_hdl,
                                          pipe_stat_ent_idx_t *stat_ent_idx_arr,
                                          size_t num_entries,
                                          pipe_stat_data_t **stat_data_arr) {
  (void)sess_hdl;
  pipe_status_t status = PIPE_SUCCESS;
  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance = NULL;
  pipe_stat_stage_ent_idx_t stage_ent_idx = 0;
  pipe_stat_data_t local_stat_data = {0};
  bf_dev_pipe_t pipe = 0;
  uint8_t stage_id = 0;
  pipe_bitmap_t pipe_bmp;
  PIPE_BITMAP_INIT(&pipe_bmp, PIPE_BMP_SIZE);

  stat_tbl = pipe_mgr_stat_tbl_get(dev_tgt.device_id, stat_tbl_hdl);

  if (stat_tbl == NULL) {
    LOG_ERROR("%s : Stat table info not found, device id %d, tbl hdl %#x",
              __func__,
              dev_tgt.device_id,
              stat_tbl_hdl);
    return PIPE_OBJ_NOT_FOUND;
  }

  stat_tbl_instance =
      pipe_mgr_stat_tbl_get_instance(stat_tbl, dev_tgt.dev_pipe_id);

  if (stat_tbl_instance == NULL) {
    LOG_ERROR(
        "%s : Stat tbl instance not found, pipe id %d, tbl hdl %#x "
        "device id %d",
        __func__,
        dev_tgt.dev_pipe_id,
        stat_tbl_hdl,
        dev_tgt.device_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  for (size_t i = 0; i < num_entries; i++) {
    pipe_stat_data_t ret_stat_data = {0};
    pipe_stat_ent_idx_t stat_ent_idx = stat_ent_idx_arr[i];
    pipe_stat_data_t *stat_data = stat_data_arr[i];

    if (stat_ent_idx >= stat_tbl_instance->num_entries) {
      LOG_ERROR(
          "Dev %d stat table 0x%x (%s) cannot query index %u, table only has "
          "%u "
          "entries",
          stat_tbl->device_id,
          stat_tbl_hdl,
          stat_tbl->name,
          stat_ent_idx,
          stat_tbl_instance->num_entries);
      return PIPE_INVALID_ARG;
    }

    PIPE_BITMAP_ASSIGN(&pipe_bmp, &(stat_tbl_instance->pipe_bmp));

    PIPE_BITMAP_ITER(&pipe_bmp, pipe) {
      stage_id = pipe_mgr_stat_mgr_ent_get_stage(
          stat_tbl, stat_tbl_instance, stat_ent_idx, 0xff, &stage_ent_idx);
      while (stage_id != 0xff) {
        PIPE_MGR_MEMSET(&local_stat_data, 0, sizeof(pipe_stat_data_t));
        status = pipe_mgr_stat_mgr_get_ent_idx_count(stat_tbl,
                                                     stat_tbl_instance,
                                                     pipe,
                                                     stage_id,
                                                     stage_ent_idx,
                                                     &local_stat_data);
        if (status != PIPE_SUCCESS) {
          LOG_ERROR(
              "%s:%d Error in getting entry idx count for idx %d"
              " pipe %d, stage id %d, tbl 0x%x, err %s",
              __func__,
              __LINE__,
              stat_ent_idx,
              pipe,
              stage_id,
              stat_tbl->stat_tbl_hdl,
              pipe_str_err(status));
          return status;
        }
        ret_stat_data.packets += local_stat_data.packets;
        ret_stat_data.bytes += local_stat_data.bytes;

        stage_id = pipe_mgr_stat_mgr_ent_get_stage(stat_tbl,
                                                   stat_tbl_instance,
                                                   stat_ent_idx,
                                                   stage_id,
                                                   &stage_ent_idx);
      }
    }

    /* Add up the user set count */
    ret_stat_data.packets +=
        stat_tbl_instance->user_idx_count[stat_ent_idx].packets;
    ret_stat_data.bytes +=
        stat_tbl_instance->user_idx_count[stat_ent_idx].bytes;

    *stat_data = ret_stat_data;

    if (stat_mgr_enable_detail_trace) {
      PIPE_MGR_STAT_TRACE(dev_tgt.device_id,
                          stat_tbl_hdl,
                          stat_tbl->name,
                          stat_ent_idx,
                          -1,
                          dev_tgt.dev_pipe_id,
                          -1,
                          "Stat ent query, pkt count %" PRIu64
                          ", byte count %" PRIu64,
                          stat_data->packets,
                          stat_data->bytes);
    }
  }

  return PIPE_SUCCESS;
}

/* Helper function to check if an entry is the default entry of a table and, if
 * it is, whether or not the entry uses direct stats.
 * Returns true if it is the default entry and it does NOT use direct stats.
 * Returns false otherwise. */
static inline bool is_dflt_entry_without_dir_stats(pipe_sess_hdl_t sess_hdl,
                                                   bf_dev_id_t device_id,
                                                   bf_dev_pipe_t pipe_id,
                                                   pipe_mat_tbl_hdl_t tbl_hdl,
                                                   pipe_mat_ent_hdl_t ent_hdl) {
  dev_target_t dev_tgt;
  dev_tgt.device_id = device_id;
  dev_tgt.dev_pipe_id = pipe_id;
  pipe_mat_ent_hdl_t dflt_hdl = 0;
  pipe_status_t sts;
  /* First check if it is the default entry. */
  sts = pipe_mgr_table_get_default_entry_handle(
      sess_hdl, dev_tgt, tbl_hdl, &dflt_hdl);
  if (sts != PIPE_SUCCESS) return false;
  if (ent_hdl != dflt_hdl) return false;

  /* Entry get from SW is needed to determine the action function handle. */
  pipe_action_spec_t aspec;
  pipe_act_fn_hdl_t act_fn_hdl;
  sts = pipe_mgr_table_get_default_entry(sess_hdl,
                                         dev_tgt,
                                         tbl_hdl,
                                         &aspec,
                                         &act_fn_hdl,
                                         false,
                                         PIPE_RES_GET_FLAG_ENTRY,
                                         NULL);
  if (sts != PIPE_SUCCESS) return false;

  /* Check if the action function uses direct stats. */
  bool has_stats = false;
  sts = pipe_mgr_get_action_dir_res_usage(dev_tgt.device_id,
                                          tbl_hdl,
                                          act_fn_hdl,
                                          &has_stats,
                                          NULL,
                                          NULL,
                                          NULL,
                                          NULL);
  if (sts != PIPE_SUCCESS) return false;
  return !has_stats;
}

pipe_status_t rmt_stat_mgr_query_direct_stats(pipe_sess_hdl_t sess_hdl,
                                              bf_dev_id_t device_id,
                                              bf_dev_pipe_t pipe_id,
                                              pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                              pipe_mat_ent_hdl_t mat_ent_hdl,
                                              pipe_stat_tbl_hdl_t stat_tbl_hdl,
                                              pipe_stat_data_t *stat_data) {
  pipe_status_t status = PIPE_SUCCESS;
  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance = NULL;
  pipe_stat_ent_idx_t stat_ent_idx = 0;
  (void)sess_hdl;

  stat_tbl = pipe_mgr_stat_tbl_get(device_id, stat_tbl_hdl);
  if (stat_tbl == NULL) {
    LOG_ERROR("%s:%d Stat tbl 0x%x, device id %d not found",
              __func__,
              __LINE__,
              stat_tbl_hdl,
              device_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  /* Only valid for directly referenced statistics table */
  PIPE_MGR_DBGCHK(stat_tbl->ref_type == PIPE_TBL_REF_TYPE_DIRECT);

  stat_tbl_instance = pipe_mgr_stat_tbl_get_instance(stat_tbl, pipe_id);
  if (stat_tbl_instance == NULL) {
    LOG_ERROR(
        "%s/%d : Stat tbl instance for tbl %d, device id %d, pipe id %d"
        " not found",
        __func__,
        __LINE__,
        stat_tbl_hdl,
        device_id,
        pipe_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  status = pipe_mgr_stat_mgr_get_ent_hdl_count(
      stat_tbl, stat_tbl_instance, mat_ent_hdl, stat_data);

  if (status == PIPE_OBJ_NOT_FOUND) {
    if (is_dflt_entry_without_dir_stats(
            sess_hdl, device_id, pipe_id, mat_tbl_hdl, mat_ent_hdl)) {
      LOG_WARN(
          "%s:%d Querying direct stat tbl 0x%x on dev %d pipe %x for default "
          "entry but stats are not enabled for the entry",
          __func__,
          __LINE__,
          stat_tbl->stat_tbl_hdl,
          device_id,
          pipe_id);
      stat_data->bytes = 0;
      stat_data->packets = 0;
      return PIPE_SUCCESS;
    }
    LOG_ERROR(
        "%s:%d Entry handle info for entry hdl 0x%x, stat table 0x%x"
        " pipe id %x not found",
        __func__,
        __LINE__,
        mat_ent_hdl,
        stat_tbl->stat_tbl_hdl,
        stat_tbl_instance->pipe_id);
    return status;
  } else if (status != PIPE_SUCCESS) {
    return status;
  }

  if (stat_mgr_enable_detail_trace) {
    PIPE_MGR_STAT_TRACE(device_id,
                        stat_tbl_hdl,
                        stat_tbl->name,
                        stat_ent_idx,
                        -1,
                        pipe_id,
                        -1,
                        "Querying of direct stat entry. Packet count %" PRIu64
                        " byte count %" PRIu64,
                        stat_data->packets,
                        stat_data->bytes);
  }

  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_stat_tbl_instance_database_sync(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    pipe_mgr_stat_tbl_sync_cback_fn cback_fn,
    void *cookie) {
  pipe_status_t status = PIPE_SUCCESS;

  /* Invoke the driver workflow function to send the appropriate instructions
   * for doing a table dump.
   */
  status = pipe_mgr_stat_tbl_dump_drv_workflow(
      sess_hdl, dev_tgt, stat_tbl, stat_tbl_instance, cback_fn, cookie);

  if (status != PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Error in executing stat tbl dump for tbl %d, device id %d",
              __func__,
              __LINE__,
              stat_tbl->stat_tbl_hdl,
              dev_tgt.device_id);
    return status;
  }

  if (stat_mgr_enable_detail_trace) {
    PIPE_MGR_STAT_TRACE(dev_tgt.device_id,
                        stat_tbl->stat_tbl_hdl,
                        stat_tbl->name,
                        -1,
                        -1,
                        dev_tgt.dev_pipe_id,
                        -1,
                        "Request to dump stat table %s",
                        stat_tbl->name);
  }

  return status;
}

/* The following struct and callback function are used in the case of the upper
 * layer (either PD caller or BFRT) requesting a pipe-all table sync for a table
 * with multiple instances.  This is translated into separate syncs for each
 * instance and the lower level stats manager code will execute the CB as each
 * instance completes its sync.  To ensure the caller's CB is executed only once
 * after all syncs are complete we use this intermediate CB which tracks the
 * status of each instance's sync and executes the caller's CB once all
 * instances have completed their sync. */
struct pipe_mgr_stat_multi_sync_state_t {
  pipe_mgr_mutex_t mtx;
  int num_outstanding;
  pipe_mgr_stat_tbl_sync_cback_fn user_cb;
  void *user_cookie;
};
void pipe_mgr_stat_tbl_multi_instance_sync_cb(bf_dev_id_t dev_id,
                                              void *cookie) {
  if (!cookie) return;
  struct pipe_mgr_stat_multi_sync_state_t *state = cookie;
  bool all_syncs_complete = false;
  PIPE_MGR_LOCK(&state->mtx);
  --state->num_outstanding;
  all_syncs_complete = !state->num_outstanding;
  PIPE_MGR_UNLOCK(&state->mtx);

  if (all_syncs_complete) {
    state->user_cb(dev_id, state->user_cookie);
    PIPE_MGR_LOCK_DESTROY(&state->mtx);
    PIPE_MGR_FREE(state);
  }
}

pipe_status_t pipe_mgr_stat_tbl_database_sync(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_stat_tbl_hdl_t stat_tbl_hdl,
    pipe_mgr_stat_tbl_sync_cback_fn cback_fn,
    void *cookie) {
  pipe_status_t status = PIPE_SUCCESS;
  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance = NULL;

  /* If the device is locked, do not invoke the callback and
   * return PIPE_DEVICE_LOCKED.  */
  if (pipe_mgr_is_device_locked(dev_tgt.device_id)) {
    LOG_TRACE(
        "%s:%d Device id %d is locked", __func__, __LINE__, dev_tgt.device_id);
    return PIPE_DEVICE_LOCKED;
  }

  stat_tbl = pipe_mgr_stat_tbl_get(dev_tgt.device_id, stat_tbl_hdl);
  if (stat_tbl == NULL) {
    LOG_ERROR("%s:%d Stat tbl for device id %d, tbl hdl 0x%x not found",
              __func__,
              __LINE__,
              dev_tgt.device_id,
              stat_tbl_hdl);
    return PIPE_OBJ_NOT_FOUND;
  }

  if (dev_tgt.dev_pipe_id == BF_DEV_PIPE_ALL) {
    struct pipe_mgr_stat_multi_sync_state_t *state = NULL;
    if (cback_fn) {
      state = PIPE_MGR_CALLOC(1, sizeof *state);
      if (!state) return PIPE_NO_SYS_RESOURCES;
      PIPE_MGR_LOCK_INIT(state->mtx);
      state->user_cb = cback_fn;
      state->user_cookie = cookie;
      state->num_outstanding = stat_tbl->num_instances;
    }
    int failures = 0;

    pipe_status_t rc = PIPE_SUCCESS;
    for (uint32_t pipe_idx = 0; pipe_idx < stat_tbl->num_instances;
         pipe_idx++) {
      stat_tbl_instance = &stat_tbl->stat_tbl_instances[pipe_idx];

      dev_tgt.dev_pipe_id = stat_tbl_instance->pipe_id;
      status = pipe_mgr_stat_tbl_instance_database_sync(
          sess_hdl,
          dev_tgt,
          stat_tbl,
          stat_tbl_instance,
          state ? pipe_mgr_stat_tbl_multi_instance_sync_cb : NULL,
          state);
      if (status != PIPE_SUCCESS) {
        LOG_ERROR(
            "%s:%d Stat table sync failed for dev %d pipe %d table %s 0x%x "
            "instance-pipe %d err %s",
            __func__,
            __LINE__,
            dev_tgt.device_id,
            dev_tgt.dev_pipe_id,
            stat_tbl->name,
            stat_tbl->stat_tbl_hdl,
            stat_tbl_instance->pipe_id,
            pipe_str_err(status));
        ++failures;
        if (rc == PIPE_SUCCESS) rc = status;
      }
    }
    if (failures && state) {
      /* We have failed to sync some (or all) instances but it may be possible
       * that the syncs for some instances have begun.  Adjust the count of
       * outstanding syncs so the state can be cleaned up. */
      bool done = false;
      PIPE_MGR_LOCK(&state->mtx);
      state->num_outstanding -= failures;
      done = !state->num_outstanding;
      PIPE_MGR_UNLOCK(&state->mtx);
      if (done) {
        /* All instances failed, clean up the state now since no CBs will be
         * executed. */
        PIPE_MGR_LOCK_DESTROY(&state->mtx);
        PIPE_MGR_FREE(state);
      }
    }
    return rc;
  } else {
    stat_tbl_instance =
        pipe_mgr_stat_tbl_get_instance(stat_tbl, dev_tgt.dev_pipe_id);
    if (stat_tbl_instance == NULL) {
      LOG_ERROR(
          "%s:%d Stat table instance for tbl %s 0x%x, device %d, pipe %d"
          " not found",
          __func__,
          __LINE__,
          stat_tbl->name,
          stat_tbl_hdl,
          dev_tgt.device_id,
          dev_tgt.dev_pipe_id);
      return PIPE_OBJ_NOT_FOUND;
    }

    status = pipe_mgr_stat_tbl_instance_database_sync(
        sess_hdl, dev_tgt, stat_tbl, stat_tbl_instance, cback_fn, cookie);
  }

  return status;
}

pipe_status_t pipe_mgr_stat_tbl_log_database_sync(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t dev_id,
    pipe_stat_tbl_hdl_t stat_tbl_hdl) {
  pipe_status_t status = PIPE_SUCCESS;
  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance = NULL;
  dev_target_t dev_tgt;
  uint32_t pipe_idx = 0;

  dev_tgt.device_id = dev_id;
  stat_tbl = pipe_mgr_stat_tbl_get(dev_id, stat_tbl_hdl);

  if (stat_tbl == NULL) {
    LOG_ERROR("%s:%d Stat tbl for device id %d, tbl hdl %d not found",
              __func__,
              __LINE__,
              dev_tgt.device_id,
              stat_tbl_hdl);
    return PIPE_OBJ_NOT_FOUND;
  }

  for (pipe_idx = 0; pipe_idx < stat_tbl->num_instances; pipe_idx++) {
    stat_tbl_instance = &stat_tbl->stat_tbl_instances[pipe_idx];
    dev_tgt.dev_pipe_id = stat_tbl_instance->pipe_id;

    status = pipe_mgr_stat_tbl_instance_database_sync(
        sess_hdl, dev_tgt, stat_tbl, stat_tbl_instance, NULL, NULL);
    if (status != PIPE_SUCCESS) {
      LOG_ERROR(
          "%s:%d Stat table sync failed for pipe %d table 0x%x device %d "
          "err %s",
          __func__,
          __LINE__,
          stat_tbl_instance->pipe_id,
          stat_tbl->stat_tbl_hdl,
          dev_id,
          pipe_str_err(status));
      return status;
    }
  }

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stat_mgr_ent_set(pipe_sess_hdl_t sess_hdl,
                                        dev_target_t dev_tgt,
                                        pipe_stat_tbl_hdl_t stat_tbl_hdl,
                                        pipe_stat_ent_idx_t stat_ent_idx,
                                        pipe_stat_data_t *stat_data) {
  pipe_status_t status = PIPE_SUCCESS;
  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance = NULL;
  pipe_mgr_stat_ent_worklist_t *worklist = NULL;
  pipe_mgr_stat_ent_worklist_t *traverser = NULL;
  pipe_mgr_stat_ent_worklist_t *worklist_item = NULL;
  pipe_stat_stage_ent_idx_t stage_ent_idx = 0;
  bf_dev_pipe_t pipe = 0;
  uint8_t stage_id = 0;
  pipe_stat_data_t local_stat_data = {0};
  pipe_bitmap_t pipe_bmp;
  PIPE_BITMAP_INIT(&pipe_bmp, PIPE_BMP_SIZE);

  stat_tbl = pipe_mgr_stat_tbl_get(dev_tgt.device_id, stat_tbl_hdl);

  if (stat_tbl == NULL) {
    LOG_ERROR("%s:%d Stat tbl for device id %d, tbl hdl %#x not found",
              __func__,
              __LINE__,
              dev_tgt.device_id,
              stat_tbl_hdl);
    return PIPE_OBJ_NOT_FOUND;
  }

  stat_tbl_instance =
      pipe_mgr_stat_tbl_get_instance(stat_tbl, dev_tgt.dev_pipe_id);

  if (stat_tbl_instance == NULL) {
    LOG_ERROR(
        "%s:%d Stat tbl instance not found, pipe id %d, tbl hdl %#x"
        " device id %d",
        __func__,
        __LINE__,
        dev_tgt.dev_pipe_id,
        stat_tbl_hdl,
        dev_tgt.device_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  if (stat_ent_idx >= stat_tbl_instance->num_entries) {
    LOG_ERROR(
        "Dev %d stat table 0x%x (%s) cannot set index %u, table only has %u "
        "entries",
        stat_tbl->device_id,
        stat_tbl_hdl,
        stat_tbl->name,
        stat_ent_idx,
        stat_tbl_instance->num_entries);
    return PIPE_INVALID_ARG;
  }

  PIPE_BITMAP_ASSIGN(&pipe_bmp, &(stat_tbl_instance->pipe_bmp));

  /* Reset the entry in hardware if the device is not locked */
  if (!pipe_mgr_fast_recfg_warm_init_in_progress(dev_tgt.device_id) &&
      !pipe_mgr_hitless_warm_init_in_progress(dev_tgt.device_id)) {
    stage_id = pipe_mgr_stat_mgr_ent_get_stage(
        stat_tbl, stat_tbl_instance, stat_ent_idx, 0xff, &stage_ent_idx);

    while (stage_id != 0xff) {
      worklist_item = PIPE_MGR_CALLOC(1, sizeof(pipe_mgr_stat_ent_worklist_t));

      if (worklist_item == NULL) {
        LOG_ERROR("%s/%d: Malloc failure", __func__, __LINE__);
        return PIPE_NO_SYS_RESOURCES;
      }

      worklist_item->stage_id = stage_id;
      worklist_item->entry_idx = stage_ent_idx;

      BF_LIST_DLL_PP(worklist, worklist_item, next, prev);

      stage_id = pipe_mgr_stat_mgr_ent_get_stage(
          stat_tbl, stat_tbl_instance, stat_ent_idx, stage_id, &stage_ent_idx);
    }

    PIPE_BITMAP_ITER(&pipe_bmp, pipe) {
      traverser = worklist;
      while (traverser) {
        status = pipe_mgr_stat_mgr_set_ent_idx_count(stat_tbl,
                                                     stat_tbl_instance,
                                                     true,
                                                     pipe,
                                                     traverser->stage_id,
                                                     traverser->entry_idx,
                                                     &local_stat_data);
        if (status != PIPE_SUCCESS) {
          LOG_ERROR(
              "%s:%d Failed to set entry index %d, pipe id %d, tbl hdl %#x"
              " device id %d",
              __func__,
              __LINE__,
              traverser->entry_idx,
              dev_tgt.dev_pipe_id,
              stat_tbl_hdl,
              dev_tgt.device_id);
          return status;
        }
        traverser = traverser->next;
      }
    }

    /* Now invoke the driver workflow routine to write the counter value of 0
     * to hardware since the software copy has been set to the passed in value.
     */
    status = pipe_mgr_stat_mgr_ent_write_drv_workflow(sess_hdl,
                                                      dev_tgt.dev_pipe_id,
                                                      stat_tbl,
                                                      stat_tbl_instance,
                                                      worklist,
                                                      &local_stat_data);

    if (status != PIPE_SUCCESS) {
      LOG_ERROR(
          "%s:%d : Error in executing stat entry set for tbl %#x"
          " entry idx %d, device id %d, err %s",
          __func__,
          __LINE__,
          stat_tbl_hdl,
          stat_ent_idx,
          dev_tgt.device_id,
          pipe_str_err(status));
    }

    if (worklist) {
      pipe_mgr_stat_mgr_ent_free_worklist(worklist);
    }
  }

  /* Cache the user set count */
  stat_tbl_instance->user_idx_count[stat_ent_idx].packets = stat_data->packets;
  stat_tbl_instance->user_idx_count[stat_ent_idx].bytes = stat_data->bytes;

  if (stat_mgr_enable_detail_trace) {
    PIPE_MGR_STAT_TRACE(dev_tgt.device_id,
                        stat_tbl_hdl,
                        stat_tbl->name,
                        stat_ent_idx,
                        -1,
                        dev_tgt.dev_pipe_id,
                        -1,
                        "Request to write stat data : packet count %" PRIu64
                        " byte count %" PRIu64,
                        stat_data->packets,
                        stat_data->bytes);
  }

  return status;
}

static pipe_status_t stat_mgr_tbl_lock_helper(dev_target_t dev_tgt,
                                              pipe_stat_tbl_hdl_t stat_tbl_hdl,
                                              dev_stage_t stage_id,
                                              lock_id_t lock_id,
                                              bool lock) {
  pipe_status_t status = PIPE_SUCCESS;
  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance = NULL;
  pipe_mgr_stat_tbl_stage_info_t *stat_tbl_stage_info = NULL;

  stat_tbl = pipe_mgr_stat_tbl_get(dev_tgt.device_id, stat_tbl_hdl);
  if (stat_tbl == NULL) {
    LOG_ERROR("%s:%d Stat table 0x%x, device id %d not found",
              __func__,
              __LINE__,
              stat_tbl_hdl,
              dev_tgt.device_id);
    PIPE_MGR_DBGCHK(stat_tbl);
    return PIPE_UNEXPECTED;
  }

  stat_tbl_instance =
      pipe_mgr_stat_tbl_get_instance(stat_tbl, dev_tgt.dev_pipe_id);
  if (stat_tbl_instance == NULL) {
    LOG_ERROR(
        "%s:%d Stat tbl instance not found, pipe id %d, tbl hdl %#x"
        " device id %d",
        __func__,
        __LINE__,
        dev_tgt.dev_pipe_id,
        stat_tbl_hdl,
        dev_tgt.device_id);
    PIPE_MGR_DBGCHK(stat_tbl_instance);
    return PIPE_UNEXPECTED;
  }

  stat_tbl_stage_info =
      pipe_mgr_stat_mgr_get_stage_info(stat_tbl, dev_tgt.dev_pipe_id, stage_id);
  if (stat_tbl_stage_info == NULL) {
    LOG_ERROR(
        "%s:%d Stat table stage info for tbl 0x%x, pipe id %d,"
        " stage_id %d not found",
        __func__,
        __LINE__,
        stat_tbl_hdl,
        dev_tgt.dev_pipe_id,
        stage_id);
    PIPE_MGR_DBGCHK(stat_tbl_stage_info);
    return PIPE_UNEXPECTED;
  }

  if (stat_tbl_stage_info->locked == lock) {
    LOG_ERROR("%s:%d Stat table 0x%x, pipe id %d, stage id %d already %s",
              __func__,
              __LINE__,
              stat_tbl_hdl,
              dev_tgt.dev_pipe_id,
              stage_id,
              lock ? "locked" : "unlocked");
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }
  if (!lock && stat_tbl_stage_info->lock_id != lock_id) {
    LOG_ERROR(
        "%s:%d Invalid lock id %d passed for unlock for tbl 0x%x"
        " device id %d, pipe id %d, stage id %d, expected %d",
        __func__,
        __LINE__,
        lock_id,
        stat_tbl_hdl,
        dev_tgt.device_id,
        dev_tgt.dev_pipe_id,
        stage_id,
        stat_tbl_stage_info->lock_id);
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }

  if (lock)
    status = pipe_mgr_stat_mgr_lock_stage(
        stat_tbl, stat_tbl_instance, stage_id, lock_id);
  else
    status = pipe_mgr_stat_mgr_unlock_stage(
        stat_tbl, stat_tbl_instance, stage_id, lock_id);
  if (status != PIPE_SUCCESS) {
    LOG_ERROR(
        "%s:%d Error in %s stat table 0x%x, pipe id %d, stage id %d err %s",
        __func__,
        __LINE__,
        lock ? "locking" : "unlocking",
        stat_tbl_hdl,
        dev_tgt.dev_pipe_id,
        stage_id,
        pipe_str_err(status));
    return status;
  }

  /* Update the lock state in the stage. */
  stat_tbl_stage_info->locked = lock;
  stat_tbl_stage_info->lock_id = lock ? lock_id : PIPE_MGR_INVALID_LOCK_ID;

  return PIPE_SUCCESS;
}

pipe_status_t rmt_stat_mgr_tbl_lock(dev_target_t dev_tgt,
                                    pipe_stat_tbl_hdl_t stat_tbl_hdl,
                                    dev_stage_t stage_id,
                                    lock_id_t lock_id) {
  return stat_mgr_tbl_lock_helper(
      dev_tgt, stat_tbl_hdl, stage_id, lock_id, true);
}
pipe_status_t rmt_stat_mgr_tbl_unlock(dev_target_t dev_tgt,
                                      pipe_stat_tbl_hdl_t stat_tbl_hdl,
                                      dev_stage_t stage_id,
                                      lock_id_t lock_id) {
  return stat_mgr_tbl_lock_helper(
      dev_tgt, stat_tbl_hdl, stage_id, lock_id, false);
}

pipe_status_t pipe_mgr_stat_mgr_ent_load(pipe_sess_hdl_t sess_hdl,
                                         dev_target_t dev_tgt,
                                         pipe_stat_tbl_hdl_t stat_tbl_hdl,
                                         pipe_stat_ent_idx_t stat_ent_idx,
                                         pipe_stat_data_t *stat_data) {
  pipe_status_t status = PIPE_SUCCESS;
  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance = NULL;
  pipe_mgr_stat_ent_worklist_t *worklist = NULL;
  pipe_mgr_stat_ent_worklist_t *worklist_item = NULL;
  pipe_mgr_stat_ent_worklist_t *traverser = NULL;
  pipe_stat_stage_ent_idx_t stage_ent_idx = 0;
  bf_dev_pipe_t pipe_id = 0;
  bf_dev_pipe_t pipe = 0;
  uint8_t stage_id = 0;
  pipe_stat_data_t local_stat_data = {0};
  pipe_bitmap_t pipe_bmp;
  PIPE_BITMAP_INIT(&pipe_bmp, PIPE_BMP_SIZE);

  /* If the device is locked, simply return */
  if (pipe_mgr_fast_recfg_warm_init_in_progress(dev_tgt.device_id)) {
    return PIPE_SUCCESS;
  }

  stat_tbl = pipe_mgr_stat_tbl_get(dev_tgt.device_id, stat_tbl_hdl);

  if (stat_tbl == NULL) {
    LOG_ERROR("%s:%d Stat tbl for device id %d, tbl hdl %d not found",
              __func__,
              __LINE__,
              dev_tgt.device_id,
              stat_tbl_hdl);
    return PIPE_OBJ_NOT_FOUND;
  }

  stat_tbl_instance =
      pipe_mgr_stat_tbl_get_instance(stat_tbl, dev_tgt.dev_pipe_id);

  if (stat_tbl_instance == NULL) {
    LOG_ERROR(
        "%s:%d Stat tbl instance not found, pipe id %d, tbl hdl %#x"
        " device id %d",
        __func__,
        __LINE__,
        dev_tgt.dev_pipe_id,
        stat_tbl_hdl,
        dev_tgt.device_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  PIPE_BITMAP_ASSIGN(&pipe_bmp, &(stat_tbl_instance->pipe_bmp));

  pipe_id = dev_tgt.dev_pipe_id;

  stage_id = pipe_mgr_stat_mgr_ent_get_stage(
      stat_tbl, stat_tbl_instance, stat_ent_idx, 0xff, &stage_ent_idx);

  while (stage_id != 0xff) {
    worklist_item = PIPE_MGR_CALLOC(1, sizeof(pipe_mgr_stat_ent_worklist_t));

    if (worklist_item == NULL) {
      LOG_ERROR("%s/%d: Malloc failure", __func__, __LINE__);
      return PIPE_NO_SYS_RESOURCES;
    }

    worklist_item->stage_id = stage_id;
    worklist_item->entry_idx = stage_ent_idx;

    BF_LIST_DLL_PP(worklist, worklist_item, next, prev);

    stage_id = pipe_mgr_stat_mgr_ent_get_stage(
        stat_tbl, stat_tbl_instance, stat_ent_idx, stage_id, &stage_ent_idx);
  }

  PIPE_BITMAP_ITER(&pipe_bmp, pipe) {
    traverser = worklist;
    while (traverser) {
      /* Set the software copy of the counters to the Zero since we are
       * loading the counter in hardware.
       */
      status = pipe_mgr_stat_mgr_set_ent_idx_count(stat_tbl,
                                                   stat_tbl_instance,
                                                   true,
                                                   pipe_id,
                                                   traverser->stage_id,
                                                   traverser->entry_idx,
                                                   &local_stat_data);
      if (status != PIPE_SUCCESS) {
        PIPE_MGR_DBGCHK(0);
        return status;
      }
      traverser = traverser->next;
    }
  }
  /* Now invoke the driver workflow routine to write the counter value of 0
   * to hardware since the software copy has been set to the passed in value.
   */
  status = pipe_mgr_stat_mgr_ent_write_drv_workflow(sess_hdl,
                                                    dev_tgt.dev_pipe_id,
                                                    stat_tbl,
                                                    stat_tbl_instance,
                                                    worklist,
                                                    stat_data);

  if (status != PIPE_SUCCESS) {
    LOG_ERROR(
        "%s:%d : Error in executing stat entry set for tbl %#x"
        " entry idx %d, device id %d, err %s",
        __func__,
        __LINE__,
        stat_tbl_hdl,
        stat_ent_idx,
        dev_tgt.device_id,
        pipe_str_err(status));
  }

  if (worklist) {
    pipe_mgr_stat_mgr_ent_free_worklist(worklist);
  }

  if (stat_mgr_enable_detail_trace) {
    PIPE_MGR_STAT_TRACE(dev_tgt.device_id,
                        stat_tbl_hdl,
                        stat_tbl->name,
                        stat_ent_idx,
                        -1,
                        dev_tgt.dev_pipe_id,
                        -1,
                        "Request to write stat data : packet count 0x%" PRIx64
                        " byte count 0x%" PRIx64,
                        stat_data->packets,
                        stat_data->bytes);
  }

  return status;
}

pipe_status_t pipe_mgr_stat_mgr_add_entry(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    pipe_stat_tbl_hdl_t stat_tbl_hdl,
    dev_stage_t stage_id,
    pipe_stat_stage_ent_idx_t stage_ent_idx,
    bool hw_init,
    pipe_stat_data_t *stat_data,
    rmt_virt_addr_t *stat_ent_virt_addr) {
  pipe_status_t status = PIPE_SUCCESS;
  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance = NULL;
  pipe_mgr_stat_tbl_stage_info_t *stat_tbl_stage_info = NULL;
  pipe_mgr_stat_ent_worklist_t worklist_item;
  pipe_stat_data_t local_stat_data = {0};

  stat_tbl = pipe_mgr_stat_tbl_get(dev_tgt.device_id, stat_tbl_hdl);
  if (stat_tbl == NULL) {
    return PIPE_OBJ_NOT_FOUND;
  }

  stat_tbl_instance =
      pipe_mgr_stat_tbl_get_instance(stat_tbl, dev_tgt.dev_pipe_id);
  if (stat_tbl_instance == NULL) {
    return PIPE_OBJ_NOT_FOUND;
  }

  stat_tbl_stage_info =
      pipe_mgr_stat_mgr_get_stage_info(stat_tbl, dev_tgt.dev_pipe_id, stage_id);
  if (stat_tbl_stage_info == NULL) {
    return PIPE_OBJ_NOT_FOUND;
  }

  /* Validate the stage index. */
  if (NULL ==
      pipe_mgr_stat_mgr_get_ent_idx_info(stat_tbl,
                                         stat_tbl_instance,
                                         stat_tbl_instance->lowest_pipe_id,
                                         stage_id,
                                         stage_ent_idx)) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }

  /* Generally we initialize stats for a new entry by writing them to zero and
   * sending a barrier to let us know when the write has completed.  However,
   * a move-reg operation (move existing entry, add new entry) will initialize
   * the stats entry for us in which case the caller may set hw_init to false
   * to inform us that the explicit init is not required. */
  if (!pipe_mgr_is_device_locked(dev_tgt.device_id) && hw_init) {
    /* First, clean the hardware counter value to 0 */
    PIPE_MGR_MEMSET(&worklist_item, 0, sizeof(pipe_mgr_stat_ent_worklist_t));
    worklist_item.stage_id = stage_id;
    worklist_item.entry_idx = stage_ent_idx;
    /* Now invoke the driver workflow routine to write the counter value of 0
     * to hardware since the software copy has been set to the passed in value.
     */
    status = pipe_mgr_stat_mgr_ent_write_drv_workflow(sess_hdl,
                                                      dev_tgt.dev_pipe_id,
                                                      stat_tbl,
                                                      stat_tbl_instance,
                                                      &worklist_item,
                                                      &local_stat_data);

    if (status != PIPE_SUCCESS) {
      return status;
    }
  }

  status = pipe_mgr_stat_mgr_process_entry_add(stat_tbl,
                                               stat_tbl_instance,
                                               mat_ent_hdl,
                                               stage_id,
                                               stage_ent_idx,
                                               stat_data);

  if (status != PIPE_SUCCESS) {
    pipe_stat_ent_idx_t stat_ent_idx;
    stat_ent_idx = stage_ent_idx + stat_tbl_stage_info->ent_idx_offset;
    LOG_ERROR(
        "%s:%d Error in processing stat entry add for tbl 0x%x"
        " idx 0x%x, mat ent hdl 0x%x, err %s",
        __func__,
        __LINE__,
        stat_tbl_hdl,
        stat_ent_idx,
        mat_ent_hdl,
        pipe_str_err(status));
    return status;
  }

  /* Form this entry's virtual address */
  if (stat_ent_virt_addr) {
    *stat_ent_virt_addr = pipe_mgr_stat_mgr_compute_ent_virt_addr(
        stat_tbl_stage_info, stage_ent_idx);
  }

  return status;
}

static pipe_status_t pipe_mgr_stat_mgr_activate_new_entry(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    bf_dev_pipe_t pipe_id,
    dev_stage_t stage_id,
    pipe_stat_stage_ent_idx_t stage_ent_idx) {
  pipe_status_t status = PIPE_SUCCESS;
  status = pipe_mgr_stat_mgr_clear_entry_pending(stat_tbl,
                                                 stat_tbl_instance,
                                                 pipe_id,
                                                 mat_ent_hdl,
                                                 stage_id,
                                                 stage_ent_idx);
  if (status != PIPE_SUCCESS) {
    LOG_ERROR(
        "%s:%d Error in updating entry handle location for ent hdl"
        " 0x%x, for stat tbl 0x%x, pipe id %d",
        __func__,
        __LINE__,
        mat_ent_hdl,
        stat_tbl->stat_tbl_hdl,
        stat_tbl_instance->pipe_id);
    PIPE_MGR_STAT_DBGCHK(stat_tbl_instance, 0);
    return status;
  }

  return status;
}

pipe_status_t pipe_mgr_stat_mgr_delete_entry(
    dev_target_t dev_tgt,
    dev_stage_t stage_id,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    pipe_stat_stage_ent_idx_t stage_ent_idx,
    pipe_stat_tbl_hdl_t stat_tbl_hdl) {
  pipe_status_t status = PIPE_SUCCESS;
  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance = NULL;
  pipe_mgr_stat_tbl_stage_info_t *stat_tbl_stage_info = NULL;

  stat_tbl = pipe_mgr_stat_tbl_get(dev_tgt.device_id, stat_tbl_hdl);
  if (stat_tbl == NULL) {
    LOG_ERROR("%s:%d Stat table 0x%x, device id %d not found",
              __func__,
              __LINE__,
              stat_tbl_hdl,
              dev_tgt.device_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  if (stat_tbl->ref_type != PIPE_TBL_REF_TYPE_DIRECT) {
    LOG_ERROR(
        "%s:%d Stat entry delete called on an indirectly referenced table"
        " 0x%x, device id %d",
        __func__,
        __LINE__,
        stat_tbl_hdl,
        dev_tgt.device_id);
    PIPE_MGR_DBGCHK(stat_tbl->ref_type == PIPE_TBL_REF_TYPE_DIRECT);
    return PIPE_INVALID_ARG;
  }

  stat_tbl_instance =
      pipe_mgr_stat_tbl_get_instance(stat_tbl, dev_tgt.dev_pipe_id);
  if (stat_tbl_instance == NULL) {
    LOG_ERROR(
        "%s:%d Stat table instance for table 0x%x, device id %d, pipe id %d"
        " not found",
        __func__,
        __LINE__,
        stat_tbl_hdl,
        dev_tgt.device_id,
        dev_tgt.dev_pipe_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  stat_tbl_stage_info =
      pipe_mgr_stat_mgr_get_stage_info(stat_tbl, dev_tgt.dev_pipe_id, stage_id);
  if (stat_tbl_stage_info == NULL) {
    LOG_ERROR(
        "%s:%d Stat table stage info for table 0x%x, device id %d, pipe id %d"
        " stage id %d not found",
        __func__,
        __LINE__,
        stat_tbl_hdl,
        dev_tgt.device_id,
        dev_tgt.dev_pipe_id,
        stage_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  /* Validate the stage index. */
  if (NULL ==
      pipe_mgr_stat_mgr_get_ent_idx_info(stat_tbl,
                                         stat_tbl_instance,
                                         stat_tbl_instance->lowest_pipe_id,
                                         stage_id,
                                         stage_ent_idx)) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }

  if (stat_tbl->lrt_enabled) {
    if (stat_tbl_stage_info->locked == false) {
      LOG_ERROR(
          "%s:%d Attempt to delete the stat entry without locking"
          " for table 0x%x, device id %d, pipe id %d, stage id %d"
          " match entry hdl 0x%x",
          __func__,
          __LINE__,
          stat_tbl_hdl,
          dev_tgt.device_id,
          dev_tgt.dev_pipe_id,
          stage_id,
          mat_ent_hdl);
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
    }
  }

  status = pipe_mgr_stat_mgr_process_entry_del(
      stat_tbl, stat_tbl_instance, mat_ent_hdl, stage_id, stage_ent_idx);
  if (status != PIPE_SUCCESS) {
    LOG_ERROR(
        "%s:%d Error in processing entry delete for entry hdl 0x%x"
        " table 0x%x, err %s",
        __func__,
        __LINE__,
        mat_ent_hdl,
        stat_tbl->stat_tbl_hdl,
        pipe_str_err(status));
    return status;
  }

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stat_mgr_direct_ent_set(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    pipe_stat_tbl_hdl_t stat_tbl_hdl,
    dev_stage_t stage_id,
    pipe_stat_stage_ent_idx_t stage_ent_idx,
    pipe_stat_data_t *stat_data) {
  pipe_status_t status = PIPE_SUCCESS;
  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance = NULL;
  pipe_mgr_stat_tbl_stage_info_t *stat_tbl_stage_info = NULL;
  pipe_mgr_stat_ent_worklist_t worklist_item;
  pipe_stat_data_t local_stat_data = {0};

  stat_tbl = pipe_mgr_stat_tbl_get(dev_tgt.device_id, stat_tbl_hdl);
  if (stat_tbl == NULL) {
    return PIPE_OBJ_NOT_FOUND;
  }

  stat_tbl_instance =
      pipe_mgr_stat_tbl_get_instance(stat_tbl, dev_tgt.dev_pipe_id);
  if (stat_tbl_instance == NULL) {
    return PIPE_OBJ_NOT_FOUND;
  }

  if (stat_tbl->ref_type != PIPE_TBL_REF_TYPE_DIRECT) {
    /* Not applicable for an indirectly referenced table */
    LOG_ERROR(
        "%s:%d Direct stat counter set API called on an indirectly"
        " referenced stat tabl 0x%x, device id %d",
        __func__,
        __LINE__,
        stat_tbl_hdl,
        dev_tgt.device_id);
    PIPE_MGR_DBGCHK(stat_tbl->ref_type == PIPE_TBL_REF_TYPE_DIRECT);
    return PIPE_INVALID_ARG;
  }

  stat_tbl_stage_info =
      pipe_mgr_stat_mgr_get_stage_info(stat_tbl, dev_tgt.dev_pipe_id, stage_id);
  if (stat_tbl_stage_info == NULL) {
    return PIPE_OBJ_NOT_FOUND;
  }

  bool write_issued = false;

  /* First write the entry to zero and post a barrier so we know when the write
   * happened compared to any evicts. */
  if (!pipe_mgr_fast_recfg_warm_init_in_progress(dev_tgt.device_id) &&
      !pipe_mgr_hitless_warm_init_in_progress(dev_tgt.device_id)) {
    PIPE_MGR_MEMSET(&worklist_item, 0, sizeof worklist_item);
    worklist_item.stage_id = stage_id;
    worklist_item.entry_idx = stage_ent_idx;
    status = pipe_mgr_stat_mgr_ent_write_drv_workflow(sess_hdl,
                                                      dev_tgt.dev_pipe_id,
                                                      stat_tbl,
                                                      stat_tbl_instance,
                                                      &worklist_item,
                                                      &local_stat_data);

    if (status != PIPE_SUCCESS) {
      return status;
    }
    write_issued = true;
  }

  /* Next, save the user-set count against the entry handle and mark the entry's
   * deferred location as set-in-progress.  This will be cleared when the
   * barrier-ack from the write (just above) completes. */
  status = pipe_mgr_stat_mgr_update_ent_hdl_count(
      stat_tbl, stat_tbl_instance, mat_ent_hdl, stat_data, write_issued);
  if (status == PIPE_OBJ_NOT_FOUND) {
    LOG_ERROR("%s:%d Entry handl 0x%x for stat tbl 0x%x not found, pipe id %d",
              __func__,
              __LINE__,
              mat_ent_hdl,
              stat_tbl_hdl,
              stat_tbl_instance->pipe_id);
    return status;
  }

  return status;
}

pipe_status_t pipe_mgr_stat_mgr_stage_ent_load(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_stat_tbl_hdl_t stat_tbl_hdl,
    uint8_t stage_id,
    pipe_stat_stage_ent_idx_t stage_ent_idx,
    pipe_stat_data_t *stat_data) {
  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  pipe_mgr_stat_tbl_stage_info_t *stat_tbl_stage_info = NULL;
  pipe_stat_ent_idx_t stat_ent_idx;

  stat_tbl = pipe_mgr_stat_tbl_get(dev_tgt.device_id, stat_tbl_hdl);

  if (stat_tbl == NULL) {
    return PIPE_OBJ_NOT_FOUND;
  }

  stat_tbl_stage_info =
      pipe_mgr_stat_mgr_get_stage_info(stat_tbl, dev_tgt.dev_pipe_id, stage_id);

  if (stat_tbl_stage_info == NULL) {
    return PIPE_OBJ_NOT_FOUND;
  }

  stat_ent_idx = stage_ent_idx + stat_tbl_stage_info->ent_idx_offset;

  return pipe_mgr_stat_mgr_ent_load(
      sess_hdl, dev_tgt, stat_tbl_hdl, stat_ent_idx, stat_data);
}

/* Syncs either one index in an indirectly referenced table or one entry in a
 * directly referenced table. */
pipe_status_t pipe_mgr_stat_mgr_stat_ent_database_sync(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    pipe_stat_tbl_hdl_t stat_tbl_hdl,
    pipe_stat_ent_idx_t stat_ent_idx) {
  pipe_status_t status = PIPE_SUCCESS;
  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance = NULL;
  pipe_mgr_stat_ent_worklist_t *worklist = NULL;
  pipe_mgr_stat_ent_worklist_t *worklist_item = NULL;
  pipe_stat_stage_ent_idx_t stage_ent_idx = 0;
  uint8_t stage_id = 0;

  /* If the device is locked, return SUCCESS.  */
  if (pipe_mgr_is_device_locked(dev_tgt.device_id)) {
    return PIPE_SUCCESS;
  }

  stat_tbl = pipe_mgr_stat_tbl_get(dev_tgt.device_id, stat_tbl_hdl);
  if (stat_tbl == NULL) {
    LOG_ERROR("%s : Stat table 0x%x not found, device id %d",
              __func__,
              stat_tbl_hdl,
              dev_tgt.device_id);
    return PIPE_INVALID_ARG;
  }

  stat_tbl_instance =
      pipe_mgr_stat_tbl_get_instance(stat_tbl, dev_tgt.dev_pipe_id);
  if (stat_tbl_instance == NULL) {
    LOG_ERROR(
        "%s : Error in getting stat table instance for table 0x%x, device id %d"
        " pipe id %x",
        __func__,
        stat_tbl_hdl,
        dev_tgt.device_id,
        dev_tgt.dev_pipe_id);
    return PIPE_INVALID_ARG;
  }

  if (stat_ent_idx >= stat_tbl_instance->num_entries) {
    LOG_ERROR(
        "Dev %d stat table 0x%x (%s) cannot sync index %u, table only has %u "
        "entries",
        stat_tbl->device_id,
        stat_tbl_hdl,
        stat_tbl->name,
        stat_ent_idx,
        stat_tbl_instance->num_entries);
    return PIPE_INVALID_ARG;
  }

  /* This stat entry index will be present in multiple pipes if the instance has
   * more than one pipe.  The entry may be present in multiple stages as well if
   * the table is both indirectly referenced and multi-stage. Issue an
   * entry-dump instruction for each pipe/stage combination.
   * This loop below will create work items for each stage while the workflow
   * function called below will handle each pipe. */
  stage_id = pipe_mgr_stat_mgr_ent_get_stage(
      stat_tbl, stat_tbl_instance, stat_ent_idx, 0xff, &stage_ent_idx);
  while (stage_id != 0xff) {
    worklist_item = PIPE_MGR_CALLOC(1, sizeof(pipe_mgr_stat_ent_worklist_t));

    if (worklist_item == NULL) {
      LOG_ERROR("%s/%d : Malloc failure", __func__, __LINE__);
      return PIPE_NO_SYS_RESOURCES;
    }
    worklist_item->stage_id = stage_id;
    worklist_item->entry_idx = stage_ent_idx;

    BF_LIST_DLL_PP(worklist, worklist_item, next, prev);

    stage_id = pipe_mgr_stat_mgr_ent_get_stage(
        stat_tbl, stat_tbl_instance, stat_ent_idx, stage_id, &stage_ent_idx);
  }

  status = pipe_mgr_stat_mgr_ent_dump_drv_workflow(sess_hdl,
                                                   dev_tgt,
                                                   stat_tbl,
                                                   stat_tbl_instance,
                                                   mat_tbl_hdl,
                                                   mat_ent_hdl,
                                                   stat_ent_idx,
                                                   worklist);

  if (status != PIPE_SUCCESS) {
    LOG_ERROR(
        "%s : Error in executing statistic dump for tbl %d"
        " entry idx %d, device id %d, err %s",
        __func__,
        stat_tbl_hdl,
        stat_ent_idx,
        dev_tgt.device_id,
        pipe_str_err(status));
    goto cleanup;
  }

  if (stat_mgr_enable_detail_trace) {
    PIPE_MGR_STAT_TRACE(dev_tgt.device_id,
                        stat_tbl_hdl,
                        stat_tbl->name,
                        stat_ent_idx,
                        -1,
                        dev_tgt.dev_pipe_id,
                        -1,
                        "Request to dump stat ent idx %d",
                        stat_ent_idx);
  }

cleanup:
  if (worklist) {
    pipe_mgr_stat_mgr_ent_free_worklist(worklist);
  }
  return status;
}

pipe_status_t pipe_mgr_stat_mgr_direct_stat_ent_sync(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t dev_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    pipe_stat_tbl_hdl_t stat_tbl_hdl) {
  pipe_status_t sts = PIPE_SUCCESS;
  int subindex = 0;
  while (PIPE_SUCCESS == sts) {
    bf_dev_pipe_t pipe_id;
    dev_stage_t stage_id;
    uint32_t entry_idx;
    sts = pipe_mgr_mat_ent_get_dir_ent_location(dev_id,
                                                mat_tbl_hdl,
                                                mat_ent_hdl,
                                                subindex,
                                                &pipe_id,
                                                &stage_id,
                                                NULL,
                                                &entry_idx);
    if (sts == PIPE_OBJ_NOT_FOUND && subindex > 0) {
      /* No more locations for this entry. */
      return PIPE_SUCCESS;
    } else if (PIPE_SUCCESS != sts) {
      LOG_ERROR(
          "%s: Dev %d Error getting entry %d subindex %d from MAT 0x%x, %s",
          __func__,
          dev_id,
          mat_ent_hdl,
          subindex,
          mat_tbl_hdl,
          pipe_str_err(sts));
      return sts;
    }

    /* Successfully got the entry's location, sync the entry. */
    sts = rmt_stat_mgr_direct_stat_ent_database_sync(sess_hdl,
                                                     dev_id,
                                                     mat_tbl_hdl,
                                                     mat_ent_hdl,
                                                     stat_tbl_hdl,
                                                     pipe_id,
                                                     stage_id,
                                                     entry_idx);
    if (PIPE_SUCCESS != sts) {
      LOG_ERROR(
          "%s: Dev %d Error syncing entry %d from MAT 0x%x in stats table "
          "0x%x, pipe %x stage %d idx %u, %s",
          __func__,
          dev_id,
          mat_ent_hdl,
          mat_tbl_hdl,
          stat_tbl_hdl,
          pipe_id,
          stage_id,
          entry_idx,
          pipe_str_err(sts));
      return sts;
    }
    ++subindex;
  }
  return sts;
}

pipe_status_t rmt_stat_mgr_direct_stat_ent_database_sync(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t device_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    pipe_stat_tbl_hdl_t stat_tbl_hdl,
    bf_dev_pipe_t pipe_id,
    uint8_t stage_id,
    pipe_stat_stage_ent_idx_t stage_ent_idx) {
  pipe_status_t status = PIPE_SUCCESS;
  dev_target_t dev_tgt;
  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance = NULL;
  pipe_mgr_stat_tbl_stage_info_t *stat_tbl_stage_info = NULL;
  pipe_stat_ent_idx_t stat_ent_idx_offset = 0;
  pipe_stat_ent_idx_t stat_ent_idx = 0;

  /* If the device is locked, straight away return, since
   * we will not be able to issue the dump instruction when the device is locked
   * which will be during the fast reconfig case. Ideally, we should not be
   * getting this API request during the first phase of fast reconfig.
   */
  if (pipe_mgr_is_device_locked(device_id)) {
    return PIPE_SUCCESS;
  }

  /* In this API, we just calculate the absolute stat entry index and just
   * invoke the regular stat entry database sync API. Some work is repetetive,
   * but for the sake of simplicity calling that API instead of repeating a
   * similar implementation as part of this API.
   */

  stat_tbl = pipe_mgr_stat_tbl_get(device_id, stat_tbl_hdl);
  if (stat_tbl == NULL) {
    LOG_ERROR("%s:%d Stat table %d, device id %d not found",
              __func__,
              __LINE__,
              stat_tbl_hdl,
              device_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  stat_tbl_instance = pipe_mgr_stat_tbl_get_instance(stat_tbl, pipe_id);
  if (stat_tbl_instance == NULL) {
    LOG_ERROR(
        "%s:%d Stat table instance for tbl %d, pipe id %d, device id %d"
        " not found",
        __func__,
        __LINE__,
        stat_tbl_hdl,
        pipe_id,
        device_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  stat_tbl_stage_info =
      pipe_mgr_stat_mgr_get_stage_info(stat_tbl, pipe_id, stage_id);

  if (stat_tbl_stage_info == NULL) {
    LOG_ERROR(
        "%s:%d Stat table stage info not found for tbl %d, pipe id %d"
        " stage id %d, device id %d",
        __func__,
        __LINE__,
        stat_tbl_hdl,
        pipe_id,
        stage_id,
        device_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  unsigned stage_idx = 0;

  /* Get the offset that needs to be added to the stage level entry index
   * to get the global entry index.
   */
  for (stage_idx = 0; stage_idx < stat_tbl_instance->num_stages; stage_idx++) {
    stat_tbl_stage_info = &stat_tbl_instance->stat_tbl_stage_info[stage_idx];
    if (stat_tbl_stage_info->stage_id == stage_id) {
      break;
    }
    stat_ent_idx_offset += stat_tbl_stage_info->num_entries;
  }

  stat_ent_idx = stage_ent_idx + stat_ent_idx_offset;

  /* Now, invoke the entry database sync API */

  dev_tgt.device_id = device_id;
  dev_tgt.dev_pipe_id = pipe_id;

  status = pipe_mgr_stat_mgr_stat_ent_database_sync(
      sess_hdl, dev_tgt, mat_tbl_hdl, mat_ent_hdl, stat_tbl_hdl, stat_ent_idx);

  if (stat_mgr_enable_detail_trace) {
    PIPE_MGR_STAT_TRACE(dev_tgt.device_id,
                        stat_tbl_hdl,
                        stat_tbl->name,
                        stat_ent_idx,
                        stage_ent_idx,
                        dev_tgt.dev_pipe_id,
                        -1,
                        "Request to dump direct stat ent idx %d",
                        stat_ent_idx);
  }

  return status;
}

pipe_status_t rmt_stat_mgr_stat_ent_move(
    bf_dev_id_t device_id,
    pipe_stat_tbl_hdl_t stat_tbl_hdl,
    bf_dev_pipe_t pipe_id,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    dev_stage_t src_stage_id,
    dev_stage_t dst_stage_id,
    pipe_stat_stage_ent_idx_t src_ent_idx,
    pipe_stat_stage_ent_idx_t dst_ent_idx) {
  pipe_status_t status = PIPE_SUCCESS;

  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  stat_tbl = pipe_mgr_stat_tbl_get(device_id, stat_tbl_hdl);
  if (stat_tbl == NULL) {
    LOG_ERROR("%s:%d Stat tbl 0x%x, device id %d not found",
              __func__,
              __LINE__,
              stat_tbl_hdl,
              device_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  PIPE_MGR_DBGCHK(stat_tbl->ref_type == PIPE_TBL_REF_TYPE_DIRECT);

  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance = NULL;
  stat_tbl_instance = pipe_mgr_stat_tbl_get_instance(stat_tbl, pipe_id);
  if (stat_tbl_instance == NULL) {
    LOG_ERROR(
        "%s:%d Stat table instance for tbl 0x%x pipe id %d"
        " not found",
        __func__,
        __LINE__,
        stat_tbl->stat_tbl_hdl,
        device_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  /* Validate the source and dest stage indexes. */
  if (NULL ==
      pipe_mgr_stat_mgr_get_ent_idx_info(stat_tbl,
                                         stat_tbl_instance,
                                         stat_tbl_instance->lowest_pipe_id,
                                         src_stage_id,
                                         src_ent_idx)) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }
  if (NULL ==
      pipe_mgr_stat_mgr_get_ent_idx_info(stat_tbl,
                                         stat_tbl_instance,
                                         stat_tbl_instance->lowest_pipe_id,
                                         dst_stage_id,
                                         dst_ent_idx)) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }

  /* If the stat table is LR(t) enabled, then this move has to happen
   * when the table is locked. Check for that.
   */
  if (stat_tbl->lrt_enabled && src_stage_id == dst_stage_id) {
    pipe_mgr_stat_tbl_stage_info_t *stat_tbl_stage_info =
        pipe_mgr_stat_mgr_get_stage_info(stat_tbl, pipe_id, src_stage_id);
    if (stat_tbl_stage_info == NULL) {
      LOG_ERROR(
          "%s:%d Stat table stage info not found for tbl 0x%x, pipe id %d"
          " stage id %d, device id %d",
          __func__,
          __LINE__,
          stat_tbl_hdl,
          pipe_id,
          src_stage_id,
          device_id);
      return PIPE_OBJ_NOT_FOUND;
    }
    if (stat_tbl_stage_info->locked == false) {
      LOG_ERROR(
          "%s:%d Attempt to move stat entry without locking, idx %d"
          " to %d, stage id %d to %d, tbl 0x%x, pipe id %d device id %d",
          __func__,
          __LINE__,
          src_ent_idx,
          dst_ent_idx,
          src_stage_id,
          dst_stage_id,
          stat_tbl_hdl,
          pipe_id,
          device_id);
      return PIPE_NOT_SUPPORTED;
    }
  }

  /* If a barrier ACK is pending, enqueue the move */
  status = pipe_mgr_stat_mgr_process_move(stat_tbl,
                                          stat_tbl_instance,
                                          mat_ent_hdl,
                                          src_stage_id,
                                          dst_stage_id,
                                          src_ent_idx,
                                          dst_ent_idx);

  return status;
}

static pipe_status_t pipe_mgr_stat_mgr_stat_ent_move(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    bf_dev_pipe_t pipe_id,
    dev_stage_t src_stage_id,
    dev_stage_t dst_stage_id,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    pipe_stat_stage_ent_idx_t src_stat_ent_idx,
    pipe_stat_stage_ent_idx_t dst_stat_ent_idx,
    bool inline_processing) {
  pipe_status_t status = PIPE_SUCCESS;

  PIPE_MGR_LOCK(&stat_tbl_instance->ent_hdl_loc_mtx);

  /* Commit entry handle location */
  status = pipe_mgr_stat_mgr_commit_ent_hdl_loc(stat_tbl,
                                                stat_tbl_instance,
                                                pipe_id,
                                                mat_ent_hdl,
                                                src_stage_id,
                                                src_stat_ent_idx,
                                                dst_stage_id,
                                                dst_stat_ent_idx,
                                                inline_processing);

  if (status != PIPE_SUCCESS) {
    LOG_ERROR(
        "%s:%d Error in updating match entry handle location in stats"
        " for table 0x%x, device id %d, pipe id %d, err %s",
        __func__,
        __LINE__,
        stat_tbl->stat_tbl_hdl,
        stat_tbl->device_id,
        pipe_id,
        pipe_str_err(status));
    PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
    PIPE_MGR_STAT_DBGCHK(stat_tbl_instance, 0);
    return status;
  }

  /* Copy the counter info from the source to the dest */
  status = pipe_mgr_stat_mgr_copy_idx_count(stat_tbl,
                                            stat_tbl_instance,
                                            pipe_id,
                                            src_stage_id,
                                            dst_stage_id,
                                            src_stat_ent_idx,
                                            dst_stat_ent_idx);

  if (status != PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Error in copying stat data from idx %d to idx %d",
              __func__,
              __LINE__,
              src_stat_ent_idx,
              dst_stat_ent_idx);
    PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
    return status;
  }
  PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stat_mgr_reset_entry(pipe_sess_hdl_t sess_hdl,
                                            bf_dev_id_t device_id,
                                            bf_dev_pipe_t pipe_id,
                                            pipe_stat_tbl_hdl_t stat_tbl_hdl,
                                            dev_stage_t stage_id,
                                            pipe_stat_stage_ent_idx_t ent_idx) {
  pipe_status_t status = PIPE_SUCCESS;
  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  pipe_mgr_stat_tbl_stage_info_t *stat_tbl_stage_info = NULL;
  pipe_stat_data_t stat_data = {0};

  if (pipe_mgr_is_device_locked(device_id)) {
    return PIPE_SUCCESS;
  }

  stat_tbl = pipe_mgr_stat_tbl_get(device_id, stat_tbl_hdl);
  if (stat_tbl == NULL) {
    LOG_ERROR("%s:%d Stat tbl 0x%x, device id %d not found",
              __func__,
              __LINE__,
              stat_tbl_hdl,
              device_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance;
  stat_tbl_instance = pipe_mgr_stat_tbl_get_instance(stat_tbl, pipe_id);
  if (stat_tbl_instance == NULL) {
    LOG_ERROR("%s:%d Stat table instance for pipe id %d, tbl 0x%x not found",
              __func__,
              __LINE__,
              pipe_id,
              stat_tbl_hdl);
    return PIPE_OBJ_NOT_FOUND;
  }

  stat_tbl_stage_info =
      pipe_mgr_stat_mgr_get_stage_info(stat_tbl, pipe_id, stage_id);

  if (stat_tbl_stage_info == NULL) {
    LOG_ERROR(
        "%s:%d Stat tbl stage info for tbl 0x%x, device id %d, pipe id %d"
        " stage id %d not found",
        __func__,
        __LINE__,
        stat_tbl_hdl,
        device_id,
        pipe_id,
        stage_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  /* Initialize the destination to 0  on for inter-stage moves */
  pipe_mgr_stat_ent_worklist_t worklist = {0};
  worklist.stage_id = stage_id;
  worklist.entry_idx = ent_idx;

  status = pipe_mgr_stat_mgr_ent_write_drv_workflow(
      sess_hdl, pipe_id, stat_tbl, stat_tbl_instance, &worklist, &stat_data);

  if (status != PIPE_SUCCESS) {
    LOG_ERROR(
        "%s:%d Error in resetting stats for entry %d"
        " table %d, device id %d, pipe id %d stage id %d",
        __func__,
        __LINE__,
        ent_idx,
        stat_tbl_hdl,
        device_id,
        pipe_id,
        stage_id);
    return status;
  }

  return status;
}

pipe_mgr_stat_tbl_t *pipe_mgr_stat_tbl_get(bf_dev_id_t device_id,
                                           pipe_stat_tbl_hdl_t stat_tbl_hdl) {
  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  bf_map_sts_t map_sts;

  unsigned long key = 0;

  if (device_id >= PIPE_MGR_NUM_DEVICES) {
    PIPE_MGR_DBGCHK(0);
    return NULL;
  }

  key = stat_tbl_hdl;

  map_sts = pipe_mgr_stat_tbl_map_get(device_id, key, (void **)&stat_tbl);

  if (map_sts != BF_MAP_OK) {
    return NULL;
  }

  return stat_tbl;
}

pipe_mgr_stat_tbl_instance_t *pipe_mgr_stat_tbl_get_instance(
    pipe_mgr_stat_tbl_t *stat_tbl, bf_dev_pipe_t pipe_id) {
  uint8_t pipe_idx = 0;

  for (pipe_idx = 0; pipe_idx < stat_tbl->num_instances; pipe_idx++) {
    if (stat_tbl->stat_tbl_instances[pipe_idx].pipe_id == pipe_id) {
      return &stat_tbl->stat_tbl_instances[pipe_idx];
    }
  }

  return NULL;
}

/* Return the instance which manages the pipe specified. */
pipe_mgr_stat_tbl_instance_t *pipe_mgr_stat_tbl_get_instance_any_pipe(
    pipe_mgr_stat_tbl_t *stat_tbl, bf_dev_pipe_t pipe_id) {
  if (pipe_id >= stat_tbl->dev_info->num_active_pipes) {
    PIPE_MGR_DBGCHK(pipe_id < stat_tbl->dev_info->num_active_pipes);
    return NULL;
  }
  PIPE_MGR_DBGCHK(pipe_id < stat_tbl->dev_info->num_active_pipes);
  for (uint8_t idx = 0; idx < stat_tbl->num_instances; idx++) {
    if (PIPE_BITMAP_GET(&stat_tbl->stat_tbl_instances[idx].pipe_bmp, pipe_id)) {
      return &stat_tbl->stat_tbl_instances[idx];
    }
  }
  return NULL;
}

static pipe_mgr_stat_entry_info_t *pipe_mgr_stat_mgr_get_ent_idx_info(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    bf_dev_pipe_t pipe_id,
    dev_stage_t stage_id,
    pipe_stat_stage_ent_idx_t stage_ent_idx) {
  pipe_mgr_stat_ent_idx_info_t *ent_idx_info = NULL;

  if (!stat_tbl || !stat_tbl_instance) {
    PIPE_MGR_DBGCHK(stat_tbl);
    PIPE_MGR_DBGCHK(stat_tbl_instance);
    return NULL;
  }

  if (stat_tbl->dev_info->num_active_pipes <= pipe_id) {
    LOG_ERROR(
        "Invalid pipe 0x%x, num-pipes %d, dev %d stats table 0x%x (%s) stage "
        "%d index %d",
        pipe_id,
        stat_tbl->dev_info->num_active_pipes,
        stat_tbl->dev_info->dev_id,
        stat_tbl->stat_tbl_hdl,
        stat_tbl->name,
        stage_id,
        stage_ent_idx);
    PIPE_MGR_DBGCHK(pipe_id < stat_tbl->dev_info->num_active_pipes);
    return NULL;
  }
  if (stat_tbl->dev_info->num_active_mau <= stage_id) {
    LOG_ERROR(
        "Invalid stage %d requested, num-stages %d, dev %d stats table 0x%x "
        "(%s) pipe %d index %d",
        stage_id,
        stat_tbl->dev_info->num_active_mau,
        stat_tbl->dev_info->dev_id,
        stat_tbl->stat_tbl_hdl,
        stat_tbl->name,
        pipe_id,
        stage_ent_idx);
    PIPE_MGR_DBGCHK(stage_id < stat_tbl->dev_info->num_active_mau);
    return NULL;
  }
  int stage_idx = -1;
  for (int i = 0; i < stat_tbl_instance->num_stages; i++) {
    if (stat_tbl_instance->stat_tbl_stage_info[i].stage_id == stage_id) {
      stage_idx = i;
      break;
    }
  }
  if (stage_idx == -1) {
    LOG_ERROR(
        "Dev %d stats table 0x%x (%s) not found in stage %d for pipe %d index "
        "%d",
        stat_tbl->dev_info->dev_id,
        stat_tbl->stat_tbl_hdl,
        stat_tbl->name,
        stage_id,
        pipe_id,
        stage_ent_idx);
    PIPE_MGR_DBGCHK(stage_idx != -1);
    return NULL;
  }
  if (stat_tbl_instance->stat_tbl_stage_info[stage_idx].num_entries <=
      stage_ent_idx) {
    LOG_ERROR(
        "Invalid index %d, num-entries %d, dev %d stats table 0x%x (%s) pipe "
        "%d stage %d stage-idx %d",
        stage_ent_idx,
        stat_tbl_instance->stat_tbl_stage_info[stage_idx].num_entries,
        stat_tbl->dev_info->dev_id,
        stat_tbl->stat_tbl_hdl,
        stat_tbl->name,
        pipe_id,
        stage_id,
        stage_idx);
    PIPE_MGR_DBGCHK(
        stage_ent_idx <
        stat_tbl_instance->stat_tbl_stage_info[stage_idx].num_entries);
    return NULL;
  }

  PIPE_MGR_DBGCHK(stat_tbl_instance->ent_idx_info);
  PIPE_MGR_DBGCHK(stat_tbl_instance->ent_idx_info[pipe_id]);
  PIPE_MGR_DBGCHK(stat_tbl_instance->ent_idx_info[pipe_id][stage_id]);
  ent_idx_info =
      &stat_tbl_instance->ent_idx_info[pipe_id][stage_id][stage_ent_idx];

  return &ent_idx_info->entry_info;
}

pipe_mgr_stat_tbl_stage_info_t *pipe_mgr_stat_mgr_get_stage_info(
    pipe_mgr_stat_tbl_t *stat_tbl, bf_dev_pipe_t pipe_id, uint8_t stage_id) {
  uint32_t pipe_idx = 0;
  uint32_t stage_idx = 0;

  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance = NULL;

  for (pipe_idx = 0; pipe_idx < stat_tbl->num_instances; pipe_idx++) {
    if (((pipe_id == BF_DEV_PIPE_ALL) &&
         (stat_tbl->stat_tbl_instances[pipe_idx].pipe_id == pipe_id)) ||
        ((pipe_id != BF_DEV_PIPE_ALL) &&
         (PIPE_BITMAP_GET(&(stat_tbl->stat_tbl_instances[pipe_idx].pipe_bmp),
                          pipe_id)))) {
      stat_tbl_instance = &stat_tbl->stat_tbl_instances[pipe_idx];
      for (stage_idx = 0; stage_idx < stat_tbl_instance->num_stages;
           stage_idx++) {
        if (stat_tbl_instance->stat_tbl_stage_info[stage_idx].stage_id ==
            stage_id) {
          return &stat_tbl_instance->stat_tbl_stage_info[stage_idx];
        }
      }
    }
  }

  return NULL;
}

rmt_virt_addr_t pipe_mgr_stat_mgr_compute_ent_virt_addr(
    pipe_mgr_stat_tbl_stage_info_t *stat_tbl_stage_info,
    pipe_stat_stage_ent_idx_t stage_ent_idx) {
  pipe_mgr_stat_ram_alloc_info_t *stat_ram_alloc_info = NULL;
  rmt_virt_addr_t addr = 0;
  vpn_id_t stat_ram_vpn = 0;
  uint32_t ram_line_num = 0;
  uint8_t subword = 0;
  uint8_t num_entries_per_line = 0;
  uint8_t num_subword_bits = 0;
  uint8_t subword_shift = 0;
  uint8_t num_ram_line_bits = log2(TOF_SRAM_UNIT_DEPTH);
  uint8_t stat_ram_vpn_shift =
      TOF_STATS_RAM_NUM_SUBWORD_BITS + num_ram_line_bits;
  uint8_t wide_word_blk_idx = 0;
  uint32_t num_entries_per_wide_word_blk = 0;

  if (!stat_tbl_stage_info) {
    LOG_ERROR("%s : Mandatory parameters not provided", __func__);
    return 0;
  }

  stat_ram_alloc_info = stat_tbl_stage_info->stat_ram_alloc_info;
  num_entries_per_line = stat_tbl_stage_info->num_entries_per_line;
  ram_line_num = stage_ent_idx / num_entries_per_line;
  subword = stage_ent_idx % num_entries_per_line;

  switch (num_entries_per_line) {
    case 6:
      /* Currently not used */
      PIPE_MGR_DBGCHK(0);
      num_subword_bits = 3;
      subword_shift = 0;
      break;
    case 4:
      num_subword_bits = 2;
      subword_shift = 1;
      break;
    case 3:
      /* Currently not used */
      PIPE_MGR_DBGCHK(0);
      num_subword_bits = 2;
      subword_shift = 0;
      break;
    case 2:
      num_subword_bits = 1;
      subword_shift = 2;
      break;
    case 1:
      num_subword_bits = 0;
      subword_shift = 3;
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      break;
  }

  PIPE_MGR_DBGCHK(subword <= ((1 << num_subword_bits) - 1));

  num_entries_per_wide_word_blk = num_entries_per_line * TOF_SRAM_UNIT_DEPTH;
  wide_word_blk_idx = stage_ent_idx / num_entries_per_wide_word_blk;

  stat_ram_vpn = stat_ram_alloc_info->tbl_word_blk[wide_word_blk_idx].vpn_id[0];

  /* 20-bit stats address:
   *  19    PFE
   *  18:13 VPN
   *  12:3  RAM Line
   *  2:0   SubWord
   *  +--------------------------------------------------------+
   *  |PFE |STAT RAM  | RAM Line | Subword / Trailing Zeros    |
   *  |(1b)| VPN (6b) |  (10b)   |   (3b)                      |
   *  +--------------------------------------------------------+
   */

  /* Now form the address */

  addr = (((stat_ram_vpn & ((1 << TOF_STATS_RAM_NUM_VPN_BITS) - 1))
           << stat_ram_vpn_shift) |
          ((ram_line_num & ((1 << num_ram_line_bits) - 1))
           << TOF_STATS_RAM_NUM_SUBWORD_BITS) |
          ((subword & ((1 << num_subword_bits) - 1)) << subword_shift));
  /* Always encode the PFE bit. When encoding this will either be placed in
   * the right position by the encoder or not included at all if PFE is not
   * required. */
  addr |= (1 << TOF_STATS_RAM_NUM_ADDR_BITS);

  return addr;
}

pipe_status_t pipe_mgr_stat_mgr_compute_disabled_address(
    bf_dev_id_t device_id,
    pipe_stat_tbl_hdl_t stat_tbl_hdl,
    rmt_virt_addr_t *virt_addr) {
  pipe_mgr_stat_tbl_t *stat_tbl = NULL;

  stat_tbl = pipe_mgr_stat_tbl_get(device_id, stat_tbl_hdl);

  if (stat_tbl == NULL) {
    LOG_ERROR("%s:%d Stat table for device id %d, table handle %d",
              __func__,
              __LINE__,
              device_id,
              stat_tbl_hdl);
    return PIPE_OBJ_NOT_FOUND;
  }

  if (stat_tbl->enable_per_flow_enable == false) {
    LOG_ERROR(
        "%s:%d Request to generate a disabled stats address for a table"
        " which does not have per-flow-enable, device id %d, tbl hdl %d",
        __func__,
        __LINE__,
        device_id,
        stat_tbl_hdl);
    return PIPE_INVALID_ARG;
  }

  *virt_addr &= ~(1 << stat_tbl->per_flow_enable_bit_position);

  return PIPE_SUCCESS;
}

void pipe_mgr_stat_mgr_ent_free_worklist(
    pipe_mgr_stat_ent_worklist_t *worklist) {
  if (worklist == NULL) {
    /* Nothing to do */
    return;
  }

  pipe_mgr_stat_ent_worklist_t *traverser = worklist;

  /* Walk the link list and destroy each element */
  while (traverser) {
    BF_LIST_DLL_REM(worklist, traverser, next, prev);
    PIPE_MGR_FREE(traverser);
    traverser = worklist;
  }

  return;
}

void pipe_mgr_stat_mgr_free_ent_read_output(
    pipe_mgr_stat_ent_read_output_t *output) {
  if (output == NULL) {
    /* Nothing to do */
    return;
  }

  pipe_mgr_stat_ent_read_output_t *traverser = output;

  while (traverser) {
    BF_LIST_DLL_REM(output, traverser, next, prev);
    PIPE_MGR_FREE(traverser);
    traverser = output;
  }

  return;
}

/* Based on the statistic entry format, and given the byte counter value and
 * the packet counter value, this function returns the encoded 128 bit word
 * which can be used in an indirect write.
 * Notes:
 *   (1). Based on the counter format, the packet counter and/or byte counter
 *        value passed in will be considered. The appropriate value will also
 *        be ignored. For instance packet counter value for byte counter type
 *        and vice-versa.
 *   (2). The entry is encoded based on the supported configurations and format
 *        of statistic entries in Tofino.
 */

void stat_set_val(uint8_t *dst,
                  uint32_t dst_offset,
                  uint32_t len,
                  uint64_t val) {
  uint8_t *wp;  // Write pointer
  uint8_t wo;   // Write offset (bit offset within byte pointed to by wp).
  uint8_t wm;   // Write mask (mask of bits in byte pointed to by wp to set).

  wp = dst + dst_offset / 8;
  wo = dst_offset % 8;
  while (len) {
    wm = len < 8 ? (1 << len) - 1 : 0xFF;
    wm = wm << wo;

    *wp = (*wp & ~wm) | ((val << wo) & wm);

    val = val >> (8 - wo);
    ++wp;
    len -= (len < (uint8_t)(8 - wo)) ? len : (uint8_t)(8 - wo);
    wo = 0;
  }
}

void pipe_mgr_stat_mgr_encode_stat_entry(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_stage_info_t *stat_tbl_stage_info,
    dev_stage_t stage_id,
    uint8_t subword,
    uint64_t byte_counter,
    uint64_t packet_counter,
    rmt_ram_line_t *ram_line) {
  pipe_stat_type_t counter_type;
  uint64_t packet_counter_mask = 0;
  uint64_t byte_counter_mask = 0;
  uint8_t num_entries_per_line = 0;

  counter_type = stat_tbl->counter_type;

  packet_counter_mask =
      (UINT64_C(1) << stat_tbl->packet_counter_resolution) - 1;
  byte_counter_mask = (UINT64_C(1) << stat_tbl->byte_counter_resolution) - 1;

  packet_counter &= packet_counter_mask;
  byte_counter &= byte_counter_mask;

  num_entries_per_line = stat_tbl_stage_info->num_entries_per_line;

  /* Now based on number of entries per line and the counter type and resolution
   * the packing format of stat entries within the word is fixed, use that
   * to encode the entry.
   */

  if (counter_type == PACKET_AND_BYTE_COUNT) {
    switch (num_entries_per_line) {
      case 1:
        /* bits [127:64] is byte counter, bits [63:0] is packet counter */
        stat_set_val((uint8_t *)ram_line, 0, 64, byte_counter);
        stat_set_val((uint8_t *)ram_line, 64, 64, packet_counter);
        break;
      case 2:
        /* Subword 0 : bits [63:28] is byte counter, bits [27:0] is packet
         * counter
         * Subword 1 : bits [127:92] is byte counter, bits [91:64] is packet
         * counter
         */
        /* Assert if the resolution does not match up to the format */
        PIPE_MGR_DBGCHK(stat_tbl->packet_counter_resolution == 28);
        PIPE_MGR_DBGCHK(stat_tbl->byte_counter_resolution == 36);
        switch (subword) {
          case 0:
            stat_set_val((uint8_t *)ram_line, 0, 28, packet_counter);
            stat_set_val((uint8_t *)ram_line, 28, 36, byte_counter);
            break;
          case 1:
            stat_set_val((uint8_t *)ram_line, 64, 28, packet_counter);
            stat_set_val((uint8_t *)ram_line, 92, 36, byte_counter);
            break;
          default:
            /* Invalid subword */
            LOG_ERROR(
                "%s/%d : Invalid subword %d passed for tbl %#x"
                " dev %d counter type %d, stage id %d",
                __func__,
                __LINE__,
                subword,
                stat_tbl->stat_tbl_hdl,
                stat_tbl->device_id,
                counter_type,
                stage_id);
            PIPE_MGR_DBGCHK(0);
            return;
        }
        break;
      case 3:
        /* Subword 0 : bits [17:41] is byte counter, [16:0] is packet counter
         * Subword 1 : bits [84:60] is byte counter, [58:42] is packet counter
         * Subword 2 : bits [126:102] is byte counter, [101:85] is packet
         * counter
         */
        /* Assert if the resolution does not match up to the format */
        PIPE_MGR_DBGCHK(stat_tbl->packet_counter_resolution == 17);
        PIPE_MGR_DBGCHK(stat_tbl->byte_counter_resolution == 25);
        switch (subword) {
          case 0:
            stat_set_val((uint8_t *)ram_line, 0, 17, packet_counter);
            stat_set_val((uint8_t *)ram_line, 17, 25, byte_counter);
            break;
          case 1:
            stat_set_val((uint8_t *)ram_line, 42, 17, packet_counter);
            stat_set_val((uint8_t *)ram_line, 60, 25, byte_counter);
            break;
          case 2:
            stat_set_val((uint8_t *)ram_line, 42, 17, packet_counter);
            stat_set_val((uint8_t *)ram_line, 60, 25, byte_counter);
            break;
          default:
            /* Invalid subword */
            LOG_ERROR(
                "%s/%d : Invalid subword %d passed for tbl %#x"
                " dev %d counter type %d, stage id %d",
                __func__,
                __LINE__,
                subword,
                stat_tbl->stat_tbl_hdl,
                stat_tbl->device_id,
                counter_type,
                stage_id);
            PIPE_MGR_DBGCHK(0);
            return;
        }
        break;
      default:
        LOG_ERROR(
            "%s/%d : Invalid number of entries per line %d for tbl %#x"
            " dev %d counter type %d, stage id %d",
            __func__,
            __LINE__,
            num_entries_per_line,
            stat_tbl->stat_tbl_hdl,
            stat_tbl->device_id,
            counter_type,
            stage_id);
        PIPE_MGR_DBGCHK(0);
        return;
    }
  } else if (counter_type == PACKET_COUNT) {
    switch (num_entries_per_line) {
      case 2:
        /* Subword 0 : bits[63:0] packet counter
         * Subword 1 : bits[127:64] packet counter
         */
        /* Assert if the resolution does not match up to the format */
        PIPE_MGR_DBGCHK(stat_tbl->packet_counter_resolution == 64);
        PIPE_MGR_DBGCHK(stat_tbl->byte_counter_resolution == 0);
        switch (subword) {
          case 0:
            stat_set_val((uint8_t *)ram_line, 0, 64, packet_counter);
            break;
          case 1:
            stat_set_val((uint8_t *)ram_line, 64, 64, packet_counter);
            break;
          default:
            LOG_ERROR(
                "%s/%d : Invalid subword %d passed for tbl %#x"
                " dev %d counter type %d, stage id %d",
                __func__,
                __LINE__,
                subword,
                stat_tbl->stat_tbl_hdl,
                stat_tbl->device_id,
                counter_type,
                stage_id);
            PIPE_MGR_DBGCHK(0);
            return;
        }
        break;
      case 4:
        /* Subword 0 : bits[31:0] packet counter
         * Subword 1 : bits[63:32] packet counter
         * Subword 2 : bits[95:64] packet counter
         * Subword 3 : bits[127:96] packet counter
         */
        /* Assert if the resolution does not match up the format */
        PIPE_MGR_DBGCHK(stat_tbl->packet_counter_resolution == 32);
        switch (subword) {
          case 0:
            stat_set_val((uint8_t *)ram_line, 0, 32, packet_counter);
            break;
          case 1:
            stat_set_val((uint8_t *)ram_line, 32, 32, packet_counter);
            break;
          case 2:
            stat_set_val((uint8_t *)ram_line, 64, 32, packet_counter);
            break;
          case 3:
            stat_set_val((uint8_t *)ram_line, 96, 32, packet_counter);
            break;
          default:
            LOG_ERROR(
                "%s/%d : Invalid subword %d passed for tbl %#x"
                " dev %d counter type %d, stage id %d",
                __func__,
                __LINE__,
                subword,
                stat_tbl->stat_tbl_hdl,
                stat_tbl->device_id,
                counter_type,
                stage_id);
            PIPE_MGR_DBGCHK(0);
            return;
        }
        break;
      case 6:
        /* Subword 0 : bits[20:0] packet counter
         * Subword 1 : bits[41:21] packet counter
         * Subword 2 : bits[62:42] packet counter
         * Subword 3 : bits[84:64] packet counter
         * Subword 4 : bits[105:85] packet counter
         * Subword 5 : bits[126:106] packet counter
         */
        /* Assert if the resolution does not match up the format */
        PIPE_MGR_DBGCHK(stat_tbl->packet_counter_resolution == 21);
        switch (subword) {
          case 0:
            stat_set_val((uint8_t *)ram_line, 0, 21, packet_counter);
            break;
          case 1:
            stat_set_val((uint8_t *)ram_line, 21, 21, packet_counter);
            break;
          case 2:
            stat_set_val((uint8_t *)ram_line, 42, 21, packet_counter);
            break;
          case 3:
            stat_set_val((uint8_t *)ram_line, 64, 21, packet_counter);
            break;
          case 4:
            stat_set_val((uint8_t *)ram_line, 85, 21, packet_counter);
            break;
          case 5:
            stat_set_val((uint8_t *)ram_line, 106, 21, packet_counter);
            break;
          default:
            LOG_ERROR(
                "%s/%d : Invalid subword %d passed for tbl %#x"
                " dev %d counter type %d, stage id %d",
                __func__,
                __LINE__,
                subword,
                stat_tbl->stat_tbl_hdl,
                stat_tbl->device_id,
                counter_type,
                stage_id);
            PIPE_MGR_DBGCHK(0);
            return;
        }
        break;
      default:
        LOG_ERROR(
            "%s/%d : Invalid number of entries per line %d for tbl %#x"
            " dev %d counter type %d, stage id %d",
            __func__,
            __LINE__,
            num_entries_per_line,
            stat_tbl->stat_tbl_hdl,
            stat_tbl->device_id,
            counter_type,
            stage_id);
        PIPE_MGR_DBGCHK(0);
        return;
    }
  } else if (counter_type == BYTE_COUNT) {
    switch (num_entries_per_line) {
      case 2:
        /* Subword 0 : bits [63:0] byte counter
         * Subword 1 : bits [127:64] byte counter
         */
        /* Assert if the resolution does not match up the format */
        PIPE_MGR_DBGCHK(stat_tbl->packet_counter_resolution == 0);
        PIPE_MGR_DBGCHK(stat_tbl->byte_counter_resolution == 64);
        switch (subword) {
          case 0:
            stat_set_val((uint8_t *)ram_line, 0, 64, byte_counter);
            break;
          case 1:
            stat_set_val((uint8_t *)ram_line, 64, 64, byte_counter);
            break;
          default:
            LOG_ERROR(
                "%s/%d : Invalid subword %d passed for tbl %#x"
                " dev %d counter type %d, stage id %d",
                __func__,
                __LINE__,
                subword,
                stat_tbl->stat_tbl_hdl,
                stat_tbl->device_id,
                counter_type,
                stage_id);
            PIPE_MGR_DBGCHK(0);
            return;
        }
        break;
      case 4:
        /* Subword 0 : bits [31:0] byte counter
         * Subword 1 : bits [63:32] byte counter
         * Subword 2 : bits [95:64] byte counter
         * Subword 3 : bits [127:96] byte counter
         */
        /* Assert if the resolution does not match up the format */
        PIPE_MGR_DBGCHK(stat_tbl->packet_counter_resolution == 0);
        PIPE_MGR_DBGCHK(stat_tbl->byte_counter_resolution == 32);
        switch (subword) {
          case 0:
            stat_set_val((uint8_t *)ram_line, 0, 32, byte_counter);
            break;
          case 1:
            stat_set_val((uint8_t *)ram_line, 32, 32, byte_counter);
            break;
          case 2:
            stat_set_val((uint8_t *)ram_line, 64, 32, byte_counter);
            break;
          case 3:
            stat_set_val((uint8_t *)ram_line, 96, 32, byte_counter);
            break;
          default:
            LOG_ERROR(
                "%s/%d : Invalid subword %d passed for tbl %#x"
                " dev %d counter type %d, stage id %d",
                __func__,
                __LINE__,
                subword,
                stat_tbl->stat_tbl_hdl,
                stat_tbl->device_id,
                counter_type,
                stage_id);
            PIPE_MGR_DBGCHK(0);
            return;
        }
        break;
      default:
        LOG_ERROR(
            "%s/%d : Invalid number of entries per line %d for tbl %#x"
            " dev %d counter type %d, stage id %d",
            __func__,
            __LINE__,
            num_entries_per_line,
            stat_tbl->stat_tbl_hdl,
            stat_tbl->device_id,
            counter_type,
            stage_id);
        PIPE_MGR_DBGCHK(0);
        return;
    }
  } else {
    PIPE_MGR_DBGCHK(0);
    return;
  }
}

/* pipe_mgr_stat_mgr_decode_stat_entry : Given a 128 bit read value, decode it
 * to packet and byte counter value based on the format of the table. This is
 * needed because, when a virtual indirect read is done on a particular stat
 * entry, Tofino returns 128 bits of data, and this routine is used to extract
 * packet and byte counter value from 128 bits of data passed in, in the form
 * of val0 and val1, val0 being the lower 64 bits and val1 being the upper 64
 * bits.
 */

pipe_status_t pipe_mgr_stat_mgr_decode_stat_entry(pipe_mgr_stat_tbl_t *stat_tbl,
                                                  bf_dev_pipe_t pipe_id,
                                                  uint8_t stage_id,
                                                  uint8_t subword,
                                                  uint64_t *byte_counter,
                                                  uint64_t *packet_counter,
                                                  uint64_t val0,
                                                  uint64_t val1) {
  pipe_mgr_stat_tbl_stage_info_t *stat_tbl_stage_info = NULL;
  pipe_stat_type_t counter_type;
  uint64_t packet_counter_mask = 0;
  uint64_t byte_counter_mask = 0;
  uint8_t num_entries_per_line = 0;

  counter_type = stat_tbl->counter_type;

  stat_tbl_stage_info =
      pipe_mgr_stat_mgr_get_stage_info(stat_tbl, pipe_id, stage_id);

  if (stat_tbl_stage_info == NULL) {
    LOG_ERROR(
        "%s : Stat tbl stage info not found for tbl %#x dev %d, pipe id %d"
        " stage id %d",
        __func__,
        stat_tbl->stat_tbl_hdl,
        stat_tbl->device_id,
        pipe_id,
        stage_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  packet_counter_mask =
      ((UINT64_C(1) << stat_tbl->packet_counter_resolution) - 1);
  byte_counter_mask = ((UINT64_C(1) << stat_tbl->byte_counter_resolution) - 1);

  num_entries_per_line = stat_tbl_stage_info->num_entries_per_line;

  /* Now based on number of entries per line and the counter type and resolution
   * the packing format of stat entries within the word is fixed, use that
   * to decode the entry.
   */

  if (counter_type == PACKET_AND_BYTE_COUNT) {
    switch (num_entries_per_line) {
      case 1:
        /* bits [127:64] is byte counter, bits [63:0] is packet counter */
        *packet_counter = (val0 & packet_counter_mask);
        *byte_counter = (val1 & byte_counter_mask);
        break;
      case 2:
        /* Subword 0 : bits [63:28] is byte counter, bits [27:0] is packet
         * counter
         * Subword 1 : bits [127:92] is byte counter, bits [91:64] is packet
         * counter
         */
        switch (subword) {
          case 0:
            *packet_counter = (val0 & packet_counter_mask);
            *byte_counter = ((val0 >> 28) & byte_counter_mask);
            break;
          case 1:
            *packet_counter = (val1 & packet_counter_mask);
            *byte_counter = ((val1 >> 28) && byte_counter_mask);
            break;
          default:
            LOG_ERROR(
                "%s/%d : Invalid subword %d passed for tbl %#x"
                " dev %d counter type %d, stage id %d",
                __func__,
                __LINE__,
                subword,
                stat_tbl->stat_tbl_hdl,
                stat_tbl->device_id,
                counter_type,
                stage_id);
            PIPE_MGR_DBGCHK(0);
            return PIPE_INVALID_ARG;
        }
        break;
      case 3:
        /* Subword 0 : bits [17:41] is byte counter, [16:0] is packet counter
         * Subword 1 : bits [84:60] is byte counter, [58:42] is packet counter
         * Subword 2 : bits [126:102] is byte counter, [101:85] is packet
         * counter
         */
        switch (subword) {
          case 0:
            *packet_counter = (val0 & packet_counter_mask);
            *byte_counter = ((val0 >> 16) & byte_counter_mask);
            break;
          case 1:
            *packet_counter = ((val0 >> 42) & packet_counter_mask);
            *byte_counter = (val1 | (val0 >> 60)) & byte_counter_mask;
            break;
          case 2:
            *packet_counter = (val1 >> 21) & packet_counter_mask;
            *byte_counter = (val1 >> 38) & byte_counter_mask;
            break;
          default:
            LOG_ERROR(
                "%s/%d : Invalid subword %d passed for tbl %#x"
                " dev %d counter type %d, stage id %d",
                __func__,
                __LINE__,
                subword,
                stat_tbl->stat_tbl_hdl,
                stat_tbl->device_id,
                counter_type,
                stage_id);
            PIPE_MGR_DBGCHK(0);
            return PIPE_INVALID_ARG;
        }
        break;
      default:
        LOG_ERROR(
            "%s/%d : Invalid number of entries per line %d for tbl %#x"
            " dev %d counter type %d, stage id %d",
            __func__,
            __LINE__,
            num_entries_per_line,
            stat_tbl->stat_tbl_hdl,
            stat_tbl->device_id,
            counter_type,
            stage_id);
        PIPE_MGR_DBGCHK(0);
        return PIPE_INVALID_ARG;
    }
  } else if (counter_type == PACKET_COUNT) {
    switch (num_entries_per_line) {
      case 2:
        /* Subword 0 : bits[63:0] packet counter
         * Subword 1 : bits[127:64] packet counter
         */
        switch (subword) {
          case 0:
            *packet_counter = val0 & packet_counter_mask;
            *byte_counter = 0;
            break;
          case 1:
            *packet_counter = val1 & packet_counter_mask;
            *byte_counter = 0;
            break;
          default:
            LOG_ERROR(
                "%s/%d : Invalid subword %d passed for tbl %#x"
                " dev %d counter type %d, stage id %d",
                __func__,
                __LINE__,
                subword,
                stat_tbl->stat_tbl_hdl,
                stat_tbl->device_id,
                counter_type,
                stage_id);
            PIPE_MGR_DBGCHK(0);
            return PIPE_INVALID_ARG;
        }
        break;
      case 4:
        /* Subword 0 : bits[31:0] packet counter
         * Subword 1 : bits[63:32] packet counter
         * Subword 2 : bits[95:64] packet counter
         * Subword 3 : bits[127:96] packet counter
         */
        switch (subword) {
          case 0:
            *packet_counter = val0 & packet_counter_mask;
            *byte_counter = 0;
            break;
          case 1:
            *packet_counter = (val0 >> 32) & packet_counter_mask;
            *byte_counter = 0;
            break;
          case 2:
            *packet_counter = val1 & packet_counter_mask;
            *byte_counter = 0;
            break;
          case 3:
            *packet_counter = (val1 >> 32) & packet_counter_mask;
            *byte_counter = 0;
            break;
          default:
            LOG_ERROR(
                "%s/%d : Invalid subword %d passed for tbl %#x"
                " dev %d counter type %d, stage id %d",
                __func__,
                __LINE__,
                subword,
                stat_tbl->stat_tbl_hdl,
                stat_tbl->device_id,
                counter_type,
                stage_id);
            PIPE_MGR_DBGCHK(0);
            return PIPE_INVALID_ARG;
        }
        break;
      case 6:
        /* Subword 0 : bits[20:0] packet counter
         * Subword 1 : bits[41:21] packet counter
         * Subword 2 : bits[62:42] packet counter
         * Subword 3 : bits[84:64] packet counter
         * Subword 4 : bits[105:85] packet counter
         * Subword 5 : bits[126:106] packet counter
         */
        switch (subword) {
          case 0:
            *packet_counter = val0 & packet_counter_mask;
            *byte_counter = 0;
            break;
          case 1:
            *packet_counter = (val0 >> 21) & packet_counter_mask;
            *byte_counter = 0;
            break;
          case 2:
            *packet_counter = (val0 >> 42) & packet_counter_mask;
            *byte_counter = 0;
            break;
          case 3:
            *packet_counter = val1 & packet_counter_mask;
            *byte_counter = 0;
            break;
          case 4:
            *packet_counter = (val1 >> 21) & packet_counter_mask;
            *byte_counter = 0;
            break;
          case 5:
            *packet_counter = (val1 >> 42) & packet_counter_mask;
            *byte_counter = 0;
            break;
          default:
            LOG_ERROR(
                "%s/%d : Invalid subword %d passed for tbl %#x"
                " dev %d counter type %d, stage id %d",
                __func__,
                __LINE__,
                subword,
                stat_tbl->stat_tbl_hdl,
                stat_tbl->device_id,
                counter_type,
                stage_id);
            PIPE_MGR_DBGCHK(0);
            return PIPE_INVALID_ARG;
        }
        break;
      default:
        LOG_ERROR(
            "%s/%d : Invalid number of entries per line %d for tbl %#x"
            " dev %d counter type %d, stage id %d",
            __func__,
            __LINE__,
            num_entries_per_line,
            stat_tbl->stat_tbl_hdl,
            stat_tbl->device_id,
            counter_type,
            stage_id);
        PIPE_MGR_DBGCHK(0);
        return PIPE_INVALID_ARG;
    }
  } else if (counter_type == BYTE_COUNT) {
    switch (num_entries_per_line) {
      case 2:
        /* Subword 0 : bits [63:0] byte counter
         * Subword 1 : bits [127:64] byte counter
         */
        switch (subword) {
          case 0:
            *packet_counter = 0;
            *byte_counter = val0 & byte_counter_mask;
            break;
          case 1:
            *packet_counter = 0;
            *byte_counter = val1 & byte_counter_mask;
            break;
          default:
            LOG_ERROR(
                "%s/%d : Invalid subword %d passed for tbl %#x"
                " dev %d counter type %d, stage id %d",
                __func__,
                __LINE__,
                subword,
                stat_tbl->stat_tbl_hdl,
                stat_tbl->device_id,
                counter_type,
                stage_id);
            PIPE_MGR_DBGCHK(0);
            return PIPE_INVALID_ARG;
        }
        break;
      case 4:
        /* Subword 0 : bits [31:0] byte counter
         * Subword 1 : bits [63:32] byte counter
         * Subword 2 : bits [95:64] byte counter
         * Subword 3 : bits [127:96] byte counter
         */
        switch (subword) {
          case 0:
            *packet_counter = 0;
            *byte_counter = val0 & byte_counter_mask;
            break;
          case 1:
            *packet_counter = 0;
            *byte_counter = (val0 >> 32) & byte_counter_mask;
            break;
          case 2:
            *packet_counter = 0;
            *byte_counter = val1 & byte_counter_mask;
            break;
          case 3:
            *packet_counter = 0;
            *byte_counter = (val1 >> 32) & byte_counter_mask;
            break;
          default:
            LOG_ERROR(
                "%s/%d : Invalid subword %d passed for tbl %#x"
                " dev %d counter type %d, stage id %d",
                __func__,
                __LINE__,
                subword,
                stat_tbl->stat_tbl_hdl,
                stat_tbl->device_id,
                counter_type,
                stage_id);
            PIPE_MGR_DBGCHK(0);
            return PIPE_INVALID_ARG;
        }
        break;
      default:
        LOG_ERROR(
            "%s/%d : Invalid number of entries per line %d for tbl %#x"
            " counter type %d, stage id %d",
            __func__,
            __LINE__,
            num_entries_per_line,
            stat_tbl->stat_tbl_hdl,
            counter_type,
            stage_id);
        PIPE_MGR_DBGCHK(0);
        return PIPE_INVALID_ARG;
    }
  } else {
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }

  return PIPE_SUCCESS;
}

void pipe_mgr_stat_mgr_add_barrier_state(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    pipe_bitmap_t *pipe_bmp,
    pipe_mgr_stat_barrier_state_t *barrier_state) {
  pipe_mgr_stat_barrier_list_node_t
      *nodes[stat_tbl->dev_info->num_active_pipes];

  int pipe_map = 0;
  for (unsigned i = 0; i < stat_tbl->dev_info->num_active_pipes; ++i) {
    if (PIPE_BITMAP_GET(pipe_bmp, i)) {
      nodes[i] = PIPE_MGR_CALLOC(1, sizeof *nodes[i]);
      if (nodes[i] == NULL) {
        LOG_ERROR("%s/%d : Malloc failure", __func__, __LINE__);
        for (i = 0; i < stat_tbl->dev_info->num_active_pipes; ++i) {
          if (pipe_map & (1u << i)) {
            PIPE_MGR_FREE(nodes[i]);
          }
        }
        return;
      }
      pipe_map |= 1u << i;
      nodes[i]->barrier_state = barrier_state;
    } else {
      nodes[i] = NULL;
    }
  }
  PIPE_MGR_DBGCHK(pipe_map == barrier_state->pipe_ref_map);

  PIPE_MGR_LOCK(&stat_tbl_instance->barrier_data_mtx);
  bf_dev_pipe_t pipe_iter = 0;
  PIPE_BITMAP_ITER(pipe_bmp, pipe_iter) {
    if (pipe_iter >= stat_tbl->dev_info->num_active_pipes) {
      PIPE_MGR_DBGCHK(0);
    } else {
      BF_LIST_DLL_AP(stat_tbl_instance->barrier_list[pipe_iter],
                     nodes[pipe_iter],
                     next,
                     prev);
    }
  }
  pipe_mgr_stat_mgr_trace_bar(stat_tbl, stat_tbl_instance, barrier_state);
  PIPE_MGR_UNLOCK(&stat_tbl_instance->barrier_data_mtx);
}

void pipe_mgr_stat_tbl_lkup(bf_dev_id_t device_id,
                            uint8_t ltbl_id,
                            bf_dev_pipe_t pipe_id,
                            dev_stage_t stage_id,
                            pipe_mgr_stat_tbl_t **stat_tbl,
                            pipe_mgr_stat_tbl_instance_t **stat_tbl_inst) {
  unsigned long k = pipe_id;
  k = k << 8 | stage_id;
  k = k << 8 | ltbl_id;
  void *data = NULL;
  if (!stat_tbl || !stat_tbl_inst) return;
  *stat_tbl = NULL;
  *stat_tbl_inst = NULL;
  pipe_mgr_stat_tbl_hdls_map_get(device_id, k, &data);
  if (data) {
    *stat_tbl = (pipe_mgr_stat_tbl_t *)data;
    *stat_tbl_inst =
        pipe_mgr_stat_tbl_get_instance_any_pipe(*stat_tbl, pipe_id);
  }
}

pipe_status_t pipe_mgr_stat_mgr_get_ent_idx_from_virt_addr(
    pipe_mgr_stat_tbl_t *stat_tbl,
    rmt_virt_addr_t virt_addr,
    bf_dev_pipe_t pipe_id,
    dev_stage_t stage_id,
    pipe_stat_stage_ent_idx_t *stage_ent_idx) {
  uint32_t ram_line_num = 0;
  uint32_t num_entries_per_wide_word_blk = 0;
  uint8_t subword = 0;
  uint8_t wide_word_blk_idx = 0;
  uint8_t num_entries_per_line = 0;
  uint8_t num_subword_bits = 0;
  uint8_t num_trail_zeros = 0;
  vpn_id_t vpn = 0;

  pipe_mgr_stat_tbl_stage_info_t *stat_tbl_stage_info = NULL;
  pipe_mgr_stat_ram_alloc_info_t *stat_ram_alloc_info = NULL;

  stat_tbl_stage_info =
      pipe_mgr_stat_mgr_get_stage_info(stat_tbl, pipe_id, stage_id);
  if (!stat_tbl_stage_info) {
    LOG_ERROR(
        "%s : Stat table stage info not found. Tbl 0x%x, pipe id %x, stage "
        "id %d",
        __func__,
        stat_tbl->stat_tbl_hdl,
        pipe_id,
        stage_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  stat_ram_alloc_info = stat_tbl_stage_info->stat_ram_alloc_info;

  num_entries_per_line = stat_tbl_stage_info->num_entries_per_line;

  switch (num_entries_per_line) {
    case 6:
      num_trail_zeros = 0;
      num_subword_bits = 3;
      /* Not used currently */
      PIPE_MGR_DBGCHK(0);
      break;
    case 4:
      num_trail_zeros = 1;
      num_subword_bits = 2;
      break;
    case 3:
      num_trail_zeros = 0;
      num_subword_bits = 2;
      /* Not used currently */
      PIPE_MGR_DBGCHK(0);
      break;
    case 2:
      num_trail_zeros = 2;
      num_subword_bits = 1;
      break;
    case 1:
      num_trail_zeros = 3;
      num_subword_bits = 0;
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      break;
  }

  subword = (virt_addr >> num_trail_zeros) & ((1 << num_subword_bits) - 1);
  ram_line_num = (virt_addr >> (num_trail_zeros + num_subword_bits)) &
                 ((1 << TOF_SRAM_NUM_RAM_LINE_BITS) - 1);
  vpn = (virt_addr >>
         (num_trail_zeros + num_subword_bits + TOF_SRAM_NUM_RAM_LINE_BITS)) &
        ((1 << TOF_STATS_RAM_NUM_VPN_BITS) - 1);

  unsigned i = 0;
  for (i = 0; i < stat_ram_alloc_info->num_wide_word_blks; i++) {
    if (vpn == stat_ram_alloc_info->tbl_word_blk[i].vpn_id[0]) {
      wide_word_blk_idx = i;
      break;
    }
  }
  if (i >= stat_ram_alloc_info->num_wide_word_blks) {
    /* This error implies that the VPN in the address is out of range of the
     * table. This can happen in a LR(t) evict message because of a hardware
     * FIFO overflow at the stats ALU, which can happen if software does not
     * push free memory fast enough. Flag this error. This error can corrupt
     * the counters in the hardware.
     */
    LOG_ERROR(
        "%s: Dev %d Tbl %s, 0x%x, pipe %d stage %d, unexpected VPN %d (first "
        "VPN %d, last VPN %d)",
        __func__,
        stat_tbl->device_id,
        stat_tbl->name,
        stat_tbl->stat_tbl_hdl,
        pipe_id,
        stage_id,
        vpn,
        stat_ram_alloc_info->tbl_word_blk[0].vpn_id[0],
        stat_ram_alloc_info
            ->tbl_word_blk[stat_ram_alloc_info->num_wide_word_blks - 1]
            .vpn_id[0]);
    return PIPE_UNEXPECTED;
  }
  num_entries_per_wide_word_blk = num_entries_per_line * TOF_SRAM_UNIT_DEPTH;

  *stage_ent_idx = (wide_word_blk_idx * num_entries_per_wide_word_blk) +
                   (ram_line_num * num_entries_per_line) + subword;

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stat_mgr_get_ent_idx_count(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    bf_dev_pipe_t pipe_id,
    dev_stage_t stage_id,
    pipe_stat_stage_ent_idx_t stage_ent_idx,
    pipe_stat_data_t *stat_data) {
  pipe_mgr_stat_entry_info_t *stat_ent_info = NULL;

  stat_ent_info = pipe_mgr_stat_mgr_get_ent_idx_info(
      stat_tbl, stat_tbl_instance, pipe_id, stage_id, stage_ent_idx);

  if (stat_ent_info == NULL) {
    return PIPE_OBJ_NOT_FOUND;
  }

  PIPE_MGR_LOCK(&stat_ent_info->mtx);
  *stat_data = stat_ent_info->stat_data;
  PIPE_MGR_UNLOCK(&stat_ent_info->mtx);

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stat_mgr_set_ent_idx_count(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    bool set_in_prog,
    bf_dev_pipe_t pipe_id,
    dev_stage_t stage_id,
    pipe_stat_ent_idx_t stage_ent_idx,
    pipe_stat_data_t *stat_data) {
  pipe_mgr_stat_entry_info_t *stat_ent_info = NULL;

  stat_ent_info = pipe_mgr_stat_mgr_get_ent_idx_info(
      stat_tbl, stat_tbl_instance, pipe_id, stage_id, stage_ent_idx);
  if (stat_ent_info == NULL) {
    LOG_ERROR(
        "Cannot find stat-idx-info, dev %d table %s 0x%x pipe %d stage %d idx "
        "%d",
        stat_tbl->device_id,
        stat_tbl->name,
        stat_tbl->stat_tbl_hdl,
        pipe_id,
        stage_id,
        stage_ent_idx);
    PIPE_MGR_DBGCHK(stat_ent_info);
    return PIPE_OBJ_NOT_FOUND;
  }

  PIPE_MGR_LOCK(&stat_ent_info->mtx);
  stat_ent_info->stat_data = *stat_data;
  if (set_in_prog) stat_ent_info->user_set_in_progress++;
  PIPE_MGR_UNLOCK(&stat_ent_info->mtx);

  if (stat_mgr_enable_detail_trace) {
    PIPE_MGR_STAT_TRACE(stat_tbl->device_id,
                        stat_tbl->stat_tbl_hdl,
                        stat_tbl->name,
                        -1,
                        stage_ent_idx,
                        pipe_id,
                        stage_id,
                        "Updating SW count packet/byte %" PRIu64 "/%" PRIu64,
                        stat_data->packets,
                        stat_data->bytes);
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stat_mgr_incr_ent_idx_count(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    bf_dev_pipe_t pipe_id,
    dev_stage_t stage_id,
    pipe_stat_stage_ent_idx_t stage_ent_idx,
    pipe_stat_data_t *stat_data) {
  pipe_mgr_stat_entry_info_t *stat_ent_info = NULL;
  uint64_t packet_count = 0;
  uint64_t byte_count = 0;

  stat_ent_info = pipe_mgr_stat_mgr_get_ent_idx_info(
      stat_tbl, stat_tbl_instance, pipe_id, stage_id, stage_ent_idx);

  if (stat_ent_info == NULL) {
    return PIPE_OBJ_NOT_FOUND;
  }

  uint32_t user_set_cnt = 0;
  PIPE_MGR_LOCK(&stat_ent_info->mtx);
  if (stat_ent_info->user_set_in_progress) {
    user_set_cnt = stat_ent_info->user_set_in_progress;
  } else {
    stat_ent_info->stat_data.bytes += stat_data->bytes;
    stat_ent_info->stat_data.packets += stat_data->packets;
    packet_count = stat_ent_info->stat_data.packets;
    byte_count = stat_ent_info->stat_data.bytes;
  }
  PIPE_MGR_UNLOCK(&stat_ent_info->mtx);

  if (stat_mgr_enable_detail_trace) {
    if (user_set_cnt) {
      PIPE_MGR_STAT_TRACE(
          stat_tbl->device_id,
          stat_tbl->stat_tbl_hdl,
          stat_tbl->name,
          -1,
          stage_ent_idx,
          pipe_id,
          stage_id,
          "Update to entry idx %d dropped, set-cnt %d, packet count 0x%" PRIx64
          ", byte count 0x%" PRIx64,
          stage_ent_idx,
          user_set_cnt,
          packet_count,
          byte_count);
    } else {
      PIPE_MGR_STAT_TRACE(
          stat_tbl->device_id,
          stat_tbl->stat_tbl_hdl,
          stat_tbl->name,
          -1,
          stage_ent_idx,
          pipe_id,
          stage_id,
          "Updating entry idx %d count to, packet count 0x%" PRIx64
          ", byte count 0x%" PRIx64,
          stage_ent_idx,
          packet_count,
          byte_count);
    }
  }

  return PIPE_SUCCESS;
}

/* Caller must hold ent_hdl_loc_mtx */
static pipe_status_t pipe_mgr_stat_mgr_copy_idx_count(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    bf_dev_pipe_t pipe_id,
    dev_stage_t src_stage_id,
    dev_stage_t dst_stage_id,
    pipe_stat_stage_ent_idx_t src_stat_ent_idx,
    pipe_stat_stage_ent_idx_t dst_stat_ent_idx) {
  pipe_stat_data_t zero_stats = {0};

  pipe_mgr_stat_entry_info_t *dst_stat_ent_info = NULL;
  dst_stat_ent_info = pipe_mgr_stat_mgr_get_ent_idx_info(
      stat_tbl, stat_tbl_instance, pipe_id, dst_stage_id, dst_stat_ent_idx);
  if (dst_stat_ent_info == NULL) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }
  pipe_mgr_stat_entry_info_t *src_stat_ent_info = NULL;
  src_stat_ent_info = pipe_mgr_stat_mgr_get_ent_idx_info(
      stat_tbl, stat_tbl_instance, pipe_id, src_stage_id, src_stat_ent_idx);
  if (src_stat_ent_info == NULL) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }

  PIPE_MGR_LOCK(&src_stat_ent_info->mtx);
  uint32_t user_set_cnt = src_stat_ent_info->user_set_in_progress;
  pipe_stat_data_t stat_data = src_stat_ent_info->stat_data;
  src_stat_ent_info->user_set_in_progress = 0;
  src_stat_ent_info->stat_data = zero_stats;
  PIPE_MGR_UNLOCK(&src_stat_ent_info->mtx);

  PIPE_MGR_LOCK(&dst_stat_ent_info->mtx);
  dst_stat_ent_info->user_set_in_progress = user_set_cnt;
  dst_stat_ent_info->stat_data = stat_data;
  PIPE_MGR_UNLOCK(&dst_stat_ent_info->mtx);

  return PIPE_SUCCESS;
}

/* Note the caller holds stat_tbl_instance->barrier_data_mtx */
void pipe_mgr_stat_mgr_execute_task_list(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    bf_dev_pipe_t pipe_id,
    pipe_mgr_stat_mgr_task_node_t *head) {
  pipe_status_t status = PIPE_SUCCESS;
  bf_dev_id_t device_id = stat_tbl->device_id;
  pipe_mgr_stat_mgr_task_type_move_t *mov_node;
  pipe_mgr_stat_mgr_task_type_ent_add_t *add_node;
  pipe_mgr_stat_mgr_task_type_ent_del_t *del_node;

  pipe_mgr_stat_mgr_task_node_t *node = head;
  while (node) {
    switch (node->type) {
      case PIPE_MGR_STAT_TASK_MOVE:
        mov_node = &node->u.move_node;
        status = pipe_mgr_stat_mgr_stat_ent_move(stat_tbl,
                                                 stat_tbl_instance,
                                                 pipe_id,
                                                 mov_node->src_stage_id,
                                                 mov_node->dst_stage_id,
                                                 mov_node->mat_ent_hdl,
                                                 mov_node->src_ent_idx,
                                                 mov_node->dst_ent_idx,
                                                 false);

        if (status != PIPE_SUCCESS) {
          LOG_ERROR(
              "%s:%d Error in moving stat entry from idx %d"
              "to idx %d, tbl 0x%x, device id %d, pipe %d",
              __func__,
              __LINE__,
              mov_node->src_ent_idx,
              mov_node->dst_ent_idx,
              stat_tbl->stat_tbl_hdl,
              stat_tbl->device_id,
              pipe_id);
          PIPE_MGR_DBGCHK(0);
          break;
        }
        pipe_mgr_stat_mgr_trace_def_mov(stat_tbl,
                                        stat_tbl_instance,
                                        mov_node->mat_ent_hdl,
                                        mov_node->dst_stage_id,
                                        mov_node->dst_ent_idx,
                                        mov_node->src_stage_id,
                                        mov_node->src_ent_idx,
                                        pipe_id);
        break;
      case PIPE_MGR_STAT_TASK_ENTRY_ADD:
        add_node = &node->u.ent_add_node;
        status = pipe_mgr_stat_mgr_activate_new_entry(stat_tbl,
                                                      stat_tbl_instance,
                                                      add_node->mat_ent_hdl,
                                                      pipe_id,
                                                      add_node->stage_id,
                                                      add_node->stage_ent_idx);

        if (status != PIPE_SUCCESS) {
          LOG_ERROR(
              "%s:%d  Error in setting entry index count for idx %d"
              " tbl 0x%x, device id %d, pipe id %d, err %s",
              __func__,
              __LINE__,
              add_node->stage_ent_idx,
              stat_tbl->stat_tbl_hdl,
              device_id,
              stat_tbl_instance->pipe_id,
              pipe_str_err(status));
          PIPE_MGR_DBGCHK(0);
          break;
        }
        pipe_mgr_stat_mgr_trace_def_add(stat_tbl,
                                        stat_tbl_instance,
                                        add_node->mat_ent_hdl,
                                        add_node->stage_id,
                                        add_node->stage_ent_idx,
                                        pipe_id);
        break;
      case PIPE_MGR_STAT_TASK_ENTRY_DEL:
        del_node = &node->u.ent_del_node;
        status = pipe_mgr_stat_mgr_del_ent_hdl_loc(stat_tbl,
                                                   stat_tbl_instance,
                                                   del_node->ent_hdl,
                                                   pipe_id,
                                                   del_node->stage_id,
                                                   del_node->stage_ent_idx,
                                                   false);

        if (status != PIPE_SUCCESS) {
          LOG_ERROR(
              "%s:%d Error in deleting entry handle location for entry handle "
              "%d stage %d stat tbl 0x%x, pipe id %x err %s",
              __func__,
              __LINE__,
              del_node->ent_hdl,
              del_node->stage_id,
              stat_tbl->stat_tbl_hdl,
              stat_tbl_instance->pipe_id,
              pipe_str_err(status));
          PIPE_MGR_DBGCHK(0);
          break;
        }
        pipe_mgr_stat_mgr_trace_def_del(stat_tbl,
                                        stat_tbl_instance,
                                        del_node->ent_hdl,
                                        del_node->stage_id,
                                        del_node->stage_ent_idx,
                                        pipe_id);
        break;
      default:
        PIPE_MGR_DBGCHK(0);
    }
    pipe_mgr_stat_mgr_task_node_t *tmp = node;
    node = node->next;
    PIPE_MGR_FREE(tmp);
  }
}

static void pipe_mgr_stat_mgr_destroy_locs(pipe_mgr_stat_ent_location_t *locs) {
  while (locs) {
    pipe_mgr_stat_ent_location_t *traverser = locs;
    BF_LIST_DLL_REM(locs, traverser, next, prev);
    PIPE_MGR_FREE(traverser);
  }
  return;
}

void pipe_mgr_stat_mgr_destroy_ent_hdl_loc(
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance) {
  unsigned long key = 0;
  void *data = NULL;
  pipe_mgr_stat_mgr_ent_hdl_loc_t *ent_hdl_loc = NULL;
  /* Lock the map while we are deleting */
  PIPE_MGR_LOCK(&(stat_tbl_instance->ent_hdl_loc_mtx));
  while (BF_MAP_OK == bf_map_get_first_rmv(&stat_tbl_instance->ent_hdl_loc,
                                           &key,
                                           (void **)&data)) {
    ent_hdl_loc = (pipe_mgr_stat_mgr_ent_hdl_loc_t *)data;
    if (ent_hdl_loc->locations) {
      pipe_mgr_stat_mgr_destroy_locs(ent_hdl_loc->locations);
    }
    PIPE_MGR_FREE(data);
  }
  PIPE_MGR_UNLOCK(&(stat_tbl_instance->ent_hdl_loc_mtx));

  return;
}

static pipe_status_t pipe_mgr_stat_mgr_add_ent_hdl_loc(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    bf_dev_pipe_t pipe_id,
    dev_stage_t stage_id,
    pipe_stat_stage_ent_idx_t stage_ent_idx,
    bool pending,
    pipe_stat_data_t *user_count) {
  bf_map_sts_t map_sts = BF_MAP_OK;
  unsigned long key = mat_ent_hdl;
  void *data = NULL;
  pipe_mgr_stat_mgr_ent_hdl_loc_t *ent_hdl_loc = NULL;
  pipe_mgr_stat_ent_location_t *location = NULL;

  location = (pipe_mgr_stat_ent_location_t *)PIPE_MGR_CALLOC(
      1, sizeof(pipe_mgr_stat_ent_location_t));

  if (location == NULL) {
    LOG_ERROR("%s:%d Malloc failure", __func__, __LINE__);
    return PIPE_NO_SYS_RESOURCES;
  }
  location->pipe_id = pipe_id;
  location->cur_stage_id = stage_id;
  location->def_stage_id = stage_id;
  location->cur_ent_idx = stage_ent_idx;
  location->def_ent_idx = stage_ent_idx;
  location->pending = pending;

  PIPE_MGR_LOCK(&stat_tbl_instance->ent_hdl_loc_mtx);

  map_sts = bf_map_get(&stat_tbl_instance->ent_hdl_loc, key, (void **)&data);
  if (map_sts == BF_MAP_NO_KEY) {
    ent_hdl_loc = (pipe_mgr_stat_mgr_ent_hdl_loc_t *)PIPE_MGR_CALLOC(
        1, sizeof(pipe_mgr_stat_mgr_ent_hdl_loc_t));
    if (ent_hdl_loc == NULL) {
      LOG_ERROR("%s:%d Malloc failure", __func__, __LINE__);
      PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
      return PIPE_NO_SYS_RESOURCES;
    }
    /* Add to the map */
    map_sts =
        bf_map_add(&stat_tbl_instance->ent_hdl_loc, key, (void *)ent_hdl_loc);
    if (map_sts != BF_MAP_OK) {
      LOG_ERROR(
          "%s:%d Error inserting entry hdl 0x%x into the entry hdl"
          " location map",
          __func__,
          __LINE__,
          mat_ent_hdl);
      PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
      return PIPE_UNEXPECTED;
    }
  } else {
    ent_hdl_loc = (pipe_mgr_stat_mgr_ent_hdl_loc_t *)data;
  }
  if (user_count) {
    ent_hdl_loc->stat_data = *user_count;
  }

  /* There can only be a single location for the entry in each pipe.  Any
   * additional locations must be for previous entries which happened to have
   * the same entry handle, these must be delete-in-progress. */
  for (pipe_mgr_stat_ent_location_t *l = ent_hdl_loc->locations; l;
       l = l->next) {
    if (l->pipe_id != pipe_id) continue;
    if (!l->entry_del_in_progress) {
      LOG_ERROR(
          "%s: Dev %d pipe %d tbl %s 0x%x entry %u unexpected location, %d.%d "
          "%d.%d pending %d del-in-prog %d, location %d.%d",
          __func__,
          stat_tbl->device_id,
          pipe_id,
          stat_tbl->name,
          stat_tbl->stat_tbl_hdl,
          mat_ent_hdl,
          l->def_stage_id,
          l->def_ent_idx,
          l->cur_stage_id,
          l->cur_ent_idx,
          l->pending,
          l->entry_del_in_progress,
          stage_id,
          stage_ent_idx);
      PIPE_MGR_STAT_DBGCHK(stat_tbl_instance, l->entry_del_in_progress);
    }
  }
  BF_LIST_DLL_AP(ent_hdl_loc->locations, location, next, prev);

  PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stat_mgr_clear_entry_pending(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    bf_dev_pipe_t pipe_id,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    dev_stage_t stage_id,
    pipe_stat_stage_ent_idx_t stage_ent_idx) {
  bf_map_sts_t map_sts = BF_MAP_OK;
  unsigned long key = mat_ent_hdl;
  void *data = NULL;
  pipe_mgr_stat_mgr_ent_hdl_loc_t *ent_hdl_loc = NULL;
  pipe_mgr_stat_ent_location_t *location = NULL;

  PIPE_MGR_LOCK(&stat_tbl_instance->ent_hdl_loc_mtx);

  map_sts = bf_map_get(&stat_tbl_instance->ent_hdl_loc, key, (void **)&data);
  if (map_sts == BF_MAP_NO_KEY) {
    /* This cannot happen : This is the case where we are "completing" an
     * entry ADD which was deferred. The location information for that entry
     * handle should exist. While the entry add was deferred, if a delete
     * were to happen, we do not clean up the location info, and even the
     * delete is deferred since we detect that there are pending operations
     * and the entry is just marked for delete and will ultimately be deleted
     * when the pending delete task is picked up.
     */
    PIPE_MGR_DBGCHK(0);
    PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
    return PIPE_UNEXPECTED;
  }

  ent_hdl_loc = (pipe_mgr_stat_mgr_ent_hdl_loc_t *)data;
  for (location = ent_hdl_loc->locations; location; location = location->next) {
    if (location->pipe_id != pipe_id) continue;
    if (!location->pending) continue;
    break;
  }
  if (location == NULL) {
    PIPE_MGR_DBGCHK(0);
    PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
    return PIPE_SUCCESS;
  }
  if (location->def_stage_id != stage_id ||
      location->def_ent_idx != stage_ent_idx) {
    LOG_ERROR(
        "%s: Dev %d tbl %s 0x%x entry %u Unexpected pending location, pipe %d "
        "%d.%d/%d.%d, expected %d.%d",
        __func__,
        stat_tbl->device_id,
        stat_tbl->name,
        stat_tbl->stat_tbl_hdl,
        mat_ent_hdl,
        pipe_id,
        location->def_stage_id,
        location->def_ent_idx,
        location->cur_stage_id,
        location->cur_ent_idx,
        stage_id,
        stage_ent_idx);
    PIPE_MGR_DBGCHK(location->def_stage_id == stage_id);
    PIPE_MGR_DBGCHK(location->def_ent_idx == stage_ent_idx);
  }
  location->pending = false;

  pipe_mgr_stat_entry_info_t *stat_ent_info =
      pipe_mgr_stat_mgr_get_ent_idx_info(stat_tbl,
                                         stat_tbl_instance,
                                         location->pipe_id,
                                         location->def_stage_id,
                                         location->def_ent_idx);
  if (stat_ent_info == NULL) {
    LOG_ERROR(
        "%s: Cannot find stat-idx-info, dev %d table %s 0x%x pipe %d stage %d "
        "idx %d",
        __func__,
        stat_tbl->device_id,
        stat_tbl->name,
        stat_tbl->stat_tbl_hdl,
        location->pipe_id,
        location->def_stage_id,
        location->def_ent_idx);
    PIPE_MGR_DBGCHK(stat_ent_info);
    PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
    return PIPE_OBJ_NOT_FOUND;
  }

  pipe_stat_data_t zeros = {0};
  PIPE_MGR_LOCK(&stat_ent_info->mtx);
  stat_ent_info->stat_data = zeros;
  stat_ent_info->user_set_in_progress = location->def_set_in_prog;
  PIPE_MGR_UNLOCK(&stat_ent_info->mtx);

  location->def_set_in_prog = 0;

  PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_stat_mgr_update_ent_hdl_loc(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    bf_dev_pipe_t pipe_id,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    dev_stage_t src_stage_id,
    pipe_stat_stage_ent_idx_t src_stage_idx,
    dev_stage_t dst_stage_id,
    pipe_stat_stage_ent_idx_t dst_stage_idx) {
  bf_map_sts_t map_sts = BF_MAP_OK;
  unsigned long key = mat_ent_hdl;
  void *data = NULL;
  pipe_mgr_stat_mgr_ent_hdl_loc_t *ent_hdl_loc = NULL;

  PIPE_MGR_LOCK(&stat_tbl_instance->ent_hdl_loc_mtx);

  map_sts = bf_map_get(&stat_tbl_instance->ent_hdl_loc, key, &data);
  if (map_sts != BF_MAP_OK) {
    PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }
  ent_hdl_loc = (pipe_mgr_stat_mgr_ent_hdl_loc_t *)data;
  if (!ent_hdl_loc || !ent_hdl_loc->locations) {
    PIPE_MGR_DBGCHK(ent_hdl_loc);
    if (ent_hdl_loc) {
      PIPE_MGR_DBGCHK(ent_hdl_loc->locations);
    }
    PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
    return PIPE_UNEXPECTED;
  }

  /* Find the most recent location on the requested pipe and update it to the
   * requested destination.  Note there is only a single location entry per pipe
   * for the entry handle unless the entry was deleted and the handle was reused
   * for a new entry. */
  pipe_mgr_stat_ent_location_t *location = NULL;
  for (pipe_mgr_stat_ent_location_t *l = ent_hdl_loc->locations; l;
       l = l->next) {
    if (l->pipe_id != pipe_id) continue;

    /* Skip over deleted entries */
    if (l->entry_del_in_progress) {
      PIPE_MGR_DBGCHK(location == NULL);
      continue;
    }
    if (location) {
      LOG_ERROR(
          "%s: Dev %d tbl %s 0x%x entry %u, unexpected active location found, "
          "%d.%d %d.%d pending %d, on pipe %d while moving from %d.%d to %d.%d",
          __func__,
          stat_tbl->device_id,
          stat_tbl->name,
          stat_tbl->stat_tbl_hdl,
          mat_ent_hdl,
          location->cur_stage_id,
          location->cur_ent_idx,
          location->def_stage_id,
          location->def_ent_idx,
          location->pending,
          pipe_id,
          src_stage_id,
          src_stage_idx,
          dst_stage_id,
          dst_stage_idx);
      PIPE_MGR_DBGCHK(location == NULL);
    }
    location = l;
  }
  if (!location) {
    LOG_ERROR(
        "%s: Dev %d tbl %s 0x%x entry %u, no location found on pipe %d while "
        "moving from %d.%d to %d.%d",
        __func__,
        stat_tbl->device_id,
        stat_tbl->name,
        stat_tbl->stat_tbl_hdl,
        mat_ent_hdl,
        pipe_id,
        src_stage_id,
        src_stage_idx,
        dst_stage_id,
        dst_stage_idx);
    PIPE_MGR_DBGCHK(location);
    PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
    return PIPE_UNEXPECTED;
  }
  if (location->cur_stage_id != src_stage_id ||
      location->cur_ent_idx != src_stage_idx) {
    LOG_ERROR(
        "%s: Dev %d tbl %s 0x%x entry %u, unexpected location found, %d.%d "
        "%d.%d pending %d, on pipe %d while moving from %d.%d to %d.%d",
        __func__,
        stat_tbl->device_id,
        stat_tbl->name,
        stat_tbl->stat_tbl_hdl,
        mat_ent_hdl,
        location->cur_stage_id,
        location->cur_ent_idx,
        location->def_stage_id,
        location->def_ent_idx,
        location->pending,
        pipe_id,
        src_stage_id,
        src_stage_idx,
        dst_stage_id,
        dst_stage_idx);
    PIPE_MGR_DBGCHK(location->cur_stage_id == src_stage_id);
    PIPE_MGR_DBGCHK(location->cur_ent_idx == src_stage_idx);
    PIPE_MGR_STAT_DBGCHK(stat_tbl_instance, 0);
  }

  location->cur_stage_id = dst_stage_id;
  location->cur_ent_idx = dst_stage_idx;
  PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
  return PIPE_SUCCESS;
}

/* Caller must hold ent_hdl_loc_mtx */
static pipe_status_t pipe_mgr_stat_mgr_commit_ent_hdl_loc(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    bf_dev_pipe_t pipe_id,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    dev_stage_t src_stage_id,
    pipe_stat_stage_ent_idx_t src_stage_idx,
    dev_stage_t dst_stage_id,
    pipe_stat_stage_ent_idx_t dst_stage_idx,
    bool inline_processing) {
  bf_map_sts_t map_sts = BF_MAP_OK;
  unsigned long key = mat_ent_hdl;
  void *data = NULL;
  pipe_mgr_stat_mgr_ent_hdl_loc_t *ent_hdl_loc = NULL;

  /* Caller must hold ent_hdl_loc_mtx */
  map_sts = bf_map_get(&stat_tbl_instance->ent_hdl_loc, key, &data);
  if (map_sts != BF_MAP_OK) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }
  ent_hdl_loc = (pipe_mgr_stat_mgr_ent_hdl_loc_t *)data;
  if (!ent_hdl_loc || !ent_hdl_loc->locations) {
    PIPE_MGR_DBGCHK(ent_hdl_loc);
    if (ent_hdl_loc) {
      PIPE_MGR_DBGCHK(ent_hdl_loc->locations);
    }
    return PIPE_UNEXPECTED;
  }

  pipe_mgr_stat_ent_location_t *location = NULL;
  for (pipe_mgr_stat_ent_location_t *l = ent_hdl_loc->locations; l;
       l = l->next) {
    if (l->pipe_id != pipe_id) continue;

    /* If this is inline processing there should not be pending operations so
     * add-in-progress or del-in-progress are unexpected. */
    if (inline_processing) {
      PIPE_MGR_DBGCHK(!l->pending);
      PIPE_MGR_DBGCHK(!l->entry_del_in_progress);
    } else {
      /* We are commiting a deferred move, is is possible the entry was deleted
       * after the move so it may be del-in-progress but it cannot be pending
       * since we must've processed the barrier-ack associated with the add
       * before processing this move task. */
      PIPE_MGR_DBGCHK(!l->pending);
    }
    /* The first location on the pipe is the location which is being moved.  In
     * the inline case it must be the only location (no pending operations).  In
     * the deferred case the entry may have been deleted and added again which
     * added an additional location but tasks are processed in order so we
     * cannot see a move for a later version of the entry before a move and
     * delete of an earlier version. */
    location = l;
    break;
  }
  if (!location) {
    LOG_ERROR(
        "%s: Dev %d tbl %s 0x%x entry %u, no location found on pipe %d while "
        "moving from %d.%d to %d.%d",
        __func__,
        stat_tbl->device_id,
        stat_tbl->name,
        stat_tbl->stat_tbl_hdl,
        mat_ent_hdl,
        pipe_id,
        src_stage_id,
        src_stage_idx,
        dst_stage_id,
        dst_stage_idx);
    PIPE_MGR_DBGCHK(location);
    return PIPE_UNEXPECTED;
  }
  if (inline_processing) {
    if (location->cur_stage_id != location->def_stage_id ||
        location->cur_ent_idx != location->def_ent_idx ||
        location->cur_stage_id != src_stage_id ||
        location->cur_ent_idx != src_stage_idx) {
      LOG_ERROR(
          "%s: Dev %d tbl %s 0x%x entry %u, unexpected location on pipe %d "
          "while moving from %d.%d to %d.%d (inline): cur %d.%d def %d.%d "
          "pending %d del-in-prog %d set-in-prog %d pipe %d",
          __func__,
          stat_tbl->device_id,
          stat_tbl->name,
          stat_tbl->stat_tbl_hdl,
          mat_ent_hdl,
          pipe_id,
          src_stage_id,
          src_stage_idx,
          dst_stage_id,
          dst_stage_idx,
          location->cur_stage_id,
          location->cur_ent_idx,
          location->def_stage_id,
          location->def_ent_idx,
          location->pending,
          location->entry_del_in_progress,
          location->def_set_in_prog,
          location->pipe_id);
      PIPE_MGR_DBGCHK(location->cur_stage_id == location->def_stage_id);
      PIPE_MGR_DBGCHK(location->cur_ent_idx == location->def_ent_idx);
      PIPE_MGR_DBGCHK(location->cur_stage_id == src_stage_id);
      PIPE_MGR_DBGCHK(location->cur_ent_idx == src_stage_idx);
      PIPE_MGR_STAT_DBGCHK(stat_tbl_instance, 0);
    }
    location->cur_stage_id = dst_stage_id;
    location->cur_ent_idx = dst_stage_idx;
    location->def_stage_id = dst_stage_id;
    location->def_ent_idx = dst_stage_idx;
  } else {
    if (location->def_stage_id != src_stage_id ||
        location->def_ent_idx != src_stage_idx) {
      LOG_ERROR(
          "%s: Dev %d tbl %s 0x%x entry %u, unexpected location on pipe %d "
          "while moving from %d.%d to %d.%d (deferred): cur %d.%d def %d.%d "
          "pending %d del-in-prog %d set-in-prog %d pipe %d",
          __func__,
          stat_tbl->device_id,
          stat_tbl->name,
          stat_tbl->stat_tbl_hdl,
          mat_ent_hdl,
          pipe_id,
          src_stage_id,
          src_stage_idx,
          dst_stage_id,
          dst_stage_idx,
          location->cur_stage_id,
          location->cur_ent_idx,
          location->def_stage_id,
          location->def_ent_idx,
          location->pending,
          location->entry_del_in_progress,
          location->def_set_in_prog,
          location->pipe_id);
      PIPE_MGR_DBGCHK(location->def_stage_id == src_stage_id);
      PIPE_MGR_DBGCHK(location->def_ent_idx == src_stage_idx);
      PIPE_MGR_STAT_DBGCHK(stat_tbl_instance, 0);
    }
    location->def_stage_id = dst_stage_id;
    location->def_ent_idx = dst_stage_idx;
  }

  return PIPE_SUCCESS;
}

static void pipe_mgr_stat_mgr_mark_entry_del_in_progress(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    bf_dev_pipe_t pipe_id,
    dev_stage_t stage_id,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    pipe_stat_stage_ent_idx_t stage_ent_idx) {
  pipe_mgr_stat_mgr_ent_hdl_loc_t *ent_hdl_loc = NULL;
  pipe_mgr_stat_ent_location_t *location = NULL;
  bf_map_sts_t map_sts = BF_MAP_OK;
  unsigned long key = mat_ent_hdl;
  void *data = NULL;

  PIPE_MGR_LOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
  map_sts = bf_map_get(&stat_tbl_instance->ent_hdl_loc, key, (void **)&data);

  if (map_sts != BF_MAP_OK) {
    PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
    PIPE_MGR_DBGCHK(0);
    return;
  }

  ent_hdl_loc = (pipe_mgr_stat_mgr_ent_hdl_loc_t *)data;
  /* Walk the locations list and find the last location for this pipe; the last
   * location will be the most recent.  Any earlier locations would be from
   * previous entries with the same handle that have been deleted. */
  for (pipe_mgr_stat_ent_location_t *i = ent_hdl_loc->locations; i;
       i = i->next) {
    if (i->pipe_id != pipe_id) continue;
    if (location) {
      /* We've found a matching location AND we had already found a previous
       * match.  The previous match must be from a delete-in-progress entry. */
      if (!location->entry_del_in_progress) {
        LOG_ERROR(
            "%s: Dev %d tbl %s 0x%x pipe %d entry %u, unexpected location [cur "
            "%d.%d def %d.%d pend %d] [cur %d.%d def %d.%d pend %d]",
            __func__,
            stat_tbl->device_id,
            stat_tbl->name,
            stat_tbl->stat_tbl_hdl,
            pipe_id,
            mat_ent_hdl,
            location->cur_stage_id,
            location->cur_ent_idx,
            location->def_stage_id,
            location->def_ent_idx,
            location->pending,
            i->cur_stage_id,
            i->cur_ent_idx,
            i->def_stage_id,
            i->def_ent_idx,
            i->pending);
        PIPE_MGR_DBGCHK(location->entry_del_in_progress);
      }
    }
    location = i;
  }
  if (!location) {
    /* No locations found for the entry?! */
    LOG_ERROR(
        "%s: Dev %d tbl %s 0x%x pipe %d entry %u, no location, expected %d.%d",
        __func__,
        stat_tbl->device_id,
        stat_tbl->name,
        stat_tbl->stat_tbl_hdl,
        pipe_id,
        mat_ent_hdl,
        stage_id,
        stage_ent_idx);
    PIPE_MGR_DBGCHK(location);
    PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
    return;
  }
  if (location->cur_stage_id != stage_id ||
      location->cur_ent_idx != stage_ent_idx) {
    LOG_ERROR(
        "%s: Dev %d tbl %s 0x%x pipe %d entry %u, unexpected location "
        "%d.%d/%d.%d, expected %d.%d",
        __func__,
        stat_tbl->device_id,
        stat_tbl->name,
        stat_tbl->stat_tbl_hdl,
        pipe_id,
        mat_ent_hdl,
        location->cur_stage_id,
        location->cur_ent_idx,
        location->def_stage_id,
        location->def_ent_idx,
        stage_id,
        stage_ent_idx);
  }
  if (location->entry_del_in_progress) {
    LOG_ERROR(
        "%s: Dev %d tbl %s 0x%x pipe %d entry %u at %d.%d/%d.%d already "
        "del-in-prog",
        __func__,
        stat_tbl->device_id,
        stat_tbl->name,
        stat_tbl->stat_tbl_hdl,
        pipe_id,
        mat_ent_hdl,
        location->cur_stage_id,
        location->cur_ent_idx,
        location->def_stage_id,
        location->def_ent_idx);
  }
  location->entry_del_in_progress = true;

  PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
}

static pipe_status_t pipe_mgr_stat_mgr_del_ent_hdl_loc(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    bf_dev_pipe_t pipe_id,
    dev_stage_t stage_id,
    pipe_stat_stage_ent_idx_t stage_ent_idx,
    bool inline_processing) {
  bf_map_sts_t map_sts = BF_MAP_OK;
  unsigned long key = mat_ent_hdl;
  void *data = NULL;

  PIPE_MGR_LOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
  map_sts = bf_map_get(&stat_tbl_instance->ent_hdl_loc, key, &data);
  if (map_sts != BF_MAP_OK) {
    PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
    LOG_ERROR(
        "%s: Dev %d pipe %d tbl %s 0x%x no location data for entry %u, sts %d",
        __func__,
        stat_tbl->device_id,
        pipe_id,
        stat_tbl->name,
        stat_tbl->stat_tbl_hdl,
        mat_ent_hdl,
        map_sts);
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }

  pipe_mgr_stat_mgr_ent_hdl_loc_t *ent_hdl_loc = NULL;
  ent_hdl_loc = (pipe_mgr_stat_mgr_ent_hdl_loc_t *)data;

  /* Search the list of locations for the first location on the pipe, it must be
   * the entry location we are deleting.  If it was for previous entries with
   * the same handle those deletes must have completed by this point since we
   * either received the barrier ack associated with the delete of this entry or
   * because there were no pending barriers at the time the delete was issued.
   */
  pipe_mgr_stat_ent_location_t *location = NULL;
  for (location = ent_hdl_loc->locations; location; location = location->next) {
    if (location->pipe_id != pipe_id) continue;
    break;
  }
  if (!location) {
    PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
    LOG_ERROR("%s: Dev %d pipe %d tbl %s 0x%x no locations for entry %u",
              __func__,
              stat_tbl->device_id,
              pipe_id,
              stat_tbl->name,
              stat_tbl->stat_tbl_hdl,
              mat_ent_hdl);
    PIPE_MGR_STAT_DBGCHK(stat_tbl_instance, 0);
    return PIPE_UNEXPECTED;
  }

  /* The delete should be for the entry at its current location */
  if (location->cur_stage_id != stage_id ||
      location->cur_ent_idx != stage_ent_idx) {
    LOG_ERROR(
        "%s: Dev %d pipe %d tbl %s 0x%x unexpected location for entry %u, "
        "%d.%d, cur %d.%d def %d.%d, pend %d, del-in-prog %d",
        __func__,
        stat_tbl->device_id,
        pipe_id,
        stat_tbl->name,
        stat_tbl->stat_tbl_hdl,
        mat_ent_hdl,
        stage_id,
        stage_ent_idx,
        location->cur_stage_id,
        location->cur_ent_idx,
        location->def_stage_id,
        location->def_ent_idx,
        location->pending,
        location->entry_del_in_progress);
    PIPE_MGR_STAT_DBGCHK(stat_tbl_instance, 0);
  }

  /* If we are coming from a barrier-ack then this is a deferred delete and the
   * entry is expected to be delete-in-progress.  However, if we are coming
   * directly from the table-manager's delete call because there were no
   * outstanding barrier ACKs we don't expect the entry to be
   * delete-in-progress. */
  if (location->entry_del_in_progress == inline_processing) {
    LOG_ERROR(
        "%s: Dev %d pipe %d tbl %s 0x%x unexpected location state for entry "
        "%u, %d.%d, cur %d.%d def %d.%d, pend %d, del-in-prog %d",
        __func__,
        stat_tbl->device_id,
        pipe_id,
        stat_tbl->name,
        stat_tbl->stat_tbl_hdl,
        mat_ent_hdl,
        stage_id,
        stage_ent_idx,
        location->cur_stage_id,
        location->cur_ent_idx,
        location->def_stage_id,
        location->def_ent_idx,
        location->pending,
        location->entry_del_in_progress);
    PIPE_MGR_STAT_DBGCHK(stat_tbl_instance, 0);
  }

  BF_LIST_DLL_REM(ent_hdl_loc->locations, location, next, prev);
  PIPE_MGR_FREE(location);
  if (ent_hdl_loc->locations == NULL) {
    bf_map_rmv(&stat_tbl_instance->ent_hdl_loc, key);
    PIPE_MGR_FREE(ent_hdl_loc);
  }

  PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);

  return PIPE_SUCCESS;
}

void pipe_mgr_stat_mgr_reset_ent_write_in_progress(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    bf_dev_pipe_t pipe_id,
    dev_stage_t stage_id,
    pipe_stat_stage_ent_idx_t stage_ent_idx) {
  pipe_mgr_stat_entry_info_t *stat_ent_info = NULL;

  if (!stat_tbl || !stat_tbl_instance) {
    LOG_ERROR("%s:%d Null pointer arguments passed", __func__, __LINE__);
    return;
  }

  stat_ent_info = pipe_mgr_stat_mgr_get_ent_idx_info(
      stat_tbl, stat_tbl_instance, pipe_id, stage_id, stage_ent_idx);
  if (!stat_ent_info) {
    LOG_ERROR(
        "%s : Stat table stage entry info not found. Tbl 0x%x, pipe id %x, "
        "stage id %d, stage entry index %d",
        __func__,
        stat_tbl->stat_tbl_hdl,
        pipe_id,
        stage_id,
        stage_ent_idx);
    return;
  }

  PIPE_MGR_LOCK(&stat_ent_info->mtx);
  if (stat_ent_info->user_set_in_progress) {
    stat_ent_info->user_set_in_progress--;
  }
  PIPE_MGR_UNLOCK(&stat_ent_info->mtx);

  return;
}

static pipe_status_t pipe_mgr_stat_mgr_update_ent_hdl_count(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    pipe_mat_ent_hdl_t ent_hdl,
    pipe_stat_data_t *stat_data,
    bool set_in_prog) {
  pipe_status_t status = PIPE_SUCCESS;
  bf_map_sts_t map_sts = BF_MAP_OK;
  unsigned long key = ent_hdl;
  void *data = NULL;
  pipe_mgr_stat_mgr_ent_hdl_loc_t *ent_hdl_loc = NULL;
  pipe_stat_data_t local_stat_data = {0};

  PIPE_MGR_LOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
  map_sts = bf_map_get(&stat_tbl_instance->ent_hdl_loc, key, &data);
  if (map_sts != BF_MAP_OK) {
    LOG_ERROR(
        "%s:%d Failed to find entry location for entry handle %d in "
        "stat table 0x%x",
        __func__,
        __LINE__,
        ent_hdl,
        stat_tbl->stat_tbl_hdl);
    PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
    return PIPE_OBJ_NOT_FOUND;
  }

  ent_hdl_loc = (pipe_mgr_stat_mgr_ent_hdl_loc_t *)data;
  /* First update the user-set count on the entry. */
  ent_hdl_loc->stat_data = *stat_data;

  /* Then go over its locations and zero the accumulated counts.  This should
   * only be done on the non-pending and non-delete-in-progress entries.  It
   * also must be done on the deferred location since that is where stats_mgr is
   * currently tracking the entry. */
  pipe_mgr_stat_ent_location_t *traverser = ent_hdl_loc->locations;
  for (pipe_mgr_stat_ent_location_t *traverser = ent_hdl_loc->locations;
       traverser != NULL;
       traverser = traverser->next) {
    /* Skip deleted entries. */
    if (traverser->entry_del_in_progress) {
      continue;
    }
    /* If the entry is committed, clear its count. */
    if (!traverser->pending) {
      status = pipe_mgr_stat_mgr_set_ent_idx_count(stat_tbl,
                                                   stat_tbl_instance,
                                                   set_in_prog,
                                                   traverser->pipe_id,
                                                   traverser->def_stage_id,
                                                   traverser->def_ent_idx,
                                                   &local_stat_data);
      if (status != PIPE_SUCCESS) {
        PIPE_MGR_DBGCHK(0);
        break;
      }
    } else {
      /* The entry is waiting for a deferred add to complete so it doesn't have
       * a hardware location yet. */
      if (set_in_prog) traverser->def_set_in_prog++;
    }
  }
  PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);

  return status;
}

static pipe_status_t pipe_mgr_stat_mgr_get_ent_hdl_count(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    pipe_mat_ent_hdl_t ent_hdl,
    pipe_stat_data_t *stat_data) {
  pipe_status_t status = PIPE_SUCCESS;
  pipe_mgr_stat_mgr_ent_hdl_loc_t *ent_hdl_loc = NULL;
  bf_map_sts_t map_sts = BF_MAP_OK;
  unsigned long key = ent_hdl;
  void *data = NULL;
  pipe_stat_data_t local_stat_data = {0};

  PIPE_MGR_LOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
  map_sts = bf_map_get(&stat_tbl_instance->ent_hdl_loc, key, &data);

  if (map_sts != BF_MAP_OK) {
    PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
    return PIPE_OBJ_NOT_FOUND;
  }

  ent_hdl_loc = (pipe_mgr_stat_mgr_ent_hdl_loc_t *)data;
  for (pipe_mgr_stat_ent_location_t *l = ent_hdl_loc->locations; l;
       l = l->next) {
    if (l->pending || l->entry_del_in_progress) continue;
    status = pipe_mgr_stat_mgr_get_ent_idx_count(stat_tbl,
                                                 stat_tbl_instance,
                                                 l->pipe_id,
                                                 l->def_stage_id,
                                                 l->def_ent_idx,
                                                 &local_stat_data);
    if (status != PIPE_SUCCESS) {
      LOG_ERROR(
          "%s: Dev %d tbl %s 0x%x Error %s getting counts for entry %u in pipe "
          "id %d at %d.%d",
          __func__,
          stat_tbl->device_id,
          stat_tbl->name,
          stat_tbl->stat_tbl_hdl,
          pipe_str_err(status),
          ent_hdl,
          l->pipe_id,
          l->def_stage_id,
          l->def_ent_idx);
      PIPE_MGR_DBGCHK(status == PIPE_SUCCESS);
      PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);
      return status;
    }
    stat_data->packets += local_stat_data.packets;
    stat_data->bytes += local_stat_data.bytes;
  }
  /* Don't forget to add in the user-set count as they may be non-zero. */
  stat_data->packets += ent_hdl_loc->stat_data.packets;
  stat_data->bytes += ent_hdl_loc->stat_data.bytes;

  PIPE_MGR_UNLOCK(&stat_tbl_instance->ent_hdl_loc_mtx);

  return PIPE_SUCCESS;
}

void pipe_mgr_stat_mgr_update_tbl_hdl(bf_dev_id_t dev_id,
                                      bf_dev_pipe_t pipe_id,
                                      dev_stage_t stage_id,
                                      uint8_t ltbl_id,
                                      pipe_mgr_stat_tbl_t *stat_tbl) {
  unsigned long k = pipe_id;
  k = k << 8 | stage_id;
  k = k << 8 | ltbl_id;
  bf_map_sts_t rc = pipe_mgr_stat_tbl_hdls_map_add(dev_id, k, (void *)stat_tbl);
  PIPE_MGR_DBGCHK(rc == BF_MAP_OK);
}

pipe_status_t pipe_mgr_stat_mgr_decode_virt_addr(
    bf_dev_id_t device_id,
    pipe_stat_tbl_hdl_t stat_tbl_hdl,
    bf_dev_pipe_t pipe_id,
    uint8_t stage_id,
    rmt_virt_addr_t virt_addr,
    bool *pfe,
    bool *pfe_defaulted,
    pipe_stat_ent_idx_t *stats_idx) {
  /* This expects a full 20-bit stats address to be passed */
  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  pipe_mgr_stat_tbl_stage_info_t *stat_tbl_stage_info = NULL;
  pipe_mgr_stat_ram_alloc_info_t *stat_ram_alloc_info = NULL;
  uint8_t stats_ram_vpn_shift = 0;
  uint8_t subword = 0;
  uint8_t num_entries_per_line = 0;
  uint8_t num_ram_line_bits = log2(TOF_SRAM_UNIT_DEPTH);
  uint8_t subword_shift = 0;
  uint8_t num_subword_bits = 0;
  uint32_t ram_line_num = 0;
  uint32_t num_entries_per_wide_word_blk = 0;

  stat_tbl = pipe_mgr_stat_tbl_get(device_id, stat_tbl_hdl);
  if (stat_tbl == NULL) {
    LOG_ERROR("%s:%d Stat tbl for device id %d, tbl hdl 0x%x not found",
              __func__,
              __LINE__,
              device_id,
              stat_tbl_hdl);
    return PIPE_OBJ_NOT_FOUND;
  }
  stat_tbl_stage_info =
      pipe_mgr_stat_mgr_get_stage_info(stat_tbl, pipe_id, stage_id);
  if (stat_tbl_stage_info == NULL) {
    LOG_ERROR(
        "%s:%d Stat tbl stage info for pipe id %d, stage id %d, tbl hdl 0x%x, "
        "device id %d not found",
        __func__,
        __LINE__,
        pipe_id,
        stage_id,
        stat_tbl_hdl,
        device_id);
    return PIPE_OBJ_NOT_FOUND;
  }
  if (stat_tbl->enable_per_flow_enable) {
    *pfe_defaulted = false;
  } else {
    *pfe_defaulted = true;
  }
  stat_ram_alloc_info = stat_tbl_stage_info->stat_ram_alloc_info;
  num_entries_per_line = stat_tbl_stage_info->num_entries_per_line;

  /* Based on the format of the table, decompose the address */
  switch (num_entries_per_line) {
    case 6:
      /* Currently not used */
      PIPE_MGR_DBGCHK(0);
      break;
    case 4:
      num_subword_bits = 2;
      subword_shift = 1;
      break;
    case 3:
      /* Currently not used */
      PIPE_MGR_DBGCHK(0);
      num_subword_bits = 2;
      subword_shift = 0;
      break;
    case 2:
      num_subword_bits = 1;
      subword_shift = 2;
      break;
    case 1:
      num_subword_bits = 0;
      subword_shift = 3;
      break;
    default:
      PIPE_MGR_DBGCHK(0);
  }
  /* Now, based on the VPN, figure out the wide word blk idx */
  unsigned wide_word_blk_idx = 0;
  stats_ram_vpn_shift = TOF_STATS_RAM_NUM_SUBWORD_BITS + num_ram_line_bits;
  vpn_id_t vpn = (virt_addr >> stats_ram_vpn_shift) &
                 ((1 << TOF_STATS_RAM_NUM_VPN_BITS) - 1);
  for (wide_word_blk_idx = 0;
       wide_word_blk_idx < stat_ram_alloc_info->num_wide_word_blks;
       wide_word_blk_idx++) {
    if (vpn == stat_ram_alloc_info->tbl_word_blk[wide_word_blk_idx].vpn_id[0]) {
      break;
    }
  }
  if (wide_word_blk_idx == stat_ram_alloc_info->num_wide_word_blks) {
    LOG_ERROR(
        "%s:%d VPN %d invalid for stat tbl 0x%x, pipe id %d, stage id %d, "
        "device id %d",
        __func__,
        __LINE__,
        vpn,
        stat_tbl_hdl,
        pipe_id,
        stage_id,
        device_id);
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }
  subword = (virt_addr >> subword_shift) & ((1 << num_subword_bits) - 1);
  num_entries_per_wide_word_blk = num_entries_per_line * TOF_SRAM_UNIT_DEPTH;
  ram_line_num = (virt_addr >> (subword_shift + num_subword_bits)) &
                 ((1 << num_ram_line_bits) - 1);
  *stats_idx = ((ram_line_num * num_entries_per_line) + subword) +
               (wide_word_blk_idx * num_entries_per_wide_word_blk);

  *pfe =
      (virt_addr >> (stats_ram_vpn_shift + TOF_STATS_RAM_NUM_VPN_BITS)) & 0x1;
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stat_log_state(bf_dev_id_t dev_id,
                                      pipe_stat_tbl_hdl_t tbl_hdl,
                                      cJSON *stat_tbls) {
  bf_map_sts_t st;
  pipe_mgr_stat_tbl_t *stat_info = NULL;
  pipe_mgr_stat_tbl_instance_t *stat_instance = NULL;
  pipe_mgr_stat_tbl_stage_info_t *stage_info = NULL;
  pipe_stat_data_t stat_data;
  pipe_stat_data_t *stat_data_p;
  unsigned long ent_hdl;
  pipe_mgr_stat_mgr_ent_hdl_loc_t *ent_loc_data;
  pipe_mgr_stat_ent_location_t *ent_loc;
  int pipe_id = -1;
  uint32_t pipe_idx = 0;
  uint32_t stage_idx = 0;
  uint32_t ent_idx = 0;
  bool pipe_init_counts_saved = false;
  cJSON *stat_tbl, *stat_insts, *stat_inst;
  cJSON *stat_stgs, *stat_stg, *stat_ents, *stat_ent;
  cJSON *ent_hdl_locs, *ent_hdl_loc, *locations, *location;
  cJSON *ent_idx_counts, *ent_idx_count;

  stat_info = pipe_mgr_stat_tbl_get(dev_id, tbl_hdl);
  if (stat_info == NULL) {
    LOG_ERROR("%s:%d Stat table with handle 0x%x not found",
              __func__,
              __LINE__,
              tbl_hdl);
    return PIPE_OBJ_NOT_FOUND;
  }

  cJSON_AddItemToArray(stat_tbls, stat_tbl = cJSON_CreateObject());
  cJSON_AddStringToObject(stat_tbl, "name", stat_info->name);
  cJSON_AddNumberToObject(stat_tbl, "handle", tbl_hdl);
  cJSON_AddBoolToObject(stat_tbl, "symmetric", stat_info->symmetric);
  cJSON_AddItemToObject(
      stat_tbl, "stat_insts", stat_insts = cJSON_CreateArray());

  pipe_id = PIPE_BITMAP_GET_NEXT_BIT(&stat_info->pipe_bmp, pipe_id);
  while (pipe_id != -1) {
    cJSON_AddItemToArray(stat_insts, stat_inst = cJSON_CreateObject());
    cJSON_AddNumberToObject(stat_inst, "pipe_id", pipe_id);
    stat_instance = &(stat_info->stat_tbl_instances[pipe_idx]);
    if (!stat_info->symmetric) {
      PIPE_MGR_DBGCHK(pipe_id == (int)stat_instance->pipe_id);
      pipe_idx++;
    }

    cJSON_AddItemToObject(
        stat_inst, "stage_tbls", stat_stgs = cJSON_CreateArray());
    for (stage_idx = 0; stage_idx < stat_instance->num_stages; stage_idx++) {
      stage_info = &(stat_instance->stat_tbl_stage_info[stage_idx]);
      cJSON_AddItemToArray(stat_stgs, stat_stg = cJSON_CreateObject());
      cJSON_AddNumberToObject(stat_stg, "stage_id", stage_info->stage_id);
      cJSON_AddItemToObject(
          stat_stg, "stat_ents", stat_ents = cJSON_CreateArray());
      for (ent_idx = 0; ent_idx < stage_info->num_entries; ent_idx++) {
        if (PIPE_SUCCESS ==
            pipe_mgr_stat_mgr_get_ent_idx_count(stat_info,
                                                stat_instance,
                                                pipe_id,
                                                stage_info->stage_id,
                                                ent_idx,
                                                &stat_data)) {
          if (stat_data.bytes > 0 || stat_data.packets > 0) {
            cJSON_AddItemToArray(stat_ents, stat_ent = cJSON_CreateObject());
            cJSON_AddNumberToObject(stat_ent, "ent_idx", ent_idx);
            cJSON_AddNumberToObject(stat_ent, "bytes", stat_data.bytes);
            cJSON_AddNumberToObject(stat_ent, "packets", stat_data.packets);
          }
        }
      }
    }

    if (!stat_info->symmetric || !pipe_init_counts_saved) {
      if (stat_info->ref_type == PIPE_TBL_REF_TYPE_DIRECT) {
        cJSON_AddItemToObject(
            stat_inst, "ent_hdl_locs", ent_hdl_locs = cJSON_CreateArray());
        st = bf_map_get_first(
            &stat_instance->ent_hdl_loc, &ent_hdl, (void **)&ent_loc_data);
        while (st == BF_MAP_OK) {
          cJSON_AddItemToArray(ent_hdl_locs,
                               ent_hdl_loc = cJSON_CreateObject());
          cJSON_AddNumberToObject(ent_hdl_loc, "ent_hdl", ent_hdl);
          cJSON_AddItemToObject(
              ent_hdl_loc, "locations", locations = cJSON_CreateArray());
          for (ent_loc = ent_loc_data->locations; ent_loc;
               ent_loc = ent_loc->next) {
            if (ent_loc->pipe_id == (bf_dev_pipe_t)pipe_id) {
              cJSON_AddItemToArray(locations, location = cJSON_CreateObject());
              cJSON_AddNumberToObject(
                  location, "stage_id", ent_loc->cur_stage_id);
              cJSON_AddNumberToObject(
                  location, "ent_idx", ent_loc->cur_ent_idx);
            }
          }
          cJSON_AddNumberToObject(
              ent_hdl_loc, "bytes", ent_loc_data->stat_data.bytes);
          cJSON_AddNumberToObject(
              ent_hdl_loc, "packets", ent_loc_data->stat_data.packets);

          st = bf_map_get_next(
              &stat_instance->ent_hdl_loc, &ent_hdl, (void **)&ent_loc_data);
        }
      } else {
        cJSON_AddItemToObject(
            stat_inst, "ent_idx_counts", ent_idx_counts = cJSON_CreateArray());
        for (ent_idx = 0; ent_idx < stat_instance->num_entries; ent_idx++) {
          stat_data_p = &(stat_instance->user_idx_count[ent_idx]);
          if (stat_data_p->bytes > 0 || stat_data_p->packets > 0) {
            cJSON_AddItemToArray(ent_idx_counts,
                                 ent_idx_count = cJSON_CreateObject());
            cJSON_AddNumberToObject(ent_idx_count, "ent_idx", ent_idx);
            cJSON_AddNumberToObject(ent_idx_count, "bytes", stat_data_p->bytes);
            cJSON_AddNumberToObject(
                ent_idx_count, "packets", stat_data_p->packets);
          }
        }
      }
      pipe_init_counts_saved = true;
    }

    pipe_id = PIPE_BITMAP_GET_NEXT_BIT(&stat_info->pipe_bmp, pipe_id);
  }

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stat_restore_state(bf_dev_id_t dev_id, cJSON *stat_tbl) {
  pipe_status_t sts;
  bf_map_sts_t st;
  pipe_mgr_stat_tbl_t *stat_info = NULL;
  pipe_mgr_stat_tbl_instance_t *stat_instance = NULL;
  pipe_mgr_stat_tbl_stage_info_t *stage_info = NULL;
  pipe_stat_data_t stat_data;
  pipe_stat_tbl_hdl_t tbl_hdl;
  unsigned long ent_hdl;
  pipe_mgr_stat_mgr_ent_hdl_loc_t *ent_loc;
  int pipe_id = -1;
  uint32_t pipe_idx = 0;
  uint32_t stage_id = 0;
  uint32_t stage_idx = 0;
  uint32_t ent_idx = 0;
  bool symmetric = false;
  bool pipe_init_counts_saved = false;
  cJSON *stat_insts, *stat_inst;
  cJSON *stat_stgs, *stat_stg, *stat_ents, *stat_ent;
  cJSON *ent_hdl_locs, *ent_hdl_loc, *locations, *location;
  cJSON *ent_idx_counts, *ent_idx_count;
  scope_pipes_t scopes = 0xf;

  tbl_hdl = cJSON_GetObjectItem(stat_tbl, "handle")->valueint;
  stat_info = pipe_mgr_stat_tbl_get(dev_id, tbl_hdl);
  if (stat_info == NULL) {
    LOG_ERROR("%s:%d Stat table with handle 0x%x not found",
              __func__,
              __LINE__,
              tbl_hdl);
    return PIPE_OBJ_NOT_FOUND;
  }
  symmetric = (cJSON_GetObjectItem(stat_tbl, "symmetric")->type == cJSON_True);
  if (symmetric != stat_info->symmetric) {
    sts = pipe_mgr_stat_tbl_set_symmetric_mode(
        dev_id, tbl_hdl, symmetric, 1, &scopes);
    if (sts != PIPE_SUCCESS) {
      LOG_ERROR("Failed to set %ssymmetric mode on dev %u, stat tbl 0x%x",
                symmetric ? "" : "non-",
                dev_id,
                tbl_hdl);
      return sts;
    }
  }

  stat_insts = cJSON_GetObjectItem(stat_tbl, "stat_insts");
  for (stat_inst = stat_insts->child; stat_inst; stat_inst = stat_inst->next) {
    pipe_id = cJSON_GetObjectItem(stat_inst, "pipe_id")->valueint;
    stat_instance = &(stat_info->stat_tbl_instances[pipe_idx]);
    if (!stat_info->symmetric) {
      PIPE_MGR_DBGCHK(pipe_id == (int)stat_instance->pipe_id);
      pipe_idx++;
    }

    stat_stgs = cJSON_GetObjectItem(stat_inst, "stage_tbls");
    for (stat_stg = stat_stgs->child, stage_idx = 0; stat_stg;
         stat_stg = stat_stg->next, stage_idx++) {
      stage_info = &(stat_instance->stat_tbl_stage_info[stage_idx]);
      stat_ents = cJSON_GetObjectItem(stat_stg, "stat_ents");
      for (stat_ent = stat_ents->child; stat_ent; stat_ent = stat_ent->next) {
        ent_idx = cJSON_GetObjectItem(stat_ent, "ent_idx")->valuedouble;
        stat_data.bytes = cJSON_GetObjectItem(stat_ent, "bytes")->valuedouble;
        stat_data.packets =
            cJSON_GetObjectItem(stat_ent, "packets")->valuedouble;
        pipe_mgr_stat_mgr_set_ent_idx_count(stat_info,
                                            stat_instance,
                                            false,
                                            pipe_id,
                                            stage_info->stage_id,
                                            ent_idx,
                                            &stat_data);
      }
    }

    /* Only perform the following once for the symmetric case
     * (only one table instance).
     */
    if (!stat_info->symmetric || !pipe_init_counts_saved) {
      if (stat_info->ref_type == PIPE_TBL_REF_TYPE_DIRECT) {
        ent_hdl_locs = cJSON_GetObjectItem(stat_inst, "ent_hdl_locs");
        for (ent_hdl_loc = ent_hdl_locs->child; ent_hdl_loc;
             ent_hdl_loc = ent_hdl_loc->next) {
          ent_hdl = cJSON_GetObjectItem(ent_hdl_loc, "ent_hdl")->valuedouble;
          locations = cJSON_GetObjectItem(ent_hdl_loc, "locations");
          for (location = locations->child; location;
               location = location->next) {
            stage_id = cJSON_GetObjectItem(location, "stage_id")->valueint;
            ent_idx = cJSON_GetObjectItem(location, "ent_idx")->valuedouble;
            pipe_mgr_stat_mgr_add_ent_hdl_loc(stat_info,
                                              stat_instance,
                                              ent_hdl,
                                              pipe_id,
                                              stage_id,
                                              ent_idx,
                                              false,
                                              NULL);
            if (stat_info->symmetric) {
              cJSON *other_inst;
              for (other_inst = stat_inst->next; other_inst;
                   other_inst = other_inst->next) {
                pipe_mgr_stat_mgr_add_ent_hdl_loc(
                    stat_info,
                    stat_instance,
                    ent_hdl,
                    cJSON_GetObjectItem(other_inst, "pipe_id")->valueint,
                    stage_id,
                    ent_idx,
                    false,
                    NULL);
              }
            }
          }
          st = bf_map_get(
              &stat_instance->ent_hdl_loc, ent_hdl, (void **)&ent_loc);
          if (st == BF_MAP_NO_KEY) {
            ent_loc = PIPE_MGR_MALLOC(sizeof(pipe_mgr_stat_mgr_ent_hdl_loc_t));
            ent_loc->locations = NULL;
          }
          ent_loc->stat_data.bytes =
              cJSON_GetObjectItem(ent_hdl_loc, "bytes")->valuedouble;
          ent_loc->stat_data.packets =
              cJSON_GetObjectItem(ent_hdl_loc, "packets")->valuedouble;
        }
      } else {
        ent_idx_counts = cJSON_GetObjectItem(stat_inst, "ent_idx_counts");
        for (ent_idx_count = ent_idx_counts->child; ent_idx_count;
             ent_idx_count = ent_idx_count->next) {
          ent_idx = cJSON_GetObjectItem(ent_idx_count, "ent_idx")->valuedouble;
          stat_instance->user_idx_count[ent_idx].bytes =
              cJSON_GetObjectItem(ent_idx_count, "bytes")->valuedouble;
          stat_instance->user_idx_count[ent_idx].packets =
              cJSON_GetObjectItem(ent_idx_count, "packets")->valuedouble;
        }
      }
      pipe_init_counts_saved = true;
    }
  }

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stat_mgr_reset_table(pipe_sess_hdl_t sess_hdl,
                                            bf_dev_id_t device_id,
                                            pipe_stat_tbl_hdl_t stat_tbl_hdl) {
  pipe_mgr_stat_tbl_t *stat_tbl =
      pipe_mgr_stat_tbl_get(device_id, stat_tbl_hdl);
  if (stat_tbl == NULL) {
    LOG_ERROR("%s:%d Stat table for tbl hdl 0x%x, device id %d not found",
              __func__,
              __LINE__,
              stat_tbl_hdl,
              device_id);
    return PIPE_OBJ_NOT_FOUND;
  }
  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance = NULL;
  pipe_mgr_stat_tbl_stage_info_t *stat_tbl_stage_info = NULL;
  unsigned i = 0, j = 0;
  uint32_t idx = 0;
  rmt_ram_line_t ram_line;
  rmt_virt_addr_t ent_virt_addr;
  pipe_stat_data_t stat_data = {0};
  pipe_instr_set_memdata_v_t instr;
  uint8_t subword = 0;
  pipe_status_t status = PIPE_SUCCESS;
  for (i = 0; i < stat_tbl->num_instances; i++) {
    stat_tbl_instance = &stat_tbl->stat_tbl_instances[i];
    for (j = 0; j < stat_tbl_instance->num_stages; j++) {
      stat_tbl_stage_info = &stat_tbl_instance->stat_tbl_stage_info[j];
      for (idx = 0; idx < stat_tbl_stage_info->num_entries; idx++) {
        dev_stage_t stage_id = stat_tbl_stage_info->stage_id;
        PIPE_MGR_MEMSET(ram_line, 0, sizeof(rmt_ram_line_t));
        subword = idx % stat_tbl_stage_info->num_entries_per_line;
        pipe_mgr_stat_mgr_encode_stat_entry(stat_tbl,
                                            stat_tbl_stage_info,
                                            stage_id,
                                            subword,
                                            stat_data.bytes,
                                            stat_data.packets,
                                            &ram_line);
        ent_virt_addr =
            pipe_mgr_stat_mgr_compute_ent_virt_addr(stat_tbl_stage_info, idx);
        construct_instr_set_v_memdata(device_id,
                                      &instr,
                                      (uint8_t *)ram_line,
                                      TOF_SRAM_UNIT_WIDTH / 8,
                                      stat_tbl_stage_info->stage_table_handle,
                                      pipe_virt_mem_type_stat,
                                      ent_virt_addr);

        /* Do an ilist add */
        status = pipe_mgr_drv_ilist_add(&sess_hdl,
                                        stat_tbl->dev_info,
                                        &stat_tbl_instance->pipe_bmp,
                                        stage_id,
                                        (uint8_t *)&instr,
                                        sizeof(pipe_instr_set_memdata_v_t));

        if (status != PIPE_SUCCESS) {
          LOG_ERROR("%s:%d Error in ilist add, err %s",
                    __func__,
                    __LINE__,
                    pipe_str_err(status));
          return status;
        }
      }
    }
  }
  return PIPE_SUCCESS;
}

bool pipe_mgr_stat_tbl_is_indirect(bf_dev_id_t device_id,
                                   pipe_stat_tbl_hdl_t stat_tbl_hdl) {
  pipe_mgr_stat_tbl_t *stat_tbl =
      pipe_mgr_stat_tbl_get(device_id, stat_tbl_hdl);
  if (stat_tbl == NULL) {
    return false;
  }
  if (stat_tbl->ref_type == PIPE_TBL_REF_TYPE_INDIRECT) {
    return true;
  }
  return false;
}

pipe_status_t pipe_mgr_stat_tbl_sbe_correct(bf_dev_id_t dev_id,
                                            bf_dev_pipe_t log_pipe_id,
                                            dev_stage_t stage_id,
                                            pipe_stat_tbl_hdl_t tbl_hdl,
                                            int line) {
  /* If the device is locked we cannot correct anything since the software state
   * may not agree with the hardware state. */
  if (pipe_mgr_is_device_locked(dev_id)) return PIPE_SUCCESS;

  pipe_mgr_stat_tbl_t *stat_tbl = pipe_mgr_stat_tbl_get(dev_id, tbl_hdl);
  if (stat_tbl == NULL) {
    LOG_ERROR("%s:%d Stats table not found for device id %d, tbl hdl 0x%x",
              __func__,
              __LINE__,
              dev_id,
              tbl_hdl);
    return PIPE_OBJ_NOT_FOUND;
  }
  rmt_dev_info_t *dev_info = stat_tbl->dev_info;

  /* Pipe id is always a single pipe so when getting the instance special case
   * symmetric tables. */
  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance =
      stat_tbl->symmetric
          ? stat_tbl->stat_tbl_instances
          : pipe_mgr_stat_tbl_get_instance(stat_tbl, log_pipe_id);
  if (stat_tbl_instance == NULL) {
    LOG_ERROR(
        "%s:%d : Stat table instance for tbl 0x%x, device id %d, pipe id %d"
        " not found",
        __func__,
        __LINE__,
        tbl_hdl,
        dev_id,
        log_pipe_id);
    return PIPE_OBJ_NOT_FOUND;
  }
  pipe_mgr_stat_tbl_stage_info_t *stat_tbl_stage_info = NULL;
  for (int i = 0; i < stat_tbl_instance->num_stages; ++i) {
    if (stat_tbl_instance->stat_tbl_stage_info[i].stage_id == stage_id) {
      stat_tbl_stage_info = &stat_tbl_instance->stat_tbl_stage_info[i];
      break;
    }
  }
  if (stat_tbl_stage_info == NULL) {
    LOG_ERROR("%s:%d Dev %d stat table 0x%x pipe %d stage id %d info not found",
              __func__,
              __LINE__,
              dev_id,
              tbl_hdl,
              log_pipe_id,
              stage_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  bf_dev_pipe_t phy_pipe_id;
  pipe_mgr_map_pipe_id_log_to_phy(
      stat_tbl->dev_info, log_pipe_id, &phy_pipe_id);
  bf_subdev_id_t subdev = pipe_mgr_subdev_id_from_pipe(phy_pipe_id);

  /* Issue a virtual read for every VPN on the ram line.  Virtual accesses will
   * always read-modify-write the unit RAM and by accesses every VPN all the
   * map RAMs will update as well. */
  for (unsigned int i = 0;
       i < stat_tbl_stage_info->stat_ram_alloc_info->num_wide_word_blks;
       ++i) {
    int subword = 0;
    vpn_id_t vpn =
        stat_tbl_stage_info->stat_ram_alloc_info->tbl_word_blk[i].vpn_id[0];
    rmt_virt_addr_t low_vir_addr = (vpn << TOF_STATS_RAM_VPN_SHIFT) |
                                   (line << TOF_STATS_RAM_NUM_SUBWORD_BITS) |
                                   (subword);

    pipe_full_virt_addr_t vaddr;
    vaddr.addr = 0;
    construct_full_virt_addr(dev_info,
                             &vaddr,
                             stat_tbl_stage_info->stage_table_handle,
                             pipe_virt_mem_type_stat,
                             low_vir_addr,
                             phy_pipe_id,
                             stage_id);
    LOG_DBG(
        "Dev %d pipe %d stage %d lt %d stat tbl 0x%x SBE correct vpn %d line "
        "%d virt 0x%" PRIx64,
        dev_id,
        log_pipe_id,
        stage_id,
        stat_tbl_stage_info->stage_table_handle,
        tbl_hdl,
        vpn,
        line,
        vaddr.addr);
    uint64_t dont, care;
    lld_subdev_ind_read(dev_id, subdev, vaddr.addr, &dont, &care);
  }

  return PIPE_SUCCESS;
}
