#include "detail/guest.hpp"

#include <cstring>

namespace jsim::jni {

namespace detail {

/// 读 NUL 结尾的 guest 字符串。只用于 FindClass / GetMethodID 实参这类冷路径。
Result<std::string, Bail> readCStr(JniEnv& env, u64 addr, usize cap)
{
    std::string s;
    for (usize i = 0; i < cap; ++i) {
        char c = 0;
        Result<void, Bail> r = env.mem.read(addr + i, &c, 1);
        if (!r.isOk()) return Result<std::string, Bail>::err(r.error());
        if (c == '\0') return Result<std::string, Bail>::ok(std::move(s));
        s.push_back(c);
    }
    return Result<std::string, Bail>::err({Bail::Reason::BadOperand, 0, "guest 字符串缺 NUL 结尾"});
}

Result<std::vector<u8>, Bail> readBytes(JniEnv& env, u64 addr, usize n)
{
    std::vector<u8> buf(n);
    if (n != 0) {
        Result<void, Bail> r = env.mem.read(addr, buf.data(), n);
        if (!r.isOk()) return Result<std::vector<u8>, Bail>::err(r.error());
    }
    return Result<std::vector<u8>, Bail>::ok(std::move(buf));
}

Result<void, Bail> writeBytes(JniEnv& env, u64 addr, const void* p, usize n)
{
    return env.mem.write(addr, p, n);
}

/// jclass 句柄 → 内部类名。Class 对象是 session.classObjectOf 建的,类名在 data。
Result<std::string, Bail> classNameOfClass(JniEnv& env, u64 h)
{
    Result<SimValue, Bail> v = env.pool.deref(h);
    if (!v.isOk()) return Result<std::string, Bail>::err(v.error());
    if (v.value().ref.kind != RefPayload::Kind::Obj) {
        return Result<std::string, Bail>::err({Bail::Reason::BadOperand, 0, "jclass 句柄不是对象"});
    }
    auto it = env.session->heap.find(v.value().ref.objId);
    if (it == env.session->heap.end()) {
        return Result<std::string, Bail>::err({Bail::Reason::BadOperand, 0, "jclass 堆对象缺失"});
    }
    return Result<std::string, Bail>::ok(it->second.data);
}

/// 数组类 → 元素字节宽([Z/[B=1,[C/[S=2,[I/[F=4,[J/[D=8)。
usize elemWidthOf(const std::string& arrayClass)
{
    if (arrayClass == "[J" || arrayClass == "[D") return 8;
    if (arrayClass == "[I" || arrayClass == "[F") return 4;
    if (arrayClass == "[C" || arrayClass == "[S") return 2;
    return 1;
}

bool isRefArrayClass(const std::string& arrayClass)
{
    return arrayClass.size() >= 2 && (arrayClass[1] == 'L' || arrayClass[1] == '[');
}

/// MUTF-8 → UTF-16 码元序列(BMP 家族口径,同 jsim-str;4 字节序列按坏形拒绝)。
Result<std::vector<u16>, Bail> mutf8ToUtf16(const std::string& bytes)
{
    std::vector<u16> out;
    usize i = 0;
    while (i < bytes.size()) {
        const u8 b = static_cast<u8>(bytes[i]);
        if ((b & 0x80) == 0) {
            out.push_back(b);
            i += 1;
        } else if ((b & 0xE0) == 0xC0) {
            if (i + 1 >= bytes.size() || (bytes[i + 1] & 0xC0) != 0x80) {
                return Result<std::vector<u16>, Bail>::err({Bail::Reason::BadOperand, 0, "MUTF-8 续字节缺失"});
            }
            out.push_back(static_cast<u16>(((b & 0x1F) << 6) | (bytes[i + 1] & 0x3F)));
            i += 2;
        } else if ((b & 0xF0) == 0xE0) {
            if (i + 2 >= bytes.size() || (bytes[i + 1] & 0xC0) != 0x80 || (bytes[i + 2] & 0xC0) != 0x80) {
                return Result<std::vector<u16>, Bail>::err({Bail::Reason::BadOperand, 0, "MUTF-8 续字节缺失"});
            }
            out.push_back(static_cast<u16>(((b & 0x0F) << 12) | ((bytes[i + 1] & 0x3F) << 6) | (bytes[i + 2] & 0x3F)));
            i += 3;
        } else {
            return Result<std::vector<u16>, Bail>::err({Bail::Reason::BadOperand, 0, "MUTF-8 不含 4 字节形"});
        }
    }
    return Result<std::vector<u16>, Bail>::ok(std::move(out));
}

/// UTF-16 码元 → MUTF-8(NUL 编成 C0 80,代理对按两个 3 字节序列原样编)。
std::string utf16ToMutf8(const std::vector<u16>& units)
{
    std::string out;
    for (u16 u : units) {
        if (u == 0) {
            out.push_back(static_cast<char>(0xC0));
            out.push_back(static_cast<char>(0x80));
        } else if (u < 0x80) {
            out.push_back(static_cast<char>(u));
        } else if (u < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (u >> 6)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (u >> 12)));
            out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
    return out;
}

/// 位型 → SimValue;引用走句柄池(0 按 null)。Unknown 无法过 JNI 边界,
/// 由调用方在编组前把「不具体」折成 Bail。
Result<SimValue, Bail> decodeByValType(ValType t, u64 bits, JniEnv& env)
{
    switch (t) {
    case ValType::I32: return Result<SimValue, Bail>::ok(SimValue::knownI32(static_cast<i32>(bits)));
    case ValType::I64: return Result<SimValue, Bail>::ok(SimValue::knownI64(static_cast<i64>(bits)));
    case ValType::F32: return Result<SimValue, Bail>::ok(SimValue::knownF32(bitsToFloat(static_cast<u32>(bits))));
    case ValType::F64: return Result<SimValue, Bail>::ok(SimValue::knownF64(bitsToDouble(bits)));
    case ValType::Ref: return env.pool.deref(bits);
    }
    return Result<SimValue, Bail>::err({Bail::Reason::BadOperand, 0, "非法值类型"});
}

/// SimValue → 位型;引用发 local 句柄(null 编 0),不具体即 Bail{NotConcrete}。
Result<u64, Bail> encodeByValType(const SimValue& v, JniEnv& env)
{
    if (!v.known) {
        return Result<u64, Bail>::err({Bail::Reason::NotConcrete, 0, "过 JNI 边界的值不具体"});
    }
    switch (v.type) {
    case ValType::I32: return Result<u64, Bail>::ok(v.asU32());
    case ValType::I64: return Result<u64, Bail>::ok(v.asU64());
    case ValType::F32: return Result<u64, Bail>::ok(floatToBits(v.asF32()));
    case ValType::F64: return Result<u64, Bail>::ok(doubleToBits(v.asF64()));
    case ValType::Ref: {
        Result<u64, Bail> h = env.pool.makeRef(v, false);
        if (!h.isOk()) return Result<u64, Bail>::err(h.error());
        return Result<u64, Bail>::ok(h.value());
    }
    }
    return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "非法值类型"});
}

