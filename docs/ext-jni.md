# jsim-jni 本地方法扩展包

j2c 混淆器把 Java 方法编译成 native 函数,体内经 JNIEnv 函数表回调 JNI。jsim-jni 把这条执行链接进 libjsim:模拟字节码 invoke 到 native 方法时移交本机执行器(unicorn)跑机器码,机器码调 JNI 时转接到包内钩子,钩子桥接回 jsim Session(字符串、堆、静态字段)或交用户自定义实现,返回值编组回机器码继续跑。对应设计稿 §7 挂账的「本地方法接管的接缝与常用 mock 骨架」,实现 issue #4。

生态先例(c2j-native-deobfuscator 的 emulation-recovery 路径)验证过这条路线:mock JNI 环境加 CPU 模拟器是 j2c 家族的通用打法,锚点是冻结的 JNI ABI,而不是任何一个混淆器的代码形态。

## 依赖声明与零依赖边界

- `jsim-jni`(ext/jsim/jni.hpp)是**纯层**,零外部依赖:JNI 函数表常量、钩子分发、句柄池、参数编组、默认骨架、install 接缝。测试用 FakeMachine 驱动,不需要 unicorn。
- `jsim-jni-uc`(ext/jsim/jni_uc.hpp)是**绑定层**,依赖外部包 unicorn(2.1.4,经官方 xmake-repo)与纯层:unicorn 引擎持有、JNIEnv 表与 trap page 物化、x86-64 ABI(Win64 + SysV)寄存器编组、步数熔断。
- 依赖纪律的例外记录:设计稿 §9 的「扩展包互不依赖」指语义包之间;绑定层依赖语义面(jni-uc → jni)与外部机器(uc)是位面分层,不是语义堆叠。jsim-jni 是第二个带外部依赖的包位面(第一个是 jsim-jbc)。
- 核心零依赖红线不动:src/jsim 纯 std 头文件,对两个包零反向依赖。

## 执行链路

(1) 消费方把 native 方法登记进 `JniEnv::methods`(键 = owner.name+desc,同 callMocks 键形),值是 `NativeSpec{entry, abi}`。登记来源可以是手工填表,也可以是模拟 JNI_OnLoad 时 `RegisterNatives` 骨架自动录表。包不做二进制加载(PE/ELF 映射、导出解析),消费方给映射好的内存布局与入口地址——真实样本的加载属 profile/消费方层。

(2) `install(session, env)` 向 `session.callMockResolvers` 兜底解析链挂一个解析器,只认领 methods 里登记的方法。用户 callMocks 注册优先级天然更高,可以强行 mock 掉某个 native 不进机器。

(3) 认领后的 CallMock 按 callee 的 desc 与 isStatic 从 Frame 弹参,SimValue 编组成 u64 位型(引用发句柄),交给 `env.executor.run`。执行器由绑定层提供(unicorn);测试由 FakeMachine 提供。

(4) 机器码读 `JNIEnv*` 调 JNI 函数。JNI 的 env 是**双重间接**:`env` 指向一个 JNIEnv 结构,结构的第一个成员才是函数指针表(`(*env)->Fn` 即 `*(*(env))` 再取成员)。因此物化两页:`[envTable] = funcTable`(env 结构只装一个指针),`funcTable[i] = trapPage + i*8`。机器码按表内某项跳到 trap page 独立地址;unicorn 的 UC_HOOK_CODE 在 trap page 上截获,按 `(addr - trapBase) / 8` 还原函数号,进 `dispatch`。这条双重间接曾被按「env 即表」的单重间接约定实现,demo 与样本 DLL 两边一致地错,最终由真机对拍腿(HotSpot 跑同一份 DLL)抓出——JNI ABI 的每个约定都必须对真机验证,两套模拟互证不构成证据。

(5) dispatch 按函数号查钩子:用户钩子(`JniEnv::hooks`,键 = 函数名)优先于默认骨架;都缺则 Bail{UnsupportedCall} 带函数名。钩子返回 u64 位型写回返回寄存器,PC 折到返回地址继续跑。

(6) native 停机,执行器取返回位型,CallMock 按 callee.retType 解码回 SimValue 压栈;local 句柄回收;pendingThrow 置位则整体 Bail。

## API 面(纯层)

