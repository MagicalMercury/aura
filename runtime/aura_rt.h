#pragma once
// ============================================================
// aura_rt/aura_rt.h ─ Aura 运行时总头文件
// ============================================================

#include "types.h"
#include "gc.h"
#include "task.h"
#include "thread_pool.h"   // sync thread 运行时支持
#include "builtin/string.h"
#include "builtin/path.h"
#include "builtin/io.h"
#include "builtin/array.h"
#include "builtin/sync.h"
#include "builtin/channel.h"
#include "builtin/mutex.h"
#include "builtin/optional.h"       // Optional<T>
#include "builtin/variant.h"        // Variant<T...>（P1：含堆联合 GC 堆封装）
#include "builtin/iterator.h"       // Iterator<T> / RangeIter / MapIter / FilterIter / FuncIter
#include "builtin/thread_channel.h"

#include <functional>
#include <ranges>
#include <variant>
