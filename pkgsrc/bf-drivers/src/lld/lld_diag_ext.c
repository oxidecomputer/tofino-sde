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
#include <string.h>
#include <inttypes.h>
#ifdef __sun
#include <endian.h>
#endif

#include <lld/tof2_reg_drv_defs.h>
#include <lld/tof3_reg_drv_defs.h>
#include <dvm/bf_drv_intf.h>
#include <port_mgr/port_mgr_intf.h>
#include <target-sys/bf_sal/bf_sys_intf.h>
#include <lld/bf_dma_if.h>
#include <lld/lld_dr.h>
#include <lld/lld_dr_if.h>
#include <lld/lld_subdev_dr_if.h>
#include <lld/lld_reg_if.h>
#include <lld/lld_dr_descriptors.h>
#include <lld/lld_inst_list_fmt.h>
#include <lld/lld_err.h>
#include <lld/lld_sku.h>
#include <lld/tofino_defs.h>
#include <tofino_regs/tofino.h>
#include <lld/tof2_reg_drv_defs.h>
#include <lld/tof3_reg_drv_defs.h>
#include <tof2_regs/tof2_reg_drv.h>
#include <tof3_regs/tof3_reg_drv.h>
#include "lld_diag.h"
#include "lld.h"
#include "lld_map.h"
#include "lld_dev.h"
#include "lld_diag_ext.h"
#include "lld_memory_mapping.h"
#include "lldlib_log.h"
#include "lld_log.h"

lld_diag_cfg_t lld_diag_cfg = {0};
lld_diag_results_t lld_diag_results = {{0}, {0}, {0}};
lld_int_test_cached_data_t
    lld_int_test_cached_data[LLD_INT_TEST_CACHED_DATA_MAX];
extern void lld_construct_instr_dest_select(bf_dev_id_t dev_id,
                                            dest_select_t *instr,
                                            bf_dev_pipe_t pipe_id,
                                            int stage_id);

extern FILE *reg_get_outstream();

/* PEX - CLC (0x5A00000000 - 0x5600000000)*/
#define TM_PEX_CLC_OFFSET 0x400000000

void lld_diag_cfg_set(uint32_t pipe_mask,
                      uint32_t stage_mask,
                      int quick,
                      int tm,
                      bf_diag_test_pattern_t pattern,
                      uint64_t data0,
                      uint64_t data1) {
  lld_diag_cfg.pipe_mask = pipe_mask;
  lld_diag_cfg.stage_mask = stage_mask;
  lld_diag_cfg.quick = quick;
  lld_diag_cfg.do_tm = tm;
  lld_diag_cfg.pattern = pattern;

  /* For prbs calculate the pattern now itself as it is same for all entries */
  if (pattern == BF_DIAG_TEST_PATTERN_PRBS) {
    lld_diag_test_pattern_prbs_get(&(lld_diag_cfg.pattern_data0),
                                   &(lld_diag_cfg.pattern_data1));
  } else if (pattern == BF_DIAG_TEST_PATTERN_USER_DEFINED) {
    lld_diag_cfg.pattern_data0 = data0;
    lld_diag_cfg.pattern_data1 = data1;
  } else {
    lld_diag_cfg.pattern_data0 = 0;
    lld_diag_cfg.pattern_data1 = 0;
  }
  return;
}

void lld_diag_cfg_reset() { memset(&lld_diag_cfg, 0, sizeof(lld_diag_cfg)); }

/*
 * Generic DMA buffer allocation for LLD internal use (ONLY).
 * uses the BF_DMA_DIAG_ERR_NOTIFY pool for all buffers.
 */
int lld_diag_dma_alloc(bf_dev_id_t dev_id,
                       bf_subdev_id_t subdev_id,
                       uint32_t sz,
                       void **vaddr,
                       uint64_t *paddr) {
  lld_dev_t *dev_p = lld_map_subdev_id_to_dev_p(dev_id, subdev_id);

  if (dev_p == NULL) {
    fprintf(reg_get_outstream(), "Warning: dev_id %d not found!\n", dev_id);
    return -1;
  }

  struct bf_dma_info_s *bf_dma_info_p = &dev_p->dma_info;
  // allocate dma buffer for LLD internal use
  if (sz > bf_dma_info_p->dma_buff_info[BF_DMA_DIAG_ERR_NOTIFY].dma_buf_size) {
    fprintf(
        reg_get_outstream(),
        "Warning: requested %d bytes, buffers are only %d bytes. Increase pool "
        "sz\n",
        sz,
        bf_dma_info_p->dma_buff_info[BF_DMA_DIAG_ERR_NOTIFY].dma_buf_size);
    // return -1;
  }
  if (bf_sys_dma_alloc(
          bf_dma_info_p->dma_buff_info[BF_DMA_DIAG_ERR_NOTIFY]
              .dma_buf_pool_handle,
          bf_dma_info_p->dma_buff_info[BF_DMA_DIAG_ERR_NOTIFY].dma_buf_size,
          vaddr,
          paddr)) {
    fprintf(reg_get_outstream(),
            "Error: dma buff alloc failed for dev_id %d at %s:%d\n",
            dev_id,
            __func__,
            __LINE__);
    return -1;
  }
  return 0;
}

/*
 * Generic DMA buffer free for LLD internal use (ONLY).
 * uses the BF_DMA_DIAG_ERR_NOTIFY pool for all buffers.
 */
int lld_diag_dma_free(bf_dev_id_t dev_id,
                      bf_subdev_id_t subdev_id,
                      uint8_t *vaddr) {
  lld_dev_t *dev_p = lld_map_subdev_id_to_dev_p(dev_id, subdev_id);

  if (dev_p == NULL) {
    fprintf(reg_get_outstream(), "Warning: dev_id %d not found!\n", dev_id);
    return -1;
  }

  struct bf_dma_info_s *bf_dma_info_p = &dev_p->dma_info;
  bf_sys_dma_free(
      bf_dma_info_p->dma_buff_info[BF_DMA_DIAG_ERR_NOTIFY].dma_buf_pool_handle,
      vaddr);
  return 0;
}

/*
 * DMA buffer allocation with type for LLD internal use (ONLY).
 */
int lld_diag_dma_alloc_with_type(bf_dev_id_t dev_id,
                                 bf_subdev_id_t subdev_id,
                                 uint32_t sz,
                                 void **vaddr,
                                 uint64_t *paddr,
                                 bf_dma_type_t dma_type) {
  lld_dev_t *dev_p = lld_map_subdev_id_to_dev_p(dev_id, subdev_id);

  if (dev_p == NULL) {
    fprintf(reg_get_outstream(), "Warning: dev_id %d not found!\n", dev_id);
    return -1;
  }

  struct bf_dma_info_s *bf_dma_info_p = &dev_p->dma_info;
  // allocate dma buffer for LLD internal use
  if (sz > bf_dma_info_p->dma_buff_info[dma_type].dma_buf_size) {
    fprintf(
        reg_get_outstream(),
        "Warning: requested %d bytes, buffers are only %d bytes. Increase pool "
        "sz for dma-type %d\n",
        sz,
        bf_dma_info_p->dma_buff_info[dma_type].dma_buf_size,
        dma_type);
    return -1;
  }

  if (bf_sys_dma_alloc(
          bf_dma_info_p->dma_buff_info[dma_type].dma_buf_pool_handle,
          bf_dma_info_p->dma_buff_info[dma_type].dma_buf_size,
          vaddr,
          paddr)) {
#if 0
    fprintf(reg_get_outstream(),"Error: dma buff alloc with type %d failed for dev_id %d\n",
           dma_type,
           dev_id);
#endif
    return -2;
  }
  return 0;
}

/*
 * DMA buffer free with type for LLD internal use (ONLY).
 */
int lld_diag_dma_free_with_type(bf_dev_id_t dev_id,
                                bf_subdev_id_t subdev_id,
                                uint8_t *vaddr,
                                bf_dma_type_t dma_type) {
  lld_dev_t *dev_p = lld_map_subdev_id_to_dev_p(dev_id, subdev_id);

  if (dev_p == NULL) {
    fprintf(reg_get_outstream(), "Warning: dev_id %d not found!\n", dev_id);
    return -1;
  }

  struct bf_dma_info_s *bf_dma_info_p = &dev_p->dma_info;
  bf_sys_dma_free(bf_dma_info_p->dma_buff_info[dma_type].dma_buf_pool_handle,
                  vaddr);
  return 0;
}

/* Diag memory test */
void lld_memtest_results_clear() {
  memset(
      &(lld_diag_results.mem_results), 0, sizeof(lld_diag_results.mem_results));
}

void lld_memtest_result_set(bool success) {
  lld_diag_results.mem_results.overall_success = success;
}

bool lld_memtest_result_get() {
  if (lld_diag_results.mem_results.num_dma_msgs_sent ==
      lld_diag_results.mem_results.num_dma_cmplts_rcvd) {
    lld_memtest_result_set(true);
  } else {
    lld_memtest_result_set(false);
  }
  return lld_diag_results.mem_results.overall_success;
}

void *lld_memtest_results_get(bf_dev_id_t dev_id) {
  (void)dev_id;
  /* Force recalculation of result */
  lld_memtest_result_get();
  return (void *)&(lld_diag_results.mem_results);
}

void lld_memtest_read_error_set(uint64_t addr) {
  if (!lld_diag_results.mem_results.ind_read_error) {
    lld_diag_results.mem_results.ind_read_error_addr = addr;
    lld_diag_results.mem_results.ind_read_error = true;
  }
}
void lld_memtest_write_error_set(uint64_t addr) {
  if (!lld_diag_results.mem_results.ind_write_error) {
    lld_diag_results.mem_results.ind_write_error_addr = addr;
    lld_diag_results.mem_results.ind_write_error = true;
  }
}
void lld_memtest_write_list_error_set() {
  lld_diag_results.mem_results.write_list_error = true;
}
void lld_memtest_write_block_error_set() {
  lld_diag_results.mem_results.write_block_error = true;
}
void lld_memtest_data_error_set(uint64_t addr,
                                uint64_t exp_0,
                                uint64_t exp_1,
                                uint64_t data_0,
                                uint64_t data_1,
                                uint64_t mask_0,
                                uint64_t mask_1) {
  uint32_t index = lld_diag_results.mem_results.num_data_errors;
  if (index >= BF_DIAG_MEM_MAX_DATA_ERR) {
    return;
  }
  lld_diag_results.mem_results.data_error[index].addr = addr;
  lld_diag_results.mem_results.data_error[index].exp_0 = exp_0;
  lld_diag_results.mem_results.data_error[index].exp_1 = exp_1;

  lld_diag_results.mem_results.data_error[index].data_0 = data_0;
  lld_diag_results.mem_results.data_error[index].data_1 = data_1;

  lld_diag_results.mem_results.data_error[index].mask_0 = mask_0;
  lld_diag_results.mem_results.data_error[index].mask_1 = mask_1;

  lld_diag_results.mem_results.num_data_errors += 1;
}
void lld_memtest_dma_msgs_sent_inc() {
  lld_diag_results.mem_results.num_dma_msgs_sent++;
}
void lld_memtest_dma_cmplts_rcvd_inc() {
  lld_diag_results.mem_results.num_dma_cmplts_rcvd++;
}

/* Diag register test */
void lld_regtest_results_clear() {
  memset(
      &(lld_diag_results.reg_results), 0, sizeof(lld_diag_results.reg_results));
}
void lld_regtest_result_set(bool success) {
  lld_diag_results.reg_results.overall_success = success;
}

bool lld_regtest_result_get() {
  if (lld_diag_results.reg_results.num_dma_msgs_sent >
      lld_diag_results.reg_results.num_dma_cmplts_rcvd) {
    lld_regtest_result_set(false);
  }
  return lld_diag_results.reg_results.overall_success;
}

void *lld_regtest_results_get(bf_dev_id_t dev_id) {
  (void)dev_id;
  /* Force recalculation of result */
  lld_regtest_result_get();
  return (void *)&(lld_diag_results.reg_results);
}

void lld_regtest_read_error_set(uint32_t addr) {
  if (!lld_diag_results.reg_results.reg_read_error) {
    lld_diag_results.reg_results.reg_read_error_addr = addr;
    lld_diag_results.reg_results.reg_read_error = true;
  }
}
void lld_regtest_write_error_set(uint32_t addr) {
  if (!lld_diag_results.reg_results.reg_write_error) {
    lld_diag_results.reg_results.reg_write_error_addr = addr;
    lld_diag_results.reg_results.reg_write_error = true;
  }
}
void lld_regtest_ilist_error_set() {
  lld_diag_results.reg_results.ilist_error = true;
}

void lld_regtest_data_error_set(uint32_t addr,
                                uint32_t exp_data,
                                uint32_t read_data) {
  uint32_t index = lld_diag_results.reg_results.num_data_errors;
  if (index >= BF_DIAG_REG_MAX_DATA_ERR) {
    return;
  }
  lld_diag_results.reg_results.data_error[index].addr = addr;
  lld_diag_results.reg_results.data_error[index].exp_data = exp_data;
  lld_diag_results.reg_results.data_error[index].read_data = read_data;

  lld_diag_results.reg_results.num_data_errors += 1;
}
void lld_regtest_dma_msgs_sent_inc() {
  lld_diag_results.reg_results.num_dma_msgs_sent++;
}
void lld_regtest_dma_cmplts_rcvd_inc() {
  lld_diag_results.reg_results.num_dma_cmplts_rcvd++;
}

bf_diag_test_pattern_t lld_diag_test_pattern_get() {
  return lld_diag_cfg.pattern;
}

bf_diag_test_pattern_t lld_diag_test_pattern_data0_get() {
  return lld_diag_cfg.pattern_data0;
}

bf_diag_test_pattern_t lld_diag_test_pattern_data1_get() {
  return lld_diag_cfg.pattern_data1;
}

void lld_diag_test_pattern_prbs_get(uint64_t *data0, uint64_t *data1) {
  uint64_t temp0 = 0, temp1 = 0;
  uint8_t start = 0x02;
  uint8_t a = start;
  int i, newbit = 0;
  for (i = 1; i <= 16; i++) {
    newbit = (((a >> 6) ^ (a >> 5)) & 1);
    a = ((a << 1) | newbit) & 0x7f;
    if (i <= 8) {
      temp0 |= ((uint64_t)a << (8 * (i - 1)));
    } else {
      temp1 |= ((uint64_t)a << (8 * (i - 1 - 8)));
    }
  }
  *data0 = temp0;
  *data1 = temp1;
}

void lld_diag_memtest_calc_exp_result(bf_diag_test_pattern_t pattern,
                                      uint64_t index,
                                      uint64_t num_entries,
                                      uint64_t pattern_data0,
                                      uint64_t pattern_data1,
                                      uint64_t mask64_0,
                                      uint64_t mask64_1,
                                      uint64_t *exp64_0,
                                      uint64_t *exp64_1) {
  if (pattern == BF_DIAG_TEST_PATTERN_ZEROES) {
    *exp64_0 = (uint64_t)0;
    *exp64_1 = (uint64_t)0;
  } else if (pattern == BF_DIAG_TEST_PATTERN_ONES) {
    *exp64_0 = (~(uint64_t)0) & mask64_0;
    *exp64_1 = (~(uint64_t)0) & mask64_1;
  } else if (pattern == BF_DIAG_TEST_PATTERN_CHECKERBOARD) {
    *exp64_0 = LLD_DIAG_CHECKERBOARD_PATTERN & mask64_0;
    *exp64_1 = LLD_DIAG_CHECKERBOARD_PATTERN & mask64_1;
  } else if (pattern == BF_DIAG_TEST_PATTERN_INV_CHECKERBOARD) {
    *exp64_0 = LLD_DIAG_INV_CHECKERBOARD_PATTERN & mask64_0;
    *exp64_1 = LLD_DIAG_INV_CHECKERBOARD_PATTERN & mask64_1;
  } else if (pattern == BF_DIAG_TEST_PATTERN_PRBS) {
    *exp64_0 = pattern_data0 & mask64_0;
    *exp64_1 = pattern_data1 & mask64_1;
  } else if (pattern == BF_DIAG_TEST_PATTERN_USER_DEFINED) {
    *exp64_0 = pattern_data0 & mask64_0;
    *exp64_1 = pattern_data1 & mask64_1;
  } else {
    *exp64_0 = ((uint64_t)(index + num_entries) & mask64_0);
    *exp64_1 = ((uint64_t)(index)&mask64_1);
  }
}

