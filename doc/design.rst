.. _design:

libjsim 设计稿
===============

状态：设计草案 v0.5（2026-11-07）。v0.1 的三处结构性修正：默认 handler 只算类型不估值、核心只跑一个方法、语义注入全部下放扩展包。v0.3 落地扩展面缺口（字段接管、分支接管收帧、结构化观测与断点、栈平衡开关）并收紧三处口径（Str 身份位、multianewarray 全维分配、num 解析越界），见 :ref:`design-patches`。v0.4 把「桥由消费方自备」升格为官方扩展包 jsim-jbc（libjbc 模型 ↔ jsim 执行，见 :ref:`design-packages`/:ref:`design-testing` 与 :doc:`ext-jbc`）。v0.5 落地 jsim-jni 本地方法扩展包（j2c native 方法由 unicorn 执行、JNI 调用转接包内钩子，见 :ref:`design-packages`/:ref:`design-testing` 与 :doc:`ext-jni`）。

.. _design-positioning:

1. 定位
-------

libjsim 是 JVM 字节码的单方法模拟执行器。它不追求执行完备，追求框架完备：用户在任何需要的位置都能接管语义，缺什么补什么，补到够用即完备。

javavm-master 和 SSVM 的运行范围包括 JDK，实现涉及 HotSpot 内部符号、原生方法与对象布局，维护成本跟随 JDK 版本变化。Unicorn 基于 QEMU 实现 CPU 指令语义，通过 hook 让调用方补充系统调用等运行环境。libjsim 采用类似的环境接管模型，并在 JVM 字节码层支持类型已知而值未知的片段执行。

libjsim 的完备性由此定义为五个面：

.. list-table::
   :header-rows: 1

   * - 完备面
     - 内容
   * - opcode 面
     - 202 条每条有默认 handler，每条可替换
   * - 接管面
     - 调用、字段、分支、分配、指令前后、步进，全部带注册期过滤器
   * - 启动面
     - 任意指令起步、预填栈与局部变量、栈平衡检查可关
   * - 失败面
     - 语义缺失或条件不满足时结构化 Bail，不崩溃、不静默走错
   * - 观测面
     - trace、断点、帧快照、Bail 现场

.. _design-decisions:

2. 拍板记录
-----------

.. list-table::
   :header-rows: 1

   * - #
     - 决定
     - 状态
   * - D1
     - SimValue 三要素：类型、是否已知、值。类型恒在，Unknown 时值不存在
     - 已定
   * - D2
     - 成因、溯源、诊断不进框架不绑 value，用户在接管面自行记录
     - 已定
   * - D3
     - null 是 Known(Ref, null)
     - 已定
   * - D4
     - 栈平衡检查可关
     - 已定
   * - D5
     - 引用身份键为 allocId 单调序号
     - 采纳默认建议
   * - D6
     - 运行结果三态 Completed / Bailed / LimitHit；分支接管三态 Taken / NotTaken / Bail
     - 采纳默认建议
   * - D7
     - 不 fork：分支条件 Unknown 时不双路探索
     - 采纳默认建议
   * - D8
     - 默认 handler 只保证类型传播：操作数全 Known 才算值，否则压入正确类型的 Unknown 继续
     - 已定
   * - D9
     - 核心只跑一个方法，不跨方法递归；多方法由调用方开多个 sim 编排
     - 已定
   * - D10
     - 语义注入（JDK 内建、堆脚手架、indy、JNI）全部为扩展包；核心只有接管面
     - 已定

.. _design-value:

3. SimValue 代数
----------------

3.1 形状
^^^^^^^^

SimValue 是封闭 tagged union，三个成员：类型、是否已知、值。

.. code::

   Type       = I32 | I64 | F32 | F64 | Ref
   Payload    = i64 | f64 | RefPayload        // 仅 Known 时存在
   RefPayload = Null | Str(Utf8String, interned) | Obj(allocId)

