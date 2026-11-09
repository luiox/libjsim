# libjsim

[![ci](https://github.com/luiox/libjsim/actions/workflows/ci.yml/badge.svg)](https://github.com/luiox/libjsim/actions/workflows/ci.yml)

JVM 字节码单方法模拟执行器。不追求执行完备，追求框架完备：202 条 opcode 每条有只算类型的默认 handler 且可替换，调用/字段/分支/分配/指令前后/步进全部可接管；语义缺失时结构化 Bail，不崩溃、不静默走错。核心零依赖，语义注入（字符串、数值、反射、indy、libjbc 桥、JNI 本地方法）全部下放扩展包。

- 核心：类型传播栈机，运行结果三态 Completed / Bailed / LimitHit；任意指令起步、预填栈与局部变量、帧快照重放、步数熔断。
- 扩展包：jsim-str（String 家族语义）、jsim-num、jsim-reflect、jsim-indy（invokedynamic 接管域）、jsim-jbc（libjbc 字节码模型 ↔ jsim 执行的官方桥）、jsim-jni（JNI 函数表与钩子的纯层）+ jsim-jni-uc（unicorn x86-64 绑定，Win64 与 SysV 双 ABI 编组）。
- 用途面：demo/deobf 静态去混淆——libjbc 解析改写 + jsim 模拟执行解密器 + 真 JVM 对拍；demo/jni 端到端——手写 x64 native 方法经 unicorn 执行、JNI 调用转接包内钩子。

分层、契约与设计裁决见 [doc/design.rst](doc/design.rst)；范围外事项见其 v1 范围一节。

## 构建

```sh
xmake f -y
xmake build -y          # 全部 target；或逐个 xmake build jsim-test
```

依赖经 xmake 自动拉取：libjbc / libjzip / libjdk_finder 来自 [luiox-repo](https://github.com/luiox/luiox-repo)，unicorn 2.1.4 来自官方 xmake-repo。Windows 需要 Visual Studio 2022；若 xmake 误选 mingw，显式 `xmake f -p windows --toolchain=msvc`。

网络受限的本机开发可用预编译 unicorn 旁路：`xmake f -y --unicorn_dir=<dir>`（含 include/ 与 lib/unicorn.lib，如 python unicorn wheel 自带的那份），此时不触网。

## 测试与对拍

| target | 内容 |
|---|---|
| `xmake run jsim-test` | 核心单测：类型传播、接管面、快照重放（90 项断言） |
| `xmake run jsim-jbc-test` | 桥接测试：合成 class 语料 → libjbc → 桥 → jsim 执行（108/108） |
| `xmake run jsim-jni-test` | JNI 纯层测试：FakeMachine 驱动，零 unicorn 依赖（362/362） |
| `xmake run jsim-demo` / `jsim-demo-deobf` | 核心演示 / 静态去混淆（24/24 断言，含真 JVM 对拍） |
| `xmake run jsim-demo-jni` | unicorn 绑定演示：手写 x64 Win64 机器码 ABI 编组（20/20） |
| `xmake run jsim-jni-e2e-test` | 端到端：真 jar + 真 DLL，unicorn 跑真机器码（13/13，JDK 在场时加跑 HotSpot 对拍腿） |

jsim-demo-deobf 运行时经 libjdk_finder 定位 JDK 现场编译合成样本，需要本机装有 JDK 8+。

e2e 语料 jar 是构建产物不入库，缺席时测试明确 skip；再生成：

```sh
powershell -ExecutionPolicy Bypass -File test/jni/sample/build-sample.ps1
```

## 文档

三层分工（见 [CONTRIBUTING.md](CONTRIBUTING.md) 文档规范节）：

- [doc/design.rst](doc/design.rst)：核心分层、契约与设计裁决（Sphinx 源码，`sphinx-build -b html -E -W doc doc/_build` 本地构建）。
- [doc/ext-jbc.rst](doc/ext-jbc.rst) / [doc/ext-jni.rst](doc/ext-jni.rst)：扩展包规格。
- 头文件 `///` 注释：微观 API 契约（`doxygen doc/Doxyfile`，WARN_AS_ERROR）。
