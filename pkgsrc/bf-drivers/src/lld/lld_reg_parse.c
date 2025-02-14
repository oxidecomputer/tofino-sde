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
#include <stdlib.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <regex.h>
#include <ctype.h>
#include <limits.h>
#include <editline/readline.h>

#include <target-sys/bf_sal/bf_sys_intf.h>
#include <dvm/bf_drv_intf.h>

#include "lld_memory_mapping.h"
#include <lld/lld_reg_if.h>
#include <tofino_regs/tofino.h>
#include <tof2_regs/tof2_reg_drv.h>
#include <tof3_regs/tof3_reg_drv.h>
#include "lld.h"
#include "lld_diag.h"
#include "lld_diag_ext.h"
#include "lld_dev.h"
#include "lld_map.h"
#include <lld/lld_sku.h>

#ifdef __sun
#define __P(x) x
#endif

typedef enum bf_lld_decoder_access_type {
  REG_OP = 0,
  MEM_OP = 1,
} bf_lld_decoder_access_type;

#define WM_IND_LOW 0
#define WM_IND_HIGH 1
#define WM_IND_HIGH_LOW 2
#define WM_IND_INVALID 3

void initialize_readline(void);
static char *next_word(char *cmd_line, char **remnant);
int execute_line(char *line);
char *get_full_reg_path_name(bf_dev_id_t dev_id, uint32_t offset);
static char *get_full_mem_path_name(bf_dev_id_t dev_id, uint64_t offset);

static int tof_dump_by_offset(bf_dev_id_t dev_id, uint32_t target_offset);
static int tof2_dump_by_offset(bf_dev_id_t dev_id, uint32_t target_offset);
static int tof3_dump_by_offset(bf_dev_id_t dev_id,
                               bf_subdev_id_t subdev_id,
                               uint32_t target_offset);
extern lld_diag_cfg_t lld_diag_cfg;

typedef struct reg_decoder_fld_t {
  char *fld_name;
  int msb;
  int lsb;
  int is_array;
  int fld_handler_entry;
  char *fld_desc;
} reg_decoder_fld_t;

typedef struct reg_decoder_t {
  int n_flds;
  reg_decoder_fld_t *fld_list;
  int reg_width;
  int is_int_reg;
} reg_decoder_t;

typedef struct cmd_arg_item_t {
  char *key_wd;
  struct cmd_arg_t *next_arg;
  uint64_t offset;
  reg_decoder_t *decoder;
  uint32_t no_rows; /* Only for Indirect memories */
} cmd_arg_item_t;

typedef struct cmd_arg_t {
  int n;
  struct cmd_arg_item_t *arg;
} cmd_arg_t;

static bool is_number(char *number_str) {
  while (*number_str != '\0') {
    if (isdigit(*number_str) == 0) {
      return false;
    }
    number_str++;
  }

  return true;
}

static bool is_hex_number(char *number_str) {
  while (*number_str != '\0') {
    if (isxdigit(*number_str) == 0) {
      return false;
    }
    number_str++;
  }

  return true;
}

static uint64_t determine_offset(char *cmd_line,
                                 cmd_arg_item_t **reg_desc,
                                 bf_dev_id_t dev_id,
                                 bf_lld_decoder_access_type access_type);
void traverse_args(bf_dev_family_t dev_family,
                   cmd_arg_item_t *item,
                   uint64_t offset,
                   bf_lld_decoder_access_type access_type);
void dump_mem(cmd_arg_item_t *item, uint64_t mem_addr);
int dump_and_decode_mem(uint64_t mem_addr,
                        cmd_arg_item_t *mem_desc,
                        int num_entries);
int dump_and_decode_mem_detail(uint64_t mem_addr, cmd_arg_item_t *mem_desc);

extern bf_status_t lld_ind_reg_lock_init();

bf_subdev_id_t cmd_subdev_id = 0;

int user_wants_to_abort(void) {
  fd_set readfds = {{0}};
  int rv, n;
  struct timeval tv = {0};

  // clear the set ahead of time
  FD_ZERO(&readfds);

  // add our descriptors to the set
  FD_SET(0, &readfds);
  n = 1;
  rv = select(n, &readfds, NULL, NULL, &tv);
  if (rv == 0)
    return 0;
  else if (FD_ISSET(0, &readfds))
    return 1;
  else
    return 0;
}

/* A structure which contains information on the commands this program
   can understand. */

typedef int rl_icpfunc_t(char *, bf_dev_id_t dev_id, bf_subdev_id_t subdev_id);
typedef struct {
  char *name;         /* User printable name of the function. */
  rl_icpfunc_t *func; /* Function to call to do the job. */
  char *doc;          /* Documentation for this function.  */
} COMMAND;

char parse_matches[1024] = {0};

int exit_access_mode = 0;

int com_quit(char *arg, bf_dev_id_t dev_id, bf_subdev_id_t subdev_id) {
  (void)arg;
  (void)dev_id;
  (void)subdev_id;
  exit_access_mode = 1;
  return 0;
}

void dump_reg(bf_dev_family_t dev_family,
              cmd_arg_item_t *reg_desc,
              uint32_t reg) {
  bf_dev_id_t asic = 0;
  bf_subdev_id_t subdev_id = cmd_subdev_id;

  int x = 0;
  uint32_t reg_rd_data, reg_rd_data2;
  reg_decoder_t *decoder = reg_desc->decoder;

  if (dev_family == BF_DEV_FAMILY_TOFINO) {
    if (reg >= offsetof(Tofino, device_select.tm_top.tm_prc_top) &&
        reg < offsetof(Tofino, device_select.tm_top.tm_pre_top)) {
      /* This is a bulk read to a TF1 PRC register.  These will be avoided as
       * they are not safe to read while traffic is flowing. */
      fprintf(rl_outstream,
              "%d [%08x] : Skipped  : %s\n",
              asic,
              reg,
              reg_desc->key_wd);
      return;
    }
  }

  if (decoder->reg_width == 32) {
    x = lld_subdev_read_register(asic, subdev_id, (uint64_t)reg, &reg_rd_data);
    if (x) {
      return;
    }

    fprintf(rl_outstream,
            "%d [%08x] : %08x : %s\n",
            asic,
            reg,
            reg_rd_data,
            reg_desc->key_wd);
  } else if (decoder->reg_width == 64) {
    uint64_t reg64;

    x = lld_subdev_read_register(asic, subdev_id, (uint64_t)reg, &reg_rd_data);
    if (x) {
      return;
    }
    x = lld_subdev_read_register(
        asic, subdev_id, (uint64_t)reg + 4, &reg_rd_data2);
    if (x) {
      return;
    }
    reg64 = (uint64_t)reg_rd_data | ((uint64_t)reg_rd_data2 << 32ull);

    fprintf(rl_outstream,
            "%d [%08x] : %016" PRIx64 " : %s\n",
            asic,
            reg,
            reg64,
            reg_desc->key_wd);
  } else {
    int i, ofs = 0;
    int n_wds = decoder->reg_width / 32;
    uint8_t *really_wide_reg =
        (uint8_t *)bf_sys_malloc(n_wds * sizeof(uint32_t));
    if (really_wide_reg == NULL) {
      fprintf(rl_outstream,
              "Unable to print the register values, Malloc failed\n");
      return;
    }

    for (i = 0; i < n_wds; i++, ofs += 4) {
      x = lld_subdev_read_register(
          asic, subdev_id, (uint64_t)reg + ofs, &reg_rd_data);
      if (x) {
        bf_sys_free(really_wide_reg);
        return;
      }
      really_wide_reg[ofs + 0] = (reg_rd_data >> 0) & 0xff;
      really_wide_reg[ofs + 1] = (reg_rd_data >> 8) & 0xff;
      really_wide_reg[ofs + 2] = (reg_rd_data >> 16) & 0xff;
      really_wide_reg[ofs + 3] = (reg_rd_data >> 24) & 0xff;
    }
    fprintf(rl_outstream, "%d [%08x] : %s\n", asic, reg, reg_desc->key_wd);
    for (i = 0; i < n_wds * 4; i++) {
      if ((i % 16) == 0) {
        fprintf(rl_outstream, "\n%3d <%3xh> : ", ofs, ofs);
      }
      fprintf(rl_outstream, "%02x ", really_wide_reg[i]);
    }
    fprintf(rl_outstream, "\n");
    bf_sys_free(really_wide_reg);
  }
}

char formatted[32 * 1024];
char *format_fld_desc(char *desc, int start_col) {
  int col = 0, i, len = strlen(desc);
  char *p = desc;
  start_col = 12;
  memset(formatted, 0, sizeof(formatted));

  for (i = 0; i < len; i++) {
    if ((col + start_col) > 80) {
      sprintf(&formatted[strlen(formatted)], "%s", "\n             # ");
      col = 0;
    }
    formatted[strlen(formatted)] = *p++;
    col++;
  }
  return (formatted);
}