类型是槽位的静态事实，决定 cat1 / cat2 与槽位形状；值是动态事实。Unknown(I32) 仍是 I32，只是没有值。null 是已知值不是未知。字符串载荷内联进 value（Utf8String）；对象载荷只背 allocId，内容在会话堆。Str 的 interned 是身份事实（不是诊断，不违反 D2）：ldc 字面量为真——常量池 intern 保证同内容字面量同一对象，引用相等可判；动态构造的字符串（StringBuilder.toString 产物等）为假——真 JVM 里是新对象，模型没有第二身份键，身份不可判，\ ``if_acmpeq`` 走分支接管三态而不是按内容判同。

3.2 运算规则
^^^^^^^^^^^^

(1) Unknown 是吸收元：任一操作数 Unknown，结果为 Unknown(结果类型)。传染只需要这一条，不需要成因。

(2) 操作数全 Known 时按 JVM 规范计算，结果带正确类型写回。算术、转换、移位掩码、比较的 NaN 分支语义依规范实现。

(3) 浮点语义单点化，以 mjbase 常量折叠器为基准实现，全库不出现第二种浮点口径。

(4) 除数含 0 或 Unknown 的整除取余：结果 Unknown 并发观测事件，不猜值、不绕过检查。

(5) 所有恒等式（槽位重排、转换往返）不凭推导上线，按 :ref:`design-testing` 以真机对拍为准。

3.3 不绑上 value 的东西
^^^^^^^^^^^^^^^^^^^^^^^

产生点、调用链、分支条件、def-use 链、类型层次、字符串来源、任何诊断与成因。判断标准：value 的形状必须在第一天定稿，之后加任何观测或诊断能力都不得改动它。存量里 sjvm 的 JavaUnknown 携带持续膨胀的成因枚举、morpher 的产点值携带指令集合与拷贝链，都是反例。

.. _design-typeprop:

4. 类型传播：默认 handler 契约
------------------------------

框架能保证的底线是类型流，不是值流。类型对每条指令都是静态可确定的，值不是。默认 handler 的职责由此限定：算类型，不估值。

核心持有一张静态栈效应表：每条 opcode 的弹出数、压入数、结果类型规则（算术族取操作数类型，转换族按规范映射，栈操作族只动槽位）。默认 handler 行为分两类：

(1) 值内指令（常量装载、算术、转换、栈操作、数组长度等）：操作数全 Known 时算值写回；否则弹出操作数、压入正确类型的 Unknown，继续执行。用户由此可以跑通「直线段 + 关心点」的片段，无关指令不阻断执行。

(2) 调用与分配边界由接管注册提供语义。invoke 系与 indy 共用 CallRef，缺少接管时返回 UnsupportedCall。new 缺少分配脚手架时返回 UnsupportedAlloc。字段访问优先使用 :ref:`design-takeover` 的字段接管，其后查 Session.statics 或 HeapObj.fields，读取缺项时按字段类型返回 Unknown。getfield/putfield 遇到已知 null 接收者时返回 Bail。异常表派发与其他执行近似见 :ref:`design-v1-scope`。

数组写入和实例字段写入需要可定位的目标，目标未知时以 Bail 停止。已知引用数组的具体槽可以保存 Unknown。基本类型数组按位型存储，写入未知值时以 Bail 停止。

.. _design-single:

5. 单方法核心
-------------

一个 sim 实例绑定一个方法。构造参数：方法体（libjbc CodeUnit）、入口 pc、预填帧（栈与局部变量任意槽位，Known 或 Unknown）、限制配置（步数上限、栈平衡开关）。

引擎只做五件事：按 pc 取指令、查 handler、维护槽位形状与 cat2 记账、推进或跳转、熔断与 Bail。不做 verifier 式数据流合并；栈平衡检查开启时（Limits.checkBalance）只做合并点的深度与类型一致性校验——同 pc 二次到达栈形不一致即 Bail{StackBroken}，关闭时引擎信任预填。两个便利入口已落地：runEntry 方法入口起步（栈平衡默认开）与 runRegion 区间起步（栈平衡默认关，容忍预填栈与域外出口——越过方法尾按 Completed(void) 收尾）。两者共用同一引擎，只是预填与开关的预设。

跨方法一律不递归。调用点经接管面处理：扩展包的 mock、用户注册的接管函数、或 Bail。解密链跨多个方法时，调用方在 profile 层编排：取实参、跑被调方法的 sim、把结果作为接管返回值喂回调用方 sim。这个限制换来的是核心没有调用栈、没有递归深度问题、没有类层次解析、没有 clinit 自动链，实例轻到可以按调用点批量构造。

