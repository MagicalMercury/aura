# 多线程 TLAB flush 补全 — 修复 plan（A+C 结合方案）

## 问题定位

### Bug 描述
`tryAlloc` 延迟 compact 路径（`runtime/gc/alloc.cpp:34-41`）在补执行被延迟的 compact 时，
**只 flush 当前线程的 TLAB**，然后**直接执行 compact，无 STW**。

多线程场景下，其他线程的 TLAB `curPage` 可能指向即将被 compact 释放的页 →
compact 释放页后其他线程 `curPage` 悬垂 → 下次 bump 分配写入已释放内存 → 崩溃。

### 触发路径
1. 某线程在 GC 期间持有 `GcCompactSuspendGuard`（`compactSuspendedCount_ > 0`）
2. `minorGc` 检测到需要 compact 但被延迟，设置 `compactPending_ = true`
   （`runtime/gc/mark_sweep.cpp:35`）
3. `GcCompactSuspendGuard` 析构，`compactSuspendedCount_` 归 0
4. 该线程下次 `tryAlloc` 进入延迟 compact 路径（`alloc.cpp:34`）
5. 只调用 `flushTlab()`（当前线程），直接 `compact()`
6. 其他线程的 TLAB `curPage` 悬垂 → 崩溃

### 对比 safepoint 多线程路径（安全）
`safepoint()` 多线程路径是安全的：
- 每个线程进入 safepoint 时自己 `flushTlab()`（清空 curPage，`safepoint.cpp:38`）
- initiator 等待所有线程 `stopped_threads_` 到位后才执行 GC（`safepoint.cpp:71`）
- STW 保证 compact 期间没有线程访问对象

### 额外问题
`compactPending_` 是普通 `bool`（`gc.h:406`），多线程读写有数据竞争。
`compactSuspendedCount_` 已是 `std::atomic<int>`，但 `compactPending_` 未同步。

## 方案对比

### 方案 A：tryAlloc 延迟 compact 走完整 safepoint STW
- 设置 `gcPending_ = true`，调用 `safepoint()`
- safepoint 内 flushTlab（所有线程）+ STW + GC（minorGc = mark + sweep + compact）
- **优点**：复用现有 STW 机制，正确性有保障，代码改动小
- **问题**：延迟 compact 变成走完整 GC 路径，mark-sweep 是多余开销
  （compactPending_ 场景只需 compact，不需要重新 mark-sweep）

### 方案 B：compact 前遍历 tlabList_ flush 所有线程（不推荐）
- **致命问题**：只 flush TLAB 不够，compact 是移动式 GC，
  执行期间其他线程还在运行，会读到半移动状态的对象 → 数据竞争
- **结论**：不安全，否决

### 方案 C：mini-STW（只暂停 compact，不触发完整 GC）
- **优点**：只做 compact，不做 mark-sweep，开销最小
- **问题**：需要新增 STW 状态机，复杂度高

### 方案 A+C 结合（推荐）✅
**复用 safepoint 的 STW 暂停机制**（保证所有线程停止，TLAB 全部 flush），
**但 STW 期间根据"是否需要完整 GC"区分两种路径**：
- 需要完整 GC（youngBytes/oldBytes 超阈值等）→ 正常 GC 路径（minorGc 内处理 compact）
- 只需要 compact（无 GC 阈值触发）→ A+C 路径，仅 compact，跳过 mark-sweep

## A+C 结合方案细化

### compact 独立执行的可行性

compact 的实现（`compact.cpp`）：
- `computeForwardingAddresses`：遍历 `youngObjects_`/`oldObjects_`，为每个对象分配新地址
- `updateAllReferences`：更新所有引用指向新地址
- `copyObjectsToNewLocations`：拷贝对象到新位置
- `rebuildPageList`：重建页链表

**compact 不依赖 marked 标志**，它搬运所有列表中的对象。
所以可以独立执行，不需要先跑 mark-sweep。

### 唯一代价

compact 会搬运已死对象（mark-sweep 没跑，不知道哪些死了）→ 浪费空间。
但这是**暂时的**——下次正常 GC 会回收。
而且 compactPending_ 场景本身就是因为碎片率高才触发的，搬运已死对象也比不 compact 强。

### 核心思路（双布尔变量结合）

tryAlloc 延迟 compact 路径不再直接执行 compact，改为：
1. 设置 `compactPending_ = true`（标记有 compact 请求）
2. 设置 `gcPending_ = true`（触发 safepoint STW）
3. 调用 `safepoint()`

