// AArch64 RCU code generation: lowers a LirModule to an in-memory RcuFile.

#include "CodeGen/AArch64/RcuEmitter.h"

#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/Registers.h"
#include "CodeGen/FloatLiteral.h"
#include "CodeGen/IntegerLiteral.h"
#include "CodeGen/Layout.h"
#include "CodeGen/PhiMoveResolver.h"
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
    unsigned lsb = 0;       // low bit of the immediate field
    unsigned width = 0;     // width of that field, in bits
    std::uint64_t site = 0; // which branch of the function this is
};

// Which branch of a function a patch site is, which has to be a name the next
// pass over the same function arrives at again: an offset changes when a branch
// ahead of it is widened, and the block and the position inside its terminator
// do not.
constexpr std::uint64_t kNoSite = ~0ULL;

[[nodiscard]] constexpr std::uint64_t BranchSite(const std::uint32_t block, const unsigned ordinal) {
    return static_cast<std::uint64_t>(block) << 32U | ordinal;
}

// A one-instruction conditional branch: the condition-code form, and the
// compare-and-branch pair that tests a register against zero. All three keep a
// 19-bit signed immediate at bit 5, so all three reach a megabyte of code and
// all three are widened the same way when the target turns out to be further.
struct CondBranch {
    enum class Form : std::uint8_t {
        Condition, // B.<cond>
        Zero,      // CBZ
        NotZero,   // CBNZ
    };

    Form form = Form::Condition;
    A64Condition cond = A64Condition::Eq;
    A64Reg reg{};

    // The branch taken exactly where this one is not, which is what a widened
    // site branches over its own `B` with.
    [[nodiscard]] CondBranch Inverted() const {
        switch (form) {
        case Form::Zero:
            return {Form::NotZero, cond, reg};
        case Form::NotZero:
            return {Form::Zero, cond, reg};
        default:
            return {Form::Condition, A64::InvertCondition(cond), reg};
        }
    }
};

[[nodiscard]] CondBranch OnCondition(const A64Condition cond) {
    return {CondBranch::Form::Condition, cond, {}};
}

[[nodiscard]] CondBranch OnZero(const A64Reg reg) {
    return {CondBranch::Form::Zero, A64Condition::Eq, reg};
}

// The condition an integer or pointer comparison holds under. Equality reads
// the same flags whatever the operands mean; the four orderings do not, so the
// signedness of the operands chooses between two sets of conditions.
[[nodiscard]] A64Condition IntegerCondition(const LirOpcode op, const bool isSigned) {
    switch (op) {
    case LirOpcode::CmpEq:
        return A64Condition::Eq;
    case LirOpcode::CmpNe:
        return A64Condition::Ne;
    case LirOpcode::CmpLt:
        return isSigned ? A64Condition::Lt : A64::Lo;
    case LirOpcode::CmpLe:
        return isSigned ? A64Condition::Le : A64Condition::Ls;
    case LirOpcode::CmpGt:
        return isSigned ? A64Condition::Gt : A64Condition::Hi;
    default:
        return isSigned ? A64Condition::Ge : A64::Hs;
    }
}

// The condition a floating-point comparison holds under. FCMP leaves N=0, Z=0,
// C=1 and V=1 when either operand is a NaN, and every condition below except NE
// is false in that state: an ordered comparison against a NaN answers false and
// `!=` answers true, which is the same pair of answers the x86-64 back end
// arrives at by testing the parity flag beside each comparison. MI and LS are
// what makes that hold for `<` and `<=`, where the signed LT and LE would be
// satisfied by an unordered result.
[[nodiscard]] A64Condition FloatCondition(const LirOpcode op) {
    switch (op) {
    case LirOpcode::CmpEq:
        return A64Condition::Eq;
    case LirOpcode::CmpNe:
        return A64Condition::Ne;
    case LirOpcode::CmpLt:
        return A64Condition::Mi;
    case LirOpcode::CmpLe:
        return A64Condition::Ls;
    case LirOpcode::CmpGt:
        return A64Condition::Gt;
    default:
        return A64Condition::Ge;
    }
}

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

// The second vector register, for the one floating-point operation that reads
// two values at once and produces no floating-point result: a comparison.
constexpr unsigned kFpTemp2 = 17;

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

