/// @file test/jni/main.cpp
/// @brief jsim-jni 纯层测试:FakeMachine 驱动(docs/ext-jni.md 测试节)。
///        覆盖:函数表、描述符工具、句柄池、native mock 编组、骨架逐条、
///        异常位、用户钩子覆盖、RegisterNatives 录表、未实现函数 Bail。
///        期望值由本文件用同算法独立复算,不以被测件自身输出为基准。

#include <cstdio>
#include <cstring>
#include <map>

#include "jsim/engine.hpp"
#include "jsim/jni.hpp"

using namespace jsim;

namespace {

int g_total = 0;
int g_fail = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_total;                                                      \
        if (!(cond)) {                                                  \
            ++g_fail;                                                   \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

// ── FakeMem:块式 guest 内存 ─────────────────────────────────────────────────

struct FakeMem
{
    struct Block
    {
        u64 addr = 0;
        std::vector<u8> bytes;
    };

    std::vector<Block> blocks;
    u64 bump = 0x00302000;

    Block* find(u64 a, usize n)
    {
        for (Block& b : blocks) {
            if (a >= b.addr && a + n <= b.addr + b.bytes.size()) return &b;
        }
        return nullptr;
    }

    jni::GuestMemory face()
    {
        jni::GuestMemory m;
        m.map = [this](usize size, usize align) -> Result<std::pair<u64, usize>, Bail> {
            if (align == 0) align = 1;
            const u64 addr = (bump + align - 1) / align * align;
            bump = addr + ((size + 15) & ~usize(15));
            blocks.push_back({addr, std::vector<u8>(size, 0)});
            return Result<std::pair<u64, usize>, Bail>::ok({addr, size});
        };
        m.write = [this](u64 a, const void* p, usize n) -> Result<void, Bail> {
            Block* b = find(a, n);
            if (b == nullptr) {
                return Result<void, Bail>::err({Bail::Reason::BadOperand, 0, "FakeMem 写越界"});
            }
            std::memcpy(b->bytes.data() + (a - b->addr), p, n);
            return Result<void, Bail>::ok();
        };
        m.read = [this](u64 a, void* p, usize n) -> Result<void, Bail> {
            Block* b = find(a, n);
            if (b == nullptr) {
                return Result<void, Bail>::err({Bail::Reason::BadOperand, 0, "FakeMem 读越界"});
            }
            std::memcpy(p, b->bytes.data() + (a - b->addr), n);
            return Result<void, Bail>::ok();
        };
        m.free = [this](u64 a) -> Result<void, Bail> {
            for (usize i = 0; i < blocks.size(); ++i) {
                if (blocks[i].addr == a) {
                    blocks.erase(blocks.begin() + static_cast<usize>(i));
                    return Result<void, Bail>::ok();
                }
            }
            return Result<void, Bail>::err({Bail::Reason::BadOperand, 0, "FakeMem 释放未知块"});
        };
        return m;
    }

    /// 放一个 C 字符串,返回 guest 地址。
    u64 putCStr(const char* s)
    {
        const usize n = std::strlen(s) + 1;
        const u64 addr = bump;
        bump += (n + 15) & ~usize(15);
        blocks.push_back({addr, std::vector<u8>(n, 0)});
        std::memcpy(blocks.back().bytes.data(), s, n);
        return addr;
    }

    u64 putBytes(const void* p, usize n)
    {
        const u64 addr = bump;
        bump += (n + 15) & ~usize(15);
        blocks.push_back({addr, std::vector<u8>(n, 0)});
        std::memcpy(blocks.back().bytes.data(), p, n);
        return addr;
    }
};

// ── Script:预编脚本的假 native ──────────────────────────────────────────────

/// 每个 Step 是「native 体内依次发起的一次 JNI 调用」:函数号 + 各 C 参数位置
/// 的取值(直接位型 / NativeRun 实参槽 / 上一步返回)。pre 是现场改写口子
/// (FakeMem 预填、模拟 native 对副本的异或)。
struct Script
{
    struct Arg
    {
        enum class K : u8 { Bits, Slot, LastRet } k = K::Bits;
        u64 v = 0;
    };

    struct Step
    {
        u32 id = 0;
        std::map<usize, Arg> args;
        std::function<void(jni::JniEnv&, const jni::NativeRun&)> pre;
    };

    std::vector<Step> steps;
    u64 ret = 0;
    bool retIsLastRet = true;
    std::function<void(const jni::NativeRun&)> onRun;

    std::vector<u64> lastRets;

    u64 resolve(const Step& s, usize p, const jni::NativeRun& nr) const
    {
        auto it = s.args.find(p);
        if (it == s.args.end()) return 0;
        switch (it->second.k) {
        case Arg::K::Bits: return it->second.v;
        case Arg::K::Slot: return nr.args.at(static_cast<usize>(it->second.v));
        case Arg::K::LastRet: return lastRets.empty() ? 0 : lastRets.back();
        }
        return 0;
    }

    jni::NativeExecutor executor()
    {
        jni::NativeExecutor e;
        e.run = [this](jni::JniEnv& env, const jni::NativeRun& nr) -> Result<u64, Bail> {
            if (onRun) onRun(nr);
            lastRets.clear();
            for (const Step& s : steps) {
                        if (s.pre) s.pre(env, nr);
                // Call 系在位置 2 放 jmethodID(argClassesOf 需要它展类);其余函数忽略。
                const u64 midRaw = resolve(s, 2, nr);
                Result<std::vector<char>, Bail> cls = jni::argClassesOf(env, s.id, midRaw);
                if (!cls.isOk()) return Result<u64, Bail>::err(cls.error());
                auto reader = [&s, &nr, this](usize p) -> Result<u64, Bail> {
                    return Result<u64, Bail>::ok(resolve(s, p, nr));
                };
                Result<u64, Bail> r = jni::dispatch(env, s.id, reader, reader);
                if (!r.isOk()) return Result<u64, Bail>::err(r.error());
                lastRets.push_back(r.value());
            }
            const u64 out = retIsLastRet ? (lastRets.empty() ? ret : lastRets.back()) : ret;
            return Result<u64, Bail>::ok(out);
        };
        return e;
    }
};

// ── 测试工具 ────────────────────────────────────────────────────────────────

/// 造一个 invokestatic 到 native 方法的调用方字节码:实参按 desc 从 locals
/// 0..n-1 依序压栈(this 模型一个值一个条目,cat2 占一个 locals 槽)。
Method makeCaller(const char* desc, ValType ret, bool hasReturn)
{
    MethodBuilder b("Caller", desc, 8);
    i32 slot = 0;
    for (usize i = 1; i < std::strlen(desc) && desc[i] != ')'; ++i) {
        const char c = desc[i];
        if (c == 'L' || c == '[') {
            b.aload(static_cast<u32>(slot++));
            while (desc[i] != ';' && desc[i] != ')') ++i; // 跳过类名/数组后缀
        } else if (c == 'J') {
            b.raw(op::LLoad, slot++);
        } else if (c == 'F') {
            b.raw(op::FLoad, slot++);
        } else if (c == 'D') {
            b.raw(op::DLoad, slot++);
        } else {
            b.iload(static_cast<u32>(slot++)); // I/Z/B/C/S
        }
    }
    b.invoke("sample/Decrypt", "decrypt", desc, ret, hasReturn);
    if (!hasReturn) {
        b.vreturn();
    } else if (ret == ValType::Ref) {
        b.areturn();
    } else if (ret == ValType::I64) {
        b.raw(op::LReturn);
    } else {
        b.ireturn();
    }
    return b.build();
}

RunResult runNative(Session& s, jni::JniEnv& env, const char* desc, ValType ret, bool hasReturn,
                    Prefill p = {})
{
    Method m = makeCaller(desc, ret, hasReturn);
    return Sim(m, s).runEntry(std::move(p), 1u << 12);
}

// ── 函数表 ──────────────────────────────────────────────────────────────────

void testFuncTable()
{
    CHECK(jni::funcTable().size() == 233);
    CHECK(jni::metaOf(0) == nullptr);          // 0..3 保留
    CHECK(jni::metaOf(3) == nullptr);
    const jni::FuncMeta* gv = jni::metaOf(4);
    CHECK(gv != nullptr && std::strcmp(gv->name, "GetVersion") == 0 && gv->argc == 0);
    CHECK(jni::indexOf("GetStringUTFChars") == 169);
    CHECK(jni::indexOf("NewStringUTF") == 167);
    CHECK(jni::indexOf("ExceptionCheck") == 228);
    CHECK(jni::indexOf("RegisterNatives") == 215);
    CHECK(jni::indexOf("CallStaticObjectMethodA") == 116);
    CHECK(jni::indexOf("GetStaticIntField") == 150);
    CHECK(jni::indexOf("SetByteArrayRegion") == 208);
    CHECK(jni::indexOf("GetObjectRefType") == 232);
    CHECK(jni::indexOf("NoSuchFunc") == jni::kInvalidFunc);
    // 变长 bare 变体的定长前缀:env 之后的 obj/jclass 与 jmethodID 两参。
    CHECK(jni::metaOf(jni::indexOf("CallObjectMethod"))->argc == 2);
    CHECK(jni::metaOf(jni::indexOf("CallObjectMethodA"))->argc == 3);
    CHECK(jni::metaOf(jni::indexOf("NewObject"))->argc == 2);
    // 全表名非空、argc 有界(保留槽除外)。
    usize named = 0;
    for (const jni::FuncMeta& m : jni::funcTable()) {
        if (m.name == nullptr) continue;
        ++named;
        CHECK(m.argc <= 4);
    }
    CHECK(named == 229); // 233 - 4 保留
}

// ── 描述符工具 ──────────────────────────────────────────────────────────────

void testDescUtils()
{
    Result<std::vector<ValType>, Bail> t =
        jni::argTypesOfDesc("(IJFDLjava/lang/String;[B)V");
    CHECK(t.isOk());
    if (t.isOk()) {
        const std::vector<ValType>& v = t.value();
        CHECK(v.size() == 6);
        CHECK(v[0] == ValType::I32 && v[1] == ValType::I64 && v[2] == ValType::F32 &&
              v[3] == ValType::F64 && v[4] == ValType::Ref && v[5] == ValType::Ref);
    }
    Result<std::vector<ValType>, Bail> nested = jni::argTypesOfDesc("([[Ljava/lang/String;I)V");
    CHECK(nested.isOk() && nested.value().size() == 2 && nested.value()[0] == ValType::Ref);
    CHECK(!jni::argTypesOfDesc("IJFD").isOk());  // 缺 '('
    CHECK(!jni::argTypesOfDesc("(Ljava/String)V").isOk()); // 类名缺 ';'

    Result<std::pair<ValType, bool>, Bail> r1 = jni::retTypeOfDesc("(I)V");
    CHECK(r1.isOk() && !r1.value().second); // void 无返回
    Result<std::pair<ValType, bool>, Bail> r2 = jni::retTypeOfDesc("(I)D");
    CHECK(r2.isOk() && r2.value().first == ValType::F64 && r2.value().second);
    Result<std::pair<ValType, bool>, Bail> r3 = jni::retTypeOfDesc("()[B");
    CHECK(r3.isOk() && r3.value().first == ValType::Ref);
}

// ── 句柄池 ──────────────────────────────────────────────────────────────────

void testHandlePool()
{
    jni::HandlePool pool;
    Result<u64, Bail> h1 = pool.makeRef(SimValue::knownStr("a"), false);
    Result<u64, Bail> g1 = pool.makeRef(SimValue::knownStr("g"), true);
    Result<u64, Bail> n1 = pool.makeRef(SimValue::knownNull(), false);
    CHECK(h1.isOk() && g1.isOk());
    CHECK(n1.isOk() && n1.value() == 0); // null 编 0,不占句柄
    CHECK(pool.deref(h1.value()).isOk() && pool.deref(h1.value()).value().ref.str == "a");
    CHECK(!pool.deref(0xBEEF).isOk()); // 悬空 Bail

    pool.popLocals();
    CHECK(pool.alive() == 1);            // local 回收
    CHECK(!pool.deref(h1.value()).isOk());
    CHECK(pool.deref(g1.value()).isOk()); // global 持久

    Result<u64, Bail> mid = pool.makeMember({"Lsample/C;", "f", "(I)I", false});
    CHECK(mid.isOk());
    Result<const jni::HandlePool::Member*, Bail> m = pool.memberOf(mid.value());
    CHECK(m.isOk() && m.value()->name == "f" && !m.value()->isStatic);

    // IsSameObject 口径:同句柄真、双 null 真、同 objId 真、其余假。
    Result<u64, Bail> o1 = pool.makeRef(SimValue::knownObj(77), false);
    Result<u64, Bail> o2 = pool.makeRef(SimValue::knownObj(77), false);
    CHECK(pool.sameObject(o1.value(), o2.value()));
    CHECK(pool.sameObject(0, 0));
    CHECK(!pool.sameObject(o1.value(), g1.value()));
}

// ── native mock 编组:static / 实参位型 ─────────────────────────────────────

void testNativeMockStatic()
{
    Session s;
    jni::JniEnv env;
    env.session = &s;
    env.mem = FakeMem().face();
    Script script; // 空脚本:直接返回
    script.ret = 42;
    script.retIsLastRet = false;
    env.executor = script.executor();
    env.methods["sample/Decrypt.decrypt()I"] = jni::NativeSpec{0x00010040, true};
    jni::install(s, env);

    RunResult r = runNative(s, env, "()I", ValType::I32, true);
    CHECK(r.kind == RunResult::Kind::Completed);
    CHECK(r.hasReturnValue && r.returnValue.known &&
          static_cast<i32>(r.returnValue.asU32()) == 42);
}

void testArgEncoding()
{
    Session s;
    jni::JniEnv env;
    env.session = &s;
    env.mem = FakeMem().face();
    Script script;
    script.ret = 7;
    script.retIsLastRet = false;
    std::vector<u64> captured;
    script.onRun = [&captured](const jni::NativeRun& nr) { captured = nr.args; };
    env.executor = script.executor();
    env.methods["sample/Decrypt.decrypt(JF)I"] = jni::NativeSpec{0x00010040, true};
    jni::install(s, env);

    // desc (JF)I:class 句柄、long 位型、float 位型。
    Prefill p;
    p.locals = {SimValue::knownI64(-2), SimValue::knownF32(2.5f)};
    RunResult r = runNative(s, env, "(JF)I", ValType::I32, true, std::move(p));
    CHECK(r.kind == RunResult::Kind::Completed);
    CHECK(captured.size() == 3);
    if (captured.size() == 3) {
        CHECK(captured[0] != 0); // jclass 句柄
        CHECK(captured[1] == static_cast<u64>(-2)); // J 位型
        CHECK(captured[2] == floatToBits(2.5f));    // F 位型
    }
}

void testInstanceReceiver()
{
    Session s;
    jni::JniEnv env;
    env.session = &s;
    env.mem = FakeMem().face();
    Script script;
    script.steps.push_back({jni::indexOf("IsSameObject"),
                            {{1, {Script::Arg::K::Slot, 0}}, {2, {Script::Arg::K::Slot, 0}}}});
    script.ret = 1; // 不用;取 LastRet
    env.executor = script.executor();
    env.methods["sample/Decrypt.decrypt()I"] = jni::NativeSpec{0x00010040, false};
    jni::install(s, env);

    // invokevirtual:接收者入栈;native 收到的 args[0] 是接收者句柄。
    MethodBuilder b("Caller", "(Ljava/lang/String;)I", 2);
    b.aload(0);
    b.invokeVirtual("sample/Decrypt", "decrypt", "()I", ValType::I32);
    b.ireturn();
    Method m = b.build();

    Prefill p;
    p.locals = {SimValue::knownStr("recv")};
    RunResult r = Sim(m, s).runEntry(std::move(p), 1u << 12);
    CHECK(r.kind == RunResult::Kind::Completed);
    CHECK(r.hasReturnValue && r.returnValue.known &&
          static_cast<i32>(r.returnValue.asU32()) == 1); // IsSameObject(self, self)
}

// ── 字符串骨架:物化 + 回读 + local 回收 ─────────────────────────────────────

void testStringRoundtrip()
{
    Session s;
    jni::JniEnv env;
    env.session = &s;
    FakeMem mem;
    env.mem = mem.face();
    Script script;
    // native 体内:GetStringUTFChars(input) → 异或改写副本 → NewStringUTF(副本)
    script.steps.push_back({jni::indexOf("GetStringUTFChars"),
                            {{1, {Script::Arg::K::Slot, 1}}, {2, {Script::Arg::K::Bits, 0}}}});
    Script::Step step2;
    step2.id = jni::indexOf("NewStringUTF");
    step2.args = {{1, {Script::Arg::K::LastRet, 0}}};
    step2.pre = [&script, &mem](jni::JniEnv&, const jni::NativeRun&) {
        // 模拟 native 对副本的逐字节异或(真机版由机器码做,demo/jni 验证)。
        const u64 ptr = script.lastRets.back();
        usize i = 0;
        while (true) {
            u8 b = 0;
            Result<void, Bail> r = mem.face().read(ptr + i, &b, 1);
            if (!r.isOk() || b == 0) break;
            const u8 x = static_cast<u8>(b ^ 0x5A);
            mem.face().write(ptr + i, &x, 1);
            ++i;
        }
    };
    script.steps.push_back(step2);
    env.executor = script.executor();
    env.methods["sample/Decrypt.decrypt(Ljava/lang/String;)Ljava/lang/String;"] = jni::NativeSpec{0x00010040, true};
    jni::install(s, env);

    Prefill p;
    p.locals = {SimValue::internedStr("jinx")};
    RunResult r = runNative(s, env, "(Ljava/lang/String;)Ljava/lang/String;", ValType::Ref, true,
                            std::move(p));
    CHECK(r.kind == RunResult::Kind::Completed);
    CHECK(r.hasReturnValue && r.returnValue.known && r.returnValue.ref.kind == RefPayload::Kind::Str);
    if (r.hasReturnValue && r.returnValue.ref.kind == RefPayload::Kind::Str) {
        // 期望值独立复算:逐字节 ^ 0x5A。
        std::string expected;
        for (char c : std::string("jinx")) expected.push_back(static_cast<char>(c ^ 0x5A));
        CHECK(r.returnValue.ref.str == expected);
    }
    CHECK(env.pool.alive() == 0); // local 全回收(global 才留)
}

void testStaticFieldBridge()
{
    Session s;
    jni::JniEnv env;
    env.session = &s;
    FakeMem mem;
    env.mem = mem.face();
    Script script;
    script.steps.push_back({jni::indexOf("GetStaticFieldID"),
                            {{1, {Script::Arg::K::Slot, 0}},
                             {2, {Script::Arg::K::Bits, mem.putCStr("value")}},
                             {3, {Script::Arg::K::Bits, mem.putCStr("I")}}}});
    script.steps.push_back({jni::indexOf("GetStaticIntField"),
                            {{1, {Script::Arg::K::Slot, 0}}, {2, {Script::Arg::K::LastRet, 0}}}});
    env.executor = script.executor();
    env.methods["sample/Decrypt.decrypt()I"] = jni::NativeSpec{0x00010040, true};
    env.methods["sample/Decrypt.decrypt()V"] = jni::NativeSpec{0x00010040, true};
    jni::install(s, env);

    // 缺值:断链不编 0。
    RunResult miss = runNative(s, env, "()I", ValType::I32, true);
    CHECK(miss.kind == RunResult::Kind::Bailed);
    CHECK(miss.bail.reason == Bail::Reason::NotConcrete);

    // 预填后读回同一份状态。
    s.statics["sample/Decrypt::value::I"] = SimValue::knownI32(1234);
    RunResult hit = runNative(s, env, "()I", ValType::I32, true);
    CHECK(hit.kind == RunResult::Kind::Completed);
    CHECK(hit.hasReturnValue && static_cast<i32>(hit.returnValue.asU32()) == 1234);

    // 写回:native 侧 SetStaticIntField → 字节码侧 getstatic 可见。
    Script setScript;
    setScript.steps.push_back({jni::indexOf("GetStaticFieldID"),
                               {{1, {Script::Arg::K::Slot, 0}},
                                {2, {Script::Arg::K::Bits, mem.putCStr("value")}},
                                {3, {Script::Arg::K::Bits, mem.putCStr("I")}}}});
    setScript.steps.push_back({jni::indexOf("SetStaticIntField"),
                               {{1, {Script::Arg::K::Slot, 0}},
                                {2, {Script::Arg::K::LastRet, 0}},
                                {3, {Script::Arg::K::Bits, 777}}}});
    setScript.retIsLastRet = false;
    env.executor = setScript.executor();
    RunResult wr = runNative(s, env, "()V", ValType::I32, false);
    CHECK(wr.kind == RunResult::Kind::Completed);
    CHECK(s.statics["sample/Decrypt::value::I"].known &&
          static_cast<i32>(s.statics["sample/Decrypt::value::I"].asU32()) == 777);
}

// ── Call*Method 回跳 ────────────────────────────────────────────────────────

void testCallMethodA()
{
    Session s;
    jni::JniEnv env;
    env.session = &s;
    FakeMem mem;
    env.mem = mem.face();
    jni::installSkeletons(env);

    // jvalue 数组:[jstring 句柄]
    Result<u64, Bail> h = env.pool.makeRef(SimValue::knownStr("payload"), false);
    CHECK(h.isOk());
    const u64 jvAddr = mem.putBytes(&h.value(), 8);
    const u64 cid = s.classObjectOf("sample/Helper");
    Result<u64, Bail> clazz = env.pool.makeRef(SimValue::knownObj(cid), false);
    CHECK(clazz.isOk());
    Result<u64, Bail> mid = env.pool.makeMember({"sample/Helper", "build",
                                                 "(Ljava/lang/String;)Ljava/lang/String;", true});
    CHECK(mid.isOk());

    std::vector<u64> fixed = {0, clazz.value(), mid.value(), jvAddr}; // 下标 = C 位置
    auto reader = [fixed](usize p) -> Result<u64, Bail> {
        if (p >= fixed.size()) return Result<u64, Bail>::err({Bail::Reason::StackBroken, 0, "越位"});
        return Result<u64, Bail>::ok(fixed[p]);
    };

    bool invoked = false;
    env.javaInvoker = [&invoked](const CallRef& c, std::vector<SimValue> as) -> Result<SimValue, Bail> {
        invoked = true;
        CHECK(c.owner == "sample/Helper" && c.name == "build" && c.isStatic);
        CHECK(as.size() == 1 && as[0].ref.kind == RefPayload::Kind::Str && as[0].ref.str == "payload");
        return Result<SimValue, Bail>::ok(SimValue::knownStr("built"));
    };
    Result<u64, Bail> out = jni::dispatch(env, jni::indexOf("CallStaticObjectMethodA"), reader, reader);
    CHECK(invoked && out.isOk());
    if (out.isOk()) {
        Result<SimValue, Bail> back = env.pool.deref(out.value());
        CHECK(back.isOk() && back.value().ref.kind == RefPayload::Kind::Str &&
              back.value().ref.str == "built");
    }

    // 回跳结果不具体:Bail{NotConcrete},不编 0。
    env.javaInvoker = [](const CallRef&, std::vector<SimValue>) -> Result<SimValue, Bail> {
        return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::Ref));
    };
    Result<u64, Bail> out2 = jni::dispatch(env, jni::indexOf("CallStaticObjectMethodA"), reader, reader);
    CHECK(!out2.isOk() && out2.error().reason == Bail::Reason::NotConcrete);

