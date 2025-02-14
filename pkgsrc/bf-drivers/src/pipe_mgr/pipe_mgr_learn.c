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
 * @file pipe_mgr_learn.c
 * @date
 *
 * Implementation of Learn filter related functions
 */

#include <time.h>
#ifdef __sun
#include <illumos.h>
#endif
/* Module header files */
#include <dvm/bf_drv_intf.h>
#include <tofino_regs/tofino.h>
#include <tof2_regs/tof2_reg_drv.h>
#include <tof3_regs/tof3_reg_drv.h>
#include <lld/bf_dma_if.h>
#include <lld/lld_reg_if.h>
#include <pipe_mgr/pipe_mgr_intf.h>
#include <sys/types.h>
#include <tof2_regs/tof2_reg_drv.h>
#include <tofino_regs/tofino.h>

/* Local header files */
#include "pipe_mgr_drv.h"
#include "pipe_mgr_int.h"
#include "pipe_mgr_learn.h"

static uint64_t learn_digest_count = 0;

static inline void prepare_learn_enable_disable(rmt_dev_info_t *dev_info,
                                                uint8_t phy_pipe_id,
                                                bool enable,
                                                uint32_t *addr,
                                                uint32_t *data) {
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      switch (phy_pipe_id) {
        case 0:
          *addr = offsetof(Tofino, device_select.lfltr0.ctrl.common_ctrl);
          break;
        case 1:
          *addr = offsetof(Tofino, device_select.lfltr1.ctrl.common_ctrl);
          break;
        case 2:
          *addr = offsetof(Tofino, device_select.lfltr2.ctrl.common_ctrl);
          break;
        case 3:
          *addr = offsetof(Tofino, device_select.lfltr3.ctrl.common_ctrl);
          break;
      }
      *data = 0;
      setp_lfltr_common_ctrl_learn_dis(data, enable ? 0 : 1);
      break;
    case BF_DEV_FAMILY_TOFINO2:
      *addr =
          offsetof(tof2_reg, device_select.lfltr[phy_pipe_id].ctrl.common_ctrl);
      *data = 0;
      setp_tof2_lfltr_common_ctrl_learn_dis(data, enable ? 0 : 1);
      break;
    case BF_DEV_FAMILY_TOFINO3:
      *addr = offsetof(tof3_reg,
                       pipes[phy_pipe_id].pardereg.lfltr_reg.ctrl.common_ctrl);
      *data = 0;
      setp_tof3_lfltr_common_ctrl_learn_dis(data, enable ? 0 : 1);
      break;





    default:
      PIPE_MGR_DBGCHK(0);
      *addr = 1;
      *data = 0;
      return;
  }
}
static inline void prepare_rand_hash_seed(rmt_dev_info_t *dev_info,
                                          uint8_t phy_pipe_id,
                                          uint8_t hash_way,
                                          uint32_t *addr,
                                          uint32_t *data) {
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO: {
      switch (phy_pipe_id) {
        case 0:
          *addr =
              offsetof(Tofino, device_select.lfltr0.ctrl.hash_seed[hash_way]);
          break;
        case 1:
          *addr =
              offsetof(Tofino, device_select.lfltr1.ctrl.hash_seed[hash_way]);
          break;
        case 2:
          *addr =
              offsetof(Tofino, device_select.lfltr2.ctrl.hash_seed[hash_way]);
          break;
        case 3:
          *addr =
              offsetof(Tofino, device_select.lfltr3.ctrl.hash_seed[hash_way]);
          break;
        default:
          PIPE_MGR_DBGCHK(0);
          *addr = 1;
          break;
      }
      *data = 0;
      /* Set the 14 bit seed to a random value other than zero. */
      int seed = 1 + (rand() % 0x3FFF);
      setp_lfltr_hash_seed_seed(data, seed);
      break;
    }
    case BF_DEV_FAMILY_TOFINO2: {
      *addr = offsetof(
          tof2_reg, device_select.lfltr[phy_pipe_id].ctrl.hash_seed[hash_way]);
      /* Set the 16 bit seed to a random value other than zero. */
      int seed = 1 + (rand() % 0xFFFF);
      setp_lfltr_hash_seed_seed(data, seed);
      break;
    }
    case BF_DEV_FAMILY_TOFINO3: {
      *addr = offsetof(
          tof3_reg,
          pipes[phy_pipe_id].pardereg.lfltr_reg.ctrl.hash_seed[hash_way]);
      /* Set the 16 bit seed to a random value other than zero. */
      int seed = 1 + (rand() % 0xFFFF);
      setp_tof3_lfltr_hash_seed_seed(data, seed);
      break;
    }
    default:
      PIPE_MGR_DBGCHK(0);
      *addr = 1;
      *data = 0;
      return;
  }
}

static inline pipe_status_t setup_random_bf_hash(rmt_dev_info_t *dev_info,
                                                 bf_subdev_id_t subdev_id,
                                                 uint8_t phy_pipe_id) {
  bf_dev_id_t dev_id = dev_info->dev_id;
  /* Set the hash seed for all ways. */
  for (int way = 0; way < 4; ++way) {
    uint32_t reg_addr = 0, reg_data = 0;
    prepare_rand_hash_seed(dev_info, phy_pipe_id, way, &reg_addr, &reg_data);
    int x = lld_subdev_write_register(dev_id, subdev_id, reg_addr, reg_data);
    if (x) {
      LOG_ERROR(
          "Error writing to learn filter hash seed dev %d ret 0x%x", dev_id, x);
      return PIPE_COMM_FAIL;
    }
  }
  /* Set the hash function for all ways. */
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO: {
      for (int way = 0; way < 4; ++way) {
        uint32_t reg_addr, reg_data;
        /* We are looping over the hash_array register array programming each
         * element.  The addr_hi and addr_lo give the addresses of the first
         * and last register in the array.  The address step is always four for
         * PCIe registers.  It would be nice to use a constant from the
         * register spec to say how many elements are in the array but that
         * define says there are 15 elements in the array when there are
         * actually only 12. */
        uint32_t addr_lo =
            offsetof(Tofino, device_select.lfltr0.hash[way].hash_array[0]);
        uint32_t addr_hi =
            offsetof(Tofino, device_select.lfltr0.hash[way].hash_array[11]);
        uint32_t pipe_step =
            phy_pipe_id *
            (offsetof(Tofino, device_select.lfltr1.hash[way].hash_array[0]) -
             offsetof(Tofino, device_select.lfltr0.hash[way].hash_array[0]));
        for (reg_addr = addr_lo; reg_addr <= addr_hi; reg_addr += 4) {
          while ((reg_data = rand()) == 0)
            ;
          int x = lld_write_register(dev_id, reg_addr + pipe_step, reg_data);
          if (x) {
            LOG_ERROR("Error writing to learn filter hash func dev %d ret 0x%x",
                      dev_id,
                      x);
            return PIPE_COMM_FAIL;
          }
        }
      }
      break;
    }
    case BF_DEV_FAMILY_TOFINO2: {
      for (unsigned int way = 0;
           way < tof2_reg_device_select_lfltr_hash_array_count;
           ++way) {
        for (unsigned int i = 0;
             i < tof2_reg_device_select_lfltr_hash_hash_array_array_count;
             ++i) {
          uint32_t reg_addr, reg_addr_lo, reg_addr_hi, reg_data;
          reg_addr_lo = offsetof(tof2_reg,
                                 device_select.lfltr[phy_pipe_id]
                                     .hash[way]
                                     .hash_array[i]
                                     .hash_array_0_12);
          reg_addr_hi = offsetof(tof2_reg,
                                 device_select.lfltr[phy_pipe_id]
                                     .hash[way]
                                     .hash_array[i]
                                     .hash_array_11_12);
          for (reg_addr = reg_addr_lo; reg_addr <= reg_addr_hi; reg_addr += 4) {
            while ((reg_data = rand()) == 0)
              ;
            int x = lld_write_register(dev_id, reg_addr, reg_data);
            if (x) {
              LOG_ERROR(
                  "Error writing to learn filter hash func dev %d ret 0x%x",
                  dev_id,
                  x);
              return PIPE_COMM_FAIL;
            }
          }
        }
      }
      break;
    }
    case BF_DEV_FAMILY_TOFINO3: {
      for (unsigned int way = 0; way < 0x4; ++way) {
        for (unsigned int i = 0; i < 0x10; ++i) {
          uint32_t reg_addr, reg_addr_lo, reg_addr_hi, reg_data;
          reg_addr_lo = offsetof(tof3_reg,
                                 pipes[phy_pipe_id]
                                     .pardereg.lfltr_reg.hash[way]
                                     .hash_array[i]
                                     .hash_array_0_12);
          reg_addr_hi = offsetof(tof3_reg,
                                 pipes[phy_pipe_id]
                                     .pardereg.lfltr_reg.hash[way]
                                     .hash_array[i]
                                     .hash_array_11_12);
          for (reg_addr = reg_addr_lo; reg_addr <= reg_addr_hi; reg_addr += 4) {
            while ((reg_data = rand()) == 0)
              ;
            int x = lld_subdev_write_register(
                dev_id, subdev_id, reg_addr, reg_data);
            if (x) {
              LOG_ERROR(
                  "Error writing to learn filter hash func dev %d ret 0x%x",
                  dev_id,
                  x);
              return PIPE_COMM_FAIL;
            }
          }
        }
      }
      break;
    }

    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }
  return PIPE_SUCCESS;
}

