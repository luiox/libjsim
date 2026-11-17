#include "jsim/indy.hpp"

namespace jsim {
namespace indy {

namespace detail {

constexpr const char* kMethodType   = "java/lang/invoke/MethodType";
constexpr const char* kMethodHandle = "java/lang/invoke/MethodHandle";
constexpr const char* kCallSite     = "java/lang/invoke/ConstantCallSite";
constexpr const char* kLookup       = "java/lang/invoke/MethodHandles$Lookup";

/// MethodHandle 载荷的四个槽。键带 `@` 前缀，与 num 的 `@v`、reflect 的 `@元素名`
/// 同一族——`@` 开头的键不会和 Java 字段名（`name::desc`）撞。
constexpr const char* kSlotKind  = "@kind";
constexpr const char* kSlotOwner = "@owner";
constexpr const char* kSlotName  = "@name";
constexpr const char* kSlotDesc  = "@desc";

inline const HeapObj* heapOf(Session& session, const SimValue& v)
{
    if (!v.known || v.ref.kind != RefPayload::Kind::Obj) return nullptr;
    auto it = session.heap.find(v.ref.objId);
    return it == session.heap.end() ? nullptr : &it->second;
}

inline bool hasClass(Session& session, const SimValue& v, const char* className)
{
    const HeapObj* obj = heapOf(session, v);
    return obj != nullptr && obj->className == className;
}

/// 新建一个载荷为单字符串的对象（MethodType / CallSite 都用）。
inline SimValue allocText(Session& session, const char* className, std::string data)
{
    const u64 id = session.nextAllocId++;
    HeapObj obj;
    obj.className = className;
    obj.data      = std::move(data);
    session.heap[id] = std::move(obj);
    return SimValue::knownObj(id);
}

/// 新建一个方法句柄对象。
inline SimValue allocHandle(Session& session, Target::Kind kind, std::string owner,
                            std::string name, std::string desc)
{
    const u64 id = session.nextAllocId++;
    HeapObj obj;
    obj.className = kMethodHandle;
    obj.fields[kSlotKind]  = SimValue::knownI32(static_cast<i32>(kind));
    obj.fields[kSlotOwner] = SimValue::knownStr(std::move(owner));
    obj.fields[kSlotName]  = SimValue::knownStr(std::move(name));
    obj.fields[kSlotDesc]  = SimValue::knownStr(std::move(desc));
    session.heap[id] = std::move(obj);
    return SimValue::knownObj(id);
}

inline Result<SimValue, Bail> pop(Frame& frame)
{
    return frame.popChecked();
}

}   // namespace detail

bool targetOf(Session& session, const SimValue& callSite, Target& out)
{
    const HeapObj* obj = detail::heapOf(session, callSite);
    if (obj == nullptr) return false;

    // CallSite：载荷是指向句柄的字符串？不——载荷是句柄对象本身，存在 data 槽
    // 之外。v1 的做法是让 CallSite 的 `@target` 槽存句柄的 objId。
    if (obj->className == detail::kCallSite) {
        auto it = obj->fields.find("@target");
        if (it == obj->fields.end()) return false;
        return targetOf(session, it->second, out);
    }
    if (obj->className != detail::kMethodHandle) return false;

    auto kind = obj->fields.find(detail::kSlotKind);
    auto owner = obj->fields.find(detail::kSlotOwner);
    auto name = obj->fields.find(detail::kSlotName);
    auto desc = obj->fields.find(detail::kSlotDesc);
    if (kind == obj->fields.end() || owner == obj->fields.end() || name == obj->fields.end()
        || desc == obj->fields.end()) {
        return false;
    }
    if (!owner->second.known || !name->second.known || !desc->second.known) return false;

    out.kind  = static_cast<Target::Kind>(kind->second.asU32());
    out.owner = owner->second.ref.str;
    out.name  = name->second.ref.str;
    out.desc  = desc->second.ref.str;
    return out.valid();
}

bool isCallSite(Session& session, const SimValue& v)
{
    const HeapObj* obj = detail::heapOf(session, v);
    return obj != nullptr && obj->className == detail::kCallSite;
}

SimValue makeMethodType(Session& session, std::string desc)
{
    return detail::allocText(session, detail::kMethodType, std::move(desc));
}

SimValue makeMethodHandle(Session& session, Target::Kind kind, std::string owner,
                                 std::string name, std::string desc)
{
    return detail::allocHandle(session, kind, std::move(owner), std::move(name), std::move(desc));
}

void install(Session& session)
{
    // ---- MethodType：载荷 = 描述符字符串 ----
    //
    // 描述符不合法（不是以 '(' 开头）时返回 Unknown 而不是 Bail：BSM 的入参可能
    // 没被解出来，那是「不知道」，不是「这个调用点坏了」。
    auto makeType = [](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> loader = detail::pop(frame);
        if (!loader.isOk()) return Result<SimValue, Bail>::err(loader.error());
        Result<SimValue, Bail> desc = detail::pop(frame);
        if (!desc.isOk()) return Result<SimValue, Bail>::err(desc.error());
        if (!desc.value().known || desc.value().ref.kind != RefPayload::Kind::Str) {
            return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::Ref));
        }
        const std::string& text = desc.value().ref.str;
        if (text.empty() || text.front() != '(') {
            return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::Ref));
        }
        return Result<SimValue, Bail>::ok(
            detail::allocText(frame.session(), detail::kMethodType, text));
    };

    session.callMocks["java/lang/invoke/MethodType.fromMethodDescriptorString"
                      "(Ljava/lang/String;Ljava/lang/ClassLoader;)Ljava/lang/invoke/MethodType;"] =
        makeType;
    session.callMocks["java/lang/invoke/MethodType.fromMethodDescriptorString"
                      "(Ljava/lang/String;)Ljava/lang/invoke/MethodType;"] = [](Frame& frame) {
        Result<SimValue, Bail> desc = detail::pop(frame);
        if (!desc.isOk()) return Result<SimValue, Bail>::err(desc.error());
        if (!desc.value().known || desc.value().ref.kind != RefPayload::Kind::Str) {
            return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::Ref));
        }
        const std::string& text = desc.value().ref.str;
        if (text.empty() || text.front() != '(') {
            return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::Ref));
        }
        return Result<SimValue, Bail>::ok(
            detail::allocText(frame.session(), detail::kMethodType, text));
    };

    // ---- Lookup.findXxx(Class, String, MethodType) → MethodHandle ----
    //
    // 接收者（Lookup 自己）不参与语义：它只是个「以什么身份找」的上下文，
    // 折叠只关心找到的是谁。所以这里照常弹掉但不看。
    auto finder = [](Target::Kind kind) {
        return [kind](Frame& frame) -> Result<SimValue, Bail> {
            Result<SimValue, Bail> type = detail::pop(frame);
            if (!type.isOk()) return Result<SimValue, Bail>::err(type.error());
            Result<SimValue, Bail> name = detail::pop(frame);
            if (!name.isOk()) return Result<SimValue, Bail>::err(name.error());
            Result<SimValue, Bail> owner = detail::pop(frame);
            if (!owner.isOk()) return Result<SimValue, Bail>::err(owner.error());
            Result<SimValue, Bail> lookup = detail::pop(frame);
            if (!lookup.isOk()) return Result<SimValue, Bail>::err(lookup.error());

            Session& session = frame.session();
            const HeapObj* ownerObj = detail::heapOf(session, owner.value());
            const HeapObj* typeObj  = detail::heapOf(session, type.value());
            if (ownerObj == nullptr || ownerObj->className != "java/lang/Class"
                || typeObj == nullptr || typeObj->className != detail::kMethodType) {
                return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::Ref));
            }
            if (!name.value().known || name.value().ref.kind != RefPayload::Kind::Str) {
                return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::Ref));
            }
            return Result<SimValue, Bail>::ok(detail::allocHandle(
                session, kind, ownerObj->data, name.value().ref.str, typeObj->data));
        };
    };

    const char* kFinderDesc = "(Ljava/lang/Class;Ljava/lang/String;"
                              "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/MethodHandle;";
    session.callMocks[std::string("java/lang/invoke/MethodHandles$Lookup.findVirtual") + kFinderDesc] =
        finder(Target::Kind::Virtual);
    session.callMocks[std::string("java/lang/invoke/MethodHandles$Lookup.findStatic") + kFinderDesc] =
        finder(Target::Kind::Static);
    session.callMocks[std::string("java/lang/invoke/MethodHandles$Lookup.findSpecial") + kFinderDesc] =
        finder(Target::Kind::Special);

    // ---- MethodHandle.asType(MethodType) → 同一个句柄 ----
    //
    // v1 不做类型适配。调用点的描述符与目标描述符不一致时，折叠必须放弃
    // ——这一点由折叠方按 desc 比对把关，不在这里吞掉。
    session.callMocks["java/lang/invoke/MethodHandle.asType"
                      "(Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/MethodHandle;"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> type = detail::pop(frame);
        if (!type.isOk()) return Result<SimValue, Bail>::err(type.error());
        Result<SimValue, Bail> handle = detail::pop(frame);
        if (!handle.isOk()) return Result<SimValue, Bail>::err(handle.error());
        if (!detail::hasClass(frame.session(), handle.value(), detail::kMethodHandle)) {
            return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::Ref));
        }
        return Result<SimValue, Bail>::ok(handle.value());
    };

    // ---- ConstantCallSite：分配 + 构造 ----
    session.allocScaffolds[detail::kCallSite] = [] { return std::string(); };

    session.callMocks["java/lang/invoke/ConstantCallSite.<init>"
                      "(Ljava/lang/invoke/MethodHandle;)V"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> handle = detail::pop(frame);
        if (!handle.isOk()) return Result<SimValue, Bail>::err(handle.error());
        Result<SimValue, Bail> site = detail::pop(frame);
        if (!site.isOk()) return Result<SimValue, Bail>::err(site.error());
        Session& session = frame.session();
        if (!site.value().known || site.value().ref.kind != RefPayload::Kind::Obj) {
            return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::Ref));
        }
        auto it = session.heap.find(site.value().ref.objId);
        if (it == session.heap.end()) return Result<SimValue, Bail>::ok(SimValue::knownNull());
        it->second.fields["@target"] = handle.value();
        return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::Ref));
    };
}

}   // namespace indy
}   // namespace jsim
