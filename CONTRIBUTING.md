# Contributing

libjsim 的红线是核心零依赖：src/jsim 只有接管面与类型传播，语义注入（字符串、数值、反射、indy、libjbc 桥、JNI）一律进 ext/jsim 扩展包，逐包独立 target；核心对任何扩展包零反向依赖。跨包依赖（如绑定层 → 语义面）须在 xmake.lua 注释里记录理由。细节见 [doc/design.rst](doc/design.rst)。

## 构建与测试

```sh
xmake f -y
xmake build -y
xmake run jsim-test
xmake run jsim-jbc-test
xmake run jsim-jni-test
```

Windows 下需要 Visual Studio 2022 的 C++ 工具链；若 xmake 误选 mingw，显式 `xmake f -p windows --toolchain=msvc`。网络受限时 unicorn 可用 `--unicorn_dir=<dir>` 指向本地预编译目录旁路。

改动 jsim-jni 绑定层（ext/jsim/jni_uc.hpp）时，除 jsim-jni-test 外还要跑 `xmake run jsim-demo-jni`（真 unicorn 执行）与 `xmake run jsim-jni-e2e-test`（真 jar + 真 DLL）；后者语料 jar 缺席时 skip，再生成用 test/jni/sample/build-sample.ps1（需要 JDK 与 MSVC）。

## 语料

合成语料入库：test/corpus/JsimJbcBridgeSample.class（javac --release 8 产物 v52）与其 .java 源同放。jni-sample.jar 是构建产物不入库（已 gitignore），由 build-sample.ps1 现场生成。不提交反编译产物；期望值用独立算法复算，不从实现抄。

## 代码约定

- 错误走 `Result` 值语义不抛异常；模拟失败必须是结构化 `Bail`（带 Reason），不允许静默走错路径。
- 定宽类型用 `jsim::u8/u32/u64/usize`；不引入新的第三方依赖进核心。
- 头注释用 `///`，不用 `/** */`；模块内 include `"file.hpp"`，跨模块 `"jsim/path/file.hpp"`，不用 `../`。
- 语义注入优先走接管面（takeover / callMocks / 字段 / 分支），handler 替换是最后手段；新增接管点须与 doc/design.rst 的接管面清单同步。
- 新增行为必须带测试；JNI 函数表新增条目要同步 jsim-jni-test 的骨架覆盖。

## 提交

提交标题直接说清改动，一条提交一件事；正文只写非显然的原因与验证结果（跑过哪些 target、计数多少）。提交前 `git status`，不要带入 `build/`、`.xmake/`、语料 jar 或本地 unicorn 目录。

## 文档规范

三层按**权威来源**分工，扩展名即角色：

| 载体 | 层 | 写什么 | 谁能引用它 |
|---|---|---|---|
| `///` Doxygen | 微观 | 单个符号的契约：参数、返回、失败模式、不变量；单模块的「背景/定位」（`@defgroup` 组注） | 随代码移动 |
| `doc/*.rst` | 宏观 | 跨模块设计、扩展包规格、测试策略 | 代码/测试/构建脚本 |
| `*.md` | 人 + AI 方向 | README、CONTRIBUTING 等流程文档 | 只有人和 AI 读 |

理由：markdown 撑不住代码耦合的规格——无 label、无校验引用面，代码改动后引用静默腐烂。RST 白送可校验的引用面，规范才谈得上执行。

硬规则（`doc/check_refs.py` + Sphinx `-W` 在 CI 挡）：

1. 代码/测试/工具脚本只可引用 `doc/*.rst`，路径必须存在；出现 `.md` 设计引用即失败（流程文档白名单除外）。
2. 引用定位用 label，形式 `doc:<label>`；被引用的位置必须有 `.. _<label>:` 声明（不必每节都放），命名 `<doc-slug>[-<section-slug>]`。跨文档的 `§N` 一律换 `:ref:`，编号会随插入章节漂移。
3. RST 里「`X` 头注」指向微观层：`X` 必须是某个 `@defgroup` 组名或头文件名。
4. RST 站点以 `sphinx-build -b html -E -W --keep-going doc doc/_build` 构建，警告即失败；本地验证先 `pip install "sphinx==9.1.1"`（与 CI 钉同版）。
5. 微观层以 `doxygen doc/Doxyfile` 构建，`WARN_AS_ERROR` 生效；`///` 注释是 API 契约的唯一来源，与代码同仓同改。

RST 写作约定（中文正文的坑）：表格一律 `.. list-table::`（禁 grid table，全角字形会破坏边框校验）；行内代码紧邻中文或全角开括号时在闭定界符后补转义（docutils 要求 end-string 后接空白或 ASCII 标点）；标题层级 `=` / `-` / `^`；跨仓出处带仓名标记（libjbc / libjzip / libjdk_finder / unicorn / luiox），带标记视为 prose 不校验存在性。
