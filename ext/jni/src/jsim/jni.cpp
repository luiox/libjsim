#include "detail/guest.hpp"

#include <cstring>

namespace jsim::jni {

Result<std::vector<ValType>, Bail> argTypesOfDesc(std::string_view desc)
{
    std::vector<ValType> out;
    if (desc.empty() || desc.front() != '(') {
        return Result<std::vector<ValType>, Bail>::err({Bail::Reason::BadOperand, 0, "描述符缺 '('"});
    }
    usize i = 1;
    while (i < desc.size() && desc[i] != ')') {
        const char c = desc[i];
        switch (c) {
        case 'I': case 'Z': case 'B': case 'C': case 'S':
            out.push_back(ValType::I32); ++i; break;
        case 'J': out.push_back(ValType::I64); ++i; break;
        case 'F': out.push_back(ValType::F32); ++i; break;
        case 'D': out.push_back(ValType::F64); ++i; break;
        case 'L': {
            const usize j = desc.find(';', i);
            if (j == std::string_view::npos) {
                return Result<std::vector<ValType>, Bail>::err({Bail::Reason::BadOperand, 0, "描述符缺 ';'"});
            }
            out.push_back(ValType::Ref); i = j + 1; break;
        }
        case '[': {
            usize j = i;
            while (j < desc.size() && desc[j] == '[') ++j;
            if (j < desc.size() && desc[j] == 'L') {
                const usize k = desc.find(';', j);
                if (k == std::string_view::npos) {
                    return Result<std::vector<ValType>, Bail>::err({Bail::Reason::BadOperand, 0, "描述符缺 ';'"});
                }
                i = k + 1;
            } else if (j < desc.size()) {
                i = j + 1;
            } else {
                return Result<std::vector<ValType>, Bail>::err({Bail::Reason::BadOperand, 0, "描述符截断"});
            }
            out.push_back(ValType::Ref); break;
        }
        default:
            return Result<std::vector<ValType>, Bail>::err({Bail::Reason::BadOperand, 0, "描述符非法字符"});
        }
    }
    if (i >= desc.size() || desc[i] != ')') {
        return Result<std::vector<ValType>, Bail>::err({Bail::Reason::BadOperand, 0, "描述符缺 ')'"});
    }
    return Result<std::vector<ValType>, Bail>::ok(std::move(out));
}

Result<std::pair<ValType, bool>, Bail> retTypeOfDesc(std::string_view desc)
{
    const usize r = desc.rfind(')');
    if (r == std::string_view::npos || r + 1 >= desc.size()) {
        return Result<std::pair<ValType, bool>, Bail>::err({Bail::Reason::BadOperand, 0, "描述符缺返回类型"});
    }
    switch (desc[r + 1]) {
    case 'V': return Result<std::pair<ValType, bool>, Bail>::ok({ValType::I32, false});
    case 'J': return Result<std::pair<ValType, bool>, Bail>::ok({ValType::I64, true});
    case 'F': return Result<std::pair<ValType, bool>, Bail>::ok({ValType::F32, true});
    case 'D': return Result<std::pair<ValType, bool>, Bail>::ok({ValType::F64, true});
    case 'L': case '[': return Result<std::pair<ValType, bool>, Bail>::ok({ValType::Ref, true});
    default: return Result<std::pair<ValType, bool>, Bail>::ok({ValType::I32, true});
    }
}

bool throwableSafe(const char* name)
{
    return name != nullptr
        && (std::strcmp(name, "ExceptionCheck") == 0 || std::strcmp(name, "ExceptionOccurred") == 0
            || std::strcmp(name, "ExceptionClear") == 0 || std::strcmp(name, "ExceptionDescribe") == 0);
}

std::string hexOf(u64 v)
{
    static const char* digits = "0123456789abcdef";
    std::string s = "0x";
    if (v == 0) return "0x0";
    char buf[16];
    usize n = 0;
    while (v != 0) {
        buf[n++] = digits[v & 0xF];
        v >>= 4;
    }
    while (n != 0) s += buf[--n];
    return s;
}

Result<std::vector<char>, Bail> argClassesOf(JniEnv& env, u32 id, u64 midHandle)
{
    const FuncMeta* meta = metaOf(id);
    if (meta == nullptr) {
        return Result<std::vector<char>, Bail>::err({Bail::Reason::UnsupportedCall, 0, "JNI 函数号无效"});
    }
    std::vector<char> flags(meta->argc + 1, 0); ///< 位置 0 = env,恒整
    // 变长形态只出现在 bare Call*Method / NewObject:实参在寄存器/栈上,类由
    // jmethodID 的 desc 决定。A/V 变体的实参在内存(jvalue*/va_list),不经寄存器类。
    const std::string name = meta->name;
    const bool dynamic = name.rfind("Call", 0) == 0 || name.rfind("NewObject", 0) == 0;
    if (!dynamic) return Result<std::vector<char>, Bail>::ok(std::move(flags));
    const char last = name.back();
    if (last == 'A' || last == 'V') return Result<std::vector<char>, Bail>::ok(std::move(flags));
    Result<const HandlePool::Member*, Bail> mid = env.pool.memberOf(midHandle);
    if (!mid.isOk()) return Result<std::vector<char>, Bail>::err(mid.error());
    Result<std::vector<ValType>, Bail> types = argTypesOfDesc(mid.value()->desc);
    if (!types.isOk()) return Result<std::vector<char>, Bail>::err(types.error());
    flags.clear();
    flags.push_back(0); // env
    flags.push_back(0); // 接收者 / clazz
    flags.push_back(0); // mid
    for (ValType t : types.value()) flags.push_back(t == ValType::F32 || t == ValType::F64 ? 1 : 0);
    return Result<std::vector<char>, Bail>::ok(std::move(flags));
}

Result<u64, Bail> dispatch(JniEnv& env, u32 id,
                                  const std::function<Result<u64, Bail>(usize)>& argI,
                                  const std::function<Result<u64, Bail>(usize)>& argF)
{
    const FuncMeta* meta = metaOf(id);
    if (meta == nullptr) {
        return Result<u64, Bail>::err({Bail::Reason::UnsupportedCall, 0,
                                       "JNI 函数号越界或保留:" + std::to_string(id)});
    }
    if (env.pendingThrow && !throwableSafe(meta->name)) {
        return Result<u64, Bail>::err({Bail::Reason::UnsupportedCall, 0,
                                       "异常已挂起,JNI 契约要求立即返回(调用的是 " + std::string(meta->name) + ")"});
    }
    std::vector<u64> fixed;
    fixed.reserve(meta->argc);
    for (usize p = 1; p <= meta->argc; ++p) {
        Result<u64, Bail> v = argI(p); // 定长函数全整类(浮点只在变长尾)
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        fixed.push_back(v.value());
    }
    JniCall call(id, meta->name, env, fixed, argI, argF);

    auto hit = env.hooks.find(meta->name);
    Result<u64, Bail> out = hit != env.hooks.end()
        ? hit->second(call)
        : Result<u64, Bail>::err({Bail::Reason::UnsupportedCall, 0,
                                  std::string("JNI 钩子未实现:") + meta->name});
    if (env.trace) {
        std::string line = std::string("[JNI] ") + meta->name + "(";
        for (usize i = 0; i < fixed.size(); ++i) {
            if (i != 0) line += ", ";
            line += hexOf(fixed[i]);
        }
        line += ")";
        line += out.isOk() ? " -> " + hexOf(out.value()) : " -> Bail{" + out.error().detail + "}";
        env.trace(line);
    }
    return out;
}

Result<NativeRun, Bail> buildNativeRun(JniEnv& env, const NativeSpec& spec, const CallRef& callee,
                                              Frame& frame)
{
    NativeRun run;
    run.spec = &spec;
    run.hasReturn = callee.hasReturn;
    run.retType = callee.retType;

    if (!callee.isStatic) {
        Result<SimValue, Bail> recv = frame.popChecked();
        if (!recv.isOk()) return Result<NativeRun, Bail>::err(recv.error());
        if (!recv.value().known || recv.value().type != ValType::Ref) {
            return Result<NativeRun, Bail>::err({Bail::Reason::NotConcrete, frame.pc(),
                                                 "native 接收者不具体"});
        }
        Result<u64, Bail> h = env.pool.makeRef(recv.value(), false);
        if (!h.isOk()) return Result<NativeRun, Bail>::err(h.error());
        run.args.push_back(h.value());
    } else {
        // static native 的 jclass:与 ldc 类常量同一个对象(身份一致)。
        const u64 cid = env.session->classObjectOf(callee.owner);
        Result<u64, Bail> h = env.pool.makeRef(SimValue::knownObj(cid), false);
        if (!h.isOk()) return Result<NativeRun, Bail>::err(h.error());
        run.args.push_back(h.value());
    }

    Result<std::vector<ValType>, Bail> types = argTypesOfDesc(callee.desc);
    if (!types.isOk()) return Result<NativeRun, Bail>::err(types.error());
    std::vector<u64> raw(types.value().size(), 0);
    for (usize i = types.value().size(); i-- > 0;) {
        Result<SimValue, Bail> popped = frame.popChecked();
        if (!popped.isOk()) return Result<NativeRun, Bail>::err(popped.error());
        Result<u64, Bail> bits = detail::encodeByValType(popped.value(), env);
        if (!bits.isOk()) return Result<NativeRun, Bail>::err(bits.error());
        raw[i] = bits.value();
    }
    run.args.insert(run.args.end(), raw.begin(), raw.end());
    for (ValType t : types.value()) {
        run.argIsFloat.push_back(t == ValType::F32 || t == ValType::F64 ? 1 : 0);
    }
    return Result<NativeRun, Bail>::ok(std::move(run));
}

CallMock makeNativeMock(JniEnv& env, const NativeSpec& spec, const CallRef& callee)
{
    return [&env, spec, callee](Frame& frame) -> Result<SimValue, Bail> {
        if (env.executor.run == nullptr) {
            return Result<SimValue, Bail>::err({Bail::Reason::UnsupportedCall, frame.pc(),
                                                "native 执行器未接(env.executor.run)"});
        }
        Result<NativeRun, Bail> run = buildNativeRun(env, spec, callee, frame);
        if (!run.isOk()) return Result<SimValue, Bail>::err(run.error());

        env.currentAbi = spec.abi;
        Result<u64, Bail> ret = env.executor.run(env, run.value());

        if (env.pendingThrow) {
            env.pool.popLocals();
            const std::string cls = env.throwClass;
            env.pendingThrow = false;
            env.throwClass.clear();
            return Result<SimValue, Bail>::err({Bail::Reason::UnsupportedCall, frame.pc(),
                                                "native 抛出异常:" + cls});
        }
        if (!ret.isOk()) {
            env.pool.popLocals();
            return Result<SimValue, Bail>::err(ret.error());
        }

        SimValue out = SimValue::unknownOf(ValType::I32);
        if (callee.hasReturn) {
            Result<SimValue, Bail> v = detail::decodeByValType(callee.retType, ret.value(), env);
            if (!v.isOk()) {
                env.pool.popLocals();
                return Result<SimValue, Bail>::err(v.error());
            }
            out = std::move(v.value());
        }
        env.pool.popLocals();
        return Result<SimValue, Bail>::ok(std::move(out));
    };
}

void install(Session& session, JniEnv& env)
{
    installSkeletons(env);
    if (env.session == nullptr) env.session = &session;
    session.callMockResolvers.push_back([&session, &env](const CallRef& callee, Frame&) -> CallMock* {
        const std::string key = callee.owner + "." + callee.name + callee.desc;
        auto specIt = env.methods.find(key);
        if (specIt == env.methods.end()) return nullptr; // 放行:核心照常 Bail{UnsupportedCall}
        auto& slot = env.nativeMocks[key];
        if (!slot) slot = makeNativeMock(env, specIt->second, callee);
        return &slot;
    });
}

} // namespace jsim::jni
