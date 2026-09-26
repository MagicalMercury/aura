#pragma once

// ============================================================

// aura_rt/builtin/interfaces.h — 内置接口的 C++ 形态（值视图）

//

// 与 builtin/iterator.h 的 Iterator<T> 同属一类：内置接口（builtins/interfaces.aurai

// 的 Stringer / Comparable / Iterator）的 C++ 视图定义放在 runtime 公共头，

// 不再由 CodeGen 逐模块生成产物（bug-85：同 module 多文件共享产物 namespace 时，

// 逐模块生成的内置接口视图会在共享 namespace 内重定义）。

//

//   - Stringer       —— str(obj) 的字符串化视图（to_string 单方法）

//   - Comparable<T>  —— 三路比较视图（cmp 纯虚 + equal/ne/less/greater/le/ge 默认方法体）

//   - Iterator<T>    —— 见 builtin/iterator.h

//

// 布局约定（B+W 方案：值视图，非基类、无虚函数）：
//   - 视图 = { 静态分派函数指针, self（实现对象起始 GcObject*）}
//   - self 恒为对象起始，经字段偏移/根注册，compact 自动更新
//   - 栈上视图变量用 ViewRoot 包裹（GcRootHandle<GcObject*> 持 self）
//   - 默认方法体内对视图自身再取一份副本 + ViewRoot：与 CodeGen 逐模块生成时
//     的产物逐字同构（见 interfaces.aurai 默认方法体经 genBlock 的翻译结果）
//
// 说明：interfaces.aurai 中的声明仅供 Sema（方法签名）；record impl 适配器
// 走 CodeGen 的 genIfaceAdapter，适配器基类名为 aura_rt::Stringer /
// aura_rt::Comparable<T>（与 builtin/iterator.h 的 aura_rt::Iterator<T> 一致）。
// ============================================================

#include "../types.h"
#include "../gc/gc.h"
#include "callable.h"
#include "iterator.h"     // ViewRoot / GcRootHandle 用法同源

namespace aura_rt {

// ============================================================

// Stringer — 内置接口 Stringer 的值视图

//
// 16B：{ to_stringFn 静态分派函数指针, self 实现对象起始 }

// ============================================================

struct Stringer {
  GcString* (*to_stringFn)(GcObject* self) = nullptr;
  GcObject* self = nullptr;

  GcString* to_string() { return to_stringFn(self); }
};

// Stringer 的闭包适配器（CallableObj 派生对象直接作视图 self）

struct StringerFunc final : CallableObj<GcString*> {
  static GcString* to_stringFn(GcObject* self) {
    auto* __c = static_cast<CallableObj<GcString*>*>(self);
    return __c->invoke(__c);
  }
  static Stringer view(CallableObj<GcString*>* o) {
    return { &StringerFunc::to_stringFn, o };
  }
};

// ============================================================

// Comparable<T> — 内置接口 Comparable<T> 的值视图（模板）

//
// 16B：{ cmpFn 静态分派函数指针, self 实现对象起始 }

// cmp 为纯虚（实现者提供）；equal/ne/less/greater/le/ge 为默认方法体

// （由 builtins/interfaces.aurai 的默认方法经 genBlock 翻译而来）。
// ============================================================

template <typename T>
struct Comparable {
  int32_t (*cmpFn)(GcObject* self, T) = nullptr;
  GcObject* self = nullptr;

  int32_t cmp(T other) { return cmpFn(self, other); }

  bool equal(T other) {
  Comparable self_view_equal = *this;
  ViewRoot<Comparable> _self_root(self_view_equal);
return (_self_root.get().cmp(other) == 0);
  }
  bool ne(T other) {
  Comparable self_view_ne = *this;
  ViewRoot<Comparable> _self_root(self_view_ne);
return !(_self_root.get().equal(other));
  }
  bool less(T other) {
  Comparable self_view_less = *this;
  ViewRoot<Comparable> _self_root(self_view_less);
return (_self_root.get().cmp(other) < 0);
  }
  bool greater(T other) {
  Comparable self_view_greater = *this;
  ViewRoot<Comparable> _self_root(self_view_greater);
return (_self_root.get().cmp(other) > 0);
  }
  bool le(T other) {
  Comparable self_view_le = *this;
  ViewRoot<Comparable> _self_root(self_view_le);
return !(_self_root.get().greater(other));
  }
  bool ge(T other) {
  Comparable self_view_ge = *this;
  ViewRoot<Comparable> _self_root(self_view_ge);
return !(_self_root.get().less(other));
  }
};

}  // namespace aura_rt
