#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>

#include "jsim/math.hpp"
#include "jsim/random.hpp"
#include "jsim/stacktrace.hpp"

using namespace jsim;

namespace {

int total = 0;
int failed = 0;

void check(bool ok, const char* label) {
    ++total;
    if (!ok) {
        ++failed;
        std::printf("FAIL [jsim-jdk] %s\n", label);
    }
}

RunResult run(Session& session, const Method& method, std::initializer_list<SimValue> stack = {}) {
    Prefill prefill;
    prefill.stack = stack;
    return Sim(method, session).runRegion(0, std::move(prefill));
}

u8 returnOpcode(ValType type) {
    switch (type) {
    case ValType::I32: return op::IReturn;
    case ValType::I64: return op::LReturn;
    case ValType::F32: return op::FReturn;
    case ValType::F64: return op::DReturn;
    case ValType::Ref: return op::AReturn;
    }
    return op::Return;
}

void checkValue(const RunResult& result, const SimValue& expected, const char* label) {
    const auto& v = result.returnValue;
    bool same = result.kind == RunResult::Kind::Completed && result.hasReturnValue &&
                v.type == expected.type && v.known == expected.known;
    if (same && v.known) {
        switch (v.type) {
        case ValType::I32:
        case ValType::I64:
        case ValType::F32:
            same = v.intBits == expected.intBits;
            break;
        case ValType::F64:
            same = std::isnan(expected.asF64()) ? std::isnan(v.asF64())
                : doubleToBits(v.asF64()) == doubleToBits(expected.asF64());
            break;
        case ValType::Ref:
            same = v.ref.kind == expected.ref.kind && v.ref.str == expected.ref.str &&
                   v.ref.objId == expected.ref.objId;
            break;
        }
    }
    check(same, label);
}

void testMath() {
    Session session;
    math::install(session);
    const auto i = SimValue::knownI32;
    const auto j = SimValue::knownI64;
    const auto f = SimValue::knownF32;
    const auto d = SimValue::knownF64;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    auto call = [&](const char* name, const char* desc, std::initializer_list<SimValue> args,
                    SimValue expected, const char* owner = "java/lang/Math") {
        Method method = MethodBuilder("math", desc, 0)
            .invoke(owner, name, desc, expected.type)
            .raw(returnOpcode(expected.type)).build();
        checkValue(run(session, method, args), expected, (std::string(name) + desc).c_str());
    };
    call("abs", "(I)I", {i(INT32_MIN)}, i(INT32_MIN));
    call("abs", "(J)J", {j(INT64_MIN)}, j(INT64_MIN));
    call("abs", "(F)F", {f(-0.0f)}, f(0.0f));
    call("abs", "(D)D", {d(-inf)}, d(inf));
    call("min", "(II)I", {i(-3), i(2)}, i(-3));
    call("max", "(II)I", {i(-3), i(2)}, i(2));
    call("min", "(JJ)J", {j(INT64_MAX), j(-3)}, j(-3));
    call("max", "(JJ)J", {j(INT64_MAX), j(-3)}, j(INT64_MAX));
    call("min", "(FF)F", {f(0.0f), f(-0.0f)}, f(-0.0f));
    call("max", "(FF)F", {f(-0.0f), f(0.0f)}, f(0.0f));
    call("min", "(DD)D", {d(1), d(nan)}, d(nan));
    call("min", "(DD)D", {d(-0.0), d(0.0)}, d(-0.0));
    call("max", "(DD)D", {d(0.0), d(-0.0)}, d(0.0));
    call("max", "(DD)D", {d(nan), d(1)}, d(nan));
    call("signum", "(F)F", {f(-0.0f)}, f(-0.0f));
    call("signum", "(D)D", {d(-inf)}, d(-1));
    // 期望值经 HotSpot 21 的 Math.round 核对。
    call("round", "(F)I", {f(std::nextafter(0.5f, 0.0f))}, i(0));
    call("round", "(F)I", {f(-1.5f)}, i(-1));
    call("round", "(F)I", {f(2147483648.0f)}, i(INT32_MAX));
    call("round", "(D)J", {d(std::nextafter(0.5, 0.0))}, j(0));
    call("round", "(D)J", {d(4503599627370497.0)}, j(4503599627370497ll));
    call("round", "(D)J", {d(inf)}, j(INT64_MAX));
    call("round", "(D)J", {d(-inf)}, j(INT64_MIN));
    call("round", "(D)J", {d(nan)}, j(0));
    call("round", "(D)J", {SimValue::unknownOf(ValType::F64)}, SimValue::unknownOf(ValType::I64));
    call("floor", "(D)D", {d(-0.5)}, d(-1));
    call("ceil", "(D)D", {d(-0.5)}, d(-0.0));
    call("sqrt", "(D)D", {d(-0.0)}, d(-0.0));
    call("sqrt", "(D)D", {d(-1)}, d(nan));
    call("sqrt", "(D)D", {d(9)}, d(3));

    call("rint", "(D)D", {d(-0.5)}, d(-0.0));
    call("rint", "(D)D", {d(1.5)}, d(2.0));
    call("rint", "(D)D", {d(2.5)}, d(2.0), "java/lang/StrictMath");
    call("rint", "(D)D", {d(-3.5)}, d(-4.0));
    call("rint", "(D)D", {d(-inf)}, d(-inf));
    call("rint", "(D)D", {d(nan)}, d(nan));
    call("floorDiv", "(II)I", {i(4), i(-3)}, i(-2));
    call("floorDiv", "(JI)J", {j(-4), i(3)}, j(-2), "java/lang/StrictMath");
    call("floorDiv", "(JJ)J", {j(INT64_MIN), j(-1)}, j(INT64_MIN));
    call("floorMod", "(II)I", {i(4), i(-3)}, i(-2));
    call("floorMod", "(JI)I", {j(-4), i(3)}, i(2), "java/lang/StrictMath");
    call("floorMod", "(JJ)J", {j(INT64_MIN), j(-1)}, j(0));
    call("floorMod", "(JI)I", {j(-4), SimValue::unknownOf(ValType::I32)}, SimValue::unknownOf(ValType::I32));

    const Method zero = MethodBuilder("zero", "(II)I", 0)
        .invoke("java/lang/Math", "floorDiv", "(II)I", ValType::I32).ireturn().build();
    const RunResult divided = run(session, zero, {SimValue::unknownOf(ValType::I32), i(0)});
    check(divided.kind == RunResult::Kind::Bailed &&
          divided.bail.reason == Bail::Reason::BadOperand, "floorDiv by zero");
}

void testRandom() {
    Session session;
    random::install(session);
    const auto i = SimValue::knownI32;
    const auto j = SimValue::knownI64;
    const auto f = SimValue::knownF32;
    const auto d = SimValue::knownF64;
    auto construct = [&](bool seeded) {
        MethodBuilder builder("random", "()Ljava/util/Random;", 0);
        builder.newInst("java/util/Random").dup();
        if (seeded) builder.ldcLong(0);
        return run(session, builder.invokeSpecial("java/util/Random", "<init>",
            seeded ? "(J)V" : "()V").areturn().build());
    };
    const auto constructed = construct(true);
    check(constructed.kind == RunResult::Kind::Completed && constructed.returnValue.known,
          "Random(long)");
    SimValue recv = constructed.returnValue;
    auto method = [](const char* name, const char* desc, ValType type, bool returns = true) {
        return MethodBuilder("random", desc, 0)
            .invokeVirtual("java/util/Random", name, desc, type, returns)
            .raw(returns ? returnOpcode(type) : op::Return).build();
    };
    const Method nextInt = method("nextInt", "()I", ValType::I32);
    const Method bounded = method("nextInt", "(I)I", ValType::I32);
    const Method nextLong = method("nextLong", "()J", ValType::I64);
    const Method setSeed = method("setSeed", "(J)V", ValType::I64, false);

    // 用 HotSpot 21 的 Random(0L) 顺序调用相同方法生成期望序列。
    checkValue(run(session, nextInt, {recv}), i(-1155484576), "nextInt()");
    checkValue(run(session, bounded, {recv, i(16)}), i(13), "nextInt(16)");
    checkValue(run(session, bounded, {recv, i(1073741825)}), i(516548029), "nextInt non-power-of-two bound");
    checkValue(run(session, nextLong, {recv}), j(-7261648964369397258ll), "nextLong()");
    checkValue(run(session, method("nextBoolean", "()Z", ValType::I32), {recv}), i(0), "nextBoolean()");
    checkValue(run(session, method("nextFloat", "()F", ValType::F32), {recv}),
               f(bitsToFloat(0x3f0ce970)), "nextFloat()");
    checkValue(run(session, method("nextDouble", "()D", ValType::F64), {recv}),
               d(bitsToDouble(0x3fbdf425263e2e88ull)), "nextDouble()");

    const Method bytes = MethodBuilder("bytes", "()[B", 1)
        .iconst(7).newArray(8).astore(0).aload(0)
        .invokeVirtual("java/util/Random", "nextBytes", "([B)V", ValType::Ref, false)
        .aload(0).areturn().build();
    auto filled = run(session, bytes, {recv});
    auto obj = session.heap.find(filled.returnValue.ref.objId);
    check(filled.kind == RunResult::Kind::Completed && obj != session.heap.end() &&
          obj->second.elems == std::vector<i64>({109, -89, 18, -56, 43, -51, 77}), "nextBytes");
    checkValue(run(session, nextInt, {recv}), i(1085665355), "nextInt after nextBytes");
    check(run(session, bounded, {recv, i(0)}).bail.reason == Bail::Reason::BadOperand, "zero bound");
    check(run(session, bounded, {recv, SimValue::unknownOf(ValType::I32)}).bail.reason ==
          Bail::Reason::NotConcrete, "unknown bound");
    check(run(session, setSeed, {recv, j(-1)}).kind == RunResult::Kind::Completed, "setSeed");
    checkValue(run(session, nextLong, {recv}), j(4961115982468162243ll), "nextLong after setSeed");
    const auto independent = construct(true);
    check(independent.returnValue.ref.objId != recv.ref.objId, "distinct Random objects");
    checkValue(run(session, nextLong, {independent.returnValue}), j(-4962768465676381896ll),
               "nextLong uses signed low word and independent seed");
    const auto rejection = construct(true);
    // 以同一上界从种子 0 直接抽取，前两次 31 位抽样都会被拒绝。
    checkValue(run(session, bounded, {rejection.returnValue, i(1073741825)}), i(516548029),
               "nextInt rejects out-of-range draws");
    recv = construct(false).returnValue;
    checkValue(run(session, nextInt, {recv}), SimValue::unknownOf(ValType::I32), "no seed provider");
    const auto unknownBytes = run(session, bytes, {recv});
    check(unknownBytes.kind == RunResult::Kind::Bailed &&
          unknownBytes.bail.reason == Bail::Reason::NotConcrete, "nextBytes with unknown seed");
    check(run(session, setSeed, {recv, j(0)}).kind == RunResult::Kind::Completed, "set known seed");
    checkValue(run(session, nextInt, {recv}), i(-1155484576), "known seed recovers");
    random::install(session, [seed = i64{0}](const Frame&) mutable {
        return std::optional<i64>{seed++};
    });
    recv = construct(false).returnValue;
    checkValue(run(session, nextInt, {recv}), i(-1155484576), "no-arg seed provider");
    recv = construct(false).returnValue;
    checkValue(run(session, nextInt, {recv}), i(-1155869325), "seed provider retains state");

    random::install(session, [](const Frame& frame) {
        frame.session().heap.erase(frame.stack().back().ref.objId);
        return std::optional<i64>{0};
    });
    const auto removed = construct(false);
    check(removed.kind == RunResult::Kind::Bailed && removed.bail.reason == Bail::Reason::BadOperand,
          "seed provider invalidated receiver");
}

void testStackTrace() {
    Session session;
    using Trace = Result<std::vector<stacktrace::Element>, Bail>;
    stacktrace::install(session, [includeCaller = true](const Frame& frame) mutable {
        check(frame.pc() == 1 && frame.method().name == "trace" && frame.stack().size() == 1,
              "provider observes call site");
        std::vector<stacktrace::Element> entries{{"demo.Decode", "run", std::string("Decode.java"), 42}};
        if (includeCaller) entries.push_back({"demo.Entry", "nativeRun", std::nullopt, -2});
        includeCaller = false;
        return Trace::ok(std::move(entries));
    });
    const Method trace = MethodBuilder("trace", "()[Ljava/lang/StackTraceElement;", 0)
        .invoke("java/lang/Thread", "currentThread", "()Ljava/lang/Thread;", ValType::Ref)
        .invokeVirtual("java/lang/Thread", "getStackTrace", "()[Ljava/lang/StackTraceElement;", ValType::Ref)
        .areturn().build();
    const auto first = run(session, trace);
    const auto second = run(session, trace);
    check(first.kind == RunResult::Kind::Completed && second.kind == RunResult::Kind::Completed &&
          first.returnValue.ref.objId != second.returnValue.ref.objId, "fresh trace arrays");
    const auto length = MethodBuilder("len", "()I", 0).arraylength().ireturn().build();
    checkValue(run(session, length, {first.returnValue}), SimValue::knownI32(2), "trace arraylength");
    checkValue(run(session, length, {second.returnValue}), SimValue::knownI32(1), "trace provider retains state");
    auto getter = [&](i32 index, const char* name, const char* desc, SimValue expected) {
        const Method method = MethodBuilder("get", desc, 0).iconst(index).aaload()
            .invokeVirtual("java/lang/StackTraceElement", name, desc, expected.type)
            .raw(returnOpcode(expected.type)).build();
        checkValue(run(session, method, {first.returnValue}), expected, name);
    };
    getter(0, "getClassName", "()Ljava/lang/String;", SimValue::knownStr("demo.Decode"));
    getter(1, "getMethodName", "()Ljava/lang/String;", SimValue::knownStr("nativeRun"));
    getter(0, "getFileName", "()Ljava/lang/String;", SimValue::knownStr("Decode.java"));
    getter(1, "getFileName", "()Ljava/lang/String;", SimValue::knownNull());
    getter(0, "getLineNumber", "()I", SimValue::knownI32(42));
    getter(1, "getLineNumber", "()I", SimValue::knownI32(-2));
    getter(1, "isNativeMethod", "()Z", SimValue::knownI32(1));
    const Method current = MethodBuilder("current", "()Ljava/lang/Thread;", 0)
        .invoke("java/lang/Thread", "currentThread", "()Ljava/lang/Thread;", ValType::Ref)
        .areturn().build();
    auto thread = run(session, current);
    checkValue(run(session, current), thread.returnValue, "stable currentThread identity");
    stacktrace::install(session, [](const Frame& frame) {
        return Trace::err({Bail::Reason::NotConcrete, frame.pc(), "trace unavailable"});
    });
    const auto unavailable = run(session, trace);
    check(unavailable.kind == RunResult::Kind::Bailed && unavailable.bail.detail == "trace unavailable",
          "trace provider Bail");

    stacktrace::install(session, [](const Frame& frame) {
        frame.session().heap.erase(frame.stack().back().ref.objId);
        return Trace::ok({});
    });
    const auto removed = run(session, trace);
    check(removed.kind == RunResult::Kind::Bailed && removed.bail.reason == Bail::Reason::BadOperand,
          "trace provider invalidated receiver");
}

} // namespace

int testJdkPackages() {
    testMath();
    testRandom();
    testStackTrace();
    std::printf("[jsim-jdk] %d 项断言，%d 失败\n", total, failed);
    return failed;
}
