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
 * @file pipe_mgr_entry_format.c
 * @date
 *
 *  Entry Formatting APIs used during Table entry add/decode.
 */

#include <unistd.h>
#include <stdbool.h>
#include <inttypes.h>
#include <arpa/inet.h>
#ifdef __sun
#include <endian.h>
#endif
#include <pipe_mgr/pipe_mgr_err.h>
#include <pipe_mgr/pipe_mgr_intf.h>
#include <pipe_mgr/pipe_mgr_config.h>
#include <pipe_mgr/pipe_mgr_porting.h>
#include "pipe_mgr_int.h"
#include "pipe_mgr_table_packing.h"
#include "pipe_mgr_entry_format_json.h"
#include "pipe_mgr_drv_intf.h"
#include "pipe_mgr_learn.h"
#include "pipe_mgr_mau_snapshot.h"
#include "pipe_mgr_tof2_mau_snapshot.h"
#include "pipe_mgr_dkm.h"
#include "pipe_mgr_select_tbl.h"
#include "pipe_mgr_meter_mgr_int.h"
#include "pipe_mgr_stful_tbl_mgr.h"
#include <tofino_regs/tofino.h>
#include <tof3_regs/tof3_mem_drv.h>

pipemgr_tbl_pkg_ctx_t *g_tbl_pkg_ctx[BF_MAX_DEV_COUNT];
#define PIPE_MGR_TBL_PKG_CTX(_dev, _prof) (g_tbl_pkg_ctx[_dev]->profiles[_prof])

#define TBLPACK_TOF_BYTES_IN_TCAM_WORD 6
#define TBLPACK_TOF_BYTES_IN_RAM_WORD 16
#define TBLPACK_LONG_STR_LEN 4000
#define TBLPACK_SMALL_STR_LEN 800
#define TBLPACK_WORD_BYTES_MAX 16
#define TBLPACK_BYTES_MAX 128

#define LEARN_QUANTA_SIZE 48

#define ENTRYFORMAT_MIN(x, y) ((x) < (y)) ? (x) : (y)

static char *pipe_mgr_stful_ctrl_info_strings[] = {
    "STATEFUL:NOP",
    "STATEFUL:STATEFUL INST0",
    "STATEFUL:CFG_RD",
    "STATEFUL:STATEFUL INST1",
    "STATEFUL:CFG_WR",
    "STATEFUL:STATEFUL INST2",
    "STATEFUL:STATEFUL_CLEAR",
    "STATEFUL:STATEFUL INST3",
    "STATEFUL:MOVEREG_RD",
    "STATEFUL:CFG STATEFUL INST0",
    "STATEFUL:MOVEREG_WR",
    "STATEFUL:CFG STATEFUL INST1",
    "STATEFUL:CFG STATEFUL INST2",
    "STATEFUL:CFG STATEFUL INST3",
    "STATEFUL:MOVEREG_WR_POP",
    "STATEFUL:SELECTOR",
};
static char *pipe_mgr_sel_ctrl_info_strings[] = {
    "SEL:NOP",
    "SEL:STATEFUL INST0",
    "SEL:CFG_RD",
    "SEL:STATEFUL INST1",
    "SEL:CFG_WR",
    "SEL:STATEFUL INST2",
    "SEL:STATEFUL_CLEAR",
    "SEL:STATEFUL INST3",
    "SEL:MOVEREG_RD",
    "SEL:CFG STATEFUL INST0",
    "SEL:MOVEREG_WR",
    "SEL:CFG STATEFUL INST1",
    "SEL:CFG STATEFUL INST2",
    "SEL:CFG STATEFUL INST3",
    "SEL:MOVEREG_WR_POP",
    "SEL:SELECTOR",
};
static char *pipe_mgr_meter_ctrl_info_strings[] = {
    "METER:NOP",
    "METER:COLOR0",
    "METER:CFG_RD",
    "METER:COLOR1",
    "METER:CFG_WR",
    "METER:COLOR2",
    "METER:SWEEP",
    "METER:COLOR3",
    "METER:MOVEREG_RD",
    "NONE",
    "METER:MOVEREG_WR",
    "NONE",
    "NONE",
    "NONE",
    "METER:MOVEREG_WR_POP",
    "NONE",
};

typedef struct {
  uint32_t vliw_addr;
  uint32_t table_mask;
  uint32_t next_table_long_branch;
  uint64_t immediate_val;
  uint32_t next_table;
  uint32_t next_table_default;
  uint32_t next_table_exec;


} pipe_default_ent_reg_data;

// Forward declaration
static uint64_t get_val(uint8_t *src, uint16_t offset, uint16_t len);
static uint64_t get_val_decode(uint8_t **src,
                               uint16_t word_offset,
                               uint16_t offset,
                               uint16_t len);

static void pipemgr_tbl_pkg_free_p4_str_table(bf_dev_id_t devid,
                                              profile_id_t prof_id) {
  int i;
  for (i = 0; i < PIPEMGR_TBL_PKG_MAX_P4_FIELD_NAMES; i++) {
    if (PIPE_MGR_TBL_PKG_CTX(devid, prof_id).p4_strings[i])
      PIPE_MGR_FREE(PIPE_MGR_TBL_PKG_CTX(devid, prof_id).p4_strings[i]);
  }
}

char *pipemgr_tbl_pkg_get_field_string_name(bf_dev_id_t devid,
                                            profile_id_t prof_id,
                                            uint32_t index) {
  PIPE_MGR_DBGCHK(index <
                  PIPE_MGR_TBL_PKG_CTX(devid, prof_id).field_name_str_index);
  return (PIPE_MGR_TBL_PKG_CTX(devid, prof_id).p4_strings[index]);
}

pipe_status_t pipemgr_tbl_pkging_init(bf_dev_id_t devid,
                                      uint32_t num_profiles) {
  g_tbl_pkg_ctx[devid] = PIPE_MGR_CALLOC(1, sizeof(pipemgr_tbl_pkg_ctx_t));
  if (g_tbl_pkg_ctx[devid] == NULL) {
    LOG_ERROR(
        "%s: Entry formatting error. Unable to allocate memory for dev %d",
        __func__,
        devid);
    return (PIPE_NO_SYS_RESOURCES);
  }
  g_tbl_pkg_ctx[devid]->profiles =
      PIPE_MGR_CALLOC(num_profiles, sizeof(pipemgr_tbl_pkg_profile_ctx_t));

  if (g_tbl_pkg_ctx[devid]->profiles == NULL) {
    LOG_ERROR(
        "%s: Entry formatting error. Unable to allocate memory for "
        "profiles, dev %d",
        __func__,
        devid);
    PIPE_MGR_FREE(g_tbl_pkg_ctx[devid]);
    g_tbl_pkg_ctx[devid] = NULL;
    return (PIPE_NO_SYS_RESOURCES);
  }
  g_tbl_pkg_ctx[devid]->num_profiles = num_profiles;
  return (PIPE_SUCCESS);
}

static void PIPEMGR_TBL_PKG_FREE_LUT(int table_depth,
                                     pipemgr_tbl_pkg_lut_t *lut) {
  int i;
  for (i = 0; lut && i < table_depth * PIPEMGR_TBL_PKG_HASH_COLLISION_FACTOR;
       i++) {
    /* union field. Pick any */
    if (lut[i].u.exm_ptr) {
      PIPE_MGR_FREE(lut[i].u.exm_ptr);
    }
  }
}

void pipemgr_tbl_pkging_cleanup(bf_dev_id_t devid) {
  pipemgr_tbl_pkg_lut_t *lut;
  profile_id_t prof_id = 0;
  for (prof_id = 0; prof_id < (int)g_tbl_pkg_ctx[devid]->num_profiles;
       prof_id++) {
    lut = PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut;
    PIPEMGR_TBL_PKG_FREE_LUT(PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut_depth,
                             lut);
    PIPE_MGR_FREE(PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut);

    lut = PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tind_lut;
    PIPEMGR_TBL_PKG_FREE_LUT(
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tind_lut_depth, lut);
    PIPE_MGR_FREE(PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tind_lut);

    lut = PIPE_MGR_TBL_PKG_CTX(devid, prof_id).adt_lut;
    PIPEMGR_TBL_PKG_FREE_LUT(PIPE_MGR_TBL_PKG_CTX(devid, prof_id).adt_lut_depth,
                             lut);
    PIPE_MGR_FREE(PIPE_MGR_TBL_PKG_CTX(devid, prof_id).adt_lut);

    lut = PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tern_lut;
    PIPEMGR_TBL_PKG_FREE_LUT(
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tern_lut_depth, lut);
    PIPE_MGR_FREE(PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tern_lut);

    lut = PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut;
    PIPEMGR_TBL_PKG_FREE_LUT(PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut_depth,
                             lut);
    PIPE_MGR_FREE(PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut);

    lut = PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_match_lut;
    PIPEMGR_TBL_PKG_FREE_LUT(
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_match_lut_depth, lut);
    PIPE_MGR_FREE(PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_match_lut);

    lut = PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_action_lut;
    PIPEMGR_TBL_PKG_FREE_LUT(
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_action_lut_depth, lut);
    PIPE_MGR_FREE(PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_action_lut);

    lut = PIPE_MGR_TBL_PKG_CTX(devid, prof_id).matchspec_lut;
    PIPEMGR_TBL_PKG_FREE_LUT(
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).matchspec_lut_depth, lut);
    PIPE_MGR_FREE(PIPE_MGR_TBL_PKG_CTX(devid, prof_id).matchspec_lut);

    lut = PIPE_MGR_TBL_PKG_CTX(devid, prof_id).actionspec_lut;
    PIPEMGR_TBL_PKG_FREE_LUT(
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).actionspec_lut_depth, lut);
    PIPE_MGR_FREE(PIPE_MGR_TBL_PKG_CTX(devid, prof_id).actionspec_lut);

    lut = PIPE_MGR_TBL_PKG_CTX(devid, prof_id).rangetbl_lut;
    PIPEMGR_TBL_PKG_FREE_LUT(
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).rangetbl_lut_depth, lut);
    PIPE_MGR_FREE(PIPE_MGR_TBL_PKG_CTX(devid, prof_id).rangetbl_lut);

    lut = PIPE_MGR_TBL_PKG_CTX(devid, prof_id).lq_lut;
    PIPEMGR_TBL_PKG_FREE_LUT(PIPE_MGR_TBL_PKG_CTX(devid, prof_id).lq_lut_depth,
                             lut);
    PIPE_MGR_FREE(PIPE_MGR_TBL_PKG_CTX(devid, prof_id).lq_lut);

    lut = PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut;
    PIPEMGR_TBL_PKG_FREE_LUT(
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth, lut);
    PIPE_MGR_FREE(PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut);

    pipemgr_tbl_pkg_free_p4_str_table(devid, prof_id);
  }
  if (g_tbl_pkg_ctx[devid]->profiles) {
    PIPE_MGR_FREE(g_tbl_pkg_ctx[devid]->profiles);
    g_tbl_pkg_ctx[devid]->profiles = NULL;
    g_tbl_pkg_ctx[devid]->num_profiles = 0;
  }

  if (g_tbl_pkg_ctx[devid]) {
    PIPE_MGR_FREE(g_tbl_pkg_ctx[devid]);
    g_tbl_pkg_ctx[devid] = NULL;
  }
}

static void get_shifted_val_decode(uint8_t *src,
                                   uint16_t offset,
                                   uint16_t len,
                                   uint64_t *vals);

/* Prints bytes which are in Big-endian(Network byte order) format */
size_t print_bytes(uint8_t *in,
                   uint32_t sz,
                   char *indent,
                   char *dest_str,
                   size_t dest_str_size) {
  uint32_t i = 0;
  char *end = dest_str + dest_str_size;
  char *ptr = dest_str;

  for (i = 0; i < sz; i++) {
    if (i && !(i % 16)) {
      ptr +=
          snprintf(ptr, end > ptr ? (end - ptr) : 0, "\n%s%4d -- ", indent, i);
    }
    ptr += snprintf(ptr, end > ptr ? (end - ptr) : 0, "%02x  ", in[i]);
  }
  return (end > ptr ? (size_t)(ptr - dest_str) : dest_str_size);
}

static void set_val(uint8_t **dst,
                    uint8_t word_offset,
                    uint16_t dst_offset,
                    uint16_t len,
                    uint64_t val) {
  uint8_t *wp;  // Write pointer
  uint8_t wo;   // Write offset (bit offset within byte pointed to by wp).
  uint8_t wm;   // Write mask (mask of bits in byte pointed to by wp to set).
  uint8_t word_iter = 0;

  /* Use dst_offset to find the correct 128-bit word to update. */
  word_iter = (dst_offset / 8) / 16;
  /* Decrement dst_offset by the number of 128-bit words skipped over. */
  dst_offset -= (128 * word_iter);

  /* Set the write-ptr to the correct byte offset in the 128-bit word. */
  wp = dst[word_offset + word_iter] + dst_offset / 8;

  /* The write-offset is 0-7 and handles the case where the field is not byte
   * aligned so the first write updates only part of a byte. */
  wo = dst_offset % 8;
  while (len) {
    /* If the write pointer has advanced past the end of the 128-bit word then
     * reset it to the start of the next 128-bit word. */
    if (wp - dst[word_offset + word_iter] > 15) {
      word_iter++;
      wp = dst[word_offset + word_iter];
    }

    wm = len < 8 ? (1 << len) - 1 : 0xFF;
    wm = wm << wo;

    *wp = (*wp & ~wm) | ((val << wo) & wm);

    val = val >> (8 - wo);
    ++wp;
    len -= (len < (8 - wo)) ? len : (8 - wo);
    wo = 0;
  }
}

static void set_val_old(uint8_t *dst,
                        uint16_t dst_offset,
                        uint16_t len,
                        uint64_t val) {
  uint8_t *wp;  // Write pointer
  uint8_t wo;   // Write offset (bit offset within byte pointed to by wp).
  uint8_t wm;   // Write mask (mask of bits in byte pointed to by wp to set).

  wp = dst + dst_offset / 8;
  wo = dst_offset % 8;
  while (len) {
    wm = len < 8 ? (1 << len) - 1 : 0xFF;
    wm = wm << wo;

    *wp = (*wp & ~wm) | ((val << wo) & wm);

    val = val >> (8 - wo);
    ++wp;
    len -= (len < (8 - wo)) ? len : (8 - wo);
    wo = 0;
  }
}

static void set_val_reverse(uint8_t *dst,
                            uint16_t dst_offset,
                            uint16_t len,
                            uint64_t val) {
  uint8_t *wp;  // Write pointer
  uint8_t wo;   // Write offset (bit offset within byte pointed to by wp).
  uint8_t wm;   // Write mask (mask of bits in byte pointed to by wp to set).

  wp = dst + dst_offset / 8;
  wo = dst_offset % 8;
  while (len) {
    wm = len < 8 ? (1 << len) - 1 : 0xFF;
    wm = wm << wo;

    *wp = (*wp & ~wm) | ((val << wo) & wm);

    val = val >> (8 - wo);
    --wp;
    len -= (len < (8 - wo)) ? len : (8 - wo);
    wo = 0;
  }
}

static uint64_t get_val_reverse(uint8_t *src, uint16_t offset, uint16_t len) {
  uint8_t *rp; /* Read Pointer, points to current byte where data is being
                  extracted. */
  uint16_t cnt = 0;
  uint64_t val = 0;
  int l = len;
  /* Last (highest address) byte of the field to extract. */
  rp = src + ((offset + len - 1) / 8);

  /* Extract bytes from the src from the highest address down to the lowest
   * address.  Store them in val from the lowest address to the higest. */
  while (l > 0) {
    uint64_t x = *rp;
    val |= x << (8 * cnt);
    ++cnt;
    --rp;
    l -= 8;
  }
  /* Mask off high bits which are not used. */
  uint64_t mask = 0ull;
  mask = (len == 64) ? ~0ull : ((1ULL << len) - 1);
  return val & mask;
}

static void get_shifted_val(uint8_t *src,
                            uint16_t offset,
                            uint16_t len,
                            uint16_t shift,
                            uint64_t *vals) {
  unsigned i = 0;

  // Read out the entire source into the vals array.
  while (len) {
    uint16_t l = len < 64 ? len : 64;
    vals[i] = get_val_reverse(src, offset + len - l, l);
    len -= l;
    ++i;
  }

  // Right shift the vals array based on the requested shift amount.
  // First shift by blocks of 64 since that is just copies.
  while (shift >= 64) {
    for (i = 1; i < 16; ++i) {
      vals[i - 1] = vals[i];
    }
    vals[15] = 0;
    shift -= 64;
  }
  // Shift by the remaining amount, moving the lsbs of higher words into the
  // msbs of lower words.
  if (shift) {
    for (i = 0; i < 15; ++i) {
      vals[i] = (vals[i] >> shift) | (vals[i + 1] << (64 - shift));
    }
    vals[15] = vals[15] >> shift;
  }
}

static void get_reverse_shifted_val(uint8_t *src,
                                    uint16_t offset,
                                    uint16_t len,
                                    uint16_t shift,
                                    uint64_t *vals) {
  unsigned i = 0;

  // Read out the entire source into the vals array.
  while (len) {
    uint16_t l = len < 64 ? len : 64;
    vals[i] = get_val(src, offset + len - l, l);
    len -= l;
    ++i;
  }

  // Left shift the vals array based on the requested shift amount.
  // First shift by blocks of 64 since that is just copies.
  while (shift >= 64) {
    for (i = 15; i > 0; --i) {
      vals[i] = vals[i - 1];
    }
    vals[0] = 0;
    shift -= 64;
  }
  // Shift by the remaining amount, moving the msbs of lower words into the
  // lsbs of higher words.
  if (shift) {
    for (i = 15; i > 0; --i) {
      vals[i] = (vals[i] << shift) | (vals[i - 1] >> (64 - shift));
    }
    vals[0] = vals[0] << shift;
  }
}

/* Write a 64 bit value into the match spec.
 * The destination field in the match spec is described by the offset and len
 * arguments and may be larger than the field being written.  The dst will be
 * written with len number of bits from offset to offset + len - 1.  It is
 * assumed that fields in the match spec are in network order so the most
 * significant byte of the field is at offset and the least significant byte
 * starts at offset + len - 8.  The value in val will be written from the least
 * significant byte to the most significant byte.  Any additional bytes in the
 * field (if val is smaller than the field size) are written to zero. */
void put_shifted_val(uint8_t *dst,
                     uint16_t offset,
                     uint16_t len,
                     uint64_t val) {
  uint8_t *wp; /* Write Pointer, points to current byte where data is being
                  extracted to. */
  int l = len;
  /* Last (highest address) byte of the field to extract. */
  wp = dst + ((offset + len - 1) / 8);

  while (l > 0) {
    *wp = val & 0xFF;
    val >>= 8;
    --wp;
    l -= 8;
  }
}

static void put_reverse_shifted_val(uint8_t *dst,
                                    uint16_t offset,
                                    uint16_t len,
                                    uint64_t val) {
  uint8_t *wp; /* Write Pointer, points to current byte where data is being
                  extracted to. */
  int l = len;
  int i = 0;
  /* Last (highest address) byte of the field to extract. */
  i = ((offset + len - 1) / 8);
  wp = dst + i;
  while (l > 0) {
    *wp = (val >> i * 8) & 0xFF;
    --wp;
    l -= 8;
    i--;
    if (i < 0) break;
  }
}

static void copy_bits_old(uint8_t *dst,
                          uint16_t dst_offset,
                          uint16_t dst_len,
                          uint8_t *src,
                          uint16_t src_offset,
                          uint16_t src_len,
                          uint16_t src_shift) {
  uint64_t vals[TBLPACK_WORD_BYTES_MAX] = {0};  // 1024 bits
  unsigned i = 0;

  get_shifted_val(src, src_offset, src_len, src_shift, vals);

  // Write the shifted source value to the destination.
  i = 0;
  while (dst_len) {
    uint16_t l = dst_len < 64 ? dst_len : 64;
    set_val_old(dst, dst_offset + 64 * i, l, vals[i]);
    dst_len -= l;
    ++i;
  }
}

static void copy_bits_old_opp(uint8_t *dst,
                              uint16_t dst_offset,
                              uint16_t dst_len,
                              uint16_t dst_shift,
                              uint8_t *src,
                              uint16_t src_offset,
                              uint16_t src_len) {
  uint64_t vals[TBLPACK_WORD_BYTES_MAX] = {0};  // 1024 bits
  int i = 0;

  get_reverse_shifted_val(src, src_offset, src_len, dst_shift, vals);

  // Write the shifted source value to the destination.
  i = 0;
  while (dst_len) {
    uint16_t l = dst_len < 64 ? dst_len : 64;
    put_reverse_shifted_val(dst, dst_offset + 64 * i, l, vals[i]);
    dst_len -= l;
    ++i;
  }
}

/* Src should be in big-endian.
 * Dst will be in little-endian
 */
static void copy_bits(uint8_t **dst,
                      uint8_t word_offset,
                      uint16_t dst_offset,
                      uint16_t dst_len,
                      uint8_t *src,
                      uint16_t src_offset,
                      uint16_t src_len,
                      uint16_t src_shift) {
  uint64_t vals[16] = {0};  // 1024 bits
  unsigned i = 0;

  get_shifted_val(src, src_offset, src_len, src_shift, vals);

  // Write the shifted source value to the destination.
  i = 0;
  while (dst_len) {
    uint16_t l = dst_len < 64 ? dst_len : 64;
    set_val(dst, word_offset, dst_offset + 64 * i, l, vals[i]);
    dst_len -= l;
    ++i;
  }
}

static void set_key_mask_default_encode(uint8_t *key,
                                        uint8_t *msk,
                                        uint16_t dst_offset,
                                        uint16_t dst_len,
                                        uint8_t *key_src,
                                        uint8_t *msk_src,
                                        uint16_t src_offset,
                                        uint16_t src_len,
                                        uint16_t src_shift,
                                        uint64_t range_mask) {
  /* Range mask fields should not be modified */
  /* Extract key and mask fields from src. */
  uint64_t k[16] = {0};
  uint64_t m[16] = {0};
  unsigned i = 0;

  get_shifted_val(key_src, src_offset, src_len, src_shift, k);
  get_shifted_val(msk_src, src_offset, src_len, src_shift, m);

  /* Convert key & mask to xy format.
   *   key/mask --> x/y
   *   0/0      --> 1/1
   *   0/1      --> 1/0
   *   1/0      --> 1/1
   *   1/1      --> 0/1 */
  uint64_t x[16];
  uint64_t y[16];
  unsigned j;
  for (j = 0; j < 16; ++j) {
    x[j] = ~(k[j] & m[j]);
    x[j] = (x[j] & ~range_mask) | (range_mask & k[j]);

    y[j] = k[j] | ~m[j];
    y[j] = (y[j] & ~range_mask) | (range_mask & m[j]);
  }

  /* Write converted key and mask to dst. */
  i = 0;
  while (dst_len) {
    uint16_t l = dst_len < 64 ? dst_len : 64;
    set_val_old(key, dst_offset + 64 * i, l, x[i]);
    set_val_old(msk, dst_offset + 64 * i, l, y[i]);
    dst_len -= l;
    ++i;
  }
}

static void set_key_mask_s1q0_encode(uint8_t **s1q0,
                                     uint8_t **s0q1,
                                     uint16_t word_offset,
                                     uint16_t dst_offset,
                                     uint16_t dst_len,
                                     uint8_t *key_src,
                                     uint8_t *msk_src,
                                     uint16_t src_offset,
                                     uint16_t src_len,
                                     uint16_t src_shift) {
  /* Extract key and mask fields from src. */
  uint64_t k[16] = {0};
  uint64_t m[16] = {0};
  unsigned i = 0;

  get_shifted_val(key_src, src_offset, src_len, src_shift, k);
  get_shifted_val(msk_src, src_offset, src_len, src_shift, m);

  /* Convert key & mask to s1q0/s0q1 format.
   *   key/mask --> s1q0/s0q1
   *   0/0      --> 1/0
   *   0/1      --> 0/0
   *   1/0      --> 1/0
   *   1/1      --> 1/1 */
  uint64_t x[16];
  uint64_t y[16];
  unsigned j;
  for (j = 0; j < 16; ++j) {
    if (s1q0) {
      x[j] = ~(~k[j] & m[j]);
    }

    if (s0q1) {
      y[j] = k[j] & m[j];
    }
  }

  /* Write converted key and mask to dst. */
  i = 0;
  while (dst_len) {
    uint16_t l = dst_len < 64 ? dst_len : 64;
    if (s1q0) {
      set_val(s1q0, word_offset, dst_offset + 64 * i, l, x[i]);
    }
    if (s0q1) {
      set_val(s0q1, word_offset, dst_offset + 64 * i, l, y[i]);
    }
    dst_len -= l;
    ++i;
  }
}

static void set_key_mask_no_encoding(uint8_t *key,
                                     uint8_t *msk,
                                     uint16_t dst_offset,
                                     uint16_t dst_len,
                                     uint8_t *key_src,
                                     uint8_t *msk_src,
                                     uint16_t src_offset,
                                     uint16_t src_len,
                                     uint16_t src_shift) {
  copy_bits_old(
      key, dst_offset, dst_len, key_src, src_offset, src_len, src_shift);
  copy_bits_old(
      msk, dst_offset, dst_len, msk_src, src_offset, src_len, src_shift);
}

/* For mask_2b | key_2b, the array index will give the
 * word_1_2b | word_0_2b
 */
static uint32_t tcam_key_mask_converter[16] = {
    0xf, /*0b1111, */
    0xf, /*0b1111, */
    0xf, /*0b1111, */
    0xf, /*0b1111, */
    0x5, /*0b0101, */
    0xa, /*0b1010, */
    0x5, /*0b0101, */
    0xa, /*0b1010, */
    0x3, /*0b0011, */
    0x3, /*0b0011, */
    0xc, /*0b1100, */
    0xc, /*0b1100, */
    0x1, /*0b0001, */
    0x2, /*0b0010, */
    0x4, /*0b0100, */
    0x8  /*0b1000  */
};
// static bool tcam_key_mask_converter_set = false;










































static void set_key_mask_2bit_mode(uint8_t *word0,
                                   uint8_t *word1,
                                   uint64_t range_mask,
                                   bool version) {
  /* Range mask fields should not be modified */
  /* Extract key and mask fields from src. */
  uint64_t k = 0;
  uint64_t m = 0;
  unsigned i;

  for (i = 0; i < 6; i++) {
    k |= (uint64_t)word0[i] << (8 * i);
    m |= (uint64_t)word1[i] << (8 * i);
  }

  uint64_t w1_val = 0, w0_val = 0;
  int p;
  /* Ignore the Payload/mrd bit */
  w0_val = k & 0x1;
  w1_val = m & 0x1;

  int match_key_end = 44;
  /* For tcam words that include the versioning bit, the most significant
   * nibble (versioning nibble) needs normal encoding as opposed to 2-bit
   * due to a hardware requirement.
   */
  if (version) {
    match_key_end = 40;
  }
  for (p = 1; p < match_key_end; p += 2) {
    /* For every 2 bits */
    uint64_t mask = (m >> p) & 0x3;
    uint64_t key = (k >> p) & 0x3;
    uint64_t a_index = (mask << 2) | key;

    uint64_t w0 = tcam_key_mask_converter[a_index] & 0x3;
    uint64_t w1 = (tcam_key_mask_converter[a_index] >> 2) & 0x3;

    w0_val |= w0 << p;
    w1_val |= w1 << p;
  }
  if (version) {
    /* Encode the last nibble using normal encoding:
     *     k/m ---> w0/w1
     *     0/1       1/0
     *     1/1       0/1
     *     -/0       1/1
     */
    for (p = 41; p < 45; p++) {
      uint64_t mask = (m >> p) & 0x1;
      uint64_t key = (k >> p) & 0x1;
      if (mask) {
        if (key) {
          w1_val |= 1ull << p;
        } else {
          w0_val |= 1ull << p;
        }
      } else {
        w0_val |= 1ull << p;
        w1_val |= 1ull << p;
      }
    }
  }

  w0_val = (w0_val & ~range_mask) | (range_mask & k);
  w1_val = (w1_val & ~range_mask) | (range_mask & m);

  /* Write converted key and mask back */
  set_val_old(word0, 0, 48, w0_val);
  set_val_old(word1, 0, 48, w1_val);
}

static void extract_key_mask_2bit_mode(uint8_t *word0,
                                       uint8_t *word1,
                                       uint64_t range_mask) {
  uint64_t w0_val = 0;
  uint64_t w1_val = 0;
  uint64_t orig_w0, orig_w1;
  unsigned i;

  for (i = 0; i < 6; i++) {
    w0_val |= (uint64_t)word0[i] << (8 * i);
    w1_val |= (uint64_t)word1[i] << (8 * i);
  }

  /* Mask out the range-masks */
  orig_w0 = w0_val;
  orig_w1 = w1_val;
  w0_val = w0_val | range_mask;
  w1_val = w1_val | range_mask;

  uint64_t k = 0, m = 0;
  int p;
  /* Ignore the Payload/mrd bit */
  k = w0_val & 0x1;
  m = w1_val & 0x1;
  for (p = 1; p < 44; p += 2) {
    /* For every 2 bits */
    uint64_t w0 = (w0_val >> p) & 0x3;
    uint64_t w1 = (w1_val >> p) & 0x3;
    uint64_t val = (w1 << 2) | w0;

    uint64_t j;

    for (j = 0; j < sizeof(tcam_key_mask_converter) /
                        sizeof(tcam_key_mask_converter[0]);
         j++) {
      if (tcam_key_mask_converter[j] == val) {
        break;
      }
    }
    if (j ==
        sizeof(tcam_key_mask_converter) / sizeof(tcam_key_mask_converter[0])) {
      /* Invalid combination. Indicate by making mask-0, value-1 */
      j = 0x3; /*0b0011; */
    }

    k |= (j & 0x3) << p;
    m |= ((j >> 2) & 0x3) << p;
  }

  k = (k & ~range_mask) | (range_mask & orig_w0);
  m = (m & ~range_mask) | (range_mask & orig_w1);

  /* Write converted key and mask back. */
  set_val_old(word0, 0, 48, k);
  set_val_old(word1, 0, 48, m);
}

static void set_val_in_network_order(uint8_t *dst,
                                     uint16_t offset,
                                     uint16_t len,
                                     uint64_t val) {
  uint8_t *wp = NULL;
  uint8_t wm = 0;
  uint8_t wo = 0;
  uint16_t l = 0, l1 = 0, l2 = 0;
  wp = dst + offset / 8;
  wo = offset % 8;
  while (len) {
    if (wo) {
      if (len <= (8 - wo)) {
        l1 = len;
      } else {
        l1 = (8 - wo);
      }
      wm = ((1 << l1) - 1) << wo;
      l = l1;
    } else {
      if (len < 8) {
        l2 = len;
      } else {
        l2 = 8;
      }
      wm = ((1 << l2) - 1);
      l = l2;
    }
    if (wo) {
      *wp = ((*wp) & ~wm) | (val & ((1 << l) - 1)) << wo;
    } else {
      *wp = ((*wp) & ~wm) | (val & ((1 << l) - 1));
    }
    val = val >> l;
    wp--;
    wo = 0;
    len -= l;
  }
  return;
}

static void extract_key_mask_to_match_spec(uint8_t *word0,
                                           uint8_t *word1,
                                           uint32_t start_offset,
                                           uint16_t bitwidth,
                                           uint8_t *match_value_bits,
                                           uint8_t *match_mask_bits,
                                           uint16_t src_offset,
                                           bool version_word) {
  unsigned i = 0;
  unsigned l = 0;
  uint64_t w0_val = 0;
  uint64_t w1_val = 0;
  uint64_t k = 0, m = 0;
  bool shift = false;
  uint16_t shift_len = bitwidth;
  for (i = 0; i < 6; i++) {
    w0_val |= (uint64_t)word0[i] << (8 * i);
    w1_val |= (uint64_t)word1[i] << (8 * i);
  }

  if (version_word && (start_offset == 41 || start_offset == 42)) {
    /* Decode container validity bits using normal encoding
     *    w0/w1 ---> k/m
     *     1/0       0/1
     *     0/1       1/1
     *     1/1       -/0
     */
    uint64_t w0 = (w0_val >> start_offset) & 0x1;
    uint64_t w1 = (w1_val >> start_offset) & 0x1;
    if (w0) {
      if (w1) {
        m = 0;
      } else {
        m = 1;
      }
    } else {
      k = 1;
      m = 1;
    }
  } else {
    if (start_offset % 2 == 0 && start_offset > 0) {
      /* 2-bit encoding starts at odd offsets */
      start_offset--;
      shift_len++;
      shift = true;
    }
    for (i = start_offset, l = 0; i < start_offset + shift_len;
         i += 2, l += 2) {
      uint64_t w0 = (w0_val >> i) & 0x3;
      uint64_t w1 = (w1_val >> i) & 0x3;
      uint64_t val = (w1 << 2) | w0;
      uint64_t j;

      for (j = 0; j < sizeof(tcam_key_mask_converter) /
                          sizeof(tcam_key_mask_converter[0]);
           j++) {
        if (tcam_key_mask_converter[j] == val) {
          break;
        }
      }
      if (j == sizeof(tcam_key_mask_converter) /
                   sizeof(tcam_key_mask_converter[0])) {
        /* Invalid combination. Indicate by making mask-0, value-1 */
        j = 0x3; /*0b0011; */
      }

      k |= (j & 0x3) << l;
      m |= ((j >> 2) & 0x3) << l;
    }
    if (shift) {
      /* Ignore the extra bit if the original offset was even */
      k >>= 1;
      m >>= 1;
    }
  }

  if (match_value_bits && match_mask_bits) {
    set_val_in_network_order(match_value_bits, src_offset, bitwidth, k);
    set_val_in_network_order(match_mask_bits, src_offset, bitwidth, m);
  }
  return;
}

static void set_range(uint8_t *key,        /* TCAM word0 */
                      uint8_t *msk,        /* TCAM word1 */
                      uint16_t dst_offset, /* Offset in TCAM word */
                      uint16_t dst_len,    /* Length in TCAM word */
                      uint8_t *key_src,    /* Match spec value (range start) */
                      uint8_t *msk_src,    /* Match spec mask (range end) */
                      uint16_t src_offset, /* Offset within the match spec */
                      uint16_t src_len,    /* Length within the match spec */
                      uint16_t src_shift,  /* Field start bit */
                      uint8_t range_width, /* 2 or 4 bit range encoding */
                      uint8_t hi_byte) {
  PIPE_MGR_DBGCHK(dst_len <= 4);
  PIPE_MGR_DBGCHK((range_width == 2) || (range_width == 4));
  PIPE_MGR_DBGCHK(dst_len <= range_width);
  /* Extract key and mask fields from src. */
  uint64_t k[16] = {0};
  uint64_t m[16] = {0};
  /* Convert the offset within the TCAM word to an offset with a nibble (4-bit
   * range) or bit pair (2-bit range).  The extra -1 is due to the fact that bit
   * zero in both TCAM words has special meaning and is not part of normal
   * match data. */
  uint64_t offset = (dst_offset - 1) % range_width;
  uint64_t base =
      1 + ((uint64_t)range_width * ((dst_offset - 1) / range_width));

  /* Extract our source field from the match spec into local arrays. */
  get_shifted_val(key_src, src_offset, src_len, src_shift, k);
  get_shifted_val(msk_src, src_offset, src_len, src_shift, m);

  /* Create a mask to keep only bits that will be encoded now and filter our
   * values to encode with it. */
  uint64_t range_mask = (1ull << dst_len) - 1;
  uint64_t start = k[0] & range_mask;
  uint64_t end = m[0] & range_mask;

  uint64_t x = 0, y = 0, val = 0, range_val = 0;

  /* Since each nibble/bit-pair can hold multiple range fields, we need to
   * perform a read-modify-write on the relevant location of word0/1 to
   * preserve the bits not belonging to this range field.
   */
  x = get_val(key, base, range_width);
  y = get_val(msk, base, range_width);

  /* Two bit and four bit range both use a one hot encoding for the values to
   * be matched.  For example, 4 bit range uses 8 bits of W0 and 8 bits of W1
   * to give 16 bits in total.  The four bits of key can take values 0..15 so
   * if a value should be included in the match the corresponding bit in W0/W1
   * would be set.  W0 covers the lower values and W1 the upper.  We also encode
   * a nibble at a time so a given nibble would represent values 0-3 (in W0) and
   * 8-11 (in W1) or 4-7 and 12-15 depending on if "hi_byte" is 0 or 1.
   */

  /* Go over the 4, or 2, bits of this W0 entry and check if each value that bit
   * represents is in the range or out of the range.  If it is out of the range,
   * unset the bit in W0. */
  for (unsigned i = 0; i < range_width; i++) {
    /* val is the value bit i in the nibble represents. */
    val = (hi_byte * range_width) + i;
    /* Since the field might not start at bit zero of the nibble/bit-pair,
     * shift it down to get the value it represents for the field. */
    range_val = (val >> offset) & range_mask;
    if ((range_val < start) || (range_val > end)) {
      x &= ~(1 << i);
    }
  }
  /* Do the same for W1... */
  for (unsigned i = 0; i < range_width; i++) {
    /* Since this is W1 it has the high half of the range values. For two-bit
     * range this is 2 and 3, for 4 bit range this is 8-15. */
    if (range_width == 2) {
      val = 2 + i;
    } else if (range_width == 4) {
      val = 8 + (hi_byte * range_width) + i;
    } else {
      PIPE_MGR_DBGCHK(0);
    }
    range_val = (val >> offset) & range_mask;
    if ((range_val < start) || (range_val > end)) {
      y &= ~(1 << i);
    }
  }

  /* Write updated values back into word0/1 */
  set_val_old(key, base, range_width, x);
  set_val_old(msk, base, range_width, y);
}

static void decode_range(uint8_t *key_src,
                         uint8_t *msk_src,
                         uint16_t src_offset,
                         uint16_t src_len,
                         uint16_t *range_bitval,
                         uint64_t *range_field0,
                         uint64_t *range_field1,
                         uint8_t range_width,
                         uint8_t hi_byte) {
  PIPE_MGR_DBGCHK(src_len <= 4);
  PIPE_MGR_DBGCHK((range_width == 2) || (range_width == 4));
  uint64_t k[TBLPACK_WORD_BYTES_MAX] = {0};
  uint64_t m[TBLPACK_WORD_BYTES_MAX] = {0};
  uint64_t offset = (src_offset - 1) % range_width;
  uint16_t normalized_src_offset = src_offset - offset;

  /* Extract the value at range_width * ((dst_offset-1)/range_width)
   * of size range_width
   */
  get_shifted_val_decode(key_src, normalized_src_offset, range_width, k);
  get_shifted_val_decode(msk_src, normalized_src_offset, range_width, m);

  *range_field0 = k[0];
  *range_field1 = m[0];

  *range_bitval = 0;

  uint64_t range_mask = (1ull << src_len) - 1;
  uint32_t i;
  uint32_t val, bit_val;
  for (i = 0; i < range_width; i++) {
    val = (hi_byte * range_width) + i;
    if (k[0] & (1 << i)) {
      /* Figure out the value of the range bits in this value */
      bit_val = (val >> offset) & range_mask;
      *range_bitval |= 1 << bit_val;
    }
  }

  for (i = 0; i < range_width; i++) {
    if (range_width == 2) {
      val = 2 + i;
    } else if (range_width == 4) {
      val = 8 + (hi_byte * range_width) + i;
    } else {
      PIPE_MGR_DBGCHK(0);
    }
    if (m[0] & (1 << i)) {
      /* Figure out the value of the range bits in this value */
      bit_val = (val >> offset) & range_mask;
      *range_bitval |= 1 << bit_val;
    }
  }
}

static uint8_t get_byte_reverse_decode(uint8_t *src,
                                       uint16_t offset,
                                       uint8_t len) {
  PIPE_MGR_DBGCHK(len <= 8);
  uint8_t ret = 0;
  int start_byte_index = offset / 8;
  int bit_offset_in_first_byte = offset - start_byte_index * 8;
  int num_bits_in_first_byte = 8 - bit_offset_in_first_byte;
  ret = src[start_byte_index] >> bit_offset_in_first_byte;
  /* We need to mask in the event that len was small and we didn't read all the
   * way to the msb of the byte. */
  if (num_bits_in_first_byte > len) {
    num_bits_in_first_byte = len;
    ret &= (1 << num_bits_in_first_byte) - 1;
  }

  offset += num_bits_in_first_byte;
  len -= num_bits_in_first_byte;

  /* The field may run into the neighboring byte, pull those bits out now and
   * tack them onto the msb end of our value. */
  if (len) {
    /* We must be byte aligned now. */
    if (!((offset % 8) == 0)) {
      PIPE_MGR_DBGCHK(0);
    }
    uint8_t x = src[start_byte_index + 1];
    x &= (1 << len) - 1;
    ret |= x << num_bits_in_first_byte;
  }
  return ret;
}

/* Extract bytes from the lsb end of src towards the msb end placing them in a
 * uint64_t to return in reverse order.  We make the first extraction byte
 * aligned in the MSB of the destination and the last extraction is padded with
 * zeros to fill the LSB of the destination.
 * Given an input of:
 * {0x1, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x1, 0x23, 0x45, 0x67, 0x89,
 * 0xab, 0xcd, 0xef}
 * with offset = 44 and length 20 we would return 0xdafc0e */
static uint64_t get_val_reverse_decode(uint8_t *src,
                                       uint16_t offset,
                                       uint16_t len) {
  uint64_t ret = 0;
  int total_bytes = (len + 7) / 8;
  int dst_byte_idx = total_bytes - 1;
  while (len) {
    uint16_t amount_to_read = len > 8 ? 8 : len;
    uint64_t x = get_byte_reverse_decode(src, offset, amount_to_read);
    len -= amount_to_read;
    offset += amount_to_read;
    ret |= x << (8 * dst_byte_idx);
    --dst_byte_idx;
  }
  return ret;
}

static uint64_t get_val_decode(uint8_t **src,
                               uint16_t word_offset,
                               uint16_t offset,
                               uint16_t len) {
  uint8_t *rp; /* Read Pointer, points to current byte where data is being
                  extracted. */
  uint8_t *rp_next;
  uint16_t cnt = 0, max_cnt = 0;
  uint64_t val = 0;
  uint8_t val_next_offset = 0, off_mask = 0;
  uint8_t off_rem = offset % 8;
  /* x & x_next get shifted around more than 32 bits, hence define it to be
   * a 64 bit value.
   */
  uint64_t x, x_next;
  /* Last (highest address) byte of the field to extract. */
  rp = src[word_offset] + offset / 8;
  uint16_t rp_word_iter = 0;
  uint16_t rp_next_word_iter = 0;
  /* Figure out where the next byte to read is.  If at the end of the 16B array
   * then the next byte is in the next array.  If we will only read a single
   * byte then fall back to the else case as src[word_offset+1] might not be
   * there. */
  if ((off_rem + len) > 8 && (offset / 8 + 1) > 15) {
    rp_next = src[word_offset + 1];
    rp_next_word_iter++;
  } else {
    rp_next = rp + 1;
  }
  max_cnt = len / 8;
  if ((len % 8) > 0) max_cnt++;
  // printf("OFFSET %d, LEN %d \n", offset, len);
  while (cnt < max_cnt) {
    x = *rp;
    if (off_rem > 0) {
      /* Before accessing rp_next, ensure we don't overrun the passed in array,
       * this calculation takes the remaining number of bits and checks if it
       * exceeds the given byte
       */
      if ((len - (8 * cnt)) > (8 - off_rem)) {
        x_next = *rp_next;
      } else {
        x_next = 0;
      }
      off_mask = 0xff >> (8 - off_rem);
      // printf("x %x, x_next %x, off_rem %d, off_mask %x \n",
      //    x, x_next, off_rem, off_mask);
      val_next_offset = x_next & off_mask;
      val_next_offset = val_next_offset << (8 - off_rem);
      val |= ((x >> off_rem) | val_next_offset) << (8 * cnt);
      // printf("cnt %d, x %x, x_next %x, val %"PRIx64", val_next_offset %x \n",
      //        cnt, x, x_next, val, val_next_offset);
    } else {
      val |= x << (8 * cnt);
    }
    ++cnt;
    if (cnt == max_cnt) {
      break;
    }
    ++rp;
    if (rp - src[word_offset + rp_word_iter] > 15) {
      rp_word_iter++;
      rp = src[word_offset + rp_word_iter];
    }
    ++rp_next;
    if (rp_next - src[word_offset + rp_next_word_iter] > 15) {
      if (cnt < max_cnt - 1) {
        // Ensure we don't overrun the passed in array
        rp_next_word_iter++;
        rp_next = src[word_offset + rp_next_word_iter];
      }
    }
  }

  uint64_t mask = 0ull;
  mask = (len == 64) ? ~0ull : ((1ULL << len) - 1);
  return val & mask;
}

static uint64_t get_val(uint8_t *src, uint16_t offset, uint16_t len) {
  return get_val_reverse_decode(src, offset, len);
}

static uint64_t extract_val(uint8_t *src, uint16_t offset, uint16_t len) {
  uint8_t *rp = src + offset / 8;
  unsigned int x = 0;
  uint16_t num_bits_remaining = len;
  uint8_t write_mask = 0xff;
  uint8_t num_bits_this_time = 0;
  bool partial_byte = offset % 8;
  uint64_t val = 0;
  uint8_t shift = 0;
  uint8_t num_bits_written = 0;
  while (num_bits_remaining) {
    if (partial_byte) {
      num_bits_this_time = (num_bits_remaining < (8 - (offset % 8)))
                               ? num_bits_remaining
                               : (8 - (offset % 8));
      write_mask = (1 << num_bits_this_time) - 1;
      write_mask <<= (offset % 8);
      shift = offset % 8;
    } else {
      num_bits_this_time = (num_bits_remaining < 8) ? num_bits_remaining : 8;
      write_mask = (1 << num_bits_this_time) - 1;
      shift = 0;
    }
    x = ((*rp) & write_mask) >> shift;
    val |= ((uint64_t)x << num_bits_written);
    num_bits_remaining -= num_bits_this_time;
    partial_byte = false;
    num_bits_written += num_bits_this_time;
    rp++;
  }
  return val;
}

static void get_shifted_val_decode(uint8_t *src,
                                   uint16_t offset,
                                   uint16_t len,
                                   uint64_t *vals) {
  unsigned i = 0;

  // Read out the entire source into the vals array.
  while (len) {
    uint16_t l = len < 64 ? len : 64;
    vals[i] = get_val_reverse_decode(src, offset + len - l, l);
    // printf("Vals[%d]=0x%"PRIx64" \n", i, vals[i]);
    len -= l;
    ++i;
  }
}

static void get_val_decode_to_match_spec(uint8_t **src,
                                         uint16_t word_offset,
                                         uint16_t offset,
                                         uint16_t len,
                                         uint64_t *vals) {
  unsigned i = 0;

  // Read out the entire source into the vals array.
  while (len) {
    uint16_t l = len < 64 ? len : 64;
    vals[i] = get_val_decode(src, word_offset, offset + 64 * i, l);
    // printf("Vals[%d]=0x%"PRIx64" \n", i, vals[i]);
    len -= l;
    ++i;
  }
}

static void decode_range_to_match_spec(
    uint8_t *key_src,    /* TCAM word0 */
    uint8_t *mask_src,   /* TCAM word1 */
    uint16_t src_offset, /* Offset in the match spec */
    uint16_t src_len,    /* Length in the match spec */
    uint16_t src_shift,  /* Field start bit */
    uint16_t dst_offset, /* Start bit in the TCAM word */
    uint16_t dst_len,    /* Length (number of bits) in the TCAM word */
    uint8_t range_width, /* 2 or 4 bit range encoding */
    uint8_t hi_byte,     /* 4 bit range: which nibble 0-3/8-11 or 4-7/12-15 */
    uint8_t *match_value_bits,  /* Match spec value (range start) */
    uint8_t *match_mask_bits) { /* Mactch spec mask (range end) */
  PIPE_MGR_DBGCHK(dst_len <= 4);
  PIPE_MGR_DBGCHK((range_width == 2) || (range_width == 4));
  PIPE_MGR_DBGCHK(dst_len <= range_width);

  /* Get the offset within the range block (nibble or bit pair) for this field.
   */
  int offset = (dst_offset - 1) % range_width;
  int range_mask = (1 << dst_len) - 1;

  /* Get our existing range start and end values from the match spec since we
   * are decoding piece by piece.  The range start and end are stored in the
   * match spec and hence are padded (on msb end) to a byte boundry and stored
   * in network order. */
  int spec_start_byte = src_offset / 8;
  int spec_num_bytes = (src_len + 7) / 8;
  uint64_t full_start = 0;
  uint64_t full_end = 0;
  for (int i = 0, j = spec_start_byte; i < spec_num_bytes; ++i, ++j) {
    full_start = (full_start << 8) | match_value_bits[j];
    full_end = (full_end << 8) | match_mask_bits[j];
  }
  /* Extract the start and end related to the section of the field we are
   * currently decoding. */
  uint64_t existing_start = (full_start >> src_shift) & ((1 << dst_len) - 1);
  uint64_t existing_end = (full_end >> src_shift) & ((1 << dst_len) - 1);

  /* Extract our nibble (or bit pair) from the TCAM word. */
  int tcam_word_start_bit = dst_offset - offset;
  uint8_t x = get_val(key_src, tcam_word_start_bit, range_width);
  uint8_t y = get_val(mask_src, tcam_word_start_bit, range_width);

  int start = -1, end = -1;

  /* Determine which values our nibble matches by finding the msb and lsb set in
   * the relevant locations of the nibble. We can iterate by step here, since
   * we only care about bits starting from offset. */
  int step = 1 << offset;
  if (x) {
    int lsb = -1, msb = 0;
    for (unsigned i = 0; i < range_width; i += step) {
      /* Since other ranges occupying the lower bits may have turned off some
       * bits, accept this step iteration if any of the lower bits are on.
       */
      for (unsigned j = i; j < i + step; j++) {
        if (x & (1u << j)) {
          msb = i;
          if (lsb == -1) lsb = i;
          break;
        }
      }
    }
    start = lsb + (hi_byte * range_width);
    end = msb + (hi_byte * range_width);
  }
  if (y) {
    int lsb = -1, msb = 0;
    for (unsigned i = 0; i < range_width; i += step) {
      for (unsigned j = i; j < i + step; j++) {
        if (y & (1u << j)) {
          msb = i;
          if (lsb == -1) lsb = i;
          break;
        }
      }
    }
    /* Word 1 has the higher values so don't overwrite start if it was already
     * set from tcam word0. */
    if (range_width == 2) {
      if (start == -1) start = 2 + lsb;
      end = 2 + msb;
    } else if (range_width == 4) {
      if (start == -1) start = 8 + lsb + (hi_byte * range_width);
      end = 8 + msb + (hi_byte * range_width);
    }
  }

  /* Account for fields that do not start at the beginning of the
   * nibble/bit-pair by shifting the value the tcam entry would match by the
   * offset within the nibble.  For example, if a 4 bit range entry had a
   * field starting at bit 2 of the nibble then a search-data (key) value of 0
   * would still map to a match value of 0 but a search-data (key) value of 1
   * would map to a match value of 4.
   */
  if (start != -1) start = (start >> offset) & range_mask;
  if (end != -1) end = (end >> offset) & range_mask;

  if (start != -1) {
    /* Currently, we always process the hi_byte (which is actually the higher
     * nibble) first and then the low_byte.
     * When we process the hi_byte, we either find a valid start or not. Invalid
     * start is when the nibble did not have any bit set. In which case the
     * start and end will not be encoded back into the match spec.
     * Lets consider low_byte processing, here is what can happen.
     * (we are here because we found a valid start for the low_byte).
     *  1. A valid start is from word0, this will be the new start regardless
     *     of what has been found in hi_byte.
     *  2. A valid start is word1, this will be the new start if
     *        a. This start value is lesser than what has been found in
     *           hi_byte. If a valid start was found in hi_byte, we can just
     *           compare. If a valid start was not found in hi_byte,
     *           existing_start will be zero. */
    if (!hi_byte) {
      if (existing_start == 0 || (uint64_t)start < existing_start)
        existing_start = start;
    } else {
      existing_start = start;
    }
  }
  /* Updating the end is a bit simpler... */
  if (end != -1 && (uint64_t)end > existing_end) {
    existing_end = end;
  }

  /* Encode the start and end back into the match spec. */
  uint64_t msk = ((uint64_t)1 << dst_len) - 1;
  uint64_t clr_msk = ~(msk << src_shift);
  full_start = (full_start & clr_msk) | ((existing_start & msk) << src_shift);
  full_end = (full_end & clr_msk) | ((existing_end & msk) << src_shift);
  for (int i = 0, j = spec_start_byte + spec_num_bytes - 1; i < spec_num_bytes;
       ++i, --j) {
    match_value_bits[j] = (full_start >> (i * 8)) & 0xFF;
    match_mask_bits[j] = (full_end >> (i * 8)) & 0xFF;
  }

  return;
}

/* Src should be in little-endian format
 * Dst will be in big-endian format
 */
static void decode_copy_bits(uint8_t *dst,
                             uint16_t dst_offset,
                             uint16_t dst_len,
                             uint8_t *src,
                             uint16_t src_offset,
                             uint16_t src_len) {
  uint64_t vals[TBLPACK_WORD_BYTES_MAX] = {0};  // 1024 bits
  unsigned i = 0;
  uint16_t off_val = 0;

  get_shifted_val_decode(src, src_offset, src_len, vals);

  // Write the shifted source value to the destination.
  i = 0;
  while (dst_len) {
    uint16_t l = dst_len < 64 ? dst_len : 64;
    if ((dst_len < 64) && ((dst_len / 8) > 0)) {
      off_val = (8 - (dst_len % 8));
    } else {
      off_val = 0;
    }
    set_val_old(dst, dst_offset + 64 * i, l + off_val, vals[i]);
    dst_len -= l;
    ++i;
  }
}

static void decode_copy_bits_snapshot(uint8_t *dst,
                                      uint16_t dst_offset,
                                      uint16_t dst_len,
                                      uint8_t *src,
                                      uint16_t src_offset,
                                      uint16_t src_len) {
  uint64_t vals[TBLPACK_WORD_BYTES_MAX] = {0};  // 1024 bits
  unsigned i = 0;

  get_shifted_val_decode(src, src_offset, src_len, vals);

  // Write the shifted source value to the destination.
  i = 0;
  while (dst_len) {
    uint16_t l = dst_len < 64 ? dst_len : 64;
    set_val_old(dst, dst_offset + 64 * i, l, vals[i]);
    dst_len -= l;
    ++i;
  }
}

static void decode_bits_to_spec(uint8_t *dst,
                                uint16_t dst_offset,
                                uint16_t dst_len,
                                uint8_t **src,
                                uint16_t src_word_offset,
                                uint16_t src_offset,
                                uint16_t src_len) {
  uint64_t vals[TBLPACK_WORD_BYTES_MAX] = {0};  // 1024 bits
  unsigned i = 0;
  if (!dst) return;
  get_val_decode_to_match_spec(src, src_word_offset, src_offset, src_len, vals);
  // Write the shifted source value to the destination.
  i = 0;
  while (dst_len) {
    uint16_t l = dst_len < 64 ? dst_len : 64;
    set_val_reverse(dst, dst_offset - 64 * i, l, vals[i]);
    dst_len -= l;
    ++i;
  }
}

/*
 * s1q0 and s0q1 should be in big-endian format - because generated from
 * decode_copy_bits
 * key and mask will be in little-endian format
 */
static void decode_key_mask_s1q0_encode(uint8_t *key,
                                        uint8_t *msk,
                                        uint16_t dst_offset,
                                        uint16_t dst_len,
                                        uint8_t *s1q0,
                                        uint8_t *s0q1,
                                        uint16_t src_offset,
                                        uint16_t src_len,
                                        uint16_t src_shift) {
  /* Extract key and mask fields from src. */
  uint64_t k[TBLPACK_WORD_BYTES_MAX] = {0};
  uint64_t m[TBLPACK_WORD_BYTES_MAX] = {0};
  unsigned j = 0;
  uint64_t x[TBLPACK_WORD_BYTES_MAX];
  uint64_t y[TBLPACK_WORD_BYTES_MAX];

  get_shifted_val(s1q0, src_offset, src_len, src_shift, k);
  get_shifted_val(s0q1, src_offset, src_len, src_shift, m);

  /* Convert s1q0/s0q1 to key & mask format.
   *   key/mask <-- s1q0/s0q1
   *   0/0      <-- 1/0
   *   0/1      <-- 0/0
   *   0/0      <-- 1/0
   *   1/1      <-- 1/1 */
  for (j = 0; j < TBLPACK_WORD_BYTES_MAX; ++j) {
    x[j] = k[j] & m[j];
    y[j] = ~(k[j] ^ m[j]);
  }

  /* Now put it back in little-endian format */
  j = 0;
  while (dst_len) {
    uint16_t l = dst_len < 64 ? dst_len : 64;
    if (key) {
      set_val_old(key, dst_offset + 64 * j, l, x[j]);
    }
    if (msk) {
      set_val_old(msk, dst_offset + 64 * j, l, y[j]);
    }
    dst_len -= l;
    ++j;
  }
}

static void decode_key_mask_no_encode(uint8_t *key,
                                      uint8_t *msk,
                                      uint16_t dst_offset,
                                      uint16_t dst_len,
                                      uint8_t *key_src,
                                      uint8_t *msk_src,
                                      uint16_t src_offset,
                                      uint16_t src_len) {
  /* Extract key and mask fields from src. */
  uint64_t k[TBLPACK_WORD_BYTES_MAX] = {0};
  uint64_t m[TBLPACK_WORD_BYTES_MAX] = {0};
  unsigned j = 0;
  uint16_t off_val = 0;

  get_shifted_val_decode(key_src, src_offset, src_len, k);
  get_shifted_val_decode(msk_src, src_offset, src_len, m);

  j = 0;
  while (dst_len) {
    uint16_t l = dst_len < 64 ? dst_len : 64;
    if ((dst_len < 64) && ((dst_len % 8) > 0)) {
      off_val = (8 - (dst_len % 8));
    } else {
      off_val = 0;
    }
    set_val_old(key, dst_offset + 64 * j, l + off_val, k[j]);
    set_val_old(msk, dst_offset + 64 * j, l + off_val, m[j]);
    dst_len -= l;
    ++j;
  }
}

void print_tcam_in_key_mask_fmt(uint64_t new_key,
                                uint64_t new_mask,
                                char *str,
                                int *c_str_len,
                                int max_len,
                                int width) {
  int c_len = *c_str_len;
  char buf[TBLPACK_LONG_STR_LEN];
  int i = 0;
  char *ptr = buf;
  char *end = buf + TBLPACK_LONG_STR_LEN;
  uint8_t *data_ptr;
  uint64_t upd_key = 0, upd_mask = 0;

  PIPE_MGR_MEMSET(buf, 0, sizeof(buf));

  /* Remove bits 0, 45-48 from key */
  upd_key = new_key;
  upd_key = upd_key >> 1;
  upd_key = upd_key & 0x0fffffffffff;

  data_ptr = (uint8_t *)&upd_key;
  for (i = width / 8 - 1; i >= 0; i--) {
    ptr += snprintf(ptr, end > ptr ? (end - ptr) : 0, "%02x ", data_ptr[i]);
  }

  /* Remove bits 0, 45-48 from mask */
  upd_mask = new_mask;
  upd_mask = upd_mask >> 1;
  upd_mask = upd_mask & 0x0fffffffffff;

  ptr += snprintf(ptr, end > ptr ? (end - ptr) : 0, "\n%-15s", " ");

  data_ptr = (uint8_t *)&upd_mask;
  for (i = width / 8 - 1; i >= 0; i--) {
    ptr += snprintf(ptr, end > ptr ? (end - ptr) : 0, "%02x ", data_ptr[i]);
  }
  c_len += snprintf(
      str + c_len, (c_len < max_len) ? (max_len - c_len - 1) : 0, "%s", buf);

  *c_str_len = c_len;
}

void print_field_value_in_bytes_fmt(uint64_t value,
                                    int width,
                                    bool endline,
                                    char *str,
                                    int *c_str_len,
                                    int max_len) {
  int num_bytes = 0;
  int i = 0;
  int c_len = *c_str_len;
  uint8_t *data_ptr;

  num_bytes = width / 8;
  if ((width % 8) > 0) num_bytes++;

  data_ptr = (uint8_t *)&value;
  for (i = 0; i < num_bytes; i++) {
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "%02x ",
                      data_ptr[i]);
  }
  if (endline) {
    c_len += snprintf(
        str + c_len, (c_len < max_len) ? (max_len - c_len - 1) : 0, "\n");
  }

  *c_str_len = c_len;
}

void dump_tbl_word(uint8_t *word_ptr,
                   int offset,
                   char *str,
                   int *c_len,
                   int max_len,
                   int index,
                   int width) {
  char buf[TBLPACK_SMALL_STR_LEN];
  int j = 0;
  char *ptr = buf;
  char *end = buf + TBLPACK_SMALL_STR_LEN;
  uint8_t *data_ptr;

  PIPE_MGR_MEMSET(buf, 0, sizeof(buf));

  data_ptr = (uint8_t *)word_ptr;
  data_ptr = data_ptr + offset;
  for (j = width / 8 - 1; (data_ptr) && (j >= 0); j--) {
    /* Print every byte */
    ptr += snprintf(ptr, end > ptr ? (end - ptr) : 0, "%02x ", data_ptr[j]);
  }

  if (str) {
    int start = index * width;
    int end_pos = start + width - 1;
    *c_len += snprintf(str + (*c_len),
                       (*c_len < max_len) ? (max_len - (*c_len) - 1) : 0,
                       "Value(%-3d..%-3d): %s\n",
                       end_pos,
                       start,
                       buf);
  }
}

/*        -------------------
 * Entry Formatting using datastructures built after parsing
 * compiler generated json.
 *        -------------------
 */

static pipemgr_tbl_pkg_lut_t *pipemgr_entry_format_get_lut_entry(
    uint32_t bj_lut_index,
    uint16_t table_depth,
    pipemgr_tbl_pkg_lut_t *tbl_lut_ptr,
    uint32_t hndl,
    uint8_t stage) {
  if (!tbl_lut_ptr) return NULL;
  pipemgr_tbl_pkg_lut_t *lut_ptr = tbl_lut_ptr + bj_lut_index;
  int k = 0;

  while (k < PIPEMGR_TBL_PKG_HASH_COLLISION_FACTOR &&
         ((lut_ptr->tbl_hndl != hndl) || (lut_ptr->stage != stage))) {
    // Handle hash collison...
    lut_ptr += table_depth;
    k++;
  }

  if (k >= PIPEMGR_TBL_PKG_HASH_COLLISION_FACTOR) {
    // Key Doesn't exist
    LOG_ERROR(
        "Entry formatting Error: unable to find (table handle, stage) in "
        "lookup table. handle=0x%x, stage=%d",
        hndl,
        stage);
    return (NULL);
  }

  return (lut_ptr);
}

static pipemgr_tbl_pkg_lut_t *pipemgr_entry_format_get_lut_entry_no_logging(
    uint32_t bj_lut_index,
    uint16_t table_depth,
    pipemgr_tbl_pkg_lut_t *tbl_lut_ptr,
    uint32_t hndl,
    uint8_t stage) {
  if (!tbl_lut_ptr) {
    return NULL;
  }
  pipemgr_tbl_pkg_lut_t *lut_ptr = tbl_lut_ptr + bj_lut_index;
  int k = 0;

  while (k < PIPEMGR_TBL_PKG_HASH_COLLISION_FACTOR &&
         ((lut_ptr->tbl_hndl != hndl) || (lut_ptr->stage != stage))) {
    // Handle hash collison...
    lut_ptr += table_depth;
    k++;
  }

  if (k >= PIPEMGR_TBL_PKG_HASH_COLLISION_FACTOR) {
    // Learn init queries all lrn_cfg_type in all pipes.
    // Not all p4 program will have all learn-cfg-type.
    // Do not log any error.
    return (NULL);
  }

  return (lut_ptr);
}

static pipemgr_tbl_pkg_exm_stage_hdl_format_t *
pipemmgr_entry_format_get_stage_handle_details_ptr(
    uint8_t stage_table_handle, pipemgr_tbl_pkg_lut_t *lut_ptr) {
  uint32_t i;
  pipemgr_tbl_pkg_exm_format_t *format_details_ptr = lut_ptr->u.exm_ptr;
  pipemgr_tbl_pkg_exm_stage_hdl_format_t *stg_hdl_ptr =
      format_details_ptr->stg_hdl;
  uint16_t way_count, entry_count, field_count;
  uint16_t stash_way_count, stash_entry_count, stash_field_count;

  // Get pointer entry format details memory area for stage_table_handle
  for (i = 0; i < format_details_ptr->stage_handle_count; i++) {
    if (stg_hdl_ptr->stage_handle == stage_table_handle) {
      break;
    }
    way_count = format_details_ptr->stg_hdl->way_count;
    entry_count = format_details_ptr->stg_hdl->total_entries_in_all_ways;
    field_count = format_details_ptr->stg_hdl->total_fields_in_all_ways;
    stash_way_count = format_details_ptr->stg_hdl->stash_way_count;
    stash_entry_count =
        format_details_ptr->stg_hdl->stash_total_entries_in_all_ways;
    stash_field_count =
        format_details_ptr->stg_hdl->stash_total_fields_in_all_ways;
    stg_hdl_ptr++;
    stg_hdl_ptr =
        (pipemgr_tbl_pkg_exm_stage_hdl_format_t
             *)((uint8_t *)stg_hdl_ptr +
                (entry_count * sizeof(pipemgr_tbl_pkg_match_entry_line_t)) +
                (field_count * sizeof(pipemgr_tbl_pkg_match_entry_field_t)) +
                (way_count * (sizeof(pipemgr_tbl_pkg_way_format_t) +
                              sizeof(pipemgr_tbl_pkg_match_entry_format_t))) +

                (stash_entry_count *
                 sizeof(pipemgr_tbl_pkg_match_entry_line_t)) +
                (stash_field_count *
                 sizeof(pipemgr_tbl_pkg_match_entry_field_t)) +
                (stash_way_count *
                 sizeof(pipemgr_tbl_pkg_match_entry_format_t)));
  }
  if (i >= format_details_ptr->stage_handle_count) {
    return (NULL);
  } else {
    return (stg_hdl_ptr);
  }
}

static pipemgr_tbl_pkg_action_entry_field_t *
pipemmgr_entry_format_get_action_handle_details_ptr(
    uint32_t act_fn_hdl, pipemgr_tbl_pkg_lut_t *lut_ptr) {
  uint32_t i;
  pipemgr_tbl_pkg_action_handles_t *action_details_ptr;
  pipemgr_tbl_pkg_action_entry_field_t *action_hdl_ptr;

  action_details_ptr =
      (pipemgr_tbl_pkg_action_handles_t *)((uint8_t *)(lut_ptr->u.exm_ptr) +
                                           lut_ptr->match_field_mem_size);
  action_hdl_ptr = action_details_ptr->action_hdl;
  for (i = 0; i < action_details_ptr->action_hdl_count; i++) {
    if (action_hdl_ptr->action_handle == act_fn_hdl) {
      break;
    }
    action_hdl_ptr =
        (pipemgr_tbl_pkg_action_entry_field_t
             *)((uint8_t *)action_hdl_ptr +
                sizeof(pipemgr_tbl_pkg_action_entry_field_t) +
                (action_hdl_ptr->param_count *
                 sizeof(pipemgr_tbl_pkg_action_parameter_field_t)));
  }
  if (i >= action_details_ptr->action_hdl_count) {
    return (NULL);
  }
  return (action_hdl_ptr);
}

static pipemgr_tbl_pkg_way_format_t *
pipemmgr_entry_format_get_hash_way_details_ptr(
    uint8_t hash_way,
    pipemgr_tbl_pkg_exm_stage_hdl_format_t *stg_hdl_ptr,
    bool is_stash) {
  // Fetch correct hash-way. Currently only hash-way zero is used because,
  // entryformatting across all hash ways is same.
  // However the data structures do allow to fetch formatting details of
  // any hash-way. In future if needed make necessary changes to
  // fetch the correct hash-way.
  (void)hash_way;
  if (is_stash) {
    return (stg_hdl_ptr->stash_way_pkg);  // Using hash-way zero all the time.
  }
  return (stg_hdl_ptr->way_pkg);  // Using hash-way zero all the time.
}

static pipemgr_tbl_pkg_match_entry_line_t *
pipemmgr_entry_format_get_entry_details_ptr(
    uint8_t entry_position, pipemgr_tbl_pkg_match_entry_format_t *entry_ptr) {
  uint32_t i, field_count;
  pipemgr_tbl_pkg_match_entry_line_t *entry_line_ptr;

  entry_line_ptr = entry_ptr->entry_pkg;
  for (i = 0; i < entry_ptr->entry_count; i++) {
    if (entry_line_ptr->entry == entry_position) {
      // entry_line_ptr is now pointing to list of fields using which exm key
      // can be formatted.
      break;
    }
    field_count = entry_line_ptr->field_count;
    entry_line_ptr++;
    entry_line_ptr =
        (pipemgr_tbl_pkg_match_entry_line_t
             *)((uint8_t *)entry_line_ptr +
                (field_count * sizeof(pipemgr_tbl_pkg_match_entry_field_t)));
  }
  if (i >= entry_ptr->entry_count) {
    return (NULL);
  }
  return (entry_line_ptr);
}

static uint32_t pipemgr_tbl_pkg_transform_meter_ptr(
    uint32_t indirect_meter_ptr, pipemgr_tbl_pkg_match_entry_field_t *field) {
  uint32_t pfe_meter_type = 0;
  uint32_t pfe_meter_type_mask = 0;
  uint32_t transformed_ptr = indirect_meter_ptr;
  pfe_meter_type_mask = ((1 << (TOF_METER_ADDR_NUM_PFE_BITS +
                                TOF_METER_ADDR_NUM_METER_TYPE_BITS)) -
                         1);
  pfe_meter_type = ((transformed_ptr >> TOF_METER_ADDR_PFE_BIT_POSITION) &
                    pfe_meter_type_mask);
  /* Move the per-flow bits to the right position */
  /* Currently, there is support for some "upper" bits to be defaulted by the
   * compiler. If the compiler chooses to default non-contiguous bits, this
   * does not work. Currently the compiler only defaults contiguous MSB bits
   * only, in addition to the lower HUFFMAN bits. Unclear about when the
   * compiler will ever do that.
   */

  /* The field->msbit will be as big as the length of the field allocated. Hence
   * if we have to lose the PFE bit, we first "edit" the extracted pfe and meter
   * type from the full virtual address passed here, and shift it field->fieldsb
   * (this is the number of bits that the compiler has defaulted) + field->msbit
   */
  /* FIXME -- the context json doesn't really contain enough info to determine
   * where the type and pfe fields are in the meter pointer.  We assume that the
   * msb is the pfe iff it is <= 16.  If the field is larger than 17 bits, we
   * assume bit 16 is the pfe bit and the bits above it are the type */
  uint16_t field_typebit = ENTRYFORMAT_MIN(field->msbit, 16);
  if (!field->perflow) {
    /* Lose the PFE bit */
    pfe_meter_type >>= 1;
    pfe_meter_type_mask >>= 1;
  }
  transformed_ptr &= ~(pfe_meter_type_mask << (field_typebit + field->fieldsb));
  transformed_ptr |= (pfe_meter_type << (field_typebit + field->fieldsb));
  return transformed_ptr;
}

static uint32_t pipemgr_tbl_pkg_reverse_transform_meter_ptr(
    uint32_t indirect_meter_ptr,
    pipemgr_tbl_pkg_match_entry_field_t *field,
    pipemgr_tbl_pkg_action_entry_field_t *action_hdl_ptr) {
  uint32_t transformed_ptr = indirect_meter_ptr;
  uint32_t pfe_meter_type = 0;
  uint32_t pfe_meter_type_mask = 0;

  pfe_meter_type_mask = ((1 << (TOF_METER_ADDR_NUM_PFE_BITS +
                                TOF_METER_ADDR_NUM_METER_TYPE_BITS)) -
                         1);
  uint16_t field_typebit = ENTRYFORMAT_MIN(field->msbit, 16);
  pfe_meter_type = (indirect_meter_ptr >> field_typebit) & pfe_meter_type_mask;

  /* Force the full address if the action requires it. */
  if (action_hdl_ptr && action_hdl_ptr->force_meter_addr) {
    transformed_ptr = action_hdl_ptr->forced_full_meter_addr;
  }
  /* Force the PFE bit to be set if the action requires it. */
  if (action_hdl_ptr && action_hdl_ptr->force_meter_pfe_set) {
    transformed_ptr |= 1 << field_typebit;
  }
  /* Extract the PFE + METER TYPE portion of the address */
  if (!field->perflow) {
    /* Per-flow is defaulted, just put that PFE bit */
    pfe_meter_type = ((pfe_meter_type << 1) | 1) & pfe_meter_type_mask;
  }
  transformed_ptr |= (pfe_meter_type << field_typebit);
  transformed_ptr = (transformed_ptr << field->fieldsb);

  return transformed_ptr;
}

pipe_status_t pipe_mgr_tcam_compress_decoded_range_entries(
    bf_dev_id_t dev_id,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t **match_specs,
    pipe_tbl_match_spec_t *pipe_match_spec,
    uint32_t range_count) {
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_rangetbl_t *rangetbl_ptr;
  pipemgr_tbl_pkg_rangetbl_field_t *rangefield_ptr;
  if (!PIPE_MGR_TBL_PKG_CTX(dev_id, prof_id).rangetbl_lut) {
    /* Ugh, why are we even here, if there are no range tables. Carp */
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }
  uint32_t bj_hash;
  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(dev_id, prof_id).rangetbl_lut_depth,
      mat_tbl_hdl,
      0,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry_no_logging(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(dev_id, prof_id).rangetbl_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(dev_id, prof_id).rangetbl_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }
  rangetbl_ptr = lut_ptr->u.rangetbl_ptr;
  rangefield_ptr = rangetbl_ptr->fields;
  /* Loop over each field in the key that has a range match type. */
  for (unsigned i = 0; i < rangetbl_ptr->fieldcount; i++) {
    uint64_t final_start = 0;
    uint64_t final_end = 0;
    /* Get the location in the match spec of this range field.  Note, like all
     * match spec fields, it is padded to a byte boundry and stored in network
     * order. */
    int spec_start_byte = rangefield_ptr->param_startbit / 8;
    int spec_num_bytes = (rangefield_ptr->param_fieldwidth + 7) / 8;
    /* Range entries need not be in contiguous order of increasing range. Hence
     * loop over all entries to which it expanded and find out the least start
     * value and the highest end value.
     */
    for (unsigned j = 0; j < range_count; j++) {
      uint64_t cur_start = 0;
      uint64_t cur_end = 0;
      /* Extract start and end from this match spec and compare it with our
       * running start and end. */
      for (int m = 0, n = spec_start_byte; m < spec_num_bytes; ++m, ++n) {
        cur_start = (cur_start << 8) | match_specs[j]->match_value_bits[n];
        cur_end = (cur_end << 8) | match_specs[j]->match_mask_bits[n];
      }

      if (j == 0 || cur_start < final_start) {
        final_start = cur_start;
      }
      if (j == 0 || cur_end > final_end) {
        final_end = cur_end;
      }
    }
    /* Write the final start/end values into the return match spec. */
    for (int m = 0, n = spec_start_byte + spec_num_bytes - 1;
         m < spec_num_bytes;
         ++m, --n) {
      pipe_match_spec->match_value_bits[n] = (final_start >> (m * 8)) & 0xFF;
      pipe_match_spec->match_mask_bits[n] = (final_end >> (m * 8)) & 0xFF;
    }

    /* Why?!  This is what arrays are for... */
    uint32_t maufieldcount = rangefield_ptr->maufieldcount;
    rangefield_ptr++;
    rangefield_ptr =
        (pipemgr_tbl_pkg_rangetbl_field_t
             *)((uint8_t *)rangefield_ptr +
                (maufieldcount * sizeof(pipemgr_tbl_pkg_rangetbl_mau_field_t)));
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_tof_dkm_tbl_keymask_encode(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    dev_stage_t stage_id,
    uint8_t stage_table_handle,
    uint8_t sub_entry,
    pipe_tbl_match_spec_t *match_spec,
    uint8_t **exm_tbl_word) {
  pipemgr_tbl_pkg_exm_stage_hdl_format_t *stage_hdl_ptr;
  pipemgr_tbl_pkg_way_format_t *way_ptr;
  pipemgr_tbl_pkg_match_entry_line_t *entry_line_ptr;
  pipemgr_tbl_pkg_match_entry_field_t *field;
  uint8_t version_valid_bits;
  uint32_t hash_way = 0, bj_hash;
  uint32_t i;
  pipemgr_tbl_pkg_lut_t *lut_ptr;

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut_depth,
      mat_tbl_hdl,
      stage_id,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry_no_logging(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut,
      mat_tbl_hdl,
      stage_id);
  if (!lut_ptr) {
    // mat table indentified by mat_tbl_hdl' is not in stage 'stage_id'
    return (PIPE_TABLE_NOT_FOUND);
  }

  RMT_EXM_SET_ENTRY_VERSION_VALID_BITS(RMT_EXM_ENTRY_VERSION_DONT_CARE,
                                       RMT_EXM_ENTRY_VERSION_DONT_CARE,
                                       version_valid_bits);

  stage_hdl_ptr = pipemmgr_entry_format_get_stage_handle_details_ptr(
      stage_table_handle, lut_ptr);
  if (!stage_hdl_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl "
        "0x%x, stage %d logical table id %d",
        __func__,
        mat_tbl_hdl,
        stage_id,
        stage_table_handle);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  // use hash-way 0
  way_ptr =
      pipemmgr_entry_format_get_hash_way_details_ptr(0, stage_hdl_ptr, false);
  if (!way_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl "
        "0x%x stage %d, hash-way %d",
        __func__,
        mat_tbl_hdl,
        stage_id,
        hash_way);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  entry_line_ptr = pipemmgr_entry_format_get_entry_details_ptr(
      sub_entry, way_ptr->entry_format);
  if (!entry_line_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl "
        "0x%x, stage %d, entry %d",
        __func__,
        mat_tbl_hdl,
        stage_id,
        0);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  for (i = 0; i < entry_line_ptr->field_count; i++) {
    field = entry_line_ptr->fields + i;
    switch (field->source) {
      case TBL_PKG_FIELD_SOURCE_VERSION:
        set_val(exm_tbl_word,
                field->memword_index[0],
                field->field_offset,
                field->field_width,
                version_valid_bits);
        break;
      case TBL_PKG_FIELD_SOURCE_SPEC:
        if (field->match_mode == TBL_PKG_FIELD_MATCHMODE_S1Q0 ||
            field->match_mode == TBL_PKG_FIELD_MATCHMODE_S0Q1) {
          // currently DKM support is not available for alpm
          LOG_ERROR(
              "%s: %d dynamic-key-mask is not supported for ALPM match-tbl "
              "0x%x",
              __func__,
              __LINE__,
              mat_tbl_hdl);
          PIPE_MGR_DBGCHK(0);
          return PIPE_INVALID_ARG;
        } else {
          copy_bits(exm_tbl_word,
                    field->memword_index[0],
                    field->field_offset,
                    field->field_width,
                    match_spec->match_mask_bits,  // use dynamic mask bits
                                                  // not value bits
                    field->spec_start_bit,
                    field->spec_len,
                    field->fieldsb);
        }
        break;
      case TBL_PKG_FIELD_SOURCE_PROXYHASH:
        // currently DKM support is not for proxy-hash
        LOG_ERROR(
            "%s: %d dynamic-key-mask is not supported for proxy-hash match-tbl "
            "0x%x",
            __func__,
            __LINE__,
            mat_tbl_hdl);
        PIPE_MGR_DBGCHK(0);
        return (PIPE_INVALID_ARG);
      default:
        /* Skip all other entry overhead fields. */
        continue;
    }
  }
  return PIPE_SUCCESS;
}

/* Prior to schema version 1.3.11 in Brig, we write the full indirect pointer
 * in one go into the match overhead. In this case, we must perform certain
 * pointer transformations.
 */
static bool transform_ptr(rmt_dev_profile_info_t *prof_info) {
  /* Glass requires indirect pointer transforms */
  if (prof_info->compiler_version[0] < 6) {
    return true;
  }

  /* Brig requires indirect pointer transforms for schema versions under 1.3.11
   */
  if (prof_info->schema_version[0] < 1) {
    return true;
  }
  if (prof_info->schema_version[0] == 1 && prof_info->schema_version[1] < 3) {
    return true;
  }
  if (prof_info->schema_version[0] == 1 && prof_info->schema_version[1] == 3 &&
      prof_info->schema_version[2] < 11) {
    return true;
  }

  return false;
}

pipe_status_t pipe_mgr_entry_format_tof_exm_tbl_ent_update(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint8_t stage_table_handle,
    uint8_t version_valid_bits,
    pipe_tbl_match_spec_t *match_spec,
    pipe_act_fn_hdl_t act_fn_hdl,
    pipe_action_data_spec_t *act_data_spec,
    uint32_t indirect_action_ptr,
    uint32_t indirect_stats_ptr,
    uint32_t indirect_meter_ptr,
    uint32_t indirect_stfl_ptr,
    uint32_t indirect_sel_ptr,
    uint32_t selector_len,
    uint8_t entry_position,
    uint64_t proxy_hash,
    uint8_t **exm_tbl_word,
    bool ram_words_updated[TOF_MAX_RAM_WORDS_IN_EXM_TBL_WORD],
    int *vv_word_index,
    bool is_stash) {
  pipemgr_tbl_pkg_exm_stage_hdl_format_t *stage_hdl_ptr;
  pipemgr_tbl_pkg_action_entry_field_t *action_hdl_ptr;
  pipemgr_tbl_pkg_way_format_t *way_ptr;
  pipemgr_tbl_pkg_match_entry_line_t *entry_line_ptr;
  pipemgr_tbl_pkg_match_entry_field_t *field;
  pipemgr_tbl_pkg_action_parameter_field_t *act_param_ptr;
  uint32_t hash_way = 0, bj_hash;
  uint16_t sel_len = selector_len;
  uint8_t sel_len_shift = 0;
  uint32_t i, j, k;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  bool imm_data_zero = false;
  *vv_word_index = 0;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  rmt_dev_profile_info_t *prof_info = dev_info->profile_info[prof_id];

  if (selector_len) {
    indirect_sel_ptr |= (1 << TOF_METER_ADDR_PFE_BIT_POSITION);
  } else {
    indirect_sel_ptr &= ~(1 << TOF_METER_ADDR_PFE_BIT_POSITION);
  }
  while (sel_len >= (1 << 5)) {
    PIPE_MGR_DBGCHK((sel_len & 0x1) == 0);
    sel_len >>= 1;
    sel_len_shift++;
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut_depth,
      mat_tbl_hdl,
      stage_id,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut,
      mat_tbl_hdl,
      stage_id);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x stage "
        "%d",
        __func__,
        mat_tbl_hdl,
        stage_id);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  stage_hdl_ptr = pipemmgr_entry_format_get_stage_handle_details_ptr(
      stage_table_handle, lut_ptr);
  if (!stage_hdl_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d lt-id %d",
        __func__,
        mat_tbl_hdl,
        stage_id,
        stage_table_handle);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }
  action_hdl_ptr =
      pipemmgr_entry_format_get_action_handle_details_ptr(act_fn_hdl, lut_ptr);
  if (!action_hdl_ptr) {
    LOG_ERROR(
        "%s: Unable to find action handle 0x%x formatting details for "
        "match-tbl 0x%x, stage %d",
        __func__,
        act_fn_hdl,
        mat_tbl_hdl,
        stage_id);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  way_ptr = pipemmgr_entry_format_get_hash_way_details_ptr(
      hash_way, stage_hdl_ptr, is_stash);
  if (!way_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d, hash-way %d",
        __func__,
        mat_tbl_hdl,
        stage_id,
        hash_way);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  entry_line_ptr = pipemmgr_entry_format_get_entry_details_ptr(
      entry_position, way_ptr->entry_format);
  if (!entry_line_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d, entry %d",
        __func__,
        mat_tbl_hdl,
        stage_id,
        entry_position);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  field = entry_line_ptr->fields;
  for (i = 0; i < entry_line_ptr->field_count; i++) {
    switch (field->source) {
      case TBL_PKG_FIELD_SOURCE_VERSION:
        set_val(exm_tbl_word,
                field->memword_index[0],
                field->field_offset,
                field->field_width,
                version_valid_bits);
        *vv_word_index = field->memword_index[0];
        break;
      case TBL_PKG_FIELD_SOURCE_ZERO:
        break;
      case TBL_PKG_FIELD_SOURCE_SPEC:
        if (field->match_mode == TBL_PKG_FIELD_MATCHMODE_S1Q0) {
          set_key_mask_s1q0_encode(exm_tbl_word,
                                   NULL,
                                   field->memword_index[0],
                                   field->field_offset,
                                   field->field_width,
                                   match_spec->match_value_bits,
                                   match_spec->match_mask_bits,
                                   field->spec_start_bit,
                                   field->spec_len,
                                   field->fieldsb);
        } else if (field->match_mode == TBL_PKG_FIELD_MATCHMODE_S0Q1) {
          set_key_mask_s1q0_encode(NULL,
                                   exm_tbl_word,
                                   field->memword_index[0],
                                   field->field_offset,
                                   field->field_width,
                                   match_spec->match_value_bits,
                                   match_spec->match_mask_bits,
                                   field->spec_start_bit,
                                   field->spec_len,
                                   field->fieldsb);
        } else {
          copy_bits(exm_tbl_word,
                    field->memword_index[0],
                    field->field_offset,
                    field->field_width,
                    match_spec->match_value_bits,
                    field->spec_start_bit,
                    field->spec_len,
                    field->fieldsb);
        }
        break;
      case TBL_PKG_FIELD_SOURCE_SELPTR:
        set_val(exm_tbl_word,
                field->memword_index[0],
                field->field_offset,
                field->field_width,
                indirect_sel_ptr >> field->fieldsb);
        break;
      case TBL_PKG_FIELD_SOURCE_ADTPTR:
        set_val(exm_tbl_word,
                field->memword_index[0],
                field->field_offset,
                field->field_width,
                indirect_action_ptr >> field->fieldsb);
        break;
      case TBL_PKG_FIELD_SOURCE_INSTR:
        set_val(exm_tbl_word,
                field->memword_index[0],
                field->field_offset,
                field->field_width,
                action_hdl_ptr->instr);
        break;
      case TBL_PKG_FIELD_SOURCE_NXT_TBL:
        set_val(exm_tbl_word,
                field->memword_index[0],
                field->field_offset,
                field->field_width,
                action_hdl_ptr->next_tbl);
        break;
      case TBL_PKG_FIELD_SOURCE_IMMEDIATE:
        /* Zero out immediate data before populating */
        if (!imm_data_zero) {
          set_val(exm_tbl_word,
                  field->memword_index[0],
                  field->field_offset,
                  field->field_width,
                  0);
          imm_data_zero = true;
        }
        if (action_hdl_ptr->act_param) {
          act_param_ptr = action_hdl_ptr->act_param;
          for (k = 0; k < action_hdl_ptr->param_count; k++) {
            PIPE_MGR_DBGCHK(act_param_ptr->bit_width +
                                act_param_ptr->start_offset <=
                            field->field_width);
            if (act_param_ptr->is_parameter == 0 || !act_data_spec) {
              set_val(exm_tbl_word,
                      field->memword_index[0],
                      field->field_offset + act_param_ptr->start_offset,
                      act_param_ptr->bit_width,
                      act_param_ptr->constant_value);
            } else {
              if (act_param_ptr->is_mod_field_conditionally_mask) {
                uint64_t val = 0;
                get_shifted_val(act_data_spec->action_data_bits,
                                act_param_ptr->param_start,
                                act_param_ptr->param_width,
                                0,
                                &val);
                if (val) {
                  uint16_t padded_field_width = (field->field_width + 7) / 8;
                  uint8_t *data = (uint8_t *)PIPE_MGR_CALLOC(padded_field_width,
                                                             sizeof(uint8_t));
                  if (!data) {
                    LOG_ERROR("%s:%d Malloc failure", __func__, __LINE__);
                    return PIPE_NO_SYS_RESOURCES;
                  }
                  PIPE_MGR_MEMSET(
                      data, 0xff, sizeof(uint8_t) * padded_field_width);
                  copy_bits(exm_tbl_word,
                            field->memword_index[0],
                            field->field_offset + act_param_ptr->start_offset,
                            act_param_ptr->bit_width,
                            data,
                            0,
                            padded_field_width * 8,
                            0);
                  PIPE_MGR_FREE(data);
                } else {
                  set_val(exm_tbl_word,
                          field->memword_index[0],
                          field->field_offset + act_param_ptr->start_offset,
                          act_param_ptr->bit_width,
                          val);
                }
              } else if (act_param_ptr->is_mod_field_conditionally_value) {
                uint64_t val = 0;
                get_shifted_val(act_data_spec->action_data_bits,
                                act_param_ptr->mask_offset,
                                act_param_ptr->mask_width,
                                0,
                                &val);
                if (val) {
                  /* Write the spec value if the condition is on */
                  copy_bits(exm_tbl_word,
                            field->memword_index[0],
                            field->field_offset + act_param_ptr->start_offset,
                            act_param_ptr->bit_width,
                            act_data_spec->action_data_bits,
                            act_param_ptr->param_start,
                            act_param_ptr->param_width,
                            act_param_ptr->param_shift);
                } else {
                  set_val(exm_tbl_word,
                          field->memword_index[0],
                          field->field_offset + act_param_ptr->start_offset,
                          act_param_ptr->bit_width,
                          val);
                }
              } else {
                copy_bits(exm_tbl_word,
                          field->memword_index[0],
                          field->field_offset + act_param_ptr->start_offset,
                          act_param_ptr->bit_width,
                          act_data_spec->action_data_bits,
                          act_param_ptr->param_start,
                          act_param_ptr->param_width,
                          act_param_ptr->param_shift);
              }
            }
            act_param_ptr++;
          }
        }
        break;
      case TBL_PKG_FIELD_SOURCE_METERPTR:
        /* Force the full address if the action requires it. */
        if (action_hdl_ptr->force_meter_addr) {
          indirect_meter_ptr = action_hdl_ptr->forced_full_meter_addr;
        }
        /* Force the PFE bit to be set if the action requires it. */
        if (action_hdl_ptr->force_meter_pfe_set) {
          indirect_meter_ptr |= 1 << TOF_METER_ADDR_PFE_BIT_POSITION;
        }
        /* Transform the meter ptr */
        if (transform_ptr(prof_info)) {
          indirect_meter_ptr =
              pipemgr_tbl_pkg_transform_meter_ptr(indirect_meter_ptr, field);
        }
        set_val(exm_tbl_word,
                field->memword_index[0],
                field->field_offset,
                field->field_width,
                indirect_meter_ptr >> field->fieldsb);
        break;
      case TBL_PKG_FIELD_SOURCE_STATSPTR:
        /* Force the full stats address if the action requires it. */
        if (action_hdl_ptr->force_stats_addr) {
          indirect_stats_ptr = action_hdl_ptr->forced_full_stats_addr;
        }
        /* Force the PFE bit to be set if the action requires it. */
        if (action_hdl_ptr->force_stats_pfe_set) {
          indirect_stats_ptr |= 1 << TOF_STATS_RAM_NUM_ADDR_BITS;
        }
        set_val(exm_tbl_word,
                field->memword_index[0],
                field->field_offset,
                field->field_width,
                indirect_stats_ptr >> field->fieldsb);
        break;
      case TBL_PKG_FIELD_SOURCE_STFLPTR: {
        uint32_t sptr = indirect_stfl_ptr;
        /* Force the full stateful address if the action requires it. */
        if (action_hdl_ptr->force_stful_addr) {
          sptr = action_hdl_ptr->forced_full_stful_addr;
        }
        /* Force the PFE bit to be set if the action requires it. */
        if (action_hdl_ptr->force_stful_pfe_set) {
          sptr |= 1 << 23;
        }
        /* Transform the stful ptr */
        if (transform_ptr(prof_info)) {
          /* Remove bit 24 from the pointer before encoding.  This bit always
           * has a fixed value for stateful pointers and the compiler can
           * default it. */
          sptr = ((sptr & 0x0E000000) >> 1) | (sptr & 0x00FFFFFF);
        }
        set_val(exm_tbl_word,
                field->memword_index[0],
                field->field_offset,
                field->field_width,
                sptr >> field->fieldsb);
        break;
      }
      case TBL_PKG_FIELD_SOURCE_SELLEN:
        set_val(exm_tbl_word,
                field->memword_index[0],
                field->field_offset,
                field->field_width,
                sel_len);
        break;
      case TBL_PKG_FIELD_SOURCE_SELLENSHIFT:
        set_val(exm_tbl_word,
                field->memword_index[0],
                field->field_offset,
                field->field_width,
                sel_len_shift);
        break;
      case TBL_PKG_FIELD_SOURCE_PROXYHASH:
        set_val(exm_tbl_word,
                field->memword_index[0],
                field->field_offset,
                field->field_width,
                proxy_hash >> field->fieldsb);
        break;
      case TBL_PKG_FIELD_SOURCE_INVALID:
        LOG_ERROR(
            "%s: Unable to find entry formatting details for match-tbl 0x%x, "
            "stage "
            "%d, hash-way %d, entry %d",
            __func__,
            mat_tbl_hdl,
            stage_id,
            hash_way,
            entry_position);
        PIPE_MGR_DBGCHK(0);
        return (PIPE_INVALID_ARG);
      default:
        LOG_ERROR(
            "%s: Unable to find entry formatting details for match-tbl 0x%x, "
            "stage "
            "%d, hash-way %d, entry %d",
            __func__,
            mat_tbl_hdl,
            stage_id,
            hash_way,
            entry_position);
        PIPE_MGR_DBGCHK(0);
        return (PIPE_INVALID_ARG);
    }
    for (j = field->memword_index[0]; j < field->memword_index[1] + 1; j++) {
      ram_words_updated[j] = true;
    }
    field++;
  }
  return (PIPE_SUCCESS);
}

pipe_status_t pipe_mgr_entry_format_tof_exm_get_next_tbl(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint8_t stage_table_handle,
    pipe_act_fn_hdl_t act_fn_hdl,
    uint32_t *next_table) {
  pipemgr_tbl_pkg_exm_stage_hdl_format_t *stage_hdl_ptr;
  pipemgr_tbl_pkg_action_entry_field_t *action_hdl_ptr;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  uint32_t bj_hash;

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut_depth,
      mat_tbl_hdl,
      stage_id,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut,
      mat_tbl_hdl,
      stage_id);
  if (!lut_ptr) {
    LOG_ERROR("%s: Unable to find entry formatting details for match-tbl 0x%x",
              __func__,
              mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  stage_hdl_ptr = pipemmgr_entry_format_get_stage_handle_details_ptr(
      stage_table_handle, lut_ptr);
  if (!stage_hdl_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d",
        __func__,
        mat_tbl_hdl,
        stage_id);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }
  action_hdl_ptr =
      pipemmgr_entry_format_get_action_handle_details_ptr(act_fn_hdl, lut_ptr);
  if (!action_hdl_ptr) {
    LOG_ERROR(
        "%s: Unable to find action handle formatting details for match-tbl "
        "0x%x, act_fn_hdl %u",
        __func__,
        mat_tbl_hdl,
        act_fn_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  *next_table = action_hdl_ptr->next_tbl;
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_tof_exm_tbl_ent_set_vv(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint8_t stage_table_handle,
    uint8_t entry_position,
    uint8_t version_valid_bits,
    uint8_t **exm_tbl_word,
    bool ram_words_updated[TOF_MAX_RAM_WORDS_IN_EXM_TBL_WORD],
    bool is_stash) {
  pipemgr_tbl_pkg_exm_stage_hdl_format_t *stage_hdl_ptr;
  pipemgr_tbl_pkg_way_format_t *way_ptr;
  pipemgr_tbl_pkg_match_entry_line_t *entry_line_ptr;
  pipemgr_tbl_pkg_match_entry_field_t *field;
  uint32_t hash_way = 0, bj_hash;
  uint32_t i, j;
  pipemgr_tbl_pkg_lut_t *lut_ptr;

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut_depth,
      mat_tbl_hdl,
      stage_id,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut,
      mat_tbl_hdl,
      stage_id);
  if (!lut_ptr) {
    LOG_ERROR("%s: Unable to find entry formatting details for match-tbl 0x%x",
              __func__,
              mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  stage_hdl_ptr = pipemmgr_entry_format_get_stage_handle_details_ptr(
      stage_table_handle, lut_ptr);
  if (!stage_hdl_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d",
        __func__,
        mat_tbl_hdl,
        stage_id);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  way_ptr = pipemmgr_entry_format_get_hash_way_details_ptr(
      hash_way, stage_hdl_ptr, is_stash);
  if (!way_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d, hash-way %d",
        __func__,
        mat_tbl_hdl,
        stage_id,
        hash_way);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  entry_line_ptr = pipemmgr_entry_format_get_entry_details_ptr(
      entry_position, way_ptr->entry_format);
  if (!entry_line_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d, entry %d",
        __func__,
        mat_tbl_hdl,
        stage_id,
        entry_position);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  field = entry_line_ptr->fields;
  for (i = 0; i < entry_line_ptr->field_count; i++) {
    switch (field->source) {
      case TBL_PKG_FIELD_SOURCE_VERSION:
        set_val(exm_tbl_word,
                field->memword_index[0],
                field->field_offset,
                field->field_width,
                version_valid_bits);
        for (j = field->memword_index[0]; j < field->memword_index[1] + 1;
             j++) {
          ram_words_updated[j] = true;
        }
        break;
      default:
        break;
    }
    field++;
  }
  return (PIPE_SUCCESS);
}

pipe_status_t pipe_mgr_entry_format_tof_tbl_uses_imm_data(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_act_fn_hdl_t act_fn_hdl,
    bool is_tind,
    bool *uses_imm_data) {
  pipemgr_tbl_pkg_lut_t *lut_ptr = NULL;
  pipemgr_tbl_pkg_action_entry_field_t *action_hdl_ptr = NULL;
  pipemgr_tbl_pkg_action_parameter_field_t *act_param_ptr = NULL;
  uint32_t bj_hash = 0, i = 0;

  if (is_tind) {
    bj_hash = bob_jenkin_hash_one_at_a_time(
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tind_lut_depth,
        mat_tbl_hdl,
        stage_id,
        0);
    lut_ptr = pipemgr_entry_format_get_lut_entry(
        bj_hash,
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tind_lut_depth,
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tind_lut,
        mat_tbl_hdl,
        stage_id);
  } else {
    bj_hash = bob_jenkin_hash_one_at_a_time(
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut_depth,
        mat_tbl_hdl,
        stage_id,
        0);
    lut_ptr = pipemgr_entry_format_get_lut_entry(
        bj_hash,
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut_depth,
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut,
        mat_tbl_hdl,
        stage_id);
  }
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d",
        __func__,
        mat_tbl_hdl,
        stage_id);
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }

  action_hdl_ptr =
      pipemmgr_entry_format_get_action_handle_details_ptr(act_fn_hdl, lut_ptr);
  if (!action_hdl_ptr) {
    LOG_ERROR(
        "%s: Unable to find action handle formatting details for match-tbl "
        "0x%x, stage %d",
        __func__,
        mat_tbl_hdl,
        stage_id);
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }

  *uses_imm_data = false;
  if (action_hdl_ptr->act_param) {
    act_param_ptr = action_hdl_ptr->act_param;
    for (i = 0; i < action_hdl_ptr->param_count; i++) {
      if (act_param_ptr->is_parameter > 0) {
        *uses_imm_data = true;
        break;
      }
      act_param_ptr++;
    }
  }

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_tof_tind_tbl_ent_update(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_act_fn_hdl_t act_fn_hdl,
    pipe_action_data_spec_t *act_data_spec,
    uint32_t indirect_action_ptr,
    uint32_t indirect_stats_ptr,
    uint32_t indirect_meter_ptr,
    uint32_t indirect_stfl_ptr,
    uint32_t indirect_sel_ptr,
    uint32_t selector_len,
    uint8_t entry_position,
    uint32_t offset,
    tind_tbl_word_t *tind_tbl_word) {
  pipemgr_tbl_pkg_action_entry_field_t *action_hdl_ptr;
  pipemgr_tbl_pkg_match_entry_line_t *entry_line_ptr;
  pipemgr_tbl_pkg_match_entry_field_t *field;
  pipemgr_tbl_pkg_action_parameter_field_t *act_param_ptr;
  uint32_t bj_hash;
  uint16_t sel_len = selector_len;
  uint8_t sel_len_shift = 0;
  uint32_t i, k;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  bool imm_data_zero = false;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }
  rmt_dev_profile_info_t *prof_info = dev_info->profile_info[prof_id];

  if (!act_fn_hdl) {
    return (PIPE_SUCCESS);
  }

  // Selector_len = 0 indicates a per-flow disable
  if (selector_len) {
    indirect_sel_ptr |= (1 << TOF_METER_ADDR_PFE_BIT_POSITION);
  } else {
    indirect_sel_ptr &= ~(1 << TOF_METER_ADDR_PFE_BIT_POSITION);
  }
  while (sel_len >= (1 << 5)) {
    PIPE_MGR_DBGCHK((sel_len & 0x1) == 0);
    sel_len >>= 1;
    sel_len_shift++;
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tind_lut_depth,
      mat_tbl_hdl,
      stage_id,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tind_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tind_lut,
      mat_tbl_hdl,
      stage_id);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl %d, stage "
        "%d",
        __func__,
        mat_tbl_hdl,
        stage_id);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  entry_line_ptr = pipemmgr_entry_format_get_entry_details_ptr(
      entry_position, lut_ptr->u.tind_ptr);
  if (!entry_line_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d, entry %d",
        __func__,
        mat_tbl_hdl,
        stage_id,
        entry_position);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  action_hdl_ptr =
      pipemmgr_entry_format_get_action_handle_details_ptr(act_fn_hdl, lut_ptr);
  if (!action_hdl_ptr) {
    LOG_ERROR(
        "%s: Unable to find action entry formatting details for action-tbl "
        "0x%x, stage %d, entry %d",
        __func__,
        act_fn_hdl,
        stage_id,
        entry_position);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  field = entry_line_ptr->fields;
  for (i = 0; i < entry_line_ptr->field_count; i++) {
    switch (field->source) {
      case TBL_PKG_FIELD_SOURCE_ZERO:
        set_val_old(*tind_tbl_word,
                    field->field_offset + offset,
                    field->field_width,
                    0);
        break;
      case TBL_PKG_FIELD_SOURCE_INSTR:
        set_val_old(*tind_tbl_word,
                    field->field_offset + offset,
                    field->field_width,
                    action_hdl_ptr->instr);
        break;
      case TBL_PKG_FIELD_SOURCE_ADTPTR:
        set_val_old(*tind_tbl_word,
                    field->field_offset + offset,
                    field->field_width,
                    indirect_action_ptr >> field->fieldsb);
        break;
      case TBL_PKG_FIELD_SOURCE_METERPTR:
        /* Force the full address if the action requires it. */
        if (action_hdl_ptr->force_meter_addr) {
          indirect_meter_ptr = action_hdl_ptr->forced_full_meter_addr;
        }
        /* Force the PFE bit to be set if the action requires it. */
        if (action_hdl_ptr->force_meter_pfe_set) {
          indirect_meter_ptr |= 1 << TOF_METER_ADDR_PFE_BIT_POSITION;
        }
        /* Transform the meter ptr */
        if (transform_ptr(prof_info)) {
          /* First, extract pfe bits/meter type bits - One PFE for meter ALU, 3
           * bits indicating meter type. |METER-TYPE(3b)|PFE(1b)|
           */
          indirect_meter_ptr =
              pipemgr_tbl_pkg_transform_meter_ptr(indirect_meter_ptr, field);
        }
        set_val_old(*tind_tbl_word,
                    field->field_offset + offset,
                    field->field_width,
                    indirect_meter_ptr >> field->fieldsb);
        break;
      case TBL_PKG_FIELD_SOURCE_STATSPTR:
        /* Force the full stats address if the action requires it. */
        if (action_hdl_ptr->force_stats_addr) {
          indirect_stats_ptr = action_hdl_ptr->forced_full_stats_addr;
        }
        /* Force the PFE bit to be set if the action requires it. */
        if (action_hdl_ptr->force_stats_pfe_set) {
          indirect_stats_ptr |= 1 << TOF_STATS_RAM_NUM_ADDR_BITS;
        }
        set_val_old(*tind_tbl_word,
                    field->field_offset + offset,
                    field->field_width,
                    indirect_stats_ptr >> field->fieldsb);
        break;
      case TBL_PKG_FIELD_SOURCE_STFLPTR: {
        uint32_t sptr = indirect_stfl_ptr;
        /* Force the full stateful address if the action requires it. */
        if (action_hdl_ptr->force_stful_addr) {
          sptr = action_hdl_ptr->forced_full_stful_addr;
        }
        /* Force the PFE bit to be set if the action requires it. */
        if (action_hdl_ptr->force_stful_pfe_set) {
          sptr |= 1 << 23;
        }
        /* Transform the stful ptr */
        if (transform_ptr(prof_info)) {
          /* Remove bit 24 from the pointer before encoding.  This bit always
           * has a fixed value for stateful pointers and the compiler can
           * default it. */
          sptr = ((sptr & 0x0E000000) >> 1) | (sptr & 0x00FFFFFF);
        }
        set_val_old(*tind_tbl_word,
                    field->field_offset + offset,
                    field->field_width,
                    sptr >> field->fieldsb);
        break;
      }
      case TBL_PKG_FIELD_SOURCE_SELPTR:
        set_val_old(*tind_tbl_word,
                    field->field_offset + offset,
                    field->field_width,
                    indirect_sel_ptr >> field->fieldsb);
        break;
      case TBL_PKG_FIELD_SOURCE_SELLEN:
        set_val_old(*tind_tbl_word,
                    field->field_offset + offset,
                    field->field_width,
                    sel_len);
        break;
      case TBL_PKG_FIELD_SOURCE_SELLENSHIFT:
        set_val_old(*tind_tbl_word,
                    field->field_offset + offset,
                    field->field_width,
                    sel_len_shift);
        break;
      case TBL_PKG_FIELD_SOURCE_NXT_TBL:
        set_val_old(*tind_tbl_word,
                    field->field_offset + offset,
                    field->field_width,
                    action_hdl_ptr->next_tbl);
        break;
      case TBL_PKG_FIELD_SOURCE_IMMEDIATE:
        /* Zero out immediate data before populating */
        if (!imm_data_zero) {
          set_val_old(*tind_tbl_word,
                      field->field_offset + offset,
                      field->field_width,
                      0);
          imm_data_zero = true;
        }
        if (action_hdl_ptr->act_param) {
          act_param_ptr = action_hdl_ptr->act_param;
          for (k = 0; k < action_hdl_ptr->param_count; k++) {
            PIPE_MGR_DBGCHK(act_param_ptr->bit_width +
                                act_param_ptr->start_offset <=
                            field->field_width);
            if (act_param_ptr->is_parameter == 0 || !act_data_spec) {
              set_val_old(
                  *tind_tbl_word,
                  field->field_offset + offset + act_param_ptr->start_offset,
                  act_param_ptr->bit_width,
                  act_param_ptr->constant_value);
            } else {
              if (act_param_ptr->is_mod_field_conditionally_mask) {
                uint64_t val = 0;
                get_shifted_val(act_data_spec->action_data_bits,
                                act_param_ptr->param_start,
                                act_param_ptr->param_width,
                                0,
                                &val);
                if (val) {
                  uint16_t padded_field_width = (field->field_width + 7) / 8;
                  uint8_t *data = (uint8_t *)PIPE_MGR_CALLOC(padded_field_width,
                                                             sizeof(uint8_t));
                  if (!data) {
                    LOG_ERROR("%s:%d Malloc failure", __func__, __LINE__);
                    return PIPE_NO_SYS_RESOURCES;
                  }
                  PIPE_MGR_MEMSET(
                      data, 0xff, sizeof(uint8_t) * padded_field_width);
                  copy_bits_old(*tind_tbl_word,
                                field->field_offset + offset +
                                    act_param_ptr->start_offset,
                                act_param_ptr->bit_width,
                                data,
                                0,
                                padded_field_width * 8,
                                0);
                  PIPE_MGR_FREE(data);
                } else {
                  set_val_old(*tind_tbl_word,
                              field->field_offset + offset +
                                  act_param_ptr->start_offset,
                              act_param_ptr->bit_width,
                              val);
                }
              } else if (act_param_ptr->is_mod_field_conditionally_value) {
                uint64_t val = 0;
                get_shifted_val(act_data_spec->action_data_bits,
                                act_param_ptr->mask_offset,
                                act_param_ptr->mask_width,
                                0,
                                &val);
                if (val) {
                  /* Write the spec value if the condition is on */
                  copy_bits_old(*tind_tbl_word,
                                field->field_offset + offset +
                                    act_param_ptr->start_offset,
                                act_param_ptr->bit_width,
                                act_data_spec->action_data_bits,
                                act_param_ptr->param_start,
                                act_param_ptr->param_width,
                                act_param_ptr->param_shift);
                } else {
                  set_val_old(*tind_tbl_word,
                              field->field_offset + offset +
                                  act_param_ptr->start_offset,
                              act_param_ptr->bit_width,
                              val);
                }
              } else {
                copy_bits_old(
                    *tind_tbl_word,
                    field->field_offset + offset + act_param_ptr->start_offset,
                    act_param_ptr->bit_width,
                    act_data_spec->action_data_bits,
                    act_param_ptr->param_start,
                    act_param_ptr->param_width,
                    act_param_ptr->param_shift);
              }
            }
            act_param_ptr++;
          }
        }
        break;
      default:
        break;
    }
    field++;
  }
  return PIPE_SUCCESS;
}

/* Get the ADT LUT entry for a given device and profile. */
static inline pipemgr_tbl_pkg_lut_t *get_adt_lut_entry(
    bf_dev_id_t devid, profile_id_t prof_id, pipe_adt_tbl_hdl_t adt_tbl_hdl) {
  uint32_t bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).adt_lut_depth, adt_tbl_hdl, 0, 0);
  return pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).adt_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).adt_lut,
      adt_tbl_hdl,
      0);
}

/* Helper to go through the list of action function handle information in the
 * ADT LUT. */
static inline pipemgr_tbl_pkg_act_fn_handle_t *next_act_fn_handle(
    pipemgr_tbl_pkg_act_fn_handle_t *hdl_ptr) {
  size_t memsize =
      (hdl_ptr->stagecount * sizeof(pipemgr_tbl_pkg_act_fn_stage_format_t));
  memsize += (hdl_ptr->totalentries * sizeof(pipemgr_tbl_pkg_act_fn_entry_t));
  memsize +=
      (hdl_ptr->totalfields * sizeof(pipemgr_tbl_pkg_act_fn_entry_field_t));
  memsize += (hdl_ptr->totalconsttuples *
              sizeof(pipemgr_tbl_pkg_act_fn_field_constvalues_t));
  hdl_ptr++;
  return (pipemgr_tbl_pkg_act_fn_handle_t *)((uint8_t *)hdl_ptr + memsize);
}

/* Get the action function handle info for a specific action function handle in
 * the ADT LUT. */
static inline pipemgr_tbl_pkg_act_fn_handle_t *find_adt_act_fn_handle(
    pipemgr_tbl_pkg_lut_t *lut_ptr, pipe_act_fn_hdl_t act_fn_hdl) {
  pipemgr_tbl_pkg_act_fn_handle_t *hdl_ptr = lut_ptr->u.adt_ptr->hdl;
  for (uint32_t i = 0; i < lut_ptr->u.adt_ptr->act_fn_hdl_count; ++i) {
    if (hdl_ptr->actfunchandle == act_fn_hdl) {
      return hdl_ptr;
    }
    // Advance to next pkg_act_fn_handle.
    hdl_ptr = next_act_fn_handle(hdl_ptr);
  }
  return NULL;
}

/* Get the stage info for a specific stage from the action function handle info.
 */
static inline pipemgr_tbl_pkg_act_fn_stage_format_t *find_act_fn_stage_format(
    pipemgr_tbl_pkg_act_fn_handle_t *act_fn_hdl_ptr, uint8_t stage_id) {
  pipemgr_tbl_pkg_act_fn_stage_format_t *format_ptr =
      act_fn_hdl_ptr->stageformat;
  for (uint32_t i = 0; i < act_fn_hdl_ptr->stagecount; ++i) {
    if (format_ptr->stage == stage_id) {
      return format_ptr;
    }
    // Advance to next pkg_act_fn_stage_format.
    size_t memsize =
        format_ptr->entrycount * sizeof(pipemgr_tbl_pkg_act_fn_entry_t);
    memsize +=
        format_ptr->totalfields * sizeof(pipemgr_tbl_pkg_act_fn_entry_field_t);
    memsize += format_ptr->totalconsttuples *
               sizeof(pipemgr_tbl_pkg_act_fn_field_constvalues_t);
    format_ptr++;
    format_ptr =
        (pipemgr_tbl_pkg_act_fn_stage_format_t *)((uint8_t *)format_ptr +
                                                  memsize);
  }
  return NULL;
}

/* Get the entry info for a specific sub-entry-id in the stage info for an
 * action function handle. */
static inline pipemgr_tbl_pkg_act_fn_entry_t *find_act_fn_entry_format(
    pipemgr_tbl_pkg_act_fn_stage_format_t *stage_format,
    unsigned int entry_position) {
  pipemgr_tbl_pkg_act_fn_entry_t *entry_ptr = stage_format->entry;
  for (uint32_t i = 0; i < stage_format->entrycount; ++i) {
    if (entry_position == entry_ptr->entry) {
      return entry_ptr;
    }
    /* Advance to the next entry pointer. */
    size_t memsize = entry_ptr->entryfieldcount *
                     sizeof(pipemgr_tbl_pkg_act_fn_entry_field_t);
    memsize += entry_ptr->totalconsttuples *
               sizeof(pipemgr_tbl_pkg_act_fn_field_constvalues_t);
    entry_ptr++;
    entry_ptr =
        (pipemgr_tbl_pkg_act_fn_entry_t *)((uint8_t *)entry_ptr + memsize);
  }
  return NULL;
}

/* Checks if a direct ADT is used for a given action function in a given stage.
 */
pipe_status_t pipe_mgr_entry_format_adt_tbl_used(rmt_dev_info_t *dev_info,
                                                 profile_id_t prof_id,
                                                 dev_stage_t stage_id,
                                                 pipe_adt_tbl_hdl_t adt_tbl_hdl,
                                                 pipe_act_fn_hdl_t act_fn_hdl,
                                                 bool *adt_used) {
  bf_dev_id_t devid = dev_info->dev_id;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_act_fn_handle_t *act_fn_hdl_ptr;
  pipemgr_tbl_pkg_act_fn_stage_format_t *stageformat_ptr;
  pipemgr_tbl_pkg_act_fn_entry_t *entry_ptr;
  pipemgr_tbl_pkg_act_fn_entry_field_t *field;

  if (dev_info->fake_rmt_cfg) {
    return PIPE_SUCCESS;
  }

  lut_ptr = get_adt_lut_entry(devid, prof_id, adt_tbl_hdl);
  if (!lut_ptr) {
    LOG_ERROR("%s: Unable to find entry formatting details for ADT handle 0x%x",
              __func__,
              adt_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  act_fn_hdl_ptr = find_adt_act_fn_handle(lut_ptr, act_fn_hdl);
  if (!act_fn_hdl_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for action function "
        "handle 0x%x in ADT 0x%x",
        __func__,
        act_fn_hdl,
        adt_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  stageformat_ptr = find_act_fn_stage_format(act_fn_hdl_ptr, stage_id);
  if (!stageformat_ptr) {
    /* The ADT, or at least this action, is not present in the requested stage.
     * This can happen when a multi-stage match table uses a direct ADT in one
     * stage but only immediate for action parameters in the second stage. */
    *adt_used = false;
    return PIPE_SUCCESS;
  }

  entry_ptr = stageformat_ptr->entry;
  field = entry_ptr->field;
  for (uint32_t i = 0; i < entry_ptr->entryfieldcount; i++) {
    switch (field->source) {
      case TBL_PKG_FIELD_SOURCE_ZERO:
        /* Does not require ADT. */
        break;
      case TBL_PKG_FIELD_SOURCE_SPEC:
      case TBL_PKG_FIELD_SOURCE_CONSTANT:
        /* ADT required. */
        *adt_used = true;
        return PIPE_SUCCESS;
      default:
        break;
    }
    uint32_t constcount = field->constvalue_count;
    field++;
    field = (pipemgr_tbl_pkg_act_fn_entry_field_t
                 *)((uint8_t *)field +
                    (constcount *
                     sizeof(pipemgr_tbl_pkg_act_fn_field_constvalues_t)));
  }

  *adt_used = false;
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_tof_adt_tbl_ent_update(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_adt_tbl_hdl_t adt_tbl_hdl,
    pipe_act_fn_hdl_t act_fn_hdl,
    pipe_action_data_spec_t *act_data_spec,
    uint8_t entry_position,
    uint8_t **adt_tbl_word) {
  uint32_t i, j, const_value, constcount;
  uint8_t const_start_offset, const_bit_width;
  uint8_t const_end_offset, shift, start_bit, width, end_bit;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_act_fn_handle_t *act_fn_hdl_ptr;
  pipemgr_tbl_pkg_act_fn_stage_format_t *stageformat_ptr;
  pipemgr_tbl_pkg_act_fn_entry_t *entry_ptr;
  pipemgr_tbl_pkg_act_fn_entry_field_t *field;
  pipemgr_tbl_pkg_act_fn_field_constvalues_t *constvalues;
  bf_dev_id_t devid = dev_info->dev_id;

  if (dev_info->fake_rmt_cfg) {
    return PIPE_SUCCESS;
  }

  lut_ptr = get_adt_lut_entry(devid, prof_id, adt_tbl_hdl);
  if (!lut_ptr) {
    LOG_ERROR("%s: Unable to find entry formatting details for ADT handle 0x%x",
              __func__,
              adt_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  act_fn_hdl_ptr = find_adt_act_fn_handle(lut_ptr, act_fn_hdl);
  if (!act_fn_hdl_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for action function "
        "handle 0x%x in ADT 0x%x",
        __func__,
        act_fn_hdl,
        adt_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  stageformat_ptr = find_act_fn_stage_format(act_fn_hdl_ptr, stage_id);
  if (!stageformat_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for action function "
        "handle 0x%x, stage %d, ADT 0x%x",
        __func__,
        act_fn_hdl,
        stage_id,
        adt_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  entry_ptr = find_act_fn_entry_format(stageformat_ptr, entry_position);
  if (!entry_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for action function "
        "handle 0x%x, stage %d, entry position %d, ADT 0x%x",
        __func__,
        act_fn_hdl,
        stage_id,
        entry_position,
        adt_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  field = entry_ptr->field;
  for (i = 0; i < entry_ptr->entryfieldcount; i++) {
    switch (field->source) {
      case TBL_PKG_FIELD_SOURCE_ZERO:
        set_val(adt_tbl_word,
                field->word_index,
                field->field_offset,
                field->field_width,
                0);
        break;
      case TBL_PKG_FIELD_SOURCE_SPEC:
        if (field->is_mod_field_conditionally_mask) {
          uint64_t val = 0;
          get_shifted_val(act_data_spec->action_data_bits,
                          field->source_offset,
                          field->source_width,
                          0,
                          &val);
          if (val) {
            /* For a conditionally modified field (mask), the passed in spec
             * will contain a byte of information (ideally should be a BOOL). A
             * value of 0 indicates the condition as FALSE, which will make us
             * encode a value of 0 for the entire field. A value of 1 indicates
             * a TRUE, which will make us encode a value of all 1's for the
             * entire field
             */
            uint16_t padded_field_width = (field->field_width + 7) / 8;
            uint8_t *data =
                (uint8_t *)PIPE_MGR_CALLOC(padded_field_width, sizeof(uint8_t));
            if (!data) {
              LOG_ERROR("%s:%d Malloc failure", __func__, __LINE__);
              return PIPE_NO_SYS_RESOURCES;
            }
            PIPE_MGR_MEMSET(data, 0xff, sizeof(uint8_t) * padded_field_width);
            copy_bits(adt_tbl_word,
                      field->word_index,
                      field->field_offset,
                      field->field_width,
                      data,
                      0,
                      padded_field_width * 8,
                      0);
            PIPE_MGR_FREE(data);
          } else {
            set_val(adt_tbl_word,
                    field->word_index,
                    field->field_offset,
                    field->field_width,
                    val);
          }
        } else if (field->is_mod_field_conditionally_value) {
          uint64_t val = 0;
          get_shifted_val(act_data_spec->action_data_bits,
                          field->mask_offset,
                          field->mask_width,
                          0,
                          &val);
          if (val) {
            /* Write the spec value if the condition is on */
            copy_bits(adt_tbl_word,
                      field->word_index,
                      field->field_offset,
                      field->field_width,
                      act_data_spec->action_data_bits,
                      field->source_offset,
                      field->source_width,
                      field->shift);
          } else {
            /* Write zero to the spec if the condition is off */
            uint16_t padded_field_width = (field->field_width + 7) / 8;
            uint8_t *data =
                (uint8_t *)PIPE_MGR_CALLOC(padded_field_width, sizeof(uint8_t));
            if (!data) {
              LOG_ERROR("%s:%d Malloc failure", __func__, __LINE__);
              return PIPE_NO_SYS_RESOURCES;
            }
            copy_bits(adt_tbl_word,
                      field->word_index,
                      field->field_offset,
                      field->field_width,
                      data,
                      0,
                      padded_field_width * 8,
                      0);
            PIPE_MGR_FREE(data);
          }
        } else {
          copy_bits(adt_tbl_word,
                    field->word_index,
                    field->field_offset,
                    field->field_width,
                    act_data_spec->action_data_bits,
                    field->source_offset,
                    field->source_width,
                    field->shift);
        }
        break;
      case TBL_PKG_FIELD_SOURCE_CONSTANT:
        constvalues = field->constvalues;
        for (j = 0; j < field->constvalue_count; j++) {
          uint64_t val = 0;

          if (field->is_mod_field_conditionally_value) {
            get_shifted_val(act_data_spec->action_data_bits,
                            field->mask_offset,
                            field->mask_width,
                            0,
                            &val);
          }

          if (field->is_mod_field_conditionally_value && !val) {
            set_val(adt_tbl_word,
                    field->word_index,
                    field->field_offset,
                    field->field_width,
                    val);
          } else {
            const_start_offset = constvalues->dststart;
            const_bit_width = constvalues->dstwidth;
            const_value = constvalues->constvalue;
            const_end_offset = const_start_offset + const_bit_width - 1;
            if ((const_start_offset <= field->shift) &&
                (const_end_offset >= field->shift)) {
              shift = field->shift - const_start_offset;
              start_bit = field->field_offset;
              width =
                  ENTRYFORMAT_MIN(field->field_width, const_bit_width - shift);
              set_val(adt_tbl_word,
                      field->word_index,
                      start_bit,
                      width,
                      const_value >> shift);
            } else if ((const_start_offset >= field->shift) &&
                       (const_start_offset <=
                        (field->shift + field->field_width - 1))) {
              end_bit = ENTRYFORMAT_MIN(
                  field->field_offset + field->field_width - 1,
                  field->field_offset + const_end_offset - field->shift);
              start_bit = field->field_offset + const_start_offset;
              width = end_bit - start_bit + 1;
              set_val(adt_tbl_word,
                      field->word_index,
                      start_bit,
                      width,
                      const_value);
            }
          }
          constvalues++;
        }
        break;

      default:
        break;
    }
    constcount = field->constvalue_count;
    field++;
    field = (pipemgr_tbl_pkg_act_fn_entry_field_t
                 *)((uint8_t *)field +
                    (constcount *
                     sizeof(pipemgr_tbl_pkg_act_fn_field_constvalues_t)));
  }

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_tof_tern_tbl_ent_update(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint8_t version_valid_bits,
    pipe_tbl_match_spec_t *match_spec,
    bool mrd_terminate,
    bool *mrd_terminate_indices,
    uint8_t *tbl_words[],
    uint32_t tbl_words_count,
    bool tcam_words_updated[TOF_MAX_TCAM_WORDS_IN_TERN_TBL_WORD]) {
  uint64_t range_mask[TOF_MAX_TCAM_WORDS_IN_TERN_TBL_WORD] = {0};
  uint32_t bj_hash, i, j, k;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_tern_range_mask_t *rangemask;
  pipemgr_tbl_pkg_tern_entry_t *entry;
  pipemgr_tbl_pkg_tern_entry_field_t *field, *version_field = NULL;
  uint8_t version_key, version_msk, fieldcount;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tern_lut_depth,
      mat_tbl_hdl,
      stage_id,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tern_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tern_lut,
      mat_tbl_hdl,
      stage_id);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for tern table handle "
        "0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  rangemask = lut_ptr->u.tern_ptr->rangeformat;
  for (i = 0; i < lut_ptr->u.tern_ptr->rangemaskcount; i++) {
    PIPE_MGR_DBGCHK(rangemask->word_index <
                    TOF_MAX_TCAM_WORDS_IN_TERN_TBL_WORD);
    range_mask[rangemask->word_index] = rangemask->mask;
    rangemask++;
  }

  /* Pre-populate all range words to match everything. When programming the
   * actual ranges, we will remove the pieces that fall outside each range.
   */
  entry = lut_ptr->u.tern_ptr->entry;
  for (i = 0; i < lut_ptr->u.tern_ptr->entrycount; i++) {
    field = entry->field;
    for (j = 0; j < entry->entryfieldcount; j++) {
      if (tbl_words_count <= field->memword_index[0]) {
        return PIPE_INVALID_ARG;
      }
      if (field->location == TBL_PKG_TERN_FIELD_LOCATION_RANGE) {
        uint8_t width = field->range_type;
        uint8_t base = ((field->lsbmemwordoffset - 1) / width) * width + 1;
        set_val_old(tbl_words[field->memword_index[0]],
                    base,
                    width,
                    ((uint64_t)1 << width) - 1);
        set_val_old(&tbl_words[field->memword_index[0]][8],
                    base,
                    width,
                    ((uint64_t)1 << width) - 1);
      }
      field++;
    }

    fieldcount = entry->entryfieldcount;
    entry++;
    entry = (pipemgr_tbl_pkg_tern_entry_t
                 *)((uint8_t *)entry +
                    sizeof(pipemgr_tbl_pkg_tern_entry_field_t) * fieldcount);
  }

  entry = lut_ptr->u.tern_ptr->entry;
  for (i = 0; i < lut_ptr->u.tern_ptr->entrycount; i++) {
    field = entry->field;
    for (j = 0; j < entry->entryfieldcount; j++) {
      if (tbl_words_count <= field->memword_index[0]) {
        return PIPE_INVALID_ARG;
      }
      switch (field->location) {
        case TBL_PKG_TERN_FIELD_LOCATION_ZERO:
          set_val_old(tbl_words[field->memword_index[0]],
                      field->lsbmemwordoffset,
                      field->bitwidth,
                      0);
          set_val_old(&tbl_words[field->memword_index[0]][8],
                      field->lsbmemwordoffset,
                      field->bitwidth,
                      0);
          for (k = field->memword_index[0]; k < (field->memword_index[1] + 1);
               k++) {
            tcam_words_updated[k] = true;
          }
          break;
        case TBL_PKG_TERN_FIELD_LOCATION_PARITY:
          set_val_old(tbl_words[field->memword_index[0]],
                      field->lsbmemwordoffset,
                      field->bitwidth,
                      0);
          set_val_old(&tbl_words[field->memword_index[0]][8],
                      field->lsbmemwordoffset,
                      field->bitwidth,
                      0);
          break;
        case TBL_PKG_TERN_FIELD_LOCATION_PAYLOAD:
          // Note that MR bit (in word0[0]) has been inverted, now '0' means
          // perform multi-range match, '1' means do not
          set_val_old(
              tbl_words[field->memword_index[0]],
              field->lsbmemwordoffset,
              field->bitwidth,
              (mrd_terminate || mrd_terminate_indices[field->memword_index[0]])
                  ? 1
                  : 0);
          set_val_old(&tbl_words[field->memword_index[0]][8],
                      field->lsbmemwordoffset,
                      field->bitwidth,
                      0);
          for (k = field->memword_index[0]; k < field->memword_index[1] + 1;
               k++) {
            tcam_words_updated[k] = true;
          }
          break;
        case TBL_PKG_TERN_FIELD_LOCATION_VERSION:
          version_key = (version_valid_bits >> 2) & 0x03;
          version_msk = (version_valid_bits >> 0) & 0x03;
          set_key_mask_no_encoding(tbl_words[field->memword_index[0]],
                                   &tbl_words[field->memword_index[0]][8],
                                   field->lsbmemwordoffset,
                                   field->bitwidth,
                                   &version_key,
                                   &version_msk,
                                   0,
                                   2,
                                   0);
          for (k = field->memword_index[0]; k < field->memword_index[1] + 1;
               k++) {
            tcam_words_updated[k] = true;
          }
          version_field = field;
          break;
        case TBL_PKG_TERN_FIELD_LOCATION_RANGE:
          set_range(tbl_words[field->memword_index[0]],
                    &tbl_words[field->memword_index[0]][8],
                    field->lsbmemwordoffset + field->range_nibble_offset,
                    field->bitwidth,
                    match_spec->match_value_bits,
                    match_spec->match_mask_bits,
                    field->srcoffset,
                    field->src_len,
                    field->startbit,
                    field->range_type,
                    field->range_hi_byte ? 1 : 0);
          for (k = field->memword_index[0]; k < field->memword_index[1] + 1;
               k++) {
            tcam_words_updated[k] = true;
          }
          break;
        case TBL_PKG_TERN_FIELD_LOCATION_SPEC:
          set_key_mask_no_encoding(tbl_words[field->memword_index[0]],
                                   &tbl_words[field->memword_index[0]][8],
                                   field->lsbmemwordoffset,
                                   field->bitwidth,
                                   match_spec->match_value_bits,
                                   match_spec->match_mask_bits,
                                   field->srcoffset,
                                   field->src_len,
                                   field->startbit);
          for (k = field->memword_index[0]; k < field->memword_index[1] + 1;
               k++) {
            tcam_words_updated[k] = true;
          }
          break;
        default:
          LOG_ERROR(
              "%s: Unable to find entry formatting details for tern table "
              "handle "
              "0x%x",
              __func__,
              mat_tbl_hdl);
          PIPE_MGR_DBGCHK(0);
          return (PIPE_INVALID_ARG);
      }
      field++;
    }

    fieldcount = entry->entryfieldcount;
    entry++;
    entry = (pipemgr_tbl_pkg_tern_entry_t
                 *)((uint8_t *)entry +
                    sizeof(pipemgr_tbl_pkg_tern_entry_field_t) * fieldcount);
  }

  for (i = 0; i < TOF_MAX_TCAM_WORDS_IN_TERN_TBL_WORD; i++) {
    if (tcam_words_updated[i] == true) {







        bool version = (version_field && version_field->memword_index[0] == i);
        set_key_mask_2bit_mode(
            tbl_words[i], &tbl_words[i][8], range_mask[i], version);

    }
  }
  return PIPE_SUCCESS;
}

static void pipe_mgr_entry_format_tof_tbl_default_entry_prepare(
    rmt_dev_info_t *dev_info,
    bf_dev_pipe_t log_pipe,
    int stage,
    int logical_table,
    pipe_register_spec_t *register_spec_list) {
  bf_dev_pipe_t phy_pipe = 0;
  pipe_mgr_map_pipe_id_log_to_phy(dev_info, log_pipe, &phy_pipe);

  /* mau_immediate_data_miss_value */
  register_spec_list[0].reg_addr = offsetof(
      Tofino,
      pipes[phy_pipe]
          .mau[stage]
          .rams.match.merge.mau_immediate_data_miss_value[logical_table]);
  /* mau_action_insruction_adr_miss_value */
  register_spec_list[1].reg_addr =
      offsetof(Tofino,
               pipes[phy_pipe]
                   .mau[stage]
                   .rams.match.merge
                   .mau_action_instruction_adr_miss_value[logical_table]);
  /* mau_action_data_adr_miss_value */
  register_spec_list[2].reg_addr = offsetof(
      Tofino,
      pipes[phy_pipe]
          .mau[stage]
          .rams.match.merge.mau_actiondata_adr_miss_value[logical_table]);
  /* mau_stats_adr_miss_value */
  register_spec_list[3].reg_addr =
      offsetof(Tofino,
               pipes[phy_pipe]
                   .mau[stage]
                   .rams.match.merge.mau_stats_adr_miss_value[logical_table]);
  /* mau_meter_adr_miss_value */
  register_spec_list[4].reg_addr =
      offsetof(Tofino,
               pipes[phy_pipe]
                   .mau[stage]
                   .rams.match.merge.mau_meter_adr_miss_value[logical_table]);
  /* mau_idletime_adr_miss_value */
  register_spec_list[5].reg_addr = offsetof(
      Tofino,
      pipes[phy_pipe]
          .mau[stage]
          .rams.match.merge.mau_idletime_adr_miss_value[logical_table]);
  /* next_table_format_data */
  register_spec_list[6].reg_addr =
      offsetof(Tofino,
               pipes[phy_pipe]
                   .mau[stage]
                   .rams.match.merge.next_table_format_data[logical_table]);
}

static void pipe_mgr_entry_format_tof2_tbl_default_entry_prepare(
    rmt_dev_info_t *dev_info,
    bf_dev_pipe_t log_pipe,
    int stage,
    int logical_table,
    pipe_register_spec_t *register_spec_list) {
  bf_dev_pipe_t phy_pipe = 0;
  pipe_mgr_map_pipe_id_log_to_phy(dev_info, log_pipe, &phy_pipe);

  /* mau_immediate_data_miss_value */
  register_spec_list[0].reg_addr = offsetof(
      tof2_reg,
      pipes[phy_pipe]
          .mau[stage]
          .rams.match.merge.mau_immediate_data_miss_value[logical_table]);
  /* mau_action_insruction_adr_miss_value */
  register_spec_list[1].reg_addr =
      offsetof(tof2_reg,
               pipes[phy_pipe]
                   .mau[stage]
                   .rams.match.merge
                   .mau_action_instruction_adr_miss_value[logical_table]);
  /* mau_action_data_adr_miss_value */
  register_spec_list[2].reg_addr = offsetof(
      tof2_reg,
      pipes[phy_pipe]
          .mau[stage]
          .rams.match.merge.mau_actiondata_adr_miss_value[logical_table]);
  /* mau_stats_adr_miss_value */
  register_spec_list[3].reg_addr =
      offsetof(tof2_reg,
               pipes[phy_pipe]
                   .mau[stage]
                   .rams.match.merge.mau_stats_adr_miss_value[logical_table]);
  /* mau_meter_adr_miss_value */
  register_spec_list[4].reg_addr =
      offsetof(tof2_reg,
               pipes[phy_pipe]
                   .mau[stage]
                   .rams.match.merge.mau_meter_adr_miss_value[logical_table]);
  /* mau_idletime_adr_miss_value */
  register_spec_list[5].reg_addr = offsetof(
      tof2_reg,
      pipes[phy_pipe]
          .mau[stage]
          .rams.match.merge.mau_idletime_adr_miss_value[logical_table]);
  /* next_table_format_data */
  register_spec_list[6].reg_addr =
      offsetof(tof2_reg,
               pipes[phy_pipe]
                   .mau[stage]
                   .rams.match.merge.next_table_format_data[logical_table]);
  /* pred_miss_exec */
  register_spec_list[7].reg_addr =
      offsetof(tof2_reg,
               pipes[phy_pipe]
                   .mau[stage]
                   .rams.match.merge.pred_miss_exec[logical_table]);
  /* pred_miss_long_brch */
  register_spec_list[8].reg_addr =
      offsetof(tof2_reg,
               pipes[phy_pipe]
                   .mau[stage]
                   .rams.match.merge.pred_miss_long_brch[logical_table]);
}

static void pipe_mgr_entry_format_tof3_tbl_default_entry_prepare(
    rmt_dev_info_t *dev_info,
    bf_dev_pipe_t log_pipe,
    int stage,
    int logical_table,
    pipe_register_spec_t *register_spec_list) {
  bf_dev_pipe_t phy_pipe = 0;
  pipe_mgr_map_pipe_id_log_to_phy(dev_info, log_pipe, &phy_pipe);
  phy_pipe = phy_pipe % BF_SUBDEV_PIPE_COUNT;

  /* mau_immediate_data_miss_value */
  register_spec_list[0].reg_addr = offsetof(
      tof3_reg,
      pipes[phy_pipe]
          .mau[stage]
          .rams.match.merge.mau_immediate_data_miss_value[logical_table]);
  /* mau_action_insruction_adr_miss_value */
  register_spec_list[1].reg_addr =
      offsetof(tof3_reg,
               pipes[phy_pipe]
                   .mau[stage]
                   .rams.match.merge
                   .mau_action_instruction_adr_miss_value[logical_table]);
  /* mau_action_data_adr_miss_value */
  register_spec_list[2].reg_addr = offsetof(
      tof3_reg,
      pipes[phy_pipe]
          .mau[stage]
          .rams.match.merge.mau_actiondata_adr_miss_value[logical_table]);
  /* mau_stats_adr_miss_value */
  register_spec_list[3].reg_addr =
      offsetof(tof3_reg,
               pipes[phy_pipe]
                   .mau[stage]
                   .rams.match.merge.mau_stats_adr_miss_value[logical_table]);
  /* mau_meter_adr_miss_value */
  register_spec_list[4].reg_addr =
      offsetof(tof3_reg,
               pipes[phy_pipe]
                   .mau[stage]
                   .rams.match.merge.mau_meter_adr_miss_value[logical_table]);
  /* mau_idletime_adr_miss_value */
  register_spec_list[5].reg_addr = offsetof(
      tof3_reg,
      pipes[phy_pipe]
          .mau[stage]
          .rams.match.merge.mau_idletime_adr_miss_value[logical_table]);
  /* next_table_format_data */
  register_spec_list[6].reg_addr =
      offsetof(tof3_reg,
               pipes[phy_pipe]
                   .mau[stage]
                   .rams.match.merge.next_table_format_data[logical_table]);
  /* pred_miss_exec */
  register_spec_list[7].reg_addr =
      offsetof(tof3_reg,
               pipes[phy_pipe]
                   .mau[stage]
                   .rams.match.merge.pred_miss_exec[logical_table]);
  /* pred_miss_long_brch */
  register_spec_list[8].reg_addr =
      offsetof(tof3_reg,
               pipes[phy_pipe]
                   .mau[stage]
                   .rams.match.merge.pred_miss_long_brch[logical_table]);
}




































































































pipe_status_t pipe_mgr_entry_format_tbl_default_entry_prepare(
    rmt_dev_info_t *dev_info,
    uint8_t direction,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    bf_dev_pipe_t log_pipe,
    pipe_register_spec_t *register_spec_list) {
  uint32_t bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(dev_info->dev_id, prof_id).dft_lut_depth,
      mat_tbl_hdl,
      0,
      0);
  pipemgr_tbl_pkg_lut_t *lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(dev_info->dev_id, prof_id).dft_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(dev_info->dev_id, prof_id).dft_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for default entry for "
        "table handle 0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }
  int logical_table = lut_ptr->u.dft_ptr->logicalid;

  int stage = lut_ptr->u.dft_ptr->stage;
  // keep compiler happy and not return unused error
  (void)direction;
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      pipe_mgr_entry_format_tof_tbl_default_entry_prepare(
          dev_info, log_pipe, stage, logical_table, register_spec_list);
      return PIPE_SUCCESS;
    case BF_DEV_FAMILY_TOFINO2:
      pipe_mgr_entry_format_tof2_tbl_default_entry_prepare(
          dev_info, log_pipe, stage, logical_table, register_spec_list);
      return PIPE_SUCCESS;
    case BF_DEV_FAMILY_TOFINO3:
      pipe_mgr_entry_format_tof3_tbl_default_entry_prepare(
          dev_info, log_pipe, stage, logical_table, register_spec_list);
      return PIPE_SUCCESS;












    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }
}

void pipe_mgr_entry_format_tbl_default_entry_print(
    rmt_dev_info_t *dev_info,
    pipe_register_spec_t *register_spec_list,
    bf_dev_pipe_t pipe,
    char *str,
    int *c_str_len,
    int max_len) {
  int c_len = *c_str_len;
  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "---- | ------------------------------------- | "
                    "------------ | ------------\n");
  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "%-4s |  %-36s | %-12s | %-12s\n",
                    "Pipe",
                    "Register-Name",
                    "Address",
                    "Value");

  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "---- | ------------------------------------- | "
                    "------------ | ------------\n");

  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "%-4d |  %-36s | 0x%-10x | 0x%-10x \n",
                    pipe,
                    "mau_immediate_data_miss_value",
                    register_spec_list[0].reg_addr,
                    register_spec_list[0].reg_data);

  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "%-4d |  %-36s | 0x%-10x | 0x%-10x \n",
                    pipe,
                    "mau_action_insruction_adr_miss_value",
                    register_spec_list[1].reg_addr,
                    register_spec_list[1].reg_data);

  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "%-4d |  %-36s | 0x%-10x | 0x%-10x \n",
                    pipe,
                    "mau_action_data_adr_miss_value",
                    register_spec_list[2].reg_addr,
                    register_spec_list[2].reg_data);

  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "%-4d |  %-36s | 0x%-10x | 0x%-10x \n",
                    pipe,
                    "mau_stats_adr_miss_value",
                    register_spec_list[3].reg_addr,
                    register_spec_list[3].reg_data);

  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "%-4d |  %-36s | 0x%-10x | 0x%-10x \n",
                    pipe,
                    "mau_meter_adr_miss_value",
                    register_spec_list[4].reg_addr,
                    register_spec_list[4].reg_data);

  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "%-4d |  %-36s | 0x%-10x | 0x%-10x \n",
                    pipe,
                    "mau_idletime_adr_miss_value",
                    register_spec_list[5].reg_addr,
                    register_spec_list[5].reg_data);

  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "%-4d |  %-36s | 0x%-10x | 0x%-10x \n",
                    pipe,
                    "next_table_format_data",
                    register_spec_list[6].reg_addr,
                    register_spec_list[6].reg_data);
  if (dev_info->dev_family != BF_DEV_FAMILY_TOFINO) {
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "%-4d |  %-36s | 0x%-10x | 0x%-10x \n",
                      pipe,
                      "next_table_exec",
                      register_spec_list[7].reg_addr,
                      register_spec_list[7].reg_data);
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "%-4d |  %-36s | 0x%-10x | 0x%-10x \n",
                      pipe,
                      "next_table_long_branch",
                      register_spec_list[8].reg_addr,
                      register_spec_list[8].reg_data);
  }
  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "---- | ------------------------------------- | "
                    "------------ | ------------\n");

  *c_str_len = c_len;
}

/* Helper function for pipe_mgr_entry_format_tof*_tbl_default_entry_update()
   to extract pipe_default_ent_reg_data from the action data
*/
static pipe_status_t get_default_ent_reg_data(
    rmt_dev_info_t *dev_info,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_act_fn_hdl_t act_fn_hdl,
    pipe_action_data_spec_t *act_data_spec,
    pipe_default_ent_reg_data *reg_data,
    pipemgr_tbl_pkg_lut_t *lut_ptr,
    pipemgr_tbl_pkg_default_action_fn_t **action_ptr) {
  uint32_t i;
  pipemgr_tbl_pkg_default_action_param_t *action_param_ptr;
  pipemgr_tbl_pkg_action_parameter_field_t *act_field;
  uint16_t imm_start_offset, imm_bit_width, param_count;

  action_param_ptr = lut_ptr->u.dft_ptr->act_param;
  *action_ptr = lut_ptr->u.dft_ptr->actions;
  for (i = 0; i < lut_ptr->u.dft_ptr->totalactionhandles; i++) {
    if ((*action_ptr)->action_fn_handle == act_fn_hdl) {
      break;
    }
    (*action_ptr)++;
  }
  if (i >= lut_ptr->u.dft_ptr->totalactionhandles) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for default entry for "
        "table handle "
        "0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_ASSERT(0);
    return PIPE_INVALID_ARG;
  }
  for (i = 0; i < lut_ptr->u.dft_ptr->totalactionhandles; i++) {
    if (action_param_ptr->action_fn_handle == act_fn_hdl) {
      break;
    }
    param_count = action_param_ptr->act_param_count;
    action_param_ptr++;
    action_param_ptr =
        (pipemgr_tbl_pkg_default_action_param_t
             *)((uint8_t *)action_param_ptr +
                (param_count *
                 sizeof(pipemgr_tbl_pkg_action_parameter_field_t)));
  }
  if (i >= lut_ptr->u.dft_ptr->totalactionhandles) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for default entry for "
        "table handle "
        "0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_ASSERT(0);
    return PIPE_INVALID_ARG;
  }
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      break;
    case BF_DEV_FAMILY_TOFINO2:
    case BF_DEV_FAMILY_TOFINO3:
      reg_data->next_table_exec = (*action_ptr)->next_tbl_exec;
      reg_data->next_table_long_branch = (*action_ptr)->next_tbl_long_branch;
      break;





    default:
      return PIPE_INVALID_ARG;
  }
  reg_data->vliw_addr = (*action_ptr)->instr;
  reg_data->next_table = (*action_ptr)->next_tbl;
  reg_data->table_mask = (*action_ptr)->tbl_mask;
  reg_data->next_table_default = (*action_ptr)->next_tbl_default;
  reg_data->immediate_val = 0;
  act_field = action_param_ptr->act_field;
  for (i = 0; i < action_param_ptr->act_param_count; i++) {
    imm_start_offset = act_field->start_offset;
    imm_bit_width = act_field->bit_width;
    switch (dev_info->dev_family) {
      case BF_DEV_FAMILY_TOFINO:
        if (act_field->is_parameter && act_data_spec) {
          if (act_field->is_mod_field_conditionally_mask) {
            uint64_t val = 0;
            get_shifted_val(act_data_spec->action_data_bits,
                            act_field->param_start,
                            act_field->param_width,
                            0,
                            &val);
            if (val) {
              /* For a conditionally modified field (mask), the passed in spec
               * will contain a byte of information (ideally should be a BOOL).
               * A value of 0 indicates the condition as FALSE, which will make
               * us encode a value of 0 for the entire field. A value of 1
               * indicates a TRUE, which will make us encode a value of all 1's
               * for the entire field
               */
              uint16_t padded_field_width = (imm_bit_width + 7) / 8;
              uint8_t *data = (uint8_t *)PIPE_MGR_CALLOC(padded_field_width,
                                                         sizeof(uint8_t));
              if (!data) {
                LOG_ERROR("%s:%d Malloc failure", __func__, __LINE__);
                return PIPE_NO_SYS_RESOURCES;
              }
              PIPE_MGR_MEMSET(data, 0xff, sizeof(uint8_t) * padded_field_width);
              copy_bits_old((uint8_t *)&(reg_data->immediate_val),
                            imm_start_offset,
                            imm_bit_width,
                            data,
                            0,
                            padded_field_width * 8,
                            0);
              PIPE_MGR_FREE(data);
            } else {
              set_val_old((uint8_t *)&(reg_data->immediate_val),
                          imm_start_offset,
                          imm_bit_width,
                          val);
            }
          } else if (act_field->is_mod_field_conditionally_value) {
            uint64_t val = 0;
            get_shifted_val(act_data_spec->action_data_bits,
                            act_field->mask_offset,
                            act_field->mask_width,
                            0,
                            &val);
            if (val) {
              /* Write the spec value if the condition is on */
              copy_bits_old((uint8_t *)&(reg_data->immediate_val),
                            imm_start_offset,
                            imm_bit_width,
                            act_data_spec->action_data_bits,
                            act_field->param_start,
                            act_field->param_width,
                            act_field->param_shift);
            } else {
              set_val_old((uint8_t *)&(reg_data->immediate_val),
                          imm_start_offset,
                          imm_bit_width,
                          val);
            }
          } else {
            copy_bits_old((uint8_t *)&(reg_data->immediate_val),
                          imm_start_offset,
                          imm_bit_width,
                          act_data_spec->action_data_bits,
                          act_field->param_start,
                          act_field->param_width,
                          act_field->param_shift);
          }
        } else {
          set_val_old((uint8_t *)&(reg_data->immediate_val),
                      imm_start_offset,
                      imm_bit_width,
                      act_field->constant_value);
        }
        act_field++;
        break;
      case BF_DEV_FAMILY_TOFINO2:
      case BF_DEV_FAMILY_TOFINO3:

        if (act_field->is_parameter && act_data_spec) {
          copy_bits_old((uint8_t *)&(reg_data->immediate_val),
                        imm_start_offset,
                        imm_bit_width,
                        act_data_spec->action_data_bits,
                        act_field->param_start,
                        act_field->param_width,
                        act_field->param_shift);
        } else {
          set_val_old((uint8_t *)&(reg_data->immediate_val),
                      imm_start_offset,
                      imm_bit_width,
                      act_field->constant_value);
        }
        act_field++;
        break;
      default:
        return PIPE_UNEXPECTED;
    }
  }
  return PIPE_SUCCESS;
}
static pipe_status_t pipe_mgr_entry_format_tof_tbl_default_entry_update(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_act_fn_hdl_t act_fn_hdl,
    pipe_action_data_spec_t *act_data_spec,  // Only for immediate action data.
    uint32_t indirect_action_ptr,
    uint32_t indirect_stats_ptr,
    uint32_t indirect_meter_ptr,
    uint32_t indirect_stful_ptr,
    uint32_t indirect_idle_ptr,
    uint32_t indirect_sel_ptr,
    uint32_t selector_len,
    pipe_register_spec_t *register_spec_list) {
  bf_dev_id_t devid = dev_info->dev_id;
  uint8_t logical_table, stage;
  pipe_default_ent_reg_data reg_data;
  uint32_t indirect_sel_stful_meter_ptr;
  uint32_t bj_hash, sel_default_val, sel_default_mask;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_default_action_fn_t *action_ptr;

  if (selector_len > 1) {
    return PIPE_NOT_SUPPORTED;
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut_depth, mat_tbl_hdl, 0, 0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for default entry for "
        "table handle "
        "0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }
  logical_table = lut_ptr->u.dft_ptr->logicalid;
  stage = lut_ptr->u.dft_ptr->stage;

  pipe_status_t status = get_default_ent_reg_data(dev_info,
                                                  mat_tbl_hdl,
                                                  act_fn_hdl,
                                                  act_data_spec,
                                                  &reg_data,
                                                  lut_ptr,
                                                  &action_ptr);

  if (status != PIPE_SUCCESS) return status;

  if (action_ptr->force_stats_addr) {
    indirect_stats_ptr = action_ptr->forced_full_stats_addr;
  }
  if (action_ptr->force_meter_addr) {
    indirect_meter_ptr = action_ptr->forced_full_meter_addr;
  }
  if (action_ptr->force_stful_addr) {
    indirect_stful_ptr = action_ptr->forced_full_stful_addr;
  }
  if (action_ptr->force_stats_pfe_set) {
    indirect_stats_ptr |= 1 << TOF_STATS_RAM_NUM_ADDR_BITS;
  }
  if (action_ptr->force_meter_pfe_set) {
    indirect_meter_ptr |= 1 << TOF_METER_ADDR_PFE_BIT_POSITION;
  }
  if (action_ptr->force_stful_pfe_set) {
    indirect_stful_ptr |= 1 << 23;
  }
  indirect_sel_stful_meter_ptr =
      indirect_sel_ptr | indirect_stful_ptr | indirect_meter_ptr;
  /* Can have at most one select, stateful, and meter pointer. */
  PIPE_MGR_DBGCHK(
      !indirect_sel_stful_meter_ptr ||
      (indirect_sel_ptr && !indirect_stful_ptr && !indirect_meter_ptr) ||
      (!indirect_sel_ptr && indirect_stful_ptr && !indirect_meter_ptr) ||
      (!indirect_sel_ptr && !indirect_stful_ptr && indirect_meter_ptr));

  /* Get all the register address values. */
  pipe_mgr_entry_format_tof_tbl_default_entry_prepare(
      dev_info, 0, stage, logical_table, register_spec_list);
  /* Populate the data in the register spec array. */
  /* mau_immediate_data_miss_value */
  setp_mau_immediate_data_miss_value_mau_immediate_data_miss_value(
      &register_spec_list[0].reg_data, reg_data.immediate_val);
  /* mau_action_insruction_adr_miss_value */
  setp_mau_action_instruction_adr_miss_value_mau_action_instruction_adr_miss_value(
      &register_spec_list[1].reg_data, reg_data.vliw_addr);
  /* mau_action_data_adr_miss_value */
  setp_mau_actiondata_adr_miss_value_mau_actiondata_adr_miss_value(
      &register_spec_list[2].reg_data, indirect_action_ptr);
  /* mau_stats_adr_miss_value */
  setp_mau_stats_adr_miss_value_mau_stats_adr_miss_value(
      &register_spec_list[3].reg_data, indirect_stats_ptr);
  /* mau_meter_adr_miss_value */
  if (lut_ptr->u.dft_ptr->selectorcount) {
    sel_default_val = (1 << TOF_METER_ADDR_SEL_TYPE_BIT_POSITION);
    sel_default_mask = (1 << TOF_METER_ADDR_METER_TYPE_BIT_POSITION) - 1;
    sel_default_mask -= (1 << TOF_SELECTOR_HUFFMAN_BITS) - 1;
    indirect_sel_ptr = sel_default_val | (indirect_sel_ptr & sel_default_mask);
    if (selector_len) {
      indirect_sel_ptr |= (1 << TOF_METER_ADDR_PFE_BIT_POSITION);
    } else {
      indirect_sel_ptr &= ~(1 << TOF_METER_ADDR_PFE_BIT_POSITION);
    }
    setp_mau_meter_adr_miss_value_mau_meter_adr_miss_value(
        &register_spec_list[4].reg_data, indirect_sel_ptr);
  } else {
    setp_mau_meter_adr_miss_value_mau_meter_adr_miss_value(
        &register_spec_list[4].reg_data, indirect_sel_stful_meter_ptr);
  }
  /* mau_idletime_adr_miss_value */
  setp_mau_idletime_adr_miss_value_mau_idletime_adr_miss_value(
      &register_spec_list[5].reg_data, indirect_idle_ptr);
  /* next_table_format_data */
  setp_next_table_format_data_match_next_table_adr_miss_value(
      &register_spec_list[6].reg_data, reg_data.next_table);
  setp_next_table_format_data_match_next_table_adr_default(
      &register_spec_list[6].reg_data, reg_data.next_table_default);
  setp_next_table_format_data_match_next_table_adr_mask(
      &register_spec_list[6].reg_data, reg_data.table_mask);

  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_entry_format_tof2_tbl_default_entry_update(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_act_fn_hdl_t act_fn_hdl,
    pipe_action_data_spec_t *act_data_spec,  // Only for immediate action data.
    uint32_t indirect_action_ptr,
    uint32_t indirect_stats_ptr,
    uint32_t indirect_meter_ptr,
    uint32_t indirect_stful_ptr,
    uint32_t indirect_idle_ptr,
    uint32_t indirect_sel_ptr,
    uint32_t selector_len,
    pipe_register_spec_t *register_spec_list) {
  bf_dev_id_t devid = dev_info->dev_id;
  pipe_default_ent_reg_data reg_data;
  uint32_t logical_table, stage;
  uint32_t indirect_sel_stful_meter_ptr;
  uint32_t bj_hash, sel_default_val, sel_default_mask;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_default_action_fn_t *action_ptr;

  if (selector_len > 1) {
    return PIPE_NOT_SUPPORTED;
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut_depth, mat_tbl_hdl, 0, 0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for default entry for "
        "table handle "
        "0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }
  logical_table = lut_ptr->u.dft_ptr->logicalid;
  stage = lut_ptr->u.dft_ptr->stage;

  pipe_status_t status = get_default_ent_reg_data(dev_info,
                                                  mat_tbl_hdl,
                                                  act_fn_hdl,
                                                  act_data_spec,
                                                  &reg_data,
                                                  lut_ptr,
                                                  &action_ptr);

  if (status != PIPE_SUCCESS) return status;

  if (action_ptr->force_stats_addr) {
    indirect_stats_ptr = action_ptr->forced_full_stats_addr;
  }
  if (action_ptr->force_meter_addr) {
    indirect_meter_ptr = action_ptr->forced_full_meter_addr;
  }
  if (action_ptr->force_stful_addr) {
    indirect_stful_ptr = action_ptr->forced_full_stful_addr;
  }
  if (action_ptr->force_stats_pfe_set) {
    indirect_stats_ptr |= 1 << TOF2_STATS_ADDR_PFE_BIT_POSITION;
  }
  if (action_ptr->force_meter_pfe_set) {
    indirect_meter_ptr |= 1 << TOF2_METER_ADDR_PFE_BIT_POSITION;
  }
  if (action_ptr->force_stful_pfe_set) {
    indirect_stful_ptr |= 1 << 23;
  }
  indirect_sel_stful_meter_ptr =
      indirect_sel_ptr | indirect_stful_ptr | indirect_meter_ptr;
  /* Can have at most one select, stateful, and meter pointer. */
  PIPE_MGR_DBGCHK(
      !indirect_sel_stful_meter_ptr ||
      (indirect_sel_ptr && !indirect_stful_ptr && !indirect_meter_ptr) ||
      (!indirect_sel_ptr && indirect_stful_ptr && !indirect_meter_ptr) ||
      (!indirect_sel_ptr && !indirect_stful_ptr && indirect_meter_ptr));

  /* Get all the register address values. */
  pipe_mgr_entry_format_tof2_tbl_default_entry_prepare(
      dev_info, 0, stage, logical_table, register_spec_list);
  /* Populate the data in the register spec array. */
  /* mau_immediate_data_miss_value */
  setp_tof2_mau_immediate_data_miss_value_mau_immediate_data_miss_value(
      &register_spec_list[0].reg_data, reg_data.immediate_val);
  /* mau_action_insruction_adr_miss_value */
  setp_tof2_mau_action_instruction_adr_miss_value_mau_action_instruction_adr_miss_value(
      &register_spec_list[1].reg_data, reg_data.vliw_addr);
  /* mau_action_data_adr_miss_value */
  setp_tof2_mau_actiondata_adr_miss_value_mau_actiondata_adr_miss_value(
      &register_spec_list[2].reg_data, indirect_action_ptr);
  /* mau_stats_adr_miss_value */
  setp_tof2_mau_stats_adr_miss_value_mau_stats_adr_miss_value(
      &register_spec_list[3].reg_data, indirect_stats_ptr);
  /* mau_meter_adr_miss_value */
  if (lut_ptr->u.dft_ptr->selectorcount) {
    sel_default_val = (1 << TOF2_METER_ADDR_SEL_TYPE_BIT_POSITION);
    sel_default_mask = (1 << TOF2_METER_ADDR_METER_TYPE_BIT_POSITION) - 1;
    sel_default_mask -= (1 << TOF2_SELECTOR_HUFFMAN_BITS) - 1;
    indirect_sel_ptr = sel_default_val | (indirect_sel_ptr & sel_default_mask);
    if (selector_len) {
      indirect_sel_ptr |= (1 << TOF2_METER_ADDR_PFE_BIT_POSITION);
    } else {
      indirect_sel_ptr &= ~(1 << TOF2_METER_ADDR_PFE_BIT_POSITION);
    }
    setp_tof2_mau_meter_adr_miss_value_mau_meter_adr_miss_value(
        &register_spec_list[4].reg_data, indirect_sel_ptr);
  } else {
    setp_tof2_mau_meter_adr_miss_value_mau_meter_adr_miss_value(
        &register_spec_list[4].reg_data, indirect_sel_stful_meter_ptr);
  }
  /* mau_idletime_adr_miss_value */
  setp_tof2_mau_idletime_adr_miss_value_mau_idletime_adr_miss_value(
      &register_spec_list[5].reg_data, indirect_idle_ptr);
  /* next_table_format_data */
  setp_tof2_next_table_format_data_match_next_table_adr_miss_value(
      &register_spec_list[6].reg_data, reg_data.next_table);
  setp_tof2_next_table_format_data_match_next_table_adr_default(
      &register_spec_list[6].reg_data, reg_data.next_table_default);
  setp_tof2_next_table_format_data_match_next_table_adr_mask(
      &register_spec_list[6].reg_data, reg_data.table_mask);
  /* pred_miss_exec */
  register_spec_list[7].reg_data = reg_data.next_table_exec;
  /* pred_miss_long_brch */
  register_spec_list[8].reg_data = reg_data.next_table_long_branch;

  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_entry_format_tof3_tbl_default_entry_update(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_act_fn_hdl_t act_fn_hdl,
    pipe_action_data_spec_t *act_data_spec,  // Only for immediate action data.
    uint32_t indirect_action_ptr,
    uint32_t indirect_stats_ptr,
    uint32_t indirect_meter_ptr,
    uint32_t indirect_stful_ptr,
    uint32_t indirect_idle_ptr,
    uint32_t indirect_sel_ptr,
    uint32_t selector_len,
    pipe_register_spec_t *register_spec_list) {
  bf_dev_id_t devid = dev_info->dev_id;
  pipe_default_ent_reg_data reg_data;
  uint32_t logical_table, stage;
  uint32_t indirect_sel_stful_meter_ptr;
  uint32_t bj_hash, sel_default_val, sel_default_mask;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_default_action_fn_t *action_ptr;

  if (selector_len > 1) {
    return PIPE_NOT_SUPPORTED;
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut_depth, mat_tbl_hdl, 0, 0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for default entry for "
        "table handle "
        "0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_ASSERT(0);
    return (PIPE_INVALID_ARG);
  }
  logical_table = lut_ptr->u.dft_ptr->logicalid;
  stage = lut_ptr->u.dft_ptr->stage;
  pipe_status_t status = get_default_ent_reg_data(dev_info,
                                                  mat_tbl_hdl,
                                                  act_fn_hdl,
                                                  act_data_spec,
                                                  &reg_data,
                                                  lut_ptr,
                                                  &action_ptr);

  if (status != PIPE_SUCCESS) return status;
  if (action_ptr->force_stats_addr) {
    indirect_stats_ptr = action_ptr->forced_full_stats_addr;
  }
  if (action_ptr->force_meter_addr) {
    indirect_meter_ptr = action_ptr->forced_full_meter_addr;
  }
  if (action_ptr->force_stful_addr) {
    indirect_stful_ptr = action_ptr->forced_full_stful_addr;
  }
  if (action_ptr->force_stats_pfe_set) {
    indirect_stats_ptr |= 1 << TOF2_STATS_ADDR_PFE_BIT_POSITION;
  }
  if (action_ptr->force_meter_pfe_set) {
    indirect_meter_ptr |= 1 << TOF2_METER_ADDR_PFE_BIT_POSITION;
  }
  if (action_ptr->force_stful_pfe_set) {
    indirect_stful_ptr |= 1 << 23;
  }
  indirect_sel_stful_meter_ptr =
      indirect_sel_ptr | indirect_stful_ptr | indirect_meter_ptr;
  /* Can have at most one select, stateful, and meter pointer. */
  PIPE_MGR_ASSERT(
      !indirect_sel_stful_meter_ptr ||
      (indirect_sel_ptr && !indirect_stful_ptr && !indirect_meter_ptr) ||
      (!indirect_sel_ptr && indirect_stful_ptr && !indirect_meter_ptr) ||
      (!indirect_sel_ptr && !indirect_stful_ptr && indirect_meter_ptr));

  /* Get all the register address values. */
  pipe_mgr_entry_format_tof3_tbl_default_entry_prepare(
      dev_info, 0, stage, logical_table, register_spec_list);
  /* Populate the data in the register spec array. */
  /* mau_immediate_data_miss_value */
  setp_tof3_mau_immediate_data_miss_value_mau_immediate_data_miss_value(
      &register_spec_list[0].reg_data, reg_data.immediate_val);
  /* mau_action_insruction_adr_miss_value */
  setp_tof3_mau_action_instruction_adr_miss_value_mau_action_instruction_adr_miss_value(
      &register_spec_list[1].reg_data, reg_data.vliw_addr);
  /* mau_action_data_adr_miss_value */
  setp_tof3_mau_actiondata_adr_miss_value_mau_actiondata_adr_miss_value(
      &register_spec_list[2].reg_data, indirect_action_ptr | (1 << 22));
  /* mau_stats_adr_miss_value */
  setp_tof3_mau_stats_adr_miss_value_mau_stats_adr_miss_value(
      &register_spec_list[3].reg_data, indirect_stats_ptr);
  /* mau_meter_adr_miss_value */
  if (lut_ptr->u.dft_ptr->selectorcount) {
    sel_default_val = (1 << TOF3_METER_ADDR_SEL_TYPE_BIT_POSITION);
    sel_default_mask = (1 << TOF3_METER_ADDR_METER_TYPE_BIT_POSITION) - 1;
    sel_default_mask -= (1 << TOF3_SELECTOR_HUFFMAN_BITS) - 1;
    indirect_sel_ptr = sel_default_val | (indirect_sel_ptr & sel_default_mask);
    if (selector_len) {
      indirect_sel_ptr |= (1 << TOF3_METER_ADDR_PFE_BIT_POSITION);
    } else {
      indirect_sel_ptr &= ~(1 << TOF3_METER_ADDR_PFE_BIT_POSITION);
    }
    setp_tof3_mau_meter_adr_miss_value_mau_meter_adr_miss_value(
        &register_spec_list[4].reg_data, indirect_sel_ptr);
  } else {
    setp_tof3_mau_meter_adr_miss_value_mau_meter_adr_miss_value(
        &register_spec_list[4].reg_data, indirect_sel_stful_meter_ptr);
  }
  /* mau_idletime_adr_miss_value */
  setp_tof3_mau_idletime_adr_miss_value_mau_idletime_adr_miss_value(
      &register_spec_list[5].reg_data, indirect_idle_ptr);
  /* next_table_format_data */
  setp_tof3_next_table_format_data_match_next_table_adr_miss_value(
      &register_spec_list[6].reg_data, reg_data.next_table);
  setp_tof3_next_table_format_data_match_next_table_adr_default(
      &register_spec_list[6].reg_data, reg_data.next_table_default);
  setp_tof3_next_table_format_data_match_next_table_adr_mask(
      &register_spec_list[6].reg_data, reg_data.table_mask);
  /* pred_miss_exec */
  register_spec_list[7].reg_data = reg_data.next_table_exec;
  /* pred_miss_long_brch */
  register_spec_list[8].reg_data = reg_data.next_table_long_branch;

  return PIPE_SUCCESS;
}






























































































pipe_status_t pipe_mgr_entry_format_tbl_default_entry_update(
    rmt_dev_info_t *dev_info,
    uint8_t direction,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_act_fn_hdl_t act_fn_hdl,
    pipe_action_data_spec_t *act_data_spec,  // Only for immediate action data.
    uint32_t indirect_action_ptr,
    uint32_t indirect_stats_ptr,
    uint32_t indirect_meter_ptr,
    uint32_t indirect_stful_ptr,
    uint32_t indirect_idle_ptr,
    uint32_t indirect_sel_ptr,
    uint32_t selector_len,
    pipe_register_spec_t *register_spec_list) {
  // keep compiler happy and not return unused error
  (void)direction;
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      return pipe_mgr_entry_format_tof_tbl_default_entry_update(
          dev_info,
          prof_id,
          mat_tbl_hdl,
          act_fn_hdl,
          act_data_spec,
          indirect_action_ptr,
          indirect_stats_ptr,
          indirect_meter_ptr,
          indirect_stful_ptr,
          indirect_idle_ptr,
          indirect_sel_ptr,
          selector_len,
          register_spec_list);
    case BF_DEV_FAMILY_TOFINO2:
      return pipe_mgr_entry_format_tof2_tbl_default_entry_update(
          dev_info,
          prof_id,
          mat_tbl_hdl,
          act_fn_hdl,
          act_data_spec,
          indirect_action_ptr,
          indirect_stats_ptr,
          indirect_meter_ptr,
          indirect_stful_ptr,
          indirect_idle_ptr,
          indirect_sel_ptr,
          selector_len,
          register_spec_list);
    case BF_DEV_FAMILY_TOFINO3:
      return pipe_mgr_entry_format_tof3_tbl_default_entry_update(
          dev_info,
          prof_id,
          mat_tbl_hdl,
          act_fn_hdl,
          act_data_spec,
          indirect_action_ptr,
          indirect_stats_ptr,
          indirect_meter_ptr,
          indirect_stful_ptr,
          indirect_idle_ptr,
          indirect_sel_ptr,
          selector_len,
          register_spec_list);











    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }
}

static pipe_status_t pipe_mgr_entry_format_tof_tbl_default_entry_get(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_act_fn_hdl_t *act_fn_hdl,
    pipe_register_spec_t *register_spec_list,
    pipe_action_data_spec_t *act_data_spec,  // Only for immediate action data.
    uint32_t *indirect_action_ptr,
    uint32_t *indirect_stats_ptr,
    uint32_t *indirect_meter_ptr,
    uint32_t *indirect_stful_ptr,
    uint32_t *indirect_idle_ptr,
    uint32_t *indirect_sel_ptr,
    uint32_t *selector_len) {
  bf_dev_id_t devid = dev_info->dev_id;
  uint32_t indirect_sel_stful_meter_ptr = 0;
  uint32_t bj_hash, i, immediate_val;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_default_action_fn_t *action_ptr;
  pipemgr_tbl_pkg_default_action_param_t *action_param_ptr;
  pipemgr_tbl_pkg_action_parameter_field_t *act_field;
  uint16_t imm_start_offset, imm_bit_width, param_count;
  uint16_t next_table, next_table_default, table_mask;
  uint32_t vliw_addr;
  pipe_mat_tbl_info_t *mat_tbl_info = NULL;
  uint8_t *imm_val_ptr = (uint8_t *)&immediate_val;

  mat_tbl_info =
      pipe_mgr_get_tbl_info(dev_info->dev_id, mat_tbl_hdl, __func__, __LINE__);
  if (!mat_tbl_info) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut_depth, mat_tbl_hdl, 0, 0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for default entry for "
        "table handle 0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  /* mau_action_insruction_adr_miss_value */
  vliw_addr =
      getp_mau_action_instruction_adr_miss_value_mau_action_instruction_adr_miss_value(
          &register_spec_list[1].reg_data);
  /* next_table_format_data */
  next_table = getp_next_table_format_data_match_next_table_adr_miss_value(
      &register_spec_list[6].reg_data);
  next_table_default = getp_next_table_format_data_match_next_table_adr_default(
      &register_spec_list[6].reg_data);
  table_mask = getp_next_table_format_data_match_next_table_adr_mask(
      &register_spec_list[6].reg_data);

  action_param_ptr = lut_ptr->u.dft_ptr->act_param;
  action_ptr = lut_ptr->u.dft_ptr->actions;
  for (i = 0; i < lut_ptr->u.dft_ptr->totalactionhandles; i++) {
    if (action_ptr->instr == vliw_addr && action_ptr->next_tbl == next_table &&
        action_ptr->tbl_mask == table_mask &&
        action_ptr->next_tbl_default == next_table_default) {
      break;
    }
    action_ptr++;
  }
  if (i >= lut_ptr->u.dft_ptr->totalactionhandles) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for default entry for "
        "table handle 0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }
  // Set the action function handle
  *act_fn_hdl = action_ptr->action_fn_handle;

  for (i = 0; i < lut_ptr->u.dft_ptr->totalactionhandles; i++) {
    if (action_param_ptr->action_fn_handle == *act_fn_hdl) {
      break;
    }
    param_count = action_param_ptr->act_param_count;
    action_param_ptr++;
    action_param_ptr =
        (pipemgr_tbl_pkg_default_action_param_t
             *)((uint8_t *)action_param_ptr +
                (param_count *
                 sizeof(pipemgr_tbl_pkg_action_parameter_field_t)));
  }
  if (i >= lut_ptr->u.dft_ptr->totalactionhandles) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for default entry for "
        "table handle 0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  /* mau_immediate_data_miss_value */
  immediate_val =
      getp_mau_immediate_data_miss_value_mau_immediate_data_miss_value(
          &register_spec_list[0].reg_data);
  /* mau_action_data_adr_miss_value */
  *indirect_action_ptr =
      getp_mau_actiondata_adr_miss_value_mau_actiondata_adr_miss_value(
          &register_spec_list[2].reg_data);
  *indirect_action_ptr &= ~(1 << 22);  // zero out 22nd bit
  /* mau_stats_adr_miss_value */
  *indirect_stats_ptr = getp_mau_stats_adr_miss_value_mau_stats_adr_miss_value(
      &register_spec_list[3].reg_data);
  /* mau_meter_adr_miss_value */
  if (lut_ptr->u.dft_ptr->selectorcount) {
    *indirect_sel_ptr = getp_mau_meter_adr_miss_value_mau_meter_adr_miss_value(
        &register_spec_list[4].reg_data);
    *selector_len =
        (*indirect_sel_ptr >> TOF_METER_ADDR_PFE_BIT_POSITION) & 0x01;
  } else {
    indirect_sel_stful_meter_ptr =
        getp_mau_meter_adr_miss_value_mau_meter_adr_miss_value(
            &register_spec_list[4].reg_data);
  }
  /* mau_idletime_adr_miss_value */
  *indirect_idle_ptr =
      getp_mau_idletime_adr_miss_value_mau_idletime_adr_miss_value(
          &register_spec_list[5].reg_data);

  *indirect_meter_ptr = 0;
  *indirect_stful_ptr = 0;
  if (!lut_ptr->u.dft_ptr->selectorcount) {
    // Implies that the sel pointer must be NULL
    *indirect_sel_ptr = 0;

    // Now determine if the meter or stful pointer is NULL
    PIPE_MGR_DBGCHK(!(mat_tbl_info->meter_tbl_ref != NULL &&
                      mat_tbl_info->stful_tbl_ref != NULL));
    if (mat_tbl_info->meter_tbl_ref) {
      *indirect_meter_ptr = indirect_sel_stful_meter_ptr;
    } else {
      *indirect_stful_ptr = indirect_sel_stful_meter_ptr;
    }
  }

  act_field = action_param_ptr->act_field;
  for (i = 0; i < action_param_ptr->act_param_count; i++) {
    imm_start_offset = act_field->start_offset;
    imm_bit_width = act_field->bit_width;
    if (act_field->is_parameter) {
      uint16_t start_offset =
          (act_field->param_start + act_field->param_width - 1) - 7;
      uint8_t byte_offset = act_field->param_shift / 8;
      uint8_t bit_offset = act_field->param_shift % 8;
      start_offset -= (byte_offset * 8);
      start_offset += bit_offset;
      decode_bits_to_spec(act_data_spec->action_data_bits,
                          start_offset,
                          act_field->param_width,
                          &imm_val_ptr,
                          0,
                          imm_start_offset,
                          imm_bit_width);
    }
    act_field++;
  }

  return PIPE_SUCCESS;
}
static pipe_status_t pipe_mgr_entry_format_tof2_tbl_default_entry_get(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_act_fn_hdl_t *act_fn_hdl,
    pipe_register_spec_t *register_spec_list,
    pipe_action_data_spec_t *act_data_spec,  // Only for immediate action data.
    uint32_t *indirect_action_ptr,
    uint32_t *indirect_stats_ptr,
    uint32_t *indirect_meter_ptr,
    uint32_t *indirect_stful_ptr,
    uint32_t *indirect_idle_ptr,
    uint32_t *indirect_sel_ptr,
    uint32_t *selector_len) {
  bf_dev_id_t devid = dev_info->dev_id;
  uint32_t indirect_sel_stful_meter_ptr = 0;
  uint32_t bj_hash, i, immediate_val;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_default_action_fn_t *action_ptr;
  pipemgr_tbl_pkg_default_action_param_t *action_param_ptr;
  pipemgr_tbl_pkg_action_parameter_field_t *act_field;
  uint16_t imm_start_offset, imm_bit_width, param_count;
  uint16_t next_table, next_table_default, table_mask;
  uint32_t next_table_exec, next_table_long_branch;
  uint32_t vliw_addr;
  pipe_mat_tbl_info_t *mat_tbl_info = NULL;
  uint8_t *imm_val_ptr = (uint8_t *)&immediate_val;

  mat_tbl_info =
      pipe_mgr_get_tbl_info(dev_info->dev_id, mat_tbl_hdl, __func__, __LINE__);
  if (!mat_tbl_info) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut_depth, mat_tbl_hdl, 0, 0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for default entry for "
        "table handle 0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  /* mau_action_insruction_adr_miss_value */
  vliw_addr =
      getp_tof2_mau_action_instruction_adr_miss_value_mau_action_instruction_adr_miss_value(
          &register_spec_list[1].reg_data);
  /* next_table_format_data */
  next_table = getp_tof2_next_table_format_data_match_next_table_adr_miss_value(
      &register_spec_list[6].reg_data);
  next_table_default =
      getp_tof2_next_table_format_data_match_next_table_adr_default(
          &register_spec_list[6].reg_data);
  table_mask = getp_tof2_next_table_format_data_match_next_table_adr_mask(
      &register_spec_list[6].reg_data);
  next_table_exec = register_spec_list[7].reg_data;
  next_table_long_branch = register_spec_list[8].reg_data;

  action_param_ptr = lut_ptr->u.dft_ptr->act_param;
  action_ptr = lut_ptr->u.dft_ptr->actions;
  for (i = 0; i < lut_ptr->u.dft_ptr->totalactionhandles; i++) {
    if (action_ptr->instr == vliw_addr && action_ptr->next_tbl == next_table &&
        action_ptr->tbl_mask == table_mask &&
        action_ptr->next_tbl_default == next_table_default &&
        action_ptr->next_tbl_exec == next_table_exec &&
        action_ptr->next_tbl_long_branch == next_table_long_branch) {
      break;
    }
    action_ptr++;
  }
  if (i >= lut_ptr->u.dft_ptr->totalactionhandles) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for default entry for "
        "table handle 0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }
  // Set the action function handle
  *act_fn_hdl = action_ptr->action_fn_handle;

  for (i = 0; i < lut_ptr->u.dft_ptr->totalactionhandles; i++) {
    if (action_param_ptr->action_fn_handle == *act_fn_hdl) {
      break;
    }
    param_count = action_param_ptr->act_param_count;
    action_param_ptr++;
    action_param_ptr =
        (pipemgr_tbl_pkg_default_action_param_t
             *)((uint8_t *)action_param_ptr +
                (param_count *
                 sizeof(pipemgr_tbl_pkg_action_parameter_field_t)));
  }
  if (i >= lut_ptr->u.dft_ptr->totalactionhandles) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for default entry for "
        "table handle 0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  /* mau_immediate_data_miss_value */
  immediate_val =
      getp_tof2_mau_immediate_data_miss_value_mau_immediate_data_miss_value(
          &register_spec_list[0].reg_data);
  /* mau_action_data_adr_miss_value */
  *indirect_action_ptr =
      getp_tof2_mau_actiondata_adr_miss_value_mau_actiondata_adr_miss_value(
          &register_spec_list[2].reg_data);
  *indirect_action_ptr &= ~(1 << 22);  // zero out 22nd bit
  /* mau_stats_adr_miss_value */
  *indirect_stats_ptr =
      getp_tof2_mau_stats_adr_miss_value_mau_stats_adr_miss_value(
          &register_spec_list[3].reg_data);
  /* mau_meter_adr_miss_value */
  if (lut_ptr->u.dft_ptr->selectorcount) {
    *indirect_sel_ptr =
        getp_tof2_mau_meter_adr_miss_value_mau_meter_adr_miss_value(
            &register_spec_list[4].reg_data);
    *selector_len =
        (*indirect_sel_ptr >> TOF_METER_ADDR_PFE_BIT_POSITION) & 0x01;
  } else {
    indirect_sel_stful_meter_ptr =
        getp_tof2_mau_meter_adr_miss_value_mau_meter_adr_miss_value(
            &register_spec_list[4].reg_data);
  }
  /* mau_idletime_adr_miss_value */
  *indirect_idle_ptr =
      getp_tof2_mau_idletime_adr_miss_value_mau_idletime_adr_miss_value(
          &register_spec_list[5].reg_data);

  *indirect_meter_ptr = 0;
  *indirect_stful_ptr = 0;
  if (!lut_ptr->u.dft_ptr->selectorcount) {
    // Implies that the sel pointer must be NULL
    *indirect_sel_ptr = 0;

    // Now determine if the meter or stful pointer is NULL
    PIPE_MGR_DBGCHK(!(mat_tbl_info->meter_tbl_ref != NULL &&
                      mat_tbl_info->stful_tbl_ref != NULL));
    if (mat_tbl_info->meter_tbl_ref) {
      *indirect_meter_ptr = indirect_sel_stful_meter_ptr;
    } else {
      *indirect_stful_ptr = indirect_sel_stful_meter_ptr;
    }
  }

  act_field = action_param_ptr->act_field;
  for (i = 0; i < action_param_ptr->act_param_count; i++) {
    imm_start_offset = act_field->start_offset;
    imm_bit_width = act_field->bit_width;
    if (act_field->is_parameter) {
      uint16_t start_offset =
          (act_field->param_start + act_field->param_width - 1) - 7;
      uint8_t byte_offset = act_field->param_shift / 8;
      uint8_t bit_offset = act_field->param_shift % 8;
      start_offset -= (byte_offset * 8);
      start_offset += bit_offset;
      decode_bits_to_spec(act_data_spec->action_data_bits,
                          start_offset,
                          act_field->param_width,
                          &imm_val_ptr,
                          0,
                          imm_start_offset,
                          imm_bit_width);
    }
    act_field++;
  }

  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_entry_format_tof3_tbl_default_entry_get(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_act_fn_hdl_t *act_fn_hdl,
    pipe_register_spec_t *register_spec_list,
    pipe_action_data_spec_t *act_data_spec,  // Only for immediate action data.
    uint32_t *indirect_action_ptr,
    uint32_t *indirect_stats_ptr,
    uint32_t *indirect_meter_ptr,
    uint32_t *indirect_stful_ptr,
    uint32_t *indirect_idle_ptr,
    uint32_t *indirect_sel_ptr,
    uint32_t *selector_len) {
  bf_dev_id_t devid = dev_info->dev_id;
  uint32_t indirect_sel_stful_meter_ptr = 0;
  uint32_t bj_hash, i, immediate_val;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_default_action_fn_t *action_ptr;
  pipemgr_tbl_pkg_default_action_param_t *action_param_ptr;
  pipemgr_tbl_pkg_action_parameter_field_t *act_field;
  uint16_t imm_start_offset, imm_bit_width, param_count;
  uint16_t next_table, next_table_default, table_mask;
  uint32_t next_table_exec, next_table_long_branch;
  uint32_t vliw_addr;
  pipe_mat_tbl_info_t *mat_tbl_info = NULL;
  uint8_t *imm_val_ptr = (uint8_t *)&immediate_val;

  mat_tbl_info =
      pipe_mgr_get_tbl_info(dev_info->dev_id, mat_tbl_hdl, __func__, __LINE__);
  if (!mat_tbl_info) {
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut_depth, mat_tbl_hdl, 0, 0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).dft_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for default entry for "
        "table handle 0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  /* mau_action_insruction_adr_miss_value */
  vliw_addr =
      getp_tof3_mau_action_instruction_adr_miss_value_mau_action_instruction_adr_miss_value(
          &register_spec_list[1].reg_data);
  /* next_table_format_data */
  next_table = getp_tof3_next_table_format_data_match_next_table_adr_miss_value(
      &register_spec_list[6].reg_data);
  next_table_default =
      getp_tof3_next_table_format_data_match_next_table_adr_default(
          &register_spec_list[6].reg_data);
  table_mask = getp_tof3_next_table_format_data_match_next_table_adr_mask(
      &register_spec_list[6].reg_data);
  next_table_exec = register_spec_list[7].reg_data;
  next_table_long_branch = register_spec_list[8].reg_data;

  action_param_ptr = lut_ptr->u.dft_ptr->act_param;
  action_ptr = lut_ptr->u.dft_ptr->actions;
  for (i = 0; i < lut_ptr->u.dft_ptr->totalactionhandles; i++) {
    if (action_ptr->instr == vliw_addr && action_ptr->next_tbl == next_table &&
        action_ptr->tbl_mask == table_mask &&
        action_ptr->next_tbl_default == next_table_default &&
        action_ptr->next_tbl_exec == next_table_exec &&
        action_ptr->next_tbl_long_branch == next_table_long_branch) {
      break;
    }
    action_ptr++;
  }
  if (i >= lut_ptr->u.dft_ptr->totalactionhandles) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for default entry for "
        "table handle 0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }
  // Set the action function handle
  *act_fn_hdl = action_ptr->action_fn_handle;

  for (i = 0; i < lut_ptr->u.dft_ptr->totalactionhandles; i++) {
    if (action_param_ptr->action_fn_handle == *act_fn_hdl) {
      break;
    }
    param_count = action_param_ptr->act_param_count;
    action_param_ptr++;
    action_param_ptr =
        (pipemgr_tbl_pkg_default_action_param_t
             *)((uint8_t *)action_param_ptr +
                (param_count *
                 sizeof(pipemgr_tbl_pkg_action_parameter_field_t)));
  }
  if (i >= lut_ptr->u.dft_ptr->totalactionhandles) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for default entry for "
        "table handle 0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  /* mau_immediate_data_miss_value */
  immediate_val =
      getp_tof3_mau_immediate_data_miss_value_mau_immediate_data_miss_value(
          &register_spec_list[0].reg_data);
  /* mau_action_data_adr_miss_value */
  *indirect_action_ptr =
      getp_tof3_mau_actiondata_adr_miss_value_mau_actiondata_adr_miss_value(
          &register_spec_list[2].reg_data);
  *indirect_action_ptr &= ~(1 << 22);  // zero out 22nd bit
  /* mau_stats_adr_miss_value */
  *indirect_stats_ptr =
      getp_tof3_mau_stats_adr_miss_value_mau_stats_adr_miss_value(
          &register_spec_list[3].reg_data);
  /* mau_meter_adr_miss_value */
  if (lut_ptr->u.dft_ptr->selectorcount) {
    *indirect_sel_ptr =
        getp_tof3_mau_meter_adr_miss_value_mau_meter_adr_miss_value(
            &register_spec_list[4].reg_data);
    *selector_len =
        (*indirect_sel_ptr >> TOF_METER_ADDR_PFE_BIT_POSITION) & 0x01;
  } else {
    indirect_sel_stful_meter_ptr =
        getp_tof3_mau_meter_adr_miss_value_mau_meter_adr_miss_value(
            &register_spec_list[4].reg_data);
  }
  /* mau_idletime_adr_miss_value */
  *indirect_idle_ptr =
      getp_tof3_mau_idletime_adr_miss_value_mau_idletime_adr_miss_value(
          &register_spec_list[5].reg_data);

  *indirect_meter_ptr = 0;
  *indirect_stful_ptr = 0;
  if (!lut_ptr->u.dft_ptr->selectorcount) {
    // Implies that the sel pointer must be NULL
    *indirect_sel_ptr = 0;

    // Now determine if the meter or stful pointer is NULL
    PIPE_MGR_DBGCHK(!(mat_tbl_info->meter_tbl_ref != NULL &&
                      mat_tbl_info->stful_tbl_ref != NULL));
    if (mat_tbl_info->meter_tbl_ref) {
      *indirect_meter_ptr = indirect_sel_stful_meter_ptr;
    } else {
      *indirect_stful_ptr = indirect_sel_stful_meter_ptr;
    }
  }

  act_field = action_param_ptr->act_field;
  for (i = 0; i < action_param_ptr->act_param_count; i++) {
    imm_start_offset = act_field->start_offset;
    imm_bit_width = act_field->bit_width;
    if (act_field->is_parameter) {
      uint16_t start_offset =
          (act_field->param_start + act_field->param_width - 1) - 7;
      uint8_t byte_offset = act_field->param_shift / 8;
      uint8_t bit_offset = act_field->param_shift % 8;
      start_offset -= (byte_offset * 8);
      start_offset += bit_offset;
      decode_bits_to_spec(act_data_spec->action_data_bits,
                          start_offset,
                          act_field->param_width,
                          &imm_val_ptr,
                          0,
                          imm_start_offset,
                          imm_bit_width);
    }
    act_field++;
  }

  return PIPE_SUCCESS;
}







































































































































pipe_status_t pipe_mgr_entry_format_tbl_default_entry_get(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_act_fn_hdl_t *act_fn_hdl,
    pipe_register_spec_t *register_spec_list,
    pipe_action_data_spec_t *act_data_spec,  // Only for immediate action data.
    uint32_t *indirect_action_ptr,
    uint32_t *indirect_stats_ptr,
    uint32_t *indirect_meter_ptr,
    uint32_t *indirect_stful_ptr,
    uint32_t *indirect_idle_ptr,
    uint32_t *indirect_sel_ptr,
    uint32_t *selector_len) {
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      return pipe_mgr_entry_format_tof_tbl_default_entry_get(
          dev_info,
          prof_id,
          mat_tbl_hdl,
          act_fn_hdl,
          register_spec_list,
          act_data_spec,
          indirect_action_ptr,
          indirect_stats_ptr,
          indirect_meter_ptr,
          indirect_stful_ptr,
          indirect_idle_ptr,
          indirect_sel_ptr,
          selector_len);
    case BF_DEV_FAMILY_TOFINO2:
      return pipe_mgr_entry_format_tof2_tbl_default_entry_get(
          dev_info,
          prof_id,
          mat_tbl_hdl,
          act_fn_hdl,
          register_spec_list,
          act_data_spec,
          indirect_action_ptr,
          indirect_stats_ptr,
          indirect_meter_ptr,
          indirect_stful_ptr,
          indirect_idle_ptr,
          indirect_sel_ptr,
          selector_len);
    case BF_DEV_FAMILY_TOFINO3:
      return pipe_mgr_entry_format_tof3_tbl_default_entry_get(
          dev_info,
          prof_id,
          mat_tbl_hdl,
          act_fn_hdl,
          register_spec_list,
          act_data_spec,
          indirect_action_ptr,
          indirect_stats_ptr,
          indirect_meter_ptr,
          indirect_stful_ptr,
          indirect_idle_ptr,
          indirect_sel_ptr,
          selector_len);









    default:
      LOG_ERROR("%s: Unknown device family for dev-id %d",
                __func__,
                dev_info->dev_id);
      return PIPE_INVALID_ARG;
  }
}

/////////////  Entry Decode functions. ////////////////////

static pipe_act_fn_hdl_t get_exm_action_fn_hdl_for_decode(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint8_t stage_table_handle,
    uint8_t entry_position,
    uint8_t **exm_tbl_word,
    pipemgr_tbl_pkg_action_entry_field_t **act_hdl_ptr,
    bool is_stash) {
  uint64_t field_value;
  int mem_index = 0;
  pipemgr_tbl_pkg_exm_stage_hdl_format_t *stage_hdl_ptr;
  pipemgr_tbl_pkg_action_entry_field_t *action_hdl_ptr;
  pipemgr_tbl_pkg_way_format_t *way_ptr;
  pipemgr_tbl_pkg_match_entry_line_t *entry_line_ptr;
  pipemgr_tbl_pkg_match_entry_field_t *field;
  pipemgr_tbl_pkg_action_handles_t *action_details_ptr;
  uint32_t hash_way = 0, bj_hash, i, param_count = 0;
  pipemgr_tbl_pkg_lut_t *lut_ptr;

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut_depth,
      mat_tbl_hdl,
      stage_id,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut,
      mat_tbl_hdl,
      stage_id);
  if (!lut_ptr) {
    LOG_ERROR("%s: Unable to find entry formatting details for match-tbl 0x%x",
              __func__,
              mat_tbl_hdl);
    return (PIPE_INVALID_ARG);
  }

  stage_hdl_ptr = pipemmgr_entry_format_get_stage_handle_details_ptr(
      stage_table_handle, lut_ptr);
  if (!stage_hdl_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d",
        __func__,
        mat_tbl_hdl,
        stage_id);
    return (PIPE_INVALID_ARG);
  }

  way_ptr = pipemmgr_entry_format_get_hash_way_details_ptr(
      hash_way, stage_hdl_ptr, is_stash);
  if (!way_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d, hash-way %d",
        __func__,
        mat_tbl_hdl,
        stage_id,
        hash_way);
    return (PIPE_INVALID_ARG);
  }

  entry_line_ptr = pipemmgr_entry_format_get_entry_details_ptr(
      entry_position, way_ptr->entry_format);
  if (!entry_line_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d, entry %d",
        __func__,
        mat_tbl_hdl,
        stage_id,
        entry_position);
    return (PIPE_INVALID_ARG);
  }

  field = entry_line_ptr->fields;
  for (i = 0; i < entry_line_ptr->field_count; i++) {
    mem_index = field->memword_index[0];
    if (field->source == TBL_PKG_FIELD_SOURCE_INSTR) {
      field_value = get_val(
          exm_tbl_word[mem_index], field->field_offset, field->field_width);

      // go over all action-handles for this match-tbl. return the action-handle
      // whoes instruction matches with field_value.
      action_details_ptr =
          (pipemgr_tbl_pkg_action_handles_t *)((uint8_t *)(lut_ptr->u.exm_ptr) +
                                               lut_ptr->match_field_mem_size);
      action_hdl_ptr = action_details_ptr->action_hdl;
      for (i = 0; i < action_details_ptr->action_hdl_count; i++) {
        if (action_hdl_ptr->instr == field_value) {
          if (act_hdl_ptr) {
            *act_hdl_ptr = action_hdl_ptr;
          }
          return (action_hdl_ptr->action_handle);
        }
        param_count = action_hdl_ptr->param_count;
        action_hdl_ptr++;
        action_hdl_ptr =
            (pipemgr_tbl_pkg_action_entry_field_t
                 *)((uint8_t *)action_hdl_ptr +
                    (param_count *
                     sizeof(pipemgr_tbl_pkg_action_parameter_field_t)));
      }
      /* There was a vliw instruction encoded in the entry but the value didn't
       * match any of the expected actions. */
      if (act_hdl_ptr) *act_hdl_ptr = NULL;
      return 0;
    }
    field++;
  }

  /* We have checked all fields and there is no vliw instruction encoded in the
   * entry therefore there must be only a single hit-path action on this entry
   * although there can be any number of miss-path actions.  Go through all
   * actions and find the hit-path only action. */
  action_details_ptr =
      (pipemgr_tbl_pkg_action_handles_t *)((uint8_t *)(lut_ptr->u.exm_ptr) +
                                           lut_ptr->match_field_mem_size);
  action_hdl_ptr = action_details_ptr->action_hdl;
  for (i = 0; i < action_details_ptr->action_hdl_count; ++i) {
    /* For default-only actions the context.json will specify -1 for the
     * "vliw_instruction" and a valid value for the hit-path instruction.
     * For ATCAM tables a default action to advance to the next logical table in
     * the ATCAM table is generated by older versions of the compiler and should
     * be skipped.  It has an invalid "vliw_instruction" of 64. */
    if (action_hdl_ptr->instr != (uint16_t)-1 && action_hdl_ptr->instr != 64) {
      if (act_hdl_ptr) *act_hdl_ptr = action_hdl_ptr;
      return action_hdl_ptr->action_handle;
    } else {
      /* Skip over this action and move onto the next. */
      param_count = action_hdl_ptr->param_count;
      action_hdl_ptr++;
      action_hdl_ptr =
          (pipemgr_tbl_pkg_action_entry_field_t
               *)((uint8_t *)action_hdl_ptr +
                  (param_count *
                   sizeof(pipemgr_tbl_pkg_action_parameter_field_t)));
    }
  }
  return 0;
}

static pipe_act_fn_hdl_t get_exm_action_fn_hdl(bf_dev_id_t devid,
                                               profile_id_t prof_id,
                                               uint8_t stage_id,
                                               pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                               uint8_t stage_table_handle,
                                               uint8_t entry_position,
                                               exm_tbl_word_t *exm_tbl_word,
                                               bool is_stash) {
  uint64_t field_value;
  uint8_t mem_id_vals[TBLPACK_WORD_BYTES_MAX] = {0};
  int mem_id = -1, mem_index = 0;
  pipemgr_tbl_pkg_exm_stage_hdl_format_t *stage_hdl_ptr;
  pipemgr_tbl_pkg_action_entry_field_t *action_hdl_ptr;
  pipemgr_tbl_pkg_way_format_t *way_ptr;
  pipemgr_tbl_pkg_match_entry_line_t *entry_line_ptr;
  pipemgr_tbl_pkg_match_entry_field_t *field;
  pipemgr_tbl_pkg_action_handles_t *action_details_ptr;
  uint32_t hash_way = 0, bj_hash, i, param_count = 0;
  pipemgr_tbl_pkg_lut_t *lut_ptr;

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut_depth,
      mat_tbl_hdl,
      stage_id,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut,
      mat_tbl_hdl,
      stage_id);
  if (!lut_ptr) {
    LOG_ERROR("%s: Unable to find entry formatting details for match-tbl 0x%x",
              __func__,
              mat_tbl_hdl);
    return (PIPE_INVALID_ARG);
  }

  stage_hdl_ptr = pipemmgr_entry_format_get_stage_handle_details_ptr(
      stage_table_handle, lut_ptr);
  if (!stage_hdl_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d",
        __func__,
        mat_tbl_hdl,
        stage_id);
    return (PIPE_INVALID_ARG);
  }

  way_ptr = pipemmgr_entry_format_get_hash_way_details_ptr(
      hash_way, stage_hdl_ptr, is_stash);
  if (!way_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d, hash-way %d",
        __func__,
        mat_tbl_hdl,
        stage_id,
        hash_way);
    return (PIPE_INVALID_ARG);
  }

  entry_line_ptr = pipemmgr_entry_format_get_entry_details_ptr(
      entry_position, way_ptr->entry_format);
  if (!entry_line_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d, entry %d",
        __func__,
        mat_tbl_hdl,
        stage_id,
        entry_position);
    return (PIPE_INVALID_ARG);
  }

  field = entry_line_ptr->fields;
  for (i = 0; i < entry_line_ptr->field_count; i++) {
    mem_index = field->memword_index[0];
    if (mem_id_vals[mem_index] == 0) {
      mem_id++;
    }
    mem_id_vals[mem_index] = 1;
    if (field->source == TBL_PKG_FIELD_SOURCE_INSTR) {
      field_value = get_val(
          (*exm_tbl_word)[mem_id], field->field_offset, field->field_width);

      // go over all action-handles for this match-tbl. return the action-handle
      // whoes instruction matches with field_value.
      action_details_ptr =
          (pipemgr_tbl_pkg_action_handles_t *)((uint8_t *)(lut_ptr->u.exm_ptr) +
                                               lut_ptr->match_field_mem_size);
      action_hdl_ptr = action_details_ptr->action_hdl;
      for (i = 0; i < action_details_ptr->action_hdl_count; i++) {
        if (action_hdl_ptr->instr == field_value) {
          return action_hdl_ptr->action_handle;
        }
        param_count = action_hdl_ptr->param_count;
        action_hdl_ptr++;
        action_hdl_ptr =
            (pipemgr_tbl_pkg_action_entry_field_t
                 *)((uint8_t *)action_hdl_ptr +
                    (param_count *
                     sizeof(pipemgr_tbl_pkg_action_parameter_field_t)));
      }
      /* There was a vliw instruction encoded in the entry but the value didn't
       * match any of the expected actions. */
      return 0;
    }
    field++;
  }

  /* We have checked all fields and there is no vliw instruction encoded in the
   * entry therefore there must be only a single hit-path action on this entry
   * although there can be any number of miss-path actions.  Go through all
   * actions and find the hit-path only action. */
  action_details_ptr =
      (pipemgr_tbl_pkg_action_handles_t *)((uint8_t *)(lut_ptr->u.exm_ptr) +
                                           lut_ptr->match_field_mem_size);
  action_hdl_ptr = action_details_ptr->action_hdl;
  for (i = 0; i < action_details_ptr->action_hdl_count; ++i) {
    /* For default-only actions the context.json will specify -1 for the
     * "vliw_instruction" and a valid value for the hit-path instruction.
     * For ATCAM tables a default action to advance to the next logical table in
     * the ATCAM table is generated by older versions of the compiler and should
     * be skipped.  It has an invalid "vliw_instruction" of 64. */
    if (action_hdl_ptr->instr != (uint16_t)-1 && action_hdl_ptr->instr != 64) {
      return action_hdl_ptr->action_handle;
    } else {
      /* Skip over this action and move onto the next. */
      param_count = action_hdl_ptr->param_count;
      action_hdl_ptr++;
      action_hdl_ptr =
          (pipemgr_tbl_pkg_action_entry_field_t
               *)((uint8_t *)action_hdl_ptr +
                  (param_count *
                   sizeof(pipemgr_tbl_pkg_action_parameter_field_t)));
    }
  }
  return 0;
}

static pipe_act_fn_hdl_t get_tind_action_fn_hdl(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint8_t entry_position,
    uint16_t offset,
    tind_tbl_word_t *tind_tbl_word) {
  uint64_t field_value;
  pipemgr_tbl_pkg_action_entry_field_t *action_hdl_ptr;
  pipemgr_tbl_pkg_match_entry_line_t *entry_line_ptr;
  pipemgr_tbl_pkg_match_entry_field_t *field;
  pipemgr_tbl_pkg_action_handles_t *action_details_ptr;
  uint32_t bj_hash, i, j, param_count;
  pipemgr_tbl_pkg_lut_t *lut_ptr;

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tind_lut_depth,
      mat_tbl_hdl,
      stage_id,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tind_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tind_lut,
      mat_tbl_hdl,
      stage_id);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl %d, stage "
        "%d",
        __func__,
        mat_tbl_hdl,
        stage_id);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  entry_line_ptr = pipemmgr_entry_format_get_entry_details_ptr(
      entry_position, lut_ptr->u.tind_ptr);
  if (!entry_line_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d, entry %d",
        __func__,
        mat_tbl_hdl,
        stage_id,
        entry_position);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  field = entry_line_ptr->fields;
  for (i = 0; i < entry_line_ptr->field_count; i++) {
    if (field->source == TBL_PKG_FIELD_SOURCE_INSTR) {
      field_value = get_val(
          *tind_tbl_word, field->field_offset + offset, field->field_width);
      // go over all action-handles for this match-tbl. return the action-handle
      // whoes instruction matches with field_value.
      action_details_ptr = (pipemgr_tbl_pkg_action_handles_t
                                *)((uint8_t *)(lut_ptr->u.tind_ptr) +
                                   lut_ptr->match_field_mem_size);
      action_hdl_ptr = action_details_ptr->action_hdl;
      for (j = 0; j < action_details_ptr->action_hdl_count; j++) {
        if (action_hdl_ptr->instr == field_value) {
          return (action_hdl_ptr->action_handle);
        }
        param_count = action_hdl_ptr->param_count;
        action_hdl_ptr++;
        action_hdl_ptr =
            (pipemgr_tbl_pkg_action_entry_field_t
                 *)((uint8_t *)action_hdl_ptr +
                    (param_count *
                     sizeof(pipemgr_tbl_pkg_action_parameter_field_t)));
      }
      /* There was a vliw instruction encoded in the entry but the value didn't
       * match any of the expected actions. */
      return 0;
    }
    field++;
  }

  /* We have checked all fields and there is no vliw instruction encoded in the
   * entry therefore there must be only a single hit-path action on this entry
   * although there can be any number of miss-path actions.  Go through all
   * actions and find the hit-path only action. */
  action_details_ptr =
      (pipemgr_tbl_pkg_action_handles_t *)((uint8_t *)(lut_ptr->u.tind_ptr) +
                                           lut_ptr->match_field_mem_size);
  action_hdl_ptr = action_details_ptr->action_hdl;
  for (i = 0; i < action_details_ptr->action_hdl_count; ++i) {
    /* For default-only actions the context.json will specify -1 for the
     * "vliw_instruction" and a valid value for the hit-path instruction. */
    if (action_hdl_ptr->instr != (uint16_t)-1) {
      return action_hdl_ptr->action_handle;
    } else {
      /* "vliw_instruction" was -1, skip this action and advance to the next. */
      param_count = action_hdl_ptr->param_count;
      action_hdl_ptr++;
      action_hdl_ptr =
          (pipemgr_tbl_pkg_action_entry_field_t
               *)((uint8_t *)action_hdl_ptr +
                  (param_count *
                   sizeof(pipemgr_tbl_pkg_action_parameter_field_t)));
    }
  }
  return 0;
}

pipe_status_t pipe_mgr_entry_format_tof_exm_tbl_ent_to_str(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint8_t stage_table_handle,
    uint8_t entry_position,
    exm_tbl_word_t *exm_tbl_word,
    char *str,
    int *c_str_len,
    int max_len,
    uint64_t *addr_arr,
    bool is_stash) {
  int c_len = *c_str_len;
  uint64_t field_value;
  uint8_t *fl_val_ptr;
  int fl_len = 0, num_vals = 0;
  int bit_start = 0, bit_end = 0;
  uint8_t vals[TBLPACK_BYTES_MAX] = {0};
  uint8_t mem_id_vals[TBLPACK_WORD_BYTES_MAX] = {0};
  int mem_id = -1, mem_index = 0, k;
  // char        act_fld_name[TBLPACK_SMALL_STR_LEN];
  char *act_fld_name;
  pipe_act_fn_hdl_t act_fn_hdl = 0;

  pipemgr_tbl_pkg_exm_stage_hdl_format_t *stage_hdl_ptr;
  pipemgr_tbl_pkg_action_entry_field_t *action_hdl_ptr;
  pipemgr_tbl_pkg_way_format_t *way_ptr;
  pipemgr_tbl_pkg_match_entry_line_t *entry_line_ptr;
  pipemgr_tbl_pkg_match_entry_field_t *field;
  pipemgr_tbl_pkg_action_parameter_field_t *act_param_ptr;
  uint32_t hash_way = 0, bj_hash;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  uint32_t i;
  char *f_name, *fieldmaskstr = NULL, *fname;

  act_fn_hdl = get_exm_action_fn_hdl(devid,
                                     prof_id,
                                     stage_id,
                                     mat_tbl_hdl,
                                     stage_table_handle,
                                     entry_position,
                                     exm_tbl_word,
                                     is_stash);
  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "Action Function Handle: 0x%x \n",
                    act_fn_hdl);
  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut_depth,
      mat_tbl_hdl,
      stage_id,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut,
      mat_tbl_hdl,
      stage_id);
  if (!lut_ptr) {
    LOG_ERROR("%s: Unable to find entry formatting details for match-tbl 0x%x",
              __func__,
              mat_tbl_hdl);
    return (PIPE_INVALID_ARG);
  }

  stage_hdl_ptr = pipemmgr_entry_format_get_stage_handle_details_ptr(
      stage_table_handle, lut_ptr);
  if (!stage_hdl_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d",
        __func__,
        mat_tbl_hdl,
        stage_id);
    return (PIPE_INVALID_ARG);
  }

  action_hdl_ptr =
      pipemmgr_entry_format_get_action_handle_details_ptr(act_fn_hdl, lut_ptr);
  if (!action_hdl_ptr) {
    LOG_ERROR(
        "%s: Unable to find action handle formatting details for match-tbl "
        "0x%x, stage %d",
        __func__,
        mat_tbl_hdl,
        stage_id);
    return (PIPE_INVALID_ARG);
  }
  way_ptr = pipemmgr_entry_format_get_hash_way_details_ptr(
      hash_way, stage_hdl_ptr, is_stash);
  if (!way_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d, hash-way %d",
        __func__,
        mat_tbl_hdl,
        stage_id,
        hash_way);
    return (PIPE_INVALID_ARG);
  }

  entry_line_ptr = pipemmgr_entry_format_get_entry_details_ptr(
      entry_position, way_ptr->entry_format);
  if (!entry_line_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d, entry %d",
        __func__,
        mat_tbl_hdl,
        stage_id,
        entry_position);
    return (PIPE_INVALID_ARG);
  }

  uint8_t __s0q1[TBLPACK_BYTES_MAX] = {0};
  uint8_t __s1q0[TBLPACK_BYTES_MAX] = {0};

  field = entry_line_ptr->fields;
  for (i = 0; i < entry_line_ptr->field_count; i++) {
    mem_index = field->memword_index[0];
    bit_start = field->field_offset;
    bit_end = bit_start + field->field_width - 1;

    if (mem_id_vals[mem_index] == 0) {
      mem_id++;
      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        "--------- | ------------------------ | ---------- | "
                        "-------------------\n");
      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        "Exact-match Address 0x%" PRIx64 " \n",
                        addr_arr[mem_id]);
      dump_tbl_word((uint8_t *)exm_tbl_word,
                    mem_id * TBLPACK_TOF_BYTES_IN_RAM_WORD,
                    str,
                    &c_len,
                    max_len,
                    0,
                    TBLPACK_TOF_BYTES_IN_RAM_WORD * 8);
      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        "--------- | ------------------------ | ---------- | "
                        "-------------------\n");
      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        "%-9s | %-24s | %-10s | %-20s\n",
                        "Bit-field",
                        "Field-Name",
                        "Fld-Range",
                        " Value");
      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        "--------- | ------------------------ | ---------- | "
                        "-------------------\n");
    }
    mem_id_vals[mem_index] = 1;

    if (field->source != TBL_PKG_FIELD_SOURCE_IMMEDIATE) {
      f_name = pipemgr_tbl_pkg_get_field_string_name(
          devid, prof_id, field->stringindex);
      if (field->source == TBL_PKG_FIELD_SOURCE_SPEC &&
          field->match_mode == TBL_PKG_FIELD_MATCHMODE_S0Q1) {
        fieldmaskstr = PIPE_MGR_MALLOC(strlen(f_name) + strlen("__mask") + 1);
        sprintf(fieldmaskstr, "%s%s", f_name, "__mask");
        fieldmaskstr[strlen(f_name) + strlen("__mask")] = '\0';
        fname = fieldmaskstr;
      } else {
        fname = f_name;
      }
      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        "%-3d:%-3d   | %-24s | (%-3d..%-3d) | ",
                        bit_end,
                        bit_start,
                        fname,
                        field->fieldsb + field->field_width - 1,
                        field->fieldsb);
      if (field->source == TBL_PKG_FIELD_SOURCE_SPEC &&
          field->match_mode == TBL_PKG_FIELD_MATCHMODE_S0Q1) {
        if (fieldmaskstr) {
          PIPE_MGR_FREE(fieldmaskstr);
        }
      }
    }
    if (mem_id == -1) /*This should not happen.return*/
      return PIPE_UNEXPECTED;
    switch (field->source) {
      case TBL_PKG_FIELD_SOURCE_SPEC:
        if (field->match_mode == TBL_PKG_FIELD_MATCHMODE_S0Q1) {
          PIPE_MGR_DBGCHK(field->spec_len >=
                          field->fieldsb + field->field_width);
          decode_copy_bits(
              &__s0q1[0],
              field->spec_len - (field->fieldsb + field->field_width),
              field->field_width,
              (*exm_tbl_word)[field->memword_index[0]],
              field->field_offset,
              field->field_width);
        } else if (field->match_mode == TBL_PKG_FIELD_MATCHMODE_S1Q0) {
          PIPE_MGR_DBGCHK(field->spec_len >=
                          field->fieldsb + field->field_width);
          decode_copy_bits(
              &__s1q0[0],
              field->spec_len - (field->fieldsb + field->field_width),
              field->field_width,
              (*exm_tbl_word)[field->memword_index[0]],
              field->field_offset,
              field->field_width);
        }
        fl_val_ptr = (*exm_tbl_word)[mem_id];
        num_vals = field->field_width / 8;
        if ((field->field_width % 8) > 0) num_vals++;
        PIPE_MGR_MEMSET(&vals, 0, sizeof(vals));
        if (field->match_mode == TBL_PKG_FIELD_MATCHMODE_S1Q0) {
          decode_key_mask_s1q0_encode(vals,
                                      NULL,
                                      0,
                                      field->field_width,
                                      __s1q0,
                                      __s0q1,
                                      0,
                                      field->spec_len,
                                      field->fieldsb);
          for (fl_len = 0; fl_len < num_vals; fl_len++) {
            int index = num_vals - fl_len - 1;
            if ((index >= 0) && (index < TBLPACK_BYTES_MAX)) {
              c_len += snprintf(str + c_len,
                                (c_len < max_len) ? (max_len - c_len - 1) : 0,
                                "%02x ",
                                vals[index]);
            }
          }
        } else if (field->match_mode == TBL_PKG_FIELD_MATCHMODE_S0Q1) {
          decode_key_mask_s1q0_encode(NULL,
                                      vals,
                                      0,
                                      field->field_width,
                                      __s1q0,
                                      __s0q1,
                                      0,
                                      field->spec_len,
                                      field->fieldsb);
          for (fl_len = 0; fl_len < num_vals; fl_len++) {
            int index = num_vals - fl_len - 1;
            if ((index >= 0) && (index < TBLPACK_BYTES_MAX)) {
              c_len += snprintf(str + c_len,
                                (c_len < max_len) ? (max_len - c_len - 1) : 0,
                                "%02x ",
                                vals[index]);
            }
          }
        } else {
          decode_copy_bits(&vals[0],
                           0,
                           field->field_width,
                           fl_val_ptr,
                           field->field_offset,
                           field->field_width);
          for (fl_len = 0; fl_len < num_vals; fl_len++) {
            c_len += snprintf(str + c_len,
                              (c_len < max_len) ? (max_len - c_len - 1) : 0,
                              "%02x ",
                              vals[fl_len]);
          }
        }
        c_len += snprintf(
            str + c_len, (c_len < max_len) ? (max_len - c_len - 1) : 0, " \n");
        break;
      case TBL_PKG_FIELD_SOURCE_VERSION:
        field_value = get_val(
            (*exm_tbl_word)[mem_id], field->field_offset, field->field_width);
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_ADTPTR:
        field_value = get_val(
            (*exm_tbl_word)[mem_id], field->field_offset, field->field_width);
        // field_value = field_value <<  field->fieldsb;
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_METERPTR:
        field_value = get_val(
            (*exm_tbl_word)[mem_id], field->field_offset, field->field_width);
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_STATSPTR:
        field_value = get_val(
            (*exm_tbl_word)[mem_id], field->field_offset, field->field_width);
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_STFLPTR:
        field_value = get_val(
            (*exm_tbl_word)[mem_id], field->field_offset, field->field_width);
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_SELPTR:
        field_value = get_val(
            (*exm_tbl_word)[mem_id], field->field_offset, field->field_width);
        // field_value = field_value << field->fieldsb;
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_SELLEN:
        field_value = get_val(
            (*exm_tbl_word)[mem_id], field->field_offset, field->field_width);
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_SELLENSHIFT:
        field_value = get_val(
            (*exm_tbl_word)[mem_id], field->field_offset, field->field_width);
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_INSTR:
        field_value = get_val(
            (*exm_tbl_word)[mem_id], field->field_offset, field->field_width);
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_NXT_TBL:
        field_value = get_val(
            (*exm_tbl_word)[mem_id], field->field_offset, field->field_width);
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_ZERO: {
        uint16_t o = field->field_offset;
        uint16_t w = field->field_width;
        int count = (field->field_width + 63) / 64;
        if (count > 1) {
          for (int j = 0; j < count; ++j) {
            field_value = get_val((*exm_tbl_word)[mem_id], o, w > 64 ? 64 : w);
            print_field_value_in_bytes_fmt(field_value,
                                           w > 64 ? 64 : w,
                                           w <= 64 /* newline */,
                                           str,
                                           &c_len,
                                           max_len);
            w -= 64;
            o += 64;
          }
        } else {
          field_value = get_val(
              (*exm_tbl_word)[mem_id], field->field_offset, field->field_width);
          print_field_value_in_bytes_fmt(
              field_value, field->field_width, true, str, &c_len, max_len);
        }
        break;
      }
      case TBL_PKG_FIELD_SOURCE_IMMEDIATE:
        if (action_hdl_ptr->act_param) {
          act_param_ptr = action_hdl_ptr->act_param;
          // Need to traverse action parameter in reverse order.
          for (k = 0; k < action_hdl_ptr->param_count - 1; k++) {
            act_param_ptr++;
          }
          for (k = action_hdl_ptr->param_count; k > 0; k--) {
            if (act_param_ptr->is_parameter == 0) {
              field_value =
                  get_val((*exm_tbl_word)[mem_id],
                          field->field_offset + act_param_ptr->start_offset,
                          act_param_ptr->bit_width);
              c_len += snprintf(str + c_len,
                                (c_len < max_len) ? (max_len - c_len - 1) : 0,
                                "%-3d:%-3d   | %-24s | (%-3d..%-3d) | ",
                                bit_start + act_param_ptr->start_offset +
                                    act_param_ptr->bit_width - 1,
                                bit_start + act_param_ptr->start_offset,
                                "Constant-value",
                                field->fieldsb + act_param_ptr->bit_width - 1,
                                field->fieldsb);
              print_field_value_in_bytes_fmt(field_value,
                                             act_param_ptr->bit_width,
                                             true,
                                             str,
                                             &c_len,
                                             max_len);
            } else {
              // act_fld_name[0] = '\0';
              // get_action_field_name(act_fld_name, act_fn_hdl,
              //                      act_param_ptr->param_start,
              //                      act_param_ptr->param_width,
              //                      sizeof(act_fld_name));
              act_fld_name = pipemgr_tbl_pkg_get_field_string_name(
                  devid, prof_id, act_param_ptr->stringindex);

              fl_val_ptr = (*exm_tbl_word)[mem_id];
              num_vals = act_param_ptr->bit_width / 8;
              if ((act_param_ptr->bit_width % 8) > 0) num_vals++;
              PIPE_MGR_MEMSET(&vals, 0, sizeof(vals));
              // printf("field_offset %d, imm_start_off %d, imm_bit_width %d
              // \n",
              //        field->field_offset, act_param_ptr->start_offset,
              //        act_param_ptr->bit_width);
              decode_copy_bits(
                  &vals[0],
                  0,
                  act_param_ptr->bit_width,
                  fl_val_ptr,
                  field->field_offset + act_param_ptr->start_offset,
                  act_param_ptr->bit_width);

              c_len += snprintf(
                  str + c_len,
                  (c_len < max_len) ? (max_len - c_len - 1) : 0,
                  "%-3d:%-3d   | %-24s | (%-3d..%-3d) | ",
                  bit_start + act_param_ptr->start_offset +
                      act_param_ptr->bit_width - 1,
                  bit_start + act_param_ptr->start_offset,
                  act_fld_name,
                  act_param_ptr->param_shift + act_param_ptr->bit_width - 1,
                  act_param_ptr->param_shift);
              for (fl_len = 0; fl_len < num_vals; fl_len++) {
                c_len += snprintf(str + c_len,
                                  (c_len < max_len) ? (max_len - c_len - 1) : 0,
                                  "%02x ",
                                  vals[fl_len]);
              }
              c_len += snprintf(str + c_len,
                                (c_len < max_len) ? (max_len - c_len - 1) : 0,
                                " \n");
            }
            act_param_ptr--;
          }
        }
        break;
      default:
        break;
    }
    field++;
  }

  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "--------- | ------------------------ | ---------- | "
                    "-------------------\n");
  *c_str_len = c_len;
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_tof_tind_tbl_ent_to_str(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint8_t entry_position,
    tind_tbl_word_t *tind_tbl_word,
    char *str,
    int *c_str_len,
    int max_len,
    uint64_t *addr_arr) {
  (void)addr_arr;

  int c_len = *c_str_len;
  int fl_len = 0, num_vals = 0;
  int bit_start = 0, bit_end = 0;
  uint8_t vals[TBLPACK_WORD_BYTES_MAX] = {0};
  uint8_t *fl_val_ptr;
  uint64_t field_value = 0;
  uint8_t mem_id_vals[TBLPACK_WORD_BYTES_MAX] = {0};
  int mem_id = 0, i, k;
  // char        act_fld_name[TBLPACK_SMALL_STR_LEN];
  char *act_fld_name;
  pipe_act_fn_hdl_t act_fn_hdl = 0;
  pipemgr_tbl_pkg_action_entry_field_t *action_hdl_ptr;
  pipemgr_tbl_pkg_match_entry_line_t *entry_line_ptr;
  pipemgr_tbl_pkg_match_entry_field_t *field;
  pipemgr_tbl_pkg_action_parameter_field_t *act_param_ptr;
  uint32_t bj_hash;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  char *f_name, *immediate_name;

  act_fn_hdl = get_tind_action_fn_hdl(devid,
                                      prof_id,
                                      stage_id,
                                      mat_tbl_hdl,
                                      entry_position,
                                      0 /*offset*/,
                                      tind_tbl_word);
  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "Action Function Handle: 0x%x \n",
                    act_fn_hdl);

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tind_lut_depth,
      mat_tbl_hdl,
      stage_id,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tind_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tind_lut,
      mat_tbl_hdl,
      stage_id);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl %d, stage "
        "%d",
        __func__,
        mat_tbl_hdl,
        stage_id);
    return (PIPE_INVALID_ARG);
  }

  entry_line_ptr = pipemmgr_entry_format_get_entry_details_ptr(
      entry_position, lut_ptr->u.tind_ptr);
  if (!entry_line_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d, entry %d",
        __func__,
        mat_tbl_hdl,
        stage_id,
        entry_position);
    return (PIPE_INVALID_ARG);
  }

  action_hdl_ptr =
      pipemmgr_entry_format_get_action_handle_details_ptr(act_fn_hdl, lut_ptr);
  if (!action_hdl_ptr) {
    LOG_ERROR(
        "%s: Unable to find action entry formatting details for action-tbl "
        "0x%x, stage %d, entry %d",
        __func__,
        act_fn_hdl,
        stage_id,
        entry_position);
    return (PIPE_INVALID_ARG);
  }

  field = entry_line_ptr->fields;
  for (i = 0; i < entry_line_ptr->field_count; i++) {
    mem_id = field->memword_index[0];
    bit_start = field->field_offset;
    bit_end = bit_start + field->field_width - 1;
    if (mem_id_vals[mem_id] == 0) {
      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        "Tind Address: 0x%" PRIx64 " \n",
                        addr_arr[mem_id]);
      dump_tbl_word((uint8_t *)tind_tbl_word,
                    mem_id * TBLPACK_TOF_BYTES_IN_RAM_WORD,
                    str,
                    &c_len,
                    max_len,
                    0,
                    TBLPACK_TOF_BYTES_IN_RAM_WORD * 8);
      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        "--------- | ------------------------ | ---------- | "
                        "-------------------\n");
      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        "%-9s | %-24s | %-10s | %-20s\n",
                        "Bit-field",
                        "Field-Name",
                        "Fld-Range",
                        "Value");
      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        "--------- | ------------------------ | ---------- | "
                        "-------------------\n");
    }

    mem_id_vals[mem_id] = 1;
    f_name = pipemgr_tbl_pkg_get_field_string_name(
        devid, prof_id, field->stringindex);
    if (field->source != TBL_PKG_FIELD_SOURCE_IMMEDIATE) {
      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        "%-3d:%-3d   | %-24s | (%-3d..%-3d) | ",
                        bit_end,
                        bit_start,
                        f_name,
                        field->fieldsb + field->field_width - 1,
                        field->fieldsb);
    }
    switch (field->source) {
      case TBL_PKG_FIELD_SOURCE_ZERO:
        field_value =
            get_val(*tind_tbl_word, field->field_offset, field->field_width);
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_INSTR:
        field_value =
            get_val(*tind_tbl_word, field->field_offset, field->field_width);
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_ADTPTR:
        field_value =
            get_val(*tind_tbl_word, field->field_offset, field->field_width);
        // field_value = field_value <<  field->fieldsb;
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_METERPTR:
        field_value =
            get_val(*tind_tbl_word, field->field_offset, field->field_width);
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_STATSPTR:
        field_value =
            get_val(*tind_tbl_word, field->field_offset, field->field_width);
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_STFLPTR:
        field_value =
            get_val(*tind_tbl_word, field->field_offset, field->field_width);
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_SELPTR:
        field_value =
            get_val(*tind_tbl_word, field->field_offset, field->field_width);
        // field_value = field_value << field->fieldsb;
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_SELLEN:
        field_value =
            get_val(*tind_tbl_word, field->field_offset, field->field_width);
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_SELLENSHIFT:
        field_value =
            get_val(*tind_tbl_word, field->field_offset, field->field_width);
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_NXT_TBL:
        field_value =
            get_val(*tind_tbl_word, field->field_offset, field->field_width);
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_IMMEDIATE:
        if (action_hdl_ptr->act_param) {
          act_param_ptr = action_hdl_ptr->act_param;
          // Need to traverse action parameter in reverse order.
          for (k = 0; k < action_hdl_ptr->param_count - 1; k++) {
            act_param_ptr++;
          }
          for (k = action_hdl_ptr->param_count; k > 0; k--) {
            if (act_param_ptr->is_parameter == 0) {
              immediate_name = pipemgr_tbl_pkg_get_field_string_name(
                  devid, prof_id, field->stringindex);
              field_value =
                  get_val(*tind_tbl_word,
                          field->field_offset + act_param_ptr->start_offset,
                          act_param_ptr->bit_width);
              c_len += snprintf(
                  str + c_len,
                  (c_len < max_len) ? (max_len - c_len - 1) : 0,
                  "%-3d:%-3d   | %-24s | (%-3d..%-3d) | ",
                  bit_start + act_param_ptr->start_offset +
                      act_param_ptr->bit_width - 1,
                  bit_start + act_param_ptr->start_offset,
                  immediate_name,
                  act_param_ptr->start_offset + act_param_ptr->bit_width - 1,
                  act_param_ptr->start_offset);
              print_field_value_in_bytes_fmt(field_value,
                                             act_param_ptr->bit_width,
                                             true,
                                             str,
                                             &c_len,
                                             max_len);
            } else {
              // act_fld_name[0] = '\0';
              // get_action_field_name(act_fld_name, act_fn_hdl,
              //                      act_param_ptr->param_start,
              //                      act_param_ptr->param_width,
              //                      sizeof(act_fld_name));
              act_fld_name = pipemgr_tbl_pkg_get_field_string_name(
                  devid, prof_id, act_param_ptr->stringindex);

              fl_val_ptr = *tind_tbl_word;
              num_vals = act_param_ptr->bit_width / 8;
              if ((act_param_ptr->bit_width % 8) > 0) num_vals++;
              PIPE_MGR_MEMSET(&vals, 0, sizeof(vals));
              decode_copy_bits(
                  &vals[0],
                  0,
                  act_param_ptr->bit_width,
                  fl_val_ptr,
                  field->field_offset + act_param_ptr->start_offset,
                  act_param_ptr->bit_width);
              c_len += snprintf(
                  str + c_len,
                  (c_len < max_len) ? (max_len - c_len - 1) : 0,
                  "%-3d:%-3d   | %-24s | (%-3d..%-3d) | ",
                  bit_start + act_param_ptr->start_offset +
                      act_param_ptr->bit_width - 1,
                  bit_start + act_param_ptr->start_offset,
                  act_fld_name,
                  act_param_ptr->param_shift + act_param_ptr->bit_width - 1,
                  act_param_ptr->param_shift);
              for (fl_len = 0; fl_len < num_vals; fl_len++) {
                c_len += snprintf(str + c_len,
                                  (c_len < max_len) ? (max_len - c_len - 1) : 0,
                                  "%02x ",
                                  vals[fl_len]);
              }
              c_len += snprintf(str + c_len,
                                (c_len < max_len) ? (max_len - c_len - 1) : 0,
                                " \n");
            }
            act_param_ptr--;
          }
        }
        break;
      default:
        break;
    }
    field++;
  }
  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "--------- | ------------------------ | ---------- | "
                    "-------------------\n");
  *c_str_len = c_len;
  return PIPE_SUCCESS;
}

static pipe_status_t get_action_data_spec_widths(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_act_fn_hdl_t act_fn_hdl,
    uint32_t *num_valid_action_data_bits_p,
    uint32_t *num_action_data_bytes_p) {
  uint32_t bj_hash, i;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_action_spec_t *actionspec_ptr;
  pipemgr_tbl_pkg_spec_field_t *field_ptr;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  if (dev_info->fake_rmt_cfg) {
    return PIPE_SUCCESS;
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).actionspec_lut_depth,
      act_fn_hdl,
      0,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).actionspec_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).actionspec_lut,
      act_fn_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find action spec details for action function "
        "handle "
        "0x%x",
        __func__,
        act_fn_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  actionspec_ptr = lut_ptr->u.actionspec_ptr;
  field_ptr = actionspec_ptr->fields;
  *num_valid_action_data_bits_p = 0;
  *num_action_data_bytes_p = 0;
  for (i = 0; i < actionspec_ptr->fieldcount; i++) {
    *num_valid_action_data_bits_p += field_ptr->fieldwidth;
    *num_action_data_bytes_p += (field_ptr->fieldwidth + 7) >> 3;
    field_ptr++;
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_tof_tind_tbl_ent_decode_to_components(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint8_t entry_position,
    uint16_t offset,
    uint8_t *tind_tbl_word,
    pipe_action_data_spec_t *act_data_spec,
    pipe_act_fn_hdl_t *act_fn_hdl,
    pipe_mgr_indirect_ptrs_t *indirect_ptrs,
    pipe_mgr_ent_decode_ptr_info_t *ptr_info) {
  pipemgr_tbl_pkg_action_entry_field_t *action_hdl_ptr = NULL;
  pipemgr_tbl_pkg_match_entry_line_t *entry_line_ptr;
  pipemgr_tbl_pkg_match_entry_field_t *field;
  pipemgr_tbl_pkg_action_parameter_field_t *act_param_ptr;
  uint32_t bj_hash;
  uint8_t sel_len_shift = 0;
  uint32_t i, k;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  bool sel_ptr_found = false;
  bool sel_len_found = false;
  bool sel_len_shift_found = false;
  uint32_t indirect_read_ptr = 0;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  rmt_dev_profile_info_t *prof_info = dev_info->profile_info[prof_id];

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tind_lut_depth,
      mat_tbl_hdl,
      stage_id,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tind_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tind_lut,
      mat_tbl_hdl,
      stage_id);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl %d, stage "
        "%d",
        __func__,
        mat_tbl_hdl,
        stage_id);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  entry_line_ptr = pipemmgr_entry_format_get_entry_details_ptr(
      entry_position, lut_ptr->u.tind_ptr);
  if (!entry_line_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d, entry %d",
        __func__,
        mat_tbl_hdl,
        stage_id,
        entry_position);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }
  *act_fn_hdl = get_tind_action_fn_hdl(devid,
                                       prof_id,
                                       stage_id,
                                       mat_tbl_hdl,
                                       entry_position,
                                       offset,
                                       (tind_tbl_word_t *)tind_tbl_word);
  action_hdl_ptr =
      pipemmgr_entry_format_get_action_handle_details_ptr(*act_fn_hdl, lut_ptr);
  if (!action_hdl_ptr) {
    LOG_ERROR(
        "%s: Unable to find action handle formatting details for match-tbl "
        "0x%x, stage %d",
        __func__,
        mat_tbl_hdl,
        stage_id);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  if (ptr_info) {
    ptr_info->force_stats = action_hdl_ptr->force_stats_addr;
    ptr_info->force_meter = action_hdl_ptr->force_meter_addr;
    ptr_info->force_stful = action_hdl_ptr->force_stful_addr;
  }

  if (act_data_spec->num_valid_action_data_bits == 0 ||
      act_data_spec->num_action_data_bytes == 0) {
    /* Based on the action function handle, determine the number of valid action
     * data bytes and bits */
    uint32_t num_valid_action_data_bits = 0, num_action_data_bytes = 0;
    get_action_data_spec_widths(devid,
                                prof_id,
                                *act_fn_hdl,
                                &num_valid_action_data_bits,
                                &num_action_data_bytes);
    act_data_spec->num_valid_action_data_bits = num_valid_action_data_bits;
    act_data_spec->num_action_data_bytes = num_action_data_bytes;
  }

  field = entry_line_ptr->fields;
  for (i = 0; i < entry_line_ptr->field_count; i++) {
    switch (field->source) {
      case TBL_PKG_FIELD_SOURCE_INSTR:
        break;
      case TBL_PKG_FIELD_SOURCE_ADTPTR:
        indirect_read_ptr = extract_val(
            tind_tbl_word, field->field_offset + offset, field->field_width);
        indirect_ptrs->adt_ptr |= indirect_read_ptr << field->fieldsb;
        if (!field->perflow) {
          indirect_ptrs->adt_ptr |= 1 << TOF_ADT_ADDR_PFE_BIT_POSITION;
        }
        break;
      case TBL_PKG_FIELD_SOURCE_STATSPTR:
        indirect_read_ptr = extract_val(
            tind_tbl_word, field->field_offset + offset, field->field_width);
        if (!transform_ptr(prof_info)) {
          indirect_ptrs->stats_ptr |= indirect_read_ptr << field->fieldsb;
        } else {
          indirect_ptrs->stats_ptr = indirect_read_ptr;
          if (!field->perflow) {
            indirect_ptrs->stats_ptr |= (1 << field->field_width);
          }
          /* Now, left shift the address by the number of bits defaulted by the
           * compiler.
           */
          indirect_ptrs->stats_ptr <<= field->fieldsb;
        }
        break;
      case TBL_PKG_FIELD_SOURCE_METERPTR:
        indirect_read_ptr = extract_val(
            tind_tbl_word, field->field_offset + offset, field->field_width);
        if (!transform_ptr(prof_info)) {
          indirect_ptrs->meter_ptr |= indirect_read_ptr << field->fieldsb;
        } else {
          indirect_ptrs->meter_ptr =
              pipemgr_tbl_pkg_reverse_transform_meter_ptr(
                  indirect_read_ptr, field, action_hdl_ptr);
        }
        break;
      case TBL_PKG_FIELD_SOURCE_SELPTR:
        indirect_read_ptr = extract_val(
            tind_tbl_word, field->field_offset + offset, field->field_width);
        indirect_ptrs->sel_ptr |= indirect_read_ptr << field->fieldsb;
        sel_ptr_found = true;
        break;
      case TBL_PKG_FIELD_SOURCE_SELLEN:
        indirect_ptrs->sel_len = extract_val(
            tind_tbl_word, field->field_offset + offset, field->field_width);
        sel_len_found = true;
        if (sel_len_shift_found) {
          indirect_ptrs->sel_len <<= sel_len_shift;
        }
        break;
      case TBL_PKG_FIELD_SOURCE_SELLENSHIFT:
        sel_len_shift_found = true;
        sel_len_shift = extract_val(
            tind_tbl_word, field->field_offset + offset, field->field_width);
        if (sel_len_found) {
          indirect_ptrs->sel_len <<= sel_len_shift;
        }
        break;
      case TBL_PKG_FIELD_SOURCE_STFLPTR:
        indirect_read_ptr = extract_val(
            tind_tbl_word, field->field_offset + offset, field->field_width);
        if (!transform_ptr(prof_info)) {
          indirect_ptrs->stfl_ptr |= indirect_read_ptr << field->fieldsb;
        } else {
          indirect_ptrs->stfl_ptr = indirect_read_ptr;
          /* Stateful address contain the 23 bits (including the LSB zeros)
           * followed
           * a PFE bit, followed by a default "1" and followed by 2 bits of
           * stateful ALU instruction index. If a PFE is present then it will
           * in the bit position (field_length - 2 - 1). When we return the
           * stateful address, we don't care about the opcode part here. Hence
           * when putting the PFE bit back, we just put it as the 24th bit of
           * the
           * extracted address which may clobber a bit in the opcode, but that
           * is not used to interpret the stateful address from this decode.
           */
          if (!field->perflow && field->field_width >= 2) {
            indirect_ptrs->stfl_ptr |= (1ul << (field->field_width - 2));
          }
          /* Pad the LSBs with appropriate number of zeros */
          indirect_ptrs->stfl_ptr <<= field->fieldsb;
        }
        break;
      case TBL_PKG_FIELD_SOURCE_IMMEDIATE:
        if (action_hdl_ptr->act_param && act_data_spec->action_data_bits) {
          act_param_ptr = action_hdl_ptr->act_param;
          for (k = 0; k < action_hdl_ptr->param_count; k++) {
            if (act_param_ptr->is_parameter != 0) {
              uint16_t start_offset = (act_param_ptr->param_start +
                                       act_param_ptr->param_width - 1) -
                                      7;
              uint8_t byte_offset = act_param_ptr->param_shift / 8;
              uint8_t bit_offset = act_param_ptr->param_shift % 8;
              start_offset -= (byte_offset * 8);
              start_offset += bit_offset;
              decode_bits_to_spec(
                  act_data_spec->action_data_bits,
                  start_offset,
                  act_param_ptr->bit_width,
                  &tind_tbl_word,
                  0,
                  field->field_offset + offset + act_param_ptr->start_offset,
                  act_param_ptr->bit_width);
            }
            act_param_ptr++;
          }
        }
        break;
      default:
        break;
    }
    field++;
  }

  /* Since the sel len and sel ptr could be read out of order, perform the
   * selector ptr decoding after all fields are found. If the selector length
   * is not found, set it to the default value of 1 word.
   */
  if (sel_ptr_found) {
    if (!sel_len_found) {
      if (indirect_ptrs->sel_ptr & (1 << TOF_METER_ADDR_PFE_BIT_POSITION)) {
        indirect_ptrs->sel_len = 1;
      } else {
        indirect_ptrs->sel_len = 0;
      }
    }
    indirect_ptrs->sel_ptr &= ~(1 << TOF_METER_ADDR_PFE_BIT_POSITION);
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_tof_adt_tbl_ent_decode(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_adt_tbl_hdl_t adt_tbl_hdl,
    pipe_act_fn_hdl_t act_fn_hdl,
    uint8_t entry_position,
    adt_tbl_word_t *adt_tbl_word,
    char *str,
    int *c_str_len,
    int max_len,
    uint64_t *addr_arr) {
  int c_len = *c_str_len;
  int fl_len = 0, num_vals = 0;
  int bit_start = 0, bit_end = 0;
  uint8_t vals[TBLPACK_WORD_BYTES_MAX] = {0};
  uint8_t *fl_val_ptr;
  uint64_t field_value = 0;
  uint8_t mem_id_vals[TBLPACK_WORD_BYTES_MAX] = {0};
  int mem_id = 0;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_act_fn_handle_t *act_fn_hdl_ptr;
  pipemgr_tbl_pkg_act_fn_stage_format_t *stageformat_ptr;
  pipemgr_tbl_pkg_act_fn_entry_t *entry_ptr;
  pipemgr_tbl_pkg_act_fn_entry_field_t *field;
  uint8_t const_start_offset, const_bit_width;
  uint8_t const_end_offset, shift, start_bit, width;
  pipemgr_tbl_pkg_act_fn_field_constvalues_t *constvalues;

  int i, j;
  char *f_name;

  bf_dev_id_t devid = dev_info->dev_id;

  if (dev_info->fake_rmt_cfg) {
    return PIPE_SUCCESS;
  }

  lut_ptr = get_adt_lut_entry(devid, prof_id, adt_tbl_hdl);
  if (!lut_ptr) {
    LOG_ERROR("%s: Unable to find entry formatting details for ADT handle 0x%x",
              __func__,
              adt_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  act_fn_hdl_ptr = find_adt_act_fn_handle(lut_ptr, act_fn_hdl);
  if (!act_fn_hdl_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for action function "
        "handle 0x%x",
        __func__,
        act_fn_hdl);
    return PIPE_UNEXPECTED;
  }

  stageformat_ptr = find_act_fn_stage_format(act_fn_hdl_ptr, stage_id);
  if (!stageformat_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for action function "
        "handle 0x%x, stage %d",
        __func__,
        act_fn_hdl,
        stage_id);
    return PIPE_UNEXPECTED;
  }

  entry_ptr = find_act_fn_entry_format(stageformat_ptr, entry_position);
  if (!entry_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for action function "
        "handle 0x%x, stage %d, entry position %d",
        __func__,
        act_fn_hdl,
        stage_id,
        entry_position);
    return PIPE_UNEXPECTED;
  }

  field = entry_ptr->field;
  for (i = 0; i < entry_ptr->entryfieldcount; i++) {
    mem_id = field->word_index;
    bit_start = field->field_offset;
    bit_end = bit_start + field->field_width - 1;
    if (mem_id_vals[mem_id] == 0) {
      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        "Action-table Address: 0x%" PRIx64 " \n",
                        addr_arr[mem_id]);
      dump_tbl_word((uint8_t *)adt_tbl_word,
                    mem_id * TBLPACK_TOF_BYTES_IN_RAM_WORD,
                    str,
                    &c_len,
                    max_len,
                    0,
                    TBLPACK_TOF_BYTES_IN_RAM_WORD * 8);
      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        "--------- | ------------------------ | ---------- | "
                        "-------------------\n");
      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        "%-9s | %-24s | %-10s | %-20s\n",
                        "Bit-field",
                        "Field-Name",
                        "Fld-Range",
                        "Value");
      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        "--------- | ------------------------ | ---------- | "
                        "-------------------\n");
    }
    mem_id_vals[mem_id] = 1;
    f_name = pipemgr_tbl_pkg_get_field_string_name(
        devid, prof_id, field->stringindex);
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "%-3d:%-3d   | %-24s | (%-3d..%-3d) | ",
                      bit_end,
                      bit_start,
                      f_name,
                      field->shift + field->field_width - 1,
                      field->shift);
    switch (field->source) {
      case TBL_PKG_FIELD_SOURCE_ZERO:
        field_value = get_val((*adt_tbl_word)[field->word_index],
                              field->field_offset,
                              field->field_width);
        print_field_value_in_bytes_fmt(
            field_value, field->field_width, true, str, &c_len, max_len);
        break;
      case TBL_PKG_FIELD_SOURCE_CONSTANT:
        constvalues = field->constvalues;
        for (j = 0; j < field->constvalue_count; j++) {
          const_start_offset = constvalues->dststart;
          const_bit_width = constvalues->dstwidth;
          const_end_offset = const_start_offset + const_bit_width - 1;
          if ((const_start_offset <= field->shift) &&
              (const_end_offset >= field->shift)) {
            shift = field->shift - const_start_offset;
            start_bit = field->field_offset;
            width =
                ENTRYFORMAT_MIN(field->field_width, const_bit_width - shift);
            field_value =
                get_val((*adt_tbl_word)[field->word_index], start_bit, width);
            // field_value = field_value << shift;
            print_field_value_in_bytes_fmt(
                field_value, width, true, str, &c_len, max_len);
          } else if ((const_start_offset >= field->shift) &&
                     (const_start_offset <=
                      field->shift + field->field_width - 1)) {
            // Const starts in field and ends within field or ends after the
            // field
            int end_bit_pos = ENTRYFORMAT_MIN(
                field->field_offset + field->field_width - 1,
                field->field_offset + const_end_offset - field->shift);
            int start_bit_pos = field->field_offset + const_start_offset;
            int bit_width = end_bit_pos - start_bit_pos + 1;
            field_value = get_val(
                (*adt_tbl_word)[field->word_index], start_bit_pos, bit_width);
            print_field_value_in_bytes_fmt(
                field_value, bit_width, true, str, &c_len, max_len);
          }
          constvalues++;
        }
        break;
      case TBL_PKG_FIELD_SOURCE_SPEC:
        fl_val_ptr = (*adt_tbl_word)[field->word_index];
        num_vals = field->field_width / 8;
        if ((field->field_width % 8) > 0) num_vals++;
        memset(&vals, 0, sizeof(vals));
        decode_copy_bits(&vals[0],
                         0,
                         field->field_width,
                         fl_val_ptr,
                         field->field_offset,
                         field->field_width);
        for (fl_len = 0; fl_len < num_vals; fl_len++) {
          c_len += snprintf(str + c_len,
                            (c_len < max_len) ? (max_len - c_len - 1) : 0,
                            "%02x ",
                            vals[fl_len]);
        }
        c_len += snprintf(
            str + c_len, (c_len < max_len) ? (max_len - c_len - 1) : 0, " \n");
        break;
      default:
        break;
    }
    field++;
  }
  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "--------- | ------------------------ | ---------- | "
                    "-------------------\n");
  *c_str_len = c_len;
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_tof_adt_tbl_ent_decode_to_action_spec(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_adt_tbl_hdl_t adt_tbl_hdl,
    pipe_act_fn_hdl_t act_fn_hdl,
    uint8_t entry_position,
    pipe_action_data_spec_t *act_data_spec,
    uint8_t **adt_tbl_word) {
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_act_fn_handle_t *act_fn_hdl_ptr;
  pipemgr_tbl_pkg_act_fn_stage_format_t *stageformat_ptr;
  pipemgr_tbl_pkg_act_fn_entry_t *entry_ptr;
  pipemgr_tbl_pkg_act_fn_entry_field_t *field;
  uint32_t constcount;
  int i;
  bf_dev_id_t devid = dev_info->dev_id;

  if (dev_info->fake_rmt_cfg) {
    return PIPE_SUCCESS;
  }

  lut_ptr = get_adt_lut_entry(devid, prof_id, adt_tbl_hdl);
  if (!lut_ptr) {
    LOG_ERROR("%s: Unable to find entry formatting details for ADT handle 0x%x",
              __func__,
              adt_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  act_fn_hdl_ptr = find_adt_act_fn_handle(lut_ptr, act_fn_hdl);
  if (!act_fn_hdl_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for action function "
        "handle 0x%x",
        __func__,
        act_fn_hdl);
    return PIPE_UNEXPECTED;
  }

  stageformat_ptr = find_act_fn_stage_format(act_fn_hdl_ptr, stage_id);
  if (!stageformat_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for action function "
        "handle 0x%x, stage %d",
        __func__,
        act_fn_hdl,
        stage_id);
    return PIPE_UNEXPECTED;
  }

  entry_ptr = find_act_fn_entry_format(stageformat_ptr, entry_position);
  if (!entry_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for action function "
        "handle 0x%x, stage %d, entry position %d",
        __func__,
        act_fn_hdl,
        stage_id,
        entry_position);
    return PIPE_UNEXPECTED;
  }

  field = entry_ptr->field;
  for (i = 0; i < entry_ptr->entryfieldcount; i++) {
    switch (field->source) {
      case TBL_PKG_FIELD_SOURCE_ZERO:
        break;
      case TBL_PKG_FIELD_SOURCE_CONSTANT:
        break;
      case TBL_PKG_FIELD_SOURCE_SPEC:
        if (!act_data_spec->action_data_bits) {
          break;
        }
        if (field->is_mod_field_conditionally_mask) {
          uint64_t vals[TBLPACK_WORD_BYTES_MAX] = {0};
          get_shifted_val_decode(adt_tbl_word[field->word_index],
                                 field->field_offset,
                                 field->field_width,
                                 vals);
          if (vals[0]) {
            /* The conditionally modified field (or mask) can be of any width,
             * we just need to check if its non-zero. During encoding, the
             * passed in field is either 0 or 1. If its 0, the whole field is
             * encoded as 0, if its 1, the whole field is enoded as all 1's.
             */
            act_data_spec->action_data_bits[field->source_offset / 8] = 1;
          } else {
            act_data_spec->action_data_bits[field->source_offset / 8] = 0;
          }
        } else {
          uint16_t start_offset =
              (field->source_offset + field->source_width - 1) - 7;
          uint8_t byte_offset = field->shift / 8;
          uint8_t bit_offset = field->shift % 8;
          start_offset -= (byte_offset * 8);
          start_offset += bit_offset;
          decode_bits_to_spec(act_data_spec->action_data_bits,
                              start_offset,
                              field->field_width,
                              adt_tbl_word,
                              field->word_index,
                              field->field_offset,
                              field->field_width);
        }
        break;
      default:
        break;
    }
    constcount = field->constvalue_count;
    field++;
    field = (pipemgr_tbl_pkg_act_fn_entry_field_t
                 *)((uint8_t *)field +
                    (constcount *
                     sizeof(pipemgr_tbl_pkg_act_fn_field_constvalues_t)));
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_tof_tern_decode_to_key_mask(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    bool mem_id_valid,
    uint8_t mem_id,
    bool wide_word_idx_valid,
    uint8_t wide_word_idx,
    uint8_t *word0,
    uint8_t *word1) {
  uint64_t range_mask = 0;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_tern_range_mask_t *rangemask;
  uint32_t bj_hash, i, j;

  PIPE_MGR_DBGCHK(mem_id_valid || wide_word_idx_valid);

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tern_lut_depth,
      mat_tbl_hdl,
      stage_id,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tern_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tern_lut,
      mat_tbl_hdl,
      stage_id);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for tern table handle "
        "0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  rangemask = lut_ptr->u.tern_ptr->rangeformat;
  if (mem_id_valid) {
    for (i = 0; i < lut_ptr->u.tern_ptr->rangemaskcount; i++) {
      for (j = 0; j < TBL_PKG_MAX_DEPTH_TCAM_UNITS; j++) {
        if (rangemask->memids[j] != mem_id) {
          range_mask = rangemask->mask;
          break;
        }
      }
      rangemask++;
    }
  } else if (wide_word_idx_valid) {
    for (i = 0; i < lut_ptr->u.tern_ptr->rangemaskcount; i++) {
      if (rangemask->word_index != wide_word_idx) {
        rangemask++;
      } else {
        range_mask = rangemask->mask;
        break;
      }
    }
  }
  extract_key_mask_2bit_mode(word0, word1, range_mask);
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_tof_tern_tbl_ent_to_str(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    tern_tbl_word_t *tbl_word0,
    tern_tbl_word_t *tbl_word1,
    char *str,
    int *c_str_len,
    int max_len,
    uint64_t *addr_arr) {
  int c_len = *c_str_len;
  uint64_t field_value0 = 0, field_value1 = 0;
  int bit_start = 0, bit_end = 0;
  uint8_t version_key = 0, version_mask = 0;
  int fl_len = 0, len_key = 0, shift_left = 0;
  int mem_id = 0, mem_id_vals[TBLPACK_WORD_BYTES_MAX] = {0};
  uint64_t key[TBLPACK_WORD_BYTES_MAX] = {0}, mask[16] = {0};
  uint8_t *key_ptr, *mask_ptr;
  uint64_t new_key[TBLPACK_WORD_BYTES_MAX] = {0},
           new_mask[TBLPACK_WORD_BYTES_MAX] = {0};
  uint64_t payload[TBLPACK_WORD_BYTES_MAX] = {0},
           mrd[TBLPACK_WORD_BYTES_MAX] = {0};
  char f0_str[TBLPACK_SMALL_STR_LEN], f1_str[TBLPACK_SMALL_STR_LEN];
  int f0_len = 0, f1_len = 0;
  ;
  int f0_max_len = TBLPACK_SMALL_STR_LEN, f1_max_len = TBLPACK_SMALL_STR_LEN;
  int prev_mem_id = -1;
  char str_t[TBLPACK_LONG_STR_LEN];
  int c_len_t = 0, max_len_t = TBLPACK_LONG_STR_LEN;
  uint32_t bj_hash, i, j;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_tern_entry_t *entry;
  pipemgr_tbl_pkg_tern_entry_field_t *field;
  uint8_t field_width;
  uint16_t range_bitvals;
  uint64_t range_field0, range_field1;

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tern_lut_depth,
      mat_tbl_hdl,
      stage_id,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tern_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tern_lut,
      mat_tbl_hdl,
      stage_id);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for tern table handle "
        "0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  for (i = 0; i < TOF_MAX_TCAM_WORDS_IN_TERN_TBL_WORD; i++) {
    pipe_mgr_entry_format_tof_tern_decode_to_key_mask(devid,
                                                      prof_id,
                                                      stage_id,
                                                      mat_tbl_hdl,
                                                      false,
                                                      0,
                                                      true,
                                                      i,
                                                      (*tbl_word0)[i],
                                                      (*tbl_word1)[i]);
  }
  c_len += snprintf(
      str + c_len, (c_len < max_len) ? (max_len - c_len - 1) : 0, "\n");

  entry = lut_ptr->u.tern_ptr->entry;
  for (i = 0; i < lut_ptr->u.tern_ptr->entrycount; i++) {
    field = entry->field;
    for (j = 0; j < entry->entryfieldcount; j++) {
      field_width = field->bitwidth;
      mem_id = field->memword_index[0];
      bit_start = field->lsbmemwordoffset - 1;  // subtract one for payload bit
      if (bit_start >= 46)
        bit_start -= 2;  // if this is the 47th bit after parity
      bit_end = bit_start + field->bitwidth - 1;
      shift_left = field->lsbmemwordoffset;
      if (mem_id_vals[mem_id] == 0) {
        /* Print the prev mem-ids addr and value */
        if (c_len_t > 0) {
          c_len += snprintf(str + c_len,
                            (c_len < max_len) ? (max_len - c_len - 1) : 0,
                            "--------- | ------------------------ | ---------- "
                            "| -------------------\n");

          c_len += snprintf(str + c_len,
                            (c_len < max_len) ? (max_len - c_len - 1) : 0,
                            "Tcam address: 0x%" PRIx64 " \nValue (43..0): ",
                            addr_arr[prev_mem_id]);
          /* Print the key/mask value without payload and parity */
          print_tcam_in_key_mask_fmt(new_key[prev_mem_id],
                                     new_mask[prev_mem_id],
                                     str,
                                     &c_len,
                                     max_len,
                                     TBLPACK_TOF_BYTES_IN_TCAM_WORD * 8);

          c_len +=
              snprintf(str + c_len,
                       (c_len < max_len) ? (max_len - c_len - 1) : 0,
                       "\nMrd: 0x%02" PRIx64 ", payload 0x%02" PRIx64 " \n",
                       mrd[prev_mem_id],
                       payload[prev_mem_id]);
          c_len += snprintf(str + c_len,
                            (c_len < max_len) ? (max_len - c_len - 1) : 0,
                            "%s",
                            str_t);

          c_len_t = 0;
        }
        /* Start printing in temp buffer */
        c_len_t +=
            snprintf(str_t + c_len_t,
                     (c_len_t < max_len_t) ? (max_len_t - c_len_t - 1) : 0,
                     "--------- | ------------------------ | ---------- | "
                     "-------------------\n");
        c_len_t +=
            snprintf(str_t + c_len_t,
                     (c_len_t < max_len_t) ? (max_len_t - c_len_t - 1) : 0,
                     "%-9s | %-24s | %-10s | %-20s\n",
                     "Bit-field",
                     "Field-Name",
                     "Fld-Range",
                     "Key / Mask");
        c_len_t +=
            snprintf(str_t + c_len_t,
                     (c_len_t < max_len_t) ? (max_len_t - c_len_t - 1) : 0,
                     "--------- | ------------------------ | ---------- | "
                     "-------------------\n");
      }

      if ((field->location != TBL_PKG_TERN_FIELD_LOCATION_PARITY) &&
          (field->location != TBL_PKG_TERN_FIELD_LOCATION_PAYLOAD)) {
        c_len_t +=
            snprintf(str_t + c_len_t,
                     (c_len_t < max_len_t) ? (max_len_t - c_len_t - 1) : 0,
                     "%-3d:%-3d   | %-24s | (%-3d..%-3d) | ",
                     bit_end,
                     bit_start,
                     pipemgr_tbl_pkg_get_field_string_name(
                         devid, prof_id, field->stringindex),
                     field->startbit + field->bitwidth - 1,
                     field->startbit);
      }

      prev_mem_id = mem_id;
      mem_id_vals[mem_id] = 1;

      switch (field->location) {
        case TBL_PKG_TERN_FIELD_LOCATION_ZERO:
          decode_key_mask_no_encode((uint8_t *)&field_value0,
                                    (uint8_t *)&field_value1,
                                    0,
                                    field_width,
                                    (*tbl_word0)[field->memword_index[0]],
                                    (*tbl_word1)[field->memword_index[0]],
                                    field->lsbmemwordoffset,
                                    field->bitwidth);
          f0_len = f1_len = 0;
          print_field_value_in_bytes_fmt(
              field_value0, field_width, false, f0_str, &f0_len, f0_max_len);
          print_field_value_in_bytes_fmt(
              field_value1, field_width, false, f1_str, &f1_len, f1_max_len);
          c_len_t +=
              snprintf(str_t + c_len_t,
                       (c_len_t < max_len_t) ? (max_len_t - c_len_t - 1) : 0,
                       "%s\n%-9s | %-24s | %-10s | %s\n",
                       f0_str,
                       " ",
                       " ",
                       " ",
                       f1_str);

          /* Reverse the bytes before adding them to key and mask  */
          key_ptr = (uint8_t *)&field_value0;
          mask_ptr = (uint8_t *)&field_value1;
          len_key = field_width / 8;
          if ((field_width % 8) > 0) len_key++;
          for (fl_len = 0; fl_len < len_key; fl_len++) {
            new_key[mem_id] |= ((uint64_t)*key_ptr
                                << (shift_left + (len_key - fl_len - 1) * 8));
            new_mask[mem_id] |= ((uint64_t)*mask_ptr
                                 << (shift_left + (len_key - fl_len - 1) * 8));
            key_ptr++;
            mask_ptr++;
          }
          break;
        case TBL_PKG_TERN_FIELD_LOCATION_PARITY:
          field_value0 = get_val((*tbl_word0)[field->memword_index[0]],
                                 field->lsbmemwordoffset,
                                 field->bitwidth);
          field_value1 = get_val((*tbl_word1)[field->memword_index[0]],
                                 field->lsbmemwordoffset,
                                 field->bitwidth);
          new_key[mem_id] |= field_value0 << shift_left;
          new_mask[mem_id] |= field_value1 << shift_left;
          break;
        case TBL_PKG_TERN_FIELD_LOCATION_PAYLOAD:
          // Note that MR bit (in word0[0]) has been inverted, now '0' means
          // perform multi-range match, '1' means do not
          field_value0 = get_val((*tbl_word0)[field->memword_index[0]],
                                 field->lsbmemwordoffset,
                                 field->bitwidth);
          field_value1 = get_val((*tbl_word1)[field->memword_index[0]],
                                 field->lsbmemwordoffset,
                                 field->bitwidth);
          mrd[mem_id] = field_value0;
          payload[mem_id] = field_value1;
          new_key[mem_id] |= field_value0 << shift_left;
          new_mask[mem_id] |= field_value1 << shift_left;
          break;
        case TBL_PKG_TERN_FIELD_LOCATION_VERSION:
          decode_key_mask_no_encode(&version_key,
                                    &version_mask,
                                    0,
                                    2,
                                    (*tbl_word0)[field->memword_index[0]],
                                    (*tbl_word1)[field->memword_index[0]],
                                    field->lsbmemwordoffset,
                                    field->bitwidth);
          c_len_t +=
              snprintf(str_t + c_len_t,
                       (c_len_t < max_len_t) ? (max_len_t - c_len_t - 1) : 0,
                       "%02x\n%-9s | %-24s | %-10s | %02x\n",
                       version_key,
                       " ",
                       " ",
                       " ",
                       version_mask);
          new_key[mem_id] |= (uint64_t)version_key << shift_left;
          new_mask[mem_id] |= (uint64_t)version_mask << shift_left;
          break;
        case TBL_PKG_TERN_FIELD_LOCATION_RANGE:
          decode_range((*tbl_word0)[field->memword_index[0]],
                       (*tbl_word1)[field->memword_index[0]],
                       field->lsbmemwordoffset + field->range_nibble_offset,
                       field->bitwidth,
                       &range_bitvals,
                       &range_field0,
                       &range_field1,
                       field->range_type,
                       field->range_hi_byte ? 1 : 0);
          for (i = 0; i < sizeof(range_bitvals) * 8; i++) {
            if (range_bitvals & (1 << i)) {
              c_len_t += snprintf(
                  str_t + c_len_t,
                  (c_len_t < max_len_t) ? (max_len_t - c_len_t - 1) : 0,
                  "%x, ",
                  i);
            }
          }
          new_key[mem_id] |= (range_field0) << shift_left;
          new_mask[mem_id] |= (range_field1) << shift_left;

          c_len_t +=
              snprintf(str_t + c_len_t,
                       (c_len_t < max_len_t) ? (max_len_t - c_len_t - 1) : 0,
                       "\n");
          break;
        case TBL_PKG_TERN_FIELD_LOCATION_SPEC:
          key_ptr = (uint8_t *)&key[0];
          mask_ptr = (uint8_t *)&mask[0];
          decode_key_mask_no_encode(key_ptr,
                                    mask_ptr,
                                    0,
                                    field->bitwidth,
                                    (*tbl_word0)[field->memword_index[0]],
                                    (*tbl_word1)[field->memword_index[0]],
                                    field->lsbmemwordoffset,
                                    field->bitwidth);
          len_key = field->bitwidth / 8;
          if ((field->bitwidth % 8) > 0) len_key++;
          for (fl_len = 0; fl_len < len_key; fl_len++) {
            c_len_t +=
                snprintf(str_t + c_len_t,
                         (c_len_t < max_len_t) ? (max_len_t - c_len_t - 1) : 0,
                         "%02x ",
                         *key_ptr);
            new_key[mem_id] |= ((uint64_t)*key_ptr
                                << (shift_left + (len_key - fl_len - 1) * 8));
            key_ptr++;
          }
          c_len_t +=
              snprintf(str_t + c_len_t,
                       (c_len_t < max_len_t) ? (max_len_t - c_len_t - 1) : 0,
                       "\n%-9s | %-24s | %-10s | ",
                       " ",
                       " ",
                       " ");
          for (fl_len = 0; fl_len < len_key; fl_len++) {
            c_len_t +=
                snprintf(str_t + c_len_t,
                         (c_len_t < max_len_t) ? (max_len_t - c_len_t - 1) : 0,
                         "%02x ",
                         *mask_ptr);
            new_mask[mem_id] |= ((uint64_t)*mask_ptr
                                 << (shift_left + (len_key - fl_len - 1) * 8));
            mask_ptr++;
          }
          c_len_t +=
              snprintf(str_t + c_len_t,
                       (c_len_t < max_len_t) ? (max_len_t - c_len_t - 1) : 0,
                       "\n");
          break;
        default:
          break;
      }
      field++;
    }
    entry++;
  }
  if (c_len_t > 0) {
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "--------- | ------------------------ | ---------- | "
                      "-------------------\n\n");

    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "Tcam Match address: 0x%" PRIx64 " \nValue (43..0): ",
                      addr_arr[prev_mem_id]);
    /* Print the key/mask value without payload and parity */
    print_tcam_in_key_mask_fmt(new_key[prev_mem_id],
                               new_mask[prev_mem_id],
                               str,
                               &c_len,
                               max_len,
                               TBLPACK_TOF_BYTES_IN_TCAM_WORD * 8);

    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "\nMrd: 0x%02" PRIx64 ", payload 0x%02" PRIx64 " \n",
                      mrd[prev_mem_id],
                      payload[prev_mem_id]);
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "%s",
                      str_t);
  }
  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "--------- | ------------------------ | ---------- | "
                    "-------------------\n\n");

  *c_str_len = c_len;
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_tof_tern_tbl_conv_key_mask(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    uint8_t stage_id,
    uint8_t mem_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint64_t key,
    uint64_t mask,
    uint64_t *data0,
    uint64_t *data1,
    bool version) {
  uint64_t new_data0 = key, new_data1 = mask;
  uint64_t range_mask = 0;
  uint8_t word0[TBLPACK_TOF_BYTES_IN_TCAM_WORD],
      word1[TBLPACK_TOF_BYTES_IN_TCAM_WORD];
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_tern_range_mask_t *rangemask;
  uint32_t bj_hash, i, j;

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tern_lut_depth,
      mat_tbl_hdl,
      stage_id,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tern_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tern_lut,
      mat_tbl_hdl,
      stage_id);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for tern table handle "
        "0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  rangemask = lut_ptr->u.tern_ptr->rangeformat;
  for (i = 0; i < lut_ptr->u.tern_ptr->rangemaskcount; i++) {
    for (j = 0; j < TBL_PKG_MAX_DEPTH_TCAM_UNITS; j++) {
      if (rangemask->memids[j] == mem_id) {
        range_mask = rangemask->mask;
        break;
      }
    }
    rangemask++;
  }

  for (i = 0; i < TBLPACK_TOF_BYTES_IN_TCAM_WORD; i++) {
    word0[i] = new_data0 & 0xff;
    word1[i] = new_data1 & 0xff;
    new_data0 >>= 8;
    new_data1 >>= 8;
  }
  set_key_mask_2bit_mode(word0, word1, range_mask, version);

  new_data0 = 0;
  new_data1 = 0;
  for (i = 0; i < TBLPACK_TOF_BYTES_IN_TCAM_WORD; i++) {
    new_data0 |= (uint64_t)word0[i] << (8 * i);
    new_data1 |= (uint64_t)word1[i] << (8 * i);
  }

  /* payload/mrd is bit 0, parity is 45, 46 */
  *data0 = new_data0;
  *data1 = new_data1;

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_tof_phase0_tbl_get_match_port(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec,
    uint32_t *match_port) {
  uint32_t port = 0, bj_hash, i;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_phase0_match_field_t *field_ptr;

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_match_lut_depth,
      mat_tbl_hdl,
      0,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_match_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_match_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for phase0 match table "
        "handle "
        "0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  field_ptr = lut_ptr->u.phase0_match_ptr->matchfields;
  for (i = 0; i < lut_ptr->u.phase0_match_ptr->fieldcount; i++) {
    copy_bits_old((uint8_t *)&port,
                  0,
                  field_ptr->fieldwidth,
                  match_spec->match_value_bits,
                  field_ptr->startbit,
                  field_ptr->fieldwidth,
                  0);
    field_ptr++;
  }
  port = le32toh(port);
  *match_port = port;
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_tof_phase0_tbl_get_match_spec(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint32_t match_port,
    pipe_tbl_match_spec_t *match_spec) {
  uint32_t bj_hash, i;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_phase0_match_field_t *field_ptr;
  uint8_t *match_port_ptr = (uint8_t *)&match_port;

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_match_lut_depth,
      mat_tbl_hdl,
      0,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_match_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_match_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for phase0 match table "
        "handle "
        "0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  field_ptr = lut_ptr->u.phase0_match_ptr->matchfields;
  for (i = 0; i < lut_ptr->u.phase0_match_ptr->fieldcount; i++) {
    copy_bits_old_opp(match_spec->match_value_bits,
                      field_ptr->startbit,
                      field_ptr->fieldwidth,
                      0,
                      match_port_ptr,
                      0,
                      field_ptr->fieldwidth);

    field_ptr++;
  }
  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_entry_format_tof_phase0_tbl_addr_get(
    rmt_dev_info_t *dev_info,
    uint32_t port,
    bool is_write,
    pipe_register_spec_t *register_spec_list) {
  bf_dev_pipe_t log_pipe = dev_info->dev_cfg.dev_port_to_pipe(port);
  bf_dev_pipe_t phy_pipe;
  pipe_mgr_map_pipe_id_log_to_phy(dev_info, log_pipe, &phy_pipe);
  int local_port = dev_info->dev_cfg.dev_port_to_local_port(port) / 4;
  int port_chnl = dev_info->dev_cfg.dev_port_to_local_port(port) % 4;
  int first, second;
  /* Wide registers on Tofino must be written from high address to low address
   * and read from low address to high address.  We will hide this fact from
   * the high layers by ordering the list of register specs based on the
   * is_write flag so the callers can simply go through the array the same way
   * regardless of the operation. */
  if (is_write) {
    first = 1;
    second = 0;
  } else {
    first = 0;
    second = 1;
  }

  switch (port_chnl) {
    case 0:
      register_spec_list[first].reg_addr =
          offsetof(Tofino,
                   pipes[phy_pipe]
                       .pmarb.ibp18_reg.ibp_reg[local_port]
                       .ing_buf_regs.chan0_group.chnl_metadata_fix
                       .chnl_metadata_fix_0_2);
      register_spec_list[second].reg_addr =
          offsetof(Tofino,
                   pipes[phy_pipe]
                       .pmarb.ibp18_reg.ibp_reg[local_port]
                       .ing_buf_regs.chan0_group.chnl_metadata_fix
                       .chnl_metadata_fix_1_2);
      break;
    case 1:
      register_spec_list[first].reg_addr =
          offsetof(Tofino,
                   pipes[phy_pipe]
                       .pmarb.ibp18_reg.ibp_reg[local_port]
                       .ing_buf_regs.chan1_group.chnl_metadata_fix
                       .chnl_metadata_fix_0_2);
      register_spec_list[second].reg_addr =
          offsetof(Tofino,
                   pipes[phy_pipe]
                       .pmarb.ibp18_reg.ibp_reg[local_port]
                       .ing_buf_regs.chan1_group.chnl_metadata_fix
                       .chnl_metadata_fix_1_2);
      break;
    case 2:
      register_spec_list[first].reg_addr =
          offsetof(Tofino,
                   pipes[phy_pipe]
                       .pmarb.ibp18_reg.ibp_reg[local_port]
                       .ing_buf_regs.chan2_group.chnl_metadata_fix
                       .chnl_metadata_fix_0_2);
      register_spec_list[second].reg_addr =
          offsetof(Tofino,
                   pipes[phy_pipe]
                       .pmarb.ibp18_reg.ibp_reg[local_port]
                       .ing_buf_regs.chan2_group.chnl_metadata_fix
                       .chnl_metadata_fix_1_2);
      break;
    case 3:
      register_spec_list[first].reg_addr =
          offsetof(Tofino,
                   pipes[phy_pipe]
                       .pmarb.ibp18_reg.ibp_reg[local_port]
                       .ing_buf_regs.chan3_group.chnl_metadata_fix
                       .chnl_metadata_fix_0_2);
      register_spec_list[second].reg_addr =
          offsetof(Tofino,
                   pipes[phy_pipe]
                       .pmarb.ibp18_reg.ibp_reg[local_port]
                       .ing_buf_regs.chan3_group.chnl_metadata_fix
                       .chnl_metadata_fix_1_2);
      break;
    default:
      break;
  }

  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_entry_format_tof2_phase0_tbl_addr_get(
    rmt_dev_info_t *dev_info,
    uint32_t port,
    pipe_register_spec_t *register_spec_list) {
  bf_dev_pipe_t pipe = dev_info->dev_cfg.dev_port_to_pipe(port);
  bf_dev_pipe_t phy_pipe;
  pipe_status_t sts =
      pipe_mgr_map_pipe_id_log_to_phy(dev_info, pipe, &phy_pipe);
  if (sts != PIPE_SUCCESS) return sts;
  int local_port = dev_info->dev_cfg.dev_port_to_local_port(port);
  int ipb = local_port / TOF2_NUM_CHN_PER_IPB;
  int chan = local_port % TOF2_NUM_CHN_PER_IPB;
  size_t ipb_step =
      offsetof(tof2_reg, pipes[0].pardereg.pgstnreg.ipbprsr4reg[1]) -
      offsetof(tof2_reg, pipes[0].pardereg.pgstnreg.ipbprsr4reg[0]);
  size_t chan_step =
      offsetof(tof2_reg,
               pipes[0].pardereg.pgstnreg.ipbprsr4reg[0].ipbreg.chan1_group) -
      offsetof(tof2_reg,
               pipes[0].pardereg.pgstnreg.ipbprsr4reg[0].ipbreg.chan0_group);
  size_t base = offsetof(tof2_reg,
                         pipes[phy_pipe]
                             .pardereg.pgstnreg.ipbprsr4reg[0]
                             .ipbreg.chan0_group.chnl_meta);

  for (unsigned int i = 0; i < dev_info->dev_cfg.p0_width; ++i) {
    register_spec_list[i].reg_addr = ipb * ipb_step + chan * chan_step + base;
    register_spec_list[i].reg_addr += i * 4;
  }

  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_entry_format_tof3_phase0_tbl_addr_get(
    rmt_dev_info_t *dev_info,
    uint32_t port,
    pipe_memory_spec_t *memory_spec_list,
    bf_subdev_id_t *subdev) {
  bf_dev_pipe_t pipe = dev_info->dev_cfg.dev_port_to_pipe(port);
  bf_dev_pipe_t phy_pipe;
  pipe_status_t sts =
      pipe_mgr_map_pipe_id_log_to_phy(dev_info, pipe, &phy_pipe);
  if (sts != PIPE_SUCCESS) return sts;
  *subdev = pipe_mgr_subdev_id_from_pipe(phy_pipe);

  int local_port = dev_info->dev_cfg.dev_port_to_local_port(port);

  uint64_t base =
      tof3_mem_pipes_parde_i_prsr_mem_ipb_mem_meta_phase0_16byte_entry_address;
  uint64_t step =
      tof3_mem_pipes_parde_i_prsr_mem_ipb_mem_meta_phase0_16byte_entry_array_element_size;
  uint64_t limit =
      tof3_mem_pipes_parde_i_prsr_mem_ipb_mem_meta_phase0_16byte_entry_array_index_max;
  PIPE_MGR_ASSERT((uint64_t)local_port <= limit);
  uint64_t addr = (base + (phy_pipe * tof3_mem_pipes_array_element_size) +
                   local_port * step) >>
                  4;
  memory_spec_list->mem_addr0 = addr;

  int ipb_num = local_port / TOF3_NUM_CHN_PER_IPB;
  int ipb_local_port =
      TOF3_NUM_PORTS_PER_PIPE + (local_port % TOF3_NUM_CHN_PER_IPB);
  uint64_t prsr_step = tof3_mem_pipes_parde_i_prsr_mem_array_element_size;
  PIPE_MGR_ASSERT((uint64_t)ipb_local_port <= limit);
  /* There are 4 parser instances per ipb */
  addr = (base + (phy_pipe * tof3_mem_pipes_array_element_size) +
          (ipb_num * 4 * prsr_step) + (ipb_local_port * step)) >>
         4;
  memory_spec_list->mem_addr1 = addr;

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_phase0_tbl_addr_get(
    rmt_dev_info_t *dev_info,
    uint32_t port,
    bool is_write,
    bf_subdev_id_t *subdev,
    pipe_register_spec_t *register_spec_list,
    pipe_memory_spec_t *memory_spec_list) {
  *subdev = 0;
  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      return pipe_mgr_entry_format_tof_phase0_tbl_addr_get(
          dev_info, port, is_write, register_spec_list);
    case BF_DEV_FAMILY_TOFINO2:
      return pipe_mgr_entry_format_tof2_phase0_tbl_addr_get(
          dev_info, port, register_spec_list);
    case BF_DEV_FAMILY_TOFINO3:
      return pipe_mgr_entry_format_tof3_phase0_tbl_addr_get(
          dev_info, port, memory_spec_list, subdev);
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }
}

static pipe_status_t pipe_mgr_entry_format_tof_phase0_tbl_update(
    rmt_dev_info_t *dev_info,
    pipemgr_tbl_pkg_lut_t *lut_ptr,
    uint32_t match_port,
    pipe_action_data_spec_t *act_data_spec,
    pipe_register_spec_t *register_spec_list) {
  uint32_t reg_val[dev_info->dev_cfg.p0_width];
  uint64_t reg_val_full = 0;
  uint32_t i, j;
  pipemgr_tbl_pkg_phase0_action_field_t *field_ptr;
  pipemgr_tbl_pkg_phase0_action_entry_t *entry_ptr;

  entry_ptr = lut_ptr->u.phase0_action_ptr->entry;
  for (i = 0; i < lut_ptr->u.phase0_action_ptr->totalentries; i++) {
    field_ptr = entry_ptr->actionfields;
    for (j = 0; j < entry_ptr->fieldcount; j++) {
      if (field_ptr->location == TBL_PKG_PHASE0_FIELD_LOCATION_CONSTANT) {
        set_val_old((uint8_t *)&reg_val_full,
                    field_ptr->lsbmemwordoffset,
                    field_ptr->fieldwidth,
                    field_ptr->constant_value);
      } else if (field_ptr->location == TBL_PKG_PHASE0_FIELD_LOCATION_SPEC) {
        copy_bits_old((uint8_t *)&reg_val_full,
                      field_ptr->lsbmemwordoffset,
                      field_ptr->fieldwidth,
                      act_data_spec->action_data_bits,
                      field_ptr->param_start,
                      field_ptr->param_width,
                      field_ptr->param_shift);
      }
      field_ptr++;
    }
    entry_ptr = (pipemgr_tbl_pkg_phase0_action_entry_t *)field_ptr;
  }

  /* split the 64 bit value to two 32 bit values */
  reg_val_full = le64toh(reg_val_full);
  reg_val[0] = reg_val_full & 0xffffffff;
  reg_val[1] = (reg_val_full >> 32) & 0xffffffff;

  /* Get the register addresses. */
  pipe_mgr_entry_format_tof_phase0_tbl_addr_get(
      dev_info, match_port, true, register_spec_list);

  /* Populate the register data.  Note that we are putting the low half in index
   * one and the high half in index zero because Tofino requires wide registers
   * to be written from high address to low address and we expect the caller to
   * iterate through this array starting at zero. */
  setp_chnl_metadata_fix_chnl_metadata_fix_0_2_chnl_meta_fix_31_0(
      &register_spec_list[1].reg_data, reg_val[0]);
  setp_chnl_metadata_fix_chnl_metadata_fix_1_2_chnl_meta_fix_63_32(
      &register_spec_list[0].reg_data, reg_val[1]);

  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_entry_format_tof2_phase0_tbl_update(
    rmt_dev_info_t *dev_info,
    pipemgr_tbl_pkg_lut_t *lut_ptr,
    uint32_t match_port,
    pipe_action_data_spec_t *act_data_spec,
    pipe_register_spec_t *register_spec_list) {
  uint8_t reg_val_full[dev_info->dev_cfg.p0_width * sizeof(uint32_t)];
  PIPE_MGR_MEMSET(
      reg_val_full, 0, dev_info->dev_cfg.p0_width * sizeof(uint32_t));
  pipemgr_tbl_pkg_phase0_action_field_t *field_ptr;
  pipemgr_tbl_pkg_phase0_action_entry_t *entry_ptr;

  entry_ptr = lut_ptr->u.phase0_action_ptr->entry;
  for (int i = 0; i < (int)lut_ptr->u.phase0_action_ptr->totalentries; i++) {
    field_ptr = entry_ptr->actionfields;
    for (int j = 0; j < (int)entry_ptr->fieldcount; j++) {
      if (field_ptr->location == TBL_PKG_PHASE0_FIELD_LOCATION_CONSTANT) {
        set_val_old(reg_val_full,
                    field_ptr->lsbmemwordoffset,
                    field_ptr->fieldwidth,
                    field_ptr->constant_value);
      } else if (field_ptr->location == TBL_PKG_PHASE0_FIELD_LOCATION_SPEC) {
        copy_bits_old(reg_val_full,
                      field_ptr->lsbmemwordoffset,
                      field_ptr->fieldwidth,
                      act_data_spec->action_data_bits,
                      field_ptr->param_start,
                      field_ptr->param_width,
                      field_ptr->param_shift);
      }
      field_ptr++;
    }
    entry_ptr = (pipemgr_tbl_pkg_phase0_action_entry_t *)field_ptr;
  }

  /* Get the register addresses. */
  pipe_mgr_entry_format_tof2_phase0_tbl_addr_get(
      dev_info, match_port, register_spec_list);

  for (unsigned int i = 0; i < dev_info->dev_cfg.p0_width; ++i) {
    register_spec_list[i].reg_data = ((uint32_t *)reg_val_full)[i];
  }

  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_entry_format_tof3_phase0_tbl_update(
    rmt_dev_info_t *dev_info,
    pipemgr_tbl_pkg_lut_t *lut_ptr,
    uint32_t match_port,
    pipe_action_data_spec_t *act_data_spec,
    pipe_register_spec_t *register_spec_list,
    pipe_memory_spec_t *memory_spec_list) {
  uint8_t reg_val_full[dev_info->dev_cfg.p0_width * sizeof(uint32_t)];
  PIPE_MGR_MEMSET(
      reg_val_full, 0, dev_info->dev_cfg.p0_width * sizeof(uint32_t));
  pipemgr_tbl_pkg_phase0_action_field_t *field_ptr;
  pipemgr_tbl_pkg_phase0_action_entry_t *entry_ptr;

  entry_ptr = lut_ptr->u.phase0_action_ptr->entry;
  for (int i = 0; i < (int)lut_ptr->u.phase0_action_ptr->totalentries; i++) {
    field_ptr = entry_ptr->actionfields;
    for (int j = 0; j < (int)entry_ptr->fieldcount; j++) {
      if (field_ptr->location == TBL_PKG_PHASE0_FIELD_LOCATION_CONSTANT) {
        set_val_old(reg_val_full,
                    field_ptr->lsbmemwordoffset,
                    field_ptr->fieldwidth,
                    field_ptr->constant_value);
      } else if (field_ptr->location == TBL_PKG_PHASE0_FIELD_LOCATION_SPEC) {
        copy_bits_old(reg_val_full,
                      field_ptr->lsbmemwordoffset,
                      field_ptr->fieldwidth,
                      act_data_spec->action_data_bits,
                      field_ptr->param_start,
                      field_ptr->param_width,
                      field_ptr->param_shift);
      }
      field_ptr++;
    }
    entry_ptr = (pipemgr_tbl_pkg_phase0_action_entry_t *)field_ptr;
  }
  /* Get the memory addresses. */
  bf_subdev_id_t subdev;
  pipe_mgr_entry_format_tof3_phase0_tbl_addr_get(
      dev_info, match_port, memory_spec_list, &subdev);

  for (unsigned int i = 0; i < dev_info->dev_cfg.p0_width; ++i) {
    register_spec_list[i].reg_data = ((uint32_t *)reg_val_full)[i];
  }

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_phase0_tbl_update(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint32_t match_port,
    pipe_action_data_spec_t *act_data_spec,
    pipe_register_spec_t *register_spec_list,
    pipe_memory_spec_t *memory_spec_list) {
  bf_dev_id_t devid = dev_info->dev_id;
  uint32_t bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_action_lut_depth,
      mat_tbl_hdl,
      0,
      0);
  pipemgr_tbl_pkg_lut_t *lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_action_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_action_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for phase0 match table "
        "handle "
        "0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      return pipe_mgr_entry_format_tof_phase0_tbl_update(
          dev_info, lut_ptr, match_port, act_data_spec, register_spec_list);
    case BF_DEV_FAMILY_TOFINO2:
      return pipe_mgr_entry_format_tof2_phase0_tbl_update(
          dev_info, lut_ptr, match_port, act_data_spec, register_spec_list);
    case BF_DEV_FAMILY_TOFINO3:
      return pipe_mgr_entry_format_tof3_phase0_tbl_update(dev_info,
                                                          lut_ptr,
                                                          match_port,
                                                          act_data_spec,
                                                          register_spec_list,
                                                          memory_spec_list);
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }
}

static pipe_status_t
pipe_mgr_entry_format_tof_phase0_tbl_decode_reg_to_action_data(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_register_spec_t *register_spec_list,
    pipe_action_data_spec_t *act_data_spec) {
  if (!dev_info) return PIPE_INVALID_ARG;
  if (!register_spec_list) return PIPE_INVALID_ARG;
  if (!act_data_spec) return PIPE_INVALID_ARG;
  bf_dev_id_t devid = dev_info->dev_id;
  uint32_t bj_hash, i, j;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_phase0_action_field_t *field_ptr;
  pipemgr_tbl_pkg_phase0_action_entry_t *entry_ptr;
  uint64_t reg_val_full[dev_info->dev_cfg.p0_width / 2];
  uint8_t *reg_ptr = (uint8_t *)reg_val_full;

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_action_lut_depth,
      mat_tbl_hdl,
      0,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_action_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_action_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for phase0 match table "
        "handle "
        "0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  for (i = 0; i < dev_info->dev_cfg.p0_width; i += 2) {
    reg_val_full[i / 2] = register_spec_list[i + 1].reg_data;
    reg_val_full[i / 2] = reg_val_full[i / 2] << 32;
    reg_val_full[i / 2] = reg_val_full[i / 2] | register_spec_list[i].reg_data;
  }

  entry_ptr = lut_ptr->u.phase0_action_ptr->entry;
  for (i = 0; i < lut_ptr->u.phase0_action_ptr->totalentries; i++) {
    field_ptr = entry_ptr->actionfields;
    for (j = 0; j < entry_ptr->fieldcount; j++) {
      if (field_ptr->location == TBL_PKG_PHASE0_FIELD_LOCATION_CONSTANT) {
        // Do nothing for constant values
      } else if (field_ptr->location == TBL_PKG_PHASE0_FIELD_LOCATION_SPEC) {
        uint16_t start_offset =
            (field_ptr->param_start + field_ptr->param_width - 1) - 7;
        uint8_t byte_offset = field_ptr->param_shift / 8;
        uint8_t bit_offset = field_ptr->param_shift % 8;
        start_offset -= (byte_offset * 8);
        start_offset += bit_offset;
        decode_bits_to_spec(act_data_spec->action_data_bits,
                            start_offset,
                            field_ptr->param_width,
                            &reg_ptr,
                            0,
                            field_ptr->lsbmemwordoffset,
                            field_ptr->fieldwidth);
      }
      field_ptr++;
    }
    entry_ptr = (pipemgr_tbl_pkg_phase0_action_entry_t *)field_ptr;
  }

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_phase0_tbl_decode_reg_to_action_data(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_register_spec_t *register_spec_list,
    pipe_action_data_spec_t *act_data_spec) {
  return pipe_mgr_entry_format_tof_phase0_tbl_decode_reg_to_action_data(
      dev_info, prof_id, mat_tbl_hdl, register_spec_list, act_data_spec);
}

pipe_status_t pipe_mgr_entry_format_tof_phase0_tbl_print(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_register_spec_t *reg_spec_list,
    char *str,
    int *c_str_len,
    int max_len) {
  bf_dev_id_t devid = dev_info->dev_id;
  int c_len = *c_str_len;
  int mem_idx = 0;
  uint64_t field_value = 0;
  uint8_t mem_id_vals[dev_info->dev_cfg.p0_width];
  uint8_t reg_val_full[dev_info->dev_cfg.p0_width * sizeof(uint32_t)];
  uint32_t bj_hash, i, j;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_phase0_action_field_t *field_ptr;
  pipemgr_tbl_pkg_phase0_action_entry_t *entry_ptr;

  for (i = 0; i < dev_info->dev_cfg.p0_width; ++i) {
    mem_id_vals[i] = 0;
    ((uint32_t *)reg_val_full)[i] = reg_spec_list[i].reg_data;
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_action_lut_depth,
      mat_tbl_hdl,
      0,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_action_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).phase0_action_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for phase0 match table "
        "handle "
        "0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  entry_ptr = lut_ptr->u.phase0_action_ptr->entry;
  for (i = 0; i < lut_ptr->u.phase0_action_ptr->totalentries; i++) {
    field_ptr = entry_ptr->actionfields;
    for (j = 0; j < entry_ptr->fieldcount; j++) {
      mem_idx = field_ptr->memword_index[0];
      if (mem_id_vals[mem_idx] == 0) {
        mem_id_vals[mem_idx] = 1;
        c_len += snprintf(
            str + c_len,
            (c_len < max_len) ? (max_len - c_len - 1) : 0,
            "--------- ------------------------------------------------\n");
        c_len += snprintf(str + c_len,
                          (c_len < max_len) ? (max_len - c_len - 1) : 0,
                          "---------REG %d, Addr 0x%x, Value 0x%x----------\n",
                          mem_idx,
                          reg_spec_list[mem_idx].reg_addr,
                          reg_spec_list[mem_idx].reg_data);
        c_len += snprintf(
            str + c_len,
            (c_len < max_len) ? (max_len - c_len - 1) : 0,
            "--------- | ------------------------ | -------------------\n");
        c_len += snprintf(str + c_len,
                          (c_len < max_len) ? (max_len - c_len - 1) : 0,
                          "%-9s | %-24s | %-20s \n",
                          "Bit-field",
                          "Field-Name",
                          "Value");
        c_len += snprintf(
            str + c_len,
            (c_len < max_len) ? (max_len - c_len - 1) : 0,
            "--------- | ------------------------ | -------------------\n");
      }
      field_value = get_val(
          reg_val_full, field_ptr->lsbmemwordoffset, field_ptr->fieldwidth);
      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        "%-3d:%-3d   | %-24s | ",
                        field_ptr->lsbmemwordoffset + field_ptr->fieldwidth - 1,
                        field_ptr->lsbmemwordoffset,
                        pipemgr_tbl_pkg_get_field_string_name(
                            devid, prof_id, field_ptr->fieldname_str_index));
      print_field_value_in_bytes_fmt(
          field_value, field_ptr->fieldwidth, true, str, &c_len, max_len);
      field_ptr++;
    }
    entry_ptr = (pipemgr_tbl_pkg_phase0_action_entry_t *)field_ptr;
  }
  c_len +=
      snprintf(str + c_len,
               (c_len < max_len) ? (max_len - c_len - 1) : 0,
               "--------- | ------------------------ | -------------------\n");
  *c_str_len = c_len;

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_print_match_spec(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec,
    char *dest_str,
    size_t dest_str_size,
    size_t *bytes_written) {
  char *end;
  char *ptr;
  uint32_t bj_hash, i;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_spec_field_t *field_ptr;
  pipemgr_tbl_pkg_spec_t *matchspec_ptr;
  pipe_mat_tbl_info_t *exm_tbl_info = NULL;
  uint8_t *match_mask_bits;

  *bytes_written = 0;

  if (!dest_str) {
    return PIPE_SUCCESS;
  }
  end = dest_str + dest_str_size;
  ptr = dest_str;

  PIPE_MGR_MEMSET(ptr, 0, end - ptr);

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).matchspec_lut_depth,
      mat_tbl_hdl,
      0,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).matchspec_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).matchspec_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find match spec details for match table "
        "handle "
        "0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  bool is_dkm_exm_tbl = pipe_mgr_is_dkm_table(devid, mat_tbl_hdl);
  if (is_dkm_exm_tbl) {
    exm_tbl_info =
        pipe_mgr_get_tbl_info(devid, mat_tbl_hdl, __func__, __LINE__);
    if (exm_tbl_info == NULL) {
      LOG_ERROR("%s:%d Unable to find exm table 0x%x device %d",
                __func__,
                __LINE__,
                mat_tbl_hdl,
                devid);
      return PIPE_INVALID_ARG;
    }
    if (exm_tbl_info->match_key_mask_width) {
      // Dynamic key mask applied...
      match_mask_bits = exm_tbl_info->match_key_mask;
    } else {
      // No Dynamic key mask applied...
      // Power on default value is being used.
      match_mask_bits = match_spec->match_mask_bits;
    }
  } else {
    match_mask_bits = match_spec->match_mask_bits;
  }

  matchspec_ptr = lut_ptr->u.matchspec_ptr;
  field_ptr = matchspec_ptr->fields;
  for (i = 0; i < matchspec_ptr->fieldcount; i++) {
    ptr += snprintf(ptr,
                    end > ptr ? (end - ptr) : 0,
                    "\t%s :\n",
                    pipemgr_tbl_pkg_get_field_string_name(
                        devid, prof_id, field_ptr->fieldname_str_index));
    ptr += snprintf(ptr, end > ptr ? (end - ptr) : 0, "\t\tValue: ");
    ptr +=
        print_bytes(&match_spec->match_value_bits[(field_ptr->startbit >> 3)],
                    (field_ptr->fieldwidth + 7) >> 3,
                    "\t\t\t",
                    ptr,
                    end > ptr ? (end - ptr) : 0);
    ptr += snprintf(ptr, end > ptr ? (end - ptr) : 0, "\n\t\tMask:  ");
    ptr += print_bytes(&match_mask_bits[(field_ptr->startbit >> 3)],
                       (field_ptr->fieldwidth + 7) >> 3,
                       "\t\t\t",
                       ptr,
                       end > ptr ? (end - ptr) : 0);
    ptr += snprintf(ptr, end > ptr ? (end - ptr) : 0, "\n");
    field_ptr++;
  }

  if (ptr >= end) {
    *(end - 1) = 0;
    *(end - 2) = '.';
    *(end - 3) = '.';
    *(end - 4) = '.';
  }

  *bytes_written = ((ptr > end) ? end : ptr) - dest_str;
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_print_action_spec(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_action_data_spec_t *action_spec,
    pipe_act_fn_hdl_t act_fn_hdl,
    char *dest_str,
    size_t dest_str_size,
    size_t *bytes_written) {
  char *end;
  char *ptr;
  uint32_t bj_hash, i;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_spec_field_t *field_ptr;
  pipemgr_tbl_pkg_action_spec_t *actionspec_ptr;

  *bytes_written = 0;

  if (!dest_str) {
    return PIPE_SUCCESS;
  }

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  if (dev_info->fake_rmt_cfg) {
    return PIPE_SUCCESS;
  }

  end = dest_str + dest_str_size;
  ptr = dest_str;

  PIPE_MGR_MEMSET(ptr, 0, end - ptr);

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).actionspec_lut_depth,
      act_fn_hdl,
      0,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).actionspec_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).actionspec_lut,
      act_fn_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find action spec details for action function "
        "handle "
        "0x%x",
        __func__,
        act_fn_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  actionspec_ptr = lut_ptr->u.actionspec_ptr;
  field_ptr = actionspec_ptr->fields;
  ptr +=
      snprintf(ptr,
               end > ptr ? (end - ptr) : 0,
               "\tAction function :%s (%d)\n",
               pipemgr_tbl_pkg_get_field_string_name(
                   devid, prof_id, actionspec_ptr->actionfunc_name_str_index),
               act_fn_hdl);
  for (i = 0; i < actionspec_ptr->fieldcount; i++) {
    ptr += snprintf(ptr,
                    end > ptr ? (end - ptr) : 0,
                    "\t%s :\n",
                    pipemgr_tbl_pkg_get_field_string_name(
                        devid, prof_id, field_ptr->fieldname_str_index));
    ptr += snprintf(ptr, end > ptr ? (end - ptr) : 0, "\t\tValue: ");
    ptr +=
        print_bytes(&action_spec->action_data_bits[(field_ptr->startbit >> 3)],
                    (field_ptr->fieldwidth + 7) >> 3,
                    "\t\t\t",
                    ptr,
                    end > ptr ? (end - ptr) : 0);
    ptr += snprintf(ptr, end > ptr ? (end - ptr) : 0, "\n");
    field_ptr++;
  }

  if (ptr >= end) {
    *(end - 1) = 0;
    *(end - 2) = '.';
    *(end - 3) = '.';
    *(end - 4) = '.';
  }

  *bytes_written = ((ptr > end) ? end : ptr) - dest_str;
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_log_match_spec(
    bf_dev_id_t devid,
    int log_lvl,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec) {
  if (bf_sys_log_is_log_enabled(BF_MOD_PIPE, log_lvl) != 1) return PIPE_SUCCESS;
  if (!match_spec) return PIPE_INVALID_ARG;
  char buf[40];
  char format_str[1024];  // A single field of match spec can be atmost 128bytes
                          // ??
  uint32_t bj_hash, total_bytes;
  int i, j, c, spec_bytes;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_spec_field_t *field_ptr;
  pipemgr_tbl_pkg_spec_t *matchspec_ptr;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  if (dev_info->fake_rmt_cfg) {
    return PIPE_SUCCESS;
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).matchspec_lut_depth,
      mat_tbl_hdl,
      0,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).matchspec_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).matchspec_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find match spec details for match table handle 0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  matchspec_ptr = lut_ptr->u.matchspec_ptr;
  field_ptr = matchspec_ptr->fields;
  if (match_spec->priority)
    bf_sys_log_and_trace(
        BF_MOD_PIPE, log_lvl, "\tPriority : %d", match_spec->priority);
  if (match_spec->partition_index)
    bf_sys_log_and_trace(
        BF_MOD_PIPE, log_lvl, "\tPIndex : %d", match_spec->partition_index);
  for (i = 0; i < matchspec_ptr->fieldcount; i++) {
    bf_sys_log_and_trace(BF_MOD_PIPE,
                         log_lvl,
                         "\t%s :",
                         pipemgr_tbl_pkg_get_field_string_name(
                             devid, prof_id, field_ptr->fieldname_str_index));

    spec_bytes = (field_ptr->fieldwidth + 7) / 8;
    total_bytes = 0;
    format_str[0] = '\0';
    for (j = 0; j < spec_bytes && total_bytes < sizeof(format_str); j++) {
      c = snprintf(buf,
                   sizeof(buf),
                   "%02x ",
                   match_spec->match_value_bits[field_ptr->startbit / 8 + j]);
      strncat(format_str + total_bytes,
              buf,
              ((sizeof(format_str) - total_bytes) > 0) ? c : 0);
      total_bytes += c;
    }
    format_str[(total_bytes >= sizeof(format_str)) ? sizeof(format_str) - 1
                                                   : total_bytes] = '\0';
    bf_sys_log_and_trace(BF_MOD_PIPE, log_lvl, "\t\tValue: %s", format_str);

    total_bytes = 0;
    format_str[0] = '\0';
    for (j = 0; j < spec_bytes && total_bytes < sizeof(format_str); j++) {
      c = snprintf(buf,
                   sizeof(buf),
                   "%02x ",
                   match_spec->match_mask_bits[field_ptr->startbit / 8 + j]);
      strncat(format_str + total_bytes,
              buf,
              ((sizeof(format_str) - total_bytes) > 0) ? c : 0);
      total_bytes += c;
    }
    format_str[(total_bytes >= sizeof(format_str)) ? sizeof(format_str) - 1
                                                   : total_bytes] = '\0';
    bf_sys_log_and_trace(BF_MOD_PIPE, log_lvl, "\t\tMask:  %s", format_str);

    field_ptr++;
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_log_action_spec(
    bf_dev_id_t devid,
    int log_lvl,
    profile_id_t prof_id,
    pipe_action_spec_t *action_spec,
    pipe_act_fn_hdl_t act_fn_hdl) {
  if (bf_sys_log_is_log_enabled(BF_MOD_PIPE, log_lvl) != 1) return PIPE_SUCCESS;
  if (!action_spec) return PIPE_INVALID_ARG;

  int resource_count = action_spec->resource_count;
  pipe_res_spec_t *resources = action_spec->resources;
  if (IS_ACTION_SPEC_ACT_DATA(action_spec)) {
    pipemgr_tbl_pkg_lut_t *lut_ptr;
    pipemgr_tbl_pkg_spec_field_t *field_ptr;
    pipemgr_tbl_pkg_action_spec_t *actionspec_ptr;
    uint32_t bj_hash = bob_jenkin_hash_one_at_a_time(
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).actionspec_lut_depth,
        act_fn_hdl,
        0,
        0);
    lut_ptr = pipemgr_entry_format_get_lut_entry(
        bj_hash,
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).actionspec_lut_depth,
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).actionspec_lut,
        act_fn_hdl,
        0);
    if (!lut_ptr) {
      LOG_ERROR(
          "%s: Unable to find action spec LUT for action function handle 0x%x",
          __func__,
          act_fn_hdl);
      return PIPE_INVALID_ARG;
    }

    actionspec_ptr = lut_ptr->u.actionspec_ptr;
    if (!actionspec_ptr) {
      LOG_ERROR(
          "%s: Unable to find action spec details for action function handle "
          "0x%x",
          __func__,
          act_fn_hdl);
      return PIPE_INVALID_ARG;
    }
    field_ptr = actionspec_ptr->fields;
    bf_sys_log_and_trace(
        BF_MOD_PIPE,
        log_lvl,
        "\tAction function :%s",
        pipemgr_tbl_pkg_get_field_string_name(
            devid, prof_id, actionspec_ptr->actionfunc_name_str_index));
    for (int i = 0; i < actionspec_ptr->fieldcount; i++) {
      bf_sys_log_and_trace(BF_MOD_PIPE,
                           log_lvl,
                           "\t%s :",
                           pipemgr_tbl_pkg_get_field_string_name(
                               devid, prof_id, field_ptr->fieldname_str_index));
      int spec_bytes = (field_ptr->fieldwidth + 7) / 8;
      uint32_t total_bytes = 0;
      /* A single field of action spec can be atmost 128bytes */
      char format_str[1024];
      format_str[0] = '\0';
      for (int j = 0; j < spec_bytes && total_bytes < sizeof(format_str); j++) {
        char buf[40];
        int c = snprintf(buf,
                         sizeof(buf),
                         "%02x ",
                         action_spec->act_data
                             .action_data_bits[field_ptr->startbit / 8 + j]);
        strncat(format_str + total_bytes,
                buf,
                ((sizeof(format_str) - total_bytes) > 0) ? c : 0);
        total_bytes += c;
      }
      format_str[(total_bytes >= sizeof(format_str)) ? sizeof(format_str) - 1
                                                     : total_bytes] = '\0';
      bf_sys_log_and_trace(BF_MOD_PIPE, log_lvl, "\t\tValue: %s", format_str);

      field_ptr++;
    }
  } else if (IS_ACTION_SPEC_ACT_DATA_HDL(action_spec)) {
    bf_sys_log_and_trace(BF_MOD_PIPE,
                         log_lvl,
                         "\tAction Profile Member Handle: %d",
                         action_spec->adt_ent_hdl);
  } else if (IS_ACTION_SPEC_SEL_GRP(action_spec)) {
    bf_sys_log_and_trace(BF_MOD_PIPE,
                         log_lvl,
                         "\tAction Selector Group Handle: %d",
                         action_spec->sel_grp_hdl);
  }

  for (int i = 0; i < resource_count; ++i) {
    switch (PIPE_GET_HDL_TYPE(resources[i].tbl_hdl)) {
      case PIPE_HDL_TYPE_STFUL_TBL: {
        pipe_stful_tbl_info_t *rtbl = pipe_mgr_get_stful_tbl_info(
            devid, resources[i].tbl_hdl, __func__, __LINE__);
        if (!rtbl) continue;
        if (resources[i].tag == PIPE_RES_ACTION_TAG_ATTACHED) {
          if (rtbl->ref_type == PIPE_TBL_REF_TYPE_INDIRECT) {
            bf_sys_log_and_trace(BF_MOD_PIPE,
                                 log_lvl,
                                 "\tRegister Table: %s (ATTACH)",
                                 rtbl->name);
            bf_sys_log_and_trace(BF_MOD_PIPE,
                                 log_lvl,
                                 "\t\tRegister Index: %d",
                                 resources[i].tbl_idx);
          } else if (rtbl->ref_type == PIPE_TBL_REF_TYPE_DIRECT) {
            bf_sys_log_and_trace(
                BF_MOD_PIPE, log_lvl, "\tRegister Table: %s", rtbl->name);
            switch (rtbl->width) {
              case 1:
                bf_sys_log_and_trace(BF_MOD_PIPE,
                                     log_lvl,
                                     "\t\tRegister Value: %d",
                                     resources[i].data.stful.bit);
                break;
              case 8:
                if (rtbl->dbl_width)
                  bf_sys_log_and_trace(BF_MOD_PIPE,
                                       log_lvl,
                                       "\t\tRegister Value: 0x%x 0x%x",
                                       resources[i].data.stful.dbl_byte.hi,
                                       resources[i].data.stful.dbl_byte.lo);
                else
                  bf_sys_log_and_trace(BF_MOD_PIPE,
                                       log_lvl,
                                       "\t\tRegister Value: 0x%x",
                                       resources[i].data.stful.byte);
                break;
              case 16:
                if (rtbl->dbl_width)
                  bf_sys_log_and_trace(BF_MOD_PIPE,
                                       log_lvl,
                                       "\t\tRegister Value: 0x%x 0x%x",
                                       resources[i].data.stful.dbl_half.hi,
                                       resources[i].data.stful.dbl_half.lo);
                else
                  bf_sys_log_and_trace(BF_MOD_PIPE,
                                       log_lvl,
                                       "\t\tRegister Value: 0x%x",
                                       resources[i].data.stful.half);
                break;
              case 32:
                if (rtbl->dbl_width)
                  bf_sys_log_and_trace(BF_MOD_PIPE,
                                       log_lvl,
                                       "\t\tRegister Value: 0x%x 0x%x",
                                       resources[i].data.stful.dbl_word.hi,
                                       resources[i].data.stful.dbl_word.lo);
                else
                  bf_sys_log_and_trace(BF_MOD_PIPE,
                                       log_lvl,
                                       "\t\tRegister Value: 0x%x",
                                       resources[i].data.stful.word);
                break;
              case 64:
                if (rtbl->dbl_width)
                  bf_sys_log_and_trace(BF_MOD_PIPE,
                                       log_lvl,
                                       "\t\tRegister Value: 0x%" PRIx64
                                       " 0x%" PRIx64,
                                       resources[i].data.stful.dbl_dbl.hi,
                                       resources[i].data.stful.dbl_dbl.lo);
                else
                  bf_sys_log_and_trace(BF_MOD_PIPE,
                                       log_lvl,
                                       "\t\tRegister Value: 0x%" PRIx64,
                                       resources[i].data.stful.dbl);
                break;
            }
          }
        } else if (resources[i].tag == PIPE_RES_ACTION_TAG_DETACHED) {
          bf_sys_log_and_trace(BF_MOD_PIPE,
                               log_lvl,
                               "\tRegister Table: %s (DETACH)",
                               rtbl->name);
        } else if (resources[i].tag == PIPE_RES_ACTION_TAG_NO_CHANGE) {
          bf_sys_log_and_trace(BF_MOD_PIPE,
                               log_lvl,
                               "\tRegister Table: %s (NO CHANGE)",
                               rtbl->name);
        }
        break;
      }
      case PIPE_HDL_TYPE_STAT_TBL: {
        pipe_stat_tbl_info_t *rtbl = pipe_mgr_get_stat_tbl_info(
            devid, resources[i].tbl_hdl, __func__, __LINE__);
        if (!rtbl) continue;
        if (resources[i].tag == PIPE_RES_ACTION_TAG_ATTACHED) {
          if (rtbl->ref_type == PIPE_TBL_REF_TYPE_INDIRECT) {
            bf_sys_log_and_trace(BF_MOD_PIPE,
                                 log_lvl,
                                 "\tCounter Table: %s (ATTACH)",
                                 rtbl->name);
            bf_sys_log_and_trace(BF_MOD_PIPE,
                                 log_lvl,
                                 "\t\tCounter Index: %d",
                                 resources[i].tbl_idx);
          } else if (rtbl->ref_type == PIPE_TBL_REF_TYPE_DIRECT) {
            bf_sys_log_and_trace(
                BF_MOD_PIPE, log_lvl, "\tCounter Table: %s", rtbl->name);
            bf_sys_log_and_trace(BF_MOD_PIPE,
                                 log_lvl,
                                 "\t\tCounter Spec: %" PRIu64
                                 " Packets %" PRIu64 " Bytes",
                                 resources[i].data.counter.packets,
                                 resources[i].data.counter.bytes);
          }
        } else if (resources[i].tag == PIPE_RES_ACTION_TAG_DETACHED) {
          bf_sys_log_and_trace(
              BF_MOD_PIPE, log_lvl, "\tCounter Table: %s (DETACH)", rtbl->name);
        } else if (resources[i].tag == PIPE_RES_ACTION_TAG_NO_CHANGE) {
          bf_sys_log_and_trace(BF_MOD_PIPE,
                               log_lvl,
                               "\tCounter Table: %s (NO CHANGE)",
                               rtbl->name);
        }
        break;
      }
      case PIPE_HDL_TYPE_METER_TBL: {
        pipe_meter_tbl_info_t *rtbl = pipe_mgr_get_meter_tbl_info(
            devid, resources[i].tbl_hdl, __func__, __LINE__);
        if (!rtbl) continue;
        if (resources[i].tag == PIPE_RES_ACTION_TAG_ATTACHED) {
          bf_sys_log_and_trace(
              BF_MOD_PIPE, log_lvl, "\tMeter Table: %s (ATTACH)", rtbl->name);
          if (rtbl->ref_type == PIPE_TBL_REF_TYPE_INDIRECT) {
            bf_sys_log_and_trace(BF_MOD_PIPE,
                                 log_lvl,
                                 "\t\tMeter Index: %d",
                                 resources[i].tbl_idx);
          } else if (rtbl->ref_type == PIPE_TBL_REF_TYPE_DIRECT) {
            if (rtbl->meter_type == PIPE_METER_TYPE_STANDARD) {
              if (rtbl->meter_granularity == PIPE_METER_GRANULARITY_BYTES) {
                bf_sys_log_and_trace(BF_MOD_PIPE,
                                     log_lvl,
                                     "\t\tCIR: %" PRIx64 " kbps",
                                     resources[i].data.meter.cir.value.kbps);
                bf_sys_log_and_trace(BF_MOD_PIPE,
                                     log_lvl,
                                     "\t\tCBS: %" PRIx64,
                                     resources[i].data.meter.cburst);
                bf_sys_log_and_trace(BF_MOD_PIPE,
                                     log_lvl,
                                     "\t\tPIR: %" PRIx64 " kbps",
                                     resources[i].data.meter.pir.value.kbps);
                bf_sys_log_and_trace(BF_MOD_PIPE,
                                     log_lvl,
                                     "\t\tPBS: %" PRIx64,
                                     resources[i].data.meter.pburst);
              } else if (rtbl->meter_granularity ==
                         PIPE_METER_GRANULARITY_PACKETS) {
                bf_sys_log_and_trace(
                    BF_MOD_PIPE, log_lvl, "\t\tMeter Table: %s", rtbl->name);
                bf_sys_log_and_trace(BF_MOD_PIPE,
                                     log_lvl,
                                     "\t\tCIR: %" PRIx64 " pps",
                                     resources[i].data.meter.cir.value.pps);
                bf_sys_log_and_trace(BF_MOD_PIPE,
                                     log_lvl,
                                     "\t\tCBS: %" PRIx64,
                                     resources[i].data.meter.cburst);
                bf_sys_log_and_trace(BF_MOD_PIPE,
                                     log_lvl,
                                     "\t\tPIR: %" PRIx64 " pps",
                                     resources[i].data.meter.pir.value.pps);
                bf_sys_log_and_trace(BF_MOD_PIPE,
                                     log_lvl,
                                     "\t\tPBS: %" PRIx64,
                                     resources[i].data.meter.pburst);
              }
            } else if (rtbl->meter_type == PIPE_METER_TYPE_LPF) {
              bf_sys_log_and_trace(
                  BF_MOD_PIPE, log_lvl, "\tLPF Table: %s", rtbl->name);
              bf_sys_log_and_trace(
                  BF_MOD_PIPE,
                  log_lvl,
                  "\t\tSeparate Gain/Decay: %d",
                  resources[i].data.lpf.gain_decay_separate_time_constant);
              bf_sys_log_and_trace(BF_MOD_PIPE,
                                   log_lvl,
                                   "\t\tGain: %f nsec",
                                   resources[i].data.lpf.gain_time_constant);
              bf_sys_log_and_trace(BF_MOD_PIPE,
                                   log_lvl,
                                   "\t\tDecay: %f nsec",
                                   resources[i].data.lpf.decay_time_constant);
              bf_sys_log_and_trace(
                  BF_MOD_PIPE,
                  log_lvl,
                  "\t\tScale: %u",
                  resources[i].data.lpf.output_scale_down_factor);
            } else if (rtbl->meter_type == PIPE_METER_TYPE_WRED) {
              bf_sys_log_and_trace(
                  BF_MOD_PIPE, log_lvl, "\tWRED Table: %s", rtbl->name);
              bf_sys_log_and_trace(BF_MOD_PIPE,
                                   log_lvl,
                                   "\t\tTime: %f nsec",
                                   resources[i].data.red.time_constant);
              bf_sys_log_and_trace(BF_MOD_PIPE,
                                   log_lvl,
                                   "\t\tRMin: %u",
                                   resources[i].data.red.red_min_threshold);
              bf_sys_log_and_trace(BF_MOD_PIPE,
                                   log_lvl,
                                   "\t\tRMax: %u",
                                   resources[i].data.red.red_max_threshold);
              bf_sys_log_and_trace(BF_MOD_PIPE,
                                   log_lvl,
                                   "\t\tMaxProb: %f",
                                   resources[i].data.red.max_probability);
            }
          }
        } else if (resources[i].tag == PIPE_RES_ACTION_TAG_DETACHED) {
          bf_sys_log_and_trace(
              BF_MOD_PIPE, log_lvl, "\tMeter Table: %s (DETACH)", rtbl->name);
        } else if (resources[i].tag == PIPE_RES_ACTION_TAG_NO_CHANGE) {
          bf_sys_log_and_trace(BF_MOD_PIPE,
                               log_lvl,
                               "\tMeter Table: %s (NO CHANGE)",
                               rtbl->name);
        }
        break;
      }
    }
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_jsonify_match_spec(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec,
    cJSON *ms_node) {
  uint32_t bj_hash, i;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_spec_field_t *field_ptr;
  pipemgr_tbl_pkg_spec_t *matchspec_ptr;
  cJSON *fields, *field;
  char *field_str = NULL;
  char *field_str_ptr = NULL;
  size_t field_len = 0;
  size_t idx = 0;

  if (!ms_node) {
    return PIPE_SUCCESS;
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).matchspec_lut_depth,
      mat_tbl_hdl,
      0,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).matchspec_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).matchspec_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find match spec details for match table "
        "handle "
        "0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  matchspec_ptr = lut_ptr->u.matchspec_ptr;
  field_ptr = matchspec_ptr->fields;
  cJSON_AddItemToObject(ms_node, "fields", fields = cJSON_CreateArray());
  for (i = 0; i < matchspec_ptr->fieldcount; i++) {
    cJSON_AddItemToArray(fields, field = cJSON_CreateObject());
    cJSON_AddStringToObject(
        field,
        "name",
        pipemgr_tbl_pkg_get_field_string_name(
            devid, prof_id, field_ptr->fieldname_str_index));

    field_len = (field_ptr->fieldwidth + 7) >> 3;
    field_str = PIPE_MGR_REALLOC(field_str, 2 * field_len + 1);
    field_str_ptr = field_str;
    for (idx = 0; idx < field_len; idx++) {
      field_str_ptr += sprintf(
          field_str_ptr,
          "%02hhX",
          match_spec->match_value_bits[(field_ptr->startbit >> 3) + idx]);
    }
    field_str[2 * field_len] = 0;
    cJSON_AddStringToObject(field, "value", field_str);

    field_len = (field_ptr->fieldwidth + 7) >> 3;
    field_str = PIPE_MGR_REALLOC(field_str, 2 * field_len + 1);
    field_str_ptr = field_str;
    for (idx = 0; idx < field_len; idx++) {
      field_str_ptr += sprintf(
          field_str_ptr,
          "%02hhX",
          match_spec->match_mask_bits[(field_ptr->startbit >> 3) + idx]);
    }
    field_str[2 * field_len] = 0;
    cJSON_AddStringToObject(field, "mask", field_str);
    field_ptr++;
  }

  if (field_str) {
    PIPE_MGR_FREE(field_str);
  }
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_unjsonify_match_spec(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec,
    cJSON *ms_node) {
  uint32_t bj_hash, i;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_spec_field_t *field_ptr;
  pipemgr_tbl_pkg_spec_t *matchspec_ptr;
  cJSON *fields, *field;
  char *field_name, *field_str = NULL;
  uint32_t field_start;
  size_t field_len = 0;
  size_t idx = 0;

  if (!ms_node) {
    return PIPE_SUCCESS;
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).matchspec_lut_depth,
      mat_tbl_hdl,
      0,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).matchspec_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).matchspec_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find match spec details for match table "
        "handle "
        "0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  matchspec_ptr = lut_ptr->u.matchspec_ptr;
  field_ptr = matchspec_ptr->fields;
  match_spec->num_valid_match_bits = 0;
  match_spec->num_match_bytes = 0;
  for (i = 0; i < matchspec_ptr->fieldcount; i++) {
    match_spec->num_valid_match_bits += field_ptr->fieldwidth;
    match_spec->num_match_bytes += (field_ptr->fieldwidth + 7) >> 3;
    field_ptr++;
  }
  match_spec->match_value_bits = PIPE_MGR_MALLOC(match_spec->num_match_bytes);
  match_spec->match_mask_bits = PIPE_MGR_MALLOC(match_spec->num_match_bytes);

  field_ptr = matchspec_ptr->fields;
  fields = cJSON_GetObjectItem(ms_node, "fields");
  for (i = 0; i < matchspec_ptr->fieldcount; i++) {
    field_name = pipemgr_tbl_pkg_get_field_string_name(
        devid, prof_id, field_ptr->fieldname_str_index);

    for (field = fields->child; field; field = field->next) {
      if (!strcmp(field_name,
                  cJSON_GetObjectItem(field, "name")->valuestring)) {
        field_start = field_ptr->startbit >> 3;
        field_len = (field_ptr->fieldwidth + 7) >> 3;
        field_str = cJSON_GetObjectItem(field, "value")->valuestring;
        PIPE_MGR_DBGCHK(strlen(field_str) == (2 * field_len));
        for (idx = 0; idx < field_len; idx++) {
          sscanf(field_str + 2 * idx,
                 "%2hhx",
                 &match_spec->match_value_bits[field_start + idx]);
        }

        field_str = cJSON_GetObjectItem(field, "mask")->valuestring;
        PIPE_MGR_DBGCHK(strlen(field_str) == (2 * field_len));
        for (idx = 0; idx < field_len; idx++) {
          sscanf(field_str + 2 * idx,
                 "%2hhx",
                 &match_spec->match_mask_bits[field_start + idx]);
        }
      }
    }
    field_ptr++;
  }

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_jsonify_action_spec(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_action_data_spec_t *action_spec,
    pipe_act_fn_hdl_t act_fn_hdl,
    cJSON *as_node) {
  uint32_t bj_hash, i;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_spec_field_t *field_ptr;
  pipemgr_tbl_pkg_action_spec_t *actionspec_ptr;
  cJSON *fields, *field;
  char *field_str = NULL;
  char *field_str_ptr = NULL;
  size_t field_len = 0;
  size_t idx = 0;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  if (dev_info->fake_rmt_cfg) {
    return PIPE_SUCCESS;
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).actionspec_lut_depth,
      act_fn_hdl,
      0,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).actionspec_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).actionspec_lut,
      act_fn_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find action spec details for action function "
        "handle "
        "0x%x",
        __func__,
        act_fn_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  actionspec_ptr = lut_ptr->u.actionspec_ptr;
  field_ptr = actionspec_ptr->fields;
  cJSON_AddStringToObject(
      as_node,
      "action_fn_name",
      pipemgr_tbl_pkg_get_field_string_name(
          devid, prof_id, actionspec_ptr->actionfunc_name_str_index));
  cJSON_AddItemToObject(as_node, "fields", fields = cJSON_CreateArray());
  for (i = 0; i < actionspec_ptr->fieldcount; i++) {
    cJSON_AddItemToArray(fields, field = cJSON_CreateObject());
    cJSON_AddStringToObject(
        field,
        "name",
        pipemgr_tbl_pkg_get_field_string_name(
            devid, prof_id, field_ptr->fieldname_str_index));

    field_len = (field_ptr->fieldwidth + 7) >> 3;
    field_str = PIPE_MGR_REALLOC(field_str, 2 * field_len + 1);
    field_str_ptr = field_str;
    for (idx = 0; idx < field_len; idx++) {
      field_str_ptr += sprintf(
          field_str_ptr,
          "%02hhX",
          action_spec->action_data_bits[(field_ptr->startbit >> 3) + idx]);
    }
    field_str[2 * field_len] = 0;
    cJSON_AddStringToObject(field, "value", field_str);
    field_ptr++;
  }

  if (field_str) PIPE_MGR_FREE(field_str);

  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_unjsonify_action_spec(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_action_data_spec_t *action_spec,
    pipe_act_fn_hdl_t act_fn_hdl,
    cJSON *as_node) {
  uint32_t bj_hash, i;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_spec_field_t *field_ptr;
  pipemgr_tbl_pkg_action_spec_t *actionspec_ptr;
  cJSON *fields, *field;
  char *field_name, *field_value;
  size_t field_len;
  size_t idx = 0;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  if (dev_info->fake_rmt_cfg) {
    return PIPE_SUCCESS;
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).actionspec_lut_depth,
      act_fn_hdl,
      0,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).actionspec_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).actionspec_lut,
      act_fn_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find action spec details for action function "
        "handle "
        "0x%x",
        __func__,
        act_fn_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  actionspec_ptr = lut_ptr->u.actionspec_ptr;
  field_ptr = actionspec_ptr->fields;
  action_spec->num_valid_action_data_bits = 0;
  action_spec->num_action_data_bytes = 0;
  for (i = 0; i < actionspec_ptr->fieldcount; i++) {
    action_spec->num_valid_action_data_bits += field_ptr->fieldwidth;
    action_spec->num_action_data_bytes += (field_ptr->fieldwidth + 7) >> 3;
    field_ptr++;
  }
  action_spec->action_data_bits =
      PIPE_MGR_MALLOC(action_spec->num_action_data_bytes);

  field_ptr = actionspec_ptr->fields;
  fields = cJSON_GetObjectItem(as_node, "fields");
  for (i = 0; i < actionspec_ptr->fieldcount; i++) {
    field_name = pipemgr_tbl_pkg_get_field_string_name(
        devid, prof_id, field_ptr->fieldname_str_index);

    for (field = fields->child; field; field = field->next) {
      if (!strcmp(field_name,
                  cJSON_GetObjectItem(field, "name")->valuestring)) {
        field_len = (field_ptr->fieldwidth + 7) >> 3;
        field_value = cJSON_GetObjectItem(field, "value")->valuestring;
        PIPE_MGR_DBGCHK(strlen(field_value) == (2 * field_len));
        for (idx = 0; idx < field_len; idx++) {
          sscanf(
              field_value + 2 * idx,
              "%2hhx",
              &action_spec->action_data_bits[(field_ptr->startbit >> 3) + idx]);
        }
      }
    }
    field_ptr++;
  }

  return PIPE_SUCCESS;
}

/* Given a match spec bit ("bit"), returns the name of the field and bit within
 * that field it corresponds to.  For example, if the match spec had a single
 * 6-byte dmac and the input was bit==40, then field_name would be set to "dmac"
 * and field_bit would be set to 0.
 * Note that match spec bits have the usual/expected numbering where the first
 * byte of the array has bits 0..7, next byte has 8..15, etc.  The field bits
 * have a more unusual numbering.  They are considered "big endian" so the more
 * significant field bits are at the lower bytes.*/
pipe_status_t pipe_mgr_entry_format_ms_bit_to_field(bf_dev_id_t dev_id,
                                                    profile_id_t prof_id,
                                                    pipe_mat_tbl_hdl_t tbl_hdl,
                                                    int bit,
                                                    char **field_name,
                                                    int *field_bit) {
  uint32_t bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(dev_id, prof_id).matchspec_lut_depth, tbl_hdl, 0, 0);
  pipemgr_tbl_pkg_lut_t *lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(dev_id, prof_id).matchspec_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(dev_id, prof_id).matchspec_lut,
      tbl_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find match spec details for match table handle 0x%x",
        __func__,
        tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return PIPE_INVALID_ARG;
  }

  pipemgr_tbl_pkg_spec_t *matchspec_ptr = lut_ptr->u.matchspec_ptr;
  for (unsigned i = 0; i < matchspec_ptr->fieldcount; ++i) {
    pipemgr_tbl_pkg_spec_field_t *field_ptr = &matchspec_ptr->fields[i];

    int ms_end_byte;   /* MS byte with the msb of the field. */
    int ms_start_byte; /* MS byte with the lsb of the field. */
    int ms_end_bit;    /* MS bit of the field's msb. */

    /* Note that end byte is numerically smaller than start byte as the fields
     * are stored in the match spec with the field's lsb stored at the larger
     * index to the match bits and the field's msb stored at the lower index. */
    ms_end_byte = field_ptr->startbit / 8;
    ms_start_byte = ms_end_byte + (field_ptr->fieldwidth + 7) / 8 - 1;
    /* We recalculate the start bit since it is represented differently if the
     * field is a byte or less vs more than a byte. */
    int start_bit = (ms_start_byte + 1) * 8 - field_ptr->fieldwidth;
    /* The match spec start bit is in "big endian" notation even for bits!  For
     * example a match spec with two one bit fields would have start bits of 7
     * and 15, not 0 and 8.
     * Translate it into a sane value. */
    int extra_bits = start_bit % 8;
    ms_end_bit = ms_end_byte * 8;
    if (extra_bits) {
      ms_end_bit += 7 - extra_bits;
    } else {
      ms_end_bit += 7;
    }

    /* If the requested bit does not belong to this field skip ahead to the next
     * field in the match spec. */
    int b = bit / 8;
    if (b > ms_start_byte || b < ms_end_byte) continue;
    if (b == ms_end_byte && bit > ms_end_bit) continue;

    /* Retrieve the field name for the caller. */
    *field_name = pipemgr_tbl_pkg_get_field_string_name(
        dev_id, prof_id, field_ptr->fieldname_str_index);

    /* Fields in the match spec are in big endian format.  For example a MS with
     * a single 12-bit vlan-id would be two bytes, 16-bits.  The least
     * significant bit of vlan-id would be at offset 15.  The least significant
     * byte of vlan-id would be at offset 8-15 while the upper for bits of id
     * would be at offset 4-7.  Bits 0-3 are padding.
     * The field_bit is in little endian notation, so zero corresponds to the
     * least significant bit. */
    *field_bit = (ms_start_byte - b) * 8 + (bit % 8);
    return PIPE_SUCCESS;
  }
  return PIPE_OBJ_NOT_FOUND;
}

static uint8_t extract_to_byte(uint8_t *src,
                               int byte_offset,
                               int bit_offset,
                               int len) {
  uint8_t tmp;
  uint8_t mask = 0xFF;
  uint8_t shift;

  PIPE_MGR_DBGCHK(len && (len <= 8));
  mask = (bit_offset == 7) ? 0xFF : ((1 << (bit_offset + 1)) - 1);
  shift = (len <= bit_offset) ? (bit_offset + 1 - len) : 0;
  tmp = (src[byte_offset] & mask) >> shift;
  if (len > (bit_offset + 1)) {
    /* We need some from the previous byte */
    shift = 8 - (len - (bit_offset + 1));
    PIPE_MGR_DBGCHK(byte_offset > 0);
    tmp = (tmp << (8 - shift)) | (src[byte_offset - 1] >> shift);
  }
  return tmp;
}

#if __BYTE_ORDER == __LITTLE_ENDIAN
#define LITTLE_ENDIAN_CALLER 1
#endif

static uint16_t extract_to_short(uint8_t *src,
                                 int byte_offset,
                                 int bit_offset,
                                 int len) {
  uint16_t tmp = 0;
  int l;
  int i = 0;

  PIPE_MGR_DBGCHK((len > 8) && (len <= 16));
  PIPE_MGR_DBGCHK(bit_offset < 8);

  l = len + (8 - bit_offset - 1);
  while (l > 0) {
    PIPE_MGR_DBGCHK(byte_offset >= i);
    if (l >= 8) {
      tmp = (tmp << 8) | src[byte_offset - i];
      l -= 8;
    } else {
      tmp = (tmp << l) | extract_to_byte(src, byte_offset - i, 7, l);
      l = 0;
    }
    i++;
  }
  uint16_t mask = ~0;
  mask = mask >> (16 - len);
  tmp &= mask;
#ifdef LITTLE_ENDIAN_CALLER
  // The caller will use the value without any conversion
  return tmp;
#else
  // The caller expects the value in network format.
  return htons(tmp);
#endif
}

static uint32_t extract_to_long(uint8_t *src,
                                int byte_offset,
                                int bit_offset,
                                int len) {
  uint32_t tmp = 0;
  int l;
  int i = 0;

  PIPE_MGR_DBGCHK((len > 16) && (len <= 32));
  PIPE_MGR_DBGCHK(bit_offset < 8);

  l = len + (8 - bit_offset - 1);
  while (l > 0) {
    PIPE_MGR_DBGCHK(byte_offset >= i);
    if (l >= 8) {
      tmp = (tmp << 8) | src[byte_offset - i];
      l -= 8;
    } else {
      tmp = (tmp << l) | extract_to_byte(src, byte_offset - i, 7, l);
      l = 0;
    }
    i++;
  }
  uint32_t mask = ~0;
  mask = mask >> (32 - len);
  tmp &= mask;
#ifdef LITTLE_ENDIAN_CALLER
  // The caller will use the value without any conversion
  return tmp;
#else
  // The caller expects the value in network format.
  return htonl(tmp);
#endif
}

static void extract_to_arr(
    uint8_t *src, uint8_t *dest, int byte_offset, int bit_offset, int len) {
  int l = len;
  int nbyte;
  int nbit;

  PIPE_MGR_DBGCHK(byte_offset >= (len - 1) / 8);
  nbyte = byte_offset - (len - 1) / 8;
  if (len % 8) {
    nbit = bit_offset + 8 - (len % 8);
    if (nbit > 7) {
      nbyte++;
      nbit -= 8;
    }
  } else {
    nbit = bit_offset;
  }
  while (l > 8) {
    PIPE_MGR_DBGCHK(nbyte >= 0);
    dest[(l - 1) / 8] = extract_to_byte(src, nbyte, nbit, 8);
    l -= 8;
    nbyte++;
  }
  if (l) {
    dest[0] = extract_to_byte(src, byte_offset, bit_offset, l);
  }
}

size_t pipe_mgr_entry_format_lrn_cfg_type_sz(bf_dev_id_t devid,
                                             profile_id_t prof_id,
                                             uint8_t learn_cfg_type) {
  uint32_t bj_hash;
  pipemgr_tbl_pkg_lut_t *lut_ptr;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  if (dev_info->fake_rmt_cfg) {
    return (p4_fake_lrn_cfg_type_sz(learn_cfg_type));
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).lq_lut_depth,
      (learn_cfg_type + 1),
      0,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry_no_logging(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).lq_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).lq_lut,
      (learn_cfg_type + 1),
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find learn quanta details for config type "
        "0x%x",
        __func__,
        learn_cfg_type);
    return 0;
  }
  return (lut_ptr->u.lq_ptr->lrn_cfg_type_sz);
}

pipe_fld_lst_hdl_t pipe_mgr_entry_format_get_handle_of_lrn_cfg_type(
    bf_dev_id_t devid, profile_id_t prof_id, uint8_t learn_cfg_type) {
  uint32_t bj_hash;
  pipemgr_tbl_pkg_lut_t *lut_ptr;

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).lq_lut_depth,
      (learn_cfg_type + 1),
      0,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry_no_logging(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).lq_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).lq_lut,
      (learn_cfg_type + 1),
      0);
  if (!lut_ptr) {
    return 0;
  }
  return (lut_ptr->u.lq_ptr->handle);
}

uint8_t pipe_mgr_entry_format_fld_lst_hdl_to_lq_cfg_type(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_fld_lst_hdl_t flow_lrn_fld_lst_hdl) {
  uint32_t bj_hash, handle;
  int i;
  pipemgr_tbl_pkg_lut_t *lut_ptr;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  if (dev_info->fake_rmt_cfg) {
    return (p4_fake_fld_lst_hdl_to_lq_cfg_type(flow_lrn_fld_lst_hdl));
  }

  for (i = 0; i < PIPE_MGR_NUM_LEARN_TYPES; i++) {
    handle =
        pipe_mgr_entry_format_get_handle_of_lrn_cfg_type(devid, prof_id, i);
    if (handle != 0 && handle == flow_lrn_fld_lst_hdl) {
      break;
    }
  }
  if (i >= PIPE_MGR_NUM_LEARN_TYPES) {
    return -1;
  }
  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).lq_lut_depth, (i + 1), 0, 0);
  lut_ptr = pipemgr_entry_format_get_lut_entry_no_logging(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).lq_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).lq_lut,
      (i + 1),
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find learn quanta details for handle "
        "0x%x",
        __func__,
        flow_lrn_fld_lst_hdl);
    return 0;
  }
  return (lut_ptr->u.lq_ptr->lq_cfg_type);
}

pipe_status_t pipe_mgr_entry_format_fld_lst_hdl_to_profile(
    bf_dev_id_t devid,
    pipe_fld_lst_hdl_t flow_lrn_fld_lst_hdl,
    profile_id_t *ret_prof_id) {
  uint32_t handle;
  int i;
  profile_id_t prof_id = 0;
  uint32_t idx = 0;

  *ret_prof_id = 0;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  for (idx = 0; idx < dev_info->num_pipeline_profiles; idx++) {
    prof_id = dev_info->profile_info[idx]->profile_id;
    if (!PIPE_MGR_TBL_PKG_CTX(devid, prof_id).lq_lut) {
      /* lq_lut could be NULL for some profiles on the device as they
         may not have any learning tables,
      */
      continue;
    }

    for (i = 0; i < PIPE_MGR_NUM_LEARN_TYPES; i++) {
      handle =
          pipe_mgr_entry_format_get_handle_of_lrn_cfg_type(devid, prof_id, i);
      if (handle != 0 && handle == flow_lrn_fld_lst_hdl) {
        *ret_prof_id = prof_id;
        return PIPE_SUCCESS;
      }
    }
  }

  return PIPE_OBJ_NOT_FOUND;
}

uint8_t pipe_mgr_get_digest_cfg_type(rmt_dev_info_t *dev_info,
                                     profile_id_t prof_id,
                                     uint8_t lq_data[48]) {
  // digest_type field defines width of cfg type field in digest.
  // It must be checked because depending on PHV allocation cfg_type field
  // can end up in differently sized PHVs.
  // digest_type field is always in the last stage of PHV definition as that is
  // deparser stage.
  // Decode cfg_type from digest data using fetched digest_type PHV info.
  // If found return PIPE_SUCCESS and cfg_type value.
  // If not found return ERROR
  char *fieldname = "ig_intr_md_for_dprsr_digest_type";
  int direction = 0;  // INGRESS
  uint8_t width = 0;
  uint32_t bj_hash;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_snapshot_t *snapshot_ptr;
  pipemgr_tbl_pkg_snapshot_phv_t *phv_recs;

  bf_dev_id_t devid = dev_info->dev_id;
  uint8_t last_stage = dev_info->profile_info[prof_id]->num_stages;

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth,
      0,
      last_stage + 1,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut,
      0,
      last_stage + 1);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find phv details from context json, dev %d profile-id "
        "%d direction %s field %s stage %d",
        __func__,
        devid,
        prof_id,
        direction ? "egress" : "ingess",
        fieldname,
        last_stage);
    return (~0);
  }

  snapshot_ptr = lut_ptr->u.snapshot_ptr;
  phv_recs = snapshot_ptr->phv_recs;
  // Find respective PHV
  for (uint32_t i = 0; i < snapshot_ptr->total_phv_recs; i++) {
    if (phv_recs->direction != direction) {
      phv_recs++;
      continue;
    }
    if (!strcmp(fieldname,
                pipemgr_tbl_pkg_get_field_string_name(
                    devid, prof_id, phv_recs->phvname_str_index))) {
      width = phv_recs->container_width;
      break;
    }
    phv_recs++;
  }
  // Digest data is 48 byte long, cfg_type is always last so count from the end.
  // cfg_type is max 3 bit wide, so regardless of PHV width value itself will
  // always fit.
  uint8_t cfg_type;
  switch (width) {
    case 8:
      cfg_type = lq_data[47];
      break;
    case 16:
      cfg_type = le16toh(*(uint16_t *)(lq_data + 46));
      break;
    case 32:
      cfg_type = le32toh(*(uint32_t *)(lq_data + 44));
      break;
    default:
      cfg_type = ~0;
  }
  return cfg_type;
}

/* Decode a learn quanta (48 bytes of data in lq_data) into a learn digest
 * entry for the application.  Fields will be extracted from the lq_data,
 * padded to a byte boundary and written, in order, to the lrn_digest_entry
 * memory.  Here lrn_digest_entry is a void pointer but the application will
 * see it as a packed array of a structure type representing the learn data.
 * In this struct fields are rounded up to byte boundaries AND any three byte
 * fields are rounded up to four bytes.  For example, learning a 12-bit vlan
 * id 48 bit MAC and 9 bit port number would give a struct of:
 *   u16 vlan
 *   u16 port
 *   u8[6] mac
 * The struct would be 8 bytes long so this function will take 12 bits of vlan
 * from lq_data, pad it to 16 bits and write it to lrn_digest_entry.  Then take
 * 9 bits of port from lq_data, pad it to 16 and write it to lrn_digest_entry.
 * Finally it will take 48 bits of MAC from lq_data and add it as well.
 * Note, if a field is 3 bytes we round it up to four bytes.  So an MPLS label
 * is 20 bits, rounded up to a byte boundary is 24 bits but we pad it to 32 bits
 * instead.
 */
pipe_status_t pipe_mgr_entry_format_lrn_decode(rmt_dev_info_t *dev_info,
                                               profile_id_t prof_id,
                                               uint8_t pipe,
                                               uint8_t learn_cfg_type,
                                               uint8_t lq_data[48],
                                               void *lrn_digest_entry,
                                               uint32_t index,
                                               bool network_order) {
  uint16_t byte_off, bit_off, phv_off, bit_width, byte_width, shift;
  uint32_t i = 0, j = 0;
  uint32_t bj_hash;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_lq_field_t *field_ptr;
  uint8_t *lrn_digest_mem_ptr = (uint8_t *)lrn_digest_entry;
  bf_dev_id_t devid = dev_info->dev_id;

  if (dev_info->fake_rmt_cfg) {
    p4_fake_lrn_decode(pipe, learn_cfg_type, lq_data, lrn_digest_entry, index);
    return PIPE_SUCCESS;
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).lq_lut_depth,
      (learn_cfg_type + 1),
      0,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry_no_logging(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).lq_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).lq_lut,
      (learn_cfg_type + 1),
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find learn quanta details for config type "
        "0x%x",
        __func__,
        learn_cfg_type);
    return 0;
  }
  pipemgr_tbl_pkg_lq_t *lq_ptr = lut_ptr->u.lq_ptr;

  field_ptr = lq_ptr->fields;
  /* lrn_cfg_type_sz is the size of one digest entry struct.  Use it along with
   * the index into the lrn_digest_entry array of these structs to get a pointer
   * to where we will store this digest entry. */
  lrn_digest_mem_ptr += (lq_ptr->lrn_cfg_type_sz * index);
  /* field_buf will be scratch space used to build one field of the digest.
   * Since a field may be spread over multiple containers we may be required to
   * perform several extractions from lq_data into this scratch space to
   * assemble the entire field. */
  uint8_t field_buf[lq_ptr->lrn_cfg_type_sz];
  while (i < lq_ptr->fieldcount) {
    PIPE_MGR_MEMSET(field_buf, 0, lq_ptr->lrn_cfg_type_sz);
    uint32_t total_field_width = 0;
    uint32_t stringindex = field_ptr->fieldname_str_index;
    byte_width = 0;
    /* The records in the lq_ptr's fields array are grouped by field.  So all
     * records for a given field will be consecutive in the array.  This while
     * loop will let us go over all records for a given field. */
    while (i < lq_ptr->fieldcount &&
           field_ptr->fieldname_str_index == stringindex) {
      /* Identifies which byte in lq_data we will start the extraction from. */
      byte_off = field_ptr->byteoffset;
      /* Identifies a bit in the lq_data to start the extraction from in a
       * backwards way, 7 means the entire byte, 6 means to skip the lsb, 0
       * means the msb. */
      bit_off = field_ptr->startbit;
      /* phv_off is the offset of this slice within the complete field.  If this
       * record is directing us to extract f[6:4] then phv_off would be 4 since
       * this three bit slice starts at bit 4 of the overall field. */
      phv_off = field_ptr->phvoffset;
      /* The number of bits to extract for this portion of the field. */
      bit_width = field_ptr->fieldwidth;
      /* The number of bytes we need to extract from lq_data. */
      byte_width = (bit_width + 7) / 8;

      shift = field_ptr->totalfieldwidth - phv_off - bit_width;

      /* For each byte of field, extract the byte from lq_data and populate the
       * scratch space. */
      for (j = 0; j < byte_width; j++) {
        PIPE_MGR_DBGCHK(bit_width > 0);
        /* Grab up to 8 bits from lq_data, note that it may pull data from the
         * adjacent byte as well if bit_width and bit_off request it. */
        uint8_t lq_byte = extract_to_byte(lq_data,
                                          LEARN_QUANTA_SIZE - 1 - byte_off,
                                          bit_off,
                                          bit_width >= 8 ? 8 : bit_width);

        if (bit_width < 8) {
          lq_byte <<= (8 - bit_width);
        }

        int idx = lq_ptr->lrn_cfg_type_sz - shift / 8;
        field_buf[idx - 1] |= lq_byte >> (shift % 8);

        if ((idx - 2) > 0 && (shift % 8)) {
          field_buf[idx - 2] |= lq_byte << (8 - (shift % 8));
        }
        byte_off++;
        bit_width -= 8;
        shift += 8;
      }

      total_field_width += field_ptr->fieldwidth;
      field_ptr++;
      i++;
    }

    if (bf_sys_log_is_log_enabled(BF_MOD_PIPE, BF_LOG_DBG) == 1) {
      char buf[512] = {0};
      for (unsigned int k = 0;
           k < lq_ptr->lrn_cfg_type_sz && k < (sizeof buf - 1) / 2;
           ++k) {
        snprintf(&buf[2 * k], 3, "%02X", field_buf[k]);
      }
      LOG_DBG("Decoding field %s",
              PIPE_MGR_TBL_PKG_CTX(dev_info->dev_id, prof_id)
                  .p4_strings[stringindex]);
      LOG_DBG("  Value: %s", buf);
    }

    byte_width = (total_field_width + 7) / 8;
    if (byte_width > 4 || network_order) {
      extract_to_arr(field_buf,
                     lrn_digest_mem_ptr,
                     lq_ptr->lrn_cfg_type_sz - 1,
                     7,
                     total_field_width);
    } else if (byte_width == 1) {
      *lrn_digest_mem_ptr = extract_to_byte(
          field_buf, lq_ptr->lrn_cfg_type_sz - 1, 7, total_field_width);
    } else if (byte_width == 2) {
      uint16_t *short_ptr = (uint16_t *)lrn_digest_mem_ptr;
      *short_ptr = extract_to_short(
          field_buf, lq_ptr->lrn_cfg_type_sz - 1, 7, total_field_width);
    } else if ((byte_width == 4) || (byte_width == 3)) {
      uint32_t *long_ptr = (uint32_t *)lrn_digest_mem_ptr;
      *long_ptr = extract_to_long(
          field_buf, lq_ptr->lrn_cfg_type_sz - 1, 7, total_field_width);
    } else {
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
    }
    lrn_digest_mem_ptr += (byte_width == 3) ? 4 : byte_width;
  }

  return PIPE_SUCCESS;
}

bool pipe_mgr_entry_format_is_lrn_type_valid(bf_dev_id_t devid,
                                             profile_id_t prof_id,
                                             int lq_type) {
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  if (dev_info->fake_rmt_cfg) {
    return true;
  }

  uint32_t bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).lq_lut_depth, lq_type + 1, 0, 0);
  pipemgr_tbl_pkg_lut_t *lut_ptr =
      pipemgr_entry_format_get_lut_entry_no_logging(
          bj_hash,
          PIPE_MGR_TBL_PKG_CTX(devid, prof_id).lq_lut_depth,
          PIPE_MGR_TBL_PKG_CTX(devid, prof_id).lq_lut,
          lq_type + 1,
          0);
  if (!lut_ptr) {
    return false;
  }
  return true;
}

static void multiply_range(uint32_t index,
                           uint32_t no_fields,
                           uint64_t *range_starts[],
                           uint64_t *range_ends[],
                           uint32_t range_expand_count[],
                           uint64_t *output_start[],
                           uint64_t *output_end[],
                           pipe_tbl_match_spec_t *expand_match_spec[],
                           uint32_t entries_per_block,
                           uint32_t *exp_idx_p,
                           uint32_t spec_offsets[],
                           uint32_t spec_lens[],
                           uint16_t shift[]) {
  uint32_t i, j;
  if (index == no_fields) {
    /* Now copy over the spec */
    for (j = 0; j < entries_per_block; j++) {
      for (i = 0; i < no_fields; i++) {
        put_shifted_val(expand_match_spec[*exp_idx_p][j].match_value_bits,
                        spec_offsets[i],
                        spec_lens[i],
                        output_start[j][i] << shift[i]);
        put_shifted_val(expand_match_spec[*exp_idx_p][j].match_mask_bits,
                        spec_offsets[i],
                        spec_lens[i],
                        output_end[j][i] << shift[i]);
      }
    }
    (*exp_idx_p)++;
    return;
  }
  for (i = 0; i < range_expand_count[index]; i += entries_per_block) {
    for (j = 0; j < entries_per_block; j++) {
      if ((i + j) < range_expand_count[index]) {
        output_start[j][index] = range_starts[index][i + j];
        output_end[j][index] = range_ends[index][i + j];
      } else {
        /* Fill up with the last valid range */
        output_start[j][index] =
            range_starts[index][range_expand_count[index] - 1];
        output_end[j][index] = range_ends[index][range_expand_count[index] - 1];
      }
    }
    multiply_range(index + 1,
                   no_fields,
                   range_starts,
                   range_ends,
                   range_expand_count,
                   output_start,
                   output_end,
                   expand_match_spec,
                   entries_per_block,
                   exp_idx_p,
                   spec_offsets,
                   spec_lens,
                   shift);
  }
}

static pipe_status_t expand_range(pipe_tbl_match_spec_t *match_spec,
                                  uint16_t spec_offset,
                                  uint16_t spec_len,
                                  uint16_t spec_shift,
                                  uint64_t *nibble_counts,
                                  uint32_t nibble_count_size,
                                  uint64_t *start_vals,
                                  uint64_t *end_vals,
                                  uint32_t *count_p,
                                  uint64_t *expand_start,
                                  uint64_t *expand_end,
                                  uint32_t expand_count) {
  uint32_t exp_idx = 0;
  uint64_t k[16] = {0};
  uint64_t m[16] = {0};
  /* Get the range start and end into k and m arrays.  Arrays incase the fields
   * are wider than 64 bit. */
  get_shifted_val(
      match_spec->match_value_bits, spec_offset, spec_len, spec_shift, k);
  get_shifted_val(
      match_spec->match_mask_bits, spec_offset, spec_len, spec_shift, m);

  /* Get the total number of bits we are ranging matching. */
  int range_match_bits = 0;
  for (uint32_t n = 0; n < nibble_count_size; ++n)
    range_match_bits += nibble_counts[n];

  /* Only include the portion of the spec that we are matching against in the
   * start and end values. */
  uint64_t spec_mask = range_match_bits < 64 ? (1ull << range_match_bits) - 1
                                             : 0xFFFFFFFFFFFFFFFFull;
  uint64_t start = k[0] & spec_mask, end = m[0] & spec_mask;
  uint32_t range_start = 0, range_end = 0;

  if (end < start) {
    return PIPE_INVALID_ARG;
  }

  range_start = start;

  uint32_t i;
  uint32_t start_nibble;
  if (count_p) {
    *count_p = 0;
  }

  do {
    if (range_start == 0) {
      start_nibble = nibble_count_size - 1;
    } else {
      uint64_t zeroes = __builtin_ctzl(range_start);
      uint64_t j;
      uint64_t count = 0;
      for (j = 0; j < nibble_count_size; j++) {
        if ((count + nibble_counts[j]) > zeroes) {
          break;
        }
        count += nibble_counts[j];
      }
      start_nibble = j;
    }

    PIPE_MGR_DBGCHK(start_nibble < nibble_count_size);
    for (i = start_nibble + 1; i > 0; i--) {
      range_end = range_start | end_vals[i - 1];

      for (; (range_end >= range_start) && (range_end > end) &&
             (range_end >= start_vals[i - 1]);
           range_end -= start_vals[i - 1]) {
      }

      if ((range_end >= range_start) && (range_end <= end)) {
        break;
      }
    }

    if (expand_end) {
      PIPE_MGR_DBGCHK(exp_idx < expand_count);
      expand_start[exp_idx] = range_start;
      expand_end[exp_idx] = range_end;
      exp_idx++;
    }
    if (count_p) {
      (*count_p)++;
    }

    range_start = range_end + 1;
  } while (range_end < end);
  return PIPE_SUCCESS;
}

pipe_status_t pipe_mgr_entry_format_tof_count_range_expand(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec,
    uint32_t *num_blocks_p,
    uint32_t *num_entries_per_block_p) {
  uint32_t no_entries = 1, no_blocks = 1, index;
  pipe_status_t rc = PIPE_SUCCESS;
  uint32_t bj_hash;
  int i, j, maufieldcount;
  uint16_t spec_start_bit, spec_bit_width;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_rangetbl_t *rangetbl_ptr;
  pipemgr_tbl_pkg_rangetbl_field_t *rangefield_ptr;
  pipemgr_tbl_pkg_rangetbl_mau_field_t *maufield_ptr;
  if (!PIPE_MGR_TBL_PKG_CTX(devid, prof_id).rangetbl_lut) {
    // Range checks are done even when range tables are not present.
    // return.
    return PIPE_SUCCESS;
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).rangetbl_lut_depth,
      mat_tbl_hdl,
      0,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry_no_logging(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).rangetbl_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).rangetbl_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    // Range related operations are checked on tables that may not
    // support range.
    // If match-table handle is not in range tables, return success.
    return (PIPE_SUCCESS);
  }

  rangetbl_ptr = lut_ptr->u.rangetbl_ptr;
  uint32_t count_expand_entries[rangetbl_ptr->fieldcount];
  // For each field, figure out how many entries this gets expanded to
  // Multiply them together
  // range_width = 4;
  index = 0;
  rangefield_ptr = rangetbl_ptr->fields;
  for (i = 0; i < rangetbl_ptr->fieldcount; i++) {
    spec_start_bit = rangefield_ptr->param_startbit;
    spec_bit_width = rangefield_ptr->param_fieldwidth;
    maufieldcount = rangefield_ptr->maufieldcount;
    maufield_ptr = rangefield_ptr->maufields;
    uint16_t shift = maufield_ptr->startbit;
    uint64_t nibble_counts[maufieldcount];
    uint64_t start_vals[maufieldcount];
    uint64_t end_vals[maufieldcount];
    for (j = 0; j < maufieldcount; j++) {
      maufield_ptr = &rangefield_ptr->maufields[j];
      nibble_counts[j] = maufield_ptr->fieldwidth;
      start_vals[j] = (uint64_t)(1ull << (maufield_ptr->startbit - shift));
      end_vals[j] = (uint64_t)((1ull << (maufield_ptr->fieldwidth +
                                         maufield_ptr->startbit - shift)) -
                               1);
    }
    rc = expand_range(match_spec,
                      spec_start_bit,
                      spec_bit_width,
                      shift,
                      nibble_counts,
                      maufieldcount,
                      start_vals,
                      end_vals,
                      &count_expand_entries[index],
                      NULL,
                      NULL,
                      0);
    if (rc != PIPE_SUCCESS) {
      return rc;
    }
    index += 1;
    rangefield_ptr++;
    rangefield_ptr =
        (pipemgr_tbl_pkg_rangetbl_field_t
             *)((uint8_t *)rangefield_ptr +
                (maufieldcount * sizeof(pipemgr_tbl_pkg_rangetbl_mau_field_t)));
  }
  uint32_t k;
  for (k = 0; k < index; k++) {
    no_blocks *= (count_expand_entries[k] + 7) / 8;
    if (count_expand_entries[k] > no_entries) {
      no_entries = count_expand_entries[k] > 8 ? 8 : count_expand_entries[k];
    }
  }
  *num_blocks_p = no_blocks;
  *num_entries_per_block_p = no_entries;
  return (PIPE_SUCCESS);
}

pipe_status_t pipe_mgr_entry_format_tof_range_expand(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec,
    /* Prepopulated Expanded match-specs -
     * array of size num_blocks * num_entries_per_block
     */
    pipe_tbl_match_spec_t *expand_match_spec[],
    uint32_t num_blocks,
    uint32_t num_entries_per_block) {
  pipe_status_t rc = PIPE_SUCCESS;
  uint32_t bj_hash, i, p;
  int j, index, maufieldcount, max_expansion_entry_count;
  uint16_t spec_start_bit, spec_bit_width;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_rangetbl_t *rangetbl_ptr;
  pipemgr_tbl_pkg_rangetbl_field_t *rangefield_ptr;
  pipemgr_tbl_pkg_rangetbl_mau_field_t *maufield_ptr;

  (void)num_blocks;
  if (!PIPE_MGR_TBL_PKG_CTX(devid, prof_id).rangetbl_lut) {
    // Range checks are done even when range tables are not present.
    // return.
    return PIPE_SUCCESS;
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).rangetbl_lut_depth,
      mat_tbl_hdl,
      0,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry_no_logging(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).rangetbl_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).rangetbl_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    // Range related operations are checked on tables that may not
    // support range.
    // If match-table handle is not in range tables, return success.
    return (PIPE_SUCCESS);
  }

  rangetbl_ptr = lut_ptr->u.rangetbl_ptr;
  rangefield_ptr = rangetbl_ptr->fields;

  uint64_t *_range_start[rangetbl_ptr->fieldcount];
  uint64_t *_range_end[rangetbl_ptr->fieldcount];
  uint32_t _expand_count[rangetbl_ptr->fieldcount];

  uint16_t shift[rangetbl_ptr->fieldcount];
  for (i = 0; i < rangetbl_ptr->fieldcount; i++) {
    shift[i] = 0;
    spec_start_bit = rangefield_ptr->param_startbit;
    spec_bit_width = rangefield_ptr->param_fieldwidth;
    maufieldcount = rangefield_ptr->maufieldcount;
    max_expansion_entry_count = (2 * maufieldcount) - 1;
    maufield_ptr = rangefield_ptr->maufields;
    _range_start[i] =
        PIPE_MGR_CALLOC(1, max_expansion_entry_count * sizeof(uint64_t));
    _range_end[i] =
        PIPE_MGR_CALLOC(1, max_expansion_entry_count * sizeof(uint64_t));
    if (!_range_start[i] || !_range_end[i]) {
      for (p = 0; p < i; p++) {
        PIPE_MGR_FREE(_range_start[p]);
        PIPE_MGR_FREE(_range_end[p]);
      }
      LOG_ERROR(
          "%s: Unable to allocate memory during range expansion table 0x%x",
          __func__,
          mat_tbl_hdl);
      PIPE_MGR_DBGCHK(0);
      return (PIPE_NO_SYS_RESOURCES);
    }
    _expand_count[i] = 0;
    /* For each range-based field, there will "maufieldcount" number of nibbles
     * (given that we do 4-bit ranges).
     */
    shift[i] = rangefield_ptr->maufields[0].startbit;
    uint64_t nibble_counts[maufieldcount];
    uint64_t start_vals[maufieldcount];
    uint64_t end_vals[maufieldcount];
    for (j = 0; j < maufieldcount; j++) {
      maufield_ptr = &rangefield_ptr->maufields[j];
      nibble_counts[j] = maufield_ptr->fieldwidth;
      start_vals[j] = (uint64_t)(1ull << (maufield_ptr->startbit - shift[i]));
      end_vals[j] = (uint64_t)((1ull << (maufield_ptr->fieldwidth +
                                         maufield_ptr->startbit - shift[i])) -
                               1);
    }

    rc = expand_range(match_spec,
                      spec_start_bit,
                      spec_bit_width,
                      shift[i],
                      nibble_counts,
                      maufieldcount,
                      start_vals,
                      end_vals,
                      &(_expand_count[i]),
                      _range_start[i],
                      _range_end[i],
                      max_expansion_entry_count);
    if (rc != PIPE_SUCCESS) {
      for (p = 0; p < i; p++) {
        PIPE_MGR_FREE(_range_start[p]);
        PIPE_MGR_FREE(_range_end[p]);
      }
      return rc;
    }
    rangefield_ptr++;
    rangefield_ptr =
        (pipemgr_tbl_pkg_rangetbl_field_t
             *)((uint8_t *)rangefield_ptr +
                (maufieldcount * sizeof(pipemgr_tbl_pkg_rangetbl_mau_field_t)));
  }

  // Now we need to multiply each of them
  uint64_t *range_starts[rangetbl_ptr->fieldcount];
  uint64_t *range_ends[rangetbl_ptr->fieldcount];
  uint32_t range_expand_counts[rangetbl_ptr->fieldcount];
  uint32_t spec_offsets[rangetbl_ptr->fieldcount];
  uint32_t spec_lens[rangetbl_ptr->fieldcount];

  index = 0;
  rangefield_ptr = rangetbl_ptr->fields;
  for (i = 0; i < rangetbl_ptr->fieldcount; i++) {
    spec_start_bit = rangefield_ptr->param_startbit;
    spec_bit_width = rangefield_ptr->param_fieldwidth;
    maufieldcount = rangefield_ptr->maufieldcount;
    range_starts[index] = _range_start[i];
    range_ends[index] = _range_end[i];
    range_expand_counts[index] = _expand_count[i];
    spec_offsets[index] = spec_start_bit;
    spec_lens[index] = spec_bit_width;
    index += 1;
    rangefield_ptr++;
    rangefield_ptr =
        (pipemgr_tbl_pkg_rangetbl_field_t
             *)((uint8_t *)rangefield_ptr +
                (maufieldcount * sizeof(pipemgr_tbl_pkg_rangetbl_mau_field_t)));
  }

  uint64_t o_start[num_entries_per_block][rangetbl_ptr->fieldcount];
  uint64_t o_end[num_entries_per_block][rangetbl_ptr->fieldcount];
  uint64_t *output_starts[num_entries_per_block];
  uint64_t *output_ends[num_entries_per_block];

  for (i = 0; i < num_entries_per_block; i++) {
    output_starts[i] = o_start[i];
    output_ends[i] = o_end[i];
  }

  uint32_t exp_idx = 0;
  multiply_range(0,
                 rangetbl_ptr->fieldcount,
                 range_starts,
                 range_ends,
                 range_expand_counts,
                 output_starts,
                 output_ends,
                 expand_match_spec,
                 num_entries_per_block,
                 &exp_idx,
                 spec_offsets,
                 spec_lens,
                 shift);

  for (i = 0; i < rangetbl_ptr->fieldcount; i++) {
    PIPE_MGR_FREE(_range_start[i]);
    PIPE_MGR_FREE(_range_end[i]);
  }

  return rc;
}

pipe_status_t pipe_mgr_entry_format_tof_range_max_expansion_entry_count_get(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint32_t *count) {
  uint32_t bj_hash;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_rangetbl_t *rangetbl_ptr;
  pipemgr_tbl_pkg_rangetbl_field_t *rangefield_ptr;
  uint32_t expansion_entry_count = 0, max_expansion_entry_count = 0,
           maufieldcount = 0;
  uint32_t i = 0;

  *count = 1;

  if (!PIPE_MGR_TBL_PKG_CTX(devid, prof_id).rangetbl_lut) {
    return PIPE_SUCCESS;
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).rangetbl_lut_depth,
      mat_tbl_hdl,
      0,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry_no_logging(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).rangetbl_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).rangetbl_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    // LOG_ERROR(
    //    "%s: Unable to find range table details for match "
    //    "handle "
    //    "0x%x",
    //    __func__,
    //    mat_tbl_hdl);
    // PIPE_MGR_DBGCHK(0);
    // return (PIPE_INVALID_ARG);

    // Range related operations are checked on tables that may not
    // support range.
    // If match-table handle is not in range tables, return success.
    return PIPE_SUCCESS;
  }

  rangetbl_ptr = lut_ptr->u.rangetbl_ptr;
  rangefield_ptr = rangetbl_ptr->fields;

  for (i = 0; i < rangetbl_ptr->fieldcount; i++) {
    maufieldcount = rangefield_ptr->maufieldcount;
    expansion_entry_count = (2 * maufieldcount) - 1;
    if (expansion_entry_count > max_expansion_entry_count) {
      max_expansion_entry_count = expansion_entry_count;
    }
    rangefield_ptr++;
    rangefield_ptr =
        (pipemgr_tbl_pkg_rangetbl_field_t
             *)((uint8_t *)rangefield_ptr +
                (maufieldcount * sizeof(pipemgr_tbl_pkg_rangetbl_mau_field_t)));
  }

  *count = max_expansion_entry_count;
  return PIPE_SUCCESS;
}

/* Create bit mask of required bits only */
static uint32_t get_bit_mask(int lsb, int msb) {
  uint32_t mask = 0;
  int idx = 0;

  for (idx = lsb; idx <= msb; idx++) {
    mask |= (1u << idx);
  }

  return (mask);
}

/* Populate the phv spec to program
 * phv_spec must be initialized to all 1's */
static pipe_status_t tof_snapshot_populate_phv_spec(
    pipe_mgr_phv_spec_t *phv_spec,
    pipe_mgr_phv_spec_t *phv_words_updated,
    int word_bit_width,
    int container_num,
    uint32_t field_val,
    uint32_t field_mask,
    bool val_valid,
    int phv_lsb,
    int phv_msb) {
  uint32_t bit_mask = 0;
  int cntr_idx = 0;

  bit_mask = get_bit_mask(phv_lsb, phv_msb);

  if (word_bit_width == 32) {
    cntr_idx = container_num - phv_spec->base_32;
    phv_spec[0].phvs32bit_lo[cntr_idx] ^=
        (~field_val & 0xffff) & (bit_mask & 0xffff);
    phv_spec[0].phvs32bit_hi[cntr_idx] ^=
        ((~field_val >> 16) & 0xffff) & (bit_mask >> 16);
    if (val_valid) {
      phv_spec[0].phvs32bit_hi[cntr_idx] &= ~(0x10000);  // set to 0 to match
    }
    phv_spec[1].phvs32bit_lo[cntr_idx] ^=
        (~field_mask & 0xffff) & (bit_mask & 0xffff);
    phv_spec[1].phvs32bit_hi[cntr_idx] ^=
        ((~field_mask >> 16) & 0xffff) & (bit_mask >> 16);
    phv_spec[1].phvs32bit_hi[cntr_idx] |=
        0x10000;  // valid bit match (w0, w1)=>(0, 1)
    phv_words_updated->phvs32bit_lo[cntr_idx] = 1;
    phv_words_updated->phvs32bit_hi[cntr_idx] = 1;
  } else if (word_bit_width == 16) {
    cntr_idx = container_num - phv_spec->base_16;
    phv_spec[0].phvs16bit[cntr_idx] ^= (~field_val) & bit_mask;
    if (val_valid) {
      phv_spec[0].phvs16bit[cntr_idx] &= ~(0x10000);  // set to 0 to match
    }
    phv_spec[1].phvs16bit[cntr_idx] ^= (~field_mask) & bit_mask;
    phv_spec[1].phvs16bit[cntr_idx] |= 0x10000;  // valid bit match
    phv_words_updated->phvs16bit[cntr_idx] = 1;
  } else if (word_bit_width == 8) {
    cntr_idx = container_num - phv_spec->base_8;
    phv_spec[0].phvs8bit[cntr_idx] ^= (~field_val) & bit_mask;
    if (val_valid) {
      phv_spec[0].phvs8bit[cntr_idx] &=
          ~(0x100);  // set to 1 to make it don't care
    }
    phv_spec[1].phvs8bit[cntr_idx] ^= (~field_mask) & bit_mask;
    phv_spec[1].phvs8bit[cntr_idx] |= 0x100;  // valid bit match
    phv_words_updated->phvs8bit[cntr_idx] = 1;
  } else {
    PIPE_MGR_DBGCHK(0);
    return PIPE_UNEXPECTED;
  }

  return 0;
}

pipe_status_t pipe_mgr_ctxjson_snapshot_encode(
    bf_dev_family_t family,
    uint8_t stage_id,
    int direction,
    pipe_snap_stage_field_info_t *field_details,
    pipe_snap_trig_field_info_t *field_info,
    pipe_mgr_phv_spec_t *phv_spec,  // array of 2 (key/mask)
    pipe_mgr_phv_spec_t *phv_words_updated) {
  uint8_t *field_val_ptr = NULL, *field_mask_ptr = NULL;
  uint8_t *data_ptr = NULL, *mask_ptr = NULL;
  uint32_t field_val = 0, field_mask = 0;

  (void)stage_id;
  (void)direction;

  field_val_ptr = (uint8_t *)&field_val;
  field_mask_ptr = (uint8_t *)&field_mask;

  data_ptr = (uint8_t *)&(field_info->value[0]);
  mask_ptr = (uint8_t *)&(field_info->mask[0]);

  /* lsb bit is from other side */
  set_key_mask_default_encode(
      field_val_ptr,
      field_mask_ptr,
      field_details->phv_lsb,
      field_details->phv_msb - field_details->phv_lsb + 1,
      data_ptr,
      mask_ptr,
      (field_info->width * 8) - field_details->field_msb - 1,
      field_details->field_msb - field_details->field_lsb + 1,
      0,
      0);

  field_val = le32toh(field_val);
  field_mask = le32toh(field_mask);
  switch (family) {
    case BF_DEV_FAMILY_TOFINO:
    case BF_DEV_FAMILY_TOFINO2:
    case BF_DEV_FAMILY_TOFINO3:
      tof_snapshot_populate_phv_spec(phv_spec,
                                     phv_words_updated,
                                     field_details->container_width,
                                     field_details->container_num,
                                     field_val,
                                     field_mask,
                                     true,
                                     field_details->phv_lsb,
                                     field_details->phv_msb);

      return PIPE_SUCCESS;
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }
}

pipe_status_t pipe_mgr_ctxjson_phv_fields_dict_get(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    dev_stage_t stage,
    bf_snapshot_dir_t dir,
    pipe_snap_stage_field_info_t *dict,
    uint32_t *num_fields) {
  uint32_t bj_hash, i, k;
  int j;
  int phv_numb_limit;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_snapshot_t *snapshot_ptr;
  pipemgr_tbl_pkg_snapshot_phv_t *phv_recs;
  pipemgr_tbl_pkg_snapshot_pov_t *pov_hdrs;

  // PHV fields for all stages is the same.
  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth, 0, stage + 1, 0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut,
      0,
      stage + 1);
  if (!lut_ptr) {
    LOG_ERROR("%s: Unable to find phv details from context json", __func__);
    return (PIPE_INVALID_ARG);
  }

  snapshot_ptr = lut_ptr->u.snapshot_ptr;
  phv_recs = snapshot_ptr->phv_recs;
  k = 0;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      phv_numb_limit = 224;
      break;
    case BF_DEV_FAMILY_TOFINO2:
      phv_numb_limit = 280;
      break;
    case BF_DEV_FAMILY_TOFINO3:
      phv_numb_limit = 280;
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }
  for (i = 0; i < snapshot_ptr->total_phv_recs; i++) {
    if (phv_recs->direction != dir) {
      phv_recs++;
      continue;
    }

    if (phv_recs->phvnumber >= phv_numb_limit) {
      // if tagalong phv record is in mau context json, snapshot data
      // is not in tag along phv. Skip.
      phv_recs++;
      continue;
    }
    dict->valid = true;
    strncpy(dict->name,
            pipemgr_tbl_pkg_get_field_string_name(
                devid, prof_id, phv_recs->phvname_str_index),
            PIPE_SNAP_TRIG_FIELD_NAME_LEN - 1);
    dict->name[PIPE_SNAP_TRIG_FIELD_NAME_LEN - 1] = '\0';
    dict->container_num = phv_recs->phvnumber;
    dict->container_width = phv_recs->container_width;
    dict->container_type = phv_recs->container_type;
    dict->field_lsb = phv_recs->fieldlsb;
    dict->field_msb = phv_recs->fieldmsb;
    dict->phv_lsb = phv_recs->phvlsb;
    dict->phv_msb = phv_recs->phvmsb;
    pov_hdrs = phv_recs->pov_hdrs;
    if (pov_hdrs) {
      for (j = 0; j < pov_hdrs->pov_bit_count; j++) {
        if (pov_hdrs->hidden[j]) {
          continue;
        }
        dict->valid = true;
        strncpy(dict->name,
                pipemgr_tbl_pkg_get_field_string_name(
                    devid, prof_id, pov_hdrs->pov_hdr_str_index[j]),
                PIPE_SNAP_TRIG_FIELD_NAME_LEN - 1);
        strcat(dict->name, "_valid");
        dict->name[PIPE_SNAP_TRIG_FIELD_NAME_LEN - 1] = '\0';
        dict->container_num = phv_recs->phvnumber;
        dict->container_width = phv_recs->container_width;
        dict->container_type = phv_recs->container_type;
        dict->field_lsb = 0;
        dict->field_msb = 0;
        dict->phv_lsb = pov_hdrs->pov_bit[j];
        dict->phv_msb = pov_hdrs->pov_bit[j];
        dict->byte_offset = pov_hdrs->position_offset[j];
        dict++;
        k++;
      }
    } else {
      dict->byte_offset = phv_recs->position_offset;
      dict++;
      k++;
    }
    phv_recs++;
  }
  *num_fields = k;
  return (PIPE_SUCCESS);
}

pipe_status_t pipe_mgr_ctxjson_phv_fields_dict_size_get(bf_dev_id_t devid,
                                                        profile_id_t prof_id,
                                                        dev_stage_t stage,
                                                        bf_snapshot_dir_t dir,
                                                        uint32_t *num_fields) {
  uint32_t bj_hash, i, k;
  int j;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_snapshot_t *snapshot_ptr;
  pipemgr_tbl_pkg_snapshot_phv_t *phv_recs;
  pipemgr_tbl_pkg_snapshot_pov_t *pov_hdrs;
  int phv_numb_limit;
  // PHV fields for all stages is the same.
  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth, 0, stage + 1, 0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut,
      0,
      stage + 1);
  if (!lut_ptr) {
    LOG_ERROR("%s: Unable to find phv details from context json", __func__);
    return (PIPE_INVALID_ARG);
  }

  snapshot_ptr = lut_ptr->u.snapshot_ptr;
  phv_recs = snapshot_ptr->phv_recs;
  k = 0;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      phv_numb_limit = 224;
      break;
    case BF_DEV_FAMILY_TOFINO2:
      phv_numb_limit = 280;
      break;
    case BF_DEV_FAMILY_TOFINO3:
      phv_numb_limit = 280;
      break;





    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }
  for (i = 0; (i < snapshot_ptr->total_phv_recs); i++) {
    if (phv_recs->direction != dir) {
      phv_recs++;
      continue;
    }
    if (phv_recs->phvnumber >= phv_numb_limit) {
      // if tagalong phv record is in mau context json, snapshot data
      // is not in tag along phv. Skip.
      phv_recs++;
      continue;
    }
    pov_hdrs = phv_recs->pov_hdrs;
    if (pov_hdrs) {
      for (j = 0; j < pov_hdrs->pov_bit_count; j++) {
        k++;
      }
    } else {
      k++;
    }

    phv_recs++;
  }
  *num_fields = k;
  return (PIPE_SUCCESS);
}

uint32_t pipe_mgr_ctxjson_phv_fields_dict_size(bf_dev_id_t devid,
                                               profile_id_t prof_id,
                                               bf_snapshot_dir_t dir) {
  uint32_t bj_hash, i, j;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_snapshot_t *snapshot_ptr;
  pipemgr_tbl_pkg_snapshot_phv_t *phv_recs;
  pipemgr_tbl_pkg_snapshot_pov_t *pov_hdrs;
  uint32_t largest_position_offset;
  uint32_t largest_position_offset_fieldwidth;
  uint32_t max_phv_dict_size = 0;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  /* Iterate including extra stage with iPHV for deparser */
  for (int k = 0; k <= dev_info->profile_info[prof_id]->num_stages; ++k) {
    bj_hash = bob_jenkin_hash_one_at_a_time(
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth, 0, k + 1, 0);
    lut_ptr = pipemgr_entry_format_get_lut_entry(
        bj_hash,
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth,
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut,
        0,
        k + 1);
    if (!lut_ptr) {
      LOG_ERROR(
          "%s: Unable to find phv details from context json, dev %d profile-id "
          "%d stage %d",
          __func__,
          devid,
          prof_id,
          k);
      return (PIPE_INVALID_ARG);
    }

    snapshot_ptr = lut_ptr->u.snapshot_ptr;
    phv_recs = snapshot_ptr->phv_recs;
    largest_position_offset = 0;
    largest_position_offset_fieldwidth = 0;
    for (i = 0; i < snapshot_ptr->total_phv_recs; i++) {
      if (phv_recs->direction != dir) {
        phv_recs++;
        continue;
      }
      if (phv_recs->position_offset > largest_position_offset) {
        largest_position_offset = phv_recs->position_offset;
        largest_position_offset_fieldwidth = phv_recs->fieldwidth;
      }
      pov_hdrs = phv_recs->pov_hdrs;
      if (pov_hdrs) {
        for (j = 0; j < pov_hdrs->pov_bit_count; j++) {
          if (pov_hdrs->position_offset[j] > largest_position_offset) {
            largest_position_offset = pov_hdrs->position_offset[j];
            largest_position_offset_fieldwidth = 1;
          }
        }
      }
      phv_recs++;
    }
    if ((largest_position_offset + largest_position_offset_fieldwidth) >
        max_phv_dict_size) {
      max_phv_dict_size =
          largest_position_offset + largest_position_offset_fieldwidth;
    }
  }
  return (max_phv_dict_size);
}

static pipe_status_t pipe_mgr_get_phv_field_byte_offset(bf_dev_id_t devid,
                                                        profile_id_t prof_id,
                                                        int direction,
                                                        char *fieldname,
                                                        uint32_t *offset) {
  // Search for phv/pov field name in snapshot dict.
  // If found return PIPE_SUCCESS and byte-offset.
  // If not found return ERROR
  uint32_t bj_hash, i;
  int j;
  char povfieldname[PIPE_SNAP_TRIG_FIELD_NAME_LEN];
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_snapshot_t *snapshot_ptr;
  pipemgr_tbl_pkg_snapshot_phv_t *phv_recs;
  pipemgr_tbl_pkg_snapshot_pov_t *pov_hdrs;
  char *pov_str;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  for (int k = 0; k <= dev_info->profile_info[prof_id]->num_stages; ++k) {
    bj_hash = bob_jenkin_hash_one_at_a_time(
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth, 0, k + 1, 0);
    lut_ptr = pipemgr_entry_format_get_lut_entry(
        bj_hash,
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth,
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut,
        0,
        k + 1);
    if (!lut_ptr) {
      LOG_ERROR(
          "%s: Unable to find phv details from context json, dev %d profile-id "
          "%d direction %s field %s stage %d",
          __func__,
          devid,
          prof_id,
          direction ? "egress" : "ingess",
          fieldname,
          k);
      return (PIPE_INVALID_ARG);
    }

    snapshot_ptr = lut_ptr->u.snapshot_ptr;
    phv_recs = snapshot_ptr->phv_recs;
    for (i = 0; i < snapshot_ptr->total_phv_recs; i++) {
      if (phv_recs->direction != direction) {
        phv_recs++;
        continue;
      }
      pov_hdrs = phv_recs->pov_hdrs;
      if (pov_hdrs) {
        for (j = 0; j < pov_hdrs->pov_bit_count; j++) {
          pov_str = pipemgr_tbl_pkg_get_field_string_name(
              devid, prof_id, pov_hdrs->pov_hdr_str_index[j]);
          snprintf(
              povfieldname, sizeof(povfieldname), "%s%s", pov_str, "_valid");
          if (!strcmp(fieldname, povfieldname)) {
            *offset = pov_hdrs->position_offset[j];
            return (PIPE_SUCCESS);
          }
        }
      } else {
        if (!strcmp(fieldname,
                    pipemgr_tbl_pkg_get_field_string_name(
                        devid, prof_id, phv_recs->phvname_str_index))) {
          *offset = phv_recs->position_offset;
          return (PIPE_SUCCESS);
        }
      }
      phv_recs++;
    }
  }
  return (PIPE_INVALID_ARG);
}

bool pipe_mgr_stage_match_dependent_get(bf_dev_id_t devid,
                                        profile_id_t profile_id,
                                        dev_stage_t stage,
                                        int dir) {
  unsigned long key = (stage << 1) | dir;
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return false;
  }
  if ((profile_id < 0) ||
      ((uint32_t)profile_id >= dev_info->num_pipeline_profiles)) {
    LOG_ERROR("%s: Unable to use profile_id %d", __func__, profile_id);
    PIPE_MGR_DBGCHK(0);
    return false;
  }
  pipe_mgr_stage_char_t *stage_characteristics;
  rmt_dev_profile_info_t *prof_info = dev_info->profile_info[profile_id];

  bf_map_sts_t rc = bf_map_get(
      &prof_info->stage_characteristics, key, (void **)&stage_characteristics);
  if (rc == BF_MAP_NO_KEY) {
    /* The key (stage id and gress) were not in the map.  This is expected for
     * Tofino-1 where we do not need to track stage dependencies or for Tofino-2
     * and later where the stage has a bypass configuration.  For these cases we
     * can return false for "not match dependent". */
    return false;
  }
  PIPE_MGR_DBGCHK(rc == 0);
  return stage_characteristics->match_dp;
}

static pipemgr_tbl_pkg_snapshot_t *pipe_mgr_get_snapshot_ptr(
    bf_dev_id_t devid, profile_id_t prof_id, uint8_t stage_id) {
  uint32_t bj_hash;
  pipemgr_tbl_pkg_lut_t *lut_ptr;

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth,
      0,
      stage_id + 1,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut,
      0,
      stage_id + 1);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find phv details from context json, dev %d profile-id "
        "%d stage %d",
        __func__,
        devid,
        prof_id,
        stage_id);
    return NULL;
  }

  return (lut_ptr->u.snapshot_ptr);
}

static pipe_status_t pipe_mgr_get_next_tbl_name(rmt_dev_info_t *dev_info,
                                                profile_id_t prof_id,
                                                int stage_id,
                                                uint32_t next_table_out,
                                                char *next_table) {
  int idx;
  pipemgr_tbl_pkg_snapshot_t *snapshot_ptr;
  pipemgr_tbl_pkg_logical_table_details_t *table_ptr;
  int max_len = BF_TBL_NAME_LEN;

  /* Get next table name */
  int next_tbl_stage =
      next_table_out / dev_info->dev_cfg.stage_cfg.num_logical_tables;
  PIPE_MGR_MEMSET(next_table, 0, max_len);
  if ((next_tbl_stage <= stage_id) ||
      (next_tbl_stage >= dev_info->profile_info[prof_id]->num_stages))
    return PIPE_SUCCESS;
  // Next table
  snapshot_ptr =
      pipe_mgr_get_snapshot_ptr(dev_info->dev_id, prof_id, next_tbl_stage);
  if (snapshot_ptr == NULL) return PIPE_OBJ_NOT_FOUND;
  table_ptr = snapshot_ptr->tables;

  for (idx = 0; idx < snapshot_ptr->total_tables; idx++) {
    if (table_ptr->logical_id == next_table_out) break;
    table_ptr++;
  }
  if (idx < snapshot_ptr->total_tables) {
    strncpy(&(next_table[0]),
            pipemgr_tbl_pkg_get_field_string_name(
                dev_info->dev_id, prof_id, table_ptr->tablename_str_index),
            max_len);
  } else {
    // did not find next table name
    return (PIPE_OBJ_NOT_FOUND);
  }

  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_get_enabled_next_tbl_names(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    int stage_id,
    uint16_t next_table_out,
    uint16_t enabled_next_tables,
    char *next_tbl_names) {
  int idx;
  pipemgr_tbl_pkg_snapshot_t *snapshot_ptr;
  pipemgr_tbl_pkg_logical_table_details_t *table_ptr;
  int max_len = BF_TBL_NAME_LEN * BF_MAX_LOG_TBLS;
  int c_len = 0;

  int next_tbl_stage =
      next_table_out / dev_info->dev_cfg.stage_cfg.num_logical_tables;
  PIPE_MGR_MEMSET(next_tbl_names, 0, max_len);
  if ((enabled_next_tables == 0) || (next_tbl_stage <= stage_id) ||
      (next_tbl_stage >= dev_info->profile_info[prof_id]->num_stages))
    return PIPE_SUCCESS;

  snapshot_ptr =
      pipe_mgr_get_snapshot_ptr(dev_info->dev_id, prof_id, next_tbl_stage);
  if (snapshot_ptr == NULL) return PIPE_OBJ_NOT_FOUND;
  table_ptr = snapshot_ptr->tables;

  for (idx = 0; idx < snapshot_ptr->total_tables; idx++) {
    uint8_t tbl_idx =
        table_ptr->logical_id % dev_info->dev_cfg.stage_cfg.num_logical_tables;
    if (enabled_next_tables & (0x1 << tbl_idx))
      c_len += snprintf(
          next_tbl_names + c_len,
          (c_len < max_len) ? (max_len - c_len - 1) : 0,
          "%s ",
          pipemgr_tbl_pkg_get_field_string_name(
              dev_info->dev_id, prof_id, table_ptr->tablename_str_index));
    table_ptr++;
  }

  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_get_gbl_exec_tbl_names(rmt_dev_info_t *dev_info,
                                                     profile_id_t prof_id,
                                                     uint8_t stage_id,
                                                     uint16_t gbl_exec_out,
                                                     char *tbl_names) {
  int idx;
  pipemgr_tbl_pkg_snapshot_t *snapshot_ptr;
  pipemgr_tbl_pkg_logical_table_details_t *table_ptr;
  int c_len = 0;
  int max_len = BF_MAX_LOG_TBLS * BF_TBL_NAME_LEN;
  int tbl_idx;
  int next_stage = stage_id + 1;

  PIPE_MGR_MEMSET(tbl_names, 0, BF_MAX_LOG_TBLS * BF_TBL_NAME_LEN);
  if ((next_stage >= dev_info->profile_info[prof_id]->num_stages) ||
      (gbl_exec_out == 0))
    return PIPE_SUCCESS;

  snapshot_ptr =
      pipe_mgr_get_snapshot_ptr(dev_info->dev_id, prof_id, next_stage);
  if (snapshot_ptr == NULL) return PIPE_OBJ_NOT_FOUND;
  table_ptr = snapshot_ptr->tables;
  for (idx = 0; idx < snapshot_ptr->total_tables; idx++) {
    tbl_idx =
        table_ptr->logical_id % dev_info->dev_cfg.stage_cfg.num_logical_tables;
    if ((0x1 << tbl_idx) & gbl_exec_out) {
      c_len += snprintf(
          tbl_names + c_len,
          (c_len < max_len) ? (max_len - c_len - 1) : 0,
          "%s ",
          pipemgr_tbl_pkg_get_field_string_name(
              dev_info->dev_id, prof_id, table_ptr->tablename_str_index));
    }
    table_ptr++;
  }
  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_get_pred_long_branch_tbl_names(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    uint8_t stage_id,
    uint32_t long_branch_out,
    pipe_mgr_snapshot_long_branch_t *long_branch,
    char *tbl_names) {
  int idx;
  pipemgr_tbl_pkg_snapshot_t *snapshot_ptr;
  pipemgr_tbl_pkg_logical_table_details_t *table_ptr;
  int c_len = 0;
  int max_len = BF_MAX_LOG_TBLS * BF_TBL_NAME_LEN;
  int tbl_idx;
  int next_stage = stage_id + 1;

  PIPE_MGR_MEMSET(tbl_names, 0, BF_MAX_LOG_TBLS * BF_TBL_NAME_LEN);
  if ((next_stage >= dev_info->profile_info[prof_id]->num_stages) ||
      (long_branch_out == 0))
    return PIPE_SUCCESS;

  for (int stage = next_stage;
       stage < dev_info->profile_info[prof_id]->num_stages && long_branch_out;
       stage++) {
    int data_idx = stage - long_branch->s_stage;
    snapshot_ptr = pipe_mgr_get_snapshot_ptr(dev_info->dev_id, prof_id, stage);
    if (snapshot_ptr == NULL) return PIPE_OBJ_NOT_FOUND;
    for (idx = 0; idx < snapshot_ptr->total_tables; idx++) {
      table_ptr = snapshot_ptr->tables + idx;
      tbl_idx = table_ptr->logical_id %
                dev_info->dev_cfg.stage_cfg.num_logical_tables;
      if (long_branch->data[data_idx].pred_lt_src[tbl_idx] ==
          PIPE_MGR_TOF2_INVALID_LT_SRC)
        continue;
      if (long_branch_out &
          (0x1 << long_branch->data[data_idx].pred_lt_src[tbl_idx])) {
        c_len += snprintf(
            tbl_names + c_len,
            (c_len < max_len) ? (max_len - c_len - 1) : 0,
            "%s ",
            pipemgr_tbl_pkg_get_field_string_name(
                dev_info->dev_id, prof_id, table_ptr->tablename_str_index));
      }
    }
    long_branch_out &= ~long_branch->data[data_idx].terminate;
  }
  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_get_enabled_long_branch_tbl_names(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    uint8_t stage_id,
    uint32_t long_branch_out,
    pipe_mgr_snapshot_long_branch_t *long_branch,
    char *tbl_names) {
  int idx;
  pipemgr_tbl_pkg_snapshot_t *snapshot_ptr;
  pipemgr_tbl_pkg_logical_table_details_t *table_ptr;
  int c_len = 0;
  int max_len = BF_MAX_LOG_TBLS * BF_TBL_NAME_LEN;
  int tbl_idx;
  int next_stage = stage_id + 1;

  PIPE_MGR_MEMSET(tbl_names, 0, BF_MAX_LOG_TBLS * BF_TBL_NAME_LEN);
  if ((next_stage >= dev_info->profile_info[prof_id]->num_stages) ||
      (long_branch_out == 0))
    return PIPE_SUCCESS;

  for (int stage = next_stage;
       stage < dev_info->profile_info[prof_id]->num_stages && long_branch_out;
       stage++) {
    int data_idx = stage - long_branch->s_stage;
    uint16_t enabled_local_tables = 0;
    for (int i = 0; i < PIPE_MGR_NUM_LONG_BRCH; i++) {
      if (long_branch_out & (0x1 << i))
        enabled_local_tables |= long_branch->data[data_idx].mpr_lut[i];
    }
    snapshot_ptr = pipe_mgr_get_snapshot_ptr(dev_info->dev_id, prof_id, stage);
    if (snapshot_ptr == NULL) return PIPE_OBJ_NOT_FOUND;
    for (idx = 0; idx < snapshot_ptr->total_tables; idx++) {
      table_ptr = snapshot_ptr->tables + idx;
      tbl_idx = table_ptr->logical_id %
                dev_info->dev_cfg.stage_cfg.num_logical_tables;

      if (enabled_local_tables & (0x1 << tbl_idx)) {
        c_len += snprintf(
            tbl_names + c_len,
            (c_len < max_len) ? (max_len - c_len - 1) : 0,
            "%s ",
            pipemgr_tbl_pkg_get_field_string_name(
                dev_info->dev_id, prof_id, table_ptr->tablename_str_index));
      }
    }
    long_branch_out &= ~long_branch->data[data_idx].terminate;
  }
  return PIPE_SUCCESS;
}

static pipe_status_t pipe_mgr_get_meter_alu_info(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    bf_dev_pipe_t pipe,
    uint8_t stage_id,
    pipe_mgr_snapshot_capture_data_t *pipe_capture,
    bf_snapshot_capture_ctrl_info_t *pd_ctrl) {
  int idx, ref_idx, tbl_idx, alu_idx;
  pipe_mat_tbl_info_t *tbl_info;
  bf_dev_id_t devid = dev_info->dev_id;
  uint8_t ctrl_info = 0;
  pipemgr_tbl_pkg_logical_table_details_t *table_ptr;
  pipemgr_tbl_pkg_snapshot_t *snapshot_ptr = NULL;

  sel_tbl_t *sel_tbl = NULL;
  sel_tbl_info_t *sel_tbl_info = NULL;
  sel_tbl_stage_info_t *sel_stage_info = NULL;

  pipe_mgr_meter_tbl_t *meter_tbl = NULL;
  pipe_mgr_meter_tbl_instance_t *meter_tbl_instance = NULL;
  pipe_mgr_meter_tbl_stage_info_t *meter_tbl_stage_info = NULL;
  bf_dev_pipe_t pipe_tmp = pipe;

  struct pipe_mgr_stful_tbl *stful_tbl = NULL;
  struct pipe_mgr_stful_tbl_stage_info *stful_stage_info = NULL;
  uint32_t meter_alu_tbl_handle[4] = {0, 0, 0, 0};
  pipe_snap_meter_alu_type_t alu_type[4] = {
      METER_ALU_NONE, METER_ALU_NONE, METER_ALU_NONE, METER_ALU_NONE};
  snapshot_ptr = pipe_mgr_get_snapshot_ptr(devid, prof_id, stage_id);
  if (snapshot_ptr == NULL) return PIPE_OBJ_NOT_FOUND;
  table_ptr = snapshot_ptr->tables;
  for (idx = 0; idx < snapshot_ptr->total_tables; idx++) {
    tbl_idx =
        table_ptr->logical_id % dev_info->dev_cfg.stage_cfg.num_logical_tables;
    tbl_info = pipe_mgr_get_tbl_info(
        devid, pd_ctrl->tables_info[tbl_idx].table_handle, __func__, __LINE__);
    if (tbl_info == NULL) {
      continue;
    }
    // sel:1
    ref_idx = tbl_info->num_sel_tbl_refs;
    while (ref_idx > 0) {
      sel_tbl_info = pipe_mgr_sel_tbl_info_get(
          devid, tbl_info->sel_tbl_ref[ref_idx - 1].tbl_hdl, false);
      if (sel_tbl_info == NULL) {
        ref_idx--;
        continue;
      }
      /* Just take the first table instance.  Doesn't matter if the table is
       * symmetric or asymmetric, the ALU info is the same regardless. */
      sel_tbl = &(sel_tbl_info->sel_tbl[0]);
      if (!sel_tbl) {
        ref_idx--;
        continue;
      }
      for (int i = 0; i < sel_tbl->num_stages; ++i) {
        if (sel_tbl->sel_tbl_stage_info[i].stage_id == stage_id) {
          sel_stage_info = &(sel_tbl->sel_tbl_stage_info[i]);
          break;
        }
      }
      if (sel_stage_info == NULL) {
        ref_idx--;
        continue;
      }
      for (alu_idx = 0; alu_idx < sel_stage_info->num_alu_ids; alu_idx++) {
        if (sel_stage_info->alu_ids[alu_idx] < 4) {
          if ((alu_type[sel_stage_info->alu_ids[alu_idx]] != METER_ALU_NONE) &&
              (alu_type[sel_stage_info->alu_ids[alu_idx]] != METER_ALU_SEL))
            return PIPE_INIT_ERROR;
          alu_type[sel_stage_info->alu_ids[alu_idx]] = METER_ALU_SEL;
          meter_alu_tbl_handle[sel_stage_info->alu_ids[alu_idx]] =
              tbl_info->sel_tbl_ref[ref_idx - 1].tbl_hdl;
        }
      }
      ref_idx--;
    }
    // meter:2
    ref_idx = tbl_info->num_meter_tbl_refs;
    while (ref_idx > 0) {
      meter_tbl = pipe_mgr_meter_tbl_get(
          devid, tbl_info->meter_tbl_ref[ref_idx - 1].tbl_hdl);
      if (!meter_tbl) {
        LOG_ERROR(
            "%s:%d Unable to find meter table for handle %d and device %d",
            __func__,
            __LINE__,
            tbl_info->meter_tbl_ref[ref_idx - 1].tbl_hdl,
            devid);
        return (PIPE_OBJ_NOT_FOUND);
      }

      if (meter_tbl->symmetric) {
        pipe_tmp = BF_DEV_PIPE_ALL;
      }
      meter_tbl_instance = pipe_mgr_meter_tbl_get_instance(meter_tbl, pipe_tmp);
      if (meter_tbl_instance == NULL) {
        ref_idx--;
        continue;
      }
      meter_tbl_stage_info =
          pipe_mgr_meter_tbl_get_stage_info(meter_tbl_instance, stage_id);
      if (meter_tbl_stage_info == NULL) {
        ref_idx--;
        continue;
      }
      if (sel_stage_info == NULL) {
        ref_idx--;
        continue;
      }
      for (alu_idx = 0; alu_idx < sel_stage_info->num_alu_ids; alu_idx++) {
        if (meter_tbl_stage_info->alu_ids[alu_idx] < 4) {
          if ((alu_type[meter_tbl_stage_info->alu_ids[alu_idx]] !=
               METER_ALU_NONE) &&
              (alu_type[meter_tbl_stage_info->alu_ids[alu_idx]] !=
               METER_ALU_METER))
            return PIPE_INIT_ERROR;
          alu_type[meter_tbl_stage_info->alu_ids[alu_idx]] = METER_ALU_METER;
          meter_alu_tbl_handle[meter_tbl_stage_info->alu_ids[alu_idx]] =
              tbl_info->meter_tbl_ref[ref_idx - 1].tbl_hdl;
        }
      }
      ref_idx--;
    }
    // stful:3
    ref_idx = tbl_info->num_stful_tbl_refs;
    while (ref_idx > 0) {
      bf_map_sts_t msts;
      msts = pipe_mgr_stful_tbl_map_get(
          devid,
          tbl_info->stful_tbl_ref[ref_idx - 1].tbl_hdl,
          (void **)&stful_tbl);
      if (BF_MAP_OK != msts) {
        ref_idx--;
        continue;
      }
      if (stful_tbl == NULL) {
        ref_idx--;
        continue;
      }
      // stful_tbl = stful_tbl_lkup(devid,
      // tbl_info->stful_tbl_ref[ref_idx-1].tbl_hdl);
      // if (stful_tbl == NULL) {ref_idx--;continue;}
      for (int i = 0; i < stful_tbl->num_stages; ++i) {
        if (stful_tbl->stages[i].stage_id == stage_id) {
          stful_stage_info = &(stful_tbl->stages[i]);
          break;
        }
      }
      if (!stful_stage_info) {
        ref_idx--;
        continue;
      }
      for (alu_idx = 0; alu_idx < stful_stage_info->num_alu_ids; ++alu_idx) {
        if (stful_stage_info->alu_ids[alu_idx] < 4) {
          if ((alu_type[stful_stage_info->alu_ids[alu_idx]] !=
               METER_ALU_NONE) &&
              (alu_type[stful_stage_info->alu_ids[alu_idx]] != METER_ALU_STFUL))
            return PIPE_INIT_ERROR;
          alu_type[stful_stage_info->alu_ids[alu_idx]] = METER_ALU_STFUL;
          meter_alu_tbl_handle[stful_stage_info->alu_ids[alu_idx]] =
              tbl_info->stful_tbl_ref[ref_idx - 1].tbl_hdl;
        }
      }
      ref_idx--;
    }
  }
  // decode ctrl info
  for (idx = 0; idx < 4; idx++) {
    pd_ctrl->meter_alu_info[idx].ctrl_info_p = NULL;
    pd_ctrl->meter_alu_info[idx].table_handle = 0;
    if ((pipe_capture->meter_adr[idx] == 0) ||
        (alu_type[idx] == METER_ALU_NONE))
      continue;
    ctrl_info = (pipe_capture->meter_adr[idx] >> 23) & 0xf;
    // entry_adr = pipe_capture->meter_adr[idx] & 0x7fffff;
    switch (alu_type[idx]) {
      case METER_ALU_SEL:
        pd_ctrl->meter_alu_info[idx].ctrl_info_p =
            pipe_mgr_sel_ctrl_info_strings[ctrl_info];
        pd_ctrl->meter_alu_info[idx].table_handle = meter_alu_tbl_handle[idx];
        break;
      case METER_ALU_STFUL:
        pd_ctrl->meter_alu_info[idx].ctrl_info_p =
            pipe_mgr_stful_ctrl_info_strings[ctrl_info];
        pd_ctrl->meter_alu_info[idx].table_handle = meter_alu_tbl_handle[idx];
        break;
      case METER_ALU_METER:
        pd_ctrl->meter_alu_info[idx].ctrl_info_p =
            pipe_mgr_meter_ctrl_info_strings[ctrl_info];
        pd_ctrl->meter_alu_info[idx].table_handle = meter_alu_tbl_handle[idx];
        break;
      default:
        return PIPE_OBJ_NOT_FOUND;
    }
  }
  return PIPE_SUCCESS;
}
/* Decode phvs to p4 fields */
// This function should not invoked without creating phv field dict
// using function pipe_mgr_ctxjson_phv_fields_dict_get()
pipe_status_t pipe_mgr_ctxjson_snapshot_decode(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    bf_dev_pipe_t pipe,
    uint8_t stage_id,
    int direction,
    pipe_mgr_snapshot_capture_data_t *pipe_capture,
    bf_snapshot_capture_ctrl_info_t *pd_ctrl,
    uint8_t *pd_capture,
    uint8_t *pd_capture_v) {
  pipe_status_t rc = PIPE_SUCCESS;
  bf_dev_id_t devid = dev_info->dev_id;
  uint32_t field_val = 0, byteoffset;
  uint8_t *data_ptr = NULL;
  uint8_t *field_val_ptr = NULL;
  int idx = 0;
  uint32_t datapath = 0, tcam_addr_shift = 0;
  uint32_t decode_data = 0;
  uint32_t phy_bus_arr[PIPE_MGR_MAX_PHY_BUS], phy_bus = 0;
  int cntr = 0;
  pipe_mgr_phv_spec_t *phv_spec = NULL;
  uint32_t i, j, width, tbl_idx;
  pipemgr_tbl_pkg_snapshot_t *snapshot_ptr;
  pipemgr_tbl_pkg_snapshot_phv_t *phv_recs;
  pipemgr_tbl_pkg_snapshot_pov_t *pov_hdrs;
  pipemgr_tbl_pkg_logical_table_details_t *table_ptr;
  char povfieldname[PIPE_SNAP_TRIG_FIELD_NAME_LEN];
  phv_spec = &(pipe_capture->phv_spec);
  field_val_ptr = (uint8_t *)&field_val;

  // For PHVs take stage_id + 1
  snapshot_ptr = pipe_mgr_get_snapshot_ptr(devid, prof_id, stage_id + 1);
  if (snapshot_ptr == NULL) return PIPE_OBJ_NOT_FOUND;
  phv_recs = snapshot_ptr->phv_recs;
  for (i = 0; i < snapshot_ptr->total_phv_recs; i++) {
    if (phv_recs->direction != direction) {
      phv_recs++;
      continue;
    }
    if (phv_recs->phvnumber >= phv_spec->phv_count) {
      // if tagalong phv record is in mau context json, snapshot data
      // is not in tag along phv. Skip.
      phv_recs++;
      continue;
    }
    if (phv_recs->container_width == 32) {
      field_val =
          (phv_spec->phvs32bit_lo[phv_recs->phvnumber - phv_spec->base_32] &
           0xffff) |
          (phv_spec->phvs32bit_hi[phv_recs->phvnumber - phv_spec->base_32]
           << 16);
    } else if (phv_recs->container_width == 16) {
      field_val =
          phv_spec->phvs16bit[phv_recs->phvnumber - phv_spec->base_16] & 0xffff;
    } else if (phv_recs->container_width == 8) {
      field_val =
          phv_spec->phvs8bit[phv_recs->phvnumber - phv_spec->base_8] & 0xff;
    } else {
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
    }
    if (dev_info->dev_family == BF_DEV_FAMILY_TOFINO && pd_capture_v) {
      uint32_t bit_offset = phv_recs->phvnumber % 8;
      uint8_t validity = 0x0;
      switch (phv_recs->container_width) {
        case 32:
          validity =
              (phv_spec->phvs32bit_hi[phv_recs->phvnumber - phv_spec->base_32] &
               0x10000)
                  ? (0x1 << bit_offset)
                  : (0x0 << bit_offset);
          break;
        case 16:
          validity =
              (phv_spec->phvs16bit[phv_recs->phvnumber - phv_spec->base_16] &
               0x10000)
                  ? (0x1 << bit_offset)
                  : (0x0 << bit_offset);
          break;
        case 8:
          validity =
              (phv_spec->phvs8bit[phv_recs->phvnumber - phv_spec->base_8] &
               0x100)
                  ? (0x1 << bit_offset)
                  : (0x0 << bit_offset);
          break;
        default:
          PIPE_MGR_DBGCHK(0);
          return PIPE_UNEXPECTED;
      }
      j = phv_recs->phvnumber / 8;
      pd_capture_v[j] &= ~(validity);
      pd_capture_v[j] |= validity;
    }

    if (phv_recs->pov_hdrs) {
      pov_hdrs = phv_recs->pov_hdrs;
      for (j = 0; j < pov_hdrs->pov_bit_count; j++) {
        if (pov_hdrs->hidden[j]) {
          continue;
        }
        snprintf(povfieldname,
                 sizeof(povfieldname),
                 "%s%s",
                 pipemgr_tbl_pkg_get_field_string_name(
                     devid, prof_id, pov_hdrs->pov_hdr_str_index[j]),
                 "_valid");
        rc = pipe_mgr_get_phv_field_byte_offset(
            devid, prof_id, direction, povfieldname, &byteoffset);
        if (rc == PIPE_SUCCESS) {
          data_ptr = pd_capture + byteoffset;
          *data_ptr = (field_val >> pov_hdrs->pov_bit[j]) & 0x1;
        } else {
          PIPE_MGR_DBGCHK(0);
          return PIPE_UNEXPECTED;
        }
      }
    } else {
      field_val = le32toh(field_val);
      rc = pipe_mgr_get_phv_field_byte_offset(
          devid,
          prof_id,
          direction,
          pipemgr_tbl_pkg_get_field_string_name(
              devid, prof_id, phv_recs->phvname_str_index),
          &byteoffset);
      if (rc == PIPE_SUCCESS) {
        data_ptr = pd_capture + byteoffset;
        // When field is in multiple PHVs, copy at correct offset inside
        // p4-field allocated memory.
        width = phv_recs->container_width / 8;  // container width is in bits
        decode_copy_bits_snapshot(
            (uint8_t *)&decode_data, 0, 8 * width, field_val_ptr, 0, 8 * width);

        if (width == 2) {
#ifdef LITTLE_ENDIAN_CALLER
          uint16_t tmp = decode_data;
          decode_data = ntohs(tmp);
#else
          decode_data = decode_data;
#endif
        }
        if (width == 4) {
#ifdef LITTLE_ENDIAN_CALLER
          decode_data = ntohl(decode_data);
#endif
        }
#ifndef LITTLE_ENDIAN_CALLER
        // Special handling for big endian systems
        decode_data = le32toh(decode_data);
        if (width == 2) {
          uint16_t tmp = decode_data & 0xffff;
          decode_data = le16toh(tmp);
        }
#endif
        // Fetch only those bits relevant to field.
        decode_data = decode_data >> phv_recs->phvlsb;
        decode_data = decode_data &
                      ((1ull << (phv_recs->phvmsb - phv_recs->phvlsb + 1)) - 1);

        int count;
        uint8_t bit_shift = phv_recs->fieldlsb % 8;
        uint64_t data_shifted = (uint64_t)decode_data << bit_shift;

        if (phv_recs->fieldwidth <= 4) {
          data_ptr += (phv_recs->fieldlsb / 8);
          for (count = 0;
               count <
               (((phv_recs->phvmsb - phv_recs->phvlsb + bit_shift) / 8) + 1);
               count++) {
            ((uint8_t *)data_ptr)[count] |=
                (data_shifted >> (count * 8)) & 0xFF;
          }
        } else {
          int off = phv_recs->fieldwidth - (phv_recs->fieldlsb / 8) - 1;
          for (count = 0;
               count <
               (((phv_recs->phvmsb - phv_recs->phvlsb + bit_shift) / 8) + 1);
               count++) {
            ((uint8_t *)data_ptr)[off - count] |=
                (data_shifted >> (count * 8)) & 0xFF;
          }
        }
      } else {
        PIPE_MGR_DBGCHK(0);
        return PIPE_UNEXPECTED;
        ;
      }
    }
    phv_recs++;
  }

  /* Copy the snapshot register info */
  datapath = pipe_capture->datapath_capture;
  pd_ctrl->prev_stage_trigger = (datapath >> direction) & 0x1;
  pd_ctrl->timer_trigger = (datapath >> 2) & 0x1;
  pd_ctrl->local_stage_trigger = (datapath >> (3 + direction)) & 0x1;

  /* For other info get actual stage */
  snapshot_ptr = pipe_mgr_get_snapshot_ptr(devid, prof_id, stage_id);
  if (snapshot_ptr == NULL) return PIPE_OBJ_NOT_FOUND;
  table_ptr = snapshot_ptr->tables;

  for (idx = 0; idx < snapshot_ptr->total_tables; idx++) {
    /* Determine table type */
    tbl_idx =
        table_ptr->logical_id % dev_info->dev_cfg.stage_cfg.num_logical_tables;
    pd_ctrl->tables_info[tbl_idx].table_handle = table_ptr->table_handle;
    if (table_ptr->table_type_tcam) {
      pd_ctrl->tables_info[tbl_idx].table_type_tcam = 1;
      tcam_addr_shift = table_ptr->tcam_addr_shift;
    } else {
      pd_ctrl->tables_info[tbl_idx].table_type_tcam = 0;
      tcam_addr_shift = 0;
    }

    /* Get table hit-address */
    PIPE_MGR_MEMSET(phy_bus_arr, 0, sizeof(phy_bus_arr));

    /* Phy Address register is indexed by physical bus.
       Get list of physical bus from logical id
    */
    for (cntr = 0; cntr < table_ptr->inuse_physical_buses; cntr++) {
      phy_bus = table_ptr->physical_buses[cntr];
      if (phy_bus >= PIPE_MGR_MAX_PHY_BUS) {
        continue;
      }
      if (pd_ctrl->tables_info[tbl_idx].table_type_tcam) {
        if (pipe_capture->physical_tcam_hit_address[phy_bus] != 0) {
          pd_ctrl->tables_info[tbl_idx].match_hit_address =
              pipe_capture->physical_tcam_hit_address[phy_bus] >>
              tcam_addr_shift;
          break;
        }
      } else {
        if (pipe_capture->physical_exact_match_hit_address[phy_bus] != 0) {
          pd_ctrl->tables_info[tbl_idx].match_hit_address =
              pipe_capture->physical_exact_match_hit_address[phy_bus];
          break;
        }
      }
    }

    /* Get table name */
    PIPE_MGR_MEMSET(
        pd_ctrl->tables_info[tbl_idx].table_name, 0, BF_TBL_NAME_LEN);
    strncpy(pd_ctrl->tables_info[tbl_idx].table_name,
            pipemgr_tbl_pkg_get_field_string_name(
                devid, prof_id, table_ptr->tablename_str_index),
            BF_TBL_NAME_LEN - 1);

    if ((pipe_capture->logical_table_hit >> tbl_idx) & 0x1) {
      pd_ctrl->tables_info[tbl_idx].table_hit = 1;
    }
    if ((pipe_capture->gateway_table_inhibit_logical >> tbl_idx) & 0x1) {
      pd_ctrl->tables_info[tbl_idx].table_inhibited = 1;
    }
    if ((pipe_capture->table_active >> tbl_idx) & 0x1) {
      pd_ctrl->tables_info[tbl_idx].table_executed = 1;
    }

    /* Multiple signals (hit, inhibited, executed)  could be set at same time
       in asic. We will massage the output so that user understands it better.
    */
    if (!pd_ctrl->tables_info[tbl_idx].table_executed) {
      /* Mark table as no hit as table was not active DRV-518 */
      pd_ctrl->tables_info[tbl_idx].table_hit = 0;
      pd_ctrl->tables_info[tbl_idx].table_inhibited = 0;
      pd_ctrl->tables_info[tbl_idx].match_hit_address = 0;
    } else {
      /* Tofino A0 bug: Table inhibit does not get set correctly.
         Workaround: If match address is special gateway address then table
         was inhibited (COMPILER-496, DRV-518)
      */
      if (dev_info->dev_family == BF_DEV_FAMILY_TOFINO &&
          dev_info->part_rev == BF_SKU_CHIP_PART_REV_A0) {
        if (pd_ctrl->tables_info[tbl_idx].match_hit_address == 0x7ffff) {
          pd_ctrl->tables_info[tbl_idx].table_inhibited = 1;
        } else {
          pd_ctrl->tables_info[tbl_idx].table_inhibited = 0;
        }
      }

      /* If gateway is not enabled for this tbl, turn off table inhibited */
      if ((pd_ctrl->tables_info[tbl_idx].table_inhibited) &&
          (!table_ptr->has_attached_gateway)) {
        pd_ctrl->tables_info[tbl_idx].table_inhibited = 0;
      }
      /* We determined the inhibited status accurately above.
         If table was inhibited then table cannot be a hit. DRV-781
      */
      if (pd_ctrl->tables_info[tbl_idx].table_inhibited) {
        pd_ctrl->tables_info[tbl_idx].table_hit = 0;
        pd_ctrl->tables_info[tbl_idx].match_hit_address = 0;
      }
    }
    table_ptr++;
  }
  /* Get next table name */
  rc = pipe_mgr_get_next_tbl_name(dev_info,
                                  prof_id,
                                  stage_id,
                                  pipe_capture->next_table_out,
                                  pd_ctrl->next_table);
  if (rc != PIPE_SUCCESS) return PIPE_OBJ_NOT_FOUND;

  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      break;
    case BF_DEV_FAMILY_TOFINO2:
      /* Get enabled next table names*/
      rc =
          pipe_mgr_get_enabled_next_tbl_names(dev_info,
                                              prof_id,
                                              stage_id,
                                              pipe_capture->mpr_next_table_out,
                                              pipe_capture->enabled_next_tables,
                                              pd_ctrl->enabled_next_tbl_names);
      if (rc != PIPE_SUCCESS) return PIPE_OBJ_NOT_FOUND;
      /* Get datapath_error */
      // get stage_dependency of stage 7, if true, error info valid.
      pd_ctrl->ingr_dp_error = 0;
      pd_ctrl->egr_dp_error = 0;
      if (pipe_mgr_stage_match_dependent_get(devid, prof_id, 7, direction)) {
        pd_ctrl->ingr_dp_error = pipe_capture->datapath_error & 0x1;
        pd_ctrl->egr_dp_error = (pipe_capture->datapath_error & 0x2) >> 1;
      }

      /* Get global_exec_out */
      rc = pipe_mgr_get_gbl_exec_tbl_names(dev_info,
                                           prof_id,
                                           stage_id,
                                           pipe_capture->global_exec_out,
                                           pd_ctrl->gbl_exec_tbl_names);
      if (rc != PIPE_SUCCESS) return PIPE_OBJ_NOT_FOUND;
      /* Get mpr global_exec_out */
      rc =
          pipe_mgr_get_gbl_exec_tbl_names(dev_info,
                                          prof_id,
                                          stage_id,
                                          pipe_capture->enabled_global_exec_out,
                                          pd_ctrl->enabled_gbl_exec_tbl_names);
      if (rc != PIPE_SUCCESS) return PIPE_OBJ_NOT_FOUND;

      /* Get long_branch_out */
      rc = pipe_mgr_get_pred_long_branch_tbl_names(
          dev_info,
          prof_id,
          stage_id,
          pipe_capture->long_branch_out,
          &pipe_capture->long_branch,
          pd_ctrl->long_branch_tbl_names);
      if (rc != PIPE_SUCCESS) return PIPE_OBJ_NOT_FOUND;

      /* Get mpr long_branch_out */
      rc = pipe_mgr_get_enabled_long_branch_tbl_names(
          dev_info,
          prof_id,
          stage_id,
          pipe_capture->mpr_long_branch_out,
          &pipe_capture->long_branch,
          pd_ctrl->enabled_long_branch_tbl_names);
      if (rc != PIPE_SUCCESS) return PIPE_OBJ_NOT_FOUND;

      /* Get meter adr */
      rc = pipe_mgr_get_meter_alu_info(
          dev_info, prof_id, pipe, stage_id, pipe_capture, pd_ctrl);
      if (rc != PIPE_SUCCESS) return PIPE_OBJ_NOT_FOUND;
      break;
    default:
      // PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }
  return PIPE_SUCCESS;
}

static bool is_field_valid(rmt_dev_info_t *dev_info,
                           pipemgr_tbl_pkg_snapshot_t *snapshot_ptr,
                           profile_id_t prof_id,
                           int direction,
                           char *field_name,
                           uint8_t *data_v) {
  /* In Tofino, because of power savings mechanisms in HW,
   * it is possible to have invalid containers, with value leftovers
   * from previous stages. Check validity bit on all containers for
   * all phv fields matching requested one in order to handle fields
   * spanning over multiple containers. All container fields are valid
   * for newer chips versions. */
  if (dev_info->dev_family != BF_DEV_FAMILY_TOFINO) {
    return true;
  }
  int phv_numb_limit = 224;

  pipemgr_tbl_pkg_snapshot_phv_t *phv_recs = snapshot_ptr->phv_recs;
  for (int i = 0; i < snapshot_ptr->total_phv_recs; i++) {
    if (phv_recs->direction != direction || phv_recs->pov_hdrs != NULL) {
      phv_recs++;
      continue;
    }
    char *fieldname = pipemgr_tbl_pkg_get_field_string_name(
        dev_info->dev_id, prof_id, phv_recs->phvname_str_index);
    if (strncmp(fieldname, field_name, strlen(field_name)) != 0) {
      phv_recs++;
      continue;
    }
    if (phv_recs->phvnumber >= phv_numb_limit) {
      // if part of the field is present in a tagalong phv record skip it
      continue;
    }
    uint32_t bit_offset = phv_recs->phvnumber % 8;
    uint32_t index = phv_recs->phvnumber / 8;
    /* If even one part of the field is invalid, whole field is. */
    if (!(data_v[index] & (1 << bit_offset))) {
      return false;
    }
    phv_recs++;
  }

  return true;
}

pipe_status_t pipe_mgr_ctxjson_tof_snapshot_capture_print(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    int direction,
    bf_snapshot_capture_ctrl_info_t *pd_ctrl,
    uint8_t *pd_capture,
    uint8_t *pd_capture_v,
    char *str,
    int *c_str_len,
    int max_len,
    dev_stage_t print_stage) {
  pipe_status_t rc = PIPE_SUCCESS;
  int idx = 0;
  int c_len = *c_str_len;
  uint8_t *data_ptr = NULL;
  uint32_t num_fields_in_dict = 0, num_fields = 0;
  pipe_snap_stage_field_info_t *all_fields_dict = NULL;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_snapshot_t *snapshot_ptr;
  pipemgr_tbl_pkg_snapshot_phv_t *phv_recs;
  pipemgr_tbl_pkg_snapshot_pov_t *pov_hdrs;
  uint32_t byteoffset, capture_value = 0, bj_hash, i, j;
  char *fieldname, *povname, povfieldname[PIPE_SNAP_TRIG_FIELD_NAME_LEN];
  uint16_t stage, decode_stage;
  bool tagalong_phv = false;
  int phv_numb_limit;
  stage = pd_ctrl->stage_id;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  /* Dump only required stages that have data. */
  if ((print_stage != PIPE_MGR_SNAP_STAGE_INVALID && print_stage != stage) ||
      (pd_ctrl->prev_stage_trigger == false &&
       pd_ctrl->local_stage_trigger == false &&
       pd_ctrl->timer_trigger == false)) {
    return PIPE_SUCCESS;
  }

  // For data capture (oPHV) stage to get field list and decode should be
  // 1 higher than capture itself as context.json defines iPHV only.
  if (stage >= dev_info->profile_info[prof_id]->num_stages) {
    decode_stage = dev_info->profile_info[prof_id]->num_stages;
  } else {
    decode_stage = stage + 1;
  }

  pipe_mgr_ctxjson_phv_fields_dict_size_get(
      devid, prof_id, decode_stage, direction, &num_fields);
  all_fields_dict =
      PIPE_MGR_CALLOC(num_fields, sizeof(pipe_snap_stage_field_info_t));
  if (!all_fields_dict) {
    return PIPE_NO_SYS_RESOURCES;
  }

  /* Get fields in this stage */
  pipe_mgr_ctxjson_phv_fields_dict_get(devid,
                                       prof_id,
                                       decode_stage,
                                       direction,
                                       &all_fields_dict[0],
                                       &num_fields_in_dict);

  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "-------------- Snapshot Capture for stage %d, direction "
                    "%s ----------------\n",
                    stage,
                    (direction == 0) ? "Ingress" : "Egress");

  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "Snapshot trigger type: \n");

  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "    Prev stage  : %s \n",
                    (pd_ctrl->prev_stage_trigger) ? "Yes" : "No");

  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "    Local stage : %s \n",
                    (pd_ctrl->local_stage_trigger) ? "Yes" : "No");

  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "    Timer       : %s \n",
                    (pd_ctrl->timer_trigger) ? "Yes" : "No");

  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "------------------- Captured Fields "
                    "-----------------------------------------------\n");
  c_len += snprintf(
      str + c_len,
      (c_len < max_len) ? (max_len - c_len - 1) : 0,
      "%-40s   Value %s\n",
      "Field name",
      (dev_info->dev_family == BF_DEV_FAMILY_TOFINO) ? "(* Invalid)" : "");
  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "----------------------------------------------------------"
                    "-------------------------\n");
  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth,
      0,
      decode_stage + 1,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut,
      0,
      decode_stage + 1);
  if (!lut_ptr) {
    LOG_ERROR("%s: Unable to find phv details from context json", __func__);
    PIPE_MGR_FREE(all_fields_dict);
    return (PIPE_INVALID_ARG);
  }

  /* This API is used by ucli, need to ntoh conversion */
  snapshot_ptr = lut_ptr->u.snapshot_ptr;
  phv_recs = snapshot_ptr->phv_recs;

  switch (dev_info->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      phv_numb_limit = 224;
      break;
    case BF_DEV_FAMILY_TOFINO2:
      phv_numb_limit = 280;
      break;
    case BF_DEV_FAMILY_TOFINO3:
      phv_numb_limit = 280;
      break;
    default:
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
  }
  for (i = 0; i < snapshot_ptr->total_phv_recs; i++) {
    if (phv_recs->direction != direction || phv_recs->pov_hdrs != NULL) {
      phv_recs++;
      continue;
    }
    fieldname = pipemgr_tbl_pkg_get_field_string_name(
        devid, prof_id, phv_recs->phvname_str_index);
    rc = pipe_mgr_get_phv_field_byte_offset(
        devid, prof_id, direction, fieldname, &byteoffset);
    if (rc == PIPE_SUCCESS) {
      data_ptr = pd_capture + byteoffset;
    } else {
      PIPE_MGR_DBGCHK(0);
      return PIPE_UNEXPECTED;
    }
    if (phv_recs->phvnumber >= phv_numb_limit) {
      tagalong_phv = true;
    } else {
      tagalong_phv = false;
    }
    if (phv_recs->fieldwidth == 1) {
      capture_value = *data_ptr;
    } else if (phv_recs->fieldwidth == 2) {
      capture_value = *(uint16_t *)data_ptr;
    } else if (phv_recs->fieldwidth == 3) {
      capture_value = *(uint32_t *)data_ptr;
      capture_value &= 0xFFFFFF;
    } else if (phv_recs->fieldwidth == 4) {
      capture_value = *(uint32_t *)data_ptr;
    }
    if (phv_recs->fieldwidth <= 4) {
      if (!phv_recs->fieldlsb) {
        // When field is spread across multiple PHV records,
        // (example mac addr is in 32b phv and 16bit phv), walking
        // over all PHV records will print fieldname more than once.
        // Hence print fieldname only when fieldlsb is zero. i.e
        // use PHV record that has field lsb bit zero
        bool field_valid = is_field_valid(dev_info,
                                          snapshot_ptr,
                                          prof_id,
                                          direction,
                                          fieldname,
                                          pd_capture_v);
        c_len += snprintf(str + c_len,
                          (c_len < max_len) ? (max_len - c_len - 1) : 0,
                          "%-40s   0x%x %s%s\n",
                          fieldname,
                          capture_value,
                          tagalong_phv ? "(T-PHV)" : "",
                          field_valid ? "" : "*");
      }
    } else {
      // When field is spread across multiple PHV records,
      // (example mac addr is in 32b phv and 16bit phv), walking
      // over all PHV records will print fieldname more than once.
      // Hence print fieldname only when fieldlsb is zero. i.e
      // use PHV record that has field lsb bit zero
      if (!phv_recs->fieldlsb) {
        bool field_valid = is_field_valid(dev_info,
                                          snapshot_ptr,
                                          prof_id,
                                          direction,
                                          fieldname,
                                          pd_capture_v);
        c_len += snprintf(str + c_len,
                          (c_len < max_len) ? (max_len - c_len - 1) : 0,
                          "%-40s   ",
                          fieldname);
        for (idx = 0; idx < phv_recs->fieldwidth; idx++) {
          c_len += snprintf(str + c_len,
                            (c_len < max_len) ? (max_len - c_len - 1) : 0,
                            "%02x ",
                            data_ptr[idx]);
        }
        c_len += snprintf(str + c_len,
                          (c_len < max_len) ? (max_len - c_len - 1) : 0,
                          "%s%s\n",
                          tagalong_phv ? "(T-PHV)" : "",
                          field_valid ? "" : "*");
      }
    }
    phv_recs++;
  }

  phv_recs = snapshot_ptr->phv_recs;
  for (i = 0; i < snapshot_ptr->total_phv_recs; i++) {
    if (phv_recs->direction != direction || phv_recs->pov_hdrs == NULL) {
      phv_recs++;
      continue;
    }
    if (phv_recs->phvnumber >= phv_numb_limit) {
      tagalong_phv = true;
    } else {
      tagalong_phv = false;
    }
    pov_hdrs = phv_recs->pov_hdrs;
    for (j = 0; j < pov_hdrs->pov_bit_count; j++) {
      if (pov_hdrs->hidden[j]) {
        continue;
      }
      povname = pipemgr_tbl_pkg_get_field_string_name(
          devid, prof_id, pov_hdrs->pov_hdr_str_index[j]);
      snprintf(povfieldname, sizeof(povfieldname), "%s%s", povname, "_valid");
      rc = pipe_mgr_get_phv_field_byte_offset(
          devid, prof_id, direction, povfieldname, &byteoffset);
      if (rc == PIPE_SUCCESS) {
        data_ptr = pd_capture + byteoffset;
      } else {
        PIPE_MGR_DBGCHK(0);
        return PIPE_UNEXPECTED;
      }
      c_len += snprintf(str + c_len,
                        (c_len < max_len) ? (max_len - c_len - 1) : 0,
                        "%-40s   0x%x %s\n",
                        povfieldname,
                        *data_ptr,
                        tagalong_phv ? "(T-PHV)" : "");
    }
    phv_recs++;
  }

  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "\n----------------------- Table info "
                    "-------------------------------------------------\n");
  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "----------------------------------------------------------"
                    "--------------------------\n");

  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "%-40s %-10s %-5s %-5s %-10s %-13s %-11s %-10s\n",
                    "Table Name",
                    "Handle",
                    "Type",
                    "Hit",
                    "Hit-Addr",
                    "Hit-Entry-Hdl",
                    "Inhibited",
                    "Executed");

  for (idx = 0; idx < dev_info->dev_cfg.stage_cfg.num_logical_tables; idx++) {
    if (strlen(pd_ctrl->tables_info[idx].table_name) <= 0) {
      continue;
    }

    c_len += snprintf(
        str + c_len,
        (c_len < max_len) ? (max_len - c_len - 1) : 0,
        "%-40s 0x%-8x %-5s %-5s 0x%-8x 0x%-11x %-11s %-10s\n",
        pd_ctrl->tables_info[idx].table_name,
        pd_ctrl->tables_info[idx].table_handle,
        (pd_ctrl->tables_info[idx].table_type_tcam == 1) ? "Tcam" : "Exact",
        (pd_ctrl->tables_info[idx].table_hit) ? "Yes" : "No",
        pd_ctrl->tables_info[idx].match_hit_address,
        pd_ctrl->tables_info[idx].hit_entry_handle,
        (pd_ctrl->tables_info[idx].table_inhibited) ? "Yes" : "No",
        (pd_ctrl->tables_info[idx].table_executed) ? "Yes" : "No");
  }
  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "----------------------------------------------------------"
                    "-------------------------\n");

  if (strnlen(pd_ctrl->next_table, sizeof(pd_ctrl->next_table)) > 0) {
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "Next Table: %s\n",
                      pd_ctrl->next_table);
  }
  if (strnlen(pd_ctrl->enabled_next_tbl_names,
              sizeof(pd_ctrl->enabled_next_tbl_names)) > 0) {
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "Enabled Next Tables: %s\n",
                      pd_ctrl->enabled_next_tbl_names);
  }
  c_len +=
      snprintf(str + c_len,
               (c_len < max_len) ? (max_len - c_len - 1) : 0,
               "Datapath_error: %s\n",
               (pd_ctrl->ingr_dp_error && pd_ctrl->egr_dp_error)
                   ? "Ingress and Egress Error"
                   : (pd_ctrl->ingr_dp_error
                          ? "Ingress Error"
                          : (pd_ctrl->egr_dp_error ? "Egress Error" : "None")));

  for (idx = 0; idx < 4; idx++) {
    if (pd_ctrl->meter_alu_info[idx].ctrl_info_p == NULL) continue;
    c_len += snprintf(
        str + c_len,
        (c_len < max_len) ? (max_len - c_len - 1) : 0,
        "Meter ALU %-1d info: Ctrl-%s, Table_handle-0x%x\n",
        idx,
        pd_ctrl->meter_alu_info[idx]
            .ctrl_info_p,  //[pd_ctrl->meter_alu_info[idx].ctrl_info_num],
        pd_ctrl->meter_alu_info[idx].table_handle);
  }

  if (strnlen(pd_ctrl->gbl_exec_tbl_names,
              sizeof(pd_ctrl->gbl_exec_tbl_names)) > 0) {
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "Global Execute Tables: %s\n",
                      pd_ctrl->gbl_exec_tbl_names);
  }
  if (strnlen(pd_ctrl->enabled_gbl_exec_tbl_names,
              sizeof(pd_ctrl->enabled_gbl_exec_tbl_names)) > 0) {
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "Enabled Global Execute Tables: %s\n",
                      pd_ctrl->enabled_gbl_exec_tbl_names);
  }

  if (strnlen(pd_ctrl->long_branch_tbl_names,
              sizeof(pd_ctrl->long_branch_tbl_names)) > 0) {
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "Long Branch Tables: %s\n",
                      pd_ctrl->long_branch_tbl_names);
  }
  if (strnlen(pd_ctrl->enabled_long_branch_tbl_names,
              sizeof(pd_ctrl->enabled_long_branch_tbl_names)) > 0) {
    c_len += snprintf(str + c_len,
                      (c_len < max_len) ? (max_len - c_len - 1) : 0,
                      "Enabled Long Branch Tables: %s\n",
                      pd_ctrl->enabled_long_branch_tbl_names);
  }
  c_len += snprintf(str + c_len,
                    (c_len < max_len) ? (max_len - c_len - 1) : 0,
                    "----------------------------------------------------------"
                    "-------------------------\n");

  *c_str_len = c_len;
  PIPE_MGR_FREE(all_fields_dict);

  return rc;
}

/* Get the captured value for a particular field */
pipe_status_t pipe_mgr_ctxjson_tof_snapshot_capture_field_value_get(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    dev_stage_t stage,
    int direction,
    uint8_t *pd_capture,
    uint8_t *pd_capture_v,
    char *field_name,
    uint64_t *field_value,
    bool *field_valid) {
  pipe_status_t rc = PIPE_SUCCESS;
  uint8_t *data_ptr = NULL;
  uint32_t num_fields_in_dict = 0, num_fields = 0;
  pipe_snap_stage_field_info_t *all_fields_dict = NULL;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_snapshot_t *snapshot_ptr;
  pipemgr_tbl_pkg_snapshot_phv_t *phv_recs;
  pipemgr_tbl_pkg_snapshot_pov_t *pov_hdrs;
  uint32_t byteoffset, capture_value = 0, bj_hash, i, j;
  char *fieldname, *povname, povfieldname[PIPE_SNAP_TRIG_FIELD_NAME_LEN];

  if (!field_valid) {
    return PIPE_INVALID_ARG;
  }

  *field_value = 0;

  pipe_mgr_ctxjson_phv_fields_dict_size_get(
      devid, prof_id, stage, direction, &num_fields);
  all_fields_dict =
      PIPE_MGR_CALLOC(num_fields, sizeof(pipe_snap_stage_field_info_t));
  if (!all_fields_dict) {
    return PIPE_NO_SYS_RESOURCES;
  }

  /* Get fields in this stage */
  pipe_mgr_ctxjson_phv_fields_dict_get(devid,
                                       prof_id,
                                       stage,
                                       direction,
                                       &all_fields_dict[0],
                                       &num_fields_in_dict);

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth, 0, stage + 1, 0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut,
      0,
      stage + 1);
  if (!lut_ptr) {
    LOG_ERROR("%s: Unable to find phv details from context json", __func__);
    PIPE_MGR_FREE(all_fields_dict);
    return (PIPE_INVALID_ARG);
  }

  /* This API is used by ucli, need to ntoh conversion */
  snapshot_ptr = lut_ptr->u.snapshot_ptr;
  phv_recs = snapshot_ptr->phv_recs;

  bool field_found = false;
  for (i = 0; (i < snapshot_ptr->total_phv_recs) && (field_found == false);
       i++) {
    if (phv_recs->direction != direction || phv_recs->pov_hdrs != NULL) {
      phv_recs++;
      continue;
    }
    fieldname = pipemgr_tbl_pkg_get_field_string_name(
        devid, prof_id, phv_recs->phvname_str_index);
    if (strncmp(fieldname, field_name, strlen(field_name)) != 0) {
      phv_recs++;
      continue;
    }
    rc = pipe_mgr_get_phv_field_byte_offset(
        devid, prof_id, direction, fieldname, &byteoffset);
    if (rc == PIPE_SUCCESS) {
      data_ptr = pd_capture + byteoffset;
    } else {
      PIPE_MGR_DBGCHK(0);
      PIPE_MGR_FREE(all_fields_dict);
      return PIPE_UNEXPECTED;
    }

    if (phv_recs->fieldwidth == 1) {
      capture_value = *data_ptr;
    } else if (phv_recs->fieldwidth == 2) {
      capture_value = *(uint16_t *)data_ptr;
    } else if (phv_recs->fieldwidth == 3) {
      capture_value = *(uint32_t *)data_ptr;
      capture_value &= 0xFFFFFF;
    } else if (phv_recs->fieldwidth == 4) {
      capture_value = *(uint32_t *)data_ptr;
    }

    if (phv_recs->fieldwidth <= 4) {
      if (!phv_recs->fieldlsb) {
        field_found = true;
        *field_value = capture_value;
      }
    } else {
      if (!phv_recs->fieldlsb) {
        field_found = true;
        // When field is spread across multiple PHV records,
        // (example mac addr is in 32b phv and 16bit phv), walking
        // over all PHV records will print fieldname more than once.
        // Hence print fieldname only when fieldlsb is zero. i.e
        // use PHV record that has field lsb bit zero
        int idx = 0;
        int max_width = phv_recs->fieldwidth;
        if (max_width > (int)sizeof(uint64_t)) {
          // Take least significant bytes, so start from proper offset.
          idx = phv_recs->fieldwidth - sizeof(uint64_t);
        }

        uint64_t temp = 0;
        for (; idx < max_width; idx++) {
          temp = data_ptr[idx];
          temp = temp << ((max_width - idx - 1) * 8);
          *field_value |= temp;
        }
      }
    }
    phv_recs++;
  }

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    PIPE_MGR_FREE(all_fields_dict);
    return (PIPE_UNEXPECTED);
  }
  *field_valid = is_field_valid(
      dev_info, snapshot_ptr, prof_id, direction, field_name, pd_capture_v);

  /* POV bits */
  phv_recs = snapshot_ptr->phv_recs;
  for (i = 0; (i < snapshot_ptr->total_phv_recs) && (field_found == false);
       i++) {
    if (phv_recs->direction != direction || phv_recs->pov_hdrs == NULL) {
      phv_recs++;
      continue;
    }
    pov_hdrs = phv_recs->pov_hdrs;
    for (j = 0; (j < pov_hdrs->pov_bit_count) && (field_found == false); j++) {
      if (pov_hdrs->hidden[j]) {
        continue;
      }
      povname = pipemgr_tbl_pkg_get_field_string_name(
          devid, prof_id, pov_hdrs->pov_hdr_str_index[j]);
      snprintf(povfieldname, sizeof(povfieldname), "%s%s", povname, "_valid");
      if (strncmp(povfieldname, field_name, strlen(field_name)) != 0) {
        continue;
      }
      field_found = true;
      rc = pipe_mgr_get_phv_field_byte_offset(
          devid, prof_id, direction, povfieldname, &byteoffset);
      if (rc == PIPE_SUCCESS) {
        data_ptr = pd_capture + byteoffset;
      } else {
        PIPE_MGR_DBGCHK(0);
        return PIPE_UNEXPECTED;
      }
      *field_value = *data_ptr;
    }
    phv_recs++;
  }

  PIPE_MGR_FREE(all_fields_dict);

  if (field_found) {
    return PIPE_SUCCESS;
  }

  *field_valid = false;
  return PIPE_INVALID_ARG;
}

/* Copy fields from the PD trigger spec (value and mask) into the driver's field
 * info structure. */
pipe_status_t pipe_mgr_ctxjson_snapshot_trig_field_set_from_pd(
    pipe_snapshot_hdl_t hdl,
    bf_dev_id_t devid,
    profile_id_t prof_id,
    int direction,
    void *trig_spec,
    void *trig_mask,
    pipe_snap_trig_field_info_t *field_info,
    int max_fields) {
  pipe_status_t rc = PIPE_SUCCESS;
  int field_idx = 0, idx = 0, m;
  uint8_t *data_ptr = NULL, *mask_ptr = NULL;
  bool val_valid = false, mask_valid = false;
  uint8_t *pd_trig_spec, *pd_trig_mask;
  uint32_t i, j, bj_hash;
  uint32_t mask = 0;
  uint32_t val = 0;
  char *fieldname, *povname, povfieldname[PIPE_SNAP_TRIG_FIELD_NAME_LEN];
  pipemgr_tbl_pkg_lut_t *lut_ptr = NULL;
  pipemgr_tbl_pkg_snapshot_t *snapshot_ptr = NULL;
  pipemgr_tbl_pkg_snapshot_phv_t *phv_recs = NULL;
  pipemgr_tbl_pkg_snapshot_pov_t *pov_hdrs = NULL;
  uint32_t byteoffset;
  bool field_already_added;

  if (!trig_spec || !trig_mask || !field_info) {
    LOG_ERROR("%s: Null pointer arguments passed", __func__);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  pd_trig_spec = (uint8_t *)trig_spec;
  pd_trig_mask = (uint8_t *)trig_mask;

  for (int k = 0; k < dev_info->profile_info[prof_id]->num_stages; ++k) {
    bj_hash = bob_jenkin_hash_one_at_a_time(
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth, 0, k + 1, 0);
    lut_ptr = pipemgr_entry_format_get_lut_entry(
        bj_hash,
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth,
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut,
        0,
        k + 1);
    if (!lut_ptr) {
      LOG_ERROR(
          "%s: Unable to find phv details from context json, dev %d profile-id "
          "%d direction %s stage %d",
          __func__,
          devid,
          prof_id,
          direction ? "Egress" : "Ingress",
          k);
      return (PIPE_OBJ_NOT_FOUND);
    }

    /* This API is used by ucli, need to ntoh conversion */
    snapshot_ptr = lut_ptr->u.snapshot_ptr;
    phv_recs = snapshot_ptr->phv_recs;
    for (i = 0; i < snapshot_ptr->total_phv_recs; i++) {
      if (phv_recs->direction != direction || phv_recs->pov_hdrs != NULL) {
        phv_recs++;
        continue;
      }

      fieldname = pipemgr_tbl_pkg_get_field_string_name(
          devid, prof_id, phv_recs->phvname_str_index);
      rc = pipe_mgr_get_phv_field_byte_offset(
          devid, prof_id, direction, fieldname, &byteoffset);
      if (rc == PIPE_SUCCESS) {
        data_ptr = pd_trig_spec + byteoffset;
        mask_ptr = pd_trig_mask + byteoffset;
      } else {
        PIPE_MGR_DBGCHK(0);
        return PIPE_UNEXPECTED;
      }
      val_valid = false;
      mask_valid = false;
      uint16_t data16 = 0, mask16 = 0;
      uint32_t data32 = 0, mask32 = 0;
      if (phv_recs->fieldwidth > 4) {
        for (idx = 0; idx < phv_recs->fieldwidth; idx++) {
          if (data_ptr[idx] != 0) val_valid = true;
          if (mask_ptr[idx] != 0) mask_valid = true;
        }
      } else if (phv_recs->fieldwidth == 1) {
        if (*data_ptr) val_valid = true;
        if (*mask_ptr) mask_valid = true;
      } else if (phv_recs->fieldwidth == 2) {
        PIPE_MGR_MEMCPY(&data16, data_ptr, sizeof data16);
        PIPE_MGR_MEMCPY(&mask16, mask_ptr, sizeof mask16);
        if (data16) val_valid = true;
        if (mask16) mask_valid = true;
      } else {
        PIPE_MGR_MEMCPY(&data32, data_ptr, sizeof data32);
        PIPE_MGR_MEMCPY(&mask32, mask_ptr, sizeof mask32);
        if (data32) val_valid = true;
        if (mask32) mask_valid = true;
      }

      if (val_valid || mask_valid) {
        // Certain trigger fields can be split into multiple PHVs ; hence there
        // can be multiple phv_recs with same field name.
        // If already processed, do not add the same field into field_info list
        // again.
        field_already_added = false;
        for (m = 0; m < field_idx; m++) {
          if (!strcmp(field_info[m].name, fieldname)) {
            // field already added to trigger list of field_infos.
            field_already_added = true;
            break;
          }
        }
        if (field_already_added) {
          continue;
        }
        field_info[field_idx].valid = true;
        PIPE_MGR_MEMSET(
            field_info[field_idx].name, 0, PIPE_SNAP_TRIG_FIELD_NAME_LEN);
        strncpy(field_info[field_idx].name,
                fieldname,
                PIPE_SNAP_TRIG_FIELD_NAME_LEN - 1);
        if (phv_recs->fieldwidth > 4) {
          PIPE_MGR_MEMCPY(&(field_info[field_idx].value[0]),
                          data_ptr,
                          phv_recs->fieldwidth);
          PIPE_MGR_MEMCPY(
              &(field_info[field_idx].mask[0]), mask_ptr, phv_recs->fieldwidth);
          /* Large field, just log the first 8 bytes. */
          LOG_DBG(
              "Dev %d Snapshot 0x%x: Added %d byte trigger "
              "0x%02x%02x%02x%02x%02x%02x%02x%02x/"
              "0x%02x%02x%02x%02x%02x%02x%02x%02x %s",
              devid,
              hdl,
              phv_recs->fieldwidth,
              *data_ptr,
              *(data_ptr + 1),
              *(data_ptr + 2),
              *(data_ptr + 3),
              phv_recs->fieldwidth >= 5 ? *(data_ptr + 4) : 0,
              phv_recs->fieldwidth >= 6 ? *(data_ptr + 6) : 0,
              phv_recs->fieldwidth >= 7 ? *(data_ptr + 7) : 0,
              phv_recs->fieldwidth >= 8 ? *(data_ptr + 8) : 0,
              *mask_ptr,
              *(mask_ptr + 1),
              *(mask_ptr + 2),
              *(mask_ptr + 3),
              phv_recs->fieldwidth >= 5 ? *(mask_ptr + 4) : 0,
              phv_recs->fieldwidth >= 6 ? *(mask_ptr + 6) : 0,
              phv_recs->fieldwidth >= 7 ? *(mask_ptr + 7) : 0,
              phv_recs->fieldwidth >= 8 ? *(mask_ptr + 8) : 0,
              fieldname);
        } else {
          if (phv_recs->fieldwidth == 1) {
            val = *data_ptr;
            mask = *mask_ptr;
            LOG_DBG(
                "Dev %d Snapshot 0x%x: Added %d byte trigger 0x%02x/0x%02x %s",
                devid,
                hdl,
                phv_recs->fieldwidth,
                val,
                mask,
                fieldname);
          } else if (phv_recs->fieldwidth == 2) {
            val = data16;
            mask = mask16;
            LOG_DBG(
                "Dev %d Snapshot 0x%x: Added %d byte trigger 0x%04x/0x%04x %s",
                devid,
                hdl,
                phv_recs->fieldwidth,
                val,
                mask,
                fieldname);
          } else if (phv_recs->fieldwidth == 4) {
            val = data32;
            mask = mask32;
            LOG_DBG(
                "Dev %d Snapshot 0x%x: Added %d byte trigger 0x%08x/0x%08x %s",
                devid,
                hdl,
                phv_recs->fieldwidth,
                val,
                mask,
                fieldname);
          } else if (phv_recs->fieldwidth == 3) {
            val = data32 & 0x00ffffff;
            mask = mask32 & 0x00ffffff;
            LOG_DBG(
                "Dev %d Snapshot 0x%x: Added %d byte trigger 0x%06x/0x%06x %s",
                devid,
                hdl,
                phv_recs->fieldwidth,
                val,
                mask,
                fieldname);
          }
          PIPE_MGR_MEMCPY(
              &(field_info[field_idx].value[0]), &val, phv_recs->fieldwidth);
          PIPE_MGR_MEMCPY(
              &(field_info[field_idx].mask[0]), &mask, phv_recs->fieldwidth);
        }
        field_info[field_idx].width = phv_recs->fieldwidth;
        field_idx++;
        if (field_idx >= max_fields) {
          return PIPE_INVALID_ARG;
        }
      }
      phv_recs++;
    }
    phv_recs = snapshot_ptr->phv_recs;
    for (i = 0; i < snapshot_ptr->total_phv_recs; i++) {
      if (phv_recs->direction != direction || phv_recs->pov_hdrs == NULL) {
        phv_recs++;
        continue;
      }
      pov_hdrs = phv_recs->pov_hdrs;
      for (j = 0; j < pov_hdrs->pov_bit_count; j++) {
        if (pov_hdrs->hidden[j]) {
          continue;
        }
        val_valid = false;
        mask_valid = false;
        povname = pipemgr_tbl_pkg_get_field_string_name(
            devid, prof_id, pov_hdrs->pov_hdr_str_index[j]);
        snprintf(povfieldname, sizeof(povfieldname), "%s%s", povname, "_valid");
        rc = pipe_mgr_get_phv_field_byte_offset(
            devid, prof_id, direction, povfieldname, &byteoffset);
        if (rc == PIPE_SUCCESS) {
          data_ptr = pd_trig_spec + byteoffset;
          mask_ptr = pd_trig_mask + byteoffset;
        } else {
          PIPE_MGR_DBGCHK(0);
          return PIPE_UNEXPECTED;
        }
        if (*data_ptr) val_valid = true;
        if (*mask_ptr) mask_valid = true;
        if (val_valid || mask_valid) {
          field_already_added = false;
          for (m = 0; m < field_idx; m++) {
            if (!strcmp(field_info[m].name, povfieldname)) {
              // field already added to trigger list of field_infos.
              field_already_added = true;
              break;
            }
          }
          if (field_already_added) {
            continue;
          }
          field_info[field_idx].valid = true;
          PIPE_MGR_MEMSET(
              field_info[field_idx].name, 0, PIPE_SNAP_TRIG_FIELD_NAME_LEN);
          strncpy(field_info[field_idx].name,
                  povfieldname,
                  PIPE_SNAP_TRIG_FIELD_NAME_LEN - 1);
          PIPE_MGR_MEMCPY(&(field_info[field_idx].value[0]), data_ptr, 1);
          PIPE_MGR_MEMCPY(&(field_info[field_idx].mask[0]), mask_ptr, 1);
          LOG_DBG("Dev %d Snapshot 0x%x: Added pov trigger 0x%02x/0x%02x %s",
                  devid,
                  hdl,
                  *data_ptr,
                  *mask_ptr,
                  povfieldname);
          field_info[field_idx].width = 1;
          field_idx++;
          if (field_idx >= max_fields) {
            return PIPE_INVALID_ARG;
          }
        }
      }
      phv_recs++;
    }
  }

  return rc;
}

pipe_status_t pipe_mgr_ctxjson_tof_log_id_to_tbl_name(bf_dev_id_t devid,
                                                      profile_id_t prof_id,
                                                      int logical_tbl_id,
                                                      char *table_name) {
  uint8_t stage;
  int idx = 0;
  uint32_t bj_hash;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_snapshot_t *snapshot_ptr;
  pipemgr_tbl_pkg_logical_table_details_t *table_ptr;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  stage = logical_tbl_id / dev_info->dev_cfg.stage_cfg.num_logical_tables;

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth, 0, stage + 1, 0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut,
      0,
      stage + 1);
  if (!lut_ptr) {
    LOG_TRACE("%s: Unable to find logical table details from context json",
              __func__);
    return (PIPE_OBJ_NOT_FOUND);
  }
  snapshot_ptr = lut_ptr->u.snapshot_ptr;
  table_ptr = snapshot_ptr->tables;
  for (idx = 0; idx < snapshot_ptr->total_tables; idx++) {
    if (table_ptr->logical_id == logical_tbl_id) {
      break;
    }
    table_ptr++;
  }
  if (idx < snapshot_ptr->total_tables) {
    strncpy(table_name,
            pipemgr_tbl_pkg_get_field_string_name(
                devid, prof_id, table_ptr->tablename_str_index),
            BF_TBL_NAME_LEN);
  } else {
    // did not find next table name
    LOG_TRACE("%s: Unable to find next table details from context json",
              __func__);
    return (PIPE_OBJ_NOT_FOUND);
  }
  return (PIPE_SUCCESS);
}

static void populate_log_id_arr(uint32_t log_id,
                                uint32_t *log_id_arr,
                                int *num_entries) {
  log_id_arr[*num_entries] = log_id;
  *num_entries += 1;

  return;
}

pipe_status_t pipe_mgr_ctxjson_tof_tbl_name_to_log_id(bf_dev_id_t devid,
                                                      profile_id_t prof_id,
                                                      char *tbl_name,
                                                      uint8_t stage_id,
                                                      bool stage_valid,
                                                      uint32_t *log_id_arr,
                                                      int *num_entries,
                                                      int arr_sz) {
  int idx = 0;
  uint32_t bj_hash;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_snapshot_t *snapshot_ptr;
  pipemgr_tbl_pkg_logical_table_details_t *table_ptr;

  *num_entries = 0;
  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth,
      0,
      stage_id + 1,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut,
      0,
      stage_id + 1);
  if (!lut_ptr) {
    LOG_ERROR("%s: Unable to find logical table details from context json",
              __func__);
    return (PIPE_OBJ_NOT_FOUND);
  }
  snapshot_ptr = lut_ptr->u.snapshot_ptr;
  table_ptr = snapshot_ptr->tables;
  if (stage_valid) {
    // Get all tables/tableIDs of the stage
    for (idx = 0; idx < snapshot_ptr->total_tables && *num_entries < arr_sz;
         idx++) {
      populate_log_id_arr(table_ptr->logical_id, log_id_arr, num_entries);
      table_ptr++;
    }
  } else {
    // Get table ID/s that match table name

    rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
    if (!dev_info) {
      LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
      PIPE_MGR_DBGCHK(0);
      return (PIPE_INVALID_ARG);
    }

    int num_stages = dev_info->profile_info[prof_id]->num_stages;
    for (int stage = 0; stage < num_stages; ++stage) {
      bj_hash = bob_jenkin_hash_one_at_a_time(
          PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth,
          0,
          stage + 1,
          0);
      lut_ptr = pipemgr_entry_format_get_lut_entry(
          bj_hash,
          PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth,
          PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut,
          0,
          stage + 1);
      if (!lut_ptr) {
        LOG_ERROR(
            "%s: Unable to find logical table details from context json, dev "
            "%d profile-id %d table %s stage %d",
            __func__,
            devid,
            prof_id,
            tbl_name,
            stage);
        return (PIPE_OBJ_NOT_FOUND);
      }
      snapshot_ptr = lut_ptr->u.snapshot_ptr;
      table_ptr = snapshot_ptr->tables;
      for (idx = 0; idx < snapshot_ptr->total_tables && *num_entries < arr_sz;
           idx++) {
        if (!strcmp(tbl_name,
                    pipemgr_tbl_pkg_get_field_string_name(
                        devid, prof_id, table_ptr->tablename_str_index))) {
          populate_log_id_arr(table_ptr->logical_id, log_id_arr, num_entries);
        }
        table_ptr++;
      }
    }
  }
  return (PIPE_SUCCESS);
}

pipe_status_t pipe_mgr_ctxjson_tof_tbl_name_to_direction(bf_dev_id_t devid,
                                                         profile_id_t prof_id,
                                                         char *tbl_name,
                                                         int *dir) {
  int idx = 0;
  uint32_t bj_hash;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_snapshot_t *snapshot_ptr;
  pipemgr_tbl_pkg_logical_table_details_t *table_ptr;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  int num_stages = dev_info->profile_info[prof_id]->num_stages;
  for (int stage = 0; stage < num_stages; ++stage) {
    bj_hash = bob_jenkin_hash_one_at_a_time(
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth,
        0,
        stage + 1,
        0);
    lut_ptr = pipemgr_entry_format_get_lut_entry(
        bj_hash,
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut_depth,
        PIPE_MGR_TBL_PKG_CTX(devid, prof_id).snapshot_lut,
        0,
        stage + 1);
    if (!lut_ptr) {
      LOG_ERROR(
          "%s: Unable to find logical table details from context json, dev %d "
          "profile-id %d table %s",
          __func__,
          devid,
          prof_id,
          tbl_name);
      return (PIPE_OBJ_NOT_FOUND);
    }
    snapshot_ptr = lut_ptr->u.snapshot_ptr;
    table_ptr = snapshot_ptr->tables;
    for (idx = 0; idx < snapshot_ptr->total_tables; idx++) {
      if (!strcmp(tbl_name,
                  pipemgr_tbl_pkg_get_field_string_name(
                      devid, prof_id, table_ptr->tablename_str_index))) {
        *dir = table_ptr->direction;
        return (PIPE_SUCCESS);
      }
      table_ptr++;
    }
  }
  return (PIPE_INVALID_ARG);
}

/* This is a 4 entry lookup table which is indexed using one bit of s0q1 and
 * one bit of s1qo (s0q1|s1q0, in that order). Given these two bits, it returns
 * a 2 bit value representing key and mask (key|mask, in that order
 */
static uint8_t s0q1_s1q0_decode_[4] = {
    0x1,  /* This means match a ZERO */
    0x0,  /* This means DON'T CARE */
    0xff, /* Invalid */
    0x3   /* This means match a ONE */
};

static void decode_s0q1_s1q0(pipemgr_tbl_pkg_match_entry_line_t *entry_line_ptr,
                             pipe_tbl_match_spec_t *match_spec) {
  pipemgr_tbl_pkg_match_entry_field_t *field;
  unsigned i = 0, j = 0;
  uint8_t *match_value_bits = match_spec->match_value_bits;
  uint8_t *match_mask_bits = match_spec->match_mask_bits;

  /* We have s0q1 and s1q0 fields held in match value bits and match mask bits
   * respectively. Now, for each pair of those bits, convert it to actual
   * match value bits and match mask bits.
   */
  field = entry_line_ptr->fields;
  for (i = 0; i < entry_line_ptr->field_count; i++) {
    if (field->source == TBL_PKG_FIELD_SOURCE_SPEC &&
        (field->match_mode == TBL_PKG_FIELD_MATCHMODE_S0Q1)) {
      uint16_t padded_len = (field->spec_len + 7) / 8;
      padded_len *= 8;
      uint16_t byte_offset = (field->spec_start_bit + padded_len - 1 - 7) / 8;
      byte_offset -= field->fieldsb / 8;
      uint16_t bit_offset = field->fieldsb % 8;
      for (j = 0; j < field->field_width; j++) {
        uint8_t val =
            ((((match_value_bits[byte_offset] >> bit_offset) & 0x1) << 1) |
             ((match_mask_bits[byte_offset] >> bit_offset) & 0x1));
        uint8_t result = s0q1_s1q0_decode_[val];
        /* Create a mask to Zero out the bit in match and mask */
        uint8_t mask = (~(1 << bit_offset)) & 0xff;
        match_mask_bits[byte_offset] &= mask;
        match_value_bits[byte_offset] &= mask;
        match_mask_bits[byte_offset] |= ((result & 0x1) << bit_offset);
        match_value_bits[byte_offset] |= (((result >> 1) & 0x1) << bit_offset);
        bit_offset++;
        if (bit_offset == 8) {
          bit_offset = 0;
          byte_offset--;
        }
      }
    }
    field++;
  }
  return;
}

static void decode_buffer(pipemgr_tbl_pkg_match_entry_field_t *field,
                          uint8_t *value_buf,
                          uint8_t **exm_tbl_word) {
  uint16_t start_offset = (field->spec_start_bit + field->spec_len - 1) - 7;
  uint8_t byte_offset = field->fieldsb / 8;
  uint8_t bit_offset = field->fieldsb % 8;
  start_offset -= (byte_offset * 8);
  start_offset += bit_offset;
  decode_bits_to_spec(value_buf,
                      start_offset,
                      field->field_width,
                      exm_tbl_word,
                      field->memword_index[0],
                      field->field_offset,
                      field->field_width);
}

pipe_status_t pipe_mgr_entry_format_tof_exm_tbl_ent_decode_to_components(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    dev_stage_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint8_t stage_table_handle,
    uint8_t entry_position,
    uint8_t *version_valid_bits,
    pipe_tbl_match_spec_t *match_spec,
    pipe_action_data_spec_t *act_data_spec,
    uint8_t **exm_tbl_word,
    pipe_mgr_exm_hash_info_for_decode_t *hash_info,
    pipe_mgr_indirect_ptrs_t *indirect_ptrs,
    pipe_mgr_ent_decode_ptr_info_t *ptr_info,
    pipe_act_fn_hdl_t *act_fn_hdl_p,
    bool *ram_ids_read,
    bool is_stash,
    uint64_t *proxy_hash) {
  pipemgr_tbl_pkg_exm_stage_hdl_format_t *stage_hdl_ptr;
  pipemgr_tbl_pkg_way_format_t *way_ptr;
  pipemgr_tbl_pkg_match_entry_line_t *entry_line_ptr;
  pipemgr_tbl_pkg_match_entry_field_t *field;
  pipemgr_tbl_pkg_action_entry_field_t *action_hdl_ptr = NULL;
  pipemgr_tbl_pkg_action_parameter_field_t *act_param_ptr = NULL;
  uint32_t hash_way = 0, bj_hash;
  uint8_t sel_len_shift = 0;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  uint32_t i, j;
  unsigned k = 0;
  bool s0q1_s1q0 = false;
  (void)version_valid_bits;
  bool sel_ptr_found = false;
  bool sel_len_found = false;
  bool sel_len_shift_found = false;
  uint32_t indirect_read_ptr = 0;

  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  rmt_dev_profile_info_t *prof_info = dev_info->profile_info[prof_id];

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut_depth,
      mat_tbl_hdl,
      stage_id,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).exm_lut,
      mat_tbl_hdl,
      stage_id);
  if (!lut_ptr) {
    LOG_ERROR("%s: Unable to find entry formatting details for match-tbl 0x%x",
              __func__,
              mat_tbl_hdl);
    return (PIPE_INVALID_ARG);
  }
  stage_hdl_ptr = pipemmgr_entry_format_get_stage_handle_details_ptr(
      stage_table_handle, lut_ptr);
  if (!stage_hdl_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d",
        __func__,
        mat_tbl_hdl,
        stage_id);
    return (PIPE_INVALID_ARG);
  }
  way_ptr = pipemmgr_entry_format_get_hash_way_details_ptr(
      hash_way, stage_hdl_ptr, is_stash);
  if (!way_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d, hash-way %d",
        __func__,
        mat_tbl_hdl,
        stage_id,
        hash_way);
    return (PIPE_INVALID_ARG);
  }
  entry_line_ptr = pipemmgr_entry_format_get_entry_details_ptr(
      entry_position, way_ptr->entry_format);
  if (!entry_line_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for match-tbl 0x%x, stage "
        "%d, entry %d",
        __func__,
        mat_tbl_hdl,
        stage_id,
        entry_position);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  /* Decode the version/valid bits first.  If the entry is not valid return. */
  field = entry_line_ptr->fields;
  for (i = 0; i < entry_line_ptr->field_count; i++) {
    int mem_index = field->memword_index[0];
    if (field->source == TBL_PKG_FIELD_SOURCE_VERSION) {
      *version_valid_bits = get_val(
          exm_tbl_word[mem_index], field->field_offset, field->field_width);
      if (*version_valid_bits == RMT_EXM_ENTRY_VERSION_INVALID) {
        return PIPE_SUCCESS;
      } else {
        match_spec->version_bits = *version_valid_bits;
      }
      break;
    }
    field++;
  }

  /* Get the action function handle */
  *act_fn_hdl_p = get_exm_action_fn_hdl_for_decode(devid,
                                                   prof_id,
                                                   stage_id,
                                                   mat_tbl_hdl,
                                                   stage_table_handle,
                                                   entry_position,
                                                   exm_tbl_word,
                                                   &action_hdl_ptr,
                                                   is_stash);
  if (!action_hdl_ptr) {
    LOG_ERROR(
        "%s: Unable to find action handle decode for match-tbl "
        "0x%x, stage %d",
        __func__,
        mat_tbl_hdl,
        stage_id);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  if (ptr_info) {
    ptr_info->force_stats = action_hdl_ptr->force_stats_addr;
    ptr_info->force_meter = action_hdl_ptr->force_meter_addr;
    ptr_info->force_stful = action_hdl_ptr->force_stful_addr;
  }
  if (proxy_hash) *proxy_hash = 0;

  /* Force version to valid until stash has its own shadow memory implementation
   */
  if (is_stash) {
    *version_valid_bits = RMT_EXM_ENTRY_VERSION_DONT_CARE;
  }
  field = entry_line_ptr->fields;
  for (i = 0; i < entry_line_ptr->field_count; i++) {
    if (field->source == TBL_PKG_FIELD_SOURCE_SPEC) {
      if (field->match_mode == TBL_PKG_FIELD_MATCHMODE_S0Q1) {
        /* Decoding s0q1_s1q0 is done in two phases. In the first phase, the
         * s0q1/s1q0 bits from the passed in RAM word is just put in the
         * appropriate place in the match spec. SOQ1 bits are put in the match
         * spec, S1Q0 bits are put in the matck mask. In the second phase, a
         * pair of these bits are decoded to key and mask based on the lookup
         * table above. */
        s0q1_s1q0 = true;
        decode_buffer(field, match_spec->match_value_bits, exm_tbl_word);
      } else if (field->match_mode == TBL_PKG_FIELD_MATCHMODE_S1Q0) {
        s0q1_s1q0 = true;
        decode_buffer(field, match_spec->match_mask_bits, exm_tbl_word);
      } else if (field->match_mode != TBL_PKG_FIELD_MATCHMODE_S0Q1 &&
                 field->match_mode != TBL_PKG_FIELD_MATCHMODE_S1Q0) {
        decode_buffer(field, match_spec->match_value_bits, exm_tbl_word);
        // Exact match field, populate mask with 0xFF
        uint16_t start_offset = field->spec_start_bit / 8;
        uint8_t end_offset = (field->spec_start_bit + field->spec_len) / 8;
        uint8_t bit_offset = field->spec_start_bit % 8;
        for (j = start_offset; j < end_offset; j++) {
          match_spec->match_mask_bits[j] |= ((1 << (8 - bit_offset)) - 1);
          bit_offset = 0;
        }
        if ((field->spec_start_bit + field->spec_len) % 8 != 0) {
          bit_offset =
              field->spec_start_bit + field->spec_len - (8 * end_offset);
          match_spec->match_mask_bits[j] |= ((1 << bit_offset) - 1)
                                            << (8 - bit_offset);
        }
      }
    } else if (field->source == TBL_PKG_FIELD_SOURCE_IMMEDIATE) {
      if (action_hdl_ptr) {
        if (action_hdl_ptr->act_param) {
          act_param_ptr = action_hdl_ptr->act_param;
          for (k = 0; k < action_hdl_ptr->param_count; k++) {
            if (act_param_ptr->is_parameter != 0) {
              uint16_t start_offset = (act_param_ptr->param_start +
                                       act_param_ptr->param_width - 1) -
                                      7;
              uint8_t byte_offset = act_param_ptr->param_shift / 8;
              uint8_t bit_offset = act_param_ptr->param_shift % 8;
              start_offset -= (byte_offset * 8);
              start_offset += bit_offset;
              decode_bits_to_spec(
                  act_data_spec->action_data_bits,
                  start_offset,
                  act_param_ptr->bit_width,
                  exm_tbl_word,
                  field->memword_index[0],
                  field->field_offset + act_param_ptr->start_offset,
                  act_param_ptr->bit_width);
            }
            act_param_ptr++;
          }
        }
      }
    } else if (field->source == TBL_PKG_FIELD_SOURCE_ADTPTR && indirect_ptrs) {
      indirect_read_ptr = extract_val(exm_tbl_word[field->memword_index[0]],
                                      field->field_offset,
                                      field->field_width);
      indirect_ptrs->adt_ptr |= indirect_read_ptr << field->fieldsb;
      if (!field->perflow) {
        indirect_ptrs->adt_ptr |= 1 << TOF_ADT_ADDR_PFE_BIT_POSITION;
      }
    } else if (field->source == TBL_PKG_FIELD_SOURCE_SELPTR && indirect_ptrs) {
      indirect_read_ptr = extract_val(exm_tbl_word[field->memword_index[0]],
                                      field->field_offset,
                                      field->field_width);
      indirect_ptrs->sel_ptr |= indirect_read_ptr << field->fieldsb;
      sel_ptr_found = true;
    } else if (field->source == TBL_PKG_FIELD_SOURCE_METERPTR &&
               indirect_ptrs) {
      indirect_read_ptr = extract_val(exm_tbl_word[field->memword_index[0]],
                                      field->field_offset,
                                      field->field_width);
      if (!transform_ptr(prof_info)) {
        indirect_ptrs->meter_ptr |= indirect_read_ptr << field->fieldsb;
      } else {
        indirect_ptrs->meter_ptr = pipemgr_tbl_pkg_reverse_transform_meter_ptr(
            indirect_read_ptr, field, action_hdl_ptr);
      }
    } else if (field->source == TBL_PKG_FIELD_SOURCE_STATSPTR &&
               indirect_ptrs) {
      indirect_read_ptr = extract_val(exm_tbl_word[field->memword_index[0]],
                                      field->field_offset,
                                      field->field_width);
      if (!transform_ptr(prof_info)) {
        indirect_ptrs->stats_ptr |= indirect_read_ptr << field->fieldsb;
      } else {
        indirect_ptrs->stats_ptr = indirect_read_ptr;
        if (!field->perflow) {
          indirect_ptrs->stats_ptr |= (1 << field->field_width);
        }
        /* Now, left shift the address by the number of bits defaulted by the
         * compiler.
         */
        indirect_ptrs->stats_ptr <<= field->fieldsb;
      }
    } else if (field->source == TBL_PKG_FIELD_SOURCE_STFLPTR && indirect_ptrs) {
      indirect_read_ptr = extract_val(exm_tbl_word[field->memword_index[0]],
                                      field->field_offset,
                                      field->field_width);
      if (!transform_ptr(prof_info)) {
        indirect_ptrs->stfl_ptr |= indirect_read_ptr << field->fieldsb;
      } else {
        indirect_ptrs->stfl_ptr = indirect_read_ptr;
        /* Stateful address contain the 23 bits (including the LSB zeros)
         * followed
         * a PFE bit, followed by a default "1" and followed by 2 bits of
         * stateful ALU instruction index. If a PFE is present then it will
         * in the bit position (field_length - 2 - 1). When we return the
         * stateful address, we don't care about the opcode part here. Hence
         * when putting the PFE bit back, we just put it as the 24th bit of the
         * extracted address which may clobber a bit in the opcode, but that
         * is not used to interpret the stateful address from this decode.
         */
        if (!field->perflow) {
          indirect_ptrs->stfl_ptr |= (1 << (field->field_width - 2));
        }
        /* Pad the LSBs with appropriate number of zeros */
        indirect_ptrs->stfl_ptr <<= field->fieldsb;
      }
    } else if (field->source == TBL_PKG_FIELD_SOURCE_SELLEN && indirect_ptrs) {
      indirect_ptrs->sel_len =
          extract_val(exm_tbl_word[field->memword_index[0]],
                      field->field_offset,
                      field->field_width);
      sel_len_found = true;
      if (sel_len_shift_found) {
        indirect_ptrs->sel_len <<= sel_len_shift;
      }
    } else if (field->source == TBL_PKG_FIELD_SOURCE_SELLENSHIFT &&
               indirect_ptrs) {
      sel_len_shift_found = true;
      sel_len_shift = extract_val(exm_tbl_word[field->memword_index[0]],
                                  field->field_offset,
                                  field->field_width);
      if (sel_len_found) {
        indirect_ptrs->sel_len <<= sel_len_shift;
      }
    } else if (field->source == TBL_PKG_FIELD_SOURCE_PROXYHASH && proxy_hash) {
      *proxy_hash |= (extract_val(exm_tbl_word[field->memword_index[0]],
                                  field->field_offset,
                                  field->field_width)
                      << field->fieldsb);
    }
    if (ram_ids_read) {
      /* Entry decode provides information about the the ram ids read in the
       * wide word, which can be used to restore state about where this entry
       * lives, the ram-id(s).
       */
      for (j = field->memword_index[0]; j < field->memword_index[1] + 1; j++) {
        ram_ids_read[j] = true;
      }
    }
    field++;
  }
  if (s0q1_s1q0) {
    decode_s0q1_s1q0(entry_line_ptr, match_spec);
  } else {
    /* Compute ghost bits, if any */
    pipe_status_t status = bf_hash_mat_entry_ghost_bits_compute(
        devid, prof_id, stage_id, mat_tbl_hdl, match_spec, hash_info);
    if (status != PIPE_SUCCESS) {
      LOG_ERROR(
          "%s:%d Error in computing ghost bits for tbl 0x%x, device id %d, err "
          "%s",
          __func__,
          __LINE__,
          mat_tbl_hdl,
          devid,
          pipe_str_err(status));
      return status;
    }
  }

  /* Since the sel len and sel ptr could be read out of order, perform the
   * selector ptr decoding after all fields are found. If the selector length
   * is not found, set it to the default value of 1 word.
   */
  if (sel_ptr_found) {
    if (!sel_len_found) {
      if (indirect_ptrs->sel_ptr & (1 << TOF_METER_ADDR_PFE_BIT_POSITION)) {
        indirect_ptrs->sel_len = 1;
      } else {
        indirect_ptrs->sel_len = 0;
      }
    }
    indirect_ptrs->sel_ptr &= ~(1 << TOF_METER_ADDR_PFE_BIT_POSITION);
  }
  return PIPE_SUCCESS;
}

static void decode_range_fields(pipemgr_tbl_pkg_lut_t *lut_ptr,
                                uint8_t **tbl_words,
                                uint8_t *match_value_bits,
                                uint8_t *match_mask_bits,
                                bool hi_byte) {
  pipemgr_tbl_pkg_tern_entry_t *entry;
  pipemgr_tbl_pkg_tern_entry_field_t *field;

  entry = lut_ptr->u.tern_ptr->entry;
  unsigned i = 0, j = 0;
  for (i = 0; i < lut_ptr->u.tern_ptr->entrycount; i++) {
    field = entry->field;
    for (j = 0; j < entry->entryfieldcount; j++) {
      switch (field->location) {
        case TBL_PKG_TERN_FIELD_LOCATION_RANGE:
          if (field->range_hi_byte == hi_byte) {
            decode_range_to_match_spec(
                tbl_words[field->memword_index[0]],
                &tbl_words[field->memword_index[0]][8],
                field->srcoffset,
                field->src_len,
                field->startbit,
                field->lsbmemwordoffset + field->range_nibble_offset,
                field->bitwidth,
                field->range_type,
                field->range_hi_byte ? 1 : 0,
                match_value_bits,
                match_mask_bits);
          }
          break;
        default:
          break;
      }
      field++;
    }
  }
  return;
}

pipe_status_t pipe_mgr_entry_format_tof_tern_tbl_ent_decode_to_match_spec(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec,
    uint8_t **tbl_words,
    uint8_t *version_key_p,
    uint8_t *version_mask_p,
    bool *is_range_entry_head) {
  // uint64_t range_mask[TOF_MAX_TCAM_WORDS_IN_TERN_TBL_WORD] = {0};
  uint32_t bj_hash, i, j;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_tern_entry_t *entry;
  pipemgr_tbl_pkg_tern_entry_field_t *field, *version_field = NULL;
  bool range = false;
  bool mrd_terminate = true;

  *version_key_p = 0x3;
  *version_mask_p = 0;
  rmt_dev_info_t *dev_info = pipe_mgr_get_dev_info(devid);
  if (!dev_info) {
    LOG_ERROR("%s: Unable to use dev id %d", __func__, devid);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tern_lut_depth,
      mat_tbl_hdl,
      stage_id,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tern_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).tern_lut,
      mat_tbl_hdl,
      stage_id);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find entry formatting details for tern table handle "
        "0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }
  entry = lut_ptr->u.tern_ptr->entry;

  for (i = 0; i < lut_ptr->u.tern_ptr->entrycount; i++) {
    field = entry->field;
    // First, find the version field
    for (j = 0; j < entry->entryfieldcount; j++) {
      if (field->location == TBL_PKG_TERN_FIELD_LOCATION_VERSION) {
        version_field = field;
        break;
      }
      field++;
    }

    if (!version_field) {




        /* Versioning is disabled, let's check if the entry is valid or not */
        if (tbl_words[0][0] == 1 && !tbl_words[0][8])
          *version_mask_p = 0; /* Invalid entry */
        else
          *version_mask_p = 1; /* Valid entry */

    }

    field = entry->field;
    for (j = 0; j < entry->entryfieldcount; j++) {
      switch (field->location) {
        case TBL_PKG_TERN_FIELD_LOCATION_ZERO:
        case TBL_PKG_TERN_FIELD_LOCATION_PARITY:
          break;
        case TBL_PKG_TERN_FIELD_LOCATION_PAYLOAD:
          /* MRD bit is in word0, payload bit is in word1.  If the MRD bit is
           * set in all TCAMs of the wide word then the entry is the head of a
           * range. */
          mrd_terminate = mrd_terminate &&
                          (get_val_reverse(tbl_words[field->memword_index[0]],
                                           field->lsbmemwordoffset,
                                           field->bitwidth)
                               ? true
                               : false);
          break;
        case TBL_PKG_TERN_FIELD_LOCATION_VERSION: {
          /* The version valid key is in LSB of Word1 and the version valid mask
           * is in LSB of Word0 of the read TCAM
           */

          uint8_t *word0 = tbl_words[field->memword_index[0]];
          uint8_t *word1 = &tbl_words[field->memword_index[0]][8];

          uint64_t w0_val = 0;
          uint64_t w1_val = 0;
          unsigned index;
          for (index = 0; index < 6; index++) {
            w0_val |= (uint64_t)word0[index] << (8 * index);
            w1_val |= (uint64_t)word1[index] << (8 * index);
          }

          uint8_t v0, v1;
          v0 = (w0_val >> field->lsbmemwordoffset) & 0x3;
          v1 = (w1_val >> field->lsbmemwordoffset) & 0x3;

          *version_key_p = (~v0) & 0x3;
          *version_mask_p = (v0 ^ v1) & 0x3;
        } break;
        case TBL_PKG_TERN_FIELD_LOCATION_RANGE:
          /* Range field decoding is done separately for various reasons. */
          range = true;
          break;
        case TBL_PKG_TERN_FIELD_LOCATION_SPEC: {
          bool version_word =
              (version_field &&
               version_field->memword_index[0] == field->memword_index[0]);
          uint16_t start_offset = (field->srcoffset + field->src_len - 1 - 7);
          uint8_t byte_offset = field->startbit / 8;
          uint8_t bit_offset = field->startbit % 8;
          start_offset -= (byte_offset * 8);
          start_offset += bit_offset;
          extract_key_mask_to_match_spec(tbl_words[field->memword_index[0]],
                                         &tbl_words[field->memword_index[0]][8],
                                         field->lsbmemwordoffset,
                                         field->bitwidth,
                                         match_spec->match_value_bits,
                                         match_spec->match_mask_bits,
                                         start_offset,
                                         version_word);
        } break;
        default:
          LOG_ERROR(
              "%s: Unable to find entry formatting details for tern table "
              "handle "
              "0x%x",
              __func__,
              mat_tbl_hdl);
          PIPE_MGR_DBGCHK(0);
          return (PIPE_INVALID_ARG);
      }
      field++;
    }
  }

  if (range) {
    /* If there was a range field involved, decode them. The reason why this
     * is done separately is to not be dependent on the order of processing
     * of range fields. Range fields are published in the entry format JSON
     * as nibbles (we use 4-bit range mode). Each nibble in the word0
     * corresponds either to bits (4-7) or bits (0-3) of the encoding. Similarly
     * each nibble ins word1 either corresponds to bits (8-11) or (12-15) of
     * the encoding of that particular field. Which bits it corrsponds to is
     * indicated by the hi_byte flag that we pass in. We always first process
     * the hi_byte fields first and then the non hi-byte fields. The decoding
     * logic depends on this.
     */
    decode_range_fields(lut_ptr,
                        tbl_words,
                        match_spec->match_value_bits,
                        match_spec->match_mask_bits,
                        true);
    decode_range_fields(lut_ptr,
                        tbl_words,
                        match_spec->match_value_bits,
                        match_spec->match_mask_bits,
                        false);
  }
  *is_range_entry_head = mrd_terminate;
  return PIPE_SUCCESS;
}
pipe_status_t pipe_mgr_entry_format_construct_match_key_mask_spec(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec) {
  uint32_t bj_hash, i;
  pipemgr_tbl_pkg_lut_t *lut_ptr;
  pipemgr_tbl_pkg_spec_field_t *field_ptr;
  pipemgr_tbl_pkg_spec_t *matchspec_ptr;
  size_t total_len = 0;
  size_t field_len = 0;
  size_t field_len_left = 0;
  size_t num_bits = 0;
  size_t idx = 0;

  if (!match_spec) {
    return PIPE_SUCCESS;
  }

  bj_hash = bob_jenkin_hash_one_at_a_time(
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).matchspec_lut_depth,
      mat_tbl_hdl,
      0,
      0);
  lut_ptr = pipemgr_entry_format_get_lut_entry(
      bj_hash,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).matchspec_lut_depth,
      PIPE_MGR_TBL_PKG_CTX(devid, prof_id).matchspec_lut,
      mat_tbl_hdl,
      0);
  if (!lut_ptr) {
    LOG_ERROR(
        "%s: Unable to find match spec details for match table "
        "handle 0x%x",
        __func__,
        mat_tbl_hdl);
    PIPE_MGR_DBGCHK(0);
    return (PIPE_INVALID_ARG);
  }

  matchspec_ptr = lut_ptr->u.matchspec_ptr;
  field_ptr = matchspec_ptr->fields;
  for (i = 0; i < matchspec_ptr->fieldcount; i++) {
    total_len += (field_ptr->fieldwidth + 7) >> 3;
    field_ptr++;
  }
  if (match_spec->num_match_bytes != total_len) {
    LOG_ERROR(
        "%s:%d Match spec mask has length %d, expected length %zd for "
        "dkm tbl 0x%x device id %d",
        __func__,
        __LINE__,
        match_spec->num_match_bytes,
        total_len,
        mat_tbl_hdl,
        devid);
    PIPE_MGR_DBGCHK(match_spec->num_match_bytes == total_len);
    return PIPE_INVALID_ARG;
  }

  field_ptr = matchspec_ptr->fields;
  for (i = 0; i < matchspec_ptr->fieldcount; i++) {
    field_len = (field_ptr->fieldwidth + 7) >> 3;
    field_len_left = field_ptr->fieldwidth;
    for (idx = 0; idx < field_len; idx++) {
      if (field_len_left % 8 != 0) {
        // First iter in a padded field
        num_bits = field_len_left - (field_len - 1) * 8;
        match_spec->match_mask_bits[(field_ptr->startbit >> 3) + idx] =
            ((1 << num_bits) - 1);
        field_len_left -= num_bits;
      } else {
        match_spec->match_mask_bits[(field_ptr->startbit >> 3) + idx] = 0xff;
        field_len_left -= 8;
      }
    }
    field_ptr++;
  }

  return PIPE_SUCCESS;
}
