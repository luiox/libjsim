/// @file main.cpp
/// @brief libjsim MVP demo：类型传播、接管面、区间起步、扩展包、完整解密方法。

#include <cstdio>

#include "jsim/engine.hpp"
#include "jsim/num.hpp"
#include "jsim/reflect.hpp"
#include "jsim/str.hpp"

using namespace jsim;

namespace {

const char* reasonName(Bail::Reason r) {
    switch (r) {
    case Bail::Reason::UnsupportedOpcode: return "UnsupportedOpcode";
    case Bail::Reason::UnsupportedCall: return "UnsupportedCall";
    case Bail::Reason::UnsupportedAlloc: return "UnsupportedAlloc";
    case Bail::Reason::NotConcrete: return "NotConcrete";
    case Bail::Reason::BadOperand: return "BadOperand";
    case Bail::Reason::StackBroken: return "StackBroken";
    case Bail::Reason::OffMethodEnd: return "OffMethodEnd";
    }
    return "?";
}

void printResult(const RunResult& r) {
    switch (r.kind) {
    case RunResult::Kind::Completed:
        std::printf("→ Completed：%s（%llu 步）\n",
                    r.hasReturnValue ? toDisplay(r.returnValue).c_str() : "void",
                    static_cast<unsigned long long>(r.steps));
        break;
    case RunResult::Kind::Bailed:
        std::printf("→ Bailed[%s] @pc=%u：%s\n", reasonName(r.bail.reason), r.bail.pc,
                    r.bail.detail.c_str());
        break;
    case RunResult::Kind::LimitHit:
        std::printf("→ LimitHit：步数熔断（%llu 步）\n", static_cast<unsigned long long>(r.steps));
        break;
    }
}

/// 用户给的目标方法：真实混淆器 opaque 谓词 + MBA 形态。
/// qZgQ9tTLBwi6uJX(IILdev/realme/cI;Ldev/realme/apt;)I
/// = 常量链 + (i0 & ~常量链)，i1/v2/v3 不参与。
Method buildQZgQ() {
    return MethodBuilder("qZgQ9tTLBwi6uJX", "(IILdev/realme/cI;Ldev/realme/apt;)I", 4)
        .iload(0)
        .ldcI(-2062239700)
        .ldcI(-1731249325)
        .ior()
        .ldcI(-1574409898)
        .ixor()
        .dupX1()
        .iconst(-1)
        .ixor()
        .iand()
        .iadd()
        .ireturn()
        .build();
}

void scenario1() {
    std::printf("\n── 场景 1 · 类型传播：qZgQ9tTLBwi6uJX (IIL...;)I ──\n");
    std::printf("真实混淆器形态：ior/ixor 常量链、dup_x1 槽位重排、iand+iadd 汇入参数。\n");

    Method qp = buildQZgQ();
    Session session;

    // 参数由用户灌值：i0 给 Known，其余按描述符声明类型灌 Unknown。
    Prefill full;
    full.locals = {SimValue::knownI32(7), SimValue::unknownOf(ValType::I32),
                   SimValue::unknownOf(ValType::Ref), SimValue::unknownOf(ValType::Ref)};

    session.trace = [](u32 pc, const char* opcode, const std::string& stackText) {
        std::printf("  [pc %2u] %-12s 栈: %s\n", pc, opcode, stackText.c_str());
    };
    Sim sim(qp, session);
    RunResult r = sim.run(0, std::move(full), Limits{64});
    session.trace = nullptr;
    printResult(r);

    // i0 换成 Unknown：常量链照常折叠，参数未知传染结果，执行不断链。
    Prefill partial;
    partial.locals = {SimValue::unknownOf(ValType::I32), SimValue::unknownOf(ValType::I32),
                      SimValue::unknownOf(ValType::Ref), SimValue::unknownOf(ValType::Ref)};
    RunResult r2 = Sim(qp, session).run(0, std::move(partial), Limits{64});
    std::printf("同一方法，i0 灌 Unknown(I32)：\n");
    printResult(r2);
}

void scenario2() {
    std::printf("\n── 场景 2 · 接管面：invokestatic 不内置，语义用户建模 ──\n");
    std::printf("方法 describe(Ljava/lang/String;)I = lenOf(s) + 1。\n");

    Method desc = MethodBuilder("describe", "(Ljava/lang/String;)I", 1)
                      .aload(0)
                      .invoke("demo", "lenOf", "(Ljava/lang/String;)I", ValType::I32)
                      .iconst(1)
                      .iadd()
                      .ireturn()
                      .build();
    Session session;

    Prefill args;
    args.locals = {SimValue::knownStr("hello")};

    std::printf("未注册接管直接跑：\n");
    printResult(Sim(desc, session).run(0, std::move(args), Limits{64}));

    // 用户建模：lenOf 的语义（MVP mock；正式形态是 jsim-str 扩展包的一条接管）。
    session.callMocks["demo.lenOf(Ljava/lang/String;)I"] =
        [](Frame& f) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> arg = f.popChecked();
        if (!arg.isOk()) return Result<SimValue, Bail>::err(arg.error());
        const SimValue& v = arg.value();
        if (!v.known || v.ref.kind != RefPayload::Kind::Str) {
            return Result<SimValue, Bail>::ok(SimValue::unknownOf(ValType::I32));
        }
        return Result<SimValue, Bail>::ok(SimValue::knownI32(static_cast<i32>(v.ref.str.size())));
    };

