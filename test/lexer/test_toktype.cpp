// ============================================================
// test_toktype.cpp — TokType 关键字查找与名称映射测试
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"
#include "TokType.h"

using namespace Aura;
using namespace aura_test;

TEST(TokType, KeywordLookupAllKeywords) {
    // 所有关键字都应被识别为对应 TokType
    EXPECT_EQ(lookupKeyword("fun"), TokType::Fun);
    EXPECT_EQ(lookupKeyword("let"), TokType::Let);
    EXPECT_EQ(lookupKeyword("const"), TokType::Const);
    EXPECT_EQ(lookupKeyword("throws"), TokType::Throws);
    EXPECT_EQ(lookupKeyword("throw"), TokType::Throw);
    EXPECT_EQ(lookupKeyword("try"), TokType::Try);
    EXPECT_EQ(lookupKeyword("catch"), TokType::Catch);
    EXPECT_EQ(lookupKeyword("match"), TokType::Match);
    EXPECT_EQ(lookupKeyword("if"), TokType::If);
    EXPECT_EQ(lookupKeyword("else"), TokType::Else);
    EXPECT_EQ(lookupKeyword("for"), TokType::For);
    EXPECT_EQ(lookupKeyword("while"), TokType::While);
    EXPECT_EQ(lookupKeyword("loop"), TokType::Loop);
    EXPECT_EQ(lookupKeyword("break"), TokType::Break);
    EXPECT_EQ(lookupKeyword("continue"), TokType::Continue);
    EXPECT_EQ(lookupKeyword("spawn"), TokType::Spawn);
    EXPECT_EQ(lookupKeyword("sync"), TokType::Sync);
    EXPECT_EQ(lookupKeyword("return"), TokType::Return);
    EXPECT_EQ(lookupKeyword("import"), TokType::Import);
    EXPECT_EQ(lookupKeyword("type"), TokType::Type);
    EXPECT_EQ(lookupKeyword("interface"), TokType::Interface);
    EXPECT_EQ(lookupKeyword("true"), TokType::True);
    EXPECT_EQ(lookupKeyword("false"), TokType::False);
    EXPECT_EQ(lookupKeyword("None"), TokType::None);
    EXPECT_EQ(lookupKeyword("impl"), TokType::Impl);
    EXPECT_EQ(lookupKeyword("pub"), TokType::Pub);
    EXPECT_EQ(lookupKeyword("and"), TokType::And);
    EXPECT_EQ(lookupKeyword("or"), TokType::Or);
    EXPECT_EQ(lookupKeyword("not"), TokType::Not);
}

TEST(TokType, KeywordLookupNonKeywords) {
    // 非关键字 → Identifier
    EXPECT_EQ(lookupKeyword("funx"), TokType::Identifier);
    EXPECT_EQ(lookupKeyword("Fun"), TokType::Identifier);   // 大小写敏感
    EXPECT_EQ(lookupKeyword("let_"), TokType::Identifier);
    EXPECT_EQ(lookupKeyword("_let"), TokType::Identifier);
    EXPECT_EQ(lookupKeyword("spawner"), TokType::Identifier);
    EXPECT_EQ(lookupKeyword("syncs"), TokType::Identifier);
    EXPECT_EQ(lookupKeyword("none"), TokType::Identifier);  // None 大写才是关键字
    EXPECT_EQ(lookupKeyword("True"), TokType::Identifier);
    EXPECT_EQ(lookupKeyword("async"), TokType::Identifier); // 无 async 关键字
    EXPECT_EQ(lookupKeyword("await"), TokType::Identifier);
    EXPECT_EQ(lookupKeyword("as"), TokType::Identifier);    // as 不是关键字
    EXPECT_EQ(lookupKeyword(""), TokType::Identifier);
}

TEST(TokType, KeywordLookupCaseSensitive) {
    // 关键字区分大小写
    EXPECT_EQ(lookupKeyword("IF"), TokType::Identifier);
    EXPECT_EQ(lookupKeyword("Return"), TokType::Identifier);
    EXPECT_EQ(lookupKeyword("TYPE"), TokType::Identifier);
    EXPECT_EQ(lookupKeyword("None"), TokType::None);
    EXPECT_EQ(lookupKeyword("none"), TokType::Identifier);
}

TEST(TokType, TypeNameMapping) {
    // tokTypeName 不应返回空串
    EXPECT_FALSE(tokTypeName(TokType::Fun).empty());
    EXPECT_FALSE(tokTypeName(TokType::Eof).empty());
    EXPECT_FALSE(tokTypeName(TokType::Error).empty());
    // 所有枚举值都有名称
    for (int i = 0; i <= (int)TokType::Error; ++i) {
        EXPECT_FALSE(tokTypeName((TokType)i).empty());
    }
}

TEST(TokType, KeywordBoundary) {
    // 关键字枚举范围检查：Fun..None 是关键字区间
    // 软关键字 lock/thread 不是 TokType 关键字（在 Parser 中特殊处理）
    EXPECT_EQ(lookupKeyword("lock"), TokType::Identifier);
    EXPECT_EQ(lookupKeyword("thread"), TokType::Identifier);
    EXPECT_EQ(lookupKeyword("self"), TokType::Identifier);
    EXPECT_EQ(lookupKeyword("io"), TokType::Identifier);
    EXPECT_EQ(lookupKeyword("main"), TokType::Identifier);
}
