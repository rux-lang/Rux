string(TIMESTAMP RUX_BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP RUX_BUILD_TIME "%H:%M:%S")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/Driver/Version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/Driver/Version.h
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

function(rux_add_component target)
    add_library(${target} STATIC ${ARGN})
    target_include_directories(${target} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(${target} PUBLIC RuxWarnings)
endfunction()

rux_add_component(RuxDiagnostics
    Diagnostics/Diagnostics.cpp)

rux_add_component(RuxSource
    Source/SourceLoader.cpp)
target_link_libraries(RuxSource PUBLIC RuxDiagnostics)

rux_add_component(RuxSystem
    System/Host.cpp
    System/Os.cpp
    System/Process.cpp)

rux_add_component(RuxPackage
    Package/Manifest.cpp
    Package/Package.cpp)

rux_add_component(RuxLexer
    Lexer/Token.cpp
    Lexer/Lexer.cpp)
target_link_libraries(RuxLexer PUBLIC RuxDiagnostics)

rux_add_component(RuxSyntax
    Syntax/Parser/Parser.cpp
    Syntax/Parser/ParserDecl.cpp
    Syntax/Parser/ParserType.cpp
    Syntax/Parser/ParserStmt.cpp
    Syntax/Parser/ParserExpr.cpp
    Syntax/Parser/ParserDump.cpp)
target_link_libraries(RuxSyntax PUBLIC RuxLexer RuxDiagnostics)

rux_add_component(RuxSemantic
    Semantic/Type.cpp
    Semantic/SemanticAnalyzer.cpp
    Semantic/SemanticPrinter.cpp)
target_link_libraries(RuxSemantic PUBLIC RuxSyntax RuxDiagnostics)

rux_add_component(RuxFormatter
    Formatter/Formatter.cpp)
target_link_libraries(RuxFormatter PUBLIC RuxSyntax)

rux_add_component(RuxLinter
    Linter/Linter.cpp)
target_link_libraries(RuxLinter PUBLIC RuxSyntax RuxSemantic)

rux_add_component(RuxHir
    Ir/Hir/HirDump.cpp)
target_link_libraries(RuxHir PUBLIC RuxSemantic RuxLexer)

rux_add_component(RuxHirPasses
    Ir/Hir/Passes/PassManager.cpp)
target_link_libraries(RuxHirPasses PUBLIC RuxHir)

rux_add_component(RuxLir
    Ir/Lir/LirDump.cpp)
target_link_libraries(RuxLir PUBLIC RuxSemantic)

rux_add_component(RuxLowering
    Lowering/AstToHir/AstToHir.cpp
    Lowering/HirToLir/HirToLir.cpp)
target_link_libraries(RuxLowering PUBLIC
    RuxSyntax RuxSemantic RuxHir RuxHirPasses RuxLir)

rux_add_component(RuxObjectRcu
    Object/Rcu/RcuDump.cpp
    Object/Rcu/RcuWriter.cpp)

rux_add_component(RuxCodeGenX86_64
    CodeGen/Layout.cpp
    CodeGen/X86_64/AssemblyPrinter.cpp
    CodeGen/X86_64/RcuEmitter.cpp)
target_link_libraries(RuxCodeGenX86_64 PUBLIC RuxLir RuxObjectRcu)

rux_add_component(RuxLinker
    Linker/Linker.cpp
    Linker/Pe/PeWriter.cpp
    Linker/Elf/ElfWriter.cpp
    Linker/MachO/MachOWriter.cpp)
target_link_libraries(RuxLinker PUBLIC RuxObjectRcu RuxSystem)

rux_add_component(RuxDriver
    Driver/CompilerDriver.cpp
    Driver/BuildReport.cpp
    Driver/BuildTarget.cpp)
target_link_libraries(RuxDriver PUBLIC
    RuxSource RuxSystem RuxPackage RuxLexer RuxSyntax RuxSemantic
    RuxHir RuxHirPasses RuxLir RuxLowering RuxCodeGenX86_64
    RuxObjectRcu RuxLinker)

# Compatibility aggregation target for embedders and existing tests. It owns
# no sources; dependency validation happens in the component targets above.
add_library(RuxCore INTERFACE)
target_link_libraries(RuxCore INTERFACE RuxDriver)

add_executable(rux
    Main.cpp
    Cli/Cli.cpp
    Cli/CliHelp.cpp
    Cli/TerminalStyle.cpp
    Cli/CmdBuild.cpp
    Cli/CmdCheck.cpp
    Cli/CmdLint.cpp
    Cli/CmdMisc.cpp
    Cli/CmdNew.cpp
    Cli/CmdPackage.cpp
    Cli/CmdRun.cpp
    Cli/CmdTest.cpp
)

target_link_libraries(rux PRIVATE RuxCore RuxFormatter RuxLinter)

# Emit the binary to Bin/<Config>/ (e.g. Bin/Release/rux) in the source tree,
# keeping CMake intermediates in the build directory. $<CONFIG> resolves to the
# build type for both single- and multi-config generators. CMAKE_SOURCE_DIR is
# the repository root, so the binary lands in <repo>/Bin, not <repo>/Compiler.
set_target_properties(rux PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/Bin/$<CONFIG>)

if (MSVC OR (WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    set_property(TARGET rux RuxDiagnostics RuxSource RuxSystem RuxPackage
        RuxLexer RuxSyntax RuxSemantic RuxFormatter RuxLinter RuxHir RuxHirPasses RuxLir
        RuxLowering RuxObjectRcu RuxCodeGenX86_64 RuxLinker RuxDriver PROPERTY
        MSVC_RUNTIME_LIBRARY
        "$<IF:$<CONFIG:Release>,MultiThreaded,MultiThreaded$<$<CONFIG:Debug>:Debug>DLL>")
endif ()

include(GNUInstallDirs)
install(TARGETS rux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
