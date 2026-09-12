---
type: bug_report
module: CodeGen
sub_module: genConstructor（DeclFun.cpp）/ genFunExpr（ExprClosure.cpp）捕获分析
status:
  - fixed
severity:
  - high
discover_date: 2026-09-01
related_issues:
  - "[[bug-24-closure-this-not-captured]]"
tags:
  - closure
  - constructor
  - gc-safety
  - dangling
  - bad-cpp-runtime
---

# 【构造闭包 GC 悬垂】构造函数体内闭包捕获 self（裸指针）存字段，GC compact 移动 record 后调用 → SIGSEGV

[x] **主标题：genConstructor 不设 currentReceiverName_，构造闭包把 self（ctor 局部 `X*` 裸指针）当普通变量 `[self]` 捕获存入 record 字段（std::function 堆内 GC 不可见）→ gc_force() compact 移动 record → 闭包内 self 指向旧地址 → 0xC0000005 崩溃**

> **一句话摘要**：构造函数体内创建闭包引用 self 标识符（如 `self.add = fun(x:int)->int { return x + self.inc }`）时，genConstructor（DeclFun.cpp:637-684）**不设置 currentReceiverName_**，`self` 在 ctor 中是局部裸指针变量 `X* self = gc_alloc<X>(...)` → 捕获分析（ExprClosure.cpp:464-478）把 self 当普通变量捕获 `[self]` → 闭包存入 record 字段 add（C++ std::function，位于 GC 堆对象内部，GC 类型描述符不扫描其内部指针）→ 后续 `gc_force()` major compact 移动 record 对象 → 闭包内捕获的 self 仍指旧地址（无 GcRootHandle 保护、无保守扫描覆盖堆内 std::function）→ 调用 `c.add(5)` 访问旧地址 → **0xC0000005 访问违例（8/8 次稳定复现）**。与 bug-24 同族但独立：bug-24 是「方法内 self 映射 this 未捕获 → 编译错误」，本缺陷是「构造内 self 按普通变量捕获 → 编译通过但运行时 GC 悬垂」。

## 1. 调研背景与发现
- **发现时间**：2026-09-01（修 bug-24 时新增 repro_ctor_closure_self 验证构造形态，追加 gc_force 探测时发现）。
- **触发场景**：构造闭包捕获 self 并**逃逸到 GC 堆**（存 record 字段）后，GC major compact 移动 record。
- **影响范围**：凡「构造函数体内闭包引用 self 且闭包逃逸（存字段/返回）」形态在 GC compact 后调用均悬垂；若闭包在 ctor 内即时调用（不逃逸、无跨 GC）则侥幸通过。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：genConstructor（DeclFun.cpp:676-681）生成 `X* self = aura_rt::gc_alloc<X>(&X::_desc);` + body——`self` 是 ctor **局部裸指针**，与「方法 receiver = this」语义不同（ctor 是静态工厂函数 `X* X_ctor(...)`，无 this），故不设 currentReceiverName_（设了会把 self 误映射 this 而坏 C++）；→ 构造闭包捕获分析（ExprClosure.cpp:464-478）对 self 走普通变量路径 push 进 captures → 生成 `[self](int32_t x) -> int32_t { return x + self->inc; }`（编译通过）；→ `self.add = <闭包>` 把闭包（含捕获的 self 副本）写入 GC 堆对象字段 add（std::function）；→ GC compact（relocateGlobalRootPtrs 只重写 GcRootHandle 与对象内显式 ptr_ref_ 槽位）移动 record → 字段 add 内 std::function 捕获的 self **是裸指针，不被 GC 重写**（堆内对象按 TypeDescriptor 扫描，std::function 内部结构对 GC 不可见）→ 旧地址访问 → SIGSEGV。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及（编译通过，运行时才崩）。
- **CodeGen 相关路径**：
  - `src\CodeGen\DeclFun.cpp:637-684`（genConstructor：生成 `X* self = gc_alloc<X>(...)` 局部变量，**不设 currentReceiverName_**）/ `:677`（self 局部声明）。
  - `src\CodeGen\ExprClosure.cpp:464-478`（捕获过滤：ctor 上下文 self 非 declared/param/builtin/registeredType → push 进 captures）/ `:479-492`（捕获列表生成普通 `[self]`）。
  - `src\CodeGen\CodeGen.h`（gcRootVarNames_/gcRootTypes_ 只覆盖 let 变量，self 不在其中 → 不走 GcRootHandle init-capture 分支 L604-609）。

