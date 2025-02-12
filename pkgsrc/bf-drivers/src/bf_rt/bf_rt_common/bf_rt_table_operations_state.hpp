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


#ifndef _BF_RT_TBL_OPERATIONS_STATE_HPP
#define _BF_RT_TBL_OPERATIONS_STATE_HPP

#ifdef __cplusplus
extern "C" {
#endif
#include <bf_rt/bf_rt_common.h>
#ifdef __cplusplus
}
#endif

#include <time.h>
#include <string>
#include <cstring>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <tuple>

#ifdef __sun
#include <illumos.h>
#endif

#include <bf_rt/bf_rt_table_operations.h>
#include <bf_rt/bf_rt_table_operations.hpp>

namespace bfrt {

namespace {

// Client's callback code execution wrapper function.
template <typename T, typename U>
void bfRtStateTableOperationsCallback(const bf_rt_target_t &dev_tgt,
                                      T callback_cpp,
                                      U callback_c,
                                      void *cookie) {
  pthread_t tid = pthread_self();
  LOG_DBG("%s:%d Thread 0x%lx enters table operation client callback",
          __func__,
          __LINE__,
          (long)tid);
  if (callback_cpp) {
    callback_cpp(dev_tgt, cookie);
  } else if (callback_c) {
    bf_rt_target_t dev_tgt_ = dev_tgt;
    callback_c(&dev_tgt_, cookie);
  } else {
    LOG_TRACE("%s:%d Thread 0x%lx no callback function to call",
              __func__,
              __LINE__,
              (long) tid);
    return;
  }
  LOG_DBG("%s:%d Thread 0x%lx exits table operation client callback",
          __func__,
          __LINE__,
          (long) tid);
}

}  // namespace

class BfRtTableObj;

// Bitmap signatrure of a valid table operation object in memory.
#define BFRT_TABLE_OPERATIONS_FINGERPRINT 0x61466416

template <typename T, typename U>
class BfRtStateTableOperationsPool;

template <typename T, typename U>
class BfRtStateTableOperations {
 public:
  BfRtStateTableOperations(bf_rt_id_t id)
      : table_id_(id),
        callback_cpp_(nullptr),
        callback_c_(nullptr),
        cookie_(nullptr),
        fingerprint_(BFRT_TABLE_OPERATIONS_FINGERPRINT),
        in_use_(false),
        parent_pool_(nullptr),
        retain_seq_(0){};

  BfRtStateTableOperations(bf_rt_id_t id,
                           BfRtStateTableOperationsPool<T, U> *parent_pool)
      : table_id_(id),
        callback_cpp_(nullptr),
        callback_c_(nullptr),
        cookie_(nullptr),
        fingerprint_(BFRT_TABLE_OPERATIONS_FINGERPRINT),
        in_use_(false),
        parent_pool_(parent_pool),
        retain_seq_(0){};

  // When a pointer is passed as a cookie, it is important to check
  // that it is valid before trying to lock its mutex.
  bool isValid() const {
    return (fingerprint_ == BFRT_TABLE_OPERATIONS_FINGERPRINT);
  }

  // Internal callback entry point for the low-level driver's thread.
  static void stateTableOperationsCallback(bf_dev_id_t device_id,
                                           void *cb_cookie) {
    auto op_ptr = static_cast<BfRtStateTableOperations<T, U> *>(cb_cookie);
    if (op_ptr && (op_ptr->fingerprint_ == BFRT_TABLE_OPERATIONS_FINGERPRINT)) {
      op_ptr->stateTableOperationsCallback(device_id);
    } else {
      LOG_ERROR("%s:%d Invalid table operations cookie", __func__, __LINE__);
    }
  }

