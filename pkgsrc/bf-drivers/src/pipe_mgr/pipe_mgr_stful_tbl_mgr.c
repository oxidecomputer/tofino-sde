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


#include <sched.h>
#include <dvm/bf_drv_intf.h>
#ifdef __sun
#include <limits.h>
#endif
#include <lld/bf_dma_if.h>
#include <tofino_regs/tofino.h>
#include "pipe_mgr_int.h"
#include "pipe_mgr_stful_tbl_mgr.h"
#include "pipe_mgr_tbl.h"
#include "pipe_mgr_phy_mem_map.h"

extern pipe_mgr_ctx_t *pipe_mgr_ctx;

enum stful_tbl_op_type {
  PIPE_MGR_STFUL_TBL_IDX_WR,
  PIPE_MGR_STFUL_TBL_ENT_RST
};
struct stful_tbl_idx_wr {
  bf_dev_pipe_t pipe;
  pipe_stful_mem_idx_t idx;
  pipe_stful_mem_spec_t spec;
};
struct stful_tbl_ent_rst {
  pipe_mat_ent_hdl_t mat_tbl_hdl;
  pipe_mat_ent_hdl_t mat_ent_hdl;
};
struct pipe_mgr_stful_op_list_t {
  struct pipe_mgr_stful_op_list_t *next;
  enum stful_tbl_op_type op;
  union {
    struct stful_tbl_idx_wr idx_wr;
    struct stful_tbl_ent_rst ent_rst;
  } u;
};

void pipe_mgr_stful_free_ops(struct pipe_mgr_stful_op_list_t **op) {
  struct pipe_mgr_stful_op_list_t *x = *op;
  while (x) {
    *op = x->next;
    PIPE_MGR_FREE(x);
    x = *op;
  }
}

static struct pipe_mgr_stful_op_list_t *alloc_op() {
  struct pipe_mgr_stful_op_list_t *x =
      PIPE_MGR_MALLOC(sizeof(struct pipe_mgr_stful_op_list_t));
  if (!x) return x;
  x->next = NULL;
  return x;
}
union pipe_mgr_stful_mem_instr {
  pipe_instr_set_memdata_v_t multi_bit;
  pipe_run_salu_instr_t one_bit;
};

enum stateful_width {
  STFL_WDTH_1 = 1,
  STFL_WDTH_8,
  STFL_WDTH_8x2,
  STFL_WDTH_16,
  STFL_WDTH_16x2,
  STFL_WDTH_32,
  STFL_WDTH_32x2,
  STFL_WDTH_64,
  STFL_WDTH_64x2
};

static bool table_width_is_valid(int width) {
  return 1 == width || 8 == width || 16 == width || 32 == width || 64 == width;
}

static enum stateful_width table_width(struct pipe_mgr_stful_tbl *t) {
  PIPE_MGR_DBGCHK(t);
  if (!t) return 0;
  if (1 == t->width) return STFL_WDTH_1;
  if (8 == t->width && !t->dbl_width) return STFL_WDTH_8;
  if (8 == t->width && t->dbl_width) return STFL_WDTH_8x2;
  if (16 == t->width && !t->dbl_width) return STFL_WDTH_16;
  if (16 == t->width && t->dbl_width) return STFL_WDTH_16x2;
  if (32 == t->width && !t->dbl_width) return STFL_WDTH_32;
  if (32 == t->width && t->dbl_width) return STFL_WDTH_32x2;
  if (64 == t->width && !t->dbl_width) return STFL_WDTH_64;
  if (64 == t->width && t->dbl_width) return STFL_WDTH_64x2;
  PIPE_MGR_DBGCHK(0);
  return 0;
}

static int bit_width_from_stateful_width(enum stateful_width w) {
  switch (w) {
    case STFL_WDTH_1:
      return 1;
    case STFL_WDTH_8:
      return 8;
    case STFL_WDTH_8x2:
      return 16;
    case STFL_WDTH_16:
      return 16;
    case STFL_WDTH_16x2:
      return 32;
    case STFL_WDTH_32:
      return 32;
    case STFL_WDTH_32x2:
      return 64;
    case STFL_WDTH_64:
      return 64;
    case STFL_WDTH_64x2:
      return 128;
    default:
      PIPE_MGR_DBGCHK(0);
      return 0;
  }
}
static int table_bit_width(struct pipe_mgr_stful_tbl *t) {
  PIPE_MGR_DBGCHK(t);
  if (!t) return 0;
  if (1 == t->width) return 1;
  if (8 == t->width && !t->dbl_width) return 8;
  if (8 == t->width && t->dbl_width) return 16;
  if (16 == t->width && !t->dbl_width) return 16;
  if (16 == t->width && t->dbl_width) return 32;
  if (32 == t->width && !t->dbl_width) return 32;
  if (32 == t->width && t->dbl_width) return 64;
  if (64 == t->width && !t->dbl_width) return 64;
  if (64 == t->width && t->dbl_width) return 128;
  PIPE_MGR_DBGCHK(0);
  return 0;
}

static pipe_status_t pipe_mgr_stful_init_shadow_mem(
    struct pipe_mgr_stful_tbl *stbl) {
  pipe_status_t status = PIPE_SUCCESS;
  struct pipe_mgr_stful_tbl_stage_info *stful_stage_info = NULL;
  int i = 0;
  int j = 0;
  uint32_t tbl_idx = 0;

  for (tbl_idx = 0; tbl_idx < stbl->num_tbl_instances; tbl_idx++) {
    struct pipe_mgr_stful_tbl_inst *stbl_inst = &stbl->stful_tbl_inst[tbl_idx];
    /* Since asymmetric, must be called for every pipe */
    uint32_t pipe_id;
    PIPE_BITMAP_ITER(&stbl_inst->pipe_bmp, pipe_id) {
      pipe_bitmap_t pipe_bmp = {{0}};
      PIPE_BITMAP_INIT(&pipe_bmp, PIPE_BMP_SIZE);
      PIPE_BITMAP_SET(&pipe_bmp, pipe_id);
      for (i = 0; i < stbl->num_stages; i++) {
        stful_stage_info = &stbl->stages[i];
        for (j = 0; j < stful_stage_info->num_rams; j++) {
          status = pipe_mgr_phy_mem_map_symmetric_mode_set(
              stbl->dev,
              stbl->direction,
              &pipe_bmp,
              stful_stage_info->stage_id,
              pipe_mem_type_unit_ram,
              stful_stage_info->ram_ids[j],
              false /* Stful is always asymetric */);
          if (status != PIPE_SUCCESS) {
            LOG_ERROR(
                "%s:%d Error in setting symmetric mode for shadow memory for "
                "tbl 0x%x, dev id %d, stage id %d mem id %d, err %s",
                __func__,
                __LINE__,
                stbl->hdl,
                stbl->dev,
                stful_stage_info->stage_id,
                stful_stage_info->ram_ids[j],
                pipe_str_err(status));
            return status;
          }
        }
        for (j = 0; j < stful_stage_info->num_spare_rams; j++) {
          status = pipe_mgr_phy_mem_map_symmetric_mode_set(
              stbl->dev,
              stbl->direction,
              &pipe_bmp,
              stful_stage_info->stage_id,
              pipe_mem_type_unit_ram,
              stful_stage_info->spare_rams[j],
              false /* Stful is always asymetric */);
          if (status != PIPE_SUCCESS) {
            LOG_ERROR(
                "%s:%d Error in setting symmetric mode for shadow memory for "
                "tbl 0x%x, dev id %d, stage id %d mem id %d, err %s",
                __func__,
                __LINE__,
                stbl->hdl,
                stbl->dev,
                stful_stage_info->stage_id,
                stful_stage_info->spare_rams[j],
                pipe_str_err(status));
            return status;
          }
        }
      }
    }
  }
  return PIPE_SUCCESS;
}

static bool table_is_in_pipe(struct pipe_mgr_stful_tbl *t, bf_dev_pipe_t p) {
  return (p / PIPE_BITMAP_BITS_PER_WORD < (uint32_t)t->pipes.hdr.wordcount) &&
         !!PIPE_BITMAP_GET(&t->pipes, p);
}
static int stage_to_stage_idx(struct pipe_mgr_stful_tbl *t, int stage) {
  int i;
  for (i = 0; i < t->num_stages; ++i)
    if (t->stages[i].stage_id == stage) return i;
  return -1;
}
static bool table_is_in_stage(struct pipe_mgr_stful_tbl *t, int s) {
  return -1 != stage_to_stage_idx(t, s);
}
static bool is_valid_index(struct pipe_mgr_stful_tbl *t,
                           pipe_stful_mem_idx_t i,
                           const char *where,
                           const int line) {
  if (i >= t->num_entries_real) {
    LOG_ERROR(
        "Invalid stateful index 0x%x(%d) for tbl 0x%x on dev %d, max 0x%x(%d) "
        "from %s:%d",
        i,
        i,
        t->hdl,
        t->dev,
        t->num_entries_real - 1,
        t->num_entries_real - 1,
        where,
        line);
    return false;
  }
  return true;
}
static void log_index_to_stage_and_offset(struct pipe_mgr_stful_tbl *t,
                                          int log_idx,
                                          int *stage_index,
                                          int *log_stage_offset) {
  for (int i = 0; i < t->num_stages; ++i) {
    if (log_idx < (int)t->stages[i].num_entries) {
      *stage_index = i;
      *log_stage_offset = log_idx;
      return;
    } else {
      log_idx -= t->stages[i].num_entries;
    }
  }
  PIPE_MGR_DBGCHK(0);
}

static bool table_is_one_bit_wide(struct pipe_mgr_stful_tbl *t) {
  PIPE_MGR_DBGCHK(t);
  return STFL_WDTH_1 == table_width(t);
}

static struct pipe_mgr_stful_tbl *stful_tbl_lkup(bf_dev_id_t dev,
                                                 pipe_stful_tbl_hdl_t hdl) {
  struct pipe_mgr_stful_tbl *t = NULL;
  bf_map_sts_t msts;
  msts = pipe_mgr_stful_tbl_map_get(dev, hdl, (void **)&t);
  if (BF_MAP_OK != msts) {
    LOG_ERROR("Cannot find stateful tbl 0x%x on dev %d, from %s:%d",
              hdl,
              dev,
              __func__,
              __LINE__);
    return NULL;
  }
  return t;
}

struct pipe_mgr_stful_tbl *pipe_mgr_stful_tbl_get(bf_dev_id_t dev,
                                                  pipe_stful_tbl_hdl_t hdl) {
  return stful_tbl_lkup(dev, hdl);
}

pipe_status_t pipe_mgr_stful_get_direct_stful_hdl(
    bf_dev_id_t dev,
    pipe_mat_tbl_hdl_t mat_hdl,
    pipe_stful_tbl_hdl_t *stful_hdl) {
  pipe_mat_tbl_info_t *info =
      pipe_mgr_get_tbl_info(dev, mat_hdl, __func__, __LINE__);
  if (info == NULL) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }
  /* Ensure the MAT is referencing a stateful table. */
  if (!info->stful_tbl_ref) {
    LOG_ERROR("Table 0x%x on dev %d does not reference a stateful table",
              mat_hdl,
              dev);
    return PIPE_INVALID_ARG;
  }
  /* Ensure the reference is direct. */
  if (PIPE_TBL_REF_TYPE_DIRECT != info->stful_tbl_ref[0].ref_type) {
    LOG_ERROR(
        "Table 0x%x on dev %d does not directly reference a stateful table",
        mat_hdl,
        dev);
    return PIPE_INVALID_ARG;
  }
  /* Look up the stateful table and handle used by the MAT. */
  *stful_hdl = info->stful_tbl_ref[0].tbl_hdl;
  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_stful_get_mat_hdl(
    bf_dev_id_t dev_id,
    pipe_stful_tbl_hdl_t stful_tbl_hdl,
    pipe_mat_tbl_hdl_t *mat_tbl_hdl) {
  rmt_dev_info_t *dev_info = NULL;
  pipe_mat_tbl_info_t *tbl_info = NULL;
  unsigned i;
  uint32_t p = 0;

  if (!(dev_info = pipe_mgr_get_dev_info(dev_id))) {
    LOG_ERROR(
        "%s:%d Device info for id %u not found", __func__, __LINE__, dev_id);
    return PIPE_OBJ_NOT_FOUND;
  }

  bool found = false;
  for (p = 0; p < dev_info->num_pipeline_profiles; p++) {
    for (tbl_info = dev_info->profile_info[p]->tbl_info_list.mat_tbl_list,
        i = 0;
         i < dev_info->profile_info[p]->tbl_info_list.num_mat_tbls;
         ++tbl_info, ++i) {
      if (tbl_info->stful_tbl_ref &&
          stful_tbl_hdl == tbl_info->stful_tbl_ref[0].tbl_hdl &&
          (!tbl_info->alpm_info || tbl_info->match_type == ALPM_MATCH)) {
        *mat_tbl_hdl = tbl_info->handle;
        found = true;
        break;
      }
    }
    if (found) {
      break;
    }
  }

  return (found ? PIPE_SUCCESS : PIPE_OBJ_NOT_FOUND);
}

static struct pipe_mgr_stful_tbl *get_stful_tbl_from_direct_mat(
    bf_dev_id_t dev, pipe_mat_tbl_hdl_t mat_tbl_hdl) {
  pipe_stful_tbl_hdl_t st_hdl = 0;
  pipe_status_t sts =
      pipe_mgr_stful_get_direct_stful_hdl(dev, mat_tbl_hdl, &st_hdl);
  if (PIPE_SUCCESS != sts) return NULL;
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev, st_hdl);
  if (!t) {
    LOG_ERROR("Table 0x%x on dev %d references unknown stateful table 0x%x",
              mat_tbl_hdl,
              dev,
              st_hdl);
    PIPE_MGR_DBGCHK(t);
    return NULL;
  }
  return t;
}

static void stful_data_encode(enum stateful_width w,
                              int idx,
                              pipe_stful_mem_spec_t *data,
                              uint8_t *payload,
                              uint8_t *mask) {
  switch (w) {
    case STFL_WDTH_1: {
      uint8_t *x = &payload[(idx >> 3) & 0xF];
      *x = data->bit ? (*x | (1u << (idx & 0x7))) : (*x & ~(1u << (idx & 0x7)));
      if (mask) mask[(idx >> 3) & 0xF] = 1 << (idx & 0x7);
      break;
    }
    case STFL_WDTH_8: {
      uint8_t *x = &payload[idx & 0xF];
      *x = data->byte;
      if (mask) mask[idx & 0xF] = 0xFF;
      break;
    }
    case STFL_WDTH_8x2: {
      uint8_t *x = &payload[(idx & 0x7) * 2];
      uint8_t *y = x + 1;
      *x = data->dbl_byte.lo;
      *y = data->dbl_byte.hi;
      if (mask) mask[(idx & 0x7) * 2] = 0xFF;
      if (mask) mask[(idx & 0x7) * 2 + 1] = 0xFF;
      break;
    }
    case STFL_WDTH_16: {
      uint16_t *x = &((uint16_t *)payload)[idx & 0x7];
      *x = data->half;
      if (mask) mask[(idx & 0x7) * 2] = 0xFF;
      if (mask) mask[(idx & 0x7) * 2 + 1] = 0xFF;
      break;
    }
    case STFL_WDTH_16x2: {
      uint16_t *x = &((uint16_t *)payload)[(idx & 0x3) * 2];
      uint16_t *y = x + 1;
      *x = data->dbl_half.lo;
      *y = data->dbl_half.hi;
      if (mask) mask[(idx & 0x3) * 4 + 0] = 0xFF;
      if (mask) mask[(idx & 0x3) * 4 + 1] = 0xFF;
      if (mask) mask[(idx & 0x3) * 4 + 2] = 0xFF;
      if (mask) mask[(idx & 0x3) * 4 + 3] = 0xFF;
      break;
    }
    case STFL_WDTH_32: {
      uint32_t *x = &((uint32_t *)payload)[idx & 0x3];
      *x = data->word;
      if (mask) mask[(idx & 0x3) * 4 + 0] = 0xFF;
      if (mask) mask[(idx & 0x3) * 4 + 1] = 0xFF;
      if (mask) mask[(idx & 0x3) * 4 + 2] = 0xFF;
      if (mask) mask[(idx & 0x3) * 4 + 3] = 0xFF;
      break;
    }
    case STFL_WDTH_32x2: {
      uint32_t *x = &((uint32_t *)payload)[(idx & 0x1) * 2];
      uint32_t *y = x + 1;
      *x = data->dbl_word.lo;
      *y = data->dbl_word.hi;
      if (mask) mask[(idx & 0x1) * 8 + 0] = 0xFF;
      if (mask) mask[(idx & 0x1) * 8 + 1] = 0xFF;
      if (mask) mask[(idx & 0x1) * 8 + 2] = 0xFF;
      if (mask) mask[(idx & 0x1) * 8 + 3] = 0xFF;
      if (mask) mask[(idx & 0x1) * 8 + 4] = 0xFF;
      if (mask) mask[(idx & 0x1) * 8 + 5] = 0xFF;
      if (mask) mask[(idx & 0x1) * 8 + 6] = 0xFF;
      if (mask) mask[(idx & 0x1) * 8 + 7] = 0xFF;
      break;
    }
    case STFL_WDTH_64: {
      PIPE_MGR_MEMCPY(payload + (idx & 0x1) * 8, &data->dbl, 8);
      if (mask) PIPE_MGR_MEMSET(mask + (idx & 0x1) * 8, 0xFF, 8);
      break;
    }
    case STFL_WDTH_64x2: {
      PIPE_MGR_MEMCPY(payload, &data->dbl_dbl.lo, 8);
      PIPE_MGR_MEMCPY(payload + 8, &data->dbl_dbl.hi, 8);
      if (mask) PIPE_MGR_MEMSET(mask, 0xFF, 16);
      break;
    }
    default:
      PIPE_MGR_DBGCHK(0);
      break;
  }
}
static void stful_data_encode_row(enum stateful_width w,
                                  pipe_stful_mem_spec_t *data,
                                  uint8_t *payload) {
  int i = 0, I = 0;
  /* Determine how many subwords are in a RAM row. */
  I = TOF_SRAM_UNIT_WIDTH / bit_width_from_stateful_width(w);
  /* Encode each subword. */
  for (i = 0; i < I; ++i) stful_data_encode(w, i, data, payload, NULL);
}

/* Convert a RAM line to a stateful mem spec. */
static void stful_data_decode(enum stateful_width w,
                              pipe_stful_mem_idx_t idx,
                              pipe_stful_mem_spec_t *data,
                              uint8_t *payload) {
  switch (w) {
    case STFL_WDTH_1: {
      data->bit = (payload[(idx & 0x7F) >> 3] >> (idx & 0x7)) & 1;
      break;
    }
    case STFL_WDTH_8: {
      data->byte = payload[idx & 0xF];
      break;
    }
    case STFL_WDTH_8x2: {
      uint8_t *x = &payload[(idx & 0x7) * 2];
      uint8_t *y = x + 1;
      data->dbl_byte.lo = *x;
      data->dbl_byte.hi = *y;
      break;
    }
    case STFL_WDTH_16: {
      uint16_t *x = &((uint16_t *)payload)[idx & 0x7];
      data->half = *x;
      break;
    }
    case STFL_WDTH_16x2: {
      uint16_t *x = &((uint16_t *)payload)[(idx & 0x3) * 2];
      uint16_t *y = x + 1;
      data->dbl_half.lo = *x;
      data->dbl_half.hi = *y;
      break;
    }
    case STFL_WDTH_32: {
      uint32_t *x = &((uint32_t *)payload)[idx & 0x3];
      data->word = *x;
      break;
    }
    case STFL_WDTH_32x2: {
      uint32_t *x = &((uint32_t *)payload)[(idx & 0x1) * 2];
      uint32_t *y = x + 1;
      data->dbl_word.lo = *x;
      data->dbl_word.hi = *y;
      break;
    }
    case STFL_WDTH_64: {
      uint64_t *x = &((uint64_t *)payload)[idx & 1];
      data->dbl = *x;
      break;
    }
    case STFL_WDTH_64x2: {
      uint64_t *x = &((uint64_t *)payload)[0];
      uint64_t *y = x + 1;
      data->dbl_dbl.lo = *x;
      data->dbl_dbl.hi = *y;
      break;
    }
    default:
      PIPE_MGR_DBGCHK(0);
      break;
  }
}

/* Given a logical index, break it down into the three components of the
 * stateful address.  Returns the number of zeros to append to the subword
 * for data plane cases. */
static int decompose_index(enum stateful_width w,
                           pipe_stful_mem_idx_t i,
                           int *ram,
                           int *line,
                           int *subword) {
  int s = 0;
  switch (w) {
    case STFL_WDTH_1:
      break;
    case STFL_WDTH_8:
      i <<= 3;
      s = 3;
      break;
    case STFL_WDTH_8x2:
    case STFL_WDTH_16:
      i <<= 4;
      s = 4;
      break;
    case STFL_WDTH_16x2:
    case STFL_WDTH_32:
      i <<= 5;
      s = 5;
      break;
    case STFL_WDTH_32x2:
    case STFL_WDTH_64:
      i <<= 6;
      s = 6;
      break;
    case STFL_WDTH_64x2:
      /* There are no subwords in the 64x2 case so set s to 7 so that the shift
       * below to compute the subword always gives zero. */
      s = 7;
      i <<= 7;
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      break;
  }
  /* Note that there are always 7 bits of subword, 10 bits of RAM line and 6
   * bits of RAM unit VPN in the address.  Depending on the size of the sub-
   * word, some of the lsbs of the 7 bit subword will be zero.  One bit wide
   * tables will use all 7 bits to specify a subword of 0-127.  Eight bit
   * wide tables will use 4 bits to specify a subword of 0-15 so the lower 3
   * bits of subword will be zero.  A double width 32 bit wide table only has
   * two subwords so a single bit is needed, therefore the lower 6 bits are
   * zero and the seventh bit is the subword bit. */
  if (subword) *subword = (i & 0x7F) >> s;
  if (line) *line = (i >> 7) & 0x3FF;
  if (ram) *ram = (i >> 17) & 0x3F;
  return s;
}

static pipe_stful_mem_idx_t decompose_virt_addr(enum stateful_width w,
                                                uint32_t addr,
                                                bool *pfe) {
  int s = 0;
  int ram_line;
  int subword;
  int vpn;
  pipe_stful_mem_idx_t idx = 0;
  uint32_t entries_per_line = 0;
  switch (w) {
    case STFL_WDTH_1:
      entries_per_line = 128;
      break;
    case STFL_WDTH_8:
      entries_per_line = 16;
      s = 3;
      break;
    case STFL_WDTH_8x2:
    case STFL_WDTH_16:
      entries_per_line = 8;
      s = 4;
      break;
    case STFL_WDTH_16x2:
    case STFL_WDTH_32:
      entries_per_line = 4;
      s = 5;
      break;
    case STFL_WDTH_32x2:
    case STFL_WDTH_64:
      entries_per_line = 2;
      s = 6;
      break;
    case STFL_WDTH_64x2:
      entries_per_line = 1;
      s = 7;
      break;
    default:
      PIPE_MGR_DBGCHK(0);
  }

  /* There are 7 - (number of bits required for subword) number of
   * LSBs followed by 10 bits of RAM Line, followed by 6 bits of VPN.
   */
  subword = (addr >> s) & ((1 << (7 - s)) - 1);
  ram_line = (addr >> 7) & 0x3FF;
  vpn = (addr >> 17) & 0x3F;

  idx = (ram_line * entries_per_line) +
        (vpn * (TOF_SRAM_UNIT_DEPTH * entries_per_line)) + subword;

  if ((addr >> 23) & 0x1) {
    *pfe = true;
  } else {
    *pfe = false;
  }
  return idx;
}

static void get_shadow_params(struct pipe_mgr_stful_tbl *t,
                              pipe_stful_mem_idx_t index,
                              uint8_t stage_idx,
                              uint32_t *ram_line_num,
                              mem_id_t *mem_id) {
  int ram, line;
  int stage = t->stages[stage_idx].stage_id;
  decompose_index(table_width(t), index, &ram, &line, NULL);
  if (!(table_is_in_stage(t, stage))) {
    PIPE_MGR_DBGCHK(0);
    return;
  }

  if (!(is_valid_index(t, index, __func__, __LINE__))) {
    PIPE_MGR_DBGCHK(0);
    return;
  }

  if (ram < 0) {
    PIPE_MGR_DBGCHK(0);
    return;
  }

  if (ram >= t->stages[stage_idx].num_rams) {
    PIPE_MGR_DBGCHK(0);
    return;
  }

  *ram_line_num = line;
  *mem_id = t->stages[stage_idx].ram_ids[ram];
}

static void get_spec_from_ram_shadow(bf_dev_pipe_t pipe,
                                     struct pipe_mgr_stful_tbl *t,
                                     int stage_idx,
                                     pipe_stful_mem_idx_t index_in_stage,
                                     pipe_stful_mem_spec_t *spec) {
  int stage = t->stages[stage_idx].stage_id;
  uint32_t ram_line_num = 0;
  mem_id_t mem_id = 0;
  get_shadow_params(t, index_in_stage, stage_idx, &ram_line_num, &mem_id);

  /* Getting spec from just first pipe as it is the same in all pipes. */
  if (BF_DEV_PIPE_ALL == pipe || t->symmetric)
    pipe = PIPE_BITMAP_GET_FIRST_SET(&t->stful_tbl_inst[0].pipe_bmp);

  uint8_t *ram_data = NULL;
  pipe_status_t sts = pipe_mgr_phy_mem_map_get_ref(t->dev,
                                                   t->direction,
                                                   pipe_mem_type_unit_ram,
                                                   pipe,
                                                   stage,
                                                   mem_id,
                                                   ram_line_num,
                                                   &ram_data,
                                                   true);
  PIPE_MGR_DBGCHK(PIPE_SUCCESS == sts);

  stful_data_decode(table_width(t), index_in_stage, spec, ram_data);
}

static uint32_t stful_idx_encode_dp(struct pipe_mgr_stful_tbl *t,
                                    int stage,
                                    pipe_act_fn_hdl_t act_fn_hdl,
                                    pipe_stful_mem_idx_t idx) {
  /* Look up the instruction number based on the action function handle. */
  int *instrP = NULL;
  int instr = -1;
  bf_map_sts_t msts =
      bf_map_get(&t->action_to_instr_map, act_fn_hdl, (void **)&instrP);
  if (BF_MAP_OK != msts) {
    PIPE_MGR_DBGCHK(0);
    return ~0u;
  }
  instr = (unsigned long)instrP;

  /* Ensure the index is valid. */
  if (!is_valid_index(t, idx, __func__, __LINE__)) {
    PIPE_MGR_DBGCHK(0);
    return ~0u;
  }

  /* Ensure the stage is valid. */
  if (!table_is_in_stage(t, stage)) {
    PIPE_MGR_DBGCHK(0);
    return ~0u;
  }
  int stage_idx = stage_to_stage_idx(t, stage);

  /* Convert the index to a RAM, line, and subword. */
  int subword, ram_line, ram_idx;
  int ss = decompose_index(table_width(t), idx, &ram_idx, &ram_line, &subword);
  subword = subword << ss;

  /* Map the RAM to a VPN number. */
  if (!(0 <= stage_idx && stage_idx < t->num_stages)) {
    PIPE_MGR_DBGCHK(0);
    return ~0u;
  }
  if (!(ram_idx < t->stages[stage_idx].num_rams)) {
    PIPE_MGR_DBGCHK(0);
    return ~0u;
  }
  int vpn = t->stages[stage_idx].ram_vpn[ram_idx];

  /* The 28 bit indirect address pointer, which is encoded in the match
   * entry overhead, is as follows from MSB to LSB:
   *  - 4 Bits Meter Type to encode the instruction number.
   *  - 1 Bit Per-Flow-Enable
   *  - 6 Bits RAM VPN
   *  - 10 Bits RAM Line
   *  - 7 Bits Subword
   * Note that the Meter Type is encoded as following:
   * To run instruction 0, use value 1
   * To run instruction 1, use value 3
   * To run instruction 2, use value 5
   * To run instruction 3, use value 7
   */
  int instr_val = instr * 2 + 1;
  uint32_t addr = (subword & 0x0000007F) | ((ram_line << 7) & 0x0001FF80) |
                  ((vpn << 17) & 0x007E0000) | ((1 << 23) & 0x00800000) |
                  ((instr_val << 24) & 0x0F000000);
  return addr;
}

static uint32_t assemble_vaddr(uint32_t vpn, uint32_t line, uint32_t subword) {
  return (vpn << 15) | (line << 5) | (subword);
}
static uint32_t stful_idx_to_cpu_wr_addr(enum stateful_width w,
                                         pipe_stful_mem_idx_t idx,
                                         int vpn_cnt,
                                         vpn_id_t *vpn_map) {
  int subword, ram_line, ram_idx, addr;
  decompose_index(w, idx, &ram_idx, &ram_line, &subword);
  PIPE_MGR_DBGCHK(ram_idx < vpn_cnt);
  /* Encode the subword to indicate both the width of the write and the
   * subword index.  The mapping is as follows where an "x" bit indicates the
   * subword.
   * 128 bit: 01111
   *  64 bit: x0111
   *  32 bit: xx011
   *  16 bit: xxx01
   *   8 bit: xxxx0
   *   1 bit: Not possible, must use a stateful ALU instruction.
   */
  switch (w) {
    case STFL_WDTH_1:
      PIPE_MGR_DBGCHK(0);
      break;
    case STFL_WDTH_8:
      PIPE_MGR_DBGCHK(!(subword & 0x10));
      subword = subword << 1;
      break;
    case STFL_WDTH_8x2:
    case STFL_WDTH_16:
      PIPE_MGR_DBGCHK(!(subword & 0x18));
      subword = subword << 2;
      subword |= 1;
      break;
    case STFL_WDTH_16x2:
    case STFL_WDTH_32:
      PIPE_MGR_DBGCHK(!(subword & 0x1C));
      subword = subword << 3;
      subword |= 3;
      break;
    case STFL_WDTH_32x2:
    case STFL_WDTH_64:
      PIPE_MGR_DBGCHK(!(subword & 0x1E));
      subword = subword << 4;
      subword |= 7;
      break;
    case STFL_WDTH_64x2:
      subword = 0xF;
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      break;
  }
  addr = assemble_vaddr(vpn_map[ram_idx], ram_line, subword);
  return addr;
}
/* Form the low bits of a virtual address to allow the CPU to read a 128 bit RAM
 * line.  The logical table will need to be set in the upper bits to complete
 * the virtual address.  The data read back will need to be shifted and masked
 * to extract a subword. */
static uint32_t ram_row_to_cpu_rd_addr(int vpn, int ram_line) {
  /* Hardcode the subword/huffman bits to indicate 128 bit access. */
  int subword = 0xF;
  return (vpn << 15) | (ram_line << 5) | (subword);
}

