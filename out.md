# GLM 审查报告：change.md（enum v1.1 **实施文档**，989 行 · 细化后）

> **来自**：GLM（外部审查）
> **对象**：`change.md` 现行版（2026-10-17，实测 **989 行 / 88,396 B / md5 `dbd6ba64d1c43ec15994d73f681b5dd1`**）
> **性质**：**覆盖重写后的细化实施方案**（旧 4468 行 v1 版本已归档至 `backups/change-md/change.md.20261017_115801.pre-enum-v1.1.md`，md5 `d86f242f3af13fc4f17910973220e583`）
> **上一轮**：GLM 审 `plan/enum-v1.1-plan.md`（432 行 / `5fce38a7…`）报出 C1–C5；本档 §0.3 已逐条裁决（C1/C2/C3/C5①/C5③ 采纳，C4 改为引用既有机制，**C5② 驳回**）
> **本轮性质**：**实施文档首审 + 载荷性实证抽验 + 边缘情况挖掘**
> **实证口径**：本轮直读源码 —— `src/CodeGen/{MetaEmit.cpp, CodeGen.cpp, DeclFun.cpp, TypeMap.cpp, CodeGen.h, DeclEnum.cpp, ExprGen.cpp, ExprClosureCaptures.cpp}`、`runtime/meta.h`（含 PowerShell 物理行号复核）、`runtime/gc/compact.cpp`、`src/Sema/Checker/{DeclChecker.cpp, StmtChecker.cpp, ExprInferMisc.cpp}`、`src/Diag/DiagnosticEngine.cpp`、`test/{sema/test_sema_enum.cpp, rt/test_enum_gc.cpp}` + `awk`/`ReadAllLines` 行号交叉核验

---

## 〇、裁决

**结论：可实施（高质量）；但有 1 🔴 + 6 🟡 需先修。另：GLM 撤回自己上一轮的 C5②（判定其为自己误报）。**

---

## 一、先撤回：**C5② 是 GLM 错了，本档的驳回正确**

本档 §0.3 列了 GLM 上轮主张「`runtime/meta.h` 的 `Interface` 实在 `:31`、区间实在 `:22-32`」并**驳回**，且注明「不得再被订正」。GLM 用**第三方独立方法**（PowerShell `[System.IO.File]::ReadAllLines` 物理行号，独立于本档的 `awk` 与 GLM 的 Read 工具）复核：

```
23: enum class TypeKindBits : uint32_t {
32:     Interface   = 1u << 8,
33: };
```

⇒ **本档正确（`:23` / `:32` / `:33`），GLM 偏 −1，撤回 C5②。** 本档「不得再订正」的措辞**成立**。

**方法教训（比该条本身更重要）**：这是本会话**第二次** GLM 的行号读数相对物理行号偏 −1（上一次是 `src/TokType.h` 的 N-1 误报，同被文档驳回）。⇒ **GLM 此后凡仅凭 Read 输出得出的行号，必须经 `grep` / PowerShell 物理行号交叉验证后才主张**（本轮起已执行：凡未交叉验证者一律不作判决依据）。

---

## 二、载荷性抽验（本轮直读源码，逐条命中）

| 本档断言 | 核验结果 |
| :--- | :--- |
| `MetaEmit.cpp` 的 `typeKindExpr` 位表 = 9 项，末项 `1u << 8, "TypeKindBits::Interface"`；**fallback 返回 `"0"`** | ✅ **逐字精确**（`kB[]` 九项齐全；`return out.empty() ? std::string("0") : out;` 实读命中）⇒ 本档「漏改位 ⇒ 静默渲染 `static_cast<uint32_t>(0)`」的机制描述**成立** |
| Step 0 准入 gate：第一遍末遍历 `program.decls`，`EnumTypeExpr` 而 `enumTypes_.count(t->name) == 0` ⇒ `error(*t, "enum '…' SemType is missing at the end of the first pass … change.md §4.8 Step 0")` | ✅ **逐字精确**；其上方注释已列「四项依赖同时失效（值接收者窄守卫 / `isEnumRecv` 的 const / `genEnumDecl` 取 variant 表 / `enumUnderlyingType`）」 |
| 生成期硬失败：`genMethodDecl` 内 `declaredEnumNames_.count(recv) && enumTypes_.count(recv)==0` ⇒ `error(decl, …)` + **立即 `return`**（注释标注 **R-1（GLM 终审）**） | ✅ **逐字精确** ⇒ GLM 在 v1 终审提出的 R-1 已被实施 |
| `TypeMap.cpp` 的 enum 分支：裸壳名（过 `safeName`）+ 跨模块 `declModule + "::" + …` 前缀 + 空名 `/* unknown_enum */` | ✅ 精确 |
| 编译期常量白名单 = `IntLiteral`/`FloatLiteral`/`BoolLiteral`/`StringLiteral`/`NoneLiteral` + `Enum.Variant` + `RecordExpr` | ✅ 精确；**并暴露 E3**（见 §三） |
| `CodeGen.h`：`std::map<std::string, const EnumSemType*> enumTypes_;`（`:1058`）与 `std::set<std::string> declaredEnumNames_;`（`:1063`） | ✅ 精确（类型与文档描述**完全一致**） |
| `compact.cpp`「步骤 3 缺 `isGCAddress`、步骤 1 已加固」的不对称 | ✅ 主张为真（步骤 1 守卫 `if (fieldPtr && *fieldPtr && isGCAddress(*fieldPtr))` vs 步骤 3 `if (obj && obj->forwarded())`；注释逐字「脏值（0x1 等）即未定义行为」） |
| `DeclEnum.cpp` `emitBoundValueAccessors` 签名含 `cpp`、`:196` `"_val_" + safeName`、兜底注释逐字点名「record / 数组 / 另一 enum 变体 / NoneLiteral ⇒ 批 B…本批不产」 | ✅ 逐字精确 |
| `ExprClosureCaptures.cpp` 门控 `!currentReceiverName_.empty() && name == currentReceiverName_`；`DeclFun.cpp` 值接收者分支 `const <Recv> self = *this;` 且**不设** `currentReceiverName_` | ✅ 逐字精确 ⇒ L-3② 的「**可能不可达**」推断**成立**（本档已正确地把它降为「先探针」） |