/* Write and read register value */
bf_status_t lld_diag_write_read_reg_test(bf_dev_id_t dev_id,
                                         bf_subdev_id_t subdev_id,
                                         uint32_t addr,
                                         uint32_t data,
                                         int phy_pipe,
                                         int stage,
                                         bool *overall_passed) {
  int rc = 0;
  uint32_t read_data = 0;

  rc = lld_subdev_write_register(dev_id, subdev_id, addr, data);
  if (rc != 0) {
    LOG_ERROR(
        "Error in write_register: %d <%xh> : addr=%08x (pipe %d, stage %d) "
        "\n",
        rc,
        rc,
        addr,
        phy_pipe,
        stage);
    lld_regtest_write_error_set(addr);
    return rc;
  }
  rc = lld_subdev_read_register(dev_id, subdev_id, addr, &read_data);
  if (rc != 0) {
    LOG_ERROR(
        "Error in read_register: %d <%xh> : addr=%08x (pipe %d, stage %d) "
        "\n",
        rc,
        rc,
        addr,
        phy_pipe,
        stage);
    lld_regtest_read_error_set(addr);
    return rc;
  }
  if (data != read_data) {
    *overall_passed = false;
    lld_regtest_data_error_set(addr, data, read_data);
    LOG_ERROR(
        "Data Error: addr=%08x, pipe %d, stage %d : got=%08x : exp=%08x\n",
        addr,
        phy_pipe,
        stage,
        read_data,
        data);
  }

  return rc;
}

bf_status_t lld_diag_reg_test_mau(bf_dev_id_t dev_id, bool quick, bool dma) {
  uint32_t addr = 0, data = 0;
  bf_subdev_id_t subdev_id = 0;
  uint32_t stage = 0;
  uint32_t num_pipes = 0, num_stages = 0;
  bf_dev_pipe_t pipe = 0;
  bf_dev_pipe_t phy_pipe = 0;
  bool overall_passed = false;
  int rc = 0;

  (void)quick;
  lld_regtest_results_clear();
  lld_regtest_result_set(true);
  lld_sku_get_num_active_pipes(dev_id, &num_pipes);

  overall_passed = true;
  fprintf(reg_get_outstream(), "Num of pipes %d \n", num_pipes);

  for (pipe = 0; pipe < num_pipes; pipe++) {
    // make sure pipe is configured for testing
    if (!(lld_diag_cfg.pipe_mask & (1 << pipe))) continue;
    lld_sku_map_pipe_id_to_phy_pipe_id(dev_id, pipe, &phy_pipe);
    phy_pipe = phy_pipe % BF_SUBDEV_PIPE_COUNT;
    subdev_id = pipe / BF_SUBDEV_PIPE_COUNT;
    lld_sku_get_num_active_mau_stages(dev_id, &num_stages, phy_pipe);
    fprintf(reg_get_outstream(),
            "Running test on subdev %d, logical pipe %d, Num of stages %d \n",
            subdev_id,
            pipe,
            num_stages);
    for (stage = 0; stage < num_stages; stage++) {
      // make sure stage is configured for testing
      if (!(lld_diag_cfg.stage_mask & (1 << stage))) continue;
      data = LLD_DIAG_MAGIC_DATA + phy_pipe + stage;

      if (lld_dev_is_tofino(dev_id)) {
        addr = offsetof(Tofino, pipes[phy_pipe].mau[stage].dp.mau_scratch);
      } else if (lld_dev_is_tof2(dev_id)) {
        addr = offsetof(tof2_reg, pipes[phy_pipe].mau[stage].dp.mau_scratch);
      } else if (lld_dev_is_tof3(dev_id)) {
        addr = offsetof(tof3_reg, pipes[phy_pipe].mau[stage].dp.mau_scratch);
      }
      if (dma) {
        rc = lld_ilist_add_register_write(dev_id, subdev_id, addr, data);
      } else {
        rc = lld_diag_write_read_reg_test(
            dev_id, subdev_id, addr, data, phy_pipe, stage, &overall_passed);
      }
      if (rc != 0) {
        lld_regtest_result_set(false);
        return rc;
      }
    }
  }
  if (!overall_passed) {
    lld_regtest_result_set(false);
    return BF_OBJECT_NOT_FOUND;
  }
  if (dma) {
    /* Wait for DMA callbacks to finish */
    fprintf(reg_get_outstream(), "Waiting for dma to complete \n");
    bf_sys_usleep(LLD_DIAG_SEC(3));
  }

  return BF_SUCCESS;
}

bf_status_t lld_diag_reg_test_parde(bf_dev_id_t dev_id, bool quick, bool dma) {
  uint32_t addr[3], data = 0;
  bf_dev_pipe_t pipe = 0;
  bf_subdev_id_t subdev_id = 0;
  uint32_t stage[3];
  uint32_t num_pipes = 0;
  bf_dev_pipe_t phy_pipe = 0;
  bool overall_passed = false;
  int rc = 0;
  bool is_tofino = lld_dev_is_tofino(dev_id);
  bool is_tofino2 = lld_dev_is_tof2(dev_id);
  bool is_tofino3 = lld_dev_is_tof3(dev_id);

  (void)quick;
  lld_regtest_results_clear();
  lld_regtest_result_set(true);
  lld_sku_get_num_active_pipes(dev_id, &num_pipes);
  fprintf(reg_get_outstream(), "Num of pipes %d \n", num_pipes);

  overall_passed = true;
  for (pipe = 0; pipe < num_pipes; pipe++) {
    // make sure pipe is configured for testing
    if (!(lld_diag_cfg.pipe_mask & (1 << pipe))) continue;
    lld_sku_map_pipe_id_to_phy_pipe_id(dev_id, pipe, &phy_pipe);
    phy_pipe = phy_pipe % BF_SUBDEV_PIPE_COUNT;
    subdev_id = pipe / BF_SUBDEV_PIPE_COUNT;

    fprintf(reg_get_outstream(),
            "Running test on subdev %d, physical Pipe %d\n",
            subdev_id,
            phy_pipe);

    if (is_tofino) {
      /* ibp */
      addr[0] = offsetof(
          Tofino, pipes[phy_pipe].pmarb.ibp18_reg.ibp_reg[0].prsr_reg.scratch);
      stage[0] = LLD_DIAG_TF1_STAGE_GET(addr[0]);
      /* ebp */
      addr[1] = offsetof(Tofino,
                         pipes[phy_pipe]
                             .pmarb.ebp18_reg.ebp_reg[0]
                             .epb_prsr_port_regs.scratch);
      stage[1] = LLD_DIAG_TF1_STAGE_GET(addr[1]);
      /* deparser */
      addr[2] = offsetof(Tofino, pipes[phy_pipe].deparser.inp.iim.scratch);
      stage[2] = LLD_DIAG_TF1_STAGE_GET(addr[2]);
    } else if (is_tofino2) {
      /* ibp */
      addr[0] = offsetof(
          tof2_reg,
          pipes[phy_pipe].pardereg.pgstnreg.ipbprsr4reg[0].prsr[0].scratch);
      stage[0] = LLD_DIAG_TF2_STAGE_GET(addr[0]);
      /* ebp */
      addr[1] = offsetof(
          tof2_reg,
          pipes[phy_pipe].pardereg.pgstnreg.epbprsr4reg[0].epbreg.scratch);
      stage[1] = LLD_DIAG_TF2_STAGE_GET(addr[1]);
      /* deparser */
      addr[2] = offsetof(
          tof2_reg, pipes[phy_pipe].pardereg.dprsrreg.dprsrreg.inp.icr.scratch);
      stage[2] = LLD_DIAG_TF2_STAGE_GET(addr[2]);
    } else if (is_tofino3) {
      /* ibp */
      addr[0] = offsetof(
          tof3_reg,
          pipes[phy_pipe].pardereg.pgstnreg.ipbprsr4reg[0].prsr[0].scratch);
      stage[0] = LLD_DIAG_TF3_STAGE_GET(addr[0]);
      /* ebp */
      addr[1] = offsetof(
          tof3_reg,
          pipes[phy_pipe].pardereg.pgstnreg.epbprsr4reg[0].epbreg.scratch);
      stage[1] = LLD_DIAG_TF3_STAGE_GET(addr[1]);
      /* deparser */
      addr[2] = offsetof(
          tof3_reg, pipes[phy_pipe].pardereg.dprsrreg.dprsrreg.inp.icr.scratch);
      stage[2] = LLD_DIAG_TF3_STAGE_GET(addr[2]);
    } else {
      return BF_INVALID_ARG;
    }

    /* ibp */
    data = LLD_DIAG_MAGIC_DATA + phy_pipe + stage[0];
    if (dma) {
      rc = lld_ilist_add_register_write(dev_id, subdev_id, addr[0], data);
    } else {
      rc = lld_diag_write_read_reg_test(dev_id,
                                        subdev_id,
                                        addr[0],
                                        data,
                                        phy_pipe,
                                        stage[0],
                                        &overall_passed);
    }
    if (rc != 0) {
      lld_regtest_result_set(false);
      return rc;
    }
    /* ebp */
    data = LLD_DIAG_MAGIC_DATA + phy_pipe + stage[1];
    if (dma) {
      rc = lld_ilist_add_register_write(dev_id, subdev_id, addr[1], data);
    } else {
      rc = lld_diag_write_read_reg_test(
          dev_id, subdev_id, addr[1], data, phy_pipe, -1, &overall_passed);
    }
    if (rc != 0) {
      lld_regtest_result_set(false);
      return rc;
    }

    /* deparser */
    data = LLD_DIAG_MAGIC_DATA + phy_pipe + stage[2];
    if (dma) {
      rc = lld_ilist_add_register_write(dev_id, subdev_id, addr[2], data);
    } else {
      rc = lld_diag_write_read_reg_test(dev_id,
                                        subdev_id,
                                        addr[2],
                                        data,
                                        phy_pipe,
                                        stage[2],
                                        &overall_passed);
    }
    if (rc != 0) {
      lld_regtest_result_set(false);
      return rc;
    }
  }
  if (!overall_passed) {
    lld_regtest_result_set(false);
    return BF_OBJECT_NOT_FOUND;
  }
  if (dma) {
    /* Wait for DMA callbacks to finish */
    fprintf(reg_get_outstream(), "Waiting for dma to complete \n");
    bf_sys_usleep(LLD_DIAG_SEC(3));
  }

  return BF_SUCCESS;
}

bf_status_t lld_diag_reg_test_tm(bf_dev_id_t dev_id, bool quick, bool dma) {
  uint32_t addr[2] = {0}, data = 0;
  bool overall_passed = false;
  bf_subdev_id_t subdev_id = 0;
  uint32_t phy_pipe = 0, stage = 0;
  int rc = 0;
  bool is_tofino = lld_dev_is_tofino(dev_id);
  bool is_tofino2 = lld_dev_is_tof2(dev_id);
  bool is_tofino3 = lld_dev_is_tof3(dev_id);

  (void)quick;
  lld_regtest_results_clear();
  lld_regtest_result_set(true);
  overall_passed = true;

  if (is_tofino) {
    addr[0] = offsetof(Tofino, device_select.tm_top.tm_caa.scratch);
    addr[1] =
        offsetof(Tofino, device_select.tm_top.tm_pre_top.pre_common.scratch);
    phy_pipe = LLD_DIAG_TF1_PHY_PIPE_GET(addr[1]);
    stage = LLD_DIAG_TF1_STAGE_GET(addr[1]);
  } else if (is_tofino2) {
    addr[0] = offsetof(tof2_reg, device_select.tm_top.tm_caa_top.scratch[0]);
    addr[1] =
        offsetof(tof2_reg, device_select.tm_top.tm_pre_top.pre_common.scratch);
    phy_pipe = LLD_DIAG_TF2_PHY_PIPE_GET(addr[1]);
    stage = LLD_DIAG_TF2_STAGE_GET(addr[1]);
  } else if (is_tofino3) {
    addr[0] = offsetof(tof3_reg, device_select.tm_top.tm_caa_top.scratch[0]);
    addr[1] =
        offsetof(tof3_reg, device_select.tm_top.tm_pre_top.pre_common.scratch);
    phy_pipe = LLD_DIAG_TF3_PHY_PIPE_GET(addr[1]);
    stage = LLD_DIAG_TF3_STAGE_GET(addr[1]);
  } else {
    return BF_INVALID_ARG;
  }

  uint32_t num_subdev = 0;
  lld_sku_get_num_subdev(dev_id, &num_subdev, NULL);
  for (subdev_id = 0; subdev_id < (int)num_subdev; subdev_id++) {
    if (((lld_diag_cfg.pipe_mask >> (BF_SUBDEV_PIPE_COUNT * subdev_id)) &
         0xf) == 0) {
      continue;
    }

    fprintf(reg_get_outstream(), "Running test on subdev %d\n", subdev_id);
    /* Write TM regs */
    rc = lld_diag_write_read_reg_test(
        dev_id, subdev_id, addr[0], data, -1, -1, &overall_passed);
    if (rc != 0) {
      lld_regtest_result_set(false);
      return rc;
    }

    /* The pipe and stage bit values are extracted here to create the data field
     */
    data = LLD_DIAG_MAGIC_DATA + phy_pipe + stage;

    if (dma) {
      rc = lld_wlist_add_register_write(dev_id, subdev_id, addr[1], data);
    } else {
      rc = lld_diag_write_read_reg_test(
          dev_id, subdev_id, addr[1], data, -1, -1, &overall_passed);
    }
    if (rc != 0) {
      lld_regtest_result_set(false);
      return rc;
    }
  }

  if (!overall_passed) {
    lld_regtest_result_set(false);
    return BF_OBJECT_NOT_FOUND;
  }
  if (dma) {
    /* Wait for DMA callbacks to finish */
    fprintf(reg_get_outstream(), "Waiting for dma to complete \n");
    bf_sys_usleep(LLD_DIAG_SEC(3));
  }

  return BF_SUCCESS;
}

/* Diag Interrupt test */
void lld_inttest_results_clear() {
  memset(&(lld_diag_results.interrupt_results),
         0,
         sizeof(lld_diag_results.interrupt_results));
  memset(&lld_int_test_cached_data[0],
         0,
         sizeof(lld_int_test_cached_data_t) * LLD_INT_TEST_CACHED_DATA_MAX);
}
bool lld_inttest_result_get(bf_dev_id_t dev_id) {
  (void)dev_id;
  /* Update result */
  if (lld_diag_results.interrupt_results.num_int_events_exp >
      lld_diag_results.interrupt_results.num_int_events_rcvd) {
    lld_inttest_result_set(false);
  }
  return lld_diag_results.interrupt_results.overall_success;
}

void *lld_inttest_results_get(bf_dev_id_t dev_id) {
  (void)dev_id;
  lld_inttest_result_get(dev_id);
  return (void *)&(lld_diag_results.interrupt_results);
}
void lld_inttest_result_set(bool success) {
  lld_diag_results.interrupt_results.overall_success = success;
}

void lld_inttest_error_set(char *reg_name,
                           uint32_t addr,
                           uint32_t exp_status,
                           uint32_t rcvd_status) {
  uint32_t index = lld_diag_results.interrupt_results.num_int_errors;
  if (index >= BF_DIAG_INTERRUPT_ERR_MAX) {
    return;
  }
  lld_diag_results.interrupt_results.int_error[index].addr = addr;
  lld_diag_results.interrupt_results.int_error[index].exp_status = exp_status;
  lld_diag_results.interrupt_results.int_error[index].rcvd_status = rcvd_status;
  strncpy(lld_diag_results.interrupt_results.int_error[index].reg_name,
          reg_name,
          LLD_INT_RES_REG_NAME_LEN - 1);
  lld_diag_results.interrupt_results.int_error[index]
      .reg_name[LLD_INT_RES_REG_NAME_LEN - 1] = '\0';

  lld_diag_results.interrupt_results.num_int_errors += 1;
}

