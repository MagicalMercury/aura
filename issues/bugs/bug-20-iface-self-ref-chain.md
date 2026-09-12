---
type: bug_report
module: Sema
sub_module: resolveInterfaceMethods（TypeResolver.cpp:191-236）/ resolveNamedType（SemTypeUtils.cpp:311-361）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-29
related_issues:
  - "[[bug-09-iface-late-interface]]"
tags:
  - interface
  - self-ref
  - method
  - sema
---

> [!note] 审查状态
> **已按审查报告修改**（2026-08-30）：锁定第二/三轮「**就地覆写**」实施形态（禁止每轮 clear 后重新 push）；补占位中间态说明与消费面安全；回归清单补 `nd.next().val()` 返回值参与运算、`nd.next().next()` 两个关键用例；接口互引局限写入已知限制（bug-09 域）。
> **原裁决摘要**：`changes_requested`（major）——根因链全部精确实证，三轮填充方向正确且第二/三轮确有必要，但方案「填充/重填」未锁定**就地覆写**实施形态：第三轮若实现为 clear+重推，克隆时点向量中后声明方法再次缺失，**静默退回原缺陷**（且 val-先声明形态部分通过可能漏检）。修复后本笔记须与 [[review-bug-20-iface-self-ref-chain]] 裁决一致。

# 【接口自引用链式调用】接口自引用方法返回自身时调用点链式调用方法集空（next().val() 报 has no method）
[x] **主标题：resolveInterfaceMethods 先 clear 再逐个 push → 返回自身视图方法集不完整 → 链式调用报 has no method**

> **一句话摘要**：接口自引用 `interface Node { next() -> Node; val() -> int }` → `nd.next().val()` 报 `interface 'Node' has no method 'val'`——resolveInterfaceMethods 解析 next() 返回类型时 interfaceMethods 尚未含自身及后续方法 → resolveNamedType 复制出空/不完整方法集快照（干净报错，非坏 C++）。

## 1. 调研背景与发现
- **发现时间**：2026-08-29（调研「接口后置接口」时发现，独立缺口）。
- **触发场景**：接口自引用方法返回自身 + 调用点链式调用。
- **影响范围**：凡「resolveInterfaceMethods 逐个 push 过程中 resolveType 遇到引用接口自身（或解析顺序在其后的接口）的名字」均同源（返回类型直接/嵌套/泛型实参/默认方法/默认方法体）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：resolveInterfaceMethods（TypeResolver.cpp:191-236）L216 clear 先清空 → L217-234 按声明顺序逐个方法 push（L230-231 先 resolveType 参数、L232 解析返回类型、L233 才 push 当前 sig）→ 解析 next() 返回类型（引用自身 Node）时 interfaceMethods 既无 next 也无后续 val → resolveNamedType 复制空/不完整方法集快照。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：
  - `src\Sema\Checker\TypeResolver.cpp:191-236`（resolveInterfaceMethods clear + 逐个 push 顺序问题）。
  - `src\Sema\SemTypeUtils.cpp:311-361`（resolveNamedType L335-349 遍历 sym->interfaceMethods 克隆 MethodSig，是解析时点快照而非实时引用）。
  - `src\Sema\Checker\CallInfer.cpp:417-498`（inferMethodCall L418-419 按名查 iface->methods，L496 未命中报 has no method）。
- **CodeGen 相关路径**：不涉及（干净报错，非坏 C++）。

### 2.2 关键逻辑细节
- **顺序部分缓解**：val 先声明 → 处理 next 时 interfaceMethods 已含 val → nd.next().val() ✅；但 next 自身未 push → nd.next().next() 恒失败。
- **finalize 二次解析不修复**：finalizeInterfaceSignatures（TypeResolver.cpp:303-306）再调 resolveInterfaceMethods 也是 clear+逐个 push，结果相同。
- **函数签名不误伤**：DeclFun/DeclChecker 的签名 resolveType 发生在 finalize 之后（interfaceMethods 已完整）。

