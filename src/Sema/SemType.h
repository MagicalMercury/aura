#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Aura {

// 前置声明：EnumVariantSem 只持有指向关联值 AST 的裸指针（生命周期由 Program 保证），
// 故此处不需要 ASTNode 的完整定义（实施批 2 最小修正，见报告 §报红 B2-1）
struct ASTNode;

// ============================================================
// SemType ─ 语义类型（非 AST，用于类型检查）
// 所有类型是不可变的，使用 std::unique_ptr<SemType> 传递
// ============================================================

struct SemType {
    virtual ~SemType() = default;

    // 结构等价性比较
    [[nodiscard]] virtual bool equals(const SemType& other) const = 0;

    // 可读字符串
    [[nodiscard]] virtual std::string toString() const = 0;

    // 深拷贝
    [[nodiscard]] virtual std::unique_ptr<SemType> clone() const = 0;
};

// --- 基础类型 ---
struct ErrorSemType : SemType {
    [[nodiscard]] bool equals(const SemType& other) const override;
    [[nodiscard]] std::string toString() const override { return "error"; }
    [[nodiscard]] std::unique_ptr<SemType> clone() const override;
    static std::unique_ptr<ErrorSemType> make() { return std::make_unique<ErrorSemType>(); }
};

struct PrimSemType : SemType {
    enum Kind { Int, Float, Bool, String } kind;
    explicit PrimSemType(Kind k) : kind(k) {}
    [[nodiscard]] bool equals(const SemType& other) const override;
    [[nodiscard]] std::string toString() const override;
    [[nodiscard]] std::unique_ptr<SemType> clone() const override;
};

struct NoneSemType : SemType {
    [[nodiscard]] bool equals(const SemType& other) const override;
    [[nodiscard]] std::string toString() const override { return "None"; }
    [[nodiscard]] std::unique_ptr<SemType> clone() const override;
    static std::unique_ptr<NoneSemType> make() { return std::make_unique<NoneSemType>(); }
};

// --- 复合类型 ---
struct RecordFieldSem {
    std::string name;
    std::unique_ptr<SemType> type;
};

struct RecordSemType : SemType {
    std::vector<RecordFieldSem> fields; // 字段按定义顺序，但等价性检查忽略顺序
    std::string canonicalName;          // 类型别名名（如 "Tree"），用于 CodeGen 映射 C++ 类型
    bool isTuple = false;               // 元组（匿名 record 语法糖）：位置字段 _0/_1/...；canonicalName 留空，C++ 类型名由 CodeGen 现场合成
    [[nodiscard]] bool equals(const SemType& other) const override;
    [[nodiscard]] std::string toString() const override;
    [[nodiscard]] std::unique_ptr<SemType> clone() const override;
};

struct UnionSemType : SemType {
    std::vector<std::unique_ptr<SemType>> variants;
    [[nodiscard]] bool equals(const SemType& other) const override;
    [[nodiscard]] std::string toString() const override;
    [[nodiscard]] std::unique_ptr<SemType> clone() const override;
};

// enum 的 variant 语义表示（成员表挂在 EnumSemType 上，**不进符号表**）
struct EnumVariantSem {
    std::string name;
    // 形态 1（int 支撑）
    bool    hasExplicitValue = false;   // 是否写了 `= 整数`
    int64_t intValue = 0;               // 最终整数值（未写则自增后的值）
    // 形态 2（绑定类型）
    std::unique_ptr<SemType> valueType; // 关联值的语义类型
    const ASTNode*           valueExpr = nullptr;  // 关联值 AST（编译期常量；CodeGen 直接生成）
};

// enum 类型。⚠️ 判等语义：按【类型名身份】而非结构等价（SemType 家族里首个按名判等的类型，
// 否则 `enum type A = {X}` 与 `enum type B = {X}` 会被判为同一类型）——见 change.md §4.5
// ⚠️ 跨模块（F-1）：身份 = 【模块限定名 + 名字】。`declModule` 由 `qualifyRecordTypes`
//    （src/Sema/SemAnalyzer.cpp:52–75）在 importExports 路径填成导入别名。
struct EnumSemType : SemType {
    std::string name;                                        // 类型名（如 "Color"）
    std::string canonicalName;                               // = name（方法表键；跨模块限定名见 §4.9）
    // 模块限定名（F-1）：本模块声明 ⇒ 空串；经 `importExports` 导入的副本 ⇒ 别名（如 "math"）。
    // 🔴 **不要**把限定名塞进 canonicalName —— canonicalName 参与 CodeGen 的 C++ 类型名映射
    //    与方法表键（record 侧先例会加 `alias::`，enum 侧照抄会污染生成名）。
    std::string declModule;
    std::vector<EnumVariantSem> variants;
    bool plainInt = true;                                    // true = 形态 1；false = 形态 2（有绑定类型）
    std::unique_ptr<SemType> bindingType;                    // 形态 2 的绑定类型（形态 1 为 nullptr）
    [[nodiscard]] bool equals(const SemType& other) const override;
    [[nodiscard]] std::string toString() const override;     // "enum Color"
    [[nodiscard]] std::unique_ptr<SemType> clone() const override;
};

