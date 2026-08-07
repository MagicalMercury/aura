# threadRootLists_ node（this）悬垂修复（闭包捕获 ThreadLocal 模式 GcRootHandle 进堆）— 草案

> 工作流：起草 issue 实现草案（工作流 2）
> 提出时间：2026-08-06
> 状态：**待审查**（已根据首轮审查反馈修正：填充点 3 处、清理点扩到 6 处补齐状态泄漏、value 表达式 decltype 模式、补充 SBO 事实、标注与联合变体 plan 的共享 ViewRoot 构造改动；第三轮审查 change.md 合并文档确认本 plan 源码映射全部一致、无修改点，已知限制 1-3 保留）。
> 来源：[TODO.txt](file:///d:/you/Aura/TODO.txt) [五] P2「threadRootLists_ node（this）悬垂」（L208-229）。
> 关联：[plan/compact全局根地址重定位修复.md](file:///d:/you/Aura/plan/compact全局根地址重定位修复.md) §10 限制 1（已知限制，本次解决）。

---

## 1. 元信息

| 项目 | 内容 |
| ---- | ---- |
| Plan 标题 | threadRootLists_ node（this）悬垂修复（闭包捕获 ThreadLocal 模式 GcRootHandle 进堆） |
| 相关模块 | `runtime/builtin/iterator.h`（ViewRoot）、`runtime/gc/handles.h`、`src/CodeGen/ExprGen.cpp`、`src/CodeGen/StmtGen.cpp`、`src/CodeGen/DeclGen.cpp`、`src/CodeGen/CodeGen.h` |
| 触发场景 | 闭包值捕获 ViewRoot（含 ThreadLocal 模式 GcRootHandle）进 GC 堆对象（如 MapIter::fn_） |
| 优先级 | P2（潜在缺陷，当前无触发代码路径） |
| 前置条件 | [x] compact 全局根重定位修复（relocateGlobalRootPtrs 已实现，覆盖 ValueGlobal 模式） |

---

## 2. Objectives

消除 ThreadLocal 模式 GcRootHandle 进 GC 堆后的链表 node 悬垂隐患。当闭包值捕获 ViewRoot（含 ThreadLocal 句柄）进 GC 堆对象时，compact 搬运对象后 `threadRootLists_` 链表内的 node（this）地址悬垂，下轮 GC 链表遍历读到被复用内存 → 崩溃。本 plan 通过 CodeGen 层改造，将闭包捕获的 ViewRoot 副本转为 Global 根，复用已有 `relocateGlobalRootPtrs` 机制，零 compact 层改动。

---

## 3. Current State Summary（分析报告）

### 3.1 Codebase Scan

| 文件 | 职责 | 关键点 |
| ---- | ---- | ---- |
| [iterator.h:72-81](file:///d:/you/Aura/runtime/builtin/iterator.h#L72-L81) | ViewRoot 模板 | `T v; GcRootHandle<GcObject*> h;`，构造硬编码 `GcRootScope::ThreadLocal`（L76） |
| [handles.h:32-41](file:///d:/you/Aura/runtime/gc/handles.h#L32-L41) | GcRootHandle 值持有构造 | ValueThreadLocal 调 `registerRootThreadLocal(this)`（L40）；ValueGlobal 调 `registerGlobalRoot(ptr_ref_)`（L38） |
| [roots.cpp:25-44](file:///d:/you/Aura/runtime/gc/roots.cpp#L25-L44) | 线程局部链表管理 | `registerRootThreadLocal(this)` 头插链表，`this` 地址存入链表 head/前驱 next_/后继 prev_ |
| [compact.cpp:296-315](file:///d:/you/Aura/runtime/gc/compact.cpp#L296-L315) | updateAllReferences 步骤 1 | 遍历 `threadRootLists_`，仅更新 `*node->ptr_ref_` 值；**不重定位 node（this）地址** |
| [compact.cpp:416-445](file:///d:/you/Aura/runtime/gc/compact.cpp#L416-L445) | relocateGlobalRootPtrs | 重定位 `globalRoots_` 中的 rootPtr + 对象内 ptr_ref_ 槽位（方案 P，已修复 ValueGlobal） |
| [ExprGen.cpp:1607-1624](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L1607-L1624) | genFunExpr 闭包捕获 | L1611 `cn = safeName(captures[i])`；L1615-1620 gcRootVarNames_ 走 init-capture 转 Global；L1621-1623 else 分支（含 viewRootVarNames_）走按值捕获 `oss << cn;` |
| [StmtGen.cpp:361](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L361) | ViewRoot 生成点 1 | genLetStmt 中 let 视图变量；**有 viewRootType 局部变量可用**（L140 声明，L161/L342/L351 赋值） |
| [DeclGen.cpp:454](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L454) | ViewRoot 生成点 2 | genFunDecl 中接口视图函数参数；**无 viewRootType 变量**，用 `decltype(varName_raw)` |
| [DeclGen.cpp:692](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L692) | ViewRoot 生成点 3 | genMethodDecl 中接口视图方法参数；**无 viewRootType 变量**，用 `decltype(varName_raw)` |
| [CodeGen.h:522](file:///d:/you/Aura/src/CodeGen/CodeGen.h#L522) | viewRootVarNames_ | `std::set<std::string>`，**无配套 viewRootTypes_**（gcRootTypes_ 在 L526） |

### 3.2 Dependency Map

```
用户代码：it.map([viewVar](x) { ... })  // 闭包捕获视图变量
  → genFunExpr (ExprGen.cpp:1621-1623): viewRootVarNames_ 走按值捕获 → 生成 [viewVar]
  → lambda 副本含 ViewRoot 副本 → ViewRoot 副本的 GcRootHandle<GcObject*> 构造
    → registerRootThreadLocal(this)（handles.h:40）→ this 落在 GC 堆内（lambda 副本是 MapIter::fn_ 成员）
  → compact 搬运 MapIter：
    - updateAllReferences 步骤 1 遍历 threadRootLists_，deref node->ptr_ref_（旧地址，可能悬垂）
    - relocateGlobalRootPtrs 不处理 threadRootLists_（仅处理 globalRoots_）
    - rebuildPageList 释放旧页 → this 地址悬垂
  → 下轮 GC markPhase 遍历 threadRootLists_ → deref 悬垂 this → 崩溃
```

### 3.3 Interface Inventory

| 接口 | 契约 | 当前状态 |
| ---- | ---- | ---- |
| `ViewRoot(T it)` | 构造栈上视图根，scope=ThreadLocal | 硬编码 ThreadLocal，闭包捕获不安全 |
| `genFunExpr` 捕获 | gcRootVarNames_ → Global；viewRootVarNames_ → 按值捕获（ThreadLocal） | viewRootVarNames_ 未转 Global |
| `relocateGlobalRootPtrs` | 重定位 globalRoots_ 中 rootPtr + 对象内 ptr_ref_ | 已修复 ValueGlobal ✓ |
| `updateAllReferences` 步骤 1 | 遍历 threadRootLists_ 更新 *ptr_ref_ | 不重定位 node（this）地址 |
| `viewRootTypes_` map | 记录视图变量 C++ 类型名 | **不存在**，需新增 |

### 3.4 Business Logic Extraction

- **ThreadLocal 模式**：GcRootHandle 构造时 `registerRootThreadLocal(this)`，`this` 地址存入线程局部链表。栈上使用时 this 稳定，compact 不移动栈 → 安全。
- **Global 模式**：GcRootHandle 构造时 `registerGlobalRoot(&val_)`，`&val_` 存入 `globalRoots_` 容器。&val_ 可能在堆内 → compact 时由 `relocateGlobalRootPtrs` 重定位 → 安全。
- **当前缺陷**：ViewRoot 闭包捕获走按值捕获（ThreadLocal），副本进堆后 this 悬垂。
- **修复思路**：闭包捕获时转 Global，复用 `relocateGlobalRootPtrs` 机制。
- **SBO 补充事实**：lambda 通常超 SBO（Small Buffer Optimization）→ std::function 堆分配（非 GC 管理、不移动），实际多数场景 `&val_` 天然稳定；SBO 场景（lambda 捕获量小）才真正靠 `relocateGlobalRootPtrs` 兜底。两者都安全，无碍。但 CodeGen 层无法静态判断 lambda 是否超 SBO，故统一转 Global 保证正确性。

### 3.5 State & Side Effects

- `viewRootVarNames_` 是 CodeGen 状态，genFunExpr 捕获时查询。
- `viewRootTypes_`（新增）需在 ViewRoot 生成点同步填充，与 `gcRootTypes_` 机制一致。
- ViewRoot 副本的 `T v` 字段含视图值（含 self 指针），不被 GC 扫描（ViewRoot 非 GcObject）。ViewRoot::get() 每次从 h.get() 恢复 self，不依赖 v.self 持久有效。

---

## 4. Proposed Changes

### 4.1 方案选择：CodeGen 层转 Global（推荐，方向 B）

genFunExpr 中 viewRootVarNames_ 走 init-capture，构造 ViewRoot 副本时转 GcRootScope::Global。复用已有 `relocateGlobalRootPtrs` 机制（已修复 ValueGlobal），compact 层零改动。

**不选方向 A（compact 层重定位链表 node）的理由**：需快照所有要重定位的 {oldNode, newNode, prev, next, list}，统一执行链表重连，顺序耦合复杂（须在 deref 前完成快照），改动 compact.cpp 大量代码。方向 B 最小化改动，复用已验证机制。

### 4.2 改动 A：ViewRoot 模板添加闭包捕获专用构造

**文件**：[iterator.h:72-81](file:///d:/you/Aura/runtime/builtin/iterator.h#L72-L81)

```cpp
template <typename T>
struct ViewRoot {
    T v;
    GcRootHandle<GcObject*> h;
    // 栈上使用：scope 默认 ThreadLocal（无锁，线程局部链表）
    explicit ViewRoot(T it, GcRootScope scope = GcRootScope::ThreadLocal)
        : v(it), h(it.self, scope) {}
    // 闭包捕获专用：从已有 ViewRoot 的最新 self 构造 Global 副本
    // - 用 other.h.get() 取最新 self（避免 compact 后 self 旧值悬垂）
    // - scope 强制 Global，&val_ 注册到 globalRoots_，compact 由 relocateGlobalRootPtrs 重定位
    ViewRoot(T it, GcObject* self, GcRootScope scope)
        : v(it), h(self, scope) {
        v.self = self;  // 同步 v.self 为最新值
    }
    T get() {
        v.self = h.get();
        return v;
    }
};
```

**原理**：
- 栈上使用：`ViewRoot<T>(it)` → scope=ThreadLocal（默认），无锁开销。
- 闭包捕获：`ViewRoot<T>(it, self, GcRootScope::Global)` → scope=Global，&val_ 注册到 globalRoots_，compact 时由 `relocateGlobalRootPtrs` 重定位（已修复）。
- `v.self = self` 确保 v 字段的 self 也是最新值（虽然 get() 会覆盖，但防止闭包体内直接访问 v.self 时悬垂）。
- 构造函数签名 `ViewRoot(T, GcObject*, GcRootScope)` 与既有 `explicit ViewRoot(T, GcRootScope=ThreadLocal)` 参数数量不同，无冲突。

### 4.3 改动 B：genFunExpr 中 viewRootVarNames_ 走 init-capture 转 Global

**文件**：[ExprGen.cpp:1607-1624](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L1607-L1624)

**现状**（L1621-1623 else 分支）：viewRootVarNames_ 走按值捕获 `oss << cn;`（cn 已是 safeName 后的，L1611）。

**改造**：在 else 分支前插入 viewRootVarNames_ 分支（类似 gcRootVarNames_）：

```cpp
} else if (viewRootVarNames_.count(captures[i])) {
    // 视图根变量 → init-capture 创建 ViewRoot 副本（Global 根，闭包跨线程安全）
    // 复用 relocateGlobalRootPtrs 机制，避免 ThreadLocal 句柄进堆后 this 悬垂
    // 注意：查询用 captures[i]（与填充时 safeName 后的 key 一致），生成代码用 cn（已 safeName）
    std::string type = viewRootTypes_[captures[i]];
    oss << cn << " = aura_rt::ViewRoot<" << type << ">(" << cn << ".v, "
        << cn << ".h.get(), aura_rt::GcRootScope::Global)";
} else {
    oss << cn;
}
```

**生成代码示例**（用户代码 `it.map([viewVar](x) { ... })`）：
```cpp
[viewVar = aura_rt::ViewRoot<Iterator<int32_t>>(viewVar.v, viewVar.h.get(), aura_rt::GcRootScope::Global)]
```

> **查询 key 一致性说明**：`viewRootVarNames_.count(captures[i])` 和 `viewRootTypes_[captures[i]]` 用 captures[i]（原始名）；填充点用 `safeName(p.name)` 作 key。当变量名是 C++ 关键字时两者不一致——这是 gcRootVarNames_ 路径的既有问题（[ExprGen.cpp:1615](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L1615) 同样用 captures[i] 查询），本 plan 保持一致，不引入新问题。

### 4.4 改动 C：新增 viewRootTypes_ map 并在 3 处填充点同步

**文件**：[CodeGen.h:522](file:///d:/you/Aura/src/CodeGen/CodeGen.h#L522)

新增声明（类似 L526 `gcRootTypes_`）：
```cpp
std::unordered_map<std::string, std::string> viewRootTypes_;  // 视图类型 T 的 C++ 类型名
```

**填充点：3 处**（与 viewRootVarNames_.insert 一一对应）：

| # | 文件 | 行号 | 上下文 | value 表达式 | 说明 |
|---|------|------|--------|-------------|------|
| 1 | [StmtGen.cpp](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L361) | L361 后 | genLetStmt let 视图变量 | `viewRootType` | 局部变量（L140 声明，L161/L342/L351 赋值），已非空 |
| 2 | [DeclGen.cpp](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L454) | L454 后 | genFunDecl 接口视图函数参数 | `"decltype(" + safeName(p.name) + "_raw)"` | 无 viewRootType 变量，用 decltype 模式（与 gcRootTypes_ L470 一致） |
| 3 | [DeclGen.cpp](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L692) | L692 后 | genMethodDecl 接口视图方法参数 | `"decltype(" + safeName(p.name) + "_raw)"` | 同上 |

> **修正首轮草案错误**：首轮草案将 DeclGen.cpp:454 描述为"record 字段为接口视图"——实际是 genFunDecl 中接口视图函数参数。record 字段为接口视图走 genRecordStruct 的 ptrFields（"field+ViewType" 复合偏移，[DeclGen.cpp:92-96](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L92-L96)），不进 viewRootVarNames_。

**清理点：6 处**（与 `gcRootVarNames_.clear()` 的既有 6 处对齐；其中 genFunDecl 末尾、genConstructor 末尾是本次补齐的**状态泄漏修复**——上轮 P1 落码时 `gcRootVarNames_` 有 6 处 clear 而 `viewRootVarNames_` 只有 4 处，残留状态会让后续 `genFunExpr` 对同名普通变量误触发改动 B 的 init-capture）：

| # | 文件 | 行号 | 所在函数 |
|---|------|------|---------|
| 1 | [DeclGen.cpp](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L189) | L189 后 | genInterfaceDecl 默认方法体后 |
| 2 | [DeclGen.cpp](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L434) | L434 后 | genFunDecl 开始前 |
| 3 | [DeclGen.cpp](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L526-L530) | L529 后 | genFunDecl 结束后（与 `gcRootVarNames_.clear()` L528 同处；**本次补齐**） |
| 4 | [DeclGen.cpp](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L673) | L673 后 | genMethodDecl 开始前 |
| 5 | [DeclGen.cpp](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L781) | L781 后 | genMethodDecl 结束后 |
| 6 | [DeclGen.cpp](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L830-L834) | L833 后 | genConstructor 结束后（与 `gcRootVarNames_.clear()` L832 同处；**本次补齐**） |

### 4.5 不需要改动的部分

- **compact.cpp**：零改动，复用 `relocateGlobalRootPtrs`（已修复 ValueGlobal）。
- **handles.h**：GcRootHandle 构造函数已支持 GcRootScope::Global。
- **roots.cpp**：线程局部链表逻辑不变（栈上使用仍走 ThreadLocal）。
- **genIdentifier**（[ExprGen.cpp:248-252](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L248-L252)）：`.get()` 访问路径不变（闭包体内的 viewRootVarNames_ 变量仍走此路径）。

---

## 5. Impact Analysis

| 影响面 | 说明 |
| ---- | ---- |
| `runtime/builtin/iterator.h` | ViewRoot 新增构造函数（闭包捕获专用） |
| `src/CodeGen/ExprGen.cpp` | genFunExpr 新增 viewRootVarNames_ init-capture 分支 |
| `src/CodeGen/CodeGen.h` | 新增 viewRootTypes_ map |
| `src/CodeGen/StmtGen.cpp` | L361 填充 viewRootTypes_ |
| `src/CodeGen/DeclGen.cpp` | L454/L692 填充 viewRootTypes_；L189/L434/L673/L781/L529/L833 同步 clear（后 2 处本次补齐状态泄漏） |
| ⚠️ BREAKING | 无：纯内部改造，用户代码不变 |
| 行为变化 | 闭包捕获 ViewRoot 时，副本的 GcRootHandle 从 ThreadLocal 转为 Global；compact 时由 relocateGlobalRootPtrs 重定位 |
| 性能 | 闭包捕获的 ViewRoot 副本构造时多一次 `globalRoots_m_` 锁（仅闭包捕获时，栈上使用无影响） |
| 并发 | Global 根跨线程安全（与既有 gcRootVarNames_ init-capture 同机制） |
| SBO 事实 | lambda 超 SBO 时 std::function 堆分配（非 GC 管理、不移动），&val_ 天然稳定；SBO 场景才靠 relocateGlobalRootPtrs 兜底。两者都安全，但 CodeGen 统一转 Global 保证正确性 |

### 升级/回滚兼容

- 独立可交付；回滚 = `git restore` 五个文件。
- 不影响 compact、mark-sweep 对栈上 ViewRoot 的处理。

---

## 6. Boundary Condition Handling Strategy

| 边界条件 | 现状处理 | 计划处理 | 测试策略 |
| ---- | ---- | ---- | ---- |
| 栈上 ViewRoot（let / for-in / 参数） | ThreadLocal，无锁 ✓ | 不变（默认 scope=ThreadLocal） | 既有用例回归 |
| 闭包捕获 ViewRoot 进堆 | ThreadLocal，this 悬垂 ✗ | init-capture 转 Global，复用 relocateGlobalRootPtrs | 新增用例 + ASAN |
| 闭包捕获 ViewRoot 不进堆（栈上 lambda） | ThreadLocal，this 在栈上 ✓ | 转 Global（多一次锁，安全但略慢） | 既有用例回归 |
| ViewRoot 副本的 v.self 旧值 | get() 会覆盖 ✓ | 构造时同步 v.self = self | 代码审查 |
| 闭包捕获时 compact 正发生 | cn.h.get() 可能返回旧值 | init-capture 在 lambda 构造时求值，此时 cn 仍在栈上有效 | ASAN 压力测试 |
| 多线程闭包捕获 ViewRoot | ThreadLocal 跨线程不安全 ✗ | Global 跨线程安全 ✓ | 多线程用例 |
| viewRootTypes_ 缺失（填充遗漏） | — | genFunExpr 查询返回空字符串 → 生成代码编译错误（编译期发现） | 编译验证 |
| lambda 超 SBO（std::function 堆分配） | &val_ 天然稳定 | 转 Global 仍安全（relocateGlobalRootPtrs 无副作用） | 既有用例回归 |
| lambda SBO（栈上捕获量小） | &val_ 可能进堆（MapIter::fn_） | 转 Global，relocateGlobalRootPtrs 兜底 | 新增用例 + ASAN |

---

## 7. Test Plan

### 7.1 单元/集成

1. **新增用例：闭包捕获视图变量**
   ```
   let it = range(0, 10)
   let mapped = it.map([it](x) => x * 2)  # 闭包捕获视图变量 it
   # GC 压力：触发 mark + compact + promote
   force_gc()
   force_gc()
   # 验证 map 结果
   let result = collect_all(mapped)
   assert_eq(result.length, 10)
   assert_eq(result[0], 0)
   assert_eq(result[9], 18)
   ```

2. **ASAN 压力**：上述用例循环 100 次，每次 force_gc，验证无悬垂。

3. **多线程用例**：sync thread 中闭包捕获视图变量，验证跨线程安全。

4. **全量回归**：`compile.cmd` + test.aura（含既有迭代器用例 V1-V4）。

### 7.2 深度检测

按 AGENTS.md：清空 build 重建 ASAN 版 → 编译新增用例 → 运行捕获 stderr → 0 报告后切回普通模式。

### 7.3 回归风险区

- genFunExpr 捕获逻辑改动：所有闭包捕获路径（gcRootVarNames_ / viewRootVarNames_ / 普通变量）。
- viewRootTypes_ 填充遗漏：编译期发现（空类型名 → 编译错误）。
- genFunDecl/genMethodDecl 参数注册路径：viewRootTypes_ 填充与 viewRootVarNames_ 同步。

---

## 8. Implementation Steps（Ordered）

1. **新增 viewRootTypes_ map**（改动 C）：CodeGen.h 声明 + 3 处填充点同步 + 6 处 clear 点同步（含补齐 genFunDecl 末尾 L529、genConstructor 末尾 L833 两处状态泄漏）。
   → 产物：编译通过，既有用例不退步。
2. **ViewRoot 模板改造**（改动 A）：iterator.h 新增闭包捕获专用构造函数。
   → 产物：runtime 编译通过。
   > ⚠️ 共享改动：本步骤的构造 1（`explicit ViewRoot(T it, GcRootScope scope = GcRootScope::ThreadLocal)`）同时是 [plan/联合变体含接口isPtrActive钩子支持.md](file:///d:/you/Aura/plan/联合变体含接口isPtrActive钩子支持.md) 改动 D/E 的前置依赖（其装箱/match 分支用两参构造）。两个 plan 合并实施时，此步骤只需做一次。
3. **genFunExpr 改造**（改动 B）：ExprGen.cpp 新增 viewRootVarNames_ init-capture 分支。
   → 产物：编译通过，闭包捕获 ViewRoot 生成 Global 副本。
4. **新增用例 + 回归**：步骤 1 用例 + 全量回归。
5. **ASAN 深度检测**。
6. **完成**：TODO.txt [五] P2 标记 `[x]`。

**回滚**：每步均 `git restore` 对应文件即可，独立可回滚。

**依赖关系**：
- 步骤 1 必须先于步骤 3（genFunExpr 查询 viewRootTypes_）。
- 步骤 2 必须先于步骤 3（genFunExpr 生成 ViewRoot 构造调用）。
- 步骤 1、2 可并行（独立文件）。

---

## 9. Risks & Mitigations

| 风险 | 缓解 |
| ---- | ---- |
| viewRootTypes_ 填充遗漏 | genFunExpr 查询返回空 → 编译错误（编译期发现） |
| ViewRoot 副本的 v 字段不被 GC 扫描 | ViewRoot::get() 每次从 h.get() 恢复 self，不依赖 v.self 持久有效；这是 ViewRoot 设计的固有行为 |
| 闭包捕获时 compact 正发生 | init-capture 在 lambda 构造时求值，此时 cn 仍在栈上有效；lambda 构造后副本独立 |
| Global 根性能开销 | 仅闭包捕获时多一次 `globalRoots_m_` 锁；栈上使用无影响；与 gcRootVarNames_ 同机制 |
| ViewRoot 拷贝构造函数冲突 | 新增构造函数签名 `ViewRoot(T, GcObject*, GcRootScope)` 与既有 `explicit ViewRoot(T, GcRootScope=ThreadLocal)` 参数数量不同，无冲突 |
| 查询 key 与填充 key 不一致（C++ 关键字变量名） | 与 gcRootVarNames_ 既有问题一致，不引入新问题；未来可统一修正 |

---

## 10. 已知限制（本次不修复）

1. **ViewRoot 副本的 v 字段含视图值（含 self 指针）不被 GC 扫描**：ViewRoot 非 GcObject，GC 不扫描其字段。v.self 在 compact 后悬垂，但 ViewRoot::get() 会用 h.get() 覆盖，所以访问时安全。若用户代码直接访问 `v.self`（不通过 get()），会悬垂。当前 CodeGen 生成 `cn.get()` 访问 ViewRoot，不直接访问 `cn.v.self`，故不触发。**本次不修复，保持 ViewRoot 设计**。
2. **collect_all 内的 `it.self = guard.get()` 在 next() 内部触发 GC 时悬垂**：这是 ViewRoot 固有问题（collect_all L228），与本 issue 无关。collect_all 每次迭代开始恢复 self，next() 内部用到的 self 是参数传递的值（已拷贝），不受影响。**本次不修复**。
3. **方向 A（compact 层重定位链表 node）**：若未来出现"无法用 Global 替代的 ThreadLocal 进堆场景"（如 ThreadLocal 句柄直接存入 GC 堆对象成员，非闭包捕获路径），需走方向 A。当前无此场景，**本次不实现**。

> ~~4. genFunDecl/genConstructor 末尾缺 viewRootVarNames_.clear()~~：已确认是上轮 P1 落码的不对称遗漏（gcRootVarNames_ 6 处 vs viewRootVarNames_ 4 处），**本 plan §4.4 已修复**（清理点扩到 6 处）。不再记 TODO。
