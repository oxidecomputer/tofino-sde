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


#ifndef BF_DMA_TYPES_H_INCLUDED
#define BF_DMA_TYPES_H_INCLUDED

/**
 * @addtogroup dvm-device-mgmt
 * @{
 */

#define BF_DMA_DR_DIR_DEV2CPU 0
#define BF_DMA_DR_DIR_CPU2DEV 1
#define BF_DMA_DR_DIRS 2

typedef enum bf_dma_type_e {
  BF_DMA_TYPE_FIRST = 0,
  BF_DMA_PIPE_INSTRUCTION_LIST = BF_DMA_TYPE_FIRST,
  BF_DMA_PIPE_LEARN_NOTIFY,
  BF_DMA_PIPE_STAT_NOTIFY,
  BF_DMA_PIPE_IDLE_STATE_NOTIFY,
  BF_DMA_PIPE_BLOCK_WRITE,
  BF_DMA_PIPE_BLOCK_READ,
  BF_DMA_TM_WRITE_LIST,
  BF_DMA_DIAG_ERR_NOTIFY,
  BF_DMA_MAC_STAT_RECEIVE,
  BF_DMA_CPU_PKT_RECEIVE_0,
  BF_DMA_CPU_PKT_RECEIVE_1,
  BF_DMA_CPU_PKT_RECEIVE_2,
  BF_DMA_CPU_PKT_RECEIVE_3,
  BF_DMA_CPU_PKT_RECEIVE_4,
  BF_DMA_CPU_PKT_RECEIVE_5,
  BF_DMA_CPU_PKT_RECEIVE_6,
  BF_DMA_CPU_PKT_RECEIVE_7,
  BF_DMA_CPU_PKT_TRANSMIT_0,
  BF_DMA_CPU_PKT_TRANSMIT_1,
  BF_DMA_CPU_PKT_TRANSMIT_2,
  BF_DMA_CPU_PKT_TRANSMIT_3,
  BF_DMA_MAC_BLOCK_WRITE,
  BF_DMA_TM_WRITE_LIST_1,
  BF_DMA_TM_BLOCK_READ_0,
  BF_DMA_TM_BLOCK_READ_1,
  BF_DMA_TYPE_MAX
} bf_dma_type_t;
#define BF_DMA_TYPE_MAX_TOF (BF_DMA_CPU_PKT_TRANSMIT_3 + 1)
#define BF_DMA_TYPE_MAX_TOF2 (BF_DMA_TM_BLOCK_READ_1 + 1)
#define BF_DMA_TYPE_MAX_TOF3 (BF_DMA_TM_BLOCK_READ_1 + 1)

static const char *bf_dma_type_strs[BF_DMA_TYPE_MAX + 1] = {
    "BF_DMA_PIPE_INSTRUCTION_LIST", "BF_DMA_PIPE_LEARN_NOTIFY",
    "BF_DMA_PIPE_STAT_NOTIFY",      "BF_DMA_PIPE_IDLE_STATE_NOTIFY",
    "BF_DMA_PIPE_BLOCK_WRITE",      "BF_DMA_PIPE_BLOCK_READ",
    "BF_DMA_TM_WRITE_LIST",         "BF_DMA_DIAG_ERR_NOTIFY",
    "BF_DMA_MAC_STAT_RECEIVE",      "BF_DMA_CPU_PKT_RECEIVE_0",
    "BF_DMA_CPU_PKT_RECEIVE_1",     "BF_DMA_CPU_PKT_RECEIVE_2",
    "BF_DMA_CPU_PKT_RECEIVE_3",     "BF_DMA_CPU_PKT_RECEIVE_4",
    "BF_DMA_CPU_PKT_RECEIVE_5",     "BF_DMA_CPU_PKT_RECEIVE_6",
    "BF_DMA_CPU_PKT_RECEIVE_7",     "BF_DMA_CPU_PKT_TRANSMIT_0",
    "BF_DMA_CPU_PKT_TRANSMIT_1",    "BF_DMA_CPU_PKT_TRANSMIT_2",
    "BF_DMA_CPU_PKT_TRANSMIT_3",    "BF_DMA_MAC_BLOCK_WRITE",
    "BF_DMA_TM_WRITE_LIST_1",       "BF_DMA_TM_BLOCK_READ_0",
    "BF_DMA_TM_BLOCK_READ_1",       "BF_DMA_TYPE_MAX"};
static inline const char *bf_dma_type_str(bf_dma_type_t t) {
  return bf_dma_type_strs[t];
}

#endif  // BF_DMA_TYPES_H_INCLUDED