    Prefill args2;
    args2.locals = {SimValue::knownStr("hello")};
    std::printf("注册 mock 后重跑：\n");
    printResult(Sim(desc, session).run(0, std::move(args2), Limits{64}));
}

void scenario3() {
    std::printf("\n── 场景 3 · 区间起步：从方法中间某条指令起跑，预填栈 ──\n");

    Method qp = buildQZgQ();
    Session session;

    // C++ 参考实现：常量链与期望值（同一位运算口径）。
    u32 a = static_cast<u32>(static_cast<i32>(-2062239700));
    u32 b = static_cast<u32>(static_cast<i32>(-1731249325));
    u32 c = static_cast<u32>(static_cast<i32>(-1574409898));
    u32 chain = ((a | b) ^ c);
    u32 expected = chain + (7u & (chain ^ 0xFFFFFFFFu));

    // 从 pc=6（dup_x1）起步。pc5 执行完的栈形是 [i0, chain]（i0 栈底、chain 栈顶），
    // 用 stackAt 按栈位灌值（0=栈底），不依赖书写顺序。
    Prefill mid;
    mid.stackAt = {{0, SimValue::knownI32(7)}, {1, SimValue::knownI32(static_cast<i32>(chain))}};
    RunResult r = Sim(qp, session).run(6, std::move(mid), Limits{64});
    printResult(r);

    bool consistent = r.kind == RunResult::Kind::Completed && r.hasReturnValue &&
                      r.returnValue.known && r.returnValue.asU32() == expected;
    std::printf("区间结果与全方法结果一致：%s（参考值 %u）\n", consistent ? "是" : "否", expected);
}

void scenario4() {
    std::printf("\n── 场景 4 · null 是已知 + 步数熔断 ──\n");

    Method tag =
        MethodBuilder("tag", "()Ljava/lang/String;", 0).ldcStr("jsim").areturn().build();
    Method none =
        MethodBuilder("none", "()Ljava/lang/Object;", 0).aconstNull().areturn().build();
    Method spin = MethodBuilder("spin", "()V", 0).iconst(0).gotoPc(0).build();

    Session session;
    std::printf("ldc \"jsim\"; areturn →\n");
    printResult(Sim(tag, session).run(0, Prefill{}, Limits{64}));
    std::printf("aconst_null; areturn →\n");
    printResult(Sim(none, session).run(0, Prefill{}, Limits{64}));
    std::printf("iconst_0; goto 0（自旋）→\n");
    printResult(Sim(spin, session).run(0, Prefill{}, Limits{64}));
}

