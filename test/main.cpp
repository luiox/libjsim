/// @file test/main.cpp
/// @brief jsim 测试：值代数、逐类 handler、五个完备面（opcode/接管/启动/失败/观测）、
///        扩展包每条接管一测（设计 §9）。零外部依赖的轻量断言件。

#include <cstdio>

#include "jsim/engine.hpp"
#include "jsim/indy.hpp"
#include "jsim/num.hpp"
#include "jsim/reflect.hpp"
#include "jsim/str.hpp"

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

bool completedWith(const RunResult& r, i32 expected) {
    return r.kind == RunResult::Kind::Completed && r.hasReturnValue && r.returnValue.known &&
           r.returnValue.type == ValType::I32 &&
           static_cast<i32>(r.returnValue.asU32()) == expected;
}

RunResult runIt(const Method& m, Session& s, Prefill p = {}, u64 maxSteps = 256) {
    return Sim(m, s).run(0, std::move(p), Limits{maxSteps});
}

// ── 值代数（§3）──────────────────────────────────────────────────────────────

void testValueAlgebra() {
    const SimValue u = SimValue::unknownOf(ValType::I64);
    CHECK(!u.known && u.type == ValType::I64 && u.isCat2());

    const SimValue dyn = SimValue::knownStr("k");
    const SimValue lit = SimValue::internedStr("k");
    CHECK(dyn.ref.interned == false && lit.ref.interned == true);
    CHECK(dyn.ref.str == lit.ref.str); // 内容相同、身份口径不同

    const SimValue n = SimValue::knownNull();
    CHECK(n.known && n.ref.kind == RefPayload::Kind::Null); // D3：null 是已知

    const SimValue f = SimValue::knownF32(1.5f);
    CHECK(f.type == ValType::F32 && f.asF32() == 1.5f && !f.isCat2());
}

// ── 算术与转换恒等式（§9(1)：以 JVM 规范值为基准）───────────────────────────

void testArithmetic() {
    Session s;

    auto runII = [&](i32 a, i32 b, u8 tailOp) {
        Method m = MethodBuilder("e", "(II)I", 2)
                       .iload(0)
                       .iload(1)
                       .raw(tailOp)
                       .ireturn()
                       .build();
        Prefill p;
        p.locals = {SimValue::knownI32(a), SimValue::knownI32(b)};
        return runIt(m, s, std::move(p));
    };

    CHECK(completedWith(runII(INT32_MAX, 1, op::IAdd), INT32_MIN));  // 溢出回绕
    CHECK(completedWith(runII(-1, 33, op::IShl), -2));               // 移位掩 31
    CHECK(completedWith(runII(INT32_MIN, -1, op::IDiv), INT32_MIN)); // JVM 定义值
    CHECK(completedWith(runII(INT32_MIN, -1, op::IRem), 0));
    CHECK(completedWith(runII(-1, 1, op::IUshr), 2147483647));

    RunResult divZero = runII(7, 0, op::IDiv);
    CHECK(divZero.kind == RunResult::Kind::Bailed &&
          divZero.bail.reason == Bail::Reason::NotConcrete); // 除零需异常派发

    Method dm = MethodBuilder("e", "(II)I", 2).iload(0).iload(1).raw(op::IDiv).ireturn().build();
    Prefill uz;
    uz.locals = {SimValue::knownI32(7), SimValue::unknownOf(ValType::I32)};
    RunResult unknownDiv = runIt(dm, s, std::move(uz));
    CHECK(unknownDiv.kind == RunResult::Kind::Completed && !unknownDiv.returnValue.known &&
          unknownDiv.returnValue.type == ValType::I32); // 除数未知按类型传播

    // f2i 饱和：NaN → 0，越界钳端点。
    Method nan2i = MethodBuilder("n", "(F)I", 1).iload(0).raw(op::F2i).ireturn().build();
    Prefill pn;
    pn.locals = {SimValue::knownF32(bitsToFloat(0x7FC00000u))}; // 规范 NaN 位型
    CHECK(completedWith(runIt(nan2i, s, std::move(pn)), 0));
    Prefill pb;
    pb.locals = {SimValue::knownF32(1e30f)};
    CHECK(completedWith(runIt(nan2i, s, std::move(pb)), INT32_MAX));

    // i2c 截 16 位。
    Method c = MethodBuilder("c", "(I)I", 1).iload(0).i2c().ireturn().build();
    Prefill pc;
    pc.locals = {SimValue::knownI32(0x12345)};
    CHECK(completedWith(runIt(c, s, std::move(pc)), 0x2345));

    // lshl 移位掩 63。
    Method ls = MethodBuilder("ls", "(JI)J", 3)
                    .raw(op::LLoad, 0)
                    .iload(2)
                    .raw(op::LShl)
                    .raw(op::LReturn)
                    .build();
    Prefill pl;
    pl.locals = {SimValue::knownI64(1), SimValue::unknownOf(ValType::I32),
                 SimValue::knownI32(65)};
    RunResult lr = runIt(ls, s, std::move(pl));
    CHECK(lr.kind == RunResult::Kind::Completed && lr.returnValue.known &&
          lr.returnValue.type == ValType::I64 && lr.returnValue.intBits == 2);

    // 类型传播：任一操作数 Unknown 吸收结果（D8），执行不断链。
    Method add = MethodBuilder("add", "(II)I", 2).iload(0).iload(1).iadd().ireturn().build();
    Prefill pu;
    pu.locals = {SimValue::unknownOf(ValType::I32), SimValue::knownI32(5)};
    RunResult ua = runIt(add, s, std::move(pu));
    CHECK(ua.kind == RunResult::Kind::Completed && !ua.returnValue.known &&
          ua.returnValue.type == ValType::I32);
}