int dump_and_decode_reg(bf_dev_id_t dev_id,
                        bf_subdev_id_t subdev_id,
                        uint32_t reg,
                        cmd_arg_item_t *reg_desc) {
  reg_decoder_t *decoder = NULL;
  reg_decoder_fld_t *fld_desc = NULL;
  bf_dev_id_t asic = dev_id;
  int x;
  uint32_t reg_rd_data, reg_rd_data2;

  decoder = reg_desc->decoder;
  {
    int i;
    // read_u  getp_fn;
    // write_u setp_fn;
    char *formatted_desc = NULL;

    if ((decoder->reg_width == 16) || (decoder->reg_width == 32)) {
      char *path = get_full_reg_path_name(asic, reg);
      x = lld_subdev_read_register(
          asic, subdev_id, (uint64_t)reg, &reg_rd_data);
      if (x) {
        return 1;
      }

      // printf("%d [%08x] : %08x : %s\n", asic, reg, reg_rd_data,
      // reg_desc->key_wd );
      fprintf(rl_outstream,
              "%d:%d [%08x] : %08x : %s\n",
              asic,
              subdev_id,
              reg,
              reg_rd_data,
              path ? path : "NONE");
      for (i = 0; i < decoder->n_flds; i++) {
        uint32_t fld_val;

        fld_desc = &decoder->fld_list[i];

        //                auto_gen_get_fields( fld_desc->fld_handler_entry,
        //                NULL, NULL, NULL,
        //                                     &getp_fn, &setp_fn );
        if (!fld_desc->fld_desc) {
          formatted_desc = NULL;
        } else if (!*fld_desc->fld_desc) {
          formatted_desc = NULL;
        } else {
          formatted_desc = format_fld_desc(fld_desc->fld_desc, 0);
          fprintf(rl_outstream, "             # %s\n", formatted_desc);
        }
        // fld_val = getp_fn.read_f_32( &reg_rd_data );
        fld_val = ((reg_rd_data << (31 - fld_desc->msb)) >>
                   (31 - fld_desc->msb + fld_desc->lsb));
        fprintf(rl_outstream,
                "  [%2d:%2d] %2x : %s\n",
                fld_desc->msb,
                fld_desc->lsb,
                fld_val,
                fld_desc->fld_name);
      }
    } else if (decoder->reg_width == 64) {
      uint64_t reg64;
      char *path = get_full_reg_path_name(asic, reg);

      x = lld_subdev_read_register(
          asic, subdev_id, (uint64_t)reg, &reg_rd_data);
      if (x) {
        return 1;
      }
      x = lld_subdev_read_register(
          asic, subdev_id, (uint64_t)reg + 4, &reg_rd_data2);
      if (x) {
        return 1;
      }
      reg64 = (uint64_t)reg_rd_data | ((uint64_t)reg_rd_data2 << 32ull);

      // printf("%d [%08x] : %016"PRIx64" : %s\n", asic, reg, reg64,
      // reg_desc->key_wd );
      fprintf(rl_outstream,
              "%d:%d [%08x] : %016" PRIx64 " : %s\n",
              asic,
              subdev_id,
              reg,
              reg64,
              path ? path : "NONE");
      for (i = 0; i < decoder->n_flds; i++) {
        fld_desc = &decoder->fld_list[i];
        uint64_t fld_val;

        // auto_gen_get_fields( fld_desc->fld_handler_entry, NULL, NULL, NULL,
        //                     &getp_fn, &setp_fn );
        fld_val = ((reg64 << (63ull - fld_desc->msb)) >>
                   (63ull - fld_desc->msb + fld_desc->lsb));

        fprintf(rl_outstream, "# %s\n", fld_desc->fld_desc);
        fprintf(rl_outstream,
                "  [%2d:%2d] %" PRIx64 " : %s \n",
                fld_desc->msb,
                fld_desc->lsb,
                fld_val,
                fld_desc->fld_name);
      }
    } else {
      int n_wds = decoder->reg_width / 32;
      uint32_t really_wide_reg[64] = {0};
      int f_n, f_val, f_bit, f_width;
      int w_n, w_bit, w_width, bit;
      char *path = get_full_reg_path_name(asic, reg);

      fprintf(rl_outstream,
              "%d:%d [%08x] : %s\n",
              asic,
              subdev_id,
              reg,
              path ? path : "NONE");
      fld_desc = &decoder->fld_list[0];
      fprintf(rl_outstream, "# %s\n", fld_desc->fld_desc);

      for (w_n = 0; w_n < n_wds; w_n++) {
        lld_subdev_read_register(
            asic, subdev_id, (uint64_t)reg + (4 * w_n), &really_wide_reg[w_n]);
      }
      f_n = 0;
      f_bit = 0;
      f_val = 0;
      f_width = fld_desc->msb + 1;
      w_n = 0;  // reset
      w_bit = 0;
      w_width = 32;
      for (i = 0; i < decoder->reg_width; i++) {
        bit = (really_wide_reg[w_n] >> w_bit) & 1;
        f_val |= (bit << f_bit);
        // wrap field if done
        f_bit++;
        if (f_bit >= f_width) {  // done
          fprintf(rl_outstream, "  [%d] : %04x\n", f_n, f_val);
          f_n++;
          f_bit = 0;
          f_val = 0;
        }
        // wrap window if done
        w_bit++;
        if (w_bit >= w_width) {
          w_n++;
          w_bit = 0;
        }
      }
    }
  }
  return 0;
}

void dump_mem(cmd_arg_item_t *mem_desc, uint64_t mem_addr) {
  bf_dev_id_t asic = 0;
  int x = 0;
  uint64_t data_hi = 0;
  uint64_t data_lo = 0;
  bf_subdev_id_t subdev_id = cmd_subdev_id;

  x = lld_subdev_ind_read(asic, subdev_id, mem_addr, &data_hi, &data_lo);
  if (x) {
    return;
  }
  fprintf(
      rl_outstream, "%d:%d [0x%lx] ", asic, subdev_id, (unsigned long)mem_addr);
  fprintf(rl_outstream,
          "0x"
          "%016" PRIx64
          " 0x"
          "%016" PRIx64
          " - %s_0_%lu"
          "\n",
          data_hi,
          data_lo,
          mem_desc->key_wd,
          (((unsigned long)mem_desc->no_rows) >> 4) - 1);
  return;
}

int dump_and_decode_mem(uint64_t mem_addr,
                        cmd_arg_item_t *mem_desc,
                        int num_entries) {
  bf_dev_id_t asic = 0;
  char *path = get_full_mem_path_name(asic, mem_addr);
  uint64_t data_hi = 0;
  uint64_t data_lo = 0;
  bf_subdev_id_t subdev_id = cmd_subdev_id;

  (void)mem_desc;

  // TODO: Need to update once it is available
  fprintf(rl_outstream, "Device - %d : Sub_Device - %d\n", asic, subdev_id);
  fprintf(rl_outstream,
          "|---------------------------------------------------------|\n");
  fprintf(rl_outstream,
          "      Address         High (127b-64b)       Low (63b-0b) \n");
  fprintf(rl_outstream,
          "|---------------------------------------------------------|\n");

  for (int iter = 0; iter < num_entries; iter++) {
    lld_subdev_ind_read(asic, subdev_id, mem_addr, &data_hi, &data_lo);
    fprintf(rl_outstream,
            "0x"
            "%016" PRIX64
            "  0x"
            "%016" PRIX64
            "  0x"
            "%016" PRIX64 "\n",
            mem_addr,
            data_hi,
            data_lo);
    data_hi = 0;
    data_lo = 0;
    mem_addr += 1;
  }

  fprintf(rl_outstream,
          "|---------------------------------------------------------|\n");
  if (path) {
    fprintf(rl_outstream, "%s\n", path);
  } else {
    fprintf(rl_outstream, "NONE");
  }

  return 0;
}

int dump_and_decode_mem_detail(uint64_t mem_addr, cmd_arg_item_t *mem_desc) {
  reg_decoder_t *decoder = NULL;
  reg_decoder_fld_t *fld_desc = NULL;
  bf_dev_id_t asic = 0;
  uint64_t data_hi = 0;
  uint64_t data_lo = 0;
  bf_subdev_id_t subdev_id = cmd_subdev_id;

  decoder = mem_desc->decoder;

  if (decoder) {
    int i;
    // char *formatted_desc = NULL;

    if (decoder->reg_width == 128) {
      lld_subdev_ind_read(asic, subdev_id, mem_addr, &data_hi, &data_lo);

      fprintf(rl_outstream,
              "0x"
              "%016" PRIX64
              "  0x"
              "%016" PRIX64
              "  0x"
              "%016" PRIX64 "\n",
              mem_addr,
              data_hi,
              data_lo);

      for (i = 0; i < decoder->n_flds; i++) {
        fld_desc = &decoder->fld_list[i];
        uint64_t temp_data = data_lo;
        uint64_t temp_data_lo = 0;
        uint64_t temp_data_hi = 0;
        uint64_t fld_val;
        int msb = fld_desc->msb;
        int lsb = fld_desc->lsb;
        bool print_hi_lo = false;

        // In case length is greater than 64 bits
        if ((msb - lsb) >= 64) {
          int msb_diff = msb - 64;

          temp_data_lo = data_lo >> lsb;
          temp_data_hi = data_hi << (63ull - msb_diff);

          if (lsb != 0) {
            temp_data_lo = (temp_data_hi << (msb - lsb - 63ull)) | temp_data_lo;
            temp_data_hi = temp_data_hi >> (63ull - msb_diff);
            temp_data_hi = temp_data_hi >> lsb;
          } else {
            temp_data_hi = temp_data_hi >> (63ull - msb_diff);
          }

          print_hi_lo = true;
        } else if ((lsb <= 63) && (msb > 63)) {
          // In case the field is between lower and higher 64 bits
          int diff_bit_pos = msb - lsb;

          msb = msb - 64;
          temp_data = data_hi << (63ull - msb);
          temp_data = temp_data >> (63ull - diff_bit_pos);
          temp_data |= (data_lo >> lsb);
          fld_val = temp_data;
        } else {           // Field is within single 64 bits
          if (lsb > 63) {  // Field is in higher 64 bits
            temp_data = data_hi;
            lsb = lsb - 63ull;
            msb = msb - 63ull;
          }

          fld_val = ((temp_data << (63ull - msb)) >> (63ull - msb + lsb));
        }

        fprintf(rl_outstream, "# %s\n", fld_desc->fld_desc);
        if (print_hi_lo == false) {
          fprintf(rl_outstream,
                  "  [%2d:%2d] 0x%" PRIX64 " : %s \n",
                  fld_desc->msb,
                  fld_desc->lsb,
                  fld_val,
                  fld_desc->fld_name);
        } else {
          fprintf(rl_outstream,
                  "  [%2d:%2d] 0x%" PRIX64 " : 0x%" PRIX64 " : %s \n",
                  fld_desc->msb,
                  fld_desc->lsb,
                  temp_data_hi,
                  temp_data_lo,
                  fld_desc->fld_name);
          print_hi_lo = false;
        }
      }
    }
  }

  return 0;
}

