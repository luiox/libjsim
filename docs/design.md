# libjsim 设计稿

状态：设计草案 v0.3（2026-10-29）。v0.1 的三处结构性修正：默认 handler 只算类型不估值、核心只跑一个方法、语义注入全部下放扩展包。v0.3 落地扩展面缺口（字段接管、分支接管收帧、结构化观测与断点、栈平衡开关）并收紧三处口径（Str 身份位、multianewarray 全维分配、num 解析越界），见 §12。

## 1. 定位

libjsim 是 JVM 字节码的单方法模拟执行器。它不追求执行完备，追求框架完备：用户在任何需要的位置都能接管语义，缺什么补什么，补到够用即完备。

同类的两个先行者都死于执行完备路线。javavm-master（samczsun，java-deobfuscator 系）真跑整个 JDK 8，为复刻 HotSpot 维护了大量 native 移植，全部钉死在 JDK 8 内部符号上，JDK 9 即死。SSVM（xxDark，Recaf v3 引擎）opcode 全覆盖，但语义契约是逐位复刻某个具体 HotSpot 版本：boot 类取自宿主 classloader，数十个 Natives 类复刻 JDK native，字符串布局按 JDK 版本分叉。两者的共同点：opcode 之外的部分永远做不完，维护成本先于功能耗尽维护者。unicorn 生态位相反：它自己不实现指令语义，产品是启动接口加 hook 挡板，架构覆盖面与需求无关地稳定。

libjsim 的完备性由此定义为五个面：

| 完备面 | 内容 |
|---|---|
| opcode 面 | 202 条每条有默认 handler，每条可替换 |
| 接管面 | 调用、字段、分支、分配、指令前后、步进，全部带注册期过滤器 |
| 启动面 | 任意指令起步、预填栈与局部变量、栈平衡检查可关 |
| 失败面 | 语义缺失或条件不满足时结构化 Bail，不崩溃、不静默走错 |
| 观测面 | trace、断点、帧快照、Bail 现场 |

## 2. 拍板记录

| # | 决定 | 状态 |
|---|---|---|
| D1 | SimValue 三要素：类型、是否已知、值。类型恒在，Unknown 时值不存在 | 已定 |
| D2 | 成因、溯源、诊断不进框架不绑 value，用户在接管面自行记录 | 已定 |
| D3 | null 是 Known(Ref, null) | 已定 |
| D4 | 栈平衡检查可关 | 已定 |
| D5 | 引用身份键为 allocId 单调序号 | 采纳默认建议 |
| D6 | 运行结果三态 Completed / Bailed / LimitHit；分支接管三态 Taken / NotTaken / Bail | 采纳默认建议 |
| D7 | 不 fork：分支条件 Unknown 时不双路探索 | 采纳默认建议 |
| D8 | 默认 handler 只保证类型传播：操作数全 Known 才算值，否则压入正确类型的 Unknown 继续 | 已定 |
| D9 | 核心只跑一个方法，不跨方法递归；多方法由调用方开多个 sim 编排 | 已定 |
| D10 | 语义注入（JDK 内建、堆脚手架、indy、JNI）全部为扩展包；核心只有接管面 | 已定 |

## 3. SimValue 代数

### 3.1 形状

SimValue 是封闭 tagged union，三个成员：类型、是否已知、值。

```
Type       = I32 | I64 | F32 | F64 | Ref
Payload    = i64 | f64 | RefPayload        // 仅 Known 时存在
RefPayload = Null | Str(Utf8String, interned) | Obj(allocId)
```

类型是槽位的静态事实，决定 cat1 / cat2 与槽位形状；值是动态事实。Unknown(I32) 仍是 I32，只是没有值。null 是已知值不是未知。字符串载荷内联进 value（Utf8String）；对象载荷只背 allocId，内容在会话堆。Str 的 interned 是身份事实（不是诊断，不违反 D2）：ldc 字面量为真——常量池 intern 保证同内容字面量同一对象，引用相等可判；动态构造的字符串（StringBuilder.toString 产物等）为假——真 JVM 里是新对象，模型没有第二身份键，身份不可判，`if_acmpeq` 走分支接管三态而不是按内容判同。

