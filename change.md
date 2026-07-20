# Change Plan v6.1 — 静态对象析构顺序崩溃修复（方案 C）

> **本 plan 完全覆盖之前的 v6 内容**（字面量 intern + Rope 表示），转为单一目标：
> 修复运行 `./example/test.exe` 时的 `0xC0000005 (Access Violation)` 崩溃。
>
> 原则：**暂时禁止改动 Rope/Intern 等新功能**，仅做最小修复让程序能正常退出。

---

## 一、崩溃现象

```
PS D:\you\Aura> ./example/test.exe 2>&1
GC: alloc=655.5KB young=183.9KB old=40.2KB gc=0 minor=2 live=4951 pages=165
Run exit: -1073741819   ← 0xC0000005 Access Violation
```

- GC 统计已正常输出（`alloc=655.5KB ...`），说明 `io.println(gc_stats())` 执行成功
- 崩溃发生在**程序退出阶段**（静态对象析构）
- `minor=2` 说明运行期间触发了 2 次 minor GC

---

## 二、根本原因分析

### 2.1 涉及的静态对象

| 静态对象 | 文件 | 类型 | 析构时调用 |
|:---|:---|:---|:---|
| `GcHeap::heap` | [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) `instance()` 内 | Meyers Singleton | vector/mutex 自动析构 |
| `g_internPool` | [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) 匿名命名空间 | `unordered_map<string, unique_ptr<GcGlobalRoot<GcString>>>` | 逐个 `~GcGlobalRoot()` |
| `g_internMutex` | 同上 | `std::shared_mutex` | 自动析构 |
| `GcString::from(int)::_cache[2048]` | string.cpp | 裸指针数组 | 不析构（new 对象泄漏，OS 回收） |
| `GcString::empty()::_e` | string.cpp | `GcGlobalRoot<GcString>` | `unregisterGlobalRoot()` |
| `GcString::from(bool)::_t/_f` | string.cpp | `GcGlobalRoot<GcString>` | `unregisterGlobalRoot()` |

### 2.2 C++ 静态对象析构顺序规则

- **同一翻译单元内**：按声明逆序析构（C++ 标准保证）
- **不同翻译单元之间**：**顺序未定义**（C++ 标准 §3.6.3）

`g_internPool`（string.cpp）和 `GcHeap::heap`（gc.cpp）位于不同翻译单元，析构顺序由编译器/链接器决定。

### 2.3 崩溃路径

当 `GcHeap::heap` 先于 `g_internPool` 析构时：

```
程序退出
├─ GcHeap::heap 析构
│  ├─ globalRoots_ vector 析构（内存释放）
│  └─ globalRoots_m_ mutex 析构（不可用）
└─ g_internPool 析构
   └─ unique_ptr<GcGlobalRoot<GcString>> 析构
      └─ GcGlobalRoot::~GcGlobalRoot()
         └─ GcHeap::instance().unregisterGlobalRoot(...)
            ├─ std::lock_guard lk(globalRoots_m_)  ← 💥 mutex 已析构
            └─ globalRoots_.push_back/erase(...)     ← 💥 vector 已析构
```

→ **Access Violation（0xC0000005）**

### 2.4 为什么之前没崩溃？

本次 v6 plan 新增了 `g_internPool` 静态变量。之前的静态 `GcGlobalRoot`（`_t`/`_f`/`_e`）都在 string.cpp 中，析构顺序由编译器在 string.cpp 内决定，可能恰好都在 `GcHeap::heap` 之前析构。

新增 `g_internPool` 后：
- `g_internPool` 是 `unordered_map`，析构时会逐个调用 value 的析构函数（`~GcGlobalRoot`）
- 链接器可能决定 `g_internPool` 在 `GcHeap::heap` 之后析构
- 导致 `~GcGlobalRoot` 访问已析构的 GcHeap 成员

### 2.5 同样有风险的静态对象

| 静态对象 | 析构时调用 GcHeap | 风险 |
|:---|:---|:---:|
| `g_internPool` | `unregisterGlobalRoot` × N | 🔴 高（新增触发崩溃） |
| `GcString::from(bool)::_t/_f` | `unregisterGlobalRoot` | 🟡 中 |
| `GcString::empty()::_e` | `unregisterGlobalRoot` | 🟡 中 |
| `GcString::from(int)::_cache[2048]` | 不析构 | 🟢 无 |

