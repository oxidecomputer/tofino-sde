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
 * @file pipe_mgr_instr.h
 * @date
 *
 * Utilities to build MAU instructions.
 */
#ifndef PIPE_MGR_INSTR_H
#define PIPE_MGR_INSTR_H

#ifdef __sun
#include <endian.h>
#endif
#include "lld/lld_inst_list_fmt.h"


/* Construct an instruction to do a physical write; does not include the data
 * to be written. */
static inline void construct_instr_set_memdata_no_data(
    bf_dev_id_t dev_id,
    pipe_instr_set_memdata_i_only_t *instr,
    int datasz,
    mem_id_t mem_id,
    pipe_tbl_dir_t gress,
    dev_stage_t stage_id,
    uint32_t addr,
    pipe_mem_type_t mem_type) {
  (void)gress;
  (void)stage_id;
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  if (!dev_info) {
    PIPE_MGR_DBGCHK(0);
    return;
  }
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
    case BF_DEV_FAMILY_TOFINO2:
    case BF_DEV_FAMILY_TOFINO3:
      instr->tf1.pipe_ring_addr_type = addr_type_memdata;
      switch (datasz) {
        case 16:
          instr->tf1.data_width = 3;
          break;
        case 8:
          instr->tf1.data_width = 2;
          break;
        case 4:
          instr->tf1.data_width = 1;
          break;
        default:
          PIPE_MGR_DBGCHK(0);
          break;
      }
      instr->tf1.mem_type = mem_type;
      instr->tf1.mem_col = dev_info->dev_cfg.mem_id_to_col(mem_id, mem_type);
      instr->tf1.mem_row = dev_info->dev_cfg.mem_id_to_row(mem_id, mem_type);
      instr->tf1.mem_address = addr;
      break;





    default:
      PIPE_MGR_DBGCHK(0);
  }
}

static inline void construct_instr_set_memdata_by_addr(
    rmt_dev_info_t *dev_info,
    pipe_instr_set_memdata_i_only_t *instr,
    int datasz,
    uint32_t lower_addr_bits) {
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
    case BF_DEV_FAMILY_TOFINO2:
    case BF_DEV_FAMILY_TOFINO3:
      instr->tf1.pipe_ring_addr_type = addr_type_memdata;
      switch (datasz) {
        case 16:
          instr->tf1.data_width = 3;
          break;
        case 8:
          instr->tf1.data_width = 2;
          break;
        case 4:
          instr->tf1.data_width = 1;
          break;
        default:
          PIPE_MGR_DBGCHK(0);
          break;
      }
      ((pipe_instr_common_wd0_t *)instr)->specific = lower_addr_bits;
      break;

    default:
      PIPE_MGR_DBGCHK(0);
  }
}
/* Construct an instruction to do a physical write. */
static inline void construct_instr_set_memdata(rmt_dev_info_t *dev_info,
                                               pipe_instr_set_memdata_t *instr,
                                               uint8_t *data,
                                               int datasz,
                                               mem_id_t mem_id,
                                               pipe_tbl_dir_t gress,
                                               dev_stage_t stage_id,
                                               uint32_t addr,
                                               pipe_mem_type_t mem_type) {
  int data_sz_in32b = datasz / 4;
  int size;
  uint32_t *buf;
  (void)gress;
  (void)stage_id;
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
    case BF_DEV_FAMILY_TOFINO2:
    case BF_DEV_FAMILY_TOFINO3:
      instr->head.tf1.pipe_ring_addr_type = addr_type_memdata;
      switch (datasz) {
        case 16:
          instr->head.tf1.data_width = 3;
          break;
        case 8:
          instr->head.tf1.data_width = 2;
          break;
        case 4:
          instr->head.tf1.data_width = 1;
          break;
        default:
          PIPE_MGR_DBGCHK(0);
          break;
      }
      instr->head.tf1.mem_type = mem_type;
      instr->head.tf1.mem_col =
          dev_info->dev_cfg.mem_id_to_col(mem_id, mem_type);
      instr->head.tf1.mem_row =
          dev_info->dev_cfg.mem_id_to_row(mem_id, mem_type);
      instr->head.tf1.mem_address = addr;
      break;





    default:
      PIPE_MGR_DBGCHK(0);
      return;
  }
  buf = (uint32_t *)data;
  for (size = 0; size < data_sz_in32b; size++) {
    instr->data[size] = le32toh(*(buf + size));
  }
}

