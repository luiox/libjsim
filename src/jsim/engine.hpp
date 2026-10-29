/// @file engine.hpp
/// @brief 单方法引擎（设计 §5）：入口、预填、类型传播主循环、接管面、Bail。

#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "jsim/handler.hpp"
#include "jsim/method.hpp"
#include "jsim/value.hpp"

namespace jsim {

/// MVP 版 Result 替身；接入时换 libca 的 Result<T, E>。
template <class T, class E>
class Result {
public:
    static Result ok(T v) {
        Result r;
        r.hasValue_ = true;
        r.value_ = std::move(v);
        return r;
    }

    static Result err(E e) {
        Result r;
        r.hasValue_ = false;
        r.error_ = std::move(e);
        return r;
    }

    bool isOk() const { return hasValue_; }
    T& value() { return value_; }
    const T& value() const { return value_; }
    E& error() { return error_; }
    const E& error() const { return error_; }

private:
    bool hasValue_ = false;
    T value_{};
    E error_{};
};

/// 结构化失败（设计 §1 失败面）。Bail 是一等公民：带原因码、pc 与现场说明，
/// 调用方决定降级策略；框架不崩溃、不静默走错。步数超限走 RunResult::LimitHit，
/// 不经 Bail。
struct Bail {
    enum class Reason : u8 {
        UnsupportedOpcode,
        UnsupportedCall,
        UnsupportedAlloc,
        NotConcrete,
        BadOperand,
        StackBroken,
        OffMethodEnd,
    };

    Reason reason = Reason::UnsupportedOpcode;
    u32 pc = 0;
    std::string detail;
};

/// 会话堆：allocId → 对象。载荷由注册它的脚手架（扩展包）解释，核心不认内容。
struct HeapObj {
    std::string className;
    std::string data;     // MVP 脚手架私有载荷；正式版按类分型
    std::vector<i32> ints; // "[I" int 数组脚手架载荷（JVM 指令集语义，非 JDK）
};

class Session;

/// 完整进入帧视图（设计 §6 红线：钩子与接管收完整帧，引擎不预弹栈）。
class Frame {
public:
    Frame(std::vector<SimValue>& stack, u32 pc, const Method& method, Session& session)
        : stack_(&stack), pc_(pc), method_(&method), session_(&session) {}

    u32 pc() const { return pc_; }
    const Method& method() const { return *method_; }
    Session& session() const { return *session_; }

    /// 受检弹栈：下溢返回 Bail{StackBroken}，不产生未定义行为。
    Result<SimValue, Bail> popChecked() {
        if (stack_->empty()) {
            return Result<SimValue, Bail>::err({Bail::Reason::StackBroken, pc_, "栈下溢"});
        }
        SimValue v = stack_->back();
        stack_->pop_back();
        return Result<SimValue, Bail>::ok(v);
    }

    /// 受检窥视（不弹出）：接管方先看栈顶形态再决定弹多少时用。
    Result<SimValue, Bail> peekChecked(usize fromTop = 0) const {
        if (stack_->size() <= fromTop) {
            return Result<SimValue, Bail>::err({Bail::Reason::StackBroken, pc_, "窥视越界"});
        }
        return Result<SimValue, Bail>::ok((*stack_)[stack_->size() - 1 - fromTop]);
    }

    void push(SimValue v) { stack_->push_back(std::move(v)); }

private:
    std::vector<SimValue>* stack_ = nullptr;
    u32 pc_ = 0;
    const Method* method_ = nullptr;
    Session* session_ = nullptr;
};

/// 调用接管：扩展包与用户走同一注册面（设计 §6/§7），核心无特权内建。
/// 返回 Known/Unknown 值即接管语义；返回 Bail 即该调用点无法继续。
using CallMock = std::function<Result<SimValue, Bail>(Frame&)>;

/// 分支接管（设计 §4.4 三态）：返回 true/false 即单边裁决，返回 Bail 即放弃。
using BranchHook = std::function<Result<bool, Bail>(u32 pc)>;

/// 会话：接管注册表、分配脚手架、堆、静态字段、观测口。核心不内置任何 JDK
/// 语义（设计 D10）；静态字段值是「多方法编排」的接力棒——clinit 先跑，
/// 后续方法读同一 Session。
struct Session {
    std::unordered_map<std::string, CallMock> callMocks;  // 键 = owner.name+desc
    std::unordered_map<u32, CallMock> callMocksAtPc;      // 按调用点注册，优先于按签名
    std::unordered_map<u32, BranchHook> branchHooks;      // 分支接管，按 pc
    std::unordered_map<std::string, std::function<std::string()>> allocScaffolds; // 类名 → 初始载荷工厂
    std::unordered_map<u64, HeapObj> heap;
    std::unordered_map<std::string, SimValue> statics;    // 键 = owner::name::desc
    std::function<void(u32 pc, const char* opcode, const std::string& stackText)> trace;
    u64 nextAllocId = 1;

