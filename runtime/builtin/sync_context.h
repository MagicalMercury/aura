#pragma once
// ============================================================
// aura_rt/builtin/sync_context.h — feature-14：运行时同步域
//
// 语义：spawn 归属「生成时刻最近」的同步域（sync / sync thread 统一）。
//       归属随任务携带（SpawnTask::owner），不查执行线程的 thread_local 栈。
//
// P1 范围（纯新增）：
//   - SyncContext：域对象（tasks 收集器 + activeCoroutines 计数 + wait_all）
//   - SpawnTask：任务条目（body + owner）
//   - syncStack() / g_syncStackPtr / currentSync()：生成线程的「最近域」判定
//     （bug-95：零初始化 TLS 指针 + 惰性创建，见下方 §thread_local 域栈）
//   - SyncContextScope：域栈 push/pop 的 RAII
//   ⚠️ P1 不改动任何既有 runtime 行为（sync.h / task.h / thread_pool.h 原样）。
//      CodeGen 侧的 push/pop 与 spawn 改接属 P2。
// ============================================================

#include "../task.h"
#include "error.h"      // make_runtime_error（requireSyncPanic 抛 Error，用户 2026-09-20 裁定）

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <vector>

namespace aura_rt {

struct SyncContext;   // 前向

// ============================================================
// SpawnTask — 归属本域的 spawn 产物
//
// ⚠️ owner 属于**任务**，不属于 SyncContext：任务在「生成时刻」绑定
//    当时的域指针，跨线程执行 / 嵌套 spawn 时以它为准（不查线程栈）。
//    反例：worker 线程 thread_local 栈为空 → currentSync() == nullptr，
//    若按执行时刻判定会误 panic。
// ============================================================
struct SpawnTask {
    task<void> body;
    SyncContext* owner;    // 生成时绑定的「最近的域」；worker 内嵌套 spawn 继承它

    SpawnTask() noexcept : owner(nullptr) {}
    SpawnTask(task<void> b, SyncContext* o) noexcept
        : body(static_cast<task<void>&&>(b)), owner(o) {}

    // move-only（task<void> 是 move-only，task.h）
    SpawnTask(SpawnTask&&) noexcept = default;
    SpawnTask& operator=(SpawnTask&&) noexcept = default;
    SpawnTask(const SpawnTask&) = delete;
    SpawnTask& operator=(const SpawnTask&) = delete;
};

// ============================================================
// SyncContext — 一个同步域（sync / sync thread 共用）
//
// 生命周期：由**生成线程**在块入口于栈上构造，块出口 wait_all() 后析构。
//   push/pop 与 wait_all 的调用点由 P2 的 CodeGen 生成（P1 只提供设施）。
//
// ⚠️ owner 生命周期不变量：
//    wait_all() 必须等「全部任务 + 全部嵌套任务迁并完成」之后，
//    才允许 pop_back() / 析构该 SyncContext。
//    反例（禁止）：worker 任务未结束、嵌套收集未 join 回写时主线程先 pop
//                  → 嵌套任务回写 owner->tasks 时指针悬垂。
// ============================================================
struct SyncContext {
    // 归属本域的 spawn 任务收集器（替代词法 _tasks 形参）
    //
    // ⚠️ GC（GC-1 复核）：容器 realloc 与 GC **无交互**——task<void> 唯一成员是
    //    handle_（task.h），帧在 C++ 堆而帧内 GcRootHandle 挂在 threadRootLists_
    //    （gc/gc.h），条目地址 = 帧内成员地址，与 task 对象在哪个容器、移到哪
    //    无关；realloc 只做句柄平凡拷贝。→ reserve 仅作可选防御。
    // ⚠️ 但**并发 push 是数据竞争**（非 GC 问题，同样必须防）——P2 的
    //    per-thread 本地收集 → join 合并回写（方案 a）负责；P1 只保证
    //    单线程生成期的收集正确。
    std::vector<SpawnTask> tasks;

    // 活动协程计数（推论 A：块内启动且未结束的协程）
    // ⚠️ GC-3：**detach 的帧不计入**（run_to_completion 异步挂起路径，
    //    task.h:242-257；触发点 src/CodeGen/StmtSpawn.cpp:375）——否则计数
    //    永不归零 → wait_all 死等
    std::atomic<int> activeCoroutines {0};

    SyncContext() = default;
    SyncContext(const SyncContext&) = delete;
    SyncContext& operator=(const SyncContext&) = delete;

    // 收集一个任务（生成线程调用；owner 由本域自身绑定）
    // ⚠️ P2 的 worker 线程内嵌套 spawn **不得**走这里（与主线程并发写同一
    //    vector = 数据竞争），改走 addNested（per-thread 本地收集 + join 合并）。
    void addTask(task<void> body) {
        tasks.push_back(SpawnTask(static_cast<task<void>&&>(body), this));
    }