### 3.2 运算规则

(1) Unknown 是吸收元：任一操作数 Unknown，结果为 Unknown(结果类型)。传染只需要这一条，不需要成因。

(2) 操作数全 Known 时按 JVM 规范计算，结果带正确类型写回。算术、转换、移位掩码、比较的 NaN 分支语义依规范实现。

(3) 浮点语义单点化，以 mjbase 常量折叠器为基准实现，全库不出现第二种浮点口径。

(4) 除数含 0 或 Unknown 的整除取余：结果 Unknown 并发观测事件，不猜值、不绕过检查。

(5) 所有恒等式（槽位重排、转换往返）不凭推导上线，按第 9 节以真机对拍为准。

### 3.3 不绑上 value 的东西

产生点、调用链、分支条件、def-use 链、类型层次、字符串来源、任何诊断与成因。判断标准：value 的形状必须在第一天定稿，之后加任何观测或诊断能力都不得改动它。存量里 sjvm 的 JavaUnknown 携带持续膨胀的成因枚举、morpher 的产点值携带指令集合与拷贝链，都是反例。

## 4. 类型传播：默认 handler 契约

框架能保证的底线是类型流，不是值流。类型对每条指令都是静态可确定的，值不是。默认 handler 的职责由此限定：算类型，不估值。

核心持有一张静态栈效应表：每条 opcode 的弹出数、压入数、结果类型规则（算术族取操作数类型，转换族按规范映射，栈操作族只动槽位）。默认 handler 行为分两类：

(1) 值内指令（常量装载、算术、转换、栈操作、数组长度等）：操作数全 Known 时算值写回；否则弹出操作数、压入正确类型的 Unknown，继续执行。用户由此可以跑通「直线段 + 关心点」的片段，无关指令不阻断执行。

(2) 边界指令（invoke 系、new、indy、monitor、athrow）：核心不自带继续语义。先查接管注册，命中则由接管方给语义；未命中一律 Bail，原因码区分 UnsupportedCall / UnsupportedAlloc 等。字段访问单列（v0.3 定稿）：接管优先（§6 字段接管），未命中走会话存贮底座——静态字段 Session.statics、实例字段 HeapObj.fields，缺项按字段类型传播 Unknown（字段没被赋值过是常态，不是失败）；Known null 接收者的 getfield/putfield 按 NPE 口径 Bail，与整数除零同一纪律。边界指令的语义只能来自注入，框架绝不编造。

这条契约的推论：核心不存在「尚未实现的内建」这种状态。语义要么被注入，要么 Bail，不存在第三种半成品状态。

## 5. 单方法核心

一个 sim 实例绑定一个方法。构造参数：方法体（libjbc CodeUnit）、入口 pc、预填帧（栈与局部变量任意槽位，Known 或 Unknown）、限制配置（步数上限、栈平衡开关）。

引擎只做五件事：按 pc 取指令、查 handler、维护槽位形状与 cat2 记账、推进或跳转、熔断与 Bail。不做 verifier 式数据流合并；栈平衡检查开启时（Limits.checkBalance）只做合并点的深度与类型一致性校验——同 pc 二次到达栈形不一致即 Bail{StackBroken}，关闭时引擎信任预填。两个便利入口已落地：runEntry 方法入口起步（栈平衡默认开）与 runRegion 区间起步（栈平衡默认关，容忍预填栈与域外出口——越过方法尾按 Completed(void) 收尾）。两者共用同一引擎，只是预填与开关的预设。

跨方法一律不递归。调用点经接管面处理：扩展包的 mock、用户注册的接管函数、或 Bail。解密链跨多个方法时，调用方在 profile 层编排：取实参、跑被调方法的 sim、把结果作为接管返回值喂回调用方 sim。这个限制换来的是核心没有调用栈、没有递归深度问题、没有类层次解析、没有 clinit 自动链，实例轻到可以按调用点批量构造。

便利入口按用途分两个封装：方法入口起步（栈平衡默认开）与任意点区间起步（栈平衡默认关，容忍预填栈与域外出口）。两者共用同一引擎，只是预填与开关的预设（v0.3 已实现：runEntry / runRegion）。