// ── 分支：接管收完整帧、三态裁决、acmp 身份、switch 纪律（§6）──────────────

void testBranchFace() {
    // Unknown 条件：未注册接管 → Bail{NotConcrete}；注册后收完整帧并可窥操作数。
    Method m = MethodBuilder("b", "(I)I", 1)
                   .iload(0)   // 0
                   .ifNe(4)    // 1
                   .iconst(0)  // 2
                   .ireturn()  // 3
                   .iconst(1)  // 4
                   .ireturn()  // 5
                   .build();
    Session bare;
    Prefill unk;
    unk.locals = {SimValue::unknownOf(ValType::I32)};
    RunResult noHook = runIt(m, bare, std::move(unk));
    CHECK(noHook.kind == RunResult::Kind::Bailed &&
          noHook.bail.reason == Bail::Reason::NotConcrete);

    Session hooked;
    bool sawOperandOnStack = false;
    hooked.branchHooks[1] = [&](Frame& frame) -> Result<bool, Bail> {
        // §6 红线：接管收完整进入帧，条件操作数仍在栈上。
        Result<SimValue, Bail> cond = frame.peekChecked(0);
        sawOperandOnStack = frame.stack().size() == 1 && cond.isOk() &&
                            cond.value().type == ValType::I32 && !cond.value().known;
        return Result<bool, Bail>::ok(true); // 裁决 Taken
    };
    Prefill unk2;
    unk2.locals = {SimValue::unknownOf(ValType::I32)};
    RunResult ruled = runIt(m, hooked, std::move(unk2));
    CHECK(sawOperandOnStack);
    CHECK(completedWith(ruled, 1)); // Taken → iconst_1
    CHECK(ruled.finalStack.empty()); // 裁决后操作数由引擎消费，不残留

    // acmp 身份：ldc 字面量（intern）可判；动态构造字符串身份不可判。
    Method acmp = MethodBuilder("a", "(Ljava/lang/String;)I", 1)
                      .ldcStr("key") // 0
                      .aload(0)      // 1
                      .ifAcmpeq(5)   // 2
                      .iconst(0)     // 3
                      .ireturn()     // 4
                      .iconst(1)     // 5
                      .ireturn()     // 6
                      .build();
    Session s;
    Prefill litSame;
    litSame.locals = {SimValue::internedStr("key")};
    CHECK(completedWith(runIt(acmp, s, std::move(litSame)), 1)); // intern 同对象
    Prefill litDiff;
    litDiff.locals = {SimValue::internedStr("other")};
    CHECK(completedWith(runIt(acmp, s, std::move(litDiff)), 0));
    Prefill dynSame;
    dynSame.locals = {SimValue::knownStr("key")}; // StringBuilder.toString 产物口径
    RunResult undecided = runIt(acmp, s, std::move(dynSame));
    CHECK(undecided.kind == RunResult::Kind::Bailed &&
          undecided.bail.reason == Bail::Reason::NotConcrete); // 身份不可判，绝不按内容折真
    Prefill nullArg;
    nullArg.locals = {SimValue::knownNull()};
    CHECK(completedWith(runIt(acmp, s, std::move(nullArg)), 0)); // null 与非 null 不等

    // switch：命中/default 路由，键 Unknown → Bail{NotConcrete}。
    Method sw = MethodBuilder("s", "(I)I", 1)
                    .iload(0)                       // 0
                    .tableswitch(0, {5, 7, 9}, 9)   // 1：key 0→5、1→7、2→9、域外→9
                    .nop()                          // 2
                    .nop()                          // 3
                    .nop()                          // 4
                    .iconst(50)                     // 5
                    .ireturn()                      // 6
                    .iconst(60)                     // 7
                    .ireturn()                      // 8
                    .iconst(70)                     // 9
                    .ireturn()                      // 10
                    .build();
    Prefill k0;
    k0.locals = {SimValue::knownI32(0)};
    CHECK(completedWith(runIt(sw, s, std::move(k0)), 50));
    Prefill k1;
    k1.locals = {SimValue::knownI32(1)};
    CHECK(completedWith(runIt(sw, s, std::move(k1)), 60));
    Prefill kOut;
    kOut.locals = {SimValue::knownI32(9)};
    CHECK(completedWith(runIt(sw, s, std::move(kOut)), 70)); // 域外走 default
    Prefill kUnk;
    kUnk.locals = {SimValue::unknownOf(ValType::I32)};
    RunResult swBail = runIt(sw, s, std::move(kUnk));
    CHECK(swBail.kind == RunResult::Kind::Bailed &&
          swBail.bail.reason == Bail::Reason::NotConcrete);
}

