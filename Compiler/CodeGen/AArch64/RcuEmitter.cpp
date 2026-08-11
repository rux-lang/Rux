// AArch64 RCU code generation: lowers a LirModule to an in-memory RcuFile.

#include "CodeGen/AArch64/RcuEmitter.h"

#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/Registers.h"
#include "CodeGen/IntegerLiteral.h"
#include "CodeGen/Layout.h"
#include "Object/Rcu/RcuMetadata.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Rux {
using namespace Layout;

namespace {
// A branch waiting for the offset of the block it targets. AArch64 splits a
// branch immediate across a field of its own width — 26 bits for B, 19 for the
// conditional forms, 14 for the test-and-branch pair — so a patch site names
// the instruction and the field inside it rather than a displacement the way an
// x86-64 patch site does.
struct JumpPatch {
    std::uint32_t patchOff = 0; // byte offset of the branch instruction
    std::uint32_t targetBlock = 0;
    unsigned lsb = 0;   // low bit of the immediate field
    unsigned width = 0; // width of that field, in bits
};

// The frame record — the caller's frame pointer and the return address — that
// every prologue stores and every epilogue restores.
constexpr std::int32_t kFrameRecordSize = 16;

// The largest frame a single pre-indexed STP can open: its 7-bit immediate
// counts pairs of doublewords, so it reaches 64 of them below the stack
// pointer. A frame past this opens with FrameAdjust instead.
constexpr std::int32_t kInlineFrameLimit = 512;

// The register this generator computes in. AAPCS64 leaves X9 through X15 to the
// caller, so nothing that has to survive a call lives here, and it is clear of
// both the argument registers and the X16/X17 pair the encoder's composite
// sequences take as scratch.
constexpr unsigned kTemp = 9;

// The register an integer or pointer result is returned in.
constexpr unsigned kReturn = 0;

// Whether a value of this type fits one general-purpose register and moves
// without any float or aggregate handling — which is the whole of what this
// back end lowers so far.
[[nodiscard]] bool IsScalarInteger(const TypeRef &t) {
    return t.IsInteger() || t.IsBool() || t.kind == TypeRef::Kind::Char8 || t.kind == TypeRef::Kind::Char16 ||
           t.kind == TypeRef::Kind::Char32 || t.kind == TypeRef::Kind::Pointer;
}

// The access width a scalar of `size` bytes is moved at: the four widths a
// load or store names, with anything else rounded up to a whole register.
[[nodiscard]] unsigned AccessWidth(const int size) {
    if (size <= 1) {
        return 1;
    }
    if (size <= 2) {
        return 2;
    }
    if (size <= 4) {
        return 4;
    }
    return 8;
}

class AArch64CodeGen {
public:
    explicit AArch64CodeGen(const LirModule &module, const std::vector<LirStructDecl> &inputStructDecls,
                            const std::vector<std::string> &inputPackageInterfaceNames, std::string inputPackageName,
                            std::vector<Diagnostic> &inputDiagnostics)
        : mod(module)
        , structDecls(inputStructDecls)
        , packageInterfaceNames(inputPackageInterfaceNames)
        , pkgName(std::move(inputPackageName))
        , diagnostics(inputDiagnostics)
        , enc(textData) {
    }

    RcuFile Generate();

private:
    const LirModule &mod;
    const std::vector<LirStructDecl> &structDecls;
    const std::vector<std::string> &packageInterfaceNames;
    std::string pkgName;
    std::vector<Diagnostic> &diagnostics;

    // Section data buffers
    std::vector<std::uint8_t> textData;
    std::vector<std::uint8_t> rodataData;
    std::vector<std::uint8_t> dataData;

    // Per-section relocations
    std::vector<RcuReloc> textRelocs;
    std::vector<RcuReloc> rodataRelocs;

    // Symbol table
    std::vector<RcuSymbol> symbols;

    // Encoder writing into textData
    A64Enc enc;

