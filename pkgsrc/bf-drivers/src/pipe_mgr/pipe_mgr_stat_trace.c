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

#ifdef __sun
#include <illumos.h>
#endif
#include "pipe_mgr_stat_trace.h"

void pipe_mgr_stat_mgr_trace_add(pipe_mgr_stat_tbl_t *stat_tbl,
                                 pipe_mgr_stat_tbl_instance_t *inst,
                                 pipe_mat_ent_hdl_t ent_hdl,
                                 dev_stage_t stage_id,
                                 pipe_stat_stage_ent_idx_t stage_idx,
                                 uint32_t deferred) {
  PIPE_MGR_LOCK(&inst->trace_mtx);
  unsigned int i = inst->trace_idx & PIPE_MGR_STAT_TRACE_MASK;
  ++inst->trace_idx;
  clock_gettime(CLOCK_MONOTONIC_RAW, &inst->trace[i].ts);
  inst->trace[i].op = BF_STAT_TRACE_TYPE_ADD;
  inst->trace[i].u.dir_api.ent_hdl = ent_hdl;
  inst->trace[i].u.dir_api.stage_id = stage_id;
  inst->trace[i].u.dir_api.stage_idx = stage_idx;
  inst->trace[i].u.dir_api.deferred = deferred;
  PIPE_MGR_UNLOCK(&inst->trace_mtx);
  LOG_DBG("%s (0x%x) Add: inst_pipe %X entry %u at %d.%d deferred 0x%x",
          stat_tbl->name,
          stat_tbl->stat_tbl_hdl,
          inst->pipe_id,
          ent_hdl,
          stage_id,
          stage_idx,
          deferred);
}

void pipe_mgr_stat_mgr_trace_def_add(pipe_mgr_stat_tbl_t *stat_tbl,
                                     pipe_mgr_stat_tbl_instance_t *inst,
                                     pipe_mat_ent_hdl_t ent_hdl,
                                     dev_stage_t stage_id,
                                     pipe_stat_stage_ent_idx_t stage_idx,
                                     bf_dev_pipe_t pipe_id) {
  PIPE_MGR_LOCK(&inst->trace_mtx);
  unsigned int i = inst->trace_idx & PIPE_MGR_STAT_TRACE_MASK;
  ++inst->trace_idx;
  clock_gettime(CLOCK_MONOTONIC_RAW, &inst->trace[i].ts);
  inst->trace[i].op = BF_STAT_TRACE_TYPE_DEF_ADD;
  inst->trace[i].u.task.ent_hdl = ent_hdl;
  inst->trace[i].u.task.stage_id = stage_id;
  inst->trace[i].u.task.stage_idx = stage_idx;
  inst->trace[i].u.task.pipe_id = pipe_id;
  PIPE_MGR_UNLOCK(&inst->trace_mtx);
  LOG_DBG("%s (0x%x) DefAdd: pipe %d entry %u at %d.%d",
          stat_tbl->name,
          stat_tbl->stat_tbl_hdl,
          pipe_id,
          ent_hdl,
          stage_id,
          stage_idx);
}

void pipe_mgr_stat_mgr_trace_del(pipe_mgr_stat_tbl_t *stat_tbl,
                                 pipe_mgr_stat_tbl_instance_t *inst,
                                 pipe_mat_ent_hdl_t ent_hdl,
                                 dev_stage_t stage_id,
                                 pipe_stat_stage_ent_idx_t stage_idx,
                                 uint32_t deferred) {
  PIPE_MGR_LOCK(&inst->trace_mtx);
  unsigned int i = inst->trace_idx & PIPE_MGR_STAT_TRACE_MASK;
  ++inst->trace_idx;
  clock_gettime(CLOCK_MONOTONIC_RAW, &inst->trace[i].ts);
  inst->trace[i].op = BF_STAT_TRACE_TYPE_DEL;
  inst->trace[i].u.dir_api.ent_hdl = ent_hdl;
  inst->trace[i].u.dir_api.stage_id = stage_id;
  inst->trace[i].u.dir_api.stage_idx = stage_idx;
  inst->trace[i].u.dir_api.deferred = deferred;
  PIPE_MGR_UNLOCK(&inst->trace_mtx);
  LOG_DBG("%s (0x%x) Del: inst_pipe %X entry %u at %d.%d deferred 0x%x",
          stat_tbl->name,
          stat_tbl->stat_tbl_hdl,
          inst->pipe_id,
          ent_hdl,
          stage_id,
          stage_idx,
          deferred);
}

