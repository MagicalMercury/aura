---
type: bug_report
module: runtime/builtin
sub_module: intern_string 的 L1 线程缓存把「调用方指针 s」当 key 存储（internCacheInsert 存 {s, len, val}）——调用方复用缓冲区时 memcmp 自己比自己，静默返回旧字符串
status:
  - fixed
severity:
  - high
discover_date: 2026-10-17
discovered_by: Hermes（bug-110/111 只读调研批顺带发现；主 Agent 自建探针实测确认）
related_issues:
  - bug-86（同文件 L1 intern 缓存：代次作废时机；本项是同一缓存的 key 语义缺陷）
  - "[[bug-86-sync-thread-gc-force-stw-deadlock]]"
tags:
  - runtime
  - string
  - intern
  - cache
  - correctness
  - silent-wrong-result
---

# [x] 【`intern_string` L1 缓存把调用方指针当 key ⇒ 静默返回旧字符串】

**状态**：`[x] 已修复`（2026-10-17，见 §8）
**严重度**：**high**（静默错值，无报错、无崩溃）
**发现场景**：bug-110/111 只读调研批的探针里发现「连续 `intern_string(buf)` 不同内容恒返回同一指针」，主 Agent 自建最小探针复现并定位到 `internCacheInsert`。

---

## 0. 一句话摘要

`intern_string(const char* s, size_t len)` 的 **L1 线程局部缓存**（`runtime/builtin/string.cpp`）用
**调用方传入的指针 `s`** 作为缓存条目 key：

```cpp
cache.entries[0] = {s, len, val};              // ← key 存的是调用方指针，不是字节副本
```

命中判定：

```cpp
if (e.keyLen == len && std::memcmp(e.key, s, len) == 0) return e.val;
```

⇒ 当**调用方复用同一缓冲区**（典型：`char buf[N]` + `snprintf`）装入**不同内容、相同长度**时，
`e.key` 与 `s` 是同一个地址 ⇒ `memcmp` **自己和自己比**，恒为 0 ⇒ **命中旧条目，返回旧字符串对象**。
即：`intern_string("BBB")` 可能返回一个内容为 `"AAA"` 的 `GcString*`，**静默错值**。

## 1. 实测（主 Agent 自建探针，`scripts/_probe_intern_alias.cpp`）

```cpp
char buf[16];
snprintf(buf, sizeof buf, "AAA");
GcString* a = intern_string(buf);          // 'AAA'
snprintf(buf, sizeof buf, "BBB");          // 同一地址，内容不同、等长
GcString* b = intern_string(buf);          // ← 期望 'BBB'
GcString* lit = intern_string("BBB");      // 字面量（不同地址）作对照
```

实测输出（rc=0，无任何报错）：

```
buf_addr=000000965bdff910
intern(buf='AAA') = 000001fa91980078 view='AAA'
intern(buf='BBB') = 000001fa91980078 view='AAA'   <-- 应为 'BBB'
intern("BBB")     = 000001fa91980078 view='AAA'
VERDICT=WRONG_INTERN_RESULT
```

注意第 3 行：**连字面量调用都被污染** —— 因为缓存条目 key 指向的缓冲区此刻内容已是 `"BBB"`，
`memcmp(e.key=buf, s="BBB", 3)` 相等 ⇒ 命中 ⇒ 返回 `'AAA'` 对象。污染范围是**整个线程的 L1 缓存**。

## 2. 影响与可达性

- **当前生产不可达（幸运）**：`runtime/` 内所有 `intern_string` 调用点实参都是**字面量**
  （`builtin/error.h` 全系列、`gc/alloc.cpp:505-506`、`task.h:143`、`thread_pool.cpp:185`），
  编译器生成代码也只用字面量（`.rodata` 地址恒定）⇒ 现状不触发。
- **但它是公开 API**：`intern_string(const char*)`（`string.cpp:789` 包装）任何未来调用方
  （工具、插件、runtime 新增代码、测试）只要传**可变缓冲区**就会踩，且**静默错值**（不是崩溃）。