struct ListSemType : SemType {
    std::unique_ptr<SemType> elementType;
    [[nodiscard]] bool equals(const SemType& other) const override;
    [[nodiscard]] std::string toString() const override;
    [[nodiscard]] std::unique_ptr<SemType> clone() const override;
};

struct FuncSemType : SemType {
    std::vector<std::unique_ptr<SemType>> paramTypes;
    std::unique_ptr<SemType> returnType;
    bool throws = false;
    [[nodiscard]] bool equals(const SemType& other) const override;
    [[nodiscard]] std::string toString() const override;
    [[nodiscard]] std::unique_ptr<SemType> clone() const override;
};

// feature-06（阶段 C）：裸 Callable 类型（origins 溯源签名集）
// 溯源签名集：空 = erased（裸 Callable 形参 / 跨模块 opaque / 传播丢失）
// 运行时 C++ 表示 = aura_rt::CallableErased*（GC 堆包装，desc 追踪 target 槽）
struct CallableSemType : SemType {
    // 溯源签名集（shared_ptr 共享不可变签名——clone 只拷贝句柄）
    std::vector<std::shared_ptr<const FuncSemType>> origins;
    bool erased() const { return origins.empty(); }
    // equals：同为 Callable 即等（origins 不参与相等性——赋值兼容性由
    // Assignability 按 origins 判定，避免传播精度差异影响类型等同）
    [[nodiscard]] bool equals(const SemType& o) const override {
        return dynamic_cast<const CallableSemType*>(&o) != nullptr;
    }
    [[nodiscard]] std::string toString() const override {
        if (erased()) return "Callable";
        std::string s = "Callable<";
        for (size_t i = 0; i < origins.size(); ++i)
            s += (i ? " | " : "") + origins[i]->toString();
        return s + ">";
    }
    [[nodiscard]] std::unique_ptr<SemType> clone() const override {
        auto n = std::make_unique<CallableSemType>();
        n->origins = origins;   // shared_ptr 共享签名（不可变）
        return n;
    }
};

struct InterfaceSemType : SemType {
    std::string name;
    // 泛型接口实例化实参（如 Comparable<Point> 的 [Point]）；非泛型接口为空。
    // P2b：union 变体为泛型接口视图时，mapSemType 需要实参生成完整 C++ 类型名
    // （Comparable<Point*>），否则生成裸模板名 "Comparable" 无法编译。
    std::vector<std::unique_ptr<SemType>> typeArgs;
    // 方法签名列表（在定义接口时填充）
    struct MethodSig {
        std::string name;
        std::vector<std::unique_ptr<SemType>> paramTypes;
        std::unique_ptr<SemType> returnType;
        bool throws = false;
        bool hasDefault = false;   // 接口默认方法（结构匹配时豁免，实现者无需提供）
        bool hasCppImpl = false;   // C++ 桥接方法（CppBridge，record 无需实现，同豁免）
        size_t defaultCount = 0;   // 尾部默认参数个数（record 方法由 buildTypeMethods 填充）
    };
    std::vector<MethodSig> methods;
    [[nodiscard]] bool equals(const SemType& other) const override;
    [[nodiscard]] std::string toString() const override;
    [[nodiscard]] std::unique_ptr<SemType> clone() const override;
};

// 泛型类型变量 <T>（未实例化时保留名称，实例化后替换为具体类型）
struct GenericSemType : SemType {
    std::string name;           // 如 "A", "T"
    std::string resolvedName;   // 自引用类型的 C++ 名（如 "Tree<int32_t>"），空 = 未解析
    [[nodiscard]] bool equals(const SemType& other) const override;
    [[nodiscard]] std::string toString() const override;
    [[nodiscard]] std::unique_ptr<SemType> clone() const override;
};

// 迭代器类型 — range() 返回 Iter<int>，未来 .iter() 返回 Iter<T>
struct IterSemType : SemType {
    std::unique_ptr<SemType> elementType;
    [[nodiscard]] bool equals(const SemType& other) const override;
    [[nodiscard]] std::string toString() const override {
        return "Iter<" + (elementType ? elementType->toString() : "?") + ">";
    }
    [[nodiscard]] std::unique_ptr<SemType> clone() const override;
    static std::unique_ptr<IterSemType> make(std::unique_ptr<SemType> el) {
        auto n = std::make_unique<IterSemType>();
        n->elementType = std::move(el);
        return n;
    }
};

