# change.md — GC 并发数据竞争修复

> 审计发现 5 处共享数据未正确保护，导致多线程下 UAF / 数据竞争。
> 修复策略：补锁 + atomic + call_once。

## 修改文件清单

| 文件 | 改动 |
|:---|:---|
| runtime/gc.h | compactSuspendedCount_→atomic, gcPending_→atomic, 删 oomInit_ 改 once_flag, 新增 stackRootsM_/rememberedSetM_ |
| runtime/gc.cpp | ensureOomError 用 call_once, register/unregisterStackRoots 加锁, writeBarrier 加锁, gcPending_ 用 load/store |

---

## Part A：runtime/gc.h

### A1. compactSuspendedCount_ 改为 atomic
```cpp
// 原：int compactSuspendedCount_ = 0;
std::atomic<int> compactSuspendedCount_{0};
```
注：incCompactSuspend/decCompactSuspend 的 `++`/`--` 对 atomic 自动原子，无需改。

### A2. gcPending_ 改为 atomic
```cpp
// 原：bool gcPending_ = false;
std::atomic<bool> gcPending_{false};
```

### A3. ensureOomError 改用 atomic<bool>（call_once 不可行）
**call_once 不可行原因**：make_string → alloc → tryAlloc → ensureOomError 会递归调用
自身，call_once 在 lambda 执行期间持锁，递归调用同一 once_flag 会自死锁。
**改用 atomic<bool> oomInit_**：递归调用被 oomInit_.load() 挡掉，fast path 检查
oomError_.kind 挡并发，无锁无死锁。
```cpp
// 删除：bool oomInit_ = false;
std::atomic<bool> oomInit_{false};
```

### A4. 新增 stackRootsM_ 保护 stackRoots_
```cpp
std::vector<std::pair<void*, void*>> stackRoots_;
std::mutex              stackRootsM_;  // 保护 register/unregisterStackRoots 并发
```

### A5. 新增 rememberedSetM_ 保护 rememberedSet_
```cpp
std::set<GcObject*> rememberedSet_;
std::mutex          rememberedSetM_;  // 保护 writeBarrier 中 insert 并发
```

---

## Part B：runtime/gc.cpp

### B1. ensureOomError 用 atomic<bool>（call_once 会自死锁）
```cpp
void GcHeap::ensureOomError() {
    if (oomError_.kind) return;  // 已初始化（fast path）
    if (oomInit_.load()) return;  // 递归防护
    oomInit_.store(true);
    oomError_.kind    = make_string("OutOfMemoryError");
    oomError_.message = make_string("memory exhausted after GC");
    oomInit_.store(false);
}
```

### B2. register/unregisterStackRoots 加锁
```cpp
void GcHeap::registerStackRoots(void* begin, void* end) {
    std::lock_guard<std::mutex> lk(stackRootsM_);
    stackRoots_.push_back({begin, end});
}
void GcHeap::unregisterStackRoots(void* begin, void* end) {
    std::lock_guard<std::mutex> lk(stackRootsM_);
    auto it = std::find_if(...);
    if (it != stackRoots_.end()) stackRoots_.erase(it);
}
```

### B3. writeBarrier 加锁
```cpp
void GcHeap::writeBarrier(...) {
    if (parent && parent->generation() == 1 && newVal && newVal->generation() == 0) {
        std::lock_guard<std::mutex> lk(rememberedSetM_);
        rememberedSet_.insert(parent);
    }
}
```

### B4. gcPending_ 访问改为 .load()/.store()
所有 `gcPending_` 读取 → `.load()`，赋值 → `.store()`。

### B5. compactSuspendedCount_ 读取改为 .load()
`compactSuspendedCount_ == 0` → `.load() == 0`，`> 0` → `.load() > 0`。

---

## 实施步骤

| Step | 操作 |
|:---|:---|
| 1 | gc.h: compactSuspendedCount_ → atomic<int> |
| 2 | gc.h: gcPending_ → atomic<bool> |
| 3 | gc.h: 删 oomInit_，加 oomOnceFlag_ |
| 4 | gc.h: 新增 stackRootsM_ / rememberedSetM_ |
| 5 | gc.cpp: ensureOomError 用 call_once |
| 6 | gc.cpp: register/unregisterStackRoots 加锁 |
| 7 | gc.cpp: writeBarrier 加锁 |
| 8 | gc.cpp: gcPending_ 全部改 load/store |
| 9 | gc.cpp: compactSuspendedCount_ 读取改 load |
| 10 | 编译 + ASAN 测试 |