    // feature-14 P2: factory-form collection (sync(max=N) throttling path only)
    //
    // addTask takes an already-constructed task<void>; this takes a factory
    // std::function<task<void>()> so that a task rejected by the throttle is
    // never constructed at all (no frame, no GC root registration).
    void addTaskFactory(std::function<task<void>()> factory) {
        tasks.push_back(SpawnTask(factory(), this));
    }

    // 预留接口：嵌套任务的跨线程收集（方案 a：per-thread 本地收集 → join 合并）
    //
    // ⚠️ P1 **不实现**完整的跨线程收集机制：它依赖 P2 的 CodeGen 改造才能真正
    //    用起来，且会拖累 P1 的可验证性。此处仅声明入口语义供 P2 接入 ——
    //    worker 线程在任务体内嵌套 spawn 时，把产物暂存到 per-thread 本地队列，
    //    任务结束时 join 合并回 owner->tasks；合并必须发生在 wait_all 返回之前。
    // P1 实现 = 单线程直通（仅为语义占位，不做跨线程收集）。
    // P2 替换为 per-thread 队列 + join 合并回写。
    void addNested(SpawnTask&& t) {
        tasks.push_back(static_cast<SpawnTask&&>(t));
    }

    // 本域是否已无待等：tasks 容器为空 且 活动协程计数为 0
    bool quiescent() const {
        return tasks.empty() && activeCoroutines.load() == 0;
    }

    // 完成等待：等 tasks 全部完成 + activeCoroutines 归零
    //
    // ⚠️ 等待形态**二分**（实测结论，不可合并）：
    //    sync { } / sync(max=N) → **挂起让出**（co_await，跑在主协程上，
    //                             阻塞会卡死事件循环）
    //    sync thread { }        → **阻塞**（跑在独立线程上，阻塞无害）
    //    本函数为**让出语义**（co_await 友好）：sync 系直接 co_await；
    //    sync thread 系在调用点自行阻塞驱动（P2 决定，不在此处实现）。
    //
    // 实现（P1，取出一式，与 task.h when_all 同构）：
    //   - 逐项把 task 从容器**取出**（帧内地址固定），再 co_await；
    //   - 每次循环重读 size（等待期间任务执行流可能追加，P2 场景）；
    //   - 已取走的槽位持有的 handle 由 task 的移动赋值接管，无 done handle 残留。
    // ⚠️ 单线程协作调度前提（无并发 push）——并发安全由 P2 的 per-thread
    //    收集 + join 合并保证（见 addNested）。
    // ============================================================
    // ⚠️ U5 待办（2026-09-20 主 Agent 处置）：组合等待**尚未实现**
    //
    // 目标（change.md §3.3 / 推论 A）：wait_all 应同时满足
    //   ① tasks 容器排空   ② activeCoroutines == 0
    // 现状只满足 ① —— 因为 ② **无法用现有原语表达**：
    //   - 待归零必须**让出**（co_await 循环），自旋会占死单线程事件循环 → 死等；
    //   - 但 runtime 里**没有任何「让出一次」awaiter**：task.h 只提供
    //     task<T>（协程句柄）与 when_all（等一组 task），没有 yield/suspend 一次的包；
    //     EventLoop 有 schedule() 但那是「把句柄入就绪队列」，不是 awaitable。
    //   → 真做 U5 需要**新增一个 yield awaiter**（新机制，属设计层），
    //     不是改一行的事。故此处**暂时保持 P1 语义**，等 U2（activeCoroutines
    //     的写入方）落地 + 让出原语设计定案后一并处理。
    //
    // 附：当前计数**恒为 0**（CodeGen 侧尚无 activeCoroutines++ 生成点），
    //     故即使写了这个循环也是死代码 —— 这也是可以先移除的实证依据。
    // ============================================================
    task<void> wait_all() {
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            task<void> t = static_cast<task<void>&&>(tasks[i].body);
            if (t) co_await t;
        }
        tasks.clear();
        // U5 待办：此处还需 `while (activeCoroutines.load() != 0) co_await <yield>;`
        //           —— <yield> 原语待设计（见上方说明）。
        co_return;
    }
};