⇒ **本档 §4.4 的 C4 结论「四段机制已在位 ⇒ `enumTypes_` 时序不是开放风险」成立** —— GLM 上轮列为「唯一硬前置」的风险，已由**既有 Step 0 gate + 生成期硬失败**双兜底闭环 ✅。这是本档相对 plan 稿最重要的一处实质提升。

---

## 三、新发现的问题

### 🔴 E1：(iii) 档缺 `canonicalName` 空守卫 ⇒ 匿名 record 关联值产 `static * _val_A();`（**两条守卫都漏网**）

本档 §4.7 的守卫链是：

```cpp
if (cppTy.empty() || cppTy.rfind("/* unknown_", 0) == 0) { 报错; continue; }
```

而 `mapSemType(RecordSemType)` 的产物是 **`canonicalName + "*"`**（本档 A2 自述「未过 `safeName`」）。`src/Sema/Checker/DeclChecker.cpp` 的白名单只做 `dynamic_cast<const RecordExpr*>`，**不要求具名**；同一处注释逐字写明该节点「**`e.typeName` 非空即具名字面量**」⇒ **存在 `typeName` 为空的形态**。若该匿名 record 字面量被 Sema 放行为关联值，其 `valueType` 的 `canonicalName` 为空 ⇒ `cppTy == "*"`：

- `"*"` **非空** ✅ 过第一道守卫；
- `"*"` **不以 `/* unknown_` 开头** ✅ 过第二道守卫；

⇒ 生成 `static * _val_A();` + `* Wrap::_val_A() { … }` ⇒ **坏 C++，且只有 `g++` 能抓**（正是本档反复申明「不可接受」的那一类错）。

**修法（成本一行）**：守卫补 `if (cppTy == "*" || cppTy.front() == '*') { 报错; continue; }`；或在 (iii) 分支前先断言 `RecordSemType::canonicalName` 非空。**并补负例**：匿名 record 关联值 ⇒ 断言**报生成期错**（而非产坏码）。

### 🟡 E2：访问器返回类型**首次打破「只用内建类型名」的隐含前提** ⇒ 同模块**乱序声明**会产坏 C++

形式 A 的返回类型全是内建名（`int32_t` / `double` / `bool` / `aura_rt::GcString*`）⇒ **天然免疫声明顺序**。新增的 (ii)/(iii) 两档首次返回**用户类型**（`Color` / `Point*`）。而壳 struct 与 record 都在 A 遍**按源码顺序**发射 ⇒ 若源码为

```aura
enum type Slot<Color> = { empty = Color.Red }   // Slot 在前
enum type Color = { Red, Green }                 // Color 在后
```

则 header 中 `struct Slot { … static Color _val_empty(); };` 先于 `Color` 的声明出现 ⇒ **`'Color' does not name a type`**（C++ 允许**函数声明**按值返回不完整类型，但**类型名必须先被声明**；指针档 `Point*` 同理需要 `Point` 至少被声明）。

**修法（二选一）**：
- ① **生成器在壳 struct 前为被引用的关联值类型发射前向声明**（`struct Point;` / `struct Color;`）——一行级、纯生成侧、不改语义；
- ② 先裁定「Aura 是否允许乱序类型引用」（若 Sema 已强制声明先于使用，本条自动消解）。