  // Transfer the state from the waiting pool into the callback task queue,
  // or execute the client code directly.
  void stateTableOperationsCallback(bf_dev_id_t device_id) {
    // Release the state object atomically.
    auto t = this->stateTableOperationsRelease();
    auto callback_cpp = std::get<0>(t);
    auto callback_c = std::get<1>(t);
    auto cookie = std::get<2>(t);
    auto dev_tgt = std::get<3>(t);
    auto parent_pool = std::get<4>(t);
    // An invalid state in the cookie is returned with all items nullptr.
    if (!callback_cpp && !callback_c) {
      // No allocated cookie is expected in this case, or its ownership lost.
      LOG_WARN(
          "%s:%d State has no callback function to call, or not in sequence",
          __func__,
          __LINE__);
      return;
    }
    if (dev_tgt.dev_id != device_id) {
      // Client's cookie might be invalid or memory leak.
      LOG_ERROR(
          "%s:%d Skip mismatched callback device_id=%d to state with "
          "device_id=%d",
          __func__,
          __LINE__,
          device_id,
          dev_tgt.dev_id);
      return;
    }
    if (parent_pool) {
      // Let the parent pool decide how to execute the client's callback.
      parent_pool->stateTableOperationsCallback(
          dev_tgt, callback_cpp, callback_c, cookie);
    } else {
      // A standalone state has no parent pool assigned and it executes
      // the client's callback directly by the calling thread.
      bfRtStateTableOperationsCallback<T, U>(
          dev_tgt, callback_cpp, callback_c, cookie);
    }
  }

  // Set the operation's state unconditionally.
  void stateTableOperationsSet(T callback_cpp,
                               U callback_c,
                               const void *cookie,
                               const bf_rt_target_t &dev_tgt);

  // Set the operation's state only if it is not in use,
  // or its current use is not longer than the grace time
  // period given (in seconds) since the state was set
  // previously.
  // On successful retain the retain_seq will be assigned
  // to track the change. Zero value increments a standalone
  // state's sequence by 1. Non-zero retain_seq is what to assign
  // on this retain attempt; usually it is given atomically
  // from the parent's pool common sequence for all its states.
  // Returns true on successfull state change.
  // Returns false if the state can't be changed yet.
  bool stateTableOperationsRetain(T callback_cpp,
                                  U callback_c,
                                  const void *cookie,
                                  const bf_rt_target_t &dev_tgt,
                                  int grace_sec,
                                  uint32_t retain_seq);

  // Reset the state object properties unconditionally.
  void stateTableOperationsReset();

  // Get the table operation's state properties as a tuple.
  std::tuple<T, U, void *, bf_rt_target_t> stateTableOperationsGet();

  // Get the table operation's state properties as a tuple
  // and reset the state object itself atomically releasing
  // it for another use.
  std::tuple<T, U, void *, bf_rt_target_t, BfRtStateTableOperationsPool<T, U> *>
  stateTableOperationsRelease();

 private:
  std::mutex state_lock;
  bf_rt_id_t table_id_;
  T callback_cpp_;
  U callback_c_;
  void *cookie_;
  bf_rt_target_t dev_tgt_ = {0, 0, BF_DEV_DIR_ALL, 0};
  uint32_t fingerprint_;  // magic value for memory check
  bool in_use_;
  struct timespec time_created_ {
    0
  };

  // Backpointer to the pool container if this state object is a pool item.
  // A standalone state has no parent pool and it executes client's callback
  // directly by the calling thread.
  BfRtStateTableOperationsPool<T, U> *parent_pool_;

  // Retain rolling counter. It allows to track the state as an id.
  // It is either local, for a standalone state, or from the parent_pool_
  // retain sequence common for all its items.
  uint32_t retain_seq_;
};  // namespace bfrt

// Pool container for the Table Operations states.
// Keeps an active operation state as the pool item waiting for synchronous
// execution, or for low-level callbacks to arrive asynchronously.
// When a callback arrives from the low-level driver code it will
// move the state from the waiting pool into the callback execution queue
// for the service thread(s), so the client callback code execution
// will be separated from the low-level driver thread to avoid delays.
// With this templated pool each callback queue is kept per-table id
// and per-operation type to be served either by the max_thread number
// of threads or directly by driver's thread if max_thread is zero.
// Separate waiting pool and callback queue allows to decouple
// request state buffering and ackowledgement callback execution.
template <typename T, typename U>
class BfRtStateTableOperationsPool {
 public:
  BfRtStateTableOperationsPool(bf_rt_id_t id)
      : cb_pool_(), table_id_(id), next_item_(0), pool_retain_seq_(0){};