// ============================================================
// thread_local 域栈（嵌套 sync / sync thread 支持「最近」语义）
//
// ⚠️ 仅描述**创建 / 生成线程**的域栈；worker 线程栈为空，故归属一律以
//    spawn 生成时刻为准（currentSync() 在生成线程求值后随任务携带）。
//
// 🔴 bug-95（2026-10-01 修复）：本栈**必须**保持「零初始化」形态 ——
//    ⛔ 不得退回 `inline thread_local std::vector<SyncContext*> g_syncStack;`
//    原因：`std::vector` 需要**动态初始化** ⇒ 每个包含本头的 TU 都发射一份
//    `__tls_init`，而 **MinGW emutls 的 `__tls_init` 不做 COMDAT 合并** ⇒
//    多文件真链接必然 `multiple definition of 'TLS init function for
//    aura_rt::g_syncStack'`（Windows 多文件编译**全量不可用**，产品路径亦然）。
//    判据对照（实测）：`logical_stack.h` 的 `g_throwCounts`（零初始化 POD）
//    **不报错** ⇒ 关键是「**是否需要动态初始化**」，不是 `inline` / `thread_local`。
//    ⛔ 也不得改成 `std::unique_ptr<...>`：它有**析构**（非平凡）⇒ 仍会发射
//    `__tls_init` ⇒ 等于没修。
//    完整根因链 / 否决方案见 issues/bugs/bug-95-tls-dynamic-init-gsyncstack-multiple-definition-on-windows.md。
//
// 取舍（已知、一次性、判断为可接受）：裸指针在线程退出时**不析构** ⇒ 泄漏一个
//    `std::vector` 的头部（约 24B 栈对象 + 一次堆分配开销；元素是裸指针，
//    本身**不拥有**域对象）。执行域栈在线程结束时理应已空（ScopePop 成对）⇒
//    泄漏量极小且每线程一次性。若将来该泄漏不可接受，替代方向 = 把容器换成
//    **零初始化 POD 定长栈**（数组 + 计数），而非引入析构。
// ============================================================
inline thread_local std::vector<SyncContext*>* g_syncStackPtr = nullptr;   // 指针 ⇒ 零初始化 ⇒ 不发射 __tls_init

// 惰性创建：首次**取用**才 new（唯一分配入口 = 本函数）
inline std::vector<SyncContext*>& syncStack() {
    if (!g_syncStackPtr) g_syncStackPtr = new std::vector<SyncContext*>();
    return *g_syncStackPtr;
}

inline SyncContext* currentSync() {
    // ⚠️ 只读路径**直接读指针**（不走 syncStack()）：worker 线程等无域线程
    //    会频繁走到这里，不该为「只想知道有没有域」而触发一次分配。
    if (!g_syncStackPtr || g_syncStackPtr->empty()) return nullptr;
    return g_syncStackPtr->back();
}

// feature-14 P2: spawn generation-point domain accessor (called by generated code).
//
// change.md 3.1 requires "no domain -> runtime panic" (Q5). P1 currentSync()
// returns nullptr safely; requireSync() is the single entry point used by
// generated spawn sites. Domain ownership is bound at *generation* time and
// travels with the task (SpawnTask::owner, GC-4), so this only resolves the
// nearest domain on the generating thread.
//
// ⚠️ 用户 2026-09-20 裁定：**抛 Error，不用 std::abort()**。
//    理由：Aura 有 Error 机制（error.h），EventLoop::run（task.cpp:96-111）
//    已有成体系的 Error 处理（catch Error → 打印 kind/message → exit(1)）；
//    std::abort() 会绕过该机制（析构不执行、无结构化错误）。
//    错误 kind = "RuntimeError"（make_runtime_error 预设）。
//    例外：worker 线程等无异常处理上下文时，Error 会向上传播至 workerLoop
//    的 catch（已由 run_to_completion 的异常重抛路径覆盖）。
inline void requireSyncPanic() {
    throw make_runtime_error(
        "[aura_rt] spawn requires a sync context: 'spawn' was reached at runtime "
        "while no 'sync' / 'sync thread' block was active on this thread (the "
        "enclosing function must itself be called from a sync domain).");
}

inline SyncContext* requireSync() {
    SyncContext* ctx = currentSync();
    if (!ctx) requireSyncPanic();
    return ctx;
}

// ============================================================
// SyncContextScope — 域栈 push/pop 的 RAII（P2 生成代码的落点）
//
// 用法（P2 生成）：
//   {
//       aura_rt::SyncContext _ctx;
//       aura_rt::SyncContextScope _scope(_ctx);   // 构造 → push
//       ... spawn 生成点：_ctx.addTask(task);
//       co_await _ctx.wait_all();                 // sync 系：让出等待
//   }                                             // _scope 析构 → pop
//
// ⚠️ pop 由析构触发，必然发生在 wait_all 之后，满足 owner 生命周期不变量。
// ============================================================
class SyncContextScope {
public:
    explicit SyncContextScope(SyncContext& ctx) : ctx_(&ctx) {
        syncStack().push_back(ctx_);
    }
    ~SyncContextScope() {
        // 正常路径：栈顶即本域。异常 / 乱序路径：仍弹出栈顶，避免栈泄漏。
        // ⚠️ 只读 + 收尾路径**直接读指针**（不走 syncStack()）：本线程若从未
        //    建栈（ptr 为空）就直接返回，避免为「无栈可弹」而无谓分配一个 vector。
        if (g_syncStackPtr && !g_syncStackPtr->empty()) g_syncStackPtr->pop_back();
    }
    SyncContextScope(const SyncContextScope&) = delete;
    SyncContextScope& operator=(const SyncContextScope&) = delete;
private:
    SyncContext* ctx_;
};

} // namespace aura_rt
