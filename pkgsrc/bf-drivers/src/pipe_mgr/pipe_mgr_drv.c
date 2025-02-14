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
 * @file pipe_mgr_drv.c
 * @date
 *
 * Implementation of pipeline management driver interface
 */

#include <sched.h>
#ifdef __sun
#include <endian.h>
#endif
/* Module header files */
#include <sys/types.h>
#include <sched.h>
#include <dvm/bf_drv_intf.h>
#include <lld/bf_dma_if.h>
#include <lld/lld_reg_if.h>
#include <lld/lld_subdev_dr_if.h>

#include <pipe_mgr/pipe_mgr_intf.h>
#include <pipe_mgr/pipe_mgr_mirror_intf.h>
/* Local header files */
#include "pipe_mgr_int.h"
#include "pipe_mgr_drv.h"
#include "pipe_mgr_learn.h"
#include "pipe_mgr_stat_lrt.h"
#include "pipe_mgr_idle.h"
#include "pipe_mgr_db.h"

extern char *lld_reg_parse_get_full_reg_path_name(bf_dev_family_t dev_family,
                                                  uint32_t offset);

/* Pointer to global pipe_mgr context */
extern pipe_mgr_ctx_t *pipe_mgr_ctx;

/* Global Variables */
static struct pipe_mgr_drv_ctx_t drv_ctx;
struct pipe_mgr_drv_ctx_t *pipe_mgr_drv_ctx() {
  return &drv_ctx;
}

enum msgIdTypes {
  rd_blk_msgid_type = 0,
  wr_blk_msgid_type = 1,
  i_list_msgid_type = 2,
  i_list_rd_msgid_type = 3,
};

/* Static Function Prototypes */
static uint64_t pipe_mgr_drv_next_msgId(pipe_mgr_drv_ses_state_t *st,
                                        uint8_t dev_id,
                                        uint8_t type);
static void pipe_mgr_drv_parse_msgId(uint64_t msgId,
                                     uint8_t *sid,
                                     uint8_t *dev_id,
                                     uint8_t *type);

static pipe_status_t pipe_mgr_drv_buf_pool_init(bf_dev_id_t dev,
                                                bf_dma_info_t *dma_info);

static void rd_blk_cmplt(pipe_mgr_drv_ses_state_t *st,
                         uint64_t msgId,
                         bool hadError);
static void wr_blk_cmplt(pipe_mgr_drv_ses_state_t *st,
                         uint64_t msgId,
                         uint8_t dev_id,
                         bf_subdev_id_t subdev_id);
static void i_list_cmplt(pipe_mgr_drv_ses_state_t *st,
                         uint64_t msgId,
                         uint32_t errorCode,
                         uint8_t dev_id,
                         bf_subdev_id_t subdev_id,
                         uint8_t fifo);
static void i_list_rd_cmplt(pipe_mgr_drv_ses_state_t *st,
                            uint64_t msgId,
                            bool hadError,
                            uint8_t dev_id,
                            bf_subdev_id_t subdev_id,
                            uint8_t fifo);
static pipe_status_t pipe_mgr_drv_buf_pool_clean(bf_dev_id_t dev);

static pipe_status_t pipe_mgr_drv_ilist_add_2_(pipe_sess_hdl_t *sess,
                                               rmt_dev_info_t *dev_info,
                                               pipe_bitmap_t *pipe_bmp,
                                               uint8_t stage,
                                               uint8_t *instr,
                                               uint8_t instr_len,
                                               uint8_t *data,
                                               uint8_t data_len,
                                               bool is_internal_add);
static pipe_status_t ilist_add(pipe_mgr_drv_ses_state_t *st,
                               pipe_mgr_drv_list_op_t *il,
                               rmt_dev_info_t *dev_info,
                               bf_subdev_id_t subdev,
                               uint8_t phy_pipe_mask,
                               uint8_t *instr,
                               uint8_t instr_len,
                               uint8_t *data,
                               uint8_t data_len,
                               bool is_internal_add,
                               int reserved_space);

static pipe_status_t pipe_mgr_drv_ilist_push_alloc(pipe_sess_hdl_t *sess);
static pipe_status_t pipe_mgr_drv_ilist_push_int(pipe_mgr_drv_ses_state_t *st,
                                                 pipe_mgr_drv_list_op_t *il,
                                                 pipe_mgr_drv_ilist_cb cb_func,
                                                 void *usrData);

pipe_sess_hdl_t pipe_mgr_get_int_sess_hdl(void) {
  return pipe_mgr_ctx->int_ses_hndl;
}

static pipe_status_t service_dr(bf_dev_id_t dev_id, bf_dma_dr_id_t dr) {
  bf_subdev_id_t subdev_id = 0;
  uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(dev_id);
  for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
    /* LLD will ensure that it takes locks while pulling from the DR */
    int ret = lld_dr_service(dev_id, subdev_id, dr, 10000);
    if (ret < LLD_OK) {
      LOG_ERROR("%s:%d Error %d pulling DRs from hw on dev %d subdev %d, dr %d",
                __func__,
                __LINE__,
                ret,
                dev_id,
                subdev_id,
                dr);
      return PIPE_LLD_FAILED;
    }
  }
  return PIPE_SUCCESS;
}

static pipe_status_t push_dr(bf_dev_id_t dev_id, bf_dma_dr_id_t dr) {
  int ret;
  bf_subdev_id_t subdev_id = 0;

  uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(dev_id);
  for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
    /* LLD will ensure that it takes locks while pushing the DR */
    ret = lld_dr_start(dev_id, subdev_id, dr);
    if (ret != PIPE_SUCCESS) {
      LOG_ERROR("%s:%d Error 0x%x pushing DRs to hw on dev %d subdev %d dr %d",
                __func__,
                __LINE__,
                ret,
                dev_id,
                subdev_id,
                dr);
      return PIPE_LLD_FAILED;
    }
  }

  return PIPE_SUCCESS;
}

static void ilist_rd_buf_space(uint32_t buf_sz,
                               uint32_t *rd_sz,
                               uint32_t *rsp_offset) {
  /* Assume each read instruction is 32 bits.
   * Assume each read response is 128 bits.
   * Reads will fill the first 20% of the buffer
   * Read responses will be stored in the last 80% of the buffer
   * The responses must start at a 64 byte aligned address.
   * Divide the buffer into two at the 20/80 boundray enforcing the 64 byte
   * alignment requirement.
   * To be 64 byte aligned, reads should be done in groups of 16.
   * 16 reads consumes 64 bytes of request and 256 bytes of response, 320
   * bytes total.*/
  uint32_t num_rd_grps = buf_sz / 320;
  uint32_t rd_start = 0;
  uint32_t rd_end = rd_start + num_rd_grps * 16 * 4 - 1;
  uint32_t rx_start = rd_end + 1;
  // uint32_t rx_end      = rx_start + num_rd_grps * 16 * 16 - 1;

  if (rd_sz) *rd_sz = rd_end + 1;
  if (rsp_offset) *rsp_offset = rx_start;
  // 32,768 buf sz, 102 read groups, 1632 reads
  // Bytes 0 - 6527 (0-0x197F) hold read instructions
  // Bytes 6528 - 32639 (0x1980-0x7F7F) hold read data
}

pipe_status_t pipe_mgr_drv_service_ilist_drs(bf_dev_id_t dev_id) {
  pipe_status_t ret = PIPE_SUCCESS;
  bf_dma_dr_id_t dr, tx_end = 0, rx_end = 0;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  if (!dev_info) return PIPE_INVALID_ARG;
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      tx_end = lld_dr_tx_pipe_inst_list_3;
      rx_end = lld_dr_cmp_pipe_inst_list_3;
      break;
    case BF_DEV_FAMILY_TOFINO2:
      tx_end = lld_dr_tx_pipe_inst_list_1;
      rx_end = lld_dr_cmp_pipe_inst_list_1;
      break;
    case BF_DEV_FAMILY_TOFINO3:
      tx_end = lld_dr_tx_pipe_inst_list_1;
      rx_end = lld_dr_cmp_pipe_inst_list_1;
      break;







    case BF_DEV_FAMILY_UNKNOWN:
      PIPE_MGR_DBGCHK(0);
      return PIPE_INVALID_ARG;
  }

  for (dr = lld_dr_tx_pipe_inst_list_0; dr <= tx_end; dr++) {
    ret = service_dr(dev_id, dr);
    if (ret != PIPE_SUCCESS) {
      LOG_ERROR("%s:%d Error 0x%x pulling ilist DRs from hw on dev %d dr %d",
                __func__,
                __LINE__,
                ret,
                dev_id,
                dr);
      return PIPE_COMM_FAIL;
    }
  }
  for (dr = lld_dr_cmp_pipe_inst_list_0; dr <= rx_end; dr++) {
    ret = service_dr(dev_id, dr);
    if (ret != PIPE_SUCCESS) {
      LOG_ERROR("%s:%d Error 0x%x pulling ilist DRs from hw on dev %d dr %d",
                __func__,
                __LINE__,
                ret,
                dev_id,
                dr);
      return PIPE_COMM_FAIL;
    }
  }

  return PIPE_SUCCESS;
}
static pipe_status_t service_ilist_drs(bf_dev_id_t dev_id) {
#ifdef PIPE_MGR_INLINE_DR_SERVICE
  return pipe_mgr_drv_service_ilist_drs(dev_id);
#else
  (void)dev_id;
  return PIPE_SUCCESS;
#endif
}

static pipe_status_t push_ilist_drs(rmt_dev_info_t *dev_info) {
  pipe_status_t ret = PIPE_SUCCESS;
  bf_dma_dr_id_t dr, tx_end = 0;

  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      tx_end = lld_dr_tx_pipe_inst_list_3;
      break;
    case BF_DEV_FAMILY_TOFINO2:
      tx_end = lld_dr_tx_pipe_inst_list_1;
      break;
    case BF_DEV_FAMILY_TOFINO3:

      tx_end = lld_dr_tx_pipe_inst_list_1;
      break;
    case BF_DEV_FAMILY_UNKNOWN:
      PIPE_MGR_DBGCHK(0);
      return PIPE_INVALID_ARG;
  }

  for (dr = lld_dr_tx_pipe_inst_list_0; dr <= tx_end; dr++) {
    ret = push_dr(dev_info->dev_id, dr);
    if (ret != PIPE_SUCCESS) {
      LOG_ERROR("%s:%d Error 0x%x pushing ilist DRs to hw on dev %d dr %d",
                __func__,
                __LINE__,
                ret,
                dev_info->dev_id,
                dr);
      return PIPE_COMM_FAIL;
    }
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_drv_service_learn_drs(bf_dev_id_t dev_id) {
  pipe_status_t ret = PIPE_SUCCESS;
  bf_dma_dr_id_t dr = BF_DMA_NO_DR;

  dr = lld_dr_fm_learn;
  ret = service_dr(dev_id, dr);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Error 0x%x pulling learn DRs from hw on dev %d dr %d",
              __func__,
              __LINE__,
              ret,
              dev_id,
              dr);
    return PIPE_COMM_FAIL;
  }

  dr = lld_dr_rx_learn;
  ret = service_dr(dev_id, dr);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Error 0x%x pulling learn DRs from hw on dev %d dr %d",
              __func__,
              __LINE__,
              ret,
              dev_id,
              dr);
    return PIPE_COMM_FAIL;
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_drv_service_stats_drs(bf_dev_id_t dev_id) {
  pipe_status_t ret = PIPE_SUCCESS;
  bf_dma_dr_id_t dr = BF_DMA_NO_DR;

  dr = lld_dr_fm_lrt;
  ret = service_dr(dev_id, dr);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Error 0x%x pulling learn DRs from hw on dev %d dr %d",
              __func__,
              __LINE__,
              ret,
              dev_id,
              dr);
    return PIPE_COMM_FAIL;
  }

  dr = lld_dr_rx_lrt;
  ret = service_dr(dev_id, dr);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Error 0x%x pulling learn DRs from hw on dev %d dr %d",
              __func__,
              __LINE__,
              ret,
              dev_id,
              dr);
    return PIPE_COMM_FAIL;
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_drv_push_learn_drs(bf_dev_id_t dev_id) {
  pipe_status_t ret = PIPE_SUCCESS;
  bf_dma_dr_id_t dr = BF_DMA_NO_DR;

  if (pipe_mgr_is_device_locked(dev_id)) {
    return PIPE_SUCCESS;
  }

  dr = lld_dr_fm_learn;
  ret = push_dr(dev_id, dr);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Error 0x%x pushing learn DRs to hw on dev %d dr %d",
              __func__,
              __LINE__,
              ret,
              dev_id,
              dr);
    return PIPE_COMM_FAIL;
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_drv_push_lrt_drs(bf_dev_id_t dev_id) {
  pipe_status_t ret = PIPE_SUCCESS;
  bf_dma_dr_id_t dr = BF_DMA_NO_DR;

  if (pipe_mgr_is_device_locked(dev_id)) {
    return PIPE_SUCCESS;
  }

  dr = lld_dr_fm_lrt;
  ret = push_dr(dev_id, dr);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Error 0x%x pushing lrt DRs to hw on dev %d dr %d",
              __func__,
              __LINE__,
              ret,
              dev_id,
              dr);
    return PIPE_COMM_FAIL;
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_drv_service_idle_time_drs(bf_dev_id_t dev_id) {
  pipe_status_t ret = PIPE_SUCCESS;
  bf_dma_dr_id_t dr = BF_DMA_NO_DR;

  dr = lld_dr_fm_idle;
  ret = service_dr(dev_id, dr);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Error 0x%x pulling idle_time DRs from hw on dev %d dr %d",
              __func__,
              __LINE__,
              ret,
              dev_id,
              dr);
    return PIPE_COMM_FAIL;
  }

  dr = lld_dr_rx_idle;
  ret = service_dr(dev_id, dr);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Error 0x%x pulling idle_time DRs from hw on dev %d dr %d",
              __func__,
              __LINE__,
              ret,
              dev_id,
              dr);
    return PIPE_COMM_FAIL;
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_drv_push_idle_time_drs(bf_dev_id_t dev_id) {
  pipe_status_t ret = PIPE_SUCCESS;
  bf_dma_dr_id_t dr = BF_DMA_NO_DR;

  dr = lld_dr_fm_idle;
  ret = push_dr(dev_id, dr);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Error 0x%x pushing idle_time DRs to hw on dev %d dr %d",
              __func__,
              __LINE__,
              ret,
              dev_id,
              dr);
    return PIPE_COMM_FAIL;
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_drv_service_read_blk_drs(bf_dev_id_t dev_id) {
  pipe_status_t ret = PIPE_SUCCESS;
  bf_dma_dr_id_t dr = BF_DMA_NO_DR;

  dr = lld_dr_tx_pipe_read_block;
  ret = service_dr(dev_id, dr);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Error 0x%x pulling read_blk DRs from hw on dev %d dr %d",
              __func__,
              __LINE__,
              ret,
              dev_id,
              dr);
    return PIPE_COMM_FAIL;
  }

  dr = lld_dr_cmp_pipe_read_blk;
  ret = service_dr(dev_id, dr);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Error 0x%x pulling read_blk DRs from hw on dev %d dr %d",
              __func__,
              __LINE__,
              ret,
              dev_id,
              dr);
    return PIPE_COMM_FAIL;
  }
  return PIPE_SUCCESS;
}

static pipe_status_t push_read_blk_drs(bf_dev_id_t dev_id) {
  pipe_status_t ret = PIPE_SUCCESS;
  bf_dma_dr_id_t dr = BF_DMA_NO_DR;

  dr = lld_dr_tx_pipe_read_block;
  ret = push_dr(dev_id, dr);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Error 0x%x pushing read_blk DRs to hw on dev %d dr %d",
              __func__,
              __LINE__,
              ret,
              dev_id,
              dr);
    return PIPE_COMM_FAIL;
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_drv_service_write_blk_drs(bf_dev_id_t dev_id,
                                                 bool tx,
                                                 bool rx) {
  pipe_status_t ret = PIPE_SUCCESS;
  bf_dma_dr_id_t dr = BF_DMA_NO_DR;

  if (tx) {
    dr = lld_dr_tx_pipe_write_block;
    ret = service_dr(dev_id, dr);
    if (ret != PIPE_SUCCESS) {
      LOG_ERROR(
          "%s:%d Error 0x%x pulling write_blk DRs from hw on dev %d dr %d",
          __func__,
          __LINE__,
          ret,
          dev_id,
          dr);
      return PIPE_COMM_FAIL;
    }
  }

  if (rx) {
    dr = lld_dr_cmp_pipe_write_blk;
    ret = service_dr(dev_id, dr);
    if (ret != PIPE_SUCCESS) {
      LOG_ERROR(
          "%s:%d Error 0x%x pulling write_blk DRs from hw on dev %d dr %d",
          __func__,
          __LINE__,
          ret,
          dev_id,
          dr);
      return PIPE_COMM_FAIL;
    }
  }

  return PIPE_SUCCESS;
}

static pipe_status_t push_write_blk_drs(bf_dev_id_t dev_id) {
  pipe_status_t ret = PIPE_SUCCESS;
  bf_dma_dr_id_t dr = BF_DMA_NO_DR;

  dr = lld_dr_tx_pipe_write_block;
  ret = push_dr(dev_id, dr);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Error 0x%x pushing write_blk DRs to hw on dev %d dr %d",
              __func__,
              __LINE__,
              ret,
              dev_id,
              dr);
    return PIPE_COMM_FAIL;
  }
  return PIPE_SUCCESS;
}

void pipe_mgr_drv_service_drs(bf_dev_id_t dev_id) {
  pipe_mgr_drv_service_ilist_drs(dev_id);
  pipe_mgr_drv_service_stats_drs(dev_id);
  pipe_mgr_drv_service_idle_time_drs(dev_id);
  pipe_mgr_drv_service_read_blk_drs(dev_id);
  pipe_mgr_drv_service_write_blk_drs(dev_id, false, true);
}

/******************************************************************************
 *                                                                            *
 * A few utility functions.                                                   *
 *                                                                            *
 *****************************************************************************/
static void ilist_pending_dec(bf_dev_id_t dev,
                              bf_subdev_id_t subdev,
                              uint8_t dr) {
  PIPE_MGR_LOCK(&pipe_mgr_drv_ctx()->drv_ctx_mtx);
  --pipe_mgr_drv_ctx()->ilist_pending_cnt[dev][subdev][dr];
  PIPE_MGR_UNLOCK(&pipe_mgr_drv_ctx()->drv_ctx_mtx);
  PIPE_MGR_DBGCHK(pipe_mgr_drv_ctx()->ilist_pending_cnt[dev][subdev][dr] >= 0);
}
static void ilist_pending_inc(bf_dev_id_t dev,
                              bf_subdev_id_t subdev,
                              uint8_t dr) {
  PIPE_MGR_LOCK(&pipe_mgr_drv_ctx()->drv_ctx_mtx);
  ++pipe_mgr_drv_ctx()->ilist_pending_cnt[dev][subdev][dr];
  PIPE_MGR_UNLOCK(&pipe_mgr_drv_ctx()->drv_ctx_mtx);
  PIPE_MGR_DBGCHK(pipe_mgr_drv_ctx()->ilist_pending_cnt[dev][subdev][dr] > 0);
}
static bool ilist_pending_full(bf_dev_id_t dev) {
  /* Return true if any of the ilist Tx counts are greater than the size of
   * the response DR.  This means that more data was transmitted than will
   * fit in the response DR, so the response DR will flow control the Tx DR
   * and data will be stuck waiting in the Tx DR until the response DR is
   * serviced. */
  bool x = false;
  PIPE_MGR_LOCK(&pipe_mgr_drv_ctx()->drv_ctx_mtx);
  x = pipe_mgr_drv_ctx()->ilist_pending_cnt[dev][0][0] >
          pipe_mgr_drv_ctx()->ilist_dr_size[dev][0][0] ||
      pipe_mgr_drv_ctx()->ilist_pending_cnt[dev][0][1] >
          pipe_mgr_drv_ctx()->ilist_dr_size[dev][0][1] ||
      pipe_mgr_drv_ctx()->ilist_pending_cnt[dev][0][2] >
          pipe_mgr_drv_ctx()->ilist_dr_size[dev][0][2] ||
      pipe_mgr_drv_ctx()->ilist_pending_cnt[dev][0][3] >
          pipe_mgr_drv_ctx()->ilist_dr_size[dev][0][3];

  uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(dev);
  if (num_subdevices > 1) {
    x = x ||
        pipe_mgr_drv_ctx()->ilist_pending_cnt[dev][1][0] >
            pipe_mgr_drv_ctx()->ilist_dr_size[dev][1][0] ||
        pipe_mgr_drv_ctx()->ilist_pending_cnt[dev][1][1] >
            pipe_mgr_drv_ctx()->ilist_dr_size[dev][1][1] ||
        pipe_mgr_drv_ctx()->ilist_pending_cnt[dev][1][2] >
            pipe_mgr_drv_ctx()->ilist_dr_size[dev][1][2] ||
        pipe_mgr_drv_ctx()->ilist_pending_cnt[dev][1][3] >
            pipe_mgr_drv_ctx()->ilist_dr_size[dev][1][3];
  }
  PIPE_MGR_UNLOCK(&pipe_mgr_drv_ctx()->drv_ctx_mtx);
  return x;
}
static uint64_t pipe_mgr_drv_next_msgId(pipe_mgr_drv_ses_state_t *st,
                                        uint8_t dev_id,
                                        uint8_t type) {
  PIPE_MGR_ASSERT(st);
  uint64_t msgId = ++st->msgId;
  /* To make ids unique across sessions, include the session number as the
   * MSB of the ID.
   * Also, for convience, include the device the buffer will be sent to and
   * the type of operation it will be used for. */
  uint64_t msgIdHi = (st->sid << 16) | (dev_id << 8) | type;
  msgId =
      (msgId & (uint64_t)0x00000000FFFFFFFF) | (msgIdHi << (64 - 8 - 8 - 8));
  return msgId;
}

static void pipe_mgr_drv_parse_msgId(uint64_t msgId,
                                     uint8_t *sid,
                                     uint8_t *dev_id,
                                     uint8_t *type) {
  if (sid) *sid = (msgId >> 56) & 0xFF;
  if (dev_id) *dev_id = (msgId >> 48) & 0xFF;
  if (type) *type = (msgId >> 40) & 0xFF;
}

static pipe_mgr_drv_ses_state_t *pipe_mgr_drv_msgId_to_sess(uint64_t msgId) {
  uint8_t sid;
  pipe_mgr_drv_parse_msgId(msgId, &sid, NULL, NULL);
  unsigned i;
  for (i = 0; i < PIPE_MGR_MAX_SESSIONS; ++i) {
    if (sid == pipe_mgr_drv_ctx()->sesStates[i].sid) {
      return &pipe_mgr_drv_ctx()->sesStates[i];
    }
  }
  return NULL;
}

pipe_mgr_drv_ses_state_t *pipe_mgr_drv_get_ses_state(pipe_sess_hdl_t *sess_hdl,
                                                     const char *where,
                                                     const int line) {
  if (!pipe_mgr_valid_session(sess_hdl, where, line)) {
    LOG_ERROR("No session state for session %u at %s:%d",
              sess_hdl ? *sess_hdl : ~0u,
              __func__,
              __LINE__);
    return NULL;
  }
  if (pipe_mgr_drv_ctx()->sesStates[*sess_hdl].sid != *sess_hdl) {
    LOG_ERROR("Session handle %u doesn't match sid %u at %s:%d",
              *sess_hdl,
              pipe_mgr_drv_ctx()->sesStates[*sess_hdl].sid,
              where,
              line);
    PIPE_MGR_DBGCHK(pipe_mgr_drv_ctx()->sesStates[*sess_hdl].sid == *sess_hdl);
  }
  return &pipe_mgr_drv_ctx()->sesStates[*sess_hdl];
}

void pipe_mgr_drv_buf_check(bool allFree) {
  (void)allFree;

  /* Count allocated buffers by going through the used arrays in each pool. */

  /* Query the dma library to find how many buffers are free/used. */

  /* Verify that they add up to the expected number. */
}

