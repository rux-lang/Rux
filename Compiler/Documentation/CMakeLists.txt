# Sources and dependencies owned by Documentation.

rux_add_component(RuxDocumentation
    Generator.cpp
    Signatures.cpp)
target_link_libraries(RuxDocumentation PUBLIC RuxSyntax RuxPackage RuxDiagnostics)