void pipe_mgr_stat_mgr_trace_def_del(pipe_mgr_stat_tbl_t *stat_tbl,
                                     pipe_mgr_stat_tbl_instance_t *inst,
                                     pipe_mat_ent_hdl_t ent_hdl,
                                     dev_stage_t stage_id,
                                     pipe_stat_stage_ent_idx_t stage_idx,
                                     bf_dev_pipe_t pipe_id) {
  PIPE_MGR_LOCK(&inst->trace_mtx);
  unsigned int i = inst->trace_idx & PIPE_MGR_STAT_TRACE_MASK;
  ++inst->trace_idx;
  clock_gettime(CLOCK_MONOTONIC_RAW, &inst->trace[i].ts);
  inst->trace[i].op = BF_STAT_TRACE_TYPE_DEF_DEL;
  inst->trace[i].u.task.ent_hdl = ent_hdl;
  inst->trace[i].u.task.stage_id = stage_id;
  inst->trace[i].u.task.stage_idx = stage_idx;
  inst->trace[i].u.task.pipe_id = pipe_id;
  PIPE_MGR_UNLOCK(&inst->trace_mtx);
  LOG_DBG("%s (0x%x) DefDel: pipe %d entry %u at %d.%d",
          stat_tbl->name,
          stat_tbl->stat_tbl_hdl,
          pipe_id,
          ent_hdl,
          stage_id,
          stage_idx);
}

void pipe_mgr_stat_mgr_trace_mov(pipe_mgr_stat_tbl_t *stat_tbl,
                                 pipe_mgr_stat_tbl_instance_t *inst,
                                 pipe_mat_ent_hdl_t ent_hdl,
                                 dev_stage_t dst_stage_id,
                                 pipe_stat_stage_ent_idx_t dst_stage_idx,
                                 dev_stage_t src_stage_id,
                                 pipe_stat_stage_ent_idx_t src_stage_idx,
                                 uint32_t deferred) {
  PIPE_MGR_LOCK(&inst->trace_mtx);
  unsigned int i = inst->trace_idx & PIPE_MGR_STAT_TRACE_MASK;
  ++inst->trace_idx;
  clock_gettime(CLOCK_MONOTONIC_RAW, &inst->trace[i].ts);
  inst->trace[i].op = BF_STAT_TRACE_TYPE_MOV;
  inst->trace[i].u.dir_api.ent_hdl = ent_hdl;
  inst->trace[i].u.dir_api.stage_id = dst_stage_id;
  inst->trace[i].u.dir_api.stage_idx = dst_stage_idx;
  inst->trace[i].u.dir_api.src_stage_id = src_stage_id;
  inst->trace[i].u.dir_api.src_stage_idx = src_stage_idx;
  inst->trace[i].u.dir_api.deferred = deferred;
  PIPE_MGR_UNLOCK(&inst->trace_mtx);
  LOG_DBG("%s (0x%x) Mov: inst_pipe %X entry %u %d.%d -> %d.%d deferred 0x%x",
          stat_tbl->name,
          stat_tbl->stat_tbl_hdl,
          inst->pipe_id,
          ent_hdl,
          src_stage_id,
          src_stage_idx,
          dst_stage_id,
          dst_stage_idx,
          deferred);
}

void pipe_mgr_stat_mgr_trace_def_mov(pipe_mgr_stat_tbl_t *stat_tbl,
                                     pipe_mgr_stat_tbl_instance_t *inst,
                                     pipe_mat_ent_hdl_t ent_hdl,
                                     dev_stage_t dst_stage_id,
                                     pipe_stat_stage_ent_idx_t dst_stage_idx,
                                     dev_stage_t src_stage_id,
                                     pipe_stat_stage_ent_idx_t src_stage_idx,
                                     bf_dev_pipe_t pipe_id) {
  PIPE_MGR_LOCK(&inst->trace_mtx);
  unsigned int i = inst->trace_idx & PIPE_MGR_STAT_TRACE_MASK;
  ++inst->trace_idx;
  clock_gettime(CLOCK_MONOTONIC_RAW, &inst->trace[i].ts);
  inst->trace[i].op = BF_STAT_TRACE_TYPE_DEF_MOV;
  inst->trace[i].u.task.ent_hdl = ent_hdl;
  inst->trace[i].u.task.stage_id = dst_stage_id;
  inst->trace[i].u.task.stage_idx = dst_stage_idx;
  inst->trace[i].u.task.src_stage_id = src_stage_id;
  inst->trace[i].u.task.src_stage_idx = src_stage_idx;
  inst->trace[i].u.task.pipe_id = pipe_id;
  PIPE_MGR_UNLOCK(&inst->trace_mtx);
  LOG_DBG("%s (0x%x) DefMov: pipe %d entry %u %d.%d -> %d.%d",
          stat_tbl->name,
          stat_tbl->stat_tbl_hdl,
          pipe_id,
          ent_hdl,
          src_stage_id,
          src_stage_idx,
          dst_stage_id,
          dst_stage_idx);
}