- **严重度 high 的理由**：字符串内容错误会污染错误消息、键查找、序列化等一切下游；无任何可观测信号。

## 3. 修法建议（待裁定）

1. **key 存字节副本（首选）**：`InternCacheEntry` 改为内联定长数组（如 `char key[32]`），
   `len > 32` 的字符串**不进 L1**（直接走 L2 `g_internPool`）；零堆分配、热路径无额外开销。
2. 或把 key 改成 `std::string`（正确但每条缓存命中/插入都涉及分配，热路径代价大）。
3. 无论哪种，都要保留现有 **代次作废**（`cache.gen`，bug-86 修复）：**这是两个独立问题**
   （代次管"指针失效"，key 语义管"内容错配"），修一个不能替代另一个。

## 4. 验证建议

- 单测（承重）：上述探针形态（同缓冲区、等长、不同内容）⇒ 修前 `view()` 错、修后正确；
  再加「`len > 32` 走 L2」的边界用例 与「字面量命中」回归。
- 回归：全量 0 failed（基线 **1585/1585**）+ `--filter=Examples` 17/17 + `example/test.aura` md5。

## 5. 关联

- **bug-86**：同一 L1 缓存（`tl_internCache`）的**代次作废**修复（`compact` 后指针悬垂）；
  本项是同缓存的 **key 语义**缺陷，两者正交。
- **bug-110/111**：本项即在其只读调研批中被顺带发现（探针因此改用 `GcString::make`）。

---

## 8. 修复记录（2026-10-17）

### 8.1 修法（按 `scripts/bug112_fix_brief.md` §1 实施，未改设计）

L1 缓存条目改为**自持字节副本**，不再持有调用方指针：

```cpp
static constexpr size_t kInternL1MaxKey = 32;   // 超过则不缓存（走 L2 全局池）
struct InternCacheEntry {
    char      key[kInternL1MaxKey];   // 内容字节副本（零堆分配）
    uint32_t  keyLen;
    GcString* val;
};
```

- **插入**（`internCacheInsert`）：`len > 32` ⇒ 直接 `return`（不进 L1，L2 仍正确）；
  否则 `InternCacheEntry fresh{}` + `memcpy(fresh.key, s, len)` + `fresh.keyLen` + `fresh.val`。
  查重/提前到头部/64 槽 LRU 语义**逐行保持不变**（比较改比内联 key）。
- **命中**（`intern_string` 的 L1 循环）：`e.keyLen == len && memcmp(e.key, s, len)` —— 两侧现在都是稳定字节。
- **代次作废**（`cache.gen` vs `g_internCacheGen`，bug-86）：**原样保留，一行未动**。
- **不动**：L2 `g_internPool`、`clear_intern_cache`、`make_string` 与其它函数。
- **契约注释改正**：原 `:685`「key 指向 Aura 源码字符串字面量（编译期常量，永久存活），不会失效」
  是**错误前提**（API 是公开的，实参可以是任意缓冲区），已改写为「key 是本缓存自持的字节副本，
  不依赖调用方指针存活；长度 > kInternL1MaxKey 不进 L1」。

### 8.2 改动文件

| 文件 | 改动 |
|---|---|
| `runtime/builtin/string.cpp:681-746` | `InternCacheEntry` 内联 key + `kInternL1MaxKey` + 插入路径 memcpy/超长早退 + 契约注释改正（`intern_string` 命中循环仅加一行注释，逻辑未变） |
| `test/rt/test_intern_cache.cpp` | **新建**，7 个用例（套件 `InternCache`） |
| `test/CMakeLists.txt:137-139` | 登记 `rt/test_intern_cache.cpp` |
| `issues/bugs/bug-112-*.md` | 本文件（状态 + §8） |

`string.cpp` md5：修复前 `ca4ee78f4bef49c37101f147136a2957` ⇒ 修复后 `9ed0405496973ad181f3274b70c34713`。

