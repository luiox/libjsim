#include <cstdio>

#include "jsim/str.hpp"

using namespace jsim;

namespace {

RunResult stringCall(Session& session, const char* name, const char* desc, ValType resultType,
                     std::vector<SimValue> stack) {
    Method method = MethodBuilder("str", desc, 0)
        .invokeVirtual("java/lang/String", name, desc, resultType)
        .raw(resultType == ValType::Ref ? op::AReturn : op::IReturn)
        .build();
    Prefill prefill;
    prefill.stack = std::move(stack);
    return Sim(method, session).runRegion(0, std::move(prefill));
}

bool isInt(const RunResult& run, i32 value) {
    return run.kind == RunResult::Kind::Completed && run.hasReturnValue &&
           run.returnValue.known && run.returnValue.intBits == value;
}

int total = 0;
int failed = 0;

#define CHECK(condition) do {                                  \
    ++total;                                                   \
    if (!(condition)) {                                        \
        ++failed;                                              \
        std::printf("FAIL String:%d %s\n", __LINE__, #condition); \
    }                                                          \
} while (false)

void testStrings(Session& session) {
    // HotSpot 21 的对拍串为 "A\u0000\ud83d\ude00Z"。
    const std::string utf8 = std::string("A\0", 2) + "\xF0\x9F\x98\x80" "Z";
    const std::string mutf8 = "A\xC0\x80\xED\xA0\xBD\xED\xB8\x80" "Z";
    const SimValue text = SimValue::knownStr(utf8);
    const SimValue literal = SimValue::internedStr(mutf8);
    const SimValue face = SimValue::knownStr("\xF0\x9F\x98\x80");
    const SimValue empty = SimValue::knownStr("");
    const SimValue missing = SimValue::unknownOf(ValType::Ref);
    const auto i = SimValue::knownI32;

    CHECK(str::codeUnitCount(utf8) == 5);
    CHECK(str::codeUnitCount(mutf8) == 5);
    CHECK(str::codeUnitAt(utf8, 2) == 0xD83D);
    CHECK(str::codeUnitAt(utf8, 3) == 0xDE00);
    CHECK(str::codeUnitAt("\xE2\x82", 0) == -1);
    CHECK(isInt(stringCall(session, "length", "()I", ValType::I32, {literal}), 5));
    CHECK(isInt(stringCall(session, "charAt", "(I)C", ValType::I32, {literal, i(1)}), 0));
    CHECK(isInt(stringCall(session, "charAt", "(I)C", ValType::I32, {text, i(2)}), 0xD83D));
    CHECK(isInt(stringCall(session, "charAt", "(I)C", ValType::I32, {text, i(3)}), 0xDE00));
    CHECK(isInt(stringCall(session, "isEmpty", "()Z", ValType::I32, {empty}), 1));
    CHECK(isInt(stringCall(session, "isEmpty", "()Z", ValType::I32, {literal}), 0));
    CHECK(isInt(stringCall(session, "hashCode", "()I", ValType::I32, {text}), 114988824));
    CHECK(isInt(stringCall(session, "hashCode", "()I", ValType::I32, {literal}), 114988824));
    CHECK(isInt(stringCall(session, "equals", "(Ljava/lang/Object;)Z", ValType::I32, {text, literal}), 1));
    CHECK(isInt(stringCall(session, "equals", "(Ljava/lang/Object;)Z", ValType::I32,
                           {text, SimValue::knownNull()}), 0));

    const auto low = stringCall(session, "substring", "(II)Ljava/lang/String;", ValType::Ref,
                                {text, i(3), i(4)});
    CHECK(low.kind == RunResult::Kind::Completed && low.returnValue.known &&
          str::codeUnitAt(low.returnValue.ref.str, 0) == 0xDE00);
    const auto tail = stringCall(session, "substring", "(I)Ljava/lang/String;", ValType::Ref,
                                 {text, i(4)});
    CHECK(tail.kind == RunResult::Kind::Completed && tail.returnValue.ref.str == "Z");
    const auto unchanged = stringCall(session, "substring", "(I)Ljava/lang/String;", ValType::Ref,
                                      {literal, i(0)});
    CHECK(unchanged.kind == RunResult::Kind::Completed && unchanged.returnValue.ref.interned);
    CHECK(stringCall(session, "substring", "(II)Ljava/lang/String;", ValType::Ref,
                     {text, i(3), i(2)}).kind == RunResult::Kind::Bailed);

    CHECK(isInt(stringCall(session, "indexOf", "(Ljava/lang/String;)I", ValType::I32,
                           {literal, face}), 2));
    CHECK(isInt(stringCall(session, "indexOf", "(Ljava/lang/String;I)I", ValType::I32,
                           {literal, face, i(3)}), -1));
    CHECK(isInt(stringCall(session, "indexOf", "(Ljava/lang/String;I)I", ValType::I32,
                           {literal, empty, i(200)}), 5));
    CHECK(stringCall(session, "indexOf", "(Ljava/lang/String;)I", ValType::I32,
                     {text, SimValue::knownNull()}).kind == RunResult::Kind::Bailed);

    const auto unknown = stringCall(session, "hashCode", "()I", ValType::I32, {missing});
    CHECK(unknown.kind == RunResult::Kind::Completed && unknown.hasReturnValue &&
          unknown.returnValue.type == ValType::I32 && !unknown.returnValue.known);
    const auto malformed = stringCall(session, "charAt", "(I)C", ValType::I32,
                                      {SimValue::knownStr("\xE2\x82"), i(0)});
    CHECK(malformed.kind == RunResult::Kind::Bailed && malformed.bail.reason == Bail::Reason::BadOperand);
}

void testConstructedString(Session& session) {
    const Method method = MethodBuilder("newString", "()I", 0)
        .newInst("java/lang/String").dup().iconst(1).newArray(5)
        .dup().iconst(0).ldcI(0xD83D).raw(op::CAStore)
        .invokeSpecial("java/lang/String", "<init>", "([C)V")
        .iconst(0).invokeVirtual("java/lang/String", "charAt", "(I)C", ValType::I32)
        .ireturn().build();
    CHECK(isInt(Sim(method, session).runEntry({}), 0xD83D));
}

void testBuffer(Session& session, const std::string& owner) {
    const std::string refDesc = ")L" + owner + ";";
    const Method toString = MethodBuilder("buffer", "()Ljava/lang/String;", 0)
        .newInst(owner).dup().invokeSpecial(owner, "<init>", "()V")
        .iconst('A').invokeVirtual(owner, "append", "(C" + refDesc, ValType::Ref)
        .aconstNull().invokeVirtual(owner, "append", "(Ljava/lang/String;" + refDesc, ValType::Ref)
        .invokeVirtual(owner, "toString", "()Ljava/lang/String;", ValType::Ref)
        .areturn().build();
    const auto output = Sim(toString, session).runEntry({});
    CHECK(output.kind == RunResult::Kind::Completed && output.returnValue.known &&
          output.returnValue.ref.str == "Anull");

    const Method unknownAppend = MethodBuilder("bufferUnknown", "(I)V", 1)
        .newInst(owner).dup().invokeSpecial(owner, "<init>", "()V")
        .iload(0).invokeVirtual(owner, "append", "(C" + refDesc, ValType::Ref)
        .pop().vreturn().build();
    Prefill prefill;
    prefill.locals = {SimValue::unknownOf(ValType::I32)};
    const auto stopped = Sim(unknownAppend, session).runEntry(std::move(prefill));
    CHECK(stopped.kind == RunResult::Kind::Bailed && stopped.bail.reason == Bail::Reason::NotConcrete);
}

} // namespace

int testStrExtensions() {
    Session session;
    str::install(session);
    str::installStringAlloc(session);
    testStrings(session);
    testConstructedString(session);
    testBuffer(session, "java/lang/StringBuilder");
    testBuffer(session, "java/lang/StringBuffer");
    std::printf("[jsim-str] %d 项断言，%d 失败\n", total, failed);
    return failed;
}
