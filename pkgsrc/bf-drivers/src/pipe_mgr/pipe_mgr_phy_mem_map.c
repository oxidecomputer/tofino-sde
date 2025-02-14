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
 * @file pipe_mgr_phy_mem_map.c
 * @date
 *
 * Implementation of pipeline management physical memory shadow copy
 */

/* Standard header includes */
#include <math.h>
#include <unistd.h>
#include <dlfcn.h>
#ifdef __sun
#include <limits.h>
#endif

/* Module header files */
#include <dvm/bf_drv_intf.h>
#include <lld/bf_dma_if.h>
#include <pipe_mgr/pipe_mgr_intf.h>
#include "pipe_mgr_phy_mem_map.h"
#include "pipe_mgr_int.h"
#include "pipe_mgr_drv.h"

extern pipe_mgr_ctx_t *pipe_mgr_ctx;

/* ------------- Data structures ----------- */
typedef struct sram_mem_info_s {
  uint8_t data[PIPE_MGR_MAU_WORD_WIDTH];
} sram_mem_info_t;

typedef struct sram_map_s {
  sram_mem_info_t mem_info[TOF_SRAM_UNIT_DEPTH];
} sram_map_t;

typedef struct sram_info_s {
  bool symmetric;
  uint8_t pipe_list; /* logical pipe list */
} sram_info_t;
























typedef struct tcam_mem_info_s {
  uint8_t data[PIPE_MGR_MAU_WORD_WIDTH];
} tcam_mem_info_t;

typedef struct tcam_map_s {
  tcam_mem_info_t mem_info[TOF_TCAM_UNIT_DEPTH];
} tcam_map_t;

typedef struct tcam_info_s {
  bool symmetric;
  uint8_t pipe_list; /* logical pipe list */
} tcam_info_t;

/* Shadow memory mgmt */
typedef struct shadow_mem_s {
  /* sram blocks */
  sram_map_t **sram_map;
  /* tcam blocks */
  tcam_map_t **tcam_map;

  /* Info for sram mgmt */
  sram_info_t *sram_info;
  /* Info for tcam mgmt */
  tcam_info_t *tcam_info;








  /* Length of the sram_map/sram_info and tcam_map/tcam_info arrays. */
  unsigned int sram_cnt, tcam_cnt;
} shadow_mem_t;

static shadow_mem_t *shadow_mem[PIPE_MGR_NUM_DEVICES] = {NULL};

int pipe_mgr_phy_mem_map_debug = 0;

#define PHY_MEM_DBG \
  if (pipe_mgr_phy_mem_map_debug) printf

#define PIPE_MGR_MEM_TYPE_SRAM(mem_type) (mem_type == pipe_mem_type_unit_ram)

#define PIPE_MGR_MEM_TYPE_TCAM(mem_type) (mem_type == pipe_mem_type_tcam)

#define PIPE_MGR_MEM_TYPE_LAMB(mem_type) \


#define PIPE_MGR_MEM_TYPE_TO_WORD_WIDTH(mem_type)                         \
  ((PIPE_MGR_MEM_TYPE_SRAM(mem_type) || PIPE_MGR_MEM_TYPE_TCAM(mem_type)) \
       ? PIPE_MGR_MAU_WORD_WIDTH                                          \
       : PIPE_MGR_MAU_WORD_WIDTH)

/* Physical address field extraction */
static inline int addr_to_pipe(bf_dev_id_t dev, uint64_t address) {
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  PIPE_MGR_ASSERT(dev_info);
  return dev_info->dev_cfg.pipe_id_from_addr(address);
}
static inline int addr_to_stage(rmt_dev_info_t *dev_info, uint64_t address) {
  return dev_info->dev_cfg.stage_id_from_addr(address);
}
static inline int addr_to_mem_type(bf_dev_id_t dev, uint64_t address) {
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  PIPE_MGR_ASSERT(dev_info);
  return dev_info->dev_cfg.mem_type_from_addr(address);
}
static inline int addr_to_mem_offset(bf_dev_id_t dev, uint64_t address) {
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  PIPE_MGR_ASSERT(dev_info);
  return dev_info->dev_cfg.mem_addr_from_addr(address);
}
static inline uint64_t addr_set_pipe_id(bf_dev_id_t dev,
                                        uint64_t address,
                                        bf_dev_pipe_t pipe_id) {
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  PIPE_MGR_ASSERT(dev_info);
  return dev_info->dev_cfg.set_pipe_id_in_addr(address, pipe_id);
}














































static inline int sram_mem_map_arr_index(bf_dev_id_t dev, uint64_t address) {
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  PIPE_MGR_ASSERT(dev_info);
  rmt_dev_cfg_t *cfg = &dev_info->dev_cfg;
  return cfg->pipe_id_from_addr(address) * cfg->num_gress * cfg->num_stages *
             cfg->stage_cfg.num_sram_rows * cfg->stage_cfg.num_sram_cols +
         cfg->gress_from_addr(address) * cfg->num_stages *
             cfg->stage_cfg.num_sram_rows * cfg->stage_cfg.num_sram_cols +
         cfg->stage_id_from_addr(address) * cfg->stage_cfg.num_sram_rows *
             cfg->stage_cfg.num_sram_cols +
         cfg->row_from_addr(address) * cfg->stage_cfg.num_sram_cols +
         cfg->col_from_addr(address);
}

static inline int sram_mem_id_to_arr_index(rmt_dev_info_t *dev_info,
                                           bf_dev_pipe_t pipe_id,
                                           pipe_tbl_dir_t gress,
                                           dev_stage_t stage_id,
                                           int mem_id) {
  rmt_dev_cfg_t *cfg = &dev_info->dev_cfg;
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
    case BF_DEV_FAMILY_TOFINO2:
    case BF_DEV_FAMILY_TOFINO3:
      /*tf1,2,3 does not have memory per gress */
      gress = 0;
      break;


    default:
      return INT_MAX;
  }
  return pipe_id * cfg->num_gress * cfg->num_stages *
             cfg->stage_cfg.num_sram_rows * cfg->stage_cfg.num_sram_cols +
         gress * cfg->num_stages * cfg->stage_cfg.num_sram_rows *
             cfg->stage_cfg.num_sram_cols +
         stage_id * cfg->stage_cfg.num_sram_rows *
             cfg->stage_cfg.num_sram_cols +
         cfg->mem_id_to_row(mem_id, pipe_mem_type_unit_ram) *
             cfg->stage_cfg.num_sram_cols +
         cfg->mem_id_to_col(mem_id, pipe_mem_type_unit_ram);
}

static inline int tcam_mem_map_arr_index(bf_dev_id_t dev, uint64_t address) {
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  PIPE_MGR_ASSERT(dev_info);
  rmt_dev_cfg_t *cfg = &dev_info->dev_cfg;
  return cfg->pipe_id_from_addr(address) * cfg->num_stages *
             cfg->stage_cfg.num_tcam_rows * cfg->stage_cfg.num_tcam_cols +
         cfg->stage_id_from_addr(address) * cfg->stage_cfg.num_tcam_rows *
             cfg->stage_cfg.num_tcam_cols +
         cfg->row_from_addr(address) * cfg->stage_cfg.num_tcam_cols +
         cfg->col_from_addr(address);
}

static inline int tcam_mem_id_to_arr_index(rmt_dev_info_t *dev_info,
                                           bf_dev_pipe_t pipe_id,
                                           dev_stage_t stage_id,
                                           int mem_id) {
  rmt_dev_cfg_t *cfg = &dev_info->dev_cfg;
  return pipe_id * cfg->num_stages * cfg->stage_cfg.num_tcam_rows *
             cfg->stage_cfg.num_tcam_cols +
         stage_id * cfg->stage_cfg.num_tcam_rows *
             cfg->stage_cfg.num_tcam_cols +
         cfg->mem_id_to_row(mem_id, pipe_mem_type_tcam) *
             cfg->stage_cfg.num_tcam_cols +
         cfg->mem_id_to_col(mem_id, pipe_mem_type_tcam);
}

