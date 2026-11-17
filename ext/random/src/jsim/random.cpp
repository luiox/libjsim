#include "jsim/random.hpp"

#include <memory>

namespace jsim::random {
namespace {

inline constexpr char className[] = "java/util/Random";
inline constexpr char seedSlot[] = "@seed";
inline constexpr u64 multiplier = 0x5DEECE66Dull;
inline constexpr u64 mask = (1ull << 48) - 1;

using ValueResult = Result<SimValue, Bail>;
using ObjectResult = Result<HeapObj*, Bail>;

inline ObjectResult findObject(const Frame& frame, const SimValue& ref, const char* expectedClass) {
    if (ref.type == ValType::Ref && !ref.known) {
        return ObjectResult::err({Bail::Reason::NotConcrete, frame.pc(), "Random 对象身份未知"});
    }
    if (ref.type == ValType::Ref && ref.ref.kind == RefPayload::Kind::Obj) {
        auto& heap = frame.session().heap;
        auto it = heap.find(ref.ref.objId);
        if (it != heap.end() && it->second.className == expectedClass) {
            return ObjectResult::ok(&it->second);
        }
    }
    return ObjectResult::err({Bail::Reason::BadOperand, frame.pc(),
                              std::string("Random 需要对象类型 ") + expectedClass});
}

inline ObjectResult popObject(Frame& frame, const char* expectedClass) {
    auto value = frame.popChecked();
    if (!value.isOk()) return ObjectResult::err(value.error());
    return findObject(frame, value.value(), expectedClass);
}

inline SimValue seedValue(const std::optional<i64>& seed) {
    if (!seed) return SimValue::unknownOf(ValType::I64);
    return SimValue::knownI64(static_cast<i64>((static_cast<u64>(*seed) ^ multiplier) & mask));
}

inline u32 next(u64& seed, unsigned bits) {
    seed = (seed * multiplier + 11) & mask;
    return static_cast<u32>(seed >> (48 - bits));
}

inline ValueResult setSeed(Frame& frame) {
    auto arg = frame.popChecked();
    if (!arg.isOk()) return arg;
    if (arg.value().type != ValType::I64) {
        return ValueResult::err({Bail::Reason::BadOperand, frame.pc(), "Random 种子需要 long"});
    }
    auto recv = popObject(frame, className);
    if (!recv.isOk()) return ValueResult::err(recv.error());
    std::optional<i64> seed;
    if (arg.value().known) seed = arg.value().intBits;
    recv.value()->fields[seedSlot] = seedValue(seed);
    return ValueResult::ok({});
}

template <typename Draw>
ValueResult generate(Frame& frame, ValType type, Draw draw) {
    auto recv = popObject(frame, className);
    if (!recv.isOk()) return ValueResult::err(recv.error());
    auto& fields = recv.value()->fields;
    auto it = fields.find(seedSlot);
    if (it == fields.end()) {
        return ValueResult::err({Bail::Reason::BadOperand, frame.pc(), "Random 未初始化"});
    }
    if (!it->second.known) return ValueResult::ok(SimValue::unknownOf(type));
    u64 seed = it->second.asU64();
    SimValue value = draw(seed);
    it->second = SimValue::knownI64(static_cast<i64>(seed));
    return ValueResult::ok(std::move(value));
}

inline ValueResult nextIntBounded(Frame& frame) {
    auto arg = frame.popChecked();
    if (!arg.isOk()) return arg;
    if (arg.value().type != ValType::I32) {
        return ValueResult::err({Bail::Reason::BadOperand, frame.pc(), "nextInt 上界需要 int"});
    }
    if (!arg.value().known) {
        return ValueResult::err({Bail::Reason::NotConcrete, frame.pc(), "nextInt 上界未知"});
    }
    const i32 bound = static_cast<i32>(arg.value().intBits);
    if (bound <= 0) {
        return ValueResult::err({Bail::Reason::BadOperand, frame.pc(), "nextInt 上界必须为正"});
    }
    return generate(frame, ValType::I32, [bound](u64& seed) {
        u32 bits = next(seed, 31);
        if ((bound & (bound - 1)) == 0) {
            return SimValue::knownI32(static_cast<i32>((static_cast<u64>(bound) * bits) >> 31));
        }
        const u32 range = static_cast<u32>(bound);
        u32 value = bits % range;
        // 在 u64 中检查 Java 有符号加法是否会回绕到负数。
        while (static_cast<u64>(bits - value) + range > (1ull << 31)) {
            bits = next(seed, 31);
            value = bits % range;
        }
        return SimValue::knownI32(static_cast<i32>(value));
    });
}

inline ValueResult nextBytes(Frame& frame) {
    auto array = popObject(frame, "[B");
    if (!array.isOk()) return ValueResult::err(array.error());
    auto result = generate(frame, ValType::I32, [&elems = array.value()->elems](u64& seed) {
        for (usize i = 0; i < elems.size();) {
            u32 word = next(seed, 32);
            for (unsigned j = 0; j < 4 && i < elems.size(); ++j, ++i) {
                const i32 byte = static_cast<i32>(word & 0xff);
                elems[i] = byte < 128 ? byte : byte - 256;
                word >>= 8;
            }
        }
        return SimValue::knownI32(0);
    });
    if (result.isOk() && !result.value().known && !array.value()->elems.empty()) {
        return ValueResult::err({Bail::Reason::NotConcrete, frame.pc(),
                                 "Random 种子未知，无法写入 byte[]"});
    }
    return result;
}

} // namespace

void install(Session& session, SeedProvider defaultSeed) {
    session.allocScaffolds[className] = [] { return std::string{}; };
    auto& mocks = session.callMocks;
    // CallMock 在派发时会被复制，种子 provider 可以保留跨调用状态。
    const auto provider = std::make_shared<SeedProvider>(std::move(defaultSeed));
    mocks["java/util/Random.<init>()V"] = [provider](Frame& frame) -> ValueResult {
        auto receiver = frame.peekChecked();
        if (!receiver.isOk()) return receiver;
        auto checked = findObject(frame, receiver.value(), className);
        if (!checked.isOk()) return ValueResult::err(checked.error());
        const auto seed = *provider ? (*provider)(frame) : std::nullopt;
        // Provider 可以修改 Session。回调后重新定位接收者再写入种子。
        auto recv = popObject(frame, className);
        if (!recv.isOk()) return ValueResult::err(recv.error());
        recv.value()->fields[seedSlot] = seedValue(seed);
        return ValueResult::ok({});
    };
    mocks["java/util/Random.<init>(J)V"] = setSeed;
    mocks["java/util/Random.setSeed(J)V"] = setSeed;
    mocks["java/util/Random.nextInt(I)I"] = nextIntBounded;
    mocks["java/util/Random.nextBytes([B)V"] = nextBytes;
    mocks["java/util/Random.nextInt()I"] = [](Frame& frame) {
        return generate(frame, ValType::I32, [](u64& seed) {
            return SimValue::knownI32(static_cast<i32>(next(seed, 32)));
        });
    };
    mocks["java/util/Random.nextLong()J"] = [](Frame& frame) {
        return generate(frame, ValType::I64, [](u64& seed) {
            const u64 high = static_cast<u64>(next(seed, 32)) << 32;
            const i64 low = static_cast<i32>(next(seed, 32));
            return SimValue::knownI64(static_cast<i64>(high + static_cast<u64>(low)));
        });
    };
    mocks["java/util/Random.nextBoolean()Z"] = [](Frame& frame) {
        return generate(frame, ValType::I32, [](u64& seed) {
            return SimValue::knownI32(static_cast<i32>(next(seed, 1)));
        });
    };
    mocks["java/util/Random.nextFloat()F"] = [](Frame& frame) {
        return generate(frame, ValType::F32, [](u64& seed) {
            return SimValue::knownF32(static_cast<float>(next(seed, 24)) * 0x1p-24f);
        });
    };
    mocks["java/util/Random.nextDouble()D"] = [](Frame& frame) {
        return generate(frame, ValType::F64, [](u64& seed) {
            const u64 high = static_cast<u64>(next(seed, 26)) << 27;
            return SimValue::knownF64(static_cast<double>(high + next(seed, 27)) * 0x1p-53);
        });
    };
}

} // namespace jsim::random
