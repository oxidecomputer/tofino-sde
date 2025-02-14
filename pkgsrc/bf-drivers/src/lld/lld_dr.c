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


#ifndef __KERNEL__
#include <stdio.h>
#include <string.h>  //for memset
#ifdef __sun
#include <endian.h>
#endif
#else
#include <linux/string.h>  //for memset
#include <asm/byteorder.h>
#ifdef __BIG_ENDIAN
#define __BYTE_ORDER __ORDER_BIG_ENDIAN__
#else
#define __BYTE_ORDER __ORDER_LITTLE_ENDIAN__
#endif
#define bf_sys_assert(x) \
  do {                   \
  } while (0)
#endif
#include <bf_types/bf_types.h>
#include <tofino_regs/tofino.h>
#include <dvm/bf_dma_types.h>
#include <lld/bf_dma_if.h>
#include <lld/bf_int_if.h>
#include "lld_dr.h"
#include <lld/lld_dr_if.h>
#include <lld/lld_subdev_dr_if.h>
#include <lld/lld_reg_if.h>
#include <lld/lld_dr_regs.h>
#include <lld/lld_dr_regs_tof.h>
#include <lld/lld_dr_regs_tof2.h>
#include <lld/lld_dr_regs_tof3.h>

#include <lld/lld_bits.h>
#include <lld/lld_dr_descriptors.h>
#include <lld/lld_fault.h>
#include "lld_dr_tof.h"
#include "lld_dr_tof2.h"
#include "lld_dr_tof3.h"
#include "lld.h"
#include "lld_map.h"
#include "lld_log.h"
#include "lld_memory_mapping.h"
#include "lld_dev.h"


/* Default values for PBUS Arbitration Control Registers */
#define PBUS_ARB_TOF3_CTRL_0_DEF 0x01f00ff1;
#define PBUS_ARB_TOF2_CTRL_0_DEF 0x01f00ff1;
#define PBUS_ARB_TOF_CTRL_0_DEF 0x01fffff1;
#define PBUS_ARB_CTRL_1_DEF_A 0x0000000f;
#define PBUS_ARB_CTRL_1_DEF_B 0x00000001;
#define PBUS_ARB_TOF2_CTRL_1_DEF_A 0x0000000f;
#define PBUS_ARB_TOF2_CTRL_1_DEF_B 0x00000001;
#define PBUS_ARB_TOF_CTRL_1_DEF 0x00000001;

/*
   LLD "slices"

   LLD DMA functionality is designed to be "sliced", vertically, so
   functionality may be hosted in independent processes if desired.
   Each slice controls a subset of the DRs. DRs cannot be shared by
   multiple slices without locking provided at the user level.
   The DRs controlled by a slice are determined by the bf_dma_info_s
   structure provided to bf_add_device. Any DR with a specified "depth"
   of 0 will be ignored by the DR initialization code in this slice.

   DR "views"

   A DR "view" is a cached copy of the head and tail pointers for a DR.
   The actual pointers are maintained on-chip and at the end of each DR
   in "pushed pointer" mode (which is the LLD default). Since reading
   either thru PCIe or from a non-cacheable DMA memory region is costly,
   in terms of time, we only update our cached copy when we need to
   and use the cached values at all other times.

   dr_publish_view updates the hardware with our cached pointer value.
   dr_update_view updates the cached pointer value from hardware.

*/

/* DR locks
 *
 * By default, the DMA interface is lock free.
 *
 * The DR interface can be used lock free iff the following
 * are true,
 *
 * 1. Only a single thread will push descriptors into a given DR, so no
 *    lock is required on the descriptor ring.
 * 2. Only a single thread will service a given DR, so no lock is
 *    required on the (cached) DR view.
 *
 * DR locking is supported via the run-time API, lld_dr_lock_required().
 * This API enables locking on a per DR basis.
 *
 * Two locks are supported per DR, one for the descriptor ring
 * (to prevent simulataneous access to a single descriptor entry)
 * and one for the cached DR "view" structure (to prevent
 * simultaneous calls to dr_update_view() from incorrectly updating
 * the cached device pointer in the view).
 *
 */
typedef enum {
  LLD_DR_LOCK_RING = 0,
  LLD_DR_LOCK_VIEW = 1,
} lld_dr_lock_t;

// fwd refs
static int lld_dr_lock(lld_dr_view_t *view, lld_dr_lock_t which);
static int lld_dr_unlock(lld_dr_view_t *view, lld_dr_lock_t which);
static int dr_empty(lld_dr_view_t *view);
static void lld_dr_pbus_arb_ctrl_set(lld_dev_t *dev_p,
                                     pbus_arb_ctrl_t *pbus_arb_ctrl);

lld_dr_basic_cfg_t lld_dr_cfg_default[] = {
    // lld_dr_fm_pkt_0,
    {FM, 0, 1},
    // lld_dr_fm_pkt_1,
    {FM, 0, 1},
    // lld_dr_fm_pkt_2,
    {FM, 0, 1},
    // lld_dr_fm_pkt_3,
    {FM, 0, 1},
    // lld_dr_fm_pkt_4,
    {FM, 0, 1},
    // lld_dr_fm_pkt_5,
    {FM, 0, 1},
    // lld_dr_fm_pkt_6,
    {FM, 0, 1},
    // lld_dr_fm_pkt_7,
    {FM, 0, 1},
    // lld_dr_fm_lrt,
    {FM, 0, 1},
    // lld_dr_fm_idle,
    {FM, 0, 1},
    // lld_dr_fm_learn_quanta,
    {FM, 0, 1},
    // lld_dr_fm_diag,
    {FM, 0, 1},
    // lld_dr_tx_pipe_inst_list_0,
    {TX, 0, 1},
    // lld_dr_tx_pipe_inst_list_1,
    {TX, 0, 1},
    // lld_dr_tx_pipe_inst_list_2,
    {TX, 0, 1},
    // lld_dr_tx_pipe_inst_list_3,
    {TX, 0, 1},
    // lld_dr_tx_pipe_write_block,
    {TX, 0, 1},
    // lld_dr_tx_pipe_read_block,
    {TX, 0, 1},
    // lld_dr_tx_que_write_list
    {TX, 0, 1},
    // lld_dr_tx_pkt_0,
    {TX, 0, 1},
    // lld_dr_tx_pkt_1,
    {TX, 0, 1},
    // lld_dr_tx_pkt_2,
    {TX, 0, 1},
    // lld_dr_tx_pkt_3,
    {TX, 0, 1},
    // lld_dr_tx_mac_stat,
    {TX, 0, 1},
    // lld_dr_rx_pkt_0,
    {RX, 0, 0},
    // lld_dr_rx_pkt_1,
    {RX, 0, 0},
    // lld_dr_rx_pkt_2,
    {RX, 0, 0},
    // lld_dr_rx_pkt_3,
    {RX, 0, 0},
    // lld_dr_rx_pkt_4,
    {RX, 0, 0},
    // lld_dr_rx_pkt_5,
    {RX, 0, 0},
    // lld_dr_rx_pkt_6,
    {RX, 0, 0},
    // lld_dr_rx_pkt_7,
    {RX, 0, 0},
    // lld_dr_rx_lrt,
    {RX, 0, 0},
    // lld_dr_rx_idle,
    {RX, 0, 0},
    // lld_dr_rx_learn,
    {RX, 0, 0},
    // lld_dr_rx_diag,
    {RX, 0, 0},
    // lld_dr_cmp_pipe_inst_list_0
    {CP, 0, 0},
    // lld_dr_cmp_pipe_inst_list_1
    {CP, 0, 0},
    // lld_dr_cmp_pipe_inst_list_2
    {CP, 0, 0},
    // lld_dr_cmp_pipe_inst_list_3
    {CP, 0, 0},
    // lld_dr_cmp_que_write_list,
    {CP, 0, 0},
    // lld_dr_cmp_pipe_write_blk,
    {CP, 0, 0},
    // lld_dr_cmp_pipe_read_blk,
    {CP, 0, 0},
    // lld_dr_cmp_mac_stat,
    {CP, 0, 0},
    // lld_dr_cmp_tx_pkt_0,
    {CP, 0, 0},
    // lld_dr_cmp_tx_pkt_1,
    {CP, 0, 0},
    // lld_dr_cmp_tx_pkt_2,
    {CP, 0, 0},
    // lld_dr_cmp_tx_pkt_3,
    {CP, 0, 0},
    // new added ones for tof2
    // lld_dr_tx_mac_write_block,
    {TX, 0, 1},
    // lld_dr_tx_que_write_list_1,
    {TX, 0, 1},
    // lld_dr_tx_que_read_block_0,
    {TX, 0, 1},
    // lld_dr_tx_que_read_block_1,
    {TX, 0, 1},
    // lld_dr_cmp_mac_write_block,
    {CP, 0, 0},
    // lld_dr_cmp_que_write_list_1,
    {CP, 0, 0},
    // lld_dr_cmp_que_read_block_0,
    {CP, 0, 0},
    // lld_dr_cmp_que_read_block_1,
    {CP, 0, 0},
};

static void lld_dr_get_dru_offsets(bf_dev_family_t dev_family,
                                   uint32_t *size,
                                   uint32_t *base_lo,
                                   uint32_t *base_hi,
                                   uint32_t *limit_lo,
                                   uint32_t *limit_hi,
                                   uint32_t *head,
                                   uint32_t *tail) {
  switch (dev_family) {
    case BF_DEV_FAMILY_TOFINO:
    case BF_DEV_FAMILY_TOFINO2:
    case BF_DEV_FAMILY_TOFINO3:

      /*  These chips use the same offsets. */
      *size = offsetof(Dru_rspec, size);
      *base_lo = offsetof(Dru_rspec, base_addr_low);
      *base_hi = offsetof(Dru_rspec, base_addr_high);
      *limit_lo = offsetof(Dru_rspec, limit_addr_low);
      *limit_hi = offsetof(Dru_rspec, limit_addr_high);
      *head = offsetof(Dru_rspec, head_ptr);
      *tail = offsetof(Dru_rspec, tail_ptr);
      return;
    case BF_DEV_FAMILY_UNKNOWN:
      break;
  }
  *size = 0, *base_lo = 0, *base_hi = 0, *limit_lo = 0, *limit_hi = 0,
  *head = 0, *tail = 0;
}

static inline bf_dma_dr_id_t lld_dr_max_dr(bf_dev_family_t dev_family) {
  switch (dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      return BF_DMA_MAX_TOF_DR;
    case BF_DEV_FAMILY_TOFINO2:
      return BF_DMA_MAX_TOF2_DR;
    case BF_DEV_FAMILY_TOFINO3:
      return BF_DMA_MAX_TOF3_DR;


    default:
      bf_sys_assert(0);
      return 0;
  }
  return 0;
}

/***********************************************************
 * lld_dr_get_max_dr
 *
 ***********************************************************/
bf_dma_dr_id_t lld_dr_get_max_dr(bf_dev_id_t dev_id, bf_subdev_id_t subdev_id) {
  lld_dev_t *dev_p = lld_map_subdev_id_to_dev_p(dev_id, subdev_id);
  if (!dev_p) return 0;
  return lld_dr_max_dr(dev_p->dev_family);
}

/***********************************************************
 * lld_dr_host_bus
 *
 ***********************************************************/
lld_dr_bus_t lld_dr_host_bus(bf_dev_family_t dev_family, bf_dma_dr_id_t dr_id) {
  switch (dev_family) {
    case BF_DEV_FAMILY_TOFINO:
    case BF_DEV_FAMILY_TOFINO2: /* TF1 and TF2 use the same mapping. */
      return lld_dr_tof_dr_to_host_bus(dr_id);
    case BF_DEV_FAMILY_TOFINO3:
      return lld_dr_tof3_dr_to_host_bus(dr_id);


    case BF_DEV_FAMILY_UNKNOWN:
      break;
  }
  return lld_dr_mbus;
}

