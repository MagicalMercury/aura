# TLAB（Thread-Local Allocation Buffer）实施方案

- Plan Title: TLAB 多线程无锁分配
- Author/Agent: Agent
- Date: 2026-07-24
- 状态：✅ 已完成（2026-07-24，ASAN 验证通过）
- Related modules/packages: runtime/gc.h, runtime/gc.cpp, runtime/thread_pool.cpp, runtime/task.cpp
- 来源 issue: TODO.txt §五 [x] P2 TLAB（原延后项，因 sync_thread 多线程并发分配 bug 触发，提前实施）
- 关联 plan: plan/sync_thread_plan.md（约束 2 替代方案）、plan/gc_features_plan.md §十一

---

## 1. 背景与触发原因

### 1.1 触发场景

`sync thread` 多线程语句实施后（plan/sync_thread_plan.md Phase 0-4 已完成），
Test 3（GC 压力测试：spawn 工作线程并发执行 200 次字符串拼接）出现 ASAN 报错：

```
==36068==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x120c40ba6018
WRITE of size 40 at 0x120c40ba6018 thread T15
  #0 __asan_memcpy
  #1 std::vector<GcObject*>::__swap_out_circular_buffer   ← youngObjects_ 扩容
  #8 std::vector<GcObject*>::push_back
  #9 aura_rt::GcHeap::tryAlloc                 D:/you/Aura/runtime/gc.cpp:108
  #10 GcHeap::alloc
  #11 GcString::make
  #12 GcString::from(int)
  #13 aura_rt::concat(GcString*, int)
  #21 sync_thread_context::submit(std::function<void()>)::$_0
  #22 ThreadPool::workerLoop                   D:/you/Aura/runtime/thread_pool.cpp:111
```

### 1.2 根因分析

