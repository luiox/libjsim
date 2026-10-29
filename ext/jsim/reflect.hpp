/// @file reflect.hpp
/// @brief jsim-reflect 扩展包：Class.forName / getAnnotation / 注解元素访问器。
///
/// 反射是「方法名就是数据」的调用面：`invokeinterface Booger.d1()` 的语义不在
/// 签名里，而在接收者对象上。所以本包不用静态键注册，而是走 Session 的兜底
/// 解析口（callMockResolver），按接收者的注解代理对象取元素值。
///
/// 元数据不自己解析：provider 是唯一入口，libjbc 桥、JSON 表、手写脚本都从
/// 这里灌进来。核心与本包都不碰容器格式（设计 D10）。
///
/// 可选接入，核心零依赖本文件：
///   jsim::reflect::install(session, [](const std::string& n) { ... });

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jsim/engine.hpp"

namespace jsim {
namespace reflect {

/// 注解元素值的数据形——与 Session 无关，可跨会话复用。
/// 堆对象（数组）在物化时才分配，因此 provider 不必知道 allocId 分配规则。
struct AnnoValue {
    enum class Kind : u8 {
        Int,
        Long,
        Float,
        Double,
        Bool,
        Char,
        Byte,
        Short,
        Str,
        StrArray,
        IntArray,
    };

    Kind kind = Kind::Int;
    i32 i = 0;
    i64 j = 0;
    f64 d = 0.0;
    std::string s;
    std::vector<std::string> strs;
    std::vector<i32> ints;

    static AnnoValue int32(i32 v) {
        AnnoValue a;
        a.kind = Kind::Int;
        a.i = v;
        return a;
    }
    static AnnoValue int64(i64 v) {
        AnnoValue a;
        a.kind = Kind::Long;
        a.j = v;
        return a;
    }
    static AnnoValue boolean(bool v) {
        AnnoValue a;
        a.kind = Kind::Bool;
        a.i = v ? 1 : 0;
        return a;
    }
    static AnnoValue text(std::string v) {
        AnnoValue a;
        a.kind = Kind::Str;
        a.s = std::move(v);
        return a;
    }
    static AnnoValue textArray(std::vector<std::string> v) {
        AnnoValue a;
        a.kind = Kind::StrArray;
        a.strs = std::move(v);
        return a;
    }
    static AnnoValue intArray(std::vector<i32> v) {
        AnnoValue a;
        a.kind = Kind::IntArray;
        a.ints = std::move(v);
        return a;
    }