### 8.3 三步对照（真跑）

**① 修复版**：`--filter=InternCache` ⇒ 7/7 全绿（`1598 tests, 7 passed, 0 failed`）。

**② 临时改回指针 key**（`InternCacheEntry{const char* key; size_t keyLen;}` + `entries[0] = {s, len, val}`，
仅改这两处，其它不变）⇒ 承重用例**转红**，原始输出：

```
[  FAILED  ] InternCache.BufferReuseEqualLengthYieldsFreshCorrectContent
    D:/you/Aura/test/rt/test_intern_cache.cpp:74: EXPECT_EQ failed: bText vs std::string("BBB") -> left=AAA right=BBB
    D:/you/Aura/test/rt/test_intern_cache.cpp:75: EXPECT_TRUE failed: *holdA != *holdB
[  FAILED  ] InternCache.LiteralLookupNotPollutedByReusedBuffer
    D:/you/Aura/test/rt/test_intern_cache.cpp:92: EXPECT_EQ failed: viewOf(viaBuf) vs std::string("BBB") -> left=AAA right=BBB
    D:/you/Aura/test/rt/test_intern_cache.cpp:97: EXPECT_EQ failed: viewOf(lit) vs std::string("BBB") -> left=AAA right=BBB
[  FAILED  ] InternCache.KeyLen32BufferReuseIsCorrect
    ...:134: EXPECT_EQ failed: viewOf(*holdQ) vs std::string(32, 'y') -> left=xxxx… right=yyyy…
[  FAILED  ] InternCache.KeyLen33BufferReuseIsCorrect
    ...:158: EXPECT_EQ failed: viewOf(*holdQ) vs std::string(33, 'q') -> left=pppp… right=qqqq…
[  FAILED  ] InternCache.LruOverflowKeepsEveryContentCorrect
    ...:179: EXPECT_EQ failed: viewOf(p) vs want -> left=LRU_00 right=LRU_01  （LRU_01…LRU_69 共 69 条）
[==========] 1598 tests, 2 passed, 5 failed
```

`left=AAA right=BBB` 正是缺陷本体：**返回的是旧对象**（`SameContentDedupesToSameObject`、
`ClearThenInternStillCorrect` 两个「行为不变」用例在修前也绿 —— 说明新增用例集里只有承重点能抓它，
不是全盘靠修后状态自证）。

**③ 恢复** ⇒ `string.cpp` md5 回到 `9ed0405496973ad181f3274b70c34713`（与 ① 一致），
`--filter=InternCache` 重新 7/7 绿。

### 8.4 回归（双目标重建后实测）

- `cmake --build runtime/build` ✅；`cmake --build test/build` ✅
- 全量：**开工基线 1591/1591**（本次实测）⇒ 修复后 **1598/1598 passed, 0 failed**（1591 + 新增 7）
- `--filter=Examples`：**17/17**
- `example/test.aura` md5：`5f1760a5a360f4139d176434775abddd`（与基线一致）

### 8.5 备选方案评价（**未实施**）

1. **命中时回读 `val->view()` 校验内容**：能挡住错配，但每次命中多一次比较/潜在宽字符处理；
   且它把「缓存 key 不可信」这一根因留在原地（等于用校验掩盖），未来任何新增读路径都可能漏。不取。
2. **`std::string` 作 key**：语义最直白，但每次插入都要堆分配（热点路径 1000 万次循环的
   目标就是消除分配/加锁），且 64 条条目会各自持有堆块。不取。

### 8.6 残余说明

- `tl_internCache` 体积从 64×16B≈1KB 增至 64×40B≈2.5KB（线程局部）。属可接受代价，
  换来零堆分配 + 命中路径不变。
- 长度 > 32 的 intern 现在**恒走 L2**（每次加一次 `g_internMutex`）。生产调用点全是短字面量，
  实际无影响；这是「定长内联 key」的明示代价。
- 代次作废（bug-86）与 key 语义（本项）互不替代，已在同一结构上共存并各自保留。
