---
type: bug_report
module: src/CodeGen（接口默认实现的 vtable/view 生成）
sub_module: `interface` 默认方法体 → `IFunc::view` 初始化
status:
  - researching
severity:
  - high
discover_date: 2026-11-01
discovered_by: feature-21 Phase 2 S7 独立验证（无 `#const` 对照件复现）
related_issues:
  - feature-21（Phase 2 验证时发现，与本特性无关）
tags:
  - codegen
  - interface
  - default-method
  - invalid-cpp
---

# [ ] bug-117 —— 接口默认实现体生成的 C++ 无法编译（`IFunc::view` 返回花括号初始化无法转 `I`）

> **一句话摘要**：`interface I { f() -> int { return 7 } }` 的产物过不了 `g++`：
> `IFunc::view` 里 `return { &IFunc::fFn, o };` 无法转换成接口类型 `I`。

## 1. 调研背景与发现

- **发现时间**：2026-11-01，feature-21 Phase 2 S7（`#N` 已正确展开为 `return 7;`，失败点在接口机制本身）。
- **对照实验**：`interface J { f() -> int { return 7 } }`（**无任何 `#const`**）同样失败 ⇒ 与 Phase 2 无关。
- **影响**：含接口默认方法体的正例不能通过 `-fsyntax-only` 两步铁律；会影响后续所有接口默认体特性的验证。

## 2. 复现

```bash
printf 'interface I { f() -> int { return 7 } }\nfun main(io: Io) { }\n' > /tmp/i1.aura
cd /d/you/Aura && ./build/aurac.exe /tmp/i1.aura -S --cpp /tmp/i1.cpp
g++ -std=gnu++20 -fsyntax-only -w -I D:/you/Aura -I D:/you/Aura/runtime /tmp/i1.cpp
# error: could not convert '{&IFunc::fFn, o}' from '<brace-enclosed initializer list>' to 'I'
```

## 3. 待定位

- `IFunc::view` 的生成点：默认实现是否应走不同的 vtable/包装类型。
- 与 `I` 的值语义（`I` 是否为可持有 view 的抽象类型）相关，需读 CodeGen 接口生成路径。

> 状态 `researching`：只记录可复现证据与影响面，根因待定位。