// fwd ref
static void regx_cmd_handler(bf_dev_id_t dev_id,
                             bf_subdev_id_t subdev_id,
                             char *pattern);

int com_access_rx(char *arg, bf_dev_id_t dev_id, bf_subdev_id_t subdev_id) {
  regx_cmd_handler(dev_id, subdev_id, arg);
  return 0;
}

int com_access_rr(char *arg, bf_dev_id_t dev_id, bf_subdev_id_t subdev_id) {
  cmd_arg_item_t *reg_desc = NULL;
  uint64_t reg;
  bf_dev_family_t dev_family = lld_dev_family_get(dev_id);

  cmd_subdev_id = subdev_id;
  reg = determine_offset(arg, &reg_desc, dev_id, REG_OP);

  if (!reg_desc) {
    fprintf(rl_outstream, "parsing terminated after: '%s'\n", parse_matches);
    return 0;
  }
  if (reg_desc->next_arg != NULL) {  // incomplete path
    traverse_args(dev_family, reg_desc, reg, REG_OP);
    return 0;
  }
  dump_and_decode_reg(dev_id, subdev_id, reg, reg_desc);
  return 0;
}

int com_access_wr(char *arg, bf_dev_id_t dev_id, bf_subdev_id_t subdev_id) {
  cmd_arg_item_t *reg_desc = NULL;
  uint64_t reg;
  uint32_t write_val;
  int str_len = strlen(arg);
  char *pos;
  bf_dev_family_t dev_family = lld_dev_family_get(dev_id);

  cmd_subdev_id = subdev_id;

  /* strip off last "word", assuming its a hex value to write */
  if (str_len == 0) {
    fprintf(rl_outstream, "Write what?\n");
    return 0;
  }
  pos = arg + str_len;
  while ((pos > arg) && (*pos != ' ')) pos--;
  if (pos == arg) {
    fprintf(rl_outstream, "No write-value given\n");
    return 0;
  }
  *pos = 0;  // terminate original arg string prior to value

  write_val = strtoul(pos + 1, NULL, 16);

  reg = determine_offset(arg, &reg_desc, dev_id, REG_OP);

  if (!reg_desc) {
    fprintf(rl_outstream, "parsing terminated after: '%s'\n", parse_matches);
    return 0;
  }
  if (reg_desc->next_arg != NULL) {  // incomplete path
    traverse_args(dev_family, reg_desc, reg, REG_OP);
    return 0;
  }
  if (reg != 0xffffffff) {
    lld_subdev_write_register(dev_id, subdev_id, reg, write_val);
    dump_and_decode_reg(dev_id, subdev_id, reg, reg_desc);
  }
  return 0;
}

int com_access_rm(char *arg, bf_dev_id_t dev_id, bf_subdev_id_t subdev_id) {
  cmd_arg_item_t *mem_desc = NULL;
  uint64_t mem_addr = 0;
  uint64_t num_entries = 0;
  uint64_t entry_idx = 0;
  int str_len = strlen(arg);
  char *pos;
  char *number_ptr = NULL;
  bf_dev_family_t dev_family = lld_dev_family_get(dev_id);

  cmd_subdev_id = subdev_id;
  /* strip off last "word", assuming its an index */
  if (str_len == 0) {
    fprintf(rl_outstream, "Write what?\n");
    return -1;
  }

  pos = arg + str_len;

  // Get Number of Entries
  while ((pos > arg) && (*pos != ' ')) pos--;
  *pos = 0;  // terminate original arg string prior to value
  number_ptr = pos + 1;
  num_entries =
      strtoul(number_ptr, NULL, 0);  // number of etries to read from memory

  if (is_number(number_ptr) == true) {
    // Get entry Index
    while ((pos > arg) && (*pos != ' ')) pos--;
    number_ptr = pos + 1;
    entry_idx =
        strtoul(number_ptr,
                NULL,
                0);  // entry will be set to 0 if there is no arg for index

    if (is_number(number_ptr) != true) {
      entry_idx = num_entries;
      num_entries = 1;
    } else {
      *pos = 0;  // terminate original arg string prior to value
    }
  }

  mem_addr = determine_offset(arg, &mem_desc, dev_id, MEM_OP);
  if (!mem_desc) {
    fprintf(rl_outstream, "parsing terminated after: '%s'\n", parse_matches);
    return -1;
  }
  if (mem_addr == ULONG_MAX) {
    fprintf(rl_outstream, "Invalid memory address\n");
    return 0;
  }

  if (num_entries > 10000) {
    fprintf(rl_outstream,
            "Num entries are restricted for max of 10000 entries\n");
    return -1;
  }

  if (mem_desc->next_arg != NULL) {  // incomplete path
    traverse_args(dev_family, mem_desc, mem_addr, MEM_OP);
    return 0;
  }

  mem_addr += entry_idx;

  if (num_entries != 1) {
    dump_and_decode_mem(mem_addr, mem_desc, num_entries);
  } else {
    dump_and_decode_mem_detail(mem_addr, mem_desc);
  }

  return 0;
}

static int parse_high_low_values(char *arg,
                                 uint64_t *index,
                                 uint64_t *high,
                                 uint64_t *low) {
  char *pos = arg + strlen(arg);
  char *mem_data = NULL;
  uint64_t value = 0;
  bool found_high = false;
  bool found_low = false;

  // Parse first parameter
  while ((pos > arg) && (*pos != ' ')) pos--;
  if (pos == arg) {
    fprintf(rl_outstream, "No write-value given\n");
    return WM_IND_INVALID;
  }

  mem_data = pos + 1;

  if (is_hex_number(mem_data) != true) {
    fprintf(rl_outstream, "Error: Invalid argument\n");
    return WM_IND_INVALID;
  }

  if (strlen(mem_data) > 16) {
    fprintf(rl_outstream,
            "Error: Given value %s is greater than 16 byte\n",
            mem_data);
    return WM_IND_INVALID;
  }

  *pos = 0;  // terminate original arg string prior to value
  value = strtoul(mem_data, NULL, 16);

  // Parse next parameter
  while ((pos > arg) && (*pos != ' ')) pos--;
  if (pos == arg) {
    fprintf(rl_outstream, "No Index value given\n");
    return WM_IND_INVALID;
  }

  mem_data = pos + 1;

  // Verify parameter option
  if (!strcmp(mem_data, "-h")) {
    *high = value;
    found_high = true;
  } else if (!strcmp(mem_data, "-l")) {
    *low = value;
    found_low = true;
  } else {
    fprintf(rl_outstream, "Error: Invalid argument\n");
    return WM_IND_INVALID;
  }

  *pos = 0;  // terminate original arg string prior to value
  // Parse next parameter
  while ((pos > arg) && (*pos != ' ')) pos--;
  if (pos == arg) {
    fprintf(rl_outstream, "No write-value given\n");
    return WM_IND_INVALID;
  }

  mem_data = pos + 1;

  // Verify parameter option
  if (is_hex_number(mem_data) != true) {
    fprintf(rl_outstream, "Error: Invalid argument\n");
    return WM_IND_INVALID;
  }

  if (strlen(mem_data) > 16) {
    fprintf(rl_outstream,
            "Error: Given value %s is greater than 16 byte\n",
            mem_data);
    return WM_IND_INVALID;
  }

  value = strtoul(mem_data, NULL, 16);

  *pos = 0;  // terminate original arg string prior to value
  // Parse next parameter
  while ((pos > arg) && (*pos != ' ')) pos--;
  if (pos == arg) {
    fprintf(rl_outstream, "No write-value given\n");
    return WM_IND_INVALID;
  }

  mem_data = pos + 1;

  // Verify parameter option
  if (!strcmp(mem_data, "-h") && (found_high == false)) {
    *high = value;
    found_high = true;
  } else if (!strcmp(mem_data, "-l") && (found_low == false)) {
    *low = value;
    found_low = true;
  } else {
    goto parse_found;
  }

  *pos = 0;  // terminate original arg string prior to value
  // Parse next parameter
  // Get the Index to write the value
  while ((pos > arg) && (*pos != ' ')) pos--;
  if (pos == arg) {
    fprintf(rl_outstream, "No Index value given\n");
    return WM_IND_INVALID;
  }

  mem_data = pos + 1;
  *index = strtoul(mem_data, NULL, 0);

  if (is_number(mem_data) != true) {
    fprintf(rl_outstream, "Error: Invalid argument\n");
    return WM_IND_INVALID;
  }
  *pos = 0;  // terminate original arg string prior to value

parse_found:
  if ((found_high == true) && (found_low == true)) {
    return WM_IND_HIGH_LOW;
  } else if (found_low == true) {
    return WM_IND_LOW;
  } else if (found_high == true) {
    return WM_IND_HIGH;
  }

  fprintf(rl_outstream, "Error: Invalid argument\n");
  return WM_IND_INVALID;
}