### 2.2 关键逻辑细节
- **与 bug-24 的差异**：bug-24（方法）self 映射 this → 编译错误；本缺陷（构造）self 是局部变量 → 编译通过但**跨 GC 悬垂**——比编译错误更隐蔽（静默 + 运行时崩溃）。
- **为何不设 currentReceiverName_**：genConstructor 生成的是静态工厂函数（`X* X_ctor(...)`），无 `this`；若设 currentReceiverName_ = "self"，genIdentifier（ExprGen.cpp:151）会把 ctor 内 self 映射为 `this` → `this->inc` → `this` 未声明坏 C++。故 ctor 保持不设（正确）。
- **GcRootHandle 先例**：ExprClosure.cpp:604-609 已有对 gcRootVarNames_ 命中的捕获变量生成 `[name = GcRootHandle<Type>(name.get(), Global)]` init-capture 的机制——本缺陷可复用该机制（把 ctor receiver self 也路由进 GcRootHandle init-capture），但需新增 ctor receiver 类型跟踪（仿 bug-24 的 currentReceiverCppType_）。

## 3. 影响范围（Scope）
- **结论**：所有「构造闭包捕获 self + 逃逸存字段/返回 + 后续 GC compact」形态均悬垂。
- **不受影响路径**：ctor 闭包即时调用（不逃逸、创建与调用间无 GC）；方法内闭包（bug-24 已修，GcRootHandle init-capture）；ctor 闭包不引用 self（只引用普通参数/局部）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `probe_ctor_closure_self_gcforce.aura` | 构造闭包捕获 self 存字段 add，main 中 gc_force() 后调 c.add(5) | 输出 15 | ❌ **0xC0000005 SIGSEGV（8/8 稳定复现）** | 本条目 |
| `repro_ctor_closure_self.aura` | 同形态**无 gc_force**（ctor→调用间无 GC） | 输出 15 | ✅ 输出 15 | 对照：无 GC 时侥幸通过 |

## 5. 修复方案（Fix Plan，批次 9 最终方案）
> 详细方案见 `change.md`（批次 9 §3）。review-batch9 裁决：本方案机制推演成立（✅ 全部核实），可直接实施。**方案优于笔记原方案**（原 currentCtorReceiverName_ 新字段方案 → 改 gcRootVarNames_ 注册，零新增机制）。

- **修复位置**：`src\CodeGen\DeclFun.cpp`（genConstructor，单文件）。
- **修复逻辑（设计优于笔记原方案）**：不改捕获分析、不加新字段——**把 self 注册进 gcRootVarNames_/gcRootTypes_**，闭包捕获（ExprClosure L639-644 init-capture `GcRootHandle<type>(self.get(), Global)` 分支）、体内引用（genIdentifier `.get()`）、嵌套闭包传播全部自动走既有机制：
  ```cpp
  // genConstructor：self 改 _raw + GcRootHandle + 注册
  std::string recvName = safeName(decl.receiverName);
  out << "  " << fullType << "* " << recvName << "_raw = aura_rt::gc_alloc<"
      << fullType << ">(&" << fullType << "::_desc);\n";
  out << "  aura_rt::GcRootHandle<" << fullType << "*> " << recvName
      << "(" << recvName << "_raw);\n";
  gcRootVarNames_.insert(recvName);
  gcRootTypes_[recvName] = fullType + "*";   // 闭包作用域可见（decltype(self_raw) 不可用于 init-capture）
  if (decl.body) genBlock(out, *decl.body, false);
  out << "  return " << recvName << ".get();\n";
  ```
- **关键点**：
  - gcRootTypes_["self"] 必须用 `fullType + "*"`（如 "Counter*"/"Box<T>*"）——闭包 init-capture 的 `GcRootHandle<type>` 在闭包作用域展开，`decltype(self_raw)` 在彼处不可见（self_raw 是 ctor 局部）。
  - **不设 currentReceiverName_**（ctor 是静态工厂，无 this；设了会把 self 误映射 this 坏 C++——预存在设计，保持）。
  - ctor 体内字段赋值（`self.f = v`）经 `self.get()`——**连带修复 ctor 体自身 GC 安全**（字段初始化 alloc 触发 GC 时 self 悬垂的同族缺口）。
  - 尾部既有 `clearVarTrackingState()`（L684）清理 gcRootVarNames_ ✓，多 ctor 顺序生成各自清理。
