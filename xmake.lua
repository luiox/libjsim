add_rules("mode.debug", "mode.release")

set_languages("c++17")

-- 中心包仓直连（无仓内副本、无 phony 垫 shim）。demo 的 deobf 线消费
-- libjdk_finder（JDK 定位）/ libjbc（classfile 解析与改写）/ libjzip
-- （导出 jar 读回查验），核心与扩展包零依赖纪律不动。
add_repositories("luiox-repo https://github.com/luiox/luiox-repo.git")
add_requires("libjdk_finder 0.1.1")
add_requires("libjbc 0.4.24")
add_requires("libjzip 0.0.4")

-- 网络受限的本机开发:--unicorn_dir=<dir> 指向预编译 unicorn(include/ +
-- lib/unicorn.lib,如 python unicorn wheel 自带的那份,2.1.x),三个 unicorn
-- 消费面改走本地链接;CI 不设该配置,照常 add_requires("unicorn 2.1.4")。
option("unicorn_dir")
    set_default("")
    set_showmenu(true)
    set_description("local prebuilt unicorn dir (include/ + lib/unicorn.lib)")
option_end()

local unicorn_dir = get_config("unicorn_dir")
if unicorn_dir == nil then unicorn_dir = "" end

-- 官方 xmake-repo 依赖只在走网络路径时装载;--unicorn_dir 旁路时完全不触网。
-- archs 裁到 x86:本仓 unicorn 消费面只有 x86-64(jni_uc Win64/SysV 编组、
-- e2e 手写 x64 机器码),aarch64 纯死重;不开它还规避一个 Windows 链接坑——
-- unicorn 把 qemu/util/crc32c.c 无条件编进静态库(导出 crc32c 与裸 crc32),
-- 其 aarch64 CRC 助手引用这组符号,把该 obj 拉进链接闭包,与 libjzip 传递
-- 依赖 zlib 的 crc32 撞名(LNK2005/LNK1169);x86 的 CRC32 助手自带位循环,
-- 不引用这组符号,该 obj 保持惰性不进闭包。
if unicorn_dir == "" then
    add_requires("unicorn 2.1.4", { configs = { archs = { "x86" } } })
end

-- 核心零外部依赖（doc:design-testing）；扩展包逐包独立 target，按需依赖（doc:design-packages）。
target("jsim")
    set_kind("headeronly")
    add_headerfiles("src/jsim/*.hpp")
    add_includedirs("src", { public = true })

target("jsim-str")
    set_kind("headeronly")
    add_headerfiles("ext/jsim/str.hpp")
    add_includedirs("ext", { public = true })
    add_deps("jsim")

target("jsim-num")
    set_kind("headeronly")
    add_headerfiles("ext/jsim/num.hpp")
    add_includedirs("ext", { public = true })
    add_deps("jsim")

target("jsim-reflect")
    set_kind("headeronly")
    add_headerfiles("ext/jsim/reflect.hpp")
    add_includedirs("ext", { public = true })
    add_deps("jsim")

target("jsim-indy")
    set_kind("headeronly")
    add_headerfiles("ext/jsim/indy.hpp")
    add_includedirs("ext", { public = true })
    add_deps("jsim")

-- jsim-jbc 桥扩展包（doc:design-packages/design-testing）：libjbc 字节码模型 ↔ jsim 模拟执行的
-- 官方桥——CodeUnit → jsim::Method 形状搬运、LDC 字符串面（setString 自动
-- 升格口径）、消费方最小接管点。唯一带 libjbc 依赖的扩展包位面；核心
-- 零依赖红线不动（核心对本包零反向依赖）。
target("jsim-jbc")
    set_kind("headeronly")
    add_headerfiles("ext/jsim/jbc.hpp")
    add_includedirs("ext", { public = true })
    add_deps("jsim")
    add_packages("libjbc", { public = true })

-- jsim-jni 本地方法扩展包(doc:ext-jni):纯层——JNI 函数表(JDK 8 布局)、
-- 钩子分发、句柄池、编组、默认骨架、install 接缝。零外部依赖,测试走 FakeMachine。
target("jsim-jni")
    set_kind("headeronly")
    add_headerfiles("ext/jsim/jni.hpp")
    add_includedirs("ext", { public = true })
    add_deps("jsim")

