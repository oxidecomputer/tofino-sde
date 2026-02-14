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


/*
 *  APIs for hash computation , table entry formatting.
 *
 * BareFoot Networks Inc.
 */

#ifndef __PIPE_MGR_TABLE_PACKING__
#define __PIPE_MGR_TABLE_PACKING__

#include <target-utils/bit_utils/bit_utils.h>
#include <target-utils/third-party/cJSON/cJSON.h>
#include <inttypes.h>
#include <stdint.h>
#include "pipe_mgr_exm_hash.h"
#include "pipe_mgr_int.h"
#include "pipe_mgr_mau_snapshot.h"
#include "pipe_mgr_parse_ctx_json.h"
#include "pipe_mgr_tof2_mau_snapshot.h"

#include <tofino/pdfixed/pd_common.h>

#define PIPEMGR_TBL_PKG_HASH_COLLISION_FACTOR \
  (16) /* Higher value                        \
        * implies more                        \
        * memory for LUT                      \
        * but  provides                       \
        * ability to                          \
        * handle more                         \
        * hash collisons                      \
        */

pipe_status_t bf_hash_mat_entry_hash_compute(bf_dev_id_t devid,
                                             profile_id_t prof_id,
                                             dev_stage_t stage_id,
                                             pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                             pipe_tbl_match_spec_t *match_spec,
                                             bool proxy_hash,
                                             pipe_exm_hash_t *hash_bits);

pipe_status_t bf_hash_mat_entry_hash2_compute(bf_dev_id_t devid,
                                              profile_id_t prof_id,
                                              dev_stage_t stage_id,
                                              pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                              pipe_tbl_match_spec_t *match_spec,
                                              bool proxy_hash,
                                              pipe_exm_hash_t *hash_bits);

pipe_status_t bf_hash_mat_entry_radix_hash_compute(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    dev_stage_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec,
    bool proxy_hash,
    pipe_exm_hash_t *hash_bits);

pipe_status_t bf_hash_mat_entry_hash_action_match_spec_decode_from_hash(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    dev_stage_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint64_t hash_value,
    bool proxy_hash,
    pipe_tbl_match_spec_t *match_spec);

pipe_status_t bf_hash_get_hash_bits_for_key_bit(bf_dev_id_t devid,
                                                profile_id_t prof_id,
                                                dev_stage_t stage_id,
                                                pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                                int key_bit,
                                                bool proxy_hash,
                                                uint64_t *hash_bits);

pipe_status_t bf_hash_get_hash_seed(bf_dev_id_t devid,
                                    profile_id_t prof_id,
                                    dev_stage_t stage_id,
                                    pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                    bool proxy_hash,
                                    uint64_t *seed);

typedef struct pipe_mgr_exm_hash_info_for_decode_ {
  uint64_t hash;
  uint8_t hash_bit_lo;
  uint8_t hash_bit_hi;
  uint8_t num_ram_select_bits;
  uint8_t ram_select_lo;
  uint8_t ram_select_hi;
  uint8_t wide_hash_idx;
  uint8_t num_subword_bits;
} pipe_mgr_exm_hash_info_for_decode_t;

/* A structure to hold all the possible indirect ptrs a match entry overhead may
 * house.
 */
typedef struct pipe_mgr_indirect_ptrs_ {
  uint32_t adt_ptr;
  uint32_t sel_ptr;
  uint32_t meter_ptr;
  uint32_t stats_ptr;
  uint32_t stfl_ptr;
  uint32_t sel_len;
  uint32_t sel_len_shift;
} pipe_mgr_indirect_ptrs_t;

typedef struct pipe_mgr_ent_decode_ptr_info_t {
  bool force_stats;
  bool force_meter;
  bool force_stful;
} pipe_mgr_ent_decode_ptr_info_t;

pipe_status_t bf_hash_mat_entry_ghost_bits_compute(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    dev_stage_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec,
    struct pipe_mgr_exm_hash_info_for_decode_ *hash_info);

char *pipemgr_tbl_pkg_get_field_string_name(bf_dev_id_t devid,
                                            profile_id_t prof_id,
                                            uint32_t index);

