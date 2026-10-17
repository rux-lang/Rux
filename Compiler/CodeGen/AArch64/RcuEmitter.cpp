#include "CodeGen/AArch64/MachineEmission.h"
#include "CodeGen/AArch64/MachineSelection.h"
#include "CodeGen/AArch64/ModuleEmitter.h"
#include "Ir/Lir/LirVectors.h"

#include <algorithm>

namespace Rux::AArch64Detail {

RcuFile AArch64ModuleEmitter::Generate() {
    GenModule();
    auto built = moduleBuilder.Finalize();
    diagnostics.insert(diagnostics.end(), std::make_move_iterator(built.diagnostics.begin()),
                       std::make_move_iterator(built.diagnostics.end()));
    return built.file ? std::move(*built.file) : RcuFile{};
}

void AArch64ModuleEmitter::GenInstr(AArch64FunctionEmitter &functionEmitter, AArch64CallEmitter &callEmitter,
                                    AArch64TerminatorEmitter &terminatorEmitter, const LirInstr &instr) {
    if (functionEmitter.EmitArithmetic(instr) || functionEmitter.EmitMemory(instr) ||
        functionEmitter.EmitAtomic(instr) || functionEmitter.EmitThreadLocal(instr) || callEmitter.Emit(instr)) {
        return;
    }
    switch (instr.op) {
    case LirOpcode::Assert:
    case LirOpcode::Panic:
        GenAssert(terminatorEmitter, instr);
        break;
    case LirOpcode::FramePointer:
        StoreToSlot(A64::Fp, instr.dst, instr.type);
        break;
    case LirOpcode::WriteStandardError:
        EmitWriteStandardError(instr);
        break;
    case LirOpcode::Phi:
        // Nothing: the value arrived in this register's slot along the edge
        // that reached this block, written by the predecessor's terminator.
        break;
    default:
        Report(UnsupportedLirDiagnostic(instr.op, targetOs, Target::Arch::AArch64, currentFunc));
        break;
    }
}

// Inline assembly
//
// An `asm func` is the one body this generator does not select instructions
// for: the program wrote them down, and CodeGen/AArch64/Assembler.cpp
// encodes exactly those. What arrives back is machine code and the
// references it could not settle on its own — a branch to another function,
// the address of a global — which become ordinary text relocations here.

// The symbol an `asm func` names: a function of this module, a constant or
// global of it, an extern already declared, or, failing all three, an
// extern function this reference declares.
std::uint32_t AArch64ModuleEmitter::ResolveAsmSymbol(const std::string &name) {
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

// A raw blob: no prologue, no epilogue and no frame, because a body written
// in assembly has already said what it does with the stack. Its arguments
// are wherever AAPCS64 left them and its result is wherever it puts one.
void AArch64ModuleEmitter::GenAsmFunc(const LirFunc &func) {
    const std::uint32_t symIdx = funcSyms.contains(func.name)
                                   ? funcSyms.at(func.name)
                                   : DeclareSymbol(func.name, func.returnType.ToString(), RcuSymKind::Func,
                                                   RcuSymVis::ForDefinition(func.isPublic, func.isExported));
    funcSyms[func.name] = symIdx;
    if (!moduleBuilder.BeginFunction(symIdx)) {
        return;
    }

    AsmAssembly assembled = AssembleAArch64AsmFunc(func.asmBody, mod.name, TextData(), targetOs);
    for (const auto &fixup : assembled.fixups) {
        (void)moduleBuilder.AddRelocation(RcuModuleSection::Text, fixup.offset, ResolveAsmSymbol(fixup.symbol),
                                          fixup.relType, fixup.addend);
    }
    for (auto &diagnostic : assembled.diagnostics) {
        diagnostics.push_back(std::move(diagnostic));
    }
    (void)moduleBuilder.EndFunction(symIdx);
}

void AArch64ModuleEmitter::GenFunc(const LirFunc &func) {
    if (func.isExtern) {
        GetOrAddExtern(func.name, RcuSymKind::ExternFunc, ExternLibrary(func));
        return;
    }
    currentFunc = func.name;
    if (func.isAsm) {
        GenAsmFunc(func);
        currentFunc.clear();
        return;
    }
    // This back end selects no vectors: it emits the scalar instructions they stand for.
    if (HasVectorInstructions(func)) {
        LirFunc scalar = func;
        Devectorize(scalar);
        GenFunc(scalar);
        return;
    }
    if (machineSelection && GenMachineFunc(func)) {
        currentFunc.clear();
        return;
    }
    const AArch64FramePlan framePlan = PlanAArch64Frame(func, layouts, interfaceNames, structDecls, targetOs);
    activeFramePlan = &framePlan;
    AArch64FunctionEmitter functionEmitter(enc, framePlan, layouts, interfaceNames, currentFunc, targetOs, *this);
    AArch64CallEmitter callEmitter(enc, framePlan, callPlanner, currentFunc, *this);
    AArch64TerminatorEmitter terminatorEmitter(enc, framePlan, callEmitter, currentFunc, *this);
    terminatorEmitter.BeginFunction();

    // The body is emitted, and emitted again if any conditional branch in it
    // turned out not to reach its target: which form such a branch takes has
    // to be chosen before the offsets that decide it are known, so the
    // choice is corrected rather than guessed. Each pass widens every site
    // the pass before it found short and never narrows one back, so the
    // number of passes is bounded by the number of branches, and a function
    // small enough to have none — which is very nearly all of them — is
    // emitted exactly once.
    const std::uint32_t funcStart = enc.Size();
    const std::size_t relocCount = moduleBuilder.Relocations(RcuModuleSection::Text).size();
    const std::uint32_t symIdx = funcSyms.contains(func.name)
                                   ? funcSyms.at(func.name)
                                   : DeclareSymbol(func.name, func.returnType.ToString(), RcuSymKind::Func,
                                                   RcuSymVis::ForDefinition(func.isPublic, func.isExported));
    funcSyms[func.name] = symIdx;
    if (!moduleBuilder.BeginFunction(symIdx)) {
        activeFramePlan = nullptr;
        currentFunc.clear();
        return;
    }
    while (true) {
        terminatorEmitter.BeginPass(func.blocks.size());
        EmitPrologue();
        if (FramePlan().IndirectResultOffset() != 0) {
            StoreScalar(A64::Xn(kIndirectResult), A64::Fp, FramePlan().IndirectResultOffset(), 8);
        }
        callEmitter.EmitParamSpills(func);
        for (std::uint32_t bi = 0; bi < func.blocks.size(); ++bi) {
            terminatorEmitter.MarkBlock(bi);
            for (const auto &instr : func.blocks[bi].instrs) {
                GenInstr(functionEmitter, callEmitter, terminatorEmitter, instr);
            }
            if (func.blocks[bi].term) {
                terminatorEmitter.Emit(bi, *func.blocks[bi].term);
            }
        }
        const std::size_t widened = terminatorEmitter.WidenedSiteCount();
        if (terminatorEmitter.PatchJumps() || terminatorEmitter.WidenedSiteCount() == widened) {
            break;
        }
        // Nothing this pass produced is kept: the constants it interned are
        // reached by name and are found again, but its instructions and the
        // relocations hung on them are about to be emitted a second time.
        (void)moduleBuilder.TruncateSection(RcuModuleSection::Text, funcStart, relocCount);
    }

    (void)moduleBuilder.EndFunction(symIdx);
    activeFramePlan = nullptr;
    currentFunc.clear();
}

std::uint32_t AArch64ModuleEmitter::ResolveMachineSymbol(const AArch64Machine::SymbolRef &symbol) {
    using Kind = AArch64Machine::SymbolRef::Kind;
    switch (symbol.kind) {
    case Kind::Global:
        return ResolveGlobalSymbol(symbol.text);
    case Kind::NamedData:
        return GetOrAddExtern(symbol.text, RcuSymKind::ExternData);
    case Kind::Function:
        return ResolveCallSymbol(symbol.text);
    case Kind::String:
        return InternStringLiteral(symbol.text);
    case Kind::Float32:
        return InternF32(symbol.text);
    case Kind::Float64:
        return InternF64(symbol.text);
    case Kind::Kernel32:
        return GetOrAddExtern(symbol.text, RcuSymKind::ExternFunc, std::string(kKernel32));
    }
    return kNoSymbol;
}

bool AArch64ModuleEmitter::GenMachineFunc(const LirFunc &func) {
    if (ReachesImportedData(func)) {
        return false;
    }
    const AArch64Machine::SelectionContext context{layouts, interfaceNames, callPlanner, targetOs, backtrace.enabled};
    auto selected = AArch64Machine::SelectFunction(func, context);
    if (!selected) {
        return false;
    }
    const auto emitted = AArch64Machine::EmitFunction(*selected, targetOs);
    if (!emitted) {
        return false;
    }
    const std::uint32_t symIdx = funcSyms.contains(func.name)
                                   ? funcSyms.at(func.name)
                                   : DeclareSymbol(func.name, func.returnType.ToString(), RcuSymKind::Func,
                                                   RcuSymVis::ForDefinition(func.isPublic, func.isExported));
    funcSyms[func.name] = symIdx;
    std::vector<std::uint32_t> symbols;
    symbols.reserve(selected->symbols.size());
    for (const auto &symbol : selected->symbols) {
        symbols.push_back(ResolveMachineSymbol(symbol));
    }
    if (!moduleBuilder.BeginFunction(symIdx)) {
        return true;
    }
    const auto start = static_cast<std::uint32_t>(TextData().size());
    TextData().insert(TextData().end(), emitted->bytes.begin(), emitted->bytes.end());
    for (const auto &use : emitted->symbolUses) {
        AddTextReloc(start + use.offset, symbols.at(use.symbol), use.type, use.addend);
    }
    (void)moduleBuilder.EndFunction(symIdx);
    return true;
}
} // namespace Rux::AArch64Detail

namespace Rux {
using AArch64Detail::AArch64ModuleEmitter;

AArch64RcuEmitter::AArch64RcuEmitter(const LirPackage &package, std::string inputPackageName,
                                     const Target::OS inputTargetOs, BuildInfo inputBuildInfo)
    : lir(package)
    , packageName(std::move(inputPackageName))
    , targetOs(inputTargetOs)
    , buildInfo(std::move(inputBuildInfo)) {
}

std::vector<RcuFile> AArch64RcuEmitter::Generate() const {
    // Struct layouts and interface names are gathered across the whole package
    // first: a module reaches types the module beside it declared, and a frame
    // cannot be laid out without their sizes.
    const auto layouts = Layout::BuildPackageLayouts(lir);
    // Modules trace their failures and list their functions when asked to, or when the package holds the stack-trace
    // printer; the module holding the printer also lists every module's table.
    const auto holdsPrinter = [](const LirModule &module) {
        return std::ranges::any_of(module.funcs,
                                   [](const LirFunc &function) { return function.name == kBacktraceFunctionName; });
    };
    const bool tracesPanics = backtraces || std::ranges::any_of(lir.modules, holdsPrinter);
    std::vector<std::string> tables;
    tables.reserve(lir.modules.size());
    for (const LirModule &module : lir.modules)
        tables.push_back(FunctionTableSymbol(module.name));
    const auto backtraceOf = [&](const std::size_t index) {
        return BacktraceTables{tracesPanics, tables[index],
                               holdsPrinter(lir.modules[index]) ? tables : std::vector<std::string>{}};
    };
    // Modules are independent objects, so each is emitted on its own; diagnostics merge in module order.
    std::vector<RcuFile> result(lir.modules.size());
    std::vector<std::vector<Diagnostic>> moduleDiagnostics(lir.modules.size());
    const auto emit = [&](const std::size_t index) {
        if (index < emittedModules.size() && !emittedModules[index])
            return;
        AArch64ModuleEmitter gen(lir.modules[index], layouts, packageName, targetOs, buildInfo,
                                 moduleDiagnostics[index]);
        gen.SetBacktrace(backtraceOf(index));
        gen.SetMachineSelection(machineSelection);
        result[index] = gen.Generate();
    };
    if (pool != nullptr)
        pool->ForEach(lir.modules.size(), emit);
    else
        for (std::size_t index = 0; index < lir.modules.size(); ++index)
            emit(index);
    for (auto &emitted : moduleDiagnostics)
        diagnostics.insert(diagnostics.end(), std::make_move_iterator(emitted.begin()),
                           std::make_move_iterator(emitted.end()));
    return result;
}
} // namespace Rux
