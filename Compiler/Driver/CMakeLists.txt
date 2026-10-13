# Sources and dependencies owned by Driver.

rux_add_component(RuxDriverModel
    BuildStats.cpp
    CompileEvents.cpp)
target_link_libraries(RuxDriverModel PUBLIC RuxBuildInfo RuxTarget RuxTokens)

rux_add_component(RuxDriver
    BuildCache.cpp
    CompilerDriver.cpp
    DependencyGraph.cpp
    BuildPlan.cpp
    BuildTarget.cpp
    TestEntry.cpp
)
target_link_libraries(RuxDriver PUBLIC RuxDriverModel RuxDiagnostics RuxPackage RuxAst
    PRIVATE RuxBuildInfo RuxSource RuxSystem RuxPackageCache RuxLexer RuxSyntax RuxSemantic
    RuxHir RuxLir RuxLowering RuxOptimization RuxCodeGenX86_64 RuxCodeGenAArch64
    RuxObjectRcu RuxLinker RuxParallel RuxCrypto)
