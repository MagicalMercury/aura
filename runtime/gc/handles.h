#pragma once
// ============================================================
// aura_rt/gc/handles.h ─ GC 句柄模板实现
//
// 包含：
//   - GcRootHandle<T> 模板方法实现（统一三模式 Ref/ValueTL/ValueGlobal）
//   - GcWeakHandleBase 内联实现
//
// 本文件由 gc.h 末尾 #include，此时 GcHeap 已完整定义。
// 拆分自原 runtime/gc.h（L471-585）。
// ============================================================

namespace aura_rt {

// ============================================================
// GcRootHandle 模板方法实现（必须在 GcHeap 定义之后）
//
// 模式 A（Ref）：引用外部变量。ptr_ref_ 存储 ptr_ 的值
//   （用户栈上 GC 指针变量的地址），GC 单次解引用 *ptr_ref_
//   即得用户变量值（对象指针）。
// 模式 B/C（Value）：值持有。val_ 存对象指针，ptr_ref_ 指向 &val_，
//   GC compact 直接更新 val_，无悬垂。
// ============================================================
template <typename T>
GcRootHandle<T>::GcRootHandle(T& ref)
    : GcRootHandleBase(), ptr_(&ref), mode_(GcRootMode::Ref) {
    ptr_ref_ = reinterpret_cast<GcObject**>(ptr_);
    GcHeap::instance().registerRootThreadLocal(this);
}

template <typename T>
GcRootHandle<T>::GcRootHandle(T val, GcRootScope scope)
    : GcRootHandleBase(), val_(val),
      mode_(scope == GcRootScope::Global
                ? GcRootMode::ValueGlobal : GcRootMode::ValueThreadLocal) {
    ptr_ref_ = reinterpret_cast<GcObject**>(&val_);   // 根指向内部值
    if (mode_ == GcRootMode::ValueGlobal)
        GcHeap::instance().registerGlobalRoot(ptr_ref_);   // 全局根容器
    else
        GcHeap::instance().registerRootThreadLocal(this);  // 线程局部链表
}

template <typename T>
GcRootHandle<T>::~GcRootHandle() {
    if (mode_ == GcRootMode::ValueGlobal)
        GcHeap::instance().unregisterGlobalRoot(ptr_ref_);
    else
        GcHeap::instance().unregisterRootThreadLocal(this);
}

// 拷贝构造：按 other.mode_ 分支
// - Ref：引用同一外部变量（注册独立线程局部根，指针相同）
//   安全前提：原 GcRootHandle 的生命周期覆盖拷贝的生命周期
//   （sync thread 的 waitGroup 保证 worker 任务完成前主线程栈稳定）
// - Value：深拷贝值 + 独立注册（闭包捕获语义，各副本独立持有根）
template <typename T>
GcRootHandle<T>::GcRootHandle(const GcRootHandle& other)
    : GcRootHandleBase(), mode_(other.mode_) {
    if (other.mode_ == GcRootMode::Ref) {
        ptr_ = other.ptr_;                       // 引用同一外部变量
        ptr_ref_ = reinterpret_cast<GcObject**>(ptr_);
        GcHeap::instance().registerRootThreadLocal(this);
    } else {
        val_ = other.get();                      // 深拷贝值
        ptr_ref_ = reinterpret_cast<GcObject**>(&val_);
        if (other.mode_ == GcRootMode::ValueGlobal)
            GcHeap::instance().registerGlobalRoot(ptr_ref_);
        else
            GcHeap::instance().registerRootThreadLocal(this);
    }
}

template <typename T>
void GcRootHandle<T>::set(T v) {
    if (mode_ == GcRootMode::Ref) {
        *ptr_ = v;                               // 写外部变量
    } else {
        val_ = v;                                // 写内部 val_
    }
}

// GcWeakHandleBase 实现（必须在 GcHeap 定义之后）
inline GcWeakHandleBase::GcWeakHandleBase(GcObject* obj) : ptr_(obj) {
    GcHeap::instance().registerWeak(this);
}
inline GcWeakHandleBase::~GcWeakHandleBase() {
    GcHeap::instance().unregisterWeak(this);
}

} // namespace aura_rt
