#pragma once
// ============================================================
// aura_rt/gc.h ─ 向后兼容转发头文件
//
// 原 gc.h 已拆分到 gc/ 文件夹：
//   - gc/gc.h       GcHeap 类主声明 + 便捷接口
//   - gc/handles.h  GcRootHandle/GcWeakHandle/GcSharedRoot 等模板
//   - gc/*.cpp      按功能区分散实现（alloc/tlab/roots/safepoint/mark_sweep/compact）
//
// 保留此文件是为了让 `#include "gc.h"` 的外部代码无需修改。
// ============================================================
#include "gc/gc.h"
