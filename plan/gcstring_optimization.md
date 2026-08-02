# GcString 优化方案（剩余项）

> 日期：2026-07-20（v7 精简版）
> 状态：主体完成，本文档仅保留剩余项
> 历史实施：第一/二/三阶段 + 第四阶段 D1/D2 已全部完成（详见 [TODO.txt §六](file:///d:/you/Aura/TODO.txt)）
> 关联：
> - [done/plan13.md](file:///d:/you/Aura/plan/done/plan13.md)：GcString 重构原始 plan
> - [gc_features_plan.md](file:///d:/you/Aura/plan/gc_features_plan.md)：GC 功能完整 plan（§二 CodeGen 生成 GcRootHandle，GcString 间接依赖）
> - [generational_paged_gc.md](file:///d:/you/Aura/plan/generational_paged_gc.md)：分代分页 GC + LOS（GcString 拼接/append 等所有 alloc 路径需 GcRootHandle 保护，已在阶段 1 修复）

---

## 一、已完成项一笔带过

| 阶段 | 内容 | 实施日期 |
|:---|:---|:---:|
| 第一阶段 Step 1 | 空串/布尔/小整数缓存（GcGlobalRoot 包装） | 2026-07-19 |
| 第一阶段 Step 2 | concat_multi 运行时支持 | 2026-07-19 |
| 第一阶段 Step 3 | CodeGen 链式 `+` 脱糖 concat_multi | 2026-07-19 |
| 第二阶段 Step 4a | GcObject 头部压缩 56→16 字节（bit-packed flags + uint32 allocSize） | 2026-07-20 |
| 第二阶段 Step 4b | GcString union（capacity/offset 共用槽位） | 2026-07-20 |
| 第二阶段 Step 4 | string.append + CodeGen 优化 `s = s + x` + concat_multi A 优化 + concat 重构为 concat_multi 包装 | 2026-07-20 |
| 第二阶段 Step 5 | 子串共享 slice（零拷贝，parent + offset） | 2026-07-20 |
| 第三阶段 Step 7 | Rope 表示（GcRopeNode 继承 GcString + 三层防护 + 三个入口接入） | 2026-07-20 |
| 第四阶段 D1 | 字面量 Intern 池（intern_string + g_internPool + CodeGen 改造） | 2026-07-20 |
| 第四阶段 D2 | 小整数缓存扩展 [-128, 127] → [-1024, 1023] | 2026-07-20 |
| v6.1 修复 | 静态对象析构顺序崩溃（`[[gnu::init_priority(101-105)]]`） | 2026-07-20 |

---

## 二、剩余项

### D3. 动态字符串 Intern API（可选，P3）

**前置条件**：GcWeakHandle 弱引用机制（[gc.h:75-96](file:///d:/you/Aura/runtime/gc.h#L75)）已落地，可用于弱引用版本 intern。

**改动文件**：[runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) + [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) + [src/Sema/BuiltinRegistry.h](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h)

**API 设计**：

```cpp
// 显式 intern 任意 GcString（含动态生成的）
// 相同内容返回同一指针
GcString* GcString::intern(const GcString* s);
```

**实现思路**：
- 复用 D1 的 `g_internPool` 和 `g_internMutex`（[string.cpp:478-481](file:///d:/you/Aura/runtime/builtin/string.cpp#L478)）
- 双重检查锁定，与 D1 路径一致
- 入池的 GcString 通过 `GcGlobalRoot` 注册为全局根

**风险与缓解**：
- ⚠️ 内存膨胀：动态 intern 的字符串永不回收
- ✅ 缓解：引入弱引用版本 `intern_weak`，GcWeakHandle 包装池内对象；GC 时若外部无引用则回收
- ⚠️ 风险点：弱引用 intern 与字面量 intern 共用一个池需特别处理（字面量必须强引用保活）

**BuiltinRegistry 注册**：

```cpp
// Aura 侧使用
let key = intern("user_" + id + "_config")
```

**收益**：用户显式去重热点字符串，多次查询同一 key 时零分配。

---

### D4. from(int) 统一到 Intern 池（长期，P3）

**前置条件**：D3 弱引用 intern 机制成熟（避免内存膨胀）。

**目标**：将 D2 的固定范围缓存统一到 D1/D3 的 intern 池，消除两套缓存机制。

**改造**：

```cpp
GcString* GcString::from(int32_t val) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", val);
    return intern_string(buf, static_cast<size_t>(len));
}
```

**优势**：
- 统一缓存机制（一套 intern 池覆盖所有场景）
- 无需固定范围限制（任意 int32_t 都能去重）
- 代码简洁

**劣势**：
- intern 池增长（所有 int 都入池）
- 需要弱引用版本才能避免内存膨胀

**当前决策**：推迟到弱引用 intern 机制成熟后再做。短期保留 D2 的静态扩展 [-1024, 1023]。

---

### 哈希缓存（推迟，P2）

**前置条件**：引入 `Map<K, V>` 类型（当前未引入）。

```cpp
struct GcString : GcObject {
    // ... 现有字段
    mutable uint32_t hash = 0;  // 0 表示未计算

    uint32_t get_hash() const {
        if (hash == 0) hash = compute_hash(data(), length);
        return hash;
    }
};
```

**当前不可做**：BuiltinRegistry 未注册 Map 类型，hash 缓存收益为 0。

**注意**：加 `hash` 字段会使 GcString 头部从 32 字节膨胀到 40 字节，需评估是否值得。可在 Map 类型落地时再做。

---

## 三、明确排除项

- ❌ **GC 暂停期 interning**：D1（字面量）+ D3（动态）已覆盖主要场景，无需在 GC 暂停期做 interning
- ❌ **单引用原地修改（is_unique）**：需引用计数，破坏 GC 简单性，明确放弃

---

## 四、推荐执行顺序

1. **D3 动态 intern API**（可选）：需先评估实际场景中是否真有用户去重需求，否则可推迟
2. **D4 from(int) 统一**：D3 弱引用版本成熟后再做
3. **哈希缓存**：Map<K,V> 类型落地后同步推进

**当前结论**：GcString 优化主体已完成，剩余项均依赖其他系统（弱引用 / Map 类型），暂无紧迫任务。
