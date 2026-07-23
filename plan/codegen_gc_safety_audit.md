# CodeGen GC 安全审查报告 — complex_closure + Array 全量测试用例

> 日期：2026-07-23（初次）/ 2026-07-23（更新：Array 全量测试 + ASAN 验证）
> 状态：**全部 Bug 已修复 ✅ | Compact GC 单线程稳定 ✅**
> 输入：`example/test.aura` → aurac 生成 → `example/test.cpp`
> 触发场景：complex_closure（接口、闭包、Tree 递归、lambda 捕获）+ Array 全量测试（迭代器、方法堆参数、GC 压力）
> 验证方式：clang++ 22.1.8 + AddressSanitizer（动态库 `libclang_rt.asan_dynamic-x86_64.dll`）

---

## 一、审查范围

对 `example/test.cpp`（由 aurac 从 `example/test.aura` 生成）做完整 GC 移动安全审查，识别所有可能在 alloc 触发 compact 后导致裸指针悬垂的位置。

**GC 行为简述**：
- `aura_rt::GcHeap::alloc` 在 `youngBytes_ >= kYoungThreshold` 时触发 `minorGc()`
- `minorGc()` 内 `shouldCompact()` 为真时执行 `compact()`，**移动对象到新页**
- compact 后只有通过 `roots_`（GcRootHandle）或 `globalRoots_` 注册的指针会被自动更新
- 裸指针 / 引用 / 迭代器 / 内部 char* 都**不会**被自动更新 → 悬垂

---

## 二、发现的问题

### Bug A — 接口/抽象类生成 `auto` 值拷贝（编译期失败）✅ 已修复

**位置**：`example/test.cpp:163`（`process_with_interface` 内）

```cpp
aura_rt::task<void> process_with_interface(aura_rt::Io io, const StringProcessor& processor, aura_rt::GcString* data) {
auto result = [&]() -> auto {
    auto _a4_0 = (processor);                                // ← 错误！
    aura_rt::GcRootHandle<decltype(_a4_0)> _h4_0(_a4_0);
    auto _a4_1 = (data);
    return _h4_0.get().process(_a4_1);
  }();
```

**根因**：`genGcRootedArgs` ([ExprGen.cpp:68](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L68)) 对所有参数统一生成 `auto _a{hid}_{i} = (argExpr);` 值拷贝。

对接口类型 `const StringProcessor&`：
- `auto` 推断为非引用类型 `StringProcessor`
- `StringProcessor` 是抽象类（有纯虚 `process`），不能值构造
- → 编译错误：`cannot construct an object of abstract type 'StringProcessor'`

**编译错误**：
```
example/test.cpp:163:28: error: cannot construct an object of abstract type 'StringProcessor'
```

**修复方向**：`genGcRootedArgs` 中：
- 堆类型（`isHeapSemType(type) == true`）：保持 `auto _a = (expr); GcRootHandle<decltype(_a)> _h(_a);` 值拷贝 + 包装
- **非堆类型**：改用 `const auto& _a = (expr);` 引用绑定，避免拷贝

非堆类型涵盖：
- 接口（`const StringProcessor&`、`const Io&`）
- `std::function<T(U)>`（避免昂贵拷贝）
- 值类型 `int32_t`/`double`/`bool`（const auto& 对内置类型也合法）

---

### Bug B — `map_tree` 中 `node` 裸指针参数未保护（运行期 SIGSEGV）✅ 已修复

**位置**：`example/test.cpp:83-113`

