# 详细实施方案：P1 math 内置模块

> 工作流：3（详细实施方案；替换原草案内容）
> 提出时间：2026-08-10
> 状态：**待审查**
> 来源：[math内置模块issue.md](file:///d:/you/Aura/plan/math内置模块issue.md)（工作流 2 产物，已作废）；issue 见 TODO.txt §一 L29-32
> 源码核对：3 个并行 Search Agent（2026-08-10）+ 直接 Read 复核；全部行号为当前磁盘实际行号

---

## 1. 目标

新增 `math` 内置模块：`import math` 后可通过 `math.abs(x)` / `math.sqrt(x)` 等 8 个纯函数进行数学运算。完全照搬 `path` 模块的「模块内置」模式（plan11.md:54 规划），C++ 侧 `<cmath>` 一行映射，无副作用、无 throws、无 GC 分配。

**设计决策**：
- 8 个函数全部 **float 签名**（`math.abs(x: float) -> float`），int 实参经 C++ 隐式转换（`math.abs(-7)` → `math::abs(-7)`，int→double 提升）——因为 `findFunction` 仅按参数数量区分重载（BuiltinRegistry.h L163-171），同参数量不同类型重载不可共存；plan11 P3 表中 `math.abs(int|float)` 联合签名方案推迟（需 std::variant，代价大于收益）
- 返回 float：`let x: float = math.abs(-3)` 成立；`let x: int = math.abs(-3.0)` 被 Sema isAssignable 拒绝（数值提升单向 int→float，合理）
- Sema/CodeGen 核心逻辑**零改动**（inferMethodCall Phase B + importNsNames_ isNs 路径已通用支持）

## 2. 现状核对（plan 断言 → 源码事实）

| # | 断言 | 核实结果 |
| - | ---- | -------- |
| 1 | `fun a.b()` 点号函数名解析（DeclParser.cpp:43-46） | ✅ 一致：`match(Dot)` → name = `"a.b"` |
| 2 | doLoadAurai FunDecl 注册 `BuiltinGlobalFn{name="math.abs"}`（BuiltinRegistry.h:222-233） | ✅ 一致（含 throws、defaultCount 尾部默认参数统计） |
| 3 | inferMethodCall Phase B：`fqName = id->name + "." + e.method` → findFunction（ExprInfer.cpp:297-310） | ✅ 一致；返回 `semTypeFromBuiltinReturn(fn->returns)`（Named "float" → floatType） |
| 4 | isKnownBuiltin 白名单仅 `{"path"}`（ModuleManager.cpp:97-104） | ✅ 一致；`import math` 若不命中白名单会被当用户模块走 resolveImportPath 找 `math.aura` 而失败 |
| 5 | import 内置模块 → `loadAuraiFile("path.aurai")`（ModuleManager.cpp:150-153） | ✅ 一致；loadAuraiFile L207-224：current_path()/builtins/ 下读取 + tryLoadAurai 防重复 |
| 6 | CodeGen import 生成 `namespace path = aura_rt::path;`（CodeGen.cpp:58-76） | ✅ 一致；`importNsNames_.insert(imp.modName)`（L68） |
| 7 | genMethodCall isNs：`importNsNames_` 命中 → `obj::method(...)`（ExprGen.cpp:1010-1022、L1126-1134） | ✅ 一致；参数走 genGcRootedArgs（float 非堆 → 无包装，直接替换占位符） |
| 8 | path 模块 C++ 实现 `aura_rt::path` namespace（path.h:86-129），含 GcString* 适配 | ✅ 一致；math 无需字符串适配 |
| 9 | aura_rt.h 总头文件聚合 builtin/*.h（aura_rt.h:6-21） | ✅ 一致；`#include "builtin/path.h"` 在 L11 |
| 10 | 测试流程：compile.cmd 从项目根运行（`cd D:\you\Aura`）→ current_path() 能找到 builtins/ | ✅ 一致（example/compile.cmd L1） |
| 11 | plan11.md 8 个函数规划（commit a715f1a 找回）：`math.abs / math.sqrt / math.floor 等 8 个，<cmath> 一行映射` | ✅ 一致；plan11 亦确认 Parser/CodeGen/Sema 无需改动（当时 Sema 宽松通过，现 Phase B 精确查表 → 必须有 math.aurai） |

**关键事实（决定实现路径）**：
1. Sema 查表需要 `builtins/math.aurai` 提供注册条目——**必须新建**（plan11 当时无 .aurai 体系；path 走 .aurai 后 BuiltinRegistry.h:338 注释"path.new / path.join 不再硬编码"）
2. `str(x: float)` 已存在（builtin.aurai L19）→ 测试断言 `str(math.sqrt(2.0))` 可用
3. `math` 白名单加入后，`import math` 自动按需加载 math.aurai（ModuleManager.cpp:150-153 复用）

## 3. 逐层修改

### 3.1 Runtime：新增 `runtime/builtin/math.h`

**① 头文件**（仿 path.h 结构，无 GcString 适配）：

```cpp
#pragma once
// ============================================================
// aura_rt/builtin/math.h — `math` 内置模块
//
// README §15: math 提供纯函数数学运算，无副作用、无 throws、无 GC 分配
//
// 使用方式：
//   import math
//   let x = math.abs(-3.5)
//
// C++ 映射：math.abs(x) → math::abs(x)（import 生成 namespace math = aura_rt::math）
// int 实参经 C++ 隐式转换（math.abs(-7) → math::abs(int) → int→double 提升）
// ============================================================

#include <cmath>

namespace aura_rt {
namespace math {

// abs(x) → |x|（float 签名；int 实参隐式提升）
inline double abs(double x)          { return std::abs(x); }
// sqrt(x) → √x（x<0 → NaN）
inline double sqrt(double x)         { return std::sqrt(x); }
// floor(x) → 向下取整
inline double floor(double x)        { return std::floor(x); }
// ceil(x) → 向上取整
inline double ceil(double x)         { return std::ceil(x); }
// round(x) → 四舍五入（.5 远离零）
inline double round(double x)        { return std::round(x); }
// pow(x, y) → x^y
inline double pow(double x, double y) { return std::pow(x, y); }
// exp(x) → e^x
inline double exp(double x)          { return std::exp(x); }
// log(x) → ln(x)（x<=0 → -inf/NaN）
inline double log(double x)          { return std::log(x); }

} // namespace math
} // namespace aura_rt
```

**② aura_rt.h 聚合**（L11 `#include "builtin/path.h"` 之后追加）：

```cpp
#include "builtin/path.h"
#include "builtin/math.h"
```

### 3.2 声明：新增 `builtins/math.aurai`

```aura
// ============================================================
// builtins/math.aurai — math 内置模块
//
// 纯函数数学运算（C++ <cmath> 一行映射），无副作用、无 throws
// 所有函数接受 float（int 实参经 C++ 隐式提升），返回 float
// 经 import math 按需加载（与 path.aurai 同机制）
//
// 声明中的 '...' = C++ 桥接标记：aura 层无实现，c++ 层有实现
// ============================================================

fun math.abs(x: float) -> float ...
fun math.sqrt(x: float) -> float ...
fun math.floor(x: float) -> float ...
fun math.ceil(x: float) -> float ...
fun math.round(x: float) -> float ...
fun math.pow(x: float, y: float) -> float ...
fun math.exp(x: float) -> float ...
fun math.log(x: float) -> float ...
```

### 3.3 加载：`src/Module/ModuleManager.cpp` L100-102

```cpp
// 修改前：
    static const std::unordered_set<std::string> builtins = {
        "path"
    };

// 修改后：
    static const std::unordered_set<std::string> builtins = {
        "path",
        "math"
    };
```

### 3.4 文档：新增 `READMEs/16-math-module.md`（仿 14-path-module.md 格式；编号 16 因 `15-example.md` 已存在）

```markdown
# 16. `math` 内置模块

`math` 是内置模块，提供纯函数数学运算，无副作用、无 throws。导入方式为 `import math`（无引号）。

```aura
import math
```

## 15.1 绝对值

```aura
// --- math.abs(x: float) -> float ---
// 返回 x 的绝对值；int 实参自动提升为 float
let a = math.abs(-3.5)    // 3.5
let b = math.abs(-7)      // 7.0
```

## 15.2 平方根

```aura
// --- math.sqrt(x: float) -> float ---
// 返回 x 的平方根；x < 0 时返回 NaN
let s = math.sqrt(16.0)   // 4.0
```

## 15.3 取整

```aura
// --- math.floor(x: float) -> float ---
// 向下取整（向 -∞）
let f = math.floor(2.7)   // 2.0

// --- math.ceil(x: float) -> float ---
// 向上取整（向 +∞）
let c = math.ceil(2.1)    // 3.0

// --- math.round(x: float) -> float ---
// 四舍五入（.5 远离零）
let r = math.round(2.5)   // 3.0
```

## 15.4 幂与指数

```aura
// --- math.pow(x: float, y: float) -> float ---
// 返回 x 的 y 次幂
let p = math.pow(2.0, 10.0)   // 1024.0

// --- math.exp(x: float) -> float ---
// 返回 e 的 x 次幂
let e = math.exp(0.0)     // 1.0
```

## 15.5 对数

```aura
// --- math.log(x: float) -> float ---
// 返回 x 的自然对数；x <= 0 时返回 -inf / NaN
let l = math.log(1.0)     // 0.0
```

> 注：所有函数参数均为 `float`；传 `int` 实参时自动提升（与语言级数值提升规则一致）。
```

### 3.5 README.md 目录更新

- 目录（L10-27）`- [14. ...]` 后追加 `- [16. math 内置模块](READMEs/16-math-module.md)`（15 已被 `15-example.md` 占用，新章节顺延为 16）
- 附录 C（L31-33）无 math 条目（math 在 TODO 不在 README 附录），仅确认 README 标题行版本号无需变更

### 3.6 测试：`example/test.aura`

**① 顶部加 import**（L8 注释块之后）：

```aura
import math
```

**② main 内追加用例**（`ALL TESTS PASSED` 之前）：

```aura
    // ---- math 内置模块 ----
    let ma = math.abs(-3.5)                     // 3.5
    let mb = math.abs(-7)                       // 7.0（int 实参隐式提升）
    let ms = math.sqrt(16.0)                    // 4.0
    let mf = math.floor(2.7)                    // 2.0
    let mc = math.ceil(2.1)                     // 3.0
    let mr = math.round(2.5)                    // 3.0
    let mp = math.pow(2.0, 10.0)                // 1024.0
    let me = math.exp(0.0)                      // 1.0
    let ml = math.log(1.0)                      // 0.0
    io.println("ma=" + str(ma) + " mb=" + str(mb) + " ms=" + str(ms)
        + " mf=" + str(mf) + " mc=" + str(mc) + " mr=" + str(mr))
    io.println("mp=" + str(mp) + " me=" + str(me) + " ml=" + str(ml))
```

**③ 错误用例（编译期报错，不写入 test.aura，验证时单独编译确认）**：
- `let bad: int = math.abs(3.0)`（float→int，isAssignable 拒绝）

### 3.7 TODO.txt 更新

`[ ] P1  math 内置模块`（L29-32）→ 标记 `[x]` 并追加完成说明（日期 + 文件清单 + 验证结论）。

## 4. 测试方案

1. `cmake --build runtime/build`：math.h 编译通过（aura_rt.h 聚合）
2. `cmake --build build`：编译器本体编译通过（ModuleManager 改动）
3. `example/compile.cmd`（非 ASAN）→ `example/test.exe`：断言输出逐项核对（ma=3.5 mb=7.0 ms=4.0 mf=2.0 mc=3.0 mr=3.0 mp=1024.0 me=1.0 ml=0.0）+ `ALL TESTS PASSED`
4. 错误用例编译确认：`let bad: int = math.abs(3.0)` → Sema 报错
5. 全量回归：既有用例不回归（math 模块为纯新增，无既有路径改动）

## 5. 风险与应对

| 风险 | 应对 |
| ---- | ---- |
| `std::abs` int/double 重载歧义 | math.h 仅提供 double 签名，`math::abs(-7)` 唯一匹配 double 版本（隐式转换）；无歧义 |
| `math` 命名与用户模块冲突 | isKnownBuiltin 优先（与 path 同行为）；局部变量名 math 遮蔽不影响 import |
| 浮点断言精度 | 用例全部选精确可表示值（3.5/16.0/2.7→floor 2.0 等），无精度问题 |
| math.h 影响所有编译单元编译时间 | 纯 inline 8 函数，可忽略 |
| 未来同参数量重载需求（abs(int) 与 abs(float)） | findFunction 仅按参数数量匹配，无法共存；float 签名 + 隐式提升已覆盖；如需 int 精确语义（如 int 溢出）再议独立机制 |
