/// @file test/jbc/main.cpp
/// @brief jsim-jbc 桥接测试（设计 §9(2)：扩展包每条面一测）：合成 class 语料
///        （test/corpus/JsimJbcBridgeSample.class，javac --release 8 产物 v52）
///        → libjbc 会话 → jbcbridge::toSimMethod → jsim 执行，期望值由本文件
///        用同算法独立复算，不以桥自身输出为基准。运行期不需要 JDK。

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "jsim/engine.hpp"
#include "jsim/jbc.hpp"
#include "jsim/str.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace jsim;
using jbcbridge::BridgeError;

namespace fs = std::filesystem;

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

/// 语料定位：CWD 相对优先（xmake run 在仓根），否则沿可执行文件目录逐级
/// 上溯（build/<plat>/<arch>/<mode> 四层到仓根）。与 demo/deobf 同款策略。
fs::path locateCorpus(const char* argv0) {
    const fs::path relative = "test/corpus/JsimJbcBridgeSample.class";
    std::error_code ec;
    if (fs::exists(relative)) return relative;
    fs::path dir = fs::weakly_canonical(fs::path(argv0), ec).parent_path();
    for (int up = 0; up < 6 && !dir.empty(); ++up) {
        const fs::path candidate = dir / relative;
        if (fs::exists(candidate)) return candidate;
        dir = dir.parent_path();
    }
    return {};
}