static void trace_add(bf_dev_id_t dev_id,
                      bf_dev_pipe_t log_pipe,
                      enum bf_learn_filter_trace_type t,
                      int lrn_type,
                      int cnt,
                      void *ptr) {
  // Only subdevice index 0 stores configuration
  pipe_mgr_drv_learn_cfg_t *learn_cfg =
      &pipe_mgr_drv_ctx()->learn_cfg[dev_id][0];
  if (log_pipe >= learn_cfg->num_pipes) return;
  PIPE_MGR_LOCK(&learn_cfg->trace_mtx);
  unsigned int i = learn_cfg->trace_idx[log_pipe] & PIPE_MGR_LEARN_TRACE_MASK;
  ++learn_cfg->trace_idx[log_pipe];
  learn_cfg->trace[log_pipe][i].trace_type = t;
  learn_cfg->trace[log_pipe][i].lrn_type = lrn_type;
  learn_cfg->trace[log_pipe][i].buf_cnt = cnt;
  learn_cfg->trace[log_pipe][i].ptr = ptr;
  clock_gettime(CLOCK_MONOTONIC_RAW, &learn_cfg->trace[log_pipe][i].ts);
  PIPE_MGR_UNLOCK(&learn_cfg->trace_mtx);
}
static void learn_filter_trace_add_clr(bf_dev_id_t dev_id,
                                       bf_dev_pipe_t log_pipe) {
  trace_add(dev_id, log_pipe, BF_LRN_TRACE_FLTR_CLR, -1, -1, NULL);
}
static void learn_filter_trace_add_cb_skip(bf_dev_id_t dev_id,
                                           bf_dev_pipe_t log_pipe,
                                           int lrn_type,
                                           int buf_cnt,
                                           void *ptr) {
  trace_add(dev_id, log_pipe, BF_LRN_TRACE_CB_SKIP, lrn_type, buf_cnt, ptr);
}
static void learn_filter_trace_add_cb_call(bf_dev_id_t dev_id,
                                           bf_dev_pipe_t log_pipe,
                                           int lrn_type,
                                           int buf_cnt,
                                           void *ptr) {
  trace_add(dev_id, log_pipe, BF_LRN_TRACE_CB_CALL, lrn_type, buf_cnt, ptr);
}
static void learn_filter_trace_add_cb_done(bf_dev_id_t dev_id,
                                           bf_dev_pipe_t log_pipe,
                                           int lrn_type,
                                           int buf_cnt,
                                           void *ptr) {
  trace_add(dev_id, log_pipe, BF_LRN_TRACE_CB_DONE, lrn_type, buf_cnt, ptr);
}
static void learn_filter_trace_add_ack(bf_dev_id_t dev_id,
                                       bf_dev_pipe_t log_pipe,
                                       int lrn_type,
                                       int buf_cnt,
                                       void *ptr) {
  trace_add(dev_id, log_pipe, BF_LRN_TRACE_FLTR_ACK, lrn_type, buf_cnt, ptr);
}
static void learn_filter_trace_dump(bf_dev_id_t dev_id) {
  // Only subdevice index 0 stores configuration
  pipe_mgr_drv_learn_cfg_t *learn_cfg =
      &pipe_mgr_drv_ctx()->learn_cfg[dev_id][0];
  PIPE_MGR_LOCK(&learn_cfg->trace_mtx);
  for (unsigned int pipe = 0; pipe < learn_cfg->num_pipes; ++pipe) {
    for (unsigned int i = 0; i < PIPE_MGR_LEARN_TRACE_SIZE; ++i) {
      unsigned int j =
          (learn_cfg->trace_idx[pipe] + i) & PIPE_MGR_LEARN_TRACE_MASK;
      bf_learn_filter_trace_t *e = &learn_cfg->trace[pipe][j];
      LOG_ERROR("%4d %ld.%09ld %8s Pipe %d Type %2d Cnt %4d Ptr %p",
                j,
                e->ts.tv_sec,
                e->ts.tv_nsec,
                bf_learn_filter_trace_type_str(e->trace_type),
                pipe,
                e->lrn_type,
                e->buf_cnt,
                e->ptr);
    }
  }
  PIPE_MGR_UNLOCK(&learn_cfg->trace_mtx);
}

static inline pipe_status_t drain_filter(rmt_dev_info_t *dev_info,
                                         bf_dev_pipe_t log_pipe_id,
                                         bf_dev_pipe_t phy_pipe_id) {
  bf_dev_id_t dev_id = dev_info->dev_id;
  bf_subdev_id_t subdev_id = 0;
  uint32_t val = 0, attempts = 0;
  uint32_t bft_ctrl = 0, int_stat = 0, com_ctrl = 0, bft_state = 0,
           lqt_state = 0, creq_state = 0;
  /* Collect chip specific register addresses first. */
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO: {
      uint32_t pipe_offset = (offsetof(Tofino, device_select.lfltr1) -
                              offsetof(Tofino, device_select.lfltr0));
      bft_ctrl = phy_pipe_id * pipe_offset +
                 offsetof(Tofino, device_select.lfltr0.ctrl.bft_ctrl);
      int_stat = phy_pipe_id * pipe_offset +
                 offsetof(Tofino, device_select.lfltr0.ctrl.int_stat);
      com_ctrl = phy_pipe_id * pipe_offset +
                 offsetof(Tofino, device_select.lfltr0.ctrl.common_ctrl);
      bft_state = phy_pipe_id * pipe_offset +
                  offsetof(Tofino, device_select.lfltr0.ctrl.bft_state);
      lqt_state = phy_pipe_id * pipe_offset +
                  offsetof(Tofino, device_select.lfltr0.ctrl.lqt_state);
      creq_state = phy_pipe_id * pipe_offset +
                   offsetof(Tofino, device_select.lfltr0.ctrl.creq_state);
      break;
    }
    case BF_DEV_FAMILY_TOFINO2: {
      bft_ctrl =
          offsetof(tof2_reg, device_select.lfltr[phy_pipe_id].ctrl.bft_ctrl);
      int_stat =
          offsetof(tof2_reg, device_select.lfltr[phy_pipe_id].ctrl.intr_stat);
      com_ctrl =
          offsetof(tof2_reg, device_select.lfltr[phy_pipe_id].ctrl.common_ctrl);
      bft_state =
          offsetof(tof2_reg, device_select.lfltr[phy_pipe_id].ctrl.bft_state);
      lqt_state =
          offsetof(tof2_reg, device_select.lfltr[phy_pipe_id].ctrl.lqt_state);
      creq_state =
          offsetof(tof2_reg, device_select.lfltr[phy_pipe_id].ctrl.creq_state);
      break;
    }
    case BF_DEV_FAMILY_TOFINO3: {
      subdev_id = phy_pipe_id / BF_SUBDEV_PIPE_COUNT;
      phy_pipe_id = phy_pipe_id % BF_SUBDEV_PIPE_COUNT;
      bft_ctrl = offsetof(tof3_reg,
                          pipes[phy_pipe_id].pardereg.lfltr_reg.ctrl.bft_ctrl);
      int_stat = offsetof(tof3_reg,
                          pipes[phy_pipe_id].pardereg.lfltr_reg.ctrl.intr_stat);
      com_ctrl = offsetof(
          tof3_reg, pipes[phy_pipe_id].pardereg.lfltr_reg.ctrl.common_ctrl);
      bft_state = offsetof(
          tof3_reg, pipes[phy_pipe_id].pardereg.lfltr_reg.ctrl.bft_state);
      lqt_state = offsetof(
          tof3_reg, pipes[phy_pipe_id].pardereg.lfltr_reg.ctrl.lqt_state);
      creq_state = offsetof(
          tof3_reg, pipes[phy_pipe_id].pardereg.lfltr_reg.ctrl.creq_state);
      break;
    }
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }

  /* First wait for the drain bit to be cleared by HW. */
  val = 1;
  while (val) {
    lld_subdev_read_register(dev_id, subdev_id, bft_ctrl, &val);
    if (val != 0 && val != 1) {
      /* Zero and one are the only legal values for the register, any other
       * value indicates a problem. */
      LOG_ERROR(
          "Dev %d phy pipe %d, read unexpected value 0x%x from learn filter "
          "control",
          dev_id,
          phy_pipe_id,
          val);
      return PIPE_COMM_FAIL;
    }
    if (++attempts >= 200000000) {
      /* At this point we've been polling for something in the order of seconds
       * and the filter hasn't finished clearing, something is wrong. */
      uint32_t bft_ctrl_val, int_stat_val, com_ctrl_val, bft_state_val,
          lqt_state_val, creq_state_val;
      lld_subdev_read_register(dev_id, subdev_id, bft_ctrl, &bft_ctrl_val);
      lld_subdev_read_register(dev_id, subdev_id, int_stat, &int_stat_val);
      lld_subdev_read_register(dev_id, subdev_id, com_ctrl, &com_ctrl_val);
      lld_subdev_read_register(dev_id, subdev_id, bft_state, &bft_state_val);
      lld_subdev_read_register(dev_id, subdev_id, lqt_state, &lqt_state_val);
      lld_subdev_read_register(dev_id, subdev_id, creq_state, &creq_state_val);
      LOG_ERROR("Dev %d phy pipe %d, learn filter clear timed out",
                dev_id,
                phy_pipe_id);
      LOG_ERROR(
          "bft_ctrl 0x%x int_stat 0x%x com_ctrl 0x%x bft_state 0x%x lqt_state "
          "0x%x creq_state 0x%x",
          bft_ctrl_val,
          int_stat_val,
          com_ctrl_val,
          bft_state_val,
          lqt_state_val,
          creq_state_val);
      bf_notify_error_events(BF_ERR_SEV_FATAL,
                             dev_id,
                             phy_pipe_id,
                             0,
                             0,
                             BF_ERR_TYPE_GENERIC,
                             BF_ERR_BLK_LFLTR,
                             BF_ERR_LOC_LFLTR_BFT_CLR,
                             NULL,
                             false,
                             NULL,
                             0,
                             "LearnFilter Clear Failure");
      learn_filter_trace_dump(dev_id);
      PIPE_MGR_DBGCHK(val == 0);
      return PIPE_UNEXPECTED;
    }
  }
  /* Now that the filter is ready, write one to start a(nother) clear. */
  learn_filter_trace_add_clr(dev_id, log_pipe_id);
  lld_subdev_write_register(dev_id, subdev_id, bft_ctrl, 1);
  return PIPE_SUCCESS;
}