    // javaInvoker 未设:Bail{UnsupportedCall}。句柄池属 env,env2 里重新发号。
    jni::JniEnv env2;
    env2.session = &s;
    env2.mem = mem.face();
    jni::installSkeletons(env2);
    Result<u64, Bail> h2 = env2.pool.makeRef(SimValue::knownStr("payload"), false);
    CHECK(h2.isOk());
    const u64 jvAddr2 = mem.putBytes(&h2.value(), 8);
    Result<u64, Bail> clazz2 = env2.pool.makeRef(SimValue::knownObj(cid), false);
    CHECK(clazz2.isOk());
    Result<u64, Bail> mid2 = env2.pool.makeMember({"sample/Helper", "build",
                                                   "(Ljava/lang/String;)Ljava/lang/String;", true});
    CHECK(mid2.isOk());
    std::vector<u64> fixed2 = {0, clazz2.value(), mid2.value(), jvAddr2}; // 下标 = C 位置
    auto reader2 = [fixed2](usize p) -> Result<u64, Bail> {
        return p < fixed2.size() ? Result<u64, Bail>::ok(fixed2[p])
                                 : Result<u64, Bail>::err({Bail::Reason::StackBroken, 0, "越位"});
    };
    Result<u64, Bail> out3 = jni::dispatch(env2, jni::indexOf("CallStaticObjectMethodA"), reader2, reader2);
    CHECK(!out3.isOk() && out3.error().reason == Bail::Reason::UnsupportedCall);
}