帧快照保存栈和局部变量，堆与静态字段保留在 Session。重复执行有副作用的片段时，调用方需要用同一初始状态准备会话和接管环境，包括随机数种子或 JNI 状态。

.. _design-takeover:

6. 接管面
---------

接管面是核心唯一的扩展机制，扩展包与用户代码走同一套注册接口，核心没有特权内建。

.. list-table::
   :header-rows: 1

   * - 注册项
     - 触发
     - 语义
     - 过滤器
   * - opcode handler
     - 指令派发（\ **先于**\ 效应表）
     - 替换该指令语义，收完整进入帧
     - opcode 槽位
   * - 调用接管
     - invoke 解析接管后
     - 返回 Known 值、写堆、或 Bail
     - owner+name+desc，按调用点注册优先于按签名
   * - 调用兜底解析
     - 静态键都查不到时
     - 按**接收者对象**现场决定语义，或返回 nullptr 放行
     - 收完整帧，可窥视接收者后认领/不认领；解析链按注册序逐个问，单槽不独占
   * - 字段接管
     - getstatic/getfield/putstatic/putfield
     - 注入值或记录；get 系返回值按描述符定型压栈，put 系自行弹栈
     - owner+name+desc，命中优先于存贮底座
   * - 分支接管
     - 条件不具体时（值 Unknown 或引用身份不可判）
     - 三态裁决 Taken / NotTaken / Bail；收完整进入帧，操作数由引擎裁决后统一弹出
     - pc
   * - 堆形态
     - new
     - 注册 allocId 背后的初始载荷
     - 类名
   * - 观测
     - 每步 / Bail
     - stepObservers（指令前结构化只读视图）、trace（步后文本）、bailObserver（Bail 现场）、RunResult 终止帧快照、Limits.stopAtPc 断点熔断（LimitHit 语义，快照灌回即续跑）
     - 全量或观测方自过滤

handler 与接管的签名统一收完整进入帧：栈、局部变量、pc、方法体只读视图、会话引用。引擎不做预弹栈，取数由接管方经受检栈操作完成。这条约束来自 mjbase 的教训：只给产点不给栈的钩子，会迫使用户做两趟探测注入。

**兜底解析**\ 是为「方法名就是数据」的调用面留的口子：注解元素访问器（\ ``d1()``\ ）、getter 家族、反射代理，语义由接收者决定，静态签名键永远匹配不全。解析方拿到帧后自己窥视接收者，认领才返回 mock，不认领返回 nullptr 交回核心 Bail——不允许把「缺环境」静默吞成 Unknown。

.. _design-packages:

7. 扩展包
---------

语义注入全部外置为扩展包。扩展包是独立构建目标，只依赖核心公开头，核心零反向依赖。用户按需链接，不需要的包不进二进制。

.. list-table::
   :header-rows: 1

   * - 包
     - 内容
   * - jsim-str
     - String 的 UTF-16 码元访问、相等性、哈希、切片和搜索，及 StringBuilder/StringBuffer 的构造和追加
   * - jsim-num
     - Integer/Long 的装箱拆箱与十进制解析（解析失败明确 Bail，不编造 0）
   * - jsim-reflect
     - Class.forName / getAnnotation / 注解元素访问器；元数据走 provider 注入，包内不解析 class 文件
   * - jsim-math
     - Math 常用纯函数，涵盖 NaN、带符号零、整数最小值和舍入边界，见 :ref:`ext-jdk-math`
   * - jsim-random
     - java.util.Random 的 48 位 LCG，种子由实参或调用方 provider 提供，见 :ref:`ext-jdk-random`
   * - jsim-stacktrace
     - 将调用方提供的帧物化成 Thread.getStackTrace 结果，见 :ref:`ext-jdk-stacktrace`
   * - jsim-indy
     - invokedynamic 的 bootstrap 脚手架，供用户组合特定调用点形态
   * - jsim-jbc
     - libjbc 字节码模型与 jsim 的官方桥，依赖 libjbc。提供 CodeUnit 到 jsim::Method 的转换和 LDC 字符串读写，详见 :doc:`ext-jbc`
   * - jsim-jni
     - 本地方法接管的接缝与常用 mock 骨架：纯层（JNI 函数表、钩子分发、句柄池、编组、骨架、install 接缝，零依赖）+ jni-uc 绑定层（unicorn x86-64 Win64/SysV，env 表与 trap page 物化、ABI 编组、步数熔断）。native 由 unicorn 执行，JNI 调用转接钩子，用户钩子可整体替换任何 JNI 函数，见 :doc:`ext-jni`

