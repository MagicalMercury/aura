# Change Plan — complex_closure GC 安全审查 + 接口适配器生命周期修复

> 日期：2026-07-23
> 状态：**✅ 已修复并验证通过**
> 触发：`example/test.aura`（complex_closure 用例）编译通过但运行期 SIGSEGV

---

## 一、任务背景

用户要求：「尝试修复这个小 bug。以及全面审查此次生成的代码中是否可能出现因为中途移动 GC 导致指针失效」。

输入：
- `example/test.aura`：complex_closure 测试用例（管道、重试、接口、Tree 递归、计数器、条件组合）
- `example/test.cpp`：aurac 生成的 C++ 代码

完整审查报告见 `plan/codegen_gc_safety_audit.md`，本节聚焦当前**未解决**的运行期崩溃。

---

## 二、已修复的 Bug（确认有效）

### Bug A — 接口/抽象类生成 `auto` 值拷贝（编译期失败）✅

- **位置**：`src/CodeGen/ExprGen.cpp` [line 12-30](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L12-30)、[line 62-78](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L62-78)
- **根因**：`genGcRootedArgs` 对所有参数统一生成 `auto _a = (expr);` 值拷贝；对抽象类 `StringProcessor` 触发编译错误
- **修复**：
  1. `isHeapSemType` 显式排除 `InterfaceSemType` 和 `FuncSemType`（否则两者 fall through 到 `return true`）
  2. 非堆类型用 `const auto&` 引用绑定，避免抽象类拷贝

### Bug B — 用户函数堆类型参数未保护（运行期 SIGSEGV）✅

- **位置**：`src/CodeGen/DeclGen.cpp` [line 243-256](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L243-256)、[line 273-285](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L273-285)、[line 344-352](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L344-352)
- **根因**：`map_tree(f, node)` 中 `node` 是裸指针，`Array::make(0)` 触发 GC compact 移动 node 后悬垂
- **修复**：
  1. `funSignature` 中堆类型参数加 `_raw` 后缀（如 `Tree<T>* node_raw`）
  2. 函数体入口生成 `GcRootHandle<decltype(node_raw)> node(node_raw);`
  3. 参数名加入 `gcRootVarNames_`，`genIdentifier` 生成 `.get()`
  4. `gcRootTypes_` 存 `decltype(varName_raw)`，避免泛型闭包 `compose(auto transforms)` 中未绑定模板参数 T 无法解析

### Bug D — compose lambda 捕获 transforms 裸指针 ✅

- **位置**：`example/test.cpp:42-55`
- **根因**：`[transforms]` 值捕获裸 `Array*`，循环体 `t()` 触发 GC 后悬垂
- **修复**：CodeGen 已生成 `GcSharedRoot<decltype(transforms_raw)>` init-capture，lambda 副本独立持有 GC 根

---

## 三、process_with_interface SIGSEGV — 已修复 ✅

### 现象

```
Thread 1 received signal SIGSEGV, Segmentation fault.
0x0000000000000000 in ?? ()
#1  process_with_interface (frame_ptr=0x7060e0) at example/test.cpp:165
#2  aura_main (frame_ptr=0x6e5850) at example/test.cpp:187
```

- 退出码 `-1073741819`（0xC0000005）
- rip=0x0 → null 函数指针调用（虚表失效）

### gdb 关键现场（frame 1）

```
processor = @0x5ff650: {_vptr.StringProcessor = 0x7ff7ff5eb480 <vtable for StringProcessor+16>}
data_raw  = 0x1a03b8
result    = 0xbaadf00dbaadf00d      ← Windows 未初始化内存标记
_Coro_resume_index = 2
Aw0/T002/T003/Fs 局部变量全部 = 0xbaadf00dbaadf00d
```

反汇编（崩溃指令）：
```
0x7ff7ff5b2529 <process_with_interface()+392>:   call   *%rsi   ← rsi=0，调用 null
=> 0x7ff7ff5b252b <+394>:   mov    0x20(%rbp),%rdx
```

### 根因分析

**核心证据**：`processor._vptr.StringProcessor` 指向 **`vtable for StringProcessor+16`（基类 vtable）**，而不是 `StringProcessorFunc`（派生类）的 vtable。

`StringProcessor` 是抽象类（含纯虚 `process`），无法直接构造。vptr 指向基类 vtable 的**唯一可能**是：**对象已经被析构**——C++ 析构链中，派生类析构后 vptr 会被编译器调整为基类 vtable，再调用基类析构函数。

→ **processor 引用绑定的 StringProcessorFunc 临时对象在协程恢复时已被析构**。

### 临时对象生命周期追踪

崩溃调用链（`example/test.cpp:256-263`）：

```cpp
co_await [&]() -> auto {
    auto _a20_0 = (io);
    aura_rt::GcRootHandle<decltype(_a20_0)> _h20_0(_a20_0);
    const auto& _a20_1 = (StringProcessorFunc(processor));   // ← 临时对象，生命周期延长到 _a20_1 作用域
    auto _a20_2 = (aura_rt::intern_string("Hello"));
    aura_rt::GcRootHandle<decltype(_a20_2)> _h20_2(_a20_2);
    return process_with_interface(_h20_0.get(), _a20_1, _h20_2.get());
}();
```

