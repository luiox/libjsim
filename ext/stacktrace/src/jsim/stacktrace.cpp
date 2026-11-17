#include "jsim/stacktrace.hpp"

#include <limits>
#include <memory>

namespace jsim::stacktrace {
namespace {

inline constexpr char threadClass[] = "java/lang/Thread";
inline constexpr char elementClass[] = "java/lang/StackTraceElement";
inline constexpr char elementArray[] = "[Ljava/lang/StackTraceElement;";

using ValueResult = Result<SimValue, Bail>;

inline SimValue allocate(Session& session, HeapObj obj) {
    const u64 id = session.nextAllocId++;
    session.heap.emplace(id, std::move(obj));
    return SimValue::knownObj(id);
}

inline bool hasThread(const Session& session, u64 id) {
    const auto it = session.heap.find(id);
    return it != session.heap.end() && it->second.className == threadClass;
}

inline CallMock getter(const char* field, ValType type) {
    return [field, type](Frame& frame) -> ValueResult {
        auto value = frame.popChecked();
        if (!value.isOk()) return value;
        const SimValue& recv = value.value();
        if (recv.type == ValType::Ref && !recv.known) {
            return ValueResult::ok(SimValue::unknownOf(type));
        }
        if (recv.type == ValType::Ref && recv.ref.kind == RefPayload::Kind::Obj) {
            const auto& heap = frame.session().heap;
            auto obj = heap.find(recv.ref.objId);
            if (obj != heap.end() && obj->second.className == elementClass) {
                auto slot = obj->second.fields.find(field);
                if (slot != obj->second.fields.end()) return ValueResult::ok(slot->second);
            }
        }
        return ValueResult::err({Bail::Reason::BadOperand, frame.pc(),
                                 "需要 jsim-stacktrace 生成的 StackTraceElement"});
    };
}

} // namespace

void install(Session& session, Provider provider) {
    HeapObj thread;
    thread.className = threadClass;
    const SimValue currentThread = allocate(session, std::move(thread));
    auto& mocks = session.callMocks;
    mocks["java/lang/Thread.currentThread()Ljava/lang/Thread;"] =
        [currentThread](Frame&) { return ValueResult::ok(currentThread); };
    // CallMock 在派发时会被复制，调用栈 provider 可以保留跨调用状态。
    const auto traceProvider = std::make_shared<Provider>(std::move(provider));
    mocks["java/lang/Thread.getStackTrace()[Ljava/lang/StackTraceElement;"] =
        [id = currentThread.ref.objId, traceProvider](Frame& frame) -> ValueResult {
        auto value = frame.peekChecked();
        if (!value.isOk()) return value;
        const SimValue& recv = value.value();
        if (recv.type != ValType::Ref || (recv.known && recv.ref.kind == RefPayload::Kind::Null)) {
            return ValueResult::err({Bail::Reason::BadOperand, frame.pc(), "无效的 Thread 接收者"});
        }
        if (!recv.known || recv.ref.kind != RefPayload::Kind::Obj || recv.ref.objId != id || !*traceProvider) {
            return ValueResult::err({Bail::Reason::NotConcrete, frame.pc(), "线程调用栈不可用"});
        }
        Session& session = frame.session();
        if (!hasThread(session, id)) {
            return ValueResult::err({Bail::Reason::BadOperand, frame.pc(), "虚拟线程对象已失效"});
        }
        auto trace = (*traceProvider)(frame);
        if (!trace.isOk()) return ValueResult::err(trace.error());
        if (!hasThread(session, id)) {
            return ValueResult::err({Bail::Reason::BadOperand, frame.pc(), "虚拟线程对象已失效"});
        }
        const auto& entries = trace.value();
        if (entries.size() > static_cast<usize>((std::numeric_limits<i32>::max)())) {
            return ValueResult::err({Bail::Reason::BadOperand, frame.pc(), "调用栈超过 JVM 数组容量"});
        }
        frame.popChecked(); // 已确认栈顶接收者存在。
        HeapObj array;
        array.className = elementArray;
        array.refs.reserve(entries.size());
        for (const Element& entry : entries) {
            HeapObj element;
            element.className = elementClass;
            element.fields["@class"] = SimValue::knownStr(entry.className);
            element.fields["@method"] = SimValue::knownStr(entry.methodName);
            element.fields["@file"] = entry.fileName ? SimValue::knownStr(*entry.fileName)
                                                       : SimValue::knownNull();
            element.fields["@line"] = SimValue::knownI32(entry.lineNumber);
            element.fields["@native"] = SimValue::knownI32(entry.lineNumber == -2);
            array.refs.push_back(allocate(session, std::move(element)));
        }
        return ValueResult::ok(allocate(session, std::move(array)));
    };
    mocks["java/lang/StackTraceElement.getClassName()Ljava/lang/String;"] = getter("@class", ValType::Ref);
    mocks["java/lang/StackTraceElement.getMethodName()Ljava/lang/String;"] = getter("@method", ValType::Ref);
    mocks["java/lang/StackTraceElement.getFileName()Ljava/lang/String;"] = getter("@file", ValType::Ref);
    mocks["java/lang/StackTraceElement.getLineNumber()I"] = getter("@line", ValType::I32);
    mocks["java/lang/StackTraceElement.isNativeMethod()Z"] = getter("@native", ValType::I32);
}

} // namespace jsim::stacktrace