/// jvalue* / va_list 形的变长实参读取:每个 jvalue 占 8 字节,按 desc 序读位型。
/// I32 槽读低 32 位(高位是 ABI 垃圾,截掉)。
Result<std::vector<u64>, Bail> readJValueArray(JniEnv& env, u64 addr, const std::string& desc)
{
    Result<std::vector<ValType>, Bail> types = argTypesOfDesc(desc);
    if (!types.isOk()) return Result<std::vector<u64>, Bail>::err(types.error());
    std::vector<u64> bits;
    for (ValType t : types.value()) {
        u64 raw = 0;
        Result<void, Bail> r = env.mem.read(addr + bits.size() * 8, &raw, 8);
        if (!r.isOk()) return Result<std::vector<u64>, Bail>::err(r.error());
        bits.push_back(t == ValType::I32 ? (raw & 0xFFFFFFFFull) : raw);
    }
    return Result<std::vector<u64>, Bail>::ok(std::move(bits));
}

/// bare Call*Method 变体:变长实参在寄存器/栈上,按 desc 逐位取。env=0、接收者
/// 或 clazz=1、jmethodID=2,实参从位置 3 起;浮点实参走 XMM 位型。
Result<std::vector<u64>, Bail> collectBareArgs(JniCall& call, const std::string& desc)
{
    Result<std::vector<ValType>, Bail> types = argTypesOfDesc(desc);
    if (!types.isOk()) return Result<std::vector<u64>, Bail>::err(types.error());
    std::vector<u64> bits;
    usize pos = 3;
    for (ValType t : types.value()) {
        if (t == ValType::F32 || t == ValType::F64) {
            Result<u64, Bail> v = call.argF(pos++);
            if (!v.isOk()) return Result<std::vector<u64>, Bail>::err(v.error());
            bits.push_back(t == ValType::F32 ? (v.value() & 0xFFFFFFFFull) : v.value());
        } else {
            Result<u64, Bail> v = call.argI(pos++);
            if (!v.isOk()) return Result<std::vector<u64>, Bail>::err(v.error());
            bits.push_back(t == ValType::I32 ? (v.value() & 0xFFFFFFFFull) : v.value());
        }
    }
    return Result<std::vector<u64>, Bail>::ok(std::move(bits));
}

