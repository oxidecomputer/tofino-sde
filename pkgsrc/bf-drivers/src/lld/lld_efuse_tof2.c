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
#include <inttypes.h>  // for PRIx64
#else
#include <bf_types/bf_kernel_types.h>
#include <linux/delay.h>
#endif
#ifdef __sun
#include <illumos.h>
#endif

#include <bf_types/bf_types.h>
#include <lld/lld_reg_if.h>
#include <lld/lld_bits.h>
#include <lld/lld_err.h>
#include <lld/lld_sku.h>
#include "lld.h"
#include "lld_map.h"
#include "lld_log.h"
#include "lld_efuse_tof2.h"
#include <tof2_regs/tof2_reg_drv.h>

#ifndef __KERNEL__
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __sun
#include <illumos.h>
#else
#include <byteswap.h>
#endif // __sun
#include <target-utils/third-party/cJSON/cJSON.h>
#include <ctx_json/ctx_json_utils.h>
#endif

#define EFUSE_WORDS_TOF2 16

/* clang-format off */
static bf_dev_id_t device_id[2] = {  0,  15};
static int version[2]       = { 16,  17};
static int freq_dis[2]      = { 18,  18};
static int freq_bps[2]      = { 19,  20};
static int freq_pps[2]      = { 21,  22};
static int pcie_dis[2]      = { 23,  24};
static int cpu_speed_dis[2] = { 25,  26};
static int speed_dis[2]     = { 27,  90};
static int port_dis[2]      = { 91, 130};
static int pipe_dis[2]      = {131, 134};
static int pipe0_mau_dis[2] = {135, 155};
static int pipe1_mau_dis[2] = {156, 176};
static int pipe2_mau_dis[2] = {177, 197};
static int pipe3_mau_dis[2] = {198, 218};
static int tm_mem_dis[2]    = {219, 250};
static int bsync_dis[2]     = {251, 251};
static int pgen_dis[2]      = {252, 252};
static int resub_dis[2]     = {253, 253};
static int vid[2]           = {254, 265};
static int rsvd_22[2]       = {266, 287};
static int part_num[2]      = {288, 301};
static int rev_num[2]       = {302, 309};
static int pkg_id[2]        = {310, 311};
static int silent_spin[2]   = {312, 313};
static bf_dev_id_t chip_id[2] = {314, 376};//combination of multiple fields
static int fab_id_num[2]    = {314, 320};
static int lot_id_num[2]    = {321, 327};
static int lot_num[2]       = {328, 355};
static int wafer_num[2]     = {356, 360};
static int xsign[2]         = {361, 361};
static int x_coord[2]       = {362, 368};
static int ysign[2]         = {369, 369};
static int y_coord[2]       = {370, 376};
static int pmro[2]          = {377, 388};
static int wf_core_repair[2]= {389, 389};
static int core_repair[2]   = {390, 390};
static int tile_repair[2]   = {391, 391};
static int freq_bps_2[2]    = {392, 395};
static int freq_pps_2[2]    = {396, 399};
static int die_rotation[2]  = {400, 400};
static int soft_pipe_dis[2] = {401, 404};
static int rsvd_113[2]      = {405, 511};
/* clang-format on */

#if !defined DEVICE_IS_EMULATOR || DEVICE_IS_EMULATOR != 2
static void lld_delay(int microsec) {
#ifndef __KERNEL__
  bf_sys_usleep(microsec);
#else
  usleep_range(microsec, microsec + 10);
#endif
}

