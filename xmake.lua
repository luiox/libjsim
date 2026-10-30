add_rules("mode.debug", "mode.release")

set_languages("c++17")

-- 中心包仓直连（无仓内副本、无 phony 垫 shim）。demo 的 deobf 线消费
-- libjdk_finder（JDK 定位）/ libjbc（classfile 解析与改写）/ libjzip
-- （导出 jar 读回查验），核心与扩展包零依赖纪律不动。
add_repositories("luiox-repo https://github.com/luiox/luiox-repo.git")
add_requires("libjdk_finder 0.1.1")
add_requires("libjbc 0.4.24")
add_requires("libjzip 0.0.4")

-- 核心零外部依赖（设计 §9）；扩展包逐包独立 target，按需依赖（设计 §7）。
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

-- jsim-jbc 桥扩展包（设计 §7/§9）：libjbc 字节码模型 ↔ jsim 模拟执行的
-- 官方桥——CodeUnit → jsim::Method 形状搬运、LDC 字符串面（setString 自动
-- 升格口径）、消费方最小接管点。唯一带 libjbc 依赖的扩展包位面；核心
-- 零依赖红线不动（核心对本包零反向依赖）。
target("jsim-jbc")
    set_kind("headeronly")
    add_headerfiles("ext/jsim/jbc.hpp")
    add_includedirs("ext", { public = true })
    add_deps("jsim")
    add_packages("libjbc", { public = true })

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
-- → libjbc → 桥 → jsim 执行，期望值同算法独立复算（设计 §9）。
target("jsim-jbc-test")
    set_kind("binary")
    add_files("test/jbc/main.cpp")
    add_deps("jsim", "jsim-str", "jsim-jbc")
    if is_plat("windows") then
        add_cxxflags("/utf-8")
    end