void lld_inttest_events_exp_inc() {
  lld_diag_results.interrupt_results.num_int_events_exp += 1;
}

void lld_inttest_events_rcvd_inc() {
  lld_diag_results.interrupt_results.num_int_events_rcvd += 1;
}

void lld_reg_ilist_completion_callback_fn(bf_dev_id_t dev_id,
                                          bf_subdev_id_t subdev_id,
                                          bf_dma_dr_id_t dr,
                                          uint64_t ts_sz,
                                          uint32_t attr,
                                          uint32_t status,
                                          uint32_t type,
                                          uint64_t msg_id,
                                          int s,
                                          int e) {
  uint32_t stage_id = 0;
  bf_dev_pipe_t phy_pipe = 0;
  uint32_t reg_addr = 0, exp_data = 0, read_data = 0;
  int rc = 0;
  void *temp = NULL;
  lld_reg_dma_cmplt_info_t *reg_dma_info = NULL;

  (void)s;
  (void)e;
  (void)dr;
  (void)attr;

  if (status != 0) {
    fprintf(reg_get_outstream(),
            "ERROR: Error status %d in regtest completion callback, dev %d, "
            "subdev %d dr %d, "
            "type %d\n",
            status,
            dev_id,
            subdev_id,
            dr,
            type);
    return;
  }

  /* Inc the dma completes received */
  lld_regtest_dma_cmplts_rcvd_inc();
  temp = lld_u64_to_void_ptr(msg_id);
  reg_dma_info = (lld_reg_dma_cmplt_info_t *)temp;
  if (!reg_dma_info) {
    fprintf(
        reg_get_outstream(),
        "ERROR: Ilist info is NULL in reg completion callback, bailing out \n");
    return;
  }
  reg_addr = reg_dma_info->reg_addr;

  rc = lld_diag_dma_free_with_type(
      dev_id, subdev_id, reg_dma_info->vaddr, reg_dma_info->type);
  if (rc != 0) {
    LOG_ERROR("Unable to unmap DMA buffer %" PRIx64 " at %s:%d",
              reg_dma_info->phy_addr,
              __func__,
              __LINE__);
  }
  bf_sys_free(reg_dma_info);
  reg_dma_info = NULL;

  if (lld_dev_is_tofino(dev_id)) {
    phy_pipe = LLD_DIAG_TF1_PHY_PIPE_GET(reg_addr);
    stage_id = LLD_DIAG_TF1_STAGE_GET(reg_addr);
  } else if (lld_dev_is_tof2(dev_id)) {
    phy_pipe = LLD_DIAG_TF2_PHY_PIPE_GET(reg_addr);
    stage_id = LLD_DIAG_TF2_STAGE_GET(reg_addr);
  } else if (lld_dev_is_tof3(dev_id)) {
    phy_pipe = LLD_DIAG_TF3_PHY_PIPE_GET(reg_addr);
    stage_id = LLD_DIAG_TF3_STAGE_GET(reg_addr);
  }

  fprintf(reg_get_outstream(),
          "register diag ilist cmpl: dev=%d subdev=%d pipe=%d stage=%d: dr=%d "
          ": ts/sz=%" PRIx64
          " : attr=%x : sts=%d : "
          "typ=%d : "
          "msg-id=%" PRIx64 " reg-addr 0x%x\n",
          dev_id,
          subdev_id,
          phy_pipe,
          stage_id,
          dr,
          ts_sz,
          attr,
          status,
          type,
          msg_id,
          reg_addr);

  exp_data = LLD_DIAG_MAGIC_DATA + phy_pipe + stage_id;
  rc = lld_subdev_read_register(dev_id, subdev_id, reg_addr, &read_data);
  if (rc != 0) {
    LOG_ERROR("reg diag ilist cmpl: Error in read register: %x", reg_addr);
    lld_regtest_read_error_set(reg_addr);
    lld_regtest_result_set(false);
    return;
  }
  if (exp_data != read_data) {
    LOG_ERROR(
        "reg diag ilist cmpl: Data does not match, Addr: 0x%x, subdev %d, pipe "
        "%d, stage "
        "%d, exp_data %x, Read data %x ",
        reg_addr,
        subdev_id,
        phy_pipe,
        stage_id,
        exp_data,
        read_data);
    lld_regtest_data_error_set(reg_addr, exp_data, read_data);
    lld_regtest_result_set(false);
    return;
  }
}

/* Write register using ilist */
bf_status_t lld_ilist_add_register_write(bf_dev_id_t dev_id,
                                         bf_subdev_id_t subdev_id,
                                         uint32_t reg_addr,
                                         uint32_t reg_data) {
  // construct_ilist
  uint32_t stage_id = 0;
  bf_dev_pipe_t phy_pipe = 0;
  pipe_instr_write_reg_t *instr;
  int rc;
  int dma_buf_len = 1024 + 256;
  int dr_0_3 = 0;
  uint8_t *ilist_buf_aligned;
  int alignment = 64;
  bf_dma_addr_t int_il_buf_p = 0;
  void *int_il_buf_v = 0;
  lld_reg_dma_cmplt_info_t *reg_dma_info = NULL;
  int push_cnt = 0, alloc_cnt = 0;
  bf_dma_addr_t dma_addr_p;
  bf_dma_type_t dma_type = BF_DMA_PIPE_BLOCK_WRITE;
  bool is_tofino = lld_dev_is_tofino(dev_id);
  bool is_tofino2 = lld_dev_is_tof2(dev_id);
  bool is_tofino3 = lld_dev_is_tof3(dev_id);

  if (is_tofino) {
    if (!((reg_addr >> 25) & 0x1)) {
      /* write to non-pipe register not supported by ilist */
      fprintf(reg_get_outstream(),
              "Write to non-pipe register not supported by ilist \n");
      return BF_INVALID_ARG;
    }
    phy_pipe = LLD_DIAG_TF1_PHY_PIPE_GET(reg_addr);

    /* Extract stage_id */
    stage_id = LLD_DIAG_TF1_STAGE_GET(reg_addr);
  } else if (is_tofino2) {
    if (!((reg_addr >> 26) & 0x1)) {
      /* write to non-pipe register not supported by ilist */
      fprintf(reg_get_outstream(),
              "Write to non-pipe register not supported by ilist \n");
      return BF_INVALID_ARG;
    }
    phy_pipe = LLD_DIAG_TF2_PHY_PIPE_GET(reg_addr);

    /* Extract stage_id */
    stage_id = LLD_DIAG_TF2_STAGE_GET(reg_addr);
  } else if (is_tofino3) {
    if (!((reg_addr >> 26) & 0x1)) {
      /* write to non-pipe register not supported by ilist */
      fprintf(reg_get_outstream(),
              "Write to non-pipe register not supported by ilist \n");
      return BF_INVALID_ARG;
    }
    phy_pipe = LLD_DIAG_TF3_PHY_PIPE_GET(reg_addr);

    /* Extract stage_id */
    stage_id = LLD_DIAG_TF3_STAGE_GET(reg_addr);
  } else {
    return BF_INVALID_ARG;
  }

  do {
    rc = lld_diag_dma_alloc_with_type(
        dev_id, subdev_id, dma_buf_len, &int_il_buf_v, &int_il_buf_p, dma_type);
    alloc_cnt++;
    if (rc == -2) {
      /* Sleep for some time if buffers are unavailable */
      bf_sys_usleep(LLD_DIAG_MILLI_SEC(1));
      lld_dr_service(
          dev_id, subdev_id, lld_dr_tx_pipe_inst_list_0 + dr_0_3, 10000);
    }
  } while ((rc == -2) && (alloc_cnt < 500));
  if (rc != 0) {
    fprintf(reg_get_outstream(), "Error allocating DMA buffer for WB\n");
    return rc;
  }
  ilist_buf_aligned = (uint8_t *)(uintptr_t)(
      ((uintptr_t)int_il_buf_v + ((uint64_t)alignment - 1ull)) &
      ~((uint64_t)alignment - 1ull));
  dma_addr_p = (int_il_buf_p + ((uint64_t)alignment - 1ull)) &
               ~((uint64_t)alignment - 1ull);

  instr = (pipe_instr_write_reg_t *)(ilist_buf_aligned);

  /* Generate the instructions */
  lld_construct_instr_dest_select(
      0, (dest_select_t *)&instr[0], phy_pipe, stage_id);
  TOF_CONSTRUCT_WRITE_REG_INSTR(&instr[1], reg_addr, reg_data);

  // fprintf(reg_get_outstream(),"ilist write to register 0x%x, data 0x%x \n",
  // reg_addr, reg_data);
  /* Create the ilist info to be used in completion callback */
  reg_dma_info = bf_sys_malloc(sizeof(lld_reg_dma_cmplt_info_t));
  if (!reg_dma_info) {
    fprintf(reg_get_outstream(), "Failed to allocate memory for ilist info \n");
    return BF_NO_SYS_RESOURCES;
  }
  memset(reg_dma_info, 0, sizeof(lld_reg_dma_cmplt_info_t));
  reg_dma_info->reg_addr = reg_addr;
  reg_dma_info->phy_addr = int_il_buf_p;
  reg_dma_info->vaddr = int_il_buf_v;
  reg_dma_info->buf_size = dma_buf_len;
  reg_dma_info->type = dma_type;

  lld_register_completion_callback(dev_id,
                                   subdev_id,
                                   lld_dr_cmp_pipe_inst_list_0 + dr_0_3,
                                   lld_reg_ilist_completion_callback_fn);

  fprintf(reg_get_outstream(),
          "register write: dev=%d subdev=%d reg-addr 0x%x reg-data 0x%x, pipe "
          "%d, stage %d\n",
          dev_id,
          subdev_id,
          reg_addr,
          reg_data,
          phy_pipe,
          stage_id);

  push_cnt = 0;
  do {
    rc = lld_subdev_push_ilist(dev_id,
                               subdev_id,
                               dr_0_3,
                               dma_addr_p,
                               2 * sizeof(pipe_instr_write_reg_t),
                               0,
                               0,
                               0,
                               (uint64_t)lld_ptr_to_u64(reg_dma_info));
    lld_dr_start(dev_id, subdev_id, lld_dr_tx_pipe_inst_list_0 + dr_0_3);
    push_cnt++;
    if (rc == LLD_ERR_DR_FULL) {
      fprintf(reg_get_outstream(),
              "DR Full: Servicing DR %d to free them, push-cnt %d \n",
              lld_dr_tx_pipe_inst_list_0 + dr_0_3,
              push_cnt);
      /* Sleep for sometime */
      bf_sys_usleep(LLD_DIAG_MILLI_SEC(1));
      lld_dr_service(
          dev_id, subdev_id, lld_dr_tx_pipe_inst_list_0 + dr_0_3, 10000);
    }
  } while ((rc == LLD_ERR_DR_FULL) && (push_cnt < 1000));

  if (rc != 0) {
    fprintf(reg_get_outstream(), "Error: %d : from lld_push_ilist\n", rc);
    lld_regtest_ilist_error_set();
  } else {
    lld_regtest_dma_msgs_sent_inc();
  }
  return rc;
}

extern uint64_t tof_map_32b_to_42b_dev_sel_addr(uint32_t a32);
extern uint64_t tof2_map_32b_to_42b_dev_sel_addr(uint32_t a32);
extern uint64_t tof3_map_32b_to_42b_dev_sel_addr(uint32_t a32);
/* Write register using write-list */
bf_status_t lld_wlist_add_register_write(bf_dev_id_t dev_id,
                                         bf_subdev_id_t subdev_id,
                                         uint32_t reg_addr,
                                         uint32_t reg_data) {
  uint64_t calc;
  int rc, entry_sz, n_entries = 0, aligned_to;
  uint32_t *wd;
  int dma_buf_len = 0x400 + 512;
  bf_dma_addr_t dma_addr_p;
  void *dma_addr_v;
  bf_dma_addr_t int_wl_buf_p = 0;
  void *int_wl_buf_v = NULL;
  lld_reg_dma_cmplt_info_t *reg_dma_info = NULL;
  int push_cnt = 0, alloc_cnt = 0;
  bf_dma_type_t dma_type = BF_DMA_TM_WRITE_LIST;
  bool is_tofino = lld_dev_is_tofino(dev_id);
  bool is_tofino2 = lld_dev_is_tof2(dev_id);
  bool is_tofino3 = lld_dev_is_tof3(dev_id);

  entry_sz = 4;
  aligned_to = 64;

  do {
    rc = lld_diag_dma_alloc_with_type(
        dev_id, subdev_id, dma_buf_len, &int_wl_buf_v, &int_wl_buf_p, dma_type);
    alloc_cnt++;
    if (rc == -2) {
      /* Sleep for some time as buffers are unavailable */
      bf_sys_usleep(LLD_DIAG_MILLI_SEC(1));
      lld_dr_service(dev_id, subdev_id, lld_dr_tx_que_write_list, 10000);
    }
  } while ((rc == -2) && (alloc_cnt < 500));
  if (rc != 0) {
    fprintf(reg_get_outstream(),
            "Error allocating DMA buffer for Write list\n");
    return rc;
  }

  calc = (uintptr_t)int_wl_buf_v;
  dma_addr_p = ((int_wl_buf_p + (aligned_to - 1)) & ~(aligned_to - 1));
  dma_addr_v =
      lld_u64_to_void_ptr(((calc + (aligned_to - 1)) & ~(aligned_to - 1)));

  wd = (uint32_t *)dma_addr_v;
  uint64_t addr_42b = 0;
  if (is_tofino) addr_42b = tof_map_32b_to_42b_dev_sel_addr(reg_addr);
  if (is_tofino2) addr_42b = tof2_map_32b_to_42b_dev_sel_addr(reg_addr);
  if (is_tofino3) addr_42b = tof3_map_32b_to_42b_dev_sel_addr(reg_addr);
  *wd++ = (addr_42b & 0xffffffff);
  *wd++ = ((addr_42b >> 32ull) & 0xffffffff);
  *wd++ = reg_data;
  n_entries++;

  /* Create the ilist info to be used in completion callback */
  reg_dma_info = bf_sys_malloc(sizeof(lld_reg_dma_cmplt_info_t));
  if (!reg_dma_info) {
    fprintf(reg_get_outstream(), "Failed to allocate memory for ilist info \n");
    return BF_NO_SYS_RESOURCES;
  }
  memset(reg_dma_info, 0, sizeof(lld_reg_dma_cmplt_info_t));
  reg_dma_info->reg_addr = reg_addr;
  reg_dma_info->phy_addr = int_wl_buf_p;
  reg_dma_info->vaddr = int_wl_buf_v;
  reg_dma_info->buf_size = dma_buf_len;
  reg_dma_info->type = dma_type;
  reg_dma_info->dma_cb_type = BF_DIAG_REG_DMA_CB;

  push_cnt = 0;
  do {
    rc = lld_subdev_push_wl(dev_id,
                            subdev_id,
                            0,
                            entry_sz,
                            n_entries,
                            dma_addr_p,
                            (uint64_t)lld_ptr_to_u64(reg_dma_info));
    lld_dr_start(dev_id, subdev_id, lld_dr_tx_que_write_list);
    push_cnt++;
    if (rc == LLD_ERR_DR_FULL) {
      fprintf(reg_get_outstream(),
              "DR Full: Servicing DR %d to free them, push-cnt %d \n",
              lld_dr_tx_que_write_list,
              push_cnt);
      /* Sleep for sometime */
      bf_sys_usleep(LLD_DIAG_MILLI_SEC(1));
      lld_dr_service(dev_id, subdev_id, lld_dr_tx_que_write_list, 10000);
    }
  } while ((rc == LLD_ERR_DR_FULL) && (push_cnt < 1000));

  if (rc != 0) {
    fprintf(reg_get_outstream(), "Error: %d : from lld_push_wl\n", rc);
    lld_regtest_ilist_error_set();
  } else {
    lld_regtest_dma_msgs_sent_inc();
  }

  return rc;
}

