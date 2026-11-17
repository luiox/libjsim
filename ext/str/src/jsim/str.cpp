#include "jsim/str.hpp"

#include <algorithm>
#include <optional>
#include <string_view>

namespace jsim::str {
namespace {

using ValueResult = Result<SimValue, Bail>;
using TextResult = Result<std::optional<std::string_view>, Bail>;

// Class 文件带来 MUTF-8，调用方也可输入 UTF-8。两者解码到同一组 UTF-16 码元。
// 允许 MUTF-8 的 NUL 与独立代理项。四字节 UTF-8 补充字符展开成代理对。
template <typename Visit>
bool visitCodeUnits(std::string_view text, Visit visit) {
    for (usize pos = 0; pos < text.size();) {
        const u8 lead = static_cast<u8>(text[pos++]);
        if (lead < 0x80) {
            if (!visit(static_cast<char16_t>(lead))) return true;
            continue;
        }

        usize tail;
        u32 point;
        u32 minimum;
        if (lead < 0xC0 || lead > 0xF4) return false;
        if (lead < 0xE0) {
            tail = 1;
            point = lead & 0x1F;
            minimum = 0x80;
        } else if (lead < 0xF0) {
            tail = 2;
            point = lead & 0x0F;
            minimum = 0x800;
        } else {
            tail = 3;
            point = lead & 0x07;
            minimum = 0x10000;
        }
        if (tail > text.size() - pos) return false;
        const bool modifiedNul = lead == 0xC0 && static_cast<u8>(text[pos]) == 0x80;
        for (usize i = 0; i < tail; ++i) {
            const u8 next = static_cast<u8>(text[pos++]);
            if ((next & 0xC0) != 0x80) return false;
            point = (point << 6) | (next & 0x3F);
        }
        if ((point < minimum && !modifiedNul) || point > 0x10FFFF) return false;
        if (point > 0xFFFF) {
            point -= 0x10000;
            if (!visit(static_cast<char16_t>(0xD800 + (point >> 10)))) return true;
            point = 0xDC00 + (point & 0x3FF);
        }
        if (!visit(static_cast<char16_t>(point))) return true;
    }
    return true;
}

bool decode(std::string_view text, std::u16string& units) {
    return visitCodeUnits(text, [&units](char16_t unit) {
        units.push_back(unit);
        return true;
    });
}

i32 unitAt(std::string_view text, usize index) {
    i32 found = -1;
    visitCodeUnits(text, [&](char16_t unit) {
        if (index != 0) {
            --index;
            return true;
        }
        found = unit;
        return false;
    });
    return found;
}

Bail badOperand(const Frame& frame, const char* message) {
    return {Bail::Reason::BadOperand, frame.pc(), message};
}

ValueResult wrongEncoding(const Frame& frame) {
    return ValueResult::err(badOperand(frame, "String 编码无效"));
}

ValueResult unknown(ValType type) {
    return ValueResult::ok(SimValue::unknownOf(type));
}

ValueResult popAs(Frame& frame, ValType type) {
    auto arg = frame.popChecked();
    if (arg.isOk() && arg.value().type != type) {
        return ValueResult::err(badOperand(frame, "String 接管参数槽类型不匹配"));
    }
    return arg;
}

HeapObj* objectOf(Frame& frame, const SimValue& ref) {
    if (ref.ref.kind != RefPayload::Kind::Obj) return nullptr;
    auto& heap = frame.session().heap;
    auto it = heap.find(ref.ref.objId);
    return it != heap.end() ? &it->second : nullptr;
}

TextResult textOf(Frame& frame, const SimValue& ref) {
    if (!ref.known) return TextResult::ok(std::nullopt);
    if (ref.ref.kind == RefPayload::Kind::Str) return TextResult::ok(ref.ref.str);
    const HeapObj* obj = objectOf(frame, ref);
    if (obj && obj->className == "java/lang/String") return TextResult::ok(obj->data);
    return TextResult::err(badOperand(frame, "需要有效的 java/lang/String 引用"));
}

template <typename Operation>
ValueResult withString(Frame& frame, ValType resultType, Operation operation) {
    auto receiver = popAs(frame, ValType::Ref);
    if (!receiver.isOk()) return receiver;
    auto text = textOf(frame, receiver.value());
    if (!text.isOk()) return ValueResult::err(text.error());
    if (!text.value()) return unknown(resultType);
    return operation(*text.value(), receiver.value());
}

ValueResult charAt(Frame& frame) {
    auto index = popAs(frame, ValType::I32);
    if (!index.isOk()) return index;
    return withString(frame, ValType::I32, [&](std::string_view text, const SimValue&) {
        if (!index.value().known) return unknown(ValType::I32);
        const i32 unit = index.value().intBits < 0 ? -1 : unitAt(text, index.value().asU32());
        if (unit < 0) {
            return ValueResult::err(badOperand(frame, "String.charAt 下标越界或编码无效"));
        }
        return ValueResult::ok(SimValue::knownI32(unit));
    });
}

ValueResult length(Frame& frame, bool emptyOnly) {
    return withString(frame, ValType::I32, [&](std::string_view text, const SimValue&) {
        usize units = 0;
        if (!visitCodeUnits(text, [&](char16_t) { ++units; return true; })) {
            return wrongEncoding(frame);
        }
        if (units > static_cast<usize>(INT32_MAX)) {
            return ValueResult::err(badOperand(frame, "字符串长度超过 Java int 范围"));
        }
        return ValueResult::ok(SimValue::knownI32(emptyOnly ? (units == 0) : static_cast<i32>(units)));
    });
}

ValueResult hashCode(Frame& frame) {
    return withString(frame, ValType::I32, [&](std::string_view text, const SimValue&) {
        u32 hash = 0;
        if (!visitCodeUnits(text, [&](char16_t unit) { hash = hash * 31 + unit; return true; })) {
            return wrongEncoding(frame);
        }
        return ValueResult::ok(SimValue::knownI32(static_cast<i32>(hash)));
    });
}

ValueResult equals(Frame& frame) {
    auto arg = popAs(frame, ValType::Ref);
    if (!arg.isOk()) return arg;
    return withString(frame, ValType::I32, [&](std::string_view text, const SimValue&) {
        const SimValue& other = arg.value();
        if (!other.known) return unknown(ValType::I32);
        const HeapObj* obj = objectOf(frame, other);
        if (other.ref.kind == RefPayload::Kind::Null || (obj && obj->className != "java/lang/String")) {
            return ValueResult::ok(SimValue::knownI32(0));
        }
        auto otherText = textOf(frame, other);
        if (!otherText.isOk()) return ValueResult::err(otherText.error());

        std::u16string lhs;
        std::u16string rhs;
        if (!decode(text, lhs) || !decode(*otherText.value(), rhs)) return wrongEncoding(frame);
        return ValueResult::ok(SimValue::knownI32(lhs == rhs));
    });
}

ValueResult substring(Frame& frame, bool hasEnd) {
    auto end = hasEnd ? popAs(frame, ValType::I32) : ValueResult::ok(SimValue::knownI32(0));
    if (!end.isOk()) return end;
    auto begin = popAs(frame, ValType::I32);
    if (!begin.isOk()) return begin;
    return withString(frame, ValType::Ref, [&](std::string_view text, const SimValue& receiver) {
        if (!begin.value().known || !end.value().known) return unknown(ValType::Ref);
        std::u16string units;
        if (!decode(text, units)) return wrongEncoding(frame);
        const usize first = begin.value().asU32();
        const usize last = hasEnd ? static_cast<usize>(end.value().asU32()) : units.size();
        if (begin.value().intBits < 0 || (hasEnd && end.value().intBits < 0) ||
            first > last || last > units.size()) {
            return ValueResult::err(badOperand(frame, "String.substring 区间越界"));
        }
        if (first == 0 && last == units.size()) return ValueResult::ok(receiver);
        std::string part;
        for (usize i = first; i < last; ++i) appendCodeUnit(part, units[i]);
        return ValueResult::ok(SimValue::knownStr(std::move(part)));
    });
}

ValueResult indexOf(Frame& frame, bool hasFrom) {
    auto from = hasFrom ? popAs(frame, ValType::I32) : ValueResult::ok(SimValue::knownI32(0));
    if (!from.isOk()) return from;
    auto needle = popAs(frame, ValType::Ref);
    if (!needle.isOk()) return needle;
    return withString(frame, ValType::I32, [&](std::string_view text, const SimValue&) {
        auto otherText = textOf(frame, needle.value());
        if (!otherText.isOk()) return ValueResult::err(otherText.error());
        if (!otherText.value() || !from.value().known) return unknown(ValType::I32);
        std::u16string haystack;
        std::u16string pattern;
        if (!decode(text, haystack) || !decode(*otherText.value(), pattern)) return wrongEncoding(frame);
        const usize start = from.value().intBits > 0 ? from.value().asU32() : 0;
        const usize match = haystack.find(pattern, (std::min)(start, haystack.size()));
        return ValueResult::ok(SimValue::knownI32(
            match == std::u16string::npos ? -1 : static_cast<i32>(match)));
    });
}

struct BufferRef {
    u64 id = 0;
    HeapObj* object = nullptr;
};

using BufferResult = Result<BufferRef, Bail>;

BufferResult takeBuffer(Frame& frame, const std::string& owner, bool writes) {
    auto recv = popAs(frame, ValType::Ref);
    if (!recv.isOk()) return BufferResult::err(recv.error());
    if (!recv.value().known) {
        if (!writes) return BufferResult::ok({});
        return BufferResult::err(
            {Bail::Reason::NotConcrete, frame.pc(), "字符串缓冲区接收者未知"});
    }
    HeapObj* obj = objectOf(frame, recv.value());
    if (!obj || obj->className != owner) {
        return BufferResult::err(badOperand(frame, "字符串缓冲区接收者无效"));
    }
    return BufferResult::ok({recv.value().ref.objId, obj});
}

void installBuilder(Session& session, const std::string& owner) {
    session.allocScaffolds[owner] = [] { return std::string{}; };

    session.callMocks[owner + ".<init>()V"] = [owner](Frame& frame) -> ValueResult {
        auto recv = takeBuffer(frame, owner, true);
        if (!recv.isOk()) return ValueResult::err(recv.error());
        recv.value().object->data.clear();
        return ValueResult::ok(SimValue::knownObj(recv.value().id));
    };

    session.callMocks[owner + ".append(C)L" + owner + ";"] = [owner](Frame& frame) -> ValueResult {
        auto value = popAs(frame, ValType::I32);
        if (!value.isOk()) return value;
        auto recv = takeBuffer(frame, owner, true);
        if (!recv.isOk()) return ValueResult::err(recv.error());
        if (!value.value().known) {
            return ValueResult::err({Bail::Reason::NotConcrete, frame.pc(), "字符串缓冲区写入不具体"});
        }
        appendCodeUnit(recv.value().object->data, static_cast<i32>(value.value().intBits));
        return ValueResult::ok(SimValue::knownObj(recv.value().id));
    };

    session.callMocks[owner + ".append(Ljava/lang/String;)L" + owner + ";"] =
        [owner](Frame& frame) -> ValueResult {
        auto value = popAs(frame, ValType::Ref);
        if (!value.isOk()) return value;
        auto recv = takeBuffer(frame, owner, true);
        if (!recv.isOk()) return ValueResult::err(recv.error());
        if (!value.value().known) {
            return ValueResult::err({Bail::Reason::NotConcrete, frame.pc(), "字符串缓冲区写入不具体"});
        }
        std::string& data = recv.value().object->data;
        if (value.value().ref.kind == RefPayload::Kind::Null) {
            data += "null";
        } else {
            auto text = textOf(frame, value.value());
            if (!text.isOk()) return ValueResult::err(text.error());
            if (!visitCodeUnits(*text.value(), [](char16_t) { return true; })) {
                return wrongEncoding(frame);
            }
            data += *text.value();
        }
        return ValueResult::ok(SimValue::knownObj(recv.value().id));
    };

    session.callMocks[owner + ".toString()Ljava/lang/String;"] = [owner](Frame& frame) {
        auto recv = takeBuffer(frame, owner, false);
        if (!recv.isOk()) return ValueResult::err(recv.error());
        if (!recv.value().object) return unknown(ValType::Ref);
        return ValueResult::ok(SimValue::knownStr(recv.value().object->data));
    };
}

} // namespace

void appendCodeUnit(std::string& buffer, i32 codeUnit) {
    const u32 c = static_cast<u32>(codeUnit) & 0xFFFF;
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

usize codeUnitCount(const std::string& buffer) {
    usize units = 0;
    visitCodeUnits(buffer, [&](char16_t) { ++units; return true; });
    return units;
}

i32 codeUnitAt(const std::string& buffer, usize index) {
    return unitAt(buffer, index);
}

void install(Session& session) {
    installBuilder(session, "java/lang/StringBuilder");
    installBuilder(session, "java/lang/StringBuffer");

    auto& mocks = session.callMocks;
    mocks["java/lang/String.charAt(I)C"] = charAt;
    mocks["java/lang/String.length()I"] = [](Frame& frame) { return length(frame, false); };
    mocks["java/lang/String.isEmpty()Z"] = [](Frame& frame) { return length(frame, true); };
    mocks["java/lang/String.hashCode()I"] = hashCode;
    mocks["java/lang/String.equals(Ljava/lang/Object;)Z"] = equals;
    mocks["java/lang/String.substring(I)Ljava/lang/String;"] =
        [](Frame& frame) { return substring(frame, false); };
    mocks["java/lang/String.substring(II)Ljava/lang/String;"] =
        [](Frame& frame) { return substring(frame, true); };
    mocks["java/lang/String.indexOf(Ljava/lang/String;)I"] =
        [](Frame& frame) { return indexOf(frame, false); };
    mocks["java/lang/String.indexOf(Ljava/lang/String;I)I"] =
        [](Frame& frame) { return indexOf(frame, true); };
}

void installStringAlloc(Session& session) {
    session.allocScaffolds["java/lang/String"] = [] { return std::string{}; };
    session.callMocks["java/lang/String.<init>([C)V"] = [](Frame& frame) -> ValueResult {
        auto value = popAs(frame, ValType::Ref);
        if (!value.isOk()) return value;
        auto recv = popAs(frame, ValType::Ref);
        if (!recv.isOk()) return recv;
        if (!value.value().known || !recv.value().known) {
            return ValueResult::err({Bail::Reason::NotConcrete, frame.pc(), "String 构造参数或接收者未知"});
        }
        const HeapObj* chars = objectOf(frame, value.value());
        HeapObj* obj = objectOf(frame, recv.value());
        if (!chars || chars->className != "[C" || !obj || obj->className != "java/lang/String") {
            return ValueResult::err(badOperand(frame, "String.<init> 需要 (String, char[])"));
        }
        std::string text;
        for (i64 unit : chars->elems) appendCodeUnit(text, static_cast<i32>(unit));
        obj->data = std::move(text);
        return recv;
    };
}

} // namespace jsim::str
