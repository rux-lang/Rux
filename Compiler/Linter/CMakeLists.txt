# Sources and dependencies owned by Linter.

rux_add_component(RuxLinter
    Linter.cpp)
target_link_libraries(RuxLinter PUBLIC RuxSyntax RuxSemantic)
