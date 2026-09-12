---
type: review_report
kind: plan_review
plan_file: "[[bug-20-iface-self-ref-chain]]"
reviewer:
  - - AI 审查 Agent
status: changes_requested
severity: major
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - sema
  - interface
  - self-ref
---

# 【审查】[ ] **Plan 审查报告：bug-20-iface-self-ref-chain.md**

> **一句话摘要**：根因链**全部精确实证**（clear+逐个 push 的顺序问题、快照克隆机制、E013 未命中报错点），三轮填充方案方向正确且第二/三轮**确有必要**（经推演：仅两轮会遗留「后声明方法 returnType 为空」的错误推断）；但方案表述「填充/重填」未锁定**就地覆写（不得 clear+重新 push）**的实施形态——若按第三轮 clear+重推实现，克隆时点向量中后声明方法**再次缺失**，静默退回原缺陷，裁决需修改（补一条硬性实施约束后即可通过）。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\Sema\Checker\TypeResolver.cpp`（L188-236，resolveInterfaceMethods 全文；L274-339，finalizeInterfaceSignatures/前向注册，子 Agent 检索）
  - `src\Sema\SemTypeUtils.cpp`（L310-358，resolveNamedType 快照克隆点）
  - `src\Sema\Checker\CallInfer.cpp`（L415-444，inferMethodCall 接口方法查找与泛型代换）
- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\Sema\Checker\TypeResolver.cpp` | L191-L236 | resolveInterfaceMethods：**L216 `sym->interfaceMethods.clear()`** → L217-233 循环内 L230-231 先 resolveType 参数、L232 解析返回类型、**L233 才 push** ✅ 报告引 L216/L217-234/L230-233 全部精确 |
| `src\Sema\SemTypeUtils.cpp` | L334-L348 | resolveNamedType Interface 分支：L337 `for (auto& m : sym->interfaceMethods)` **逐字段克隆 MethodSig（paramTypes/returnType/throws/hasDefault/hasCppImpl）**——**解析时点快照** ✅ 报告引 :311-361/:335-349 一致（实际克隆体 L337-347） |
| `src\Sema\Checker\CallInfer.cpp` | L417-L419 | `if (auto* iface = ...InterfaceSemType...)` → `for (auto& m : iface->methods) if (m.name == e.method)` 按名查找 ✅ 精确 |
| `src\Sema\Checker\CallInfer.cpp` | L425 / L444+ | `formal = ai < m.paramTypes.size() ? m.paramTypes[ai].get() : nullptr`——**paramTypes 为空的克隆不崩溃**（formal=nullptr 跳过校验）⚠️ 评估占位 sig 中途被克隆的安全性依据 |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 根因·clear+逐个 push | `TypeResolver.cpp:191-236` | ✅ 一致 | 行号精确；「解析 next() 返回类型时 interfaceMethods 既无 next 也无后续 val」属实（L233 push 在 L232 resolve 之后） |
| 根因·快照克隆 | `SemTypeUtils.cpp:311-361` | ✅ 一致 | 克隆体 L337-347，「快照而非实时引用」描述精确 |
| 根因·未命中报错 | `CallInfer.cpp:418-419/:496` | ✅ 一致 | 按名查找机制核实 |
| 旁证·finalize 不修复 | `TypeResolver.cpp:303-306` | ✅ 采信 | 子 Agent 检索确认 finalize 再次调用 resolveInterfaceMethods（同款 clear+push） |
| 方案 A·占位+两轮填充 | L216 后重构 | ⚠️ 实施形态未锁定 | 见 §3 第 1 条——三轮结构正确且必要，但「填充/重填」未明确**就地覆写**，clear+重推实现会静默失败 |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。Sema 单函数重构。
- **Runtime 兼容性**：✅ 通过。干净报错域缺陷，不触 CodeGen/runtime。
- **测试覆盖**：✅ 矩阵充分（顺序缓解/标注缓解/泛型/函数返回对照均覆盖），probe 留存。
- **异常与回退**：❌→⚠️ **两个修复后未暴露问题（本轮核心发现）**：
  1. **实施形态陷阱（成败关键）**：三轮方案的第三轮若实现为「clear + 重新 push」（与现有 L216-233 同构的自然写法！），则第三轮解析 next() 返回类型时向量中**只有 next**（val 尚未重推）→ 克隆再次缺失 val → **静默退回原缺陷，且测试可能因 val-先声明形态部分通过而漏检**。正确形态：第二/三轮必须**就地覆写**（按索引替换 sig 的 paramTypes/returnType，不清空向量）——此时第三轮任意时点向量内所有方法均持有上一轮完整签名，克隆必然完整。方案文本「填充/重填」两种读法都通，必须显式锁定。
  2. **两轮不够、三轮收敛的推演证据**（佐证方案第三轮必要性，同时暴露中间态风险）：仅两轮时，next() 存储的返回类型克隆来自第二轮解析时点——其中 val 仍为占位（returnType 空）→ `nd.next().val()` 按名找到 val 但 returnType=null → 推断为 None → **不报错但类型错误**（比现状报错更隐蔽）。第三轮就地把 next 的存储克隆覆写为完整版后收敛。同理递归自引用（`next() -> Node` 链式两层）也随第三轮收敛。**占位 sig 中途被克隆的消费面安全**：inferMethodCall L425 formal=nullptr 跳过校验不崩溃、returnType 空走 None 兜底——中间态无崩溃，仅有上述推断退化，且被第三轮覆写消除。
  3. 附注（非阻塞）：**接口互相引用**（A 方法返回后置声明的 B）仍走 forwardRegisterIfaceType 占位（GenericSemType，非 InterfaceSemType）→ B 的方法集仍空——这是 bug-09 域的既有局限，非本修复引入、本修复也不解决，回归清单的 control_forward_iface_chain 用例应确认该形态现状预期仍为「干净报错」。