#define PIPE_MGR_SHADOW_PTR(dev) shadow_mem[dev]

#define PIPE_MGR_GET_SRAM(dev, index) \
  (PIPE_MGR_SHADOW_PTR(dev)->sram_map[index])

#define PIPE_MGR_GET_TCAM(dev, index) \
  (PIPE_MGR_SHADOW_PTR(dev)->tcam_map[index])

#define PIPE_MGR_SRAM_LINE(dev, index, offset) \
  (&(PIPE_MGR_SHADOW_PTR(dev)->sram_map[index]->mem_info[offset]))

#define PIPE_MGR_TCAM_LINE(dev, index, offset) \
  (&(PIPE_MGR_SHADOW_PTR(dev)->tcam_map[index]->mem_info[offset]))

#define PIPE_MGR_GET_SRAM_PIPE_LIST(dev, index) \
  PIPE_MGR_SHADOW_PTR(dev)->sram_info[index].pipe_list

#define PIPE_MGR_GET_TCAM_PIPE_LIST(dev, index) \
  PIPE_MGR_SHADOW_PTR(dev)->tcam_info[index].pipe_list

#define PIPE_MGR_GET_SRAM_SYM(dev, index) \
  PIPE_MGR_SHADOW_PTR(dev)->sram_info[index].symmetric

#define PIPE_MGR_GET_TCAM_SYM(dev, index) \
  PIPE_MGR_SHADOW_PTR(dev)->tcam_info[index].symmetric

#define PIPE_MGR_IS_SRAM_SYM(dev, index) \
  (PIPE_MGR_SHADOW_PTR(dev)->sram_info[index].symmetric == true)

#define PIPE_MGR_IS_TCAM_SYM(dev, index) \
  (PIPE_MGR_SHADOW_PTR(dev)->tcam_info[index].symmetric == true)
































/** Init the shadow memory data structs
 *
 * dev - Device target
 */
pipe_status_t pipe_mgr_phy_mem_map_init(bf_dev_id_t dev) {
  pipe_status_t status = PIPE_SUCCESS;

  if (shadow_mem[dev]) {
    return PIPE_SUCCESS;
  }
  shadow_mem[dev] = PIPE_MGR_CALLOC(1, sizeof(shadow_mem_t));
  if (!shadow_mem[dev]) return PIPE_NO_SYS_RESOURCES;
  shadow_mem_t *shdw = shadow_mem[dev];

  /* Get the number of memories on the chip from the dev info. */
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) {
    LOG_ERROR("%s:%d : Could not get device info for device %d",
              __func__,
              __LINE__,
              dev);
    return PIPE_OBJ_NOT_FOUND;
  }

  rmt_dev_cfg_t *cfg = &dev_info->dev_cfg;
  if (!cfg) {
    LOG_ERROR("%s:%d : Could not get device config for device %d",
              __func__,
              __LINE__,
              dev);
    return PIPE_UNEXPECTED;
  }
  shdw->sram_cnt = dev_info->num_active_pipes * cfg->num_gress *
                   cfg->num_stages * cfg->stage_cfg.num_sram_rows *
                   cfg->stage_cfg.num_sram_cols;
  shdw->tcam_cnt = dev_info->num_active_pipes * cfg->num_stages *
                   cfg->stage_cfg.num_tcam_rows * cfg->stage_cfg.num_tcam_cols;





















  /* Allocate memory for the shadows. */
  shdw->sram_map = PIPE_MGR_CALLOC(shdw->sram_cnt, sizeof(sram_map_t *));
  shdw->tcam_map = PIPE_MGR_CALLOC(shdw->tcam_cnt, sizeof(tcam_map_t *));
  shdw->sram_info = PIPE_MGR_CALLOC(shdw->sram_cnt, sizeof(sram_info_t));
  shdw->tcam_info = PIPE_MGR_CALLOC(shdw->tcam_cnt, sizeof(tcam_info_t));
  if (!shdw->sram_map || !shdw->tcam_map || !shdw->sram_info ||
      !shdw->tcam_info) {
    pipe_mgr_phy_mem_map_cleanup(dev);
    return PIPE_NO_SYS_RESOURCES;
  }

  return status;
}

/** Cleanup of shadow memory map
 *
 * dev - Device id
 */
pipe_status_t pipe_mgr_phy_mem_map_cleanup(bf_dev_id_t dev) {
  unsigned int i;

  if (!shadow_mem[dev]) {
    return PIPE_SUCCESS;
  }
  shadow_mem_t *shdw = shadow_mem[dev];
  shadow_mem[dev] = NULL;

  if (shdw->sram_map) {
    for (i = 0; i < shdw->sram_cnt; ++i)
      if (shdw->sram_map[i]) PIPE_MGR_FREE(shdw->sram_map[i]);
    PIPE_MGR_FREE(shdw->sram_map);
  }
  if (shdw->tcam_map) {
    for (i = 0; i < shdw->tcam_cnt; ++i)
      if (shdw->tcam_map[i]) PIPE_MGR_FREE(shdw->tcam_map[i]);
    PIPE_MGR_FREE(shdw->tcam_map);
  }














  if (shdw->sram_info) PIPE_MGR_FREE(shdw->sram_info);
  if (shdw->tcam_info) PIPE_MGR_FREE(shdw->tcam_info);

  PIPE_MGR_FREE(shdw);

  return PIPE_SUCCESS;
}

/* For symmetric tables, get the lowest pipe-id where the table is */
static inline bf_dev_pipe_t pipe_mgr_get_sym_lowest_pipe_id(
    bf_dev_id_t dev, int index, pipe_mem_type_t mem_type) {
  bf_dev_pipe_t i = 0;
  uint8_t pipe_list = 0;
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) {
    LOG_ERROR("%s:%d : Could not get device info for device %d",
              __func__,
              __LINE__,
              dev);
    return PIPE_OBJ_NOT_FOUND;
  }

  switch (mem_type) {
    case pipe_mem_type_unit_ram:
      PIPE_MGR_DBGCHK(PIPE_MGR_IS_SRAM_SYM(dev, index));
      pipe_list = PIPE_MGR_GET_SRAM_PIPE_LIST(dev, index);
      break;
    case pipe_mem_type_tcam:
      PIPE_MGR_DBGCHK(PIPE_MGR_IS_TCAM_SYM(dev, index));
      pipe_list = PIPE_MGR_GET_TCAM_PIPE_LIST(dev, index);
      break;










    default:
      LOG_ERROR("%s:%d : Unknown mem type %d on dev %d",
                __func__,
                __LINE__,
                mem_type,
                dev);
      PIPE_MGR_DBGCHK(0);
  }

  // rmt_dev_cfg_t *cfg = &pipe_mgr_get_dev_info(dev)->dev_cfg;
  for (i = 0; i < dev_info->num_active_pipes; i++) {
    if (pipe_list & (1 << i)) {
      return i;
    }
  }
  /* If symmetric and no pipe was found, log error and return 0 */
  PIPE_MGR_DBGCHK(0);

  return 0;
}

/* For symmetric tables, get the array index where the data
   is stored
*/
static inline int pipe_mgr_get_sym_arr_index(bf_dev_id_t dev,
                                             uint8_t gress,
                                             dev_stage_t stage_id,
                                             uint64_t address,
                                             int arr_index,
                                             pipe_mem_type_t mem_type,
                                             uint64_t *new_address) {
  bf_dev_pipe_t lowest_pipe = 0;
  int new_arr_index = arr_index;
  (void)gress;
  (void)stage_id;
  *new_address = address;
  lowest_pipe = pipe_mgr_get_sym_lowest_pipe_id(dev, arr_index, mem_type);

  if ((unsigned int)addr_to_pipe(dev, address) != lowest_pipe) {
    *new_address = addr_set_pipe_id(dev, address, lowest_pipe);
    switch (mem_type) {
      case pipe_mem_type_unit_ram:
        new_arr_index = sram_mem_map_arr_index(dev, *new_address);
        break;
      case pipe_mem_type_tcam:
        new_arr_index = tcam_mem_map_arr_index(dev, *new_address);
        break;









      default:
        PIPE_MGR_DBGCHK(0);
    }
  }

  return new_arr_index;
}

