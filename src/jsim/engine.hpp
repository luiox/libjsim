/// @file engine.hpp
/// @defgroup engine 单方法引擎
/// @brief 单方法引擎（doc:design-single）：入口、预填、类型传播主循环、接管面、Bail。

#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
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

/// void 特化:无值成功面(写内存、释放这类只有成败的操作)。MVP Result 原版
/// 无法承载 Result<void, E>——值成员与引用成员对 void 全部非法。
template <class E>
class Result<void, E> {
public:
    static Result ok() { return Result{}; }

    static Result err(E e) {
        Result r;
        r.hasValue_ = false;
        r.error_ = std::move(e);
        return r;
    }

    bool isOk() const { return hasValue_; }
    E& error() { return error_; }
    const E& error() const { return error_; }

private:
    bool hasValue_ = true;
    E error_{};
};

/// 结构化失败（doc:design-positioning 失败面）。Bail 是一等公民：带原因码、pc 与现场说明，
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
///
/// 数组面按 JVM **指令集**语义建模（不是 JDK 语义）：原始类型数组一律存位型
/// （elems），元素宽度与符号性由数组类名首字符决定；引用数组存 SimValue，
/// 元素可以是 Unknown——「没读到具体值」不是错误。实例字段走 fields 键值面，
/// 键 = name::desc，缺项即 Unknown（设计 D8）。
struct HeapObj {
    std::string className;
    std::string data;      // MVP 脚手架私有载荷；正式版按类分型
    std::vector<i64> elems; // 原始类型数组载荷（按 className 解释位型）
    std::vector<SimValue> refs; // 引用数组（"[Ljava/lang/String;" 等）载荷，元素可 Unknown
    std::unordered_map<std::string, SimValue> fields; // 实例字段：键 = name::desc
};

struct Session;

/// 观测面的结构化步进视图：每条指令**执行前**对栈与局部变量的只读快照。
/// 观测属于观测面（D2）：只读、不改帧，观测方要控制流时用断点熔断或接管面。
struct StepView {
    u32 pc = 0;
    u8 opcode = 0;
    const std::vector<SimValue>* stack = nullptr;
    const std::vector<SimValue>* locals = nullptr;
    const Method* method = nullptr;
    const Session* session = nullptr;
};

/// 完整进入帧视图（doc:design-takeover 红线：钩子与接管收完整帧，引擎不预弹栈）。
class Frame {
public:
    Frame(std::vector<SimValue>& stack, std::vector<SimValue>& locals, u32 pc,
          const Method& method, Session& session)
        : stack_(&stack), locals_(&locals), pc_(pc), method_(&method), session_(&session) {}

    u32 pc() const { return pc_; }
    const Method& method() const { return *method_; }
    Session& session() const { return *session_; }

    /// 只读栈视图（观测与接管方判定形态用；写操作走 push/popChecked）。
    const std::vector<SimValue>& stack() const { return *stack_; }
    /// 只读局部变量视图。
    const std::vector<SimValue>& locals() const { return *locals_; }

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
    std::vector<SimValue>* locals_ = nullptr;
    u32 pc_ = 0;
    const Method* method_ = nullptr;
    Session* session_ = nullptr;
};

/// 调用接管：扩展包与用户走同一注册面（doc:design-takeover/design-packages），核心无特权内建。
/// 返回 Known/Unknown 值即接管语义；返回 Bail 即该调用点无法继续。
using CallMock = std::function<Result<SimValue, Bail>(Frame&)>;

/// 字段接管（doc:design-takeover）：get 系返回注入值（引擎按描述符定型压栈），put 系自行
/// 经 Frame 弹栈记录、返回值被忽略。get/put 共用一个键，接管方按
/// frame.method().code[frame.pc()].opcode 区分指令。
using FieldMock = std::function<Result<SimValue, Bail>(Frame&)>;

/// 分支接管（doc:design-typeprop 三态）：返回 true/false 即单边裁决，返回 Bail 即放弃。
/// 收完整进入帧（doc:design-takeover 红线）：条件操作数仍在栈上，接管方经 peekChecked 窥视；
/// 操作数由引擎在裁决后统一弹出，接管方只裁决不消费。
using BranchHook = std::function<Result<bool, Bail>(Frame&)>;

/// 指令接管的产出：只管控制流，栈由接管方经 Frame 自行操作（doc:design-takeover 红线）。
struct OpOutcome {
    u32 nextPc = 0xFFFFFFFFu; ///< 0xFFFFFFFF = 顺序推进
    bool doReturn = false;
    bool hasReturnValue = false;
    SimValue returnValue;
};