---

## 三、方案 C 设计：`[[gnu::init_priority(N)]]` 控制析构顺序

### 3.1 原理

GCC/Clang 扩展属性 `[[gnu::init_priority(N)]]`（N ∈ [1, 65535]）：
- 静态对象按 **priority 升序初始化**
- 静态对象按 **priority 降序析构**（与初始化相反）

通过为每个静态对象分配 priority，强制确定析构顺序，消除跨翻译单元的顺序未定义问题。

### 3.2 Priority 分配方案

| Priority | 对象 | 文件 | 作用 |
|:---:|:---|:---|:---|
| 101 | `GcHeap::heap` | gc.cpp | GC 单例（必须最先初始化，最后析构） |
| 102 | `GcString::from(int)::_cache[2048]` | string.cpp | 小整数缓存（裸指针数组） |
| 103 | `GcString::empty()::_e` | string.cpp | 空字符串单例 |
| 104 | `GcString::from(bool)::_t` / `_f` | string.cpp | 布尔字符串单例 |
| 105 | `g_internPool` + `g_internMutex` | string.cpp | Intern 池 |

### 3.3 初始化顺序（priority 升序）

```
101: GcHeap::heap             ← 最先初始化
102: from(int)::_cache
103: empty()::_e
104: from(bool)::_t/_f
105: g_internPool              ← 最后初始化
```

**保证**：所有 `GcGlobalRoot` 在 `GcHeap::heap` 之后初始化，因此 `GcGlobalRoot` 构造时调用 `registerGlobalRoot()` 时 GcHeap 已就绪。

### 3.4 析构顺序（priority 降序，与初始化相反）

```
105: g_internPool + g_internMutex  ← 最先析构
     └─ ~GcGlobalRoot() → unregisterGlobalRoot() → GcHeap 仍存活 ✅
104: from(bool)::_t/_f
     └─ ~GcGlobalRoot() → unregisterGlobalRoot() → GcHeap 仍存活 ✅
103: empty()::_e
     └─ ~GcGlobalRoot() → unregisterGlobalRoot() → GcHeap 仍存活 ✅
102: from(int)::_cache           ← 不析构（裸指针数组）
101: GcHeap::heap                ← 最后析构
     └─ vector/mutex 自动析构 ✅ 无外部依赖
```

**保证**：所有 `GcGlobalRoot` 在 `GcHeap::heap` 之前析构，因此 `~GcGlobalRoot` 调用 `unregisterGlobalRoot()` 时 GcHeap 仍可用。

---

## 四、改动详情

### 4.1 [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) — GcHeap 单例加 priority 101

```cpp
GcHeap& GcHeap::instance() {
    // 方案 C：用 gnu::init_priority 强制 GcHeap 最先初始化、最后析构
    [[gnu::init_priority(101)]] static GcHeap heap;
    return heap;
}
```

### 4.2 [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) — 静态对象加 priority 102-105

```cpp
// string.cpp 顶部匿名命名空间
namespace {
    [[gnu::init_priority(105)]] std::unordered_map<std::string,
        std::unique_ptr<GcGlobalRoot<GcString>>> g_internPool;
    [[gnu::init_priority(105)]] std::shared_mutex g_internMutex;
}

// GcString::from(int)
GcString* GcString::from(int32_t val) {
    [[gnu::init_priority(102)]] static GcGlobalRoot<GcString>* _cache[2048] = {};
    // ...
}

// GcString::empty()
GcString* GcString::empty() {
    [[gnu::init_priority(103)]] static GcGlobalRoot<GcString> _e{make("", 0)};
    return _e.get();
}

// GcString::from(bool)
GcString* GcString::from(bool val) {
    [[gnu::init_priority(104)]] static GcGlobalRoot<GcString> _t{make("true")};
    [[gnu::init_priority(104)]] static GcGlobalRoot<GcString> _f{make("false")};
    return val ? _t.get() : _f.get();
}
```

### 4.3 改动规模