/***********************************************************
 * lld_dr_cfg
 *
 ***********************************************************/
static lld_dr_basic_cfg_t lld_dr_cfg[BF_MAX_DEV_COUNT][BF_MAX_SUBDEV_COUNT]
                                    [BF_DMA_MAX_DR];

/***********************************************************
 * lld_dr_cfg
 *
 ***********************************************************/
lld_dr_basic_cfg_t *lld_dr_basic_cfg_get(bf_dev_id_t dev_id,
                                         bf_subdev_id_t subdev_id,
                                         bf_dma_dr_id_t dr_id) {
  bf_dma_dr_id_t dr_max = 0;
  lld_dev_t *dev_p = lld_map_subdev_id_to_dev_p(dev_id, subdev_id);
  if (!dev_p) return NULL;
  dr_max = lld_dr_max_dr(dev_p->dev_family);

  bf_sys_assert(dr_id >= 0 && dr_id <= dr_max);
  return &lld_dr_cfg[dev_id][subdev_id][dr_id];
}

/***********************************************************
 * lld_dr_words_per_desc
 *
 * Note: this function gets the DR type from the "default"
 * config. This is intentional. This fn is callable via
 * API and may be called prior to the device add (in
 * particular, bf_switchd calls this to set the amount
 * of DR mem in the dma-info struct).
 ***********************************************************/
int lld_dr_words_per_desc(bf_dev_family_t dev_fam, int dr_id) {
  lld_dr_basic_cfg_t *cfg;
  (void)dev_fam;
  bf_sys_assert(dr_id >= 0 && dr_id < BF_DMA_MAX_DR);
  cfg = &lld_dr_cfg_default[dr_id];
  return cfg->type == FM ? 1 : cfg->type == RX ? 2 : cfg->type == CP ? 2 : 4;
}

/***********************************************************
 * lld_validate_dr_id
 *
 * Returns 0 if dr_id is valid for the device, non-zero
 * otherwise.
 ***********************************************************/
int lld_validate_dr_id(bf_dev_id_t dev_id, bf_dma_dr_id_t dr_id) {
  lld_dev_t *dev_p = lld_map_dev_id_to_dev_p(dev_id);
  if (dev_p == NULL) return LLD_ERR_BAD_PARM;
  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      if (dr_id > BF_DMA_MAX_TOF_DR) return LLD_ERR_BAD_PARM;
      break;
    case BF_DEV_FAMILY_TOFINO2:
      if (dr_id > BF_DMA_MAX_TOF2_DR) return LLD_ERR_BAD_PARM;
      break;
    case BF_DEV_FAMILY_TOFINO3:
      if (dr_id > BF_DMA_MAX_TOF3_DR) return LLD_ERR_BAD_PARM;
      break;




    default:
      return LLD_ERR_BAD_PARM;
  }
  return 0;
}

/***********************************************************
 * lld_dr_cfg_this
 *
 * Set the depth of the specified DR in lld_dr_cfg. This fn
 * converts "number of entries" to "number of bytes", taking
 * into account the required 64B alignment.
 ***********************************************************/
static lld_err_t lld_dr_cfg_this(bf_dev_id_t dev_id,
                                 bf_subdev_id_t subdev_id,
                                 bf_dma_dr_id_t dr_id,
                                 int depth) {
  int this_dr_sz, n_words_per_desc, n_desc_bytes;
  lld_dr_basic_cfg_t *cfg;
  lld_dev_t *dev_p = lld_map_subdev_id_to_dev_p(dev_id, subdev_id);
  if (!dev_p) return LLD_ERR_BAD_PARM;

  if (lld_validate_dr_id(dev_id, dr_id)) return LLD_ERR_BAD_PARM;
  cfg = &lld_dr_cfg[dev_id][subdev_id][dr_id];

  // copy type and prod/consmr fields from default cfg
  cfg->type = lld_dr_cfg_default[dr_id].type;
  cfg->producer = lld_dr_cfg_default[dr_id].producer;

  n_words_per_desc = lld_dr_words_per_desc(dev_p->dev_family, dr_id);
  n_desc_bytes = (8 * n_words_per_desc);

  // max DR sz is 1MB
  if ((depth * n_desc_bytes) >= BF_DMA_MAX_DR_LEN) {
    return LLD_ERR_BAD_PARM;
  }
  this_dr_sz = depth * n_desc_bytes;

  // round up to (64B) cache-line boundary
  this_dr_sz = (this_dr_sz + 63) & ~(63);
  cfg->depth = this_dr_sz / n_desc_bytes;

  return LLD_OK;
}

/***********************************************************
 * lld_dr_cfg_depth_get
 *
 * returns the descriptor ring's configured depth
 * (primarily important to find out if the dr_id is not managed by this process)
 ***********************************************************/
int lld_dr_cfg_depth_get(bf_dev_id_t dev_id,
                         bf_subdev_id_t subdev_id,
                         bf_dma_dr_id_t dr_id,
                         int *depth) {
  if (!depth) return LLD_ERR_BAD_PARM;
  *depth = 0;
  if (!lld_dev_ready(dev_id, subdev_id))
    return LLD_ERR_NOT_READY;  // chip not ready to use
  if (lld_validate_dr_id(dev_id, dr_id)) return LLD_ERR_BAD_PARM;
  *depth = lld_dr_cfg[dev_id][subdev_id][dr_id].depth;
  return LLD_OK;
}

/***********************************************************
 * lld_dr_desc_area_sz
 *
 * Compute space required for all descriptor rings (DRs)
 * for the specified chip.
 ***********************************************************/
int lld_dr_desc_area_sz(bf_dev_id_t dev_id, bf_subdev_id_t subdev_id) {
  lld_dev_t *dev_p = lld_map_subdev_id_to_dev_p(dev_id, subdev_id);
  int sz = 0, this_dr_sz, n_words_per_desc;
  bf_dma_dr_id_t dr_id;
  bf_dma_dr_id_t dr_max = 0;
  if (dev_p == NULL) return LLD_ERR_BAD_PARM;

  dr_max = lld_dr_max_dr(dev_p->dev_family);

  for (dr_id = 0; dr_id <= dr_max; dr_id++) {
    lld_dr_basic_cfg_t *cfg = &lld_dr_cfg[dev_id][subdev_id][dr_id];

    n_words_per_desc = lld_dr_words_per_desc(dev_p->dev_family, dr_id);
    this_dr_sz = cfg->depth * (8 * n_words_per_desc);

    // special-case if DR unused by this LLD instance
    if (this_dr_sz == 0) {
      continue;
    }
    // round up to cache-line boundary
    this_dr_sz = (this_dr_sz + 63) & ~(63);

    // fix-up depth for use in dr_init
    cfg->depth = this_dr_sz / (8 * n_words_per_desc);

    // add in the extra cache-line used for the pushed ptr
    this_dr_sz += 64;

    sz += this_dr_sz;
  }
  return sz;
}

/***********************************************************
 * get_dr_pos
 *
 * Inline function to be used in order to get wrap bit and head or
 * tail pointers.
 *
 * Returns 0 on success.
 */
static inline int get_dr_pos(bf_dev_id_t dev_id,
                             uint32_t addr,
                             int *dr_wrap_bit_pos,
                             uint32_t *out_ptr,
                             uint32_t *wrap_bit) {
  lld_dev_t *dev_p = lld_map_dev_id_to_dev_p(dev_id);
  if (dev_p == NULL) return LLD_ERR_BAD_PARM;
  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      *dr_wrap_bit_pos = TOF_DR_WRAP_BIT_POSITION;
      *out_ptr = TOF_DR_PTR_PART(addr);
      *wrap_bit = TOF_DR_WRP_PART(addr);
      break;
    case BF_DEV_FAMILY_TOFINO2:
      *dr_wrap_bit_pos = TOF2_DR_WRAP_BIT_POSITION;
      *out_ptr = TOF2_DR_PTR_PART(addr);
      *wrap_bit = TOF2_DR_WRP_PART(addr);
      break;
    case BF_DEV_FAMILY_TOFINO3:
      *dr_wrap_bit_pos = TOF3_DR_WRAP_BIT_POSITION;
      *out_ptr = TOF3_DR_PTR_PART(addr);
      *wrap_bit = TOF3_DR_WRP_PART(addr);
      break;







    default:
      return LLD_ERR_BAD_PARM;
  }
  return LLD_OK;
}

/***********************************************************
 * dr_push_1
 *
 * Push 1x64b DR descriptor into DMA'able memory and update
 * the tail pointer in the DR view.
 *
 * It is possible the DMA'able memory will not be cacheable
 * so the descriptor is constructed before pushing. The
 * actual data movement is accomplished as efficiently as
 * possible, by aligned 64b load/stores.
 ***********************************************************/
int dr_push_1(struct lld_dr_view_s *view, uint64_t *desc) {
  int lock_failed;
  uint32_t tail_ptr, tail_wrap_bit;
  uint64_t volatile *dr_desc_addr;
  int dr_wrap_bit_pos;
  lock_failed = lld_dr_lock(view, LLD_DR_LOCK_RING);
  if (lock_failed) {
    return LLD_ERR_LOCK_FAILED;
  }
  if (dr_full(view)) {
    lld_dr_unlock(view, LLD_DR_LOCK_RING);
    return LLD_ERR_DR_FULL;
  }
  if (get_dr_pos(view->dev_id,
                 view->tail,
                 &dr_wrap_bit_pos,
                 &tail_ptr,
                 &tail_wrap_bit)) {
    return LLD_ERR_BAD_PARM;
  }
  dr_desc_addr = (uint64_t *)(uintptr_t)(
      view->base + (tail_ptr * view->n_words_per_desc * 8));

#if __BYTE_ORDER == __ORDER_LITTLE_ENDIAN__
  *(dr_desc_addr + 0) = *(desc + 0);
#else
  *(dr_desc_addr + 0) = htole64(*(desc + 0));
#endif

  if (tail_ptr == ((uint32_t)view->n_entries - 1)) {  // wrap
    tail_wrap_bit ^= 0x1;                             // toggle wrap bit
    view->tail = 0 | (tail_wrap_bit << dr_wrap_bit_pos);
  } else {
    view->tail = (view->tail + 1) | (tail_wrap_bit << dr_wrap_bit_pos);
  }

  lld_log_dma(0 /*push*/,
              view->dev_id,
              view->subdev_id,
              view->dr_id,
              desc,
              1,
              view->head,
              view->tail);
  view->n_descs++;
  lld_dr_unlock(view, LLD_DR_LOCK_RING);

  return LLD_OK;
}

/***********************************************************
 * dr_push_2
 *
 * Push 2x64b DR descriptor into DMA'able memory and update
 * the tail pointer in the DR view.
 *
 * It is possible the DMA'able memory will not be cacheable
 * so the descriptor is constructed before pushing. The
 * actual data movement is accomplished as efficiently as
 * possible, by aligned 64b load/stores.
 ***********************************************************/