    void observe(u32 pc, u8 opcode, const std::vector<SimValue>& stack) {
        if (!trace) return;
        std::string text = "[";
        for (usize i = 0; i < stack.size(); ++i) {
            if (i != 0) text += ", ";
            text += toDisplay(stack[i]);
        }
        text += "]";
        trace(pc, opcodeName(opcode), text);
    }
};

/// 预填帧：参数与栈由用户灌值（Known 或 Unknown），类型由调用方声明——
/// 「用户自己初始化参数值」落到这里。栈支持两种灌法，stackAt 优先：
/// stack 按下标即 push 序（0=栈底），stackAt 指定栈位填值，不依赖书写顺序。
struct Prefill {
    std::vector<SimValue> locals;
    std::vector<SimValue> stack;
    std::unordered_map<usize, SimValue> stackAt;
};

struct Limits {
    u64 maxSteps = 4096;
};

/// 运行结果三态（设计 D6）：Completed 带可选返回值 / Bailed 带现场 / LimitHit 带步数。
struct RunResult {
    enum class Kind : u8 {
        Completed,
        Bailed,
        LimitHit,
    };

    Kind kind = Kind::Completed;
    bool hasReturnValue = false;
    SimValue returnValue;
    Bail bail;
    u64 steps = 0;
};

/// 单方法模拟器（设计 D9）：一个实例绑定一个方法，跨方法不递归。
class Sim {
public:
    Sim(const Method& method, Session& session) : method_(&method), session_(&session) {}

    RunResult run(u32 entryPc, Prefill prefill, Limits limits) {
        RunResult out;
        locals_ = std::move(prefill.locals);
        if (locals_.size() < method_->maxLocals) {
            locals_.resize(method_->maxLocals, SimValue::unknownOf(ValType::I32));
        }
        stack_ = std::move(prefill.stack);
        usize deepest = 0;
        for (const auto& slot : prefill.stackAt) {
            deepest = slot.first + 1 > deepest ? slot.first + 1 : deepest;
        }
        if (stack_.size() < deepest) {
            stack_.resize(deepest, SimValue::unknownOf(ValType::I32));
        }
        for (const auto& slot : prefill.stackAt) {
            stack_[slot.first] = slot.second;
        }

        u32 pc = entryPc;
        while (true) {
            if (pc >= method_->code.size()) {
                return finishBail(out, {Bail::Reason::OffMethodEnd, pc, "越过方法尾"});
            }
            if (out.steps >= limits.maxSteps) {
                out.kind = RunResult::Kind::LimitHit;
                return out;
            }

            const Insn insn = method_->code[pc];
            const OpInfo* info = infoOf(insn.opcode);
            if (info == nullptr) {
                return finishBail(out, {Bail::Reason::UnsupportedOpcode, pc,
                                        "效应表外 opcode " + std::to_string(insn.opcode)});
            }

            StepResult step = execute(pc, insn, *info);
            out.steps += 1;
            if (step.kind == StepResult::Kind::Return) {
                out.kind = RunResult::Kind::Completed;
                out.hasReturnValue = step.hasReturnValue;
                out.returnValue = std::move(step.value);
                return out;
            }
            if (step.kind == StepResult::Kind::BailOut) {
                return finishBail(out, std::move(step.bail));
            }
            if (step.pushResult) stack_.push_back(std::move(step.pushValue));
            session_->observe(pc, insn.opcode, stack_);
            pc = (step.nextPc == kNextPc) ? pc + 1 : step.nextPc;
        }
    }

private:
    struct StepResult {
        enum class Kind : u8 {
            Next,
            Return,
            BailOut,
        };

