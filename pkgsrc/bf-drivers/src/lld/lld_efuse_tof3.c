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
#include "lld_efuse_tof3.h"
#include <tof3_regs/tof3_reg_drv.h>

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

#define EFUSE_WORDS_TOF3 16

/* clang-format off */
static int device_id[2]     = {  0,  15};
static int wafer_rpr_en[2]  = { 16,  16};
static int rev_id[2]        = { 17,  24};
static int pkg_rev_id[2]    = { 25,  26};
static int version[2]       = { 27,  28};
static int freq_dis[2]      = { 29,  29};
static int ft_rpr_en[2]     = { 30,  30};
static int part_num[2]      = { 31,  35};
static int pcie_dis[2]      = { 36,  37};
static int pcie_def_speed[2]= { 38,  39};
static int pcie_max_speed[2]= { 40,  41};
static int pcie_rom2ram[2]  = { 42,  42};
static int cpu_speed_dis[2] = { 43,  44};
static int speed_dis[2]     = { 45, 112};
static int port_dis[2]      = {113, 147};
static int pipe_dis[2]      = {148, 155};
static int die_config[2]    = {156, 156};
static int pipe0_mau_dis[2] = {157, 177};
static int pipe1_mau_dis[2] = {178, 198};
static int pipe2_mau_dis[2] = {199, 219};
static int pipe3_mau_dis[2] = {220, 240};
static int tm_mem_dis[2]    = {241, 272};
static int bsync_dis[2]     = {273, 273};
static int pgen_dis[2]      = {274, 274};
static int resub_dis[2]     = {275, 275};
static int rev_num[2]       = {276, 283};
static int pkg_id[2]        = {284, 285};
static int silent_spin[2]   = {286, 287};
static int eth_fuse_dis[2]  = {288, 321};
static int serdes_dis_e[2]  = {322, 353};
static int serdes_dis_o[2]  = {354, 385};
static int rddr[2]          = {386, 393};
static int constant0[2]     = {394, 394};
static int constant1[2]     = {395, 395};
static int i2c[2]           = {396, 396};
static int spi[2]           = {397, 397};
static int rsvd[2]          = {398, 415};
static int vid[2]           = {416, 447};
static int chip_id[2]       = {448, 511};
/* clang-format on */

#if !defined DEVICE_IS_EMULATOR || DEVICE_IS_EMULATOR != 2
static void lld_delay(int microsec) {
#ifndef __KERNEL__
  bf_sys_usleep(microsec);
#else
  usleep_range(microsec, microsec + 10);
#endif
}
#endif