| 符号 | 职责 |
|---|---|
| `kFuncCount` / `metaOf(id)` / `indexOf(name)` | JDK 8 冻结布局 233 项(索引 0..3 保留);`FuncMeta{name, argc, ret}` 供分发与返回编组 |
| `Layout` | guest 地址约定:envTable(env 结构页)/ funcTable(函数指针表页)/ trapPage / scratch / handles / stack / sentinel,消费方可整体改 |
| `GuestMemory` | 纯层唯一面向机器的口子:map / read / write 三个函数槽,FakeMachine 与 unicorn 各给一份实现 |
| `NativeSpec` / `NativeRun` | 方法登记(entry + abi)与一次编组好的调用(retType + 位型实参序列) |
| `JniEnv` | 会话 JNI 面:session 指针、methods 表、hooks 用户钩子表、javaInvoker 回跳、GuestMemory、句柄池、pendingThrow、trace、maxSteps |
| `HandlePool` | jobject/jclass/jstring/jmethodID/jfieldID 形状的假 guest 指针 ↔ SimValue / 成员身份;local/global 两域 |
| `argClassesOf(env, id, midHandle)` | 一次调用的参数类布局(C 位置是否浮点类):定长函数全整,bare Call\*Method / NewObject 按 jmethodID 的 desc 展开。绑定层据此把「位置+类」映到寄存器文件 |
| `dispatch(env, id, argI, argF)` | 一次 JNI 调用的分发入口:查钩子、发观测、返回 guest 位型。argI/argF 由执行器按 ABI 实现并各自闭包参数类布局,绑定层与 FakeMachine 共用 |
| `install(session, env)` | 挂 callMockResolvers 兜底解析 + 注册公共骨架。幂等:骨架只补缺,不覆盖用户钩子 |
| `argTypesOfDesc(desc)` | 描述符 → 参数类型序列(编组与位置布局共用) |

## 口径

**句柄池**:句柄是 8 对齐单调发号的假 guest 指针,不对应任何真实内存,绑定层不为它映射页。ref 句柄背 SimValue(Null/Str/Obj 载荷),memberID 句柄背 (owner, name, desc)。local 句柄一次 native 调用结束统一回收,global 句柄(NewGlobalRef)跨调用持久——缓存 jclass 的解密器依赖这一点。悬空句柄一律 Bail{BadOperand}。

**内存物化**:GetStringUTFChars / GetStringChars / Get*ArrayElements / Get*Critical 在 scratch 区物化一份副本,返回 guest 指针,Release* 系回收;写回语义(Set*ArrayElements 带 isCopy=false 时)按副本写回堆。Get*Region / Set*Region 系不经副本直通 Session 堆。字符串字节按家族口径 MUTF-8 原样透传(GetStringUTFChars 不转码);GetStringChars / GetStringLength 按 UTF-16 码元解码(BMP 假设,同 jsim-str 家族口径)。

**字段与类桥接**:GetStaticXField / SetStaticXField 直连 `Session.statics`,GetXField / SetXField 直连 `HeapObj.fields`,与 jsim 字段面同键同口径——native 侧和字节码侧读写同一份状态。FindClass / GetObjectClass 走 `session.classObjectOf`,与 ldc 类常量同一个对象。

**Call\*Method 家族**:jmethodID 载荷带 (owner, name, desc)。默认 handler 把实参按 desc 解开,交 `JniEnv::javaInvoker` 回跳——消费方在那里开嵌套 jsim::Sim 跑被调方法(核心 D9 不破:核心仍单方法,编排属消费方);javaInvoker 未设则 Bail{UnsupportedCall}。A 变体读 guest 内存 jvalue 数组;V 变体 Win64 按 char* 直读、SysV 的 va_list 结构 v1 不做;裸变体按 ABI 位置读寄存器与栈。异常回跳例外:被调方法若 Bail,上抛同口径。

**异常面**:pendingThrow 位是 JniEnv 状态。ThrowNew / Throw 置位并返回 0;ExceptionCheck / ExceptionOccurred 反映它;置位后 dispatch 拒绝后续 JNI 调用(JNI 契约是钩子立即返回,这里做防御)。native 停机若 pendingThrow 仍在,CallMock 返回 Bail——与 athrow v1 不做同一条纪律:异常派发不进模型,宁可断链不静默走错。

**观测与熔断**:`env.trace` 每次 JNI 调用记一行(函数名、参数展示、返回值展示),与 jsim 观测面同责(只读)。native 侧步数上限 `JniEnv::maxSteps` 由绑定层经 uc_emu_start 的步数参数消费,超限 Bail{NotConcrete},防混淆器埋死循环。

**IsInstanceOf 无类型层次**:同句柄或同 objId 判真,其余判假并打 trace 警告。类型层次不进模型是既定口径,这里宁近似不断链。

## 默认骨架覆盖面

