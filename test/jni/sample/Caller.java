package sample;

/**
 * 调用方:模拟侧(字节码)真正执行的方法——invokestatic 到 native 解密器。
 * E2E 测试桥接的就是这个方法。
 */
public class Caller {
    public static String run(String input) {
        return Decrypt.decrypt(input);
    }
}
