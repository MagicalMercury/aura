#include "mutex.h"

namespace aura_rt {

// v1.2：Mutex 改为 Inner* inner_，终结器释放 Inner（含 owner 字段）
// 安全性：finalizer 在 GC STW 期间执行，所有 mutator 线程已暂停，
//        可安全 delete 同步原语（无并发访问）
static void mutex_finalizer(GcObject* o) {
    auto* m = static_cast<Mutex*>(o);
    delete m->inner_;
    m->inner_ = nullptr;
}

// TypeDescriptor：inner_ 是裸指针指向非 GC 对象，ptrFieldCount=0
// GC 不会追踪该指针（指向独立堆内存，由 finalizer 释放）
const TypeDescriptor Mutex::_desc = {
    sizeof(Mutex),         // size
    0,                      // ptrFieldCount（inner_ 不是 GC 指针）
    nullptr,                // ptrFieldOffsets
    0,                      // inlineArrayFieldCount
    nullptr,                // inlineArrayFields
    mutex_finalizer         // finalizer
};

// v1.2：RWMutex Inner 新增 waiting_readers/generation/writer_owner 字段
// 终结器逻辑不变（delete inner_ 释放整个 Inner 结构）
static void rwmutex_finalizer(GcObject* o) {
    auto* rw = static_cast<RWMutex*>(o);
    delete rw->inner_;
    rw->inner_ = nullptr;
}

const TypeDescriptor RWMutex::_desc = {
    sizeof(RWMutex),        // size
    0,                      // ptrFieldCount（inner_ 不是 GC 指针）
    nullptr,                // ptrFieldOffsets
    0,                      // inlineArrayFieldCount
    nullptr,                // inlineArrayFields
    rwmutex_finalizer       // finalizer
};

// 终结器：GC 回收 Once 时释放间接持有的 m_ 和 done_
static void once_finalizer(GcObject* o) {
    auto* once = static_cast<Once*>(o);
    delete once->m_;
    delete once->done_;
    once->m_ = nullptr;
    once->done_ = nullptr;
}

const TypeDescriptor Once::_desc = {
    sizeof(Once),           // size
    0,                      // ptrFieldCount（m_ 和 done_ 都不是 GC 指针）
    nullptr,                // ptrFieldOffsets
    0,                      // inlineArrayFieldCount
    nullptr,                // inlineArrayFields
    once_finalizer          // finalizer
};

} // namespace aura_rt