int dr_push_2(struct lld_dr_view_s *view, uint64_t *desc) {
  int lock_failed;
  uint32_t tail_ptr, tail_wrap_bit;
  uint64_t volatile *dr_desc_addr;
  int dr_wrap_bit_pos;
  lock_failed = lld_dr_lock(view, LLD_DR_LOCK_RING);
  if (lock_failed) {
    return LLD_ERR_LOCK_FAILED;
  }
  if (dr_full(view)) {
    lld_dr_unlock(view, LLD_DR_LOCK_RING);
    return LLD_ERR_DR_FULL;
  }
  if (get_dr_pos(view->dev_id,
                 view->tail,
                 &dr_wrap_bit_pos,
                 &tail_ptr,
                 &tail_wrap_bit)) {
    return LLD_ERR_BAD_PARM;
  }
  dr_desc_addr = (uint64_t *)(uintptr_t)(
      view->base + (tail_ptr * view->n_words_per_desc * 8));
#if __BYTE_ORDER == __ORDER_LITTLE_ENDIAN__
  *(dr_desc_addr + 0) = *(desc + 0);
  *(dr_desc_addr + 1) = *(desc + 1);
#else
  *(dr_desc_addr + 0) = htole64(*(desc + 0));
  *(dr_desc_addr + 1) = htole64(*(desc + 1));
#endif

  if (tail_ptr == ((uint32_t)view->n_entries - 1)) {  // wrap
    tail_wrap_bit ^= 0x1;                             // toggle wrap bit
    view->tail = 0 | (tail_wrap_bit << dr_wrap_bit_pos);
  } else {
    view->tail = (view->tail + 1) | (tail_wrap_bit << dr_wrap_bit_pos);
  }

  lld_log_dma(0 /*push*/,
              view->dev_id,
              view->subdev_id,
              view->dr_id,
              desc,
              2,
              view->head,
              view->tail);
  view->n_descs++;
  lld_dr_unlock(view, LLD_DR_LOCK_RING);

  return LLD_OK;
}

/***********************************************************
 * dr_push_4
 *
 * Push 4x64b DR descriptor into DMA'able memory and update
 * the tail pointer in the DR view.
 *
 * It is possible the DMA'able memory will not be cacheable
 * so the descriptor is constructed before pushing. The
 * actual data movement is accomplished as efficiently as
 * possible, by aligned 64b load/stores.
 ***********************************************************/
int dr_push_4(struct lld_dr_view_s *view, uint64_t *desc) {
  int lock_failed;
  uint32_t tail_ptr, tail_wrap_bit;
  uint64_t volatile *dr_desc_addr;
  int dr_wrap_bit_pos;
  lock_failed = lld_dr_lock(view, LLD_DR_LOCK_RING);
  if (lock_failed) {
    return LLD_ERR_LOCK_FAILED;
  }
  if (dr_full(view)) {
    lld_dr_unlock(view, LLD_DR_LOCK_RING);
    return LLD_ERR_DR_FULL;
  }
  if (get_dr_pos(view->dev_id,
                 view->tail,
                 &dr_wrap_bit_pos,
                 &tail_ptr,
                 &tail_wrap_bit)) {
    return LLD_ERR_BAD_PARM;
  }
  dr_desc_addr = (uint64_t *)(uintptr_t)(
      view->base + (tail_ptr * view->n_words_per_desc * 8));

#if __BYTE_ORDER == __ORDER_LITTLE_ENDIAN__
  *(dr_desc_addr + 0) = *(desc + 0);
  *(dr_desc_addr + 1) = *(desc + 1);
  *(dr_desc_addr + 2) = *(desc + 2);
  *(dr_desc_addr + 3) = *(desc + 3);
#else
  *(dr_desc_addr + 0) = htole64(*(desc + 0));
  *(dr_desc_addr + 1) = htole64(*(desc + 1));
  *(dr_desc_addr + 2) = htole64(*(desc + 2));
  *(dr_desc_addr + 3) = htole64(*(desc + 3));
#endif

  if (tail_ptr == ((uint32_t)view->n_entries - 1)) {  // wrap
    tail_wrap_bit ^= 0x1;                             // toggle wrap bit
    view->tail = 0 | (tail_wrap_bit << dr_wrap_bit_pos);
  } else {
    view->tail = (view->tail + 1) | (tail_wrap_bit << dr_wrap_bit_pos);
  }

  lld_log_dma(0 /*push*/,
              view->dev_id,
              view->subdev_id,
              view->dr_id,
              desc,
              4,
              view->head,
              view->tail);
  view->n_descs++;
  lld_dr_unlock(view, LLD_DR_LOCK_RING);

  return LLD_OK;
}

/***********************************************************
 * dr_pull
 *
 * "Pull" a DR descriptor.
 * This function returns a copy of the descriptor from the DMA'able memory.
 * It will be be "consumed" by the DR callback function after this function
 * is called, so there is no risk that dr_publish_view() is called by another
 * thread and the descriptor contents become overwritten before they are
 * extracted from DMA'able memory.
 *
 * Update the head pointer in the DR view.
 ***********************************************************/
int dr_pull(struct lld_dr_view_s *view, dr_descr_value_t *desc_val) {
  int lock_failed;
  uint32_t head_ptr, head_wrap_bit;
  int dr_wrap_bit_pos;
  uint64_t *desc;
  int num_desc;
  (void)num_desc;

  lock_failed = lld_dr_lock(view, LLD_DR_LOCK_RING);
  if (lock_failed) {
    return LLD_ERR_LOCK_FAILED;
  }
  if (dr_empty(view)) {
    lld_dr_unlock(view, LLD_DR_LOCK_RING);
    return LLD_ERR_DR_EMPTY;
  }
  if (get_dr_pos(view->dev_id,
                 view->head,
                 &dr_wrap_bit_pos,
                 &head_ptr,
                 &head_wrap_bit)) {
    // should never get here not knowing what kind of chip we are
    bf_sys_assert(0);
    lld_dr_unlock(view, LLD_DR_LOCK_RING);
    return LLD_ERR_BAD_PARM;
  }
  desc = (uint64_t *)(uintptr_t)(view->base +
                                 (head_ptr * view->n_words_per_desc * 8));

#if __BYTE_ORDER == __ORDER_BIG_ENDIAN__
  for (num_desc = 0; num_desc < view->n_words_per_desc; num_desc++) {
    *(desc + num_desc) = le64toh(*(desc + num_desc));
  }
#endif

  if (head_ptr == ((uint32_t)view->n_entries - 1)) {  // wrap
    head_wrap_bit ^= 0x1;                             // toggle wrap bit
    view->head = 0 | (head_wrap_bit << dr_wrap_bit_pos);
  } else {
    view->head = (view->head + 1) | (head_wrap_bit << dr_wrap_bit_pos);
  }

  *desc_val = *((dr_descr_value_t *)desc);

  lld_log_dma(1 /*pull*/,
              view->dev_id,
              view->subdev_id,
              view->dr_id,
              desc,
              view->n_words_per_desc,
              view->head,
              view->tail);

  view->n_descs++;
  lld_dr_unlock(view, LLD_DR_LOCK_RING);
  return LLD_OK;
}

/** \brief dr_init:
 *
 *  Descriptor Ring Initialization.
 *
 *  +-----------+ ^     <-- base
 *  |           | |
 *  |           | |
 *  |    DR     | size
 *  |           | |
 *  |           | |
 *  +-----------+ v
 *   <pushed ptr>       <-- limit
 *
 *  "base" must be 64B (cache-line) aligned
 *  "size" must be a power-of-2 multiple of 64B (the cache-line size)
 *  "size" is used to determine the wrap point
 *  "limit" is the address used to store the "pushed pointer"
 *  So the amount of space required for a DR is 64B (one cache-line)
 *  larger than the "size" would indicate (for DRs in pushed-ptr mode,
 *  which is what we always use).
 *
 */
static void lld_dr_init(lld_dev_t *dev_p,
                        bf_dma_dr_id_t dr_id,
                        lld_dr_view_t *view,
                        uint64_t desc_area_v,
                        uint64_t desc_area_p) {
  bf_dev_id_t dev_id = dev_p->dev_id;
  bf_subdev_id_t subdev_id = dev_p->subdev_id;
  lld_dr_basic_cfg_t *cfg = &lld_dr_cfg[dev_id][subdev_id][dr_id];
  uint64_t base_v, base_p;
  uint64_t dru_base, dr_size;
  uint32_t off_sz, off_base_lo, off_base_hi, off_limit_lo, off_limit_hi,
      off_head, off_tail;
  uint32_t reg;

  view->n_words_per_desc = lld_dr_words_per_desc(dev_p->dev_family, dr_id);
  view->n_entries = cfg->depth;
  view->producer = cfg->producer;
  view->dr_id = dr_id;
  view->dev_id = dev_id;
  view->subdev_id = subdev_id;
  // view->lock_reqd = 0; Don't clear as lld_dr_lock_required may have set it.

#ifdef DONT_USE_PUSHED_PTR_MODE
  view->use_pushed_ptr_mode = false;
#else
  // set to push hd/tl ptr to memory
  view->use_pushed_ptr_mode = true;
#endif  // USE_PUSHED_PTR_MODE

  if (view->n_words_per_desc == 1) {
    view->push = dr_push_1;
  } else if (view->n_words_per_desc == 2) {
    view->push = dr_push_2;
  } else {
    view->push = dr_push_4;
  }

  base_v = (desc_area_v + 63ull) & ~(63ull);
  base_p = (desc_area_p + 63ull) & ~(63ull);
  view->base = base_v;  // view uses virtual address

  dru_base = lld_dr_base_get(dev_id, dr_id);
  /* Skip over DRs which do not apply to this chip type. */
  if (dru_base == 0) return;

  dr_size = (cfg->depth * view->n_words_per_desc * 8ull);

  /* disable DR first */
  lld_subdev_dr_enable_set(dev_id, subdev_id, dr_id, false);

  /* Configure DR size, base address, and limit.  Initialize head and tail
   * pointers both in the view and on the chip. */
  lld_dr_get_dru_offsets(dev_p->dev_family,
                         &off_sz,
                         &off_base_lo,
                         &off_base_hi,
                         &off_limit_lo,
                         &off_limit_hi,
                         &off_head,
                         &off_tail);
  reg = dru_base + off_sz;
  lld_subdev_write_register(dev_id, subdev_id, reg, (uint32_t)(dr_size));

  reg = dru_base + off_base_lo;
  lld_subdev_write_register(
      dev_id, subdev_id, reg, (uint32_t)(base_p & 0xFFFFFFFFULL));

  reg = dru_base + off_base_hi;
  lld_subdev_write_register(dev_id, subdev_id, reg, (uint32_t)(base_p >> 32));

  reg = dru_base + off_limit_lo;
  lld_subdev_write_register(
      dev_id, subdev_id, reg, (uint32_t)((base_p + dr_size) & 0xFFFFFFFFULL));

  reg = dru_base + off_limit_hi;
  lld_subdev_write_register(
      dev_id, subdev_id, reg, (uint32_t)((base_p + dr_size) >> 32));

  /* reset head/tail pointers (required for re-connecting to model) */
  view->head = 0;
  view->tail = 0;

  view->dr_head_reg = dru_base + off_head;
  lld_subdev_write_register(dev_id, subdev_id, view->dr_head_reg, 0);

  view->dr_tail_reg = dru_base + off_tail;
  lld_subdev_write_register(dev_id, subdev_id, view->dr_tail_reg, 0);

  // last cache line begin
  view->dev_pushed_ptr = (uint64_t *)(uintptr_t)(base_v + dr_size);
  *view->dev_pushed_ptr = 0;  // init to 0

  if (cfg->type == RX) {
    // set data timeout = 1
    lld_subdev_dr_data_timeout_set(dev_id, subdev_id, dr_id, 1);
  }

  // set to push hd/tl ptr to memory
  if (view->use_pushed_ptr_mode) {
    lld_dr_pushed_ptr_mode_set(dev_id, subdev_id, dr_id, true);
  }

  /* enable DR last */
  lld_subdev_dr_enable_set(dev_id, subdev_id, dr_id, true);
}

