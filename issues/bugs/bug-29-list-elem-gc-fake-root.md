---
type: bug_report
module: CodeGen / Runtime
sub_module: genListLiteral（ExprGen.cpp:342-360）/ isHeapSemType（ExprGen.cpp:55）
status:
  - fixed
severity:
  - critical
discover_date: 2026-08-30
related_issues:
  - "[[bug-14-gc-root-self-value-field]]"
tags:
  - gc
  - list
  - generic
  - crash
---

# 【列表字面量 GC 假根】列表字面量元素未绑定泛型 T 值被 GcRootHandle 误包装（bug-14 同源残留）
[x] **主标题：列表字面量元素保护路径对未绑定 GenericSemType("T") 误判堆 → GcRootHandle\<int\> 假根 → GC 崩溃 0xC0000005**

> **一句话摘要**：泛型方法/函数体内 `let arr = [self.value]`（value: T）或 `[v]`（v: T 局部变量）时，列表字面量元素保护路径（ExprGen.cpp:342-360）对未绑定泛型 T 值仍直接生成 `GcRootHandle<int>` 假根 → GC mark 扫描读 int 当根指针 → 0xC0000005。

## 1. 调研背景与发现
- **发现时间**：2026-08-30（修复 bug-14 genGcRootedArgs if constexpr 延迟判定时确认，同源残留）。
- **触发场景**：泛型方法/函数体内列表字面量含未绑定泛型 T 值元素（`let arr = [self.value]` value: T / `[v]` v: T 局部变量）。
- **影响范围**：凡列表字面量元素 Sema 类型为未绑定 GenericSemType("T") 的形态。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：genListLiteral（ExprGen.cpp:342-360）L349-350 `isHeap = ... isHeapSemType(e.elements[i]->inferredType)` → 未绑定 GenericSemType("T") 被 isHeapSemType（ExprGen.cpp:55 return true）误判堆 → L353-356 生成 `aura_rt::GcRootHandle<decltype(_eX_Y)> _ehX_Y(_eX_Y)`（T=int → GcRootHandle<int> 假根）。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及（inferredType 正常填充为未绑定 GenericSemType("T")）。
- **CodeGen 相关路径**：
  - `src\CodeGen\ExprGen.cpp:342-360` - 列表字面量元素保护路径（**核心漏洞点**）：L349-350 `isHeapSemType` 误判堆，L353-356 直接生成 `GcRootHandle<decltype(_eX_Y)>`，无未绑定泛型分支。
  - `src\CodeGen\ExprGen.cpp:55` - isHeapSemType 对未绑定 GenericSemType 默认 return true（误判点，与 bug-14 同源）。
- **Runtime 崩溃点**：`runtime\gc\handles.h:27`（GcRootHandle<int> 构造 reinterpret_cast<GcObject**>(&int 变量)）→ `runtime\gc\mark_sweep.cpp:89-90`（memcpy 读 int 值）→ `parallel_mark.cpp:62`（obj->forwarded() 访问非法地址）→ 0xC0000005。

### 2.2 关键逻辑细节
- **同源关系**：与 bug-14 同一 isHeapSemType 误判堆根因，仅消费点不同（列表字面量元素保护 vs genGcRootedArgs 实参保护）；bug-14 修复（genGcRootedArgs if constexpr 延迟判定）**不覆盖**本条路径。
- **T=record 形态**：decltype=Point* → GcRootHandle<Point*>「碰巧正确」（机制仍错）。

