string(TIMESTAMP RUX_BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP RUX_BUILD_TIME "%H:%M:%S")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/Support/Version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/Support/Version.h
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
    # Shared support
    Support/Diagnostics.cpp

    # Platform layer
    Platform/Host.cpp
    Platform/Process.cpp
    Platform/Terminal.cpp

    # Driver: compile pipeline orchestration
    Driver/Driver.cpp
    Driver/BuildReport.cpp
    Driver/BuildTarget.cpp

    # Package / manifest handling
    Package/Manifest.cpp
    Package/Package.cpp

    # Frontend: lexing, parsing, semantic analysis
    Frontend/Token.cpp
    Frontend/Lexer.cpp
    Frontend/SourceLoader.cpp
    Frontend/Parser/Parser.cpp
    Frontend/Parser/ParserDecl.cpp
    Frontend/Parser/ParserType.cpp
    Frontend/Parser/ParserStmt.cpp
    Frontend/Parser/ParserExpr.cpp
    Frontend/Parser/ParserDump.cpp
    Frontend/Sema/Type.cpp
    Frontend/Sema/Sema.cpp
    Frontend/Sema/SemaDump.cpp

    # Intermediate representations and optimization
    Ir/Hir/Hir.cpp
    Ir/Hir/HirDump.cpp
    Ir/Lir/Lir.cpp
    Ir/Lir/LirDump.cpp
    Ir/Optimizer.cpp

    # Backend: code generation, object files, linking
    Backend/Layout.cpp
    Backend/X64/Asm.cpp
    Backend/Rcu/Rcu.cpp
    Backend/Rcu/RcuCodeGen.cpp
    Backend/Rcu/RcuDump.cpp
    Backend/Rcu/RcuWriter.cpp
    Backend/Link/Linker.cpp
    Backend/Link/PeWriter.cpp
    Backend/Link/ElfWriter.cpp
    Backend/Link/MachOWriter.cpp
)

target_include_directories(RuxCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_link_libraries(RuxCore PUBLIC RuxWarnings)

add_executable(rux
    Main.cpp
    Cli/Cli.cpp
    Cli/CliHelp.cpp
    Cli/CmdBuild.cpp
    Cli/CmdCheck.cpp
    Cli/CmdMisc.cpp
    Cli/CmdNew.cpp
    Cli/CmdPackage.cpp
    Cli/CmdRun.cpp
    Cli/CmdTest.cpp
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
