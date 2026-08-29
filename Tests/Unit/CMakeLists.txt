# C++ unit tests for the compiler internals, built only when RUX_BUILD_TESTS
# is ON. Uses the vendored doctest single header (ThirdParty/doctest.h) and
# registers with CTest, so `ctest --test-dir <build>` runs everything.

set(RUX_TEST_SOURCES
    Main.cpp
    AArch64AssemblerTests.cpp
    AArch64CallLayoutTests.cpp
    AArch64EncoderTests.cpp
    AArch64FramePlanTests.cpp
    AArch64ObjectTests.cpp
    AArch64RelocationTests.cpp
    AArch64RcuAbiTests.cpp
    AArch64RcuAssemblyTests.cpp
    AArch64RcuControlFlowTests.cpp
    AArch64RcuDataTests.cpp
    AArch64RcuEmitterTestSupport.cpp
    AArch64RcuFloatingTests.cpp
    AArch64RcuFunctionImageTests.cpp
    AArch64RcuFrameTests.cpp
    AArch64RcuIntegerTests.cpp
    AArch64RcuRuntimeTests.cpp
    AArch64RcuVariadicAbiTests.cpp
    AArch64SoftwareFloatTests.cpp
    AArch64WideIntegerTests.cpp
    AllocationMathTests.cpp
    ArtifactTests.cpp
    BackendDiagnosticTests.cpp
    AsmParserTests.cpp
    BuiltinAssertTests.cpp
    BuiltinPanicTests.cpp
    BorrowCheckerTests.cpp
    BuildPlanTests.cpp
    BuildReportTests.cpp
    CheckedLirBuilderTests.cpp
    CodegenTests.cpp
    ChecksumTests.cpp
    CliContractTests.cpp
    CliProcessTests.cpp
    CliScaffoldProcessTests.cpp
    CmdInspectProcessTests.cpp
    CompilerDriverTests.cpp
    CompilerPipelineDiagnosticTests.cpp
    ConstantEvaluatorTests.cpp
    ConditionalCompilationTestSupport.cpp
    ConditionalEvaluationTests.cpp
    ConditionalFoldingTests.cpp
    CredentialsTests.cpp
    DiagnosticsTests.cpp
    DocumentationTests.cpp
    DropGlueLoweringTests.cpp
    EnumRepresentationTests.cpp
    FloatFormatTests.cpp
    GoldenDiagnosticsTests.cpp
    InspectionOutputTests.cpp
    InterfaceReferenceTests.cpp
    IterationLoweringTests.cpp
    JsonTests.cpp
    LayoutTests.cpp
    LexerTests.cpp
    LinearScanTests.cpp
    PrimitiveCatalogTests.cpp
    ReferenceTypeTests.cpp
    WideIntegerTests.cpp
    LinkerTests.cpp
    RcuLinkGraphTests.cpp
    RcuObjectLayoutTests.cpp
    FreeBSDAArch64LinkerTests.cpp
    MachOTests.cpp
    ManifestSyntaxTests.cpp
    ManifestValidationTests.cpp
    ManifestTests.cpp
    MoveStateTests.cpp
    MoveConsumptionTests.cpp
    OptimizerRegressionTests.cpp
    OperandWideningLoweringTests.cpp
    OptimizerTests.cpp
    OsTests.cpp
    PackageCacheTests.cpp
    PackageIdentityTests.cpp
    PackageResolutionTests.cpp
    PackageScaffoldTests.cpp
    PackageVersionTests.cpp
    ParserDiagnosticsTests.cpp
    ParserDumpTests.cpp
    PeLinkerTests.cpp
    PlatformAsmTests.cpp
    ProcessTests.cpp
    PropagationLoweringTests.cpp
    PrimitiveConstantsTests.cpp
    PublicationValidationTests.cpp
    ReporterTests.cpp
    ReportingTests.cpp
    RcuModuleBuilderTests.cpp
    RcuTests.cpp
    RuntimeFailureTests.cpp
    RegistryTests.cpp
    SemanticProgramIndexTests.cpp
    SemanticPropagationDiagnosticsTests.cpp
    SemanticConstraintDiagnosticsTests.cpp
    SemanticControlFlowDiagnosticsTests.cpp
    SemanticGenericInstantiationTests.cpp
    SemanticIterationDiagnosticsTests.cpp
    SemanticNameDiagnosticsTests.cpp
    SemanticOperatorAssignmentDiagnosticsTests.cpp
    SemanticTypeCallDiagnosticsTests.cpp
    SemanticTests.cpp
    SemanticVisibilityTests.cpp
    SemanticTypePropertyTests.cpp
    SoftwareFloatTests.cpp
    SourceLoaderTests.cpp
    SourceModelTests.cpp
    UserMessageProcessTests.cpp
    VariantEqualityTests.cpp
    VariantRepresentationTests.cpp
    ZeroizationTests.cpp
    X86_64AssemblyControlFlowPrinterTests.cpp
    X86_64AssemblyInstructionPrinterTests.cpp
    X86_64AssemblyModulePrinterTests.cpp
    X86_64FramePlanTests.cpp
    X86_64WideIntegerTests.cpp
    X86_64SoftwareFloatTests.cpp
)

