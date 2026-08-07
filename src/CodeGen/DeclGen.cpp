#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include <sstream>

namespace Aura {

// ============================================================
// 记录类型声明（plan §4.2）
// ============================================================

void CodeGenerator::genTypeDecl(std::ostream& h, std::ostream& cpp,
                                 const TypeDecl& decl) {
    if (!decl.type) return;

    // 记录类型 → 生成 struct
    if (auto* rec = dynamic_cast<const RecordType*>(decl.type.get())) {
        // 优先使用 TypeDecl 显式声明的泛型参数，否则从字段中扫描 GenericTypeRef
        std::vector<std::string> tparams = decl.typeParams;
        if (tparams.empty()) {
            for (auto& f : rec->fields) {
                if (f.type && dynamic_cast<const GenericTypeRef*>(f.type.get())) {
                    tparams.push_back(dynamic_cast<const GenericTypeRef*>(f.type.get())->name);
                }
            }
        }
        genRecordStruct(h, cpp, decl.name, *rec, tparams);
        // 记录模板参数表（供后续 genLetStmt 等跳过模板类型的 canonicalName）
        if (!tparams.empty())
            typeAliasTemplateParams_[decl.name] = tparams;
        return;
    }

    // 其他类型（函数类型、联合类型、命名类型）→ 生成 C++ using 别名
    std::string mappedType = mapType(*decl.type);

    // 注册类型名（使后续代码生成知晓该类型的存在）
    registerTypeName(decl.name, false); // 函数/联合/命名类型默认非堆对象

    // 收集泛型参数：优先用 decl.typeParams，否则从类型内部扫描 GenericTypeRef
    std::vector<std::string> tparams = decl.typeParams;
    if (tparams.empty()) {
        std::set<std::string> tpSet;
        collectTParams(*decl.type, tpSet);
        tparams.assign(tpSet.begin(), tpSet.end());
    }

    // 记录类型别名的模板参数（供后续 funSignature 生成 Name<T,U> 形式的返回类型）
    if (!tparams.empty())
        typeAliasTemplateParams_[decl.name] = tparams;

    // 模板前缀
    if (!tparams.empty()) {
        h << "template<";
        for (size_t i = 0; i < tparams.size(); ++i) {
            if (i > 0) h << ", ";
            h << "typename " << tparams[i];
        }
        h << ">\n";
    }
    h << "using " << decl.name << " = " << mappedType << ";\n\n";
}

void CodeGenerator::genRecordStruct(std::ostream& h, std::ostream& cpp,
                                     const std::string& name,
                                     const RecordType& body,
                                     const std::vector<std::string>& tparams) {
    // 模板前缀
    std::string tprefix;
    if (!tparams.empty()) {
        tprefix = "template<";
        for (size_t i = 0; i < tparams.size(); ++i) {
            if (i > 0) tprefix += ", ";
            tprefix += "typename " + tparams[i];
        }
        tprefix += ">\n";
    }

    h << tprefix << "struct " << name << " : aura_rt::GcObject {\n";

    std::vector<std::string> ptrFields;
    std::set<std::string> fieldNames;

    for (auto& f : body.fields) {
        std::string cppType = f.type ? mapType(*f.type) : "???";
        h << "  " << cppType << " " << safeName(f.name) << ";\n";
        fieldNames.insert(f.name);

        // 检测 GC 指针字段：任何映射后以 * 结尾的 C++ 类型都是堆对象指针
        // 包括 NamedType（如 User*）、ListType（如 Array<T>*）、GcString* 等
        if (!cppType.empty() && cppType.back() == '*') {
            ptrFields.push_back(safeName(f.name));
        } else if (isIfaceViewTypeName(cppType)) {
            // 接口视图字段（值类型，非 * 结尾）：注册 self 子偏移
            // 格式 "field+ViewType"，genTypeDescriptor 展开为
            // offsetof(Self, field) + offsetof(ViewType, self)
            ptrFields.push_back(safeName(f.name) + "+" + cppType);
        }
    }

    structFieldNames_[name] = fieldNames;

    // 收集此类型的所有方法声明并嵌入 struct 内部
    for (auto& md : pendingMethods_) {
        if (md.receiverType == name) {
            // 检测方法名与字段名冲突（C++ 不允许同名成员函数与成员变量）
            std::string cppMethodName = safeName(md.methodName);
            if (fieldNames.count(cppMethodName))
                cppMethodName += "_fun";
            h << "  " << md.returnTypeStr << " " << cppMethodName << "(";
            for (size_t i = 0; i < md.paramTypes.size(); ++i) {
                if (i > 0) h << ", ";
                h << md.paramTypes[i] << " " << safeName(md.paramNames[i]);
            }
            h << ");\n";
        }
    }

    h << "\n  static const aura_rt::TypeDescriptor _desc;\n";
    h << "};\n\n";

    // 模板类型的 _desc 必须在头文件中实例化（跨模块链接需要）
    genTypeDescriptor(tparams.empty() ? cpp : h, name, tparams, ptrFields);
}

// ============================================================
// 接口声明 → 值视图结构体 + XFunc GC 化（P1 全接口去虚化）
// ============================================================

void CodeGenerator::genInterfaceDecl(std::ostream& h,
                                      const InterfaceDecl& decl) {
    std::string name = decl.name;

    // 内置 Iterator：C++ 形态来自 runtime/builtin/iterator.h（aura_rt::Iterator<T>），
    // 不在此生成视图（避免与 runtime 的 Iterator<T> 重复/冲突）。
    // interfaces.aurai 中的声明仅供 Sema（方法签名），record impl 适配器走 genIfaceAdapter 特判。
    if (name == "Iterator") return;

    // 泛型接口 → 模板视图（template<typename T> struct Comparable { ... }）
    // 方法签名中的泛型引用（GenericTypeRef → "T"）在模板作用域内有效
    std::string tprefix;
    if (!decl.typeParams.empty()) {
        tprefix = "template<";
        for (size_t i = 0; i < decl.typeParams.size(); ++i) {
            if (i > 0) tprefix += ", ";
            tprefix += "typename " + decl.typeParams[i];
        }
        tprefix += ">\n";
    }

    // 1. 值视图结构体（B+W 统一对象模型）：
    //    - 每个纯虚方法 → 无捕获函数指针字段（名 = 方法名 + "Fn"，避免与成员函数名冲突）
    //      + 转发成员函数（内部调用函数指针，首参 self）
    //    - 默认方法 → 视图内普通成员函数，体内经 currentReceiverName_="self" 映射 this，
    //      转发调用本视图的纯虚转发成员（完全复用现有 genBlock 翻译）
    //    - CppBridge（返回类型含未绑定 U，无法在 C++ 表达）→ 不生成
    h << tprefix << "struct " << name << " {\n";
    // 1a. 函数指针字段（仅纯虚方法；默认方法不占 Fn 字段，由视图内默认方法体承接）
    for (auto& m : decl.methods) {
        if (m.defaultBody || m.bodyKind == InterfaceMethodSig::BodyKind::CppBridge) continue;
        std::string retType = m.returnType ? mapType(*m.returnType) : "void";
        h << "  " << retType << " (*" << m.name << "Fn)(aura_rt::GcObject* self";
        for (size_t i = 0; i < m.params.size(); ++i) {
            h << ", " << (m.params[i].type ? mapType(*m.params[i].type) : "auto");
        }
        h << ") = nullptr;\n";
    }
    h << "  aura_rt::GcObject* self = nullptr;\n";
    // 1b. 成员函数（纯虚转发 / 默认方法体）
    for (auto& m : decl.methods) {
        if (m.bodyKind == InterfaceMethodSig::BodyKind::CppBridge) continue;
        std::string retType = m.returnType ? mapType(*m.returnType) : "void";
        if (m.defaultBody) {
            // 默认方法：体内 Aura 代码（self.xxx(...)）经 genBlock 翻译为 this->xxx(...)
            h << "  " << retType << " " << m.name << "(";
            for (size_t i = 0; i < m.params.size(); ++i) {
                if (i > 0) h << ", ";
                h << (m.params[i].type ? mapType(*m.params[i].type) : "auto")
                  << " " << safeName(m.params[i].name);
            }
            h << ") {\n";
            currentReceiverName_ = "self";
            genBlock(h, *m.defaultBody, /*isCoroutine=*/false);
            currentReceiverName_.clear();
            // 清理方法体生成残留的变量跟踪状态（与 genMethodDecl 末尾一致）
            clearVarTrackingState();
            h << "  }\n";
        } else {
            // 纯虚方法：转发成员 → 函数指针
            h << "  " << retType << " " << m.name << "(";
            for (size_t i = 0; i < m.params.size(); ++i) {
                if (i > 0) h << ", ";
                h << (m.params[i].type ? mapType(*m.params[i].type) : "auto")
                  << " " << safeName(m.params[i].name);
            }
            h << ") { return " << m.name << "Fn(self";
            for (size_t i = 0; i < m.params.size(); ++i) {
                h << ", " << safeName(m.params[i].name);
            }
            h << "); }\n";
        }
    }
    h << "};\n\n";

    // 2. 闭包适配器（XFunc）——仅非泛型单方法接口（泛型接口无类型参数可绑定）
    //    GC 化：单继承 GcObject，std::function 由 finalizer 显式析构
    if (decl.typeParams.empty() && decl.methods.size() == 1) {
        auto& m = decl.methods[0];
        std::string retType = m.returnType ? mapType(*m.returnType) : "void";
        std::string params, argNames;
        for (size_t i = 0; i < m.params.size(); ++i) {
            if (i > 0) { params += ", "; argNames += ", "; }
            params += m.params[i].type ? mapType(*m.params[i].type) : "auto";
            argNames += safeName(m.params[i].name);
        }
        std::string fnType = "std::function<" + retType + "(" + params + ")>";
        h << "struct " << name << "Func final : aura_rt::GcObject {\n";
        // FnType 类型别名：C++ 语法不允许 qualified template-id 跟在 ~ 后（~std::function<...> 非法），
        // 用别名承接析构调用（与 runtime iterator.h 的 MapIter::fn_.~F() 同模式）
        h << "  using FnType = " << fnType << ";\n";
        h << "  FnType func;\n";
        h << "  explicit " << name << "Func(FnType f) : func(std::move(f)) {}\n";
        h << "  static " << retType << " " << m.name << "Fn(aura_rt::GcObject* self";
        for (size_t i = 0; i < m.params.size(); ++i) {
            h << ", " << (m.params[i].type ? mapType(*m.params[i].type) : "auto")
              << " " << safeName(m.params[i].name);
        }
        h << ") {\n";
        h << "    return static_cast<" << name << "Func*>(self)->func(" << argNames << ");\n";
        h << "  }\n";
        h << "  static " << name << " view(" << name << "Func* o) {\n";
        h << "    return { &" << m.name << "Fn, o };\n";
        h << "  }\n";
        h << "  static const aura_rt::TypeDescriptor& desc() {\n";
        h << "    static const aura_rt::TypeDescriptor d = { sizeof(" << name
          << "Func), 0, nullptr, 0, nullptr,\n";
        h << "        [](aura_rt::GcObject* obj) { static_cast<" << name
          << "Func*>(obj)->func.~FnType(); } };\n";
        h << "    return d;\n";
        h << "  }\n";
        h << "};\n\n";
    }
}

// ============================================================
// "类型 × 接口"适配器（方案 B：record 保持不动，适配器持值持有根）
// ============================================================

void CodeGenerator::genIfaceAdapter(std::ostream& h,
                                    const std::string& recordName,
                                    const InterfaceDecl& iface) {
    std::string adapterName = safeName(recordName) + iface.name;
    // 适配器可能生成于 record 定义之前（接口 decl 先于 type decl）：
    // 前向声明 record（重复声明无害），保证 GcRootHandle<record*> 成员合法
    h << "struct " << safeName(recordName) << ";\n";
    // 泛型接口：基类实例化为 Comparable<Point>（类型实参来自 interfaceImplementations_ 收集）
    // 同时构建"泛型形参名 → 具体 C++ 类型"映射（T → Point*），
    // 适配器无模板上下文，方法签名中的泛型引用必须替换为具体类型
    std::map<std::string, std::string> tmap;
    // 内置 Iterator：C++ 形态来自 runtime（aura_rt::Iterator<T>），基类名需带命名空间
    std::string baseType = (iface.name == "Iterator") ? "aura_rt::Iterator" : iface.name;
    if (!iface.typeParams.empty()) {
        auto recIt = interfaceImplementations_.find(recordName);
        if (recIt != interfaceImplementations_.end()) {
            auto ifIt = recIt->second.find(iface.name);
            if (ifIt != recIt->second.end() && !ifIt->second.empty()) {
                baseType += "<";
                for (size_t i = 0; i < ifIt->second.size(); ++i) {
                    if (i > 0) baseType += ", ";
                    baseType += ifIt->second[i];
                    if (i < iface.typeParams.size())
                        tmap[iface.typeParams[i]] = ifIt->second[i];
                }
                baseType += ">";
            }
        }
    }
    // 接口方法签名类型映射：泛型引用（T）→ 具体实参类型；其余走 mapType
    // 注：接口参数/返回类型中的裸泛型名（如 cmp(other: T) 的 T）由 TypeParser 解析为
    // NamedType 而非 GenericTypeRef，两者都需要查 tmap
    auto mapIfaceType = [&](const TypeExpr* t) -> std::string {
        if (auto* g = dynamic_cast<const GenericTypeRef*>(t)) {
            auto it = tmap.find(g->name);
            if (it != tmap.end()) return it->second;
        }
        if (auto* n = dynamic_cast<const NamedType*>(t)) {
            auto it = tmap.find(n->name);
            if (it != tmap.end()) return it->second;
        }
        return t ? mapType(*t) : "auto";
    };
    // 内置 Iterator：适配器 GC 化（单继承 GcObject），owner 裸指针经 desc 扫描；
    // 视图 {nextFn, self} 分派到 owner->next()（self 恒为适配器对象起始）
    if (iface.name == "Iterator") {
        std::string elem = "int32_t";
        auto recIt = interfaceImplementations_.find(recordName);
        if (recIt != interfaceImplementations_.end()) {
            auto ifIt = recIt->second.find(iface.name);
            if (ifIt != recIt->second.end() && !ifIt->second.empty())
                elem = ifIt->second[0];
        }
        h << "struct " << adapterName << " final : aura_rt::GcObject {\n";
        h << "  " << recordName << "* owner;\n";
        h << "  explicit " << adapterName << "(" << recordName << "* o) : owner(o) {}\n";
        h << "  static aura_rt::Optional<" << elem << ">* nextFn(aura_rt::GcObject* self) {\n";
        h << "    return static_cast<" << adapterName << "*>(self)->owner->next();\n";
        h << "  }\n";
        h << "  static aura_rt::Iterator<" << elem << "> view(" << adapterName << "* o) {\n";
        h << "    return { &nextFn, o };\n";
        h << "  }\n";
        h << "  static const aura_rt::TypeDescriptor& desc() {\n";
        h << "    static const size_t _o[] = { offsetof(" << adapterName << ", owner) };\n";
        h << "    static const aura_rt::TypeDescriptor d = { sizeof(" << adapterName
          << "), 1, _o, 0, nullptr, nullptr };\n";
        h << "    return d;\n";
        h << "  }\n";
        h << "};\n\n";
        return;
    }
    // 非 Iterator 接口：适配器 GC 化（单继承 GcObject），owner 裸指针经 desc 扫描；
    // 视图 { 纯虚方法Fn..., self } 分派到 owner->方法(...)
    // 仅非默认/非 CppBridge 方法生成 static Fn（视图结构体中只有纯虚方法占用 Fn 字段）
    std::string viewType = baseType;   // 泛型接口已实例化（如 Comparable<Point*>）
    h << "struct " << adapterName << " final : aura_rt::GcObject {\n";
    h << "  " << recordName << "* owner;\n";
    h << "  explicit " << adapterName << "(" << recordName << "* o) : owner(o) {}\n";
    for (auto& m : iface.methods) {
        if (m.defaultBody || m.bodyKind == InterfaceMethodSig::BodyKind::CppBridge) continue;
        std::string retType = m.returnType ? mapIfaceType(m.returnType.get()) : "void";
        h << "  static " << retType << " " << m.name << "Fn(aura_rt::GcObject* self";
        for (size_t i = 0; i < m.params.size(); ++i) {
            h << ", " << mapIfaceType(m.params[i].type.get())
              << " " << safeName(m.params[i].name);
        }
        h << ") {\n";
        h << "    return static_cast<" << adapterName << "*>(self)->owner->"
          << m.name << "(";
        for (size_t i = 0; i < m.params.size(); ++i) {
            if (i > 0) h << ", ";
            h << safeName(m.params[i].name);
        }
        h << ");\n";
        h << "  }\n";
    }
    // view()：聚合初始化，字段顺序 = 纯虚方法 Fn 声明序 + self
    h << "  static " << viewType << " view(" << adapterName << "* o) {\n";
    h << "    return {";
    std::string viewFields;
    for (auto& m : iface.methods) {
        if (m.defaultBody || m.bodyKind == InterfaceMethodSig::BodyKind::CppBridge) continue;
        if (!viewFields.empty()) viewFields += ", ";
        viewFields += "&" + std::string(m.name) + "Fn";
    }
    h << viewFields << (viewFields.empty() ? "o" : ", o") << " };\n";
    h << "  }\n";
    h << "  static const aura_rt::TypeDescriptor& desc() {\n";
    h << "    static const size_t _o[] = { offsetof(" << adapterName << ", owner) };\n";
    h << "    static const aura_rt::TypeDescriptor d = { sizeof(" << adapterName
      << "), 1, _o, 0, nullptr, nullptr };\n";
    h << "    return d;\n";
    h << "  }\n";
    h << "};\n\n";
}

// ============================================================
// 函数声明 + 实现（plan §4.6, §4.8）
// ============================================================

void CodeGenerator::clearVarTrackingState() {
    valueTypeVarNames_.clear();
    stringVarNames_.clear();
    gcRootVarNames_.clear();
    gcRootTypes_.clear();
    viewRootVarNames_.clear();
    viewRootTypes_.clear();
}

void CodeGenerator::registerParamTracking(const Param& p) {
    if (!p.type) return;
    std::string ptype = mapType(*p.type);
    // string 参数 → stringVarNames_
    if (ptype.find("aura_rt::GcString*") != std::string::npos)
        stringVarNames_.insert(p.name);
    // 接口参数 → valueTypeVarNames_（引用用 . 不是 ->）
    if (auto* nt = dynamic_cast<const NamedType*>(p.type.get()))
        if (interfaceNames_.count(nt->name))
            valueTypeVarNames_.insert(p.name);
    // 值类型 NamedType → valueTypeVarNames_（registeredTypes_ 非堆 / BuiltinRegistry 非堆）
    if (auto* nt = dynamic_cast<const NamedType*>(p.type.get()))
        if ((registeredTypes_.count(nt->name) && !registeredTypes_[nt->name])
            || (BuiltinRegistry::get().findType(nt->name) != nullptr
                && !BuiltinRegistry::get().isHeapType(nt->name)))
            valueTypeVarNames_.insert(p.name);
}

void CodeGenerator::registerRawParamTracking(const Param& p) {
    if (!p.type) return;
    std::string ptype = mapParamType(*p.type);
    if (isIfaceViewTypeName(ptype)) {
        valueTypeVarNames_.insert(p.name);
        viewRootVarNames_.insert(p.name);
        viewRootTypes_[p.name] = "decltype(" + safeName(p.name) + "_raw)";
    } else if (auto* nt = dynamic_cast<const NamedType*>(p.type.get())) {
        if (interfaceNames_.count(nt->name))
            valueTypeVarNames_.insert(p.name);
    }
    if (isGcPointerType(ptype)) {
        std::string varName = safeName(p.name);
        gcRootVarNames_.insert(varName);
        gcRootTypes_[varName] = "decltype(" + varName + "_raw)";
    }
}

void CodeGenerator::genFunDecl(std::ostream& h, std::ostream& cpp,
                                const FunDecl& decl, bool declarationsOnly) {
    bool isCoro = coroutineFunctions_.count(decl.name);
    currentFunctionIsCoroutine_ = isCoro;
    // Bug 2-B: 函数入口重置闭包协程标记——genFunExpr 在 return 语句中不会被
    // genLetStmt 消费 lastClosureIsCoro_，残留会污染下一个函数的 let 绑定
    lastClosureIsCoro_ = false;

    std::vector<std::string> tparams = collectFunTParams(decl);
    currentTParams_ = tparams;

    std::string tprefix;
    if (!tparams.empty()) {
        tprefix = "template<";
        for (size_t i = 0; i < tparams.size(); ++i) {
            if (i > 0) tprefix += ", ";
            tprefix += "typename " + tparams[i];
        }
        tprefix += ">\n";
    }

    std::string sig = funSignature(decl, tparams);

    // 注册回调类型参数（必须在 declarationsOnly return 之前，确保其他函数闭包体可见）
    fnInterfaceParams_.erase(decl.name);
    fnCallbackParams_.erase(decl.name);
    for (size_t i = 0; i < decl.params.size(); ++i) {
        if (auto* nt = decl.params[i].type
                ? dynamic_cast<const NamedType*>(decl.params[i].type.get())
                : nullptr) {
            if (interfaceNames_.count(nt->name)) {
                fnInterfaceParams_[decl.name].push_back({i, nt->name});
            }
        }
        // 仅当函数本身是模板时，才注册回调包装（需要模板参数 T, U 在作用域内）
        if (!tparams.empty() && decl.params[i].type
            && dynamic_cast<const FunctionType*>(decl.params[i].type.get())) {
            auto ft = dynamic_cast<const FunctionType*>(decl.params[i].type.get());
            fnCallbackParams_[decl.name].push_back({i, mapType(*ft)});
        }
    }

    // C5.1: 收集函数默认参数表（调用点补实参用；长度 = 形参总数，无默认值为 nullptr）
    // 必须在 declarationsOnly（A 遍）收集：调用点函数可能先于定义生成（如 main 在前）
    {
        std::vector<const ASTNode*> defaults(decl.params.size(), nullptr);
        bool any = false;
        for (size_t i = 0; i < decl.params.size(); ++i)
            if (decl.params[i].defaultExpr) { defaults[i] = decl.params[i].defaultExpr.get(); any = true; }
        if (any) fnDefaultArgs_[decl.name] = std::move(defaults);
    }

    // declarationsOnly 模式：仅输出前向声明
    if (declarationsOnly) {
        h << tprefix << sig << ";\n";
        return;
    }

    valueTypeVarNames_.clear();
    stringVarNames_.clear();
    gcRootVarNames_.clear();
    gcRootTypes_.clear();
    viewRootVarNames_.clear();
    viewRootTypes_.clear();
    for (auto& p : decl.params) {
        registerParamTracking(p);
        registerRawParamTracking(p);
    }

    // 模板函数或 auto 返回（泛型闭包）→ 体放入头文件（跨模块可见）
    bool needsHeader = !tparams.empty();
    if (!needsHeader && decl.returnType) {
        // plan12: 泛型闭包返回 → auto → 需要 .h
        std::set<std::string> retGen;
        if (auto* ft = dynamic_cast<const FunctionType*>(decl.returnType.get())) {
            collectTParams(*ft, retGen);
            needsHeader = !retGen.empty();
        } else if (auto* nt = dynamic_cast<const NamedType*>(decl.returnType.get())) {
            needsHeader = typeAliasTemplateParams_.count(nt->name) > 0;
        }
    }
    std::ostream& out = needsHeader
        ? static_cast<std::ostream&>(h)
        : static_cast<std::ostream&>(cpp);

    out << tprefix << sig << " {\n";
    // C3.2: 跟踪当前函数返回 Optional<T> 的元素类型（none() 直转 make_none<T> 用）
    currentReturnElem_ = optionalElemOf(decl.returnType.get());
    // Bug B 修复：函数体入口为堆类型参数生成 GcRootHandle 包装
    // 签名形如 `Tree<T>* node_raw`，此处生成 `GcRootHandle<decltype(node_raw)> node(node_raw);`
    // 用 decltype 而非显式 ptype，避免泛型闭包（compose(auto transforms)）中
    // 源类型含未绑定模板参数 T 而无法在函数作用域解析的问题
    // P1：接口视图参数同样处理——ViewRoot 包裹（self 跨 GC 保护）
    for (auto& p : decl.params) {
        if (!p.type) continue;
        std::string ptype = mapParamType(*p.type);
        std::string varName = safeName(p.name);
        if (isGcPointerType(ptype)) {
            out << "  aura_rt::GcRootHandle<decltype(" << varName << "_raw)> "
                << varName << "(" << varName << "_raw);\n";
        } else if (isIfaceViewTypeName(ptype)) {
            out << "  aura_rt::ViewRoot<decltype(" << varName << "_raw)> "
                << varName << "(" << varName << "_raw);\n";
        }
    }
    if (decl.body) genBlock(out, *decl.body, isCoro);
    bool lastIsReturn = decl.body && !decl.body->stmts.empty()
        && dynamic_cast<const ReturnStmt*>(decl.body->stmts.back().get());
    // 协程函数末尾无 return 时补 co_return，确保 C++20 将其识别为协程
    if (isCoro) {
        if (!lastIsReturn)
            out << "  co_return;\n";
    } else if (!lastIsReturn && decl.returnType
               && mapType(*decl.returnType) == "aura_rt::NoneType") {
        // 显式 `-> None` 的普通函数返回 NoneType（非 void），体末尾无 return 时
        // GCC 对"非 void 函数走到末尾"的未定义行为路径插入 ud2 非法指令
        // → 补 return aura_rt::NoneType{}; 使函数体合法
        out << "  return aura_rt::NoneType{};\n";
    }
    out << "}\n\n";
    clearVarTrackingState();
    currentReturnElem_.clear();
}

std::string CodeGenerator::funSignature(const FunDecl& decl,
                                         const std::vector<std::string>& tparams) {
    bool isCoro = coroutineFunctions_.count(decl.name);
    std::ostringstream sig;

    std::string retType = decl.returnType ? mapType(*decl.returnType) : "void";

    // plan12: 泛型闭包返回 → auto
    bool isGenClosureRet = tparams.empty() && decl.returnType;
    if (isGenClosureRet) {
        std::set<std::string> check;
        collectTParams(*decl.returnType, check);
        bool isGenericFT = (dynamic_cast<const FunctionType*>(decl.returnType.get()) && !check.empty());
        bool isAlias = false;
        if (auto* nt = dynamic_cast<const NamedType*>(decl.returnType.get()))
            isAlias = typeAliasTemplateParams_.count(nt->name) > 0;
        isGenClosureRet = isGenericFT || isAlias;
    }

    if (isGenClosureRet)
        retType = "auto";

    currentReturnCppType_ = retType;
    // P3b：填充当前函数返回"含堆联合"的变体 C++ 类型列表（供 genReturnStmt 隐式装箱）
    currentReturnVariantCppTypes_.clear();
    if (decl.returnType && decl.returnType->inferredType) {
        if (auto* u = dynamic_cast<const UnionSemType*>(decl.returnType->inferredType)) {
            std::vector<std::string> cppTypes;
            bool hasHeap = false;
            for (auto& v : u->variants) {
                cppTypes.push_back(v ? mapSemType(*v) : "void");
                if (v && isHeapSemType(v.get())) hasHeap = true;
            }
            if (hasHeap) currentReturnVariantCppTypes_ = std::move(cppTypes);
        }
    }

    std::string fn = safeName(decl.name);
    // 所有协程：NoneType 返回 → void（task<void> 有 return_void()，task<NoneType> 没有）
    if (retType == "aura_rt::NoneType") retType = "void";
    if (fn == "main") fn = "aura_main";

    sig << (isCoro ? "aura_rt::task<" + retType + ">" : retType);
    sig << " " << fn << "(";
    for (size_t i = 0; i < decl.params.size(); ++i) {
        if (i > 0) sig << ", ";
        if (isGenClosureRet && decl.params[i].type) {
            std::set<std::string> pGen;
            collectTParams(*decl.params[i].type, pGen);
            if (!pGen.empty())
                sig << "auto";
            else
                sig << mapType(*decl.params[i].type);
        } else {
            sig << (decl.params[i].type ? mapParamType(*decl.params[i].type) : "auto");
        }
        sig << " " << safeName(decl.params[i].name);

        // 堆类型参数加 _raw 后缀，函数体开头会用 GcRootHandle 包装为同名变量
        // （防止函数体内 alloc 触发 GC 移动对象后参数悬垂）
        // P1：接口视图参数同样加 _raw，函数体开头用 ViewRoot 包裹（self 跨 GC 保护）
        if (decl.params[i].type) {
            std::string ptype = mapParamType(*decl.params[i].type);
            if (isGcPointerType(ptype) || isIfaceViewTypeName(ptype)) {
                sig << "_raw";
            }
        }
    }
    sig << ")";
    return sig.str();
}

void CodeGenerator::genMethodDecl(std::ostream& h, std::ostream& cpp,
                                   const MethodDecl& decl,
                                   bool declarationsOnly) {
    // C5.3: 收集方法默认参数表（调用点补实参用；键 = ReceiverType 或 ReceiverType.methodName）
    // 必须在 declarationsOnly（A 遍）收集：调用点函数可能先于方法定义生成（如 main 在前）
    {
        std::vector<const ASTNode*> defaults(decl.params.size(), nullptr);
        bool any = false;
        for (size_t i = 0; i < decl.params.size(); ++i)
            if (decl.params[i].defaultExpr) { defaults[i] = decl.params[i].defaultExpr.get(); any = true; }
        if (any) methodDefaultArgs_[decl.isConstructor
            ? decl.receiverType                                   // ctor 键 = "ReceiverType"
            : decl.receiverType + "." + decl.name] = std::move(defaults);
    }
    if (decl.isConstructor) {
        if (declarationsOnly) {
            // A 遍：生成 ctor 前向声明（调用点可能先于定义生成，如 main 在前调用 Counter()）
            std::vector<std::string> tparams = collectMethodTParams(decl);
            std::string tprefix;
            if (!tparams.empty()) {
                tprefix = "template<";
                for (size_t i = 0; i < tparams.size(); ++i) {
                    if (i > 0) tprefix += ", ";
                    tprefix += "typename " + tparams[i];
                }
                tprefix += ">\n";
            }
            h << tprefix << constructorSignature(decl, tparams) << ";\n";
        } else {
            genConstructor(cpp, decl);  // 构造函数体只在定义阶段生成
        }
        return;
    }
    if (declarationsOnly) return;  // 方法声明已在 struct 内部，无需重复

    bool isCoro = coroutineFunctions_.count(decl.name);
    currentFunctionIsCoroutine_ = isCoro;
    // Bug 2-B: 方法入口同样重置闭包协程标记（防跨函数泄漏，见 genFunDecl）
    lastClosureIsCoro_ = false;

    std::vector<std::string> tparams = collectMethodTParams(decl);
    currentTParams_ = tparams;

    std::string tprefix;
    if (!tparams.empty()) {
        tprefix = "template<";
        for (size_t i = 0; i < tparams.size(); ++i) {
            if (i > 0) tprefix += ", ";
            tprefix += "typename " + tparams[i];
        }
        tprefix += ">\n";
    }

    // 构建带模板参数的接收者类型名
    std::string recvFullType = decl.receiverType;
    if (!tparams.empty()) {
        recvFullType += "<";
        for (size_t i = 0; i < tparams.size(); ++i) {
            if (i > 0) recvFullType += ", ";
            recvFullType += tparams[i];
        }
        recvFullType += ">";
    }

    clearVarTrackingState();

    // 跟踪方法接收者 self 的类型
    if (!decl.receiverTypeArgs.empty() || !registeredTypes_.count(decl.receiverType) || registeredTypes_[decl.receiverType])
        ; // self 通常为指针类型
    else
        valueTypeVarNames_.insert(decl.receiverName);

    for (auto& p : decl.params) {
        registerParamTracking(p);
        registerRawParamTracking(p);
    }

    std::string retType = decl.returnType ? mapType(*decl.returnType) : "void";

    // 存储 C++ 返回类型，供 genReturnStmt 生成正确 RecordExpr
    currentReturnCppType_ = retType;
    // P3b：填充当前方法返回"含堆联合"的变体 C++ 类型列表（供 genReturnStmt 隐式装箱）
    currentReturnVariantCppTypes_.clear();
    if (decl.returnType && decl.returnType->inferredType) {
        if (auto* u = dynamic_cast<const UnionSemType*>(decl.returnType->inferredType)) {
            std::vector<std::string> cppTypes;
            bool hasHeap = false;
            for (auto& v : u->variants) {
                cppTypes.push_back(v ? mapSemType(*v) : "void");
                if (v && isHeapSemType(v.get())) hasHeap = true;
            }
            if (hasHeap) currentReturnVariantCppTypes_ = std::move(cppTypes);
        }
    }

    // 检测方法名与 receiver 的字段名是否冲突，冲突时加 _fun 后缀
    std::string methodCppName = safeName(decl.name);
    auto fnIt = structFieldNames_.find(decl.receiverType);
    if (fnIt != structFieldNames_.end() && fnIt->second.count(methodCppName))
        methodCppName += "_fun";

    std::string sig = retType + " " + recvFullType + "::" + methodCppName + "(";
    for (size_t i = 0; i < decl.params.size(); ++i) {
        if (i > 0) sig += ", ";
        sig += (decl.params[i].type ? mapParamType(*decl.params[i].type) : "auto")
             + " " + safeName(decl.params[i].name);
        // Bug B 同步修复：堆类型参数加 _raw 后缀，方法体入口用 GcRootHandle 包装
        // P1：接口视图参数同样加 _raw，方法体入口用 ViewRoot 包裹（self 跨 GC 保护）
        if (decl.params[i].type) {
            std::string ptype = mapParamType(*decl.params[i].type);
            if (isGcPointerType(ptype) || isIfaceViewTypeName(ptype)) {
                sig += "_raw";
            }
        }
    }
    sig += ")";

    // 模板方法：体放入头文件（跨模块可见）
    std::ostream& out = tparams.empty()
        ? static_cast<std::ostream&>(cpp)
        : static_cast<std::ostream&>(h);

    out << tprefix << sig << " {\n";
    currentReceiverName_ = decl.receiverName;
    // C3.2: 跟踪当前方法返回 Optional<T> 的元素类型（none() 直转 make_none<T> 用）
    currentReturnElem_ = optionalElemOf(decl.returnType.get());
    // Bug B 同步修复：方法体入口为堆类型参数生成 GcRootHandle 包装
    // 签名形如 `Tree<T>::map(Tree<U>* node_raw)`，此处生成 `GcRootHandle<decltype(node_raw)> node(node_raw);`
    // 用 decltype 避免泛型方法中未绑定模板参数无法解析的问题
    // P1：接口视图参数 → ViewRoot 包裹（视图含 self 裸指针，compact 不重写栈上指针，
    //     必须注册 self 为 GcRootHandle，GC 后 get() 重建视图取最新 self）
    for (auto& p : decl.params) {
        if (!p.type) continue;
        std::string ptype = mapParamType(*p.type);
        std::string varName = safeName(p.name);
        if (isGcPointerType(ptype)) {
            out << "  aura_rt::GcRootHandle<decltype(" << varName << "_raw)> "
                << varName << "(" << varName << "_raw);\n";
        } else if (isIfaceViewTypeName(ptype)) {
            out << "  aura_rt::ViewRoot<decltype(" << varName << "_raw)> "
                << varName << "(" << varName << "_raw);\n";
        }
    }
    if (decl.body) genBlock(out, *decl.body, isCoro);
    currentReceiverName_.clear();
    out << "}\n\n";
    clearVarTrackingState();
    currentReturnElem_.clear();
}

// 构造函数 ============================================================
// 构造函数生成（plan §4.3）
// ============================================================

void CodeGenerator::genConstructor(std::ostream& cpp, const MethodDecl& decl) {
    // 默认参数表已在 genMethodDecl A 遍收集（methodDefaultArgs_[receiverType]）
    // 提取泛型类型参数（统一用 collectMethodTParams）
    std::vector<std::string> tparams = collectMethodTParams(decl);
    currentTParams_ = tparams;

    // 构建模板前缀
    std::string tprefix;
    if (!tparams.empty()) {
        tprefix = "template<";
        for (size_t i = 0; i < tparams.size(); ++i) {
            if (i > 0) tprefix += ", ";
            tprefix += "typename " + tparams[i];
        }
        tprefix += ">\n";
    }

    // 构建带模板参数的完整类型名（如 Pair<A, B>）
    std::string fullType = decl.receiverType;
    if (!tparams.empty()) {
        fullType += "<";
        for (size_t i = 0; i < tparams.size(); ++i) {
            if (i > 0) fullType += ", ";
            fullType += tparams[i];
        }
        fullType += ">";
    }

    std::string sig = constructorSignature(decl, tparams);

    // 模板构造函数：体放入头文件（跨模块可见）
    std::ostream& out = tparams.empty()
        ? static_cast<std::ostream&>(cpp)
        : *headerStream_;

    out << tprefix << sig << " {\n";
    out << "  " << fullType << "* " << safeName(decl.receiverName) << " = aura_rt::gc_alloc<"
        << fullType << ">(&" << fullType << "::_desc);\n";
    if (decl.body) genBlock(out, *decl.body, false);
    out << "  return " << safeName(decl.receiverName) << ";\n";
    out << "}\n\n";
    clearVarTrackingState();
    currentTParams_.clear();
}

std::string CodeGenerator::constructorSignature(const MethodDecl& decl,
                                                  const std::vector<std::string>& tparams) {
    std::ostringstream sig;
    std::string fullType = decl.receiverType;
    if (!tparams.empty()) {
        fullType += "<";
        for (size_t i = 0; i < tparams.size(); ++i) {
            if (i > 0) fullType += ", ";
            fullType += tparams[i];
        }
        fullType += ">";
    }
    sig << fullType << "* " << decl.receiverType << "_ctor(";
    for (size_t i = 0; i < decl.params.size(); ++i) {
        if (i > 0) sig << ", ";
        sig << (decl.params[i].type ? mapType(*decl.params[i].type) : "auto")
            << " " << safeName(decl.params[i].name);
    }
    sig << ")";
    return sig.str();
}

// ============================================================
// 主入口（plan §5）
// ============================================================

void CodeGenerator::genMainEntry(std::ostream& cpp, const FunDecl& mainDecl,
                                    const std::string& nsName) {
    cpp << "\n// ============================================================\n";
    cpp << "// Aura 程序入口\n";
    cpp << "// ============================================================\n";
    cpp << "int main(int /*argc*/, char** /*argv*/) {\n";
    cpp << "  aura_rt::Io io;\n";
    std::string callPrefix = nsName.empty() ? "::aura_main" : nsName + "::aura_main";
    if (ioSync_) {
        // 同步模式：aura_main 返回 void，直接调用
        cpp << "  " << callPrefix << "(io);\n";
        cpp << "  return 0;\n";
    } else {
        // 异步模式：aura_main 返回 task<void>，走 run_event_loop
        cpp << "  auto t = " << callPrefix << "(io);\n";
        cpp << "  aura_rt::run_event_loop(t);\n";
        cpp << "  return 0;\n";
    }
    cpp << "}\n";
    (void)mainDecl;
}

// ============================================================
// 泛型模板参数收集
// ============================================================

void CodeGenerator::collectTParams(const TypeExpr& type, std::set<std::string>& out) const {
    if (auto* g = dynamic_cast<const GenericTypeRef*>(&type)) {
        out.insert(g->name);
        return;
    }
    if (auto* n = dynamic_cast<const NamedType*>(&type)) {
        for (auto& a : n->typeArgs)
            if (a) collectTParams(*a, out);
        // 若该名称是模板类型别名，添加其模板参数
        auto aliasIt = typeAliasTemplateParams_.find(n->name);
        if (aliasIt != typeAliasTemplateParams_.end()) {
            for (auto& tp : aliasIt->second) out.insert(tp);
            return;
        }
        // 排除内置类型（int/float/bool/string/None 不是泛型参数，无需 template<...>）
        if (n->name == "int" || n->name == "float" || n->name == "bool"
            || n->name == "string" || n->name == "None")
            return;

        // 无 typeArgs + 非注册类型 → 是泛型参数（如 Pair<A,B> 中的 A/B）
        if (n->typeArgs.empty() && !registeredTypes_.count(n->name)
            && !interfaceNames_.count(n->name)
            && !BuiltinRegistry::get().findType(n->name))
            out.insert(n->name);
        return;
    }
    if (auto* l = dynamic_cast<const ListType*>(&type)) {
        if (l->elementType) collectTParams(*l->elementType, out);
        return;
    }
    if (auto* r = dynamic_cast<const RecordType*>(&type)) {
        for (auto& f : r->fields)
            if (f.type) collectTParams(*f.type, out);
        return;
    }
    if (auto* u = dynamic_cast<const UnionType*>(&type)) {
        for (auto& v : u->types)
            if (v) collectTParams(*v, out);
        return;
    }
    if (auto* fn = dynamic_cast<const FunctionType*>(&type)) {
        for (auto& p : fn->paramTypes)
            if (p) collectTParams(*p, out);
        if (fn->returnType) collectTParams(*fn->returnType, out);
        return;
    }
}

std::vector<std::string> CodeGenerator::collectFunTParams(const FunDecl& decl) const {
    std::set<std::string> names;

    // plan12 统一方案：若返回泛型闭包，外层函数不模板化，泛型由闭包自身声明
    if (decl.returnType) {
        std::set<std::string> retGen;
        if (auto* ft = dynamic_cast<const FunctionType*>(decl.returnType.get())) {
            collectTParams(*ft, retGen);
            if (!retGen.empty()) return {}; // 泛型闭包 → 不模板化
        } else if (auto* nt = dynamic_cast<const NamedType*>(decl.returnType.get())) {
            // 仅函数式类型别名（如 Pipeline<T> = fun(T)->T，非堆类型）跳过模板化
            // 堆类型（如 Tree<T>）保持模板参数
            auto it = registeredTypes_.find(nt->name);
            if (typeAliasTemplateParams_.count(nt->name)
                && it != registeredTypes_.end() && !it->second)  // registered as non-heap
                return {};
        }
    }

    for (auto& p : decl.params)
        if (p.type) collectTParams(*p.type, names);
    if (decl.returnType) collectTParams(*decl.returnType, names);
    return {names.begin(), names.end()};
}

std::vector<std::string> CodeGenerator::collectMethodTParams(const MethodDecl& decl) const {
    std::set<std::string> names;
    // 优先从 receiverTypeArgs（如 Stack<T> 中的 T）
    for (auto& ta : decl.receiverTypeArgs)
        names.insert(ta);
    // 从参数类型中收集
    for (auto& p : decl.params)
        if (p.type) collectTParams(*p.type, names);
    if (decl.returnType) collectTParams(*decl.returnType, names);
    return {names.begin(), names.end()};
}

} // namespace Aura
