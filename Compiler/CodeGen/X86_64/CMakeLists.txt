# Sources and dependencies owned by CodeGen/X86_64.

rux_add_component(RuxCodeGenX86_64
    AssemblerEncoding.cpp
    AssemblerInteger.cpp
    AssemblerFloating.cpp
    Assembler.cpp
    AssemblerSupport.cpp
    AssemblyControlFlowPrinter.cpp
    AssemblyInstructionPrinter.cpp
    AssemblyModulePrinter.cpp
    AssemblyPrinter.cpp
    CallAndTerminatorEmitter.cpp
    Encoder.cpp
    EncoderFloating.cpp
    EncoderMemory.cpp
    FramePlan.cpp
    FunctionEmitter.cpp
    WideIntegerEmitter.cpp
    ModuleData.cpp
    ModuleValues.cpp
    ModuleRuntimeFailure.cpp
    RcuEmitter.cpp
    SoftwareFloatEmitter.cpp)
target_link_libraries(RuxCodeGenX86_64 PUBLIC RuxBuildInfo RuxCodeGenCommon RuxObjectRcu RuxDiagnostics)
