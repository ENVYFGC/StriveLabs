local projectName = "StriveLabs_apr10"

target(projectName)
    add_rules("ue4ss.mod")

    add_includedirs("include", { public = true })
    add_includedirs("include/core", "include/game", "include/render",
                    "include/ui", "include/recordings", { public = true })
    add_headerfiles("include/**.hpp")
    add_headerfiles("include/**.h")

    add_files("src/core/dllmain.cpp")
    add_files("src/core/replay_manager.cpp")
    add_files("src/core/timeline.cpp")
    add_files("src/core/game_state.cpp")
    add_files("src/core/keybindings.cpp")
    add_files("src/game/arcsys.cpp")
    add_files("src/game/bbscript.cpp")
    add_files("src/ui/bind_watcher.cpp")
    add_files("src/render/draw_utils.cpp")
    add_files("src/render/framebar.cpp")
    add_files("src/render/hitboxes.cpp")
    add_files("src/ui/menu.cpp")
    add_files("src/ui/keybinds.cpp")
    add_files("src/core/sigscan.cpp")
    add_files("src/render/tension_overlay.cpp")
    add_files("src/recordings/recordings.cpp")
    add_files("src/ui/imgui_overlay.cpp")
    add_files("src/core/crash_log.cpp")
    add_files("src/core/select_memory.cpp")

    -- Dear ImGui (MIT, third_party/imgui/LICENSE.txt), drawn through a DirectX 11 Present hook
    add_includedirs("third_party/imgui")
    add_files("third_party/imgui/imgui.cpp", "third_party/imgui/imgui_draw.cpp",
              "third_party/imgui/imgui_tables.cpp", "third_party/imgui/imgui_widgets.cpp",
              "third_party/imgui/backends/imgui_impl_dx11.cpp", "third_party/imgui/backends/imgui_impl_win32.cpp")
    add_syslinks("d3d11", "dxgi", "d3dcompiler", "windowscodecs")

    add_syslinks("ole32", "user32", "comdlg32")

    -- Keep local folder paths out of the release DLL: the debug record only names
    -- the .pdb file, and no_source_paths.h compiles out the asserts that embed
    -- header paths
    add_cxflags("/Z7-", "/Zo-", {force = true})
    add_shflags("/PDBALTPATH:%_PDB%", {force = true})
    add_cxflags("/FIno_source_paths.h", {force = true})