**建议**：把「**声明顺序颠倒**」加进 R9 / T-B7 的两模块探针 —— 一次探针同时验「跨模块」与「同模块乱序」，否则这类错要等 `g++` 在很晚的环节才暴露。

### 🟡 E3：判据表列了 4 种 (iii) 形态，而 Sema 白名单**只放行 1 种**（缺交叉自查）

§4.7 判据表把 (iii) 覆盖写作「`RecordSemType`（含元组）/ `ListSemType` / `OptionalSemType`」，但白名单（`DeclChecker.cpp:536-563`）**不含数组字面量、不含 `some(...)`（`CallExpr`）** ⇒ **`ListSemType` / `OptionalSemType` 作为顶层关联值类型疑不可达**（死分支）。

⇒ 这正是本仓「**实现章节与边界章节必须交叉自查**」的教训点。**动作**：在白名单面复核后，把这两档标注为「防御性分支（现行 Sema 不可达）」或补白名单。**并注意**：`NoneLiteral` 的 `valueType` 究竟是 `NoneSemType` 还是 `Optional<…>`（本档 A3 已挂 **T-B6** ✅）—— **若为后者，它会落进 (iii) 而非本档所写的 (ii)**，两种走法的产物差异极大（有无 Global 根）。

### 🟡 E4：`MetaKindTeeth` 的**全产物否定断言**必然假红

§5.2 的 `EXPECT_NOT_CONTAINS(r.unit.metaImpl, "static_cast<uint32_t>(0)")` 是**全产物**断言；而 `typeKindExpr` 与 `symbolFlagsExpr` 的 fallback **同为 `"0"`** ⇒ 任何 **flags/kind 为 0 的其它行**（如符号表的 flags=0）都会渲染出 `static_cast<uint32_t>(0)` ⇒ 用例**因无关原因失败**（本仓已有「断言偏差 D-B3-1/D-B3-7」的前例）。

**修法**：复用同一节**已写好**的 `typeRowOf(meta, "Color")` 把否定断言**收窄到该行** —— 牙齿不减（漏改位表时该行仍渲染 `static_cast<uint32_t>(0)`），假红消除。

### 🟡 E5：§4.7 的默认产物（模板①）与 §5.2 用例源**自相矛盾**

§4.8 的三步判据为「**≥2 次分配 ⇒ 模板②**」，而 §5.2 的 `BoundGlobalRoot` 用例源是 `Point { x = 1, name = "ON" }` —— **含 1 个 string 字段** ⇒ 分配点 = record 本体 + 字符串 intern（**≥2**）⇒ 依其**自身判据**应落**模板②**；但 §4.7 落地代码写的是**模板①**，且本档未点明「本用例预期走②」。

⇒ 若 Step 10 不真跑 T-B2 就照 §4.7 落①，该形态存在「alloc 记录 → alloc 字符串（可触发 GC 移位）→ 再注册**陈旧**指针」的窗口 ⇒ **悬垂**。

**动作**：Step 10 **必须**把**该用例**的分配点数实测落盘，并在文档写明「本用例预期 ①/②」，以免重演「实现章节与测试章节边界语义未交叉自查」（本仓 feature-18 P1 的原型教训）。

> 值得肯定：该用例的断言（`GcRootHandle<Point*> _h{` / `GcRootScope::Global` / `return _h.get();`）**对两版模板都成立** ⇒ 模板切换不破坏用例 ✅。

### 🟡 E6：跨模块 enum 的 **kind** 未进 R9 / T-B7

R9 / T-B7 只验「record 定义可见性」。但 **改动 4 的判据容器 `enumTypes_` 只装填「本单元 AST 的 `TypeDecl`」**（`CodeGen.h` 注释实读明写）⇒ 若 `kTypeTable` 中出现了**导入模块**的 enum 行，`enumTypes_.count(name) == 0` ⇒ **静默回落 `Record`**（L-2 对跨模块 enum 失效且无报错）。

**动作**：在 T-B7 的两模块探针里**同时**断言「导入 enum 的 kind」；若确为 `Record` ⇒ 判据需扩展。**注意**：勿新增第二套判据容器（会与本档 C4「不新增机制」冲突）—— 先探针定事实。

### � E7：`BoundNoRootForEnumValue` 的反向断言**面过宽**

`EXPECT_NOT_CONTAINS(unit.impl, "GcRootHandle")` 断言**整个 impl**。该源含 `fun main(io: Io)`；impl 中若因**其它通路**（io / 闭包捕获 / 驱动代码）合法出现 `GcRootHandle` ⇒ **假红**。

**修法**：切片到 `_val_empty` 的函数体（或断言精确串 `GcRootHandle<Color>`）。

### 🟢 其余（提示级）