/** Convert the array index to physical memory
 *
 * index   - index in array
 * mem_type - sram/tcam
 */
static inline uint64_t pipe_mgr_convert_arr_index_to_phy_addr(
    rmt_dev_info_t *dev_info, int arr_index, pipe_mem_type_t mem_type) {
  pipe_status_t status = PIPE_SUCCESS;
  uint64_t phy_addr = 0;
  bf_dev_pipe_t pipe_id = 0;
  bf_dev_pipe_t phy_pipe_id = 0;
  dev_stage_t stage_id = 0;
  uint8_t row = 0, col = 0;
  uint32_t mem_offset = 0;
  uint32_t mem_id = 0;
  int index = arr_index;
  pipe_tbl_dir_t gress = 0;
  /* Convert the logical pipe id to physical pipe id */
  status = pipe_mgr_map_pipe_id_log_to_phy(dev_info, pipe_id, &phy_pipe_id);
  if (status != PIPE_SUCCESS) {
    LOG_ERROR(
        "%s:%d Error in convering logical pipe id %d to phy pipe id"
        " for dev id %d, err %s",
        __func__,
        __LINE__,
        pipe_id,
        dev_info->dev_id,
        pipe_str_err(status));
    PIPE_MGR_DBGCHK(0);
    return ~0;
  }

  /* In some cases gress is counted in other it is not.
   * gress_enable = false means for that particular case gress should
   * not be counted. */
  bool gress_enable = false;
  uint32_t num_rows, num_cols, num_units_per_stage;
  uint32_t num_stages = dev_info->num_active_mau;
  switch (mem_type) {
    case pipe_mem_type_unit_ram:
      num_rows = dev_info->dev_cfg.stage_cfg.num_sram_rows;
      num_cols = dev_info->dev_cfg.stage_cfg.num_sram_cols;
      num_units_per_stage = num_rows * num_cols;

      break;
    case pipe_mem_type_tcam:
      num_rows = dev_info->dev_cfg.stage_cfg.num_tcam_rows;
      num_cols = dev_info->dev_cfg.stage_cfg.num_tcam_cols;
      num_units_per_stage = num_rows * num_cols;
      break;












    default:
      LOG_ERROR("Invalid mem_type %d \n", mem_type);
      PIPE_MGR_DBGCHK(0);
      return ~0;
  }

  pipe_id = index / (num_stages * num_units_per_stage);
  index -= pipe_id * (num_stages * num_units_per_stage);

  if (gress_enable) {
    gress = index / (num_stages * num_units_per_stage);
    index -= gress * (num_stages * num_units_per_stage);
  }

  stage_id = index / (num_units_per_stage);
  index -= stage_id * (num_units_per_stage);

  row = index / num_cols;
  index -= row * num_cols;

  col = index;

  mem_id = dev_info->dev_cfg.mem_id_from_col_row(stage_id, col, row, mem_type);

  /* Offset of 0, as we want start of tcam block */
  phy_addr = dev_info->dev_cfg.get_full_phy_addr(
      gress, phy_pipe_id, stage_id, mem_id, mem_offset, mem_type);
  PHY_MEM_DBG("%s Arr decode: addr %" PRIx64
              ", pipe %d, stage %d, row %d, col %d,"
              " off %d, arr_idx %d \n",
              mem_type_to_str(mem_type),
              phy_addr,
              pipe_id,
              stage_id,
              row,
              col,
              mem_offset,
              arr_index);

  return phy_addr;
}

/** Validite supplied physical address
 *
 * address - Physical address
 */
static inline bool pipe_mgr_phy_mem_map_addr_valid(rmt_dev_info_t *dev_info,
                                                   uint64_t address) {
  rmt_dev_cfg_t *cfg = &dev_info->dev_cfg;

  if (!cfg->is_pipe_addr(address)) return false;

  if (addr_type_memdata != cfg->addr_type_from_addr(address)) return false;

  bf_dev_pipe_t phy_pipe = cfg->pipe_id_from_addr(address);
  if (phy_pipe >= cfg->num_pipelines) return false;

  bf_dev_pipe_t log_pipe = phy_pipe;
  pipe_mgr_map_phy_pipe_id_to_log_pipe_id_optimized(
      dev_info, phy_pipe, &log_pipe);
  if (log_pipe >= dev_info->num_active_pipes) return false;

  dev_stage_t stage_id = cfg->stage_id_from_addr(address);
  if (stage_id >= dev_info->num_active_mau) return false;

  pipe_mem_type_t mem_type = cfg->mem_type_from_addr(address);
  mem_id_t mem_id = cfg->mem_id_from_addr(address);
  uint8_t row = cfg->mem_id_to_row(mem_id, mem_type);
  uint8_t col = cfg->mem_id_to_col(mem_id, mem_type);
  uint16_t off = cfg->mem_addr_from_addr(address);
  switch (mem_type) {
    case pipe_mem_type_unit_ram:
      if (!cfg->sram_row_valid(row)) return false;
      if (!cfg->sram_col_valid(col)) return false;
      if (off >= cfg->stage_cfg.sram_unit_depth) return false;
      break;
    case pipe_mem_type_tcam:
      if (!cfg->tcam_row_valid(row)) return false;
      if (!cfg->tcam_col_valid(col)) return false;
      if (off >= cfg->stage_cfg.tcam_unit_depth) return false;
      break;














    default:
      return false;
  }
  return true;
}

/** Set a sram/tcam as symmetric for all pipes
 *
 * dev     - Device target
 * address - physical addr
 * sym    - Symmetric/Unsymmetric
 */
static void pipe_mgr_phy_mem_map_symmetric_helper(rmt_dev_info_t *dev_info,
                                                  uint8_t gress,
                                                  bf_dev_pipe_t log_pipe_id,
                                                  dev_stage_t stage_id,
                                                  pipe_mem_type_t mem_type,
                                                  mem_id_t mem_id,
                                                  bool symmetric,
                                                  uint8_t pipe_list) {
  uint32_t arr_index = 0;
  (void)gress;
  switch (mem_type) {
    case pipe_mem_type_unit_ram:
      arr_index = sram_mem_id_to_arr_index(
          dev_info, log_pipe_id, gress, stage_id, mem_id);
      PIPE_MGR_GET_SRAM_SYM(dev_info->dev_id, arr_index) = symmetric;
      PIPE_MGR_GET_SRAM_PIPE_LIST(dev_info->dev_id, arr_index) = pipe_list;
      break;
    case pipe_mem_type_tcam:
      arr_index =
          tcam_mem_id_to_arr_index(dev_info, log_pipe_id, stage_id, mem_id);
      PIPE_MGR_GET_TCAM_SYM(dev_info->dev_id, arr_index) = symmetric;
      PIPE_MGR_GET_TCAM_PIPE_LIST(dev_info->dev_id, arr_index) = pipe_list;
      break;














    default:
      PIPE_MGR_DBGCHK(0);
  }
}

/** Physical memory map data reference pointer
    Get pointer to data in shadow memory
  *
  * dev     - Device target
  * pipe_bmp - list of pipes
  * stage_id - Stage
  * lower_phy_addr - lower 32 bit of physical addr
  * data_ref  - Data pointer (out)
  */
