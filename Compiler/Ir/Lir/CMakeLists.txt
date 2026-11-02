# Sources and dependencies owned by Ir/Lir.

rux_add_component(RuxLir
    LirCheckedArithmetic.cpp
    LirDump.cpp
    LirVectors.cpp)
target_link_libraries(RuxLir PUBLIC RuxTypes RuxTarget)
