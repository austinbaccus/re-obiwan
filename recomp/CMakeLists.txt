cmake_minimum_required(VERSION 3.24)
project(obiwan_recomp C)

# Generated C from the translator (never committed; see README.md).
set(RECOMP_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/generated" CACHE PATH
    "Directory with funcs_*.c, funcs.h and functab.c from pcode2c.py")

file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS "${RECOMP_GENERATED_DIR}/funcs_*.c")
if(NOT GENERATED_SOURCES)
    message(FATAL_ERROR "No generated sources in ${RECOMP_GENERATED_DIR}; run the translator first.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(guest STATIC
    runtime/runtime.c
    ${GENERATED_SOURCES}
    "${RECOMP_GENERATED_DIR}/functab.c")
target_include_directories(guest PUBLIC runtime "${RECOMP_GENERATED_DIR}")
if(MSVC)
    # /fp:precise keeps float and double arithmetic in their own precision;
    # 4102: unreferenced labels are normal in generated code.
    target_compile_options(guest PRIVATE /fp:precise /O2 /wd4102 /wd4146 /wd4334 /wd4996)
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND MSVC)
    # The harness proper, loaded at a normal address.
    # hooks_xapi/kernel satisfy the hook_* symbols the generated code references
    set(RENDER_SOURCES host/render.c host/render_common.c host/render_d3d9.c host/render_d3d11.c host/shader_xlate.c)
    set(RENDER_LIBS d3d9 d3d11 dxgi)
    add_library(difftest_core SHARED tests/difftest.c host/hooks_xapi.c host/hooks_d3d.c ${RENDER_SOURCES} host/hooks_dsound.c host/audio.c host/kernel.c
                host/native.c host/debugkeys.c)
    target_include_directories(difftest_core PRIVATE host)
    target_compile_options(difftest_core PRIVATE /wd4996)
    target_link_libraries(difftest_core PRIVATE guest shell32 user32 ${RENDER_LIBS})
    target_link_options(difftest_core PRIVATE /SAFESEH:NO)
    # The loader owns 0x00010000..0x0052D000 so the XBE can be mapped there.
    add_executable(difftest tests/loader.c)
    target_compile_options(difftest PRIVATE /GS- /O1)
    target_link_options(difftest PRIVATE /BASE:0x10000 /FIXED /DYNAMICBASE:NO /NODEFAULTLIB
        /ENTRY:loader_start /SUBSYSTEM:CONSOLE /STACK:0x4000000 /SAFESEH:NO /OPT:NOREF)
    set_target_properties(difftest PROPERTIES LINK_LIBRARIES kernel32)
    add_dependencies(difftest difftest_core)

    # Offline check of the shader translator: xs_check.exe vs_*.txt (shader sources dumped from the XBE)
    add_executable(xs_check tests/xs_check.c host/shader_xlate.c)
    target_compile_options(xs_check PRIVATE /wd4996)

    # The game itself: host kernel + hooks + recompiled code, entered the same way.
    add_library(obiwan_core SHARED host/boot.c host/native.c host/widescreen.c host/debugkeys.c host/kernel.c host/hooks_xapi.c host/hooks_d3d.c ${RENDER_SOURCES} host/hooks_dsound.c host/audio.c runtime/xbe_image.c)
    target_include_directories(obiwan_core PRIVATE host)
    target_link_libraries(obiwan_core PRIVATE guest shell32 user32 dbghelp ${RENDER_LIBS})
    target_compile_options(obiwan_core PRIVATE /wd4996)
    target_link_options(obiwan_core PRIVATE /SAFESEH:NO)
    add_executable(obiwan tests/loader.c)
    target_compile_definitions(obiwan PRIVATE LOADER_DLL="obiwan_core.dll" LOADER_ENTRY="game_main" LOADER_RESERVE=0x3FED000)
    target_compile_options(obiwan PRIVATE /GS- /O1)
    target_link_options(obiwan PRIVATE /BASE:0x10000 /FIXED /DYNAMICBASE:NO /NODEFAULTLIB
        /ENTRY:loader_start /SUBSYSTEM:CONSOLE /STACK:0x4000000 /SAFESEH:NO /OPT:NOREF /LARGEADDRESSAWARE)
    set_target_properties(obiwan PROPERTIES LINK_LIBRARIES kernel32)
    add_dependencies(obiwan obiwan_core)
    # The same game as a windowed program, for a double-click: Obi-Wan.exe
    # (obiwan.ini settings, obiwan.log, message boxes; see boot.c)
    add_executable(obiwan_windowed tests/loader.c)
    target_compile_definitions(obiwan_windowed PRIVATE LOADER_DLL="obiwan_core.dll" LOADER_ENTRY="game_main_windowed" LOADER_RESERVE=0x3FED000)
    target_compile_options(obiwan_windowed PRIVATE /GS- /O1)
    target_link_options(obiwan_windowed PRIVATE /BASE:0x10000 /FIXED /DYNAMICBASE:NO /NODEFAULTLIB
        /ENTRY:loader_start /SUBSYSTEM:WINDOWS /STACK:0x4000000 /SAFESEH:NO /OPT:NOREF /LARGEADDRESSAWARE)
    set_target_properties(obiwan_windowed PROPERTIES LINK_LIBRARIES kernel32 OUTPUT_NAME "Obi-Wan")
    add_dependencies(obiwan_windowed obiwan_core)
    add_dependencies(obiwan obiwan_windowed) # building the game builds both

    # Program icon: one of the game's HUD portraits, converted at build time
    # (nothing from the disc is committed). Without the extracted files the
    # programs build with Windows' default icon.
    set(OBIWAN_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../extracted/data/interface/hud/HUD_enemyQuiGon.dds" CACHE FILEPATH
        "DXT texture for the program icon (e.g. HUD_enemyDarthMaul.dds)")
    if(EXISTS "${OBIWAN_ICON}")
        add_executable(dds_icon tests/dds_icon.c)
        set(ICON_FILE "${CMAKE_CURRENT_BINARY_DIR}/obiwan.ico")
        add_custom_command(OUTPUT "${ICON_FILE}" COMMAND dds_icon "${OBIWAN_ICON}" "${ICON_FILE}"
                           DEPENDS dds_icon "${OBIWAN_ICON}" VERBATIM)
        add_custom_target(obiwan_icon DEPENDS "${ICON_FILE}")
        set(ICON_RC "${CMAKE_CURRENT_BINARY_DIR}/obiwan_icon.rc")
        file(WRITE "${ICON_RC}" "1 ICON \"${ICON_FILE}\"\n") # resource 1: also the window's icon (render_*.c)
        set_source_files_properties("${ICON_RC}" PROPERTIES OBJECT_DEPENDS "${ICON_FILE}")
        foreach(t obiwan obiwan_windowed)
            target_sources(${t} PRIVATE "${ICON_RC}")
            add_dependencies(${t} obiwan_icon)
        endforeach()
    else()
        message(STATUS "No ${OBIWAN_ICON}: the programs get the default icon")
    endif()
endif()