-- jsim-jni 绑定层:unicorn 引擎持有、JNIEnv 表与 trap page 物化、x86-64 ABI
-- (Win64 + SysV)寄存器编组、步数熔断。依赖例外已记录:绑定子面 → 语义面。
target("jsim-jni-uc")
    set_kind("headeronly")
    add_headerfiles("ext/jsim/jni_uc.hpp")
    add_includedirs("ext", { public = true })
    add_deps("jsim", "jsim-jni")
    if unicorn_dir ~= "" then
        add_defines("UNICORN_SHARED", { public = true })
        add_includedirs(path.join(unicorn_dir, "include"), { public = true })
        add_linkdirs(path.join(unicorn_dir, "lib"), { public = true })
        add_links("unicorn", { public = true })
        add_runenvs("PATH", path.join(unicorn_dir, "lib"), path.join(unicorn_dir, "bin"))
    else
        add_packages("unicorn", { public = true })
    end

target("jsim-demo")
    set_kind("binary")
    add_files("demo/main.cpp")
    add_deps("jsim", "jsim-str", "jsim-num", "jsim-reflect")
    if is_plat("windows") then
        add_cxxflags("/utf-8")
    end

-- 静态 deobf demo：javac 现场编译合成样本 → libjbc 解析/改写 + jsim-jbc 官方桥
-- （CodeUnit → jsim::Method，ext/jsim/jbc.hpp）→ 解密器模拟还原 → 导出 deobf
-- jar → libjzip 读回结构对拍 + 真 JVM 对拍。JDK 定位/编译/打包走
-- libjdk_finder 找到的 JDK 工具（运行时探测，不进构建依赖）。
target("jsim-demo-deobf")
    set_kind("binary")
    add_files("demo/deobf/main.cpp")
    add_deps("jsim", "jsim-str", "jsim-jbc")
    add_packages("libjdk_finder", "libjbc", "libjzip")
    if is_plat("windows") then
        add_cxxflags("/utf-8")
    end

target("jsim-test")
    set_kind("binary")
    add_files("test/main.cpp")
    add_deps("jsim", "jsim-str", "jsim-num", "jsim-reflect", "jsim-indy")
    if is_plat("windows") then
        add_cxxflags("/utf-8")
    end

-- jsim-jbc 桥接测试（独立 target：与零依赖的 jsim-test 分开，语料与依赖
-- 只挂本面）：合成 class 语料（test/corpus，javac --release 8 产物 v52）
-- → libjbc → 桥 → jsim 执行，期望值同算法独立复算（doc:design-testing）。
target("jsim-jbc-test")
    set_kind("binary")
    add_files("test/jbc/main.cpp")
    add_deps("jsim", "jsim-str", "jsim-jbc")
    if is_plat("windows") then
        add_cxxflags("/utf-8")
    end

-- jsim-jni 纯层测试(FakeMachine 驱动,零 unicorn 依赖):骨架逐条、句柄池
-- 生命周期、异常位、编组对拍、钩子覆盖优先级(doc:ext-jni 测试节)。
target("jsim-jni-test")
    set_kind("binary")
    add_files("test/jni/main.cpp")
    add_deps("jsim", "jsim-jni")
    if is_plat("windows") then
        add_cxxflags("/utf-8")
    end

-- jsim-jni 端到端 demo:手写 x64 Win64 机器码的 native 方法(GetStringUTFChars
-- → XOR → NewStringUTF),jsim 字节码 invokestatic 发起走完整链;兼作绑定层
-- 的真机验证。期望值 C++ 独立复算。
target("jsim-demo-jni")
    set_kind("binary")
    add_files("demo/jni/main.cpp")
    add_deps("jsim", "jsim-jni", "jsim-jni-uc")
    if unicorn_dir == "" then
        add_packages("unicorn")
    else
        add_runenvs("PATH", path.join(unicorn_dir, "lib"), path.join(unicorn_dir, "bin"))
    end
    if is_plat("windows") then
        add_cxxflags("/utf-8")
    end

-- jsim-jni 端到端测试(真 jar + 真 DLL):libjzip 解包语料 jar → PE 装载(首选
-- 基址 + 导出表定位入口)→ jsim-jbc 桥出调用方字节码 → jsim → unicorn 跑真
-- 机器码 → JNI 骨架钩子 → 明文还原;JDK 可用时另跑真 JVM 对拍腿。样本再生成:
-- test/jni/sample/build-sample.ps1 或 gradle sampleJar。
target("jsim-jni-e2e-test")
    set_kind("binary")
    add_files("test/jni/e2e/main.cpp")
    add_deps("jsim", "jsim-jni", "jsim-jni-uc", "jsim-jbc")
    if unicorn_dir == "" then
        add_packages("unicorn")
    else
        add_runenvs("PATH", path.join(unicorn_dir, "lib"), path.join(unicorn_dir, "bin"))
    end
    add_packages("libjbc", "libjzip", "libjdk_finder")
    if is_plat("windows") then
        add_cxxflags("/utf-8")
    end
