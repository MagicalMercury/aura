// ============================================================
// test/rt/test_enum_gc.cpp — enum 形态 2 单例/关联值的 GC 语义（**运行期**）
//
// 背景（change.md §4.7.4b / B-1 + C-1 + G-1）：
//   生成侧原先设想的「函数局部 static 裸指针缓存」在 compact 后悬垂（B-1），
//   且 magic-static guard 内可能 alloc（C-1）。修复 = 形式 A **直调 intern_string**。
//   本文件用【与生成形态逐字一致】的手写体，在**真 compact** 后断言语义：
//   文本断言抓不到「compact 后悬垂」与「guard/alloc 死锁面」，故必须有这一层。
//
// ⚠️ include 只取必要头（与 test_gc_bug86.cpp 同口径，避开 aura_rt.h 的
//    `inline thread_local` TLS init 双定义链接问题）。
// ⚠️ 覆盖 scope：调用方**逐例**记录实际走到的 scope（不静默降级成「不 compact 的假通过」）。
// ============================================================
#include "gc/gc.h"              // GcHeap::compact / CompactScope / gc_force_major / GcRootHandle
#include "builtin/string.h"     // intern_string / string_eq / GcString
#include "framework/test_framework.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <type_traits>

// 与 change.md §4.4-⑩ 生成形态一致的手写体（形态 2 / 形式 A）
struct EnumErrorCode {
    enum class _Tag : uint8_t { not_found = 0, permission = 1 };
    _Tag _tag = _Tag::not_found;

    static const EnumErrorCode not_found;
    static const EnumErrorCode permission;

    // 形式 A：**每次直调 intern_string**（无 static 缓存、无动态初始化、无 guard）
    static const aura_rt::GcString* _val_not_found()  { return aura_rt::intern_string("NOT_FOUND"); }
    static const aura_rt::GcString* _val_permission() { return aura_rt::intern_string("PERMISSION"); }

    bool operator==(const EnumErrorCode& o) const { return _tag == o._tag; }
    bool operator!=(const EnumErrorCode& o) const { return _tag != o._tag; }
};
const EnumErrorCode EnumErrorCode::not_found{EnumErrorCode::_Tag::not_found};
const EnumErrorCode EnumErrorCode::permission{EnumErrorCode::_Tag::permission};

namespace {

// 走一次真正覆盖 `CompactScope::All` 的压缩，并回显实际 scope（防「假通过」）
bool forceCompactAll() {
    aura_rt::gc_force_major();
    aura_rt::GcHeap::instance().compact(aura_rt::GcHeap::CompactScope::All);
    std::cout << "[EnumGc] compact(CompactScope::All) executed\n";
    return true;
}

// ⚠️ 报红 B3-6（最小修正）：change.md §4.7.4b 的手写体逐字写
//    `EXPECT_TRUE(aura_rt::string_eq(after, EnumErrorCode::_val_not_found()))`，但
//    `string_eq` 的形参是**非 const** 的 `GcString*`（runtime/builtin/string.h:201），
//    而形式 A 的访问器返回 `const aura_rt::GcString*`（§4.4-⑩ 的签名）
//    ⇒ 逐字编译失败：`invalid conversion from 'const aura_rt::GcString*' to 'aura_rt::GcString*'`。
//    最小修正 = 显式 `const_cast`（不新增只读重载 ⇒ 不动 runtime，红线）。
inline aura_rt::GcString* nc(const aura_rt::GcString* p) {
    return const_cast<aura_rt::GcString*>(p);
}

} // namespace

