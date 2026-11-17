#include "detail/guest.hpp"

namespace jsim::jni {
namespace {

Result<std::vector<u16>, Bail> readStringUnits(JniCall& call)
{
    auto payload = detail::strPayloadOf(call.env, call.args[0]);
    if (!payload.isOk()) return Result<std::vector<u16>, Bail>::err(payload.error());
    return detail::mutf8ToUtf16(payload.value());
}

Result<std::vector<u16>, Bail> readStringRegion(JniCall& call)
{
    auto units = readStringUnits(call);
    if (!units.isOk()) return units;
    const i32 start = static_cast<i32>(call.args[1]);
    const i32 len = static_cast<i32>(call.args[2]);
    if (start < 0 || len < 0 || static_cast<usize>(start) > units.value().size() ||
        static_cast<usize>(len) > units.value().size() - static_cast<usize>(start)) {
        return Result<std::vector<u16>, Bail>::err(
            {Bail::Reason::BadOperand, 0, std::string(call.name) + " 区间越界"});
    }
    if (len == 0) return Result<std::vector<u16>, Bail>::ok({});
    const auto first = units.value().begin() + start;
    return Result<std::vector<u16>, Bail>::ok(std::vector<u16>(first, first + len));
}

std::vector<u8> utf16le(const std::vector<u16>& units)
{
    std::vector<u8> bytes;
    bytes.reserve(units.size() * 2);
    for (u16 unit : units) {
        bytes.push_back(static_cast<u8>(unit & 0xFF));
        bytes.push_back(static_cast<u8>(unit >> 8));
    }
    return bytes;
}

} // namespace

void detail::installStringHooks(JniEnv& env)
{
    putHook(env, "NewStringUTF", [](JniCall& call) -> Result<u64, Bail> {
        if (call.args[0] == 0) return Result<u64, Bail>::ok(0);
        Result<std::string, Bail> s = detail::readCStr(call.env, call.args[0]);
        if (!s.isOk()) return Result<u64, Bail>::err(s.error());
        return call.env.pool.makeRef(SimValue::knownStr(std::move(s.value())), false);
    });
    putHook(env, "GetStringUTFChars", [](JniCall& call) -> Result<u64, Bail> {
        if (call.args[0] == 0) return Result<u64, Bail>::ok(0);
        Result<std::string, Bail> s = strPayloadOf(call.env, call.args[0]);
        if (!s.isOk()) return Result<u64, Bail>::err(s.error());
        std::string buf = s.value();
        buf.push_back('\0'); // JNI 契约:NUL 结尾
        Result<u64, Bail> p = materialize(call.env, buf.data(), buf.size());
        if (!p.isOk()) return Result<u64, Bail>::err(p.error());
        detail::markCopy(call.env, call.args[1]);
        return Result<u64, Bail>::ok(p.value());
    });
    const auto release = [](JniCall& call) -> Result<u64, Bail> {
        if (call.args[1] != 0 && call.env.mem.free != nullptr) call.env.mem.free(call.args[1]);
        return Result<u64, Bail>::ok(0);
    };
    putHook(env, "ReleaseStringUTFChars", release);
    putHook(env, "GetStringUTFLength", [](JniCall& call) -> Result<u64, Bail> {
        Result<std::string, Bail> s = strPayloadOf(call.env, call.args[0]);
        if (!s.isOk()) return Result<u64, Bail>::err(s.error());
        return Result<u64, Bail>::ok(s.value().size()); // 不含结尾 NUL(对齐 JDK)
    });
    putHook(env, "NewString", [](JniCall& call) -> Result<u64, Bail> {
        const i32 length = static_cast<i32>(call.args[1]);
        if (length == 0) return call.env.pool.makeRef(SimValue::knownStr(""), false);
        if (length < 0 || call.args[0] == 0) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "NewString 需要有效的 UTF-16 输入"});
        }
        const usize len = static_cast<usize>(length);
        Result<std::vector<u8>, Bail> raw = detail::readBytes(call.env, call.args[0], len * 2);
        if (!raw.isOk()) return Result<u64, Bail>::err(raw.error());
        std::vector<u16> units(len);
        for (usize i = 0; i < len; ++i) {
            units[i] = static_cast<u16>(raw.value()[i * 2] | (raw.value()[i * 2 + 1] << 8)); // UTF-16LE
        }
        return call.env.pool.makeRef(SimValue::knownStr(detail::utf16ToMutf8(units)), false);
    });
    putHook(env, "GetStringLength", [](JniCall& call) -> Result<u64, Bail> {
        auto units = readStringUnits(call);
        if (!units.isOk()) return Result<u64, Bail>::err(units.error());
        return Result<u64, Bail>::ok(units.value().size());
    });
    const auto getChars = [](JniCall& call) -> Result<u64, Bail> {
        auto units = readStringUnits(call);
        if (!units.isOk()) return Result<u64, Bail>::err(units.error());
        units.value().push_back(0); // 空串也物化一个可释放的对齐块。
        const std::vector<u8> buf = utf16le(units.value());
        Result<u64, Bail> p = materialize(call.env, buf.data(), buf.size(), 2);
        if (!p.isOk()) return Result<u64, Bail>::err(p.error());
        detail::markCopy(call.env, call.args[1]);
        return Result<u64, Bail>::ok(p.value());
    };
    putHook(env, "GetStringChars", getChars);
    putHook(env, "ReleaseStringChars", release);
    putHook(env, "GetStringCritical", getChars);
    putHook(env, "ReleaseStringCritical", release);

    putHook(env, "GetStringUTFRegion", [](JniCall& call) -> Result<u64, Bail> {
        auto units = readStringRegion(call);
        if (!units.isOk()) return Result<u64, Bail>::err(units.error());
        if (units.value().empty()) return Result<u64, Bail>::ok(0);
        const std::string buf = detail::utf16ToMutf8(units.value());
        Result<void, Bail> w = detail::writeBytes(call.env, call.args[3], buf.data(), buf.size());
        if (!w.isOk()) return Result<u64, Bail>::err(w.error());
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "GetStringRegion", [](JniCall& call) -> Result<u64, Bail> {
        auto units = readStringRegion(call);
        if (!units.isOk()) return Result<u64, Bail>::err(units.error());
        if (units.value().empty()) return Result<u64, Bail>::ok(0);
        const std::vector<u8> buf = utf16le(units.value());
        Result<void, Bail> w = detail::writeBytes(call.env, call.args[3], buf.data(), buf.size());
        if (!w.isOk()) return Result<u64, Bail>::err(w.error());
        return Result<u64, Bail>::ok(0);
    });
}

} // namespace jsim::jni