    // Declared symbols, by name → symbol index
    std::unordered_map<std::string, std::uint32_t> externSyms;
    std::unordered_map<std::string, std::uint32_t> funcSyms;

    // Struct field layouts and the interfaces whose values are fat pointers
    LayoutMap layouts;
    std::unordered_set<std::string> interfaceNames;

    // Per-function state, the same set the x86-64 generator keeps: where each
    // virtual register spills to, where an alloca's storage sits, what type
    // each register holds, where each block began, and which branches are still
    // waiting for a block offset.
    std::unordered_map<LirReg, std::int32_t> slotMap;
    std::unordered_map<LirReg, std::int32_t> allocaData;
    std::unordered_map<LirReg, TypeRef> regTypes;
    std::vector<std::uint32_t> blockOffsets;
    std::vector<JumpPatch> jumpPatches;
    std::int32_t nextOff = 0;
    std::int32_t frameSize = 0;

    // The function being generated, so a report can say where it was reached.
    std::string currentFunc;

    // Reports already made, so a construct reached in a loop is named once
    // rather than once per instruction.
    std::unordered_set<std::string> reported;

    // Diagnostics
    //
    // A back end under construction refuses far more than it accepts, so
    // "not implemented yet" on its own would say nothing about which program to
    // change. Every report names the construct and the function it was reached
    // in.

    void Report(std::string message) {
        if (reported.insert(message).second) {
            diagnostics.push_back(ErrorDiagnostic(std::move(message)));
        }
    }

    void NotImplemented(const std::string_view what) {
        if (currentFunc.empty()) {
            Report(std::format("AArch64 code generation for {} is not implemented yet", what));
            return;
        }
        Report(
            std::format("AArch64 code generation for {} is not implemented yet, reached in '{}'", what, currentFunc));
    }

    // Every encoder reports a status rather than asserting, so an operand
    // combination this generator got wrong becomes a diagnostic instead of
    // silence or a crash. Nothing below should ever fail — the operands are
    // this generator's own — so a report here is a bug in the selection above
    // it, named precisely enough to find.
    void Must(const A64Status status, const std::string_view what) {
        if (status != A64Status::Ok) {
            Report(std::format("AArch64 code generation could not encode {} in '{}': {}", what, currentFunc,
                               A64StatusName(status)));
        }
    }

    // Symbols

    std::uint32_t AddSymbol(RcuSymbol s) {
        const auto idx = static_cast<std::uint32_t>(symbols.size());
        symbols.push_back(std::move(s));
        return idx;
    }

    std::uint32_t GetOrAddExtern(const std::string &name, const std::uint8_t kind, const std::string &dll = {}) {
        if (const auto it = externSyms.find(name); it != externSyms.end()) {
            return it->second;
        }
        RcuSymbol s;
        s.name = name;
        s.typeName = dll;
        s.kind = kind;
        s.visibility = RcuSymVis::Global;
        s.sectionIdx = RCU_SEC_EXTERNAL;
        const std::uint32_t idx = AddSymbol(std::move(s));
        externSyms[name] = idx;
        return idx;
    }

    // Every function this module defines gets its symbol before any body is
    // emitted, so a call can name a function declared further down the file and
    // a body can be placed at whatever offset it turns out to start at.
    void PredeclareFunctions() {
        for (const auto &func : mod.funcs) {
            if (func.isExtern || funcSyms.contains(func.name)) {
                continue;
            }
            RcuSymbol sym;
            sym.name = func.name;
            sym.sectionIdx = RCU_TEXT_IDX;
            sym.value = 0;
            sym.kind = RcuSymKind::Func;
            sym.visibility = func.isPublic ? RcuSymVis::Global : RcuSymVis::Local;
            sym.typeName = func.returnType.ToString();
            funcSyms[func.name] = AddSymbol(std::move(sym));
        }
    }