/* Helper functions to program soft efuse. */
static void wr_tcu_ctrl(bf_dev_id_t dev_id, int which, uint32_t val) {
  uint32_t addr = which ? tof2_reg_device_select_misc_regs_tcu_control1_address
                        : tof2_reg_device_select_misc_regs_tcu_control0_address;
  lld_write_register(dev_id, addr, val);
  lld_delay(100);
#ifdef DEVICE_IS_EMULATOR
  lld_delay(10 * 1000);
#endif
}
static void poll_wrack(bf_dev_id_t dev_id) {
  uint32_t i, x = 0;
  for (i = 0; i < 1000; ++i) {
    lld_read_register(
        dev_id, tof2_reg_device_select_misc_regs_tcu_wrack_address, &x);
    if (x == 1) return;
    lld_delay(10);
#ifdef DEVICE_IS_EMULATOR
    lld_delay(10 * 1000);
#endif
  }
  lld_log("TCU WR ACK did not go to zero (0x%x)", x);
}
static void verify_tcu_status(bf_dev_id_t dev_id, uint32_t expected) {
  uint32_t x = 0;
  lld_read_register(
      dev_id, tof2_reg_device_select_misc_regs_tcu_status_address, &x);
  if (x == expected) return;
  lld_log("TCU STATUS did not go to expected value (actual 0x%x expected 0x%x)",
          x,
          expected);
}
static void verify_tcu_status_dc(bf_dev_id_t dev_id) {
  uint32_t x = 0;
  lld_read_register(
      dev_id, tof2_reg_device_select_misc_regs_tcu_status_address, &x);
}
static void csr2jtag_soft_efuse(bf_dev_id_t dev_id, uint32_t *efuse_bits) {
  int i;
  /* Flush hold_din register first. */
  for (i = 0; i < 16; ++i) wr_tcu_ctrl(dev_id, 0, 0x00000000);
  wr_tcu_ctrl(dev_id, 1, 0x80001FF1);
  poll_wrack(dev_id);
  verify_tcu_status_dc(dev_id);

  /* Program new soft efuse values. */
  wr_tcu_ctrl(dev_id, 0, 0x00000027);
  wr_tcu_ctrl(dev_id, 1, 0x80000071);
  poll_wrack(dev_id);
  verify_tcu_status(dev_id, 0x00000001);

  for (i = 0; i < 16; ++i) wr_tcu_ctrl(dev_id, 0, efuse_bits[i]);
  wr_tcu_ctrl(dev_id, 1, 0x80001FF4);
  for (i = 0; i < 16; ++i) {
    poll_wrack(dev_id);
    verify_tcu_status(dev_id, 0x00000000);
  }

  wr_tcu_ctrl(dev_id, 0, 0x00000027);
  wr_tcu_ctrl(dev_id, 1, 0x80000071);
  poll_wrack(dev_id);
  verify_tcu_status(dev_id, 0x00000001);

  for (i = 0; i < 16; ++i) wr_tcu_ctrl(dev_id, 0, 0xE0000007);
  wr_tcu_ctrl(dev_id, 1, 0x80001FF4);
  for (i = 0; i < 16; ++i) {
    poll_wrack(dev_id);
    verify_tcu_status(dev_id, 0x00000000);
  }
}

#ifndef __KERNEL__
/* Helper functions to create the 512 bits of efuse data to program. */
static void set_bits(uint32_t *efuse, int *se, uint64_t val) {
  int start = se[0];
  int end = se[1];
  for (; start <= end; ++start) {
    efuse[start / 32] &= ~(1u << (start % 32));
    efuse[start / 32] |= (val & 1) << (start % 32);
    val = val >> 1;
  }
}

