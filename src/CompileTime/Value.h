// ============================================================
// feature-21 Phase 3 · S2：编译期值模型 Value / TypeInfo（header-only）
//
// 权威：change.md §4.4（`change.md:368-455`）
// 简报：scripts/f21_p3_s2_brief.md
//
// 约定：
//   - 仅数据 + 字面量构造 / 结构相等 / 调试输出，不含任何求值逻辑
//     （求值在 Evaluator/EvalPass，本批不接线）。
//   - `TypeInfo::params` 递归承载 List 元素 / Optional payload / Union 分支；
//     故类型信息可表达嵌套泛型（如 `Box<Box<int>>`）。
//   - `Value::type` 指向该值的递归类型信息（§4.6 `Evaluator::typeOf` 填充）。
// ============================================================

#pragma once
#include <cstdint>
#include <memory>
#include <utility>
#include <string>
#include <vector>

namespace Aura::ct {

struct TypeInfo {
    enum class Kind { Unknown, Int, Float, Bool, String, None,
                      List, Record, Enum, Optional, Union };
    Kind kind = Kind::Unknown;
    std::string name;                              // Record/Enum/Union 类型名
    std::vector<std::string> typeArgs;             // 泛型实参
    std::vector<std::shared_ptr<TypeInfo>> params; // List 元素 / Optional 元素 / Union 分支
};

struct Value {
    enum class Kind { Int, Float, Bool, String, None, List, Record, Enum,
                      Optional, Union };
    Kind kind = Kind::None;
    int64_t i = 0;
    double f = 0.0;
    bool b = false;
    std::string s;                       // String / Record/Enum/Union 类型名
    std::string variant;                 // Enum/Union 活动变体
    std::vector<Value> elems;            // List / Record 字段 / Optional payload / Union payload
    std::vector<std::string> names;      // Record 字段名
    std::shared_ptr<TypeInfo> type;      // 递归类型信息（3A）
    bool hasValue = false;               // Optional: present?

    static Value makeInt(int64_t v);
    static Value makeFloat(double v);
    static Value makeBool(bool v);
    static Value makeString(std::string v);
    static Value makeNone();
    bool equals(const Value& other) const;
    std::string toString() const;
};

inline Value Value::makeInt(int64_t v)       { Value x; x.kind = Kind::Int;    x.i = v; return x; }
inline Value Value::makeFloat(double v)      { Value x; x.kind = Kind::Float;  x.f = v; return x; }
inline Value Value::makeBool(bool v)         { Value x; x.kind = Kind::Bool;   x.b = v; return x; }
inline Value Value::makeString(std::string v){ Value x; x.kind = Kind::String; x.s = std::move(v); return x; }
inline Value Value::makeNone()               { Value x; x.kind = Kind::None;   return x; }

inline bool Value::equals(const Value& o) const {
    if (kind != o.kind) return false;
    switch (kind) {
        case Kind::Int:    return i == o.i;
        case Kind::Float:  return f == o.f;
        case Kind::Bool:   return b == o.b;
        case Kind::String: return s == o.s;
        case Kind::None:   return true;
        case Kind::List:
        case Kind::Record:
        case Kind::Optional:
        case Kind::Union: {
            if (hasValue != o.hasValue || variant != o.variant || s != o.s) return false;
            if (elems.size() != o.elems.size() || names != o.names) return false;
            for (size_t k = 0; k < elems.size(); ++k)
                if (!elems[k].equals(o.elems[k])) return false;
            return true;
        }
        case Kind::Enum:   return s == o.s && variant == o.variant;
    }
    return false;
}

inline std::string Value::toString() const {
    switch (kind) {
        case Kind::Int:    return std::to_string(i);
        case Kind::Float:  return std::to_string(f);
        case Kind::Bool:   return b ? "true" : "false";
        case Kind::String: return s;
        case Kind::None:   return "None";
        case Kind::Enum:   return s + "." + variant;
        case Kind::List: {
            std::string out = "[";
            for (size_t k = 0; k < elems.size(); ++k) {
                if (k) out += ", ";
                out += elems[k].toString();
            }
            return out + "]";
        }
        case Kind::Record: {
            std::string out = s + "{";
            for (size_t k = 0; k < elems.size(); ++k) {
                if (k) out += ", ";
                out += (k < names.size() ? names[k] : std::string("?")) + "=" + elems[k].toString();
            }
            return out + "}";
        }
        case Kind::Optional:
            return (hasValue && !elems.empty()) ? ("some(" + elems.front().toString() + ")") : "none()";
        case Kind::Union:
            return elems.empty() ? ("<" + s + ">") : elems.front().toString();
    }
    return "";
}

} // namespace Aura::ct
