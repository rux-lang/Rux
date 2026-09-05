#include "CodeGen/X86_64/ModuleEmitter.h"

namespace Rux::X86_64Detail {

// X86_64ModuleEmitter::Generate
RcuFile X86_64ModuleEmitter::Generate() {
    GenModule();
    auto built = moduleBuilder.Finalize();
    diagnostics.insert(diagnostics.end(), std::make_move_iterator(built.diagnostics.begin()),
                       std::make_move_iterator(built.diagnostics.end()));
    return built.file ? std::move(*built.file) : RcuFile{};
}

void X86_64ModuleEmitter::EmitStackAlloc(int32_t bytes) const {
    constexpr int32_t kPageSize = 4096;
    while (bytes > kPageSize) {
        enc.SubRspImm32(kPageSize);
        enc.TouchRsp();
        bytes -= kPageSize;
    }
    if (bytes > 0) {
        enc.SubRspImm32(bytes);
        if (bytes == kPageSize) {
            enc.TouchRsp();
        }
    }
}

// Resolve a symbol referenced from inline assembly to its symbol-table
// index: a local text symbol, module data/const, an already-declared
// extern, or a newly declared extern function.
uint32_t X86_64ModuleEmitter::ResolveAsmSymbol(const std::string &name) {
    if (const auto it = funcSyms.find(name); it != funcSyms.end()) {
        return it->second;
    }
    if (const auto it = dataSyms.find(name); it != dataSyms.end()) {
        return it->second;
    }
    if (const auto it = externSyms.find(name); it != externSyms.end()) {
        return it->second;
    }
    return GetOrAddExtern(name, RcuSymKind::ExternFunc);
}

// An `asm func` is emitted as a raw blob: no prologue, epilogue or frame.
// Its instructions are encoded directly and any symbol references become
// ordinary text relocations.
void X86_64ModuleEmitter::GenAsmFunc(const LirFunc &func) {
    const uint32_t symIdx = funcSyms.contains(func.name)
                              ? funcSyms.at(func.name)
                              : DeclareSymbol(func.name, func.returnType.ToString(), RcuSymKind::Func,
                                              func.isPublic ? RcuSymVis::Global : RcuSymVis::Local);
    funcSyms[func.name] = symIdx;
    if (!moduleBuilder.BeginFunction(symIdx)) {
        return;
    }

    AsmAssembly asmResult = AssembleAsmFunc(func.asmBody, mod.name, TextData(), targetOs);
    for (const auto &fixup : asmResult.fixups) {
        (void)moduleBuilder.AddRelocation(RcuModuleSection::Text, fixup.offset, ResolveAsmSymbol(fixup.symbol),
                                          fixup.relType, fixup.addend);
    }
    for (auto &diag : asmResult.diagnostics) {
        diagnostics.push_back(std::move(diag));
    }
    (void)moduleBuilder.EndFunction(symIdx);
}

// Function generation
void X86_64ModuleEmitter::GenFunc(const LirFunc &func) {
    if (func.isExtern) {
        GetOrAddExtern(func.name, RcuSymKind::ExternFunc, func.dll);
        return;
    }
    if (func.isAsm) {
        currentFunc = func.name;
        GenAsmFunc(func);
        currentFunc.clear();
        return;
    }
    currentFunc = func.name;
    const X86_64FramePlan framePlan = PlanX86_64Frame(func, layouts, interfaceNames, targetOs);
    activeFramePlan = &framePlan;
    X86_64CallEmitter callEmitter(enc, framePlan, targetOs, *this);
    X86_64TerminatorEmitter terminatorEmitter(enc, framePlan, *this);
    activeCallEmitter = &callEmitter;
    X86_64FunctionEmitter functionEmitter(enc, framePlan, layouts, interfaceNames, *this);
    const auto &usedPhysicalRegisters = framePlan.UsedPhysicalRegisters();
    const auto &physicalRegisters = framePlan.PhysicalRegisters();
    const uint32_t symIdx = funcSyms.contains(func.name)
                              ? funcSyms.at(func.name)
                              : DeclareSymbol(func.name, func.returnType.ToString(), RcuSymKind::Func,
                                              func.isPublic ? RcuSymVis::Global : RcuSymVis::Local);
    funcSyms[func.name] = symIdx;
    if (!moduleBuilder.BeginFunction(symIdx)) {
        activeCallEmitter = nullptr;
        activeFramePlan = nullptr;
        currentFunc.clear();
        return;
    }
    // Prologue
    enc.PushRbp();
    enc.MovRbpRsp();
    for (int rIdx : usedPhysicalRegisters) {
        enc.PushReg(rIdx);
    }
    const int32_t remainingFrame = framePlan.FrameSize() - static_cast<int32_t>(usedPhysicalRegisters.size() * 8);
    EmitStackAlloc(remainingFrame);
    // Spill ABI param registers to stack slots
    bool win64Func = EffectiveConv(func.callConv) == CallingConvention::Win64;
    int intIdx = 0, fltIdx = 0, sysvStackIdx = 0, win64Idx = 0;
    if (framePlan.HiddenReturnOffset() != 0) {
        if (win64Func) {
            enc.MovArgStoreWin64(0, -framePlan.HiddenReturnOffset());
            win64Idx = 1;
        }
        else {
            enc.MovArgStore(0, -framePlan.HiddenReturnOffset());
            intIdx = 1;
        }
    }
    for (const auto &p : func.params) {
        int sz = SizeOf(p.type);
        int32_t d = Disp(p.reg);
        if (win64Func) {
            // Win64: first 4 args are registers; the rest start
            // above return address + saved rbp + 32-byte home
            // space.
            if (win64Idx >= 4) {
                const int32_t stackArgOff = 48 + (win64Idx - 4) * 8;
                if (IsWin64AddressParam(p.type)) {
                    enc.MovRaxLoad(stackArgOff);
                    enc.MovRaxStore(d);
                }
                else if (IsWin64ByRefAggregate(p.type)) {
                    enc.MovR10Load(stackArgOff);
                    CopyAggregateFromR10ToStack(d, SizeOfRuntime(p.type));
                }
                else if (IsFloat(p.type)) {
                    if (sz == 4) {
                        enc.MovssXmm0Load(stackArgOff);
                        enc.MovssXmm0Store(d);
                    }
                    else {
                        enc.MovsdXmm0Load(stackArgOff);
                        enc.MovsdXmm0Store(d);
                    }
                }
                else {
                    enc.MovRaxLoad(stackArgOff);
                    StoreStack(p.reg, p.type);
                }
                ++win64Idx;
                continue;
            }
            if (IsWin64AddressParam(p.type)) {
                enc.MovRaxArgWin64(win64Idx);
                enc.MovRaxStore(d);
            }
            else if (IsWin64ByRefAggregate(p.type)) {
                enc.MovR10ArgWin64(win64Idx);
                CopyAggregateFromR10ToStack(d, SizeOfRuntime(p.type));
            }
            else if (IsFloat(p.type)) {
                // MOVSS/MOVSD [rbp+d], xmmN
                enc.Byte(sz == 4 ? 0xF3 : 0xF2);
                enc.Byte(0x0F);
                enc.Byte(0x11);
                enc.Byte(static_cast<uint8_t>(0x80 | (win64Idx << 3) | 5));
                enc.Dword(static_cast<uint32_t>(d));
            }
            else {
                enc.MovRaxArgWin64(win64Idx);
                StoreStack(p.reg, p.type);
            }
            ++win64Idx;
        }
        else {
            if (IsSysVMemoryAggregate(p.type)) {
                const int size = AlignUp(SizeOfRuntime(p.type), 8);
                for (int offset = 0; offset < size; offset += 8) {
                    enc.MovRaxLoad(16 + sysvStackIdx++ * 8);
                    enc.MovRaxStore(d + offset);
                }
            }
            else if (IsFloat(p.type)) {
                if (fltIdx < 8) {
                    // MOVSS/MOVSD [rbp+d], xmmN
                    enc.Byte(sz == 4 ? 0xF3 : 0xF2);
                    enc.Byte(0x0F);
                    enc.Byte(0x11);
                    enc.Byte(static_cast<uint8_t>(0x80 | (fltIdx << 3) | 5));
                    enc.Dword(static_cast<uint32_t>(d));
                    ++fltIdx;
                }
                else {
                    const int32_t stackArgOff = 16 + sysvStackIdx++ * 8;
                    if (sz == 4) {
                        enc.MovssXmm0Load(stackArgOff);
                        enc.MovssXmm0Store(d);
                    }
                    else {
                        enc.MovsdXmm0Load(stackArgOff);
                        enc.MovsdXmm0Store(d);
                    }
                }
            }
            // Measured as the running program lays it out, exactly as the caller measures it: SizeOf answers 8
            // for a named struct it has no layout for, and a callee classifying by that spilled one register of
            // a two-register aggregate — the second half of every by-value 16-byte struct arrived as garbage on
            // System V targets.
            else if (IsAggregate(p.type) && SizeOfRuntime(p.type) == 16) {
                if (intIdx <= 4) {
                    enc.MovArgStore(intIdx++, d);
                    enc.MovArgStore(intIdx++, d + 8);
                }
                else {
                    enc.MovRaxLoad(16 + sysvStackIdx++ * 8);
                    enc.MovRaxStore(d);
                    enc.MovRaxLoad(16 + sysvStackIdx++ * 8);
                    enc.MovRaxStore(d + 8);
                }
            }
            else {
                if (intIdx < 6) {
                    enc.MovArgStore(intIdx, d);
                    ++intIdx;
                }
                else {
                    const int32_t stackArgOff = 16 + sysvStackIdx++ * 8;
                    enc.MovRaxLoad(stackArgOff);
                    StoreStack(p.reg, p.type);
                }
            }
        }
    }
    // Load params into their allocated physical registers
    for (const auto &p : func.params) {
        auto it = physicalRegisters.find(p.reg);
        if (it != physicalRegisters.end()) {
            int sz = IsWin64AddressParam(p.type) ? 8 : SizeOfRuntime(p.type);
            int32_t d = Disp(p.reg);
            if (sz == 8 || sz == 0) {
                enc.MovRaxLoad(d);
            }
            else if (p.type.IsSigned()) {
                if (sz == 4)
                    enc.MovsxdRaxDword(d);
                else if (sz == 2)
                    enc.MovsxRaxWord(d);
                else
                    enc.MovsxRaxByte(d);
            }
            else {
                if (sz == 4)
                    enc.MovEaxLoad(d);
                else if (sz == 2)
                    enc.MovzxRaxWord(d);
                else
                    enc.MovzxRaxByte(d);
            }
            enc.MovPhysRegRax(it->second);
        }
    }
    // Basic blocks
    terminatorEmitter.Begin(func.blocks.size());
    for (uint32_t bi = 0; bi < func.blocks.size(); ++bi) {
        terminatorEmitter.MarkBlock(bi);
        const auto &block = func.blocks[bi];
        for (const auto &instr : block.instrs) {
            GenInstr(functionEmitter, callEmitter, instr);
        }
        if (block.term) {
            terminatorEmitter.Emit(bi, *block.term);
        }
    }
    terminatorEmitter.PatchJumps();
    (void)moduleBuilder.EndFunction(symIdx);
    activeCallEmitter = nullptr;
    activeFramePlan = nullptr;
    currentFunc.clear();
}
} // namespace Rux::X86_64Detail