static void set_write_instr(union pipe_mgr_stful_mem_instr *instr,
                            struct pipe_mgr_stful_tbl *t,
                            int stage_idx,
                            pipe_stful_mem_idx_t tbl_idx,
                            pipe_stful_mem_spec_t *spec,
                            int *sz) {
  int lt = t->stages[stage_idx].logical_table;
  if (table_is_one_bit_wide(t)) {
    /* Construct the stateful address based on a 7 bit subword, 10 bit RAM
     * line, and 6 bit VPN. */
    int ram, line, subword;
    decompose_index(STFL_WDTH_1, tbl_idx, &ram, &line, &subword);
    int vpn = t->stages[stage_idx].ram_vpn[ram];
    uint32_t vaddr =
        (vpn & 0x3F) << 17 | (line & 0x3FF) << 7 | (subword & 0x7F);
    int instr_index = spec->bit ? t->set_instr : t->clr_instr;
    construct_instr_run_salu(t->dev, &instr->one_bit, instr_index, lt, vaddr);
    *sz = sizeof(instr->one_bit);
  } else {
    /* Construct the virtual address based on the table width, index, and
     * RAM VPN numbers. */
    uint32_t vaddr = stful_idx_to_cpu_wr_addr(table_width(t),
                                              tbl_idx,
                                              t->stages[stage_idx].num_rams,
                                              t->stages[stage_idx].ram_vpn);
    uint8_t payload[16] = {0};
    /* Align the data in the 128-bit RAM word based on the table width and
     * index. */
    stful_data_encode(table_width(t), tbl_idx, spec, payload, NULL);
    construct_instr_set_v_memdata(t->dev,
                                  &instr->multi_bit,
                                  payload,
                                  16,
                                  lt,
                                  pipe_virt_mem_type_sel_stful,
                                  vaddr);
    *sz = sizeof(instr->multi_bit);
  }
}

static pipe_status_t update_shadow(struct pipe_mgr_stful_tbl *t,
                                   int stage_idx,
                                   pipe_stful_mem_idx_t index,
                                   bf_dev_pipe_t dp,
                                   pipe_stful_mem_spec_t *spec) {
  pipe_status_t sts = PIPE_SUCCESS;
  uint32_t ram_line_num = 0;
  mem_id_t mem_id;
  bf_dev_pipe_t pipe_id;

  /* Format data into a shifted 16 byte array with a 16 byte mask. */
  uint8_t data[TOF_SRAM_UNIT_WIDTH / 8] = {0};
  uint8_t mask[TOF_SRAM_UNIT_WIDTH / 8] = {0};
  stful_data_encode(table_width(t), index, spec, data, mask);

  /* Get the low bits of the physical address for the index. */
  get_shadow_params(t, index, stage_idx, &ram_line_num, &mem_id);

  /* Update the shadow. */
  PIPE_BITMAP_ITER(&t->pipes, pipe_id) {
    if (BF_DEV_PIPE_ALL == dp || dp == pipe_id) {
      sts = pipe_mgr_phy_mem_map_write(t->dev,
                                       t->direction,
                                       pipe_id,
                                       t->stages[stage_idx].stage_id,
                                       pipe_mem_type_unit_ram,
                                       mem_id,
                                       ram_line_num,
                                       data,
                                       mask);
    }
  }
  return sts;
}

static struct pipe_mgr_stful_tbl_inst *get_stful_tbl_inst_by_pipe_id(
    struct pipe_mgr_stful_tbl *stbl, bf_dev_pipe_t pipe_id) {
  uint32_t i;
  for (i = 0; i < stbl->num_tbl_instances; i++) {
    struct pipe_mgr_stful_tbl_inst *stbl_inst = &stbl->stful_tbl_inst[i];
    if (stbl_inst->pipe_id == pipe_id ||
        stbl_inst->pipe_id == BF_DEV_PIPE_ALL) {
      return stbl_inst;
    }
  }
  char msg[256];
  char msg_offset = 0;
  msg[0] = 0;
  for (i = 0; i < stbl->num_tbl_instances; ++i) {
    int x = snprintf(msg + msg_offset,
                     256 - msg_offset,
                     "%x ",
                     stbl->stful_tbl_inst[i].pipe_id);
    if (x < 0 || x > 256 - msg_offset) break;
    msg_offset += x;
  }
  LOG_ERROR(
      "Stful table instance not found.  Dev %d table 0x%x pipe %x, expected %s",
      stbl->dev,
      stbl->hdl,
      pipe_id,
      msg);
  return NULL;
}

static pipe_status_t write_one_stage(pipe_sess_hdl_t sess_hdl,
                                     bf_dev_target_t dev_tgt,
                                     struct pipe_mgr_stful_tbl *t,
                                     int stage_idx,
                                     pipe_stful_mem_idx_t index,
                                     pipe_stful_mem_spec_t *spec) {
  union pipe_mgr_stful_mem_instr instr;
  int sz = 0;

  set_write_instr(&instr, t, stage_idx, index, spec, &sz);
  if (!t->skip_shadow)
    update_shadow(t, stage_idx, index, dev_tgt.dev_pipe_id, spec);

  pipe_status_t sts;
  pipe_bitmap_t pipes = {{0}};
  PIPE_BITMAP_INIT(&pipes, PIPE_BMP_SIZE);
  if (dev_tgt.dev_pipe_id == BF_DEV_PIPE_ALL) {
    PIPE_BITMAP_ASSIGN(&pipes, &t->pipes);
  } else {
    if (!table_is_in_pipe(t, dev_tgt.dev_pipe_id)) return PIPE_INVALID_ARG;
    if (t->in_restore || t->num_scopes == 1) {
      // In state restore, we want to program each pipe in a scope separately.
      // If scopes are not used, per single pipe write is allowed.
      PIPE_BITMAP_SET(&pipes, dev_tgt.dev_pipe_id);
    } else {
      struct pipe_mgr_stful_tbl_inst *stbl_inst = NULL;
      stbl_inst = get_stful_tbl_inst_by_pipe_id(t, dev_tgt.dev_pipe_id);
      if (!stbl_inst) {
        LOG_ERROR("%s:%d Stful table inst lookup failure", __func__, __LINE__);
        return PIPE_INVALID_ARG;
      }
      PIPE_BITMAP_ASSIGN(&pipes, &stbl_inst->pipe_bmp);
    }
  }

  /* For indirect tables update our shadow on writes. */
  if (!t->direct) {
    int entry_bit_width = table_bit_width(t);
    int entry_bit_offset = entry_bit_width * index;
    int entry_ram_line = entry_bit_offset / TOF_SRAM_UNIT_WIDTH;
    bf_dev_pipe_t p;
    PIPE_BITMAP_ITER(&pipes, p) {
      uint8_t *ram_line = NULL;
      ram_line = t->stages[stage_idx].db[p] + 16 * entry_ram_line;
      stful_data_encode(table_width(t), index, spec, ram_line, NULL);
    }
  }

  sts = pipe_mgr_drv_ilist_add(&sess_hdl,
                               t->dev_info,
                               &pipes,
                               t->stages[stage_idx].stage_id,
                               (uint8_t *)&instr,
                               sz);
  if (PIPE_SUCCESS != sts) {
    LOG_ERROR("%s:%d Error in ilist add, err %s",
              __func__,
              __LINE__,
              pipe_str_err(sts));
  }
  return sts;
}

pipe_status_t pipe_mgr_stful_dir_ent_del(bf_dev_id_t dev_id,
                                         pipe_stful_tbl_hdl_t hdl,
                                         bf_dev_pipe_t pipe_id,
                                         pipe_mat_ent_hdl_t ent_hdl) {
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev_id, hdl);
  if (!t) {
    LOG_ERROR("%s: Dev %d cannot find stful tbl 0x%x", __func__, dev_id, hdl);
    return PIPE_INVALID_ARG;
  }
  struct pipe_mgr_stful_tbl_inst *inst =
      get_stful_tbl_inst_by_pipe_id(t, pipe_id);
  if (!inst) {
    LOG_ERROR("%s: Dev %d tbl 0x%x cannot find instance for pipe %x",
              __func__,
              dev_id,
              hdl,
              pipe_id);
    return PIPE_INVALID_ARG;
  }
  bf_dev_pipe_t p;
  PIPE_BITMAP_ITER(&inst->pipe_bmp, p) {
    pipe_stful_mem_spec_t *shdw_spec = NULL;
    bf_map_sts_t sts =
        bf_map_get_rmv(&t->hdl_to_shdw[p], ent_hdl, (void **)&shdw_spec);
    if (sts == BF_MAP_OK && shdw_spec) PIPE_MGR_FREE(shdw_spec);
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stful_move(pipe_sess_hdl_t ses,
                                  bf_dev_id_t dev,
                                  pipe_stful_tbl_hdl_t h,
                                  bf_dev_pipe_t pipe,
                                  int src_stage,
                                  int dst_stage,
                                  pipe_stful_mem_idx_t src_idx,
                                  pipe_stful_mem_idx_t dst_idx) {
  pipe_status_t sts = PIPE_SUCCESS;
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev, h);
  if (!t) {
    LOG_ERROR(
        "%s:%d Failed to find stateful table for device %d and table handle "
        "%#x",
        __func__,
        __LINE__,
        dev,
        h);
    return PIPE_TABLE_NOT_FOUND;
  }

  /* Determine which pipes to write to. */
  if (t->symmetric) {
    PIPE_MGR_DBGCHK(pipe == BF_DEV_PIPE_ALL);
  } else {
    PIPE_MGR_DBGCHK(pipe < t->dev_info->num_active_pipes);
  }

  int src_stage_idx = stage_to_stage_idx(t, src_stage);
  int dst_stage_idx = stage_to_stage_idx(t, dst_stage);
  if (src_stage_idx == -1 || dst_stage_idx == -1) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }

  /* Decode stateful spec from RAM shadow. */
  pipe_stful_mem_spec_t spec = {0};
  get_spec_from_ram_shadow(pipe, t, src_stage_idx, src_idx, &spec);

  /* Write the spec to the new location in the RAM shadow. */
  bf_dev_pipe_t pipe_id;
  PIPE_BITMAP_ITER(&t->pipes, pipe_id) {
    if (BF_DEV_PIPE_ALL == pipe || pipe == pipe_id)
      sts = update_shadow(t, dst_stage_idx, dst_idx, pipe_id, &spec);
    if (PIPE_SUCCESS != sts) {
      PIPE_MGR_DBGCHK(0);
      return sts;
    }
  }

  /* Log it for good measure. */
  uint32_t hi = 0, lo = 0;
  bool dual = false;
  pipe_mgr_stful_spec_decode(dev, h, spec, &dual, &hi, &lo);
  LOG_TRACE(
      "Moved stful entry: dev %d pipe %#x tbl %#x stage.idx %d.%d -> %d.%d "
      "(spec %#x_%#x)",
      dev,
      pipe,
      h,
      src_stage,
      src_idx,
      dst_stage,
      dst_idx,
      hi,
      lo);

  /* When moving between stages write the spec at the new location. */
  if (dst_stage != src_stage) {
    bf_dev_target_t dev_tgt = {0};
    dev_tgt.device_id = dev;
    dev_tgt.dev_pipe_id = t->symmetric ? BF_DEV_PIPE_ALL : pipe;
    sts = write_one_stage(ses, dev_tgt, t, dst_stage_idx, dst_idx, &spec);
  }
  return sts;
}

pipe_status_t pipe_mgr_stful_word_write(bf_dev_target_t dev_tgt,
                                        pipe_stful_tbl_hdl_t tbl_hdl,
                                        pipe_stful_mem_idx_t index,
                                        pipe_stful_mem_spec_t *spec,
                                        struct pipe_mgr_stful_op_list_t **l) {
  /* Lookup the table, SM layer has already validated that the table exists. */
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev_tgt.device_id, tbl_hdl);
  if (t == NULL || t->direct) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }

  /* Validate the dev target. */
  if (!pipe_mgr_valid_dev_tgt(dev_tgt, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }

  /* Validate the index. */
  if (!is_valid_index(t, index, __func__, __LINE__)) return PIPE_INVALID_ARG;

  /* If the spec is NULL use the table's initial value. */
  if (!spec) spec = &t->initial_value;

  /* Save this operation for later processing. */
  *l = alloc_op();
  if (!*l) return PIPE_NO_SYS_RESOURCES;

  (*l)->op = PIPE_MGR_STFUL_TBL_IDX_WR;
  (*l)->u.idx_wr.pipe = dev_tgt.dev_pipe_id;
  (*l)->u.idx_wr.idx = index;
  PIPE_MGR_MEMCPY(&(*l)->u.idx_wr.spec, spec, sizeof(*spec));
  return PIPE_SUCCESS;
}

static pipe_status_t do_word_write(pipe_sess_hdl_t sess_hdl,
                                   bf_dev_id_t dev,
                                   pipe_stful_tbl_hdl_t tbl_hdl,
                                   struct pipe_mgr_stful_op_list_t *l) {
  bf_dev_target_t dev_tgt = {.device_id = dev, .dev_pipe_id = l->u.idx_wr.pipe};
  pipe_stful_mem_idx_t index = l->u.idx_wr.idx;
  pipe_stful_mem_spec_t *spec = &l->u.idx_wr.spec;
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev, tbl_hdl);
  // This function supports only indirect tables.
  if (t == NULL || t->direct) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }

  /* Validate the dev target. */
  if (!pipe_mgr_valid_dev_tgt(dev_tgt, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }
  /* Validate the index. */
  if (!is_valid_index(t, index, __func__, __LINE__)) return PIPE_INVALID_ARG;

  /* Issue the write to the stage containing the index. */
  int s = 0, idx_in_stage = 0;
  log_index_to_stage_and_offset(t, index, &s, &idx_in_stage);
  pipe_status_t sts = PIPE_SUCCESS;
  sts = write_one_stage(sess_hdl, dev_tgt, t, s, idx_in_stage, spec);
  if (PIPE_SUCCESS != sts) {
    LOG_ERROR(
        "Dev %d Pipe %x Tbl 0x%x Write Idx %d (stage %d idx %d) failed %s",
        dev_tgt.device_id,
        dev_tgt.dev_pipe_id,
        t->hdl,
        index,
        t->stages[s].stage_id,
        idx_in_stage,
        pipe_str_err(sts));
  }
  return sts;
}

static bool validate_direct_idx(struct pipe_mgr_stful_tbl *t,
                                int stage,
                                int idx) {
  if ((uint32_t)idx >= t->num_entries_real) return false;
  int s;
  for (s = 0; s < t->num_stages; ++s) {
    if (t->stages[s].stage_id == stage) {
      return idx >= 0 && (uint32_t)idx < t->stages[s].num_entries;
    }
  }
  PIPE_MGR_DBGCHK(0);
  return false;
}

pipe_status_t pipe_mgr_stful_direct_word_write_at(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t dev,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    bf_dev_pipe_t pipe,
    int stage,
    uint32_t idx,
    pipe_stful_mem_spec_t *spec,
    uint32_t api_flags) {
  (void)api_flags;
  /* Assume SM layer has validated the session. */
  /* Validate the device id. */
  if (!pipe_mgr_valid_deviceId(dev, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }
  struct pipe_mgr_stful_tbl *t =
      get_stful_tbl_from_direct_mat(dev, mat_tbl_hdl);
  if (!t) return PIPE_INVALID_ARG;
  if (!t->direct) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }

  if (!validate_direct_idx(t, stage, idx)) {
    LOG_ERROR(
        "Invalid direct index 0x%x(%d) stage %d, stful tbl 0x%x (%d) MAT 0x%x "
        "(%d), entry_hdl 0x%x (%d), dev %d, max 0x%x(%d) at %s:%d",
        idx,
        idx,
        stage,
        t->hdl,
        t->hdl,
        mat_tbl_hdl,
        mat_tbl_hdl,
        mat_ent_hdl,
        mat_ent_hdl,
        dev,
        t->num_entries_real - 1,
        t->num_entries_real - 1,
        __func__,
        __LINE__);
    return PIPE_INVALID_ARG;
  }
  if (t->symmetric) {
    PIPE_MGR_DBGCHK(BF_DEV_PIPE_ALL == pipe);
  } else {
    PIPE_MGR_DBGCHK(BF_DEV_PIPE_ALL != pipe);
  }
  if (!table_is_in_stage(t, stage)) {
    PIPE_MGR_DBGCHK(table_is_in_stage(t, stage));
    return PIPE_INVALID_ARG;
  }

  int stage_idx = stage_to_stage_idx(t, stage);
  if (stage_idx == -1) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }
  /* Determine if the write is to all pipes or one pipe and build a
   * dev_target. */
  bf_dev_target_t dev_tgt;
  dev_tgt.device_id = dev;
  dev_tgt.dev_pipe_id = t->symmetric ? BF_DEV_PIPE_ALL : pipe;

  /* Get the existing spec. */
  pipe_stful_mem_spec_t old_spec;
  if (!spec) {
    /* The register is being reset back to the last written value. */
    get_spec_from_ram_shadow(dev_tgt.dev_pipe_id, t, stage_idx, idx, &old_spec);
    spec = &old_spec;
  } else {
    /* Writting the spec to a new value. */
  }

  switch (table_width(t)) {
    case STFL_WDTH_1:
      LOG_TRACE(
          "Stful write dev %d pipe %X stage %d tbl %#x index %#x data %#x",
          dev,
          pipe,
          stage,
          t->hdl,
          idx,
          spec->bit);
      break;
    case STFL_WDTH_8:
      LOG_TRACE(
          "Stful write dev %d pipe %X stage %d tbl %#x index %#x data %#x",
          dev,
          pipe,
          stage,
          t->hdl,
          idx,
          spec->byte);
      break;
    case STFL_WDTH_8x2:
      LOG_TRACE(
          "Stful write dev %d pipe %X stage %d tbl %#x index %#x data %#x:%#x",
          dev,
          pipe,
          stage,
          t->hdl,
          idx,
          spec->dbl_byte.hi,
          spec->dbl_byte.lo);
      break;
    case STFL_WDTH_16:
      LOG_TRACE(
          "Stful write dev %d pipe %X stage %d tbl %#x index %#x data %#x",
          dev,
          pipe,
          stage,
          t->hdl,
          idx,
          spec->half);
      break;
    case STFL_WDTH_16x2:
      LOG_TRACE(
          "Stful write dev %d pipe %X stage %d tbl %#x index %#x data %#x:%#x",
          dev,
          pipe,
          stage,
          t->hdl,
          idx,
          spec->dbl_half.hi,
          spec->dbl_half.lo);
      break;
    case STFL_WDTH_32:
      LOG_TRACE(
          "Stful write dev %d pipe %X stage %d tbl %#x index %#x data %#x",
          dev,
          pipe,
          stage,
          t->hdl,
          idx,
          spec->word);
      break;
    case STFL_WDTH_32x2:
      LOG_TRACE(
          "Stful write dev %d pipe %X stage %d tbl %#x index %#x data %#x:%#x",
          dev,
          pipe,
          stage,
          t->hdl,
          idx,
          spec->dbl_word.hi,
          spec->dbl_word.lo);
      break;
    case STFL_WDTH_64:
      LOG_TRACE(
          "Stful write dev %d pipe %X stage %d tbl %#x index %#x data "
          "0x%" PRIx64,
          dev,
          pipe,
          stage,
          t->hdl,
          idx,
          spec->dbl);
      break;
    case STFL_WDTH_64x2:
      LOG_TRACE(
          "Stful write dev %d pipe %X stage %d tbl %#x index %#x data "
          "0x%" PRIx64 ":0x%" PRIx64,
          dev,
          pipe,
          stage,
          t->hdl,
          idx,
          spec->dbl_dbl.hi,
          spec->dbl_dbl.lo);
      break;
  }

  /* Issue the write to the stateful table at the index of the MAT entry in
   * the correct stage. */
  pipe_status_t sts;
  sts = write_one_stage(sess_hdl, dev_tgt, t, stage_idx, idx, spec);
  if (PIPE_SUCCESS != sts) {
    return sts;
  }

  /* Update the shadow with the entry's data, this needs to be done for each
   * pipe in the scope. */
  struct pipe_mgr_stful_tbl_inst *inst = get_stful_tbl_inst_by_pipe_id(t, pipe);
  if (!inst) {
    LOG_ERROR(
        "Dev %d pipe %x tbl 0x%x instance not found during write for tbl 0x%x "
        "entry %d",
        t->dev,
        pipe,
        t->hdl,
        mat_tbl_hdl,
        mat_ent_hdl);
    return PIPE_INVALID_ARG;
  }

  /* Update our shadow with the data written. */
  for (int i = 0; i < PIPE_BMP_SIZE; ++i) {
    if (!PIPE_BITMAP_GET(&inst->pipe_bmp, i)) continue;
    pipe_stful_mem_spec_t *shdw_spec = NULL;
    bf_map_sts_t msts =
        bf_map_get(&t->hdl_to_shdw[i], mat_ent_hdl, (void **)&shdw_spec);
    if (msts == BF_MAP_OK) {
      *shdw_spec = *spec;
    } else if (msts == BF_MAP_NO_KEY) {
      shdw_spec = PIPE_MGR_MALLOC(sizeof *shdw_spec);
      if (!shdw_spec) {
        LOG_ERROR("%s:%d Malloc failure", __func__, __LINE__);
        return PIPE_NO_SYS_RESOURCES;
      }
      *shdw_spec = *spec;
      msts = bf_map_add(&t->hdl_to_shdw[i], mat_ent_hdl, shdw_spec);
      if (msts != BF_MAP_OK) {
        /* Failed to insert the entry to the map.  This should not happen since
         * we already verified the key wasn't in the map. */
        LOG_ERROR(
            "Dev %d pipe %x tbl %x error %d writing shadow for tbl %x entry %d",
            t->dev,
            i,
            t->hdl,
            msts,
            mat_tbl_hdl,
            mat_ent_hdl);
        PIPE_MGR_DBGCHK(0);
        return PIPE_UNEXPECTED;
      }
    } else {
      LOG_ERROR(
          "Dev %d pipe %x tbl %x, error %d updating shadow for tbl %x entry %d "
          "update",
          t->dev,
          i,
          t->hdl,
          msts,
          mat_tbl_hdl,
          mat_ent_hdl);
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
    }
  }
  return sts;
}

static pipe_status_t do_direct_word_reset(pipe_sess_hdl_t sess_hdl,
                                          bf_dev_id_t dev,
                                          struct pipe_mgr_stful_op_list_t *l) {
  pipe_mat_ent_hdl_t mat_tbl_hdl = l->u.ent_rst.mat_tbl_hdl;
  pipe_mat_ent_hdl_t mat_ent_hdl = l->u.ent_rst.mat_ent_hdl;
  struct pipe_mgr_stful_tbl *t =
      get_stful_tbl_from_direct_mat(dev, mat_tbl_hdl);
  if (!t) return PIPE_INVALID_ARG;
  if (!t->direct) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }

  /* Get the location of the match entry. */
  bf_dev_pipe_t pipe;
  dev_stage_t stage;
  uint32_t idx;
  if (PIPE_SUCCESS !=
      pipe_mgr_mat_ent_get_dir_ent_location(
          dev, mat_tbl_hdl, mat_ent_hdl, 0, &pipe, &stage, NULL, &idx)) {
    LOG_ERROR(
        "Failed to get entry location, MAT 0x%x (%d), entry_hdl 0x%x (%d), dev "
        "%d at %s:%d",
        mat_tbl_hdl,
        mat_tbl_hdl,
        mat_ent_hdl,
        mat_ent_hdl,
        dev,
        __func__,
        __LINE__);
    return PIPE_OBJ_NOT_FOUND;
  }

  return pipe_mgr_stful_direct_word_write_at(
      sess_hdl, dev, mat_tbl_hdl, mat_ent_hdl, pipe, stage, idx, NULL, 0);
}

static void set_initial_value(pipe_sess_hdl_t sess_hdl,
                              struct pipe_mgr_stful_tbl *t,
                              bool shadow_only) {
  struct pipe_mgr_stful_tbl_inst *stbl_inst = NULL;
  if (t->skip_shadow) return;
  rmt_dev_info_t *dev_info = t->dev_info;
  if (dev_info == NULL) {
    PIPE_MGR_DBGCHK(0);
    return;
  }
  rmt_dev_cfg_t *dev_cfg = &dev_info->dev_cfg;

  int entry_width = t->width + (t->dbl_width ? t->width : 0);
  int entries_per_row = TOF_SRAM_UNIT_WIDTH / entry_width;

  const uint8_t bytes_per_row = (TOF_SRAM_UNIT_WIDTH / 8);
  uint8_t data[TOF_SRAM_UNIT_WIDTH / 8] = {0};
  int i;
  uint32_t ram_line_num = 0, tbl_idx = 0;
  mem_id_t mem_id = 0;
  bf_dev_pipe_t pipe_id;

  for (i = 0; i < entries_per_row; ++i) {
    stful_data_encode(table_width(t), i, &t->initial_value, data, NULL);
  }

  pipe_status_t x = PIPE_SUCCESS;

  int s;
  for (tbl_idx = 0; tbl_idx < t->num_tbl_instances; tbl_idx++) {
    stbl_inst = &t->stful_tbl_inst[tbl_idx];

    /* Go over all pipes */
    PIPE_BITMAP_ITER(&stbl_inst->pipe_bmp, pipe_id) {
      for (s = 0; s < t->num_stages; ++s) {
        int stage = t->stages[s].stage_id;
        uint32_t idx = 0;
        /* Write this to each RAM line of each RAM this table uses. */
        for (idx = 0; idx < t->stages[s].num_entries; idx += entries_per_row) {
          /* Update the shadow only. */
          get_shadow_params(t, idx, s, &ram_line_num, &mem_id);
          x = pipe_mgr_phy_mem_map_write(t->dev,
                                         t->direction,
                                         pipe_id,
                                         stage,
                                         pipe_mem_type_unit_ram,
                                         mem_id,
                                         ram_line_num,
                                         data,
                                         NULL);
          PIPE_MGR_DBGCHK(PIPE_SUCCESS == x);

          if (!t->direct) {
            int entry_bit_width = table_bit_width(t);
            int entry_bit_offset = entry_bit_width * idx;
            int entry_ram_line = entry_bit_offset / TOF_SRAM_UNIT_WIDTH;
            uint8_t *ram_line = NULL;

            /*ram line is 128bit/16byte of size (TOF_SRAM_UNIT_WIDTH)*/
            ram_line = t->stages[s].db[pipe_id] + 16 * entry_ram_line;
            /* Iterate over all entries in one row */
            /* We can encode initial value into the target memory or just
             * memcpy from 'data' pointer, which is already encoded */
            /* Let's do memcpy */
            PIPE_MGR_MEMCPY(ram_line, data, bytes_per_row);
          }
        }

        if (shadow_only) continue;

        /* Block write all RAMs in this stage execpt the spare with the
         * initial value.
         * Note that this should be skipped in fast-reconfig mode because the
         * entire shadow will be downloaded later.
         * Note that this should be skipped in hitless HA mode because the table
         * will be rewritten as part of push-delta-changes. */
        int ram_idx;
        if (!pipe_mgr_is_device_locked(t->dev)) {
          for (ram_idx = 0; ram_idx < t->stages[s].num_rams; ++ram_idx) {
            x = pipe_mgr_phy_mem_map_download_one_block(
                t->dev,
                t->direction,
                pipe_id,
                stage,
                pipe_mem_type_unit_ram,
                t->stages[s].ram_ids[ram_idx]);
            PIPE_MGR_DBGCHK(PIPE_SUCCESS == x);
          }
        }
      }
    }
  }

  /* We do not write the mapRAMs or FIFO pointers during hitless HA, for cold
   * boot, fast reconfig, and fast reconfig quick this is required though.
   * During hitless HA the hardware is already initialized and traffic is
   * flowing where as for cold boot and fast reconfig the hardware is reset and
   * must be fully initialized. */
  if (!shadow_only && !pipe_mgr_hitless_warm_init_in_progress(t->dev)) {
    /* Build a bit mask of pipes for the block write. */
    bf_dev_pipe_t p = 0;
    int pipe_mask = 0;
    PIPE_BITMAP_ITER(&t->pipes, p) { pipe_mask |= 1 << p; }
    for (s = 0; s < t->num_stages; ++s) {
      int stage = t->stages[s].stage_id;
      /* Block write MapRAM associated with RAM.  Fill it with the VPN
       * for the associated RAM. */
      int ram_idx;
      for (ram_idx = 0; ram_idx < t->stages[s].num_rams; ++ram_idx) {
        uint64_t map_phy_addr = 0;
        uint8_t map_ram_id =
            dev_cfg->map_ram_from_unit_ram(t->stages[s].ram_ids[ram_idx]);
        map_phy_addr = dev_cfg->get_full_phy_addr(
            t->direction, 0, stage, map_ram_id, 0, pipe_mem_type_map_ram);
        uint8_t map_ram_data[4] = {
            ~t->stages[s].ram_vpn[ram_idx] & 0x3F, 0, 0, 0};
        x = pipe_mgr_drv_blk_wr_data(&sess_hdl,
                                     t->dev_info,
                                     4,
                                     TOF_MAP_RAM_UNIT_DEPTH,
                                     1,
                                     map_phy_addr,
                                     pipe_mask,
                                     map_ram_data);
        PIPE_MGR_DBGCHK(PIPE_SUCCESS == x);
      }
      /* Block write the map RAM for the spare RAM to zeros. */
      for (ram_idx = 0; ram_idx < t->stages[s].num_spare_rams; ++ram_idx) {
        uint64_t map_phy_addr = 0;
        mem_id_t map_ram_id =
            dev_cfg->map_ram_from_unit_ram(t->stages[s].spare_rams[ram_idx]);
        map_phy_addr = dev_cfg->get_full_phy_addr(
            t->direction, 0, stage, map_ram_id, 0, pipe_mem_type_map_ram);
        uint8_t map_ram_data[4] = {0, 0, 0, 0};
        x = pipe_mgr_drv_blk_wr_data(&sess_hdl,
                                     t->dev_info,
                                     4,
                                     TOF_MAP_RAM_UNIT_DEPTH,
                                     1,
                                     map_phy_addr,
                                     pipe_mask,
                                     map_ram_data);
        PIPE_MGR_DBGCHK(PIPE_SUCCESS == x);
      }
    }
    /* Initialize the log/push/pop counters if the table uses them. */
    if (t->tbl_type == pipe_mgr_stful_tbl_type_fifo) {
      uint32_t addr = 0;
      uint32_t value = 0;
      switch (t->dev_info->dev_family) {
        case BF_DEV_FAMILY_TOFINO2:
          addr = offsetof(tof2_reg,
                          pipes[0]
                              .mau[t->stages[0].stage_id]
                              .rams.match.merge.mau_stateful_log_counter_clear);
          value = 1 << t->cntr_idx;
          break;
        case BF_DEV_FAMILY_TOFINO3:
          addr = offsetof(tof3_reg,
                          pipes[0]
                              .mau[t->stages[0].stage_id]
                              .rams.match.merge.mau_stateful_log_counter_clear);
          value = 1 << t->cntr_idx;
          break;
        default:
          PIPE_MGR_DBGCHK(0);
          return;
      }
      pipe_instr_write_reg_t instr;
      construct_instr_reg_write(t->dev, &instr, addr, value);
      x = pipe_mgr_drv_ilist_add(&sess_hdl,
                                 t->dev_info,
                                 &t->pipes,
                                 t->stages[0].stage_id,
                                 (uint8_t *)&instr,
                                 sizeof instr);
      if (PIPE_SUCCESS != x) {
        LOG_ERROR(
            "Failed to post log-counter init instr, dev %d tbl 0x%x status %s",
            t->dev,
            t->hdl,
            pipe_str_err(x));
        PIPE_MGR_DBGCHK(x == PIPE_SUCCESS);
        return;
      }
    }
  }
}