    // Fill in the predeclared symbol with where the body actually landed, and
    // return its index so the size can be written once the body is complete.
    std::uint32_t DefineFunction(const LirFunc &func, const std::uint32_t funcStart) {
        RcuSymbol sym;
        sym.name = func.name;
        sym.sectionIdx = RCU_TEXT_IDX;
        sym.value = funcStart;
        sym.kind = RcuSymKind::Func;
        sym.visibility = func.isPublic ? RcuSymVis::Global : RcuSymVis::Local;
        sym.typeName = func.returnType.ToString();
        if (const auto it = funcSyms.find(func.name); it != funcSyms.end()) {
            symbols[it->second] = std::move(sym);
            return it->second;
        }
        const std::uint32_t idx = AddSymbol(std::move(sym));
        funcSyms[func.name] = idx;
        return idx;
    }

    void PatchJumps() {
        for (const auto &patch : jumpPatches) {
            if (patch.targetBlock >= blockOffsets.size()) {
                continue;
            }
            const auto target = static_cast<std::int32_t>(blockOffsets[patch.targetBlock]);
            const std::int32_t instructions =
                (target - static_cast<std::int32_t>(patch.patchOff)) / static_cast<std::int32_t>(A64Enc::InstrSize);
            enc.PatchField(patch.patchOff, patch.lsb, patch.width, static_cast<std::uint32_t>(instructions));
        }
    }

    // Frame layout
    //
    // The frame record sits at the bottom of the frame, which is where X29
    // points once the prologue has run, so every local is at a positive
    // displacement from both X29 and SP. That is the opposite of the x86-64
    // frame, where RBP sits at the top and a local is reached below it; the
    // sign is the only thing that differs, and it differs because STP writes
    // upward from the address it has just decremented SP to.

    [[nodiscard]] std::int32_t Disp(const LirReg reg) {
        if (const auto it = slotMap.find(reg); it != slotMap.end()) {
            return it->second;
        }
        Report(
            std::format("AArch64 code generation reached register %{} with no stack slot in '{}'", reg, currentFunc));
        return kFrameRecordSize;
    }

    std::int32_t AllocRegion(const int bytes) {
        const int align = bytes > 0 ? std::min(bytes, 8) : 1;
        nextOff = AlignUp(nextOff, align);
        const std::int32_t offset = nextOff;
        nextOff += bytes > 0 ? bytes : 8;
        return offset;
    }

    std::int32_t AllocSlot(const LirReg reg, const int bytes) {
        if (const auto it = slotMap.find(reg); it != slotMap.end()) {
            return it->second;
        }
        const std::int32_t offset = AllocRegion(bytes);
        slotMap[reg] = offset;
        return offset;
    }

    [[nodiscard]] int RuntimeSize(const TypeRef &t) const {
        return RuntimeSizeOf(t, layouts, interfaceNames);
    }

    // How much storage an alloca needs: an element count in `strArg` makes it
    // an array of that many, and otherwise it is one value of its type.
    [[nodiscard]] int AllocaBytes(const LirInstr &instr) const {
        if (instr.strArg.empty()) {
            const int size = RuntimeSize(instr.type);
            return size > 0 ? size : 8;
        }
        int count = 0;
        const char *first = instr.strArg.data();
        std::from_chars(first, first + instr.strArg.size(), count);
        const TypeRef &elemType = instr.type.inner.empty() ? instr.type : instr.type.inner[0];
        const int elemSize = RuntimeSize(elemType);
        const int bytes = count * (elemSize > 0 ? elemSize : 8);
        return bytes > 0 ? bytes : 8;
    }

    void PrepassFunc(const LirFunc &func) {
        slotMap.clear();
        allocaData.clear();
        regTypes.clear();
        nextOff = kFrameRecordSize;

        for (const auto &p : func.params) {
            regTypes[p.reg] = p.type;
            AllocSlot(p.reg, std::max(8, RuntimeSize(p.type)));
        }
        for (const auto &block : func.blocks) {
            for (const auto &instr : block.instrs) {
                if (instr.dst == LirNoReg) {
                    continue;
                }
                if (instr.op == LirOpcode::Alloca) {
                    regTypes[instr.dst] = TypeRef::MakePointer(instr.type);
                    AllocSlot(instr.dst, 8);
                    allocaData[instr.dst] = AllocRegion(AllocaBytes(instr));
                    continue;
                }
                regTypes[instr.dst] = instr.type;
                const int size = RuntimeSize(instr.type);
                AllocSlot(instr.dst, size > 0 ? size : 8);
            }
        }
        frameSize = AlignUp(nextOff, 16);
    }

