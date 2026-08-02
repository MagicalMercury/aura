#pragma once
// ============================================================
// aura_rt/gc/handles.h ─ GC 句柄模板实现
//
// 包含：
//   - GcRootHandle<T> 模板方法实现
//   - GcWeakHandleBase 内联实现
//   - GcGlobalRoot<T> 模板方法实现
//   - GcSharedRoot<T> 模板类（闭包捕获用）
//
// 本文件由 gc.h 末尾 #include，此时 GcHeap 已完整定义。
// 拆分自原 runtime/gc.h（L471-585）。
// ============================================================

namespace aura_rt {

// ============================================================
// GcRootHandle 模板方法实现（必须在 GcHeap 定义之后）
// ============================================================
template <typename T>
GcRootHandle<T>::GcRootHandle(T& ref) : GcRootHandleBase(), ptr_(&ref) {
    // ptr_ref_ 存储 ptr_ 的值（用户栈上 GC 指针变量的地址）
    // GC 单次解引用 *ptr_ref_ 即得用户变量值（对象指针）
    ptr_ref_ = reinterpret_cast<GcObject**>(ptr_);
    GcHeap::instance().registerRootThreadLocal(this);
}

template <typename T>
GcRootHandle<T>::~GcRootHandle() {
    if (ptr_) GcHeap::instance().unregisterRootThreadLocal(this);
}

// 拷贝构造：新 GcRootHandle 注册独立 GC 根，ptr_ 指向同一栈地址
// 安全前提：原 GcRootHandle 的生命周期覆盖拷贝的生命周期
// （sync thread 的 waitGroup 保证 worker 任务完成前主线程栈稳定）
template <typename T>
GcRootHandle<T>::GcRootHandle(const GcRootHandle& other) : GcRootHandleBase(), ptr_(other.ptr_) {
    ptr_ref_ = reinterpret_cast<GcObject**>(ptr_);
    GcHeap::instance().registerRootThreadLocal(this);
}

// GcWeakHandleBase 实现（必须在 GcHeap 定义之后）
inline GcWeakHandleBase::GcWeakHandleBase(GcObject* obj) : ptr_(obj) {
    GcHeap::instance().registerWeak(this);
}
inline GcWeakHandleBase::~GcWeakHandleBase() {
    GcHeap::instance().unregisterWeak(this);
}

// GcGlobalRoot 模板方法实现（必须在 GcHeap 定义之后）
template <typename T>
GcGlobalRoot<T>::GcGlobalRoot(T* obj) : ptr_(obj) {
    GcHeap::instance().registerGlobalRoot(
        reinterpret_cast<GcObject**>(&ptr_));
}

template <typename T>
GcGlobalRoot<T>::~GcGlobalRoot() {
    GcHeap::instance().unregisterGlobalRoot(
        reinterpret_cast<GcObject**>(&ptr_));
}

// ============================================================
// GcSharedRoot<T> — 闭包捕获 GC 根的共享所有权版本
//
// 与 GcRootHandle<T> 互补：
// - GcRootHandle：栈上包装，不可拷贝，ptr_ 指向栈变量
// - GcSharedRoot：堆上独立存值，可拷贝，专为闭包捕获设计
//
// 使用场景：闭包 lambda 按值捕获 GC 指针类型变量时，
// 用 GcSharedRoot 包装，每个 lambda 副本独立持有 GC 根。
// CodeGen 使用 C++14 init-capture 生成：
//   [name = aura_rt::GcSharedRoot<T>(name.get())]
// ============================================================
template <typename T>
class GcSharedRoot {
public:
    explicit GcSharedRoot(T val) : ptr_(new T(val)) {
        GcHeap::instance().registerGlobalRoot(reinterpret_cast<GcObject**>(ptr_));
    }

    ~GcSharedRoot() {
        if (ptr_) {
            GcHeap::instance().unregisterGlobalRoot(reinterpret_cast<GcObject**>(ptr_));
            delete ptr_;
            ptr_ = nullptr;
        }
    }

    // 拷贝构造：新对象独立堆分配 + 独立 register
    GcSharedRoot(const GcSharedRoot& other) : ptr_(new T(*other.ptr_)) {
        GcHeap::instance().registerGlobalRoot(reinterpret_cast<GcObject**>(ptr_));
    }

    // 拷贝赋值：先 unregister 旧值，再分配新值
    GcSharedRoot& operator=(const GcSharedRoot& other) {
        if (this != &other) {
            GcHeap::instance().unregisterGlobalRoot(reinterpret_cast<GcObject**>(ptr_));
            delete ptr_;
            ptr_ = new T(*other.ptr_);
            GcHeap::instance().registerGlobalRoot(reinterpret_cast<GcObject**>(ptr_));
        }
        return *this;
    }

    // 移动构造
    GcSharedRoot(GcSharedRoot&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    GcSharedRoot& operator=(GcSharedRoot&& other) noexcept {
        if (this != &other) {
            if (ptr_) {
                GcHeap::instance().unregisterGlobalRoot(reinterpret_cast<GcObject**>(ptr_));
                delete ptr_;
            }
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    // 读取值（与 GcRootHandle::get() 兼容，返回类型相同）
    T  get() const { return *ptr_; }
    T& get()       { return *ptr_; }

    void set(T val) { *ptr_ = val; }

private:
    T* ptr_;  // 堆上持有值，独立于栈帧生命周期
};

} // namespace aura_rt