## 6. 接管面

接管面是核心唯一的扩展机制，扩展包与用户代码走同一套注册接口，核心没有特权内建。

| 注册项 | 触发 | 语义 | 过滤器 |
|---|---|---|---|
| opcode handler | 指令派发（**先于**效应表） | 替换该指令语义，收完整进入帧 | opcode 槽位 |
| 调用接管 | invoke 解析接管后 | 返回 Known 值、写堆、或 Bail | owner+name+desc，按调用点注册优先于按签名 |
| 调用兜底解析 | 静态键都查不到时 | 按**接收者对象**现场决定语义，或返回 nullptr 放行 | 收完整帧，可窥视接收者后认领/不认领；解析链按注册序逐个问，单槽不独占 |
| 字段接管 | getstatic/getfield/putstatic/putfield | 注入值或记录；get 系返回值按描述符定型压栈，put 系自行弹栈 | owner+name+desc，命中优先于存贮底座 |
| 分支接管 | 条件不具体时（值 Unknown 或引用身份不可判） | 三态裁决 Taken / NotTaken / Bail；收完整进入帧，操作数由引擎裁决后统一弹出 | pc |
| 堆形态 | new | 注册 allocId 背后的初始载荷 | 类名 |
| 观测 | 每步 / Bail | stepObservers（指令前结构化只读视图）、trace（步后文本）、bailObserver（Bail 现场）、RunResult 终止帧快照、Limits.stopAtPc 断点熔断（LimitHit 语义，快照灌回即续跑） | 全量或观测方自过滤 |

handler 与接管的签名统一收完整进入帧：栈、局部变量、pc、方法体只读视图、会话引用。引擎不做预弹栈，取数由接管方经受检栈操作完成。这条约束来自 mjbase 的教训：只给产点不给栈的钩子，会迫使用户做两趟探测注入。

**兜底解析**是为「方法名就是数据」的调用面留的口子：注解元素访问器（`d1()`）、getter 家族、反射代理，语义由接收者决定，静态签名键永远匹配不全。解析方拿到帧后自己窥视接收者，认领才返回 mock，不认领返回 nullptr 交回核心 Bail——不允许把「缺环境」静默吞成 Unknown。

## 7. 扩展包

语义注入全部外置为扩展包。扩展包是独立构建目标，只依赖核心公开头，核心零反向依赖。用户按需链接，不需要的包不进二进制。

| 包 | 内容 |
|---|---|
| jsim-str | String 家族与 StringBuilder/StringBuffer 的调用接管，char[] 数组行为，字符串构造两阶段 |
| jsim-num | Integer/Long 的装箱拆箱与十进制解析（解析失败明确 Bail，不编造 0） |
| jsim-reflect | Class.forName / getAnnotation / 注解元素访问器；元数据走 provider 注入，包内不解析 class 文件 |
| jsim-math | Math 系与浮点辅助函数的调用接管 |
| jsim-random | java/util/Random 的 48 位 LCG 复刻 |
| jsim-stacktrace | getStackTrace 假栈帧构造 |
| jsim-indy | invokedynamic 的 bootstrap 脚手架，供用户组合特定调用点形态 |
| jsim-jni | 本地方法接管的接缝与常用 mock 骨架 |

包内每条接管带独立测试。包之间不互相依赖，组合发生在用户侧。框架不内置任何混淆器知识；jsim-stacktrace 虽然因解密场景而设，但内容是 Thread.getStackTrace 的 JDK 语义，归入 JDK 面扩展包。

`jsim-reflect` 的边界值得单说：它**不解析 class 文件**。`ClassMeta`（类名 + 注解 + 元素值）由 provider 灌进来，libjbc 桥、JSON 表、手写脚本都从同一个口进。这样反射面既不依赖容器格式，也不把「怎么读注解」写死成一种实现。

## 8. profile 层

profile 是用户仓对特定混淆器的知识：识别启发式、编排顺序、判稳标准。框架不含 profile。