static void printOneBuf(pipe_mgr_drv_buf_t *b, unsigned idx) {
  LOG_TRACE(
      "%3u: Buffer %p Addr %p Sz 0x%-4x Used 0x%-4x "
      "Dev %u MsgId 0x%-16" PRIx64 " PshMsk %x PipeMsk %x [%p %p]",
      idx,
      (void *)b,
      b->addr,
      b->size,
      b->used,
      b->devId,
      b->msgId,
      b->buf_pushed,
      b->pipeMask,
      (void *)b->prev,
      (void *)b->next);
}
static void printOneBufPool(pipe_mgr_drv_buf_pool_t *p) {
  LOG_TRACE("Buffer Pool: %p", (void *)p);
  enum pipe_mgr_drv_buf_type type;
  for (type = PIPE_MGR_DRV_BUF_FIRST; type < PIPE_MGR_DRV_BUF_CNT; ++type) {
    LOG_TRACE("  %s: Handle %p BufSz %#x BufCnt %#x",
              PIPE_MGR_DRV_BUF_IL == type
                  ? "IL "
                  : PIPE_MGR_DRV_BUF_LRN == type
                        ? "LRN"
                        : PIPE_MGR_DRV_BUF_LRT == type
                              ? "LRT"
                              : PIPE_MGR_DRV_BUF_IDL == type
                                    ? "IDL"
                                    : PIPE_MGR_DRV_BUF_BRD == type
                                          ? "BRD"
                                          : PIPE_MGR_DRV_BUF_BWR == type
                                                ? "BWR"
                                                : "???",
              p->pool[type],
              p->buf_sz[type],
              p->buf_cnt[type]);
    LOG_TRACE("  Used List:");
    unsigned int i = 0;
    for (i = 0; i < p->buf_cnt[type]; ++i) {
      pipe_mgr_drv_buf_t *b = &p->used[type][i];
      if (b->addr) printOneBuf(b, i);
    }
  }
}
void pipe_mgr_drv_dump_state(void) {
  unsigned i, dev;
  uint32_t num_subdevices = 0;
  int subdev_id = 0;
  for (dev = 0; dev < PIPE_MGR_NUM_DEVICES; ++dev) {
    num_subdevices = pipe_mgr_get_num_active_subdevices(dev);
    for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
      LOG_TRACE("Device %u, subdevice: %u", dev, subdev_id);
      printOneBufPool(&pipe_mgr_drv_ctx()->gBufPool[dev][subdev_id]);
    }
  }
  for (i = 0; i < PIPE_MGR_MAX_SESSIONS; ++i) {
    pipe_mgr_drv_ses_state_t *s = &pipe_mgr_drv_ctx()->sesStates[i];
    LOG_TRACE("Sess %u (%u):", i, s->sid);
    if (s->rdBlk) {
      pipe_mgr_drv_rd_blk_op_t *op;
      for (op = s->rdBlk; op; op = op->next) {
        LOG_TRACE("ReadBlk: %d Ops CbFunc %s UsrData %p",
                  op->num_sub_ops,
                  op->cb_func ? "SET" : "CLR",
                  op->usrData);
        for (int j = 0; j < op->num_sub_ops; ++j) {
          struct pipe_mgr_drv_rd_blk_sub_op_t *sop = op->sub_ops + j;
          LOG_TRACE("  Op %2d: Buf %p (%p) MsgId 0x%" PRIx64,
                    j,
                    (void *)sop->buf,
                    (void *)(sop->buf ? sop->buf->addr : NULL),
                    sop->msgId);
        }
      }
    } else {
      LOG_TRACE("ReadBlk: none pending");
    }
    if (s->wrBlk) {
      pipe_mgr_drv_wr_blk_op_t *op;
      for (op = s->wrBlk; op; op = op->next) {
        LOG_TRACE("WriteBlk: MsgId 0x%" PRIx64 " Buf %p Op %p [%p %p]",
                  op->msgId,
                  (void *)op->buf,
                  (void *)op,
                  (void *)op->prev,
                  (void *)op->next);
      }
    } else {
      LOG_TRACE("WriteBlk: none pending");
    }
    if (s->iListPending) {
      pipe_mgr_drv_list_op_t *op;
      for (op = s->iListPending; op; op = op->next) {
        LOG_TRACE("IList: Ack %s BufCnt %u CbFunc %s UsrData %p op %p [%p %p]",
                  op->needsAck ? "T" : "F",
                  op->bufCnt,
                  op->cb_func ? "SET" : "CLR",
                  op->usrData,
                  (void *)op,
                  (void *)op->prev,
                  (void *)op->next);
        unsigned d;
        for (d = 0; d < PIPE_MGR_NUM_DEVICES; ++d) {
          rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(d);
          if (!dev_info) continue;
          num_subdevices = pipe_mgr_get_num_active_subdevices(d);
          for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
            LOG_TRACE("  Dev %u Subdev %d Buf %p FirstLock 0x%x lockCnt 0x%x",
                      d,
                      subdev_id,
                      (void *)op->bufs[d][subdev_id],
                      op->firstLock[d][subdev_id],
                      op->lockCnt[d][subdev_id]);
          }
        }
      }
    } else {
      LOG_TRACE("IList: none in progress");
    }
    for (dev = 0; dev < PIPE_MGR_NUM_DEVICES; dev++) {
      rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
      if (!dev_info) continue;
      if (s->iListReconfig[dev]) {
        pipe_mgr_drv_list_op_t *op;
        for (op = s->iListReconfig[dev]; op; op = op->next) {
          LOG_TRACE(
              "IListRcfg(dev%d): Ack %s BufCnt %u CbFunc %s UsrData %p op %p "
              "[%p %p]",
              dev,
              op->needsAck ? "T" : "F",
              op->bufCnt,
              op->cb_func ? "SET" : "CLR",
              op->usrData,
              (void *)op,
              (void *)op->prev,
              (void *)op->next);
          unsigned d;
          d = dev;
          num_subdevices = pipe_mgr_get_num_active_subdevices(dev);
          for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
            LOG_TRACE("  Dev %u Subdev %d Buf %p FirstLock 0x%x lockCnt 0x%x",
                      d,
                      subdev_id,
                      (void *)op->bufs[d][subdev_id],
                      op->firstLock[d][subdev_id],
                      op->lockCnt[d][subdev_id]);
          }
        }
      } else {
        LOG_TRACE("IListRcfg (dev %d): none in progress", dev);
      }
    }
    if (s->iList) {
      pipe_mgr_drv_list_op_t *op;
      for (op = s->iList; op; op = op->next) {
        LOG_TRACE("IList: Ack %s BufCnt %u CbFunc %s UsrData %p op %p [%p %p]",
                  op->needsAck ? "T" : "F",
                  op->bufCnt,
                  op->cb_func ? "SET" : "CLR",
                  op->usrData,
                  (void *)op,
                  (void *)op->prev,
                  (void *)op->next);
        unsigned d;
        for (d = 0; d < PIPE_MGR_NUM_DEVICES; ++d) {
          rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(d);
          if (!dev_info) continue;
          num_subdevices = pipe_mgr_get_num_active_subdevices(d);
          for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
            LOG_TRACE("  Dev %u Subdev %d Buf %p FirstLock 0x%x lockCnt 0x%x",
                      d,
                      subdev_id,
                      (void *)op->bufs[d][subdev_id],
                      op->firstLock[d][subdev_id],
                      op->lockCnt[d][subdev_id]);
          }
        }
      }
    } else {
      LOG_TRACE("IList: none pending");
    }
  }
}

/******************************************************************************
 *                                                                            *
 * Functions which are private to the Pipeline Manager Driver Interface; that *
 * is, they are only to implement the public APIs and are not useful outside  *
 * of that.                                                                   *
 *                                                                            *
 *****************************************************************************/
pipe_status_t pipe_mgr_drv_init() {
  LOG_TRACE("Entering %s", __func__);

  PIPE_MGR_MEMSET(pipe_mgr_drv_ctx(), 0, sizeof(struct pipe_mgr_drv_ctx_t));

  unsigned i, j;
  for (i = 0; i < PIPE_MGR_MAX_SESSIONS; ++i) {
    PIPE_MGR_LOCK_INIT(pipe_mgr_drv_ctx()->sesStates[i].mtx_ses);
  }
  for (i = 0; i < PIPE_MGR_NUM_DEVICES; ++i) {
    for (j = 0; j < BF_MAX_SUBDEV_COUNT; ++j) {
      PIPE_MGR_LOCK_INIT(pipe_mgr_drv_ctx()->learn_cfg[i][j].learn_mtx);
      PIPE_MGR_LOCK_INIT(pipe_mgr_drv_ctx()->learn_cfg[i][j].trace_mtx);
    }
  }
  PIPE_MGR_LOCK_INIT(pipe_mgr_drv_ctx()->drv_ctx_mtx);
  for (i = 0; i < PIPE_MGR_MAX_SESSIONS; ++i) {
    pipe_mgr_drv_ctx()->sesStates[i].sid = i;
  }

  LOG_TRACE("Exiting %s", __func__);
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_drv_init_dev(uint8_t dev_id, bf_dma_info_t *dma_info) {
  LOG_TRACE("Entering %s for dev %u", __func__, dev_id);

  if (pipe_mgr_drv_ctx()->valid[dev_id]) {
    LOG_ERROR("Skip drv init, Device %d already exists", dev_id);
    return PIPE_SUCCESS;
  }

  pipe_status_t sts = pipe_mgr_drv_buf_pool_init(dev_id, dma_info);
  if (PIPE_SUCCESS != sts) {
    LOG_ERROR("%s: Failed to setup buffer pool, dev %u, sts %s (%d)",
              __func__,
              dev_id,
              pipe_str_err(sts),
              sts);
    return sts;
  }

  dr_completion_callback_fn cmplt_cb;
  bf_subdev_id_t subdev_id = 0;
  uint32_t num_subdevices = 0, subdev_mask = 0;
  lld_sku_get_num_subdev(dev_id, &num_subdevices, &subdev_mask);
  for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
    // Instruction List DMA and Write Block completion DMA could be serviced by
    // multiple threads simultaneously. Mark all these DRs as "lock required".
    int x = lld_subdev_dr_lock_required(
        dev_id, subdev_id, lld_dr_tx_pipe_inst_list_0);
    if (x != LLD_OK) return PIPE_NO_SYS_RESOURCES;
    x = lld_subdev_dr_lock_required(
        dev_id, subdev_id, lld_dr_tx_pipe_inst_list_1);
    if (x != LLD_OK) return PIPE_NO_SYS_RESOURCES;
    x = lld_subdev_dr_lock_required(
        dev_id, subdev_id, lld_dr_tx_pipe_inst_list_2);
    if (x != LLD_OK) return PIPE_NO_SYS_RESOURCES;
    x = lld_subdev_dr_lock_required(
        dev_id, subdev_id, lld_dr_tx_pipe_inst_list_3);
    if (x != LLD_OK) return PIPE_NO_SYS_RESOURCES;
    x = lld_subdev_dr_lock_required(
        dev_id, subdev_id, lld_dr_tx_pipe_write_block);
    if (x != LLD_OK) return PIPE_NO_SYS_RESOURCES;
    x = lld_subdev_dr_lock_required(
        dev_id, subdev_id, lld_dr_tx_pipe_read_block);
    if (x != LLD_OK) return PIPE_NO_SYS_RESOURCES;
    x = lld_subdev_dr_lock_required(
        dev_id, subdev_id, lld_dr_cmp_pipe_inst_list_0);
    if (x != LLD_OK) return PIPE_NO_SYS_RESOURCES;
    x = lld_subdev_dr_lock_required(
        dev_id, subdev_id, lld_dr_cmp_pipe_inst_list_1);
    if (x != LLD_OK) return PIPE_NO_SYS_RESOURCES;
    x = lld_subdev_dr_lock_required(
        dev_id, subdev_id, lld_dr_cmp_pipe_inst_list_2);
    if (x != LLD_OK) return PIPE_NO_SYS_RESOURCES;
    x = lld_subdev_dr_lock_required(
        dev_id, subdev_id, lld_dr_cmp_pipe_inst_list_3);
    if (x != LLD_OK) return PIPE_NO_SYS_RESOURCES;
    x = lld_subdev_dr_lock_required(
        dev_id, subdev_id, lld_dr_cmp_pipe_write_blk);
    if (x != LLD_OK) return PIPE_NO_SYS_RESOURCES;
    x = lld_subdev_dr_lock_required(
        dev_id, subdev_id, lld_dr_cmp_pipe_read_blk);
    if (x != LLD_OK) return PIPE_NO_SYS_RESOURCES;
    x = lld_subdev_dr_lock_required(dev_id, subdev_id, lld_dr_rx_lrt);
    if (x != LLD_OK) return PIPE_NO_SYS_RESOURCES;
    x = lld_subdev_dr_lock_required(dev_id, subdev_id, lld_dr_fm_lrt);
    if (x != LLD_OK) return PIPE_NO_SYS_RESOURCES;
    x = lld_subdev_dr_lock_required(dev_id, subdev_id, lld_dr_rx_idle);
    if (x != LLD_OK) return PIPE_NO_SYS_RESOURCES;
    x = lld_subdev_dr_lock_required(dev_id, subdev_id, lld_dr_fm_idle);
    if (x != LLD_OK) return PIPE_NO_SYS_RESOURCES;

    cmplt_cb = (dr_completion_callback_fn)pipe_mgr_drv_completion_cb;
    // Register callbacks with LLD.
    for (int i = 0; i < 4; ++i) {
      if (0 >
          lld_register_completion_callback(
              dev_id, subdev_id, lld_dr_cmp_pipe_inst_list_0 + i, cmplt_cb)) {
        LOG_ERROR("LLD callback registration fails, dev %u, fifo %u at %s:%d",
                  dev_id,
                  lld_dr_cmp_pipe_inst_list_0 + i,
                  __func__,
                  __LINE__);
        return PIPE_COMM_FAIL;
      }
    }
    if (0 > lld_register_completion_callback(
                dev_id, subdev_id, lld_dr_cmp_pipe_write_blk, cmplt_cb)) {
      LOG_ERROR("LLD callback registration fails, dev %u, fifo %u at %s:%d",
                dev_id,
                lld_dr_cmp_pipe_write_blk,
                __func__,
                __LINE__);
      return PIPE_COMM_FAIL;
    }
    if (0 > lld_register_completion_callback(
                dev_id, subdev_id, lld_dr_cmp_pipe_read_blk, cmplt_cb)) {
      LOG_ERROR("LLD callback registration fails, dev %u, fifo %u at %s:%d",
                dev_id,
                lld_dr_cmp_pipe_read_blk,
                __func__,
                __LINE__);
      return PIPE_COMM_FAIL;
    }

    if (0 > lld_register_rx_learn_callback(
                dev_id, subdev_id, pipe_mgr_drv_learn_cb)) {
      LOG_ERROR("LLD learn callback registration fails, dev %u at %s:%d",
                dev_id,
                __func__,
                __LINE__);
      return PIPE_COMM_FAIL;
    }

    if (0 >
        lld_register_rx_lrt_callback(dev_id, subdev_id, pipe_mgr_drv_lrt_cb)) {
      LOG_ERROR("LLD lrt callback registration fails, dev %u at %s:%d",
                dev_id,
                __func__,
                __LINE__);
      return PIPE_COMM_FAIL;
    }

    if (0 > lld_register_rx_idle_callback(
                dev_id, subdev_id, pipe_mgr_drv_idle_cb)) {
      LOG_ERROR("LLD idle callback registration fails, dev %u at %s:%d",
                dev_id,
                __func__,
                __LINE__);
      return PIPE_COMM_FAIL;
    }
  }

  pipe_mgr_drv_ctx()->valid[dev_id] = true;
  LOG_TRACE("Exiting %s for dev %u", __func__, dev_id);
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_drv_setup_fm_buffers(pipe_sess_hdl_t shdl,
                                            bf_dev_id_t dev_id) {
  pipe_status_t ret = PIPE_SUCCESS;

  /* Reserve buffers for learning */
  ret = pipe_mgr_drv_learn_buf_init(shdl, dev_id);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Error reserving learn buffers for dev %d ret 0x%x",
              __func__,
              __LINE__,
              dev_id,
              ret);
    return ret;
  }

  /* Reserve buffers for LR(t) */
  ret = pipe_mgr_drv_lrt_buf_init(shdl, dev_id);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Error reserving lrt buffers for dev %d ret 0x%x",
              __func__,
              __LINE__,
              dev_id,
              ret);
    return ret;
  }

  /* Reserve buffers for Idle-time updates */
  ret = pipe_mgr_drv_idl_buf_init(shdl, dev_id);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Error reserving idle buffers for dev %d ret 0x%x",
              __func__,
              __LINE__,
              dev_id,
              ret);
    return ret;
  }

  return PIPE_SUCCESS;
}

void pipe_mgr_drv_cleanup_fm_buffers(bf_dev_id_t dev_id) {
  pipe_mgr_drv_learn_buf_cleanup(dev_id);
  pipe_mgr_drv_lrt_buf_cleanup(dev_id);
  pipe_mgr_drv_idl_buf_cleanup(dev_id);
}

pipe_status_t pipe_mgr_drv_load_fm_buffers(bf_dev_id_t dev_id,
                                           bf_dev_init_mode_t warm_init_mode) {
  pipe_status_t ret = PIPE_SUCCESS;

  ret = pipe_mgr_learn_buf_load(dev_id, warm_init_mode);
  if (PIPE_SUCCESS != ret) {
    LOG_ERROR("Failed to load learning free memory, dev %d sts %s",
              dev_id,
              pipe_str_err(ret));
    return ret;
  }
  ret = pipe_mgr_lrt_buf_load(dev_id);
  if (PIPE_SUCCESS != ret) {
    LOG_ERROR("Failed to load stats free memory, dev %d sts %s",
              dev_id,
              pipe_str_err(ret));
    return ret;
  }
  ret = pipe_mgr_idl_buf_load(dev_id);
  if (PIPE_SUCCESS != ret) {
    LOG_ERROR("Failed to load idle free memory, dev %d sts %s",
              dev_id,
              pipe_str_err(ret));
    return ret;
  }

  return ret;
}

/* Initialize the instruction list Descriptor Ring (DR) sizes in the device
 * context block. */
pipe_status_t pipe_mgr_drv_get_dr_size(bf_dev_id_t dev_id) {
  unsigned i;
  bf_subdev_id_t subdev_id = 0;
  uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(dev_id);
  for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
    for (i = 0; i < 4; ++i) {
      int x = lld_subdev_dr_depth_get(
          dev_id, subdev_id, lld_dr_cmp_pipe_inst_list_0 + i);
      if (0 > x) {
        LOG_ERROR(
            "Cannot determine instruction list cmpl DR[%u] size, dev %u"
            " subdev %d, sts %d",
            i,
            dev_id,
            subdev_id,
            x);
        return PIPE_INVALID_ARG;
      }
      pipe_mgr_drv_ctx()->ilist_dr_size[dev_id][subdev_id][i] = x;
      pipe_mgr_drv_ctx()->ilist_pending_cnt[dev_id][subdev_id][i] = 0;
    }
  }

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_drv_prepare_device(pipe_sess_hdl_t shdl,
                                          rmt_dev_info_t *dev_info) {
  unsigned i;
  pipe_bitmap_t pipe_bmp;
  PIPE_BITMAP_INIT(&pipe_bmp, PIPE_BMP_SIZE);

  bf_dev_id_t dev_id = dev_info->dev_id;

  /* Send a "Set Dest" to each of the 4 LLD FIFOs/DRs to associate them with
   * a specific pipeline. */
  for (i = 0; i < dev_info->num_active_pipes; ++i) {
    pipe_status_t ret;
    dest_select_t dest;
    bf_dev_pipe_t phy_pipe = 0;

    pipe_mgr_map_pipe_id_log_to_phy(dev_info, i, &phy_pipe);
    construct_instr_dest_select(
        dev_id, &dest, phy_pipe % BF_SUBDEV_PIPE_COUNT, 0);

    PIPE_BITMAP_CLR_ALL(&pipe_bmp);
    PIPE_BITMAP_SET(&pipe_bmp, i);

    ret = pipe_mgr_drv_ilist_add_2_(&shdl,
                                    dev_info,
                                    &pipe_bmp,
                                    0,
                                    (uint8_t *)&dest,
                                    sizeof(dest),
                                    NULL,
                                    0,
                                    true);
    if (ret != PIPE_SUCCESS) {
      PIPE_MGR_DBGCHK(ret == PIPE_SUCCESS);
      return ret;
    }
  }

  return PIPE_SUCCESS;
}

/* pipe_mgr_drv_device_cleanup
 * Called during the removal of a device to cleanup state
 */
pipe_status_t pipe_mgr_drv_device_cleanup(bf_dev_id_t dev_id) {
  LOG_TRACE("Entering %s for dev %u", __func__, dev_id);

  if (!pipe_mgr_valid_deviceId(dev_id, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  if (!dev_info) return PIPE_INVALID_ARG;
  uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(dev_id);

  int s;
  for (s = 0; s < PIPE_MGR_MAX_SESSIONS; ++s) {
    pipe_mgr_drv_ses_state_t *st = &pipe_mgr_drv_ctx()->sesStates[s];
    PIPE_MGR_LOCK(&st->mtx_ses);
    /* Clean up read blocks for this device. */
    pipe_mgr_drv_rd_blk_op_t *rb, *next_rb;
    for (rb = st->rdBlk; rb; rb = next_rb) {
      next_rb = rb->next;
      if (rb->sub_ops[0].buf->devId != dev_id) continue;
      PIPE_MGR_DLL_REM(st->rdBlk, rb, next, prev);
      PIPE_MGR_FREE(rb->sub_ops);
      PIPE_MGR_FREE(rb);
    }
    /* Clean up write blocks for this device. */
    pipe_mgr_drv_wr_blk_op_t *wb, *next_wb;
    for (wb = st->wrBlk; wb; wb = next_wb) {
      next_wb = wb->next;
      if (wb->buf->devId != dev_id) continue;
      PIPE_MGR_DLL_REM(st->wrBlk, wb, next, prev);
      PIPE_MGR_FREE(wb);
    }
    /* Clean up instruction lists for this device. */
    pipe_mgr_drv_list_op_t *l, *next_l;
    int subdev_id = 0;
    for (l = st->iListPending; l; l = NULL) {
      for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
        l->firstLock[dev_id][subdev_id] = 0;
        l->lockCnt[dev_id][subdev_id] = 0;
        l->stage[dev_id][subdev_id] = 0;
        l->curDest[dev_id][subdev_id] = 0;
        pipe_mgr_drv_buf_t *b;
        for (b = l->bufs[dev_id][subdev_id]; b; b = b->next) {
          --l->bufCnt;
        }
        l->bufs[dev_id][subdev_id] = NULL;
      }
      if (!l->bufCnt) {
        PIPE_MGR_FREE(l);
        st->iListPending = NULL;
      }
    }
    pipe_mgr_drv_rd_list_op_t *lr, *next_lr;
    for (lr = st->iListRdPending; lr;) {
      if (lr->dev_id != dev_id) {
        lr = lr->next;
        continue;
      }
      int p;
      for (p = 0; p < (int)dev_info->dev_cfg.num_pipelines; ++p) {
        pipe_mgr_drv_buf_t *b;
        for (b = lr->bufs[p]; b; b = b->next) {
          /* If the buffer is pushed, tag it so the code doing the freeing just
           * below can unmap it in the correct direction.  Note that since this
           * is the pending read list we don't expect any to be pushed. */
          if (b->buf_pushed) b->buf_pushed = 0xFF;
        }
      }
      PIPE_MGR_DLL_REM(st->iListRdPending, lr, next, prev);
      free_pipe_mgr_drv_rd_list_op_t(lr);
      lr = st->iListRdPending;
    }
    for (l = st->iList; l; l = next_l) {
      next_l = l->next;
      for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
        l->firstLock[dev_id][subdev_id] = 0;
        l->lockCnt[dev_id][subdev_id] = 0;
        l->stage[dev_id][subdev_id] = 0;
        l->curDest[dev_id][subdev_id] = 0;
        pipe_mgr_drv_buf_t *b;
        for (b = l->bufs[dev_id][subdev_id]; b; b = b->next) {
          --l->bufCnt;
        }
        l->bufs[dev_id][subdev_id] = NULL;
      }
      if (!l->bufCnt) {
        PIPE_MGR_DLL_REM(st->iList, l, next, prev);
        PIPE_MGR_FREE(l);
      }
    }
    for (lr = st->iListRd; lr; lr = next_lr) {
      next_lr = lr->next;
      if (lr->dev_id != dev_id) continue;
      int p;
      for (p = 0; p < (int)dev_info->dev_cfg.num_pipelines; ++p) {
        pipe_mgr_drv_buf_t *b;
        for (b = lr->bufs[p]; b; b = b->next) {
          /* If the buffer is pushed, tag it so the code doing the freeing just
           * below can unmap it in the correct direction.  Note that since this
           * is the pending read list we don't expect any to be pushed. */
          if (b->buf_pushed) b->buf_pushed = 0xFF;
        }
      }
      PIPE_MGR_DLL_REM(st->iListRd, lr, next, prev);
      free_pipe_mgr_drv_rd_list_op_t(lr);
    }
    for (l = st->iListReconfig[dev_id]; l; l = next_l) {
      next_l = l->next;
      for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
        l->firstLock[dev_id][subdev_id] = 0;
        l->lockCnt[dev_id][subdev_id] = 0;
        l->stage[dev_id][subdev_id] = 0;
        l->curDest[dev_id][subdev_id] = 0;
        pipe_mgr_drv_buf_t *b;
        for (b = l->bufs[dev_id][subdev_id]; b; b = b->next) {
          --l->bufCnt;
        }
        l->bufs[dev_id][subdev_id] = NULL;
      }
      if (!l->bufCnt) {
        st->iListReconfig[dev_id] = NULL;
        PIPE_MGR_FREE(l);
      }
    }

    PIPE_MGR_UNLOCK(&st->mtx_ses);
  }

  /* Go through each buffer pool to free allocated buffers. */

  pipe_mgr_disable_all_dr(dev_id);
  for (uint32_t subdev = 0; subdev < num_subdevices; subdev++) {
    pipe_mgr_drv_buf_pool_t *bp = &pipe_mgr_drv_ctx()->gBufPool[dev_id][subdev];

    enum pipe_mgr_drv_buf_type t;
    for (t = PIPE_MGR_DRV_BUF_FIRST; t != PIPE_MGR_DRV_BUF_CNT; ++t) {
      /* Go through each buffer in the pool. */
      unsigned int i;
      for (i = 0; i < bp->buf_cnt[t]; ++i) {
        /* Skip over unallocated buffers. */
        if (!bp->used[t][i].addr) continue;
        /* Only for buffers allocated to the dev_id. */
        if (bp->used[t][i].devId != dev_id) continue;
        /* If it is pushed, also unmap it. */
        if (bp->used[t][i].buf_pushed) {
          int unmap_sts = 0;
          if (t == PIPE_MGR_DRV_BUF_LRN || t == PIPE_MGR_DRV_BUF_LRT ||
              t == PIPE_MGR_DRV_BUF_IDL || t == PIPE_MGR_DRV_BUF_BRD) {
            unmap_sts = bf_sys_dma_unmap(bp->pool[t],
                                         bp->used[t][i].addr,
                                         bp->used[t][i].size,
                                         BF_DMA_TO_CPU);
          } else if (t == PIPE_MGR_DRV_BUF_BWR) {
            unmap_sts = bf_sys_dma_unmap(bp->pool[t],
                                         bp->used[t][i].addr,
                                         bp->used[t][i].size,
                                         BF_DMA_FROM_CPU);
          } else if (t == PIPE_MGR_DRV_BUF_IL) {
            if (0xFF == bp->used[t][i].buf_pushed) {
              uint32_t buf_sz = pipe_mgr_drv_subdev_buf_size(
                  dev_id, subdev, PIPE_MGR_DRV_BUF_IL);
              uint32_t resp_start = 0;
              ilist_rd_buf_space(buf_sz, NULL, &resp_start);
              /* Has special tag set above indicating a read operation. */
              unmap_sts = bf_sys_dma_unmap(bp->pool[t],
                                           bp->used[t][i].addr,
                                           resp_start,
                                           BF_DMA_FROM_CPU);
              unmap_sts |= bf_sys_dma_unmap(bp->pool[t],
                                            bp->used[t][i].addr + resp_start,
                                            bp->used[t][i].size - resp_start,
                                            BF_DMA_TO_CPU);
            } else {
              unmap_sts = bf_sys_dma_unmap(bp->pool[t],
                                           bp->used[t][i].addr,
                                           bp->used[t][i].size,
                                           BF_DMA_FROM_CPU);
            }
          }
          if (unmap_sts) {
            LOG_ERROR("Unable to unmap DMA buffer %p at %s:%d",
                      bp->used[t][i].addr,
                      __func__,
                      __LINE__);
          }
        }
        pipe_mgr_drv_buf_free(&bp->used[t][i]);
      }
    }
  }

  pipe_status_t sts = pipe_mgr_drv_buf_pool_clean(dev_id);
  if (PIPE_SUCCESS != sts) {
    LOG_ERROR("%s: Failed to remove buffer pool, dev %u, sts %s (%d)",
              __func__,
              dev_id,
              pipe_str_err(sts),
              sts);
  }

  PIPE_MGR_MEMSET(&pipe_mgr_drv_ctx()->ilist_dr_size[dev_id],
                  0,
                  sizeof pipe_mgr_drv_ctx()->ilist_dr_size[dev_id]);
  PIPE_MGR_MEMSET(&pipe_mgr_drv_ctx()->ilist_pending_cnt[dev_id],
                  0,
                  sizeof pipe_mgr_drv_ctx()->ilist_pending_cnt[dev_id]);

  pipe_mgr_drv_ctx()->valid[dev_id] = false;

  LOG_TRACE("Exiting %s for dev %u", __func__, dev_id);
  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_drv_buf_pool_init(bf_dev_id_t dev,
                                                bf_dma_info_t *dma_info) {
  uint32_t num_subdevices;
  if (lld_sku_get_num_subdev(dev, &num_subdevices, NULL)) {
    LOG_ERROR("%s:%d : Unknown number of subdevices", __func__, __LINE__);
    return PIPE_UNEXPECTED;
  }

  bf_dma_buf_info_t *bi;
  pipe_mgr_drv_buf_pool_t *bp = &pipe_mgr_drv_ctx()->gBufPool[dev][0];

  bi = &dma_info->dma_buff_info[BF_DMA_PIPE_INSTRUCTION_LIST];
  bp->pool[PIPE_MGR_DRV_BUF_IL] = bi->dma_buf_pool_handle;
  bp->buf_sz[PIPE_MGR_DRV_BUF_IL] = bi->dma_buf_size;
  bp->buf_cnt[PIPE_MGR_DRV_BUF_IL] = bi->dma_buf_cnt;
  bp->used[PIPE_MGR_DRV_BUF_IL] =
      PIPE_MGR_CALLOC(bi->dma_buf_cnt, sizeof(pipe_mgr_drv_buf_t));
  if (!bp->used[PIPE_MGR_DRV_BUF_IL]) goto cleanup;

  bi = &dma_info->dma_buff_info[BF_DMA_PIPE_BLOCK_READ];
  bp->pool[PIPE_MGR_DRV_BUF_BRD] = bi->dma_buf_pool_handle;
  bp->buf_sz[PIPE_MGR_DRV_BUF_BRD] = bi->dma_buf_size;
  bp->buf_cnt[PIPE_MGR_DRV_BUF_BRD] = bi->dma_buf_cnt;
  bp->used[PIPE_MGR_DRV_BUF_BRD] =
      PIPE_MGR_CALLOC(bi->dma_buf_cnt, sizeof(pipe_mgr_drv_buf_t));
  if (!bp->used[PIPE_MGR_DRV_BUF_BRD]) goto cleanup;

  bi = &dma_info->dma_buff_info[BF_DMA_PIPE_BLOCK_WRITE];
  bp->pool[PIPE_MGR_DRV_BUF_BWR] = bi->dma_buf_pool_handle;
  bp->buf_sz[PIPE_MGR_DRV_BUF_BWR] = bi->dma_buf_size;
  bp->buf_cnt[PIPE_MGR_DRV_BUF_BWR] = bi->dma_buf_cnt;
  bp->used[PIPE_MGR_DRV_BUF_BWR] =
      PIPE_MGR_CALLOC(bi->dma_buf_cnt, sizeof(pipe_mgr_drv_buf_t));
  if (!bp->used[PIPE_MGR_DRV_BUF_BWR]) goto cleanup;

  /* Idle, stats and learn must be handled per subdevice */
  for (uint32_t subdev = 0; subdev < num_subdevices; subdev++) {
    bp = &pipe_mgr_drv_ctx()->gBufPool[dev][subdev];
    bi = &dma_info[subdev].dma_buff_info[BF_DMA_PIPE_LEARN_NOTIFY];
    bp->pool[PIPE_MGR_DRV_BUF_LRN] = bi->dma_buf_pool_handle;
    bp->buf_sz[PIPE_MGR_DRV_BUF_LRN] = bi->dma_buf_size;
    bp->buf_cnt[PIPE_MGR_DRV_BUF_LRN] = bi->dma_buf_cnt;
    bp->used[PIPE_MGR_DRV_BUF_LRN] =
        PIPE_MGR_CALLOC(bi->dma_buf_cnt, sizeof(pipe_mgr_drv_buf_t));
    if (!bp->used[PIPE_MGR_DRV_BUF_LRN]) goto cleanup;

    bi = &dma_info[subdev].dma_buff_info[BF_DMA_PIPE_STAT_NOTIFY];
    bp->pool[PIPE_MGR_DRV_BUF_LRT] = bi->dma_buf_pool_handle;
    bp->buf_sz[PIPE_MGR_DRV_BUF_LRT] = bi->dma_buf_size;
    bp->buf_cnt[PIPE_MGR_DRV_BUF_LRT] = bi->dma_buf_cnt;
    bp->used[PIPE_MGR_DRV_BUF_LRT] =
        PIPE_MGR_CALLOC(bi->dma_buf_cnt, sizeof(pipe_mgr_drv_buf_t));
    if (!bp->used[PIPE_MGR_DRV_BUF_LRT]) goto cleanup;

    bi = &dma_info[subdev].dma_buff_info[BF_DMA_PIPE_IDLE_STATE_NOTIFY];
    bp->pool[PIPE_MGR_DRV_BUF_IDL] = bi->dma_buf_pool_handle;
    bp->buf_sz[PIPE_MGR_DRV_BUF_IDL] = bi->dma_buf_size;
    bp->buf_cnt[PIPE_MGR_DRV_BUF_IDL] = bi->dma_buf_cnt;
    bp->used[PIPE_MGR_DRV_BUF_IDL] =
        PIPE_MGR_CALLOC(bi->dma_buf_cnt, sizeof(pipe_mgr_drv_buf_t));
    if (!bp->used[PIPE_MGR_DRV_BUF_IDL]) goto cleanup;
  }

  return PIPE_SUCCESS;

cleanup:
  for (uint32_t subdev = 0; subdev < num_subdevices; subdev++) {
    bp = &pipe_mgr_drv_ctx()->gBufPool[dev][subdev];
    if (bp->used[PIPE_MGR_DRV_BUF_IL])
      PIPE_MGR_FREE(bp->used[PIPE_MGR_DRV_BUF_IL]);
    if (bp->used[PIPE_MGR_DRV_BUF_LRN])
      PIPE_MGR_FREE(bp->used[PIPE_MGR_DRV_BUF_LRN]);
    if (bp->used[PIPE_MGR_DRV_BUF_LRT])
      PIPE_MGR_FREE(bp->used[PIPE_MGR_DRV_BUF_LRT]);
    if (bp->used[PIPE_MGR_DRV_BUF_IDL])
      PIPE_MGR_FREE(bp->used[PIPE_MGR_DRV_BUF_IDL]);
    if (bp->used[PIPE_MGR_DRV_BUF_BRD])
      PIPE_MGR_FREE(bp->used[PIPE_MGR_DRV_BUF_BRD]);
    if (bp->used[PIPE_MGR_DRV_BUF_BWR])
      PIPE_MGR_FREE(bp->used[PIPE_MGR_DRV_BUF_BWR]);
    PIPE_MGR_MEMSET(bp, 0, sizeof(pipe_mgr_drv_buf_pool_t));
  }
  return PIPE_NO_SYS_RESOURCES;
}