        Kind kind = Kind::Next;
        bool pushResult = false; // 值内指令把结果（Known 或正确类型 Unknown）压栈
        u32 nextPc = kNextPc;    // kNextPc = 顺序推进；否则显式目标
        SimValue pushValue;
        SimValue value; // Return 用
        bool hasReturnValue = false;
        Bail bail;
    };

    static RunResult finishBail(RunResult& out, Bail b) {
        out.kind = RunResult::Kind::Bailed;
        out.bail = std::move(b);
        return out;
    }

    static StepResult nextStep(SimValue toPush) {
        StepResult s;
        s.pushResult = true;
        s.pushValue = std::move(toPush);
        return s;
    }

    static StepResult jumpStep(u32 target) {
        StepResult s;
        s.nextPc = target;
        return s;
    }

    static StepResult bailStep(Bail b) {
        StepResult s;
        s.kind = StepResult::Kind::BailOut;
        s.bail = std::move(b);
        return s;
    }

    Result<SimValue, Bail> popChecked(u32 pc) {
        if (stack_.empty()) {
            return Result<SimValue, Bail>::err({Bail::Reason::StackBroken, pc, "栈下溢"});
        }
        SimValue v = stack_.back();
        stack_.pop_back();
        return Result<SimValue, Bail>::ok(v);
    }

    /// 类型传播默认语义（设计 D8）：弹 N 槽，全 Known 算值，否则压正确类型 Unknown。
    StepResult popAndCompute(u32 pc, const OpInfo& info, u32 (*compute)(u32, u32)) {
        std::vector<SimValue> args;
        for (int i = 0; i < info.popSlots; ++i) {
            Result<SimValue, Bail> popped = popChecked(pc);
            if (!popped.isOk()) return bailStep(popped.error());
            args.push_back(std::move(popped.value()));
        }

        bool allKnown = true;
        for (const SimValue& a : args) allKnown = allKnown && a.known;
        if (!allKnown || compute == nullptr) {
            return nextStep(SimValue::unknownOf(info.resultType));
        }

        // args 按弹出序（顶在前）：args.back() = 先压的 value1（栈底）。
        // compute(value1, value2)；二元折叠从栈底向栈顶进行。单操作数时 value1
        // 恒为 0、操作数走 value2 位（如 i2c 只看第二参做掩码）。
        u32 acc = (args.size() == 1) ? compute(0, args.front().asU32()) : args.back().asU32();
        for (usize i = args.size() - 1; i > 0; --i) {
            acc = compute(acc, args[i - 1].asU32());
        }
        return nextStep(SimValue::knownI32(static_cast<i32>(acc)));
    }

    /// cat2 版折叠：u64 位型运算，产出 Known I64。栈上 cat2 是一个条目，
    /// 弹栈形状与 cat1 完全同构。
    StepResult popAndCompute64(u32 pc, const OpInfo& info, u64 (*compute)(u64, u64)) {
        std::vector<SimValue> args;
        for (int i = 0; i < info.popSlots; ++i) {
            Result<SimValue, Bail> popped = popChecked(pc);
            if (!popped.isOk()) return bailStep(popped.error());
            args.push_back(std::move(popped.value()));
        }

        bool allKnown = true;
        for (const SimValue& a : args) allKnown = allKnown && a.known;
        if (!allKnown || compute == nullptr) {
            return nextStep(SimValue::unknownOf(info.resultType));
        }

        u64 acc = (args.size() == 1) ? args.front().asU64() : args.back().asU64();
        for (usize i = args.size() - 1; i > 0; --i) {
            acc = compute(acc, args[i - 1].asU64());
        }
        return nextStep(SimValue::knownI64(static_cast<i64>(acc)));
    }

    StepResult execute(u32 pc, const Insn& insn, const OpInfo& info) {
        switch (info.kind) {
        case InsnKind::Value:
            return executeValue(pc, insn, info);
        case InsnKind::StackOp:
            return executeStackOp(pc, insn);
        case InsnKind::Branch:
            return executeBranch(pc, insn, info);
        case InsnKind::Switch:
            return executeSwitch(pc, insn);
        case InsnKind::Alloc:
            return executeAlloc(pc, insn);
        case InsnKind::Array:
            return executeArray(pc, insn);
        case InsnKind::Field:
            return executeField(pc, insn);
        case InsnKind::BoundaryCall:
            return executeCall(pc, insn);
        case InsnKind::Control:
            return jumpStep(static_cast<u32>(insn.a));
        case InsnKind::Return: {
            StepResult s;
            s.kind = StepResult::Kind::Return;
            if (info.popSlots == 1) {
                Result<SimValue, Bail> popped = popChecked(pc);
                if (!popped.isOk()) return bailStep(popped.error());
                s.hasReturnValue = true;
                s.value = std::move(popped.value());
            }
            return s;
        }
        }
        return bailStep({Bail::Reason::UnsupportedOpcode, pc, "未分类指令"});
    }

