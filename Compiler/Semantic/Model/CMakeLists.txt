# Sources and dependencies owned by Semantic/Model.

rux_add_component(RuxSemanticModel
    SemanticPrinter.cpp
    SemanticModel.cpp
)
target_link_libraries(RuxSemanticModel PUBLIC RuxTypes RuxAst RuxBuildInfo RuxDiagnostics)
