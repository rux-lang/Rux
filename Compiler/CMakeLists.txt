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

rux_add_component(RuxNumeric
    Numeric/FloatEncoding.cpp
    Numeric/FloatFormat.cpp
    Numeric/FloatLiteral.cpp
    Numeric/FloatParsing.cpp
    Numeric/IntegerLiteral.cpp
    Numeric/SoftwareFloat.cpp
    Numeric/SoftwareFloatArithmetic.cpp
    Numeric/SoftwareFloatConversion.cpp
    Numeric/SoftwareFloatDivide.cpp
    Numeric/SoftwareFloatMultiply.cpp
    Numeric/WideInteger.cpp)

rux_add_component(RuxUnicode
    Unicode/Utf.cpp)

rux_add_component(RuxReporting
    Reporting/Reporting.cpp)

rux_add_component(RuxTarget
    Target/TargetTriple.cpp)
target_link_libraries(RuxTarget PUBLIC RuxSourceModel)

rux_add_component(RuxDiagnostics
    Diagnostics/Diagnostics.cpp)
target_link_libraries(RuxDiagnostics PUBLIC RuxSourceModel PRIVATE RuxReporting)

rux_add_component(RuxCliContract
    Cli/CliSpec.cpp)

rux_add_component(RuxCliHelp
    Cli/CliHelpRenderer.cpp)
target_link_libraries(RuxCliHelp PUBLIC RuxCliContract PRIVATE RuxDiagnostics RuxReporting)

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

rux_add_component(RuxCliReporting
    Cli/PublicationProblem.cpp
    Cli/Reporter.cpp
    Cli/TerminalStyle.cpp)
target_link_libraries(RuxCliReporting PUBLIC RuxReporting PRIVATE RuxSystem)

rux_add_component(RuxCrypto
    Crypto/Sha256.cpp)

rux_add_component(RuxPackage
    Package/Artifact.cpp
    Package/Checksum.cpp
    Package/Identity.cpp
    Package/Manifest.cpp
    Package/ManifestSerialization.cpp
    Package/ManifestSyntax.cpp
    Package/ManifestValidation.cpp
    Package/Package.cpp
    Package/PublicationValidation.cpp
    Package/Version.cpp)
target_link_libraries(RuxPackage PUBLIC RuxCrypto RuxTarget)

rux_add_component(RuxTypes
    Types/Type.cpp
    Types/PrimitiveCatalog.cpp
    Types/TypeProperties.cpp)
target_link_libraries(RuxTypes PUBLIC RuxTarget)

rux_add_component(RuxTokens Lexer/Token.cpp)
target_link_libraries(RuxTokens PUBLIC RuxSourceModel)

rux_add_component(RuxAst
    Syntax/DocumentationComment.cpp
    Syntax/DocumentationTags.cpp)
target_link_libraries(RuxAst PUBLIC RuxTokens RuxTarget)

rux_add_component(RuxSemanticModel
    Semantic/Model/SemanticModel.cpp
    Semantic/Model/PrimitiveConstants.cpp)
target_link_libraries(RuxSemanticModel PUBLIC RuxTypes RuxAst RuxBuildInfo RuxDiagnostics)

rux_add_component(RuxLexer
    Lexer/Lexer.cpp)
target_link_libraries(RuxLexer PUBLIC RuxDiagnostics RuxNumeric RuxTokens RuxSourceModel RuxUnicode)

rux_add_component(RuxSyntax
    Syntax/Parser/Parser.cpp
    Syntax/Parser/ParserAsm.cpp
    Syntax/Parser/ParserDecl.cpp
    Syntax/Parser/ParserDeclDispatch.cpp
    Syntax/Parser/ParserType.cpp
    Syntax/Parser/ParserStmt.cpp
    Syntax/Parser/ParserExpr.cpp
    Syntax/Parser/ParserDump.cpp
    Syntax/Parser/ParserDumpDecl.cpp
    Syntax/Parser/ParserDumpExpr.cpp
    Syntax/Parser/ParserDumpStmt.cpp)
target_link_libraries(RuxSyntax PUBLIC RuxAst RuxLexer RuxDiagnostics RuxTarget)