int com_access_wm(char *arg, bf_dev_id_t dev_id, bf_subdev_id_t subdev_id) {
  cmd_arg_item_t *mem_desc = NULL;
  uint64_t mem_addr = 0, write_hi = 0, write_lo = 0;
  uint64_t read_hi = 0, read_lo = 0;
  int str_len = strlen(arg);
  int ret_val = 0;
  uint64_t entry_idx = 0;
  bf_dev_family_t dev_family = lld_dev_family_get(dev_id);

  cmd_subdev_id = subdev_id;

  if (str_len == 0) {
    fprintf(rl_outstream, "Write what?\n");
    return 0;
  }

  ret_val = parse_high_low_values(arg, &entry_idx, &write_hi, &write_lo);
  if (ret_val == WM_IND_INVALID) {
    return -1;
  }

  mem_addr = determine_offset(arg, &mem_desc, dev_id, MEM_OP);
  if (!mem_desc) {
    fprintf(rl_outstream, "parsing terminated after: '%s'\n", parse_matches);
    return 0;
  }
  if (mem_addr == ULONG_MAX) {
    fprintf(rl_outstream, "Invalid memory address\n");
    return 0;
  }

  mem_addr += entry_idx;

  if (mem_desc->next_arg != NULL) {  // incomplete path
    traverse_args(dev_family, mem_desc, mem_addr, MEM_OP);
    return 0;
  }

  // Read from the memory
  if (lld_subdev_ind_read(dev_id, subdev_id, mem_addr, &read_hi, &read_lo)) {
    return -1;
  }

  if (ret_val == WM_IND_HIGH) {
    write_lo = read_lo;
  } else if (ret_val == WM_IND_LOW) {
    write_hi = read_hi;
  }

  // Write into indirect address
  ret_val =
      lld_subdev_ind_write(dev_id, subdev_id, mem_addr, write_hi, write_lo);
  if (ret_val) {
    return ret_val;
  }

  // Read and dump the written content back
  dump_and_decode_mem(mem_addr, mem_desc, 1);

  return 0;
}

void find_string_in_regs(char *pattern, bf_dev_id_t dev_id);

int com_access_find(char *arg, bf_dev_id_t dev_id, bf_subdev_id_t subdev_id) {
  (void)subdev_id;
  find_string_in_regs(arg, dev_id);
  return 0;
}

COMMAND commands[] = {
    {"rr", com_access_rr, "Read register"},
    {"rx", com_access_rx, "Read register matching regular-exp"},
    {"wr", com_access_wr, "Write register"},
    {"rm", com_access_rm, "Read Memory"},
    {"wm", com_access_wm, "Write Memory"},
    {"f", com_access_find, "Find registers/blocks matching pattern"},
    {"quit", com_quit, "Exit access mode"},
    {NULL, NULL, NULL}};

char *pipes[] = {"0", "1", "2", "3"};

char *prsrs[] = {"0",
                 "1",
                 "2",
                 "3",
                 "4",
                 "5",
                 "6",
                 "7",
                 "8",
                 "9",
                 "10",
                 "3",
                 "0",
                 "1",
                 "2",
                 "3"};

/* Forward declarations. */
char *stripwhite();
COMMAND *find_command();

/* When non-zero, this means the user is done using this program. */
int done;

char *dupstr(char *s) {
  char *r;
  r = bf_sys_malloc(strlen(s) + 1);
  strcpy(r, s);
  return (r);
}

void reg_set_instream(FILE *fp) { rl_instream = fp; }

void reg_set_outstream(FILE *fp) { rl_outstream = fp; }

FILE *reg_get_outstream() { return rl_outstream; }

void reg_parse_main(void) {
  char *line, *s;

  initialize_readline(); /* Bind our completer. */

  /* Loop reading and executing lines until the user quits. */
  for (; done == 0;) {
    line = readline("Access: ");

    if (!line) break;

    /* Remove leading and trailing whitespace from the line.
       Then, if there is anything left, add it to the history list
       and execute it. */
    s = stripwhite(line);

    if (*s) {
      add_history(s);
      execute_line(s);
      // "quit" will set this flag to terminate
      if (exit_access_mode) {
        bf_sys_free(line);
        exit_access_mode = 0;
        return;
      }
    }

    bf_sys_free(line);
  }
}

/* Execute a command line. */
int execute_line(char *line) {
  register int i;
  COMMAND *dev_command;
  char *word;
  char *remnant;
  char *key_wd, *key_no;
  bf_dev_id_t dev_id;
  bf_subdev_id_t subdev_id;

  i = 0;
  /* Isolate the command word. */
  while (line[i] && isspace(line[i])) i++;
  word = line + i;

  while (line[i] && !isspace(line[i])) i++;

  if (line[i]) line[i++] = '\0';

  dev_command = find_command(word);

  if (!dev_command) {
    fprintf(rl_outstream, "%s: No such command.\n", word);
    return (-1);
  }

  /* Get argument to command, if any. */
  while (isspace(line[i])) i++;

  word = line + i;

  /* Expect key_wd to be dev_x-y where x is the device id and y is the sub-
   * device id. */
  key_wd = next_word(word, &remnant);
  key_no = strchr(key_wd, '_');
  subdev_id = 0;
  if (key_no == NULL) {
    /* Indicates that the device id was not entered correctly. */
    dev_id = 0;
  } else {
    dev_id = strtoul(key_no + 1, NULL, 16);
    key_no = strchr(key_wd, '-');
    if (key_no == NULL) {
      subdev_id = 0;
    } else {
      subdev_id = strtoul(key_no + 1, NULL, 16);
    }
  }
  /* strip spaces until next keywd */
  while (*remnant == ' ') remnant++;

  /* Call the function. */
  return ((*(dev_command->func))(remnant, dev_id, subdev_id));
}

/* Look up NAME as the name of a command, and return a pointer to that
   command.  Return a NULL pointer if NAME isn't a command name. */
COMMAND *find_command(name) char *name;
{
  register int i = 0;

  for (i = 0; commands[i].name; i++)
    if (strcmp(name, commands[i].name) == 0) return (&commands[i]);

  return ((COMMAND *)NULL);
}

/* Strip whitespace from the start and end of STRING.  Return a pointer
   into STRING. */
char *stripwhite(string) char *string;
{
  register char *s, *t;

  for (s = string; isspace(*s); s++)
    ;

  if (*s == 0) return (s);

  t = s + strlen(s) - 1;
  while (t > s && isspace(*t)) t--;
  *++t = '\0';

  return s;
}

/* **************************************************************** */
/*                                                                  */
/*                  Interface to Readline Completion                */
/*                                                                  */
/* **************************************************************** */

char *command_generator __P((const char *, int));
char **fileman_completion __P((const char *, int, int));

/* Tell the GNU Readline library how to complete.  We want to try to
   complete on command names if this is the first word in the line, or
   on filenames if not.
*/
void initialize_readline(void) {
  /* Allow conditional parsing of the ~/.inputrc file. */
  rl_readline_name = "FileMan";

  /* Tell the completer that we want a crack first. */
  rl_attempted_completion_function = fileman_completion;

  /* initialize readline */
  rl_initialize();
}

/* Attempt to complete on the contents of TEXT.  START and END
   bound the region of rl_line_buffer that contains the word to
   complete.  TEXT is the word to complete.  We can use the entire
   contents of rl_line_buffer in case we want to do some simple
   parsing.  Returnthe array of matches, or NULL if there aren't any.
*/
char **fileman_completion(text, start, end) const char *text;
int start, end;
{
  char **matches;

  (void)start;
  (void)end;

  matches = rl_completion_matches(text, command_generator);

  // skip default completer operations
  rl_attempted_completion_over = 1;

  return (matches);
}

//============================================
// hack to resolve some improperly formed names

// New funcs that seem to be generated differently
static inline uint32_t getp_rsvd_fld(uint32_t *csr) {
  return (*csr & 0xfffffffful);
}
static inline void setp_rsvd_fld(uint32_t *csr, uint32_t value) {
  *csr = (value & 0xfffffffful) | (*csr & (~0xfffffffful));
}

static inline uint32_t getp_prsr_reg_main_rspec_mbus_int_inj(uint32_t *csr) {
  return (*csr & 0xfffffffful);
}
static inline void setp_prsr_reg_main_rspec_mbus_int_inj(uint32_t *csr,
                                                         uint32_t value) {
  *csr = (value & 0xfffffffful) | (*csr & (~0xfffffffful));
}

static inline uint32_t getp_prsr_reg_common_rspec_mbus_int_inj(uint32_t *csr) {
  return (*csr & 0xfffffffful);
}
static inline void setp_prsr_reg_common_rspec_mbus_int_inj(uint32_t *csr,
                                                           uint32_t value) {
  *csr = (value & 0xfffffffful) | (*csr & (~0xfffffffful));
}

static inline uint32_t getp_mac_addrmap_dummy_register(uint32_t *csr) {
  return (*csr & 0xfffffffful);
}
static inline void setp_mac_addrmap_dummy_register(uint32_t *csr,
                                                   uint32_t value) {
  *csr = (value & 0xfffffffful) | (*csr & (~0xfffffffful));
}

static inline uint32_t getp_serdes_addrmap_dummy_register(uint32_t *csr) {
  return (*csr & 0xfffffffful);
}
static inline void setp_serdes_addrmap_dummy_register(uint32_t *csr,
                                                      uint32_t value) {
  *csr = (value & 0xfffffffful) | (*csr & (~0xfffffffful));
}
// end hack
//=================================

//#include "comira_reg_access_autogen.h"
#include "comira_dbg_info_autogen.h"
#include "comira_dbg_access_autogen.h"
#include "reg_list_autogen.h"
#include "mem_list_autogen.h"
#include "tof2_reg_list_autogen.h"
#include "tof2_mem_list_autogen.h"
#include "tof3_reg_list_autogen.h"
#include "tof3_mem_list_autogen.h"