allatori 字符串解密 profile 的骨架：识别 `(Ljava/lang/String;)Ljava/lang/String;` 且静态且体内调 String.length 的方法为解密器；扫描 `ldc` 后紧邻 `invokestatic` 的站点；对每个站点开 sim，密文灌入 locals，strong 形态挂 jsim-stacktrace；返回 Known 字符串则原位改写为 ldc 明文并擦除调用，Bail 则保留原样。改写按不动点逐点推进，一处 Bail 不影响其他站点。

dasho profile 的骨架：调用点形态多样，不做形态匹配，直接对候选区间开区间模式 sim；解密器掺随机时以多 seed 结果一致为判稳条件，配合明文合法性校验（拒绝 NUL、孤立代理项、C0/C1 控制字符）。

## 9. 测试纪律

(1) 真机对拍：算术与槽位重排恒等式以 HotSpot 实测结果为基准，不凭推导。对拍脚手架进共享测试层。

(2) 语料：复用 mjt 仓已 vendor 的字节码语料；扩展包每条接管一测。

(3) 判稳件：多 seed 一致性与明文合法性校验做成库内断言件，profile 直接调用。

(4) 构建：delete-then-rebuild 门，测试产物进项目 test 目录。xmake 按包独立 target（jsim / jsim-str / jsim-num / jsim-reflect / jsim-indy / jsim-demo / jsim-test），扩展包互不依赖。

核心零外部依赖（纯 std 头文件）。jbc 会话面（CodeUnit → jsim::Method）与
MUTF-8 字符串面属边界胶水，由消费方仓自备（桥不在本仓）；libjstr 仅扩展包
（ext/jsim）可选消费。

## 10. 仓库骨架

```
libjsim/
  xmake.lua
  docs/design.md
  src/jsim/        // 核心：value、session（堆+allocId+限制）、frame、
                   //       handler（含栈效应表）、接管面、engine、bail、trace
  ext/jsim/        // 扩展包：str、num、reflect、math、random、stacktrace、indy、jni
  demo/            // 逐场景可执行样例（类型传播 / 接管面 / 区间 / 解密 / 反射）
  test/            // 值代数、逐 handler、恒等式对拍、扩展包接管测试
```

命名空间 jsim。仓规沿用生态约定：`///` 行注释、禁 RTTI、i32/u32/usize、Result 值语义、Utf8String、约定式提交带作用域。

## 11. v1 不做

invokedynamic（Bail，语义由 jsim-indy 组合）、异常表派发（athrow 与区间模式本就绕开帧重算，v1 直接 Bail）、多线程与 monitor（monitor 指令按类型传播为 Unknown）、类初始化链（字段值由预填或接管注入）、finalize 与 GC、JDK 9+ indy 字符串拼接（Bail）、跨方法递归（永久不做，见 D9）、浮点 strict/loose 模式差异（单点口径不分叉）。

## 12. 实现现状：核心补丁记录

设计稿与首版代码之间曾有四处偏离，已按 D8/D9 补齐：

**(1) 效应表从行表改 dense 索引。** 原来 `infoOf` 每步线性扫约 80 行，百万步就是热点。改成 256 槽一次性摊平，`infoOf` 退化成两次数组访问。

**(2) 指令接管面补上。** §6 承诺 opcode handler，首版代码里 `infoOf == nullptr` 直接 Bail，扩展包无路可走。现在主循环**先**查 `opcodeHandlers` 再查效应表——表外 opcode 也能接管，「缺什么补什么」不必等核心补表。

**(3) 默认 handler 从 Bail 改为类型传播。** 这是 D8 的原文，但首版 `executeValue` 的 `default:` 是 Bail。现在改成「弹掉该弹的槽、压出正确类型的 Unknown」：值算不出来是常态，不是失败；真正需要具体值的地方（分支、switch 键、数组下标）各自有具体性检查兜底。用户没补环境的表现，是一路 Unknown 走到分支处，而不是第一条没见过的指令就断链。

**(4) 栈指令的 cat2 形态。** `pop2`/`dup2`/`dup_x2`/`dup2_x1`/`dup2_x2` 在 JVMS §6.5 里各有 2~4 种形，首版只做了全 cat1 那一形，`pop2` 还错发成 `pop`。现在按形分流：本模型一个值一个条目，cat2 只影响「弹几个条目」。