static inline pipe_status_t set_timeout(rmt_dev_info_t *dev_info,
                                        uint8_t phy_pipe_id,
                                        uint32_t timeout) {
#ifdef DEVICE_IS_EMULATOR
  if (timeout / 10) {
    timeout = timeout / 10;
  }
#endif
  bf_dev_id_t dev_id = dev_info->dev_id;
  uint32_t reg_addr, reg_data = 0;
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO: {
      reg_addr = offsetof(Tofino, device_select.lfltr0.ctrl.lqt_timeout) +
                 phy_pipe_id *
                     (offsetof(Tofino, device_select.lfltr1.ctrl.lqt_timeout) -
                      offsetof(Tofino, device_select.lfltr0.ctrl.lqt_timeout));
      setp_lfltr_lqt_timeout_timeout(&reg_data, timeout);
      int x = lld_write_register(dev_id, reg_addr, reg_data);
      if (x) {
        LOG_ERROR("%s:%d Error writing to learn filter timeout dev %d ret 0x%x",
                  __func__,
                  __LINE__,
                  dev_id,
                  x);
        return PIPE_COMM_FAIL;
      }
      break;
    }
    case BF_DEV_FAMILY_TOFINO2: {
      reg_addr =
          offsetof(tof2_reg, device_select.lfltr[phy_pipe_id].ctrl.lqt_timeout);
      setp_tof2_lfltr_lqt_timeout_timeout(&reg_data, timeout);
      int x = lld_write_register(dev_id, reg_addr, reg_data);
      if (x) {
        LOG_ERROR("%s:%d Error writing to learn filter timeout dev %d ret 0x%x",
                  __func__,
                  __LINE__,
                  dev_id,
                  x);
        return PIPE_COMM_FAIL;
      }
      break;
    }
    case BF_DEV_FAMILY_TOFINO3: {
      reg_addr = offsetof(tof3_reg,
                          pipes[phy_pipe_id % BF_SUBDEV_PIPE_COUNT]
                              .pardereg.lfltr_reg.ctrl.lqt_timeout);
      setp_tof3_lfltr_lqt_timeout_timeout(&reg_data, timeout);
      int x = lld_subdev_write_register(
          dev_id, phy_pipe_id / BF_SUBDEV_PIPE_COUNT, reg_addr, reg_data);
      if (x) {
        LOG_ERROR("%s:%d Error writing to learn filter timeout dev %d ret 0x%x",
                  __func__,
                  __LINE__,
                  dev_id,
                  x);
        return PIPE_COMM_FAIL;
      }
      break;
    }
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }
  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_drv_learn_move_idle_one(
    rmt_dev_info_t *dev_info, bf_dev_pipe_t log_pipe_id);

static pipe_status_t pipe_mgr_learn_hardware_init(rmt_dev_info_t *dev_info,
                                                  bf_dev_pipe_t pipe_id) {
  pipe_status_t ret;
  uint32_t reg_addr = 0, reg_data = 0;
  bf_dev_id_t dev_id = dev_info->dev_id;
  bf_dev_pipe_t phy_pipe_id;
  bf_subdev_id_t subdev_id;
  int lld_ret;

  ret = pipe_mgr_map_pipe_id_log_to_phy(dev_info, pipe_id, &phy_pipe_id);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR(
        "%s : Error in getting physical pipe-id for logical pipe %d, error %s",
        __func__,
        pipe_id,
        pipe_str_err(ret));
    return ret;
  }
  subdev_id = phy_pipe_id / BF_SUBDEV_PIPE_COUNT;
  phy_pipe_id = phy_pipe_id % BF_SUBDEV_PIPE_COUNT;

  if (dev_info->dev_family == BF_DEV_FAMILY_TOFINO3) {
    bf_sku_chip_part_rev_t rev;
    lld_ret = lld_sku_get_chip_part_revision_number(dev_info->dev_id, &rev);
    if (lld_ret) {
      LOG_ERROR("Failed to fetch SKU revision info, dev %d pipe %d status 0x%x",
                dev_id,
                pipe_id,
                lld_ret);
      return PIPE_COMM_FAIL;
    }
    switch (rev) {
      case BF_SKU_CHIP_PART_REV_B0:
      case BF_SKU_CHIP_PART_REV_B1:
        reg_data = 0xa;
        break;
      case BF_SKU_CHIP_PART_REV_A0:
      case BF_SKU_CHIP_PART_REV_A1:
        reg_data = 0x9;
        break;
      default:
        PIPE_MGR_DBGCHK(0);
        return PIPE_UNEXPECTED;
    }
    reg_addr = offsetof(
        tof3_reg,
        pipes[phy_pipe_id].pardereg.dprsrreg.dprsrreg.inp.icr.lfltr_eop_delay);
    lld_ret = lld_subdev_write_register(dev_id, subdev_id, reg_addr, reg_data);
    if (lld_ret) {
      LOG_ERROR(
          "%s:%d Error writing to learn enable register dev %d pipe %d ret "
          "0x%x",
          __func__,
          __LINE__,
          dev_id,
          pipe_id,
          lld_ret);
      return PIPE_COMM_FAIL;
    }
  }

  /* Enable learning */
  prepare_learn_enable_disable(
      dev_info, phy_pipe_id, true, &reg_addr, &reg_data);

  /* The learn filter is in the TM and not accessable via instruction lists so
   * use a direct register write instead. */
  lld_ret = lld_subdev_write_register(dev_id, subdev_id, reg_addr, reg_data);
  if (lld_ret) {
    LOG_ERROR(
        "%s:%d Error writing to learn enable register dev %d pipe %d ret 0x%x",
        __func__,
        __LINE__,
        dev_id,
        pipe_id,
        lld_ret);
    return PIPE_COMM_FAIL;
  }

  srand(1000);
  ret = setup_random_bf_hash(dev_info, subdev_id, phy_pipe_id);
  if (PIPE_SUCCESS != ret) {
    LOG_ERROR("Failed to setup learning filter hash, dev %d pipe %d status %s",
              dev_id,
              pipe_id,
              pipe_str_err(ret));
    return ret;
  }

  return PIPE_SUCCESS;
}

static bool filter_needs_clear(rmt_dev_info_t *dev_info,
                               bf_dev_pipe_t log_pipe) {
  bf_dev_pipe_t phy_pipe;
  if (PIPE_SUCCESS !=
      pipe_mgr_map_pipe_id_log_to_phy(dev_info, log_pipe, &phy_pipe))
    return false;

  uint32_t addr, val, state0 = 0, state1 = 0;
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      addr = offsetof(Tofino, device_select.lfltr0.ctrl.bft_state) +
             phy_pipe * (offsetof(Tofino, device_select.lfltr1.ctrl.bft_state) -
                         offsetof(Tofino, device_select.lfltr0.ctrl.bft_state));
      lld_read_register(dev_info->dev_id, addr, &val);
      state0 = getp_lfltr_bft_state_state0(&val);
      state1 = getp_lfltr_bft_state_state1(&val);
      break;
    case BF_DEV_FAMILY_TOFINO2:
      addr = offsetof(tof2_reg, device_select.lfltr[phy_pipe].ctrl.bft_state);
      lld_read_register(dev_info->dev_id, addr, &val);
      state0 = getp_tof2_lfltr_bft_state_state0(&val);
      state1 = getp_tof2_lfltr_bft_state_state1(&val);
      break;
    case BF_DEV_FAMILY_TOFINO3:
      addr =
          offsetof(tof3_reg, pipes[phy_pipe].pardereg.lfltr_reg.ctrl.bft_ctrl);
      lld_read_register(dev_info->dev_id, addr, &val);
      state0 = getp_tof3_lfltr_bft_state_state0(&val);
      state1 = getp_tof3_lfltr_bft_state_state1(&val);
      break;
      break;

    case BF_DEV_FAMILY_UNKNOWN:
      PIPE_MGR_DBGCHK(0);
      return false;
  }

  /* Correct decoding of learn filter state:
   *   LFLTR_BFT_SM_IDLE  = 0,
   *   LFLTR_BFT_SM_INIT  = 1,
   *   LFLTR_BFT_SM_CLR   = 2,
   *   LFLTR_BFT_SM_EMPTY = 3,
   *   LFLTR_BFT_SM_FILL  = 4,
   *   LFLTR_BFT_SM_DRAIN = 5,
   *   LFLTR_BFT_SM_WAIT  = 6
   * The filter can be reset the bft_ctrl register when it is in the init or the
   * wait states. */
  return state0 == 1 || state1 == 1 || state0 == 6 || state1 == 6;
}

static pipe_status_t pipe_mgr_drv_learn_settings_write(
    bf_dev_id_t dev_id, bf_dev_init_mode_t warm_init_mode) {
  pipe_status_t ret = PIPE_SUCCESS;
  unsigned int i = 0, j = 0;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  if (!dev_info) {
    PIPE_MGR_DBGCHK(dev_info);
    return PIPE_INVALID_ARG;
  }






  for (i = 0; i < dev_info->num_active_pipes; i++) {
    /* In hitless mode the learn filters are already programmed so no need to do
     * the hardware init.  Otherwise we need to program hash functions, enable
     * learning, etc. */
    if (warm_init_mode != BF_DEV_WARM_INIT_HITLESS) {
      ret = pipe_mgr_learn_hardware_init(dev_info, i);
      if (ret != PIPE_SUCCESS) {
        LOG_ERROR("Error in learn hardware init dev %d log-pipe %d rc %s",
                  dev_id,
                  i,
                  pipe_str_err(ret));
        return ret;
      }
    }

    for (j = 0; j < PIPE_MGR_LEARN_FILTERS_PER_PIPE; j++) {
      /* At this point the filters are usually in init mode and need to be
       * cleared in order to get them ready to use.  However, in hitless mode,
       * they may already be in a ready state.  So, in hitless, check the state
       * of the filters and only reset them if needed. */
      if (warm_init_mode == BF_DEV_WARM_INIT_HITLESS)
        if (!filter_needs_clear(dev_info, i)) continue;

      ret = pipe_mgr_drv_learn_move_idle_one(dev_info, i);
      if (ret != PIPE_SUCCESS) {
        LOG_ERROR("Error draining learn filter %d dev %d log-pipe %d ret %s",
                  j,
                  dev_id,
                  i,
                  pipe_str_err(ret));
        return ret;
      }
    }
  }

  /* Configuration is stored only at subdev index 0. */
  pipe_mgr_drv_learn_cfg_t *learn_cfg =
      &pipe_mgr_drv_ctx()->learn_cfg[dev_id][0];
  pipe_mgr_lrn_set_timeout(dev_id, learn_cfg->lrn_dr_tmo_usecs);

  return ret;
}

/** \brief pipe_mgr_drv_learn_buf_init
 *        Init the buffers for learning for the device and push them to device
 *        Allocate memory to store the learn entries
 */