static void efuse_json_to_bits(cJSON *json, uint32_t *efuse) {
  cJSON *i = NULL, *j = NULL;
  if (bf_cjson_has_int(json, "device_id")) {
    int x = 0;
    bf_cjson_try_get_int(json, "device_id", &x);
    set_bits(efuse, device_id, x);
  }
  if (bf_cjson_has_int(json, "versioning")) {
    int x = 0;
    bf_cjson_try_get_int(json, "versioning", &x);
    set_bits(efuse, version, x);
  }
  if (bf_cjson_has_int(json, "freq_dis")) {
    int x = 0;
    bf_cjson_try_get_int(json, "freq_dis", &x);
    set_bits(efuse, freq_dis, x);
  }
  if (bf_cjson_has_int(json, "freq_bps")) {
    int x = 0;
    bf_cjson_try_get_int(json, "freq_bps", &x);
    set_bits(efuse, freq_bps, x);
  }
  if (bf_cjson_has_int(json, "freq_pps")) {
    int x = 0;
    bf_cjson_try_get_int(json, "freq_pps", &x);
    set_bits(efuse, freq_pps, x);
  }
  if (bf_cjson_has_int(json, "pcie_dis")) {
    int x = 0;
    bf_cjson_try_get_int(json, "pcie_dis", &x);
    set_bits(efuse, pcie_dis, x);
  }
  if (bf_cjson_has_int(json, "cpu_port_speed")) {
    int x = 0;
    bf_cjson_try_get_int(json, "cpu_port_speed", &x);
    set_bits(efuse, cpu_speed_dis, x);
  }
  if (bf_cjson_has_int(json, "rotated")) {
    int x = 0;
    bf_cjson_try_get_int(json, "rotated", &x);
    set_bits(efuse, die_rotation, x);
  }

  /* Eth port speed reduction, 2 bits per MAC, 32 MACs. */
  uint64_t eth_port_speed = 0;
  bf_cjson_try_get_object(json, "eth_port_speed", &i);
  if (i) {
    CTX_JSON_FOR_EACH(j, i) {
      int mac = 0, val = 0;
      bf_cjson_try_get_int(j, "mac", &mac);
      bf_cjson_try_get_int(j, "val", &val);
      eth_port_speed |= (uint64_t)val << (mac * 2);
    }
    set_bits(efuse, speed_dis, eth_port_speed);
  }

  /* Port Disable */
  uint64_t port_disable = 0;
  i = NULL, j = NULL;
  bf_cjson_try_get_object(json, "port_disable", &i);
  if (i) {
    CTX_JSON_FOR_EACH(j, i) { port_disable |= 1 << j->valueint; }
    set_bits(efuse, port_dis, port_disable);
  }

  /* Pipe Disable */
  i = NULL, j = NULL;
  bf_cjson_try_get_object(json, "pipe_disable", &i);
  if (i) {
    int x = 0;
    CTX_JSON_FOR_EACH(j, i) { x |= 1 << j->valueint; }
    set_bits(efuse, pipe_dis, x);
  }

  /* Soft Pipe Disable */
  i = NULL, j = NULL;
  bf_cjson_try_get_object(json, "soft_pipe_disable", &i);
  if (i) {
    int x = 0;
    CTX_JSON_FOR_EACH(j, i) { x |= 1 << j->valueint; }
    set_bits(efuse, soft_pipe_dis, x);
  }

  /* Pipe Stage Disable */
  for (int p = 0; p < 4; ++p) {
    int x = 0;
    char *keys[] = {"p0_stage_disable",
                    "p1_stage_disable",
                    "p2_stage_disable",
                    "p3_stage_disable"};
    int *setters[] = {
        pipe0_mau_dis, pipe1_mau_dis, pipe2_mau_dis, pipe3_mau_dis};
    i = NULL, j = NULL;
    bf_cjson_try_get_object(json, keys[p], &i);
    if (i) {
      CTX_JSON_FOR_EACH(j, i) { x |= 1 << j->valueint; }
      set_bits(efuse, setters[p], x);
    }
  }

  if (bf_cjson_has_int(json, "tm_mem_dis")) {
    int x = 0;
    bf_cjson_try_get_int(json, "tm_mem_dis", &x);
    set_bits(efuse, tm_mem_dis, x);
  }
  if (bf_cjson_has_int(json, "bsync_dis")) {
    int x = 0;
    bf_cjson_try_get_int(json, "bsync_dis", &x);
    set_bits(efuse, bsync_dis, x);
  }
  if (bf_cjson_has_int(json, "pgen_dis")) {
    int x = 0;
    bf_cjson_try_get_int(json, "pgen_dis", &x);
    set_bits(efuse, pgen_dis, x);
  }
  if (bf_cjson_has_int(json, "resub_dis")) {
    int x = 0;
    bf_cjson_try_get_int(json, "resub_dis", &x);
    set_bits(efuse, resub_dis, x);
  }
  if (bf_cjson_has_int(json, "v_scale")) {
    int x = 0;
    bf_cjson_try_get_int(json, "v_scale", &x);
    set_bits(efuse, vid, x);
  }
  if (bf_cjson_has_int(json, "tile_repair")) {
    int x = 0;
    bf_cjson_try_get_int(json, "tile_repair", &x);
    set_bits(efuse, tile_repair, x);
  }
  if (bf_cjson_has_int(json, "core_repair")) {
    int x = 0;
    bf_cjson_try_get_int(json, "core_repair", &x);
    set_bits(efuse, core_repair, x);
  }

  if (bf_cjson_has_hex(json, "chip_id")) {
    uint64_t chip_id_val = 0;
    bf_cjson_try_get_hex(json, "chip_id", (uint8_t *)&chip_id_val, 8);
    set_bits(efuse, chip_id, bswap_64(chip_id_val));
  }

  if (bf_cjson_has_int(json, "silent_spin")) {
    int x = 0;
    bf_cjson_try_get_int(json, "silent_spin", &x);
    set_bits(efuse, silent_spin, x);
  }
  if (bf_cjson_has_int(json, "pkg_id")) {
    int x = 0;
    bf_cjson_try_get_int(json, "pkg_id", &x);
    set_bits(efuse, pkg_id, x);
  }
  if (bf_cjson_has_int(json, "revision")) {
    int x = 0;
    bf_cjson_try_get_int(json, "revision", &x);
    set_bits(efuse, rev_num, x);
  }
  if (bf_cjson_has_int(json, "part_num")) {
    int x = 0;
    bf_cjson_try_get_int(json, "part_num", &x);
    set_bits(efuse, part_num, x);
  }
}
#endif  // __KERNEL__