  // On each retain it either attempts to reuse one of released items,
  // or expands items up to the max_items. If neither is possible,
  // then it attempts to find a stale object older than grace_sec
  // to override it.
  // Returns True with item_index successfully retained.
  bool stateTableOperationsRetain(T callback_cpp,
                                  U callback_c,
                                  const void *cookie,
                                  const bf_rt_target_t &dev_tgt,
                                  int grace_sec,
                                  std::size_t max_items,
                                  std::size_t max_threads,
                                  std::size_t &item_idx);

  // Reset the obj_index to released state.
  void stateTableOperationsReset(std::size_t item_idx);

  BfRtStateTableOperations<T, U> *getItem(std::size_t item_idx);

  friend class BfRtStateTableOperations<T, U>;

 protected:
  // Moves callback task details into the thread queue, if it is set,
  // or executes the callback directly.
  // The queue size might grow on slow client code which even can hang.
  // We enqueue all requests from the low-level driver thread expecting
  // their arrival as a result of the operation's acceptance at the waiting
  // pool with limited size and the queue growth check at that moment.
  void stateTableOperationsCallback(const bf_rt_target_t &dev_tgt,
                                    T callback_cpp,
                                    U callback_c,
                                    void *cookie) {
    pthread_t tid = pthread_self();
    if (cb_pool_) {
      LOG_DBG(
          "%s:%d Thread 0x%lx enqueues new callback (threads=%zu, tasks=%zu, "
          "table_id=%u)",
          __func__,
          __LINE__,
          (long)tid,
          cb_pool_->getThreadsCount(),
          cb_pool_->getQueueSize(),
          table_id_);
      cb_pool_->submitTask(bfRtStateTableOperationsCallback<T, U>,
                           dev_tgt,
                           callback_cpp,
                           callback_c,
                           cookie);
    } else {
      // Execute client's callback directly by the current thread.
      LOG_DBG("%s:%d Thread 0x%lx executes callback, table_id=%u",
              __func__,
              __LINE__,
              (long)tid,
              table_id_);
      bfRtStateTableOperationsCallback<T, U>(
          dev_tgt, callback_cpp, callback_c, cookie);
    }
  }

 private:
  void setCallbackPool(std::size_t max_threads) {
    if (max_threads) {
      if (!cb_pool_) {
        LOG_DBG(
            "%s:%d Enable callback execution pool with %zu threads, "
            "table_id=%u",
            __func__,
            __LINE__,
            max_threads,
            table_id_);
        cb_pool_ =
            std::unique_ptr<BfRtThreadPool>(new BfRtThreadPool(max_threads));
        // Direct callback execution if no memory for the pool.
      } else {
        // TODO: Adjust the running threads number dynamically
        // with more sophisticated thread pool implementation.
        if (max_threads != cb_pool_->getThreadsCount()) {
          LOG_DBG(
              "%s:%d Can't change the callback execution pool from %zu to %zu "
              "threads, table_id=%u",
              __func__,
              __LINE__,
              cb_pool_->getThreadsCount(),
              max_threads,
              table_id_);
        }
      }
    } else {
      if (cb_pool_) {
        // Keep the callback thread pool once it is allocated to avoid race
        // conditions at the callback task submit without having lock there.
        LOG_DBG(
            "%s:%d Can't stop callback execution pool with %zu threads, "
            "table_id=%u",
            __func__,
            __LINE__,
            cb_pool_->getThreadsCount(),
            table_id_);
      }
    }
  }

 private:
  // The thread pool to execute client's callback code by servicing thread(s).
  // The pool is allocated and spawns threads at the first async state retain.
  std::unique_ptr<BfRtThreadPool> cb_pool_;

