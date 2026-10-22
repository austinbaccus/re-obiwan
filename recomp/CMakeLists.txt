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
    target_compile_options(guest PRIVATE /fp:precise /O2 /wd4102 /wd4146 /wd4334)
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND MSVC)
    # The harness proper, loaded at a normal address.
    add_library(difftest_core SHARED tests/difftest.c)
    target_link_libraries(difftest_core PRIVATE guest shell32)
    target_link_options(difftest_core PRIVATE /SAFESEH:NO)
    # The loader owns 0x00010000..0x0052D000 so the XBE can be mapped there.
    add_executable(difftest tests/loader.c)
    target_compile_options(difftest PRIVATE /GS- /O1)
    target_link_options(difftest PRIVATE /BASE:0x10000 /FIXED /DYNAMICBASE:NO /NODEFAULTLIB
        /ENTRY:loader_start /SUBSYSTEM:CONSOLE /STACK:0x800000 /SAFESEH:NO /OPT:NOREF)
    set_target_properties(difftest PROPERTIES LINK_LIBRARIES kernel32)
    add_dependencies(difftest difftest_core)
endif()
