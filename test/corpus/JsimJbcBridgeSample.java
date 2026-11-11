/// jsim-jbc 桥接测试的合成样本：一个类覆盖桥面的各指令族——
/// ldc 全家桶（int/long/float/double/String/Class）、分支与回边、
/// tableswitch/lookupswitch、四类 invoke、字段面（含 statics 接力）、
/// new/newarray/anewarray/multianewarray、字符串构造两阶段、
/// 以及 indy/athrow 的调用点与异常表正路径（lambdaSite / throwCatch）。
///
/// 语料再生：javac --release 8 -encoding UTF-8 JsimJbcBridgeSample.java
/// （class v52；产物 JsimJbcBridgeSample.class 检入本目录，测试读它，
/// 运行期不需要 JDK）。期望值由 test/jbc/main.cpp 用同算法独立复算。
public class JsimJbcBridgeSample {

    /// invokeinterface 候选：引用落在主类 CP，桥按 InterfaceMethodref 出 CallRef。
    interface Probe {
        int probe(int x);
    }

    /// 解密器形（与 demo/deobf 同骨架）：逐码元算术 + 逆序写 char[] +
    /// new String(char[]) 两阶段构造收尾。算法：out[n-1-i] = c ^ (i+7)。
    public static String decrypt(String cipher) {
        int n = cipher.length();
        char[] out = new char[n];
        for (int i = 0; i < n; i++) {
            char c = cipher.charAt(i);
            out[n - 1 - i] = (char) ((c ^ (i + 7)) & 0xFFFF);
        }
        return new String(out);
    }

    /// 连续 case → tableswitch。
    public static int dense(int k) {
        switch (k) {
            case 10: return 100;
            case 11: return 111;
            case 12: return 122;
            default: return -1;
        }
    }

    /// 稀疏 case → lookupswitch。
    public static int sparse(int k) {
        switch (k) {
            case 1: return 10;
            case 1000: return 10000;
            case 100000: return 1000000;
            default: return 0;
        }
    }

    /// invokestatic 形：同静态双连调（接管点按签名注册的典型形态）。
    public static int viaStatic(int a, int b) {
        return dense(a) + sparse(b);
    }

    /// ldc 族全家桶 + 宽窄混合求值（求值序：((l + d) + f) + i，逐级 widen）。
    public static double wide() {
        long l = 0x1122334455667788L;   // ldc2_w long
        double d = 3.141592653589793;   // ldc2_w double
        float f = 2.5f;                 // ldc float
        int i = 1234567;                // ldc int
        return l + d + f + i;
    }

    /// 循环 + 回边 + iinc：sum(i^2, i=1..n)。
    public static long sum(int n) {
        long acc = 0;
        for (int i = 1; i <= n; i++) acc += i * i;
        return acc;
    }

    /// 非 final 静态字段：不折叠，<clinit> 真 putstatic，用点 getstatic。
    private static long seed = 0x5A5A5A5A5A5A5A5AL;

    /// statics 接力棒：<clinit> 先跑（putstatic），本方法同 Session 读回（getstatic）。
    public static long withSeed(int add) {
        return seed + add;
    }

    /// ldc 类常量：压出该类的 Class 对象（classObjectOf 单例口径）。
    public static Class<?> classConst() {
        return JsimJbcBridgeSample.class;
    }

    /// multianewarray：[[I 维数 2，栈上两维计数；只读最外层长度（内层不递归展开）。
    public static int gridDim(int a) {
        int[][] g = new int[a][3];
        return g.length;
    }

    /// anewarray 对象分量（桥描述符形回归）：CP 类常量是内部名分量
    ///（java/lang/String），桥须装成描述符形入 allocTypes，否则 engine 堆
    /// className 形如 "[java/lang/String"，isRefArray 不识别 → arraylength
    /// 读原始载荷恒 0。
    public static int objArrayDim(int a) {
        Object[] arr = new String[a];
        return arr.length;
    }

    /// 实例面：ctor 走 invokespecial（Object.<init> + putfield），heldTwice 走
    /// getfield + invokevirtual(String.length)，throughProbe 走 invokeinterface。
    private final int held;

    public JsimJbcBridgeSample(int held) {
        this.held = held;
    }

    public int heldTwice(String tag) {
        return held * 2 + tag.length();
    }

    public int throughProbe(Probe p, int x) {
        return p.probe(x) + held;
    }

    /// indy 调用点正路径：lambda 编译为 invokedynamic，桥把它搬成 CallRef
    ///（无 owner 的 NAT 调用点），语义由消费方接管面（callMocksAtPc）决定。
    public static Runnable lambdaSite() {
        return () -> System.out.println("lambda");
    }

    /// athrow 派发正路径：抛出即捕获（ZKM 控制流同形）——try 内 new + athrow，
    /// catch 接住返回哨兵；异常表经桥落 Method.handlers（表序即派发序）。
    public static int throwCatch(int k) {
        try {
            if (k < 0) {
                throw new IllegalStateException("neg");
            }
            return k + 1;
        } catch (IllegalStateException e) {
            return -7;
        }
    }
}