bf_status_t lld_get_memory_parameters(uint64_t memtype,
                                      uint64_t *min_row,
                                      uint64_t *max_row,
                                      uint64_t *min_col,
                                      uint64_t *max_col,
                                      uint64_t *max_ofs) {
  switch (memtype) {
    case BF_DIAG_MEM_UNIT_RAM: /*unit rams*/
      *min_row = 0;
      *max_row = 7;
      *min_col = 2;
      *max_col = 11;
      *max_ofs = 0x3ff;
      break;
    case BF_DIAG_MEM_MAP_RAM: /*map  rams*/
      *min_row = 0;
      *max_row = 7;
      *min_col = 6;
      *max_col = 11;
      *max_ofs = 0x3ff;
      break;
    case BF_DIAG_MEM_STATS_RAM: /*stats deferred access rams*/
      *min_row = 0;
      *max_row = 3;
      *min_col = 0;
      *max_col = 0;
      *max_ofs = 143;
      break;
    case BF_DIAG_MEM_METERS_RAM: /*meters deferred access rams*/
      *min_row = 0;
      *max_row = 3;
      *min_col = 0;
      *max_col = 0;
      *max_ofs = 143;
      break;
    case BF_DIAG_MEM_TCAM: /*tcams*/
      *min_row = 0;
      *max_row = 11;
      *min_col = 0;
      *max_col = 1;
      *max_ofs = 0x1ff;
      break;
    default:
      return BF_OBJECT_NOT_FOUND;
  }

  return BF_SUCCESS;
}

bf_status_t lld_traverse_mau_memories(bf_dev_id_t dev_id) {
  uint64_t pipe, stage, memtype, row, col, offset, phy_pipe;
  uint64_t data64_0, data64_1, exp64_0, exp64_1;
  uint64_t mask64_0, mask64_1;
  int rc, consecutive_errors;
  int stride;
  bf_subdev_id_t subdev_id = 0;
  bool passed = false, overall_passed = false;
  uint32_t num_pipes = 0, num_stages = 0;
  bf_dev_pipe_t p_pipe = 0;
  bf_status_t status = BF_SUCCESS;
  bool quick = lld_diag_cfg.quick;
  bool is_tofino = lld_dev_is_tofino(dev_id);
  bool is_tofino2 = lld_dev_is_tof2(dev_id);
  bool is_tofino3 = lld_dev_is_tof3(dev_id);

  /* Init test result */
  lld_memtest_results_clear();
  lld_memtest_result_set(false);

  lld_sku_get_num_active_pipes(dev_id, &num_pipes);

  overall_passed = true;
  fprintf(reg_get_outstream(), "Num of pipes %d \n", num_pipes);
  for (pipe = 0; pipe < num_pipes; pipe++) {
    // make sure pipe is configured for testing
    if (!(lld_diag_cfg.pipe_mask & (1 << pipe))) continue;
    lld_sku_map_pipe_id_to_phy_pipe_id(dev_id, pipe, &p_pipe);
    phy_pipe = p_pipe;
    phy_pipe = phy_pipe % BF_SUBDEV_PIPE_COUNT;
    subdev_id = phy_pipe / BF_SUBDEV_PIPE_COUNT;
    lld_sku_get_num_active_mau_stages(dev_id, &num_stages, phy_pipe);
    fprintf(reg_get_outstream(),
            "Pipe %" PRIx64 ": Num of stages %d \n",
            pipe,
            num_stages);
    fprintf(reg_get_outstream(),
            "Running MAU memory test on subdev %d, logical pipe %" PRIx64 "\n",
            subdev_id,
            pipe);

    for (stage = 0; stage < num_stages; stage++) {
      // make sure stage is configured for testing
      if (!(lld_diag_cfg.stage_mask & (1 << stage))) continue;
      fprintf(reg_get_outstream(),
              "  Running MAU memory test on stage %" PRIx64 "\n",
              stage);

      for (memtype = 0; memtype < BF_DIAG_MEM_MAX; memtype++) {
        uint64_t min_row, max_row, min_col, max_col, min_ofs = 0, max_ofs = 0;
        bool is_map_ram = false;
        min_ofs = 0;
        status = lld_get_memory_parameters(
            memtype, &min_row, &max_row, &min_col, &max_col, &max_ofs);
        if (status != BF_SUCCESS) {
          continue;
        }
        if (memtype == BF_DIAG_MEM_MAP_RAM) {
          is_map_ram = true;
        } else {
          is_map_ram = false;
        }

        for (row = min_row; row <= max_row; row++) {
          for (col = min_col; col <= max_col; col++) {
            passed = true;
            uint64_t addr = 0;
            if (is_tofino) {
              uint64_t pipe_base = LLD_DIAG_TF1_PIPE_BASE_ADDR;
              addr = pipe_base | (phy_pipe << 37ull) | (stage << 33ull) |
                     (2ull << 30ull) | (memtype << 18ull) | (row << 14ull) |
                     (col << 10ull) | 0;
            } else if (is_tofino2) {
              pipe_physical_addr_t a;
              a.addr = 0;
              a.tof2.pipe_always_1 = 1;
              a.tof2.pipe_id = phy_pipe;
              a.tof2.pipe_stage = stage;
              a.tof2.pipe_ring_addr_type = 2;
              a.tof2.mem_type = memtype;
              a.tof2.mem_row = row;
              a.tof2.mem_col = col;
              addr = a.addr;
            } else if (is_tofino3) {
              pipe_physical_addr_t a;
              a.addr = 0;
              a.tof3.pipe_always_1 = 1;
              a.tof3.pipe_id = phy_pipe;
              a.tof3.pipe_stage = stage;
              a.tof3.pipe_ring_addr_type = 2;
              a.tof3.mem_type = memtype;
              a.tof3.mem_row = row;
              a.tof3.mem_col = col;
              addr = a.addr;
            }

            if (is_map_ram) {
              /* For map rams use this mask as wider values can fail.  If the
               * RAM is configured for 6 bit idletime the remaining 5 bits are
               * used by the HW for ECC so they cannot be included in the
               * access. */
              mask64_0 = 0;
              mask64_1 = 0x3f;
            } else {
              // try to automatically determine the valid bits in the register
              rc = lld_subdev_ind_write(dev_id,
                                        subdev_id,
                                        addr,
                                        LLD_DIAG_ULONG_DATA_MASK,
                                        LLD_DIAG_ULONG_DATA_MASK);
              if (rc != 0) {
                LOG_ERROR("Error: %d <%xh> : addr=%016" PRIx64
                          " : determining mask (wr)",
                          rc,
                          rc,
                          addr);
                lld_memtest_write_error_set(addr);
                return rc;
              }
              rc = lld_subdev_ind_read(
                  dev_id, subdev_id, addr, &mask64_0, &mask64_1);
              if (rc != 0) {
                LOG_ERROR("Error: %d <%xh> : addr=%016" PRIx64
                          " : determining mask (rd)",
                          rc,
                          rc,
                          addr);
                lld_memtest_read_error_set(addr);
                return rc;
              }
            }

            // special-case for TCAM parity bits 46:45
            if (memtype == BF_DIAG_MEM_TCAM) {
              mask64_0 &= ~LLD_DIAG_TCAM_PARITY_BITS;
              mask64_1 &= ~LLD_DIAG_TCAM_PARITY_BITS;
            }
            if (quick) {
              stride = max_ofs;
            } else {
              stride = 1;
            }
            for (offset = min_ofs; offset <= max_ofs; offset += stride) {
              lld_diag_memtest_calc_exp_result(
                  lld_diag_test_pattern_get(),
                  addr,
                  offset,
                  lld_diag_test_pattern_data0_get(),
                  lld_diag_test_pattern_data1_get(),
                  mask64_0,
                  mask64_1,
                  &data64_0,
                  &data64_1);
              /* Write only relevant bits of map-ram, higher bits are used
                 as mask for 2b meter color updates
              */
              if (is_map_ram) {
                data64_0 &= mask64_0;
                data64_1 &= mask64_1;
              }
              rc = lld_subdev_ind_write(
                  dev_id, subdev_id, addr + offset, data64_0, data64_1);
              if (rc != 0) {
                LOG_ERROR("Error: %d <%xh> : Wr : addr=%016" PRIx64,
                          rc,
                          rc,
                          addr + offset);
                lld_memtest_write_error_set(addr + offset);
                return rc;
              }
            }
            // then all the readbacks
            consecutive_errors = 0;
            for (offset = min_ofs; offset <= max_ofs; offset += stride) {
              lld_diag_memtest_calc_exp_result(
                  lld_diag_test_pattern_get(),
                  addr,
                  offset,
                  lld_diag_test_pattern_data0_get(),
                  lld_diag_test_pattern_data1_get(),
                  mask64_0,
                  mask64_1,
                  &exp64_0,
                  &exp64_1);
              rc = lld_subdev_ind_read(
                  dev_id, subdev_id, addr + offset, &data64_0, &data64_1);
              if (rc != 0) {
                LOG_ERROR("Error: %d <%xh> : Rd : addr=%016" PRIx64,
                          rc,
                          rc,
                          addr + offset);
                lld_memtest_read_error_set(addr);
                return rc;
              }
              if (((data64_0 & mask64_0) != exp64_0) ||
                  ((data64_1 & mask64_1) != exp64_1)) {
                passed = false;
                overall_passed = false;
                LOG_ERROR("Data Error: addr=%016" PRIx64
                          "(pipe=%d : stage=%d : typ=%d : row=%d : "
                          "col=%d, offset=%d)",
                          addr + offset,
                          (int)phy_pipe,
                          (int)stage,
                          (int)memtype,
                          (int)row,
                          (int)col,
                          (int)offset);
                LOG_ERROR("-- expected : %016" PRIx64 "_%016" PRIx64,
                          exp64_0,
                          exp64_1);
                LOG_ERROR("--      got : %016" PRIx64 "_%016" PRIx64,
                          data64_0,
                          data64_1);
                LOG_ERROR("--     mask : %016" PRIx64 "_%016" PRIx64,
                          mask64_0,
                          mask64_1);
                lld_memtest_data_error_set(addr,
                                           exp64_0,
                                           exp64_1,
                                           data64_0,
                                           data64_1,
                                           mask64_0,
                                           mask64_1);

                if (LLD_DIAG_MAX_ALLOWED_FAILURES < ++consecutive_errors) {
                  return BF_INTERNAL_ERROR;
                }
              } else {
                consecutive_errors = 0;
              }
            }
            if (passed) {
              LOG_TRACE("Passed : %016" PRIx64
                        " : pipe=%d : stage=%d : typ=%d : row=%d : col=%d",
                        addr,
                        (int)phy_pipe,
                        (int)stage,
                        (int)memtype,
                        (int)row,
                        (int)col);
            } else {
              LOG_ERROR("FAILED : %016" PRIx64
                        " : pipe=%d : stage=%d : typ=%d : row=%d : col=%d",
                        addr,
                        (int)phy_pipe,
                        (int)stage,
                        (int)memtype,
                        (int)row,
                        (int)col);
            }
          }
        }
      }
    }
  }
  if (!overall_passed) {
    return BF_OBJECT_NOT_FOUND;
  }
  lld_memtest_result_set(true);
  return BF_SUCCESS;
}

bf_status_t lld_traverse_mau_memories_dma(bf_dev_id_t dev_id) {
  bf_dev_pipe_t pipe;
  uint64_t phy_pipe;
  bf_subdev_id_t subdev_id = 0;
  uint64_t stage, memtype, row, col;
  uint32_t num_pipes = 0, num_stages = 0;
  bf_dev_pipe_t p_pipe = 0;
  bf_status_t status = BF_SUCCESS;
  bool quick = lld_diag_cfg.quick;
  bool is_tofino = lld_dev_is_tofino(dev_id);
  bool is_tofino2 = lld_dev_is_tof2(dev_id);
  bool is_tofino3 = lld_dev_is_tof3(dev_id);

  /* Init test result to pass */
  lld_memtest_results_clear();
  lld_memtest_result_set(true);

  lld_sku_get_num_active_pipes(dev_id, &num_pipes);
  fprintf(reg_get_outstream(), "Num of pipes %d \n", num_pipes);

  for (pipe = 0; pipe < num_pipes; pipe++) {
    // make sure pipe is configured for testing
    if (!(lld_diag_cfg.pipe_mask & (1 << pipe))) continue;
    lld_sku_map_pipe_id_to_phy_pipe_id(dev_id, pipe, &p_pipe);
    phy_pipe = p_pipe;
    phy_pipe = phy_pipe % BF_SUBDEV_PIPE_COUNT;
    subdev_id = phy_pipe / BF_SUBDEV_PIPE_COUNT;
    lld_sku_get_num_active_mau_stages(dev_id, &num_stages, phy_pipe);
    fprintf(
        reg_get_outstream(), "Pipe %d: Num of stages %d \n", pipe, num_stages);
    fprintf(reg_get_outstream(),
            "Running MAU memory test on subdev %d, logical pipe %d\n",
            subdev_id,
            pipe);

    for (stage = 0; stage < num_stages; stage++) {
      // make sure stage is configured for testing
      if (!(lld_diag_cfg.stage_mask & (1 << stage))) continue;
      fprintf(reg_get_outstream(),
              "  Running MAU memory test on stage %" PRIx64 "\n",
              stage);

      for (memtype = 0; memtype < BF_DIAG_MEM_MAX; memtype++) {
        uint64_t min_row, max_row, min_col, max_col, min_ofs = 0, max_ofs = 0;
        min_ofs = 0;
        status = lld_get_memory_parameters(
            memtype, &min_row, &max_row, &min_col, &max_col, &max_ofs);
        if (status != BF_SUCCESS) {
          continue;
        }

        if (quick) {
          if (max_ofs > (min_ofs + 1)) {
            max_ofs = min_ofs + 1;
          }
        }

        // first all the writes
        for (row = min_row; row <= max_row; row++) {
          for (col = min_col; col <= max_col; col++) {
            uint64_t addr = 0;
            char mem_name[132] = {0};
            if (is_tofino) {
              uint64_t pipe_base = LLD_DIAG_TF1_PIPE_BASE_ADDR;
              addr = pipe_base | (phy_pipe << 37ull) | (stage << 33ull) |
                     (2ull << 30ull) | (memtype << 18ull) | (row << 14ull) |
                     (col << 10ull) | 0;
            } else if (is_tofino2) {
              pipe_physical_addr_t a;
              a.addr = 0;
              a.tof2.pipe_always_1 = 1;
              a.tof2.pipe_id = phy_pipe;
              a.tof2.pipe_stage = stage;
              a.tof2.pipe_ring_addr_type = 2;
              a.tof2.mem_type = memtype;
              a.tof2.mem_row = row;
              a.tof2.mem_col = col;
              addr = a.addr;
            } else if (is_tofino3) {
              pipe_physical_addr_t a;
              a.addr = 0;
              a.tof3.pipe_always_1 = 1;
              a.tof3.pipe_id = phy_pipe;
              a.tof3.pipe_stage = stage;
              a.tof3.pipe_ring_addr_type = 2;
              a.tof3.mem_type = memtype;
              a.tof3.mem_row = row;
              a.tof3.mem_col = col;
              addr = a.addr;
            }
            sprintf(mem_name,
                    "pipe=%" PRIu64 " : stage=%" PRIu64 " : typ=%" PRIu64
                    " : row=%" PRIu64 " : col=%" PRIu64 "",
                    phy_pipe,
                    stage,
                    memtype,
                    row,
                    col);

            status |= lld_mem_test_dma_cb(dev_id,
                                          subdev_id,
                                          addr + min_ofs,
                                          (max_ofs - min_ofs + 1) * 16,
                                          mem_name);
          }
        }
      }
    }
    if (quick) {
      bf_sys_usleep(LLD_DIAG_SEC(3));
    } else {
      bf_sys_usleep(LLD_DIAG_SEC(6));
    }
  }
  if (status != BF_SUCCESS) {
    lld_memtest_result_set(false);
  }
  /* Wait for DMA callbacks to finish */
  fprintf(reg_get_outstream(), "Waiting for dma to complete \n");
  bf_sys_usleep(LLD_DIAG_SEC(8));
  return status;
}