pipe_status_t pipe_mgr_drv_learn_buf_init(pipe_sess_hdl_t h,
                                          bf_dev_id_t dev_id) {
  pipe_status_t ret = PIPE_SUCCESS;

  if (!pipe_mgr_valid_deviceId(dev_id, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }

  pipe_mgr_drv_ses_state_t *st;
  st = pipe_mgr_drv_get_ses_state(&h, __func__, __LINE__);
  if (NULL == st) {
    return PIPE_INVALID_ARG;
  }

  /* Not able to use the below API since the device gets added to
   * device list after this step
   */
  uint32_t num_pipes = 0;
  pipe_mgr_get_num_pipelines(dev_id, &num_pipes);
  if (num_pipes == 0) {
    LOG_ERROR("%s:%d Num pipes is 0 for dev %d", __func__, __LINE__, dev_id);
    return PIPE_INVALID_ARG;
  }

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  if (!dev_info) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }

  uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(dev_id);
  for (uint32_t subdev = 0; subdev < num_subdevices; subdev++) {
    pipe_mgr_drv_learn_cfg_t *learn_cfg =
        &pipe_mgr_drv_ctx()->learn_cfg[dev_id][subdev];
    /* Config stored in subdevice 0. Set num_pipes to 0 for non 0 subdevices.
     * Non 0 subdevs only need to store DR buf related info. */
    learn_cfg->num_bufs =
        pipe_mgr_drv_subdev_buf_count(dev_id, subdev, PIPE_MGR_DRV_BUF_LRN);
    learn_cfg->buf_size =
        pipe_mgr_drv_subdev_buf_size(dev_id, subdev, PIPE_MGR_DRV_BUF_LRN);
    learn_cfg->num_pipes = (subdev == 0) ? num_pipes : 0;
    learn_cfg->num_pipe_profiles =
        (subdev == 0) ? dev_info->num_pipeline_profiles : 0;
    learn_cfg->network_order = false;
    learn_cfg->intr_learn = false;
    learn_cfg->lrn_dr_tmo_usecs = PIPE_MGR_DEFAULT_LEARN_TIMEOUT;

    learn_cfg->bufs = (pipe_mgr_drv_buf_t **)PIPE_MGR_CALLOC(
        learn_cfg->num_bufs, sizeof(pipe_mgr_drv_buf_t *));
    if (learn_cfg->bufs == NULL) {
      LOG_ERROR("%s:%d Malloc failed", __func__, __LINE__);
      return PIPE_NO_SYS_RESOURCES;
    }

    if (subdev == 0) {
      learn_cfg->lrn_pipe_q = (pipe_mgr_drv_lrn_pipe_queue_t *)PIPE_MGR_CALLOC(
          learn_cfg->num_pipes, sizeof(pipe_mgr_drv_lrn_pipe_queue_t));
      if (learn_cfg->lrn_pipe_q == NULL) {
        PIPE_MGR_FREE(learn_cfg->bufs);
        learn_cfg->bufs = NULL;
        learn_cfg->num_pipes = 0;
        learn_cfg->num_bufs = 0;
        LOG_ERROR("%s:%d Malloc failed", __func__, __LINE__);
        return PIPE_NO_SYS_RESOURCES;
      }

      uint32_t i = 0, j = 0;
      learn_cfg->learn_profiles =
          (pipe_mgr_drv_learn_profile_t *)PIPE_MGR_CALLOC(
              learn_cfg->num_pipe_profiles,
              sizeof(pipe_mgr_drv_learn_profile_t));
      if (learn_cfg->learn_profiles == NULL) {
        LOG_ERROR("%s:%d Malloc failed", __func__, __LINE__);
        ret = PIPE_NO_SYS_RESOURCES;
        goto cleanup;
      }
      for (j = 0; j < learn_cfg->num_pipe_profiles; j++) {
        pipe_mgr_drv_learn_profile_t *learn_profile =
            &learn_cfg->learn_profiles[j];
        learn_profile->discard = false;
        for (i = 0; i < PIPE_MGR_NUM_LEARN_TYPES; i++) {
          pipe_mgr_drv_learn_client_t *learn_client =
              &learn_cfg->learn_profiles[j].learn_clients[i];

          learn_client->num_msgs = num_pipes * PIPE_MGR_LEARN_FILTERS_PER_PIPE;
          learn_client->msgs = (pipe_flow_lrn_msg_t **)PIPE_MGR_CALLOC(
              learn_client->num_msgs, sizeof(pipe_flow_lrn_msg_t *));
          if (learn_client->msgs == NULL) {
            LOG_ERROR("%s:%d Malloc failed", __func__, __LINE__);
            ret = PIPE_NO_SYS_RESOURCES;
            goto cleanup;
          }
        }
      }

      /* Allocate memory for the learn filter trace. */
      learn_cfg->trace_idx =
          PIPE_MGR_CALLOC(num_pipes, sizeof *learn_cfg->trace_idx);
      if (!learn_cfg->trace_idx) goto cleanup;
      learn_cfg->trace = PIPE_MGR_CALLOC(num_pipes, sizeof *learn_cfg->trace);
      if (!learn_cfg->trace) goto cleanup;
      for (i = 0; i < num_pipes; ++i) {
        learn_cfg->trace[i] = PIPE_MGR_CALLOC(PIPE_MGR_LEARN_TRACE_SIZE,
                                              sizeof **learn_cfg->trace);
        if (!learn_cfg->trace[i]) goto cleanup;
      }
    } else {  // if subdev == 0
      learn_cfg->lrn_pipe_q = NULL;
      learn_cfg->learn_profiles = NULL;
      learn_cfg->trace_idx = NULL;
      learn_cfg->trace = NULL;
    }

    for (uint32_t i = 0; i < learn_cfg->num_bufs; i++) {
      pipe_mgr_drv_buf_t *b;
      b = pipe_mgr_drv_buf_alloc_subdev(st->sid,
                                        dev_id,
                                        subdev,
                                        learn_cfg->buf_size,
                                        PIPE_MGR_DRV_BUF_LRN,
                                        false);
      if (!b) {
        LOG_ERROR("%s:%d Error allocating learn buffer for dev %d",
                  __func__,
                  __LINE__,
                  dev_id);
        ret = PIPE_NO_SYS_RESOURCES;
        goto cleanup;
      }

      learn_cfg->bufs[i] = b;
    }

    int lq_per_filter = 0;
    switch (dev_info->dev_family) {
      case BF_DEV_FAMILY_TOFINO:
        lq_per_filter = PIPE_MGR_TOF1_MAX_LEARNQ_BUF_SIZE;
        break;
      case BF_DEV_FAMILY_TOFINO2:
        lq_per_filter = PIPE_MGR_TOF2_MAX_LEARNQ_BUF_SIZE;
        break;
      case BF_DEV_FAMILY_TOFINO3:
        lq_per_filter = PIPE_MGR_TOF3_MAX_LEARNQ_BUF_SIZE;
        break;





      case BF_DEV_FAMILY_UNKNOWN:
        PIPE_MGR_DBGCHK(0);
        return PIPE_UNEXPECTED;
    }

    // Will skip if subdev != 0
    for (uint8_t pipe = 0; pipe < learn_cfg->num_pipes; pipe++) {
      profile_id_t prof_id = 0;
      if (pipe_mgr_pipe_to_profile(
              dev_info, pipe, &prof_id, __func__, __LINE__) != PIPE_SUCCESS) {
        continue;
      }
      /* Allocate memory for storing the learn messages */
      for (uint32_t i = 0; i < PIPE_MGR_NUM_LEARN_TYPES; i++) {
        if (!pipe_mgr_entry_format_is_lrn_type_valid(dev_id, prof_id, i))
          continue;

        size_t lrn_digest_data_byte_sz =
            pipe_mgr_entry_format_lrn_cfg_type_sz(dev_id, prof_id, i);
        if (!lrn_digest_data_byte_sz) {
          continue;
        }

        uint8_t filter_idx = 0;
        for (filter_idx = 0; filter_idx < PIPE_MGR_LEARN_FILTERS_PER_PIPE;
             filter_idx++) {
          void *lrn_digest_data = NULL;
          lrn_digest_data =
              PIPE_MGR_MALLOC(lq_per_filter * lrn_digest_data_byte_sz);
          if (lrn_digest_data == NULL) {
            PIPE_MGR_DBGCHK(0);
            ret = PIPE_NO_SYS_RESOURCES;
            goto cleanup;
          }
          learn_cfg->lrn_pipe_q[pipe]
              .lrn_buf[filter_idx]
              .lrn_digest_entries[i] = lrn_digest_data;
        }
      }
    }
  }
  if (!pipe_mgr_is_device_locked(dev_id)) {
    ret = pipe_mgr_learn_buf_load(dev_id, BF_DEV_INIT_COLD);
    if (PIPE_SUCCESS != ret) {
      goto cleanup;
    }
  }
  return ret;

cleanup:
  pipe_mgr_drv_learn_buf_cleanup(dev_id);
  return ret;
}

pipe_status_t pipe_mgr_learn_buf_load(bf_dev_id_t dev_id,
                                      bf_dev_init_mode_t warm_init_mode) {
  uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(dev_id);
  for (uint32_t subdev = 0; subdev < num_subdevices; subdev++) {
    uint32_t i = 0;
    bf_dma_addr_t dma_addr;
    pipe_mgr_drv_learn_cfg_t *learn_cfg =
        &pipe_mgr_drv_ctx()->learn_cfg[dev_id][subdev];

    for (i = 0; i < learn_cfg->num_bufs; i++) {
      pipe_mgr_drv_buf_t *b = learn_cfg->bufs[i];
      /* Map the virtual address of the buffer to the DMA address before it is
         pushed into the Free memory DR */
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
      int x;
      if (LLD_OK != (x = lld_subdev_push_fm(
                         dev_id, subdev, lld_dr_fm_learn, dma_addr, b->size))) {
        /* Unmap the buffer */
        if (bf_sys_dma_unmap(b->pool, b->addr, b->size, BF_DMA_TO_CPU) != 0) {
          LOG_ERROR("Unable to unmap DMA buffer %p at %s:%d",
                    b->addr,
                    __func__,
                    __LINE__);
        }
        LOG_ERROR(
            "%s Error pushing learn free memory to device %d rc %d addr "
            "0x%" PRIx64 " size %d",
            __func__,
            dev_id,
            x,
            dma_addr,
            b->size);
        PIPE_MGR_DBGCHK(x == LLD_OK);
        return PIPE_LLD_FAILED;
      }
    }
  }

  /* Push the buffers */
  pipe_mgr_drv_push_learn_drs(dev_id);

  pipe_mgr_drv_learn_settings_write(dev_id, warm_init_mode);

  return PIPE_SUCCESS;
}

/** \brief pipe_mgr_drv_learn_buf_cleanup
 *        Cleanup the memory and buffers allocated for the device.
 *        SHOULD BE DONE ONLY DURING DEVICE REMOVE. The DMA buffers will
 *        be put into the free pool
 */