safepoint 内结合两个布尔变量判断执行路径：
```cpp
bool needCompactOnly = compactPending_.exchange(false);  // compact 延迟请求
bool needFullGc = (youngBytes_ >= kYoungThreshold / 2) ||  // GC 阈值触发
                  (oldBytes_ >= kOldThreshold) ||
                  shouldCompactMedium() ||
                  shouldSweepLargePages();

if (needFullGc) {
    // 正常 GC 路径：minorGc/mixedGc/majorGc/sweepLargePages
    // minorGc 内 shouldCompact 会处理 compact（compactSuspendedCount_ == 0 时）
    // needCompactOnly 已被 exchange 消费，compact 由 minorGc 内部处理
    ...
} else if (needCompactOnly) {
    // A+C 路径：只 compact，跳过 mark-sweep
    ...
}
```

### 为什么不能只看 compactPending_

**场景**：compactPending_ = true（有延迟 compact 请求），同时 youngBytes_ 也超阈值。

**错误做法**（只看 compactPending_）：
```
if (needCompactOnly) compact(...)  // 只 compact，漏掉 mark-sweep
else minorGc(...)                    // 永远不会执行
```
→ 已死对象不被回收，内存持续增长，compact 搬运已死对象也浪费空间。

**正确做法**（双布尔结合）：
```
if (needFullGc) minorGc(...)  // 完整 GC（含 compact）
else if (needCompactOnly) compact(...)  // 只 compact
```
→ 有 GC 需求时优先走完整 GC，minorGc 内 shouldCompact 会处理 compact；
  无 GC 需求时才走 A+C 只 compact 路径。

### 文件修改

#### 1. `runtime/gc/gc.h`
- `compactPending_` 改为 `std::atomic<bool>`（消除数据竞争）
  ```cpp
  std::atomic<bool> compactPending_{false};
  ```

#### 2. `runtime/gc/alloc.cpp`（tryAlloc 延迟 compact 路径）
```cpp
if (compactSuspendedCount_.load() == 0 && compactPending_.load()) {
    // 不再直接 compact，改为走 safepoint STW 机制
    // safepoint 内会 flushTlab（所有线程）+ STW
    // safepoint 内根据 needFullGc/needCompactOnly 双布尔判断执行路径
    gcPending_.store(true);
    safepoint();
}
```
- **注意**：不在这里消费 compactPending_（不在 tryAlloc 中 exchange/store(false)）
  因为 safepoint 内 needFullGc 为 true 时，compact 由 minorGc 内部处理，
  此时 compactPending_ 应该保持 true 直到 minorGc 内消费
- **修正**：compactPending_ 的消费放在 safepoint 内
  - needFullGc 为 true 路径：minorGc 内部 shouldCompact 判断，
    若 compactSuspendedCount_ == 0 则执行 compact，并消费 compactPending_
    若 compactSuspendedCount_ > 0 则重新设置 compactPending_（minorGc 内已有此逻辑）
  - needCompactOnly 路径：exchange(false) 消费
  - 所以 safepoint 入口先 exchange(false) 消费，needFullGc 路径若 minorGc 内
    再次延迟会重新 store(true)，逻辑自洽

#### 3. `runtime/gc/safepoint.cpp`（单线程 + 多线程路径）
在 GC 执行前增加双布尔变量结合判断：
```cpp
// 单线程路径
gcPending_.store(false);
// A+C 结合：双布尔变量结合判断
bool needCompactOnly = compactPending_.exchange(false);
bool needFullGc = (youngBytes_ >= kYoungThreshold / 2) ||
                  (oldBytes_ >= kOldThreshold) ||
                  (shouldCompactMedium() && !compactSuspendedCount_.load()) ||
                  (shouldSweepLargePages() && !compactSuspendedCount_.load());

if (needFullGc) {
    // 正常 GC 路径：minorGc 内 shouldCompact 会处理 compact
    // 注：needCompactOnly 已被 exchange 消费，若 minorGc 内 compact 被延迟
    //     会重新设置 compactPending_ = true，下次 tryAlloc 再次走 safepoint
    if (youngBytes_ >= kYoungThreshold / 2) minorGc();
    if (shouldCompactMedium() && !compactSuspendedCount_.load()) mixedGc();
    if (oldBytes_ >= kOldThreshold) majorGc();
    if (shouldSweepLargePages() && !compactSuspendedCount_.load()) sweepLargePages();
} else if (needCompactOnly) {
    // A+C 路径：只 compact，跳过 mark-sweep
    if (compactSuspendedCount_.load() > 0) {
        compactPending_.store(true);  // Guard 仍活跃，重新延迟
    } else if (shouldCompact(CompactScope::Young)) {
        compact(CompactScope::Young);
    } else if (shouldCompact(CompactScope::All)) {
        compact(CompactScope::All);
    }
}
```
多线程路径（initiator）同理。

