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


#include <array>
#include <bf_rt/bf_rt_session.hpp>
#include <bf_rt/bf_rt_table_attributes.hpp>

#include "bf_rt_table_attributes_impl.hpp"
#include "bf_rt_table_impl.hpp"
#include "bf_rt_state.hpp"

#include <array>
#include <bitset>

namespace bfrt {

BfRtTableEntryScopeArgumentsImpl::BfRtTableEntryScopeArgumentsImpl(
    const std::bitset<64> &val) {
  scope_value_ = val.to_ullong();
}

BfRtTableEntryScopeArgumentsImpl::BfRtTableEntryScopeArgumentsImpl(
    const std::array<std::bitset<8>, 8> &val_arr)
    : scope_value_() {
  int i = 0;
  for (const auto &ele : val_arr) {
    scope_value_ |= (ele.to_ullong() << (i * 8));
    i++;
  }
}

bf_status_t BfRtTableEntryScopeArgumentsImpl::setValue(
    const std::bitset<64> &val) {
  scope_value_ = val.to_ullong();
  return BF_SUCCESS;
}

bf_status_t BfRtTableEntryScopeArgumentsImpl::setValue(
    const std::array<std::bitset<8>, 8> &val_arr) {
  int i = 0;
  for (const auto &ele : val_arr) {
    // Form scope_value_ from the elements of the array passed in with least
    // significant byte of scope_value_ (byte 0)  = val_arr[0], byte 1 =
    // val_arr[1] and so on.
    scope_value_ |= (ele.to_ullong() << (i * 8));
    i++;
  }
  return BF_SUCCESS;
}

bf_status_t BfRtTableEntryScopeArgumentsImpl::getValue(
    std::bitset<64> *val) const {
  if (val == nullptr) {
    LOG_ERROR("%s:%d No memory assigned for out-param to get scope arguments",
              __func__,
              __LINE__);
    return BF_INVALID_ARG;
  }
  *val = scope_value_;
  return BF_SUCCESS;
}

bf_status_t BfRtTableEntryScopeArgumentsImpl::getValue(
    std::array<std::bitset<8>, 8> *val_arr) const {
  int i = 0;
  // Form a byte array of size 8 from the scope_value_ with val_arr[0] = least
  // signnificant byte of scope_value_ (byte 0), val_arr[1] = byte 1
  // and so on
  for (auto &ele : *val_arr) {
    ele = ((scope_value_ >> (i * 8)) & 0xff);
    i++;
  }
  return BF_SUCCESS;
}

BfRtTableAttributesIdleTable::BfRtTableAttributesIdleTable() : enable_(false) {
  idle_time_param_.mode = POLL_MODE;
  // TODO confirm correctness
  idle_time_param_.u.notify.max_ttl = idle_time_param_.u.notify.min_ttl = 0;
}

void BfRtTableAttributesIdleTable::idleTableAllParamsClear() {
  enable_ = false;
  idle_cb_cpp_ = nullptr;
  active_cb_cpp_ = nullptr;
  idle_cb_c_ = nullptr;
  active_cb_c_ = nullptr;
  std::memset(&idle_time_param_, 0, sizeof(idle_time_param_));
}

bf_status_t BfRtTableAttributesIdleTable::idleTableModeSet(
    const TableAttributesIdleTableMode &table_type) {
  switch (table_type) {
    case TableAttributesIdleTableMode::NOTIFY_MODE:
      idle_time_param_.mode = NOTIFY_MODE;
      break;
    case TableAttributesIdleTableMode::POLL_MODE:
      idle_time_param_.mode = POLL_MODE;
      break;
    case TableAttributesIdleTableMode::INVALID_MODE:
      idle_time_param_.mode = INVALID_MODE;
      break;
    default:
      LOG_ERROR("%s:%d Invalid value for Idle Table Mode", __func__, __LINE__);
      return BF_INVALID_ARG;
  }
  return BF_SUCCESS;
}

TableAttributesIdleTableMode BfRtTableAttributesIdleTable::idleTableModeGet() {
  switch (idle_time_param_.mode) {
    case NOTIFY_MODE:
      return TableAttributesIdleTableMode::NOTIFY_MODE;
    case POLL_MODE:
      return TableAttributesIdleTableMode::POLL_MODE;
    case INVALID_MODE:
      return TableAttributesIdleTableMode::INVALID_MODE;
    default:
      LOG_ERROR("%s:%d Invalid Idle Table Mode Found set internally",
                __func__,
                __LINE__);
      BF_RT_ASSERT(0);
  }
  return TableAttributesIdleTableMode::POLL_MODE;
}

bf_status_t BfRtTableAttributesIdleTable::idleTablePollModeSet(
    const bool &enable) {
  if (idle_time_param_.mode != POLL_MODE) {
    LOG_ERROR(
        "%s:%d Table Attribute Obj with Idle Table mode %d can't be used to "
        "set POLL_MODE",
        __func__,
        __LINE__,
        idle_time_param_.mode);
  }
  enable_ = enable;
  idle_cb_cpp_ = nullptr;
  active_cb_cpp_ = nullptr;
  idle_cb_c_ = nullptr;
  active_cb_c_ = nullptr;
  idle_time_param_.u.notify.ttl_query_interval = 0;
  idle_time_param_.u.notify.max_ttl = 0;
  idle_time_param_.u.notify.min_ttl = 0;
  idle_time_param_.u.notify.client_data = nullptr;
  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesIdleTable::idleTableNotifyModeSet(
    const bool &enable,
    const BfRtIdleTmoExpiryCb &idle_cb_cpp,
    const BfRtIdleTmoActiveCb &active_cb_cpp,
    const bf_rt_idle_tmo_expiry_cb &idle_cb_c,
    const bf_rt_idle_tmo_active_cb &active_cb_c,
    const uint32_t &ttl_query_interval,
    const uint32_t &max_ttl,
    const uint32_t &min_ttl,
    const void *cookie) {
  if (idle_time_param_.mode != NOTIFY_MODE) {
    LOG_ERROR(
        "%s:%d Table Attribute Obj with Idle Table mode %d can't be used to "
        "set NOTIFY_MODE",
        __func__,
        __LINE__,
        idle_time_param_.mode);
  }
  enable_ = enable;
  idle_cb_cpp_ = idle_cb_cpp;
  active_cb_cpp_ = active_cb_cpp;
  idle_cb_c_ = idle_cb_c;
  active_cb_c_ = active_cb_c;
  idle_time_param_.u.notify.ttl_query_interval = ttl_query_interval;
  idle_time_param_.u.notify.max_ttl = max_ttl;
  idle_time_param_.u.notify.min_ttl = min_ttl;
  idle_time_param_.u.notify.client_data = const_cast<void *>(cookie);
  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesIdleTable::idleTableGet(
    TableAttributesIdleTableMode *mode,
    bool *enable,
    BfRtIdleTmoExpiryCb *idle_cb_cpp,
    BfRtIdleTmoActiveCb *active_cb_cpp,
    bf_rt_idle_tmo_expiry_cb *idle_cb_c,
    bf_rt_idle_tmo_active_cb *active_cb_c,
    uint32_t *ttl_query_interval,
    uint32_t *max_ttl,
    uint32_t *min_ttl,
    void **cookie) const {
  if (mode) {
    switch (idle_time_param_.mode) {
      case POLL_MODE:
        *mode = TableAttributesIdleTableMode::POLL_MODE;
        break;
      case NOTIFY_MODE:
        *mode = TableAttributesIdleTableMode::NOTIFY_MODE;
        break;
      case INVALID_MODE:
        *mode = TableAttributesIdleTableMode::INVALID_MODE;
        break;
      default:
        LOG_ERROR("%s:%d Invalid Idle Table Mode found to be set",
                  __func__,
                  __LINE__);
        BF_RT_ASSERT(0);
    }
  }
  if (enable) {
    *enable = enable_;
  }
  if (idle_cb_cpp) {
    *idle_cb_cpp = idle_cb_cpp_;
  }
  if (active_cb_cpp) {
    *active_cb_cpp = active_cb_cpp_;
  }
  if (idle_cb_c) {
    *idle_cb_c = idle_cb_c_;
  }
  if (active_cb_c) {
    *active_cb_c = active_cb_c_;
  }
  if (ttl_query_interval) {
    *ttl_query_interval = idle_time_param_.u.notify.ttl_query_interval;
  }
  if (max_ttl) {
    *max_ttl = idle_time_param_.u.notify.max_ttl;
  }
  if (min_ttl) {
    *min_ttl = idle_time_param_.u.notify.min_ttl;
  }
  if (cookie) {
    *cookie = idle_time_param_.u.notify.client_data;
  }
  return BF_SUCCESS;
}

BfRtTableAttributesEntryScope::BfRtTableAttributesEntryScope()
    : entry_scope_(TableEntryScope::ENTRY_SCOPE_ALL_PIPELINES) {
  property_args_value_ = 0;
}

bf_status_t BfRtTableAttributesEntryScope::entryScopeArgumentsAllocate(
    std::unique_ptr<BfRtTableEntryScopeArguments> *scope_args_ret) const {
  *scope_args_ret = std::unique_ptr<BfRtTableEntryScopeArguments>(
      new BfRtTableEntryScopeArgumentsImpl(0));
  if (*scope_args_ret == nullptr) {
    LOG_ERROR("%s:%d Unable to allocate memory for Table Entry Scope Arguments",
              __func__,
              __LINE__);
    return BF_NO_SYS_RESOURCES;
  }
  return BF_SUCCESS;
}

void BfRtTableAttributesEntryScope::entryScopeAllParamsClear() {
  entry_scope_ = TableEntryScope::ENTRY_SCOPE_ALL_PIPELINES;
  gress_scope_ = TableGressScope::GRESS_SCOPE_ALL_GRESS;
  prsr_scope_ = TablePrsrScope::PRSR_SCOPE_ALL_PRSRS_IN_PIPE;
  std::memset(&property_args_value_, 0, sizeof(property_args_value_));
  std::memset(&prsr_args_value_, 0, sizeof(prsr_args_value_));
}

bf_status_t BfRtTableAttributesEntryScope::entryScopeParamsSet(
    const TableEntryScope &entry_scope,
    const BfRtTableEntryScopeArguments &scope_args) {
  switch (entry_scope) {
    case TableEntryScope::ENTRY_SCOPE_ALL_PIPELINES:
    case TableEntryScope::ENTRY_SCOPE_SINGLE_PIPELINE:
    case TableEntryScope::ENTRY_SCOPE_USER_DEFINED:
      break;
    default:
      LOG_ERROR("%s:%d Unrecognized entry scope", __func__, __LINE__);
      return BF_INVALID_ARG;
  }
  std::bitset<64> bitval;
  scope_args.getValue(&bitval);
  property_args_value_ = bitval.to_ullong();
  entry_scope_ = entry_scope;
  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesEntryScope::entryScopeParamsSet(
    const TableEntryScope &entry_scope) {
  switch (entry_scope) {
    case TableEntryScope::ENTRY_SCOPE_ALL_PIPELINES:
    case TableEntryScope::ENTRY_SCOPE_SINGLE_PIPELINE:
      break;
    case TableEntryScope::ENTRY_SCOPE_USER_DEFINED:
    default:
      LOG_ERROR("%s:%d Unrecognized entry scope", __func__, __LINE__);
      return BF_INVALID_ARG;
  }
  entry_scope_ = entry_scope;
  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesEntryScope::entryScopeParamsGet(
    TableEntryScope *entry_scope,
    BfRtTableEntryScopeArguments *scope_args) const {
  if (entry_scope == nullptr) {
    LOG_ERROR(
        "%s:%d No memory assigned for out-param to get TableEntryScope type",
        __func__,
        __LINE__);
    return BF_INVALID_ARG;
  }
  std::bitset<64> bitval(property_args_value_);
  if (scope_args != nullptr) {
    scope_args->setValue(bitval);
  }
  *entry_scope = entry_scope_;
  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesEntryScope::prsrScopeParamsSet(
    const TablePrsrScope &prsr_scope, const GressTarget &prsr_gress) {
  switch (prsr_gress) {
    case GressTarget::GRESS_TARGET_INGRESS:
      prsr_args_value_ = static_cast<uint32_t>(BF_DEV_DIR_INGRESS);
      break;
    case GressTarget::GRESS_TARGET_EGRESS:
      prsr_args_value_ = static_cast<uint32_t>(BF_DEV_DIR_EGRESS);
      break;
    case GressTarget::GRESS_TARGET_ALL:
      prsr_args_value_ = static_cast<uint32_t>(BF_DEV_DIR_ALL);
      break;
    default:
      LOG_ERROR("%s:%d Unrecognized parser scope gress (%d)",
                __func__,
                __LINE__,
                static_cast<int>(prsr_gress));
      return BF_INVALID_ARG;
  }
  prsr_scope_ = prsr_scope;
  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesEntryScope::prsrScopeParamsGet(
    TablePrsrScope *prsr_scope, GressTarget *prsr_gress) const {
  if ((prsr_scope == nullptr) || (prsr_gress == nullptr)) {
    LOG_ERROR(
        "%s:%d No memory assigned for out-param to get TablePrsrScope type",
        __func__,
        __LINE__);
    return BF_INVALID_ARG;
  }
  switch (prsr_args_value_) {
    case (static_cast<uint32_t>(BF_DEV_DIR_INGRESS)):
      *prsr_gress = GressTarget::GRESS_TARGET_INGRESS;
      break;
    case (static_cast<uint32_t>(BF_DEV_DIR_EGRESS)):
      *prsr_gress = GressTarget::GRESS_TARGET_EGRESS;
      break;
    case (static_cast<uint32_t>(BF_DEV_DIR_ALL)):
      *prsr_gress = GressTarget::GRESS_TARGET_ALL;
      break;
    default:
      LOG_ERROR("%s:%d Unrecognized parser scope args (%d)",
                __func__,
                __LINE__,
                static_cast<int>(prsr_args_value_));
      return BF_INVALID_ARG;
  }
  *prsr_scope = prsr_scope_;
  return BF_SUCCESS;
}

BfRtTableAttributesDynKeyMask::BfRtTableAttributesDynKeyMask() {
  dynKeyMaskAllParamsClear();
}

void BfRtTableAttributesDynKeyMask::dynKeyMaskAllParamsClear() {
  field_mask_.clear();
}

bf_status_t BfRtTableAttributesDynKeyMask::dynKeyMaskParamsSet(
    const std::unordered_map<bf_rt_id_t, std::vector<uint8_t>> &field_mask) {
  field_mask_ = field_mask;
  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesDynKeyMask::dynKeyMaskParamsGet(
    std::unordered_map<bf_rt_id_t, std::vector<uint8_t>> *field_mask) const {
  if (field_mask == NULL) {
    LOG_ERROR(
        "%s:%d No memory assigned for out-param to get dynamic key mask "
        "attribute",
        __func__,
        __LINE__);
    return BF_INVALID_ARG;
  }
  *field_mask = field_mask_;
  return BF_SUCCESS;
}

BfRtTableAttributesDynHashing::BfRtTableAttributesDynHashing() {
  dynHashingAllParamsClear();
}

void BfRtTableAttributesDynHashing::dynHashingAllParamsClear() {
  alg_hdl_ = 0;
  seed_ = 0;
}

bf_status_t BfRtTableAttributesDynHashing::dynHashingParamsSet(
    const uint32_t &alg_hdl, const uint64_t &seed) {
  alg_hdl_ = alg_hdl;
  seed_ = seed;
  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesDynHashing::dynHashingParamsGet(
    uint32_t *alg_hdl, uint64_t *seed) const {
  if ((alg_hdl == NULL) || (seed == NULL)) {
    LOG_ERROR(
        "%s:%d No memory assigned for out-param to get dynamic hashing "
        "algorithm and seed "
        "attribute",
        __func__,
        __LINE__);
    return BF_INVALID_ARG;
  }
  *alg_hdl = alg_hdl_;
  *seed = seed_;
  return BF_SUCCESS;
}

BfRtTableAttributesMeterByteCountAdj::BfRtTableAttributesMeterByteCountAdj() {
  byteCountAdjClear();
}

void BfRtTableAttributesMeterByteCountAdj::byteCountAdjClear() {
  byte_count_adj_ = 0;
}

bf_status_t BfRtTableAttributesMeterByteCountAdj::byteCountAdjSet(
    const int &byte_count_adj) {
  byte_count_adj_ = byte_count_adj;
  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesMeterByteCountAdj::byteCountAdjGet(
    int *byte_count_adj) const {
  if (byte_count_adj == NULL) {
    LOG_ERROR(
        "%s:%d No memory assigned for out-param to get meter byte count adjust "
        "attribute",
        __func__,
        __LINE__);
    return BF_INVALID_ARG;
  }
  *byte_count_adj = byte_count_adj_;
  return BF_SUCCESS;
}

BfRtTableAttributePortStatusChangeReg::BfRtTableAttributePortStatusChangeReg() {
  portStatusChgParamsClear();
}

bf_status_t BfRtTableAttributePortStatusChangeReg::portStatusChgCbSet(
    const bool enable,
    const BfRtPortStatusNotifCb &cb,
    const bf_rt_port_status_chg_cb &cb_c,
    const void *cookie) {
  if (cb && cb_c) {
    LOG_ERROR(
        "%s:%d Not allow to set both c and c++ callback functions at the same "
        "time",
        __func__,
        __LINE__);
    return BF_INVALID_ARG;
  }
  enable_ = enable;
  callback_func_ = cb;
  callback_func_c_ = cb_c;
  client_data_ = const_cast<void *>(cookie);
  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributePortStatusChangeReg::portStatusChgCbGet(
    bool *enable,
    BfRtPortStatusNotifCb *cb,
    bf_rt_port_status_chg_cb *cb_c,
    void **cookie) const {
  if (cb) {
    *cb = callback_func_;
  }
  if (cb_c) {
    *cb_c = callback_func_c_;
  }
  *enable = enable_;
  if (cookie) *cookie = client_data_;
  return BF_SUCCESS;
}

void BfRtTableAttributePortStatusChangeReg::portStatusChgParamsClear() {
  enable_ = false;
  callback_func_ = nullptr;
  callback_func_c_ = nullptr;
  client_data_ = nullptr;
}

BfRtTableAttributesPortStatPollIntvl::BfRtTableAttributesPortStatPollIntvl() {
  portStatPollIntvlParamClear();
}

bf_status_t BfRtTableAttributesPortStatPollIntvl::portStatPollIntvlParamSet(
    const uint32_t &poll_intvl) {
  intvl_ = poll_intvl;
  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesPortStatPollIntvl::portStatPollIntvlParamGet(
    uint32_t *poll_intvl) const {
  if (poll_intvl == NULL) {
    LOG_ERROR(
        "%s:%d No memory assigned for out-param to get port stat poll interval "
        "attribute",
        __func__,
        __LINE__);
    return BF_INVALID_ARG;
  }
  *poll_intvl = intvl_;
  return BF_SUCCESS;
}

void BfRtTableAttributesPortStatPollIntvl::portStatPollIntvlParamClear() {
  intvl_ = 0;
}

BfRtTableAttributesPREDeviceConfig::BfRtTableAttributesPREDeviceConfig() {
  this->preDeviceConfigParamsClear();
}

bf_status_t BfRtTableAttributesPREDeviceConfig::preGlobalRidParamSet(
    const uint32_t &global_rid) {
  // First mark the PRE global rid attribute active by adding it to
  // the active attibutes set. No need to check if it is present already
  // or for any errors.
  this->active_pre_attributes_.insert(this->PREAttributeType::PRE_GLOBAL_RID);

  // Set the value
  global_rid_ = global_rid;

  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesPREDeviceConfig::preGlobalRidParamGet(
    uint32_t *global_rid) const {
  if (global_rid == NULL) {
    LOG_ERROR(
        "%s:%d No memory assigned for out-param to get PRE global rid "
        "attribute",
        __func__,
        __LINE__);
    return BF_INVALID_ARG;
  }

  // Check whether global rid attribute is part of the active attributes set
  auto elem =
      this->active_pre_attributes_.find(this->PREAttributeType::PRE_GLOBAL_RID);
  if (elem == this->active_pre_attributes_.end()) {
    // No need to log error here as this could be valid case
    return BF_OBJECT_NOT_FOUND;
  }

  *global_rid = global_rid_;

  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesPREDeviceConfig::prePortProtectionParamSet(
    const bool &enable) {
  // First mark the PRE port protection attribute active by adding it to
  // the active attibutes set. No need to check if it is present already
  // or for any errors.
  this->active_pre_attributes_.insert(
      this->PREAttributeType::PRE_PORT_PROTECTION);

  // Set the value
  port_protection_enable_ = enable;

  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesPREDeviceConfig::prePortProtectionParamGet(
    bool *enable) const {
  if (enable == NULL) {
    LOG_ERROR(
        "%s:%d No memory assigned for out-param to get PRE port protection "
        "attribute",
        __func__,
        __LINE__);
    return BF_INVALID_ARG;
  }

  // Check whether port protection attribute is part of the active attributes
  // set
  auto elem = this->active_pre_attributes_.find(
      this->PREAttributeType::PRE_PORT_PROTECTION);
  if (elem == this->active_pre_attributes_.end()) {
    // No need to log error here as this could be valid case
    return BF_OBJECT_NOT_FOUND;
  }

  *enable = port_protection_enable_;

  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesPREDeviceConfig::preFastFailoverParamSet(
    const bool &enable) {
  // First mark the PRE fast failover attribute active by adding it to
  // the active attibutes set. No need to check if it is present already
  // or for any errors.
  this->active_pre_attributes_.insert(
      this->PREAttributeType::PRE_FAST_FAILOVER);

  // Set the value
  fast_failover_enable_ = enable;

  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesPREDeviceConfig::preFastFailoverParamGet(
    bool *enable) const {
  if (enable == NULL) {
    LOG_ERROR(
        "%s:%d No memory assigned for out-param to get PRE fast failover "
        "attribute",
        __func__,
        __LINE__);
    return BF_INVALID_ARG;
  }

  // Check whether fast failover attribute is part of the active attributes set
  auto elem = this->active_pre_attributes_.find(
      this->PREAttributeType::PRE_FAST_FAILOVER);
  if (elem == this->active_pre_attributes_.end()) {
    // No need to log error here as this could be valid case
    return BF_OBJECT_NOT_FOUND;
  }

  *enable = fast_failover_enable_;

  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesPREDeviceConfig::preMaxNodesBeforeYieldParamSet(
    const uint32_t &count) {
  // First mark the PRE max nodes before yield attribute active by adding it to
  // the active attibutes set. No need to check if it is present already
  // or for any errors.
  this->active_pre_attributes_.insert(
      this->PREAttributeType::PRE_MAX_NODES_BEFORE_YIELD);

  // Set the value
  max_nodes_before_yield_ = count;

  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesPREDeviceConfig::preMaxNodesBeforeYieldParamGet(
    uint32_t *count) const {
  if (count == NULL) {
    LOG_ERROR(
        "%s:%d No memory assigned for out-param to get PRE max nodes before "
        "yield "
        "attribute",
        __func__,
        __LINE__);
    return BF_INVALID_ARG;
  }

  // Check whether max nodes before yield attribute is part of the active
  // attributes set
  auto elem = this->active_pre_attributes_.find(
      this->PREAttributeType::PRE_MAX_NODES_BEFORE_YIELD);
  if (elem == this->active_pre_attributes_.end()) {
    // No need to log error here as this could be valid case
    return BF_OBJECT_NOT_FOUND;
  }

  *count = max_nodes_before_yield_;

  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesPREDeviceConfig::preMaxNodeThresholdParamSet(
    const uint32_t &node_count, const uint32_t &node_port_lag_count) {
  // First mark the PRE max threshold attribute active by adding it to
  // the active attibutes set. No need to check if it is present already
  // or for any errors.
  this->active_pre_attributes_.insert(
      this->PREAttributeType::PRE_MAX_NODE_THRESHOLD);

  // Set the value
  max_node_threshold_node_count_ = node_count;
  max_node_threshold_port_lag_count_ = node_port_lag_count;

  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesPREDeviceConfig::preMaxNodeThresholdParamGet(
    uint32_t *node_count, uint32_t *node_port_lag_count) const {
  if (node_count == NULL || node_port_lag_count == NULL) {
    LOG_ERROR(
        "%s:%d No memory assigned for out-param(s) to get PRE max node "
        "threshold "
        "attribute",
        __func__,
        __LINE__);
    return BF_INVALID_ARG;
  }

  // Check whether max node threshold attribute is part of the active attributes
  // set
  auto elem = this->active_pre_attributes_.find(
      this->PREAttributeType::PRE_MAX_NODE_THRESHOLD);
  if (elem == this->active_pre_attributes_.end()) {
    // No need to log error here as this could be valid case
    return BF_OBJECT_NOT_FOUND;
  }

  *node_count = max_node_threshold_node_count_;
  *node_port_lag_count = max_node_threshold_port_lag_count_;

  return BF_SUCCESS;
}

void BfRtTableAttributesPREDeviceConfig::preDeviceConfigParamsClear() {
  // Clear the active PRE attributes set
  this->active_pre_attributes_.clear();

  // Though the actual PRE attribute's value doesn't
  // matter if the PRE attributes set is all clear, better to initialize each
  // attribute filed to default values.
  global_rid_ = 0;
  port_protection_enable_ = false;
  fast_failover_enable_ = false;
  max_nodes_before_yield_ = 0;
  max_node_threshold_node_count_ = 0;
  max_node_threshold_port_lag_count_ = 0;
}

BfRtTableAttributesSelectorUpdateCallback::
    BfRtTableAttributesSelectorUpdateCallback() {
  this->paramsClear();
}

void BfRtTableAttributesSelectorUpdateCallback::paramsClear() {
  enable_ = false;
  session_.reset();
  cpp_callback_fn_ = nullptr;
  c_callback_fn_ = nullptr;
  cookie_ = nullptr;
}

void BfRtTableAttributesSelectorUpdateCallback::paramSet(
    const bool &enable,
    const std::shared_ptr<BfRtSession> session,
    const selUpdateCb &cpp_callback_fn,
    const bf_rt_selector_table_update_cb &c_callback_fn,
    const void *cookie) {
  enable_ = enable;
  if (enable_) {
    session_ = session;
    cpp_callback_fn_ = cpp_callback_fn;
    c_callback_fn_ = c_callback_fn;
    cookie_ = cookie;
  }
  return;
}

void BfRtTableAttributesSelectorUpdateCallback::paramGet(
    bool *enable,
    BfRtSession **session,
    selUpdateCb *cpp_callback_fn,
    bf_rt_selector_table_update_cb *c_callback_fn,
    void **cookie) const {
  *enable = enable_;
  if (enable_) {
    auto session_obj = session_.lock();
    if (session_obj != nullptr) {
      *session = session_obj.get();
    } else {
      LOG_ERROR(
          "%s:%d ERROR The session for which the selector update callback was "
          "set up no longer exists. Hence unavailable to retrieve the session",
          __func__,
          __LINE__);
      *session = nullptr;
    }
    if (cpp_callback_fn) {
      *cpp_callback_fn = cpp_callback_fn_;
    }
    if (c_callback_fn) {
      *c_callback_fn = c_callback_fn_;
    }
    *cookie = const_cast<void *>(cookie_);
  }
  return;
}

void BfRtTableAttributesSelectorUpdateCallback::paramSetInternal(
    const std::tuple<bool,
                     std::weak_ptr<BfRtSession>,
                     selUpdateCb,
                     bf_rt_selector_table_update_cb,
                     const void *> &t) {
  enable_ = std::get<0>(t);
  session_ = std::get<1>(t);
  cpp_callback_fn_ = std::get<2>(t);
  c_callback_fn_ = std::get<3>(t);
  cookie_ = std::get<4>(t);
}
std::tuple<bool,
           std::weak_ptr<BfRtSession>,
           selUpdateCb,
           bf_rt_selector_table_update_cb,
           const void *>
BfRtTableAttributesSelectorUpdateCallback::paramGetInternal() const {
  return std::make_tuple(
      enable_, session_, cpp_callback_fn_, c_callback_fn_, cookie_);
}

BfRtTableAttributesImpl::BfRtTableAttributesImpl(
    const BfRtTableObj *table,
    const TableAttributesType &attr_type,
    const TableAttributesIdleTableMode &idle_table_mode) {
  table_ = table;
  attr_type_ = attr_type;
  idle_table_.idleTableModeSet(idle_table_mode);
}

bf_status_t BfRtTableAttributesImpl::resetAttributeType(
    const TableAttributesType &attr) {
  switch (attr) {
    case TableAttributesType::IDLE_TABLE_RUNTIME:
      LOG_ERROR("%s:%d This API can't be used to set Attribute type as %d",
                __func__,
                __LINE__,
                static_cast<int>(attr));
      return BF_INVALID_ARG;
    case TableAttributesType::ENTRY_SCOPE:
      entry_scope_.entryScopeAllParamsClear();
      break;
    case TableAttributesType::DYNAMIC_KEY_MASK:
      dyn_key_mask_.dynKeyMaskAllParamsClear();
      break;
    case TableAttributesType::DYNAMIC_HASH_ALG_SEED:
      dyn_hashing_.dynHashingAllParamsClear();
      break;
    case TableAttributesType::METER_BYTE_COUNT_ADJ:
      meter_bytecount_adj_.byteCountAdjClear();
      break;
    case TableAttributesType::PORT_STATUS_NOTIF:
      port_status_chg_reg_.portStatusChgParamsClear();
      break;
    case TableAttributesType::PORT_STAT_POLL_INTVL_MS:
      port_stat_poll_.portStatPollIntvlParamClear();
      break;
    case TableAttributesType::PRE_DEVICE_CONFIG:
      pre_device_config_.preDeviceConfigParamsClear();
      break;
    case TableAttributesType::SELECTOR_UPDATE_CALLBACK:
      selector_update_callback_.paramsClear();
      break;
    default:
      LOG_ERROR("%s:%d Invalid Attribute Type %d",
                __func__,
                __LINE__,
                static_cast<int>(attr));
      return BF_INVALID_ARG;
  }
  attr_type_ = attr;
  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesImpl::resetAttributeType(
    const TableAttributesType &attr,
    const TableAttributesIdleTableMode &idle_mode) {
  switch (attr) {
    case TableAttributesType::IDLE_TABLE_RUNTIME:
      break;
    case TableAttributesType::ENTRY_SCOPE:
    case TableAttributesType::DYNAMIC_KEY_MASK:
    case TableAttributesType::DYNAMIC_HASH_ALG_SEED:
    case TableAttributesType::METER_BYTE_COUNT_ADJ:
    case TableAttributesType::PORT_STATUS_NOTIF:
    case TableAttributesType::PORT_STAT_POLL_INTVL_MS:
    case TableAttributesType::PRE_DEVICE_CONFIG:
      LOG_ERROR("%s:%d This API can't be used to set Attribute type as %d",
                __func__,
                __LINE__,
                static_cast<int>(attr));
      return BF_NOT_SUPPORTED;
    default:
      LOG_ERROR("%s:%d Invalid Attribute Type %d",
                __func__,
                __LINE__,
                static_cast<int>(attr));
      return BF_INVALID_ARG;
  }
  // clear all the params in idle table obj
  idle_table_.idleTableAllParamsClear();
  // Set the new idle table mode
  auto bf_status = idleTableModeSet(idle_mode);
  if (bf_status != BF_SUCCESS) {
    return bf_status;
  }
  attr_type_ = attr;
  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesImpl::idleTablePollModeSet(const bool &enable) {
  if (attr_type_ != TableAttributesType::IDLE_TABLE_RUNTIME) {
    LOG_ERROR("%s:%d Can't set Idle Table Attribute with Object of type %d",
              __func__,
              __LINE__,
              static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  const auto idle_mode = idleTableModeGet();
  if (idle_mode != TableAttributesIdleTableMode::POLL_MODE) {
    LOG_ERROR(
        "%s:%d Can't set Idle Table Poll Mode with Attribute object of type %d",
        __func__,
        __LINE__,
        static_cast<int>(idle_mode));
    return BF_INVALID_ARG;
  }
  return idle_table_.idleTablePollModeSet(enable);
}

bf_status_t BfRtTableAttributesImpl::idleTableNotifyModeSet(
    const bool &enable,
    const BfRtIdleTmoExpiryCb &idle_cb,
    const uint32_t &ttl_query_interval,
    const uint32_t &max_ttl,
    const uint32_t &min_ttl,
    const void *cookie) {
  if (attr_type_ != TableAttributesType::IDLE_TABLE_RUNTIME) {
    LOG_ERROR("%s:%d Can't set Idle Table Attribute with Object of type %d",
              __func__,
              __LINE__,
              static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  const auto idle_mode = idleTableModeGet();
  if (idle_mode != TableAttributesIdleTableMode::NOTIFY_MODE) {
    LOG_ERROR(
        "%s:%d Can't set Idle Table Notify Mode with Attribute object of type "
        "%d",
        __func__,
        __LINE__,
        static_cast<int>(idle_mode));
    return BF_INVALID_ARG;
  }
  return idle_table_.idleTableNotifyModeSet(enable,
                                            idle_cb,
                                            nullptr,
                                            nullptr,
                                            nullptr,
                                            ttl_query_interval,
                                            max_ttl,
                                            min_ttl,
                                            cookie);
}

bf_status_t BfRtTableAttributesImpl::idleTableNotifyModeSet(
    const bool &enable,
    const BfRtIdleTmoExpiryCb &idle_cb,
    const BfRtIdleTmoExpiryCb &active_cb,
    const uint32_t &ttl_query_interval,
    const uint32_t &max_ttl,
    const uint32_t &min_ttl,
    const void *cookie) {
  if (attr_type_ != TableAttributesType::IDLE_TABLE_RUNTIME) {
    LOG_ERROR("%s:%d Can't set Idle Table Attribute with Object of type %d",
              __func__,
              __LINE__,
              static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  const auto idle_mode = idleTableModeGet();
  if (idle_mode != TableAttributesIdleTableMode::NOTIFY_MODE) {
    LOG_ERROR(
        "%s:%d Can't set Idle Table Notify Mode with Attribute object of type "
        "%d",
        __func__,
        __LINE__,
        static_cast<int>(idle_mode));
    return BF_INVALID_ARG;
  }
  return idle_table_.idleTableNotifyModeSet(enable,
                                            idle_cb,
                                            active_cb,
                                            nullptr,
                                            nullptr,
                                            ttl_query_interval,
                                            max_ttl,
                                            min_ttl,
                                            cookie);
}

bf_status_t BfRtTableAttributesImpl::idleTableGet(
    TableAttributesIdleTableMode *mode,
    bool *enable,
    BfRtIdleTmoExpiryCb *idle_cb,
    uint32_t *ttl_query_interval,
    uint32_t *max_ttl,
    uint32_t *min_ttl,
    void **cookie) const {
  if (attr_type_ != TableAttributesType::IDLE_TABLE_RUNTIME) {
    LOG_ERROR("%s:%d Can't get Idle Table Attribute with Object of type %d",
              __func__,
              __LINE__,
              static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return idle_table_.idleTableGet(mode,
                                  enable,
                                  idle_cb,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  ttl_query_interval,
                                  max_ttl,
                                  min_ttl,
                                  cookie);
}

bf_status_t BfRtTableAttributesImpl::idleTableGet(
    TableAttributesIdleTableMode *mode,
    bool *enable,
    BfRtIdleTmoExpiryCb *idle_cb,
    BfRtIdleTmoActiveCb *active_cb,
    uint32_t *ttl_query_interval,
    uint32_t *max_ttl,
    uint32_t *min_ttl,
    void **cookie) const {
  if (attr_type_ != TableAttributesType::IDLE_TABLE_RUNTIME) {
    LOG_ERROR("%s:%d Can't get Idle Table Attribute with Object of type %d",
              __func__,
              __LINE__,
              static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return idle_table_.idleTableGet(mode,
                                  enable,
                                  idle_cb,
                                  active_cb,
                                  nullptr,
                                  nullptr,
                                  ttl_query_interval,
                                  max_ttl,
                                  min_ttl,
                                  cookie);
}

bf_status_t BfRtTableAttributesImpl::entryScopeParamsSet(
    const TableGressScope &gress,
    const TableEntryScope &pipe,
    const BfRtTableEntryScopeArguments &pipe_args,
    const TablePrsrScope &prsr,
    const GressTarget &prsr_gress) {
  if (attr_type_ != TableAttributesType::ENTRY_SCOPE) {
    LOG_ERROR("%s:%d Can't get PVS Table Attribute with Object of type %d",
              __func__,
              __LINE__,
              static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  std::bitset<64> bitval;
  pipe_args.getValue(&bitval);
  // When gress_scope set to single_gress, pipe_scope and prsr_scope have to be
  // set on single gress
  // Check gress parameter of pipe_scope and prsr_scope here
  if ((gress == TableGressScope::GRESS_SCOPE_SINGLE_GRESS) &&
      (((bitval == static_cast<uint32_t>(GressTarget::GRESS_TARGET_ALL)) &&
        (pipe != TableEntryScope::ENTRY_SCOPE_USER_DEFINED)) ||
       (prsr_gress == GressTarget::GRESS_TARGET_ALL))) {
    LOG_ERROR("%s:%d Conflict in Attributes setting.", __func__, __LINE__);
    return BF_INVALID_ARG;
  }
  switch (gress) {
    case TableGressScope::GRESS_SCOPE_ALL_GRESS:
    case TableGressScope::GRESS_SCOPE_SINGLE_GRESS:
      break;
    default:
      LOG_ERROR("%s:%d Unrecognized gress scope", __func__, __LINE__);
      return BF_INVALID_ARG;
  }
  switch (pipe) {
    case TableEntryScope::ENTRY_SCOPE_ALL_PIPELINES:
    case TableEntryScope::ENTRY_SCOPE_SINGLE_PIPELINE:
    case TableEntryScope::ENTRY_SCOPE_USER_DEFINED:
      break;
    default:
      LOG_ERROR("%s:%d Unrecognized pipe scope", __func__, __LINE__);
      return BF_INVALID_ARG;
  }
  switch (prsr) {
    case TablePrsrScope::PRSR_SCOPE_ALL_PRSRS_IN_PIPE:
    case TablePrsrScope::PRSR_SCOPE_SINGLE_PRSR:
      break;
    default:
      LOG_ERROR("%s:%d Unrecognized parser scope", __func__, __LINE__);
      return BF_INVALID_ARG;
  }
  bf_status_t sts = BF_SUCCESS;
  sts = entry_scope_.gressScopeParamsSet(gress);
  sts |= entry_scope_.entryScopeParamsSet(pipe, pipe_args);
  sts |= entry_scope_.prsrScopeParamsSet(prsr, prsr_gress);
  return sts;
}

bf_status_t BfRtTableAttributesImpl::entryScopeParamsGet(
    TableGressScope *gress,
    TableEntryScope *pipe,
    BfRtTableEntryScopeArguments *pipe_args,
    TablePrsrScope *prsr,
    GressTarget *prsr_gress) const {
  if (attr_type_ != TableAttributesType::ENTRY_SCOPE) {
    LOG_ERROR("%s:%d Can't get PVS Table Attribute with Object of type %d",
              __func__,
              __LINE__,
              static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  bf_status_t sts = BF_SUCCESS;
  sts = entry_scope_.gressScopeParamsGet(gress);
  sts |= entry_scope_.entryScopeParamsGet(pipe, pipe_args);
  sts |= entry_scope_.prsrScopeParamsGet(prsr, prsr_gress);
  return sts;
}

bf_status_t BfRtTableAttributesImpl::dynKeyMaskSet(
    const std::unordered_map<bf_rt_id_t, std::vector<uint8_t>> &field_mask) {
  if (attr_type_ != TableAttributesType::DYNAMIC_KEY_MASK) {
    LOG_ERROR(
        "%s:%d Can't set Dynamic Key Mask Attribute with Object of type %d",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return dyn_key_mask_.dynKeyMaskParamsSet(field_mask);
}

bf_status_t BfRtTableAttributesImpl::dynKeyMaskGet(
    std::unordered_map<bf_rt_id_t, std::vector<uint8_t>> *field_mask) const {
  if (attr_type_ != TableAttributesType::DYNAMIC_KEY_MASK) {
    LOG_ERROR(
        "%s:%d Can't get Dynamic Key Mask Attribute with Object of type %d",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return dyn_key_mask_.dynKeyMaskParamsGet(field_mask);
}

bf_status_t BfRtTableAttributesImpl::dynHashingSet(const uint32_t &alg_hdl,
                                                   const uint64_t &seed) {
  if (attr_type_ != TableAttributesType::DYNAMIC_HASH_ALG_SEED) {
    LOG_ERROR(
        "%s:%d Can't set Dynamic Hashing Attribute with Object of type %d",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return dyn_hashing_.dynHashingParamsSet(alg_hdl, seed);
}

bf_status_t BfRtTableAttributesImpl::dynHashingGet(uint32_t *alg_hdl,
                                                   uint64_t *seed) const {
  if (attr_type_ != TableAttributesType::DYNAMIC_HASH_ALG_SEED) {
    LOG_ERROR(
        "%s:%d Can't get Dynamic Hashing Attribute with Object of type %d",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return dyn_hashing_.dynHashingParamsGet(alg_hdl, seed);
}

bf_status_t BfRtTableAttributesImpl::meterByteCountAdjSet(
    const int &byte_count) {
  if (attr_type_ != TableAttributesType::METER_BYTE_COUNT_ADJ) {
    LOG_ERROR(
        "%s:%d Can't set Meter Byte Count Adjust Attribute with Object of type "
        "%d",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return meter_bytecount_adj_.byteCountAdjSet(byte_count);
}
bf_status_t BfRtTableAttributesImpl::meterByteCountAdjGet(
    int *byte_count) const {
  if (attr_type_ != TableAttributesType::METER_BYTE_COUNT_ADJ) {
    LOG_ERROR(
        "%s:%d Can't get Meter Byte Count Adjust Attribute with Object of type "
        "%d",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return meter_bytecount_adj_.byteCountAdjGet(byte_count);
}

bf_status_t BfRtTableAttributesImpl::idleTableNotifyModeSetCFrontend(
    const bool &enable,
    bf_rt_idle_tmo_expiry_cb idle_cb,
    bf_rt_idle_tmo_active_cb active_cb,
    const uint32_t &ttl_query_interval,
    const uint32_t &max_ttl,
    const uint32_t &min_ttl,
    const void *cookie) {
  if (attr_type_ != TableAttributesType::IDLE_TABLE_RUNTIME) {
    LOG_ERROR("%s:%d Can't set Idle Table Attribute with Object of type %d",
              __func__,
              __LINE__,
              static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  const auto idle_mode = idleTableModeGet();
  if (idle_mode != TableAttributesIdleTableMode::NOTIFY_MODE) {
    LOG_ERROR(
        "%s:%d Can't set Idle Table Notify Mode with Attribute object of type "
        "%d",
        __func__,
        __LINE__,
        static_cast<int>(idle_mode));
    return BF_INVALID_ARG;
  }
  return idle_table_.idleTableNotifyModeSet(enable,
                                            nullptr,
                                            nullptr,
                                            idle_cb,
                                            active_cb,
                                            ttl_query_interval,
                                            max_ttl,
                                            min_ttl,
                                            cookie);
}

bf_status_t BfRtTableAttributesImpl::idleTableGetCFrontend(
    TableAttributesIdleTableMode *mode,
    bool *enable,
    bf_rt_idle_tmo_expiry_cb *idle_cb,
    bf_rt_idle_tmo_active_cb *active_cb,
    uint32_t *ttl_query_interval,
    uint32_t *max_ttl,
    uint32_t *min_ttl,
    void **cookie) const {
  if (attr_type_ != TableAttributesType::IDLE_TABLE_RUNTIME) {
    LOG_ERROR("%s:%d Can't get Idle Table Attribute with Object of type %d",
              __func__,
              __LINE__,
              static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return idle_table_.idleTableGet(mode,
                                  enable,
                                  nullptr,
                                  nullptr,
                                  idle_cb,
                                  active_cb,
                                  ttl_query_interval,
                                  max_ttl,
                                  min_ttl,
                                  cookie);
}

bf_status_t BfRtTableAttributesImpl::entryScopeArgumentsAllocate(
    std::unique_ptr<BfRtTableEntryScopeArguments> *scope_args_ret) const {
  if (attr_type_ != TableAttributesType::ENTRY_SCOPE) {
    LOG_ERROR(
        "%s:%d Can't use Object of type %d for Entry Scope Attribute Operation",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return entry_scope_.entryScopeArgumentsAllocate(scope_args_ret);
}

bf_status_t BfRtTableAttributesImpl::entryScopeParamsSet(
    const TableEntryScope &entry_scope,
    const BfRtTableEntryScopeArguments &scope_args) {
  if (attr_type_ != TableAttributesType::ENTRY_SCOPE) {
    LOG_ERROR(
        "%s:%d Can't use Object of type %d for Entry Scope Attribute Operation",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return entry_scope_.entryScopeParamsSet(entry_scope, scope_args);
}

bf_status_t BfRtTableAttributesImpl::entryScopeParamsSet(
    const TableEntryScope &entry_scope) {
  if (attr_type_ != TableAttributesType::ENTRY_SCOPE) {
    LOG_ERROR(
        "%s:%d Can't use Object of type %d for Entry Scope Attribute Operation",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return entry_scope_.entryScopeParamsSet(entry_scope);
}

bf_status_t BfRtTableAttributesImpl::entryScopeParamsGet(
    TableEntryScope *entry_scope,
    BfRtTableEntryScopeArguments *scope_args) const {
  if (attr_type_ != TableAttributesType::ENTRY_SCOPE) {
    LOG_ERROR(
        "%s:%d Can't use Object of type %d for Entry Scope Attribute Operation",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return entry_scope_.entryScopeParamsGet(entry_scope, scope_args);
}

bf_status_t BfRtTableAttributesImpl::portStatusChangeNotifSet(
    const bool &enable,
    const BfRtPortStatusNotifCb &callback,
    const bf_rt_port_status_chg_cb &callback_c,
    const void *cookie) {
  if (attr_type_ != TableAttributesType::PORT_STATUS_NOTIF) {
    LOG_ERROR("%s:%d Can't set Port Cfg Notif Attribute with Object of type %d",
              __func__,
              __LINE__,
              static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return port_status_chg_reg_.portStatusChgCbSet(
      enable, callback, callback_c, cookie);
}

bf_status_t BfRtTableAttributesImpl::portStatusChangeNotifGet(
    bool *enable,
    BfRtPortStatusNotifCb *callback,
    bf_rt_port_status_chg_cb *callback_c,
    void **cookie) const {
  if (attr_type_ != TableAttributesType::PORT_STATUS_NOTIF) {
    LOG_ERROR("%s:%d Can't get Port Cfg Notif Attribute with Object of type %d",
              __func__,
              __LINE__,
              static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return port_status_chg_reg_.portStatusChgCbGet(
      enable, callback, callback_c, cookie);
}

bf_status_t BfRtTableAttributesImpl::portStatusChangeNotifSetCFrontend(
    const bool &enable,
    bf_rt_port_status_chg_cb callback_c,
    const void *cookie) {
  if (attr_type_ != TableAttributesType::PORT_STATUS_NOTIF) {
    LOG_ERROR("%s:%d Can't set port cfg Table Attribute with Object of type %d",
              __func__,
              __LINE__,
              static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return port_status_chg_reg_.portStatusChgCbSet(
      enable, nullptr, callback_c, cookie);
}

bf_status_t BfRtTableAttributesImpl::portStatusChangeNotifGetCFrontend(
    bool *enable, bf_rt_port_status_chg_cb *callback_c, void **cookie) const {
  if (attr_type_ != TableAttributesType::PORT_STATUS_NOTIF) {
    LOG_ERROR("%s:%d Can't get port cfg Table Attribute with Object of type %d",
              __func__,
              __LINE__,
              static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return port_status_chg_reg_.portStatusChgCbGet(
      enable, nullptr, callback_c, cookie);
}

bf_status_t BfRtTableAttributesImpl::portStatPollIntvlMsSet(
    const uint32_t &poll_intvl_ms) {
  if (attr_type_ != TableAttributesType::PORT_STAT_POLL_INTVL_MS) {
    LOG_ERROR(
        "%s:%d Can't set Port Stat Poll Intvl Attribute with Object of type %d",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return port_stat_poll_.portStatPollIntvlParamSet(poll_intvl_ms);
}

bf_status_t BfRtTableAttributesImpl::portStatPollIntvlMsGet(
    uint32_t *poll_intvl_ms) const {
  if (attr_type_ != TableAttributesType::PORT_STAT_POLL_INTVL_MS) {
    LOG_ERROR(
        "%s:%d Can't get Port Stat Poll Intvl Attribute with Object of type %d",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return port_stat_poll_.portStatPollIntvlParamGet(poll_intvl_ms);
}

bf_status_t BfRtTableAttributesImpl::preGlobalRidSet(
    const uint32_t &global_rid) {
  if (attr_type_ != TableAttributesType::PRE_DEVICE_CONFIG) {
    LOG_ERROR("%s:%d Can't set PRE Global RID Attribute with Object of type %d",
              __func__,
              __LINE__,
              static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }

  // Actual global RID size in MC mgr is 16 bits. So, make sure
  // the value is valid.
  auto size_in_bits = sizeof(uint16_t) * 8;
  auto max_limit = (1U << size_in_bits) - 1;
  if (global_rid > max_limit) {
    LOG_ERROR(
        "%s:%d Can't set PRE Global RID Attribute, value %u greater "
        "than max limit %u",
        __func__,
        __LINE__,
        global_rid,
        max_limit);
    return BF_INVALID_ARG;
  }

  return pre_device_config_.preGlobalRidParamSet(global_rid);
}

bf_status_t BfRtTableAttributesImpl::preGlobalRidGet(
    uint32_t *global_rid) const {
  if (attr_type_ != TableAttributesType::PRE_DEVICE_CONFIG) {
    LOG_ERROR("%s:%d Can't get PRE Global RID Attribute with Object of type %d",
              __func__,
              __LINE__,
              static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return pre_device_config_.preGlobalRidParamGet(global_rid);
}

bf_status_t BfRtTableAttributesImpl::prePortProtectionSet(const bool &enable) {
  if (attr_type_ != TableAttributesType::PRE_DEVICE_CONFIG) {
    LOG_ERROR(
        "%s:%d Can't set PRE Port Protection Attribute with Object of type %d",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return pre_device_config_.prePortProtectionParamSet(enable);
}

bf_status_t BfRtTableAttributesImpl::prePortProtectionGet(bool *enable) const {
  if (attr_type_ != TableAttributesType::PRE_DEVICE_CONFIG) {
    LOG_ERROR(
        "%s:%d Can't get PRE Port Protection Attribute with Object of type %d",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return pre_device_config_.prePortProtectionParamGet(enable);
}

bf_status_t BfRtTableAttributesImpl::preFastFailoverSet(const bool &enable) {
  if (attr_type_ != TableAttributesType::PRE_DEVICE_CONFIG) {
    LOG_ERROR(
        "%s:%d Can't set PRE Fast Failover Attribute with Object of type %d",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return pre_device_config_.preFastFailoverParamSet(enable);
}

bf_status_t BfRtTableAttributesImpl::preFastFailoverGet(bool *enable) const {
  if (attr_type_ != TableAttributesType::PRE_DEVICE_CONFIG) {
    LOG_ERROR(
        "%s:%d Can't get PRE Fast Failover Attribute with Object of type %d",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return pre_device_config_.preFastFailoverParamGet(enable);
}

bf_status_t BfRtTableAttributesImpl::preMaxNodesBeforeYieldSet(
    const uint32_t &count) {
  if (attr_type_ != TableAttributesType::PRE_DEVICE_CONFIG) {
    LOG_ERROR(
        "%s:%d Can't set PRE Max Nodes Before Yield Attribute with Object of "
        "type %d",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return pre_device_config_.preMaxNodesBeforeYieldParamSet(count);
}

bf_status_t BfRtTableAttributesImpl::preMaxNodesBeforeYieldGet(
    uint32_t *count) const {
  if (attr_type_ != TableAttributesType::PRE_DEVICE_CONFIG) {
    LOG_ERROR(
        "%s:%d Can't get PRE Max Nodes Before Yield Attribute with Object of "
        "type %d",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return pre_device_config_.preMaxNodesBeforeYieldParamGet(count);
}

bf_status_t BfRtTableAttributesImpl::preMaxNodeThresholdSet(
    const uint32_t &node_count, const uint32_t &node_port_lag_count) {
  if (attr_type_ != TableAttributesType::PRE_DEVICE_CONFIG) {
    LOG_ERROR(
        "%s:%d Can't set PRE Max Node Threshold Attribute with Object of type "
        "%d",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return pre_device_config_.preMaxNodeThresholdParamSet(node_count,
                                                        node_port_lag_count);
}

bf_status_t BfRtTableAttributesImpl::preMaxNodeThresholdGet(
    uint32_t *node_count, uint32_t *node_port_lag_count) const {
  if (attr_type_ != TableAttributesType::PRE_DEVICE_CONFIG) {
    LOG_ERROR(
        "%s:%d Can't get PRE Max Node Threshold Attribute with Object of type "
        "%d",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  return pre_device_config_.preMaxNodeThresholdParamGet(node_count,
                                                        node_port_lag_count);
}

bf_status_t BfRtTableAttributesImpl::selectorUpdateCbSet(
    const bool &enable,
    const std::shared_ptr<BfRtSession> session,
    const selUpdateCb &callback_fn,
    const void *cookie) {
  if (attr_type_ != TableAttributesType::SELECTOR_UPDATE_CALLBACK) {
    LOG_ERROR(
        "%s:%d Can't set Selector update callback Attribute with Object of "
        "type "
        "%d",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  selector_update_callback_.paramSet(
      enable, session, callback_fn, nullptr /* c_cb */, cookie);

  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesImpl::selectorUpdateCbSetCFrontend(
    const bool &enable,
    const std::shared_ptr<BfRtSession> session,
    const bf_rt_selector_table_update_cb &callback_fn,
    const void *cookie) {
  if (attr_type_ != TableAttributesType::SELECTOR_UPDATE_CALLBACK) {
    LOG_ERROR(
        "%s:%d Can't set Selector update callback Attribute with Object of "
        "type "
        "%d",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  selector_update_callback_.paramSet(
      enable, session, nullptr /* cpp_cb */, callback_fn, cookie);

  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesImpl::selectorUpdateCbGet(
    bool *enable,
    BfRtSession **session,
    selUpdateCb *callback_fn,
    void **cookie) const {
  if (attr_type_ != TableAttributesType::SELECTOR_UPDATE_CALLBACK) {
    LOG_ERROR(
        "%s:%d Can't get Selector update callback Attribute with Object of "
        "type "
        "%d",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  bf_rt_selector_table_update_cb unused_fn;
  selector_update_callback_.paramGet(
      enable, session, callback_fn, &unused_fn /* c_cb */, cookie);

  (void)unused_fn;
  return BF_SUCCESS;
}

bf_status_t BfRtTableAttributesImpl::selectorUpdateCbGetCFrontend(
    bool *enable,
    BfRtSession **session,
    bf_rt_selector_table_update_cb *callback_fn,
    void **cookie) const {
  if (attr_type_ != TableAttributesType::SELECTOR_UPDATE_CALLBACK) {
    LOG_ERROR(
        "%s:%d Can't get Selector update callback Attribute with Object of "
        "type "
        "%d",
        __func__,
        __LINE__,
        static_cast<int>(attr_type_));
    return BF_INVALID_ARG;
  }
  selUpdateCb unused_fn;
  selector_update_callback_.paramGet(
      enable, session, &unused_fn /* cpp_cb*/, callback_fn, cookie);

  (void)unused_fn;
  return BF_SUCCESS;
}

void BfRtTableAttributesImpl::selectorUpdateCbInternalSet(
    const std::tuple<bool,
                     std::weak_ptr<BfRtSession>,
                     selUpdateCb,
                     bf_rt_selector_table_update_cb,
                     const void *> &t) {
  return selector_update_callback_.paramSetInternal(t);
}

std::tuple<bool,
           std::weak_ptr<BfRtSession>,
           selUpdateCb,
           bf_rt_selector_table_update_cb,
           const void *>
BfRtTableAttributesImpl::selectorUpdateCbInternalGet() const {
  return selector_update_callback_.paramGetInternal();
}

}  // namespace bfrt