// ── 异常面 ──────────────────────────────────────────────────────────────────

void testExceptions()
{
    Session s;
    jni::JniEnv env;
    env.session = &s;
    FakeMem mem;
    env.mem = mem.face();
    Script script;
    script.steps.push_back({jni::indexOf("ThrowNew"),
                            {{1, {Script::Arg::K::Slot, 0}},
                             {2, {Script::Arg::K::Bits, mem.putCStr("boom")}}}});
    script.steps.push_back({jni::indexOf("ExceptionCheck"), {}});
    env.executor = script.executor();
    env.methods["sample/Decrypt.decrypt()I"] = jni::NativeSpec{0x00010040, true};
    jni::install(s, env);

    RunResult r = runNative(s, env, "()I", ValType::I32, true);
    CHECK(r.kind == RunResult::Kind::Bailed);
    CHECK(r.bail.reason == Bail::Reason::UnsupportedCall);
    CHECK(r.bail.detail.find("native 抛出异常") != std::string::npos);
    CHECK(!env.pendingThrow); // CallMock 收尾已清位

    // 挂起期间非检查类调用被拒。
    env.pendingThrow = true;
    env.throwClass = "java/lang/RuntimeException";
    auto zero = [](usize) { return Result<u64, Bail>::ok(0); };
    Result<u64, Bail> blocked = jni::dispatch(env, jni::indexOf("GetVersion"), zero, zero);
    CHECK(!blocked.isOk());
    Result<u64, Bail> check = jni::dispatch(env, jni::indexOf("ExceptionCheck"), zero, zero);
    CHECK(check.isOk() && check.value() == 1);
    Result<u64, Bail> cleared = jni::dispatch(env, jni::indexOf("ExceptionClear"), zero, zero);
    CHECK(cleared.isOk() && !env.pendingThrow);
}