包内每条接管带独立测试。包之间不互相依赖，组合发生在用户侧（jsim-jbc 只依赖外部包 libjbc，不依赖其他扩展包）。框架不内置任何混淆器知识；jsim-stacktrace 虽然因解密场景而设，但内容是 Thread.getStackTrace 的 JDK 语义，归入 JDK 面扩展包。

\ ``jsim-reflect``\ 的边界值得单说：它**不解析 class 文件**。\ ``ClassMeta``\ （类名 + 注解 + 元素值）由 provider 灌进来，libjbc 桥、JSON 表、手写脚本都从同一个口进。这样反射面既不依赖容器格式，也不把「怎么读注解」写死成一种实现。

\ ``jsim-jbc``\ 的边界同样单说：它是**模型搬运桥，不是语义包**。toSimMethod 只做形状搬运（指令摊 pc、标签折 pc、CP 索引重映射），不注册任何接管；桥出的 Method 配普通 Session 跑，缺的语义照 :ref:`design-takeover` 接管面补（callMocks / allocScaffolds / opcodeHandlers / fieldMocks / branchHooks 对消费方全开放）。v0.3 起「桥由消费方自备」（原 :ref:`design-testing`）升格为官方扩展包位面，demo/deobf 即其消费方。

.. _design-profile:

8. profile 层
-------------

profile 是用户仓对特定混淆器的知识：识别启发式、编排顺序、判稳标准。框架不含 profile。

allatori 字符串解密 profile 的骨架：识别 \ ``(Ljava/lang/String;)Ljava/lang/String;``\ 且静态且体内调 String.length 的方法为解密器；扫描 \ ``ldc``\ 后紧邻 \ ``invokestatic``\ 的站点；对每个站点开 sim，密文灌入 locals，strong 形态挂 jsim-stacktrace；返回 Known 字符串则原位改写为 ldc 明文并擦除调用，Bail 则保留原样。改写按不动点逐点推进，一处 Bail 不影响其他站点。

dasho profile 的骨架：调用点形态多样，不做形态匹配，直接对候选区间开区间模式 sim；解密器掺随机时以多 seed 结果一致为判稳条件，配合明文合法性校验（拒绝 NUL、孤立代理项、C0/C1 控制字符）。

.. _design-testing:

9. 测试纪律
-----------

(1) 真机对拍：算术与槽位重排恒等式以 HotSpot 实测结果为基准，不凭推导。对拍脚手架进共享测试层。

(2) jsim-jbc 使用 test/corpus 中的合成 class，来自 ``javac --release 8``。测试运行时无需 JDK。Random 与 Math 的组合样例保存同一 class 在 HotSpot 上的执行结果。

(3) 判稳件：多 seed 一致性与明文合法性校验做成库内断言件，profile 直接调用。

(4) xmake 为核心和每个扩展包提供独立 target。jsim-test 覆盖核心及零依赖的 JDK 扩展包，jsim-jbc-test 通过 libjbc 验证桥接。jsim-jni-test 用 FakeMachine 验证 JNI 纯层。jsim-jni-uc 绑定层依赖 jsim-jni 与 Unicorn，供 jsim-demo-jni 和 jsim-jni-e2e-test 使用。jsim-jni-e2e-test 另依赖 libjbc、libjzip 和 libjdk_finder。Unicorn 2.1.4 从官方 xmake-repo 安装，也可用 \ ``--unicorn_dir=<本地预编译目录>``\ 指定。

核心仅依赖 C++ 标准库。jsim-jbc 依赖 libjbc，通过公开的 ``jsim/jbc.hpp`` 提供 CodeUnit 到 Method 的转换。关闭 ``with_jbc`` 和 ``with_unicorn`` 后，仍可独立构建核心、JDK 扩展和 JNI 纯层。