/***********************************************************************
 *  lld_dr_cfgd_depth_get
 *
 *  Return the # of descriptor entries specified in the bf_dma_dr_info_t
 *  structure.
 ***********************************************************************/
static int lld_dr_cfgd_depth_get(bf_dev_family_t dev_family,
                                 bf_dma_dr_id_t dr_id,
                                 bf_dma_dr_info_t *info) {
  bf_dma_type_t type;
  int dir;
  bf_dma_dr_id_t tx_s, tx_e, rx_s, rx_e;

  switch (dr_id) {
    case lld_dr_fm_pkt_0:
    case lld_dr_fm_pkt_1:
    case lld_dr_fm_pkt_2:
    case lld_dr_fm_pkt_3:
    case lld_dr_fm_pkt_4:
    case lld_dr_fm_pkt_5:
    case lld_dr_fm_pkt_6:
    case lld_dr_fm_pkt_7:
      type = BF_DMA_CPU_PKT_RECEIVE_0 + (dr_id - lld_dr_fm_pkt_0);
      dir = BF_DMA_DR_DIR_CPU2DEV;
      break;
    case lld_dr_fm_lrt:
      type = BF_DMA_PIPE_STAT_NOTIFY;
      dir = BF_DMA_DR_DIR_CPU2DEV;
      break;
    case lld_dr_fm_idle:
      type = BF_DMA_PIPE_IDLE_STATE_NOTIFY;
      dir = BF_DMA_DR_DIR_CPU2DEV;
      break;
    case lld_dr_fm_learn:
      type = BF_DMA_PIPE_LEARN_NOTIFY;
      dir = BF_DMA_DR_DIR_CPU2DEV;
      break;
    case lld_dr_fm_diag:
      type = BF_DMA_DIAG_ERR_NOTIFY;
      dir = BF_DMA_DR_DIR_CPU2DEV;
      break;
    case lld_dr_tx_pipe_inst_list_0:
    case lld_dr_tx_pipe_inst_list_1:
    case lld_dr_tx_pipe_inst_list_2:
    case lld_dr_tx_pipe_inst_list_3:
      type = BF_DMA_PIPE_INSTRUCTION_LIST;
      dir = BF_DMA_DR_DIR_CPU2DEV;
      break;
    case lld_dr_tx_pipe_write_block:
      type = BF_DMA_PIPE_BLOCK_WRITE;
      dir = BF_DMA_DR_DIR_CPU2DEV;
      break;
    case lld_dr_tx_pipe_read_block:
      type = BF_DMA_PIPE_BLOCK_READ;
      dir = BF_DMA_DR_DIR_CPU2DEV;
      break;
    case lld_dr_tx_que_write_list:
      type = BF_DMA_TM_WRITE_LIST;
      dir = BF_DMA_DR_DIR_CPU2DEV;
      break;
    case lld_dr_tx_pkt_0:
    case lld_dr_tx_pkt_1:
    case lld_dr_tx_pkt_2:
    case lld_dr_tx_pkt_3:
      type = BF_DMA_CPU_PKT_TRANSMIT_0 + (dr_id - lld_dr_tx_pkt_0);
      dir = BF_DMA_DR_DIR_CPU2DEV;
      break;
    case lld_dr_tx_mac_stat:
      type = BF_DMA_MAC_STAT_RECEIVE;
      dir = BF_DMA_DR_DIR_CPU2DEV;
      break;
    case lld_dr_rx_pkt_0:
    case lld_dr_rx_pkt_1:
    case lld_dr_rx_pkt_2:
    case lld_dr_rx_pkt_3:
    case lld_dr_rx_pkt_4:
    case lld_dr_rx_pkt_5:
    case lld_dr_rx_pkt_6:
    case lld_dr_rx_pkt_7:
      type = BF_DMA_CPU_PKT_RECEIVE_0 + (dr_id - lld_dr_rx_pkt_0);
      dir = BF_DMA_DR_DIR_DEV2CPU;
      break;
    case lld_dr_rx_lrt:
      type = BF_DMA_PIPE_STAT_NOTIFY;
      dir = BF_DMA_DR_DIR_DEV2CPU;
      break;
    case lld_dr_rx_idle:
      type = BF_DMA_PIPE_IDLE_STATE_NOTIFY;
      dir = BF_DMA_DR_DIR_DEV2CPU;
      break;
    case lld_dr_rx_learn:
      type = BF_DMA_PIPE_LEARN_NOTIFY;
      dir = BF_DMA_DR_DIR_DEV2CPU;
      break;
    case lld_dr_rx_diag:
      type = BF_DMA_DIAG_ERR_NOTIFY;
      dir = BF_DMA_DR_DIR_DEV2CPU;
      break;
    case lld_dr_cmp_pipe_inst_list_0:
    case lld_dr_cmp_pipe_inst_list_1:
    case lld_dr_cmp_pipe_inst_list_2:
    case lld_dr_cmp_pipe_inst_list_3:
      type = BF_DMA_PIPE_INSTRUCTION_LIST;
      dir = BF_DMA_DR_DIR_DEV2CPU;
      break;
    case lld_dr_cmp_que_write_list:
      type = BF_DMA_TM_WRITE_LIST;
      dir = BF_DMA_DR_DIR_DEV2CPU;
      break;
    case lld_dr_cmp_pipe_write_blk:
      type = BF_DMA_PIPE_BLOCK_WRITE;
      dir = BF_DMA_DR_DIR_DEV2CPU;
      break;
    case lld_dr_cmp_pipe_read_blk:
      type = BF_DMA_PIPE_BLOCK_READ;
      dir = BF_DMA_DR_DIR_DEV2CPU;
      break;
    case lld_dr_cmp_mac_stat:
      type = BF_DMA_MAC_STAT_RECEIVE;
      dir = BF_DMA_DR_DIR_DEV2CPU;
      break;
    case lld_dr_cmp_tx_pkt_0:
    case lld_dr_cmp_tx_pkt_1:
    case lld_dr_cmp_tx_pkt_2:
    case lld_dr_cmp_tx_pkt_3:
      type = BF_DMA_CPU_PKT_TRANSMIT_0 + (dr_id - lld_dr_cmp_tx_pkt_0);
      dir = BF_DMA_DR_DIR_DEV2CPU;
      break;
    case lld_dr_tx_mac_write_block:
      type = BF_DMA_MAC_BLOCK_WRITE;
      dir = BF_DMA_DR_DIR_CPU2DEV;
      break;
    case lld_dr_tx_que_write_list_1:
      type = BF_DMA_TM_WRITE_LIST_1;
      dir = BF_DMA_DR_DIR_CPU2DEV;
      break;
    case lld_dr_tx_que_read_block_0:
      type = BF_DMA_TM_BLOCK_READ_0;
      dir = BF_DMA_DR_DIR_CPU2DEV;
      break;
    case lld_dr_tx_que_read_block_1:
      type = BF_DMA_TM_BLOCK_READ_1;
      dir = BF_DMA_DR_DIR_CPU2DEV;
      break;
    case lld_dr_cmp_mac_write_block:
      type = BF_DMA_MAC_BLOCK_WRITE;
      dir = BF_DMA_DR_DIR_DEV2CPU;
      break;
    case lld_dr_cmp_que_write_list_1:
      type = BF_DMA_TM_WRITE_LIST_1;
      dir = BF_DMA_DR_DIR_DEV2CPU;
      break;
    case lld_dr_cmp_que_read_block_0:
      type = BF_DMA_TM_BLOCK_READ_0;
      dir = BF_DMA_DR_DIR_DEV2CPU;
      break;
    case lld_dr_cmp_que_read_block_1:
      type = BF_DMA_TM_BLOCK_READ_1;
      dir = BF_DMA_DR_DIR_DEV2CPU;
      break;
    default:
      bf_sys_assert(0);
      return 0;
  }
  /* Make sure the DR is valid for the device. */
  lld_dr_map_dma_type_to_dr_id(dev_family, type, &tx_s, &tx_e, &rx_s, &rx_e);
  if ((dr_id >= tx_s && dr_id <= tx_e) || (dr_id >= rx_s && dr_id <= rx_e))
    return info[type].dma_dr_entry_count[dir];
  return 0;
}

/********************************************************************
 *  lld_dr_construct_bus_map
 *
 *  Build a bit-map of which busses are associated with the DRs being
 *  initialized by the bf_dma_info_s structure.
 *
 *  This bit-map is used to control which portion(s) of the DR flush
 *  sequence to execute in thei LLD slice.
 *********************************************************************/
lld_dr_bus_map_t lld_dr_construct_bus_map(bf_dev_id_t dev_id,
                                          bf_subdev_id_t subdev_id,
                                          struct bf_dma_info_s *dma_info) {
  lld_dev_t *dev_p =
      lld_map_subdev_id_to_dev_p_allow_unassigned(dev_id, subdev_id);
  bf_dma_dr_id_t dr_id;
  int depth;
  lld_dr_bus_map_t bus_map = 0x0;
  bf_dma_dr_id_t dr_max = 0;

  bf_sys_assert(dma_info != NULL);
  bf_sys_assert(dev_p != NULL);

  dr_max = lld_dr_max_dr(dev_p->dev_family);
  for (dr_id = 0; dr_id <= dr_max; dr_id++) {
    depth =
        lld_dr_cfgd_depth_get(dev_p->dev_family, dr_id, dma_info->dma_dr_info);
    if (depth != 0) {  // if DR included in this "slice"
      lld_dr_bus_t bus = lld_dr_host_bus(dev_p->dev_family, dr_id);
      bus_map |= (1 << bus);
    }
  }
  return bus_map;
}

#ifndef __KERNEL__
static inline void get_buf_for_dr(bf_sys_dma_pool_handle_t pool_hdl,
                                  int buf_sz,
                                  uint64_t *va,
                                  uint64_t *pa,
                                  lld_dr_view_t *view) {
  void *dr_v_addr = NULL;
  bf_phys_addr_t dr_p_addr = 0;
  int r = bf_sys_dma_alloc(pool_hdl, buf_sz, &dr_v_addr, &dr_p_addr);
  if (r == 0) {
    *va = (intptr_t)dr_v_addr;
    *pa = dr_p_addr;
    view->dr_buffer = dr_v_addr;
  } else {
    *va = *pa = 0;
    view->dr_buffer = NULL;
    bf_sys_assert(r == 0);
  }
}
#endif
/************************************************************************
 * \brief lld_dr_init_dev
 *         Initialize DR hardware and DR views for this LLD slice
 *         based on the bf_dma_info_s structure.
 *
 * The buses associated with the DRs being initialized are also
 * programmed (if necessary).
 ************************************************************************/
