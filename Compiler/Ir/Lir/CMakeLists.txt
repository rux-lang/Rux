# Sources and dependencies owned by Ir/Lir.

rux_add_component(RuxLir
    LirDump.cpp)
target_link_libraries(RuxLir PUBLIC RuxTypes RuxTarget)