// Optional<T> 类型 — sync.ThreadChannel.receive 等方法的返回类型
// 与 runtime/types.h 的 aura_rt::Optional<T> 对应
struct OptionalSemType : SemType {
    std::unique_ptr<SemType> elementType;
    [[nodiscard]] bool equals(const SemType& other) const override;
    [[nodiscard]] std::string toString() const override {
        return "Optional<" + (elementType ? elementType->toString() : "?") + ">";
    }
    [[nodiscard]] std::unique_ptr<SemType> clone() const override;
    static std::unique_ptr<OptionalSemType> make(std::unique_ptr<SemType> el) {
        auto n = std::make_unique<OptionalSemType>();
        n->elementType = std::move(el);
        return n;
    }
};

// ============================================================
// 工具函数
// ============================================================

// 内置类型的便捷工厂
inline std::unique_ptr<PrimSemType> intType()    { return std::make_unique<PrimSemType>(PrimSemType::Int); }
inline std::unique_ptr<PrimSemType> floatType()  { return std::make_unique<PrimSemType>(PrimSemType::Float); }
inline std::unique_ptr<PrimSemType> boolType()   { return std::make_unique<PrimSemType>(PrimSemType::Bool); }
inline std::unique_ptr<PrimSemType> stringType() { return std::make_unique<PrimSemType>(PrimSemType::String); }

// 解引用比较两个 unique_ptr<SemType>
inline bool typeEquals(const std::unique_ptr<SemType>& a, const std::unique_ptr<SemType>& b) {
    if (!a && !b) return true;
    if (!a || !b) return false;
    return a->equals(*b);
}

// ============================================================
// bug-107：enum match 中「裸名臂」的定向解析（三处同源唯一真源）
// ------------------------------------------------------------
// 裸标识符模式（`Red` / `Foo` / `Color v`）在 Parser 侧一律是 TypePattern
// （src/Parser/TypeParser.cpp:231–241，裸名变体与类型模式语法同形 ⇒ Parser 无缺陷，
// 由 Sema 按 scrutinee 的变体表定向解析）。当 scrutinee 是 enum 时，判定顺序**必须**是：
//   ① varName 非空（`Color v`）           ⇒ 类型模式（整型兜底 `<all>` 恒真）—— 既有语义不变
//   ② 纯裸名且 typeName ∈ 变体表          ⇒ **变体常量模式**（tag 比较）—— **先查变体表**
//   ③ 纯裸名且 typeName == 枚举名          ⇒ 类型模式（`<all>` 恒真）—— 保持现状
//   ④ 其余                                ⇒ Other（交给调用方的既有「不可定向」诊断/兜底）
// 🔴 陷阱（bug-107 根因）：变体名与枚举类型名同名时（`enum type Foo = { Foo, Bar }`），
//    旧代码「先判 typeName == 枚举名」⇒ 裸名 `Foo` 被误判成类型模式 ⇒ 生成 `if (true)`、
//    `Bar` 臂死代码、且穷尽性判全覆盖 ⇒ `aurac` **零诊断**（静默错码）。
//    ⇒ ② 必须排在 ③ **之前**（先查变体表）。
// 三处调用点（判定顺序必须一致，任一单独改都会分叉）：
//   · src/Sema/Checker/StmtFlow.cpp       —— 覆盖判定 / 重复检测 / 不可定向报错
//   · src/Sema/Checker/ExprInferMisc.cpp  —— isMatchExhaustive（穷尽性）
//   · src/CodeGen/StmtMatch.cpp           —— 生成 tag 比较 / 恒真兜底
// ============================================================
enum class BareEnumPatternKind {
    Variant,   // ② 纯裸名命中变体表 ⇒ 变体常量模式（生成 tag 比较）
    TypeAll,   // ①/③ 类型模式（`Color v` 或裸枚举名）⇒ `<all>` 恒真兜底
    Other      // ④ 既非变体亦非本枚举名 ⇒ 既有行为（"cannot be resolved" 类诊断）
};

inline BareEnumPatternKind resolveBareEnumPattern(const EnumSemType& en,
                                                  const std::string& typeName,
                                                  bool hasVarName) {
    if (hasVarName) return BareEnumPatternKind::TypeAll;        // ① `Color v`
    for (auto& v : en.variants)
        if (v.name == typeName) return BareEnumPatternKind::Variant;  // ② 先查变体表
    if (typeName == en.name) return BareEnumPatternKind::TypeAll;     // ③ 裸枚举名
    return BareEnumPatternKind::Other;                                // ④
}

// 便捷判定：名是否是该 enum 的变体（与 SemAnalyzer::isEnumVariant 同判据的跨层内联版）
inline bool enumHasVariant(const EnumSemType& en, const std::string& name) {
    for (auto& v : en.variants) if (v.name == name) return true;
    return false;
}

} // namespace Aura