/// 位型序列 → SimValue 序列(按 desc)。
Result<std::vector<SimValue>, Bail> decodeArgs(const std::string& desc, const std::vector<u64>& bits,
                                                      JniEnv& env)
{
    Result<std::vector<ValType>, Bail> types = argTypesOfDesc(desc);
    if (!types.isOk()) return Result<std::vector<SimValue>, Bail>::err(types.error());
    if (types.value().size() != bits.size()) {
        return Result<std::vector<SimValue>, Bail>::err({Bail::Reason::BadOperand, 0, "实参数与描述符不符"});
    }
    std::vector<SimValue> out;
    for (usize i = 0; i < types.value().size(); ++i) {
        Result<SimValue, Bail> v = decodeByValType(types.value()[i], bits[i], env);
        if (!v.isOk()) return Result<std::vector<SimValue>, Bail>::err(v.error());
        out.push_back(std::move(v.value()));
    }
    return Result<std::vector<SimValue>, Bail>::ok(std::move(out));
}

/// 字符串句柄 → Str 载荷拷贝。非字符串引用一律 Bail,不静默当空串。
/// 按值返回:deref 给的是 SimValue 拷贝,取其内嵌字符串的地址会悬空。
Result<std::string, Bail> strPayloadOf(JniEnv& env, u64 h)
{
    Result<SimValue, Bail> v = env.pool.deref(h);
    if (!v.isOk()) return Result<std::string, Bail>::err(v.error());
    if (v.value().ref.kind != RefPayload::Kind::Str) {
        return Result<std::string, Bail>::err({Bail::Reason::BadOperand, 0, "jstring 句柄不是字符串"});
    }
    return Result<std::string, Bail>::ok(v.value().ref.str);
}

/// 在 scratch 物化一段字节,返回 guest 指针。
Result<u64, Bail> materialize(JniEnv& env, const void* p, usize n, usize align)
{
    if (env.mem.map == nullptr) {
        return Result<u64, Bail>::err({Bail::Reason::UnsupportedCall, 0, "GuestMemory.map 未接"});
    }
    Result<std::pair<u64, usize>, Bail> m = env.mem.map(n, align);
    if (!m.isOk()) return Result<u64, Bail>::err(m.error());
    Result<void, Bail> w = env.mem.write(m.value().first, p, n);
    if (!w.isOk()) return Result<u64, Bail>::err(w.error());
    return Result<u64, Bail>::ok(m.value().first);
}

/// 写一个 jboolean 到 isCopy 出参。
void markCopy(JniEnv& env, u64 isCopyAddr)
{
    if (isCopyAddr != 0) {
        const u8 one = 1;
        env.mem.write(isCopyAddr, &one, 1);
    }
}

} // namespace detail

} // namespace jsim::jni