/* Construct an instruction to do a physical read. */
static inline void construct_instr_get_memdata(bf_dev_id_t dev_id,
                                               pipe_instr_get_memdata_t *instr,
                                               int mem_id,
                                               uint32_t addr,
                                               pipe_mem_type_t mem_type) {
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  if (!dev_info) {
    PIPE_MGR_DBGCHK(0);
    return;
  }
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->pipe_ring_addr_type = addr_type_memdata;
  /* Data width is always zero for reads since no data is sent. */
  instr->data_width = 0;
  instr->mem_type = mem_type;
  instr->mem_col = dev_info->dev_cfg.mem_id_to_col(mem_id, mem_type);
  instr->mem_row = dev_info->dev_cfg.mem_id_to_row(mem_id, mem_type);
  instr->mem_address = addr;
}

/* Construct a virtual address. */
static inline void construct_full_virt_addr(rmt_dev_info_t *dev_info,
                                            pipe_full_virt_addr_t *addr,
                                            int logical_tbl_id,
                                            pipe_virt_mem_type_e v_type,
                                            uint32_t vaddr,
                                            bf_dev_pipe_t pipe_id,
                                            int stage) {
  PIPE_MGR_MEMSET(addr, 0, sizeof *addr);
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      addr->tof.v_address = vaddr;
      addr->tof.table_id = logical_tbl_id;
      addr->tof.v_mem_type = v_type;
      addr->tof.pipe_ring_addr_type = addr_type_memdata_v;
      addr->tof.stage_id = stage;
      addr->tof.pipe_id = pipe_id;
      addr->tof.bits_41_40 = 2;
      return;
    case BF_DEV_FAMILY_TOFINO2:
      addr->tof2.v_address = vaddr;
      addr->tof2.table_id = logical_tbl_id;
      addr->tof2.v_mem_type = v_type;
      addr->tof2.pipe_ring_addr_type = addr_type_memdata_v;
      addr->tof2.stage_id = stage;
      addr->tof2.pipe_id = pipe_id;
      addr->tof2.pipe_always_1 = 1;
      return;
    case BF_DEV_FAMILY_TOFINO3:
      addr->tof3.v_address = vaddr;
      addr->tof3.table_id = logical_tbl_id;
      addr->tof3.v_mem_type = v_type;
      addr->tof3.pipe_ring_addr_type = addr_type_memdata_v;
      addr->tof3.stage_id = stage;
      addr->tof3.pipe_id = pipe_id;
      addr->tof3.pipe_always_1 = 1;
      return;
    default:
      PIPE_MGR_DBGCHK(0);
      return;
  }
}
/* Construct an instruction to do a virtual write; does not include the data. */
static inline void construct_instr_set_v_memdata_no_data(
    bf_dev_id_t dev_id,
    pipe_instr_set_memdata_v_i_only_t *instr,
    int datasz,
    int logical_tbl_id,
    pipe_virt_mem_type_e v_type,
    uint32_t vaddr) {
  (void)dev_id;
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->pipe_ring_addr_type = addr_type_memdata_v;
  switch (datasz) {
    case 16:
      instr->data_width = 3;
      break;
    case 8:
      instr->data_width = 2;
      break;
    case 4:
      instr->data_width = 1;
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      break;
  }
  instr->table_id = logical_tbl_id;
  instr->v_mem_type = v_type;
  instr->v_address = vaddr;
}

/* Construct an instruction to do a virtual write. */
static inline void construct_instr_set_v_memdata(
    bf_dev_id_t dev_id,
    pipe_instr_set_memdata_v_t *instr,
    uint8_t *data,
    int datasz,
    int logical_tbl_id,
    pipe_virt_mem_type_e v_type,
    uint32_t vaddr) {
  (void)dev_id;
  int data_sz_in32b = datasz / 4;
  int size = 0;
  uint32_t *buf;

  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->s.pipe_ring_addr_type = addr_type_memdata_v;
  switch (datasz) {
    case 16:
      instr->s.data_width = 3;
      break;
    case 8:
      instr->s.data_width = 2;
      break;
    case 4:
      instr->s.data_width = 1;
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      break;
  }
  instr->s.table_id = logical_tbl_id;
  instr->s.v_mem_type = v_type;
  instr->s.v_address = vaddr;
  buf = (uint32_t *)data;
  for (size = 0; size < data_sz_in32b; size++) {
    instr->data[size] = le32toh(*(buf + size));
  }
}

