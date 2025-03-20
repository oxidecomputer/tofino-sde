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
 * @file pipe_mgr_stat_lrt.c
 * @date
 *
 *
 * Contains implementation of lrt and other statistics related message
 *processing code.
 */

/* Global header includes */
#include <stdbool.h>
#include <sys/types.h>
#ifdef __sun
#include <endian.h>
#endif

/* Module header includes */
#include "bf_types/bf_types.h"
#include "dvm/bf_dma_types.h"
#include "lld/bf_dma_if.h"
#include "lld/lld_err.h"
#include "pipe_mgr/pipe_mgr_config.h"
#include "pipe_mgr/pipe_mgr_porting.h"

/* Local header includes */
#include "pipe_mgr_stat_lrt.h"
#include "pipe_mgr_stat_mgr_int.h"
#include "pipe_mgr_log.h"
#include "pipe_mgr_int.h"
#include "pipe_mgr_tbl.h"
#include "pipe_mgr_drv.h"
#include "pipe_mgr_stat_trace.h"

/* Pointer to global pipe_mgr context */
extern pipe_mgr_ctx_t *pipe_mgr_ctx;
/* Timeout in micro-seconds */
#define PIPE_MGR_STAT_MGR_LRT_FM_TIMEOUT_US 50

extern bool stat_mgr_enable_detail_trace;

static pipe_status_t pipe_mgr_stat_process_data_msg(
    rmt_dev_info_t *dev_info,
    bf_subdev_id_t subdev_id,
    pipe_mgr_stat_msg_data_t *msg);
static pipe_status_t pipe_mgr_stat_process_stat_dump(
    rmt_dev_info_t *dev_info,
    rmt_stat_ent_addr_t address,
    uint64_t data,
    bool lrt_evict);
static pipe_status_t pipe_mgr_stat_mgr_process_lock_ack(bf_dev_id_t device_id,
                                                        lock_id_t lock_id,
                                                        bf_dev_pipe_t pipe_id,
                                                        dev_stage_t stage_id,
                                                        uint8_t ltbl_id);
static pipe_status_t pipe_mgr_stat_mgr_process_lock_ack_msg(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    lock_id_t lock_id,
    bf_dev_pipe_t pipe_id,
    dev_stage_t stage_id);

void pipe_mgr_drv_lrt_cb(bf_dev_id_t logical_device,
                         bf_subdev_id_t subdev_id,
                         int size,
                         bf_dma_addr_t dma_addr) {
  uint8_t device_id = logical_device;
  pipe_status_t status = PIPE_SUCCESS;
  pipe_mgr_stat_msg_data_t *msg = NULL;
  pipe_mgr_stat_msg_data_t *curr_msg = NULL;
  uint32_t num_msgs = 0;
  unsigned i = 0;
  unsigned j = 0;
  size_t buf_size;
  bf_dma_addr_t addr_dma;
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(device_id);
  pipe_mgr_drv_lrt_cfg_t *lrt_cfg =
      &pipe_mgr_drv_ctx()->lrt_cfg[logical_device][subdev_id];

  /* The LRT callback function is thread-safe. Multiple threads can be pulling
   * off updates from the LRT DR and invoking this callback. There is a
   * requirement that the responses from the LRT DR are processed in the order
   * they are received. Given this, this function checks whether the dma buffer
   * received is the next one to be processed. If yes, it proceeds, if not
   * waits on the previous buffers to be processed. The dam buffer addresses
   * tracked after processing this buffer, the expected value is advanced.
   */
  PIPE_MGR_LOCK(&(lrt_cfg->lrt_buffer_array_mtx));
  while (lrt_cfg->buffer_array[lrt_cfg->next_expected_buffer_idx] != dma_addr) {
    PIPE_MGR_COND_WAIT(&lrt_cfg->lrt_buffer_array_condvar,
                       &lrt_cfg->lrt_buffer_array_mtx);
  }
  PIPE_MGR_UNLOCK(&(lrt_cfg->lrt_buffer_array_mtx));

  bf_sys_dma_pool_handle_t hndl = pipe_mgr_drv_subdev_dma_pool_handle(
      device_id, subdev_id, PIPE_MGR_DRV_BUF_LRT);
  buf_size =
      pipe_mgr_drv_subdev_buf_size(device_id, subdev_id, PIPE_MGR_DRV_BUF_LRT);

  /* Map the DMA address of the buffer to its virtual address before it can be
   * used by pipe manager */
  uint8_t *addr = bf_mem_dma2virt(hndl, dma_addr);
  if (addr != NULL) {
    if (bf_sys_dma_unmap(hndl, addr, buf_size, BF_DMA_TO_CPU) != 0) {
      LOG_ERROR(
          "Unable to unmap DMA buffer %p at %s:%d", addr, __func__, __LINE__);
    }
  } else {
    LOG_ERROR("Invalid virtual address derived from %" PRIx64 "at %s:%d",
              (uint64_t)dma_addr,
              __func__,
              __LINE__);
    goto advance_buffer_idx;
  }
  msg = (pipe_mgr_stat_msg_data_t *)addr;
  num_msgs = size / sizeof(pipe_mgr_stat_msg_data_t);

  /* Start processing the messages */
  for (i = 0; i < num_msgs; i++) {
    curr_msg = &msg[i];

    curr_msg->word0 = le64toh(curr_msg->word0);
    curr_msg->data = le64toh(curr_msg->data);
    status = pipe_mgr_stat_process_data_msg(dev_info, subdev_id, curr_msg);
    if (status != PIPE_SUCCESS) {
      LOG_ERROR(
          "%s : Error in processing stat data message %d within"
          " stat rx message %d, err %s",
          __func__,
          j,
          i,
          pipe_str_err(status));
    }
  }

  /* Return the buffers to the free pool */
  int push_cnt = 0;
  bool dr_push_err = false;
  lld_err_t x = LLD_OK;
  do {
    /* Remap the virtual address to the DMA address before the buffer is pushed
     * back into the DR */
    if (bf_sys_dma_map(hndl,
                       addr,
                       (bf_phys_addr_t)dma_addr,
                       size,
                       &addr_dma,
                       BF_DMA_TO_CPU) != 0) {
      LOG_ERROR(
          "Unable to map DMA buffer %p at %s:%d", addr, __func__, __LINE__);
      goto advance_buffer_idx;
    }
    x = lld_subdev_push_fm(
        device_id, subdev_id, lld_dr_fm_lrt, addr_dma, lrt_cfg->buf_size);
    if (x != LLD_OK) {
      /* Unmap the buffer */
      if (bf_sys_dma_unmap(hndl, addr, size, BF_DMA_TO_CPU) != 0) {
        LOG_ERROR(
            "Unable to unmap DMA buffer %p at %s:%d", addr, __func__, __LINE__);
      }
      ++push_cnt;
      if (!dr_push_err) {
        LOG_ERROR(
            "%s Error pushing lrt free memory to device %d rc %d addr "
            "0x%" PRIx64 "size %d",
            __func__,
            device_id,
            x,
            addr_dma,
            lrt_cfg->buf_size);
        dr_push_err = true;
      }
    } else if (dr_push_err) {
      LOG_ERROR(
          "%s:%d Retry pushing lrt free memory to device %d successful, try %d",
          __func__,
          __LINE__,
          device_id,
          push_cnt);
    }
  } while (x == LLD_ERR_DR_FULL && push_cnt < 1000);

  pipe_mgr_drv_push_lrt_drs(device_id);

advance_buffer_idx:
  PIPE_MGR_LOCK(&(lrt_cfg->lrt_buffer_array_mtx));
  lrt_cfg->next_expected_buffer_idx =
      ((lrt_cfg->next_expected_buffer_idx + 1) % lrt_cfg->num_buffers);
  PIPE_MGR_COND_BROADCAST_SIGNAL(&lrt_cfg->lrt_buffer_array_condvar);
  PIPE_MGR_UNLOCK(&(lrt_cfg->lrt_buffer_array_mtx));

  return;
}

