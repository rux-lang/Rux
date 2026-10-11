# Sources and dependencies owned by Optimization.

rux_add_component(RuxOptimization
    ConstantEvaluator.cpp
    HirConstantFolder.cpp
    LirCfgPasses.cpp
    LirConstantPropagation.cpp
    LirConstantMemory.cpp
    LirDeclarationPruner.cpp
    LirDeadCodeElimination.cpp
    LirMemoryPromotion.cpp
    LirInliner.cpp
    LirScalarCombines.cpp
    LirValueNumbering.cpp
    LirLoopInvariants.cpp
    LirLoopVersioning.cpp
    LirLoopUnrolling.cpp
    LirPointerInduction.cpp
    LirSsaVerifier.cpp
    LirScalarReplacement.cpp
    LirReachability.cpp
    LirViewParameters.cpp
    Pipeline.cpp)
target_link_libraries(RuxOptimization PUBLIC RuxBuildInfo RuxDiagnostics RuxHir RuxLir RuxUnicode)
