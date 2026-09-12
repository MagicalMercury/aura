---
type: bug_report
module: CodeGen / Runtime
sub_module: record 字面量字段保护（StmtLet.cpp:182-189 / ExprGen.cpp:408-413 / StmtControl.cpp:119-126）/ isHeapSemType（ExprGen.cpp:55）
status:
  - fixed
severity:
  - critical
discover_date: 2026-08-30
related_issues:
  - "[[bug-14-gc-root-self-value-field]]"
tags:
  - gc
  - record
  - generic
  - crash
---

# 【record 字面量 GC 假根】record 字面量字段未绑定泛型 T 值被 GcRootHandle 误包装（bug-14 同源残留，同族三处）
[ ] **主标题：record 字面量字段保护路径（同族三处）对未绑定 GenericSemType("T") 误判堆 → GcRootHandle\<int\> 假根 → GC 崩溃 0xC0000005**

> **一句话摘要**：泛型方法/函数体内 `Box{ value: t }`（t: T）时，record 字面量字段保护路径（同族三处）对未绑定泛型 T 值仍直接生成 `GcRootHandle<int>` 假根 → GC mark 扫描读 int 当根指针 → 0xC0000005。

## 1. 调研背景与发现
- **发现时间**：2026-08-30（修复 bug-14 genGcRootedArgs if constexpr 延迟判定时确认，同源残留）。
- **触发场景**：泛型方法/函数体内 record 字面量字段为未绑定泛型 T 值（`Box{ value: t }` t: T）。
- **影响范围**：凡 record 字面量字段 Sema 类型为未绑定 GenericSemType("T") 的形态，覆盖 let 声明 / 表达式 / return 三处生成路径。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：同族三处均 `isHeapSemType(f.value->inferredType)` → 未绑定 GenericSemType("T") 被 isHeapSemType（ExprGen.cpp:55 return true）误判堆 → 生成 `aura_rt::GcRootHandle<decltype(_fv_...)> _fh_...(_fv_...)`（T=int → GcRootHandle<int> 假根 → GC 崩溃 0xC0000005）。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及（inferredType 正常填充为未绑定 GenericSemType("T")）。
- **CodeGen 相关路径**（同族三处，均直接生成 GcRootHandle，无未绑定泛型分支）：
  - `src\CodeGen\StmtLet.cpp:182-189` - let 声明 record 字面量字段保护。
  - `src\CodeGen\ExprGen.cpp:408-413` - 表达式 record 字面量（genRecordExpr）字段保护。
  - `src\CodeGen\StmtControl.cpp:119-126` - return record 字面量字段保护。
  - `src\CodeGen\ExprGen.cpp:55` - isHeapSemType 对未绑定 GenericSemType 默认 return true（误判点，与 bug-14 同源）。
- **Runtime 崩溃点**：`runtime\gc\handles.h:27` → `runtime\gc\mark_sweep.cpp:89-90` → `parallel_mark.cpp:62` → 0xC0000005。

### 2.2 关键逻辑细节
- **同源关系**：与 bug-14 同一 isHeapSemType 误判堆根因，仅消费点不同（record 字面量字段保护 vs genGcRootedArgs 实参保护）；bug-14 修复（genGcRootedArgs if constexpr 延迟判定）**不覆盖**本条路径。
- **T=record 形态**：decltype=Point* → GcRootHandle<Point*>「碰巧正确」（机制仍错）。

