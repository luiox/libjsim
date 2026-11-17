add_rules("mode.debug", "mode.release")
set_languages("c++17")
if is_plat("windows") then
    add_cxxflags("/utf-8")
end

option("with_jbc")
    set_default(true)
    set_showmenu(true)
    set_description("build the libjbc bridge and its consumers")
option_end()

option("with_unicorn")
    set_default(true)
    set_showmenu(true)
    set_description("build the Unicorn JNI binding and its consumers")
option_end()

option("unicorn_dir")
    set_default("")
    set_showmenu(true)
    set_description("prebuilt Unicorn directory with include/ and lib/")
option_end()

local with_jbc = has_config("with_jbc")
local with_unicorn = has_config("with_unicorn")
local unicorn_dir = get_config("unicorn_dir") or ""
if with_jbc then
    add_repositories("luiox-repo https://github.com/luiox/luiox-repo.git")
    add_requires("libjbc 0.4.24")
    add_requires("libjzip 0.0.4")
    add_requires("libjdk_finder 0.1.1")
end

-- The x86-only static build also avoids the ARM CRC helper pulling in a crc32
-- symbol that conflicts with libjzip's zlib dependency on Windows.
if with_unicorn and unicorn_dir == "" then
    add_requires("unicorn 2.1.4", { configs = { archs = { "x86" } } })
end

target("jsim")
    set_kind("static")
    add_files("src/jsim/engine.cpp")
    add_headerfiles("src/(jsim/*.hpp)")
    add_includedirs("src", { public = true })
target_end()

includes("ext/xmake.lua")

target("jsim-demo")
    set_kind("binary")
    add_files("demo/main.cpp")
    add_deps("jsim", "jsim-str", "jsim-num", "jsim-reflect")
target_end()

target("jsim-test")
    set_kind("binary")
    add_files("test/main.cpp", "test/jdk.cpp", "test/str.cpp")
    add_deps("jsim", "jsim-str", "jsim-num", "jsim-reflect", "jsim-indy",
             "jsim-math", "jsim-random", "jsim-stacktrace")
target_end()

target("jsim-jni-test")
    set_kind("binary")
    add_files("test/jni/main.cpp")
    add_deps("jsim-jni")
target_end()

if with_jbc then
    target("jsim-demo-deobf")
        set_kind("binary")
        add_files("demo/deobf/main.cpp")
        add_deps("jsim-str", "jsim-jbc")
        add_packages("libjdk_finder", "libjzip")
    target_end()

    target("jsim-jbc-test")
        set_kind("binary")
        add_files("test/jbc/main.cpp")
        add_deps("jsim-str", "jsim-jbc", "jsim-math", "jsim-random")
    target_end()
end

if with_unicorn then
    target("jsim-demo-jni")
        set_kind("binary")
        add_files("demo/jni/main.cpp")
        add_deps("jsim-jni-uc")
        if unicorn_dir ~= "" then
            add_runenvs("PATH", path.join(unicorn_dir, "lib"), path.join(unicorn_dir, "bin"))
        end
    target_end()
end

if with_jbc and with_unicorn then
    target("jsim-jni-e2e-test")
        set_kind("binary")
        add_files("test/jni/e2e/main.cpp")
        add_deps("jsim-jni-uc", "jsim-jbc")
        add_packages("libjzip", "libjdk_finder")
        if unicorn_dir ~= "" then
            add_runenvs("PATH", path.join(unicorn_dir, "lib"), path.join(unicorn_dir, "bin"))
        end
    target_end()
end