v1 内建约 40 个函数,按解密场景的调用频度选:版本与类(GetVersion / FindClass / GetObjectClass / GetSuperclass / IsAssignableFrom)、字符串 12 件套(NewString / GetStringFamily / NewStringUTF / GetStringUTFChars / Release / Length / Region / Critical)、引用 6 件套(GlobalRef / LocalRef / Delete / IsSameObject)、异常 4 件套(ThrowNew / Throw / Occurred / Check / Clear)、成员 ID 4 件套(GetMethodID / GetStaticMethodID / GetFieldID / GetStaticFieldID)、字段读写 16 形(实例与静态 × Object/Int/Long/Boolean 等常用形)、数组 10 件套(NewXArray / GetArrayLength / Get/SetXArrayRegion / Elements)、调用面(Call\*Method 三变体)、RegisterNatives(录表)、杂项(MonitorEnter/Exit no-op、GetJavaVM 假句柄、PushLocalFrame/PopLocalFrame、EnsureLocalCapacity)。

未覆盖的函数号 stub 存在但钩子缺,调到即 Bail{UnsupportedCall} 带函数名——补什么由用户钩子决定,框架不编造语义。

## 消费方最小接管点

```cpp
jsim::Session session;
jsim::jni::JniEnv env;
env.session = &session;
env.mem = uc.memFace();                       // 绑定层给的 GuestMemory 实现
env.methods["sample/Decrypt.decrypt(Ljava/lang/String;)Ljava/lang/String;"] = {entry, abi};
env.javaInvoker = [](const jsim::CallRef& c, std::vector<jsim::SimValue> args)
                  -> jsim::Result<jsim::SimValue, jsim::Bail> { /* 嵌套 Sim */ };
jsim::jni::install(session, env);             // 骨架 + 兜底解析

// 针对特定 JNI 函数给用户实现:钩子键 = 函数名
env.hooks["GetStringUTFChars"] = [](jsim::jni::JniCall& call) -> jsim::Result<u64, jsim::Bail> {
    // 自定义语义;返回 guest 位型
};

auto m = jbcbridge::toSimMethod(*jbcSession, decryptIdx);   // jsim-jbc 桥出字节码
jsim::Prefill p;
p.locals = {jsim::SimValue::internedStr(cipher)};
jsim::RunResult r = jsim::Sim(m.value(), session).runEntry(std::move(p), 1u << 16);
```

默认骨架已装钩子的函数,用户钩子整体替换,不做参数级组合——语义替换是接管面既定形状。

## 测试

- `jsim-jni-test`(独立 target):FakeMachine 驱动纯层。FakeMachine 是一段预编脚本(假 native 依次发起的 JNI 调用 + 最终返回值),驱动 dispatch 与编组,覆盖:骨架逐条、句柄池生命周期、异常位、参数编组对拍、用户钩子覆盖优先级、未实现函数 Bail。期望值由测试用同算法独立复算,同 jsim-jbc-test 纪律。
- `jsim-demo-jni`(端到端):手写 x64 Win64 机器码的 native 方法(GetStringUTFChars → XOR 循环 → NewStringUTF),jsim 字节码 invokestatic 发起,走完整链还原明文;兼作绑定层的真机验证。期望值 C++ 独立复算。
- `jsim-jni-e2e-test`(真实样本端到端):语料 jar `test/corpus/jni/jni-sample.jar` 是**构建产物,不入库**,内含真实 MSVC 编译的 `decrypt.dll`(样本源在 `test/jni/sample/`,自包含 C 源 `decrypt_jni.c`);jar 缺失时整条 E2E 跳过并提示再生成命令。测试读 jar 条目(libjzip)→ 按字节偏移解析 PE64、在首选基址展开映像、走导出表定位 `Java_sample_Decrypt_decrypt`(消费方装载职责的参考实现)→ jsim-jbc 桥出 `Caller.run` 字节码 → invokestatic → unicorn 跑真机器码 → 明文还原。**真机对拍腿**:同 jar 同 DLL 交给本机 HotSpot 跑一遍,两边输出必须一致——这条腿曾抓出 env 双重间接约定错位,是纯模拟自证不能替代的证据(本机无 JDK 时该腿单独跳过)。
- 样本再生成:`test/jni/sample/build-sample.ps1`(vswhere 定位 MSVC,javac --release 8,cl/link 产出非 FIXED 基址 DLL,打 jar);也可 `gradle sampleJar`(build.gradle,CI 环境)。产出的 jar 覆盖 `test/corpus/jni/jni-sample.jar`。
- 核心回归:jsim-test 90 项 / jsim-jbc-test 108 项既有断言不破(本轮为 jsim-jni 在 engine.hpp 加了 Result&lt;void,E&gt; 特化,已回归)。

**JNI C 字符串口径**:GetStringUTFChars 契约是 NUL 终止的 C 字符串——实参字符串里首个 NUL 之后的内容不可往返(NewStringUTF 重建即截断),HotSpot 真机同口径。涉及含 0x00 字节的密文样本时,期望值须按首个 NUL 截断后比对,这是 ABI 事实而非缺陷。

