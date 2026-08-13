string(TIMESTAMP RUX_BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP RUX_BUILD_TIME "%H:%M:%S")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/BuildInfo/CompilerMetadata.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/BuildInfo/CompilerMetadata.h
    @ONLY
)

option(RUX_WERROR "Treat compiler warnings as errors" OFF)

# Warning flags shared by every Rux target. Interface-only, so consumers pick
# the flags up via target_link_libraries. The frontend variant (not the
# compiler id) selects the flag style. CMake calls these frontend variants
# MSVC for clang-cl and GNU for the ordinary clang++ driver.
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

function(rux_add_interface_component target)
    add_library(${target} INTERFACE)
    target_include_directories(${target} INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(${target} INTERFACE RuxWarnings)
endfunction()

rux_add_interface_component(RuxSourceModel)

rux_add_interface_component(RuxBuildInfo)

rux_add_component(RuxTarget
    Target/TargetTriple.cpp)
target_link_libraries(RuxTarget PUBLIC RuxSourceModel)

rux_add_component(RuxDiagnostics
    Diagnostics/Diagnostics.cpp)
target_link_libraries(RuxDiagnostics PUBLIC RuxSourceModel)

rux_add_component(RuxSource
    Source/SourceLoader.cpp)
target_link_libraries(RuxSource PUBLIC RuxDiagnostics)

rux_add_component(RuxSystem
    System/Host.cpp
    System/Json.cpp
    System/Os.cpp
    System/Process.cpp)
target_link_libraries(RuxSystem PUBLIC RuxTarget)
if (WIN32)
    target_link_libraries(RuxSystem PRIVATE winhttp)
endif ()

rux_add_component(RuxCrypto
    Crypto/Sha256.cpp)

rux_add_component(RuxPackage
    Package/Artifact.cpp
    Package/Checksum.cpp
    Package/Identity.cpp
    Package/Manifest.cpp
    Package/Package.cpp
    Package/Version.cpp)
target_link_libraries(RuxPackage PUBLIC RuxCrypto RuxTarget)

rux_add_component(RuxLexer
    Lexer/Token.cpp
    Lexer/Lexer.cpp)
target_link_libraries(RuxLexer PUBLIC RuxDiagnostics RuxSourceModel)

rux_add_component(RuxSyntax
    Syntax/Parser/Parser.cpp
    Syntax/Parser/ParserDecl.cpp
    Syntax/Parser/ParserType.cpp
    Syntax/Parser/ParserStmt.cpp
    Syntax/Parser/ParserExpr.cpp
    Syntax/Parser/ParserDump.cpp)
target_link_libraries(RuxSyntax PUBLIC RuxLexer RuxDiagnostics RuxTarget)

rux_add_component(RuxSemantic
    Semantic/ConditionalCompilation.cpp
    Semantic/PrimitiveConstants.cpp
    Semantic/Type.cpp
    Semantic/SemanticAnalyzer.cpp
    Semantic/SemanticPrinter.cpp)
target_link_libraries(RuxSemantic PUBLIC RuxBuildInfo RuxSyntax RuxDiagnostics)

rux_add_component(RuxFormatter
    Formatter/Formatter.cpp)
target_link_libraries(RuxFormatter PUBLIC RuxSyntax)

rux_add_component(RuxDocumentation
    Documentation/Generator.cpp)
target_link_libraries(RuxDocumentation PUBLIC RuxSyntax RuxPackage RuxDiagnostics)

rux_add_component(RuxLinter
    Linter/Linter.cpp)
target_link_libraries(RuxLinter PUBLIC RuxSyntax RuxSemantic)

rux_add_component(RuxHir
    Ir/Hir/HirDump.cpp)
target_link_libraries(RuxHir PUBLIC RuxSemantic RuxLexer RuxSourceModel RuxTarget)

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
target_link_libraries(RuxObjectRcu PUBLIC RuxBuildInfo RuxTarget)

rux_add_component(RuxCodeGenCommon
    CodeGen/Layout.cpp)
target_link_libraries(RuxCodeGenCommon PUBLIC RuxLir)

rux_add_component(RuxCodeGenX86_64
    CodeGen/X86_64/Assembler.cpp
    CodeGen/X86_64/AssemblyPrinter.cpp
    CodeGen/X86_64/RcuEmitter.cpp)
target_link_libraries(RuxCodeGenX86_64 PUBLIC RuxBuildInfo RuxCodeGenCommon RuxObjectRcu RuxDiagnostics)

rux_add_component(RuxArchive
    Linker/ArchiveWriter.cpp)
target_link_libraries(RuxArchive PUBLIC RuxObjectRcu)

rux_add_component(RuxCodeGenAArch64
    CodeGen/AArch64/Assembler.cpp
    CodeGen/AArch64/Encoder.cpp
    CodeGen/AArch64/RcuEmitter.cpp)
target_link_libraries(RuxCodeGenAArch64 PUBLIC RuxBuildInfo RuxCodeGenCommon RuxObjectRcu RuxDiagnostics)

rux_add_component(RuxLinker
    Linker/AArch64Relocation.cpp
    Linker/Linker.cpp
    Linker/MachO/CodeSignature.cpp
    Linker/NativeObjectWriter.cpp
    Linker/Pe/PeWriter.cpp
    Linker/Elf/ElfWriter.cpp
    Linker/MachO/MachOWriter.cpp)
target_link_libraries(RuxLinker PUBLIC RuxObjectRcu RuxSystem RuxArchive RuxCrypto)

rux_add_component(RuxDriver
    Driver/CompilerDriver.cpp
    Driver/BuildReport.cpp
    Driver/BuildTarget.cpp
    Driver/Credentials.cpp
    Driver/Registry.cpp)
target_link_libraries(RuxDriver PUBLIC
    RuxBuildInfo RuxSource RuxSystem RuxPackage RuxLexer RuxSyntax RuxSemantic
    RuxHir RuxHirPasses RuxLir RuxLowering RuxCodeGenX86_64 RuxCodeGenAArch64
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
    Cli/CmdAuth.cpp
    Cli/CmdBuild.cpp
    Cli/CmdCheck.cpp
    Cli/CmdLint.cpp
    Cli/CmdMisc.cpp
    Cli/CmdNew.cpp
    Cli/CmdPackage.cpp
    Cli/CmdPublish.cpp
    Cli/CmdRun.cpp
    Cli/CmdTest.cpp
)

target_link_libraries(rux PRIVATE RuxCore RuxFormatter RuxLinter RuxDocumentation)

# Emit the compiler directly to Bin/ in the source tree while keeping CMake
# intermediates in the build directory. Configuration-specific properties keep
# multi-config generators from appending Debug/ or Release/ subdirectories.
set_target_properties(rux PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/Bin
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/Bin
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/Bin
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/Bin
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_SOURCE_DIR}/Bin)

if (WIN32)
    set_property(TARGET rux RuxTarget RuxDiagnostics RuxSource RuxSystem RuxCrypto RuxPackage
        RuxLexer RuxSyntax RuxSemantic RuxFormatter RuxDocumentation RuxLinter RuxHir RuxHirPasses RuxLir
        RuxLowering RuxObjectRcu RuxCodeGenCommon RuxCodeGenX86_64 RuxCodeGenAArch64 RuxArchive
        RuxLinker RuxDriver PROPERTY
        MSVC_RUNTIME_LIBRARY
        "$<IF:$<CONFIG:Release>,MultiThreaded,MultiThreaded$<$<CONFIG:Debug>:Debug>DLL>")
endif ()

include(GNUInstallDirs)
install(TARGETS rux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
