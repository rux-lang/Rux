# Sources and dependencies owned by CodeGen/AArch64.

rux_add_component(RuxCodeGenAArch64
    Assembler.cpp
    AssemblerBranchSystem.cpp
    AssemblerInteger.cpp
    AssemblerMemoryFloating.cpp
    AssemblerSupport.cpp
    CallAndTerminatorEmitter.cpp
    CallLayout.cpp
    Encoder.cpp
    EncoderFloating.cpp
    EncoderInteger.cpp
    EncoderMemory.cpp
    FramePlan.cpp
    FunctionEmitter.cpp
    SoftwareFloatEmitter.cpp
    WideIntegerEmitter.cpp
    ModuleData.cpp
    ModuleValues.cpp
    ModuleRuntimeFailure.cpp
    RcuEmitter.cpp)
target_link_libraries(RuxCodeGenAArch64 PUBLIC RuxBuildInfo RuxCodeGenCommon RuxObjectRcu RuxDiagnostics)