cmd_arg_item_t asic_cmd_list[] = {
    {"dev_0"},   {"dev_1"},   {"dev_2"},   {"dev_3"},   {"dev_4"},
    {"dev_5"},   {"dev_6"},   {"dev_7"},   {"dev_0-0"}, {"dev_0-1"},
    {"dev_1-0"}, {"dev_1-1"}, {"dev_2-0"}, {"dev_2-1"}, {"dev_3-0"},
    {"dev_3-1"}, {"dev_4-0"}, {"dev_4-1"}, {"dev_5-0"}, {"dev_5-1"},
    {"dev_6-0"}, {"dev_6-1"}, {"dev_7-0"}, {"dev_7-1"},
};

cmd_arg_t asic_cmd = {sizeof(asic_cmd_list) / sizeof(asic_cmd_list[0]),
                      asic_cmd_list};

cmd_arg_item_t cmd_list[] = {
    {"rr", &asic_cmd},
    {"wr", &asic_cmd},
    {"rm", &asic_cmd},
    {"wm", &asic_cmd},
    {"f", &asic_cmd},
    {"quit", &asic_cmd},
};
#define NO_OF_COMMANDS ((int)(sizeof(cmd_list) / sizeof(cmd_list[0])))

cmd_arg_t cmd = {NO_OF_COMMANDS, cmd_list};

#define interm_keywd_sz 132
char interim_keywd[interm_keywd_sz + 1] = {0};

static char *next_word(char *cmd_line, char **remnant) {
  char *last_char_p = cmd_line;
  int i = 0;

  // skip leading whitespace
  while (*last_char_p && (*last_char_p == ' ')) {
    last_char_p++;
  }

  while (*last_char_p && (*last_char_p != ' ')) {
    interim_keywd[i++] = *last_char_p;
    last_char_p++;
  }
  interim_keywd[i] = 0;
  *remnant = *last_char_p ? last_char_p++ : last_char_p;
  return interim_keywd;
}

cmd_arg_t *match_key_wd(cmd_arg_t *possible_args, char *cmd_line) {
  int i;
  char *remnant;
  char *key_wd = next_word(cmd_line, &remnant);

  for (i = 0; i < possible_args->n; i++) {
    if (!strcmp(key_wd, possible_args->arg[i].key_wd)) {
      if (possible_args->arg[i].next_arg == NULL) {
        return NULL;  // leaf node
      }
      return match_key_wd(possible_args->arg[i].next_arg, remnant);
    }
  }
  return possible_args;
}

static cmd_arg_t *get_chip_reg_tree_fam(bf_dev_family_t dev_fam) {
  switch (dev_fam) {
    case BF_DEV_FAMILY_TOFINO:
      return &tofino;
    case BF_DEV_FAMILY_TOFINO2:
      return &tof2_tof2_reg;
    case BF_DEV_FAMILY_TOFINO3:
      return &tof3_cb_reg;


    default:
      break;
  }
  /* Display Tofino-1 tree by default. */
  return &tofino;
}
static cmd_arg_t *get_chip_reg_tree(bf_dev_id_t dev_id) {
  return get_chip_reg_tree_fam(lld_dev_family_get(dev_id));
}

static cmd_arg_t *get_chip_mem_tree_fam(bf_dev_family_t dev_fam) {
  switch (dev_fam) {
    case BF_DEV_FAMILY_TOFINO:
      return &tofino;
    case BF_DEV_FAMILY_TOFINO2:
      return &tof2_mem;
    case BF_DEV_FAMILY_TOFINO3:
      return &tof3_cb_mem_mem;


    default:
      break;
  }
  /* Display Tofino-1 tree by default. */
  return &tofino;
}
static cmd_arg_t *get_chip_mem_tree(bf_dev_id_t dev_id) {
  return get_chip_mem_tree_fam(lld_dev_family_get(dev_id));
}

cmd_arg_t *determine_prs_st(char *cur_cmd) {
  char *remnant_1, *remnant_2;
  char *key_wd;
  char *key_no;
  bf_dev_id_t dev_id = 0;
  bf_subdev_id_t subdev_id = 0;
  cmd_arg_t *chip_tree;
  bool mem_read_write = false;

  next_word(cur_cmd, &remnant_1);
  key_wd = next_word(remnant_1, &remnant_2);
  key_no = strchr(key_wd, '_');
  if (key_no == NULL) {
    /* Indicates that the device id was not entered correctly.  Default to
     * Tofino-1. */
    chip_tree = &tofino;
  } else {
    // key_no for root is _x-y or _x.
    // x is dev y is subdev
    char *end_ptr = NULL;
    dev_id = strtoul(key_no + 1, &end_ptr, 16);
    if (dev_id < 0 || dev_id >= BF_MAX_DEV_COUNT) {
      return NULL;
    }
    if (end_ptr) {
      subdev_id = strtoul(end_ptr + 1, NULL, 16);
      if (subdev_id < 0 || subdev_id >= BF_MAX_SUBDEV_COUNT) {
        return NULL;
      }
    }

    if (!strncmp(cur_cmd, "rm", 2) || !strncmp(cur_cmd, "wm", 2)) {
      mem_read_write = true;
    }

    if (mem_read_write == false) {
      chip_tree = get_chip_reg_tree(dev_id);
    } else {
      chip_tree = get_chip_mem_tree(dev_id);
    }
  }

  /* Loop over each command (rr, wr, rm, etc.) */
  for (int i = 0; i < NO_OF_COMMANDS; i++) {
    /* For each command, loop over its next level commands (dev_0, dev_0-0,
     * dev_0-1, dev_1, etc.) to look for the commands which match the requested
     * device.  Note that for device X we expect 3 matches: dev_X, dev_X-0 and
     * dev_X-1.  We could probably hardcode the ordering of asic_cmd_list so
     * the three matches for a device are grouped together and directly go to
     * that group by dev_id*3 in the array but the following approach to use
     * a prefix match and scan all elements seems less brittle. */
    char requested_dev[16] = {0};
    sprintf(requested_dev, "dev_%d", dev_id);
    for (int j = 0; j < cmd.arg[i].next_arg->n; ++j) {
      /* For each command matching the device populate the chip specific command
       * tree. */
      if (!strncmp(cmd.arg[i].next_arg->arg[j].key_wd,
                   requested_dev,
                   strlen(requested_dev))) {
        cmd.arg[i].next_arg->arg[j].next_arg = chip_tree;
      }
    }
  }
  return match_key_wd(&cmd, cur_cmd);
}

static uint32_t accumulate_and_locate_offset(cmd_arg_t *possible_args,
                                             uint32_t cur_offset,
                                             uint32_t target_offset,
                                             cmd_arg_item_t **reg_desc,
                                             uint32_t *desc_offset) {
  int i;
  uint32_t test_offset;

  for (i = 0; i < possible_args->n; i++) {
    test_offset = cur_offset + possible_args->arg[i].offset;
    if (possible_args->arg[i].next_arg == NULL) {
      if (test_offset == target_offset) {
        if (reg_desc) {
          *reg_desc = &possible_args->arg[i];
        }
        return (test_offset);
      } else {
        int wid_size = (possible_args->arg[i].decoder->reg_width) / 32;
        int offset_diff = (int)target_offset - (int)test_offset;
        if ((wid_size > 1) && (offset_diff % 4 == 0) && (offset_diff / 4 > 0) &&
            (offset_diff / 4 < wid_size)) {
          if (reg_desc && desc_offset) {
            *reg_desc = &possible_args->arg[i];
            *desc_offset = (uint32_t)offset_diff / 4;
          }
          return (test_offset);
        }
      }
    } else {
      accumulate_and_locate_offset(possible_args->arg[i].next_arg,
                                   test_offset,
                                   target_offset,
                                   reg_desc,
                                   desc_offset);
    }
  }
  return -1;
}

char lld_gbl_path_var[1024] = {0};

static void accumulate_and_locate_offset_w_path(cmd_arg_t *possible_args,
                                                bool is_reg,
                                                uint64_t cur_offset,
                                                uint64_t target_offset,
                                                cmd_arg_item_t **reg_desc,
                                                uint32_t *desc_offset,
                                                char *path) {
  int i;
  uint64_t test_offset, next_offset;

  for (i = 0; i < possible_args->n; i++) {
    test_offset = cur_offset + possible_args->arg[i].offset;
    /* Prune the search space by checking if the address requested
     * (target_offset) is within the address range represented by this item.
     * This requires the entries in possible_args to be sorted by their address,
     * this is always the case given that is enforced by the tool used to
     * generate these structs. */
    if (i < (possible_args->n - 1)) {
      next_offset = cur_offset + possible_args->arg[i + 1].offset;
    } else {
      next_offset = 0xFFFFFFFFFFFFFFFFull;
    }
    if (target_offset < test_offset || target_offset >= next_offset) {
      /* Skip this item since it does not hold the target address. */
      continue;
    }

    if (possible_args->arg[i].next_arg == NULL) {
      if (test_offset == target_offset) {
        if (reg_desc) {
          *reg_desc = &possible_args->arg[i];
        }
        sprintf(lld_gbl_path_var, "%s__%s", path, possible_args->arg[i].key_wd);
        return;
      } else {
        bool found = false;
        int offset_diff = (int)target_offset - (int)test_offset;
        int wid_size = 0;
        if (is_reg) {
          wid_size = (possible_args->arg[i].decoder->reg_width) / 32;
          if ((wid_size > 1) && (offset_diff % 4 == 0) &&
              (offset_diff / 4 > 0) && (offset_diff / 4 < wid_size)) {
            found = true;
          }
        } else {
          uint64_t num_bytes =
              (uint64_t)lld_ptr_to_u64(possible_args->arg[i].no_rows);
          wid_size = num_bytes / 16;
          if (target_offset >= test_offset &&
              target_offset < (test_offset + wid_size)) {
            found = true;
          }
        }
        if (found) {
          if (reg_desc && desc_offset) {
            *reg_desc = &possible_args->arg[i];
            *desc_offset = (uint32_t)offset_diff / 4;
          }
          sprintf(lld_gbl_path_var,
                  "%s__%s_%d_%d",
                  path,
                  possible_args->arg[i].key_wd,
                  (offset_diff / 4),
                  wid_size);
          return;
        }
      }
    } else {
      char new_path[256];
      sprintf(new_path, "%s__%s", path, possible_args->arg[i].key_wd);
      accumulate_and_locate_offset_w_path(possible_args->arg[i].next_arg,
                                          is_reg,
                                          test_offset,
                                          target_offset,
                                          reg_desc,
                                          desc_offset,
                                          new_path);
    }
  }
  return;
}

