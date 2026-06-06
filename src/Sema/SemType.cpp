#include "SemType.h"
#include <algorithm>
#include <sstream>

namespace Aura {

// ============================================================
// ErrorSemType
// ============================================================
bool ErrorSemType::equals(const SemType& other) const {
    return dynamic_cast<const ErrorSemType*>(&other) != nullptr;
}
std::unique_ptr<SemType> ErrorSemType::clone() const { return make(); }

// ============================================================
// PrimSemType
// ============================================================
bool PrimSemType::equals(const SemType& other) const {
    auto* o = dynamic_cast<const PrimSemType*>(&other);
    return o && o->kind == kind;
}
std::string PrimSemType::toString() const {
    switch (kind) {
    case Int:    return "int";
    case Float:  return "float";
    case Bool:   return "bool";
    case String: return "string";
    }
    return "???";
}
std::unique_ptr<SemType> PrimSemType::clone() const {
    return std::make_unique<PrimSemType>(kind);
}

// ============================================================
// NoneSemType
// ============================================================
bool NoneSemType::equals(const SemType& other) const {
    return dynamic_cast<const NoneSemType*>(&other) != nullptr;
}
std::unique_ptr<SemType> NoneSemType::clone() const { return make(); }

// ============================================================
// RecordSemType
// ============================================================
bool RecordSemType::equals(const SemType& other) const {
    auto* o = dynamic_cast<const RecordSemType*>(&other);
    if (!o) return false;
    if (fields.size() != o->fields.size()) return false;
    // 结构等价：字段名和类型匹配，忽略顺序
    for (auto& f : fields) {
        auto it = std::find_if(o->fields.begin(), o->fields.end(),
            [&](const RecordFieldSem& of) {
                return of.name == f.name && typeEquals(f.type, of.type);
            });
        if (it == o->fields.end()) return false;
    }
    return true;
}
std::string RecordSemType::toString() const {
    std::ostringstream oss;
    oss << "{ ";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << fields[i].name << ": " << (fields[i].type ? fields[i].type->toString() : "?");
    }
    oss << " }";
    return oss.str();
}
std::unique_ptr<SemType> RecordSemType::clone() const {
    auto n = std::make_unique<RecordSemType>();
    for (auto& f : fields) {
        n->fields.push_back({f.name, f.type ? f.type->clone() : nullptr});
    }
    return n;
}

// ============================================================
// UnionSemType
// ============================================================
bool UnionSemType::equals(const SemType& other) const {
    auto* o = dynamic_cast<const UnionSemType*>(&other);
    if (!o) return false;
    if (variants.size() != o->variants.size()) return false;
    for (size_t i = 0; i < variants.size(); ++i) {
        if (!typeEquals(variants[i], o->variants[i])) return false;
    }
    return true;
}
std::string UnionSemType::toString() const {
    std::ostringstream oss;
    for (size_t i = 0; i < variants.size(); ++i) {
        if (i > 0) oss << " | ";
        oss << (variants[i] ? variants[i]->toString() : "?");
    }
    return oss.str();
}
std::unique_ptr<SemType> UnionSemType::clone() const {
    auto n = std::make_unique<UnionSemType>();
    for (auto& v : variants) {
        n->variants.push_back(v ? v->clone() : nullptr);
    }
    return n;
}

// ============================================================
// ListSemType
// ============================================================
bool ListSemType::equals(const SemType& other) const {
    auto* o = dynamic_cast<const ListSemType*>(&other);
    return o && typeEquals(elementType, o->elementType);
}
std::string ListSemType::toString() const {
    return "[" + (elementType ? elementType->toString() : "?") + "]";
}
std::unique_ptr<SemType> ListSemType::clone() const {
    auto n = std::make_unique<ListSemType>();
    n->elementType = elementType ? elementType->clone() : nullptr;
    return n;
}

// ============================================================
// FuncSemType
// ============================================================
bool FuncSemType::equals(const SemType& other) const {
    auto* o = dynamic_cast<const FuncSemType*>(&other);
    if (!o) return false;
    if (throws != o->throws) return false;
    if (paramTypes.size() != o->paramTypes.size()) return false;
    for (size_t i = 0; i < paramTypes.size(); ++i) {
        if (!typeEquals(paramTypes[i], o->paramTypes[i])) return false;
    }
    return typeEquals(returnType, o->returnType);
}
std::string FuncSemType::toString() const {
    std::ostringstream oss;
    oss << "(";
    for (size_t i = 0; i < paramTypes.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << (paramTypes[i] ? paramTypes[i]->toString() : "?");
    }
    oss << ") -> " << (returnType ? returnType->toString() : "?");
    if (throws) oss << " throws";
    return oss.str();
}
std::unique_ptr<SemType> FuncSemType::clone() const {
    auto n = std::make_unique<FuncSemType>();
    for (auto& p : paramTypes) {
        n->paramTypes.push_back(p ? p->clone() : nullptr);
    }
    n->returnType = returnType ? returnType->clone() : nullptr;
    n->throws = throws;
    return n;
}

// ============================================================
// InterfaceSemType
// ============================================================
bool InterfaceSemType::equals(const SemType& other) const {
    auto* o = dynamic_cast<const InterfaceSemType*>(&other);
    if (!o) return false;
    if (methods.size() != o->methods.size()) return false;
    // 结构等价：方法名和签名匹配
    for (auto& m : methods) {
        auto it = std::find_if(o->methods.begin(), o->methods.end(),
            [&](const MethodSig& om) {
                if (om.name != m.name) return false;
                if (om.throws != m.throws) return false;
                if (om.paramTypes.size() != m.paramTypes.size()) return false;
                for (size_t i = 0; i < m.paramTypes.size(); ++i) {
                    if (!typeEquals(m.paramTypes[i], om.paramTypes[i])) return false;
                }
                return typeEquals(m.returnType, om.returnType);
            });
        if (it == o->methods.end()) return false;
    }
    return true;
}
std::string InterfaceSemType::toString() const {
    return "interface " + name;
}
std::unique_ptr<SemType> InterfaceSemType::clone() const {
    auto n = std::make_unique<InterfaceSemType>();
    n->name = name;
    for (auto& m : methods) {
        MethodSig ms;
        ms.name = m.name;
        for (auto& p : m.paramTypes) {
            ms.paramTypes.push_back(p ? p->clone() : nullptr);
        }
        ms.returnType = m.returnType ? m.returnType->clone() : nullptr;
        ms.throws = m.throws;
        n->methods.push_back(std::move(ms));
    }
    return n;
}

// ============================================================
// GenericSemType
// ============================================================
bool GenericSemType::equals(const SemType& other) const {
    auto* o = dynamic_cast<const GenericSemType*>(&other);
    return o && o->name == name;
}
std::string GenericSemType::toString() const {
    return "<" + name + ">";
}
std::unique_ptr<SemType> GenericSemType::clone() const {
    auto n = std::make_unique<GenericSemType>();
    n->name = name;
    return n;
}

} // namespace Aura