void lld_dr_init_dev(bf_dev_id_t dev_id,
                     bf_subdev_id_t subdev_id,
                     struct bf_dma_info_s *dma_info) {
  bf_dma_dr_id_t dr_id;
  lld_dev_t *dev_p =
      lld_map_subdev_id_to_dev_p_allow_unassigned(dev_id, subdev_id);
  pbus_arb_ctrl_t pbus_arb_ctrl = {0};
  lld_dr_bus_map_t bus_map;
  bf_dma_dr_id_t dr_max = 0;
  uint32_t cbus_arb_ctrl = 0;

  bf_sys_assert(dma_info != NULL);
  bf_sys_assert(dev_p != NULL);

  /* determine which busses have DRs we are initializing */
  bus_map = lld_dr_construct_bus_map(dev_id, subdev_id, dma_info);

  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      dr_max = BF_DMA_MAX_TOF_DR;
      cbus_arb_ctrl = 0x00001111;
      pbus_arb_ctrl.ctrl_0 = PBUS_ARB_TOF_CTRL_0_DEF;
      /* Disable notifications from the PBus stations to speed up the config
       * download.  Pipe_mgr will reset the weights to default after init. */
      pbus_arb_ctrl.ctrl_1[0] = PBUS_ARB_TOF_CTRL_1_DEF;
      pbus_arb_ctrl.ctrl_1[1] = PBUS_ARB_TOF_CTRL_1_DEF;
      pbus_arb_ctrl.ctrl_1[2] = PBUS_ARB_TOF_CTRL_1_DEF;
      pbus_arb_ctrl.ctrl_1[3] = PBUS_ARB_TOF_CTRL_1_DEF;
      break;
    case BF_DEV_FAMILY_TOFINO2:
      dr_max = BF_DMA_MAX_TOF2_DR;
      /* TF2LAB-139 - Set host response weight to 15. */
      cbus_arb_ctrl = 0xF1111111;
      pbus_arb_ctrl.ctrl_0 = PBUS_ARB_TOF2_CTRL_0_DEF;
      /* These non-default weights (pipe 0 has higher host response) give
       * better PBus utilization and are used for init.  After init pipe_mgr
       * will reprogram default values. */
      pbus_arb_ctrl.ctrl_1[0] = PBUS_ARB_TOF2_CTRL_1_DEF_A;
      pbus_arb_ctrl.ctrl_1[1] = PBUS_ARB_TOF2_CTRL_1_DEF_B;
      pbus_arb_ctrl.ctrl_1[2] = PBUS_ARB_TOF2_CTRL_1_DEF_B;
      pbus_arb_ctrl.ctrl_1[3] = PBUS_ARB_TOF2_CTRL_1_DEF_B;
      break;
    case BF_DEV_FAMILY_TOFINO3:
      dr_max = BF_DMA_MAX_TOF3_DR;
      cbus_arb_ctrl = 0x11111111;
      /* Disable notifications from the PBus stations to speed up the config
       * download.  Pipe_mgr will reset the weights to default after init. */
      pbus_arb_ctrl.ctrl_0 = PBUS_ARB_TOF3_CTRL_0_DEF;
      pbus_arb_ctrl.ctrl_1[0] = PBUS_ARB_CTRL_1_DEF_B;
      pbus_arb_ctrl.ctrl_1[1] = PBUS_ARB_CTRL_1_DEF_B;
      pbus_arb_ctrl.ctrl_1[2] = PBUS_ARB_CTRL_1_DEF_B;
      pbus_arb_ctrl.ctrl_1[3] = PBUS_ARB_CTRL_1_DEF_B;
      break;









    case BF_DEV_FAMILY_UNKNOWN:
    default:
      bf_sys_assert(0);
      break;
  }
  /* Apply the requested DR configuration. */
  for (dr_id = 0; dr_id <= dr_max; dr_id++) {
    int rc, depth = lld_dr_cfgd_depth_get(
                dev_p->dev_family, dr_id, dma_info->dma_dr_info);
    rc = lld_dr_cfg_this(dev_id, subdev_id, dr_id, depth);
    bf_sys_assert(rc == LLD_OK);
  }

#ifndef __KERNEL__
  unsigned int dr_buf_sz = dma_info->dma_dr_buf_size;
  /* Initialize offset to ensure we perform an alloc for the first DR. */
  uint64_t offset = dr_buf_sz;
  uint64_t va = 0, pa = 0;
  for (dr_id = 0; dr_id <= dr_max; dr_id++) {
    lld_dr_view_t *view;
    lld_dr_bus_t bus = lld_dr_host_bus(dev_p->dev_family, dr_id);
    if (!bus_in_map(bus, bus_map)) continue;

    view = lld_map_subdev_id_and_dr_to_view_allow_unassigned(
        dev_id, subdev_id, dr_id);
    bf_sys_assert(view != NULL);

    /* If the DR memory is not allocated then allocate it now. */
    if (!view->base) {
      unsigned int cur_dr_sz, n_words_per_desc;
      /* Get the size, in bytes, of this DR. */
      n_words_per_desc = lld_dr_words_per_desc(dev_p->dev_family, dr_id);
      cur_dr_sz =
          lld_dr_cfg[dev_id][subdev_id][dr_id].depth * (8 * n_words_per_desc) +
          64;

      /* If this DR won't fit in the current buffer get a new one. */
      if ((offset + cur_dr_sz) > dr_buf_sz) {
        get_buf_for_dr(dma_info->dma_dr_pool_handle, dr_buf_sz, &va, &pa, view);
        offset = 0;
      }
      lld_dr_init(dev_p, dr_id, view, va + offset, pa + offset);
      offset += cur_dr_sz;
    } else {
      /* DMA memory was already allocated so just get the physical address
       * again. */
      void *vaddr = (uint8_t *)0 + view->base;
      int x = bf_sys_dma_get_phy_addr_from_pool(
          dma_info->dma_dr_pool_handle, vaddr, &pa);
      if (x != 0) {
        lld_log("Dev %d DR %d could not get physical address from virtual %p",
                dev_id,
                dr_id,
                vaddr);
        bf_sys_assert(x == 0);
      }
      lld_dr_init(dev_p, dr_id, view, view->base, pa);
    }
  }
#else
  {
    unsigned int n_words_per_desc;
    /* Save the virtual and physical address of the DR memory. */
    uint64_t desc_area_v = (long)dma_info->dr_mem_vaddr;
    uint64_t desc_area_p = dma_info->dr_mem_dma_addr;
    uint64_t offset = 0;
    for (dr_id = 0; dr_id <= dr_max; dr_id++) {
      lld_dr_view_t *view = NULL;
      lld_dr_bus_t bus = lld_dr_host_bus(dev_p->dev_family, dr_id);
      if (!bus_in_map(bus, bus_map)) continue;
      view = lld_map_subdev_id_and_dr_to_view_allow_unassigned(
          dev_id, subdev_id, dr_id);

      if (!view) continue;

      lld_dr_init(
          dev_p, dr_id, view, desc_area_v + offset, desc_area_p + offset);

      n_words_per_desc = lld_dr_words_per_desc(dev_p->dev_family, dr_id);
      offset +=
          (lld_dr_cfg[dev_id][subdev_id][dr_id].depth * (8 * n_words_per_desc) +
           64);
    }
  }
#endif

  if (pbus_in_map(bus_map)) {
    /* Initial configuration; anything else needed will be enabled by pipe_mgr
     * later. */
    lld_dr_pbus_arb_ctrl_set(dev_p, &pbus_arb_ctrl);
  }
  if (cbus_in_map(bus_map)) {
    lld_dr_cbus_arb_ctrl_set(dev_id, subdev_id, cbus_arb_ctrl);
  }
}

/*********************************************************************
 * \brief dr_update_view:
 * Update our cached copy of the head/tail pointer
 *
 * \param view, lld_dr_view_t
 *********************************************************************/
void dr_update_view(lld_dr_view_t *view) {
  uint64_t new_ptr;
  uint32_t u32_ptr, u32_ptr_wout_wrap_bit, u32_wrap_bit;

  lld_dr_lock(view, LLD_DR_LOCK_VIEW);

  if (view->use_pushed_ptr_mode) {
    new_ptr = (*view->dev_pushed_ptr);
#if __BYTE_ORDER == __ORDER_BIG_ENDIAN__
    new_ptr = le64toh(new_ptr);
#endif
    u32_ptr = (uint32_t)new_ptr;
  } else {
    if (view->producer) {  // LLD is producer
      lld_subdev_read_register(
          view->dev_id, view->subdev_id, view->dr_head_reg, &u32_ptr);
    } else {
      lld_subdev_read_register(
          view->dev_id, view->subdev_id, view->dr_tail_reg, &u32_ptr);
    }
  }
  u32_ptr_wout_wrap_bit = TOF_DR_PTR_PART(u32_ptr);
  u32_wrap_bit = TOF_DR_WRP_PART(u32_ptr);
  if (view->producer) {  // LLD is producer
    view->head = (u32_ptr_wout_wrap_bit / (view->n_words_per_desc * 8)) |
                 (u32_wrap_bit << TOF_DR_WRAP_BIT_POSITION);
  } else {
    view->tail = (u32_ptr_wout_wrap_bit / (view->n_words_per_desc * 8)) |
                 (u32_wrap_bit << TOF_DR_WRAP_BIT_POSITION);
  }
  lld_dr_unlock(view, LLD_DR_LOCK_VIEW);
}

/********************************************************************
 * \brief dr_publish_view
 * Update Tofinos head/tail pointer register from our view.
 *
 * \param view, lld_dr_view_t
 *********************************************************************/
void dr_publish_view(lld_dr_view_t *view) {
  uint32_t new_ptr, old_ptr;
  uint32_t wrap_bit;

  lld_dr_lock(view, LLD_DR_LOCK_VIEW);
  if (view->producer) {
    old_ptr = TOF_DR_PTR_PART(view->tail);
    wrap_bit = TOF_DR_WRP_PART(view->tail);

    new_ptr = (uint32_t)(old_ptr * view->n_words_per_desc * 8);
    new_ptr |= (wrap_bit << TOF_DR_WRAP_BIT_POSITION);
    lld_subdev_write_register(view->dev_id,
                              view->subdev_id,
                              lld_ptr_to_u64(view->dr_tail_reg),
                              new_ptr);  // for simulation
  } else {
    old_ptr = TOF_DR_PTR_PART(view->head);
    wrap_bit = TOF_DR_WRP_PART(view->head);

    new_ptr = (uint32_t)(old_ptr * view->n_words_per_desc * 8);
    new_ptr |= (wrap_bit << TOF_DR_WRAP_BIT_POSITION);
    lld_subdev_write_register(view->dev_id,
                              view->subdev_id,
                              lld_ptr_to_u64(view->dr_head_reg),
                              new_ptr);  // for simulation
  }
  view->last_pointer_written = new_ptr & 0xfffff;  // mask off wrap_bit
  lld_dr_unlock(view, LLD_DR_LOCK_VIEW);
}

/********************************************************************
 * \brief dr_empty
 *        Return non-0 if DR has no used descriptor entries
 ********************************************************************/
int dr_empty(lld_dr_view_t *view) { return (view->head == view->tail); }

/********************************************************************
 * \brief dr_full
 *        Return non-0 if DR has no unused descriptor entries
 ********************************************************************/
int dr_full(lld_dr_view_t *view) {
  uint32_t head_ptr, tail_ptr;
  uint32_t head_wrap_bit, tail_wrap_bit;

  head_ptr = TOF_DR_PTR_PART(view->head);
  tail_ptr = TOF_DR_PTR_PART(view->tail);
  head_wrap_bit = TOF_DR_WRP_PART(view->head);
  tail_wrap_bit = TOF_DR_WRP_PART(view->tail);

  // full is defined as,
  //
  // dr_full = (dr_head_ptr[19:0] == dr_tail_ptr[19:0]) &
  //           ~(dr_head_ptr[20] == hr_tail_ptr[20])
  //
  if ((head_ptr == tail_ptr) && (head_wrap_bit != tail_wrap_bit)) {
    return 1;  // DR full
  }
  return 0;
}

/********************************************************************
 * \brief dr_used
 *        Return the number of used entries in the DR
 ********************************************************************/