pipe_status_t pipe_mgr_phy_mem_map_get_ref(bf_dev_id_t dev,
                                           pipe_tbl_dir_t gress,
                                           uint8_t mem_type,
                                           bf_dev_pipe_t pipe_id,
                                           dev_stage_t stage_id,
                                           mem_id_t mem_id,
                                           uint32_t line_num,
                                           uint8_t **data_ref,
                                           bool read_only) {
  pipe_status_t status = PIPE_SUCCESS;
  int arr_index;
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return PIPE_INVALID_ARG;
  (void)gress;
  switch (mem_type) {
    case pipe_mem_type_unit_ram: {
      sram_mem_info_t *sram_line;
      arr_index =
          sram_mem_id_to_arr_index(dev_info, pipe_id, gress, stage_id, mem_id);
      PHY_MEM_DBG(
          "SRAM get by ref: addr dev %d, pipe_id %d, stage id %d arr_idx %d \n",
          dev,
          pipe_id,
          stage_id,
          arr_index);

      if (!PIPE_MGR_GET_SRAM(dev, arr_index)) {
        if (read_only) {
          return PIPE_SUCCESS;
        }
        PIPE_MGR_GET_SRAM(dev, arr_index) =
            PIPE_MGR_CALLOC(1, sizeof(sram_map_t));
        if (!PIPE_MGR_GET_SRAM(dev, arr_index)) {
          LOG_ERROR("Malloc failure in sram blk");
          return PIPE_NO_SYS_RESOURCES;
        }
      }
      sram_line = PIPE_MGR_SRAM_LINE(dev, arr_index, line_num);
      *data_ref = (uint8_t *)sram_line;
      break;
    }
    case pipe_mem_type_tcam: {
      tcam_mem_info_t *tcam_line;

      arr_index = tcam_mem_id_to_arr_index(dev_info, pipe_id, stage_id, mem_id);
      PHY_MEM_DBG(
          "TCAM get by ref: dev %d, pipe id %d, stage id %d, arr_idx %d \n",
          dev,
          pipe_id,
          stage_id,
          arr_index);

      if (!PIPE_MGR_GET_TCAM(dev, arr_index)) {
        PIPE_MGR_GET_TCAM(dev, arr_index) =
            PIPE_MGR_CALLOC(1, sizeof(tcam_map_t));
        if (!PIPE_MGR_GET_TCAM(dev, arr_index)) {
          LOG_ERROR("Malloc failure in tcam blk");
          return PIPE_NO_SYS_RESOURCES;
        }
        tcam_map_t *tcam_map = PIPE_MGR_GET_TCAM(dev, arr_index);
        int depth = 0;
        // Set the MRD Bit to 1 for power saving
        for (depth = 0; depth < TOF_TCAM_UNIT_DEPTH; depth++) {
          tcam_map->mem_info[depth].data[0] = 0x1;
        }
      }
      tcam_line = PIPE_MGR_TCAM_LINE(dev, arr_index, line_num);
      *data_ref = (uint8_t *)tcam_line;
      break;
    }
























































    default:
      return PIPE_INVALID_ARG;
  }

  return status;
}

/** Physical memory map write
    Add the addr/value pair to data structs
  *
  * sess_hdl - Session handle
  * dev     - Device target
  * address - physical addr
  * data    - Data
  * mask    - mask
  */