/* --- Format entry functions according to table packing syntax --- */

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
    bool is_stash);

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
    struct pipe_mgr_exm_hash_info_for_decode_ *hash_info,
    pipe_mgr_indirect_ptrs_t *indirect_ptrs,
    pipe_mgr_ent_decode_ptr_info_t *ptr_info,
    pipe_act_fn_hdl_t *act_fn_hdl_p,
    bool *ram_ids_read,
    bool is_stash,
    uint64_t *proxy_hash);

pipe_status_t pipe_mgr_entry_format_tof_tern_tbl_ent_decode_to_match_spec(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec,
    uint8_t **tbl_words,
    uint8_t *version_key_p,
    uint8_t *version_mask_p,
    bool *is_range_entry_head);

pipe_status_t pipe_mgr_entry_format_tof_range_max_expansion_entry_count_get(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint32_t *count);

pipe_status_t pipe_mgr_entry_format_tof_exm_get_next_tbl(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint8_t stage_table_handle,
    pipe_act_fn_hdl_t act_fn_hdl,
    uint32_t *next_table);

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
    bool is_stash);

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
    tind_tbl_word_t *tind_tbl_word);

pipe_status_t pipe_mgr_entry_format_tof_tbl_uses_imm_data(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_act_fn_hdl_t act_fn_hdl,
    bool is_tind,
    bool *uses_imm_data);

pipe_status_t pipe_mgr_entry_format_adt_tbl_used(rmt_dev_info_t *dev_info,
                                                 profile_id_t prof_id,
                                                 dev_stage_t stage_id,
                                                 pipe_adt_tbl_hdl_t adt_tbl_hdl,
                                                 pipe_act_fn_hdl_t act_fn_hdl,
                                                 bool *adt_used);

pipe_status_t pipe_mgr_entry_format_tof_adt_tbl_ent_update(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_adt_tbl_hdl_t adt_tbl_hdl,
    pipe_act_fn_hdl_t act_fn_hdl,
    pipe_action_data_spec_t *act_data_spec,
    uint8_t entry_position,
    uint8_t **adt_tbl_word);

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
    bool tcam_words_updated[TOF_MAX_TCAM_WORDS_IN_TERN_TBL_WORD]);

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
    pipe_register_spec_t *register_spec_list);

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
    uint32_t *selector_len);

pipe_status_t pipe_mgr_entry_format_tbl_default_entry_prepare(
    rmt_dev_info_t *dev_info,
    uint8_t direction,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    bf_dev_pipe_t log_pipe,
    pipe_register_spec_t *register_spec_list);

void pipe_mgr_entry_format_tbl_default_entry_print(
    rmt_dev_info_t *dev_info,
    pipe_register_spec_t *register_spec_list,
    bf_dev_pipe_t pipe,
    char *str,
    int *c_str_len,
    int max_len);

/* --- Decode entry functions based on table packing syntax --- */

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
    bool is_stash);

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
    uint64_t *addr_arr);

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
    pipe_mgr_ent_decode_ptr_info_t *ptr_info);

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
    uint64_t *addr_arr);

pipe_status_t pipe_mgr_entry_format_tof_adt_tbl_ent_decode_to_action_spec(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    uint8_t stage_id,
    pipe_adt_tbl_hdl_t adt_tbl_hdl,
    pipe_act_fn_hdl_t act_fn_hdl,
    uint8_t entry_position,
    pipe_action_data_spec_t *act_data_spec,
    uint8_t **adt_tbl_word);

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
    uint8_t *word1);

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
    uint64_t *addr_arr);

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
    bool version);

pipe_status_t pipe_mgr_entry_format_tof_phase0_tbl_get_match_port(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec,
    uint32_t *match_port);

pipe_status_t pipe_mgr_entry_format_tof_phase0_tbl_get_match_spec(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint32_t match_port,
    pipe_tbl_match_spec_t *match_spec);

pipe_status_t pipe_mgr_entry_format_phase0_tbl_update(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint32_t match_port,
    pipe_action_data_spec_t *act_data_spec,
    pipe_register_spec_t *register_spec_list,
    pipe_memory_spec_t *memory_spec_list);

pipe_status_t pipe_mgr_entry_format_phase0_tbl_addr_get(
    rmt_dev_info_t *dev_info,
    uint32_t port,
    bool is_write,
    bf_subdev_id_t *subdev,
    pipe_register_spec_t *register_spec_list,
    pipe_memory_spec_t *memory_spec_list);

