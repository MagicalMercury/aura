#include "mutex.h"

namespace aura_rt {

// 终结器：GC 回收 Mutex 时释放间接持有的 std::mutex
// 安全性：finalizer 在 GC STW 期间执行，所有 mutator 线程已暂停，
//        可安全 delete 同步原语（无并发访问）
static void mutex_finalizer(GcObject* o) {
    auto* m = static_cast<Mutex*>(o);
    delete m->m_;
    m->m_ = nullptr;
}

// TypeDescriptor：m_ 是裸指针指向非 GC 对象，ptrFieldCount=0
// GC 不会追踪该指针（指向独立堆内存，由 finalizer 释放）
const TypeDescriptor Mutex::_desc = {
    sizeof(Mutex),         // size
    0,                      // ptrFieldCount（m_ 不是 GC 指针）
    nullptr,                // ptrFieldOffsets
    0,                      // inlineArrayFieldCount
    nullptr,                // inlineArrayFields
    mutex_finalizer         // finalizer
};

} // namespace aura_rt