pipe_status_t pipe_mgr_drv_learn_buf_cleanup(bf_dev_id_t dev_id) {
  if (!pipe_mgr_valid_deviceId(dev_id, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }
  uint32_t num_subdevices = pipe_mgr_get_num_active_subdevices(dev_id);
  for (uint32_t subdev = 0; subdev < num_subdevices; subdev++) {
    pipe_mgr_drv_learn_cfg_t *learn_cfg =
        &pipe_mgr_drv_ctx()->learn_cfg[dev_id][subdev];

    uint32_t i, j;
    for (j = 0; j < learn_cfg->num_pipe_profiles; j++) {
      for (i = 0; i < PIPE_MGR_NUM_LEARN_TYPES; i++) {
        pipe_mgr_drv_learn_client_t *learn_client =
            &learn_cfg->learn_profiles[j].learn_clients[i];
        if (learn_client->msgs) {
          PIPE_MGR_FREE(learn_client->msgs);
          learn_client->msgs = NULL;
        }
        learn_client->num_msgs = 0;
        learn_client->inuse = false;
      }
    }

    /* Free the memory allocated for storing the learn messages */
    pipe_mgr_drv_lrn_buf_t *lrn_buf = NULL;
    uint8_t pipe = 0;
    uint8_t filter_idx = 0;
    for (pipe = 0; pipe < learn_cfg->num_pipes; pipe++) {
      for (filter_idx = 0; filter_idx < PIPE_MGR_LEARN_FILTERS_PER_PIPE;
           filter_idx++) {
        lrn_buf = &learn_cfg->lrn_pipe_q[pipe].lrn_buf[filter_idx];
        for (i = 0; i < PIPE_MGR_NUM_LEARN_TYPES; i++) {
          /* We can't free the digest entry memory for any digest with an
           * outstanding ack, since the application may still be using it.
           * Free the memory later when the application acks.
           */
          if (lrn_buf->cur_usage[i] == 0 && lrn_buf->lrn_digest_entries[i]) {
            PIPE_MGR_FREE(lrn_buf->lrn_digest_entries[i]);
          }
          lrn_buf->lrn_digest_entries[i] = NULL;
        }
        lrn_buf->waiting_for_ack = false;
      }
    }

    for (i = 0; i < learn_cfg->num_bufs; i++) {
      pipe_mgr_drv_buf_t *b =
          pipe_mgr_drv_ctx()->learn_cfg[dev_id][subdev].bufs[i];
      if (b) {
        pipe_mgr_drv_buf_free(b);
      }
    }

    /* Release memory used by the learn filter trace. */
    if (learn_cfg->trace_idx) {
      PIPE_MGR_FREE(learn_cfg->trace_idx);
      learn_cfg->trace_idx = NULL;
    }
    if (learn_cfg->trace) {
      for (pipe = 0; pipe < learn_cfg->num_pipes; pipe++) {
        if (learn_cfg->trace[pipe]) {
          PIPE_MGR_FREE(learn_cfg->trace[pipe]);
          learn_cfg->trace[pipe] = NULL;
        }
      }
      PIPE_MGR_FREE(learn_cfg->trace);
      learn_cfg->trace = NULL;
    }

    PIPE_MGR_FREE(learn_cfg->bufs);
    PIPE_MGR_FREE(learn_cfg->lrn_pipe_q);
    PIPE_MGR_FREE(learn_cfg->learn_profiles);
    learn_cfg->bufs = NULL;
    learn_cfg->lrn_pipe_q = NULL;
    learn_cfg->learn_profiles = NULL;
    learn_cfg->num_bufs = 0;
    learn_cfg->num_pipes = 0;
  }

  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_drv_learn_move_idle_one(
    rmt_dev_info_t *dev_info, bf_dev_pipe_t log_pipe_id) {
  pipe_status_t ret = PIPE_SUCCESS;
  bf_dev_id_t dev_id = dev_info->dev_id;
  bf_dev_pipe_t phy_pipe_id;

  if (pipe_mgr_is_device_locked(dev_id)) {
    return PIPE_SUCCESS;
  }

  ret = pipe_mgr_map_pipe_id_log_to_phy(dev_info, log_pipe_id, &phy_pipe_id);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR(
        "%s : Error in getting physical pipe-id for logical pipe %d, error %s",
        __func__,
        log_pipe_id,
        pipe_str_err(ret));
    return ret;
  }
  ret = drain_filter(dev_info, log_pipe_id, phy_pipe_id);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR(
        "%s:%d Error moving learn filter to idle, dev %d pipe %d status %s",
        __func__,
        __LINE__,
        dev_id,
        log_pipe_id,
        pipe_str_err(ret));
    return ret;
  }

  LOG_TRACE("%s:%d dev %d pipe %d moved learn filter to idle",
            __func__,
            __LINE__,
            dev_id,
            log_pipe_id);

  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_drv_learn_move_drain_to_idle(
    rmt_dev_info_t *dev_info, bf_dev_pipe_t pipe_id) {
  bf_dev_id_t dev_id = dev_info->dev_id;
  // Only subdevice index 0 stores configuration
  pipe_mgr_drv_learn_cfg_t *learn_cfg =
      &pipe_mgr_drv_ctx()->learn_cfg[dev_id][0];
  uint8_t i;
  pipe_status_t ret = PIPE_SUCCESS;

  for (i = 0; i < PIPE_MGR_LEARN_FILTERS_PER_PIPE; i++) {
    PIPE_MGR_LOCK(&learn_cfg->learn_mtx);
    pipe_mgr_drv_lrn_buf_t *lrn_buf =
        &learn_cfg->lrn_pipe_q[pipe_id]
             .lrn_buf[learn_cfg->lrn_pipe_q[pipe_id].tail];
    if ((learn_cfg->lrn_pipe_q[pipe_id].tail ==
         learn_cfg->lrn_pipe_q[pipe_id].head) &&
        (!lrn_buf->inuse)) {
      /* No more stuff */
      PIPE_MGR_UNLOCK(&learn_cfg->learn_mtx);
      break;
    }

    if (lrn_buf->processing || lrn_buf->waiting_for_ack) {
      PIPE_MGR_UNLOCK(&learn_cfg->learn_mtx);
      break;
    }

    lrn_buf->inuse = false;
    learn_cfg->lrn_pipe_q[pipe_id].tail =
        (learn_cfg->lrn_pipe_q[pipe_id].tail + 1) %
        PIPE_MGR_LEARN_FILTERS_PER_PIPE;
    PIPE_MGR_UNLOCK(&learn_cfg->learn_mtx);

    ret = pipe_mgr_drv_learn_move_idle_one(dev_info, pipe_id);
    if (ret != PIPE_SUCCESS) {
      LOG_ERROR(
          "%s:%d Error draining learn filters on dev %d pipe %d"
          " ret 0x%x",
          __func__,
          __LINE__,
          dev_id,
          pipe_id,
          ret);
    }
  }
  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_drv_learn_send_to_client(
    bf_dev_id_t dev_id,
    profile_id_t prof_id,
    bf_dev_pipe_t pipe_id,
    pipe_mgr_drv_lrn_buf_t *lrn_buf,
    uint32_t filter_idx) {
  // Only subdevice index 0 stores configuration
  pipe_mgr_drv_learn_cfg_t *learn_cfg =
      &pipe_mgr_drv_ctx()->learn_cfg[dev_id][0];
  pipe_mgr_drv_learn_profile_t *learn_profile =
      &learn_cfg->learn_profiles[prof_id];
  unsigned int i = 0;
  pipe_status_t ret = PIPE_SUCCESS;

  for (i = 0; i < PIPE_MGR_NUM_LEARN_TYPES; i++) {
    pipe_mgr_drv_learn_client_t *learn_client =
        &learn_profile->learn_clients[i];

    /* If there are no learn quanta of this type then skip it. */
    if (!lrn_buf->cur_usage[i]) continue;

    pipe_fld_lst_hdl_t fld_lst_hdl =
        pipe_mgr_entry_format_get_handle_of_lrn_cfg_type(dev_id, prof_id, i);

    pipe_flow_lrn_msg_t *lrn_msg =
        PIPE_MGR_CALLOC(1, sizeof(pipe_flow_lrn_msg_t));
    if (lrn_msg == NULL) {
      PIPE_MGR_DBGCHK(0);
      return PIPE_NO_SYS_RESOURCES;
    }

    uint32_t msg_idx = pipe_id * PIPE_MGR_LEARN_FILTERS_PER_PIPE + filter_idx;
    learn_client->msgs[msg_idx] = lrn_msg;
    /* Populate the message */
    lrn_msg->dev_tgt.device_id = dev_id;
    lrn_msg->dev_tgt.dev_pipe_id = pipe_id;
    lrn_msg->num_entries = lrn_buf->cur_usage[i];
    lrn_msg->entries = lrn_buf->lrn_digest_entries[i];
    lrn_msg->flow_lrn_fld_lst_hdl = fld_lst_hdl;

    learn_digest_count += lrn_msg->num_entries;

    /* If nobody has registered for this type then ack it now. */
    if (!learn_client->inuse) {
      learn_filter_trace_add_cb_skip(
          dev_id, pipe_id, i, lrn_buf->count, lrn_msg);
      pipe_mgr_lrn_notify_ack(
          learn_profile->learn_sess_hdl, fld_lst_hdl, lrn_msg);
    } else {
      /* call the callback */
      learn_filter_trace_add_cb_call(
          dev_id, pipe_id, i, lrn_buf->count, lrn_msg);
      ret = learn_client->cb_func(learn_profile->learn_sess_hdl,
                                  lrn_msg,
                                  learn_client->callback_fn_cookie);
      learn_filter_trace_add_cb_done(
          dev_id, pipe_id, i, lrn_buf->count, lrn_msg);
      if (ret != PIPE_SUCCESS) {
        LOG_ERROR(
            "%s:%d Error calling the learn client's callback function"
            " dev %d pipe_id %d lrn_fld_lst_hdl 0x%x ret 0x%x",
            __func__,
            __LINE__,
            dev_id,
            pipe_id,
            learn_client->fld_lst_hdl,
            ret);
        return ret;
      }
    }
  }
  return PIPE_SUCCESS;
}

static pipe_status_t parse_new_lq(uint8_t *learn_entry,
                                  rmt_dev_info_t *dev_info,
                                  profile_id_t prof_id,
                                  uint32_t max_lq,
                                  bf_dev_pipe_t log_pipe,
                                  pipe_mgr_drv_lrn_buf_t *lrn_buf) {
  /* Learn cfg type is guarranteed to be at the 3 least-significant bits of
   * byte 0 */
  /* For learning the bytes are reverse ordered */
  pipe_status_t ret = PIPE_SUCCESS;
  bf_dev_id_t dev_id = dev_info->dev_id;
  // Only subdevice index 0 stores configuration
  pipe_mgr_drv_learn_cfg_t *learn_cfg =
      &pipe_mgr_drv_ctx()->learn_cfg[dev_id][0];
  uint32_t cur_usage;
  uint8_t learn_cfg_type =
      pipe_mgr_get_digest_cfg_type(dev_info, prof_id, learn_entry);
  if (learn_cfg_type >= PIPE_MGR_NUM_LEARN_TYPES) {
    LOG_ERROR("%s:%d Invalid learn_cfg_type %d for dev %d pipe %d",
              __func__,
              __LINE__,
              learn_cfg_type,
              dev_id,
              log_pipe);
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }

  cur_usage = lrn_buf->cur_usage[learn_cfg_type];

  PIPE_MGR_DBGCHK(cur_usage < max_lq);
  /* Call the auto-gen routine to convert learn entry to learn spec format
   * and copy that
   */
  ret = pipe_mgr_entry_format_lrn_decode(
      dev_info,
      prof_id,
      log_pipe,
      learn_cfg_type,
      learn_entry,
      lrn_buf->lrn_digest_entries[learn_cfg_type],
      cur_usage,
      learn_cfg->network_order);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR(
        "%s:%d Error converting learn quanta to learn spec dev %d "
        "pipe %d learn_cfg_type %d ret 0x%x",
        __func__,
        __LINE__,
        dev_id,
        log_pipe,
        learn_cfg_type,
        ret);
    return ret;
  }

  lrn_buf->cur_usage[learn_cfg_type]++;
  lrn_buf->count++;
  return PIPE_SUCCESS;
}
void pipe_mgr_drv_learn_cb(bf_dev_id_t logical_device,
                           bf_subdev_id_t subdev_id,
                           int size,
                           bf_dma_addr_t dma_addr,
                           int start,
                           int end,
                           bf_dev_pipe_t phy_pipe) {
  pipe_status_t ret = PIPE_SUCCESS;
  uint8_t filter_idx = 0;
  pipe_status_t error = PIPE_SUCCESS;
  int push_cnt = 0;
  bool dr_push_err = false;
  lld_err_t x = LLD_OK;
  int i = 0;

  bf_dev_pipe_t pipe;

  bf_dma_addr_t addr_dma;
  size_t buf_size;

  phy_pipe += subdev_id * BF_SUBDEV_PIPE_COUNT;

  /* Get the virtual address from the dma bus address */
  bf_sys_dma_pool_handle_t hndl = pipe_mgr_drv_subdev_dma_pool_handle(
      logical_device, subdev_id, PIPE_MGR_DRV_BUF_LRN);
  buf_size = pipe_mgr_drv_subdev_buf_size(
      logical_device, subdev_id, PIPE_MGR_DRV_BUF_LRN);
  /* Unmap the DMA address of the buffer before it can be used by the pipe
     manager */
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
    return;
  }

  if (bf_sys_log_is_log_enabled(BF_MOD_PIPE, BF_LOG_DBG) == 1) {
    LOG_DBG("Learn Quanta [phy-pipe=%d]:", phy_pipe);
    LOG_DBG("asic=%d : sz=%d : ptr=%p : start=%d : end:%d",
            logical_device,
            size,
            addr,
            start,
            end);
    int q = size / 16;
    /* Print 16 bytes each time and rest at the end */
    for (i = 0; i < q * 16; i += 16) {
      LOG_DBG(
          "%4x : %02x %02x %02x %02x %02x %02x %02x %02x "
          "%02x %02x %02x %02x %02x %02x %02x %02x ",
          i,
          addr[i],
          addr[i + 1],
          addr[i + 2],
          addr[i + 3],
          addr[i + 4],
          addr[i + 5],
          addr[i + 6],
          addr[i + 7],
          addr[i + 8],
          addr[i + 9],
          addr[i + 10],
          addr[i + 11],
          addr[i + 12],
          addr[i + 13],
          addr[i + 14],
          addr[i + 15]);
    }
  }

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(logical_device);
  if (!dev_info) {
    LOG_ERROR("Invalid device %u at %s:%d", logical_device, __func__, __LINE__);
    return;
  }
  bf_dev_id_t devId = dev_info->dev_id;
  ret = pipe_mgr_map_phy_pipe_id_to_log_pipe_id_optimized(
      dev_info, phy_pipe, &pipe);
  if (ret != PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Unable to get logical pipe for physical pipe %d on dev %d",
              __func__,
              __LINE__,
              phy_pipe,
              devId);
    PIPE_MGR_DBGCHK(0);
    return;
  }
  uint32_t max_lq;
  if (dev_info->dev_family == BF_DEV_FAMILY_TOFINO)
    max_lq = PIPE_MGR_TOF1_MAX_LEARNQ_BUF_SIZE;
  else
    max_lq = PIPE_MGR_TOF2_MAX_LEARNQ_BUF_SIZE;

  uint8_t *learn_entry = addr;
  // Handling is done on subdev index 0
  pipe_mgr_drv_learn_cfg_t *learn_cfg =
      &pipe_mgr_drv_ctx()->learn_cfg[devId][0];

  if (start) {
    /* This is the start of a new learn filter eviction.  Update state to mark
     * this filter as current and the other filter as next in line. */
    PIPE_MGR_LOCK(&learn_cfg->learn_mtx);
    filter_idx = learn_cfg->lrn_pipe_q[pipe].head;
    learn_cfg->lrn_pipe_q[pipe].cur = filter_idx;
    learn_cfg->lrn_pipe_q[pipe].head = (learn_cfg->lrn_pipe_q[pipe].head + 1) %
                                       PIPE_MGR_LEARN_FILTERS_PER_PIPE;

    /* Since this is a new filter it should not have any digests associated with
     * it yet. */
    PIPE_MGR_DBGCHK(learn_cfg->lrn_pipe_q[pipe].lrn_buf[filter_idx].count == 0);
    learn_cfg->lrn_pipe_q[pipe].lrn_buf[filter_idx].processing = true;
    learn_cfg->lrn_pipe_q[pipe].lrn_buf[filter_idx].inuse = true;
    PIPE_MGR_UNLOCK(&learn_cfg->learn_mtx);
  } else {
    /* This is a middle or last DMA buffer for the current filter that the HW
     * is evicting. */
    filter_idx = learn_cfg->lrn_pipe_q[pipe].cur;
    PIPE_MGR_DBGCHK(learn_cfg->lrn_pipe_q[pipe].lrn_buf[filter_idx].inuse);
  }

  pipe_mgr_drv_lrn_buf_t *lrn_buf =
      &learn_cfg->lrn_pipe_q[pipe].lrn_buf[filter_idx];
  PIPE_MGR_DBGCHK(lrn_buf->waiting_for_ack == false);

  if (!size) {
    LOG_ERROR("%s:%d Learn buffer with 0 size", __func__, __LINE__);
    error = PIPE_UNEXPECTED;
    goto cleanup;
  }

  profile_id_t prof_id = 0;
  if (pipe_mgr_pipe_to_profile(dev_info, pipe, &prof_id, __func__, __LINE__) !=
      PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Error getting profile from pipe %d for dev %d",
              __func__,
              __LINE__,
              pipe,
              logical_device);
    goto cleanup;
  }
  pipe_mgr_drv_learn_profile_t *learn_profile =
      &learn_cfg->learn_profiles[prof_id];

  PIPE_MGR_LOCK(&learn_cfg->learn_mtx);
  if (!learn_profile->learn_sess_hdl_set || learn_profile->discard) {
    if (start) {
      /* Make sure we discard all messages up until the end */
      learn_profile->discard = true;
    }
    if (end) {
      learn_profile->discard = false;
    }
    PIPE_MGR_UNLOCK(&learn_cfg->learn_mtx);
    LOG_WARN("%s:%d Learn data when no learn clients registered",
             __func__,
             __LINE__);
    error = PIPE_NO_LEARN_CLIENTS;
    goto cleanup;
  }
  PIPE_MGR_UNLOCK(&learn_cfg->learn_mtx);

  if (lrn_buf->c_trailing_bytes) {
    /* Copy the required amount of bytes into the trailing bytes buffer and
     * process the lq
     */
    PIPE_MGR_MEMCPY(&lrn_buf->trailing_bytes[lrn_buf->c_trailing_bytes],
                    learn_entry,
                    PIPE_MGR_LEARNQ_SIZE - lrn_buf->c_trailing_bytes);
    learn_entry += PIPE_MGR_LEARNQ_SIZE - lrn_buf->c_trailing_bytes;
    ret = parse_new_lq(
        lrn_buf->trailing_bytes, dev_info, prof_id, max_lq, pipe, lrn_buf);
    if (ret != PIPE_SUCCESS) {
      LOG_ERROR(
          "%s:%d Error parsing new learn quanta dev %d "
          "pipe %d ret 0x%x",
          __func__,
          __LINE__,
          devId,
          pipe,
          ret);
      error = ret;
      goto cleanup;
    }
  }

  for (; (learn_entry + PIPE_MGR_LEARNQ_SIZE) <= (addr + size);
       learn_entry += PIPE_MGR_LEARNQ_SIZE) {
    ret = parse_new_lq(learn_entry, dev_info, prof_id, max_lq, pipe, lrn_buf);
    if (ret != PIPE_SUCCESS) {
      LOG_ERROR(
          "%s:%d Error parsing new learn quanta dev %d "
          "pipe %d ret 0x%x",
          __func__,
          __LINE__,
          devId,
          pipe,
          ret);
      error = ret;
      goto cleanup;
    }
  }

  /* Store the trailing bytes at the end */
  uint32_t c_trailing_bytes = (addr + size) - learn_entry;
  if (c_trailing_bytes) {
    PIPE_MGR_MEMCPY(lrn_buf->trailing_bytes, learn_entry, c_trailing_bytes);
  }
  lrn_buf->c_trailing_bytes = c_trailing_bytes;

  if (end) {
    PIPE_MGR_DBGCHK(c_trailing_bytes == 0);
    PIPE_MGR_DBGCHK(lrn_buf->count);

    lrn_buf->waiting_for_ack = true;
    lrn_buf->processing = false;
    /* Now ship all the messages for this pipe */
    ret = pipe_mgr_drv_learn_send_to_client(
        devId, prof_id, pipe, lrn_buf, filter_idx);
    if (ret != PIPE_SUCCESS) {
      LOG_ERROR(
          "%s:%d Error sending learn messaage to client dev %d, pipe %d"
          " ret 0x%x",
          __func__,
          __LINE__,
          devId,
          pipe,
          ret);
      error = ret;
      goto cleanup;
    }
  }