int dr_used(lld_dr_view_t *view) {
  uint32_t head_ptr, tail_ptr;
  uint32_t head_wrap_bit, tail_wrap_bit;

  head_ptr = TOF_DR_PTR_PART(view->head);
  tail_ptr = TOF_DR_PTR_PART(view->tail);
  head_wrap_bit = TOF_DR_WRP_PART(view->head);
  tail_wrap_bit = TOF_DR_WRP_PART(view->tail);

  if (head_ptr == tail_ptr) {
    if (head_wrap_bit == tail_wrap_bit) {  // empty
      return 0;
    } else {  // full
      return view->n_entries;
    }
  } else if (tail_ptr > head_ptr) {
    return (tail_ptr - head_ptr);
  } else {
    return (tail_ptr + view->n_entries - head_ptr);
  }
}

/********************************************************************
 * \brief dr_space
 *        Return the number of empty entries remaining in the DR
 ********************************************************************/
int dr_space(lld_dr_view_t *view) { return (view->n_entries - dr_used(view)); }

/********************************************************************
 * \brief dr_depth
 *        Return the total number of entries in the DR
 ********************************************************************/
int dr_depth(lld_dr_view_t *view) { return (view->n_entries); }

/************************************************************
 * lld_dr_lock
 *
 ************************************************************/
static int lld_dr_lock(lld_dr_view_t *view, lld_dr_lock_t which) {
  if (view->lock_reqd) {
    return bf_sys_mutex_lock(&view->mtx[which]);
  }
  return 0;
}

/************************************************************
 * lld_dr_unlock
 *
 ************************************************************/
static int lld_dr_unlock(lld_dr_view_t *view, lld_dr_lock_t which) {
  if (view->lock_reqd) {
    return bf_sys_mutex_unlock(&view->mtx[which]);
  }
  return 0;
}

/************************************************************
 * lld_dr_pbus_arb_ctrl_set
 *
 ************************************************************/
static void lld_dr_pbus_arb_ctrl_set(lld_dev_t *dev_p,
                                     pbus_arb_ctrl_t *pbus_arb_ctrl) {
  bf_dev_id_t dev_id = dev_p->dev_id;
  bf_subdev_id_t subdev_id = dev_p->subdev_id;
  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      lld_dr_tof_pbus_arb_ctrl_set(dev_id, pbus_arb_ctrl);
      break;
    case BF_DEV_FAMILY_TOFINO2:
      lld_dr_tof2_pbus_arb_ctrl_set(dev_id, pbus_arb_ctrl);
      break;
    case BF_DEV_FAMILY_TOFINO3:
      lld_dr_tof3_pbus_arb_ctrl_set(dev_id, subdev_id, pbus_arb_ctrl);
      break;



    case BF_DEV_FAMILY_UNKNOWN:
      break;
  }
}

/************************************************************
 * lld_dr_cbus_arb_ctrl_set
 *
 ************************************************************/
void lld_dr_cbus_arb_ctrl_set(bf_dev_id_t dev_id,
                              bf_subdev_id_t subdev_id,
                              uint32_t cbus_arb_ctrl_val) {
  lld_dev_t *dev_p = lld_map_dev_id_to_dev_p(dev_id);
  if (dev_p == NULL) return;
  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      lld_dr_tof_cbus_arb_ctrl_set(dev_id, cbus_arb_ctrl_val);
      return;
    case BF_DEV_FAMILY_TOFINO2:
      lld_dr_tof2_cbus_arb_ctrl_set(dev_id, cbus_arb_ctrl_val);
      return;
    case BF_DEV_FAMILY_TOFINO3:
      lld_dr_tof3_cbus_arb_ctrl_set(dev_id, subdev_id, cbus_arb_ctrl_val);
      return;



    case BF_DEV_FAMILY_UNKNOWN:
      return;
  }
}

/************************************************************
 * lld_dr_enable_set
 *
 ************************************************************/
void lld_dr_enable_set(bf_dev_id_t dev_id, bf_dma_dr_id_t dr_id, bool en) {
  if (lld_dev_is_tofino(dev_id)) {
    lld_dr_tof_enable_set(dev_id, dr_id, en);
  } else if (lld_dev_is_tof2(dev_id)) {
    lld_dr_tof2_enable_set(dev_id, dr_id, en);
  } else if (lld_dev_is_tof3(dev_id)) {
    lld_dr_tof3_enable_set(dev_id, 0, dr_id, en);
  }
}

/************************************************************
 * lld_subdev_dr_enable_set
 *
 ************************************************************/
void lld_subdev_dr_enable_set(bf_dev_id_t dev_id,
                              bf_subdev_id_t subdev_id,
                              bf_dma_dr_id_t dr_id,
                              bool en) {
  lld_dev_t *dev_p = lld_map_dev_id_to_dev_p(dev_id);
  if (dev_p == NULL) return;
  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      lld_dr_tof_enable_set(dev_id, dr_id, en);
      return;
    case BF_DEV_FAMILY_TOFINO2:
      lld_dr_tof2_enable_set(dev_id, dr_id, en);
      return;
    case BF_DEV_FAMILY_TOFINO3:
      lld_dr_tof3_enable_set(dev_id, subdev_id, dr_id, en);
      return;



    default:
      break;
  }
}

/************************************************************
 * lld_dr_set_write_time_mode
 *
 ************************************************************/
bf_status_t lld_dr_set_write_time_mode(bf_dev_id_t dev_id,
                                       bf_subdev_id_t subdev_id,
                                       bf_dma_dr_id_t dr_id,
                                       bool en) {
  lld_dev_t *dev_p = lld_map_dev_id_to_dev_p(dev_id);
  if (dev_p == NULL) return BF_INVALID_ARG;
  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      return lld_dr_tof_write_time_mode_set(dev_id, dr_id, en);
    case BF_DEV_FAMILY_TOFINO2:
      return lld_dr_tof2_write_time_mode_set(dev_id, dr_id, en);
    case BF_DEV_FAMILY_TOFINO3:
      return lld_dr_tof3_write_time_mode_set(dev_id, subdev_id, dr_id, en);



    case BF_DEV_FAMILY_UNKNOWN:
      break;
  }
  return BF_UNEXPECTED;
}

/************************************************************
 * lld_dr_pushed_ptr_mode_set
 *
 ************************************************************/
bf_status_t lld_dr_pushed_ptr_mode_set(bf_dev_id_t dev_id,
                                       bf_subdev_id_t subdev_id,
                                       bf_dma_dr_id_t dr_id,
                                       bool en) {
  lld_dev_t *dev_p = lld_map_dev_id_to_dev_p(dev_id);
  if (dev_p == NULL) return BF_INVALID_ARG;
  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      return lld_dr_tof_pushed_ptr_mode_set(dev_id, dr_id, en);
    case BF_DEV_FAMILY_TOFINO2:
      return lld_dr_tof2_pushed_ptr_mode_set(dev_id, dr_id, en);
    case BF_DEV_FAMILY_TOFINO3:
      return lld_dr_tof3_pushed_ptr_mode_set(dev_id, subdev_id, dr_id, en);



    case BF_DEV_FAMILY_UNKNOWN:
      break;
  }
  return BF_UNEXPECTED;
}

/************************************************************
 * lld_dr_ring_timeout_set
 *
 ************************************************************/
void lld_dr_ring_timeout_set(bf_dev_id_t dev_id,
                             bf_subdev_id_t subdev_id,
                             bf_dma_dr_id_t dr_id,
                             uint16_t timeout) {
  lld_dev_t *dev_p = lld_map_dev_id_to_dev_p(dev_id);
  if (dev_p == NULL) return;
  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      lld_dr_tof_ring_timeout_set(dev_id, dr_id, timeout);
      return;
    case BF_DEV_FAMILY_TOFINO2:
      lld_dr_tof2_ring_timeout_set(dev_id, dr_id, timeout);
      return;
    case BF_DEV_FAMILY_TOFINO3:
      lld_dr_tof3_ring_timeout_set(dev_id, subdev_id, dr_id, timeout);
      return;



    case BF_DEV_FAMILY_UNKNOWN:
      return;
  }
}

/************************************************************
 * lld_dr_data_timeout_set
 *
 ************************************************************/
void lld_dr_data_timeout_set(bf_dev_id_t dev_id,
                             bf_dma_dr_id_t dr_id,
                             uint32_t timeout) {
  lld_subdev_dr_data_timeout_set(dev_id, 0, dr_id, timeout);
}

/************************************************************
 * lld_dr_data_timeout_set
 *
 ************************************************************/
void lld_subdev_dr_data_timeout_set(bf_dev_id_t dev_id,
                                    bf_subdev_id_t subdev_id,
                                    bf_dma_dr_id_t dr_id,
                                    uint32_t timeout) {
  lld_dev_t *dev_p = lld_map_dev_id_to_dev_p(dev_id);
  if (dev_p == NULL) return;
  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      lld_dr_tof_data_timeout_set(dev_id, dr_id, timeout);
      return;
    case BF_DEV_FAMILY_TOFINO2:
      lld_dr_tof2_data_timeout_set(dev_id, dr_id, timeout);
      return;
    case BF_DEV_FAMILY_TOFINO3:
      lld_dr_tof3_data_timeout_set(dev_id, subdev_id, dr_id, timeout);
      return;



    case BF_DEV_FAMILY_UNKNOWN:
      return;
  }
}

/************************************************************
 * lld_subdev_dr_data_timeout_get
 *
 ************************************************************/
bf_status_t lld_subdev_dr_data_timeout_get(bf_dev_id_t dev_id,
                                           bf_subdev_id_t subdev_id,
                                           bf_dma_dr_id_t dr_id,
                                           uint32_t *timeout) {
  lld_dev_t *dev_p = lld_map_subdev_id_to_dev_p(dev_id, subdev_id);
  if (!dev_p) return BF_INVALID_ARG;
  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      return lld_dr_tof_data_timeout_get(dev_id, dr_id, timeout);
    case BF_DEV_FAMILY_TOFINO2:
      return lld_dr_tof2_data_timeout_get(dev_id, dr_id, timeout);
    case BF_DEV_FAMILY_TOFINO3:
      return lld_dr_tof3_data_timeout_get(dev_id, subdev_id, dr_id, timeout);



    case BF_DEV_FAMILY_UNKNOWN:
      break;
  }
  *timeout = 0;
  return BF_UNEXPECTED;
}

/************************************************************
 * lld_subdev_dr_disable_all
 *
 ************************************************************/
static void lld_subdev_dr_disable_all(const lld_dev_t *dev_p) {
  uint32_t dr_id;
  uint32_t dr_max = 0;

  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      dr_max = BF_DMA_MAX_TOF_DR;
      break;
    case BF_DEV_FAMILY_TOFINO2:
      dr_max = BF_DMA_MAX_TOF2_DR;
      break;
    case BF_DEV_FAMILY_TOFINO3:
      dr_max = BF_DMA_MAX_TOF3_DR;
      break;



    default:
      return;
  }
  /* disable all DRs */
  for (dr_id = 0; dr_id <= dr_max; dr_id++) {
    lld_subdev_dr_enable_set(dev_p->dev_id, dev_p->subdev_id, dr_id, false);
  }
}

/************************************************************
 * lld_dr_dma_enable_set
 *
 ************************************************************/
static void lld_dr_dma_enable_set(const lld_dev_t *dev_p, bool en_dma) {
  bf_dev_id_t dev_id = dev_p->dev_id;
  bf_subdev_id_t subdev_id = dev_p->subdev_id;

  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      lld_dr_tof_dma_enable_set(dev_id, en_dma);
      break;
    case BF_DEV_FAMILY_TOFINO2:
      lld_dr_tof2_dma_enable_set(dev_id, en_dma);
      break;
    case BF_DEV_FAMILY_TOFINO3:
      lld_dr_tof3_dma_enable_set(dev_id, subdev_id, en_dma);
      break;



    case BF_DEV_FAMILY_UNKNOWN:
      break;
  }
}