rux_add_component(RuxSemantic
    Semantic/Analysis/Imports.cpp
    Semantic/Analysis/TypeResolution.cpp
    Semantic/Analysis/ConstantChecking.cpp
    Semantic/Analysis/Assignments.cpp
    Semantic/Analysis/CallableResolution.cpp
    Semantic/Analysis/TypeLayout.cpp
    Semantic/Analysis/Declarations.cpp
    Semantic/Analysis/SymbolIdentities.cpp
    Semantic/Analysis/ExpressionDispatch.cpp
    Semantic/Analysis/GenericInstantiation.cpp
    Semantic/Conditional/ConditionalCompilation.cpp
    Semantic/Conditional/ConditionalDeclarationFolding.cpp
    Semantic/Conditional/ConditionalEvaluator.cpp
    Semantic/Conditional/ConditionalValueEvaluation.cpp
    Semantic/Conditional/ConditionalStatementFolding.cpp
    Semantic/Analysis/MoveStateTracker.cpp
    Semantic/Analysis/MovePlace.cpp
    Semantic/Analysis/TypePropertyClassifier.cpp
    Semantic/SemanticAnalyzer.cpp
    Semantic/Analysis/AnalysisContext.cpp
    Semantic/Analysis/AggregateExpressions.cpp
    Semantic/Analysis/AllocationMath.cpp
    Semantic/Analysis/Zeroization.cpp
    Semantic/Analysis/BasicExpressions.cpp
    Semantic/Analysis/BorrowState.cpp
    Semantic/Analysis/CallExpressions.cpp
    Semantic/Analysis/Constraints.cpp
    Semantic/Analysis/MoveState.cpp
    Semantic/Analysis/DropGlue.cpp
    Semantic/Analysis/FunctionSignatures.cpp
    Semantic/Analysis/Iteration.cpp
    Semantic/Analysis/Statements.cpp
    Semantic/Analysis/ProgramIndex.cpp
    Semantic/Analysis/Propagation.cpp
    Semantic/Analysis/PublicApi.cpp
    Semantic/Analysis/Receivers.cpp
    Semantic/Analysis/TypeProperties.cpp
    Semantic/Model/SemanticPrinter.cpp)
target_link_libraries(RuxSemantic PUBLIC RuxSemanticModel RuxNumeric RuxSyntax RuxDiagnostics)

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
target_link_libraries(RuxHir PUBLIC RuxTypes RuxTokens RuxSourceModel RuxTarget)

rux_add_component(RuxLir
    Ir/Lir/LirDump.cpp)
target_link_libraries(RuxLir PUBLIC RuxTypes RuxTarget)

rux_add_component(RuxOptimization
    Optimization/ConstantEvaluator.cpp
    Optimization/HirConstantFolder.cpp
    Optimization/LirCfgPasses.cpp
    Optimization/LirConstantPropagation.cpp
    Optimization/LirDeclarationPruner.cpp
    Optimization/LirDeadCodeElimination.cpp
    Optimization/LirReachability.cpp
    Optimization/Pipeline.cpp)
target_link_libraries(RuxOptimization PUBLIC RuxBuildInfo RuxDiagnostics RuxHir RuxLir)

rux_add_component(RuxLowering
    Lowering/AstToHir/AstToHir.cpp
    Lowering/AstToHir/AstToHirAggregates.cpp
    Lowering/AstToHir/AstToHirCalls.cpp
    Lowering/AstToHir/CleanupPlanner.cpp
    Lowering/AstToHir/AstToHirContext.cpp
    Lowering/AstToHir/AstToHirDeclarations.cpp
    Lowering/AstToHir/AstToHirExpressions.cpp
    Lowering/AstToHir/AstToHirAllocationMath.cpp
    Lowering/AstToHir/AstToHirZeroization.cpp
    Lowering/AstToHir/AstToHirIteration.cpp
    Lowering/AstToHir/AstToHirPropagation.cpp
    Lowering/AstToHir/AstToHirStatements.cpp
    Lowering/AstToHir/AstToHirTypes.cpp
    Lowering/HirToLir/CheckedLirBuilder.cpp
    Lowering/HirToLir/HirToLirAggregates.cpp
    Lowering/HirToLir/HirToLirCore.cpp
    Lowering/HirToLir/HirToLirDrop.cpp
    Lowering/HirToLir/HirToLirExpressions.cpp
    Lowering/HirToLir/HirToLirStatements.cpp)
target_link_libraries(RuxLowering PUBLIC
    RuxAst RuxSemanticModel RuxHir RuxLir RuxUnicode)

rux_add_component(RuxObjectRcu
    Object/Rcu/RcuDump.cpp
    Object/Rcu/RcuWriter.cpp)
target_link_libraries(RuxObjectRcu PUBLIC RuxBuildInfo RuxTarget)

rux_add_component(RuxCodeGenCommon
    CodeGen/Layout.cpp
    CodeGen/RcuModuleBuilder.cpp
    CodeGen/RuntimeFailure.cpp)
target_link_libraries(RuxCodeGenCommon PUBLIC RuxNumeric RuxLir RuxObjectRcu RuxDiagnostics RuxUnicode)

rux_add_component(RuxCodeGenX86_64
    CodeGen/X86_64/Assembler.cpp
    CodeGen/X86_64/AssemblerSupport.cpp
    CodeGen/X86_64/AssemblyControlFlowPrinter.cpp
    CodeGen/X86_64/AssemblyInstructionPrinter.cpp
    CodeGen/X86_64/AssemblyModulePrinter.cpp
    CodeGen/X86_64/AssemblyPrinter.cpp
    CodeGen/X86_64/CallAndTerminatorEmitter.cpp
    CodeGen/X86_64/Encoder.cpp
    CodeGen/X86_64/EncoderFloating.cpp
    CodeGen/X86_64/EncoderMemory.cpp
    CodeGen/X86_64/FramePlan.cpp
    CodeGen/X86_64/FunctionEmitter.cpp
    CodeGen/X86_64/WideIntegerEmitter.cpp
    CodeGen/X86_64/RcuEmitter.cpp
    CodeGen/X86_64/SoftwareFloatEmitter.cpp)
