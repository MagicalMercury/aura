#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"

namespace Aura {

// ============================================================
// 类型名注册
// ============================================================

void CodeGenerator::registerTypeName(const std::string& auraName, bool isHeap) {
    registeredTypes_[auraName] = isHeap;
}

bool CodeGenerator::isValueType(const std::string& auraName) const {
    // 先查 BuiltinRegistry
    if (auto* ti = Aura::BuiltinRegistry::get().findType(auraName))
        return !ti->isHeap;
    auto it = registeredTypes_.find(auraName);
    if (it == registeredTypes_.end()) return false;
    return !it->second;
}

bool CodeGenerator::isHeapType(const std::string& auraName) const {
    // 先查 BuiltinRegistry
    if (auto* ti = Aura::BuiltinRegistry::get().findType(auraName))
        return ti->isHeap;
    auto it = registeredTypes_.find(auraName);
    if (it == registeredTypes_.end()) return false;
    return it->second;
}

bool CodeGenerator::isGcPointerType(const std::string& cppType) const {
    if (cppType.empty() || cppType.back() != '*') return false;
    if (cppType == "int32_t*" || cppType == "double*" || cppType == "bool*")
        return false;
    if (cppType == "const char*") return false;
    if (cppType == "auto") return false;
    return true;
}

// P1：判定 C++ 类型名是否为接口视图类型
//   - 内置 Iterator<T>（aura_rt::Iterator<...>）
//   - 用户接口名 / 内置接口名（Stringer / Comparable<...> / Greetable 等）
// 视图是值类型（{ 方法Fn, self }），非 GC 指针；record 字段含视图时注册 self 子偏移
bool CodeGenerator::isIfaceViewTypeName(const std::string& cppType) const {
    // 内置接口的 C++ 形态带 aura_rt:: 命名空间（定义在 runtime 公共头）：
    //   - Iterator<T>   → aura_rt::Iterator<T>（builtin/iterator.h）
    //   - Stringer      → aura_rt::Stringer（builtin/interfaces.h）
    //   - Comparable<T> → aura_rt::Comparable<T>（builtin/interfaces.h）
    // bug-85（方案 B3）：三者统一，均以 aura_rt:: 前缀 + 可选 "<" 判定。
    if (cppType.rfind("aura_rt::Iterator<", 0) == 0) return true;
    // ⚠️ 长度常量必须 = 前缀串的【实际字符数】（否则越界/误判）：
    //   "aura_rt::Stringer"   = 17（aura_rt:: 9 + Stringer 8）
    //   "aura_rt::Comparable" = 19（aura_rt:: 9 + Comparable 10）
    //   裸名（size == 前缀长）→ 命中；带实参（如 aura_rt::Comparable<Point>）
    //   → 下一字符必为 '<' → 命中；否则（如 aura_rt::StringerXxx）→ 不命中。
    // bug-85 修复（2026-09-18）：原写 18/20（各多 1），使裸名走 `cppType[N]` 越界
    // → libstdc++ _GLIBCXX_ASSERTIONS 下 basic_string::operator[] 断言 abort
    //（现象：单测在 InterfaceDefaultMethod* 处进程崩溃，非普通失败）。
    if (cppType.rfind("aura_rt::Stringer", 0) == 0
        && (cppType.size() == 17 || cppType[17] == '<'))
        return true;
    if (cppType.rfind("aura_rt::Comparable", 0) == 0
        && (cppType.size() == 19 || cppType[19] == '<'))
        return true;
    for (auto& in : interfaceNames_) {
        if (cppType == in || cppType.rfind(in + "<", 0) == 0) return true;
    }
    // 内置接口（interfaces.aurai：Stringer/Comparable）不在 interfaceNames_（仅 program.decls 收集）
    for (auto& i : BuiltinRegistry::get().auraiInterfaces()) {
        const std::string& in = i->name;
        if (in == "Iterator" || in == "Stringer" || in == "Comparable") continue;  // 已在上方处理
        if (cppType == in || cppType.rfind(in + "<", 0) == 0) return true;
    }
    return false;
}

// ============================================================
// 类型映射（plan §4.1）
// ============================================================

std::string CodeGenerator::mapType(const TypeExpr& type) {
    if (auto* n = dynamic_cast<const NamedType*>(&type)) {
        // 泛型接口适配器上下文（ifaceTypeMap_ 非空）：裸泛型形参名（如 Optional<T> 的 T，
        // TypeParser 将其解析为 NamedType）→ 替换为具体实参类型（tmap 值为完整 C++ 类型，
        // 如 "Point*"）。仅裸名（无 typeArgs）短路；Name<T> 走下方实例化路径，内部实参经
        // mapType 递归时同样查 ifaceTypeMap_ → 容器/复合类型内嵌 T 被递归代换
        if (n->typeArgs.empty()) {
            auto it = ifaceTypeMap_.find(n->name);
            if (it != ifaceTypeMap_.end()) return it->second;
            // M3：默认参数闭包物化——调用点补默认实参时裸泛型名（如闭包参数 x: T
            // 的 T，TypeParser 解析为 NamedType）已物化为具体类型 → 直接返回
            auto mit = defaultArgMaterializedTypes_.find(n->name);
            if (mit != defaultArgMaterializedTypes_.end()) return mit->second;
        }
        // 有命名空间前缀时，先查全限定名（如 "sync.Channel"）是否在 BuiltinRegistry
        // 命中则直接取 cppType 并注入模板参数，避免后续 mapNamedType 只解析短名 "Channel"
        if (!n->namespacePrefix.empty()) {
            std::string fullName;
            for (auto& ns : n->namespacePrefix) fullName += ns + ".";
            fullName += n->name;
            if (auto* ti = BuiltinRegistry::get().findType(fullName)) {
                std::string cpp = ti->cppType;  // e.g. "aura_rt::ThreadChannel*"
                if (!n->typeArgs.empty()) {
                    bool hasStar = !cpp.empty() && cpp.back() == '*';
                    if (hasStar) cpp.pop_back();
                    cpp += "<";
                    for (size_t i = 0; i < n->typeArgs.size(); ++i) {
                        if (i > 0) cpp += ", ";
                        cpp += n->typeArgs[i] ? mapType(*n->typeArgs[i]) : "???";
                    }
                    cpp += ">";
                    if (hasStar) cpp += "*";
                }
                return cpp;
            }
        }

        std::string base = mapNamedType(n->name);
        if (!n->typeArgs.empty()) {
            bool hadStar = base.size() > 1 && base.back() == '*';
            if (hadStar) base.pop_back();
            base += "<";
            for (size_t i = 0; i < n->typeArgs.size(); ++i) {
                if (i > 0) base += ", ";
                base += n->typeArgs[i] ? mapType(*n->typeArgs[i]) : "???";
            }
            base += ">";
            if (hadStar) base += "*";
        }
        // 命名空间前缀：math.Pair → math::Pair
        // 注意：sync 是伪模块名（sync.Mutex/RWMutex/Once），
        // BuiltinRegistry 已注册为 aura_rt::Mutex* 等，无需加 sync:: 前缀
        if (!n->namespacePrefix.empty()) {
            // sync 伪命名空间：跳过，不加前缀
            bool isSyncBuiltin = (n->namespacePrefix.size() == 1
                                  && n->namespacePrefix[0] == "sync"
                                  && base.find("aura_rt::") == 0);
            if (!isSyncBuiltin) {
                std::string prefix;
                for (auto& ns : n->namespacePrefix)
                    prefix += ns + "::";
                // 跨模块类型都是堆指针，若 base 不是以 * 结尾则追加
                if (!base.empty() && base.back() != '*')
                    base += "*";
                if (base.find("aura_rt::") == 0) {
                    base.insert(std::string("aura_rt::").size(), prefix);
                } else {
                    base = prefix + base;
                }
            }
        }
        return base;
    }
    if (auto* tp = dynamic_cast<const TupleTypeExpr*>(&type)) {
        // 元组类型注解：现场合成 TupleN<elem...>*
        std::string cn = "aura_rt::Tuple" + std::to_string(tp->elementTypes.size()) + "<";
        for (size_t i = 0; i < tp->elementTypes.size(); ++i) {
            if (i > 0) cn += ", ";
            cn += tp->elementTypes[i] ? mapType(*tp->elementTypes[i]) : "void";
        }
        return cn + ">*";
    }
    if (auto* g = dynamic_cast<const GenericTypeRef*>(&type))
        return mapGenericRef(*g);
    if (auto* l = dynamic_cast<const ListType*>(&type)) {
        auto elem = l->elementType ? mapType(*l->elementType) : "???";
        return "aura_rt::Array<" + elem + ">*";
    }
    if (dynamic_cast<const RecordType*>(&type)) {
        return "/* inline record */ aura_rt::GcObject*";
    }
    if (auto* u = dynamic_cast<const UnionType*>(&type)) {
        // P3a：`T | None`（恰 2 变体、另一为堆类型）→ aura_rt::Optional<elem>*（与 Sema 折叠一致）
        if (u->types.size() == 2) {
            auto* na = dynamic_cast<const NamedType*>(u->types[0].get());
            auto* nb = dynamic_cast<const NamedType*>(u->types[1].get());
            auto isNoneNamed = [](const NamedType* n) {
                return n && n->name == "None" && n->typeArgs.empty() && n->namespacePrefix.empty();
            };
            bool aIsNone = isNoneNamed(na);
            bool bIsNone = isNoneNamed(nb);
            if (aIsNone != bIsNone) {
                const auto& other = aIsNone ? u->types[1] : u->types[0];
                // 堆判定与 Sema unionVariantGcUnsafe 对齐：类型标注处无 SemType
                // （如 [int] | None 的 [int] 是 ListType），按 C++ 名回退（指针 = 堆）
                bool otherHeap = false;
                // 内置泛型模板（Optional/Iterator/channel 等，BuiltinPrim::Other）：
                // Sema 将它们物化为 GenericSemType，unionVariantGcUnsafe 恒 false →
                // resolveType 不折叠，保持 UnionSemType（Variant 路径）。CodeGen mapType
                // 必须一致，否则声明类型与 genUnionBoxing 初始化类型不匹配（u5/u12/u62）
                auto isBuiltinGenericNamed = [](const TypeExpr* t) {
                    if (auto* n = dynamic_cast<const NamedType*>(t)) {
                        auto* ti = BuiltinRegistry::get().findType(n->name);
                        return ti && ti->primKind == BuiltinPrim::Other;
                    }
                    return false;
                };
                if (other && other->inferredType) {
                    otherHeap = !dynamic_cast<const GenericSemType*>(other->inferredType)
                             && isHeapSemType(other->inferredType);
                } else if (other && isBuiltinGenericNamed(other.get())) {
                    otherHeap = false;   // 内置泛型变体不折叠
                } else if (other) {
                    std::string cpp = mapType(*other);
                    otherHeap = !cpp.empty() && cpp.back() == '*';
                }
                if (otherHeap)
                    return "aura_rt::Optional<" + mapType(*other) + ">*";
            }
        }
        // P3b：含堆变体 → aura_rt::Variant<...>*（GC 堆封装）；全值 → aura_rt::ValueVariant<...>
        // （feature-05：全值联合弃用 std::variant → ValueVariant 值语义，static_assert
        //  编译期拒绝指针/接口视图变体误入值语义路径）
        // #42（bug-60 修正）：保守判堆的"未注册裸泛型名"检测——判据对象是 TypeExpr
        // 的 Aura 名而非 mapType 的 C++ 产物（"int32_t" 等内置 C++ 名不在以 Aura 名
        // 为 key 的 BuiltinRegistry 中：int→"int32_t" 查 findType("int32_t") 落空 →
        // 误判未注册 → 全值 Union（int|None）被误判堆 Variant → 声明/装箱形态分裂
        // 坏 C++）。Aura 名已注册（BuiltinRegistry / registeredTypes_ / interfaceNames_
        // / aurai 接口）即具体类型不判裸；错误类型名同样保守判堆（无害）。
        auto isBareAuraName = [this](const std::string& nm) -> bool {
            if (nm.empty()) return false;
            if (registeredTypes_.count(nm)) return false;
            if (interfaceNames_.contains(nm)) return false;
            if (BuiltinRegistry::get().findType(nm)) return false;
            for (auto& ai : BuiltinRegistry::get().auraiInterfaces())
                if (ai->name == nm) return false;
            return true;
        };
        bool hasHeap = false;
        for (auto& v : u->types) {
            if (!v) continue;
            bool heap = false;
            if (v->inferredType) {
                // 视图变体（Iterator/接口）也需堆 Variant 封装（self 子偏移扫描）
                heap = isUnionHeapVariant(v->inferredType);
            } else {
                // 无 SemType（如类型声明处）：按 C++ 名回退判断（指针类型 = 堆）
                std::string cpp = mapType(*v);
                heap = !cpp.empty() && cpp.back() == '*';
                // #42（bug-60 修正）：未绑定泛型变体（mapType 对未注册 Aura 名原样返回
                // 裸标识符的 <T>/T）→ 保守判堆（与 mapSemType 的 isHeapSemType
                // (GenericSemType)=true 对齐，split-brain 消除）。否则生成 by-value
                // std::variant<int32_t,T> → genParamBoxing 不命中（不认 std::variant）→
                // 调用点不装箱坏 C++；且 T 实例化为堆类型时 std::variant 内指针不可被
                // GC 追踪。判据用 TypeExpr 的 Aura 名（NamedType.name / GenericTypeRef），
                // 不用 mapType 的 C++ 产物——具体值类型变体（int→"int32_t"）Aura 名已
                // 注册不判裸（bug-60：此前 C++ 名查 Aura 注册表落空误伤全值 Union）。
                if (!heap && v) {
                    bool bareGeneric = false;
                    if (dynamic_cast<const GenericTypeRef*>(v.get())) {
                        bareGeneric = true;   // <T> 泛型引用即未绑定形态
                    } else if (auto* n = dynamic_cast<const NamedType*>(v.get())) {
                        bareGeneric = n->typeArgs.empty()
                                     && n->namespacePrefix.empty()
                                     && isBareAuraName(n->name);
                    }
                    if (bareGeneric) heap = true;
                }
                // 接口视图变体（值视图含 GC 指针 self，C++ 名非 * 结尾）→ 需
                // Variant 堆封装供 descForI 扫描（与 isUnionHeapVariant 对齐）。
                // 含内置 Iterator：其值视图含 self，B+W 后由 descForI is_iface_view_v
                // 子偏移 + ViewRoot 保护（2026-08-10 评估放开，见 plan）
                if (!heap && v) {
                    if (auto* n = dynamic_cast<const NamedType*>(v.get())) {
                        if (n->namespacePrefix.empty()) {
                            // 用户接口（interfaceNames_）+ 内置接口（auraiInterfaces，
                            // 如 Stringer/Comparable，与 isIfaceViewTypeName 判定一致）
                            bool isIface = interfaceNames_.contains(n->name);
                            if (!isIface)
                                for (auto& ai : BuiltinRegistry::get().auraiInterfaces())
                                    if (ai->name == n->name) { isIface = true; break; }
                            // 内置 Iterator：NamedType{name="Iterator"}（含 Iterator<int> 带 typeArgs）
                            if (isIface || n->name == "Iterator") heap = true;
                        }
                    }
                }
            }
            if (heap) { hasHeap = true; break; }
        }
        std::string result = hasHeap ? "aura_rt::Variant<" : "aura_rt::ValueVariant<";
        for (size_t i = 0; i < u->types.size(); ++i) {
            if (i > 0) result += ", ";
            result += u->types[i] ? mapType(*u->types[i]) : "???";
        }
        result += hasHeap ? ">*" : ">";
        return result;
    }
    if (auto* f = dynamic_cast<const FunctionType*>(&type)) {
        // feature-06（阶段 B）：fun(A)->R → aura_rt::CallableObj<R, A...>*（GC 堆
        // 对象，捕获槽 desc 追踪）。返回类型 None 保持 aura_rt::NoneType（与旧
        // std::function<aura_rt::NoneType()> 时代 lambda 返回约定一致，闭包体生成
        // 复用无分裂）。无参形态无尾缀参数；有参逐参 mapType。
        std::string sig = "aura_rt::CallableObj<";
        sig += f->returnType ? mapType(*f->returnType) : "void";
        for (size_t i = 0; i < f->paramTypes.size(); ++i) {
            sig += ", ";
            sig += f->paramTypes[i] ? mapType(*f->paramTypes[i]) : "/*?*/";
        }
        sig += ">*";
        return sig;
    }
    return "/* unknown_type */";
}

std::string CodeGenerator::optionalElemOf(const TypeExpr* retType) {
    // 从返回类型提取 Optional<T> 的 T（C++ 名）；非 Optional 返回空（C3.2）
    auto* nt = retType ? dynamic_cast<const NamedType*>(retType) : nullptr;
    if (nt && nt->name == "Optional" && !nt->typeArgs.empty())
        return mapType(*nt->typeArgs[0]);
    return "";
}

std::string CodeGenerator::optionalElemCppName(const SemType* optType) {
    // P1-1/A1/A2：从"Optional 语义类型"提取元素 C++ 名。
    // 显式 `Optional<T>` 注解经 DeclChecker 物化为 GenericSemType{name=="Optional",
    // resolvedName="aura_rt::Optional<T>"}（非 OptionalSemType）；mapSemType 对
    // GenericSemType 会追加 * 尾缀（Iterator 特判外），故 Generic 形态从 resolvedName
    // 直接取 <...> 内元素 C++ 名（含嵌套尖括号用末个 '>' 定位）。
    //
    // 元素 C++ 名可能缺失堆 record 的 *（Sema 的 resolvedName/cppNameOfTypeExpr 对
    // record 不加 *，而声明侧 mapType 对 record 追加 *）→ 统一补 * 与声明侧一致，
    // 否则 `let x: Optional<Point> = none()` 生成 Optional<Point>*（元素无 *）与
    // 声明 Optional<Point*>* 不匹配。嵌套容器（list/Iterator/嵌套 Optional）经
    // finalizeCppElem 递归补全（如 Optional<[Point]> 元素 Array<Point>* → Array<Point*>*）。

    if (auto* os = dynamic_cast<const OptionalSemType*>(optType)) {
        if (os->elementType
            && !dynamic_cast<const ErrorSemType*>(os->elementType.get())) {
            // 元素 GenericSemType（Sema elemTypeOf 经 semTypeFromCppName 还原）：
            //   resolvedName 即元素 C++ 名（可能缺 record 的 *）；其余走 mapSemType
            if (auto* ge = dynamic_cast<const GenericSemType*>(os->elementType.get())) {
                if (!ge->resolvedName.empty()) return finalizeCppElem(ge->resolvedName);
                // bug-64 配套：模板期裸泛型元素（OptionalSemType{GenericSemType{T}}，
                // T 为当前 C++ 模板参数）→ 返回模板参数名；不在 currentTParams_ 中则
                // 维持空（防御报错，不伪装具体类型）
                for (const auto& tp : currentTParams_)
                    if (tp == ge->name) return ge->name;
                return "";
            }
            return mapSemType(*os->elementType);
        }
        return "";
    }
    if (auto* gs = dynamic_cast<const GenericSemType*>(optType)) {
        if (gs->name == "Optional" && !gs->resolvedName.empty()) {
            auto lt = gs->resolvedName.find('<');
            auto rt = gs->resolvedName.rfind('>');
            if (lt != std::string::npos && rt != std::string::npos && rt > lt)
                return finalizeCppElem(gs->resolvedName.substr(lt + 1, rt - lt - 1));
        }
    }
    return "";
}

std::string CodeGenerator::finalizeCppElem(const std::string& elem) {
    // #2：Optional 元素 C++ 名递归补齐堆 record 的 '*'。
    // 从 resolvedName 提取的元素名可能缺失内嵌堆 record 的 '*'（Sema 的
    // cppNameOfTypeExpr 对 record 裸名不加 '*'、对 ListType/Iterator 内嵌 record
    // 也不加）→ 递归处理容器，叶子按既有 finalizeElem 判定：
    //   - 已带 '*' 指针（GcString* / 值类型指针）不动
    //   - 接口/Iterator 值视图不加 '*'
    //   - registeredTypes_ 命中的堆 record 补 '*'
    // 与 mapSemType 的容器语义一致（Optional<[Point]> / Optional<Iterator<Point>>
    // / Optional<Optional<Point>> 内嵌 record 都要补 '*'）。
    if (elem.empty()) return elem;
    static const std::string kArray = "aura_rt::Array<";
    static const std::string kIter  = "aura_rt::Iterator<";
    static const std::string kOpt   = "aura_rt::Optional<";
    static const std::string kChn   = "aura_rt::Channel<";
    static const std::string kTChn  = "aura_rt::ThreadChannel<";
    // 提取 "Prefix<Inner>" 的内层（末个 '>' 定位，兼容嵌套尖括号与尾缀 '*'
    // 如 "aura_rt::Array<Point>*" → "Point"）
    auto templateInner = [](const std::string& s, const std::string& prefix) -> std::string {
        auto rt = s.rfind('>');
        if (rt == std::string::npos || rt <= prefix.size()) return "";
        return s.substr(prefix.size(), rt - prefix.size());
    };
    // 容器：Array<X>*（list，堆指针）→ Array<finalize(X)>*
    if (elem.rfind(kArray, 0) == 0 && elem.back() == '*') {
        std::string inner = templateInner(elem, kArray);
        if (!inner.empty()) return kArray + finalizeCppElem(inner) + ">*";
        return elem;
    }
    // 容器：Iterator<X>（值视图，无尾 *；内嵌 record 元素仍要补 *，如 Iterator<Point>）
    if (elem.rfind(kIter, 0) == 0 && elem.back() == '>') {
        std::string inner = templateInner(elem, kIter);
        if (!inner.empty()) return kIter + finalizeCppElem(inner) + ">";
        return elem;
    }
    // 容器：Optional<X>[*]（嵌套 Optional 堆指针，内层补 * 后追加尾 *）
    if (elem.rfind(kOpt, 0) == 0) {
        std::string inner = templateInner(elem, kOpt);
        if (!inner.empty()) return kOpt + finalizeCppElem(inner) + ">*";
        return elem;
    }
    // 容器：Channel<X> / ThreadChannel<X>（channel<T> / sync.Channel<T> 堆指针，
    // 内嵌 record 元素同样补 *，如 Channel<Point> → Channel<Point*>*，与 mapType
    // 声明侧一致；修复 [channel<Point>] 列表元素缺 *）
    if (elem.rfind(kChn, 0) == 0) {
        std::string inner = templateInner(elem, kChn);
        if (!inner.empty()) return kChn + finalizeCppElem(inner) + ">*";
        return elem;
    }
    if (elem.rfind(kTChn, 0) == 0) {
        std::string inner = templateInner(elem, kTChn);
        if (!inner.empty()) return kTChn + finalizeCppElem(inner) + ">*";
        return elem;
    }
    // 叶子
    if (elem.back() == '*') return elem;           // 已是指针（GcString*/值类型指针）不动
    if (isIfaceViewTypeName(elem)) return elem;    // 接口/Iterator 值视图不加 *
    auto it = registeredTypes_.find(elem);
    if (it != registeredTypes_.end() && it->second) return elem + "*";  // 堆 record → 补 *
    return elem;
}

std::string CodeGenerator::mapNamedType(const std::string& name) {
    // 内置接口：C++ 形态为 16B 值视图（无 *），定义在 runtime 公共头。
    // 必须在 BuiltinRegistry 命中前特判（registry 存的是旧指针形态，如
    // "aura_rt::Iterator*"）。
    //   - Iterator   → builtin/iterator.h  （aura_rt::Iterator<T>）
    //   - Stringer   → builtin/interfaces.h（aura_rt::Stringer）
    //   - Comparable → builtin/interfaces.h（aura_rt::Comparable<T>）
    // bug-85（方案 B3）：Stringer/Comparable 与 Iterator 同源，不再是产物内裸名，
    // 故所有引用点统一带 aura_rt:: 命名空间。
    if (name == "Iterator") return "aura_rt::Iterator";
    if (name == "Stringer") return "aura_rt::Stringer";
    if (name == "Comparable") return "aura_rt::Comparable";

    // 先查 BuiltinRegistry（内置类型）
    if (auto* ti = Aura::BuiltinRegistry::get().findType(name)) {
        return ti->cppType;
    }

    // 用户定义类型
    auto it = registeredTypes_.find(name);
    if (it != registeredTypes_.end()) {
        // 堆对象 → 返回指针类型
        if (it->second) {
            return name + "*";
        }
        return name;
    }

    // 接口类型 → 保留原名（函数参数生成处做 const& 处理）
    if (interfaceNames_.contains(name)) {
        return name;
    }

    // 未知类型 → 保守返回
    return name;
}

std::string CodeGenerator::mapGenericRef(const GenericTypeRef& genericRef) {
    // 泛型接口适配器上下文（ifaceTypeMap_ 非空）：<T> 泛型引用 → 具体实参类型
    // （与 mapType NamedType 分支短路一致，覆盖 <T> 解析为 GenericTypeRef 的形态）
    auto it = ifaceTypeMap_.find(genericRef.name);
    if (it != ifaceTypeMap_.end()) return it->second;
    // M3：默认参数闭包物化——调用点补默认实参时 <T> 泛型引用（GenericTypeRef 形态）
    // 已物化为具体类型（如 useT(inc:<T>,...) 调用 useT(5,10) → T → int32_t）
    auto mit = defaultArgMaterializedTypes_.find(genericRef.name);
    if (mit != defaultArgMaterializedTypes_.end()) return mit->second;
    // <A>, <T> → 直接映射为 C++ 模板参数名（A, T）
    return genericRef.name;
}

std::string CodeGenerator::mapValueType(const TypeExpr& type) {
    // 值上下文：指针类型去掉 *
    std::string result = mapType(type);
    if (result.size() > 1 && result.back() == '*') {
        result.pop_back();
    }
    return result;
}

std::string CodeGenerator::mapParamType(const TypeExpr& type) {
    // P1：接口参数按值传视图（视图 { 方法Fn, self } 是值类型，可拷贝；
    //     self 指向的堆适配器由保守栈扫描保护，无需 const& 引用形态）
    return mapType(type);
}

// ============================================================
// SemType → C++ 类型映射
// ============================================================

std::string CodeGenerator::mapSemType(const SemType& semType) {
    if (auto* p = dynamic_cast<const PrimSemType*>(&semType)) {
        switch (p->kind) {
            case PrimSemType::Int:    return "int32_t";
            case PrimSemType::Float:  return "double";
            case PrimSemType::Bool:   return "bool";
            case PrimSemType::String: return "aura_rt::GcString*";
        }
    }
    if (dynamic_cast<const NoneSemType*>(&semType))
        return "aura_rt::NoneType";
    if (dynamic_cast<const ErrorSemType*>(&semType)) {
        // 兜底：error_type 泄漏到 CodeGen 会生成无效模板参数（如 Array</* error_type */>）→ C++ 错误。
        // 正常应被 Sema 的 containsErrorElement 拦截；此处报一次干净错误并返回安全占位，
        // 使 CodeGen 自身不崩溃、且 driver 检测到错误后不会调用 g++。
        if (!reportedErrorType_) {
            reportedErrorType_ = true;
            diag_.error(0, 0, "codegen: unresolved 'error_type' reached code generation (missing Sema check); add an explicit type annotation");
        }
        return "int32_t";
    }
    if (auto* l = dynamic_cast<const ListSemType*>(&semType)) {
        return "aura_rt::Array<" + mapSemType(*l->elementType) + ">*";
    }
    if (auto* o = dynamic_cast<const OptionalSemType*>(&semType)) {
        // Optional<T> → aura_rt::Optional<T>*（堆对象指针）
        std::string elem = o->elementType ? mapSemType(*o->elementType) : "void";
        return "aura_rt::Optional<" + elem + ">*";
    }
    if (auto* u = dynamic_cast<const UnionSemType*>(&semType)) {
        // P3a：`T | None`（含堆）折叠 → Optional（与 resolveType 一致，顺序无关）
        if (u->variants.size() == 2) {
            const SemType* noneV = nullptr;
            const SemType* otherV = nullptr;
            for (auto& v : u->variants) {
                if (!v) continue;
                if (dynamic_cast<const NoneSemType*>(v.get())) noneV = v.get();
                else otherV = v.get();
            }
            // 与 resolveType 的 unionVariantGcUnsafe 对齐：GenericSemType（内置泛型
            // 物化）与 InterfaceSemType（接口视图，P1-2 Option B）不折叠——Sema 对
            // `Optional<int>|None` / `Stringer|None` 保持 UnionSemType，mapSemType 须
            // 一致（Variant 路径），否则与 mapType 声明类型不匹配
            bool otherFold = otherV
                && !dynamic_cast<const GenericSemType*>(otherV)
                && !dynamic_cast<const InterfaceSemType*>(otherV)
                && isHeapSemType(otherV);
            if (noneV && otherFold)
                return "aura_rt::Optional<" + mapSemType(*otherV) + ">*";
        }
        // P3b：含堆 → Variant 指针；全值 → ValueVariant 值语义（feature-05，
        // 与上方 mapType UnionType 分支同步切换，防声明/表达式形态分裂）
        bool hasHeap = false;
        for (auto& v : u->variants)
            if (v && isUnionHeapVariant(v.get())) { hasHeap = true; break; }
        std::string result = hasHeap ? "aura_rt::Variant<" : "aura_rt::ValueVariant<";
        for (size_t i = 0; i < u->variants.size(); ++i) {
            if (i > 0) result += ", ";
            result += u->variants[i] ? mapSemType(*u->variants[i]) : "void";
        }
        result += hasHeap ? ">*" : ">";
        return result;
    }
    if (auto* r = dynamic_cast<const RecordSemType*>(&semType)) {
        if (r->isTuple) {
            // 元组：现场合成 TupleN<elem...>*（canonicalName 留空，递归映射元素）
            std::string cn = "aura_rt::Tuple" + std::to_string(r->fields.size()) + "<";
            for (size_t i = 0; i < r->fields.size(); ++i) {
                if (i > 0) cn += ", ";
                cn += r->fields[i].type ? mapSemType(*r->fields[i].type) : "void";
            }
            return cn + ">*";
        }
        if (!r->canonicalName.empty()) {
            return r->canonicalName + "*";
        }
        return "aura_rt::GcObject*";
    }
    if (auto* f = dynamic_cast<const FuncSemType*>(&semType)) {
        // feature-06（阶段 B）：FuncSemType → aura_rt::CallableObj<R, A...>*（与
        // mapType FunctionType 分支同构；None 保持 aura_rt::NoneType 防闭包体分裂）。
        std::string sig = "aura_rt::CallableObj<";
        sig += f->returnType ? mapSemType(*f->returnType) : "void";
        for (size_t i = 0; i < f->paramTypes.size(); ++i) {
            sig += ", ";
            sig += f->paramTypes[i] ? mapSemType(*f->paramTypes[i]) : "auto";
        }
        sig += ">*";
        return sig;
    }
    if (dynamic_cast<const CallableSemType*>(&semType)) {
        // feature-06（阶段 C）：裸 Callable → aura_rt::CallableErased*（GC 堆包装，
        // desc 追踪 target 槽；与 BuiltinRegistry "Callable" 条目 cppType 一致）
        return "aura_rt::CallableErased*";
    }
    if (auto* is = dynamic_cast<const InterfaceSemType*>(&semType)) {
        // P1：接口 → 值视图类型名。泛型接口实例化（Comparable<Point>）时用 typeArgs
        // 生成完整 C++ 名 Comparable<Point*>（实参经 mapSemType：record → 指针）。
        // 内置接口（Stringer / Comparable / Iterator）的 C++ 形态定义在 runtime 公共头
        // → 带 aura_rt:: 命名空间（bug-85 方案 B3；与 mapNamedType 同一映射）。
        std::string baseName = is->name;
        if (baseName == "Iterator" || baseName == "Stringer" || baseName == "Comparable")
            baseName = "aura_rt::" + baseName;
        if (is->typeArgs.empty()) return baseName;
        std::string result = baseName + "<";
        for (size_t i = 0; i < is->typeArgs.size(); ++i) {
            if (i > 0) result += ", ";
            result += is->typeArgs[i] ? mapSemType(*is->typeArgs[i]) : "void";
        }
        return result + ">";
    }
    if (auto* gs = dynamic_cast<const GenericSemType*>(&semType)) {
        if (!gs->resolvedName.empty()) {
            const std::string& rn = gs->resolvedName;
            // 内置 Iterator：resolvedName 即值视图类型（aura_rt::Iterator<T>，无 *）；
            // 内嵌 record 元素经 finalizeCppElem 补 *（Iterator<Point> → Iterator<Point*>
            // 与声明侧 mapType 一致，修复 [Iterator<Point>] 列表元素缺 *）
            if (gs->name == "Iterator") return finalizeCppElem(rn);
            // unwrap 兜底（2026-08-26）：resolvedName 已含 '*' 或为接口/Iterator 值视图
            // 时不再追加 '*'，避免双重指针 / 视图值被当指针（与 optionalElemCppName
            // finalizeElem TypeMap.cpp:256-262 的判定一致）。一律经 finalizeCppElem 递归
            // 补内嵌堆 record 的 '*'（Optional<Point> → Optional<Point*>*、channel<Point>
            // → Channel<Point*>*，与 optionalElemCppName / genLetStmt 一致，修复
            // [[Optional<Point>]] 嵌套列表声明侧元素物化缺 *）
            std::string fin = finalizeCppElem(rn);
            // 需堆指针的容器/裸名若未被 finalizeCppElem 补尾 '*'（如自引用泛型
            // Tree<int32_t>、未知类型名），追加外层 '*' 以保持既有 rn+"*" 语义
            if (!fin.empty() && fin.back() != '*' && !isIfaceViewTypeName(fin))
                fin += "*";
            return fin;
        }
        // 未实例化的泛型：先查调用点默认参数闭包物化映射（bug-06，跨模块默认参数
        // 闭包引用函数模板 T 时按调用点实参物化；与 mapType NamedType L74-75 /
        // mapGenericRef L390-391 行为统一。作用域由 genMethodCall isNs 默认参数补全
        // save-restore 限定，避免同名外层模板参数被误物化）
        auto mit = defaultArgMaterializedTypes_.find(gs->name);
        if (mit != defaultArgMaterializedTypes_.end()) return mit->second;
        // 未实例化的泛型：查 BuiltinRegistry 回退（如 sync.Channel → aura_rt::ThreadChannel*）
        if (auto* ti = BuiltinRegistry::get().findType(gs->name))
            return ti->cppType;
        return "auto";
    }
    return "/* unknown_semtype */";
}

// ============================================================
// 类型描述符生成（plan §4.2, §4.10）
// ============================================================

void CodeGenerator::genTypeDescriptor(std::ostream& cpp,
                                       const std::string& structName,
                                       const std::vector<std::string>& templateParams,
                                       const std::vector<std::string>& ptrFieldNames,
                                       const std::vector<std::string>& deferredPtrFieldNames) {
    // 构建 C++ 模板前缀: template<typename A, typename B>
    std::string tprefix, tparamsStr;
    if (!templateParams.empty()) {
        tprefix = "template<";
        for (size_t i = 0; i < templateParams.size(); ++i) {
            if (i > 0) { tprefix += ", "; tparamsStr += ", "; }
            tprefix += "typename " + templateParams[i];
            tparamsStr += templateParams[i];
        }
        tprefix += ">\n";
        tparamsStr = "<" + tparamsStr + ">";
    }

    std::string fullName = structName + tparamsStr;

    // 非模板类型不需要 prefix
    bool isTemplate = !templateParams.empty();

    if (!isTemplate) {
        // 非模板类型：cpp 中直接定义（现状不变）
        if (ptrFieldNames.empty()) {
            cpp << "const aura_rt::TypeDescriptor " << fullName
                << "::_desc = { sizeof(" << fullName << "), 0, nullptr };\n";
        } else {
            cpp << "static const size_t _" << structName << "_ptrs[] = {";
            for (size_t i = 0; i < ptrFieldNames.size(); ++i) {
                if (i > 0) cpp << ", ";
                auto& pf = ptrFieldNames[i];
                // 视图字段条目格式 "field+ViewType" → offsetof(Self, field) + offsetof(ViewType, self)
                auto plus = pf.find('+');
                if (plus != std::string::npos) {
                    cpp << "offsetof(" << fullName << ", " << pf.substr(0, plus)
                        << ") + offsetof(" << pf.substr(plus + 1) << ", self)";
                } else {
                    cpp << "offsetof(" << fullName << ", " << pf << ")";
                }
            }
            cpp << "};\n";
            cpp << "const aura_rt::TypeDescriptor " << fullName
                << "::_desc = { sizeof(" << fullName << "), "
                << ptrFieldNames.size() << ", _"
                << structName << "_ptrs };\n";
        }
        return;
    }

    // 模板类型：若既无确定指针字段也无延迟字段 → 原快速路径（现状不变）
    if (ptrFieldNames.empty() && deferredPtrFieldNames.empty()) {
        cpp << tprefix << "const aura_rt::TypeDescriptor " << fullName
            << "::_desc = { sizeof(" << fullName << "), 0, nullptr };\n";
        return;
    }

    // #54：per-instantiation desc——全量数组 + constexpr 计数变量模板，_desc 保持常量初始化。
    // 数组布局「确定字段在前、延迟候选在后」；_cnt<T> = N确定 + Σ(延迟字段
    // viewSlotCoreCond(裸名) ? 1 : 0)（feature-07 Step 2：判据含视图 traits 侧，
    // 与 _ptrs 单点同源），GC 只消费前 _cnt 项。
    // 变量模板每实例化独立（现状 _X_ptrs<T> 同机制）；_desc 仍为聚合常量初始化
    // （无动态初始化时序风险，区别于 make_desc 函数方案）；消费点 &X::_desc 零改动。
    // T=值类型 → is_convertible false → 不计数不消费（desc 层不误追踪假根）；
    // T=堆（string/record/列表等 GcObject 派生指针）→ 计数消费（不漏追踪）。
    // 审查后修正（问题 C）：延迟候选数组项不能按声明顺序全量排列 + 前缀计数——
    // Pair2<A,B>{a:A,b:B} 在 A=int、B=Point* 时 _cnt=1 却消费 offsetof(a)
    // （int 值 1 被 mark 当指针读 → 0xC0000005，gdb 实证 parallel_mark.cpp:33）。
    // 每项改为「第 i+1 个有效延迟字段（is_convertible 为 true）的偏移」的嵌套
    // 条件表达式：有效字段稳定排前（前 _cnt 项恰好是有效偏移），无效字段落在
    // cnt 之后不被消费；数组仍为裸数组常量初始化。不足 i+1 个有效字段时项值
    // 0（仅占位，不被消费）。
    cpp << tprefix << "static const size_t _" << structName << "_ptrs[] = {\n";
    for (size_t i = 0; i < ptrFieldNames.size(); ++i) {
        auto& pf = ptrFieldNames[i];
        auto plus = pf.find('+');
        // 审查后修正（问题 A）：模板分支用 __builtin_offsetof（非宏，模板实参列表
        // 逗号不被分裂；offsetof 是宏，多模板参数 Pair2<A, B> 时 g++ 报
        // "macro 'offsetof' passed 3 arguments"）。视图子偏移同样替换。
        cpp << "    __builtin_offsetof(" << fullName << ", " << pf.substr(0, plus) << ")";
        if (plus != std::string::npos)
            cpp << " + __builtin_offsetof(" << pf.substr(plus + 1) << ", self)";
        cpp << ",\n";
    }
    for (size_t i = 0; i < deferredPtrFieldNames.size(); ++i) {
        cpp << "    " << genDeferredSelectExpr(fullName, deferredPtrFieldNames, 0, i + 1) << ",\n";
    }
    cpp << "};\n";
    cpp << tprefix << "constexpr size_t _" << structName << "_cnt = " << ptrFieldNames.size();
    for (auto& df : deferredPtrFieldNames) {
        auto bar = df.find('|');
        // feature-07 Step 2（G1）：判据与 _ptrs 内 genDeferredSelectExpr 单点同源——
        // 旧串仅 is_convertible（视图值字段不计入）会与已 traits 化的 _ptrs 分叉：
        // _ptrs 项为视图复合偏移、_cnt 不计 → 视图 self 漏标（静默内存错误）
        cpp << "\n    + ((" << viewSlotCoreCond(df.substr(bar + 1)) << ") ? 1 : 0)";
    }
    cpp << ";\n";
    cpp << tprefix << "const aura_rt::TypeDescriptor " << fullName
        << "::_desc = { sizeof(" << fullName << "), _" << structName << "_cnt<";
    for (size_t i = 0; i < templateParams.size(); ++i) {
        if (i > 0) cpp << ", ";
        cpp << templateParams[i];
    }
    cpp << ">, _" << structName << "_ptrs<";
    for (size_t i = 0; i < templateParams.size(); ++i) {
        if (i > 0) cpp << ", ";
        cpp << templateParams[i];
    }
    cpp << "> };\n";
}

// #54（审查后修正，问题 C）：递归生成「第 k 个有效延迟字段偏移」嵌套条件表达式。
// deferred 条目格式 "name|cppType"；有效 = std::is_convertible_v<cppType, GcObject*>
//        || (!is_convertible_v && aura_rt::GcViewSlot<cppType>::value)（feature-07 Step 2）。
// 语义：从 idx 起扫描，第 k 个有效字段（1-based）的 __builtin_offsetof；不足返回 "0"。
// 生成示例（Pair2 a:A,b:B，位置 1 = 第 2 个有效字段）：
//   (A可转 ? (B可转 ? off(b) : 0) : 0)
// 递归终止：idx 越界 → "0"（每次递归 idx+1，字段数有限，无死循环）。
// feature-07 Step 2（G1）：有效槽判据串唯一生成点（与 runtime/types.h GcViewSlot 配套，
// P1 traits 修订 + G2 类型可转换性双判据）。同一字符串同时用于 _ptrs 条件分支与 _cnt
// 计数项——两处均由本函数单点产出，构造性保证「逐字同源」（G1：判据错位 = GC 消费端
// 读越界偏移、或视图 self 漏标）。
std::string CodeGenerator::viewSlotCoreCond(const std::string& cppType) const {
    std::string ptrConv = "std::is_convertible_v<" + cppType + ", aura_rt::GcObject*>";
    return ptrConv + " || (!" + ptrConv + " && aura_rt::GcViewSlot<" + cppType + ">::value)";
}

std::string CodeGenerator::genDeferredSelectExpr(
    const std::string& fullName,
    const std::vector<std::string>& deferredPtrFieldNames,
    size_t idx, size_t k,
    const std::set<std::string>& viewSlots) const {
    // feature-07 Step 2：viewSlots 为字符串层面登记的视图槽名集合（契约/诊断用）。
    // 有效槽判据与偏移分支恒由**类型层面**判据驱动（下方 ptrConv/cond），与生成代码
    // 内 _cnt 判据逐字同源——若改用字符串集合做第二判据，一旦分叉即 _cnt 计入而 _ptrs
    // 偏移错位（GC 把非 GC 数据当 GcObject* 解引用，崩溃级，审查 G1）。
    (void)viewSlots;
    if (idx >= deferredPtrFieldNames.size()) return "0";
    auto bar = deferredPtrFieldNames[idx].find('|');
    std::string name = deferredPtrFieldNames[idx].substr(0, bar);
    std::string cppType = deferredPtrFieldNames[idx].substr(bar + 1);
    // GC 指针槽判据（视图槽判据为其补集侧：视图值类型不可转换为 GcObject*）
    std::string ptrConv = "std::is_convertible_v<" + cppType + ", aura_rt::GcObject*>";
    // 有效槽判据（P1 traits 修订 + G2 类型可转换性双判据）——单点取自 viewSlotCoreCond，
    // 与代码内 _cnt 判据串**构造性逐字同源**（G1）
    std::string cond = "(" + viewSlotCoreCond(cppType) + ")";
    // 偏移（G5：弃 offsetof(VT, self) 依赖表达式）：GC 指针槽取槽偏移；视图槽取
    // 槽偏移 + sizeof(void*)——接口/迭代器视图布局恒为 {fnPtr, self}，self 偏移恒
    // sizeof(void*)（与 iterator.h MapFnIter desc 的 offsetof(Iterator<T>, self) 语义
    // 等价，规避泛型上下文非标准布局 UB 面）。分支判据 = ptrConv（与 cond 同源）
    std::string off = "(" + ptrConv + " ? __builtin_offsetof(" + fullName + ", " + name + ")"
        + " : __builtin_offsetof(" + fullName + ", " + name + ") + sizeof(void*))";
    if (k == 1) {
        // 本字段有效 → 本字段偏移；否则递归下一个字段
        return "(" + cond + " ? " + off + " : "
            + genDeferredSelectExpr(fullName, deferredPtrFieldNames, idx + 1, k) + ")";
    }
    // 本字段有效 → 从下一个起选第 k-1 个；否则从下一个起选第 k 个
    return "(" + cond + " ? "
        + genDeferredSelectExpr(fullName, deferredPtrFieldNames, idx + 1, k - 1) + " : "
        + genDeferredSelectExpr(fullName, deferredPtrFieldNames, idx + 1, k) + ")";
}

} // namespace Aura
