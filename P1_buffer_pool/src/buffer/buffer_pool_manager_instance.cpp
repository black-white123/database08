//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// buffer_pool_manager_instance.cpp
//
// Identification: src/buffer/buffer_pool_manager.cpp
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "buffer/buffer_pool_manager_instance.h"

#include "common/exception.h"
#include "common/macros.h"

namespace bustub {

BufferPoolManagerInstance::BufferPoolManagerInstance(size_t pool_size, DiskManager *disk_manager, size_t replacer_k,
                                                     LogManager *log_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager), log_manager_(log_manager) {
  // we allocate a consecutive memory space for the buffer pool
  pages_ = new Page[pool_size_];
  page_table_ = new ExtendibleHashTable<page_id_t, frame_id_t>(bucket_size_);
  // initially, every page is not in the buffer pool and all the page slots are in the free list
  replacer_ = new LRUKReplacer(pool_size, replacer_k);

  // Initially, every page is in the free list.
  for (size_t i = 0; i < pool_size_; ++i) {
    free_list_.emplace_back(static_cast<int>(i));
  }
}

BufferPoolManagerInstance::~BufferPoolManagerInstance() {
  delete[] pages_;
  delete page_table_;
  delete replacer_;
}

/**
 * @brief 新建一页：优先从 free_list 取空闲帧，其次经 LRU-K 驱逐；全部被固定时返回 nullptr。
 */
auto BufferPoolManagerInstance::NewPgImp(page_id_t *page_id) -> Page * {
  std::scoped_lock<std::mutex> lock(latch_);

  frame_id_t frame_id;

  // 优先从空闲列表取帧
  if (!free_list_.empty()) {
    frame_id = free_list_.front();
    free_list_.pop_front();
  } else {
    // 没有空闲帧则尝试驱逐
    if (!replacer_->Evict(&frame_id)) {
      return nullptr;
    }

    // 被驱逐页：脏页写回磁盘，然后从页表移除
    page_id_t evicted_page_id = pages_[frame_id].GetPageId();
    if (pages_[frame_id].IsDirty()) {
      disk_manager_->WritePage(evicted_page_id, pages_[frame_id].GetData());
      pages_[frame_id].is_dirty_ = false;
    }
    pages_[frame_id].ResetMemory();
    page_table_->Remove(evicted_page_id);
  }

  // 分配新页号并初始化该帧
  *page_id = AllocatePage();
  page_table_->Insert(*page_id, frame_id);

  pages_[frame_id].page_id_ = *page_id;
  pages_[frame_id].is_dirty_ = false;
  pages_[frame_id].pin_count_ = 1;

  replacer_->RecordAccess(frame_id);
  replacer_->SetEvictable(frame_id, false);

  return &pages_[frame_id];
}

/**
 * @brief 取页：先查页表命中直接返回；否则从 free_list / 驱逐腾帧后从磁盘读入。
 */
auto BufferPoolManagerInstance::FetchPgImp(page_id_t page_id) -> Page * {
  std::scoped_lock<std::mutex> lock(latch_);

  frame_id_t frame_id;

  // 命中缓冲池
  if (page_table_->Find(page_id, frame_id)) {
    pages_[frame_id].pin_count_++;
    replacer_->RecordAccess(frame_id);
    replacer_->SetEvictable(frame_id, false);
    return &pages_[frame_id];
  }

  // 未命中：先取空闲帧，再尝试驱逐
  if (!free_list_.empty()) {
    frame_id = free_list_.front();
    free_list_.pop_front();
  } else {
    if (!replacer_->Evict(&frame_id)) {
      return nullptr;
    }

    page_id_t evicted_page_id = pages_[frame_id].GetPageId();
    if (pages_[frame_id].IsDirty()) {
      disk_manager_->WritePage(evicted_page_id, pages_[frame_id].GetData());
      pages_[frame_id].is_dirty_ = false;
    }
    pages_[frame_id].ResetMemory();
    page_table_->Remove(evicted_page_id);
  }

  // 从磁盘读入目标页
  disk_manager_->ReadPage(page_id, pages_[frame_id].GetData());
  page_table_->Insert(page_id, frame_id);

  pages_[frame_id].page_id_ = page_id;
  pages_[frame_id].is_dirty_ = false;
  pages_[frame_id].pin_count_ = 1;

  replacer_->RecordAccess(frame_id);
  replacer_->SetEvictable(frame_id, false);

  return &pages_[frame_id];
}

/**
 * @brief 解固定一页：pin_count 减一；降到 0 时标记可淘汰；按 is_dirty 更新脏标志。
 */
auto BufferPoolManagerInstance::UnpinPgImp(page_id_t page_id, bool is_dirty) -> bool {
  std::scoped_lock<std::mutex> lock(latch_);

  frame_id_t frame_id;
  if (!page_table_->Find(page_id, frame_id)) {
    return false;
  }

  if (pages_[frame_id].GetPinCount() <= 0) {
    return false;
  }

  // dirty 标志只允许从 false 置 true，不清除已有脏标志
  if (is_dirty) {
    pages_[frame_id].is_dirty_ = true;
  }

  pages_[frame_id].pin_count_--;
  if (pages_[frame_id].pin_count_ == 0) {
    replacer_->SetEvictable(frame_id, true);
  }

  return true;
}

/**
 * @brief 将指定页刷盘：无论脏标志都写回，写完后清除脏标志。
 */
auto BufferPoolManagerInstance::FlushPgImp(page_id_t page_id) -> bool {
  if (page_id == INVALID_PAGE_ID) {
    return false;
  }

  std::scoped_lock<std::mutex> lock(latch_);

  frame_id_t frame_id;
  if (!page_table_->Find(page_id, frame_id)) {
    return false;
  }

  disk_manager_->WritePage(page_id, pages_[frame_id].GetData());
  pages_[frame_id].is_dirty_ = false;
  return true;
}

/**
 * @brief 将缓冲池中所有页刷盘（逐帧直接处理，避免重入死锁）。
 */
void BufferPoolManagerInstance::FlushAllPgsImp() {
  std::scoped_lock<std::mutex> lock(latch_);

  for (size_t i = 0; i < pool_size_; i++) {
    page_id_t page_id = pages_[i].GetPageId();
    if (page_id == INVALID_PAGE_ID) {
      continue;
    }
    frame_id_t frame_id;
    if (page_table_->Find(page_id, frame_id) && static_cast<size_t>(frame_id) == i) {
      disk_manager_->WritePage(page_id, pages_[i].GetData());
      pages_[i].is_dirty_ = false;
    }
  }
}

/**
 * @brief 删除一页：被固定返回 false；成功则清理页表、替换器、重置帧并归还 free_list。
 */
auto BufferPoolManagerInstance::DeletePgImp(page_id_t page_id) -> bool {
  std::scoped_lock<std::mutex> lock(latch_);

  frame_id_t frame_id;
  if (!page_table_->Find(page_id, frame_id)) {
    return true;
  }

  if (pages_[frame_id].GetPinCount() > 0) {
    return false;
  }

  // 从替换器移除（帧此刻必然可淘汰，因为 pin_count == 0）
  replacer_->Remove(frame_id);

  pages_[frame_id].ResetMemory();
  pages_[frame_id].page_id_ = INVALID_PAGE_ID;
  pages_[frame_id].pin_count_ = 0;
  pages_[frame_id].is_dirty_ = false;

  page_table_->Remove(page_id);
  free_list_.push_back(frame_id);
  DeallocatePage(page_id);

  return true;
}

auto BufferPoolManagerInstance::AllocatePage() -> page_id_t { return next_page_id_++; }

}  // namespace bustub