pipe_status_t pipe_mgr_phy_mem_map_write_helper(bf_dev_id_t dev,
                                                pipe_tbl_dir_t gress,
                                                bf_dev_pipe_t pipe_id,
                                                dev_stage_t stage_id,
                                                pipe_mem_type_t mem_type,
                                                mem_id_t mem_id,
                                                uint32_t line_num,
                                                uint8_t *data,
                                                uint8_t *mask) {
  pipe_status_t status = PIPE_SUCCESS;
  int i = 0;
  int data_len = 0;
  uint8_t *data_ptr = NULL;

  if (!pipe_mgr_valid_deviceId(dev, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }

  if (!PIPE_MGR_SHADOW_PTR(dev)) {
    LOG_ERROR("Shadow ptr for dev %d is null ", dev);
    return PIPE_INVALID_ARG;
  }
  data_len = PIPE_MGR_MEM_TYPE_TO_WORD_WIDTH(mem_type);

  PHY_MEM_DBG(
      "mem write: dev %d, pipe %d, stage %d, mem_id %d, off %d, mem_type %d \n",
      dev,
      pipe_id,
      stage_id,
      mem_id,
      line_num,
      mem_type);

  status = pipe_mgr_phy_mem_map_get_ref(dev,
                                        gress,
                                        mem_type,
                                        pipe_id,
                                        stage_id,
                                        mem_id,
                                        line_num,
                                        &data_ptr,
                                        false);
  if (status != PIPE_SUCCESS) {
    LOG_ERROR(
        "Failed to get ref ptr for mem id %d, pipe id %d, stage id %d"
        " line_num %d, dev %d ",
        mem_id,
        pipe_id,
        stage_id,
        line_num,
        dev);
    return status;
  }

  if (!mask) {
    PIPE_MGR_MEMCPY(data_ptr, data, data_len);
  } else {
    for (i = 0; i < data_len; i++) {
      data_ptr[i] = (data[i] & mask[i]) | (data_ptr[i] & ~mask[i]);
    }
  }

  return status;
}

/** Physical memory map write with list of pipes
    Add the addr/value pair to data structs
  *
  * sess_hdl - Session handle
  * dev     - Device target
  * pipe_bmp - list of pipes
  * stage_id - Stage
  * lower_phy_addr - lower 32 bit of physical addr
  * data    - Data
  * mask    - mask
  */
pipe_status_t pipe_mgr_phy_mem_map_write(bf_dev_id_t dev,
                                         pipe_tbl_dir_t gress,
                                         bf_dev_pipe_t pipe_id,
                                         dev_stage_t stage_id,
                                         pipe_mem_type_t mem_type,
                                         mem_id_t mem_id,
                                         uint32_t line_num,
                                         uint8_t *data,
                                         uint8_t *mask) {
  pipe_status_t status = PIPE_SUCCESS;

  if (!pipe_mgr_valid_deviceId(dev, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }
  status |= pipe_mgr_phy_mem_map_write_helper(
      dev, gress, pipe_id, stage_id, mem_type, mem_id, line_num, data, mask);
  return status;
}

/** Read data value of SRAM/TCAM at a particular address
 *
 * address - Address
 * data    - Data
 * data_len - Data len
 */
static pipe_status_t pipe_mgr_phy_mem_map_read_helper(rmt_dev_info_t *dev_info,
                                                      pipe_tbl_dir_t gress,
                                                      bf_dev_pipe_t pipe_id,
                                                      dev_stage_t stage_id,
                                                      pipe_mem_type_t mem_type,
                                                      mem_id_t mem_id,
                                                      uint32_t line_num,
                                                      uint8_t *data,
                                                      int data_len) {
  pipe_status_t status = PIPE_SUCCESS;
  bf_dev_id_t dev = dev_info->dev_id;
  int arr_index = 0;
  uint8_t *data_ptr = NULL;
  bf_dev_pipe_t lowest_pipe = pipe_id;

  if (!PIPE_MGR_SHADOW_PTR(dev)) {
    LOG_ERROR("Shadow ptr for dev %d is null ", dev);
    return PIPE_INVALID_ARG;
  }
  PIPE_MGR_DBGCHK(data_len >= PIPE_MGR_MEM_TYPE_TO_WORD_WIDTH(mem_type));
  PIPE_MGR_MEMSET(data, 0, PIPE_MGR_MEM_TYPE_TO_WORD_WIDTH(mem_type));

  switch (mem_type) {
    case pipe_mem_type_unit_ram:
      arr_index =
          sram_mem_id_to_arr_index(dev_info, pipe_id, gress, stage_id, mem_id);
      if (PIPE_MGR_IS_SRAM_SYM(dev, arr_index)) {
        lowest_pipe = pipe_mgr_get_sym_lowest_pipe_id(dev, arr_index, mem_type);
      }
      break;
    case pipe_mem_type_tcam:
      arr_index = tcam_mem_id_to_arr_index(dev_info, pipe_id, stage_id, mem_id);
      if (PIPE_MGR_IS_TCAM_SYM(dev, arr_index)) {
        lowest_pipe = pipe_mgr_get_sym_lowest_pipe_id(dev, arr_index, mem_type);
      }
      break;















    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }

  PHY_MEM_DBG(
      "mem read: , dev %d, pipe %d, stage %d,  mem_id %d, off %d"
      " mem_type %d \n",
      dev,
      lowest_pipe,
      stage_id,
      mem_id,
      line_num,
      mem_type);

  status = pipe_mgr_phy_mem_map_get_ref(dev,
                                        gress,
                                        mem_type,
                                        lowest_pipe,
                                        stage_id,
                                        mem_id,
                                        line_num,
                                        &data_ptr,
                                        true);

  if (status != PIPE_SUCCESS) {
    LOG_ERROR(
        "Failed to get ref ptr for dev %d pipe id %d, stage id %d "
        " mem_id %d, line num %d",
        dev,
        pipe_id,
        stage_id,
        mem_id,
        line_num);
    return status;
  }

  /* If pointer is null, data was never written to that address
     and assume default value of zero
  */
  if (data_ptr) {
    PIPE_MGR_MEMCPY(data, data_ptr, PIPE_MGR_MEM_TYPE_TO_WORD_WIDTH(mem_type));
  }

  return status;
}

/** Read data value of SRAM/TCAM at a particular address
 *
 * dev     - Device target
 * pipe_bmp - list of pipes
 * stage_id - Stage
 * lower_phy_addr - lower 32 bit of physical addr
 * data    - Data
 * data_len - length of data allocated
 */
pipe_status_t pipe_mgr_phy_mem_map_read(bf_dev_id_t dev,
                                        pipe_tbl_dir_t gress,
                                        bf_dev_pipe_t pipe_id,
                                        dev_stage_t stage_id,
                                        pipe_mem_type_t mem_type,
                                        mem_id_t mem_id,
                                        uint32_t line_num,
                                        uint8_t *data,
                                        int data_len) {
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return PIPE_INVALID_ARG;

  return pipe_mgr_phy_mem_map_read_helper(dev_info,
                                          gress,
                                          pipe_id,
                                          stage_id,
                                          mem_type,
                                          mem_id,
                                          line_num,
                                          data,
                                          data_len);
}

/** Copy value from one location to other on list of pipes
 *
 * sess_hdl - Session handle
 * dev     - Device target
 * pipe_bmp - list of pipes
 * src_stage_id - Stage of src
 * dst_stage_id - Stage of dst
 * src_address - lower 32 bit of Src Address
 * dst_address - lower 32 bit of Dest Address
 * invalidate_src - Invalidate value at src
 */
pipe_status_t pipe_mgr_phy_mem_map_copy(bf_dev_id_t dev,
                                        pipe_tbl_dir_t gress,
                                        bf_dev_pipe_t pipe_id,
                                        dev_stage_t src_stage_id,
                                        dev_stage_t dst_stage_id,
                                        uint32_t lower_src_addr,
                                        uint32_t lower_dst_addr,
                                        bool invalidate_src) {
  pipe_status_t status = PIPE_SUCCESS;
  uint8_t data[PIPE_MGR_MAU_WORD_WIDTH];
  mem_id_t src_mem_id, dst_mem_id;
  pipe_mem_type_t src_mem_type, dst_mem_type;
  uint32_t src_line_num, dst_line_num;

  if (!pipe_mgr_valid_deviceId(dev, __func__, __LINE__)) {
    return PIPE_INVALID_ARG;
  }
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return PIPE_INVALID_ARG;

  rmt_dev_cfg_t *cfg = &dev_info->dev_cfg;
  src_mem_id = cfg->mem_id_from_addr(lower_src_addr);
  src_mem_type = addr_to_mem_type(dev, lower_src_addr);
  src_line_num = addr_to_mem_offset(dev, lower_src_addr);

  dst_mem_id = cfg->mem_id_from_addr(lower_dst_addr);
  dst_mem_type = addr_to_mem_type(dev, lower_dst_addr);
  dst_line_num = addr_to_mem_offset(dev, lower_dst_addr);

  /* Read from src and write to dst */
  PIPE_MGR_MEMSET(&data, 0, sizeof(data));
  status |= pipe_mgr_phy_mem_map_read_helper(dev_info,
                                             gress,
                                             pipe_id,
                                             src_stage_id,
                                             src_mem_type,
                                             src_mem_id,
                                             src_line_num,
                                             &data[0],
                                             PIPE_MGR_MAU_WORD_WIDTH);

  status |= pipe_mgr_phy_mem_map_write_helper(dev,
                                              gress,
                                              pipe_id,
                                              dst_stage_id,
                                              dst_mem_type,
                                              dst_mem_id,
                                              dst_line_num,
                                              &data[0],
                                              NULL);

  /* Write zeroes to invalidate */
  if (invalidate_src) {
    PIPE_MGR_MEMSET(&data, 0, sizeof(data));
    status |= pipe_mgr_phy_mem_map_write_helper(dev,
                                                gress,
                                                pipe_id,
                                                src_stage_id,
                                                src_mem_type,
                                                src_mem_id,
                                                src_line_num,
                                                &data[0],
                                                NULL);
  }

  return status;
}

/** Set the symmetric mode for a memory block
 *
 * dev     - Device target
 * pipe_bmp - list of all pipes in profile
 * stage_id - Stage
 * lower_phy_addr - Address
 * symmetric - symmetric mode of table
 */
pipe_status_t pipe_mgr_phy_mem_map_symmetric_mode_set(
    bf_dev_id_t dev,
    pipe_tbl_dir_t gress,
    pipe_bitmap_t *log_pipe_bmp,
    dev_stage_t stage_id,
    pipe_mem_type_t mem_type,
    mem_id_t mem_id,
    bool symmetric) {
  pipe_status_t status = PIPE_SUCCESS;
  bf_dev_pipe_t pipe;
  uint64_t address = 0;
  int arr_index = 0;
  uint8_t pipe_list = 0;
  uint8_t local_pipe_list = 0;
  int pipe_cnt = 0;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return PIPE_INVALID_ARG;

  PIPE_BITMAP_ITER(log_pipe_bmp, pipe) { pipe_list |= (1 << pipe); }

  for (pipe = 0; pipe < dev_info->num_active_pipes; pipe++) {
    if (!(pipe_list & (1 << pipe))) {
      continue;
    }

    local_pipe_list = pipe_list;
    pipe_cnt++;
    /* Set the supplied symmetric mode */
    pipe_mgr_phy_mem_map_symmetric_helper(dev_info,
                                          gress,
                                          pipe,
                                          stage_id,
                                          mem_type,
                                          mem_id,
                                          symmetric,
                                          local_pipe_list);

    /* going from non-sym to sym, free blocks */
    if (symmetric && (pipe_cnt > 1)) {
      switch (mem_type) {
        case pipe_mem_type_unit_ram: {
          sram_map_t *sram;

          arr_index =
              sram_mem_id_to_arr_index(dev_info, pipe, gress, stage_id, mem_id);
          PHY_MEM_DBG("SRAM blk invalidate: addr 0x%" PRIx64
                      ", dev %d, arr_idx %d \n",
                      address,
                      dev,
                      arr_index);

          sram = (sram_map_t *)PIPE_MGR_GET_SRAM(dev, arr_index);
          if (sram) {
            PIPE_MGR_FREE(sram);
            PIPE_MGR_GET_SRAM(dev, arr_index) = NULL;
          }
          break;
        }
        case pipe_mem_type_tcam: {
          tcam_map_t *tcam;

          arr_index =
              tcam_mem_id_to_arr_index(dev_info, pipe, stage_id, mem_id);
          PHY_MEM_DBG("TCAM blk invalidate: addr 0x%" PRIx64
                      ", dev %d, arr_idx %d \n",
                      address,
                      dev,
                      arr_index);

          tcam = (tcam_map_t *)PIPE_MGR_GET_TCAM(dev, arr_index);
          if (tcam) {
            PIPE_MGR_FREE(tcam);
            PIPE_MGR_GET_TCAM(dev, arr_index) = NULL;
          }
          break;
        }






































        default:
          return PIPE_INVALID_ARG;
      }
    }
  }

  return status;
}

/* Use write block to write to asic */
static inline pipe_status_t pipe_mgr_phy_mem_map_write_block(
    rmt_dev_info_t *dev_info,
    uint64_t address,
    pipe_mem_type_t mem_type,
    int pipe_mask,
    uint8_t *data_ptr,
    int depth,
    int width) {
  pipe_status_t status = PIPE_SUCCESS;
  pipe_sess_hdl_t shdl = pipe_mgr_ctx->int_ses_hndl;
  (void)mem_type;

  if (!data_ptr) {
#if DEVICE_IS_EMULATOR != 2
    /* No data provided so write the memory to zero (also setting MRD for TCAMs)
     * using the blk_wr_data function. */
    uint8_t zero[16] = {0};
    if (mem_type == pipe_mem_type_tcam) zero[0] = 1;
    status = pipe_mgr_drv_blk_wr_data(
        &shdl, dev_info, width, depth, 1, address, pipe_mask, zero);
#endif
  } else {
    /* Copy the data into a DMA buffer and send it down. */
    int bwr_size =
        pipe_mgr_drv_buf_size(dev_info->dev_id, PIPE_MGR_DRV_BUF_BWR);
    pipe_mgr_drv_buf_t *b = pipe_mgr_drv_buf_alloc(
        shdl, dev_info->dev_id, bwr_size, PIPE_MGR_DRV_BUF_BWR, false);
    if (!b) {
      LOG_ERROR(
          "%s : Out of DMA memory blocks, dev %d ", __func__, dev_info->dev_id);
      return PIPE_NO_SYS_RESOURCES;
    }

    PIPE_MGR_MEMCPY(b->addr, data_ptr, depth * width);
    /* write blocks */
    status = pipe_mgr_drv_blk_wr(&shdl, width, depth, 1, address, pipe_mask, b);
  }

  if (status != PIPE_SUCCESS) {
    LOG_ERROR(
        "%s : Write block push failed in blk dnld error %s, dev %d pipe-msk %x "
        "addr 0x%" PRIx64,
        __func__,
        pipe_str_err(status),
        dev_info->dev_id,
        pipe_mask,
        address);
    PIPE_MGR_DBGCHK(PIPE_SUCCESS == status);
  }

  return status;
}

/** Handle an ECC error
 *
 * dev     - Device target
 * address - Address
 */
pipe_status_t pipe_mgr_sram_tcam_ecc_error_correct(bf_dev_id_t dev,
                                                   uint64_t address) {
  pipe_status_t status = PIPE_SUCCESS;
  int arr_index = 0;
  uint8_t mem_type = 0;
  uint64_t log_addr = 0;
  uint64_t real_addr = 0;
  pipe_bitmap_t pipe_bmp;
  bf_dev_pipe_t log_pipe = 0;
  bool ilist_push = false;
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);

  if (dev_info == NULL) {
    LOG_ERROR("%s:%d Device info for device id %d not found",
              __func__,
              __LINE__,
              dev);
    return PIPE_OBJ_NOT_FOUND;
  }

  /* Check if this is a valid pipe address. */
  if (!pipe_mgr_phy_mem_map_addr_valid(dev_info, address)) {
    LOG_ERROR("Received ecc error for address that is not being handled ");
    return PIPE_INVALID_ARG;
  }

  pipe_mgr_map_phy_pipe_id_to_log_pipe_id(
      dev, addr_to_pipe(dev, address), &log_pipe);
  PIPE_BITMAP_INIT(&pipe_bmp, PIPE_BMP_SIZE);
  PIPE_BITMAP_SET(&pipe_bmp, log_pipe);

  log_addr = addr_set_pipe_id(dev, address, log_pipe);

  /* Convert the address to contain the logical pipe, since the shadow
   * memory database is logical pipe based.
   * The logical address is what is used to index data structures.
   */
  mem_type = addr_to_mem_type(dev, log_addr);
  if (PIPE_MGR_MEM_TYPE_SRAM(mem_type)) {
    sram_mem_info_t *sram_line;
    pipe_instr_set_memdata_t instruction_word;
    sram_mem_info_t zero_sram_info;

    arr_index = sram_mem_map_arr_index(dev, log_addr);

    /* if symmetric get block at lowest pipe-id */
    if (PIPE_MGR_IS_SRAM_SYM(dev, arr_index)) {

      arr_index = pipe_mgr_get_sym_arr_index(
          dev, 0, 0, log_addr, arr_index, mem_type, &real_addr);
    }
    if (!PIPE_MGR_GET_SRAM(dev, arr_index)) {
      /* If data not exist in shadow memory, zero it and fix the
         memory error. Also, if table has no entry installed yet then
         the shadow memory might be empty.
      */
      PIPE_MGR_MEMSET(&zero_sram_info, 0, sizeof(zero_sram_info));
      sram_line = &zero_sram_info;
    } else {
      sram_line =
          PIPE_MGR_SRAM_LINE(dev, arr_index, addr_to_mem_offset(dev, log_addr));
    }
    /* Normal Table updates go through ilist.
       Use ilist for ecc correction also to make sure all table update data
       is processed by asic in same order it has been sent
    */

    construct_instr_set_memdata(dev_info,
                                &instruction_word,
                                sram_line->data,
                                PIPE_MGR_MAU_WORD_WIDTH,
                                dev_info->dev_cfg.mem_id_from_addr(log_addr),
                                0,
                                0,
                                addr_to_mem_offset(dev, address),
                                pipe_mem_type_unit_ram);

    status = pipe_mgr_api_enter(pipe_mgr_ctx->int_ses_hndl);
    if (PIPE_SUCCESS != status) return status;
    status = pipe_mgr_drv_ilist_add(&pipe_mgr_ctx->int_ses_hndl,
                                    dev_info,
                                    &pipe_bmp,
                                    addr_to_stage(dev_info, address),
                                    (uint8_t *)&instruction_word,
                                    sizeof(pipe_instr_set_memdata_t));
    pipe_mgr_api_exit(pipe_mgr_ctx->int_ses_hndl);
    if (status != PIPE_SUCCESS) {
      LOG_ERROR("%s : Error in iist add for ecc correction, %s",
                __func__,
                pipe_str_err(status));
      return status;
    }
    ilist_push = true;

  } else if (PIPE_MGR_MEM_TYPE_TCAM(mem_type)) {
    tcam_mem_info_t *tcam_line;
    pipe_set_tcam_write_reg_instr_t tcam_wr_instr;
    tcam_mem_info_t zero_tcam_info;

    arr_index = tcam_mem_map_arr_index(dev, log_addr);

    if (PIPE_MGR_IS_TCAM_SYM(dev, arr_index)) {

      arr_index = pipe_mgr_get_sym_arr_index(
          dev, 0, 0, log_addr, arr_index, mem_type, &real_addr);
    }
    if (!PIPE_MGR_GET_TCAM(dev, arr_index)) {
      /* If data not exist in shadow memory, zero it and fix the
         memory error. Also, if table has no entry installed yet then
         the shadow memory might be empty.
      */
      PIPE_MGR_MEMSET(&zero_tcam_info, 0, sizeof(zero_tcam_info));
      zero_tcam_info.data[0] = 1;
      tcam_line = &zero_tcam_info;
    } else {
      tcam_line =
          PIPE_MGR_TCAM_LINE(dev, arr_index, addr_to_mem_offset(dev, log_addr));
    }

    construct_instr_tcam_write(dev,
                               &tcam_wr_instr,
                               dev_info->dev_cfg.mem_id_from_addr(log_addr),
                               addr_to_mem_offset(dev, log_addr),
                               1);
    status = pipe_mgr_api_enter(pipe_mgr_ctx->int_ses_hndl);
    if (PIPE_SUCCESS != status) return status;
    status = pipe_mgr_drv_ilist_add_2(&pipe_mgr_ctx->int_ses_hndl,
                                      dev_info,
                                      &pipe_bmp,
                                      addr_to_stage(dev_info, log_addr),
                                      (uint8_t *)&tcam_wr_instr,
                                      sizeof(pipe_set_tcam_write_reg_instr_t),
                                      (uint8_t *)tcam_line,
                                      PIPE_MGR_MAU_WORD_WIDTH);
    pipe_mgr_api_exit(pipe_mgr_ctx->int_ses_hndl);
    if (status != PIPE_SUCCESS) {
      LOG_ERROR("%s : Error in iist add for ecc correction, %s",
                __func__,
                pipe_str_err(status));
      return status;
    }
    ilist_push = true;
  }

  if (ilist_push) {
    status = pipe_mgr_api_enter(pipe_mgr_ctx->int_ses_hndl);
    if (PIPE_SUCCESS != status) return status;
    status = pipe_mgr_drv_ilist_push(&pipe_mgr_ctx->int_ses_hndl, NULL, NULL);
    pipe_mgr_api_exit(pipe_mgr_ctx->int_ses_hndl);
    if (status != PIPE_SUCCESS) {
      LOG_ERROR(
          "Failed to push sram-tcam ecc corr instruction"
          " list (%d)",
          status);
    }
  }

  return status;
}

static pipe_status_t download_by_index(rmt_dev_info_t *dev_info,
                                       pipe_mem_type_t mem_type,
                                       int index) {
  rmt_dev_cfg_t *cfg = &dev_info->dev_cfg;
  uint64_t phy_addr =
      pipe_mgr_convert_arr_index_to_phy_addr(dev_info, index, mem_type);
  uint8_t *data = NULL;
  uint8_t pipe_list = 0;
  int depth = 0;
  int width = 0;
  switch (mem_type) {
    case pipe_mem_type_unit_ram:
      data = (uint8_t *)PIPE_MGR_SRAM_LINE(dev_info->dev_id, index, 0);
      pipe_list = PIPE_MGR_GET_SRAM_PIPE_LIST(dev_info->dev_id, index);
      depth = cfg->stage_cfg.sram_unit_depth;
      width = PIPE_MGR_MAU_WORD_WIDTH;
      break;














    case pipe_mem_type_tcam:
      data = (uint8_t *)PIPE_MGR_TCAM_LINE(dev_info->dev_id, index, 0);
      pipe_list = PIPE_MGR_GET_TCAM_PIPE_LIST(dev_info->dev_id, index);
      depth = cfg->stage_cfg.tcam_unit_depth;
      width = PIPE_MGR_MAU_WORD_WIDTH;
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }
  return pipe_mgr_phy_mem_map_write_block(
      dev_info, phy_addr, mem_type, pipe_list, data, depth, width);
}

/** Download one memory block to the ASIC.
 *
 * dev - device
 * pipe_bmp - which pipe(s)
 * stage_id - which stage the memory block is in
 * lower_phy_addr - lowest address within the memory block
 */
pipe_status_t pipe_mgr_phy_mem_map_download_one_block(bf_dev_id_t dev,
                                                      pipe_tbl_dir_t gress,
                                                      bf_dev_pipe_t pipe_id,
                                                      dev_stage_t stage_id,
                                                      pipe_mem_type_t mem_type,
                                                      mem_id_t mem_id) {
  int arr_index = 0;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return PIPE_INVALID_ARG;
  (void)gress;
  /* Sanity checks:
   *  - If symmetric, the requested set of pipes must match the shadow's
   *    set of pipes.
   *  - If not symmetric, the requested pipe must match the pipe set in
   *    the shadow.
   *  - The requested symmetric mode must match the shadow's for the
   *    requested memory.
   *  - The memory must exist in the shadow. */
  switch (mem_type) {
    case pipe_mem_type_unit_ram:
      arr_index =
          sram_mem_id_to_arr_index(dev_info, pipe_id, gress, stage_id, mem_id);
      PIPE_MGR_DBGCHK(PIPE_MGR_GET_SRAM(dev_info->dev_id, arr_index));
      break;
    case pipe_mem_type_tcam:
      arr_index = tcam_mem_id_to_arr_index(dev_info, pipe_id, stage_id, mem_id);
      PIPE_MGR_DBGCHK(PIPE_MGR_GET_TCAM(dev_info->dev_id, arr_index));
      break;











    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }

  return download_by_index(dev_info, mem_type, arr_index);
}

static inline void pipe_mgr_dump_shadow_mem_line(
    uint16_t offset, uint8_t *data_ptr, char *str, int *curr_len, int max_len) {
  int j = 0;
  int c_len = *curr_len;

  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "Line %d: ",
                    offset);

  for (j = 0; (data_ptr) && (j < PIPE_MGR_MAU_WORD_WIDTH); j++) {
    /* Print every byte */
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "%02x ",
                      data_ptr[j]);
  }
  c_len += snprintf(
      str + c_len, (c_len < max_len) ? (max_len - c_len - 1) : 0, "\n");
  *curr_len = c_len;
}