void lld_memtest_completion_callback_fn(int dev_id,
                                        bf_subdev_id_t subdev_id,
                                        bf_dma_dr_id_t dr,
                                        uint64_t ts_sz,
                                        uint32_t attr,
                                        uint32_t status,
                                        uint32_t type,
                                        uint64_t msg_id,
                                        int s,
                                        int e) {
  int i, rc, consecutive_failures;
  lld_mem_dma_cmplt_info_t *sm_p =
      (lld_mem_dma_cmplt_info_t *)lld_u64_to_void_ptr(msg_id);
  uint64_t data64_0, data64_1;
  bool passed = true;
  bool is_tofino = lld_dev_is_tofino(dev_id);
  bool is_tofino2 = lld_dev_is_tof2(dev_id);
  bool is_tofino3 = lld_dev_is_tof3(dev_id);
  pipe_physical_addr_t addr;
  if (!sm_p) {
    fprintf(
        reg_get_outstream(),
        "ERROR: memtest info is NULL in reg completion callback, bailing out "
        "\n");
    return;
  }
  addr.addr = sm_p->tofino_addr;

  (void)s;
  (void)e;
  (void)dr;
  (void)ts_sz;
  (void)attr;

  if (status != 0) {
    fprintf(
        reg_get_outstream(),
        "ERROR: Error status %d in memtest completion callback, dev %d, dr %d, "
        "type %d\n",
        status,
        dev_id,
        dr,
        type);
    return;
  }

  if ((type != tx_m_type_wr_blk) && (type != tx_m_type_que_wr_list)) {
    fprintf(reg_get_outstream(),
            "Hmm, not a write-blk or write-list completion? <typ=%d>\n",
            type);
    return;
  }

  /* Inc number of completions received */
  lld_memtest_dma_cmplts_rcvd_inc();
#if 0
  fprintf(reg_get_outstream(),"DMA c/b: dev=%d, dr=%d, sz=%d, attr=%xh, sts=%xh, type=%d,"
      "id=%"PRIx64" : addr=%016"PRIx64"\n",
             dev_id, dr, data_sz, attr, status, type, msg_id,
             sm_p->tofino_addr );
#endif

  rc = lld_diag_dma_free_with_type(dev_id, subdev_id, sm_p->vaddr, sm_p->type);
  if (rc != 0) {
    LOG_ERROR("Unable to unmap DMA buffer %" PRIx64 " at %s:%d",
              sm_p->phy_addr,
              __func__,
              __LINE__);
  }

  // readback each entry and verify the entry # matches (i.e. no aliasing)
  consecutive_failures = 0;

  if (type == tx_m_type_que_wr_list) {
    fprintf(reg_get_outstream(),
            "Addr-> %016" PRIx64 " - %016" PRIx64 " : Checking .. : \n",
            sm_p->tofino_addr,
            sm_p->tofino_addr + sm_p->n_entry);

    for (i = 0; i < sm_p->n_entry; i++) {
      uint64_t exp64_0 = 0, exp64_1 = 0;
      lld_diag_memtest_calc_exp_result(sm_p->pattern,
                                       i,
                                       sm_p->n_entry,
                                       sm_p->pattern_data0,
                                       sm_p->pattern_data1,
                                       sm_p->mask64_0,
                                       sm_p->mask64_1,
                                       &exp64_0,
                                       &exp64_1);

      rc = lld_subdev_ind_read(
          dev_id, subdev_id, sm_p->tofino_addr + i, &data64_0, &data64_1);

      if (rc != 0) {
        fprintf(reg_get_outstream(), "Error: %d <%xh> : entry=%d\n", rc, rc, i);
        lld_memtest_read_error_set(sm_p->tofino_addr);
        bf_sys_free(sm_p);
        return;
      }
      if (((data64_0 & sm_p->mask64_0) != exp64_0) ||
          ((data64_1 & sm_p->mask64_1) != exp64_1)) {
        passed = false;  // set "failed"
        LOG_ERROR("Data Error   : %016" PRIx64 " : entry=%d : Exp: %016" PRIx64
                  "_%016" PRIx64 " : Got: %016" PRIx64 "_%016" PRIx64 "\n",
                  sm_p->tofino_addr,
                  i,
                  exp64_0,
                  exp64_1,
                  data64_0,
                  data64_1);
        lld_memtest_data_error_set(sm_p->tofino_addr,
                                   exp64_0,
                                   exp64_1,
                                   data64_0,
                                   data64_1,
                                   sm_p->mask64_0,
                                   sm_p->mask64_1);
        if (LLD_DIAG_MAX_ALLOWED_FAILURES < ++consecutive_failures) {
          break;
        }
      } else {
        consecutive_failures = 0;
      }
    }
    if (passed) {
      LOG_TRACE(
          "Passed : %016" PRIx64 " : %s\n", sm_p->tofino_addr, sm_p->name);
    } else {
      lld_memtest_result_set(false);
      LOG_ERROR(
          "FAILED : %016" PRIx64 " : %s\n", sm_p->tofino_addr, sm_p->name);
    }
    bf_sys_free(sm_p);
    return;
  }

  for (i = 0; i < sm_p->n_entry; i++) {
    uint64_t exp64_0 = 0, exp64_1 = 0;
    lld_diag_memtest_calc_exp_result(sm_p->pattern,
                                     i,
                                     sm_p->n_entry,
                                     sm_p->pattern_data0,
                                     sm_p->pattern_data1,
                                     sm_p->mask64_0,
                                     sm_p->mask64_1,
                                     &exp64_0,
                                     &exp64_1);
    bool is_map_ram = false, is_tm_addr = false;
    uint64_t memtype = 0;

    if (is_tofino) {
      memtype = tf1_get_mem_type_from_pipe_addr(sm_p->tofino_addr);
    } else if (is_tofino2) {
      memtype = tf2_get_mem_type_from_pipe_addr(sm_p->tofino_addr);
    } else if (is_tofino3) {
      memtype = tf3_get_mem_type_from_pipe_addr(sm_p->tofino_addr);
    } else {
      LOG_ERROR("Unknown device type");
      return;
    }

    rc = lld_subdev_ind_read(
        dev_id, subdev_id, sm_p->tofino_addr + i, &data64_0, &data64_1);

    if (rc != 0) {
      fprintf(reg_get_outstream(), "Error: %d <%xh> : entry=%d\n", rc, rc, i);
      bf_sys_free(sm_p);
      return;
    }

    if ((is_tofino && 2 == ((sm_p->tofino_addr >> 40) & 3)) ||
        (is_tofino2 && addr.tof2.pipe_always_1 == 1) ||
        (is_tofino3 && addr.tof3.pipe_always_1 == 1)) {
      is_tm_addr = false;
    } else {
      is_tm_addr = true;
    }

    if ((memtype == BF_DIAG_MEM_MAP_RAM) && (!is_tm_addr)) {
      is_map_ram = true;
      exp64_1 = 0x3f;
      exp64_0 = 0;
    } else {
      is_map_ram = false;
    }
    if (((data64_0 & sm_p->mask64_0) != exp64_0) ||
        ((data64_1 & sm_p->mask64_1) != exp64_1)) {
      passed = false;  // set "failed"
      LOG_ERROR("Data Error   : %016" PRIx64 " : entry=%d : Exp: %016" PRIx64
                "_%016" PRIx64 " : Got: %016" PRIx64 "_%016" PRIx64 "\n",
                sm_p->tofino_addr,
                i,
                exp64_0,
                exp64_1,
                data64_0,
                data64_1);
      lld_memtest_data_error_set(sm_p->tofino_addr,
                                 exp64_0,
                                 exp64_1,
                                 data64_0,
                                 data64_1,
                                 sm_p->mask64_0,
                                 sm_p->mask64_1);
      if (!lld_diag_cfg.quick) {
        lld_subdev_ind_read(
            dev_id, subdev_id, sm_p->tofino_addr + i, &data64_0, &data64_1);

        if ((data64_0 != exp64_0) || (data64_1 != exp64_1)) {
          fprintf(reg_get_outstream(),
                  "Data Error(2): %016" PRIx64 " : entry=%d : Exp: %016" PRIx64
                  "_%016" PRIx64 " : Got: %016" PRIx64 "_%016" PRIx64 "\n",
                  sm_p->tofino_addr,
                  i,
                  exp64_0,
                  exp64_1,
                  data64_0,
                  data64_1);
          lld_subdev_ind_write(
              dev_id, subdev_id, sm_p->tofino_addr + i, exp64_0, exp64_1);

          lld_subdev_ind_read(
              dev_id, subdev_id, sm_p->tofino_addr + i, &data64_0, &data64_1);

          if ((data64_0 != exp64_0) || (data64_1 != exp64_1)) {
            fprintf(reg_get_outstream(),
                    "Data Error(3): %016" PRIx64
                    " : entry=%d : Exp: %016" PRIx64 "_%016" PRIx64
                    " : Got: %016" PRIx64 "_%016" PRIx64 "\n",
                    sm_p->tofino_addr,
                    i,
                    exp64_0,
                    exp64_1,
                    data64_0,
                    data64_1);
          } else {
            fprintf(reg_get_outstream(), "Fixed itself on re-write\n");
          }
        } else {
          fprintf(reg_get_outstream(), "Fixed itself on re-read\n");
        }
      }

      if (LLD_DIAG_MAX_ALLOWED_FAILURES < ++consecutive_failures) {
        break;
      }
    } else {
      consecutive_failures = 0;
      /* Restore MAP ram to some value so that it continues counting */
      if (is_map_ram) {
        lld_subdev_ind_write(dev_id, subdev_id, sm_p->tofino_addr + i, 0, 0x9);
      }
    }
  }
  if (passed) {
    LOG_TRACE("Passed : %016" PRIx64 " : %s\n", sm_p->tofino_addr, sm_p->name);
  } else {
    lld_memtest_result_set(false);
    LOG_ERROR("FAILED : %016" PRIx64 " : %s\n", sm_p->tofino_addr, sm_p->name);
  }
  bf_sys_free(sm_p);
  return;
}

void lld_reg_mem_test_completion_cb(int dev_id,
                                    bf_subdev_id_t subdev_id,
                                    bf_dma_dr_id_t dr,
                                    uint64_t ts_sz,
                                    uint32_t attr,
                                    uint32_t status,
                                    uint32_t type,
                                    uint64_t msg_id,
                                    int s,
                                    int e) {
  lld_mem_dma_cmplt_info_t *sm_p =
      (lld_mem_dma_cmplt_info_t *)lld_u64_to_void_ptr(msg_id);

  if (!sm_p) {
    fprintf(
        reg_get_outstream(),
        "ERROR: memtest info is NULL in dma completion callback for dev %d, "
        "subdev %d, and msg_id  %016" PRIx64 " bailing out \n",
        dev_id,
        subdev_id,
        msg_id);
    return;
  }

  if (sm_p->dma_cb_type ==
      BF_DIAG_REG_DMA_CB)  // DMA call back is for register DMA
  {
    lld_reg_ilist_completion_callback_fn(
        dev_id, subdev_id, dr, ts_sz, attr, status, type, msg_id, s, e);
  } else if (sm_p->dma_cb_type ==
             BF_DIAG_MEM_DMA_CB)  // DMA call back is for memory DMA
  {
    lld_memtest_completion_callback_fn(
        dev_id, subdev_id, dr, ts_sz, attr, status, type, msg_id, s, e);
  }
  return;
}

bf_status_t lld_mem_test_sch_refresh_enable(bf_dev_id_t dev_id, bool enable) {
  bf_status_t rc = BF_SUCCESS;
  uint32_t val = 0;
  uint32_t ref_en = enable ? 1 : 0;
  bool is_tofino2 = lld_dev_is_tof2(dev_id);
  bool is_tofino3 = lld_dev_is_tof3(dev_id);
  uint32_t offset = 0;
  bf_subdev_id_t subdev_id = 0;
  int num_subdev = 0;
  uint32_t num_pipes = 0;

  if (!is_tofino2 && !is_tofino3) {
    // Silenty return for non TF2 and TF3 platforms
    return rc;
  }

  lld_sku_get_num_active_pipes(dev_id, &num_pipes);
  num_subdev = num_pipes / BF_SUBDEV_PIPE_COUNT;

  for (int index = 0; index < 4; index++) {
    if (is_tofino2) {
      if (index < 2) {
        offset = offsetof(tof2_reg,
                          device_select.tm_top.tm_scha_top.sch[index].ctrl);
      } else {
        offset = offsetof(tof2_reg,
                          device_select.tm_top.tm_schb_top.sch[index - 2].ctrl);
      }
    } else {
      offset =
          offsetof(tof3_reg, device_select.tm_top.tm_sch_top.sch[index].ctrl);
    }

    for (subdev_id = 0; subdev_id < num_subdev; subdev_id++) {
      rc = lld_subdev_read_register(dev_id, subdev_id, offset, &val);
      if (rc != BF_SUCCESS) {
        return rc;
      }

      if (is_tofino2) {
        if ((!getp_tof2_sch_ctrl_r_rate_refresh_en(&val) && ref_en) ||
            (getp_tof2_sch_ctrl_r_rate_refresh_en(&val) && !ref_en)) {
          setp_tof2_sch_ctrl_r_rate_refresh_en(&val, ref_en);
        } else {
          continue;
        }
      } else {
        if ((!getp_tof3_sch_ctrl_r_rate_refresh_en(&val) && ref_en) ||
            (getp_tof3_sch_ctrl_r_rate_refresh_en(&val) && !ref_en)) {
          setp_tof3_sch_ctrl_r_rate_refresh_en(&val, ref_en);
        } else {
          continue;
        }
      }

      rc = lld_subdev_write_register(dev_id, subdev_id, offset, val);
      if (rc != BF_SUCCESS) {
        return rc;
      }
    }
  }

  return rc;
}