void pipe_mgr_stat_mgr_trace_bar(pipe_mgr_stat_tbl_t *stat_tbl,
                                 pipe_mgr_stat_tbl_instance_t *inst,
                                 pipe_mgr_stat_barrier_state_t *bs) {
  PIPE_MGR_LOCK(&inst->trace_mtx);
  unsigned int i = inst->trace_idx & PIPE_MGR_STAT_TRACE_MASK;
  ++inst->trace_idx;
  clock_gettime(CLOCK_MONOTONIC_RAW, &inst->trace[i].ts);
  inst->trace[i].op = BF_STAT_TRACE_TYPE_BAR;
  inst->trace[i].u.bar = *bs;
  PIPE_MGR_UNLOCK(&inst->trace_mtx);
  switch (bs->operation) {
    case PIPE_MGR_STAT_ENTRY_DUMP_OP:
      LOG_DBG(
          "%s (0x%x) Barrier: pipe-map 0x%x stage %d %s id 0x%x: pipe %x entry "
          "%u idx %d",
          stat_tbl->name,
          stat_tbl->stat_tbl_hdl,
          bs->pipe_ref_map,
          bs->stage_id,
          pipe_mgr_stat_mgr_operation_str(bs->operation),
          bs->lock_id,
          bs->op_state.entry_dump.pipe_id,
          bs->op_state.entry_dump.mat_ent_hdl,
          bs->op_state.entry_dump.ent_idx);
      break;
    case PIPE_MGR_STAT_TBL_DUMP_OP:
      LOG_DBG("%s (0x%x) Barrier: pipe-map 0x%x stage %d %s id 0x%x: pipe %x",
              stat_tbl->name,
              stat_tbl->stat_tbl_hdl,
              bs->pipe_ref_map,
              bs->stage_id,
              pipe_mgr_stat_mgr_operation_str(bs->operation),
              bs->lock_id,
              bs->op_state.tbl_dump.pipe_id);
      break;
    case PIPE_MGR_STAT_ENT_WRITE_OP:
      LOG_DBG(
          "%s (0x%x) Barrier: pipe-map 0x%x stage %d %s id 0x%x: pipe %x idx "
          "%d",
          stat_tbl->name,
          stat_tbl->stat_tbl_hdl,
          bs->pipe_ref_map,
          bs->stage_id,
          pipe_mgr_stat_mgr_operation_str(bs->operation),
          bs->lock_id,
          bs->op_state.ent_write.pipe_id,
          bs->op_state.ent_write.ent_idx);
      break;
    case PIPE_MGR_STAT_LOCK_OP:
    case PIPE_MGR_STAT_UNLOCK_OP:
      LOG_DBG("%s (0x%x) Barrier: pipe-map 0x%x stage %d %s id 0x%x",
              stat_tbl->name,
              stat_tbl->stat_tbl_hdl,
              bs->pipe_ref_map,
              bs->stage_id,
              pipe_mgr_stat_mgr_operation_str(bs->operation),
              bs->lock_id);
      break;
  }
}

void pipe_mgr_stat_mgr_trace_bar_ack(pipe_mgr_stat_tbl_t *stat_tbl,
                                     pipe_mgr_stat_tbl_instance_t *inst,
                                     bf_dev_pipe_t pipe_id,
                                     dev_stage_t stage_id,
                                     lock_id_t lock_id,
                                     bool deferred) {
  PIPE_MGR_LOCK(&inst->trace_mtx);
  unsigned int i = inst->trace_idx & PIPE_MGR_STAT_TRACE_MASK;
  ++inst->trace_idx;
  clock_gettime(CLOCK_MONOTONIC_RAW, &inst->trace[i].ts);
  inst->trace[i].op = BF_STAT_TRACE_TYPE_BAR_ACK;
  inst->trace[i].u.b_ack.pipe_id = pipe_id;
  inst->trace[i].u.b_ack.stage_id = stage_id;
  inst->trace[i].u.b_ack.lock_id = lock_id;
  inst->trace[i].u.b_ack.deferred = deferred ? 1 : 0;
  PIPE_MGR_UNLOCK(&inst->trace_mtx);
  LOG_DBG("%s (0x%x) BarrierAck: pipe %d stage %d id 0x%x",
          stat_tbl->name,
          stat_tbl->stat_tbl_hdl,
          pipe_id,
          stage_id,
          lock_id);
}