    /// 注解元素在字节码里就是一个无参有返回值的方法，描述符由值类型决定。
    const char* accessorDesc() const {
        switch (kind) {
        case Kind::Int: return "()I";
        case Kind::Long: return "()J";
        case Kind::Float: return "()F";
        case Kind::Double: return "()D";
        case Kind::Bool: return "()Z";
        case Kind::Char: return "()C";
        case Kind::Byte: return "()B";
        case Kind::Short: return "()S";
        case Kind::Str: return "()Ljava/lang/String;";
        case Kind::StrArray: return "()[Ljava/lang/String;";
        case Kind::IntArray: return "()[I";
        }
        return "()I";
    }
};

/// 一个注解实例：类型 + 元素表。元素顺序即声明顺序（访问器无顺序语义）。
struct AnnotationData {
    std::string type; // 内部名，如 "dev/realme/Booger"（不带 L;）
    std::vector<std::pair<std::string, AnnoValue>> elements;
};

/// 类元数据。provider 只需回答「这个类有哪些注解」——反射面用到的就这些。
struct ClassMeta {
    std::string internalName;
    std::vector<AnnotationData> annotations;
};

using MetaProvider = std::function<const ClassMeta*(const std::string& internalName)>;

namespace detail {

/// 内部名归一：容忍 "Ldev/realme/Booger;"、点分名、"dev/realme/Booger"。
inline std::string normalizeName(std::string name) {
    if (name.size() >= 2 && name.front() == 'L' && name.back() == ';') {
        name = name.substr(1, name.size() - 2);
    }
    for (char& ch : name) {
        if (ch == '.') ch = '/';
    }
    return name;
}

/// 描述符 → 返回类型。只认无参形态（注解元素访问器全是 ()X）。
inline ValType retTypeOfDesc(const std::string& desc) {
    if (desc.size() < 3 || desc[0] != '(') return ValType::Ref;
    const char head = desc[1];
    switch (head) {
    case 'I':
    case 'Z':
    case 'B':
    case 'C':
    case 'S': return ValType::I32;
    case 'J': return ValType::I64;
    case 'F': return ValType::F32;
    case 'D': return ValType::F64;
    default: return ValType::Ref;
    }
}

/// 包内状态：注解代理集合、按需生成的访问器 mock。
/// Class 对象缓存不在这里——`ldc` 类常量与 `forName` 必须同一对象，那是核心的
/// Session::classObjectOf 负责的事。
struct State {
    Session* session = nullptr;
    MetaProvider provider;
    std::unordered_set<u64> proxies;                        // 注解代理对象 id
    std::unordered_map<std::string, CallMock> accessorMocks; // 键 = name+desc
};

/// AnnoValue → SimValue。数组在这里才落堆，provider 因此不依赖会话。
inline SimValue materialize(Session& session, const AnnoValue& v) {
    switch (v.kind) {
    case AnnoValue::Kind::Int:
    case AnnoValue::Kind::Bool:
    case AnnoValue::Kind::Char:
    case AnnoValue::Kind::Byte:
    case AnnoValue::Kind::Short:
        return SimValue::knownI32(v.i);
    case AnnoValue::Kind::Long:
        return SimValue::knownI64(v.j);
    case AnnoValue::Kind::Float:
        return SimValue::knownF32(static_cast<float>(v.d));
    case AnnoValue::Kind::Double:
        return SimValue::knownF64(v.d);
    case AnnoValue::Kind::Str:
        return SimValue::knownStr(v.s);
    case AnnoValue::Kind::StrArray: {
        const u64 id = session.nextAllocId++;
        HeapObj obj;
        obj.className = "[Ljava/lang/String;";
        obj.refs.reserve(v.strs.size());
        for (const std::string& text : v.strs) obj.refs.push_back(SimValue::knownStr(text));
        session.heap[id] = std::move(obj);
        return SimValue::knownObj(id);
    }
    case AnnoValue::Kind::IntArray: {
        const u64 id = session.nextAllocId++;
        HeapObj obj;
        obj.className = "[I";
        obj.elems.reserve(v.ints.size());
        for (i32 value : v.ints) obj.elems.push_back(static_cast<i64>(value));
        session.heap[id] = std::move(obj);
        return SimValue::knownObj(id);
    }
    }
    return SimValue::unknownOf(ValType::Ref);
}

/// 取堆对象的类名（非 Obj 或缺失返回空串）。
inline std::string heapClassOf(Session& session, const SimValue& v) {
    if (!v.known || v.ref.kind != RefPayload::Kind::Obj) return std::string();
    auto it = session.heap.find(v.ref.objId);
    if (it == session.heap.end()) return std::string();
    return it->second.className;
}

} // namespace detail

/// 安装反射面。返回的 State 由 Session 的 resolver 持有，调用方可以丢弃返回值。
inline void install(Session& session, MetaProvider provider) {
    auto state = std::make_shared<detail::State>();
    state->session = &session;
    state->provider = std::move(provider);

    // Class.forName(String)：名字可能是点分名（Java 源级）或内部名（字节码级），
    // 统一归一到内部名。类不存在不在这里判死——模拟器的职责不是复刻类加载器
    // 的失败面，元数据缺失会在 getAnnotation 处自然表现为 null。
    session.callMocks["java/lang/Class.forName(Ljava/lang/String;)Ljava/lang/Class;"] =
        [state](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> arg = frame.popChecked();
        if (!arg.isOk()) return Result<SimValue, Bail>::err(arg.error());
        if (!arg.value().known || arg.value().ref.kind != RefPayload::Kind::Str) {
            return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::Ref));
        }
        const std::string internal = detail::normalizeName(arg.value().ref.str);
        return Result<SimValue, Bail>::ok(
            SimValue::knownObj(frame.session().classObjectOf(internal)));
    };