// ── 用户钩子覆盖 / 未实现 ───────────────────────────────────────────────────

void testHookOverrideAndMissing()
{
    Session s;
    jni::JniEnv env;
    env.session = &s;
    env.mem = FakeMem().face();
    Script script;
    // native 体内先问版本:用户钩子应整体替换骨架,返回自定义值。
    script.steps.push_back({jni::indexOf("GetVersion"), {}});
    script.ret = 0x1234;
    script.retIsLastRet = false;
    env.executor = script.executor();

    env.hooks["GetVersion"] = [](jni::JniCall&) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(0x1234);
    };
    env.methods["sample/Decrypt.decrypt()I"] = jni::NativeSpec{0x00010040, true};
    jni::install(s, env); // 骨架只补缺,不覆盖上面的钩子

    RunResult r = runNative(s, env, "()I", ValType::I32, true);
    CHECK(r.kind == RunResult::Kind::Completed);
    CHECK(r.hasReturnValue && static_cast<i32>(r.returnValue.asU32()) == 0x1234);

    // 未实现函数:dispatch 拒绝,带函数名。
    auto zero = [](usize) { return Result<u64, Bail>::ok(0); };
    Result<u64, Bail> missing =
        jni::dispatch(env, jni::indexOf("GetDirectBufferAddress"), zero, zero);
    CHECK(!missing.isOk() && missing.error().reason == Bail::Reason::UnsupportedCall);
    CHECK(missing.error().detail.find("GetDirectBufferAddress") != std::string::npos);
}