static pipe_status_t reset_table(pipe_sess_hdl_t sess_hdl,
                                 struct pipe_mgr_stful_tbl *t,
                                 bf_dev_pipe_t pipe,
                                 pipe_stful_mem_spec_t *stful_spec,
                                 int start_row,
                                 int end_row,
                                 int start_stage_idx,
                                 int end_stage_idx,
                                 bool shadow_only) {
  pipe_status_t sts = PIPE_SUCCESS;
  mem_id_t mem_id = 0;
  uint32_t ram_line_num = 0;
  if (t->skip_shadow) return sts;

  /* Build one RAM row worth of data based on the table's initial value or
   * based on the requested spec. */
  uint32_t payload[4] = {0};
  stful_data_encode_row(table_width(t),
                        stful_spec ? stful_spec : &t->initial_value,
                        (uint8_t *)payload);

  /* Build a pipe bit mask either for all pipes the table is in or for the
   * requested pipe. */
  pipe_bitmap_t *pbm = &t->pipes;
  if (BF_DEV_PIPE_ALL != pipe) {
    PIPE_MGR_DBGCHK(pipe < t->dev_info->num_active_pipes);
  }

  int entry_width = table_bit_width(t);
  if (!entry_width) {
    LOG_ERROR(
        "%s:%d Entry width for table 0x%x is 0", __func__, __LINE__, t->hdl);
    return PIPE_UNEXPECTED;
  }
  /* Update the shadow for every entry in the table. */
  int entries_per_row = TOF_SRAM_UNIT_WIDTH / entry_width;
  int s = 0;
  for (s = start_stage_idx; s < t->num_stages && s <= end_stage_idx; ++s) {
    int stage = t->stages[s].stage_id;
    int row = 0;
    for (row = start_row; row <= end_row; ++row) {
      get_shadow_params(t, row * entries_per_row, s, &ram_line_num, &mem_id);
      bf_dev_pipe_t p;
      PIPE_BITMAP_ITER(pbm, p) {
        sts = pipe_mgr_phy_mem_map_write(t->dev,
                                         t->direction,
                                         p,
                                         stage,
                                         pipe_mem_type_unit_ram,
                                         mem_id,
                                         ram_line_num,
                                         (uint8_t *)payload,
                                         NULL);
        /* For indirect tables update our shadow on writes. */
        if (!t->direct) {
          uint8_t *ram_line = NULL;
          ram_line = t->stages[s].db[p] + 16 * row;
          stful_data_encode_row(table_width(t),
                                stful_spec ? stful_spec : &t->initial_value,
                                ram_line);
        }

        if (PIPE_SUCCESS != sts) {
          LOG_ERROR("%s:%d Error in shadow update, err %s",
                    __func__,
                    __LINE__,
                    pipe_str_err(sts));
          return sts;
        }
      }
    }
  }

  if (shadow_only) return sts;

  /* For each stage requested perform a 128 bit virtual write for each row of
   * each RAM used by the table. */
  pipe_instr_set_memdata_v_t instr;
  uint32_t instr_size = sizeof(instr);
  for (s = start_stage_idx; s < t->num_stages && s <= end_stage_idx; ++s) {
    int stage = t->stages[s].stage_id;
    int lt = t->stages[s].logical_table;
    int row;
    for (row = start_row; row <= end_row; ++row) {
      int i = row * entries_per_row;
      int ram = 0, line = 0;
      decompose_index(table_width(t), i, &ram, &line, NULL);
      uint32_t vpn = t->stages[s].ram_vpn[ram];
      uint32_t vaddr = assemble_vaddr(vpn, line, 0xF);
      construct_instr_set_v_memdata(t->dev,
                                    &instr,
                                    (uint8_t *)payload,
                                    16,
                                    lt,
                                    pipe_virt_mem_type_sel_stful,
                                    vaddr);
      sts = pipe_mgr_drv_ilist_add(
          &sess_hdl, t->dev_info, pbm, stage, (uint8_t *)&instr, instr_size);
      if (PIPE_SUCCESS != sts) {
        LOG_ERROR("%s:%d Error in ilist add, err %s",
                  __func__,
                  __LINE__,
                  pipe_str_err(sts));
        return sts;
      }
    }
  }
  return sts;
}

pipe_status_t pipe_mgr_stful_table_reset(pipe_sess_hdl_t sess_hdl,
                                         bf_dev_target_t dev_tgt,
                                         pipe_stful_tbl_hdl_t tbl_hdl,
                                         pipe_stful_mem_spec_t *stful_spec) {
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev_tgt.device_id, tbl_hdl);
  if (!t) return PIPE_INVALID_ARG;

  int entry_width = table_bit_width(t);
  if (!entry_width) {
    LOG_ERROR(
        "%s:%d Entry width for table 0x%x is 0", __func__, __LINE__, t->hdl);
    return PIPE_UNEXPECTED;
  }

  int entries_per_row = TOF_SRAM_UNIT_WIDTH / entry_width;

  pipe_status_t sts = PIPE_SUCCESS;
  /* Assume that both direct and indirect tables can be of different sizes in
   * each stage table. */
  for (int s = 0; s < t->num_stages && sts == PIPE_SUCCESS; ++s) {
    int rows =
        (t->stages[s].num_entries + entries_per_row - 1) / entries_per_row;
    sts = reset_table(
        sess_hdl, t, dev_tgt.dev_pipe_id, stful_spec, 0, rows - 1, s, s, false);
  }
  return sts;
}

static pipe_status_t range_write_one_stage(pipe_sess_hdl_t sess_hdl,
                                           struct pipe_mgr_stful_tbl *t,
                                           int stage_idx,
                                           bf_dev_target_t dev_tgt,
                                           pipe_stful_mem_spec_t *stful_spec,
                                           int start_idx,
                                           int num_indices) {
  /* Divide the write into three sections.  Section one is a partial row from
   * the middle of a row to the end of the row.  Section two is a block of
   * rows.  Section three is a partial row from the begining to the middle. */
  int start = start_idx;
  if (!t || !stful_spec) {
    LOG_ERROR("%s:%d Null arguments passed", __func__, __LINE__);
    return PIPE_INVALID_ARG;
  }
  int entry_width = table_bit_width(t);
  if (!entry_width) {
    LOG_ERROR(
        "%s:%d Entry width for table 0x%x is 0", __func__, __LINE__, t->hdl);
    return PIPE_UNEXPECTED;
  }
  int per_row = TOF_SRAM_UNIT_WIDTH / entry_width;
  int s1 = (start % per_row) ? per_row - (start % per_row) : 0;
  s1 = s1 > num_indices ? num_indices : s1;
  int s2 = ((num_indices - s1) / per_row) * per_row;
  int s3 = num_indices - s1 - s2;
  PIPE_MGR_DBGCHK((s1 + s2 + s3) == num_indices);
  int start_row = (start + s1) / per_row;
  int end_row = start_row + (s2 / per_row) - 1;

  pipe_status_t sts = PIPE_SUCCESS;
  int i;
  for (i = 0; i < s1 && PIPE_SUCCESS == sts; ++start, ++i) {
    sts = write_one_stage(sess_hdl, dev_tgt, t, stage_idx, start, stful_spec);
  }
  if (PIPE_SUCCESS == sts && s2) {
    sts = reset_table(sess_hdl,
                      t,
                      dev_tgt.dev_pipe_id,
                      stful_spec,
                      start_row,
                      end_row,
                      stage_idx,
                      stage_idx,
                      false);
  }
  start += s2;
  for (i = 0; i < s3 && PIPE_SUCCESS == sts; ++start, ++i) {
    sts = write_one_stage(sess_hdl, dev_tgt, t, stage_idx, start, stful_spec);
  }
  return sts;
}

pipe_status_t pipe_mgr_stful_table_reset_range(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_target_t dev_tgt,
    pipe_stful_tbl_hdl_t tbl_hdl,
    pipe_stful_mem_idx_t stful_ent_idx,
    uint32_t num_indices_,
    pipe_stful_mem_spec_t *stful_spec) {
  uint32_t num_indices = num_indices_;
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev_tgt.device_id, tbl_hdl);
  if (!t) return PIPE_INVALID_ARG;
  if (!pipe_mgr_valid_dev_tgt(dev_tgt, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }
  if (!is_valid_index(t, stful_ent_idx, __func__, __LINE__))
    return PIPE_INVALID_ARG;
  if (!num_indices) return PIPE_INVALID_ARG;
  if (t->direct) return PIPE_INVALID_ARG;

  stful_spec = stful_spec ? stful_spec : &t->initial_value;
  num_indices =
      num_indices > t->num_entries_real ? t->num_entries_real : num_indices;
  num_indices = stful_ent_idx + num_indices <= t->num_entries_real
                    ? num_indices
                    : t->num_entries_real - stful_ent_idx;

  /* Determine which stages contain the index range and break the writes out to
   * each stage. */
  uint32_t stage_start_idx = 0;
  for (int s = 0; s < t->num_stages; ++s) {
    uint32_t stage_end_idx = stage_start_idx + t->stages[s].num_entries - 1;
    /* Is the starting index in this stage?  If so write the stage. */
    if (stful_ent_idx >= stage_start_idx && stful_ent_idx <= stage_end_idx) {
      /* Don't write more than this stage contains. */
      uint32_t num_to_write = num_indices < t->stages[s].num_entries
                                  ? num_indices
                                  : t->stages[s].num_entries;
      /* Don't write past the end of the stage either. */
      if ((stful_ent_idx + num_to_write - 1) > stage_end_idx) {
        num_to_write = stage_end_idx - stful_ent_idx + 1;
      }
      /* Write the stage. */
      pipe_status_t sts = range_write_one_stage(sess_hdl,
                                                t,
                                                s,
                                                dev_tgt,
                                                stful_spec,
                                                stful_ent_idx - stage_start_idx,
                                                num_to_write);
      if (PIPE_SUCCESS != sts) {
        LOG_ERROR(
            "Reset range failed. Dev %d pipe %x tbl 0x%x stg %d idx %d count "
            "%d, sts %s",
            t->dev,
            dev_tgt.dev_pipe_id,
            t->hdl,
            t->stages[s].stage_id,
            stful_ent_idx - stage_start_idx,
            num_to_write,
            pipe_str_err(sts));
        return sts;
      }

      /* Update our write index and number to write to account for what was just
       * written. */
      stful_ent_idx += num_to_write;
      PIPE_MGR_DBGCHK(num_indices >= num_to_write);
      num_indices -= num_to_write;
      if (!num_indices) break;
    }

    stage_start_idx = stage_end_idx + 1;
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stful_fifo_occupancy(pipe_sess_hdl_t sess_hdl,
                                            dev_target_t dev_tgt,
                                            pipe_stful_tbl_hdl_t tbl_hdl,
                                            int *occupancy) {
  (void)sess_hdl;
  if (!occupancy) return PIPE_INVALID_ARG;
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev_tgt.device_id, tbl_hdl);
  if (!t) return PIPE_INVALID_ARG;
  if (t->tbl_type != pipe_mgr_stful_tbl_type_fifo) {
    LOG_ERROR("%s called for dev %d table 0x%x of type %s, not a FIFO",
              __func__,
              t->dev,
              t->hdl,
              pipe_mgr_stful_tbl_type_str(t->tbl_type));
    return PIPE_INVALID_ARG;
  }

  int stage = t->stages[0].stage_id;
  int cntr_idx = t->cntr_idx;
  bf_subdev_id_t subdev = 0;

  struct pipe_mgr_stful_tbl_inst *stbl_inst = NULL;
  stbl_inst = get_stful_tbl_inst_by_pipe_id(t, dev_tgt.dev_pipe_id);
  if (!stbl_inst) {
    LOG_ERROR("%s:%d Stful table inst lookup failure", __func__, __LINE__);
    return PIPE_INVALID_ARG;
  }
  unsigned int log_pipe_id;
  int min_value = -1;
  PIPE_BITMAP_ITER(&stbl_inst->pipe_bmp, log_pipe_id) {
    bf_dev_pipe_t phy_pipe_id;
    pipe_mgr_map_pipe_id_log_to_phy(t->dev_info, log_pipe_id, &phy_pipe_id);
    uint32_t value = 0;
    uint32_t addr = 0;
    switch (t->dev_info->dev_family) {
      case BF_DEV_FAMILY_TOFINO2:
        addr = offsetof(
            tof2_reg,
            pipes[phy_pipe_id]
                .mau[stage]
                .rams.match.merge.mau_stateful_log_fifo_level[cntr_idx]);
        break;
      case BF_DEV_FAMILY_TOFINO3:
        subdev = phy_pipe_id / BF_SUBDEV_PIPE_COUNT;
        addr = offsetof(
            tof3_reg,
            pipes[phy_pipe_id % BF_SUBDEV_PIPE_COUNT]
                .mau[stage]
                .rams.match.merge.mau_stateful_log_fifo_level[cntr_idx]);
        break;
      default:
        PIPE_MGR_DBGCHK(0);
        return PIPE_UNEXPECTED;
    }
    lld_subdev_read_register(t->dev, subdev, addr, &value);
    LOG_TRACE("Dev %d FIFO-Table 0x%x Pipe %d has occupancy %d",
              t->dev,
              t->hdl,
              log_pipe_id,
              value);
    if (min_value == -1 || (uint32_t)min_value > value) {
      min_value = value;
    }
  }

  *occupancy = min_value == -1 ? 0 : min_value;
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stful_fifo_reset(pipe_sess_hdl_t sess_hdl,
                                        dev_target_t dev_tgt,
                                        pipe_stful_tbl_hdl_t tbl_hdl) {
  bf_dev_id_t dev = dev_tgt.device_id;
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev, tbl_hdl);
  if (!t) {
    LOG_ERROR(
        "Cannot find table 0x%x on dev %d for stful fifo reset", tbl_hdl, dev);
    return PIPE_INVALID_ARG;
  }

  pipe_bitmap_t pipes = {{0}};
  PIPE_BITMAP_INIT(&pipes, PIPE_BMP_SIZE);
  if (dev_tgt.dev_pipe_id == BF_DEV_PIPE_ALL) {
    PIPE_BITMAP_ASSIGN(&pipes, &t->pipes);
  } else {
    if (!table_is_in_pipe(t, dev_tgt.dev_pipe_id)) return PIPE_INVALID_ARG;
    struct pipe_mgr_stful_tbl_inst *stbl_inst = NULL;
    stbl_inst = get_stful_tbl_inst_by_pipe_id(t, dev_tgt.dev_pipe_id);
    if (!stbl_inst) {
      LOG_ERROR("%s:%d Stful table inst lookup failure", __func__, __LINE__);
      return PIPE_INVALID_ARG;
    }
    PIPE_BITMAP_ASSIGN(&pipes, &stbl_inst->pipe_bmp);
  }

  uint32_t addr = 0;
  uint32_t value = 0;
  switch (t->dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO2:
      addr = offsetof(tof2_reg,
                      pipes[0]
                          .mau[t->stages[0].stage_id]
                          .rams.match.merge.mau_stateful_log_counter_clear);
      value = 1 << t->cntr_idx;
      break;
    case BF_DEV_FAMILY_TOFINO3:
      addr = offsetof(tof3_reg,
                      pipes[0]
                          .mau[t->stages[0].stage_id]
                          .rams.match.merge.mau_stateful_log_counter_clear);
      value = 1 << t->cntr_idx;
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }
  pipe_instr_write_reg_t instr;
  construct_instr_reg_write(t->dev, &instr, addr, value);
  pipe_status_t sts = pipe_mgr_drv_ilist_add(&sess_hdl,
                                             t->dev_info,
                                             &pipes,
                                             t->stages[0].stage_id,
                                             (uint8_t *)&instr,
                                             sizeof instr);
  if (PIPE_SUCCESS != sts) {
    LOG_ERROR(
        "Failed to post log-counter init instr, dev %d tbl 0x%x status %s",
        t->dev,
        t->hdl,
        pipe_str_err(sts));
    PIPE_MGR_DBGCHK(sts == PIPE_SUCCESS);
    return sts;
  }
  unsigned int p = 0;
  PIPE_BITMAP_ITER(&pipes, p) { t->cntr_val[p] = 0; }

  return PIPE_SUCCESS;
}

struct pipe_mgr_stful_fifo_deq_data {
  struct pipe_mgr_stful_tbl *t;
  bf_dev_pipe_t log_pipe;
  pipe_stful_mem_spec_t *values;
  int num_values;
  int skip;
};
void pipe_mgr_stful_fifo_deq_cb(pipe_mgr_drv_buf_t *b,
                                uint32_t offset,
                                uint32_t entry_count,
                                bool is_errored,
                                void *user_data) {
  (void)is_errored;
  struct pipe_mgr_stful_fifo_deq_data *deq_data =
      (struct pipe_mgr_stful_fifo_deq_data *)user_data;

  /* We may need to skip a few entries in the first line since we read the
   * entire 16B line.  This only needs to happen on the data in the first buffer
   * so only skip if the offset is zero. */
  int skip = offset == 0 ? deq_data->skip : 0;

  /* We are reading deq_data->num_values in total but not all of them are
   * guarenteed to be in this buffer.  There are offset number of lines in
   * earlier buffer so factor that in when deciding how much data to take from
   * this buffer. */
  int entry_width = table_bit_width(deq_data->t);
  if (!entry_width) {
    LOG_ERROR("%s:%d Entry width for table 0x%x is 0",
              __func__,
              __LINE__,
              deq_data->t->hdl);
    return;
  }
  int entries_per_line = 128 / entry_width;
  int already_seen = offset * entries_per_line;
  int remaining = deq_data->num_values - already_seen;
  int available_now = entry_count * entries_per_line;
  int stop_at = remaining < available_now ? remaining : available_now;
  int read_so_far = 0;
  for (int i = 0; i < (int)entry_count; ++i) {
    for (int j = skip; j < entries_per_line; ++j) {
      stful_data_decode(table_width(deq_data->t),
                        j,
                        deq_data->values + already_seen + read_so_far,
                        b->addr + 16 * i);
      ++read_so_far;
      if (read_so_far >= stop_at) break;
    }
    skip = 0;
    if (read_so_far >= stop_at) break;
  }
}

pipe_status_t pipe_mgr_stful_fifo_dequeue(pipe_sess_hdl_t sess_hdl,
                                          dev_target_t dev_tgt,
                                          pipe_stful_tbl_hdl_t tbl_hdl,
                                          int num_to_dequeue_requested,
                                          pipe_stful_mem_spec_t *values,
                                          int *num_dequeued) {
  int num_to_dequeue = num_to_dequeue_requested;
  if (!values || !num_dequeued) return PIPE_INVALID_ARG;
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev_tgt.device_id, tbl_hdl);
  if (!t) return PIPE_INVALID_ARG;
  if (t->tbl_type != pipe_mgr_stful_tbl_type_fifo) {
    LOG_ERROR("%s called for dev %d table 0x%x of type %s, not a FIFO",
              __func__,
              t->dev,
              t->hdl,
              pipe_mgr_stful_tbl_type_str(t->tbl_type));
    return PIPE_INVALID_ARG;
  }
  if (dev_tgt.dev_pipe_id == BF_DEV_PIPE_ALL) {
    if (1 != PIPE_BITMAP_COUNT(&t->pipes)) {
      LOG_ERROR("%s called for multiple pipes on dev %d table 0x%x",
                __func__,
                t->dev,
                t->hdl);
      return PIPE_INVALID_ARG;
    }
  }

  /* Read cntr to check number of entries in the FIFO. */
  bf_dev_pipe_t log_pipe_id = dev_tgt.dev_pipe_id;
  bf_dev_pipe_t phy_pipe_id;
  pipe_mgr_map_pipe_id_log_to_phy(t->dev_info, log_pipe_id, &phy_pipe_id);
  uint32_t value = 0;
  uint32_t addr = 0;
  bf_subdev_id_t subdev = 0;
  switch (t->dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO2:
      addr = offsetof(
          tof2_reg,
          pipes[phy_pipe_id]
              .mau[t->stages[0].stage_id]
              .rams.match.merge.mau_stateful_log_fifo_level[t->cntr_idx]);
      break;
    case BF_DEV_FAMILY_TOFINO3:
      subdev = phy_pipe_id / BF_SUBDEV_PIPE_COUNT;
      addr = offsetof(
          tof3_reg,
          pipes[phy_pipe_id % BF_SUBDEV_PIPE_COUNT]
              .mau[t->stages[0].stage_id]
              .rams.match.merge.mau_stateful_log_fifo_level[t->cntr_idx]);
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }
  lld_subdev_read_register(t->dev, subdev, addr, &value);
  LOG_TRACE("Dev %d FIFO-Table 0x%x Pipe %d has occupancy %d",
            t->dev,
            t->hdl,
            log_pipe_id,
            value);
  *num_dequeued = 0;
  if (value == 0) return PIPE_SUCCESS;

  /* Set read count to min(num_to_dequeue, cntr) */
  if ((int)value > num_to_dequeue) num_to_dequeue = value;

  /* Count the number of read-block operations required. */
  int read_start = 0, read_stage_idx = 0,
      logical_start_idx = t->cntr_val[log_pipe_id];
  int read_count = num_to_dequeue;
  int num_blk_rds = 0;
  /* Note the <= is to allow us to read exactly one stage twice.  If we start
   * reading at the end of a stage we may end up reading that stage, then all
   * other stages, and then come back to the first stage to read a little in the
   * beginning of the first stage we read. */
  PIPE_MGR_DBGCHK(t->num_stages > 0);
  for (int i = 0; i <= t->num_stages; ++i) {
    log_index_to_stage_and_offset(
        t, logical_start_idx, &read_stage_idx, &read_start);
    int num_to_read = t->stages[read_stage_idx].num_entries - read_start;
    if (num_to_read > read_count) num_to_read = read_count;
    num_blk_rds += 1;
    read_count -= num_to_read;
    if (!read_count) break;
    logical_start_idx += num_to_read;
    if (logical_start_idx >= (int)t->num_entries_real) {
      logical_start_idx -= t->num_entries_real;
    }
  }

  LOG_TRACE(
      "Dev %d Fifo-Tbl 0x%x pipe %x dequeueing %d (requested %d) in %d ops",
      t->dev,
      t->hdl,
      log_pipe_id,
      num_to_dequeue,
      num_to_dequeue_requested,
      num_blk_rds);

  /* Allocate a bit of state to pass into the read block callbacks. */
  struct pipe_mgr_stful_fifo_deq_data deq_data[num_blk_rds];

  /* Issue the read-blocks. */
  int cntr_inc_val = num_to_dequeue;
  logical_start_idx = t->cntr_val[log_pipe_id];
  for (int i = 0; i < num_blk_rds; ++i) {
    log_index_to_stage_and_offset(
        t, logical_start_idx, &read_stage_idx, &read_start);
    int num_to_read = t->stages[read_stage_idx].num_entries - read_start;
    if (num_to_read > num_to_dequeue) num_to_read = num_to_dequeue;
    num_to_dequeue -= num_to_read;
    int last_idx_to_read = read_start + num_to_read - 1;

    /* Read num_to_read entries from this stage.  BUT, read them with 16B
     * virtual reads... */
    int entry_width = table_bit_width(t);
    if (!entry_width) {
      LOG_ERROR(
          "%s:%d Entry width for table 0x%x is 0", __func__, __LINE__, t->hdl);
      return PIPE_UNEXPECTED;
    }
    int entry_per_line = 128 / entry_width;
    int start_line = read_start / entry_per_line;
    int end_line = last_idx_to_read / entry_per_line;
    int num_lines = end_line - start_line + 1;
    int addr_incr = 32;

    /* Map the index to a RAM/VPN, and a row within the RAM and the build the
     * full virtual address. */
    int ram, line, vpn;
    int stage_id = t->stages[read_stage_idx].stage_id;
    decompose_index(table_width(t), read_start, &ram, &line, NULL);
    vpn = t->stages[read_stage_idx].ram_vpn[ram];
    uint32_t low_vir_addr = ram_row_to_cpu_rd_addr(vpn, line);
    pipe_full_virt_addr_t vaddr;
    vaddr.addr = 0;
    construct_full_virt_addr(t->dev_info,
                             &vaddr,
                             t->stages[read_stage_idx].logical_table,
                             pipe_virt_mem_type_sel_stful,
                             low_vir_addr,
                             phy_pipe_id,
                             stage_id);

    LOG_TRACE("Reading %d entries in %d lines from addr 0x%" PRIx64,
              num_to_read,
              num_lines,
              vaddr.addr);
    deq_data[i].t = t;
    deq_data[i].log_pipe = log_pipe_id;
    deq_data[i].values = values;
    deq_data[i].num_values = num_to_read;
    deq_data[i].skip = read_start % entry_per_line;
    values += num_to_read;
    pipe_mgr_drv_blk_rd(&sess_hdl,
                        t->dev,
                        16,
                        num_lines,
                        addr_incr,
                        vaddr.addr,
                        pipe_mgr_stful_fifo_deq_cb,
                        &deq_data[i]);

    if (!num_to_dequeue) break;
    logical_start_idx += num_to_read;
    if (logical_start_idx >= (int)t->num_entries_real) {
      logical_start_idx -= t->num_entries_real;
    }
  }

  /* Wait for read blocks to complete.  They must complete before sending the
   * pop instruction because they are on separate DMA channels and the pop
   * cannot happen before the reads otherwise the data may be overwritten. */
  pipe_mgr_drv_rd_blk_cmplt_all(sess_hdl, t->dev);

  /* Issue pop instruction to remove all the elements just read. */
  pipe_salu_cntr_instr_t instr;
  construct_instr_salu_cntr(t->dev, &instr, false, t->cntr_idx, cntr_inc_val);
  pipe_bitmap_t pipes = {{0}};
  PIPE_BITMAP_INIT(&pipes, PIPE_BMP_SIZE);
  PIPE_BITMAP_SET(&pipes, dev_tgt.dev_pipe_id);
  pipe_status_t sts = pipe_mgr_drv_ilist_add(&sess_hdl,
                                             t->dev_info,
                                             &pipes,
                                             t->stages[0].stage_id,
                                             (uint8_t *)&instr,
                                             sizeof instr);
  if (PIPE_SUCCESS != sts) {
    LOG_ERROR("%s:%d Error in ilist add, err %s",
              __func__,
              __LINE__,
              pipe_str_err(sts));
  } else {
    /* Increment our counter handling the wrap around case. */
    t->cntr_val[log_pipe_id] += cntr_inc_val;
    if (t->cntr_val[log_pipe_id] >= (int)t->num_entries_real)
      t->cntr_val[log_pipe_id] -= t->num_entries_real;
  }
  *num_dequeued = cntr_inc_val;

  return sts;
}

pipe_status_t pipe_mgr_stful_fifo_enqueue(pipe_sess_hdl_t sess_hdl,
                                          dev_target_t dev_tgt,
                                          pipe_stful_tbl_hdl_t tbl_hdl,
                                          int num_to_enqueue,
                                          pipe_stful_mem_spec_t *values) {
  pipe_status_t sts = PIPE_SUCCESS;

  if (!values || !num_to_enqueue) return PIPE_INVALID_ARG;
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev_tgt.device_id, tbl_hdl);
  if (!t) return PIPE_INVALID_ARG;
  if (t->tbl_type != pipe_mgr_stful_tbl_type_fifo) {
    LOG_ERROR("%s called for dev %d table 0x%x of type %s, not a FIFO",
              __func__,
              t->dev,
              t->hdl,
              pipe_mgr_stful_tbl_type_str(t->tbl_type));
    return PIPE_INVALID_ARG;
  }
  if (dev_tgt.dev_pipe_id == BF_DEV_PIPE_ALL) {
    if (1 != PIPE_BITMAP_COUNT(&t->pipes)) {
      LOG_ERROR("%s called for multiple pipes on dev %d table 0x%x",
                __func__,
                t->dev,
                t->hdl);
      return PIPE_INVALID_ARG;
    }
  }

  /* Read FIFO level to know current occupancy. */
  bf_dev_pipe_t log_pipe_id = dev_tgt.dev_pipe_id;
  bf_dev_pipe_t phy_pipe_id;
  pipe_mgr_map_pipe_id_log_to_phy(t->dev_info, log_pipe_id, &phy_pipe_id);
  uint32_t value = 0;
  uint32_t addr = 0;
  bf_subdev_id_t subdev = 0;
  switch (t->dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO2:
      addr = offsetof(
          tof2_reg,
          pipes[phy_pipe_id]
              .mau[t->stages[0].stage_id]
              .rams.match.merge.mau_stateful_log_fifo_level[t->cntr_idx]);
      break;
    case BF_DEV_FAMILY_TOFINO3:
      subdev = phy_pipe_id / BF_SUBDEV_PIPE_COUNT;
      addr = offsetof(
          tof3_reg,
          pipes[phy_pipe_id % BF_SUBDEV_PIPE_COUNT]
              .mau[t->stages[0].stage_id]
              .rams.match.merge.mau_stateful_log_fifo_level[t->cntr_idx]);
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }
  lld_subdev_read_register(t->dev, subdev, addr, &value);
  LOG_TRACE("Dev %d FIFO-Table 0x%x Pipe %d has occupancy %d",
            t->dev,
            t->hdl,
            log_pipe_id,
            value);

  /* Subract with size to know how much room there is to add new entries. */
  int remaining_capacity = t->num_entries_real - value;

  /* Check that num_to_enqueue entries will actually fit in the FIFO and fail
   * if they do not fit. */
  if (remaining_capacity < num_to_enqueue) {
    LOG_ERROR(
        "Dev %d pipe %d FIFO-Tbl 0x%x requested to install %d entries but only "
        "room for %d",
        t->dev,
        log_pipe_id,
        t->hdl,
        num_to_enqueue,
        remaining_capacity);
    return PIPE_INVALID_ARG;
  }
  int cntr_inc_val = num_to_enqueue;

  /* Perform the write operations. */
  int wr_start = 0, wr_stage_idx = 0,
      logical_start_idx = t->cntr_val[log_pipe_id];
  /* Note the <= is to allow us to write exactly one stage twice.  If we start
   * filling at the end of a stage we may end up writting that stage, then all
   * other stages, and then come back to the first stage to put a little in the
   * beginning of the first stage we wrote. */
  for (int i = 0; i <= t->num_stages; ++i) {
    log_index_to_stage_and_offset(
        t, logical_start_idx, &wr_stage_idx, &wr_start);
    int num_to_wr = t->stages[wr_stage_idx].num_entries - wr_start;
    if (num_to_wr > num_to_enqueue) num_to_wr = num_to_enqueue;
    num_to_enqueue -= num_to_wr;
    for (int j = 0; j < num_to_wr; ++j) {
      sts = write_one_stage(
          sess_hdl, dev_tgt, t, wr_stage_idx, wr_start + j, &values[j]);
      if (PIPE_SUCCESS != sts) {
        LOG_ERROR(
            "%s:%d Failed to write stage %d, dev %d table 0x%x pipe %x, err %s",
            __func__,
            __LINE__,
            t->stages[wr_stage_idx].stage_id,
            dev_tgt.device_id,
            t->hdl,
            dev_tgt.dev_pipe_id,
            pipe_str_err(sts));
        return sts;
      }
    }

    values += num_to_wr;
    logical_start_idx += num_to_wr;
    if (logical_start_idx >= (int)t->num_entries_real) {
      logical_start_idx -= t->num_entries_real;
    }
  }

  /* Issue the push counter to the first stage. */
  pipe_salu_cntr_instr_t instr;
  construct_instr_salu_cntr(t->dev, &instr, true, t->cntr_idx, cntr_inc_val);
  pipe_bitmap_t pipes = {{0}};
  PIPE_BITMAP_INIT(&pipes, PIPE_BMP_SIZE);
  PIPE_BITMAP_SET(&pipes, dev_tgt.dev_pipe_id);
  sts = pipe_mgr_drv_ilist_add(&sess_hdl,
                               t->dev_info,
                               &pipes,
                               t->stages[0].stage_id,
                               (uint8_t *)&instr,
                               sizeof instr);
  if (PIPE_SUCCESS != sts) {
    LOG_ERROR("%s:%d Error in ilist add, err %s",
              __func__,
              __LINE__,
              pipe_str_err(sts));
  } else {
    /* Increment our counter handling the wrap around case. */
    t->cntr_val[log_pipe_id] += cntr_inc_val;
    if (t->cntr_val[log_pipe_id] >= (int)t->num_entries_real)
      t->cntr_val[log_pipe_id] -= t->num_entries_real;
  }
  return sts;
}