pipe_status_t pipe_mgr_stat_trace_str(pipe_mgr_stat_tbl_trace_entry_t *trace,
                                      char *buf,
                                      int buf_len) {
  int x = -1;
  switch (trace->op) {
    case BF_STAT_TRACE_TYPE_INVALID:
      *buf = '\0';
      return PIPE_OBJ_NOT_FOUND;
    case BF_STAT_TRACE_TYPE_ADD:
      x = snprintf(buf,
                   buf_len,
                   "%ld.%09ld Add entry %u at %d.%d deferred 0x%x",
                   trace->ts.tv_sec,
                   trace->ts.tv_nsec,
                   trace->u.dir_api.ent_hdl,
                   trace->u.dir_api.stage_id,
                   trace->u.dir_api.stage_idx,
                   trace->u.dir_api.deferred);
      break;
    case BF_STAT_TRACE_TYPE_DEF_ADD:
      x = snprintf(buf,
                   buf_len,
                   "%ld.%09ld DefAdd entry %u at %d.%d pipe-id %d",
                   trace->ts.tv_sec,
                   trace->ts.tv_nsec,
                   trace->u.task.ent_hdl,
                   trace->u.task.stage_id,
                   trace->u.task.stage_idx,
                   trace->u.task.pipe_id);
      break;
    case BF_STAT_TRACE_TYPE_DEL:
      x = snprintf(buf,
                   buf_len,
                   "%ld.%09ld Del entry %u at %d.%d deferred 0x%x",
                   trace->ts.tv_sec,
                   trace->ts.tv_nsec,
                   trace->u.dir_api.ent_hdl,
                   trace->u.dir_api.stage_id,
                   trace->u.dir_api.stage_idx,
                   trace->u.dir_api.deferred);
      break;
    case BF_STAT_TRACE_TYPE_DEF_DEL:
      x = snprintf(buf,
                   buf_len,
                   "%ld.%09ld DefDel entry %u at %d.%d pipe-id %d",
                   trace->ts.tv_sec,
                   trace->ts.tv_nsec,
                   trace->u.task.ent_hdl,
                   trace->u.task.stage_id,
                   trace->u.task.stage_idx,
                   trace->u.task.pipe_id);
      break;
    case BF_STAT_TRACE_TYPE_MOV:
      x = snprintf(buf,
                   buf_len,
                   "%ld.%09ld Mov entry %u %d.%d to %d.%d deferred 0x%x",
                   trace->ts.tv_sec,
                   trace->ts.tv_nsec,
                   trace->u.dir_api.ent_hdl,
                   trace->u.dir_api.src_stage_id,
                   trace->u.dir_api.src_stage_idx,
                   trace->u.dir_api.stage_id,
                   trace->u.dir_api.stage_idx,
                   trace->u.dir_api.deferred);
      break;
    case BF_STAT_TRACE_TYPE_DEF_MOV:
      x = snprintf(buf,
                   buf_len,
                   "%ld.%09ld DefMov entry %u %d.%d to %d.%d pipe-id %d",
                   trace->ts.tv_sec,
                   trace->ts.tv_nsec,
                   trace->u.task.ent_hdl,
                   trace->u.task.src_stage_id,
                   trace->u.task.src_stage_idx,
                   trace->u.task.stage_id,
                   trace->u.task.stage_idx,
                   trace->u.task.pipe_id);
      break;
    case BF_STAT_TRACE_TYPE_BAR:
      switch (trace->u.bar.operation) {
        case PIPE_MGR_STAT_ENTRY_DUMP_OP:
          x = snprintf(buf,
                       buf_len,
                       "%ld.%09ld Barrier: pipe-map 0x%x stage %d %s id 0x%x: "
                       "pipe %x entry %u idx %d",
                       trace->ts.tv_sec,
                       trace->ts.tv_nsec,
                       trace->u.bar.pipe_ref_map,
                       trace->u.bar.stage_id,
                       pipe_mgr_stat_mgr_operation_str(trace->u.bar.operation),
                       trace->u.bar.lock_id,
                       trace->u.bar.op_state.entry_dump.pipe_id,
                       trace->u.bar.op_state.entry_dump.mat_ent_hdl,
                       trace->u.bar.op_state.entry_dump.ent_idx);
          break;
        case PIPE_MGR_STAT_TBL_DUMP_OP:
          x = snprintf(
              buf,
              buf_len,
              "%ld.%09ld Barrier: pipe-map 0x%x stage %d %s id 0x%x: pipe %x",
              trace->ts.tv_sec,
              trace->ts.tv_nsec,
              trace->u.bar.pipe_ref_map,
              trace->u.bar.stage_id,
              pipe_mgr_stat_mgr_operation_str(trace->u.bar.operation),
              trace->u.bar.lock_id,
              trace->u.bar.op_state.tbl_dump.pipe_id);
          break;
        case PIPE_MGR_STAT_ENT_WRITE_OP:
          x = snprintf(buf,
                       buf_len,
                       "%ld.%09ld Barrier: pipe-map 0x%x stage %d %s id 0x%x: "
                       "pipe %x idx %d",
                       trace->ts.tv_sec,
                       trace->ts.tv_nsec,
                       trace->u.bar.pipe_ref_map,
                       trace->u.bar.stage_id,
                       pipe_mgr_stat_mgr_operation_str(trace->u.bar.operation),
                       trace->u.bar.lock_id,
                       trace->u.bar.op_state.ent_write.pipe_id,
                       trace->u.bar.op_state.ent_write.ent_idx);
          break;
        case PIPE_MGR_STAT_LOCK_OP:
        case PIPE_MGR_STAT_UNLOCK_OP:
          x = snprintf(buf,
                       buf_len,
                       "%ld.%09ld Barrier: pipe-map 0x%x stage %d %s id 0x%x",
                       trace->ts.tv_sec,
                       trace->ts.tv_nsec,
                       trace->u.bar.pipe_ref_map,
                       trace->u.bar.stage_id,
                       pipe_mgr_stat_mgr_operation_str(trace->u.bar.operation),
                       trace->u.bar.lock_id);
          break;
      }
      break;
    case BF_STAT_TRACE_TYPE_BAR_ACK:
      x = snprintf(buf,
                   buf_len,
                   "%ld.%09ld BarrierAck: pipe %d stage %d id 0x%x%s",
                   trace->ts.tv_sec,
                   trace->ts.tv_nsec,
                   trace->u.b_ack.pipe_id,
                   trace->u.b_ack.stage_id,
                   trace->u.b_ack.lock_id,
                   trace->u.b_ack.deferred ? " Deferred" : "");
      break;
  }
  if (x == -1) return PIPE_UNEXPECTED;
  if (x >= buf_len) return PIPE_NO_SPACE;
  return PIPE_SUCCESS;
}