// ── RegisterNatives 录表 ────────────────────────────────────────────────────

void testRegisterNatives()
{
    Session s;
    jni::JniEnv env;
    env.session = &s;
    FakeMem mem;
    env.mem = mem.face();
    jni::install(s, env);

    const u64 cid = s.classObjectOf("sample/N");
    Result<u64, Bail> clazz = env.pool.makeRef(SimValue::knownObj(cid), false);
    CHECK(clazz.isOk());
    // JNINativeMethod 表:{name*, sig*, fnPtr}
    struct Row
    {
        u64 name;
        u64 sig;
        u64 fn;
    } row{mem.putCStr("enc"), mem.putCStr("(I)I"), 0x00010040};
    const u64 tableAddr = mem.putBytes(&row, sizeof(row));

    auto zero = [](usize) { return Result<u64, Bail>::ok(0); };
    std::vector<u64> fixed = {0, clazz.value(), tableAddr, 1}; // 下标 = C 位置
    auto reader = [fixed](usize p) -> Result<u64, Bail> {
        return p < fixed.size() ? Result<u64, Bail>::ok(fixed[p])
                                : Result<u64, Bail>::err({Bail::Reason::StackBroken, 0, "越位"});
    };
    Result<u64, Bail> r = jni::dispatch(env, jni::indexOf("RegisterNatives"), reader, reader);
    CHECK(r.isOk());
    CHECK(env.methods.count("sample/N.enc(I)I") == 1);
    CHECK(env.methods["sample/N.enc(I)I"].entry == 0x00010040);

    // 录表后 resolver 认领新 native,空脚本返回值直通。
    Script script;
    script.ret = 99;
    script.retIsLastRet = false;
    env.executor = script.executor();
    MethodBuilder b("Caller", "(I)I", 2);
    b.iload(0);
    b.invoke("sample/N", "enc", "(I)I", ValType::I32);
    b.ireturn();
    Method m = b.build();
    Prefill p;
    p.locals = {SimValue::knownI32(1)};
    RunResult rr = Sim(m, s).runEntry(std::move(p), 1u << 12);
    CHECK(rr.kind == RunResult::Kind::Completed);
    CHECK(rr.hasReturnValue && static_cast<i32>(rr.returnValue.asU32()) == 99);
}