if (TARGET RuxFormatter)
    list(APPEND RUX_TEST_SOURCES FormatterTests.cpp)
endif ()
if (TARGET RuxLinter)
    list(APPEND RUX_TEST_SOURCES LinterTests.cpp)
endif ()
add_executable(rux-tests ${RUX_TEST_SOURCES})
add_dependencies(rux-tests rux)

# Keep the unit-test executable with the other repository binaries. Explicit
# per-config properties prevent multi-config generators from adding profile
# subdirectories below Bin/Tests/Unit.
set_target_properties(rux-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/Bin/Tests/Unit
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/Bin/Tests/Unit
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/Bin/Tests/Unit
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/Bin/Tests/Unit
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_SOURCE_DIR}/Bin/Tests/Unit)

target_link_libraries(rux-tests PRIVATE RuxCore RuxCliContract RuxCliHelp RuxCliReporting RuxReporting)
if (TARGET RuxFormatter)
    target_link_libraries(rux-tests PRIVATE RuxFormatter)
endif ()
if (TARGET RuxLinter)
    target_link_libraries(rux-tests PRIVATE RuxLinter)
endif ()
if (TARGET RuxDocumentation)
    target_link_libraries(rux-tests PRIVATE RuxDocumentation)
endif ()

# SYSTEM keeps the project's warning flags (and -Werror in CI) from applying
# to the vendored doctest header.
target_include_directories(rux-tests SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty)

# By default doctest forward-declares std::ostream instead of including it, and
# only switches to the real headers when it detects libc++. On Windows the
# The Microsoft standard library declares operator<< for std::string_view in
# <string_view> but leaves std::ostream undefined, so any assertion doctest has
# to stringify a string_view for fails to compile. Asking doctest for the real
# <ostream> fixes that everywhere instead of one test file at a time.
target_compile_definitions(rux-tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)

# Two vendored-doctest warnings have to be suppressed on our own translation
# units. The RuxWarnings interface flags land after target options, so the
# suppressions are attached to the sources, whose options come last on the
# command line.
#
# -Wno-c2y-extensions: doctest's assertion macros expand __COUNTER__ inside our
# translation units, which -Wpedantic flags as a C2y extension under Clang.
#
# -Wno-#warnings: doctest.h includes <ciso646>, which libstdc++ 15 deprecated
# with a #warning directive. The SYSTEM include directory above does not help,
# because Clang emits #warning even from system headers.
set_source_files_properties(${RUX_TEST_SOURCES} PROPERTIES
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:Clang>:-Wno-c2y-extensions;-Wno-#warnings>")

# The golden diagnostics runner reads its cases from the source tree.
target_compile_definitions(rux-tests PRIVATE
    RUX_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/Tests/Unit/Golden"
    RUX_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    RUX_TESTS_DIR="${CMAKE_SOURCE_DIR}/Tests"
    RUX_PACKAGES_DIR="${CMAKE_SOURCE_DIR}/Packages"
    RUX_TEST_BIN_DIR="${CMAKE_SOURCE_DIR}/Bin/Tests")

# Match the static-CRT choice made for rux/RuxCore in Compiler/CMakeLists.txt;
# mixing runtimes across the RuxCore boundary breaks the link.
if (WIN32)
    set_property(TARGET rux-tests PROPERTY
        MSVC_RUNTIME_LIBRARY
        "$<IF:$<CONFIG:Release>,MultiThreaded,MultiThreaded$<$<CONFIG:Debug>:Debug>DLL>")
endif ()

add_test(NAME UnitTests COMMAND rux-tests)