static pipe_status_t pipe_mgr_drv_buf_pool_clean(bf_dev_id_t dev) {
  pipe_mgr_drv_buf_pool_t *bp = &pipe_mgr_drv_ctx()->gBufPool[dev][0];
  PIPE_MGR_FREE(bp->used[PIPE_MGR_DRV_BUF_IL]);
  PIPE_MGR_FREE(bp->used[PIPE_MGR_DRV_BUF_BRD]);
  PIPE_MGR_FREE(bp->used[PIPE_MGR_DRV_BUF_BWR]);
  uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(dev);
  for (uint32_t subdev = 0; subdev < num_subdevices; subdev++) {
    bp = &pipe_mgr_drv_ctx()->gBufPool[dev][subdev];
    PIPE_MGR_FREE(bp->used[PIPE_MGR_DRV_BUF_LRN]);
    PIPE_MGR_FREE(bp->used[PIPE_MGR_DRV_BUF_LRT]);
    PIPE_MGR_FREE(bp->used[PIPE_MGR_DRV_BUF_IDL]);
    PIPE_MGR_MEMSET(bp, 0, sizeof(pipe_mgr_drv_buf_pool_t));
  }
  return PIPE_SUCCESS;
}

pipe_mgr_drv_buf_t *pipe_mgr_drv_buf_alloc(uint8_t sid,
                                           uint8_t dev_id,
                                           uint32_t size,
                                           enum pipe_mgr_drv_buf_type type,
                                           bool waitOk) {
  return pipe_mgr_drv_buf_alloc_subdev(sid, dev_id, 0, size, type, waitOk);
}

pipe_mgr_drv_buf_t *pipe_mgr_drv_buf_alloc_subdev(
    uint8_t sid,
    uint8_t dev_id,
    bf_subdev_id_t subdev_id,
    uint32_t size,
    enum pipe_mgr_drv_buf_type type,
    bool waitOk) {
  if (!pipe_mgr_valid_deviceId(dev_id, __func__, __LINE__)) {
    return NULL;
  }
  if (sid >= PIPE_MGR_MAX_SESSIONS) {
    LOG_ERROR("Attempt to allocate with invalid sid %u from %s:%d",
              sid,
              __func__,
              __LINE__);
    return NULL;
  }
  if (type >= PIPE_MGR_DRV_BUF_CNT) {
    LOG_ERROR("Attempt to allocate with invalid type %u from %s:%d",
              type,
              __func__,
              __LINE__);
    return NULL;
  }
  pipe_mgr_drv_buf_pool_t *bp =
      &pipe_mgr_drv_ctx()->gBufPool[dev_id][subdev_id];
  if (size > bp->buf_sz[type]) {
    LOG_ERROR("Requested buffer size (0x%x) too large, max 0x%x, from %s:%d",
              size,
              bp->buf_sz[type],
              __func__,
              __LINE__);
    return NULL;
  }

  bool first = true;
  pipe_mgr_drv_buf_t *b = NULL;
  do {
    void *vaddr = NULL;
    bf_phys_addr_t paddr = 0;
    int x = bf_sys_dma_alloc(bp->pool[type], bp->buf_sz[type], &vaddr, &paddr);
    if (!x) {
      x = bf_sys_dma_buffer_index(bp->pool[type], vaddr);
      if (x >= 0) {
        if (first)
          ++pipe_mgr_drv_ctx()->sesStates[sid].cntrs.bufAllocGlbNoWait;
        else
          ++pipe_mgr_drv_ctx()->sesStates[sid].cntrs.bufAllocGlbWait;
        b = &bp->used[type][x];
        b->pool = bp->pool[type];
        b->addr = vaddr;
        b->phys_addr = paddr;
        b->used = 0;
        b->size = bp->buf_sz[type];
        b->devId = dev_id;
        b->subdev = subdev_id;
        b->pipeMask = 0;
        b->buf_pushed = 0;
        b->msgId = 0;
        b->next = NULL;
        b->prev = NULL;
        pipe_mgr_drv_buf_t *first_buf = &bp->used[type][0];
        pipe_mgr_drv_buf_t *last_buf = &bp->used[type][bp->buf_cnt[type] - 1];
        PIPE_MGR_DBGCHK(first_buf <= b && b <= last_buf);
        return b;
      }
    } else if (first) {
      pipe_sess_hdl_t sh = sid;
      if (pipe_mgr_drv_ctx()->sesStates[sid].iListPending) {
        // We have some buffers pending but no buffers free.
        // Push the current ilist to free up buffers.
        LOG_TRACE("Session %u, no buffers on dev %u, subdev %d pushing batch",
                  sid,
                  dev_id,
                  subdev_id);
        pipe_mgr_drv_ilist_push_alloc(&sh);
      }
    }
    first = false;

    if (waitOk) {
#ifdef PIPE_MGR_INLINE_DR_SERVICE
      pipe_mgr_drv_service_drs(dev_id);
#endif
    }
  } while (waitOk);

  LOG_ERROR("Session %u out of buffers for dev %d, type %d, from %s:%d",
            sid,
            dev_id,
            type,
            __func__,
            __LINE__);
  return NULL;
}

void pipe_mgr_drv_buf_free(pipe_mgr_drv_buf_t *buf) {
  if (buf == NULL) {
    PIPE_MGR_DBGCHK(0);
    return;
  }

  if (!pipe_mgr_valid_deviceId(buf->devId, __func__, __LINE__)) {
    PIPE_MGR_DBGCHK(0);
    return;
  }

  pipe_mgr_drv_buf_pool_t *bp =
      &pipe_mgr_drv_ctx()->gBufPool[buf->devId][buf->subdev];
  /* Find the buffer in the buffer pool. */
  enum pipe_mgr_drv_buf_type type;
  int idx = -1;
  for (type = PIPE_MGR_DRV_BUF_FIRST; type < PIPE_MGR_DRV_BUF_CNT; ++type) {
    if (!bp->buf_cnt[type]) continue;
    pipe_mgr_drv_buf_t *first = &bp->used[type][0];
    pipe_mgr_drv_buf_t *last = &bp->used[type][bp->buf_cnt[type] - 1];
    if (first <= buf && buf <= last) {
      idx = buf - first;
      break;
    }
  }
  if (-1 == idx) {
    PIPE_MGR_DBGCHK(-1 != idx);
    return;
  }

  void *vaddr = buf->addr;
  buf->addr = NULL;
  buf->phys_addr = 0;
  buf->used = 0;
  buf->pipeMask = 0;
  buf->buf_pushed = 0;
  buf->devId = ~0;
  buf->msgId = ~UINT64_C(0);
  buf->next = buf->prev = NULL;

  bf_sys_dma_free(bp->pool[type], vaddr);
  return;
}

/******************************************************************************
 *                                                                            *
 * Implementations of the Pipeline Manager's Driver Interface APIs functions. *
 *                                                                            *
 *****************************************************************************/
int pipe_mgr_drv_subdev_buf_size(bf_dev_id_t dev,
                                 bf_subdev_id_t subdev,
                                 enum pipe_mgr_drv_buf_type type) {
  if (!pipe_mgr_valid_deviceId(dev, __func__, __LINE__)) {
    PIPE_MGR_DBGCHK(0);
    return 0;
  }
  if (type >= PIPE_MGR_DRV_BUF_CNT) {
    PIPE_MGR_DBGCHK(0);
    return 0;
  }
  return pipe_mgr_drv_ctx()->gBufPool[dev][subdev].buf_sz[type];
}

int pipe_mgr_drv_buf_size(bf_dev_id_t dev, enum pipe_mgr_drv_buf_type type) {
  return pipe_mgr_drv_subdev_buf_size(dev, 0, type);
}

int pipe_mgr_drv_subdev_buf_count(bf_dev_id_t dev,
                                  bf_subdev_id_t subdev,
                                  enum pipe_mgr_drv_buf_type type) {
  if (!pipe_mgr_valid_deviceId(dev, __func__, __LINE__)) {
    PIPE_MGR_DBGCHK(0);
    return 0;
  }
  if (type >= PIPE_MGR_DRV_BUF_CNT) {
    PIPE_MGR_DBGCHK(0);
    return 0;
  }
  return pipe_mgr_drv_ctx()->gBufPool[dev][subdev].buf_cnt[type];
}

bf_sys_dma_pool_handle_t pipe_mgr_drv_subdev_dma_pool_handle(
    bf_dev_id_t dev, bf_subdev_id_t subdev, enum pipe_mgr_drv_buf_type type) {
  if (!pipe_mgr_valid_deviceId(dev, __func__, __LINE__)) {
    PIPE_MGR_DBGCHK(0);
    return NULL;
  }
  if (type >= PIPE_MGR_DRV_BUF_CNT) {
    PIPE_MGR_DBGCHK(0);
    return NULL;
  }
  return pipe_mgr_drv_ctx()->gBufPool[dev][subdev].pool[type];
}

/*
 * Blocking Read Register API.
 */
pipe_status_t pipe_mgr_drv_subdev_reg_rd(pipe_sess_hdl_t *sess,
                                         bf_dev_id_t dev_id,
                                         bf_subdev_id_t subdev,
                                         uint32_t addr,
                                         uint32_t *val) {
  if (NULL == sess || NULL == val) {
    LOG_ERROR("Invalid args (session %p dev %u addr 0x%x val %p) at %s:%d",
              (void *)sess,
              dev_id,
              addr,
              (void *)val,
              __func__,
              __LINE__);
    return PIPE_INVALID_ARG;
  }
  ++pipe_mgr_drv_ctx()->sesStates[*sess].cntrs.rdReg;

  int ret = lld_subdev_read_register(dev_id, subdev, addr, val);
  if (ret) {
    *val = 0;
    return PIPE_COMM_FAIL;
  }

  return PIPE_SUCCESS;
}

/*
 * Blocking Read Register API.
 */
pipe_status_t pipe_mgr_drv_reg_rd(pipe_sess_hdl_t *sess,
                                  uint8_t dev_id,
                                  uint32_t addr,
                                  uint32_t *val) {
  /* Read from sub device 0 by default */
  bf_subdev_id_t subdev = 0;
  return pipe_mgr_drv_subdev_reg_rd(sess, dev_id, subdev, addr, val);
}

int pipe_mgr_drv_blk_rd_max_sz(uint8_t dev_id) {
  return pipe_mgr_drv_buf_size(dev_id, PIPE_MGR_DRV_BUF_IL);
}
/*
 * Asynchronous Block Read API.
 */
pipe_status_t pipe_mgr_drv_blk_rd(pipe_sess_hdl_t *sess,
                                  uint8_t dev_id,
                                  uint8_t memoryWidth,
                                  uint32_t entryCount,
                                  int addr_step,
                                  uint64_t addr,
                                  pipe_mgr_drv_rd_blk_cb cb_func,
                                  void *usrData) {
  pipe_mgr_drv_rd_blk_op_t *rdBlk = NULL;
  /* Validate input. */
  if (!pipe_mgr_valid_deviceId(dev_id, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }
  if (4 != memoryWidth && 8 != memoryWidth && 16 != memoryWidth) {
    LOG_ERROR(
        "Invalid entry size (%u) at %s:%d", memoryWidth, __func__, __LINE__);
    return PIPE_INVALID_ARG;
  }
  if (!entryCount) return PIPE_INVALID_ARG;
  uint32_t buf_sz = pipe_mgr_drv_buf_size(dev_id, PIPE_MGR_DRV_BUF_IL);
  if (!buf_sz) return PIPE_UNEXPECTED;
  uint32_t entries_per_buf = buf_sz / memoryWidth;
  uint32_t num_bufs = (entryCount + entries_per_buf - 1) / entries_per_buf;

  /* Look up the session state. */
  pipe_mgr_drv_ses_state_t *st =
      pipe_mgr_drv_get_ses_state(sess, __func__, __LINE__);
  if (NULL == st) {
    return PIPE_INVALID_ARG;
  }

  /* Allocate space to store state about this read block operation. */
  rdBlk = PIPE_MGR_MALLOC(sizeof(pipe_mgr_drv_rd_blk_op_t));
  if (!rdBlk) {
    LOG_ERROR("Cannot allocate memory for state at %s:%d", __func__, __LINE__);
    return PIPE_NO_SYS_RESOURCES;
  }
  rdBlk->cb_func = cb_func;
  rdBlk->usrData = usrData;
  rdBlk->num_sub_ops = num_bufs;

  /* Allocate space to store state for each DMA transaction in this read block
   * operation. */
  rdBlk->sub_ops = PIPE_MGR_CALLOC(rdBlk->num_sub_ops, sizeof *rdBlk->sub_ops);
  if (!rdBlk->sub_ops) {
    PIPE_MGR_FREE(rdBlk);
    LOG_ERROR("Cannot allocate memory for state at %s:%d", __func__, __LINE__);
    return PIPE_NO_SYS_RESOURCES;
  }

  /* Allocate buffers for the actual read operations. */
  for (int i = 0; i < (int)num_bufs; ++i) {
    /* Decide how much data to read in this operation and what the offset/
     * address is. */
    int num_reads = entryCount > entries_per_buf ? entries_per_buf : entryCount;
    /* Get a DMA buffer to hold the read response. */
    pipe_mgr_drv_buf_t *b = pipe_mgr_drv_buf_alloc(
        st->sid, dev_id, memoryWidth * num_reads, PIPE_MGR_DRV_BUF_BRD, true);
    if (NULL == b) {
      for (int j = 0; j < i; ++j) {
        pipe_mgr_drv_buf_free(rdBlk->sub_ops[j].buf);
      }
      PIPE_MGR_FREE(rdBlk->sub_ops);
      PIPE_MGR_FREE(rdBlk);
      return PIPE_TRY_AGAIN;
    }
    b->used = memoryWidth * num_reads;

    rdBlk->sub_ops[i].buf = b;
    rdBlk->sub_ops[i].msgId = b->msgId =
        pipe_mgr_drv_next_msgId(st, dev_id, rd_blk_msgid_type);
    rdBlk->sub_ops[i].offset = i * entries_per_buf;
    rdBlk->sub_ops[i].entryCount = num_reads;
  }

  /* Add the read block operation to our list of pending read blocks. */
  PIPE_MGR_LOCK(&st->mtx_ses);
  PIPE_MGR_DLL_AP(st->rdBlk, rdBlk, next, prev);
  PIPE_MGR_UNLOCK(&st->mtx_ses);

  /* Push the buffers into the DR. */
  for (int i = 0; i < (int)num_bufs; ++i) {
    int ret = 0;
    struct pipe_mgr_drv_rd_blk_sub_op_t *op = rdBlk->sub_ops + i;
    pipe_mgr_drv_buf_t *b = op->buf;
    bf_dma_addr_t dma_addr;

    /* Map the virtual address of the buffer to the DMA address */
    if (bf_sys_dma_map(b->pool,
                       b->addr,
                       b->phys_addr,
                       b->size,
                       &dma_addr,
                       BF_DMA_TO_CPU) != 0) {
      LOG_ERROR(
          "Unable to map DMA buffer %p at %s:%d", b->addr, __func__, __LINE__);
      for (int j = i; j < rdBlk->num_sub_ops; ++j) {
        pipe_mgr_drv_buf_free(rdBlk->sub_ops[j].buf);
      }
      pipe_mgr_drv_buf_free(b);
      PIPE_MGR_LOCK(&st->mtx_ses);
      PIPE_MGR_DLL_REM(st->rdBlk, rdBlk, next, prev);
      PIPE_MGR_FREE(rdBlk->sub_ops);
      PIPE_MGR_FREE(rdBlk);
      PIPE_MGR_UNLOCK(&st->mtx_ses);
      return PIPE_COMM_FAIL;
    }
  push_again:
    PIPE_MGR_LOCK(&st->mtx_ses);
    /* Push the request to LLD. */
    ret = lld_push_rb(dev_id,
                      memoryWidth,
                      addr_step,
                      op->entryCount,
                      addr + addr_step * op->offset,
                      dma_addr,
                      b->msgId);
    if (ret == LLD_ERR_DR_FULL) {
      LOG_ERROR("Push to LLD fails read block DR full dev %d", dev_id);
      push_read_blk_drs(dev_id);
      PIPE_MGR_UNLOCK(&st->mtx_ses);
      pipe_mgr_drv_service_read_blk_drs(dev_id);
      goto push_again;
    } else if (ret) {
      /* Unmap the buffer */
      LOG_ERROR(
          "Push read block fails (%d) dev %d width %d step %d size %d src "
          "0x%" PRIx64 " dst 0x%" PRIx64 " id 0x%" PRIx64,
          ret,
          dev_id,
          memoryWidth,
          addr_step,
          rdBlk->sub_ops[i].entryCount,
          addr + addr_step * op->offset,
          dma_addr,
          op->buf->msgId);
      if (bf_sys_dma_unmap(b->pool, b->addr, b->size, BF_DMA_TO_CPU) != 0) {
        LOG_ERROR("Unable to unmap DMA buffer %p at %s:%d",
                  b->addr,
                  __func__,
                  __LINE__);
      }
      for (int j = i; j < rdBlk->num_sub_ops; ++j) {
        pipe_mgr_drv_buf_free(rdBlk->sub_ops[j].buf);
      }
      PIPE_MGR_DLL_REM(st->rdBlk, rdBlk, next, prev);
      PIPE_MGR_FREE(rdBlk->sub_ops);
      PIPE_MGR_FREE(rdBlk);
      PIPE_MGR_UNLOCK(&st->mtx_ses);
      return PIPE_COMM_FAIL;
    }

    b->buf_pushed = true;
    ++st->cntrs.rdBlkReq;
    PIPE_MGR_UNLOCK(&st->mtx_ses);
  }

  /* Start the DMA. */
  push_read_blk_drs(dev_id);

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_push_wr_blks_to_dr(pipe_sess_hdl_t *sess,
                                          bf_dev_id_t dev_id) {
  int ret = 0;
  bf_dma_addr_t dma_addr;
  pipe_mgr_drv_buf_t *buf;
  pipe_mgr_drv_wr_blk_op_t *op;
  pipe_mgr_drv_ses_state_t *st =
      pipe_mgr_drv_get_ses_state(sess, __func__, __LINE__);
  if (NULL == st) {
    return PIPE_INVALID_ARG;
  }

  uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(dev_id);

  /* Take a per-session mutex to ensure exclusive access to the list of write
   * block states stored in the session. */
  PIPE_MGR_LOCK(&st->mtx_ses);

  /* Search the list of pending operations for one matching this
   * msgId. */
  for (op = st->wrBlk; (op); op = op->next) {
    buf = op->buf;
    if (!buf) {
      continue;
    }
    if (buf->buf_pushed) {
      continue;
    }
    if (buf->devId != dev_id) {
      continue;
    }
    /* Map the virtual address of the buffer to the DMA address before pushing
       it to the DR */
    if (bf_sys_dma_map(op->buf->pool,
                       op->buf->addr,
                       op->buf->phys_addr,
                       op->buf->size,
                       &dma_addr,
                       BF_DMA_FROM_CPU) != 0) {
      LOG_ERROR("Unable to map DMA buffer %p at %s:%d",
                op->buf->addr,
                __func__,
                __LINE__);
      return PIPE_COMM_FAIL;
    }
    bf_subdev_id_t subdev_id = 0;
    for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
      int phy_pipe_mask = (buf->pipeMask >> (subdev_id * 4)) & 0xF;
      if (phy_pipe_mask == 0) continue;
      while ((ret = lld_subdev_push_wb_mcast(buf->devId,
                                             subdev_id,
                                             op->entry_sz,
                                             op->addr_step,
                                             op->data_sz,
                                             op->single_data,
                                             dma_addr,
                                             op->addr,
                                             phy_pipe_mask,
                                             op->msgId)) == LLD_ERR_DR_FULL) {
        PIPE_MGR_UNLOCK(&st->mtx_ses);
        push_write_blk_drs(dev_id);
#ifdef PIPE_MGR_INLINE_DR_SERVICE
        pipe_mgr_drv_service_write_blk_drs(dev_id, true, true);
#endif
        PIPE_MGR_LOCK(&st->mtx_ses);
      }
      if (ret) {
        break;
      }
    }

    if (ret) {
      /* Unmap the buffer */
      if (bf_sys_dma_unmap(
              op->buf->pool, op->buf->addr, op->buf->size, BF_DMA_FROM_CPU) !=
          0) {
        LOG_ERROR("Unable to unmap DMA buffer %p at %s:%d",
                  op->buf->addr,
                  __func__,
                  __LINE__);
      }
      LOG_ERROR(
          "%s Push write block fails (%d) dev %d width %d step %d size %d "
          "single %d src 0x%" PRIx64 " dst 0x%" PRIx64 " vec %x id 0x%" PRIx64,
          __func__,
          ret,
          buf->devId,
          op->entry_sz,
          op->addr_step,
          op->data_sz,
          op->single_data,
          dma_addr,
          op->addr,
          buf->pipeMask,
          op->msgId);
      PIPE_MGR_UNLOCK(&st->mtx_ses);
      return PIPE_COMM_FAIL;
    }
    op->buf->buf_pushed = true;
  }
  PIPE_MGR_UNLOCK(&st->mtx_ses);

  ret = push_write_blk_drs(dev_id);
  if (PIPE_SUCCESS != ret) {
    LOG_ERROR("Failed to push wr blk drs (%d)", ret);
    PIPE_MGR_DBGCHK(PIPE_SUCCESS == ret);
    return PIPE_COMM_FAIL;
  }

  return PIPE_SUCCESS;
}

static pipe_status_t blk_wr_int(pipe_sess_hdl_t *sess,
                                uint8_t entry_sz,
                                uint32_t entryCount,
                                uint8_t addr_step,
                                uint64_t addr,
                                int pipe_mask,
                                bool single_data,
                                pipe_mgr_drv_buf_t *buf) {
  int phy_pipe_mask = 0;
  bf_dev_pipe_t phy_pipe_id = 0;
  int ret = 0;
  bf_dma_addr_t dma_addr;

  /* Convert the logical pipe mask to physical pipe mask */
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(buf->devId);
  if (!dev_info) return PIPE_INVALID_ARG;
  unsigned int i;
  for (i = 0; i < dev_info->num_active_pipes; ++i) {
    if (pipe_mask & (1 << i)) {
      ret = pipe_mgr_map_pipe_id_log_to_phy(dev_info, i, &phy_pipe_id);
      if (PIPE_SUCCESS != ret) {
        LOG_ERROR(
            "%s: Could not map pipe %d on dev %d", __func__, i, buf->devId);
        PIPE_MGR_DBGCHK(ret == PIPE_SUCCESS);
        return ret;
      }
      phy_pipe_mask |= (1 << phy_pipe_id);
    }
  }
  buf->pipeMask = phy_pipe_mask;

  buf->used = !single_data ? entry_sz * entryCount : entry_sz;

  pipe_mgr_drv_ses_state_t *st =
      pipe_mgr_drv_get_ses_state(sess, __func__, __LINE__);
  if (NULL == st) {
    return PIPE_INVALID_ARG;
  }

  /* Allocate a new Write Block State and store it so that the correct buffer
   * can be freed in the completion callback. */
  pipe_mgr_drv_wr_blk_op_t *wrBlk;
  wrBlk = PIPE_MGR_MALLOC(sizeof(pipe_mgr_drv_wr_blk_op_t));
  if (!wrBlk) {
    LOG_ERROR("Cannot allocate memory for state at %s:%d", __func__, __LINE__);
    return PIPE_NO_SYS_RESOURCES;
  }
  buf->msgId = pipe_mgr_drv_next_msgId(st, buf->devId, wr_blk_msgid_type);
  wrBlk->msgId = buf->msgId;
  wrBlk->buf = buf;
  wrBlk->addr = addr;
  wrBlk->entry_sz = entry_sz;
  wrBlk->data_sz = entryCount;
  wrBlk->addr_step = addr_step;
  wrBlk->single_data = single_data;

  PIPE_MGR_LOCK(&st->mtx_ses);
  if (!pipe_mgr_is_device_locked(buf->devId)) {
    /* Map the virtual address to the DMA address of the buffer before
       it is pushed in the DR */
    if (bf_sys_dma_map(buf->pool,
                       buf->addr,
                       buf->phys_addr,
                       buf->size,
                       &dma_addr,
                       BF_DMA_FROM_CPU) != 0) {
      LOG_ERROR("Unable to map DMA buffer %p at %s:%d",
                buf->addr,
                __func__,
                __LINE__);
      PIPE_MGR_UNLOCK(&st->mtx_ses);
      return PIPE_COMM_FAIL;
    }
    bf_subdev_id_t subdev_id = 0;
    uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(buf->devId);
    for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
      int s_phy_pipe_mask = (buf->pipeMask >> (subdev_id * 4)) & 0xF;
      if (s_phy_pipe_mask == 0) continue;
      while ((ret = lld_subdev_push_wb_mcast(buf->devId,
                                             subdev_id,
                                             entry_sz,
                                             addr_step,
                                             entryCount,
                                             single_data,
                                             dma_addr,
                                             addr,
                                             s_phy_pipe_mask,
                                             buf->msgId)) == LLD_ERR_DR_FULL) {
        // LOG_TRACE("Write list DR on dev %d full while pushing "
        //          "at %s:%d", buf->devId, __func__, __LINE__);
        PIPE_MGR_UNLOCK(&st->mtx_ses);
        push_write_blk_drs(buf->devId);
        pipe_mgr_drv_service_write_blk_drs(buf->devId, true, true);
        PIPE_MGR_LOCK(&st->mtx_ses);
      }
      if (ret) {
        break;
      }
    }

    if (ret) {
      /* Unmap the buffer */
      if (bf_sys_dma_unmap(buf->pool, buf->addr, buf->size, BF_DMA_FROM_CPU) !=
          0) {
        LOG_ERROR("Unable to unmap DMA buffer %p at %s:%d",
                  buf->addr,
                  __func__,
                  __LINE__);
      }
      LOG_ERROR(
          "%s Push write block fails (%d) dev %d width %d step %d size %d "
          "single %d src 0x%" PRIx64 " dst 0x%" PRIx64 " vec %x id 0x%" PRIx64,
          __func__,
          ret,
          buf->devId,
          entry_sz,
          addr_step,
          entryCount,
          single_data,
          dma_addr,
          addr,
          buf->pipeMask,
          buf->msgId);
      PIPE_MGR_UNLOCK(&st->mtx_ses);
      return PIPE_COMM_FAIL;
    }
    buf->buf_pushed = true;
  }

  PIPE_MGR_DLL_AP(st->wrBlk, wrBlk, next, prev);
  ++st->cntrs.wrBlkReq;

  if (!pipe_mgr_is_device_locked(buf->devId)) {
    push_write_blk_drs(buf->devId);
  }
  PIPE_MGR_UNLOCK(&st->mtx_ses);

  return PIPE_SUCCESS;
}
/*
 * Write the same data to multiple addresses.
 */