void pipe_mgr_stat_trace_err_log(pipe_mgr_stat_tbl_instance_t *inst) {
  int buf_len = 32;
  char *buf = PIPE_MGR_MALLOC(buf_len);
  if (!buf) {
    return;
  }
  pipe_status_t x = PIPE_SUCCESS;

  PIPE_MGR_LOCK(&inst->trace_mtx);
  for (uint32_t i = 0; i < PIPE_MGR_STAT_TRACE_SIZE; ++i) {
    uint32_t idx = (inst->trace_idx + i) & PIPE_MGR_STAT_TRACE_MASK;
    pipe_mgr_stat_tbl_trace_entry_t *t = &inst->trace[idx];
    do {
      x = pipe_mgr_stat_trace_str(t, buf, buf_len);
      if (x == PIPE_NO_SPACE) {
        PIPE_MGR_FREE(buf);
        buf_len = buf_len * 2;
        buf = PIPE_MGR_MALLOC(buf_len);
        if (!buf) {
          PIPE_MGR_UNLOCK(&inst->trace_mtx);
          return;
        }
      }
    } while (x == PIPE_NO_SPACE && buf_len < 2048);
    if (x == PIPE_SUCCESS) {
      LOG_ERROR("%4d: %s", i, buf);
    } else if (x == PIPE_OBJ_NOT_FOUND) {
      /* Trace entry is not valid, just continue. */
    } else {
      LOG_ERROR("%4d: %s", i, pipe_str_err(x));
    }
  }
  PIPE_MGR_UNLOCK(&inst->trace_mtx);
  if (buf) PIPE_MGR_FREE(buf);
}