#if !defined DEVICE_IS_EMULATOR || DEVICE_IS_EMULATOR != 2
/* Helper functions to program soft efuse. */
static void wr_tcu_ctrl(bf_dev_id_t dev_id,
                        bf_subdev_id_t subdev_id,
                        int which,
                        uint32_t val) {
  uint32_t addr =
      which
          ? tof3_reg_device_select_misc_all_regs_misc_regs_tcu_control1_address
          : tof3_reg_device_select_misc_all_regs_misc_regs_tcu_control0_address;
  lld_subdev_write_register(dev_id, subdev_id, addr, val);
  lld_delay(100);
#ifdef DEVICE_IS_EMULATOR
  lld_delay(10 * 1000);
#endif
}
static void poll_wrack(bf_dev_id_t dev_id, bf_subdev_id_t subdev_id) {
  uint32_t i, x = 0;
  for (i = 0; i < 1000; ++i) {
    lld_subdev_read_register(
        dev_id,
        subdev_id,
        tof3_reg_device_select_misc_all_regs_misc_regs_tcu_wrack_address,
        &x);
    if (x == 1) return;
    lld_delay(10);
#ifdef DEVICE_IS_EMULATOR
    lld_delay(10 * 1000);
#endif
  }
  lld_log("TCU WR ACK did not go to zero (0x%x)", x);
}
static void verify_tcu_status(bf_dev_id_t dev_id,
                              bf_subdev_id_t subdev_id,
                              uint32_t expected) {
  uint32_t x = 0;
  lld_subdev_read_register(
      dev_id,
      subdev_id,
      tof3_reg_device_select_misc_all_regs_misc_regs_tcu_status_address,
      &x);
  if (x == expected) return;
  lld_log("TCU STATUS did not go to expected value (actual 0x%x expected 0x%x)",
          x,
          expected);
}
static void verify_tcu_status_dc(bf_dev_id_t dev_id, bf_subdev_id_t subdev_id) {
  uint32_t x = 0;
  lld_subdev_read_register(
      dev_id,
      subdev_id,
      tof3_reg_device_select_misc_all_regs_misc_regs_tcu_status_address,
      &x);
}
static void csr2jtag_soft_efuse(bf_dev_id_t dev_id,
                                bf_subdev_id_t subdev_id,
                                uint32_t *efuse_bits) {
  int i;
  /* Flush hold_din register first. */
  for (i = 0; i < 16; ++i) wr_tcu_ctrl(dev_id, subdev_id, 0, 0x00000000);
  wr_tcu_ctrl(dev_id, subdev_id, 1, 0x80001FF1);
  poll_wrack(dev_id, subdev_id);
  verify_tcu_status_dc(dev_id, subdev_id);

  /* Program new soft efuse values. */
  wr_tcu_ctrl(dev_id, subdev_id, 0, 0x0000001F);
  wr_tcu_ctrl(dev_id, subdev_id, 1, 0x80000071);
  poll_wrack(dev_id, subdev_id);
  verify_tcu_status(dev_id, subdev_id, 0x00000001);

  for (i = 0; i < 16; ++i) wr_tcu_ctrl(dev_id, subdev_id, 0, efuse_bits[i]);
  wr_tcu_ctrl(dev_id, subdev_id, 1, 0x80001FF4);
  for (i = 0; i < 16; ++i) {
    poll_wrack(dev_id, subdev_id);
    verify_tcu_status(dev_id, subdev_id, 0x00000000);
  }

  wr_tcu_ctrl(dev_id, subdev_id, 0, 0x0000001F);
  wr_tcu_ctrl(dev_id, subdev_id, 1, 0x80000071);
  poll_wrack(dev_id, subdev_id);
  verify_tcu_status(dev_id, subdev_id, 0x00000001);

  for (i = 0; i < 16; ++i) wr_tcu_ctrl(dev_id, subdev_id, 0, 0xE0000007);
  wr_tcu_ctrl(dev_id, subdev_id, 1, 0x80001FF4);
  for (i = 0; i < 16; ++i) {
    poll_wrack(dev_id, subdev_id);
    verify_tcu_status(dev_id, subdev_id, 0x00000000);
  }
}

#ifndef __KERNEL__
/* Helper functions to create the 512 bits of efuse data to program. */
static void set_bits(uint32_t *efuse, int *se, uint64_t val) {
  int start = se[0];
  int end = se[1];
  for (; start <= end; ++start) {
    efuse[start / 32] |= (val & 1) << (start % 32);
    val = val >> 1;
  }
}