pipe_status_t pipe_mgr_drv_blk_wr_data(pipe_sess_hdl_t *sess,
                                       rmt_dev_info_t *dev_info,
                                       uint8_t entry_sz,
                                       uint32_t entryCount,
                                       uint8_t addr_step,
                                       uint64_t addr,
                                       int pipe_mask,
                                       uint8_t *one_entry) {
  if (entry_sz != 4 && entry_sz != 8 && entry_sz != 16) {
    LOG_ERROR("Invalid entry size %d for wrBlk", entry_sz);
    return PIPE_INVALID_ARG;
  }
  uint32_t all_log_pipes = (1 << dev_info->num_active_pipes) - 1;
  if (pipe_mask & ~all_log_pipes) {
    LOG_ERROR("Invalid logical pipe mask 0x%x for dev %d wrBlk",
              pipe_mask,
              dev_info->dev_id);
    return PIPE_INVALID_ARG;
  }

  pipe_mgr_drv_buf_t *b = NULL;
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      if (addr_step != 1 && addr_step != 4) {
        LOG_ERROR("Invalid address step %d for dev %d wrBlk",
                  addr_step,
                  dev_info->dev_id);
        return PIPE_INVALID_ARG;
      }
      b = pipe_mgr_drv_buf_alloc(*sess,
                                 dev_info->dev_id,
                                 entry_sz * entryCount,
                                 PIPE_MGR_DRV_BUF_BWR,
                                 true);
      if (!b) {
        LOG_ERROR(
            "Failed to alloate DMA buffer of size %d * %d = %d for blkWr on "
            "dev %d",
            entry_sz,
            entryCount,
            entry_sz * entryCount,
            dev_info->dev_id);
        return PIPE_INVALID_ARG; /* Bad DMA buffer size. */
      }
      for (unsigned int i = 0; i < entryCount; ++i) {
        PIPE_MGR_MEMCPY(b->addr + i * entry_sz, one_entry, entry_sz);
      }
      return blk_wr_int(
          sess, entry_sz, entryCount, addr_step, addr, pipe_mask, false, b);
      break;
    case BF_DEV_FAMILY_TOFINO2:
    case BF_DEV_FAMILY_TOFINO3:

      if (addr_step != 1 && addr_step != 4 && addr_step != 8 &&
          addr_step != 16 && addr_step != 32) {
        LOG_ERROR("Invalid address step %d for dev %d blkWr",
                  addr_step,
                  dev_info->dev_id);
        return PIPE_INVALID_ARG;
      }
      b = pipe_mgr_drv_buf_alloc(
          *sess, dev_info->dev_id, entry_sz, PIPE_MGR_DRV_BUF_BWR, true);
      if (!b) {
        LOG_ERROR(
            "Failed to alloate DMA buffer of size %d * %d = %d for blkWr on "
            "dev %d",
            entry_sz,
            entryCount,
            entry_sz * entryCount,
            dev_info->dev_id);
        return PIPE_INVALID_ARG; /* Bad DMA buffer size. */
      }
      PIPE_MGR_MEMCPY(b->addr, one_entry, entry_sz);
      return blk_wr_int(
          sess, entry_sz, entryCount, addr_step, addr, pipe_mask, true, b);
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }
}
/*
 * Asynchronous Block Write API.
 */
pipe_status_t pipe_mgr_drv_blk_wr(pipe_sess_hdl_t *sess,
                                  uint8_t entry_sz,
                                  uint32_t entryCount,
                                  uint8_t addr_step,
                                  uint64_t addr,
                                  int log_pipe_mask,
                                  pipe_mgr_drv_buf_t *buf) {
  /* Validate input. */
  if (4 != entry_sz && 8 != entry_sz && 16 != entry_sz) {
    LOG_ERROR("Invalid entry size (%u) at %s:%d", entry_sz, __func__, __LINE__);
    return PIPE_INVALID_ARG;
  }
  if (!buf) {
    LOG_ERROR("No data provided to %s", __func__);
    return PIPE_INVALID_ARG;
  }
  if (!pipe_mgr_valid_deviceId(buf->devId, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }
  if (pipe_mgr_is_device_virtual(buf->devId)) {
    return PIPE_INVALID_ARG;
  }
  if (!log_pipe_mask) {
    return PIPE_INVALID_ARG;
  }

  if (buf->size < (entry_sz * entryCount)) {
    LOG_ERROR(
        "Overfilled buffer of size 0x%x bytes! "
        "Data is %u * %u = 0x%x bytes! at %s:%d",
        buf->size,
        entryCount,
        entry_sz,
        entryCount * entry_sz,
        __func__,
        __LINE__);
    PIPE_MGR_DBGCHK(buf->size >= (entry_sz * entryCount));
    return PIPE_INVALID_ARG;
  }

  unsigned int buf_sz = pipe_mgr_drv_buf_size(buf->devId, PIPE_MGR_DRV_BUF_BWR);
  if (entryCount > (buf_sz / entry_sz)) {
    LOG_ERROR("Block size %u * %u = %u is larger than max %u at %s:%d",
              entry_sz,
              entryCount,
              entry_sz * entryCount,
              buf_sz,
              __func__,
              __LINE__);
    return PIPE_INVALID_ARG;
  }

  return blk_wr_int(
      sess, entry_sz, entryCount, addr_step, addr, log_pipe_mask, false, buf);
}

pipe_status_t pipe_mgr_write_register(bf_dev_id_t dev_id,
                                      bf_subdev_id_t subdev_id,
                                      uint32_t reg_addr,
                                      uint32_t reg_data) {
  pipe_status_t status = PIPE_SUCCESS;

  if (pipe_mgr_is_device_locked(dev_id)) {
    status = pipe_mgr_ilist_add_register_write(
        dev_id, subdev_id, reg_addr, reg_data);
  } else {
    status = lld_subdev_write_register(dev_id, subdev_id, reg_addr, reg_data);
  }

  return status;
}

/* Write register in list of pipes using ilist */
pipe_status_t pipe_mgr_sess_pipes_ilist_add_register_write(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t dev_id,
    pipe_bitmap_t *pipe_bmp,
    uint8_t stage_id,
    uint32_t reg_addr,
    uint32_t reg_data) {
  pipe_status_t status = PIPE_SUCCESS;
  pipe_instr_write_reg_t instr;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, dev_id);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  if (!((reg_addr >> 25) & 0x1)) {
    /* write to non-pipe register not supported by ilist */
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }
  construct_instr_reg_write(dev_id, &instr, reg_addr, reg_data);
  status = pipe_mgr_drv_ilist_add(&sess_hdl,
                                  dev_info,
                                  pipe_bmp,
                                  stage_id,
                                  (uint8_t *)&instr,
                                  sizeof(pipe_instr_write_reg_t));
  return status;
}

/* Write register using ilist */
pipe_status_t pipe_mgr_sess_ilist_add_register_write(pipe_sess_hdl_t sess_hdl,
                                                     bf_dev_id_t dev_id,
                                                     bf_subdev_id_t subdev_id,
                                                     uint32_t reg_addr,
                                                     uint32_t reg_data) {
  pipe_status_t status = PIPE_SUCCESS;
  uint32_t stage_id = 0;
  pipe_bitmap_t pipe_bmp;
  bf_dev_pipe_t log_pipe = 0, phy_pipe = 0;
  pipe_instr_write_reg_t instr;
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, dev_id);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  if (!dev_info->dev_cfg.is_pcie_pipe_addr(reg_addr)) {
    /* write to non-pipe register not supported by ilist */
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }
  phy_pipe = dev_info->dev_cfg.dir_addr_get_pipe_id(reg_addr);
  phy_pipe = phy_pipe + (BF_SUBDEV_PIPE_COUNT * subdev_id);
  /* Callers of reg write pass physical write, convert to logical pipe */
  pipe_mgr_map_phy_pipe_id_to_log_pipe_id(dev_id, phy_pipe, &log_pipe);
  PIPE_BITMAP_INIT(&pipe_bmp, PIPE_BMP_SIZE);
  /* pipe_mgr_drv_ilist_add expects logical pipe-id */
  PIPE_BITMAP_SET(&pipe_bmp, log_pipe);
  /* Extract stage_id */
  stage_id = dev_info->dev_cfg.pcie_pipe_addr_get_stage(reg_addr);
  construct_instr_reg_write(dev_id, &instr, reg_addr, reg_data);
  status = pipe_mgr_drv_ilist_add(&sess_hdl,
                                  dev_info,
                                  &pipe_bmp,
                                  stage_id,
                                  (uint8_t *)&instr,
                                  sizeof(pipe_instr_write_reg_t));
  return status;
}

/* Write register using ilist */
pipe_status_t pipe_mgr_ilist_add_register_write(bf_dev_id_t dev_id,
                                                bf_subdev_id_t subdev_id,
                                                uint32_t reg_addr,
                                                uint32_t reg_data) {
  return pipe_mgr_sess_ilist_add_register_write(
      pipe_mgr_ctx->int_ses_hndl, dev_id, subdev_id, reg_addr, reg_data);
}

/*
 * Blocking register write call.
 *
 */
pipe_status_t pipe_mgr_drv_reg_wr(pipe_sess_hdl_t *sess,
                                  uint8_t dev_id,
                                  bf_subdev_id_t subdev,
                                  uint32_t addr,
                                  uint32_t val) {
  /* Validate input. */
  if (!pipe_mgr_valid_deviceId(dev_id, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }

  pipe_mgr_drv_ses_state_t *st =
      pipe_mgr_drv_get_ses_state(sess, __func__, __LINE__);
  if (NULL == st) {
    return PIPE_INVALID_ARG;
  }

  lld_subdev_write_register(dev_id, subdev, addr, val);

  return PIPE_SUCCESS;
}

/*
 *
 * Instruction List APIs.
 *
 */
static pipe_mgr_drv_list_op_t *find_pending_ilist(
    pipe_mgr_drv_ses_state_t *st) {
  if (!st->iListPending) {
    /* No instruction list is currently pending, allocate a new one. */
    st->iListPending = PIPE_MGR_MALLOC(sizeof(pipe_mgr_drv_list_op_t));
    PIPE_MGR_MEMSET(st->iListPending, 0, sizeof(pipe_mgr_drv_list_op_t));
  }
  return st->iListPending;
}

static pipe_mgr_drv_list_op_t *find_pending_reconfig_ilist(
    pipe_mgr_drv_ses_state_t *st, bool allocateIfNeeded, bf_dev_id_t dev_id) {
  if (allocateIfNeeded && !st->iListReconfig[dev_id]) {
    /* No instruction list is currently pending, allocate a new one. */
    st->iListReconfig[dev_id] = PIPE_MGR_MALLOC(sizeof(pipe_mgr_drv_list_op_t));
    PIPE_MGR_MEMSET(
        st->iListReconfig[dev_id], 0, sizeof(pipe_mgr_drv_list_op_t));
  }
  return st->iListReconfig[dev_id];
}

static pipe_status_t set_il_stage(pipe_mgr_drv_ses_state_t *st,
                                  pipe_mgr_drv_list_op_t *il,
                                  rmt_dev_info_t *dev_info,
                                  bf_subdev_id_t subdev,
                                  uint8_t phy_pipe_mask,
                                  bool need_noops,
                                  int new_stage,
                                  int cur_stage) {
  pipe_status_t sts = PIPE_SUCCESS;
  if (need_noops && cur_stage < dev_info->num_active_mau &&
      !pipe_mgr_is_device_locked(dev_info->dev_id)) {
    /* Need to issue 32 NOOP instructions before the set-stage instruction.
     * SBC allows 32 outstanding instructions to an MAU station, since the work
     * at any MAU is unknown by software we cannot be sure that one MAU has
     * completed all instructions before the stage is changed and the next
     * instruction is sent to another MAU.  To ensure the instructions are
     * processed in order send 32 noops to the MAU before changing stages to
     * another MAU.  This ensures that any real instructions are processed
     * before moving on to the next MAU. */
    pipe_noop_instr_t noop;
    construct_instr_noop(dev_info->dev_id, &noop);
    int i;
    for (i = 0; i < 32; ++i) {
      sts = ilist_add(st,
                      il,
                      dev_info,
                      subdev,
                      phy_pipe_mask,
                      (uint8_t *)&noop,
                      sizeof(pipe_noop_instr_t),
                      NULL,
                      0,
                      true,
                      (32 - i) * sizeof(pipe_noop_instr_t) +
                          2 * sizeof(dest_select_stage_t));
      if (PIPE_SUCCESS != sts) {
        LOG_ERROR("%s: Failed to add instr while changing stage, sts %s",
                  __func__,
                  pipe_str_err(sts));
        return sts;
      }
    }
  }
  dest_select_stage_t setStage;
  construct_instr_dest_select_stage(dev_info->dev_id, &setStage, new_stage);
  sts = ilist_add(st,
                  il,
                  dev_info,
                  subdev,
                  phy_pipe_mask,
                  (uint8_t *)&setStage,
                  sizeof(dest_select_stage_t),
                  NULL,
                  0,
                  true,
                  2 * sizeof(dest_select_stage_t));
  if (PIPE_SUCCESS != sts) {
    LOG_ERROR("%s: Failed to add set stage instr, sts %s",
              __func__,
              pipe_str_err(sts));
  }
  return sts;
}

static pipe_status_t ilist_append_buf(pipe_mgr_drv_ses_state_t *st,
                                      rmt_dev_info_t *dev_info,
                                      bf_subdev_id_t subdev,
                                      uint8_t phy_pipe_mask,
                                      pipe_mgr_drv_list_op_t *il,
                                      uint32_t minSize,
                                      bool is_internal_add) {
  pipe_status_t ret = PIPE_SUCCESS;
  pipe_mgr_drv_buf_t *b;
  bf_dev_id_t dev_id = dev_info->dev_id;
  int buf_sz = pipe_mgr_drv_buf_size(dev_id, PIPE_MGR_DRV_BUF_IL);
  b = pipe_mgr_drv_buf_alloc(
      st->sid, dev_id, buf_sz, PIPE_MGR_DRV_BUF_IL, true);
  if (!b) {
    return PIPE_TRY_AGAIN;
  } else if (minSize > b->size) {
    LOG_ERROR("Session %u buffer size too small (%#x/%#x) %s:%d",
              st->sid,
              minSize,
              b->size,
              __func__,
              __LINE__);
    pipe_mgr_drv_buf_free(b);
    return PIPE_NO_SPACE;
  }
  /* No lock required when manipulating il->bufs here because the instruction
   * list has not been pushed yet.  This means that there is no possibility
   * of another thread taking buffers off the list because that only happens
   * in the completion callback. */
  PIPE_MGR_DLL_AP(il->bufs[dev_id][subdev], b, next, prev);
  ++il->bufCnt;
  b->pipeMask = phy_pipe_mask;
  b->buf_pushed = 0;

  /* Put a set-dest instruction at the start of EVERY buffer in the ilist,
   * this is done for a few reasons:
   * - We don't track the current destination an ilist DR is set to.
   * - In some cases (i.e. batching) a partial instruction list may be pushed
   *   to the DR meaning a list of three buffers will have the first two pushed
   *   and the last deferred.  Since other sessions are free to send their
   *   ilists after the first two buffers are pushed but before the last is
   *   pushed that buffer must reset the destination. */
  uint8_t target_stage;
  if (il->lockCnt[dev_id][subdev]) {
    target_stage = il->firstLockStage[dev_id][subdev];
  } else {
    target_stage = il->stage[dev_id][subdev];
  }
  ret = set_il_stage(st,
                     il,
                     dev_info,
                     subdev,
                     phy_pipe_mask,
                     !is_internal_add,
                     target_stage,
                     target_stage);
  return ret;
}

static void insert_set_dest_instruction(pipe_mgr_drv_buf_t *b,
                                        rmt_dev_info_t *dev_info) {
  uint32_t free_space = b->size - b->used;
  PIPE_MGR_DBGCHK(free_space >= sizeof(dest_select_stage_t));

  dest_select_stage_t *setStage = (dest_select_stage_t *)(b->addr + b->used);
  construct_instr_dest_select_stage(dev_info->dev_id, setStage, 0);
  uint32_t *w = (uint32_t *)(b->addr + b->used);
  for (unsigned int i = 0; i < sizeof(dest_select_stage_t) / 4; i++)
    *(w + i) = htole32(*(w + i));
  b->used += sizeof(dest_select_stage_t);
}

static pipe_status_t ilist_add(pipe_mgr_drv_ses_state_t *st,
                               pipe_mgr_drv_list_op_t *il,
                               rmt_dev_info_t *dev_info,
                               bf_subdev_id_t subdev,
                               uint8_t phy_pipe_mask,
                               uint8_t *instr,
                               uint8_t instr_len,
                               uint8_t *data,
                               uint8_t data_len,
                               bool is_internal_add,
                               int reserved_space) {
  bf_dev_id_t dev_id = dev_info->dev_id;
  uint32_t *buf;
  uint32_t buf_len_in_32b, i;
  if (!instr) {
    PIPE_MGR_DBGCHK(instr);
    return PIPE_INVALID_ARG;
  }
  /* Must have both or neither for data and data_len. */
  if (data || data_len) {
    PIPE_MGR_DBGCHK(data && data_len);
    if (!data || !data_len) return PIPE_INVALID_ARG;
  }
  pipe_mgr_drv_buf_t *l;
  PIPE_MGR_DBGCHK(il->bufs[dev_id][subdev]);

  /* Get the last buffer for this device in the instruction list. */
  PIPE_MGR_DLL_LAST(il->bufs[dev_id][subdev], l, next, prev);
  if (l == NULL) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }

  /* Allocate a buffer if there is not enough room in the current buffer.
   * If the current buffer is pointing to an MAU stage we can fill it
   * completely.  However, if it is not we must leave room for a set-dest
   * instruction at the end. */
  int available_space = (int)l->size - reserved_space - (int)l->used;
  int required_space = instr_len + data_len;

  PIPE_MGR_DBGCHK(l->size >= l->used);
  if (required_space > available_space) {
    if (il->stage[dev_id][subdev] >= dev_info->num_active_mau) {
      /* Current buffer points to a parde stage and does not have enough room
       * to hold the requested instruction.  Before moving onto a new buffer
       * switch the stage in this buffer to MAU stage 0.  The new buffer will
       * start with a set-dest instruction as well and reset the destination to
       * the correct parde stage. */
      PIPE_MGR_DBGCHK((l->size - l->used) >= sizeof(dest_select_stage_t));
      insert_set_dest_instruction(l, dev_info);
    }
    uint32_t s =
        il->lockCnt[dev_id][subdev]
            ? l->used - il->firstLock[dev_id][subdev] + instr_len + data_len
            : 0;
    if (PIPE_SUCCESS !=
        ilist_append_buf(
            st, dev_info, subdev, phy_pipe_mask, il, s, is_internal_add)) {
      LOG_ERROR("Failed to get free DMA buffer for dev %d from %s:%d",
                dev_id,
                __func__,
                __LINE__);
      return PIPE_TRY_AGAIN;
    }
    pipe_mgr_drv_buf_t *b;
    PIPE_MGR_DLL_LAST(il->bufs[dev_id][subdev], b, next, prev);
    if (b == NULL) {
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
    }
    if (il->lockCnt[dev_id][subdev]) {
      /* Move data from the last buffer, starting at the first lock, into
       * the new buffer.  Note that the new buffer is not empty!  It has
       * a set-destination instruction as the first entry in the buffer. */
      uint32_t x = il->firstLock[dev_id][subdev];
      PIPE_MGR_MEMCPY(b->addr + b->used,
                      l->addr + il->firstLock[dev_id][subdev],
                      l->used - x);

      /* Reset the lock tracking data to reflect the fact that the first
       * lock instruction is now at the second instruction in the new
       * buffer. */
      il->firstLock[dev_id][subdev] = b->used;

      /* Adjust the usage of the buffers as we took data from one and
       * moved it to another. */
      b->used += l->used - x;
      PIPE_MGR_DBGCHK(b->used <= b->size);
      l->used = x;
      PIPE_MGR_DBGCHK(l->used <= l->size);

      PIPE_MGR_DBGCHK(b->used <= (b->size - instr_len - data_len));
    }
    l = b;
  }

  /* If this instruction is a lock, record it so we can track the lock/unlock
   * operations per buffer.  We treat Atomic Mod CSR instructions the same as
   * lock instructions here because they are handled in the same way.  Just as
   * the lock and unlock stay in the same DMA buffer the begin and end of an
   * Atomic Mod CSR operation must be in the same buffer. */
  if (is_lock_instr(((uint32_t *)instr)[0]) ||
      is_csr_mod_begin(((uint32_t *)instr)[0])) {
    if (0 == il->lockCnt[dev_id][subdev]) {
      il->firstLock[dev_id][subdev] = l->used;
      il->firstLockStage[dev_id][subdev] = il->stage[dev_id][subdev];
    }
    ++il->lockCnt[dev_id][subdev];
  } else if (is_unlock_instr(((uint32_t *)instr)[0]) ||
             is_csr_mod_end(((uint32_t *)instr)[0])) {
    if (0 == --il->lockCnt[dev_id][subdev]) {
      il->firstLock[dev_id][subdev] = 0;
    }
  }

  if (instr_len % 4 != 0 || data_len % 4 != 0) {
    LOG_ERROR("ilist add, invalid len for instr %d or data %d for dev %d",
              instr_len,
              data_len,
              dev_id);
    PIPE_MGR_DBGCHK(0);
  }

  buf_len_in_32b = instr_len / 4;
  buf = (uint32_t *)instr;
  for (i = 0; i < buf_len_in_32b; i++) {
    *(buf + i) = htole32(*(buf + i));
  }
  /* Copy the command into the buffer. */
  PIPE_MGR_MEMCPY(l->addr + l->used, instr, instr_len);
  l->used += instr_len;
  PIPE_MGR_DBGCHK(l->used <= l->size);
  /* Optionally copy any data for the command if it was provided. */
  if (data) {
    PIPE_MGR_MEMCPY(l->addr + l->used, data, data_len);
    l->used += data_len;
    PIPE_MGR_DBGCHK(l->used <= l->size);
  }

  ++st->cntrs.iListAdd;
  ++il->instrCnt[dev_id][subdev];

  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_drv_ilist_add_2_(pipe_sess_hdl_t *sess,
                                               rmt_dev_info_t *dev_info,
                                               pipe_bitmap_t *pipe_bmp,
                                               uint8_t stage,
                                               uint8_t *instr,
                                               uint8_t instr_len,
                                               uint8_t *data,
                                               uint8_t data_len,
                                               bool is_internal_add) {
  pipe_status_t ret = PIPE_SUCCESS;
  bf_dev_pipe_t pipe_id = 0;
  bf_dev_pipe_t phy_pipe_id = 0;
  uint8_t phy_pipe_mask = 0, subdev_phy_pipe_mask = 0;
  bf_dev_id_t dev_id = dev_info->dev_id;

  if (!instr || (instr_len + data_len) < PIPE_MGR_DRV_ILIST_ENTRY_MIN_SZ ||
      (instr_len + data_len) > PIPE_MGR_DRV_ILIST_ENTRY_MAX_SZ) {
    LOG_ERROR("Invalid instruction instr/instr_len (%p/%u) at %s:%d",
              instr,
              instr_len + data_len,
              __func__,
              __LINE__);
    return PIPE_INVALID_ARG;
  }

#if defined(EMU_SKIP_BLOCKS_OPT)
  if (dev_info->dev_family == BF_DEV_FAMILY_TOFINO3) {
    pipe_instr_write_reg_t *full_instr = (pipe_instr_write_reg_t *)instr;
    if (full_instr->pipe_ring_addr_type == addr_type_register) {
      if (full_instr->reg_address >=
              offsetof(tof3_reg, pipes[0].pardereg.pgstnreg.ipbprsr4reg[0]) &&
          full_instr->reg_address <
              offsetof(tof3_reg, pipes[0].pardereg.pgstnreg.ipbprsr4reg[1])) {
        return PIPE_SUCCESS;
      }

      if (full_instr->reg_address >=
              offsetof(tof3_reg, pipes[0].pardereg.pgstnreg.epbprsr4reg[0]) &&
          full_instr->reg_address <
              offsetof(tof3_reg, pipes[0].pardereg.pgstnreg.epbprsr4reg[1])) {
        return PIPE_SUCCESS;
      }
    }
  }
#endif

  if (data) {
    switch (data_len) {
      case 4:
      case 8:
      case 16:
        break;
      default:
        LOG_ERROR("Invalid instruction data/data_len (%p/%u) at %s:%d",
                  data,
                  data_len,
                  __func__,
                  __LINE__);
        return PIPE_INVALID_ARG;
    }
  }

  if (0 == PIPE_BITMAP_COUNT(pipe_bmp)) {
    PIPE_MGR_DBGCHK(PIPE_BITMAP_COUNT(pipe_bmp) != 0);
    return PIPE_SUCCESS;
  }

  PIPE_BITMAP_ITER(pipe_bmp, pipe_id) {
    /* Get physical pipe from logical pipe */
    ret = pipe_mgr_map_pipe_id_log_to_phy(dev_info, pipe_id, &phy_pipe_id);
    if (ret != PIPE_SUCCESS) {
      LOG_ERROR(
          "%s : Error in getting physical pipe-id  "
          "for logical pipe %d, error %s",
          __func__,
          pipe_id,
          pipe_str_err(ret));
      return ret;
    }
    if (phy_pipe_id < PIPE_BMP_SIZE) phy_pipe_mask |= (1 << phy_pipe_id);
  }

  pipe_mgr_drv_ses_state_t *st;
  st = pipe_mgr_drv_get_ses_state(sess, __func__, __LINE__);
  if (NULL == st) {
    return PIPE_INVALID_ARG;
  }

  /* When device is locked (fast-recfg, cold-boot, hitless warm init), allow
   * only reg writes for MAU, except for hitless-HA case.  */
  if (pipe_mgr_is_device_locked(dev_id) &&
      !pipe_mgr_hitless_warm_init_in_progress(dev_id) &&
      (stage < dev_info->num_active_mau) && ((instr_len + data_len) >= 4) &&
      !is_internal_add) {
    uint32_t *temp = (uint32_t *)instr;
    uint8_t addr_type = 0;

    addr_type = ((*temp) >> 30) & 0x3;  // bit 30:31 is addr-type
    if (addr_type != addr_type_register) {
      return PIPE_SUCCESS;
    }
  }

  /* Find the first Instruction List state that hasn't been pushed yet (there
   * should only ever be one that isn't pushed).
   * If there isn't one, allocate it and add it to the list. */
  pipe_mgr_drv_list_op_t *il;
  /* If device is locked put on seperate ilist */
  if (pipe_mgr_is_device_locked(dev_id)) {
    il = find_pending_reconfig_ilist(st, true, dev_id);
    if (*sess != pipe_mgr_ctx->int_ses_hndl)
      /* fast-reconfig in progress, the user session cannot be destroyed */
      PIPE_MGR_SESS_SET_RECFG(pipe_mgr_ctx->pipe_mgr_sessions[*sess].flags);
  } else {
    il = find_pending_ilist(st);
  }
  if (!il) {
    return PIPE_NO_SYS_RESOURCES;
  }

  /* The first instruction in an instruction list to a device must set the
   * stage.  This is because we don't track stages between instruction lists
   * as that would be very difficult since they can be pushed or aborted at
   * any time.
   * If the instruction list's destination is changing allocate a new buffer
   * and set the stage.
   * Also, if the requested stage doesn't match the current stage then a set-
   * stage instruction must be issued. */
  uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(dev_id);
  int subdev = 0;
  for (subdev = 0; subdev < (int)num_subdevices; subdev++) {
    subdev_phy_pipe_mask = (phy_pipe_mask >> (subdev * 4)) & 0xf;
    if (!subdev_phy_pipe_mask) {
      continue;
    }

    if (!il->bufs[dev_id][subdev]) {
      /* No current buffer.  This is the first instruction to this device so
       * allocate a new buffer. */
      il->stage[dev_id][subdev] = stage;
      il->curDest[dev_id][subdev] = subdev_phy_pipe_mask;

      if (PIPE_SUCCESS != ilist_append_buf(st,
                                           dev_info,
                                           subdev,
                                           subdev_phy_pipe_mask,
                                           il,
                                           0,
                                           is_internal_add)) {
        LOG_ERROR("Failed to get free DMA buffer for dev %d from %s:%d",
                  dev_id,
                  __func__,
                  __LINE__);
        return PIPE_TRY_AGAIN;
      }
    } else if (subdev_phy_pipe_mask != il->curDest[dev_id][subdev]) {
      /* The instruction list is changing destinations so close the current
       * buffer
       * and open a new one.  Before closing the current buffer make sure there
       * are no open lock instructions in it. */
      if (il->lockCnt[dev_id][subdev]) {
        LOG_ERROR(
            "Changing dest (%#x to %#x) with table lock count %d "
            "at %s:%d",
            il->curDest[dev_id][subdev],
            subdev_phy_pipe_mask,
            il->lockCnt[dev_id][subdev],
            __func__,
            __LINE__);
        return PIPE_NO_SPACE;
      }

      /* If the current buffer is not pointing to an MAU stage switch it to one.
       * This will ensure another buffer which may start with NOOPs will send
       * them
       * to an MAU and not a parde stage. */
      if (il->stage[dev_id][subdev] >= dev_info->num_active_mau) {
        pipe_mgr_drv_buf_t *b;
        PIPE_MGR_DLL_LAST(il->bufs[dev_id][subdev], b, next, prev)
        if (b) {
          insert_set_dest_instruction(b, dev_info);
        }
      }

      il->stage[dev_id][subdev] = stage;
      il->curDest[dev_id][subdev] = subdev_phy_pipe_mask;
      if (PIPE_SUCCESS != ilist_append_buf(st,
                                           dev_info,
                                           subdev,
                                           subdev_phy_pipe_mask,
                                           il,
                                           0,
                                           is_internal_add)) {
        LOG_ERROR("Failed to get free DMA buffer for dev %d from %s:%d",
                  dev_id,
                  __func__,
                  __LINE__);
        return PIPE_TRY_AGAIN;
      }
    } else if (stage != il->stage[dev_id][subdev]) {
      ret = set_il_stage(st,
                         il,
                         dev_info,
                         subdev,
                         subdev_phy_pipe_mask,
                         !is_internal_add,
                         stage,
                         il->stage[dev_id][subdev]);
      il->stage[dev_id][subdev] = stage;
    }
    if (PIPE_SUCCESS != ret) {
      return ret;
    }

    /* Add the actual instruction to the list. */
    ret = ilist_add(st,
                    il,
                    dev_info,
                    subdev,
                    subdev_phy_pipe_mask,
                    instr,
                    instr_len,
                    data,
                    data_len,
                    is_internal_add,
                    il->stage[dev_id][subdev] >= dev_info->num_active_mau
                        ? sizeof(dest_select_stage_t)
                        : 0);
    if (PIPE_SUCCESS != ret) {
      return ret;
    }
  }
  return ret;
}

uint32_t pipe_mgr_drv_ilist_locked_size_remaining(pipe_sess_hdl_t sess,
                                                  bf_dev_id_t dev_id) {
  pipe_mgr_drv_ses_state_t *st;
  st = pipe_mgr_drv_get_ses_state(&sess, __func__, __LINE__);
  if (NULL == st || NULL == st->iListPending) return 0;
  pipe_mgr_drv_list_op_t *il = st->iListPending;
  uint32_t num_subdevices = 0;
  int subdev_id = 0;

  num_subdevices = pipe_mgr_get_num_active_subdevices(dev_id);
  /* No locks so return zero. */
  bool zero = true;
  for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
    if (0 != il->lockCnt[dev_id][subdev_id]) zero = false;
  }

  if (zero) {
    return 0;
  }

  pipe_mgr_drv_buf_t *b;
  PIPE_MGR_DLL_LAST(il->bufs[dev_id][0], b, next, prev);
  if (!b) {
    /* Since there is a lock there must be a buffer... */
    PIPE_MGR_DBGCHK(b);
    return 0;
  }

  if (b->used < il->firstLock[dev_id][0]) {
    /* Something is wrong, the first lock in the buffer is beyond the number of
     * bytes used in the buffer! */
    PIPE_MGR_DBGCHK(b->used >= il->firstLock[dev_id][0]);
    return 0;
  }

  /* A few fixed instructions are added to every buffer which reduces the amount
   * of space available for instructions between a lock and unlock. */
  uint32_t overhead = 0;
  overhead += 32 * 4; /* Account for 32 nops at the start of a buffer. */
  overhead += 8;      /* Account for a set-dest at the start of a buffer. */

  /* The amount of data in the buffer starting at the lock. */
  uint32_t sz_under_lock = b->used - il->firstLock[dev_id][0];

  /* The amount of additional space allowed to be under the lock. */
  uint32_t extra = b->size - overhead - sz_under_lock;
  return extra;
}

