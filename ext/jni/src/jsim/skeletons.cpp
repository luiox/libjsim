#include "detail/guest.hpp"

#include <cstring>

namespace jsim::jni {

void putHook(JniEnv& env, const char* name, JniHook fn)
{
    if (env.hooks.find(name) == env.hooks.end()) env.hooks[name] = std::move(fn);
}

Result<u64, Bail> callMethodImpl(JniCall& call, bool isStatic, int variant)
{
    JniEnv& env = call.env;
    if (call.args.size() < 2) {
        return Result<u64, Bail>::err({Bail::Reason::StackBroken, 0, "Call* 实参不足"});
    }
    Result<const HandlePool::Member*, Bail> mid = env.pool.memberOf(call.args[1]);
    if (!mid.isOk()) return Result<u64, Bail>::err(mid.error());
    const HandlePool::Member& m = *mid.value();

    std::vector<u64> bits;
    if (variant == 0) {
        Result<std::vector<u64>, Bail> b = detail::collectBareArgs(call, m.desc);
        if (!b.isOk()) return Result<u64, Bail>::err(b.error());
        bits = std::move(b.value());
    } else if (variant == 2) {
        if (call.args.size() < 3) {
            return Result<u64, Bail>::err({Bail::Reason::StackBroken, 0, "Call*A 缺 jvalue*"});
        }
        Result<std::vector<u64>, Bail> b = detail::readJValueArray(env, call.args[2], m.desc);
        if (!b.isOk()) return Result<u64, Bail>::err(b.error());
        bits = std::move(b.value());
    } else { // V
        if (call.args.size() < 3) {
            return Result<u64, Bail>::err({Bail::Reason::StackBroken, 0, "Call*V 缺 va_list"});
        }
        if (env.currentAbi == NativeSpec::Abi::SysV) {
            return Result<u64, Bail>::err({Bail::Reason::UnsupportedCall, 0,
                                           "SysV va_list 结构 v1 不做,用 A 形变体"});
        }
        // Win64 va_list 是 char*,指向实参溢出区,逐 8 字节槽读。
        Result<std::vector<u64>, Bail> b = detail::readJValueArray(env, call.args[2], m.desc);
        if (!b.isOk()) return Result<u64, Bail>::err(b.error());
        bits = std::move(b.value());
    }

    Result<std::vector<SimValue>, Bail> argv = detail::decodeArgs(m.desc, bits, env);
    if (!argv.isOk()) return Result<u64, Bail>::err(argv.error());

    if (env.javaInvoker == nullptr) {
        return Result<u64, Bail>::err({Bail::Reason::UnsupportedCall, 0,
                                       "Call*Method 需要 env.javaInvoker(嵌套 Sim 编排)"});
    }
    Result<std::pair<ValType, bool>, Bail> rt = retTypeOfDesc(m.desc);
    if (!rt.isOk()) return Result<u64, Bail>::err(rt.error());
    CallRef callee;
    callee.owner = m.owner;
    callee.name = m.name;
    callee.desc = m.desc;
    callee.retType = rt.value().first;
    callee.hasReturn = rt.value().second;
    callee.isStatic = isStatic;
    Result<SimValue, Bail> out = env.javaInvoker(callee, std::move(argv.value()));
    if (!out.isOk()) return Result<u64, Bail>::err(out.error());
    if (!callee.hasReturn) return Result<u64, Bail>::ok(0);

    // 回跳结果过 JNI 边界:不具体即 Bail,不编 0。
    Result<u64, Bail> bits2 = detail::encodeByValType(out.value(), env);
    if (!bits2.isOk()) return Result<u64, Bail>::err(bits2.error());
    // 浮点返回走 XMM 位型(执行器按 FuncMeta::Ret 决定写哪个寄存器文件)。
    return Result<u64, Bail>::ok(bits2.value());
}

Result<u64, Bail> newObjectImpl(JniCall& call, int variant)
{
    JniEnv& env = call.env;
    if (call.args.size() < 2) {
        return Result<u64, Bail>::err({Bail::Reason::StackBroken, 0, "NewObject 实参不足"});
    }
    Result<const HandlePool::Member*, Bail> mid = env.pool.memberOf(call.args[1]);
    if (!mid.isOk()) return Result<u64, Bail>::err(mid.error());
    const HandlePool::Member& m = *mid.value();

    const u64 id = env.session->nextAllocId++;
    HeapObj obj;
    obj.className = m.owner;
    env.session->heap[id] = std::move(obj);
    Result<u64, Bail> selfRef = env.pool.makeRef(SimValue::knownObj(id), false);
    if (!selfRef.isOk()) return Result<u64, Bail>::err(selfRef.error());
    const u64 selfHandle = selfRef.value();

    if (env.javaInvoker == nullptr) {
        return Result<u64, Bail>::err({Bail::Reason::UnsupportedCall, 0,
                                       "NewObject 需要 env.javaInvoker(构造语义属消费方)"});
    }
    std::vector<u64> bits;
    if (variant == 0) {
        Result<std::vector<u64>, Bail> b = detail::collectBareArgs(call, m.desc);
        if (!b.isOk()) return Result<u64, Bail>::err(b.error());
        bits = std::move(b.value());
    } else if (variant == 2) {
        if (call.args.size() < 3) {
            return Result<u64, Bail>::err({Bail::Reason::StackBroken, 0, "NewObjectA 缺 jvalue*"});
        }
        Result<std::vector<u64>, Bail> b = detail::readJValueArray(env, call.args[2], m.desc);
        if (!b.isOk()) return Result<u64, Bail>::err(b.error());
        bits = std::move(b.value());
    } else {
        if (env.currentAbi == NativeSpec::Abi::SysV) {
            return Result<u64, Bail>::err({Bail::Reason::UnsupportedCall, 0, "SysV va_list v1 不做"});
        }
        if (call.args.size() < 3) {
            return Result<u64, Bail>::err({Bail::Reason::StackBroken, 0, "NewObjectV 缺 va_list"});
        }
        Result<std::vector<u64>, Bail> b = detail::readJValueArray(env, call.args[2], m.desc);
        if (!b.isOk()) return Result<u64, Bail>::err(b.error());
        bits = std::move(b.value());
    }
    // 实参按 \<init> 的 desc 解码(this 不在 desc 里,解码后前插)。
    Result<std::vector<SimValue>, Bail> argv = detail::decodeArgs(m.desc, bits, env);
    if (!argv.isOk()) return Result<u64, Bail>::err(argv.error());
    argv.value().insert(argv.value().begin(), SimValue::knownObj(id));

    CallRef callee;
    callee.owner = m.owner;
    callee.name = "<init>";
    callee.desc = m.desc;
    callee.retType = ValType::I32;
    callee.hasReturn = false;
    callee.isStatic = false;
    Result<SimValue, Bail> out = env.javaInvoker(callee, std::move(argv.value()));
    if (!out.isOk()) return Result<u64, Bail>::err(out.error());
    return Result<u64, Bail>::ok(selfHandle);
}

void installSkeletons(JniEnv& env)
{
    // ---- 版本与类 ----
    putHook(env, "GetVersion", [](JniCall&) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(0x00010008); // JNI_VERSION_1_8
    });
    putHook(env, "FindClass", [](JniCall& call) -> Result<u64, Bail> {
        if (call.args[0] == 0) return Result<u64, Bail>::ok(0);
        Result<std::string, Bail> name = detail::readCStr(call.env, call.args[0]);
        if (!name.isOk()) return Result<u64, Bail>::err(name.error());
        // 模型无类层次,任何名字都建对象——typo 类当找到用是已接受的近似。
        const u64 cid = call.env.session->classObjectOf(name.value());
        return call.env.pool.makeRef(SimValue::knownObj(cid), false);
    });
    putHook(env, "GetObjectClass", [](JniCall& call) -> Result<u64, Bail> {
        Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        std::string cls;
        if (v.value().ref.kind == RefPayload::Kind::Null) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "GetObjectClass(null)"});
        }
        if (v.value().ref.kind == RefPayload::Kind::Str) {
            cls = "java/lang/String";
        } else {
            auto it = call.env.session->heap.find(v.value().ref.objId);
            if (it == call.env.session->heap.end()) {
                return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "GetObjectClass 堆对象缺失"});
            }
            cls = it->second.className;
        }
        const u64 cid = call.env.session->classObjectOf(cls);
        return call.env.pool.makeRef(SimValue::knownObj(cid), false);
    });
    putHook(env, "GetSuperclass", [](JniCall&) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(0); // 无类层次,v1 恒 null
    });
    putHook(env, "IsAssignableFrom", [](JniCall& call) -> Result<u64, Bail> {
        Result<std::string, Bail> a = detail::classNameOfClass(call.env, call.args[0]);
        Result<std::string, Bail> b = detail::classNameOfClass(call.env, call.args[1]);
        // 同名可赋值;其余无层次可判,按 false 并留观测。
        const bool ok = a.isOk() && b.isOk() && a.value() == b.value();
        if (!ok && call.env.trace) call.env.trace("[JNI] IsAssignableFrom 无类型层次,按同名判定");
        return Result<u64, Bail>::ok(ok ? 1 : 0);
    });

    // ---- 异常面 ----
    putHook(env, "ThrowNew", [](JniCall& call) -> Result<u64, Bail> {
        Result<std::string, Bail> cls = detail::classNameOfClass(call.env, call.args[0]);
        call.env.pendingThrow = true;
        call.env.throwClass = cls.isOk() ? cls.value() : "<unknown>";
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "Throw", [](JniCall& call) -> Result<u64, Bail> {
        Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
        std::string cls = "<unknown>";
        if (v.isOk() && v.value().ref.kind == RefPayload::Kind::Obj) {
            auto it = call.env.session->heap.find(v.value().ref.objId);
            if (it != call.env.session->heap.end()) cls = it->second.className;
        }
        call.env.pendingThrow = true;
        call.env.throwClass = cls;
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "ExceptionOccurred", [](JniCall& call) -> Result<u64, Bail> {
        if (!call.env.pendingThrow) return Result<u64, Bail>::ok(0);
        // 假 throwable:空壳对象,类名即异常类名(观测用)。
        const u64 id = call.env.session->nextAllocId++;
        HeapObj obj;
        obj.className = call.env.throwClass;
        call.env.session->heap[id] = std::move(obj);
        return call.env.pool.makeRef(SimValue::knownObj(id), false);
    });
    putHook(env, "ExceptionDescribe", [](JniCall&) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(0); // 无 stderr 面,no-op
    });
    putHook(env, "ExceptionClear", [](JniCall& call) -> Result<u64, Bail> {
        call.env.pendingThrow = false;
        call.env.throwClass.clear();
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "ExceptionCheck", [](JniCall& call) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(call.env.pendingThrow ? 1 : 0);
    });

    // ---- 引用面 ----
    putHook(env, "NewGlobalRef", [](JniCall& call) -> Result<u64, Bail> {
        if (call.args[0] == 0) return Result<u64, Bail>::ok(0);
        Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        return call.env.pool.makeRef(v.value(), true);
    });
    putHook(env, "NewLocalRef", [](JniCall& call) -> Result<u64, Bail> {
        if (call.args[0] == 0) return Result<u64, Bail>::ok(0);
        Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        return call.env.pool.makeRef(v.value(), false);
    });
    putHook(env, "DeleteGlobalRef", [](JniCall& call) -> Result<u64, Bail> {
        call.env.pool.refs.erase(call.args[0]);
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "DeleteLocalRef", [](JniCall& call) -> Result<u64, Bail> {
        call.env.pool.refs.erase(call.args[0]);
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "DeleteWeakGlobalRef", [](JniCall& call) -> Result<u64, Bail> {
        call.env.pool.refs.erase(call.args[0]); // 弱引用按强引用建模(v1 简化)
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "NewWeakGlobalRef", [](JniCall& call) -> Result<u64, Bail> {
        if (call.args[0] == 0) return Result<u64, Bail>::ok(0);
        Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        return call.env.pool.makeRef(v.value(), true);
    });
    putHook(env, "IsSameObject", [](JniCall& call) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(call.env.pool.sameObject(call.args[0], call.args[1]) ? 1 : 0);
    });
    putHook(env, "EnsureLocalCapacity", [](JniCall&) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(0); // 池无上限
    });
    putHook(env, "PushLocalFrame", [](JniCall&) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(0); // 帧嵌套 v1 不分域,统一 popLocals
    });
    putHook(env, "PopLocalFrame", [](JniCall& call) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(call.args[0]); // result 句柄原样带回
    });

    // ---- 成员 ID ----
    auto memberHook = [](bool isStatic) {
        return [isStatic](JniCall& call) -> Result<u64, Bail> {
            Result<std::string, Bail> cls = detail::classNameOfClass(call.env, call.args[0]);
            if (!cls.isOk()) return Result<u64, Bail>::err(cls.error());
            Result<std::string, Bail> name = detail::readCStr(call.env, call.args[1]);
            if (!name.isOk()) return Result<u64, Bail>::err(name.error());
            Result<std::string, Bail> sig = detail::readCStr(call.env, call.args[2]);
            if (!sig.isOk()) return Result<u64, Bail>::err(sig.error());
            HandlePool::Member m;
            m.owner = cls.value();
            m.name = name.value();
            m.desc = sig.value();
            m.isStatic = isStatic;
            return call.env.pool.makeMember(m);
        };
    };
    putHook(env, "GetMethodID", memberHook(false));
    putHook(env, "GetStaticMethodID", memberHook(true));
    putHook(env, "GetFieldID", memberHook(false));
    putHook(env, "GetStaticFieldID", memberHook(true));

    // ---- Call*Method:十类型 × (bare/V/A) × (实例/静态),同名共用实现 ----
    static const char* kMethodTypes[] = {"Object", "Boolean", "Byte",  "Char",   "Short",
                                         "Int",    "Long",    "Float", "Double", "Void"};
    auto installCallFamily = [&env](bool isStatic, int variant) {
        const char* stem = isStatic ? "CallStatic" : "Call";
        const char* tail = variant == 0 ? "" : (variant == 1 ? "V" : "A");
        for (const char* t : kMethodTypes) {
            putHook(env, (std::string(stem) + t + "Method" + tail).c_str(),
                    [isStatic, variant](JniCall& call) -> Result<u64, Bail> {
                        return callMethodImpl(call, isStatic, variant);
                    });
        }
    };
    installCallFamily(false, 0);  // Call*Method
    installCallFamily(false, 1);  // Call*MethodV
    installCallFamily(false, 2);  // Call*MethodA
    installCallFamily(true, 0);   // CallStatic*Method
    installCallFamily(true, 1);
    installCallFamily(true, 2);

    auto installNewObjectFamily = [&env](int variant) {
        std::string name = std::string("NewObject") + (variant == 0 ? "" : (variant == 1 ? "V" : "A"));
        putHook(env, name.c_str(), [variant](JniCall& call) -> Result<u64, Bail> {
            return newObjectImpl(call, variant);
        });
    };
    installNewObjectFamily(0);
    installNewObjectFamily(1);
    installNewObjectFamily(2);

    // ---- 字段读写:类型由 fieldID 的 desc 决定,九形共用四个实现 ----
    auto getStaticField = [](JniCall& call) -> Result<u64, Bail> {
        Result<const HandlePool::Member*, Bail> f = call.env.pool.memberOf(call.args[1]);
        if (!f.isOk()) return Result<u64, Bail>::err(f.error());
        const std::string key = f.value()->owner + "::" + f.value()->name + "::" + f.value()->desc;
        auto it = call.env.session->statics.find(key);
        if (it == call.env.session->statics.end()) {
            // native 侧无法表达 Unknown:缺值即断链,消费方用 Session.statics 预填。
            return Result<u64, Bail>::err({Bail::Reason::NotConcrete, 0,
                                           "静态字段无值(预填 Session.statics):" + key});
        }
        return detail::encodeByValType(it->second, call.env);
    };
    auto setStaticField = [](JniCall& call) -> Result<u64, Bail> {
        Result<const HandlePool::Member*, Bail> f = call.env.pool.memberOf(call.args[1]);
        if (!f.isOk()) return Result<u64, Bail>::err(f.error());
        const std::string key = f.value()->owner + "::" + f.value()->name + "::" + f.value()->desc;
        Result<std::pair<ValType, bool>, Bail> rt = retTypeOfDesc("(x)" + f.value()->desc);
        if (!rt.isOk()) return Result<u64, Bail>::err(rt.error());
        Result<SimValue, Bail> v = detail::decodeByValType(rt.value().first, call.args[2], call.env);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        call.env.session->statics[key] = std::move(v.value());
        return Result<u64, Bail>::ok(0);
    };
    auto getInstanceField = [](JniCall& call) -> Result<u64, Bail> {
        Result<SimValue, Bail> recv = call.env.pool.deref(call.args[0]);
        if (!recv.isOk()) return Result<u64, Bail>::err(recv.error());
        if (recv.value().ref.kind == RefPayload::Kind::Null) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "getXField(null) 需 NPE 派发"});
        }
        if (recv.value().ref.kind != RefPayload::Kind::Obj) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "接收者无实例字段面"});
        }
        Result<const HandlePool::Member*, Bail> f = call.env.pool.memberOf(call.args[1]);
        if (!f.isOk()) return Result<u64, Bail>::err(f.error());
        auto it = call.env.session->heap.find(recv.value().ref.objId);
        if (it == call.env.session->heap.end()) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "接收者堆对象缺失"});
        }
        auto slot = it->second.fields.find(f.value()->name + "::" + f.value()->desc);
        if (slot == it->second.fields.end()) {
            return Result<u64, Bail>::err({Bail::Reason::NotConcrete, 0,
                                           "实例字段无值:" + f.value()->name});
        }
        return detail::encodeByValType(slot->second, call.env);
    };
    auto setInstanceField = [](JniCall& call) -> Result<u64, Bail> {
        Result<SimValue, Bail> recv = call.env.pool.deref(call.args[0]);
        if (!recv.isOk()) return Result<u64, Bail>::err(recv.error());
        if (recv.value().ref.kind == RefPayload::Kind::Null) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "putXField(null) 需 NPE 派发"});
        }
        if (recv.value().ref.kind != RefPayload::Kind::Obj) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "接收者无实例字段面"});
        }
        Result<const HandlePool::Member*, Bail> f = call.env.pool.memberOf(call.args[1]);
        if (!f.isOk()) return Result<u64, Bail>::err(f.error());
        Result<std::pair<ValType, bool>, Bail> rt = retTypeOfDesc("(x)" + f.value()->desc);
        if (!rt.isOk()) return Result<u64, Bail>::err(rt.error());
        Result<SimValue, Bail> v = detail::decodeByValType(rt.value().first, call.args[2], call.env);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        auto it = call.env.session->heap.find(recv.value().ref.objId);
        if (it == call.env.session->heap.end()) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "接收者堆对象缺失"});
        }
        it->second.fields[f.value()->name + "::" + f.value()->desc] = std::move(v.value());
        return Result<u64, Bail>::ok(0);
    };
    static const char* kFieldTypes[] = {"Object", "Boolean", "Byte",  "Char",   "Short",
                                        "Int",    "Long",    "Float", "Double"};
    for (const char* t : kFieldTypes) {
        putHook(env, (std::string("GetStatic") + t + "Field").c_str(), getStaticField);
        putHook(env, (std::string("SetStatic") + t + "Field").c_str(), setStaticField);
        putHook(env, (std::string("Get") + t + "Field").c_str(), getInstanceField);
        putHook(env, (std::string("Set") + t + "Field").c_str(), setInstanceField);
    }

    detail::installStringHooks(env);
    detail::installArrayHooks(env);

    // ---- 杂项 ----
    putHook(env, "RegisterNatives", [](JniCall& call) -> Result<u64, Bail> {
        Result<std::string, Bail> cls = detail::classNameOfClass(call.env, call.args[0]);
        if (!cls.isOk()) return Result<u64, Bail>::err(cls.error());
        const u64 tableAddr = call.args[1];
        const i32 n = static_cast<i32>(call.args[2]);
        // JNINativeMethod = { const char* name; const char* sig; void* fnPtr; } × 8 字节
        for (i32 i = 0; i < n; ++i) {
            u64 row[3] = {0, 0, 0};
            Result<void, Bail> r = call.env.mem.read(tableAddr + static_cast<usize>(i) * 24, row, 24);
            if (!r.isOk()) return Result<u64, Bail>::err(r.error());
            Result<std::string, Bail> name = detail::readCStr(call.env, row[0]);
            if (!name.isOk()) return Result<u64, Bail>::err(name.error());
            Result<std::string, Bail> sig = detail::readCStr(call.env, row[1]);
            if (!sig.isOk()) return Result<u64, Bail>::err(sig.error());
            NativeSpec spec;
            spec.entry = row[2];
            spec.abi = call.env.currentAbi;
            call.env.methods[cls.value() + "." + name.value() + sig.value()] = spec;
        }
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "UnregisterNatives", [](JniCall&) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(0); // v1 不清表(j2c 注册一次跑全程)
    });
    putHook(env, "MonitorEnter", [](JniCall&) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(0); // monitor 不进模型(doc:design-v1-scope)
    });
    putHook(env, "MonitorExit", [](JniCall&) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "GetJavaVM", [](JniCall& call) -> Result<u64, Bail> {
        const u64 vm = HandlePool::kJavaVm;
        Result<void, Bail> r = call.env.mem.write(call.args[0], &vm, 8);
        if (!r.isOk()) return Result<u64, Bail>::err(r.error());
        return Result<u64, Bail>::ok(0); // JNI_OK
    });
    putHook(env, "AllocObject", [](JniCall& call) -> Result<u64, Bail> {
        Result<std::string, Bail> cls = detail::classNameOfClass(call.env, call.args[0]);
        if (!cls.isOk()) return Result<u64, Bail>::err(cls.error());
        const u64 id = call.env.session->nextAllocId++;
        HeapObj obj;
        obj.className = cls.value();
        call.env.session->heap[id] = std::move(obj);
        return call.env.pool.makeRef(SimValue::knownObj(id), false);
    });
    putHook(env, "IsInstanceOf", [](JniCall& call) -> Result<u64, Bail> {
        // 无类型层次:同对象判真,其余判假并留观测(doc:ext-jni 口径)。
        const bool same = call.env.pool.sameObject(call.args[0], call.args[1]);
        if (!same && call.env.trace) call.env.trace("[JNI] IsInstanceOf 无类型层次,按身份判定");
        return Result<u64, Bail>::ok(same ? 1 : 0);
    });
}

} // namespace jsim::jni
