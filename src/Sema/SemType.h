#pragma once

#include <memory>
#include <string>
#include <vector>

namespace Aura {

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

struct InterfaceSemType : SemType {
    std::string name;
    // 方法签名列表（在定义接口时填充）
    struct MethodSig {
        std::string name;
        std::vector<std::unique_ptr<SemType>> paramTypes;
        std::unique_ptr<SemType> returnType;
        bool throws = false;
    };
    std::vector<MethodSig> methods;
    [[nodiscard]] bool equals(const SemType& other) const override;
    [[nodiscard]] std::string toString() const override;
    [[nodiscard]] std::unique_ptr<SemType> clone() const override;
};

// 泛型类型变量 <T>（未实例化时保留名称，实例化后替换为具体类型）
struct GenericSemType : SemType {
    std::string name; // 如 "A", "T"
    [[nodiscard]] bool equals(const SemType& other) const override;
    [[nodiscard]] std::string toString() const override;
    [[nodiscard]] std::unique_ptr<SemType> clone() const override;
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

} // namespace Aura