同时补齐了：`dup_x2 = 0x5b` 常量错（首版误作 0x5d，那是 `dup2_x1`）、opcode 常量补全到 202 条、约 190 条效应表、`ldc` 的 class 常量与 `Session::classObjectOf`（`ldc` 类常量与 `Class.forName` 必须同一对象，否则 `if_acmpeq` 假性失败）、数组面按元素宽度与符号性建模（`[B` 有符号、`[Z`/`[C` 无符号）、实例字段面（`HeapObj.fields`）、`getfield`/`putfield`、浮点与饱和转换、整数除零明确 Bail（需异常派发，不编造商）。

### v0.3（2026-10-29）：扩展面补齐与口径收紧

架构审查结论：框架没有向执行完备漂移（核心仍单方法、零 JDK 语义、扩展包克制），偏离集中在**扩展面本身没做完**。本版按 §6/§5 的原文兑现欠账，并收紧三处顺嘴实现的口径。

**(5) 字段接管落地。** §6 表中承诺的字段接管（owner+name+desc 过滤）首版无实现面，用户只能整条替换 opcode 且必须自己重写弹压语义。现在 `Session.fieldMocks` 落地：get 系返回值由引擎按描述符定型压栈，put 系由接管方自行弹栈；命中优先于存贮底座。§4 原文把字段访问归入「未命中一律 Bail」的边界清单并点名 FieldAbsent 原因码，与 §12 已挂账的「缺项即 Unknown」互相矛盾——v0.3 定稿为后者（缺项是常态不是失败），§4 已同步改写；getfield/putfield 的 Known null 接收者按 NPE 口径 Bail{BadOperand}，与整数除零同一纪律。

**(6) 分支接管收完整帧。** 首版 `executeBranch` 先弹操作数再查接管，`BranchHook` 只收 pc——违反 §6 红线（「引擎不做预弹栈」，正是 mjbase 的教训原文），裁决方既看不到条件操作数、操作数也已被丢弃。现改为窥视判定、裁决后统一弹出，签名收 Frame。

**(7) 观测面结构化。** 首版只有单槽文本 trace（toDisplay 渲染串，拿不到 SimValue）。现在补齐：`stepObservers`（每条指令执行前的结构化只读视图：栈、局部变量、方法体、会话）、`bailObserver`（Bail 现场回调）、`RunResult.finalStack/finalLocals`（三种终态都带的终止帧快照）、`Limits.stopAtPc`（断点熔断，LimitHit 语义，快照灌回 Prefill 即续跑）。

**(8) 启动面补齐。** D4 的「栈平衡检查可关」首版是「不存在」而非「可关」。现在 `Limits.checkBalance` 落地（同 pc 二次到达深度或类型序列不一致即 Bail{StackBroken}，向量按 pc 索引，不做数据流合并）；两个便利入口 `runEntry`（默认开）/`runRegion`（默认关、tolerateOffEnd 容忍域外出口）成对落地。

**(9) Str 身份位。** 首版 `sameRef` 对所有 Str 载荷按内容判等，把 `sb.toString() == 字面量` 假性折真（真 JVM 里 toString 产物是新对象，恒 false）——正是失败面「不静默走错」要防的事。现在 `RefPayload.Str` 带 interned 位：ldc 字面量按常量池 intern 事实可判同；动态构造字符串身份不可判，`if_acmpeq` 走分支接管三态（compareRef 三态 Same/Diff/Unknown）。

**(10) multianewarray 全维分配。** 首版只建最外层、内层静默成 null，并引 D9（不跨方法递归）为据——数组嵌套是数据递归，不是方法递归，依据不成立。现在全部维度已知即按 JVMS 逐层建满；内层长度未知时最外层元素挂 Unknown(Ref)（内层确实存在，指向不可知，不猜 null）；已知负长度按 NegativeArraySizeException 口径 Bail。

**(11) jsim-num 解析越界。** `Integer.parseInt("2147483648")` 首版静默回绕（注释自称「按 JVM 口径回绕」——那是算术口径，解析口径在 JDK 是 NumberFormatException）。现在越界与位数溢出一律解析失败 Bail，对齐本包「不编造 0」的契约。

