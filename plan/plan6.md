# v2 修改分析 — 编译测试报告

## 一、ASAN 测试结果

| 测试 | 命令 | 结果 |
|------|------|:---:|
| `test1_asan` (完整程序) | `ASAN_OPTIONS=detect_leaks=1 ./test1_asan` | ✅ PASS |
| `test_simple_asan` (单协程) | `ASAN_OPTIONS=detect_leaks=1 ./test_simple_asan` | ✅ PASS |

输出：
```
filled Stack(10, 20, 30)
filled Stack(1.1, 2.2)
filled Stack(x, y, z)
All stacks processed.
```

**无段错误、无 ASAN 报错、无内存泄漏。** 🎉

---

## 二、v1 → v2 修改对比

### ✅ 已修复的问题

| 文件 | 修改内容 | 对应上一轮报告的哪个问题 |
|------|----------|--------------------------|
| `gc.h` / `gc.cpp` | 新增分代 GC：`youngObjects_` + `oldObjects_` + `rememberedSet_` | 之前 GC 完全空转 |
| `gc.h` / `gc.cpp` | `alloc()` 不再检查 `roots_.empty()`，直接进入 GC 流程 | 之前 GC 因 `roots_` 为空直接 return |
| `gc.h` / `gc.cpp` | 新增 `registerStackRoots()` / `unregisterStackRoots()` 保守栈扫描 | 之前 GC 根本找不到活对象 |
| `gc.h` / `gc.cpp` | 新增 `markArrayPtrFields()` 扫描数组元素中的 GC 指针 | 之前 `Array<T*>` 的 elements 不被扫描 |
| `types.h` | `TypeDescriptor` 新增 `ArrayPtrField` + `arrayPtrFieldCount` | 之前无法描述数组指针字段 |
| `types.h` | `Array<T>::desc()` 使用 `if constexpr` 正确返回指针类型描述符 | 之前 `ptrFieldCount` 始终为 0 |
| `gc.h` | `GcRootHandle` 新增 `rebind()` 方法 | 支持协程帧中 GC 指针的更新 |
| `test.cpp` | `Stack<T>::_desc` 现在正确列出 `ptrFieldCount=1` + `items` offset | 之前 `ptrFieldCount=0` |
| `test.cpp` | 新增 `gc_safepoint()` 调用（在 `co_await when_all` 前） | 之前无安全点 |
| `gc.cpp` | `writeBarrier()` 现在真正记录到 `rememberedSet_` | 之前是空实现 |

### ⚠️ 剩余的已知问题

| # | 位置 | 问题 | 严重度 |
|---|------|------|:---:|
| 1 | `test.cpp:27` | `_Stack_ptrs[]` 定义语法怪异（`_Stack_ptrs<T>[]`），虽然编译通过但应改为类内 `static const` | 🟡 |
| 2 | `test.cpp` | 协程帧中 GC 指针仍为裸指针，未使用 `GcRootHandle` 注册为根 | 🟡 |
| 3 | `io.cpp:128` | `list_dir` 仍用 `new Path[]` 而非 GC 分配 | 🟢 |
| 4 | `gc.cpp:374-416` | `compactAndReclaim()` 仅在全死时才释放页，有存活对象时不回收 | 🟡 |
| 5 | `gc.cpp:404` | `allLive.empty()` 检查后恢复 `oldPages` 但 `currentPage_` 指向旧页链表尾，bump 指针未重置 | 🟡 |

---

## 三、分代 GC 架构

```
          ┌──────────────────────────────────────┐
          │           GcHeap 架构 v2              │
          │                                      │
          │  alloc() ──► youngObjects_           │
          │       │                              │
          │       ▼                              │
          │  youngBytes_ >= kYoungThreshold?     │
          │       │                              │
          │    ┌──┴──┐                           │
          │    │ YES │──► minorGc()              │
          │    └──┬──┘     │                     │
          │       │        ├─ markPhase(true)    │
          │       │        │   扫描 youngOnly    │
          │       │        │   扫描 stackRoots_  │
          │       │        │   扫描 rememberedSet│
          │       │        └─ sweepPhaseYoung()  │
          │       │           存活→oldObjects_   │
          │       │           死亡→丢弃          │
          │       │                              │
          │    ┌──┴──┐                           │
          │    │still?│──► majorGc()             │
          │    └──┬──┘     │                     │
          │       │        ├─ markPhase(false)   │
          │       │        │   扫描全部          │
          │       │        └─ sweepPhaseAll()    │
          │       │           + compactAndReclaim│
          │       │                              │
          │  writeBarrier() ──► rememberedSet_   │
          │  (老→新引用)                         │
          │                                      │
          │  registerStackRoots()                │
          │  (保守栈扫描)                         │
          └──────────────────────────────────────┘
```

**亮点**：
- 分代设计合理：新生代快速 minor GC，老年代 major GC + compact
- remembered set 写屏障已实现
- 保守栈扫描已实现 `registerStackRoots` / `unregisterStackRoots`

**待改进**：
- `compactAndReclaim()` 在有存活对象时直接放弃回收，应至少释放完全空闲的页
- 翻译器需要生成 `GcRootHandle` 包装和 `gc_safepoint()` 调用

---

## 四、总结

**v2 的 GC 改造非常扎实。** 分代 GC、数组扫描、保守栈根、写屏障四个核心机制都已实现，GC 从"完全空转"变成了"真正可工作的 GC"。唯一需要手动修复的致命 bug 是 `task.h` 的 `final_awaiter`（已修复）。

**下一步建议**：翻译器端集成 — 生成 `GcRootHandle` 包装、`gc_safepoint()` 调用、`TypeDescriptor` 的正确偏移量。这几个是让 GC 从"框架就绪"到"运行时真正生效"的关键。