pipe_status_t pipe_mgr_stful_process_op_list(pipe_sess_hdl_t sess_hdl,
                                             bf_dev_id_t dev,
                                             pipe_tbl_hdl_t tbl_hdl,
                                             struct pipe_mgr_stful_op_list_t *l,
                                             uint32_t *num_processed) {
  pipe_status_t sts = PIPE_SUCCESS;
  for (; l && PIPE_SUCCESS == sts; l = l->next) {
    switch (l->op) {
      case PIPE_MGR_STFUL_TBL_IDX_WR:
        sts = do_word_write(sess_hdl, dev, tbl_hdl, l);
        break;
      case PIPE_MGR_STFUL_TBL_ENT_RST:
        sts = do_direct_word_reset(sess_hdl, dev, l);
        break;
      default:
        sts = PIPE_UNEXPECTED;
        break;
    }
    if (PIPE_SUCCESS == sts) (*num_processed)++;
  }
  return sts;
}

struct pipe_mgr_stful_direct_ent_sync_cookie {
  struct pipe_mgr_stful_tbl *t;
  pipe_mat_ent_hdl_t ent_hdl;
  uint32_t index;
};
struct pipe_mgr_stful_direct_tbl_sync_cookie {
  struct pipe_mgr_stful_tbl *t;
  int num_hdls;
  /* Arrays sized by num_hdls holding the entry handle, the first pipe of the
   * scope it is in, the stage (not stage-index) it is in, and the index within
   * the stage. */
  pipe_mat_ent_hdl_t *ent_hdls;
  bf_dev_pipe_t *ent_pipes;
  dev_stage_t *ent_stages;
  uint32_t *ent_index;
  /* Array of maps, indexed by log-pipe, mapping keys of stage-id+ram line to
   * the ram line data.  Always sized by num-active-pipes. */
  bf_map_t *data;
};

static void rd_one_direct_cb(void *cookie,
                             bf_dev_id_t dev,
                             bf_dev_pipe_t log_pipe,
                             uint32_t byte_offset,
                             uint8_t *buf,
                             uint32_t buf_sz,
                             bool all_data_complete,
                             bool user_cb_safe) {
  (void)byte_offset;
  struct pipe_mgr_stful_direct_ent_sync_cookie *c = cookie;
  struct pipe_mgr_stful_tbl *t = c->t;
  if (buf_sz) {
    /* Lookup the entry in the map.  It should exist for direct entries since
     * there is always an initial spec provided. */
    pipe_stful_mem_spec_t *shdw_spec = NULL;
    bf_map_sts_t msts =
        bf_map_get(&t->hdl_to_shdw[log_pipe], c->ent_hdl, (void **)&shdw_spec);
    if (msts != BF_MAP_OK || !shdw_spec) return;

    /* Convert the ram line to a stateful spec. */
    stful_data_decode(table_width(t), c->index, shdw_spec, buf);
    LOG_TRACE("Dev %d tbl 0x%x sync entry %u in pipe %d, spec:",
              t->dev,
              t->hdl,
              c->ent_hdl,
              log_pipe);
    switch (table_width(t)) {
      case STFL_WDTH_1:
        LOG_TRACE("%d", shdw_spec->bit);
        break;
      case STFL_WDTH_8:
        LOG_TRACE("0x%x", shdw_spec->byte);
        break;
      case STFL_WDTH_8x2:
        LOG_TRACE("Hi: 0x%x Lo: 0x%x",
                  shdw_spec->dbl_byte.hi,
                  shdw_spec->dbl_byte.lo);
        break;
      case STFL_WDTH_16:
        LOG_TRACE("0x%x", shdw_spec->half);
        break;
      case STFL_WDTH_16x2:
        LOG_TRACE("Hi: 0x%x Lo: 0x%x",
                  shdw_spec->dbl_half.hi,
                  shdw_spec->dbl_half.lo);
        break;
      case STFL_WDTH_32:
        LOG_TRACE("0x%x", shdw_spec->word);
        break;
      case STFL_WDTH_32x2:
        LOG_TRACE("Hi: 0x%x Lo: 0x%x",
                  shdw_spec->dbl_word.hi,
                  shdw_spec->dbl_word.lo);
        break;
      case STFL_WDTH_64:
        LOG_TRACE("0x%" PRIx64, shdw_spec->dbl);
        break;
      case STFL_WDTH_64x2:
        LOG_TRACE("Hi: 0x%" PRIx64 " Lo: 0x%" PRIx64,
                  shdw_spec->dbl_dbl.hi,
                  shdw_spec->dbl_dbl.lo);
        break;
    }
  }
  if (all_data_complete) {
    LOG_TRACE("StfulDirEntSync: dev %d tbl 0x%x ent %d complete",
              dev,
              t->hdl,
              c->ent_hdl);
  } else if (user_cb_safe) {
    PIPE_MGR_FREE(cookie);
  }
}
static void rd_all_direct_cb(void *cookie,
                             bf_dev_id_t dev,
                             bf_dev_pipe_t log_pipe,
                             uint32_t byte_offset,
                             uint8_t *buf,
                             uint32_t buf_sz,
                             bool all_data_complete,
                             bool user_cb_safe) {
  struct pipe_mgr_stful_direct_tbl_sync_cookie *sc = cookie;
  LOG_TRACE("Dev %d Pipe %d Sz %u Offset %u all-data %d cb-safe %d",
            dev,
            log_pipe,
            buf_sz,
            byte_offset,
            all_data_complete,
            user_cb_safe);
  if (user_cb_safe) {
    struct pipe_mgr_stful_tbl *t = sc->t;
    /* Clear sync state in the stful table. */
    pipe_stful_tbl_sync_cback_fn sync_cb = t->sync_cb;
    void *sync_cookie = t->sync_cookie;
    t->sync_cb = NULL;
    t->sync_cookie = NULL;
    /* Free the syncing-cookie. */
    PIPE_MGR_FREE(sc->ent_hdls);
    PIPE_MGR_FREE(sc->ent_pipes);
    PIPE_MGR_FREE(sc->ent_stages);
    PIPE_MGR_FREE(sc->ent_index);
    for (unsigned p = 0; p < t->dev_info->num_active_pipes; ++p) {
      bf_map_t *m = sc->data + p;
      unsigned long key;
      void *data = NULL;
      while (BF_MAP_OK == bf_map_get_first_rmv(m, &key, &data)) {
        if (data) PIPE_MGR_FREE(data);
      }
      bf_map_destroy(m);
    }
    PIPE_MGR_FREE(sc->data);
    PIPE_MGR_FREE(sc);

    /* Call the user's callback now that the table sync has completed. */
    if (sync_cb) sync_cb(t->dev, sync_cookie);
    return;
  } else if (all_data_complete) {
    for (int i = 0; i < sc->num_hdls; ++i) {
      /* Find the table instance for the entry so we know which pipes it lives
       * in. */
      struct pipe_mgr_stful_tbl_inst *inst = NULL;
      inst = get_stful_tbl_inst_by_pipe_id(sc->t, sc->ent_pipes[i]);
      if (!inst) {
        LOG_ERROR("Dev %d tbl 0x%x cannot find pipe instance %x for entry %d",
                  dev,
                  sc->t->hdl,
                  sc->ent_pipes[i],
                  sc->ent_hdls[i]);
        return;
      }
      /* For each pipe, get the ram line data and update our shadow. */
      int entry_width = table_bit_width(sc->t);
      if (!entry_width) {
        LOG_ERROR("%s:%d Entry width for table 0x%x is 0",
                  __func__,
                  __LINE__,
                  sc->t->hdl);
        return;
      }
      int entries_per_row = TOF_SRAM_UNIT_WIDTH / entry_width;
      unsigned long key = sc->ent_stages[i];
      key = (key << 16) | sc->ent_index[i] / entries_per_row;
      bf_dev_pipe_t p;
      PIPE_BITMAP_ITER(&inst->pipe_bmp, p) {
        uint8_t *ram_line = NULL;
        bf_map_sts_t msts = bf_map_get(&sc->data[p], key, (void **)&ram_line);
        if (BF_MAP_OK != msts || !ram_line) {
          LOG_ERROR(
              "Dev %d tbl 0x%x sync, no data for pipe %d stage %d entry %d, "
              "msts %d",
              sc->t->dev,
              sc->t->hdl,
              p,
              sc->ent_stages[i],
              sc->ent_index[i],
              msts);
          return;
        }
        pipe_stful_mem_spec_t *shdw_spec = NULL;
        msts = bf_map_get(
            &sc->t->hdl_to_shdw[p], sc->ent_hdls[i], (void **)&shdw_spec);
        if (BF_MAP_OK != msts || !shdw_spec) {
          LOG_ERROR(
              "Dev %d tbl 0x%x sync, no shadow spec for pipe %d entry %d, msts "
              "%d",
              sc->t->dev,
              sc->t->hdl,
              p,
              sc->ent_hdls[i],
              msts);
          return;
        }
        stful_data_decode(
            table_width(sc->t), sc->ent_index[i], shdw_spec, ram_line);
        LOG_TRACE("Dev %d tbl 0x%x sync entry %d in pipe %d stage %d, spec:",
                  sc->t->dev,
                  sc->t->hdl,
                  sc->ent_index[i],
                  sc->ent_pipes[i],
                  sc->ent_stages[i]);
        switch (table_width(sc->t)) {
          case STFL_WDTH_1:
            LOG_TRACE("%d", shdw_spec->bit);
            break;
          case STFL_WDTH_8:
            LOG_TRACE("0x%x", shdw_spec->byte);
            break;
          case STFL_WDTH_8x2:
            LOG_TRACE("Hi: 0x%x Lo: 0x%x",
                      shdw_spec->dbl_byte.hi,
                      shdw_spec->dbl_byte.lo);
            break;
          case STFL_WDTH_16:
            LOG_TRACE("0x%x", shdw_spec->half);
            break;
          case STFL_WDTH_16x2:
            LOG_TRACE("Hi: 0x%x Lo: 0x%x",
                      shdw_spec->dbl_half.hi,
                      shdw_spec->dbl_half.lo);
            break;
          case STFL_WDTH_32:
            LOG_TRACE("0x%x", shdw_spec->word);
            break;
          case STFL_WDTH_32x2:
            LOG_TRACE("Hi: 0x%x Lo: 0x%x",
                      shdw_spec->dbl_word.hi,
                      shdw_spec->dbl_word.lo);
            break;
          case STFL_WDTH_64:
            LOG_TRACE("0x%" PRIx64, shdw_spec->dbl);
            break;
          case STFL_WDTH_64x2:
            LOG_TRACE("Hi: 0x%" PRIx64 " Lo: 0x%" PRIx64,
                      shdw_spec->dbl_dbl.hi,
                      shdw_spec->dbl_dbl.lo);
            break;
        }
      }
    }
  } else {
    /* Get the map of locations for this pipe. */
    bf_map_t *m = &sc->data[log_pipe];
    /* Iterate over the keys in the map and populate their data. */
    uint8_t *ram_line = NULL;
    unsigned long key = 0;
    uint32_t offset = 0;
    uint32_t consumed = 0;
    for (bf_map_sts_t msts = bf_map_get_first(m, &key, (void **)&ram_line);
         msts == BF_MAP_OK;
         msts = bf_map_get_next(m, &key, (void **)&ram_line)) {
      /* If the read was broken into multiple DMA buffers then this callback is
       * invoked multiple times, the byte_offset will tell us where to resume
       * processing the data.  Each read is always 16 bytes. */
      if (offset < byte_offset) {
        /* This key was already processed with data from a previous CB. */
        offset += 16;
        continue;
      } else if (consumed >= buf_sz) {
        /* Processed all data from this CB we've done as much as possible. */
        return;
      }
      PIPE_MGR_MEMCPY(ram_line, buf + consumed, 16);
      consumed += 16;
    }
    return;
  }
}
static void rd_one_cb(void *cookie,
                      bf_dev_id_t dev,
                      bf_dev_pipe_t pipe,
                      uint32_t byte_offset,
                      uint8_t *buf,
                      uint32_t buf_sz,
                      bool all_data_complete,
                      bool user_cb_safe) {
  (void)user_cb_safe;
  struct pipe_mgr_stful_tbl *t = cookie;
  if (buf_sz) {
    int ram = t->sync_row_idx >> 10;
    int line = t->sync_row_idx & 0x3FF;
    uint32_t bytes_per_line = TOF_SRAM_UNIT_WIDTH / 8;
    uint32_t bytes_per_ram = bytes_per_line * TOF_SRAM_UNIT_DEPTH;
    uint32_t base = ram * bytes_per_ram + line * bytes_per_line;
    uint32_t bytes_per_stage = TOF_SRAM_UNIT_WIDTH / 8;
    uint32_t stage_idx = t->sync_stage_idx + byte_offset / bytes_per_stage;
    uint32_t offset = byte_offset % bytes_per_stage;
    offset += base;
    PIPE_MGR_DBGCHK(t->stages[stage_idx].db[pipe]);
    PIPE_MGR_DBGCHK(t->stages[stage_idx].db_sz >= offset + buf_sz);
    PIPE_MGR_MEMCPY(t->stages[stage_idx].db[pipe] + offset, buf, buf_sz);
    LOG_TRACE(
        "RdCmplt: Dev %d Tbl 0x%x Pipe %d Stage %d(%d) offset 0x%x sz 0x%x%s",
        dev,
        t->hdl,
        pipe,
        t->stages[stage_idx].stage_id,
        stage_idx,
        offset,
        buf_sz,
        all_data_complete ? " Last" : "");
    uint32_t i;
    for (i = 0; i < buf_sz; i += 16) {
      LOG_TRACE(
          "       : %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x "
          "%02x%02x%02x%02x",
          i + 0 < buf_sz ? buf[i + 0] : 0,
          i + 1 < buf_sz ? buf[i + 1] : 0,
          i + 2 < buf_sz ? buf[i + 2] : 0,
          i + 3 < buf_sz ? buf[i + 3] : 0,
          i + 4 < buf_sz ? buf[i + 4] : 0,
          i + 5 < buf_sz ? buf[i + 5] : 0,
          i + 6 < buf_sz ? buf[i + 6] : 0,
          i + 7 < buf_sz ? buf[i + 7] : 0,
          i + 8 < buf_sz ? buf[i + 8] : 0,
          i + 9 < buf_sz ? buf[i + 9] : 0,
          i + 10 < buf_sz ? buf[i + 10] : 0,
          i + 11 < buf_sz ? buf[i + 11] : 0,
          i + 12 < buf_sz ? buf[i + 12] : 0,
          i + 13 < buf_sz ? buf[i + 13] : 0,
          i + 14 < buf_sz ? buf[i + 14] : 0,
          i + 15 < buf_sz ? buf[i + 15] : 0);
    }
  }
  if (all_data_complete) {
    t->sync_row_idx = 0;
    t->sync_stage_idx = 0;
  }
}
static void rd_tbl_cb(void *cookie,
                      bf_dev_id_t dev,
                      bf_dev_pipe_t pipe,
                      uint32_t byte_offset,
                      uint8_t *buf,
                      uint32_t buf_sz,
                      bool all_data_complete,
                      bool user_cb_safe) {
  struct pipe_mgr_stful_tbl *t = cookie;

  if (buf_sz) {
    /* Determine which stage this response is for. */
    int s = 0;
    uint32_t x = 0;
    for (; s < t->num_stages; ++s) {
      if ((x + t->stages[s].db_sz) > t->tbl_sync_rcvd_sz[pipe]) break;
      x += t->stages[s].db_sz;
    }
    PIPE_MGR_DBGCHK(s < t->num_stages);

    /* Determine the offset within the stage. */
    uint32_t stage_offset = byte_offset - x;

    /* Determine how much data is for this "stage".  In certain cases there can
     * be two stages tables for the register in the same physical stage. */
    uint32_t stage_data_sz = (buf_sz + stage_offset <= t->stages[s].db_sz)
                                 ? buf_sz
                                 : (t->stages[s].db_sz - stage_offset);
    PIPE_MGR_DBGCHK(t->stages[s].db[pipe]);
    PIPE_MGR_DBGCHK(t->stages[s].db_sz >= stage_offset + stage_data_sz);
    PIPE_MGR_MEMCPY(t->stages[s].db[pipe] + stage_offset, buf, stage_data_sz);
    t->tbl_sync_rcvd_sz[pipe] += stage_data_sz;
    /* If there is more data for the next stage table, call it again. */
    if (stage_data_sz != buf_sz) {
      rd_tbl_cb(cookie,
                dev,
                pipe,
                byte_offset + stage_data_sz,
                buf + stage_data_sz,
                buf_sz - stage_data_sz,
                false,
                false);
    }
  }
  if (all_data_complete) {
    unsigned int p = 0;
    for (; p < t->dev_info->num_active_pipes; ++p) t->tbl_sync_rcvd_sz[p] = 0;
  }
  if (user_cb_safe) {
    pipe_stful_tbl_sync_cback_fn sync_cb = t->sync_cb;
    void *sync_cookie = t->sync_cookie;
    t->sync_cb = NULL;
    t->sync_cookie = NULL;
    if (sync_cb) sync_cb(dev, sync_cookie);
  }
}

pipe_status_t pipe_mgr_stful_tbl_sync(pipe_sess_hdl_t sess_hdl,
                                      dev_target_t dev_tgt,
                                      pipe_stful_tbl_hdl_t st_hdl,
                                      pipe_stful_tbl_sync_cback_fn cback_fn,
                                      void *cookie) {
  pipe_status_t sts = PIPE_SUCCESS;
  /* Lookup the table, SM layer has already validated that the table exists. */
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev_tgt.device_id, st_hdl);
  if (t == NULL) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }
  /* Only one read in progress at a time. */
  if (t->sync_cb) return PIPE_ALREADY_EXISTS;
  /* Validate the dev target. */
  if (!pipe_mgr_valid_dev_tgt(dev_tgt, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }

  /* Allow BF_DEV_PIPE_ALL even for asymmetric tables and allow specific
   * pipes even for symmetric tables.  This is because the entire table will
   * be sync'ed and the data can be different in each pipe even for symmetric
   * tables since the data plane is modifying the table contents. */
  pipe_bitmap_t pipes;
  PIPE_BITMAP_INIT(&pipes, PIPE_BMP_SIZE);
  if (BF_DEV_PIPE_ALL == dev_tgt.dev_pipe_id) {
    PIPE_BITMAP_ASSIGN(&pipes, &t->pipes);
  } else {
    if (!table_is_in_pipe(t, dev_tgt.dev_pipe_id)) return PIPE_INVALID_ARG;
    if (t->symmetric) {
      PIPE_BITMAP_SET(&pipes, dev_tgt.dev_pipe_id);
    } else {
      struct pipe_mgr_stful_tbl_inst *stbl_inst = NULL;
      stbl_inst = get_stful_tbl_inst_by_pipe_id(t, dev_tgt.dev_pipe_id);
      if (!stbl_inst) {
        LOG_ERROR("%s:%d Stful table inst lookup failure", __func__, __LINE__);
        return PIPE_INVALID_ARG;
      }
      PIPE_BITMAP_ASSIGN(&pipes, &stbl_inst->pipe_bmp);
    }
  }

  /* Send one read instruction for each RAM line the table occupies. */
  rmt_dev_info_t *dev_info = t->dev_info;
  if (!dev_info) return PIPE_INVALID_ARG;
  int i, s, r;
  for (s = 0; s < t->num_stages; ++s) {
    int stage = t->stages[s].stage_id;
    for (i = 0; i < t->stages[s].num_rams; ++i) {
      for (r = 0; r < TOF_SRAM_UNIT_DEPTH; ++r) {
        uint32_t vaddr = ram_row_to_cpu_rd_addr(t->stages[s].ram_vpn[i], r);
        pipe_instr_get_memdata_v_t instr;
        construct_instr_get_v_memdata(t->dev,
                                      &instr,
                                      t->stages[s].logical_table,
                                      pipe_virt_mem_type_sel_stful,
                                      vaddr);
        bf_dev_pipe_t p;
        PIPE_BITMAP_ITER(&pipes, p) {
          sts = pipe_mgr_drv_ilist_rd_add(
              &sess_hdl, dev_info, p, stage, (uint8_t *)&instr, sizeof(instr));
          if (PIPE_SUCCESS != sts) goto cleanup;
        }
      }
    }
  }
  t->sync_cb = cback_fn;
  t->sync_cookie = cookie;
  unsigned int p = 0;
  for (; p < t->dev_info->num_active_pipes; ++p) t->tbl_sync_rcvd_sz[p] = 0;
  sts = pipe_mgr_drv_ilist_rd_push(&sess_hdl, rd_tbl_cb, t);
  if (PIPE_SUCCESS != sts) goto cleanup;

  if (!cback_fn) {
    sts = pipe_mgr_drv_ilist_rd_cmplt_all(&sess_hdl);
    if (sts != PIPE_SUCCESS) {
      LOG_ERROR(
          "Failed to cmplt stateful tbl sync: Dev %d pipe %#x stfulTbl %#x "
          "sts %s",
          dev_tgt.device_id,
          dev_tgt.dev_pipe_id,
          st_hdl,
          pipe_str_err(sts));
      return sts;
    }
  }
  return sts;

cleanup:
  pipe_mgr_drv_ilist_rd_abort(&sess_hdl);
  return sts;
}