/* Construct an instruction to do a virtual read. */
static inline void construct_instr_get_v_memdata(
    bf_dev_id_t dev_id,
    pipe_instr_get_memdata_v_t *instr,
    int logical_tbl_id,
    pipe_virt_mem_type_e v_type,
    uint32_t vaddr) {
  (void)dev_id;
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->s.pipe_ring_addr_type = addr_type_memdata_v;
  instr->s.v_mem_type = v_type;
  instr->s.table_id = logical_tbl_id;
  instr->s.v_address = vaddr;
}

/* Construct an instruction to handle atomic csr modify */
static inline void construct_instr_atomic_mod_csr(
    bf_dev_id_t dev_id,
    pipe_atomic_mod_csr_instr_t *instr,
    uint32_t dir,
    bool start,
    bool wide) {
  (void)dev_id;
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->dir = dir;
  instr->start = start ? 0 : 1;
  instr->wide = wide;
  instr->opcode = INSTR_OPCODE_ATOMIC_MOD_CSR;
  instr->data_width = 0;
  instr->pipe_ring_addr_type = addr_type_instruction;
}

/* Construct an instruction to handle atomic sram modify */
static inline void construct_instr_atomic_mod_sram(
    bf_dev_id_t dev_id, pipe_atomic_mod_sram_instr_t *instr, uint32_t dir) {
  (void)dev_id;
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->dir = dir;
  instr->opcode = INSTR_OPCODE_ATOMIC_MOD_SRAM;
  instr->data_width = 0;
  instr->pipe_ring_addr_type = addr_type_instruction;
}

/* Construct an instruction to start a stats table dump. */
static inline void construct_instr_stat_dump_tbl(
    bf_dev_id_t dev_id, pipe_dump_stat_tbl_instr_t *instr, int logical_tbl) {
  (void)dev_id;
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->tbl_id = logical_tbl;
  instr->opcode = INSTR_OPCODE_DUMP_STAT_TABLE;
  instr->data_width = 0;
  instr->pipe_ring_addr_type = addr_type_instruction;
}

/* Construct an instruction to dump one stats table entry. */
static inline void construct_instr_stat_dump_entry(
    bf_dev_id_t dev_id,
    pipe_dump_stat_ent_instr_t *instr,
    uint32_t addr,
    int logical_tbl) {
  (void)dev_id;
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->addr = addr & 0x7ffff;
  instr->tbl_id = logical_tbl;
  instr->opcode = INSTR_OPCODE_DUMP_STAT_TABLE_ENTRY;
  instr->data_width = 0;
  instr->pipe_ring_addr_type = addr_type_instruction;
}

/* Construct an instruction to start a stats table dump. */
static inline void construct_instr_idle_dump_tbl(
    bf_dev_id_t dev_id,
    pipe_dump_idle_table_instr_t *instr,
    int logical_tbl,
    bool clear) {
  (void)dev_id;
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->tbl_id = logical_tbl;
  instr->clear = clear;
  instr->opcode = INSTR_OPCODE_DUMP_IDLE_TABLE;
  instr->pipe_ring_addr_type = addr_type_instruction;
}

/* Construct an instruction to do a TCAM copy. */
static inline void construct_instr_tcam_copy(bf_dev_id_t dev_id,
                                             pipe_tcam_copy_instr_t *instr) {
  (void)dev_id;
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->pipe_ring_addr_type = addr_type_instruction;
  instr->data_width = 1;
  instr->opcode = INSTR_OPCODE_TCAM_COPY_DATA;
  instr->addr_inc_dir = 1;
  instr->num_words = 1;
}

/* Construct the data for a TCAM copy instruction. */
static inline void construct_instr_tcam_copy_data(
    bf_dev_id_t dev_id,
    pipe_tcam_copy_instr_data_t *instr,
    int dst_line,
    int src_line,
    int top_row,
    int bottom_row,
    int dst_col,
    int src_col) {
  (void)dev_id;
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->dest_line_no = dst_line;
  instr->src_line_no = src_line;
  instr->top_row = top_row;
  instr->bottom_row = bottom_row;
  instr->dest_col = dst_col;
  instr->src_col = src_col;
}