- **与 bug-56 互斥**：ctor 不设 currentReceiverName_（needsThisCapture 不触发）→ 走 gcRootVarNames_ 路径；方法设 → 走 needsThisCapture 路径；两机制无交叠。
- **生成效果**（probe_ctor_closure_self_gcforce 主形态）：
  ```cpp
  Counter* Counter_ctor() {
    Counter* self_raw = aura_rt::gc_alloc<Counter>(&Counter::_desc);
    aura_rt::GcRootHandle<Counter*> self(self_raw);
    self.get()->add = [self = aura_rt::GcRootHandle<Counter*>(self.get(),
                             aura_rt::GcRootScope::Global)](int32_t x) -> int32_t {
        return (x + self.get()->inc);       // compact 后取最新地址
    };
    return self.get();
  }
  ```

## 6. 回归验证清单（Regression Checklist）
- [x] `probe_ctor_closure_self_gcforce.aura` 修复后输出 15（多轮运行无 SIGSEGV）
- [x] `repro_ctor_closure_self.aura` 保持 ✅
- [x] `used/1-6.aura` 全量回归
- [x] 全量测试 0 failed（1206/1206；既有 1 个 pre-existing Examples.TestGcMutex 路径错位不在本次回归面内）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\generic_method_closure_this\`
- **留存产物**：`probe_ctor_closure_self_gcforce.aura` + `.gen.cpp` + `.gen.exe`（8/8 SIGSEGV）；`repro_ctor_closure_self.aura`（对照，输出 15）

## 8. 修复记录

**修复批次**：批次 9（2026-09-02 实施 + 验证）。

**修复要点**：`DeclFun.cpp` genConstructor 单文件——self 改 `_raw` 局部 + `GcRootHandle<fullType*>` 包装 + **gcRootVarNames_/gcRootTypes_ 注册**（gcRootTypes_ 用 `fullType + "*"`，闭包作用域可见）；闭包捕获（ExprClosure gcRootVarNames_ 分支 init-capture `GcRootHandle<fullType*>(self.get(), Global)`）、体内引用（genIdentifier `.get()`）、嵌套闭包传播全部走既有机制，零新增机制；尾部既有 `clearVarTrackingState()` 清理。不设 currentReceiverName_（ctor 是静态工厂，无 this——预存在设计保持）。连带修复 ctor 体自身字段赋值跨 GC 悬垂（写屏障经 `self.get()`）。

**验证统计**：
- 复现矩阵（修复前 → 修复后）：`probe_ctor_closure_self_gcforce`（15）❌ 8/8 SIGSEGV → ✅ 15（15/15 多轮无 SIGSEGV）；`repro_ctor_closure_self`（对照，无 gc_force）✅ 15 → ✅ 15（5/5 不变）。
- 生成代码实证：`Counter* self_raw = gc_alloc<Counter>(&Counter::_desc);` + `GcRootHandle<Counter*> self(self_raw);` + 闭包 init-capture `self = GcRootHandle<Counter*>(self.get(), Global)` + `return self.get();`。
- 全量单测：**1206 tests / 1206 passed / 0 failed**（新增 Batch52 ctor self 断言用例 + 同步更新 3 个既有 ctor/method 写屏障断言为 `self.get()`/`_this.get()`——预期行为变化）。
- 红线：`example/used/1-6.aura` ALL TESTS PASSED、`example/test.aura` ALL TESTS PASSED。

**当前状态**：`2026-09-01` 调研完成；`2026-09-02` 批次 9 修复完成并验证（[x]）。

> **机制性消灭（feature-06，2026-09-09）**：非泛型非协程闭包统一为 GC 堆 `CallableObj`（捕获槽 desc 追踪）后，闭包捕获/自引用的手工 GcRootHandle 包根被类型驱动 GC 保护取代，本缺陷根因（ctor/method 闭包 self 悬垂）机制性消除；既有修复与泛型/协程/ViewRoot 旧路径兜底保留，fixed 状态不变（详见 issues/features/feature-06）。