.. _design-layout:

10. 仓库骨架
------------

.. code::

   libjsim/
     xmake.lua
     doc/design.rst
     doc/ext-jbc.rst    // jsim-jbc 桥扩展包：依赖声明、API 面、最小接管点
     doc/ext-jni.rst    // jsim-jni 扩展包：执行链路、API 面、口径、已知限制
     src/jsim/          // 核心公开头和 engine.cpp
     ext/xmake.lua      // 扩展目标及依赖
     ext/<包名>/src/jsim/  // 扩展公开头和实现，私有头可放在 detail/
     demo/              // 逐场景可执行样例（类型传播 / 接管面 / 区间 / 解密 / 反射）
     demo/deobf/        // 静态 deobf 样例：jsim-jbc 桥 + 模型层改写 + 双查验
     test/              // 值代数、逐 handler、恒等式对拍、扩展包接管测试
     test/jni/          // jsim-jni 测试：FakeMachine 纯层测试 + e2e + sample/
                        //（Java 源 + 自包含 C 源 + build-sample.ps1 + build.gradle）
     test/corpus/       // jsim-jbc 合成语料（.java 源 + javac --release 8 产物）
                        //   jni/（jsim-jni 语料 jar 生成到此处：构建产物不入库）

每个包构建为独立静态库，消费方用 ``jsim/…`` 引用公开头。xmake 的
``add_deps`` 传递所选库和作为 include 根的 ``src/`` 目录。

.. _design-v1-scope:

11. v1 不做
-----------

v1 的范围包括调用与分配接管、单方法执行和有限的异常表派发。具名 catch 仅在堆类名精确匹配时派发，涉及继承关系或 ``athrow null`` 的隐式异常时返回 Bail。monitor 指令采用无操作近似，checkcast 保留引用值。类初始化链、JVM 跨方法调用栈、GC、多线程、indy bootstrap 与 JDK 9+ indy 字符串拼接由消费方按需接管。浮点执行采用单一模式。

.. _design-patches:

12. 实现现状：核心补丁记录
--------------------------

设计稿与首版代码之间曾有四处偏离，已按 D8/D9 补齐：

**(1) 效应表从行表改 dense 索引。** 原来 \ ``infoOf``\ 每步线性扫约 80 行，百万步就是热点。改成 256 槽一次性摊平，\ ``infoOf``\ 退化成两次数组访问。

**(2) 指令接管面补上。** :ref:`design-takeover` 承诺 opcode handler，首版代码里 \ ``infoOf == nullptr``\ 直接 Bail，扩展包无路可走。现在主循环**先**查 \ ``opcodeHandlers``\ 再查效应表——表外 opcode 也能接管，「缺什么补什么」不必等核心补表。

**(3) 默认 handler 从 Bail 改为类型传播。** 这是 D8 的原文，但首版 \ ``executeValue``\ 的 \ ``default:``\ 是 Bail。现在改成「弹掉该弹的槽、压出正确类型的 Unknown」：值算不出来是常态，不是失败；真正需要具体值的地方（分支、switch 键、数组下标）各自有具体性检查兜底。用户没补环境的表现，是一路 Unknown 走到分支处，而不是第一条没见过的指令就断链。

**(4) 栈指令的 cat2 形态。** \ ``pop2``\ /\ ``dup2``\ /\ ``dup_x2``\ /\ ``dup2_x1``\ /\ ``dup2_x2``\ 在 JVMS §6.5 里各有 2~4 种形，首版只做了全 cat1 那一形，\ ``pop2``\ 还错发成 \ ``pop``\。现在按形分流：本模型一个值一个条目，cat2 只影响「弹几个条目」。

