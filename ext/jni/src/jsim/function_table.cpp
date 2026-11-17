#include "jsim/jni.hpp"

namespace jsim::jni {

/// 每行:(索引, 名, 定长参数个数, 返回类)。顺序即 JDK 8 jni.h 结构序。
#define JSIM_JNI_FUNCTIONS(X)                                                                                       \
    /* 0..3 保留 */                                                                                                  \
    X(4,   GetVersion, 0, Int)                                                                                      \
    X(5,   DefineClass, 4, Ptr)                                                                                     \
    X(6,   FindClass, 1, Ptr)                                                                                       \
    X(7,   FromReflectedMethod, 1, Ptr)                                                                             \
    X(8,   FromReflectedField, 1, Ptr)                                                                              \
    X(9,   ToReflectedMethod, 3, Ptr)                                                                               \
    X(10,  GetSuperclass, 1, Ptr)                                                                                   \
    X(11,  IsAssignableFrom, 2, Int)                                                                                \
    X(12,  ToReflectedField, 3, Ptr)                                                                                \
    X(13,  Throw, 1, Int)                                                                                           \
    X(14,  ThrowNew, 2, Int)                                                                                        \
    X(15,  ExceptionOccurred, 0, Ptr)                                                                               \
    X(16,  ExceptionDescribe, 0, Void)                                                                              \
    X(17,  ExceptionClear, 0, Void)                                                                                 \
    X(18,  FatalError, 1, Void)                                                                                     \
    X(19,  PushLocalFrame, 1, Int)                                                                                  \
    X(20,  PopLocalFrame, 1, Ptr)                                                                                   \
    X(21,  NewGlobalRef, 1, Ptr)                                                                                    \
    X(22,  DeleteGlobalRef, 1, Void)                                                                                \
    X(23,  DeleteLocalRef, 1, Void)                                                                                 \
    X(24,  IsSameObject, 2, Int)                                                                                    \
    X(25,  NewLocalRef, 1, Ptr)                                                                                     \
    X(26,  EnsureLocalCapacity, 1, Int)                                                                             \
    X(27,  AllocObject, 1, Ptr)                                                                                     \
    X(28,  NewObject, 2, Ptr)                                                                                       \
    X(29,  NewObjectV, 3, Ptr)                                                                                      \
    X(30,  NewObjectA, 3, Ptr)                                                                                      \
    X(31,  GetObjectClass, 1, Ptr)                                                                                  \
    X(32,  IsInstanceOf, 2, Int)                                                                                    \
    X(33,  GetMethodID, 3, Ptr)                                                                                     \
    X(34,  CallObjectMethod, 2, Ptr)                                                                                \
    X(35,  CallObjectMethodV, 3, Ptr)                                                                               \
    X(36,  CallObjectMethodA, 3, Ptr)                                                                               \
    X(37,  CallBooleanMethod, 2, Int)                                                                               \
    X(38,  CallBooleanMethodV, 3, Int)                                                                              \
    X(39,  CallBooleanMethodA, 3, Int)                                                                              \
    X(40,  CallByteMethod, 2, Int)                                                                                  \
    X(41,  CallByteMethodV, 3, Int)                                                                                 \
    X(42,  CallByteMethodA, 3, Int)                                                                                 \
    X(43,  CallCharMethod, 2, Int)                                                                                  \
    X(44,  CallCharMethodV, 3, Int)                                                                                 \
    X(45,  CallCharMethodA, 3, Int)                                                                                 \
    X(46,  CallShortMethod, 2, Int)                                                                                 \
    X(47,  CallShortMethodV, 3, Int)                                                                                \
    X(48,  CallShortMethodA, 3, Int)                                                                                \
    X(49,  CallIntMethod, 2, Int)                                                                                   \
    X(50,  CallIntMethodV, 3, Int)                                                                                  \
    X(51,  CallIntMethodA, 3, Int)                                                                                  \
    X(52,  CallLongMethod, 2, Int)                                                                                  \
    X(53,  CallLongMethodV, 3, Int)                                                                                 \
    X(54,  CallLongMethodA, 3, Int)                                                                                 \
    X(55,  CallFloatMethod, 2, F32)                                                                                 \
    X(56,  CallFloatMethodV, 3, F32)                                                                                \
    X(57,  CallFloatMethodA, 3, F32)                                                                                \
    X(58,  CallDoubleMethod, 2, F64)                                                                                \
    X(59,  CallDoubleMethodV, 3, F64)                                                                               \
    X(60,  CallDoubleMethodA, 3, F64)                                                                               \
    X(61,  CallVoidMethod, 2, Void)                                                                                 \
    X(62,  CallVoidMethodV, 3, Void)                                                                                \
    X(63,  CallVoidMethodA, 3, Void)                                                                                \
    X(64,  CallNonvirtualObjectMethod, 3, Ptr)                                                                      \
    X(65,  CallNonvirtualObjectMethodV, 4, Ptr)                                                                     \
    X(66,  CallNonvirtualObjectMethodA, 4, Ptr)                                                                     \
    X(67,  CallNonvirtualBooleanMethod, 3, Int)                                                                     \
    X(68,  CallNonvirtualBooleanMethodV, 4, Int)                                                                    \
    X(69,  CallNonvirtualBooleanMethodA, 4, Int)                                                                    \
    X(70,  CallNonvirtualByteMethod, 3, Int)                                                                        \
    X(71,  CallNonvirtualByteMethodV, 4, Int)                                                                       \
    X(72,  CallNonvirtualByteMethodA, 4, Int)                                                                       \
    X(73,  CallNonvirtualCharMethod, 3, Int)                                                                        \
    X(74,  CallNonvirtualCharMethodV, 4, Int)                                                                       \
    X(75,  CallNonvirtualCharMethodA, 4, Int)                                                                       \
    X(76,  CallNonvirtualShortMethod, 3, Int)                                                                       \
    X(77,  CallNonvirtualShortMethodV, 4, Int)                                                                      \
    X(78,  CallNonvirtualShortMethodA, 4, Int)                                                                      \
    X(79,  CallNonvirtualIntMethod, 3, Int)                                                                         \
    X(80,  CallNonvirtualIntMethodV, 4, Int)                                                                        \
    X(81,  CallNonvirtualIntMethodA, 4, Int)                                                                        \
    X(82,  CallNonvirtualLongMethod, 3, Int)                                                                        \
    X(83,  CallNonvirtualLongMethodV, 4, Int)                                                                       \
    X(84,  CallNonvirtualLongMethodA, 4, Int)                                                                       \
    X(85,  CallNonvirtualFloatMethod, 3, F32)                                                                       \
    X(86,  CallNonvirtualFloatMethodV, 4, F32)                                                                      \
    X(87,  CallNonvirtualFloatMethodA, 4, F32)                                                                      \
    X(88,  CallNonvirtualDoubleMethod, 3, F64)                                                                      \
    X(89,  CallNonvirtualDoubleMethodV, 4, F64)                                                                     \
    X(90,  CallNonvirtualDoubleMethodA, 4, F64)                                                                     \
    X(91,  CallNonvirtualVoidMethod, 3, Void)                                                                       \
    X(92,  CallNonvirtualVoidMethodV, 4, Void)                                                                      \
    X(93,  CallNonvirtualVoidMethodA, 4, Void)                                                                      \
    X(94,  GetFieldID, 3, Ptr)                                                                                      \
    X(95,  GetObjectField, 2, Ptr)                                                                                  \
    X(96,  GetBooleanField, 2, Int)                                                                                 \
    X(97,  GetByteField, 2, Int)                                                                                    \
    X(98,  GetCharField, 2, Int)                                                                                    \
    X(99,  GetShortField, 2, Int)                                                                                   \
    X(100, GetIntField, 2, Int)                                                                                     \
    X(101, GetLongField, 2, Int)                                                                                    \
    X(102, GetFloatField, 2, F32)                                                                                   \
    X(103, GetDoubleField, 2, F64)                                                                                  \
    X(104, SetObjectField, 3, Void)                                                                                 \
    X(105, SetBooleanField, 3, Void)                                                                                \
    X(106, SetByteField, 3, Void)                                                                                   \
    X(107, SetCharField, 3, Void)                                                                                   \
    X(108, SetShortField, 3, Void)                                                                                  \
    X(109, SetIntField, 3, Void)                                                                                    \
    X(110, SetLongField, 3, Void)                                                                                   \
    X(111, SetFloatField, 3, Void)                                                                                  \
    X(112, SetDoubleField, 3, Void)                                                                                 \
    X(113, GetStaticMethodID, 3, Ptr)                                                                               \
    X(114, CallStaticObjectMethod, 2, Ptr)                                                                          \
    X(115, CallStaticObjectMethodV, 3, Ptr)                                                                         \
    X(116, CallStaticObjectMethodA, 3, Ptr)                                                                         \
    X(117, CallStaticBooleanMethod, 2, Int)                                                                         \
    X(118, CallStaticBooleanMethodV, 3, Int)                                                                        \
    X(119, CallStaticBooleanMethodA, 3, Int)                                                                        \
    X(120, CallStaticByteMethod, 2, Int)                                                                            \
    X(121, CallStaticByteMethodV, 3, Int)                                                                           \
    X(122, CallStaticByteMethodA, 3, Int)                                                                           \
    X(123, CallStaticCharMethod, 2, Int)                                                                            \
    X(124, CallStaticCharMethodV, 3, Int)                                                                           \
    X(125, CallStaticCharMethodA, 3, Int)                                                                           \
    X(126, CallStaticShortMethod, 2, Int)                                                                           \
    X(127, CallStaticShortMethodV, 3, Int)                                                                          \
    X(128, CallStaticShortMethodA, 3, Int)                                                                          \
    X(129, CallStaticIntMethod, 2, Int)                                                                             \
    X(130, CallStaticIntMethodV, 3, Int)                                                                            \
    X(131, CallStaticIntMethodA, 3, Int)                                                                            \
    X(132, CallStaticLongMethod, 2, Int)                                                                            \
    X(133, CallStaticLongMethodV, 3, Int)                                                                           \
    X(134, CallStaticLongMethodA, 3, Int)                                                                           \
    X(135, CallStaticFloatMethod, 2, F32)                                                                           \
    X(136, CallStaticFloatMethodV, 3, F32)                                                                          \
    X(137, CallStaticFloatMethodA, 3, F32)                                                                          \
    X(138, CallStaticDoubleMethod, 2, F64)                                                                          \
    X(139, CallStaticDoubleMethodV, 3, F64)                                                                         \
    X(140, CallStaticDoubleMethodA, 3, F64)                                                                         \
    X(141, CallStaticVoidMethod, 2, Void)                                                                           \
    X(142, CallStaticVoidMethodV, 3, Void)                                                                          \
    X(143, CallStaticVoidMethodA, 3, Void)                                                                          \
    X(144, GetStaticFieldID, 3, Ptr)                                                                                \
    X(145, GetStaticObjectField, 2, Ptr)                                                                            \
    X(146, GetStaticBooleanField, 2, Int)                                                                           \
    X(147, GetStaticByteField, 2, Int)                                                                              \
    X(148, GetStaticCharField, 2, Int)                                                                              \
    X(149, GetStaticShortField, 2, Int)                                                                             \
    X(150, GetStaticIntField, 2, Int)                                                                               \
    X(151, GetStaticLongField, 2, Int)                                                                              \
    X(152, GetStaticFloatField, 2, F32)                                                                             \
    X(153, GetStaticDoubleField, 2, F64)                                                                            \
    X(154, SetStaticObjectField, 3, Void)                                                                           \
    X(155, SetStaticBooleanField, 3, Void)                                                                          \
    X(156, SetStaticByteField, 3, Void)                                                                             \
    X(157, SetStaticCharField, 3, Void)                                                                             \
    X(158, SetStaticShortField, 3, Void)                                                                            \
    X(159, SetStaticIntField, 3, Void)                                                                              \
    X(160, SetStaticLongField, 3, Void)                                                                             \
    X(161, SetStaticFloatField, 3, Void)                                                                            \
    X(162, SetStaticDoubleField, 3, Void)                                                                           \
    X(163, NewString, 2, Ptr)                                                                                       \
    X(164, GetStringLength, 1, Int)                                                                                 \
    X(165, GetStringChars, 2, Ptr)                                                                                  \
    X(166, ReleaseStringChars, 2, Void)                                                                             \
    X(167, NewStringUTF, 1, Ptr)                                                                                    \
    X(168, GetStringUTFLength, 1, Int)                                                                              \
    X(169, GetStringUTFChars, 2, Ptr)                                                                               \
    X(170, ReleaseStringUTFChars, 2, Void)                                                                          \
    X(171, GetArrayLength, 1, Int)                                                                                  \
    X(172, NewObjectArray, 3, Ptr)                                                                                  \
    X(173, GetObjectArrayElement, 2, Ptr)                                                                           \
    X(174, SetObjectArrayElement, 3, Void)                                                                          \
    X(175, NewBooleanArray, 1, Ptr)                                                                                 \
    X(176, NewByteArray, 1, Ptr)                                                                                    \
    X(177, NewCharArray, 1, Ptr)                                                                                    \
    X(178, NewShortArray, 1, Ptr)                                                                                   \
    X(179, NewIntArray, 1, Ptr)                                                                                     \
    X(180, NewLongArray, 1, Ptr)                                                                                    \
    X(181, NewFloatArray, 1, Ptr)                                                                                   \
    X(182, NewDoubleArray, 1, Ptr)                                                                                  \
    X(183, GetBooleanArrayElements, 2, Ptr)                                                                         \
    X(184, GetByteArrayElements, 2, Ptr)                                                                            \
    X(185, GetCharArrayElements, 2, Ptr)                                                                            \
    X(186, GetShortArrayElements, 2, Ptr)                                                                           \
    X(187, GetIntArrayElements, 2, Ptr)                                                                             \
    X(188, GetLongArrayElements, 2, Ptr)                                                                            \
    X(189, GetFloatArrayElements, 2, Ptr)                                                                           \
    X(190, GetDoubleArrayElements, 2, Ptr)                                                                          \
    X(191, ReleaseBooleanArrayElements, 3, Void)                                                                    \
    X(192, ReleaseByteArrayElements, 3, Void)                                                                       \
    X(193, ReleaseCharArrayElements, 3, Void)                                                                       \
    X(194, ReleaseShortArrayElements, 3, Void)                                                                      \
    X(195, ReleaseIntArrayElements, 3, Void)                                                                        \
    X(196, ReleaseLongArrayElements, 3, Void)                                                                       \
    X(197, ReleaseFloatArrayElements, 3, Void)                                                                      \
    X(198, ReleaseDoubleArrayElements, 3, Void)                                                                     \
    X(199, GetBooleanArrayRegion, 4, Void)                                                                          \
    X(200, GetByteArrayRegion, 4, Void)                                                                             \
    X(201, GetCharArrayRegion, 4, Void)                                                                             \
    X(202, GetShortArrayRegion, 4, Void)                                                                            \
    X(203, GetIntArrayRegion, 4, Void)                                                                              \
    X(204, GetLongArrayRegion, 4, Void)                                                                             \
    X(205, GetFloatArrayRegion, 4, Void)                                                                            \
    X(206, GetDoubleArrayRegion, 4, Void)                                                                           \
    X(207, SetBooleanArrayRegion, 4, Void)                                                                          \
    X(208, SetByteArrayRegion, 4, Void)                                                                             \
    X(209, SetCharArrayRegion, 4, Void)                                                                             \
    X(210, SetShortArrayRegion, 4, Void)                                                                            \
    X(211, SetIntArrayRegion, 4, Void)                                                                              \
    X(212, SetLongArrayRegion, 4, Void)                                                                             \
    X(213, SetFloatArrayRegion, 4, Void)                                                                            \
    X(214, SetDoubleArrayRegion, 4, Void)                                                                           \
    X(215, RegisterNatives, 3, Int)                                                                                 \
    X(216, UnregisterNatives, 1, Int)                                                                               \
    X(217, MonitorEnter, 1, Int)                                                                                    \
    X(218, MonitorExit, 1, Int)                                                                                     \
    X(219, GetJavaVM, 1, Int)                                                                                       \
    X(220, GetStringRegion, 4, Void)                                                                                \
    X(221, GetStringUTFRegion, 4, Void)                                                                             \
    X(222, GetPrimitiveArrayCritical, 2, Ptr)                                                                       \
    X(223, ReleasePrimitiveArrayCritical, 3, Void)                                                                  \
    X(224, GetStringCritical, 2, Ptr)                                                                               \
    X(225, ReleaseStringCritical, 2, Void)                                                                          \
    X(226, NewWeakGlobalRef, 1, Ptr)                                                                                \
    X(227, DeleteWeakGlobalRef, 1, Void)                                                                            \
    X(228, ExceptionCheck, 0, Int)                                                                                  \
    X(229, NewDirectByteBuffer, 2, Ptr)                                                                             \
    X(230, GetDirectBufferAddress, 1, Ptr)                                                                          \
    X(231, GetDirectBufferCapacity, 1, Int)                                                                         \
    X(232, GetObjectRefType, 1, Int)

const std::vector<FuncMeta>& funcTable()
{
    static const std::vector<FuncMeta> table = [] {
        std::vector<FuncMeta> v(kFuncCount);
#define JSIM_JNI_PLACE(id, name, argc, ret) v[id] = {#name, u8(argc), FuncMeta::Ret::ret};
        JSIM_JNI_FUNCTIONS(JSIM_JNI_PLACE)
#undef JSIM_JNI_PLACE
        return v;
    }();
    return table;
}

const FuncMeta* metaOf(u32 id)
{
    const std::vector<FuncMeta>& t = funcTable();
    if (id >= t.size() || t[id].name == nullptr) return nullptr;
    return &t[id];
}

u32 indexOf(std::string_view name)
{
    const std::vector<FuncMeta>& t = funcTable();
    for (usize i = 0; i < t.size(); ++i) {
        if (t[i].name != nullptr && name == t[i].name) return static_cast<u32>(i);
    }
    return kInvalidFunc;
}

#undef JSIM_JNI_FUNCTIONS

} // namespace jsim::jni