// ── 调用接管：调用点优先于签名、兜底解析链、未注册 Bail（§6）───────────────

void testCallFace() {
    Method m = MethodBuilder("c", "()I", 0)
                   .invoke("A", "f", "()I", ValType::I32) // pc0
                   .invoke("A", "f", "()I", ValType::I32) // pc1
                   .iadd()
                   .ireturn()
                   .build();
    Session s;
    s.callMocks["A.f()I"] = [](Frame&) -> Result<SimValue, Bail> {
        return Result<SimValue, Bail>::ok(SimValue::knownI32(7));
    };
    s.callMocksAtPc[1] = [](Frame&) -> Result<SimValue, Bail> {
        return Result<SimValue, Bail>::ok(SimValue::knownI32(10));
    };
    CHECK(completedWith(runIt(m, s), 17)); // pc0 按签名 7，pc1 按调用点 10

    Session unreg;
    Method one = MethodBuilder("o", "()I", 0)
                     .invoke("X", "h", "()I", ValType::I32)
                     .ireturn()
                     .build();
    RunResult bail = runIt(one, unreg);
    CHECK(bail.kind == RunResult::Kind::Bailed &&
          bail.bail.reason == Bail::Reason::UnsupportedCall);

    Session chained;
    chained.callMockResolvers.push_back(
        [](const CallRef&, Frame&) -> CallMock* { return nullptr; }); // 不认领
    chained.callMockResolvers.push_back([](const CallRef& c, Frame&) -> CallMock* {
        if (c.owner != "X") return nullptr;
        static CallMock mock = [](Frame&) -> Result<SimValue, Bail> {
            return Result<SimValue, Bail>::ok(SimValue::knownI32(5));
        };
        return &mock;
    });
    CHECK(completedWith(runIt(one, chained), 5)); // 链上第二个认领
}

// ── 字段接管（§6）：注入、记录、缺项 Unknown、null 接收者纪律（§12 v0.3）───

void testFieldFace() {
    Session s;
    s.fieldMocks["cfg.magicI"] = [](Frame&) -> Result<SimValue, Bail> {
        return Result<SimValue, Bail>::ok(SimValue::knownI32(42));
    };
    Method get = MethodBuilder("g", "()I", 0)
                     .getStatic("cfg", "magic", "I", ValType::I32)
                     .ireturn()
                     .build();
    CHECK(completedWith(runIt(get, s), 42));

    i64 recorded = 0;
    bool putPopped = false;
    s.fieldMocks["cfg.flagI"] = [&](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> v = frame.popChecked();
        if (v.isOk()) {
            putPopped = true;
            recorded = v.value().intBits;
        }
        return v;
    };
    Method put = MethodBuilder("p", "()V", 0)
                     .iconst(7)
                     .putStatic("cfg", "flag", "I", ValType::I32)
                     .vreturn()
                     .build();
    RunResult pr = runIt(put, s);
    CHECK(pr.kind == RunResult::Kind::Completed && !pr.hasReturnValue);
    CHECK(putPopped && recorded == 7);

    // 未接管、未赋值：getstatic 缺项按类型传播 Unknown（缺项是常态不是错误）。
    Method miss = MethodBuilder("m", "()I", 0)
                      .getStatic("cfg", "none", "I", ValType::I32)
                      .ireturn()
                      .build();
    RunResult mr = runIt(miss, s);
    CHECK(mr.kind == RunResult::Kind::Completed && !mr.returnValue.known &&
          mr.returnValue.type == ValType::I32);

    // 底座：putstatic 后 getstatic 读回同一 Session（跨方法接力棒）。
    Method store = MethodBuilder("st", "(I)V", 1)
                       .iload(0)
                       .putStatic("cfg", "box", "I", ValType::I32)
                       .vreturn()
                       .build();
    Method load = MethodBuilder("ld", "()I", 0)
                      .getStatic("cfg", "box", "I", ValType::I32)
                      .ireturn()
                      .build();
    Prefill nine;
    nine.locals = {SimValue::knownI32(9)};
    runIt(store, s, std::move(nine));
    CHECK(completedWith(runIt(load, s), 9));

    // getfield：Known null 接收者按 NPE 口径 Bail；Unknown 接收者类型传播。
    Method gf = MethodBuilder("gf", "(Ljava/lang/Object;)I", 1)
                    .aload(0)
                    .getField("p", "x", "I", ValType::I32)
                    .ireturn()
                    .build();
    Prefill nul;
    nul.locals = {SimValue::knownNull()};
    RunResult npe = runIt(gf, s, std::move(nul));
    CHECK(npe.kind == RunResult::Kind::Bailed && npe.bail.reason == Bail::Reason::BadOperand);
    Prefill unk;
    unk.locals = {SimValue::unknownOf(ValType::Ref)};
    RunResult propagated = runIt(gf, s, std::move(unk));
    CHECK(propagated.kind == RunResult::Kind::Completed && !propagated.returnValue.known);
}