cleanup:
  if (error != PIPE_SUCCESS) {
    lrn_buf->c_trailing_bytes = 0;
    lrn_buf->count = 0;
  }
  /* Free this buffer */
  do {
    /* Remap the virtual address of the buffer to the DMA address before it is
       pushed again into the Free memory DR */
    if (bf_sys_dma_map(hndl,
                       addr,
                       (bf_phys_addr_t)dma_addr,
                       buf_size,
                       &addr_dma,
                       BF_DMA_TO_CPU) != 0) {
      LOG_ERROR(
          "Unable to map DMA buffer %p at %s:%d", addr, __func__, __LINE__);
      return;
    }
    x = lld_push_fm(devId, lld_dr_fm_learn, addr_dma, learn_cfg->buf_size);
    if (x != LLD_OK) {
      /* Unmap the buffer */
      if (bf_sys_dma_unmap(hndl, addr, buf_size, BF_DMA_TO_CPU) != 0) {
        LOG_ERROR(
            "Unable to unmap DMA buffer %p at %s:%d", addr, __func__, __LINE__);
      }
      ++push_cnt;
      if (!dr_push_err) {
        LOG_ERROR(
            "%s Error pushing learn free memory to device %d rc %d addr "
            "0x%" PRIx64 " size %zu",
            __func__,
            devId,
            x,
            dma_addr,
            buf_size);
        dr_push_err = true;
      }
    } else if (dr_push_err) {
      LOG_ERROR(
          "%s:%d Retry pushing learn free memory to device %d successful, try "
          "%d",
          __func__,
          __LINE__,
          devId,
          push_cnt);
    }
  } while (x == LLD_ERR_DR_FULL && push_cnt < 1000);
  if (x != LLD_OK) error = PIPE_COMM_FAIL;

  if (end && (error != PIPE_SUCCESS)) {
    if (lrn_buf) {
      lrn_buf->processing = false;
    }
    /* Move the hardware learn buffer into idle mode */
    ret = pipe_mgr_drv_learn_move_drain_to_idle(dev_info, pipe);
    if (ret != PIPE_SUCCESS) {
      LOG_ERROR(
          "%s:%d Error moving learn drain to idle dev %d pipe %d "
          " ret 0x%x",
          __func__,
          __LINE__,
          devId,
          pipe,
          ret);
    }
  }

  /* Push everything */
  pipe_mgr_drv_push_learn_drs(devId);

  return;
}