```cpp
template<typename T, typename U>
Tree<U>* map_tree(std::function<U(T)> f, Tree<T>* node) {
aura_rt::Array<Tree<U>*>* new_children_raw = aura_rt::Array<Tree<U>*>::make(0);  // ① 触发 GC，移动 node
aura_rt::GcRootHandle<aura_rt::Array<Tree<U>*>*> new_children(new_children_raw);
for (auto child : *node->children) {                                              // ② node 已悬垂！
    ...
    return map_tree(_h7_0.get(), _h7_1.get());
    ...
}
auto* _raw = aura_rt::gc_alloc<Tree<U>>(&Tree<U>::_desc);                          // ③ 再次触发 GC
aura_rt::GcRootHandle<decltype(_raw)> _rec_0(_raw);
auto _fv_value = ([&]() -> auto {
    auto _a9_0 = (node->value);                                                    // ④ node 已悬垂！
    aura_rt::GcRootHandle<decltype(_a9_0)> _h9_0(_a9_0);
    return f(_h9_0.get());
  }());
```

**根因**：`map_tree` 是用户定义函数，参数 `Tree<T>* node` 是裸指针。**CodeGen 不会为用户函数参数自动包装 GcRootHandle**——只有 `let` 局部变量、lambda 调用 IIFE 参数走 `genGcRootedArgs`。

函数体内：
- ① `Array::make(0)` 内部 `alloc` 触发 GC → 移动 `node` 对象 → `node` 本地变量仍是旧地址
- ② `*node->children` 解引用悬垂指针 → SIGSEGV
- ③ `gc_alloc` 再次触发 GC → `node` 仍悬垂
- ④ `node->value` 解引用悬垂指针 → SIGSEGV

**修复方向**：
1. **短期方案**：函数入口处把所有 GC 堆类型参数包装为 GcRootHandle：
   ```cpp
   Tree<U>* map_tree(std::function<U(T)> f, Tree<T>* node_raw) {
       aura_rt::GcRootHandle<Tree<T>*> node(node_raw);
       // 后续全用 node.get()
   }
   ```
2. **长期方案**：在 `genFunctionDecl` 中识别堆类型参数，自动生成 raw + GcRootHandle 包装对。

---

### Bug C — for-range 迭代器在 GC 移动 chunk 后悬垂（运行期）✅ 已修复

**位置**：
- `example/test.cpp:86` `for (auto child : *node->children)`
- `example/test.cpp:45` `for (auto t : *transforms)`

```cpp
for (auto child : *node->children) {  // range-based for 内部展开：
    // auto&& __range = *node->children;
    // auto __begin = __range.begin();
    // auto __end = __range.end();
    // for (; __begin != __end; ++__begin) {
    //     auto child = *__begin;
    //     ...循环体... map_tree(f, child) ← 触发 GC
    // }
}
```

**根因**：`Array::begin()`/`end()` 返回的迭代器内部存储指向 chunk 的裸指针。如果循环体触发 GC compact 移动 chunk，`__begin`/`__end` 悬垂。

**修复方向**：
1. **方案 A**：用索引访问替代 range-based for：
   ```cpp
   auto size = node->children->size();  // 缓存 size
   for (size_t i = 0; i < size; ++i) {
       auto child = (*node->children)[i];  // 每次重新解引用（chunk 内部指针每次重读）
       // 或用 GcRootHandle 保护 child 后再用
   }
   ```
2. **方案 B**：循环体外加 `GcCompactSuspendGuard`，循环期间禁 compact。

方案 A 更激进但根治迭代器悬垂；方案 B 是最小改动但延迟 compact。

---

### Bug D — `compose` lambda 闭包成员 transforms 未保护（运行期）✅ 已修复

**位置**：`example/test.cpp:42-54`

```cpp
auto compose(auto transforms) {  // transforms 是裸 Array*
return [transforms]<typename T>(T input) -> T {  // ← 值捕获（仍是裸指针）
    auto current = input;
    for (auto t : *transforms) {  // ← 解引用裸指针
    current = [&]() -> auto {
        ...
        return t(_h0_0.get());  // ← t() 可能触发 GC
      }();
    }
    return current;
};
}
```

**根因**：`transforms` 被 lambda 值捕获，类型是 `aura_rt::Array<...>*`（裸指针）。lambda 内部循环每次解引用 `*transforms`——如果 `t()` 内部触发 GC compact，移动 transforms 数组，下次迭代 `*transforms` 悬垂。