/* Shadow memory dump */
pipe_status_t pipe_mgr_dump_phy_shadow_memory(bf_dev_id_t dev,
                                              pipe_tbl_dir_t gress,
                                              bf_dev_pipe_t log_pipe_id,
                                              dev_stage_t stage_id,
                                              uint16_t mem_id,
                                              uint8_t mem_type,
                                              uint16_t line_no,
                                              bool all_lines,
                                              char *str,
                                              int max_len) {
  pipe_status_t status = PIPE_SUCCESS;
  uint64_t address = 0;
  uint16_t offset = 0;
  bf_dev_pipe_t phy_pipe_id = 0;
  int arr_index = 0;
  int c_len = 0;
  uint64_t real_addr = 0;
  (void)gress;
  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "Dumping shadow memory for dev %d, pipe %d, stage %d, "
                    "mem_id %d, mem_type %d \n",
                    dev,
                    log_pipe_id,
                    stage_id,
                    mem_id,
                    mem_type);

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(dev);
  if (!dev_info) return PIPE_INVALID_ARG;
  if (stage_id > dev_info->num_active_mau) return PIPE_INVALID_ARG;
  pipe_mgr_map_pipe_id_log_to_phy(dev_info, log_pipe_id, &phy_pipe_id);
  address = dev_info->dev_cfg.get_full_phy_addr(
      gress, phy_pipe_id, stage_id, mem_id, line_no, mem_type);

  switch (mem_type) {
    case pipe_mem_type_unit_ram: {
      sram_mem_info_t *sram_line;

      if (line_no >= TOF_SRAM_UNIT_DEPTH) return PIPE_INVALID_ARG;

      arr_index = sram_mem_map_arr_index(dev, address);

      if (PIPE_MGR_IS_SRAM_SYM(dev, arr_index)) {
        arr_index = pipe_mgr_get_sym_arr_index(
            dev, gress, stage_id, address, arr_index, mem_type, &real_addr);
      }

      if (!PIPE_MGR_GET_SRAM(dev, arr_index)) {
        c_len += snprintf(str + c_len,
                          (c_len < max_len) ? (max_len - c_len - 1) : 0,
                          "SRAM block has default values \n");
        return PIPE_SUCCESS;
      }

      if (!all_lines) {
        offset = line_no;
        sram_line = PIPE_MGR_SRAM_LINE(dev, arr_index, offset);
        pipe_mgr_dump_shadow_mem_line(
            offset, (uint8_t *)sram_line, str, &c_len, max_len);
      } else {
        for (offset = 0; offset < TOF_SRAM_UNIT_DEPTH; offset++) {
          sram_line = PIPE_MGR_SRAM_LINE(dev, arr_index, offset);
          pipe_mgr_dump_shadow_mem_line(
              offset, (uint8_t *)sram_line, str, &c_len, max_len);
        }
      }
      break;
    }
    case pipe_mem_type_tcam: {
      tcam_mem_info_t *tcam_line;

      if (line_no >= TOF_TCAM_UNIT_DEPTH) return PIPE_INVALID_ARG;

      arr_index = tcam_mem_map_arr_index(dev, address);

      if (PIPE_MGR_IS_TCAM_SYM(dev, arr_index)) {
        arr_index = pipe_mgr_get_sym_arr_index(
            dev, gress, stage_id, address, arr_index, mem_type, &real_addr);
      }

      if (!PIPE_MGR_GET_TCAM(dev, arr_index)) {
        c_len += snprintf(str + c_len,
                          (c_len < max_len) ? (max_len - c_len - 1) : 0,
                          "TCAM block has default values \n");
        return PIPE_SUCCESS;
      }

      if (!all_lines) {
        offset = line_no;
        tcam_line = PIPE_MGR_TCAM_LINE(dev, arr_index, offset);
        pipe_mgr_dump_shadow_mem_line(
            offset, (uint8_t *)tcam_line, str, &c_len, max_len);
      } else {
        for (offset = 0; offset < TOF_TCAM_UNIT_DEPTH; offset++) {
          tcam_line = PIPE_MGR_TCAM_LINE(dev, arr_index, offset);
          pipe_mgr_dump_shadow_mem_line(
              offset, (uint8_t *)tcam_line, str, &c_len, max_len);
        }
      }
      break;
    }




































































    default:
      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        "Invalid mem-type %d \n",
                        mem_type);
      return PIPE_SUCCESS;
  }

  return status;
}