    // Class.getAnnotation(Class)：命中就建一个注解代理对象，元素值挂在 fields
    // 的 "@元素名" 槽上；未命中返回 null（这是 Java 的真实行为，不是"未知"）。
    session.callMocks["java/lang/Class.getAnnotation(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;"] =
        [state](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> annoType = frame.popChecked();
        if (!annoType.isOk()) return Result<SimValue, Bail>::err(annoType.error());
        Result<SimValue, Bail> target = frame.popChecked();
        if (!target.isOk()) return Result<SimValue, Bail>::err(target.error());

        Session& session = frame.session();
        if (!target.value().known || !annoType.value().known) {
            return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::Ref));
        }
        const std::string targetClass = detail::heapClassOf(session, target.value());
        const std::string annoClass = detail::heapClassOf(session, annoType.value());
        if (targetClass != "java/lang/Class" || annoClass != "java/lang/Class") {
            return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::Ref));
        }
        const std::string targetName = session.heap[target.value().ref.objId].data;
        const std::string wanted = session.heap[annoType.value().ref.objId].data;

        const ClassMeta* meta = state->provider ? state->provider(targetName) : nullptr;
        if (meta == nullptr) return Result<SimValue, Bail>::ok(SimValue::knownNull());

        const AnnotationData* found = nullptr;
        for (const AnnotationData& anno : meta->annotations) {
            if (detail::normalizeName(anno.type) == wanted) {
                found = &anno;
                break;
            }
        }
        if (found == nullptr) return Result<SimValue, Bail>::ok(SimValue::knownNull());

        const u64 id = session.nextAllocId++;
        HeapObj proxy;
        proxy.className = "L" + wanted + ";";
        for (const auto& element : found->elements) {
            proxy.fields["@" + element.first] = detail::materialize(session, element.second);
        }
        session.heap[id] = std::move(proxy);
        state->proxies.insert(id);
        return Result<SimValue, Bail>::ok(SimValue::knownObj(id));
    };

    // Class.getName()：返回点分全名（Java 语义）。
    session.callMocks["java/lang/Class.getName()Ljava/lang/String;"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> recv = frame.popChecked();
        if (!recv.isOk()) return Result<SimValue, Bail>::err(recv.error());
        Session& session = frame.session();
        if (detail::heapClassOf(session, recv.value()) != "java/lang/Class") {
            return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::Ref));
        }
        std::string name = session.heap[recv.value().ref.objId].data;
        for (char& ch : name) {
            if (ch == '/') ch = '.';
        }
        return Result<SimValue, Bail>::ok(SimValue::knownStr(name));
    };

    // Class.getClassLoader()：BSM 里只为传给 MethodType.fromMethodDescriptorString。
    // 载荷不参与语义，给一个稳定的哨兵对象即可。
    session.callMocks["java/lang/Class.getClassLoader()Ljava/lang/ClassLoader;"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> recv = frame.popChecked();
        if (!recv.isOk()) return Result<SimValue, Bail>::err(recv.error());
        (void)recv;
        // 载荷不参与语义，给一个稳定的哨兵对象即可（同一会话内恒等）。
        return Result<SimValue, Bail>::ok(
            SimValue::knownObj(frame.session().classObjectOf("jsim/ClassLoader")));
    };

    // 注解元素访问器：兜底解析。只认领「接收者是注解代理 + 无参有返回值」的调用，
    // 认不出就返回 nullptr 交回核心 Bail——绝不把真正的缺环境吞成 Unknown。
    session.callMockResolver = [state](const CallRef& callee, Frame& frame) -> CallMock* {
        if (callee.desc.size() < 3 || callee.desc[0] != '(' || callee.desc[1] != ')') {
            return nullptr;
        }
        // 无参形态：接收者就在栈顶。不是注解代理就不是本包的事。
        Result<SimValue, Bail> recv = frame.peekChecked(0);
        if (!recv.isOk() || !recv.value().known) return nullptr;
        if (recv.value().ref.kind != RefPayload::Kind::Obj) return nullptr;
        if (state->proxies.count(recv.value().ref.objId) == 0) return nullptr;

        const std::string key = callee.name + callee.desc;
        auto cached = state->accessorMocks.find(key);
        if (cached != state->accessorMocks.end()) return &cached->second;

        const std::string elementName = "@" + callee.name;
        const ValType retType = detail::retTypeOfDesc(callee.desc);
        CallMock mock = [elementName, retType](Frame& frame) -> Result<SimValue, Bail> {
            Result<SimValue, Bail> recv = frame.popChecked();
            if (!recv.isOk()) return Result<SimValue, Bail>::err(recv.error());
            if (!recv.value().known || recv.value().ref.kind != RefPayload::Kind::Obj) {
                return Result<SimValue, Bail>::ok(SimValue::unknownOf(retType));
            }
            Session& session = frame.session();
            auto it = session.heap.find(recv.value().ref.objId);
            if (it == session.heap.end()) {
                return Result<SimValue, Bail>::ok(SimValue::unknownOf(retType));
            }
            auto slot = it->second.fields.find(elementName);
            if (slot == it->second.fields.end()) {
                return Result<SimValue, Bail>::ok(SimValue::unknownOf(retType));
            }
            SimValue v = slot->second;
            v.type = retType;
            return Result<SimValue, Bail>::ok(v);
        };
        return &state->accessorMocks.emplace(key, std::move(mock)).first->second;
    };
}

} // namespace reflect
} // namespace jsim