std::vector<u8> readFileBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::vector<u8>((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
}

/// 消费方最小接管装配：jsim-str 出 String 家族调用语义 + 字符串构造两阶段；
/// Object.<init>()V 是所有 invokespecial 构造链的公共尾巴，消费方自备
///（demo/deobf 同款口径）。三个注册表保持开放，测试按需再加。
Session makeSession() {
    Session session;
    str::install(session);
    str::installStringAlloc(session);
    session.callMocks["java/lang/Object.<init>()V"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> recv = frame.popChecked();
        if (!recv.isOk()) return Result<SimValue, Bail>::err(recv.error());
        return Result<SimValue, Bail>::ok(recv.value());
    };
    return session;
}

/// 建堆对象并返回引用 id（fields 由调用方装填）。
u64 allocObj(Session& s, std::string className) {
    const u64 id = s.nextAllocId++;
    HeapObj obj;
    obj.className = std::move(className);
    s.heap[id] = std::move(obj);
    return id;
}

/// decrypt 期望值复算：out[n-1-i] = (cipher[i] ^ (i+7)) & 0xFFFF，
/// 再把 out 码元按 jsim 家族口径编回字节（str::appendCodeUnit 同码）。
std::string expectedDecrypt(const std::string& cipher) {
    const usize n = str::codeUnitCount(cipher);
    std::vector<i32> out(n, 0);
    for (usize i = 0; i < n; ++i) {
        const i32 c = str::codeUnitAt(cipher, static_cast<usize>(i));
        out[n - 1 - i] = (c ^ static_cast<i32>(i + 7)) & 0xFFFF;
    }
    std::string text;
    for (i32 unit : out) str::appendCodeUnit(text, unit);
    return text;
}

const char* stageName(BridgeError::Stage s) {
    switch (s) {
    case BridgeError::Stage::NoCode: return "NoCode";
    case BridgeError::Stage::PromoteFailed: return "PromoteFailed";
    case BridgeError::Stage::BadDom: return "BadDom";
    case BridgeError::Stage::BadLabel: return "BadLabel";
    case BridgeError::Stage::BadConst: return "BadConst";
    case BridgeError::Stage::BadMemberRef: return "BadMemberRef";
    case BridgeError::Stage::BadDesc: return "BadDesc";
    case BridgeError::Stage::BadClass: return "BadClass";
    case BridgeError::Stage::UnsupportedOp: return "UnsupportedOp";
    }
    return "?";
}

std::shared_ptr<jbc::ir::ClassSession> g_session;

/// 桥接指定方法，失败即 CHECK 失败并返回空 Method 引用（测试继续走断言计数）。
Method bridgeMethod(const char* name, const char* desc) {
    static Method empty;
    const ca::usize idx = g_session->unit().findMethod(name, desc);
    CHECK(idx != jbc::ir::kNoMethod);
    if (idx == jbc::ir::kNoMethod) return empty;
    auto bridged = jbcbridge::toSimMethod(*g_session, idx);
    CHECK(bridged.isOk());
    if (!bridged.isOk()) {
        std::printf("  bridge %s%s failed: %s: %s\n", name, desc,
                    stageName(bridged.error().stage), bridged.error().detail.c_str());
        return empty;
    }    return std::move(bridged.value());
}

/// 在桥出的 Method 里找第一条满足谓词的指令下标；找不到返回 -1。
i32 findInsn(const Method& m, bool (*pred)(const Insn&, const Method&)) {
    for (u32 i = 0; i < m.code.size(); ++i) {
        if (pred(m.code[i], m)) return static_cast<i32>(i);
    }
    return -1;
}

// ── 1. 解密器形：桥 + 形态 + 端到端执行 ────────────────────────────────────

void testDecrypt() {
    Method m = bridgeMethod("decrypt", "(Ljava/lang/String;)Ljava/lang/String;");
    CHECK(m.code.size() > 10);
    CHECK(m.maxLocals >= 3);

    // 形态：newarray char（atype 在 b 槽 = 5）、castore、String.<init>([C)V、回边。
    const i32 newArrayAt = findInsn(m, [](const Insn& in, const Method&) {
        return in.opcode == op::NewArray && in.b == 5;
    });
    CHECK(newArrayAt >= 0);
    CHECK(findInsn(m, [](const Insn& in, const Method&) { return in.opcode == op::CAStore; })
          >= 0);
    const i32 ctorAt = findInsn(m, [](const Insn& in, const Method& mm) {
        if (in.opcode != op::InvokeSpecial) return false;
        const CallRef& call = mm.calls.at(static_cast<usize>(in.a));
        return call.owner == "java/lang/String" && call.name == "<init>"
               && call.desc == "([C)V" && !call.hasReturn && !call.isStatic;
    });
    CHECK(ctorAt >= 0);
    bool backJump = false;
    for (u32 i = 0; i < m.code.size(); ++i) {
        const Insn& in = m.code[i];
        const bool isJump = in.opcode == op::Goto
                            || (in.opcode >= op::IfEq && in.opcode <= op::IfAcmpne);
        if (isJump && static_cast<u32>(in.a) < i) backJump = true;
    }
    CHECK(backJump);

    // 端到端：密文灌 internedStr（ldc 字面量同款身份口径）→ 逆序异或还原。
    for (const char* cipher : {"bridge-me", "0123456789", "x"}) {
        Session s = makeSession();
        Prefill p;
        p.locals = {SimValue::internedStr(cipher)};
        RunResult r = Sim(m, s).runEntry(std::move(p), 1u << 16);
        CHECK(r.kind == RunResult::Kind::Completed && r.hasReturnValue && r.returnValue.known
              && r.returnValue.ref.kind == RefPayload::Kind::Obj);
        if (r.kind != RunResult::Kind::Completed
            || r.returnValue.ref.kind != RefPayload::Kind::Obj) {
            continue;
        }
        auto it = s.heap.find(r.returnValue.ref.objId);
        CHECK(it != s.heap.end());
        if (it != s.heap.end()) {
            CHECK(it->second.data == expectedDecrypt(cipher));
        }
    }
}

// ── 2. ldc 全家桶与宽窄混合求值 ───────────────────────────────────────────

void testLdcFamily() {
    Method m = bridgeMethod("wide", "()D");
    CHECK(m.cp.size() >= 4); // long / double / float / int 各一条

    Session s = makeSession();
    RunResult r = Sim(m, s).runEntry(Prefill{}, 256);
    CHECK(r.kind == RunResult::Kind::Completed && r.hasReturnValue && r.returnValue.known
          && r.returnValue.type == ValType::F64);

    // 期望值独立复算，求值序与 javac 产出一致：((l2d + d) + f2d) + i2d。
    const i64 l = static_cast<i64>(0x1122334455667788ULL);
    const f64 d = 3.141592653589793;
    const f64 f = 2.5;
    const f64 i = 1234567.0;
    const f64 expect = ((static_cast<f64>(l) + d) + f) + i;
    CHECK(r.returnValue.asF64() == expect);

    // ldc 类常量：压出的 Class 对象走 classObjectOf 单例口径。
    Method cm = bridgeMethod("classConst", "()Ljava/lang/Class;");
    Session cs = makeSession();
    RunResult cr = Sim(cm, cs).runEntry(Prefill{}, 256);
    CHECK(cr.kind == RunResult::Kind::Completed && cr.returnValue.known
          && cr.returnValue.ref.kind == RefPayload::Kind::Obj);
    if (cr.returnValue.ref.kind == RefPayload::Kind::Obj) {
        auto it = cs.heap.find(cr.returnValue.ref.objId);
        CHECK(it != cs.heap.end());
        if (it != cs.heap.end()) {
            CHECK(it->second.className == "java/lang/Class");
            CHECK(it->second.data == "JsimJbcBridgeSample");
        }
    }
}

// ── 3. switch 双形：tableswitch / lookupswitch ─────────────────────────────

void testSwitch() {
    Method dense = bridgeMethod("dense", "(I)I");
    CHECK(dense.switchTables.size() == 1);
    if (dense.switchTables.size() == 1) {
        const SwitchTable& t = dense.switchTables.front();
        CHECK(t.min == 10 && t.max == 12);
        CHECK(t.targets.size() == 3);
        CHECK(t.targets[0] != t.targets[1] && t.targets[1] != t.targets[2]);
    }
    Session s = makeSession();
    const struct { i32 in; i32 want; } denseCases[] = {
        {10, 100}, {11, 111}, {12, 122}, {13, -1}, {9, -1}, {0, -1},
    };
    for (const auto& c : denseCases) {
        Prefill p;
        p.locals = {SimValue::knownI32(c.in)};
        RunResult r = Sim(dense, s).runEntry(std::move(p), 256);
        CHECK(r.kind == RunResult::Kind::Completed && r.hasReturnValue && r.returnValue.known
              && static_cast<i32>(r.returnValue.asU32()) == c.want);
    }

    Method sparse = bridgeMethod("sparse", "(I)I");
    CHECK(sparse.lookupTables.size() == 1);
    if (sparse.lookupTables.size() == 1) {
        const LookupTable& t = sparse.lookupTables.front();
        CHECK(t.pairs.size() == 3);
        bool saw1 = false, saw1000 = false, saw100000 = false;
        for (const auto& pair : t.pairs) {
            saw1 = saw1 || pair.first == 1;
            saw1000 = saw1000 || pair.first == 1000;
            saw100000 = saw100000 || pair.first == 100000;
        }
        CHECK(saw1 && saw1000 && saw100000);
    }
    const struct { i32 in; i32 want; } sparseCases[] = {
        {1, 10}, {1000, 10000}, {100000, 1000000}, {7, 0}, {-1, 0},
    };
    for (const auto& c : sparseCases) {
        Prefill p;
        p.locals = {SimValue::knownI32(c.in)};
        RunResult r = Sim(sparse, s).runEntry(std::move(p), 256);
        CHECK(r.kind == RunResult::Kind::Completed && r.hasReturnValue && r.returnValue.known
              && static_cast<i32>(r.returnValue.asU32()) == c.want);
    }
}

// ── 4. 循环与回边执行（iinc + 条件跳转） ──────────────────────────────────

void testLoop() {
    Method m = bridgeMethod("sum", "(I)J");
    Session s = makeSession();
    Prefill p;
    p.locals = {SimValue::knownI32(10)};
    RunResult r = Sim(m, s).runEntry(std::move(p), 1 << 12);
    i64 want = 0;
    for (i32 i = 1; i <= 10; ++i) want += static_cast<i64>(i) * i; // 385
    CHECK(r.kind == RunResult::Kind::Completed && r.hasReturnValue && r.returnValue.known
          && r.returnValue.type == ValType::I64 && r.returnValue.intBits == want);
}

// ── 5. statics 接力棒：<clinit> 先跑 putstatic，withSeed 同 Session 读回 ───

void testStaticsRelay() {
    Method clinit = bridgeMethod("<clinit>", "()V");
    Method withSeed = bridgeMethod("withSeed", "(I)J");
    CHECK(withSeed.fields.size() == 1);
    if (!withSeed.fields.empty()) {
        const FieldRef& field = withSeed.fields.front();
        CHECK(field.owner == "JsimJbcBridgeSample" && field.name == "seed"
              && field.desc == "J" && field.type == ValType::I64);
    }

    Session s = makeSession();
    RunResult c = Sim(clinit, s).runEntry(Prefill{}, 256);
    CHECK(c.kind == RunResult::Kind::Completed && !c.hasReturnValue);
    const std::string key = "JsimJbcBridgeSample::seed::J";
    CHECK(s.statics.count(key) == 1 && s.statics[key].known
          && s.statics[key].intBits == static_cast<i64>(0x5A5A5A5A5A5A5A5AULL));

    Prefill p;
    p.locals = {SimValue::knownI32(3)};
    RunResult r = Sim(withSeed, s).runEntry(std::move(p), 256);
    CHECK(r.kind == RunResult::Kind::Completed && r.hasReturnValue && r.returnValue.known
          && r.returnValue.intBits == static_cast<i64>(0x5A5A5A5A5A5A5A5AULL) + 3);
}

// ── 6. invoke 四形：static / virtual / special / interface ─────────────────

void testInvokeFamily() {
    // invokestatic：CallRef.isStatic + 签名接管 + 未注册走 UnsupportedCall。
    Method m = bridgeMethod("viaStatic", "(II)I");
    CHECK(m.calls.size() == 2);
    if (m.calls.size() == 2) {
        CHECK(m.calls[0].owner == "JsimJbcBridgeSample" && m.calls[0].name == "dense"
              && m.calls[0].desc == "(I)I" && m.calls[0].isStatic && m.calls[0].hasReturn
              && m.calls[0].retType == ValType::I32);
        CHECK(m.calls[1].name == "sparse" && m.calls[1].isStatic);
    }

    Session bare = makeSession();
    Prefill p0;
    p0.locals = {SimValue::knownI32(10), SimValue::knownI32(1000)};
    RunResult unmocked = Sim(m, bare).runEntry(std::move(p0), 256);
    CHECK(unmocked.kind == RunResult::Kind::Bailed
          && unmocked.bail.reason == Bail::Reason::UnsupportedCall);

    Session s = makeSession();
    s.callMocks["JsimJbcBridgeSample.dense(I)I"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> arg = frame.popChecked();
        if (!arg.isOk()) return Result<SimValue, Bail>::err(arg.error());
        return Result<SimValue, Bail>::ok(SimValue::knownI32(static_cast<i32>(arg.value().asU32()) * 2));
    };
    s.callMocks["JsimJbcBridgeSample.sparse(I)I"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> arg = frame.popChecked();
        if (!arg.isOk()) return Result<SimValue, Bail>::err(arg.error());
        return Result<SimValue, Bail>::ok(SimValue::knownI32(static_cast<i32>(arg.value().asU32()) * 3));
    };
    Prefill p;
    p.locals = {SimValue::knownI32(10), SimValue::knownI32(1000)};
    RunResult r = Sim(m, s).runEntry(std::move(p), 256);
    CHECK(r.kind == RunResult::Kind::Completed && r.hasReturnValue && r.returnValue.known
          && static_cast<i32>(r.returnValue.asU32()) == 10 * 2 + 1000 * 3);

    // invokevirtual：String.length 走 jsim-str 的签名接管。
    Method ht = bridgeMethod("heldTwice", "(Ljava/lang/String;)I");
    CHECK(findInsn(ht, [](const Insn& in, const Method& mm) {
        if (in.opcode != op::InvokeVirtual) return false;
        const CallRef& call = mm.calls.at(static_cast<usize>(in.a));
        return call.owner == "java/lang/String" && call.name == "length" && call.desc == "()I"
               && !call.isStatic;
    }) >= 0);
    Session hs = makeSession();
    const u64 self = allocObj(hs, "JsimJbcBridgeSample");
    hs.heap[self].fields["held::I"] = SimValue::knownI32(21);
    Prefill hp;
    hp.locals = {SimValue::knownObj(self), SimValue::knownStr("abcd")};
    RunResult hr = Sim(ht, hs).runEntry(std::move(hp), 256);
    CHECK(hr.kind == RunResult::Kind::Completed && hr.hasReturnValue && hr.returnValue.known
          && static_cast<i32>(hr.returnValue.asU32()) == 21 * 2 + 4);

    // invokeinterface：嵌套接口引用按 InterfaceMethodref 桥出。
    Method tp = bridgeMethod("throughProbe", "(LJsimJbcBridgeSample$Probe;I)I");
    bool sawProbe = false;
    for (const CallRef& call : tp.calls) {
        sawProbe = sawProbe || (call.owner == "JsimJbcBridgeSample$Probe"
                                && call.name == "probe" && call.desc == "(I)I"
                                && !call.isStatic && call.hasReturn);
    }
    CHECK(sawProbe);
    Session ps = makeSession();
    const u64 probeSelf = allocObj(ps, "JsimJbcBridgeSample");
    ps.heap[probeSelf].fields["held::I"] = SimValue::knownI32(5);
    ps.callMocks["JsimJbcBridgeSample$Probe.probe(I)I"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> arg = frame.popChecked();
        if (!arg.isOk()) return Result<SimValue, Bail>::err(arg.error());
        Result<SimValue, Bail> recv = frame.popChecked();
        if (!recv.isOk()) return Result<SimValue, Bail>::err(recv.error());
        return Result<SimValue, Bail>::ok(SimValue::knownI32(static_cast<i32>(arg.value().asU32()) * 3));
    };
    Prefill pp;
    pp.locals = {SimValue::knownObj(probeSelf), SimValue::knownNull(),
                 SimValue::knownI32(4)};
    RunResult pr = Sim(tp, ps).runEntry(std::move(pp), 256);
    CHECK(pr.kind == RunResult::Kind::Completed && pr.hasReturnValue && pr.returnValue.known
          && static_cast<i32>(pr.returnValue.asU32()) == 4 * 3 + 5);
}

// ── 7. 构造器：invokespecial 链 + putfield 落堆 ────────────────────────────

void testCtor() {
    Method m = bridgeMethod("<init>", "(I)V");
    bool sawSuperCtor = false;
    for (const CallRef& call : m.calls) {
        sawSuperCtor = sawSuperCtor || (call.owner == "java/lang/Object" && call.name == "<init>"
                                        && call.desc == "()V" && !call.hasReturn);
    }
    CHECK(sawSuperCtor);
    CHECK(!m.fields.empty());
    if (!m.fields.empty()) {
        CHECK(m.fields.front().name == "held" && m.fields.front().desc == "I");
    }

    Session s = makeSession();
    const u64 self = allocObj(s, "JsimJbcBridgeSample");
    Prefill p;
    p.locals = {SimValue::knownObj(self), SimValue::knownI32(42)};
    RunResult r = Sim(m, s).runEntry(std::move(p), 256);
    CHECK(r.kind == RunResult::Kind::Completed && !r.hasReturnValue);
    auto it = s.heap.find(self);
    CHECK(it != s.heap.end());
    if (it != s.heap.end()) {
        auto slot = it->second.fields.find("held::I");
        CHECK(slot != it->second.fields.end() && slot->second.known
              && static_cast<i32>(slot->second.asU32()) == 42);
    }
}

// ── 8. multianewarray：元素类型剥维 + 最外层长度 ───────────────────────────

void testMultiArray() {
    Method m = bridgeMethod("gridDim", "(I)I");
    const i32 at = findInsn(m, [](const Insn& in, const Method&) {
        return in.opcode == op::MultiANewArray;
    });
    CHECK(at >= 0);
    if (at >= 0) {
        const Insn& in = m.code.at(static_cast<usize>(at));
        CHECK(in.b == 2); // 维数
        CHECK(m.allocTypes.at(static_cast<usize>(in.a)) == "I"); // [[I 剥 2 层
    }
    Session s = makeSession();
    Prefill p;
    p.locals = {SimValue::knownI32(4)};
    RunResult r = Sim(m, s).runEntry(std::move(p), 256);
    CHECK(r.kind == RunResult::Kind::Completed && r.hasReturnValue && r.returnValue.known
          && static_cast<i32>(r.returnValue.asU32()) == 4);
}

// ── 9. LDC 字符串面：读密文（MUTF-8 原样）+ setLdcString 明文改写回读 ─────

void testLdcStringSurface() {
    const ca::usize idx = g_session->unit().findMethod("lambda$lambdaSite$0", "()V");
    CHECK(idx != jbc::ir::kNoMethod);
    if (idx == jbc::ir::kNoMethod) return;
    auto promoted = jbc::ir::promoteMethod(*g_session, idx);
    CHECK(promoted.is_ok());
    if (promoted.is_err()) return;
    jbc::ir::CodeUnitRef cu = promoted.unwrap();

    jbc::ir::InsnId ldcId{};
    std::string cipher;
    for (const jbc::ir::InsnId id : cu->dom.order()) {
        const jbc::ir::Insn* in = cu->dom.peek(id);
        if (in == nullptr) continue;
        if (jbcbridge::ldcStringOf(*g_session, *in, cipher)) {
            ldcId = id;
            break;
        }
    }
    CHECK(ldcId != jbc::ir::kNoInsn);
    CHECK(cipher == "lambda");

    // setLdcString：CP 追加去重 + LDC→LDC_W 自动升格由 libjbc 负责，桥只透传。
    CHECK(jbcbridge::setLdcString(*g_session, cu->dom, ldcId, "plain"));
    auto lowered = jbc::ir::lowerMethod(*g_session, idx);
    CHECK(lowered.is_ok());
    auto written = jbc::core::writeClass(g_session->backing());
    CHECK(written.is_ok());
    if (written.is_ok()) {
        auto reopened = jbc::ir::makeSessionFromBytes(
            jbc::core::ByteView{written.unwrap()});
        CHECK(reopened.is_ok());
        if (reopened.is_ok()) {
            std::shared_ptr<jbc::ir::ClassSession> again = reopened.unwrap();
            const ca::usize m2 = again->unit().findMethod("lambda$lambdaSite$0", "()V");
            CHECK(m2 != jbc::ir::kNoMethod);
            if (m2 != jbc::ir::kNoMethod) {
                auto cu2 = jbc::ir::promoteMethod(*again, m2);
                CHECK(cu2.is_ok());
                if (cu2.is_ok()) {
                    std::string got;
                    bool sawPlain = false;
                    for (const jbc::ir::InsnId id : cu2.unwrap()->dom.order()) {
                        const jbc::ir::Insn* in = cu2.unwrap()->dom.peek(id);
                        if (in != nullptr
                            && jbcbridge::ldcStringOf(*again, *in, got) && got == "plain") {
                            sawPlain = true;
                        }
                    }
                    CHECK(sawPlain);
                }
            }
        }
    }
}

// ── 10. 失败面：invokedynamic 整方法拒绝，不产半截方法 ─────────────────────

void testRejects() {
    const ca::usize idx = g_session->unit().findMethod("lambdaSite", "()Ljava/lang/Runnable;");
    CHECK(idx != jbc::ir::kNoMethod);
    if (idx == jbc::ir::kNoMethod) return;
    auto bridged = jbcbridge::toSimMethod(*g_session, idx);
    CHECK(!bridged.isOk());
    if (!bridged.isOk()) {
        CHECK(bridged.error().stage == BridgeError::Stage::UnsupportedOp);
        CHECK(bridged.error().detail.find("invokedynamic") != std::string::npos);
        std::printf("  lambdaSite 拒绝（预期）：%s: %s\n",
                    stageName(bridged.error().stage), bridged.error().detail.c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8); // 报告含非 ASCII 标签
#endif
    std::printf("[jsim-jbc-test] 桥接正确性最小面：合成 class 语料 → libjbc → 桥 → jsim 执行\n");

    const fs::path corpus = locateCorpus(argc > 0 ? argv[0] : "");
    if (corpus.empty()) {
        std::printf("找不到 test/corpus/JsimJbcBridgeSample.class（CWD 与可执行文件目录均未命中）\n");
        return 1;
    }
    const std::vector<u8> bytes = readFileBytes(corpus);
    if (bytes.empty()) {
        std::printf("语料读取失败：%s\n", corpus.string().c_str());
        return 1;
    }

    auto sessionRes = jbc::ir::makeSessionFromBytes(jbc::core::ByteView{bytes});
    if (sessionRes.is_err()) {
        std::printf("libjbc 建会话失败（stage=%d）\n",
                    static_cast<int>(sessionRes.unwrap_err().stage));
        return 1;
    }
    g_session = sessionRes.unwrap();
    CHECK(g_session->backing().state() == jbc::core::ParseState::Validated);
    CHECK(g_session->backing().major == 52); // javac --release 8

    testDecrypt();
    testLdcFamily();
    testSwitch();
    testLoop();
    testStaticsRelay();
    testInvokeFamily();
    testCtor();
    testMultiArray();
    testLdcStringSurface();
    testRejects();

    std::printf("jsim-jbc 桥接测试：%d/%d 通过\n", g_total - g_fail, g_total);
    if (g_fail != 0) std::printf("存在失败断言\n");
    return g_fail == 0 ? 0 : 1;
}
