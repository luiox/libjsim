# jsim-jbc 桥扩展包

libjbc 字节码模型 ↔ libjsim 模拟执行的官方桥（`ext/jsim/jbc.hpp`，
命名空间 `jsim::jbcbridge`）。v0.4 起从「消费方自备」（原设计 §9）升格为
库内扩展包；demo/deobf 是它的首个消费方。

## 依赖声明与零依赖边界

- 本包依赖**外部包 libjbc**（`jbc/ir` 会话面 + `jbc/core` Op，经 luiox-repo
  `add_requires("libjbc")`），是唯一带外部依赖的扩展包位面。
- 核心零依赖红线不动：src/jsim 纯 std 头文件，不进任何包依赖，对本包
  （及一切扩展包）零反向依赖。
- 本包不依赖其他扩展包（str/num/reflect/indy…），组合发生在用户侧：
  典型消费方装配 = `str::install` + `str::installStringAlloc` + 你自己的接管。

## API 面

| 符号 | 职责 |
|---|---|
| `toSimMethod(ClassSession&, methodIdx) -> Result<Method, BridgeError>` | 单方法形状搬运：指令摊 pc 索引、标签折 pc（末尾锚折 code.size()）、ldc 族 CP 索引重映射成 jsim 紧凑表下标、NEWARRAY atype a→b、MULTIANEWARRAY 剥维。语义视图走 `constOf`/`memberOf`/`classOf`，桥不手走常量池 |
| `isLdcFamily(Op)` | LDC / LDC_W / LDC2_W 判定 |
| `ldcStringOf(ClassSession&, Insn&, std::string&)` | 读 ldc 字符串常量；MUTF-8 字节按家族口径**原样透传**（BMP 非零码元下与 UTF-8 逐字节相同） |
| `setLdcString(ClassSession&, CodeDom&, InsnId, text)` | 明文写回 ldc：走 libjbc `setString`（CP 追加去重、新索引超 u1 域自动升格 LDC→LDC_W，宽度陷阱由 libjbc 挡掉） |
| `BridgeError` | 结构化失败（stage + detail）。纪律：宁可整方法放弃，不产半截方法 |

桥接覆盖面：常量/装载存储/算术/转换/栈操作/iinc/monitor（a/b 原样）、ldc 全
家桶（int/long/float/double/String/Class）、四类 invoke（CallRef 带
owner/name/desc/retType/hasReturn/**isStatic**）、字段四形（FieldRef）、
new/newarray/anewarray/multianewarray、分支 19 形（标签折 pc）、
tableswitch/lookupswitch、checkcast/instanceof（jsim 默认语义）。

明确拒绝（`BridgeError::UnsupportedOp`）：invokedynamic（jsim-indy 组合域）、
athrow（异常表派发 v1 不做）、jsr/jsr_w/ret（子例程模型不进 jsim）。

## 消费方最小接管点

桥只搬运模型，**不注册任何语义**。桥出的 `Method` 配普通 `jsim::Session`
跑，缺什么按接管面（设计 §6）补，注册表对消费方全开放：

```cpp
Session s;
str::install(s);                 // String.length/charAt、StringBuilder 家族
str::installStringAlloc(s);      // new String 的分配脚手架 + <init>([C)V 两阶段
s.callMocks["java/lang/Object.<init>()V"] = ...;  // 构造链公共尾巴，消费方自备

auto m = jbcbridge::toSimMethod(*session, decryptIdx);
Prefill p;
p.locals = {SimValue::internedStr(cipher)};   // ldc 字面量带 intern 身份口径
RunResult r = Sim(m.value(), s).runEntry(std::move(p), 1u << 16);
```

- `callMocks` / `callMocksAtPc`：invoke 系与跨方法编排接力（解密器场景主面）；
- `allocScaffolds`：new 系分配脚手架；
- `opcodeHandlers`：表外 opcode 或默认语义不够时的整条接管；
- `fieldMocks` / `branchHooks` / `callMockResolver`：字段注入、分支三态裁决、
  按接收者的兜底解析。

## 测试

`jsim-jbc-test`（独立 target，与零依赖的 jsim-test 分立，libjbc 只挂本面）：
合成语料 `test/corpus/JsimJbcBridgeSample.class`（javac --release 8 产物
v52，源码同目录，运行期零 JDK 依赖），期望值由测试用同算法独立复算。
再生语料：

```sh
javac --release 8 -encoding UTF-8 test/corpus/JsimJbcBridgeSample.java
```

（嵌套接口产物 `JsimJbcBridgeSample$Probe.class` 不需要入库——主类 CP 自带
invokeinterface 引用，解析不需要它。）
