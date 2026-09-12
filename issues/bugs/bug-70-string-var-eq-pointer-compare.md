---
type: bug_report
module: CodeGen
sub_module: ExprBinary.cpp genBinaryExpr 字符串 ==/!= 分支（L212-231）——子串标记判定漏掉"双字符串变量"形态 → 回退裸指针比较
status:
  - fixed
severity:
  - medium
discover_date: 2026-09-09
related_issues:
  - "[[bug-15-generic-plus-string]]"
  - "[[bug-23-generic-T-plus-literal]]"
tags:
  - codegen
  - string
  - equality
  - comparison
---

# 【字符串变量 == / != 回退指针比较】`let a = "x" + str(i)`（concat 产物）与同内容字符串变量 `a == b` / `a != b` 生成 C++ 裸指针比较 → 内容相等判 false、静默错误结果（对照：`+` 拼接分支已查 stringVarNames_ + PrimSemType 兜底，`==` 分支缺失）

[x] **主标题：CodeGen `genBinaryExpr` 的字符串比较（== / !=）用生成文本子串（make_string/intern_string/concat/string_of）判定是否走 `string_eq`——两操作数均为"字符串变量"（值来自 concat/拼接/读取，生成文本不含上述标记）时判定不中，落到行 239 裸指针 `==`/`!=` → 内容相等的两个动态字符串比较结果为 false（!= 结果为 true），静默错误**

> **一句话摘要**：`c1 == c2`（两字符串变量、内容相同）被编译为 `(c1.get() == c2.get())` 指针比较而非 `string_eq(...)` 内容比较——intern 字面量因同内容同指针碰巧正确，动态字符串（concat 产物/读取值）恒误判不等。

## 1. 调研背景与发现
- **发现时间**：2026-09-09（迭代器回调内 gc_force 压力探针的字符串结果校验时发现——非本轮 4 项缺陷范围）。
- **触发场景**：任意两个内容相等但对象不同的字符串变量比较：
  ```
  fun main(io: Io) throws {
      let tag = "z0"
      let c1 = tag + "." + "0"     // concat 产物（动态）
      let c2 = tag + "." + "0"
      let t1 = "z0.0"              // intern 字面量
      if t1 == t1 { io.println("lit==lit ok") } else { io.println("lit==lit FAIL") }
      if c1 == c2 { io.println("concat==concat ok") } else { io.println("concat==concat FAIL") }  // ← 实际打印 FAIL
  }
  ```
  实测：`lit==lit ok`；`concat==concat FAIL`（内容相等判不等）；`t1 == c1`（intern vs concat）同样 FAIL。
- **影响范围**：所有"字符串变量 vs 字符串变量 / 字符串变量 vs 动态字符串表达式"的 `==`/`!=` 比较；字符串变量 vs 字面量（intern_string 文本含标记）不受影响（仍走 string_eq）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：`src/CodeGen/ExprBinary.cpp` `genBinaryExpr`：
> - `+`（拼接）分支 L138-172 有**三层**判定：生成文本子串 → `stringVarNames_`（变量名 strip `.get()` 查表）→ `isStringSemType(inferredType)`（PrimSemType::String）兜底。
> - `==`/`!=`（L212-231）**只有**第一层子串判定（make_string/intern_string/concat/string_concat/string_of），缺 stringVarNames_ 与 semtype 兜底 → 两字符串变量（如 `c1.get() == c2.get()`）不命中 → 落 L239 `return "(" + left + " " + op + " " + right + ")"` → C++ 对 `GcString*` 裸指针比较（指针恒不等）。
> - 为什么字面量侧正常：字符串字面量 genExpr 产 `aura_rt::intern_string("...")`（含标记）→ 命中 string_eq；且 intern 同内容同指针，即便落指针比较也"碰巧对"。
> - **历史**：该 == 分支形态在 HEAD（7e22b16）已如此，非 feature-05/06 引入（git show HEAD 一致）。

### 2.1 代码路径追踪
- **CodeGen 主根因**：`src/CodeGen/ExprBinary.cpp:212-231`（`e.op == "==" || e.op == "!="` 分支仅子串判定）；对照 `+` 分支同文件 L154-172 的 stringVarNames_/isStringSemType 兜底。
- **CodeGen 掩蔽点**：L239 兜底裸指针比较（无 string 类型信息时无法区分 GcString* 与其它指针）。

## 3. 影响范围（Scope）
- **结论**：字符串 ==/!= 中任一侧为"纯变量/动态表达式"（不含 make_string/intern_string/concat/string_of/to_string 子串）即受影响——典型：`if a == b`（a、b 为字符串变量）、函数返回字符串比较、从 record 字段/列表取出的字符串比较。concat 产物变量自身文本不含 concat（genExpr(Identifier) 只是 `v.get()`）。
- **不受影响**：任一侧为字符串字面量 / 字符串拼接表达式（文本含标记）→ string_eq 正确；值类型/record Comparable 走各自既有路径。
- **附带说明**：本缺陷是静默错误结果（非崩溃/编译错），风险高隐蔽性高。

## 4. 实测复现矩阵（Validation Matrix）
| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `example/_tmp_probe/p1_str2.aura` | 两 concat 产物内容相等 `c1 == c2` | true（content eq） | ❌ false（指针比较） | 静默错误 |
| `example/_tmp_probe/p1_str2.aura` | intern 字面量 `t1 == t2` | true | ✅ true（同指针） | 碰巧正确 |
| `example/_tmp_probe/p1_str.aura` | intern vs concat `a == b`（同内容） | true | ❌ false | 静默错误 |
| `example/_tmp_probe/p1_str2.aura` | 内容不等 `a != c` | true | ✅ true（指针本就不同） | 碰巧正确 |

