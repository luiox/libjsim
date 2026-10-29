add_rules("mode.debug", "mode.release")

-- libjstr 消费的 libca 单源版本（与 mjt 同口径：同一消费图内各包的
-- libca (版本, 模块集) 必须完全一致，否则多实例双归档）。
LUIOX_LIBCA_VERSION = "0.0.16"
LUIOX_LIBCA_MODULES = "core,crypto,str,fs,env,io,json,net,http,process,opt,thread,log,zip,resources,i18n,test"

add_requires("libjstr 0.0.3")
add_requireconfs("libjstr.libca", {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})

target("jsim")
    set_kind("headeronly")
    add_headerfiles("src/jsim/*.hpp")
    add_includedirs("src", { public = true })

target("jsim-str")
    set_kind("headeronly")
    add_headerfiles("ext/jsim/*.hpp")
    add_includedirs("ext", { public = true })
    add_deps("jsim")

target("jsim-demo")
    set_kind("binary")
    add_files("demo/main.cpp")
    add_deps("jsim", "jsim-str")
    add_packages("libjstr")
    if is_plat("windows") then
        add_cxxflags("/utf-8")
    end