- **E8**：(i) 档用 **AST 种类**判、(ii)/(iii) 用 **SemType** 判 ⇒ 判据二元化。本档 §9.4 已警示顺序风险；长期建议统一为 SemType 判据，否则白名单一扩展就会出现「字符串值落进 (iii) 产 `GcRootHandle<GcString*>`」的错位。
- **E9**：模板②的 `set` 按 `handles.h` 是**不持锁**写 `val_` ⇒ 与并发 compact 的 `memcpy` 存在理论竞争；已被本档「并发面登记为已知（不测）」总括覆盖，建议在 R3 补一句明写，免后来者误判为「已保证线程安全」。
- **E10**：L-1 产物**当前零消费**（v1 无关联值读取语法，`grep _val_ src/` 零命中）⇒ 性质是**预留接口**，收益**不可由运行期行为证明**；本档 §1 已如实标注 ✅ —— 仅提示勿据此宣称为「用户可见功能」。
- **E11（未抽验）**：`forceCompactAll()` 是否在测试框架真实存在（§5.4 依赖；编译期即暴露，低风险）；`test_codegen_frames_p4b1.cpp:57-82` 的 helper 模板形态（本档 A6 已挂 ✅）。

---

## 四、值得肯定（本档相对 `plan` 稿的实质提升）

1. **C1 采纳且判据更硬**：把「是否聚合**含** GC 指针」改成「**映射后 C++ 类型是否以 `*` 结尾**」，并以 D-1 给出偏差理由（纯标量字段的 record 仍是 `GcObject`、仍会被 compact 搬走 ⇒ 裸 static 指针悬垂）——**这个纠偏比 plan 原稿更安全**。
2. **C2 采纳且写了能力边界**：明确「模板② **不消除**表达式内部窗口」「选择由 T-B2 定，**禁**『②更安全』的表述」⇒ **拒绝了虚假安全感** ✅。
3. **C4 处理得当**：不新增机制、不重复报错，改为引用**已在位**的四段 —— GLM 实证其中 ③（Step 0 gate）与 ④（生成期硬失败）**逐字在位** ✅。
4. **自纠机制罕见地强**：§0.4 D1–D6 漂移清单（含把 GLM 的 C5② 驳倒并**留证**）、§9.2 A1–A8 待核实 API 逐条给验证方式、§9.4 主动列出三处最脆弱点、§4.9 删除/替换项逐条 —— 这是本仓少见的**可审查性**。
5. **探针先行的纪律**：L-3 两项**不写码、只探针**（且明确「禁止在未探针前写码」）✅，并给出「0 命中须先确认产物文件非空」的防假通过条件 ✅。

---

## 五、必改优先级

| 优先级 | 项 | 动作 |
| :-: | :--- | :--- |
| **1** | **E1** 🔴 | (iii) 守卫补 `"*"` / 前导 `*` 拦截；补匿名 record 负例（断言**报错**而非产坏码） |
| **2** | **E2** 🟡 | 生成器补被引用类型的前向声明；把「乱序声明」并入 T-B7 探针 |
| **3** | **E5** 🟡 | Step 10 真跑 T-B2，落盘**该用例**的分配点数；文档写明该用例预期模板 |
| **4** | **E4 / E7** 🟡 | 两处否定断言**收窄到目标行 / 目标函数体** |
| **5** | **E3 / E6** 🟡 | 白名单交叉自查（List/Optional 可达性、NoneLiteral 落档）；T-B7 补「导入 enum 的 kind」断言 |
| 6 | E8–E10 | 提示级，可选加固 |

---

## 六、总评

本档的**证据层与自纠机制**（D 清单、A 清单、§9.4 脆弱点、C4 裁决、探针先行）已达到可实施标准；四项源码机制经 GLM 抽验**全部在位**（含 GLM 上轮列为唯一硬前置的 `enumTypes_` 时序风险，已被既有 gate + 硬失败双兜底闭环）。

剩余风险集中在 **(iii) 档新增返回类型所引入的两个新面**：

- **E1**：`canonicalName` 空 ⇒ `cppTy == "*"`，两条守卫**都漏网**（坏 C++，只有 `g++` 能抓）；
- **E2**：首次返回**用户类型名** ⇒ 同模块**乱序声明**时声明早于类型声明（坏 C++，只有 `g++` 能抓）。

这两处**恰好都是「新增形态首次打破既有隐含前提」**——与历轮同源：**既有通路查得很细，新造形态的隐含前提没人验**（既有形式 A 因只用内建名而天然免疫，故该前提从未被记录过）。

<!-- 本回复由 GLM 生成于 2026-10-17。审查对象：change.md md5 dbd6ba64d1c43ec15994d73f681b5dd1（989 行 / 88,396 B）。GLM 本轮撤回自身 C5② 误报（meta.h 行号本档正确），并确立「行号须经 grep/PowerShell 交叉验证」的取数纪律。 -->