namespace Rux {
using X86_64Detail::X86_64ModuleEmitter;

RcuFile GenerateRcuModule(const LirModule &mod, const std::vector<LirStructDecl> &structDecls,
                          const std::vector<std::string> &interfaceNames, const std::string &packageName,
                          const Target::OS targetOs, const BuildInfo &buildInfo, std::vector<Diagnostic> &diagnostics) {
    X86_64ModuleEmitter gen(mod, structDecls, interfaceNames, packageName, targetOs, buildInfo, diagnostics);
    return gen.Generate();
}

RcuEmitter::RcuEmitter(const LirPackage &package, std::string inputPackageName, const Target::OS inputTargetOs,
                       BuildInfo inputBuildInfo)
    : lir(package)
    , packageName(std::move(inputPackageName))
    , targetOs(inputTargetOs)
    , buildInfo(std::move(inputBuildInfo)) {
}

std::vector<RcuFile> RcuEmitter::Generate() const {
    std::vector<RcuFile> result;
    result.reserve(lir.modules.size());
    std::vector<LirStructDecl> structDecls;
    std::vector<std::string> interfaceNames;
    for (const auto &module : lir.modules) {
        structDecls.insert(structDecls.end(), module.structs.begin(), module.structs.end());
        interfaceNames.insert(interfaceNames.end(), module.interfaceNames.begin(), module.interfaceNames.end());
    }
    for (const auto &module : lir.modules) {
        result.push_back(
            GenerateRcuModule(module, structDecls, interfaceNames, packageName, targetOs, buildInfo, diagnostics));
    }
    return result;
}
} // namespace Rux