static void efuse_json_to_bits(cJSON *json, uint32_t *efuse) {
  int x = 0;
  cJSON *i = NULL, *j = NULL;
  bf_cjson_try_get_int(json, "device_id", &x);
  set_bits(efuse, device_id, x);
  x = 0;
  bf_cjson_try_get_int(json, "versioning", &x);
  set_bits(efuse, version, x);
  x = 0;
  bf_cjson_try_get_int(json, "freq_dis", &x);
  set_bits(efuse, freq_dis, x);
  x = 0;
  bf_cjson_try_get_int(json, "pcie_dis", &x);
  set_bits(efuse, pcie_dis, x);
  x = 0;
  bf_cjson_try_get_int(json, "cpu_port_speed", &x);
  set_bits(efuse, cpu_speed_dis, x);

  /* Eth port speed reduction, 2 bits per MAC, 32 MACs. */
  uint64_t eth_port_speed = 0;
  bf_cjson_get_object(json, "eth_port_speed", &i);
  CTX_JSON_FOR_EACH(j, i) {
    int mac = 0, val = 0;
    bf_cjson_try_get_int(j, "mac", &mac);
    bf_cjson_try_get_int(j, "val", &val);
    eth_port_speed |= (uint64_t)val << (mac * 2);
  }
  set_bits(efuse, speed_dis, eth_port_speed);

  /* Port Disable */
  uint64_t port_disable = 0;
  i = NULL, j = NULL;
  bf_cjson_get_object(json, "port_disable", &i);
  CTX_JSON_FOR_EACH(j, i) { port_disable |= 1ULL << j->valueint; }
  set_bits(efuse, port_dis, port_disable);

  /* Pipe Disable */
  i = NULL, j = NULL;
  x = 0;
  bf_cjson_get_object(json, "pipe_disable", &i);
  CTX_JSON_FOR_EACH(j, i) { x |= 1 << j->valueint; }
  set_bits(efuse, pipe_dis, x);

  /* Pipe Stage Disable */
  for (int p = 0; p < 4; ++p) {
    char *keys[] = {"p0_stage_disable",
                    "p1_stage_disable",
                    "p2_stage_disable",
                    "p3_stage_disable"};
    int *setters[] = {
        pipe0_mau_dis, pipe1_mau_dis, pipe2_mau_dis, pipe3_mau_dis};
    i = NULL, j = NULL;
    x = 0;
    bf_cjson_get_object(json, keys[p], &i);
    CTX_JSON_FOR_EACH(j, i) { x |= 1 << j->valueint; }
    set_bits(efuse, setters[p], x);
  }

  x = 0;
  bf_cjson_try_get_int(json, "tm_mem_dis", &x);
  set_bits(efuse, tm_mem_dis, x);
  x = 0;
  bf_cjson_try_get_int(json, "bsync_dis", &x);
  set_bits(efuse, bsync_dis, x);
  x = 0;
  bf_cjson_try_get_int(json, "pgen_dis", &x);
  set_bits(efuse, pgen_dis, x);
  x = 0;
  bf_cjson_try_get_int(json, "resub_dis", &x);
  set_bits(efuse, resub_dis, x);
  x = 0;
  bf_cjson_try_get_int(json, "v_scale", &x);
  set_bits(efuse, vid, x);

  uint64_t chip_id_val = 0;
  bf_cjson_try_get_hex(json, "chip_id", (uint8_t *)&chip_id_val, 8);
  set_bits(efuse, chip_id, bswap_64(chip_id_val));

  x = 0;
  bf_cjson_try_get_int(json, "silent_spin", &x);
  set_bits(efuse, silent_spin, x);
  x = 0;
  bf_cjson_try_get_int(json, "pkg_id", &x);
  set_bits(efuse, pkg_id, x);
  x = 0;
  bf_cjson_try_get_int(json, "revision", &x);
  set_bits(efuse, rev_num, x);
  x = 0;
  bf_cjson_try_get_int(json, "part_num", &x);
  set_bits(efuse, part_num, x);
  x = 0;
  bf_cjson_try_get_int(json, "die_config", &x);
  set_bits(efuse, die_config, x);
  uint32_t serdes_even_val = 0;
  bf_cjson_try_get_hex(
      json, "serdes_disable_even", (uint8_t *)&serdes_even_val, 4);
  set_bits(efuse, serdes_dis_e, serdes_even_val);
  uint32_t serdes_odd_val = 0;
  bf_cjson_try_get_hex(
      json, "serdes_disable_odd", (uint8_t *)&serdes_odd_val, 4);
  set_bits(efuse, serdes_dis_o, serdes_odd_val);
  x = 0;
  bf_cjson_try_get_int(json, "constant0", &x);
  set_bits(efuse, constant0, 0);
  x = 0;
  bf_cjson_try_get_int(json, "constant1", &x);
  set_bits(efuse, constant1, 1);
}
#endif  // __KERNEL__

