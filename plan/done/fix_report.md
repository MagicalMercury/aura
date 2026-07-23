# Aura 语言 — 空指针崩溃 + 字符串输出错误 修复报告

> **测试文件：** `example/test.aura` → `example/test.cpp`
> **症状：** 运行 `test.cpp` 时 SIGSEGV（退出码 139），ASan 报告 stack-buffer-underflow
> **根因：** 两个独立 Bug — (1) 懒协程 + IIFE 导致接口适配器悬垂引用；(2) 闭包 string 参数未注册到 `stringVarNames_`，被 `GcString::from(bool)` 隐式转换
> **修复文件：** `example/test.cpp`（手动修复生成代码 2 处）、`src/CodeGen/ExprGen.cpp`（修复 CodeGen 根因 2 处）
> **验证结果：** ✅ 全部通过，ASan 无报错，输出 `Processed: [Hello]` 正确

---

## 1. Bug 1：懒协程 + IIFE 导致接口适配器悬垂引用 `[崩溃]` `[CodeGen]`

### 1.1 现象

运行 `test.cpp` 时在 `process_with_interface` 协程体第 165 行崩溃：

```
#0 process_with_interface example/test.cpp:165   ← auto result = processor.process(data.get());
#1 aura_main example/test.cpp:187
```

ASan 报告 `stack-buffer-underflow`，读取了已销毁的栈上对象。

### 1.2 根因分析

CodeGen 在 `genGcRootedArgs` 中为含堆类型参数的函数调用生成 IIFE 包装：

```cpp
// 生成的代码（修复前）：
co_await [&]() -> auto {
    auto _a20_0 = (io);
    aura_rt::GcRootHandle<decltype(_a20_0)> _h20_0(_a20_0);
    const auto& _a20_1 = (StringProcessorFunc(processor));  ← 临时对象在 IIFE 内
    auto _a20_2 = (aura_rt::intern_string("Hello"));
    aura_rt::GcRootHandle<decltype(_a20_2)> _h20_2(_a20_2);
    return process_with_interface(_h20_0.get(), _a20_1, _h20_2.get());
}();
```

问题链条：

1. `task<void>` 的 `initial_suspend()` 返回 `std::suspend_always` — 协程是**懒启动**的，调用时不执行函数体
2. IIFE 内调用 `process_with_interface(...)` 仅创建协程帧并返回 `task` 对象，**不执行**协程体
3. IIFE 返回 `task` 后，所有局部变量（包括 `_a20_1` 和它引用的 `StringProcessorFunc` 临时对象）立即**析构**
4. `co_await` 此时才恢复协程执行 → 协程体访问 `processor`（引用已销毁的临时对象）→ **悬垂引用 → SIGSEGV**

关键：对**非协程**调用（如 `map_tree`），IIFE 同步执行完毕后才析构局部变量，临时对象生命周期足够。问题**仅**出现在协程调用场景。

### 1.3 修复方案

**CodeGen 修复**（`src/CodeGen/ExprGen.cpp` `genGcRootedArgs` 函数）：

当 `co_await` 前缀存在（协程调用）时，非堆类型参数声明到 IIFE **外部**（`auto` 值拷贝），堆类型参数仍留在 IIFE 内部（配 `GcRootHandle`）：

```cpp
// 修复后生成的代码：
auto _a20_1 = StringProcessorFunc(processor);     ← 在 IIFE 外声明，生命周期跨越 co_await
co_await [&]() -> auto {
    auto _a20_0 = (io);
    aura_rt::GcRootHandle<decltype(_a20_0)> _h20_0(_a20_0);
    auto _a20_2 = (aura_rt::intern_string("Hello"));
    aura_rt::GcRootHandle<decltype(_a20_2)> _h20_2(_a20_2);
    return process_with_interface(_h20_0.get(), _a20_1, _h20_2.get());
}()
```

**三种分支逻辑：**

| 条件 | 声明位置 | 声明方式 | 原因 |
|------|----------|----------|------|
| 堆类型 | IIFE 内 | `auto` + `GcRootHandle` | GC compact 移动对象后需自动更新指针 |
| 非堆 + 协程调用 | IIFE 外 | `auto` 值拷贝 | 临时对象生命周期必须跨越 `co_await` |
| 非堆 + 非协程调用 | IIFE 内 | `const auto&` | 同步调用，临时对象生命周期足够 |

