# database08 — 数据库实验

本仓库按实验分目录存放，每个目录包含**该次实验改动的源码**和一份实验报告（README.md）。
源码目录结构与 BusTub 上游工程的 `src/` 完全一致，可直接覆盖回工程使用。

## 实验索引

| 实验 | 内容 | 状态 | 版本标签 | 日期 |
|---|---|---|---|---|
| P1 | Buffer Pool Manager（可扩展哈希表 / LRU-K / 缓冲池管理器） | ✅ 完成 | `p1-submit` | 2026-11 |

## 如何把某次实验的源码放回工程

以 P1 为例：

```bash
git clone --depth 1 --branch v20221128-2022fall https://github.com/cmu-db/bustub.git ~/bustub
cp -r P1_buffer_pool/src/* ~/bustub/src/
```

## 目录说明

```
.
├── P1_buffer_pool/
│   ├── README.md     实验报告：目标、实现思路、问题与解决、测试结果
│   └── src/          本次实验改动的源码（层级同 bustub/src）
└── README.md         本文件
```