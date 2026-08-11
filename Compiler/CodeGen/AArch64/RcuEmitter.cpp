// AArch64 RCU code generation: lowers a LirModule to an in-memory RcuFile.

#include "CodeGen/AArch64/RcuEmitter.h"

#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/Registers.h"
#include "CodeGen/FloatLiteral.h"
#include "CodeGen/IntegerLiteral.h"
#include "CodeGen/Layout.h"
#include "Object/Rcu/RcuMetadata.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdint>
#include <cstring>
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

// The register an address is computed in. A load reads through it and a store
// writes through it, so it is never the register the value itself is in.
constexpr unsigned kAddr = 10;

// The second address a block copy needs: an aggregate is read through one
// pointer and written through another, and both are live at once.
constexpr unsigned kSrcAddr = 11;

// The second value register. LDP and STP move two registers at a time, and a
// multiply by an element width needs somewhere to put the width.
constexpr unsigned kTemp2 = 12;

// The register an integer or pointer result is returned in.
constexpr unsigned kReturn = 0;

// The vector register a floating-point value is computed in. AAPCS64 preserves
// only the low half of V8 through V15 across a call, so the caller-saved half
// of the file starts at V16 and nothing here has to be saved by a callee.
constexpr unsigned kFpTemp = 16;

// Whether a value of this type fits one general-purpose register and moves
// without any float or aggregate handling — which is the whole of what this
// back end lowers so far.
[[nodiscard]] bool IsScalarInteger(const TypeRef &t) {
    return t.IsInteger() || t.IsBool() || t.kind == TypeRef::Kind::Char8 || t.kind == TypeRef::Kind::Char16 ||
           t.kind == TypeRef::Kind::Char32 || t.kind == TypeRef::Kind::Pointer;
}

