# libjsim 设计稿

状态：设计草案 v0.2（2026-10-28）。v0.1 的三处结构性修正：默认 handler 只算类型不估值、核心只跑一个方法、语义注入全部下放扩展包。

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
RefPayload = Null | Str(Utf8String) | Obj(allocId)
```

类型是槽位的静态事实，决定 cat1 / cat2 与槽位形状；值是动态事实。Unknown(I32) 仍是 I32，只是没有值。null 是已知值不是未知。字符串载荷内联进 value（Utf8String）；对象载荷只背 allocId，内容在会话堆。

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

(2) 边界指令（invoke 系、new、字段访问、indy、monitor、athrow）：核心不自带继续语义。先查接管注册，命中则由接管方给语义；未命中一律 Bail，原因码区分 UnsupportedCall / UnsupportedAlloc / FieldAbsent 等。边界指令的语义只能来自注入，框架绝不编造。

这条契约的推论：核心不存在「尚未实现的内建」这种状态。语义要么被注入，要么 Bail，不存在第三种半成品状态。

## 5. 单方法核心

一个 sim 实例绑定一个方法。构造参数：方法体（libjbc CodeUnit）、入口 pc、预填帧（栈与局部变量任意槽位，Known 或 Unknown）、限制配置（步数上限、栈平衡开关）。

引擎只做五件事：按 pc 取指令、查 handler、维护槽位形状与 cat2 记账、推进或跳转、熔断与 Bail。不做 verifier 式数据流合并；栈平衡检查开启时只做合并点的深度与类型一致性校验，关闭时引擎信任预填。

跨方法一律不递归。调用点经接管面处理：扩展包的 mock、用户注册的接管函数、或 Bail。解密链跨多个方法时，调用方在 profile 层编排：取实参、跑被调方法的 sim、把结果作为接管返回值喂回调用方 sim。这个限制换来的是核心没有调用栈、没有递归深度问题、没有类层次解析、没有 clinit 自动链，实例轻到可以按调用点批量构造。

便利入口按用途分两个封装：方法入口起步（栈平衡默认开）与任意点区间起步（栈平衡默认关，容忍预填栈与域外出口）。两者共用同一引擎，只是预填与开关的预设。

## 6. 接管面

接管面是核心唯一的扩展机制，扩展包与用户代码走同一套注册接口，核心没有特权内建。

| 注册项 | 触发 | 语义 | 过滤器 |
|---|---|---|---|
| opcode handler | 指令派发 | 替换该指令语义，收完整进入帧 | opcode 槽位 |
| 调用接管 | invoke 解析接管后 | 返回 Known 值、写堆、或 Bail | owner+name+desc，按调用点注册优先于按签名 |
| 字段接管 | getfield/getstatic/put 系 | 注入值或记录 | owner+name+desc |
| 分支接管 | 条件求值 Unknown 时 | 三态裁决 Taken / NotTaken / Bail | opcode、pc |
| 堆形态 | new / newarray | 注册 allocId 背后的对象变体与行为 | 类名 |
| 观测 | 每步 / Bail | trace、断点、帧快照 | 全量或过滤 |

handler 与接管的签名统一收完整进入帧：栈、局部变量、pc、方法体只读视图、会话引用。引擎不做预弹栈，取数由接管方经受检栈操作完成。这条约束来自 mjbase 的教训：只给产点不给栈的钩子，会迫使用户做两趟探测注入。

## 7. 扩展包

语义注入全部外置为扩展包。扩展包是独立构建目标，只依赖核心公开头，核心零反向依赖。用户按需链接，不需要的包不进二进制。

| 包 | 内容 |
|---|---|
| jsim-str | String 家族与 StringBuilder/StringBuffer 的调用接管，char[] 数组行为，字符串构造两阶段 |
| jsim-math | Math 系与整型装箱拆箱的调用接管 |
| jsim-random | java/util/Random 的 48 位 LCG 复刻 |
| jsim-stacktrace | getStackTrace 假栈帧构造 |
| jsim-indy | invokedynamic 的 bootstrap 脚手架，供用户组合特定调用点形态 |
| jsim-jni | 本地方法接管的接缝与常用 mock 骨架 |

包内每条接管带独立测试。包之间不互相依赖，组合发生在用户侧。框架不内置任何混淆器知识；jsim-stacktrace 虽然因解密场景而设，但内容是 Thread.getStackTrace 的 JDK 语义，归入 JDK 面扩展包。

## 8. profile 层

profile 是用户仓对特定混淆器的知识：识别启发式、编排顺序、判稳标准。框架不含 profile。

allatori 字符串解密 profile 的骨架：识别 `(Ljava/lang/String;)Ljava/lang/String;` 且静态且体内调 String.length 的方法为解密器；扫描 `ldc` 后紧邻 `invokestatic` 的站点；对每个站点开 sim，密文灌入 locals，strong 形态挂 jsim-stacktrace；返回 Known 字符串则原位改写为 ldc 明文并擦除调用，Bail 则保留原样。改写按不动点逐点推进，一处 Bail 不影响其他站点。

dasho profile 的骨架：调用点形态多样，不做形态匹配，直接对候选区间开区间模式 sim；解密器掺随机时以多 seed 结果一致为判稳条件，配合明文合法性校验（拒绝 NUL、孤立代理项、C0/C1 控制字符）。

## 9. 测试纪律

(1) 真机对拍：算术与槽位重排恒等式以 HotSpot 实测结果为基准，不凭推导。对拍脚手架进共享测试层。

(2) 语料：复用 mjt 仓已 vendor 的字节码语料；扩展包每条接管一测。

(3) 判稳件：多 seed 一致性与明文合法性校验做成库内断言件，profile 直接调用。

(4) 构建：delete-then-rebuild 门，测试产物进项目 test 目录。

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
  ext/             // 扩展包：str、math、random、stacktrace、indy、jni
  test/            // 值代数、逐 handler、恒等式对拍、扩展包接管测试
```

命名空间 jsim。仓规沿用生态约定：`///` 行注释、禁 RTTI、i32/u32/usize、Result 值语义、Utf8String、约定式提交带作用域。

## 11. v1 不做

invokedynamic（Bail，语义由 jsim-indy 组合）、异常表派发（athrow 与区间模式本就绕开帧重算，v1 直接 Bail）、多线程与 monitor（monitor 指令按类型传播为 Unknown）、类初始化链（字段值由预填或接管注入）、finalize 与 GC、JDK 9+ indy 字符串拼接（Bail）、跨方法递归（永久不做，见 D9）、浮点 strict/loose 模式差异（单点口径不分叉）。

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
