/*
 * 静态 deobf demo 的合成样本：jsim-demo-deobf 运行时用 javac --release 8 现场编译
 * 成 class（class v52），再由 demo 侧的最小面 classfile 解析器提取解密器与调用点，
 * 桥接进 libjsim 模拟执行还原密文。
 *
 * 这是按公开可复述的混淆模式结构自写的复刻件（算法等价，标识符、明文与密文全部
 * 自起自编），不是任何真实样本的节选或改写：
 *
 *   1. 双 key 交替 XOR 解密器 decrypt(Object)：两把密钥硬编码在函数体内，用可
 *      折叠算术表达式掩盖——(2^5)<<3^(3^5) 折 62、(2^5)<<4^1 折 113；从字符串
 *      末位向前两步一组交替取 key 异或写回 char[]；new String(char[]) 收尾；
 *      参数走 Object、体内 checkcast。
 *   2. 死常量迷惑赋值：4<<4^(3<<2^1) 折 49，从不参与运算。
 *   3. 数字常量算术混淆：全部可静态折叠，纯装饰。
 *   4. 调用点形态：ldc 密文（十六进制 unicode 转义的控制字符）→ invokestatic
 *      解密器 → println。
 *
 * 密文可复现：XOR 对合，加密即解密——
 *   javac -d <out> JsimObfSample.java && java -cp <out> JsimObfSample --gen
 * 把 PLAINS 逐条加密并打印十六进制转义字面量（下方四个调用点即如此生成）；直接跑
 * main 则在真 JVM 上自检四个调用点还原出的明文。
 */
public final class JsimObfSample {

    /// 自编明文清单：与 jsim-demo-deobf 内嵌的期望值一一对应。
    private static final String[] PLAINS = {
        "jsim static deobf demo",
        "the keys swap at dusk",
        "双钥匙逆序起舞",
        "灯",
    };

    private JsimObfSample() {}

    /// 解密器：双 key 交替 XOR，密钥用可折叠算术表达式硬编码，末位向前两步一组。
    public static String decrypt(Object input) {
        String text = (String) input;
        int last = text.length() - 1;
        char[] out = new char[text.length()];
        int keyOddFromEnd = (2 ^ 5) << 3 ^ (3 ^ 5); // 折 62：距尾奇数位
        int decoy = 4 << 4 ^ (3 << 2 ^ 1);          // 死常量迷惑：折 49，从不使用
        int keyEvenFromEnd = (2 ^ 5) << 4 ^ 1;      // 折 113：距尾偶数位（含末位）
        while (last >= 0) {
            out[last] = (char) (text.charAt(last) ^ keyEvenFromEnd);
            if (--last < 0) {
                break;
            }
            out[last] = (char) (text.charAt(last) ^ keyOddFromEnd);
            last--;
        }
        return new String(out);
    }

    /// 调用点 1：偶数长度密文（22 码元，ASCII），密文全为控制字符形态。
    public static void banner() {
        System.out.println(decrypt("\u0054\u0002\u0057\u001c\u001e\u0002\u004a\u0010\u004a\u0018\u005d\u0051\u005a\u0014\u0051\u0013\u0058\u0051\u005a\u0014\u0053\u001e"));
    }

    /// 调用点 2：奇数长度密文（21 码元，ASCII）。
    public static void dusk() {
        System.out.println(decrypt("\u0005\u0056\u0014\u001e\u001a\u005b\u0008\u004d\u0051\u004d\u0006\u005f\u0001\u001e\u0010\u004a\u0051\u005a\u0004\u004d\u001a"));
    }

    /// 调用点 3：非 ASCII 码元密文（7 码元，奇数长度）。
    public static void dance() {
        System.out.println(decrypt("\u53bd\u949b\u5368\u9038\u5efe\u8d49\u826f"));
    }

    /// 调用点 4：单码元密文（逆序循环一步即出的边界）。
    public static void solo() {
        System.out.println(decrypt("\u701e"));
    }

    /// 生成器自带的加密例程：与 decrypt 同算法（XOR 对合，加密即解密）。独立成
    /// 方法而不直接调 decrypt——deobf 导出件会摘掉解密器，生成入口只在原样件
    /// 里复现密文，不依赖解密器存在。
    private static String encryptForGen(String text) {
        int last = text.length() - 1;
        char[] out = new char[text.length()];
        int keyOddFromEnd = (2 ^ 5) << 3 ^ (3 ^ 5); // 折 62：距尾奇数位
        int decoy = 4 << 4 ^ (3 << 2 ^ 1);          // 死常量迷惑：折 77，从不使用
        int keyEvenFromEnd = (2 ^ 5) << 4 ^ 1;      // 折 113：距尾偶数位（含末位）
        while (last >= 0) {
            out[last] = (char) (text.charAt(last) ^ keyEvenFromEnd);
            if (--last < 0) {
                break;
            }
            out[last] = (char) (text.charAt(last) ^ keyOddFromEnd);
            last--;
        }
        return new String(out);
    }

    /// --gen 打印 PLAINS 的密文十六进制转义字面量（复现调用点密文）；无参则真 JVM 自检。
    public static void main(String[] args) {
        if (args.length == 1 && "--gen".equals(args[0])) {
            for (String plain : PLAINS) {
                String cipher = encryptForGen(plain);
                StringBuilder literal = new StringBuilder("\"");
                for (int i = 0; i < cipher.length(); i++) {
                    literal.append(String.format("\\u%04x", (int) cipher.charAt(i)));
                }
                System.out.println(plain + " => " + literal.append('"'));
            }
            return;
        }
        banner();
        dusk();
        dance();
        solo();
    }
}
