# GC 缺失功能详细实现 Plan

> 来源：[TODO.txt §五 GC 缺失功能清单](file:///d:/you/Aura/TODO.txt)（2026-07-18 重新核验）
> 当前实现：[runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) + [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp)
> 状态：草案（待审核）
> 日期：2026-07-18

---

## 一、当前 GC 状态诊断

### 1.1 已实现（✅）

| 功能 | 实现位置 |
|:---|:---|
| 标记-清除 + 分代（young/old） | [gc.cpp:204-229](file:///d:/you/Aura/runtime/gc.cpp#L204) |
| 写屏障 + 记忆集 | [gc.cpp:144-149](file:///d:/you/Aura/runtime/gc.cpp#L144) |
| safepoint | [gc.cpp:154-164](file:///d:/you/Aura/runtime/gc.cpp#L154) |
| 精确标记（TypeDescriptor.ptrFieldOffsets） | [gc.cpp:296-310](file:///d:/you/Aura/runtime/gc.cpp#L296) |
| 内联数组扫描（InlineArrayField） | [gc.cpp:312-335](file:///d:/you/Aura/runtime/gc.cpp#L312) |
| minor/major GC + 晋升 | [gc.cpp:341-362](file:///d:/you/Aura/runtime/gc.cpp#L341) |
| 页回收（compactAndReclaim） | [gc.cpp:414-467](file:///d:/you/Aura/runtime/gc.cpp#L414) |
| OOM 错误缓存 | [gc.cpp:125-139](file:///d:/you/Aura/runtime/gc.cpp#L125) |
| 平台抽象（VirtualAlloc/mmap） | [gc.cpp:107-118](file:///d:/you/Aura/runtime/gc.cpp#L107) |

### 1.2 真正的问题（核心诊断）

**问题不在 GC 本身，而在根集合**：

1. **`roots_` 是全局 std::vector**：[gc.h:167](file:///d:/you/Aura/runtime/gc.h#L167) `std::vector<GcRootHandle<GcObject*>*> roots_;`
2. **不是 thread_local**：多线程下所有线程共用一个 roots_，存在数据竞争
3. **CodeGen 不生成 GcRootHandle 包装**：导致 `roots_` 实际为空

### 1.3 未实现清单（本 plan 详细规划）

| 优先级 | 功能 | 章节号 | 状态 |
|:---:|:---|:---:|:---:|
| P0 | GC 实际运行（CodeGen 生成 GcRootHandle） | §二 | ✅ 已完成 |
| P0 | 协程帧 GC 根追踪 | §三 | ✅ 已完成 |
| P1 | 全局变量/静态变量的 GC 根注册 | §五 | ✅ 已完成 |
| P1 | 弱引用 GcWeakHandle<T> | §六 | ✅ 已完成 |
| P1 | Finalizer（终结器）支持 | §七 | ✅ 已完成 |
| P1 | 多线程 GC 暂停（stop-the-world） | §八 | ✅ 已完成 |
| P1 | forceGc 暴露给 Aura 语言 + 多线程安全 | §十八 | ✅ 已完成 |
| P2 | 精确栈扫描（替代保守扫描） | §九 | [-] 暂不实施 |
| P2 | compactAndReclaim 性能优化 | §十 | 待实施 |
| P2 | TLAB（Thread-Local Allocation Buffer） | §十一 | [~] 延后 |
| P2 | GC 触发策略调优 | §十二 | [-] 暂不实施 |
| P2 | 对象可移动性（compacting GC） | §十三 | [~] 延后 |
| P3 | 分代年龄记录 | §十四 | 远期 |
| P3 | 并发 GC（concurrent marking） | §十五 | 远期 |
| P3 | Large Object Space（大对象区） | §十六 | 远期 |
| P3 | GC 日志与统计 | §十七 | 远期 |

### 1.4 状态说明（2026-07-18 更新）

- **§九 精确栈扫描**：P0 完成后 GcRootHandle 已精确注册 roots_，stackRoots_ 保守扫描仅作协程帧兜底。实际影响小，暂不实施。
- **§十一 TLAB**：当前 sync_thread 尚未实施，无多线程分配压力。延后到 sync_thread 完成后再做。
- **§十二 GC 触发策略调优**：当前无配置系统（ConfigDecl 未实现），环境变量方式价值有限。暂不实施，待配置系统完善后再做。
- **§十三 对象可移动性**：风险过高（需更新所有引用：roots / stack / fields / array 元素）。compactAndReclaim 已能回收空页，碎片问题不严重。延后到 profiling 显示严重碎片问题再做。

---

## 二、P0 — GC 实际运行（CodeGen 生成 GcRootHandle）

### 2.1 问题

当前 CodeGen 对 GC 指针局部变量**不生成 GcRootHandle 包装**，导致：
- `roots_` 始终为空
- `markPhase` 从空 roots_ 出发，所有对象被标记为 dead
- `sweepPhaseAll` 误回收所有对象

### 2.2 设计

在 [ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) 中，对每个 GC 类型局部变量生成包装：

```cpp
// Aura 源码：
fun foo() {
    let s = "hello"   // s 是 GcString*（GC 对象）
    io.println(s)
}

// 改造后生成的 C++：
void foo() {
    GcString* s_raw = make_string("hello");
    GcRootHandle<GcString*> s(s_raw);  // ← 注册为 GC 根
    io.println_sync(s.get());
    // s 析构时自动 unregisterRoot
}
```

### 2.3 实施步骤

| Step | 内容 | 文件 |
|:---|:---|:---|
| 2.1 | 在 SemAnalyzer 中标记 GC 类型的局部变量 | [src/Sema/](file:///d:/you/Aura/src/Sema/) |
| 2.2 | CodeGen 的 `genLetDecl` 生成 `GcRootHandle<T>` 包装 | [src/CodeGen/DeclGen.cpp](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp) |
| 2.3 | 变量引用 `genIdentifier` 生成 `s.get()` 而非 `s` | [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) |
| 2.4 | 函数参数也需包装（非协程函数） | 同上 |

### 2.4 验证

```aura
fun main(io: Io) {
    let s = "hello"   // 应当注册为根
    force_gc()        // 手动触发 GC
    io.println(s)     // s 应当仍然可用（未被回收）
}
```

### 2.5 风险

- 🟡 变量名变换：原 `s` → `s_raw` + `s`（GcRootHandle），需调整所有引用
- 🟡 协程函数的帧变量不能用栈 GcRootHandle（需要 registerStackRoots）

---

## 三、P0 — 协程帧 GC 根追踪

### 3.1 问题

协程帧（C++20 coroutine frame）分配在堆上，但：
1. 不在 GC 页上
2. 帧内的 GC 指针未注册为根
3. [task.h](file:///d:/you/Aura/runtime/task.h) 的 promise_type 持有 `continuation_`，但帧内变量不在 roots_

### 3.2 设计

**方案**：在协程函数入口注册帧范围，出口注销。

```cpp
// 生成的协程函数：
task<void> foo() {
    // 协程帧地址（C++20 内置）
    void* frameBegin = __builtin_coro_frame();
    void* frameEnd = static_cast<char*>(frameBegin) + __builtin_coro_frame_size();
    gc_register_stack_roots(frameBegin, frameEnd);

    // ... 函数体

    gc_unregister_stack_roots(frameBegin, frameEnd);
    co_return;
}
```

**问题**：C++20 没有 `__builtin_coro_frame_size`，需要通过 `std::coroutine_handle::promise()` 间接获取。

**替代方案**：在协程函数内每个 GC 变量都用 `GcRootHandle` 包装（同 §二），不走 `stackRoots_` 路径。

### 3.3 实施步骤

| Step | 内容 | 文件 |
|:---|:---|:---|
| 3.1 | 在 `genFunDecl` 中检测是否为协程 | [src/CodeGen/DeclGen.cpp](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp) |
| 3.2 | 协程函数内的 GC 变量用 GcRootHandle 包装（同 §二） | 同上 |
| 3.3 | 验证协程挂起/恢复时 GC 根正确 | - |

---

## 五、P1 — 全局变量/静态变量的 GC 根注册

### 5.1 问题

静态缓存的 GC 指针（如 `GcString::from(true)` 缓存）无法被 markPhase 发现，GC 启用后会被错误回收。

### 5.2 设计

```cpp
// runtime/gc.h 新增
class GcHeap {
public:
    // 注册全局根（永久存活，直到程序退出）
    void registerGlobalRoot(GcObject** globalPtr);
    void unregisterGlobalRoot(GcObject** globalPtr);

private:
    std::vector<GcObject**> globalRoots_;
    std::mutex globalRoots_m_;
};

// 便捷包装
template <typename T>
class GcGlobalRoot {
public:
    GcGlobalRoot(T* obj) : ptr_(obj) {
        GcHeap::instance().registerGlobalRoot(
            reinterpret_cast<GcObject**>(&ptr_));
    }
    ~GcGlobalRoot() {
        GcHeap::instance().unregisterGlobalRoot(
            reinterpret_cast<GcObject**>(&ptr_));
    }
    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
private:
    T* ptr_;
};
```

### 5.3 markPhase 集成

```cpp
// gc.cpp markPhase 新增
void GcHeap::markPhase(bool youngOnly) {
    // ... 现有 roots_ / stackRoots_ / rememberedSet_ 扫描 ...

    // 新增：扫描全局根
    for (auto* globalPtr : globalRoots_) {
        if (globalPtr && *globalPtr) {
            markObject(*globalPtr);
        }
    }
    // ...
}
```

### 5.4 使用示例

```cpp
// string.cpp 中 GcString::from(bool) 缓存
GcString* GcString::from(bool val) {
    static GcGlobalRoot<GcString> _true(GcString::make("true"));
    static GcGlobalRoot<GcString> _false(GcString::make("false"));
    return val ? _true.get() : _false.get();
}
```

### 5.5 实施步骤

| Step | 内容 | 文件 |
|:---|:---|:---|
| 5.1 | `GcHeap` 加 `globalRoots_` + register/unregister API | [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) + [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) |
| 5.2 | `markPhase` 扫描 `globalRoots_` | [gc.cpp:235](file:///d:/you/Aura/runtime/gc.cpp#L235) |
| 5.3 | 新增 `GcGlobalRoot<T>` 模板 | [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) |
| 5.4 | 改造 `GcString::empty()` / `from(bool)` / `from(int)` 用 `GcGlobalRoot` | [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) |

---

## 六、P1 — 弱引用 GcWeakHandle<T>

### 6.1 用途

- 字符串驻留表（interning）
- Map 缓存
- 不阻止 GC 回收，但访问前需检查 valid()

### 6.2 设计

```cpp
// runtime/gc.h 新增
class GcWeakHandleBase {
public:
    explicit GcWeakHandleBase(GcObject* obj) : ptr_(obj) {
        GcHeap::instance().registerWeak(this);
    }
    ~GcWeakHandleBase() {
        GcHeap::instance().unregisterWeak(this);
    }
    GcObject* get() const { return ptr_; }
    bool valid() const { return ptr_ != nullptr; }
    void clear() { ptr_ = nullptr; }  // GC 在 sweep 时调用
private:
    GcObject* ptr_;
    friend class GcHeap;
};

template <typename T>
class GcWeakHandle : public GcWeakHandleBase {
public:
    explicit GcWeakHandle(T* obj) : GcWeakHandleBase(obj) {}
    T* get() const { return static_cast<T*>(GcWeakHandleBase::get()); }
};

class GcHeap {
public:
    void registerWeak(GcWeakHandleBase* wh);
    void unregisterWeak(GcWeakHandleBase* wh);
private:
    std::vector<GcWeakHandleBase*> weakHandles_;
};
```

### 6.3 sweep 时清空无效弱引用

```cpp
// gc.cpp sweepPhaseAll
void GcHeap::sweepPhaseAll() {
    // ... 现有逻辑 ...

    // 清空未标记对象的弱引用
    for (auto* wh : weakHandles_) {
        if (wh->get() && !wh->get()->marked) {
            wh->clear();
        }
    }
}
```

### 6.4 实施步骤

| Step | 内容 | 文件 |
|:---|:---|:---|
| 6.1 | `GcHeap` 加 `weakHandles_` + register/unregister API | [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) + [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) |
| 6.2 | `GcWeakHandle<T>` 模板 | 同上 |
| 6.3 | `sweepPhaseAll` 清空无效弱引用 | [gc.cpp:368](file:///d:/you/Aura/runtime/gc.cpp#L368) |
| 6.4 | 单元测试：弱引用持有对象被 GC 后 valid() 返回 false | - |

---

## 七、P1 — Finalizer（终结器）支持

### 7.1 用途

资源句柄（文件、socket、数据库连接）在 GC 时自动关闭。

### 7.2 设计

```cpp
// runtime/types.h 新增
struct GcObject {
    const TypeDescriptor* desc;
    bool marked;
    GcObject* next;
    uint8_t generation;
    bool finalized = false;  // 新增：防止多次 finalizer 调用
};

// TypeDescriptor 新增 finalizer 函数指针
struct TypeDescriptor {
    size_t size;
    size_t ptrFieldCount;
    const size_t* ptrFieldOffsets;
    size_t inlineArrayFieldCount;
    const InlineArrayField* inlineArrayFields;
    void (*finalizer)(GcObject*) = nullptr;  // 新增
};
```

### 7.3 sweep 前调用 finalizer

```cpp
// gc.cpp sweepPhaseAll 改造
void GcHeap::sweepPhaseAll() {
    // 1. 对未标记对象调用 finalizer（仅一次）
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
    // 2. 现有清除逻辑
    // ...
}
```

### 7.4 实施步骤

| Step | 内容 | 文件 |
|:---|:---|:---|
| 7.1 | `GcObject` 加 `finalized` 字段 | [runtime/types.h](file:///d:/you/Aura/runtime/types.h) |
| 7.2 | `TypeDescriptor` 加 `finalizer` 函数指针 | 同上 |
| 7.3 | `sweepPhaseAll` 在清除前调用 finalizer | [gc.cpp:368](file:///d:/you/Aura/runtime/gc.cpp#L368) |
| 7.4 | `sweepPhaseYoung` 同样处理（minor GC 时） | [gc.cpp:341](file:///d:/you/Aura/runtime/gc.cpp#L341) |

### 7.5 风险

- ⚠️ finalizer 中再分配会触发递归 GC
- **缓解**：finalizer 执行期间设置 `inFinalizer_ = true`，禁止 GC

---

## 八、P1 — 多线程 GC 暂停（stop-the-world）

### 8.1 用途

[sync_thread_plan.md](file:///d:/you/Aura/plan/sync_thread_plan.md) 和 [io_coroutine_plan.md](file:///d:/you/Aura/plan/io_coroutine_plan.md) 的共同前置。

### 8.2 设计

```cpp
// runtime/gc.h 新增
class GcHeap {
public:
    // 线程注册（用于 GC stop-the-world）
    void registerThread(std::thread::id id);
    void unregisterThread(std::thread::id id);

    // safepoint 改造：若 gc_pending_ 则阻塞
    void safepoint();

private:
    std::mutex threads_m_;
    std::vector<std::thread::id> registered_threads_;
    std::atomic<bool> gc_in_progress_{false};
    std::atomic<int> stopped_threads_{0};
    std::condition_variable all_stopped_cv_;
    std::mutex all_stopped_m_;
};

// gc.cpp safepoint 改造
void GcHeap::safepoint() {
    if (!gc_pending_) return;

    // 单线程场景：直接执行 GC
    if (registered_threads_.size() <= 1) {
        gc_pending_ = false;
        if (youngBytes_ >= kYoungThreshold / 2) minorGc();
        if (oldBytes_ >= kOldThreshold) majorGc();
        return;
    }

    // 多线程场景：请求所有线程停止
    gc_in_progress_ = true;
    // 等待所有线程到达 safepoint
    {
        std::unique_lock<std::mutex> lk(all_stopped_m_);
        all_stopped_cv_.wait(lk, [this]{
            return stopped_threads_.load() >= registered_threads_.size() - 1;
        });
    }

    // 所有线程已停止，执行 GC
    gc_pending_ = false;
    if (youngBytes_ >= kYoungThreshold / 2) minorGc();
    if (oldBytes_ >= kOldThreshold) majorGc();

    // 唤醒所有线程
    gc_in_progress_ = false;
    stopped_threads_ = 0;
    all_stopped_cv_.notify_all();
}
```

### 8.3 每个线程的 safepoint 行为

```cpp
// 修改 safepoint：多线程场景下阻塞
void GcHeap::safepoint() {
    if (!gc_pending_) return;

    if (gc_in_progress_) {
        // 其他线程正在执行 GC，本线程停止
        stopped_threads_++;
        std::unique_lock<std::mutex> lk(all_stopped_m_);
        all_stopped_cv_.wait(lk, [this]{ return !gc_in_progress_; });
        return;
    }

    // 本线程成为 GC 执行者
    // ... 上面的逻辑
}
```

### 8.4 实施步骤

| Step | 内容 | 文件 |
|:---|:---|:---|
| 8.1 | `GcHeap` 加线程注册 + STW 机制 | [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) + [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) |
| 8.2 | 改造 `safepoint()` 支持多线程 | [gc.cpp:154-164](file:///d:/you/Aura/runtime/gc.cpp#L154) |
| 8.3 | IoCompletionPort worker / sync thread 线程注册 | [runtime/win_iocp.h](file:///d:/you/Aura/runtime/win_iocp.h) + sync_thread |
| 8.4 | 单元测试：多线程下 GC 不 crash | - |

---

## 九、P2 — 精确栈扫描（替代保守扫描）  [-] 暂不实施（2026-07-18）

> **暂不实施原因**：P0 完成后 CodeGen 已为 GC 指针类型的局部变量生成 GcRootHandle 包装（精确注册到 roots_），协程帧也由 GcRootHandle 精确扫描。stackRoots_ 的保守扫描仅作协程帧兜底。实际影响小：协程帧内大部分对齐数据是 GcRootHandle::ptr_ 字段，本来就是 GC 指针；desc/size 验证会跳过非 GC 对象。
>
> **远期改进**：若 profiling 显示内存占用过高，再做精确扫描。

### 9.1 现状

[gc.cpp:243-262](file:///d:/you/Aura/runtime/gc.cpp#L243) 的 `stackRoots_` 采用保守扫描 — 按指针对齐扫描栈内存，把候选指针当作 GcObject* 检查是否在 page 范围。

**问题**：保守扫描会"过度保留"对象（把整数误认为指针），导致内存泄漏。

### 9.2 设计

**编译器生成栈帧描述符**：

```cpp
// 每个函数生成一个静态描述符
struct StackFrameDesc {
    int localVarCount;
    const int* gcVarOffsets;  // GC 指针局部变量在帧中的偏移
};

// 函数入口注册
void foo() {
    static const StackFrameDesc _frame_desc = {
        .localVarCount = 2,
        .gcVarOffsets = new int[2]{0, 8}  // 偏移
    };
    GcHeap::instance().registerFrame(&_frame_desc, __builtin_frame_address(0));
    // ...
    GcHeap::instance().unregisterFrame(&_frame_desc);
}
```

### 9.3 markPhase 改造

```cpp
void GcHeap::markPhase(bool youngOnly) {
    // ... roots_ 扫描 ...

    // 精确扫描栈帧（替代保守扫描）
    for (auto& [desc, frameBase] : registeredFrames_) {
        for (int i = 0; i < desc->localVarCount; ++i) {
            void* slot = static_cast<char*>(frameBase) + desc->gcVarOffsets[i];
            GcObject* obj = *static_cast<GcObject**>(slot);
            if (obj) markObject(obj);
        }
    }
}
```

### 9.4 实施步骤

| Step | 内容 | 文件 |
|:---|:---|:---|
| 9.1 | 定义 `StackFrameDesc` 结构 | [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) |
| 9.2 | `GcHeap` 加 `registeredFrames_` + register/unregister API | 同上 |
| 9.3 | `markPhase` 用精确扫描替代保守扫描 | [gc.cpp:243-262](file:///d:/you/Aura/runtime/gc.cpp#L243) |
| 9.4 | CodeGen 生成 `StackFrameDesc` + 注册代码 | [src/CodeGen/](file:///d:/you/Aura/src/CodeGen/) |

**注意**：保守扫描作为 fallback 保留（用于协程帧等无法生成描述符的场景）。

---

## 十、P2 — compactAndReclaim 性能优化

### 10.1 现状

[gc.cpp:414-467](file:///d:/you/Aura/runtime/gc.cpp#L414) 的 `compactAndReclaim` 用 O(n×m) 双重循环判断每个 page 是否有存活对象。

### 10.2 设计

**按 page 地址分桶**：

```cpp
void GcHeap::compactAndReclaim() {
    std::vector<GcObject*> allLive;
    allLive.reserve(youngObjects_.size() + oldObjects_.size());
    for (auto* obj : youngObjects_) allLive.push_back(obj);
    for (auto* obj : oldObjects_)   allLive.push_back(obj);

    if (allLive.empty()) {
        freeAllPages();
        return;
    }

    // 按 page 地址分桶（O(m log m) 排序）
    std::unordered_map<Page*, std::vector<GcObject*>> liveByPage;
    for (auto* obj : allLive) {
        char* objPtr = reinterpret_cast<char*>(obj);
        // 找到 obj 所在的 page（线性查找，但通常 page 数量少）
        for (Page* page = headPage_; page; page = page->next) {
            if (objPtr >= page->data && objPtr < page->data + kPageSize) {
                liveByPage[page].push_back(obj);
                break;
            }
        }
    }

    // 一次遍历 pages（O(n)）
    Page* page = headPage_;
    Page* newHead = nullptr;
    Page* newTail = nullptr;
    while (page) {
        Page* next = page->next;
        if (liveByPage.count(page)) {
            // 保留此页
            page->next = nullptr;
            if (!newHead) { newHead = page; newTail = page; }
            else { newTail->next = page; newTail = page; }
        } else {
            // 释放空页
            #ifdef _WIN32
            VirtualFree(page, 0, MEM_RELEASE);
            #else
            munmap(page, sizeof(Page));
            #endif
        }
        page = next;
    }
    headPage_ = newHead;
    currentPage_ = newTail;
}
```

### 10.3 性能改进

| | 改造前 | 改造后 |
|:---|:---|:---|
| 时间复杂度 | O(n × m)（n=pages, m=live objects） | O(n + m log m) |
| 1000 个对象，100 个 page | 100K 次比较 | ~1K 次操作 |

### 10.4 实施步骤

| Step | 内容 | 文件 |
|:---|:---|:---|
| 10.1 | 改造 `compactAndReclaim` 用分桶 | [gc.cpp:414](file:///d:/you/Aura/runtime/gc.cpp#L414) |
| 10.2 | 性能测试对比 | - |

---

## 十一、P2 — TLAB（Thread-Local Allocation Buffer）  [~] 延后（2026-07-18）

> **延后原因**：当前 sync_thread_plan.md 尚未实施，无多线程分配压力。bumpAlloc 全局单线程在当前规模下足够用。待 sync_thread 完成后再做。
>
> **前置条件**：§八 多线程 GC 暂停（已完成）。

### 11.1 用途

多线程并发分配避免锁竞争。

### 11.2 设计

```cpp
// runtime/gc.h 新增
class GcHeap {
public:
    // 每线程独立分配缓冲
    struct Tlab {
        Page* currentPage = nullptr;
        size_t bumpOffset = 0;
    };
    thread_local static Tlab* tlab_;

    // 线程注册时分配 TLAB
    void registerThread(std::thread::id id);
};

// alloc 改造：优先用 TLAB
GcObject* GcHeap::tryAlloc(size_t size, const TypeDescriptor* desc) {
    // 1. 尝试 TLAB 分配（无锁）
    if (tlab_ && tlab_->currentPage &&
        tlab_->bumpOffset + size <= kPageSize) {
        void* mem = tlab_->currentPage->data + tlab_->bumpOffset;
        tlab_->bumpOffset += size;
        // ... 初始化对象
        return static_cast<GcObject*>(mem);
    }

    // 2. TLAB 满或无 TLAB，走全局分配（加锁）
    std::lock_guard<std::mutex> lk(alloc_m_);
    // ... 现有 bumpAlloc 逻辑
}
```

### 11.3 实施步骤

| Step | 内容 | 文件 |
|:---|:---|:---|
| 11.1 | `GcHeap` 加 TLAB 机制 | [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) + [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) |
| 11.2 | `alloc` 改造优先用 TLAB | [gc.cpp:44](file:///d:/you/Aura/runtime/gc.cpp#L44) |
| 11.3 | 线程注册时分配 TLAB | [gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) |

**前置**：§八 多线程 GC 暂停

---

## 十二、P2 — GC 触发策略调优  [-] 暂不实施（2026-07-18）

> **暂不实施原因**：当前 Aura 无配置系统（ConfigDecl 未实现），环境变量方式价值有限。默认阈值（256KB / 1MB）在 Aura 当前规模下足够用。
>
> **远期改进**：待 Aura 配置系统完善后再做，通过 `#gc.young_threshold` 等指令配置。

### 12.1 现状

[gc.h:122-124](file:///d:/you/Aura/runtime/gc.h#L122) `kYoungThreshold=256KB` / `kOldThreshold=1MB` 写死。

### 12.2 设计

```cpp
class GcHeap {
public:
    // 运行时可配置
    void setYoungThreshold(size_t bytes);
    void setOldThreshold(size_t bytes);
private:
    size_t youngThreshold_ = 256 * 1024;
    size_t oldThreshold_ = 1024 * 1024;
};

// 通过 #gc.* 配置
// #gc.young_threshold = 512KB
// #gc.old_threshold = 2MB
```

### 12.3 实施步骤

| Step | 内容 | 文件 |
|:---|:---|:---|
| 12.1 | `kYoungThreshold` / `kOldThreshold` 改为非 const 成员 | [runtime/gc.h:122-124](file:///d:/you/Aura/runtime/gc.h#L122) |
| 12.2 | 加 setter + ConfigDecl 支持 | 同上 + [src/Sema/](file:///d:/you/Aura/src/Sema/) |

---

## 十三、P2 — 对象可移动性（compacting GC）  [~] 延后（2026-07-18）

> **延后原因**：风险过高（需更新所有引用：roots / stack / fields / array 元素）。compactAndReclaim 已能回收空页，碎片问题不严重。
>
> **远期改进**：待 profiling 显示严重碎片问题再做。需要为 GcRootHandle::ptr_ / globalRoots_ / Array<T*> 元素 / 字段引用等所有引用都增加更新逻辑。

### 13.1 用途

消除内存碎片，存活对象拷贝到新页。

### 13.2 设计

**转发指针**：

```cpp
struct GcObject {
    // ... 现有字段
    GcObject* forwarded = nullptr;  // 新增：GC 移动后的新地址
};

// mark-compact 算法
void GcHeap::compact() {
    // 1. 计算每个存活对象的新地址
    Page* newPage = allocPage();
    size_t offset = 0;
    for (auto* obj : allLiveObjects_) {
        obj->forwarded = reinterpret_cast<GcObject*>(newPage->data + offset);
        offset += obj->desc->size;
    }

    // 2. 更新所有引用（roots / stackRoots / fields / array elements）
    updateAllReferences();

    // 3. 拷贝对象到新页
    for (auto* obj : allLiveObjects_) {
        memcpy(obj->forwarded, obj, obj->desc->size);
    }

    // 4. 释放旧页
    freeAllPages();
    headPage_ = newPage;
}
```

### 13.3 风险

- ⚠️ 高复杂度：需要更新所有引用（roots / stack / fields / array）
- ⚠️ GcRootHandle 的 `ptr_` 需要更新
- ⚠️ stackRoots_ 的保守扫描指针需要更新

### 13.4 实施步骤

| Step | 内容 | 文件 |
|:---|:---|:---|
| 13.1 | `GcObject` 加 `forwarded` 字段 | [runtime/types.h](file:///d:/you/Aura/runtime/types.h) |
| 13.2 | 实现 `compact()` 算法 | [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) |
| 13.3 | 实现 `updateAllReferences()` | 同上 |
| 13.4 | majorGc 中调用 compact() | [gc.cpp:217](file:///d:/you/Aura/runtime/gc.cpp#L217) |

---

## 十四、P3 — 分代年龄记录

### 14.1 设计

```cpp
struct GcObject {
    // ... 现有字段
    uint8_t age = 0;  // 新增：存活次数
};

void GcHeap::sweepPhaseYoung() {
    for (auto* obj : youngObjects_) {
        if (obj->marked) {
            obj->age++;
            if (obj->age >= kPromotionAge) {  // 默认 2
                promoteToOld(obj);
            } else {
                obj->marked = false;
                // 保留在新生代
            }
        }
    }
    // ...
}
```

---

## 十五、P3 — 并发 GC（concurrent marking）

### 15.1 设计

mark 阶段并发执行，仅 sweep 阶段暂停。

**需要**：
- 读屏障（追踪并发 mark 期间的引用变更）
- 写屏障加强（三色标记 invariant）

**风险**：极复杂，建议远期考虑。

---

## 十六、P3 — Large Object Space（大对象区）

### 16.1 设计

```cpp
class GcHeap {
    std::vector<std::pair<void*, size_t>> largeObjects_;  // 独立追踪的大对象
};

GcObject* GcHeap::tryAlloc(size_t size, const TypeDescriptor* desc) {
    if (size > kLargeThreshold) {  // 32KB
        return allocLarge(size, desc);
    }
    // ... 现有 bumpAlloc
}
```

---

## 十七、P3 — GC 日志与统计

### 17.1 设计

```cpp
class GcHeap {
public:
    void setVerbose(bool v) { verbose_ = v; }
private:
    bool verbose_ = false;

    void logGcEvent(const char* event, size_t durationMs, size_t freedBytes) {
        if (!verbose_) return;
        std::fprintf(stderr, "[GC] %s: %zums, freed %zu bytes\n",
                     event, durationMs, freedBytes);
    }
};
```

通过 `#gc.verbose = true` 开启。

---

## 十八、P1 — forceGc 暴露给 Aura 语言 + 多线程安全

### 18.1 当前状态

[gc.h:100](file:///d:/you/Aura/runtime/gc.h#L100) 声明 `void forceGc()`，[gc.cpp:196-199](file:///d:/you/Aura/runtime/gc.cpp#L196) 实现：

```cpp
void GcHeap::forceGc() {
    gcPending_ = false;
    majorGc();   // 直接调 majorGc，无 STW 保护
}
```

[gc.h:215-217](file:///d:/you/Aura/runtime/gc.h#L215) 提供内联 C++ API：

```cpp
inline void force_gc() {
    GcHeap::instance().forceGc();
}
```

### 18.2 三个核心问题

| # | 问题 | 影响 |
|:---:|:---|:---|
| Q1 | **未暴露给 Aura 语言** — BuiltinRegistry 无 GC 函数注册 | Aura 用户无法手动触发 GC（我之前 plan §二.4 的验证代码用了 `force_gc()`，实际 Aura 不支持） |
| Q2 | **多线程不安全** — 直接调 `majorGc()`，没有 STW | 多线程下会与 mutator 并发访问对象，导致 use-after-free 或标记错误 |
| Q3 | **无返回值/统计** — void 返回 | 用户/调试无法知道 GC 回收了多少内存、耗时多久 |

### 18.3 Q1 — 暴露给 Aura 语言

**设计**：在 BuiltinRegistry 注册 `gc_force` / `gc_force_minor` / `gc_stats` 函数。

```cpp
// src/Sema/BuiltinRegistry.h 中 functions_ 表新增
// 参考 [BuiltinRegistry.h:249-254](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h#L249) 的 range 注册方式
{
    "gc_force",
    BuiltinFunction{
        .returnType = ReturnTypeInfo{.kind = ReturnTypeInfo::Kind::None},
        .paramCount = 0,
        .async = false,  // 同步函数
    }
},
{
    "gc_force_minor",
    BuiltinFunction{
        .returnType = ReturnTypeInfo{.kind = ReturnTypeInfo::Kind::None},
        .paramCount = 0,
        .async = false,
    }
},
{
    "gc_stats",
    BuiltinFunction{
        .returnType = ReturnTypeInfo{
            .kind = ReturnTypeInfo::Kind::Named,
            .typeName = "string"  // 返回统计信息字符串
        },
        .paramCount = 0,
        .async = false,
    }
}
```

**Aura 调用方式**：

```aura
fun main(io: Io) {
    let s = "hello"
    gc_force()              // 手动触发 major GC
    gc_force_minor()        // 手动触发 minor GC
    let info = gc_stats()   // 获取统计信息
    io.println(info)
    io.println(s)           // s 仍可用
}
```

**runtime 实现层**：

```cpp
// runtime/gc.h 新增公开 API
inline void gc_force_major() { GcHeap::instance().forceGc(); }
inline void gc_force_minor_gc() { GcHeap::instance().forceMinorGc(); }
inline GcStats gc_get_stats() { return GcHeap::instance().getStats(); }
```

### 18.4 Q2 — 多线程安全

**问题**：[gc.cpp:196-199](file:///d:/you/Aura/runtime/gc.cpp#L196) 的 `forceGc()` 直接调 `majorGc()`，在多线程场景下：
- 其他线程可能正在修改对象图（mutator 与 marker 并发）
- 导致标记错误或 use-after-free

**改造**：forceGc 走 STW 流程，不直接执行 GC。

```cpp
// gc.cpp 改造
void GcHeap::forceGc() {
    // 不直接调 majorGc，而是设置 gcPending_ 让 safepoint 处理
    gcPending_ = true;
    forceRequested_ = true;  // 新增：标记是用户主动请求

    // 单线程场景：当前线程直接执行
    if (registered_threads_.size() <= 1) {
        gcPending_ = false;
        forceRequested_ = false;
        majorGc();
        return;
    }

    // 多线程场景：请求 STW，等待所有线程到达 safepoint
    // （复用 §八 的 STW 机制）
    safepoint();
    forceRequested_ = false;
}
```

**前置**：§八 多线程 GC 暂停（STW）

### 18.5 Q3 — 返回统计信息

**设计**：新增 `GcStats` 结构 + `gc_stats()` 函数。

```cpp
// runtime/gc.h 新增
struct GcStats {
    size_t allocatedBytes;
    size_t youngBytes;
    size_t oldBytes;
    size_t gcCount;
    size_t minorGcCount;
    size_t liveObjectCount;     // 新增：存活对象数
    size_t pageCount;           // 新增：GC 页数
    int64_t lastGcDurationMs;   // 新增：上次 GC 耗时
};

class GcHeap {
public:
    GcStats getStats() const;
private:
    int64_t lastGcDurationMs_ = 0;
};
```

**gc_stats() Aura 返回**：格式化字符串（类似 JVM 的 `-XX:+PrintGC`）。

```
GC Stats: allocated=1.2MB young=256KB old=960KB
  gcCount=3 minorGcCount=12 liveObjects=42 pages=8
  lastGc=2ms
```

### 18.6 新增 forceMinorGc

当前只有 `forceGc()`（major），应该补充 `forceMinorGc()`：

```cpp
// runtime/gc.h
class GcHeap {
public:
    void forceGc();         // major（已有）
    void forceMinorGc();   // 新增：minor
};

// runtime/gc.cpp
void GcHeap::forceMinorGc() {
    gcPending_ = true;
    forceMinorRequested_ = true;
    if (registered_threads_.size() <= 1) {
        gcPending_ = false;
        forceMinorRequested_ = false;
        minorGc();
        return;
    }
    safepoint();
    forceMinorRequested_ = false;
}
```

### 18.7 实施步骤

| Step | 内容 | 文件 | 优先级 |
|:---|:---|:---|:---:|
| 18.1 | BuiltinRegistry 注册 `gc_force` / `gc_force_minor` / `gc_stats` | [src/Sema/BuiltinRegistry.h](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h) | P1 |
| 18.2 | runtime 新增 `GcStats` 结构 + `getStats()` | [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) + [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) | P1 |
| 18.3 | CodeGen 生成 `gc_force()` 调用 `aura_rt::gc_force_major()` | [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) | P1 |
| 18.4 | `forceGc()` 多线程安全改造（走 STW） | [runtime/gc.cpp:196](file:///d:/you/Aura/runtime/gc.cpp#L196) | P1（依赖 §八） |
| 18.5 | 新增 `forceMinorGc()` | [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) + [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) | P2 |
| 18.6 | `gc_stats()` 格式化字符串实现 | [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) | P2 |

### 18.8 验证

```aura
fun main(io: Io) {
    let s = "hello"
    gc_force()                          // 用户主动触发 GC
    io.println("after gc: " + s)        // s 仍可用
    let info = gc_stats()
    io.println(info)                     // 打印统计
    gc_force_minor()                     // minor GC
}
```

### 18.9 与其他 plan 的依赖

- **依赖**：§八 多线程 GC 暂停（Q2 多线程安全）
- **被依赖**：gcstring_optimization 第一阶段验证代码需要 `gc_force()`

---

## 十九、实施优先级总览

### Phase 1：让 GC 真正工作（P0）

| 顺序 | 任务 | 依赖 |
|:---:|:---|:---|
| 1 | §二 CodeGen 生成 GcRootHandle | 无 |
| 2 | §三 协程帧 GC 根追踪 | §二 |

### Phase 2：多线程支持（P1）

| 顺序 | 任务 | 依赖 |
|:---:|:---|:---|
| 3 | §八 多线程 GC 暂停 | §二 |
| 4 | §十八 forceGc 暴露 + 多线程安全（Q1/Q3） | §二 |
| 5 | §五 全局变量 GC 根注册 | §二 |
| 6 | §六 弱引用 GcWeakHandle | §二 |
| 7 | §七 Finalizer | §二 |

### Phase 3：性能优化（P2）

| 顺序 | 任务 | 依赖 |
|:---:|:---|:---|
| 8 | §十 compactAndReclaim 性能优化 | 无 |
| 9 | §九 精确栈扫描 | §二 |
| 10 | §十一 TLAB | §八 |
| 11 | §十二 GC 触发策略调优 | 无 |
| 12 | §十三 对象可移动性 | §九 |
| 13 | §十八 forceMinorGc + gc_stats 格式化（Q3 剩余） | §八 |

### Phase 4：远期（P3）

| 顺序 | 任务 | 依赖 |
|:---:|:---|:---|
| 14 | §十四 分代年龄记录 | 无 |
| 15 | §十五 并发 GC | §十三 |
| 16 | §十六 Large Object Space | 无 |
| 17 | §十七 GC 日志与统计 | 无 |

---

## 二十、风险总结

### 19.1 主要风险

1. **§二 CodeGen 改造规模大**：需要修改所有变量引用 `s` → `s.get()`
   - **缓解**：分阶段实施，先支持函数内局部变量，再扩展到参数

2. **§八 多线程 STW 复杂**：需要处理死锁、活锁、优先级反转
   - **缓解**：先用最简版本（所有线程都到达 safepoint 才 GC）

3. **§十三 对象可移动性最高风险**：更新所有引用易出 bug
   - **缓解**：推迟到 P2，先确保 non-moving GC 稳定

### 19.2 验证策略

每完成一个 Phase：
1. 单元测试：基本功能不回归
2. 压力测试：长时间运行 + 频繁 GC，检查内存使用
3. 多线程测试（Phase 2 后）：多线程下 GC 不 crash

---

## 二十一、与 sync_thread_plan / io_coroutine_plan 的关系

| plan | 依赖的 GC 功能 |
|:---|:---|
| [sync_thread_plan.md](file:///d:/you/Aura/plan/sync_thread_plan.md) | §八 多线程 GC 暂停 |
| [io_coroutine_plan.md](file:///d:/you/Aura/plan/io_coroutine_plan.md) | §八 多线程 GC 暂停 |
| [gcstring_optimization.md](file:///d:/you/Aura/plan/gcstring_optimization.md) | §二 CodeGen 生成 GcRootHandle（间接） |

**关键路径**：
1. **先做 §二/§三**（让 GC 真正工作）
2. **再做 §八**（多线程 GC 暂停）
3. **然后才能做 sync_thread / io_coroutine**

---

## 二十二、总结

GC 缺失功能共 17 项，按优先级分 4 个 Phase：

- **Phase 1（P0）**：让 GC 真正工作（§二/§三/§四）— 没有这个，所有其他优化都没意义
- **Phase 2（P1）**：多线程支持（§八/§十八/§五/§六/§七）— sync_thread 和 io_coroutine 的前置
- **Phase 3（P2）**：性能优化（§十/§九/§十一/§十二/§十三）— 内存泄漏 / 性能问题
- **Phase 4（P3）**：远期功能（§十四/§十五/§十六/§十七）— 高级 GC 特性

**关键洞察**：当前 GC 实现是**完整的**（mark/sweep/promote/compact 都在），**真正缺失的是 CodeGen 的根注册**。Phase 1 完成后，GC 立即能工作。