    StepResult executeValue(u32 pc, const Insn& insn, const OpInfo& info) {
        switch (insn.opcode) {
        case op::Nop:
            return StepResult{};
        case op::AConstNull:
            return nextStep(SimValue::knownNull());
        case op::IConstM1:
            return nextStep(SimValue::knownI32(-1));
        case op::IConst0:
        case op::IConst1:
        case op::IConst2:
        case op::IConst3:
        case op::IConst4:
        case op::IConst5:
            return nextStep(SimValue::knownI32(insn.opcode - op::IConst0));
        case op::Bipush:
        case op::Sipush:
            return nextStep(SimValue::knownI32(insn.a));
        case op::Ldc: {
            const CpEntry& entry = method_->cp[static_cast<usize>(insn.a)];
            if (entry.kind == CpEntry::Kind::Int) {
                return nextStep(SimValue::knownI32(entry.intValue));
            }
            if (entry.kind == CpEntry::Kind::Long) {
                return nextStep(SimValue::knownI64(entry.longValue));
            }
            return nextStep(SimValue::knownStr(entry.strValue));
        }
        case op::ILoad:
            return nextStep(localOrUnknown(insn.a, ValType::I32));
        case op::ALoad:
            return nextStep(localOrUnknown(insn.a, ValType::Ref));
        case op::IStore:
        case op::AStore: {
            Result<SimValue, Bail> popped = popChecked(pc);
            if (!popped.isOk()) return bailStep(popped.error());
            storeLocal(insn.a, std::move(popped.value()));
            return StepResult{};
        }
        case op::Iinc: {
            SimValue v = localOrUnknown(insn.a, ValType::I32);
            if (v.known) {
                v = SimValue::knownI32(static_cast<i32>(v.intBits) + insn.b);
            }
            storeLocal(insn.a, std::move(v));
            return StepResult{};
        }
        case op::IAdd:
            return popAndCompute(pc, info, [](u32 a, u32 b) { return a + b; });
        case op::ISub:
            return popAndCompute(pc, info, [](u32 a, u32 b) { return a - b; });
        case op::IMul:
            return popAndCompute(pc, info, [](u32 a, u32 b) { return a * b; });
        case op::INeg:
            // 单操作数：操作数在 value2 位，value1 恒 0（见 popAndCompute 注释）。
            return popAndCompute(pc, info,
                                 [](u32 /*value1*/, u32 operand) {
                                     return static_cast<u32>(-static_cast<i32>(operand));
                                 });
        case op::IShl:
            return popAndCompute(pc, info,
                                 [](u32 value1, u32 value2) { return value1 << (value2 & 31u); });
        case op::IShr:
            return popAndCompute(
                pc, info,
                [](u32 value1, u32 value2) { return static_cast<u32>(static_cast<i32>(value1) >> (value2 & 31u)); });
        case op::IUshr:
            return popAndCompute(pc, info, [](u32 value1, u32 value2) { return value1 >> (value2 & 31u); });
        case op::IAnd:
            return popAndCompute(pc, info, [](u32 a, u32 b) { return a & b; });
        case op::Ior:
            return popAndCompute(pc, info, [](u32 a, u32 b) { return a | b; });
        case op::IXor:
            return popAndCompute(pc, info, [](u32 a, u32 b) { return a ^ b; });
        case op::I2b:
            return popAndCompute(
                pc, info,
                [](u32 /*value1*/, u32 operand) {
                    return static_cast<u32>(static_cast<i32>(static_cast<signed char>(operand & 0xFFu)));
                });
        case op::I2c:
            // 单操作数：操作数在 value2 位，value1 恒 0（见 popAndCompute 注释）。
            return popAndCompute(pc, info,
                                 [](u32 /*value1*/, u32 operand) { return operand & 0xFFFFu; });
        case op::I2s:
            return popAndCompute(
                pc, info,
                [](u32 /*value1*/, u32 operand) {
                    return static_cast<u32>(static_cast<i32>(static_cast<short>(operand & 0xFFFFu)));
                });
        case op::LConst0:
            return nextStep(SimValue::knownI64(0));
        case op::LConst1:
            return nextStep(SimValue::knownI64(1));
        case op::I2l: {
            Result<SimValue, Bail> popped = popChecked(pc);
            if (!popped.isOk()) return bailStep(popped.error());
            if (!popped.value().known) {
                return nextStep(SimValue::unknownOf(ValType::I64));
            }
            return nextStep(SimValue::knownI64(static_cast<i64>(static_cast<i32>(popped.value().asU32()))));
        }
        case op::L2i: {
            Result<SimValue, Bail> popped = popChecked(pc);
            if (!popped.isOk()) return bailStep(popped.error());
            if (!popped.value().known) {
                return nextStep(SimValue::unknownOf(ValType::I32));
            }
            return nextStep(SimValue::knownI32(static_cast<i32>(popped.value().asU64())));
        }
        case op::LAdd:
            return popAndCompute64(pc, info, [](u64 a, u64 b) { return a + b; });
        case op::LShl:
            return popAndCompute64(pc, info, [](u64 a, u64 b) { return a << (b & 63u); });
        case op::LShr:
            return popAndCompute64(
                pc, info, [](u64 a, u64 b) { return static_cast<u64>(static_cast<i64>(a) >> (b & 63u)); });
        case op::LUshr:
            return popAndCompute64(pc, info, [](u64 a, u64 b) { return a >> (b & 63u); });
        case op::LAnd:
            return popAndCompute64(pc, info, [](u64 a, u64 b) { return a & b; });
        case op::Lor:
            return popAndCompute64(pc, info, [](u64 a, u64 b) { return a | b; });
        case op::LXor:
            return popAndCompute64(pc, info, [](u64 a, u64 b) { return a ^ b; });
        default:
            return bailStep({Bail::Reason::UnsupportedOpcode, pc,
                             "值内指令缺语义：" + std::string(opcodeName(insn.opcode))});
        }
    }