#### 4. `runtime/gc/mark_sweep.cpp`
compactPending_ 读写改为 atomic 操作：
- `compactPending_ = true` → `compactPending_.store(true)`（3 处：L35, L419, L519）

### 边界条件

1. **compactPending_ 在 safepoint 内被消费后，minorGc 内又设置新的 compactPending_**
   - 场景：needFullGc 为 true，minorGc 内 compact 被 Guard 延迟
   - 处理：minorGc 内 `compactPending_.store(true)`（已有逻辑，改为 atomic）
   - 下次 tryAlloc 再次走 safepoint，双布尔重新判断
   - 正确性：保证，不会丢失 compact 请求

2. **多线程下多个线程同时进入 tryAlloc 延迟 compact 路径**
   - 场景：线程 A 和 B 都读到 `compactPending_ == true`
   - 处理：两个线程都设置 `gcPending_ = true` 并调用 safepoint
   - safepoint 内 `compactPending_.exchange(false)` 只有一个线程会得到 true
   - 另一个线程的 exchange 返回 false，走 needFullGc 或空操作路径
   - 正确性：保证，无重复 compact

3. **safepoint 内 compact 被 compactSuspendedCount_ 阻止**
   - 场景：A+C 路径（needCompactOnly 为 true），但 Guard 仍活跃
   - 处理：`compactPending_.store(true)` 重新延迟
   - 下次 tryAlloc 再次走 safepoint
   - 正确性：保证

4. **compact 搬运已死对象导致空间浪费**
   - 场景：A+C 路径（needCompactOnly 为 true, needFullGc 为 false），
     youngObjects_ 含已死对象（未 mark-sweep）
   - 处理：compact 搬运所有对象（含已死），浪费空间
   - 下次正常 GC 回收已死对象
   - 正确性：保证，只是暂时浪费空间

5. **needFullGc 与 needCompactOnly 同时为 true**
   - 场景：compactPending_ = true 且 youngBytes_ 超阈值
   - 处理：走 needFullGc 路径（完整 GC），minorGc 内 shouldCompact 处理 compact
   - needCompactOnly 已被 exchange 消费，compact 由 minorGc 内部处理
   - 正确性：保证，既回收内存又整理碎片

### 测试方案

#### T1: 单线程回归（确保不破坏现有功能）
- 编译 test.aura，运行 T1-T5 全部通过
- 验证 compact 延迟路径仍正常工作

#### T2: 多线程 compact 触发
```aura
sync thread {
    sync max=4 {
        for i in 0..100000 {
            var s = "x"
            s = s + "a"  # 触发大量分配 → compact
        }
    }
}
```
- 验证：无崩溃、无 SIGSEGV、无数据损坏
- 验证：GC 统计正常（gc/minor/mixed 计数合理）

#### T3: ASAN 验证
- ASAN 模式编译，运行 T2 测试
- 验证：无 use-after-free、无 heap-buffer-overflow

## 影响分析

### 受影响文件
| 文件 | 修改 |
|------|------|
| `runtime/gc/gc.h` | `compactPending_` 改 atomic |
| `runtime/gc/alloc.cpp` | tryAlloc 延迟 compact 路径走 safepoint |
| `runtime/gc/safepoint.cpp` | 单/多线程路径增加双布尔结合判断 |
| `runtime/gc/mark_sweep.cpp` | compactPending_ 读写改 atomic（3 处） |

### 风险
- **低风险**：复用现有 safepoint STW 机制，不引入新并发原语
- **性能影响**：延迟 compact 路径只做 compact（跳过 mark-sweep），开销最小
- **空间代价**：A+C 路径搬运已死对象，暂时浪费空间，下次 GC 回收
- **向后兼容**：API 无变更，仅内部实现调整

### 依赖
- 无外部依赖
- 基于现有 safepoint STW 机制（`safepoint.cpp:62-126`）

## 实施步骤

1. `gc.h`：`compactPending_` 改 `std::atomic<bool>`
2. `mark_sweep.cpp`：3 处 `compactPending_ = true` 改为 `.store(true)`
3. `alloc.cpp`：tryAlloc 延迟 compact 路径改为 `gcPending_.store(true); safepoint();`
4. `safepoint.cpp`：单/多线程路径增加双布尔结合判断：
   - `needCompactOnly = compactPending_.exchange(false)`
   - `needFullGc` = GC 阈值/碎片率/分配失败率触发条件
   - needFullGc 优先：走正常 GC（minorGc 内处理 compact）
   - 否则 needCompactOnly：只 compact（跳过 mark-sweep），
     若 compactSuspendedCount_ > 0 则重新设置 compactPending_ = true
5. 编译验证（不测试）
