//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// lru_k_replacer.cpp
//
// Identification: src/buffer/lru_k_replacer.cpp
//
// Copyright (c) 2015-2022, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "buffer/lru_k_replacer.h"

#include "common/exception.h"

namespace bustub {

LRUKReplacer::LRUKReplacer(size_t num_frames, size_t k) : replacer_size_(num_frames), k_(k) {}

/**
 * @brief 淘汰 backward k-distance 最大的可淘汰帧。
 * backward k-distance = 当前时间戳 - 第 k 次最近访问的时间戳；
 * 访问次数不足 k 次的帧 backward k-distance 视作 +inf（优先级高于任何有限距离）；
 * 多个 +inf 帧中淘汰整体时间戳最早（最久未访问）的帧。
 */
auto LRUKReplacer::Evict(frame_id_t *frame_id) -> bool {
  std::scoped_lock<std::mutex> lock(latch_);

  frame_id_t victim = -1;
  bool found = false;
  bool victim_is_inf = false;
  size_t victim_first_ts = 0;   // inf 组比较用：最早访问时间戳
  size_t victim_distance = 0;   // 有限组比较用：backward k-distance

  for (const auto &pair : node_store_) {
    const frame_id_t fid = pair.first;
    const FrameEntry &entry = pair.second;
    if (!entry.is_evictable_) {
      continue;
    }

    const bool is_inf = entry.history_.size() < k_;
    if (is_inf) {
      // +inf 组：距离比任何有限组都大；组内取首次访问最早者
      const size_t first_ts = entry.history_.front();
      if (!found || !victim_is_inf || first_ts < victim_first_ts) {
        found = true;
        victim_is_inf = true;
        victim_first_ts = first_ts;
        victim = fid;
      }
    } else {
      // 有限组：history_ 只保留最近 k 个，front() 即第 k 次最近访问
      const size_t distance = current_timestamp_ - entry.history_.front();
      if (!found) {
        found = true;
        victim_is_inf = false;
        victim_distance = distance;
        victim = fid;
      } else if (!victim_is_inf && distance > victim_distance) {
        victim_is_inf = false;
        victim_distance = distance;
        victim = fid;
      }
      // 已有 inf 候选时，有限组不能取代它（inf 距离更大）
    }
  }

  if (!found) {
    return false;
  }

  curr_size_--;
  node_store_.erase(victim);
  *frame_id = victim;
  return true;
}

/**
 * @brief 记录帧在当前时间戳的一次访问。
 */
void LRUKReplacer::RecordAccess(frame_id_t frame_id) {
  std::scoped_lock<std::mutex> lock(latch_);

  if (static_cast<size_t>(frame_id) >= replacer_size_) {
    throw Exception(ExceptionType::OUT_OF_RANGE, "frame_id out of range");
  }

  current_timestamp_++;
  auto &entry = node_store_[frame_id];
  entry.history_.push_back(current_timestamp_);

  // 只保留最近 k 个时间戳
  while (entry.history_.size() > k_) {
    entry.history_.pop_front();
  }
}

/**
 * @brief 设置帧是否可淘汰，并维护 replacer 大小。
 */
void LRUKReplacer::SetEvictable(frame_id_t frame_id, bool set_evictable) {
  std::scoped_lock<std::mutex> lock(latch_);

  if (static_cast<size_t>(frame_id) >= replacer_size_) {
    throw Exception(ExceptionType::OUT_OF_RANGE, "frame_id out of range");
  }

  auto it = node_store_.find(frame_id);
  if (it == node_store_.end()) {
    return;
  }

  auto &entry = it->second;
  if (entry.is_evictable_ == set_evictable) {
    return;
  }
  curr_size_ = set_evictable ? curr_size_ + 1 : curr_size_ - 1;
  entry.is_evictable_ = set_evictable;
}

/**
 * @brief 直接移除指定帧及其访问历史（与 Evict 不同，不看 k-distance）。
 */
void LRUKReplacer::Remove(frame_id_t frame_id) {
  std::scoped_lock<std::mutex> lock(latch_);

  auto it = node_store_.find(frame_id);
  if (it == node_store_.end()) {
    return;
  }

  if (!it->second.is_evictable_) {
    throw Exception(ExceptionType::OUT_OF_RANGE, "cannot remove a non-evictable frame");
  }

  curr_size_--;
  node_store_.erase(it);
}

auto LRUKReplacer::Size() -> size_t {
  std::scoped_lock<std::mutex> lock(latch_);
  return curr_size_;
}

}  // namespace bustub