// ── 观测面（§6）：结构化步进、Bail 现场、帧快照、断点熔断与续跑 ─────────────

void testObservationFace() {
    Method m = MethodBuilder("o", "(I)I", 1)
                   .iload(0)  // 0
                   .ldcI(3)   // 1
                   .iadd()    // 2
                   .ireturn() // 3
                   .build();

    Session s;
    int steps = 0;
    bool sawPreState = false;
    s.stepObservers.push_back([&](const StepView& view) {
        ++steps;
        if (view.pc == 0 && view.stack->empty()) sawPreState = true; // 指令前视图
        CHECK(view.method != nullptr && view.session != nullptr);
    });

    // 断点熔断：iadd 执行前停，LimitHit 语义 + 帧快照。
    Limits lim;
    lim.maxSteps = 64;
    lim.stopAtPc = 2;
    Prefill one;
    one.locals = {SimValue::knownI32(1)};
    RunResult stopped = Sim(m, s).run(0, std::move(one), lim);
    CHECK(stopped.kind == RunResult::Kind::LimitHit && stopped.steps == 2);
    CHECK(stopped.finalStack.size() == 2 && stopped.finalStack[0].known &&
          static_cast<i32>(stopped.finalStack[0].asU32()) == 1 &&
          static_cast<i32>(stopped.finalStack[1].asU32()) == 3);
    CHECK(stopped.finalLocals.size() == 1 && stopped.finalLocals[0].known);

    // 续跑：快照灌回 Prefill，区间起步收尾。
    Prefill resume;
    resume.stack = stopped.finalStack;
    resume.locals = stopped.finalLocals;
    RunResult done = Sim(m, s).runRegion(2, std::move(resume));
    CHECK(completedWith(done, 4));
    CHECK(steps == 4); // 每条执行到的指令各观测一次
    CHECK(sawPreState);

    // Bail 现场回调 + 终止帧快照。
    Session bs;
    bool bailSeen = false;
    bool bailSceneIntact = false;
    bs.bailObserver = [&](const Bail& b, const StepView& view) {
        bailSeen = b.reason == Bail::Reason::UnsupportedCall;
        bailSceneIntact = view.stack != nullptr && view.stack->size() == 1;
    };
    Method bo = MethodBuilder("bo", "()I", 0)
                    .iconst(1)
                    .invoke("nope", "f", "()I", ValType::I32)
                    .ireturn()
                    .build();
    RunResult br = runIt(bo, bs);
    CHECK(br.kind == RunResult::Kind::Bailed && bailSeen && bailSceneIntact);
    CHECK(br.finalStack.size() == 1 && static_cast<i32>(br.finalStack[0].asU32()) == 1);
}

// ── 启动面（§5）：栈平衡校验、两个便利入口、域外出口容忍 ────────────────────