char *lld_reg_parse_get_full_reg_path_name(bf_dev_family_t dev_family,
                                           uint32_t offset) {
  sprintf(lld_gbl_path_var, "<not found>");
  cmd_arg_t *chip_tree = get_chip_reg_tree_fam(dev_family);
  accumulate_and_locate_offset_w_path(
      chip_tree, true, 0, offset, NULL, NULL, "");
  return lld_gbl_path_var;
}

char *get_full_reg_path_name(bf_dev_id_t dev_id, uint32_t offset) {
  lld_dev_t *dev_p = lld_map_dev_id_to_dev_p(dev_id);
  return dev_p ? lld_reg_parse_get_full_reg_path_name(dev_p->dev_family, offset)
               : NULL;
}

static char *get_full_mem_path_name(bf_dev_id_t dev_id, uint64_t offset) {
  lld_dev_t *dev_p = lld_map_dev_id_to_dev_p(dev_id);

  sprintf(lld_gbl_path_var, "<not found>");

  if (!dev_p) {
    return lld_gbl_path_var;
  }

  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      accumulate_and_locate_offset_w_path(
          &pipe_top_level_mem, false, 0, offset, NULL, NULL, "");
      break;
    case BF_DEV_FAMILY_TOFINO2:
      accumulate_and_locate_offset_w_path(
          &tof2_mem, false, 0, offset, NULL, NULL, "");
      break;
    case BF_DEV_FAMILY_TOFINO3:
      accumulate_and_locate_offset_w_path(
          &tof3_cb_mem_mem, false, 0, offset, NULL, NULL, "");
      break;




    case BF_DEV_FAMILY_UNKNOWN:
      break;
  }
  return lld_gbl_path_var;
}

static int tof_dump_by_offset(bf_dev_id_t dev_id, uint32_t target_offset) {
  cmd_arg_item_t *reg_desc = NULL;
  uint32_t desc_offset = 0;
  accumulate_and_locate_offset(
      &tofino, 0, target_offset, &reg_desc, &desc_offset);
  if ((reg_desc == NULL) || (desc_offset != 0)) {
    fprintf(rl_outstream, "Register not found\n");
    return -1;
  } else {
    dump_and_decode_reg(dev_id, 0, target_offset, reg_desc);
    return 0;
  }
}
static int tof2_dump_by_offset(bf_dev_id_t dev_id, uint32_t target_offset) {
  cmd_arg_item_t *reg_desc = NULL;
  uint32_t desc_offset = 0;
  accumulate_and_locate_offset(
      &tof2_tof2_reg, 0, target_offset, &reg_desc, &desc_offset);
  if (reg_desc == NULL) {
    fprintf(rl_outstream, "Register not found\n");
    return -1;
  } else {
    dump_and_decode_reg(dev_id, 0, target_offset, reg_desc);
    return 0;
  }
}
static int tof3_dump_by_offset(bf_dev_id_t dev_id,
                               bf_subdev_id_t subdev_id,
                               uint32_t target_offset) {
  cmd_arg_item_t *reg_desc = NULL;
  uint32_t desc_offset = 0;
  accumulate_and_locate_offset(
      &tof3_cb_reg, 0, target_offset, &reg_desc, &desc_offset);
  if (reg_desc == NULL) {
    fprintf(rl_outstream, "Register not found\n");
    return -1;
  } else {
    dump_and_decode_reg(dev_id, subdev_id, target_offset, reg_desc);
    return 0;
  }
}

















int dump_by_offset(int dev_id,
                   bf_subdev_id_t subdev_id,
                   uint32_t target_offset) {
  switch (lld_dev_family_get(dev_id)) {
    case BF_DEV_FAMILY_TOFINO:
      return tof_dump_by_offset(dev_id, target_offset);
    case BF_DEV_FAMILY_TOFINO2:
      return tof2_dump_by_offset(dev_id, target_offset);
    case BF_DEV_FAMILY_TOFINO3:
      return tof3_dump_by_offset(dev_id, subdev_id, target_offset);


    default:
      break;
  }
  return 0;
}

int dump_field_name_by_offset(bf_dev_id_t dev_id,
                              uint32_t target_offset,
                              int bit) {
  cmd_arg_t *chip_tree = get_chip_reg_tree(dev_id);
  cmd_arg_item_t *reg_desc = NULL;
  uint32_t desc_offset = 0;
  accumulate_and_locate_offset(
      chip_tree, 0, target_offset, &reg_desc, &desc_offset);
  if (!reg_desc) return -1;

  int fld;
  reg_decoder_fld_t *fld_list;
  int bit_in_widereg = 32 * desc_offset + bit;
  if (reg_desc->decoder == NULL) {
    return -1;
  }
  fld_list = reg_desc->decoder->fld_list;
  for (fld = 0; fld < reg_desc->decoder->n_flds; fld++) {
    if ((fld_list[fld].lsb <= bit_in_widereg) &&
        (fld_list[fld].msb >= bit_in_widereg)) {
      if (rl_outstream) {
        fprintf(rl_outstream,
                "%-24s : %s",
                fld_list[fld].fld_name,
                fld_list[fld].fld_desc);
      } else {
        printf("%-24s : %s", fld_list[fld].fld_name, fld_list[fld].fld_desc);
      }
      return 0;
    }
  }
  return -1;
}

int dump_reg_name_by_offset(bf_dev_id_t dev_id, uint32_t target_offset) {
  cmd_arg_item_t *reg_desc = NULL;
  uint32_t desc_offset = 0;
  cmd_arg_t *chip_tree = get_chip_reg_tree(dev_id);

  accumulate_and_locate_offset(
      chip_tree, 0, target_offset, &reg_desc, &desc_offset);
  if ((reg_desc == NULL) || (desc_offset != 0)) {
    fprintf(rl_outstream, "<unknown>");
    return -1;
  } else {
    fprintf(rl_outstream, "%s", reg_desc->key_wd);
    return 0;
  }
}

int get_reg_width(bf_dev_id_t dev_id, uint32_t offset) {
  cmd_arg_item_t *reg_desc = NULL;
  uint32_t desc_offset = 0;
  cmd_arg_t *chip_tree = get_chip_reg_tree(dev_id);

  accumulate_and_locate_offset(chip_tree, 0, offset, &reg_desc, &desc_offset);
  if ((reg_desc == NULL) || (desc_offset != 0)) {
    return 0;
  }
  if (reg_desc->decoder == NULL) {
    return 0;
  }
  return reg_desc->decoder->reg_width;
}

int get_reg_num_fields(bf_dev_id_t dev_id, uint32_t offset) {
  cmd_arg_item_t *reg_desc = NULL;
  uint32_t desc_offset = 0;
  cmd_arg_t *chip_tree = get_chip_reg_tree(dev_id);

  accumulate_and_locate_offset(chip_tree, 0, offset, &reg_desc, &desc_offset);
  if ((reg_desc == NULL) || (desc_offset != 0)) {
    return 0;
  }
  if (reg_desc->decoder == NULL) {
    return 0;
  }
  return reg_desc->decoder->n_flds;
}

uint64_t accumulate_offset(cmd_arg_t *possible_args,
                           char *cmd_line,
                           uint64_t cur_offset,
                           cmd_arg_item_t **reg_desc) {
  int i;
  char *remnant;
  char *key_wd = next_word(cmd_line, &remnant);

  for (i = 0; i < possible_args->n; i++) {
    if (!strcmp(key_wd, possible_args->arg[i].key_wd)) {
      sprintf(&parse_matches[strlen(parse_matches)], "%s", key_wd);
      cur_offset += possible_args->arg[i].offset;
      if ((possible_args->arg[i].next_arg == NULL) || (*remnant == '\0')) {
        if (reg_desc) {
          *reg_desc = &possible_args->arg[i];
        }
        return cur_offset;
      }
      return accumulate_offset(
          possible_args->arg[i].next_arg, remnant, cur_offset, reg_desc);
    }
  }
  return -1;
}

