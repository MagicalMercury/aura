#pragma once
// ============================================================
// aura_rt/aura_rt.h ─ Aura 运行时总头文件
// ============================================================

#include "logical_stack.h"   // feature-18 P1：逻辑调用栈（轻量、无重量依赖，故置于最前）
#include "types.h"
#include "gc.h"
#include "task.h"
#include "thread_pool.h"   // sync thread 运行时支持
#include "builtin/string.h"
#include "builtin/path.h"
#include "builtin/math.h"
#include "builtin/io.h"
#include "builtin/array.h"
#include "builtin/sync.h"
#include "builtin/sync_context.h"   // feature-14: runtime sync domain (SyncContext / SpawnTask)
#include "builtin/channel.h"
#include "builtin/mutex.h"
#include "builtin/optional.h"       // Optional<T>
#include "builtin/variant.h"        // Variant<T...>（P1：含堆联合 GC 堆封装）
#include "builtin/callable.h"       // CallableObj / CallableErased / CallArg（feature-06 统一可调用对象）
#include "builtin/iterator.h"       // Iterator<T> / RangeIter / MapIter / FilterIter / FuncIter
#include "builtin/interfaces.h"     // Stringer / Comparable<T>（内置接口值视图；bug-85 B3）

#include "builtin/thread_channel.h"
#include "builtin/tuple.h"          // Tuple2~Tuple8（函数多返回值打包；单元素保持分组无 Tuple1）

#include <functional>
#include <ranges>