/* Construct an instruction to do a TCAM invalidate. */
static inline void construct_instr_tcam_invalidate(
    bf_dev_id_t dev_id,
    pipe_tcam_invalidate_instr_t *instr,
    int mem_id,
    uint32_t mem_addr,
    bool write_tcam) {
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  if (!dev_info) {
    PIPE_MGR_DBGCHK(0);
    return;
  }
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->pipe_ring_addr_type = addr_type_instruction;
  instr->data_width = 3;
  instr->opcode = INSTR_OPCODE_SET_TCAM_WRITE_REG;
  instr->tcam_row = dev_info->dev_cfg.mem_id_to_row(mem_id, pipe_mem_type_tcam);
  instr->tcam_col = dev_info->dev_cfg.mem_id_to_col(mem_id, pipe_mem_type_tcam);
  instr->write_tcam = write_tcam;
  instr->mem_address = mem_addr;
}

/* Construct an instruction to do a TCAM write. */
static inline void construct_instr_tcam_write(
    bf_dev_id_t dev_id,
    pipe_set_tcam_write_reg_instr_t *instr,
    int mem_id,
    uint32_t mem_addr,
    bool write_tcam) {
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  if (!dev_info) {
    PIPE_MGR_DBGCHK(0);
    return;
  }
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->pipe_ring_addr_type = addr_type_instruction;
  instr->data_width = 3;
  instr->opcode = INSTR_OPCODE_SET_TCAM_WRITE_REG;
  instr->tcam_row = dev_info->dev_cfg.mem_id_to_row(mem_id, pipe_mem_type_tcam);
  instr->tcam_col = dev_info->dev_cfg.mem_id_to_col(mem_id, pipe_mem_type_tcam);
  instr->write_tcam = write_tcam;
  instr->mem_address = mem_addr;
}

/* Construct an instruction to do a Move-Reg Push. */
static inline void construct_instr_move_reg_push(
    bf_dev_id_t dev_id,
    pipe_push_table_move_adr_instr_t *instr,
    int logical_tbl,
    uint32_t src_addr) {
  (void)dev_id;
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->s_address = src_addr;
  instr->tbl_id = logical_tbl;
  instr->opcode = INSTR_OPCODE_PUSH_TABLE_MOVE_ADR;
  instr->pipe_ring_addr_type = addr_type_instruction;
}

/* Construct an instruction to do a Move-Reg Pop. */
static inline void construct_instr_move_reg_pop(
    bf_dev_id_t dev_id,
    pipe_pop_table_move_adr_instr_t *instr,
    int logical_tbl,
    bool stat_init,
    bool idle_max) {
  (void)dev_id;
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->tbl_id = logical_tbl;
  instr->idle_max_val = idle_max;
  instr->stat_init = stat_init;
  instr->opcode = INSTR_OPCODE_POP_TABLE_MOVE_ADR;
  instr->pipe_ring_addr_type = addr_type_instruction;
}

/* Construct an instruction to issue a barrier, lock or unlock. */
static inline void construct_instr_barrier_lock(
    bf_dev_id_t dev_id,
    pipe_barrier_lock_instr_t *instr,
    uint16_t lock_id,
    int logical_tbl,
    pipe_barrier_lock_type_e lock_type) {
  (void)dev_id;
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->lock_id = lock_id;
  instr->lock_type = lock_type;
  instr->tbl_id = logical_tbl;
  instr->opcode = INSTR_OPCODE_BARRIER_LOCK;
  instr->pipe_ring_addr_type = addr_type_instruction;
}

/* Construct an idle barrier instruction. */
static inline void construct_instr_barrier_idle(
    bf_dev_id_t dev_id,
    pipe_barrier_lock_instr_t *instr,
    uint16_t lock_id,
    int logical_tbl) {
  construct_instr_barrier_lock(
      dev_id, instr, lock_id, logical_tbl, IDLE_BARRIER);
}

/* Construct a stats barrier instruction. */
static inline void construct_instr_barrier_stats(
    bf_dev_id_t dev_id,
    pipe_barrier_lock_instr_t *instr,
    uint16_t lock_id,
    int logical_tbl) {
  construct_instr_barrier_lock(
      dev_id, instr, lock_id, logical_tbl, STAT_BARRIER);
}