bf_status_t lld_mem_test_dma_cb(bf_dev_id_t dev_id,
                                bf_subdev_id_t subdev_id,
                                uint64_t offset,
                                uint32_t len,
                                char *key_wd) {
  uint64_t mask64_0, mask64_1;
  uint64_t exp64_0, exp64_1;
  uint8_t *unaligned_p;
  uint64_t *wb_buf64, memtype = 0;
  int aligned_to = 64;
  int i, rc, n_entry = len / 16, entry_sz = 16;
  lld_mem_dma_cmplt_info_t *sm_p;
  int is_tm_addr = 0;
  bf_dma_addr_t dma_addr_p;
  int dma_buf_len = 0;
  bf_status_t status = BF_SUCCESS;
  int push_cnt = 0, alloc_cnt = 0;
  bf_dma_dr_id_t dma_dr = 0;
  bf_dma_addr_t diag_buf_p = 0;
  void *diag_buf_v = NULL;
  void *vaddr = NULL;
  uint64_t paddr = 0;
  bf_dma_type_t dma_type = 0;
  bool is_map_ram = false;
  bf_dev_pipe_t log_pipe = 0;
  uint32_t num_stages = 0;
  pipe_physical_addr_t addr;
  addr.addr = offset;
  bool is_tofino = lld_dev_is_tofino(dev_id);
  bool is_tofino2 = lld_dev_is_tof2(dev_id);
  bool is_tofino3 = lld_dev_is_tof3(dev_id);

  if (n_entry < 1) {
    // fprintf(reg_get_outstream(),"DMA Mem test: %016"PRIx64" : %d bytes : %s
    // -- skipped\n", offset,
    // len, key_wd );
    return BF_SUCCESS;
  }

  if ((is_tofino && 2 == ((offset >> 40) & 3)) ||
      (is_tofino2 && addr.tof2.pipe_always_1 == 1) ||
      (is_tofino3 && addr.tof3.pipe_always_1 == 1)) {
    int stage = 0;
    bf_dev_pipe_t pipe = 0;

    if (lld_diag_cfg.do_tm == 1) return BF_SUCCESS;  // only do TM mems

    if (is_tofino3) {
      if (strstr(key_wd, "e_prsr_mem")) return BF_SUCCESS;
    }

    if (is_tofino) {
      pipe = addr.tof.pipe_id_39_37;
      stage = addr.tof.pipe_element_36_33;
      memtype = addr.tof.mem_type;
    } else if (is_tofino2) {
      pipe = addr.tof2.pipe_id;
      stage = addr.tof2.pipe_stage;
      memtype = addr.tof2.mem_type;
    } else if (is_tofino3) {
      pipe = addr.tof3.pipe_id;
      stage = addr.tof3.pipe_stage;
      memtype = addr.tof3.mem_type;
    }
    is_map_ram = BF_DIAG_MEM_MAP_RAM == memtype;

    lld_sku_map_phy_pipe_id_to_pipe_id(dev_id, pipe, &log_pipe);
    if (!(lld_diag_cfg.pipe_mask &
          (1 << (log_pipe + subdev_id * BF_SUBDEV_PIPE_COUNT))))
      return BF_SUCCESS;

    /* Allow parde stages but filter MAU stages by the diag config. */
    lld_sku_get_num_active_mau_stages(dev_id, &num_stages, pipe);
    if (stage < (int)num_stages) {
      if (!(lld_diag_cfg.stage_mask & (1 << stage))) return BF_SUCCESS;
    }
  } else {                                           // not a pipe address
    if (lld_diag_cfg.do_tm == 0) return BF_SUCCESS;  // only do pipe mems
    is_tm_addr = 1;

    /* Some TM memories on Tofino2 are read-only, so skip them. */
    if (is_tofino2 || is_tofino3) {
      if (strstr(key_wd, "p_occ_mem")) return BF_SUCCESS;
      if (strstr(key_wd, "l1_occ_mem")) return BF_SUCCESS;
      if (strstr(key_wd, "l2_occ_mem")) return BF_SUCCESS;
      if (strstr(key_wd, "q_occ_mem")) return BF_SUCCESS;

      if (strstr(key_wd, "csr_memory_wac_")) {
        if (strstr(key_wd, "count")) return BF_SUCCESS;
        if (strstr(key_wd, "st")) return BF_SUCCESS;
        if (strstr(key_wd, "ppg_pfc")) return BF_SUCCESS;
        if (strstr(key_wd, "qid_map")) return BF_SUCCESS;
      }
      if (strstr(key_wd, "csr_memory_clc_clm")) return BF_SUCCESS;
      if (strstr(key_wd, "csr_memory_pex")) return BF_SUCCESS;
      if (strstr(key_wd, "csr_memory_prc_cache")) return BF_SUCCESS;
      if (strstr(key_wd, "csr_memory_qlc_ht")) return BF_SUCCESS;
      if (strstr(key_wd, "csr_memory_qlc_qlm")) return BF_SUCCESS;
    }

    if (is_tofino3) {
      uint32_t num_subdev = 0;
      lld_sku_get_num_subdev(dev_id, &num_subdev, NULL);
      if (num_subdev < 2) {
        if (strstr(key_wd, "qac_pipe_mem[4]")) return BF_SUCCESS;
        if (strstr(key_wd, "qac_pipe_mem[5]")) return BF_SUCCESS;
        if (strstr(key_wd, "qac_pipe_mem[6]")) return BF_SUCCESS;
        if (strstr(key_wd, "qac_pipe_mem[7]")) return BF_SUCCESS;

        if (strstr(key_wd, "prc__prc_mem[4]")) return BF_SUCCESS;
        if (strstr(key_wd, "prc__prc_mem[5]")) return BF_SUCCESS;
        if (strstr(key_wd, "prc__prc_mem[6]")) return BF_SUCCESS;
        if (strstr(key_wd, "prc__prc_mem[7]")) return BF_SUCCESS;

        if (strstr(key_wd, "qlc_mem[4]")) return BF_SUCCESS;
        if (strstr(key_wd, "qlc_mem[5]")) return BF_SUCCESS;
        if (strstr(key_wd, "qlc_mem[6]")) return BF_SUCCESS;
        if (strstr(key_wd, "qlc_mem[7]")) return BF_SUCCESS;

        if (strstr(key_wd, "pre_pipe_mem[4]")) return BF_SUCCESS;
        if (strstr(key_wd, "pre_pipe_mem[5]")) return BF_SUCCESS;
        if (strstr(key_wd, "pre_pipe_mem[6]")) return BF_SUCCESS;
        if (strstr(key_wd, "pre_pipe_mem[7]")) return BF_SUCCESS;
      }
      // TODO: Failure seen when test for different pattern. Should anlayze
      if (strstr(key_wd, "csr_memory_qac_port_wm_cell_count"))
        return BF_SUCCESS;
    }
  }

  // trim large memories to max DMA sz
  if (len > LLD_MAX_DMA_SZ) {
    fprintf(reg_get_outstream(),
            "Len trimmed from %dB to %dB (LLD_MAX_DMA_SZ)\n",
            len,
            LLD_MAX_DMA_SZ);
    len = LLD_MAX_DMA_SZ;
  }
  if (is_tm_addr) {
    n_entry = len / (8 + 16);
    dma_buf_len = 0x1000;
    dma_dr = lld_dr_tx_que_write_list;
    dma_type = BF_DMA_TM_WRITE_LIST;
  } else {
    n_entry = len / 16;
    dma_buf_len = 0x4000;
    dma_dr = lld_dr_tx_pipe_write_block;
    dma_type = BF_DMA_PIPE_BLOCK_WRITE;
  }

  if (lld_diag_cfg.quick) {
    if (n_entry > 2) {
      n_entry = 2;
    }
  }
  // length check
  if ((entry_sz * n_entry) > dma_buf_len) {
    fprintf(reg_get_outstream(),
            "Length %d is greater than DMA buffer length: %d\n",
            (entry_sz * n_entry),
            dma_buf_len);
    n_entry = dma_buf_len / entry_sz;
    fprintf(
        reg_get_outstream(), "Adjusted Length to %d \n", (entry_sz * n_entry));
  }

  /* Determining mask */
  if (is_map_ram) {
    /* For map rams use this mask as wider values can fail.  If the RAM is
     * configured for 6 bit idletime the remaining 5 bits are used by the HW for
     * ECC so they cannot be included in the access. */
    mask64_0 = 0;
    mask64_1 = 0x3f;
  } else {
    rc = lld_subdev_ind_write(dev_id,
                              subdev_id,
                              offset,
                              LLD_DIAG_ULONG_DATA_MASK,
                              LLD_DIAG_ULONG_DATA_MASK);
    if (rc != 0) {
      fprintf(reg_get_outstream(),
              "Error: %d <%xh> : determining mask (wr)\n",
              rc,
              rc);
      return rc;
    }
    rc = lld_subdev_ind_read(dev_id, subdev_id, offset, &mask64_0, &mask64_1);
    if (rc != 0) {
      fprintf(reg_get_outstream(),
              "            : Error: %d <%xh> : determining mask (rd)\n",
              rc,
              rc);
      return rc;
    }
    if ((mask64_0 == 0ull) && (mask64_1 == 0ull)) {
      fprintf(reg_get_outstream(),
              "            : skipping %016" PRIx64
              " due to no valid bits in mask\n",
              offset);
      return BF_SUCCESS;
    }
    // special-case for TCAM parity bits 46:45
    if (memtype == BF_DIAG_MEM_TCAM) {
      mask64_0 &= ~LLD_DIAG_TCAM_PARITY_BITS;
      mask64_1 &= ~LLD_DIAG_TCAM_PARITY_BITS;
    }
  }

  // fprintf(reg_get_outstream(),"      : Mask : %016"PRIx64"_%016"PRIx64"\n",
  // mask64_0, mask64_1 );

  unaligned_p = (uint8_t *)bf_sys_malloc(len + aligned_to +
                                         sizeof(lld_mem_dma_cmplt_info_t));
  sm_p = (lld_mem_dma_cmplt_info_t *)unaligned_p;
  sm_p->base_p = unaligned_p;
  sm_p->tofino_addr = offset;
  sm_p->entry_sz = entry_sz;
  sm_p->n_entry = n_entry;
  sm_p->mask64_0 = mask64_0;
  sm_p->mask64_1 = mask64_1;
  sprintf(sm_p->name_storage, "%s", key_wd);
  sm_p->name = &sm_p->name_storage[0];

  if (is_tm_addr) {
    do {
      vaddr = NULL;
      paddr = 0;
      rc = lld_diag_dma_alloc_with_type(
          dev_id, subdev_id, dma_buf_len, &vaddr, &paddr, dma_type);
      alloc_cnt++;
      if (rc == -2) {
        // fprintf(reg_get_outstream(),"Sleeping as buf are unavail, alloc-cnt
        // %d, type %d \n",
        // alloc_cnt, dma_type);
        /* Sleep for some time if buffers are unavailable */
        bf_sys_usleep(LLD_DIAG_MILLI_SEC(1));
        lld_dr_service(dev_id, subdev_id, dma_dr, 10000);
      }
    } while ((rc == -2) && (alloc_cnt < 500));

    if (rc != 0) {
      fprintf(reg_get_outstream(), "Error allocating DMA buffer for WL \n");
      bf_sys_free(sm_p);
      return rc;
    }
    diag_buf_p = paddr;
    diag_buf_v = vaddr;
    dma_addr_p = ((diag_buf_p + (aligned_to - 1)) & ~(aligned_to - 1));

    wb_buf64 = (uint64_t *)diag_buf_v;
  } else {
    do {
      vaddr = NULL;
      paddr = 0;
      rc = lld_diag_dma_alloc_with_type(
          dev_id, subdev_id, dma_buf_len, &vaddr, &paddr, dma_type);
      alloc_cnt++;
      if (rc == -2) {
        // fprintf(reg_get_outstream(),"Sleeping as buf are unvail, alloc-cnt
        // %d, type %d \n",
        // alloc_cnt, dma_type);
        lld_dr_service(dev_id, subdev_id, dma_dr, 10000);
        /* Sleep for some time if buffers are unavailable */
        bf_sys_usleep(LLD_DIAG_MILLI_SEC(1));
      }
    } while ((rc == -2) && (alloc_cnt < 500));
    if (rc != 0) {
      fprintf(reg_get_outstream(), "Error allocating DMA buffer for WB \n");
      lld_print_dr_stats();
      bf_sys_free(sm_p);
      return BF_INVALID_ARG;
    }
    diag_buf_p = paddr;
    diag_buf_v = vaddr;
    dma_addr_p = ((diag_buf_p + (aligned_to - 1)) & ~(aligned_to - 1));

    wb_buf64 = (uint64_t *)diag_buf_v;
  }
  sm_p->type = dma_type;
  sm_p->buf_size = dma_buf_len;
  sm_p->vaddr = vaddr;
  sm_p->phy_addr = paddr;
  sm_p->pattern = lld_diag_test_pattern_get();
  sm_p->pattern_data0 = lld_diag_test_pattern_data0_get();
  sm_p->pattern_data1 = lld_diag_test_pattern_data1_get();
  sm_p->dma_cb_type = BF_DIAG_MEM_DMA_CB;

  for (i = 0; i < n_entry; i++) {
    /* Write 0x3f to map-ram so that it asic stops incrementing it */
    if (is_map_ram) {
      exp64_0 = ((uint64_t)(0) & mask64_0);
      exp64_1 = ((uint64_t)(0x3f) & mask64_1);
    } else {
      lld_diag_memtest_calc_exp_result(lld_diag_test_pattern_get(),
                                       i,
                                       n_entry,
                                       lld_diag_test_pattern_data0_get(),
                                       lld_diag_test_pattern_data1_get(),
                                       mask64_0,
                                       mask64_1,
                                       &exp64_0,
                                       &exp64_1);
    }

    if (is_tm_addr) {
      wb_buf64[3 * i + 0] = htole64(offset + i);
      wb_buf64[3 * i + 1] = htole64(exp64_1);
      wb_buf64[3 * i + 2] = htole64(exp64_0);
    } else {
      wb_buf64[2 * i + 0] = htole64(exp64_1);
      wb_buf64[2 * i + 1] = htole64(exp64_0);
    }
  }

  push_cnt = 0;
  do {
    if (is_tm_addr) {
      rc = lld_subdev_push_wl(dev_id,
                              subdev_id,
                              0,
                              entry_sz,
                              n_entry,
                              dma_addr_p,
                              (uint64_t)lld_ptr_to_u64(sm_p));
      if (rc != 0) {
        fprintf(reg_get_outstream(), "Error: %d : from lld_push_wl\n", rc);
        lld_memtest_write_list_error_set();
      }
      lld_dr_start(dev_id, subdev_id, dma_dr);
    } else {
      lld_register_completion_callback(dev_id,
                                       subdev_id,
                                       lld_dr_cmp_pipe_write_blk,
                                       lld_memtest_completion_callback_fn);

      if ((is_tofino && addr.tof.pipe_ring_addr_type == addr_type_register) ||
          (is_tofino2 && addr.tof2.pipe_ring_addr_type == addr_type_register) ||
          (is_tofino3 && addr.tof3.pipe_ring_addr_type == addr_type_register)) {
        rc = lld_subdev_push_wb(dev_id,
                                subdev_id,
                                entry_sz /*bytes ea entry*/,
                                4,
                                n_entry,
                                false /* single entry */,
                                dma_addr_p,
                                offset,
                                (uint64_t)lld_ptr_to_u64(sm_p));
      } else {  // memory
        rc = lld_subdev_push_wb(dev_id,
                                subdev_id,
                                entry_sz /*bytes ea entry*/,
                                1,
                                n_entry,
                                false /* single entry */,
                                dma_addr_p,
                                offset,
                                (uint64_t)lld_ptr_to_u64(sm_p));
      }
      lld_dr_start(dev_id, subdev_id, dma_dr);
    }
    push_cnt++;
    if (rc == LLD_ERR_DR_FULL) {
      fprintf(reg_get_outstream(),
              "DR Full: Servicing DR's to free them, push-cnt %d \n",
              push_cnt);
      /* Sleep for sometime */
      bf_sys_usleep(LLD_DIAG_MILLI_SEC(1));
      lld_dr_service(dev_id, subdev_id, dma_dr, 10000);
    }
  } while ((rc == LLD_ERR_DR_FULL) && (push_cnt < 1000));

  if (rc != 0) {
    fprintf(reg_get_outstream(), "Error: %d : from lld_push_wb\n", rc);
    lld_memtest_write_block_error_set();
  } else {
    lld_memtest_dma_msgs_sent_inc();
  }

  return status;
}