## 3. 影响范围（Scope）
- **结论**：泛型方法/函数体内 record 字面量字段为未绑定泛型 T 值（`Box{ value: t }` t: T）→ GC 触发即崩溃；三处生成路径（let 声明 / 表达式 / return）全部受影响。
- **不受影响路径**：非泛型字段（PrimSemType 正确不包装）、string 字段（正确包装）、已实例化绑定后的具体类型字段。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro30_record_T_let.aura` | 泛型方法 `let b2: Box<T> = { val = self.val }`（T=int）+ gc_force | 编译运行 a=42 | ❌ **实测崩溃 0xC0000005**（exit=-1073741819） | 本条目主线（StmtLet 路径）✅ **修复后 a=42** |
| `repro30_record_T_multifield.aura` | 多字段混合（双 T 假根 + string 包装） | 编译运行 | ❌ **实测崩溃 0xC0000005** | 本条目（边界：多字段独立分支）✅ **修复后 a+s+b=3** |
| `repro30_record_T_return.aura` | return 路径 + 大列表字段触发 | 编译运行 v=42 | ⚠️ 实测碰巧通过（v=42，40 元素大列表未触发 GC——return 单字段假根存活窗口窄） | 本条目（StmtControl 路径）✅ **修复后 v=42** |
| `repro30_record_T_expr.aura` | 表达式路径（实参上下文）+ 大列表字段 | 编译运行 v=42 | ⚠️ 实测碰巧通过（v=42，同上） | 本条目（genRecordExpr 路径）✅ **修复后 v=42** |
| `repro30_record_T_record.aura` | T=Point（预期碰巧不崩→修复后仍保护） | 编译运行 x=3 | ❌ **实测崩溃 0xC0000005**——**超预期，根因是 bug-54**（desc 层不追踪泛型 T 指针字段，新登记） | 边界 ✅ **修复后 x=3**（bug-54 同步修复后） |
| `control30_record_int_field.aura` | 非泛型 int 字段 | 编译运行 r=7 | ✅ 实测编译运行 r=7 | 对照组 ✅ r=7（行为不变） |
| `control30_record_string_field.aura` | T=string（预期正确包装不崩） | 编译运行 r=hello | ❌ **实测崩溃 0xC0000005**——**超预期，根因是 bug-54**（desc 层，新登记） | 对照组 ✅ **修复后 r=hello**（bug-54 同步修复后） |

> **实测（2026-09-01，`batch8_gc_root_family\` 批次 8 验证）**：三处生成路径（let/表达式/return）+ 多字段边界全部修复后编译运行通过；T=record 与 T=string 两用例随 **bug-54**（desc 层 per-instantiation 追踪）同步修复后从崩溃转通过；对照组行为不变。

## 5. 修复方案（Fix Plan）
> 详细方案（2026-09-01 调研更新）：与 bug-29 同批修复（批次 8），依赖 bug-29 的公共辅助提取；复现已建（batch8_gc_root_family，实测见 §4）。
> **分层警示（2026-09-01 实测新增）**：本条修复仅覆盖**包装层**假根；T=string/T=record 实例化形态的崩溃根因在 **bug-54**（desc 层不追踪泛型 T 指针字段）——本条修复后须与 bug-54 联动回归，`control30_record_string_field` / `repro30_record_T_record` 两用例以 bug-54 修复为最终通过条件。

- **修复位置**（同族三处，行号已按现状核对）：
  - `src\CodeGen\StmtLet.cpp:182-192`（let 声明；笔记前引 182-189 一致）
  - `src\CodeGen\ExprGen.cpp:434-443`（genRecordExpr 表达式；笔记前引 408-413 已偏移）
  - `src\CodeGen\StmtControl.cpp:137-147`（return；笔记前引 119-126 已偏移）
- **前置（依赖 bug-29）**：`isUnboundGenericSemType` 公共化（CodeGen.h + ExprGen.cpp）+ 组合判定 `isDeferredGcRoot(t) = isHeapSemType(t) && !isIfaceView(t) && isUnboundGenericSemType(t)`（见 bug-29 笔记）。
- **修复逻辑**（三分支，每字段独立 if constexpr，非 deferred 路径生成代码逐字符不变）：
  ```
  判定：isHeap = (f.value && isHeapSemType(f.value->inferredType) && !isViewField)（原判定不变）
        deferred = isHeap && isDeferredGcRoot(f.value->inferredType)
  生成：
    isHeap && !deferred → 原 GcRootHandle 路径（不变）
    deferred →
      auto {fv} = (字段值表达式);
      if constexpr (std::is_convertible_v<decltype({fv}), aura_rt::GcObject*>) {
          aura_rt::GcRootHandle<decltype({fv})> {fh}({fv});
          {var}.get()->{field} = {fh}.get();
      } else { {var}.get()->{field} = {fv}; }
    其余 → 原裸值路径（不变）
  ```
  其中 `{fv}/{fh}` 保持各文件现状命名（StmtLet/StmtControl 为 `_fv_/{recIdx}_{name}`；genRecordExpr 为 `_fv_{name}`，**其缺 recIdx 后缀为预存在问题，本次不改**）。
- **复现设计**（待建，目录 `example\used\leakcheck\_repro\closure_self_value_field\`）：
  - 主形态 `repro_generic_record_T_field.aura`：覆盖 let 声明（`let b = Box<T> { value = t }` + gc_force）与 return（`return Box<T> { value = t }`）两路径，T=int → 修复前崩溃，修复后 a=42 / c=43。
  - 表达式路径：record 字面量置于表达式上下文（如作调用实参），T=int → 修复后正常。
  - T=record 形态 `repro_generic_record_T_record_field.aura`：T=Point → 修复前碰巧正确，修复后 r=3,4（仍保护）。
  - 对照组：`control_record_nongeneric_field.aura`（`Box<int>{value=7}` 不包装）、`control_record_string_field.aura`（`Box<string>{value="hi"}` 正确包装）。
- **边界**：isViewField=true 时字段值必为视图（isUnboundGenericSemType 对其返回 false，双保险）；三处均为同步语句，无 genGcRootedArgs 的 outer/IIFE 内外差异；多字段（`Box<T>{a: t1, b: t2}`）各自独立 if constexpr，变量名天然唯一。
- **配套修复**：与 bug-29 同步落地。

## 6. 回归验证清单（Regression Checklist）
- [ ] 非泛型 record 字段保持不包装（对照组行为不变）
- [ ] string 字段 record 字面量保持正确包装
- [ ] 泛型 T 值字段 record 字面量（三处生成路径）修复后 gc_force 运行不崩溃
- [ ] 泛型 T=record 字段仍被保护（不漏保护，GC 安全）
- [ ] 全量回归保持通过

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\batch8_gc_root_family\`（2026-09-01 建立：7 个 .aura——let/return/expr 三路径 + 多字段/T=record 边界 + int/string 对照）
- **留存产物**：`.aura` / `.gen.cpp` / `.gen.exe` / `.compile.log`（崩溃 exit=-1073741819 实测留存）

---

## 8. 修复记录（2026-09-01，批次 8 实施 + 验证）

### 修复要点
- 同族三处（`StmtLet.cpp` let 声明 / `ExprGen.cpp` genRecordExpr 表达式 / `StmtControl.cpp` return）：未绑定泛型字段值（`isDeferredGcRoot` 判定）→ `if constexpr (std::is_convertible_v<decltype(_fv_{recIdx}_{name}), aura_rt::GcObject*>)` 延迟包装（T=值类型走 else 裸赋值消除 `GcRootHandle<int>` 假根；T=堆仍保护）；非泛型路径生成代码逐字符不变。
- 依赖 bug-54（desc 层 per-instantiation 追踪）同步落地：T=GcString*/T=Point 实例化形态修复后不悬垂。

### 验证统计
- 全量单测：**1194/1194 passed, 0 failed**（新增 `GenericRecordFieldDeferredIfConstexpr` 断言字段 if constexpr 生成形态）。
- 复现矩阵实测：let ✅ a=42 / expr ✅ v=42 / return ✅ v=42 / multifield ✅ a+s+b=3 / T=record ✅ x=3（仍保护）/ 对照组 int r=7、string r=hello 行为不变（详见 §4 回填）。
- 红线：`example/test.aura` ALL TESTS PASSED；used/1-6.aura 见 bug-55 §8。

### 说明
- 本条目（包装层假根）三处生成路径全部验证通过；T=record/T=string 两用例的最终通过依赖 bug-54（已同步修复，见 bug-54 笔记 §8）。

---
**当前状态**：`2026-09-01` 修复完成并验证通过（[x]）。