/* Construct an idle lock instruction. */
static inline void construct_instr_lock_idle(bf_dev_id_t dev_id,
                                             pipe_barrier_lock_instr_t *instr,
                                             uint16_t lock_id,
                                             int logical_tbl) {
  construct_instr_barrier_lock(dev_id, instr, lock_id, logical_tbl, IDLE_LOCK);
}

/* Construct a stats lock instruction. */
static inline void construct_instr_lock_stats(bf_dev_id_t dev_id,
                                              pipe_barrier_lock_instr_t *instr,
                                              uint16_t lock_id,
                                              int logical_tbl) {
  construct_instr_barrier_lock(dev_id, instr, lock_id, logical_tbl, STAT_LOCK);
}

/* Construct a stats and idle lock instruction. */
static inline void construct_instr_lock_all(bf_dev_id_t dev_id,
                                            pipe_barrier_lock_instr_t *instr,
                                            uint16_t lock_id,
                                            int logical_tbl) {
  construct_instr_barrier_lock(dev_id, instr, lock_id, logical_tbl, ALL_LOCK);
}

/* Construct an idle unlock instruction. */
static inline void construct_instr_unlock_idle(bf_dev_id_t dev_id,
                                               pipe_barrier_lock_instr_t *instr,
                                               uint16_t lock_id,
                                               int logical_tbl) {
  construct_instr_barrier_lock(
      dev_id, instr, lock_id, logical_tbl, IDLE_UNLOCK);
}

/* Construct a stats unlock instruction. */
static inline void construct_instr_unlock_stats(
    bf_dev_id_t dev_id,
    pipe_barrier_lock_instr_t *instr,
    uint16_t lock_id,
    int logical_tbl) {
  construct_instr_barrier_lock(
      dev_id, instr, lock_id, logical_tbl, STAT_UNLOCK);
}

/* Construct a stats and idle unlock instruction. */
static inline void construct_instr_unlock_all(bf_dev_id_t dev_id,
                                              pipe_barrier_lock_instr_t *instr,
                                              uint16_t lock_id,
                                              int logical_tbl) {
  construct_instr_barrier_lock(dev_id, instr, lock_id, logical_tbl, ALL_UNLOCK);
}

/* Construct an instruction to adjust a stateful push/pop counter. */
static inline void construct_instr_salu_cntr(bf_dev_id_t dev_id,
                                             pipe_salu_cntr_instr_t *instr,
                                             bool push_t_pop_f,
                                             int cntr_index,
                                             int amount) {
  (void)dev_id;
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->pipe_ring_addr_type = addr_type_instruction;
  instr->data_width = 0;
  instr->opcode = INSTR_OPCODE_SALU_CNTR;
  instr->push_1_pop_0 = push_t_pop_f;
  instr->resvd = 0;
  instr->cntr = cntr_index;
  instr->inc_amount = amount - 1;
}

/* Construct an instruction to run a stateful ALU operation. */
static inline void construct_instr_run_salu(bf_dev_id_t dev_id,
                                            pipe_run_salu_instr_t *instr,
                                            int salu_instr_num,
                                            int logical_tbl,
                                            uint32_t addr) {
  (void)dev_id;
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->pipe_ring_addr_type = addr_type_instruction;
  instr->data_width = 1;
  instr->opcode = INSTR_OPCODE_RUN_SALU;
  instr->pad0 = 0;
  instr->logical_table = logical_tbl;
  instr->instr_index = salu_instr_num;
  instr->data.addr = addr;
  instr->data.pad1 = 0;
}

/* Construct an instruction to write a register; data is provided later. */
static inline void construct_instr_reg_write_no_data(
    bf_dev_id_t dev_id, pipe_instr_write_reg_i_only_t *instr, uint32_t addr) {
  (void)dev_id;
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  if (!dev_info) {
    LOG_ERROR("%s:%d Device info not found", __func__, __LINE__);
    return;
  }
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
    case BF_DEV_FAMILY_TOFINO2:
    case BF_DEV_FAMILY_TOFINO3:
      instr->tf1.pipe_ring_addr_type = addr_type_register;
      instr->tf1.data_width = 1;
      instr->tf1.reg_address = addr;
      break;



    default:
      PIPE_MGR_DBGCHK(0);
      return;
  }
}