static void set_asic_device_args(bf_dev_id_t dev_id,
                                 cmd_arg_t **args,
                                 bf_lld_decoder_access_type access_type) {
  lld_dev_t *dev_p = lld_map_dev_id_to_dev_p(dev_id);
  if (!dev_p) return;

  *args = NULL;

  if (access_type == REG_OP) {
    switch (dev_p->dev_family) {
      case BF_DEV_FAMILY_TOFINO:
        *args = &tofino;
        break;
      case BF_DEV_FAMILY_TOFINO2:
        *args = &tof2_tof2_reg;
        break;
      case BF_DEV_FAMILY_TOFINO3:
        *args = &tof3_cb_reg;
        break;



      case BF_DEV_FAMILY_UNKNOWN:
        break;
      default:
        break;
    }
  } else {
    switch (dev_p->dev_family) {
      case BF_DEV_FAMILY_TOFINO:
        *args = NULL;
        break;
      case BF_DEV_FAMILY_TOFINO2:
        *args = &tof2_mem;
        break;
      case BF_DEV_FAMILY_TOFINO3:
        *args = &tof3_cb_mem_mem;
        break;



      case BF_DEV_FAMILY_UNKNOWN:
        break;
      default:
        break;
    }
  }
  return;
}

uint64_t determine_offset(char *cmd_line,
                          cmd_arg_item_t **reg_desc,
                          bf_dev_id_t dev_id,
                          bf_lld_decoder_access_type access_type) {
  cmd_arg_t *asic_device_args = NULL;
  memset(parse_matches, 0, sizeof(parse_matches));
  set_asic_device_args(dev_id, &asic_device_args, access_type);
  return asic_device_args
             ? accumulate_offset(asic_device_args, cmd_line, 0x0, reg_desc)
             : 0;
}

static bf_status_t traverse_mem(bf_dev_id_t dev_id,
                                bf_subdev_id_t subdev_id,
                                cmd_arg_item_t *item,
                                uint64_t offset,
                                bf_status_t *sts,
                                lld_mem_traverse_cb fn) {
  int i;
  cmd_arg_t *args = item->next_arg;
  bf_status_t status = BF_SUCCESS;

  if (!args) {
    return BF_INVALID_ARG;
  }
  for (i = 0; i < args->n; i++) {
    if (user_wants_to_abort()) return BF_SUCCESS;

    if (args->arg[i].next_arg == NULL) {
      uint64_t len64 = (uint64_t)lld_ptr_to_u64(args->arg[i].no_rows);
      uint32_t len32 = (uint32_t)len64;

      status = fn(dev_id,
                  subdev_id,
                  offset + args->arg[i].offset,
                  len32,
                  get_full_mem_path_name(dev_id, offset + args->arg[i].offset));
      if (status != BF_SUCCESS) {
        lld_memtest_result_set(false);
        if (*sts == BF_SUCCESS) {
          /* Save first error encountered. */
          *sts = status;
        }
      }
      continue;
    }
    traverse_mem(dev_id,
                 subdev_id,
                 &args->arg[i],
                 offset + args->arg[i].offset,
                 sts,
                 fn);
  }
  return BF_SUCCESS;
}

bf_status_t lld_traverse_all_mems(bf_dev_id_t dev_id,
                                  lld_mem_traverse_cb fn,
                                  bool all_mems,
                                  bool tm_only) {
  cmd_arg_t *top = &pipe_top_level_mem;
  int i, xpb, prsr, start_index = 0, num_indexes = 0;
  bf_subdev_id_t subdev_id = 0;
  bf_status_t status = BF_SUCCESS;
  bool is_tofino = lld_dev_is_tofino(dev_id);
  bool is_tofino2 = lld_dev_is_tof2(dev_id);
  bool is_tofino3 = lld_dev_is_tof3(dev_id);
  if (is_tofino) {
    top = &pipe_top_level_mem;
    start_index = 0;
    num_indexes = 5;
  } else if (is_tofino2) {
    /* Turn off parde memory write multicasting. */
    uint32_t num_pipes = 0;
    bf_dev_pipe_t p = 0;
    bf_dev_pipe_t phy_pipe = 0;
    lld_sku_get_num_active_pipes(dev_id, &num_pipes);
    for (p = 0; p < num_pipes; ++p) {
      lld_sku_map_pipe_id_to_phy_pipe_id(dev_id, p, &phy_pipe);
      for (xpb = 0; xpb < 9; ++xpb) {
        for (prsr = 0; prsr < 4; ++prsr) {
          uint32_t a = offsetof(tof2_reg,
                                pipes[phy_pipe]
                                    .pardereg.pgstnreg.ipbprsr4reg[xpb]
                                    .prsr[prsr]
                                    .mem_ctrl);
          lld_subdev_write_register(dev_id, 0, a, prsr);
          a = offsetof(tof2_reg,
                       pipes[phy_pipe]
                           .pardereg.pgstnreg.epbprsr4reg[xpb]
                           .prsr[prsr]
                           .mem_ctrl);
          lld_subdev_write_register(dev_id, 0, a, prsr);
        }
      }
    }

    top = &tof2_mem;
    start_index = 0;
    num_indexes = 5;
  } else if (is_tofino3) {
    /* Turn off parde memory write multicasting. */
    uint32_t num_pipes = 0, p, phy_pipe = 0;
    lld_sku_get_num_active_pipes(dev_id, &num_pipes);
    for (p = 0; p < num_pipes; ++p) {
      if (!(lld_diag_cfg.pipe_mask & (1 << p))) continue;
      lld_sku_map_pipe_id_to_phy_pipe_id(dev_id, p, &phy_pipe);
      phy_pipe = phy_pipe % BF_SUBDEV_PIPE_COUNT;
      subdev_id = phy_pipe / BF_SUBDEV_PIPE_COUNT;
      for (xpb = 0; xpb < 9; ++xpb) {
        for (prsr = 0; prsr < 4; ++prsr) {
          uint32_t a = offsetof(tof3_reg,
                                pipes[phy_pipe]
                                    .pardereg.pgstnreg.ipbprsr4reg[xpb]
                                    .prsr[prsr]
                                    .mem_ctrl);
          lld_subdev_write_register(dev_id, subdev_id, a, prsr);
          a = offsetof(tof3_reg,
                       pipes[phy_pipe]
                           .pardereg.pgstnreg.epbprsr4reg[xpb]
                           .prsr[prsr]
                           .mem_ctrl);
          lld_subdev_write_register(dev_id, subdev_id, a, prsr);
        }
      }
    }

    top = &tof3_cb_mem_mem;
    if (all_mems) {
      start_index = 0;
      num_indexes = 5;
    } else if (tm_only) {
      start_index = 0;
      num_indexes = 1;
    } else {
      start_index = 1;
      num_indexes = 4;
    }
  }

  /* Init test result to pass */
  lld_memtest_results_clear();
  lld_memtest_result_set(true);

  status = lld_ind_reg_lock_init();
  if (status != BF_SUCCESS) {
    return status;
  }

  for (i = start_index; (i < top->n) && (i < (start_index + num_indexes));
       i++) {
    bf_status_t s = BF_SUCCESS;
    traverse_mem(dev_id, subdev_id, &top->arg[i], top->arg[i].offset, &s, fn);
    if (s != BF_SUCCESS && status == BF_SUCCESS) {
      /* Save the first error return. */
      status = s;
    }
  }
  if (is_tofino2) {
    /* Restore parser memory multicast. */
    uint32_t num_pipes = 0;
    bf_dev_pipe_t p = 0;
    bf_dev_pipe_t phy_pipe = 0;
    lld_sku_get_num_active_pipes(dev_id, &num_pipes);
    for (p = 0; p < num_pipes; ++p) {
      lld_sku_map_pipe_id_to_phy_pipe_id(dev_id, p, &phy_pipe);
      for (xpb = 0; xpb < 9; ++xpb) {
        for (prsr = 0; prsr < 4; ++prsr) {
          uint32_t a = offsetof(tof2_reg,
                                pipes[phy_pipe]
                                    .pardereg.pgstnreg.ipbprsr4reg[xpb]
                                    .prsr[prsr]
                                    .mem_ctrl);
          lld_subdev_write_register(dev_id, 0, a, 0);
          a = offsetof(tof2_reg,
                       pipes[phy_pipe]
                           .pardereg.pgstnreg.epbprsr4reg[xpb]
                           .prsr[prsr]
                           .mem_ctrl);
          lld_subdev_write_register(dev_id, 0, a, 0);
        }
      }
    }
  }
  if (is_tofino3) {
    /* Restore parser memory multicast. */
    uint32_t num_pipes = 0, p, phy_pipe = 0;
    lld_sku_get_num_active_pipes(dev_id, &num_pipes);
    for (p = 0; p < num_pipes; ++p) {
      if (!(lld_diag_cfg.pipe_mask & (1 << p))) continue;
      lld_sku_map_pipe_id_to_phy_pipe_id(dev_id, p, &phy_pipe);
      phy_pipe = phy_pipe % BF_SUBDEV_PIPE_COUNT;
      subdev_id = phy_pipe / BF_SUBDEV_PIPE_COUNT;
      for (xpb = 0; xpb < 9; ++xpb) {
        for (prsr = 0; prsr < 4; ++prsr) {
          uint32_t a = offsetof(tof3_reg,
                                pipes[phy_pipe]
                                    .pardereg.pgstnreg.ipbprsr4reg[xpb]
                                    .prsr[prsr]
                                    .mem_ctrl);
          lld_subdev_write_register(dev_id, subdev_id, a, 0);
          a = offsetof(tof3_reg,
                       pipes[phy_pipe]
                           .pardereg.pgstnreg.epbprsr4reg[xpb]
                           .prsr[prsr]
                           .mem_ctrl);
          lld_subdev_write_register(dev_id, subdev_id, a, 0);
        }
      }
    }
  }

  return status;
}

typedef void (*reg_traverse_cb)(bf_dev_id_t dev_id,
                                bf_subdev_id_t subdev_id,
                                char *reg_name,
                                uint64_t offset);