**生成代码手动修复**（`example/test.cpp` 第 256-263 行）：将 `StringProcessorFunc(processor)` 从 IIFE 内部移到外部。

### 1.4 CodeGen 代码变更（ExprGen.cpp genGcRootedArgs）

```diff
-    std::ostringstream oss;
-    oss << "[&]() -> auto {\n";
+    // 协程调用时，非堆临时对象必须在 IIFE 外声明
+    std::ostringstream outer;
+    std::ostringstream inner;
+    inner << "[&]() -> auto {\n";

     for (size_t i = 0; i < args.size(); ++i) {
         auto& [argExpr, type] = args[i];
         std::string vi = "_a" + std::to_string(hid) + "_" + std::to_string(i);
         bool isHeap = isHeapSemType(type);
-        oss << "    " << (isHeap ? "auto " : "const auto& ")
-            << vi << " = (" << argExpr << ");\n";
         if (isHeap) {
-            oss << "    aura_rt::GcRootHandle<decltype(" << vi
-                << ")> _h" << hid << "_" << i << "(" << vi << ");\n";
+            inner << "    auto " << vi << " = (" << argExpr << ");\n";
+            inner << "    aura_rt::GcRootHandle<decltype(" << vi
+                << ")> _h" << hid << "_" << i << "(" << vi << ");\n";
+        } else if (!awaitPrefix.empty()) {
+            // 非堆 + 协程调用：IIFE 外 auto 值拷贝
+            outer << "auto " << vi << " = (" << argExpr << ");\n";
+        } else {
+            // 非堆 + 非协程：IIFE 内 const auto&
+            inner << "    const auto& " << vi << " = (" << argExpr << ");\n";
         }
     }
-    oss << "    return " << expr << ";\n";
-    oss << "  }()";
+    inner << "    return " << expr << ";\n";
+    inner << "  }()";
-    std::string result = oss.str();
+    std::string result = outer.str() + awaitPrefix + inner.str();
```

---

## 2. Bug 2：闭包 string 参数被 GcString::from(bool) 隐式转换 `[输出错误]` `[CodeGen]`

### 2.1 现象

修复 Bug 1 后程序不再崩溃，但输出 `Processed: [true]` 而非 `Processed: [Hello]`。

### 2.2 根因分析

`make_processor` 返回的闭包参数 `s: string`（`GcString*`）在字符串拼接链中被 CodeGen 错误地包装为 `GcString::from(s)`。

由于 `GcString` 没有提供 `from(GcString*)` 重载，C++ 重载决议选择了 `from(bool)` — `GcString*` 指针隐式转换为 `bool`（非空 = true），返回字符串 `"true"`。

根因在 `genFunExpr`（ExprGen.cpp）：生成闭包（lambda）时未将 string 类型参数注册到 `stringVarNames_` 集合。而 `isStringExprInChain` 依赖 `stringVarNames_` 判断表达式是否为 string 类型 — 若不在集合中，则用 `GcString::from()` 包装。

对比 `genFunctionDecl`（DeclGen.cpp:234）正确注册了函数参数：

```cpp
// genFunctionDecl 中有此逻辑（正确）：
if (p.type && mapType(*p.type).find("aura_rt::GcString*") != std::string::npos)
    stringVarNames_.insert(p.name);

// genFunExpr 中缺失（Bug）—— 闭包参数未注册
```

### 2.3 修复方案

**CodeGen 修复**（`src/CodeGen/ExprGen.cpp` `genFunExpr` 函数）：在生成闭包体前，将 string 类型参数注册到 `stringVarNames_`，生成完毕后恢复原值（避免污染外层作用域）。

