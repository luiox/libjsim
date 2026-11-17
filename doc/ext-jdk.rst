.. _ext-jdk:

JDK 扩展包
==========

三包分别注册在 ``Session`` 上，只依赖核心。xmake 消费方按需添加
``jsim-math``、``jsim-random``、``jsim-stacktrace``。
按调用点的接管优先于包注册的签名接管。调用方也可在安装后按签名替换。

.. _ext-jdk-math:

Math
----

``jsim::math::install(session)`` 为 Math 和 StrictMath 注册四种基本数值类型的
``abs``、``min``、``max``，以及 float/double 的 ``signum`` 和 ``round``、
double 的 ``floor``、``ceil``、``sqrt``、``rint`` 和整数的
``floorDiv``、``floorMod``。数值未知时按返回类型传播 Unknown。整数除数
已知为零时返回 Bail。未注册的方法走常规接管流程。

.. _ext-jdk-random:

Random
------

``jsim::random::install(session, seedProvider)`` 注册 ``Random()``、
``Random(long)``、``setSeed(long)``、两种 ``nextInt``、
``nextLong``、``nextBoolean``、``nextFloat``、``nextDouble`` 和
``nextBytes(byte[])``。每个对象在堆中保存自己的 48 位种子，通过同一
Session 跨调用保留状态。

无参构造从可选 provider 取得种子。provider 缺席或返回空值时，生成器状态
为 Unknown，数值输出随之传播 Unknown，直至收到具体的 ``setSeed(long)``。
``nextInt(bound)`` 需要已知正上界。种子未知时，对非空 byte[] 的写入以
Bail 停止，因为基本类型数组只能保存具体元素。接管覆盖标准
``java.util.Random`` 对象，子类和其他生成器由调用方提供语义。

.. _ext-jdk-stacktrace:

Thread 与 StackTraceElement
---------------------------

``jsim::stacktrace::install(session, provider)`` 为单方法核心提供虚拟当前
线程。调用方按最新帧在前的顺序返回帧列表，可读取当前方法和 pc，也可返回
Bail。每次 ``Thread.getStackTrace()`` 物化一个新数组。包提供
``getClassName``、``getMethodName``、``getFileName``、``getLineNumber``
和 ``isNativeMethod``。缺少源文件时 getter 返回 null。行号 -2 表示本地方法。

.. code-block:: cpp

   #include "jsim/stacktrace.hpp"

   jsim::stacktrace::install(session, [](const jsim::Frame&) {
       using Trace = jsim::Result<std::vector<jsim::stacktrace::Element>, jsim::Bail>;
       return Trace::ok({{"sample.Decode", "run", std::string("Decode.java"), 42}});
   });

包接受该虚拟线程的栈查询。真实线程、Throwable 的调用栈和自动跨方法采集
由消费方按需接管。