pipe_status_t pipe_mgr_stful_direct_tbl_sync(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_stful_tbl_sync_cback_fn cback_fn,
    void *cookie,
    bool *immediate_cb) {
  pipe_status_t sts = PIPE_SUCCESS;
  bf_dev_id_t dev = dev_tgt.device_id;
  *immediate_cb = false;
  /* Validate the dev target. */
  if (!pipe_mgr_valid_dev_tgt(dev_tgt, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }

  /* Look up the stateful table from the MAT handle. */
  struct pipe_mgr_stful_tbl *t =
      get_stful_tbl_from_direct_mat(dev, mat_tbl_hdl);
  if (!t) return PIPE_INVALID_ARG;
  /* Only one read in progress at a time. */
  if (t->sync_cb) return PIPE_ALREADY_EXISTS;

  /* Only sync valid pipes */
  if (dev_tgt.dev_pipe_id != BF_DEV_PIPE_ALL) {
    uint32_t pipe_idx = 0;
    for (pipe_idx = 0; pipe_idx < t->num_tbl_instances; pipe_idx++) {
      if (t->stful_tbl_inst[pipe_idx].pipe_id == dev_tgt.dev_pipe_id) {
        break;
      }
    }
    if (pipe_idx == t->num_tbl_instances) {
      LOG_ERROR("%s:%d Unable to sync direct stful table on invalid pipe %d",
                __func__,
                __LINE__,
                dev_tgt.dev_pipe_id);
      return PIPE_INVALID_ARG;
    }
  }

  struct pipe_mgr_stful_direct_tbl_sync_cookie *sc = NULL;

  /* Find the list of entries in the table we need to sync. */
  int hdl_count = 0;
  pipe_mat_ent_hdl_t *hdls = NULL;
  for (unsigned i = 0; i < t->num_tbl_instances; ++i) {
    struct pipe_mgr_stful_tbl_inst *inst = &t->stful_tbl_inst[i];
    /* If we are syncing a specific instance then skip over all other table
     * instances that don't match the requested pipe. */
    if (dev_tgt.dev_pipe_id != BF_DEV_PIPE_ALL &&
        dev_tgt.dev_pipe_id != inst->pipe_id) {
      continue;
    }

    /* Get the size of our shadow map for the first pipe in the instance. */
    int first_pipe = PIPE_BITMAP_GET_FIRST_SET(&inst->pipe_bmp);
    int num_entries_in_inst = 0;
    unsigned long key = 0;
    void *data = NULL;
    bf_map_t *m = &t->hdl_to_shdw[first_pipe];
    for (bf_map_sts_t msts = bf_map_get_first(m, &key, &data);
         msts == BF_MAP_OK;
         msts = bf_map_get_next(m, &key, &data)) {
      ++num_entries_in_inst;
    }
    /* Add all the handles from the map to our list of handles to sync. */
    if (num_entries_in_inst) {
      /* Allocate memory to hold the handles. */
      pipe_mat_ent_hdl_t *x = PIPE_MGR_REALLOC(
          hdls, (hdl_count + num_entries_in_inst) * sizeof(pipe_mat_ent_hdl_t));
      if (!x) {
        if (hdls) PIPE_MGR_FREE(hdls);
        return PIPE_NO_SYS_RESOURCES;
      }
      hdls = x;

      /* Populate the list of handles. */
      for (bf_map_sts_t msts = bf_map_get_first(m, &key, &data);
           msts == BF_MAP_OK;
           msts = bf_map_get_next(m, &key, &data)) {
        hdls[hdl_count++] = key;
      }
    }
  }

  /* If there was nothing to sync we're done and the user's callback can be
   * invoked immediately. */
  if (!hdl_count) {
    *immediate_cb = true;
    return PIPE_SUCCESS;
  }

  /* Allocate memory to hold the entry handle to index mapping for the MAT. */
  sc = PIPE_MGR_CALLOC(1, sizeof *sc);
  if (!sc) return PIPE_NO_SYS_RESOURCES;
  sc->t = t;
  sc->num_hdls = hdl_count;
  sc->data = PIPE_MGR_CALLOC(t->dev_info->num_active_pipes, sizeof *sc->data);
  for (unsigned p = 0; sc->data && p < t->dev_info->num_active_pipes; ++p) {
    bf_map_init(&sc->data[p]);
  }
  sc->ent_hdls = hdls;
  sc->ent_pipes = PIPE_MGR_CALLOC(hdl_count, sizeof *sc->ent_pipes);
  sc->ent_stages = PIPE_MGR_CALLOC(hdl_count, sizeof *sc->ent_stages);
  sc->ent_index = PIPE_MGR_CALLOC(hdl_count, sizeof *sc->ent_index);
  if (!sc->data || !sc->ent_hdls || !sc->ent_pipes || !sc->ent_stages ||
      !sc->ent_index) {
    sts = PIPE_NO_SYS_RESOURCES;
    goto cleanup;
  }

  /* Fill in the entry locations. */
  for (int i = 0; i < hdl_count; ++i) {
    sts = pipe_mgr_mat_ent_get_dir_ent_location(dev,
                                                mat_tbl_hdl,
                                                sc->ent_hdls[i],
                                                0,
                                                &sc->ent_pipes[i],
                                                &sc->ent_stages[i],
                                                NULL,
                                                &sc->ent_index[i]);
    if (PIPE_SUCCESS != sts) {
      LOG_ERROR(
          "Dev %d Failed to get handle to location mapping for table 0x%x "
          "entry %d, status %s",
          dev,
          mat_tbl_hdl,
          sc->ent_hdls[i],
          pipe_str_err(sts));
      goto cleanup;
    }
  }

  /* Get the list of locations (16B RAM lines) to read.  Since multiple entries
   * in the table may share a RAM line place all the entries in a map to filter
   * the duplicates. */
  int entry_width = table_bit_width(t);
  if (!entry_width) {
    LOG_ERROR(
        "%s:%d Entry width for table 0x%x is 0", __func__, __LINE__, t->hdl);
    return PIPE_UNEXPECTED;
  }

  int entries_per_row = TOF_SRAM_UNIT_WIDTH / entry_width;
  uint8_t *ram_data = PIPE_MGR_CALLOC(TOF_SRAM_UNIT_WIDTH / 8, 1);
  unsigned p;
  if (!ram_data) {
    sts = PIPE_NO_SYS_RESOURCES;
    goto cleanup;
  }
  for (int i = 0; i < hdl_count; ++i) {
    /* Build the location as 8 bits of stage-id and 16 bits of ram line. */
    unsigned long loc = sc->ent_stages[i];
    loc = (loc << 16) | sc->ent_index[i] / entries_per_row;

    struct pipe_mgr_stful_tbl_inst *inst = NULL;
    inst = get_stful_tbl_inst_by_pipe_id(t, sc->ent_pipes[i]);
    if (!inst) {
      sts = PIPE_UNEXPECTED;
      goto cleanup;
    }
    PIPE_BITMAP_ITER(&inst->pipe_bmp, p) {
      bf_map_sts_t msts = bf_map_add(&sc->data[p], loc, ram_data);
      if (msts == BF_MAP_OK) {
        /* Added a new location, now get more memory for the next location. */
        ram_data = NULL;
        ram_data = PIPE_MGR_CALLOC(TOF_SRAM_UNIT_WIDTH / 8, 1);
        if (!ram_data) {
          sts = PIPE_NO_SYS_RESOURCES;
          goto cleanup;
        }
      } else if (msts == BF_MAP_KEY_EXISTS) {
        /* This just means that multiple entries are sharing the same RAM line.
         */
      } else {
        PIPE_MGR_DBGCHK(0);
        sts = PIPE_UNEXPECTED;
        goto cleanup;
      }
    }
  }
  /* We have one extra allocation from the loops above, release it. */
  PIPE_MGR_FREE(ram_data);

  /* Issue the reads. */
  PIPE_BITMAP_ITER(&t->pipes, p) {
    void *dont_care = NULL;
    unsigned long key = 0;
    for (bf_map_sts_t msts = bf_map_get_first(&sc->data[p], &key, &dont_care);
         msts == BF_MAP_OK;
         msts = bf_map_get_next(&sc->data[p], &key, &dont_care)) {
      int stage_id = key >> 16;
      int stage_idx = stage_to_stage_idx(t, stage_id);
      int stage_line = key & 0xFFFF;
      int ram_idx = stage_line >> TOF_SRAM_NUM_RAM_LINE_BITS;
      int ram_line = stage_line & ((1 << TOF_SRAM_NUM_RAM_LINE_BITS) - 1);
      uint32_t vaddr = ram_row_to_cpu_rd_addr(
          t->stages[stage_idx].ram_vpn[ram_idx], ram_line);
      pipe_instr_get_memdata_v_t instr;
      construct_instr_get_v_memdata(t->dev,
                                    &instr,
                                    t->stages[stage_idx].logical_table,
                                    pipe_virt_mem_type_sel_stful,
                                    vaddr);
      sts = pipe_mgr_drv_ilist_rd_add(
          &sess_hdl, t->dev_info, p, stage_id, (uint8_t *)&instr, sizeof instr);
      if (PIPE_SUCCESS != sts) goto cleanup;
    }
  }
  sts = pipe_mgr_drv_ilist_rd_push(&sess_hdl, rd_all_direct_cb, sc);
  if (PIPE_SUCCESS != sts) goto cleanup;

  t->sync_cb = cback_fn;
  t->sync_cookie = cookie;

  /* Make this a blocking call if a callback function was not provided. */
  return sts;

cleanup:
  if (sc) {
    if (sc->data) {
      for (p = 0; p < t->dev_info->num_active_pipes; ++p)
        bf_map_destroy(&sc->data[p]);
      PIPE_MGR_FREE(sc->data);
    }
    if (sc->ent_hdls) PIPE_MGR_FREE(sc->ent_hdls);
    if (sc->ent_pipes) PIPE_MGR_FREE(sc->ent_pipes);
    if (sc->ent_stages) PIPE_MGR_FREE(sc->ent_stages);
    if (sc->ent_index) PIPE_MGR_FREE(sc->ent_index);
    pipe_mgr_drv_ilist_rd_abort(&sess_hdl);
    PIPE_MGR_FREE(sc);
  }
  return sts;
}

static pipe_status_t read_one(pipe_sess_hdl_t sess_hdl,
                              dev_target_t dev_tgt,
                              struct pipe_mgr_stful_tbl *t,
                              int stage_idx,
                              int ram,
                              int line) {
  /* Allow BF_DEV_PIPE_ALL even for asymmetric tables and allow specific
   * pipes even for symmetric tables.  This is because the entire table will
   * be sync'ed and the data can be different in each pipe even for symmetric
   * tables since the data plane is modifying the table contents. */
  pipe_bitmap_t pipes;
  PIPE_BITMAP_INIT(&pipes, PIPE_BMP_SIZE);
  if (BF_DEV_PIPE_ALL == dev_tgt.dev_pipe_id) {
    PIPE_BITMAP_ASSIGN(&pipes, &t->pipes);
  } else {
    if (!table_is_in_pipe(t, dev_tgt.dev_pipe_id)) return PIPE_INVALID_ARG;
    if (t->symmetric) {
      PIPE_BITMAP_SET(&pipes, dev_tgt.dev_pipe_id);
    } else {
      struct pipe_mgr_stful_tbl_inst *stbl_inst = NULL;
      stbl_inst = get_stful_tbl_inst_by_pipe_id(t, dev_tgt.dev_pipe_id);
      if (!stbl_inst) {
        LOG_ERROR("%s:%d Stful table inst lookup failure", __func__, __LINE__);
        return PIPE_INVALID_ARG;
      }
      PIPE_BITMAP_ASSIGN(&pipes, &stbl_inst->pipe_bmp);
    }
  }

  uint32_t vaddr =
      ram_row_to_cpu_rd_addr(t->stages[stage_idx].ram_vpn[ram], line);
  pipe_instr_get_memdata_v_t instr;
  construct_instr_get_v_memdata(t->dev,
                                &instr,
                                t->stages[stage_idx].logical_table,
                                pipe_virt_mem_type_sel_stful,
                                vaddr);

  rmt_dev_info_t *dev_info = t->dev_info;
  if (!dev_info) return PIPE_INVALID_ARG;
  pipe_status_t sts = PIPE_SUCCESS;
  bf_dev_pipe_t p;
  PIPE_BITMAP_ITER(&pipes, p) {
    sts = pipe_mgr_drv_ilist_rd_add(&sess_hdl,
                                    dev_info,
                                    p,
                                    t->stages[stage_idx].stage_id,
                                    (uint8_t *)&instr,
                                    sizeof(instr));
    if (PIPE_SUCCESS != sts) return sts;
  }
  return sts;
}

static pipe_status_t pipe_mgr_stful_ent_sync(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_stful_tbl_hdl_t stful_tbl_hdl,
    pipe_stful_mem_idx_t stful_ent_idx) {
  pipe_status_t sts = PIPE_SUCCESS;
  /* Lookup the table, SM layer has already validated that the table exists. */
  struct pipe_mgr_stful_tbl *t =
      stful_tbl_lkup(dev_tgt.device_id, stful_tbl_hdl);
  if (t == NULL) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }
  /* Only one read in progress at a time. */
  if (t->sync_cb) return PIPE_ALREADY_EXISTS;
  /* Validate the dev target. */
  if (!pipe_mgr_valid_dev_tgt(dev_tgt, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }

  if (!is_valid_index(t, stful_ent_idx, __func__, __LINE__))
    return PIPE_INVALID_ARG;

  /* Send one read instruction to the stage the index is in. */
  int stage_idx = 0, idx_in_stage = 0;
  log_index_to_stage_and_offset(t, stful_ent_idx, &stage_idx, &idx_in_stage);
  int vpn, line;
  decompose_index(table_width(t), idx_in_stage, &vpn, &line, NULL);
  sts = read_one(sess_hdl, dev_tgt, t, stage_idx, vpn, line);
  if (PIPE_SUCCESS != sts) goto cleanup;

  t->sync_row_idx = vpn << 10 | line;
  t->sync_stage_idx = stage_idx;
  sts = pipe_mgr_drv_ilist_rd_push(&sess_hdl, rd_one_cb, t);
  if (PIPE_SUCCESS != sts) goto cleanup;
  return sts;
cleanup:
  pipe_mgr_drv_ilist_rd_abort(&sess_hdl);
  t->sync_row_idx = 0;
  t->sync_stage_idx = 0;
  return sts;
}

static pipe_status_t pipe_mgr_stful_direct_ent_sync(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t dev,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_mat_ent_hdl_t mat_ent_hdl) {
  pipe_status_t sts = PIPE_SUCCESS;
  struct pipe_mgr_stful_direct_ent_sync_cookie *m = NULL;
  /* Validate the dev id. */
  if (!pipe_mgr_valid_deviceId(dev, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }

  /* Look up the stateful table from the MAT handle. */
  struct pipe_mgr_stful_tbl *t =
      get_stful_tbl_from_direct_mat(dev, mat_tbl_hdl);
  if (!t) {
    LOG_ERROR("No stateful table with hdl 0x%x on dev %d at %s:%d",
              mat_tbl_hdl,
              dev,
              __func__,
              __LINE__);
    return PIPE_INVALID_ARG;
  }

  /* Allocate memory to hold the entry handle to index mapping for the MAT. */
  m = PIPE_MGR_CALLOC(1, sizeof *m);
  if (!m) {
    LOG_ERROR(
        "No memory for stateful table sync, table 0x%x on dev %d at %s:%d",
        mat_tbl_hdl,
        dev,
        __func__,
        __LINE__);
    return PIPE_NO_SYS_RESOURCES;
  }

  /* Cache the current entry handle to index mapping. */
  m->t = t;
  m->ent_hdl = mat_ent_hdl;
  bf_dev_pipe_t ent_pipe;
  dev_stage_t ent_stage;
  if (PIPE_SUCCESS != pipe_mgr_mat_ent_get_dir_ent_location(dev,
                                                            mat_tbl_hdl,
                                                            m->ent_hdl,
                                                            0,
                                                            &ent_pipe,
                                                            &ent_stage,
                                                            NULL,
                                                            &m->index)) {
    sts = PIPE_OBJ_NOT_FOUND;
    LOG_ERROR(
        "Cannot find entry to sync; entry 0x%x, table 0x%x, dev %d at %s:%d",
        mat_ent_hdl,
        mat_tbl_hdl,
        dev,
        __func__,
        __LINE__);
    goto cleanup;
  }
  LOG_TRACE("Stateful Sync: dev %d pipe %#x table %#x entry %#x at %d.%d",
            dev,
            ent_pipe,
            mat_tbl_hdl,
            mat_ent_hdl,
            ent_stage,
            m->index);

  /* Map the index to a RAM/VPN, and a row within the RAM. */
  int ram, line;
  pipe_stful_mem_idx_t stful_ent_idx = m->index;
  decompose_index(table_width(t), stful_ent_idx, &ram, &line, NULL);

  /* Read the table at the VPN and RAM line. */
  int stage = ent_stage;
  int stage_idx = stage_to_stage_idx(t, stage);
  if (stage_idx == -1) {
    PIPE_MGR_DBGCHK(0);
    goto cleanup;
  }
  dev_target_t dev_tgt;
  dev_tgt.device_id = dev;
  dev_tgt.dev_pipe_id = ent_pipe;
  sts = read_one(sess_hdl, dev_tgt, t, stage_idx, ram, line);
  if (PIPE_SUCCESS != sts) goto cleanup;
  sts = pipe_mgr_drv_ilist_rd_push(&sess_hdl, rd_one_direct_cb, m);
  if (PIPE_SUCCESS != sts) goto cleanup;

  return PIPE_SUCCESS;

cleanup:
  LOG_ERROR("Sync fails with \"%s\"; entry 0x%x, table 0x%x, dev %d at %s:%d",
            pipe_str_err(sts),
            mat_ent_hdl,
            mat_tbl_hdl,
            dev,
            __func__,
            __LINE__);
  pipe_mgr_drv_ilist_rd_abort(&sess_hdl);
  if (m) PIPE_MGR_FREE(m);
  return sts;
}

pipe_status_t pipe_mgr_stful_query_sizes(pipe_sess_hdl_t sess_hdl,
                                         bf_dev_id_t device_id,
                                         pipe_stful_tbl_hdl_t stful_tbl_hdl,
                                         int *num_pipes) {
  pipe_status_t ret;
  if (PIPE_SUCCESS != (ret = pipe_mgr_api_enter(sess_hdl))) {
    LOG_ERROR("%s: Invalid session handle 0x%x", __func__, sess_hdl);
    return ret;
  }
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(device_id);
  if (!dev_info) {
    LOG_ERROR("%s: Invalid device id %d", __func__, device_id);
    ret = PIPE_INVALID_ARG;
    goto done;
  }
  if (!num_pipes) {
    LOG_ERROR("%s: No output pointer provided for dev %d tbl 0x%x",
              __func__,
              device_id,
              stful_tbl_hdl);
    ret = PIPE_INVALID_ARG;
    goto done;
  }
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(device_id, stful_tbl_hdl);
  if (!t) {
    LOG_ERROR("%s: Invalid table handle 0x%x for device %d",
              __func__,
              stful_tbl_hdl,
              device_id);
    ret = PIPE_INVALID_ARG;
    goto done;
  }

  *num_pipes = dev_info->num_active_pipes;

done:
  pipe_mgr_api_exit(sess_hdl);
  return ret;
}

pipe_status_t pipe_mgr_stful_direct_query_sizes(pipe_sess_hdl_t sess_hdl,
                                                bf_dev_id_t device_id,
                                                pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                                int *num_pipes) {
  pipe_status_t ret;
  if (PIPE_SUCCESS != (ret = pipe_mgr_api_enter(sess_hdl))) {
    LOG_ERROR("%s: Invalid session handle 0x%x", __func__, sess_hdl);
    return ret;
  }

  struct pipe_mgr_stful_tbl *t;
  t = get_stful_tbl_from_direct_mat(device_id, mat_tbl_hdl);
  if (!t) {
    LOG_ERROR("%s: Invalid table handle 0x%x for device %d",
              __func__,
              mat_tbl_hdl,
              device_id);
    pipe_mgr_api_exit(sess_hdl);
    return PIPE_INVALID_ARG;
  }

  pipe_mgr_api_exit(sess_hdl);
  return pipe_mgr_stful_query_sizes(sess_hdl, device_id, t->hdl, num_pipes);
}

pipe_status_t pipe_mgr_stful_ent_query(pipe_sess_hdl_t sess_hdl,
                                       dev_target_t dev_tgt,
                                       pipe_stful_tbl_hdl_t stful_tbl_hdl,
                                       pipe_stful_mem_idx_t stful_ent_idx,
                                       pipe_stful_mem_query_t *data,
                                       uint32_t pipe_api_flags) {
  pipe_status_t sts;
  if (pipe_api_flags & PIPE_FLAG_SYNC_REQ) {
    sts = pipe_mgr_stful_ent_sync(
        sess_hdl, dev_tgt, stful_tbl_hdl, stful_ent_idx);
    if (sts != PIPE_SUCCESS) {
      LOG_ERROR(
          "Failed to sync stateful entry: Dev %d pipe %#x stfulTbl %#x idx %#x "
          "sts %s",
          dev_tgt.device_id,
          dev_tgt.dev_pipe_id,
          stful_tbl_hdl,
          stful_ent_idx,
          pipe_str_err(sts));
      return sts;
    }
    sts = pipe_mgr_drv_ilist_rd_cmplt_all(&sess_hdl);
    if (sts != PIPE_SUCCESS) {
      LOG_ERROR(
          "Failed to cmplt stateful entry sync: Dev %d pipe %#x stfulTbl %#x "
          "idx %#x sts %s",
          dev_tgt.device_id,
          dev_tgt.dev_pipe_id,
          stful_tbl_hdl,
          stful_ent_idx,
          pipe_str_err(sts));
      return sts;
    }
  }

  /* Lookup the table, SM layer has already validated that the table exists. */
  struct pipe_mgr_stful_tbl *t =
      stful_tbl_lkup(dev_tgt.device_id, stful_tbl_hdl);
  if (t == NULL) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }
  /* Validate the table index. */
  if (!is_valid_index(t, stful_ent_idx, __func__, __LINE__))
    return PIPE_INVALID_ARG;
  /* Validate the dev target. */
  if (!pipe_mgr_valid_dev_tgt(dev_tgt, __func__, __LINE__))
    return PIPE_INVALID_ARG;
  if (!data) return PIPE_INVALID_ARG;

  /* Determine which pipes to read and store them in a bit mask. */
  unsigned int rd_msk = 0;
  if (BF_DEV_PIPE_ALL == dev_tgt.dev_pipe_id) {
    unsigned int b;
    PIPE_BITMAP_ITER(&t->pipes, b) { rd_msk |= 1 << b; }
  } else {
    if (!table_is_in_pipe(t, dev_tgt.dev_pipe_id)) return PIPE_INVALID_ARG;
    if (t->symmetric) {
      /* Table is symmetric but we are requesting to read only a single pipe. */
      rd_msk = 1 << dev_tgt.dev_pipe_id;
    } else {
      /* Table is not symmetric so the specified pipe should be the first pipe
       * in a table instance and we'll read all pipes that belong to that
       * instance. */
      unsigned int b;
      struct pipe_mgr_stful_tbl_inst *stbl_inst = NULL;
      stbl_inst = get_stful_tbl_inst_by_pipe_id(t, dev_tgt.dev_pipe_id);
      if (!stbl_inst) {
        LOG_ERROR("%s:%d Stful table inst lookup failure", __func__, __LINE__);
        return PIPE_INVALID_ARG;
      }
      PIPE_BITMAP_ITER(&stbl_inst->pipe_bmp, b) { rd_msk |= 1 << b; }
    }
  }
  if (!rd_msk) return PIPE_INVALID_ARG;
  /* Make sure thte caller made enough room for the data. */
  int pipe_count = __builtin_popcount(rd_msk);
  if (data->pipe_count < pipe_count) {
    LOG_ERROR(
        "Stateful table read failed, passed %d specs but need to populate %d. "
        "Dev %d Pipe %x Tbl 0x%x Idx %d",
        data->pipe_count,
        pipe_count,
        dev_tgt.device_id,
        dev_tgt.dev_pipe_id,
        t->hdl,
        stful_ent_idx);
    return PIPE_INVALID_ARG;
  }

  /* Verify that we have data ready for the index. */
  uint32_t idx_in_stage = stful_ent_idx;
  int s = 0;
  for (s = 0; s < t->num_stages; ++s) {
    if (idx_in_stage < t->stages[s].num_entries) {
      /* Found the stage containing the index. */
      /* For each pipe we are reading make sure we have data. */
      for (int p = 0; p < (int)t->dev_info->num_active_pipes; ++p) {
        if (!(rd_msk & (1u << p))) continue;
        if (!t->stages[s].db[p]) {
          LOG_ERROR("Read data not ready for dev %d tbl 0x%x pipe %d stage %d",
                    t->dev,
                    t->hdl,
                    p,
                    t->stages[s].stage_id);
          return PIPE_INVALID_ARG;
        }
      }
      break;
    } else {
      idx_in_stage -= t->stages[s].num_entries;
    }
  }

  /* Copy the data for the caller. */
  int ram, line;
  decompose_index(table_width(t), idx_in_stage, &ram, &line, NULL);
  uint32_t offset =
      (ram * TOF_SRAM_UNIT_DEPTH + line) * (TOF_SRAM_UNIT_WIDTH / 8);
  data->pipe_count = pipe_count;
  PIPE_MGR_DBGCHK((offset + (TOF_SRAM_UNIT_WIDTH / 8) - 1) <
                  t->stages[s].db_sz);
  bf_dev_pipe_t pipe = 0;
  for (int p = 0; p < (int)t->dev_info->num_active_pipes; ++p) {
    if (!(rd_msk & (1 << p))) continue;
    stful_data_decode(table_width(t),
                      stful_ent_idx,
                      &data->data[pipe],
                      t->stages[s].db[p] + offset);
    ++pipe;
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stful_ent_query_range(pipe_sess_hdl_t sess_hdl,
                                             dev_target_t dev_tgt,
                                             pipe_stful_tbl_hdl_t stful_tbl_hdl,
                                             pipe_stful_mem_idx_t stful_ent_idx,
                                             uint32_t num_indices,
                                             pipe_stful_mem_query_t *data,
                                             uint32_t *num_indices_read,
                                             uint32_t pipe_api_flags) {
  pipe_status_t s = PIPE_SUCCESS;
  LOG_TRACE("Dev %d pipe %x tbl 0x%x range read %d to %d%s",
            dev_tgt.device_id,
            dev_tgt.dev_pipe_id,
            stful_tbl_hdl,
            stful_ent_idx,
            stful_ent_idx + num_indices - 1,
            pipe_api_flags & PIPE_FLAG_SYNC_REQ ? " SYNC" : "");

  /* Validate the index range passed in, if it goes past the end of the table
   * shorten it. */
  struct pipe_mgr_stful_tbl *t =
      stful_tbl_lkup(dev_tgt.device_id, stful_tbl_hdl);
  if (!t) return PIPE_INVALID_ARG;
  if (!is_valid_index(t, stful_ent_idx, __func__, __LINE__))
    return PIPE_INVALID_ARG;
  num_indices =
      num_indices > t->num_entries_real ? t->num_entries_real : num_indices;
  num_indices = stful_ent_idx + num_indices <= t->num_entries_real
                    ? num_indices
                    : t->num_entries_real - stful_ent_idx;

  /* If the caller requested a synchronous query then bring in the entire table
   * before reading. */
  if (pipe_api_flags & PIPE_FLAG_SYNC_REQ) {
    pipe_api_flags &= ~PIPE_FLAG_SYNC_REQ;

    s = pipe_stful_database_sync(sess_hdl, dev_tgt, stful_tbl_hdl, NULL, NULL);
    if (s != PIPE_SUCCESS) {
      LOG_ERROR(
          "Failed to sync stateful table: Dev %d pipe %#x stfulTbl %#x sts %s",
          dev_tgt.device_id,
          dev_tgt.dev_pipe_id,
          stful_tbl_hdl,
          pipe_str_err(s));
      return s;
    }
  }

  uint32_t i = 0;
  for (; i < num_indices && s == PIPE_SUCCESS; ++i) {
    s = pipe_mgr_stful_ent_query(sess_hdl,
                                 dev_tgt,
                                 stful_tbl_hdl,
                                 stful_ent_idx + i,
                                 data + i,
                                 pipe_api_flags);
  }
  *num_indices_read = num_indices;
  if (s != PIPE_SUCCESS) {
    LOG_ERROR("Dev %d pipe %x stateful query at %d failed with status %s",
              dev_tgt.device_id,
              dev_tgt.dev_pipe_id,
              stful_ent_idx + i - 1,
              pipe_str_err(s));
    *num_indices_read = 0;
  }
  return s;
}

pipe_status_t pipe_mgr_stful_direct_ent_query(pipe_sess_hdl_t sess_hdl,
                                              bf_dev_id_t dev,
                                              pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                              pipe_mat_ent_hdl_t mat_ent_hdl,
                                              pipe_stful_mem_query_t *data,
                                              uint32_t pipe_api_flags) {
  pipe_status_t sts;
  if (pipe_api_flags & PIPE_FLAG_SYNC_REQ) {
    sts =
        pipe_mgr_stful_direct_ent_sync(sess_hdl, dev, mat_tbl_hdl, mat_ent_hdl);
    if (sts != PIPE_SUCCESS) {
      LOG_ERROR(
          "Failed to sync stateful entry: Dev %d tbl %#x entHdl %#x sts %s",
          dev,
          mat_tbl_hdl,
          mat_ent_hdl,
          pipe_str_err(sts));
      return sts;
    }
    sts = pipe_mgr_drv_ilist_rd_cmplt_all(&sess_hdl);
    if (sts != PIPE_SUCCESS) {
      LOG_ERROR(
          "Failed to cmplt stateful entry sync: Dev %d tbl %#x entHdl %#x sts "
          "%s",
          dev,
          mat_tbl_hdl,
          mat_ent_hdl,
          pipe_str_err(sts));
      return sts;
    }
  }

  /* Look up the stateful table from the MAT handle. */
  struct pipe_mgr_stful_tbl *t =
      get_stful_tbl_from_direct_mat(dev, mat_tbl_hdl);
  if (!t) return PIPE_INVALID_ARG;

  /* Get the entry location to determine which pipes it is in. */
  bf_dev_pipe_t ent_pipe;
  dev_stage_t ent_stage;
  uint32_t ent_index;
  sts = pipe_mgr_mat_ent_get_dir_ent_location(dev,
                                              mat_tbl_hdl,
                                              mat_ent_hdl,
                                              0,
                                              &ent_pipe,
                                              &ent_stage,
                                              NULL,
                                              &ent_index);
  if (PIPE_SUCCESS != sts) {
    LOG_ERROR(
        "%s: Dev %d Failed to get handle to location mapping for table 0x%x "
        "entry %d, status %s",
        __func__,
        dev,
        mat_tbl_hdl,
        mat_ent_hdl,
        pipe_str_err(sts));
    return sts;
  }

  /* Find the stateful table instance for the entry. */
  struct pipe_mgr_stful_tbl_inst *inst = NULL;
  inst = get_stful_tbl_inst_by_pipe_id(t, ent_pipe);
  if (!inst) {
    LOG_ERROR("Dev %d tbl 0x%x failed to find instance for pipe %x",
              dev,
              t->hdl,
              ent_pipe);
    return PIPE_UNEXPECTED;
  }

  /* Copy the spec for the entry out of our shadow for each pipe in the
   * instance. */
  data->pipe_count = 0;
  bf_dev_pipe_t p;
  PIPE_BITMAP_ITER(&inst->pipe_bmp, p) {
    pipe_stful_mem_spec_t *shdw_spec = NULL;
    bf_map_sts_t msts =
        bf_map_get(&t->hdl_to_shdw[p], mat_ent_hdl, (void **)&shdw_spec);
    if (BF_MAP_OK != msts) {
      LOG_ERROR(
          "Dev %d tbl 0x%x error %d looking up spec for tbl 0x%x entry %d "
          "log-pipe %d",
          dev,
          t->hdl,
          msts,
          mat_tbl_hdl,
          mat_ent_hdl,
          p);
      return PIPE_OBJ_NOT_FOUND;
    }
    data->data[data->pipe_count] = *shdw_spec;
    ++data->pipe_count;
  }

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stful_verify_idx(bf_dev_id_t dev,
                                        bf_dev_pipe_t pipe,
                                        pipe_stful_tbl_hdl_t hdl,
                                        pipe_stful_mem_idx_t idx) {
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev, hdl);
  if (!t) return PIPE_OBJ_NOT_FOUND;
  if (BF_DEV_PIPE_ALL != pipe && !table_is_in_pipe(t, pipe))
    return PIPE_INVALID_ARG;
  if (!is_valid_index(t, idx, __func__, __LINE__)) return PIPE_INVALID_ARG;

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stful_get_indirect_ptr(bf_dev_id_t dev,
                                              bf_dev_pipe_t pipe,
                                              int stage,
                                              pipe_act_fn_hdl_t act_fn_hdl,
                                              pipe_stful_tbl_hdl_t hdl,
                                              pipe_stful_mem_idx_t idx,
                                              uint32_t *ptr) {
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev, hdl);
  if (!t) return PIPE_OBJ_NOT_FOUND;
  if (BF_DEV_PIPE_ALL != pipe && !table_is_in_pipe(t, pipe))
    return PIPE_INVALID_ARG;
  if (!table_is_in_stage(t, stage)) return PIPE_INVALID_ARG;
  if (!is_valid_index(t, idx, __func__, __LINE__)) return PIPE_INVALID_ARG;
  if (ptr == NULL) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }

  *ptr = stful_idx_encode_dp(t, stage, act_fn_hdl, idx);
  LOG_TRACE(
      "Mapped dev %d, pipe %d, stage %d, action %#x tbl %#x idx %d to 0x%08x",
      dev,
      pipe,
      stage,
      act_fn_hdl,
      t->hdl,
      idx,
      *ptr);
  return PIPE_SUCCESS;
}
pipe_status_t pipe_mgr_stful_get_indirect_disabled_ptr(bf_dev_id_t dev,
                                                       bf_dev_pipe_t pipe,
                                                       int stage,
                                                       pipe_stful_tbl_hdl_t hdl,
                                                       uint32_t *ptr) {
  (void)dev;
  (void)pipe;
  (void)stage;
  (void)hdl;
  if (ptr == NULL) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }

  *ptr = 0;
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stful_instance_free(struct pipe_mgr_stful_tbl *stbl) {
  if (stbl->stful_tbl_inst) {
    PIPE_MGR_FREE(stbl->stful_tbl_inst);
    stbl->stful_tbl_inst = NULL;
  }

  return PIPE_SUCCESS;
}

/* Create stful table instances */
pipe_status_t pipe_mgr_stful_instance_alloc(struct pipe_mgr_stful_tbl *stbl) {
  uint32_t tbl_idx = 0;
  bf_dev_pipe_t pipe_id = 0;

  stbl->stful_tbl_inst = (struct pipe_mgr_stful_tbl_inst *)PIPE_MGR_CALLOC(
      stbl->num_tbl_instances, sizeof(struct pipe_mgr_stful_tbl_inst));

  if (!stbl->stful_tbl_inst) {
    LOG_ERROR(
        "%s : Could not allocate memory for stful table data "
        "for table with device id %d, tbl-hdl 0x%x",
        __func__,
        stbl->dev,
        stbl->hdl);
    return PIPE_NO_SYS_RESOURCES;
  }

  for (tbl_idx = 0; tbl_idx < stbl->num_tbl_instances; tbl_idx++) {
    struct pipe_mgr_stful_tbl_inst *stbl_inst = &stbl->stful_tbl_inst[tbl_idx];
    if (stbl->symmetric) {
      pipe_id = BF_DEV_PIPE_ALL;
    } else {
      pipe_id =
          pipe_mgr_get_lowest_pipe_in_scope(stbl->scope_pipe_bmp[tbl_idx]);
    }
    stbl_inst->pipe_id = pipe_id;
    PIPE_BITMAP_INIT(&(stbl_inst->pipe_bmp), PIPE_BMP_SIZE);
    pipe_mgr_convert_scope_pipe_bmp(stbl->scope_pipe_bmp[tbl_idx],
                                    &(stbl_inst->pipe_bmp));
  }

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stful_tbl_get_symmetric_mode(
    bf_dev_id_t dev_id,
    pipe_mat_tbl_hdl_t tbl_hdl,
    bool *symmetric,
    scope_num_t *num_scopes,
    scope_pipes_t *scope_pipe_bmp) {
  struct pipe_mgr_stful_tbl *tbl = stful_tbl_lkup(dev_id, tbl_hdl);
  if (!tbl) {
    PIPE_MGR_DBGCHK(tbl);
    return PIPE_UNEXPECTED;
  }

  *symmetric = tbl->symmetric;
  *num_scopes = tbl->num_scopes;
  PIPE_MGR_MEMCPY(scope_pipe_bmp,
                  tbl->scope_pipe_bmp,
                  tbl->num_scopes * sizeof tbl->scope_pipe_bmp[0]);
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stful_tbl_set_symmetric_mode(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t dev,
    pipe_stful_tbl_hdl_t h,
    bool new_mode,
    scope_num_t num_scopes,
    scope_pipes_t *scope_pipe_bmp) {
  pipe_status_t sts = PIPE_SUCCESS;
  /* Lookup the table, SM layer has already validated that the table exists. */
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev, h);
  if (t == NULL) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }

  /* Check if the scope has changed */
  if (!pipe_mgr_tbl_is_scope_different(dev,
                                       h,
                                       new_mode,
                                       num_scopes,
                                       scope_pipe_bmp,
                                       t->symmetric,
                                       t->num_scopes,
                                       &t->scope_pipe_bmp[0])) {
    LOG_TRACE(
        "%s: Dev %d, Table 0x%x, No change to symmetric mode %d, Num-scopes "
        "%d ",
        __func__,
        t->dev,
        t->hdl,
        t->symmetric,
        t->num_scopes);
    return PIPE_SUCCESS;
  }

  /* Free current instance info */
  pipe_mgr_stful_instance_free(t);

  /* Set new scope info */
  t->symmetric = new_mode;
  t->num_scopes = num_scopes;
  PIPE_MGR_MEMCPY(
      t->scope_pipe_bmp, scope_pipe_bmp, num_scopes * sizeof(scope_pipes_t));
  t->num_tbl_instances = t->num_scopes;
  sts = pipe_mgr_stful_instance_alloc(t);
  if (sts != PIPE_SUCCESS) {
    LOG_ERROR(
        "%s:%d Error in initializing stateful table instances for tbl 0x%x"
        " device id %d, err %s",
        __func__,
        __LINE__,
        t->hdl,
        t->dev,
        pipe_str_err(sts));
    return sts;
  }

  if (!pipe_mgr_is_device_virtual(dev)) {
    sts = pipe_mgr_stful_init_shadow_mem(t);
    if (sts != PIPE_SUCCESS) {
      LOG_ERROR(
          "%s:%d Error in initializing shadow memory metadata for tbl 0x%x"
          " device id %d, err %s",
          __func__,
          __LINE__,
          t->hdl,
          t->dev,
          pipe_str_err(sts));
      return sts;
    }
    /* Update the shadow with the table initial values. */
    set_initial_value(sess_hdl, t, true);
  }

  return sts;
}

bool pipe_mgr_stful_tbl_is_direct(bf_dev_id_t dev, pipe_stful_tbl_hdl_t hdl) {
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev, hdl);
  if (!t) return false;
  return t->direct;
}

void pipe_mgr_stful_spec_decode(bf_dev_id_t dev,
                                pipe_stful_tbl_hdl_t hdl,
                                pipe_stful_mem_spec_t spec,
                                bool *dual,
                                uint32_t *hi,
                                uint32_t *lo) {
  *dual = false;
  *hi = 0;
  *lo = 0;
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev, hdl);
  if (!t) return;

  switch (table_width(t)) {
    case STFL_WDTH_1:
      *lo = spec.bit;
      break;
    case STFL_WDTH_8:
      *lo = spec.byte;
      break;
    case STFL_WDTH_8x2:
      *lo = spec.dbl_byte.lo;
      *hi = spec.dbl_byte.hi;
      *dual = true;
      break;
    case STFL_WDTH_16:
      *lo = spec.half;
      break;
    case STFL_WDTH_16x2:
      *lo = spec.dbl_half.lo;
      *hi = spec.dbl_half.hi;
      *dual = true;
      break;
    case STFL_WDTH_32:
      *lo = spec.word;
      break;
    case STFL_WDTH_32x2:
      *lo = spec.dbl_word.lo;
      *hi = spec.dbl_word.hi;
      *dual = true;
      break;
    case STFL_WDTH_64:
      *lo = spec.dbl;
      break;
    case STFL_WDTH_64x2:
      *lo = spec.dbl_dbl.lo;
      *hi = spec.dbl_dbl.hi;
      *dual = true;
      break;
    default:
      break;
  }
  return;
}