void traverse_regs_and_build_path(bf_dev_id_t dev_id,
                                  bf_subdev_id_t subdev_id,
                                  cmd_arg_item_t *item,
                                  uint32_t offset,
                                  char *path,
                                  reg_traverse_cb fn) {
  int i;
  cmd_arg_t *args = item->next_arg;
  char new_path[256];

  if (!args) {
    return;
  }
  for (i = 0; i < args->n; i++) {
    if (args->arg[i].next_arg == NULL) {
      char full_name[256];
      sprintf(full_name, "%s__%s", path, args->arg[i].key_wd);
      fn(dev_id, subdev_id, full_name, offset + args->arg[i].offset);
    }
    sprintf(new_path, "%s__%s", path, args->arg[i].key_wd);
    traverse_regs_and_build_path(dev_id,
                                 subdev_id,
                                 &args->arg[i],
                                 offset + args->arg[i].offset,
                                 new_path,
                                 fn);
  }
}

void traverse_regs(bf_dev_id_t dev_id,
                   bf_subdev_id_t subdev_id,
                   reg_traverse_cb fn) {
  cmd_arg_t *top = NULL;
  int i;
  char path[256] = {0};
  lld_dev_t *dev_p = lld_map_subdev_id_to_dev_p(dev_id, subdev_id);

  if (!dev_p) {
    return;
  }

  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      top = &tofino;
      subdev_id = 0;
      break;
    case BF_DEV_FAMILY_TOFINO2:
      top = &tof2_tof2_reg;
      subdev_id = 0;
      break;
    case BF_DEV_FAMILY_TOFINO3:
      top = &tof3_cb_reg;
      break;



    case BF_DEV_FAMILY_UNKNOWN:
      return;
  }

  for (i = 0; i < top->n; i++) {
    sprintf(path, "%s", top->arg[i].key_wd);
    traverse_regs_and_build_path(
        dev_id, subdev_id, &top->arg[i], top->arg[i].offset, path, fn);
  }

  return;
}

static void traverse_int_regs_and_build_path(bf_dev_id_t dev_id,
                                             cmd_arg_item_t *item,
                                             uint32_t offset,
                                             char *path,
                                             reg_traverse_cb fn) {
  int i;
  cmd_arg_t *args = item->next_arg;
  char new_path[256];

  if (!args) {
    return;
  }
  for (i = 0; i < args->n; i++) {
    if ((args->arg[i].next_arg == NULL) && (args->arg[i].decoder->is_int_reg)) {
      char full_name[256];
      sprintf(full_name, "%s__%s", path, args->arg[i].key_wd);
      fn(dev_id, 0, full_name, offset + args->arg[i].offset);
    }
    sprintf(new_path, "%s__%s", path, args->arg[i].key_wd);
    traverse_int_regs_and_build_path(
        dev_id, &args->arg[i], offset + args->arg[i].offset, new_path, fn);
  }
}

void traverse_int_regs(bf_dev_id_t dev_id, reg_traverse_cb fn) {
  cmd_arg_t *top = &tofino;
  int i;
  char path[256] = {0};
  lld_dev_t *dev_p = lld_map_dev_id_to_dev_p(dev_id);
  if (!dev_p) return;
  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      top = &tofino;
      break;
    case BF_DEV_FAMILY_TOFINO2:
      top = &tof2_tof2_reg;
      break;
    case BF_DEV_FAMILY_TOFINO3:
      top = &tof3_cb_reg;
      break;



    case BF_DEV_FAMILY_UNKNOWN:
      return;
  }

  for (i = 0; i < top->n; i++) {
    sprintf(path, "%s", top->arg[i].key_wd);
    traverse_int_regs_and_build_path(
        dev_id, &top->arg[i], top->arg[i].offset, path, fn);
  }
}

#include <sys/types.h>
#include <regex.h>

char *g_cur_pattern;
regex_t g_regex;

static void tof_regx_traverse_cb(bf_dev_id_t dev_id,
                                 bf_subdev_id_t subdev_id,
                                 char *reg_name,
                                 uint64_t offset) {
  int rc;
  (void)subdev_id;

  rc = regexec(&g_regex, reg_name, 0, NULL, 0);
  if (rc == 0) {
    tof_dump_by_offset(dev_id, (uint32_t)offset);
  }
}
static void tof2_regx_traverse_cb(bf_dev_id_t dev_id,
                                  bf_subdev_id_t subdev_id,
                                  char *reg_name,
                                  uint64_t offset) {
  int rc;
  (void)subdev_id;

  rc = regexec(&g_regex, reg_name, 0, NULL, 0);
  if (rc == 0) {
    tof2_dump_by_offset(dev_id, (uint32_t)offset);
  }
}
static void tof3_regx_traverse_cb(bf_dev_id_t dev_id,
                                  bf_subdev_id_t subdev_id,
                                  char *reg_name,
                                  uint64_t offset) {
  int rc;

  rc = regexec(&g_regex, reg_name, 0, NULL, 0);
  if (rc == 0) {
    tof3_dump_by_offset(dev_id, subdev_id, (uint32_t)offset);
  }
}














static void regx_cmd_handler(bf_dev_id_t dev_id,
                             bf_subdev_id_t subdev_id,
                             char *pattern) {
  int rc;

  rc = regcomp(&g_regex, pattern, 0);
  if (rc != 0) {
    fprintf(rl_outstream, "Invalid regular expression: '%s'\n", pattern);
    return;
  }
  g_cur_pattern = pattern;
  lld_dev_t *dev_p = lld_map_subdev_id_to_dev_p(dev_id, subdev_id);
  if (!dev_p) return;
  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      traverse_regs(dev_id, 0, tof_regx_traverse_cb);
      break;
    case BF_DEV_FAMILY_TOFINO2:
      traverse_regs(dev_id, 0, tof2_regx_traverse_cb);
      break;
    case BF_DEV_FAMILY_TOFINO3:
      traverse_regs(dev_id, subdev_id, tof3_regx_traverse_cb);
      break;



    case BF_DEV_FAMILY_UNKNOWN:
      break;
  }
}

void traverse_args(bf_dev_family_t dev_family,
                   cmd_arg_item_t *item,
                   uint64_t offset,
                   bf_lld_decoder_access_type access_type) {
  int i;
  cmd_arg_t *args = item->next_arg;

  if (!args) {
    return;
  }
  for (i = 0; i < args->n; i++) {
    if (user_wants_to_abort()) return;

    if (args->arg[i].next_arg == NULL) {
      if (access_type != MEM_OP) {
        dump_reg(dev_family, &args->arg[i], offset + args->arg[i].offset);
      } else {
        dump_mem(&args->arg[i], offset + args->arg[i].offset);
      }
    }
    traverse_args(
        dev_family, &args->arg[i], offset + args->arg[i].offset, access_type);
  }
}

void find_pattern(cmd_arg_t *args, regex_t *regex, char *prefix) {
  int i, f;
  char new_prefix[256] = {0};

  if (!args) {
    return;
  }
  for (i = 0; i < args->n; i++) {
    int ret;

    ret = regexec(regex, args->arg[i].key_wd, 0, NULL, 0);
    if (!ret) {
      fprintf(rl_outstream, "%s %s\n", prefix, args->arg[i].key_wd);
    }
    if (args->arg[i].next_arg != NULL) {
      sprintf(new_prefix, "%s %s", prefix, args->arg[i].key_wd);
      find_pattern(args->arg[i].next_arg, regex, new_prefix);
    } else if (args->arg[i].decoder != NULL) {
      // go thru the fields too
      reg_decoder_t *decoder = args->arg[i].decoder;

      for (f = 0; f < decoder->n_flds; f++) {
        ret = regexec(regex, decoder->fld_list[f].fld_name, 0, NULL, 0);
        if (!ret) {
          fprintf(rl_outstream,
                  "%s %s.%s\n",
                  prefix,
                  args->arg[i].key_wd,
                  decoder->fld_list[f].fld_name);
        }
      }
    }
  }
}

void find_string_in_regs(char *pattern, bf_dev_id_t dev_id) {
  regex_t regex;
  int ret;

  ret = regcomp(&regex, pattern, 0);
  if (ret != 0) {
    fprintf(rl_outstream, "malformed pattern: <%s>\n", pattern);
    return;
  }
  lld_dev_t *dev_p = lld_map_dev_id_to_dev_p(dev_id);
  if (!dev_p) return;
  switch (dev_p->dev_family) {
    case BF_DEV_FAMILY_TOFINO:
      find_pattern(&tofino, &regex, "");
      break;
    case BF_DEV_FAMILY_TOFINO2:
      find_pattern(&tof2_tof2_reg, &regex, "");
      break;
    case BF_DEV_FAMILY_TOFINO3:
      find_pattern(&tof3_cb_reg, &regex, "");
      break;



    case BF_DEV_FAMILY_UNKNOWN:
      fprintf(rl_outstream, "Unknown Device\n");
      break;
  }
}

/* Generator function for command completion.  STATE lets us
   know whether to start from scratch; without any state
   (i.e. STATE == 0), then we start at the top of the list. */
char *command_generator(const char *text, int state) {
  static int list_index, len;
  static cmd_arg_t *possible_args;

  /* If this is a new word to complete, initialize now.  This
     includes saving the length of TEXT for efficiency, and
     initializing the index variable to 0. */
  if (!state) {
    list_index = 0;
    len = strlen(text);
    rl_line_buffer[rl_point] = 0;
    possible_args = determine_prs_st(rl_line_buffer);
  }
  if (possible_args == NULL) {  // leaf node
    return NULL;
  }

  while (list_index < possible_args->n) {
    char *key_wd = possible_args->arg[list_index++].key_wd;

    if (strncmp(key_wd, text, len) == 0) {
      return (dupstr(key_wd));
    }
  }

  /* If no names matched, then return NULL. */
  return ((char *)NULL);
}
