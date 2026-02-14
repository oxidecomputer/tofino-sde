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
 * @file pipe_mgr_intf.h
 * @date
 *
 * Definitions for interfaces to pipeline manager
 */

#ifndef _PIPE_MGR_INTF_H
#define _PIPE_MGR_INTF_H

#ifndef __KERNEL__
/* Standard includes */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#endif

/* Module header files */
#include <bf_types/bf_types.h>
#include <pipe_mgr/pipe_mgr_err.h>

/* Allow the use in C++ code.  */
#ifdef __cplusplus
extern "C" {
#endif

#include <bfutils/dynamic_hash/bfn_hash_algorithm.h>

typedef enum pipe_hdl_type {
  /* API session handles */
  PIPE_HDL_TYPE_SESSION = 0x0,

  /* Handles for logical table objects */
  PIPE_HDL_TYPE_MAT_TBL = 0x1,   /* Match action table */
  PIPE_HDL_TYPE_ADT_TBL = 0x2,   /* Action data table */
  PIPE_HDL_TYPE_SEL_TBL = 0x3,   /* Selection table */
  PIPE_HDL_TYPE_STAT_TBL = 0x4,  /* Statistics table */
  PIPE_HDL_TYPE_METER_TBL = 0x5, /* Meter, LPF and WRED table */
  PIPE_HDL_TYPE_STFUL_TBL = 0x6, /* Stateful table */
  PIPE_HDL_TYPE_COND_TBL = 0x7,  /* Gateway table */

  /* Handles for other P4 level objects */
  PIPE_HDL_TYPE_CALC_ALGO = 0x8, /* P4 calculation algorithm */

  /* Reserve one type for invalid type */
  PIPE_HDL_TYPE_INVALID = 0xF
} pipe_hdl_type_t;

#define PIPE_SET_HDL_TYPE(hdl, type) (((type) << 24) | (hdl))
#define PIPE_GET_HDL_TYPE(hdl) (((hdl) >> 24) & 0xF)
#define PIPE_SET_HDL_PIPE(hdl, pipe) (((pipe) << 28) | (hdl))
#define PIPE_GET_HDL_PIPE(hdl) (((hdl) >> 28) & 0xF)
#define PIPE_GET_HDL_VAL(hdl) ((hdl)&0x00FFFFFF)

typedef enum pipe_mgr_tbl_prop_type_ {
  PIPE_MGR_TABLE_PROP_NONE = 0,
  PIPE_MGR_TABLE_ENTRY_SCOPE,
  PIPE_MGR_TERN_TABLE_ENTRY_PLACEMENT,
  PIPE_MGR_DUPLICATE_ENTRY_CHECK,
  PIPE_MGR_IDLETIME_REPEATED_NOTIFICATION,
} pipe_mgr_tbl_prop_type_t;

typedef enum pipe_mgr_tbl_prop_scope_value {
  PIPE_MGR_ENTRY_SCOPE_ALL_PIPELINES = 0,
  PIPE_MGR_ENTRY_SCOPE_SINGLE_PIPELINE = 1,
  PIPE_MGR_ENTRY_SCOPE_USER_DEFINED = 2,
} pipe_mgr_tbl_prop_scope_value_t;

typedef enum pipe_mgr_tbl_prop_tern_placement_value {
  PIPE_MGR_TERN_ENTRY_PLACEMENT_DRV_MANAGED = 0,
  PIPE_MGR_TERN_ENTRY_PLACEMENT_APP_MANAGED = 1,
} pipe_mgr_tbl_prop_tern_placement_value_t;

typedef enum pipe_mgr_tbl_prop_duplicate_entry_check_value {
  PIPE_MGR_DUPLICATE_ENTRY_CHECK_DISABLE = 0,
  PIPE_MGR_DUPLICATE_ENTRY_CHECK_ENABLE = 1,
} pipe_mgr_tbl_prop_duplicate_entry_check_value_t;

typedef enum pipe_mgr_tbl_prop_idletime_repeated_notification_enable_value {
  PIPE_MGR_IDLETIME_REPEATED_NOTIFICATION_DISABLE = 0,
  PIPE_MGR_IDLETIME_REPEATED_NOTIFICATION_ENABLE = 1,
} pipe_mgr_tbl_prop_idletime_repeated_notification_enable_value_t;

typedef union pipe_mgr_tbl_prop_value {
  uint32_t value;
  pipe_mgr_tbl_prop_scope_value_t scope;
  pipe_mgr_tbl_prop_tern_placement_value_t tern_placement;
  pipe_mgr_tbl_prop_duplicate_entry_check_value_t duplicate_check;
  pipe_mgr_tbl_prop_idletime_repeated_notification_enable_value_t
      repeated_notify;
} pipe_mgr_tbl_prop_value_t;

#define PIPE_MGR_MAX_USER_DEFINED_SCOPES 8
typedef uint8_t scope_pipes_t;
typedef union pipe_mgr_tbl_prop_args {
  uint64_t value;
  scope_pipes_t user_defined_entry_scope[PIPE_MGR_MAX_USER_DEFINED_SCOPES];
} pipe_mgr_tbl_prop_args_t;

typedef enum {
  /* keep enums in sync with p4_pd_pvs_parser_scope_en */
  PIPE_MGR_PVS_SCOPE_ALL_PARSERS_IN_PIPE = 0,
  PIPE_MGR_PVS_SCOPE_SINGLE_PARSER = 1
} pipe_mgr_pvs_parser_scope_en;

typedef enum {
  /* keep enums in sync with p4_pd_pvs_gress_scope_en */
  PIPE_MGR_PVS_SCOPE_ALL_GRESS = 0,
  PIPE_MGR_PVS_SCOPE_SINGLE_GRESS = 1
} pipe_mgr_pvs_gress_scope_en;

typedef enum {
  /* keep enums in sync with p4_pd_pvs_pipe_scope_en */
  PIPE_MGR_PVS_SCOPE_ALL_PIPELINES = 0,
  PIPE_MGR_PVS_SCOPE_SINGLE_PIPELINE = 1,
  PIPE_MGR_PVS_SCOPE_USER_DEFINED = 2
} pipe_mgr_pvs_pipe_scope_en;

typedef enum pipe_mgr_pvs_prop_type_ {
  /* keep enum in sync with p4_pd_pvs_prop_type_t */
  PIPE_MGR_PVS_PROP_NONE = 0,
  PIPE_MGR_PVS_GRESS_SCOPE,
  PIPE_MGR_PVS_PIPE_SCOPE,
  PIPE_MGR_PVS_PARSER_SCOPE
} pipe_mgr_pvs_prop_type_t;

typedef union pipe_mgr_pvs_prop_value {
  /* keep enum in sync with p4_pd_pvs_prop_value_t */
  uint64_t value;
  pipe_mgr_pvs_gress_scope_en gress_scope;
  pipe_mgr_pvs_pipe_scope_en pipe_scope;
  pipe_mgr_pvs_parser_scope_en parser_scope;
} pipe_mgr_pvs_prop_value_t;

typedef struct pipe_mgr_pvs_prop_user_defined_args_ {
  bf_dev_direction_t gress;
  scope_pipes_t user_defined_scope[PIPE_MGR_MAX_USER_DEFINED_SCOPES];
} pipe_mgr_pvs_prop_user_defined_args_t;

typedef union pipe_mgr_pvs_prop_args {
  /* keep enum in sync with p4_pd_pvs_prop_args_t */
  uint64_t value;
  bf_dev_direction_t gress;
  pipe_mgr_pvs_prop_user_defined_args_t user_defined;
} pipe_mgr_pvs_prop_args_t;

// Wildcard when you want to specify all parsers
// keep in sync with PD_DEV_PIPE_PARSER_ALL
#define PIPE_MGR_PVS_PARSER_ALL 0xff

#define PIPE_DIR_MAX 2

/*!
 * Typedefs for pipeline object handles
 */
typedef uint32_t pipe_sess_hdl_t;

typedef uint32_t pipe_tbl_hdl_t;
typedef pipe_tbl_hdl_t pipe_mat_tbl_hdl_t;
typedef pipe_tbl_hdl_t pipe_adt_tbl_hdl_t;
typedef pipe_tbl_hdl_t pipe_sel_tbl_hdl_t;
typedef pipe_tbl_hdl_t pipe_stat_tbl_hdl_t;
typedef pipe_tbl_hdl_t pipe_meter_tbl_hdl_t;
typedef pipe_tbl_hdl_t pipe_lpf_tbl_hdl_t;
typedef pipe_tbl_hdl_t pipe_wred_tbl_hdl_t;
typedef pipe_tbl_hdl_t pipe_stful_tbl_hdl_t;
typedef pipe_tbl_hdl_t pipe_ind_res_hdl_t;
typedef pipe_tbl_hdl_t pipe_res_hdl_t;
typedef pipe_tbl_hdl_t pipe_prsr_instance_hdl_t;

typedef uint32_t pipe_ent_hdl_t;
typedef pipe_ent_hdl_t pipe_mat_ent_hdl_t;
typedef pipe_ent_hdl_t pipe_adt_ent_hdl_t;
typedef pipe_ent_hdl_t pipe_sel_grp_hdl_t;
typedef pipe_ent_hdl_t pipe_stat_ent_idx_t;
typedef pipe_ent_hdl_t pipe_meter_idx_t;
typedef pipe_ent_hdl_t pipe_lpf_idx_t;
typedef pipe_ent_hdl_t pipe_wred_idx_t;
typedef pipe_ent_hdl_t pipe_stful_mem_idx_t;
typedef pipe_ent_hdl_t pipe_ind_res_idx_t;
typedef pipe_ent_hdl_t pipe_res_idx_t;
typedef pipe_ent_hdl_t pipe_pvs_hdl_t;

typedef uint32_t pipe_act_fn_hdl_t;
typedef uint32_t pipe_adt_mbr_id_t;
typedef uint32_t pipe_sel_grp_id_t;
typedef uint32_t pipe_fld_lst_hdl_t;
typedef uint32_t pipe_hash_alg_hdl_t;
typedef uint32_t pipe_snapshot_hdl_t;
typedef uint32_t pipe_reg_param_hdl_t;
typedef uint8_t dev_stage_t;
typedef int profile_id_t;
typedef uint32_t pipe_idx_t;
typedef uint8_t pipe_parser_id_t;
typedef uint32_t pipe_hash_calc_hdl_t;

/* Seed value for a hash calculation */
typedef uint64_t pipe_hash_seed_t;

#define TOF_NUM_PARSERS 18
#define TOF2_NUM_PARSERS 36
#define TOF3_NUM_PARSERS 36

// multi parser instance
typedef struct pipe_mgr_tof_prsr_instance_hdl_map_ {
  pipe_prsr_instance_hdl_t prsr_instance_hdl[PIPE_DIR_MAX][TOF_NUM_PARSERS];
} pipe_mgr_tof_prsr_instance_hdl_map_t;

typedef struct pipe_mgr_tof2_prsr_instance_hdl_map_ {
  pipe_prsr_instance_hdl_t prsr_instance_hdl[PIPE_DIR_MAX][TOF2_NUM_PARSERS];
} pipe_mgr_tof2_prsr_instance_hdl_map_t;

typedef struct pipe_mgr_tof3_prsr_instance_hdl_map_ {
  pipe_prsr_instance_hdl_t prsr_instance_hdl[PIPE_DIR_MAX][TOF3_NUM_PARSERS];
} pipe_mgr_tof3_prsr_instance_hdl_map_t;

union pipe_mgr_prsr_instance_hdl_map_t {
  pipe_mgr_tof_prsr_instance_hdl_map_t tof;
  pipe_mgr_tof2_prsr_instance_hdl_map_t tof2;
  pipe_mgr_tof2_prsr_instance_hdl_map_t tof3;
};

/* Structure definition for a hash value computed by the device */
typedef struct pipe_exm_hash_ {
  uint32_t num_bits; /* Number of bits of hash */
  uint64_t hash_value;

} pipe_exm_hash_t;

typedef void (*pipe_mgr_stat_ent_sync_cback_fn)(bf_dev_id_t device_id,
                                                void *cookie);
typedef void (*pipe_mgr_stat_tbl_sync_cback_fn)(bf_dev_id_t device_id,
                                                void *cookie);
typedef void (*pipe_stful_ent_sync_cback_fn)(bf_dev_id_t device_id,
                                             void *cookie);
typedef void (*pipe_stful_tbl_sync_cback_fn)(bf_dev_id_t device_id,
                                             void *cookie);
/*!
 * Flags that can be used in conjunction with API calls
 */
/*! Flag to make hardware synchronous API requests */
#define PIPE_FLAG_SYNC_REQ (1 << 0)

/* Not to be used */
#define PIPE_FLAG_INTERNAL (1 << 1)

/*!
 * Flag to enable handling of mbr_id and grp_id for adt and selector tables.
 * Without setting this flag arguments will be ignored. Should be set on
 * the first *_ent_add() function call on a specific table.
 * Cannot be turned off after turning on.
 */
#define PIPE_FLAG_CACHE_ENT_ID (1 << 2)

/*!
 * Flag will cause delete entry calls to ignore NOT_FOUND error in TXN handling.
 * Doesn't apply to all table types.
 */
#define PIPE_FLAG_IGNORE_NOT_FOUND (1 << 3)

/*!
 * Data types used with this library
 */

/*! Definitions used to identify the target of an API request */
#define DEV_PIPE_ALL BF_DEV_PIPE_ALL
typedef bf_dev_target_t dev_target_t;

/*!
 * Generalized match specification for any lookup table entry
 */
typedef struct pipe_tbl_match_spec {
  uint8_t *match_value_bits; /*!< Value of match bits */
  uint8_t *match_mask_bits;
  /*!< Mask for matching. Valid only for ternary tables */
  uint32_t partition_index; /*!< The partition index for this entry */
  uint32_t priority;
  /*!< Priority dictates the position of a ternary table
   * entry in relation to other entries in the table
   */
  uint16_t num_valid_match_bits; /*!< Number of match bits valid */
  uint16_t num_match_bytes;      /*!< Size of padded match_value_bits */
  uint8_t version_bits;          /*!< Entry version bits - internal only */
} pipe_tbl_match_spec_t;

typedef struct pipe_tbl_ha_reconc_report {
  /*!< Number of entries that were added after delta compute */
  uint32_t num_entries_added;
  /*!< Number of entries that were deleted after delta compute */
  uint32_t num_entries_deleted;
  /*!< Number of entries that were modified after delta compute */
  uint32_t num_entries_modified;
} pipe_tbl_ha_reconc_report_t;

/* Profile of number groups sizes of a certain size in a selection table */
typedef struct pipe_sel_grp_profile {
  uint16_t grp_size; /* Max members in a grp <2..4K> */
  uint16_t num_grps; /* Number of groups of this <grp_size> */
} pipe_sel_grp_profile_t;

typedef struct pipe_sel_tbl_profile {
  uint16_t num_grp_profiles;
  pipe_sel_grp_profile_t *grp_profile_list; /* array */
} pipe_sel_tbl_profile_t;

/*! Flow learn notification entry format. This is interpreted
 * in terms of P4 fields by entry decoder routines
 */

/*! Flow learn notification message format
 */
typedef struct pipe_lrn_digest_msg {
  dev_target_t dev_tgt;
  /*< Device that is the originator of the learn notification */
  uint16_t num_entries;
  /*< number of learn notifications in this message */
  void *entries;
  /*< array of <num_entries> of
    pd_<program_name>_<lrn_digest_field_list_name>_digest_entry_t;*/
  pipe_fld_lst_hdl_t flow_lrn_fld_lst_hdl;
} pipe_flow_lrn_msg_t;

/* Prototype for flow learn notification handler */
typedef pipe_status_t (*pipe_flow_lrn_notify_cb)(
    pipe_sess_hdl_t sess_hdl,
    pipe_flow_lrn_msg_t *pipe_flow_lrn_msg,
    void *callback_fn_cookie);

/*!
 * Enum to define meter rate unit types
 */
typedef enum pipe_meter_type {
  METER_TYPE_COLOR_AWARE,   /*< Color aware meter */
  METER_TYPE_COLOR_UNAWARE, /*< Color unaware meter */
} pipe_meter_type_e;

typedef enum pipe_meter_rate_type {
  METER_RATE_TYPE_KBPS, /*< Type to measure rate in kilobits per sec */
  METER_RATE_TYPE_PPS,  /*< Type to measure rate in pkts per sec */
} pipe_meter_rate_type_e;

/*!
 * Structure for meter specification
 */
typedef struct pipe_meter_rate {
  pipe_meter_rate_type_e type; /*< Type of rate specified */
  union {
    uint64_t kbps; /*< Rate in units of kilobits per second  */
    uint64_t pps;  /*< Rate units of pkts per second */
  } value;
} pipe_meter_rate_t;

/*! Structure for meter specification
 */
typedef struct pipe_meter_spec {
  pipe_meter_type_e meter_type;
  /*< Meter type */
  pipe_meter_rate_t cir;
  /*< Meter committed information rate */
  uint64_t cburst;
  /*< Meter committed burst size */
  pipe_meter_rate_t pir;
  /*< Meter peak information rate */
  uint64_t pburst;
  /*< Meter peak burst size */
} pipe_meter_spec_t;

/*!
 * Enum to define lpf type
 */
typedef enum pipe_lpf_type_ {
  LPF_TYPE_RATE,   /*< Rate LPF */
  LPF_TYPE_SAMPLE, /*< Sample LPF */
} pipe_lpf_type_e;

/*! Structure for a LPF specification
 */
typedef struct pipe_lpf_spec {
  pipe_lpf_type_e lpf_type;
  /*< Enum indicating the type of lpf */
  bool gain_decay_separate_time_constant;
  /*< A flag indicating if a separate rise/fall time constant is desired */
  float gain_time_constant;
  /*< Rise time constant, in nanoseconds, valid only if the above flag is set */
  float decay_time_constant;
  /*< Fall time constant, in nanoseconds valid only if
   * rise_fall_separate_time_constant
   *  flag is set
   */
  float time_constant;
  /*< A common time constant, in nanoseconds valid only if the
   *  rise_fall_separate_time_constant is not set
   */
  uint32_t output_scale_down_factor;
  /*< An integer indicating the scale down factor, right-shifted by these
   *  many bits. Values range from 0 to 31
   */

} pipe_lpf_spec_t;

/*! Structure for a WRED specification
 */
typedef struct pipe_wred_spec {
  float time_constant;
  /*< Time constant, in nanoseconds*/
  uint32_t red_min_threshold;
  /*< Queue threshold above which the probabilistic dropping starts in units
   *  of packet buffer cells
   */
  uint32_t red_max_threshold;
  /*< Queue threshold above which all packets are dropped in units cells*/
  float max_probability;
  /*< Maximum probability desired for marking the packet, with range from 0.0 to
   * 1.0 */

} pipe_wred_spec_t;

/*! Idle Timers
 */
/*!
 * Enum for Idle timer hit state
 */
typedef enum pipe_idle_time_hit_state_ {
  ENTRY_IDLE,
  ENTRY_ACTIVE
} pipe_idle_time_hit_state_e;

/* Prototype for idle timer expiry notification handler */
typedef void (*pipe_idle_tmo_expiry_cb)(bf_dev_id_t dev_id,
                                        pipe_mat_ent_hdl_t mat_ent_hdl,
                                        pipe_idle_time_hit_state_e hs,
                                        void *client_data);

/* Second Prototype for idle timer expiry notification handler */
typedef void (*pipe_idle_tmo_expiry_cb_with_match_spec_copy)(
    bf_dev_id_t dev_id,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    pipe_idle_time_hit_state_e hs,
    pipe_tbl_match_spec_t *match_spec,
    void *client_data);

/* Prototype for idle time update complete callback handler */
typedef void (*pipe_idle_tmo_update_complete_cb)(bf_dev_id_t dev_id,
                                                 void *cb_data);

typedef enum pipe_idle_time_mode_ {
  POLL_MODE = 0,
  NOTIFY_MODE = 1,
  INVALID_MODE = 2
} pipe_idle_time_mode_e;

static inline const char *idle_time_mode_to_str(pipe_idle_time_mode_e mode) {
  switch (mode) {
    case POLL_MODE:
      return "poll";
    case NOTIFY_MODE:
      return "notify";
    case INVALID_MODE:
      return "disabled";
  }
  return "unknown";
}

typedef struct pipe_idle_time_params_ {
  /* Default mode is POLL_MODE */
  pipe_idle_time_mode_e mode;
  union {
    struct {
      uint32_t ttl_query_interval;
      /*< Minimum query interval with which the application will call
       *  pipe_mgr_idle_time_get_ttl to get the TTL of an entry.
       * If the API is called sooner than the query interval, then
       * the value received will be same
       */
      uint32_t max_ttl;
      /*< deprecated, will be ignored */
      uint32_t min_ttl;
      /*< deprecated, will be ignored */
      pipe_idle_tmo_expiry_cb callback_fn;
      pipe_idle_tmo_expiry_cb_with_match_spec_copy callback_fn2;
      /*< Callback function to call in case of notification mode */
      void *client_data;
      /*< Client data for the callback function */
      bool default_callback_choice;
      /*< 0 for callback_fn; 1 for callback_fn2 >*/
    } notify;
  } u;
} pipe_idle_time_params_t;

typedef union pipe_stful_mem_spec_t {
  bool bit;
  uint8_t byte;
  uint16_t half;
  uint32_t word;
  uint64_t dbl;
  struct {
    uint8_t hi;
    uint8_t lo;
  } dbl_byte;
  struct {
    uint16_t hi;
    uint16_t lo;
  } dbl_half;
  struct {
    uint32_t hi;
    uint32_t lo;
  } dbl_word;
  struct {
    uint64_t hi;
    uint64_t lo;
  } dbl_dbl;
} pipe_stful_mem_spec_t;

typedef struct pipe_stful_mem_query_t {
  int pipe_count; /* Number of valid indices in the "data" array */
  /* Use pipe_stful_direct_query_get_sizes to determine the size of the data
   * array to allocate.  Note that when using a pipe_res_get_data_t pipe_mgr
   * will allocate this, caller is responsible for freeing. */
  pipe_stful_mem_spec_t *data;
} pipe_stful_mem_query_t;

/*! Statstics state data
 */
typedef struct pipe_stat_data {
  uint64_t bytes;   /*< Byte statistics */
  uint64_t packets; /*< Packet statistics */
} pipe_stat_data_t;

/*! Meter state data
 */
typedef struct pipe_meter_data {
  uint64_t comm_conformed_units;
  /*< bytes or packets that conformed to meter committed rate */
  uint64_t comm_exceeded_units;
  /*< bytes or packets that exceeded meter committed rate */
  uint64_t peak_exceeded_units;
  /*< bytes or packets that exceeded meter peak rate */
} pipe_meter_data_t;

/*! Stateful memory data
 */
enum pipe_res_action_tag {
  PIPE_RES_ACTION_TAG_NO_CHANGE,
  PIPE_RES_ACTION_TAG_ATTACHED,
  PIPE_RES_ACTION_TAG_DETACHED
};
typedef union pipe_res_data_spec_t {
  pipe_stful_mem_spec_t stful;
  pipe_meter_spec_t meter;
  pipe_lpf_spec_t lpf;
  pipe_wred_spec_t red;
  pipe_stat_data_t counter;
} pipe_res_data_spec_t;

typedef struct pipe_res_spec_t {
  pipe_res_hdl_t tbl_hdl;  // Use PIPE_GET_HDL_TYPE to decode
  pipe_res_idx_t tbl_idx;
  pipe_res_data_spec_t data;
  enum pipe_res_action_tag tag;
} pipe_res_spec_t;

/* When set fetch match and action specs. */
#define PIPE_RES_GET_FLAG_ENTRY (1 << 0)
/* When set fetch stats. */
#define PIPE_RES_GET_FLAG_CNTR (1 << 1)
/* When set fetch the meter/lpf/wred spec. */
#define PIPE_RES_GET_FLAG_METER (1 << 2)
/* When set fetch the stateful spec. */
#define PIPE_RES_GET_FLAG_STFUL (1 << 3)
/* When set fetch the idle information (hit-state or TTL). */
#define PIPE_RES_GET_FLAG_IDLE (1 << 4)
/* When all flags set. */
#define PIPE_RES_GET_FLAG_ALL                         \
  (PIPE_RES_GET_FLAG_ENTRY | PIPE_RES_GET_FLAG_CNTR | \
   PIPE_RES_GET_FLAG_METER | PIPE_RES_GET_FLAG_IDLE | PIPE_RES_GET_FLAG_STFUL)
typedef struct pipe_res_get_data_t {
  pipe_stat_data_t counter;
  pipe_stful_mem_query_t stful;
  union {
    pipe_meter_spec_t meter;
    pipe_lpf_spec_t lpf;
    pipe_wred_spec_t red;
  } mtr;
  union {
    pipe_idle_time_hit_state_e hit_state;
    uint32_t ttl;
  } idle;
  /* The valid flags below will be set to indicate whether the above resource
   * specs are valid. */
  bool has_counter;
  bool has_stful;
  bool has_meter;
  bool has_lpf;
  bool has_red;
  bool has_ttl;
  bool has_hit_state;
} pipe_res_get_data_t;

/*!
 * Action data specification
 */
typedef struct pipe_action_data_spec {
  uint16_t num_valid_action_data_bits;
  uint16_t num_action_data_bytes;
  /*!< Number of action data bits valid */
  uint8_t *action_data_bits;
  /*!< Action data */
} pipe_action_data_spec_t;

/* Types of action data for a match-action table entry */
#define PIPE_ACTION_DATA_TYPE (1 << 0)
#define PIPE_ACTION_DATA_HDL_TYPE (1 << 1)
#define PIPE_SEL_GRP_HDL_TYPE (1 << 2)
/*!
 * Generalized action specification that encodes all types of action data refs
 */
typedef struct pipe_action_spec {
  uint8_t pipe_action_datatype_bmap;
  uint8_t resource_count;
  /* bitmap of action datatypes */
  pipe_action_data_spec_t act_data;
  pipe_adt_ent_hdl_t adt_ent_hdl;
  pipe_sel_grp_hdl_t sel_grp_hdl;
#define PIPE_NUM_TBL_RESOURCES 4
  pipe_res_spec_t resources[PIPE_NUM_TBL_RESOURCES];
} pipe_action_spec_t;

#define IS_ACTION_SPEC_ACT_DATA(act_spec) \
  ((act_spec)->pipe_action_datatype_bmap & PIPE_ACTION_DATA_TYPE)
#define IS_ACTION_SPEC_ACT_DATA_HDL(act_spec) \
  ((act_spec)->pipe_action_datatype_bmap & PIPE_ACTION_DATA_HDL_TYPE)
#define IS_ACTION_SPEC_SEL_GRP(act_spec) \
  ((act_spec)->pipe_action_datatype_bmap & PIPE_SEL_GRP_HDL_TYPE)

typedef enum bf_tbl_dbg_counter_type_e {
  BF_TBL_DBG_CNTR_DISABLED = 0,
  BF_TBL_DBG_CNTR_LOG_TBL_MISS = 1,
  BF_TBL_DBG_CNTR_LOG_TBL_HIT = 2,
  BF_TBL_DBG_CNTR_GW_TBL_MISS = 3,
  BF_TBL_DBG_CNTR_GW_TBL_HIT = 4,
  BF_TBL_DBG_CNTR_GW_TBL_INHIBIT = 5,
  BF_TBL_DBG_CNTR_MAX
} bf_tbl_dbg_counter_type_t;
static inline const char *bf_tbl_dbg_counter_type_to_str(
    bf_tbl_dbg_counter_type_t t) {
  switch (t) {
    case BF_TBL_DBG_CNTR_DISABLED:
      return "BF_TBL_DBG_CNTR_DISABLED";
    case BF_TBL_DBG_CNTR_LOG_TBL_MISS:
      return "BF_TBL_DBG_CNTR_LOG_TBL_MISS";
    case BF_TBL_DBG_CNTR_LOG_TBL_HIT:
      return "BF_TBL_DBG_CNTR_LOG_TBL_HIT";
    case BF_TBL_DBG_CNTR_GW_TBL_MISS:
      return "BF_TBL_DBG_CNTR_GW_TBL_MISS";
    case BF_TBL_DBG_CNTR_GW_TBL_HIT:
      return "BF_TBL_DBG_CNTR_GW_TBL_HIT";
    case BF_TBL_DBG_CNTR_GW_TBL_INHIBIT:
      return "BF_TBL_DBG_CNTR_GW_TBL_INHIBIT";
    case BF_TBL_DBG_CNTR_MAX:
      return "BF_TBL_DBG_CNTR_MAX";
  }
  return "Unknown";
}
/* Snapshot enable/disable state */
typedef enum bf_snapshot_state_e {
  BF_SNAPSHOT_ST_DISABLED = 0,
  BF_SNAPSHOT_ST_ENABLED
} bf_snapshot_state_t;

/* Snapshot direction */
typedef enum {
  BF_SNAPSHOT_DIR_INGRESS = 0,
  BF_SNAPSHOT_DIR_EGRESS = 1
} bf_snapshot_dir_t;

/* Snapshot thread bit offset when getting thread capture mask */
typedef enum {
  BF_SNAPSHOT_THREAD_INGRESS = 0,
  BF_SNAPSHOT_THREAD_EGRESS = 1,
  BF_SNAPSHOT_THREAD_GHOST = 2
} bf_snapshot_thread_t;

/* Snapshot ingress mode used to configure which thread can trigger
 * ingress snapshots. This setting doesn't affect egress snapshots.
 */
typedef enum {
  BF_SNAPSHOT_IGM_INGRESS = 0,
  BF_SNAPSHOT_IGM_GHOST = 1,
  BF_SNAPSHOT_IGM_BOTH = 2,
  BF_SNAPSHOT_IGM_ANY = 3
} bf_snapshot_ig_mode_t;

/* Snapshot Fsm States */
typedef enum pipe_snapshot_fsm_state_e {
  PIPE_SNAPSHOT_FSM_ST_PASSIVE = 0,
  PIPE_SNAPSHOT_FSM_ST_ARMED,
  PIPE_SNAPSHOT_FSM_ST_TRIGGER_HAPPY,
  PIPE_SNAPSHOT_FSM_ST_FULL,
  /* Do not use */
  PIPE_SNAPSHOT_FSM_ST_MAX
} pipe_snapshot_fsm_state_t;

#define BF_TBL_NAME_LEN 200
#define BF_MAX_LOG_TBLS 16
#define BF_MAX_SNAPSHOT_CAPTURES 20
typedef struct bf_snapshot_tables_info {
  char table_name[BF_TBL_NAME_LEN];
  uint32_t table_handle;
  uint32_t match_hit_address;
  uint32_t hit_entry_handle;
  bool table_hit;
  bool table_inhibited;
  bool table_executed;
  bool table_type_tcam;
} bf_snapshot_tables_info_t;

typedef struct bf_snapshot_meter_alu_info {
  uint32_t table_handle;
  char *ctrl_info_p;
} bf_snapshot_meter_alu_info_t;

typedef struct bf_snapshot_capture_ctrl_info {
  uint8_t stage_id;
  bool valid;
  bool prev_stage_trigger;
  bool timer_trigger;
  bool local_stage_trigger;
  bf_snapshot_tables_info_t tables_info[BF_MAX_LOG_TBLS];
  char next_table[BF_TBL_NAME_LEN];
  char enabled_next_tbl_names[BF_TBL_NAME_LEN * BF_MAX_LOG_TBLS];
  bool ingr_dp_error;
  bool egr_dp_error;
  bf_snapshot_meter_alu_info_t meter_alu_info[4];
  char gbl_exec_tbl_names[BF_TBL_NAME_LEN * BF_MAX_LOG_TBLS];
  char enabled_gbl_exec_tbl_names[BF_TBL_NAME_LEN * BF_MAX_LOG_TBLS];
  char long_branch_tbl_names[BF_TBL_NAME_LEN * BF_MAX_LOG_TBLS];
  char enabled_long_branch_tbl_names[BF_TBL_NAME_LEN * BF_MAX_LOG_TBLS];
} bf_snapshot_capture_ctrl_info_t;

/* Array of snapshot control info captures if start and en stage are different*/
typedef struct bf_snapshot_capture_ctrl_info_arr {
  bf_snapshot_capture_ctrl_info_t ctrl[BF_MAX_SNAPSHOT_CAPTURES];
} bf_snapshot_capture_ctrl_info_arr_t;

typedef enum {
  INPUT_FIELD_ATTR_TYPE_MASK = 0,
  INPUT_FIELD_ATTR_TYPE_VALUE = 1,
  INPUT_FIELD_ATTR_TYPE_STREAM = 2,
} pipe_input_field_attr_type_t;

typedef enum {
  INPUT_FIELD_EXCLUDED = 0,
  INPUT_FIELD_INCLUDED
} pipe_input_field_attr_value_mask_t;

typedef struct pipe_hash_calc_input_field_attribute {
  /* This indicates index in fl_list*/
  uint32_t input_field;
  /* Runtime slice start bit. This is different from
     P4 defined slice. This can be slice of a P4
     slice too. If P4 defined slice is
     src_ip[32:9], then slice_start_bit 2 will mean
     bit number 11 in src_ip */
  uint32_t slice_start_bit;
  /* Length of runtime slice as described above.
     Value of 0 means till end of field  */
  uint32_t slice_length;
  /* This would only be used by pipe_mgr to check for
     symmetric fields.
     0 here DOESN'T mean disable a field in pipe_mgr. To
     disable field, mask needs to be INPUT_FIELD_EXCLUDED.
     If 0 is present in order in pipe_mgr,
     it will be just treated as a normal field and
     symmetric hashing won't kick in. Otherwise, fields
     with same order are grouped as symmetric */
  uint32_t order;
  pipe_input_field_attr_type_t type;
  union {
    pipe_input_field_attr_value_mask_t mask;
    uint32_t val;
    uint8_t *stream;
  } value;
} pipe_hash_calc_input_field_attribute_t;

/******************************************************************************
 *
 * MAT Placement Callbacks
 *
 *****************************************************************************/
enum pipe_mat_update_type {
  PIPE_MAT_UPDATE_ADD,
  PIPE_MAT_UPDATE_ADD_MULTI, /* An add to multiple logical indices. */
  PIPE_MAT_UPDATE_SET_DFLT,  /* Set the table's default action. */
  PIPE_MAT_UPDATE_CLR_DFLT,  /* Clear the table's default action. */
  PIPE_MAT_UPDATE_DEL,
  PIPE_MAT_UPDATE_MOD,
  PIPE_MAT_UPDATE_MOV,
  PIPE_MAT_UPDATE_MOV_MULTI, /* A move involving mulitple logical indices. */
  PIPE_MAT_UPDATE_MOV_MOD,
  PIPE_MAT_UPDATE_MOV_MULTI_MOD,
  PIPE_MAT_UPDATE_ADD_IDLE
};
static inline const char *pipe_mgr_move_list_op_str(
    enum pipe_mat_update_type op) {
  switch (op) {
    case PIPE_MAT_UPDATE_ADD:
      return "ADD";
    case PIPE_MAT_UPDATE_ADD_MULTI:
      return "ADD_MULTI";
    case PIPE_MAT_UPDATE_SET_DFLT:
      return "SET_DFLT";
    case PIPE_MAT_UPDATE_CLR_DFLT:
      return "CLR_DFLT";
    case PIPE_MAT_UPDATE_DEL:
      return "DEL";
    case PIPE_MAT_UPDATE_MOD:
      return "MOD";
    case PIPE_MAT_UPDATE_MOV:
      return "MOV";
    case PIPE_MAT_UPDATE_MOV_MULTI:
      return "MOV_MULTI";
    case PIPE_MAT_UPDATE_MOV_MOD:
      return "MOV_MOD";
    case PIPE_MAT_UPDATE_MOV_MULTI_MOD:
      return "MOV_MULTI_MOD";
    case PIPE_MAT_UPDATE_ADD_IDLE:
      return "ADD_IDLE";
    default:
      return "UNKNOWN";
  }
}
struct pipe_mat_update_set_dflt_params {
  pipe_mat_ent_hdl_t ent_hdl;
  pipe_adt_ent_hdl_t action_profile_mbr;
  pipe_idx_t indirect_action_index;
  bool action_profile_mbr_exists;
  pipe_sel_grp_hdl_t sel_grp_hdl;
  pipe_idx_t indirect_selection_index;
  uint32_t num_selector_words;
  bool sel_grp_exists;
  void *data;
};
struct pipe_mat_update_clr_dflt_params {
  pipe_mat_ent_hdl_t ent_hdl;
};
struct pipe_mat_update_add_params {
  pipe_mat_ent_hdl_t ent_hdl;
  uint32_t priority; /* TCAM priority, only valid for TCAM tables. */
  pipe_idx_t logical_index;
  pipe_adt_ent_hdl_t action_profile_mbr;
  pipe_idx_t indirect_action_index;
  bool action_profile_mbr_exists;
  pipe_sel_grp_hdl_t sel_grp_hdl;
  pipe_idx_t indirect_selection_index;
  uint32_t num_selector_words;
  bool sel_grp_exists;
  void *data;
};
struct pipe_mat_update_del_params {
  pipe_mat_ent_hdl_t ent_hdl;
};
struct pipe_mat_update_mod_params {
  pipe_mat_ent_hdl_t ent_hdl;
  pipe_adt_ent_hdl_t action_profile_mbr;
  pipe_idx_t indirect_action_index;
  bool action_profile_mbr_exists;
  pipe_sel_grp_hdl_t sel_grp_hdl;
  pipe_idx_t indirect_selection_index;
  uint32_t num_selector_words;
  bool sel_grp_exists;
  void *data;
};
struct pipe_mat_update_mov_params {
  pipe_mat_ent_hdl_t ent_hdl;
  pipe_idx_t logical_index;
  pipe_adt_ent_hdl_t action_profile_mbr;
  pipe_idx_t indirect_action_index;
  bool action_profile_mbr_exists;
  pipe_sel_grp_hdl_t sel_grp_hdl;
  pipe_idx_t indirect_selection_index;
  uint32_t num_selector_words;
  bool sel_grp_exists;
  void *data;
};
struct pipe_multi_index {
  /* Base logical index. */
  pipe_idx_t logical_index_base;
  /* Count of consecutive indexes (a minimum of one). */
  uint8_t logical_index_count;
};
/* Represents an add to multiple logical indices.  There are
 * logical_index_array_length number of base indices, specified in
 * logical_index_base_array.  Associated with each base index is a count,
 * specified in logical_index_count_array, saying how many consecutive indices
 * are used.  For example, given:
 *   logical_index_array_length = 3
 *   location_array = [ [250,2], [200,1], [300,6] ]
 * The following nine logical indices would be used: 250-251, 200, and 300-305.
 */
struct pipe_mat_update_add_multi_params {
  pipe_mat_ent_hdl_t ent_hdl;
  uint32_t priority; /* TCAM priority, only valid for TCAM tables. */
  pipe_adt_ent_hdl_t action_profile_mbr;
  pipe_idx_t indirect_action_index;
  bool action_profile_mbr_exists;
  pipe_sel_grp_hdl_t sel_grp_hdl;
  pipe_idx_t indirect_selection_index;
  uint32_t num_selector_words;
  bool sel_grp_exists;
  int logical_index_array_length;
  struct pipe_multi_index *location_array;
  void *data;
};
/* Represents a move of an entry occupying multiple logical indices.  Similar
 * to struct pipe_mat_update_add_multi_params, logical_index_array_length
 * specifies how many sets of logical indices are moving.  The location_array
 * provides the new logical indexes of the entry specified as a series of base
 * and count pairs representing count number of logical indexes starting at and
 * includeing the base.  For example, given:
 *   logical_index_array_length = 2
 *   location_array = [[50,3], [100,1]]
 * The entry now occupies logical indices 50-52 and 100. */
struct pipe_mat_update_mov_multi_params {
  pipe_mat_ent_hdl_t ent_hdl;
  pipe_adt_ent_hdl_t action_profile_mbr;
  pipe_idx_t indirect_action_index;
  bool action_profile_mbr_exists;
  pipe_sel_grp_hdl_t sel_grp_hdl;
  pipe_idx_t indirect_selection_index;
  uint32_t num_selector_words;
  bool sel_grp_exists;
  int logical_index_array_length;
  struct pipe_multi_index *location_array;
  void *data;
};
/* A union representing all the possible parameters to a MAT update. */
union pipe_mat_update_params {
  struct pipe_mat_update_set_dflt_params set_dflt;
  struct pipe_mat_update_clr_dflt_params clr_dflt;
  struct pipe_mat_update_add_params add;
  struct pipe_mat_update_del_params del;
  struct pipe_mat_update_mod_params mod;
  struct pipe_mat_update_mov_params mov;
  struct pipe_mat_update_add_multi_params add_multi;
  struct pipe_mat_update_mov_multi_params mov_multi;
};
typedef void (*pipe_mat_update_cb)(bf_dev_target_t dev_tgt,
                                   pipe_tbl_hdl_t tbl_hdl,
                                   enum pipe_mat_update_type update_type,
                                   union pipe_mat_update_params *update_params,
                                   void *cookie);
bf_status_t pipe_register_mat_update_cb(pipe_sess_hdl_t sess_hdl,
                                        bf_dev_id_t device_id,
                                        pipe_mat_tbl_hdl_t tbl_hdl,
                                        pipe_mat_update_cb cb,
                                        void *cb_cookie);

/******************************************************************************
 *
 * ADT Placement Callbacks
 *
 *****************************************************************************/
enum pipe_adt_update_type {
  PIPE_ADT_UPDATE_ADD,
  PIPE_ADT_UPDATE_DEL,
  PIPE_ADT_UPDATE_MOD
};
struct pipe_adt_update_add_params {
  pipe_adt_ent_hdl_t ent_hdl;
  void *data;
};
struct pipe_adt_update_del_params {
  pipe_adt_ent_hdl_t ent_hdl;
};
struct pipe_adt_update_mod_params {
  pipe_adt_ent_hdl_t ent_hdl;
  void *data;
};
union pipe_adt_update_params {
  struct pipe_adt_update_add_params add;
  struct pipe_adt_update_del_params del;
  struct pipe_adt_update_mod_params mod;
};
typedef void (*pipe_adt_update_cb)(bf_dev_target_t dev_tgt,
                                   pipe_tbl_hdl_t tbl_hdl,
                                   enum pipe_adt_update_type update_type,
                                   union pipe_adt_update_params *update_params,
                                   void *cookie);
bf_status_t pipe_register_adt_update_cb(pipe_sess_hdl_t sess_hdl,
                                        bf_dev_id_t device_id,
                                        pipe_adt_tbl_hdl_t tbl_hdl,
                                        pipe_adt_update_cb cb,
                                        void *cb_cookie);

/******************************************************************************
 *
 * SEL Placement Callbacks
 *
 *****************************************************************************/
enum pipe_sel_update_type {
  PIPE_SEL_UPDATE_GROUP_CREATE,
  PIPE_SEL_UPDATE_GROUP_DESTROY,
  PIPE_SEL_UPDATE_ADD,
  PIPE_SEL_UPDATE_DEL,
  PIPE_SEL_UPDATE_ACTIVATE,
  PIPE_SEL_UPDATE_DEACTIVATE,
  PIPE_SEL_UPDATE_SET_FALLBACK,
  PIPE_SEL_UPDATE_CLR_FALLBACK
};
struct pipe_sel_update_group_create_params {
  pipe_sel_grp_hdl_t grp_hdl;
  uint32_t max_members;
  uint32_t num_indexes;
  pipe_idx_t base_logical_index;
  int logical_adt_index_array_length;
  struct pipe_multi_index *logical_adt_indexes;
};
struct pipe_sel_update_group_destroy_params {
  pipe_sel_grp_hdl_t grp_hdl;
};
struct pipe_sel_update_add_params {
  pipe_sel_grp_hdl_t grp_hdl;
  pipe_adt_ent_hdl_t ent_hdl;
  pipe_idx_t logical_index;
  pipe_idx_t logical_subindex;
  void *data;
};
struct pipe_sel_update_del_params {
  pipe_sel_grp_hdl_t grp_hdl;
  pipe_adt_ent_hdl_t ent_hdl;
  pipe_idx_t logical_index;
  pipe_idx_t logical_subindex;
};
struct pipe_sel_update_activate_params {
  pipe_sel_grp_hdl_t grp_hdl;
  pipe_adt_ent_hdl_t ent_hdl;
  pipe_idx_t logical_index;
  pipe_idx_t logical_subindex;
};
struct pipe_sel_update_deactivate_params {
  pipe_sel_grp_hdl_t grp_hdl;
  pipe_adt_ent_hdl_t ent_hdl;
  pipe_idx_t logical_index;
  pipe_idx_t logical_subindex;
};
struct pipe_sel_set_fallback_params {
  pipe_adt_ent_hdl_t ent_hdl;
  void *data;
};
union pipe_sel_update_params {
  struct pipe_sel_update_group_create_params grp_create;
  struct pipe_sel_update_group_destroy_params grp_destroy;
  struct pipe_sel_update_add_params add;
  struct pipe_sel_update_del_params del;
  struct pipe_sel_update_activate_params activate;
  struct pipe_sel_update_deactivate_params deactivate;
  struct pipe_sel_set_fallback_params set_fallback;
};
typedef void (*pipe_sel_update_cb)(bf_dev_target_t dev_tgt,
                                   pipe_tbl_hdl_t tbl_hdl,
                                   enum pipe_sel_update_type update_type,
                                   union pipe_sel_update_params *update_params,
                                   void *cookie);
bf_status_t pipe_register_sel_update_cb(pipe_sess_hdl_t sess_hdl,
                                        bf_dev_id_t device_id,
                                        pipe_sel_tbl_hdl_t tbl_hdl,
                                        pipe_sel_update_cb cb,
                                        void *cb_cookie);

/****************************************
 * ParDe EBUF cut-through APIs
 ****************************************/
/*
 * NOTE:
 *     These APIs shouldn't be called directly and should be called only
 *     through BF_PAL APIs
 */
bf_status_t bf_pipe_mgr_port_ebuf_enable_cut_through(bf_dev_id_t dev_id,
                                                     bf_dev_port_t port_id);

bf_status_t bf_pipe_mgr_port_ebuf_disable_cut_through(bf_dev_id_t dev_id,
                                                      bf_dev_port_t port_id);

/****************************************
 * ParDe IBUF drop threshold API
 ****************************************/
/*
 * This API can be used to set ibuf high and low drop thresholds
 * for a specific port.
 *
 * Note:
 *   This API is not intended to be used by application as changing
 *   defaults would cause over-carving or under-carving of IBUF. This
 *   should be used for debugging purpose or for internal use.
 *
 * Related APIs: bf_pipe_mgr_port_ibuf_set_afull_threshold()
 *
 * @param dev_id              ASIC device identifier.
 * @param port_id             Port Identifier
 * @param drop_hi_thrd        Drop high threshold value in bytes
 * @param drop_low_thrd       Drop low threshold value in bytes
 * @return                    Status of API call.
 */
bf_status_t bf_pipe_mgr_port_ibuf_set_drop_threshold(bf_dev_id_t dev_id,
                                                     bf_dev_port_t port_id,
                                                     uint32_t drop_hi_thrd,
                                                     uint32_t drop_low_thrd);

/****************************************
 * ParDe IBUF almost full threshold API
 ****************************************/
/*
 * This API can be used to set ibuf high and low almost full thresholds
 * for a specific port.
 *
 * Note:
 *   This API is not intended to be used by application as changing
 *   defaults would cause over-carving or under-carving of IBUF. This
 *   should be used for debugging purpose or for internal use.
 *
 * Related APIs: bf_pipe_mgr_port_ibuf_set_drop_threshold()
 *
 * @param dev_id              ASIC device identifier.
 * @param port_id             Port Identifier
 * @param afull_hi_thrd       Afull high threshold value in bytes
 * @param afull_low_thrd      Afull low threshold value in bytes
 * @return                    Status of API call.
 */
bf_status_t bf_pipe_mgr_port_ibuf_set_afull_threshold(bf_dev_id_t dev_id,
                                                      bf_dev_port_t port_id,
                                                      uint32_t afull_hi_thrd,
                                                      uint32_t afull_low_thrd);

/******************************************************************************
 *
 * Placement Data Management
 *   Functions and types used to manage the "void *data" pointer in the
 *   placement update param structures.
 *
 *****************************************************************************/
/* Register the allocation and deallocation functions the driver should use to
 * obtain and release memory required for driver specific placement data.  Note
 * that the registered functions will be used across all devices and therefore
 * only need to be registered once.  If no functions are registered then
 * bf_sys_malloc and bf_sys_free will be used. */
typedef void *(*pipe_plcmnt_alloc)(size_t size);
typedef void (*pipe_plcmnt_free)(void *data);
bf_status_t pipe_register_plcmnt_mem_fns(pipe_plcmnt_alloc alloc_fn,
                                         pipe_plcmnt_free free_fn);
/* Returns the size of the memory pointed to by data.  This can be used to know
 * how much memory to allocate when copying data. */
bf_status_t bf_drv_plcmt_data_size(const void *data, size_t *size);
/* Copies the data at src to dst.  Dst should be pre-allocated and of adequate
 * size. src and dst must not overlap. */
bf_status_t bf_drv_plcmt_copy(const void *src, void *dst);
/* Allocates memory and then copys the data at src into it.  The memory will be
 * allocated using the registered alloc_fn if set.  The address of the copied
 * data will be returned in *copy. */
bf_status_t bf_drv_plcmt_duplicate(const void *src, void **copy);
/* Given an unpacked entry data, returns the size of that data if it were to be
 * packed.  This may be used to determine how much memory to allocate when using
 * bf_drv_plcmt_copy_pack. */
bf_status_t bf_drv_plcmt_pack_data_size(const void *unpacked_data,
                                        size_t *size);
/* Given a packed entry data, returns the size of that data if it were to be
 * unpacked.  This may be used to determine how much memory to allocate when
 * using bf_drv_plcmt_copy_unpack. */
bf_status_t bf_drv_plcmt_unpack_data_size(const void *packed_data,
                                          size_t *size);
/* Copies from unpacked_src into packed_dst packing the source data as it is
 * copied.  The destination must be allocated ahead of time.  The size of the
 * allocation can be determined with bf_drv_plcmt_pack_data_size. */
bf_status_t bf_drv_plcmt_copy_pack(const void *unpacked_src, void *packed_dst);
/* Copies from packed_src into an unpacked_dst unpacking the source data as it
 * is copied.  The destination must be allocated ahead of time.  The size of the
 * allocation can be determined with bf_drv_plcmt_unpack_data_size. */
bf_status_t bf_drv_plcmt_copy_unpack(const void *packed_src,
                                     void *unpacked_dst);
/* Frees placement data previously allocated by the driver which is no longer
 * needed.  This should be used to free the data returned by a delete callback
 * or the old data returned by a modify callback. */
bf_status_t bf_drv_plcmt_free(void *data);

/******************************************************************************
 *
 * Placement Data Decode
 *
 *****************************************************************************/
pipe_status_t pipe_mgr_plcmt_mat_data_get_entry(
    void *mat_data,
    pipe_tbl_match_spec_t *pipe_match_spec,
    pipe_action_spec_t *pipe_action_spec,
    pipe_act_fn_hdl_t *act_fn_hdl);
pipe_status_t pipe_mgr_plcmt_adt_data_get_entry(
    void *adt_data,
    pipe_action_data_spec_t *pipe_action_data_spec,
    pipe_act_fn_hdl_t *act_fn_hdl);

/******************************************************************************
 *
 * Placement Data Get
 *
 *****************************************************************************/
pipe_status_t pipe_mgr_plcmt_mat_data_get(pipe_sess_hdl_t sess_hdl,
                                          bf_dev_id_t dev_id,
                                          pipe_mat_tbl_hdl_t mat_tbl_hdl);
pipe_status_t pipe_mgr_plcmt_adt_data_get(pipe_sess_hdl_t sess_hdl,
                                          bf_dev_id_t dev_id,
                                          pipe_mat_tbl_hdl_t adt_tbl_hdl);
pipe_status_t pipe_mgr_plcmt_sel_data_get(pipe_sess_hdl_t sess_hdl,
                                          bf_dev_id_t dev_id,
                                          pipe_sel_tbl_hdl_t sel_tbl_hdl);

/******************************************************************************
 *
 * Placement Operations
 *
 *****************************************************************************/

/* Structure representing an ordered group of placement information on the same
 * table.
 * This structure must be created with the pipe_create_plcmt_info function.
 * This structure must be freed with the pipe_destroy_plcmt_info function.
 * The various table operations should be added to the structure using the
 * pipe_set_one_plcmt_op_XXX functions.*/
struct pipe_plcmt_info;
struct pipe_plcmt_info *pipe_create_plcmt_info(void);
void pipe_destroy_plcmt_info(struct pipe_plcmt_info *info);

/* To populate one operation in a pipe_placement_info structure for MAT entries.
 */
pipe_status_t pipe_set_one_plcmt_op_mat_add(struct pipe_plcmt_info *info,
                                            pipe_ent_hdl_t ent_hdl,
                                            bf_dev_pipe_t pipe,
                                            pipe_idx_t ent_idx,
                                            pipe_idx_t indirect_selection_idx,
                                            pipe_idx_t indirect_action_idx,
                                            void *ent_data);
pipe_status_t pipe_set_one_plcmt_op_mat_mov(struct pipe_plcmt_info *info,
                                            pipe_ent_hdl_t ent_hdl,
                                            pipe_idx_t ent_idx,
                                            pipe_idx_t indirect_selection_index,
                                            pipe_idx_t indirect_action_index,
                                            void *ent_data);
pipe_status_t pipe_set_one_plcmt_op_mat_mod(struct pipe_plcmt_info *info,
                                            pipe_ent_hdl_t ent_hdl,
                                            pipe_idx_t indirect_selection_index,
                                            pipe_idx_t indirect_action_index,
                                            void *ent_data);
pipe_status_t pipe_set_one_plcmt_op_mat_del(struct pipe_plcmt_info *info,
                                            pipe_ent_hdl_t ent_hdl);
pipe_status_t pipe_set_one_plcmt_op_mat_set_dflt(
    struct pipe_plcmt_info *info,
    pipe_ent_hdl_t ent_hdl,
    bf_dev_pipe_t pipe,
    pipe_idx_t indirect_selection_idx,
    pipe_idx_t logical_action_idx,
    void *ent_data);
pipe_status_t pipe_set_one_plcmt_op_mat_clr_dflt(struct pipe_plcmt_info *info,
                                                 pipe_ent_hdl_t ent_hdl,
                                                 bf_dev_pipe_t pipe);
pipe_status_t pipe_set_one_plcmt_op_mat_add_multi(
    struct pipe_plcmt_info *info,
    pipe_ent_hdl_t ent_hdl,
    bf_dev_pipe_t pipe,
    pipe_idx_t indirect_selection_idx,
    pipe_idx_t indirect_action_idx,
    int array_length,
    struct pipe_multi_index *location_array,
    void *ent_data);
pipe_status_t pipe_set_one_plcmt_op_mat_mov_multi(
    struct pipe_plcmt_info *info,
    pipe_ent_hdl_t ent_hdl,
    pipe_idx_t indirect_selection_idx,
    pipe_idx_t indirect_action_idx,
    int array_length,
    struct pipe_multi_index *location_array,
    void *ent_data);

/* To populate one operation in a pipe_plcmt_info structure for ADT entries.
 */
pipe_status_t pipe_set_one_plcmt_op_adt_add(struct pipe_plcmt_info *info,
                                            pipe_ent_hdl_t ent_hdl,
                                            bf_dev_pipe_t pipe,
                                            void *ent_data);
pipe_status_t pipe_set_one_plcmt_op_adt_mod(struct pipe_plcmt_info *info,
                                            pipe_ent_hdl_t ent_hdl,
                                            void *ent_data);
pipe_status_t pipe_set_one_plcmt_op_adt_del(struct pipe_plcmt_info *info,
                                            pipe_ent_hdl_t ent_hdl);

/* To populate one operation in a pipe_plcmt_info structure for Selection
 * entries.
 */
pipe_status_t pipe_set_one_plcmt_op_sel_grp_create(
    struct pipe_plcmt_info *info,
    pipe_sel_grp_hdl_t grp_hdl,
    bf_dev_pipe_t pipe,
    uint32_t num_indexes,
    uint32_t max_members,
    pipe_idx_t base_logical_index,
    int array_length,
    struct pipe_multi_index *location_array);
pipe_status_t pipe_set_one_plcmt_op_sel_grp_destroy(
    struct pipe_plcmt_info *info,
    pipe_sel_grp_hdl_t grp_hdl,
    bf_dev_pipe_t pipe);
pipe_status_t pipe_set_one_plcmt_op_sel_add(struct pipe_plcmt_info *info,
                                            pipe_sel_grp_hdl_t grp_hdl,
                                            pipe_ent_hdl_t ent_hdl,
                                            bf_dev_pipe_t pipe,
                                            pipe_idx_t ent_idx,
                                            pipe_idx_t ent_subidx,
                                            void *ent_data);
pipe_status_t pipe_set_one_plcmt_op_sel_del(struct pipe_plcmt_info *info,
                                            pipe_sel_grp_hdl_t grp_hdl,
                                            pipe_ent_hdl_t ent_hdl,
                                            bf_dev_pipe_t pipe,
                                            pipe_idx_t ent_idx,
                                            pipe_idx_t ent_subidx);
pipe_status_t pipe_set_one_plcmt_op_sel_activate(struct pipe_plcmt_info *info,
                                                 pipe_sel_grp_hdl_t grp_hdl,
                                                 pipe_ent_hdl_t ent_hdl,
                                                 bf_dev_pipe_t pipe,
                                                 pipe_idx_t ent_idx,
                                                 pipe_idx_t ent_subidx);
pipe_status_t pipe_set_one_plcmt_op_sel_deactivate(struct pipe_plcmt_info *info,
                                                   pipe_sel_grp_hdl_t grp_hdl,
                                                   pipe_ent_hdl_t ent_hdl,
                                                   bf_dev_pipe_t pipe,
                                                   pipe_idx_t ent_idx,
                                                   pipe_idx_t ent_subidx);
pipe_status_t pipe_set_one_plcmt_op_sel_set_fallback(
    struct pipe_plcmt_info *info,
    pipe_ent_hdl_t ent_hdl,
    bf_dev_pipe_t pipe,
    void *ent_data);
pipe_status_t pipe_set_one_plcmt_op_sel_clr_fallback(
    struct pipe_plcmt_info *info, bf_dev_pipe_t pipe);

/* A function to process the placement info contained in a pipe_plcmt_info
 * structure. */
pipe_status_t pipe_process_plcmt_info(pipe_sess_hdl_t sess_hdl,
                                      bf_dev_id_t dev_id,
                                      pipe_tbl_hdl_t tbl_hdl,
                                      struct pipe_plcmt_info *info,
                                      uint32_t pipe_api_flags,
                                      uint32_t *processed);

/********************************************
 * Library init/cleanup API
 ********************************************/

/* API to invoke pipe_mgr initialization */
pipe_status_t pipe_mgr_init(void);

void pipe_mgr_cleanup(void);

/********************************************
 * CLIENT API
 ********************************************/

/* API to invoke client library registration */
pipe_status_t pipe_mgr_client_init(pipe_sess_hdl_t *sess_hdl);

/* API to invoke client library de-registration */
pipe_status_t pipe_mgr_client_cleanup(pipe_sess_hdl_t def_sess_hdl);

/********************************************
 * Transaction related API */
/*!
 * Begin a transaction on a session. Only one transaction can be in progress
 * on any given session
 *
 * @param shdl Handle to an active session
 * @param isAtomic If @c true, upon committing the transaction, all changes
 *        will be applied atomically such that a packet being processed will
 *        see either all of the changes or none of the changes.
 * @return Status of the API call
 */
pipe_status_t pipe_mgr_begin_txn(pipe_sess_hdl_t shdl, bool isAtomic);

/*!
 * Verify if all the API requests against the transaction in progress have
 * resources to be committed. This also ends the transaction implicitly
 *
 * @param Handle to an active session
 * @return Status of the API call
 */
pipe_status_t pipe_mgr_verify_txn(pipe_sess_hdl_t shdl);

/*!
 * Abort and rollback all API requests against the transaction in progress
 * This also ends the transaction implicitly
 *
 * @param Handle to an active session
 * @return Status of the API call
 */
pipe_status_t pipe_mgr_abort_txn(pipe_sess_hdl_t shdl);

/*!
 * Abort and rollback all API requests against the transaction in progress
 * This also ends the transaction implicitly
 *
 * @param Handle to an active session
 * @return Status of the API call
 */
pipe_status_t pipe_mgr_commit_txn(pipe_sess_hdl_t shdl, bool hwSynchronous);

/********************************************
 * Batch related API */
/*!
 * Begin a batch on a session. Only one batch can be in progress
 * on any given session.  Updates to the hardware will be batch together
 * and delayed until the batch is ended.
 *
 * @param shdl Handle to an active session
 * @return Status of the API call
 */
pipe_status_t pipe_mgr_begin_batch(pipe_sess_hdl_t shdl);

/*!
 * Flush a batch on a session pushing all pending updates to hardware.
 *
 * @param shdl Handle to an active session
 * @return Status of the API call
 */
pipe_status_t pipe_mgr_flush_batch(pipe_sess_hdl_t shdl);

/*!
 * End a batch on a session and push all batched updated to hardware.
 *
 * @param shdl Handle to an active session
 * @return Status of the API call
 */
pipe_status_t pipe_mgr_end_batch(pipe_sess_hdl_t shdl, bool hwSynchronous);

/*!
 * Helper function for of-tests. Return after all the pending operations
 * for the given session have been completed.
 *
 * @param Handle to an active session
 * @return Status of the API call
 */
pipe_status_t pipe_mgr_complete_operations(pipe_sess_hdl_t shdl);

pipe_status_t pipe_mgr_tbl_is_tern(bf_dev_id_t dev_id,
                                   pipe_tbl_hdl_t tbl_hdl,
                                   bool *is_tern);

/*!
 * API to get a match entry handle for the given match spec
 */
pipe_status_t pipe_mgr_match_spec_to_ent_hdl(pipe_sess_hdl_t sess_hdl,
                                             dev_target_t dev_tgt,
                                             pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                             pipe_tbl_match_spec_t *match_spec,
                                             pipe_mat_ent_hdl_t *mat_ent_hdl,
                                             bool light_pipe_validation);

/*!
 * API to get a match spec reference for the given match-entry handle
 * @output param : entry_pipe_id
 * @output param : match_spec
 * @Note It is user's responsibility to copy/caching the match spec since it can
 * be altered
 * as the table get updated.
 */
pipe_status_t pipe_mgr_ent_hdl_to_match_spec(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    bf_dev_pipe_t *ent_pipe_id,
    pipe_tbl_match_spec_t const **match_spec);

/*!
 * API to free a pipe_tbl_match_spec_t
 */
pipe_status_t pipe_mgr_match_spec_free(pipe_tbl_match_spec_t *match_spec);

/*!
 * API to duplicate an existing match spec to a new match spec
 * It allocates new memory. (dynamic memory)
 */
pipe_status_t pipe_mgr_match_spec_duplicate(
    pipe_tbl_match_spec_t **match_spec_dest,
    pipe_tbl_match_spec_t const *match_spec_src);

/*!
 * API to apply match key mask on a dynamic key mask table
 */
pipe_status_t pipe_mgr_match_key_mask_spec_set(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t device_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec);

pipe_status_t pipe_mgr_match_key_mask_spec_reset(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t device_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl);

pipe_status_t pipe_mgr_match_key_mask_spec_get(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t device_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec);

struct pipe_mgr_move_list_t;
pipe_status_t pipe_mgr_mat_tbl_update_action(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    struct pipe_mgr_move_list_t **move_list);

/*!
 * API to install an entry into a match action table
 */
pipe_status_t pipe_mgr_mat_ent_add(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec,
    pipe_act_fn_hdl_t act_fn_hdl,
    pipe_action_spec_t *act_data_spec,
    uint32_t ttl, /*< TTL value in msecs, 0 for disable */
    uint32_t pipe_api_flags,
    pipe_mat_ent_hdl_t *ent_hdl_p);

/*!
 * API to install an entry into a match action table,
 * if the entry already exist modify that entry
 */
pipe_status_t pipe_mgr_mat_ent_add_or_mod(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec,
    pipe_act_fn_hdl_t act_fn_hdl,
    pipe_action_spec_t *act_spec,
    uint32_t ttl, /*< TTL value in msecs, 0 for disable */
    uint32_t pipe_api_flags,
    pipe_mat_ent_hdl_t *ent_hdl_p,
    bool *is_added);

/*!
 * API to install default (miss) entry for a match action table
 */
pipe_status_t pipe_mgr_mat_default_entry_set(pipe_sess_hdl_t sess_hdl,
                                             dev_target_t dev_tgt,
                                             pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                             pipe_act_fn_hdl_t act_fn_hdl,
                                             pipe_action_spec_t *act_spec,
                                             uint32_t pipe_api_flags,
                                             pipe_mat_ent_hdl_t *ent_hdl_p);

/*!
 * API to get default (miss) entry handle for a match action table
 */
pipe_status_t pipe_mgr_table_get_default_entry_handle(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_mat_ent_hdl_t *ent_hdl_p);

/**
 * Get default entry information
 *
 * @param  sess_hdl              Session handle
 * @param  dev_tgt               Device ID and pipe-id to query.
 * @param  mat_tbl_hdl           Table handle.
 * @param  pipe_action_spec      Action data spec to populate.
 * @param  act_fn_hdl            Action function handle to populate.
 * @param  from_hw               Read from HW.
 * @param  res_get_flags         Bitwise OR of PIPE_RES_GET_FLAG_xxx indicating
 *                               which direct resources should be queried.  If
 *                               an entry does not use a resource it will not be
 *                               queried even if requested.
 *                               Set to PIPE_RES_GET_FLAG_ENTRY for original
 *                               behavior.
 * @param  res_data              Pointer to a pipe_res_data_t to hold the
 *                               resource data requested by res_get_flags.  Can
 *                               be NULL if res_get_flags is zero.
 *                               Note that if PIPE_RES_GET_FLAG_STFUL is set
 *                               pipe_mgr will allocate the data in the
 *                               res_data.stful structure and the caller must
 *                               free it with bf_sys_free.
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_table_get_default_entry(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_action_spec_t *pipe_action_spec,
    pipe_act_fn_hdl_t *act_fn_hdl,
    bool from_hw,
    uint32_t res_get_flags,
    pipe_res_get_data_t *res_data);

/**
 * Query which direct resources are used by an action on a MAT
 *
 * @param  dev_id                Device ID
 * @param  mat_tbl_hdl           Table handle being queried
 * @param  act_fn_hdl            Action function handle being queried
 * @param  has_dir_stats         Pointer to a bool which will be set to true if
 *                               the action uses direct stats.  May be NULL.
 * @param  has_dir_meter         Pointer to a bool which will be set to true if
 *                               the action uses a direct meter.  May be NULL.
 * @param  has_dir_lpf           Pointer to a bool which will be set to true if
 *                               the action uses a direct LPF.  May be NULL.
 * @param  has_dir_wred          Pointer to a bool which will be set to true if
 *                               the action uses a direct WRED.  May be NULL.
 * @param  has_dir_stful         Pointer to a bool which will be set to true if
 *                               the action uses a direct stful.  May be NULL.
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_get_action_dir_res_usage(bf_dev_id_t dev_id,
                                                pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                                pipe_act_fn_hdl_t act_fn_hdl,
                                                bool *has_dir_stats,
                                                bool *has_dir_meter,
                                                bool *has_dir_lpf,
                                                bool *has_dir_wred,
                                                bool *has_dir_stful);

/*!
 * API function to clear all entries from a match action table. This API
 * doesn't clear default entry. Use pipe_mgr_mat_tbl_default_entry_reset
 * in conjunction with this API to do the same.
 */
pipe_status_t pipe_mgr_mat_tbl_clear(pipe_sess_hdl_t sess_hdl,
                                     dev_target_t dev_tgt,
                                     pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                     uint32_t pipe_api_flags);

/*!
 * API function to delete an entry from a match action table using an ent hdl
 */
pipe_status_t pipe_mgr_mat_ent_del(pipe_sess_hdl_t sess_hdl,
                                   bf_dev_id_t device_id,
                                   pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                   pipe_mat_ent_hdl_t mat_ent_hdl,
                                   uint32_t pipe_api_flags);

/*!
 * API function to delete an entry from a match action table using a match spec
 */
pipe_status_t pipe_mgr_mat_ent_del_by_match_spec(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec,
    uint32_t pipe_api_flags);

/*!
 * API function to clear the default entry installed.
 */
pipe_status_t pipe_mgr_mat_tbl_default_entry_reset(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    uint32_t pipe_api_flags);

/*!
 * API function to modify action for a match action table entry using an ent hdl
 */
pipe_status_t pipe_mgr_mat_ent_set_action(pipe_sess_hdl_t sess_hdl,
                                          bf_dev_id_t device_id,
                                          pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                          pipe_mat_ent_hdl_t mat_ent_hdl,
                                          pipe_act_fn_hdl_t act_fn_hdl,
                                          pipe_action_spec_t *act_spec,
                                          uint32_t pipe_api_flags);

/*!
 * API function to modify action for a match action table entry using match spec
 */
pipe_status_t pipe_mgr_mat_ent_set_action_by_match_spec(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_match_spec_t *match_spec,
    pipe_act_fn_hdl_t act_fn_hdl,
    pipe_action_spec_t *act_spec,
    uint32_t pipe_api_flags);

pipe_status_t pipe_mgr_mat_ent_set_resource(pipe_sess_hdl_t sess_hdl,
                                            bf_dev_id_t device_id,
                                            pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                            pipe_mat_ent_hdl_t mat_ent_hdl,
                                            pipe_res_spec_t *resources,
                                            int resource_count,
                                            uint32_t pipe_api_flags);

/********************************************
 * API FOR ACTION DATA TABLE MANIPULATION   *
 ********************************************/
typedef uint16_t pipe_mgr_data_tag_t;

typedef struct adt_data_resources_ {
  pipe_res_hdl_t tbl_hdl;
  pipe_res_idx_t tbl_idx;
} adt_data_resources_t;

typedef struct pipe_mgr_adt_ent_data {
  pipe_mgr_data_tag_t tag;
  uint8_t num_resources;
  uint8_t is_const;
  pipe_act_fn_hdl_t act_fn_hdl;
  pipe_action_data_spec_t action_data;
  adt_data_resources_t adt_data_resources[PIPE_NUM_TBL_RESOURCES];
} pipe_mgr_adt_ent_data_t;

/**
 * Get entry member handle for specified member id.
 *
 * @param  sess_hdl              Session handle
 * @param  dev_tgt               Device ID and pipe-id to query.
 * @param  adt_tbl_hdl           Table handle.
 * @param  mbr_id                Entry member id.
 * @param  adt_ent_hdl           Pointer to where entry handle should be stored.
 * @param  check_only            If member is not found will not trigger any
 *                               txn related code.
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_adt_ent_hdl_get(pipe_sess_hdl_t shdl,
                                       bf_dev_target_t dev_tgt,
                                       pipe_adt_tbl_hdl_t adt_tbl_hdl,
                                       pipe_adt_mbr_id_t mbr_id,
                                       pipe_adt_ent_hdl_t *adt_ent_hdl,
                                       bool check_only);

/**
 * Get entry member id for specified entry handle.
 *
 * @param  sess_hdl              Session handle
 * @param  dev_id                Device ID to query.
 * @param  adt_tbl_hdl           Table handle.
 * @param  ent_hdl               Entry handle.
 * @param  adt_mbr_id            Pointer to where member id should be stored.
 * @param  adt_mbr_pipe          Pointer to where member pipe should be stored.
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_adt_mbr_id_get(pipe_sess_hdl_t shdl,
                                      bf_dev_id_t dev_id,
                                      pipe_adt_tbl_hdl_t adt_tbl_hdl,
                                      pipe_adt_ent_hdl_t ent_hdl,
                                      pipe_adt_mbr_id_t *adt_mbr_id,
                                      bf_dev_pipe_t *adt_mbr_pipe);

/**
 * Get entry data and handle for specified member id.
 *
 * @param  sess_hdl              Session handle
 * @param  dev_tgt               Device ID and pipe-id to query.
 * @param  adt_tbl_hdl           Table handle.
 * @param  mbr_id                Entry member id.
 * @param  adt_ent_hdl           Pointer to where new entry handle will be
 *                               stored.
 * @param  ent_data              Action entry data to populate.
 * @param  pipe_api_flags        Flags variable for this API call.
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_adt_ent_data_get(pipe_sess_hdl_t shdl,
                                        bf_dev_target_t dev_tgt,
                                        pipe_adt_tbl_hdl_t adt_tbl_hdl,
                                        pipe_adt_mbr_id_t mbr_id,
                                        pipe_adt_ent_hdl_t *adt_ent_hdl,
                                        pipe_mgr_adt_ent_data_t *ent_data);

/**
 * Add entry to action data table.
 *
 * @param  sess_hdl              Session handle
 * @param  dev_tgt               Device ID and pipe-id to query.
 * @param  adt_tbl_hdl           Table handle.
 * @param  act_fn_hdl            Action function handle to use by new entry.
 * @param  mbr_id                When specified pipe_mgr will cache mbr_id to
 *                               entry mapping. This allows to fetch entry
 *                               handle by mbr_id specified by user. In order
 *                               to enable this feature PIPE_FLAG_CACHE_ENT_ID
 *                               must be set in the pipe_api_flags for
 *                               the first API call.
 * @param  action_spec           Action data spec to populate.
 * @param  adt_ent_hdl_p         Pointer to where new entry handle will be
 *                               stored.
 * @param  pipe_api_flags        Flags variable for this API call.
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_adt_ent_add(pipe_sess_hdl_t sess_hdl,
                                   dev_target_t dev_tgt,
                                   pipe_adt_tbl_hdl_t adt_tbl_hdl,
                                   pipe_act_fn_hdl_t act_fn_hdl,
                                   pipe_adt_mbr_id_t mbr_id,
                                   pipe_action_spec_t *action_spec,
                                   pipe_adt_ent_hdl_t *adt_ent_hdl_p,
                                   uint32_t pipe_api_flags);

/* API function to free an action data entry */
pipe_status_t pipe_mgr_adt_ent_del(pipe_sess_hdl_t sess_hdl,
                                   bf_dev_id_t device_id,
                                   pipe_adt_tbl_hdl_t adt_tbl_hdl,
                                   pipe_adt_ent_hdl_t adt_ent_hdl,
                                   uint32_t pipe_api_flags);

/* API function to add an initial (P4-specified) action data entry */
pipe_status_t pipe_mgr_adt_init_ent_add(pipe_sess_hdl_t shdl,
                                        bf_dev_target_t dev_tgt,
                                        pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                        pipe_adt_tbl_hdl_t adt_tbl_hdl,
                                        pipe_act_fn_hdl_t act_fn_hdl,
                                        pipe_action_spec_t *action_spec,
                                        pipe_adt_ent_hdl_t *adt_ent_hdl_p,
                                        uint32_t pipe_api_flags);

/* API function to update an action data entry */
pipe_status_t pipe_mgr_adt_ent_set(pipe_sess_hdl_t sess_hdl,
                                   bf_dev_id_t device_id,
                                   pipe_adt_tbl_hdl_t adt_tbl_hdl,
                                   pipe_adt_ent_hdl_t adt_ent_hdl,
                                   pipe_act_fn_hdl_t act_fn_hdl,
                                   pipe_action_spec_t *action_spec,
                                   uint32_t pipe_api_flags);

/*********************************************
 * API FOR SELECTOR TABLE MANIPULATION *
 ********************************************/

/*!
 * Callback function prototype to track updates within a stateful selection
 * table.
 */
typedef pipe_status_t (*pipe_mgr_sel_tbl_update_callback)(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    void *cookie,
    pipe_sel_grp_hdl_t sel_grp_hdl,
    pipe_adt_ent_hdl_t adt_ent_hdl,
    int logical_table_index,
    bool is_add);

/*!
 * API function to register a callback function to track updates to groups in
 * the selection table.
 */
pipe_status_t pipe_mgr_sel_tbl_register_cb(pipe_sess_hdl_t sess_hdl,
                                           bf_dev_id_t device_id,
                                           pipe_sel_tbl_hdl_t sel_tbl_hdl,
                                           pipe_mgr_sel_tbl_update_callback cb,
                                           void *cb_cookie);

/*!
 * API function to set the group profile for a selection table
 */
pipe_status_t pipe_mgr_sel_tbl_profile_set(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_sel_tbl_hdl_t sel_tbl_hdl,
    pipe_sel_tbl_profile_t *sel_tbl_profile);

/**
 * Add entry to selector table.
 *
 * @param  sess_hdl              Session handle
 * @param  dev_tgt               Device ID and pipe-id to query.
 * @param  sel_tbl_hdl           Table handle.
 * @param  sel_grp_id            When specified pipe_mgr will cache sel_grp_id
 *                               to entry mapping. This allows to fetch entry
 *                               handle by sel_grp_id specified by the user.
 *                               In order to enable this feature
 *                               PIPE_FLAG_CACHE_ENT_ID must be set in
 *                               pipe_api_flags for the first API call.
 * @param  max_grp_size          Maximum number of member for new group.
 * @param  sel_grp_hdl_p         Pointer to where new entry handle will be
 *                               stored.
 * @param  pipe_api_flags        Flags variable for this API call.
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_sel_grp_add(pipe_sess_hdl_t sess_hdl,
                                   dev_target_t dev_tgt,
                                   pipe_sel_tbl_hdl_t sel_tbl_hdl,
                                   pipe_sel_grp_id_t sel_grp_id,
                                   uint32_t max_grp_size,
                                   uint32_t adt_offset,
                                   pipe_sel_grp_hdl_t *sel_grp_hdl_p,
                                   uint32_t pipe_api_flags);

/*!
 * API function to delete a group from a selection table
 */
pipe_status_t pipe_mgr_sel_grp_del(pipe_sess_hdl_t sess_hdl,
                                   bf_dev_id_t device_id,
                                   pipe_sel_tbl_hdl_t sel_tbl_hdl,
                                   pipe_sel_grp_hdl_t sel_grp_hdl,
                                   uint32_t pipe_api_flags);

/*!
 * API function to set size of a group from a selection table
 */
pipe_status_t pipe_mgr_sel_grp_size_set(pipe_sess_hdl_t sess_hdl,
                                        dev_target_t dev_tgt,
                                        pipe_sel_tbl_hdl_t sel_tbl_hdl,
                                        pipe_sel_grp_hdl_t sel_grp_hdl,
                                        uint32_t max_grp_size);

/*!
 * API function to add a member to a group of a selection table
 */
pipe_status_t pipe_mgr_sel_grp_mbr_add(pipe_sess_hdl_t sess_hdl,
                                       bf_dev_id_t device_id,
                                       pipe_sel_tbl_hdl_t sel_tbl_hdl,
                                       pipe_sel_grp_hdl_t sel_grp_hdl,
                                       pipe_act_fn_hdl_t act_fn_hdl,
                                       pipe_adt_ent_hdl_t adt_ent_hdl,
                                       uint32_t pipe_api_flags);

/*!
 * API function to delete a member from a group of a selection table
 */
pipe_status_t pipe_mgr_sel_grp_mbr_del(pipe_sess_hdl_t sess_hdl,
                                       bf_dev_id_t device_id,
                                       pipe_sel_tbl_hdl_t sel_tbl_hdl,
                                       pipe_sel_grp_hdl_t sel_grp_hdl,
                                       pipe_adt_ent_hdl_t adt_ent_hdl,
                                       uint32_t pipe_api_flags);

/*!
 * API function to set membership of a group
 */
pipe_status_t pipe_mgr_sel_grp_mbrs_set(pipe_sess_hdl_t sess_hdl,
                                        bf_dev_id_t device_id,
                                        pipe_sel_tbl_hdl_t sel_tbl_hdl,
                                        pipe_sel_grp_hdl_t sel_grp_hdl,
                                        uint32_t num_mbrs,
                                        pipe_adt_ent_hdl_t *mbrs,
                                        bool *enable,
                                        uint32_t pipe_api_flags);

/*!
 * API function to get membership of a group
 */
pipe_status_t pipe_mgr_sel_grp_mbrs_get(pipe_sess_hdl_t sess_hdl,
                                        bf_dev_id_t device_id,
                                        pipe_sel_tbl_hdl_t sel_tbl_hdl,
                                        pipe_sel_grp_hdl_t sel_grp_hdl,
                                        uint32_t mbrs_size,
                                        pipe_adt_ent_hdl_t *mbrs,
                                        bool *enable,
                                        uint32_t *mbrs_populated,
                                        bool from_hw);

enum pipe_mgr_grp_mbr_state_e {
  PIPE_MGR_GRP_MBR_STATE_ACTIVE = 0,
  PIPE_MGR_GRP_MBR_STATE_INACTIVE = 1
};

/* API function to disable a group member of a selection table */
pipe_status_t pipe_mgr_sel_grp_mbr_disable(pipe_sess_hdl_t sess_hdl,
                                           bf_dev_id_t device_id,
                                           pipe_sel_tbl_hdl_t sel_tbl_hdl,
                                           pipe_sel_grp_hdl_t sel_grp_hdl,
                                           pipe_adt_ent_hdl_t adt_ent_hdl,
                                           uint32_t pipe_api_flags);

/* API function to re-enable a group member of a selection table */
pipe_status_t pipe_mgr_sel_grp_mbr_enable(pipe_sess_hdl_t sess_hdl,
                                          bf_dev_id_t device_id,
                                          pipe_sel_tbl_hdl_t sel_tbl_hdl,
                                          pipe_sel_grp_hdl_t sel_grp_hdl,
                                          pipe_adt_ent_hdl_t adt_ent_hdl,
                                          uint32_t pipe_api_flags);

/* API function to get the current state of a selection member */
pipe_status_t pipe_mgr_sel_grp_mbr_state_get(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t device_id,
    pipe_sel_tbl_hdl_t sel_tbl_hdl,
    pipe_sel_grp_hdl_t sel_grp_hdl,
    pipe_adt_ent_hdl_t adt_ent_hdl,
    enum pipe_mgr_grp_mbr_state_e *mbr_state_p);

/**
 * Get selector group id by handle.
 *
 * @param  sess_hdl              Session handle.
 * @param  dev_id                Device id.
 * @param  seltbl_hdl            Selector table handle.
 * @param  sel_grp_hdl           Selector group handle.
 * @param  sel_grp_id            Pointer to selector group id variable.
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_sel_grp_id_get(pipe_sess_hdl_t sess_hdl,
                                      dev_target_t dev_tgt,
                                      pipe_sel_tbl_hdl_t sel_tbl_hdl,
                                      pipe_sel_grp_hdl_t sel_grp_hdl,
                                      pipe_sel_grp_id_t *sel_grp_id);

/**
 * Get selector group handle by grp_id
 *
 * @param  sess_hdl              Session handle.
 * @param  dev_id                Device id.
 * @param  sel_tbl_hdl           Selector table handle.
 * @param  sel_grp_id            Selector group id..
 * @param  sel_grp_hdl           Pointer to handle variable.
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_sel_grp_hdl_get(pipe_sess_hdl_t sess_hdl,
                                       dev_target_t dev_tgt,
                                       pipe_sel_tbl_hdl_t sel_tbl_hdl,
                                       pipe_sel_grp_id_t sel_grp_id,
                                       pipe_sel_grp_hdl_t *sel_grp_hdl);

/* API function to get the member handle given a hash value */
pipe_status_t pipe_mgr_sel_grp_mbr_get_from_hash(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t dev_id,
    pipe_sel_tbl_hdl_t sel_tbl_hdl,
    pipe_sel_grp_hdl_t grp_hdl,
    uint8_t *hash,
    uint32_t hash_len,
    pipe_adt_ent_hdl_t *adt_ent_hdl_p);

/* API function to set the fallback member */
pipe_status_t pipe_mgr_sel_fallback_mbr_set(pipe_sess_hdl_t sess_hdl,
                                            dev_target_t dev_tgt,
                                            pipe_sel_tbl_hdl_t sel_tbl_hdl,
                                            pipe_adt_ent_hdl_t adt_ent_hdl,
                                            uint32_t pipe_api_flags);

/* API function to reset the fallback member */
pipe_status_t pipe_mgr_sel_fallback_mbr_reset(pipe_sess_hdl_t sess_hdl,
                                              dev_target_t dev_tgt,
                                              pipe_sel_tbl_hdl_t sel_tbl_hdl,
                                              uint32_t pipe_api_flags);

/***************************************
 * API FOR FLOW LEARNING NOTIFICATIONS *
 ***************************************/
/* Flow learn notify registration */
pipe_status_t pipe_mgr_lrn_digest_notification_register(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t device_id,
    pipe_fld_lst_hdl_t flow_lrn_fld_lst_hdl,
    pipe_flow_lrn_notify_cb callback_fn,
    void *callback_fn_cookie);

/* Flow learn notify de-registration */
pipe_status_t pipe_mgr_lrn_digest_notification_deregister(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t device_id,
    pipe_fld_lst_hdl_t flow_lrn_fld_lst_hdl);

/* Flow learn notification processing completion acknowledgment */
pipe_status_t pipe_mgr_flow_lrn_notify_ack(
    pipe_sess_hdl_t sess_hdl,
    pipe_fld_lst_hdl_t flow_lrn_fld_lst_hdl,
    pipe_flow_lrn_msg_t *pipe_flow_lrn_msg);

/* Flow learn notification set timeout */
pipe_status_t pipe_mgr_flow_lrn_set_timeout(pipe_sess_hdl_t sess_hdl,
                                            bf_dev_id_t device_id,
                                            uint32_t usecs);

/* Flow learn notification get timeout */
pipe_status_t pipe_mgr_flow_lrn_get_timeout(bf_dev_id_t device_id,
                                            uint32_t *usecs);

pipe_status_t pipe_mgr_flow_lrn_set_network_order_digest(bf_dev_id_t device_id,
                                                         bool network_order);

pipe_status_t pipe_mgr_flow_lrn_set_intr_mode(pipe_sess_hdl_t sess_hdl,
                                              bf_dev_id_t device_id,
                                              bool en);

pipe_status_t pipe_mgr_flow_lrn_get_intr_mode(pipe_sess_hdl_t sess_hdl,
                                              bf_dev_id_t device_id,
                                              bool *en);

/* Inactive node delete set */
pipe_status_t pipe_mgr_inactive_node_delete_set(pipe_sess_hdl_t sess_hdl,
                                                bf_dev_id_t device_id,
                                                bool en);

/* Inactive node delete get */
pipe_status_t pipe_mgr_inactive_node_delete_get(pipe_sess_hdl_t sess_hdl,
                                                bf_dev_id_t device_id,
                                                bool *en);

/* Selector table members sequence order set */
pipe_status_t pipe_mgr_selector_tbl_member_order_set(pipe_sess_hdl_t sess_hdl,
                                                     bf_dev_id_t device_id,
                                                     bool en);

/* Selector table members sequence order get */
pipe_status_t pipe_mgr_selector_tbl_member_order_get(pipe_sess_hdl_t sess_hdl,
                                                     bf_dev_id_t device_id,
                                                     bool *en);

bool pipe_mgr_sel_is_mode_fair(bf_dev_id_t dev_id, pipe_sel_tbl_hdl_t hdl);

/*****************************************
 * API FOR STATISTICS TABLE MANIPULATION *
 *****************************************/

/* API function to query a direct stats entry */
pipe_status_t pipe_mgr_mat_ent_direct_stat_query(pipe_sess_hdl_t sess_hdl,
                                                 bf_dev_id_t device_id,
                                                 pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                                 pipe_mat_ent_hdl_t mat_ent_hdl,
                                                 pipe_stat_data_t *stat_data);

/* API function to set/clear a direct stats entry */
pipe_status_t pipe_mgr_mat_ent_direct_stat_set(pipe_sess_hdl_t sess_hdl,
                                               bf_dev_id_t device_id,
                                               pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                               pipe_mat_ent_hdl_t mat_ent_hdl,
                                               pipe_stat_data_t *stat_data);

/* API function to load a direct stats entry */
pipe_status_t pipe_mgr_mat_ent_direct_stat_load(pipe_sess_hdl_t sess_hdl,
                                                bf_dev_id_t device_id,
                                                pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                                pipe_mat_ent_hdl_t mat_ent_hdl,
                                                pipe_stat_data_t *stat_data);

/* API function to reset a stat table */
pipe_status_t pipe_mgr_stat_table_reset(pipe_sess_hdl_t sess_hdl,
                                        dev_target_t dev_tgt,
                                        pipe_stat_tbl_hdl_t stat_tbl_hdl,
                                        pipe_stat_data_t *stat_data);

/* API function to query a stats entry */
pipe_status_t pipe_mgr_stat_ent_query(pipe_sess_hdl_t sess_hdl,
                                      dev_target_t dev_target,
                                      pipe_stat_tbl_hdl_t stat_tbl_hdl,
                                      pipe_stat_ent_idx_t *stat_ent_idx,
                                      size_t num_entries,
                                      pipe_stat_data_t **stat_data);

/* API function to set/clear a stats entry */
pipe_status_t pipe_mgr_stat_ent_set(pipe_sess_hdl_t sess_hdl,
                                    dev_target_t dev_tgt,
                                    pipe_stat_tbl_hdl_t stat_tbl_hdl,
                                    pipe_stat_ent_idx_t stat_ent_idx,
                                    pipe_stat_data_t *stat_data);

/* API function to load a stats entry (in Hardware) */
pipe_status_t pipe_mgr_stat_ent_load(pipe_sess_hdl_t sess_hdl,
                                     dev_target_t dev_tgt,
                                     pipe_stat_tbl_hdl_t stat_tbl_hdl,
                                     pipe_stat_ent_idx_t stat_idx,
                                     pipe_stat_data_t *stat_data);

/* API to trigger a stats database sync on the indirectly referenced
 * stats table.
 */
pipe_status_t pipe_mgr_stat_database_sync(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_stat_tbl_hdl_t stat_tbl_hdl,
    pipe_mgr_stat_tbl_sync_cback_fn cback_fn,
    void *cookie);

/* API to trigger a stats database sync on the directly referenced
 * stats table.
 */
pipe_status_t pipe_mgr_direct_stat_database_sync(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_mgr_stat_tbl_sync_cback_fn cback_fn,
    void *cookie);

/* API to trigger a stats entry database sync for an indirectly
 * addressed stat table.
 */
pipe_status_t pipe_mgr_stat_ent_database_sync(pipe_sess_hdl_t sess_hdl,
                                              dev_target_t dev_tgt,
                                              pipe_stat_tbl_hdl_t stat_tbl_hdl,
                                              pipe_stat_ent_idx_t stat_ent_idx);

/* API to trigger a stats entry database sync for a directly
 * addressed stat table.
 */
pipe_status_t pipe_mgr_direct_stat_ent_database_sync(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_mat_ent_hdl_t mat_ent_hdl);

/*************************************
 * API FOR METER TABLE MANIPULATION  *
 *************************************/

/* API to reset a meter table */
pipe_status_t pipe_mgr_meter_reset(pipe_sess_hdl_t sess_hdl,
                                   dev_target_t dev_tgt,
                                   pipe_meter_tbl_hdl_t meter_tbl_hdl,
                                   uint32_t pipe_api_flags);

/* API to reset a lpf table */
pipe_status_t pipe_mgr_lpf_reset(pipe_sess_hdl_t sess_hdl,
                                 dev_target_t dev_tgt,
                                 pipe_lpf_tbl_hdl_t lpf_tbl_hdl,
                                 uint32_t pipe_api_flags);

/* API to reset a wred table */
pipe_status_t pipe_mgr_wred_reset(pipe_sess_hdl_t sess_hdl,
                                  dev_target_t dev_tgt,
                                  pipe_wred_tbl_hdl_t wred_tbl_hdl,
                                  uint32_t pipe_api_flags);

/* API to update a meter entry specification */
pipe_status_t pipe_mgr_meter_ent_set(pipe_sess_hdl_t sess_hdl,
                                     dev_target_t dev_tgt,
                                     pipe_meter_tbl_hdl_t meter_tbl_hdl,
                                     pipe_meter_idx_t meter_idx,
                                     pipe_meter_spec_t *meter_spec,
                                     uint32_t pipe_api_flags);

/* API to set a meter table bytecount adjust */
pipe_status_t pipe_mgr_meter_set_bytecount_adjust(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_meter_tbl_hdl_t meter_tbl_hdl,
    int bytecount);

/* API to get a meter table bytecount adjust */
pipe_status_t pipe_mgr_meter_get_bytecount_adjust(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_meter_tbl_hdl_t meter_tbl_hdl,
    int *bytecount);

/*************************************
 * API FOR LPF MANIPULATION  *
 *************************************/
pipe_status_t pipe_mgr_lpf_ent_set(pipe_sess_hdl_t sess_hdl,
                                   dev_target_t dev_tgt,
                                   pipe_meter_tbl_hdl_t meter_tbl_hdl,
                                   pipe_lpf_idx_t lpf_idx,
                                   pipe_lpf_spec_t *lpf_spec,
                                   uint32_t pipe_api_flags);

/*************************************
 * API FOR WRED MANIPULATION  *
 *************************************/
pipe_status_t pipe_mgr_wred_ent_set(pipe_sess_hdl_t sess_hdl,
                                    dev_target_t dev_tgt,
                                    pipe_meter_tbl_hdl_t meter_tbl_hdl,
                                    pipe_wred_idx_t red_idx,
                                    pipe_wred_spec_t *wred_spec,
                                    uint32_t pipe_api_flags);

/* API to set the time that the model sees, purely for testing purposes. */
pipe_status_t pipe_mgr_model_time_advance(pipe_sess_hdl_t sess_hdl,
                                          bf_dev_id_t device_id,
                                          uint64_t tick_time);

pipe_status_t pipe_mgr_meter_read_entry(pipe_sess_hdl_t sess_hdl,
                                        dev_target_t dev_tgt,
                                        pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                        pipe_mat_ent_hdl_t mat_ent_hdl,
                                        pipe_meter_spec_t *meter_spec);

pipe_status_t pipe_mgr_meter_read_entry_idx(pipe_sess_hdl_t sess_hdl,
                                            dev_target_t dev_tgt,
                                            pipe_meter_tbl_hdl_t meter_tbl_hdl,
                                            pipe_meter_idx_t index,
                                            pipe_meter_spec_t *meter_spec,
                                            bool from_hw);

pipe_status_t pipe_mgr_lpf_read_entry(pipe_sess_hdl_t sess_hdl,
                                      dev_target_t dev_tgt,
                                      pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                      pipe_mat_ent_hdl_t mat_ent_hdl,
                                      pipe_lpf_spec_t *lpf_spec);

pipe_status_t pipe_mgr_lpf_read_entry_idx(pipe_sess_hdl_t sess_hdl,
                                          dev_target_t dev_tgt,
                                          pipe_meter_tbl_hdl_t meter_tbl_hdl,
                                          pipe_lpf_idx_t index,
                                          pipe_lpf_spec_t *lpf_spec,
                                          bool from_hw);

pipe_status_t pipe_mgr_wred_read_entry(pipe_sess_hdl_t sess_hdl,
                                       dev_target_t dev_tgt,
                                       pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                       pipe_mat_ent_hdl_t mat_ent_hdl,
                                       pipe_wred_spec_t *wred_spec);

pipe_status_t pipe_mgr_wred_read_entry_idx(pipe_sess_hdl_t sess_hdl,
                                           dev_target_t dev_tgt,
                                           pipe_meter_tbl_hdl_t meter_tbl_hdl,
                                           pipe_wred_idx_t index,
                                           pipe_wred_spec_t *wred_spec,
                                           bool from_hw);

pipe_status_t pipe_mgr_exm_entry_activate(pipe_sess_hdl_t sess_hdl,
                                          bf_dev_id_t device_id,
                                          pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                          pipe_mat_ent_hdl_t mat_ent_hdl);

pipe_status_t pipe_mgr_exm_entry_deactivate(pipe_sess_hdl_t sess_hdl,
                                            bf_dev_id_t device_id,
                                            pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                            pipe_mat_ent_hdl_t mat_ent_hdl);

/* Set the Idle timeout TTL for a given match entry */
pipe_status_t pipe_mgr_mat_ent_set_idle_ttl(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t device_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    uint32_t ttl, /*< TTL value in msecs */
    uint32_t pipe_api_flags,
    bool reset);

pipe_status_t pipe_mgr_mat_ent_reset_idle_ttl(pipe_sess_hdl_t sess_hdl,
                                              bf_dev_id_t device_id,
                                              pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                              pipe_mat_ent_hdl_t mat_ent_hdl);

/***************************
 * API FOR IDLE-TMEOUT MGMT*
 ***************************/

/* Configure idle timeout at table level */
pipe_status_t pipe_mgr_idle_get_params(pipe_sess_hdl_t sess_hdl,
                                       bf_dev_id_t device_id,
                                       pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                       pipe_idle_time_params_t *params);

pipe_status_t pipe_mgr_idle_set_params(pipe_sess_hdl_t sess_hdl,
                                       bf_dev_id_t device_id,
                                       pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                       pipe_idle_time_params_t params);

pipe_status_t pipe_mgr_idle_tmo_set_enable(pipe_sess_hdl_t sess_hdl,
                                           bf_dev_id_t device_id,
                                           pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                           bool enable);

pipe_status_t pipe_mgr_idle_tmo_get_enable(pipe_sess_hdl_t sess_hdl,
                                           bf_dev_id_t device_id,
                                           pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                           bool *enable);

pipe_status_t pipe_mgr_idle_register_tmo_cb(pipe_sess_hdl_t sess_hdl,
                                            bf_dev_id_t device_id,
                                            pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                            pipe_idle_tmo_expiry_cb cb,
                                            void *client_data);

pipe_status_t pipe_mgr_idle_register_tmo_cb_with_match_spec_copy(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t device_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_idle_tmo_expiry_cb_with_match_spec_copy cb,
    void *client_data);

/* The below APIs are used for Poll mode operation only */

/* API function to poll idle timeout data for a table entry */
pipe_status_t pipe_mgr_idle_time_get_hit_state(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t device_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    pipe_idle_time_hit_state_e *idle_time_data);

/* API function to set hit state data for a table entry */
pipe_status_t pipe_mgr_idle_time_set_hit_state(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t device_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_mat_ent_hdl_t mat_ent_hdl,
    pipe_idle_time_hit_state_e idle_time_data);

/* API function that should be called
 * periodically or on-demand prior to querying for the hit state
 * The function completes asynchronously and the client will
 * be notified of it's completion via the provided callback function
 * After fetching hit state HW values will be reset to idle.
 */
pipe_status_t pipe_mgr_idle_time_update_hit_state(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t device_id,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_idle_tmo_update_complete_cb callback_fn,
    void *cb_data);

/* The below APIs are used in notify mode */
/* API function to get the current TTL value of the table entry */
pipe_status_t pipe_mgr_mat_ent_get_idle_ttl(pipe_sess_hdl_t sess_hdl,
                                            bf_dev_id_t device_id,
                                            pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                            pipe_mat_ent_hdl_t mat_ent_hdl,
                                            uint32_t *ttl);

/***********************************************
 * API FOR STATEFUL MEMORY TABLE MANIPULATION  *
 ***********************************************/
pipe_status_t pipe_stful_ent_set(pipe_sess_hdl_t sess_hdl,
                                 dev_target_t dev_target,
                                 pipe_stful_tbl_hdl_t stful_tbl_hdl,
                                 pipe_stful_mem_idx_t stful_ent_idx,
                                 pipe_stful_mem_spec_t *stful_spec,
                                 uint32_t pipe_api_flags);

pipe_status_t pipe_stful_database_sync(pipe_sess_hdl_t sess_hdl,
                                       dev_target_t dev_tgt,
                                       pipe_stful_tbl_hdl_t stful_tbl_hdl,
                                       pipe_stful_tbl_sync_cback_fn cback_fn,
                                       void *cookie);

pipe_status_t pipe_stful_direct_database_sync(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_stful_tbl_sync_cback_fn cback_fn,
    void *cookie);

pipe_status_t pipe_stful_query_get_sizes(pipe_sess_hdl_t sess_hdl,
                                         bf_dev_id_t device_id,
                                         pipe_stful_tbl_hdl_t stful_tbl_hdl,
                                         int *num_pipes);

pipe_status_t pipe_stful_direct_query_get_sizes(pipe_sess_hdl_t sess_hdl,
                                                bf_dev_id_t device_id,
                                                pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                                int *num_pipes);

pipe_status_t pipe_stful_ent_query(pipe_sess_hdl_t sess_hdl,
                                   dev_target_t dev_tgt,
                                   pipe_stful_tbl_hdl_t stful_tbl_hdl,
                                   pipe_stful_mem_idx_t stful_ent_idx,
                                   pipe_stful_mem_query_t *stful_query,
                                   uint32_t pipe_api_flags);

pipe_status_t pipe_stful_direct_ent_query(pipe_sess_hdl_t sess_hdl,
                                          bf_dev_id_t device_id,
                                          pipe_mat_tbl_hdl_t mat_tbl_hdl,
                                          pipe_mat_ent_hdl_t mat_ent_hdl,
                                          pipe_stful_mem_query_t *stful_query,
                                          uint32_t pipe_api_flags);

pipe_status_t pipe_stful_table_reset(pipe_sess_hdl_t sess_hdl,
                                     dev_target_t dev_tgt,
                                     pipe_stful_tbl_hdl_t stful_tbl_hdl,
                                     pipe_stful_mem_spec_t *stful_spec);

pipe_status_t pipe_stful_table_reset_range(pipe_sess_hdl_t sess_hdl,
                                           dev_target_t dev_tgt,
                                           pipe_stful_tbl_hdl_t stful_tbl_hdl,
                                           pipe_stful_mem_idx_t stful_ent_idx,
                                           uint32_t num_indices,
                                           pipe_stful_mem_spec_t *stful_spec);
pipe_status_t pipe_stful_fifo_occupancy(pipe_sess_hdl_t sess_hdl,
                                        dev_target_t dev_tgt,
                                        pipe_stful_tbl_hdl_t stful_tbl_hdl,
                                        int *occupancy);
pipe_status_t pipe_stful_fifo_reset(pipe_sess_hdl_t sess_hdl,
                                    dev_target_t dev_tgt,
                                    pipe_stful_tbl_hdl_t stful_tbl_hdl);
pipe_status_t pipe_stful_fifo_dequeue(pipe_sess_hdl_t sess_hdl,
                                      dev_target_t dev_tgt,
                                      pipe_stful_tbl_hdl_t stful_tbl_hdl,
                                      int num_to_dequeue,
                                      pipe_stful_mem_spec_t *values,
                                      int *num_dequeued);
pipe_status_t pipe_stful_fifo_enqueue(pipe_sess_hdl_t sess_hdl,
                                      dev_target_t dev_tgt,
                                      pipe_stful_tbl_hdl_t stful_tbl_hdl,
                                      int num_to_enqueue,
                                      pipe_stful_mem_spec_t *values);

pipe_status_t pipe_stful_ent_query_range(pipe_sess_hdl_t sess_hdl,
                                         dev_target_t dev_tgt,
                                         pipe_stful_tbl_hdl_t stful_tbl_hdl,
                                         pipe_stful_mem_idx_t stful_ent_idx,
                                         uint32_t num_indices_to_read,
                                         pipe_stful_mem_query_t *stful_query,
                                         uint32_t *num_indices_read,
                                         uint32_t pipe_api_flags);

pipe_status_t pipe_stful_param_set(pipe_sess_hdl_t sess_hdl,
                                   dev_target_t dev_tgt,
                                   pipe_tbl_hdl_t stful_tbl_hdl,
                                   pipe_reg_param_hdl_t rp_hdl,
                                   int64_t value);

pipe_status_t pipe_stful_param_get(pipe_sess_hdl_t sess_hdl,
                                   dev_target_t dev_tgt,
                                   pipe_tbl_hdl_t stful_tbl_hdl,
                                   pipe_reg_param_hdl_t rp_hdl,
                                   int64_t *value);

pipe_status_t pipe_stful_param_reset(pipe_sess_hdl_t sess_hdl,
                                     dev_target_t dev_tgt,
                                     pipe_tbl_hdl_t stful_tbl_hdl,
                                     pipe_reg_param_hdl_t rp_hdl);

pipe_status_t pipe_stful_param_get_hdl(bf_dev_id_t dev,
                                       const char *name,
                                       pipe_reg_param_hdl_t *hdl);

/**
 * Service the instruction list completion DR
 * @param dev_id Device id
 * @return Returns @c PIPE_LLD_FAILED if the servicing fails
 *         Returns @c PIPE_SUCCESS on success.
 */
pipe_status_t bf_dma_service_pipe_ilist_completion(bf_dev_id_t dev_id);

/**
 * Service the learn DR
 * @param dev_id Device id
 * @return Returns @c PIPE_LLD_FAILED if the servicing fails
 *         Returns @c PIPE_SUCCESS on success.
 */
pipe_status_t bf_dma_service_pipe_learning(bf_dev_id_t dev_id);

/**
 * Service the idle time DR
 * @param dev_id Device id
 * @return Returns @c PIPE_LLD_FAILED if the servicing fails
 *         Returns @c PIPE_SUCCESS on success.
 */
pipe_status_t bf_dma_service_pipe_idle_time(bf_dev_id_t dev_id);

/**
 * Service the stats (lr(t)) DR
 * @param dev_id Device id
 * @return Returns @c PIPE_LLD_FAILED if the servicing fails
 *         Returns @c PIPE_SUCCESS on success.
 */
pipe_status_t bf_dma_service_pipe_stats(bf_dev_id_t dev_id);

/**
 * Service the read block completion DR
 * @param dev_id Device id
 * @return Returns @c PIPE_LLD_FAILED if the servicing fails
 *         Returns @c PIPE_SUCCESS on success.
 */
pipe_status_t bf_dma_service_pipe_read_block_completion(bf_dev_id_t dev_id);

/**
 * Service the write block completion DR
 * @param dev_id Device id
 * @return Returns @c PIPE_LLD_FAILED if the servicing fails
 *         Returns @c PIPE_SUCCESS on success.
 */
pipe_status_t bf_dma_service_pipe_write_block_completion(bf_dev_id_t dev_id);

/**
 * Get pipe-id for a particular port
 *
 * @param  dev_port_id           Port-id.
 * @return                       Pipe
 */
bf_dev_pipe_t dev_port_to_pipe_id(uint16_t dev_port_id);

/**
 * Get first entry handle
 *
 * @param  sess_hdl              Session handle
 * @param  tbl_hdl               Table handle.
 * @param  dev_tgt               Device target.
 * @param  entry_handle          Entry handle.
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_get_first_entry_handle(pipe_sess_hdl_t sess_hdl,
                                              pipe_mat_tbl_hdl_t tbl_hdl,
                                              dev_target_t dev_tgt,
                                              int *entry_handle);

/**
 * Get next entry handles
 *
 * @param  sess_hdl              Session handle
 * @param  tbl_hdl               Table handle.
 * @param  dev_tgt               Device target.
 * @param  entry_handle          Entry handle.
 * @param  n                     Num of handles
 * @param  next_entry_handles    Next Entry handles.
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_get_next_entry_handles(pipe_sess_hdl_t sess_hdl,
                                              pipe_mat_tbl_hdl_t tbl_hdl,
                                              dev_target_t dev_tgt,
                                              pipe_mat_ent_hdl_t entry_handle,
                                              int n,
                                              int *next_entry_handles);

/**
 * Get first group member
 *
 * @param  sess_hdl              Session handle
 * @param  tbl_hdl               Table handle.
 * @param  dev_id                Device ID.
 * @param  sel_grp_hdl           Group handle
 * @param  mbr_hdl               Pointer to the member handle
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_get_first_group_member(pipe_sess_hdl_t sess_hdl,
                                              pipe_tbl_hdl_t tbl_hdl,
                                              bf_dev_id_t dev_id,
                                              pipe_sel_grp_hdl_t sel_grp_hdl,
                                              pipe_adt_ent_hdl_t *mbr_hdl);

/**
 * Get next group members
 *
 * @param  sess_hdl              Session handle.
 * @param  tbl_hdl               Table handle.
 * @param  dev_id                Device ID.
 * @param  sel_grp_hdl           Group handle
 * @param  mbr_hdl               Member handle
 * @param  n                     Number of group member handles requested
 * @param  next_mbr_hdls         Array big enough to hold n member handles
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_get_next_group_members(
    pipe_sess_hdl_t sess_hdl,
    pipe_tbl_hdl_t tbl_hdl,
    bf_dev_id_t dev_id,
    pipe_sel_grp_hdl_t sel_grp_hdl,
    pipe_adt_ent_hdl_t mbr_hdl,
    int n,
    pipe_adt_ent_hdl_t *next_mbr_hdls);

pipe_status_t pipe_mgr_get_word_llp_active_member_count(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_sel_tbl_hdl_t tbl_hdl,
    uint32_t word_index,
    uint32_t *count);

pipe_status_t pipe_mgr_get_word_llp_active_members(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_sel_tbl_hdl_t tbl_hdl,
    uint32_t word_index,
    uint32_t count,
    pipe_adt_ent_hdl_t *mbr_hdls);

/**
 * Get reserved entry count for the given table
 *
 * Returns the number of reserved table locations.
 * Table locations may be reserved by the driver for uses such as
 * default entry direct resources or atomic entry modification.
 * Currently supported only for TCAM tables and can be used by applications
 * which need to know exactly how many entries can be inserted into the table.
 *
 * @param  sess_hdl              Session handle.
 * @param  dev_tgt               Device target.
 * @param  tbl_hdl               Table handle.
 * @param  count                 Pointer to the size
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_get_reserved_entry_count(pipe_sess_hdl_t sess_hdl,
                                                dev_target_t dev_tgt,
                                                pipe_mat_tbl_hdl_t tbl_hdl,
                                                size_t *count);
/**
 * Get total HW entry count for the given table.
 *
 * Returns the number of HW total entry count allocated by compiler.
 *
 * @param  sess_hdl              Session handle.
 * @param  dev_tgt               Device target.
 * @param  tbl_hdl               Table handle.
 * @param  count                 Pointer to the size
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_get_total_hw_entry_count(pipe_sess_hdl_t sess_hdl,
                                                dev_target_t dev_tgt,
                                                pipe_mat_tbl_hdl_t tbl_hdl,
                                                size_t *count);

/**
 * Get entry count for the given table
 *
 * @param  sess_hdl              Session handle.
 * @param  dev_tgt               Device target.
 * @param  tbl_hdl               Table handle.
 * @param  count                 Pointer to the entry count
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_get_entry_count(pipe_sess_hdl_t sess_hdl,
                                       dev_target_t dev_tgt,
                                       pipe_mat_tbl_hdl_t tbl_hdl,
                                       bool read_from_hw,
                                       uint32_t *count);

/**
 * Get member count for the given selector group
 *
 * @param  sess_hdl              Session handle.
 * @param  dev_id                Device id.
 * @param  tbl_hdl               Selector table handle.
 * @param  grp_hdl               Selector group handle.
 * @param  count                 Pointer to the member count.
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_get_sel_grp_mbr_count(pipe_sess_hdl_t sess_hdl,
                                             bf_dev_id_t dev_id,
                                             pipe_sel_tbl_hdl_t tbl_hdl,
                                             pipe_sel_grp_hdl_t grp_hdl,
                                             uint32_t *count);

/**
 * Get maximum selector group size and offset
 *
 * @param  sess_hdl              Session handle.
 * @param  dev_id                Device id.
 * @param  tbl_hdl               Selector table handle.
 * @param  grp_hdl               Selector group handle.
 * @param  max_size              Pointer to the size variable.
 * @param  adt_offset            Pointer to the offset variable.
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_get_sel_grp_params(pipe_sess_hdl_t sess_hdl,
                                          bf_dev_id_t dev_id,
                                          pipe_sel_tbl_hdl_t tbl_hdl,
                                          pipe_sel_grp_hdl_t grp_hdl,
                                          uint32_t *max_size,
                                          uint32_t *adt_offset);

/**
 * Get entry information
 *
 * @param  sess_hdl              Session handle
 * @param  tbl_hdl               Table handle.
 * @param  dev_tgt               Device Target. Pipe BF_DEV_PIPE_ALL indicates
 *                               that the first pipe in the scope should be
 *                               used.
 * @param  pipe_match_spec       Match spec to populate.
 * @param  pipe_action_spec      Action data spec to populate.
 * @param  act_fn_hdl            Action function handle to populate.
 * @param  from_hw               Read from HW.
 * @param  res_get_flags         Bitwise OR of PIPE_RES_GET_FLAG_xxx indicating
 *                               which direct resources should be queried.  If
 *                               an entry does not use a resource it will not be
 *                               queried even if requested.
 *                               Set to PIPE_RES_GET_FLAG_ENTRY for original
 *                               behavior.
 * @param  res_data              Pointer to a pipe_res_data_t to hold the
 *                               resource data requested by res_get_flags.  Can
 *                               be NULL if res_get_flags is zero.
 *                               Note that if PIPE_RES_GET_FLAG_STFUL is set
 *                               pipe_mgr will allocate the data in the
 *                               res_data.stful structure and the caller must
 *                               free it with bf_sys_free.
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_get_entry(pipe_sess_hdl_t sess_hdl,
                                 pipe_mat_tbl_hdl_t tbl_hdl,
                                 dev_target_t dev_tgt,
                                 pipe_mat_ent_hdl_t entry_hdl,
                                 pipe_tbl_match_spec_t *pipe_match_spec,
                                 pipe_action_spec_t *pipe_action_spec,
                                 pipe_act_fn_hdl_t *act_fn_hdl,
                                 bool from_hw,
                                 uint32_t res_get_flags,
                                 pipe_res_get_data_t *res_data);

/**
 * Get number of "n" next entries after specified handle.
 * This function doesn't return handles except for the last one. Only match,
 * action specs and all related data is returned in respective output array.
 * All memory for output arguments used should be allocated by the caller,
 * with one exception to res_data.stful, if it is not null it should be
 * freed by the caller. Allocating res_data.stful by the caller may cause
 * memory leak as pipe_mgr will overwrite that pointer if stful is fetched.
 *
 * @param  sess_hdl              Session handle
 * @param  tbl_hdl               Table handle.
 * @param  dev_tgt               Device Target. Pipe BF_DEV_PIPE_ALL indicates
 *                               that the first pipe in the scope should be
 *                               used.
 * @param  n                     Number of next entries requested. All array
 *                               arguments must be of size >= n.
 * @param  from_hw               Read from HW if true.
 * @param  res_get_flags         Bitwise OR of PIPE_RES_GET_FLAG_xxx indicating
 *                               which direct resources should be queried.  If
 *                               an entry does not use a resource it will not be
 *                               queried even if requested.
 *                               Set to PIPE_RES_GET_FLAG_ENTRY for original
 *                               behavior.
 * @param  pipe_match_spec       Pointer to the first element of match spec
 *                               array to populate.
 * @param  pipe_action_spec      Pointer to array of pointers to action data
 *                               spec to populate.
 * @param  act_fn_hdl            Pointer to the first element of action
 *                               function handle array to populate.
 * @param  res_data              Pointer to the first element of an array
 *                               of pipe_res_data_t to hold the
 *                               resource data requested by res_get_flags.  Can
 *                               be NULL if res_get_flags is zero.
 *                               Note that if PIPE_RES_GET_FLAG_STFUL is set
 *                               pipe_mgr will allocate the data in the
 *                               res_data.stful structure and the caller must
 *                               free it with bf_sys_free.
 * @param  last_ent_hdl          Output argument that holds handle of last
 *                               retrieved entry. Needed for bfrt caching.
 * @param  num_returned          Number of indexes populated in each output
 *                               array. Will be <= n.
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_get_n_next_entries(
    pipe_sess_hdl_t sess_hdl,
    pipe_mat_tbl_hdl_t tbl_hdl,
    dev_target_t dev_tgt,
    pipe_mat_ent_hdl_t entry_hdl,
    size_t n,
    bool from_hw,
    uint32_t *res_get_flags,
    pipe_tbl_match_spec_t *pipe_match_spec,
    pipe_action_spec_t **pipe_action_spec,
    pipe_act_fn_hdl_t *act_fn_hdl,
    pipe_res_get_data_t *res_data,
    pipe_mat_ent_hdl_t *last_ent_hdl,
    uint32_t *num_returned);

/**
 * Get action data entry
 *
 * @param  tbl_hdl               Table handle.
 * @param  dev_tgt               Device Target.
 * @param  pipe_action_data_spec Action data spec.
 * @param  act_fn_hdl            Action function handle.
 * @param  from_hw               Read from HW.
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_get_action_data_entry(
    pipe_adt_tbl_hdl_t tbl_hdl,
    dev_target_t dev_tgt,
    pipe_adt_ent_hdl_t entry_hdl,
    pipe_action_data_spec_t *pipe_action_data_spec,
    pipe_act_fn_hdl_t *act_fn_hdl,
    bool from_hw);

/**
 * Set the table property
 *
 * @param  sess_hdl              Session handle.
 * @param  tbl_hdl               Table handle.
 * @param  property              Property Type.
 * @param  value                 Value.
 * @param  args                  Scope args.
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_tbl_set_property(pipe_sess_hdl_t sess_hdl,
                                        bf_dev_id_t dev_id,
                                        pipe_mat_tbl_hdl_t tbl_hdl,
                                        pipe_mgr_tbl_prop_type_t property,
                                        pipe_mgr_tbl_prop_value_t value,
                                        pipe_mgr_tbl_prop_args_t args);

/**
 * Get the table property
 *
 * @param  sess_hdl              Session handle.
 * @param  tbl_hdl               Table handle.
 * @param  property              Property Type.
 * @param  value                 Value.
 * @param  args                  Scope args.
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_tbl_get_property(pipe_sess_hdl_t sess_hdl,
                                        bf_dev_id_t dev_id,
                                        pipe_mat_tbl_hdl_t tbl_hdl,
                                        pipe_mgr_tbl_prop_type_t property,
                                        pipe_mgr_tbl_prop_value_t *value,
                                        pipe_mgr_tbl_prop_args_t *args);

/**
 * Get parser engine id associated with pipe, port.
 *
 * @param  dev_id                Device ID.
 * @param  port                  port with in pipe
 * @param  parser_id             Parser engine id associated with port in the
 *pipe.
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_parser_id_get(bf_dev_id_t devid,
                                     bf_dev_port_t port,
                                     pipe_parser_id_t *parser_id);

/**
 * Set property of PVS instance. This can be use to set the gress scope
 * of the pvs instance as well as the pipe and the parser scope of the
 * pvs instance within the gresses
 *
 * @param  sess_hdl              Session handle.
 * @param  dev_id                Device ID.
 * @param  pvs_handle            Handle that identifies PVS instance.
 * @param  property 		 PVS property which can be gress or pipe or
 * 				 parser scope
 * @param  value		 PVS property value
 * @param  args 		 PVS property args which are used to specify
 *				 the gress instance in which the pipe or
 *				 parser scope property is to be set. This
 * 				 parameter is unusued when setting gress scope
 *				 property
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_pvs_set_property(pipe_sess_hdl_t sess_hdl,
                                        bf_dev_id_t dev_id,
                                        pipe_pvs_hdl_t pvs_handle,
                                        pipe_mgr_pvs_prop_type_t property,
                                        pipe_mgr_pvs_prop_value_t value,
                                        pipe_mgr_pvs_prop_args_t args);

/**
 * Get property of PVS instance. This can be use to get the gress scope
 * of the pvs instance as well as the pipe and the parser scope of the
 * pvs instance within the gresses
 *
 * @param  sess_hdl              Session handle.
 * @param  dev_id                Device ID.
 * @param  pvs_handle            Handle that identifies PVS instance.
 * @param  property 		 PVS property which can be gress or pipe or
 * 				 parser scope
 * @param  value		 PVS property value
 * @param  args 		 PVS property args which are used to specify
 *				 the gress instance in which the pipe or
 *				 parser scope property is to be fetched from.
 * 				 This parameter is unusued when getting gress
 *				 scope property
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_pvs_get_property(pipe_sess_hdl_t sess_hdl,
                                        bf_dev_id_t dev_id,
                                        pipe_pvs_hdl_t pvs_handle,
                                        pipe_mgr_pvs_prop_type_t property,
                                        pipe_mgr_pvs_prop_value_t *value,
                                        pipe_mgr_pvs_prop_args_t args);

/**
 * Set/Add parser value in parser tcam
 *
 * @param  sess_hdl              Session handle.
 * @param  dev_id                Device ID.
 * @param  pvs_handle            Handle that identifies PVS instance.
 * @param  gress		 Gress ID or all Gress
 * @param  pipeid                Pipe ID or all Pipes.
 * @param  parser_id		 Parser ID or all Parsers
 * @param  parser_value          parser value that packet parser need to match
 *                               This value is programmed into parser ML tcam.
 * @param  parser_mask           parser mask to be applied on parser value
 * @param  pvs_entry_handle      A new entry handle is allocated for the newly
 *added pvs entry
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_pvs_entry_add(pipe_sess_hdl_t sess_hdl,
                                     bf_dev_id_t devid,
                                     pipe_pvs_hdl_t pvs_handle,
                                     bf_dev_direction_t gress,
                                     bf_dev_pipe_t pipeid,
                                     uint8_t parser_id,
                                     uint32_t parser_value,
                                     uint32_t parser_value_mask,
                                     pipe_pvs_hdl_t *pvs_entry_handle);

/**
 * Modify existing parser tcam entry parser value to new value.
 *
 * @param  sess_hdl              Session handle.
 * @param  dev_id                Device ID.
 * @param  pvs_handle            Handle that identifies PVS instance.
 * @param  pvs_entry_handle      Entry Handle allocated at the time of adding
 *pvs entry
 * @param  parser_value          parser value that packet parser need to match
 *                               This value is programmed into parser ML tcam.
 * @param  parser_mask           parser mask to be applied on parser value
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_pvs_entry_modify(pipe_sess_hdl_t sess_hdl,
                                        bf_dev_id_t devid,
                                        pipe_pvs_hdl_t pvs_handle,
                                        pipe_pvs_hdl_t pvs_entry_handle,
                                        uint32_t parser_value,
                                        uint32_t parser_value_mask);

/**
 * Delete parser value entry in parser tcam.
 *
 * @param  sess_hdl              Session handle.
 * @param  dev_id                Device ID.
 * @param  pvs_handle            Handle that identifies PVS instance.
 * @param  pvs_entry_handle      Entry Handle allocated at the time of adding
 *pvs entry
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_pvs_entry_delete(pipe_sess_hdl_t sess_hdl,
                                        bf_dev_id_t devid,
                                        pipe_pvs_hdl_t pvs_handle,
                                        pipe_pvs_hdl_t pvs_entry_handle);

/**
 * Delete all parser value entries in parser tcam instance for specific
 * direction, pipe and parser.
 *
 * @param  sess_hdl              Session handle.
 * @param  dev_id                Device ID.
 * @param  pvs_handle            Handle that identifies PVS instance.
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_pvs_table_clear(pipe_sess_hdl_t sess_hdl,
                                       bf_dev_id_t devid,
                                       pipe_pvs_hdl_t pvs_handle,
                                       bf_dev_direction_t gress,
                                       bf_dev_pipe_t pipeid,
                                       uint8_t parser_id);

/**
 * Get parser value entry in parser tcam from software shadow.
 *
 * @param[in]  sess_hdl              Session handle.
 * @param[in]  dev_id                Device ID.
 * @param[in]  pvs_handle            Handle that identifies PVS instance.
 * @param[in]  pvs_entry_handle      Entry Handle allocated at the time of
 *                                   adding pvs entry
 * @param[out] parser_value          parser value return
 * @param[out] parser_value_mask     parser value mask return
 * @param[out] entry_gress           If not NULL, return the entry's gress
 * @param[out] entry_pipe            If not NULL, return the entry's pipe
 * @param[out] entry_parser_id       If not NULL, return the entry's parser-id
 * @return                           Status of the API call
 */
pipe_status_t pipe_mgr_pvs_entry_get(pipe_sess_hdl_t sess_hdl,
                                     bf_dev_id_t devid,
                                     pipe_pvs_hdl_t pvs_handle,
                                     pipe_pvs_hdl_t pvs_entry_handle,
                                     uint32_t *parser_value,
                                     uint32_t *parser_value_mask,
                                     uint8_t *entry_gress,
                                     bf_dev_pipe_t *entry_pipe,
                                     uint8_t *entry_parser_id);

/**
 * Get parser value entry in parser tcam from hardware.
 *
 * @param  sess_hdl              Session handle.
 * @param  dev_id                Device ID
 * @param  gress                 Gress ID
 * @param  pipeid                Pipe ID
 * @param  parser_id             Parser ID
 * @param  pvs_handle            Handle that identifies PVS instance.
 * @param  pvs_entry_handle      Entry Handle allocated at the time of adding
 *pvs entry
 * @param parser_value           parser value return
 * @param parser_value_mask      parser value mask return
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_pvs_entry_hw_get(pipe_sess_hdl_t sess_hdl,
                                        bf_dev_id_t devid,
                                        uint8_t gress,
                                        bf_dev_pipe_t pipeid,
                                        uint8_t parser_id,
                                        pipe_pvs_hdl_t pvs_handle,
                                        pipe_pvs_hdl_t pvs_entry_handle,
                                        uint32_t *parser_value,
                                        uint32_t *parser_value_mask);

/**
 * Get parser value entry in parser tcam.
 *
 * @param  sess_hdl              Session handle.
 * @param  dev_id                Device ID.
 * @param  pvs_handle            Handle that identifies PVS instance.
 * @param  gress                 Gress ID or all Gress
 * @param  pipeid                Pipe ID or all Pipes.
 * @param  parser_id             Parser ID or all Parsers
 * @param  parser_value          parser value that packet parser need to match
 *                               This value is programmed into parser ML tcam.
 * @param  parser_mask           parser mask to be applied on parser value
 * @param  pvs_entry_handle      A existing entry handle is return.
 *added pvs entry
 * @return                       Status of the API call
 */
pipe_status_t pipe_mgr_pvs_entry_handle_get(pipe_sess_hdl_t sess_hdl,
                                            bf_dev_id_t devid,
                                            pipe_pvs_hdl_t pvs_handle,
                                            bf_dev_direction_t gress,
                                            bf_dev_pipe_t pipeid,
                                            uint8_t parser_id,
                                            uint32_t parser_value,
                                            uint32_t parser_value_mask,
                                            pipe_pvs_hdl_t *pvs_entry_handle);

/**
 * Get the first PVS (Parser Value Set) entry handle in a PVS.
 * @param  sess_hdl     Session handle.
 * @param  dev_id       Device ID.
 * @param  pvs_handle   Handle that identifies PVS instance.
 * @param  gress        If the gress-scope of the PVS has been configured to
 *                      PIPE_MGR_PVS_SCOPE_ALL_GRESS then BF_DEV_DIR_ALL
 *                      must be passed.  If the gress has been configured to
 *                      PIPE_MGR_PVS_SCOPE_SINGLE_GRESS then any legal value of
 *                      bf_dev_direction_t can be passed to get the first
 *                      handle to allow for iteration over both ingress and
 *                      egress or iteration over a single gress.
 * @param  pipe_id      The pipe id to get the first handle from.  Similar to
 *                      the gress argument BF_DEV_PIPE_ALL must be passed if the
 *                      PVS' pipe scope is PIPE_MGR_PVS_SCOPE_ALL_PIPELINES but
 *                      any legal pipe-id can be passed otherwise to allow
 *                      iteration over all entries or entries of a specific pipe
 *                      when the PVS' scope is not pipe-all.
 * @param  parser_id    Parser ID to get the first handle from.  Similar to the
 *                      gress and pipeid arguments this must be
 *                      PIPE_MGR_PVS_PARSER_ALL if the scope is configured to
 *                      PIPE_MGR_PVS_SCOPE_ALL_PARSERS_IN_PIPE but can be any
 *                      legal value otherwise to allow iterating over all
 *                      entries or entries within a specific parser.
 * @return              Status of the API call
 */
pipe_status_t pipe_mgr_pvs_entry_get_first(pipe_sess_hdl_t sess_hdl,
                                           bf_dev_id_t dev_id,
                                           pipe_pvs_hdl_t pvs_handle,
                                           bf_dev_direction_t gress,
                                           bf_dev_pipe_t pipe_id,
                                           uint8_t parser_id,
                                           pipe_pvs_hdl_t *entry_handle);
/**
 * Get the next n PVS (Parser Value Set) entry handle in a PVS.
 * @param  sess_hdl     Session handle.
 * @param  dev_id       Device ID.
 * @param  pvs_handle   Handle that identifies PVS instance.
 * @param  gress        If the gress-scope of the PVS has been configured to
 *                      PIPE_MGR_PVS_SCOPE_ALL_GRESS then BF_DEV_DIR_ALL
 *                      must be passed.  If the gress has been configured to
 *                      PIPE_MGR_PVS_SCOPE_SINGLE_GRESS then any legal value of
 *                      bf_dev_direction_t can be passed to get the first
 *                      handle to allow for iteration over both ingress and
 *                      egress or iteration over a single gress.
 * @param  pipe_id      The pipe id to get the next handle from.  Similar to
 *                      the gress argument BF_DEV_PIPE_ALL must be passed if the
 *                      PVS' pipe scope is PIPE_MGR_PVS_SCOPE_ALL_PIPELINES but
 *                      any legal pipe-id can be passed otherwise to allow
 *                      iteration over all entries or entries of a specific pipe
 *                      when the PVS' scope is not pipe-all.
 * @param  parser_id    Parser ID to get the next handle from.  Similar to the
 *                      gress and pipeid arguments this must be
 *                      PIPE_MGR_PVS_PARSER_ALL if the scope is configured to
 *                      PIPE_MGR_PVS_SCOPE_ALL_PARSERS_IN_PIPE but can be any
 *                      legal value otherwise to allow iterating over all
 *                      entries or entries within a specific parser.
 * @param entry_handle  The entry handle from which to resume the search.
 * @param n             The number of handles to return.
 * @param next_handles  Pointer to an array of at least n handles.
 * @return              Status of the API call
 */
pipe_status_t pipe_mgr_pvs_entry_get_next(pipe_sess_hdl_t sess_hdl,
                                          bf_dev_id_t dev_id,
                                          pipe_pvs_hdl_t pvs_handle,
                                          bf_dev_direction_t gress,
                                          bf_dev_pipe_t pipe_id,
                                          uint8_t parser_id,
                                          pipe_pvs_hdl_t entry_handle,
                                          int n,
                                          pipe_pvs_hdl_t *next_handles);

/**
 * Get entry count for the given PVS
 *
 * @param  sess_hdl     Session handle.
 * @param  dev_id       Device ID.
 * @param  pvs_handle   Handle that identifies PVS instance.
 * @param  gress        Specify the gress to query.
 * @param  pipe_id      Specify the pipe to query.
 * @param  parser_id    Specify the parser to query.
 * @param  count        Pointer to the entry count
 * @return              Status of the API call
 */
pipe_status_t pipe_mgr_pvs_entry_get_count(pipe_sess_hdl_t sess_hdl,
                                           bf_dev_id_t dev_id,
                                           pipe_pvs_hdl_t pvs_handle,
                                           bf_dev_direction_t gress,
                                           bf_dev_pipe_t pipe_id,
                                           uint8_t parser_id,
                                           bool read_from_hw,
                                           uint32_t *count);
/* Hash Calculation APIs */
pipe_status_t pipe_mgr_hash_calc_input_set(pipe_sess_hdl_t sess_hdl,
                                           bf_dev_id_t dev_id,
                                           pipe_hash_calc_hdl_t handle,
                                           pipe_fld_lst_hdl_t fl_handle);
pipe_status_t pipe_mgr_hash_calc_input_default_set(pipe_sess_hdl_t sess_hdl,
                                                   bf_dev_id_t dev_id,
                                                   pipe_hash_calc_hdl_t handle);

pipe_status_t pipe_mgr_hash_calc_input_get(pipe_sess_hdl_t sess_hdl,
                                           bf_dev_id_t dev_id,
                                           pipe_hash_calc_hdl_t handle,
                                           pipe_fld_lst_hdl_t *fl_handle);

pipe_status_t pipe_mgr_hash_calc_input_field_attribute_set(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t dev_id,
    pipe_hash_calc_hdl_t handle,
    pipe_fld_lst_hdl_t fl_handle,
    uint32_t attr_count,
    pipe_hash_calc_input_field_attribute_t *attr_list);

pipe_status_t pipe_mgr_hash_calc_input_field_attribute_get(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t dev_id,
    pipe_hash_calc_hdl_t handle,
    pipe_fld_lst_hdl_t fl_handle,
    uint32_t max_attr_count,
    pipe_hash_calc_input_field_attribute_t *attr_list,
    uint32_t *num_attr_filled);

/**
 * @brief Same as pipe_mgr_hash_calc_input_field_attribute_get() but
 * allocates correct amount of memory for attr_list too.
 * User is needed to call pipe_mgr_hash_calc_attribute_list_destroy
 * to destroy the memory.
 *
 * @param[in] sess_hdl Session handle
 * @param[in] dev_id Device ID
 * @param[in] handle Hash handle
 * @param[in] fl_handle Field List handle
 * @param[out] attr_list Attribute list
 * @param[out] num_attr_filled Number of attributes filled
 *
 * @return Status of API call
 */
pipe_status_t pipe_mgr_hash_calc_input_field_attribute_2_get(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t dev_id,
    pipe_hash_calc_hdl_t handle,
    pipe_fld_lst_hdl_t fl_handle,
    pipe_hash_calc_input_field_attribute_t **attr_list,
    uint32_t *num_attr_filled);

pipe_status_t pipe_mgr_hash_calc_attribute_list_destroy(
    pipe_hash_calc_input_field_attribute_t *attr_list);

pipe_status_t pipe_mgr_hash_calc_input_field_attribute_count_get(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t dev_id,
    pipe_hash_calc_hdl_t handle,
    pipe_fld_lst_hdl_t fl_handle,
    uint32_t *attr_count);

pipe_status_t pipe_mgr_hash_calc_algorithm_set(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t dev_id,
    pipe_hash_calc_hdl_t handle,
    pipe_hash_alg_hdl_t al_handle,
    const bfn_hash_algorithm_t *algorithm,
    uint64_t rotate);

pipe_status_t pipe_mgr_hash_calc_algorithm_get(pipe_sess_hdl_t sess_hdl,
                                               bf_dev_id_t dev_id,
                                               pipe_hash_calc_hdl_t handle,
                                               pipe_hash_alg_hdl_t *al_handle,
                                               bfn_hash_algorithm_t *algorithm,
                                               uint64_t *rotate);

pipe_status_t pipe_mgr_hash_calc_algorithm_reset(pipe_sess_hdl_t sess_hdl,
                                                 bf_dev_id_t dev_id,
                                                 pipe_hash_calc_hdl_t handle);

pipe_status_t pipe_mgr_hash_calc_seed_set(pipe_sess_hdl_t sess_hdl,
                                          bf_dev_id_t dev_id,
                                          pipe_hash_calc_hdl_t handle,
                                          pipe_hash_seed_t seed);

pipe_status_t pipe_mgr_hash_calc_seed_get(pipe_sess_hdl_t sess_hdl,
                                          bf_dev_id_t dev_id,
                                          pipe_hash_calc_hdl_t handle,
                                          pipe_hash_seed_t *seed);

pipe_status_t pipe_mgr_hash_calc_calculate_hash_value(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t dev_id,
    pipe_hash_calc_hdl_t handle,
    uint8_t *stream,
    uint32_t stream_len,
    uint8_t *hash,
    uint32_t hash_len);

/**
 * @brief Used to fetch the hash value which will be computed
 * by the hash object during runtime with certain values of attributes.
 * It only calculates for entire fields and not slices so slice
 * related fields in pipe_hash_calc_input_field_attribute_t are
 * ignored.
 * Output is copied in hash array in Network order
 *
 * @param[in]  sess_hdl Session handle
 * @param[in]  dev_id Device ID
 * @param[in]  handle Hash handle
 * @param[in]  attr_count Size of attr_list
 * @param[in]  attrs Attribute list
 * @param[in]  hash_len Length of hash array
 * @param[out] hash Hash value byte array
 *
 * @return Status of API call
 */
pipe_status_t pipe_mgr_hash_calc_calculate_hash_value_with_cfg(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t dev_id,
    pipe_hash_calc_hdl_t handle,
    uint32_t attr_count,
    pipe_hash_calc_input_field_attribute_t *attrs,
    uint32_t hash_len,
    uint8_t *hash);

/*  ---- Table debug counter APIs start  ---- */

/**
 * The function be used to set the counter type for table
   debug counter.
 *
 * @param  dev_tgt               ASIC Device information.
 * @param  tbl_name              Table name.
 * @param  type                  Type.
 * @return                       Status of the API call
 */
pipe_status_t bf_tbl_dbg_counter_type_set(dev_target_t dev_tgt,
                                          char *tbl_name,
                                          bf_tbl_dbg_counter_type_t type);

/**
 * The function be used to set the counter type for table
   debug counter.
 *
 * @param  dev_tgt               ASIC Device information.
 * @param  stage                 Stage to operate on.
 * @param  log_tbl_id            Logical table id in stage.
 * @param  type                  Type.
 * @return                       Status of the API call
 */
pipe_status_t bf_log_tbl_dbg_counter_type_set(dev_target_t dev_tgt,
                                              uint32_t stage,
                                              uint32_t log_tbl_id,
                                              bf_tbl_dbg_counter_type_t type);

/**
 * The function be used to get the counter value for table
   debug counter.
 *
 * @param  dev_tgt               ASIC Device information.
 * @param  tbl_name              Table name.
 * @param  type                  Type.
 * @param  value                 Value.
 * @return                       Status of the API call
 */
pipe_status_t bf_tbl_dbg_counter_get(dev_target_t dev_tgt,
                                     char *tbl_name,
                                     bf_tbl_dbg_counter_type_t *type,
                                     uint32_t *value);

/**
 * The function be used to get the counter value for table
   debug counter.
 *
 * @param  dev_tgt               ASIC Device information.
 * @param  stage                 Stage to operate on.
 * @param  log_tbl_id            Logical table id in stage.
 * @param  type                  Type.
 * @param  value                 Value.
 * @return                       Status of the API call
 */
pipe_status_t bf_log_tbl_dbg_counter_get(dev_target_t dev_tgt,
                                         uint32_t stage,
                                         uint32_t log_tbl_id,
                                         bf_tbl_dbg_counter_type_t *type,
                                         uint32_t *value);

/**
 * The function be used to clear the counter value for table
   debug counter.
 *
 * @param  dev_tgt               ASIC Device information.
 * @param  tbl_name              Table name.
 * @return                       Status of the API call
 */
pipe_status_t bf_tbl_dbg_counter_clear(dev_target_t dev_tgt, char *tbl_name);

/**
 * The function be used to clear the counter value for table
   debug counter.
 *
 * @param  dev_tgt               ASIC Device information.
 * @param  stage                 Stage number.
 * @param  log_tbl_id            Logical table id in stage.
 * @return                       Status of the API call
 */
pipe_status_t bf_log_tbl_dbg_counter_clear(dev_target_t dev_tgt,
                                           uint32_t stage,
                                           uint32_t log_tbl_id);

/**
 * The function be used to get list of tables present in the system. Function
 * automatically allocates required amount of memory to store tables names as
 * strings. Caller is responsible to free the memory. If pointer is set to non
 * NULL value then it should be freed.
 *
 * @param  dev_tgt[in]           ASIC Device information.
 * @param  tbl_names[out]        Pointer to array of c strings where allocated
 *                               memory pointer will be stored.
 * @param  num_tbl[out]          Amount of tables returned.
 * @return                       Status of the API call
 */
bf_status_t pipe_mgr_tbl_dbg_counter_get_list(dev_target_t dev_tgt,
                                              char **tbl_names,
                                              int *num_tbl);

/**
 * The function be used to set the debug counter type for all tables
   in a stage
 *
 * @param  dev_tgt               ASIC Device information.
 * @param  stage_id              Stage.
 * @param  type                  Counter type.
 * @return                       Status of the API call
 */
pipe_status_t bf_tbl_dbg_counter_type_stage_set(dev_target_t dev_tgt,
                                                dev_stage_t stage_id,
                                                bf_tbl_dbg_counter_type_t type);

/**
 * The function be used to get the debug counter value for all tables
   in a stage
 *
 * @param  dev_tgt               ASIC Device information.
 * @param  stage_id              Stage.
 * @param  type_arr              List of counter types.
 * @param  value_arr             List of counter values.
 * @param  tbl_name              List of table names.
 * @param  num_counters          Number of counters.
 * @return                       Status of the API call
 */
#define PIPE_MGR_TBL_NAME_LEN 200
pipe_status_t bf_tbl_dbg_counter_stage_get(
    dev_target_t dev_tgt,
    dev_stage_t stage_id,
    bf_tbl_dbg_counter_type_t *type_arr,
    uint32_t *value_arr,
    char tbl_name[][PIPE_MGR_TBL_NAME_LEN],
    int *num_counters);

/**
 * The function be used to clear the debug counter value for all tables
   in a stage
 *
 * @param  dev_tgt               ASIC Device information.
 * @param  stage_id              Stage.
 * @return                       Status of the API call
 */
pipe_status_t bf_tbl_dbg_counter_stage_clear(dev_target_t dev_tgt,
                                             dev_stage_t stage_id);

/*  ---- Table debug counter APIs end  ---- */

/*  ---- Snapshot APIs start  ---- */

typedef bf_status_t (*bf_snapshot_triggered_cb)(bf_dev_id_t dev_id,
                                                bf_dev_pipe_t pipe_id,
                                                pipe_snapshot_hdl_t hdl);
/**
 * The function be used to set the monitoring mode for snapshot
   Also, register callback function for snapshot
 *
 * @param[in] dev_id                ASIC device identifier
 * @param[in] interrupt_or_polling  True for interrupt, false for polling
 * @param[in] trig_cb               Callback API
 * @return                          Status of the API call
 */
bf_status_t bf_snapshot_monitoring_mode(bf_dev_id_t dev_id,
                                        bool interrupt_or_polling,
                                        bf_snapshot_triggered_cb trig_cb);

/**
 * The function be used to poll the snapshots
 * Callback is given if a snapshot has been triggered
 *
 * @param[in] dev_id             ASIC device identifier
 * @return                       Status of the API call
 */
bf_status_t bf_snapshot_do_polling(bf_dev_id_t dev);

/**
 * The function be used to create the snapshot
 *
 * @param[in] dev                ASIC device identifier
 * @param[in] pipe               Pipe
 * @param[in] start_stage        Snapshot start stage
 * @param[in] end_stage          Snapshot end stage
 * @param[in] dir                Direction (ingress/egress)
 * @param[out] hdl               Snapshot handle
 * @return                       Status of the API call
 */
bf_status_t bf_snapshot_create(bf_dev_id_t dev,
                               bf_dev_pipe_t pipe,
                               dev_stage_t start_stage,
                               dev_stage_t end_stage,
                               bf_snapshot_dir_t dir,
                               pipe_snapshot_hdl_t *hdl);

/**
 * The function be used to get the snapshot handle
 *
 * @param[in] dev                ASIC device identifier
 * @param[in] pipe               Pipe
 * @param[in] start_stage        Snapshot start stage
 * @param[in] end_stage          Snapshot end stage
 * @param[out] dir               Direction (ingress/egress)
 * @param[out] hdl               Snapshot handle
 * @return                       Status of the API call
 */
bf_status_t bf_snapshot_handle_get(bf_dev_id_t dev,
                                   bf_dev_pipe_t pipe,
                                   dev_stage_t start_stage,
                                   dev_stage_t end_stage,
                                   bf_snapshot_dir_t *dir,
                                   pipe_snapshot_hdl_t *hdl);
/**
 * The function be used to get specified number of next snapshot handles.
 * Only handles on the same pipe will be returned if handle is provided.
 * If provided handle is equal to zero, this function will reutrn all
 * available handles up to the specified size.
 *
 * @param[in] dev               ASIC device identifier
 * @param[in] hdl               Snapshot handle
 * @param[out] n                Size of the output array
 * @param[out] next_handles     Array to store the next entry handles
 * @return                      Status of the API call
 */
bf_status_t bf_snapshot_next_handles_get(bf_dev_id_t dev,
                                         pipe_snapshot_hdl_t hdl,
                                         int n,
                                         int *next_handles);

/**
 * The function be used to delete the snapshot
 *
 * @param[in] hdl             Snapshot handle
 * @return                    Status of the API call
 */
bf_status_t bf_snapshot_delete(pipe_snapshot_hdl_t hdl);

/**
 * The function be used to clear all the snapshot entries
 *
 * @param[in] dev             ASIC device identifier
 * @return                    Status of the API call
 */
bf_status_t bf_snapshot_clear(bf_dev_id_t dev);

/**
 * The function be used to set all required fields
    for capture trigger
 *
 * @param[in] hdl             Snapshot handle
 * @param[in] trig_spec       Trig Spec
 * @param[in] trig_mask       Trig Mask
 * @return                    Status of the API call
 */
bf_status_t bf_snapshot_capture_trigger_set(pipe_snapshot_hdl_t hdl,
                                            void *trig_spec,
                                            void *trig_mask);

/**
 * The function be used to add one snapshot capture trigger
 * field to the trigger list
 *
 * @param[in] hdl             Snapshot handle
 * @param[in] field_name      Name of field
 * @param[in] value           Field value
 * @param[in] mask            Field mask
 * @return                    Status of the API call
 */
bf_status_t bf_snapshot_capture_trigger_field_add(pipe_snapshot_hdl_t hdl,
                                                  char *field_name,
                                                  uint64_t value,
                                                  uint64_t mask);

#define BF_SNAPSHOT_FIELD_NAME_LEN 100

/**
 * Snapshot trigger field specification.  Passed by value to avoid
 * pointer lifetime concerns across FFI boundaries.
 *
 * value and mask are big-endian byte arrays: byte[0] is the MSB.
 */
typedef struct bf_snapshot_trigger_field {
  uint8_t value[16];
  uint8_t mask[16];
  char name[BF_SNAPSHOT_FIELD_NAME_LEN];
} bf_snapshot_trigger_field_t;

/**
 * Add one snapshot capture trigger field using a by-value struct.
 * Unlike bf_snapshot_capture_trigger_field_add, this variant supports
 * fields wider than 64 bits (e.g. IPv6 addresses).
 *
 * @param[in] hdl             Snapshot handle
 * @param[in] field           Trigger field (name, value, mask)
 * @return                    Status of the API call
 */
bf_status_t bf_snapshot_capture_trigger_field_add_bytes(
    pipe_snapshot_hdl_t hdl,
    bf_snapshot_trigger_field_t field);

/**
 * The function be used to get trigger field value and mask.
 *
 * @param[in] hdl             Snapshot handle
 * @param[in] field_name      Name of field
 * @param[out] value          Field value
 * @param[out] mask           Field mask
 * @return                    Status of the API call
 */
pipe_status_t bf_snapshot_capture_trigger_field_get(pipe_snapshot_hdl_t hdl,
                                                    char *field_name,
                                                    uint64_t *value,
                                                    uint64_t *mask);
/**
 * The function be used to clear all snapshot capture trigger fields
 *
 * @param[in] hdl             Snapshot handle
 * @return                    Status of the API call
 */
bf_status_t bf_snapshot_capture_trigger_fields_clr(pipe_snapshot_hdl_t hdl);

/**
 * The function be used to get the size of the capture data that needs to
 * be allocated
 *
 * @param[in] hdl               Snapshot handle
 * @param[out] total_size       Field value
 * @param[out] per_stage_size   Field value
 * @return                      Status of the API call
 */
bf_status_t bf_snapshot_capture_phv_fields_dict_size(pipe_snapshot_hdl_t hdl,
                                                     uint32_t *total_size,
                                                     uint32_t *per_stage_size);

/**
 * The function be used to get the snapshot capture
 *
 * @param[in] hdl                Snapshot handle
 * @param[in] pipe               Pipe
 * @param[out] capture           Captures for all stages in snapshot
 * @param[out] num_captures      Num of Captures
 * @return                       Status of the API call
 */
bf_status_t bf_snapshot_capture_get(
    pipe_snapshot_hdl_t hdl,
    bf_dev_pipe_t pipe,
    uint8_t *capture,
    bf_snapshot_capture_ctrl_info_arr_t *capture_ctrl_arr,
    int *num_captures);

/**
 * The function be used to decode the snapshot capture and get the value of a
 * field
 *
 * @param[in] hdl                Snapshot handle
 * @param[in] pipe               Pipe
 * @param[in] stage              Stage
 * @param[in] capture            Captures for all stages in snapshot
 * @param[in] num_captures       Num of Captures
 * @param[in] field_name         Name of field
 * @param[out] field_value       Value of field
 * @param[out] field_valid       Defines if field value is valid
 * @return                       Status of the API call
 */
bf_status_t bf_snapshot_capture_decode_field_value(pipe_snapshot_hdl_t hdl,
                                                   bf_dev_pipe_t pipe,
                                                   dev_stage_t stage,
                                                   uint8_t *capture,
                                                   int num_captures,
                                                   char *field_name,
                                                   uint64_t *field_value,
                                                   bool *field_valid);

bf_status_t bf_snapshot_capture_decode_field_value_bytes(
    pipe_snapshot_hdl_t hdl,
    bf_dev_pipe_t pipe,
    dev_stage_t stage,
    uint8_t *capture,
    int num_captures,
    char *field_name,
    uint8_t *out_value,
    int out_len,
    int *out_width,
    bool *field_valid);

/**
 * The function be used to set the snapshot state
 *
 * @param[in] hdl                Snapshot handle
 * @param[in] state              Snapshot state (enable/disable)
 * @param[in] usec               Timeout in micro-sec
 * @return                       Status of the API call
 */
bf_status_t bf_snapshot_state_set(pipe_snapshot_hdl_t hdl,
                                  bf_snapshot_state_t state,
                                  uint32_t usec);

/**
 * The function be used to get the snapshot state
 *
 * @param[in] hdl                Snapshot handle
 * @param[in] pipe               Pipe
 * @param[out] state             Snapshot state (enable/disable)
 * @return                       Status of the API call
 */
bf_status_t bf_snapshot_pd_state_get(pipe_snapshot_hdl_t hdl,
                                     bf_dev_pipe_t pipe,
                                     int *state);

/**
 * The function be used to get the snapshot state
 *
 * @param[in] hdl                Snapshot handle
 * @param[in] size               Size of the output arrays
 * @param[out] trigger_state     Snapshot trigger state array
 * @param[out] state             Snapshot state array
 * @return                       Status of the API call
 */
bf_status_t bf_snapshot_state_get(pipe_snapshot_hdl_t hdl,
                                  uint32_t size,
                                  pipe_snapshot_fsm_state_t *fsm_state,
                                  bool *en_state);

/**
 * The function be used to set snapshot config parameters
 * Ingress trigger mode does not affect snapshoting behavior for egress
 * snapshots, but can still be set. Any non "ingress" (default) configuration
 * is supported only by supported by Tofino-2.
 *
 * @param[in] hdl            Snapshot handle
 * @param[in] timer_disable  Timer state (enable/disable)
 * @param[in] mode           Trigger ingress mode
 * @return                   Status of the API call
 */
bf_status_t bf_snapshot_cfg_set(pipe_snapshot_hdl_t hdl,
                                bool timer_disable,
                                bf_snapshot_ig_mode_t mode);

/**
 * The function gets snapshot config parameters
 *
 * @param[in] hdl            Snapshot handle
 * @param[out] timer_enable  State of timer trigger
 * @param[out] usec          Configured timer value
 * @param[out] mode          Trigger ingress mode
 * @return                   Status of the API call
 */
bf_status_t bf_snapshot_cfg_get(pipe_snapshot_hdl_t hdl,
                                bool *timer_enable,
                                uint32_t *usec,
                                bf_snapshot_ig_mode_t *mode);
/**
 * The function gets snapshot captured threads bitmask per pipe. Will return an
 * BF_INVALID_ARG error if provided array won't fit response for all requested
 * pipes.
 *
 * @param[in] hdl           Snapshot handle
 * @param[in] size          Length of the provided array
 * @param[out] threads      Array of bitmask values representing captured
 *                          threads per pipe.
 * @return                  Status of the API call
 */
bf_status_t bf_snapshot_capture_thread_get(pipe_snapshot_hdl_t hdl,
                                           uint32_t size,
                                           int *threads);

/**
 * This function is used to check if a snapshot capture field is in scope
 * for a particular stage. To optimize the PHV allocation the P4 compiler may
 * only allocate PHV resources to a field in some stages, this function allows
 * the caller to determine if the field is valid to be read in the requested
 * stage.
 *
 * @param[in] dev            ASIC device identifier
 * @param[in] pipe           Pipe
 * @param[in] stage          Snapshot start stage
 * @param[in] dir            Direction (ingress/egress)
 * @param[in] field_name     Field name
 * @param[out] exists        Field exists
 * @return                   Status of the API call
 */
bf_status_t bf_snapshot_field_in_scope(bf_dev_id_t dev,
                                       bf_dev_pipe_t pipe,
                                       dev_stage_t stage,
                                       bf_snapshot_dir_t dir,
                                       char *field_name,
                                       bool *exists);
/**
 * This function can be used to test whether a field can be used in the
 * snapshot trigger for the given stage. On Tofino-2 and later, even if a field
 * is in scope it may not be available to trigger the snapshot so this function
 * should be used for fields in the trigger while bf_snapshot_field_in_scope
 * can be used when querying fields in the captured data.
 *
 * @param[in] dev            ASIC device identifier
 * @param[in] pipe           Pipe
 * @param[in] stage          Snapshot start stage
 * @param[in] dir            Direction (ingress/egress)
 * @param[in] field_name     Field name
 * @param[out] exists        Field exists
 * @return                   Status of the API call
 */
bf_status_t bf_snapshot_trigger_field_in_scope(bf_dev_id_t dev,
                                               bf_dev_pipe_t pipe,
                                               dev_stage_t stage,
                                               bf_snapshot_dir_t dir,
                                               char *field_name,
                                               bool *exists);
/**
 * The function to be used in order to get snapshot entry
 * parameters such as stages, direction and pipe.
 * All output arguments are optional, specify NULL to ignore.
 *
 * @param[in] hdl           Snapshot handle
 * @param[out] dev          ASIC device identifier
 * @param[out] pipe         Pipe
 * @param[out] s_stage      Snapshot start stage
 * @param[out] e_stage      Snapshot start stage
 * @param[out] dir          Direction (ingress/egress)
 * @return                  Status of the API call
 */
bf_status_t bf_snapshot_entry_params_get(pipe_snapshot_hdl_t hdl,
                                         bf_dev_id_t *dev,
                                         bf_dev_pipe_t *pipe,
                                         dev_stage_t *s_stage,
                                         dev_stage_t *e_stage,
                                         bf_snapshot_dir_t *dir);

/**
 * The function is used to get first handle of snapshot that maches specified
 * pipe.
 *
 * @param[in] dev_id            The ASIC id.
 * @param[in] pipe              Pipeline.
 * @param[out] entry_hdl        Entry handle point to store first handle.
 *
 * @return Status of the API call.
 */
pipe_status_t bf_snapshot_first_handle_get(bf_dev_id_t dev,
                                           bf_dev_pipe_t pipe,
                                           pipe_snapshot_hdl_t *entry_hdl);

/**
 * The function is used to get number of entries present for specific pipe.
 *
 * @param[in] hdl      Snapshot handle
 * @param[in] pipe     Pipe
 * @param[out] count    Array to put stage numbers in.
 *
 * @return Status of the API call.
 */
bf_status_t bf_snapshot_usage_get(bf_dev_id_t dev,
                                  bf_dev_pipe_t pipe,
                                  uint32_t *count);
/**
 * The function is used to get number of trigger fields set in snapshot.
 *
 * @param[in] hdl       Snapshot handle
 * @param[out] count    Array to put stage numbers in.
 *
 * @return Status of the API call.
 */
pipe_status_t bf_snapshot_num_trig_fields_get(pipe_snapshot_hdl_t hdl,
                                              int *count);

/**
 * The function is used to fetch raw PHV values for specified
 * snapshot pipe and stage. For array allocation bf_snapshot_total_phv_count_get
 * function can be called to get required length.
 *
 * Note: Output arrays must be equal in length.
 *
 * @param[in] dev_id    The ASIC id.
 * @param[in] pipe      Pipeline
 * @param[in] stage     Stage
 * @param[in] size      Length of the provided arrays
 * @param[out] phvs     Array to store PHV values - one index per PHV.
 * @param[out] phvs_v   Array to store validity bit of respective PHV value.
 *
 * @return Status of the API call.
 */
pipe_status_t bf_snapshot_raw_capture_get(bf_dev_id_t dev_id,
                                          bf_dev_pipe_t pipe,
                                          dev_stage_t stage,
                                          uint32_t size,
                                          uint32_t *phvs,
                                          bool *phvs_v);

/**
 * The function is used to get total number of PHV containers available
 * in snapshot.
 *
 * @param[in] dev_id    The ASIC id.
 * @param[out] count    Total number of PHV containers.
 *
 * @return Status of the API call.
 */
pipe_status_t bf_snapshot_total_phv_count_get(bf_dev_id_t dev_id,
                                              uint32_t *count);

/**
 * The function is used to get list of stages configured by any snapshot
 * instances for the specified pipe. Function will fill provided array with
 * stage numbers in order and setting unused indexes to "-1".
 *
 * @param[in] dev_id    The ASIC id.
 * @param[in] pipe      Pipeline.
 * @param[in] size      Length of the output array provided.
 * @param[out] stages   Array to put stage numbers in.
 *
 * @return Status of the API call.
 */
pipe_status_t bf_snapshot_stages_get(bf_dev_id_t dev,
                                     bf_dev_pipe_t pipe,
                                     uint32_t size,
                                     int *stages);

/*  ---- Snapshot APIs end  ---- */

/************ IBUF, EBUF, PARB APIs **************/
/**
 * The function be used to enable buffer full notification to parser when ibuf
 * usage is above low_wm_bytes
 *
 * @param  dev_id             ASIC device identifier
 * @param  port_id            Port identifier
 * @param  low_wm_bytes       Threshold level in bytes above which notification
 *                            sent.
 * @param  hi_wm_bytes        Threshold level in bytes above which drop
 *                            occurs.
 * @return                    Status of the API call
 */
pipe_status_t bf_pipe_enable_ibuf_congestion_notif_to_parser(
    bf_dev_id_t dev_id,
    bf_dev_port_t port_id,
    uint16_t low_wm_bytes,
    uint16_t hi_wm_bytes);
/**
 * The function be used to enable buffer full notification to mac when ibuf
 * usage is above low_wm_bytes
 *
 * @param  dev_id             ASIC device identifier
 * @param  port_id            Port identifier
 * @param  low_wm_bytes       Threshold level in bytes above which notification
 *                            sent.
 * @param  hi_wm_bytes        Threshold level in bytes above which drop
 *                            occurs.
 * @return                    Status of the API call
 */
pipe_status_t bf_pipe_parb_enable_flow_control_to_mac(bf_dev_id_t dev_id,
                                                      bf_dev_port_t port_id,
                                                      uint16_t low_wm_bytes,
                                                      uint16_t hi_wm_bytes);

/**
 * The function be used to disable buffer full notification to parser
 *
 * @param  dev_id             ASIC device identifier
 * @param  port_id            Port identifier
 * @return                    Status of the API call
 */
pipe_status_t bf_pipe_disable_ibuf_congestion_notif_to_parser(
    bf_dev_id_t dev_id, bf_dev_port_t port_id);

/*
 * The function be used to disable buffer full notification to mac
 *
 * @param  dev_id             ASIC device identifier
 * @param  port_id            Port identifier
 * @return                    Status of the API call
 */
pipe_status_t bf_pipe_parb_disable_flow_control_to_mac(bf_dev_id_t dev_id,
                                                       bf_dev_port_t port_id);

/*
 * The function be used to set high priority port arbitration.
 * Setting arbitration priority high helps to avoid port starvation.
 * Recommended to use this setting only on CPU bound port.
 *
 * @param  dev_id             ASIC device identifier
 * @param  port_id            Port identifier
 * @return                    Status of the API call
 */
pipe_status_t bf_pipe_enable_port_arb_priority_high(bf_dev_id_t dev_id,
                                                    bf_dev_port_t port_id);

/*
 * The function be used to set normal priority port arbitration.
 * Recommended to use this setting on all ports expect CPU bound port.
 * Default setting arbitrates port at normal priority. Hence no need
 * to invoke this function for default behaviour.
 *
 * @param  dev_id             ASIC device identifier
 * @param  port_id            Port identifier
 * @return                    Status of the API call
 */
pipe_status_t bf_pipe_enable_port_arb_priority_normal(bf_dev_id_t dev_id,
                                                      bf_dev_port_t port_id);

/************ Hitless HA State restore APIs **************/

typedef void (*pd_ha_restore_cb_1)(pipe_sess_hdl_t sess_hdl,
                                   bf_dev_id_t dev_id,
                                   pipe_adt_tbl_hdl_t adt_tbl_hdl,
                                   pipe_act_fn_hdl_t act_hdl,
                                   pipe_sel_grp_hdl_t grp_hdl,
                                   pipe_adt_ent_hdl_t mbr_hdl,
                                   int num_resources,
                                   adt_data_resources_t *resources);

/*
 * This function is used to restore the software state at the virtual
 * device.
 */
pipe_status_t pipe_mgr_hitless_ha_restore_virtual_dev_state(
    pipe_sess_hdl_t sess_hdl,
    bf_dev_id_t dev_id,
    pipe_tbl_hdl_t tbl_hdl,
    struct pipe_plcmt_info *info,
    uint32_t *processed,
    pd_ha_restore_cb_1 cb1);

pipe_status_t pipe_set_adt_ent_hdl_in_mat_data(void *data,
                                               pipe_adt_ent_hdl_t adt_ent_hdl);

pipe_status_t pipe_set_sel_grp_hdl_in_mat_data(void *data,
                                               pipe_adt_ent_hdl_t sel_grp_hdl);

pipe_status_t pipe_set_ttl_in_mat_data(void *data, uint32_t ttl);

/* Stop the TCAM scrub timer. */
void pipe_mgr_tcam_scrub_timer_stop(bf_dev_id_t);

/* Set TCAM scrub timer in msec */
pipe_status_t pipe_mgr_tcam_scrub_timer_set(bf_dev_id_t dev,
                                            uint32_t msec_timer);
/* Get TCAM scrub timer in msec */
uint32_t pipe_mgr_tcam_scrub_timer_get(bf_dev_id_t dev);

/* Given the dev port, get the corresponding pipe id to which this port belongs
 */
pipe_status_t pipe_mgr_pipe_id_get(bf_dev_id_t dev_id,
                                   bf_dev_port_t dev_port,
                                   bf_dev_pipe_t *pipe_id);

/* Get last index per table for HW validation functions.
 * Expected usage would be e.g:
 * while (next_index <= last_index) {}
 */
pipe_status_t pipe_mgr_tbl_get_last_index(dev_target_t dev_tgt,
                                          pipe_mat_tbl_hdl_t tbl_hdl,
                                          uint32_t *last_index);

/*
 * Function to get HW values for specified index. This function allows
 * iteration over HW resources that are not used by any existing entries.
 * next_index should be used in order to get to next HW resource. Function
 * will lookup entry in LLP SW state and verify if both SW and HW data are
 * in sync in terms of entry validity. If LLP SW state and HW state are not
 * the same, error code PIPE_INTERNAL_ERROR is returned and attempt to correct
 * HW values will be made if requested. If error correction is requested this
 * function will add missing entries and remove orphaned entries that do not
 * have SW state. When required HLP state will be used to rebuild entry for
 * the add operation.
 * In case of intermediate function call fail (e.g read from HW) respective
 * error code will be returned.
 *
 * @param  sess_hdl[in]	        Session handle
 * @param  dev_tgt[in] 	        Device target (device id, pipe id)
 * @param  tbl_hdl[in]          Table handle of the match table
 * @param  index[in] 	        Query index, should start from 0, is not the
 *				same as entry handle or HW index.
 * @param  err_correction[in]   Flag that enables HW error correction, when it
 *				does not match SW state. In case where error
 *				correction is needed function will return error
 *				even if it was success.
 * @param  match_spec[out]	Populated if there is a valid non default entry
 *				present at current index.
 * @param  act_spec[out]	Populated if there is a valid or default entry
 *				present at current index.
 * @param  act_fn_hdl[out]	Populated if there is a valid or default entry
 *				present at current index.
 * @param  entry_hdl[out]	Populated if there is a valid or default entry
 *				present at current index.
 * @param  is_default[out]	Populated if there is valid and default entry
 *				present at current index.
 * @param  next_index[out]	Index to be used in next call to this function.
 *
 * @return 		        Status of the API call
 */
pipe_status_t pipe_mgr_tbl_get_entry_from_index(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_mat_tbl_hdl_t tbl_hdl,
    uint32_t index,
    bool err_correction,
    pipe_tbl_match_spec_t *match_spec,
    pipe_action_spec_t *act_spec,
    pipe_act_fn_hdl_t *act_fn_hdl,
    pipe_ent_hdl_t *entry_hdl,
    bool *is_default,
    uint32_t *next_index);

/************ Hitless HA testing APIs ******************/
pipe_status_t pipe_mgr_enable_callbacks_for_hitless_ha(pipe_sess_hdl_t sess_hdl,
                                                       bf_dev_id_t device_id);

/*
 * This function is used to get a report of the total number of entries that
 * were added/deleted/modified after HA reconcile.
 *
 * @param  sess_hdl	        Session handle
 * @param  dev_tgt 	        Device target (device id, pipe id)
 * @param  mat_tbl_hdl          Table handle of the match table
 * @param  ha_report	        Structure containing all the entry add/delete/
 *				modify count during HA reconcile
 * @return 		        Status of the API call
 */
pipe_status_t pipe_mgr_mat_ha_reconciliation_report_get(
    pipe_sess_hdl_t sess_hdl,
    dev_target_t dev_tgt,
    pipe_mat_tbl_hdl_t mat_tbl_hdl,
    pipe_tbl_ha_reconc_report_t *ha_report);

/*
 * This function is used to get the pipe val to be Or'ed with the handle
 * from context.json. Since context.json is separate per pipeline, this
 * function takes in both program and pipeline names
 *
 * @param  dev_id           Device ID
 * @param  prog_name        Program name
 * @param  pipeline_name    pipeline name
 * @param  pipe_mask (out)  Pipe mask
 * @return                  Status of the API call
 */
bf_status_t pipe_mgr_tbl_hdl_pipe_mask_get(bf_dev_id_t dev_id,
                                           const char *prog_name,
                                           const char *pipeline_name,
                                           uint32_t *pipe_mask);

/*
 * This function is used to get the number of active pipelines
 *
 * @param  dev_id 	        Device ID
 * @param  num_pipes (out)	Number of pipes
 * @return 		        Status of the API call
 */
pipe_status_t pipe_mgr_get_num_pipelines(bf_dev_id_t dev_id,
                                         uint32_t *num_pipes);

/*
 * This function is used to get the number of active stages
 *
 * @param  dev_id 	        Device ID
 * @param  num_active_stages (out)	Number of active stages
 * @return 		        Status of the API call
 */
pipe_status_t pipe_mgr_get_num_active_stages(bf_dev_id_t dev_id,
                                             uint8_t *num_active_stages);

/*
 * This function is used to complete the port mode transition workaround
 *
 * @param  dev_id         Device ID
 * @param  port_id        Port identifier
 * @return                Status of the API call
 */
bf_status_t pipe_mgr_complete_port_mode_transition_wa(bf_dev_id_t dev_id,
                                                      bf_dev_port_t port_id);

/*
 * This function is used to get the EBUF packet counter
 *
 * @param  dev_id         Device ID
 * @param  port_id        Port identifier
 * @param  value (out)    Counter value
 * @return                Status of the API call
 */
bf_status_t bf_pipe_mgr_port_ebuf_counter_get(bf_dev_id_t dev_id,
                                              bf_dev_port_t port_id,
                                              uint64_t *value);
/*
 * This function is used to get the number of active pipelines
 *
 * @param  dev_id 	        Device ID
 * @param  num_pipes (out)	Number of pipes
 * @return 		        Status of the API call
 */
pipe_status_t pipe_mgr_get_num_pipelines(bf_dev_id_t dev_id,
                                         uint32_t *num_pipes);

/*
 * Sets the maximum PPS (packet per second) for the specified pipe.
 *
 * @param  dev_tgt (in) Device and pipe to program.  Can specify either an
 *                      individual pipe or, with BF_DEV_PIPE_ALL, all pipes.
 * @param  max_pps (in)	PPS value to program.  Note the value will be rounded
 *                      to the nearest value supported by the HW.
 * @return 		          Status of the API call
 */
pipe_status_t pipe_mgr_pipe_pps_limit_set(dev_target_t dev_tgt,
                                          uint64_t max_pps);

/*
 * Gets the current PPS (packet per second) limit for the specified pipe.
 *
 * @param  dev_tgt (in)  Device and pipe to read.  Can specify either an
 *                       individual pipe or, with BF_DEV_PIPE_ALL, all pipes. If
 *                       all pipes are specified and there are different maximum
 *                       rates configured on the pipes, the highest value is
 *                       returned.
 * @param  max_pps (out) PPS value is returned here.  Note the value returned
 *                       will be what is currently programmed in hardware which
 *                       may be a rounded value.
 * @return 		           Status of the API call
 */
bf_status_t pipe_mgr_pipe_pps_limit_get(dev_target_t dev_tgt,
                                        uint64_t *max_pps);

/*
 * Gets the maximum PPS (packet per second) limit for the specified pipe.  The
 * maximum PPS supported by a pipe will be based on the clock frequency of the
 * ASIC and, possibly, the P4 program.
 *
 * @param  dev_tgt (in)  Device and pipe to read.  Can specify either an
 *                       individual pipe or, with BF_DEV_PIPE_ALL, all pipes. If
 *                       all pipes are specified and there are different maximum
 *                       rates supported on the pipes, the highest value is
 *                       returned.
 * @param  max_pps (out) PPS value is returned here.
 * @return 		           Status of the API call
 */
bf_status_t pipe_mgr_pipe_pps_max_get(dev_target_t dev_tgt, uint64_t *max_pps);

/*
 * Resets the maximum PPS (packet per second) limit for the specified pipe to
 * the default (maximum) value.
 *
 * @param  dev_tgt (in) Device and pipe to program.  Can specify either an
 *                      individual pipe or, with BF_DEV_PIPE_ALL, all pipes.
 * @return 		          Status of the API call
 */
bf_status_t pipe_mgr_pipe_pps_limit_reset(dev_target_t dev_tgt);

/*
 * This function is used to get the EBUF bypass packet counter
 *
 * @param  dev_id         Device ID
 * @param  port_id        Port identifier
 * @param  value (out)    Counter value
 * @return                Status of the API call
 */
bf_status_t bf_pipe_mgr_port_ebuf_bypass_counter_get(bf_dev_id_t dev_id,
                                                     bf_dev_port_t port_id,
                                                     uint64_t *value);

/*
 * This function is used to get the EBUF 100G port credits
 *
 * @param  dev_id         Device ID
 * @param  port_id        Port identifier
 * @param  value (out)    Counter value
 * @return                Status of the API call
 */
bf_status_t bf_pipe_mgr_port_ebuf_100g_credits_get(bf_dev_id_t dev_id,
                                                   bf_dev_port_t port_id,
                                                   uint64_t *value);

/*
 * This function is used to set ingress parser priorty threshold.
 *
 * @param  dev_id           Device ID
 * @param  port_id          Port identifier
 * @param  threshold        Ingress parser priority threshold for port.
 *                          Legal thresholds are Tf: 0-7, Tf2, Tf3: 0-3.
 * @return                  Status of the API call
 */
bf_status_t bf_pipe_mgr_port_iprsr_threshold_set(bf_dev_id_t dev_id,
                                                 bf_dev_port_t port_id,
                                                 uint32_t threshold);

/*
 * This function is used to get ingress parser priorty threshold.
 *
 * @param  dev_id           Device ID
 * @param  port_id          Port identifier
 * @param  threshold        Ingress parser priority threshold for port.
 * @return                  Status of the API call
 */
bf_status_t bf_pipe_mgr_port_iprsr_threshold_get(bf_dev_id_t dev_id,
                                                 bf_dev_port_t port_id,
                                                 uint32_t *threshold);

/*
 * This function is used to set the 25G overspeed mode state.  When enabled,
 * parts of the datapath (i-prsr, i-dprsr, EPB, e-prsr, PMArb, e-dprsr, ebuf)
 * are programmed at 50g using two channels instead of at 25g using one channel
 * to improve the egress parsing performance.  Changing the mode does not affect
 * existing ports, only ports created after the mode is set.
 *
 * Only applies to Tofino-1.
 *
 * @param  dev_id           Device ID
 * @param  dev_port         Port Identifier.  Since a 50g overspeed requires two
 *                          channels the dev_port must be an even number.
 * @param  enable           Boolean indicating whether the mode should be
 *                          enabled or disabled
 * @return                  Status of the API call
 */
bf_status_t bf_pipe_mgr_25g_overspeed_mode_set(bf_dev_id_t dev_id,
                                               bf_dev_port_t dev_port,
                                               bool enable);
/*
 * This function is used to get the 25G overspeed mode state.
 *
 * @param  dev_id           Device ID
 * @param  dev_port         Port Identifier.
 * @param  enable           Pointer to a boolean where the current mode (enabled
 *                          or disabled) will be returned.
 * @return                  Status of the API call
 */
bf_status_t bf_pipe_mgr_25g_overspeed_mode_get(bf_dev_id_t dev_id,
                                               bf_dev_port_t dev_port,
                                               bool *enable);

/*
 * Reprogram the GFM with the provided data patterns, the row parity will be
 * set to good (even) parity unless the row is marked for bad parity by the
 * caller.
 * @param  shdl             Session handle
 * @param  dev_tgt          Target device and pipe(s)
 * @param  pipe_api_flags   Flags for the API, e.g. sychronous
 * @param  gress            Gress to be used when creating wide bubbles for
 *                          atomic GFM updates
 * @param  stage_id         Stage to update or 0xFF for all stages
 * @param  num_patterns     Number of entries in the row_patterns array
 * @param  row_patterns     Pointer to an array of data patterns to program.
 *                          The patterns will be applied to each row of the GFM,
 *                          the pattern used for any given row is given by
 *                          row_patterns[ row_num MOD num_patterns ]
 * @param  row_bad_parity   Pointer to an array of 16 u64s, any set bits
 *                          indicate the corresponding row should have bad
 *                          parity.  May be NULL if good parity is requested.
 * @return                  Status of the API call
 */
pipe_status_t pipe_mgr_gfm_test_pattern_set(pipe_sess_hdl_t shdl,
                                            bf_dev_target_t dev_tgt,
                                            uint32_t pipe_api_flags,
                                            bf_dev_direction_t gress,
                                            dev_stage_t stage_id,
                                            int num_patterns,
                                            uint64_t *row_patterns,
                                            uint64_t *row_bad_parity);

/*
 * Reprogram one GFM column (all 1024 rows) with the data provided.  This may
 * be used to atomically flip between good and bad parity for all 1024 rows of
 * GFM.
 * @param  shdl             Session handle
 * @param  dev_tgt          Target device and pipe(s)
 * @param  pipe_api_flags   Flags for the API, e.g. sychronous
 * @param  gress            Gress to be used when creating wide bubbles for
 *                          atomic GFM updates
 * @param  stage_id         Stage to update or 0xFF for all stages
 * @param  column           The column to reprogram, 0..51
 * @param  col_data         An array of 1024 bits given as 64 u16 values.  This
 *                          carries the data to program in the specified column
 *                          for all 1024 rows.
 * @return                  Status of the API call
 */
pipe_status_t pipe_mgr_gfm_test_col_set(pipe_sess_hdl_t shdl,
                                        bf_dev_target_t dev_tgt,
                                        uint32_t pipe_api_flags,
                                        bf_dev_direction_t gress,
                                        dev_stage_t stage_id,
                                        int column,
                                        uint16_t col_data[64]);
#ifdef __cplusplus
}
#endif /* C++ */

#endif /* _PIPE_MGR_INTF_H */