void testStartupFace() {
    Session s;

    // 合并点形不一致：pc2 两达，栈形 [I32] → [I32, I32]。
    Method bad = MethodBuilder("bad", "()V", 0)
                     .nop()      // 0
                     .nop()      // 1
                     .ldcI(0)    // 2  首达 []
                     .ifEq(5)    // 3  0==0 → taken
                     .nop()      // 4
                     .iconst(9)  // 5
                     .gotoPc(2)  // 6  → pc2 再达 [I32]
                     .build();
    Limits strict;
    strict.maxSteps = 64;
    strict.checkBalance = true;
    Prefill seed;
    seed.stack = {SimValue::knownI32(1)};
    RunResult mismatch = Sim(bad, s).run(2, std::move(seed), strict);
    CHECK(mismatch.kind == RunResult::Kind::Bailed &&
          mismatch.bail.reason == Bail::Reason::StackBroken);

    // 关掉校验：同一方法栈无限增长 → 步数熔断（不是 StackBroken）。
    Prefill seed2;
    seed2.stack = {SimValue::knownI32(1)};
    RunResult spin = Sim(bad, s).run(2, std::move(seed2), Limits{64});
    CHECK(spin.kind == RunResult::Kind::LimitHit);

    // runEntry：一致循环（0..3 计数）平衡校验通过。
    Method loop = MethodBuilder("loop", "()I", 2)
                      .iconst(0)    // 0
                      .istore(1)    // 1
                      .iload(1)     // 2 循环头，恒空栈
                      .iconst(3)    // 3
                      .ifIcmpge(10) // 4
                      .iload(1)     // 5
                      .iconst(1)    // 6
                      .iadd()       // 7
                      .istore(1)    // 8
                      .gotoPc(2)    // 9
                      .iload(1)     // 10
                      .ireturn()    // 11
                      .build();
    RunResult counted = Sim(loop, s).runEntry(Prefill{});
    CHECK(completedWith(counted, 3));

    // runRegion：域外出口容忍（越过方法尾按 Completed(void) 收尾）。
    Method offEnd = MethodBuilder("off", "()V", 0).iconst(1).build(); // 无 return
    RunResult region = Sim(offEnd, s).runRegion(0, Prefill{});
    CHECK(region.kind == RunResult::Kind::Completed && !region.hasReturnValue);
    RunResult raw = Sim(offEnd, s).run(0, Prefill{}, Limits{16});
    CHECK(raw.kind == RunResult::Kind::Bailed &&
          raw.bail.reason == Bail::Reason::OffMethodEnd); // raw 入口不容忍
}

// ── 数组与分配（§4/§6）：符号宽度、脚手架、multianewarray 全维分配 ──────────

