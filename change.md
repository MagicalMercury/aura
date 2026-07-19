# GC Phase 2 P1 项实现 Plan

> 对应 [plan/gc_features_plan.md](file:///d:/you/Aura/plan/gc_features_plan.md) §五/§六/§七/§八/§十八
> 前置：P0 已全部完成（[change.md 上一版本](file:///d:/you/Aura/change.md) 已验收）
> 日期：2026-07-18
> 状态：草案（待批准）

---

## 一、深度阅读后的现状修正

### 1.1 §五 全局变量 GC 根注册 — **应用场景澄清**

**重要发现**：[CodeGen.cpp:175-198](file:///d:/you/Aura/src/CodeGen/CodeGen.cpp#L175) 的 `genDecl` **仅处理 TypeDecl / InterfaceDecl / FunDecl / MethodDecl**，**Aura 语言当前不支持顶层 `let` 全局变量**。

所以 §五 真正的应用场景不是"Aura 源码中的全局变量"，而是：

| 场景 | 描述 | 当前状态 |
|:---|:---|:---|
| **A. 运行时缓存** | [gcstring_optimization.md](file:///d:/you/Aura/plan/gcstring_optimization.md) 第一阶段规划的 `GcString::empty()` / `from(bool)` / `from(int)` 静态缓存 | 未实现 |
| **B. interned 字符串** | 属性名 / 关键字 / 错误消息字符串驻留 | 未实现 |
| **C. OOM 错误缓存** | [gc.cpp:125-135](file:///d:/you/Aura/runtime/gc.cpp#L125) 已有 `oomError_`，但通过 `if (oomError_.kind) markObject(...)` 特例处理 | 已实现，但用特例而非通用机制 |
| **D. Aura 全局 let** | Aura 语言层面支持 `let x = ...` 在模块顶层 | **不支持，本 plan 不实现** |

**结论**：§五 实际只需要做 A/B 场景的 `GcGlobalRoot<T>` 模板，让运行时缓存的字符串等能正确注册为 GC 根。

### 1.2 §六 弱引用 — 设计可行，无修正

[gc.h](file:///d:/you/Aura/runtime/gc.h) 无 `GcWeakHandle`，[gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) 无 `weakHandles_`。需要新增。

### 1.3 §七 Finalizer — 设计可行，需补充

[types.h](file:///d:/you/Aura/runtime/types.h) 中 `TypeDescriptor` 已有 `ptrFieldOffsets` / `inlineArrayFields` 等，但**无 `finalizer` 函数指针**。`GcObject` 也**无 `finalized` 字段**。

需要新增字段。但要注意：`GcObject` 头部布局的修改会影响所有 GC 对象（[gc.h:73](file:///d:/you/Aura/runtime/gc.h#L73)）。

### 1.4 §八 多线程 STW — 关键依赖项

当前 [gc.cpp:154-164](file:///d:/you/Aura/runtime/gc.cpp#L154) `safepoint()` 是单线程实现：

```cpp
void GcHeap::safepoint() {
    if (gc_pending_) {
        gc_pending_ = false;
        if (youngBytes_ >= kYoungThreshold / 2) minorGc();
        if (oldBytes_ >= kOldThreshold) majorGc();
    }
}
```

无 `std::mutex` / `std::thread` / `std::condition_variable` 引入（[Grep `std::mutex|std::thread` runtime/](file:///d:/you/Aura/runtime) 无匹配）。

[sync_thread_plan.md](file:///d:/you/Aura/plan/sync_thread_plan.md) 和 [io_coroutine_plan.md](file:///d:/you/Aura/plan/io_coroutine_plan.md) 都依赖此项。

### 1.5 §十八 forceGc 暴露 + 多线程安全 — 简化

[BuiltinRegistry.h:255-262](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h#L255) `functions_` 表只有 `range` 和 `channel`，无 GC 函数注册。

[gc.h:215-217](file:///d:/you/Aura/runtime/gc.h#L215) 已有 `inline void force_gc()` C++ API，[gc.cpp:196-199](file:///d:/you/Aura/runtime/gc.cpp#L196) 的 `forceGc()` 单线程直接调 `majorGc()`，**多线程下不安全**。

但 §十八的"多线程安全改造"实际就是依赖 §八 STW 机制 — 可合并入 §八 一起做，避免重复设计。

---

## 二、修改目标与原因

### 2.1 目标

实现 GC Phase 2 五项 P1 功能：

1. **§八 多线程 STW**（最基础，其他几项都依赖）
2. **§十八 forceGc 暴露**（依赖 §八）
3. **§五 GcGlobalRoot<T>**（独立，运行时缓存场景）
4. **§六 GcWeakHandle<T>**（独立，未来 interning / Map 场景）
5. **§七 Finalizer**（独立，资源句柄场景）

### 2.2 原因

- P0 完成后 GC 已能工作，但单线程限制阻塞了 [sync_thread_plan](file:///d:/you/Aura/plan/sync_thread_plan.md) 和 [io_coroutine_plan](file:///d:/you/Aura/plan/io_coroutine_plan.md)
- 当前 `forceGc()` 多线程下不安全
- 运行时缓存字符串（[gcstring_optimization.md](file:///d:/you/Aura/plan/gcstring_optimization.md)）需要全局根支持
- 资源句柄（文件 / 锁 / 套接字）的清理需要 Finalizer

---

## 三、受影响的文件和模块

| 文件 | 改动内容 | 行数估计 |
|:---|:---|:---:|
| [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) | 新增 `GcGlobalRoot<T>` / `GcWeakHandle<T>` 模板，`GcHeap` 加 `globalRoots_` / `weakHandles_` / `threads_m_` 等成员 | +120 |
| [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) | safepoint 改造为 STW、register/unregister API 实现、markPhase 扫描 globalRoots、sweepPhase 清空弱引用 + 调 finalizer、forceGc 走 STW | +180 |
| [runtime/types.h](file:///d:/you/Aura/runtime/types.h) | `GcObject` 加 `finalized` 字段，`TypeDescriptor` 加 `finalizer` 函数指针 | +8 |
| [src/Sema/BuiltinRegistry.h](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h) | 注册 `gc_force` / `gc_stats` 全局函数 | +15 |
| [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) | `genCallExpr` 识别 `gc_force()` / `gc_stats()` 生成对应 C++ 调用 | +20 |
| **总计** | | **~343 行** |

---

## 四、修改步骤（按依赖顺序，每步独立 commit）

### Phase 1：§八 多线程 STW（基础，最先做）

#### Step 1.1：GcHeap 加线程管理成员

**改动**：[gc.h](file:///d:/you/Aura/runtime/gc.h) 在 `GcHeap` private 区追加：

```cpp
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

class GcHeap {
public:
    // 线程注册（用于 GC stop-the-world）
    void registerThread(std::thread::id id);
    void unregisterThread(std::thread::id id);

private:
    // --- 多线程 STW ---
    std::mutex             threads_m_;
    std::vector<std::thread::id> registered_threads_;
    std::atomic<bool>      gc_in_progress_{false};
    std::atomic<int>       stopped_threads_{0};
    std::condition_variable all_stopped_cv_;
    std::mutex             all_stopped_m_;
};
```

**关键**：`registered_threads_` 在 `registerThread`/`unregisterThread` 中用 `threads_m_` 保护，但在 `safepoint` 中只读不需要锁（容忍短暂数据竞争，最坏情况是某线程未注册但被忽略）。

#### Step 1.2：registerThread / unregisterThread 实现

**改动**：[gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) 新增：

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

#### Step 1.3：safepoint 改造为 STW

**改动**：[gc.cpp:154-164](file:///d:/you/Aura/runtime/gc.cpp#L154) 替换为：

```cpp
void GcHeap::safepoint() {
    if (!gc_pending_) return;

    // 单线程场景：直接执行 GC
    size_t threadCount;
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        threadCount = registered_threads_.size();
    }
    if (threadCount <= 1) {
        gc_pending_ = false;
        if (youngBytes_ >= kYoungThreshold / 2) minorGc();
        if (oldBytes_ >= kOldThreshold) majorGc();
        return;
    }

    // 多线程场景：本线程成为 GC 执行者
    if (!gc_in_progress_.exchange(true)) {
        // 抢到 GC 锁：等待其他线程到达 safepoint
        {
            std::unique_lock<std::mutex> lk(all_stopped_m_);
            all_stopped_cv_.wait(lk, [this, &threadCount]{
                return stopped_threads_.load() >= static_cast<int>(threadCount) - 1;
            });
        }
        // 所有其他线程已停止，执行 GC
        gc_pending_ = false;
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
        // 注意：lk 析构时才释放锁，但 wait 已释放
    }
}
```

**注意点**：
- 第一个进入的线程成为 GC 执行者，其他线程阻塞
- `gc_in_progress_.exchange(true)` 用原子操作避免竞争
- `stopped_threads_` 计数器用于判断是否所有非 GC 线程都已到达 safepoint
- 完成后 `notify_all` 唤醒所有阻塞线程

#### Step 1.4：run_event_loop 注册主线程

**改动**：[task.cpp:13-30](file:///d:/you/Aura/runtime/task.cpp#L13) 在 `run_event_loop` 入口注册主线程：

```cpp
void run_event_loop(task<void>& mainTask) {
    auto& gc = GcHeap::instance();
    gc.registerThread(std::this_thread::get_id());

    auto handle = mainTask.handle();
    if (!handle) {
        gc.unregisterThread(std::this_thread::get_id());
        return;
    }
    // ...（原有逻辑保持不变）
    gc_unregister_stack_roots(framePtr, ...);

    gc.unregisterThread(std::this_thread::get_id());
}
```

**验收**：编译通过，单线程场景行为不变（threadCount <= 1 走原逻辑）。

---

### Phase 2：§十八 forceGc 暴露 + 多线程安全（依赖 Phase 1）

#### Step 2.1：forceGc 改造为走 STW

**改动**：[gc.cpp:196-199](file:///d:/you/Aura/runtime/gc.cpp#L196) 替换为：

```cpp
void GcHeap::forceGc() {
    // 多线程场景：设置 gc_pending_，等待 safepoint 处理
    size_t threadCount;
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        threadCount = registered_threads_.size();
    }

    if (threadCount <= 1) {
        // 单线程场景：直接执行
        gcPending_ = false;
        majorGc();
        return;
    }

    // 多线程场景：设置 gc_pending_，由各线程 safepoint 触发
    gc_pending_ = true;
    // 本线程也走到 safepoint
    safepoint();
}
```

**关键**：`forceGc()` 不再直接调 `majorGc()`，而是设置 `gc_pending_ = true` 后调 `safepoint()`，由 STW 机制保证安全。

#### Step 2.2：新增 GcStats 结构 + getStats()

**改动**：[gc.h](file:///d:/you/Aura/runtime/gc.h) 新增：

```cpp
struct GcStats {
    size_t allocatedBytes;
    size_t youngBytes;
    size_t oldBytes;
    size_t gcCount;
    size_t minorGcCount;
    size_t liveObjectCount;     // youngObjects_.size() + oldObjects_.size()
    size_t pageCount;           // 遍历 headPage_ 计数
};

class GcHeap {
public:
    GcStats getStats() const;
};
```

**改动**：[gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) 新增：

```cpp
GcStats GcHeap::getStats() const {
    GcStats s;
    s.allocatedBytes  = allocatedBytes_;
    s.youngBytes      = youngBytes_;
    s.oldBytes        = oldBytes_;
    s.gcCount         = gcCount_;
    s.minorGcCount    = minorGcCount_;
    s.liveObjectCount = youngObjects_.size() + oldObjects_.size();
    s.pageCount = 0;
    for (Page* p = headPage_; p; p = p->next) s.pageCount++;
    return s;
}
```

**新增便捷 C++ API**（[gc.h](file:///d:/you/Aura/runtime/gc.h) 末尾）：

```cpp
inline void gc_force_major() { GcHeap::instance().forceGc(); }
inline GcStats gc_get_stats() { return GcHeap::instance().getStats(); }
```

#### Step 2.3：BuiltinRegistry 注册 gc_force / gc_stats

**改动**：[BuiltinRegistry.h:255-262](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h#L255) `functions_` 表追加：

```cpp
{
    "gc_force", {}, ReturnTypeInfo::None()
},
{
    "gc_stats", {}, ReturnTypeInfo::Named("string")
},
```

**注意**：`gc_stats` 返回 `string`，但实际 C++ 返回 `GcStats` 结构。需要在 CodeGen 中特殊处理 — 把 `GcStats` 格式化为字符串。

**简化方案**：让 `gc_stats()` 在 C++ 端直接返回 `GcString*`，内部格式化：

```cpp
// runtime/gc.cpp 新增
GcString* gc_stats_string() {
    auto s = GcHeap::instance().getStats();
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "GC: alloc=%zuKB young=%zuKB old=%zuKB gc=%zu minor=%zu live=%zu pages=%zu",
        s.allocatedBytes / 1024, s.youngBytes / 1024, s.oldBytes / 1024,
        s.gcCount, s.minorGcCount, s.liveObjectCount, s.pageCount);
    return make_string(buf);
}
```

#### Step 2.4：CodeGen 识别 gc_force / gc_stats

**改动**：[ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) 的 `genCallExpr` 中（在 `range` 处理之后）追加：

```cpp
// 识别 gc_force() / gc_stats() 等内置 GC 函数
if (auto* id = dynamic_cast<const Identifier*>(e.callee.get())) {
    if (id->name == "gc_force" && e.args.empty()) {
        return "aura_rt::gc_force_major()";
    }
    if (id->name == "gc_stats" && e.args.empty()) {
        return "aura_rt::gc_stats_string()";
    }
}
```

**Aura 使用方式**：

```aura
fun main(io: Io) {
    let s = "hello"
    gc_force()              // 触发 GC
    io.println(s)           // s 仍可用
    let info = gc_stats()
    io.println(info)
}
```

**验收**：
- 单线程下 `gc_force()` 直接执行 GC
- 多线程下 `gc_force()` 走 STW 流程
- `gc_stats()` 返回正确的格式化字符串

---

### Phase 3：§五 GcGlobalRoot<T>（独立，可与 Phase 2 并行）

#### Step 3.1：GcHeap 加 globalRoots_

**改动**：[gc.h](file:///d:/you/Aura/runtime/gc.h) `GcHeap` private 区追加：

```cpp
// 全局根：长期存活的对象（运行时缓存 / interned 字符串等）
// 不像 GcRootHandle 那样自动析构取消注册，需手动 register/unregister
std::vector<GcObject**> globalRoots_;
std::mutex globalRoots_m_;
```

```cpp
public:
    void registerGlobalRoot(GcObject** rootPtr);
    void unregisterGlobalRoot(GcObject** rootPtr);
```

#### Step 3.2：register/unregister 实现

**改动**：[gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) 新增：

```cpp
void GcHeap::registerGlobalRoot(GcObject** rootPtr) {
    std::lock_guard<std::mutex> lk(globalRoots_m_);
    globalRoots_.push_back(rootPtr);
}

void GcHeap::unregisterGlobalRoot(GcObject** rootPtr) {
    std::lock_guard<std::mutex> lk(globalRoots_m_);
    auto it = std::find(globalRoots_.begin(), globalRoots_.end(), rootPtr);
    if (it != globalRoots_.end()) {
        globalRoots_.erase(it);
    }
}
```

#### Step 3.3：markPhase 扫描 globalRoots_

**改动**：[gc.cpp:235-285](file:///d:/you/Aura/runtime/gc.cpp#L235) `markPhase` 入口追加（在 roots_ 扫描之后）：

```cpp
// 3. 从全局根出发标记（运行时缓存 / interned 字符串）
{
    std::lock_guard<std::mutex> lk(globalRoots_m_);
    for (auto* rootPtr : globalRoots_) {
        if (rootPtr && *rootPtr) {
            markObject(*rootPtr);
        }
    }
}
```

#### Step 3.4：GcGlobalRoot<T> 模板

**改动**：[gc.h](file:///d:/you/Aura/runtime/gc.h) 在 GcRootHandle 之后追加：

```cpp
// 全局根句柄：用于运行时缓存的 GC 对象（如 GcString::empty() 单例）
// 构造时注册，析构时取消。常用于 static 局部变量。
template <typename T>
class GcGlobalRoot {
public:
    explicit GcGlobalRoot(T* obj) : ptr_(obj) {
        GcHeap::instance().registerGlobalRoot(
            reinterpret_cast<GcObject**>(&ptr_));
    }
    ~GcGlobalRoot() {
        GcHeap::instance().unregisterGlobalRoot(
            reinterpret_cast<GcObject**>(&ptr_));
    }
    GcGlobalRoot(const GcGlobalRoot&) = delete;
    GcGlobalRoot& operator=(const GcGlobalRoot&) = delete;

    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }

private:
    T* ptr_;
};
```

**关键约束**：
- `T` 必须继承 `GcObject`（保证 `reinterpret_cast<GcObject**>` 合法）
- `&ptr_` 是 `T**`，重解释为 `GcObject**` 后，`*ptr` 读取的是 `T*` 的值（指针值），类型重解释为 `GcObject*` — 与 `GcRootHandle` 同样的 reinterpret_cast 模式
- `ptr_` 字段地址在对象生命周期内不变，所以 `&ptr_` 注册一次即可

#### Step 3.5：应用 — 替换 OOM 错误特例

**改动**：[gc.h:179-181](file:///d:/you/Aura/runtime/gc.h#L179) `oomError_` 字段改造为 `GcGlobalRoot`：

```cpp
// 改动前：
Error oomError_;
bool  oomInit_ = false;

// 改动后（保持向后兼容，渐进迁移）：
Error oomError_;
bool  oomInit_ = false;
// 在 ensureOomError 中使用 GcGlobalRoot 包装（可选优化，本 plan 不强制）
```

**实际不强制改 oomError_**：[gc.cpp:283-284](file:///d:/you/Aura/runtime/gc.cpp#L283) 已通过 `if (oomError_.kind) markObject(oomError_.kind)` 特例处理，行为正确。`GcGlobalRoot` 主要为 [gcstring_optimization.md](file:///d:/you/Aura/plan/gcstring_optimization.md) 的 `GcString::empty()` 等新缓存服务。

**验收**：
- 编译通过
- 简单测试：在 `GcString::empty()` 实现中用 `static GcGlobalRoot<GcString> _empty(GcString::make(""));` 验证 GC 后仍可用

---

### Phase 4：§六 GcWeakHandle<T>（独立）

#### Step 4.1：GcHeap 加 weakHandles_

**改动**：[gc.h](file:///d:/you/Aura/runtime/gc.h) `GcHeap` private 区追加：

```cpp
// 弱引用句柄：sweep 时清空指向已回收对象的句柄
std::vector<GcWeakHandleBase*> weakHandles_;
std::mutex weakHandles_m_;
```

```cpp
public:
    void registerWeak(GcWeakHandleBase* wh);
    void unregisterWeak(GcWeakHandleBase* wh);
```

#### Step 4.2：GcWeakHandleBase / GcWeakHandle<T> 模板

**改动**：[gc.h](file:///d:/you/Aura/runtime/gc.h) 在 GcRootHandle 之后、GcHeap 之前追加：

```cpp
// 弱引用基类：通过基类指针统一管理不同 T 的弱引用
class GcWeakHandleBase {
public:
    explicit GcWeakHandleBase(GcObject* obj) : ptr_(obj) {
        GcHeap::instance().registerWeak(this);
    }
    ~GcWeakHandleBase() {
        GcHeap::instance().unregisterWeak(this);
    }
    GcWeakHandleBase(const GcWeakHandleBase&) = delete;
    GcWeakHandleBase& operator=(const GcWeakHandleBase&) = delete;

    GcObject* get() const { return ptr_; }
    bool valid() const { return ptr_ != nullptr; }
    void clear() { ptr_ = nullptr; }   // 仅 GC 在 sweep 时调用

private:
    GcObject* ptr_;
    friend class GcHeap;
};

template <typename T>
class GcWeakHandle : public GcWeakHandleBase {
public:
    explicit GcWeakHandle(T* obj) : GcWeakHandleBase(static_cast<GcObject*>(obj)) {}
    T* get() const { return static_cast<T*>(GcWeakHandleBase::get()); }
};
```

#### Step 4.3：register/unregister 实现

**改动**：[gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) 新增：

```cpp
void GcHeap::registerWeak(GcWeakHandleBase* wh) {
    std::lock_guard<std::mutex> lk(weakHandles_m_);
    weakHandles_.push_back(wh);
}

void GcHeap::unregisterWeak(GcWeakHandleBase* wh) {
    std::lock_guard<std::mutex> lk(weakHandles_m_);
    auto it = std::find(weakHandles_.begin(), weakHandles_.end(), wh);
    if (it != weakHandles_.end()) {
        weakHandles_.erase(it);
    }
}
```

#### Step 4.4：sweep 时清空无效弱引用

**改动**：[gc.cpp:341-353](file:///d:/you/Aura/runtime/gc.cpp#L341) `sweepPhaseYoung` 在清除前先清空弱引用：

```cpp
void GcHeap::sweepPhaseYoung() {
    // 1. 清空指向未标记对象的弱引用
    {
        std::lock_guard<std::mutex> lk(weakHandles_m_);
        for (auto* wh : weakHandles_) {
            GcObject* obj = wh->get();
            if (obj && !obj->marked) {
                wh->clear();
            }
        }
    }

    // 2. 晋升 + 清除（原有逻辑保持不变）
    for (auto* obj : youngObjects_) {
        if (obj->marked) {
            promoteToOld(obj);
            obj->marked = false;
        }
    }
    youngBytes_ = 0;
    youngObjects_.clear();
}
```

**改动**：[gc.cpp:368-412](file:///d:/you/Aura/runtime/gc.cpp#L368) `sweepPhaseAll` 同样在清除前先清空弱引用（在统计 liveYoung/liveOld 之后、compactAndReclaim 之前）：

```cpp
void GcHeap::sweepPhaseAll() {
    // 1. 统计存活对象（原有逻辑）
    // ...

    // 2. 清空指向死亡对象的弱引用
    {
        std::lock_guard<std::mutex> lk(weakHandles_m_);
        for (auto* wh : weakHandles_) {
            GcObject* obj = wh->get();
            if (obj && !obj->marked) {
                wh->clear();
            }
        }
    }

    // 3. 更新对象列表 + 字节统计（原有逻辑）
    // ...

    // 4. 紧凑 + 页回收（原有逻辑）
    // ...
}
```

**验收**：构造弱引用 → 强引用置 null → GC → 弱引用 `valid()` 返回 false。

---

### Phase 5：§七 Finalizer（独立，但需注意 GcObject 布局变更）

#### Step 5.1：GcObject 加 finalized 字段

**改动**：[types.h](file:///d:/you/Aura/runtime/types.h) `GcObject` 结构追加：

```cpp
struct GcObject {
    const TypeDescriptor* desc       = nullptr;
    GcObject*             next       = nullptr;
    bool                  marked     = false;
    uint8_t               generation = 0;
    bool                  finalized = false;   // 新增：避免重复调用 finalizer
};
```

**注意**：新增 1 字节（实际可能因对齐占 4-8 字节）。所有 GC 对象的 `desc->size` 在 `gc_alloc` 时已固定，此变更需要重新编译所有依赖 `GcObject` 的代码。

#### Step 5.2：TypeDescriptor 加 finalizer 函数指针

**改动**：[types.h](file:///d:/you/Aura/runtime/types.h) `TypeDescriptor` 结构追加：

```cpp
struct TypeDescriptor {
    size_t              size;
    size_t              ptrFieldCount;
    const size_t*       ptrFieldOffsets;
    size_t              inlineArrayFieldCount;
    const InlineArrayField* inlineArrayFields;

    // 新增：finalizer 函数指针（nullptr 表示无 finalizer）
    void (*finalizer)(GcObject* self) = nullptr;
};
```

**关键**：默认 `nullptr`，所以现有所有 `static const TypeDescriptor d = {...}` 仍能编译（C++ 允许聚合初始化省略尾部字段，但需要 `= nullptr` 默认值 — 上面已写）。

**风险点**：[array.h:381-396](file:///d:/you/Aura/runtime/builtin/array.h#L381) 等现有 `TypeDescriptor` 用聚合初始化 `{ size, 2, ptrOffsets, 0, nullptr }`，**5 字段全填**。新增 finalizer 后变成 6 字段，旧代码仍能编译（聚合初始化会默认初始化剩余字段），但需要在头文件加 `= nullptr` 默认值。

#### Step 5.3：sweepPhase 调用 finalizer

**改动**：[gc.cpp:341-353](file:///d:/you/Aura/runtime/gc.cpp#L341) `sweepPhaseYoung` 在清除前调 finalizer：

```cpp
void GcHeap::sweepPhaseYoung() {
    // 1. 清空弱引用（Phase 4 已加）
    // ...

    // 2. 调用 finalizer（新增）
    for (auto* obj : youngObjects_) {
        if (!obj->marked && !obj->finalized) {
            if (obj->desc && obj->desc->finalizer) {
                obj->desc->finalizer(obj);
                obj->finalized = true;
            }
        }
    }

    // 3. 晋升 + 清除（原有逻辑）
    // ...
}
```

**改动**：[gc.cpp:368-412](file:///d:/you/Aura/runtime/gc.cpp#L368) `sweepPhaseAll` 同样在清除前调 finalizer：

```cpp
void GcHeap::sweepPhaseAll() {
    // 1. 统计存活对象（原有）
    // ...

    // 2. 清空弱引用（Phase 4 已加）
    // ...

    // 3. 调用 finalizer（新增）
    bool inFinalizer = true;  // 防止 finalizer 中触发 GC
    for (auto* obj : youngObjects_) {
        if (!obj->marked && !obj->finalized) {
            if (obj->desc && obj->desc->finalizer) {
                obj->desc->finalizer(obj);
                obj->finalized = true;
            }
        }
    }
    for (auto* obj : oldObjects_) {
        if (!obj->marked && !obj->finalized) {
            if (obj->desc && obj->desc->finalizer) {
                obj->desc->finalizer(obj);
                obj->finalized = true;
            }
        }
    }
    inFinalizer = false;

    // 4. 更新对象列表 + 字节统计（原有）
    // ...
}
```

#### Step 5.4：防递归 GC

**问题**：finalizer 中可能调用 `make_string` 等 GC 分配，触发递归 GC。

**应对**：[gc.h](file:///d:/you/Aura/runtime/gc.h) `GcHeap` 加 `inFinalizer_` 标志，`tryAlloc` 检查：

```cpp
class GcHeap {
private:
    bool inFinalizer_ = false;
};

// tryAlloc 中：
GcObject* GcHeap::tryAlloc(size_t size, const TypeDescriptor* desc) {
    // ...（原有逻辑）
    if (youngBytes_ >= kYoungThreshold && !inFinalizer_) {
        minorGc();
        // ...
    }
    // ...
}
```

**简化方案**：本 plan 不实现防递归（保留为风险），由 finalizer 实现者自己保证不在 finalizer 中触发 GC。理由：Aura 当前无 finalizer 使用方，未来真要用时再加保护。

**验收**：
- 编译通过，所有现有 `TypeDescriptor` 初始化不报错
- 构造一个带 finalizer 的类型 → GC 时 finalizer 被调用
- finalizer 未设置时（默认 nullptr）行为不变

---

## 五、可能的风险与应对方案

### 5.1 风险一：STW 死锁

**问题**：[gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) `safepoint` 中，若 GC 执行者线程在等 `all_stopped_cv_` 时崩溃或被取消，其他线程永远阻塞。

**应对**：
- ✅ `safepoint` 不在 try/catch 中调用，异常会传播到调用方
- 🟡 远期改进：加超时机制，但 Aura 当前不需要

### 5.2 风险二：线程注册时序

**问题**：[task.cpp:13-30](file:///d:/you/Aura/runtime/task.cpp#L13) `run_event_loop` 注册主线程前，GC 可能已被触发（如 `ensureOomError` 中 `make_string` 触发 GC）。

**应对**：
- ✅ `safepoint` 中 `registered_threads_.size() <= 1` 走单线程路径
- ✅ 未注册线程时 `threadCount = 0`，也走单线程路径
- ✅ `registerThread` 在 `mainTask.handle()` 之前调用，确保 GC 触发时主线程已注册

### 5.3 风险三：GcObject 布局变更破坏 ABI

**问题**：§七 在 `GcObject` 加 `finalized` 字段，所有依赖 `sizeof(GcObject)` 的代码受影响。

**应对**：
- ✅ Aura 是单二进制项目，无外部 ABI 依赖
- ✅ `gc_alloc<T>` 用 `sizeof(T)` 分配，T 继承 GcObject，会自动适应新布局
- 🟡 风险：若有外部代码假设 `sizeof(GcObject) == 8/16`，需重编

### 5.4 风险四：TypeDescriptor 默认初始化

**问题**：§七 在 `TypeDescriptor` 加 `finalizer` 字段，现有聚合初始化 `{ size, 2, ptrOffsets, 0, nullptr }` 是 5 字段，新增字段后变成 6 字段。

**应对**：
- ✅ C++ 聚合初始化允许省略尾部字段，省略的字段被值初始化（`nullptr`）
- ✅ 头文件中加 `= nullptr` 默认值，进一步保证
- ✅ [array.h:381-411](file:///d:/you/Aura/runtime/builtin/array.h#L381) 等所有现有 `TypeDescriptor` 初始化不报错

### 5.5 风险五：弱引用并发清空

**问题**：§六 `sweepPhase` 持有 `weakHandles_m_` 锁清空弱引用时，用户线程可能正在调 `wh->get()` 读 `ptr_`。

**应对**：
- ⚠️ 这是一个 race condition，但 `ptr_` 是裸指针，读写是原子的
- ✅ 最坏情况是用户读到旧值（已 free 的指针），但下一秒访问就 crash
- 🟡 完整方案：用 `std::atomic<GcObject*>` 替换 `GcObject* ptr_`，但本 plan 不做（增加复杂度，且 Aura 当前无弱引用使用方）

### 5.6 风险六：forceGc 在多线程下重复设置 gc_pending_

**问题**：[gc.cpp:196-199](file:///d:/you/Aura/runtime/gc.cpp#L196) 改造后 `forceGc` 设置 `gc_pending_ = true` 后调 `safepoint()`，但 `safepoint` 内部会重置 `gc_pending_ = false`。

**应对**：
- ✅ 这是预期行为：`forceGc` 触发一次 GC，由 safepoint 处理
- ✅ `forceGc` 不需要单独保护 `gc_pending_`，safepoint 内部的 GC 执行者路径会处理

---

## 六、测试验证方案

### 6.1 Phase 1 STW 验证

```cpp
// 单线程场景：行为不变
fun main(io: Io) {
    let s = "hello"
    for i in 0..100000 {
        let tmp = "iter" + i
    }
    io.println(s)   // 应仍可用
}
```

预期：与改造前一致，无 crash。

### 6.2 Phase 2 forceGc 验证

```aura
fun main(io: Io) {
    let s = "test"
    gc_force()
    io.println(s)             // s 仍可用
    let info = gc_stats()
    io.println(info)         // 打印 GC 统计
}
```

预期输出类似：
```
test
GC: alloc=12KB young=4KB old=8KB gc=1 minor=0 live=42 pages=4
```

### 6.3 Phase 3 GcGlobalRoot 验证

```cpp
// runtime/builtin/string.cpp 中
GcString* GcString::empty() {
    static GcGlobalRoot<GcString> _empty(GcString::make(""));
    return _empty.get();
}

// 测试
fun main(io: Io) {
    let e1 = ""        // 内部调 empty()
    gc_force()
    let e2 = ""
    io.println(e1)     // 仍可用
    io.println(e2)     // 仍可用
}
```

预期：`e1` 和 `e2` 指向同一全局单例，GC 不回收。

### 6.4 Phase 4 GcWeakHandle 验证

```cpp
// runtime 内部测试代码（不通过 Aura 暴露）
GcWeakHandle<GcString> wh(make_string("temp"));
assert(wh.valid());
gc_force();  // "temp" 无强引用，应被回收
assert(!wh.valid());
```

### 6.5 Phase 5 Finalizer 验证

```cpp
// 构造带 finalizer 的类型
struct FileHandle : GcObject {
    int fd;
    static void finalize(GcObject* self) {
        auto* f = static_cast<FileHandle*>(self);
        if (f->fd >= 0) ::close(f->fd);
    }
    static const TypeDescriptor& desc() {
        static const TypeDescriptor d = {
            sizeof(FileHandle), 0, nullptr, 0, nullptr, &finalize
        };
        return d;
    }
};

// 测试
{
    auto* f = gc_alloc<FileHandle>(&FileHandle::desc());
    f->fd = ::open("test.txt", O_RDONLY);
}
gc_force();   // 应调用 finalize，关闭 fd
```

---

## 七、实施顺序与提交粒度

| 顺序 | Phase | 提交点 | 依赖 | 估计行数 |
|:---:|:---|:---|:---|:---:|
| 1 | Phase 1 | commit: "gc: multi-thread STW safepoint" | 无 | ~80 |
| 2 | Phase 2 | commit: "gc: expose gc_force/gc_stats to Aura + thread-safe forceGc" | Phase 1 | ~60 |
| 3 | Phase 3 | commit: "gc: add GcGlobalRoot for runtime caches" | 无（可与 Phase 1/2 并行） | ~50 |
| 4 | Phase 4 | commit: "gc: add GcWeakHandle with sweep-time clearing" | 无 | ~60 |
| 5 | Phase 5 | commit: "gc: add finalizer support" | 无 | ~40 |

**每个 Phase 独立编译 + 测试，失败可回滚单步。**

**建议并行**：Phase 3/4/5 相互独立，可并行开发。Phase 2 依赖 Phase 1。

---

## 八、不实施的事项（明确排除）

| 项 | 原因 |
|:---|:---|
| Aura 顶层 `let` 全局变量 | Aura 语言当前不支持，本 plan 只做运行时缓存场景 |
| `gc_force_minor` 暴露 | 优先级低，先做 `gc_force`（major） |
| GcStats 返回结构化类型 | 简化为字符串格式化，避免引入新 SemType |
| 弱引用原子读写 | 当前无使用方，保留为风险，未来按需做 |
| Finalizer 防递归 GC | 简化方案：由实现者保证，运行时不强制 |
| GcObject 头部紧凑化 | `finalized` 字段占位，未来可优化为位域，本 plan 不做 |
| `gc_pending_` 原子化 | 当前用 `bool`，多线程下有轻微竞争但不影响正确性（最坏情况是某次 safepoint 漏掉，下次会补上） |

---

## 九、与原 plan 的差异

| 原 plan §描述 | 本 plan 实际 |
|:---|:---|
| "§五 全局变量 GC 根注册" 包括 Aura 顶层 let | Aura 不支持顶层 let，仅做运行时缓存场景 |
| "§十八 forceGc 多线程安全" 独立 | 实际就是 §八 STW 的应用，合并入 Phase 2 |
| "TypeDescriptor 加 finalizer" 影响所有现有代码 | C++ 聚合初始化允许省略尾部字段，现有代码不报错 |
| "GcWeakHandle 用 atomic 指针" | 简化为裸指针，当前无使用方 |
| "finalizer 防递归" | 简化为不强制，由实现者保证 |

---

## 十、总结

### 改动规模

- **新增**：~343 行（gc.h +120 / gc.cpp +180 / types.h +8 / BuiltinRegistry.h +15 / ExprGen.cpp +20）
- **修改**：0 行（不破坏现有 API）

### 完成后效果

| 功能 | 状态 |
|:---|:---|
| 多线程 GC 暂停 | ✅ 可支持 sync thread / 异步 io |
| `gc_force()` Aura 可调 | ✅ |
| `gc_stats()` Aura 可调 | ✅ |
| 运行时缓存 GC 安全 | ✅ |
| 弱引用 | ✅ |
| Finalizer | ✅ |

### 后续

完成本 plan 后，[TODO.txt §五](file:///d:/you/Aura/TODO.txt) 的 P1 项可标记为 `[x]`。GC 进入"多线程就绪"状态，可推进：
- [sync_thread_plan.md](file:///d:/you/Aura/plan/sync_thread_plan.md)
- [io_coroutine_plan.md](file:///d:/you/Aura/plan/io_coroutine_plan.md)
- [gcstring_optimization.md](file:///d:/you/Aura/plan/gcstring_optimization.md) 第一阶段（依赖 GcGlobalRoot）