/// 指令接管（doc:design-takeover 第一行）：整条 opcode 的语义替换。
///
/// 引擎在查效应表**之前**先看这里——效应表是默认语义的索引，不是权限门。
/// 这是「user 补环境」的最后一层口子：表里没有的 opcode 也能接管，不必等
/// 核心补表；表里有但默认语义不够的，接管优先。
using OpHandler = std::function<Result<OpOutcome, Bail>(Frame&)>;

/// 步进观测：每条指令执行前收到结构化只读视图（doc:design-takeover 观测面）。
/// 注册期过滤由观测方自行包一层完成（按 pc/opcode 忽略即可）。
using StepObserver = std::function<void(const StepView&)>;

/// 会话：接管注册表、分配脚手架、堆、静态字段、观测口。核心不内置任何 JDK
/// 语义（设计 D10）；静态字段值是「多方法编排」的接力棒——clinit 先跑，
/// 后续方法读同一 Session。
struct Session {
    std::unordered_map<std::string, CallMock> callMocks;  // 键 = owner.name+desc
    std::unordered_map<u32, CallMock> callMocksAtPc;      // 按调用点注册，优先于按签名
    std::unordered_map<u8, OpHandler> opcodeHandlers;     // 指令接管，优先于效应表
    std::unordered_map<u32, BranchHook> branchHooks;      // 分支接管，按 pc
    std::unordered_map<std::string, FieldMock> fieldMocks; // 字段接管，键 = owner.name+desc（design-takeover）
    std::unordered_map<std::string, std::function<std::string()>> allocScaffolds; // 类名 → 初始载荷工厂
    std::unordered_map<u64, HeapObj> heap;
    std::unordered_map<std::string, SimValue> statics;    // 键 = owner::name::desc
    std::unordered_map<std::string, u64> classObjects;    // 类名 → Class 对象 id
    std::function<void(u32 pc, const char* opcode, const std::string& stackText)> trace;
    std::vector<StepObserver> stepObservers;              // 结构化步进观测（指令前）
    /// Bail 现场回调：finishBail 时带终止现场（栈与局部变量快照）。
    std::function<void(const Bail&, const StepView&)> bailObserver;
    /// 兜底解析：按签名查不到时问它一次。反射面（注解元素访问器、getter 家族）
    /// 的语义由**接收者对象**决定，静态签名键匹配不到——这类「方法名就是数据」
    /// 的调用只能运行期解析。给的是完整帧，解析方可以窥视接收者再决定认不认领；
    /// 返回 nullptr 表示不认领，引擎照常 Bail。返回的 mock 由注册方持有，
    /// 引擎只读一次即拷贝。
    std::function<CallMock*(const CallRef& callee, Frame& frame)> callMockResolver;
    /// 兜底解析链：单槽不认领时按注册序逐个问。
    ///
    /// 单槽只够一个注册方用。反射面（注解代理）、跨方法编排（容器内方法）、
    /// 方法句柄面都靠「运行期看接收者才决定认不认领」这套语义，谁也不能独占
    /// 这个口子——否则后装的把先装的顶掉，且失效是静默的。
    std::vector<std::function<CallMock*(const CallRef& callee, Frame& frame)>> callMockResolvers;
    u64 nextAllocId = 1;

    void observe(u32 pc, u8 opcode, const std::vector<SimValue>& stack);

    /// Class 对象缓存：`ldc` 类常量与 `Class.forName` 必须给出**同一个**对象，
    /// 否则引用相等（if_acmpeq）与 Class 的 identity 语义会假性失败。
    /// 类常量是 JVM 指令集事实，所以缓存放在核心而不是扩展包。
    u64 classObjectOf(const std::string& internalName);
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
    /// 断点哨兵：不熔断。
    static constexpr u32 kNoStopPc = 0xFFFFFFFFu;

    u64 maxSteps = 4096;
    /// 合并点栈形一致性校验（doc:design-single）：同一 pc 二次到达时深度或类型序列
    /// 不一致即 Bail{StackBroken}。关闭时引擎信任预填与接管方。
    bool checkBalance = false;
    /// 区间模式容忍域外出口：越过方法尾按 Completed(void) 收尾而不是 Bail
    /// （doc:design-single「容忍预填栈与域外出口」）。
    bool tolerateOffEnd = false;
    /// 断点熔断（观测面）：执行前命中即停，按 LimitHit 语义收尾并带帧快照，
    /// 配区间起步（把快照灌回 Prefill）即可续跑。
    u32 stopAtPc = kNoStopPc;
};