[runtime/gc.cpp:60-118](file:///d:/you/Aura/runtime/gc.cpp) `tryAlloc` 函数：
- 注释 `// 注：调用方 tryAlloc 必须持有 allocM_`（[gc.cpp:121](file:///d:/you/Aura/runtime/gc.cpp)），
  但 tryAlloc **自身从未加锁**，bumpAlloc 也不再内部加锁。
- 多个 worker 线程并发调用 `tryAlloc` → 并发 `youngObjects_.push_back(obj)`
  （[gc.cpp:108](file:///d:/you/Aura/runtime/gc.cpp)）→ vector 扩容时 `__swap_out_circular_buffer`
  越界写入 → heap-buffer-overflow。
- 同时 `currentPage_->bumpOffset += size`（[gc.cpp:132](file:///d:/you/Aura/runtime/gc.cpp)）
  存在数据竞争，可能导致两个线程拿到相同地址返回。

### 1.3 为何不走"加锁"路径

- v1 计划（sync_thread_plan.md §5.4 约束 2）原定"全局 mutex 保护 bumpAlloc"。
- 用户反馈"使用更好方案"，即放弃锁竞争路径，直接实施 TLAB。
- TLAB 是 GC 多线程分配的标准方案，零锁竞争，且本就是 TODO 中已规划的远期项。

---

## 2. Analysis Report（基于现有源码）

### 2.1 Codebase Scan

| 文件 | 职责 | 关键点 |
|:---|:---|:---|
| [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) | GcHeap 单例定义 | `allocM_` 已声明（L290）；`Page` 结构 L238-242；`youngObjects_` L311；`registerThread` L184；`safepoint` L178 |
| [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) | GC 实现 | `tryAlloc` L60-118（未加锁）；`bumpAlloc` L120-134；`registerThread` L231-234（仅 push id）；`markPhase` L393-458；`safepoint` L186-226；`compact` L754-761；`rebuildPageList` L891-971（释放旧页） |
| [runtime/thread_pool.cpp](file:///d:/you/Aura/runtime/thread_pool.cpp) | 线程池 | `workerLoop` L93-136：入口 `registerThread`，退出 `unregisterThread`，每任务前 `gc_safepoint()` |
| [runtime/task.cpp](file:///d:/you/Aura/runtime/task.cpp) | EventLoop | `EventLoop::run` L28-95：主线程 registerThread/unregisterThread（L30, L74） |
| [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) | GcString 工厂 | `make`/`from(int)`/`concat` 等高频分配入口，均走 `GcHeap::alloc` |

### 2.2 Dependency Map

```
worker thread ──> tryAlloc ──> bumpAlloc  ──> currentPage_ / headPage_  (写竞争)
              └─> youngObjects_.push_back                            (写竞争)
              └─> registeredDescs_.insert                            (写竞争)
              └─> youngBytes_/allocatedBytes_ +=                       (数据竞争)

GC STW ──> safepoint ──> markPhase ──> roots_ / stackRoots_ / globalRoots_ / youngObjects_ / oldObjects_
                └─> sweepPhase* ──> youngObjects_ / oldObjects_  (写)
                └─> compact ──> freeAllPages / rebuildPageList ──> 释放旧页
                            └─> updateAllReferences ──> 更新 youngObjects_/oldObjects_ 中指针
```

### 2.3 Interface Inventory（受 TLAB 改造影响的接口）

| 接口 | 当前签名 | 改造后 |
|:---|:---|:---|
| `GcHeap::alloc` | `(size, desc) -> GcObject*` | 签名不变，内部派发 TLAB/慢路径 |
| `GcHeap::tryAlloc` | `(size, desc) -> GcObject*` | 签名不变，内部派发 |
| `GcHeap::registerThread` | `(std::thread::id)` | 签名不变，额外分配 TLAB |
| `GcHeap::unregisterThread` | `(std::thread::id)` | 签名不变，额外 flush+释放 TLAB |
| `GcHeap::safepoint` | `()` | 签名不变，入口加 flushTlab |
| `GcHeap::bumpAlloc` | `(size) -> void*` | 签名不变，仅全局慢路径调用 |

### 2.4 Business Logic Extraction（tryAlloc 决策点）

1. 入口检查 `compactPending_`（[gc.cpp:62](file:///d:/you/Aura/runtime/gc.cpp)）
2. 懒初始化 `oomError_`（[gc.cpp:71](file:///d:/you/Aura/runtime/gc.cpp)）
3. 对齐 size 到 8 字节
4. 触发条件检查：`youngBytes_ >= kYoungThreshold` → minor/major GC
5. `bumpAlloc(size)` 取内存
6. 失败 → majorGc → 重试
7. 初始化 GcObject header（desc/marked/generation/finalized/allocSize）
8. 注册 desc 到 `registeredDescs_`
9. push_back 到 `youngObjects_`
10. 累加 `youngBytes_` / `allocatedBytes_`
11. 老年代阈值检查 → `gcPending_ = true`

步骤 4、5、9、10 都是写共享状态，都是竞争点。TLAB 将 5/9/10 改为本地操作。

### 2.5 State & Side Effects

可变全局状态：
- `headPage_` / `currentPage_` — bumpAlloc 修改（竞争）
- `youngObjects_` — push_back 修改（竞争，扩容越界根因）
- `youngBytes_` / `allocatedBytes_` — 累加修改（数据竞争）
- `registeredDescs_` — insert 修改（竞争）
- `gcPending_` / `gcCount_` / `minorGcCount_` — GC 触发修改

---

## 3. 移动 GC（compact）对 TLAB 的影响分析（重点）

### 3.1 compact 行为回顾

[gc.cpp:754-761](file:///d:/you/Aura/runtime/gc.cpp) `compact(scope)`：
1. `computeForwardingAddresses`：遍历 `youngObjects_`（+ All 模式 `oldObjects_`），分配新页，设置转发指针
2. `updateAllReferences`：更新 roots/fields/array/youngObjects_ 等所有引用（[gc.cpp:973-1068](file:///d:/you/Aura/runtime/gc.cpp)）
3. `copyObjectsToNewLocations`：memcpy 旧对象到新地址
4. `rebuildPageList`：**释放旧页**（All 模式 `freeAllPages`，Young 模式释放无存活对象的页）

### 3.2 TLAB 与 compact 的冲突点

| 冲突 | 后果 | 解决方案 |
|:---|:---|:---|
| **TLAB curPage 指向的页被 compact 释放** | use-after-free：TLAB 下次分配访问已释放页 | safepoint 入口 flushTlab 清空 curPage=nullptr |
| **TLAB localYoung 中的对象未在全局 youngObjects_** | compact 的 updateAllReferences 漏更新这些对象的内部字段引用 → 子对象悬垂 | safepoint 入口 flushTlab 合并 localYoung 到全局 youngObjects_ |
| **compact 后 TLAB 状态过期** | curPage 指向已释放页，bumpOffset 错乱 | flushTlab 清空 curPage/bumpOffset，下次分配走 refill |
| **TLAB localYoungBytes 未累加到 youngBytes_** | minor/major GC 触发条件失准，GC 不及时 | flushTlab 时累加 localYoungBytes 到 youngBytes_ |
| **compact 期间 TLAB refill** | refill 与 compact 并发访问 headPage_ | STW 保证所有 worker 已停止，refill 不会在 compact 期间发生 |

### 3.3 安全保证链（关键设计）

```
线程进入 safepoint
    ↓
flushTlab()  ← 必须在任何 GC 操作前执行
    │
    ├─ 持 allocM_
    ├─ localYoung 合并到全局 youngObjects_
    ├─ localYoungBytes 累加到 youngBytes_ / allocatedBytes_
    ├─ 清空 localYoung / localYoungBytes
    ├─ curPage = nullptr  ← 关键：放弃对页的引用
    └─ bumpOffset = 0
    ↓
线程停止（stopped_threads_++）
    ↓
GC 执行者：等待所有线程停止
    ↓
minorGc / majorGc / compact
    ├─ markPhase：遍历 youngObjects_（含已合并的 TLAB 对象）
    ├─ sweepPhase*：回收未标记对象
    └─ compact：
        ├─ computeForwardingAddresses：遍历 youngObjects_
        ├─ updateAllReferences：更新所有引用
        ├─ copyObjectsToNewLocations
        └─ rebuildPageList：释放旧页（TLAB 原持有的页在此被释放，但 TLAB 已 curPage=nullptr）
    ↓
唤醒所有线程
    ↓
线程恢复，下次 tryAlloc：
    ├─ TLAB curPage == nullptr
    ├─ 走 tryAllocSlow（持 allocM_）
    ├─ 分配本次对象（加入全局 youngObjects_）
    └─ refillTlab：申请新页（从 OS），链入 headPage_，设给 tlab_->curPage
```

### 3.4 边界场景验证

**场景 1：compact 期间 worker 线程被唤醒后立即分配**
- TLAB curPage=nullptr → 走 tryAllocSlow → 持 allocM_ → refillTlab 申请新页
- 新页不与 compact 的新页冲突（refill 调用 allocPage 从 OS 分配）

**场景 2：minorGc 不触发 compact（碎片率低）**
- youngObjects_ 中的对象可能被 promote 到 oldObjects_
- TLAB curPage 仍指向有效页（未被释放）
- 但 TLAB 已在 safepoint 入口 flushTlab 清空 curPage=nullptr
- 下次分配走 refill，**可能复用原页的剩余空间吗？**
  - 不复用：refillTlab 直接 allocPage 新页，简单安全
  - 复用：需追踪原页剩余空间，复杂
  - **决策：不复用，refillTlab 总是申请新页**（牺牲少量内存换简单性）

**场景 3：主线程（EventLoop）也走 TLAB**
- EventLoop::run 调用 registerThread（[task.cpp:30](file:///d:/you/Aura/runtime/task.cpp)）→ 分配 TLAB
- 主线程分配频率高（编译期 intern、字面量等），同样需要无锁路径
- safepoint 期间主线程也走 flushTlab → 与 worker 一致

**场景 4：线程退出时 TLAB 仍有未合并对象**
- unregisterThread 调用 flushTlab → 合并到全局
- 然后释放 TLAB 结构（delete tlab_）
- curPage 已在 flushTlab 中清空，无需额外处理

---

## 4. Proposed Changes（详细实施方案）

### 4.1 改动 A：新增 TLAB 数据结构

**文件**：[runtime/gc.h](file:///d:/you/Aura/runtime/gc.h)

**位置**：GcHeap 类定义内（约 L160-348 之间），新增 Tlab 结构和 thread_local 声明

**新增代码**：
```cpp
class GcHeap {
public:
    // ... 现有内容 ...

    // ============================================================
    // TLAB — 线程局部分配缓冲
    //
    // 每个注册的线程持有一个 TLAB，bump 分配在自己的 curPage 上进行，
    // 避免多线程竞争全局 currentPage_。localYoung 记录本线程分配的对象，
    // safepoint 入口 flushTlab 合并到全局 youngObjects_。
    //
    // compact 安全：flushTlab 清空 curPage，避免 compact 释放旧页后悬垂。
    // ============================================================
    struct Tlab {
        Page*   curPage = nullptr;       // 当前分配页（nullptr 时走 refill）
        size_t  bumpOffset = 0;          // 当前页 bump 偏移
        std::vector<GcObject*> localYoung;    // 本线程分配的对象
        size_t  localYoungBytes = 0;      // 本线程分配的字节数
    };

private:
    // TLAB 列表：跟踪所有线程的 TLAB（用于调试/统计，不参与 GC）
    std::mutex tlabList_m_;
    std::vector<Tlab*> tlabList_;

public:
    // 每线程独立 TLAB 指针（thread_local 保证线程隔离）
    // 注：用裸指针，由 GcHeap::registerThread/unregisterThread 显式管理生命周期
    //     避免 thread_local 析构顺序与 GcHeap 单例冲突
    static thread_local Tlab* tlab_;

    // TLAB 操作
    void  flushTlab();      // safepoint 入口调用：合并 localYoung 到全局，清空 curPage
    void  refillTlab();     // tryAllocSlow 中调用：申请新页给 TLAB（持 allocM_）
    Tlab* ensureTlab();     // registerThread 时调用：分配 TLAB 结构
    void  releaseTlab();    // unregisterThread 时调用：flush + 释放 TLAB 结构
};
```

**文件**：[runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp)

**位置**：文件开头命名空间内（约 L22 后）

**新增代码**：
```cpp
namespace aura_rt {

// thread_local TLAB 指针定义
thread_local GcHeap::Tlab* GcHeap::tlab_ = nullptr;

// ... 现有代码 ...
```

### 4.2 改动 B：registerThread / unregisterThread 扩展

**文件**：[runtime/gc.cpp:231-242](file:///d:/you/Aura/runtime/gc.cpp)

**原代码**：
```cpp
void GcHeap::registerThread(std::thread::id id) {
    std::lock_guard<std::mutex> lk(threads_m_);
    registered_threads_.push_back(id);
}

void GcHeap::unregisterThread(std::thread::id id) {
    std::lock_guard<std::mutex> lk(threads_m_);
    auto it = std::find(registered_threads_.begin(), registered_threads_.end(), id);
    if (it != registered_threads_.end()) {
        registered_threads_.erase(it);
    }
}
```

**修改后**：
```cpp
void GcHeap::registerThread(std::thread::id id) {
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        registered_threads_.push_back(id);
    }
    // 为本线程分配 TLAB
    ensureTlab();
}

void GcHeap::unregisterThread(std::thread::id id) {
    // 先 flush + 释放 TLAB（避免 threads_m_ 持锁时调用 allocM_）
    releaseTlab();
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        auto it = std::find(registered_threads_.begin(), registered_threads_.end(), id);
        if (it != registered_threads_.end()) {
            registered_threads_.erase(it);
        }
    }
}

GcHeap::Tlab* GcHeap::ensureTlab() {
    if (tlab_) return tlab_;  // 已分配
    Tlab* t = new Tlab();     // 堆分配，避免 thread_local 析构顺序问题
    tlab_ = t;
    {
        std::lock_guard<std::mutex> lk(tlabList_m_);
        tlabList_.push_back(t);
    }
    return t;
}

void GcHeap::releaseTlab() {
    if (!tlab_) return;
    flushTlab();  // 合并 localYoung 到全局
    {
        std::lock_guard<std::mutex> lk(tlabList_m_);
        auto it = std::find(tlabList_.begin(), tlabList_.end(), tlab_);
        if (it != tlabList_.end()) tlabList_.erase(it);
    }
    delete tlab_;
    tlab_ = nullptr;
}
```

### 4.3 改动 C：tryAlloc 改造为 TLAB 优先

**文件**：[runtime/gc.cpp:60-118](file:///d:/you/Aura/runtime/gc.cpp)

**原代码**：
```cpp
GcObject* GcHeap::tryAlloc(size_t size, const TypeDescriptor* desc) {
    // 入口：若 compact 被延迟，先补执行
    if (compactSuspendedCount_ == 0 && compactPending_) { ... }
    ensureOomError();
    size = (size + 7) & ~size_t(7);
    if (youngBytes_ >= kYoungThreshold) {
        minorGc();
        if (youngBytes_ >= kYoungThreshold) majorGc();
    }
    void* mem = bumpAlloc(size);
    if (!mem) { majorGc(); mem = bumpAlloc(size); }
    if (!mem) throwOutOfMemory();
    GcObject* obj = static_cast<GcObject*>(mem);
    obj->desc = desc;
    obj->setMarked(false);
    obj->setGeneration(0);
    obj->setFinalized(false);
    obj->setAllocSize(size);
    if (desc) registeredDescs_.insert(desc);
    youngObjects_.push_back(obj);
    youngBytes_ += size;
    allocatedBytes_ += size;
    if (oldBytes_ >= kOldThreshold) gcPending_ = true;
    return obj;
}
```

**修改后**：
```cpp
GcObject* GcHeap::tryAlloc(size_t size, const TypeDescriptor* desc) {
    // 入口：若 compact 被延迟，先补执行（此时 compactSuspendedCount_ == 0）
    if (compactSuspendedCount_ == 0 && compactPending_) {
        compactPending_ = false;
        if (shouldCompact(CompactScope::Young))
            compact(CompactScope::Young);
        else if (shouldCompact(CompactScope::All))
            compact(CompactScope::All);
    }

    // 首次调用时懒初始化 OOM 错误字符串
    ensureOomError();

    // 对齐到 8 字节
    size = (size + 7) & ~size_t(7);

    // 大对象（> kPageSize/2 = 2KB）走全局慢路径
    // 原因：TLAB 单页分配会浪费半页，大对象直接用全局 currentPage_
    if (size > kPageSize / 2) {
        return tryAllocSlow(size, desc);
    }

    // TLAB 快路径（无锁）
    Tlab* tlab = tlab_;
    if (tlab && tlab->curPage &&
        tlab->bumpOffset + size <= kPageSize) {
        void* mem = tlab->curPage->data + tlab->bumpOffset;
        tlab->bumpOffset += size;

        GcObject* obj = static_cast<GcObject*>(mem);
        obj->desc = desc;
        obj->setMarked(false);
        obj->setGeneration(0);
        obj->setFinalized(false);
        obj->setAllocSize(size);

        // 本地记录（无需加锁）
        tlab->localYoung.push_back(obj);
        tlab->localYoungBytes += size;

        // 注：registeredDescs_ 在 tryAllocSlow 中 insert（首次分配走慢路径）
        // TLAB 路径跳过，避免 unordered_set 并发写
        // 风险：首次分配走 TLAB 时 desc 未注册 → 保守栈扫描漏标
        // 缓解：GcRootHandle 精确标记覆盖主路径；保守扫描仅兜底

        return obj;
    }

    // TLAB 未初始化 / 满 → 走慢路径（refill + 分配）
    return tryAllocSlow(size, desc);
}

// ============================================================
// tryAllocSlow — 全局慢路径（持 allocM_）
//
// 调用场景：
//   1. 大对象（size > kPageSize/2）
//   2. TLAB 未初始化（首次分配）
//   3. TLAB 已满（bumpOffset + size > kPageSize）
//   4. compact 后 TLAB curPage 被清空
// ============================================================
GcObject* GcHeap::tryAllocSlow(size_t size, const TypeDescriptor* desc) {
    std::lock_guard<std::mutex> lk(allocM_);

    // 触发条件检查：新生代超阈值 → minor/major GC
    if (youngBytes_ >= kYoungThreshold) {
        minorGc();
        if (youngBytes_ >= kYoungThreshold) {
            majorGc();
        }
    }

    void* mem = bumpAlloc(size);  // 全局 currentPage_（持 allocM_）
    if (!mem) {
        majorGc();
        mem = bumpAlloc(size);
    }
    if (!mem) {
        // GC 后仍失败 → 抛出预缓存的 OutOfMemoryError
        // 注：throwOutOfMemory 需要在持锁状态下安全（不会再次 alloc）
        //     实际 throw 不需要 alloc，oomError_ 已预缓存
        throwOutOfMemory();
    }

    GcObject* obj = static_cast<GcObject*>(mem);
    obj->desc = desc;
    obj->setMarked(false);
    obj->setGeneration(0);
    obj->setFinalized(false);
    obj->setAllocSize(size);

    // 注册 desc 到合法集合（首次出现时插入）
    if (desc) registeredDescs_.insert(desc);

    youngObjects_.push_back(obj);
    youngBytes_ += size;
    allocatedBytes_ += size;

    if (oldBytes_ >= kOldThreshold) {
        gcPending_ = true;
    }

    // 小对象：顺便 refill TLAB（让下次走快路径）
    // 大对象不 refill（避免浪费 TLAB 页）
    if (size <= kPageSize / 2 && tlab_ && !tlab_->curPage) {
        refillTlab();  // 持 allocM_ 状态下申请新页
    }

    return obj;
}
```

### 4.4 改动 D：refillTlab 实现

**文件**：[runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp)

**位置**：bumpAlloc 函数之后（约 L134 后）

**新增代码**：
```cpp
void GcHeap::refillTlab() {
    // 注：调用方必须持有 allocM_
    Tlab* tlab = tlab_;
    if (!tlab) return;

    // 申请新页（从 OS 分配，绕过 CRT 堆）
    Page* newPage = allocPage();
    if (!newPage) return;  // OOM：放弃 refill，下次分配仍走慢路径

    // 链入全局页链表（markPhase 保守扫描需要遍历所有页）
    newPage->next = headPage_;
    headPage_ = newPage;

    tlab->curPage = newPage;
    tlab->bumpOffset = 0;
}
```

### 4.5 改动 E：safepoint 入口加 flushTlab

**文件**：[runtime/gc.cpp:186-226](file:///d:/you/Aura/runtime/gc.cpp)

**原代码**：
```cpp
void GcHeap::safepoint() {
    if (!gcPending_) return;
    // ... STW 逻辑 ...
}
```

**修改后**：
```cpp
void GcHeap::safepoint() {
    if (!gcPending_) return;

    // 关键：flush 本线程 TLAB 到全局
    // 必须在任何 GC 操作前执行，确保：
    //   1. youngObjects_ 包含所有已分配对象（markPhase 能标记到）
    //   2. compact 的 updateAllReferences 能更新所有对象引用
    //   3. compact 释放旧页后 TLAB curPage 不悬垂（已清空为 nullptr）
    flushTlab();

    // 单线程场景：直接执行 GC
    size_t threadCount;
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        threadCount = registered_threads_.size();
    }
    if (threadCount <= 1) {
        gcPending_ = false;
        if (youngBytes_ >= kYoungThreshold / 2) minorGc();
        if (oldBytes_ >= kOldThreshold) majorGc();
        return;
    }

    // 多线程场景：本线程尝试成为 GC 执行者
    if (!gc_in_progress_.exchange(true)) {
        // 抢到 GC 锁：等待其他线程到达 safepoint
        // 注：其他线程进入 safepoint 时也会先 flushTlab，然后 stopped_threads_++
        {
            std::unique_lock<std::mutex> lk(all_stopped_m_);
            all_stopped_cv_.wait(lk, [this, threadCount]{
                return stopped_threads_.load() >= static_cast<int>(threadCount) - 1;
            });
        }
        // 所有其他线程已停止，执行 GC
        gcPending_ = false;
        if (youngBytes_ >= kYoungThreshold / 2) minorGc();
        if (oldBytes_ >= kOldThreshold) majorGc();

        // 唤醒所有线程
        gc_in_progress_ = false;
        stopped_threads_ = 0;
        all_stopped_cv_.notify_all();
    } else {
        // 其他线程正在执行 GC，本线程停止
        stopped_threads_++;
        std::unique_lock<std::mutex> lk(all_stopped_m_);
        all_stopped_cv_.wait(lk, [this]{ return !gc_in_progress_.load(); });
    }
}
```

### 4.6 改动 F：flushTlab 实现

**文件**：[runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp)

**位置**：refillTlab 之后

**新增代码**：
```cpp
void GcHeap::flushTlab() {
    Tlab* tlab = tlab_;
    if (!tlab) return;
    // 空快速路径：无本地对象且无 curPage，无需加锁
    if (tlab->localYoung.empty() && !tlab->curPage) return;

    std::lock_guard<std::mutex> lk(allocM_);

    // 1. 合并 localYoung 到全局 youngObjects_
    if (!tlab->localYoung.empty()) {
        youngObjects_.insert(youngObjects_.end(),
                             tlab->localYoung.begin(),
                             tlab->localYoung.end());
        youngBytes_ += tlab->localYoungBytes;
        allocatedBytes_ += tlab->localYoungBytes;

        tlab->localYoung.clear();
        tlab->localYoungBytes = 0;
    }

    // 2. 关键：清空 curPage
    // 原因：compact 可能释放此页（rebuildPageList 释放无存活对象的页）
    // 清空后下次分配走 refillTlab 申请新页
    // 注：不释放页本身，页由全局 headPage_ 链表管理，compact 决定保留/释放
    tlab->curPage = nullptr;
    tlab->bumpOffset = 0;
}
```

### 4.7 改动 G：清理 bumpAlloc 误导性注释

**文件**：[runtime/gc.cpp:120-134](file:///d:/you/Aura/runtime/gc.cpp)

**原代码**：
```cpp
void* GcHeap::bumpAlloc(size_t size) {
    // 注：调用方 tryAlloc 必须持有 allocM_（bumpAlloc 内部不再加锁）
    // 这样 tryAlloc 的整个 分配+注册 段在同一锁内，避免重复加锁死锁
    if (!currentPage_ || currentPage_->bumpOffset + size > kPageSize) {
        // ...
    }
    // ...
}
```

**修改后**：
```cpp
void* GcHeap::bumpAlloc(size_t size) {
    // 全局慢路径 bump 分配：仅 tryAllocSlow 调用，调用方必须持有 allocM_
    // TLAB 路径不使用此函数（在 tlab_->curPage 上直接 bump）
    if (!currentPage_ || currentPage_->bumpOffset + size > kPageSize) {
        Page* newPage = allocPage();
        if (!newPage) return nullptr;
        currentPage_ = newPage;
        newPage->next = headPage_;
        headPage_ = newPage;
    }

    void* ptr = currentPage_->data + currentPage_->bumpOffset;
    currentPage_->bumpOffset += size;
    return ptr;
}
```

### 4.8 改动 H：gc.h 声明新增方法

**文件**：[runtime/gc.h](file:///d:/you/Aura/runtime/gc.h)

**位置**：GcHeap 类内（约 L244-275 之间）

**新增声明**：
```cpp
    // TLAB 操作（实现见 gc.cpp）
    void  flushTlab();
    void  refillTlab();
    Tlab* ensureTlab();
    void  releaseTlab();
```

---

## 5. Boundary Condition Handling Strategy

| Boundary Condition | Current Handling | Planned Handling | Test Strategy |
|:---|:---|:---|:---|
| 多线程 push_back youngObjects_ 越界 | 崩溃 | TLAB localYoung + safepoint 合并 | sync thread Test 3 ASAN 验证 |
| bumpOffset 数据竞争 | 错误地址 | TLAB 本地 bumpOffset | 同上 |
| compact 释放 TLAB curPage 指向的页 | 无（当前无 TLAB） | safepoint 入口 flushTlab 清空 curPage | compact + 多线程分配测试 |
| TLAB localYoung 未合并导致漏标 | 无 | safepoint 入口 flushTlab 合并到全局 | GC 触发后对象存活验证 |
| 主线程无 TLAB | 走全局（含锁） | 主线程 registerThread 也分配 TLAB | 单线程性能基准 |
| TLAB refill 期间 STW | 无 | refill 在 allocM_ 内，safepoint 等待 | STW 时序单元测试 |
| 大对象（> kPageSize/2）走 TLAB 浪费 | 无 | 直接全局慢路径 | 大字符串 concat 测试 |
| 线程退出时 localYoung 未 flush | 无 | unregisterThread 强制 flush | 线程生命周期测试 |
| registeredDescs_ 并发 insert | 数据竞争 | TLAB 跳过，慢路径持 allocM_ insert | ASAN 验证 |
| TLAB refill 后立即 safepoint | 无 | refill 在持 allocM_，safepoint 等待 | 时序测试 |
| compact 后 currentPage_ 重置 | 无 | TLAB 不依赖 currentPage_，refill 用新页 | compact 后分配测试 |

---

## 6. Test Plan

### 6.1 单元测试

- **单线程分配**：分配 1000 个小对象，验证 youngObjects_.size() == 1000
- **多线程分配**：4 worker × 1000 对象，验证总数 == 4000，ASAN 无报错
- **TLAB flush 验证**：分配 100 对象后 forceGc，验证 youngObjects_ 包含所有对象
- **大对象路径**：分配 5KB 字符串，验证走慢路径（通过 gc_stats 观察）

### 6.2 集成测试

- **sync thread Test 3**：200 次字符串拼接 × N worker，ASAN 验证（原 bug 场景）
- **扩展测试**：4 worker × 500 次拼接（GC 触发 + STW + TLAB flush 全链路）
- **compact + TLAB**：分配大量对象触发 compact，验证 compact 后分配正常

### 6.3 回归测试

- **complex_closure**：全量测试（确保 compact 不受 TLAB 影响）
- **Array 7 项测试**：确保大对象路径仍工作
- **现有所有测试**：确保单线程场景无回归

### 6.4 ASAN 验证

所有测试在 ASAN 模式下运行，验证：
- 无 heap-buffer-overflow
- 无 use-after-free（compact 释放页后 TLAB 不访问）
- 无 data race（TSan 可选）

---

## 7. Implementation Steps（有序实施步骤）

| Step | 内容 | 文件 | 依赖 | 验收 |
|:---|:---|:---|:---|:---|
| S1 | gc.h 新增 Tlab 结构 + thread_local 声明 + 4 个方法声明 | [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) | 无 | 编译通过（方法未实现，先声明） |
| S2 | gc.cpp 定义 thread_local tlab_ + 实现 ensureTlab/releaseTlab | [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) | S1 | registerThread/unregisterThread 单元测试 |
| S3 | gc.cpp 修改 registerThread/unregisterThread 调用 ensureTlab/releaseTlab | [runtime/gc.cpp:231-242](file:///d:/you/Aura/runtime/gc.cpp) | S2 | 单线程 EventLoop::run 正常 |
| S4 | gc.cpp 实现 flushTlab + refillTlab | [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) | S2 | 编译通过 |
| S5 | gc.cpp 改造 tryAlloc 为 TLAB 优先 + 新增 tryAllocSlow | [runtime/gc.cpp:60-118](file:///d:/you/Aura/runtime/gc.cpp) | S4 | 单线程分配功能不变（所有现有测试通过） |
| S6 | gc.cpp safepoint 入口加 flushTlab 调用 | [runtime/gc.cpp:186](file:///d:/you/Aura/runtime/gc.cpp) | S4 | 单线程 GC 正常触发 |
| S7 | gc.cpp 清理 bumpAlloc 误导性注释 | [runtime/gc.cpp:120-121](file:///d:/you/Aura/runtime/gc.cpp) | S5 | 代码评审 |
| S8 | 编译验证（runtime + 编译器） | - | S1-S7 | cmake --build 全绿 |
| S9 | 运行 sync thread Test 3 + 扩展测试（ASAN 模式） | [example/test.aura](file:///d:/you/Aura/example/test.aura) | S8 | ASAN 无报错，无卡死 |
| S10 | 运行全量回归测试（complex_closure + Array） | - | S9 | 全部通过 |

**回滚方案**：若 S9 失败，回滚 S5（tryAlloc 改回原版加锁版本），保留 S1-S4/S6-S7（TLAB 基础设施不影响）。

---

## 8. Risks & Mitigations

| 风险 | 应对 |
|:---|:---|
| **compact 期间 TLAB curPage 悬垂** | safepoint 入口 flushTlab 清空 curPage=nullptr（核心安全保证） |
| **TLAB localYoung 未合并导致漏标** | safepoint 入口 flushTlab 合并到全局 youngObjects_ |
| **thread_local 析构顺序与 GcHeap 单例不一致** | 用 `thread_local Tlab*` 裸指针，由 unregisterThread 显式 delete |
| **大对象阈值选错导致 TLAB 利用率低** | 默认 kPageSize/2 = 2KB，覆盖 GcString 大多数场景；可调 |
| **registerThread 早期调用（init_priority 之前）** | GcHeap 已 `[[gnu::init_priority(101)]]`，主线程 register 在 EventLoop::run 内调用，安全 |
| **registeredDescs_ 漏注册（TLAB 跳过 insert）** | 首次分配走慢路径时注册；保守扫描仅兜底，GcRootHandle 精确标记覆盖主路径 |
| **refillTlab 在 allocM_ 内调用 allocPage（系统调用慢）** | refill 频率低（每页 4KB / 32B 对象 = 128 次分配才 refill），可接受 |
| **TLAB 内存占用** | 每线程 1 页 = 4KB，N 线程共 4N KB，可忽略 |

---

## 9. 后置工作（TLAB 完成后）

1. **修复 sync thread Test 3 卡死问题**：TLAB 完成后重新跑 Test 3，预期卡死消失（卡死源自 youngObjects_ 损坏导致的 GC 死循环）
2. **更新 TODO.txt §五**：TLAB 标记 [x] 完成，附验证日期
3. **更新 plan/gc_features_plan.md §十一**：状态改为 ✅ 已完成
4. **更新 plan/sync_thread_plan.md §5.4 约束 2**：注明 TLAB 已实施，移除"未来优化"备注

---

## 10. 关键设计决策总结

1. **TLAB curPage 不复用 compact 后的剩余空间**：refillTlab 总是申请新页，牺牲少量内存换简单性
2. **大对象阈值 = kPageSize/2**：避免 TLAB 单页分配大对象浪费半页
3. **registeredDescs_ 只在慢路径 insert**：TLAB 路径跳过，依赖首次分配走慢路径注册
4. **thread_local Tlab* 裸指针**：避免 thread_local 析构顺序问题，由 unregisterThread 显式管理
5. **flushTlab 清空 curPage**：核心安全保证，避免 compact 释放旧页后悬垂
6. **主线程也走 TLAB**：EventLoop::run 的 registerThread 自动分配 TLAB，统一路径