 private:
  bf_rt_id_t table_id_;
  std::mutex items_lock;
  std::vector<std::shared_ptr<BfRtStateTableOperations<T, U>>> items_;
  std::size_t next_item_;     // To start lookup from
  uint32_t pool_retain_seq_;  // Common retain sequence for items.
};

template <typename T, typename U>
BfRtStateTableOperations<T, U> *BfRtStateTableOperationsPool<T, U>::getItem(
    std::size_t item_idx) {
  std::lock_guard<std::mutex> lock(items_lock);
  if (item_idx < items_.size() && (items_.at(item_idx))->isValid()) {
    return items_.at(item_idx).get();
  } else {
    LOG_ERROR("%s:%d Invalid item %zu of %zu in the state pool, table_id=%u",
              __func__,
              __LINE__,
              item_idx,
              items_.size(),
              table_id_);
    return nullptr;
  }
}

template <typename T, typename U>
bool BfRtStateTableOperationsPool<T, U>::stateTableOperationsRetain(
    T callback_cpp,
    U callback_c,
    const void *cookie,
    const bf_rt_target_t &dev_tgt,
    int grace_sec,
    std::size_t max_items,
    std::size_t max_threads,
    std::size_t &item_idx) {
  std::lock_guard<std::mutex> lock(items_lock);
  if (!pool_retain_seq_) {
    // Reset to 1 on the first retain or on the sequence roll.
    // Non zero value updates items' sequences.
    pool_retain_seq_ = 1;
  }
  if (!items_.empty()) {
    // Look up for a released or staled state object starting from
    // the beginning when nothing to reuse and the buffer is able to expand,
    // or from the last reused item making the buffer circular.
    // Doing it this way expecting the oldest items are either
    // at the beginning or after the last reused.
    auto item = items_.begin();
    std::size_t n_items = items_.size();
    // check and reset for sure
    next_item_ = (next_item_ >= n_items) ? 0 : next_item_;
    LOG_DBG("%s:%d Look up from %zu of %zu pool states, table_id=%u",
            __func__,
            __LINE__,
            next_item_,
            n_items,
            table_id_);
    std::advance(item, next_item_);
    do {
      for (; n_items && item != items_.end(); item++, n_items--) {
        if ((*item)->stateTableOperationsRetain(callback_cpp,
                                                callback_c,
                                                cookie,
                                                dev_tgt,
                                                grace_sec,
                                                pool_retain_seq_)) {
          // Calculate this way to enforce items is never a std::list.
          next_item_ = item - items_.begin();
          LOG_DBG("%s:%d State %zu retained pool_seq=%u, table_id=%u",
                  __func__,
                  __LINE__,
                  next_item_,
                  pool_retain_seq_,
                  table_id_);
          pool_retain_seq_++;
          item_idx = next_item_;
          next_item_++;
          if (next_item_ >= items_.size()) {
            next_item_ = 0;
          }
          // Start the pool if this reused item is the first one with callbacks.
          if (callback_cpp || callback_c) {
            setCallbackPool(max_threads);
          }
          return true;
        }
      }
      item = items_.begin();
    } while (n_items);
  }
  // Nothing suitable is found, try to expand items if possible.
  if (items_.size() >= max_items) {
    // next_item is kept as it is
    LOG_DBG("%s:%d No more space in the state pool, table_id=%u",
            __func__,
            __LINE__,
            table_id_);
    return false;
  }
  if (cb_pool_) {
    // Limit inbound requests by the outbound callback queue size.
    // Additional synchronous call slows down execution as well.
    LOG_DBG("%s:%d Callback queue status: table_id=%u, threads=%zu, tasks=%zu",
            __func__,
            __LINE__,
            table_id_,
            cb_pool_->getThreadsCount(),
            cb_pool_->getQueueSize());
    if (cb_pool_->getQueueSize() >= max_items) {
      // next_item is kept as it is
      LOG_DBG("%s:%d Too many callback tasks are already enqueued, table_id=%u",
              __func__,
              __LINE__,
              table_id_);
      return false;
    }
  }
  if (callback_cpp || callback_c) {
    setCallbackPool(max_threads);
  }

  auto new_item =
      std::make_shared<BfRtStateTableOperations<T, U>>(table_id_, this);
  next_item_ = 0;  // next time look again from the beginning after expansion
  item_idx = items_.size();
  items_.push_back(new_item);
  LOG_DBG("%s:%d Retain new %zu state, pool_seq=%u, table_id=%u",
          __func__,
          __LINE__,
          item_idx,
          pool_retain_seq_,
          table_id_);
  return (new_item->stateTableOperationsRetain(callback_cpp,
                                               callback_c,
                                               cookie,
                                               dev_tgt,
                                               grace_sec,
                                               pool_retain_seq_++));
}

template <typename T, typename U>
void BfRtStateTableOperationsPool<T, U>::stateTableOperationsReset(
    std::size_t item_idx) {
  std::lock_guard<std::mutex> lock(items_lock);
  if (item_idx >= items_.size()) {
    LOG_ERROR(
        "%s:%d incorrect obj_idx=%zu for pool with %zu items, table_id=%u",
        __func__,
        __LINE__,
        item_idx,
        items_.size(),
        table_id_);
    return;
  }
  items_[item_idx]->stateTableOperationsReset();
  next_item_ = item_idx;
  LOG_DBG("%s:%d Reset %zu state, table_id=%u",
          __func__,
          __LINE__,
          item_idx,
          table_id_);
}

//--------------------
template <typename T, typename U>
void BfRtStateTableOperations<T, U>::stateTableOperationsSet(
    T callback_cpp,
    U callback_c,
    const void *cookie,
    const bf_rt_target_t &dev_tgt) {
  if (!isValid()) {
    LOG_ERROR("%s:%d Invalid table operations state", __func__, __LINE__);
    return;
  }
  std::lock_guard<std::mutex> lock(state_lock);
  callback_cpp_ = callback_cpp;
  callback_c_ = callback_c;
  cookie_ = const_cast<void *>(cookie);
  dev_tgt_ = dev_tgt;
  clock_gettime(CLOCK_MONOTONIC_RAW, &time_created_);
  in_use_ = true;
}

template <typename T, typename U>
bool BfRtStateTableOperations<T, U>::stateTableOperationsRetain(
    T callback_cpp,
    U callback_c,
    const void *cookie,
    const bf_rt_target_t &dev_tgt,
    int grace_sec,
    uint32_t retain_seq) {
  if (!isValid()) {
    LOG_ERROR("%s:%d Invalid table operations state", __func__, __LINE__);
    return false;
  }
  std::lock_guard<std::mutex> lock(state_lock);
  struct timespec time_curr {
    0
  };
  clock_gettime(CLOCK_MONOTONIC_RAW, &time_curr);
  if (in_use_) {
    // Check is the state obsolete and it might be overridden.
    if (callback_cpp_ == nullptr && callback_c_ == nullptr) {
      LOG_DBG(
          "%s:%d Skip attempt to retain a synchronous call state, seq=%u, "
          "table_id=%u",
          __func__,
          __LINE__,
          retain_seq_,
          table_id_);
      return false;
    }
    if (time_created_.tv_sec > time_curr.tv_sec) {
      LOG_DBG(
          "%s:%d Skip attempt to retain a future state, seq=%u, table_id=%u",
          __func__,
          __LINE__,
          retain_seq_,
          table_id_);
      return false;  // strange state created for the future ?
    }
    if ((time_created_.tv_sec + grace_sec) > time_curr.tv_sec) {
      // Let the previous active state to get its callback properly.
      return false;
    }
    LOG_WARN(
        "%s:%d Override a stalled table operation:"
        " created time (0x%0lx+%0lx) curr time (0x%0lx+%0lx) grace=%d, seq=%u, "
        "table_id=%u",
        __func__,
        __LINE__,
        time_created_.tv_sec,
        time_created_.tv_nsec,
        time_curr.tv_sec,
        time_curr.tv_nsec,
        grace_sec,
        retain_seq_,
        table_id_);
  }
  // Take the given sequence or keep incrementing for itself
  retain_seq = (retain_seq) ? retain_seq : retain_seq_ + 1;
  LOG_DBG("%s:%d Retain seq=%u->%u, table_id=%u",
          __func__,
          __LINE__,
          retain_seq_,
          retain_seq,
          table_id_);
  retain_seq_ = retain_seq;
  dev_tgt_ = dev_tgt;
  time_created_ = time_curr;
  callback_cpp_ = callback_cpp;
  callback_c_ = callback_c;
  cookie_ = const_cast<void *>(cookie);
  in_use_ = true;
  return true;
}

template <typename T, typename U>
void BfRtStateTableOperations<T, U>::stateTableOperationsReset() {
  bf_rt_target_t empty_tgt{0};
  if (!isValid()) {
    LOG_ERROR("%s:%d Invalid table operations state", __func__, __LINE__);
    return;
  }
  std::lock_guard<std::mutex> lock(state_lock);
  callback_cpp_ = nullptr;
  callback_c_ = nullptr;
  cookie_ = nullptr;
  dev_tgt_ = empty_tgt;
  time_created_.tv_sec = 0;
  time_created_.tv_nsec = 0;
  in_use_ = false;
}

template <typename T, typename U>
std::tuple<T, U, void *, bf_rt_target_t>
BfRtStateTableOperations<T, U>::stateTableOperationsGet() {
  bf_rt_target_t empty_tgt{0};
  if (!isValid()) {
    LOG_ERROR("%s:%d Invalid table operations state", __func__, __LINE__);
    return std::make_tuple(nullptr, nullptr, nullptr, empty_tgt);
  }
  std::lock_guard<std::mutex> lock(state_lock);
  return std::make_tuple(
      callback_cpp_, callback_c_, cookie_, (bf_rt_target_t)dev_tgt_);
}

template <typename T, typename U>
std::tuple<T, U, void *, bf_rt_target_t, BfRtStateTableOperationsPool<T, U> *>
BfRtStateTableOperations<T, U>::stateTableOperationsRelease() {
  bf_rt_target_t empty_tgt{0};
  if (!isValid()) {
    LOG_ERROR("%s:%d Invalid table operations state", __func__, __LINE__);
    return std::make_tuple(nullptr, nullptr, nullptr, empty_tgt, nullptr);
  }
  std::lock_guard<std::mutex> lock(state_lock);
  if (!in_use_) {
    LOG_WARN("%s:%d Already released operations state, seq=%u, table_id=%u",
             __func__,
             __LINE__,
             retain_seq_,
             table_id_);
    return std::make_tuple(nullptr, nullptr, nullptr, empty_tgt, nullptr);
  }
  if (callback_cpp_ == nullptr && callback_c_ == nullptr) {
    LOG_WARN(
        "%s:%d Attempt to release a synchronous call state, seq=%u, "
        "table_id=%u",
        __func__,
        __LINE__,
        retain_seq_,
        table_id_);
    return std::make_tuple(nullptr, nullptr, nullptr, empty_tgt, nullptr);
  }
  auto state_ = std::make_tuple(
      callback_cpp_, callback_c_, cookie_, dev_tgt_, parent_pool_);
  callback_cpp_ = nullptr;
  callback_c_ = nullptr;
  cookie_ = nullptr;
  dev_tgt_ = empty_tgt;
  time_created_.tv_sec = 0;
  time_created_.tv_nsec = 0;
  in_use_ = false;
  LOG_DBG("%s:%d Released seq=%u, table_id=%u",
          __func__,
          __LINE__,
          retain_seq_,
          table_id_);
  return state_;
}

}  // namespace bfrt

#endif  // _BF_RT_TBL_OPERATIONS_STATE_HPP