// ------------------------------------------------------------
// ① 单例 tag 语义 + 形式 A 关联值：compact(All) 后仍有效且内容正确
// ------------------------------------------------------------
TEST(EnumGc, SingletonAndFormAValueSurviveCompactAll) {
    auto* before = EnumErrorCode::_val_not_found();
    ASSERT_TRUE(before != nullptr);
    EXPECT_EQ(std::string(before->view()), std::string("NOT_FOUND"));

    EXPECT_TRUE(forceCompactAll());     // ★ 覆盖 All 压缩路径（真实执行，非文本断言）

    auto* after = EnumErrorCode::_val_not_found();                          // 形式 A：重新取地址
    ASSERT_TRUE(after != nullptr);
    EXPECT_EQ(std::string(after->view()), std::string("NOT_FOUND"));        // 悬垂则此处崩/错
    EXPECT_TRUE(aura_rt::string_eq(nc(after), nc(EnumErrorCode::_val_not_found())));// 重复调用返回同一对象
    EXPECT_FALSE(aura_rt::string_eq(nc(after), nc(EnumErrorCode::_val_permission())));

    // tag 语义（按 variant 身份）在 compact 前后不变
    EXPECT_TRUE(EnumErrorCode::not_found == EnumErrorCode::not_found);
    EXPECT_TRUE(EnumErrorCode::not_found != EnumErrorCode::permission);
}

// ------------------------------------------------------------
// ② 形式 B 的对照（批 B 形态）：`GcRootHandle<…>{…, Global}` 的 val_ 会被原位更新
//    本用例证明「形式 B 安全 vs 形式 A 曾不安全」的唯一差别（`ptr_ref_ = &val_`）
// ------------------------------------------------------------
TEST(EnumGc, FormBGlobalRootHandleSurvivesCompactAll) {
    static aura_rt::GcRootHandle<aura_rt::GcString*> h{
        aura_rt::intern_string("PLAYER_ACTIVE"), aura_rt::GcRootScope::Global};

    ASSERT_TRUE(h.get() != nullptr);
    EXPECT_EQ(std::string(h.get()->view()), std::string("PLAYER_ACTIVE"));

    EXPECT_TRUE(forceCompactAll());

    ASSERT_TRUE(h.get() != nullptr);                        // 根槽被 compact 原位更新 ⇒ 仍有效
    EXPECT_EQ(std::string(h.get()->view()), std::string("PLAYER_ACTIVE"));
}

// ------------------------------------------------------------
// ③（批 B 正式形态）形态 2 的「含 GC 指针的关联值」：真 record + compact 后字段可读
//    与生成形态一致：函数局部 static + Global 根槽（模板 ②，`nullptr` 占位）⇒ 惰性构造
// ------------------------------------------------------------
struct EnumWrapper : aura_rt::GcObject {
    aura_rt::GcString* name = nullptr;
    int32_t            hp   = 0;
    static const aura_rt::TypeDescriptor _desc;
};

namespace {
const size_t kEnumWrapperPtrOffsets[] = { offsetof(EnumWrapper, name) };
} // namespace

const aura_rt::TypeDescriptor EnumWrapper::_desc = {
    sizeof(EnumWrapper), 1, kEnumWrapperPtrOffsets, 0, nullptr, nullptr, nullptr
};

TEST(EnumGc, FormBRecordValueSurvivesCompactAll) {
    static aura_rt::GcRootHandle<EnumWrapper*> hp{nullptr, aura_rt::GcRootScope::Global};
    if (!hp.get()) {
        hp.set(aura_rt::gc_alloc<EnumWrapper>(&EnumWrapper::_desc));   // 先入根
        hp.get()->name = aura_rt::intern_string("WRAPPER_VALUE");      // 再构造字段（可安全触发 GC）
        hp.get()->hp   = 7;
    }
    EXPECT_TRUE(forceCompactAll());                                    // ★ bug-111 契约：必须走 forceCompactAll()
    ASSERT_TRUE(hp.get() != nullptr);                                  // 根槽被原位更新
    EXPECT_EQ(hp.get()->hp, 7);
    ASSERT_TRUE(hp.get()->name != nullptr);
    EXPECT_EQ(std::string(hp.get()->name->view()), std::string("WRAPPER_VALUE"));
}