/************************************************************
 * lld_subdev_dr_flush_all
 *
 ************************************************************/
static void lld_subdev_dr_flush_all(const lld_dev_t *dev_p) {
  bf_dev_id_t dev_id = dev_p->dev_id;
  bf_subdev_id_t subdev_id = dev_p->subdev_id;

  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      lld_dr_tof_flush_all(dev_id);
      break;
    case BF_DEV_FAMILY_TOFINO2:
      lld_dr_tof2_flush_all(dev_id);
      break;
    case BF_DEV_FAMILY_TOFINO3:
      lld_dr_tof3_flush_all(dev_id, subdev_id);
      break;



    case BF_DEV_FAMILY_UNKNOWN:
      break;
  }
}

/************************************************************
 * lld_subdev_dr_wait_for_flush_done
 *
 ************************************************************/
static uint32_t lld_subdev_dr_wait_for_flush_done(const lld_dev_t *dev_p,
                                                  uint32_t max_tries) {
  bf_dev_id_t dev_id = dev_p->dev_id;
  bf_subdev_id_t subdev_id = dev_p->subdev_id;

  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      return lld_dr_tof_wait_for_flush_done(dev_id, max_tries);
    case BF_DEV_FAMILY_TOFINO2:
      return lld_dr_tof2_wait_for_flush_done(dev_id, max_tries);
    case BF_DEV_FAMILY_TOFINO3:
      return lld_dr_tof3_wait_for_flush_done(dev_id, subdev_id, max_tries);



    case BF_DEV_FAMILY_UNKNOWN:
      break;
  }
  return 0;
}

/************************************************************
 * lld_subdebv_dr_clear_link_down
 *
 ************************************************************/
static void lld_subdev_dr_clear_link_down(const lld_dev_t *dev_p) {
  bf_dev_id_t dev_id = dev_p->dev_id;
  bf_subdev_id_t subdev_id = dev_p->subdev_id;

  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      return;
    case BF_DEV_FAMILY_TOFINO2:
      lld_dr_tof2_clear_link_down(dev_id);
      return;
    case BF_DEV_FAMILY_TOFINO3:
      lld_dr_tof3_clear_link_down(dev_id, subdev_id);
      return;



    case BF_DEV_FAMILY_UNKNOWN:
      break;
  }
}

/************************************************************
 * lld_subdev_dr_run_flush_sequence
 *
 ************************************************************/
lld_err_t lld_subdev_dr_run_flush_sequence(bf_dev_id_t dev_id,
                                           bf_subdev_id_t subdev_id) {
  int flush_done;
  lld_err_t rc = LLD_OK;
  lld_dev_t *dev_p = lld_map_subdev_id_to_dev_p(dev_id, subdev_id);
  if (dev_p == NULL) return LLD_ERR_BAD_PARM;

  lld_subdev_dr_disable_all(dev_p);
  lld_dr_dma_enable_set(dev_p, true);
  lld_subdev_dr_flush_all(dev_p);
  flush_done = lld_subdev_dr_wait_for_flush_done(dev_p, 1000);
  if (!flush_done) rc = LLD_ERR_NOT_READY;
  lld_subdev_dr_clear_link_down(dev_p);
  return rc;
}

/*****************************************************************
 * lld_subdev_register_dr_callback
 *
 * attach a callback functio to a DR.
 * Returns:
 *          0 : if no previously installed callback
 *          1 : if previously installed callback was overwriten
 *****************************************************************/
int lld_subdev_register_dr_callback(bf_dev_id_t dev_id,
                                    bf_subdev_id_t subdev_id,
                                    bf_dma_dr_id_t dr_id,
                                    lld_cb_fn_u cb) {
  lld_dr_view_t *view;
  int rc;
  if (dr_id >= BF_DMA_MAX_DR) return LLD_ERR_BAD_PARM;  // bad DR
  view = lld_map_subdev_id_and_dr_to_view_allow_unassigned(
      dev_id, subdev_id, dr_id);
  if (view == NULL) return LLD_ERR_BAD_PARM;  // bad chip

  rc = (view->callback_fn != NULL) ? 1 : 0;
  view->callback_fn = cb.any_fn;
  return rc;
}

/*****************************************************************
 * lld_subdev_get_dr_callback
 *
 * return the callback function attached to a DR.
 *****************************************************************/
void *lld_subdev_get_dr_callback(bf_dev_id_t dev_id,
                                 bf_subdev_id_t subdev_id,
                                 bf_dma_dr_id_t dr_id) {
  lld_dr_view_t *view;

  if (dr_id >= BF_DMA_MAX_DR) return NULL;  // bad dr
  view = lld_map_subdev_id_and_dr_to_view(dev_id, subdev_id, dr_id);
  if (view == NULL) return NULL;  // bad chip

  return view->callback_fn;
}

/** \brief lld_register_completion_callback
 *         Register a callback for Tx completion DR.
 *
 * \param dev_id: int                       : chip #
 * \param dr  : bf_dma_dr_id_t                  : enum for Tx DR
 * \param fn  : lld_completion_callback_fn: callback function
 *
 * \return  0 = callback registered. No prior callback
 * \return  1 = callback overwrote a previously installed cb
 *
 */
int lld_register_completion_callback(bf_dev_id_t dev_id,
                                     bf_subdev_id_t subdev_id,
                                     bf_dma_dr_id_t dr_id,
                                     dr_completion_callback_fn fn) {
  lld_cb_fn_u cb;
  cb.cmp_fn = fn;
  return lld_subdev_register_dr_callback(dev_id, subdev_id, dr_id, cb);
}

/** \brief lld_register_rx_diag_callback
 *         Register a callback for the Diagnostic event DR.
 *
 * \param dev_id: int                    : chip #
 * \param fn  : lld_rx_diag_callback_fn: callback function
 *
 * \return  0 = callback registered. No prior callback
 * \return  1 = callback overwrote a previously installed cb
 *
 */
int lld_register_rx_diag_callback(bf_dev_id_t dev_id,
                                  bf_subdev_id_t subdev_id,
                                  dr_rx_diag_callback_fn fn) {
  lld_cb_fn_u cb;
  cb.rx_diag_fn = fn;
  return lld_subdev_register_dr_callback(dev_id, subdev_id, lld_dr_rx_diag, cb);
}

/** \brief lld_register_rx_lrt_callback
 *         Register a callback for the Lrt DR.
 *
 * \param dev_id: int                   : chip #
 * \param fn  : lld_rx_lrt_callback_fn: callback function
 *
 * \return  0 = callback registered. No prior callback
 * \return  1 = callback overwrote a previously installed cb
 *
 */
int lld_register_rx_lrt_callback(bf_dev_id_t dev_id,
                                 bf_subdev_id_t subdev_id,
                                 dr_rx_lrt_callback_fn fn) {
  lld_cb_fn_u cb;
  cb.rx_lrt_fn = fn;
  return lld_subdev_register_dr_callback(dev_id, subdev_id, lld_dr_rx_lrt, cb);
}

/** \brief lld_register_rx_idle_callback
 *         Register a callback for the idle timeout DR.
 *
 * \param dev_id: int                    : chip #
 * \param fn  : lld_rx_idle_callback_fn: callback function
 *
 * \return  0 = callback registered. No prior callback
 * \return  1 = callback overwrote a previously installed cb
 *
 */
int lld_register_rx_idle_callback(bf_dev_id_t dev_id,
                                  bf_subdev_id_t subdev_id,
                                  dr_rx_idle_callback_fn fn) {
  lld_cb_fn_u cb;
  cb.rx_idle_fn = fn;
  return lld_subdev_register_dr_callback(dev_id, subdev_id, lld_dr_rx_idle, cb);
}

/** \brief lld_register_rx_learn_callback
 *         Register a callback for the learn filter DR.
 *
 * \param dev_id: int                     : chip #
 * \param fn  : lld_rx_learn_callback_fn: callback function
 *
 * \return  0 = callback registered. No prior callback
 * \return  1 = callback overwrote a previously installed cb
 *
 */
int lld_register_rx_learn_callback(bf_dev_id_t dev_id,
                                   bf_subdev_id_t subdev_id,
                                   dr_rx_learn_callback_fn fn) {
  lld_cb_fn_u cb;
  cb.rx_learn_fn = fn;
  return lld_subdev_register_dr_callback(
      dev_id, subdev_id, lld_dr_rx_learn, cb);
}

/** \brief lld_register_rx_packet_callback
 *         Register a callback for an Rx packet DR.
 *
 * \param dev_id: bf_dev_id_t            : chip #
 * \param cos   : int                    : 0-7, identifies the particular DR to
 *register for
 * \param fn    : lld_rx_packet_callback_fn: callback function
 *
 * \return  0 = callback registered. No prior callback
 * \return  1 = callback overwrote a previously installed cb
 *
 */
int lld_register_rx_packet_callback(bf_dev_id_t dev_id,
                                    bf_subdev_id_t subdev_id,
                                    int cos,
                                    dr_rx_packet_callback_fn fn) {
  lld_cb_fn_u cb;
  cb.rx_packet_fn = fn;
  return lld_subdev_register_dr_callback(
      dev_id, subdev_id, lld_dr_rx_pkt_0 + cos, cb);
}

/** \brief dr_process_completion:
 *         Process TX completion
 *
 * \param dr        : lld_dr_view_t *    : DR
 * \param descriptor: dr_descr_value_t * : completion descriptor value pointer
 *
 * \return nothing
 *
 */
void dr_process_completion(lld_dr_view_t *view, dr_descr_value_t *descr_value) {
  lld_cb_fn_u cb;
  dr_completion_callback_fn fn;
  dr_msg_cmp_t *cmp;
  int attr, status, type, e, s;
  uint64_t temp;
  cb.any_fn = view->callback_fn;
  fn = cb.cmp_fn;

  if (fn != NULL) {
    if (lld_validate_dr_id(view->dev_id, view->dr_id)) {
      lld_fault_dma_error(view->dev_id, descr_value->raw_v);
      return;
    }
    cmp = &(descr_value->dr_msg_cmp_v);
    if (lld_dev_is_tofino(view->dev_id)) {
      extract_dr_msg_cmp_wd0(
          cmp->wd0, temp, attr, status, type, e, s);  // temp: data_sz
    } else if (lld_dev_is_tof2(view->dev_id)) {
      extract_tof2_dr_msg_cmp_wd0(
          cmp->wd0, temp, attr, status, type, e, s);  // temp: timestamp
    } else if (lld_dev_is_tof3(view->dev_id)) {
      extract_tof3_dr_msg_cmp_wd0(
          cmp->wd0, temp, attr, status, type, e, s);  // temp: timestamp





    } else {
      lld_fault_dma_error(view->dev_id, descr_value->raw_v);
      return;
    }
    if (status != 0) {  // log error
      lld_fault_dma_error(view->dev_id, descr_value->raw_v);
    }
    fn(view->dev_id,
       view->subdev_id,
       view->dr_id,
       temp,
       attr,
       status,
       type,
       cmp->message_id,
       s,
       e);
  }
}

/** \brief dr_process_rx_lrt:
 *         Process LRT stats
 *
 * \param dr        : lld_dr_view_t *    : DR
 * \param descriptor: dr_descr_value_t * : completion descriptor value pointer
 *
 * \return nothing
 *
 */
