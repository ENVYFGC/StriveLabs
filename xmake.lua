local projectName = "StriveLabs_apr10"

target(projectName)
    add_rules("ue4ss.mod")

    add_includedirs("include", { public = true })
    add_includedirs("include/core", "include/game", "include/render",
                    "include/ui", "include/recordings", { public = true })
    add_headerfiles("include/**.hpp")
    add_headerfiles("include/**.h")

    add_files("src/core/dllmain.cpp")
    add_files("src/game/arcsys.cpp")
    add_files("src/game/bbscript.cpp")
    add_files("src/ui/bind_watcher.cpp")
    add_files("src/render/draw_utils.cpp")
    add_files("src/render/framebar.cpp")
    add_files("src/render/hitboxes.cpp")
    add_files("src/ui/menu.cpp")
    add_files("src/core/sigscan.cpp")
    add_files("src/render/tension_overlay.cpp")
    add_files("src/recordings/recordings.cpp")

    add_syslinks("ole32")

    -- Strip embedded source paths from release DLL
    add_cxflags("/Z7-", "/Zo-", {force = true})
    add_ldflags("/DEBUG:NONE", {force = true})