    StepResult executeStackOp(u32 pc, const Insn& insn) {
        switch (insn.opcode) {
        case op::Pop: {
            Result<SimValue, Bail> popped = popChecked(pc);
            if (!popped.isOk()) return bailStep(popped.error());
            return StepResult{};
        }
        case op::Dup: {
            if (stack_.empty()) return bailStep({Bail::Reason::StackBroken, pc, "栈下溢"});
            stack_.push_back(stack_.back());
            return StepResult{};
        }
        case op::DupX1: {
            if (stack_.size() < 2) {
                return bailStep({Bail::Reason::StackBroken, pc, "dup_x1 需要两个槽位"});
            }
            SimValue top = stack_[stack_.size() - 1];
            SimValue under = stack_[stack_.size() - 2];
            stack_.resize(stack_.size() - 2);
            stack_.push_back(top);
            stack_.push_back(under);
            stack_.push_back(top); // MVP 值拷贝；Ref 句柄别名语义由堆脚手架承载
            return StepResult{};
        }
        case op::DupX2: {
            if (stack_.size() < 3) {
                return bailStep({Bail::Reason::StackBroken, pc, "dup_x2 需要三个槽位"});
            }
            // JVM dup_x2：..., value3, value2, value1 → ..., value1, value3, value2, value1。
            // value1 是栈顶（最后压入）。MVP cat1 版直接搬槽位。
            SimValue top = stack_[stack_.size() - 1];       // value1
            SimValue mid = stack_[stack_.size() - 2];       // value2
            SimValue bottom = stack_[stack_.size() - 3];    // value3
            stack_.resize(stack_.size() - 3);
            stack_.push_back(top);
            stack_.push_back(bottom);
            stack_.push_back(mid);
            stack_.push_back(top);
            return StepResult{};
        }
        case op::Dup2: {
            if (stack_.size() < 2) {
                return bailStep({Bail::Reason::StackBroken, pc, "dup2 需要两个槽位"});
            }
            SimValue under = stack_[stack_.size() - 2];
            SimValue top = stack_[stack_.size() - 1];
            stack_.push_back(under);
            stack_.push_back(top);
            return StepResult{};
        }
        case op::Swap: {
            if (stack_.size() < 2) {
                return bailStep({Bail::Reason::StackBroken, pc, "swap 需要两个槽位"});
            }
            std::swap(stack_[stack_.size() - 1], stack_[stack_.size() - 2]);
            return StepResult{};
        }
        default:
            return bailStep({Bail::Reason::UnsupportedOpcode, pc, "栈指令缺语义"});
        }
    }