// The access width a scalar of `size` bytes is moved at: the four widths a
// load or store names, with anything else rounded up to a whole register. A
// type with no size of its own — an opaque one — is a whole register too,
// since that is what its stack slot was given.
[[nodiscard]] unsigned AccessWidth(const int size) {
    if (size <= 0) {
        return 8;
    }
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

// Whether LDP and STP of two doublewords reach `offset`. Their 7-bit immediate
// counts pairs of doublewords, so it reaches 64 of them either way and cannot
// name a displacement that is not a multiple of eight at all.
[[nodiscard]] bool InPairRange(const std::int64_t offset) {
    return offset % 8 == 0 && offset >= -512 && offset <= 504;
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
    std::unordered_map<std::string, std::uint32_t> dataSyms;

    // Interned read-only constants, by the literal that produced them → symbol
    // index, so a value written twice is emitted once. The counter names them
    // apart; the names are local to the object and mean nothing outside it.
    std::unordered_map<std::string, std::uint32_t> strSyms;
    std::unordered_map<std::string, std::uint32_t> f32Syms;
    std::unordered_map<std::string, std::uint32_t> f64Syms;
    unsigned constIdx = 0;

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

    // The read-only pool
    //
    // A value the instruction set cannot name goes into .rodata and is reached
    // through a symbol, and a value written twice in the same module is emitted
    // once: the literal the source wrote is the key, so two spellings of the
    // same number stay two constants, exactly as they do in the x86-64 pool.

    // Pad .rodata out to `align` and report where the next constant starts.
    std::uint32_t AlignRodata(const int align) {
        while (rodataData.size() % static_cast<std::size_t>(align) != 0) {
            rodataData.push_back(0);
        }
        return static_cast<std::uint32_t>(rodataData.size());
    }

    std::uint32_t AddRodataConst(const std::string &name, const std::uint32_t offset) {
        RcuSymbol sym;
        sym.name = name;
        sym.sectionIdx = RCU_RODATA_IDX;
        sym.value = offset;
        sym.size = static_cast<std::uint32_t>(rodataData.size()) - offset;
        sym.kind = RcuSymKind::Const;
        sym.visibility = RcuSymVis::Local;
        return AddSymbol(std::move(sym));
    }

    // `bytes` is already encoded at its element width; the terminator is one
    // more element of zeroes, which AlignRodata's zero fill cannot be relied on
    // to supply.
    std::uint32_t InternStr(const std::string &bytes) {
        if (const auto it = strSyms.find(bytes); it != strSyms.end()) {
            return it->second;
        }
        const auto offset = static_cast<std::uint32_t>(rodataData.size());
        for (const unsigned char byte : bytes) {
            rodataData.push_back(byte);
        }
        rodataData.push_back(0);
        const std::uint32_t idx = AddRodataConst(std::format("__str{}", constIdx++), offset);
        strSyms[bytes] = idx;
        return idx;
    }

    std::uint32_t InternF32(const std::string &literal) {
        if (const auto it = f32Syms.find(literal); it != f32Syms.end()) {
            return it->second;
        }
        const std::uint32_t offset = AlignRodata(4);
        const float value = ParseFloatLiteral<float>(literal);
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, 4);
        for (int i = 0; i < 4; ++i) {
            rodataData.push_back(static_cast<std::uint8_t>(bits >> (8 * i) & 0xFFU));
        }
        const std::uint32_t idx = AddRodataConst(std::format("__f32_{}", constIdx++), offset);
        f32Syms[literal] = idx;
        return idx;
    }

    std::uint32_t InternF64(const std::string &literal) {
        if (const auto it = f64Syms.find(literal); it != f64Syms.end()) {
            return it->second;
        }
        const std::uint32_t offset = AlignRodata(8);
        const double value = ParseFloatLiteral<double>(literal);
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, 8);
        for (int i = 0; i < 8; ++i) {
            rodataData.push_back(static_cast<std::uint8_t>(bits >> (8 * i) & 0xFFU));
        }
        const std::uint32_t idx = AddRodataConst(std::format("__f64_{}", constIdx++), offset);
        f64Syms[literal] = idx;
        return idx;
    }

    // Relocations

    void AddTextReloc(const std::uint32_t sectionOff, const std::uint32_t symIdx, const std::uint16_t type,
                      const std::int32_t addend = 0) {
        textRelocs.push_back({sectionOff, symIdx, type, addend});
    }

    void AddRodataReloc(const std::uint32_t sectionOff, const std::uint32_t symIdx, const std::uint16_t type,
                        const std::int32_t addend = 0) {
        rodataRelocs.push_back({sectionOff, symIdx, type, addend});
    }

    // The address of a symbol: the page it sits on, then its offset within that
    // page. Both immediates are emitted as zero and belong to the two
    // relocations hung on them, so the sequence is the same two instructions
    // whatever the symbol turns out to be and wherever the linker puts it.
    void LoadSymbolAddress(const A64Reg rd, const std::uint32_t symIdx) {
        A64SymbolRef ref{};
        Must(enc.LoadAddress(rd, ref), "the address of a symbol");
        AddTextReloc(ref.adrp, symIdx, RcuRelType::AArch64AdrPrelPgHi21);
        AddTextReloc(ref.lo12, symIdx, RcuRelType::AArch64AddAbsLo12Nc);
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

    // The alignment the running program gives a value of this type, which is
    // what decides whether a block copy of it may use the pair forms. AlignOf
    // can only derive an alignment from a size, so a named type answers from
    // its computed layout wherever the package declared one.
    [[nodiscard]] int RuntimeAlign(const TypeRef &t) const {
        if (!t.IsRange() && t.kind == TypeRef::Kind::Named) {
            const std::string base = BaseTypeName(t.name);
            if (interfaceNames.contains(base)) {
                return 8;
            }
            if (const auto it = layouts.find(base); it != layouts.end()) {
                return it->second.alignment;
            }
        }
        return AlignOf(t);
    }

    // Whether a value of this type moves as a block of bytes rather than in one
    // register. What counts as an aggregate is a property of the LIR type
    // rather than of the machine, so this is the x86-64 rule unchanged.
    [[nodiscard]] bool IsAggregate(const TypeRef &t) const {
        if (t.IsRange()) {
            return true;
        }
        switch (t.kind) {
        case TypeRef::Kind::Tuple:
        case TypeRef::Kind::Array:
            return true;
        case TypeRef::Kind::Named: {
            const std::string base = BaseTypeName(t.name);
            return base == "Slice" || interfaceNames.contains(base) || layouts.contains(base) ||
                   (!t.inner.empty() && SizeOf(t) > 8);
        }
        default:
            return false;
        }
    }

    // A block move needs an address on both sides, and half the time the value
    // side is already one: a register holding a pointer to the very aggregate
    // being moved is that address, and taking the address of its slot would
    // copy the pointer rather than what it points at.
    [[nodiscard]] bool IsRegPointerTo(const LirReg reg, const TypeRef &pointee) const {
        const auto it = regTypes.find(reg);
        return it != regTypes.end() && it->second.kind == TypeRef::Kind::Pointer && !it->second.inner.empty() &&
               it->second.inner[0] == pointee;
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

    // Memory access
    //
    // Every access this back end makes is a base register and a displacement:
    // a stack slot is the frame pointer and a slot offset, a `load` or a
    // `store` is whatever pointer the program computed, and a field of an
    // aggregate is that pointer and the field's offset. ResolveMemOperand
    // decides how the displacement is reached — the scaled form where it
    // divides, the unscaled one where it does not, a scratch register where
    // neither reaches — and emits whatever that took, leaving the access itself
    // to be written here, because only here is the width and the signedness of
    // the value known.

    void StoreScalar(const A64Reg value, const A64Reg base, const std::int64_t offset, const unsigned width) {
        A64MemOperand mem{};
        Must(enc.ResolveMemOperand(base, offset, width, mem), "a memory address");
        const auto scaled = static_cast<std::uint64_t>(mem.offset);
        // A float has no representation at a width other than its own, so the
        // register decides the access and nothing is narrowed on the way.
        if (value.IsVector()) {
            Must(mem.unscaled ? enc.Stur(value, mem.base, mem.offset) : enc.Str(value, mem.base, scaled), "a store");
            return;
        }
        // A narrowing store names the register it truncates as a W one,
        // whatever width the value arrived in.
        const A64Reg src = A64::Gpr(value.code, width == 8 ? 64 : 32);
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
        Must(status, "a store");
    }

    // Load `width` bytes into the 64-bit register `dst`, widening as `sign`
    // says: a signed value sign-extends, and an unsigned one is loaded into the
    // W view, which zeroes the half of the register above it.
    void LoadScalar(const A64Reg dst, const A64Reg base, const std::int64_t offset, const unsigned width,
                    const bool sign) {
        A64MemOperand mem{};
        Must(enc.ResolveMemOperand(base, offset, width, mem), "a memory address");
        const auto scaled = static_cast<std::uint64_t>(mem.offset);
        if (dst.IsVector()) {
            Must(mem.unscaled ? enc.Ldur(dst, mem.base, mem.offset) : enc.Ldr(dst, mem.base, scaled), "a load");
            return;
        }
        const A64Reg narrow = A64::Gpr(dst.code, 32);
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
        Must(status, "a load");
    }

    // Stack slots, which are the above with the frame pointer as the base.

    void StoreToSlot(const A64Reg value, const LirReg reg, const TypeRef &type) {
        StoreScalar(value, A64::Fp, Disp(reg), AccessWidth(RuntimeSize(type)));
    }

    void StoreFpToSlot(const A64Reg value, const LirReg reg) {
        StoreScalar(value, A64::Fp, Disp(reg), value.bits / 8U);
    }

    void LoadFromSlot(const A64Reg dst, const LirReg reg, const TypeRef &type) {
        LoadScalar(dst, A64::Fp, Disp(reg), AccessWidth(RuntimeSize(type)), type.IsSigned());
    }

    void LoadFpFromSlot(const A64Reg dst, const LirReg reg) {
        LoadScalar(dst, A64::Fp, Disp(reg), dst.bits / 8U, false);
    }

    // A pointer is a doubleword whatever it points at, so bringing one out of
    // its slot is one access at a fixed width rather than LoadFromSlot's
    // type-driven one.
    void LoadPointer(const A64Reg dst, const LirReg reg) {
        LoadScalar(dst, A64::Fp, Disp(reg), 8, false);
    }

    // The address of a value's own slot, for the cases where an aggregate is
    // held in the frame rather than behind a pointer.
    void SlotAddress(const A64Reg dst, const LirReg reg) {
        Must(enc.AddSubLargeImm(dst, A64::Fp, Disp(reg)), "the address of a stack slot");
    }

    // Move `size` bytes from one address to another, widest chunk first.
    //
    // A pair of doublewords moves sixteen bytes in two instructions where
    // single registers would take four, so it takes as much of the block as it
    // reaches: `paired` says the two ends are aligned enough for it, and
    // InPairRange says the scaled immediate can name the displacement. What is
    // left over goes a doubleword, a word, a halfword and a byte at a time,
    // which is the same descent the x86-64 emitter makes.
    void CopyBlock(const A64Reg dst, const std::int64_t dstOff, const A64Reg src, const std::int64_t srcOff,
                   const int size, const bool paired) {
        const A64Reg first = A64::Xn(kTemp);
        const A64Reg second = A64::Xn(kTemp2);
        std::int64_t offset = 0;
        while (paired && offset + 16 <= size && InPairRange(srcOff + offset) && InPairRange(dstOff + offset)) {
            Must(enc.Ldp(first, second, src, srcOff + offset), "a paired load");
            Must(enc.Stp(first, second, dst, dstOff + offset), "a paired store");
            offset += 16;
        }
        for (const int chunk : {8, 4, 2, 1}) {
            while (offset + chunk <= size) {
                LoadScalar(first, src, srcOff + offset, static_cast<unsigned>(chunk), false);
                StoreScalar(first, dst, dstOff + offset, static_cast<unsigned>(chunk));
                offset += chunk;
            }
        }
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

    // Bring a floating-point constant into `dst`. FMOV names 256 values
    // outright, which covers most of what a program writes down; anything else
    // — an exact fraction the encoding misses, or a value with more precision
    // than it carries — is a word or a doubleword in the read-only pool,
    // reached in two instructions rather than the four or five a MOVZ chain
    // through a general-purpose register would take.
    void LoadFloatConstant(const A64Reg dst, const TypeRef &type, const std::string &literal) {
        const bool single = type.kind == TypeRef::Kind::Float32;
        const double value = single ? ParseFloatLiteral<float>(literal) : ParseFloatLiteral<double>(literal);
        if (TryEncodeFpImm8(value)) {
            Must(enc.FmovImm(dst, value), "a floating-point constant");
            return;
        }
        const std::uint32_t symIdx = single ? InternF32(literal) : InternF64(literal);
        A64SymbolRef ref{};
        Must(enc.LoadFromSymbol(dst, ref), "a floating-point constant");
        AddTextReloc(ref.adrp, symIdx, RcuRelType::AArch64AdrPrelPgHi21);
        AddTextReloc(ref.lo12, symIdx, RcuRelType::AArch64LdstAbsLo12Nc);
    }

    void GenInstr(const LirInstr &instr) {
        switch (instr.op) {
        case LirOpcode::Const: {
            if (instr.dst == LirNoReg) {
                break;
            }
            // A `str` is the address of its bytes rather than the bytes
            // themselves, so it is interned and its address materialized, and
            // the slot holds a pointer.
            if (instr.type.kind == TypeRef::Kind::Str) {
                LoadSymbolAddress(A64::Xn(kTemp), InternStr(instr.strArg));
                StoreToSlot(A64::Xn(kTemp), instr.dst, instr.type);
                break;
            }
            if (IsFloat(instr.type)) {
                const A64Reg value = instr.type.kind == TypeRef::Kind::Float32 ? A64::Sn(kFpTemp) : A64::Dn(kFpTemp);
                LoadFloatConstant(value, instr.type, instr.strArg);
                StoreFpToSlot(value, instr.dst);
                break;
            }
            if (!IsScalarInteger(instr.type)) {
                NotImplemented(std::format("a constant of type '{}'", instr.type.ToString()));
                break;
            }
            // Everything else — an integer of any width, a boolean, a character
            // and the null pointer alike — is a bit pattern a general-purpose
            // register holds. It is materialized at full width whatever the
            // type, and the store writes only the bytes the type occupies, so
            // the shortest sequence for the value is the one that gets emitted.
            const A64Reg value = A64::Xn(kTemp);
            Must(enc.LoadImm64(value, ConstantBits(instr)), "a constant");
            StoreToSlot(value, instr.dst, instr.type);
            break;
        }
        case LirOpcode::Alloca: {
            const auto it = allocaData.find(instr.dst);
            if (it == allocaData.end()) {
                Report(std::format("AArch64 code generation reached an alloca with no storage in '{}'", currentFunc));
                break;
            }
            // The storage was reserved by the prepass and sits above the frame
            // record, so its address is the frame pointer plus a displacement
            // rather than minus one as it is on x86-64.
            const A64Reg addr = A64::Xn(kTemp);
            Must(enc.AddSubLargeImm(addr, A64::Fp, it->second), "the address of a local");
            StoreToSlot(addr, instr.dst, TypeRef::MakePointer(instr.type));
            break;
        }
        case LirOpcode::GlobalAddr: {
            std::uint32_t symIdx = 0;
            if (const auto data = dataSyms.find(instr.strArg); data != dataSyms.end()) {
                symIdx = data->second;
            }
            else if (const auto func = funcSyms.find(instr.strArg); func != funcSyms.end()) {
                symIdx = func->second;
            }
            else {
                symIdx = GetOrAddExtern(instr.strArg, RcuSymKind::ExternData);
            }
            LoadSymbolAddress(A64::Xn(kTemp), symIdx);
            StoreToSlot(A64::Xn(kTemp), instr.dst, TypeRef::MakePointer(instr.type));
            break;
        }
        case LirOpcode::StringAddr: {
            const TypeRef elemType = instr.type.inner.empty() ? TypeRef::MakeChar8() : instr.type.inner[0];
            const std::uint32_t symIdx = InternStr(EncodeStringLiteral(instr.strArg, RuntimeSize(elemType)));
            LoadSymbolAddress(A64::Xn(kTemp), symIdx);
            StoreToSlot(A64::Xn(kTemp), instr.dst, instr.type);
            break;
        }
        case LirOpcode::Load: {
            if (instr.dst == LirNoReg) {
                break;
            }
            const TypeRef &type = instr.type;
            const int size = RuntimeSize(type);
            // A named load reads what a symbol holds rather than following a
            // pointer the program computed, which is an ADRP and a load rather
            // than an ADRP, an ADD and then a load.
            if (!instr.strArg.empty()) {
                const A64Reg value = A64::Xn(kTemp);
                A64SymbolRef ref{};
                Must(enc.LoadFromSymbol(value, ref), "a load from a symbol");
                const std::uint32_t symIdx = GetOrAddExtern(instr.strArg, RcuSymKind::ExternData);
                AddTextReloc(ref.adrp, symIdx, RcuRelType::AArch64AdrPrelPgHi21);
                AddTextReloc(ref.lo12, symIdx, RcuRelType::AArch64LdstAbsLo12Nc);
                StoreToSlot(value, instr.dst, type);
                break;
            }
            const A64Reg addr = A64::Xn(kAddr);
            LoadPointer(addr, instr.srcs[0]);
            // An aggregate is copied into the destination's slot rather than
            // brought into a register, since no register holds one.
            if (IsAggregate(type) && size > 8) {
                CopyBlock(A64::Fp, Disp(instr.dst), addr, 0, size, RuntimeAlign(type) >= 8);
                break;
            }
            if (IsFloat(type)) {
                const A64Reg value = type.kind == TypeRef::Kind::Float32 ? A64::Sn(kFpTemp) : A64::Dn(kFpTemp);
                LoadScalar(value, addr, 0, value.bits / 8U, false);
                StoreFpToSlot(value, instr.dst);
                break;
            }
            const A64Reg value = A64::Xn(kTemp);
            LoadScalar(value, addr, 0, AccessWidth(size), type.IsSigned());
            StoreToSlot(value, instr.dst, type);
            break;
        }
        case LirOpcode::Store: {
            if (instr.srcs.size() < 2) {
                Report(std::format("AArch64 code generation reached a store with no pointer in '{}'", currentFunc));
                break;
            }
            const LirReg valReg = instr.srcs[0];
            const TypeRef &type = instr.type;
            const int size = RuntimeSize(type);
            const A64Reg addr = A64::Xn(kAddr);
            LoadPointer(addr, instr.srcs[1]);
            if (IsAggregate(type) && size > 8) {
                const A64Reg source = A64::Xn(kSrcAddr);
                if (IsRegPointerTo(valReg, type)) {
                    LoadPointer(source, valReg);
                }
                else {
                    SlotAddress(source, valReg);
                }
                CopyBlock(addr, 0, source, 0, size, RuntimeAlign(type) >= 8);
                break;
            }
            if (IsFloat(type)) {
                const A64Reg value = type.kind == TypeRef::Kind::Float32 ? A64::Sn(kFpTemp) : A64::Dn(kFpTemp);
                LoadFpFromSlot(value, valReg);
                StoreScalar(value, addr, 0, value.bits / 8U);
                break;
            }
            const A64Reg value = A64::Xn(kTemp);
            LoadFromSlot(value, valReg, type);
            StoreScalar(value, addr, 0, AccessWidth(size));
            break;
        }
        case LirOpcode::FieldPtr: {
            const LirReg base = instr.srcs[0];
            const auto baseType = regTypes.find(base);
            const int offset =
                baseType == regTypes.end() ? 0 : FieldOffsetOf(baseType->second, instr.strArg, layouts, interfaceNames);
            const A64Reg addr = A64::Xn(kTemp);
            LoadPointer(addr, base);
            if (offset != 0) {
                Must(enc.AddSubLargeImm(addr, addr, offset), "the address of a field");
            }
            StoreToSlot(addr, instr.dst, TypeRef::MakePointer(instr.type));
            break;
        }
        case LirOpcode::IndexPtr: {
            if (instr.srcs.size() < 2) {
                Report(std::format("AArch64 code generation reached an index with no subscript in '{}'", currentFunc));
                break;
            }
            // The element width is what the result points at, which is the
            // shared layout rule rather than anything the index carries.
            const bool known = instr.type.kind == TypeRef::Kind::Pointer && !instr.type.inner.empty();
            const int elemSize = std::max(known ? RuntimeSize(instr.type.inner[0]) : 8, 1);

            const A64Reg addr = A64::Xn(kTemp);
            const A64Reg index = A64::Xn(kAddr);
            LoadPointer(addr, instr.srcs[0]);
            const auto indexType = regTypes.find(instr.srcs[1]);
            LoadFromSlot(index, instr.srcs[1], indexType == regTypes.end() ? TypeRef::MakeInt64() : indexType->second);

            // A power-of-two element scales inside the addition itself;
            // anything else is a multiply, which MADD folds into the same
            // instruction as the addition.
            const auto shift = static_cast<unsigned>(std::countr_zero(static_cast<unsigned>(elemSize)));
            if (std::has_single_bit(static_cast<unsigned>(elemSize)) && shift < 64) {
                Must(enc.Add(addr, addr, index, A64ShiftKind::Lsl, shift), "an element address");
            }
            else {
                const A64Reg width = A64::Xn(kTemp2);
                Must(enc.LoadImm64(width, static_cast<std::uint64_t>(elemSize)), "an element width");
                Must(enc.Madd(addr, index, width, addr), "an element address");
            }
            StoreToSlot(addr, instr.dst, TypeRef::MakePointer(instr.type));
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

    // Module-level data
    //
    // A vtable is a run of function pointers, each of which is a whole address
    // rather than a field of an instruction, so these are the one place this
    // back end emits the architecture-neutral Abs64 the x86-64 one uses
    // everywhere.
    void EmitVtables() {
        for (const auto &vt : mod.vtables) {
            AlignRodata(8);

            RcuSymbol sym;
            sym.name = vt.label;
            sym.sectionIdx = RCU_RODATA_IDX;
            sym.value = static_cast<std::uint32_t>(rodataData.size());
            sym.size = static_cast<std::uint32_t>(vt.methods.size() * 8);
            sym.kind = RcuSymKind::Const;
            sym.visibility = RcuSymVis::Global;
            dataSyms[vt.label] = AddSymbol(std::move(sym));

            for (const auto &method : vt.methods) {
                const auto slotOff = static_cast<std::uint32_t>(rodataData.size());
                rodataData.insert(rodataData.end(), 8, 0);
                const auto it = funcSyms.find(method);
                AddRodataReloc(slotOff,
                               it != funcSyms.end() ? it->second : GetOrAddExtern(method, RcuSymKind::ExternFunc),
                               RcuRelType::Abs64);
            }
        }
    }

    // Append one element of a constant sequence, little-endian, at the width
    // its type occupies. The literal is read the same way a `const` instruction
    // reads one, so the two agree on what a suffix and a base mean.
    void AppendConstElement(const std::string &literal, const TypeRef &type) {
        const int size = SizeOf(type);
        std::uint64_t bits = 0;
        if (type.kind == TypeRef::Kind::Float64) {
            const double value = ParseFloatLiteral<double>(literal);
            std::memcpy(&bits, &value, 8);
        }
        else if (type.kind == TypeRef::Kind::Float32) {
            const float value = ParseFloatLiteral<float>(literal);
            std::uint32_t narrow = 0;
            std::memcpy(&narrow, &value, 4);
            bits = narrow;
        }
        else if (type.IsBool()) {
            bits = literal == "true" || literal == "1" ? 1 : 0;
        }
        else {
            bits = ParseIntegerLiteralBits(literal).value_or(0);
        }
        for (int i = 0; i < size; ++i) {
            rodataData.push_back(static_cast<std::uint8_t>(bits >> (8 * i) & 0xFFU));
        }
    }

    // A slice constant becomes two read-only symbols: its elements, and a
    // {data, length} header under the constant's own name whose data field is
    // relocated to point at them. Code then reaches the elements the same way
    // it reaches those of any other slice.
    void EmitConstSlice(const LirConstDecl &c) {
        const int elemSize = std::max(SizeOf(c.elementType), 1);
        const std::uint32_t elemsOff = AlignRodata(std::min(elemSize, 8));
        std::uint64_t length = 0;
        if (c.isTextSlice) {
            for (const unsigned char byte : c.text) {
                rodataData.push_back(byte);
            }
            rodataData.push_back(0); // keep C interop's terminator
            length = c.text.size();
        }
        else {
            for (const auto &element : c.elements) {
                AppendConstElement(element, c.elementType);
            }
            length = c.elements.size();
        }
        const std::uint32_t elemsSym = AddRodataConst(c.name + "$elements", elemsOff);

        const std::uint32_t headerOff = AlignRodata(8);
        rodataData.insert(rodataData.end(), 16, 0);
        AddRodataReloc(headerOff, elemsSym, RcuRelType::Abs64);
        for (int i = 0; i < 8; ++i) {
            rodataData[headerOff + 8 + i] = static_cast<std::uint8_t>(length >> (8 * i) & 0xFFU);
        }

        RcuSymbol header;
        header.name = c.name;
        header.sectionIdx = RCU_RODATA_IDX;
        header.value = headerOff;
        header.size = 16;
        header.kind = RcuSymKind::Const;
        header.visibility = c.isPublic ? RcuSymVis::Global : RcuSymVis::Local;
        header.typeName = c.type.ToString();
        dataSyms[c.name] = AddSymbol(std::move(header));
    }

    void EmitConstArray(const LirConstDecl &c) {
        const std::uint32_t arrayOff = AlignRodata(AlignOf(c.elementType));
        for (const auto &element : c.elements) {
            AppendConstElement(element, c.elementType);
        }

        RcuSymbol array;
        array.name = c.name;
        array.sectionIdx = RCU_RODATA_IDX;
        array.value = arrayOff;
        array.size = static_cast<std::uint32_t>(rodataData.size()) - arrayOff;
        array.kind = RcuSymKind::Const;
        array.visibility = c.isPublic ? RcuSymVis::Global : RcuSymVis::Local;
        array.typeName = c.type.ToString();
        dataSyms[c.name] = AddSymbol(std::move(array));
    }

    // A scalar constant is inlined at every use, so its symbol exists only for
    // something to take the address of, and eight zeroed bytes in .data are
    // what stands behind it.
    void EmitScalarConst(const LirConstDecl &c) {
        RcuSymbol sym;
        sym.name = c.name;
        sym.sectionIdx = RCU_DATA_IDX;
        sym.value = static_cast<std::uint32_t>(dataData.size());
        sym.size = 8;
        sym.kind = RcuSymKind::Const;
        sym.visibility = c.isPublic ? RcuSymVis::Global : RcuSymVis::Local;
        sym.typeName = c.type.ToString();
        dataData.insert(dataData.end(), 8, 0);
        dataSyms[c.name] = AddSymbol(std::move(sym));
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
        for (const auto &c : mod.consts) {
            // A constant of sequence type is addressed rather than inlined, so
            // it needs its contents behind its name and not a placeholder.
            if (!c.hasSequenceData) {
                EmitScalarConst(c);
            }
            else if (c.type.kind == TypeRef::Kind::Array) {
                EmitConstArray(c);
            }
            else {
                EmitConstSlice(c);
            }
        }
        EmitVtables();
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