## 3. 影响范围（Scope）
- **结论**：返回类型直接/嵌套引用自身、默认方法声明在引用点之后、默认方法体 self 链式、泛型接口自引用——统一报 has no method（干净报错）。缓解因素：① 方法声明顺序（自身恒缺）；② 显式类型标注 let（仅当前层）；③ 函数签名（finalize 后完整）。
- **不受影响路径**：顶层接口变量方法调用、函数返回/参数接口、接口方法参数引用自身（isAssignable 只看 name）、record 方法链、前置接口引用链式。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_ret_self.aura` | next 先 val 后，nd.next().val()（主线） | v=5 | ❌ has no method 'val' | 同源 |
| `repro_ret_self_chain2.aura` | next 先，nd.next().next() | 编译运行 | ❌ has no method 'next'（自身恒缺） | 同源 |
| `repro_ret_self_val_first.aura` | val 先 next 后，nd.next().val() | v=5 | ✅ v1=5 | 顺序部分缓解 |
| `repro_ret_self_annot.aura` | let n2: Node = nd.next(); n2.val() | v=5 | ✅ v=5 | 标注缓解（仅当前层） |
| `repro_ret_self_generic.aura` | 泛型接口 Box\<T\> 自引用 | 编译运行 | ❌ has no method 'val' | 同源 |
| `repro_fun_ret_iface_chain.aura` | 函数返回 Node，getNode().val() | v=7 | ✅ v=7 | 不误伤（finalize 后解析） |
| `control_no_self.aura` | 无自引用接口 + 直接调 | v=42 | ✅ 编译运行 | 不误伤 |
| `control_record_chain.aura` | record 方法链 r.next().val() | v=5 | ✅ 编译运行 | 不误伤 |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\Sema\Checker\TypeResolver.cpp:191-236`（resolveInterfaceMethods）。
- **修复逻辑**（方案 A 推荐，占位 + 三轮填充，第二/三轮**就地覆写**）：
  1. L216 clear 后第一遍遍历 i.methods 先 push 占位 sig（仅 name/throws/hasDefault/hasCppImpl，CppBridge 同样占位）→ 后续 resolveType 复制到的 interfaceMethods 至少含全部方法名。
  2. 第二遍逐个 resolveType **就地覆写**：按索引替换 `sym->interfaceMethods[i]` 的 paramTypes/returnType（**不清空向量、不重新 push**）。
  3. 第三遍再次逐个**就地覆写**（此时 interfaceMethods 全部签名完整 → resolveNamedType 复制到完整视图，含自身与后续方法完整签名）。
  4. **禁止每轮 clear 后重新 push（硬性约束）**：第三轮若实现为「clear + 重新 push」（与现有 L216-233 同构的自然写法），第三轮解析 next() 返回类型时向量中只有 next（val 尚未重推）→ 克隆再次缺失 val → **静默退回原缺陷**；且 val-先声明形态部分通过可能漏检。就地覆写则第三轮任意时点向量内所有方法均持有上一轮完整签名，克隆必然完整。
  5. allowForward 前向注册（L203-215）、CppBridge 跳过（L223-229）、泛型 typeParams（L196-202）均不变。
- **中间态说明（防实施者误加特判）**：第二轮存储的返回类型克隆含占位 sig（如 val.returnType 空 → `nd.next().val()` 推断 None）属**预期中间态**，由第三轮就地覆写消除。消费面安全：inferMethodCall（CallInfer.cpp L425）formal=nullptr 跳过校验不崩溃、returnType 空走 None 兜底——看到中间态错误推断时**勿加特判**。
- **配套修复**：方案 B（resolveNamedType 惰性查符号）影响面大不推荐；bug-09 方案 A 完整支持时须保证被引用接口视图方法集完整。

### 已知限制（Known Limitations）
- **接口互相引用不解决**：接口 A 的方法返回后置声明的接口 B 时，B 仍走 forwardRegisterIfaceType 占位（GenericSemType，非 InterfaceSemType）→ B 的方法集仍空——这是 bug-09 域的既有局限，非本修复引入、本修复也不解决，由 bug-09 域覆盖。回归清单 control_forward_iface_chain 用例该形态现状预期仍为「干净报错」。