static int prog_soft_efuse(bf_dev_id_t dev_id) {
  uint32_t efuse_bits[EFUSE_WORDS_TOF2] = {0};
  uint32_t programmed_efuse_bits[EFUSE_WORDS_TOF2] = {0};
  int efuse_from_file = 0;
  int failure, i;
  uint32_t fuse_addr = tof2_reg_device_select_misc_regs_func_fuse_address;
#ifndef __KERNEL__
  FILE *f;
  if ((f = fopen("/efuse", "r")) != NULL) {
    for (i = 0; i < EFUSE_WORDS_TOF2; i++) {
      int rc = lld_read_register(dev_id, fuse_addr + (4 * i), &efuse_bits[i]);
      if (rc) {
        fclose(f);
        return -1;
      }
    }
    struct stat stat_b;
    fstat(fileno(f), &stat_b);
    size_t f_sz = stat_b.st_size + 1;
    char *b = bf_sys_calloc(1, f_sz);
    size_t items_read = fread(b, 1, f_sz, f);
    if (items_read != 0) {
      cJSON *json = cJSON_Parse(b);
      if (json) {
        efuse_json_to_bits(json, efuse_bits);
        efuse_from_file = 1;
        cJSON_Delete(json);
      }
    }
    bf_sys_free(b);
    fclose(f);
  }
#endif  // __KERNEL__
  /* If there was no efuse requested in a file do not attempt to soft efuse the
   * part. */
  if (!efuse_from_file) return 0;

  csr2jtag_soft_efuse(dev_id, efuse_bits);

  /* Verify the programming by reading the efuse registers and checking they're
   * equal to the data programmed. */
  failure = 0;
  for (i = 0; i < EFUSE_WORDS_TOF2; i++) {
    lld_read_register(dev_id, fuse_addr + (4 * i), &programmed_efuse_bits[i]);
    if (programmed_efuse_bits[i] != efuse_bits[i]) failure = 1;
  }
  if (failure) {
    lld_log("Soft EFuse FAILED");
    lld_log("   Attempted  Read-Back");
    for (i = 0; i < EFUSE_WORDS_TOF2; i++) {
      lld_log("%2d 0x%08x 0x%08x", i, efuse_bits[i], programmed_efuse_bits[i]);
    }
    return -3;
  }
  return 0;
}
#endif  // DEVICE_IS_EMULATOR != 2

static uint64_t get_bits(uint32_t *efuse, int *se) {
  int start = se[0];
  int end = se[1];
  uint64_t r = 0;
  for (; end >= start; --end) {
    r = (r << 1) | ((efuse[end / 32] >> (end % 32)) & 1);
  }
  return r;
}

