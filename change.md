# change.md — P2：Optional\<Iterator\<T\>\>（some(it)）GC 安全修复

> 依据：[plan/Optional迭代器GC安全修复.md](plan/Optional迭代器GC安全修复.md)（工作流 2 草案，已审查）
> 阶段：工作流 4 —— 详细实现代码，等待审查
> 原则：最小化改动——单文件 `runtime/builtin/optional.h`（+1 include +1 分支，约 12 行）

---

## 0. 实现决策

- **trait 引入方式选 A1**（plan §3.1 推荐项）：`optional.h` 顶部 `#include "variant.h"`，直接复用已有的 `is_iface_view_v<T>` trait。循环依赖检查：variant.h 的 include 列表（variant.h:17-23：types.h / gc.h / algorithm / cstring / tuple / type_traits / utility）不含 optional.h → 无环，安全。
- **否决 A2**（trait 迁移 types.h）：3 文件改动违反最小化原则，收益仅是依赖美观。
- **无需 dynamicDesc 钩子**：`Optional<T>` 是单值容器，`T` 编译期唯一确定（区别于 Variant 的运行时 index_ 切换），静态三分支即可。

## 1. 漏洞机制回顾（全部经源码验证）

```
some(it) ──ExprInfer.cpp:213-225（无 Iterator 拦截，clone 实参类型）──▶ Optional<Iterator<T>>
       ──ExprGen.cpp:650-652 ──▶ aura_rt::make_optional(it)   ← CTAD 推导 T=Iterator<T>
       ──optional.h:30-43 desc() ──▶ else 分支（is_pointer_v<Iterator<T>>=false）
       ──▶ ptrFieldCount=0 ──▶ value_.self（GcObject*）对 GC 不可见
       ──▶ mark 不追踪 / compact 不重写 ──▶ GC 后 self 悬垂 ──▶ use-after-free
```

修复后：`value_` 起始 + `offsetof(T, self)` 复合偏移进入 desc 指针字段表，与 `Variant::descForI` 的 `is_iface_view` 分支（variant.h:74-79，2026-08-07 生产验证）和 `MapIter::desc` 链式偏移（iterator.h:131-147）同型。

---

## 2. 修改：`runtime/builtin/optional.h`（唯一源码修改点）

### 2.1 include 区（L17-L20）

修改前：
```cpp
#include "../types.h"        // GcObject / TypeDescriptor / NoneType
#include "../gc/gc.h"
#include "error.h"
#include <type_traits>       // std::is_pointer_v
```

修改后：
```cpp
#include "../types.h"        // GcObject / TypeDescriptor / NoneType
#include "../gc/gc.h"
#include "variant.h"         // is_iface_view_v（P2：接口视图 T 的 self 子偏移注册）
#include "error.h"
#include <type_traits>       // std::is_pointer_v
```

### 2.2 `Optional<T>::desc()`（L29-L43）— 插入接口视图分支

修改前：
```cpp
    // GC 指针 T 注册 value_ offset；非指针 T 无指针字段
    static const TypeDescriptor& desc() {
        if constexpr (std::is_pointer_v<T>) {
            static const size_t offsets[] = { offsetof(Optional<T>, value_) };
            static const TypeDescriptor d = {
                sizeof(Optional<T>), 1, offsets, 0, nullptr, nullptr
            };
            return d;
        } else {
            static const TypeDescriptor d = {
                sizeof(Optional<T>), 0, nullptr, 0, nullptr, nullptr
            };
            return d;
        }
    }
```

修改后：
```cpp
    // GC 指针 T 注册 value_ offset；接口视图 T 注册 self 子偏移；其余无指针字段
    static const TypeDescriptor& desc() {
        if constexpr (std::is_pointer_v<T>) {
            static const size_t offsets[] = { offsetof(Optional<T>, value_) };
            static const TypeDescriptor d = {
                sizeof(Optional<T>), 1, offsets, 0, nullptr, nullptr
            };
            return d;
        } else if constexpr (is_iface_view_v<T>) {
            // P2：接口视图 T（Iterator<T>/Stringer 等值视图，含 GcObject* self）——
            // value_ 起始 + 视图内 self 子偏移 = 有效 GC 指针（仿 variant.h descForI
            // is_iface_view 分支的复合偏移模式），mark 追踪 + compact 重写，消除悬垂。
            // make_none 时 self=nullptr，markFields 的 if (child) 自然跳过，安全
            static const size_t offsets[] = {
                offsetof(Optional<T>, value_) + offsetof(T, self)
            };
            static const TypeDescriptor d = {
                sizeof(Optional<T>), 1, offsets, 0, nullptr, nullptr
            };
            return d;
        } else {
            static const TypeDescriptor d = {
                sizeof(Optional<T>), 0, nullptr, 0, nullptr, nullptr
            };
            return d;
        }
    }
```

### 2.3 文件头注释更新（L9-L10）

修改前：
```cpp
// API：is_none() / unwrap()（is_some 即 !is_none，无需冗余方法）
// has_value_=false 时 value_ 为 GC 零初始化 nullptr，扫描自动跳过 → 安全
```

修改后：
```cpp
// API：is_none() / unwrap()（is_some 即 !is_none，无需冗余方法）
// has_value_=false 时 value_ 为 GC 零初始化 nullptr，扫描自动跳过 → 安全
// GC 指针表：指针 T → value_ 偏移；接口视图 T（Iterator/Stringer 等含 self）→
//           value_+self 复合子偏移（2026-08-22 P2，修复 some(it) compact 悬垂）
```

---

