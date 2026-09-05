# Sources and dependencies owned by Target.

rux_add_component(RuxTarget
    TargetTriple.cpp)
target_link_libraries(RuxTarget PUBLIC RuxSourceModel)