pipe_status_t pipe_mgr_entry_format_phase0_tbl_decode_reg_to_action_data(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_register_spec_t *register_spec_list,
    pipe_action_data_spec_t *act_data_spec);

pipe_status_t pipe_mgr_entry_format_tof_phase0_tbl_print(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_register_spec_t *reg_spec_list,
    char *str,
    int *c_str_len,
    int max_len);

pipe_status_t pipe_mgr_entry_format_print_match_spec(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec,
    char *dest_str,
    size_t dest_str_size,
    size_t *bytes_written);

pipe_status_t pipe_mgr_entry_format_print_action_spec(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_action_data_spec_t *action_spec,
    pipe_act_fn_hdl_t act_fn_hdl,
    char *dest_str,
    size_t dest_str_size,
    size_t *bytes_written);

pipe_status_t pipe_mgr_entry_format_log_match_spec(
    bf_dev_id_t devid,
    int log_lvl,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec);

pipe_status_t pipe_mgr_entry_format_log_action_spec(
    bf_dev_id_t devid,
    int log_lvl,
    profile_id_t prof_id,
    pipe_action_spec_t *action_spec,
    pipe_act_fn_hdl_t act_fn_hdl);

pipe_status_t pipe_mgr_entry_format_jsonify_match_spec(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec,
    cJSON *ms_node);

pipe_status_t pipe_mgr_entry_format_unjsonify_match_spec(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec,
    cJSON *ms_node);

pipe_status_t pipe_mgr_entry_format_jsonify_action_spec(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_action_data_spec_t *action_spec,
    pipe_act_fn_hdl_t act_fn_hdl,
    cJSON *as_node);

pipe_status_t pipe_mgr_entry_format_unjsonify_action_spec(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_action_data_spec_t *action_spec,
    pipe_act_fn_hdl_t act_fn_hdl,
    cJSON *as_node);

pipe_status_t pipe_mgr_entry_format_ms_bit_to_field(bf_dev_id_t dev_id,
                                                    profile_id_t prof_id,
                                                    pipe_mat_tbl_hdl_t tbl_hdl,
                                                    int bit,
                                                    char **field_name,
                                                    int *field_bit);

size_t pipe_mgr_entry_format_lrn_cfg_type_sz(bf_dev_id_t devid,
                                             profile_id_t prof_id,
                                             uint8_t learn_cfg_type);

pipe_fld_lst_hdl_t pipe_mgr_entry_format_get_handle_of_lrn_cfg_type(
    bf_dev_id_t devid, profile_id_t prof_id, uint8_t learn_cfg_type);

uint8_t pipe_mgr_entry_format_fld_lst_hdl_to_lq_cfg_type(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_fld_lst_hdl_t flow_lrn_fld_lst_hdl);

pipe_status_t pipe_mgr_entry_format_fld_lst_hdl_to_profile(
    bf_dev_id_t devid,
    pipe_fld_lst_hdl_t flow_lrn_fld_lst_hdl,
    profile_id_t *ret_prof_id);

pipe_status_t pipe_mgr_entry_format_lrn_decode(rmt_dev_info_t *dev_info,
                                               profile_id_t prof_id,
                                               uint8_t pipe,
                                               uint8_t learn_cfg_type,
                                               uint8_t lq_data[48],
                                               void *lrn_digest_entry,
                                               uint32_t index,
                                               bool network_order);

bool pipe_mgr_entry_format_is_lrn_type_valid(bf_dev_id_t devid,
                                             profile_id_t prof_id,
                                             int lq_type);

pipe_status_t pipe_mgr_entry_format_tof_count_range_expand(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec,
    uint32_t *num_blocks_p,
    uint32_t *num_entries_per_block_p);

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
    uint32_t num_entries_per_block);

pipe_status_t pipe_mgr_ctxjson_snapshot_encode(
    bf_dev_family_t family,
    uint8_t stage_id,
    int direction,
    pipe_snap_stage_field_info_t *field_details,
    pipe_snap_trig_field_info_t *field_info,
    pipe_mgr_phv_spec_t *phv_spec,  // array of 2 (key/mask)
    pipe_mgr_phv_spec_t *phv_words_updated);

