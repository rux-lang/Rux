# Sources and dependencies owned by Formatter.

rux_add_component(RuxFormatter
    Formatter.cpp)
target_link_libraries(RuxFormatter PUBLIC RuxSyntax)
