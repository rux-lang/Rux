cmake_minimum_required(VERSION 3.31)
project(Rux VERSION 0.2.3)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

string(TIMESTAMP RUX_BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP RUX_BUILD_TIME "%H:%M:%S")

configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/Include/Rux/Version.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/generated/Rux/Version.h
        @ONLY
)

add_executable(rux
        Source/Main.cpp
        Source/Cli.cpp
        Source/Manifest.cpp
        Source/Package.cpp
        Source/Token.cpp
        Source/Lexer.cpp
        Source/SourceLoader.cpp
        Source/Parser.cpp
        Source/Sema.cpp
        Source/Hir.cpp
        Source/Lir.cpp
        Source/SsaConstruct.cpp
        Source/CopyProp.cpp
        Source/ConstFold.cpp
        Source/Asm.cpp
        Source/Rcu.cpp
        Source/Linker.cpp
        Include/Rux/Cli.h
        Include/Rux/Manifest.h
        Include/Rux/Package.h
        Include/Rux/Token.h
        Include/Rux/Lexer.h
        Include/Rux/SourceLoader.h
        Include/Rux/Ast.h
        Include/Rux/Parser.h
        Include/Rux/Type.h
        Include/Rux/Sema.h
        Include/Rux/Hir.h
        Include/Rux/Lir.h
        Include/Rux/SsaConstruct.h
        Include/Rux/CopyProp.h
        Include/Rux/ConstFold.h
        Include/Rux/Asm.h
        Include/Rux/Rcu.h
        Include/Rux/Linker.h
)

target_include_directories(rux PRIVATE
        Include
        ${CMAKE_CURRENT_BINARY_DIR}/generated
)

if (WIN32)
    target_link_libraries(rux PRIVATE Psapi Winhttp)
endif ()

set_property(TARGET rux
        PROPERTY MSVC_RUNTIME_LIBRARY "$<IF:$<CONFIG:Release>,MultiThreaded,MultiThreadedDebugDLL>"
)

include(GNUInstallDirs)
install(TARGETS rux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ── CTest integration ─────────────────────────────────────────────────────────
# Tests that invoke shell scripts must have the `rux` binary available on PATH.
# Each add_test wraps the corresponding Tests/run_*.sh script so `ctest` and
# CMake Presets can drive the full test suite.
#
# Note: Rux currently targets linux-x64 and windows-x64.  On other hosts
# (macOS, etc.) the build-and-run scripts will report a target error;
# the CMake tests still register so CI matrices can skip or expect-fail them.
enable_testing()

add_test(
    NAME regalloc_loops
    COMMAND ${CMAKE_COMMAND} -E env RUX=$<TARGET_FILE:rux>
            sh ${CMAKE_CURRENT_SOURCE_DIR}/Tests/run_regalloc_tests.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(
    NAME io_thunks
    COMMAND ${CMAKE_COMMAND} -E env RUX=$<TARGET_FILE:rux>
            sh ${CMAKE_CURRENT_SOURCE_DIR}/Tests/run_io_test.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(
    NAME pow_operator
    COMMAND ${CMAKE_COMMAND} -E env RUX=$<TARGET_FILE:rux>
            sh ${CMAKE_CURRENT_SOURCE_DIR}/Tests/run_pow_test.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# SSA construction pass tests — verify Cytron et al. 1991 pass properties:
#   - promotable alloca/load/store triples are eliminated
#   - phi nodes appear at correct merge/loop-header blocks
#   - single-definition invariant holds within each function
# These tests use --dump-lir and inspect the LIR text; they do NOT run the
# produced binary, so they execute on all host platforms.
add_test(
    NAME ssa_construct
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/Tests/run_ssa_tests.sh
                 $<TARGET_FILE:rux>
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
