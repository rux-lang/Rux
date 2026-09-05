# Sources and dependencies owned by CodeGen.

rux_add_component(RuxCodeGenCommon
    Layout.cpp
    RcuModuleBuilder.cpp
    RuntimeFailure.cpp)
target_link_libraries(RuxCodeGenCommon PUBLIC RuxNumeric RuxLir RuxObjectRcu RuxDiagnostics RuxUnicode)