**(12) 工程面。** 测试层落地（test/，90 项断言：值代数、逐类 handler、五个完备面、扩展包每条接管一测）；xmake 按包独立 target（jsim-str/num/reflect/indy）；demo 移除未消费的 libjstr 依赖；MethodBuilder 补 if_acmpeq 族与 multianewarray 助手。

## 附录 A：存量证据

证据按文件与符号引用，行号从略。

### A.1 javavm-master（samczsun，java-deobfuscator 系，2018 年起停止）

`VirtualMachine.java` 的 `registerNatives` 硬编码大量 JDK 内部符号 hook；handler 表与内联 switch 双轨并存，迁移半途丢失 l2d / f2l / d2f / goto_w / jsr_w 的实现；`JavaUnknown.merge` 以字符串拼接合并；分支对 Unknown 双路递归拷贝栈，以访问计数上限返回毒值；`execute` 只能从方法首条指令起步，栈在内部构造不可预填；无字节码校验。

### A.2 SSVM（xxDark，Recaf v3 引擎，2024 年起停止）

`RuntimeBootClassFinder` 以宿主 boot classloader 取被解释字节码；`InjectedClassLayout` 向宿主类注入 HotSpot 内部字段；`DefaultStringOperations` 按 JDK 版本分叉字符串私有布局；Natives 包按 JDK native 逐条复刻，setInvoker 注册数百处；`Value` 无已知未知之分；执行前必须完成整个 bootstrap，无片段执行与快照。

### A.3 mjbase / libjbc

libjbc `analysis/domain.hpp` 的 DomainEngine 与 IValueDomain 已验证引擎持形状、值域供钩子的分离，含 visitCap 熔断；mjbase `const_domain.hpp` 的 ConstLattice 是 SimValue 的最小可行实例。两处明文挂账：产点钩子看不到操作数栈（alias_analysis 头注）、无分支条件钩子（branch_facts 与 path_conditions 双轨）。字符串载荷口径三套并存、浮点政策三处不一、除零三种处置，均未单点化。

### A.4 morpher legacy（Java）

`morpher-sjvm` 为 javavm-master 的仓内 fork，曾尝试以 UnknownCause 枚举与 MaybeBoolean 三态续命。`MethodExecutor` 为单 switch 巨石，四个 invoke 分支复制粘贴，无步数上限，异常吞并后继续。ASM Interpreter 族有八个以上互不共享的常量折叠器。`simulator/` 与 `simulator/old/` 为两代未完成的统一模拟器遗迹。可回收资产：Provider SPI 的 can/do 双阶段与组合器、调用点 mock 优先于签名 mock 的优先级设计、三层 hook 粒度经验。

### A.5 analysis-* 四仓（C++，2026-10 建仓）

allatori 在 string_decrypt_pass 内嵌约千行解释器，值模型无 Unknown，遇不支持指令毒化整个解密器。dasho 的 jvm_interp 是最接近本设计的存量：Unknown 值、结构化失败枚举、方法与区间双入口、调用内建与纯度判定共表、StringBuilder 与 Random 堆脚手架；缺陷是 Unknown 以 0 参与运算绕过除零检查、INSTANCEOF 恒返 0。grunt 直接委托 mjbase evaluator，仓内仅薄适配层，是「有框架时仓的形态」的实证；但 evaluator 自述无 Unknown、无调用、无堆、无字符串，目标一旦上字符串加密即零覆盖。三仓重复约四千行：四套值模型、三套主循环、两份逐字相同的 MUTF-8 编解码、三个互不一致的熔断常数。skidonion 为 native 容器仓，与本题无关。

### A.6 AIR / SIR（morpher 仓 mjt-deobf）

L3 归一化 IR 与 L4 SSA，接 libjbc 会话面，SSA 回写栈机的真实 lower 未实现。与本设计正交：AIR/SIR 是 IR 层，libjsim 是求值层。潜在接合点是 SIR 侧常量折叠需要字符串或调用语义时，以 libjsim 为求值后端。
