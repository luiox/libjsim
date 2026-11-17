/// @file engine.cpp
/// @brief 单方法引擎执行实现（doc:design-single）。

#include "jsim/engine.hpp"

#include <cmath>
#include <cstring>

namespace jsim {

void Session::observe(u32 pc, u8 opcode, const std::vector<SimValue>& stack) {
    if (!trace) return;
    std::string text = "[";
    for (usize i = 0; i < stack.size(); ++i) {
        if (i != 0) text += ", ";
        text += toDisplay(stack[i]);
    }
    text += "]";
    trace(pc, opcodeName(opcode), text);
}

u64 Session::classObjectOf(const std::string& internalName) {
    auto it = classObjects.find(internalName);
    if (it != classObjects.end()) return it->second;
    const u64 id = nextAllocId++;
    HeapObj obj;
    obj.className = "java/lang/Class";
    obj.data = internalName;
    heap[id] = std::move(obj);
    classObjects.emplace(internalName, id);
    return id;
}

struct Sim::StepResult {
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

/// 引用相等三态判定（仅对 Known 引用调用，未知引用走分支接管）。
/// Unknown = 身份不可判：非 interned 的 Str 载荷没有第二身份键，真 JVM 里
/// 动态构造的同内容字符串是两个对象，按内容判同会把 == 假性折真。
enum class Sim::RefCmp : u8 {
    Same,
    Diff,
    Unknown,
};

RunResult Sim::runEntry(Prefill prefill, u64 maxSteps) {
    Limits lim;
    lim.maxSteps = maxSteps;
    lim.checkBalance = true;
    return run(0, std::move(prefill), lim);
}

RunResult Sim::runRegion(u32 entryPc, Prefill prefill, u64 maxSteps) {
    Limits lim;
    lim.maxSteps = maxSteps;
    lim.tolerateOffEnd = true;
    return run(entryPc, std::move(prefill), lim);
}

RunResult Sim::run(u32 entryPc, Prefill prefill, Limits limits) {
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

    // 栈平衡记事（design-single）：同 pc 首达记形状，再达比对深度与类型序列。
    std::vector<char> balanceSeen;
    std::vector<std::vector<ValType>> balanceShape;
    if (limits.checkBalance) {
        balanceSeen.assign(method_->code.size(), 0);
        balanceShape.resize(method_->code.size());
    }

    u32 pc = entryPc;
    while (true) {
        if (pc >= method_->code.size()) {
            if (limits.tolerateOffEnd) {
                out.kind = RunResult::Kind::Completed;
                return finish(out);
            }
            return finishBail(out, {Bail::Reason::OffMethodEnd, pc, "越过方法尾"});
        }
        if (out.steps >= limits.maxSteps) {
            out.kind = RunResult::Kind::LimitHit;
            return finish(out);
        }
        if (pc == limits.stopAtPc) {
            // 断点熔断：执行前停，LimitHit 语义，帧快照在 result 里。
            out.kind = RunResult::Kind::LimitHit;
            return finish(out);
        }

        const Insn insn = method_->code[pc];

        if (!session_->stepObservers.empty()) {
            fireStepObservers(pc, insn.opcode);
        }
        if (limits.checkBalance && !balancePass(pc, balanceSeen, balanceShape)) {
            return finishBail(out, {Bail::Reason::StackBroken, pc,
                                    "合并点栈形不一致（深度或类型序列）"});
        }

        // 指令接管优先于效应表（doc:design-takeover）。表外 opcode 因此也能被接管，
        // 「缺什么补什么」不必等核心补表——这是扩展包补环境的最后一层。
        auto handlerIt = session_->opcodeHandlers.find(insn.opcode);
        if (handlerIt != session_->opcodeHandlers.end()) {
            OpHandler handler = handlerIt->second; // 拷贝：接管内改注册表会失效迭代器
            Frame     frame(stack_, locals_, pc, *method_, *session_);
            Result<OpOutcome, Bail> outcome = handler(frame);
            out.steps += 1;
            if (!outcome.isOk()) return finishBail(out, outcome.error());
            OpOutcome& o = outcome.value();
            if (o.doReturn) {
                out.kind = RunResult::Kind::Completed;
                out.hasReturnValue = o.hasReturnValue;
                out.returnValue = std::move(o.returnValue);
                return finish(out);
            }
            session_->observe(pc, insn.opcode, stack_);
            pc = (o.nextPc == kNextPc) ? pc + 1 : o.nextPc;
            continue;
        }

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
            return finish(out);
        }
        if (step.kind == StepResult::Kind::BailOut) {
            return finishBail(out, std::move(step.bail));
        }
        if (step.pushResult) stack_.push_back(std::move(step.pushValue));
        session_->observe(pc, insn.opcode, stack_);
        pc = (step.nextPc == kNextPc) ? pc + 1 : step.nextPc;
    }
}

/// 终止收尾：带上终止帧快照（Completed / Bailed / LimitHit 共用）。
RunResult Sim::finish(RunResult& out) {
    out.finalStack = stack_;
    out.finalLocals = locals_;
    return out;
}

RunResult Sim::finishBail(RunResult& out, Bail b) {
    out.kind = RunResult::Kind::Bailed;
    out.bail = std::move(b);
    if (session_->bailObserver) {
        StepView view{out.bail.pc, 0, &stack_, &locals_, method_, session_};
        session_->bailObserver(out.bail, view);
    }
    return finish(out);
}

/// 步进观测：指令执行前发结构化只读视图。按索引快照数量遍历——观测内
/// 动态注册的新观测下一条指令才生效，不失效迭代器。
void Sim::fireStepObservers(u32 pc, u8 opcode) {
    StepView view{pc, opcode, &stack_, &locals_, method_, session_};
    const usize count = session_->stepObservers.size();
    for (usize i = 0; i < count && i < session_->stepObservers.size(); ++i) {
        if (session_->stepObservers[i]) session_->stepObservers[i](view);
    }
}

/// 合并点栈形校验（design-single）：首达记录深度与类型序列，再达比对。
bool Sim::balancePass(u32 pc, std::vector<char>& seen, std::vector<std::vector<ValType>>& shape) {
    std::vector<ValType> current;
    current.reserve(stack_.size());
    for (const SimValue& v : stack_) current.push_back(v.type);
    if (!seen[pc]) {
        seen[pc] = 1;
        shape[pc] = std::move(current);
        return true;
    }
    return shape[pc] == current;
}

Sim::StepResult Sim::nextStep(SimValue toPush) {
    StepResult s;
    s.pushResult = true;
    s.pushValue = std::move(toPush);
    return s;
}

Sim::StepResult Sim::jumpStep(u32 target) {
    StepResult s;
    s.nextPc = target;
    return s;
}

Sim::StepResult Sim::bailStep(Bail b) {
    StepResult s;
    s.kind = StepResult::Kind::BailOut;
    s.bail = std::move(b);
    return s;
}

Result<SimValue, Bail> Sim::popChecked(u32 pc) {
    if (stack_.empty()) {
        return Result<SimValue, Bail>::err({Bail::Reason::StackBroken, pc, "栈下溢"});
    }
    SimValue v = stack_.back();
    stack_.pop_back();
    return Result<SimValue, Bail>::ok(v);
}

/// 受检窥视（不弹出）：分支在裁决前取操作数形态用。
Result<SimValue, Bail> Sim::peekChecked(u32 pc, usize fromTop) const {
    if (stack_.size() <= fromTop) {
        return Result<SimValue, Bail>::err({Bail::Reason::StackBroken, pc, "窥视越界"});
    }
    return Result<SimValue, Bail>::ok(stack_[stack_.size() - 1 - fromTop]);
}

/// 弹掉 n 个操作数（分支裁决后统一消费）。
Sim::StepResult Sim::dropOperands(u32 pc, u8 n) {
    for (int i = 0; i < n; ++i) {
        Result<SimValue, Bail> popped = popChecked(pc);
        if (!popped.isOk()) return bailStep(popped.error());
    }
    return StepResult{};
}

/// 类型传播默认语义（设计 D8）：弹 N 槽，全 Known 算值，否则压正确类型 Unknown。
Sim::StepResult Sim::popAndCompute(u32 pc, const OpInfo& info, u32 (*compute)(u32, u32)) {
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
Sim::StepResult Sim::popAndCompute64(u32 pc, const OpInfo& info, u64 (*compute)(u64, u64)) {
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

    u64 acc = (args.size() == 1) ? compute(0, args.front().asU64()) : args.back().asU64();
    for (usize i = args.size() - 1; i > 0; --i) {
        acc = compute(acc, args[i - 1].asU64());
    }
    return nextStep(SimValue::knownI64(static_cast<i64>(acc)));
}

/// 纯类型传播：弹掉该弹的槽，压出正确类型的 Unknown。这是「默认什么都不做、
/// 只管类型」的落点——不猜值、不 Bail，执行流不断。
Sim::StepResult Sim::popAndPropagate(u32 pc, const OpInfo& info) {
    for (int i = 0; i < info.popSlots; ++i) {
        Result<SimValue, Bail> popped = popChecked(pc);
        if (!popped.isOk()) return bailStep(popped.error());
    }
    if (info.pushSlots == 0) return StepResult{};
    return nextStep(SimValue::unknownOf(info.resultType));
}

/// 浮点折叠（F32）：弹 info.popSlots 槽，全 Known 算值，否则压 Unknown(F32)。
/// 参数按弹出序（顶在前），args.back() 是先压的 value1，与整数折叠同口径。
template <class Fn>
Sim::StepResult Sim::foldF32(u32 pc, const OpInfo& info, Fn fn) {
    std::vector<SimValue> args;
    for (int i = 0; i < info.popSlots; ++i) {
        Result<SimValue, Bail> popped = popChecked(pc);
        if (!popped.isOk()) return bailStep(popped.error());
        args.push_back(std::move(popped.value()));
    }
    for (const SimValue& a : args) {
        if (!a.known) return nextStep(SimValue::unknownOf(ValType::F32));
    }
    float acc = (args.size() == 1) ? args.front().asF32() : args.back().asF32();
    for (usize i = args.size() - 1; i > 0; --i) {
        acc = static_cast<float>(fn(acc, args[i - 1].asF32()));
    }
    return nextStep(SimValue::knownF32(acc));
}

/// 浮点折叠（F64），口径同 foldF32。
template <class Fn>
Sim::StepResult Sim::foldF64(u32 pc, const OpInfo& info, Fn fn) {
    std::vector<SimValue> args;
    for (int i = 0; i < info.popSlots; ++i) {
        Result<SimValue, Bail> popped = popChecked(pc);
        if (!popped.isOk()) return bailStep(popped.error());
        args.push_back(std::move(popped.value()));
    }
    for (const SimValue& a : args) {
        if (!a.known) return nextStep(SimValue::unknownOf(ValType::F64));
    }
    f64 acc = (args.size() == 1) ? args.front().asF64() : args.back().asF64();
    for (usize i = args.size() - 1; i > 0; --i) {
        acc = fn(acc, args[i - 1].asF64());
    }
    return nextStep(SimValue::knownF64(acc));
}

/// f2i/d2i 的饱和语义（JVMS §6.5）：NaN → 0，越界钳到端点。
i32 Sim::saturateToI32(f64 v) {
    if (std::isnan(v)) return 0;
    if (v >= 2147483647.0) return INT32_MAX;
    if (v <= -2147483648.0) return INT32_MIN;
    return static_cast<i32>(v);
}

i64 Sim::saturateToI64(f64 v) {
    if (std::isnan(v)) return 0;
    if (v >= 9223372036854775807.0) return INT64_MAX;
    if (v <= -9223372036854775808.0) return INT64_MIN;
    return static_cast<i64>(v);
}

Sim::StepResult Sim::execute(u32 pc, const Insn& insn, const OpInfo& info) {
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

Sim::StepResult Sim::executeValue(u32 pc, const Insn& insn, const OpInfo& info) {
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
    case op::Ldc:
    case op::LdcW:
    case op::Ldc2W: {
        const CpEntry& entry = method_->cp[static_cast<usize>(insn.a)];
        if (entry.kind == CpEntry::Kind::Int) {
            return nextStep(SimValue::knownI32(entry.intValue));
        }
        if (entry.kind == CpEntry::Kind::Long) {
            return nextStep(SimValue::knownI64(entry.longValue));
        }
        if (entry.kind == CpEntry::Kind::Float) {
            return nextStep(SimValue::knownF32(bitsToFloat(static_cast<u32>(entry.intValue))));
        }
        if (entry.kind == CpEntry::Kind::Double) {
            return nextStep(SimValue::knownF64(bitsToDouble(static_cast<u64>(entry.longValue))));
        }
        if (entry.kind == CpEntry::Kind::Class) {
            return nextStep(SimValue::knownObj(session_->classObjectOf(entry.className)));
        }
        // ldc 字符串是常量池字面量：intern 事实，身份可判（compareRef）。
        return nextStep(SimValue::internedStr(entry.strValue));
    }
    case op::FConst0:
        return nextStep(SimValue::knownF32(0.0f));
    case op::FConst1:
        return nextStep(SimValue::knownF32(1.0f));
    case op::FConst2:
        return nextStep(SimValue::knownF32(2.0f));
    case op::DConst0:
        return nextStep(SimValue::knownF64(0.0));
    case op::DConst1:
        return nextStep(SimValue::knownF64(1.0));
    case op::ILoad:
        return nextStep(localOrUnknown(insn.a, ValType::I32));
    case op::LLoad:
        return nextStep(localOrUnknown(insn.a, ValType::I64));
    case op::FLoad:
        return nextStep(localOrUnknown(insn.a, ValType::F32));
    case op::DLoad:
        return nextStep(localOrUnknown(insn.a, ValType::F64));
    case op::ALoad:
        return nextStep(localOrUnknown(insn.a, ValType::Ref));
    // 短形式 load/store：0x1a..0x2d / 0x3b..0x4e 各族连续 4 个，槽号 = op - 族首。
    case op::ILoad0:
    case op::ILoad0 + 1:
    case op::ILoad0 + 2:
    case op::ILoad0 + 3:
        return nextStep(localOrUnknown(insn.opcode - op::ILoad0, ValType::I32));
    case op::LLoad0:
    case op::LLoad0 + 1:
    case op::LLoad0 + 2:
    case op::LLoad0 + 3:
        return nextStep(localOrUnknown(insn.opcode - op::LLoad0, ValType::I64));
    case op::FLoad0:
    case op::FLoad0 + 1:
    case op::FLoad0 + 2:
    case op::FLoad0 + 3:
        return nextStep(localOrUnknown(insn.opcode - op::FLoad0, ValType::F32));
    case op::DLoad0:
    case op::DLoad0 + 1:
    case op::DLoad0 + 2:
    case op::DLoad0 + 3:
        return nextStep(localOrUnknown(insn.opcode - op::DLoad0, ValType::F64));
    case op::ALoad0:
    case op::ALoad0 + 1:
    case op::ALoad0 + 2:
    case op::ALoad0 + 3:
        return nextStep(localOrUnknown(insn.opcode - op::ALoad0, ValType::Ref));
    case op::IStore:
    case op::AStore:
    case op::LStore:
    case op::FStore:
    case op::DStore: {
        Result<SimValue, Bail> popped = popChecked(pc);
        if (!popped.isOk()) return bailStep(popped.error());
        storeLocal(insn.a, std::move(popped.value()));
        return StepResult{};
    }
    case op::IStore0:
    case op::IStore0 + 1:
    case op::IStore0 + 2:
    case op::IStore0 + 3:
    case op::LStore0:
    case op::LStore0 + 1:
    case op::LStore0 + 2:
    case op::LStore0 + 3:
    case op::FStore0:
    case op::FStore0 + 1:
    case op::FStore0 + 2:
    case op::FStore0 + 3:
    case op::DStore0:
    case op::DStore0 + 1:
    case op::DStore0 + 2:
    case op::DStore0 + 3:
    case op::AStore0:
    case op::AStore0 + 1:
    case op::AStore0 + 2:
    case op::AStore0 + 3: {
        const u8 base = insn.opcode >= op::AStore0   ? op::AStore0
                        : insn.opcode >= op::DStore0 ? op::DStore0
                        : insn.opcode >= op::FStore0 ? op::FStore0
                        : insn.opcode >= op::LStore0 ? op::LStore0
                                                     : op::IStore0;
        Result<SimValue, Bail> popped = popChecked(pc);
        if (!popped.isOk()) return bailStep(popped.error());
        storeLocal(insn.opcode - base, std::move(popped.value()));
        return StepResult{};
    }
    // checkcast 是恒等：类型检查不改变值，只改静态类型视图。
    case op::CheckCast: {
        Result<SimValue, Bail> popped = popChecked(pc);
        if (!popped.isOk()) return bailStep(popped.error());
        SimValue v = std::move(popped.value());
        v.type = ValType::Ref;
        return nextStep(std::move(v));
    }
    // null 的 instanceof 结果固定为 0。其他引用需要类型信息，留给 opcode 接管。
    case op::InstanceOf: {
        Result<SimValue, Bail> popped = popChecked(pc);
        if (!popped.isOk()) return bailStep(popped.error());
        const SimValue& ref = popped.value();
        if (ref.known && ref.type == ValType::Ref && ref.ref.kind == RefPayload::Kind::Null) {
            return nextStep(SimValue::knownI32(0));
        }
        return nextStep(SimValue::unknownOf(ValType::I32));
    }
    // monitor 不进模型（doc:design-v1-scope）：按类型传播为无操作。
    case op::MonitorEnter:
    case op::MonitorExit: {
        Result<SimValue, Bail> popped = popChecked(pc);
        if (!popped.isOk()) return bailStep(popped.error());
        return StepResult{};
    }
    // athrow 异常表派发。jsim 没有类层次面，派发只做**能静态确证**的匹配
    //（fail-closed：猜错 = 假派发，比放弃更糟）——catch-all（catchType 空）
    // 对任意异常恒成立；具名 catch 要求抛出对象的堆类名与 catchType 精确
    // 相等。类名对不上（可能是子类/无关，无层次面无法区分）或抛出对象类型
    // 未知时一律 Bail。表序即 JVM 派发序，首个可确证表项胜出。
    case op::AThrow: {
        Result<SimValue, Bail> popped = popChecked(pc);
        if (!popped.isOk()) return bailStep(popped.error());
        if (popped.value().type != ValType::Ref) {
            return bailStep({Bail::Reason::BadOperand, pc, "athrow 需要引用"});
        }
        if (popped.value().known && popped.value().ref.kind == RefPayload::Kind::Null) {
            return bailStep({Bail::Reason::NotConcrete, pc, "athrow null 需要隐式 NPE 派发"});
        }
        bool typeKnown = false;
        std::string thrownClass;
        if (popped.value().known && popped.value().ref.kind == RefPayload::Kind::Obj) {
            auto it = session_->heap.find(popped.value().ref.objId);
            if (it != session_->heap.end()) {
                typeKnown = true;
                thrownClass = it->second.className;
            }
        }
        for (const ExRange& ex : method_->handlers) {
            if (!(pc >= ex.startPc && pc < ex.endPc)) continue;
            if (!ex.catchType.empty()) {
                if (!typeKnown || thrownClass != ex.catchType) {
                    return bailStep({Bail::Reason::NotConcrete, pc,
                                     "athrow 异常类型无法静态确证（无类层次面）"});
                }
            }
            stack_.clear(); // JVM 语义：进入处理器前栈清空，仅剩异常对象
            StepResult handler = jumpStep(ex.handlerPc);
            handler.pushResult = true;
            handler.pushValue = popped.value();
            return handler;
        }
        return bailStep({Bail::Reason::NotConcrete, pc, "athrow 无覆盖处理器"});
    }
    case op::Iinc: {
        SimValue v = localOrUnknown(insn.a, ValType::I32);
        if (v.known) {
            const u32 bits = v.asU32() + static_cast<u32>(insn.b);
            v = SimValue::knownI32(static_cast<i32>(bits));
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
        return popAndCompute(pc, info, [](u32, u32 operand) { return u32{0} - operand; });
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
    case op::LSub:
        return popAndCompute64(pc, info, [](u64 a, u64 b) { return a - b; });
    case op::LMul:
        return popAndCompute64(pc, info, [](u64 a, u64 b) { return a * b; });
    case op::LNeg:
        return popAndCompute64(pc, info, [](u64, u64 operand) { return u64{0} - operand; });
    // 整数除法/取余：除数为 0 时 JVM 抛 ArithmeticException，派发需要异常表
    // （v1 不做，doc:design-v1-scope）。这是「已知会发生、我们无法建模」的事件，Bail 比
    // 编一个假商诚实；操作数未知时仍按类型传播，不算也不猜。
    case op::IDiv:
    case op::IRem: {
        std::vector<SimValue> args;
        for (int i = 0; i < info.popSlots; ++i) {
            Result<SimValue, Bail> popped = popChecked(pc);
            if (!popped.isOk()) return bailStep(popped.error());
            args.push_back(std::move(popped.value()));
        }
        for (const SimValue& a : args) {
            if (!a.known) return nextStep(SimValue::unknownOf(ValType::I32));
        }
        const i32 divisor = static_cast<i32>(args[0].asU32());
        const i32 dividend = static_cast<i32>(args[1].asU32());
        if (divisor == 0) {
            return bailStep({Bail::Reason::NotConcrete, pc, "整数除零需异常派发（v1 不做）"});
        }
        // INT_MIN / -1 在 JVM 里定义为 INT_MIN（不抛），取余定义为 0。
        if (dividend == INT32_MIN && divisor == -1) {
            return nextStep(SimValue::knownI32(insn.opcode == op::IDiv ? INT32_MIN : 0));
        }
        return nextStep(SimValue::knownI32(insn.opcode == op::IDiv ? dividend / divisor
                                                                    : dividend % divisor));
    }
    case op::LDiv:
    case op::LRem: {
        std::vector<SimValue> args;
        for (int i = 0; i < info.popSlots; ++i) {
            Result<SimValue, Bail> popped = popChecked(pc);
            if (!popped.isOk()) return bailStep(popped.error());
            args.push_back(std::move(popped.value()));
        }
        for (const SimValue& a : args) {
            if (!a.known) return nextStep(SimValue::unknownOf(ValType::I64));
        }
        const i64 divisor = static_cast<i64>(args[0].asU64());
        const i64 dividend = static_cast<i64>(args[1].asU64());
        if (divisor == 0) {
            return bailStep({Bail::Reason::NotConcrete, pc, "整数除零需异常派发（v1 不做）"});
        }
        if (dividend == INT64_MIN && divisor == -1) {
            return nextStep(SimValue::knownI64(insn.opcode == op::LDiv ? INT64_MIN : 0));
        }
        return nextStep(SimValue::knownI64(insn.opcode == op::LDiv ? dividend / divisor
                                                                   : dividend % divisor));
    }
    // 浮点算术：除零是 IEEE 的 inf/nan，不是异常，照算即可。
    case op::FAdd:
        return foldF32(pc, info, [](float a, float b) { return a + b; });
    case op::FSub:
        return foldF32(pc, info, [](float a, float b) { return a - b; });
    case op::FMul:
        return foldF32(pc, info, [](float a, float b) { return a * b; });
    case op::FDiv:
        return foldF32(pc, info, [](float a, float b) { return a / b; });
    case op::FRem:
        return foldF32(pc, info, [](float a, float b) { return std::fmod(a, b); });
    case op::FNeg:
        return foldF32(pc, info, [](float /*a*/, float b) { return -b; });
    case op::DAdd:
        return foldF64(pc, info, [](f64 a, f64 b) { return a + b; });
    case op::DSub:
        return foldF64(pc, info, [](f64 a, f64 b) { return a - b; });
    case op::DMul:
        return foldF64(pc, info, [](f64 a, f64 b) { return a * b; });
    case op::DDiv:
        return foldF64(pc, info, [](f64 a, f64 b) { return a / b; });
    case op::DRem:
        return foldF64(pc, info, [](f64 a, f64 b) { return std::fmod(a, b); });
    case op::DNeg:
        return foldF64(pc, info, [](f64 /*a*/, f64 b) { return -b; });
    case op::I2f: {
        Result<SimValue, Bail> v = popChecked(pc);
        if (!v.isOk()) return bailStep(v.error());
        if (!v.value().known) return nextStep(SimValue::unknownOf(ValType::F32));
        return nextStep(SimValue::knownF32(static_cast<float>(static_cast<i32>(v.value().asU32()))));
    }
    case op::I2d: {
        Result<SimValue, Bail> v = popChecked(pc);
        if (!v.isOk()) return bailStep(v.error());
        if (!v.value().known) return nextStep(SimValue::unknownOf(ValType::F64));
        return nextStep(SimValue::knownF64(static_cast<f64>(static_cast<i32>(v.value().asU32()))));
    }
    case op::L2f: {
        Result<SimValue, Bail> v = popChecked(pc);
        if (!v.isOk()) return bailStep(v.error());
        if (!v.value().known) return nextStep(SimValue::unknownOf(ValType::F32));
        return nextStep(SimValue::knownF32(static_cast<float>(static_cast<i64>(v.value().asU64()))));
    }
    case op::L2d: {
        Result<SimValue, Bail> v = popChecked(pc);
        if (!v.isOk()) return bailStep(v.error());
        if (!v.value().known) return nextStep(SimValue::unknownOf(ValType::F64));
        return nextStep(SimValue::knownF64(static_cast<f64>(static_cast<i64>(v.value().asU64()))));
    }
    // f2i/l、d2i/l 是饱和转换：NaN → 0，越界钳到端点（JVMS §6.5）。
    case op::F2i:
    case op::D2i: {
        Result<SimValue, Bail> v = popChecked(pc);
        if (!v.isOk()) return bailStep(v.error());
        if (!v.value().known) return nextStep(SimValue::unknownOf(ValType::I32));
        const f64 raw = insn.opcode == op::F2i ? static_cast<f64>(v.value().asF32())
                                               : v.value().asF64();
        return nextStep(SimValue::knownI32(saturateToI32(raw)));
    }
    case op::F2l:
    case op::D2l: {
        Result<SimValue, Bail> v = popChecked(pc);
        if (!v.isOk()) return bailStep(v.error());
        if (!v.value().known) return nextStep(SimValue::unknownOf(ValType::I64));
        const f64 raw = insn.opcode == op::F2l ? static_cast<f64>(v.value().asF32())
                                               : v.value().asF64();
        return nextStep(SimValue::knownI64(saturateToI64(raw)));
    }
    case op::F2d: {
        Result<SimValue, Bail> v = popChecked(pc);
        if (!v.isOk()) return bailStep(v.error());
        if (!v.value().known) return nextStep(SimValue::unknownOf(ValType::F64));
        return nextStep(SimValue::knownF64(static_cast<f64>(v.value().asF32())));
    }
    case op::D2f: {
        Result<SimValue, Bail> v = popChecked(pc);
        if (!v.isOk()) return bailStep(v.error());
        if (!v.value().known) return nextStep(SimValue::unknownOf(ValType::F32));
        return nextStep(SimValue::knownF32(static_cast<float>(v.value().asF64())));
    }
    // 比较指令：结果恒为 I32 的 -1/0/1。NaN 的方向由 l/g 变体决定
    // （lcmp 的 NaN 不可能出现，因为 long 没有 NaN）。
    case op::LCmp: {
        std::vector<SimValue> args;
        for (int i = 0; i < info.popSlots; ++i) {
            Result<SimValue, Bail> popped = popChecked(pc);
            if (!popped.isOk()) return bailStep(popped.error());
            args.push_back(std::move(popped.value()));
        }
        for (const SimValue& a : args) {
            if (!a.known) return nextStep(SimValue::unknownOf(ValType::I32));
        }
        const i64 rhs = static_cast<i64>(args[0].asU64());
        const i64 lhs = static_cast<i64>(args[1].asU64());
        return nextStep(SimValue::knownI32(lhs < rhs ? -1 : (lhs > rhs ? 1 : 0)));
    }
    case op::FCmpL:
    case op::FCmpG: {
        std::vector<SimValue> args;
        for (int i = 0; i < info.popSlots; ++i) {
            Result<SimValue, Bail> popped = popChecked(pc);
            if (!popped.isOk()) return bailStep(popped.error());
            args.push_back(std::move(popped.value()));
        }
        for (const SimValue& a : args) {
            if (!a.known) return nextStep(SimValue::unknownOf(ValType::I32));
        }
        const float rhs = args[0].asF32();
        const float lhs = args[1].asF32();
        if (std::isnan(lhs) || std::isnan(rhs)) {
            return nextStep(SimValue::knownI32(insn.opcode == op::FCmpL ? -1 : 1));
        }
        return nextStep(SimValue::knownI32(lhs < rhs ? -1 : (lhs > rhs ? 1 : 0)));
    }
    case op::DCmpL:
    case op::DCmpG: {
        std::vector<SimValue> args;
        for (int i = 0; i < info.popSlots; ++i) {
            Result<SimValue, Bail> popped = popChecked(pc);
            if (!popped.isOk()) return bailStep(popped.error());
            args.push_back(std::move(popped.value()));
        }
        for (const SimValue& a : args) {
            if (!a.known) return nextStep(SimValue::unknownOf(ValType::I32));
        }
        const f64 rhs = args[0].asF64();
        const f64 lhs = args[1].asF64();
        if (std::isnan(lhs) || std::isnan(rhs)) {
            return nextStep(SimValue::knownI32(insn.opcode == op::DCmpL ? -1 : 1));
        }
        return nextStep(SimValue::knownI32(lhs < rhs ? -1 : (lhs > rhs ? 1 : 0)));
    }
    default:
        // 设计 D8 的默认语义：不认识的 opcode **不 Bail**，按效应表弹掉该弹的
        // 槽、压出正确类型的 Unknown。核心只保证类型——值算不出来是常态，
        // 不是失败；真正需要具体值的地方（分支、switch 键、数组下标）各自有
        // 具体性检查兜底。这样「用户没补环境」表现为一路 Unknown 走到分支处，
        // 而不是第一条没见过的指令就断链。
        return popAndPropagate(pc, info);
    }
}

/// 槽位重排。本模型一个值一个条目，cat2 只体现在「占两个槽」的几个指令
/// （pop2 / dup2 / dup_x2 / dup2_x1 / dup2_x2）的分支形态上——JVMS §6.5 给
/// 每条指令列了 2~4 种形，按形搬条目即可，不需要真的拆成两槽。
Sim::StepResult Sim::executeStackOp(u32 pc, const Insn& insn) {
    switch (insn.opcode) {
    case op::Pop: {
        Result<SimValue, Bail> popped = popChecked(pc);
        if (!popped.isOk()) return bailStep(popped.error());
        return StepResult{};
    }
    case op::Pop2: {
        // cat2 一个值就够；两个 cat1 才要弹两下。
        const usize count = (!stack_.empty() && stack_.back().isCat2()) ? 1 : 2;
        if (stack_.size() < count) {
            return bailStep({Bail::Reason::StackBroken, pc, "pop2 槽位不足"});
        }
        stack_.resize(stack_.size() - count);
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
        stack_.push_back(top); // 值拷贝；Ref 句柄别名语义由堆脚手架承载
        return StepResult{};
    }
    case op::DupX2: {
        // 形 1（三 cat1）：..., v3, v2, v1 → ..., v1, v3, v2, v1
        // 形 2/3（v1 或 v2 为 cat2）：..., v2, v1 → ..., v1, v2, v1
        // 两种形在本模型里都是「条目序列」的差别，按栈顶宽度分流即可。
        const bool topWide = !stack_.empty() && stack_.back().isCat2();
        const bool nextWide = stack_.size() >= 2 && stack_[stack_.size() - 2].isCat2();
        if (topWide || nextWide) {
            if (stack_.size() < 2) {
                return bailStep({Bail::Reason::StackBroken, pc, "dup_x2 槽位不足"});
            }
            SimValue top = stack_[stack_.size() - 1];
            SimValue under = stack_[stack_.size() - 2];
            stack_.resize(stack_.size() - 2);
            stack_.push_back(top);
            stack_.push_back(under);
            stack_.push_back(top);
            return StepResult{};
        }
        if (stack_.size() < 3) {
            return bailStep({Bail::Reason::StackBroken, pc, "dup_x2 需要三个槽位"});
        }
        SimValue top = stack_[stack_.size() - 1];    // value1
        SimValue mid = stack_[stack_.size() - 2];    // value2
        SimValue bottom = stack_[stack_.size() - 3]; // value3
        stack_.resize(stack_.size() - 3);
        stack_.push_back(top);
        stack_.push_back(bottom);
        stack_.push_back(mid);
        stack_.push_back(top);
        return StepResult{};
    }
    case op::Dup2: {
        // cat2 顶：复制一个值；两 cat1：复制一对。
        if (!stack_.empty() && stack_.back().isCat2()) {
            stack_.push_back(stack_.back());
            return StepResult{};
        }
        if (stack_.size() < 2) {
            return bailStep({Bail::Reason::StackBroken, pc, "dup2 需要两个槽位"});
        }
        SimValue under = stack_[stack_.size() - 2];
        SimValue top = stack_[stack_.size() - 1];
        stack_.push_back(under);
        stack_.push_back(top);
        return StepResult{};
    }
    case op::Dup2X1: {
        // 形 1（三 cat1）：..., v3, v2, v1 → ..., v2, v1, v3, v2, v1
        // 形 2（v1 cat2、v2 cat1）：..., v2, v1 → ..., v1, v2, v1
        if (!stack_.empty() && stack_.back().isCat2()) {
            if (stack_.size() < 2) {
                return bailStep({Bail::Reason::StackBroken, pc, "dup2_x1 槽位不足"});
            }
            SimValue v1 = stack_[stack_.size() - 1];
            SimValue v2 = stack_[stack_.size() - 2];
            stack_.resize(stack_.size() - 2);
            stack_.push_back(v1);
            stack_.push_back(v2);
            stack_.push_back(v1);
            return StepResult{};
        }
        if (stack_.size() < 3) {
            return bailStep({Bail::Reason::StackBroken, pc, "dup2_x1 需要三个槽位"});
        }
        SimValue v1 = stack_[stack_.size() - 1];
        SimValue v2 = stack_[stack_.size() - 2];
        SimValue v3 = stack_[stack_.size() - 3];
        stack_.resize(stack_.size() - 3);
        stack_.push_back(v2);
        stack_.push_back(v1);
        stack_.push_back(v3);
        stack_.push_back(v2);
        stack_.push_back(v1);
        return StepResult{};
    }
    case op::Dup2X2: {
        // 形 1（四 cat1）：..., v4, v3, v2, v1 → ..., v2, v1, v4, v3, v2, v1
        // 形 2（v1 cat2）：..., v3, v2, v1 → ..., v1, v3, v2, v1
        // 形 3（v1,v2 cat1、v3 cat2）：..., v3, v2, v1 → ..., v2, v1, v3, v2, v1
        // 形 4（v1,v2 皆 cat2）：..., v2, v1 → ..., v1, v2, v1
        const usize n = stack_.size();
        const bool topWide = n >= 1 && stack_[n - 1].isCat2();
        const bool secondWide = n >= 2 && stack_[n - 2].isCat2();
        if (topWide && secondWide) {
            SimValue v1 = stack_[n - 1];
            SimValue v2 = stack_[n - 2];
            stack_.resize(n - 2);
            stack_.push_back(v1);
            stack_.push_back(v2);
            stack_.push_back(v1);
            return StepResult{};
        }
        if (topWide) {
            if (n < 3) return bailStep({Bail::Reason::StackBroken, pc, "dup2_x2 槽位不足"});
            SimValue v1 = stack_[n - 1];
            SimValue v2 = stack_[n - 2];
            SimValue v3 = stack_[n - 3];
            stack_.resize(n - 3);
            stack_.push_back(v1);
            stack_.push_back(v3);
            stack_.push_back(v2);
            stack_.push_back(v1);
            return StepResult{};
        }
        if (n < 3) return bailStep({Bail::Reason::StackBroken, pc, "dup2_x2 槽位不足"});
        if (secondWide) {
            SimValue v1 = stack_[n - 1];
            SimValue v2 = stack_[n - 2];
            SimValue v3 = stack_[n - 3];
            stack_.resize(n - 3);
            stack_.push_back(v2);
            stack_.push_back(v1);
            stack_.push_back(v3);
            stack_.push_back(v2);
            stack_.push_back(v1);
            return StepResult{};
        }
        if (n < 4) return bailStep({Bail::Reason::StackBroken, pc, "dup2_x2 需要四个槽位"});
        SimValue v1 = stack_[n - 1];
        SimValue v2 = stack_[n - 2];
        SimValue v3 = stack_[n - 3];
        SimValue v4 = stack_[n - 4];
        stack_.resize(n - 4);
        stack_.push_back(v2);
        stack_.push_back(v1);
        stack_.push_back(v4);
        stack_.push_back(v3);
        stack_.push_back(v2);
        stack_.push_back(v1);
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

/// 分支纪律（doc:design-typeprop/D7）：全 Known 且身份可判才单边裁决；否则不 fork，
/// 交给分支接管三态，未注册接管即 Bail{NotConcrete}。
///
/// doc:design-takeover 红线：引擎不预弹栈——先窥视操作数判定可裁决性，接管收完整进入帧
/// （操作数仍在栈上），裁决后由引擎统一弹出。
Sim::StepResult Sim::executeBranch(u32 pc, const Insn& insn, const OpInfo& info) {
    std::vector<SimValue> args;
    for (int i = 0; i < info.popSlots; ++i) {
        Result<SimValue, Bail> peeked = peekChecked(pc, static_cast<usize>(i));
        if (!peeked.isOk()) return bailStep(peeked.error());
        args.push_back(std::move(peeked.value()));
    }

    // args 按窥视序（顶在前）：args[0] = value2（后压），args.back() = value1（先压）。
    // JVM if_icmp<cond> 语义是 value1 <cond> value2。单操作数 if<cond> 只有 args[0]。
    bool evaluable = true;
    for (const SimValue& a : args) evaluable = evaluable && a.known;
    if (evaluable && (insn.opcode == op::IfAcmpeq || insn.opcode == op::IfAcmpne)) {
        // 引用相等：非 interned 的 Str 身份不可判（真 JVM 里是两个对象），
        // 按条件不具体处理，绝不能按内容判同。
        if (compareRef(args[1], args[0]) == RefCmp::Unknown) evaluable = false;
    }

    if (!evaluable) {
        auto hook = session_->branchHooks.find(pc);
        if (hook == session_->branchHooks.end()) {
            return bailStep({Bail::Reason::NotConcrete, pc, "分支条件不具体且未注册分支接管"});
        }
        BranchHook verdict = hook->second; // 拷贝：接管内若动态改注册表，迭代器会失效
        Frame frame(stack_, locals_, pc, *method_, *session_);
        Result<bool, Bail> ruled = verdict(frame);
        if (!ruled.isOk()) return bailStep(ruled.error());
        StepResult consumed = dropOperands(pc, info.popSlots);
        if (consumed.kind == StepResult::Kind::BailOut) return consumed;
        return ruled.value() ? jumpStep(static_cast<u32>(insn.a)) : StepResult{};
    }

    auto signedAt = [&args](usize i) -> i32 {
        return static_cast<i32>(args[i].asU32());
    };

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
    // 引用比较：null 只与 null 相等；对象比句柄；interned 字面量按内容比
    // （同一 ldc 字面量在常量池里必然同一对象，这是 intern 的事实，
    // 而不是 String.equals 语义）；非 interned 的 Str 身份不可判，已在
    // evaluable 判定中折回接管面。
    case op::IfNull: taken = args[0].ref.kind == RefPayload::Kind::Null; break;
    case op::IfNonNull: taken = args[0].ref.kind != RefPayload::Kind::Null; break;
    case op::IfAcmpeq: taken = compareRef(args[1], args[0]) == RefCmp::Same; break;
    case op::IfAcmpne: taken = compareRef(args[1], args[0]) != RefCmp::Same; break;
    default:
        return bailStep({Bail::Reason::UnsupportedOpcode, pc, "分支指令缺语义"});
    }
    StepResult consumed = dropOperands(pc, info.popSlots);
    if (consumed.kind == StepResult::Kind::BailOut) return consumed;
    return taken ? jumpStep(static_cast<u32>(insn.a)) : StepResult{};
}

Sim::RefCmp Sim::compareRef(const SimValue& a, const SimValue& b) {
    const bool aNull = a.ref.kind == RefPayload::Kind::Null;
    const bool bNull = b.ref.kind == RefPayload::Kind::Null;
    if (aNull || bNull) return (aNull && bNull) ? RefCmp::Same : RefCmp::Diff;
    if (a.ref.kind == RefPayload::Kind::Obj || b.ref.kind == RefPayload::Kind::Obj) {
        return (a.ref.kind == b.ref.kind && a.ref.objId == b.ref.objId) ? RefCmp::Same
                                                                       : RefCmp::Diff;
    }
    if (a.ref.interned && b.ref.interned) {
        return a.ref.str == b.ref.str ? RefCmp::Same : RefCmp::Diff;
    }
    return RefCmp::Unknown;
}

/// switch 系：键 Known 查表或走 default；Unknown 不 fork，直接 Bail{NotConcrete}
/// （分支接管面向谓词裁决，不面向键值选择）。
Sim::StepResult Sim::executeSwitch(u32 pc, const Insn& insn) {
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
Sim::StepResult Sim::executeAlloc(u32 pc, const Insn& insn) {
    const std::string& cls = method_->allocTypes[static_cast<usize>(insn.a)];
    auto scaffold = session_->allocScaffolds.find(cls);
    if (scaffold == session_->allocScaffolds.end()) {
        return bailStep({Bail::Reason::UnsupportedAlloc, pc, "未注册分配脚手架：" + cls});
    }
    u64 id = session_->nextAllocId++;
    HeapObj obj;
    obj.className = cls;
    obj.data = scaffold->second();
    session_->heap[id] = std::move(obj);
    return nextStep(SimValue::knownObj(id));
}

/// 数组指令（JVM 指令集语义）：原始类型数组按位型存在 HeapObj.elems，引用
/// 数组存在 HeapObj.refs。元素宽度与符号性由数组类名决定——这是指令集事实，
/// 不涉及任何 JDK 类。读取时任一操作数未知即按类型传播压 Unknown：数组下标
/// 未知不是错误，只是「不知道读的是哪一格」。
Sim::StepResult Sim::executeArray(u32 pc, const Insn& insn) {
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
        const char* arrayClass = primitiveArrayClass(insn.b);
        if (arrayClass == nullptr) {
            return bailStep({Bail::Reason::UnsupportedOpcode, pc, "newarray atype 非 primitive"});
        }
        const u64 id = session_->nextAllocId++;
        HeapObj obj;
        obj.className = arrayClass;
        obj.elems.assign(static_cast<usize>(count), 0);
        session_->heap[id] = std::move(obj);
        return nextStep(SimValue::knownObj(id));
    }
    case op::ANewArray: {
        Result<SimValue, Bail> popped = popChecked(pc);
        if (!popped.isOk()) return bailStep(popped.error());
        if (!popped.value().known) {
            return bailStep({Bail::Reason::NotConcrete, pc, "anewarray 长度不具体"});
        }
        const i32 count = static_cast<i32>(popped.value().asU32());
        if (count < 0 || static_cast<u64>(count) > kMaxArrayElems) {
            return bailStep({Bail::Reason::BadOperand, pc, "anewarray 长度越界"});
        }
        const std::string& elemType = method_->allocTypes[static_cast<usize>(insn.a)];
        const u64 id = session_->nextAllocId++;
        HeapObj obj;
        obj.className = "[" + elemType;
        obj.refs.assign(static_cast<usize>(count), SimValue::knownNull());
        session_->heap[id] = std::move(obj);
        return nextStep(SimValue::knownObj(id));
    }
    case op::MultiANewArray: {
        if (insn.b < 1 || insn.b > 255 || static_cast<usize>(insn.a) >= method_->allocTypes.size()) {
            return bailStep({Bail::Reason::BadOperand, pc, "multianewarray 维数或类型索引无效"});
        }
        const usize dims = static_cast<usize>(insn.b);
        const std::string& elemType = method_->allocTypes[static_cast<usize>(insn.a)];
        const usize component = elemType.find_first_not_of('[');
        if (component == std::string::npos || component + dims > 255) {
            return bailStep({Bail::Reason::BadOperand, pc, "multianewarray 分量类型无效"});
        }

        // 栈顶是最内层的长度。按外到内记录长度，分配前检查所有给出的维度。
        std::vector<usize> lengths(dims);
        bool outerKnown = false;
        bool allKnown = true;
        for (usize depth = dims; depth > 0; --depth) {
            Result<SimValue, Bail> popped = popChecked(pc);
            if (!popped.isOk()) return bailStep(popped.error());
            const SimValue& count = popped.value();
            if (count.type != ValType::I32) {
                return bailStep({Bail::Reason::BadOperand, pc, "multianewarray 长度不是 int"});
            }
            if (!count.known) {
                allKnown = false;
                continue;
            }
            const i32 len = static_cast<i32>(count.asU32());
            if (len < 0) {
                // 负长度是确定会发生的 NegativeArraySizeException，同除零纪律。
                return bailStep({Bail::Reason::BadOperand, pc,
                                 "multianewarray 负长度需异常派发（v1 不做）"});
            }
            if (static_cast<u64>(len) > kMaxArrayElems) {
                return bailStep({Bail::Reason::BadOperand, pc, "multianewarray 长度越界"});
            }
            lengths[depth - 1] = static_cast<usize>(len);
            if (depth == 1) outerKnown = true;
        }
        if (!outerKnown) {
            return bailStep({Bail::Reason::NotConcrete, pc, "multianewarray 长度不具体"});
        }

        if (allKnown) {
            // 整条指令按数组对象和元素槽计入预算，检查乘法上界后再分配。
            u64 remaining = kMaxArrayElems + 1;
            u64 arraysAtDepth = 1;
            for (usize length : lengths) {
                const u64 costPerArray = 1 + static_cast<u64>(length);
                if (arraysAtDepth > remaining / costPerArray) {
                    return bailStep({Bail::Reason::BadOperand, pc, "multianewarray 总分配量越界"});
                }
                remaining -= arraysAtDepth * costPerArray;
                arraysAtDepth *= length;
            }

            // JVM 为每个父数组元素单独分配子数组，兄弟子数组不能互相别名。
            const auto allocateArray = [&](const auto& self, usize depth) -> SimValue {
                const usize len = lengths[depth];
                HeapObj obj;
                obj.className = std::string(dims - depth, '[') + elemType;
                if (depth + 1 < dims) {
                    obj.refs.reserve(len);
                    for (usize i = 0; i < len; ++i) {
                        obj.refs.push_back(self(self, depth + 1));
                    }
                } else if (elemIsRefType(elemType)) {
                    obj.refs.assign(len, SimValue::knownNull());
                } else {
                    obj.elems.assign(len, 0);
                }
                const u64 id = session_->nextAllocId++;
                session_->heap[id] = std::move(obj);
                return SimValue::knownObj(id);
            };
            return nextStep(allocateArray(allocateArray, 0));
        }

        // 内层长度未知：最外层照建（长度已知），元素挂 Unknown(Ref)——内层
        // 数组在真 JVM 里确实存在且已分配，只是这一格指向谁不可知；读出来
        // 按类型传播，不猜 null（null 是「已知指向空」的错误答案）。
        const u64 id = session_->nextAllocId++;
        HeapObj obj;
        obj.className = std::string(dims, '[') + elemType;
        obj.refs.assign(lengths.front(), SimValue::unknownOf(ValType::Ref));
        session_->heap[id] = std::move(obj);
        return nextStep(SimValue::knownObj(id));
    }
    case op::IALoad:
    case op::LALoad:
    case op::FALoad:
    case op::DALoad:
    case op::AALoad:
    case op::BALoad:
    case op::CALoad:
    case op::SALoad:
        return arrayLoad(pc, insn);
    case op::IAStore:
    case op::LAStore:
    case op::FAStore:
    case op::DAStore:
    case op::AAStore:
    case op::BAStore:
    case op::CAStore:
    case op::SAStore:
        return arrayStore(pc, insn);
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
        if (it == session_->heap.end()) {
            return bailStep({Bail::Reason::BadOperand, pc, "arraylength 堆对象缺失"});
        }
        return nextStep(SimValue::knownI32(static_cast<i32>(arrayLength(it->second))));
    }
    default:
        return bailStep({Bail::Reason::UnsupportedOpcode, pc, "数组指令缺语义"});
    }
}

/// primitive atype（JVMS §6.5 newarray）→ 数组类名。
const char* Sim::primitiveArrayClass(i32 atype) {
    switch (atype) {
    case 4: return "[Z";
    case 5: return "[C";
    case 6: return "[F";
    case 7: return "[D";
    case 8: return "[B";
    case 9: return "[S";
    case 10: return "[I";
    case 11: return "[J";
    default: return nullptr;
    }
}

/// 引用数组判定：类名第二字符是 L（对象数组）或 [（数组的数组）。
bool Sim::isRefArray(const std::string& arrayClass) {
    return arrayClass.size() >= 2 && (arrayClass[1] == 'L' || arrayClass[1] == '[');
}

/// 元素类型是否引用形：L...; 对象或 [ 嵌套数组；单字符 primitive 除外。
bool Sim::elemIsRefType(const std::string& elemType) {
    return !elemType.empty() && (elemType[0] == 'L' || elemType[0] == '[');
}

usize Sim::arrayLength(const HeapObj& obj) {
    return isRefArray(obj.className) ? obj.refs.size() : obj.elems.size();
}

/// 加载指令的结果类型（元素类型即指令类型，与数组类名无关）。
ValType Sim::arrayElemValType(u8 opcode) {
    switch (opcode) {
    case op::LALoad: return ValType::I64;
    case op::FALoad: return ValType::F32;
    case op::DALoad: return ValType::F64;
    case op::AALoad: return ValType::Ref;
    default: return ValType::I32;
    }
}

/// 元素位型 → SimValue。窄类型按数组类名决定符号性：[B 有符号、[Z/[C 无符号。
SimValue Sim::decodeElem(u8 opcode, const std::string& arrayClass, i64 raw) {
    switch (opcode) {
    case op::LALoad:
        return SimValue::knownI64(raw);
    case op::FALoad:
        return SimValue::knownF32(bitsToFloat(static_cast<u32>(raw)));
    case op::DALoad: {
        const u64 bits = static_cast<u64>(raw);
        f64 v = 0.0;
        std::memcpy(&v, &bits, sizeof(v));
        return SimValue::knownF64(v);
    }
    case op::BALoad:
        if (arrayClass == "[Z") return SimValue::knownI32(static_cast<i32>(raw & 0x1));
        return SimValue::knownI32(static_cast<i32>(static_cast<signed char>(raw & 0xFF)));
    case op::CALoad:
        return SimValue::knownI32(static_cast<i32>(raw & 0xFFFF));
    case op::SALoad:
        return SimValue::knownI32(static_cast<i32>(static_cast<short>(raw & 0xFFFF)));
    default:
        return SimValue::knownI32(static_cast<i32>(raw));
    }
}

/// SimValue → 元素位型。写入前按元素宽度截断，保证堆里的位型始终合法。
i64 Sim::encodeElem(u8 opcode, const SimValue& v) {
    switch (opcode) {
    case op::LAStore:
        return static_cast<i64>(v.asU64());
    case op::FAStore:
        return static_cast<i64>(static_cast<u64>(floatToBits(v.asF32())));
    case op::DAStore: {
        const f64 raw = v.asF64();
        u64 bits = 0;
        std::memcpy(&bits, &raw, sizeof(bits));
        return static_cast<i64>(bits);
    }
    case op::BAStore:
        return static_cast<i64>(static_cast<signed char>(v.asU32() & 0xFF));
    case op::CAStore:
        return static_cast<i64>(v.asU32() & 0xFFFF);
    case op::SAStore:
        return static_cast<i64>(static_cast<short>(v.asU32() & 0xFFFF));
    default:
        return static_cast<i64>(static_cast<i32>(v.asU32()));
    }
}

Sim::StepResult Sim::arrayLoad(u32 pc, const Insn& insn) {
    Result<SimValue, Bail> idx = popChecked(pc);
    if (!idx.isOk()) return bailStep(idx.error());
    Result<SimValue, Bail> ref = popChecked(pc);
    if (!ref.isOk()) return bailStep(ref.error());

    const ValType resultType = arrayElemValType(insn.opcode);
    if (!ref.value().known || !idx.value().known) {
        return nextStep(SimValue::unknownOf(resultType));
    }
    if (ref.value().ref.kind != RefPayload::Kind::Obj) {
        return bailStep({Bail::Reason::BadOperand, pc, "数组读取接收者非数组引用"});
    }
    auto it = session_->heap.find(ref.value().ref.objId);
    if (it == session_->heap.end()) {
        return bailStep({Bail::Reason::BadOperand, pc, "数组读取：堆对象缺失"});
    }
    const i64 index = static_cast<i64>(idx.value().asU32());
    if (insn.opcode == op::AALoad) {
        if (index < 0 || index >= static_cast<i64>(it->second.refs.size())) {
            return bailStep({Bail::Reason::BadOperand, pc, "aaload 下标越界"});
        }
        SimValue v = it->second.refs[static_cast<usize>(index)];
        v.type = ValType::Ref;
        return nextStep(std::move(v));
    }
    if (index < 0 || index >= static_cast<i64>(it->second.elems.size())) {
        return bailStep({Bail::Reason::BadOperand, pc, "数组下标越界"});
    }
    return nextStep(
        decodeElem(insn.opcode, it->second.className, it->second.elems[static_cast<usize>(index)]));
}

Sim::StepResult Sim::arrayStore(u32 pc, const Insn& insn) {
    Result<SimValue, Bail> value = popChecked(pc);
    if (!value.isOk()) return bailStep(value.error());
    Result<SimValue, Bail> idx = popChecked(pc);
    if (!idx.isOk()) return bailStep(idx.error());
    Result<SimValue, Bail> ref = popChecked(pc);
    if (!ref.isOk()) return bailStep(ref.error());

    if (!ref.value().known || !idx.value().known) {
        return bailStep({Bail::Reason::NotConcrete, pc, "数组写入目标不具体"});
    }
    if (ref.value().ref.kind != RefPayload::Kind::Obj) {
        return bailStep({Bail::Reason::BadOperand, pc, "数组写入接收者非数组引用"});
    }
    auto it = session_->heap.find(ref.value().ref.objId);
    if (it == session_->heap.end()) {
        return bailStep({Bail::Reason::BadOperand, pc, "数组写入：堆对象缺失"});
    }
    const i64 index = static_cast<i64>(idx.value().asU32());
    if (insn.opcode == op::AAStore) {
        if (index < 0 || index >= static_cast<i64>(it->second.refs.size())) {
            return bailStep({Bail::Reason::BadOperand, pc, "aastore 下标越界"});
        }
        SimValue v = value.value();
        v.type = ValType::Ref;
        it->second.refs[static_cast<usize>(index)] = std::move(v);
        return StepResult{};
    }
    if (index < 0 || index >= static_cast<i64>(it->second.elems.size())) {
        return bailStep({Bail::Reason::BadOperand, pc, "数组下标越界"});
    }
    if (!value.value().known) {
        return bailStep({Bail::Reason::NotConcrete, pc, "基本类型数组无法保存 Unknown 元素"});
    }
    it->second.elems[static_cast<usize>(index)] = encodeElem(insn.opcode, value.value());
    return StepResult{};
}

/// 实例字段键：同名同描述符即同一槽位（类名不参与，MVP 不做继承区分）。
std::string Sim::instanceKey(const FieldRef& field) {
    return field.name + "::" + field.desc;
}

/// 字段接管键：与调用接管同款 owner.name+desc。
std::string Sim::fieldKey(const FieldRef& field) {
    return field.owner + "." + field.name + field.desc;
}

/// 字段面（doc:design-takeover 字段接管 + 会话存贮底座）：接管优先，命中即由接管方经
/// Frame 自己弹压（get 系返回值由引擎按描述符定型压栈，put 系忽略返回值）；
/// 未命中走底座——静态字段 Session.statics（跨方法接力棒：clinit 先跑、后续
/// 方法读同一 Session），实例字段挂 HeapObj.fields。缺项一律按字段类型传播
/// 压 Unknown，不 Bail：字段没被赋值过是常态，不是错误。
/// Known null 接收者的 getfield/putfield 按 NPE 口径 Bail——那是一个确定会
/// 发生、而异常派发 v1 不做的事件，与整数除零同一纪律。
Sim::StepResult Sim::executeField(u32 pc, const Insn& insn) {
    const FieldRef& field = method_->fields[static_cast<usize>(insn.a)];
    const std::string staticKey = field.owner + "::" + field.name + "::" + field.desc;

    auto mockIt = session_->fieldMocks.find(fieldKey(field));
    if (mockIt != session_->fieldMocks.end()) {
        FieldMock mock = mockIt->second; // 拷贝：接管内改注册表会失效迭代器
        Frame frame(stack_, locals_, pc, *method_, *session_);
        Result<SimValue, Bail> taken = mock(frame);
        if (!taken.isOk()) return bailStep(taken.error());
        if (insn.opcode == op::PutStatic || insn.opcode == op::PutField) {
            return StepResult{}; // put 系：接管方已消费操作数，返回值忽略
        }
        SimValue v = std::move(taken.value());
        v.type = field.type; // 类型是静态事实，来自描述符而非接管方
        return nextStep(std::move(v));
    }

    switch (insn.opcode) {
    case op::GetStatic: {
        auto it = session_->statics.find(staticKey);
        if (it != session_->statics.end()) {
            SimValue v = it->second;
            v.type = field.type; // 类型是槽位静态事实
            return nextStep(std::move(v));
        }
        return nextStep(SimValue::unknownOf(field.type));
    }
    case op::PutStatic: {
        Result<SimValue, Bail> popped = popChecked(pc);
        if (!popped.isOk()) return bailStep(popped.error());
        session_->statics[staticKey] = std::move(popped.value());
        return StepResult{};
    }
    case op::GetField: {
        Result<SimValue, Bail> recv = popChecked(pc);
        if (!recv.isOk()) return bailStep(recv.error());
        if (!recv.value().known) {
            return nextStep(SimValue::unknownOf(field.type));
        }
        if (recv.value().ref.kind == RefPayload::Kind::Null) {
            return bailStep({Bail::Reason::BadOperand, pc,
                             "getfield null 接收者需 NPE 派发（v1 不做）"});
        }
        if (recv.value().ref.kind != RefPayload::Kind::Obj) {
            return nextStep(SimValue::unknownOf(field.type)); // Str 载荷无实例字段面
        }
        auto it = session_->heap.find(recv.value().ref.objId);
        if (it == session_->heap.end()) {
            return nextStep(SimValue::unknownOf(field.type));
        }
        auto slot = it->second.fields.find(instanceKey(field));
        if (slot == it->second.fields.end()) {
            return nextStep(SimValue::unknownOf(field.type));
        }
        SimValue v = slot->second;
        v.type = field.type;
        return nextStep(std::move(v));
    }
    case op::PutField: {
        Result<SimValue, Bail> value = popChecked(pc);
        if (!value.isOk()) return bailStep(value.error());
        Result<SimValue, Bail> recv = popChecked(pc);
        if (!recv.isOk()) return bailStep(recv.error());
        if (!recv.value().known) {
            return bailStep({Bail::Reason::NotConcrete, pc, "putfield 写入目标不具体"});
        }
        if (recv.value().ref.kind == RefPayload::Kind::Null) {
            return bailStep({Bail::Reason::BadOperand, pc,
                             "putfield null 接收者需 NPE 派发（v1 不做）"});
        }
        if (recv.value().ref.kind != RefPayload::Kind::Obj) {
            return StepResult{};
        }
        auto it = session_->heap.find(recv.value().ref.objId);
        if (it == session_->heap.end()) return StepResult{};
        it->second.fields[instanceKey(field)] = std::move(value.value());
        return StepResult{};
    }
    default:
        return bailStep({Bail::Reason::UnsupportedOpcode, pc, "字段指令缺语义"});
    }
}

/// invoke 系：查接管注册（调用点优先于签名），未注册一律 Bail{UnsupportedCall}。
/// 接收者与实参由接管方自己弹（帧完整可见）；hasReturn=false 不压返回值。
Sim::StepResult Sim::executeCall(u32 pc, const Insn& insn) {
    const CallRef& callee = method_->calls[static_cast<usize>(insn.a)];
    std::string key = callee.owner + "." + callee.name + callee.desc;

    CallMock* mock = nullptr;
    auto atPc = session_->callMocksAtPc.find(pc);
    if (atPc != session_->callMocksAtPc.end()) mock = &atPc->second;
    if (mock == nullptr) {
        auto bySig = session_->callMocks.find(key);
        if (bySig != session_->callMocks.end()) mock = &bySig->second;
    }

    Frame frame(stack_, locals_, pc, *method_, *session_);
    if (mock == nullptr && session_->callMockResolver) {
        mock = session_->callMockResolver(callee, frame);
    }
    for (usize i = 0; mock == nullptr && i < session_->callMockResolvers.size(); ++i) {
        if (!session_->callMockResolvers[i]) continue;
        mock = session_->callMockResolvers[i](callee, frame);
    }
    if (mock == nullptr) {
        return bailStep({Bail::Reason::UnsupportedCall, pc, "调用未注册接管：" + key});
    }

    CallMock takeover = *mock; // 拷贝：接管内若动态改注册表，指针会悬空
    Result<SimValue, Bail> mocked = takeover(frame);
    if (!mocked.isOk()) return bailStep(mocked.error());

    if (!callee.hasReturn) return StepResult{};
    SimValue result = std::move(mocked.value());
    result.type = callee.retType; // 类型是静态事实，来自描述符而非 mock
    return nextStep(std::move(result));
}

SimValue Sim::localOrUnknown(i32 slot, ValType type) {
    usize index = static_cast<usize>(slot);
    if (index >= locals_.size() || !locals_[index].known) {
        return SimValue::unknownOf(type);
    }
    SimValue v = locals_[index];
    v.type = type;
    return v;
}

void Sim::storeLocal(i32 slot, SimValue v) {
    usize index = static_cast<usize>(slot);
    if (index >= locals_.size()) {
        locals_.resize(index + 1, SimValue::unknownOf(ValType::I32));
    }
    locals_[index] = std::move(v);
}

} // namespace jsim
