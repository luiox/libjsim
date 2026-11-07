/*
 * decrypt_jni.c - native implementation of the E2E sample (j2c artifact shape).
 *
 * Self-contained: no jni.h include (no JDK include path needed). JNIEnv* is
 * used per the real JNI convention: env points to a struct whose first member
 * is the function table (native code always goes through (*env)->Fn), so the
 * table is *(void***)env. Function indices are the jni.hpp table indices; the
 * real-JVM cross-check leg runs this exact DLL on HotSpot, so a wrong index
 * breaks there first.
 *
 * Build constraints: /GS- /Zl + link /NOENTRY /NODEFAULTLIB - no CRT, no
 * imports, no DllMain. The emulated loader does not process an import table.
 */

typedef const char *(*GetStringUTFChars_t)(void *env, void *str, unsigned char *isCopy);
typedef void (*ReleaseStringUTFChars_t)(void *env, void *str, const char *chars);
typedef void *(*NewStringUTF_t)(void *env, const char *chars);

__declspec(dllexport) void *Java_sample_Decrypt_decrypt(void *env, void *clazz, void *input)
{
    void **table = *(void ***)env; /* (*env)-> convention */
    GetStringUTFChars_t getStringUTFChars = (GetStringUTFChars_t)table[169];
    ReleaseStringUTFChars_t releaseStringUTFChars = (ReleaseStringUTFChars_t)table[170];
    NewStringUTF_t newStringUTF = (NewStringUTF_t)table[167];

    (void)clazz; /* static native: jclass carries no semantics here */

    char *chars = (char *)getStringUTFChars(env, input, 0);
    if (chars == 0) {
        return 0; /* straight-line code does no exception dispatch */
    }
    for (int i = 0; chars[i] != 0; ++i) {
        chars[i] = (char)(chars[i] ^ 0x5A);
    }
    releaseStringUTFChars(env, input, chars);
    return newStringUTF(env, chars);
}
