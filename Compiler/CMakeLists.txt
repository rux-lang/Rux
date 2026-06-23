string(TIMESTAMP RUX_BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP RUX_BUILD_TIME "%H:%M:%S")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/Include/Rux/Version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/Rux/Version.h
    @ONLY
)

add_executable(rux
    Source/Main.cpp

    # Platform layer
    Source/Host.cpp

    # Command-line interface
    Source/Cli.cpp
    Source/Terminal.cpp
    Source/BuildReport.cpp
    Source/BuildTarget.cpp
    Source/Process.cpp
    Source/HelpCmd.cpp
    Source/BuildCmd.cpp
    Source/CheckCmd.cpp
    Source/InstallCmd.cpp
    Source/PackageCmd.cpp
    Source/UtilityCmd.cpp

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

target_include_directories(rux PRIVATE
    Include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

# Emit the binary to Bin/<Config>/ (e.g. Bin/Release/rux) in the source tree,
# keeping CMake intermediates in the build directory. $<CONFIG> resolves to the
# build type for both single- and multi-config generators. CMAKE_SOURCE_DIR is
# the repository root, so the binary lands in <repo>/Bin, not <repo>/Compiler.
set_target_properties(rux PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/Bin/$<CONFIG>)

if (MSVC OR (WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    set_property(TARGET rux PROPERTY
        MSVC_RUNTIME_LIBRARY
        "$<IF:$<CONFIG:Release>,MultiThreaded,MultiThreaded$<$<CONFIG:Debug>:Debug>DLL>")
endif ()