    /// 分支纪律（设计 §4.4/D7）：全 Known 单边裁决；Unknown 不 fork，
    /// 交给分支接管三态，未注册接管即 Bail{NotConcrete}。
    StepResult executeBranch(u32 pc, const Insn& insn, const OpInfo& info) {
        std::vector<SimValue> args;
        for (int i = 0; i < info.popSlots; ++i) {
            Result<SimValue, Bail> popped = popChecked(pc);
            if (!popped.isOk()) return bailStep(popped.error());
            args.push_back(std::move(popped.value()));
        }

        bool allKnown = true;
        for (const SimValue& a : args) allKnown = allKnown && a.known;
        if (!allKnown) {
            auto hook = session_->branchHooks.find(pc);
            if (hook == session_->branchHooks.end()) {
                return bailStep({Bail::Reason::NotConcrete, pc, "分支条件不具体且未注册分支接管"});
            }
            BranchHook verdict = hook->second; // 拷贝：接管内若动态改注册表，迭代器会失效
            Result<bool, Bail> ruled = verdict(pc);
            if (!ruled.isOk()) return bailStep(ruled.error());
            return ruled.value() ? jumpStep(static_cast<u32>(insn.a)) : StepResult{};
        }

        auto signedAt = [&args](usize i) -> i32 {
            return static_cast<i32>(args[i].asU32());
        };

        // args 按弹出序（顶在前）：args[0] = value2（后压），args[1] = value1（先压）。
        // JVM if_icmp<cond> 语义是 value1 <cond> value2。单操作数 if<cond> 只有 args[0]。
        bool taken = false;
        switch (insn.opcode) {
        case op::IfEq: taken = signedAt(0) == 0; break;
        case op::IfNe: taken = signedAt(0) != 0; break;
        case op::IfLt: taken = signedAt(0) < 0; break;
        case op::IfGe: taken = signedAt(0) >= 0; break;
        case op::IfGt: taken = signedAt(0) > 0; break;
        case op::IfLe: taken = signedAt(0) <= 0; break;
        case op::IfIcmpeq: taken = signedAt(1) == signedAt(0); break;
        case op::IfIcmpne: taken = signedAt(1) != signedAt(0); break;
        case op::IfIcmplt: taken = signedAt(1) < signedAt(0); break;
        case op::IfIcmpge: taken = signedAt(1) >= signedAt(0); break;
        case op::IfIcmpgt: taken = signedAt(1) > signedAt(0); break;
        case op::IfIcmple: taken = signedAt(1) <= signedAt(0); break;
        default:
            return bailStep({Bail::Reason::UnsupportedOpcode, pc, "分支指令缺语义"});
        }
        return taken ? jumpStep(static_cast<u32>(insn.a)) : StepResult{};
    }

    /// switch 系：键 Known 查表或走 default；Unknown 不 fork，直接 Bail{NotConcrete}
    /// （分支接管面向谓词裁决，不面向键值选择）。
    StepResult executeSwitch(u32 pc, const Insn& insn) {
        Result<SimValue, Bail> popped = popChecked(pc);
        if (!popped.isOk()) return bailStep(popped.error());
        if (!popped.value().known) {
            return bailStep({Bail::Reason::NotConcrete, pc, "switch 键不具体"});
        }

        i32 key = static_cast<i32>(popped.value().asU32());
        if (insn.opcode == op::TableSwitch) {
            const SwitchTable& table = method_->switchTables[static_cast<usize>(insn.a)];
            if (key >= table.min && key <= table.max) {
                return jumpStep(table.targets[static_cast<usize>(key - table.min)]);
            }
            return jumpStep(table.defaultPc);
        }

        const LookupTable& table = method_->lookupTables[static_cast<usize>(insn.a)];
        for (const auto& pair : table.pairs) {
            if (pair.first == key) return jumpStep(pair.second);
        }
        return jumpStep(table.defaultPc);
    }

