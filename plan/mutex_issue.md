# Issue：sync.Mutex 用户级互斥锁设计

> 来源：sync_thread_plan 实施时用户提出"锁和通道都需要"
> 类型：设计决策 issue
> 日期：2026-07-24
> 状态：草案（待审查）
> 关联：[sync_thread_plan.md](file:///d:/you/Aura/plan/sync_thread_plan.md)、[thread_pool_work_stealing_issue.md](file:///d:/you/Aura/plan/thread_pool_work_stealing_issue.md)

---

## 一、背景

sync thread 语句允许多个 spawn 任务并行执行。当任务通过指针共享 GC 对象（如 `Array<T>*`、`GcString*` 或用户自定义 record）并修改时，存在数据竞争。当前 change.md 明确写着：

> ❌ `sync thread` 块内的 GC 暂停只在 L2/L3 safepoint 触发（非并发 GC）
> spawn 参数值传递；GC 对象通过指针共享时**用户需自行加锁**（v1 不提供语言级锁）

这是个矛盾：sync thread 提供了并行能力，却没提供任何加锁机制。本 issue 解决该缺口。

设计已与用户确认采用**双轨方案**：channel<T>（CSP，独立 plan）+ sync.Mutex（本 issue）。

---

## 二、设计目标

1. **线程安全**：保护临界区，防止数据竞争
2. **无忘记 unlock 风险**：RAII 或块语句，避免人为错误
3. **与 Aura 体系一致**：与 GcRootHandle 的 RAII 模式一致
4. **spawn 友好**：能作为 spawn 参数传递（值传递语义）
5. **GC 兼容**：Mutex 本身可作为 GC 对象，生命周期由 GC 管理

---

## 三、方案对比

### 方案 A：手动 lock/unlock（Go 风格）

```aura
let m = sync.Mutex()
sync thread {
    spawn (io: Io, m: sync.Mutex) {
        m.lock()
        // 临界区
        m.unlock()
    }
}
```

- ✅ 简单直接，易理解
- ❌ 忘记 unlock 会导致死锁
- ❌ Aura 无 defer 语句，无法保证异常路径 unlock
- ❌ 与 Aura 的 RAII 哲学不一致

### 方案 B：RAII Guard（推荐）

```aura
let m = sync.Mutex()
sync thread {
    spawn (io: Io, m: sync.Mutex) {
        let guard = m.lock()   // 构造时 lock
        // 临界区
        // guard 析构自动 unlock（作用域结束）
    }
}
```

- ✅ 无忘记 unlock 风险
- ✅ 与 GcRootHandle 的 RAII 模式一致
- ✅ 异常安全（栈展开自动 unlock）
- ❌ 需引入 `MutexGuard` 类型（隐藏的临时类型）
- ❌ guard 变量名"无用"（仅用于生命周期）

### 方案 C：lock 块语句（最安全）

```aura
let m = sync.Mutex()
sync thread {
    spawn (io: Io, m: sync.Mutex) {
        lock (m) {
            // 临界区
        }
        // 块结束自动 unlock
    }
}
```

- ✅ 最安全，作用域明确
- ✅ 语法清晰，意图明显
- ❌ 需新增 AST 节点 `LockStmt`
- ❌ 需 Parser/Sema/CodeGen 全链路改动
- ❌ 不能跨函数持有锁（guard 可通过 move 跨函数，块语句不行）

### 方案 D：B + C 双重提供

同时提供 `m.lock()` 返回 guard 和 `lock (m) { }` 语句（语法糖，等价于 `{ let _g = m.lock(); ... }`）。

- ✅ 灵活，覆盖所有场景
- ❌ 实现成本最高

---

## 四、推荐方案：B（RAII Guard）

**理由**：
1. **覆盖主用例**：99% 场景是"进入临界区 → 操作 → 离开"，guard 完美匹配
2. **实现成本低**：仅需运行时实现 + BuiltinRegistry 注册，无需 AST/Parser/Sema 改动
3. **与 Aura 一致**：GcRootHandle 已是 RAII，开发者熟悉该模式
4. **异常安全**：C++ 栈展开保证 unlock
5. **未来可扩展**：若需要 `lock { }` 语句，可作为语法糖后续添加

**方案 C 的优势（跨函数持有锁）在 Aura 中罕见**：spawn 任务是独立函数，锁通常在任务内 acquire/release。

---

## 五、详细设计（方案 B）

### 5.1 类型设计

**`sync.Mutex`**：互斥锁类型（堆对象，GC 管理）
- `Mutex : GcObject`，内部封装 `std::mutex`
- 通过 `sync.Mutex()` 构造（返回 `Mutex*`）
- 方法：`lock()` / `try_lock()` / `unlock()`

**`sync.MutexGuard`**：RAII 守卫（值类型，栈对象）
- `lock()` 返回 `MutexGuard`
- 析构时自动 `unlock()`
- 不可拷贝，可移动

### 5.2 Aura 使用示例

```aura
fun main(io: Io) {
    let counter = [0]           // 共享数据
    let m = sync.Mutex()        // 互斥锁

    sync thread(max = 4) {
        for i in range(100) {
            spawn (io: Io, m: sync.Mutex, counter: [int]) {
                let guard = m.lock()      // 进入临界区
                counter.append(i)
                // guard 析构自动 unlock
            }
        }
    }

    io.println("count: " + counter.len())  // 输出 100
}
```

### 5.3 运行时实现

**新增文件**：`runtime/builtin/mutex.h`

```cpp
#pragma once
#include "../gc.h"
#include <mutex>

namespace aura_rt {

// ============================================================
// Mutex — 互斥锁（GC 堆对象）
//
// 通过 sync.Mutex() 构造，返回 Mutex*。
// lock() 返回 MutexGuard（RAII），析构自动 unlock。
// ============================================================
struct Mutex : GcObject {
    std::mutex m_;

    static const TypeDescriptor _desc;

    MutexGuard lock();
    bool try_lock();
    void unlock();
};

// ============================================================
// MutexGuard — RAII 守卫（栈对象，非 GC）
//
// 由 Mutex::lock() 返回，析构时自动 unlock。
// 不可拷贝，可移动。
// ============================================================
class MutexGuard {
public:
    explicit MutexGuard(Mutex* m) : m_(m) {}
    ~MutexGuard() { if (m_) m_->unlock(); }

    MutexGuard(MutexGuard&& o) noexcept : m_(o.m_) { o.m_ = nullptr; }
    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;
    MutexGuard& operator=(MutexGuard&&) = delete;

private:
    Mutex* m_;
};

} // namespace aura_rt
```

**新增文件**：`runtime/builtin/mutex.cpp`

```cpp
#include "mutex.h"

namespace aura_rt {

const TypeDescriptor Mutex::_desc{
    "Mutex",
    sizeof(Mutex),
    {},  // 无 GC 指针字段（std::mutex 不是 GC 对象）
    &Mutex::registerGcRoots  // 若需要（通常不需要）
};

MutexGuard Mutex::lock() {
    m_.lock();
    return MutexGuard(this);
}

bool Mutex::try_lock() {
    return m_.try_lock();
}

void Mutex::unlock() {
    m_.unlock();
}

} // namespace aura_rt
```

### 5.4 BuiltinRegistry 注册

**修改**：`src/Sema/BuiltinRegistry.h` `init()`

```cpp
// types_ 中新增：
{"Mutex", {"Mutex", true, true, BuiltinPrim::Other, "aura_rt::Mutex*"}},

// methods_ 中新增：
{"Mutex", "lock",     {},  ReturnTypeInfo::Named("MutexGuard")},
{"Mutex", "try_lock", {},  ReturnTypeInfo::Named("bool")},
{"Mutex", "unlock",   {},  ReturnTypeInfo::None()},

// functions_ 中新增（构造函数）：
{"sync.Mutex", {},  ReturnTypeInfo::Named("Mutex")},
// 或更简单：通过命名空间 sync 下的构造函数
```

### 5.5 CodeGen 类型映射

`mapNamedType` 通过 `BuiltinRegistry::findType` 查询，`Mutex` → `aura_rt::Mutex*`（自动处理）。

**`sync.Mutex()` 构造调用**生成：
```cpp
aura_rt::Mutex* _tmp = aura_rt::gc_alloc<aura_rt::Mutex>(&aura_rt::Mutex::_desc);
aura_rt::GcRootHandle<aura_rt::Mutex*> _name(_tmp);
```

**`m.lock()` 调用**生成：
```cpp
auto guard = m->lock();  // MutexGuard
```

### 5.6 与 sync thread 集成

Mutex 作为堆对象，通过 `spawn` 参数值传递 `Mutex*` 指针（与其他 GC 对象一致）。多个任务拿到同一个 `Mutex*`，通过 `m->lock()` 互斥访问共享资源。

**无额外改动**：sync thread 的 change.md 已支持 GC 对象指针传递，Mutex 自动适用。

---

## 六、实施影响评估

### 6.1 改动规模

| 模块 | 文件 | 改动 |
|:---|:---|:---|
| 运行时 | `runtime/builtin/mutex.h`（新增）, `runtime/builtin/mutex.cpp`（新增） | ~80 行 |
| 运行时 | `runtime/aura_rt.h`, `runtime/CMakeLists.txt` | include + 源文件 |
| Sema | `src/Sema/BuiltinRegistry.h` | 类型 + 方法 + 构造函数注册 |
| CodeGen | 无需改动 | 类型映射自动走 BuiltinRegistry |

**总改动**：~120 行（远小于 sync thread 的 ~650 行）

### 6.2 依赖关系

- **依赖**：sync thread 实施完成（需要多线程环境）
- **无依赖**：channel<T>（独立功能，可并行实施）

### 6.3 测试方案

```aura
fun main(io: Io) {
    let counter = [0]
    let m = sync.Mutex()

    sync thread(max = 4) {
        for i in range(1000) {
            spawn (io: Io, m: sync.Mutex, counter: [int]) {
                let guard = m.lock()
                counter.append(i)
            }
        }
    }

    // 验证：无数据竞争，len == 1000
    io.println("count: " + counter.len())
}
```

**验收**：
- 输出 `count: 1000`（无丢失）
- 无 crash、无 ASAN 报错
- 多次运行结果一致

---

## 七、替代方案与未来演进

### 7.1 若用户后续需要 `lock { }` 语句

可作为方案 B 的语法糖，后续添加：
- 新增 `LockStmt` AST 节点
- `lock (m) { body }` → `{ auto _g = m->lock(); body; }`
- 无需改动运行时

### 7.2 若需要读写锁

未来可新增 `sync.RWMutex`：
- `rlock()` 返回 `ReadGuard`
- `wlock()` 返回 `WriteGuard`
- 内部用 `std::shared_mutex`

### 7.3 若需要条件变量

未来可新增 `sync.Cond`：
- `wait(m)` 释放锁并等待
- `notify_one()` / `notify_all()`
- 内部用 `std::condition_variable`

---

## 八、决策建议

**推荐采用方案 B（RAII Guard）**：
1. 实现成本最低（~120 行）
2. 覆盖 sync thread 主用例
3. 与 Aura RAII 哲学一致
4. 未来可平滑扩展到 `lock { }` 语句、RWMutex、Cond

**不推荐方案 A（手动 lock/unlock）**：忘记 unlock 风险高，Aura 无 defer。
**不推荐方案 C（lock 块语句）**：v1 成本过高，收益有限。
**不推荐方案 D（B+C 双重）**：过度设计。

---

## 九、若审核通过

1. 写入 TODO.txt §十 新增 P2 项：
   ```
   [ ] P2  sync.Mutex 用户级互斥锁
         - issue：plan/mutex_issue.md [2026-07-24 审核通过]
         - 方案：B（RAII Guard）
         - 依赖：sync thread 实施完成
         - 文件：runtime/builtin/mutex.h(新增), mutex.cpp(新增),
                 src/Sema/BuiltinRegistry.h
   ```

2. 创建 `plan/mutex_plan.md` 详细实施方案
3. 在 sync thread 实施完成后启动 Mutex plan