static pipe_status_t pipe_mgr_stat_process_data_msg(
    rmt_dev_info_t *dev_info,
    bf_subdev_id_t subdev_id,
    pipe_mgr_stat_msg_data_t *msg) {
  pipe_stat_data_msg_type_t msg_type;
  rmt_stat_ent_addr_t addr;
  pipe_status_t status = PIPE_SUCCESS;
  bf_dev_id_t device_id = 0;
  uint64_t addr_lock_id = 0;
  int phy_pipe_id = 0;
  bf_dev_pipe_t pipe_id = 0;
  dev_stage_t stage_id = 0;
  uint8_t ltbl_id = 0;
  bool lrt_evict = false;
  lock_id_t lock_id;

  if (!dev_info || !msg) {
    LOG_ERROR("%s:%d Null pointer arguments passed", __func__, __LINE__);
    return PIPE_INVALID_ARG;
  }

  device_id = dev_info->dev_id;
  extract_dr_stat_data_wd0(dev_info->dev_family, msg, &msg_type, &addr_lock_id);

  switch (msg_type) {
    case PIPE_STAT_MSG_TYPE_EVICT:
      /* A LR(t) evict */
      lrt_evict = true;
    /* Fall through */
    case PIPE_STAT_MSG_TYPE_STAT_DUMP:
      PIPE_MGR_MEMCPY(&addr, &addr_lock_id, sizeof(addr_lock_id));
      status =
          pipe_mgr_stat_process_stat_dump(dev_info, addr, msg->data, lrt_evict);
      break;
    case PIPE_STAT_MSG_TYPE_LOCK_ACK:
      /* Extract the address which contains pipe-id, stage-id and
       * logical table id for the lock ack. */
      extract_dr_stat_data_lock_ack_addr(
          dev_info->dev_family, msg, &phy_pipe_id, &stage_id, &ltbl_id);
      /* Lock-id is just a part of the addr_lock_id field. Extract it
       * accordingly.  */
      lock_id = addr_lock_id & 0xffff;
      phy_pipe_id += subdev_id * BF_SUBDEV_PIPE_COUNT;

      /* Convert the physical pipe-id to the logical pipe-id */
      status = pipe_mgr_map_phy_pipe_id_to_log_pipe_id_optimized(
          dev_info, phy_pipe_id, &pipe_id);
      if (status != PIPE_SUCCESS) {
        LOG_ERROR(
            "%s:%d Dev %d unable to convert phy pipe %d to logical, sts %s",
            __func__,
            __LINE__,
            device_id,
            phy_pipe_id,
            pipe_str_err(status));
        return status;
      }

      status = pipe_mgr_stat_mgr_process_lock_ack(
          device_id, lock_id, pipe_id, stage_id, ltbl_id);
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }

  return status;
}

/* pipe_mgr_stat_mgr_process_lock_ack : Process LR(t) message type corresponding
 * to Lock ACK. This message can either contain a lock ack or a barrier ack.
 * This function checks the type of the message and demultiplexes accordingly.
 */
