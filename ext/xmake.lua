local function extension(name, configure)
    local source_dir = name .. "/src"
    target("jsim-" .. name)
        set_kind("static")
        add_files(source_dir .. "/jsim/*.cpp")
        add_headerfiles(source_dir .. "/(jsim/*.hpp)")
        add_includedirs(source_dir, { public = true })
        add_deps("jsim")
        if configure then configure() end
    target_end()
end

extension("str")
extension("num")
extension("reflect")
extension("indy")
extension("math")
extension("random")
extension("stacktrace")
extension("jni")

if has_config("with_jbc") then
    extension("jbc", function()
        add_packages("libjbc", { public = true })
    end)
end

if has_config("with_unicorn") then
    extension("jni-uc", function()
        add_deps("jsim-jni")
        local unicorn_dir = get_config("unicorn_dir") or ""
        if unicorn_dir == "" then
            add_packages("unicorn", { public = true })
        else
            add_defines("UNICORN_SHARED", { public = true })
            add_includedirs(path.join(unicorn_dir, "include"), { public = true })
            add_linkdirs(path.join(unicorn_dir, "lib"), { public = true })
            add_links("unicorn", { public = true })
            add_runenvs("PATH", path.join(unicorn_dir, "lib"), path.join(unicorn_dir, "bin"))
        end
    end)
end