/* Construct an instruction to write a register. */
static inline void construct_instr_reg_write(bf_dev_id_t dev_id,
                                             pipe_instr_write_reg_t *instr,
                                             uint32_t addr,
                                             uint32_t data) {
  (void)dev_id;
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  if (!dev_info) {
    LOG_ERROR("%s:%d Device info not found", __func__, __LINE__);
    return;
  }
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
    case BF_DEV_FAMILY_TOFINO2:
    case BF_DEV_FAMILY_TOFINO3:
      instr->head.tf1.pipe_ring_addr_type = addr_type_register;
      instr->head.tf1.data_width = 1;
      instr->head.tf1.reg_address = addr;
      break;



    default:
      PIPE_MGR_DBGCHK(0);
      return;
  }
  instr->data = data;
}

/* Construct an instruction for an MAU no-op. */
static inline void construct_instr_noop(bf_dev_id_t dev_id,
                                        pipe_noop_instr_t *instr) {
  (void)dev_id;
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->opcode = INSTR_OPCODE_NOOP;
  instr->pipe_ring_addr_type = addr_type_instruction;
}

/* Construct an instruction to set the pipe-id and stage-id for an instruction
 * list DR. */
static inline void construct_instr_dest_select(bf_dev_id_t dev_id,
                                               dest_select_t *instr,
                                               bf_dev_pipe_t pipe_id,
                                               int stage_id) {
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->opcode = INSTR_OPCODE_SELECT_DEST;
  instr->data_width = 1;
  instr->pipe_ring_addr_type = addr_type_instruction;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  pipe_physical_addr_t x;

  if (!dev_info) {
    PIPE_MGR_DBGCHK(0);
    return;
  }

  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      instr->pipe_element_36_33 = stage_id;
      instr->pipe_id_39_37 = pipe_id;
      instr->pipe_41_40 = (PIPE_TOP_LEVEL_PIPES_ADDRESS >> 44);
      break;
    case BF_DEV_FAMILY_TOFINO2:
      x.addr = 0;
      x.tof2.pipe_always_1 = 1;
      x.tof2.pipe_id = pipe_id;
      x.tof2.pipe_stage = stage_id;
      *(((uint32_t *)instr) + 1) = x.addr >> 32;
      break;
    case BF_DEV_FAMILY_TOFINO3:
      x.addr = 0;
      x.tof3.pipe_always_1 = 1;
      x.tof3.pipe_id = pipe_id;
      x.tof3.pipe_stage = stage_id;
      *(((uint32_t *)instr) + 1) = x.addr >> 32;
      break;










    default:
      PIPE_MGR_DBGCHK(0);
      break;
  }
}

/* Construct an instruction to set the stage-id for an instruction list DR. */
static inline void construct_instr_dest_select_stage(bf_dev_id_t dev_id,
                                                     dest_select_stage_t *instr,
                                                     uint8_t stage_id) {
  PIPE_MGR_MEMSET(instr, 0, sizeof *instr);
  instr->opcode = INSTR_OPCODE_SELECT_DEST_STAGE;
  instr->data_width = 1;
  instr->pipe_ring_addr_type = addr_type_instruction;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev_id);
  pipe_physical_addr_t x;

  if (!dev_info) {
    PIPE_MGR_DBGCHK(0);
    return;
  }

  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      instr->pipe_element_36_33 = stage_id & 0xF;
      instr->pipe_id_39_37 = 0;
      instr->pipe_41_40 = (PIPE_TOP_LEVEL_PIPES_ADDRESS >> 44);
      break;
    case BF_DEV_FAMILY_TOFINO2:
      x.addr = 0;
      x.tof2.pipe_always_1 = 1;
      x.tof2.pipe_id = 0;
      x.tof2.pipe_stage = stage_id;
      *(((uint32_t *)instr) + 1) = x.addr >> 32;
      break;
    case BF_DEV_FAMILY_TOFINO3:
      x.addr = 0;
      x.tof3.pipe_always_1 = 1;
      x.tof3.pipe_id = 0;
      x.tof3.pipe_stage = stage_id;
      *(((uint32_t *)instr) + 1) = x.addr >> 32;
      break;










    default:
      PIPE_MGR_DBGCHK(0);
      break;
  }
}

#endif
