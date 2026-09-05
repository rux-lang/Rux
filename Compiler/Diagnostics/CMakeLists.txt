# Sources and dependencies owned by Diagnostics.

rux_add_component(RuxDiagnostics
    Diagnostics.cpp)
target_link_libraries(RuxDiagnostics PUBLIC RuxSourceModel PRIVATE RuxReporting)