pipe_status_t pipe_mgr_stful_wr_bit(pipe_sess_hdl_t ses,
                                    bf_dev_id_t dev,
                                    bf_dev_pipe_t pipe,
                                    pipe_stful_tbl_hdl_t hdl,
                                    int stage,
                                    int idx,
                                    bool val,
                                    bool adjust_total) {
  pipe_status_t sts = PIPE_SUCCESS;

  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev, hdl);
  if (!t) return PIPE_OBJ_NOT_FOUND;

  /* Table must be in the requested stage. */
  if (!table_is_in_stage(t, stage)) {
    LOG_ERROR("Table %#x is not in stage %d on dev %d", hdl, stage, dev);
    return PIPE_INVALID_ARG;
  }

  /* Table index must be valid. */
  if (!is_valid_index(t, idx, __func__, __LINE__)) return PIPE_INVALID_ARG;

  /* Must be a one bit table. */
  if (STFL_WDTH_1 != table_width(t)) {
    LOG_ERROR("Table %#x on dev %d is not a 1 bit stateful table", hdl, dev);
    return PIPE_INVALID_ARG;
  }

  /* Requested pipe must agree with the selector table's symmetric mode state.*/
  if (t->symmetric != (DEV_PIPE_ALL == pipe)) {
    LOG_ERROR(
        "Invalid pipe %#x specified when accessing %ssymmetric table %#x on "
        "dev %d",
        pipe,
        t->symmetric ? "" : "a",
        hdl,
        dev);
    return PIPE_INVALID_ARG;
  }

  int stage_idx = stage_to_stage_idx(t, stage);

  /* Decide which instruction to run based on the requested operation (set or
   * clear, adjust total or not). */
  int instr_index = -1;
  if (val && !adjust_total)
    instr_index = t->set_instr;
  else if (val && adjust_total)
    instr_index = t->set_at_instr;
  else if (!val && !adjust_total)
    instr_index = t->clr_instr;
  else if (!val && adjust_total)
    instr_index = t->clr_at_instr;

  /* Construct the stateful address based on a 7 bit subword, 10 bit RAM
   * line, and 6 bit VPN. */
  int ram, line, subword;
  decompose_index(STFL_WDTH_1, idx, &ram, &line, &subword);
  int vpn = t->stages[stage_idx].ram_vpn[ram];
  uint32_t vaddr = (vpn & 0x3F) << 17 | (line & 0x3FF) << 7 | (subword & 0x7F);

  int lt = t->stages[stage_idx].logical_table;
  pipe_run_salu_instr_t instr;
  construct_instr_run_salu(t->dev, &instr, instr_index, lt, vaddr);
  int sz = sizeof instr;

  /* Get a pointer to a pipe bitmap for the target pipes.  For symmetric
   * cases simply use the bitmap saved against the table.  Otherwise
   * construct a bit map which has only the requested pipe set. */
  pipe_bitmap_t pipe_bit_map = {{0}};
  pipe_bitmap_t *pbm = NULL;
  if (t->symmetric) {
    pbm = &t->pipes;
  } else {
    /* Table must be in the specified pipe. */
    if (!PIPE_BITMAP_GET(&t->pipes, pipe)) {
      LOG_ERROR("Table %#x on dev %d is not in pipe %d", hdl, dev, pipe);
      return PIPE_INVALID_ARG;
    }
    PIPE_BITMAP_INIT(&pipe_bit_map, PIPE_BMP_SIZE);
    PIPE_BITMAP_SET(&pipe_bit_map, pipe);
    pbm = &pipe_bit_map;
  }

  /* Post the instruction. */
  sts = pipe_mgr_drv_ilist_add(
      &ses, t->dev_info, pbm, stage, (uint8_t *)&instr, sz);
  if (PIPE_SUCCESS != sts) {
    LOG_ERROR(
        "%s:%d Stateful bit write fails, dev %d pipe %x tbl %#x idx %#x stage "
        "%d, err %s",
        __func__,
        __LINE__,
        dev,
        pipe,
        hdl,
        idx,
        stage,
        pipe_str_err(sts));
  }
  return sts;
}

pipe_status_t pipe_mgr_stful_tbl_del(bf_dev_id_t dev,
                                     pipe_stful_tbl_hdl_t hdl) {
  struct pipe_mgr_stful_tbl *t = NULL;
  bf_map_sts_t msts;
  msts = pipe_mgr_stful_tbl_map_get_rmv(dev, hdl, (void **)&t);
  if (BF_MAP_OK != msts) return PIPE_OBJ_NOT_FOUND;
  if (t == NULL) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }
  if (t->stages) {
    for (int s = 0; s < t->num_stages && t->stages[s].db; ++s) {
      for (int p = 0; p < (int)t->dev_info->num_active_pipes; ++p) {
        if (t->stages[s].db[p]) PIPE_MGR_FREE(t->stages[s].db[p]);
      }
      PIPE_MGR_FREE(t->stages[s].db);
      t->stages[s].db = NULL;
    }
    PIPE_MGR_FREE(t->stages);
  }
  unsigned long key = 0;
  int *data = NULL;
  while (BF_MAP_OK ==
         bf_map_get_first_rmv(&t->action_to_instr_map, &key, (void **)&data))
    ;
  PIPE_MGR_FREE(t->tbl_sync_rcvd_sz);
  pipe_mgr_stful_instance_free(t);
  if (t->scope_pipe_bmp) {
    PIPE_MGR_FREE(t->scope_pipe_bmp);
  }
  if (t->cntr_val) {
    PIPE_MGR_FREE(t->cntr_val);
  }
  if (t->hdl_to_shdw) {
    bf_dev_pipe_t pipe;
    PIPE_BITMAP_ITER(&t->pipes, pipe) {
      pipe_stful_mem_spec_t *spec;
      unsigned long unused;
      while (BF_MAP_OK == bf_map_get_first_rmv(
                              &t->hdl_to_shdw[pipe], &unused, (void **)&spec)) {
        if (spec) PIPE_MGR_FREE(spec);
      }
      bf_map_destroy(&t->hdl_to_shdw[pipe]);
    }
    PIPE_MGR_FREE(t->hdl_to_shdw);
  }
  PIPE_MGR_FREE(t);
  return PIPE_SUCCESS;
}

static void set_initial_val(struct pipe_mgr_stful_tbl *t,
                            uint32_t lo,
                            uint32_t hi) {
  switch (table_width(t)) {
    case STFL_WDTH_1:
      t->initial_value.bit = lo;
      break;
    case STFL_WDTH_8:
      t->initial_value.byte = lo;
      break;
    case STFL_WDTH_8x2:
      t->initial_value.dbl_byte.lo = lo;
      t->initial_value.dbl_byte.hi = hi;
      break;
    case STFL_WDTH_16:
      t->initial_value.half = lo;
      break;
    case STFL_WDTH_16x2:
      t->initial_value.dbl_half.lo = lo;
      t->initial_value.dbl_half.hi = hi;
      break;
    case STFL_WDTH_32:
      t->initial_value.word = lo;
      break;
    case STFL_WDTH_32x2:
      t->initial_value.dbl_word.lo = lo;
      t->initial_value.dbl_word.hi = hi;
      break;
    case STFL_WDTH_64:
      t->initial_value.dbl = lo;
      break;
    case STFL_WDTH_64x2:
      t->initial_value.dbl_dbl.lo = lo;
      t->initial_value.dbl_dbl.hi = hi;
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      break;
  }
}

pipe_status_t pipe_mgr_stful_tbl_add(pipe_sess_hdl_t shdl,
                                     bf_dev_id_t dev,
                                     pipe_stful_tbl_hdl_t hdl,
                                     profile_id_t profile,
                                     pipe_bitmap_t *pipes) {
  pipe_status_t status = PIPE_SUCCESS;
  uint32_t q = 0;

  /* Validate handle type. */
  if (PIPE_HDL_TYPE_STFUL_TBL != PIPE_GET_HDL_TYPE(hdl)) {
    return PIPE_INVALID_ARG;
  }
  /* Look up device info. */
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) {
    return PIPE_INVALID_ARG;
  }
  /* Find the device's profile info. */
  rmt_dev_profile_info_t *pi =
      pipe_mgr_get_profile(dev_info, profile, __func__, __LINE__);
  if (!pi) {
    return PIPE_INVALID_ARG;
  }
  PIPE_MGR_DBGCHK(pi->profile_id == profile);

  /* Make sure the table does not already exist. */
  struct pipe_mgr_stful_tbl *t = NULL;
  bf_map_sts_t ms;
  ms = pipe_mgr_stful_tbl_map_get(dev, hdl, (void **)&t);
  if (ms == BF_MAP_OK) {
    LOG_ERROR(
        "%s:%d Attempt to initialize Stful table 0x%x on device %d which "
        "already exists",
        __func__,
        __LINE__,
        hdl,
        dev);
    return PIPE_ALREADY_EXISTS;
  }

  /* Find the table, using its handle, in the profile info. */
  pipe_stful_tbl_info_t *tbl_info = NULL;
  for (unsigned i = 0; i < pi->tbl_info_list.num_sful_tbls; ++i) {
    if (hdl == pi->tbl_info_list.stful_tbl_list[i].handle) {
      tbl_info = &pi->tbl_info_list.stful_tbl_list[i];
      break;
    }
  }
  if (!tbl_info) return PIPE_INVALID_ARG;

  /* Allocate memory to hold info about this table. */
  struct pipe_mgr_stful_tbl *stbl =
      PIPE_MGR_CALLOC(1, sizeof(struct pipe_mgr_stful_tbl));
  if (!stbl) return PIPE_NO_SYS_RESOURCES;
  stbl->tbl_sync_rcvd_sz = PIPE_MGR_CALLOC(dev_info->num_active_pipes,
                                           sizeof(*stbl->tbl_sync_rcvd_sz));
  if (!stbl->tbl_sync_rcvd_sz) return PIPE_NO_SYS_RESOURCES;
  stbl->stages = PIPE_MGR_CALLOC(tbl_info->num_rmt_info,
                                 sizeof(struct pipe_mgr_stful_tbl_stage_info));
  if (!stbl->stages) return PIPE_NO_SYS_RESOURCES;
  unsigned int s;
  for (s = 0; s < tbl_info->num_rmt_info; ++s) {
    stbl->stages[s].db = PIPE_MGR_CALLOC(dev_info->num_active_pipes,
                                         sizeof(*stbl->stages[s].db));
    if (!stbl->stages[s].db) return PIPE_NO_SYS_RESOURCES;
  }
  stbl->cntr_val =
      PIPE_MGR_CALLOC(dev_info->num_active_pipes, sizeof *stbl->cntr_val);
  if (!stbl->cntr_val) return PIPE_NO_SYS_RESOURCES;

  PIPE_BITMAP_INIT(&stbl->pipes, PIPE_BMP_SIZE);
  PIPE_BITMAP_ASSIGN(&stbl->pipes, pipes);

  stbl->dev_info = dev_info;
  stbl->hdl = hdl;
  stbl->sel_tbl_hdl = tbl_info->sel_tbl_hdl;
  stbl->dev = dev;
  stbl->direct = tbl_info->ref_type == PIPE_TBL_REF_TYPE_DIRECT;
  stbl->symmetric = tbl_info->symmetric;
  stbl->scope_pipe_bmp =
      PIPE_MGR_CALLOC(PIPE_BITMAP_COUNT(pipes), sizeof(scope_pipes_t));
  if (!stbl->scope_pipe_bmp) {
    LOG_ERROR("%s:%d Malloc failed", __func__, __LINE__);
    return PIPE_NO_SYS_RESOURCES;
  }
  /* Set the scope info */
  if (stbl->symmetric) {
    stbl->num_scopes = 1;
    PIPE_BITMAP_ITER(pipes, q) { stbl->scope_pipe_bmp[0] |= (1 << q); }
  } else {
    stbl->num_scopes = 0;
    PIPE_BITMAP_ITER(pipes, q) {
      stbl->scope_pipe_bmp[q] |= (1 << q);
      stbl->num_scopes += 1;
    }
  }
  stbl->num_stages = tbl_info->num_rmt_info;
  stbl->width = tbl_info->width;
  PIPE_MGR_DBGCHK(table_width_is_valid(tbl_info->width));
  stbl->dbl_width = tbl_info->dbl_width;
  stbl->num_entries_p4 = tbl_info->size;
  stbl->num_entries_real = 0;
  stbl->set_at_instr = tbl_info->set_instr_at;
  stbl->clr_at_instr = tbl_info->clr_instr_at;

  /* When selector table is configured as Fair mode, compiler will not generate
   * set and clr_instr use set and clear adjust total for modifying the selector
   * bits */
  if (tbl_info->sel_tbl_hdl &&
      pipe_mgr_sel_is_mode_fair(stbl->dev, tbl_info->sel_tbl_hdl)) {
    stbl->set_instr = tbl_info->set_instr_at;
    stbl->clr_instr = tbl_info->clr_instr_at;
  } else {
    stbl->set_instr = tbl_info->set_instr;
    stbl->clr_instr = tbl_info->clr_instr;
  }
  stbl->profile_id = profile;
  stbl->num_reg_params = tbl_info->num_reg_params;
  /* Points to objects alocated in tbl_info struct. */
  stbl->reg_params = tbl_info->reg_params;
  stbl->direction = tbl_info->rmt_info[0].direction;

  if (stbl->symmetric) {
    PIPE_MGR_DBGCHK(stbl->num_scopes == 1);
  }
  stbl->num_tbl_instances = stbl->num_scopes;
  status = pipe_mgr_stful_instance_alloc(stbl);
  if (status != PIPE_SUCCESS) {
    LOG_ERROR(
        "%s:%d Error in initializing stateful table instances for tbl 0x%x"
        " device id %d, err %s",
        __func__,
        __LINE__,
        stbl->hdl,
        stbl->dev,
        pipe_str_err(status));
    return status;
  }
  stbl->tbl_type = tbl_info->is_fifo ? pipe_mgr_stful_tbl_type_fifo
                                     : pipe_mgr_stful_tbl_type_normal;
  stbl->cntr_idx = tbl_info->cntr_index;
  if (!tbl_info->fifo_can_cpu_push && !tbl_info->fifo_can_cpu_pop) {
    stbl->fifo_direction = pipe_mgr_stful_fifo_cpu_none;
  } else if (tbl_info->fifo_can_cpu_push) {
    stbl->fifo_direction = pipe_mgr_stful_fifo_cpu_push;
  } else if (tbl_info->fifo_can_cpu_pop) {
    stbl->fifo_direction = pipe_mgr_stful_fifo_cpu_pop;
  } else {
    stbl->fifo_direction = pipe_mgr_stful_fifo_cpu_none;
  }

  for (unsigned i = 0; i < tbl_info->num_actions; ++i) {
    int *x = (int *)(intptr_t)tbl_info->actions[i].instr_number;
    bf_map_sts_t msts =
        bf_map_add(&stbl->action_to_instr_map, tbl_info->actions[i].act_hdl, x);
    if (BF_MAP_OK != msts) return PIPE_NO_SYS_RESOURCES;
  }

  for (unsigned i = 0; i < tbl_info->num_rmt_info; ++i) {
    stbl->num_entries_real += tbl_info->rmt_info[i].num_entries;
    stbl->stages[i].stage_id = tbl_info->rmt_info[i].stage_id;
    stbl->stages[i].logical_table = tbl_info->rmt_info[i].handle;
    stbl->stages[i].num_entries = tbl_info->rmt_info[i].num_entries;
    stbl->stages[i].num_spare_rams = tbl_info->rmt_info[i].num_spare_rams;
    if (stbl->stages[i].num_spare_rams > 0)
      PIPE_MGR_MEMCPY(stbl->stages[i].spare_rams,
                      tbl_info->rmt_info[i].spare_rams,
                      sizeof(mem_id_t) * stbl->stages[i].num_spare_rams);
    PIPE_MGR_DBGCHK(1 == tbl_info->rmt_info[i].num_tbl_banks);
    stbl->stages[i].num_rams =
        tbl_info->rmt_info[i].bank_map[0].num_tbl_word_blks;
    unsigned j;
    for (j = 0; j < tbl_info->rmt_info[i].bank_map[0].num_tbl_word_blks; ++j) {
      // Assumes one RAM wide.
      stbl->stages[i].ram_ids[j] =
          tbl_info->rmt_info[i].bank_map[0].tbl_word_blk[j].mem_id[0];
      stbl->stages[i].ram_vpn[j] =
          tbl_info->rmt_info[i].bank_map[0].tbl_word_blk[j].vpn_id[0];
    }
    /* For indirect tables allocate memory for a shadow. */
    for (j = 0; !stbl->direct && j < PIPE_BMP_SIZE; ++j) {
      if (!PIPE_BITMAP_GET(&stbl->pipes, j)) continue;
      stbl->stages[i].db_sz = stbl->stages[i].num_rams * TOF_SRAM_UNIT_DEPTH *
                              (TOF_SRAM_UNIT_WIDTH / 8);
      stbl->stages[i].db[j] = PIPE_MGR_CALLOC(stbl->stages[i].db_sz, 1);
      if (!stbl->stages[i].db[j]) return PIPE_NO_SYS_RESOURCES;
    }
    stbl->stages[i].num_alu_ids =
        tbl_info->rmt_info[i].params.stful_params.num_alu_indexes;
    for (j = 0; j < tbl_info->rmt_info[i].params.stful_params.num_alu_indexes;
         j++)
      stbl->stages[i].alu_ids[j] =
          tbl_info->rmt_info[i].params.stful_params.alu_indexes[j];
  }

  /* Allocate shadow for direct tables. */
  if (stbl->direct) {
    stbl->hdl_to_shdw =
        PIPE_MGR_CALLOC(dev_info->num_active_pipes, sizeof *stbl->hdl_to_shdw);
    if (!stbl->hdl_to_shdw) return PIPE_NO_SYS_RESOURCES;
    for (int i = 0; i < PIPE_BMP_SIZE; ++i) {
      if (!PIPE_BITMAP_GET(&stbl->pipes, i)) continue;
      bf_map_init(&stbl->hdl_to_shdw[i]);
    }
  }

  bf_map_sts_t msts;
  msts = pipe_mgr_stful_tbl_map_add(dev, hdl, (void *)stbl);
  if (BF_MAP_OK != msts) {
    PIPE_MGR_FREE(stbl->stages);
    PIPE_MGR_FREE(stbl);
    return PIPE_INIT_ERROR;
  }

  /* Save the initial value for the table. */
  set_initial_val(stbl, tbl_info->initial_val_lo, tbl_info->initial_val_hi);

  if (!pipe_mgr_is_device_virtual(dev)) {
    /* Write the initial value for the entries in this table. */

    /* Initialize shadow memory metadata */
    status = pipe_mgr_stful_init_shadow_mem(stbl);
    if (status != PIPE_SUCCESS) {
      LOG_ERROR(
          "%s:%d Error in initializing shadow memory metadata for tbl 0x%x"
          " device id %d, err %s",
          __func__,
          __LINE__,
          stbl->hdl,
          stbl->dev,
          pipe_str_err(status));
      return status;
    }
    set_initial_value(shdl, stbl, false);
  }

  /* Set the skip_shadow flag last so that the call to set_initial_value just
   * above will go through always. */
  if (tbl_info->sel_tbl_hdl) {
    stbl->skip_shadow = true;
  }

  return PIPE_SUCCESS;
}

int pipe_mgr_stful_log_tbl_info_to_buf(bf_dev_id_t dev,
                                       pipe_stful_tbl_hdl_t hdl,
                                       char *const buf,
                                       size_t s) {
  size_t x = 0;
  char *b = buf;
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev, hdl);
  if (!t) return -1;

  x = snprintf(b,
               s,
               "Table %#x: %s, %s, Pipes:",
               t->hdl,
               t->direct ? "Direct" : "Indirect",
               t->symmetric ? "Symmetric" : "Nonsymmetric");
  if (x <= 0 || x == s) return -1;
  b += x;
  s -= x;

  int i = 0;
  for (i = 0; i < PIPE_BMP_SIZE; ++i) {
    if (!PIPE_BITMAP_GET(&t->pipes, i)) continue;
    x = snprintf(b, s, ", %d", i);
    if (x <= 0 || x == s) return -1;
    b += x;
    s -= x;
  }
  x = snprintf(b, s, " Num of scopes : %d\n", t->num_scopes);
  if (x <= 0 || x == s) return -1;
  b += x;
  s -= x;
  for (i = 0; i < t->num_scopes; i++) {
    x = snprintf(b, s, "  Scope[%d] : 0x%x\n", i, t->scope_pipe_bmp[i]);
    if (x <= 0 || x == s) return -1;
    b += x;
    s -= x;
  }
  x = snprintf(b,
               s,
               "\n\t %sWidth %d-bit Size %d (Requested) %d (Actual)\n",
               t->dbl_width ? "Dbl-" : "",
               t->width,
               t->num_entries_p4,
               t->num_entries_real);
  if (x <= 0 || x == s) return -1;
  b += x;
  s -= x;

  bool dual = false;
  uint32_t hi = 0, lo = 0;
  pipe_mgr_stful_spec_decode(dev, t->hdl, t->initial_value, &dual, &hi, &lo);
  x = snprintf(b, s, "\t Initial Value Hi/Lo: 0x%x_%x\n", hi, lo);
  if (x <= 0 || x == s) return -1;
  b += x;
  s -= x;

  x = snprintf(b,
               s,
               "\t Has-Selector: %s [Set %d, SetAT %d, Clr %d, ClrAT %d]\n",
               t->skip_shadow ? "Y" : "N",
               t->set_instr,
               t->set_at_instr,
               t->clr_instr,
               t->clr_at_instr);
  if (x <= 0 || x == s) return -1;
  b += x;
  s -= x;

  if (t->tbl_type == pipe_mgr_stful_tbl_type_fifo) {
    x = snprintf(b,
                 s,
                 "\t FIFO: CPU-Access-Mode %s Counter %d\n",
                 pipe_mgr_stful_fifo_direction_str(t->fifo_direction),
                 t->cntr_idx);
    if (x <= 0 || x == s) return -1;
    b += x;
    s -= x;
  }

  x = snprintf(b, s, "\t Action-to-Instruction Mapping:\n");
  if (x <= 0 || x == s) return -1;
  b += x;
  s -= x;
  unsigned long act_fn = 0;
  void *instr_p = NULL;
  if (BF_MAP_OK ==
      bf_map_get_first(&t->action_to_instr_map, &act_fn, &instr_p)) {
    int instr = (unsigned long)instr_p;
    x = snprintf(b, s, "\t     %#lx - %d\n", act_fn, instr);
    if (x <= 0 || x == s) return -1;
    b += x;
    s -= x;

    while (BF_MAP_OK ==
           bf_map_get_next(&t->action_to_instr_map, &act_fn, &instr_p)) {
      instr = (unsigned long)instr_p;
      x = snprintf(b, s, "\t     %#lx - %d\n", act_fn, instr);
      if (x <= 0 || x == s) return -1;
      b += x;
      s -= x;
    }
  }

  x = snprintf(b,
               s,
               "\t SyncCB %sNULL, SyncCookie %p SyncStageIdx %d SyncRow %d\n",
               t->sync_cb ? "Not-" : "",
               t->sync_cookie,
               t->sync_stage_idx,
               t->sync_row_idx);
  if (x <= 0 || x == s) return -1;
  b += x;
  s -= x;

  int e = 0;
  for (; e < t->num_stages; ++e) {
    x = snprintf(b,
                 s,
                 "\t Stage %d, Logical Table %d, %d entries, %d ALUs %d RAMs,",
                 t->stages[e].stage_id,
                 t->stages[e].logical_table,
                 t->stages[e].num_entries,
                 t->stages[e].num_alu_ids,
                 t->stages[e].num_rams);
    if (x <= 0 || x == s) return -1;
    b += x;
    s -= x;
    int r;
    if (t->stages[e].num_alu_ids >= 1) {
      x = snprintf(b, s, " ALUs %d", t->stages[e].alu_ids[0]);
      if (x <= 0 || x == s) return -1;
      b += x;
      s -= x;
      for (r = 1; r < t->stages[e].num_alu_ids; r++) {
        x = snprintf(b, s, " %d", t->stages[e].alu_ids[r]);
        if (x <= 0 || x == s) return -1;
        b += x;
        s -= x;
      }
    }
    if (t->stages[e].num_spare_rams >= 1) {
      x = snprintf(b, s, " Spare RAMs %d", t->stages[e].spare_rams[0]);
      if (x <= 0 || x == s) return -1;
      b += x;
      s -= x;
      for (r = 1; r < t->stages[e].num_spare_rams; r++) {
        x = snprintf(b, s, " %d", t->stages[e].spare_rams[r]);
        if (x <= 0 || x == s) return -1;
        b += x;
        s -= x;
      }
    }
    r = 0;
    for (; r < t->stages[e].num_rams; ++r) {
      bool start = 0 == (r % 4);
      x = snprintf(b,
                   s,
                   "%sRAM %2d VPN %2d  ",
                   start ? "\n\t     " : "",
                   t->stages[e].ram_ids[r],
                   t->stages[e].ram_vpn[r]);
      if (x <= 0 || x == s) return -1;
      b += x;
      s -= x;
    }
    x = snprintf(b, s, "\n");
    if (x <= 0 || x == s) return -1;
    b += x;
    s -= x;
  }
  x = snprintf(b, s, "  Stful table instances:\n");
  b += x;
  s -= x;
  uint32_t j = 0;
  for (j = 0; j < t->num_tbl_instances; j++) {
    uint32_t q = 0;
    x = snprintf(b, s, "    Pipe: %d ", t->stful_tbl_inst[j].pipe_id);
    if (x <= 0 || x == s) return -1;
    b += x;
    s -= x;
    x = snprintf(b, s, "    Pipes in bitmap: ");
    if (x <= 0 || x == s) return -1;
    b += x;
    s -= x;
    PIPE_BITMAP_ITER(&(t->stful_tbl_inst[j].pipe_bmp), q) {
      x = snprintf(b, s, "%d ", q);
      if (x <= 0 || x == s) return -1;
      b += x;
      s -= x;
    }
    x = snprintf(b, s, "\n");
    if (x <= 0 || x == s) return -1;
    b += x;
    s -= x;
  }
  x = snprintf(b, s, "  Stful table paramaters:\n");
  b += x;
  s -= x;
  for (j = 0; j < t->num_reg_params; j++) {
    pipe_stful_register_param_t *reg_param = &t->reg_params[j];
    x = snprintf(b, s, "    Name: %s ", reg_param->name);
    if (x <= 0 || x == s) return -1;
    b += x;
    s -= x;
    x = snprintf(b, s, "    Hdl: %d", reg_param->handle);
    if (x <= 0 || x == s) return -1;
    b += x;
    s -= x;
    x = snprintf(b, s, "    Idx: %d", reg_param->reg_file_index);
    if (x <= 0 || x == s) return -1;
    b += x;
    s -= x;
    x = snprintf(b, s, "    Init value: %" PRId64, reg_param->init_value);
    if (x <= 0 || x == s) return -1;
    b += x;
    s -= x;
    x = snprintf(b, s, "\n");
    if (x <= 0 || x == s) return -1;
    b += x;
    s -= x;
  }

  return 0;
}

pipe_status_t pipe_mgr_stful_mgr_decode_virt_addr(
    bf_dev_id_t device_id,
    pipe_stful_tbl_hdl_t stful_tbl_hdl,
    bf_dev_pipe_t pipe_id,
    uint8_t stage_id,
    rmt_virt_addr_t stful_ptr,
    bool *pfe,
    bool *pfe_defaulted,
    uint32_t *stful_idx) {
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(device_id, stful_tbl_hdl);
  if (!t) return PIPE_OBJ_NOT_FOUND;
  if (BF_DEV_PIPE_ALL != pipe_id && !table_is_in_pipe(t, pipe_id))
    return PIPE_INVALID_ARG;
  if (!table_is_in_stage(t, stage_id)) return PIPE_INVALID_ARG;
  *stful_idx = decompose_virt_addr(table_width(t), stful_ptr, pfe);
  *pfe_defaulted = false;

  return PIPE_SUCCESS;
}

