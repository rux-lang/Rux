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
    AstToHir/AstToHirPropagation.cpp
    AstToHir/AstToHirStatements.cpp
    AstToHir/AstToHirTypes.cpp
    HirToLir/CheckedLirBuilder.cpp
    HirToLir/HirToLirAggregates.cpp
    HirToLir/HirToLirCore.cpp
    HirToLir/HirToLirDrop.cpp
    HirToLir/HirToLirExpressions.cpp
    HirToLir/HirToLirStatements.cpp)
target_link_libraries(RuxLowering PUBLIC
    RuxAst RuxSemanticModel RuxHir RuxLir RuxUnicode)