void dr_process_rx_lrt(lld_dr_view_t *view, dr_descr_value_t *descr_value) {
  lld_cb_fn_u cb;
  dr_rx_lrt_callback_fn fn;

  cb.any_fn = view->callback_fn;
  fn = cb.rx_lrt_fn;

  // issue client cb
  if (fn != NULL) {
    int data_sz, attr, status, type, e, s;
    dr_msg_rx_t *msg = &(descr_value->dr_msg_rx_v);

    extract_dr_msg_rx_wd0(msg->wd0, data_sz, attr, status, type, e, s);

    fn(view->dev_id,
       view->subdev_id,
       data_sz,
       (bf_dma_addr_t)msg->address_data);

    (void)status;  // keep compiler happy
    (void)e;
    (void)s;
    (void)type;
    (void)attr;
  }
}

/** \brief dr_process_rx_idle:
 *         Process Idle timeouts
 *
 * \param dr        : lld_dr_view_t *    : DR
 * \param descriptor: dr_descr_value_t * : completion descriptor value pointer
 *
 * \return nothing
 *
 */
void dr_process_rx_idle(lld_dr_view_t *view, dr_descr_value_t *descr_value) {
  lld_cb_fn_u cb;
  dr_rx_idle_callback_fn fn;

  cb.any_fn = view->callback_fn;
  fn = cb.rx_idle_fn;

  // issue client cb
  if (fn != NULL) {
    int data_sz, attr, status, type, e, s;
    dr_msg_rx_t *msg = &(descr_value->dr_msg_rx_v);

    extract_dr_msg_rx_wd0(msg->wd0, data_sz, attr, status, type, e, s);

    fn(view->dev_id,
       view->subdev_id,
       data_sz,
       (bf_dma_addr_t)msg->address_data);

    (void)status;  // keep compiler happy
    (void)e;
    (void)s;
    (void)type;
    (void)attr;
  }
}

/** \brief dr_process_rx_learn:
 *         Process learn filter quanta
 *
 * \param dr        : lld_dr_view_t *    : DR
 * \param descriptor: dr_descr_value_t * : completion descriptor value pointer
 *
 * \return nothing
 *
 */
void dr_process_rx_learn(lld_dr_view_t *view, dr_descr_value_t *descr_value) {
  lld_cb_fn_u cb;
  dr_rx_learn_callback_fn fn;

  cb.any_fn = view->callback_fn;
  fn = cb.rx_learn_fn;

  // issue client cb
  if (fn != NULL) {
    int data_sz, attr, status, type, e, s, pipe;
    dr_msg_rx_t *msg = &(descr_value->dr_msg_rx_v);

    extract_dr_msg_rx_wd0(msg->wd0, data_sz, attr, status, type, e, s);
    pipe = BITS64(msg->wd0, 9, 7);

    fn(view->dev_id,
       view->subdev_id,
       data_sz,
       (bf_dma_addr_t)msg->address_data,
       s,
       e,
       pipe);

    (void)status;  // keep compiler happy
    (void)type;
    (void)attr;
  }
}

/** \brief dr_process_rx_diag:
 *         Process Diagnostic events
 *
 * \param dr        : lld_dr_view_t *    : DR
 * \param descriptor: dr_descr_value_t * : completion descriptor value pointer
 *
 * \return nothing
 *
 */
void dr_process_rx_diag(lld_dr_view_t *view, dr_descr_value_t *descr_value) {
  lld_cb_fn_u cb;
  dr_rx_diag_callback_fn fn;

  cb.any_fn = view->callback_fn;
  fn = cb.rx_diag_fn;

  // issue client cb
  if (fn != NULL) {
    int data_sz, attr, status = 0, type, e, s;
    dr_msg_rx_t *msg = &(descr_value->dr_msg_rx_v);

    extract_dr_msg_rx_wd0(msg->wd0, data_sz, attr, status, type, e, s);
#if 0
    printf("DIAG: Desc: sz=%d : attr=%d : status=%x : type=%d: s=%d : e=%d\n",
           data_sz,
           attr,
           status,
           type,
           s,
           e);
#endif
    fn(view->dev_id,
       view->subdev_id,
       data_sz,
       (bf_dma_addr_t)msg->address_data);

    (void)status;  // keep compiler happy
    (void)e;
    (void)s;
    (void)type;
    (void)attr;
  }
}

/** \brief dr_process_rx_pkt:
 *         Process RX pkt reception
 *
 * \param dr        : lld_dr_view_t *    : DR
 * \param descriptor: dr_descr_value_t * : completion descriptor value pointer
 *
 * \return nothing
 *
 */
void dr_process_rx_pkt(lld_dr_view_t *view, dr_descr_value_t *descr_value) {
  lld_cb_fn_u cb;
  dr_rx_packet_callback_fn fn;

  cb.any_fn = view->callback_fn;
  fn = cb.rx_packet_fn;

  // issue client cb
  if (fn != NULL) {
    int data_sz, attr, status, type, e, s;
    dr_msg_rx_t *msg = &(descr_value->dr_msg_rx_v);
    int cos = view->dr_id - lld_dr_rx_pkt_0;

    extract_dr_msg_rx_wd0(msg->wd0, data_sz, attr, status, type, e, s);

    fn(view->dev_id,
       view->subdev_id,
       data_sz,
       (bf_dma_addr_t)msg->address_data,
       s,
       e,
       cos);

    (void)status;  // keep compiler happy
    (void)type;
    (void)attr;
  }
}

/***********************************************************
 * lld_dr_start
 *
 * Update device regs to initiate DMA
 ***********************************************************/
int lld_dr_start(bf_dev_id_t dev_id,
                 bf_subdev_id_t subdev_id,
                 bf_dma_dr_id_t dr_id) {
  int depth;
  lld_dr_view_t *view;

  if (!lld_dev_ready(dev_id, subdev_id))
    return LLD_ERR_NOT_READY;  // chip not ready to use
  if (lld_validate_dr_id(dev_id, dr_id)) return LLD_ERR_BAD_PARM;
  /* do not access the dma view if not owned by this process */
  lld_dr_cfg_depth_get(dev_id, subdev_id, dr_id, &depth);
  if (depth == 0) return LLD_ERR_NOT_READY;

  view = lld_map_subdev_id_and_dr_to_view(dev_id, subdev_id, dr_id);
  if (view == NULL) return LLD_ERR_BAD_PARM;  // invalid chip

  dr_update_view(view);

  lld_log_dma(
      2 /*start*/, dev_id, subdev_id, dr_id, NULL, 0, view->head, view->tail);

  dr_publish_view(view);
  return LLD_OK;
}

/***********************************************************
 * lld_dr_get_used_count
 *
 * Get the usage of a DR.
 ***********************************************************/
int lld_dr_get_used_count(bf_dev_id_t dev_id,
                          bf_subdev_id_t subdev_id,
                          bf_dma_dr_id_t dr_id,
                          bool from_hw) {
  int depth;
  lld_dr_view_t *view = NULL;
  if (lld_validate_dr_id(dev_id, dr_id)) return LLD_ERR_BAD_PARM;
  /* do not access the dma view if not owned by this process */
  lld_dr_cfg_depth_get(dev_id, subdev_id, dr_id, &depth);
  if (depth == 0) return LLD_ERR_NOT_READY;

  view = lld_map_subdev_id_and_dr_to_view(dev_id, subdev_id, dr_id);
  if (view == NULL) return LLD_ERR_NOT_READY;

  if (from_hw) dr_update_view(view);
  return dr_used(view);
}

/***********************************************************
 * lld_dr_service
 *
 * Process one or more descriptors from a DR.
 ***********************************************************/
int lld_dr_service(bf_dev_id_t dev_id,
                   bf_subdev_id_t subdev_id,
                   bf_dma_dr_id_t dr_id,
                   int max_work) {
  lld_dr_view_t *view = NULL;
  int num_srvcd = 0;
  int n_pending;
  int depth;
  if (!lld_dev_ready(dev_id, subdev_id)) {
    return 0;
  }
  if (lld_validate_dr_id(dev_id, dr_id)) {
    return LLD_ERR_BAD_PARM;
  }
  /* do not access the dma view if not owned by this process */
  lld_dr_cfg_depth_get(dev_id, subdev_id, dr_id, &depth);
  if (depth == 0) return LLD_ERR_NOT_READY;

  view = lld_map_subdev_id_and_dr_to_view(dev_id, subdev_id, dr_id);
  if (view == NULL) return 0;  // possibly chip not added yet

  dr_update_view(view);

  /* be sure we dont pull descriptors out of a DR
   * for which we are the producer
   */
  if (view->producer) return 0;
  /* or which no one cares about */
  if (view->callback_fn == NULL) return 0;

  /* This restriction is important for fairness in
   *  processing. it is quite possible the servicing
   *  of a DR will result in another descriptor being
   *  pushed to the device. If that descriptor DMA
   *  completes before this call to lld_dr_service()
   *  then the completion for that DMA will also be
   *  serviced. This could result essentially serializing
   *  the DMAs for a particular DR at the expense of
   *  all other DRs.
   *  So, restrict the "max work to do on this call" by
   *  the number of descriptors ready to go on entry
   *  or the passed limit, whichever is smaller.
   *  As an exception to this rule, we want the RX DR to
   *  be empty so that we can let the next interrupt post
   */
  if (dr_id != lld_dr_rx_learn) {
    n_pending = dr_used(view);
    if (max_work > n_pending) {
      max_work = n_pending;
    }
  }

  while (!dr_empty(view) && (num_srvcd < max_work)) {
    dr_descr_value_t descriptor_value;
    int pull_failed;

    pull_failed = dr_pull(view, &descriptor_value);
    if (pull_failed == LLD_ERR_DR_EMPTY) {
      break;
    } else if (pull_failed == LLD_ERR_LOCK_FAILED) {
      return LLD_ERR_LOCK_FAILED;
    }
    num_srvcd++;
    switch (dr_id) {
      case lld_dr_rx_pkt_0:
      case lld_dr_rx_pkt_1:
      case lld_dr_rx_pkt_2:
      case lld_dr_rx_pkt_3:
      case lld_dr_rx_pkt_4:
      case lld_dr_rx_pkt_5:
      case lld_dr_rx_pkt_6:
      case lld_dr_rx_pkt_7:
        dr_process_rx_pkt(view, &descriptor_value);
        break;
      case lld_dr_rx_lrt:
        dr_process_rx_lrt(view, &descriptor_value);
        break;
      case lld_dr_rx_idle:
        dr_process_rx_idle(view, &descriptor_value);
        break;
      case lld_dr_rx_learn:
        dr_process_rx_learn(view, &descriptor_value);
        break;
      case lld_dr_rx_diag:
        dr_process_rx_diag(view, &descriptor_value);
        break;
      case lld_dr_cmp_pipe_inst_list_0:
      case lld_dr_cmp_pipe_inst_list_1:
      case lld_dr_cmp_pipe_inst_list_2:
      case lld_dr_cmp_pipe_inst_list_3:
      case lld_dr_cmp_que_write_list:
      case lld_dr_cmp_pipe_write_blk:
      case lld_dr_cmp_pipe_read_blk:
      case lld_dr_cmp_mac_stat:
      case lld_dr_cmp_tx_pkt_0:
      case lld_dr_cmp_tx_pkt_1:
      case lld_dr_cmp_tx_pkt_2:
      case lld_dr_cmp_tx_pkt_3:
      case lld_dr_cmp_mac_write_block:
      case lld_dr_cmp_que_write_list_1:
      case lld_dr_cmp_que_read_block_0:
      case lld_dr_cmp_que_read_block_1:
        dr_process_completion(view, &descriptor_value);
        break;
      default:
        bf_sys_assert(0);
    }
  }
  if (num_srvcd > 0) {
    dr_publish_view(view);
  }
  return num_srvcd;
}
