#pragma once
// ============================================================
// BuiltinRegistry — 内置类型/方法/函数集中注册表
//
// 单例，全内联在 .h 中。
// 数据来源：
//   init() C++ 硬编码 — 基础类型 + string/[T] 方法 + range
//   builtin/*.aurai（Phase 4）— Io/Path 模块接口声明
// ============================================================

#include "SemType.h"
#include "../AST/Stmt.h"
#include "../AST/Type.h"
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Aura {

// ============================================================
// 数据结构
// ============================================================

enum class BuiltinPrim : uint8_t { Int, Float, Bool, String, None_, Other };

struct BuiltinTypeInfo {
    std::string name;           // Aura 类型名
    bool        isHeap;         // 堆指针类型（GcString*, Array<T>*）
    bool        isBuiltin;      // 内置类型 vs 用户 type 别名
    BuiltinPrim primKind;       // 基础类型标记
    std::string cppType;        // 映射到的 C++ 类型（如 "int32_t"）
};

struct ParamInfo {
    std::string name;
    std::string typeName;       // 参数类型名（"int", "[T]", "string", "Path" 等）
};

struct ReturnTypeInfo {
    enum class Kind { Named, Generic, None, Generator };
    Kind kind;
    std::string typeName;       // Named 时 / Generic fallback / Generator 元素类型
    int  genericParamIdx = 0;   // Generic 时：引用第几个参数的类型（0-based）

    static ReturnTypeInfo Named(const std::string& tn)    { return {Kind::Named, tn, 0}; }
    static ReturnTypeInfo Generic(int idx, const std::string& fb) { return {Kind::Generic, fb, idx}; }
    static ReturnTypeInfo None()                          { return {Kind::None, "", 0}; }
    static ReturnTypeInfo Generator(const std::string& el){ return {Kind::Generator, el, 0}; }
};

struct BuiltinGlobalFn {
    std::string name;
    std::vector<ParamInfo> params;   // 按参数数量区分重载
    ReturnTypeInfo  returns;
    bool throws = false;
};

struct BuiltinMethod {
    std::string typeName;       // "string", "[T]", "Io", "Path"
    std::string methodName;
    std::vector<ParamInfo> params;
    ReturnTypeInfo  returns;
    bool throws  = false;       // 是否标记 throws
    bool hasAsync = false;      // 是否有异步版本（用于协程判定，Io 方法特有）
};

// ============================================================
// BuiltinRegistry — 单例注册表
// ============================================================

class BuiltinRegistry {
public:
    static BuiltinRegistry& get() {
        static BuiltinRegistry instance;
        return instance;
    }

    // ----- 类型查询 -----
    const BuiltinTypeInfo* findType(const std::string& name) const {
        auto it = types_.find(name);
        return it != types_.end() ? &it->second : nullptr;
    }

    bool isHeapType(const std::string& name) const {
        auto* ti = findType(name);
        return ti && ti->isHeap;
    }

    // ----- 方法查询（按 typeName + methodName + 参数数量）-----
    const BuiltinMethod* findMethod(const std::string& typeName,
                                     const std::string& methodName,
                                     int argCount) const {
        for (auto& m : methods_) {
            if (m.typeName == typeName && m.methodName == methodName
                && (int)m.params.size() == argCount)
                return &m;
        }
        return nullptr;
    }

    // ----- 方法名是否存在（不检查参数数量，用于错误提示）-----
    bool hasMethodName(const std::string& typeName,
                       const std::string& methodName) const {
        for (auto& m : methods_)
            if (m.typeName == typeName && m.methodName == methodName)
                return true;
        return false;
    }

    // ----- 某方法是否有异步版本（用于协程判定）-----
    bool methodHasAsync(const std::string& typeName,
                        const std::string& methodName) const {
        for (auto& m : methods_)
            if (m.typeName == typeName && m.methodName == methodName && m.hasAsync)
                return true;
        return false;
    }

    // ----- 列出某类型的所有方法名 -----
    std::vector<const BuiltinMethod*> listMethods(const std::string& typeName) const {
        std::vector<const BuiltinMethod*> result;
        for (auto& m : methods_)
            if (m.typeName == typeName)
                result.push_back(&m);
        return result;
    }

    std::vector<std::string> listMethodNames(const std::string& typeName) const {
        std::vector<std::string> names;
        for (auto& m : methods_)
            if (m.typeName == typeName)
                names.push_back(m.methodName);
        return names;
    }

    // ----- 全局函数查询 -----
    const BuiltinGlobalFn* findFunction(const std::string& name, int argCount) const {
        for (auto& f : functions_) {
            if (f.name == name && (int)f.params.size() == argCount)
                return &f;
        }
        return nullptr;
    }

    // ============================================================
    // .aurai 加载（Phase 4）
    //
    // 将已解析的 .aurai AST 加载到注册表中。
    // 单例保证：同一 .aurai 文件只加载一次。
    // ============================================================
    bool tryLoadAurai(const std::string& baseName, const Program& ast) {
        // 防重复：同一 .aurai 只加载一次
        if (loadedAurai_.count(baseName)) return false;
        loadedAurai_.insert(baseName);
        doLoadAurai(ast);
        return true;
    }

    // 列出已加载的 .aurai 文件（用于调试）
    const std::set<std::string>& loadedAurai() const { return loadedAurai_; }

private:
    void doLoadAurai(const Program& ast) {
        for (auto& d : ast.decls) {
            if (!d) continue;
            if (auto* td = dynamic_cast<const TypeDecl*>(d.get())) {
                // .aurai 中的 type Name 是前向声明 — 不覆盖已有类型
                if (types_.count(td->name)) continue;
                BuiltinTypeInfo ti;
                ti.name      = td->name;
                ti.isHeap    = false;
                ti.isBuiltin = true;
                ti.primKind  = BuiltinPrim::Other;
                ti.cppType   = "";
                types_[td->name] = ti;
            } else if (auto* md = dynamic_cast<const MethodDecl*>(d.get())) {
                BuiltinMethod bm;
                bm.typeName  = md->receiverType;
                bm.methodName = md->name;
                bm.throws    = md->throws;
                for (auto& p : md->params) {
                    bm.params.push_back({p.name, typeExprToName(p.type.get())});
                }
                bm.returns = extractReturnType(md->returnType.get());
                // Io 方法根据名称判断 hasAsync
                if (bm.typeName == "Io" && md->name != "file_exists" && md->name != "cwd")
                    bm.hasAsync = true;
                methods_.push_back(std::move(bm));
            } else if (auto* fn = dynamic_cast<const FunDecl*>(d.get())) {
                BuiltinGlobalFn gf;
                gf.name   = fn->name;
                gf.throws = fn->throws;
                for (auto& p : fn->params) {
                    gf.params.push_back({p.name, typeExprToName(p.type.get())});
                }
                gf.returns = extractReturnType(fn->returnType.get());
                functions_.push_back(std::move(gf));
            }
        }
    }

    BuiltinRegistry() { init(); }

    void init() {
        // ============================================================
        // 类型（始终 C++ 硬编码）
        // ============================================================
        types_ = {
            {"int",    {"int",    false, true, BuiltinPrim::Int,    "int32_t"}},
            {"float",  {"float",  false, true, BuiltinPrim::Float,  "double"}},
            {"bool",   {"bool",   false, true, BuiltinPrim::Bool,   "bool"}},
            {"string", {"string", true,  true, BuiltinPrim::String, "aura_rt::GcString*"}},
            {"None",   {"None",   false, true, BuiltinPrim::None_,  "aura_rt::NoneType"}},
            {"Io",     {"Io",     false, true, BuiltinPrim::Other,    "aura_rt::Io"}},
            {"Path",   {"Path",   false, true, BuiltinPrim::Other,    "aura_rt::Path"}},
            {"channel",{"channel",true,  true, BuiltinPrim::Other,    "aura_rt::Channel*"}},
        };

        // ============================================================
        // 方法（始终 C++ 硬编码）
        // ============================================================
        methods_ = {
            // --- string 方法 ---
            {"string", "len",    {},                           ReturnTypeInfo::Named("int")},
            {"string", "concat", {{"other", "string"}},        ReturnTypeInfo::Generic(0, "string")},

            // --- [T] 方法（13 个）---
            {"[T]", "len",       {},                                ReturnTypeInfo::Named("int")},
            {"[T]", "size",      {},                                ReturnTypeInfo::Named("int")},
            {"[T]", "empty",     {},                                ReturnTypeInfo::Named("bool")},
            {"[T]", "capacity",  {},                                ReturnTypeInfo::Named("int")},
            {"[T]", "front",     {},                                ReturnTypeInfo::Generic(0, "[T]")},
            {"[T]", "back",      {},                                ReturnTypeInfo::Generic(0, "[T]")},
            {"[T]", "append",    {{"value", "T"}},                  ReturnTypeInfo::None()},
            {"[T]", "pop",       {},                                ReturnTypeInfo::Generic(0, "[T]")},
            {"[T]", "pop",       {{"idx", "int"}},                  ReturnTypeInfo::Generic(0, "[T]")},
            {"[T]", "remove",    {{"idx", "int"}},                  ReturnTypeInfo::Generic(0, "[T]")},
            {"[T]", "insert",    {{"idx", "int"}, {"value", "T"}},  ReturnTypeInfo::None()},
            {"[T]", "clear",     {},                                ReturnTypeInfo::None()},
            {"[T]", "reserve",   {{"cap", "int"}},                  ReturnTypeInfo::None()},
            // Io / Path 方法不再硬编码，由 builtins/*.aurai 加载

            // --- channel<T> 方法 ---
            {"channel", "send",    {{"value", "T"}},  ReturnTypeInfo::None()},
            {"channel", "receive", {},                  ReturnTypeInfo::Generic(0, "channel")},
            {"channel", "close",   {},                  ReturnTypeInfo::None()},
        };

        // ============================================================
        // 全局函数（始终 C++ 硬编码）
        // ============================================================
        functions_ = {
            {"range", {{"end", "int"}},                                      ReturnTypeInfo::Generator("int")},
            {"range", {{"start", "int"}, {"end", "int"}},                    ReturnTypeInfo::Generator("int")},
            {"range", {{"start", "int"}, {"end", "int"}, {"step", "int"}},   ReturnTypeInfo::Generator("int")},
            // path.new / path.join 不再硬编码，由 builtins/path.aurai 按需加载
            // channel 构造函数
            {"channel", {{"cap", "int"}},  ReturnTypeInfo::Named("channel")},
        };
    }

    std::unordered_map<std::string, BuiltinTypeInfo> types_;
    std::vector<BuiltinMethod>                       methods_;
    std::vector<BuiltinGlobalFn>                     functions_;
    std::set<std::string>                            loadedAurai_;  // 已加载的 .aurai 文件名

    // ============================================================
    // AuraiLoader 辅助
    // ============================================================
    static std::string typeExprToName(const TypeExpr* t) {
        if (!t) return "";
        if (auto* nt = dynamic_cast<const NamedType*>(t)) return nt->name;
        if (auto* gt = dynamic_cast<const GenericTypeRef*>(t)) return gt->name;
        if (auto* lt = dynamic_cast<const ListType*>(t)) {
            std::string inner = typeExprToName(lt->elementType.get());
            return "[" + inner + "]";
        }
        return "";
    }

    static ReturnTypeInfo extractReturnType(const TypeExpr* t) {
        if (!t) return ReturnTypeInfo::None();
        if (auto* nt = dynamic_cast<const NamedType*>(t))
            return ReturnTypeInfo::Named(nt->name);
        if (auto* lt = dynamic_cast<const ListType*>(t)) {
            std::string inner = typeExprToName(lt->elementType.get());
            return ReturnTypeInfo::Named("[" + inner + "]");
        }
        return ReturnTypeInfo::None();
    }
};

} // namespace Aura
