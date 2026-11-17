#include "detail/guest.hpp"

#include <cstring>

namespace jsim::jni {

void detail::installArrayHooks(JniEnv& env)
{
    auto newArrayHook = [](const char* cls) {
        return [cls](JniCall& call) -> Result<u64, Bail> {
            const i32 len = static_cast<i32>(call.args[0]);
            if (len < 0) {
                return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "负数组长度"});
            }
            const u64 id = call.env.session->nextAllocId++;
            HeapObj obj;
            obj.className = cls;
            obj.elems.assign(static_cast<usize>(len), 0);
            call.env.session->heap[id] = std::move(obj);
            return call.env.pool.makeRef(SimValue::knownObj(id), false);
        };
    };
    putHook(env, "NewBooleanArray", newArrayHook("[Z"));
    putHook(env, "NewByteArray", newArrayHook("[B"));
    putHook(env, "NewCharArray", newArrayHook("[C"));
    putHook(env, "NewShortArray", newArrayHook("[S"));
    putHook(env, "NewIntArray", newArrayHook("[I"));
    putHook(env, "NewLongArray", newArrayHook("[J"));
    putHook(env, "NewFloatArray", newArrayHook("[F"));
    putHook(env, "NewDoubleArray", newArrayHook("[D"));

    putHook(env, "GetArrayLength", [](JniCall& call) -> Result<u64, Bail> {
        Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        if (v.value().ref.kind != RefPayload::Kind::Obj) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "GetArrayLength 接收者非数组"});
        }
        auto it = call.env.session->heap.find(v.value().ref.objId);
        if (it == call.env.session->heap.end()) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "GetArrayLength 堆对象缺失"});
        }
        const HeapObj& obj = it->second;
        const usize n = detail::isRefArrayClass(obj.className) ? obj.refs.size() : obj.elems.size();
        return Result<u64, Bail>::ok(n);
    });
    putHook(env, "NewObjectArray", [](JniCall& call) -> Result<u64, Bail> {
        const i32 len = static_cast<i32>(call.args[0]);
        if (len < 0) return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "负数组长度"});
        Result<std::string, Bail> elem = detail::classNameOfClass(call.env, call.args[1]);
        if (!elem.isOk()) return Result<u64, Bail>::err(elem.error());
        Result<SimValue, Bail> init = call.env.pool.deref(call.args[2]);
        if (!init.isOk()) return Result<u64, Bail>::err(init.error());
        const u64 id = call.env.session->nextAllocId++;
        HeapObj obj;
        obj.className = "[L" + elem.value() + ";";
        obj.refs.assign(static_cast<usize>(len), init.value());
        call.env.session->heap[id] = std::move(obj);
        return call.env.pool.makeRef(SimValue::knownObj(id), false);
    });
    putHook(env, "GetObjectArrayElement", [](JniCall& call) -> Result<u64, Bail> {
        Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        auto it = call.env.session->heap.find(v.value().ref.objId);
        if (it == call.env.session->heap.end()) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "数组堆对象缺失"});
        }
        const usize idx = static_cast<usize>(static_cast<i32>(call.args[1]));
        if (idx >= it->second.refs.size()) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "数组下标越界"});
        }
        return call.env.pool.makeRef(it->second.refs[idx], false);
    });
    putHook(env, "SetObjectArrayElement", [](JniCall& call) -> Result<u64, Bail> {
        Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        auto it = call.env.session->heap.find(v.value().ref.objId);
        if (it == call.env.session->heap.end()) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "数组堆对象缺失"});
        }
        const usize idx = static_cast<usize>(static_cast<i32>(call.args[1]));
        if (idx >= it->second.refs.size()) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "数组下标越界"});
        }
        Result<SimValue, Bail> val = call.env.pool.deref(call.args[2]);
        if (!val.isOk()) return Result<u64, Bail>::err(val.error());
        it->second.refs[idx] = std::move(val.value());
        return Result<u64, Bail>::ok(0);
    });

    // 元素宽与符号性由数组类名决定(引擎同款口径);Region 系直通堆不经副本。
    auto regionHook = [](bool isSet) {
        return [isSet](JniCall& call) -> Result<u64, Bail> {
            Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
            if (!v.isOk()) return Result<u64, Bail>::err(v.error());
            auto it = call.env.session->heap.find(v.value().ref.objId);
            if (it == call.env.session->heap.end()) {
                return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "数组堆对象缺失"});
            }
            HeapObj& arr = it->second;
            if (detail::isRefArrayClass(arr.className)) {
                return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "引用数组不走 Region 面"});
            }
            const usize w = detail::elemWidthOf(arr.className);
            const i32 start = static_cast<i32>(call.args[1]);
            const i32 len = static_cast<i32>(call.args[2]);
            if (start < 0 || len < 0 || static_cast<usize>(start) + static_cast<usize>(len) > arr.elems.size()) {
                return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "数组区间越界"});
            }
            const usize nBytes = static_cast<usize>(len) * w;
            if (!isSet) {
                std::vector<u8> buf(nBytes);
                for (i32 i = 0; i < len; ++i) {
                    const u64 raw = arr.elems[static_cast<usize>(start) + static_cast<usize>(i)];
                    for (usize b = 0; b < w; ++b) {
                        buf[static_cast<usize>(i) * w + b] = static_cast<u8>((raw >> (b * 8)) & 0xFF);
                    }
                }
                Result<void, Bail> w = detail::writeBytes(call.env, call.args[3], buf.data(), buf.size());
                if (!w.isOk()) return Result<u64, Bail>::err(w.error());
                return Result<u64, Bail>::ok(0);
            }
            std::vector<u8> buf(nBytes);
            Result<void, Bail> r = call.env.mem.read(call.args[3], buf.data(), buf.size());
            if (!r.isOk()) return Result<u64, Bail>::err(r.error());
            for (i32 i = 0; i < len; ++i) {
                u64 raw = 0;
                for (usize b = 0; b < w; ++b) {
                    raw |= static_cast<u64>(buf[static_cast<usize>(i) * w + b]) << (b * 8);
                }
                arr.elems[static_cast<usize>(start) + static_cast<usize>(i)] = static_cast<i64>(raw);
            }
            return Result<u64, Bail>::ok(0);
        };
    };
    putHook(env, "GetBooleanArrayRegion", regionHook(false));
    putHook(env, "GetByteArrayRegion", regionHook(false));
    putHook(env, "GetCharArrayRegion", regionHook(false));
    putHook(env, "GetShortArrayRegion", regionHook(false));
    putHook(env, "GetIntArrayRegion", regionHook(false));
    putHook(env, "GetLongArrayRegion", regionHook(false));
    putHook(env, "GetFloatArrayRegion", regionHook(false));
    putHook(env, "GetDoubleArrayRegion", regionHook(false));
    putHook(env, "SetBooleanArrayRegion", regionHook(true));
    putHook(env, "SetByteArrayRegion", regionHook(true));
    putHook(env, "SetCharArrayRegion", regionHook(true));
    putHook(env, "SetShortArrayRegion", regionHook(true));
    putHook(env, "SetIntArrayRegion", regionHook(true));
    putHook(env, "SetLongArrayRegion", regionHook(true));
    putHook(env, "SetFloatArrayRegion", regionHook(true));
    putHook(env, "SetDoubleArrayRegion", regionHook(true));

    // Elements/Critical:副本物化。Release 按 JNI 契约处理 mode:0=写回+回收,
    // JNI_COMMIT(1)=写回不回收,JNI_ABORT(2)=丢弃副本只回收。
    auto getElementsHook = [](JniCall& call) -> Result<u64, Bail> {
        Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        auto it = call.env.session->heap.find(v.value().ref.objId);
        if (it == call.env.session->heap.end()) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "数组堆对象缺失"});
        }
        HeapObj& arr = it->second;
        if (detail::isRefArrayClass(arr.className)) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "引用数组不走 Elements 面"});
        }
        const usize w = detail::elemWidthOf(arr.className);
        std::vector<u8> buf(arr.elems.size() * w);
        for (usize i = 0; i < arr.elems.size(); ++i) {
            for (usize b = 0; b < w; ++b) {
                buf[i * w + b] = static_cast<u8>((static_cast<u64>(arr.elems[i]) >> (b * 8)) & 0xFF);
            }
        }
        Result<u64, Bail> p = materialize(call.env, buf.data(), buf.size(), static_cast<usize>(w));
        if (!p.isOk()) return Result<u64, Bail>::err(p.error());
        detail::markCopy(call.env, call.args[1]);
        return Result<u64, Bail>::ok(p.value());
    };
    auto releaseElementsHook = [](JniCall& call) -> Result<u64, Bail> {
        const u64 mode = call.args[2];
        const bool doCopyBack = mode != 2;             // JNI_ABORT(2):丢弃副本
        const bool doFree = mode != 1;                 // JNI_COMMIT(1):写回不回收
        if (doCopyBack) {
            Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
            if (!v.isOk()) return Result<u64, Bail>::err(v.error());
            auto it = call.env.session->heap.find(v.value().ref.objId);
            if (it == call.env.session->heap.end()) {
                return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "数组堆对象缺失"});
            }
            HeapObj& arr = it->second;
            const usize w = detail::elemWidthOf(arr.className);
            const usize n = arr.elems.size();
            std::vector<u8> buf(n * w);
            Result<void, Bail> r = call.env.mem.read(call.args[1], buf.data(), buf.size());
            if (!r.isOk()) return Result<u64, Bail>::err(r.error());
            for (usize i = 0; i < n; ++i) {
                u64 raw = 0;
                for (usize b = 0; b < w; ++b) raw |= static_cast<u64>(buf[i * w + b]) << (b * 8);
                arr.elems[i] = static_cast<i64>(raw);
            }
        }
        if (doFree && call.env.mem.free != nullptr) call.env.mem.free(call.args[1]);
        return Result<u64, Bail>::ok(0);
    };
    putHook(env, "GetBooleanArrayElements", getElementsHook);
    putHook(env, "GetByteArrayElements", getElementsHook);
    putHook(env, "GetCharArrayElements", getElementsHook);
    putHook(env, "GetShortArrayElements", getElementsHook);
    putHook(env, "GetIntArrayElements", getElementsHook);
    putHook(env, "GetLongArrayElements", getElementsHook);
    putHook(env, "GetFloatArrayElements", getElementsHook);
    putHook(env, "GetDoubleArrayElements", getElementsHook);
    putHook(env, "ReleaseBooleanArrayElements", releaseElementsHook);
    putHook(env, "ReleaseByteArrayElements", releaseElementsHook);
    putHook(env, "ReleaseCharArrayElements", releaseElementsHook);
    putHook(env, "ReleaseShortArrayElements", releaseElementsHook);
    putHook(env, "ReleaseIntArrayElements", releaseElementsHook);
    putHook(env, "ReleaseLongArrayElements", releaseElementsHook);
    putHook(env, "ReleaseFloatArrayElements", releaseElementsHook);
    putHook(env, "ReleaseDoubleArrayElements", releaseElementsHook);
    putHook(env, "GetPrimitiveArrayCritical", getElementsHook);
    putHook(env, "ReleasePrimitiveArrayCritical", releaseElementsHook);
}

} // namespace jsim::jni