    /// new 系：只走分配脚手架注册面（扩展包职责），未注册 Bail{UnsupportedAlloc}。
    StepResult executeAlloc(u32 pc, const Insn& insn) {
        const std::string& cls = method_->allocTypes[static_cast<usize>(insn.a)];
        auto scaffold = session_->allocScaffolds.find(cls);
        if (scaffold == session_->allocScaffolds.end()) {
            return bailStep({Bail::Reason::UnsupportedAlloc, pc, "未注册分配脚手架：" + cls});
        }
        u64 id = session_->nextAllocId++;
        session_->heap[id] = HeapObj{cls, scaffold->second()};
        return nextStep(SimValue::knownObj(id));
    }

    /// 数组指令（JVM 指令集语义）：int 数组走会话堆脚手架。newarray 仅收
    /// primitive atype（4..11 都按 int 数组建模，布尔/短整差异对 int 折叠面
    /// 不可见）；iaload 在引用/下标未知时按类型传播压 Unknown(I32)。
    StepResult executeArray(u32 pc, const Insn& insn) {
        static constexpr u64 kMaxArrayElems = 1u << 20;

        switch (insn.opcode) {
        case op::NewArray: {
            Result<SimValue, Bail> popped = popChecked(pc);
            if (!popped.isOk()) return bailStep(popped.error());
            if (!popped.value().known) {
                return bailStep({Bail::Reason::NotConcrete, pc, "newarray 长度不具体"});
            }
            const i32 count = static_cast<i32>(popped.value().asU32());
            if (count < 0 || static_cast<u64>(count) > kMaxArrayElems) {
                return bailStep({Bail::Reason::BadOperand, pc, "newarray 长度越界"});
            }
            if (insn.b < 4 || insn.b > 11) {
                return bailStep({Bail::Reason::UnsupportedOpcode, pc, "newarray atype 非primitive"});
            }
            const u64 id = session_->nextAllocId++;
            session_->heap[id] = HeapObj{"[I", {}, std::vector<i32>(static_cast<usize>(count), 0)};
            return nextStep(SimValue::knownObj(id));
        }
        case op::IALoad: {
            Result<SimValue, Bail> idx = popChecked(pc);
            if (!idx.isOk()) return bailStep(idx.error());
            Result<SimValue, Bail> ref = popChecked(pc);
            if (!ref.isOk()) return bailStep(ref.error());
            if (!ref.value().known) {
                return nextStep(SimValue::unknownOf(ValType::I32));
            }
            if (ref.value().ref.kind != RefPayload::Kind::Obj) {
                return bailStep({Bail::Reason::BadOperand, pc, "iaload 接收者非数组引用"});
            }
            auto it = session_->heap.find(ref.value().ref.objId);
            if (it == session_->heap.end() || it->second.className != "[I") {
                return bailStep({Bail::Reason::BadOperand, pc, "iaload 堆对象非 int 数组"});
            }
            if (!idx.value().known) {
                return nextStep(SimValue::unknownOf(ValType::I32));
            }
            const i64 index = static_cast<i64>(idx.value().asU32());
            if (index < 0 || index >= static_cast<i64>(it->second.ints.size())) {
                return bailStep({Bail::Reason::BadOperand, pc, "iaload 下标越界"});
            }
            return nextStep(SimValue::knownI32(it->second.ints[static_cast<usize>(index)]));
        }
        case op::IAStore: {
            Result<SimValue, Bail> value = popChecked(pc);
            if (!value.isOk()) return bailStep(value.error());
            Result<SimValue, Bail> idx = popChecked(pc);
            if (!idx.isOk()) return bailStep(idx.error());
            Result<SimValue, Bail> ref = popChecked(pc);
            if (!ref.isOk()) return bailStep(ref.error());
            // 引用或下标未知时无从落笔：按 MVP 口径整体跳过写（不破坏堆态），
            // 值未知也不写——写进来的假值会污染后续读取的 Known 性。
            if (!ref.value().known || !idx.value().known || !value.value().known) {
                return StepResult{};
            }
            if (ref.value().ref.kind != RefPayload::Kind::Obj) {
                return bailStep({Bail::Reason::BadOperand, pc, "iastore 接收者非数组引用"});
            }
            auto it = session_->heap.find(ref.value().ref.objId);
            if (it == session_->heap.end() || it->second.className != "[I") {
                return bailStep({Bail::Reason::BadOperand, pc, "iastore 堆对象非 int 数组"});
            }
            const i64 index = static_cast<i64>(idx.value().asU32());
            if (index < 0 || index >= static_cast<i64>(it->second.ints.size())) {
                return bailStep({Bail::Reason::BadOperand, pc, "iastore 下标越界"});
            }
            it->second.ints[static_cast<usize>(index)] = static_cast<i32>(value.value().asU32());
            return StepResult{};
        }
        case op::ArrayLength: {
            Result<SimValue, Bail> ref = popChecked(pc);
            if (!ref.isOk()) return bailStep(ref.error());
            if (!ref.value().known) {
                return nextStep(SimValue::unknownOf(ValType::I32));
            }
            if (ref.value().ref.kind != RefPayload::Kind::Obj) {
                return bailStep({Bail::Reason::BadOperand, pc, "arraylength 接收者非数组引用"});
            }
            auto it = session_->heap.find(ref.value().ref.objId);
            if (it == session_->heap.end() || it->second.className != "[I") {
                return bailStep({Bail::Reason::BadOperand, pc, "arraylength 堆对象非 int 数组"});
            }
            return nextStep(SimValue::knownI32(static_cast<i32>(it->second.ints.size())));
        }
        default:
            return bailStep({Bail::Reason::UnsupportedOpcode, pc, "数组指令缺语义"});
        }
    }