/// 用户给的完整解密方法（真名是三个空格，这里如实沿用）。
/// 死块 B 保留真实内容（ldc + 未注册接管的 xZ.ceg 调用）：谓词恒假时它一步
/// 不会执行；强行进入死区则按接管面纪律 Bail。D/F/I 死块缩写——它们不参与
/// 执行，只为 pc 布局与原方法一致。原方法 B 块开头的 glfwGetCurrentContext()J
/// + pop2 需要 cat2 记账，随 libjbc 接入补全后原样恢复。
Method buildSpacesDecryptor() {
    return MethodBuilder("   ", "(Ljava/lang/String;)Ljava/lang/String;", 3)
        .newInst("java/lang/StringBuilder")                        // 0
        .dup()                                                     // 1
        .invokeSpecial("java/lang/StringBuilder", "<init>", "()V") // 2
        .astore(1)                                                 // 3
        .iconst(0)                                                 // 4
        .istore(2)                                                 // 5
        .ldcI(1035687926)                                          // 6
        .ldcI(1035687926)                                          // 7
        .ifIcmpeq(17)                                              // 8  恒真 → C
        .ldcI(268470294)                                           // 9
        .ldcI(830660106)                                           // 10
        .ifIcmpge(13)                                              // 11 恒假 → B（死）
        .gotoPc(31)                                                // 12 → F（死）
        .ldcI(-2023458059)                                         // 13 B（死，真实内容）
        .invoke("dev/realme/xZ\u200E", "ceg\u200E", "(I)V", ValType::I32,
                false)                                             // 14 死区调用，未注册接管
        .aconstNull()                                              // 15 B
        .areturn()                                                 // 16 B
        .ldcI(-620390405)                                          // 17 C
        .ldcI(1325597069)                                          // 18
        .dup2()                                                    // 19
        .ior()                                                     // 20
        .dupX2()                                                   // 21
        .pop()                                                     // 22
        .iand()                                                    // 23
        .isub()                                                    // 24 即 A xor B
        .tableswitch(-1811507595, {26, 28, 28}, 17)                // 25 → E
        .aconstNull()                                              // 26 D（死）
        .areturn()                                                 // 27 D
        .ldcI(774051338)                                           // 28 E
        .ldcI(32684743)                                            // 29
        .ifIcmpge(43)                                              // 30 恒真 → H
        .aconstNull()                                              // 31 F（死）
        .areturn()                                                 // 32 F
        .aload(1)                                                  // 33 G 循环体
        .aload(0)                                                  // 34
        .iload(2)                                                  // 35
        .invokeVirtual("java/lang/String", "charAt", "(I)C", ValType::I32) // 36
        .ldcI(1503)                                                // 37
        .ixor()                                                    // 38
        .i2c()                                                     // 39
        .invokeVirtual("java/lang/StringBuilder", "append", "(C)Ljava/lang/StringBuilder;",
                       ValType::Ref)                               // 40
        .pop()                                                     // 41
        .iinc(2, 1)                                                // 42
        .iload(2)                                                  // 43 H 循环头
        .aload(0)                                                  // 44
        .invokeVirtual("java/lang/String", "length", "()I", ValType::I32) // 45
        .ifIcmpge(53)                                              // 46 → J 退出
        .ldcI(546805102)                                           // 47
        .ldcI(990699284)                                           // 48
        .ifIcmpge(51)                                              // 49 恒假 → I（死）
        .gotoPc(33)                                                // 50 → G
        .aconstNull()                                              // 51 I（死）
        .areturn()                                                 // 52 I
        .aload(1)                                                  // 53 J
        .invokeVirtual("java/lang/StringBuilder", "toString", "()Ljava/lang/String;",
                       ValType::Ref)                               // 54
        .areturn()                                                 // 55
        .build();
}