bf_status_t lld_mem_test_cb(bf_dev_id_t dev_id,
                            bf_subdev_id_t subdev_id,
                            uint64_t offset,
                            uint32_t len,
                            char *key_wd) {
  uint64_t mask64_0, mask64_1;
  uint64_t data64_0, data64_1;
  uint64_t exp64_0, exp64_1;
  int i, rc, n_entry = len / 16, stride = 1, consecutive_failures = 0;
  bool passed = true;
  bf_dev_pipe_t log_pipe = 0;
  uint32_t num_stages = 0;
  pipe_physical_addr_t addr;
  addr.addr = offset;
  bool is_tofino = lld_dev_is_tofino(dev_id);
  bool is_tofino2 = lld_dev_is_tof2(dev_id);
  bool is_tofino3 = lld_dev_is_tof3(dev_id);
  uint64_t write_offset = offset;
  bool is_wac_count = false;

  if (n_entry < 1) {
    // fprintf(reg_get_outstream(),"Mem test: %016"PRIx64" : %d bytes : %s --
    // skipped\n", offset,
    // len, key_wd );
    return BF_SUCCESS;
  }

  if ((is_tofino && 2 == ((offset >> 40) & 3)) ||
      (is_tofino2 && addr.tof2.pipe_always_1 == 1) ||
      (is_tofino3 && addr.tof3.pipe_always_1 == 1)) {
    bf_dev_pipe_t pipe = 0;
    int stage = 0;

    if (lld_diag_cfg.do_tm == 1) return BF_SUCCESS;  // only do TM mems

    if (is_tofino3) {
      if (strstr(key_wd, "e_prsr_mem")) return BF_SUCCESS;
    }

    if (is_tofino) {
      pipe = addr.tof.pipe_id_39_37;
      stage = addr.tof.pipe_element_36_33;
    } else if (is_tofino2) {
      pipe = addr.tof2.pipe_id;
      stage = addr.tof2.pipe_stage;
    } else if (is_tofino3) {
      pipe = addr.tof3.pipe_id;
      stage = addr.tof3.pipe_stage;
    }

    lld_sku_map_phy_pipe_id_to_pipe_id(dev_id, pipe, &log_pipe);
    if (!(lld_diag_cfg.pipe_mask &
          (1 << (log_pipe + subdev_id * BF_SUBDEV_PIPE_COUNT))))
      return BF_SUCCESS;

    /* Allow parde stages but filter MAU stages by the diag config. */
    lld_sku_get_num_active_mau_stages(dev_id, &num_stages, pipe);
    if (stage < (int)num_stages) {
      if (!(lld_diag_cfg.stage_mask & (1 << stage))) return BF_SUCCESS;
    }
  } else {
    /* TM addresses land here. */
    if (lld_diag_cfg.do_tm == 0) return BF_SUCCESS;  // only do pipe mems

    /* Some TM memories on Tofino2 are read-only, so skip them. */
    if (is_tofino2 || is_tofino3) {
      if (strstr(key_wd, "p_occ_mem")) return BF_SUCCESS;
      if (strstr(key_wd, "l1_occ_mem")) return BF_SUCCESS;
      if (strstr(key_wd, "l2_occ_mem")) return BF_SUCCESS;
      if (strstr(key_wd, "q_occ_mem")) return BF_SUCCESS;

      if (strstr(key_wd, "csr_memory_wac_")) {
        if (strstr(key_wd, "st")) return BF_SUCCESS;
        if (strstr(key_wd, "ppg_pfc")) return BF_SUCCESS;
        if (strstr(key_wd, "qid_map")) {
          // Tofino2 - 144 valid Index, Tofino3 - 288 valid Index
          n_entry = is_tofino2 ? LLD_DIAG_WAC_QID_MAP_TF2_ENTRIES
                               : LLD_DIAG_WAC_QID_MAP_TF3_ENTRIES;
        }
      }

      if (strstr(key_wd, "csr_memory_clc_clm")) return BF_SUCCESS;
      if (strstr(key_wd, "csr_memory_prc_cache")) return BF_SUCCESS;
      if (strstr(key_wd, "csr_memory_qlc_ht")) return BF_SUCCESS;
      if (strstr(key_wd, "csr_memory_qlc_qlm")) return BF_SUCCESS;

      // To test pex memory, we need to write into clc and
      // read from pex address
      if (strstr(key_wd, "csr_memory_pex")) {
        write_offset = offset - TM_PEX_CLC_OFFSET;
      }
    }

    if (is_tofino2) {
      if (strstr(key_wd, "csr_memory_wac_")) {
        if (strstr(key_wd, "count")) {
          is_wac_count = true;
        }
      }
    }

    if (is_tofino3) {
      uint32_t num_subdev = 0;
      lld_sku_get_num_subdev(dev_id, &num_subdev, NULL);
      if (num_subdev < 2) {
        if (strstr(key_wd, "qac_pipe_mem[4]")) return BF_SUCCESS;
        if (strstr(key_wd, "qac_pipe_mem[5]")) return BF_SUCCESS;
        if (strstr(key_wd, "qac_pipe_mem[6]")) return BF_SUCCESS;
        if (strstr(key_wd, "qac_pipe_mem[7]")) return BF_SUCCESS;

        if (strstr(key_wd, "prc__prc_mem[4]")) return BF_SUCCESS;
        if (strstr(key_wd, "prc__prc_mem[5]")) return BF_SUCCESS;
        if (strstr(key_wd, "prc__prc_mem[6]")) return BF_SUCCESS;
        if (strstr(key_wd, "prc__prc_mem[7]")) return BF_SUCCESS;

        if (strstr(key_wd, "qlc_mem[4]")) return BF_SUCCESS;
        if (strstr(key_wd, "qlc_mem[5]")) return BF_SUCCESS;
        if (strstr(key_wd, "qlc_mem[6]")) return BF_SUCCESS;
        if (strstr(key_wd, "qlc_mem[7]")) return BF_SUCCESS;

        if (strstr(key_wd, "pre_pipe_mem[4]")) return BF_SUCCESS;
        if (strstr(key_wd, "pre_pipe_mem[5]")) return BF_SUCCESS;
        if (strstr(key_wd, "pre_pipe_mem[6]")) return BF_SUCCESS;
        if (strstr(key_wd, "pre_pipe_mem[7]")) return BF_SUCCESS;

        // TODO: Should analyze the failure for TF3
        if (strstr(key_wd, "csr_memory_wac_")) {
          if (strstr(key_wd, "count")) {
            return BF_SUCCESS;
          }
        }
        // TODO: Failure seen when test for different pattern. Should anlayze
        if (strstr(key_wd, "csr_memory_qac_port_wm_cell_count"))
          return BF_SUCCESS;
      }

      // CLC CLM is valid only for 0 to 3
      if (strstr(key_wd, "pex__pex[4]__csr_memory_pex")) return BF_SUCCESS;
      if (strstr(key_wd, "pex__pex[5]__csr_memory_pex")) return BF_SUCCESS;
      if (strstr(key_wd, "pex__pex[6]__csr_memory_pex")) return BF_SUCCESS;
      if (strstr(key_wd, "pex__pex[7]__csr_memory_pex")) return BF_SUCCESS;
    }
  }

  fprintf(reg_get_outstream(),
          "Mem test: %016" PRIx64 " : %d entries : %d bytes : %s\n",
          offset,
          n_entry,
          len,
          key_wd);

  // try to automatically determine the valid bits in the register
  rc = lld_subdev_ind_write(dev_id,
                            subdev_id,
                            write_offset,
                            LLD_DIAG_ULONG_DATA_MASK,
                            LLD_DIAG_ULONG_DATA_MASK);
  if (rc != 0) {
    fprintf(reg_get_outstream(),
            "Error: %d <%xh> : determining mask (wr)\n",
            rc,
            rc);
    lld_memtest_write_error_set(offset);
    return rc;
  }
  rc = lld_subdev_ind_read(dev_id, subdev_id, offset, &mask64_0, &mask64_1);
  if (rc != 0) {
    fprintf(reg_get_outstream(),
            "Error: %d <%xh> : determining mask (rd)\n",
            rc,
            rc);
    lld_memtest_read_error_set(offset);
    return rc;
  }

  if (is_tofino && strstr(key_wd, "__po_action_row")) {
    /* Tofino-1 ingress/egress parser's PO Action memory has 255 bits of
     * accessable memory divided into two halves.  Even indexes (0,2,4...) have
     * 128 bits of data while odd indexes (1,3,5...) only have 127. */
    mask64_0 = mask64_0 >> 1;
  } else if (is_tofino2 && strstr(key_wd, "__po_action_row")) {
    /* Similar to the Tofino-1 case, but here four 128 bit memory word make 510
     * bits of writable data.  So here we mask off the top two bits. */
    mask64_0 = mask64_0 >> 2;
  }

  if (lld_diag_cfg.quick) {
    n_entry = 2;
    stride = (len / 16) - 1;
  }

  // now write the entry # to each entry, masked by the above
  for (i = 0; i < n_entry; i += stride) {
    lld_diag_memtest_calc_exp_result(lld_diag_test_pattern_get(),
                                     i,
                                     n_entry,
                                     lld_diag_test_pattern_data0_get(),
                                     lld_diag_test_pattern_data1_get(),
                                     mask64_0,
                                     mask64_1,
                                     &exp64_0,
                                     &exp64_1);

    data64_0 = exp64_0;
    data64_1 = exp64_1;
    rc = lld_subdev_ind_write(
        dev_id, subdev_id, write_offset + i, data64_0, data64_1);
    if (rc != 0) {
      fprintf(reg_get_outstream(), "Error: %d <%xh> : entry=%d\n", rc, rc, i);
      lld_memtest_write_error_set(offset + i);
      return rc;
    }
  }
  // readback each entry and verify the entry # matches (i.e. no aliasing)
  consecutive_failures = 0;
  for (i = 0; i < n_entry; i += stride) {
    lld_diag_memtest_calc_exp_result(lld_diag_test_pattern_get(),
                                     i,
                                     n_entry,
                                     lld_diag_test_pattern_data0_get(),
                                     lld_diag_test_pattern_data1_get(),
                                     mask64_0,
                                     mask64_1,
                                     &exp64_0,
                                     &exp64_1);
    rc = lld_subdev_ind_read(
        dev_id, subdev_id, offset + i, &data64_0, &data64_1);
    if (rc != 0) {
      fprintf(reg_get_outstream(), "Error: %d <%xh> : entry=%d\n", rc, rc, i);
      return rc;
    }

    // WAC DORP COUNTS read gives the double the value of written value.
    // This is as per "sf write-> broad cast wr both, rd is sum
    // so expect write value*2=read value"
    if (is_wac_count) {
      // This field is only 40 bits length. Shifting lower 64 by 1 is enough
      exp64_1 = exp64_1 << 1;
      exp64_1 = exp64_1 & mask64_1;
    }

    if ((data64_0 != exp64_0) || (data64_1 != exp64_1)) {
      passed = false;
      LOG_ERROR("Data Error: entry=%d : Exp: %016" PRIx64 "_%016" PRIx64
                " : Got: %016" PRIx64 "_%016" PRIx64 "\n",
                i,
                exp64_0,
                exp64_1,
                data64_0,
                data64_1);
      lld_memtest_data_error_set(
          offset + i, exp64_0, exp64_1, data64_0, data64_1, mask64_0, mask64_1);
      if (LLD_DIAG_MAX_ALLOWED_FAILURES < ++consecutive_failures) {
        break;
      }
    } else {
      consecutive_failures = 0;
    }
  }

  if (passed) {
    LOG_TRACE("Passed : %016" PRIx64 " : %s\n", offset, key_wd);
  } else {
    LOG_ERROR("FAILED : %016" PRIx64 " : %s\n", offset, key_wd);
  }
  if (!passed) {
    return BF_INVALID_ARG;
  }
  return BF_SUCCESS;
}

/* Restore any callbacks in database */
void lld_interrupt_test_restore_settings(bf_dev_id_t dev_id) {
  int idx = 0;

  for (idx = 0; idx < LLD_INT_TEST_CACHED_DATA_MAX; idx++) {
    if (!lld_int_test_cached_data[idx].valid) {
      continue;
    }
    lld_int_test_cached_data[idx].valid = false;
    /* If callback data contains valid info, restore it */
    if (!lld_int_test_cached_data[idx].poll) {
      lld_int_register_cb(dev_id,
                          lld_int_test_cached_data[idx].subdev_id,
                          lld_int_test_cached_data[idx].status_addr,
                          lld_int_test_cached_data[idx].cb_fn,
                          lld_int_test_cached_data[idx].userdata);
    }
    /* Clear status register */
    lld_subdev_write_register(dev_id,
                              lld_int_test_cached_data[idx].subdev_id,
                              lld_int_test_cached_data[idx].status_addr,
                              0xffffffff);

    /* Restore enable bits */
    lld_subdev_write_register(dev_id,
                              lld_int_test_cached_data[idx].subdev_id,
                              lld_int_test_cached_data[idx].enable_addr,
                              lld_int_test_cached_data[idx].enable_val);
    memset(
        &lld_int_test_cached_data[idx], 0, sizeof(lld_int_test_cached_data_t));
  }
}

/* Handle interrupt test callback */
uint32_t lld_interrupt_test_callback(bf_dev_id_t dev_id,
                                     bf_subdev_id_t subdev_id,
                                     uint32_t intr_address,
                                     uint32_t intr_status_val,
                                     uint32_t enable_hi_addr,
                                     uint32_t enable_lo_addr,
                                     void *userdata) {
  lld_int_test_cached_data_t *data = (lld_int_test_cached_data_t *)userdata;

  (void)enable_hi_addr;
  (void)enable_lo_addr;

  fprintf(reg_get_outstream(),
          "Interrupt-test callback received, Reg 0x%x, status-val 0x%x \n",
          intr_address,
          intr_status_val);

  if (!data) {
    fprintf(reg_get_outstream(),
            "Received callback with NULL callback data \n");
    return 0;
  }
  if (!data->valid) {
    fprintf(reg_get_outstream(),
            "Received callback with invalid callback data \n");
    return 0;
  }
  data->seen_cnt += 1;
  if (intr_status_val == 0) {
    char *path = get_full_reg_path_name(dev_id, intr_address);

    fprintf(reg_get_outstream(),
            "Received callback with invalid status of zero: %s \n",
            path ? path : "NONE");
    /* Increment rcvd count even though status is zero */
    lld_inttest_events_rcvd_inc();
    return 0;
  }
  /* Clear the interrupt */
  lld_subdev_write_register(dev_id, subdev_id, intr_address, intr_status_val);
  /* Increment received count */
  lld_inttest_events_rcvd_inc();

  if ((intr_status_val & data->exp_int_status_val) !=
      data->exp_int_status_val) {
    char reg_name[LLD_INT_RES_REG_NAME_LEN];
    char *path = get_full_reg_path_name(dev_id, intr_address);
    memset(reg_name, 0, sizeof(reg_name));
    snprintf(reg_name, LLD_INT_RES_REG_NAME_LEN, "%s", path ? path : "NONE");
    LOG_ERROR("All interrupts did not fire (msi) for %s, exp:0x%x, rcvd:0x%x",
              reg_name,
              data->exp_int_status_val,
              intr_status_val & data->exp_int_status_val);
    lld_inttest_error_set(reg_name,
                          intr_address,
                          data->exp_int_status_val,
                          intr_status_val & data->exp_int_status_val);
    lld_inttest_result_set(false);
  }

  return 0;
}

static void lld_diag_interrupt_enable_inject_poll(
    bf_dev_id_t dev_id, lld_int_test_cached_data_t *ent, uint32_t inject_addr) {
  bf_subdev_id_t subdev_id = ent->subdev_id;
  uint32_t status_addr = ent->status_addr;
  uint32_t status_data = ent->exp_int_status_val;
  uint32_t enable_addr = ent->enable_addr;
  bool poll_int = ent->poll;
  uint32_t rcvd_data = 0;

  /* Enable the interrupt */
  lld_subdev_write_register(dev_id, subdev_id, enable_addr, status_data);

  /* Inject the interrupt */
  lld_subdev_write_register(dev_id, subdev_id, inject_addr, status_data);

  if (!poll_int) {
    /* Wait for the callback to get called */
    lld_inttest_events_exp_inc();
  } else {
    /* Poll the interrupt status */
    lld_subdev_read_register(dev_id, subdev_id, status_addr, &rcvd_data);
    /* Clear the interrupt first */
    lld_subdev_write_register(dev_id, subdev_id, status_addr, rcvd_data);

    /* Compare expected value */
    if ((rcvd_data & status_data) != status_data) {
      char reg_name[LLD_INT_RES_REG_NAME_LEN];
      char *path = get_full_reg_path_name(dev_id, status_addr);
      memset(reg_name, 0, sizeof(reg_name));
      snprintf(reg_name, LLD_INT_RES_REG_NAME_LEN, "%s", path ? path : "NONE");
      LOG_ERROR("All interrupt did not fire (poll) for %s, exp:0x%x, rcvd:0x%x",
                reg_name,
                status_data,
                rcvd_data & status_data);
      lld_inttest_error_set(
          reg_name, status_addr, status_data, rcvd_data & status_data);
      lld_inttest_result_set(false);
    } else {
      ent->seen_cnt = 1;
    }
  }
}

