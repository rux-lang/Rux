# Sources and dependencies owned by Ir/Hir.

rux_add_component(RuxHir
    HirDump.cpp)
target_link_libraries(RuxHir PUBLIC RuxTypes RuxTokens RuxSourceModel RuxTarget)