    // STP writes the frame record at the address it decrements SP to, so the
    // whole frame opens in one instruction whenever its size is inside the
    // pre-indexed reach. A larger frame opens with FrameAdjust — which is where
    // a size past an imm12 becomes a scratch register and a register-form SUB —
    // and stores the record afterwards. Either way X29 ends up at the bottom of
    // the frame, and SP is a multiple of 16 at every instruction boundary,
    // since nothing between the two moves it by anything else.
    void EmitPrologue() {
        if (frameSize <= kInlineFrameLimit) {
            Must(enc.Stp(A64::Fp, A64::Lr, A64::Sp, -frameSize, A64IndexMode::PreIndex), "the frame record");
        }
        else {
            Must(enc.FrameAdjust(-frameSize), "the frame");
            Must(enc.Stp(A64::Fp, A64::Lr, A64::Sp, 0), "the frame record");
        }
        Must(enc.Mov(A64::Fp, A64::Sp), "the frame pointer");
    }

    void EmitEpilogue() {
        if (frameSize <= kInlineFrameLimit) {
            Must(enc.Ldp(A64::Fp, A64::Lr, A64::Sp, frameSize, A64IndexMode::PostIndex), "the frame record");
        }
        else {
            Must(enc.Ldp(A64::Fp, A64::Lr, A64::Sp, 0), "the frame record");
            Must(enc.FrameAdjust(frameSize), "the frame");
        }
        Must(enc.Ret(), "the return");
    }

    // Stack slots
    //
    // ResolveMemOperand decides how the frame pointer reaches a slot — the
    // scaled form where the displacement divides, the unscaled one where it
    // does not, a scratch register where neither reaches — and emits whatever
    // that took, leaving the access itself to be written here, because only
    // here is the width and the signedness of the value known.

    void StoreToSlot(const A64Reg value, const LirReg reg, const TypeRef &type) {
        const unsigned width = AccessWidth(RuntimeSize(type));
        A64MemOperand mem{};
        Must(enc.ResolveMemOperand(A64::Fp, Disp(reg), width, mem), "a stack slot address");
        // A narrowing store names the register it truncates as a W one,
        // whatever width the value arrived in.
        const A64Reg src = A64::Gpr(value.code, width == 8 ? 64 : 32);
        const auto scaled = static_cast<std::uint64_t>(mem.offset);
        A64Status status = A64Status::Ok;
        switch (width) {
        case 1:
            status = mem.unscaled ? enc.Sturb(src, mem.base, mem.offset) : enc.Strb(src, mem.base, scaled);
            break;
        case 2:
            status = mem.unscaled ? enc.Sturh(src, mem.base, mem.offset) : enc.Strh(src, mem.base, scaled);
            break;
        default:
            status = mem.unscaled ? enc.Stur(src, mem.base, mem.offset) : enc.Str(src, mem.base, scaled);
            break;
        }
        Must(status, "a spill to a stack slot");
    }

