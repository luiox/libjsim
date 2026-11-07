package sample;

/**
 * 真机对拍入口:加载 native 库后跑同一解密,E2E 用它拿 HotSpot 的实机输出。
 * 用法:java -cp jni-sample.jar sample.Main <dll 绝对路径> <cipher>
 */
public class Main {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        System.out.println(Decrypt.decrypt(args[1]));
    }
}