void scenario5() {
    std::printf("\n── 场景 5 · 完整解密方法：\"   \" (Ljava/lang/String;)Ljava/lang/String; ──\n");
    std::printf("恒真谓词 if_icmpeq 单边路由绕开死块；C 块 dup2/ior/dup_x2/iand/isub 折出\n");
    std::printf("tableswitch 键；G/H 循环逐字符 ixor 1503。jsim-str 扩展包一行接入：\n");
    std::printf("  jsim::str::install(session);\n");

    Method decryptor = buildSpacesDecryptor();
    Session session;
    str::install(session);

    Prefill cipher;
    cipher.locals = {SimValue::knownStr("mjt")};
    RunResult r1 = Sim(decryptor, session).run(0, std::move(cipher), Limits{256});
    std::printf("输入 Known(Ref, Str \"mjt\")：\n");
    printResult(r1);

    // xor 对合：把输出喂回同一方法应还原明文——顺带验证码元编解码回环。
    if (r1.kind == RunResult::Kind::Completed && r1.hasReturnValue) {
        Prefill again;
        again.locals = {r1.returnValue};
        RunResult r2 = Sim(decryptor, session).run(0, std::move(again), Limits{256});
        std::printf("把输出再喂回同一方法：\n");
        printResult(r2);
        bool restored = r2.kind == RunResult::Kind::Completed && r2.hasReturnValue &&
                        r2.returnValue.known &&
                        r2.returnValue.ref.kind == RefPayload::Kind::Str &&
                        r2.returnValue.ref.str == "mjt";
        std::printf("还原出原文 \"mjt\"：%s\n", restored ? "是" : "否");
    }
}

/// 死区里的调用真执行了：从死块 B 入口强行起跑，撞上未注册接管的
/// xZ.ceg 调用 → Bail{UnsupportedCall} 结构化放弃。sim 全程只读方法体，
/// Bail 天然无状态可回滚；换路径还是放弃站点由 profile 层决定。
void scenario6() {
    std::printf("\n── 场景 6 · 死区调用的失败面：Bail 放弃而非编造语义 ──\n");
    Method m = buildSpacesDecryptor();
    Session session;
    str::install(session);
    std::printf("从死块 B 入口（pc13）起跑：\n");
    printResult(Sim(m, session).run(13, Prefill{}, Limits{64}));
}

/// 与 Grunt2 的 encrypt 同口径：逐码元 xor 后当 char 写回。
std::string xorEncrypt(const std::string& plain, i32 key) {
    std::string out;
    for (char ch : plain) {
        str::appendCodeUnit(out, static_cast<i32>(ch) ^ key);
    }
    return out;
}

/// BSM 的前半段（照 Grunt2 InvokeDynamicTransformer.createHeavyBootstrap 缩编）：
/// Class.forName → getAnnotation → mv >= 100 → parseInt(d2[0]) % 59557 == 0 → d1[0]。
/// 常量 59557 与 mv 下限 100 都是混淆器的真实取值，不是编的。
Method buildMetaReader() {
    return MethodBuilder("meta", "()Ljava/lang/String;", 3)
        .ldcStr("dev.realme.xD")                                            // 0
        .invoke("java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Class;",
                ValType::Ref)                                               // 1
        .astore(0)                                                          // 2
        .aload(0)                                                           // 3
        .ldcClass("dev/realme/Booger")                                      // 4
        .invokeVirtual("java/lang/Class", "getAnnotation",
                       "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;",
                       ValType::Ref)                                        // 5
        .astore(1)                                                          // 6
        .aload(1)                                                           // 7
        .invokeInterface("dev/realme/Booger", "mv", "()I", ValType::I32)     // 8
        .iconst(100)                                                        // 9
        .ifIcmpge(13)                                                       // 10 版本下限
        .aconstNull()                                                       // 11
        .areturn()                                                          // 12
        .aload(1)                                                           // 13
        .invokeInterface("dev/realme/Booger", "d2", "()[Ljava/lang/String;", ValType::Ref) // 14
        .iconst(0)                                                          // 15
        .aaload()                                                           // 16
        .invoke("java/lang/Integer", "parseInt", "(Ljava/lang/String;)I", ValType::I32) // 17
        .ldcI(59557)                                                        // 18
        .irem()                                                             // 19
        .ifNe(26)                                                           // 20 键不匹配 → 失败块
        .aload(1)                                                           // 21
        .invokeInterface("dev/realme/Booger", "d1", "()[Ljava/lang/String;", ValType::Ref) // 22
        .iconst(0)                                                          // 23
        .aaload()                                                           // 24
        .areturn()                                                          // 25
        .aconstNull()                                                       // 26 失败块
        .areturn()                                                          // 27
        .build();
}

