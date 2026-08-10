# 详细实施方案：放开内置 Iterator 联合变体拦截（Variant<Iterator<T>, X>）

> 工作流：3（详细实施方案；替换原草案内容）
> 提出时间：2026-08-10
> 状态：**待审查**
> 来源：[评估放开内置Iterator联合变体拦截issue.md](file:///d:/you/Aura/plan/评估放开内置Iterator联合变体拦截issue.md)（工作流 2 产物，已作废）；issue 见 TODO.txt §二 L99-109
> 前置：联合变体含接口已实施（plan/done/联合变体含接口isPtrActive钩子支持.md）；迭代器 B+W 值视图化已完成（plan/done/迭代器GC安全修复.md）
> 源码核对：3 个并行 Search Agent（2026-08-10）+ 直接 Read 复核；全部行号为当前磁盘实际行号

---

## 1. 目标

放开 `variantStorageUnsafe` 对内置 Iterator（GenericSemType "Iterator"）的联合变体编译期拦截，使 `Iterator<T> | X`（如 `Iterator<int> | int`）成为合法类型且 **GC 安全**；经真实用例（TODO L103-105 指定）编译运行验证后正式放开，失败则恢复拦截（TODO 已预留退路）。

**核心结论（先行）**：运行时机制已完备（`is_iface_view_v<Iterator<T>>` 自动命中 → descForI 注册 self 子偏移；装箱/match ViewRoot 包裹已覆盖 `aura_rt::Iterator<...>`），**唯一缺的是编译器 3 处"视图感知"gate**——hasHeap/isVariantPtr 判定用 `isHeapSemType`（对 Iterator 恒 false，ExprGen.cpp:28-29）会把联合错误映射为全值 `std::variant<aura_rt::Iterator<T>, X>`（视图 self 指针 GC 不可见 → 运行时崩溃）。本方案统一引入 `isUnionHeapVariant` 判定修复全部 gate。

## 2. 现状核对（plan 断言 → 源码事实）

| # | 断言 | 核实结果 |
| - | ---- | -------- |
| 1 | variantStorageUnsafe 定义（DeclChecker.cpp:11-21），L18-19 拦 Iterator | ✅ 一致；注释 L14-17 为 P0.4 时代理由（"isPtrActive 不支持子偏移"），B+W 后已过时 |
| 2 | 唯一调用点 resolveType UnionType 分支（DeclChecker.cpp:448-453） | ✅ 一致；报错 "union variant '...' is not supported in a union" |
| 3 | `is_iface_view_v<T>` 泛型检测（variant.h:29-38）：有 self 成员且可转 GcObject* | ✅ 一致；`Iterator<T>` 有 `GcObject* self`（iterator.h:61）→ **自动命中**（注释 L27-28 明示"如 Stringer / Iterator<T>"） |
| 4 | descForI 视图分支注册 `kStorageOffset + offsetof(T, self)`（variant.h:74-79） | ✅ 一致；`offsetof(Iterator<T>, self)==8`，与单方法接口视图同构（DeclGen.cpp:161-167） |
| 5 | Variant 扫描走 dynamicDesc 钩子（variant.h:56-62、L97-99）→ markFields（mark_sweep.cpp:183）/ updateObjectFields（compact.cpp:486）消费 | ✅ 一致（task-2 交叉验证） |
| 6 | genUnionBoxingImpl 视图变体 ViewRoot 包裹（StmtGen.cpp:170-186），`isIfaceViewVariant = isIfaceViewTypeName(cppTypes[idx])`（L156） | ✅ 一致；isIfaceViewTypeName 显式含 `aura_rt::Iterator<`（TypeMap.cpp:44-46，Iterator 判定 L45）→ Iterator 变体命中 |
| 7 | genMatchStmt 视图变体 ViewRoot 绑定（StmtGen.cpp:1832-1844） | ✅ 一致；`isIfaceViewTypeName(cppType)` 判定 |
| 8 | genGcRootedArgs 视图排他（ExprGen.cpp isIfaceView L43-50 + L73/L102/L119/L131/L149/L151） | ✅ 一致；isIfaceView 为 **file-static**（ExprGen.cpp），需提升为成员供 TypeMap/StmtGen 复用 |
| 9 | mapSemType UnionSemType hasHeap（TypeMap.cpp:316-319）用 isHeapSemType | ✅ 一致；Iterator→false → 错误生成 `std::variant` ⚠️ |
| 10 | mapType UnionType hasHeap（TypeMap.cpp:167-197）：inferredType 路径 isHeapSemType（L172-173）；C++ 名回退路径 P2b 显式排除 Iterator（L184 `n->name != "Iterator"`） | ✅ 一致；两处均需放开 ⚠️ |
| 11 | genUnionBoxing hasHeap（StmtGen.cpp:94-97）用 isHeapSemType | ✅ 一致；需放开 ⚠️ |
| 12 | genMatchStmt isVariantPtr（StmtGen.cpp:1719-1723）用 isHeapSemType | ✅ 一致；需放开 ⚠️（否则 match 走 std::variant 路径 L1889-1894 无 _match_rh 保护） |
| 13 | isPtrActive 钩子 | ✅ **不存在**（全仓库 grep 仅 TODO/plan 文档命中）；已由静态 descForI 方案取代（TODO.txt:87-89） |
| 14 | 旧 change.md「联合变体含接口」步骤 6 | ❌ git 无法找回（b831365 前的实施版未提交）；**等价内容存活**在 plan/done/联合变体含接口isPtrActive钩子支持.md §4.2（descForI/is_iface_view）+ §4.5（genUnionBoxingImpl ViewRoot）+ §4.6（ViewRoot 构造） |
| 15 | 用例 4（plan/done/联合变体含接口isPtrActive钩子支持.md:414-422）：`let it: Iterator<int> | int = range(0, 10)` + force_gc + match | ✅ 一致（文档中 force_gc 为笔误，实际函数为 **gc_force**，builtin.aurai:13；collect_all 亦为笔误，实际为 **`it.collect()`** 方法，interfaces.aurai:30 → CodeGen 映射 `aura_rt::collect_all(obj)`，ExprGen.cpp:955-960） |
| 16 | `Iterator<T> | None` 不折叠路径 | ✅ unionVariantGcUnsafe(GenericSemType)=false（DeclChecker.cpp:45）+ mapSemType 折叠判定 isHeapSemType(otherV)=false（TypeMap.cpp:313）→ 保持 UnionSemType → 修复后走 `aura_rt::Variant<Iterator<T>, NoneType>*`，descForI<NoneType> 值变体分支（0 字段）✓ 自动安全 |
| 17 | 联合上直接方法调用（`v.map(f)`） | ✅ 现状：inferMethodCallOnVariant / genUnionDispatch sups 判定对 Iterator 无方法 → Sema 报错。保持（已知限制） |
| 18 | isAssignable 对已解析 GenericSemType 走 equals（SemAnalyzer.cpp:362-373） | ✅ P0.5 已修复（SemType.cpp:225-234 GenericSemType::equals 比较 resolvedName）→ `Iterator<int> ≠ Iterator<string>` 精确 |
| 19 | P3c 泛型实例化二次检查（SemAnalyzer.cpp:535-539）unionVariantGcUnsafe | ✅ 对 GenericSemType 恒 false（L45）→ Iterator 不触发拦截/折叠 |

**关键事实（决定实现路径）**：
1. **`isHeapSemType` 语义不可改**（ExprGen.cpp:12-36）：判"是否需要 GcRootHandle 包装"——Iterator 视图非指针不能包装（L24-29 注释），恒 false。因此新增**独立的联合变体判定** `isUnionHeapVariant(t) = isHeapSemType(t) || isIfaceView(t)`（视图含 self GC 指针，放 std::variant 内部 GC 不可见，必须堆 Variant 封装）。
2. `isIfaceView`（ExprGen.cpp:43-50，file-static）提升为成员后，TypeMap.cpp / StmtGen.cpp 可直接复用；其现有 6 处调用点（L73/L102/L119/L131/L149/L151，同文件成员函数内）不受影响。
3. 接口视图（InterfaceSemType）hasHeap=true 已由 isHeapSemType 覆盖（L22）→ 新增判定不改既有接口变体行为（回归 p2.1 u1-u4 确认）。
4. 装箱路径（genUnionBoxingImpl L170-186）与 match 绑定路径（L1832-1844）已按 `isIfaceViewTypeName` 对 Iterator 生效 → hasHeap 修复后全链路闭合：`aura_rt::Variant<aura_rt::Iterator<int32_t>, int32_t>*` + ViewRoot 包裹 + descForI 子偏移扫描。

## 3. 逐层修改

### 3.1 `src/CodeGen/CodeGen.h`：isIfaceView 提升 + isUnionHeapVariant 声明

在 `isHeapSemType` 声明（约 L257-258）附近追加：

```cpp
    // P1：视图类型判定（值视图 { 函数指针, self }，非 GC 堆对象）
    //   - 内置 Iterator<T>（GenericSemType "Iterator"）
    //   - 接口视图（InterfaceSemType：Stringer/Comparable/用户接口）
    // 视图不能被 GcRootHandle<View> 包裹（视图非指针，模板参数不成立）
    [[nodiscard]] bool isIfaceView(const SemType* t) const;
    // 联合变体堆封装判定：堆类型 或 视图类型
    // （视图含 self GC 指针，放 std::variant 内部 GC 不可见 → 必须 aura_rt::Variant<T...>* 封装，
    //   descForI 按 self 子偏移扫描；与 isHeapSemType 的"传参包装"语义不同，勿混用）
    [[nodiscard]] bool isUnionHeapVariant(const SemType* t) const;
```

### 3.2 `src/CodeGen/ExprGen.cpp`：isIfaceView 改成员定义 + isUnionHeapVariant 定义

**①** L43-50 file-static 定义改为成员定义（签名加 `CodeGenerator::` 前缀与 const）：

```cpp
// P1：视图类型判定（值视图 { 函数指针, self }，非 GC 堆对象）
//   - 内置 Iterator<T>（GenericSemType "Iterator"）
//   - 接口视图（InterfaceSemType：Stringer/Comparable/用户接口）
// 视图不能被 GcRootHandle<View> 包裹（视图非指针，模板参数不成立），
// 传参/包装时按非堆值处理，self 由保守栈扫描 / 视图字段 desc 子偏移保护。
bool CodeGenerator::isIfaceView(const SemType* t) const {
    if (!t) return false;
    if (auto* g = dynamic_cast<const GenericSemType*>(t))
        return g->name == "Iterator";
    if (dynamic_cast<const InterfaceSemType*>(t))
        return true;
    return false;
}

// 联合变体堆封装判定：isHeapSemType（堆对象）|| isIfaceView（视图含 self GC 指针）。
// 视图变体必须进 aura_rt::Variant<T...>*（descForI 子偏移扫描），
// 否则错误生成 std::variant → self 对 GC 不可见 → 悬垂崩溃。
bool CodeGenerator::isUnionHeapVariant(const SemType* t) const {
    return isHeapSemType(t) || isIfaceView(t);
}
```

（原 file-static `isIfaceView` 的 5 处调用点 L102/L119/L131/L149/L151 均为成员函数内调用，提升后无需改动。）

### 3.3 `src/CodeGen/TypeMap.cpp`：两处 hasHeap 判定

**① mapSemType UnionSemType 分支（L317-319）**：

```cpp
// 修改前：
        bool hasHeap = false;
        for (auto& v : u->variants)
            if (v && isHeapSemType(v.get())) { hasHeap = true; break; }

// 修改后：
        bool hasHeap = false;
        for (auto& v : u->variants)
            if (v && isUnionHeapVariant(v.get())) { hasHeap = true; break; }
```

**② mapType UnionType 分支（L167-197）**：

```cpp
// 修改前（L168-197）：
        bool hasHeap = false;
        for (auto& v : u->types) {
            if (!v) continue;
            bool heap = false;
            if (v->inferredType) {
                heap = isHeapSemType(v->inferredType);
            } else {
                // 无 SemType（如类型声明处）：按 C++ 名回退判断（指针类型 = 堆）
                std::string cpp = mapType(*v);
                heap = !cpp.empty() && cpp.back() == '*';
                // P2b：接口视图变体（值视图含 GC 指针 self，C++ 名非 * 结尾）→ 需
                // Variant 堆封装供 descForI 扫描。与 isHeapSemType(InterfaceSemType)=true
                // 对齐（genUnionBoxing 的 hasHeap 判定）；排除内置 Iterator——其值视图
                // 由保守栈扫描保护，isHeapSemType 判定其为非堆，此处保持一致。
                if (!heap && v) {
                    if (auto* n = dynamic_cast<const NamedType*>(v.get())) {
                        if (n->name != "Iterator" && n->namespacePrefix.empty()) {
                            // 用户接口（interfaceNames_）+ 内置接口（auraiInterfaces，
                            // 如 Stringer/Comparable，与 isIfaceViewTypeName 判定一致）
                            bool isIface = interfaceNames_.contains(n->name);
                            if (!isIface)
                                for (auto& ai : BuiltinRegistry::get().auraiInterfaces())
                                    if (ai->name == n->name) { isIface = true; break; }
                            if (isIface) heap = true;
                        }
                    }
                }
            }
            if (heap) { hasHeap = true; break; }
        }

// 修改后：
        bool hasHeap = false;
        for (auto& v : u->types) {
            if (!v) continue;
            bool heap = false;
            if (v->inferredType) {
                // 视图变体（Iterator/接口）也需堆 Variant 封装（self 子偏移扫描）
                heap = isUnionHeapVariant(v->inferredType);
            } else {
                // 无 SemType（如类型声明处）：按 C++ 名回退判断（指针类型 = 堆）
                std::string cpp = mapType(*v);
                heap = !cpp.empty() && cpp.back() == '*';
                // 接口视图变体（值视图含 GC 指针 self，C++ 名非 * 结尾）→ 需
                // Variant 堆封装供 descForI 扫描（与 isUnionHeapVariant 对齐）。
                // 含内置 Iterator：其值视图含 self，B+W 后由 descForI is_iface_view_v
                // 子偏移 + ViewRoot 保护（2026-08-10 评估放开，见 plan）
                if (!heap && v) {
                    if (auto* n = dynamic_cast<const NamedType*>(v.get())) {
                        if (n->namespacePrefix.empty()) {
                            // 用户接口（interfaceNames_）+ 内置接口（auraiInterfaces，
                            // 如 Stringer/Comparable，与 isIfaceViewTypeName 判定一致）
                            bool isIface = interfaceNames_.contains(n->name);
                            if (!isIface)
                                for (auto& ai : BuiltinRegistry::get().auraiInterfaces())
                                    if (ai->name == n->name) { isIface = true; break; }
                            // 内置 Iterator：NamedType{name="Iterator"}（含 Iterator<int> 带 typeArgs）
                            if (isIface || n->name == "Iterator") heap = true;
                        }
                    }
                }
            }
            if (heap) { hasHeap = true; break; }
        }
```

### 3.4 `src/CodeGen/StmtGen.cpp`：genUnionBoxing + genMatchStmt

**① genUnionBoxing（L92-99）**：

```cpp
// 修改前：
    for (auto& v : u.variants) {
        cppTypes.push_back(v ? mapSemType(*v) : "void");
        if (v && isHeapSemType(v.get())) hasHeap = true;
    }

// 修改后：
    for (auto& v : u.variants) {
        cppTypes.push_back(v ? mapSemType(*v) : "void");
        if (v && isUnionHeapVariant(v.get())) hasHeap = true;
    }
```

**② genMatchStmt isVariantPtr（L1719-1723）**：

```cpp
// 修改前：
    if (auto* u = dynamic_cast<const UnionSemType*>(mt)) {
        for (auto& v : u->variants) {
            if (v && isHeapSemType(v.get())) isVariantPtr = true;
            variantCppTypes.push_back(v ? mapSemType(*v) : "void");
        }
    }

// 修改后：
    if (auto* u = dynamic_cast<const UnionSemType*>(mt)) {
        for (auto& v : u->variants) {
            if (v && isUnionHeapVariant(v.get())) isVariantPtr = true;
            variantCppTypes.push_back(v ? mapSemType(*v) : "void");
        }
    }
```

（isVariantPtr=true 后：`_match_val` 为 `aura_rt::Variant<...>*` + GcRootHandle Ref 保护（L1741-1745）；TypePattern binding 走 isIfaceViewTypeName 视图分支 ViewRoot 包裹（L1832-1844）→ 全链路闭合。）

### 3.5 `src/Sema/Checker/DeclChecker.cpp`：移除拦截（L14-19）

```cpp
// 修改前（L11-21）：
static bool variantStorageUnsafe(const SemType& t) {
    if (dynamic_cast<const FuncSemType*>(&t))     return true;
    if (dynamic_cast<const UnionSemType*>(&t))    return true;
    // P0.4：内置迭代器（GenericSemType "Iterator"）联合变体编译期拦截。
    // 视图含 GC 指针 self，Variant storage_ 内 union 无法注册子偏移供 GC 扫描/compact
    // 更新（B+W 落地前视图实现为值类型，isPtrActive 不支持子偏移）→ 一律拦截。
    // B+W 落地后重新评估（见 plan 迭代器GC安全修复 §10.4）
    if (auto* g = dynamic_cast<const GenericSemType*>(&t))
        if (g->name == "Iterator") return true;
    return false;
}

// 修改后（L11-21）：
static bool variantStorageUnsafe(const SemType& t) {
    if (dynamic_cast<const FuncSemType*>(&t))     return true;
    if (dynamic_cast<const UnionSemType*>(&t))    return true;
    // 内置 Iterator（GenericSemType "Iterator"）联合变体：P0.4 起编译期拦截；
    // B+W 值视图化后 descForI is_iface_view_v 子偏移 + 装箱/match ViewRoot 保护
    // 已使其 GC 安全（2026-08-10 评估放开，见 plan/评估放开内置Iterator联合变体拦截实施方案.md）。
    return false;
}
```

### 3.6 测试：`example/test.aura`

在 `ALL TESTS PASSED` 之前追加（main 内）：

```aura
    // ---- Iterator 联合变体（Variant<Iterator<T>, X>，2026-08-10 放开）----
    let iv: Iterator<int> | int = range(0, 10)      // 视图变体装箱
    gc_force()                                      // 装箱后立即 GC
    match iv {
        i: Iterator<int> => io.println("iv iter len=" + str(i.collect().length))
        n: int => io.println("iv unexpected int=" + str(n))
    }
    let iv2: Iterator<int> | int = 42               // 值变体装箱
    match iv2 {
        i: Iterator<int> => io.println("iv2 unexpected iter")
        n: int => io.println("iv2 int=" + str(n))
    }
    // GC 压力：分支体内 alloc 触发 compact，验证 ViewRoot 保护
    let iv3: Iterator<int> | int = range(0, 3)
    match iv3 {
        i: Iterator<int> => {
            gc_force()
            let stress = "stress" + "x"             // 分支体内 alloc
            let arr = [1, 2, 3]                     // 再 alloc
            gc_force()                              // compact 触发
            io.println("iv3 collect=" + str(i.collect().length) + " s=" + stress)
        }
        n: int => io.println("iv3 unexpected int=" + str(n))
    }
    // Iterator | None（不折叠路径 → Variant<Iterator, NoneType>）
    let iv4: Iterator<int> | None = none()
    match iv4 {
        i: Iterator<int> => io.println("iv4 unexpected iter")
        None => io.println("iv4 none ok")
    }
    let iv5: Iterator<int> | None = range(0, 2)
    gc_force()
    match iv5 {
        i: Iterator<int> => io.println("iv5 iter len=" + str(i.collect().length))
        None => io.println("iv5 unexpected none")
    }
```

期望输出：
```
iv iter len=10
iv2 int=42
iv3 collect=3 s=stressx
iv4 none ok
iv5 iter len=2
```

### 3.7 文档与 TODO

1. `READMEs/03-types.md` 联合类型 §：追加一行——`Iterator<T>` 可作为联合变体（与接口视图同机制，match 分支内使用；联合上直接调用迭代器方法暂不支持，需先 match 提取）
2. `TODO.txt` L99-109 条目更新为 `[x]`：完成说明（评估日期、3 处 gate 修复、用例通过、已知限制：联合上直接方法调用不支持）
3. `READMEs/09-pattern-matching.md` 可选：补 `Iterator<int> | int` match 示例（与 03-types 一致即可，不强制）

## 4. 测试方案

1. `cmake --build build`：编译器本体编译通过（CodeGen.h/ExprGen/TypeMap/StmtGen/DeclChecker 改动）
2. `example/compile.cmd`（非 ASAN）→ `example/test.exe`：新用例 5 项断言 + `ALL TESTS PASSED`
3. **回归**（关键：hasHeap 判定改动影响所有联合路径）：
   - 接口变体：`example/used/6.aura` 的 u1-u4（Stringer|int、Comparable<Point>|int|string）——临时用 6.aura 编译运行或从存档提取等价用例进 test.aura
   - match GC 保护：m1-m11 系列（used/6.aura 或既有回归）
   - 全量既有 test.aura 用例
4. **错误用例确认**：`let x: Iterator<int> | fun(int) -> int = range(0,1)` → variantStorageUnsafe FuncSemType 仍拦截；`let y: Iterator<int> | (int | string) = 1` → UnionSemType 嵌套仍拦截
5. 可选深度：ASAN 模式（AGENTS.md 流程）跑含 iv3 压力用例的 test.cpp
6. 生成产物人工检查：test.cpp 中 `make_variant<aura_rt::Iterator<int32_t>, int32_t>(0, ...)` + `ViewRoot<...>` + `->get<0>().self = ...` 形态正确、无 `std::variant<aura_rt::Iterator` 出现

## 5. 风险与应对

| 风险 | 应对 |
| ---- | ---- |
| 放开后运行时 GC 崩溃（self 悬垂） | 机制已完备（descForI 子偏移 + ViewRoot 三处覆盖 + _match_rh Ref 保护），以 iv3 GC 压力用例实证；**失败即回退**：git checkout DeclChecker.cpp（恢复拦截），保留本 plan 记录失败原因（TODO 已预留退路） |
| hasHeap 判定改动影响既有接口变体路径 | isUnionHeapVariant = isHeapSemType(InterfaceSemType)=true（已命中）→ 结果不变；回归 u1-u4 确认 |
| `Iterator<T>|None` 折叠路径意外触发 | unionVariantGcUnsafe(GenericSemType)=false + isHeapSemType(Iterator)=false 均不折叠 → 保持 Variant 路径（iv4/iv5 用例覆盖） |
| Optional<Iterator<T>>（`some(it)`）预存缺陷 | 独立问题（Optional::desc 仅 is_pointer_v 注册 value_ 偏移，optional.h:30-43）；本方案不触碰（`Iterator|None` 不折叠）；**记入 TODO 新条目**后续处理 |
| 视图变体在 `std::variant` 残留路径（其他生成点） | grep `std::variant<` 生成点仅 mapSemType/mapType 两处（本方案已覆盖）；测试步骤 6 人工检查 |
| 联合上直接方法调用（`v.map(f)`）报错行为 | 保持现状（Sema 拦截），文档注明；不属本 issue |