| 文件 | 改动 | 净增行数 |
|:---|:---|:---:|
| [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) | `GcHeap::instance()` 内 `static GcHeap heap` 加 `[[gnu::init_priority(101)]]` | +1 |
| [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) | 5 个静态对象加 priority 102-105 | +5 |
| **合计** | | **+6** |

---

## 五、平台限制

### 5.1 GCC/Clang 支持

`[[gnu::init_priority(N)]]` 是 GCC/Clang 扩展属性：
- GCC：默认可用，无需额外编译选项
- Clang：默认可用
- 当前项目 CMakeLists.txt 已配置 `-std=c++20 -fno-rtti`，与 `[[gnu::init_priority]]` 兼容

### 5.2 MSVC 不支持

- MSVC 不识别 `[[gnu::init_priority]]`，会忽略或报错
- **用户已确认不迁移 MSVC**，本方案可接受

### 5.3 编译器警告处理

GCC 可能对 `[[gnu::init_priority]]` 发出 `-Wattributes` 警告（提示该属性不影响代码生成），可忽略或加 `-Wno-attributes` 抑制。

---

## 六、验证方案

### 6.1 编译验证

```bash
cmake --build build
# 期望：无编译错误，可能有 -Wattributes 警告（可忽略）
```

### 6.2 运行验证

```bash
./example/test.exe 2>&1; echo "Run exit: $LASTEXITCODE"
# 期望：
# GC: alloc=655.5KB young=183.9KB old=40.2KB gc=0 minor=2 live=4951 pages=165
# Run exit: 0    ← 不再 -1073741819
```

### 6.3 多次运行验证

连续运行 10 次，确认退出码稳定为 0（排除偶然性）。

---

## 七、实施顺序

| 步骤 | 改动 | 验证 |
|:---:|:---|:---|
| 1 | [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) `GcHeap::instance()` 加 `[[gnu::init_priority(101)]]` | 编译通过 |
| 2 | [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) 5 个静态对象加 priority 102-105 | 编译通过 |
| 3 | 运行 `./example/test.exe` | 退出码 0 |
| 4 | 连续运行 10 次 | 退出码稳定 0 |

---

## 八、风险评估

### 8.1 风险极低

- **不改动任何业务逻辑**：只加 `[[gnu::init_priority]]` 属性
- **不改动 GC 实现**：`unregisterGlobalRoot` 行为不变
- **不改动 GcGlobalRoot**：析构链不变
- **可立即回滚**：删除属性即恢复原状

### 8.2 不影响现有功能

- 所有现有测试（string 操作 / GC 扫描 / intern 池）行为不变
- 仅改变静态对象的析构顺序，运行时行为完全一致

---

## 九、与之前 v6 plan 的关系

### 9.1 v6 plan 内容推迟

原 v6 plan 包含的 Rope/Intern 功能**暂时不动**：

| 项 | 状态 | 说明 |
|:---|:---|:---|
| Part A: 字面量 intern | ✅ 已实施 | 引入 `g_internPool`，触发本崩溃 |
| Part B: 小整数缓存扩展 | ✅ 已实施 | `from(int)::_cache[2048]` |
| Part C: Rope 表示 | ❌ **推迟** | 待本崩溃修复后重新评估 |

### 9.2 修复后计划

1. 实施本 v6.1 plan（+6 行）
2. 运行 `./example/test.exe` 确认退出码 0
3. 若 Part C (Rope) 仍需要，再单独出 plan

---

## 十、关键变更说明

**v6.1 设计要点**：
- ✨ 采用方案 C：`[[gnu::init_priority(N)]]` 控制静态对象初始化/析构顺序
- ✨ 5 级 priority（101-105）覆盖所有静态 GcGlobalRoot 对象
- ✨ GcHeap 最先初始化（101），最后析构，保证所有 `~GcGlobalRoot` 时 GcHeap 可用
- ✨ 暂时禁止改动 Rope/Intern 等新功能，仅做最小修复
- ❌ 不采用方案 A（去掉 `unregisterGlobalRoot` 调用）：会导致 GcHeap 内部状态不一致
- ❌ 不采用方案 B（`std::quick_exit`）：需要修改 main 函数，改动范围大

**与 v6 的关系**：
- v6 引入了 `g_internPool`，触发了静态对象析构顺序问题
- v6.1 修复此问题，为后续 Rope 实施扫清障碍