static pipe_status_t pipe_mgr_stat_mgr_process_lock_ack(bf_dev_id_t device_id,
                                                        lock_id_t lock_id,
                                                        bf_dev_pipe_t pipe_id,
                                                        dev_stage_t stage_id,
                                                        uint8_t ltbl_id) {
  pipe_status_t status = PIPE_SUCCESS;

  if (PIPE_MGR_GET_LOCK_ID_TYPE(lock_id) != LOCK_ID_TYPE_STAT_BARRIER &&
      PIPE_MGR_GET_LOCK_ID_TYPE(lock_id) != LOCK_ID_TYPE_STAT_LOCK &&
      PIPE_MGR_GET_LOCK_ID_TYPE(lock_id) != LOCK_ID_TYPE_ALL_LOCK) {
    /* This barrier ACK or lock ACK not meant for stats, hence
     * ignore. This can happen because when Idele-time or Stats manager
     * issues a Lock or a Barrier instruction, Tofino sends the ACK
     * back on both the Idle time DR as well as Stat DR, hence a
     * ACK not meant for Stats is simply discarded.
     */
    return PIPE_SUCCESS;
  }

  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance = NULL;

  pipe_mgr_stat_tbl_lkup(
      device_id, ltbl_id, pipe_id, stage_id, &stat_tbl, &stat_tbl_instance);
  if (stat_tbl == NULL) {
    LOG_ERROR("%s: Stat table not found for dev %d pipe %d stage %d logTbl %d",
              __func__,
              device_id,
              pipe_id,
              stage_id,
              ltbl_id);
    return PIPE_OBJ_NOT_FOUND;
  } else if (stat_tbl_instance == NULL) {
    LOG_ERROR("%s : Dev %d stat tbl %s (0x%x) instance not found for pipe %d",
              __func__,
              stat_tbl->device_id,
              stat_tbl->name,
              stat_tbl->stat_tbl_hdl,
              pipe_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  status = pipe_mgr_stat_mgr_process_lock_ack_msg(
      stat_tbl, stat_tbl_instance, lock_id, pipe_id, stage_id);
  return status;
}

static pipe_status_t pipe_mgr_stat_process_stat_dump(
    rmt_dev_info_t *dev_info,
    rmt_stat_ent_addr_t address,
    uint64_t data,
    bool lrt_evict) {
  pipe_status_t status = PIPE_SUCCESS;
  bf_dev_id_t device_id = dev_info->dev_id;
  rmt_virt_addr_t virt_addr = 0;
  pipe_stat_ent_idx_t stat_ent_idx = 0;

  uint8_t ltbl_id = 0;
  bf_dev_pipe_t pipe_id = 0;
  bf_dev_pipe_t log_pipe_id = 0;
  bf_dev_pipe_t phy_pipe_id = 0;
  dev_stage_t stage_id = 0;
  uint64_t packet_count = 0;
  uint64_t byte_count = 0;

  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      ltbl_id = address.tof.log_tbl_id;
      phy_pipe_id = address.tof.pipe_id;
      stage_id = address.tof.stage_id;
      virt_addr = address.tof.stat_ent_virt_addr;
      break;
    case BF_DEV_FAMILY_TOFINO2:
      ltbl_id = address.tof2.log_tbl_id;
      phy_pipe_id = address.tof2.pipe_id;
      stage_id = address.tof2.stage_id;
      virt_addr = address.tof2.stat_ent_virt_addr;
      break;
    case BF_DEV_FAMILY_TOFINO3:
      ltbl_id = address.tof3.log_tbl_id;
      phy_pipe_id = address.tof3.pipe_id;
      stage_id = address.tof3.stage_id;
      virt_addr = address.tof3.stat_ent_virt_addr;
      break;

    case BF_DEV_FAMILY_UNKNOWN:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }

  status = pipe_mgr_map_phy_pipe_id_to_log_pipe_id_optimized(
      dev_info, phy_pipe_id, &log_pipe_id);
  if (status != PIPE_SUCCESS) {
    LOG_ERROR(
        "%s:%d Error in converting physical pipe id %d to logical"
        " pipe for device id %d, err %s",
        __func__,
        __LINE__,
        phy_pipe_id,
        device_id,
        pipe_str_err(status));
    return status;
  }

  pipe_id = log_pipe_id;

  pipe_mgr_stat_tbl_t *stat_tbl = NULL;
  pipe_mgr_stat_tbl_instance_t *stat_tbl_instance = NULL;
  pipe_mgr_stat_tbl_lkup(
      device_id, ltbl_id, pipe_id, stage_id, &stat_tbl, &stat_tbl_instance);
  if (stat_tbl == NULL) {
    LOG_ERROR("%s: Stat table not found for dev %d pipe %d stage %d logTbl %d",
              __func__,
              device_id,
              pipe_id,
              stage_id,
              ltbl_id);
    return PIPE_OBJ_NOT_FOUND;
  } else if (stat_tbl_instance == NULL) {
    LOG_ERROR("%s : Dev %d stat tbl %s (0x%x) instance not found for pipe %d",
              __func__,
              stat_tbl->device_id,
              stat_tbl->name,
              stat_tbl->stat_tbl_hdl,
              pipe_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  pipe_id = stat_tbl_instance->pipe_id;

  /* From the entry virtual address, get the stat entry index */
  status = pipe_mgr_stat_mgr_get_ent_idx_from_virt_addr(
      stat_tbl, virt_addr, pipe_id, stage_id, &stat_ent_idx);

  if (status != PIPE_SUCCESS) {
    LOG_ERROR(
        "%s:%d : Incorrect VPN received in an %s message for stats tbl %s, "
        "0x%x, device id %d pipe id %d, stage id %d, virt addr 0x%x",
        __func__,
        __LINE__,
        lrt_evict ? "LRT" : "entry-dump",
        stat_tbl->name,
        stat_tbl->stat_tbl_hdl,
        device_id,
        log_pipe_id,
        stage_id,
        virt_addr);
    return status;
  }

  /* Now, based on the statistics type of the table and the resolution,
   * decode the 64 bit of data into packet and byte count. This is the
   * format in which Tofino packs the data into the 64 bit field.
   */
  if (stat_tbl->counter_type == PACKET_COUNT) {
    if (lrt_evict) {
      /* For an lrt evict message, packet count is a max of 32 bits */
      packet_count = data & 0xffffffff;
    } else {
      packet_count = data;
    }
  } else if (stat_tbl->counter_type == BYTE_COUNT) {
    if (lrt_evict) {
      /* In the LRT evict message, for byte only counters the byte count
       * is from bit 28 onwards.
       */
      byte_count = data >> 28;
    } else {
      byte_count = data;
    }
  } else if (stat_tbl->counter_type == PACKET_AND_BYTE_COUNT) {
    /* Packet counter resolution dictates the byte counter resolution */
    switch (stat_tbl->packet_counter_resolution) {
      case 64:
        if (pipe_mgr_stat_mgr_get_dump_type_from_addr(virt_addr) ==
            PIPE_MGR_STAT_DUMP_TYPE_BYTE_COUNT) {
          byte_count = data;
        } else {
          packet_count = data;
        }
        break;
      case 28:
        PIPE_MGR_DBGCHK(stat_tbl->byte_counter_resolution == 36);
        /* In this case, the dump message and LRT evict message both have the
         * same format
         */
        packet_count = data & 0xfffffff;
        byte_count = (data >> 28) & 0xfffffffff;
        break;
      case 17:
        PIPE_MGR_DBGCHK(stat_tbl->byte_counter_resolution == 25);
        if (lrt_evict) {
          /* For the lrt evict message, byte counter always starts at 29th bit
           * onwards
           */
          packet_count = data & 0x1ffff;
          byte_count = (data >> 28) & 0x1ffffff;
        } else {
          packet_count = data & 0x1ffff;
          byte_count = (data >> 17) & 0x1ffffff;
        }
        break;
      default:
        PIPE_MGR_DBGCHK(0);
        return PIPE_UNEXPECTED;
    }
  }

  /* Maintain some lrt debug info */
  if (lrt_evict) {
    if (log_pipe_id < stat_tbl->dev_info->num_active_pipes &&
        stage_id < stat_tbl->dev_info->num_active_mau) {
      for (int i = 0; i < stat_tbl_instance->num_stages; ++i) {
        if (stage_id != stat_tbl_instance->stat_tbl_stage_info[i].stage_id)
          continue;
        if (stat_ent_idx >=
            stat_tbl_instance->stat_tbl_stage_info[i].num_entries)
          break;
        stat_tbl_instance
            ->ent_idx_lrt_dbg_info[log_pipe_id][stage_id][stat_ent_idx]
            .num_lrt_evicts_rcvd++;
        stat_tbl_instance
            ->ent_idx_lrt_dbg_info[log_pipe_id][stage_id][stat_ent_idx]
            .stat_data.bytes = byte_count;
        stat_tbl_instance
            ->ent_idx_lrt_dbg_info[log_pipe_id][stage_id][stat_ent_idx]
            .stat_data.packets = packet_count;
      }
    }
    stat_tbl->num_lrt_evicts_rcvd++;
  }

  if (stat_tbl_instance->def_barrier_node[log_pipe_id][stage_id]) {
    /* This indicates we've received a barrier-ack for this stage out of order
     * and that we've deferred the processing of that barrier-ack.  We must then
     * also defer any dump/evict message processing until after we've processed
     * the barrier-ack and its task_list.  So, save this message against the
     * barrier list node for later processing. */
    pipe_mgr_stat_deferred_dump_t *dump = PIPE_MGR_MALLOC(sizeof *dump);
    if (!dump) return PIPE_NO_SYS_RESOURCES;
    dump->stat_ent_idx = stat_ent_idx;
    dump->stat_data.bytes = byte_count;
    dump->stat_data.packets = packet_count;
    PIPE_MGR_DLL_AP(
        stat_tbl_instance->def_barrier_node[log_pipe_id][stage_id]->dump_list,
        dump,
        next,
        prev);
  } else {
    /* Increment the packet count and the byte count */
    pipe_stat_data_t stat_data = {0};
    stat_data.bytes = byte_count;
    stat_data.packets = packet_count;

    status = pipe_mgr_stat_mgr_incr_ent_idx_count(stat_tbl,
                                                  stat_tbl_instance,
                                                  log_pipe_id,
                                                  stage_id,
                                                  stat_ent_idx,
                                                  &stat_data);

    if (status != PIPE_SUCCESS) {
      LOG_ERROR(
          "%s:%d Error incrementing stat entry count for table %s (0x%x) pipe "
          "%d, instance %X, stage %d index %d, err %s",
          __func__,
          __LINE__,
          stat_tbl->name,
          stat_tbl->stat_tbl_hdl,
          log_pipe_id,
          pipe_id,
          stage_id,
          stat_ent_idx,
          pipe_str_err(status));
      return status;
    }
  }
  return PIPE_SUCCESS;
}

static inline void log_barrier_op(pipe_mgr_stat_tbl_t *stat_tbl,
                                  bf_dev_pipe_t pipe_id,
                                  dev_stage_t stage_id,
                                  int ref_map,
                                  pipe_mgr_stat_barrier_state_t *bs) {
  switch (bs->operation) {
    case PIPE_MGR_STAT_ENTRY_DUMP_OP:
      PIPE_MGR_STAT_TRACE(stat_tbl->device_id,
                          stat_tbl->stat_tbl_hdl,
                          stat_tbl->name,
                          bs->op_state.entry_dump.ent_idx,
                          -1,
                          pipe_id,
                          stage_id,
                          "Rx entry dump barrier, ref_map 0x%x",
                          ref_map);
      break;
    case PIPE_MGR_STAT_TBL_DUMP_OP:
      PIPE_MGR_STAT_TRACE(stat_tbl->device_id,
                          stat_tbl->stat_tbl_hdl,
                          stat_tbl->name,
                          -1,
                          -1,
                          pipe_id,
                          stage_id,
                          "Rx table dump barrier, ref_map 0x%x",
                          ref_map);
      break;
    case PIPE_MGR_STAT_ENT_WRITE_OP:
      PIPE_MGR_STAT_TRACE(stat_tbl->device_id,
                          stat_tbl->stat_tbl_hdl,
                          stat_tbl->name,
                          bs->op_state.ent_write.ent_idx,
                          -1,
                          pipe_id,
                          stage_id,
                          "Rx entry write barrier, ref_map 0x%x ",
                          ref_map);
      break;
    case PIPE_MGR_STAT_LOCK_OP:
      PIPE_MGR_STAT_TRACE(stat_tbl->device_id,
                          stat_tbl->stat_tbl_hdl,
                          stat_tbl->name,
                          -1,
                          -1,
                          pipe_id,
                          stage_id,
                          "Rx Lock ACK, ref_map 0x%x ",
                          ref_map);
      break;
    case PIPE_MGR_STAT_UNLOCK_OP:
      PIPE_MGR_STAT_TRACE(stat_tbl->device_id,
                          stat_tbl->stat_tbl_hdl,
                          stat_tbl->name,
                          -1,
                          -1,
                          pipe_id,
                          stage_id,
                          "Rx Unlock ACK, ref_map 0x%x ",
                          ref_map);
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      return;
  }
}
static void pipe_mgr_stat_mgr_execute_dump_list(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    bf_dev_pipe_t pipe_id,
    dev_stage_t stage_id,
    pipe_mgr_stat_deferred_dump_t *dump_msgs) {
  while (dump_msgs) {
    pipe_mgr_stat_deferred_dump_t *msg = dump_msgs;
    PIPE_MGR_DLL_REM(dump_msgs, msg, next, prev);
    bf_status_t status = pipe_mgr_stat_mgr_incr_ent_idx_count(stat_tbl,
                                                              stat_tbl_instance,
                                                              pipe_id,
                                                              stage_id,
                                                              msg->stat_ent_idx,
                                                              &msg->stat_data);
    if (status != PIPE_SUCCESS) {
      LOG_ERROR(
          "%s:%d Error incrementing stat entry count for table %s (0x%x) pipe "
          "%d, instance %X, stage %d index %d, err %s",
          __func__,
          __LINE__,
          stat_tbl->name,
          stat_tbl->stat_tbl_hdl,
          pipe_id,
          stat_tbl_instance->pipe_id,
          stage_id,
          msg->stat_ent_idx,
          pipe_str_err(status));
    }
    PIPE_MGR_FREE(msg);
  }
}
static pipe_status_t pipe_mgr_stat_mgr_process_lock_ack_msg(
    pipe_mgr_stat_tbl_t *stat_tbl,
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance,
    lock_id_t lock_id,
    bf_dev_pipe_t pipe_id,
    dev_stage_t stage_id) {
  bf_dev_id_t device_id = stat_tbl->device_id;
  bool next_bl_ready = false;

  /* Lock the barrier list before looking for the barrier state because another
   * thread performing a stats table API may modify the list to append a new
   * node. The lock must be held while processing the node and executing its
   * task list.  We do not want another thread to see the barrier_list be NULL
   * and execute an operation immediately instead of defering it before we
   * process the task list on the node. */
  PIPE_MGR_LOCK(&stat_tbl_instance->barrier_data_mtx);
  pipe_mgr_stat_barrier_list_node_t *bl =
      stat_tbl_instance->barrier_list[pipe_id];
  if (!bl) {
    /* Barrier received but no SW state for it.  We may have lost track of our
     * operations or we've mishandled DMA buffers. */
    LOG_ERROR(
        "%s/%d : No state found for barrier id 0x%x, for tbl 0x%x device id "
        "%d pipe %d stage %d",
        __func__,
        __LINE__,
        lock_id,
        stat_tbl->stat_tbl_hdl,
        device_id,
        pipe_id,
        stage_id);
    pipe_mgr_stat_trace_err_log(stat_tbl_instance);
    PIPE_MGR_DBGCHK(0);
    PIPE_MGR_UNLOCK(&stat_tbl_instance->barrier_data_mtx);
    return PIPE_SUCCESS;
  } else if (bl->barrier_state->stage_id == stage_id &&
             bl->barrier_state->lock_id != lock_id) {
    /* We have barrier state for this pipe and stage, but this is not the
     * operation we were expecting.  Must have either lost a barrier or we're
     * getting duplicate messages (process the same DMA buffer twice?!). */
    LOG_ERROR(
        "%s/%d : Unexpected state found for barrier id 0x%x, for tbl 0x%x "
        "device id %d pipe %d stage %d, lock_id 0x%x, expected 0x%x",
        __func__,
        __LINE__,
        lock_id,
        stat_tbl->stat_tbl_hdl,
        device_id,
        pipe_id,
        stage_id,
        lock_id,
        bl->barrier_state->lock_id);
    pipe_mgr_stat_trace_err_log(stat_tbl_instance);
    PIPE_MGR_DBGCHK(0);
    PIPE_MGR_UNLOCK(&stat_tbl_instance->barrier_data_mtx);
    return PIPE_SUCCESS;
  } else if (bl->barrier_state->stage_id != stage_id) {
    /* We have gotten a barrier-ack for an unexpected stage.  However, this may
     * be okay as the MAU stages might post their notifications to SBC out of
     * order with respect to other stages (it is always in order within the
     * stage) due to waiting for an opportunity to send the notification over
     * the PBus. Scan the pipe's barrier_list, the first entry found for this
     * stage must match this message, once it is found, mark it
     * ready-for-processing. */
    bool handled = false;
    for (pipe_mgr_stat_barrier_list_node_t *i = bl->next; i; i = i->next) {
      if (i->barrier_state->stage_id != stage_id) continue;
      if (i->received_hw_ack) continue;
      if (i->barrier_state->lock_id == lock_id) {
        i->received_hw_ack = true;
        handled = true;
        stat_tbl_instance->def_barrier_node[pipe_id][stage_id] = i;
        pipe_mgr_stat_mgr_trace_bar_ack(
            stat_tbl, stat_tbl_instance, pipe_id, stage_id, lock_id, true);
      }
      break;
    }
    if (!handled) {
      LOG_ERROR(
          "%s/%d : No state found for barrier id 0x%x, for tbl 0x%x device id "
          "%d pipe %d stage %d",
          __func__,
          __LINE__,
          lock_id,
          stat_tbl->stat_tbl_hdl,
          device_id,
          pipe_id,
          stage_id);
      pipe_mgr_stat_trace_err_log(stat_tbl_instance);
      PIPE_MGR_DBGCHK(0);
    }
    PIPE_MGR_UNLOCK(&stat_tbl_instance->barrier_data_mtx);
    return PIPE_SUCCESS;
  }

process_next:
  pipe_mgr_stat_mgr_trace_bar_ack(
      stat_tbl, stat_tbl_instance, pipe_id, stage_id, lock_id, false);

  BF_LIST_DLL_REM(stat_tbl_instance->barrier_list[pipe_id], bl, next, prev)
  pipe_mgr_stat_barrier_state_t *bs = bl->barrier_state;
  if (bl == stat_tbl_instance->def_barrier_node[pipe_id][stage_id]) {
    stat_tbl_instance->def_barrier_node[pipe_id][stage_id] = NULL;
  }

  /* Now that this entry has been removed from the head of the barrier_list,
   * peek the new head to see if it is also ready for processing. */
  next_bl_ready = stat_tbl_instance->barrier_list[pipe_id] &&
                  stat_tbl_instance->barrier_list[pipe_id]->received_hw_ack;

  /* Handle a barrier for an entry-write by decrementing the count against the
   * table location.  If this barrier-ack is for an init of a new entry then
   * user-set-in-progress would not set on the location and does not need to be
   * decremented.  If this barrier-ack is for an entry-set then the location
   * does have a user-set-in-progress count which needs to be decremented.  We
   * do not need special handling however, if this is an init then the entry is
   * still pending and any user-set-in-prog counts are not in the ent_idx_info
   * yet.  Since the reset function used already checks for zero before
   * decrementing the user_set_in_progress field no extra handling is needed. */
  if (bs->operation == PIPE_MGR_STAT_ENT_WRITE_OP) {
    pipe_mgr_stat_mgr_reset_ent_write_in_progress(
        stat_tbl,
        stat_tbl_instance,
        pipe_id,
        stage_id,
        bs->op_state.ent_write.ent_idx);
  }

  /* Clear the pipe_ref_map under a lock since there may be one thread per LR(t)
   * Notify DR working on it. */
  bs->pipe_ref_map &= ~(1u << pipe_id);
  int ref_map = bs->pipe_ref_map;

  /* Log the response for debug purposes. */
  if (stat_mgr_enable_detail_trace) {
    log_barrier_op(stat_tbl, pipe_id, stage_id, ref_map, bs);
  }

  /* Since the barrier has completed on this pipe execute any pending operations
   * on the task_list. */
  pipe_mgr_stat_mgr_task_node_t *task_list = bl->task_list;
  bl->task_list = NULL;
  pipe_mgr_stat_mgr_execute_task_list(
      stat_tbl, stat_tbl_instance, pipe_id, task_list);

  /* Now process any deferred dump/evict messages. */
  pipe_mgr_stat_mgr_execute_dump_list(
      stat_tbl, stat_tbl_instance, pipe_id, stage_id, bl->dump_list);

  /* Now that all the deferred work is done the barrier_data_mtx can be released
   * allowing any threads executing APIs to check the barrier_list and decide if
   * an operation can execute immediately or if it should be deferred. */
  PIPE_MGR_UNLOCK(&stat_tbl_instance->barrier_data_mtx);

  PIPE_MGR_FREE(bl);

  /* If the ref_map is now empty then this was the final response associated
   * with it and it can be freed.  If this is for a table dump the user callback
   * can be called on it. */
  if (!ref_map) {
    if (bs->operation == PIPE_MGR_STAT_TBL_DUMP_OP) {
      if (bs->op_state.tbl_dump.callback_fn) {
        pipe_mgr_stat_barrier_tbl_dump_t *tbl_dump = &bs->op_state.tbl_dump;
        if (stat_mgr_enable_detail_trace) {
          PIPE_MGR_STAT_TRACE(stat_tbl->device_id,
                              stat_tbl->stat_tbl_hdl,
                              stat_tbl->name,
                              -1,
                              -1,
                              bs->op_state.tbl_dump.pipe_id,
                              -1,
                              "Invoking callback for table dump for tbl 0x%x",
                              stat_tbl->stat_tbl_hdl);
        }
        (tbl_dump->callback_fn)(stat_tbl->device_id, tbl_dump->user_cookie);
      }
    }
    PIPE_MGR_FREE(bs);
  }

  if (next_bl_ready) {
    PIPE_MGR_LOCK(&stat_tbl_instance->barrier_data_mtx);
    bl = stat_tbl_instance->barrier_list[pipe_id];
    stage_id = bl->barrier_state->stage_id;
    lock_id = bl->barrier_state->lock_id;
    PIPE_MGR_DBGCHK(bl->received_hw_ack);
    goto process_next;
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_drv_lrt_buf_init(pipe_sess_hdl_t h, bf_dev_id_t dev_id) {
  pipe_status_t ret = PIPE_SUCCESS;
  uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(dev_id);
  for (uint32_t subdev = 0; subdev < num_subdevices; subdev++) {
    pipe_mgr_drv_lrt_cfg_t *lrt_cfg =
        &pipe_mgr_drv_ctx()->lrt_cfg[dev_id][subdev];
    lrt_cfg->num_buffers =
        pipe_mgr_drv_subdev_buf_count(dev_id, subdev, PIPE_MGR_DRV_BUF_LRT);
    lrt_cfg->buf_size =
        pipe_mgr_drv_subdev_buf_size(dev_id, subdev, PIPE_MGR_DRV_BUF_LRT);
    uint32_t num_buffers = lrt_cfg->num_buffers;

    pipe_mgr_drv_ses_state_t *st;
    st = pipe_mgr_drv_get_ses_state(&h, __func__, __LINE__);
    if (NULL == st) {
      return PIPE_INVALID_ARG;
    }

    lrt_cfg->bufs = (pipe_mgr_drv_buf_t **)PIPE_MGR_CALLOC(
        num_buffers, sizeof(pipe_mgr_drv_buf_t *));

    if (lrt_cfg->bufs == NULL) {
      LOG_ERROR("%s:%d : Malloc failure", __func__, __LINE__);
      return PIPE_NO_SYS_RESOURCES;
    }

    unsigned i = 0;

    for (i = 0; i < num_buffers; i++) {
      pipe_mgr_drv_buf_t *b;
      b = pipe_mgr_drv_buf_alloc_subdev(st->sid,
                                        dev_id,
                                        subdev,
                                        lrt_cfg->buf_size,
                                        PIPE_MGR_DRV_BUF_LRT,
                                        false);

      if (!b) {
        LOG_ERROR("%s:%d Error allocating stats buffer for dev %d",
                  __func__,
                  __LINE__,
                  dev_id);
        ret = PIPE_NO_SYS_RESOURCES;
        goto cleanup;
      }

      lrt_cfg->bufs[i] = b;
    }

    /* Maintain an array of buffer physical addresses and a pointer into the
     * next expected buffer.
     */
    bf_phys_addr_t *lrt_buffer_array =
        (bf_phys_addr_t *)PIPE_MGR_CALLOC(num_buffers, sizeof(bf_phys_addr_t));
    if (lrt_buffer_array == NULL) {
      LOG_ERROR("%s:%d Malloc failure", __func__, __LINE__);
      ret = PIPE_NO_SYS_RESOURCES;
      goto cleanup;
    }
    lrt_cfg->buffer_array = lrt_buffer_array;
    lrt_cfg->next_expected_buffer_idx = 0;

    for (i = 0; i < num_buffers; i++) {
      pipe_mgr_drv_buf_t *b = lrt_cfg->bufs[i];
      lrt_cfg->buffer_array[i] = b->phys_addr;
    }
    /* Initialize the buffer index mutex and the associated conditional variable
     */
    PIPE_MGR_LOCK_INIT(lrt_cfg->lrt_buffer_array_mtx);
    PIPE_MGR_COND_INIT(lrt_cfg->lrt_buffer_array_condvar);

#if DEVICE_IS_EMULATOR
    /* Set a small data timeout on emulator */
    lld_subdev_dr_data_timeout_set(
        dev_id, subdev, lld_dr_fm_lrt, 0x2710); /* 10K cycles */
#else
    /* Program the timeout on the FM LRT DR to a value of 50ms. This is the
     * amount of time hardware waits before shipping the buffer to software;
     * hence this increases the utilization of these buffers by allowing
     * hardware to pack in more messages per buffer. The timeout value
     * is in clock cycles.
     */
    uint64_t clock_speed = pipe_mgr_get_bps_clock_speed(dev_id);
    uint32_t num_clocks =
        (PIPE_MGR_STAT_MGR_LRT_FM_TIMEOUT_US * clock_speed) / 1000000;
    lld_subdev_dr_data_timeout_set(dev_id, subdev, lld_dr_fm_lrt, num_clocks);
#endif
  }

  if (!pipe_mgr_is_device_locked(dev_id)) {
    ret = pipe_mgr_lrt_buf_load(dev_id);
    if (PIPE_SUCCESS != ret) {
      return ret;
    }
  }
  return PIPE_SUCCESS;

cleanup:
  return ret;
}

pipe_status_t pipe_mgr_drv_lrt_buf_warm_init_quick(bf_dev_id_t dev_id) {
  uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(dev_id);
  for (uint32_t subdev = 0; subdev < num_subdevices; subdev++) {
    pipe_mgr_drv_lrt_cfg_t *lrt_cfg =
        &pipe_mgr_drv_ctx()->lrt_cfg[dev_id][subdev];
    /* LR(t) buffers remain initialized in the fast reconfig quick mode.
     * However it is necessary to initialize the transient state again, namely
     * the next expected buffer index.
     */
    lrt_cfg->next_expected_buffer_idx = 0;
  }

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_lrt_buf_load(bf_dev_id_t dev_id) {
  uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(dev_id);
  for (uint32_t subdev = 0; subdev < num_subdevices; subdev++) {
    pipe_mgr_drv_lrt_cfg_t *lrt_cfg =
        &pipe_mgr_drv_ctx()->lrt_cfg[dev_id][subdev];

    for (uint32_t i = 0; i < lrt_cfg->num_buffers; i++) {
      bf_dma_addr_t dma_addr;
      pipe_mgr_drv_buf_t *b = lrt_cfg->bufs[i];
      /* Map the virtual address of the buffer to the DMA address before it is
         to the free memory DR */
      if (bf_sys_dma_map(b->pool,
                         b->addr,
                         b->phys_addr,
                         b->size,
                         &dma_addr,
                         BF_DMA_TO_CPU) != 0) {
        LOG_ERROR("Unable to map DMA buffer %p at %s:%d",
                  b->addr,
                  __func__,
                  __LINE__);
        return PIPE_COMM_FAIL;
      }
      int ret =
          lld_subdev_push_fm(dev_id, subdev, lld_dr_fm_lrt, dma_addr, b->size);
      if (ret != LLD_OK) {
        /* Unmap the buffer */
        if (bf_sys_dma_unmap(b->pool, b->addr, b->size, BF_DMA_TO_CPU) != 0) {
          LOG_ERROR("Unable to unmap DMA buffer %p at %s:%d",
                    b->addr,
                    __func__,
                    __LINE__);
        }
        LOG_ERROR(
            "%s Error pushing stats free memory to device %d rc %d addr "
            "0x%" PRIx64 "size %d",
            __func__,
            dev_id,
            ret,
            dma_addr,
            b->size);
        PIPE_MGR_DBGCHK(ret == LLD_OK);
        return PIPE_LLD_FAILED;
      }
      lrt_cfg->buffer_array[i] = b->phys_addr;
    }
  }

  /* Push the buffers */
  return pipe_mgr_drv_push_lrt_drs(dev_id);
}

pipe_status_t pipe_mgr_drv_lrt_buf_cleanup(bf_dev_id_t dev_id) {
  uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(dev_id);
  for (uint32_t subdev = 0; subdev < num_subdevices; subdev++) {
    pipe_mgr_drv_lrt_cfg_t *lrt_cfg =
        &pipe_mgr_drv_ctx()->lrt_cfg[dev_id][subdev];
    uint32_t num_buffers = lrt_cfg->num_buffers;

    unsigned i = 0;
    for (i = 0; i < num_buffers; i++) {
      pipe_mgr_drv_buf_free(lrt_cfg->bufs[i]);
    }

    if (lrt_cfg->num_buffers) {
      PIPE_MGR_FREE(lrt_cfg->bufs);
      lrt_cfg->bufs = NULL;
      lrt_cfg->num_buffers = 0;
    }
    PIPE_MGR_LOCK_DESTROY(&(lrt_cfg->lrt_buffer_array_mtx));
    PIPE_MGR_COND_DESTROY(&(lrt_cfg->lrt_buffer_array_condvar));
    /* Free the array of physical addresses allocated for the purposes of
     * serializing the lrt callback function execution. Buffers have to be
     * processed in order.
     */
    PIPE_MGR_FREE(lrt_cfg->buffer_array);
    lrt_cfg->buffer_array = NULL;
  }
  return PIPE_SUCCESS;
}

static bool pipe_mgr_stat_mgr_pending_responses(pipe_mgr_stat_tbl_t *stat_tbl) {
  for (unsigned i = 0; i < stat_tbl->num_instances; i++) {
    pipe_mgr_stat_tbl_instance_t *stat_tbl_instance = NULL;
    stat_tbl_instance = &stat_tbl->stat_tbl_instances[i];
    pipe_bitmap_t *pipe_bmp = &stat_tbl_instance->pipe_bmp;
    bf_dev_pipe_t pipe = 0;
    PIPE_MGR_LOCK(&stat_tbl_instance->barrier_data_mtx);
    PIPE_BITMAP_ITER(pipe_bmp, pipe) {
      if (stat_tbl_instance->barrier_list[pipe]) {
        PIPE_MGR_UNLOCK(&stat_tbl_instance->barrier_data_mtx);
        return true;
      }
    }
    PIPE_MGR_UNLOCK(&stat_tbl_instance->barrier_data_mtx);
  }
  return false;
}

pipe_status_t pipe_mgr_stat_mgr_complete_operations(
    bf_dev_id_t device_id, pipe_stat_tbl_hdl_t stat_tbl_hdl) {
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
  while (pipe_mgr_stat_mgr_pending_responses(stat_tbl)) {
    pipe_mgr_drv_service_drs(stat_tbl->device_id);
  }
  return PIPE_SUCCESS;
}