C++20 协程参数传递规则：
- `aura_rt::Io io` — 值拷贝到 frame
- `const StringProcessor& processor` — **引用绑定**，frame 中只存引用（指向 IIFE 栈帧上的临时对象）
- `aura_rt::GcString* data_raw` — 值拷贝到 frame

执行时序：
1. IIFE 调用，进入 lambda 体
2. 创建 `_a20_1` 引用绑定的 `StringProcessorFunc(processor)` 临时对象（在 IIFE 栈帧上）
3. 调用 `process_with_interface(...)` → 进入协程
4. 协程同步执行：构造 `data`、调用 `processor.process(data.get())`（line 165）
5. 协程执行 `co_await io.println(...)`（line 166-175）→ **挂起**，返回 task
6. IIFE lambda 体返回 task → **IIFE 局部变量按声明逆序析构**
   - `_h20_2` 析构（GcRootHandle 注销）
   - `_a20_2` 析构
   - `_a20_1` 引用绑定解除 → **StringProcessorFunc 临时对象析构**（vptr 回退到 StringProcessor 基类）
   - `_h20_0` 析构
   - `_a20_0` 析构
7. `co_await task` 恢复协程 → 协程从挂起点继续
8. 协程访问 `processor.process(...)`（gdb 显示的崩溃点）→ **vptr 已失效** → call null → SIGSEGV

### 与 gdb 现场的对应

- `processor._vptr = StringProcessor vtable+16` → 对应步骤 6 的"vptr 回退到基类"
- `result = 0xbaadf00dbaadf00d` → frame 局部变量未初始化标记（Windows debug heap）
- `_Coro_resume_index = 2` → 与单 co_await 不符，疑为 frame 被破坏或 gdb 误读字段；但与 vptr 失效证据一致
- `call *%rsi` 中 rsi=0 → 虚表项为 0（纯虚函数在基类 vtable 中为 null 或 `__cxa_pure_virtual`）

### 根因总结

**`genGcRootedArgs` 生成的 IIFE 把接口适配器 `StringProcessorFunc(processor)` 作为 IIFE 临时对象。IIFE 返回 task 后临时对象立即析构，但协程 frame 中的 `const StringProcessor& processor` 引用仍指向已析构的内存。协程恢复时通过悬垂引用访问 vptr → SIGSEGV。**

**这是 GC 移动之外的另一类指针失效：C++ 临时对象生命周期与协程 frame 生命周期不匹配。** 与 GC compact 无关，但属于"中途指针失效"的同类问题。

---

## 四、修复方案（已实施 ✅）

### 方案 1（采纳）：协程调用时非堆参数声明到 IIFE 外部

**思路**：把 `StringProcessorFunc(processor)` 从 IIFE 临时对象改为调用方（`aura_main` 协程）的局部变量。aura_main 是协程，局部变量在 frame 中，生命周期持续到协程结束，能覆盖所有内部 `co_await` 的恢复时机。

**CodeGen 改动**：在 `StmtGen::genExprStmt`（或 `genCallExpr`）中，识别含接口适配器的协程调用，先在当前作用域声明局部变量：

```cpp
// 期望生成（aura_main 内）：
StringProcessorFunc _adapt_20_1(processor);    // aura_main 局部变量，frame 中
co_await [&]() -> auto {
    auto _a20_0 = (io);
    aura_rt::GcRootHandle<decltype(_a20_0)> _h20_0(_a20_0);
    auto _a20_2 = (aura_rt::intern_string("Hello"));
    aura_rt::GcRootHandle<decltype(_a20_2)> _h20_2(_a20_2);
    return process_with_interface(_h20_0.get(), _adapt_20_1, _h20_2.get());
}();
```

**影响范围**：
- `src/CodeGen/StmtGen.cpp` — 需要支持"表达式前导声明"
- `src/CodeGen/ExprGen.cpp` `genCallExpr` [line 583-598](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L583-598) — 接口适配器包装逻辑需配合
- 需要新增 `preStmts_` 机制，让表达式生成能向前驱语句注入声明

### 方案 2：接口适配器堆分配 + shared_ptr

**思路**：把 `StringProcessorFunc` 放到堆上，`std::make_shared<StringProcessorFunc>(processor)`，shared_ptr 作为 IIFE 局部变量。但 IIFE 返回时 shared_ptr 析构，对象被释放（除非 task 持有 shared_ptr）。

→ 需要让 task 持有 shared_ptr，改动 task 类型，**复杂度高，不推荐**。

### 方案 3：接口参数改为值语义（不通过引用传递）

**思路**：把 `const StringProcessor& processor` 改为 `std::function<GcString*(GcString*)>` 直接传递。但这需要改 Aura 语言的接口语义，**改动过大，不推荐**。