static pipe_status_t download_specs_from_shadow(pipe_sess_hdl_t sess_hdl,
                                                bf_dev_id_t device_id,
                                                pipe_stful_tbl_hdl_t tbl_hdl) {
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(device_id, tbl_hdl);
  if (!t) return PIPE_INVALID_ARG;

  /* If this is a stateful selection table skip it, the selection table manager
   * will handle it. */
  if (t->sel_tbl_hdl) return PIPE_SUCCESS;

  pipe_status_t sts = PIPE_SUCCESS;
  /* Skip shadow memory update, since we are updating hardware from shadow
   * memory.  */
  bool skip_shadow = t->skip_shadow;
  t->skip_shadow = true;

  int entry_width = table_bit_width(t);
  if (!entry_width) {
    LOG_ERROR(
        "%s:%d Entry width for table 0x%x is 0", __func__, __LINE__, t->hdl);
    return PIPE_UNEXPECTED;
  }

  int entries_per_row = TOF_SRAM_UNIT_WIDTH / entry_width;

  for (unsigned inst = 0; inst < t->num_tbl_instances; ++inst) {
    /* Get the lowest pipe-id for the instance. */
    struct pipe_mgr_stful_tbl_inst *t_inst = &t->stful_tbl_inst[inst];
    bf_dev_pipe_t pipe_id;
    PIPE_BITMAP_ITER(&t_inst->pipe_bmp, pipe_id) {
      for (int s = 0; s < t->num_stages; ++s) {
        int stage = t->stages[s].stage_id;
        /* Write this to each RAM line of each RAM this table uses. */
        for (uint32_t i = 0; i < t->stages[s].num_entries;
             i += entries_per_row) {
          /* First get the data to write from our shadow. */
          uint32_t ram_line_num = 0;
          mem_id_t mem_id = 0;
          get_shadow_params(t, i, s, &ram_line_num, &mem_id);
          uint8_t *ram_data = NULL;
          sts = pipe_mgr_phy_mem_map_get_ref(t->dev,
                                             t->direction,
                                             pipe_mem_type_unit_ram,
                                             pipe_id,
                                             stage,
                                             mem_id,
                                             ram_line_num,
                                             &ram_data,
                                             true);
          if (PIPE_SUCCESS != sts) {
            LOG_ERROR(
                "Dev %d pipe %d table 0x%x, failed to get shadow ref stage %d "
                "index %d unit %d line %d, %s",
                t->dev,
                pipe_id,
                t->hdl,
                stage,
                i,
                mem_id,
                ram_line_num,
                pipe_str_err(sts));
            t->skip_shadow = skip_shadow;
            return sts;
          }
          /* Write the data with a 128 bit virtual write. */
          int ram_index = 0, ram_line = 0;
          decompose_index(table_width(t), i, &ram_index, &ram_line, NULL);
          uint32_t vpn = t->stages[s].ram_vpn[ram_index];
          uint32_t vaddr = assemble_vaddr(vpn, ram_line, 0xF);
          pipe_instr_set_memdata_v_i_only_t instr;
          construct_instr_set_v_memdata_no_data(t->dev,
                                                &instr,
                                                16,
                                                t->stages[s].logical_table,
                                                pipe_virt_mem_type_sel_stful,
                                                vaddr);
          sts = pipe_mgr_drv_ilist_add_2(&sess_hdl,
                                         t->dev_info,
                                         &t_inst->pipe_bmp,
                                         stage,
                                         (uint8_t *)&instr,
                                         sizeof instr,
                                         ram_data,
                                         16);
          if (PIPE_SUCCESS != sts) {
            LOG_ERROR(
                "Dev %d pipe %d table 0x%x, failed to post v-write stage %d "
                "index %d vaddr 0x%x, %s",
                t->dev,
                pipe_id,
                t->hdl,
                stage,
                i,
                vaddr,
                pipe_str_err(sts));
            t->skip_shadow = skip_shadow;
            return sts;
          }
        } /* For each entry in stage. */
      }   /* For each stage. */
    }     /* For each pipe. */
  }       /* For each scope. */

  t->skip_shadow = skip_shadow;
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stful_download_specs_from_shadow(
    rmt_dev_info_t *dev_info,
    rmt_dev_profile_info_t *dev_profile_info,
    pipe_stful_tbl_info_t *stful_tbl_info,
    void *arg) {
  (void)dev_profile_info;
  pipe_sess_hdl_t shdl = *(pipe_sess_hdl_t *)arg;
  return download_specs_from_shadow(
      shdl, dev_info->dev_id, stful_tbl_info->handle);
}

void pipe_mgr_stful_log_tbl_state(bf_dev_id_t dev_id, void *cookie) {
  dev_target_t dev_tgt = {.device_id = dev_id, .dev_pipe_id = BF_DEV_PIPE_ALL};
  pipe_stful_tbl_hdl_t tbl_hdl;
  struct pipe_mgr_stful_tbl *stful_info = NULL;
  pipe_stful_mem_query_t stful_query;
  pipe_stful_mem_spec_t *stful_spec;
  pipe_stful_mem_spec_t *init;
  int pipe_idx = 0;
  int pipe_id = -1;
  uint32_t inst_idx = 0;
  uint32_t global_base_idx = 0, global_pipe_idx = 0;
  int stage_idx = 0;
  uint32_t base_ent_idx = 0;
  uint32_t num_entries = 0;
  uint32_t i;
  char *bit_array_template = NULL;
  char *bit_array = NULL;
  char *bit_hexstr = NULL;
  uint32_t bit_offset;
  uint32_t bit_array_idx;
  uint32_t bit_idx;
  cJSON *stful_tbl, *stful_pipes, *stful_pipe, *stful_stgs, *stful_stg,
      *dir_ent_arr;
  cJSON *stful_ents, *stful_ent, *curr_ent, *last_ent;
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  if (dev_info == NULL) {
    LOG_ERROR("%s:%d Device info for device id %d not found",
              __func__,
              __LINE__,
              dev_id);
    return;
  }

  int dev_pipes = dev_info->num_active_pipes;
  int dev_stages = dev_info->num_active_mau;
  cJSON *curr_ents[dev_pipes * dev_stages];

  PIPE_MGR_MEMSET(&stful_query, 0, sizeof(pipe_stful_mem_query_t));
  PIPE_MGR_MEMSET(curr_ents, 0, dev_pipes * dev_stages * sizeof(cJSON *));

  stful_tbl = (cJSON *)cookie;
  tbl_hdl = cJSON_GetObjectItem(stful_tbl, "handle")->valueint;

  stful_info = stful_tbl_lkup(dev_id, tbl_hdl);
  if (stful_info == NULL) {
    LOG_ERROR("%s:%d Stful tbl for hdl 0x%x, device id %d not found",
              __func__,
              __LINE__,
              tbl_hdl,
              dev_id);
    return;
  }

  stful_query.data =
      PIPE_MGR_CALLOC(dev_info->num_active_pipes, sizeof *stful_query.data);
  if (!stful_query.data) {
    LOG_ERROR("%s:%d Cannot allocate memory for register table query",
              __func__,
              __LINE__);
    return;
  }
  stful_query.pipe_count = dev_info->num_active_pipes;

  init = &(stful_info->initial_value);
  if (table_width(stful_info) == STFL_WDTH_1) {
    bit_array_template = PIPE_MGR_MALLOC((BITS_PER_LOG_ENTRY / 8) + 1);
    PIPE_MGR_MEMSET(bit_array_template, 0xFF, BITS_PER_LOG_ENTRY / 8);
    bit_array_template[BITS_PER_LOG_ENTRY / 8] = '\0';
  }

  cJSON_AddItemToObject(
      stful_tbl, "stful_pipes", stful_pipes = cJSON_CreateArray());
  pipe_idx = 0;
  for (inst_idx = 0; inst_idx < stful_info->num_tbl_instances; inst_idx++) {
    pipe_id = -1;
    pipe_id = PIPE_BITMAP_GET_NEXT_BIT(
        &stful_info->stful_tbl_inst[inst_idx].pipe_bmp, pipe_id);
    while (pipe_id != -1) {
      cJSON_AddItemToArray(stful_pipes, stful_pipe = cJSON_CreateObject());
      cJSON_AddNumberToObject(stful_pipe, "pipe_id", pipe_id);
      cJSON_AddItemToObject(
          stful_pipe, "stful_stgs", stful_stgs = cJSON_CreateArray());
      if (stful_info->direct) {
        cJSON_AddItemToObject(
            stful_pipe, "direct_ent_hdls", dir_ent_arr = cJSON_CreateArray());
      }
      for (stage_idx = 0; stage_idx < stful_info->num_stages; stage_idx++) {
        cJSON_AddItemToArray(stful_stgs, stful_stg = cJSON_CreateObject());
        cJSON_AddNumberToObject(
            stful_stg, "stage_id", stful_info->stages[stage_idx].stage_id);
        cJSON_AddItemToObject(
            stful_stg, "stful_ents", stful_ents = cJSON_CreateArray());
        curr_ents[pipe_idx * dev_pipes + stage_idx] = NULL;
      }
      pipe_id = PIPE_BITMAP_GET_NEXT_BIT(
          &stful_info->stful_tbl_inst[inst_idx].pipe_bmp, pipe_id);
      pipe_idx++;
    }
  }

  for (inst_idx = 0; inst_idx < stful_info->num_tbl_instances; inst_idx++) {
    dev_tgt.dev_pipe_id = stful_info->stful_tbl_inst[inst_idx].pipe_id;
    /* For direct tables just iterator over all entries in the hdl_to_shdw map
     * to log all entries.
     * For indirect tables just query each possible index in each stage. */
    pipe_mat_ent_hdl_t *dir_ent_hdls = NULL;
    if (stful_info->direct) {
      // Get size of map
      int map_sz = 0;
      unsigned long key;
      void *data = NULL;
      int first_pipe = PIPE_BITMAP_GET_FIRST_SET(
          &stful_info->stful_tbl_inst[inst_idx].pipe_bmp);
      bf_map_t *m = &stful_info->hdl_to_shdw[first_pipe];
      for (bf_map_sts_t msts = bf_map_get_first(m, &key, (void **)&data);
           msts == BF_MAP_OK;
           msts = bf_map_get_next(m, &key, (void **)&data)) {
        ++map_sz;
      }
      if (map_sz) {
        dir_ent_hdls = PIPE_MGR_MALLOC(map_sz * sizeof(pipe_mat_ent_hdl_t));
        if (!dir_ent_hdls) return;
        i = 0;
        for (bf_map_sts_t msts = bf_map_get_first(m, &key, (void **)&data);
             msts == BF_MAP_OK;
             msts = bf_map_get_next(m, &key, (void **)&data)) {
          dir_ent_hdls[i++] = key;
        }
      }
      num_entries = map_sz;
    } else {
      num_entries = stful_info->num_entries_p4;
    }
    for (i = 0; i < num_entries; i++) {
      global_pipe_idx = global_base_idx;
      if (stful_info->direct) {
        pipe_mat_tbl_hdl_t mat_tbl_hdl =
            cJSON_GetObjectItem(stful_tbl, "mat_tbl_hdl")->valueint;
        pipe_mgr_stful_direct_ent_query(pipe_mgr_get_int_sess_hdl(),
                                        dev_id,
                                        mat_tbl_hdl,
                                        dir_ent_hdls[i],
                                        &stful_query,
                                        0);
      } else {
        pipe_mgr_stful_ent_query(
            pipe_mgr_get_int_sess_hdl(), dev_tgt, tbl_hdl, i, &stful_query, 0);
      }
      int idx_in_stage = 0;
      log_index_to_stage_and_offset(stful_info, i, &stage_idx, &idx_in_stage);

      for (pipe_idx = 0; pipe_idx < stful_query.pipe_count; pipe_idx++) {
        stful_pipe = cJSON_GetArrayItem(stful_pipes, global_pipe_idx);
        stful_stgs = cJSON_GetObjectItem(stful_pipe, "stful_stgs");
        stful_stg = cJSON_GetArrayItem(stful_stgs, stage_idx);
        stful_ents = cJSON_GetObjectItem(stful_stg, "stful_ents");
        if (stful_info->direct) {
          dir_ent_arr = cJSON_GetObjectItem(stful_pipe, "direct_ent_hdls");
          cJSON_AddItemToArray(dir_ent_arr,
                               cJSON_CreateNumber(dir_ent_hdls[i]));
        }
        stful_ent = NULL;
        curr_ent = curr_ents[global_pipe_idx * dev_pipes + stage_idx];
        stful_spec = &stful_query.data[pipe_idx];
        switch (table_width(stful_info)) {
          case STFL_WDTH_1: {
            if (stful_spec->bit != init->bit) {
              base_ent_idx = idx_in_stage / BITS_PER_LOG_ENTRY;
              bit_offset = idx_in_stage - (base_ent_idx * BITS_PER_LOG_ENTRY);
              bit_array_idx = bit_offset / 8;
              bit_idx = 7 - (bit_offset % 8);

              if (curr_ent &&
                  cJSON_GetObjectItem(curr_ent, "base_ent_idx")->valuedouble ==
                      (double)(base_ent_idx * BITS_PER_LOG_ENTRY)) {
                stful_ent = curr_ent;
              } else {
                last_ent = NULL;
                for (stful_ent = stful_ents->child; stful_ent;
                     stful_ent = stful_ent->next) {
                  if (cJSON_GetObjectItem(stful_ent, "base_ent_idx")
                          ->valuedouble ==
                      (double)(base_ent_idx * BITS_PER_LOG_ENTRY)) {
                    break;
                  }
                  if (!stful_ent->next) {
                    last_ent = stful_ent;
                  }
                }
                if (!stful_ent) {
                  stful_ent = cJSON_CreateObject();
                  if (last_ent) {
                    last_ent->next = stful_ent;
                    stful_ent->prev = last_ent;
                  } else {
                    stful_ents->child = stful_ent;
                  }
                  cJSON_AddNumberToObject(stful_ent,
                                          "base_ent_idx",
                                          base_ent_idx * BITS_PER_LOG_ENTRY);
                  cJSON_AddStringToObject(
                      stful_ent, "bit_vals", bit_array_template);
                  if (!init->bit) {
                    PIPE_MGR_MEMSET(
                        cJSON_GetObjectItem(stful_ent, "bit_vals")->valuestring,
                        0,
                        BITS_PER_LOG_ENTRY / 8);
                  }
                }
              }

              bit_array =
                  cJSON_GetObjectItem(stful_ent, "bit_vals")->valuestring;
              if (stful_spec->bit) {
                bit_array[bit_array_idx] |= ((char)1 << bit_idx);
              } else {
                bit_array[bit_array_idx] &= ~((char)1 << bit_idx);
              }

              curr_ents[global_pipe_idx * dev_pipes + stage_idx] = stful_ent;
            }
            break;
          }
          case STFL_WDTH_8: {
            if (stful_spec->byte != init->byte) {
              cJSON_AddItemToArray(stful_ents,
                                   stful_ent = cJSON_CreateObject());
              cJSON_AddNumberToObject(stful_ent, "entry_idx", idx_in_stage);
              cJSON_AddNumberToObject(stful_ent, "val", stful_spec->byte);
              if (stful_info->direct)
                cJSON_AddNumberToObject(stful_ent, "ent_hdl", dir_ent_hdls[i]);
            }
            break;
          }
          case STFL_WDTH_8x2: {
            if (stful_spec->dbl_byte.hi != init->dbl_byte.hi ||
                stful_spec->dbl_byte.lo != init->dbl_byte.lo) {
              cJSON_AddItemToArray(stful_ents,
                                   stful_ent = cJSON_CreateObject());
              cJSON_AddNumberToObject(stful_ent, "entry_idx", idx_in_stage);
              cJSON_AddNumberToObject(stful_ent, "hi", stful_spec->dbl_byte.hi);
              cJSON_AddNumberToObject(stful_ent, "lo", stful_spec->dbl_byte.lo);
              if (stful_info->direct)
                cJSON_AddNumberToObject(stful_ent, "ent_hdl", dir_ent_hdls[i]);
            }
            break;
          }
          case STFL_WDTH_16: {
            if (stful_spec->half != init->half) {
              cJSON_AddItemToArray(stful_ents,
                                   stful_ent = cJSON_CreateObject());
              cJSON_AddNumberToObject(stful_ent, "entry_idx", idx_in_stage);
              cJSON_AddNumberToObject(stful_ent, "val", stful_spec->half);
              if (stful_info->direct)
                cJSON_AddNumberToObject(stful_ent, "ent_hdl", dir_ent_hdls[i]);
            }
            break;
          }
          case STFL_WDTH_16x2: {
            if (stful_spec->dbl_half.hi != init->dbl_half.hi ||
                stful_spec->dbl_half.lo != init->dbl_half.lo) {
              cJSON_AddItemToArray(stful_ents,
                                   stful_ent = cJSON_CreateObject());
              cJSON_AddNumberToObject(stful_ent, "entry_idx", idx_in_stage);
              cJSON_AddNumberToObject(stful_ent, "hi", stful_spec->dbl_half.hi);
              cJSON_AddNumberToObject(stful_ent, "lo", stful_spec->dbl_half.lo);
              if (stful_info->direct)
                cJSON_AddNumberToObject(stful_ent, "ent_hdl", dir_ent_hdls[i]);
            }
            break;
          }
          case STFL_WDTH_32: {
            if (stful_spec->word != init->word) {
              cJSON_AddItemToArray(stful_ents,
                                   stful_ent = cJSON_CreateObject());
              cJSON_AddNumberToObject(stful_ent, "entry_idx", idx_in_stage);
              cJSON_AddNumberToObject(stful_ent, "val", stful_spec->word);
              if (stful_info->direct)
                cJSON_AddNumberToObject(stful_ent, "ent_hdl", dir_ent_hdls[i]);
            }
            break;
          }
          case STFL_WDTH_32x2: {
            if (stful_spec->dbl_word.hi != init->dbl_word.hi ||
                stful_spec->dbl_word.lo != init->dbl_word.lo) {
              cJSON_AddItemToArray(stful_ents,
                                   stful_ent = cJSON_CreateObject());
              cJSON_AddNumberToObject(stful_ent, "entry_idx", idx_in_stage);
              cJSON_AddNumberToObject(stful_ent, "hi", stful_spec->dbl_word.hi);
              cJSON_AddNumberToObject(stful_ent, "lo", stful_spec->dbl_word.lo);
              if (stful_info->direct)
                cJSON_AddNumberToObject(stful_ent, "ent_hdl", dir_ent_hdls[i]);
            }
            break;
          }
          case STFL_WDTH_64: {
            if (stful_spec->word != init->word) {
              cJSON_AddItemToArray(stful_ents,
                                   stful_ent = cJSON_CreateObject());
              cJSON_AddNumberToObject(stful_ent, "entry_idx", idx_in_stage);
              cJSON_AddNumberToObject(stful_ent, "val", stful_spec->dbl);
              if (stful_info->direct)
                cJSON_AddNumberToObject(stful_ent, "ent_hdl", dir_ent_hdls[i]);
            }
            break;
          }
          case STFL_WDTH_64x2: {
            if (stful_spec->dbl_word.hi != init->dbl_word.hi ||
                stful_spec->dbl_word.lo != init->dbl_word.lo) {
              cJSON_AddItemToArray(stful_ents,
                                   stful_ent = cJSON_CreateObject());
              cJSON_AddNumberToObject(stful_ent, "entry_idx", idx_in_stage);
              cJSON_AddNumberToObject(stful_ent, "hi", stful_spec->dbl_dbl.hi);
              cJSON_AddNumberToObject(stful_ent, "lo", stful_spec->dbl_dbl.lo);
              if (stful_info->direct)
                cJSON_AddNumberToObject(stful_ent, "ent_hdl", dir_ent_hdls[i]);
            }
            break;
          }
          default:
            break;
        }
        global_pipe_idx++;
      }
    }
    if (dir_ent_hdls) {
      PIPE_MGR_FREE(dir_ent_hdls);
      dir_ent_hdls = NULL;
    }
    global_base_idx = global_pipe_idx;
  }

  if (table_width(stful_info) == STFL_WDTH_1) {
    bit_hexstr = PIPE_MGR_MALLOC((BITS_PER_LOG_ENTRY / 4) + 1);
    bit_hexstr[BITS_PER_LOG_ENTRY / 4] = '\0';
    for (stful_pipe = stful_pipes->child; stful_pipe;
         stful_pipe = stful_pipe->next) {
      stful_stgs = cJSON_GetObjectItem(stful_pipe, "stful_stgs");
      for (stful_stg = stful_stgs->child; stful_stg;
           stful_stg = stful_stg->next) {
        stful_ents = cJSON_GetObjectItem(stful_stg, "stful_ents");
        for (stful_ent = stful_ents->child; stful_ent;
             stful_ent = stful_ent->next) {
          bit_array = cJSON_GetObjectItem(stful_ent, "bit_vals")->valuestring;
          for (bit_array_idx = 0; bit_array_idx < (BITS_PER_LOG_ENTRY / 8);
               bit_array_idx++) {
            sprintf(bit_hexstr + (2 * bit_array_idx),
                    "%02hhX",
                    bit_array[bit_array_idx]);
          }
          PIPE_MGR_FREE(bit_array);
          cJSON_GetObjectItem(stful_ent, "bit_vals")->valuestring =
              bf_sys_strdup(bit_hexstr);
        }
      }
    }
    PIPE_MGR_FREE(bit_hexstr);
    PIPE_MGR_FREE(bit_array_template);
  }

  PIPE_MGR_FREE(stful_query.data);
  stful_info->logging_in_progress = false;
  return;
}

pipe_status_t pipe_mgr_stful_log_state(bf_dev_id_t dev_id,
                                       pipe_stful_tbl_hdl_t tbl_hdl,
                                       cJSON *stful_tbls) {
  pipe_status_t sts = PIPE_SUCCESS;
  dev_target_t dev_tgt = {.device_id = dev_id, .dev_pipe_id = BF_DEV_PIPE_ALL};
  struct pipe_mgr_stful_tbl *stful_info = NULL;
  pipe_mat_tbl_hdl_t mat_tbl_hdl;
  cJSON *stful_tbl;
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  if (!dev_info) return PIPE_INVALID_ARG;

  stful_info = stful_tbl_lkup(dev_id, tbl_hdl);
  if (stful_info == NULL) {
    LOG_ERROR("%s:%d Stful tbl for hdl 0x%x, device id %d not found",
              __func__,
              __LINE__,
              tbl_hdl,
              dev_id);
    return PIPE_OBJ_NOT_FOUND;
  } else if (stful_info->sel_tbl_hdl) {
    /* Don't log stateful selector tables, let the selection manager do it. */
    return PIPE_SUCCESS;
  }

  cJSON_AddItemToArray(stful_tbls, stful_tbl = cJSON_CreateObject());
  cJSON_AddNumberToObject(stful_tbl, "handle", tbl_hdl);
  cJSON_AddBoolToObject(stful_tbl, "symmetric", stful_info->symmetric);

  if (stful_info->direct) {
    sts = pipe_mgr_stful_get_mat_hdl(dev_id, tbl_hdl, &mat_tbl_hdl);
    if (sts != PIPE_SUCCESS) {
      LOG_ERROR("%s:%d Mat tbl hdl for direct stful tbl 0x%x not found",
                __func__,
                __LINE__,
                tbl_hdl);
      return sts;
    }
    cJSON_AddNumberToObject(stful_tbl, "mat_tbl_hdl", mat_tbl_hdl);
    bool cb_now = false;
    sts = pipe_mgr_stful_direct_tbl_sync(pipe_mgr_get_int_sess_hdl(),
                                         dev_tgt,
                                         mat_tbl_hdl,
                                         pipe_mgr_stful_log_tbl_state,
                                         stful_tbl,
                                         &cb_now);

    /* If there are no entries in the table return success. */
    if (sts == PIPE_SUCCESS && cb_now) return PIPE_SUCCESS;
  } else {
    sts = pipe_mgr_stful_tbl_sync(pipe_mgr_get_int_sess_hdl(),
                                  dev_tgt,
                                  tbl_hdl,
                                  pipe_mgr_stful_log_tbl_state,
                                  stful_tbl);
  }

  if (sts == PIPE_SUCCESS) {
    stful_info->logging_in_progress = true;
  }
  return sts;
}

pipe_status_t pipe_mgr_stful_log_cmplt(bf_dev_id_t dev_id,
                                       pipe_stful_tbl_hdl_t tbl_hdl) {
  struct pipe_mgr_stful_tbl *stful_info = NULL;

  stful_info = stful_tbl_lkup(dev_id, tbl_hdl);
  if (stful_info == NULL) {
    LOG_ERROR("%s : No information found for stful table with handle 0x%x",
              __func__,
              tbl_hdl);
    return PIPE_OBJ_NOT_FOUND;
  }

  while (stful_info->logging_in_progress) {
    pipe_mgr_drv_service_drs(dev_id);
  }

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stful_restore_state(bf_dev_id_t dev_id,
                                           cJSON *stful_tbl) {
  pipe_status_t sts = PIPE_SUCCESS;
  pipe_sess_hdl_t sess_hdl = pipe_mgr_get_int_sess_hdl();
  dev_target_t dev_tgt = {.device_id = dev_id, .dev_pipe_id = BF_DEV_PIPE_ALL};
  pipe_stful_tbl_hdl_t tbl_hdl;
  bool symmetric;
  struct pipe_mgr_stful_tbl *stful_info = NULL;
  pipe_stful_mem_spec_t stful_spec;
  pipe_stful_mem_spec_t *init;
  bf_dev_pipe_t pipe_id = BF_INVALID_PIPE;
  int stage_id;
  uint32_t ent_idx = 0;
  uint32_t base_ent_idx = 0;
  char *bit_array = NULL;
  unsigned char byte;
  uint32_t bit_array_idx;
  uint32_t bit_idx;
  cJSON *stful_pipes, *stful_pipe, *stful_stgs, *stful_stg;
  cJSON *stful_ents, *stful_ent;
  scope_pipes_t scopes = 0xf;

  PIPE_MGR_MEMSET(&stful_spec, 0, sizeof(pipe_stful_mem_spec_t));
  dev_tgt.device_id = dev_id;

  tbl_hdl = cJSON_GetObjectItem(stful_tbl, "handle")->valueint;
  stful_info = stful_tbl_lkup(dev_id, tbl_hdl);
  if (stful_info == NULL) {
    LOG_ERROR("%s:%d Stful tbl for hdl 0x%x, device id %d not found",
              __func__,
              __LINE__,
              tbl_hdl,
              dev_id);
    return PIPE_OBJ_NOT_FOUND;
  }
  symmetric = (cJSON_GetObjectItem(stful_tbl, "symmetric")->type == cJSON_True);
  if (symmetric != stful_info->symmetric) {
    sts = pipe_mgr_stful_tbl_set_symmetric_mode(
        sess_hdl, dev_id, tbl_hdl, symmetric, 1, &scopes);
    if (sts != PIPE_SUCCESS) {
      LOG_ERROR("Failed to set %ssymmetric mode on dev %u, stful tbl 0x%x",
                symmetric ? "" : "non-",
                dev_id,
                tbl_hdl);
      return sts;
    }
  }

  stful_info->in_restore = true;
  init = &(stful_info->initial_value);

  stful_pipes = cJSON_GetObjectItem(stful_tbl, "stful_pipes");
  if (!stful_pipes) {
    sts = PIPE_SUCCESS;
    goto done;
  }
  for (stful_pipe = stful_pipes->child; stful_pipe;
       stful_pipe = stful_pipe->next) {
    pipe_id = cJSON_GetObjectItem(stful_pipe, "pipe_id")->valueint;
    dev_tgt.dev_pipe_id = pipe_id;
    if (stful_info->direct) {
      cJSON *dir_ent_hdls = cJSON_GetObjectItem(stful_pipe, "direct_ent_hdls");
      for (int i = 0; i < cJSON_GetArraySize(dir_ent_hdls); ++i) {
        pipe_mat_ent_hdl_t h = cJSON_GetArrayItem(dir_ent_hdls, i)->valuedouble;
        pipe_stful_mem_spec_t *shdw_spec = NULL;
        shdw_spec = PIPE_MGR_MALLOC(sizeof *shdw_spec);
        if (!shdw_spec) {
          LOG_ERROR("%s:%d Malloc failure", __func__, __LINE__);
          return PIPE_NO_SYS_RESOURCES;
        }
        *shdw_spec = stful_spec;
        bf_map_sts_t msts = bf_map_add(
            &stful_info->hdl_to_shdw[dev_tgt.dev_pipe_id], h, shdw_spec);
        if (msts != BF_MAP_OK) {
          LOG_ERROR(
              "%s: Dev %d pipe %x tbl %x error %d setting shadow for entry "
              "%d",
              __func__,
              stful_info->dev,
              dev_tgt.dev_pipe_id,
              stful_info->hdl,
              msts,
              h);
          PIPE_MGR_DBGCHK(0);
          PIPE_MGR_FREE(shdw_spec);
          return PIPE_UNEXPECTED;
        }
      }
    }
    stful_stgs = cJSON_GetObjectItem(stful_pipe, "stful_stgs");
    for (stful_stg = stful_stgs->child; stful_stg;
         stful_stg = stful_stg->next) {
      stage_id = cJSON_GetObjectItem(stful_stg, "stage_id")->valueint;
      stful_ents = cJSON_GetObjectItem(stful_stg, "stful_ents");
      for (stful_ent = stful_ents->child; stful_ent;
           stful_ent = stful_ent->next) {
        switch (table_width(stful_info)) {
          case STFL_WDTH_1: {
            base_ent_idx =
                cJSON_GetObjectItem(stful_ent, "base_ent_idx")->valuedouble;
            bit_array = cJSON_GetObjectItem(stful_ent, "bit_vals")->valuestring;
            PIPE_MGR_DBGCHK(strlen(bit_array) == BITS_PER_LOG_ENTRY / 4);
            for (bit_array_idx = 0; bit_array_idx < BITS_PER_LOG_ENTRY / 4;
                 bit_array_idx += 2) {
              sscanf(bit_array + bit_array_idx, "%2hhx", &byte);
              for (bit_idx = 0; bit_idx < 8; bit_idx++) {
                stful_spec.bit = !!(byte & (1 << (7 - bit_idx)));
                ent_idx = base_ent_idx + bit_array_idx * 4 + bit_idx;
                if (stful_spec.bit != init->bit) {
                  sts =
                      write_one_stage(sess_hdl,
                                      dev_tgt,
                                      stful_info,
                                      stage_to_stage_idx(stful_info, stage_id),
                                      ent_idx,
                                      &stful_spec);
                  if (sts != PIPE_SUCCESS) {
                    LOG_ERROR(
                        "%s:%d Error in writing stful spec for tbl 0x%x,"
                        " dev id %d, stage id %d, entry idx %d",
                        __func__,
                        __LINE__,
                        tbl_hdl,
                        dev_id,
                        stage_id,
                        ent_idx);
                    goto done;
                  }
                }
              }
            }
            break;
          }
          case STFL_WDTH_8: {
            ent_idx = cJSON_GetObjectItem(stful_ent, "entry_idx")->valuedouble;
            stful_spec.byte = cJSON_GetObjectItem(stful_ent, "val")->valueint;
            if (stful_spec.byte != init->byte) {
              sts = write_one_stage(sess_hdl,
                                    dev_tgt,
                                    stful_info,
                                    stage_to_stage_idx(stful_info, stage_id),
                                    ent_idx,
                                    &stful_spec);
              if (sts != PIPE_SUCCESS) {
                LOG_ERROR(
                    "%s:%d Error in writing stful spec for tbl 0x%x,"
                    " dev id %d, stage id %d, entry idx %d",
                    __func__,
                    __LINE__,
                    tbl_hdl,
                    dev_id,
                    stage_id,
                    ent_idx);
                goto done;
              }
            }
            break;
          }
          case STFL_WDTH_8x2: {
            ent_idx = cJSON_GetObjectItem(stful_ent, "entry_idx")->valuedouble;
            stful_spec.dbl_byte.hi =
                cJSON_GetObjectItem(stful_ent, "hi")->valueint;
            stful_spec.dbl_byte.lo =
                cJSON_GetObjectItem(stful_ent, "lo")->valueint;
            if (stful_spec.dbl_byte.hi != init->dbl_byte.hi ||
                stful_spec.dbl_byte.lo != init->dbl_byte.lo) {
              sts = write_one_stage(sess_hdl,
                                    dev_tgt,
                                    stful_info,
                                    stage_to_stage_idx(stful_info, stage_id),
                                    ent_idx,
                                    &stful_spec);
              if (sts != PIPE_SUCCESS) {
                LOG_ERROR(
                    "%s:%d Error in writing stful spec for tbl 0x%x,"
                    " dev id %d, stage id %d, entry idx %d",
                    __func__,
                    __LINE__,
                    tbl_hdl,
                    dev_id,
                    stage_id,
                    ent_idx);
                goto done;
              }
            }
            break;
          }
          case STFL_WDTH_16: {
            ent_idx = cJSON_GetObjectItem(stful_ent, "entry_idx")->valuedouble;
            stful_spec.half = cJSON_GetObjectItem(stful_ent, "val")->valueint;
            if (stful_spec.half != init->half) {
              sts = write_one_stage(sess_hdl,
                                    dev_tgt,
                                    stful_info,
                                    stage_to_stage_idx(stful_info, stage_id),
                                    ent_idx,
                                    &stful_spec);
              if (sts != PIPE_SUCCESS) {
                LOG_ERROR(
                    "%s:%d Error in writing stful spec for tbl 0x%x,"
                    " dev id %d, stage id %d, entry idx %d",
                    __func__,
                    __LINE__,
                    tbl_hdl,
                    dev_id,
                    stage_id,
                    ent_idx);
                goto done;
              }
            }
            break;
          }
          case STFL_WDTH_16x2: {
            ent_idx = cJSON_GetObjectItem(stful_ent, "entry_idx")->valuedouble;
            stful_spec.dbl_half.hi =
                cJSON_GetObjectItem(stful_ent, "hi")->valueint;
            stful_spec.dbl_half.lo =
                cJSON_GetObjectItem(stful_ent, "lo")->valueint;
            if (stful_spec.dbl_half.hi != init->dbl_half.hi ||
                stful_spec.dbl_half.lo != init->dbl_half.lo) {
              sts = write_one_stage(sess_hdl,
                                    dev_tgt,
                                    stful_info,
                                    stage_to_stage_idx(stful_info, stage_id),
                                    ent_idx,
                                    &stful_spec);
              if (sts != PIPE_SUCCESS) {
                LOG_ERROR(
                    "%s:%d Error in writing stful spec for tbl 0x%x,"
                    " dev id %d, stage id %d, entry idx %d",
                    __func__,
                    __LINE__,
                    tbl_hdl,
                    dev_id,
                    stage_id,
                    ent_idx);
                goto done;
              }
            }
            break;
          }
          case STFL_WDTH_32: {
            ent_idx = cJSON_GetObjectItem(stful_ent, "entry_idx")->valuedouble;
            stful_spec.word =
                cJSON_GetObjectItem(stful_ent, "val")->valuedouble;
            if (stful_spec.word != init->word) {
              sts = write_one_stage(sess_hdl,
                                    dev_tgt,
                                    stful_info,
                                    stage_to_stage_idx(stful_info, stage_id),
                                    ent_idx,
                                    &stful_spec);
              if (sts != PIPE_SUCCESS) {
                LOG_ERROR(
                    "%s:%d Error in writing stful spec for tbl 0x%x,"
                    " dev id %d, stage id %d, entry idx %d",
                    __func__,
                    __LINE__,
                    tbl_hdl,
                    dev_id,
                    stage_id,
                    ent_idx);
                goto done;
              }
            }
            break;
          }
          case STFL_WDTH_32x2: {
            ent_idx = cJSON_GetObjectItem(stful_ent, "entry_idx")->valuedouble;
            stful_spec.dbl_word.hi =
                cJSON_GetObjectItem(stful_ent, "hi")->valuedouble;
            stful_spec.dbl_word.lo =
                cJSON_GetObjectItem(stful_ent, "lo")->valuedouble;
            if (stful_spec.dbl_word.hi != init->dbl_word.hi ||
                stful_spec.dbl_word.lo != init->dbl_word.lo) {
              int stage_idx = stage_to_stage_idx(stful_info, stage_id);
              if (stage_idx == -1) {
                PIPE_MGR_DBGCHK(0);
                sts = PIPE_UNEXPECTED;
                goto done;
              }
              sts = write_one_stage(sess_hdl,
                                    dev_tgt,
                                    stful_info,
                                    stage_idx,
                                    ent_idx,
                                    &stful_spec);
              if (sts != PIPE_SUCCESS) {
                LOG_ERROR(
                    "%s:%d Error in writing stful spec for tbl 0x%x,"
                    " dev id %d, stage id %d, entry idx %d",
                    __func__,
                    __LINE__,
                    tbl_hdl,
                    dev_id,
                    stage_id,
                    ent_idx);
                goto done;
              }
            }
            break;
          }
          case STFL_WDTH_64: {
            ent_idx = cJSON_GetObjectItem(stful_ent, "entry_idx")->valuedouble;
            stful_spec.dbl = cJSON_GetObjectItem(stful_ent, "val")->valuedouble;
            if (stful_spec.word != init->word) {
              int stage_idx = stage_to_stage_idx(stful_info, stage_id);
              if (stage_idx >= 0) {
                sts = write_one_stage(sess_hdl,
                                      dev_tgt,
                                      stful_info,
                                      stage_idx,
                                      ent_idx,
                                      &stful_spec);
              } else {
                sts = PIPE_UNEXPECTED;
              }
              if (sts != PIPE_SUCCESS) {
                LOG_ERROR(
                    "%s:%d Error in writing stful spec for tbl 0x%x,"
                    " dev id %d, stage id %d, entry idx %d",
                    __func__,
                    __LINE__,
                    tbl_hdl,
                    dev_id,
                    stage_id,
                    ent_idx);
                goto done;
              }
            }
            break;
          }
          case STFL_WDTH_64x2: {
            ent_idx = cJSON_GetObjectItem(stful_ent, "entry_idx")->valuedouble;
            stful_spec.dbl_dbl.hi =
                cJSON_GetObjectItem(stful_ent, "hi")->valuedouble;
            stful_spec.dbl_dbl.lo =
                cJSON_GetObjectItem(stful_ent, "lo")->valuedouble;
            if (stful_spec.dbl_word.hi != init->dbl_word.hi ||
                stful_spec.dbl_word.lo != init->dbl_word.lo) {
              sts = write_one_stage(sess_hdl,
                                    dev_tgt,
                                    stful_info,
                                    stage_to_stage_idx(stful_info, stage_id),
                                    ent_idx,
                                    &stful_spec);
              if (sts != PIPE_SUCCESS) {
                LOG_ERROR(
                    "%s:%d Error in writing stful spec for tbl 0x%x,"
                    " dev id %d, stage id %d, entry idx %d",
                    __func__,
                    __LINE__,
                    tbl_hdl,
                    dev_id,
                    stage_id,
                    ent_idx);
                goto done;
              }
            }
            break;
          }
          default:
            break;
        }
        if (stful_info->direct) {
          if (STFL_WDTH_1 != table_width(stful_info)) {
            pipe_mat_ent_hdl_t ent_hdl =
                cJSON_GetObjectItem(stful_ent, "ent_hdl")->valuedouble;
            pipe_stful_mem_spec_t *shdw_spec = NULL;
            bf_map_sts_t msts =
                bf_map_get(&stful_info->hdl_to_shdw[dev_tgt.dev_pipe_id],
                           ent_hdl,
                           (void **)&shdw_spec);
            if (msts == BF_MAP_OK) {
              *shdw_spec = stful_spec;
            } else {
              PIPE_MGR_DBGCHK(0);
              return PIPE_UNEXPECTED;
            }
          }
        }
      }
    }
  }

done:
  stful_info->in_restore = false;
  return sts;
}

pipe_status_t pipe_mgr_stful_mgr_get_stful_spec_at(
    bf_dev_id_t device_id,
    pipe_mat_tbl_hdl_t tbl_hdl,
    bf_dev_pipe_t pipe_id,
    uint8_t stage_id,
    uint32_t index_in_stage,
    pipe_stful_mem_spec_t *stful_spec) {
  struct pipe_mgr_stful_tbl *t =
      get_stful_tbl_from_direct_mat(device_id, tbl_hdl);
  if (!t) return PIPE_INVALID_ARG;

  int stage_idx = stage_to_stage_idx(t, stage_id);
  if (stage_idx == -1) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }
  get_spec_from_ram_shadow(pipe_id, t, stage_idx, index_in_stage, stful_spec);
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stful_sbe_correct(bf_dev_id_t dev_id,
                                         bf_dev_pipe_t log_pipe_id,
                                         dev_stage_t stage_id,
                                         pipe_stful_tbl_hdl_t tbl_hdl,
                                         int line) {
  /* If the device is locked we cannot correct anything since the software state
   * may not agree with the hardware state. */
  if (pipe_mgr_is_device_locked(dev_id)) return PIPE_SUCCESS;

  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev_id, tbl_hdl);
  if (!t) return PIPE_INVALID_ARG;

  struct pipe_mgr_stful_tbl_stage_info *stage_info = NULL;
  for (int i = 0; i < t->num_stages; ++i) {
    if (t->stages[i].stage_id == stage_id) {
      stage_info = &t->stages[i];
      break;
    }
  }
  if (!stage_info) return PIPE_INVALID_ARG;

  bf_dev_pipe_t phy_pipe_id;
  pipe_status_t sts =
      pipe_mgr_map_pipe_id_log_to_phy(t->dev_info, log_pipe_id, &phy_pipe_id);
  if (sts) return sts;
  bf_subdev_id_t subdev = pipe_mgr_subdev_id_from_pipe(phy_pipe_id);

  for (int i = 0; i < stage_info->num_rams; ++i) {
    vpn_id_t vpn = stage_info->ram_vpn[i];
    uint32_t low_vir_addr = ram_row_to_cpu_rd_addr(vpn, line);

    pipe_full_virt_addr_t vaddr;
    vaddr.addr = 0;
    construct_full_virt_addr(t->dev_info,
                             &vaddr,
                             stage_info->logical_table,
                             pipe_virt_mem_type_sel_stful,
                             low_vir_addr,
                             phy_pipe_id,
                             stage_id);
    LOG_DBG(
        "Dev %d pipe %d stage %d lt %d stful tbl 0x%x SBE correct vpn %d line "
        "%d virt 0x%" PRIx64,
        dev_id,
        log_pipe_id,
        stage_id,
        stage_info->logical_table,
        tbl_hdl,
        vpn,
        line,
        vaddr.addr);
    uint64_t dont, care;
    lld_subdev_ind_read(dev_id, subdev, vaddr.addr, &dont, &care);
  }
  return PIPE_SUCCESS;
}

static bool get_reg_file_index_from_hdl(struct pipe_mgr_stful_tbl *tbl,
                                        pipe_reg_param_hdl_t hdl,
                                        int *rf_idx,
                                        int64_t *def_val) {
  for (uint32_t i = 0; i < tbl->num_reg_params; i++) {
    if (hdl == tbl->reg_params[i].handle) {
      *rf_idx = tbl->reg_params[i].reg_file_index;
      if (def_val != NULL) {
        *def_val = tbl->reg_params[i].init_value;
      }
      return true;
    }
  }
  return false;
}

static pipe_status_t pipe_mgr_stful_write_atomic_mod_csr(
    pipe_sess_hdl_t sess_hdl,
    struct pipe_mgr_stful_tbl *tbl,
    pipe_bitmap_t *pipe_bmp,
    uint32_t stage_id,
    bool start) {
  pipe_atomic_mod_csr_instr_t instr;
  construct_instr_atomic_mod_csr(tbl->dev, &instr, tbl->direction, start, true);
  return pipe_mgr_drv_ilist_add(&sess_hdl,
                                tbl->dev_info,
                                pipe_bmp,
                                stage_id,
                                (uint8_t *)&instr,
                                sizeof(pipe_atomic_mod_csr_instr_t));
}

static pipe_status_t stful_tof3_set_param(pipe_sess_hdl_t sess_hdl,
                                          struct pipe_mgr_stful_tbl *tbl,
                                          pipe_bitmap_t *pipe_bmp,
                                          dev_stage_t stage,
                                          int alu_idx,
                                          int reg_file_idx,
                                          int64_t value) {
  uint32_t address = 0, data = 0;
  pipe_status_t sts;

  sts =
      pipe_mgr_stful_write_atomic_mod_csr(sess_hdl, tbl, pipe_bmp, stage, true);
  if (sts) return sts;
  /* ALU value is 34 bit signed divided between 2 registers, with 2
   * most significant bits in one and rest in the other. In ilist operations
   * the pipe and stage come from the arguments to ilist_add and are ignored
   * in the address bits. */
  address = offsetof(tof3_reg,
                     pipes[0]
                         .mau[0]
                         .rams.map_alu.meter_group[alu_idx]
                         .stateful.salu_const_regfile[reg_file_idx]);

  data = (uint32_t)(value & 0xFFFFFFFF);

  pipe_instr_write_reg_t instr;
  construct_instr_reg_write(tbl->dev, &instr, address, data);
  sts = pipe_mgr_drv_ilist_add(&sess_hdl,
                               tbl->dev_info,
                               pipe_bmp,
                               stage,
                               (uint8_t *)&instr,
                               sizeof instr);

  // If upper half bit 1 is equal to 0, that means it is positive number,
  // if it is 1 that means it is negative number. Numbers abouve those
  // thresholds should be caught by range check in caller function.
  data = (uint32_t)((value >> 32) & 0x3);
  address = offsetof(tof3_reg,
                     pipes[0]
                         .mau[0]
                         .rams.map_alu.meter_group[alu_idx]
                         .stateful.salu_const_regfile_msbs[reg_file_idx]);

  construct_instr_reg_write(tbl->dev, &instr, address, data);
  sts = pipe_mgr_drv_ilist_add(&sess_hdl,
                               tbl->dev_info,
                               pipe_bmp,
                               stage,
                               (uint8_t *)&instr,
                               sizeof instr);
  if (sts) return sts;

  return pipe_mgr_stful_write_atomic_mod_csr(
      sess_hdl, tbl, pipe_bmp, stage, false);
}

static pipe_status_t stful_tof2_set_param(pipe_sess_hdl_t sess_hdl,
                                          struct pipe_mgr_stful_tbl *tbl,
                                          pipe_bitmap_t *pipe_bmp,
                                          dev_stage_t stage,
                                          int alu_idx,
                                          int reg_file_idx,
                                          int64_t value) {
  uint32_t address = 0, data = 0;
  pipe_status_t sts;

  sts =
      pipe_mgr_stful_write_atomic_mod_csr(sess_hdl, tbl, pipe_bmp, stage, true);
  if (sts) return sts;
  /* ALU value is 34 bit signed divided between 2 registers, with 2
   * most significant bits in one and rest in the other. In ilist operations
   * the pipe and stage come from the arguments to ilist_add and are ignored
   * in the address bits. */
  address = offsetof(tof2_reg,
                     pipes[0]
                         .mau[0]
                         .rams.map_alu.meter_group[alu_idx]
                         .stateful.salu_const_regfile[reg_file_idx]);

  data = (uint32_t)(value & 0xFFFFFFFF);

  pipe_instr_write_reg_t instr;
  construct_instr_reg_write(tbl->dev, &instr, address, data);
  sts = pipe_mgr_drv_ilist_add(&sess_hdl,
                               tbl->dev_info,
                               pipe_bmp,
                               stage,
                               (uint8_t *)&instr,
                               sizeof instr);

  // If upper half bit 1 is equal to 0, that means it is positive number,
  // if it is 1 that means it is negative number. Numbers abouve those
  // thresholds should be caught by range check in caller function.
  data = (uint32_t)((value >> 32) & 0x3);
  address = offsetof(tof2_reg,
                     pipes[0]
                         .mau[0]
                         .rams.map_alu.meter_group[alu_idx]
                         .stateful.salu_const_regfile_msbs[reg_file_idx]);

  construct_instr_reg_write(tbl->dev, &instr, address, data);
  sts = pipe_mgr_drv_ilist_add(&sess_hdl,
                               tbl->dev_info,
                               pipe_bmp,
                               stage,
                               (uint8_t *)&instr,
                               sizeof instr);
  if (sts) return sts;

  return pipe_mgr_stful_write_atomic_mod_csr(
      sess_hdl, tbl, pipe_bmp, stage, false);
}

static pipe_status_t stful_tof_set_param(pipe_sess_hdl_t sess_hdl,
                                         struct pipe_mgr_stful_tbl *tbl,
                                         pipe_bitmap_t *pipe_bmp,
                                         dev_stage_t stage,
                                         int alu_idx,
                                         int reg_file_idx,
                                         int64_t value) {
  uint32_t address = 0, data = 0;
  data = (uint32_t)(value & 0xFFFFFFFF);

  address = offsetof(Tofino,
                     pipes[0]
                         .mau[0]
                         .rams.map_alu.meter_group[alu_idx]
                         .stateful.salu_const_regfile[reg_file_idx]);
  pipe_instr_write_reg_t instr;
  construct_instr_reg_write(tbl->dev, &instr, address, data);
  return pipe_mgr_drv_ilist_add(&sess_hdl,
                                tbl->dev_info,
                                pipe_bmp,
                                stage,
                                (uint8_t *)&instr,
                                sizeof instr);
}

static pipe_status_t stful_set_param(pipe_sess_hdl_t sess_hdl,
                                     pipe_bitmap_t *pipe_bmp,
                                     struct pipe_mgr_stful_tbl *tbl,
                                     int reg_file_idx,
                                     int64_t value) {
  pipe_status_t sts;

  for (int s = 0; s < tbl->num_stages; ++s) {
    for (int alu = 0; alu < tbl->stages[s].num_alu_ids; alu++) {
      switch (tbl->dev_info->dev_family) {
        case BF_DEV_FAMILY_TOFINO3:
          sts = stful_tof3_set_param(sess_hdl,
                                     tbl,
                                     pipe_bmp,
                                     tbl->stages[s].stage_id,
                                     tbl->stages[s].alu_ids[alu],
                                     reg_file_idx,
                                     value);
          break;
        case BF_DEV_FAMILY_TOFINO2:
          sts = stful_tof2_set_param(sess_hdl,
                                     tbl,
                                     pipe_bmp,
                                     tbl->stages[s].stage_id,
                                     tbl->stages[s].alu_ids[alu],
                                     reg_file_idx,
                                     value);
          break;
        case BF_DEV_FAMILY_TOFINO:
          sts = stful_tof_set_param(sess_hdl,
                                    tbl,
                                    pipe_bmp,
                                    tbl->stages[s].stage_id,
                                    tbl->stages[s].alu_ids[alu],
                                    reg_file_idx,
                                    value);
          break;
        default:
          PIPE_MGR_DBGCHK(0);
          return PIPE_UNEXPECTED;
      }
      if (sts) return sts;
    }
  }

  return sts;
}

static pipe_status_t stful_tof_get_param(bf_dev_id_t dev,
                                         bf_dev_pipe_t phy_pipe,
                                         dev_stage_t stage,
                                         int alu_idx,
                                         int reg_file_idx,
                                         int64_t *value) {
  uint32_t address = 0;
  int32_t data = 0;
  pipe_status_t sts;

  address = offsetof(Tofino,
                     pipes[phy_pipe]
                         .mau[stage]
                         .rams.map_alu.meter_group[alu_idx]
                         .stateful.salu_const_regfile[reg_file_idx]);

  sts = lld_read_register(dev, address, (uint32_t *)&data);
  if (sts) return sts;

  *value = data;

  return sts;
}

static pipe_status_t stful_tof2_get_param(bf_dev_id_t dev,
                                          bf_dev_pipe_t phy_pipe,
                                          dev_stage_t stage,
                                          int alu_idx,
                                          int reg_file_idx,
                                          int64_t *value) {
  uint32_t address = 0, data = 0;
  uint64_t tmp;
  pipe_status_t sts;

  /* ALU value is 34 bit signed divided between 2 registers, with 2
   * most significant bits in one and rest in the other. */
  address = offsetof(tof2_reg,
                     pipes[phy_pipe]
                         .mau[stage]
                         .rams.map_alu.meter_group[alu_idx]
                         .stateful.salu_const_regfile[reg_file_idx]);

  sts = lld_read_register(dev, address, &data);
  if (sts) return sts;

  // Lower 32 bits
  tmp = data;

  address = offsetof(tof2_reg,
                     pipes[phy_pipe]
                         .mau[stage]
                         .rams.map_alu.meter_group[alu_idx]
                         .stateful.salu_const_regfile_msbs[reg_file_idx]);

  sts = lld_read_register(dev, address, &data);
  if (sts) return sts;

  // Get upper 2 bits into the temp uint64
  tmp |= ((uint64_t)data & 0x3) << 32;
  // With cast right bit shift will take care of sign in the final value.
  *value = ((int64_t)tmp << 30) >> 30;

  return sts;
}

static pipe_status_t stful_tof3_get_param(bf_dev_id_t dev,
                                          bf_dev_pipe_t phy_pipe,
                                          dev_stage_t stage,
                                          int alu_idx,
                                          int reg_file_idx,
                                          int64_t *value) {
  uint32_t address = 0, data = 0;
  uint64_t tmp;
  pipe_status_t sts;

  /* ALU value is 34 bit signed divided between 2 registers, with 2
   * most significant bits in one and rest in the other. */
  address = offsetof(tof3_reg,
                     pipes[phy_pipe]
                         .mau[stage]
                         .rams.map_alu.meter_group[alu_idx]
                         .stateful.salu_const_regfile[reg_file_idx]);

  sts = lld_read_register(dev, address, &data);
  if (sts) return sts;

  // Lower 32 bits
  tmp = data;

  address = offsetof(tof3_reg,
                     pipes[phy_pipe]
                         .mau[stage]
                         .rams.map_alu.meter_group[alu_idx]
                         .stateful.salu_const_regfile_msbs[reg_file_idx]);

  sts = lld_read_register(dev, address, &data);
  if (sts) return sts;

  // Get upper 2 bits into the temp uint64
  tmp |= ((uint64_t)data & 0x3) << 32;
  // With cast right bit shift will take care of sign in the final value.
  *value = ((int64_t)tmp << 30) >> 30;

  return sts;
}

static pipe_status_t stful_get_param(bf_dev_pipe_t pipe,
                                     struct pipe_mgr_stful_tbl *tbl,
                                     int reg_file_idx,
                                     int64_t *value) {
  bf_dev_id_t dev = tbl->dev_info->dev_id;
  bf_dev_pipe_t phy_pipe = 0;
  pipe_status_t sts;

  sts = pipe_mgr_map_pipe_id_log_to_phy(tbl->dev_info, pipe, &phy_pipe);
  if (sts) return sts;

  for (int s = 0; s < tbl->num_stages; ++s) {
    for (int alu = 0; alu < tbl->stages[s].num_alu_ids; alu++) {
      switch (tbl->dev_info->dev_family) {
        case BF_DEV_FAMILY_TOFINO3:
          sts = stful_tof3_get_param(dev,
                                     phy_pipe,
                                     tbl->stages[s].stage_id,
                                     tbl->stages[s].alu_ids[alu],
                                     reg_file_idx,
                                     value);
          break;
        case BF_DEV_FAMILY_TOFINO2:
          sts = stful_tof2_get_param(dev,
                                     phy_pipe,
                                     tbl->stages[s].stage_id,
                                     tbl->stages[s].alu_ids[alu],
                                     reg_file_idx,
                                     value);
          break;
        case BF_DEV_FAMILY_TOFINO:
          sts = stful_tof_get_param(dev,
                                    phy_pipe,
                                    tbl->stages[s].stage_id,
                                    tbl->stages[s].alu_ids[alu],
                                    reg_file_idx,
                                    value);
          break;
        default:
          PIPE_MGR_DBGCHK(0);
          return PIPE_UNEXPECTED;
      }
      if (sts) return sts;
    }
  }

  return sts;
}

pipe_status_t pipe_mgr_stful_param_set(pipe_sess_hdl_t sess_hdl,
                                       dev_target_t dev_tgt,
                                       pipe_tbl_hdl_t tbl_hdl,
                                       pipe_reg_param_hdl_t rp_hdl,
                                       int64_t value,
                                       bool reset) {
  pipe_status_t sts;
  pipe_stful_tbl_hdl_t stful_hdl;

  /* Validate the dev target. */
  if (!pipe_mgr_valid_dev_tgt(dev_tgt, __func__, __LINE__))
    return PIPE_INVALID_ARG;

  /* Validate handle type. */
  if (PIPE_HDL_TYPE_MAT_TBL == PIPE_GET_HDL_TYPE(tbl_hdl)) {
    sts = pipe_mgr_stful_get_direct_stful_hdl(
        dev_tgt.device_id, tbl_hdl, &stful_hdl);
    if (sts) return sts;
  } else if (PIPE_HDL_TYPE_STFUL_TBL == PIPE_GET_HDL_TYPE(tbl_hdl)) {
    stful_hdl = tbl_hdl;
  } else {
    return PIPE_INVALID_ARG;
  }

  /* Lookup the table, SM layer has already validated that the table exists. */
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev_tgt.device_id, stful_hdl);
  if (t == NULL) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }

  /* Validate value range for ALU width. Sign bit is not included in length
   * except Tofino 1 32bit case. Register params with width 1 are not supported
   * by the compiler. */
  if (t->width == 1) {
    return PIPE_INVALID_ARG;
  } else if (t->width == 16) {
    if ((value > (1 << 16) - 1) || (value < (1 << 16) * -1))
      return PIPE_INVALID_ARG;
  } else if (t->width == 8) {
    if ((value > (1 << 8) - 1) || (value < (1 << 8) * -1))
      return PIPE_INVALID_ARG;
  } else if (t->width == 32 || t->width == 64) {
    switch (t->dev_info->dev_family) {
      case BF_DEV_FAMILY_TOFINO2:
        /* FALLTHRU */
      case BF_DEV_FAMILY_TOFINO3:
        if (value > (1LL << 32) - 1 || value < (1LL << 32) * -1) {
          return PIPE_INVALID_ARG;
        }
        break;
      case BF_DEV_FAMILY_TOFINO:
        if (value > INT_MAX || value < INT_MIN) {
          return PIPE_INVALID_ARG;
        }
        break;
      default:
        PIPE_MGR_DBGCHK(0);
        return PIPE_UNEXPECTED;
    }
  } else {
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }

  /* Validate register param handle and get default. */
  int reg_file_idx;
  int64_t def_val;
  if (!get_reg_file_index_from_hdl(t, rp_hdl, &reg_file_idx, &def_val)) {
    return PIPE_INVALID_ARG;
  }
  if (reset) {
    value = def_val;
  }

  /* Determine which pipes to operate on. */
  pipe_bitmap_t *pipe_bmp = NULL;

  if (BF_DEV_PIPE_ALL == dev_tgt.dev_pipe_id) {
    pipe_bmp = &t->pipes;
  } else {
    if (!table_is_in_pipe(t, dev_tgt.dev_pipe_id)) return PIPE_INVALID_ARG;
    if (t->symmetric) {
      return PIPE_INVALID_ARG;
    } else {
      /* Table is not symmetric so the specified pipe should be the first pipe
       * in a table instance. */
      struct pipe_mgr_stful_tbl_inst *stbl_inst = NULL;
      stbl_inst = get_stful_tbl_inst_by_pipe_id(t, dev_tgt.dev_pipe_id);
      if (!stbl_inst) {
        LOG_ERROR("%s:%d Stful table inst lookup failure", __func__, __LINE__);
        return PIPE_INVALID_ARG;
      }
      pipe_bmp = &stbl_inst->pipe_bmp;
    }
  }
  if (!pipe_bmp) return PIPE_INVALID_ARG;

  sts = stful_set_param(sess_hdl, pipe_bmp, t, reg_file_idx, value);
  if (sts) return sts;

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stful_param_get(pipe_sess_hdl_t sess_hdl,
                                       dev_target_t dev_tgt,
                                       pipe_tbl_hdl_t tbl_hdl,
                                       pipe_reg_param_hdl_t rp_hdl,
                                       int64_t *value) {
  (void)sess_hdl;
  pipe_status_t sts;
  pipe_stful_tbl_hdl_t stful_hdl;

  /* Validate the dev target. */
  if (!pipe_mgr_valid_dev_tgt(dev_tgt, __func__, __LINE__))
    return PIPE_INVALID_ARG;

  /* Validate handle type. */
  if (PIPE_HDL_TYPE_MAT_TBL == PIPE_GET_HDL_TYPE(tbl_hdl)) {
    sts = pipe_mgr_stful_get_direct_stful_hdl(
        dev_tgt.device_id, tbl_hdl, &stful_hdl);
    if (sts) return sts;
  } else if (PIPE_HDL_TYPE_STFUL_TBL == PIPE_GET_HDL_TYPE(tbl_hdl)) {
    stful_hdl = tbl_hdl;
  } else {
    return PIPE_INVALID_ARG;
  }

  /* Lookup the table, SM layer has already validated that the table exists. */
  struct pipe_mgr_stful_tbl *t = stful_tbl_lkup(dev_tgt.device_id, stful_hdl);
  if (t == NULL) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }

  /* Validate register param handle. */
  int reg_file_idx;
  if (!get_reg_file_index_from_hdl(t, rp_hdl, &reg_file_idx, NULL)) {
    return PIPE_INVALID_ARG;
  }

  /* Determine which pipes to operate on and store them in a bit mask. */
  unsigned int pipe_msk = 0;
  if (BF_DEV_PIPE_ALL == dev_tgt.dev_pipe_id) {
    unsigned int b;
    PIPE_BITMAP_ITER(&t->pipes, b) { pipe_msk |= 1 << b; }
  } else {
    if (!table_is_in_pipe(t, dev_tgt.dev_pipe_id)) return PIPE_INVALID_ARG;
    if (t->symmetric) {
      return PIPE_INVALID_ARG;
    } else {
      /* Table is not symmetric so the specified pipe should be the first pipe
       * in a table instance. */
      unsigned int b;
      struct pipe_mgr_stful_tbl_inst *stbl_inst = NULL;
      stbl_inst = get_stful_tbl_inst_by_pipe_id(t, dev_tgt.dev_pipe_id);
      if (!stbl_inst) {
        LOG_ERROR("%s:%d Stful table inst lookup failure", __func__, __LINE__);
        return PIPE_INVALID_ARG;
      }
      PIPE_BITMAP_ITER(&stbl_inst->pipe_bmp, b) { pipe_msk |= 1 << b; }
    }
  }
  if (!pipe_msk) return PIPE_INVALID_ARG;

