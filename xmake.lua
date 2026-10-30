add_rules("mode.debug", "mode.release")

set_languages("c++17")

-- 中心包仓直连（无仓内副本、无 phony 垫 shim）。仅 demo 的 JDK 定位消费
-- libjdk_finder（jsim-demo-deobf），核心与扩展包零依赖纪律不动。
add_repositories("luiox-repo https://github.com/luiox/luiox-repo.git")
add_requires("libjdk_finder 0.1.1")

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

target("jsim-demo")
    set_kind("binary")
    add_files("demo/main.cpp")
    add_deps("jsim", "jsim-str", "jsim-num", "jsim-reflect")
    if is_plat("windows") then
        add_cxxflags("/utf-8")
    end

-- 静态 deobf demo：javac 现场编译合成样本 + classfile 自备桥 + 解密器模拟还原。
-- 依赖核心与 jsim-str；JDK 定位走 libjdk_finder（运行时探测，不进构建依赖）。
target("jsim-demo-deobf")
    set_kind("binary")
    add_files("demo/deobf/main.cpp")
    add_deps("jsim", "jsim-str")
    add_packages("libjdk_finder")
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