**修复方向**：
1. **方案 A**：lambda 捕获改为 GcRootHandle：
   ```cpp
   auto compose(auto transforms_raw) {
       aura_rt::GcRootHandle<decltype(transforms_raw)> _t(transforms_raw);
       return [_t = std::move(_t)]<typename T>(T input) -> T {
           // 用 _t.get() 替代 transforms
       };
   }
   ```
2. **方案 B**：lambda 内部每次访问 `transforms` 前重新包装为 GcRootHandle（但 lambda 捕获成员无法重新注册）。
3. **方案 C**：循环期间禁 compact（覆盖 Bug C 和 D）。

方案 A 最干净；方案 C 是临时缓解。

---

### Bug E（次要）— `make_processor` 中 `from(s)` 调用语义错误 ✅ 已修复

**位置**：`example/test.cpp:155-159`

```cpp
std::function<aura_rt::GcString*(aura_rt::GcString*)> make_processor(aura_rt::GcString* prefix, aura_rt::GcString* suffix) {
return [prefix, suffix](aura_rt::GcString* s) -> aura_rt::GcString* {
    return [&](){auto _a3_0 = prefix;...;auto _a3_1 = aura_rt::GcString::from(s);...}();
};
}
```

**观察**：`aura_rt::GcString::from(s)` 中 s 是 `GcString*`，但 `from` 没有 `from(GcString*)` 重载。

**待确认**：此处编译能通过（测试运行 OK），可能匹配某个隐式转换路径，或 CodeGen 把 `s` 当作 string 字面量处理（生成 `from(s)` 但实际应直接用 s）。

**建议**：审查 CodeGen 在 `prefix + s + suffix` 链中如何处理中间变量 s；若 CodeGen 错误地包装了已是 GcString* 的变量，应跳过包装。

---

## 三、修复优先级

| Bug | 严重度 | 难度 | 优先级 |
|:---|:---:|:---:|:---:|
| A 接口拷贝编译失败 | 🔴 编译期 | 低 | P0 立即修复 |
| B map_tree node 悬垂 | 🔴 运行期 SIGSEGV | 中 | P0 必修 |
| C for-range 迭代器 | 🟡 运行期 | 中 | P1 |
| D compose 闭包成员 | 🟡 运行期 | 中 | P1 |
| E from(s) 语义 | 🟢 待确认 | 低 | P2 |

---

## 四、Bug A 修复方案（最小改动）

**文件**：`src/CodeGen/ExprGen.cpp` ([line 62-73](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L62-73))

```cpp
// 修改前
for (size_t i = 0; i < args.size(); ++i) {
    auto& [argExpr, type] = args[i];
    std::string vi = "_a" + std::to_string(hid) + "_" + std::to_string(i);
    oss << "    auto " << vi << " = (" << argExpr << ");\n";
    if (isHeapSemType(type)) {
        oss << "    aura_rt::GcRootHandle<decltype(" << vi << ")> _h"
            << hid << "_" << i << "(" << vi << ");\n";
    }
}

// 修改后：非堆类型用 const auto& 避免拷贝（接口/抽象类/Io 等）
for (size_t i = 0; i < args.size(); ++i) {
    auto& [argExpr, type] = args[i];
    std::string vi = "_a" + std::to_string(hid) + "_" + std::to_string(i);
    bool isHeap = isHeapSemType(type);
    // 堆类型用 auto 值拷贝（随后包装为 GcRootHandle）；
    // 非堆类型用 const auto& 引用绑定，避免抽象类/接口拷贝失败
    oss << "    " << (isHeap ? "auto " : "const auto& ")
        << vi << " = (" << argExpr << ");\n";
    if (isHeap) {
        oss << "    aura_rt::GcRootHandle<decltype(" << vi << ")> _h"
            << hid << "_" << i << "(" << vi << ");\n";
    }
}
```