pipe_status_t pipe_mgr_ctxjson_phv_fields_dict_get(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    dev_stage_t stage,
    bf_snapshot_dir_t dir,
    pipe_snap_stage_field_info_t *dict,
    uint32_t *num_fields);
pipe_status_t pipe_mgr_ctxjson_phv_fields_dict_size_get(bf_dev_id_t devid,
                                                        profile_id_t prof_id,
                                                        dev_stage_t stage,
                                                        bf_snapshot_dir_t dir,
                                                        uint32_t *num_fields);

uint32_t pipe_mgr_ctxjson_phv_fields_dict_size(bf_dev_id_t devid,
                                               profile_id_t prof_id,
                                               bf_snapshot_dir_t dir);

bool pipe_mgr_stage_match_dependent_get(bf_dev_id_t devid,
                                        profile_id_t prof_id,
                                        dev_stage_t stage,
                                        int dir);
pipe_status_t pipe_mgr_ctxjson_snapshot_decode(
    rmt_dev_info_t *dev_info,
    profile_id_t prof_id,
    bf_dev_pipe_t pipe,
    uint8_t stage_id,
    int direction,
    pipe_mgr_snapshot_capture_data_t *pipe_capture,
    bf_snapshot_capture_ctrl_info_t *pd_ctrl,
    uint8_t *pd_capture,
    uint8_t *pd_capture_v);

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
    dev_stage_t print_stage);

pipe_status_t pipe_mgr_ctxjson_tof_snapshot_capture_field_value_get(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    dev_stage_t stage,
    int direction,
    uint8_t *pd_capture,
    uint8_t *pd_capture_v,
    char *field_name,
    uint64_t *field_value,
    bool *field_valid);

pipe_status_t pipe_mgr_ctxjson_tof_snapshot_capture_field_value_get_bytes(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    dev_stage_t stage,
    int direction,
    uint8_t *pd_capture,
    uint8_t *pd_capture_v,
    char *field_name,
    uint8_t *out_value,
    int out_len,
    int *out_width,
    bool *field_valid);

pipe_status_t pipe_mgr_ctxjson_snapshot_trig_field_set_from_pd(
    pipe_snapshot_hdl_t hdl,
    bf_dev_id_t devid,
    profile_id_t prof_id,
    int direction,
    void *trig_spec,
    void *trig_mask,
    pipe_snap_trig_field_info_t *field_info,
    int max_fields);

pipe_status_t pipe_mgr_ctxjson_tof_log_id_to_tbl_name(bf_dev_id_t devid,
                                                      profile_id_t prof_id,
                                                      int logical_tbl_id,
                                                      char *table_name);

pipe_status_t pipe_mgr_ctxjson_tof_tbl_name_to_log_id(bf_dev_id_t devid,
                                                      profile_id_t prof_id,
                                                      char *tbl_name,
                                                      uint8_t stage_id,
                                                      bool stage_valid,
                                                      uint32_t *log_id_arr,
                                                      int *num_entries,
                                                      int arr_sz);

pipe_status_t pipe_mgr_ctxjson_tof_tbl_name_to_direction(bf_dev_id_t devid,
                                                         profile_id_t prof_id,
                                                         char *tbl_name,
                                                         int *dir);

pipe_status_t pipe_mgr_entry_format_tof_dkm_tbl_keymask_encode(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    dev_stage_t stage_id,
    uint8_t stage_table_handle,
    uint8_t sub_entry,
    pipe_tbl_match_spec_t *match_spec,
    uint8_t **exm_tbl_word);

pipe_status_t pipe_mgr_tcam_compress_decoded_range_entries(
    bf_dev_id_t dev_id,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t **match_specs,
    pipe_tbl_match_spec_t *pipe_match_spec,
    uint32_t range_count);

pipe_status_t pipe_mgr_entry_format_construct_match_key_mask_spec(
    bf_dev_id_t devid,
    profile_id_t prof_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec);

uint8_t pipe_mgr_get_digest_cfg_type(rmt_dev_info_t *dev_info,
                                     profile_id_t prof_id,
                                     uint8_t lq_data[48]);
#endif