同时补齐了：\ ``dup_x2 = 0x5b``\ 常量错（首版误作 0x5d，那是 \ ``dup2_x1``\）、opcode 常量补全到 202 条、约 190 条效应表、\ ``ldc``\ 的 class 常量与 \ ``Session::classObjectOf``\ （\ ``ldc``\ 类常量与 \ ``Class.forName``\ 必须同一对象，否则 \ ``if_acmpeq``\ 假性失败）、数组面按元素宽度与符号性建模（\ ``[B``\ 有符号、\ ``[Z``\ /\ ``[C``\ 无符号）、实例字段面（\ ``HeapObj.fields``\）、\ ``getfield``\ /\ ``putfield``\、浮点与饱和转换、整数除零明确 Bail（需异常派发，不编造商）。

v0.3（2026-10-29）：扩展面补齐与口径收紧
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

架构审查结论：框架没有向执行完备漂移（核心仍单方法、零 JDK 语义、扩展包克制），偏离集中在**扩展面本身没做完**。本版按 :ref:`design-takeover`/:ref:`design-single` 的原文兑现欠账，并收紧三处顺嘴实现的口径。

**(5) 字段接管落地。** :ref:`design-takeover` 表中承诺的字段接管（owner+name+desc 过滤）首版无实现面，用户只能整条替换 opcode 且必须自己重写弹压语义。现在 \ ``Session.fieldMocks``\ 落地：get 系返回值由引擎按描述符定型压栈，put 系由接管方自行弹栈；命中优先于存贮底座。:ref:`design-typeprop` 原文把字段访问归入「未命中一律 Bail」的边界清单并点名 FieldAbsent 原因码，与 :ref:`design-patches` 已挂账的「缺项即 Unknown」互相矛盾——v0.3 定稿为后者（缺项是常态不是失败），:ref:`design-typeprop` 已同步改写；getfield/putfield 的 Known null 接收者按 NPE 口径 Bail{BadOperand}，与整数除零同一纪律。

**(6) 分支接管收完整帧。** 首版 \ ``executeBranch``\ 先弹操作数再查接管，\ ``BranchHook``\ 只收 pc——违反 :ref:`design-takeover` 红线（「引擎不做预弹栈」，正是 mjbase 的教训原文），裁决方既看不到条件操作数、操作数也已被丢弃。现改为窥视判定、裁决后统一弹出，签名收 Frame。

**(7) 观测面结构化。** 首版只有单槽文本 trace（toDisplay 渲染串，拿不到 SimValue）。现在补齐：\ ``stepObservers``\ （每条指令执行前的结构化只读视图：栈、局部变量、方法体、会话）、\ ``bailObserver``\ （Bail 现场回调）、\ ``RunResult.finalStack/finalLocals``\ （三种终态都带的终止帧快照）、\ ``Limits.stopAtPc``\ （断点熔断，LimitHit 语义，快照灌回 Prefill 即续跑）。

**(8) 启动面补齐。** D4 的「栈平衡检查可关」首版是「不存在」而非「可关」。现在 \ ``Limits.checkBalance``\ 落地（同 pc 二次到达深度或类型序列不一致即 Bail{StackBroken}，向量按 pc 索引，不做数据流合并）；两个便利入口 \ ``runEntry``\ （默认开）/\ ``runRegion``\ （默认关、tolerateOffEnd 容忍域外出口）成对落地。

**(9) Str 身份位。** 首版 \ ``sameRef``\ 对所有 Str 载荷按内容判等，把 \ ``sb.toString() == 字面量``\ 假性折真（真 JVM 里 toString 产物是新对象，恒 false）——正是失败面「不静默走错」要防的事。现在 \ ``RefPayload.Str``\ 带 interned 位：ldc 字面量按常量池 intern 事实可判同；动态构造字符串身份不可判，\ ``if_acmpeq``\ 走分支接管三态（compareRef 三态 Same/Diff/Unknown）。

**(10) multianewarray 全维分配。** 长度已知时按 JVM 语义为每个父数组元素分配独立的子数组。内层长度未知时，最外层元素保留为 Unknown(Ref)。引擎在分配前检查负长度和总分配量，以 Bail 报告超限。

**(11) jsim-num 解析越界。** \ ``Integer.parseInt("2147483648")``\ 首版静默回绕（注释自称「按 JVM 口径回绕」——那是算术口径，解析口径在 JDK 是 NumberFormatException）。现在越界与位数溢出一律解析失败 Bail，对齐本包「不编造 0」的契约。

**(12) 工程面。** 测试层落地（test/，90 项断言：值代数、逐类 handler、五个完备面、扩展包每条接管一测）；xmake 按包独立 target（jsim-str/num/reflect/indy）；demo 移除未消费的 libjstr 依赖；MethodBuilder 补 if_acmpeq 族与 multianewarray 助手。

v0.5（2026-11-07）：jsim-jni 本地方法扩展包
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

issue #4 的落地。执行链：模拟字节码 invoke 到 native 方法 → install 挂进 callMockResolvers 兜底链的解析器认领（键 = owner.name+desc，callMocks 优先级天然更高）→ 弹参编组成位型 → unicorn 跑机器码 → 机器码经物化的 JNIEnv 函数表（JDK 8 冻结布局 233 项）调表 → trap page 上的 UC_HOOK_CODE 按地址差还原函数号 → dispatch 查钩子（用户钩子 > 默认骨架 > Bail{UnsupportedCall}）→ 返回位型写回寄存器 → native 停机后解码回 SimValue。锚点与 javavm-master/SSVM 的教训相反：锚 JNI ABI（冻结的表布局与调用约定），不锚任何混淆器的代码形态。

拍板记录：

.. list-table::
   :header-rows: 1

   * - #
     - 决定
   * - D13
     - 分层双 target：jsim-jni 纯层零依赖（FakeMachine 可测）+ jsim-jni-uc 绑定层（unicorn）。绑定子面 → 语义面是「扩展包互不依赖」的唯一例外（:ref:`design-testing` 已记）
   * - D14
     - 句柄是假 guest 指针，不映射页——机器码解引用句柄落未映射读，由绑定层转 Bail{BadOperand}；local 一次调用回收，global 跨调用持久
   * - D15
     - 字符串物化副本落 scratch 区，Release\* 回收；字节按家族口径 MUTF-8 原样透传
   * - D16
     - 字段读写直连 Session.statics / HeapObj.fields——native 侧与字节码侧同一份状态；缺值 Bail{NotConcrete}（native 无法表达 Unknown，不编 0）
   * - D17
     - Call\*Method 经 env.javaInvoker 回跳，消费方开嵌套 Sim（D9 不破）；A 变体读 jvalue\*，V 变体 Win64 直读、SysV 拒，裸变体按 ABI 位置读
   * - D18
     - 异常面是 pendingThrow 位：ThrowNew 置位，ExceptionCheck/Occurred 反映，置位期间拒非检查类调用，native 收尾仍在则整体 Bail（athrow 同纪律）
   * - D19
     - 包不做二进制加载：入口地址由消费方给（手工填表或 RegisterNatives 骨架录表）；ABI 先行 Win64 + SysV，AArch64 留位
   * - D20
     - JNIEnv 双重间接物化：env 是指向 JNIEnv 结构的指针，结构首成员才是函数表（\ ``(*env)->Fn``\ ）。物化两页 \ ``[envTable] = funcTable``\ 、\ ``funcTable[i] = trapPage + i*8``\ 。教训：demo 机器码与样本 DLL 曾按「env 即表」的单重间接两边一致地错，由真机对拍腿（HotSpot 跑同一份 DLL）抓出——JNI ABI 约定必须对真机验证，两套模拟互证不构成证据
   * - D21
     - JNI C 字符串口径：GetStringUTFChars 契约是 NUL 终止 C 串，实参字符串首个 NUL 之后不可往返，HotSpot 同口径；含 0x00 字节的样本按截断后比对，是 ABI 事实而非缺陷。测试样本构建（Java 源 + 自包含 C 源 + ps1/gradle）进仓 \ ``test/jni/sample/``\ ；产物 jar 生成到 \ ``test/corpus/jni/``\ 但不入库，E2E 语料缺失时整条跳过并提示再生成命令

E2E 证据（jsim-jni-e2e-test）：本地生成的 \ ``test/corpus/jni/jni-sample.jar``\ （真实 MSVC 编译的 decrypt.dll）→ jar 条目读出 → PE64 按字节偏移解析、首选基址展开、导出表定位入口（消费方装载职责的参考实现）→ jsim-jbc 桥出字节码 → invokestatic → unicorn 跑真机器码 → 明文还原，且与 HotSpot 跑同 jar 同 DLL 的输出一致。样本再生成：\ ``test/jni/sample/build-sample.ps1``\ 或 \ ``gradle sampleJar``\ 。

.. _design-evidence:

附录 A：存量证据
----------------

证据按文件与符号引用，行号从略。

A.1 javavm-master（samczsun，java-deobfuscator 系，2018 年起停止）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

\ ``VirtualMachine.java``\ 的 \ ``registerNatives``\ 硬编码大量 JDK 内部符号 hook；handler 表与内联 switch 双轨并存，迁移半途丢失 l2d / f2l / d2f / goto_w / jsr_w 的实现；\ ``JavaUnknown.merge``\ 以字符串拼接合并；分支对 Unknown 双路递归拷贝栈，以访问计数上限返回毒值；\ ``execute``\ 只能从方法首条指令起步，栈在内部构造不可预填；无字节码校验。

A.2 SSVM（xxDark，Recaf v3 引擎，2024 年起停止）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

\ ``RuntimeBootClassFinder``\ 以宿主 boot classloader 取被解释字节码；\ ``InjectedClassLayout``\ 向宿主类注入 HotSpot 内部字段；\ ``DefaultStringOperations``\ 按 JDK 版本分叉字符串私有布局；Natives 包按 JDK native 逐条复刻，setInvoker 注册数百处；\ ``Value``\ 无已知未知之分；执行前必须完成整个 bootstrap，无片段执行与快照。

A.3 mjbase / libjbc
^^^^^^^^^^^^^^^^^^^

libjbc \ ``analysis/domain.hpp``\ 的 DomainEngine 与 IValueDomain 已验证引擎持形状、值域供钩子的分离，含 visitCap 熔断；mjbase \ ``const_domain.hpp``\ 的 ConstLattice 是 SimValue 的最小可行实例。两处明文挂账：产点钩子看不到操作数栈（libjbc 的 alias_analysis 组注）、无分支条件钩子（branch_facts 与 path_conditions 双轨）。字符串载荷口径三套并存、浮点政策三处不一、除零三种处置，均未单点化。

A.4 morpher legacy（Java）
^^^^^^^^^^^^^^^^^^^^^^^^^^

\ ``morpher-sjvm``\ 为 javavm-master 的仓内 fork，曾尝试以 UnknownCause 枚举与 MaybeBoolean 三态续命。\ ``MethodExecutor``\ 为单 switch 巨石，四个 invoke 分支复制粘贴，无步数上限，异常吞并后继续。ASM Interpreter 族有八个以上互不共享的常量折叠器。\ ``simulator/``\ 与 \ ``simulator/old/``\ 为两代未完成的统一模拟器遗迹。可回收资产：Provider SPI 的 can/do 双阶段与组合器、调用点 mock 优先于签名 mock 的优先级设计、三层 hook 粒度经验。

A.5 analysis-\* 四仓（C++，2026-10 建仓）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

allatori 在 string_decrypt_pass 内嵌约千行解释器，值模型无 Unknown，遇不支持指令毒化整个解密器。dasho 的 jvm_interp 是最接近本设计的存量：Unknown 值、结构化失败枚举、方法与区间双入口、调用内建与纯度判定共表、StringBuilder 与 Random 堆脚手架；缺陷是 Unknown 以 0 参与运算绕过除零检查、INSTANCEOF 恒返 0。grunt 直接委托 mjbase evaluator，仓内仅薄适配层，是「有框架时仓的形态」的实证；但 evaluator 自述无 Unknown、无调用、无堆、无字符串，目标一旦上字符串加密即零覆盖。三仓重复约四千行：四套值模型、三套主循环、两份逐字相同的 MUTF-8 编解码、三个互不一致的熔断常数。skidonion 为 native 容器仓，与本题无关。

A.6 AIR / SIR（morpher 仓 mjt-deobf）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

L3 归一化 IR 与 L4 SSA，接 libjbc 会话面，SSA 回写栈机的真实 lower 未实现。与本设计正交：AIR/SIR 是 IR 层，libjsim 是求值层。潜在接合点是 SIR 侧常量折叠需要字符串或调用语义时，以 libjsim 为求值后端。
