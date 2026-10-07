# Sources and dependencies owned by Documentation.

rux_add_component(RuxDocumentation
    Generator.cpp
    Install.cpp
    JsonExport.cpp
    Signatures.cpp)
target_link_libraries(RuxDocumentation PUBLIC RuxSyntax RuxPackage RuxDiagnostics)