### 推荐采纳：方案 1

---

## 五、其他遗留 Bug（审查中发现，未修复）

### Bug C — for-range 迭代器悬垂（运行期，未触发）

- **位置**：`example/test.cpp:88` `for (auto child : *node.get()->children)`、`example/test.cpp:46` `for (auto t : *transforms.get())`
- **根因**：`Array::begin()/end()` 返回的迭代器内部存储指向 chunk 的裸指针。循环体触发 GC compact 移动 chunk 后，`__begin`/`__end` 悬垂
- **当前状态**：complex_closure 测试中 GC 未在循环中触发 compact，未崩溃；但属于潜在隐患
- **修复方向**：用索引访问替代 range-based for，或循环体外加 `GcCompactSuspendGuard`

### Bug E — `make_processor` 中 `from(s)` 语义（次要）

- **位置**：`example/test.cpp:159` `auto _a3_1 = aura_rt::GcString::from(s);`
- **观察**：`s` 已是 `GcString*`，但 `from` 没有 `from(GcString*)` 重载。当前编译通过，可能匹配隐式转换路径
- **状态**：待确认 CodeGen 是否错误包装了已是 GcString* 的变量

### genMethodDecl 同步修复（待办）

- **位置**：`src/CodeGen/DeclGen.cpp` `genMethodDecl`（line 358+）
- **问题**：当前只修了 `genFunctionDecl` 的堆类型参数 `_raw` + GcRootHandle 包装，方法（`genMethodDecl`）未做同步修复
- **状态**：complex_closure 测试未涉及方法堆类型参数，未触发；但属于遗漏

---

## 六、验证计划（修复后执行）

```powershell
cd d:\you\Aura
cmake --build build           # 编译 aurac
cmake --build runtime/build   # 编译 runtime
.\example\compile.cmd         # 重新生成 test.cpp 并编译
.\example\test.exe            # 运行测试
```

**预期输出**：
```
=== Complex Closure Tests ===
Pipeline: 9
Retry: 70
Processed: [Hello]
Tree root doubled: 2
Child 0 doubled: 4
Grandchild doubled: 8
Counter: 11, 12
Cond(4): 16
Cond(5): -5
=== All complex closure tests passed ===
```

退出码 0，不再出现 `0xC0000005` ACCESS_VIOLATION。

---

## 七、新增 Bug 修复 — 闭包 string 参数被 from(bool) 隐式转换 ✅

### Bug F — `make_processor` 中 `from(s)` 输出 "true" 而非 "Hello"

- **位置**：`src/CodeGen/ExprGen.cpp` `genFunExpr` [line 1099-1121](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L1099-1121)
- **根因**：`genFunExpr` 未将闭包参数注册到 `stringVarNames_`。`isStringExprInChain` 漏判闭包内的 string 参数 `s`，用 `GcString::from(s)` 包装已是 `GcString*` 的变量 → 匹配 `from(bool)` 隐式转换 → 输出 "true"
- **修复**：在 `genFunExpr` 闭包体生成前，将 string 参数注册到 `stringVarNames_`、接口参数注册到 `valueTypeVarNames_`、值类型参数注册到 `valueTypeVarNames_`；生成完毕后恢复原值，避免污染外层作用域

---

## 八、验证结果 ✅

```
=== Complex Closure Tests ===
Pipeline: 9
Retry: 70
Processed: [Hello]          ← 修复前为 [true]，现已正确
Tree root doubled: 2
Child 0 doubled: 4
Grandchild doubled: 8
Counter: 11, 12
Cond(4): 16
Cond(5): -5
=== All complex closure tests passed ===
GC: alloc=3.5KB young=3.5KB old=0B gc=0 minor=0 live=64 pages=1
Exit code: 0                ← 修复前为 -1073741819 (SIGSEGV)
```

所有测试通过，退出码 0。

---

## 九、遗留问题（未修复）

### Bug C — for-range 迭代器悬垂（运行期，未触发）

- **位置**：`example/test.cpp:88` `for (auto child : *node.get()->children)`、`example/test.cpp:46` `for (auto t : *transforms.get())`
- **根因**：`Array::begin()/end()` 返回的迭代器内部存储指向 chunk 的裸指针。循环体触发 GC compact 移动 chunk 后，`__begin`/`__end` 悬垂
- **当前状态**：complex_closure 测试中 GC 未在循环中触发 compact，未崩溃；但属于潜在隐患
- **修复方向**：用索引访问替代 range-based for，或循环体外加 `GcCompactSuspendGuard`

### genMethodDecl 同步修复（待办）

- **位置**：`src/CodeGen/DeclGen.cpp` `genMethodDecl`（line 358+）
- **问题**：当前只修了 `genFunctionDecl` 的堆类型参数 `_raw` + GcRootHandle 包装，方法（`genMethodDecl`）未做同步修复
- **状态**：complex_closure 测试未涉及方法堆类型参数，未触发；但属于遗漏