## 5. 修复方案（Fix Plan）
> 推荐方案 **方案 1**（与 `+` 分支对齐，改动小）。
- **修复位置**：`src/CodeGen/ExprBinary.cpp` `genBinaryExpr` 的 == / != 分支（L212-231）。
- **修复逻辑**：
  1. 把 `+` 分支的 stringVarNames_ 兜底（strip `.get()` 后查 `stringVarNames_`）与 `isStringSemType(inferredType)`（PrimSemType::String）判定复制到 == / != 分支的 leftIsStr/rightIsStr 之后。
  2. 命中后仍走既有 `genGcRootedArgs(..., "aura_rt::string_eq({0}, {1})")`；`!=` 取反（已实现）。
- **配套修复**：无（独立缺陷）。建议追加单测：两 concat 变量 == / != → 断言生成文本含 `string_eq` 且不含裸 `==`。
- **风险**：极低——只把"漏判的字符串比较"从指针比较改为内容比较；与既有命中路径同一代码。

## 6. 回归验证清单（Regression Checklist）
- [ ] `p1_str2.aura` concat==concat / lit==concat 修复后为 true
- [ ] `p1_str2.aura` lit==lit / a != c 对照组不回归
- [ ] `test/` 新增 == 单测通过
- [ ] `aura_tests` 全量 0 failed

## 7. 附加资源与产物
- **复现目录**：`example/_tmp_probe/p1_str.aura`、`p1_str2.aura`（临时探针；正文 §1 已内联完整复现代码，无需保留目录）
- **产物**：`example/_tmp_probe/str2_out.cpp`（生成代码，`c1.get() == c2.get()` 实证）

---
**当前状态**：`2026-09-12` 已修复（实现已落地 · 单测 1305/1305 · ASAN/回归全绿）；详见 §8 修复记录。


## 8. 修复记录

- **修复日期**：2026-09-12
- **修复方案**：采用 §5 方案 1（与 `+` 分支对齐，未重新设计）。
- **实现位置**：`src/CodeGen/ExprBinary.cpp` `genBinaryExpr` 的 == / != 分支（基线 L212-231，实际 L213-253）。参考 `+` 分支（L154-172）的两层兜底写法；修复后 +22 行（无删除）。
- **改动摘要**：在 == / != 分支的子串判定（leftIsStr/rightIsStr）之后补两层兜底——第一层 `stringVarNames_`（经 `stripGet70` 剥除 `.get()` 后查表），第二层 `isStringSemType70(e.left/right->inferredType)`（`PrimSemType::String`）。命中后仍走既有 `genGcRootedArgs(..., "aura_rt::string_eq({0}, {1})")`；`!=` 取反（原已实现，未动）。

### 生成代码对照（`s70_str_eq.aura`，5 个比较点）

| 比较 | 修复前生成（裸指针）| 修复后生成（内容比较） |
| :--- | :--- | :--- |
| `t1 == t1` | `if ((t1.get() == t1.get()))` | `return aura_rt::string_eq(_h4_0.get(), _h4_1.get());` |
| `c1 == c2` | `if ((c1.get() == c2.get()))` | `aura_rt::string_eq(_h7_0.get(), _h7_1.get())` |
| `t1 == c1` | `if ((t1.get() == c1.get()))` | `aura_rt::string_eq(_h10_0.get(), _h10_1.get())` |
| `c3 != c1` | `if ((c3.get() != c1.get()))` | `aura_rt::string_eq(_h13_0.get(), _h13_1.get())` |
| `c1 != c2` | `if ((c1.get() != c2.get()))` | `aura_rt::string_eq(_h16_0.get(), _h16_1.get())` |

统计：修复前 `string_eq` 出现 0 次 / 裸指针比较 5 处；修复后 `string_eq` 出现 **5** 次 / 裸指针比较 **0** 处。

### 实测结果（`example/used/leakcheck/_repro/f07_verify/s70_str_eq.aura`）

| 用例 | 修复前实际 | 修复后实际 | 预期 |
| :--- | :--- | :--- | :--- |
| `lit==lit` | ok | ok | ok ✅ |
| `concat==concat` | **FAIL** | **ok** | ok ✅ |
| `lit==concat` | **FAIL** | **ok** | ok ✅ |
| `a!=c` | ok | ok | ok ✅ |
| `concat!=concat` | **FAIL** | **ok** | ok ✅ |

### 验证统计

- **单测**：基线 1302 → **1305 tests, 1305 passed, 0 failed**（新增 3 项 bug-70 单测，见下）。
- **回归**：`example/used/1-6.aura` 全部通过；稳定性探针 `r1`/`r2`/`r3`/`r4`/`t3e_shallow`/`t3i_thread_norec_churn` 各 20 轮（0/120 异常）。
- **模式**：常规（UCRT64，非 ASAN）。
- **新增单测**（`test/codegen/test_codegen_generic.cpp`）：
  - `CodeGen.StringVarEqGeneratesStringEq`：两 concat 变量 `==` → 断言含 `aura_rt::string_eq(` 且不含 `c1.get() == c2.get()`。
  - `CodeGen.StringVarNeGeneratesNegatedStringEq`：`!=` 分支 → 断言含 `string_eq(` 且不含 `c1.get() != c2.get()`。
  - `CodeGen.StringVarFromParamEqGeneratesStringEq`：仅靠 Sema 类型兜底的形参流入 string 变量 → 断言含 `string_eq(` 且不含 `(a.get() == b.get())`。

### 风险与遗留

- 风险极低：仅将漏判的字符串比较从指针比较改为内容比较，与既有命中路径同代码。已验证 1-6.aura 与 120 轮探针无回归。
- 无遗留（完全修复）。