/* Initialize srams/tcams on chip init.
   everything -
     true:   Write all memories, unused memories will be zeroed.
     false:  Write all memories used by the current program(s).  If the memory
             has a shadow it will be used otherwise the memory will be zeroed.
   Note that "zeroing" of a TCAM actually sets the MRD bit.
 */
pipe_status_t phy_mem_map_load_srams_tcams(rmt_dev_info_t *dev_info,
                                           bool everything) {
  bf_dev_id_t dev = dev_info->dev_id;
  pipe_status_t sts = PIPE_SUCCESS;
  uint32_t arr_index = 0;

  uint32_t num_pipes = dev_info->num_active_pipes;
  rmt_dev_cfg_t *cfg = &dev_info->dev_cfg;




















  /* srams */
  for (uint32_t pipe = 0; pipe < num_pipes; ++pipe) {
    uint32_t pipe_bit_map = 1u << pipe;
    uint32_t pipe_mask = pipe_bit_map | (pipe_bit_map - 1);
    for (uint8_t gress = 0; gress < cfg->num_gress; gress++) {
      for (uint32_t stage = 0; stage < dev_info->num_active_mau; stage++) {
        /* SRAMs */
        pipe_mem_type_t mem_type = pipe_mem_type_unit_ram;
        for (uint32_t row = 0; row < cfg->stage_cfg.num_sram_rows; row++) {
          if (!cfg->sram_row_valid(row)) continue;
          for (uint32_t col = 0; col < cfg->stage_cfg.num_sram_cols; col++) {
            if (!cfg->sram_col_valid(col)) continue;
            int mem_id = cfg->mem_id_from_col_row(stage, col, row, mem_type);
            arr_index =
                sram_mem_id_to_arr_index(dev_info, pipe, gress, stage, mem_id);
            /* If the memory is used by this pipe AND this is the lowest pipe in
             * its pipe set then download it. */
            uint32_t mem_pipe_list =
                PIPE_MGR_GET_SRAM_PIPE_LIST(dev, arr_index);
            bool wrote_it = true;
            if ((pipe_mask & mem_pipe_list) == pipe_bit_map) {
              sts = download_by_index(dev_info, mem_type, arr_index);
            } else if (mem_pipe_list == 0 && everything) {
              /* The memory isn't used but we are loading everything.  */
              PIPE_MGR_GET_SRAM_PIPE_LIST(dev, arr_index) = pipe_bit_map;
              sts = download_by_index(dev_info, mem_type, arr_index);
              PIPE_MGR_GET_SRAM_PIPE_LIST(dev, arr_index) = 0;
            } else {
              wrote_it = false;
            }
            if (wrote_it) {
              if (sts != PIPE_SUCCESS) {
                LOG_ERROR(
                    "Dev %d failed to download SRAM block to pipe-mask 0x%x "
                    "stage %d row %d col %d",
                    dev_info->dev_id,
                    PIPE_MGR_GET_SRAM_PIPE_LIST(dev_info->dev_id, arr_index),
                    stage,
                    row,
                    col);
                return sts;
              } else {
                LOG_TRACE(
                    "Dev %d stage %2d loaded SRAM row %d col %d to pipe-mask "
                    "%X",
                    dev_info->dev_id,
                    stage,
                    row,
                    col,
                    mem_pipe_list ? mem_pipe_list : pipe_bit_map);
              }
            }
          }
        }

        if (gress == 1) continue;
        /* TCAMs */
        mem_type = pipe_mem_type_tcam;
        for (uint32_t row = 0; row < cfg->stage_cfg.num_tcam_rows; row++) {
          if (!cfg->tcam_row_valid(row)) continue;
          for (uint32_t col = 0; col < cfg->stage_cfg.num_tcam_cols; col++) {
            if (!cfg->tcam_col_valid(col)) continue;
            int mem_id = cfg->mem_id_from_col_row(stage, col, row, mem_type);
            arr_index = tcam_mem_id_to_arr_index(dev_info, pipe, stage, mem_id);
            /* If the memory is used by this pipe AND this is the lowest pipe in
             * its pipe set then download it. */
            uint32_t mem_pipe_list =
                PIPE_MGR_GET_TCAM_PIPE_LIST(dev, arr_index);
            bool wrote_it = true;
            if ((pipe_mask & mem_pipe_list) == pipe_bit_map) {
              sts = download_by_index(dev_info, mem_type, arr_index);
            } else if (mem_pipe_list == 0 && everything) {
              /* The memory isn't used but we are loading everything.  */
              PIPE_MGR_GET_TCAM_PIPE_LIST(dev, arr_index) = pipe_bit_map;
              sts = download_by_index(dev_info, mem_type, arr_index);
              PIPE_MGR_GET_TCAM_PIPE_LIST(dev, arr_index) = 0;
            } else {
              wrote_it = false;
            }
            if (wrote_it) {
              if (sts != PIPE_SUCCESS) {
                LOG_ERROR(
                    "Dev %d failed to download TCAM block to pipe-mask 0x%x "
                    "stage %d row %d col %d",
                    dev_info->dev_id,
                    PIPE_MGR_GET_TCAM_PIPE_LIST(dev_info->dev_id, arr_index),
                    stage,
                    row,
                    col);
                return sts;
              } else {
                LOG_TRACE(
                    "Dev %d stage %2d loaded TCAM row %d col %d to pipe-mask "
                    "%X",
                    dev_info->dev_id,
                    stage,
                    row,
                    col,
                    mem_pipe_list ? mem_pipe_list : pipe_bit_map);
              }
            }
          }
        }


















































      }
    }
  }
  return sts;
}
