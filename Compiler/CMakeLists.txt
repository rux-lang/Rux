string(TIMESTAMP RUX_BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP RUX_BUILD_TIME "%H:%M:%S")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/Include/Rux/Version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/Rux/Version.h
    @ONLY
)

option(RUX_WERROR "Treat compiler warnings as errors" OFF)

# Warning flags shared by every Rux target. Interface-only, so consumers pick
# the flags up via target_link_libraries. The frontend variant (not the
# compiler id) selects the flag style, so clang-cl gets MSVC-style flags and
# clang++/g++ get GNU-style flags on every platform.
add_library(RuxWarnings INTERFACE)
target_compile_options(RuxWarnings INTERFACE
    $<$<STREQUAL:$<CXX_COMPILER_FRONTEND_VARIANT>,MSVC>:/W4 /permissive- /utf-8>
    $<$<STREQUAL:$<CXX_COMPILER_FRONTEND_VARIANT>,GNU>:-Wall -Wextra -Wpedantic -Wshadow>)
if (RUX_WERROR)
    target_compile_options(RuxWarnings INTERFACE
        $<IF:$<STREQUAL:$<CXX_COMPILER_FRONTEND_VARIANT>,MSVC>,/WX,-Werror>)
endif ()

# Everything except the CLI surface lives in RuxCore so tests can link the
# compiler internals without pulling in main()/command handling.
add_library(RuxCore STATIC
    # Platform layer
    Source/Host.cpp
    Source/Terminal.cpp
    Source/Process.cpp

    # Driver support
    Source/BuildReport.cpp
    Source/BuildTarget.cpp

    # Package / manifest handling
    Source/Manifest.cpp
    Source/Package.cpp

    # Compiler pipeline
    Source/Token.cpp
    Source/Lexer.cpp
    Source/SourceLoader.cpp
    Source/Parser.cpp
    Source/Sema.cpp
    Source/Hir.cpp
    Source/Lir.cpp
    Source/Optimizer.cpp
    Source/Asm.cpp
    Source/Rcu.cpp
    Source/Linker.cpp
)

target_include_directories(RuxCore PUBLIC
    Include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_link_libraries(RuxCore PUBLIC RuxWarnings)

add_executable(rux
    Source/Main.cpp
    Source/Cli.cpp
)

target_link_libraries(rux PRIVATE RuxCore)

# Emit the binary to Bin/<Config>/ (e.g. Bin/Release/rux) in the source tree,
# keeping CMake intermediates in the build directory. $<CONFIG> resolves to the
# build type for both single- and multi-config generators. CMAKE_SOURCE_DIR is
# the repository root, so the binary lands in <repo>/Bin, not <repo>/Compiler.
set_target_properties(rux PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/Bin/$<CONFIG>)

if (MSVC OR (WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    set_property(TARGET rux RuxCore PROPERTY
        MSVC_RUNTIME_LIBRARY
        "$<IF:$<CONFIG:Release>,MultiThreaded,MultiThreaded$<$<CONFIG:Debug>:Debug>DLL>")
endif ()

include(GNUInstallDirs)
install(TARGETS rux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
