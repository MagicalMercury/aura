# GC 稳健性三 issue 综合修复 —— 合并实施文档

> 状态：**已实施**（工作流 5 落地完成；编译无警告、全量回归 ALL TESTS PASSED、新增 u5/u6/m10/m11 输出正确）
> 审查状态：out.txt 第三轮裁决"修复后可直接通过"；实施中 3 处偏离（matchCounter_ 未引入 / 步骤 4·5 binding 改 `_raw` Ref 模式 / 测试用例改 `T | None`）见各步"实施说明"标注
> 日期：2026-08-07
> 来源 plan：[plan/GC稳健性三issue综合修复.md](file:///d:/you/Aura/plan/GC稳健性三issue综合修复.md)（TODO.txt [五] GC 三 issue，L248-284）：
>   - [ ] P2 tryAllocSlow 路径未 memset
>   - [ ] P2 match 分支 _match_val 跨 GC 保护（Variant 对象指针悬垂）
>   - [ ] P3 方向 A：compact 层重定位 threadRootLists_ 链表 node（仅设计留档，不实施）
> 前置：compact 全局根重定位（relocateGlobalRootPtrs）与 threadRootLists 方向 B（ViewRoot 转 Global）均已落地。

---

## 1. 修改目标与原因

1. **tryAllocSlow 未 memset**（[alloc.cpp L119-173](file:///d:/you/Aura/runtime/gc/alloc.cpp#L119-L173)）：慢路径 bump 出的对象内存含旧对象残留（GC sweep 后保留旧页复用），字段初始化前 GC 触发时 markFields 读到无效指针 → SEGV。与 TLAB 快路径（L81）、tryAllocMedium（L389）、tryAllocLarge（L427）对齐补齐一行 memset。
2. **match `_match_val` 跨 GC 保护**（[StmtGen.cpp L1681](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1681)）：`auto&& _match_val = expr;` 无 GcRootHandle 保护，分支体内 alloc 触发 compact 后悬垂（Variant/Optional 堆指针）。用 Ref 模式 GcRootHandle 包裹，并同步保护非视图堆变体/堆元素 binding 的间接访问路径。
3. **threadRootLists_ node 重定位（方向 A）**：ThreadLocal 句柄进 GC 堆后链表 node（this）悬垂。当前闭包捕获路径已由方向 B 规避，本 plan 仅输出完整设计留档，不实施。

---

## 2. 受影响的文件和模块列表

| 文件 | 改动 | 所属 |
| ---- | ---- | ---- |
| `runtime/gc/alloc.cpp` | tryAllocSlow 补 memset（1 行） | issue 1 |
| `src/CodeGen/CodeGen.h` | 新增 `matchCounter_` 计数器 + 3 个公共函数声明 | issue 2 + §4.5 |
| `src/CodeGen/StmtGen.cpp` | `_match_val` Ref 包裹 + 非视图堆变体 binding 保护 + Optional binding 保护 + gcTmpVars 清理 | issue 2 |
| `src/CodeGen/DeclGen.cpp` | 新增 3 个公共函数实现 + 6 处成组 clear 替换 + 3 处参数注册替换 | §4.5 |
| `src/CodeGen/ExprGen.cpp` | genFunExpr 参数注册替换为 `registerParamTracking` | §4.5 |
| `example/test.aura` | 新增 u5 / m10 / u6 / m11 用例 | 测试 |

⚠️ 无 BREAKING：issue 1 是内部清零；issue 2 仅生成代码形态变化（指针语义不变）；§4.5 是纯重构（唯一例外：genMethodDecl 补值类型 NamedType 注册，行为补强）。

---

## 3. 修改步骤（Ordered，含详细实现代码）

### 阶段一（issue 1）：tryAllocSlow 补 memset

**步骤 1：alloc.cpp 补 memset**

文件：[runtime/gc/alloc.cpp](file:///d:/you/Aura/runtime/gc/alloc.cpp#L145-L147)（`throwOutOfMemory()` 检查后、对象头初始化前）。

修改前：
```cpp
    if (!mem) {
        // GC 后仍失败 → 抛出预缓存的 OutOfMemoryError
        throwOutOfMemory();
    }

    GcObject* obj = static_cast<GcObject*>(mem);
```

修改后：
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

说明：`size` 已在 tryAlloc L46 对齐到 8 字节（= header + payload 总大小），与现有 3 处 memset 模式完全一致。此单点覆盖全部小页路径（首次 / TLAB 满 / gcPending_ 慢路径 / compact 后）。

---

### 阶段二（issue 2）：match 跨 GC 保护

**步骤 2：CodeGen.h 新增 `matchCounter_` 计数器**

文件：[src/CodeGen/CodeGen.h](file:///d:/you/Aura/src/CodeGen/CodeGen.h#L492-L495)（unionBoxingCounter_ 之后）。

修改前：
```cpp
    int unionBoxingCounter_ = 0;  // P3b 隐式装箱临时变量名计数器
```

修改后：
```cpp
    int unionBoxingCounter_ = 0;  // P3b 隐式装箱临时变量名计数器
    int matchCounter_ = 0;        // match 分支 binding 保护 GcRootHandle 临时变量名计数器
```

> **实施说明（2026-08-07，已落地）**：步骤 2 最终**未引入** `matchCounter_`。实施中发现步骤 4/5 的 binding 保护改用"`varName_raw` 独立栈变量 + `GcRootHandle varName(varName_raw)` Ref 模式"（与函数参数 `_raw` 模式一致），句柄变量名即绑定名本身，无需 `_mh{N}` 计数器。已从 CodeGen.h 移除该声明。

**步骤 3：`_match_val` 用 Ref 模式 GcRootHandle 包裹（核心）**

文件：[src/CodeGen/StmtGen.cpp](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1679-L1682)（genMatchStmt 块作用域打开后）。

修改前：
```cpp
    cpp << indentStr() << "{\n";
    indentLevel_++;
    writeLine(cpp, "auto&& _match_val = " + expr + ";");
```

修改后：
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

要点：
- `auto&&` → `auto`：`expr` 为右值（`var.get()` 临时指针）时拷贝指针值，使 `_match_val` 成为独立栈变量，Ref 模式 `ptr_ref_` 才能稳定指向它。
- 固定名 `_match_rh`：match 语句自带 `{` 块作用域（L1679），嵌套 match 在不同作用域同名不冲突。
- **不注册 `gcRootVarNames_`**：`_match_val` 19 处使用点全为手写字符串拼接，不走 genIdentifier；注册反而引入闭包捕获转 Global 副作用。
- 生命周期：`_match_rh` 在 L1851 外层块结束析构，覆盖全部使用窗口。

**步骤 4：非视图堆变体 binding 保护（间接访问路径 1）+ 新局部变量声明**

文件：[src/CodeGen/StmtGen.cpp](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1664-L1666)（局部变量声明区，`variantCppTypes` 声明之后、`if (auto* u = ...)` 之前；L1664 isVariantPtr / L1665 isOptional / L1666 variantCppTypes）。

修改前：
```cpp
    bool isVariantPtr = false;   // aura_rt::Variant<T...>*
    bool isOptional   = false;   // aura_rt::Optional<T>*
    std::vector<std::string> variantCppTypes;  // 各变体 C++ 类型（索引对应；std::variant 与 Variant 路径共用）
```

修改后：
```cpp
    bool isVariantPtr = false;   // aura_rt::Variant<T...>*
    bool isOptional   = false;   // aura_rt::Optional<T>*
    std::string elemCppType;                 // Optional 元素 C++ 类型（步骤 5 用）
    bool elemIsHeap = false;                 // Optional 元素是否为堆类型（步骤 5 用）
    std::vector<std::string> gcTmpVars;      // 本分支临时注册的 GC 根变量名（步骤 4/5 注册、步骤 6 清理）
    std::vector<std::string> variantCppTypes;  // 各变体 C++ 类型（索引对应；std::variant 与 Variant 路径共用）
```

文件：[src/CodeGen/StmtGen.cpp](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1777-L1780)（`} else {` 分支，含闭合 `}` L1780）。`varName` 已在外层 L1763 声明（L1762 为 `if (!tp->varName.empty())` 守卫），`idx` 已解析。

修改前：
```cpp
                        } else {
                            binding = "auto& " + varName + " = _match_val->get<"
                                      + std::to_string(idx) + ">();";
                        }
```

修改后：
```cpp
                        } else {
                            // 步骤 4：值拷贝到独立栈变量 +（堆指针变体）GcRootHandle Ref 模式包裹
                            // 原 auto& 为 Variant storage 槽位引用，分支体内 alloc 后悬垂；
                            // varName_raw 为独立栈变量（get 返回 T&，auto 拷贝为 T 值），
                            // varName 句柄经 ptr_ref_ 引用之 → compact 更新 varName_raw 本体，
                            // genIdentifier 对 varName 生成 .get()（与函数参数 _raw 模式一致）
                            std::string rawName = varName + "_raw";
                            binding = "auto " + rawName + " = _match_val->get<"
                                      + std::to_string(idx) + ">();";
                            if (isGcPointerType(cppType)) {
                                binding += " aura_rt::GcRootHandle<decltype(" + rawName
                                           + ")> " + varName + "(" + rawName + ");";
                                gcRootVarNames_.insert(varName);
                                gcRootTypes_[varName] = "decltype(" + rawName + ")";
                                gcTmpVars.push_back(varName);
                            } else {
                                // 值类型变体：仅拷贝（无 GC 指针，无需包裹）
                                binding = "auto " + varName + " = _match_val->get<"
                                          + std::to_string(idx) + ">();";
                            }
                        }
```

> **实施说明（2026-08-07，已落地）**：本步与步骤 5 的 binding 方案较初稿有修正——初稿为 `auto varName = get<...>()` + `GcRootHandle<decltype(varName)> _mh{N}(varName)` 并把 `varName` 注册到 `gcRootVarNames_`。实施编译时发现缺陷：`varName` 是裸指针而非句柄，genIdentifier 对已注册的 `varName` 生成 `.get()` → `GcString*` 无 `.get()` 成员 → 编译失败。修正为函数参数同款 `_raw` 模式：裸指针拷贝到 `varName_raw` 独立栈变量，`varName` 句柄 Ref 引用之，`varName.get()` 返回 `varName_raw` 的值。

说明：`isGcPointerType(cppType)`（TypeMap.cpp L32-39）：`GcString*`/`Point*`/`Array<T>*` 等以 `*` 结尾 → 包裹；值类型 → 仅拷贝。此分支的 cppType 已排除接口视图（上层 if 已拦截）。`gcRootTypes_` 用 `decltype(varName_raw)`（与函数参数注册模式一致）。

**步骤 5：Optional 堆元素 binding 保护（间接访问路径 2）**

文件：[src/CodeGen/StmtGen.cpp](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1672-L1674)（OptionalSemType 分支，含闭合 `}` L1674）。

修改前：
```cpp
    } else if (dynamic_cast<const OptionalSemType*>(mt)) {
        isOptional = true;
    }
```

修改后：
```cpp
    } else if (dynamic_cast<const OptionalSemType*>(mt)) {
        isOptional = true;
        // 步骤 5：提取 Optional 元素堆判定（binding 保护用）
        // isHeapSemType 为成员函数：定义于 ExprGen.cpp L12，声明于 CodeGen.h L257（跨文件调用无障碍）
        if (auto* os = dynamic_cast<const OptionalSemType*>(mt)) {
            elemCppType = mapSemType(*os->elementType);
            elemIsHeap = isHeapSemType(os->elementType.get());
            if (!elemIsHeap && !elemCppType.empty() && elemCppType.back() == '*')
                elemIsHeap = true;  // C++ 名以 * 结尾回退判定
        }
    }
```

文件：[src/CodeGen/StmtGen.cpp](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1785-L1788)（Optional binding）。

修改前：
```cpp
            } else if (isOptional) {
                cond = "!_match_val->is_none()";
                if (!tp->varName.empty())
                    binding = "auto " + safeName(tp->varName) + " = _match_val->unwrap();";
```

修改后：
```cpp
            } else if (isOptional) {
                cond = "!_match_val->is_none()";
                if (!tp->varName.empty()) {
                    // 步骤 5：值拷贝到独立栈变量 +（堆元素）GcRootHandle Ref 模式包裹
                    // 原裸指针拷贝在分支体内 alloc 后悬垂；值元素仅拷贝不包裹
                    std::string varName = safeName(tp->varName);
                    if (elemIsHeap) {
                        std::string rawName = varName + "_raw";
                        binding = "auto " + rawName + " = _match_val->unwrap();"
                                + " aura_rt::GcRootHandle<decltype(" + rawName
                                + ")> " + varName + "(" + rawName + ");";
                        gcRootVarNames_.insert(varName);
                        gcRootTypes_[varName] = "decltype(" + rawName + ")";
                        gcTmpVars.push_back(varName);
                    } else {
                        // 值元素：仅拷贝（无 GC 指针，无需包裹）
                        binding = "auto " + varName + " = _match_val->unwrap();";
                    }
                }
```

（binding 方案修正同步骤 4 实施说明：`varName_raw` + Ref 模式，`unwrap()` 返回 T 值，auto 拷贝为独立栈变量。）

**步骤 6：分支体结束清理（gcTmpVar 成对 erase）**

文件：[src/CodeGen/StmtGen.cpp](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1838-L1843)（viewTmpVar 清理之后）。

修改前：
```cpp
        // P2b：接口视图分支的临时 viewRootVarNames_ 注册，分支体生成完毕后移除
        // （嵌套闭包捕获 varName 的分支体内仍能查到，走 genFunExpr 的 Global 转换）
        if (!viewTmpVar.empty()) {
            viewRootVarNames_.erase(viewTmpVar);
            valueTypeVarNames_.erase(viewTmpVar);
        }
```

修改后：
```cpp
        // P2b：接口视图分支的临时 viewRootVarNames_ 注册，分支体生成完毕后移除
        // （嵌套闭包捕获 varName 的分支体内仍能查到，走 genFunExpr 的 Global 转换）
        if (!viewTmpVar.empty()) {
            viewRootVarNames_.erase(viewTmpVar);
            valueTypeVarNames_.erase(viewTmpVar);
        }
        // 步骤 4/5：清理本分支临时注册的 GC 根（成对 erase 两个集合，防泄漏）
        for (auto& v : gcTmpVars) {
            gcRootVarNames_.erase(v);
            gcRootTypes_.erase(v);
        }
        gcTmpVars.clear();
```

说明：注册时成对 insert（`gcRootVarNames_` + `gcRootTypes_`），清理必须成对 erase——漏 erase `gcRootVarNames_` → 分支体外同名变量被误判已注册根（genIdentifier 生成 `.get()` 编译失败）；漏 erase `gcRootTypes_` → 状态残留污染后续分支。

**不动的部分**：接口视图变体分支（L1764-1776 ViewRoot 路径，change.md 步骤 7 已保护）；全值 `std::variant`（L1789-1794）与普通类型（L1795-1799）路径无 GC 指针，保持 `auto&&` / 引用绑定；genConstCond lambda（L1684-1742）全部零改动（`_match_val` 变量名不变）。

---

### 阶段三（issue 3）：threadRootLists node 重定位（方向 A 设计留档，不实施）

> 决策：不实施方向 A。当前零触发（方向 B 已闭环：闭包捕获 GC 根 → Global，ExprGen.cpp L1615-1620；闭包捕获视图 → ViewRoot Global，L1621-1628）；双向链表重连顺序耦合复杂，成本高于收益（plan/threadRootLists节点compact重定位.md §4.1 已论证）。触发场景（ThreadLocal 句柄直接存入 GC 堆对象成员）出现后再按以下设计实施。

**设计（供未来触发场景实施）**：

1. **快照点**：`updateAllReferences` 步骤 1（compact.cpp L308-315）与 `updateMediumPageReferences`（L727-733）遍历 threadRootLists_ 时——此阶段旧地址仍可安全 deref（小页在 memcpy 前；中页/大页在旧页进池前）。对每个 node 判定 `node ∈ [oldAddr, oldAddr+size)`（compactEntries_ 二分 / forwardMap 线性），记录 `{list, node, newNode, node->prev_, node->next_, isHead}`。
2. **重连点**（memcpy 后、旧页释放前）：新增 `relocateThreadRootNodes()`，与 `relocateGlobalRootPtrs`（L416-445）同锁域（STW 期间）：
   - 重定位前驱/后继地址（若也在移动区间，各自 +off）；
   - `list->head == node → newNode`；`prev->next_ = newNode`；`next->prev_ = newNode`；
   - 重写 newNode 的 `next_`/`prev_` 为重定位后值；
   - 同步改写 newNode 的 `ptr_ref_` 为 `newNode + off`（与 `kGcHandlePtrRefValDelta` 同思路，保证析构 `unregisterRootThreadLocal(this)` 与遍历一致性）。
3. **中页/大页同路径**：`relocateRootsInForwardMap` 调用处（compact.cpp L702 / **L881**，L880 为注释行）相邻新增同逻辑。

---

### 阶段四（§4.5）：状态清理与参数注册整合

**步骤 7：新增 `clearVarTrackingState()`**

文件：[src/CodeGen/CodeGen.h](file:///d:/you/Aura/src/CodeGen/CodeGen.h) 新增成员函数声明（放 public 区域）：
```cpp
    // 清理函数级变量跟踪状态（6 处调用点统一；新增跟踪集合时必须同步此处）
    void clearVarTrackingState();
```

文件：[src/CodeGen/DeclGen.cpp](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp) 新增实现（放 genFunDecl 之前）：
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

**6 处成组 clear 替换**（每处用 `clearVarTrackingState();` 替换成组 6 行；特殊字段保留原位）：

| 位置 | 特殊字段（保留原位） | 替换内容 |
| ---- | ---- | ---- |
| L183-190 | L183 `currentReceiverName_.clear()` | 仅 L185-190 6 行 → 1 调用 |
| L431-436 | 无 | L431-436 6 行 → 1 调用 |
| L529-535 | L535 `currentReturnElem_.clear()` | 仅 L529-534 6 行 → 1 调用 |
| L674-679 | 无 | L674-679 6 行 → 1 调用 |
| L782-790 | L782 `currentReceiverName_.clear()`、L790 `currentReturnElem_.clear()` | 仅 L784-789 6 行 → 1 调用 |
| L838-844 | L844 `currentTParams_.clear()` | 仅 L838-843 6 行 → 1 调用 |

**步骤 8：新增 `registerParamTracking(const Param& p)`（公共 3 类，无 _raw）**

文件：[src/CodeGen/CodeGen.h](file:///d:/you/Aura/src/CodeGen/CodeGen.h) 声明：
```cpp
    // 参数类型跟踪注册（3 处共有：string / 用户接口 / 值类型 NamedType；不含 _raw 语义）
    void registerParamTracking(const Param& p);
```

文件：[src/CodeGen/DeclGen.cpp](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp) 实现（逻辑 = genFunExpr L1699-1716 三分支 + genFunDecl 分支 1）：
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

**步骤 9：新增 `registerRawParamTracking(const Param& p)`（DeclGen 特有，_raw 语义）**

文件：[src/CodeGen/CodeGen.h](file:///d:/you/Aura/src/CodeGen/CodeGen.h) 声明：
```cpp
    // decl 函数参数特有（签名已生成 varName_raw，函数体入口 GcRootHandle/ViewRoot 包裹）：
    // 接口视图参数 → viewRootVarNames_ + viewRootTypes_[decltype(_raw)]；
    // GC 指针参数 → gcRootVarNames_ + gcRootTypes_[decltype(_raw)]
    void registerRawParamTracking(const Param& p);
```

文件：[src/CodeGen/DeclGen.cpp](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp) 实现（逻辑 = genFunDecl L452-475 与 genMethodDecl L695-711 完全相同部分）：
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

**步骤 10：三处参数注册替换**

(1) [DeclGen.cpp L437-476](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L437-L476) genFunDecl 参数循环：

修改前（4 分支内联）：
```cpp
    for (auto& p : decl.params) {
        if (p.type && dynamic_cast<const NamedType*>(p.type.get())) {
            // ... 值类型 NamedType 分支
        }
        // ... string / 接口视图 / GC 指针 分支
    }
```

修改后：
```cpp
    for (auto& p : decl.params) {
        registerParamTracking(p);
        registerRawParamTracking(p);
    }
```

(2) [DeclGen.cpp L687-713](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L687-L713) genMethodDecl 参数循环（现缺"值类型 NamedType"分支，统一后**新增**该注册——行为补强）：

修改前（3 分支内联）：
```cpp
    for (auto& p : decl.params) {
        if (p.type) {
            auto mt = mapType(*p.type);
            // ... string / 接口视图 / GC 指针 分支
        }
    }
```

修改后：
```cpp
    for (auto& p : decl.params) {
        registerParamTracking(p);
        registerRawParamTracking(p);
    }
```

(3) [ExprGen.cpp L1699-1716](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L1699-L1716) genFunExpr 参数循环（**仅调 registerParamTracking**，lambda 参数无 `_raw` 包裹，不得注册 viewRoot/gcRoot；**保留** L1697-1698 `savedStringVars`/`savedValueVars` 保存/恢复两行，不得把 saved/restore 放进公共函数）：

修改前（3 分支内联）：
```cpp
    auto savedStringVars = stringVarNames_;
    auto savedValueVars  = valueTypeVarNames_;
    for (auto& p : e.params) {
        std::string pname = safeName(p.name);
        if (!p.type) continue;
        std::string ptype = mapType(*p.type);
        // ... string / 接口 / 值类型 分支
    }
```

修改后：
```cpp
    auto savedStringVars = stringVarNames_;
    auto savedValueVars  = valueTypeVarNames_;
    for (auto& p : e.params) {
        registerParamTracking(p);   // 填充临时集合；lambda 结束时由 restore 恢复外层
    }
```

**不属参数注册、保留原位的注册点**（勿误改）：
- genMethodDecl 的 receiver 判断（[DeclGen.cpp L682-685](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L682-L685)）；
- StmtGen try-catch **catch 变量**注册（[L961](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L961) / [L1026](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1026) / [L1050](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1050)，`valueTypeVarNames_.insert(cv)`，位于 genTryCatch / genTryCatchNoSetupIIFE / genTryCatchRaw，insert 后紧跟 erase）；
- **真正的 for-in 迭代变量注册**在 [genForStmt L703](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L703)（`IterVarGuard` RAII 注册到 `gcRootVarNames_`/`gcRootTypes_`）；spawn 闭包参数（L1346-1348）同用 IterVarGuard。

**收益**：新增跟踪集合时只需改 `clearVarTrackingState()` + 注册函数一处；三处重复逻辑收敛为 2 个公共函数。

---

## 4. 测试验证方案

### 4.1 新增用例（example/test.aura）

```aura
    // ===== P2.1 match 跨 GC 保护（issue 2） =====
    // U5: match 非视图堆变体分支内 gc_force + 字符串拼接 alloc 后 binding 仍有效（间接路径 1）
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

    // U6: 嵌套 match——内层分支 gc_force 后，外层 binding 与内层 binding 均仍有效
    let u6s: int | string = "world"
    match u6s {
        int u6i => io.println("p2.1 u6 = int")
        string u6t => {
            let u6o: string | None = u6t   // 隐式装箱（P3a 折叠 string|None → Optional<GcString*>；u6t 为 GcString*）
            match u6o {
                None => io.println("p2.1 u6 inner none")
                string u6inner => {
                    gc_force()
                    io.println("p2.1 u6 inner = " + str(u6inner.length) + " outer = " + str(u6t.length))
                }
            }
        }
    }

    // ===== P5 Optional match 跨 GC 保护（issue 2） =====
    // m10: Optional<string> match 分支内 gc_force 后 unwrap 绑定仍有效（间接路径 2）
    let m10o: string | None = "abc"   // 隐式装箱（P3a 折叠 string|None → Optional<GcString*>；some("abc") 有字符串 CTAD 陷阱，故用隐式装箱）
    match m10o {
        None => io.println("m10 none")
        string m10s => {
            gc_force()
            let m10x = "x" + "y"       // 同上：制造真实 alloc 压力
            io.println("m10 s len = " + str(m10s.length))
        }
    }

    // m11: int | None（int 非堆不折叠 → std::variant<int32_t, NoneType> 全值）match
    //      值元素分支内 gc_force（值元素不包裹，验证非堆路径）
    let m11o: int | None = 7   // 隐式装箱（生成 std::variant<int32_t, NoneType>(7)）
    match m11o {
        None => io.println("m11 none")
        int m11i => {
            gc_force()
            io.println("m11 i = " + str(m11i))
        }
    }
```

> **实施说明（2026-08-07，已落地）**：u6/m10/m11 的类型注解由显式 `Optional<string>` / `Optional<int>` 改为 `T | None` 联合折叠形式。原因：显式 `Optional<T>` 注解在 Sema 中解析为 `GenericSemType{resolvedName="aura_rt::Optional<...>"}`（resolveType NamedType 分支），**不是** OptionalSemType——match 常量检查 `constCompatibleWith`（StmtChecker.cpp L40-51）与 CodeGen `genMatchStmt` 的 `isOptional` 判定（`dynamic_cast<const OptionalSemType*>`）均只认 OptionalSemType，显式 `Optional<T>` + match 是未支持路径（READMEs/09 也只展示 `T | None` 语法）。`string | None` 经 P3a 折叠为 OptionalSemType（堆变体），完整覆盖步骤 5 堆元素 unwrap 路径；`int | None` 因 int 非堆不折叠（unionVariantGcUnsafe 返回 false），走 std::variant 全值路径，等价验证值元素非包裹。

预期输出：u5 打印 `p2.1 u5 = 2`（"hi".length）；u6 打印 `p2.1 u6 inner = 5 outer = 5`；m10 打印 `m10 s len = 3`；m11 打印 `m11 i = 7`。

> 注：`let u5x`/`let m10x` 仅在分支体内构造丢弃的字符串，作用是在第二次 `gc_force()` 前制造真实 alloc 压力——若只调 `gc_force()`，GC 后堆页空闲充足时 compact 可能不搬移对象，binding 悬垂无法暴露。

### 4.2 全量回归

```powershell
cmake --build build
cmake --build runtime/build
.\example\compile.cmd
.\example\test.exe
```

重点：
- 既有 u1-u4（联合变体含接口）、C1、m1-m9、P4 u1-u8 不回归；
- 新增 u5/m10/u6/m11 输出正确；
- 末尾 `ALL TESTS PASSED`。

### 4.3 §4.5 整合重构验证

重构前后各编译一次 test.aura，对生成 test.cpp 做 diff——除 genMethodDecl 值类型注册补强外应完全等价；全量回归 ALL TESTS PASSED。

### 4.4 深度检测（ASAN，issue 1 可选）

按 AGENTS.md ASAN 流程对 test.cpp 手动编译运行（`gc_force` 循环 + 大量 alloc 压力），确认无内存错误后切回普通模式。

---

## 5. 风险与应对

| 风险 | 等级 | 缓解 |
| ---- | ---- | ---- |
| issue 2：`auto&&` → `auto` 对 Optional/Variant 指针拷贝——语义等价 | 低 | 指针 trivially copyable；生成代码目视检查 |
| issue 2：binding `auto&` → `auto` 拷贝——分支体内赋值不写回 Variant | 低 | 评审确认 Aura match binding 为只读语义；测试覆盖 |
| issue 2：`_match_rh`/`_mh{N}` 命名与用户变量冲突 | 低 | `_` 前缀为编译器保留命名空间；计数器唯一 |
| issue 2：gcRootVarNames_/gcRootTypes_ 临时注册泄漏 | 中 | 步骤 6 成对 erase + `gcTmpVars` 收集；评审检查 |
| issue 2：嵌套 match 中 `_match_rh` 遮蔽 | 低 | C++ 块作用域遮蔽合法；用例 u6 验证 |
| issue 1：memset 性能 | 低 | 与 TLAB 路径一致；慢路径低频 |
| issue 1：无法构造直接触发测试 | 中 | 全量回归 + ASAN 压力验证；缺陷链路已在调研论证 |
| issue 3：方向 A 不实施导致远期触发场景无方案 | 中 | 阶段三完整设计留档；TODO 保留 issue |
| §4.5：genMethodDecl 统一后**新增**值类型 NamedType 注册 | 低 | 生成 test.cpp diff 确认仅新增 valueTypeVarNames_ 相关判断；全量回归 |
| §4.5：genFunExpr 若误调 `registerRawParamTracking` → 生成 `.get()` 编译失败 | 低 | 设计上 genFunExpr 仅调 `registerParamTracking`；编译期即暴露 |
| §4.5：genFunExpr saved/restore 机制被破坏 | 低 | 步骤 10(3) 明确保存/恢复两行保留原位 |
| §4.5：重构后行为等价性 | 中 | 重构前后 test.cpp diff + 全量回归；清理/注册点清单留档 |

---

## 6. 已知限制（本次不修复）

1. **issue 1 无确定性复现测试**：触发需"旧页复用 + 字段初始化前 GC"精确时序，当前观察不到实际崩溃；修复为防御性对齐，靠代码等价性论证正确。
2. **issue 2 的 `_match_val` 保护覆盖 match 语句内窗口**：若用户将 Variant/Optional 指针存入手写 C++ 桥接（非编译器生成路径），仍需显式 GcRootHandle——不在本语言语义内。
3. **issue 3（方向 A）本 plan 不实施**：当前零触发；触发场景出现后按阶段三设计实施。与 plan/threadRootLists节点compact重定位.md §10 限制 3 结论一致。
4. **match 接口视图分支**（ViewRoot 路径）保持原 change.md 步骤 7 现状；self 在 compact 后的有效性由 ViewRoot 保证。
5. **`_match_val` 保护不覆盖 branch 条件判定期间**：`cond` 表达式（`_match_val->is<I>()`）在分支体 alloc 之前求值，条件求值不 alloc，无跨 GC 窗口，无风险。