## 6. 回归验证清单（Regression Checklist）
- [x] `control_no_self.aura` / `control_self_direct_val.aura` / `control_self_direct_next.aura` / `control_forward_iface_chain.aura` / `control_record_chain.aura` 保持 ✅
- [x] **next-先声明 + `nd.next().val()` 返回值参与运算**（`repro_ret_self_arith.aura`，`let v: int = nd.next().val() + 1`）——验证 None 退化已消除（v=6，返回值为 int 而非 None）
- [x] **`nd.next().next()`**（`repro_ret_self_chain2.aura`）——验证占位轮后自身签名完整
- [x] 全部 probe_*.aura 保持 ✅
- [x] `iface_ref_late_interface\` 的 control_forward / control_forward_impl / control_self_ref / control_self_ref_val_direct / repro_builtin_iface_ret 复测 ✅

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\iface_self_ref_chain\`
- **留存产物**：23 个 .aura（含新增 `repro_ret_self_arith.aura`）+ `.gen.cpp / .gen.exe`

## 8. 修复记录
- **状态**：已修复（2026-08-31，批次 4 第六项）。
- **实现**：`src\Sema\Checker\TypeResolver.cpp:216-245`（resolveInterfaceMethods）——第一遍 L216 clear 后遍历 `i.methods` push 占位 sig（仅 name/throws/hasDefault/hasCppImpl，CppBridge 同占位）；第二/三遍 L234-244 按索引**就地覆写** `sym->interfaceMethods[idx]` 的 paramTypes/returnType（CppBridge 跳过，同原逻辑）。**未使用每轮 clear+重推**（硬性约束，与审查点 1 一致）。
- **中间态验证**：第二轮 next.returnType 克隆含占位 sig（val.returnType 空 → None 推断）确为预期中间态，第三轮就地覆写消除；消费面（inferMethodCall formal=nullptr 跳过 / returnType 空走 None 兜底）无崩溃，未加特判。
- **两关键用例结果**：
  - 审查点 3a：`repro_ret_self_arith.aura`（next-先声明 + `nd.next().val() + 1`）→ v=6 ✅（None 退化消除，int 运算）；
  - 审查点 3b：`repro_ret_self_chain2.aura`（`nd.next().next()`）→ 编译运行 ✅（自身签名完整）。
- **接口互引局限确认**：A 方法返回后置声明的接口 B 仍干净报错 `interface 'B' is declared after this interface method signature references it`（bug-09 域，本修复不解决，符合已知限制）。
- **独立缺陷（随本修复暴露并已修复，见新登记）**：
  - `[[bug-43-generic-iface-return-typeargs-substitute]]`：substitute 不递归 InterfaceSemType → 泛型接口返回类型 typeArgs 未代换（`Box<auto>`）——`src\Sema\GenericSubstitution.cpp:126-149`。
  - `[[bug-44-stmtlet-generic-iface-view-typeargs]]`：StmtLet 无标注接口视图 let 用裸 `is->name`（`ViewRoot<Box>`）——`src\CodeGen\StmtLet.cpp:349` 改 `mapSemType(*is)`。
  - 二者均为既存潜在缺陷（非自引用泛型接口返回另一泛型接口形态始终可达），经 `repro_ret_self_generic.aura` 与临时非自引用用例（v=42，用后删）确认。
- **测试**：单测新增 7 条（test\sema\test_sema_interfaces.cpp，SemaInterfaces.IfaceSelfRef* 系列 + 对照组）；全量 aura_tests 1124 → 1131，0 新增失败（基线 1 个 pre-existing Examples.TestGcMutex 路径错位除外）；`example\used\1-6.aura` 全量编译运行通过。

---
**当前状态**：`2026-08-31` 已修复（批次 4 第六项）