## 3. 影响范围（Scope）
- **结论**：泛型方法/函数体内列表字面量含未绑定泛型 T 值元素（`[self.value]` value: T / `[v]` v: T 局部变量）→ GC 触发即崩溃。
- **不受影响路径**：非泛型元素（PrimSemType 正确不包装）、string 元素（正确包装）、已实例化绑定后的具体类型元素。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro29_list_T_elem_self.aura` | 泛型方法内 `let arr = [self.val]`（T=int）+ gc_force | 编译运行 r=7 | ⛔ **被 bug-55 阻塞**：g++ `Array<auto>` 坏 C++（假根机制未达运行时） | 本条目主线 ✅ **修复后 r=7** |
| `repro29_list_T_local_var.aura` | 泛型函数 `let arr = [v]`（v: T）+ gc_force | 编译运行 r=42 | ⛔ 同上（bug-55 阻塞） | 同源 ✅ **修复后 r=42** |
| `repro29_list_T_mixed_elem.aura` | 混合元素 `[self.val, "str"]` + gc_force | 编译运行 len=2 | ⛔ 同上 | 同源（边界）⚠️ **修复后仍编译失败**（混合列表元素类型，独立缺陷，见 bug-55 §8） |
| `repro29_list_nested_T.aura` | 嵌套 `[[self.val]]` + gc_force | 编译运行 r=7 | ⛔ 同上 | 同源（边界）⚠️ **修复后仍编译失败**（嵌套 elemType 回退 dead code，独立缺陷，见 bug-55 §8） |
| `repro29_list_T_record.aura` | T=Point 实例化（碰巧正确→修复后仍保护） | 编译运行 r=3 4 | ⛔ 同上（bug-55 阻塞 + bug-54 desc 层悬垂叠加） | 边界 ✅ **修复后 r=3 4（T=record 仍保护，不漏保护）** |
| `control29_list_int.aura` | 非泛型 `[7]` + gc_force | 编译运行 r=7 | ✅ 编译运行 r=7（实测） | 对照组 ✅ r=7（行为不变） |
| `control29_list_string.aura` | T=string 实例化（正确包装） | 编译运行 r=hello | ⛔ 同被 bug-55 阻塞 | 对照组 ✅ **修复后 r=hello** |

> **实测（2026-09-01，`batch8_gc_root_family\` 批次 8 验证）**：主形态（elem_self/local_var/T_record）修复后全部编译运行通过（T=record 仍保护）；混合/嵌套 2 个边界形态仍编译失败（根因独立，见 bug-55 §8 未覆盖形态）；对照组行为不变。

## 5. 修复方案（Fix Plan）
> 详细方案（2026-09-01 调研更新）：与 bug-30 同批修复（批次 8），共用公共辅助函数。
> **前置依赖（2026-09-01 实测新增）**：bug-55（泛型列表声明 `Array<auto>` 坏 C++）必须先行修复，否则本条全部复现在 g++ 阶段失败、假根机制无法实测。

- **修复位置**：`src\CodeGen\ExprGen.cpp` **L375-385**（genListExpr 元素保护路径；笔记前引 L342-360 已偏移，实际函数名为 `genListExpr`，IIFE 位于 L362-388）。
- **elemType 联动（硬性，review-change-batch8 修正，与 bug-55 同步）**：genListExpr L294-308 元素为未绑定泛型（T）时 elemType 默认 int32_t 兜底 → T=string 实例化 append(GcString*) 坏 C++。补「semElemType=="auto" 且元素为未绑定泛型 → elemType = 泛型名」；嵌套 `[[T]]` 用递归（listContainsUnboundGeneric / listElemCppOf）。与 bug-55 声明侧 auto 推导全链一致。
- **前置（公共辅助提取）**：bug-14 修复的 `isUnboundGenericSemType`（`ExprClosure.cpp:35-46`）为 file-static，需提取为 `CodeGen.h` 公共成员（定义放 `ExprGen.cpp` isHeapSemType 之后），并新增组合判定 `isDeferredGcRoot(t) = isHeapSemType(t) && !isIfaceView(t) && isUnboundGenericSemType(t)` 与递归判定 `listContainsUnboundGeneric(ls)`；`ExprClosure.cpp` 删除 file-static 改调用成员。与 bug-30 同步落地。
- **修复逻辑**（三分支，同 bug-14 方案 A；非 deferred 路径生成代码与现状逐字符一致）：
  ```
  判定：isHeap = 原判定不变（L375-376）
        deferred = isHeap && isDeferredGcRoot(元素.inferredType)
  生成：
    isHeap && !deferred → 原 GcRootHandle 路径（不变）
    deferred → if constexpr 延迟判定（每元素独立，无嵌套）：
      auto _e{idx}_{i} = (元素表达式);
      if constexpr (std::is_convertible_v<decltype(_e{idx}_{i}), aura_rt::GcObject*>) {
          aura_rt::GcRootHandle<decltype(_e{idx}_{i})> _eh{idx}_{i}(_e{idx}_{i});
          {var}.get()->append(_eh{idx}_{i}.get());
      } else { {var}.get()->append(_e{idx}_{i}); }
    其余 → 原裸值路径（不变）
  ```
- **复现设计**（待建，仿 bug-14 复现形态，目录 `example\used\leakcheck\_repro\closure_self_value_field\`）：
  - 主形态 `repro_generic_list_T_elem.aura`：泛型方法内 `let arr = [self.value]`（T=int）+ `gc_force()` → 修复前崩溃，修复后 r=7。
  - T=record 形态 `repro_generic_list_T_record.aura`：T=Point → 修复前碰巧正确，修复后 r=1,2（仍保护）。
  - 局部变量形态（补充）：泛型函数 `fun makeArr[T](v: T) -> [T] { let arr = [v]; gc_force(); return arr }`。
  - 对照组：`control_list_nongeneric_elem.aura`（`[7]` 不包装）、`control_list_string_elem.aura`（`["hello"]` 正确包装）。
- **边界**：IIFE 为同步 lambda（L362），无协程 outer 分支差异；元素表达式含 co_await 本就非法（预存在限制）。
- **配套修复**：与 bug-30（同族三处）同步落地，统一提取公共辅助函数；互不阻塞。

## 6. 回归验证清单（Regression Checklist）
- [ ] 非泛型列表元素保持不包装（对照组行为不变）
- [ ] string 元素列表保持正确包装
- [ ] 泛型 T 值元素列表修复后 gc_force 运行不崩溃
- [ ] 泛型 T=record 元素列表仍被保护（不漏保护，GC 安全）
- [ ] 全量回归保持通过

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\batch8_gc_root_family\`（2026-09-01 建立：7 个 .aura——主线/局部/混合/嵌套/T=record 边界 + int/string 对照）
- **留存产物**：`.gen.cpp` / `.compile.log`（g++ 阶段失败实证，bug-55 阻塞）+ `control29_list_int.gen.exe`（对照正常）

---

## 8. 修复记录（2026-09-01，批次 8 实施 + 验证）

### 修复要点
- `src/CodeGen/ExprGen.cpp` genListExpr 元素保护路径：未绑定泛型元素（`isDeferredGcRoot` 判定）→ `if constexpr (std::is_convertible_v<decltype(_e{idx}_{i}), aura_rt::GcObject*>)` 延迟包装（T=值类型走 else 裸 append 消除 `GcRootHandle<int>` 假根；T=堆仍保护）。
- 联动 \#55：elemType 未绑定泛型回退（泛型名 / 嵌套 `[[T]]` 递归 `listElemCppOf`）——`repro29_list_T_mixed_elem`/`nested_T` 的编译期问题见 bug-55 §8。
- 公共辅助 `isDeferredGcRoot`/`listContainsUnboundGeneric` 与 #55/#30 共用。

### 验证统计
- 全量单测：**1194/1194 passed, 0 failed**（含新增 4 个批次 8 单测，其中 `GenericListUnboundElemDeclAutoDecltypeRoot` 断言元素 if constexpr 生成形态）。
- 复现矩阵实测：elem_self ✅ r=7 / local_var ✅ r=42 / T_record ✅ r=3 4（**T=record 仍保护，不漏保护**）/ 对照组 int r=7、string r=hello 行为不变。
- 红线：`example/test.aura` ALL TESTS PASSED；used/1-6.aura 见 bug-55 §8（3.aura 归 bug-54 引入）。

### 未覆盖形态
- 混合列表（`[T, string]`）与嵌套 `[[T]]` 仍编译失败——根因与修复方向见 bug-55 §8「未覆盖形态」，建议登记独立缺陷。

---
**当前状态**：`2026-09-01` 主形态修复完成并验证通过（[x]）；混合/嵌套边界形态未覆盖（独立缺陷待登记）。
