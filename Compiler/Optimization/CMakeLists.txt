# Sources and dependencies owned by Optimization.

rux_add_component(RuxOptimization
    ConstantEvaluator.cpp
    HirConstantFolder.cpp
    LirCfgPasses.cpp
    LirConstantPropagation.cpp
    LirDeclarationPruner.cpp
    LirDeadCodeElimination.cpp
    LirMemoryPromotion.cpp
    LirInliner.cpp
    LirScalarCombines.cpp
    LirValueNumbering.cpp
    LirLoopInvariants.cpp
    LirSsaVerifier.cpp
    LirScalarReplacement.cpp
    LirReachability.cpp
    Pipeline.cpp)
target_link_libraries(RuxOptimization PUBLIC RuxBuildInfo RuxDiagnostics RuxHir RuxLir)