void lld_diag_interrupt_disable_clear(bf_dev_id_t dev_id,
                                      bf_subdev_id_t subdev_id,
                                      uint32_t status_addr,
                                      uint32_t status_data,
                                      uint32_t enable_addr) {
  (void)status_data;
  /* Disable interrupt */
  lld_subdev_write_register(dev_id, subdev_id, enable_addr, 0);

  /* Clear the interrupt status */
  lld_subdev_write_register(dev_id, subdev_id, status_addr, 0xffffffff);
}

void lld_diag_interrupt_test_save_curr_settings(bf_dev_id_t dev_id,
                                                bf_subdev_id_t subdev_id,
                                                uint32_t status_addr,
                                                uint32_t status_val,
                                                uint32_t enable_addr,
                                                int cache_idx,
                                                bool poll) {
  void *userdata = NULL;

  /* Out of callback data, poll interrupt */
  if (cache_idx >= LLD_INT_TEST_CACHED_DATA_MAX) {
    return;
  }

  lld_int_test_cached_data[cache_idx].valid = true;
  lld_int_test_cached_data[cache_idx].poll = poll;
  lld_int_test_cached_data[cache_idx].subdev_id = subdev_id;
  lld_int_test_cached_data[cache_idx].status_addr = status_addr;
  lld_int_test_cached_data[cache_idx].exp_int_status_val = status_val;
  lld_int_test_cached_data[cache_idx].enable_addr = enable_addr;
  lld_int_test_cached_data[cache_idx].seen_cnt = 0;
  lld_subdev_read_register(dev_id,
                           subdev_id,
                           enable_addr,
                           &(lld_int_test_cached_data[cache_idx].enable_val));

  /* Disable and clear the interrupt */
  lld_diag_interrupt_disable_clear(
      dev_id, subdev_id, status_addr, status_val, enable_addr);

  /* Register callback if not polling */
  if (!poll) {
    /* Get existing callback function */
    lld_int_test_cached_data[cache_idx].cb_fn =
        lld_get_int_cb(dev_id, subdev_id, status_addr, &userdata);
    lld_int_test_cached_data[cache_idx].userdata = userdata;
    /* Register new callback */
    lld_int_register_cb(dev_id,
                        subdev_id,
                        status_addr,
                        lld_interrupt_test_callback,
                        &lld_int_test_cached_data[cache_idx]);
  }
}

static bf_status_t lld_diag_interrupt_tester(bf_dev_id_t dev_id,
                                             bf_subdev_id_t subdev_id,
                                             uint32_t status_addr,
                                             uint32_t status_data,
                                             uint32_t inject_addr,
                                             uint32_t enable_addr,
                                             bool poll_int) {
  bool poll = poll_int;
  int cache_idx = 0;

  /* Find an empty entry idx */
  for (cache_idx = 0; cache_idx < LLD_INT_TEST_CACHED_DATA_MAX; cache_idx++) {
    if (lld_int_test_cached_data[cache_idx].valid) {
      continue;
    }
    break;
  }
  /* Force poll if ot of callback data */
  if (cache_idx >= LLD_INT_TEST_CACHED_DATA_MAX) {
    return BF_INVALID_ARG;
  }

  /* Save current settings */
  lld_diag_interrupt_test_save_curr_settings(dev_id,
                                             subdev_id,
                                             status_addr,
                                             status_data,
                                             enable_addr,
                                             cache_idx,
                                             poll);

  /* Enable, Inject and poll the interrupt */
  lld_diag_interrupt_enable_inject_poll(
      dev_id, &lld_int_test_cached_data[cache_idx], inject_addr);

  return BF_SUCCESS;
}

bf_status_t lld_interrupt_test_quick(bf_dev_id_t dev_id, uint32_t pipe_bmp) {
  bf_subdev_id_t subdev_id;
  uint32_t sram_data = 0, tcam_data = 0, status_data = 0;
  uint32_t status_addr = 0, enable_addr = 0, inject_addr = 0;
  uint32_t idx = 0, row = 0, stage = 0, num_stages = 0;
  bf_dev_pipe_t pipe = 0;
  bf_dev_pipe_t phy_pipe = 0;
  uint32_t num_pipes = 0;
  bool do_poll_test = false;
  bool is_tofino = lld_dev_is_tofino(dev_id);
  bool is_tofino2 = lld_dev_is_tof2(dev_id);
  bool is_tofino3 = lld_dev_is_tof3(dev_id);

  lld_inttest_results_clear();
  lld_inttest_result_set(true);

  /* For quick test, test only sram and tcam ecc interrupts */
  /* 12 bits for sbe errors, 12 bits for mbe errors */
  for (idx = 0; idx < (2 * LLD_INTR_MAX_SRAM_COLS); idx++) {
    sram_data |= 0x1 << idx;
  }
  /* bits 4-15 for single bit error */
  for (idx = 0; idx < LLD_INTR_MAX_TCAM_ROWS; idx++) {
    tcam_data |= 0x1 << (4 + idx);
  }

  lld_sku_get_num_active_pipes(dev_id, &num_pipes);
  fprintf(reg_get_outstream(), "---Testing pbus interrupts--- \n");
  for (pipe = 0; pipe < num_pipes; pipe++) {
    if (!((1 << pipe) & pipe_bmp)) {
      continue;
    }
    lld_sku_map_pipe_id_to_phy_pipe_id(dev_id, pipe, &phy_pipe);
    phy_pipe = phy_pipe % BF_SUBDEV_PIPE_COUNT;
    lld_sku_get_num_active_mau_stages(dev_id, &num_stages, phy_pipe);
    subdev_id = pipe / BF_SUBDEV_PIPE_COUNT;
    for (stage = 0; stage < num_stages; stage++) {
      bf_sys_usleep(LLD_DIAG_MILLI_SEC(100));
      /* -- Test pipe-mgr tcam (pbus) interrupt --- */
      if (is_tofino) {
        status_addr = offsetof(
            Tofino,
            pipes[phy_pipe].mau[stage].tcams.intr_status_mau_tcam_array);
        enable_addr = offsetof(
            Tofino,
            pipes[phy_pipe].mau[stage].tcams.intr_enable0_mau_tcam_array);
        inject_addr = offsetof(
            Tofino,
            pipes[phy_pipe].mau[stage].tcams.intr_inject_mau_tcam_array);
      } else if (is_tofino2) {
        status_addr = offsetof(
            tof2_reg,
            pipes[phy_pipe].mau[stage].tcams.intr_status_mau_tcam_array);
        enable_addr = offsetof(
            tof2_reg,
            pipes[phy_pipe].mau[stage].tcams.intr_enable0_mau_tcam_array);
        inject_addr = offsetof(
            tof2_reg,
            pipes[phy_pipe].mau[stage].tcams.intr_inject_mau_tcam_array);
      } else if (is_tofino3) {
        status_addr = offsetof(
            tof3_reg,
            pipes[phy_pipe].mau[stage].tcams.intr_status_mau_tcam_array);
        enable_addr = offsetof(
            tof3_reg,
            pipes[phy_pipe].mau[stage].tcams.intr_enable0_mau_tcam_array);
        inject_addr = offsetof(
            tof3_reg,
            pipes[phy_pipe].mau[stage].tcams.intr_inject_mau_tcam_array);
      } else {
        return BF_INVALID_ARG;
      }
      lld_diag_interrupt_tester(dev_id,
                                subdev_id,
                                status_addr,
                                tcam_data,
                                inject_addr,
                                enable_addr,
                                do_poll_test);

      for (row = 0; row < LLD_INTR_MAX_SRAM_ROWS; row++) {
        bf_sys_usleep(LLD_DIAG_MILLI_SEC(1));
        /* -- Test pipe-mgr sram (pbus) interrupt --- */
        if (is_tofino) {
          status_addr = offsetof(Tofino,
                                 pipes[phy_pipe]
                                     .mau[stage]
                                     .rams.array.row[row]
                                     .intr_status_mau_unit_ram_row);
          enable_addr = offsetof(Tofino,
                                 pipes[phy_pipe]
                                     .mau[stage]
                                     .rams.array.row[row]
                                     .intr_enable0_mau_unit_ram_row);
          inject_addr = offsetof(Tofino,
                                 pipes[phy_pipe]
                                     .mau[stage]
                                     .rams.array.row[row]
                                     .intr_inject_mau_unit_ram_row);
        } else if (is_tofino2) {
          status_addr = offsetof(tof2_reg,
                                 pipes[phy_pipe]
                                     .mau[stage]
                                     .rams.array.row[row]
                                     .intr_status_mau_unit_ram_row);
          enable_addr = offsetof(tof2_reg,
                                 pipes[phy_pipe]
                                     .mau[stage]
                                     .rams.array.row[row]
                                     .intr_enable0_mau_unit_ram_row);
          inject_addr = offsetof(tof2_reg,
                                 pipes[phy_pipe]
                                     .mau[stage]
                                     .rams.array.row[row]
                                     .intr_inject_mau_unit_ram_row);
        } else if (is_tofino3) {
          status_addr = offsetof(tof3_reg,
                                 pipes[phy_pipe]
                                     .mau[stage]
                                     .rams.array.row[row]
                                     .intr_status_mau_unit_ram_row);
          enable_addr = offsetof(tof3_reg,
                                 pipes[phy_pipe]
                                     .mau[stage]
                                     .rams.array.row[row]
                                     .intr_enable0_mau_unit_ram_row);
          inject_addr = offsetof(tof3_reg,
                                 pipes[phy_pipe]
                                     .mau[stage]
                                     .rams.array.row[row]
                                     .intr_inject_mau_unit_ram_row);
        } else {
          return BF_INVALID_ARG;
        }
        lld_diag_interrupt_tester(dev_id,
                                  subdev_id,
                                  status_addr,
                                  sram_data,
                                  inject_addr,
                                  enable_addr,
                                  do_poll_test);
      }
    }
  }
  for (subdev_id = 0; subdev_id < BF_MAX_SUBDEV_COUNT; ++subdev_id) {
    /* Check if this subdevice was requested, that is whether any of the logical
     * pipes in pipe_bmp belong to it. */
    uint32_t pipe_per_subdev = (1u << BF_SUBDEV_PIPE_COUNT) - 1;
    if (!(pipe_bmp & (pipe_per_subdev << (subdev_id * BF_SUBDEV_PIPE_COUNT))))
      continue;
    bf_sys_usleep(LLD_DIAG_SEC(1));

    /* Test TM (cbus) interrupt */
    fprintf(reg_get_outstream(), "---Testing cbus interrupt--- \n");
    if (is_tofino) {
      status_addr =
          DEF_tofino_device_select_tm_top_tm_prc_top_prc_intr_status_address;
      status_data = 0x1;
      enable_addr =
          DEF_tofino_device_select_tm_top_tm_prc_top_prc_intr_enable0_address;
      inject_addr =
          DEF_tofino_device_select_tm_top_tm_prc_top_prc_intr_inject_address;
    } else if (is_tofino2) {
      status_addr =
          DEF_tof2_reg_device_select_tm_top_tm_prc_top_prc_intr_stat_address;
      status_data = 0x1;
      enable_addr =
          DEF_tof2_reg_device_select_tm_top_tm_prc_top_prc_intr_en0_address;
      inject_addr =
          DEF_tof2_reg_device_select_tm_top_tm_prc_top_prc_intr_inj_address;
    } else if (is_tofino3) {
      status_addr =
          DEF_tof3_reg_device_select_tm_top_tm_prc_top_prc_intr_stat_address;
      status_data = 0x1;
      enable_addr =
          DEF_tof3_reg_device_select_tm_top_tm_prc_top_prc_intr_en0_address;
      inject_addr =
          DEF_tof3_reg_device_select_tm_top_tm_prc_top_prc_intr_inj_address;
    } else {
      return BF_INVALID_ARG;
    }
    lld_diag_interrupt_tester(dev_id,
                              subdev_id,
                              status_addr,
                              status_data,
                              inject_addr,
                              enable_addr,
                              do_poll_test);
    bf_sys_usleep(LLD_DIAG_SEC(1));

    /* Cannot test Pkt-mgr (tbus) interrupt,
       will have to inject pkt to trigger interrupt.
    */

    /* Test port-mgr (mbus) interrupt */
    fprintf(reg_get_outstream(), "---Testing mbus interrupt--- \n");
    if (is_tofino) {
      status_addr = DEF_tofino_device_select_mbc_mbc_mbus_int_stat_address;
      status_data = 0x200;
      enable_addr = DEF_tofino_device_select_mbc_mbc_mbus_int_en_0_address;
      inject_addr = DEF_tofino_device_select_mbc_mbc_mbus_int_inj_address;
    } else if (is_tofino2) {
      status_addr = DEF_tof2_reg_eth400g_p1_eth400g_mac_mem_intr_stat_address;
      status_data = 0x1;
      enable_addr = DEF_tof2_reg_eth400g_p1_eth400g_mac_mem_intr_en0_address;
      inject_addr = DEF_tof2_reg_eth400g_p1_eth400g_mac_mem_intr_inj_address;
    } else if (is_tofino3) {
      status_addr = DEF_tof3_reg_eth400g_eth400g_app_mem_intr_stat_address;
      status_data = 0x80;
      enable_addr = DEF_tof3_reg_eth400g_eth400g_app_mem_intr_en0_address;
      inject_addr = DEF_tof3_reg_eth400g_eth400g_app_mem_intr_inj_address;
    } else {
      return BF_INVALID_ARG;
    }
    lld_diag_interrupt_tester(dev_id,
                              subdev_id,
                              status_addr,
                              status_data,
                              inject_addr,
                              enable_addr,
                              do_poll_test);
    bf_sys_usleep(LLD_DIAG_SEC(1));

    /* Test lld (host-if) interrupt */
    fprintf(reg_get_outstream(), "---Testing host-if interrupt--- \n");
    if (is_tofino) {
      status_addr =
          DEF_tofino_device_select_pcie_bar01_regs_pcie_regs_pcie_int_stat_address,
      status_data = 0x1000;
      enable_addr =
          DEF_tofino_device_select_pcie_bar01_regs_pcie_regs_pcie_int_en_address;
      inject_addr =
          DEF_tofino_device_select_pcie_bar01_regs_pcie_regs_pcie_int_inj_address;
    } else if (is_tofino2) {
      status_addr =
          DEF_tof2_reg_device_select_pcie_bar01_regs_pcie_intr_stat_address,
      status_data = 0x1000;
      enable_addr =
          DEF_tof2_reg_device_select_pcie_bar01_regs_pcie_intr_en0_address;
      inject_addr =
          DEF_tof2_reg_device_select_pcie_bar01_regs_pcie_intr_inj_address;
    } else if (is_tofino3) {
      status_addr =
          DEF_tof3_reg_device_select_pcie_bar01_regs_pcie_intr_stat_address,
      status_data = 0x1000;
      enable_addr =
          DEF_tof3_reg_device_select_pcie_bar01_regs_pcie_intr_en0_address;
      inject_addr =
          DEF_tof3_reg_device_select_pcie_bar01_regs_pcie_intr_inj_address;
    } else {
      return BF_INVALID_ARG;
    }
    lld_diag_interrupt_tester(dev_id,
                              subdev_id,
                              status_addr,
                              status_data,
                              inject_addr,
                              enable_addr,
                              do_poll_test);
    bf_sys_usleep(LLD_DIAG_SEC(1));
  }

  /* Sleep for 3 sec to allow interrupt callbacks to be processed */
  bf_sys_usleep(LLD_DIAG_SEC(3));

  /* Go over expected interrupts and display any with unexpected results. */
  for (idx = 0; idx < LLD_INT_TEST_CACHED_DATA_MAX; idx++) {
    if (!lld_int_test_cached_data[idx].valid) continue;
    if (lld_int_test_cached_data[idx].seen_cnt == 1) continue;
    fprintf(reg_get_outstream(),
            "Addr 0x%x, subdev %d, has count %d, expected count is 1\n",
            lld_int_test_cached_data[idx].status_addr,
            lld_int_test_cached_data[idx].subdev_id,
            lld_int_test_cached_data[idx].seen_cnt);
  }

  /* Restore callbacks and enable status */
  lld_interrupt_test_restore_settings(dev_id);

  return BF_SUCCESS;
}