**预期效果**：
- 接口参数：`const auto& _a4_0 = (processor);` 引用绑定，不拷贝 → 编译通过
- 堆类型参数：`auto _a3_0 = prefix;` + GcRootHandle 包装 → 行为不变
- 值类型 int32_t：`const auto& _a0_0 = (current);` 对内置类型 const& 仍合法

---

## 五、Bug B 修复方案

**短期方案**：用户函数参数包装为 GcRootHandle。

**文件**：`src/CodeGen/DeclGen.cpp`（函数声明生成处）

需找到生成 `Tree<U>* map_tree(std::function<U(T)> f, Tree<T>* node)` 的代码，在函数体入口处插入：
```cpp
Tree<U>* map_tree(std::function<U(T)> f, Tree<T>* node_raw) {
    aura_rt::GcRootHandle<Tree<T>*> node(node_raw);
    // 后续代码用 node.get() 替代 node
}
```

**实现复杂度**：
- 需要识别函数参数中所有堆类型（GcString*/Tree<T>*/Array<T>* 等）
- 改写后续代码中对参数的引用：`node` → `node.get()`
- 类似 `let` 变量的 `gcRootVarNames_` 机制，把参数名加入集合

**长期方案**：在 `genFunctionDecl` 中自动包装堆类型参数。

---

## 六、Bug C/D 修复方案

**短期方案**：在 `for` 循环体外加 `GcCompactSuspendGuard`，循环期间禁 compact。

**长期方案**：
1. for-range 改为索引访问 + 重新解引用
2. lambda 闭包成员用 GcRootHandle 替代裸指针捕获

---

## 七、验证计划

```powershell
cd d:\you\Aura
cmake --build build           # 编译 aurac
cmake --build runtime/build   # 编译 runtime
.\example\compile.cmd         # 重新生成 test.cpp
.\example\test.exe            # 运行测试
```

**预期**：
- 编译通过（Bug A 修复后）
- 运行无 SIGSEGV（Bug B/C/D 修复后）
- 输出：`Pipeline: 9` `Retry: 70` `Tree root doubled: 2` 等

---

## 八、修复状态总结（更新于 2026-07-23）

### 已完成 ✅

| Bug | 严重度 | 修复文件 | 修复方式 | 验证 |
|:---|:---:|:---|:---|:---:|
| A 接口拷贝编译失败 | 🔴 编译期 | `src/CodeGen/ExprGen.cpp` | `isHeapSemType` 排除 InterfaceSemType/FuncSemType；非堆用 `const auto&` | ✅ |
| B map_tree node 悬垂 | 🔴 运行期 SIGSEGV | `src/CodeGen/DeclGen.cpp` | `funSignature` 加 `_raw` 后缀 + 函数体入口 `GcRootHandle<decltype(_raw)>` 包装 + 注册 `gcRootVarNames_`/`gcRootTypes_` | ✅ |
| D compose 闭包成员 | 🟡 运行期 | `src/CodeGen/DeclGen.cpp` + `ExprGen.cpp` | lambda init-capture 用 `GcSharedRoot<decltype(_raw)>` 包装 | ✅ |
| E from(s) 语义 | 🟢 输出错误 | `src/CodeGen/ExprGen.cpp` `genFunExpr` | 闭包参数注册到 `stringVarNames_`/`valueTypeVarNames_`，生成后恢复 | ✅ |

**验证结果**：`example/test.exe` 全部 9 项测试通过，退出码 0。

### 未完成 ⏸️

（无）

### 本轮新增修复（Array 全量测试 + ASAN 揭示）

