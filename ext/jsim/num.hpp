/// @file num.hpp
/// @brief jsim-num 扩展包：包装类型（Integer/Long）与十进制解析。
///
/// 这些是 JDK 语义，不在核心（设计 D10）。混淆器的元数据契约里 `Integer`
/// 常被当成「把 int 藏进 Object」的载体，解析失败还要抛 NumberFormatException，
/// 所以本包对解析失败明确 Bail{NotConcrete}——异常派发 v1 不做，不编造 0。
///
///   jsim::num::install(session);

#pragma once

#include <string>

#include "jsim/engine.hpp"

namespace jsim {
namespace num {

namespace detail {

/// 十进制解析（可带正负号）。越界按 JDK parseInt/parseLong 口径算解析失败
/// （真机抛 NumberFormatException，本包按契约 Bail），不回绕、不饱和——回绕是
/// 算术口径，不是解析口径。
inline bool parseDecimal(const std::string& text, u64 posLimit, u64 negLimit, i64& out) {
    usize i = 0;
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        negative = text[i] == '-';
        ++i;
    }
    if (i >= text.size()) return false;
    u64 acc = 0;
    for (; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch < '0' || ch > '9') return false;
        const u64 digit = static_cast<u64>(ch - '0');
        if (acc > (UINT64_MAX - digit) / 10) return false; // 位数溢出
        acc = acc * 10 + digit;
    }
    if (negative ? acc > negLimit : acc > posLimit) return false;
    out = static_cast<i64>(negative ? 0u - acc : acc);
    return true;
}

/// 包装对象：值挂在 "@v" 槽（与注解代理同一套载荷口径，无需新增字段面）。
inline SimValue boxed(Session& session, const char* className, SimValue value) {
    const u64 id = session.nextAllocId++;
    HeapObj obj;
    obj.className = className;
    obj.fields["@v"] = std::move(value);
    session.heap[id] = std::move(obj);
    return SimValue::knownObj(id);
}

/// 拆箱：取 "@v"；接收者未知或不是包装对象一律按类型传播 Unknown。
inline Result<SimValue, Bail> unbox(Frame& frame, ValType type) {
    Result<SimValue, Bail> recv = frame.popChecked();
    if (!recv.isOk()) return Result<SimValue, Bail>::err(recv.error());
    if (!recv.value().known || recv.value().ref.kind != RefPayload::Kind::Obj) {
        return Result<SimValue, Bail>::ok(SimValue::unknownOf(type));
    }
    Session& session = frame.session();
    auto it = session.heap.find(recv.value().ref.objId);
    if (it == session.heap.end()) {
        return Result<SimValue, Bail>::ok(SimValue::unknownOf(type));
    }
    auto slot = it->second.fields.find("@v");
    if (slot == it->second.fields.end()) {
        return Result<SimValue, Bail>::ok(SimValue::unknownOf(type));
    }
    SimValue v = slot->second;
    v.type = type;
    return Result<SimValue, Bail>::ok(v);
}

} // namespace detail

inline void install(Session& session) {
    session.callMocks["java/lang/Integer.parseInt(Ljava/lang/String;)I"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> arg = frame.popChecked();
        if (!arg.isOk()) return Result<SimValue, Bail>::err(arg.error());
        if (!arg.value().known || arg.value().ref.kind != RefPayload::Kind::Str) {
            return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::I32));
        }
        i64 parsed = 0;
        if (!detail::parseDecimal(arg.value().ref.str, 2147483647u, 2147483648ull, parsed)) {
            return Result<SimValue, Bail>::err(
                {Bail::Reason::NotConcrete, frame.pc(),
                 "Integer.parseInt 失败或越界需 NumberFormatException 派发（v1 不做）"});
        }
        return Result<SimValue, Bail>::ok(SimValue::knownI32(static_cast<i32>(parsed)));
    };

    session.callMocks["java/lang/Long.parseLong(Ljava/lang/String;)J"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> arg = frame.popChecked();
        if (!arg.isOk()) return Result<SimValue, Bail>::err(arg.error());
        if (!arg.value().known || arg.value().ref.kind != RefPayload::Kind::Str) {
            return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::I64));
        }
        i64 parsed = 0;
        if (!detail::parseDecimal(arg.value().ref.str, UINT64_C(9223372036854775807),
                                  UINT64_C(1) << 63, parsed)) {
            return Result<SimValue, Bail>::err(
                {Bail::Reason::NotConcrete, frame.pc(),
                 "Long.parseLong 失败或越界需 NumberFormatException 派发（v1 不做）"});
        }
        return Result<SimValue, Bail>::ok(SimValue::knownI64(parsed));
    };

    session.callMocks["java/lang/Integer.valueOf(I)Ljava/lang/Integer;"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> arg = frame.popChecked();
        if (!arg.isOk()) return Result<SimValue, Bail>::err(arg.error());
        if (!arg.value().known) {
            return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::Ref));
        }
        return Result<SimValue, Bail>::ok(detail::boxed(frame.session(), "java/lang/Integer",
                                                        SimValue::knownI32(
                                                            static_cast<i32>(arg.value().asU32()))));
    };

    session.callMocks["java/lang/Long.valueOf(J)Ljava/lang/Long;"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> arg = frame.popChecked();
        if (!arg.isOk()) return Result<SimValue, Bail>::err(arg.error());
        if (!arg.value().known) {
            return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::Ref));
        }
        return Result<SimValue, Bail>::ok(detail::boxed(frame.session(), "java/lang/Long",
                                                        SimValue::knownI64(
                                                            static_cast<i64>(arg.value().asU64()))));
    };

    session.callMocks["java/lang/Integer.intValue()I"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        return detail::unbox(frame, ValType::I32);
    };

    session.callMocks["java/lang/Long.longValue()J"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        return detail::unbox(frame, ValType::I64);
    };

    session.callMocks["java/lang/Integer.toString(I)Ljava/lang/String;"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> arg = frame.popChecked();
        if (!arg.isOk()) return Result<SimValue, Bail>::err(arg.error());
        if (!arg.value().known) {
            return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::Ref));
        }
        return Result<SimValue, Bail>::ok(
            SimValue::knownStr(std::to_string(static_cast<i32>(arg.value().asU32()))));
    };
}

} // namespace num
} // namespace jsim