/* Flow learn notification processing completion acknowledgment */
pipe_status_t pipe_mgr_lrn_notify_ack(pipe_sess_hdl_t sess_hdl,
                                      pipe_fld_lst_hdl_t flow_lrn_fld_lst_hdl,
                                      pipe_flow_lrn_msg_t *flow_lrn_msg) {
  pipe_status_t ret = PIPE_SUCCESS;
  if (flow_lrn_msg == NULL) {
    LOG_ERROR("%s:%d NULL flow learn msg passed", __func__, __LINE__);
    return PIPE_INVALID_ARG;
  }

  bf_dev_id_t dev_id = flow_lrn_msg->dev_tgt.device_id;
  bf_dev_pipe_t pipe_id = flow_lrn_msg->dev_tgt.dev_pipe_id;

  pipe_mgr_drv_learn_cfg_t *learn_cfg =
      &pipe_mgr_drv_ctx()->learn_cfg[dev_id][0];
  if (pipe_id >= learn_cfg->num_pipes) {
    LOG_ERROR("%s:%d Invalid pipe-id 0x%x", __func__, __LINE__, pipe_id);
    return PIPE_INVALID_ARG;
  }

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  profile_id_t prof_id = 0;
  if (pipe_mgr_pipe_to_profile(
          dev_info, pipe_id, &prof_id, __func__, __LINE__) != PIPE_SUCCESS) {
    LOG_ERROR("%s:%d Error getting profile from pipe %d for dev %d",
              __func__,
              __LINE__,
              pipe_id,
              dev_id);
    return PIPE_UNEXPECTED;
  }
  pipe_mgr_drv_learn_profile_t *learn_profile =
      &learn_cfg->learn_profiles[prof_id];

  PIPE_MGR_LOCK(&learn_cfg->learn_mtx);
  if (!learn_profile->learn_sess_hdl_set ||
      (learn_profile->learn_sess_hdl != sess_hdl)) {
    LOG_TRACE(
        "%s:%d Learn notify ack called from a non-learning registered "
        "session %d. Learning session %d",
        __func__,
        __LINE__,
        sess_hdl,
        learn_profile->learn_sess_hdl);
  }
  PIPE_MGR_UNLOCK(&learn_cfg->learn_mtx);

  uint8_t learn_cfg_type = pipe_mgr_entry_format_fld_lst_hdl_to_lq_cfg_type(
      dev_id, prof_id, flow_lrn_fld_lst_hdl);
  if (learn_cfg_type >= PIPE_MGR_NUM_LEARN_TYPES) {
    LOG_ERROR("%s:%d Invalid learn fld list handle 0x%x",
              __func__,
              __LINE__,
              flow_lrn_fld_lst_hdl);
    return PIPE_INVALID_ARG;
  }

  pipe_mgr_drv_learn_client_t *learn_client =
      &learn_profile->learn_clients[learn_cfg_type];
  uint8_t filter_idx = 0;
  uint32_t msg_idx = pipe_id * PIPE_MGR_LEARN_FILTERS_PER_PIPE;
  for (filter_idx = 0; filter_idx < PIPE_MGR_LEARN_FILTERS_PER_PIPE;
       filter_idx++) {
    if (learn_client->msgs[msg_idx + filter_idx] == flow_lrn_msg) {
      break;
    }
  }
  if (filter_idx == PIPE_MGR_LEARN_FILTERS_PER_PIPE) {
    /* A stray ack can be received if the device was re-initialized
     * after the message was sent to the application. In this case, free
     * memory associated with this learn message and return.
     */
    LOG_TRACE(
        "%s:%d Invalid pointer passed for flow learn msg ack for "
        "field list 0x%x",
        __func__,
        __LINE__,
        flow_lrn_fld_lst_hdl);
    PIPE_MGR_FREE(flow_lrn_msg->entries);
    ret = PIPE_SUCCESS;
    goto done;
  }
  learn_client->msgs[msg_idx + filter_idx] = NULL;

  pipe_mgr_drv_lrn_buf_t *lrn_buf =
      &learn_cfg->lrn_pipe_q[pipe_id].lrn_buf[filter_idx];

  if (!lrn_buf->waiting_for_ack || lrn_buf->cur_usage[learn_cfg_type] == 0) {
    /* Duplicate ack */
    LOG_ERROR("%s:%d Duplicate ack received on dev %d pipe %d",
              __func__,
              __LINE__,
              dev_id,
              pipe_id);
    ret = PIPE_INVALID_ARG;
    goto done;
  }
  if (lrn_buf->cur_usage[learn_cfg_type] != flow_lrn_msg->num_entries) {
    LOG_ERROR("%s:%d Received an ack of %d entries, but expected %d entries",
              __func__,
              __LINE__,
              flow_lrn_msg->num_entries,
              lrn_buf->cur_usage[learn_cfg_type]);
    PIPE_MGR_DBGCHK(0);
    ret = PIPE_UNEXPECTED;
    goto done;
  }
  if (lrn_buf->count < flow_lrn_msg->num_entries) {
    LOG_ERROR("%s:%d Received an ack of %d entries, but learn buf has count %d",
              __func__,
              __LINE__,
              flow_lrn_msg->num_entries,
              lrn_buf->count);
    PIPE_MGR_DBGCHK(0);
    ret = PIPE_UNEXPECTED;
    goto done;
  }

  lrn_buf->cur_usage[learn_cfg_type] = 0;
  lrn_buf->count -= flow_lrn_msg->num_entries;

  learn_filter_trace_add_ack(
      dev_id, pipe_id, learn_cfg_type, lrn_buf->count, flow_lrn_msg);
  if (lrn_buf->count == 0) {
    /* Write the register to put the learn buffer into idle state */
    lrn_buf->waiting_for_ack = false;

    ret = pipe_mgr_drv_learn_move_drain_to_idle(dev_info, pipe_id);
    if (ret != PIPE_SUCCESS) {
      LOG_ERROR(
          "%s:%d Error moving learn drain to idle dev %d pipe %d "
          " ret 0x%x",
          __func__,
          __LINE__,
          dev_id,
          pipe_id,
          ret);
    }
  }

done:
  PIPE_MGR_FREE(flow_lrn_msg);
  return ret;
}