| Bug | 严重度 | 修复文件 | 修复方式 | 验证 |
|:---|:---:|:---|:---|:---:|
| F 非泛型自引用 record 字段空列表 | 🔴 运行期 SIGSEGV | `src/Sema/Checker/DeclChecker.cpp` | 非泛型 record 定义时调用 `sealSelfRefs` 将自引用 GenericSemType 的 `resolvedName` 设为类型名（之前仅泛型路径走 `materializeCanonicalName` 调用 sealSelfRefs） | ✅ Array 测试 7 项 |
| G RecordExpr `_fv_` 变量名冲突 | 🟡 编译期 | `src/CodeGen/StmtGen.cpp` | `genLetStmt`/`genReturnStmt` 中 RecordExpr 字段变量 `_fv_{name}` 加 `recIdx` 后缀避免同一作用域内多个 RecordExpr 冲突 | ✅ Array 测试 7 项 |
| genMethodDecl 同步 | 🟡 潜在 | `src/CodeGen/DeclGen.cpp` | 参照 `genFunctionDecl` 的三处改动：param 跟踪 + 签名 `_raw` + 函数体入口 `GcRootHandle` 包装 | ✅ Array 测试 7 项 |

### Bug E 修复详情（补记）

**根因**：`genFunExpr` 未将闭包参数注册到 `stringVarNames_`。`isStringExprInChain` 漏判闭包内的 string 参数 `s`，用 `GcString::from(s)` 包装已是 `GcString*` 的变量 → 匹配 `from(bool)` 隐式转换 → 输出 `"true"` 而非 `"Hello"`。

**修复**：`genFunExpr` 闭包体生成前注册参数（string→`stringVarNames_`，接口/值类型→`valueTypeVarNames_`），生成后恢复原值，避免污染外层作用域。

### Bug C / genMethodDecl 处理详情

1. **Bug C**（✅ 已修复）：
   - **采纳方案**：ArrayIterator 内部用 `GcRootHandle<ArrayChunk<T>*>` 注册 chunk 到 GC roots
   - **机制**：GcRootHandle.ptr_ 指向 `&chunk`，compact 时 `updateAllReferences` 通过 ptr_ 找到 chunk 变量并更新其值
   - **性能**：每次 begin()/end() 各一次 register/unregister，均 O(1)（roots_ 已从 vector 改为 unordered_set）
   - **限制**：ArrayIterator 不可拷贝/移动（GcRootHandle 限制），C++17 强制 RVO 保证 `auto __begin = arr.begin()` 不需要拷贝构造
2. **genMethodDecl**（✅ 已修复）：参照 `genFunctionDecl` 的三处改动同步实施（param 跟踪 + 签名 `_raw` + 函数体入口包装）

### 性能优化（本轮）

**roots_ 容器优化**（`runtime/gc.h` + `runtime/gc.cpp`）：
- `std::vector<GcRootHandle<GcObject*>*>` → `std::unordered_set<GcRootHandle<GcObject*>*>`
- `registerRoot`：vector `push_back` O(1) → unordered_set `insert` O(1)（持平）
- `unregisterRoot`：**vector `find` + `erase` O(n) → unordered_set `erase` O(1)**（显著改善）
- 遍历顺序不影响正确性：markPhase 和 updateAllReferences 对每个 root 独立操作

### 最终验证（2026-07-23）

**测试矩阵**：

| 测试 | 用例数 | ASAN | 退出码 |
|:---|:---:|:---:|:---:|
| complex_closure（`example/used/1.aura`） | 9 项断言 | 无报错 | 0 |
| Array 全量测试（`example/test.aura`） | 7 个测试函数 | 无报错 | 0 |

**结论**：Compact GC 在单线程场景下已稳定。所有 GC 移动安全 bug 已根治，包括：
- 函数参数 / 方法参数裸指针（Bug B、genMethodDecl）
- for-range 迭代器（Bug C）
- 闭包成员捕获（Bug D）
- 接口/IIFE 适配器（Bug A）
- 空列表在 RecordExpr 字段中的类型推断（Bug F）
- RecordExpr 变量名冲突（Bug G）
- 闭包参数字符串拼接语义（Bug E）

**未覆盖场景**（需后续测试）：
- 多线程并发 GC（sync thread 场景）
- 协程并发 IO（io 协程化场景）
- Union 类型变体切换时的 GC 安全