pipe_status_t pipe_mgr_drv_ilist_add_2(pipe_sess_hdl_t *sess,
                                       rmt_dev_info_t *dev_info,
                                       pipe_bitmap_t *pipe_bmp,
                                       uint8_t stage,
                                       uint8_t *instr,
                                       uint8_t instr_len,
                                       uint8_t *data,
                                       uint8_t data_len) {
  return pipe_mgr_drv_ilist_add_2_(
      sess, dev_info, pipe_bmp, stage, instr, instr_len, data, data_len, false);
}
pipe_status_t pipe_mgr_drv_ilist_add(pipe_sess_hdl_t *sess,
                                     rmt_dev_info_t *dev_info,
                                     pipe_bitmap_t *pipe_bmp,
                                     uint8_t stage,
                                     uint8_t *data,
                                     uint8_t len) {
  return pipe_mgr_drv_ilist_add_2_(
      sess, dev_info, pipe_bmp, stage, data, len, NULL, 0, false);
}

static pipe_mgr_drv_rd_list_op_t *find_pending_rd_ilist(
    pipe_mgr_drv_ses_state_t *st, bf_dev_id_t dev_id) {
  pipe_mgr_drv_rd_list_op_t *rd_op = st->iListRdPending;
  for (; rd_op; rd_op = rd_op->next) {
    if (rd_op->dev_id == dev_id) return rd_op;
  }
  rd_op = alloc_pipe_mgr_drv_rd_list_op_t(dev_id);
  if (!rd_op) return NULL;
  PIPE_MGR_DLL_AP(st->iListRdPending, rd_op, next, prev);
  return rd_op;
}
pipe_status_t pipe_mgr_drv_ilist_rd_add(pipe_sess_hdl_t *sess,
                                        rmt_dev_info_t *dev_info,
                                        bf_dev_pipe_t pipe,
                                        uint8_t stage,
                                        uint8_t *data,
                                        uint8_t len) {
  pipe_status_t ret = PIPE_SUCCESS;
  bf_dev_pipe_t phy_pipe_id = 0;
  bf_dev_id_t dev_id = dev_info->dev_id;
  uint8_t phy_pipe_mask = 0;

  if (!pipe_mgr_valid_deviceId(dev_id, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }
  if (!data || len != PIPE_MGR_DRV_ILIST_ENTRY_MIN_SZ) {
    LOG_ERROR("Invalid instruction data/len (%p/%u) at %s:%d",
              data,
              len,
              __func__,
              __LINE__);
    return PIPE_INVALID_ARG;
  }

  /* Get physical pipe from logical pipe */
  ret = pipe_mgr_map_pipe_id_log_to_phy(dev_info, pipe, &phy_pipe_id);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR(
        "%s : Error in getting physical pipe-id  "
        "for logical pipe %d, error %s",
        __func__,
        pipe,
        pipe_str_err(ret));
    return ret;
  }
  pipe = phy_pipe_id;
  phy_pipe_mask = (1 << pipe);

  pipe_mgr_drv_ses_state_t *st;
  st = pipe_mgr_drv_get_ses_state(sess, __func__, __LINE__);
  if (NULL == st) {
    return PIPE_INVALID_ARG;
  }

  uint32_t instr_limit = 0;
  uint32_t buf_sz = pipe_mgr_drv_buf_size(dev_id, PIPE_MGR_DRV_BUF_IL);
  ilist_rd_buf_space(buf_sz, &instr_limit, NULL);

  pipe_mgr_drv_rd_list_op_t *il = find_pending_rd_ilist(st, dev_id);
  if (!il) return PIPE_NO_SYS_RESOURCES;

  pipe_mgr_drv_buf_t *b;
  PIPE_MGR_DLL_LAST(il->bufs[pipe], b, next, prev);
  if (!b || stage != il->stage[pipe] || b->used == instr_limit) {
    b = pipe_mgr_drv_buf_alloc(
        st->sid, dev_id, buf_sz, PIPE_MGR_DRV_BUF_IL, true);
    if (!b) return PIPE_NO_SYS_RESOURCES;
    PIPE_MGR_DLL_AP(il->bufs[pipe], b, next, prev);
    ++il->bufCnt;
    b->pipeMask = phy_pipe_mask;
    b->buf_pushed = 0;
    dest_select_t *set_dest = (void *)b->addr;
    construct_instr_dest_select(dev_id, set_dest, phy_pipe_id, stage);
    b->used += sizeof(dest_select_t);
    il->stage[pipe] = stage;
  }

  /* Add the actual instruction to the list. */
  PIPE_MGR_MEMCPY(b->addr + b->used, data, len);
  b->used += len;
  return PIPE_SUCCESS;
}
pipe_status_t pipe_mgr_drv_ilist_rd_push(pipe_sess_hdl_t *sess,
                                         pipe_mgr_drv_rd_ilist_cb cb_func,
                                         void *usrData) {
  // LOG_TRACE("Entering %s", __func__);
  unsigned i, j;
  pipe_mgr_drv_ses_state_t *st =
      pipe_mgr_drv_get_ses_state(sess, __func__, __LINE__);
  if (NULL == st) {
    return PIPE_INVALID_ARG;
  }

  /* Lock the session until all buffers are pushed to LLD.  This elimates the
   * possibility of completions happening for the first buffers while pushing
   * later buffers and having this thread and another thread working on the
   * instruction list at the same time. */

  PIPE_MGR_LOCK(&st->mtx_ses);
  while (st->iListRdPending) {
    pipe_mgr_drv_rd_list_op_t *il = st->iListRdPending;
    /* Store callback. */
    il->cb_func = cb_func;
    il->usrData = usrData;
    /* Move this instruction list onto the list of pushed instruction lists. */
    PIPE_MGR_DLL_REM(st->iListRdPending, il, next, prev);
    PIPE_MGR_DLL_AP(st->iListRd, il, next, prev);
    /* For each buffer in the list, assign it a message id and push it onto the
     * LLD fifo. */
    bf_dma_addr_t dma_addr_1, dma_addr_2;
    uint32_t rsp_offset = 0;
    uint32_t buf_sz = pipe_mgr_drv_buf_size(il->dev_id, PIPE_MGR_DRV_BUF_IL);
    ilist_rd_buf_space(buf_sz, NULL, &rsp_offset);
    rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(il->dev_id);
    if (!dev_info) {
      LOG_ERROR("%s: Unable to use dev id %d", __func__, il->dev_id);
      PIPE_MGR_DBGCHK(0);
      PIPE_MGR_UNLOCK(&st->mtx_ses);
      return (PIPE_INVALID_ARG);
    }

    for (j = 0; j < dev_info->dev_cfg.num_pipelines; ++j) {
      pipe_mgr_drv_buf_t *b;
      for (b = il->bufs[j]; b; b = b->next) {
        if (!b->buf_pushed) {
          b->msgId =
              pipe_mgr_drv_next_msgId(st, il->dev_id, i_list_rd_msgid_type);

          int ret;
          /* Map the virtual address of the buffer to the DMA address */
          if (bf_sys_dma_map(b->pool,
                             b->addr,
                             b->phys_addr,
                             rsp_offset,
                             &dma_addr_1,
                             BF_DMA_FROM_CPU) != 0) {
            LOG_ERROR("Unable to map DMA buffer %p at %s:%d",
                      b->addr,
                      __func__,
                      __LINE__);
            PIPE_MGR_UNLOCK(&st->mtx_ses);
            return PIPE_COMM_FAIL;
          }
          if (bf_sys_dma_map(b->pool,
                             b->addr + rsp_offset,
                             b->phys_addr + rsp_offset,
                             b->size - rsp_offset,
                             &dma_addr_2,
                             BF_DMA_TO_CPU) != 0) {
            LOG_ERROR("Unable to unmap DMA buffer %p at %s:%d",
                      b->addr + rsp_offset,
                      __func__,
                      __LINE__);
            PIPE_MGR_UNLOCK(&st->mtx_ses);
            return PIPE_COMM_FAIL;
          }

          /* Tofino will use a DR per physical pipe, later chips will use the
           * first DR but set the target pipe as the mcast destination. */
          bool use_mcast = dev_info->dev_family != BF_DEV_FAMILY_TOFINO;
          int which_dr = 0;
          bf_subdev_id_t subdev_id = 0;
          if (use_mcast) {
            subdev_id = j / 4;
            int phy_pipe_mask = (b->pipeMask >> (subdev_id * 4)) & 0xF;
            while ((ret = lld_subdev_push_ilist_mcast(il->dev_id,
                                                      subdev_id,
                                                      which_dr,
                                                      dma_addr_1,
                                                      b->used,
                                                      16,
                                                      false,
                                                      phy_pipe_mask,
                                                      dma_addr_2,
                                                      b->msgId)) ==
                   LLD_ERR_DR_FULL) {
              push_ilist_drs(dev_info);
              PIPE_MGR_UNLOCK(&st->mtx_ses);
              service_ilist_drs(il->dev_id);
              PIPE_MGR_LOCK(&st->mtx_ses);
            }
            if (ret) {
              break;
            } else {
              ilist_pending_inc(il->dev_id, subdev_id, which_dr);
              b->buf_pushed |= (1 << which_dr);
            }
          } else {
            which_dr = j % 4;
            subdev_id = j / 4;
            while ((ret = lld_subdev_push_ilist(il->dev_id,
                                                subdev_id,
                                                which_dr,
                                                dma_addr_1,
                                                b->used,
                                                16,
                                                false,
                                                dma_addr_2,
                                                b->msgId)) == LLD_ERR_DR_FULL) {
              push_ilist_drs(dev_info);
              PIPE_MGR_UNLOCK(&st->mtx_ses);
              service_ilist_drs(il->dev_id);
              PIPE_MGR_LOCK(&st->mtx_ses);
            }
            if (!ret) {
              ilist_pending_inc(il->dev_id, subdev_id, which_dr);
              b->buf_pushed |= (1 << which_dr);
            }
          }

          if (ret) {
            if (bf_sys_dma_unmap(
                    b->pool, b->addr, rsp_offset, BF_DMA_FROM_CPU) != 0) {
              LOG_ERROR("Unable to unmap DMA buffer %p at %s:%d",
                        b->addr,
                        __func__,
                        __LINE__);
            }
            if (bf_sys_dma_unmap(b->pool,
                                 b->addr + rsp_offset,
                                 b->size - rsp_offset,
                                 BF_DMA_TO_CPU) != 0) {
              LOG_ERROR("Unable to unmap DMA buffer %p at %s:%d",
                        b->addr + rsp_offset,
                        __func__,
                        __LINE__);
            }
            LOG_ERROR("%s Push ilist fails %d dev %d fifo %d src 0x%" PRIx64
                      " size %d rsp size %d s/f %d rsp 0x%" PRIx64
                      " id 0x%" PRIx64,
                      __func__,
                      ret,
                      il->dev_id,
                      which_dr,
                      dma_addr_1,
                      b->used,
                      16,
                      false,
                      dma_addr_2,
                      b->msgId);
            PIPE_MGR_UNLOCK(&st->mtx_ses);
            return PIPE_COMM_FAIL;
          }
        }
      }
    }
    for (j = 0; j < dev_info->dev_cfg.num_pipelines; ++j) {
      /* If any buffers were pushed to the device start the DMA. */
      if (il->bufs[j]) {
        push_ilist_drs(dev_info);
        break;
      }
    }
  }
  PIPE_MGR_UNLOCK(&st->mtx_ses);

  /* Process completions if we pushed enough buffers to fill the completion
   * DR. */
  for (i = 0; i < PIPE_MGR_NUM_DEVICES; ++i) {
    rmt_dev_info_t *d_info = pipe_mgr_get_dev_info(i);
    if (!d_info) continue;
    while (ilist_pending_full(i)) service_ilist_drs(i);
  }

  return PIPE_SUCCESS;
}

/* Given a physical pipe bit-map, return the first (lowest) logical pipe of the
 * profile targeted by the bit-map.  If the physical pipe bit-map is targeting
 * multiple profiles then the lowest logical pipe from any of those profiles may
 * be returned. */
static inline bf_dev_pipe_t phy_pipe_to_first_log_pipe(rmt_dev_info_t *dev_info,
                                                       uint8_t phy_pipe_mask) {
  bf_dev_pipe_t first_phy_pipe = 0;
  unsigned I = 8 * sizeof phy_pipe_mask;
  for (unsigned i = 0; i < dev_info->dev_cfg.num_pipelines && i < I; ++i) {
    if (phy_pipe_mask & (1u << i)) {
      first_phy_pipe = i;
      break;
    }
  }
  bf_dev_pipe_t log_pipe = 0, first_log_pipe = 0;
  pipe_status_t rc = pipe_mgr_map_phy_pipe_id_to_log_pipe_id_optimized(
      dev_info, first_phy_pipe, &log_pipe);
  if (rc == PIPE_SUCCESS) {
    profile_id_t prof_id = 0;
    rc = pipe_mgr_pipe_to_profile(
        dev_info, log_pipe, &prof_id, __func__, __LINE__);
    if (rc == PIPE_SUCCESS) {
      first_log_pipe = dev_info->profile_info[prof_id]->lowest_pipe;
    }
  }
  return first_log_pipe;
}

static inline pipe_status_t ilist_push(pipe_mgr_drv_ses_state_t *st,
                                       rmt_dev_info_t *dev_info,
                                       bf_subdev_id_t subdev_id,
                                       pipe_mgr_drv_buf_t *b) {
  bf_dev_id_t dev_id = dev_info->dev_id;

  /* Decode and log the instruction list if needed. */
  if (pipe_mgr_log_ilist(dev_id)) {
    /* Get the first logical pipe this iList is targeting, it will be used to
     * decode some operations to P4 object names. */
    bf_dev_pipe_t first_log_pipe =
        phy_pipe_to_first_log_pipe(dev_info, b->pipeMask);

    /* Take a guess at the required buffer size for the log message and keep
     * doubling it until it is large enough. */
    char *msg_log = NULL;
    int msg_log_sz = 2048;
    pipe_status_t msg_log_sts = PIPE_SUCCESS;
    do {
      msg_log = PIPE_MGR_MALLOC(msg_log_sz);
      msg_log_sts = pipe_mgr_decode_ilist(dev_info,
                                          first_log_pipe,
                                          (uint32_t *)b->addr,
                                          b->used,
                                          msg_log,
                                          msg_log_sz);
      if (PIPE_SUCCESS == msg_log_sts) {
        LOG_TRACE(
            "Dev %d subdev %d IList DMA %d bytes to phyPipeMask %x w/ MsgId "
            "0x%" PRIx64,
            dev_id,
            subdev_id,
            b->used,
            b->pipeMask,
            b->msgId);
        LOG_TRACE("\n%s", msg_log);
        LOG_TRACE("Dev %d subdev %d End IList DMA decode of MsgId 0x%" PRIx64,
                  dev_id,
                  subdev_id,
                  b->msgId);
      }
      if (msg_log) PIPE_MGR_FREE(msg_log);
      msg_log_sz *= 2;
    } while (msg_log_sts == PIPE_NO_SYS_RESOURCES);
  }

  /* Map the virtual address of the buffer to the DMA address */
  bf_dma_addr_t dma_addr;
  if (bf_sys_dma_map(b->pool,
                     b->addr,
                     b->phys_addr,
                     b->size,
                     &dma_addr,
                     BF_DMA_FROM_CPU) != 0) {
    LOG_ERROR(
        "Unable to map DMA buffer %p at %s:%d", b->addr, __func__, __LINE__);
    return PIPE_COMM_FAIL;
  }

  /* Tofino does not support multicast instruction lists. */
  bool use_mcast = dev_info->dev_family != BF_DEV_FAMILY_TOFINO;
  int which_dr = 0;
  int ret = 0;
  if (use_mcast) {
#if 0
      LOG_ERROR(
        "Pushing instruction list buffer "
        "(Session %u msgId 0x%" PRIx64 ", dev %d, subdev %d, pipemask 0x%x, pushed %d)",
        st->sid,
        b->msgId,
        dev_id,
        subdev_id, b->pipeMask, b->buf_pushed);
#endif
    int phy_pipe_mask = b->pipeMask;
    if (phy_pipe_mask != 0) {
      while ((ret = lld_subdev_push_ilist_mcast(dev_id,
                                                subdev_id,
                                                which_dr,
                                                dma_addr,
                                                b->used,
                                                0,
                                                false,
                                                phy_pipe_mask,
                                                0,
                                                b->msgId)) == LLD_ERR_DR_FULL) {
        push_ilist_drs(dev_info);
        PIPE_MGR_UNLOCK(&st->mtx_ses);
        service_ilist_drs(dev_id);
        PIPE_MGR_LOCK(&st->mtx_ses);
      }
      if (ret) {
        goto push_error;
      }
      ilist_pending_inc(dev_id, subdev_id, which_dr);
      b->buf_pushed |= (1 << which_dr);
    }
  } else {
    for (int phy_pipe = 0; phy_pipe < 4; phy_pipe++) {
      /* If the buffer does not go to this physical pipe continue. */
      if (!(b->pipeMask & (1 << phy_pipe))) continue;
      /* If the buffer is already pushed to the physical pipe continue. */
      if (b->buf_pushed & (1 << phy_pipe)) continue;
      /* Select the DR based on the physical pipe. */
      which_dr = phy_pipe;

      while ((ret = lld_subdev_push_ilist(dev_id,
                                          subdev_id,
                                          which_dr,
                                          dma_addr,
                                          b->used,
                                          0,
                                          false,
                                          0,
                                          b->msgId)) == LLD_ERR_DR_FULL) {
        push_ilist_drs(dev_info);
        PIPE_MGR_UNLOCK(&st->mtx_ses);
        service_ilist_drs(dev_id);
        PIPE_MGR_LOCK(&st->mtx_ses);
      }

      if (ret) {
        goto push_error;
      }

      ilist_pending_inc(dev_id, subdev_id, phy_pipe);

      b->buf_pushed |= (1 << phy_pipe);
    }
  }
  return PIPE_SUCCESS;

push_error:
  if (bf_sys_dma_unmap(b->pool, b->addr, b->size, BF_DMA_FROM_CPU) != 0) {
    LOG_ERROR(
        "Unable to unmap DMA buffer %p at %s:%d", b->addr, __func__, __LINE__);
  }
  LOG_ERROR(
      "%s Push ilist fails %d dev %d subdev %d, mc %d msk %x fifo %d "
      "src 0x%" PRIx64 " size %d s/f %d id 0x%" PRIx64,
      __func__,
      ret,
      dev_id,
      subdev_id,
      use_mcast,
      b->pipeMask,
      which_dr,
      dma_addr,
      b->used,
      false,
      b->msgId);
  return PIPE_COMM_FAIL;
}

