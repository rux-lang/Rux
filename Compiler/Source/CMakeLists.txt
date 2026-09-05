# Sources and dependencies owned by Source.

rux_add_component(RuxSource
    SourceLoader.cpp)
target_link_libraries(RuxSource PUBLIC RuxDiagnostics)