int lld_efuse_tof2_load(bf_dev_id_t dev_id, bf_dev_init_mode_t warm_init_mode) {
  lld_dev_t *dev_p = lld_map_dev_id_to_dev_p_allow_unassigned(dev_id);
  lld_efuse_data_t *e = NULL;
  int i;
  uint32_t efuse_bits[EFUSE_WORDS_TOF2] = {0};
#if !defined DEVICE_IS_EMULATOR || DEVICE_IS_EMULATOR != 2
  bool is_sw_model = false;
#endif
  if (dev_p == NULL) return -1;

#if !defined DEVICE_IS_EMULATOR || DEVICE_IS_EMULATOR != 2
  bf_drv_device_type_get(dev_id, &is_sw_model);
  if (!is_sw_model && warm_init_mode == BF_DEV_INIT_COLD)
    prog_soft_efuse(dev_id);
#else
  (void)warm_init_mode;
#endif

  for (i = 0; i < EFUSE_WORDS_TOF2; i++) {
    int rc = lld_read_register(
        dev_id,
        tof2_reg_device_select_misc_regs_func_fuse_address + (4 * i),
        &efuse_bits[i]);
    if (rc) {
      return -1;
    }
  }

  e = &dev_p->efuse_data;
  e->device_id = get_bits(efuse_bits, device_id);
  e->resubmit_disable = get_bits(efuse_bits, resub_dis);
  e->mau_tcam_reduction = 0;
  e->mau_sram_reduction = 0;
  e->packet_generator_disable = get_bits(efuse_bits, pgen_dis);
  e->pipe_disable = get_bits(efuse_bits, pipe_dis);
  e->mau_stage_disable = 0;
  e->port_disable_map_hi = 0;
  e->port_disable_map_lo = get_bits(efuse_bits, port_dis);
  e->tm_memory_disable = get_bits(efuse_bits, tm_mem_dis);
  e->port_speed_reduction = 0;
  e->cpu_port_speed_reduction = get_bits(efuse_bits, cpu_speed_dis);
  e->pcie_lane_reduction = get_bits(efuse_bits, pcie_dis);
  e->baresync_disable = get_bits(efuse_bits, bsync_dis);
  e->frequency_reduction = get_bits(efuse_bits, freq_pps) << 2;
  e->frequency_reduction |= get_bits(efuse_bits, freq_bps);
  e->frequency_check_disable = get_bits(efuse_bits, freq_dis);
  e->versioning = get_bits(efuse_bits, version);
  e->chip_part_number = get_bits(efuse_bits, part_num);
  e->part_revision_number = get_bits(efuse_bits, rev_num);
  e->package_id = get_bits(efuse_bits, pkg_id);
  e->silent_spin = get_bits(efuse_bits, silent_spin);
  e->chip_id = get_bits(efuse_bits, chip_id);
  e->pmro_and_skew = get_bits(efuse_bits, pmro);
  e->voltage_scaling = get_bits(efuse_bits, vid);
  e->soft_pipe_dis = get_bits(efuse_bits, soft_pipe_dis);
  e->tof2_pipe_mau_stage_disable[0] = get_bits(efuse_bits, pipe0_mau_dis);
  e->tof2_pipe_mau_stage_disable[1] = get_bits(efuse_bits, pipe1_mau_dis);
  e->tof2_pipe_mau_stage_disable[2] = get_bits(efuse_bits, pipe2_mau_dis);
  e->tof2_pipe_mau_stage_disable[3] = get_bits(efuse_bits, pipe3_mau_dis);
  e->tof2_eth_port_speed_reduction = get_bits(efuse_bits, speed_dis);
  e->tof2_die_rotate = get_bits(efuse_bits, die_rotation);

  (void)fab_id_num;
  (void)lot_id_num;
  (void)lot_num;
  (void)wafer_num;
  (void)xsign;
  (void)x_coord;
  (void)ysign;
  (void)y_coord;
  (void)wf_core_repair;
  (void)freq_bps_2;
  (void)freq_pps_2;
  (void)core_repair;
  (void)tile_repair;
  (void)rsvd_22;
  (void)soft_pipe_dis;
  (void)rsvd_113;
  return 0;
}

void lld_efuse_tof2_wafer_str_get(bf_dev_id_t dev_id, int s_len, char *s) {
#ifndef __KERNEL__
  uint64_t id = lld_efuse_get_chip_id(dev_id);
  uint32_t fab_id = id & 0x7F;
  id >>= 7;
  uint32_t lot_id = id & 0x7F;
  id >>= 7;
  uint32_t lot_n = id & 0x0FFFFFFF;
  id >>= 28;
  uint32_t wafer = id & 0x1F;
  id >>= 5;
  uint32_t x_sign = id & 1;
  id >>= 1;
  uint32_t x = id & 0x7F;
  id >>= 7;
  uint32_t y_sign = id & 1;
  id >>= 1;
  uint32_t y = id & 0x7F;
  char lot_c[4] = {(lot_n >> 21) & 0x7F,
                   (lot_n >> 14) & 0x7F,
                   (lot_n >> 7) & 0x7F,
                   lot_n & 0x7F};
  snprintf(s,
           s_len,
           "Lot %c%c%c%c%c%c Wafer %d X=%c%d Y=%c%d",
           (char)fab_id,
           (char)lot_id,
           lot_c[3],
           lot_c[2],
           lot_c[1],
           lot_c[0],
           wafer,
           x_sign ? '-' : '+',
           x,
           y_sign ? '-' : '+',
           y);
#else
  (void)dev_id;
  (void)s;
#endif
}