// ── 参数类布局 ──────────────────────────────────────────────────────────────

void testArgClasses()
{
    Session s;
    jni::JniEnv env;
    env.session = &s;
    env.mem = FakeMem().face();

    // 定长函数:全整。
    Result<std::vector<char>, Bail> fixed = jni::argClassesOf(env, jni::indexOf("GetStringUTFChars"), 0);
    CHECK(fixed.isOk() && fixed.value().size() == 3);
    for (char c : fixed.value()) CHECK(c == 0);

    // bare Call*Method:类由 jmethodID 的 desc 展开(env/recv/mid 恒整)。
    Result<u64, Bail> mid = env.pool.makeMember({"sample/C;", "h", "(IFJ)V", false});
    CHECK(mid.isOk());
    Result<std::vector<char>, Bail> bare =
        jni::argClassesOf(env, jni::indexOf("CallVoidMethod"), mid.value());
    CHECK(bare.isOk() && bare.value().size() == 6); // env, recv, mid, I, F, J
    if (bare.isOk() && bare.value().size() == 6) {
        CHECK(bare.value()[0] == 0 && bare.value()[1] == 0 && bare.value()[2] == 0);
        CHECK(bare.value()[3] == 0 && bare.value()[4] == 1 && bare.value()[5] == 0);
    }

    // A 变体:实参在内存,前缀恒整。
    Result<std::vector<char>, Bail> av =
        jni::argClassesOf(env, jni::indexOf("CallVoidMethodA"), mid.value());
    CHECK(av.isOk() && av.value().size() == 4);
    for (char c : av.value()) CHECK(c == 0);
}

// ── 兜底解析边界 ────────────────────────────────────────────────────────────

void testResolverBoundary()
{
    Session s;
    jni::JniEnv env;
    env.session = &s;
    env.mem = FakeMem().face();
    jni::install(s, env); // methods 为空:什么都不认领

    Method m = makeCaller("()I", ValType::I32, true);
    RunResult r = Sim(m, s).runEntry(Prefill{}, 1u << 12);
    CHECK(r.kind == RunResult::Kind::Bailed);
    CHECK(r.bail.reason == Bail::Reason::UnsupportedCall); // 核心口径,不静默
}

} // namespace

int main()
{
    testFuncTable();
    testDescUtils();
    testHandlePool();
    testNativeMockStatic();
    testArgEncoding();
    testInstanceReceiver();
    testStringRoundtrip();
    testStaticFieldBridge();
    testCallMethodA();
    testExceptions();
    testHookOverrideAndMissing();
    testRegisterNatives();
    testArgClasses();
    testResolverBoundary();

    std::printf("jsim-jni-test: %d/%d 通过\n", g_total - g_fail, g_total);
    return g_fail == 0 ? 0 : 1;
}