/// 同类里的 decrypt(String)：与 createDecryptMethod 逐条对齐（key 走 LDC）。
Method buildClassDecryptor(i32 key) {
    return MethodBuilder("decrypt", "(Ljava/lang/String;)Ljava/lang/String;", 3)
        .newInst("java/lang/StringBuilder")                                  // 0
        .dup()                                                               // 1
        .invokeSpecial("java/lang/StringBuilder", "<init>", "()V")            // 2
        .astore(1)                                                           // 3
        .iconst(0)                                                           // 4
        .istore(2)                                                           // 5
        .gotoPc(17)                                                          // 6
        .aload(1)                                                            // 7 循环体
        .aload(0)                                                            // 8
        .iload(2)                                                            // 9
        .invokeVirtual("java/lang/String", "charAt", "(I)C", ValType::I32)    // 10
        .ldcI(key)                                                           // 11
        .ixor()                                                              // 12
        .i2c()                                                               // 13
        .invokeVirtual("java/lang/StringBuilder", "append", "(C)Ljava/lang/StringBuilder;",
                       ValType::Ref)                                         // 14
        .pop()                                                               // 15
        .iinc(2, 1)                                                          // 16
        .iload(2)                                                            // 17 循环头
        .aload(0)                                                            // 18
        .invokeVirtual("java/lang/String", "length", "()I", ValType::I32)     // 19
        .ifIcmplt(7)                                                         // 20
        .aload(1)                                                            // 21
        .invokeVirtual("java/lang/StringBuilder", "toString", "()Ljava/lang/String;",
                       ValType::Ref)                                         // 22
        .areturn()                                                           // 23
        .build();
}

/// 场景 8 · 指令接管：instanceof 的答案取决于类型层次，核心不认层次（默认压
/// Unknown），profile 认层次时按 opcode 整条替换。这是「表里有、但默认语义不够」
/// 的那条路——接管优先于效应表，不需要等核心改。
void scenario8() {
    std::printf("\n── 场景 8 · 指令接管：instanceof 的默认 Unknown 与 profile 裁决 ──\n");

    Method m = MethodBuilder("isString", "(Ljava/lang/Object;)I", 1)
                   .aload(0)
                   .raw(op::InstanceOf)
                   .ireturn()
                   .build();

    Session plain;
    Prefill arg;
    arg.locals = {SimValue::knownObj(1)};
    std::printf("未接管（核心只认类型）：\n");
    printResult(Sim(m, plain).run(0, std::move(arg), Limits{16}));

    Session profiled;
    profiled.heap[1] = HeapObj{"java/lang/String", {}, {}, {}, {}};
    profiled.opcodeHandlers[op::InstanceOf] = [](Frame& frame) -> Result<OpOutcome, Bail> {
        Result<SimValue, Bail> recv = frame.popChecked();
        if (!recv.isOk()) return Result<OpOutcome, Bail>::err(recv.error());
        bool isString = false;
        if (recv.value().known && recv.value().ref.kind == RefPayload::Kind::Obj) {
            auto it = frame.session().heap.find(recv.value().ref.objId);
            if (it != frame.session().heap.end()) {
                isString = it->second.className == "java/lang/String";
            }
        }
        frame.push(SimValue::knownI32(isString ? 1 : 0));
        OpOutcome outcome; // nextPc 默认 = 顺序推进
        return Result<OpOutcome, Bail>::ok(outcome);
    };
    Prefill arg2;
    arg2.locals = {SimValue::knownObj(1)};
    std::printf("注册指令接管后（堆对象 className = java/lang/String）：\n");
    printResult(Sim(m, profiled).run(0, std::move(arg2), Limits{16}));
}