#define STFUL_ALU_INVALID_VAL (int64_t)(~0LL)
  int64_t temp_val = 0;
  *value = STFUL_ALU_INVALID_VAL;

  for (int pipe = 0; pipe < (int)t->dev_info->num_active_pipes; ++pipe) {
    if (!(pipe_msk & (1u << pipe))) continue;
    sts = stful_get_param(pipe, t, reg_file_idx, &temp_val);
    if (sts) return sts;
    if (*value != STFUL_ALU_INVALID_VAL && temp_val != *value) {
      LOG_ERROR(
          "%s:%d Register param hdl %d values differ between pipes, returning "
          "value from the first pipe in the scope",
          __func__,
          __LINE__,
          rp_hdl);
      return PIPE_INVALID_ARG;
    }
    *value = temp_val;
  }

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_stful_param_get_hdl(bf_dev_id_t dev,
                                           const char *name,
                                           pipe_reg_param_hdl_t *hdl) {
  struct pipe_mgr_stful_tbl *t = NULL;
  struct pipe_mgr_dev_ctx *ctx;
  unsigned long key;

  ctx = pipe_mgr_dev_ctx_(dev);
  if (!ctx) return PIPE_INVALID_ARG;

  PIPE_MGR_LOCK(&ctx->stful_tbl_mtx);
  bf_map_t *m = &ctx->stful_tbls;
  for (bf_map_sts_t msts = bf_map_get_first(m, &key, (void **)&t);
       msts == BF_MAP_OK;
       msts = bf_map_get_next(m, &key, (void **)&t)) {
    for (uint32_t i = 0; i < t->num_reg_params; i++) {
      if (!strcmp(name, t->reg_params[i].name)) {
        *hdl = t->reg_params[i].handle;
        PIPE_MGR_UNLOCK(&ctx->stful_tbl_mtx);
        return PIPE_SUCCESS;
      }
    }
  }
  PIPE_MGR_UNLOCK(&ctx->stful_tbl_mtx);

  return PIPE_OBJ_NOT_FOUND;
}