target_link_libraries(RuxCodeGenX86_64 PUBLIC RuxBuildInfo RuxCodeGenCommon RuxObjectRcu RuxDiagnostics)

rux_add_component(RuxArchive
    Linker/ArchiveWriter.cpp)
target_link_libraries(RuxArchive PUBLIC RuxObjectRcu)

rux_add_component(RuxCodeGenAArch64
    CodeGen/AArch64/Assembler.cpp
    CodeGen/AArch64/AssemblerBranchSystem.cpp
    CodeGen/AArch64/AssemblerInteger.cpp
    CodeGen/AArch64/AssemblerMemoryFloating.cpp
    CodeGen/AArch64/AssemblerSupport.cpp
    CodeGen/AArch64/CallAndTerminatorEmitter.cpp
    CodeGen/AArch64/CallLayout.cpp
    CodeGen/AArch64/Encoder.cpp
    CodeGen/AArch64/EncoderFloating.cpp
    CodeGen/AArch64/EncoderInteger.cpp
    CodeGen/AArch64/EncoderMemory.cpp
    CodeGen/AArch64/FramePlan.cpp
    CodeGen/AArch64/FunctionEmitter.cpp
    CodeGen/AArch64/SoftwareFloatEmitter.cpp
    CodeGen/AArch64/WideIntegerEmitter.cpp
    CodeGen/AArch64/RcuEmitter.cpp)
target_link_libraries(RuxCodeGenAArch64 PUBLIC RuxBuildInfo RuxCodeGenCommon RuxObjectRcu RuxDiagnostics)

rux_add_component(RuxLinker
    Linker/AArch64Relocation.cpp
    Linker/Coff/CoffObjectWriter.cpp
    Linker/Elf/ElfObjectWriter.cpp
    Linker/Linker.cpp
    Linker/RcuLinkGraph.cpp
    Linker/RcuObjectLayout.cpp
    Linker/MachO/CodeSignature.cpp
    Linker/MachO/MachOLayout.cpp
    Linker/MachO/MachOLinkEdit.cpp
    Linker/MachO/MachOObjectWriter.cpp
    Linker/NativeObjectWriter.cpp
    Linker/Pe/PeWriter.cpp
    Linker/Elf/ElfWriter.cpp
    Linker/MachO/MachOWriter.cpp)
target_link_libraries(RuxLinker PUBLIC RuxObjectRcu RuxSystem RuxArchive RuxCrypto)

rux_add_component(RuxDriver
    Driver/CompilerDriver.cpp
    Driver/BuildPlan.cpp
    Driver/BuildReport.cpp
    Driver/BuildTarget.cpp
    Driver/Credentials.cpp
    Driver/PackageResolution.cpp
    Driver/Registry.cpp)
target_link_libraries(RuxDriver PUBLIC
    RuxBuildInfo RuxSource RuxSystem RuxPackage RuxLexer RuxSyntax RuxSemantic
    RuxHir RuxLir RuxLowering RuxOptimization RuxCodeGenX86_64 RuxCodeGenAArch64
    RuxObjectRcu RuxLinker
    PRIVATE RuxReporting)

# Compatibility aggregation target for embedders and existing tests. It owns
# no sources; dependency validation happens in the component targets above.
add_library(RuxCore INTERFACE)
target_link_libraries(RuxCore INTERFACE RuxDriver)

add_executable(rux
    Main.cpp
    Cli/Cli.cpp
    Cli/CliHelp.cpp
    Cli/CmdAuth.cpp
    Cli/CmdBuild.cpp
    Cli/CmdCheck.cpp
    Cli/CmdDependency.cpp
    Cli/CmdInspect.cpp
    Cli/CmdInstall.cpp
    Cli/CmdLint.cpp
    Cli/CmdMisc.cpp
    Cli/CmdNew.cpp
    Cli/CmdPublish.cpp
    Cli/CmdRun.cpp
    Cli/CmdTest.cpp
)

target_link_libraries(rux PRIVATE
    RuxCore RuxFormatter RuxLinter RuxDocumentation RuxCliContract RuxCliHelp RuxCliReporting RuxReporting)

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
    set_property(TARGET rux RuxTarget RuxDiagnostics RuxCliContract RuxCliHelp RuxCliReporting RuxReporting RuxSource
        RuxSystem RuxCrypto
        RuxPackage
        RuxTypes RuxTokens RuxAst RuxSemanticModel
        RuxNumeric RuxUnicode RuxLexer RuxSyntax RuxSemantic RuxFormatter RuxDocumentation RuxLinter RuxHir RuxLir
        RuxOptimization
        RuxLowering RuxObjectRcu RuxCodeGenCommon RuxCodeGenX86_64 RuxCodeGenAArch64 RuxArchive
        RuxLinker RuxDriver PROPERTY
        MSVC_RUNTIME_LIBRARY
        "$<IF:$<CONFIG:Release>,MultiThreaded,MultiThreaded$<$<CONFIG:Debug>:Debug>DLL>")
endif ()

include(GNUInstallDirs)
install(TARGETS rux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