/* Flow learn notify registration */
pipe_status_t pipe_mgr_lrn_notification_register(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t devId,
    pipe_fld_lst_hdl_t flow_lrn_fld_lst_hdl,
    pipe_flow_lrn_notify_cb callback_fn,
    void *callback_fn_cookie) {
  if (!pipe_mgr_valid_deviceId(devId, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }

  pipe_mgr_drv_ses_state_t *st;
  st = pipe_mgr_drv_get_ses_state(&sess_hdl, __func__, __LINE__);
  if (NULL == st) {
    return PIPE_INVALID_ARG;
  }

  pipe_mgr_drv_learn_cfg_t *learn_cfg =
      &pipe_mgr_drv_ctx()->learn_cfg[devId][0];

  PIPE_MGR_LOCK(&learn_cfg->learn_mtx);
  profile_id_t prof_id = 0;
  if (pipe_mgr_entry_format_fld_lst_hdl_to_profile(
          devId, flow_lrn_fld_lst_hdl, &prof_id) != PIPE_SUCCESS) {
    PIPE_MGR_UNLOCK(&learn_cfg->learn_mtx);
    LOG_ERROR("%s:%d Error getting profile from field list hdl 0x%x for dev %d",
              __func__,
              __LINE__,
              flow_lrn_fld_lst_hdl,
              devId);
    return PIPE_INVALID_ARG;
  }
  uint8_t learn_cfg_type = pipe_mgr_entry_format_fld_lst_hdl_to_lq_cfg_type(
      devId, prof_id, flow_lrn_fld_lst_hdl);
  pipe_mgr_drv_learn_profile_t *learn_profile =
      &learn_cfg->learn_profiles[prof_id];
  pipe_mgr_drv_learn_client_t *learn_client =
      &learn_profile->learn_clients[learn_cfg_type];

  if (learn_profile->learn_sess_hdl_set &&
      (learn_profile->learn_sess_hdl != sess_hdl)) {
    PIPE_MGR_UNLOCK(&learn_cfg->learn_mtx);
    LOG_ERROR(
        "%s:%d Only one learning session is supported. %d is "
        "already registered. %d cannot be added",
        __func__,
        __LINE__,
        learn_profile->learn_sess_hdl,
        sess_hdl);
    return PIPE_NOT_SUPPORTED;
  }

  if (learn_client->inuse) {
    PIPE_MGR_UNLOCK(&learn_cfg->learn_mtx);
    LOG_ERROR("%s:%d Learn callback for 0x%x already registered in dev %d",
              __func__,
              __LINE__,
              flow_lrn_fld_lst_hdl,
              devId);
    return PIPE_ALREADY_EXISTS;
  }

  learn_client->inuse = true;
  learn_client->fld_lst_hdl = flow_lrn_fld_lst_hdl;
  learn_client->cb_func = callback_fn;
  learn_client->callback_fn_cookie = callback_fn_cookie;

  learn_profile->learn_sess_hdl = sess_hdl;
  learn_profile->learn_sess_hdl_set = true;

  LOG_TRACE("%s:%d Registered learn callback on dev %d for hdl 0x%x (type %d) ",
            __func__,
            __LINE__,
            devId,
            flow_lrn_fld_lst_hdl,
            learn_cfg_type);
  PIPE_MGR_UNLOCK(&learn_cfg->learn_mtx);

  return PIPE_SUCCESS;
}

/* Flow learn notify de-registration */
pipe_status_t pipe_mgr_lrn_notification_deregister(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t devId,
    pipe_fld_lst_hdl_t flow_lrn_fld_lst_hdl) {
  if (!pipe_mgr_valid_deviceId(devId, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }

  pipe_mgr_drv_ses_state_t *st;
  st = pipe_mgr_drv_get_ses_state(&sess_hdl, __func__, __LINE__);
  if (NULL == st) {
    return PIPE_INVALID_ARG;
  }

  pipe_mgr_drv_learn_cfg_t *learn_cfg =
      &pipe_mgr_drv_ctx()->learn_cfg[devId][0];

  PIPE_MGR_LOCK(&learn_cfg->learn_mtx);

  profile_id_t prof_id = 0;
  if (pipe_mgr_entry_format_fld_lst_hdl_to_profile(
          devId, flow_lrn_fld_lst_hdl, &prof_id) != PIPE_SUCCESS) {
    PIPE_MGR_UNLOCK(&learn_cfg->learn_mtx);
    LOG_ERROR("%s:%d Error getting profile from field list hdl 0x%x for dev %d",
              __func__,
              __LINE__,
              flow_lrn_fld_lst_hdl,
              devId);
    return PIPE_INVALID_ARG;
  }

  uint8_t learn_cfg_type = pipe_mgr_entry_format_fld_lst_hdl_to_lq_cfg_type(
      devId, prof_id, flow_lrn_fld_lst_hdl);

  pipe_mgr_drv_learn_profile_t *learn_profile =
      &learn_cfg->learn_profiles[prof_id];

  if (!learn_profile->learn_sess_hdl_set ||
      (learn_profile->learn_sess_hdl != sess_hdl)) {
    PIPE_MGR_UNLOCK(&learn_cfg->learn_mtx);
    LOG_ERROR(
        "%s:%d Learn notify deregister called from a non-learning registered "
        "session %d. Learning session %d",
        __func__,
        __LINE__,
        sess_hdl,
        learn_profile->learn_sess_hdl);
    return PIPE_INVALID_ARG;
  }

  pipe_mgr_drv_learn_client_t *learn_client =
      &learn_profile->learn_clients[learn_cfg_type];

  if (!learn_client->inuse) {
    PIPE_MGR_UNLOCK(&learn_cfg->learn_mtx);
    LOG_ERROR(
        "%s:%d Deregister called for learn callback for "
        "0x%x NOT registered in dev %d",
        __func__,
        __LINE__,
        flow_lrn_fld_lst_hdl,
        devId);
    return PIPE_INVALID_ARG;
  }

  learn_client->inuse = false;

  /* Check if all the notfications are deregistered. In that case,
   * Remove the session hdl from learning cfg
   */
  uint32_t i = 0;
  for (i = 0; i < PIPE_MGR_NUM_LEARN_TYPES; i++) {
    learn_client = &learn_profile->learn_clients[i];
    if (learn_client->inuse) {
      break;
    }
  }

  if (i == PIPE_MGR_NUM_LEARN_TYPES) {
    learn_profile->learn_sess_hdl_set = false;
  }
  PIPE_MGR_UNLOCK(&learn_cfg->learn_mtx);

  LOG_TRACE(
      "%s:%d Deregistered learn callback on dev %d for hdl 0x%x (type %d) ",
      __func__,
      __LINE__,
      devId,
      flow_lrn_fld_lst_hdl,
      learn_cfg_type);

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_lrn_get_timeout(bf_dev_id_t dev_id, uint32_t *usecs) {
  // Only subdev index 0 stores configuration
  pipe_mgr_drv_learn_cfg_t *learn_cfg =
      &pipe_mgr_drv_ctx()->learn_cfg[dev_id][0];
  if (!learn_cfg || !usecs) return PIPE_INVALID_ARG;
  *usecs = learn_cfg->lrn_dr_tmo_usecs;

  return PIPE_SUCCESS;
}

/* Flow learn notification set timeout */
pipe_status_t pipe_mgr_lrn_set_timeout(bf_dev_id_t dev_id, uint32_t usecs) {
  uint64_t timeout_full = 0;
  uint32_t timeout = 0;
  uint32_t i = 0;
  pipe_status_t ret = PIPE_SUCCESS;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  if (!dev_info) {
    PIPE_MGR_DBGCHK(dev_info);
    return PIPE_INVALID_ARG;
  }

  /* Convert the usecs to a number of clocks based on the clock speed */
  timeout_full =
      (usecs * (uint64_t)pipe_mgr_get_bps_clock_speed(dev_id)) / 1000000;

  if (timeout_full > ((1ull << 32ull) - 1)) {
    uint64_t clock_speed = pipe_mgr_get_bps_clock_speed(dev_id);
    if (!clock_speed) {
      LOG_ERROR("%s:%d Clock speed for device id %d is 0",
                __func__,
                __LINE__,
                dev_id);
      return PIPE_UNEXPECTED;
    }

    LOG_ERROR(
        "%s:%d Maximum learn timeout that's supported by device %d "
        "is %lld usecs. %d is not valid",
        __func__,
        __LINE__,
        dev_id,
        ((((1ull << 32ull) - 1) * 1000000) / clock_speed),
        usecs);
    return PIPE_INVALID_ARG;
  }

  timeout = timeout_full & ~0;

  // Store value and use only subdev0 cfg for this configuration, as different
  // values per subdevice are not supported.
  pipe_mgr_drv_learn_cfg_t *learn_cfg =
      &pipe_mgr_drv_ctx()->learn_cfg[dev_id][0];
  if (!learn_cfg) return PIPE_INVALID_ARG;
  learn_cfg->lrn_dr_tmo_usecs = usecs;

  if (!pipe_mgr_is_device_locked(dev_id)) {
    for (i = 0; i < dev_info->num_active_pipes; i++) {
      bf_dev_pipe_t phy_pipe = 0;
      pipe_mgr_map_pipe_id_log_to_phy(dev_info, i, &phy_pipe);
      ret = set_timeout(dev_info, phy_pipe, timeout);
      if (ret != PIPE_SUCCESS) {
        LOG_ERROR(
            "%s:%d Error writing learn filter timeout dev %d pipe %d ret %s",
            __func__,
            __LINE__,
            dev_id,
            i,
            pipe_str_err(ret));
        return ret;
      }
    }
  }

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_lrn_set_network_order_digest(bf_dev_id_t dev_id,
                                                    bool network_order) {
  if (!pipe_mgr_valid_deviceId(dev_id, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }

  // Only subdevice index 0 stores configuration
  pipe_mgr_drv_learn_cfg_t *learn_cfg =
      &pipe_mgr_drv_ctx()->learn_cfg[dev_id][0];
  if (!learn_cfg) return PIPE_INVALID_ARG;
  learn_cfg->network_order = network_order;
  return PIPE_SUCCESS;
}

// Return total count of learn DRs processed
uint64_t pipe_mgr_flow_lrn_dr_count(bf_dev_id_t dev_id) {
  UNUSED(dev_id);
  return learn_digest_count;
}

// Reset count of learn DRs processed
pipe_status_t pipe_mgr_flow_lrn_dr_count_reset(bf_dev_id_t dev_id) {
  UNUSED(dev_id);
  learn_digest_count = 0;
  return PIPE_SUCCESS;
}

// Enable or disable learn DR interrupt processing
pipe_status_t pipe_mgr_flow_lrn_int_enable(bf_dev_id_t dev_id, bool en) {
  uint32_t reg = 0, msk = 0, en0 = 0;

  if (dev_id < 0 || dev_id >= PIPE_MGR_NUM_DEVICES) return PIPE_INVALID_ARG;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  if (!dev_info) return PIPE_INVALID_ARG;

  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      reg = offsetof(Tofino, device_select.cbc.cbc_cbus.int_en_0);
      msk = TOFINO_CBC_CBUS_INT_STAT_LQ_RX_DR_EMPTY;
      break;
    case BF_DEV_FAMILY_TOFINO2:
      reg = offsetof(tof2_reg, device_select.cbc.cbc_cbus.intr_en0_0);
      msk = TOFINO2_CBC_CBUS_INT_STAT_LQ_RX_DR_EMPTY;
      break;
    case BF_DEV_FAMILY_TOFINO3:
      reg = offsetof(tof3_reg, device_select.cbc.cbc_cbus.intr_en0_0);
      msk = TOFINO3_CBC_CBUS_INT_STAT_LQ_RX_DR_EMPTY;
      break;

    case BF_DEV_FAMILY_UNKNOWN:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }

  // Only subdevice index 0 stores configuration
  pipe_mgr_drv_learn_cfg_t *learn_cfg =
      &pipe_mgr_drv_ctx()->learn_cfg[dev_id][0];
  if (!learn_cfg) {
    return PIPE_INVALID_ARG;
  }
  learn_cfg->intr_learn = en;

  for (uint32_t subdev = 0; subdev < dev_info->num_active_subdevices;
       ++subdev) {
    lld_subdev_read_register(dev_id, subdev, reg, &en0);
    if (en) {
      en0 |= msk;
    } else {
      en0 &= ~msk;
    }
    lld_subdev_write_register(dev_id, subdev, reg, en0);
  }

  return PIPE_SUCCESS;
}

bool pipe_mgr_flow_lrn_is_int_enabled(bf_dev_id_t dev_id) {
  if (!pipe_mgr_valid_deviceId(dev_id, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }

  // Only subdevice index 0 stores configuration
  pipe_mgr_drv_learn_cfg_t *learn_cfg =
      &pipe_mgr_drv_ctx()->learn_cfg[dev_id][0];
  if (!learn_cfg) {
    return PIPE_INVALID_ARG;
  }
  return learn_cfg->intr_learn;
}
