package sample;

/**
 * j2c 形态的解密类:方法体被"编译到 native"(demo 样本里即 decrypt.dll 的
 * Java_sample_Decrypt_decrypt),字节码只剩 native 声明。
 */
public class Decrypt {
    public static native String decrypt(String input);
}