void testArraysAndAlloc() {
    Session s;

    // [B 有符号、[C 无符号、[S 有符号。
    auto narrow = [&](i32 atype, i32 value, u8 loadOp) {
        const u8 storeOp = loadOp == op::BALoad   ? op::BAStore
                           : loadOp == op::CALoad ? op::CAStore
                                                  : op::SAStore;
        Method m = MethodBuilder("n", "()I", 1)
                       .iconst(2)
                       .newArray(atype)
                       .astore(0)
                       .aload(0)
                       .iconst(0)
                       .ldcI(value)
                       .raw(storeOp)
                       .aload(0)
                       .iconst(0)
                       .raw(loadOp)
                       .ireturn()
                       .build();
        return runIt(m, s);
    };
    CHECK(completedWith(narrow(8, -1, op::BALoad), -1));       // [B
    CHECK(completedWith(narrow(5, 65535, op::CALoad), 65535)); // [C 无符号
    CHECK(completedWith(narrow(9, 65534, op::SALoad), -2));    // [S 截短有符号

    // arraylength：null → Bail。
    Method lenNull = MethodBuilder("ln", "()I", 0).aconstNull().arraylength().ireturn().build();
    RunResult ln = runIt(lenNull, s);
    CHECK(ln.kind == RunResult::Kind::Bailed && ln.bail.reason == Bail::Reason::BadOperand);

    // 未知下标 aaload → 类型传播。
    Method unkIdx = MethodBuilder("ui", "(I)I", 2)
                        .iconst(2)
                        .newArray(10)
                        .astore(1)
                        .aload(1)
                        .iload(0)
                        .iaload()
                        .ireturn()
                        .build();
    Prefill ui;
    ui.locals = {SimValue::unknownOf(ValType::I32), SimValue::unknownOf(ValType::Ref)};
    RunResult ur = runIt(unkIdx, s, std::move(ui));
    CHECK(ur.kind == RunResult::Kind::Completed && !ur.returnValue.known &&
          ur.returnValue.type == ValType::I32);

    // aastore 值未知：整体跳写不落假值；读取仍得初始 null。
    Method aa = MethodBuilder("aa", "()Ljava/lang/Object;", 2)
                    .iconst(1)
                    .anewArray("Ljava/lang/Object;")
                    .astore(1)
                    .aload(1)
                    .iconst(0)
                    .aload(0)
                    .raw(op::AAStore)
                    .aload(1)
                    .iconst(0)
                    .aaload()
                    .areturn()
                    .build();
    Prefill aap;
    aap.locals = {SimValue::unknownOf(ValType::Ref), SimValue::unknownOf(ValType::Ref)};
    RunResult aar = runIt(aa, s, std::move(aap));
    CHECK(aar.kind == RunResult::Kind::Completed && aar.returnValue.known &&
          aar.returnValue.ref.kind == RefPayload::Kind::Null);

    // multianewarray 全维分配：2×3 int[][]，内层长度可达、类名正确。
    Method m2 = MethodBuilder("m2", "()I", 1)
                    .iconst(2)
                    .iconst(3)
                    .multiANewArray("I", 2)
                    .astore(0)
                    .aload(0)
                    .iconst(1)
                    .aaload()
                    .arraylength()
                    .ireturn()
                    .build();
    CHECK(completedWith(runIt(m2, s), 3));

    Session s2;
    Method m2ref = MethodBuilder("m2r", "()Ljava/lang/Object;", 0)
                       .iconst(2)
                       .iconst(3)
                       .multiANewArray("I", 2)
                       .iconst(1)
                       .aaload()
                       .areturn()
                       .build();
    RunResult inner = runIt(m2ref, s2);
    CHECK(inner.kind == RunResult::Kind::Completed && inner.returnValue.known &&
          inner.returnValue.ref.kind == RefPayload::Kind::Obj);
    auto innerIt = s2.heap.find(inner.returnValue.ref.objId);
    CHECK(innerIt != s2.heap.end() && innerIt->second.className == "[I" &&
          innerIt->second.elems.size() == 3);

    // 内层长度未知：外层照建、元素 Unknown(Ref)（不猜 null）。
    Method m2unk = MethodBuilder("m2u", "()Ljava/lang/Object;", 0)
                       .multiANewArray("I", 2)
                       .areturn()
                       .build();
    Prefill counts;
    counts.stack = {SimValue::knownI32(2), SimValue::unknownOf(ValType::I32)};
    RunResult unkInner = Sim(m2unk, s2).run(0, std::move(counts), Limits{16});
    CHECK(unkInner.kind == RunResult::Kind::Completed && unkInner.returnValue.known &&
          unkInner.returnValue.ref.kind == RefPayload::Kind::Obj);
    auto outerIt = s2.heap.find(unkInner.returnValue.ref.objId);
    CHECK(outerIt != s2.heap.end() && outerIt->second.className == "[[I" &&
          outerIt->second.refs.size() == 2 && !outerIt->second.refs[0].known);

    // 负长度：NegativeArraySizeException 口径 Bail。
    Method neg = MethodBuilder("neg", "()Ljava/lang/Object;", 0)
                     .iconst(1)
                     .iconst(-1)
                     .multiANewArray("I", 2)
                     .areturn()
                     .build();
    RunResult negR = runIt(neg, s2);
    CHECK(negR.kind == RunResult::Kind::Bailed && negR.bail.reason == Bail::Reason::BadOperand);

    // new：未注册脚手架 Bail；注册后建堆对象。
    Method nw = MethodBuilder("nw", "()Ljava/lang/Object;", 0)
                    .newInst("java/lang/StringBuilder")
                    .areturn()
                    .build();
    Session bare;
    RunResult noScaffold = runIt(nw, bare);
    CHECK(noScaffold.kind == RunResult::Kind::Bailed &&
          noScaffold.bail.reason == Bail::Reason::UnsupportedAlloc);
    Session withS;
    withS.allocScaffolds["java/lang/StringBuilder"] = [] { return std::string(); };
    RunResult got = runIt(nw, withS);
    CHECK(got.kind == RunResult::Kind::Completed && got.returnValue.known &&
          got.returnValue.ref.kind == RefPayload::Kind::Obj);
}

// ── ldc 类常量与 Class 对象 identity（§12）──────────────────────────────────

void testClassIdentity() {
    Session s;
    Method m = MethodBuilder("ci", "()I", 0)
                   .ldcClass("a/B")
                   .ldcClass("a/B")
                   .ifAcmpeq(5)
                   .iconst(0)
                   .ireturn()
                   .iconst(1)
                   .ireturn()
                   .build();
    CHECK(completedWith(runIt(m, s), 1)); // 同名类常量同一对象

    Method diff = MethodBuilder("cd", "()I", 0)
                      .ldcClass("a/B")
                      .ldcClass("a/C")
                      .ifAcmpne(5)
                      .iconst(0)
                      .ireturn()
                      .iconst(1)
                      .ireturn()
                      .build();
    CHECK(completedWith(runIt(diff, s), 1));
}

// ── 扩展包：jsim-str（§7 每条接管一测）──────────────────────────────────────