/// 场景 7：把「读注解」和「跑解密」串成构建期去混淆的真实编排。
/// 两个扩展包各自 install 一行，核心对注解、String、Integer 一无所知。
void scenario7() {
    std::printf("\n── 场景 7 · 注解驱动的字符串解密（jsim-reflect + jsim-str + jsim-num）──\n");
    std::printf("元数据由 provider 灌入，扩展包不解析 class 文件；解密体逐条对齐\n");
    std::printf("Grunt2 的 createDecryptMethod。\n");

    const i32 key = 1503;
    const std::string plain = "dev/realme/xD";
    const std::string cipher = xorEncrypt(plain, key);

    reflect::ClassMeta meta;
    meta.internalName = "dev/realme/xD";
    reflect::AnnotationData anno;
    anno.type = "dev/realme/Booger";
    anno.elements.push_back({"mv", reflect::AnnoValue::int32(100)});
    anno.elements.push_back({"d1", reflect::AnnoValue::textArray({cipher})});
    anno.elements.push_back({"d2", reflect::AnnoValue::textArray({"59557"})});
    meta.annotations.push_back(std::move(anno));

    Session session;
    str::install(session);
    num::install(session);
    reflect::install(session, [&meta](const std::string& name) -> const reflect::ClassMeta* {
        return name == meta.internalName ? &meta : nullptr;
    });

    RunResult got = Sim(buildMetaReader(), session).run(0, Prefill{}, Limits{64});
    std::printf("forName → getAnnotation → mv 校验 → parseInt(d2[0]) %% 59557 → d1[0]：\n");
    printResult(got);
    if (got.kind != RunResult::Kind::Completed || !got.hasReturnValue) return;

    Prefill arg;
    arg.locals = {got.returnValue};
    RunResult back = Sim(buildClassDecryptor(key), session).run(0, std::move(arg), Limits{512});
    std::printf("把密文喂回 decrypt(String)（key=%d）：\n", key);
    printResult(back);
    const bool restored = back.kind == RunResult::Kind::Completed && back.hasReturnValue &&
                          back.returnValue.known &&
                          back.returnValue.ref.kind == RefPayload::Kind::Str &&
                          back.returnValue.ref.str == plain;
    std::printf("还原出明文 \"%s\"：%s\n", plain.c_str(), restored ? "是" : "否");

    // 元数据缺失的失败面：provider 说不认识这个类，getAnnotation 返回 null，
    // 随后的 invokeinterface 认不出接收者 → 交回核心 Bail，不编造 mv 值。
    Session empty;
    str::install(empty);
    num::install(empty);
    reflect::install(empty, [](const std::string&) -> const reflect::ClassMeta* { return nullptr; });
    std::printf("provider 交不出元数据时：\n");
    printResult(Sim(buildMetaReader(), empty).run(0, Prefill{}, Limits{64}));
}

} // namespace

int main() {
    std::printf("[jsim MVP demo] 单方法引擎：类型传播 + 接管面 + 区间起步 + 扩展包 + 完整解密\n");
    scenario1();
    scenario2();
    scenario3();
    scenario4();
    scenario5();
    scenario6();
    scenario7();
    scenario8();
    return 0;
}