    // Load `reg`'s slot into the 64-bit register `dst`, widening as the type
    // says: a signed value sign-extends, and an unsigned one is loaded into the
    // W view, which zeroes the half of the register above it.
    void LoadFromSlot(const A64Reg dst, const LirReg reg, const TypeRef &type) {
        const unsigned width = AccessWidth(RuntimeSize(type));
        const bool sign = type.IsSigned();
        A64MemOperand mem{};
        Must(enc.ResolveMemOperand(A64::Fp, Disp(reg), width, mem), "a stack slot address");
        const A64Reg narrow = A64::Gpr(dst.code, 32);
        const auto scaled = static_cast<std::uint64_t>(mem.offset);
        A64Status status = A64Status::Ok;
        switch (width) {
        case 1:
            if (sign) {
                status = mem.unscaled ? enc.Ldursb(dst, mem.base, mem.offset) : enc.Ldrsb(dst, mem.base, scaled);
            }
            else {
                status = mem.unscaled ? enc.Ldurb(narrow, mem.base, mem.offset) : enc.Ldrb(narrow, mem.base, scaled);
            }
            break;
        case 2:
            if (sign) {
                status = mem.unscaled ? enc.Ldursh(dst, mem.base, mem.offset) : enc.Ldrsh(dst, mem.base, scaled);
            }
            else {
                status = mem.unscaled ? enc.Ldurh(narrow, mem.base, mem.offset) : enc.Ldrh(narrow, mem.base, scaled);
            }
            break;
        case 4:
            if (sign) {
                status = mem.unscaled ? enc.Ldursw(dst, mem.base, mem.offset) : enc.Ldrsw(dst, mem.base, scaled);
            }
            else {
                status = mem.unscaled ? enc.Ldur(narrow, mem.base, mem.offset) : enc.Ldr(narrow, mem.base, scaled);
            }
            break;
        default:
            status = mem.unscaled ? enc.Ldur(dst, mem.base, mem.offset) : enc.Ldr(dst, mem.base, scaled);
            break;
        }
        Must(status, "a reload from a stack slot");
    }

    // Instruction selection

    // The bits a constant denotes. A boolean is written as a word rather than a
    // number, and an unreadable literal is zero, which is what the x86-64 back
    // end also does with one.
    [[nodiscard]] static std::uint64_t ConstantBits(const LirInstr &instr) {
        if (instr.type.IsBool()) {
            return instr.strArg == "true" || instr.strArg == "1" ? 1 : 0;
        }
        return ParseIntegerLiteralBits(instr.strArg.empty() ? "0" : instr.strArg).value_or(0);
    }

    void GenInstr(const LirInstr &instr) {
        switch (instr.op) {
        case LirOpcode::Const: {
            if (instr.dst == LirNoReg) {
                break;
            }
            if (!IsScalarInteger(instr.type)) {
                NotImplemented(std::format("a constant of type '{}'", instr.type.ToString()));
                break;
            }
            // The constant is materialized at full width whatever the type, and
            // the store below writes only the bytes the type occupies, so the
            // shortest sequence for the value is the one that gets emitted.
            const A64Reg value = A64::Xn(kTemp);
            Must(enc.LoadImm64(value, ConstantBits(instr)), "a constant");
            StoreToSlot(value, instr.dst, instr.type);
            break;
        }
        default:
            NotImplemented(std::format("the '{}' opcode", LirOpcodeName(instr.op)));
            break;
        }
    }

    void GenTerm(const LirTerminator &term) {
        switch (term.kind) {
        case LirTermKind::Return: {
            if (term.retVal && *term.retVal != LirNoReg) {
                if (!IsScalarInteger(term.retType)) {
                    NotImplemented(std::format("returning a value of type '{}'", term.retType.ToString()));
                }
                else {
                    LoadFromSlot(A64::Xn(kReturn), *term.retVal, term.retType);
                }
            }
            EmitEpilogue();
            break;
        }
        default:
            NotImplemented(std::format("the '{}' terminator", LirTermKindName(term.kind)));
            break;
        }
    }

    void GenFunc(const LirFunc &func) {
        if (func.isExtern) {
            GetOrAddExtern(func.name, RcuSymKind::ExternFunc, func.dll);
            return;
        }
        currentFunc = func.name;
        if (func.isAsm) {
            NotImplemented("an `asm func` body");
            currentFunc.clear();
            return;
        }
        if (!func.params.empty()) {
            NotImplemented("function parameters");
        }

        PrepassFunc(func);
        jumpPatches.clear();
        const std::uint32_t funcStart = enc.Size();
        const std::uint32_t symIdx = DefineFunction(func, funcStart);

        EmitPrologue();
        blockOffsets.assign(func.blocks.size(), 0);
        for (std::uint32_t bi = 0; bi < func.blocks.size(); ++bi) {
            blockOffsets[bi] = enc.Size();
            for (const auto &instr : func.blocks[bi].instrs) {
                GenInstr(instr);
            }
            if (func.blocks[bi].term) {
                GenTerm(*func.blocks[bi].term);
            }
        }
        PatchJumps();

        symbols[symIdx].size = enc.Size() - funcStart;
        currentFunc.clear();
    }