void testStrPackage() {
    Session s;
    str::install(s);

    // StringBuilder 两阶段：构造 → append → toString。
    Method build = MethodBuilder("sb", "()Ljava/lang/String;", 1)
                       .newInst("java/lang/StringBuilder")
                       .dup()
                       .invokeSpecial("java/lang/StringBuilder", "<init>", "()V")
                       .astore(0)
                       .aload(0)
                       .ldcI('j')
                       .invokeVirtual("java/lang/StringBuilder", "append",
                                      "(C)Ljava/lang/StringBuilder;", ValType::Ref)
                       .pop()
                       .aload(0)
                       .ldcI('s')
                       .invokeVirtual("java/lang/StringBuilder", "append",
                                      "(C)Ljava/lang/StringBuilder;", ValType::Ref)
                       .pop()
                       .aload(0)
                       .invokeVirtual("java/lang/StringBuilder", "toString",
                                      "()Ljava/lang/String;", ValType::Ref)
                       .areturn()
                       .build();
    RunResult r = runIt(build, s, {}, 64);
    CHECK(r.kind == RunResult::Kind::Completed && r.returnValue.known &&
          r.returnValue.ref.kind == RefPayload::Kind::Str && r.returnValue.ref.str == "js");

    // charAt / length：越界明确 Bail。
    Method ch = MethodBuilder("ch", "(I)C", 1)
                    .ldcStr("abc")
                    .iload(0)
                    .invokeVirtual("java/lang/String", "charAt", "(I)C", ValType::I32)
                    .ireturn()
                    .build();
    Prefill i1;
    i1.locals = {SimValue::knownI32(1)};
    CHECK(completedWith(runIt(ch, s, std::move(i1)), 'b'));
    Prefill i9;
    i9.locals = {SimValue::knownI32(9)};
    RunResult oob = runIt(ch, s, std::move(i9));
    CHECK(oob.kind == RunResult::Kind::Bailed && oob.bail.reason == Bail::Reason::BadOperand);

    Method len = MethodBuilder("len", "()I", 0)
                     .ldcStr("abc")
                     .invokeVirtual("java/lang/String", "length", "()I", ValType::I32)
                     .ireturn()
                     .build();
    CHECK(completedWith(runIt(len, s), 3));
}

// ── 扩展包：jsim-num（解析失败/越界明确 Bail，不编造）──────────────────────

void testNumPackage() {
    Session s;
    num::install(s);

    auto parseIntOf = [&](const char* text) {
        Method m = MethodBuilder("pi", "()I", 0)
                       .ldcStr(text)
                       .invoke("java/lang/Integer", "parseInt", "(Ljava/lang/String;)I",
                               ValType::I32)
                       .ireturn()
                       .build();
        return runIt(m, s);
    };
    CHECK(completedWith(parseIntOf("123"), 123));
    CHECK(completedWith(parseIntOf("-2147483648"), INT32_MIN)); // 下界放行
    RunResult over = parseIntOf("2147483648");                  // 上界越界
    CHECK(over.kind == RunResult::Kind::Bailed &&
          over.bail.reason == Bail::Reason::NotConcrete);
    RunResult junk = parseIntOf("12x");
    CHECK(junk.kind == RunResult::Kind::Bailed);
    RunResult huge = parseIntOf("99999999999999999999999999"); // 位数溢出
    CHECK(huge.kind == RunResult::Kind::Bailed);

    Method pl = MethodBuilder("pl", "()J", 0)
                    .ldcStr("-9223372036854775808")
                    .invoke("java/lang/Long", "parseLong", "(Ljava/lang/String;)J",
                            ValType::I64)
                    .raw(op::LReturn)
                    .build();
    RunResult lr = runIt(pl, s);
    CHECK(lr.kind == RunResult::Kind::Completed && lr.returnValue.known &&
          lr.returnValue.intBits == INT64_MIN);

    // valueOf / intValue 往返。
    Method box = MethodBuilder("bx", "(I)I", 1)
                     .iload(0)
                     .invoke("java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;",
                             ValType::Ref)
                     .invokeVirtual("java/lang/Integer", "intValue", "()I", ValType::I32)
                     .ireturn()
                     .build();
    Prefill b41;
    b41.locals = {SimValue::knownI32(41)};
    CHECK(completedWith(runIt(box, s, std::move(b41)), 41));
}

// ── 扩展包：jsim-reflect（provider 注入元数据，不解析 class 文件）──────────

