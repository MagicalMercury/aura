// ============================================================
// aura_rt/gc/gc.cpp ─ GC 基础设施
//
// 内容：thread_local TLAB 定义、GcHeap 单例、GcCompactSuspendGuard、
//       noteCoroutineFrameImpl、~GcHeap。
// 拆分自原 runtime/gc.cpp（L1-59）。
// ============================================================

#include "gc.h"
#include "gc_log_writer.h"  // gcLogStart/gcLogShutdown（异步日志 Logger 生命周期）
#include "gc_interrupt.h"   // gcInstallSafepointSignalHandler（P2：Linux SIGURG）
#include <algorithm>  // std::min（parallelFor 分片）
#include <cstdlib>   // getenv（AURA_GC_LOG 解析）
#include <string>    // std::string
#include <cstdio>    // std::fprintf（未知标签 warning）
#include <chrono>    // steady_clock（进程启动基准时刻）


namespace aura_rt {

// thread_local TLAB 指针定义（每线程独立，初始 nullptr）
thread_local GcHeap::Tlab* GcHeap::tlab_ = nullptr;

// ============================================================
// GcCompactSuspendGuard — 实现
// ============================================================
GcCompactSuspendGuard::GcCompactSuspendGuard() {
    GcHeap::instance().incCompactSuspend();
}
GcCompactSuspendGuard::~GcCompactSuspendGuard() {
    GcHeap::instance().decCompactSuspend();
}

// ============================================================
// 空闲线程唤醒广播（阶段 2.1：GC 停靠前唤醒空闲 worker）
// ============================================================
void GcHeap::registerIdleWakeup(std::function<void()> cb) {
    std::lock_guard<std::mutex> lk(idleWakeupsM_);
    idleWakeups_.push_back(std::move(cb));
}

void GcHeap::notifyIdleWakeups() {
    std::lock_guard<std::mutex> lk(idleWakeupsM_);
    for (auto& cb : idleWakeups_) cb();   // 回调仅 notify_all（不持 ThreadPool 锁），无锁序
}

// ============================================================
// GC 内部并行辅助（并行 sweep / 引用更新 / compact 搬运分片）
// 复用 P1 runMarkPhase 的线程模式：min(hardware_concurrency, 4) +
// 每轮创建/join；total < threshold 或单核退化为串行（零线程开销）。
// ============================================================
void GcHeap::parallelFor(size_t total, size_t threshold,
                         const std::function<void(size_t begin, size_t end)>& fn) {
    if (total < threshold) { fn(0, total); return; }
    size_t n = std::min<size_t>(std::thread::hardware_concurrency(), 4);
    if (n < 2) { fn(0, total); return; }
    size_t per = (total + n - 1) / n;
    std::vector<std::thread> threads;
    threads.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        size_t begin = i * per;
        size_t end = std::min(begin + per, total);
        if (begin >= end) break;
        // GC 内部线程：in_gc_internal_（thread_local）防 dynamicDesc 钩子
        // 意外 safepoint 递归；局部临时内存用 C++ 堆（绝不走 GC 堆）
        threads.emplace_back([this, begin, end, &fn] {
            in_gc_internal_ = true;
            fn(begin, end);
            in_gc_internal_ = false;
        });
    }
    for (auto& t : threads) t.join();
}

// ============================================================
// GcHeap 构造 / GC 事件日志 env 解析（P3）
// ============================================================
namespace {
    [[gnu::init_priority(101)]] GcHeap g_gcHeap;
}

// 去除首尾空白（env 值可能带空格，如 cmd 的 set VAR=value 尾随空格）
static std::string trimWs(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static uint8_t parseGcLogLevel(const std::string& s) {
    if (s == "trace")  return static_cast<uint8_t>(GcHeap::GcLogLevel::Trace);
    if (s == "debug")  return static_cast<uint8_t>(GcHeap::GcLogLevel::Debug);
    if (s == "info")   return static_cast<uint8_t>(GcHeap::GcLogLevel::Info);
    if (s == "warn" || s == "warning") return static_cast<uint8_t>(GcHeap::GcLogLevel::Warning);
    if (s == "error")  return static_cast<uint8_t>(GcHeap::GcLogLevel::Error);
    if (s == "off")    return static_cast<uint8_t>(GcHeap::GcLogLevel::Off);
    return static_cast<uint8_t>(GcHeap::GcLogLevel::Info);  // 未知级别默认 info
}

void GcHeap::parseGcLogEnv() {
    const char* e = std::getenv("AURA_GC_LOG");
    if (!e || !*e) return;  // 未设置 → 全 Off（零开销）
    std::string s(e);
    size_t pos = 0;
    for (;;) {
        size_t comma = s.find(',', pos);
        std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t eq = item.find('=');
        std::string tag = trimWs(eq == std::string::npos ? item : item.substr(0, eq));
        std::string lvl = trimWs(eq == std::string::npos ? "info" : item.substr(eq + 1));
        uint8_t lv = parseGcLogLevel(lvl);
        bool all = !tag.empty() && tag.back() == '*';  // gc* 通配子树
        if (all) tag.pop_back();
        if (tag == "gc") {
            gcLogLevels_[kTagGc] = lv;
            if (all) { gcLogLevels_[kTagPhase] = lv; gcLogLevels_[kTagMemory] = lv; gcLogLevels_[kTagTrigger] = lv; }
        } else if (tag == "gc/phase")   gcLogLevels_[kTagPhase]   = lv;
        else if (tag == "gc/memory")    gcLogLevels_[kTagMemory]  = lv;
        else if (tag == "gc/trigger")   gcLogLevels_[kTagTrigger] = lv;
        else std::fprintf(stderr, "[GC] warning: unknown AURA_GC_LOG tag '%s'\n", tag.c_str());
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    // 子标签已设置但 gc 未设置时，gc 默认 info（对标 Java -Xlog 语义）
    if (gcLogLevels_[kTagGc] == 0 &&
        (gcLogLevels_[kTagPhase] || gcLogLevels_[kTagMemory] || gcLogLevels_[kTagTrigger]))
        gcLogLevels_[kTagGc] = static_cast<uint8_t>(GcLogLevel::Info);
}

GcHeap::GcHeap() {
    parseGcLogEnv();
#ifndef _WIN32
    // P2：进程级安装 SIGURG handler（空操作；GcHeap 全局构造于 main 前，sigaction 可用）
    gcInstallSafepointSignalHandler();
#endif
    // 进程启动基准（全局构造在 main 前，接近进程启动）
    gcStartBaseMicros_ = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // 任一日志标签启用时启动异步 Logger 线程（gcLogStart 幂等）
    for (int i = 0; i < kTagCount; ++i) {
        if (gcLogLevels_[i] > 0) { gcLogStart(); break; }
    }
}

GcHeap& GcHeap::instance() {
    return g_gcHeap;
}

// task<T>::promise_type::operator new/delete 调用
// 实现在 gc.cpp，避免 task.h → gc.h 循环依赖
void noteCoroutineFrameImpl(void* framePtr, std::size_t size) {
    GcHeap::instance().noteCoroutineFrame(framePtr, size);
}

GcHeap::~GcHeap() {
    // 关闭异步 Logger + drain 拗余日志（幂等；未启动时零开销）
    gcLogShutdown();

    // 进程退出时不主动释放 GC 页。
    // 原因：静态析构顺序不确定，其他对象（协程帧 / std::vector 等）
    // 可能仍在引用 GC 页中的内存，freeAllPages() 会导致 use-after-free。
    // 让操作系统在进程退出时统一回收所有内存。
}

} // namespace aura_rt