    void GenModule() {
        for (const auto &name : packageInterfaceNames) {
            interfaceNames.insert(name);
        }
        for (const auto &s : structDecls) {
            layouts[s.name] = ComputeStructLayout(s, layouts);
        }

        PredeclareFunctions();
        for (const auto &ev : mod.externVars) {
            GetOrAddExtern(ev.name, RcuSymKind::ExternData);
        }
        // The read-only pool and the data section are BACKLOG.md task 20's, and
        // the vtables go with them, so a module carrying either is refused
        // rather than linked against symbols nothing wrote.
        if (!mod.consts.empty()) {
            NotImplemented("module constants and the read-only data pool");
        }
        if (!mod.vtables.empty()) {
            NotImplemented("interface vtables");
        }
        for (const auto &func : mod.funcs) {
            GenFunc(func);
        }
    }
};

RcuFile AArch64CodeGen::Generate() {
    GenModule();

    RcuFile file;
    file.arch = RcuArch::AArch64;
    file.sourcePath = mod.name;
    file.packageName = pkgName;
    file.buildTimestamp = RcuBuildTimestamp();
    file.ruxVersion = RcuCompilerVersion();

    {
        RcuSection text;
        text.name = ".text";
        text.type = RcuSecType::Text;
        text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
        // Every AArch64 instruction is a word and must be word-aligned; the
        // section keeps the same 16 the x86-64 one does so a function starts on
        // a cache-friendly boundary.
        text.alignment = 16;
        text.data = std::move(textData);
        text.relocs = std::move(textRelocs);
        file.sections.push_back(std::move(text));
    }
    {
        RcuSection rodata;
        rodata.name = ".rodata";
        rodata.type = RcuSecType::RoData;
        rodata.flags = RcuSecFlag::Alloc | RcuSecFlag::Read;
        rodata.alignment = 8;
        rodata.data = std::move(rodataData);
        rodata.relocs = std::move(rodataRelocs);
        file.sections.push_back(std::move(rodata));
    }
    {
        RcuSection data;
        data.name = ".data";
        data.type = RcuSecType::Data;
        data.flags = RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write;
        data.alignment = 8;
        data.data = std::move(dataData);
        file.sections.push_back(std::move(data));
    }

    file.symbols = std::move(symbols);
    file.flags = 0x01; // F_HAS_METADATA
    file.hasMetadata = true;
    return file;
}
} // namespace

AArch64RcuEmitter::AArch64RcuEmitter(const LirPackage &package, std::string inputPackageName)
    : lir(package)
    , packageName(std::move(inputPackageName)) {
}

std::vector<RcuFile> AArch64RcuEmitter::Generate() const {
    std::vector<RcuFile> result;
    result.reserve(lir.modules.size());
    // Struct layouts and interface names are gathered across the whole package
    // first: a module reaches types the module beside it declared, and a frame
    // cannot be laid out without their sizes.
    std::vector<LirStructDecl> structDecls;
    std::vector<std::string> interfaceNames;
    for (const auto &module : lir.modules) {
        structDecls.insert(structDecls.end(), module.structs.begin(), module.structs.end());
        interfaceNames.insert(interfaceNames.end(), module.interfaceNames.begin(), module.interfaceNames.end());
    }
    for (const auto &module : lir.modules) {
        AArch64CodeGen gen(module, structDecls, interfaceNames, packageName, diagnostics);
        result.push_back(gen.Generate());
    }
    return result;
}
} // namespace Rux