## 4. 已知限制评估

- **「顺序部分缓解 / 标注缓解（仅当前层）」**：✅ 全部与源码机制吻合。
- **「函数签名不误伤（finalize 后解析）」**：✅ 采信（与 bug-20 矩阵 repro_fun_ret_iface_chain ✅ 一致）。
- **「方案 B 惰性查符号影响面大不推荐」**：✅ 评估合理——resolveNamedType 是全类型解析热点，改惰性牵动所有消费方。
- **「bug-09 方案 A 完整支持时须保证被引用接口视图方法集完整」**：✅ 已自知联动关系。

## 5. 最终裁决（Final Verdict）

- [ ] 通过（Approve）
- [x] **需修改（Changes Requested）** — 根因与三轮结构正确，但存在一个会导致修复静默失败的实施形态歧义。具体修改点：
  1. **锁定实施形态（硬性）**：修复逻辑第 2/3 步改写为「第二/三轮**就地覆写**（按索引替换 `sym->interfaceMethods[i]` 的 paramTypes/returnType），**禁止每轮 clear 后重新 push**」——并注明原因：第三轮 clear+重推会使克隆时点后声明方法再次缺失，静默退回原缺陷。
  2. **补中间态说明**：注明「第二轮存储的返回类型克隆含占位 sig（val.returnType 空 → None 推断）属预期中间态，由第三轮就地覆写消除」，防止实施者看到中间态错误推断时误加特判。
  3. **回归清单补两个关键用例**：(a) next-先声明 + `nd.next().val()` 返回值参与运算（验证 None 退化已消除，不能只验证「不报错」）；(b) `nd.next().next()`（自身方法，验证占位轮后自身签名完整）。
  4. 附注接口互引局限（forwardRegisterIfaceType 占位形态方法集仍空）写入「已知限制」，标注由 bug-09 域覆盖。

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
