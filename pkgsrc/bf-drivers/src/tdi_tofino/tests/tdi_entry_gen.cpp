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

#ifdef __sun
#include <endian.h>
#endif

#include <tdi_common/tdi_utils.hpp>
#include "tdi_entry_gen.hpp"
#include <cstdlib>
#include <random>
#include <unordered_set>
#include <iostream>

namespace tdi {

#define SELECTOR_MAX_GROUP_SIZE 250

namespace {

std::unordered_set<std::string> seen_gen_values;

uint8_t htobe(const uint8_t &value) { return value; }
uint16_t htobe(const uint16_t &value) { return htobe16(value); }
uint32_t htobe(const uint32_t &value) { return htobe32(value); }
uint64_t htobe(const uint64_t &value) { return htobe64(value); }

uint8_t betoh(const uint8_t &value) { return value; }
uint16_t betoh(const uint16_t &value) { return be16toh(value); }
uint32_t betoh(const uint32_t &value) { return be32toh(value); }
uint64_t betoh(const uint64_t &value) { return be64toh(value); }

// Fills the mbr_id arr with randomly generated mbr_ids
void fillArrayRandom(const uint32_t &mbr_count,
                     std::vector<uint32_t> *mbr_arr) {
  std::unordered_set<uint32_t> seen_mbrs;
  for (uint32_t i = 0; i < mbr_count; i++) {
    uint32_t mbr_id;
    do {
      mbr_id = rand() % (1ULL << 32);
    } while (seen_mbrs.find(mbr_id) != seen_mbrs.end());
    seen_mbrs.insert(mbr_id);
    mbr_arr->push_back(mbr_id);
  }
}
// Array of uint8_t
void fillArrayRandom_uint8(const uint32_t &mbr_count,
                           std::vector<uint32_t> *mbr_arr) {
  std::unordered_set<uint32_t> seen_mbrs;
  for (uint32_t i = 0; i < mbr_count; i++) {
    uint32_t mbr_id;
    do {
      mbr_id = rand() % (1ULL << 8);
    } while (seen_mbrs.find(mbr_id) != seen_mbrs.end());
    seen_mbrs.insert(mbr_id);
    mbr_arr->push_back(mbr_id);
  }
}

// Fills the mbr_status arr with randomly generated bools
void fillArrayRandom(const uint32_t &mbr_count,
                     std::vector<bool> *mbr_sts_arr) {
  for (uint32_t i = 0; i < mbr_count; i++) {
    bool mbr_sts = rand() % 2;
    mbr_sts_arr->push_back(mbr_sts);
  }
}

// Fills the byte array with 'size' number of bits
// of random numbers
uint8_t *fillArrayRandom(const size_t &bit_size,
                         std::vector<uint8_t> *ret_vec) {
  // randomly generate numbers from 0 to 255 and put it in
  // the array
  // In order to ensure that we generate unique keys and data, we use a set.
  // For every number we generate we convert it into a string and store it
  // in the set. Next time we generate a number, we try to find it in the
  // set. If that number already exists in the set then we generate another
  // number. We try to come up with a unique number by trying this for upto
  // 10 times. If even after 10 times we are unable to find a unique number
  // we simply carry on. This is a possibility especially for keys and data
  // of 1 or 2 bytes, and when we try to generate a large number of entries.
  std::string seen_val;
  int counter = 0;
  do {
    seen_val.clear();
    ret_vec->clear();
    const size_t byte_size = (bit_size + 7) / 8;
    for (unsigned i = 0; i < byte_size; i++) {
      uint8_t rand_num;
      if (i == 0 && (bit_size % 8 != 0)) {
        // We need to ensure that the final value that is returned is within the
        // allowed range of the field. Thus if the field size is 9 bits, we need
        // to mask out the 7 most significant bits from the value
        uint8_t max_first_byte_val = (1ULL << (bit_size % 8));
        rand_num = rand() % max_first_byte_val;
      } else {
        rand_num = rand() % 256;
      }
      ret_vec->push_back(rand_num);
      seen_val.append(std::to_string(rand_num));
    }
    // Use a counter because for fields with byte size as 1 or 2, there is a
    // high possibility that we might run out of unique numbers especially
    // if the number of entries is too large. So just break the loop and
    // continue
    counter++;
  } while ((seen_gen_values.find(seen_val) != seen_gen_values.end()) &&
           (counter < 10));
  seen_gen_values.insert(seen_val);
  return &(*ret_vec)[0];
}

// Fills the byte array ret_vec with a T = uint8/16/32/64 bit
// random number.
template <typename T>
T fillArrayRandom(std::vector<uint8_t> *ret_vec,
                  T min = 0,
                  T max = sizeof(T) == 8 ? 0xffffffffffffffff
                                         : ((1ULL << sizeof(T) * 8) - 1)) {
  // randomly generate numbers from min to max and put them in
  std::string seen_val;
  int counter = 0;
  T a;
  do {
    ret_vec->clear();
    seen_val.clear();
    if (sizeof(T) > 4) {
      a = rand();
      a = a << 4 * 8;
      a |= rand();
      a = (a % (max - min)) + min;
    }

    else
      a = (rand() % (max - min)) + min;
    auto new_a = htobe(a);
    uint8_t *ptr = reinterpret_cast<uint8_t *>(&new_a);
    for (unsigned i = 0; i < sizeof(T); i++) {
      ret_vec->push_back(*ptr);
      seen_val.append(std::to_string(*ptr));
      ptr++;
    }
    counter++;
  } while ((seen_gen_values.find(seen_val) != seen_gen_values.end()) &&
           (counter < 10));
  seen_gen_values.insert(seen_val);
  return a;
}

void fillArrayRangeKey(const size_t &size_bits,
                       std::vector<uint8_t> *ret_start,
                       std::vector<uint8_t> *ret_end) {
  // first randomly select end
  // then select a number between 0 and end to be start
  //
  if (size_bits <= 8) {
    auto end_val = fillArrayRandom<uint8_t>(ret_end);
    auto start_val = fillArrayRandom<uint8_t>(ret_start, 0, end_val);
  } else if (size_bits <= 16) {
    auto end_val = fillArrayRandom<uint16_t>(ret_end);
    auto start_val = fillArrayRandom<uint16_t>(ret_start, 0, end_val);
  } else if (size_bits <= 32) {
    auto end_val = fillArrayRandom<uint32_t>(ret_end);
    auto start_val = fillArrayRandom<uint32_t>(ret_start, 0, end_val);
  } else if (size_bits <= 64) {
    auto end_val = fillArrayRandom<uint64_t>(ret_end);
    auto start_val = fillArrayRandom<uint64_t>(ret_start, 0, end_val);
  }
  return;
}

// Fill the mask
uint8_t *fillArrayMaskHelper(uint16_t &p_len,
                             const size_t &size,
                             std::vector<uint8_t> *ret_mask) {
  // Find number of bytes which need to be 0xff
  // for the remaining bits, fill them individually
  uint16_t num_bytes = p_len / 8;
  for (unsigned i = 0; i < num_bytes; i++) {
    ret_mask->push_back(0xff);
  }
  if (num_bytes == size) {
    return &(*ret_mask)[0];
  }
  auto bits = p_len % 8;
  uint8_t to_push = 0;
  for (int i = 0; i < 8 - bits; i++) {
    to_push |= 0x01;
    to_push <<= 1;
  }
  ret_mask->push_back(~to_push);
  return &(*ret_mask)[0];
}

void fillArrayLPMKey(const size_t &size_bits,
                     std::vector<uint8_t> *ret_value,
                     std::vector<uint8_t> *ret_mask,
                     uint16_t *p_len) {
  // first randomly select end
  // then select a number between 0 and end to be start
  //
  // generated prefix length is from 0 to the bit_size
  *p_len = rand() % size_bits;
  auto size_bytes = (size_bits + 7) / 8;
  auto end_val = fillArrayRandom(size_bits, ret_value);
  fillArrayMaskHelper(*p_len, size_bytes, ret_mask);
  return;
}

std::set<tdi_id_t> getBannedFieldIdSet(
    const TdiTable &table,
    const tdi_id_t &action_id,
    const std::vector<tdi_id_t> &data_field_id_list) {
  // Now go over all the fields and check for oneofs
  // Keep a set of all sets of oneof_siblings
  std::set<std::set<tdi_id_t>> oneof_siblings_vec;
  for (const auto &field_id : data_field_id_list) {
    std::set<tdi_id_t> oneof_siblings;
    if (action_id) {
      table.dataFieldOneofSiblingsGet(field_id, action_id, &oneof_siblings);
    } else {
      table.dataFieldOneofSiblingsGet(field_id, &oneof_siblings);
    }
    oneof_siblings.insert(field_id);
    // This will replace any existing same set
    if (!oneof_siblings.empty()) oneof_siblings_vec.insert(oneof_siblings);
  }
  // Go over this set of sets {{1,2} , {4,5,6}}
  // We need to create a final set of {2,4,6} by removing
  // one number randomly and keeping rest
  std::set<tdi_id_t> ret;
  for (const auto &s : oneof_siblings_vec) {
    auto r = rand() % s.size();
    unsigned i = 0;
    for (const auto &id : s) {
      if (i != r) ret.insert(id);
      i++;
    }
  }
  return ret;
}

}  // namespace

/*****************************************************************************
 * GenericKeyField
 ****************************************************************************/
GenericKeyField::GenericKeyField(const TdiTable &table,
                                 const tdi_id_t &field_id)
    : field_id_(field_id) {
  fillKeyField(table);
}

template <typename T>
void GenericKeyField::getValue(T *value) const {
  T ret_val = 0;
  uint8_t *ptr = reinterpret_cast<uint8_t *>(&ret_val);
  if (sizeof(T) > size_) {
    ptr += sizeof(T) - size_;
  }
  for (unsigned i = 0; i < size_; i++) {
    *ptr = value_byte_[i];
    ptr++;
  }
  *value = betoh(ret_val);
}

template <typename T>
void GenericKeyField::setValue(const T &value) {
  value_byte_.clear();
  const T val = htobe(value);
  const uint8_t *ptr = reinterpret_cast<const uint8_t *>(&val);
  if (sizeof(T) > size_) {
    ptr += sizeof(T) - size_;
  }
  for (unsigned i = 0; i < size_; i++) {
    value_byte_.push_back(*ptr);
    ptr++;
  }
}

template <typename T>
void GenericKeyField::getMask(T *mask) const {
  T ret_val = 0;
  uint8_t *ptr = reinterpret_cast<uint8_t *>(&ret_val);
  if (sizeof(T) > size_) {
    ptr += sizeof(T) - size_;
  }
  for (unsigned i = 0; i < size_; i++) {
    *ptr = mask_byte_[i];
    ptr++;
  }
  *mask = betoh(ret_val);
}

void GenericKeyField::fillKeyField(const TdiTable &table) {
  KeyFieldType field_type;
  auto status = table.keyFieldTypeGet(field_id_, &field_type);

  size_t field_size_bits, field_size_bytes;
  status = table.keyFieldSizeGet(field_id_, &field_size_bits);
  field_size_bytes = (field_size_bits + 7) / 8;
  size_bits_ = field_size_bits;
  size_ = field_size_bytes;

  // Clear all the vector before proceeding
  value_byte_.clear();
  mask_byte_.clear();

  switch (field_type) {
    case KeyFieldType::EXACT: {
      // if field is priority,
      // act_pro_index or resource_tbl_index; then we need to
      // fill it up in a certain range of values
      //
      std::string key_field_name;
      status = table.keyFieldNameGet(field_id_, &key_field_name);
      if (key_field_name == "$MATCH_PRIORITY") {
        fillArrayRandom<uint32_t>(&value_byte_, 0, 10);
      } else {
        fillArrayRandom(field_size_bits, &value_byte_);
      }
      // Fill the mask with all FFs. "prefix_len" for the helper func will be
      // the
      // field size in bits, since we want all the bits to be masked.
      // If the field size is 9 bits, then we want the mask to be of 2 bytes
      // each being 0xff
      uint16_t temp = static_cast<uint16_t>(field_size_bytes * 8);
      // TODO If backend table is ternary, then we need to selectively mask
      // the bits too. For 9 bits, we need to make the rest of 7 bits as 0.
      // currently the tests passes because we aren't testing out a mixed
      // case.
      fillArrayMaskHelper(temp, field_size_bytes, &mask_byte_);
      break;
    }
    case KeyFieldType::TERNARY: {
      fillArrayRandom(field_size_bits, &value_byte_);
      // Do not care whether the mask is a continuous mask or not
      fillArrayRandom(field_size_bits, &mask_byte_);
      break;
    }
    case KeyFieldType::RANGE: {
      fillArrayRangeKey(field_size_bits, &value_byte_, &mask_byte_);
      break;
    }
    case KeyFieldType::LPM: {
      fillArrayLPMKey(field_size_bits, &value_byte_, &mask_byte_, &p_len_);
      break;
    }
    case KeyFieldType::INVALID: {
      break;
    }
  }  // end switch
  return;
}

/*****************************************************************************
 * GenericKey
 ****************************************************************************/
GenericKey::GenericKey(const TdiTable &table) : table_(table) {
  // Set the type of the table
  table_.tableTypeGet(&table_type_);

  std::vector<tdi_id_t> key_field_id_list;
  table.keyFieldIdListGet(&key_field_id_list);
  for (const auto &field_id : key_field_id_list) {
    key_field_list_.emplace_back(table, field_id);
  }
}

std::vector<tdi_id_t> GenericKey::fieldIdListGet() const {
  std::vector<tdi_id_t> ret_list;
  for (const auto &field : key_field_list_) {
    ret_list.push_back(field.fieldIdGet());
  }
  return ret_list;
}

void GenericKey::regenerate() {
  for (auto &item : key_field_list_) {
    item.fillKeyField(table_);
  }
  return;
}

void GenericKey::regenerate(tdi_id_t field_id) {
  auto foundItem = std::find_if(key_field_list_.begin(),
                                key_field_list_.end(),
                                [&field_id](const GenericKeyField &item) {
                                  return item.fieldIdGet() == field_id;
                                });

  if (foundItem == key_field_list_.end()) {
    return;
  }
  (*foundItem).fillKeyField(table_);
  return;
}

const GenericKeyField &GenericKey::keyFieldGet(tdi_id_t field_id) const {
  auto foundItem = std::find_if(key_field_list_.begin(),
                                key_field_list_.end(),
                                [&field_id](const GenericKeyField &item) {
                                  return item.fieldIdGet() == field_id;
                                });
  return *foundItem;
}

/*****************************************************************************
 * GenericDataField
 ****************************************************************************/
template <typename T>
void GenericDataField::getValue(T *value) const {
  T ret_val = 0;
  uint8_t *ptr = reinterpret_cast<uint8_t *>(&ret_val);
  if (sizeof(T) > size_) {
    ptr += sizeof(T) - size_;
  }
  for (unsigned i = 0; i < size_; i++) {
    *ptr = value_byte_[i];
    ptr++;
  }
  *value = betoh(ret_val);
}

void GenericDataField::getValue(std::string *str) const {
  *str = value_string_;
}

template <typename T>
void GenericDataField::setValue(const T &value) {
  value_byte_.clear();
  const T be_val = htobe(value);
  const uint8_t *ptr = reinterpret_cast<const uint8_t *>(&be_val);
  if (sizeof(T) > size_) {
    ptr += sizeof(T) - size_;
  }
  for (unsigned i = 0; i < size_; i++) {
    value_byte_.push_back(*ptr);
    ptr++;
  }
}

void GenericDataField::fillDataFieldTTL(const TdiTable &tabe,
                                        const tdi_id_t &action_id,
                                        const uint64_t &min_ttl,
                                        const uint64_t &max_ttl) {
  fillArrayRandom<uint32_t>(&value_byte_, min_ttl, max_ttl);
}

void GenericDataField::fillDataField(const TdiTable &table,
                                     const tdi_id_t &action_id) {
  DataType field_data_type;
  std::string field_name;
  size_t field_size_bits, field_size_bytes;
  if (action_id) {
    table.dataFieldDataTypeGet(field_id_, action_id, &field_data_type);
    table.dataFieldSizeGet(field_id_, action_id, &field_size_bits);
    table.dataFieldNameGet(field_id_, action_id, &field_name);
  } else {
    table.dataFieldDataTypeGet(field_id_, &field_data_type);
    table.dataFieldSizeGet(field_id_, &field_size_bits);
    table.dataFieldNameGet(field_id_, &field_name);
  }
  field_size_bytes = (field_size_bits + 7) / 8;
  size_bits_ = field_size_bits;
  size_ = field_size_bytes;
  field_name_ = field_name;

  // Clear all the vectors before proceeding
  value_byte_.clear();
  value_int_arr_.clear();
  value_bool_arr_.clear();
  value_string_ = "";

  switch (field_data_type) {
    case DataType::BOOL: {
      fillArrayRandom<uint64_t>(&value_byte_);
      uint8_t bool_val = value_byte_[0] % 2;
      value_byte_.clear();
      value_byte_.push_back(bool_val);
      break;
    }
    case DataType::UINT64: {
      // if data_field_name has INDEX in the end, then we need to ensure
      // that the
      if (field_name_[0] == '$' &&
          field_name_.substr(field_name_.length() - 5) == "INDEX") {
        fillArrayRandom<uint64_t>(&value_byte_);
      } else if (field_name_ == "$MAX_GROUP_SIZE") {
        // Set the byte_array from the already set parent_generic_data_
        // group_size
        setValue<uint32_t>(parent_generic_data_->max_group_size_);
      } else {
        fillArrayRandom<uint64_t>(&value_byte_);
      }
      break;
    }
    case DataType::BYTE_STREAM: {
      fillArrayRandom(field_size_bits, &value_byte_);
      break;
    }
    case DataType::INT_ARR: {
      if (parent_generic_data_->mbr_count_ == 0xffffffff) {
        parent_generic_data_->mbr_count_ =
            rand() % parent_generic_data_->max_group_size_;
      }
      if (this->size_ == 1) {  // array of uin8t
        fillArrayRandom_uint8(parent_generic_data_->mbr_count_,
                              &value_int_arr_);
      } else {  // array of uint32
        fillArrayRandom(parent_generic_data_->mbr_count_, &value_int_arr_);
      }
      break;
    }
    case DataType::BOOL_ARR: {
      if (parent_generic_data_->mbr_count_ == 0xffffffff) {
        parent_generic_data_->mbr_count_ =
            rand() % parent_generic_data_->max_group_size_;
      }
      fillArrayRandom(parent_generic_data_->mbr_count_, &value_bool_arr_);
      break;
    }

    case DataType::STRING: {
      std::vector<std::reference_wrapper<const std::string>> choices;
      if (action_id) {
        table.dataFieldAllowedChoicesGet(field_id_, action_id, &choices);
      } else {
        table.dataFieldAllowedChoicesGet(field_id_, &choices);
      }
      int idx = (rand() % choices.size());
      value_string_ = choices[idx];
      break;
    }
    default:
      break;
  }  // end switch
  return;
}

GenericDataField::GenericDataField(GenericData *gen_data,
                                   const TdiTable &table,
                                   const tdi_id_t &field_id,
                                   tdi_id_t action_id)
    : parent_generic_data_(gen_data), field_id_(field_id) {
  fillDataField(table, action_id);
}

/*****************************************************************************
 * GenericData
 ****************************************************************************/
GenericData::GenericData(const TdiTable &table) : table_(table) {
  // Set the type of the table
  table_.tableTypeGet(&table_type_);

  // Select a random action ID from the action ID
  // list of the table and set own action_id
  // from among those
  std::vector<tdi_id_t> action_id_vec;
  table.actionIdListGet(&action_id_vec);
  if (action_id_vec.size() != 0) {
    int rand_idx = rand() % action_id_vec.size();
    action_id_ = action_id_vec[rand_idx];
  } else {
    action_id_ = 0;
  }
  // std::cout<<"Action id is : "<<action_id_<<"\n";

  std::vector<tdi_id_t> data_field_id_list;
  if (action_id_) {
    table.dataFieldIdListGet(action_id_, &data_field_id_list);
  } else {
    table.dataFieldIdListGet(&data_field_id_list);
  }

  // Set the max_group_size
  max_group_size_ =
      (rand() % SELECTOR_MAX_GROUP_SIZE) + 1;  // max_group_size_ cannot be 0
  // std::cout<<"Max group size is "<<max_group_size_<<"\n";

  // Get banned data field set
  std::set<tdi_id_t> banned_id =
      getBannedFieldIdSet(table, action_id_, data_field_id_list);
  for (const auto &field_id : data_field_id_list) {
    if (banned_id.find(field_id) == banned_id.end()) {
      data_field_list_.emplace_back(this, table, field_id, action_id_);
    }
  }
}

std::vector<tdi_id_t> GenericData::fieldIdListGet() const {
  std::vector<tdi_id_t> ret_list;
  for (const auto &field : data_field_list_) {
    ret_list.push_back(field.fieldIdGet());
  }
  return ret_list;
}

void GenericData::regenerate() {
  // Only reset the mbr_count_ as we cannot modify the max_group_size_
  mbr_count_ = 0xffffffff;
  for (auto &item : data_field_list_) {
    item.fillDataField(table_, action_id_);
  }
  return;
}

void GenericData::regenerate(tdi_id_t field_id) {
  auto foundItem = std::find_if(data_field_list_.begin(),
                                data_field_list_.end(),
                                [&field_id](const GenericDataField &item) {
                                  return item.fieldIdGet() == field_id;
                                });

  if (foundItem == data_field_list_.end()) {
    return;
  }
  (*foundItem).fillDataField(table_, action_id_);
  return;
}

void GenericData::regenerateTTL(tdi_id_t field_id,
                                const uint64_t &min_ttl,
                                const uint64_t &max_ttl) {
  auto foundItem = std::find_if(data_field_list_.begin(),
                                data_field_list_.end(),
                                [&field_id](const GenericDataField &item) {
                                  return item.fieldIdGet() == field_id;
                                });

  if (foundItem == data_field_list_.end()) {
    return;
  }
  (*foundItem).fillDataFieldTTL(table_, action_id_, min_ttl, max_ttl);
  return;
}

const GenericDataField &GenericData::dataFieldGet(tdi_id_t field_id) const {
  auto foundItem = std::find_if(data_field_list_.begin(),
                                data_field_list_.end(),
                                [&field_id](const GenericDataField &item) {
                                  return item.fieldIdGet() == field_id;
                                });
  return *foundItem;
}

/*****************************************************************************
 * TdiEntryGen
 ****************************************************************************/
std::unique_ptr<TdiEntryGen> TdiEntryGen::makeGenerator(const TdiTable *table) {
  return std::unique_ptr<TdiEntryGen>(new TdiMatchActionTableGen(table));
  // FIXME
  /*TdiTable::TableType table_type;
  table->tableTypeGet(&table_type);
  switch (table_type) {
    case TdiTable::TableType::MATCH_DIRECT: {
      return std::unique_ptr<TdiEntryGen>(new TdiMatchActionTableGen(table));
    }
    case TdiTable::TableType::MATCH_INDIRECT:
    case TdiTable::TableType::MATCH_INDIRECT_SELECTOR:
    case TdiTable::TableType::ACTION_PROFILE:
    case TdiTable::TableType::SELECTOR:
    case TdiTable::TableType::COUNTER:
    case TdiTable::TableType::METER:
    case TdiTable::TableType::REGISTER:
    case TdiTable::TableType::LPF:
    case TdiTable::TableType::WRED:
    case TdiTable::TableType::PVS:
    case TdiTable::TableType::PORT_METADATA:
    case TdiTable::TableType::INVALID:
      return nullptr;
  }*/
}

std::vector<GenericKey> TdiMatchActionTableGen::getTableKeyEntries(
    uint32_t num_entries) const {
  std::vector<GenericKey> ret_vec;
  for (uint32_t i = 0; i < num_entries; i++) {
    ret_vec.emplace_back(*table_);
  }
  return ret_vec;
}

std::vector<GenericData> TdiMatchActionTableGen::getTableDataEntries(
    uint32_t num_entries) const {
  std::vector<GenericData> ret_vec;
  for (uint32_t i = 0; i < num_entries; i++) {
    ret_vec.emplace_back(*table_);
  }
  return ret_vec;
}

// FIXME : how do I get around without explicitly instantiating the templates?
// Explicitly instantiate the templates
template void GenericKeyField::getValue<uint64_t>(uint64_t *) const;
template void GenericKeyField::getValue<uint32_t>(uint32_t *) const;
template void GenericKeyField::getValue<uint16_t>(uint16_t *) const;
template void GenericKeyField::getMask<uint64_t>(uint64_t *) const;
template void GenericKeyField::setValue<uint32_t>(const uint32_t &);
template void GenericDataField::getValue<uint64_t>(uint64_t *) const;
template void GenericDataField::getValue<uint32_t>(uint32_t *) const;
template void GenericDataField::getValue<uint8_t>(uint8_t *) const;
}  // namespace tdi
