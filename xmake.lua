add_rules("mode.debug", "mode.release")

set_languages("c++17")

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

target("jsim-test")
    set_kind("binary")
    add_files("test/main.cpp")
    add_deps("jsim", "jsim-str", "jsim-num", "jsim-reflect", "jsim-indy")
    if is_plat("windows") then
        add_cxxflags("/utf-8")
    end
