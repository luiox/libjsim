/// @file str.hpp
/// @brief jsim-str 扩展包：String 家族与 StringBuilder 的调用接管、分配脚手架。
///        扩展包与用户代码走同一注册面，核心零依赖本文件（设计 §7/D10）。

#pragma once

#include <string>

#include "jsim/engine.hpp"

namespace jsim {
namespace str {

/// 码元编码为 UTF-8 字节。MVP 以 UTF-8 码位模拟 BMP 码元；正式版接 libjstr
/// 的 MUTF-8 家族。
inline void appendCodeUnit(std::string& buffer, i32 codeUnit) {
    u32 c = static_cast<u32>(codeUnit);
    if (c < 0x80) {
        buffer += static_cast<char>(c);
    } else if (c < 0x800) {
        buffer += static_cast<char>(0xC0 | (c >> 6));
        buffer += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        buffer += static_cast<char>(0xE0 | (c >> 12));
        buffer += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        buffer += static_cast<char>(0x80 | (c & 0x3F));
    }
}

/// 码元总数（UTF-8 前导字节计数）。
inline usize codeUnitCount(const std::string& buffer) {
    usize count = 0;
    for (unsigned char ch : buffer) {
        if ((ch & 0xC0) != 0x80) ++count;
    }
    return count;
}

/// 取第 index 个码元（0 基）；越界或坏编码返回 -1。
inline i32 codeUnitAt(const std::string& buffer, usize index) {
    usize i = 0;
    usize seen = 0;
    while (i < buffer.size()) {
        unsigned char lead = static_cast<unsigned char>(buffer[i]);
        usize len = 1;
        if ((lead & 0xE0) == 0xC0) {
            len = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            len = 3;
        } else if ((lead & 0x80) != 0) {
            return -1;
        }
        if (seen == index) {
            if (len == 1) return static_cast<i32>(lead);
            if (len == 2) {
                return ((lead & 0x1F) << 6) | (buffer[i + 1] & 0x3F);
            }
            return ((lead & 0x0F) << 12) | ((buffer[i + 1] & 0x3F) << 6) | (buffer[i + 2] & 0x3F);
        }
        i += len;
        ++seen;
    }
    return -1;
}

/// 注册本包全部接管。profile 一行接入：jsim::str::install(session)。
inline void install(Session& session) {
    session.allocScaffolds["java/lang/StringBuilder"] = [] { return std::string(); };

    session.callMocks["java/lang/StringBuilder.<init>()V"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> recv = frame.popChecked();
        if (!recv.isOk()) return Result<SimValue, Bail>::err(recv.error());
        auto it = frame.session().heap.find(recv.value().ref.objId);
        if (it == frame.session().heap.end()) {
            return Result<SimValue, Bail>::err(
                {Bail::Reason::StackBroken, frame.pc(), "构造器：堆对象缺失"});
        }
        it->second.data.clear();
        // hasReturn=false，引擎不压返回值。
        return Result<SimValue, Bail>::ok(SimValue::knownObj(recv.value().ref.objId));
    };

    session.callMocks["java/lang/StringBuilder.append(C)Ljava/lang/StringBuilder;"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> value = frame.popChecked();
        if (!value.isOk()) return Result<SimValue, Bail>::err(value.error());
        Result<SimValue, Bail> recv = frame.popChecked();
        if (!recv.isOk()) return Result<SimValue, Bail>::err(recv.error());
        if (!value.value().known || !recv.value().known) {
            return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::Ref));
        }
        auto it = frame.session().heap.find(recv.value().ref.objId);
        if (it == frame.session().heap.end()) {
            return Result<SimValue, Bail>::err(
                {Bail::Reason::StackBroken, frame.pc(), "append：堆对象缺失"});
        }
        appendCodeUnit(it->second.data, static_cast<i32>(value.value().intBits));
        return Result<SimValue, Bail>::ok(SimValue::knownObj(recv.value().ref.objId));
    };

    session.callMocks["java/lang/StringBuilder.toString()Ljava/lang/String;"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> recv = frame.popChecked();
        if (!recv.isOk()) return Result<SimValue, Bail>::err(recv.error());
        if (!recv.value().known) {
            return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::Ref));
        }
        auto it = frame.session().heap.find(recv.value().ref.objId);
        if (it == frame.session().heap.end()) {
            return Result<SimValue, Bail>::err(
                {Bail::Reason::StackBroken, frame.pc(), "toString：堆对象缺失"});
        }
        return Result<SimValue, Bail>::ok(SimValue::knownStr(it->second.data));
    };

    session.callMocks["java/lang/String.charAt(I)C"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> index = frame.popChecked();
        if (!index.isOk()) return Result<SimValue, Bail>::err(index.error());
        Result<SimValue, Bail> recv = frame.popChecked();
        if (!recv.isOk()) return Result<SimValue, Bail>::err(recv.error());
        if (!index.value().known || !recv.value().known ||
            recv.value().ref.kind != RefPayload::Kind::Str) {
            return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::I32));
        }
        i32 codeUnit = codeUnitAt(recv.value().ref.str,
                                  static_cast<usize>(static_cast<i32>(index.value().intBits)));
        if (codeUnit < 0) {
            return Result<SimValue, Bail>::err(
                {Bail::Reason::BadOperand, frame.pc(), "charAt 越界"});
        }
        return Result<SimValue, Bail>::ok(SimValue::knownI32(codeUnit));
    };

    session.callMocks["java/lang/String.length()I"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> recv = frame.popChecked();
        if (!recv.isOk()) return Result<SimValue, Bail>::err(recv.error());
        if (!recv.value().known || recv.value().ref.kind != RefPayload::Kind::Str) {
            return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::I32));
        }
        return Result<SimValue, Bail>::ok(
            SimValue::knownI32(static_cast<i32>(codeUnitCount(recv.value().ref.str))));
    };
}

} // namespace str
} // namespace jsim
