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
 * @file pipe_mgr_alpm.h
 * @date
 *
 * ALPM related definitions of pipeline manager
 */

#ifndef _PIPE_MGR_ALPM_H
#define _PIPE_MGR_ALPM_H

#ifdef __sun
#include <illumos.h>
#endif

/* Module header files */
#include <target-utils/id/id.h>
#include <target-utils/map/map.h>
#include <target-utils/third-party/cJSON/cJSON.h>
#include <pipe_mgr/pipe_mgr_intf.h>

/* Local header file */
#include "pipe_mgr_int.h"
#include "pipe_mgr_bitmap.h"
#include "pipe_mgr_move_list.h"
#include "pipe_mgr_hitless_ha.h"

/* Allow the use in C++ code.  */
#ifdef __cplusplus
extern "C" {
#endif

#define ALPM_SESS_IS_TXN(x) (((x)->sess_flags & PIPE_MGR_TBL_API_TXN) != 0)

#define PIPE_ALPM_INVALID_ENT_HDL 0xffffffff
#define PIPE_ALPM_DEFAULT_ENT_HDL 0x1

#define ALPM_IS_SCALE_OPT_ENB(tbl_info) \
  (tbl_info->atcam_subset_key_width || tbl_info->num_excluded_bits)
/******************************************************
 *                ALPM Data Structures                *
 ******************************************************/

/* Holds logical alpm handle info (keyed by atcam handle) */
typedef struct hdl_info_s {
  pipe_mat_ent_hdl_t alpm_hdl;
  pipe_mat_ent_hdl_t cp_hdl;  // Valid if atcam entry is cp
} hdl_info_t;

/* Holds the info for a logical alpm table entry */
typedef struct alpm_entry_s {
  pipe_mat_ent_hdl_t alpm_entry_hdl;
  pipe_mat_ent_hdl_t sram_entry_hdl;
  pipe_tbl_match_spec_t *match_spec;
  pipe_act_fn_hdl_t act_fn_hdl;
  pipe_action_spec_t *act_spec;
  uint32_t ttl;
} alpm_entry_t;

struct trie_subtree_s;

/* Trie node structure */
typedef struct trie_node_s {
  uint32_t count;
  uint32_t depth;
  alpm_entry_t *entry;
  struct trie_subtree_s *subtree;
  struct trie_node_s *parent;
  struct trie_node_s *left_child;
  struct trie_node_s *right_child;

  /* Relevant when this node acts as a covering prefix */
  uint32_t cov_pfx_count;
  uint32_t cov_pfx_restore_count;
  uint32_t cov_pfx_arr_size;
  struct trie_node_s **cov_pfx_subtree_nodes;

  bool backed_up;
} trie_node_t;

struct partition_info_s;

/* Extra information for a subtree root node */
/* Also acts as a TCAM preclassifier entry structure */
typedef struct trie_subtree_s {
  pipe_mat_ent_hdl_t tcam_entry_hdl;
  trie_node_t *node;
  trie_node_t *cov_pfx;
  alpm_entry_t *cov_pfx_entry;
  struct partition_info_s *partition;
  uint8_t subtree_id;
} trie_subtree_t;

/* SRAM partition data structure */
typedef struct partition_info_s {
  uint32_t ptn_index;
  uint32_t size;
  uint32_t num_subtrees;
  trie_node_t **subtree_nodes;
  /* Subtree id allocator is used if ALPM key width optimization is used */
  bf_id_allocator *subtree_id_allocator;
  struct partition_info_s *prev;
  struct partition_info_s *next;

  bool backed_up;
} partition_info_t;

/******************************************************
 *            ALPM Backup Data Structure              *
 ******************************************************/

#define ALPM_MOD_ENTRY (1 << 0)
#define ALPM_MOD_COV_PFX (1 << 1)

/* ALPM node data backup */
typedef struct backup_node_s {
  trie_node_t *node;
  trie_node_t *backup_node;
  struct backup_node_s *next;
} backup_node_t;

/* ALPM partition data backup */
typedef struct backup_ptn_s {
  partition_info_t *backup;
  struct backup_ptn_s *next;
} backup_ptn_t;

/******************************************************
 *            ALPM Restore Data Structure             *
 ******************************************************/

typedef struct cp_restore_s {
  uint32_t ptn_idx;
  uint8_t subtree_id;
  pipe_mat_ent_hdl_t ent_hdl;
} cp_restore_t;

/******************************************************
 *              ALPM Move List Tracking               *
 ******************************************************/

typedef struct pipe_mgr_alpm_move_list_hdr_s {
  enum pipe_mat_update_type op;
  pipe_mat_tbl_hdl_t tbl_hdl;
  pipe_mgr_move_list_t *ml_head;
  pipe_mat_ent_hdl_t atcam_ent_hdl;
  pipe_mat_ent_hdl_t alpm_ent_hdl;
  bool cov_pfx;
  struct pipe_mgr_alpm_move_list_hdr_s *next;
} pipe_mgr_alpm_move_list_hdr_t;

/******************************************************
 *                ALPM Table Structures               *
 ******************************************************/

struct alpm_tbl_info_s;

/* Logical alpm pipe table structure */
typedef struct alpm_pipe_tbl_s {
  struct alpm_tbl_info_s *alpm_tbl_info;
  bf_dev_pipe_t pipe_id;
  pipe_bitmap_t pipe_bmp;
  trie_node_t *root;
  partition_info_t *partitions;
  partition_info_t *ptn_in_use_list;
  partition_info_t *ptn_free_list;
  uint32_t partitions_in_use;
  bf_id_allocator *ent_hdl_allocator;
  bf_map_t alpm_entry_hdl_map;
  backup_node_t *backup_node_list;
  backup_ptn_t *backup_ptn_list;
  partition_info_t *backup_ptn_in_use_list;
  partition_info_t *backup_ptn_free_list;
  int backup_partitions_in_use;
  bf_map_t atcam_entry_hdl_map;
  pipe_mgr_alpm_move_list_hdr_t *ml_hdr;
  pipe_mgr_alpm_move_list_hdr_t *ml_hdr_tail;

  /* Preclassifier handling info */
  pipe_tbl_match_spec_t *match_spec_template;
  pipe_action_spec_t *act_spec_template;

  /* Default Entry */
  pipe_mat_ent_hdl_t default_alpm_ent_hdl;
  pipe_mat_ent_hdl_t default_atcam_ent_hdl;
  pipe_mat_ent_hdl_t backup_default_alpm_ent_hdl;
  pipe_mat_ent_hdl_t backup_default_atcam_ent_hdl;
  bool backup_default_ent_hdl_set;

  /* HA */
  pipe_mgr_spec_map_t spec_map;
  pipe_tbl_ha_reconc_report_t ha_reconc_report;

  dev_target_t dev_tgt;
  uint32_t sess_flags;
} alpm_pipe_tbl_t;

/* Logical alpm table structure */
typedef struct alpm_tbl_info_s {
  /* General alpm table info */
  char *name;
  bf_dev_id_t dev_id;
  rmt_dev_info_t *dev_info;
  profile_id_t profile_id;
  uint32_t size;
  pipe_mat_tbl_hdl_t alpm_tbl_hdl;
  pipe_mat_tbl_hdl_t preclass_tbl_hdl;
  pipe_mat_tbl_hdl_t atcam_tbl_hdl;

  /* Pipe-specific info */
  bool is_symmetric;
  uint32_t num_pipes;
  alpm_pipe_tbl_t **pipe_tbls;
  scope_num_t num_scopes;
  scope_pipes_t *scope_pipe_bmp;

  /* Partitioning logic info */
  uint32_t num_partitions;
  uint32_t partition_depth;
  uint32_t max_subtrees_per_partition;

  /* Preclassifier handling info */
  uint16_t ptn_idx_byte_offset;
  uint16_t num_ptn_idx_bits;
  pipe_act_fn_hdl_t *act_fn_hdls;
  uint8_t num_act_fn_hdls;

  /* Atcam handling info */
  uint32_t num_fields;
  alpm_field_info_t *field_info;
  uint32_t trie_depth;
  uint32_t num_excluded_bits;
  uint32_t atcam_subset_key_width;
  uint32_t lpm_field_key_width;
  uint32_t exm_fields_key_width;
  uint32_t lpm_field_byte_offset;
  uint32_t shift_granularity;

  /* Covering prefix restore info for hitless in scale-opt case */
  pipe_mgr_move_list_t *cp_ml_head;
  pipe_mgr_move_list_t *cp_ml_tail;
  bool is_cp_restore;
  /* Version is used in ATCAM table in order to mark covering
   * prefix entries for restore. In scale optimization it is not
   * possible to tell if entry is an actual entry or CP otherwise.
   */
  uint8_t cp_ver_bits;
} alpm_tbl_info_t;

typedef struct alpm_mgr_ctx_s {
  bf_map_t tbl_hdl_to_tbl_map[PIPE_MGR_NUM_DEVICES];
} alpm_mgr_ctx_t;

/******************************************************
 *        APIs to be used by session manager          *
 ******************************************************/

/*
 * Usage: pipe_mgr_alpm_init()
 * ---------------------------
 * Initializes the global data for alpm management.
 */
pipe_status_t pipe_mgr_alpm_init(void);

/*
 * Usage: pipe_mgr_alpm_tbl_create(device_id, mat_tbl_hdl, profile_id)
 * -------------------------------------------------------------------
 * Creates and populates a logical alpm table structure.
 */
pipe_status_t pipe_mgr_alpm_tbl_create(bf_dev_id_t dev_id,
                                       pipe_mat_tbl_hdl_t tbl_hdl,
                                       profile_id_t profile_id,
                                       pipe_bitmap_t *pipe_bmp);

/*
 * Usage: pipe_mgr_alpm_tbl_delete(device_id, mat_tbl_hdl)
 * -------------------------------------------------------
 * Deletes the alpm table data structure associated with the given handle.
 */
pipe_status_t pipe_mgr_alpm_tbl_delete(bf_dev_id_t dev_id,
                                       pipe_mat_tbl_hdl_t tbl_hdl);

alpm_tbl_info_t *pipe_mgr_alpm_tbl_info_get(bf_dev_id_t dev_id,
                                            pipe_mat_tbl_hdl_t tbl_hdl);

alpm_tbl_info_t *pipe_mgr_alpm_tbl_info_get_from_ll_hdl(
    bf_dev_id_t dev_id, pipe_mat_tbl_hdl_t ll_tbl_hdl);

pipe_mat_ent_hdl_t pipe_mgr_alpm_allocate_handle(alpm_pipe_tbl_t *pipe_tbl);

/*
 * Usage: pipe_mgr_alpm_entry_place(...)
 * -------------------------------------
 * Adds the given entry to the alpm table:
 *   -Adds the entry to the trie
 *   -Change the subtree structure if necessary
 *   -Updates the preclassifier table if necessary
 *   -Adds the entry to the correct SRAM partition
 */
pipe_status_t pipe_mgr_alpm_entry_place(dev_target_t dev_tgt,
                                        pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                        pipe_tbl_match_spec_t *match_spec,
                                        pipe_act_fn_hdl_t act_fn_hdl,
                                        pipe_action_spec_t *act_data_spec,
                                        uint32_t ttl,
                                        u_int32_t pipe_api_flags,
                                        pipe_mat_ent_hdl_t *ent_hdl_p,
                                        pipe_mgr_move_list_t **move_head_p);

pipe_status_t pipe_mgr_alpm_entry_place_with_hdl(
    dev_target_t dev_tgt,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec,
    pipe_act_fn_hdl_t act_fn_hdl,
    pipe_action_spec_t *act_spec,
    uint32_t ttl,
    uint32_t pipe_api_flags,
    pipe_mat_ent_hdl_t ent_hdl,
    pipe_mgr_move_list_t **move_head_p);

/*
 * Usage: pipe_mgr_alpm_entry_del(...)
 * -----------------------------------
 * Removes the entry from the alpm table:
 *   -Removes the entry from the underlying SRAM partition
 *   -Marks the trie node as invalid
 *   -Remove the subtree from the trie if empty
 *     -Remove the corresponding preclassifier entry
 *   -Free the partition info structure if empty
 */
pipe_status_t pipe_mgr_alpm_entry_del(bf_dev_id_t dev_id,
                                      pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                      pipe_mat_ent_hdl_t mat_ent_hdl,
                                      u_int32_t pipe_api_flags,
                                      pipe_mgr_move_list_t **move_head_p);

/******************************************************
 *       Wrappers for underlying ATCAM table          *
 ******************************************************/

/*
 * Usage: pipe_mgr_alpm_default_ent_place(...)
 * -------------------------------------------
 * Adds the given default action to the alpm table.
 */
pipe_status_t pipe_mgr_alpm_default_ent_place(
    dev_target_t dev_tgt,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_act_fn_hdl_t act_fn_hdl,
    pipe_action_spec_t *act_spec,
    uint32_t pipe_api_flags,
    pipe_mat_ent_hdl_t *ent_hdl_p,
    pipe_mgr_move_list_t **move_head_p);

/*
 * Usage: pipe_mgr_alpm_default_ent_hdl_get(...)
 * -------------------------------------------
 * Get the default entry handle
 */
pipe_status_t pipe_mgr_alpm_default_ent_hdl_get(dev_target_t dev_tgt,
                                                pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                                pipe_mat_ent_hdl_t *ent_hdl_p);

/*
 * Usage: pipe_mgr_alpm_cleanup_default_entry(...)
 * -----------------------------------------------
 * Removes the given default action from the alpm table.
 */
pipe_status_t pipe_mgr_alpm_cleanup_default_entry(
    dev_target_t dev_tgt,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint32_t pipe_api_flags,
    pipe_mgr_move_list_t **move_head_p);

/*
 * Usage: pipe_mgr_alpm_ent_set_action(...)
 * ----------------------------------------
 * Modifies the alpm entry corresponding to the given handle with the
 * given action spec.
 */
pipe_status_t pipe_mgr_alpm_ent_set_action(bf_dev_id_t device_id,
                                           pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                           pipe_mat_ent_hdl_t mat_ent_hdl,
                                           pipe_act_fn_hdl_t act_fn_hdl,
                                           pipe_action_spec_t *act_spec,
                                           uint32_t pipe_api_flags,
                                           pipe_mgr_move_list_t **move_head_p);

/*
 * Usage: pipe_mgr_alpm_ent_set_resource(...)
 * ------------------------------------------
 * Modifies the alpm entry corresponding to the given handle with the
 * given resources.
 */
pipe_status_t pipe_mgr_alpm_ent_set_resource(
    bf_dev_id_t device_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    pipe_res_spec_t *resources,
    int resource_count,
    uint32_t pipe_api_flags,
    pipe_mgr_move_list_t **move_head_p);

/*
 * Usage: pipe_mgr_alpm_get_programmed_entry_count(dev_tgt, mat_tbl_hdl, &count)
 * -----------------------------------------------------------------------------
 * Returns the number of valid entries in the alpm table corresponding
 * to the given table handle.
 */
pipe_status_t pipe_mgr_alpm_get_programmed_entry_count(
    dev_target_t dev_tgt, pipe_mat_tbl_hdl_t tbl_hdl, uint32_t *count_p);

/*
 * Usage: pipe_mgr_alpm_get_first_entry_handle(tbl_hdl, dev_tgt, &entry_hdl)
 * -------------------------------------------------------------------------
 * Retrieves the first entry handle for the given alpm table.
 */
pipe_status_t pipe_mgr_alpm_get_first_entry_handle(pipe_mat_tbl_hdl_t tbl_hdl,
                                                   dev_target_t dev_tgt,
                                                   int *entry_hdl);

/*
 * Usage: pipe_mgr_alpm_get_next_entry_handles(...)
 * ------------------------------------------------
 * Retrieves the next n entry handles for the given alpm table. If there are
 * less than n entry handles left, the end is marked with -1.
 */
pipe_status_t pipe_mgr_alpm_get_next_entry_handles(pipe_mat_tbl_hdl_t tbl_hdl,
                                                   dev_target_t dev_tgt,
                                                   pipe_mat_ent_hdl_t entry_hdl,
                                                   int n,
                                                   int *next_entry_handles);

pipe_status_t pipe_mgr_alpm_get_default_entry_handles(
    pipe_mat_tbl_hdl_t tbl_hdl,
    bf_dev_id_t dev_id,
    pipe_mat_ent_hdl_t *default_hdls,
    uint32_t *num_def_hdls);

pipe_status_t pipe_mgr_alpm_default_ent_get(pipe_sess_hdl_t sess_hdl,
                                            dev_target_t dev_tgt,
                                            pipe_mat_tbl_hdl_t tbl_hdl,
                                            pipe_action_spec_t *action_spec,
                                            pipe_act_fn_hdl_t *act_fn_hdl,
                                            bool from_hw);

/*
 * Usage: pipe_mgr_alpm_get_entry(...)
 * -----------------------------------
 * Retrieves the information corresponding to the given entry.
 */
pipe_status_t pipe_mgr_alpm_get_entry(pipe_mat_tbl_hdl_t tbl_hdl,
                                      dev_target_t dev_tgt,
                                      pipe_mat_ent_hdl_t entry_hdl,
                                      pipe_tbl_match_spec_t *pipe_match_spec,
                                      pipe_action_spec_t *pipe_action_spec,
                                      pipe_act_fn_hdl_t *act_fn_hdl,
                                      bool from_hw);

/*
 * Usage: pipe_mgr_alpm_entry_get_location(...)
 * --------------------------------------------
 * Retrives location information for the given entry.
 */
pipe_status_t pipe_mgr_alpm_entry_get_location(bf_dev_id_t dev_id,
                                               pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                               pipe_mat_ent_hdl_t mat_ent_hdl,
                                               uint32_t subindex,
                                               bf_dev_pipe_t *pipe_id_p,
                                               uint8_t *stage_id_p,
                                               rmt_tbl_hdl_t *stage_table_hdl_p,
                                               uint32_t *index_p);

trie_node_t *find_node(alpm_pipe_tbl_t *pipe_tbl,
                       pipe_tbl_match_spec_t *match_spec,
                       bool is_cp_restore,
                       trie_subtree_t **subtree,
                       pipe_mgr_move_list_t **move_head_p,
                       pipe_mgr_move_list_t **move_tail_p);

pipe_status_t pipe_mgr_alpm_entry_hdl_from_stage_idx(
    bf_dev_id_t dev_id,
    bf_dev_pipe_t pipe,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint8_t stage,
    uint8_t logical_tbl_id,
    uint32_t hit_addr,
    pipe_mat_ent_hdl_t *entry_hdl);

/*
 * Usage: pipe_mgr_alpm_tbl_set_symmetric_mode(...)
 * ------------------------------------------------
 * Sets the symmetric mode of the ALPM table according to the boolean flag.
 * This is only allowed when the table is empty.
 */
pipe_status_t pipe_mgr_alpm_tbl_set_symmetric_mode(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t dev_id,
    pipe_mat_tbl_hdl_t tbl_hdl,
    bool symmetric,
    scope_num_t num_scopes,
    scope_pipes_t *scope_pipe_bmp);

/*
 * Usage: pipe_mgr_alpm_tbl_get_symmetric_mode(...)
 * ------------------------------------------------
 * Gets the symmetric mode of the ALPM table
 */
pipe_status_t pipe_mgr_alpm_tbl_get_symmetric_mode(
    bf_dev_id_t dev_id,
    pipe_mat_tbl_hdl_t tbl_hdl,
    bool *symmetric,
    scope_num_t *num_scopes,
    scope_pipes_t *scope_pipe_bmp);

/*
 * Usage: pipe_mgr_alpm_process_move_list(...)
 * -------------------------------------------
 * Process the hardware move list built by the hlp entry placement functions.
 */
pipe_status_t pipe_mgr_alpm_process_move_list(pipe_sess_hdl_t sess_hdl,
                                              bf_dev_id_t dev_id,
                                              pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                              pipe_mgr_move_list_t *move_list,
                                              uint32_t *success_count);

/*
 * Usage: pipe_mgr_alpm_abort(device_id, alpm_tbl_hdl)
 * ---------------------------------------------------
 * Aborts the current transaction and restores the state from our backups.
 */
pipe_status_t pipe_mgr_alpm_abort(bf_dev_id_t dev_id,
                                  pipe_mat_tbl_hdl_t tbl_hdl,
                                  bf_dev_pipe_t *pipes_list,
                                  unsigned nb_pipes);

/*
 * Usage: pipe_mgr_alpm_commit(device_id, alpm_tbl_hdl)
 * ----------------------------------------------------
 * Locks in the current transaction and discards all backup states.
 */
pipe_status_t pipe_mgr_alpm_commit(bf_dev_id_t dev_id,
                                   pipe_mat_tbl_hdl_t tbl_hdl,
                                   bf_dev_pipe_t *pipes_list,
                                   unsigned nb_pipes);

/*
 * Usage: pipe_mgr_alpm_atom_cleanup(device_id, alpm_tbl_hdl)
 * ----------------------------------------------------------
 * Locks in the current atomic transaction and discards all backup states.
 * The actual atomic actions will be performed by the corresponding tcam
 * functions.
 */
pipe_status_t pipe_mgr_alpm_atom_cleanup(bf_dev_id_t dev_id,
                                         pipe_mat_tbl_hdl_t tbl_hdl,
                                         bf_dev_pipe_t *pipes_list,
                                         unsigned nb_pipes);

pipe_status_t pipe_mgr_set_alpm_tbl_match_act_info(
    alpm_tbl_info_t *tbl_info,
    alpm_pipe_tbl_t *pipe_tbl,
    pipe_tbl_match_spec_t *match_spec,
    pipe_action_spec_t *act_spec);

void pipe_mgr_alpm_print_match_spec(bf_dev_id_t device_id,
                                    pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                    pipe_tbl_match_spec_t *match_spec,
                                    char *buf,
                                    size_t buf_len);

pipe_status_t pipe_mgr_alpm_get_match_spec(pipe_mat_ent_hdl_t mat_ent_hdl,
                                           pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                           bf_dev_id_t device_id,
                                           bf_dev_pipe_t *pipe_id,
                                           pipe_tbl_match_spec_t **match_spec);

pipe_status_t pipe_mgr_alpm_get_full_match_spec(
    pipe_mat_ent_hdl_t mat_ent_hdl,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    bf_dev_id_t device_id,
    bf_dev_pipe_t *pipe_id,
    pipe_tbl_match_spec_t **match_spec);

pipe_status_t pipe_mgr_alpm_ent_hdl_atcam_to_alpm(bf_dev_id_t device_id,
                                                  pipe_mat_tbl_hdl_t tbl_hdl,
                                                  bf_dev_pipe_t pipe,
                                                  pipe_mat_ent_hdl_t *ent_hdl,
                                                  bool from_user);

pipe_status_t pipe_mgr_alpm_log_state(bf_dev_id_t dev_id,
                                      pipe_mat_tbl_info_t *mat_info,
                                      cJSON *match_tbls);

pipe_status_t pipe_mgr_alpm_restore_state(bf_dev_id_t dev_id,
                                          pipe_mat_tbl_info_t *mat_info,
                                          cJSON *alpm_tbl);

pipe_status_t pipe_mgr_alpm_preclass_restore_int(
    alpm_tbl_info_t *tbl_info,
    alpm_pipe_tbl_t *pipe_tbl,
    pipe_mat_ent_hdl_t ll_ent_hdl,
    pipe_tbl_match_spec_t *match_spec,
    pipe_action_spec_t *act_spec);

pipe_status_t pipe_mgr_alpm_atcam_restore_int(alpm_tbl_info_t *tbl_info,
                                              alpm_pipe_tbl_t *pipe_tbl,
                                              pipe_mat_ent_hdl_t ll_ent_hdl,
                                              trie_node_t *node,
                                              partition_info_t *p_info,
                                              uint8_t subtree_id,
                                              pipe_tbl_match_spec_t *match_spec,
                                              pipe_act_fn_hdl_t act_fn_hdl,
                                              pipe_action_spec_t *act_spec,
                                              bool is_cp);

void pipe_mgr_alpm_restore_cp(alpm_tbl_info_t *tbl_info);

typedef pipe_status_t (*alpm_restore_callback_fn)(
    dev_target_t dev_tgt,
    pipe_mat_tbl_info_t *mat_info,
    pipe_mat_tbl_hdl_t ll_tbl_hdl,
    pipe_mat_ent_hdl_t ll_ent_hdl,
    pipe_tbl_match_spec_t *match_spec,
    pipe_act_fn_hdl_t act_fn_hdl,
    pipe_action_spec_t *act_spec);

pipe_status_t pipe_mgr_alpm_get_inactive_node_delete(bool *enable);
pipe_status_t pipe_mgr_alpm_set_inactive_node_delete(bool enable);

void build_alpm_full_mspec(alpm_tbl_info_t *tbl_info,
                           pipe_tbl_match_spec_t *entry_mspec,
                           pipe_tbl_match_spec_t *atcam_mspec,
                           uint32_t depth);
void build_alpm_exclude_msb_bits_full_mspec(alpm_tbl_info_t *tbl_info,
                                            pipe_tbl_match_spec_t *entry_mspec,
                                            pipe_tbl_match_spec_t *atcam_mspec);
#ifdef __cplusplus
}
#endif

#endif /* _PIPE_MGR_ALPM_H */
