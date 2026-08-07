# GC 稳健性三 issue 综合修复（tryAllocSlow 未 memset / match _match_val 跨 GC 保护 / threadRootLists node 重定位）— 详细实施方案

> 工作流：准备实现 plan（工作流 3）
> 提出时间：2026-08-07
> 状态：**待审查**（详细实施方案已就绪，源码行号经 2 个 Search Agent + 直接 Read 双重复核）
> 来源：[TODO.txt](file:///d:/you/Aura/TODO.txt) [五] GC 三 issue（L248-284）：
>   - [ ] P2 tryAllocSlow 路径未 memset
>   - [ ] P2 match 分支 _match_val 跨 GC 保护（Variant 对象指针悬垂）
>   - [ ] P3 方向 A：compact 层重定位 threadRootLists_ 链表 node
> 关联：[plan/compact全局根地址重定位修复.md](file:///d:/you/Aura/plan/compact全局根地址重定位修复.md)、[plan/threadRootLists节点compact重定位.md](file:///d:/you/Aura/plan/threadRootLists节点compact重定位.md)、[plan/联合变体含接口isPtrActive钩子支持.md](file:///d:/you/Aura/plan/联合变体含接口isPtrActive钩子支持.md)
> 审查：out.txt 审查报告 5 项修改点已全部落实（for-in 注册点描述 / L838-844 范围 / genFunExpr saved 机制 / gcTmpVar 成对 erase / 测试用例补充）

---

## 1. 元信息

| 项目 | 内容 |
| ---- | ---- |
| Plan 标题 | GC 稳健性三 issue 综合修复（alloc 慢路径清零 / match 跨 GC 保护 / threadRootLists node 重定位） |
| 相关模块 | `runtime/gc/alloc.cpp`、`src/CodeGen/StmtGen.cpp`、`src/CodeGen/CodeGen.h`、`src/CodeGen/DeclGen.cpp`、`src/CodeGen/ExprGen.cpp`、`example/test.aura` |
| 优先级 | issue 1 = P2（真实缺陷，当前未观察到崩溃）；issue 2 = P2（潜在缺陷，当前不触发）；issue 3 = P3（远期，当前零触发，仅设计留档） |
| 前置条件 | [x] compact 全局根重定位修复（relocateGlobalRootPtrs 已实现）；[x] threadRootLists node 修复方向 B（ViewRoot 转 Global 根，ExprGen.cpp:1615-1628 已落地） |

**实施顺序总览**：issue 1（单行，独立可先行）→ issue 2（4 步，含新成员）→ 测试 → §4.5 整合重构（独立）→ 全量回归。

---

## 2. Objectives

消除 GC 堆分配与 match 语句在跨 GC 窗口下的三处稳健性缺陷：

1. **tryAllocSlow 未 memset**（[alloc.cpp L120-174](file:///d:/you/Aura/runtime/gc/alloc.cpp#L120-L174)）：慢路径 bump 出的对象内存含旧对象残留（GC sweep 后保留旧页复用），字段初始化前 GC 触发时 markFields 读到无效指针 → SEGV。与 TLAB 快路径（L81）、tryAllocMedium（L389）、tryAllocLarge（L427）对齐补齐一行 memset。
2. **match `_match_val` 跨 GC 保护**（[StmtGen.cpp L1681](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1681)）：`auto&& _match_val = expr;` 无 GcRootHandle 保护，分支体内 alloc 触发 compact 后悬垂（Variant/Optional 堆指针）。用 Ref 模式 GcRootHandle 包裹，并同步保护非视图堆变体/堆元素 binding 的间接访问路径。
3. **threadRootLists_ node 重定位（方向 A）**：ThreadLocal 句柄进 GC 堆后链表 node（this）悬垂。当前闭包捕获路径已由方向 B 规避，本 plan 仅输出完整设计留档，不实施。

---

## 3. 调研结论确认（源码核实摘要）

> 以下行号经 2 个并行 Search Agent 检索 + 直接 Read 复核（2026-08-07），以当前仓库为准。

### 3.1 issue 1：tryAllocSlow 全函数无任何清零操作

- [alloc.cpp L119-173](file:///d:/you/Aura/runtime/gc/alloc.cpp#L119-L173)（函数签名 L119，结束 `}` L173）逐行核对：无 memset。内存清零完全依赖 `bumpAlloc → allocPage`（L205 `std::memset(page->data, 0, kPageSize)`）对**新页**的初始清零。
- **脏数据机制**：GC 后含存活对象的旧页被保留（mark_sweep / compact.cpp rebuildPageList），`currentPage_ = 保留页链尾`，`Page::bumpOffset` 未重置 → 下次 bumpAlloc 从旧页 data[0] 分配，新对象内存 = 已死亡对象残留。该对象注册为根后、字段初始化前嵌套 alloc 触发 GC → markFields 读脏指针 → SEGV。
- 对比三处已修复：TLAB 快路径 L81（Bug 6 修复，注释详述同机制）、tryAllocMedium L389、tryAllocLarge L427。三处均用对齐后 `size`。
- `size` 语义：tryAlloc L46 `size = (size + 7) & ~size_t(7);` 已 8 字节对齐，= header + payload 总大小；tryAllocSlow 收到的 size 即对齐后值。
- **单点修复覆盖全部小页场景**：所有小页分配路径（首次 / TLAB 满 / gcPending_ 慢路径 / compact 后）都汇聚到 tryAllocSlow。

### 3.2 issue 2：genMatchStmt 全函数核实

- **函数范围**：[StmtGen.cpp L1655-1852](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1655-L1852)。
- **expr 生成**：L1657 `std::string expr = genExpr(*stmt.expr, isCoroutine);`，返回 C++ 表达式文本。
- **`_match_val` 唯一定义处**：L1681 `writeLine(cpp, "auto&& _match_val = " + expr + ";");`。此后 19 处引用（L1690/1693/1697/1719/1720/1727/1728/1735/1736/1740/1741/1761/1768/1778/1786/1788/1791/1794/1799）全部是字符串字面量拼接 → **包裹后变量名不变则零改动**。`_match_val` 生命周期与外层块 L1679 `{` / L1851 `}` 严格绑定。
- **局部变量**：`isVariantPtr`（L1664，UnionSemType 含任一堆变体）、`isOptional`（L1665，OptionalSemType）；分支优先级 isVariantPtr > isOptional > 全值 std::variant > 普通类型。
- **非视图堆变体 binding**（L1777-1779）：
  ```cpp
  binding = "auto& " + varName + " = _match_val->get<" + std::to_string(idx) + ">();";
  ```
- **Optional binding**（L1785-1788）：
  ```cpp
  cond = "!_match_val->is_none()";
  if (!tp->varName.empty())
      binding = "auto " + safeName(tp->varName) + " = _match_val->unwrap();";
  ```
- **viewTmpVar 清理**（L1840-1843）：`viewRootVarNames_.erase(viewTmpVar); valueTypeVarNames_.erase(viewTmpVar);`（只清 2 集合，**未清 viewRootTypes_**——既有疏漏，新代码不得重蹈）。
- **`matchCounter_` 不存在**：CodeGen.h 现有 4 计数器 L492-495（listCounter_/recordAllocCounter_/argHandleCounter_/unionBoxingCounter_），新成员参照 unionBoxingCounter_（L495）风格。
- **Optional<T> 同为 GC 堆对象**（optional.h L25 `struct Optional : GcObject`）→ `_match_val` 为 `Optional<T>*` 时风险与 Variant 相同。

### 3.3 issue 3：方向 B 已闭环，方向 A 仅留档

- 闭包捕获 GC 根 → Global（ExprGen.cpp L1615-1620）；闭包捕获视图 → ViewRoot Global（L1621-1628）。栈上 ThreadLocal 句柄均在栈/静态区，compact 不移动 → 当前零触发。
- 方向 A 需重连双向链表（node 自身 next_/prev_/ptr_ref_ + 外部 3 处指向），顺序耦合强，成本高于收益（threadRootLists节点compact重定位.md §4.1 已论证）。

### 3.4 §4.5 整合重构：源码现状（直接 Read 复核）

- **6 处成组 clear**（6 集合：valueTypeVarNames_/stringVarNames_/gcRootVarNames_/gcRootTypes_/viewRootVarNames_/viewRootTypes_）：
  - L183-190（接口默认方法体后，L183 先 `currentReceiverName_.clear()` 特殊字段）
  - L431-436（genFunDecl 体前，纯成组）
  - L529-535（genFunDecl 体后，L535 `currentReturnElem_.clear()` 特殊字段）
  - L674-679（genMethodDecl 体前，纯成组）
  - L782-790（genMethodDecl 体后，L782 `currentReceiverName_.clear()`、L790 `currentReturnElem_.clear()` 特殊字段）
  - L838-844（genConstructor 体后，L844 `currentTParams_.clear()` 特殊字段）
- **参数注册**：
  - genFunDecl L437-476：4 分支（NamedType 值类型 / string / 接口视图+接口 / GC 指针）
  - genMethodDecl L687-713：3 分支（string / 接口视图+接口 / GC 指针）——**缺"值类型 NamedType"分支**
  - genFunExpr L1699-1716：3 分支（string / 接口 / 值类型 NamedType），saved/restore 机制（L1697-1698）
- **Param 定义**：[Stmt.h L16-20](file:///d:/you/Aura/src/AST/Stmt.h#L16-L20) `{name, type, defaultExpr}`。

---

## 4. 详细实施方案（修改前 → 修改后）

### 4.1 issue 1：tryAllocSlow 补 memset（alloc.cpp）

**位置**：[alloc.cpp L146](file:///d:/you/Aura/runtime/gc/alloc.cpp#L145-L147)（`throwOutOfMemory()` 检查后、对象头初始化前）。

**修改前**：
```cpp
    if (!mem) {
        // GC 后仍失败 → 抛出预缓存的 OutOfMemoryError
        throwOutOfMemory();
    }

    GcObject* obj = static_cast<GcObject*>(mem);
```

**修改后**：
```cpp
    if (!mem) {
        // GC 后仍失败 → 抛出预缓存的 OutOfMemoryError
        throwOutOfMemory();
    }

    // 与 TLAB 快路径（L81）/ tryAllocMedium（L389）/ tryAllocLarge（L427）对齐：
    // GC sweep 后保留旧页被 bumpAlloc 复用（currentPage_ 可能指向保留旧页），
    // 不清零则新对象指针字段含旧对象残留 → 字段初始化前 GC 触发 → markFields 读无效指针 → SEGV
    std::memset(mem, 0, size);

    GcObject* obj = static_cast<GcObject*>(mem);
```

**接口契约**：`size` 为 8 字节对齐后的 header+payload 总大小（tryAlloc L46 已对齐）；memset 与现有 3 处模式完全一致。

### 4.2 issue 2：match 跨 GC 保护（StmtGen.cpp + CodeGen.h）

#### 4.2.1 CodeGen.h 新增计数器（前置）

**位置**：[CodeGen.h L495](file:///d:/you/Aura/src/CodeGen/CodeGen.h#L492-L495)（unionBoxingCounter_ 之后）。

**修改前**：
```cpp
    int unionBoxingCounter_ = 0;  // P3b 隐式装箱临时变量名计数器
```

**修改后**：
```cpp
    int unionBoxingCounter_ = 0;  // P3b 隐式装箱临时变量名计数器
    int matchCounter_ = 0;        // match 分支 binding 保护 GcRootHandle 临时变量名计数器
```

#### 4.2.2 `_match_val` 用 Ref 模式 GcRootHandle 包裹（核心）

**位置**：[StmtGen.cpp L1681](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1679-L1682)。

**修改前**：
```cpp
    cpp << indentStr() << "{\n";
    indentLevel_++;
    writeLine(cpp, "auto&& _match_val = " + expr + ";");
```

**修改后**：
```cpp
    cpp << indentStr() << "{\n";
    indentLevel_++;
    if (isVariantPtr || isOptional) {
        // 与 genLetStmt L428-432 / genUnionBoxingImpl L189-190 对齐：Ref 模式（T& 构造）
        // GC compact 经 ptr_ref_ 直接更新 _match_val 变量本身，后续 _match_val->... 拼接零改动
        writeLine(cpp, "auto _match_val = " + expr + ";");
        writeLine(cpp, "aura_rt::GcRootHandle<decltype(_match_val)> _match_rh(_match_val);");
    } else {
        // 全值 std::variant / 普通类型：无 GC 指针跨栈窗口，保持现状 auto&&（避免拷贝）
        writeLine(cpp, "auto&& _match_val = " + expr + ";");
    }
```

**要点**：
- `auto&&` → `auto`：`expr` 为右值（`var.get()` 临时指针）时拷贝指针值，使 `_match_val` 成为独立栈变量，Ref 模式 `ptr_ref_` 才能稳定指向它。Variant/Optional 指针拷贝 8 字节，零成本。
- 固定名 `_match_rh`：每个 match 语句自带 `{` 块作用域（L1679），嵌套 match 在不同作用域同名不冲突；无需计数器。
- **不注册 `gcRootVarNames_`**：`_match_val` 19 处使用点全为手写字符串拼接，不走 genIdentifier；注册反而引入闭包捕获转 Global 副作用。
- 生命周期：`_match_rh` 在 L1851 外层块结束析构（unregisterRootThreadLocal），覆盖生成到最后一个使用点全部窗口。
- 全部 20 处 `_match_val` 引用零改动（唯一定义处 L1681 的形态变化不影响后续拼接）。

#### 4.2.3 非视图堆变体 binding 保护（间接访问路径 1）

**位置**：[StmtGen.cpp L1777-1780](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1777-L1780)（`} else {` 分支，含闭合 `}` L1780）。`varName` 已在外层 L1763 声明（L1762 为 `if (!tp->varName.empty())` 守卫），`idx` 已解析，可直接使用。

**修改前**：
```cpp
                        } else {
                            binding = "auto& " + varName + " = _match_val->get<"
                                      + std::to_string(idx) + ">();";
                        }
```

**修改后**：
```cpp
                        } else {
                            // 4.2.3：值拷贝 +（堆指针变体）GcRootHandle 包裹
                            // 原 auto& 为 Variant storage 槽位引用，分支体内 alloc 后悬垂
                            binding = "auto " + varName + " = _match_val->get<"
                                      + std::to_string(idx) + ">();";
                            if (isGcPointerType(cppType)) {
                                std::string mhName = "_mh" + std::to_string(matchCounter_++);
                                binding += " aura_rt::GcRootHandle<decltype(" + varName
                                           + ")> " + mhName + "(" + varName + ");";
                                gcRootVarNames_.insert(varName);
                                gcRootTypes_[varName] = "decltype(" + varName + ")";
                                gcTmpVars.push_back(varName);
                            }
                        }
```

**要点**：
- `isGcPointerType(cppType)`（TypeMap.cpp L32-39 现有函数）：`GcString*`/`Point*`/`Array<T>*`/`Optional<T>*` 等以 `*` 结尾 → 包裹；`int`/`float`/`bool`/`NoneType` 等 → 仅拷贝。此分支的 cppType 已排除接口视图（上层 if 已拦截）。
- 注册 `gcRootVarNames_` + `gcRootTypes_` 使 genIdentifier 对分支体内 varName 引用生成 `.get()`（[ExprGen.cpp L243-245](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L243-L245)）；`gcRootTypes_` 用 `decltype(varName)`（binding 无 `_raw`，与 decl 的 `decltype(varName_raw)` 模式对齐）。
- `gcTmpVars` 为 genMatchStmt 局部 `std::vector<std::string>`（见 4.2.5），分支体结束统一成对 erase（防泄漏，审查项 4）。

#### 4.2.4 Optional 堆元素 binding 保护（间接访问路径 2）

**位置 a（元素堆判定提取）**：[StmtGen.cpp L1672-1674](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1663-L1674)（OptionalSemType 分支，含闭合 `}` L1674）。

**修改前**：
```cpp
    } else if (dynamic_cast<const OptionalSemType*>(mt)) {
        isOptional = true;
    }
```

**修改后**：
```cpp
    } else if (dynamic_cast<const OptionalSemType*>(mt)) {
        isOptional = true;
        // 4.2.4：提取 Optional 元素堆判定（binding 保护用）
        // isHeapSemType 为成员函数：定义于 ExprGen.cpp L12，声明于 CodeGen.h L257（跨文件调用无障碍）
        if (auto* os = dynamic_cast<const OptionalSemType*>(mt)) {
            elemCppType = mapSemType(*os->elementType);
            elemIsHeap = isHeapSemType(os->elementType.get());
            if (!elemIsHeap && !elemCppType.empty() && elemCppType.back() == '*')
                elemIsHeap = true;  // C++ 名以 * 结尾回退判定
        }
    }
```

**位置 b（新局部变量声明）**：[StmtGen.cpp L1666](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1664-L1667)（`variantCppTypes` 声明之后、`if (auto* u = ...)` 之前；L1664 isVariantPtr / L1665 isOptional / L1666 variantCppTypes）。

**修改前**（注：L1666 为 `variantCppTypes` 声明，新变量插在 L1666 之后、L1667 `if` 之前）：
```cpp
    bool isVariantPtr = false;   // aura_rt::Variant<T...>*
    bool isOptional   = false;   // aura_rt::Optional<T>*
    std::vector<std::string> variantCppTypes;  // 各变体 C++ 类型（索引对应；std::variant 与 Variant 路径共用）
```

**修改后**：
```cpp
    bool isVariantPtr = false;   // aura_rt::Variant<T...>*
    bool isOptional   = false;   // aura_rt::Optional<T>*
    std::string elemCppType;                 // Optional 元素 C++ 类型（4.2.4）
    bool elemIsHeap = false;                 // Optional 元素是否为堆类型（4.2.4）
    std::vector<std::string> gcTmpVars;      // 本分支临时注册的 GC 根变量名（4.2.3/4.2.4 清理）
```

**位置 c（Optional binding）**：[StmtGen.cpp L1785-1788](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1785-L1788)。

**修改前**：
```cpp
            } else if (isOptional) {
                cond = "!_match_val->is_none()";
                if (!tp->varName.empty())
                    binding = "auto " + safeName(tp->varName) + " = _match_val->unwrap();";
```

**修改后**：
```cpp
            } else if (isOptional) {
                cond = "!_match_val->is_none()";
                if (!tp->varName.empty()) {
                    // 4.2.4：值拷贝 +（堆元素）GcRootHandle 包裹
                    // 原裸指针拷贝在分支体内 alloc 后悬垂；值元素仅拷贝不包裹
                    std::string varName = safeName(tp->varName);
                    binding = "auto " + varName + " = _match_val->unwrap();";
                    if (elemIsHeap) {
                        std::string mhName = "_mh" + std::to_string(matchCounter_++);
                        binding += " aura_rt::GcRootHandle<decltype(" + varName
                                   + ")> " + mhName + "(" + varName + ");";
                        gcRootVarNames_.insert(varName);
                        gcRootTypes_[varName] = "decltype(" + varName + ")";
                        gcTmpVars.push_back(varName);
                    }
                }
```

#### 4.2.5 分支体结束清理（gcTmpVar 成对 erase）

**位置**：[StmtGen.cpp L1838-1843](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1838-L1843)（viewTmpVar 清理之后）。

**修改前**：
```cpp
        // P2b：接口视图分支的临时 viewRootVarNames_ 注册，分支体生成完毕后移除
        // （嵌套闭包捕获 varName 的分支体内仍能查到，走 genFunExpr 的 Global 转换）
        if (!viewTmpVar.empty()) {
            viewRootVarNames_.erase(viewTmpVar);
            valueTypeVarNames_.erase(viewTmpVar);
        }
```

**修改后**：
```cpp
        // P2b：接口视图分支的临时 viewRootVarNames_ 注册，分支体生成完毕后移除
        // （嵌套闭包捕获 varName 的分支体内仍能查到，走 genFunExpr 的 Global 转换）
        if (!viewTmpVar.empty()) {
            viewRootVarNames_.erase(viewTmpVar);
            valueTypeVarNames_.erase(viewTmpVar);
        }
        // 4.2.3/4.2.4：清理本分支临时注册的 GC 根（成对 erase 两个集合，防泄漏）
        for (auto& v : gcTmpVars) {
            gcRootVarNames_.erase(v);
            gcRootTypes_.erase(v);
        }
        gcTmpVars.clear();
```

**要点**：注册时成对 insert（`gcRootVarNames_` + `gcRootTypes_`），清理时必须成对 erase——漏 erase `gcRootVarNames_` → 分支体外同名变量被误判已注册根（genIdentifier 生成 `.get()` 编译失败）；漏 erase `gcRootTypes_` → 状态残留污染后续分支。viewTmpVar L1840-1843 只清 2 集合未清 `viewRootTypes_` 是既有疏漏，本新增代码不得重蹈。

#### 4.2.6 不动的部分

- 接口视图变体分支（L1764-1776 ViewRoot 路径）：已由 change.md 步骤 7 保护，保持不动。
- 全值 `std::variant`（L1789-1794）与普通类型（L1795-1799）路径：无 GC 指针跨栈窗口，保持 `auto&&` / 引用绑定。
- genConstCond lambda（L1684-1742）与 TypePattern 条件生成：`_match_val` 变量名不变，全部零改动。

### 4.3 issue 3：threadRootLists_ node 重定位（方向 A 设计留档）

> 决策：**本 plan 不实施方向 A，仅输出完整设计**。理由：当前零触发（方向 B 已闭环）；双向链表重连顺序耦合复杂；threadRootLists节点compact重定位.md §4.1 已论证成本高于收益。

**设计（供未来触发场景实施）**：

1. **快照点**：`updateAllReferences` 步骤 1（compact.cpp L308-315）与 `updateMediumPageReferences`（L728-733）遍历 threadRootLists_ 时——此阶段旧地址仍可安全 deref（小页在 memcpy 前；中页/大页在旧页进池前）。对每个 node 判定 `node ∈ [oldAddr, oldAddr+size)`（compactEntries_ 二分 / forwardMap 线性），记录 `{list, node, newNode, node->prev_, node->next_, isHead}`。
2. **重连点**（memcpy 后、旧页释放前）：新增 `relocateThreadRootNodes()`，与 `relocateGlobalRootPtrs`（L416-445）同锁域（STW 期间）：
   - 重定位前驱/后继地址（若也在移动区间，各自 +off）；
   - `list->head == node → newNode`；`prev->next_ = newNode`；`next->prev_ = newNode`；
   - 重写 newNode 的 `next_`/`prev_` 为重定位后值；
   - 同步改写 newNode 的 `ptr_ref_` 为 `newNode + off`（与 `kGcHandlePtrRefValDelta` 同思路，保证析构 `unregisterRootThreadLocal(this)` 与遍历一致性）。
3. **中页/大页同路径**：`relocateRootsInForwardMap` 调用处（compact.cpp L702 / **L881**，L880 为注释行）相邻新增同逻辑。

### 4.4 测试用例（test.aura）

- **issue 2 用例 A（非视图堆变体分支内 GC，间接路径 1）**：在 P2.1 区域（u4y 附近）新增：

```
// U5: match 非视图堆变体分支内 gc_force + 字符串拼接 alloc 后 binding 仍有效（issue 2 间接路径 1）
let u5s: int | string = "hi"
match u5s {
    int u5i => io.println("p2.1 u5 = int")
    string u5t => {
        gc_force()
        let u5x = "x" + "y"        // 字符串拼接 alloc：确保 gc_force 后还有真实堆分配触发 compact 搬移
        gc_force()
        io.println("p2.1 u5 = " + str(u5t.length))
    }
}
```

> 注：`let u5x` 仅在分支体内构造丢弃的字符串（无输出），作用是在第二次 `gc_force()` 前制造真实 alloc 压力——若只调 `gc_force()`，GC 后堆页空闲充足时 compact 可能不搬移对象，binding 悬垂无法暴露。`u5t` binding 在分支体末尾读取，覆盖"分支体全程存活"窗口。

- **issue 2 用例 B（Optional 堆元素分支内 GC，间接路径 2）**：在 P5 match 值模式区域（m8/m9 附近）新增：

```
// m10: Optional<string> match 分支内 gc_force 后 unwrap 绑定仍有效（issue 2 间接路径 2）
let m10o: Optional<string> = "abc"   // 隐式装箱（Aura 无 optional() 函数；some("abc") 有字符串 CTAD 陷阱，故用隐式装箱）
match m10o {
    None => io.println("m10 none")
    string m10s => {
        gc_force()
        let m10x = "x" + "y"       // 同上：制造真实 alloc 压力
        io.println("m10 s len = " + str(m10s.length))
    }
}
```

- **issue 2 用例 C（嵌套 match 边界）**：验证外层 `_match_rh` 与 `_mh{counter}` 在嵌套 match 中互不干扰（块作用域遮蔽 + `matchCounter_` 唯一性）：

```
// U6: 嵌套 match——内层分支 gc_force 后，外层 binding 与内层 binding 均仍有效
let u6s: int | string = "world"
match u6s {
    int u6i => io.println("p2.1 u6 = int")
    string u6t => {
        let u6o: Optional<string> = u6t   // 隐式装箱（u6t 为 GcString*，DeclGen P3a 折叠生成 make_optional<GcString*>）
        match u6o {
            None => io.println("p2.1 u6 inner none")
            string u6inner => {
                gc_force()
                io.println("p2.1 u6 inner = " + str(u6inner.length) + " outer = " + str(u6t.length))
            }
        }
    }
}
```

- **issue 2 用例 D（Optional 值元素边界）**：验证 `Optional<int>` 元素为值类型时 `elemIsHeap=false` 不包裹路径（仅拷贝，不生成 `_mh` handle）：

```
// m11: Optional<int> match 值元素分支内 gc_force（值元素不包裹，验证非堆路径）
let m11o: Optional<int> = 7   // 隐式装箱（生成 make_optional<int32_t>）
match m11o {
    None => io.println("m11 none")
    int m11i => {
        gc_force()
        io.println("m11 i = " + str(m11i))
    }
}
```

- **issue 1**：无直接触发测试（需"旧页复用 + 字段初始化前 GC"精确时序）；靠全量回归 + 可选 ASAN 压力测试（`gc_force` 循环 + 大量 alloc）验证不崩。
- **issue 3**：无测试（不实施）。

### 4.5 状态清理与参数注册整合（DeclGen / ExprGen）

**背景**：函数级状态管理散落多处，"新增一个跟踪集合要改 6 处清理 + 3 处注册"极易遗漏（历史上 viewRootVarNames_ 漏过 2 处 clear，见 DeclGen.cpp L533 P2b 注释）。

#### 4.5.1 新增 `clearVarTrackingState()`

**CodeGen.h**（成员函数声明，放 public 区域，靠近 six 集合声明）：
```cpp
    // 清理函数级变量跟踪状态（6 处调用点统一；新增跟踪集合时必须同步此处）
    void clearVarTrackingState();
```

**DeclGen.cpp**（实现，放在 genFunDecl 之前）：
```cpp
void CodeGenerator::clearVarTrackingState() {
    valueTypeVarNames_.clear();
    stringVarNames_.clear();
    gcRootVarNames_.clear();
    gcRootTypes_.clear();
    viewRootVarNames_.clear();
    viewRootTypes_.clear();
}
```

**6 处替换**（每处用 `clearVarTrackingState();` 替换成组 6 行；特殊字段保留原位）：

| 位置 | 特殊字段（保留原位） | 替换内容 |
| ---- | ---- | ---- |
| L183-190 | L183 `currentReceiverName_.clear()` | 仅 L185-190 6 行 → 1 调用 |
| L431-436 | 无 | L431-436 6 行 → 1 调用 |
| L529-535 | L535 `currentReturnElem_.clear()` | 仅 L529-534 6 行 → 1 调用 |
| L674-679 | 无 | L674-679 6 行 → 1 调用 |
| L782-790 | L782 `currentReceiverName_.clear()`、L790 `currentReturnElem_.clear()` | 仅 L784-789 6 行 → 1 调用 |
| L838-844 | L844 `currentTParams_.clear()` | 仅 L838-843 6 行 → 1 调用 |

**特殊字段保留原位**：`currentReceiverName_` / `currentReturnElem_` / `currentTParams_` / `currentLetName_`（StmtGen L312/L371）非成组出现、语义各自独立，不合并。

#### 4.5.2 新增 `registerParamTracking(const Param& p)`（公共 3 类，无 _raw）

**CodeGen.h**：
```cpp
    // 参数类型跟踪注册（3 处共有：string / 用户接口 / 值类型 NamedType；不含 _raw 语义）
    void registerParamTracking(const Param& p);
```

**DeclGen.cpp 实现**（逻辑 = genFunExpr L1699-1716 三分支 + genFunDecl 分支 1）：
```cpp
void CodeGenerator::registerParamTracking(const Param& p) {
    if (!p.type) return;
    std::string ptype = mapType(*p.type);
    // string 参数 → stringVarNames_
    if (ptype.find("aura_rt::GcString*") != std::string::npos)
        stringVarNames_.insert(p.name);
    // 接口参数 → valueTypeVarNames_（引用用 . 不是 ->）
    if (auto* nt = dynamic_cast<const NamedType*>(p.type.get()))
        if (interfaceNames_.count(nt->name))
            valueTypeVarNames_.insert(p.name);
    // 值类型 NamedType → valueTypeVarNames_（registeredTypes_ 非堆 / BuiltinRegistry 非堆）
    if (auto* nt = dynamic_cast<const NamedType*>(p.type.get()))
        if ((registeredTypes_.count(nt->name) && !registeredTypes_[nt->name])
            || (BuiltinRegistry::get().findType(nt->name) != nullptr
                && !BuiltinRegistry::get().isHeapType(nt->name)))
            valueTypeVarNames_.insert(p.name);
}
```

**调用点替换**：
- genFunExpr L1699-1716 的 for 循环体 → `registerParamTracking(p);`（**保留** L1697-1698 `savedStringVars`/`savedValueVars` 保存/恢复两行，不得把 saved/restore 放进公共函数——lambda 参数作用域限制语义）；
- genFunDecl L437-476 与 genMethodDecl L687-713 的参数循环 → `registerParamTracking(p);` + `registerRawParamTracking(p);`（见 4.5.3）。

#### 4.5.3 新增 `registerRawParamTracking(const Param& p)`（DeclGen 特有，`_raw` 语义）

**CodeGen.h**：
```cpp
    // decl 函数参数特有（签名已生成 varName_raw，函数体入口 GcRootHandle/ViewRoot 包裹）：
    // 接口视图参数 → viewRootVarNames_ + viewRootTypes_[decltype(_raw)]；
    // GC 指针参数 → gcRootVarNames_ + gcRootTypes_[decltype(_raw)]
    void registerRawParamTracking(const Param& p);
```

**DeclGen.cpp 实现**（逻辑 = genFunDecl L452-475 与 genMethodDecl L695-711 完全相同部分）：
```cpp
void CodeGenerator::registerRawParamTracking(const Param& p) {
    if (!p.type) return;
    std::string ptype = mapParamType(*p.type);
    if (isIfaceViewTypeName(ptype)) {
        valueTypeVarNames_.insert(p.name);
        viewRootVarNames_.insert(p.name);
        viewRootTypes_[p.name] = "decltype(" + safeName(p.name) + "_raw)";
    } else if (auto* nt = dynamic_cast<const NamedType*>(p.type.get())) {
        if (interfaceNames_.count(nt->name))
            valueTypeVarNames_.insert(p.name);
    }
    if (isGcPointerType(ptype)) {
        std::string varName = safeName(p.name);
        gcRootVarNames_.insert(varName);
        gcRootTypes_[varName] = "decltype(" + varName + "_raw)";
    }
}
```

**genFunDecl / genMethodDecl 参数循环最终形态**：
```cpp
    for (auto& p : decl.params) {
        registerParamTracking(p);
        registerRawParamTracking(p);
    }
```

**不属参数注册、保留原位的注册点**（审查修正，勿误改）：
- genMethodDecl 的 receiver 判断（L682-685）；
- StmtGen try-catch **catch 变量**注册（L961 / L1026 / L1050，`valueTypeVarNames_.insert(cv)`，分别位于 genTryCatch / genTryCatchNoSetupIIFE / genTryCatchRaw，insert 后紧跟 erase）；
- **真正的 for-in 迭代变量注册**在 genForStmt L703（`IterVarGuard` RAII 注册到 `gcRootVarNames_`/`gcRootTypes_`，非 valueTypeVarNames_）；spawn 闭包参数（L1346-1348）同用 IterVarGuard。

**收益**：新增跟踪集合时只需改 `clearVarTrackingState()` + 注册函数一处；三处重复逻辑收敛为 2 个公共函数。

---

## 5. 接口契约汇总

| 新符号 | 声明位置 | 语义 | 调用点 |
| ---- | ---- | ---- | ---- |
| `int matchCounter_` | CodeGen.h L495 后 | match binding 保护临时变量名计数器 | genMatchStmt 4.2.3/4.2.4 |
| `clearVarTrackingState()` | CodeGen.h + DeclGen.cpp | 6 集合成组 clear | DeclGen.cpp 6 处 |
| `registerParamTracking(const Param&)` | CodeGen.h + DeclGen.cpp | string/接口/值类型注册（无 _raw） | genFunDecl / genMethodDecl / genFunExpr |
| `registerRawParamTracking(const Param&)` | CodeGen.h + DeclGen.cpp | 接口视图 + GC 指针注册（_raw 语义） | genFunDecl / genMethodDecl |
| `std::vector<std::string> gcTmpVars` | genMatchStmt 局部 | 分支临时 GC 根变量名收集 | 4.2.3/4.2.4 注册 + 4.2.5 清理 |
| `elemCppType` / `elemIsHeap` | genMatchStmt 局部 | Optional 元素堆判定 | 4.2.4 |

运行时接口无任何变化（memset 内部行为；GcRootHandle/ViewRoot 均为既有 API）。

---

## 6. 实施步骤（Ordered，含依赖）

| 步骤 | 内容 | 依赖 | 产出 | 验证 |
| ---- | ---- | ---- | ---- | ---- |
| 1 | issue 1：alloc.cpp L146 补 `std::memset(mem, 0, size);` | 无 | 单行改动 | `cmake --build runtime/build` 编译 |
| 2 | issue 2 前置：CodeGen.h 新增 `int matchCounter_ = 0;` | 无 | 成员声明 | 编译 |
| 3 | issue 2 核心：StmtGen.cpp 4.2.2 `_match_val` Ref 模式包裹 | 步骤 2 | 生成代码变化 | 编译 + 目视 test.cpp `_match_rh` |
| 4 | issue 2：4.2.3 非视图堆变体 binding + 4.2.5 清理（含 gcTmpVars/elem 局部变量） | 步骤 2 | 生成代码变化 | 编译 + 目视 test.cpp `_mh{N}` |
| 5 | issue 2：4.2.4 Optional binding + 元素堆判定 | 步骤 2 | 生成代码变化 | 编译 + 目视 test.cpp |
| 6 | test.aura 新增 u5 / m10 / u6 / m11（§4.4） | 步骤 3-5 | 测试代码 | 编译 |
| 7 | 全量回归：compile.cmd + test.exe | 步骤 6 | 回归报告 | 新增输出 + ALL TESTS PASSED |
| 8 | §4.5 整合重构：3 公共函数 + 6 处替换 + 3 处注册替换 | 无（独立） | 重构代码 | 编译 + 全量回归 + 生成 test.cpp diff |
| 9 | 可选：ASAN 深度验证（issue 1） | 步骤 1 | ASAN 通过 | ASAN 报告 |
| 10 | 文档同步：TODO.txt 标记 issue 1/2 完成；issue 3 保留开放（附 §4.3 设计引用） | 全部 | TODO 更新 | 审查 |

**依赖关系**：步骤 3/4/5 依赖步骤 2（matchCounter_）；步骤 6/7 依赖步骤 3-5；步骤 8 与 1-5 无依赖可并行；步骤 9/10 最后。

**可能遇到的问题与应对**：
- 步骤 4 中 `isGcPointerType(cppType)` 对 `Array<T>*` 等模板指针类型的判定：函数按 `*` 后缀匹配（TypeMap.cpp L32-39），已覆盖；若发现漏判（如 `aura_rt::Variant<...>*` 变体嵌套），需补充后缀判定。
- 步骤 5 中 `mapSemType(*os->elementType)` 对泛型元素（如 `Optional<Array<T>>`）返回带模板参数类型，`*` 后缀判定仍有效；`isHeapSemType` 递归判定已覆盖容器。
- 步骤 8 重构后若生成代码 diff 出现非预期差异，按 §9 风险表逐项排查（重点 genMethodDecl 值类型注册补强）。

**回滚**：步骤 1 单行可逆；步骤 2-5 回滚 = 恢复 StmtGen.cpp/CodeGen.h 改动 + 移除新增用例；步骤 8 回滚 = 恢复 DeclGen.cpp/ExprGen.cpp/CodeGen.h 公共函数改动；所有步骤互不依赖，可独立回滚。

---

## 7. 影响分析

| 影响面 | issue 1 | issue 2 | issue 3 | §4.5 整合重构 |
| ---- | ---- | ---- | ---- | ---- |
| 编译器生成代码 | 无 | `_match_val` 形态变化（auto&&→auto + _match_rh）+ binding 形态变化（auto&→auto，堆指针变体多 _mh handle） | 无（不实施） | 生成代码应完全等价（纯重构；唯一例外：genMethodDecl 补值类型 NamedType 注册） |
| runtime GC | alloc.cpp 慢路径多一次 memset | 无 | compact.cpp/roots.cpp（未来） | 无 |
| 构建系统 | 无 | 无 | 无 | 无（仅 3 个源文件内改动） |
| BREAKING | 无 | binding 从引用改值拷贝：分支体内对 binding 取地址/引用语义变化（Aura 源码不可直接取地址，实际无影响）；GcString* 等指针语义不变 | — | 无 BREAKING；genMethodDecl 值类型注册为行为补强 |
| 兼容性 | 向下兼容 | 向下兼容（仅生成代码形态变化） | — | 向下兼容（重构后全量回归 + test.cpp diff 验证） |

---

## 8. 边界条件处理策略

| 边界条件 | 当前处理 | 计划处理 | 测试策略 |
| ---- | ---- | ---- | ---- |
| tryAllocSlow 首次分配（新页） | allocPage 已清零 | 防御性统一 memset（覆盖全部路径） | 全量回归 |
| tryAllocSlow TLAB 满 / gcPending_ / compact 后（保留旧页） | **脏数据 → SEGV 风险** | memset 清零 | 全量回归 + ASAN 压力 |
| 中页 freeMediumPages_ / 大页 sweepLargePages 复用 | 已修（L389/L427） | 不动 | 既有回归 |
| LOS >256KB | 无复用无需 | 不动 | — |
| match `_match_val` 为 `Optional<T>*` | 无保护悬垂风险 | Ref 模式包裹（与 Variant 同） | 用例 B（m10） |
| match 全值 std::variant / 普通类型 | 无 GC 指针无风险 | 保持 `auto&&` | 既有 m1-m9 |
| 非视图堆变体 binding（值类型 int 等） | 无风险 | 仅拷贝不包裹 | 用例 A int 分支 / u5i |
| 非视图堆变体 binding（GcString* 等堆指针） | **storage 引用悬垂** | 值拷贝 + GcRootHandle | 用例 A（u5） |
| Optional 值元素（int 等） | 值拷贝安全 | 不包裹 | 用例 D（m11） |
| Optional 堆元素（GcString* 等） | **裸指针拷贝悬垂** | 值拷贝 + GcRootHandle | 用例 B（m10） |
| 嵌套 match（内外层 `_match_val`/`_mh` 同名） | 块作用域隔离 + matchCounter_ | 同左 | 用例 C（u6） |
| 分支体内闭包捕获 binding 变量 | gcRootVarNames_ 注册后自动转 Global | 已由 ExprGen L1615-1620 覆盖 | 既有闭包用例 |
| 分支体内对 binding 变量重新赋值 | 原引用语义可写回 storage | 值拷贝不写回（match binding 只读语义，Aura 无写回用例） | 评审确认 |
| threadRootLists_ node 进堆（方向 A 触发场景） | **悬垂 → 链表遍历崩溃** | 本 plan 不实施；设计见 §4.3 | 触发场景出现后再测 |

---

## 9. 测试计划

1. **单元/编译验证**：编译 test.aura（非 ASAN 模式，`example/compile.cmd`），检查生成 test.cpp 中 `_match_rh` 包裹、`_mh{N}` binding 包裹形态。
2. **集成测试**：运行 test.exe，验证：
   - 既有 u1-u4（联合变体含接口）不回归；
   - 新增 u5 输出 `p2.1 u5 = 2`（"hi".length）；
   - 新增 m10 输出 `m10 s len = 3`；
   - 新增 u6（嵌套 match）输出 `p2.1 u6 inner = 5 outer = 5`；
   - 新增 m11（Optional<int>）输出 `m11 i = 7`；
   - 既有 m1-m9、u5-u8（P4/P5）不回归；
   - 末尾 `ALL TESTS PASSED`。
3. **边界用例**：`int | string` 的 int 分支（值类型不包裹路径）、Optional<int>（m11，值元素不包裹路径）、嵌套 match（u6，`_match_rh`/`_mh{N}` 遮蔽与唯一性）。
4. **回归风险区**：match 生成代码全链路（P2.1/P4/P5 全部 match 用例）+ alloc 路径（全量用例均为 tryAllocSlow 消费者）。
5. **§4.5 整合重构验证**：重构前后各编译一次 test.aura，对生成 test.cpp 做 diff——除 genMethodDecl 值类型注册补强外应完全等价；全量回归 ALL TESTS PASSED。
6. **ASAN 模式**（可选，issue 1 深度验证）：按 AGENTS.md ASAN 流程对 test.cpp 手动编译运行，确认无内存错误。

---

## 10. 风险与缓解

| 风险 | 等级 | 应对 |
| ---- | ---- | ---- |
| issue 2：`auto&&` → `auto` 对 Optional/Variant 指针拷贝——语义等价 | 低 | 指针 trivially copyable；生成代码目视检查 |
| issue 2：binding `auto&` → `auto` 拷贝——分支体内赋值不写回 Variant | 低 | 评审确认 Aura match binding 为只读语义；测试覆盖 |
| issue 2：`_match_rh`/`_mh{N}` 命名与用户变量冲突 | 低 | `_` 前缀为编译器保留命名空间；计数器唯一 |
| issue 2：gcRootVarNames_/gcRootTypes_ 临时注册泄漏（分支体结束后未成对清理） | 中 | 4.2.5 成对 erase + `gcTmpVars` 收集；评审检查 |
| issue 2：嵌套 match 中 `_match_rh` 遮蔽 | 低 | C++ 块作用域遮蔽合法；用例 C 验证 |
| issue 1：memset 性能（慢路径每次 alloc 多一次清零） | 低 | 与 TLAB 路径一致；慢路径低频 |
| issue 1：无法构造直接触发测试（时序敏感） | 中 | 全量回归 + ASAN 压力验证；缺陷链路已在 §3.1 论证 |
| issue 3：方向 A 不实施导致远期触发场景无方案 | 中 | §4.3 完整设计留档；TODO 保留 issue |
| §4.5：genMethodDecl 统一后**新增**值类型 NamedType 注册（行为补强） | 低 | 生成 test.cpp diff 确认仅新增 valueTypeVarNames_ 相关判断；全量回归 |
| §4.5：genFunExpr 若误调 `registerRawParamTracking` → 生成 `.get()` 编译失败 | 低 | 设计上 genFunExpr 仅调 `registerParamTracking`；编译期即暴露 |
| §4.5：genFunExpr saved/restore 与 clear 机制差异被破坏 | 低 | 4.5.2 明确保存/恢复两行保留原位，公共函数只提取单参数注册逻辑 |
| §4.5：重构后行为等价性 | 中 | 重构前后 test.cpp diff + 全量回归；清理/注册点清单 §4.5 留档 |

---

## 11. 已知限制

1. **issue 1 无确定性复现测试**：触发需"旧页复用 + 字段初始化前 GC"精确时序，当前观察不到实际崩溃；修复为防御性对齐，靠代码等价性论证正确。
2. **issue 2 的 `_match_val` 保护覆盖的是 match 语句内窗口**；若用户将 Variant/Optional 指针存入手写 C++ 桥接（非编译器生成路径），仍需显式 GcRootHandle——不在本语言语义内，不处理。
3. **issue 3（方向 A）本 plan 不实施**：当前零触发；触发场景（ThreadLocal 句柄直接存入 GC 堆对象成员）出现后按 §4.3 设计实施。与 threadRootLists节点compact重定位.md §10 限制 3 结论一致。
4. **match 接口视图分支**（ViewRoot 路径）保持 change.md 步骤 7 现状；其 self 在 compact 后的有效性由 ViewRoot 保证，不在本 issue 范围。
5. **`_match_val` 保护不覆盖 branch 条件判定期间**：`cond` 表达式（`_match_val->is<I>()`）在分支体 alloc 之前求值，此时无跨 GC 窗口（GC 只在 alloc 时触发，条件求值不 alloc），无风险。