    /// 静态字段：getstatic 读 Session.statics（未初始化按类型传播压 Unknown），
    /// putstatic 落值。字段值是 clinit → 解密方法编排的接力棒。
    StepResult executeField(u32 pc, const Insn& insn) {
        const FieldRef& field = method_->fields[static_cast<usize>(insn.a)];
        const std::string key = field.owner + "::" + field.name + "::" + field.desc;

        if (insn.opcode == op::GetStatic) {
            auto it = session_->statics.find(key);
            if (it != session_->statics.end()) {
                SimValue v = it->second;
                v.type = field.type; // 类型是槽位静态事实
                return nextStep(std::move(v));
            }
            return nextStep(SimValue::unknownOf(field.type));
        }

        Result<SimValue, Bail> popped = popChecked(pc);
        if (!popped.isOk()) return bailStep(popped.error());
        session_->statics[key] = std::move(popped.value());
        return StepResult{};
    }

    /// invoke 系：查接管注册（调用点优先于签名），未注册一律 Bail{UnsupportedCall}。
    /// 接收者与实参由接管方自己弹（帧完整可见）；hasReturn=false 不压返回值。
    StepResult executeCall(u32 pc, const Insn& insn) {
        const CallRef& callee = method_->calls[static_cast<usize>(insn.a)];
        std::string key = callee.owner + "." + callee.name + callee.desc;

        CallMock* mock = nullptr;
        auto atPc = session_->callMocksAtPc.find(pc);
        if (atPc != session_->callMocksAtPc.end()) mock = &atPc->second;
        if (mock == nullptr) {
            auto bySig = session_->callMocks.find(key);
            if (bySig != session_->callMocks.end()) mock = &bySig->second;
        }
        if (mock == nullptr) {
            return bailStep({Bail::Reason::UnsupportedCall, pc, "调用未注册接管：" + key});
        }

        CallMock takeover = *mock; // 拷贝：接管内若动态改注册表，指针会悬空
        Frame frame(stack_, pc, *method_, *session_);
        Result<SimValue, Bail> mocked = takeover(frame);
        if (!mocked.isOk()) return bailStep(mocked.error());

        if (!callee.hasReturn) return StepResult{};
        SimValue result = std::move(mocked.value());
        result.type = callee.retType; // 类型是静态事实，来自描述符而非 mock
        return nextStep(std::move(result));
    }

    SimValue localOrUnknown(i32 slot, ValType type) {
        usize index = static_cast<usize>(slot);
        if (index >= locals_.size() || !locals_[index].known) {
            return SimValue::unknownOf(type);
        }
        SimValue v = locals_[index];
        v.type = type;
        return v;
    }

    void storeLocal(i32 slot, SimValue v) {
        usize index = static_cast<usize>(slot);
        if (index >= locals_.size()) {
            locals_.resize(index + 1, SimValue::unknownOf(ValType::I32));
        }
        locals_[index] = std::move(v);
    }

    static constexpr u32 kNextPc = 0xFFFFFFFFu; // 顺序推进哨兵

    const Method* method_ = nullptr;
    Session* session_ = nullptr;
    std::vector<SimValue> locals_;
    std::vector<SimValue> stack_;
};

} // namespace jsim