```cpp
// 在 genFunExpr 中，闭包体生成前添加：
auto savedStringVars = stringVarNames_;
auto savedValueVars  = valueTypeVarNames_;
for (auto& p : e.params) {
    std::string pname = safeName(p.name);
    if (p.type) {
        std::string ptype = mapType(*p.type);
        // string 参数 → stringVarNames_
        if (ptype.find("aura_rt::GcString*") != std::string::npos)
            stringVarNames_.insert(pname);
        // 接口参数 → valueTypeVarNames_
        if (auto* nt = dynamic_cast<const NamedType*>(p.type.get()))
            if (interfaceNames_.count(nt->name))
                valueTypeVarNames_.insert(pname);
        // 值类型参数 → valueTypeVarNames_
        if (auto* nt = dynamic_cast<const NamedType*>(p.type.get()))
            if ((registeredTypes_.count(nt->name) && !registeredTypes_[nt->name])
                || (BuiltinRegistry::get().findType(nt->name) != nullptr
                    && !BuiltinRegistry::get().isHeapType(nt->name)))
                valueTypeVarNames_.insert(pname);
    }
}

// ... 闭包体生成 ...

// 闭包体生成完毕后恢复：
stringVarNames_ = savedStringVars;
valueTypeVarNames_ = savedValueVars;
```

**生成代码手动修复**（`example/test.cpp` 第 159 行）：

```diff
-auto _a3_1 = aura_rt::GcString::from(s);  // s 是 GcString*，隐式转为 bool → "true"
+auto _a3_1 = (s);                          // s 已是 GcString*，直接使用
```

---

## 3. 验证结果

### 3.1 构建验证

| 组件 | 编译选项 | 结果 |
|------|----------|------|
| Runtime 库 | `-std=c++20 -g -O0 -fsanitize=address` | ✅ OK |
| 编译器 (aurac) | `-std=c++20 -g -O0 -Wall -Wextra -Wpedantic` | ✅ OK |
| 测试程序 | `-std=c++20 -g -O0 -fsanitize=address` | ✅ OK |

### 3.2 运行结果

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
EXIT CODE: 0                ← 修复前为 139 (SIGSEGV)
```

ASan 无任何报错（无 heap-buffer-overflow、use-after-free、stack-buffer-underflow）。

---

## 4. 修改文件清单

| 文件 | 修改类型 | 修改内容 |
|------|----------|----------|
| `src/CodeGen/ExprGen.cpp` | CodeGen | **1. `genGcRootedArgs`（第 68-112 行）：** 拆分为 `outer`/`inner` 两个流。协程调用时非堆参数声明到 `outer`（IIFE 外），非协程时仍用 `const auto&` 在 `inner`（IIFE 内）。<br>**2. `genFunExpr`（第 1156-1180 行）：** 闭包体生成前将参数注册到 `stringVarNames_`/`valueTypeVarNames_`，生成后恢复。 |
| `example/test.cpp` | 生成代码 | **1. 第 256-263 行：** `StringProcessorFunc(processor)` 从 IIFE 内移到 IIFE 外。<br>**2. 第 159 行：** `GcString::from(s)` → `(s)`。 |

---

## 5. 前次会话修复（仍保留）

以下修复来自前次 GC 排查会话，本次未改动但仍然生效：

| 文件 | 修复内容 |
|------|----------|
| `example/test.cpp` | 指针运算 `acc.get() + i` → `concat(acc.get(), i)`（根因：C++ 指针算术替代字符串拼接） |
| `runtime/builtin/string.cpp` | 6 个 `operator+` 重载添加 `GcCompactSuspendGuard`（防止 compact 移动导致引用悬垂） |
| `runtime/builtin/string.h` | 2 个模板 `operator+` 添加 `GcCompactSuspendGuard`；添加 `#include "../gc.h"` |
| `runtime/gc.cpp` | 移除 debug 代码（`isOnValidPage` lambda、DANGLING ROOT 输出）；恢复 `updateAllReferences` 原始逻辑 |
| `runtime/task.cpp` | 移除保守栈扫描 |
| `runtime/gc.cpp` | `updateAllReferences` Young scope 扫描所有对象；`updateList` 处理 young+old |
| `runtime/builtin/array.h` | `GcRootHandle` + `GcCompactSuspendGuard` 保护 |
| `runtime/builtin/string.cpp` | `build_balanced_rope` 和 `GcRopeNode::make` 添加 `GcRootHandle` |