// No symbol at all, for a lazily declared runtime helper nothing has reached
// yet. Index zero is a real symbol, so absence needs a value of its own.
constexpr std::uint32_t kNoSymbol = ~0U;

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

    // The synthesized integer exponentiation helper, or kNoSymbol until a `**`
    // reaches it.
    std::uint32_t ipowSym = kNoSymbol;

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

    // The parallel copies each edge of the control-flow graph carries, by the
    // block the edge leaves and then the block it enters, and the frame region
    // one destination is preserved in where the copies form a cycle.
    std::unordered_map<std::uint32_t, std::unordered_map<std::uint32_t, std::vector<PhiMove>>> phiMoves;
    std::int32_t phiTempOff = 0;
    int phiTempBytes = 0;

    // The conditional branches of this function that a previous pass over it
    // found too far from their target to reach in one instruction.
    std::unordered_set<std::uint64_t> widenedSites;

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

    // Runtime helpers
    //
    // A `**` is the one arithmetic operator no instruction performs, so it is a
    // call to a body this object synthesizes rather than an instruction — and a
    // body of its own rather than an expansion at each use, since it is a loop.
    // The symbol is declared the first time one is reached and the body emitted
    // once, after every user function, so a call ahead of it resolves through a
    // relocation like any other local text symbol.

    std::uint32_t EnsureIntPowHelper() {
        if (ipowSym == kNoSymbol) {
            RcuSymbol sym;
            sym.name = "__rux_ipow";
            sym.sectionIdx = RCU_TEXT_IDX;
            sym.value = 0; // filled in by EmitIntPowHelper
            sym.kind = RcuSymKind::Func;
            sym.visibility = RcuSymVis::Local;
            ipowSym = AddSymbol(std::move(sym));
        }
        return ipowSym;
    }

    // X0 = X0 ** X1, with a signed exponent.
    //
    // Exponentiation by squaring, which is the algorithm the x86-64 back end
    // synthesizes and gives the same answers to: a negative exponent yields
    // zero and a zero exponent yields one. The low bits of a two's-complement
    // product do not depend on the bits above them, so this one 64-bit body
    // serves every integer width, and `**` takes on no dependency on libm or a
    // C runtime on any target.
    //
    // It reads and writes X0, X1 and X2 and touches no memory, so it needs no
    // frame of its own and leaves nothing for a caller to save.
    void EmitIntPowHelper() {
        if (ipowSym == kNoSymbol) {
            return;
        }
        currentFunc = "__rux_ipow";
        constexpr std::string_view what = "the exponentiation helper";

        const A64Reg result = A64::Xn(0);
        const A64Reg exponent = A64::Xn(1);
        const A64Reg base = A64::Xn(2);

        const std::uint32_t start = enc.Size();
        symbols[ipowSym].value = start;

        Must(enc.Mov(base, result), what);
        Must(enc.LoadImm64(result, 0), what); // a negative exponent yields zero
        const std::uint32_t negativeBranch = enc.Size();
        Must(enc.Tbnz(exponent, 63, 0), what);
        Must(enc.LoadImm64(result, 1), what);

        const std::uint32_t loop = enc.Size();
        const std::uint32_t doneBranch = enc.Size();
        Must(enc.Cbz(exponent, 0), what);
        const std::uint32_t squareBranch = enc.Size();
        Must(enc.Tbz(exponent, 0, 0), what);
        Must(enc.Mul(result, result, base), what); // an odd exponent takes one base

        const std::uint32_t square = enc.Size();
        Must(enc.Mul(base, base, base), what);
        Must(enc.Asr(exponent, exponent, 1), what);
        Must(enc.B(static_cast<std::int64_t>(loop) - static_cast<std::int64_t>(enc.Size())), what);

        const std::uint32_t done = enc.Size();
        Must(enc.Ret(), what);

        // The three forward branches, each patched into the field its own form
        // keeps its immediate in: fourteen bits for the test-and-branch pair
        // and nineteen for the compare-and-branch one, both starting at bit 5.
        const auto instructions = [](const std::uint32_t from, const std::uint32_t to) {
            return (to - from) / A64Enc::InstrSize;
        };
        enc.PatchField(negativeBranch, 5, 14, instructions(negativeBranch, done));
        enc.PatchField(doneBranch, 5, 19, instructions(doneBranch, done));
        enc.PatchField(squareBranch, 5, 14, instructions(squareBranch, square));

        symbols[ipowSym].size = enc.Size() - start;
        currentFunc.clear();
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

    // Branches
    //
    // A branch to a block further down the function is emitted before the block
    // it names has an offset, so the immediate is filled in once the whole body
    // is laid out. Which form the branch took is decided before that, though,
    // and the conditional forms reach only a megabyte: whether one of them
    // reaches is a question this pass over the function cannot answer, so a site
    // that did not is recorded and the function emitted again — see GenFunc.

    // Fill in every branch immediate, and report whether all of them fitted.
    // A conditional site that did not is remembered as one to widen; nothing
    // else can be, since the 26 bits a `B` carries reach 128 megabytes and no
    // function is that long.
    bool PatchJumps() {
        bool ok = true;
        for (const auto &patch : jumpPatches) {
            if (patch.targetBlock >= blockOffsets.size()) {
                continue;
            }
            const auto target = static_cast<std::int32_t>(blockOffsets[patch.targetBlock]);
            const std::int32_t instructions =
                (target - static_cast<std::int32_t>(patch.patchOff)) / static_cast<std::int32_t>(A64Enc::InstrSize);
            const std::int32_t limit = 1 << (patch.width - 1);
            if (instructions < -limit || instructions >= limit) {
                ok = false;
                if (patch.site == kNoSite) {
                    Report(
                        std::format("AArch64 code generation reached a branch too far to encode in '{}'", currentFunc));
                    continue;
                }
                widenedSites.insert(patch.site);
                continue;
            }
            enc.PatchField(patch.patchOff, patch.lsb, patch.width, static_cast<std::uint32_t>(instructions));
        }
        return ok;
    }

    void EmitJumpTo(const std::uint32_t targetBlock) {
        const std::uint32_t patchOff = enc.Size();
        Must(enc.B(0), "a branch");
        jumpPatches.push_back({patchOff, targetBlock, 0, 26, kNoSite});
    }

    void EmitCondBranch(const CondBranch &branch, const std::int64_t offset) {
        switch (branch.form) {
        case CondBranch::Form::Zero:
            Must(enc.Cbz(branch.reg, offset), "a branch on zero");
            break;
        case CondBranch::Form::NotZero:
            Must(enc.Cbnz(branch.reg, offset), "a branch on a nonzero value");
            break;
        default:
            Must(enc.BCond(branch.cond, offset), "a conditional branch");
            break;
        }
    }

    // A conditional branch to a block, in the one-instruction form or — where a
    // previous pass found the target out of reach — as the inverse of that
    // branch over an unconditional one, which reaches anywhere.
    void EmitCondBranchTo(const CondBranch &branch, const std::uint32_t targetBlock, const std::uint64_t site) {
        if (widenedSites.contains(site)) {
            EmitCondBranch(branch.Inverted(), 2 * A64Enc::InstrSize);
            EmitJumpTo(targetBlock);
            return;
        }
        const std::uint32_t patchOff = enc.Size();
        EmitCondBranch(branch, 0);
        jumpPatches.push_back({patchOff, targetBlock, 5, 19, site});
    }

    // A conditional branch over the instructions emitted right after it, whose
    // target is therefore known as soon as they are: the copies one edge of a
    // branch carries and the other must not run. Only a function with a hundred
    // thousand instructions' worth of copies on one edge could put that target
    // out of reach, which is a report rather than a case to widen.
    [[nodiscard]] std::uint32_t EmitBranchOver(const CondBranch &branch) {
        const std::uint32_t patchOff = enc.Size();
        EmitCondBranch(branch, 0);
        return patchOff;
    }

    void PatchBranchOver(const std::uint32_t patchOff) {
        const std::uint32_t instructions = (enc.Size() - patchOff) / A64Enc::InstrSize;
        if (instructions >= 1U << 18U) {
            Report(std::format("AArch64 code generation reached a branch too far to encode in '{}'", currentFunc));
            return;
        }
        enc.PatchField(patchOff, 5, 19, instructions);
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
        phiMoves.clear();
        phiTempOff = 0;
        phiTempBytes = 0;
        nextOff = kFrameRecordSize;

        for (const auto &p : func.params) {
            regTypes[p.reg] = p.type;
            AllocSlot(p.reg, std::max(8, RuntimeSize(p.type)));
        }
        for (std::uint32_t bi = 0; bi < func.blocks.size(); ++bi) {
            for (const auto &instr : func.blocks[bi].instrs) {
                // A phi's copies belong to the edges that reach it, so they are
                // collected by predecessor here and emitted there. The region
                // that breaks a cycle among them holds one value of the widest
                // type any of them moves.
                if (instr.op == LirOpcode::Phi) {
                    for (const auto &[src, pred] : instr.phiPreds) {
                        phiMoves[pred][bi].push_back({instr.dst, src, instr.type});
                    }
                    phiTempBytes = std::max(phiTempBytes, std::max(8, RuntimeSize(instr.type)));
                }
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
        if (phiTempBytes > 0) {
            phiTempOff = AllocRegion(phiTempBytes);
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

    // Move one value between two places in the frame, at whatever width and in
    // whichever register file its type asks for: an aggregate is a block copy,
    // a float goes through a vector register because that is the only thing that
    // holds one, and everything else is a load and a store.
    void CopyFrameValue(const std::int32_t dstOff, const std::int32_t srcOff, const TypeRef &type) {
        const int size = RuntimeSize(type);
        if (IsAggregate(type) && size > 8) {
            CopyBlock(A64::Fp, dstOff, A64::Fp, srcOff, size, RuntimeAlign(type) >= 8);
            return;
        }
        if (IsFloat(type)) {
            const A64Reg value = type.kind == TypeRef::Kind::Float32 ? A64::Sn(kFpTemp) : A64::Dn(kFpTemp);
            LoadScalar(value, A64::Fp, srcOff, value.bits / 8U, false);
            StoreScalar(value, A64::Fp, dstOff, value.bits / 8U);
            return;
        }
        const A64Reg value = A64::Xn(kTemp);
        const unsigned width = AccessWidth(size);
        LoadScalar(value, A64::Fp, srcOff, width, type.IsSigned());
        StoreScalar(value, A64::Fp, dstOff, width);
    }

    // Phi moves
    //
    // A phi is not an instruction: the value it names is whatever the edge that
    // reached it wrote into its slot, so what a phi lowers to is a copy in each
    // predecessor, emitted on the edge itself. The copies of one edge are
    // parallel — every one of them reads the values that held before any of them
    // ran — so they can form a cycle no order of stores satisfies, and
    // ResolvePhiMoves, which both back ends share, sequences them and says where
    // a destination has to be preserved first.

    [[nodiscard]] bool HasPhiMoves(const std::uint32_t from, const std::uint32_t to) const {
        const auto edges = phiMoves.find(from);
        return edges != phiMoves.end() && edges->second.contains(to);
    }

    void EmitPhiMoves(const std::uint32_t from, const std::uint32_t to) {
        const auto edges = phiMoves.find(from);
        if (edges == phiMoves.end()) {
            return;
        }
        const auto moves = edges->second.find(to);
        if (moves == edges->second.end()) {
            return;
        }
        for (const auto &step : ResolvePhiMoves(moves->second)) {
            if (step.kind == PhiMoveStep::Kind::SaveDestination) {
                CopyFrameValue(phiTempOff, Disp(step.dst), step.type);
                continue;
            }
            CopyFrameValue(Disp(step.dst), step.sourceIsTemporary ? phiTempOff : Disp(step.src), step.type);
        }
    }

    // Integer arithmetic
    //
    // Every one of these computes in a whole 64-bit register whatever width its
    // type is, and the two ends of that are what make a narrow result behave
    // the way the x86-64 back end's does. On the way in, LoadFromSlot extends
    // by the type: a signed one sign-extends and an unsigned one zero-extends,
    // which is what gives SDIV, UDIV and ASRV the narrow answers rather than
    // answers about whatever the slot's upper bytes happened to hold. On the
    // way out, StoreToSlot writes only the bytes the type occupies, so a
    // `uint8` sum wraps because the byte above it is never written and no
    // explicit truncation is emitted anywhere.

    // The type a virtual register holds, for an operand whose width is not the
    // instruction's own — a shift amount, or an index.
    [[nodiscard]] TypeRef TypeOfReg(const LirReg reg) const {
        const auto it = regTypes.find(reg);
        return it == regTypes.end() ? TypeRef::MakeInt64() : it->second;
    }

    // Bring the two operands of a binary integer instruction into X9 and X12,
    // each extended by the type it is read at. A type no arithmetic instruction
    // here reaches — a float, which is Task 26's, or an aggregate, which is
    // nothing's — is reported instead, and `false` means nothing was emitted.
    bool LoadBinaryOperands(const LirInstr &instr, const TypeRef &lhsType, const TypeRef &rhsType) {
        if (!IsScalarInteger(lhsType)) {
            NotImplemented(std::format("the '{}' opcode on '{}'", LirOpcodeName(instr.op), lhsType.ToString()));
            return false;
        }
        if (instr.srcs.size() < 2) {
            Report(std::format("AArch64 code generation reached a '{}' with one operand in '{}'",
                               LirOpcodeName(instr.op), currentFunc));
            return false;
        }
        LoadFromSlot(A64::Xn(kTemp), instr.srcs[0], lhsType);
        LoadFromSlot(A64::Xn(kTemp2), instr.srcs[1], rhsType);
        return true;
    }

    // The same for the one-operand forms, which read X9 alone.
    bool LoadUnaryOperand(const LirInstr &instr, const TypeRef &type) {
        if (!IsScalarInteger(type)) {
            NotImplemented(std::format("the '{}' opcode on '{}'", LirOpcodeName(instr.op), type.ToString()));
            return false;
        }
        if (instr.srcs.empty()) {
            Report(std::format("AArch64 code generation reached a '{}' with no operand in '{}'",
                               LirOpcodeName(instr.op), currentFunc));
            return false;
        }
        LoadFromSlot(A64::Xn(kTemp), instr.srcs[0], type);
        return true;
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
            LoadFromSlot(index, instr.srcs[1], TypeOfReg(instr.srcs[1]));

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
        case LirOpcode::Add:
        case LirOpcode::Sub:
        case LirOpcode::Mul:
        case LirOpcode::And:
        case LirOpcode::Or:
        case LirOpcode::Xor: {
            const TypeRef &type = instr.type;
            if (!LoadBinaryOperands(instr, type, type)) {
                break;
            }
            const A64Reg lhs = A64::Xn(kTemp);
            const A64Reg rhs = A64::Xn(kTemp2);
            A64Status status = A64Status::Ok;
            switch (instr.op) {
            case LirOpcode::Add:
                status = enc.Add(lhs, lhs, rhs);
                break;
            case LirOpcode::Sub:
                status = enc.Sub(lhs, lhs, rhs);
                break;
            case LirOpcode::Mul:
                status = enc.Mul(lhs, lhs, rhs);
                break;
            case LirOpcode::And:
                status = enc.And(lhs, lhs, rhs);
                break;
            case LirOpcode::Or:
                status = enc.Orr(lhs, lhs, rhs);
                break;
            default:
                status = enc.Eor(lhs, lhs, rhs);
                break;
            }
            Must(status, LirOpcodeName(instr.op));
            StoreToSlot(lhs, instr.dst, type);
            break;
        }
        case LirOpcode::Div:
        case LirOpcode::Mod: {
            const TypeRef &type = instr.type;
            if (!LoadBinaryOperands(instr, type, type)) {
                break;
            }
            const A64Reg lhs = A64::Xn(kTemp);
            const A64Reg rhs = A64::Xn(kTemp2);
            // The quotient goes somewhere neither operand is, since a
            // remainder needs both of them back afterwards.
            const A64Reg quotient = A64::Xn(kAddr);
            Must(type.IsSigned() ? enc.Sdiv(quotient, lhs, rhs) : enc.Udiv(quotient, lhs, rhs),
                 LirOpcodeName(instr.op));
            if (instr.op == LirOpcode::Div) {
                StoreToSlot(quotient, instr.dst, type);
                break;
            }
            // AArch64 has no remainder instruction: it is the dividend less
            // the quotient times the divisor, which MSUB is one instruction
            // of. The sign follows from the division, so the signed and the
            // unsigned remainder differ only in which divide came first.
            Must(enc.Msub(lhs, quotient, rhs, lhs), LirOpcodeName(instr.op));
            StoreToSlot(lhs, instr.dst, type);
            break;
        }
        case LirOpcode::Pow: {
            const TypeRef &type = instr.type;
            if (!IsScalarInteger(type)) {
                NotImplemented(std::format("the '{}' opcode on '{}'", LirOpcodeName(instr.op), type.ToString()));
                break;
            }
            if (instr.srcs.size() < 2) {
                Report(std::format("AArch64 code generation reached a 'pow' with one operand in '{}'", currentFunc));
                break;
            }
            // The helper takes its base and its exponent where AAPCS64 puts
            // the first two arguments and answers where it puts a return
            // value. It saves nothing, and neither does this: every value this
            // generator holds lives in a stack slot between instructions, so a
            // call clobbers nothing that is live.
            LoadFromSlot(A64::Xn(kReturn), instr.srcs[0], type);
            LoadFromSlot(A64::Xn(kReturn + 1), instr.srcs[1], TypeOfReg(instr.srcs[1]));
            const std::uint32_t callSite = enc.Size();
            Must(enc.Bl(0), "a call to the exponentiation helper");
            AddTextReloc(callSite, EnsureIntPowHelper(), RcuRelType::AArch64Call26);
            StoreToSlot(A64::Xn(kReturn), instr.dst, type);
            break;
        }
        case LirOpcode::Shl:
        case LirOpcode::Shr:
        case LirOpcode::Lshr: {
            const TypeRef &type = instr.type;
            if (instr.srcs.size() < 2) {
                Report(std::format("AArch64 code generation reached a '{}' with no amount in '{}'",
                                   LirOpcodeName(instr.op), currentFunc));
                break;
            }
            // A logical right shift reads its operand as unsigned whatever the
            // type says, which is the whole of what separates it from `shr`.
            // The amount is read at its own type, since nothing says it has the
            // type of the value being shifted.
            const bool logical = instr.op == LirOpcode::Lshr;
            if (!LoadBinaryOperands(instr, logical ? UnsignedIntegerType(type) : type, TypeOfReg(instr.srcs[1]))) {
                break;
            }
            const A64Reg value = A64::Xn(kTemp);
            const A64Reg amount = A64::Xn(kTemp2);
            // The variable shifts mask the amount to the width of the register
            // they shift, so an over-long shift wraps rather than being
            // undefined — and since both back ends shift a 64-bit register, it
            // wraps at the same 64 the x86-64 shifts do.
            A64Status status = A64Status::Ok;
            if (instr.op == LirOpcode::Shl) {
                status = enc.Lslv(value, value, amount);
            }
            else if (logical || !type.IsSigned()) {
                status = enc.Lsrv(value, value, amount);
            }
            else {
                status = enc.Asrv(value, value, amount);
            }
            Must(status, LirOpcodeName(instr.op));
            StoreToSlot(value, instr.dst, type);
            break;
        }
        case LirOpcode::Neg: {
            const TypeRef &type = instr.type;
            if (!LoadUnaryOperand(instr, type)) {
                break;
            }
            const A64Reg value = A64::Xn(kTemp);
            Must(enc.Neg(value, value), LirOpcodeName(instr.op));
            StoreToSlot(value, instr.dst, type);
            break;
        }
        case LirOpcode::Not: {
            // Logical negation, whose result is a boolean rather than a value
            // of the operand's type: anything that compares equal to zero
            // becomes one and everything else becomes zero.
            if (!LoadUnaryOperand(instr, instr.type)) {
                break;
            }
            const A64Reg value = A64::Xn(kTemp);
            Must(enc.SubsImm(A64::Xzr, value, 0), LirOpcodeName(instr.op));
            Must(enc.Cset(value, A64Condition::Eq), LirOpcodeName(instr.op));
            StoreToSlot(value, instr.dst, TypeRef::MakeBool());
            break;
        }
        case LirOpcode::BitNot: {
            const TypeRef &type = instr.type;
            if (!LoadUnaryOperand(instr, type)) {
                break;
            }
            const A64Reg value = A64::Xn(kTemp);
            // On a boolean `~` is the logical negation, so that `~true` is
            // `false`: complementing the whole register would leave 0xFE in the
            // byte the slot keeps, which loads back as true again.
            Must(type.IsBool() ? enc.EorImm(value, value, 1) : enc.Mvn(value, value), LirOpcodeName(instr.op));
            StoreToSlot(value, instr.dst, type);
            break;
        }
        case LirOpcode::CmpEq:
        case LirOpcode::CmpNe:
        case LirOpcode::CmpLt:
        case LirOpcode::CmpLe:
        case LirOpcode::CmpGt:
        case LirOpcode::CmpGe: {
            if (instr.srcs.size() < 2) {
                Report(std::format("AArch64 code generation reached a '{}' with one operand in '{}'",
                                   LirOpcodeName(instr.op), currentFunc));
                break;
            }
            // What is being compared is the type of the operands rather than
            // the boolean the comparison produces, and the operands agree, so
            // the left one decides both which instruction compares them and
            // which condition then reads the flags it left.
            const auto found = regTypes.find(instr.srcs[0]);
            const TypeRef &operandType = found != regTypes.end() ? found->second : instr.type;
            A64Condition cond = A64Condition::Eq;
            if (IsFloat(operandType)) {
                const bool single = operandType.kind == TypeRef::Kind::Float32;
                const A64Reg lhs = single ? A64::Sn(kFpTemp) : A64::Dn(kFpTemp);
                const A64Reg rhs = single ? A64::Sn(kFpTemp2) : A64::Dn(kFpTemp2);
                LoadFpFromSlot(lhs, instr.srcs[0]);
                LoadFpFromSlot(rhs, instr.srcs[1]);
                Must(enc.Fcmp(lhs, rhs), LirOpcodeName(instr.op));
                cond = FloatCondition(instr.op);
            }
            else {
                // Both operands are read at the left one's type, which is what
                // makes a narrow comparison a comparison of what the two slots
                // mean rather than of the bytes above them.
                if (!LoadBinaryOperands(instr, operandType, operandType)) {
                    break;
                }
                Must(enc.Cmp(A64::Xn(kTemp), A64::Xn(kTemp2)), LirOpcodeName(instr.op));
                cond = IntegerCondition(instr.op, operandType.IsSigned());
            }
            // The flags become a value in the one instruction that names a
            // condition and writes a register: a boolean here is the byte the
            // store below writes, and CSET is where it comes from.
            const A64Reg result = A64::Xn(kTemp);
            Must(enc.Cset(result, cond), LirOpcodeName(instr.op));
            StoreToSlot(result, instr.dst, TypeRef::MakeBool());
            break;
        }
        case LirOpcode::Phi:
            // Nothing: the value arrived in this register's slot along the edge
            // that reached this block, written by the predecessor's terminator.
            break;
        default:
            NotImplemented(std::format("the '{}' opcode", LirOpcodeName(instr.op)));
            break;
        }
    }

    // Compare the value in X9 against a case label. An immediate the arith form
    // reaches is one instruction; anything else — a label past 4095, or a
    // negative one — is materialized first, which is why the encoder's refusal
    // is read rather than assumed away.
    void CompareAgainstCase(const std::string &value) {
        const std::uint64_t bits = ParseIntegerLiteralBits(value).value_or(0);
        if (enc.SubsImm(A64::Xzr, A64::Xn(kTemp), bits) == A64Status::Ok) {
            return;
        }
        const A64Reg label = A64::Xn(kTemp2);
        Must(enc.LoadImm64(label, bits), "a case label");
        Must(enc.Cmp(A64::Xn(kTemp), label), "a case comparison");
    }

    void GenTerm(const std::uint32_t blockIdx, const LirTerminator &term) {
        switch (term.kind) {
        case LirTermKind::Jump: {
            EmitPhiMoves(blockIdx, term.trueTarget);
            EmitJumpTo(term.trueTarget);
            break;
        }
        case LirTermKind::Branch: {
            // The condition is read at its own type, since a `bool8` occupies
            // one byte of its slot and the bytes above it stand for nothing.
            const auto found = regTypes.find(term.cond);
            LoadFromSlot(A64::Xn(kTemp), term.cond, found != regTypes.end() ? found->second : TypeRef::MakeBool());
            const A64Reg cond = A64::Xn(kTemp);
            // With no copies on either edge, the whole terminator is a branch on
            // zero to one target and a branch to the other. With copies on
            // either edge, each edge needs instructions of its own that the
            // other must not run, so both become a short run ending in a jump.
            if (!HasPhiMoves(blockIdx, term.trueTarget) && !HasPhiMoves(blockIdx, term.falseTarget)) {
                EmitCondBranchTo(OnZero(cond), term.falseTarget, BranchSite(blockIdx, 0));
                EmitJumpTo(term.trueTarget);
                break;
            }
            const std::uint32_t toFalse = EmitBranchOver(OnZero(cond));
            EmitPhiMoves(blockIdx, term.trueTarget);
            EmitJumpTo(term.trueTarget);
            PatchBranchOver(toFalse);
            EmitPhiMoves(blockIdx, term.falseTarget);
            EmitJumpTo(term.falseTarget);
            break;
        }
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
        case LirTermKind::Switch: {
            // A chain of comparisons, which is what a jump table would be built
            // out of anyway and is all the x86-64 back end emits: the labels a
            // `match` produces are few and rarely contiguous.
            const auto found = regTypes.find(term.cond);
            LoadFromSlot(A64::Xn(kTemp), term.cond, found != regTypes.end() ? found->second : TypeRef::MakeInt64());
            unsigned ordinal = 0;
            for (const auto &branch : term.cases) {
                CompareAgainstCase(branch.value);
                // A case whose edge carries copies is entered through them,
                // which the comparison that failed has to skip over.
                if (!HasPhiMoves(blockIdx, branch.target)) {
                    EmitCondBranchTo(OnCondition(A64Condition::Eq), branch.target, BranchSite(blockIdx, ordinal++));
                    continue;
                }
                const std::uint32_t toNext = EmitBranchOver(OnCondition(A64Condition::Ne));
                EmitPhiMoves(blockIdx, branch.target);
                EmitJumpTo(branch.target);
                PatchBranchOver(toNext);
            }
            EmitPhiMoves(blockIdx, term.defaultTarget);
            EmitJumpTo(term.defaultTarget);
            break;
        }
        case LirTermKind::Unreachable:
            // The permanently undefined encoding, which is what UD2 is on
            // x86-64: control reaching here is a compiler bug, and a trap says
            // so at the instruction rather than wherever falling through led.
            Must(enc.Udf(0), "an unreachable terminator");
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
        widenedSites.clear();

        // The body is emitted, and emitted again if any conditional branch in it
        // turned out not to reach its target: which form such a branch takes has
        // to be chosen before the offsets that decide it are known, so the
        // choice is corrected rather than guessed. Each pass widens every site
        // the pass before it found short and never narrows one back, so the
        // number of passes is bounded by the number of branches, and a function
        // small enough to have none — which is very nearly all of them — is
        // emitted exactly once.
        const std::uint32_t funcStart = enc.Size();
        const std::size_t relocCount = textRelocs.size();
        const std::uint32_t symIdx = DefineFunction(func, funcStart);
        while (true) {
            jumpPatches.clear();
            EmitPrologue();
            blockOffsets.assign(func.blocks.size(), 0);
            for (std::uint32_t bi = 0; bi < func.blocks.size(); ++bi) {
                blockOffsets[bi] = enc.Size();
                for (const auto &instr : func.blocks[bi].instrs) {
                    GenInstr(instr);
                }
                if (func.blocks[bi].term) {
                    GenTerm(bi, *func.blocks[bi].term);
                }
            }
            const std::size_t widened = widenedSites.size();
            if (PatchJumps() || widenedSites.size() == widened) {
                break;
            }
            // Nothing this pass produced is kept: the constants it interned are
            // reached by name and are found again, but its instructions and the
            // relocations hung on them are about to be emitted a second time.
            textData.resize(funcStart);
            textRelocs.resize(relocCount);
        }

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
        // The runtime helpers the generated code reached, after every user
        // function so a call to one is always a forward reference.
        EmitIntPowHelper();
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
