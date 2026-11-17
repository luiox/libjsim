#include "jsim/jni.hpp"

namespace jsim::jni {

Result<u64, Bail> HandlePool::makeRef(SimValue v, bool global)
{
    if (!v.known || v.type != ValType::Ref) {
        return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "句柄池只收已知引用"});
    }
    if (v.ref.kind == RefPayload::Kind::Null) {
        return Result<u64, Bail>::ok(0);
    }
    const u64 h = next;
    next += kStride;
    refs[h] = {std::move(v), global};
    return Result<u64, Bail>::ok(h);
}

Result<SimValue, Bail> HandlePool::deref(u64 h) const
{
    if (h == 0) return Result<SimValue, Bail>::ok(SimValue::knownNull());
    auto it = refs.find(h);
    if (it == refs.end()) {
        return Result<SimValue, Bail>::err({Bail::Reason::BadOperand, 0, "悬空句柄"});
    }
    return Result<SimValue, Bail>::ok(it->second.value);
}

Result<u64, Bail> HandlePool::makeMember(const Member& m)
{
    const u64 h = next;
    next += kStride;
    members[h] = m;
    return Result<u64, Bail>::ok(h);
}

Result<const HandlePool::Member*, Bail> HandlePool::memberOf(u64 h) const
{
    auto it = members.find(h);
    if (it == members.end()) {
        return Result<const Member*, Bail>::err({Bail::Reason::BadOperand, 0, "悬空成员 ID"});
    }
    return Result<const Member*, Bail>::ok(&it->second);
}

void HandlePool::popLocals()
{
    for (auto it = refs.begin(); it != refs.end();) {
        if (it->second.global) {
            ++it;
        } else {
            it = refs.erase(it);
        }
    }
}

usize HandlePool::alive() const
{
    return refs.size();
}

bool HandlePool::sameObject(u64 a, u64 b) const
{
    if (a == b) return true;
    Result<SimValue, Bail> x = deref(a);
    Result<SimValue, Bail> y = deref(b);
    if (!x.isOk() || !y.isOk()) return false;
    const SimValue& u = x.value();
    const SimValue& v = y.value();
    if (u.ref.kind == RefPayload::Kind::Null && v.ref.kind == RefPayload::Kind::Null) return true;
    if (u.ref.kind == RefPayload::Kind::Obj && v.ref.kind == RefPayload::Kind::Obj) {
        return u.ref.objId == v.ref.objId;
    }
    return false;
}

} // namespace jsim::jni