/*
 * Push an Instruction List to hardware.
 *
 */
static pipe_status_t pipe_mgr_drv_ilist_push_int(pipe_mgr_drv_ses_state_t *st,
                                                 pipe_mgr_drv_list_op_t *il,
                                                 pipe_mgr_drv_ilist_cb cb_func,
                                                 void *usrData) {
  unsigned int i;

  /* Lock the session until all buffers are pushed to LLD.  This eliminates the
   * possibility of completions happening for the first buffers while pushing
   * later buffers and having this thread and another thread working on the
   * instruction list at the same time. */
  PIPE_MGR_LOCK(&st->mtx_ses);

  /* Store callback. */
  il->cb_func = cb_func;
  il->usrData = usrData;

  /* Add this instruction list onto the list of pushed instruction lists. */
  PIPE_MGR_DLL_AP(st->iList, il, next, prev);
  /* For each buffer in the list, assign it a message id and push it onto the
   * LLD fifo. */
  uint32_t num_subdevices = 0;
  int subdev_id = 0;
  for (i = 0; i < PIPE_MGR_NUM_DEVICES; ++i) {
    rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(i);
    if (!dev_info) continue;

    num_subdevices = pipe_mgr_get_num_active_subdevices(i);
    for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
      pipe_mgr_drv_buf_t *b;
      for (b = il->bufs[i][subdev_id]; b; b = b->next) {
        if (!b->buf_pushed) {
          b->msgId = pipe_mgr_drv_next_msgId(st, i, i_list_msgid_type);
        }

        pipe_status_t sts = ilist_push(st, dev_info, subdev_id, b);
        if (sts != PIPE_SUCCESS) {
          PIPE_MGR_UNLOCK(&st->mtx_ses);
          return sts;
        }

        ++st->cntrs.iListPushBuf;
      }
      /* If any buffers were pushed to the device start the DMA. */
      if (il->bufs[i][subdev_id]) {
        push_ilist_drs(dev_info);
      }
    }
  }
  PIPE_MGR_UNLOCK(&st->mtx_ses);

  /* Process completions if we pushed enough buffers to fill the completion
   * DR.  In cases where the Instruction List DRs are not periodically serviced
   * (e.g. no dedicated DMA servicing thread) and the DR size is less than the
   * total number of DMA buffers it is possible to get into a situation where
   * we have pushed more buffers than will fit in the completion DR.  Since the
   * completion DR will back pressure the Tx DR we need to empty enough buffers
   * from the completion DR to guarentee space for what we just pushed.  If we
   * don't then buffers will sit, flow controlled, in the Tx DR and will not be
   * processed and some hardware programming will not happen. */
  for (i = 0; i < PIPE_MGR_NUM_DEVICES; ++i) {
    rmt_dev_info_t *d_info = pipe_mgr_get_dev_info(i);
    if (!d_info) continue;
    while (ilist_pending_full(i)) service_ilist_drs(i);
  }

  // LOG_TRACE("Exiting %s", __func__);
  return PIPE_SUCCESS;
}

/* This deals with the case where the session tried to add an ilist instruction
 * but ran out of buffer space and had to push the existing buffers and wait
 * one of them to return to get space to store that instruction.  In this case
 * do not push the last buffer of the ilist as it may have have a lock or
 * barrier instruction posted by a table manager but not yet told to the
 * resource manager.  This can be the case when the table manager is in the
 * middle of a move operation and is issuing instructions for multiple resource
 * tables. In this case we must split the ilist into two.  The first list will
 * contain all buffers except the last and will be pushed, the second list
 * will contain the last buffer and remain as the pending list. */
static pipe_status_t pipe_mgr_drv_ilist_push_alloc(pipe_sess_hdl_t *sess) {
  unsigned i;
  pipe_mgr_drv_ses_state_t *st =
      pipe_mgr_drv_get_ses_state(sess, __func__, __LINE__);
  if (NULL == st) {
    return PIPE_INVALID_ARG;
  }

  pipe_mgr_drv_list_op_t *il = st->iListPending;
  if (!il) {
    return PIPE_SUCCESS;
  }

  /* During alloc, we must leave one buffer in the pending ilist per device.
   * Nothing to do if we do not have at least 2 in any device. */
  bool canPush = false;
  int subdev_id = 0;
  uint32_t num_subdevices = 0;
  for (i = 0; i < PIPE_MGR_NUM_DEVICES; ++i) {
    rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(i);
    if (!dev_info) continue;
    num_subdevices = pipe_mgr_get_num_active_subdevices(i);
    for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
      if (il->bufs[i][subdev_id] && il->bufs[i][subdev_id]->next) {
        canPush = true;
        break;
      }
    }
    if (canPush) {
      break;
    }
  }
  if (!canPush) {
    return PIPE_SUCCESS;
  }

  /* If the checkpoint is valid only buffers before the checkpoint can be
   * pushed. */
  struct pipe_mgr_drv_list_chkpt *cp = &st->iListPendingChkpt;
  if (cp->is_valid) {
    canPush = false;
    for (i = 0; i < PIPE_MGR_NUM_DEVICES; ++i) {
      rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(i);
      if (!dev_info) continue;
      num_subdevices = pipe_mgr_get_num_active_subdevices(i);
      for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
        if (il->bufs[i][subdev_id] != cp->buf[i][subdev_id]) {
          canPush = true;
          break;
        }
      }
      if (canPush) {
        break;
      }
    }
  }
  if (!canPush) {
    return PIPE_SUCCESS;
  }

  ++st->cntrs.iListPush;

  PIPE_MGR_LOCK(&st->mtx_ses);

  st->iListPending = NULL;
  pipe_mgr_drv_list_op_t *temp = NULL;
  find_pending_ilist(st);
  *st->iListPending = *il;
  temp = il;
  il = st->iListPending;
  st->iListPending = temp;
  st->iListPending->bufCnt = 0;
  cp->bufCnt = 0;
  for (i = 0; i < PIPE_MGR_NUM_DEVICES; ++i) {
    rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(i);
    if (!dev_info) continue;
    num_subdevices = pipe_mgr_get_num_active_subdevices(i);
    for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
      if (!il->bufs[i][subdev_id]) continue;
      /* If there is no checkpoint move only the last buffer in the IL onto the
       * new pending list.  If there is a checkpoint move all buffers starting
       * at
       * the checkpointed buffer onto the new pending list. */
      st->iListPending->bufs[i][subdev_id] = NULL;
      pipe_mgr_drv_buf_t *b =
          cp->is_valid ? cp->buf[i][subdev_id] : il->bufs[i][subdev_id]->prev;
      pipe_mgr_drv_buf_t *b_next = NULL;
      do {
        b_next = b->next;
        /* Remove it from the list. */
        PIPE_MGR_DLL_REM(il->bufs[i][subdev_id], b, next, prev);
        --il->bufCnt;
        /* Add it to the new IL. */
        PIPE_MGR_DLL_AP(st->iListPending->bufs[i][subdev_id], b, next, prev);
        ++st->iListPending->bufCnt;
        b = b_next;
      } while (b);
      /* Rebuild the bufCnt to restore. */
      for (b = st->iListPending->bufs[i][subdev_id]; b; b = b->next) {
        if (!cp->buf[i][subdev_id]) break;
        ++cp->bufCnt;
        if (b == cp->buf[i][subdev_id]) break;
      }
    }
  }

  PIPE_MGR_UNLOCK(&st->mtx_ses);

  return pipe_mgr_drv_ilist_push_int(st, il, NULL, NULL);
}

pipe_status_t pipe_mgr_drv_ilist_push(pipe_sess_hdl_t *sess,
                                      pipe_mgr_drv_ilist_cb cb_func,
                                      void *usrData) {
  pipe_mgr_drv_ses_state_t *st =
      pipe_mgr_drv_get_ses_state(sess, __func__, __LINE__);
  if (NULL == st) {
    return PIPE_INVALID_ARG;
  }
  PIPE_MGR_MEMSET(&st->iListPendingChkpt, 0, sizeof st->iListPendingChkpt);

  pipe_mgr_drv_list_op_t *il = st->iListPending;
  if (!il || !il->bufCnt) {
    /* No pending operations.  If there is a callback execute it now
     * since the ilist has "completed". */
    if (cb_func) cb_func(usrData, false);
    return PIPE_SUCCESS;
  }

  uint32_t num_subdevices = 0;
  int subdev_id = 0;
  /* If the IL ended targeting a parde stage switch it back to an MAU stage. */
  for (int d = 0; d < PIPE_MGR_NUM_DEVICES; ++d) {
    rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(d);
    if (!dev_info) continue;
    num_subdevices = pipe_mgr_get_num_active_subdevices(d);
    for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
      if (!il->bufs[d][subdev_id]) continue;
      if (il->stage[d][subdev_id] < dev_info->num_active_mau) continue;
      pipe_mgr_drv_buf_t *l;
      PIPE_MGR_DLL_LAST(il->bufs[d][subdev_id], l, next, prev);
      if (l) {
        insert_set_dest_instruction(l, dev_info);
      }
    }
  }

  ++st->cntrs.iListPush;

  st->iListPending = NULL;

  return pipe_mgr_drv_ilist_push_int(st, il, cb_func, usrData);
}