/// 运行结果三态（设计 D6）：Completed 带可选返回值 / Bailed 带现场 / LimitHit 帧步数。
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
    /// 终止帧快照（三种终态都带）：断点续跑把它灌回 Prefill，Bail 现场由此
    /// 落地（观测面承诺的「帧快照 / Bail 现场」）。
    std::vector<SimValue> finalStack;
    std::vector<SimValue> finalLocals;
};

/// 单方法模拟器（设计 D9）：一个实例绑定一个方法，跨方法不递归。
class Sim {
public:
    Sim(const Method& method, Session& session) : method_(&method), session_(&session) {}

    /// 方法入口起步（doc:design-single 便利入口一）：pc = 0，栈平衡校验默认开。
    RunResult runEntry(Prefill prefill, u64 maxSteps = 4096);

    /// 区间起步（doc:design-single 便利入口二）：任意 pc，栈平衡默认关、容忍预填栈与
    /// 域外出口。
    RunResult runRegion(u32 entryPc, Prefill prefill, u64 maxSteps = 4096);

    RunResult run(u32 entryPc, Prefill prefill, Limits limits);

private:
    struct StepResult;
    enum class RefCmp : u8;

    RunResult finish(RunResult& out);
    RunResult finishBail(RunResult& out, Bail b);
    void fireStepObservers(u32 pc, u8 opcode);
    bool balancePass(u32 pc, std::vector<char>& seen, std::vector<std::vector<ValType>>& shape);
    static StepResult nextStep(SimValue toPush);
    static StepResult jumpStep(u32 target);
    static StepResult bailStep(Bail b);
    Result<SimValue, Bail> popChecked(u32 pc);
    Result<SimValue, Bail> peekChecked(u32 pc, usize fromTop = 0) const;
    StepResult dropOperands(u32 pc, u8 n);
    StepResult popAndCompute(u32 pc, const OpInfo& info, u32 (*compute)(u32, u32));
    StepResult popAndCompute64(u32 pc, const OpInfo& info, u64 (*compute)(u64, u64));
    StepResult popAndPropagate(u32 pc, const OpInfo& info);
    template <class Fn>
    StepResult foldF32(u32 pc, const OpInfo& info, Fn fn);
    template <class Fn>
    StepResult foldF64(u32 pc, const OpInfo& info, Fn fn);
    static i32 saturateToI32(f64 v);
    static i64 saturateToI64(f64 v);
    StepResult execute(u32 pc, const Insn& insn, const OpInfo& info);
    StepResult executeValue(u32 pc, const Insn& insn, const OpInfo& info);
    StepResult executeStackOp(u32 pc, const Insn& insn);
    StepResult executeBranch(u32 pc, const Insn& insn, const OpInfo& info);
    static RefCmp compareRef(const SimValue& a, const SimValue& b);
    StepResult executeSwitch(u32 pc, const Insn& insn);
    StepResult executeAlloc(u32 pc, const Insn& insn);
    StepResult executeArray(u32 pc, const Insn& insn);
    static const char* primitiveArrayClass(i32 atype);
    static bool isRefArray(const std::string& arrayClass);
    static bool elemIsRefType(const std::string& elemType);
    static usize arrayLength(const HeapObj& obj);
    static ValType arrayElemValType(u8 opcode);
    static SimValue decodeElem(u8 opcode, const std::string& arrayClass, i64 raw);
    static i64 encodeElem(u8 opcode, const SimValue& v);
    StepResult arrayLoad(u32 pc, const Insn& insn);
    StepResult arrayStore(u32 pc, const Insn& insn);
    static std::string instanceKey(const FieldRef& field);
    static std::string fieldKey(const FieldRef& field);
    StepResult executeField(u32 pc, const Insn& insn);
    StepResult executeCall(u32 pc, const Insn& insn);
    SimValue localOrUnknown(i32 slot, ValType type);
    void storeLocal(i32 slot, SimValue v);

    static constexpr u32 kNextPc = 0xFFFFFFFFu; // 顺序推进哨兵

    const Method* method_ = nullptr;
    Session* session_ = nullptr;
    std::vector<SimValue> locals_;
    std::vector<SimValue> stack_;
};

} // namespace jsim