void testReflectPackage() {
    reflect::ClassMeta meta;
    meta.internalName = "dev/x/D";
    reflect::AnnotationData anno;
    anno.type = "dev/x/Anno";
    anno.elements.push_back({"mv", reflect::AnnoValue::int32(100)});
    anno.elements.push_back({"d1", reflect::AnnoValue::textArray({"v1", "v2"})});
    meta.annotations.push_back(std::move(anno));

    Session s;
    reflect::install(s, [&meta](const std::string& n) -> const reflect::ClassMeta* {
        return n == meta.internalName ? &meta : nullptr;
    });

    // forName → getAnnotation → mv() 经兜底解析链。
    Method read = MethodBuilder("rd", "()I", 2)
                      .ldcStr("dev.x.D")
                      .invoke("java/lang/Class", "forName",
                              "(Ljava/lang/String;)Ljava/lang/Class;", ValType::Ref)
                      .astore(0)
                      .aload(0)
                      .ldcClass("dev/x/Anno")
                      .invokeVirtual("java/lang/Class", "getAnnotation",
                                     "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;",
                                     ValType::Ref)
                      .astore(1)
                      .aload(1)
                      .invokeInterface("dev/x/Anno", "mv", "()I", ValType::I32)
                      .ireturn()
                      .build();
    CHECK(completedWith(runIt(read, s, {}, 64), 100));

    // 元数据缺失：getAnnotation 返回 null（真实行为），ifNonNull 不成立。
    Session empty;
    reflect::install(empty,
                     [](const std::string&) -> const reflect::ClassMeta* { return nullptr; });
    Method probe = MethodBuilder("pb", "()I", 0)
                       .ldcStr("dev.x.D")
                       .invoke("java/lang/Class", "forName",
                               "(Ljava/lang/String;)Ljava/lang/Class;", ValType::Ref)
                       .ldcClass("dev/x/Anno")
                       .invokeVirtual("java/lang/Class", "getAnnotation",
                                      "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;",
                                      ValType::Ref)
                       .ifNonNull(6)
                       .iconst(0)
                       .ireturn()
                       .iconst(1)
                       .ireturn()
                       .build();
    CHECK(completedWith(runIt(probe, empty, {}, 64), 0));
}

// ── 扩展包：jsim-indy（最小建模：把 CallSite 指向算出来）───────────────────

void testIndyPackage() {
    Session s;
    indy::install(s);

    // fromMethodDescriptorString：描述符进 MethodType 载荷。
    Method mk = MethodBuilder("mk", "()Ljava/lang/Object;", 0)
                    .ldcStr("(I)V")
                    .invoke("java/lang/invoke/MethodType", "fromMethodDescriptorString",
                            "(Ljava/lang/String;)Ljava/lang/invoke/MethodType;", ValType::Ref)
                    .areturn()
                    .build();
    RunResult mt = runIt(mk, s);
    CHECK(mt.kind == RunResult::Kind::Completed && mt.returnValue.known);
    auto mto = s.heap.find(mt.returnValue.ref.objId);
    CHECK(mto != s.heap.end() && mto->second.className == "java/lang/invoke/MethodType" &&
          mto->second.data == "(I)V");

    // 句柄 → ConstantCallSite.<init> → targetOf 读回同一目标。
    const SimValue handle =
        indy::makeMethodHandle(s, indy::Target::Kind::Virtual, "o/N", "m", "()V");
    Method cs = MethodBuilder("cs", "()Ljava/lang/Object;", 0)
                    .newInst("java/lang/invoke/ConstantCallSite") // [h, site]
                    .dupX1()                                     // [site, h, site]
                    .swap()                                      // [site, site, h]
                    .invokeSpecial("java/lang/invoke/ConstantCallSite", "<init>",
                                   "(Ljava/lang/invoke/MethodHandle;)V")
                    .areturn()
                    .build();
    Prefill withHandle;
    withHandle.stack = {handle};
    RunResult siteR = Sim(cs, s).run(0, std::move(withHandle), Limits{16});
    CHECK(siteR.kind == RunResult::Kind::Completed && siteR.returnValue.known &&
          indy::isCallSite(s, siteR.returnValue));
    indy::Target t;
    CHECK(indy::targetOf(s, siteR.returnValue, t) && t.valid());
    CHECK(t.kind == indy::Target::Kind::Virtual && t.owner == "o/N" && t.name == "m" &&
          t.desc == "()V");
}

} // namespace

int main() {
    testValueAlgebra();
    testArithmetic();
    testBranchFace();
    testCallFace();
    testFieldFace();
    testObservationFace();
    testStartupFace();
    testArraysAndAlloc();
    testClassIdentity();
    testStrPackage();
    testNumPackage();
    testReflectPackage();
    testIndyPackage();
    std::printf("[jsim-test] %d 项断言，%d 失败\n", g_total, g_fail);
    return g_fail == 0 ? 0 : 1;
}