## 3. 测试：`example/test.aura` — 新增段落（插入 L129 之后，Iterator 联合变体段末尾）

插入位置：L129 `}`（iv5 match 结束）与 L131 `// ===== 并发 GC P1：并行 STW 标记（2026-08-11）=====` 之间。

```aura
    // ---- Optional<Iterator<T>> GC 安全（2026-08-22 P2）----
    // U1 核心悬垂：some(iter) 构造 Optional（堆上 value_.self 原不可见）→
    //    多轮 GC + compact → unwrap 后完整迭代消费，验证输出正确
    let opt1 = some(range(0, 5))
    gc_force()                                      // Optional 装箱后 GC（mark 追踪 self）
    let stress1 = "s" + "t"                         // alloc（触发 compact 的分母）
    gc_force()                                      // compact 触发（self 重写验证）
    let it1 = opt1.unwrap()                         // unwrap：视图拷贝到栈
    gc_force()                                      // unwrap 后再 GC（栈上视图走保守扫描）
    io.println("U1 len=" + str(it1.collect().length))   // 5

    // U2 none 安全：has_value_=false，self=nullptr，扫描跳过
    let opt2 = none()
    gc_force()
    io.println("U2 is_none=" + str(opt2.is_none()))     // true

    // U3 指针回归：some("hello")（is_pointer_v 分支，行为不变）
    let opt3 = some("hello")
    gc_force()
    io.println("U3 unwrap=" + opt3.unwrap())            // hello

    // U4 值类型回归：some(42)（else 分支，行为不变）
    let opt4 = some(42)
    gc_force()
    io.println("U4 unwrap=" + str(opt4.unwrap()))       // 42

    // U5 多轮压力：100 轮 some(iter)+GC+unwrap+collect
    let total5 = 0
    for i5 in range(100) {
        let o5 = some(range(0, 3))
        let junk5 = "j" + str(i5)
        gc_force()
        total5 = total5 + o5.unwrap().collect().length
    }
    io.println("U5 total=" + str(total5))               // 300
```

**预期输出**：
```
U1 len=5
U2 is_none=true
U3 unwrap=hello
U4 unwrap=42
U5 total=300
```

**用例设计说明**：
- U1 覆盖三个窗口：装箱后 GC（mark 是否追踪堆上 self）、compact 后 unwrap（self 是否被重写）、unwrap 后 GC（既有栈扫描回归）
- U2 验证 [mark_sweep.cpp:226](runtime/gc/mark_sweep.cpp#L226) `if (child)` 对 null self 的跳过
- U3/U4 守卫既有两分支零行为变化（回归锚点）
- U5 100 轮压力覆盖多代 GC（young→old→compact 混合）

---

## 4. 变更汇总

| 文件 | 类型 | 变更点 |
|---|---|---|
| `runtime/builtin/optional.h` | 修改 | `#include "variant.h"` + desc() 插入 `is_iface_view_v<T>` 分支（复合偏移）+ 头注释 |
| `example/test.aura` | 修改 | L129 后插入 U1-U5 测试段落（plan §6 用例落地） |

**总计**：修改 2 文件，源码净增约 12 行 + 测试 32 行。无 Sema/CodeGen/GC 核心改动，无新文件，无 CMakeLists 变更。

---

## 5. 安全性要点（实现级复核）

1. **分支静态选择**：`if constexpr` 三分支互斥编译期选择——指针 T 无 `.self` 成员（trait 必 false，且分支顺序在前无交集）；iface view T 非指针（指针在首分支截获）；普通值类型两者皆 false → else。任何 `Optional<T>` 实例化只走一个分支。
2. **null 安全**：`make_none()` 的 `value_ = T{}` → `self = nullptr` → markFields L226 `if (child)` 跳过（源码验证）。
3. **compact 同构**：markFields 与 compact 侧 updateObjectFields 均先过 `dynamicDesc` 钩子再按同一 offsets 表遍历（mark_sweep.cpp:215-230 同构消费）——Variant 的 iface_view 复合偏移已在此路径生产验证。
4. **嵌套组合**：`Optional<Optional<X>>`/`Optional<A|B>` 的元素运行时是堆指针 → 首分支覆盖，天然安全。
5. **既有实例化零影响**：Optional<int>/Optional<GcString*>/Optional<record*> 等生成的静态 desc 逐字节不变（新分支仅对 iface view T 的实例化生效）。
6. **并发 GC**：`make_optional` 走 `alloc()` → markingInProgress_ 时 born-marked（gc.h:440-443），首次标记即用新 desc，无新增竞态。

---

## 6. 验证方案

```powershell
# 1. 编译 runtime（optional.h 为 header-only，被 builtin 使用者重编）
cmake --build runtime/build

# 2. 编译测试（compile.cmd 非编译器变更，仅重编 runtime 后链接）
#    按项目约定：example/test.aura → compile.cmd → test.exe
cd example; ..\compile.cmd; .\test.exe

# 3. 预期：新增 5 行输出（U1-U5）+ 既有输出不变 + ALL TESTS PASSED

# 4. ASAN 深度验证（可选，验证 UAF 修复）
#    .\ASAN_Test.ps1 example\test.aura
#    修复前 U1/U5 应报 heap-use-after-free；修复后干净
```

---

## 7. 实施后收尾

- 更新 TODO.txt：`[ ] P2 Optional<Iterator<T>>（some(it)）GC 安全` 条目标 `[x]` + 证据行号
- 同步删除 [二] 节 L163 的"下方独立 P2 条目"交叉引用中该条目（若整节收尾则一并清理）
