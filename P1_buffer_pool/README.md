# P1 — Buffer Pool Manager

## 一、实验目标

在 BusTub 中实现存储管理器的三个核心组件，使其支持页面的缓冲、替换与磁盘交互：

1. **可扩展哈希表**（`ExtendibleHashTable`）——对外提供 `page_id → frame_id` 的映射，禁止使用 `unordered_map` 等现成容器。
2. **LRU-K 替换器**（`LRUKReplacer`）——按 backward k-distance 选择淘汰帧。
3. **缓冲池管理器**（`BufferPoolManagerInstance`）——管理固定数量的帧，负责页面读入、淘汰、写回与删除。

## 二、实验环境

| 项目 | 版本 |
|---|---|
| 操作系统 | Ubuntu 24.04（WSL2） |
| 编译器 | GCC 13.3 |
| 构建工具 | CMake 3.28 + GNU Make |
| 上游工程 | BusTub `v20221128-2022fall` |
| 测试框架 | GoogleTest |

## 三、实现要点

### 1. 可扩展哈希表

- **哈希函数**：`std::hash<K>()(key) & ((1 << global_depth_) - 1)`，只取低若干位作为目录下标。
- **查找**：用 `GetGlobalDepthInternal()` 算掩码，定位目录项，再在桶内线性查找 key。
- **插入**：
  1. 先定位目标桶并检查 key 是否已存在——**存在则直接覆盖，不触发拆分**；
  2. 桶未满则直接插入；
  3. 桶满时拆分：若 `局部深度 == 全局深度`，先 `global_depth_++` 并把目录 `resize(capacity << 1)`、后半段指向与前半段相同的桶；
  4. 桶的局部深度自增，按**新参与索引的那一位**（`1 << old_depth`）把桶内元素分到两个新桶；
  5. 更新目录中所有指向旧桶的指针，`num_buckets_++`；
  6. 外层 `while (true)` 保证一次插入触发多次拆分时仍能正确落位。
- **删除**：从桶中移除 key，不收缩目录（实验未要求）。
- **并发**：所有公开方法持有 `std::mutex latch_`。

### 2. LRU-K 替换器

- 每个帧维护一个**最近 K 次访问的时间戳队列**（`std::list<size_t>`），以及 `is_evictable_` 标记。
- 全局单调递增计数器 `current_timestamp_`，每次 `RecordAccess` 自增并压入队列，队列长度超过 K 时弹出最早的。
- **淘汰规则**（`Evict`）：
  - 访问次数不足 K 次的帧，其 backward k-distance 视作 **+inf**，**优先淘汰**；多个 +inf 时淘汰**最早访问**（队列首元素最小）的那个；
  - 只有访问次数满 K 次的帧才参与 k-distance 比较，取 `current_timestamp_ - 最早时间戳` **最大**者。
- `SetEvictable` / `Remove` 维护 `curr_size_`（可淘汰帧计数），`Remove` 对不可淘汰帧抛 `Exception`。

### 3. 缓冲池管理器

- **取帧**：优先从 `free_list_` 取；为空则调 `replacer_->Evict()`；被淘汰的帧若是脏页，先 `disk_manager_->WritePage()` 写回，再 `ResetMemory()` 并 `page_table_->Remove()`。
- **FetchPage / NewPage**：写 `page_table_`、置 `pin_count_ = 1`、清 dirty，并 `RecordAccess` + `SetEvictable(false)`。
- **UnpinPage**：`pin_count_` 减到 0 时 `SetEvictable(true)`；只允许把 dirty 从 false 改成 true。
- **FlushPage**：写回磁盘并清 dirty；**FlushAllPages** 直接遍历所有帧自行写回，不调用 `FlushPage`，避免 `std::mutex` 重入死锁。
- **DeletePage**：`pin_count_ > 0` 返回 `false`；否则从 page_table 与 replacer 移除、重置帧、归还 free_list、通知磁盘管理器释放页。

## 四、遇到的问题与解决

| 现象 | 原因 | 解决 |
|---|---|---|
| `Compatibility with CMake < 3.5 has been removed` | CMake 4.x 拒绝旧版 `cmake_minimum_required`（第三方库） | 配置时加 `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` |
| `possibly dangling reference to a temporary [-Werror=dangling-reference]` | GCC 13 新增告警 + 工程全局 `-Werror` | 去掉 `CMakeLists.txt` 中的 `-Werror` |
| `found ':' in nested-name-specifier` / `'uint64_t' has not been declared` | 头文件缺 `<cstdint>` | 在 `simplified_token.h`、`string_util.h` 补 `#include <cstdint>` |
| 测试显示 `0 tests from 0 test suites ran` | 官方用例全部标了 `DISABLED_` 前缀 | 运行时加 `--gtest_also_run_disabled_tests` |
| `FlushAllPagesImpl` 卡死 | 内部调用 `FlushPageImpl` 造成 `std::mutex` 二次加锁 | 改为直接遍历帧写回 |
| 插入已存在的 key 却触发了拆桶 | 未先查重 | 插入前遍历桶内元素，命中即覆盖返回 |
| WSL 内 `git clone` 报 `GnuTLS recv error (-110)` | WSL 是独立虚拟机，不继承 Windows 的 localhost 代理 | 开启 VPN 的全局/虚拟网卡模式后重试，或改用镜像源 |

## 五、测试结果

```bash
cd ~/bustub/build && make extendible_hash_table_test lru_k_replacer_test buffer_pool_manager_instance_test -j2
cd ~/bustub && find . -type f -name "*_test" -exec {} --gtest_also_run_disabled_tests \;
```

```
[  PASSED  ] ExtendibleHashTableTest.DISABLED_SampleTest
[  PASSED  ] ExtendibleHashTableTest.DISABLED_ConcurrentInsertTest      (24 ms)
[  PASSED  ] LRUKReplacerTest.DISABLED_SampleTest
[  PASSED  ] BufferPoolManagerInstanceTest.DISABLED_BinaryDataTest
[  PASSED  ] BufferPoolManagerInstanceTest.DISABLED_SampleTest
```

官方 5 个用例全部通过（含并发插入与二进制数据用例）。

## 六、可改进之处

- 目前哈希表只支持扩容、不收缩；数据量大幅下降时目录与桶会一直占用内存。作业未要求，故未实现。
- 桶内查找为线性扫描，桶较大时可改为有序结构以降低查找开销。
- 缓冲池的 `free_list_` 与 `page_table_` 操作可进一步拆分细粒度锁，以减少高并发下的锁竞争。

## 七、文件对应关系

```
src/
├── buffer/
│   ├── buffer_pool_manager_instance.cpp
│   └── lru_k_replacer.cpp
├── container/hash/extendible_hash_table.cpp
└── include/
    ├── buffer/
    │   ├── buffer_pool_manager_instance.h
    │   └── lru_k_replacer.h
    └── container/hash/extendible_hash_table.h
```