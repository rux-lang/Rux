# Sources and dependencies owned by Lowering.

rux_add_component(RuxLowering
    AstToHir/AstToHir.cpp
    AstToHir/AstToHirAggregates.cpp
    AstToHir/AstToHirCalls.cpp
    AstToHir/CleanupPlanner.cpp
    AstToHir/AstToHirContext.cpp
    AstToHir/AstToHirDeclarations.cpp
    AstToHir/AstToHirExpressions.cpp
    AstToHir/AstToHirAllocationMath.cpp
    AstToHir/AstToHirZeroization.cpp
    AstToHir/AstToHirIteration.cpp
    AstToHir/AstToHirNative.cpp
    AstToHir/AstToHirPropagation.cpp
    AstToHir/AstToHirStatements.cpp
    AstToHir/AstToHirTypes.cpp
    HirToLir/CheckedLirBuilder.cpp
    HirToLir/HirToLirBacktrace.cpp
    HirToLir/HirToLirAggregates.cpp
    HirToLir/HirToLirCore.cpp
    HirToLir/HirToLirDrop.cpp
    HirToLir/HirToLirExpressions.cpp
    HirToLir/HirToLirSoftFloat.cpp
    HirToLir/HirToLirSoftFloatArithmetic.cpp
    HirToLir/HirToLirSoftFloatConvert.cpp
    HirToLir/HirToLirSoftFloatCore.cpp
    HirToLir/HirToLirSoftFloatDivide.cpp
    HirToLir/HirToLirSoftFloatMultiply.cpp
    HirToLir/HirToLirStatements.cpp)
target_link_libraries(RuxLowering PUBLIC
    RuxAst RuxSemanticModel RuxHir RuxLir RuxNumeric RuxOptimization RuxParallel RuxTarget RuxUnicode)