pipe_status_t pipe_mgr_drv_ilist_abort(pipe_sess_hdl_t *sess) {
  pipe_mgr_drv_ses_state_t *st =
      pipe_mgr_drv_get_ses_state(sess, __func__, __LINE__);
  if (NULL == st) {
    return PIPE_INVALID_ARG;
  }

  /* Find the first Instruction List state that hasn't been pushed yet. */
  pipe_mgr_drv_list_op_t *il = st->iListPending;
  if (!il) {
    return PIPE_OBJ_NOT_FOUND;
  }
  ++st->cntrs.iListAbort;
  st->iListPending = NULL;
  PIPE_MGR_MEMSET(&st->iListPendingChkpt, 0, sizeof st->iListPendingChkpt);

  unsigned i;
  uint32_t num_subdevices = 0;
  int subdev_id = 0;
  for (i = 0; i < PIPE_MGR_NUM_DEVICES; ++i) {
    rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(i);
    if (!dev_info) continue;
    num_subdevices = pipe_mgr_get_num_active_subdevices(i);
    for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
      pipe_mgr_drv_buf_t *b = il->bufs[i][subdev_id];
      while (b) {
        PIPE_MGR_DLL_REM(il->bufs[i][subdev_id], b, next, prev);
        pipe_mgr_drv_buf_free(b);
        b = il->bufs[i][subdev_id];
      }
    }
  }
  PIPE_MGR_FREE(il);
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_drv_ilist_chkpt(pipe_sess_hdl_t shdl) {
  pipe_mgr_drv_ses_state_t *st =
      pipe_mgr_drv_get_ses_state(&shdl, __func__, __LINE__);
  if (NULL == st) return PIPE_INVALID_ARG;

  struct pipe_mgr_drv_list_chkpt *cp = &st->iListPendingChkpt;
  pipe_mgr_drv_list_op_t *il = st->iListPending;
  if (!il) {
    PIPE_MGR_MEMSET(cp, 0, sizeof *cp);
    return PIPE_SUCCESS;
  }
  for (bf_dev_id_t dev_id = 0; dev_id < PIPE_MGR_NUM_DEVICES; ++dev_id) {
    rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
    if (!dev_info) continue;
    uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(dev_id);

    for (int subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
      BF_LIST_DLL_LAST(
          il->bufs[dev_id][subdev_id], cp->buf[dev_id][subdev_id], next, prev);
      cp->used[dev_id][subdev_id] =
          cp->buf[dev_id][subdev_id] ? cp->buf[dev_id][subdev_id]->used : 0;
      cp->firstLock[dev_id][subdev_id] = il->firstLock[dev_id][subdev_id];
      cp->lockCnt[dev_id][subdev_id] = il->lockCnt[dev_id][subdev_id];
      cp->firstLockStage[dev_id][subdev_id] =
          il->firstLockStage[dev_id][subdev_id];
      cp->stage[dev_id][subdev_id] = il->curDest[dev_id][subdev_id];
      cp->instrCnt[dev_id][subdev_id] = il->instrCnt[dev_id][subdev_id];
    }
  }
  cp->bufCnt = il->bufCnt;
  cp->is_valid = true;
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_drv_ilist_rollback(pipe_sess_hdl_t shdl) {
  pipe_mgr_drv_ses_state_t *st =
      pipe_mgr_drv_get_ses_state(&shdl, __func__, __LINE__);
  if (NULL == st) return PIPE_INVALID_ARG;
  struct pipe_mgr_drv_list_chkpt *cp = &st->iListPendingChkpt;
  if (!cp->is_valid) {
    /* No checkpoint, fallback to abort. */
    return pipe_mgr_drv_ilist_abort(&shdl);
  }
  if (!st->iListPending) {
    PIPE_MGR_DBGCHK(st->iListPending);
    return PIPE_INVALID_ARG;
  }

  for (int i = 0; i < PIPE_MGR_NUM_DEVICES; ++i) {
    rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(i);
    if (!dev_info) continue;
    uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(i);
    for (int subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
      st->iListPending->firstLock[i][subdev_id] = cp->firstLock[i][subdev_id];
      st->iListPending->lockCnt[i][subdev_id] = cp->lockCnt[i][subdev_id];
      st->iListPending->firstLockStage[i][subdev_id] =
          cp->firstLockStage[i][subdev_id];
      st->iListPending->stage[i][subdev_id] = cp->stage[i][subdev_id];
      st->iListPending->instrCnt[i][subdev_id] = cp->instrCnt[i][subdev_id];

      /* Discard extra DMA buffers.  If we have checkpointed a buffer then
       * discard all buffers after it.  If there is no checkpointed buffer for a
       * device then discard all buffers. */
      pipe_mgr_drv_buf_t *b, *b_next;
      b = cp->buf[i][subdev_id] ? cp->buf[i][subdev_id]->next
                                : st->iListPending->bufs[i][subdev_id];
      while (b) {
        b_next = b->next;
        PIPE_MGR_DLL_REM(st->iListPending->bufs[i][subdev_id], b, next, prev);
        pipe_mgr_drv_buf_free(b);
        b = b_next;
      }
      /* Restore the used count in the last buffer. */
      if (cp->buf[i][subdev_id]) {
        cp->buf[i][subdev_id]->used = cp->used[i][subdev_id];
      }
      /* Restore the current dest. */
      st->iListPending->curDest[i][subdev_id] =
          cp->buf[i][subdev_id] ? cp->buf[i][subdev_id]->pipeMask : 0;
    }
  }
  st->iListPending->bufCnt = cp->bufCnt;

  PIPE_MGR_MEMSET(cp, 0, sizeof *cp);
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_drv_ilist_rd_abort(pipe_sess_hdl_t *sess) {
  pipe_mgr_drv_ses_state_t *st =
      pipe_mgr_drv_get_ses_state(sess, __func__, __LINE__);
  if (NULL == st) return PIPE_INVALID_ARG;

  /* Find the first Instruction List state that hasn't been pushed yet. */
  while (st->iListRdPending) {
    pipe_mgr_drv_rd_list_op_t *il = st->iListRdPending;
    PIPE_MGR_DLL_REM(st->iListRdPending, il, next, prev);
    rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(il->dev_id);
    if (!dev_info) {
      LOG_ERROR("%s: Unable to use dev id %d", __func__, il->dev_id);
      PIPE_MGR_DBGCHK(0);
      return (PIPE_INVALID_ARG);
    }

    int p;
    for (p = 0; p < (int)dev_info->dev_cfg.num_pipelines; ++p) {
      while (il->bufs[p]) {
        pipe_mgr_drv_buf_t *b = il->bufs[p];
        PIPE_MGR_DLL_REM(il->bufs[p], b, next, prev);
        pipe_mgr_drv_buf_free(b);
      }
    }
    free_pipe_mgr_drv_rd_list_op_t(il);
  }

  return PIPE_SUCCESS;
}

/******************************************************************************
 *                                                                            *
 * Callback functions to be registered with the LLD Client Library for the    *
 * notification FIFOs.                                                        *
 *                                                                            *
 *****************************************************************************/
void pipe_mgr_drv_completion_cb(bf_dev_id_t logical_device,
                                bf_subdev_id_t subdev_id,
                                bf_dma_dr_id_t fifo,
                                uint64_t s_or_t,
                                uint32_t attr,
                                uint32_t status,
                                uint32_t type,
                                uint64_t msgId,
                                int s,
                                int e) {
  (void)s_or_t;
  (void)attr;
  (void)type;
  (void)s;
  (void)e;

  // Validate the device id.
  uint8_t dev_id = logical_device;
  if (!pipe_mgr_valid_deviceId(dev_id, __func__, __LINE__)) {
    LOG_ERROR("Invalid device %u at %s:%d", logical_device, __func__, __LINE__);
    return;
  }
  if (!pipe_mgr_drv_ctx()->valid[dev_id]) {
    LOG_ERROR("DMA completion for removed device %d ignored at %s:%d",
              logical_device,
              __func__,
              __LINE__);
    return;
  }

  // Parse the message id for the embeded information.
  uint8_t opType, dev_idMsg, sidMsg;
  pipe_mgr_drv_parse_msgId(msgId, &sidMsg, &dev_idMsg, &opType);
  pipe_mgr_drv_ses_state_t *st = pipe_mgr_drv_msgId_to_sess(msgId);
  if (st == NULL) {
    LOG_ERROR("%s:%d Message Id 0x%" PRIx64 " doesn't have a valid session",
              __func__,
              __LINE__,
              msgId);
    return;
  }

  if (dev_idMsg != dev_id || st->sid != sidMsg) {
    LOG_ERROR("Received msgId 0x%" PRIx64
              " (dev %u, sid %u) on dev %u, subdev %u DR %d by sid %u op %d",
              msgId,
              dev_idMsg,
              sidMsg,
              dev_id,
              subdev_id,
              fifo,
              st->sid,
              opType);
    PIPE_MGR_DBGCHK(dev_idMsg == dev_id);
    PIPE_MGR_DBGCHK(st->sid == sidMsg);
    return;
  }

  //
  // Verify the FIFO is handled and the opType matches up.  Also verify
  // that a valid session was found.
  //
  if (lld_dr_cmp_pipe_read_blk == fifo && rd_blk_msgid_type == opType) {
    // LOG_TRACE("Entering %s for Read Block", __func__);
    if (!st) {
      LOG_ERROR(
          "No session associated with RdBlk Completion. "
          "MsgId 0x%" PRIx64 ".",
          msgId);
      return;
    }
  } else if (lld_dr_cmp_pipe_write_blk == fifo && wr_blk_msgid_type == opType) {
    // LOG_TRACE("Entering %s for Write Block", __func__);
    if (!st) {
      LOG_ERROR(
          "No session associated with WrBlk Completion. "
          "MsgId 0x%" PRIx64 ".",
          msgId);
      return;
    }
  } else if ((lld_dr_cmp_pipe_inst_list_0 == fifo ||
              lld_dr_cmp_pipe_inst_list_1 == fifo ||
              lld_dr_cmp_pipe_inst_list_2 == fifo ||
              lld_dr_cmp_pipe_inst_list_3 == fifo) &&
             (i_list_msgid_type == opType || i_list_rd_msgid_type == opType)) {
    // LOG_TRACE("Entering %s for Instruction List", __func__);
    if (!st) {
      LOG_ERROR(
          "No session associated with Instruction List Completion. "
          "MsgId 0x%" PRIx64 ".",
          msgId);
      return;
    }
  } else {
    LOG_ERROR("Unhandled FIFO %u/opType %u MsgId 0x%" PRIx64 " at %s:%d.",
              fifo,
              opType,
              msgId,
              __func__,
              __LINE__);
    PIPE_MGR_DBGCHK(0);
    return;
  }

  //
  // Handle each completion type
  //
  if (lld_dr_cmp_pipe_read_blk == fifo) {
    rd_blk_cmplt(st, msgId, 0 != status);
  } else if (lld_dr_cmp_pipe_write_blk == fifo) {
    wr_blk_cmplt(st, msgId, dev_id, subdev_id);
  } else if ((lld_dr_cmp_pipe_inst_list_0 == fifo ||
              lld_dr_cmp_pipe_inst_list_1 == fifo ||
              lld_dr_cmp_pipe_inst_list_2 == fifo ||
              lld_dr_cmp_pipe_inst_list_3 == fifo) &&
             i_list_msgid_type == opType) {
    i_list_cmplt(st,
                 msgId,
                 status,
                 dev_id,
                 subdev_id,
                 fifo - lld_dr_cmp_pipe_inst_list_0);
  } else if ((lld_dr_cmp_pipe_inst_list_0 == fifo ||
              lld_dr_cmp_pipe_inst_list_1 == fifo ||
              lld_dr_cmp_pipe_inst_list_2 == fifo ||
              lld_dr_cmp_pipe_inst_list_3 == fifo) &&
             i_list_rd_msgid_type == opType) {
    i_list_rd_cmplt(st,
                    msgId,
                    0 != status,
                    dev_id,
                    subdev_id,
                    fifo - lld_dr_cmp_pipe_inst_list_0);
  } else {
    LOG_ERROR("Unhandled FIFO %u MsgId 0x%" PRIx64 " at %s:%d",
              fifo,
              msgId,
              __func__,
              __LINE__);
    PIPE_MGR_DBGCHK(0);
    return;
  }
}

static void rd_blk_cmplt(pipe_mgr_drv_ses_state_t *st,
                         uint64_t msgId,
                         bool hadError) {
  PIPE_MGR_ASSERT(st);
  pipe_mgr_drv_buf_t *buf = NULL;
  int ret;
  bool done = true;

  /* Lock the session to ensure exclusive access to the read block state
   * stored in the session. */
  PIPE_MGR_LOCK(&st->mtx_ses);

  /* Update debug counters. */
  ++st->cntrs.rdBlkRsp;

  /* Find the read operation and the specific sub operation for this read block
   * completion. */
  pipe_mgr_drv_rd_blk_op_t *op = NULL;
  int sub_op_idx = -1;
  for (pipe_mgr_drv_rd_blk_op_t *x = st->rdBlk; x && !op; x = x->next) {
    for (int i = 0; i < x->num_sub_ops; ++i) {
      if (x->sub_ops[i].msgId == msgId) {
        op = x;
        sub_op_idx = i;
        break;
      }
    }
  }

  /* No state for this completion, ignore it. */
  if (!op) {
    PIPE_MGR_UNLOCK(&st->mtx_ses);
    LOG_ERROR("No pending operation for RdBlk Completion MsgId 0x%" PRIx64,
              msgId);
    return;
  }

  /* Mark this sub operation as completed. */
  op->sub_ops[sub_op_idx].done = true;

  /* Check if the entire operation is now done. */
  for (int i = 0; done && i < op->num_sub_ops; ++i) done = op->sub_ops[i].done;

  /* Unmap the DMA address of the buffer before it is accessed */
  buf = op->sub_ops[sub_op_idx].buf;
  ret = bf_sys_dma_unmap(buf->pool, buf->addr, buf->size, BF_DMA_TO_CPU);
  if (ret != 0) {
    LOG_ERROR("Unable to unmap DMA buffer %p at %s:%d",
              buf->addr,
              __func__,
              __LINE__);
  }

  /* Execute our driver callback. */
  if (op->cb_func)
    op->cb_func(buf,
                op->sub_ops[sub_op_idx].offset,
                op->sub_ops[sub_op_idx].entryCount,
                hadError,
                op->usrData);

  /* Remove the operation from the list of pending operations if all sub
   * operations have compelted. */
  if (done) {
    PIPE_MGR_DLL_REM(st->rdBlk, op, next, prev);
    for (int i = 0; i < op->num_sub_ops; ++i)
      pipe_mgr_drv_buf_free(op->sub_ops[i].buf);
    PIPE_MGR_FREE(op->sub_ops);
    PIPE_MGR_FREE(op);
  }

  PIPE_MGR_UNLOCK(&st->mtx_ses);
}

pipe_status_t pipe_mgr_drv_rd_blk_cmplt_all(pipe_sess_hdl_t sess,
                                            bf_dev_id_t dev_id) {
  pipe_mgr_drv_rd_blk_op_t *op = NULL;
  pipe_mgr_drv_ses_state_t *st =
      pipe_mgr_drv_get_ses_state(&sess, __func__, __LINE__);
  if (NULL == st) {
    return PIPE_INVALID_ARG;
  }

  do {
    /* Take a per-session mutex to ensure exclusive access to the list of
     * read block states stored in the session. */
    PIPE_MGR_LOCK(&st->mtx_ses);

    /* Search the list of pending operations for one targeting this device with
     * pushed buffers. */
    for (op = st->rdBlk; op && (!op->sub_ops[0].buf->buf_pushed ||
                                op->sub_ops[0].buf->devId != dev_id);
         op = op->next)
      ;

    if (op) {
      /* Still have read block operations pending for this device. */

      /* Sanity check that the pending operation was started and is for
       * the correct device. */
      PIPE_MGR_DBGCHK(op->sub_ops[0].buf->buf_pushed);
      PIPE_MGR_DBGCHK(op->sub_ops[0].buf->devId == dev_id);

      /* Call DR service routine to check if the operation can be
       * completed. */
      PIPE_MGR_UNLOCK(&st->mtx_ses);
      pipe_mgr_drv_service_read_blk_drs(dev_id);
    } else {
      /* No pending operations for this device, just unlock the session
       * and return. */
      PIPE_MGR_UNLOCK(&st->mtx_ses);
    }

  } while (op);

  return PIPE_SUCCESS;
}

/* Find a pushed write block operation with the given device id and message id.
 * A per-session mutex lock must be taken when this function is called. */
static pipe_mgr_drv_wr_blk_op_t *drv_wr_blk_find_pushed_op(
    pipe_mgr_drv_ses_state_t *st, bf_dev_id_t dev_id, uint64_t msg_id) {
  pipe_mgr_drv_wr_blk_op_t *op = NULL;

  for (op = st->wrBlk; op; op = op->next) {
    if (op->buf->buf_pushed && op->buf->devId == dev_id && op->msgId == msg_id)
      return op;
  }

  return NULL;
}

static int pipe_mgr_drv_dr_get_used_count(bf_dev_id_t dev_id,
                                          bf_dma_dr_id_t dr_id,
                                          bool from_hw) {
  int dr_usage = 0;
  uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(dev_id);
  for (bf_subdev_id_t subdev_id = 0; subdev_id < (int)num_subdevices;
       subdev_id++) {
    dr_usage += lld_dr_get_used_count(dev_id, subdev_id, dr_id, from_hw);
  }

  return dr_usage;
}

pipe_status_t pipe_mgr_drv_wr_blk_cmplt_all(pipe_sess_hdl_t sess,
                                            bf_dev_id_t dev_id) {
  pipe_mgr_drv_wr_blk_op_t *op = NULL;
  pipe_mgr_drv_ses_state_t *st =
      pipe_mgr_drv_get_ses_state(&sess, __func__, __LINE__);
  if (NULL == st) {
    return PIPE_INVALID_ARG;
  }
  const unsigned int loop_check_limit = 2000000;
  unsigned int loop_counter = 0;
  int initial_cmp_used = -1;
  uint64_t initial_msg_id = 0;
  bool initial_msg_id_set = false;
  int used_cmp = 0;

  do {
    loop_counter++;
    if (loop_counter == loop_check_limit) {
      if (initial_cmp_used < 0) {
        LOG_ERROR("%s:%d Error in getting DR usage for dev: %d",
                  __func__,
                  __LINE__,
                  dev_id);
        return PIPE_COMM_FAIL;
      }
      // TODO do we need to loop for all subdevices?
      used_cmp = pipe_mgr_drv_dr_get_used_count(
          dev_id, lld_dr_cmp_pipe_write_blk, false);
    }
    /* Take a per-session mutex to ensure exclusive access to the list of
     * write block states stored in the session. */
    PIPE_MGR_LOCK(&st->mtx_ses);
    /* Search the list of pending operations for one targeting this device
     * with pushed buffers. */
    for (op = st->wrBlk;
         op && (!op->buf->buf_pushed || op->buf->devId != dev_id);
         op = op->next)
      ;

    if (op) {
      /* Still have write block operations pending for this device. */

      /* Sanity check that the pending operation was started and is for
       * the correct device. */
      PIPE_MGR_DBGCHK(op->buf->buf_pushed);
      PIPE_MGR_DBGCHK(op->buf->devId == dev_id);

      if (loop_counter == loop_check_limit) {
        if ((used_cmp == initial_cmp_used) &&
            drv_wr_blk_find_pushed_op(st, dev_id, initial_msg_id)) {
#ifndef DEVICE_IS_EMULATOR
          LOG_ERROR(
              "%s:%d No progress in processing DR pipe write blk for dev: %d",
              __func__,
              __LINE__,
              dev_id);
          PIPE_MGR_UNLOCK(&st->mtx_ses);
          return PIPE_COMM_FAIL;
#endif
        }
        initial_cmp_used = used_cmp;
        initial_msg_id = op->msgId;
        loop_counter = 0;
      }
      if (!initial_msg_id_set) {
        initial_msg_id = op->msgId;
        initial_msg_id_set = true;
      }

      PIPE_MGR_UNLOCK(&st->mtx_ses);

      /* Call DR service routine to check if the operation can be
       * completed. */
      pipe_mgr_drv_service_write_blk_drs(dev_id, false, true);
      if (initial_cmp_used < 0)
        // TODO do we need to loop for all subdevices?
        initial_cmp_used = pipe_mgr_drv_dr_get_used_count(
            dev_id, lld_dr_cmp_pipe_write_blk, false);

    } else {
      /* No pending operations for this device, just unlock the session
       * and return. */
      PIPE_MGR_UNLOCK(&st->mtx_ses);
    }

  } while (op);

  return PIPE_SUCCESS;
}

static void wr_blk_cmplt(pipe_mgr_drv_ses_state_t *st,
                         uint64_t msgId,
                         uint8_t dev_id,
                         bf_subdev_id_t subdev_id) {
  int ret;
  PIPE_MGR_ASSERT(st);

  /* Take a per-session mutex to ensure exclusive access to the list of write
   * block states stored in the session. */
  PIPE_MGR_LOCK(&st->mtx_ses);

  /* Increment a debug counter. */
  ++st->cntrs.wrBlkRsp;

  /* Search the list of pending operations for one matching this
   * msgId. */
  pipe_mgr_drv_wr_blk_op_t *op;
  for (op = st->wrBlk; op && op->msgId != msgId; op = op->next)
    ;
  if (!op) {
    LOG_ERROR(
        "No pending operation for WrBlk Completion. "
        "MsgId 0x%" PRIx64 ".",
        msgId);
  } else {
    /* Zero out the completed pipes */
    uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(dev_id);
    if (num_subdevices > 1) {
      op->buf->pipeMask &= (~(0xF << (subdev_id * 4)));
    } else {
      op->buf->pipeMask = 0;
    }
    if (op->buf->pipeMask) {
      /* This buffer is still being processed by hardware for some pipelines
       * so don't do anything with it yet. */
      PIPE_MGR_UNLOCK(&st->mtx_ses);
      return;
    }
    /* No extra work to do on a write block completion, free the DMA buffer
     * and the state saved in the sesion. */
    // LOG_TRACE("---FREEING MEMORY for DEV %d, msgId %ld ----- \n",
    //        op->buf->devId, msgId);

    /* Unmap the DMA address of the buffer before it is accessed */
    ret = bf_sys_dma_unmap(
        op->buf->pool, op->buf->addr, op->buf->size, BF_DMA_FROM_CPU);
    if (ret != 0) {
      LOG_ERROR("Unable to unmap DMA buffer %p at %s:%d",
                op->buf->addr,
                __func__,
                __LINE__);
    }
    pipe_mgr_drv_buf_free(op->buf);
    PIPE_MGR_DLL_REM(st->wrBlk, op, next, prev);
    PIPE_MGR_FREE(op);
  }
  PIPE_MGR_UNLOCK(&st->mtx_ses);
}

pipe_status_t pipe_mgr_drv_ilist_rd_cmplt_all(pipe_sess_hdl_t *sess) {
  pipe_mgr_drv_ses_state_t *st;
  st = pipe_mgr_drv_get_ses_state(sess, __func__, __LINE__);
  if (NULL == st) {
    return PIPE_INVALID_ARG;
  }

  while (st->iListRd) {
    unsigned i;
    for (i = 0; i < PIPE_MGR_NUM_DEVICES; ++i) {
      rmt_dev_info_t *d_info = pipe_mgr_get_dev_info(i);
      if (!d_info) continue;
      service_ilist_drs(i);
    }
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_drv_i_list_cmplt_all(pipe_sess_hdl_t *sess) {
  pipe_mgr_drv_ses_state_t *st;
  uint32_t num_subdevices = 0;
  int subdev_id = 0;
  st = pipe_mgr_drv_get_ses_state(sess, __func__, __LINE__);
  if (NULL == st) {
    return PIPE_INVALID_ARG;
  }
  if (!st->iList) {
    return PIPE_SUCCESS;
  }
  pipe_mgr_drv_list_op_t *il;
  int ret;
  while (st->iList) {
    /* Take a per-session mutex to ensure exclusive access to the list of
     * Instruction List states stored in the session. */
    PIPE_MGR_LOCK(&st->mtx_ses);
    /* Loop through the pending operations, stop at the first one that has
     * completed (has a zero buffer count). */
    for (il = st->iList; il && il->bufCnt; il = il->next)
      ;
    if (il) {
      PIPE_MGR_DLL_REM(st->iList, il, next, prev);
      PIPE_MGR_UNLOCK(&st->mtx_ses);
      /* Call the client's completion callback if set. */
      if (il->cb_func) {
        il->cb_func(il->usrData, 0);
      }
      /* Free any buffers still in the list. */
      unsigned i;
      for (i = 0; i < PIPE_MGR_NUM_DEVICES; ++i) {
        rmt_dev_info_t *d_info = pipe_mgr_get_dev_info(i);
        if (!d_info) continue;
        num_subdevices = pipe_mgr_get_num_active_subdevices(i);
        for (subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
          pipe_mgr_drv_buf_t *b = il->bufs[i][subdev_id];
          while (b) {
            PIPE_MGR_DLL_REM(il->bufs[i][subdev_id], b, next, prev);
            /* Unmap the dma address of the buffer */
            ret = bf_sys_dma_unmap(b->pool, b->addr, b->size, BF_DMA_FROM_CPU);
            if (ret != 0) {
              LOG_ERROR("Unable to unmap DMA buffer %p at %s:%d",
                        b->addr,
                        __func__,
                        __LINE__);
            }
            pipe_mgr_drv_buf_free(b);
            b = il->bufs[i][subdev_id];
          }
        }
      }
      /* Clean up the saved state. */
      PIPE_MGR_FREE(il);
    } else {
      PIPE_MGR_UNLOCK(&st->mtx_ses);
    }
    if (st->iList) {
      unsigned i;
      for (i = 0; i < PIPE_MGR_NUM_DEVICES; ++i) {
        /* Service the stats notify DR and the idle notify DR as well as the
         * instruction list dr.  This is done because instruction list
         * operations such as lock instructions will generate messages in
         * these DRs and if they are full then the instruction list processing
         * will block until those DRs have space. */
        rmt_dev_info_t *d_info = pipe_mgr_get_dev_info(i);
        if (!d_info) continue;
        pipe_mgr_drv_service_stats_drs(i);
        pipe_mgr_drv_service_idle_time_drs(i);
        service_ilist_drs(i);
      }
    }
  }
  return PIPE_SUCCESS;
}
static void i_list_cmplt(pipe_mgr_drv_ses_state_t *st,
                         uint64_t msgId,
                         uint32_t errorCode,
                         uint8_t dev_id,
                         bf_subdev_id_t subdev_id,
                         uint8_t fifo) {
  PIPE_MGR_DBGCHK(st);
  PIPE_MGR_DBGCHK(!(fifo & 0xFC));
  PIPE_MGR_LOCK(&st->mtx_ses);
  ++st->cntrs.iListRspBuf;
  pipe_mgr_drv_list_op_t *il = NULL;
  pipe_mgr_drv_buf_t *b = NULL;
  int ret;

  /* For each list pushed to LLD... */
  for (il = st->iList; il; il = il->next) {
    /* For each buffer in that list... */
    for (b = il->bufs[dev_id][subdev_id]; b; b = b->next) {
      /* If the completion is for this buffer break out of both loops. */
      if (b->msgId == msgId) {
        break;
      }
    }
    if (b) {
      break;
    }
  }

  if (!il || !b) {
    LOG_ERROR(
        "No state associated with instruction list buffer "
        "(Session %u msgId 0x%" PRIx64
        ", status %d, dev %d, subdev %d, fifo %d)",
        st->sid,
        msgId,
        errorCode,
        dev_id,
        subdev_id,
        fifo);
    PIPE_MGR_UNLOCK(&st->mtx_ses);
    return;
  }

  if (errorCode) {
    LOG_ERROR(
        "Instruction list DMA completion on dev %d subdev %d IL %d has error "
        "%d, msgId "
        "0x%016" PRIx64,
        dev_id,
        subdev_id,
        fifo,
        errorCode,
        msgId);

    LOG_ERROR(
        "Dev %d Subdev %d IList DMA %d bytes to phyPipeMask %x w/ MsgId "
        "0x%" PRIx64,
        dev_id,
        subdev_id,
        b->used,
        b->pipeMask,
        b->msgId);
    /* Decode the errored instruction list and log it.  Take a guess at the
     * required buffer size for the log message and keep doubling it until it is
     * large enough. */
    char *err_log = NULL;
    int err_log_sz = 2048;
    pipe_status_t err_log_sts = PIPE_SUCCESS;
    rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
    if (!dev_info) {
      LOG_ERROR("%s: Unable to use dev id %d", __func__, dev_id);
      PIPE_MGR_DBGCHK(0);
      return;
    }
    /* Before decoding the instruction list, get the first logical pipe it was
     * targeting.  This may be used to decode some instructions to P4 names. */
    bf_dev_pipe_t first_log_pipe =
        phy_pipe_to_first_log_pipe(dev_info, b->pipeMask);

    do {
      err_log = PIPE_MGR_MALLOC(err_log_sz);
      err_log_sts = pipe_mgr_decode_ilist(dev_info,
                                          first_log_pipe,
                                          (uint32_t *)b->addr,
                                          b->used,
                                          err_log,
                                          err_log_sz);
      if (PIPE_SUCCESS == err_log_sts) {
        LOG_ERROR("\n%s", err_log);
        LOG_ERROR("Dev %d subdev %d End IList DMA decode of MsgId 0x%" PRIx64,
                  dev_id,
                  subdev_id,
                  b->msgId);
      }
      PIPE_MGR_FREE(err_log);
      err_log_sz *= 2;
    } while (err_log_sts == PIPE_NO_SYS_RESOURCES);
  }

  /* Tofino does not support multicast instruction lists. */
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  bool use_mcast = dev_info && dev_info->dev_family != BF_DEV_FAMILY_TOFINO;

  if (use_mcast) {
    PIPE_MGR_DBGCHK(b->buf_pushed & (1 << fifo));

    ilist_pending_dec(dev_id, subdev_id, fifo);

    ++st->cntrs.iListRsp[subdev_id][fifo];
    /* There is only one completion event for all pipes so set the pipe mask
     * to zero since all pipes have processed this buffer. */
    b->pipeMask = 0;
  } else {
    PIPE_MGR_DBGCHK(b->buf_pushed & (1 << fifo));

    ilist_pending_dec(dev_id, subdev_id, fifo);

    ++st->cntrs.iListRsp[subdev_id][fifo];
    /* Clear the bit in the pipeMask corresponding to the FIFO on which the
     * buffer returned.  Once all bits are cleared then hardware is done with
     * the buffer. */
    b->pipeMask &= ~(1 << fifo);
  }
  if (b->pipeMask) {
    /* This buffer is still being processed by hardware for some pipelines
     * so don't do anything with it yet. */
    PIPE_MGR_UNLOCK(&st->mtx_ses);
    return;
  }

  --il->bufCnt;
  if (0 == il->bufCnt) {
    /* Last buffer of the instruction list if the client didn't
     * request an ACK this DMA buffer can be free.
     * If the client didn't register a callback then also clean up the
     * saved state.
     * Note that it is an error to request an ACK but not register a
     * callback. */
    if (il->cb_func && !il->needsAck) {
      /* Callback but no ACK.  Free DMA buffer only. */
      PIPE_MGR_DLL_REM(il->bufs[dev_id][subdev_id], b, next, prev);
      PIPE_MGR_UNLOCK(&st->mtx_ses);
      /* Unmap the DMA address of the buffer */
      ret = bf_sys_dma_unmap(b->pool, b->addr, b->size, BF_DMA_FROM_CPU);
      if (ret != 0) {
        LOG_ERROR("Unable to unmap DMA buffer %p at %s:%d",
                  b->addr,
                  __func__,
                  __LINE__);
      }
      pipe_mgr_drv_buf_free(b);
    } else if (il->cb_func && il->needsAck) {
      /* Callback and ACK, nothing else to do here. */
      PIPE_MGR_UNLOCK(&st->mtx_ses);
    } else {
      /* No callback function.  Free the DMA buffer and clean up the
       * saved operation state. */
      PIPE_MGR_DLL_REM(st->iList, il, next, prev);
      PIPE_MGR_UNLOCK(&st->mtx_ses);
      PIPE_MGR_DLL_REM(il->bufs[dev_id][subdev_id], b, next, prev);
      ret = bf_sys_dma_unmap(b->pool, b->addr, b->size, BF_DMA_FROM_CPU);
      if (ret != 0) {
        LOG_ERROR("Unable to unmap DMA buffer %p at %s:%d",
                  b->addr,
                  __func__,
                  __LINE__);
      }
      pipe_mgr_drv_buf_free(b);
      PIPE_MGR_FREE(il);
    }
  } else if (il->needsAck) {
    /* Not the last buffer in the instruction list and the client
     * requested an ACK.  Therefore do not free the buffer now, the
     * client will want to inspect the data in all buffers once they
     * all return. */
    PIPE_MGR_UNLOCK(&st->mtx_ses);
  } else {
    /* Not the last buffer in the instruction list but since the client
     * didn't request an ACK it is safe to free the DMA buffer now. */
    PIPE_MGR_DLL_REM(il->bufs[dev_id][subdev_id], b, next, prev);
    PIPE_MGR_UNLOCK(&st->mtx_ses);
    ret = bf_sys_dma_unmap(b->pool, b->addr, b->size, BF_DMA_FROM_CPU);
    if (ret != 0) {
      LOG_ERROR("Unable to unmap DMA buffer %p at %s:%d",
                b->addr,
                __func__,
                __LINE__);
    }
    pipe_mgr_drv_buf_free(b);
  }
}

static void i_list_rd_cmplt(pipe_mgr_drv_ses_state_t *st,
                            uint64_t msgId,
                            bool hadError,
                            uint8_t dev_id,
                            bf_subdev_id_t subdev_id,
                            uint8_t fifo) {
  PIPE_MGR_DBGCHK(!hadError);
  PIPE_MGR_DBGCHK(st);
  PIPE_MGR_DBGCHK(!(fifo & 0xFC));
  PIPE_MGR_LOCK(&st->mtx_ses);
  pipe_mgr_drv_rd_list_op_t *il = NULL;
  pipe_mgr_drv_buf_t *b = NULL;
  int ret;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  if (!dev_info) {
    PIPE_MGR_UNLOCK(&st->mtx_ses);
    return;
  }
  /* For each list pushed to LLD... */
  for (il = st->iListRd; il; il = il->next) {
    if (il->dev_id != dev_id) continue;
    int p;
    for (p = 0; p < (int)dev_info->dev_cfg.num_pipelines; ++p) {
      /* For each buffer in that list... */
      for (b = il->bufs[p]; b; b = b->next) {
        /* If the completion is for this buffer break out of all loops. */
        if (b->msgId == msgId) break;
      }
      if (b) break;
    }
    if (b) break;
  }

  if (!il || !b) {
    LOG_ERROR(
        "No state associated with read instruction list buffer "
        "(Session %u msgId 0x%" PRIx64 ")",
        st->sid,
        msgId);
    PIPE_MGR_UNLOCK(&st->mtx_ses);
    return;
  }

  PIPE_MGR_DBGCHK(b->buf_pushed & (1 << fifo));

  ilist_pending_dec(dev_id, subdev_id, fifo);

  --il->bufCnt;
  /* Calculate the returned data size with a few assumptions.  All reads are 4B
   * commands with 16B of response.  There is exactly one set-stage instruction
   * at the begining of the buffer which does not get a response. */
  b->used = ((b->used - 8) / 4) * 16;

  if (0 == il->bufCnt) {
    int p;
    uint32_t buf_sz = pipe_mgr_drv_buf_size(il->dev_id, PIPE_MGR_DRV_BUF_IL);
    for (p = 0; p < (int)dev_info->dev_cfg.num_pipelines; ++p) {
      uint32_t offset = 0;
      uint32_t resp_start = 0;
      bf_dev_pipe_t log_pipe = 0;
      ilist_rd_buf_space(buf_sz, NULL, &resp_start);
      if (il->bufs[p]) {
        /* Get logical pipe-id */
        pipe_mgr_map_phy_pipe_id_to_log_pipe_id(il->dev_id, p, &log_pipe);
      }
      while (il->bufs[p]) {
        b = il->bufs[p];
        ret = bf_sys_dma_unmap(b->pool, b->addr, resp_start, BF_DMA_FROM_CPU);
        if (ret != 0) {
          LOG_ERROR("Unable to unmap DMA buffer %p at %s:%d",
                    b->addr,
                    __func__,
                    __LINE__);
        }
        ret = bf_sys_dma_unmap(
            b->pool, b->addr + resp_start, b->size - resp_start, BF_DMA_TO_CPU);
        if (ret != 0) {
          LOG_ERROR("Unable to unmap DMA buffer %p at %s:%d",
                    b->addr,
                    __func__,
                    __LINE__);
        }
        if (il->cb_func) {
          il->cb_func(il->usrData,
                      il->dev_id,
                      log_pipe,
                      offset,
                      b->addr + resp_start,
                      b->used,
                      false,
                      false);
        }
        offset += b->used;
        PIPE_MGR_DLL_REM(il->bufs[p], b, next, prev);
        pipe_mgr_drv_buf_free(b);
      }
    }
    if (il->cb_func) il->cb_func(il->usrData, 0, 0, 0, 0, 0, true, false);
    PIPE_MGR_DLL_REM(st->iListRd, il, next, prev);
    PIPE_MGR_UNLOCK(&st->mtx_ses);
    if (il->cb_func) il->cb_func(il->usrData, 0, 0, 0, 0, 0, false, true);
    free_pipe_mgr_drv_rd_list_op_t(il);
  } else {
    PIPE_MGR_UNLOCK(&st->mtx_ses);
  }
}

/* Push ilist after fast-reconfig */
pipe_status_t pipe_mgr_drv_reconfig_ilist_push(pipe_sess_hdl_t *sess,
                                               bf_dev_id_t dev_id) {
  unsigned i;
  if (pipe_mgr_is_device_locked(dev_id)) {
    return PIPE_SUCCESS;
  }
  pipe_mgr_drv_ses_state_t *st =
      pipe_mgr_drv_get_ses_state(sess, __func__, __LINE__);
  if (NULL == st) {
    return PIPE_INVALID_ARG;
  }

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, dev_id);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  pipe_mgr_drv_list_op_t *il = st->iListReconfig[dev_id];
  if (!il || !il->bufCnt) {
    /* No pending operations. */
    return PIPE_SUCCESS;
  }

  uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(dev_id);
  for (int subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
    /* If the IL ended targeting a parde stage switch it back to an MAU stage.
     */
    if (il->stage[dev_id][subdev_id] >= dev_info->num_active_mau) {
      pipe_mgr_drv_buf_t *l;
      PIPE_MGR_DLL_LAST(il->bufs[dev_id][subdev_id], l, next, prev);
      if (l) {
        insert_set_dest_instruction(l, dev_info);
      }
    }
  }

  ++st->cntrs.iListPush;

  /* Lock the session until all buffers are pushed to LLD.  This elimates the
   * possibility of completions happening for the first buffers while pushing
   * later buffers and having this thread and another thread working on the
   * instruction list at the same time. */
  PIPE_MGR_LOCK(&st->mtx_ses);

  /* Add this instruction list onto the list of pushed instruction lists. */
  PIPE_MGR_DLL_AP(st->iList, il, next, prev);
  st->iListReconfig[dev_id] = NULL;

  if (*sess != pipe_mgr_ctx->int_ses_hndl) {
    /* Indicate to pipe mgr that fast-reconfig is over for the given session */
    PIPE_MGR_SESS_CLR_RECFG(pipe_mgr_ctx->pipe_mgr_sessions[*sess].flags);
    if (PIPE_MGR_SESS_DELETE(pipe_mgr_ctx->pipe_mgr_sessions[*sess].flags)) {
      /* The pipe mgr session has been previously destroyed */
      PIPE_MGR_SESS_CLR_VALID(pipe_mgr_ctx->pipe_mgr_sessions[*sess].flags);
      PIPE_MGR_SESS_CLR_DELETE(pipe_mgr_ctx->pipe_mgr_sessions[*sess].flags);
    }
  }

  /* For each buffer in the list, assign it a message id and push it onto the
   * LLD fifo. */
  i = dev_id;
  pipe_mgr_drv_buf_t *b;
  for (int subdev_id = 0; subdev_id < (int)num_subdevices; subdev_id++) {
    for (b = il->bufs[i][subdev_id]; b; b = b->next) {
      if (!b->buf_pushed) {
        b->msgId = pipe_mgr_drv_next_msgId(st, i, i_list_msgid_type);
      }

      pipe_status_t sts = ilist_push(st, dev_info, subdev_id, b);
      if (sts != PIPE_SUCCESS) {
        PIPE_MGR_UNLOCK(&st->mtx_ses);
        return sts;
      }

      ++st->cntrs.iListPushBuf;
    }
    /* If any buffers were pushed to the device start the DMA. */
    if (il->bufs[i][subdev_id]) {
      push_ilist_drs(dev_info);
    }
  }

  /* Process completions if we pushed enough buffers to fill the completion
   * DR. */
  for (i = 0; i < PIPE_MGR_NUM_DEVICES; ++i) {
    rmt_dev_info_t *d_info = pipe_mgr_get_dev_info(i);
    if (!d_info) continue;
    while (ilist_pending_full(i)) service_ilist_drs(i);
  }

  PIPE_MGR_UNLOCK(&st->mtx_ses);
  // LOG_TRACE("Exiting %s", __func__);
  return PIPE_SUCCESS;
}

static int log_instr(rmt_dev_info_t *dev_info,
                     int *stage,
                     uint32_t *d,
                     char *log_dst,
                     int *num_instr_words,
                     int log_len) {
  uint32_t data[5] = {le32toh(*d),
                      le32toh(*(d + 1)),
                      le32toh(*(d + 2)),
                      le32toh(*(d + 3)),
                      le32toh(*(d + 4))};
  pipe_instr_common_wd0_t *x = (pipe_instr_common_wd0_t *)data;
  // instr_sz: 0=4bytes, 1=8 bytes, 2=12 bytes, 3=20 bytes
  uint32_t instr_sz = (d[0] >> 28) & 3;  // x->data_width;
  int r = 0;
  if (0 == instr_sz) {
    int extra = 0;
    if (PIPE_INSTR_PUSH_TABLE_MOVE_ADR == decode_instr_opcode(x)) {
      pipe_push_table_move_adr_instr_t *y =
          (pipe_push_table_move_adr_instr_t *)x;
      r = snprintf(log_dst,
                   log_len,
                   "%08X Stg %d MovReg%s Push Tbl %d Addr 0x%x\n",
                   *data,
                   *stage,
                   y->pad0 || y->pad1 ? " INVALID" : "",
                   y->tbl_id,
                   y->s_address);
    } else if (PIPE_INSTR_NOOP == decode_instr_opcode(x)) {
      /* Check for consecutive noops, max 32. */
      int how_many = 1;
      uint32_t next_instr = le32toh(*(d + how_many));
      x = (pipe_instr_common_wd0_t *)&next_instr;
      while (how_many <= 31 &&
             addr_type_instruction == x->pipe_ring_addr_type &&
             PIPE_INSTR_NOOP == decode_instr_opcode(x)) {
        ++how_many;
        next_instr = le32toh(*(d + how_many));
        x = (pipe_instr_common_wd0_t *)&next_instr;
      }
      r = snprintf(log_dst, log_len, "%08X NOPx%d\n", *data, how_many);
      extra = (how_many - 1) * 4;
    } else if (PIPE_INSTR_BARRIER_LOCK == decode_instr_opcode(x)) {
      pipe_barrier_lock_instr_t *y = (pipe_barrier_lock_instr_t *)x;
      r = snprintf(
          log_dst,
          log_len,
          "%08X Stg %d %s %s Tbl %d id 0x%04x\n",
          *data,
          *stage,
          y->lock_type < 2 ? "Barrier" : y->lock_type & 1 ? "UnLock" : "Lock",
          y->lock_type == 1 || y->lock_type == 6 || y->lock_type == 7
              ? "Idle"
              : y->lock_type == 0 || y->lock_type == 4 || y->lock_type == 5
                    ? "Stats"
                    : "Stats+Idle",
          y->tbl_id,
          y->lock_id);
    } else if (PIPE_INSTR_DUMP_STAT_TABLE_ENTRY == decode_instr_opcode(x)) {
      pipe_dump_stat_ent_instr_t *y = (pipe_dump_stat_ent_instr_t *)x;
      r = snprintf(log_dst,
                   log_len,
                   "%08X Stg %d StatEntryDump Tbl %d, Addr 0x%x\n",
                   *data,
                   *stage,
                   y->tbl_id,
                   y->addr);
    } else if (PIPE_INSTR_DUMP_IDLE_TABLE_ENTRY == decode_instr_opcode(x)) {
      pipe_dump_idle_ent_instr_t *y = (pipe_dump_idle_ent_instr_t *)x;
      r = snprintf(log_dst,
                   log_len,
                   "%08X Stg %d IdleEntryDump Tbl %d, %sReset, Addr 0x%x\n",
                   *data,
                   *stage,
                   y->tbl_id,
                   y->clear ? "Do-" : "No-",
                   y->addr);
    } else if (PIPE_INSTR_POP_TABLE_MOVE_ADR == decode_instr_opcode(x)) {
      pipe_pop_table_move_adr_instr_t *y = (pipe_pop_table_move_adr_instr_t *)x;
      r = snprintf(log_dst,
                   log_len,
                   "%08X Stg %d MovReg Pop Tbl %d, Stats %s, Idle %s\n",
                   *data,
                   *stage,
                   y->tbl_id,
                   y->stat_init ? "All 1s" : "Zero",
                   y->idle_max_val ? "Max" : "Max-1");
    } else if (PIPE_INSTR_DUMP_IDLE_TABLE == decode_instr_opcode(x)) {
      pipe_dump_idle_table_instr_t *y = (pipe_dump_idle_table_instr_t *)x;
      r = snprintf(log_dst,
                   log_len,
                   "%08X Stg %d IdleTableDump Tbl %d, %sReset\n",
                   *data,
                   *stage,
                   y->tbl_id,
                   y->clear ? "Do-" : "No-");
    } else if (PIPE_INSTR_DUMP_STAT_TABLE == decode_instr_opcode(x)) {
      pipe_dump_stat_tbl_instr_t *y = (pipe_dump_stat_tbl_instr_t *)x;
      r = snprintf(log_dst,
                   log_len,
                   "%08X Stg %d StatTableDump Tbl %d\n",
                   *data,
                   *stage,
                   y->tbl_id);
    } else if (PIPE_INSTR_ATOMIC_MOD_CSR == decode_instr_opcode(x)) {
      pipe_atomic_mod_csr_instr_t *y = (pipe_atomic_mod_csr_instr_t *)x;
      r = snprintf(log_dst,
                   log_len,
                   "%08X Stg %d AtomicModCsr Dir %d Wide %d Start %d\n",
                   *data,
                   *stage,
                   y->dir,
                   y->wide,
                   y->start);
    } else if (PIPE_INSTR_ATOMIC_MOD_SRAM == decode_instr_opcode(x)) {
      pipe_atomic_mod_sram_instr_t *y = (pipe_atomic_mod_sram_instr_t *)x;
      r = snprintf(log_dst,
                   log_len,
                   "%08X Stg %d AtomicModSram Dir %d\n",
                   *data,
                   *stage,
                   y->dir);
    } else {
      r = snprintf(log_dst, log_len, "%08X Stg %d UNEXPECTED\n", *data, *stage);
    }
    *num_instr_words = 1 + extra / 4;
  } else if (1 == instr_sz) {
    if (PIPE_INSTR_SELECT_DEST == decode_instr_opcode(x)) {
      int instr_pipe = -1;
      int instr_stage = -1;
      bool invalid = false;
      pipe_physical_addr_t addr;
      addr.addr = *(data + 1);
      addr.addr <<= 32;
      switch (dev_info->dev_family) {
        case BF_DEV_FAMILY_TOFINO:
          invalid = (addr.tof.pipe_41_40 != 2) || addr.tof.pipe_always_0 ||
                    (addr.tof.pipe_id_39_37 & 4);
          instr_pipe = addr.tof.pipe_id_39_37;
          instr_stage = addr.tof.pipe_element_36_33;
          break;
        case BF_DEV_FAMILY_TOFINO2:
          invalid = (addr.tof2.pipe_always_1 != 1) || addr.tof2.pipe_always_0;
          instr_pipe = addr.tof2.pipe_id;
          instr_stage = addr.tof2.pipe_stage;
          break;
        case BF_DEV_FAMILY_TOFINO3:
          invalid = (addr.tof3.pipe_always_1 != 1) || addr.tof3.pipe_always_0;
          instr_pipe = addr.tof3.pipe_id;
          instr_stage = addr.tof3.pipe_stage;
          break;
        default:
          break;
      }
      r = snprintf(log_dst,
                   log_len,
                   "%08X %08X SetDest%s Pipe %d Stage %d\n",
                   *data,
                   *(data + 1),
                   invalid ? " INVALID" : "",
                   instr_pipe,
                   instr_stage);
      *stage = instr_stage;
    } else if (PIPE_INSTR_SELECT_DEST_STAGE == decode_instr_opcode(x)) {
      int instr_stage = -1;
      bool invalid = false;
      pipe_physical_addr_t addr;
      addr.addr = *(data + 1);
      addr.addr <<= 32;
      switch (dev_info->dev_family) {
        case BF_DEV_FAMILY_TOFINO:
          invalid = (addr.tof.pipe_41_40 != 2) || addr.tof.pipe_always_0;
          instr_stage = addr.tof.pipe_element_36_33;
          break;
        case BF_DEV_FAMILY_TOFINO2:
          invalid = (addr.tof2.pipe_always_1 != 1) || addr.tof2.pipe_always_0;
          instr_stage = addr.tof2.pipe_stage;
          break;
        case BF_DEV_FAMILY_TOFINO3:
          invalid = (addr.tof3.pipe_always_1 != 1) || addr.tof3.pipe_always_0;
          instr_stage = addr.tof3.pipe_stage;
          break;






        default:
          break;
      }
      r = snprintf(log_dst,
                   log_len,
                   "%08X %08X SetStage Stage %d%s\n",
                   *data,
                   *(data + 1),
                   instr_stage,
                   invalid ? " INVALID" : "");
      *stage = instr_stage;
    } else if (PIPE_INSTR_RUN_SALU == decode_instr_opcode(x)) {
      pipe_run_salu_instr_t *y = (pipe_run_salu_instr_t *)x;
      r = snprintf(log_dst,
                   log_len,
                   "%08X %08X Stg %d RunSALU Table %d Instr %d Addr 0x%x%s\n",
                   *data,
                   *(data + 1),
                   *stage,
                   y->logical_table,
                   y->instr_index,
                   y->data.addr,
                   y->data.pad1 ? " INVALID PAD" : "");
    } else if (PIPE_INSTR_TCAM_COPY_DATA == decode_instr_opcode(x)) {
      uint32_t num_words = x->specific & 0x1FF;
      uint32_t addr_inc_dir = (x->specific >> 9) & 0x1;
      uint32_t dest_line_no = (x + 1)->specific & 0x1FF;
      uint32_t src_line_no = ((x + 1)->specific >> 9) & 0x1FF;
      uint32_t top_row = ((x + 1)->specific >> 18) & 0xF;
      uint32_t bottom_row = ((x + 1)->specific >> 22) & 0xF;
      uint32_t dest_col = ((x + 1)->specific >> 26) & 0x1;
      uint32_t src_col = ((x + 1)->specific >> 27) & 0x1;
      r = snprintf(
          log_dst,
          log_len,
          "%08X %08X Stg %d TcamCopy %s Cnt %d sCol %d dCol %d bRow %d tRow %d "
          "src %d dst %d\n",
          *data,
          *(data + 1),
          *stage,
          addr_inc_dir ? "Inc" : "Dec",
          num_words,
          src_col,
          dest_col,
          bottom_row,
          top_row,
          src_line_no,
          dest_line_no);
    } else {
      r = snprintf(log_dst,
                   log_len,
                   "%08X %08X Stg %d UNEXPECTED\n",
                   *data,
                   *(data + 1),
                   *stage);
    }
    *num_instr_words = 2;
  } else if (2 == instr_sz) {
    r = snprintf(log_dst,
                 log_len,
                 "%08X %08X %08X Stg %d UNEXPECTED\n",
                 *data,
                 *(data + 1),
                 *(data + 2),
                 *stage);
    *num_instr_words = 3;
  } else if (3 == instr_sz) {
    if (PIPE_INSTR_SET_TCAM_WRITE_REG == decode_instr_opcode(x)) {
      uint32_t mem_address = x->specific & 0x1FF;
      uint32_t write_tcam = (x->specific >> 9) & 0x1;
      uint32_t tcam_col = (x->specific >> 10) & 0x1;
      uint32_t tcam_row = (x->specific >> 11) & 0xF;
      r = snprintf(
          log_dst,
          log_len,
          "%08X %08X %08X %08X %08X Stg %d TcamWrite Row %d Col %d Addr %d "
          "WrPending %d\n",
          *data,
          /* Put the next 4 word in little endian format since they are a
           * memory write. */
          htole32(*(data + 1)),
          htole32(*(data + 2)),
          htole32(*(data + 3)),
          htole32(*(data + 4)),
          *stage,
          tcam_row,
          tcam_col,
          mem_address,
          write_tcam);
    } else {
      r = snprintf(log_dst,
                   log_len,
                   "%08X %08X %08X %08X %08X Stg %d UNEXPECTED\n",
                   *data,
                   *(data + 1),
                   *(data + 2),
                   *(data + 3),
                   *(data + 4),
                   *stage);
    }
    *num_instr_words = 5;
  }
  return r;
}
static int log_reg_wr(rmt_dev_info_t *dev_info,
                      int stage,
                      uint32_t *data,
                      char *log_dst,
                      int *num_instr_words,
                      int log_len) {
  uint32_t d[1] = {le32toh(*data)};
  pipe_instr_common_wd0_t *x = (pipe_instr_common_wd0_t *)d;
  uint32_t instr_sz = (d[0] >> 28) & 3;  // x->data_width;
  int r = 0;

  if (0 == instr_sz) {
    r = snprintf(log_dst, log_len, "%08X UNEXPECTED\n", le32toh(*data));
    *num_instr_words = 1;
  } else if (1 == instr_sz) {
    if ((x->specific >> 19) & 0x1FF) {
      r = snprintf(log_dst,
                   log_len,
                   "%08X %08X UNEXPECTED\n",
                   le32toh(*data),
                   le32toh(*(data + 1)));
    } else {
      uint32_t addr = x->specific & 0x7FFFF;
      addr = dev_info->dev_cfg.dir_addr_set_pipe_type(addr);
      addr = dev_info->dev_cfg.pcie_pipe_addr_set_stage(addr, stage);
      char *reg_name = "";
      if (pipe_mgr_log_ilist(dev_info->dev_id) > 1) {
        reg_name =
            lld_reg_parse_get_full_reg_path_name(dev_info->dev_family, addr);
      }
      r = snprintf(log_dst,
                   log_len,
                   "%08X %08X Stg %d WriteReg Addr 0x%07x data 0x%08x %s\n",
                   le32toh(*data),
                   le32toh(*(data + 1)),
                   stage,
                   addr,
                   le32toh(*(data + 1)),
                   reg_name ? reg_name : "");
    }
    *num_instr_words = 2;
  } else if (2 == instr_sz) {
    r = snprintf(log_dst,
                 log_len,
                 "%08X %08X %08X UNEXPECTED\n",
                 le32toh(*data),
                 le32toh(*(data + 1)),
                 le32toh(*(data + 2)));
    *num_instr_words = 3;
  } else {
    r = snprintf(log_dst,
                 log_len,
                 "%08X %08X %08X %08X %08X UNEXPECTED\n",
                 le32toh(*data),
                 le32toh(*(data + 1)),
                 le32toh(*(data + 2)),
                 le32toh(*(data + 3)),
                 le32toh(*(data + 4)));
    *num_instr_words = 5;
  }
  return r;
}
static int log_mem_wr(rmt_dev_info_t *dev_info,
                      bf_dev_pipe_t first_log_pipe,
                      int stage,
                      uint32_t *d,
                      char *log_dst,
                      int *num_instr_words,
                      int log_len) {
  uint32_t data[5] = {le32toh(*d), 0, 0, 0, 0};
  pipe_instr_common_wd0_t *x = (pipe_instr_common_wd0_t *)d;
  pipe_instr_set_memdata_t *y = (pipe_instr_set_memdata_t *)d;
  uint32_t instr_sz = x->data_width;
  int r = 0;
  int unit = dev_info->dev_cfg.mem_id_from_col_row(
      stage, y->head.tf1.mem_col, y->head.tf1.mem_row, y->head.tf1.mem_type);

  /* Look up the name of the table which has this memory.  This only applies to
   * MAU tables (stages 0 .. num-active-stages-1), parde memories do not have
   * table names. */
  pipe_tbl_hdl_t tbl_hdl = 0;
  rmt_tbl_type_t tbl_type;
  pipe_status_t rc =
      stage >= dev_info->num_active_mau
          ? PIPE_INVALID_ARG
          : pipe_mgr_get_mem_id_to_tbl_hdl_mapping(dev_info->dev_id,
                                                   first_log_pipe,
                                                   stage,
                                                   unit,
                                                   y->head.tf1.mem_type,
                                                   &tbl_hdl,
                                                   &tbl_type);
  const char *tbl_name = "";
  if (rc == PIPE_SUCCESS) {
    const char *c =
        pipe_mgr_get_tbl_name(dev_info->dev_id, tbl_hdl, __func__, __LINE__);
    if (c) tbl_name = c;
  }

  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
    case BF_DEV_FAMILY_TOFINO2:
    case BF_DEV_FAMILY_TOFINO3:
      if (0 == instr_sz) {
        r = snprintf(log_dst, log_len, "%08X UNEXPECTED\n", *data);
        *num_instr_words = 1;
      } else if (1 == instr_sz) {
        data[1] = *(d + 1);
        if (y->head.tf1.reserved) {
          r = snprintf(
              log_dst, log_len, "%08X %08X UNEXPECTED\n", *data, *(data + 1));
        } else {
          r = snprintf(log_dst,
                       log_len,
                       "%08X %08X Stg %d PhyWr Type %s Row %d Col %d Unit %d "
                       "Addr %d %s\n",
                       *data,
                       *(data + 1),
                       stage,
                       mem_type_to_str(y->head.tf1.mem_type),
                       y->head.tf1.mem_row,
                       y->head.tf1.mem_col,
                       unit,
                       y->head.tf1.mem_address,
                       tbl_name);
        }
        *num_instr_words = 2;
      } else if (2 == instr_sz) {
        data[1] = *(d + 1);
        data[2] = *(d + 2);
        if (y->head.tf1.reserved) {
          r = snprintf(log_dst,
                       log_len,
                       "%08X %08X %08X UNEXPECTED\n",
                       *data,
                       *(data + 1),
                       *(data + 2));
        } else {
          r = snprintf(log_dst,
                       log_len,
                       "%08X %08X %08X Stg %d PhyWr Type %s Row %d Col %d Unit "
                       "%d Addr %d %s\n",
                       *data,
                       *(data + 1),
                       *(data + 2),
                       stage,
                       mem_type_to_str(y->head.tf1.mem_type),
                       y->head.tf1.mem_row,
                       y->head.tf1.mem_col,
                       unit,
                       y->head.tf1.mem_address,
                       tbl_name);
        }
        *num_instr_words = 3;
      } else {
        data[1] = *(d + 1);
        data[2] = *(d + 2);
        data[3] = *(d + 3);
        data[4] = *(d + 4);
        if (y->head.tf1.reserved) {
          r = snprintf(log_dst,
                       log_len,
                       "%08X %08X %08X %08X %08X UNEXPECTED\n",
                       *data,
                       *(data + 1),
                       *(data + 2),
                       *(data + 3),
                       *(data + 4));
        } else {
          r = snprintf(
              log_dst,
              log_len,
              "%08X %08X %08X %08X %08X Stg %d PhyWr Type %s Row %d Col "
              "%d Unit %d Addr %d %s\n",
              *data,
              *(data + 1),
              *(data + 2),
              *(data + 3),
              *(data + 4),
              stage,
              mem_type_to_str(y->head.tf1.mem_type),
              y->head.tf1.mem_row,
              y->head.tf1.mem_col,
              unit,
              y->head.tf1.mem_address,
              tbl_name);
        }
        *num_instr_words = 5;
      }
      break;










    default:
      PIPE_MGR_DBGCHK(0);
      break;
  }
  return r;
}
static int log_vir_wr(
    int stage, uint32_t *d, char *log_dst, int *num_instr_words, int log_len) {
  uint32_t data[5] = {le32toh(*d), 0, 0, 0, 0};
  pipe_instr_common_wd0_t *x = (pipe_instr_common_wd0_t *)d;
  pipe_instr_set_memdata_v_t *y = (pipe_instr_set_memdata_v_t *)d;
  uint32_t instr_sz = x->data_width;
  int r = 0;

  if (0 == instr_sz) {
    r = snprintf(log_dst, log_len, "%08X UNEXPECTED\n", *data);
    *num_instr_words = 1;
  } else if (1 == instr_sz) {
    data[1] = *(d + 1);
    if (y->s.reserved) {
      r = snprintf(
          log_dst, log_len, "%08X %08X UNEXPECTED\n", *data, *(data + 1));
    } else {
      switch (y->s.v_mem_type) {
        case pipe_virt_mem_type_stat:
          r = snprintf(
              log_dst,
              log_len,
              "%08X %08X Stg %d StatWr Tbl %d Addr 0x%x VPN %d Row %d Wrd %d\n",
              *data,
              *(data + 1),
              stage,
              y->s.table_id,
              y->s.v_address,
              y->s.v_address >> 13,
              (y->s.v_address >> 3) & 0x3FF,
              y->s.v_address & 0x7);
          break;
        case pipe_virt_mem_type_meter:
          r = snprintf(
              log_dst,
              log_len,
              "%08X %08X Stg %d MeterWr Tbl %d Addr 0x%x VPN %d Row %d\n",
              *data,
              *(data + 1),
              stage,
              y->s.table_id,
              y->s.v_address,
              y->s.v_address >> 10,
              y->s.v_address & 0x3FF);
          break;
        case pipe_virt_mem_type_sel_stful:
          r = snprintf(
              log_dst,
              log_len,
              "%08X %08X Stg %d SelStfulWr Tbl %d Addr 0x%x VPN %d Row %d Wrd "
              "%d\n",
              *data,
              *(data + 1),
              stage,
              y->s.table_id,
              y->s.v_address,
              y->s.v_address >> 15,
              (y->s.v_address >> 5) & 0x3FF,
              y->s.v_address & 0x1F);
          break;
        case pipe_virt_mem_type_idle:
          r = snprintf(
              log_dst,
              log_len,
              "%08X %08X Stg %d IdleWr Tbl %d Addr 0x%x VPN %d Row %d Wrd %d\n",
              *data,
              *(data + 1),
              stage,
              y->s.table_id,
              y->s.v_address,
              y->s.v_address >> 14,
              (y->s.v_address >> 4) & 0x3FF,
              y->s.v_address & 0xF);
          break;
      }
    }
    *num_instr_words = 2;
  } else if (2 == instr_sz) {
    data[1] = *(d + 1);
    data[2] = *(d + 2);
    if (y->s.reserved) {
      r = snprintf(log_dst,
                   log_len,
                   "%08X %08X %08X UNEXPECTED\n",
                   *data,
                   *(data + 1),
                   *(data + 2));
    } else {
      switch (y->s.v_mem_type) {
        case pipe_virt_mem_type_stat:
          r = snprintf(
              log_dst,
              log_len,
              "%08X %08X %08X Stg %d StatWr Tbl %d Addr 0x%x VPN %d Row %d Wrd "
              "%d\n",
              *data,
              *(data + 1),
              *(data + 2),
              stage,
              y->s.table_id,
              y->s.v_address,
              y->s.v_address >> 13,
              (y->s.v_address >> 3) & 0x3FF,
              y->s.v_address & 0x7);
          break;
        case pipe_virt_mem_type_meter:
          r = snprintf(
              log_dst,
              log_len,
              "%08X %08X %08X Stg %d MeterWr Tbl %d Addr 0x%x VPN %d Row %d\n",
              *data,
              *(data + 1),
              *(data + 2),
              stage,
              y->s.table_id,
              y->s.v_address,
              y->s.v_address >> 10,
              y->s.v_address & 0x3FF);
          break;
        case pipe_virt_mem_type_sel_stful:
          r = snprintf(
              log_dst,
              log_len,
              "%08X %08X %08X Stg %d SelStfulWr Tbl %d Addr 0x%x VPN %d Row %d "
              "Wrd %d\n",
              *data,
              *(data + 1),
              *(data + 2),
              stage,
              y->s.table_id,
              y->s.v_address,
              y->s.v_address >> 15,
              (y->s.v_address >> 5) & 0x3FF,
              y->s.v_address & 0x1F);
          break;
        case pipe_virt_mem_type_idle:
          r = snprintf(
              log_dst,
              log_len,
              "%08X %08X %08X Stg %d IdleWr Tbl %d Addr 0x%x VPN %d Row %d Wrd "
              "%d\n",
              *data,
              *(data + 1),
              *(data + 2),
              stage,
              y->s.table_id,
              y->s.v_address,
              y->s.v_address >> 14,
              (y->s.v_address >> 4) & 0x3FF,
              y->s.v_address & 0xF);
          break;
      }
    }
    *num_instr_words = 3;
  } else {
    data[1] = *(d + 1);
    data[2] = *(d + 2);
    data[3] = *(d + 3);
    data[4] = *(d + 4);
    if (y->s.reserved) {
      r = snprintf(log_dst,
                   log_len,
                   "%08X %08X %08X %08X %08X UNEXPECTED\n",
                   *data,
                   *(data + 1),
                   *(data + 2),
                   *(data + 3),
                   *(data + 4));
    } else {
      switch (y->s.v_mem_type) {
        case pipe_virt_mem_type_stat:
          r = snprintf(
              log_dst,
              log_len,
              "%08X %08X %08X %08X %08X Stg %d StatWr Tbl %d Addr 0x%x VPN %d "
              "Row %d Wrd %d\n",
              *data,
              *(data + 1),
              *(data + 2),
              *(data + 3),
              *(data + 4),
              stage,
              y->s.table_id,
              y->s.v_address,
              y->s.v_address >> 13,
              (y->s.v_address >> 3) & 0x3FF,
              y->s.v_address & 0x7);
          break;
        case pipe_virt_mem_type_meter:
          r = snprintf(
              log_dst,
              log_len,
              "%08X %08X %08X %08X %08X Stg %d MeterWr Tbl %d Addr 0x%x VPN %d "
              "Row %d\n",
              *data,
              *(data + 1),
              *(data + 2),
              *(data + 3),
              *(data + 4),
              stage,
              y->s.table_id,
              y->s.v_address,
              y->s.v_address >> 10,
              y->s.v_address & 0x3FF);
          break;
        case pipe_virt_mem_type_sel_stful:
          r = snprintf(
              log_dst,
              log_len,
              "%08X %08X %08X %08X %08X Stg %d SelStfulWr Tbl %d Addr 0x%x VPN "
              "%d Row %d Wrd %d\n",
              *data,
              *(data + 1),
              *(data + 2),
              *(data + 3),
              *(data + 4),
              stage,
              y->s.table_id,
              y->s.v_address,
              y->s.v_address >> 15,
              (y->s.v_address >> 5) & 0x3FF,
              y->s.v_address & 0x1F);
          break;
        case pipe_virt_mem_type_idle:
          r = snprintf(
              log_dst,
              log_len,
              "%08X %08X %08X %08X %08X Stg %d IdleWr Tbl %d Addr 0x%x VPN %d "
              "Row %d Wrd %d\n",
              *data,
              *(data + 1),
              *(data + 2),
              *(data + 3),
              *(data + 4),
              stage,
              y->s.table_id,
              y->s.v_address,
              y->s.v_address >> 14,
              (y->s.v_address >> 4) & 0x3FF,
              y->s.v_address & 0xF);
          break;
      }
    }
    *num_instr_words = 5;
  }
  return r;
}

pipe_status_t pipe_mgr_decode_ilist(rmt_dev_info_t *dev_info,
                                    bf_dev_pipe_t log_pipe,
                                    uint32_t *addr,
                                    int buf_len,
                                    char *log_buf,
                                    int log_len) {
  if (log_len < 2 || !log_buf || !addr) return PIPE_INVALID_ARG;
  int stage = -1;
  int pos = 0;
  int log_buf_used = 0, log_buf_left = log_len - 1;
  uint32_t *it = addr;
  while (pos < buf_len) {
    uint32_t d[1] = {le32toh(*it)};
    pipe_instr_common_wd0_t *instr = (pipe_instr_common_wd0_t *)d;
    int instr_size, print_len;
    switch (instr->pipe_ring_addr_type) {
      case addr_type_register:
        print_len = log_reg_wr(dev_info,
                               stage,
                               it,
                               log_buf + log_buf_used,
                               &instr_size,
                               log_buf_left);
        break;
      case addr_type_instruction:
        print_len = log_instr(dev_info,
                              &stage,
                              it,
                              log_buf + log_buf_used,
                              &instr_size,
                              log_buf_left);
        break;
      case addr_type_memdata:
        print_len = log_mem_wr(dev_info,
                               log_pipe,
                               stage,
                               it,
                               log_buf + log_buf_used,
                               &instr_size,
                               log_buf_left);
        break;
      default: /* addr_type_memdata_v */
        print_len = log_vir_wr(
            stage, it, log_buf + log_buf_used, &instr_size, log_buf_left);
        break;
    }
    log_buf_left -= print_len;
    log_buf_used += print_len;
    pos += instr_size * 4;
    it += instr_size;
    if (log_buf_left <= 0) break;
  }
  /* Make sure there is room for one last '\0' */
  if (log_len <= log_buf_used) return PIPE_NO_SYS_RESOURCES;
  log_buf[log_buf_used] = '\0';
  return PIPE_SUCCESS;
}