static int prog_soft_efuse(bf_dev_id_t dev_id, bf_subdev_id_t subdev_id) {
  uint32_t efuse_bits[EFUSE_WORDS_TOF3] = {0};
  uint32_t programmed_efuse_bits[EFUSE_WORDS_TOF3] = {0};
  int efuse_from_file = 0;
  int failure, i;
#ifndef __KERNEL__
  FILE *f;
  char file_name[100];
  memset(file_name, 0, sizeof(file_name));
  /* Use different efuse files for each die as some bits are diferent */
  if (subdev_id == 0) {
    snprintf(file_name, sizeof(file_name), "%s", "/efuse0");
  } else {
    snprintf(file_name, sizeof(file_name), "%s", "/efuse1");
  }
  if ((f = fopen(file_name, "r")) != NULL) {
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
      }
    }
    bf_sys_free(b);
    fclose(f);
  }
#endif  // __KERNEL__
  /* If there was no efuse requested in a file do not attempt to soft efuse the
   * part. */
  if (!efuse_from_file) return 0;

  csr2jtag_soft_efuse(dev_id, subdev_id, efuse_bits);

  /* Verify the programming by reading the efuse registers and checking they're
   * equal to the data programmed. */
  failure = 0;
  for (i = 0; i < EFUSE_WORDS_TOF3; i++) {
    lld_subdev_read_register(
        dev_id,
        subdev_id,
        tof3_reg_device_select_misc_all_regs_misc_regs_func_fuse_address +
            (4 * i),
        &programmed_efuse_bits[i]);
    if (programmed_efuse_bits[i] != efuse_bits[i]) failure = 1;
  }
  if (failure) {
    lld_log("Soft EFuse FAILED");
    lld_log("   Attempted  Read-Back");
    for (i = 0; i < EFUSE_WORDS_TOF3; i++) {
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

int lld_efuse_tof3_load(bf_dev_id_t dev_id,
                        bf_subdev_id_t subdev_id,
                        bf_dev_init_mode_t warm_init_mode) {
  lld_dev_t *dev_p =
      lld_map_subdev_id_to_dev_p_allow_unassigned(dev_id, subdev_id);
  lld_efuse_data_t *e = NULL;
  int i;
  uint32_t efuse_bits[EFUSE_WORDS_TOF3] = {0};
#if !defined DEVICE_IS_EMULATOR || DEVICE_IS_EMULATOR != 2
  bool is_sw_model = false;
#endif
  if (dev_p == NULL) return -1;

#if !defined DEVICE_IS_EMULATOR || DEVICE_IS_EMULATOR != 2
  bf_drv_device_type_get(dev_id, &is_sw_model);
  if (!is_sw_model && warm_init_mode == BF_DEV_INIT_COLD) {
    prog_soft_efuse(dev_id, subdev_id);
  }
#else
  (void)warm_init_mode;
#endif

  for (i = 0; i < EFUSE_WORDS_TOF3; i++) {
    int rc = lld_subdev_read_register(
        dev_id,
        subdev_id,
        tof3_reg_device_select_misc_all_regs_misc_regs_func_fuse_address +
            (4 * i),
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
  e->frequency_reduction = 0; /* removed from efuse */
  e->frequency_check_disable = get_bits(efuse_bits, freq_dis);
  e->versioning = get_bits(efuse_bits, version);
  /* A zero part-num will default to BFN_PART_NBR_BFNT3_56G */
  e->chip_part_number = get_bits(efuse_bits, part_num);
  e->part_revision_number = get_bits(efuse_bits, rev_id);
  e->package_id = get_bits(efuse_bits, pkg_rev_id);
  e->silent_spin = get_bits(efuse_bits, silent_spin);
  e->chip_id = get_bits(efuse_bits, chip_id);
  e->voltage_scaling = get_bits(efuse_bits, vid);
  e->tof3_pipe_mau_stage_disable[0] = get_bits(efuse_bits, pipe0_mau_dis);
  e->tof3_pipe_mau_stage_disable[1] = get_bits(efuse_bits, pipe1_mau_dis);
  e->tof3_pipe_mau_stage_disable[2] = get_bits(efuse_bits, pipe2_mau_dis);
  e->tof3_pipe_mau_stage_disable[3] = get_bits(efuse_bits, pipe3_mau_dis);
  e->tof3_eth_port_speed_reduction = get_bits(efuse_bits, speed_dis);
  e->tof3_die_config = get_bits(efuse_bits, die_config);
  e->tof3_constant0 = get_bits(efuse_bits, constant0);
  e->tof3_constant1 = get_bits(efuse_bits, constant1);
  e->tof3_fuse_serdes_even = get_bits(efuse_bits, serdes_dis_e);
  e->tof3_fuse_serdes_odd = get_bits(efuse_bits, serdes_dis_o);

/*
   Override part-num to 12.8 for emulator. Emulator does not have efuse.
*/
#if defined(DEVICE_IS_EMULATOR)
#if defined(EMU_2DIE_DEV_ENABLE)
  // Enable 25.6 SKU for Multi-Die on Emulator
  e->chip_part_number = BFN_PART_NBR_BFNT3_56G;
  e->tof3_die_config = 1;
#else
  // 12.8 SKU for Single-Die on Emulator
  e->chip_part_number = BFN_PART_NBR_BFNT3_56G;
  e->tof3_die_config = 0;
#endif
#endif

  (void)wafer_rpr_en;
  (void)rev_num;
  (void)pkg_id;
  (void)ft_rpr_en;
  (void)pcie_def_speed;
  (void)pcie_max_speed;
  (void)pcie_rom2ram;
  (void)eth_fuse_dis;
  (void)rddr;
  (void)rsvd;
  (void)spi;
  (void)i2c;

  return 0;
}

#ifndef __KERNEL__
static char tof3_a0_chipid_field_char(int c_id) {
  char c;
  if (c_id >= 0 && c_id < 10) {  // 0..9
    c = '0' + c_id;
  } else if (c_id >= 10 && c_id < 36) {  // 10..35
    c = 'A' + c_id - 10;
  } else {
    c = ' ';
  }
  return c;
}
#endif

void lld_efuse_tof3_wafer_str_get(bf_dev_id_t dev_id,
                                  bf_subdev_id_t subdev_id,
                                  int s_len,
                                  char *s) {
#ifndef __KERNEL__
  uint64_t id = lld_subdev_efuse_get_chip_id(dev_id, subdev_id);
  // TF3 chip id has different encoding scheme (which might change in future)
  // lower 6 fields, fab_id, lot_id and 4 fields of lot are 6 bits each and
  // spare bits on MSB side. Total used bits are 42 (same as that on TF2).
  // all 6 bit fields use the encoding that is different than that on TF2.
  uint32_t fab_id = id & 0x3F;
  id >>= 6;
  uint32_t lot_id = id & 0x3F;
  id >>= 6;
  uint32_t lot_n = id & 0x0FFFFFFF;
  id >>= 30;
  uint32_t wafer = id & 0x1F;
  id >>= 5;
  uint32_t x_sign = id & 1;
  id >>= 1;
  uint32_t x = id & 0x7F;
  id >>= 7;
  uint32_t y_sign = id & 1;
  id >>= 1;
  uint32_t y = id & 0x7F;
  uint32_t lot_c[4] = {(lot_n >> 18) & 0x3F,
                       (lot_n >> 12) & 0x3F,
                       (lot_n >> 6) & 0x3F,
                       lot_n & 0x3F};
  snprintf(s,
           s_len,
           "Lot %c%c%c%c%c%c Wafer %d X%c%d Y%c%d",
           tof3_a0_chipid_field_char(fab_id),
           tof3_a0_chipid_field_char(lot_id),
           tof3_a0_chipid_field_char(lot_c[3]),
           tof3_a0_chipid_field_char(lot_c[2]),
           tof3_a0_chipid_field_char(lot_c[1]),
           tof3_a0_chipid_field_char(lot_c[0]),
           wafer,
           x_sign ? '-' : '+',
           x,
           y_sign ? '-' : '+',
           y);
#else
  (void)dev_id;
  (void)subdev_id;
  (void)s;
#endif
}
