// AArch64 RCU code generation: lowers a LirModule to an in-memory RcuFile.

#include "CodeGen/AArch64/RcuEmitter.h"

#include "CodeGen/AArch64/Assembler.h"
#include "CodeGen/AArch64/CallLayout.h"
#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/FramePlan.h"
#include "CodeGen/AArch64/Registers.h"
#include "CodeGen/AArch64/RuntimeHelpers.h"
#include "CodeGen/FloatLiteral.h"
#include "CodeGen/IntegerLiteral.h"
#include "CodeGen/Layout.h"
#include "CodeGen/RcuModuleBuilder.h"
#include "Object/Rcu/RcuMetadata.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Rux {
using namespace Layout;

namespace {
using ArgLocation = AArch64ArgumentLocation;
using CallLayout = AArch64CallLayout;

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

[[nodiscard]] CondBranch OnNotZero(const A64Reg reg) {
    return {CondBranch::Form::NotZero, A64Condition::Eq, reg};
}

// How a write is asked of a Unix kernel on this system: the number the call is
// known by, the register that number travels in, and the immediate SVC carries.
// A failed assertion prints its message before it traps, and that print is the
// one thing in this back end that is a property of the operating system rather
// than of the architecture — AAPCS64 settles everything else. Windows reaches
// the same operation through KERNEL32 imports below rather than a kernel trap.
//
// The three answers are the three families of AArch64 kernel: Linux keeps its
// own numbering and takes the number in X8, the BSDs share the historical UNIX
// numbering and take it the same way, and Darwin takes it in X16 and is asked
// through a trap of its own. An operating system with no entry has no assertion
// message, which is a report rather than a wrong system call.
struct WriteSyscall {
    std::uint64_t number = 64;
    unsigned numberReg = 8;
    std::uint16_t trap = 0;
};

[[nodiscard]] std::optional<WriteSyscall> WriteSyscallFor(const Target::OS os) {
    switch (os) {
    case Target::OS::Linux:
        return WriteSyscall{64, 8, 0};
    case Target::OS::MacOS:
        return WriteSyscall{4, 16, 0x80};
    case Target::OS::FreeBSD:
        return WriteSyscall{4, 8, 0};
    default:
        return std::nullopt;
    }
}

// The file descriptor a failed assertion writes to on Unix.
constexpr std::uint64_t kStandardError = 2;

// GetStdHandle's signed pseudo-handle for standard error, carried in X0 as the
// two's-complement value the Windows API receives.
constexpr std::uint64_t kStandardErrorHandle = static_cast<std::uint64_t>(static_cast<std::int64_t>(-12));
constexpr std::string_view kKernel32 = "KERNEL32.DLL";

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

// How many arguments AAPCS64 passes in general-purpose registers: X0 through X7,
// which is also where a callee finds them and why kReturn is the first of them.
// Everything past the eighth travels on the stack.
constexpr unsigned kIntArgRegs = 8;

// The register a caller puts the address of an indirect result in. It is
// neither an argument register nor a result one — a callee returning something
// too large for registers reads it and writes there, and returns nothing.
constexpr unsigned kIndirectResult = 8;

// The vector register a floating-point value is computed in. AAPCS64 preserves
// only the low half of V8 through V15 across a call, so the caller-saved half
// of the file starts at V16 and nothing here has to be saved by a callee.
constexpr unsigned kFpTemp = 16;

// The second vector register, for the one floating-point operation that reads
// two values at once and produces no floating-point result: a comparison.
constexpr unsigned kFpTemp2 = 17;

// The third, for the one floating-point operation that is not an instruction:
// a remainder needs its quotient somewhere neither operand is.
constexpr unsigned kFpTemp3 = 18;

// Whether this call names one of the four reinterpretations the front end
// lowers as a call: a value moves between the register files and no branch is
// taken. They are the x86-64 back end's four names, spelled out rather than
// matched by prefix so that a program's own function is never mistaken for one.
[[nodiscard]] bool IsFloatBitsBuiltin(const std::string &name) {
    return name == "FloatBits32" || name == "FloatBits64" || name == "FloatFromBits32" || name == "FloatFromBits64";
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
                            const Target::OS inputTargetOs, const BuildInfo &inputBuildInfo,
                            std::vector<Diagnostic> &inputDiagnostics)
        : mod(module)
        , structDecls(inputStructDecls)
        , packageInterfaceNames(inputPackageInterfaceNames)
        , pkgName(std::move(inputPackageName))
        , targetOs(inputTargetOs)
        , buildInfo(inputBuildInfo)
        , diagnostics(inputDiagnostics)
        , moduleBuilder({.arch = RcuArch::AArch64,
                         .sourcePath = module.name,
                         .packageName = pkgName,
                         .buildTimestamp = RcuBuildTimestamp(buildInfo),
                         .ruxVersion = RcuCompilerVersion(buildInfo)})
        , enc(moduleBuilder.SectionData(RcuModuleSection::Text))
        , runtimeHelpers(moduleBuilder, constIdx, [this](std::string message) { Report(std::move(message)); })
        , callPlanner(layouts, interfaceNames, inputStructDecls, inputTargetOs) {
    }

    RcuFile Generate();

private:
    const LirModule &mod;
    const std::vector<LirStructDecl> &structDecls;
    const std::vector<std::string> &packageInterfaceNames;
    std::string pkgName;
    Target::OS targetOs;
    const BuildInfo &buildInfo;
    std::vector<Diagnostic> &diagnostics;

    RcuModuleBuilder moduleBuilder;

    // Encoder writes target instructions into the builder-owned text section.
    A64Enc enc;

    // Declared symbols, by name → symbol index
    std::unordered_map<std::string, std::uint32_t> externSyms;
    std::unordered_map<std::string, std::uint32_t> funcSyms;
    std::unordered_map<std::string, std::uint32_t> dataSyms;

    // The counter names interned constants apart; the names are local to the
    // object and mean nothing outside it.
    unsigned constIdx = 0;

    AArch64RuntimeHelperEmitter runtimeHelpers;

    [[nodiscard]] std::vector<std::uint8_t> &TextData() {
        return moduleBuilder.SectionData(RcuModuleSection::Text);
    }

    [[nodiscard]] std::vector<std::uint8_t> &RodataData() {
        return moduleBuilder.SectionData(RcuModuleSection::RoData);
    }

    [[nodiscard]] std::vector<std::uint8_t> &DataData() {
        return moduleBuilder.SectionData(RcuModuleSection::Data);
    }

    [[nodiscard]] std::uint32_t DeclareSymbol(std::string name, std::string typeName, const std::uint8_t kind,
                                              const std::uint8_t visibility) {
        return moduleBuilder
            .DeclareSymbol(
                {.name = std::move(name), .typeName = std::move(typeName), .kind = kind, .visibility = visibility})
            .value_or(kNoSymbol);
    }

    [[nodiscard]] std::uint32_t DefineDataSymbol(std::string name, std::string typeName, const std::uint8_t kind,
                                                 const std::uint8_t visibility, const RcuModuleSection section,
                                                 const std::uint32_t offset, const std::uint32_t size) {
        return moduleBuilder
            .AddDefinition(
                {.name = std::move(name), .typeName = std::move(typeName), .kind = kind, .visibility = visibility},
                section, offset, size)
            .value_or(kNoSymbol);
    }

    // Struct field layouts and the interfaces whose values are fat pointers
    LayoutMap layouts;
    std::unordered_set<std::string> interfaceNames;

    AArch64CallPlanner callPlanner;

    // The immutable placement decisions for the function being emitted. The
    // pointed-to plan lives for every widening pass over that function.
    const AArch64FramePlan *activeFramePlan = nullptr;

    // Per-emission state: where each block began and which branches are still
    // waiting for a block offset.
    std::vector<std::uint32_t> blockOffsets;
    std::vector<JumpPatch> jumpPatches;

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

    std::uint32_t GetOrAddExtern(const std::string &name, const std::uint8_t kind, const std::string &dll = {}) {
        if (const auto it = externSyms.find(name); it != externSyms.end()) {
            return it->second;
        }
        const std::uint32_t idx = moduleBuilder.DeclareExternal(name, kind, dll).value_or(kNoSymbol);
        externSyms[name] = idx;
        return idx;
    }

    // Every function this module defines gets its symbol before any body is
    // emitted, so a call can name a function declared further down the file and
    // a body can be placed at whatever offset it turns out to start at. An
    // extern declaration is predeclared too, for the same reason and one more:
    // the library it names belongs to its symbol, and a call site reached before
    // the declaration would otherwise create that symbol without it.
    void PredeclareFunctions() {
        for (const auto &func : mod.funcs) {
            if (func.isExtern) {
                GetOrAddExtern(func.name, RcuSymKind::ExternFunc, func.dll);
                continue;
            }
            if (funcSyms.contains(func.name)) {
                continue;
            }
            funcSyms[func.name] = DeclareSymbol(func.name, func.returnType.ToString(), RcuSymKind::Func,
                                                func.isPublic ? RcuSymVis::Global : RcuSymVis::Local);
        }
    }

    // The read-only pool
    //
    // A value the instruction set cannot name goes into .rodata and is reached
    // through a symbol, and a value written twice in the same module is emitted
    // once: the literal the source wrote is the key, so two spellings of the
    // same number stay two constants, exactly as they do in the x86-64 pool.

    // Pad .rodata out to `align` and report where the next constant starts.
    std::uint32_t AlignRodata(const int align) {
        return moduleBuilder.AlignSection(RcuModuleSection::RoData, static_cast<std::uint16_t>(align));
    }

    std::uint32_t AddRodataConst(const std::string &name, const std::uint32_t offset) {
        return DefineDataSymbol(name, {}, RcuSymKind::Const, RcuSymVis::Local, RcuModuleSection::RoData, offset,
                                static_cast<std::uint32_t>(RodataData().size()) - offset);
    }

    // `bytes` is already encoded at its element width; the terminator is one
    // more element of zeroes, which AlignRodata's zero fill cannot be relied on
    // to supply.
    std::uint32_t InternStr(const std::string &bytes) {
        if (const auto symbol = moduleBuilder.InternedLiteral("string", bytes)) {
            return *symbol;
        }
        auto &data = RodataData();
        const auto offset = static_cast<std::uint32_t>(data.size());
        for (const unsigned char byte : bytes) {
            data.push_back(byte);
        }
        data.push_back(0);
        const std::uint32_t idx = AddRodataConst(std::format("__str{}", constIdx++), offset);
        (void)moduleBuilder.RecordInternedLiteral("string", bytes, idx);
        return idx;
    }

    std::uint32_t InternF32(const std::string &literal) {
        if (const auto symbol = moduleBuilder.InternedLiteral("f32", literal)) {
            return *symbol;
        }
        const std::uint32_t offset = AlignRodata(4);
        const float value = ParseFloatLiteral<float>(literal);
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, 4);
        for (int i = 0; i < 4; ++i) {
            RodataData().push_back(static_cast<std::uint8_t>(bits >> (8 * i) & 0xFFU));
        }
        const std::uint32_t idx = AddRodataConst(std::format("__f32_{}", constIdx++), offset);
        (void)moduleBuilder.RecordInternedLiteral("f32", literal, idx);
        return idx;
    }

    std::uint32_t InternF64(const std::string &literal) {
        if (const auto symbol = moduleBuilder.InternedLiteral("f64", literal)) {
            return *symbol;
        }
        const std::uint32_t offset = AlignRodata(8);
        const double value = ParseFloatLiteral<double>(literal);
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, 8);
        for (int i = 0; i < 8; ++i) {
            RodataData().push_back(static_cast<std::uint8_t>(bits >> (8 * i) & 0xFFU));
        }
        const std::uint32_t idx = AddRodataConst(std::format("__f64_{}", constIdx++), offset);
        (void)moduleBuilder.RecordInternedLiteral("f64", literal, idx);
        return idx;
    }

    // Relocations

    void AddTextReloc(const std::uint32_t sectionOff, const std::uint32_t symIdx, const std::uint16_t type,
                      const std::int32_t addend = 0) {
        (void)moduleBuilder.AddRelocation(RcuModuleSection::Text, sectionOff, symIdx, type, addend);
    }

    void AddRodataReloc(const std::uint32_t sectionOff, const std::uint32_t symIdx, const std::uint16_t type,
                        const std::int32_t addend = 0) {
        (void)moduleBuilder.AddRelocation(RcuModuleSection::RoData, sectionOff, symIdx, type, addend);
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

    [[nodiscard]] const AArch64FramePlan &FramePlan() const {
        return *activeFramePlan;
    }

    [[nodiscard]] std::int32_t Disp(const LirReg reg) {
        if (const auto it = FramePlan().SlotOffsets().find(reg); it != FramePlan().SlotOffsets().end()) {
            return it->second;
        }
        Report(
            std::format("AArch64 code generation reached register %{} with no stack slot in '{}'", reg, currentFunc));
        return kFrameRecordSize;
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

    // Whether a value of this type is a bit pattern a general-purpose register
    // holds, which is everything that is not a float, an aggregate or a string.
    // Naming the integer kinds instead would miss the two types whose values
    // are integers without being one: an enum, whose value is its discriminant,
    // and the untyped `null` the front end writes as a constant of no type at
    // all. Both are a constant, a comparison and a cast away from a program,
    // and the x86-64 back end reads them the same way.
    [[nodiscard]] bool IsRegisterValue(const TypeRef &t) const {
        return !IsFloat(t) && !IsAggregate(t) && t.kind != TypeRef::Kind::Str;
    }

    // The vector register a value of this type is computed in: the S view for a
    // float32 and the D view for a float64, which is what selects the precision
    // of every floating-point instruction that names it.
    [[nodiscard]] static A64Reg FpReg(const TypeRef &t, const unsigned index) {
        return t.kind == TypeRef::Kind::Float32 ? A64::Sn(index) : A64::Dn(index);
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
        const auto &registerTypes = FramePlan().RegisterTypes();
        const auto it = registerTypes.find(reg);
        return it != registerTypes.end() && it->second.kind == TypeRef::Kind::Pointer && !it->second.inner.empty() &&
               it->second.inner[0] == pointee;
    }

    // The machine register a virtual one lives in, or nothing where it lives in
    // the frame. A general-purpose home is always read and written whole: what
    // narrows a value is the mention that reads it, not the register itself.
    [[nodiscard]] std::optional<A64Reg> GeneralHome(const LirReg reg) const {
        const auto &homes = FramePlan().GeneralRegisterHomes();
        const auto it = homes.find(reg);
        if (it == homes.end()) {
            return std::nullopt;
        }
        return A64::Xn(it->second);
    }

    // A vector home is read at the precision of the value in it, which is fixed
    // by that register's type and so is the same at every mention.
    [[nodiscard]] std::optional<A64Reg> VectorHome(const LirReg reg, const unsigned bits) const {
        const auto &homes = FramePlan().VectorRegisterHomes();
        const auto it = homes.find(reg);
        if (it == homes.end()) {
            return std::nullopt;
        }
        return A64::Vn(it->second, bits);
    }

    // STP writes the frame record at the address it decrements SP to, so the
    // whole frame opens in one instruction whenever its size is inside the
    // pre-indexed reach. A larger frame opens with FrameAdjust — which is where
    // a size past an imm12 becomes a scratch register and a register-form SUB —
    // and stores the record afterwards. Either way X29 ends up at the bottom of
    // the frame, and SP is a multiple of 16 at every instruction boundary,
    // since nothing between the two moves it by anything else.
    // The registers the allocation handed out, in the order it handed them out.
    // Two runs rather than one list, because STP names two registers of one
    // file: a general-purpose register and a vector one have no pair form
    // between them however adjacent their slots are.
    [[nodiscard]] std::vector<A64Reg> SavedRegisters(const bool vectorFile) const {
        std::vector<A64Reg> regs;
        if (vectorFile) {
            regs.reserve(FramePlan().SavedVectorRegisters().size());
            for (const unsigned reg : FramePlan().SavedVectorRegisters()) {
                regs.push_back(A64::Dn(reg));
            }
            return regs;
        }
        regs.reserve(FramePlan().SavedGeneralRegisters().size());
        for (const unsigned reg : FramePlan().SavedGeneralRegisters()) {
            regs.push_back(A64::Xn(reg));
        }
        return regs;
    }

    // Write one run of them into the save area or read it back, two at a time
    // wherever the pair immediate reaches — which is all of it unless the area
    // sits more than 504 bytes up a large frame, where the single forms take
    // over and ResolveMemOperand reaches whatever displacement is left.
    void EmitCalleeSaveRun(const std::vector<A64Reg> &regs, std::int32_t &offset, const bool restore) {
        std::size_t index = 0;
        while (index + 1 < regs.size() && InPairRange(offset)) {
            const A64Reg first = regs[index];
            const A64Reg second = regs[index + 1];
            Must(restore ? enc.Ldp(first, second, A64::Fp, offset) : enc.Stp(first, second, A64::Fp, offset),
                 "the callee-saved registers");
            offset += 16;
            index += 2;
        }
        for (; index < regs.size(); ++index) {
            if (restore) {
                LoadScalar(regs[index], A64::Fp, offset, 8, false);
            }
            else {
                StoreScalar(regs[index], A64::Fp, offset, 8);
            }
            offset += 8;
        }
    }

    // A vector register is saved as its D view: AAPCS64 preserves the low 64
    // bits of V8 through V15 and no more, and 64 bits is every float this back
    // end puts in one.
    void EmitCalleeSaves(const bool restore) {
        if (FramePlan().CalleeSaveOffset() == 0) {
            return;
        }
        std::int32_t offset = FramePlan().CalleeSaveOffset();
        EmitCalleeSaveRun(SavedRegisters(false), offset, restore);
        EmitCalleeSaveRun(SavedRegisters(true), offset, restore);
    }

    // Windows commits a thread stack one guard page at a time. Its downward
    // adjustments therefore take the encoder's inline probing path, while the
    // other systems keep the exact FrameAdjust sequence they emitted before.
    // Small Windows areas pass through ProbeStack too, but it deliberately
    // becomes the same single adjustment below one page.
    void OpenStackArea(const std::int32_t bytes, const std::string_view what) {
        Must(targetOs == Target::OS::Windows ? enc.ProbeStack(bytes) : enc.FrameAdjust(-bytes), what);
    }

    void EmitPrologue() {
        if (FramePlan().FrameSize() <= kInlineFrameLimit) {
            Must(enc.Stp(A64::Fp, A64::Lr, A64::Sp, -FramePlan().FrameSize(), A64IndexMode::PreIndex),
                 "the frame record");
        }
        else {
            OpenStackArea(FramePlan().FrameSize(), "the frame");
            Must(enc.Stp(A64::Fp, A64::Lr, A64::Sp, 0), "the frame record");
        }
        Must(enc.Mov(A64::Fp, A64::Sp), "the frame pointer");
        EmitCalleeSaves(false);
    }

    void EmitEpilogue() {
        EmitCalleeSaves(true);
        if (FramePlan().FrameSize() <= kInlineFrameLimit) {
            Must(enc.Ldp(A64::Fp, A64::Lr, A64::Sp, FramePlan().FrameSize(), A64IndexMode::PostIndex),
                 "the frame record");
        }
        else {
            Must(enc.Ldp(A64::Fp, A64::Lr, A64::Sp, 0), "the frame record");
            Must(enc.FrameAdjust(FramePlan().FrameSize()), "the frame");
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

    // Where a virtual register's value is, which is a slot in the frame or a
    // machine register the allocation gave it. The two are the same to every
    // caller below: one emits an access and the other a move, and what a
    // mention asks for — a width and a signedness — is the same question either
    // way.
    //
    // A general-purpose home holds the whole register and nothing narrows on
    // the way in, exactly as the x86-64 back end does. What makes that correct
    // is that the reads narrow: a store into a slot writes only the bytes the
    // type occupies and a load out of it extends them again, so a `uint8` sum
    // wraps because the load reads a byte — and a load out of a register
    // extends the same byte for the same reason.

    // Bring the value out of `src` at the width `width` and the signedness
    // `sign`, which is the extension the load out of a slot would have made.
    void EmitExtendFromHome(const A64Reg dst, const A64Reg src, const unsigned width, const bool sign) {
        const A64Reg wide = A64::Xn(dst.code);
        const A64Reg narrow = A64::Wn(dst.code);
        const A64Reg source = A64::Wn(src.code);
        A64Status status = A64Status::Ok;
        switch (width) {
        case 1:
            status = sign ? enc.Sxtb(wide, source) : enc.Uxtb(narrow, source);
            break;
        case 2:
            status = sign ? enc.Sxth(wide, source) : enc.Uxth(narrow, source);
            break;
        case 4:
            // The unsigned word is a MOV of the W view, which zeroes the half
            // of the register above it — a whole instruction the extends have
            // no cheaper form of.
            status = sign ? enc.Sxtw(wide, source) : enc.Mov(narrow, source);
            break;
        default:
            status = enc.Mov(wide, A64::Xn(src.code));
            break;
        }
        Must(status, "a value out of its register");
    }

    void StoreWidthToSlot(const A64Reg value, const LirReg reg, const unsigned width) {
        if (const std::optional<A64Reg> home = GeneralHome(reg)) {
            if (home->code != value.code) {
                Must(enc.Mov(*home, A64::Xn(value.code)), "a value into its register");
            }
            return;
        }
        StoreScalar(value, A64::Fp, Disp(reg), width);
    }

    void LoadWidthFromSlot(const A64Reg dst, const LirReg reg, const unsigned width, const bool sign) {
        if (const std::optional<A64Reg> home = GeneralHome(reg)) {
            EmitExtendFromHome(dst, *home, width, sign);
            return;
        }
        LoadScalar(dst, A64::Fp, Disp(reg), width, sign);
    }

    void StoreToSlot(const A64Reg value, const LirReg reg, const TypeRef &type) {
        StoreWidthToSlot(value, reg, AccessWidth(RuntimeSize(type)));
    }

    void StoreFpToSlot(const A64Reg value, const LirReg reg) {
        if (const std::optional<A64Reg> home = VectorHome(reg, value.bits)) {
            if (home->code != value.code) {
                Must(enc.Fmov(*home, value), "a float into its register");
            }
            return;
        }
        StoreScalar(value, A64::Fp, Disp(reg), value.bits / 8U);
    }

    void LoadFromSlot(const A64Reg dst, const LirReg reg, const TypeRef &type) {
        LoadWidthFromSlot(dst, reg, AccessWidth(RuntimeSize(type)), type.IsSigned());
    }

    void LoadFpFromSlot(const A64Reg dst, const LirReg reg) {
        if (const std::optional<A64Reg> home = VectorHome(reg, dst.bits)) {
            if (home->code != dst.code) {
                Must(enc.Fmov(dst, *home), "a float out of its register");
            }
            return;
        }
        LoadScalar(dst, A64::Fp, Disp(reg), dst.bits / 8U, false);
    }

    // A pointer is a doubleword whatever it points at, so bringing one out is
    // one access at a fixed width rather than LoadFromSlot's type-driven one.
    void LoadPointer(const A64Reg dst, const LirReg reg) {
        LoadWidthFromSlot(dst, reg, 8, false);
    }

    // Operands and results
    //
    // Everything above brings a value to a register the caller named, which is
    // what a fixed place — an argument register, a result register — asks for.
    // Selection asks something weaker: it wants the value in *some* register,
    // and a value already in one is already there. The three below answer that,
    // and answering it is where the allocation pays for itself, because a
    // mention of a register that has a home costs no instruction at all.

    // The register an operand can be read from, and whatever it took to put it
    // there. A home holds the whole register, so it answers a mention that
    // wants the whole of it; a narrower mention extends out of it into the
    // scratch, which is the same instruction count a slot would have cost.
    [[nodiscard]] A64Reg ReadWidthOperand(const LirReg reg, const unsigned width, const bool sign,
                                          const A64Reg scratch) {
        if (const std::optional<A64Reg> home = GeneralHome(reg)) {
            if (width == 8) {
                return *home;
            }
            EmitExtendFromHome(scratch, *home, width, sign);
            return scratch;
        }
        LoadScalar(scratch, A64::Fp, Disp(reg), width, sign);
        return scratch;
    }

    [[nodiscard]] A64Reg ReadOperand(const LirReg reg, const TypeRef &type, const A64Reg scratch) {
        return ReadWidthOperand(reg, AccessWidth(RuntimeSize(type)), type.IsSigned(), scratch);
    }

    // The same where only the low bytes of the value matter — a store writes
    // the width its type occupies and reads nothing above it — so the
    // extension every other mention needs is not emitted at all.
    [[nodiscard]] A64Reg ReadRawOperand(const LirReg reg, const unsigned width, const A64Reg scratch) {
        if (const std::optional<A64Reg> home = GeneralHome(reg)) {
            return *home;
        }
        LoadScalar(scratch, A64::Fp, Disp(reg), width, false);
        return scratch;
    }

    // The same for an address, which is a doubleword whatever it points at and
    // so is never the narrow case.
    [[nodiscard]] A64Reg ReadPointerOperand(const LirReg reg, const A64Reg scratch) {
        return ReadWidthOperand(reg, 8, false, scratch);
    }

    [[nodiscard]] A64Reg ReadFloatOperand(const LirReg reg, const A64Reg scratch) {
        if (const std::optional<A64Reg> home = VectorHome(reg, scratch.bits)) {
            return *home;
        }
        LoadScalar(scratch, A64::Fp, Disp(reg), scratch.bits / 8U, false);
        return scratch;
    }

    // Where a result is computed. Naming the home means the instruction that
    // produces the value writes it where every later mention will read it, so
    // the store the frame would have needed is not emitted and StoreToSlot
    // below finds the value already home.
    //
    // Only the instruction that finishes a value may take this: a sequence
    // that still has an operand to read must not write a register another
    // value may be living in, and the pool is shared by every interval that
    // does not overlap.
    [[nodiscard]] A64Reg ResultRegister(const LirReg reg, const A64Reg scratch) const {
        if (const std::optional<A64Reg> home = GeneralHome(reg)) {
            return A64::Gpr(home->code, scratch.bits);
        }
        return scratch;
    }

    [[nodiscard]] A64Reg FloatResultRegister(const LirReg reg, const A64Reg scratch) const {
        if (const std::optional<A64Reg> home = VectorHome(reg, scratch.bits)) {
            return *home;
        }
        return scratch;
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
        const auto &phiMoves = FramePlan().PhiMoves();
        const auto edges = phiMoves.find(from);
        return edges != phiMoves.end() && edges->second.contains(to);
    }

    void EmitPhiMoves(const std::uint32_t from, const std::uint32_t to) {
        const auto &phiMoves = FramePlan().PhiMoves();
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
                CopyFrameValue(FramePlan().PhiTemporaryOffset(), Disp(step.dst), step.type);
                continue;
            }
            CopyFrameValue(Disp(step.dst), step.sourceIsTemporary ? FramePlan().PhiTemporaryOffset() : Disp(step.src),
                           step.type);
        }
    }

    // AAPCS64 calls
    //
    // One convention, whatever the LIR says. Target-aware lowering resolves C
    // calls to AAPCS64 on this architecture, while ordinary Rux calls may still
    // carry `Default`; both select the same procedure call standard. Nothing
    // here reads `instr.callConv` because there is no second AArch64 answer for
    // it to select. The independent C-variadic distinction is consumed when
    // argument classification needs a platform-specific variant.
    //
    // Nothing has to be saved around a call either. Every value this generator
    // computes lives in a stack slot between instructions, and the registers it
    // computes in — X9 through X12, V16 and V17 — are ones AAPCS64 lets a callee
    // clobber, so a call destroys nothing that is live. X30 is the exception, and
    // the prologue has already written it into the frame record.

    // Fill the imaginary stack of a Windows C variadic call. Stack slots are
    // written before registers so the X9 scratch used for them cannot disturb
    // an already-filled argument. A scalar float crosses to the general file
    // by bit-preserving FMOV; an HFA or any other composite is read as ordinary
    // consecutive doublewords, with no SIMD classification at all.
    void EmitWindowsVariadicCallArgs(const std::vector<LirReg> &args, const std::vector<TypeRef> &types,
                                     const CallLayout &layout) {
        const auto loadSlot = [&](const std::size_t index, const std::int32_t sourceOffset, const A64Reg dst) {
            const ArgLocation &loc = layout.args[index];
            if (loc.byReference) {
                Must(enc.AddSubLargeImm(dst, A64::Sp, loc.copyOffset), "the address of an argument copy");
                return;
            }
            if (IsAggregate(types[index])) {
                LoadScalar(dst, A64::Fp, static_cast<std::int64_t>(Disp(args[index])) + sourceOffset, 8, false);
                return;
            }
            if (IsFloat(types[index])) {
                const A64Reg value = FpReg(types[index], kFpTemp);
                LoadFpFromSlot(value, args[index]);
                Must(enc.Fmov(A64::Gpr(dst.code, value.bits), value), "a variadic floating-point argument");
                return;
            }
            LoadFromSlot(dst, args[index], types[index]);
        };

        // Caller-owned copies are part of the real outgoing area irrespective
        // of whether their addresses land in a register or a stack slot.
        for (std::size_t i = 0; i < args.size(); ++i) {
            const ArgLocation &loc = layout.args[i];
            if (!loc.byReference) {
                continue;
            }
            const A64Reg source = A64::Xn(kSrcAddr);
            SlotAddress(source, args[i]);
            CopyBlock(A64::Sp, loc.copyOffset, source, 0, loc.copyBytes, RuntimeAlign(types[i]) >= 8);
        }

        constexpr std::int32_t registerBytes = static_cast<std::int32_t>(kIntArgRegs * 8);
        for (const bool toRegisters : {false, true}) {
            for (std::size_t i = 0; i < args.size(); ++i) {
                const ArgLocation &loc = layout.args[i];
                for (std::int32_t byte = 0; byte < loc.bytes; byte += 8) {
                    const std::int32_t imaginaryOffset = loc.offset + byte;
                    if ((imaginaryOffset < registerBytes) != toRegisters) {
                        continue;
                    }
                    if (toRegisters) {
                        loadSlot(i, byte, A64::Xn(static_cast<unsigned>(imaginaryOffset / 8)));
                    }
                    else {
                        const A64Reg value = A64::Xn(kTemp);
                        loadSlot(i, byte, value);
                        StoreScalar(value, A64::Sp, imaginaryOffset - registerBytes, 8);
                    }
                }
            }
        }
    }

    // Move one argument between the registers AAPCS64 puts it in and the slot it
    // lives in here: out of the slot where a caller fills them, into the slot
    // where a callee spills them. What the two directions have in common is the
    // location, so this is one function and a direction rather than two that
    // would have to be kept in step.
    void MoveRegisterArgument(const ArgLocation &loc, const LirReg reg, const TypeRef &type, const bool toRegisters) {
        // A float of its own goes through the slot helpers rather than the HFA
        // walk below, so that a value the allocation gave a register to is
        // moved between two registers and reaches no memory. An aggregate of
        // one float is still an aggregate and still takes the walk.
        if (loc.kind == ArgLocation::Kind::Vector && loc.count == 1 && IsFloat(type)) {
            const A64Reg value = A64::Vn(loc.first, loc.memberBytes * 8U);
            if (toRegisters) {
                LoadFpFromSlot(value, reg);
            }
            else {
                StoreFpToSlot(value, reg);
            }
            return;
        }
        const auto disp = static_cast<std::int64_t>(Disp(reg));
        if (loc.kind == ArgLocation::Kind::Vector) {
            // The members of an HFA sit at their own width in memory and in one
            // register each, so a pair of float32s is two S registers loaded
            // four bytes apart.
            for (unsigned i = 0; i < loc.count; ++i) {
                const A64Reg member = A64::Vn(loc.first + i, loc.memberBytes * 8U);
                const std::int64_t offset = disp + static_cast<std::int64_t>(i) * loc.memberBytes;
                if (toRegisters) {
                    LoadScalar(member, A64::Fp, offset, loc.memberBytes, false);
                }
                else {
                    StoreScalar(member, A64::Fp, offset, loc.memberBytes);
                }
            }
            return;
        }
        if (loc.count > 1) {
            // A composite of no more than sixteen bytes is a whole number of
            // doublewords here, since its slot was rounded up to one: the last
            // register carries that padding rather than the value beside it.
            for (unsigned i = 0; i < loc.count; ++i) {
                const A64Reg word = A64::Xn(loc.first + i);
                const std::int64_t offset = disp + static_cast<std::int64_t>(i) * 8;
                if (toRegisters) {
                    LoadScalar(word, A64::Fp, offset, 8, false);
                }
                else {
                    StoreScalar(word, A64::Fp, offset, 8);
                }
            }
            return;
        }
        // One register, at the width and the signedness the type asks for: the
        // load extends a narrow value the way AAPCS64 asks a caller to, and the
        // store writes only the bytes the type occupies.
        if (toRegisters) {
            // Apple makes this extension the caller's responsibility. The
            // generic path deliberately keeps its previous eager extension,
            // which is permitted and keeps Linux and Windows output stable.
            const bool appleNarrowInteger = callPlanner.Policy().callerExtendsNarrowIntegers && !IsAggregate(type) &&
                                            !IsFloat(type) && RuntimeSize(type) < 4;
            if (appleNarrowInteger) {
                LoadWidthFromSlot(A64::Xn(loc.first), reg, AccessWidth(RuntimeSize(type)), type.IsSigned());
                return;
            }
            LoadFromSlot(A64::Xn(loc.first), reg, type);
        }
        else {
            StoreToSlot(A64::Xn(loc.first), reg, type);
        }
    }

    // The same movement into the registers a location names, from a value that
    // lives at an address rather than in a slot of its own. A `ret` whose
    // operand is the alloca holding the value is the one place that happens:
    // the slot there holds the address, so reading the slot would return the
    // pointer instead of what it points at.
    void LoadResultFromAddress(const ArgLocation &loc, const A64Reg base, const TypeRef &type) {
        if (loc.kind == ArgLocation::Kind::Vector) {
            for (unsigned i = 0; i < loc.count; ++i) {
                const A64Reg member = A64::Vn(loc.first + i, loc.memberBytes * 8U);
                LoadScalar(member, base, static_cast<std::int64_t>(i) * loc.memberBytes, loc.memberBytes, false);
            }
            return;
        }
        if (loc.count > 1) {
            for (unsigned i = 0; i < loc.count; ++i) {
                LoadScalar(A64::Xn(loc.first + i), base, static_cast<std::int64_t>(i) * 8, 8, false);
            }
            return;
        }
        // One register: an aggregate that fits in it carries whatever padding
        // follows it, and a scalar is read at its own width and signedness.
        const bool aggregate = IsAggregate(type);
        LoadScalar(A64::Xn(loc.first), base, 0, aggregate ? 8U : AccessWidth(RuntimeSize(type)),
                   !aggregate && type.IsSigned());
    }

    // Put the arguments where the callee will look for them. Everything that
    // travels in memory goes first, while no argument register holds anything
    // yet: a value is read out of its slot through X9 and an aggregate is moved
    // through X10 and X11, none of which an argument ever occupies, so neither
    // pass disturbs the other.
    void EmitCallArgs(const std::vector<LirReg> &args, const std::vector<TypeRef> &types, const CallLayout &layout) {
        if (layout.windowsVariadic) {
            EmitWindowsVariadicCallArgs(args, types, layout);
            return;
        }
        for (std::size_t i = 0; i < args.size(); ++i) {
            const ArgLocation &loc = layout.args[i];
            const bool paired = RuntimeAlign(types[i]) >= 8;
            if (loc.byReference) {
                // The copy belongs to the caller, so a callee that writes to its
                // own parameter writes to that rather than to the value here.
                const A64Reg source = A64::Xn(kSrcAddr);
                SlotAddress(source, args[i]);
                CopyBlock(A64::Sp, loc.copyOffset, source, 0, loc.copyBytes, paired);
                if (loc.kind == ArgLocation::Kind::Stack) {
                    const A64Reg address = A64::Xn(kTemp);
                    Must(enc.AddSubLargeImm(address, A64::Sp, loc.copyOffset), "the address of an argument copy");
                    StoreScalar(address, A64::Sp, loc.offset, 8);
                }
                continue;
            }
            if (loc.kind != ArgLocation::Kind::Stack) {
                continue;
            }
            const int size = RuntimeSize(types[i]);
            if (IsAggregate(types[i]) && (size > 8 || callPlanner.Policy().compactStackArguments)) {
                const A64Reg source = A64::Xn(kSrcAddr);
                SlotAddress(source, args[i]);
                CopyBlock(A64::Sp, loc.offset, source, 0, size, paired);
                continue;
            }
            if (IsFloat(types[i])) {
                // A float occupies the low bytes of its doubleword and leaves
                // the rest as it found them, exactly as a narrow integer does.
                const A64Reg value = types[i].kind == TypeRef::Kind::Float32 ? A64::Sn(kFpTemp) : A64::Dn(kFpTemp);
                LoadFpFromSlot(value, args[i]);
                StoreScalar(value, A64::Sp, loc.offset, value.bits / 8U);
                continue;
            }
            const A64Reg value = A64::Xn(kTemp);
            LoadFromSlot(value, args[i], types[i]);
            const unsigned width = callPlanner.Policy().compactStackArguments ? AccessWidth(size) : 8U;
            StoreScalar(value, A64::Sp, loc.offset, width);
        }
        for (std::size_t i = 0; i < args.size(); ++i) {
            const ArgLocation &loc = layout.args[i];
            if (loc.kind == ArgLocation::Kind::Stack) {
                continue;
            }
            if (loc.byReference) {
                Must(enc.AddSubLargeImm(A64::Xn(loc.first), A64::Sp, loc.copyOffset),
                     "the address of an argument copy");
                continue;
            }
            MoveRegisterArgument(loc, args[i], types[i], true);
        }
    }

    // The symbol a direct call names: a function this module defines, or an
    // external one. An extern declaration's `dll` reached its symbol when the
    // module was predeclared, so a call to it carries that library whether the
    // declaration was read before this call site or after it.
    std::uint32_t CallTarget(const std::string &name) {
        if (const auto it = funcSyms.find(name); it != funcSyms.end()) {
            return it->second;
        }
        return GetOrAddExtern(name, RcuSymKind::ExternFunc);
    }

    // Open the outgoing argument area, fill the argument registers, branch, close
    // the area again, and keep what came back.
    void GenCall(const LirInstr &instr, const std::vector<LirReg> &args, const bool indirect) {
        if (instr.isCVariadic != instr.cVariadicFixedParamCount.has_value()) {
            Report(std::format("AArch64 code generation reached inconsistent C variadic call metadata in '{}'",
                               currentFunc));
            return;
        }
        if (instr.cVariadicFixedParamCount &&
            (indirect || *instr.cVariadicFixedParamCount > static_cast<std::uint32_t>(args.size()))) {
            Report(std::format("AArch64 code generation reached an invalid C variadic fixed-parameter count in '{}'",
                               currentFunc));
            return;
        }

        std::vector<TypeRef> types;
        types.reserve(args.size());
        for (const LirReg arg : args) {
            types.push_back(TypeOfReg(arg));
        }
        const CallLayout layout = callPlanner.PlanArguments(types, instr.cVariadicFixedParamCount);
        const bool keepsResult = instr.dst != LirNoReg && !instr.type.IsOpaque();
        const bool indirectResult = keepsResult && callPlanner.ReturnsInMemory(instr.type);

        if (layout.areaBytes > 0) {
            OpenStackArea(layout.areaBytes, "the outgoing argument area");
        }
        EmitCallArgs(args, types, layout);
        if (indirectResult) {
            // A result too large for registers is written where the caller says,
            // and the slot the value already has is somewhere it may be written:
            // nothing else reads that slot until the call has returned.
            SlotAddress(A64::Xn(kIndirectResult), instr.dst);
        }
        if (indirect) {
            // The address is fetched after the arguments and into X9 rather than
            // an argument register, so that fetching it cannot disturb them.
            const A64Reg target = A64::Xn(kTemp);
            LoadPointer(target, instr.srcs[0]);
            Must(enc.Blr(target), "an indirect call");
        }
        else {
            const std::uint32_t callSite = enc.Size();
            Must(enc.Bl(0), "a call");
            AddTextReloc(callSite, CallTarget(instr.strArg), RcuRelType::AArch64Call26);
        }
        if (layout.areaBytes > 0) {
            Must(enc.FrameAdjust(layout.areaBytes), "the outgoing argument area");
        }
        // What came back is kept where every later mention of it will look. A
        // narrow return arrives extended to at least a word with the bits above
        // it unspecified, so the store writes only the bytes the type occupies
        // and reads none of the rest; a result too large for registers needs
        // nothing at all, since the callee has already written it here.
        if (keepsResult && !indirectResult) {
            MoveRegisterArgument(callPlanner.PlanResult(instr.type), instr.dst, instr.type, false);
        }
    }

    // Bring the parameters in from where the caller left them, which is the same
    // classification the caller made read the other way round. What arrived in
    // registers is stored into the slot the prepass gave it; what arrived on the
    // caller's stack is copied down into that slot, since the caller's area sits
    // directly above this frame — the frame record is at the bottom of it, so
    // what the caller wrote at its own stack pointer is at X29 plus the frame
    // size. Every later mention of a parameter is then a read of its slot.
    void EmitParamSpills(const LirFunc &func) {
        std::vector<TypeRef> types;
        types.reserve(func.params.size());
        for (const auto &param : func.params) {
            types.push_back(param.type);
        }
        const CallLayout layout = callPlanner.PlanArguments(types);

        for (std::size_t i = 0; i < func.params.size(); ++i) {
            const LirParam &param = func.params[i];
            const ArgLocation &loc = layout.args[i];
            const int size = RuntimeSize(param.type);
            const bool paired = RuntimeAlign(param.type) >= 8;
            const std::int64_t incoming = static_cast<std::int64_t>(FramePlan().FrameSize()) + loc.offset;
            if (loc.byReference) {
                // The caller's copy is the caller's: it is read into this frame
                // once, and nothing here ever writes through the address again.
                const A64Reg source = A64::Xn(kSrcAddr);
                if (loc.kind == ArgLocation::Kind::Stack) {
                    LoadScalar(source, A64::Fp, incoming, 8, false);
                }
                else {
                    Must(enc.Mov(source, A64::Xn(loc.first)), "the address of an argument");
                }
                CopyBlock(A64::Fp, Disp(param.reg), source, 0, size, paired);
                continue;
            }
            if (loc.kind != ArgLocation::Kind::Stack) {
                MoveRegisterArgument(loc, param.reg, param.type, false);
                continue;
            }
            if (IsAggregate(param.type) && size > 8) {
                CopyBlock(A64::Fp, Disp(param.reg), A64::Fp, incoming, size, paired);
                continue;
            }
            if (IsFloat(param.type)) {
                const A64Reg value = param.type.kind == TypeRef::Kind::Float32 ? A64::Sn(kFpTemp) : A64::Dn(kFpTemp);
                LoadScalar(value, A64::Fp, incoming, value.bits / 8U, false);
                StoreFpToSlot(value, param.reg);
                continue;
            }
            // Read at the width the parameter's own type occupies rather than a
            // whole doubleword: a C caller writes a narrow argument into the low
            // bytes of its stack slot and leaves the rest as it found it.
            const A64Reg value = A64::Xn(kTemp);
            LoadScalar(value, A64::Fp, incoming, AccessWidth(size), param.type.IsSigned());
            StoreToSlot(value, param.reg, param.type);
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
        const auto &registerTypes = FramePlan().RegisterTypes();
        const auto it = registerTypes.find(reg);
        return it == registerTypes.end() ? TypeRef::MakeInt64() : it->second;
    }

    // The two operands of a binary instruction, in whatever registers they turn
    // out to be readable from: their homes where the allocation gave them one
    // and the mention wants the whole register, and X9 and X12 otherwise. A
    // type no arithmetic instruction here reaches — an aggregate, which is
    // nothing's — is reported instead, and an empty answer means nothing was
    // emitted and nothing may be selected.
    struct BinaryOperands {
        A64Reg lhs;
        A64Reg rhs;
    };

    [[nodiscard]] std::optional<BinaryOperands> LoadBinaryOperands(const LirInstr &instr, const TypeRef &lhsType,
                                                                   const TypeRef &rhsType) {
        if (!IsRegisterValue(lhsType)) {
            NotImplemented(std::format("the '{}' opcode on '{}'", LirOpcodeName(instr.op), lhsType.ToString()));
            return std::nullopt;
        }
        if (instr.srcs.size() < 2) {
            Report(std::format("AArch64 code generation reached a '{}' with one operand in '{}'",
                               LirOpcodeName(instr.op), currentFunc));
            return std::nullopt;
        }
        const A64Reg lhs = ReadOperand(instr.srcs[0], lhsType, A64::Xn(kTemp));
        const A64Reg rhs = ReadOperand(instr.srcs[1], rhsType, A64::Xn(kTemp2));
        return BinaryOperands{lhs, rhs};
    }

    // The same for the one-operand forms, which read X9 where they read a
    // scratch register at all.
    [[nodiscard]] std::optional<A64Reg> LoadUnaryOperand(const LirInstr &instr, const TypeRef &type) {
        if (!IsRegisterValue(type)) {
            NotImplemented(std::format("the '{}' opcode on '{}'", LirOpcodeName(instr.op), type.ToString()));
            return std::nullopt;
        }
        if (instr.srcs.empty()) {
            Report(std::format("AArch64 code generation reached a '{}' with no operand in '{}'",
                               LirOpcodeName(instr.op), currentFunc));
            return std::nullopt;
        }
        return ReadOperand(instr.srcs[0], type, A64::Xn(kTemp));
    }

    // The same for a floating-point instruction, whose scratch registers are
    // V16 and V17. Both operands of one are of the instruction's own type — the
    // front end refuses to mix a float with anything else, and refuses to mix
    // the two precisions with each other — so one type selects both registers
    // and the precision of every instruction that reads them.
    [[nodiscard]] std::optional<BinaryOperands> LoadFloatOperands(const LirInstr &instr, const TypeRef &type) {
        if (instr.srcs.size() < 2) {
            Report(std::format("AArch64 code generation reached a '{}' with one operand in '{}'",
                               LirOpcodeName(instr.op), currentFunc));
            return std::nullopt;
        }
        const A64Reg lhs = ReadFloatOperand(instr.srcs[0], FpReg(type, kFpTemp));
        const A64Reg rhs = ReadFloatOperand(instr.srcs[1], FpReg(type, kFpTemp2));
        return BinaryOperands{lhs, rhs};
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

    // One of the four reinterpretations, which is one FMOV between the register
    // files: the two registers hold the same number of bits — a word pairs with
    // an S register and a doubleword with a D one — and nothing is converted on
    // the way. Which direction it is decides which file the slot is read at.
    // Assertions
    //
    // A failed assertion says what failed and where, then stops the process
    // where it failed. Saying it is a write to standard error, reached directly
    // through a Unix system call or through KERNEL32 on Windows so neither path
    // needs a C runtime. Stopping is BRK, which is what UD2 is on x86-64 — an
    // instruction no operand makes valid, so a debugger attached to the process
    // lands on the assertion rather than on whatever unwinding it to a handler
    // reached.
    //
    // Three writes rather than one, for the reason the x86-64 back end makes
    // three: the prefix and the location are constants this object interns, the
    // message is a slice the program built at runtime, and joining them would
    // mean allocating somewhere to join them in.

    // fd, buffer and length are already in X0, X1 and X2.
    void EmitWriteSyscall(const WriteSyscall &call) {
        Must(enc.LoadImm64(A64::Xn(call.numberReg), call.number), "a system call number");
        Must(enc.Svc(call.trap), "a system call");
    }

    // Write a run of bytes this object holds: its address is a page and an
    // offset, and its length is known here rather than at run time.
    void EmitWriteStatic(const WriteSyscall &call, const std::string &text) {
        LoadSymbolAddress(A64::Xn(1), InternStr(text));
        Must(enc.LoadImm64(A64::Xn(2), text.size()), "the length of an assertion message");
        Must(enc.LoadImm64(A64::Xn(0), kStandardError), "the standard error descriptor");
        EmitWriteSyscall(call);
    }

    // A direct Win32 call is an ordinary AAPCS64 branch with an imported symbol
    // as its target. PE linking will turn this relocation into the import thunk
    // selected for that symbol.
    void EmitWindowsCall(const std::uint32_t symbol, const std::string_view name) {
        const std::uint32_t site = enc.Size();
        Must(enc.Bl(0), name);
        AddTextReloc(site, symbol, RcuRelType::AArch64Call26);
    }

    // Fill the arguments shared by all three WriteFile calls. GetStdHandle
    // returns the handle in X0; X3 names the DWORD reserved at SP and X4 is the
    // null OVERLAPPED pointer. The caller fills X1 and X2 after this returns so
    // GetStdHandle cannot clobber the buffer or byte count.
    void PrepareWindowsWrite(const std::uint32_t getStdHandle) {
        Must(enc.LoadImm64(A64::Xn(0), kStandardErrorHandle), "the standard error handle kind");
        EmitWindowsCall(getStdHandle, "a call to GetStdHandle");
        Must(enc.Mov(A64::Xn(3), A64::Sp), "the written-byte count pointer");
        Must(enc.LoadImm64(A64::Xn(4), 0), "a null overlapped pointer");
    }

    void EmitWindowsWriteStatic(const std::uint32_t getStdHandle, const std::uint32_t writeFile,
                                const std::string &text) {
        PrepareWindowsWrite(getStdHandle);
        LoadSymbolAddress(A64::Xn(1), InternStr(text));
        Must(enc.LoadImm64(A64::Xn(2), text.size()), "the length of an assertion message");
        EmitWindowsCall(writeFile, "a call to WriteFile");
    }

    void GenAssert(const LirInstr &instr) {
        const bool isAssertion = instr.op == LirOpcode::Assert;
        if (instr.srcs.size() < (isAssertion ? 2U : 1U)) {
            Report(std::format("AArch64 code generation reached a '{}' with too few operands in '{}'",
                               LirOpcodeName(instr.op), currentFunc));
            return;
        }
        const bool isWindows = targetOs == Target::OS::Windows;
        const auto syscall = WriteSyscallFor(targetOs);
        if (!isWindows && !syscall) {
            NotImplemented("an assertion on this operating system");
            return;
        }

        // A condition that held skips the whole failure path, which is the one
        // thing about this opcode that costs a running program anything. A
        // panic has no condition and no branch.
        std::uint32_t heldBranch = 0;
        if (isAssertion) {
            LoadFromSlot(A64::Xn(kTemp), instr.srcs[0], TypeRef::MakeBool());
            heldBranch = EmitBranchOver(OnNotZero(A64::Xn(kTemp)));
        }

        const std::string function = instr.sourceFunction.empty() ? "<unknown>" : instr.sourceFunction;
        const std::string file = instr.sourceFile.empty() ? "<unknown>" : instr.sourceFile;
        const std::string prefix = isAssertion ? "Assertion failed: " : "Panic: ";
        const std::string suffix =
            std::format("\n  at {} ({}:{}:{})\n", function, file, instr.sourceLine, instr.sourceColumn);

        if (isWindows) {
            const std::uint32_t getStdHandle =
                GetOrAddExtern("GetStdHandle", RcuSymKind::ExternFunc, std::string(kKernel32));
            const std::uint32_t writeFile = GetOrAddExtern("WriteFile", RcuSymKind::ExternFunc, std::string(kKernel32));

            // One aligned doubleword pair gives WriteFile a writable DWORD for
            // lpNumberOfBytesWritten. The failure path ends at BRK, so it never
            // has to close this area; a held assertion branches over it.
            Must(enc.FrameAdjust(-16), "the assertion write area");
            EmitWindowsWriteStatic(getStdHandle, writeFile, prefix);

            const LirReg messageReg = instr.srcs[isAssertion ? 1 : 0];
            PrepareWindowsWrite(getStdHandle);
            LoadPointer(A64::Xn(kAddr), messageReg);
            Must(enc.Ldp(A64::Xn(1), A64::Xn(2), A64::Xn(kAddr), 0), "an assertion message");
            EmitWindowsCall(writeFile, "a call to WriteFile");

            EmitWindowsWriteStatic(getStdHandle, writeFile, suffix);
        }
        else {
            EmitWriteStatic(*syscall, prefix);

            // The message is a `Slice<char8>` the caller built, so what the
            // operand holds is its address and the two doublewords behind it
            // are what the system call takes.
            const LirReg messageReg = instr.srcs[isAssertion ? 1 : 0];
            LoadPointer(A64::Xn(kAddr), messageReg);
            Must(enc.Ldp(A64::Xn(1), A64::Xn(2), A64::Xn(kAddr), 0), "an assertion message");
            Must(enc.LoadImm64(A64::Xn(0), kStandardError), "the standard error descriptor");
            EmitWriteSyscall(*syscall);

            EmitWriteStatic(*syscall, suffix);
        }

        Must(enc.Brk(1), "an assertion trap");
        if (isAssertion) {
            PatchBranchOver(heldBranch);
        }
    }

    void GenFloatBits(const LirInstr &instr) {
        const bool single = instr.strArg.ends_with("32");
        const bool toBits = instr.strArg.starts_with("FloatBits");
        const TypeRef floatType = single ? TypeRef::MakeFloat32() : TypeRef::MakeFloat64();
        const unsigned width = single ? 4 : 8;
        const bool keepsResult = instr.dst != LirNoReg && !instr.type.IsOpaque();
        if (toBits) {
            const A64Reg vector = ReadFloatOperand(instr.srcs[0], FpReg(floatType, kFpTemp));
            const A64Reg result = ResultRegister(instr.dst, A64::Xn(kTemp));
            Must(enc.Fmov(A64::Gpr(result.code, single ? 32 : 64), vector), "a reinterpretation of a float");
            if (keepsResult) {
                StoreWidthToSlot(result, instr.dst, width);
            }
            return;
        }
        const A64Reg general = ReadRawOperand(instr.srcs[0], width, A64::Xn(kTemp));
        const A64Reg vector = FloatResultRegister(instr.dst, FpReg(floatType, kFpTemp));
        Must(enc.Fmov(vector, A64::Gpr(general.code, single ? 32 : 64)), "a reinterpretation of an integer");
        if (keepsResult) {
            StoreFpToSlot(vector, instr.dst);
        }
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
                const A64Reg text = ResultRegister(instr.dst, A64::Xn(kTemp));
                LoadSymbolAddress(text, InternStr(instr.strArg));
                StoreToSlot(text, instr.dst, instr.type);
                break;
            }
            if (IsFloat(instr.type)) {
                const A64Reg value = FloatResultRegister(instr.dst, FpReg(instr.type, kFpTemp));
                LoadFloatConstant(value, instr.type, instr.strArg);
                StoreFpToSlot(value, instr.dst);
                break;
            }
            if (!IsRegisterValue(instr.type)) {
                NotImplemented(std::format("a constant of type '{}'", instr.type.ToString()));
                break;
            }
            // Everything else — an integer of any width, a boolean, a
            // character, an enum's discriminant and the null pointer alike — is
            // a bit pattern a general-purpose register holds. It is
            // materialized at full width whatever the type, and the store
            // writes only the bytes the type occupies, so the shortest sequence
            // for the value is the one that gets emitted.
            const A64Reg value = ResultRegister(instr.dst, A64::Xn(kTemp));
            Must(enc.LoadImm64(value, ConstantBits(instr)), "a constant");
            StoreToSlot(value, instr.dst, instr.type);
            break;
        }
        case LirOpcode::Alloca: {
            const auto &allocaData = FramePlan().AllocaDataOffsets();
            const auto it = allocaData.find(instr.dst);
            if (it == allocaData.end()) {
                Report(std::format("AArch64 code generation reached an alloca with no storage in '{}'", currentFunc));
                break;
            }
            // The storage was reserved by the prepass and sits above the frame
            // record, so its address is the frame pointer plus a displacement
            // rather than minus one as it is on x86-64.
            const A64Reg addr = ResultRegister(instr.dst, A64::Xn(kTemp));
            Must(enc.AddSubLargeImm(addr, A64::Fp, it->second), "the address of a local");
            StoreToSlot(addr, instr.dst, TypeRef::MakePointer(instr.type));
            break;
        }
        case LirOpcode::Assert:
        case LirOpcode::Panic:
            GenAssert(instr);
            break;
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
            const A64Reg addr = ResultRegister(instr.dst, A64::Xn(kTemp));
            LoadSymbolAddress(addr, symIdx);
            StoreToSlot(addr, instr.dst, TypeRef::MakePointer(instr.type));
            break;
        }
        case LirOpcode::StringAddr: {
            const TypeRef elemType = instr.type.inner.empty() ? TypeRef::MakeChar8() : instr.type.inner[0];
            const std::uint32_t symIdx = InternStr(EncodeStringLiteral(instr.strArg, RuntimeSize(elemType)));
            const A64Reg addr = ResultRegister(instr.dst, A64::Xn(kTemp));
            LoadSymbolAddress(addr, symIdx);
            StoreToSlot(addr, instr.dst, instr.type);
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
                const A64Reg value = ResultRegister(instr.dst, A64::Xn(kTemp));
                A64SymbolRef ref{};
                Must(enc.LoadFromSymbol(value, ref), "a load from a symbol");
                const std::uint32_t symIdx = GetOrAddExtern(instr.strArg, RcuSymKind::ExternData);
                AddTextReloc(ref.adrp, symIdx, RcuRelType::AArch64AdrPrelPgHi21);
                AddTextReloc(ref.lo12, symIdx, RcuRelType::AArch64LdstAbsLo12Nc);
                StoreToSlot(value, instr.dst, type);
                break;
            }
            const A64Reg addr = ReadPointerOperand(instr.srcs[0], A64::Xn(kAddr));
            // An aggregate is copied into the destination's slot rather than
            // brought into a register, since no register holds one.
            if (IsAggregate(type) && size > 8) {
                CopyBlock(A64::Fp, Disp(instr.dst), addr, 0, size, RuntimeAlign(type) >= 8);
                break;
            }
            if (IsFloat(type)) {
                const A64Reg value = FloatResultRegister(instr.dst, FpReg(type, kFpTemp));
                LoadScalar(value, addr, 0, value.bits / 8U, false);
                StoreFpToSlot(value, instr.dst);
                break;
            }
            const A64Reg value = ResultRegister(instr.dst, A64::Xn(kTemp));
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
            const A64Reg addr = ReadPointerOperand(instr.srcs[1], A64::Xn(kAddr));
            if (IsAggregate(type) && size > 8) {
                A64Reg source = A64::Xn(kSrcAddr);
                if (IsRegPointerTo(valReg, type)) {
                    source = ReadPointerOperand(valReg, source);
                }
                else {
                    SlotAddress(source, valReg);
                }
                CopyBlock(addr, 0, source, 0, size, RuntimeAlign(type) >= 8);
                break;
            }
            if (IsFloat(type)) {
                const A64Reg value = ReadFloatOperand(valReg, FpReg(type, kFpTemp));
                StoreScalar(value, addr, 0, value.bits / 8U);
                break;
            }
            const A64Reg value = ReadRawOperand(valReg, AccessWidth(size), A64::Xn(kTemp));
            StoreScalar(value, addr, 0, AccessWidth(size));
            break;
        }
        case LirOpcode::FieldPtr: {
            const LirReg base = instr.srcs[0];
            const auto &registerTypes = FramePlan().RegisterTypes();
            const auto baseType = registerTypes.find(base);
            const int offset = baseType == registerTypes.end()
                                 ? 0
                                 : FieldOffsetOf(baseType->second, instr.strArg, layouts, interfaceNames);
            const A64Reg source = ReadPointerOperand(base, A64::Xn(kTemp));
            const A64Reg addr = ResultRegister(instr.dst, A64::Xn(kTemp));
            if (offset != 0) {
                Must(enc.AddSubLargeImm(addr, source, offset), "the address of a field");
            }
            else if (addr.code != source.code) {
                Must(enc.Mov(addr, source), "the address of a field");
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

            const A64Reg source = ReadPointerOperand(instr.srcs[0], A64::Xn(kTemp));
            const A64Reg index = ReadOperand(instr.srcs[1], TypeOfReg(instr.srcs[1]), A64::Xn(kAddr));
            const A64Reg addr = ResultRegister(instr.dst, A64::Xn(kTemp));

            // A power-of-two element scales inside the addition itself;
            // anything else is a multiply, which MADD folds into the same
            // instruction as the addition.
            const auto shift = static_cast<unsigned>(std::countr_zero(static_cast<unsigned>(elemSize)));
            if (std::has_single_bit(static_cast<unsigned>(elemSize)) && shift < 64) {
                Must(enc.Add(addr, source, index, A64ShiftKind::Lsl, shift), "an element address");
            }
            else {
                const A64Reg width = A64::Xn(kTemp2);
                Must(enc.LoadImm64(width, static_cast<std::uint64_t>(elemSize)), "an element width");
                Must(enc.Madd(addr, index, width, source), "an element address");
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
            if (IsFloat(type)) {
                // Only the three arithmetic operators of the six have a
                // floating-point form; the bitwise ones are refused by the
                // front end before they reach a back end, and the x86-64
                // emitter's fall-through to an addition for them stands for
                // nothing a program can write.
                if (instr.op != LirOpcode::Add && instr.op != LirOpcode::Sub && instr.op != LirOpcode::Mul) {
                    NotImplemented(std::format("the '{}' opcode on '{}'", LirOpcodeName(instr.op), type.ToString()));
                    break;
                }
                const auto operands = LoadFloatOperands(instr, type);
                if (!operands) {
                    break;
                }
                const A64Reg out = FloatResultRegister(instr.dst, FpReg(type, kFpTemp));
                A64Status status = A64Status::Ok;
                if (instr.op == LirOpcode::Add) {
                    status = enc.Fadd(out, operands->lhs, operands->rhs);
                }
                else if (instr.op == LirOpcode::Sub) {
                    status = enc.Fsub(out, operands->lhs, operands->rhs);
                }
                else {
                    status = enc.Fmul(out, operands->lhs, operands->rhs);
                }
                Must(status, LirOpcodeName(instr.op));
                StoreFpToSlot(out, instr.dst);
                break;
            }
            const auto operands = LoadBinaryOperands(instr, type, type);
            if (!operands) {
                break;
            }
            const A64Reg lhs = operands->lhs;
            const A64Reg rhs = operands->rhs;
            const A64Reg out = ResultRegister(instr.dst, A64::Xn(kTemp));
            A64Status status = A64Status::Ok;
            switch (instr.op) {
            case LirOpcode::Add:
                status = enc.Add(out, lhs, rhs);
                break;
            case LirOpcode::Sub:
                status = enc.Sub(out, lhs, rhs);
                break;
            case LirOpcode::Mul:
                status = enc.Mul(out, lhs, rhs);
                break;
            case LirOpcode::And:
                status = enc.And(out, lhs, rhs);
                break;
            case LirOpcode::Or:
                status = enc.Orr(out, lhs, rhs);
                break;
            default:
                status = enc.Eor(out, lhs, rhs);
                break;
            }
            Must(status, LirOpcodeName(instr.op));
            StoreToSlot(out, instr.dst, type);
            break;
        }
        case LirOpcode::Div:
        case LirOpcode::Mod: {
            const TypeRef &type = instr.type;
            if (IsFloat(type)) {
                const auto operands = LoadFloatOperands(instr, type);
                if (!operands) {
                    break;
                }
                const A64Reg lhs = operands->lhs;
                const A64Reg rhs = operands->rhs;
                const A64Reg out = FloatResultRegister(instr.dst, FpReg(type, kFpTemp));
                if (instr.op == LirOpcode::Div) {
                    Must(enc.Fdiv(out, lhs, rhs), LirOpcodeName(instr.op));
                    StoreFpToSlot(out, instr.dst);
                    break;
                }
                // A floating-point remainder is no instruction either: it is
                // the dividend less its quotient truncated toward zero times
                // the divisor, which is the sequence the x86-64 back end
                // synthesizes too. Two differences, both in this one's favor:
                // FRINTZ truncates in the register rather than through a
                // 64-bit integer, so a quotient past the reach of one still has
                // an answer, and FMSUB rounds the product and the subtraction
                // once between them rather than twice.
                const A64Reg quotient = FpReg(type, kFpTemp3);
                Must(enc.Fdiv(quotient, lhs, rhs), LirOpcodeName(instr.op));
                Must(enc.Frintz(quotient, quotient), LirOpcodeName(instr.op));
                Must(enc.Fmsub(out, quotient, rhs, lhs), LirOpcodeName(instr.op));
                StoreFpToSlot(out, instr.dst);
                break;
            }
            const auto operands = LoadBinaryOperands(instr, type, type);
            if (!operands) {
                break;
            }
            const A64Reg lhs = operands->lhs;
            const A64Reg rhs = operands->rhs;
            // A division writes its answer where the answer lives; a remainder
            // needs both operands back afterwards, so its quotient goes
            // somewhere neither of them is.
            const bool divide = instr.op == LirOpcode::Div;
            const A64Reg quotient = divide ? ResultRegister(instr.dst, A64::Xn(kAddr)) : A64::Xn(kAddr);
            Must(type.IsSigned() ? enc.Sdiv(quotient, lhs, rhs) : enc.Udiv(quotient, lhs, rhs),
                 LirOpcodeName(instr.op));
            if (divide) {
                StoreToSlot(quotient, instr.dst, type);
                break;
            }
            // AArch64 has no remainder instruction: it is the dividend less
            // the quotient times the divisor, which MSUB is one instruction
            // of. The sign follows from the division, so the signed and the
            // unsigned remainder differ only in which divide came first.
            const A64Reg remainder = ResultRegister(instr.dst, A64::Xn(kTemp));
            Must(enc.Msub(remainder, quotient, rhs, lhs), LirOpcodeName(instr.op));
            StoreToSlot(remainder, instr.dst, type);
            break;
        }
        case LirOpcode::Pow: {
            const TypeRef &type = instr.type;
            if (instr.srcs.size() < 2) {
                Report(std::format("AArch64 code generation reached a 'pow' with one operand in '{}'", currentFunc));
                break;
            }
            if (IsFloat(type)) {
                // The floating-point helpers take their base and their exponent
                // in V0 and V1 and answer in V0, which is where AAPCS64 puts
                // the first two arguments and the result of a function of two
                // floats. Neither saves anything, for the reason the integer
                // helper does not.
                const bool single = type.kind == TypeRef::Kind::Float32;
                LoadFpFromSlot(FpReg(type, 0), instr.srcs[0]);
                LoadFpFromSlot(FpReg(type, 1), instr.srcs[1]);
                const std::uint32_t site = enc.Size();
                Must(enc.Bl(0), "a call to the exponentiation helper");
                runtimeHelpers.AddCallRelocation(site, single ? AArch64RuntimeHelper::FloatPower32
                                                              : AArch64RuntimeHelper::FloatPower64);
                StoreFpToSlot(FpReg(type, 0), instr.dst);
                break;
            }
            if (!IsRegisterValue(type)) {
                NotImplemented(std::format("the '{}' opcode on '{}'", LirOpcodeName(instr.op), type.ToString()));
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
            runtimeHelpers.AddCallRelocation(callSite, AArch64RuntimeHelper::IntegerPower);
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
            const auto operands =
                LoadBinaryOperands(instr, logical ? UnsignedIntegerType(type) : type, TypeOfReg(instr.srcs[1]));
            if (!operands) {
                break;
            }
            const A64Reg value = operands->lhs;
            const A64Reg amount = operands->rhs;
            const A64Reg out = ResultRegister(instr.dst, A64::Xn(kTemp));
            // The variable shifts mask the amount to the width of the register
            // they shift, so an over-long shift wraps rather than being
            // undefined — and since both back ends shift a 64-bit register, it
            // wraps at the same 64 the x86-64 shifts do.
            A64Status status = A64Status::Ok;
            if (instr.op == LirOpcode::Shl) {
                status = enc.Lslv(out, value, amount);
            }
            else if (logical || !type.IsSigned()) {
                status = enc.Lsrv(out, value, amount);
            }
            else {
                status = enc.Asrv(out, value, amount);
            }
            Must(status, LirOpcodeName(instr.op));
            StoreToSlot(out, instr.dst, type);
            break;
        }
        case LirOpcode::Neg: {
            const TypeRef &type = instr.type;
            if (IsFloat(type)) {
                if (instr.srcs.empty()) {
                    Report(std::format("AArch64 code generation reached a '{}' with no operand in '{}'",
                                       LirOpcodeName(instr.op), currentFunc));
                    break;
                }
                // FNEG flips the sign bit and reads nothing else, so it is
                // right for a zero and for a NaN alike — where the x86-64 back
                // end needs a mask in .rodata to XOR against, this back end
                // needs no constant at all.
                const A64Reg value = ReadFloatOperand(instr.srcs[0], FpReg(type, kFpTemp));
                const A64Reg out = FloatResultRegister(instr.dst, FpReg(type, kFpTemp));
                Must(enc.Fneg(out, value), LirOpcodeName(instr.op));
                StoreFpToSlot(out, instr.dst);
                break;
            }
            const auto value = LoadUnaryOperand(instr, type);
            if (!value) {
                break;
            }
            const A64Reg out = ResultRegister(instr.dst, A64::Xn(kTemp));
            Must(enc.Neg(out, *value), LirOpcodeName(instr.op));
            StoreToSlot(out, instr.dst, type);
            break;
        }
        case LirOpcode::Not: {
            // Logical negation, whose result is a boolean rather than a value
            // of the operand's type: anything that compares equal to zero
            // becomes one and everything else becomes zero.
            const auto value = LoadUnaryOperand(instr, instr.type);
            if (!value) {
                break;
            }
            const A64Reg out = ResultRegister(instr.dst, A64::Xn(kTemp));
            Must(enc.SubsImm(A64::Xzr, *value, 0), LirOpcodeName(instr.op));
            Must(enc.Cset(out, A64Condition::Eq), LirOpcodeName(instr.op));
            StoreToSlot(out, instr.dst, TypeRef::MakeBool());
            break;
        }
        case LirOpcode::BitNot: {
            const TypeRef &type = instr.type;
            const auto value = LoadUnaryOperand(instr, type);
            if (!value) {
                break;
            }
            const A64Reg out = ResultRegister(instr.dst, A64::Xn(kTemp));
            // On a boolean `~` is the logical negation, so that `~true` is
            // `false`: complementing the whole register would leave 0xFE in the
            // byte the slot keeps, which loads back as true again.
            Must(type.IsBool() ? enc.EorImm(out, *value, 1) : enc.Mvn(out, *value), LirOpcodeName(instr.op));
            StoreToSlot(out, instr.dst, type);
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
            const auto &registerTypes = FramePlan().RegisterTypes();
            const auto found = registerTypes.find(instr.srcs[0]);
            const TypeRef &operandType = found != registerTypes.end() ? found->second : instr.type;
            A64Condition cond = A64Condition::Eq;
            if (IsFloat(operandType)) {
                const auto operands = LoadFloatOperands(instr, operandType);
                if (!operands) {
                    break;
                }
                Must(enc.Fcmp(operands->lhs, operands->rhs), LirOpcodeName(instr.op));
                cond = FloatCondition(instr.op);
            }
            else {
                // Both operands are read at the left one's type, which is what
                // makes a narrow comparison a comparison of what the two slots
                // mean rather than of the bytes above them.
                const auto operands = LoadBinaryOperands(instr, operandType, operandType);
                if (!operands) {
                    break;
                }
                Must(enc.Cmp(operands->lhs, operands->rhs), LirOpcodeName(instr.op));
                cond = IntegerCondition(instr.op, operandType.IsSigned());
            }
            // The flags become a value in the one instruction that names a
            // condition and writes a register: a boolean here is the byte the
            // store below writes, and CSET is where it comes from.
            const A64Reg result = ResultRegister(instr.dst, A64::Xn(kTemp));
            Must(enc.Cset(result, cond), LirOpcodeName(instr.op));
            StoreToSlot(result, instr.dst, TypeRef::MakeBool());
            break;
        }
        case LirOpcode::Cast: {
            if (instr.srcs.empty()) {
                Report(std::format("AArch64 code generation reached a cast with no operand in '{}'", currentFunc));
                break;
            }
            const TypeRef &dstType = instr.type;
            const auto &registerTypes = FramePlan().RegisterTypes();
            const auto found = registerTypes.find(instr.srcs[0]);
            const TypeRef &srcType = found != registerTypes.end() ? found->second : dstType;
            const bool srcFloat = IsFloat(srcType);
            const bool dstFloat = IsFloat(dstType);

            if (!srcFloat && !dstFloat) {
                // An integer to an integer of any width, a pointer to a
                // pointer, an enum to its base and back: none of these is an
                // instruction. The load extends by what the source means and
                // the store keeps only what the destination occupies, so a
                // widening, a narrowing and a reinterpretation are the same
                // pair, and each is already the pair every other opcode uses.
                if (!IsRegisterValue(srcType) || !IsRegisterValue(dstType)) {
                    NotImplemented(std::format("a cast from '{}' to '{}'", srcType.ToString(), dstType.ToString()));
                    break;
                }
                const A64Reg value = ReadOperand(instr.srcs[0], srcType, A64::Xn(kTemp));
                StoreToSlot(value, instr.dst, dstType);
                break;
            }
            if (srcFloat && dstFloat) {
                const A64Reg value = ReadFloatOperand(instr.srcs[0], FpReg(srcType, kFpTemp));
                if (srcType.kind == dstType.kind) {
                    StoreFpToSlot(value, instr.dst);
                    break;
                }
                // FCVT names the source precision in its type field and the
                // destination in its opcode, so it cannot convert a precision
                // to itself and the two registers are never the same view.
                const A64Reg result = FloatResultRegister(instr.dst, FpReg(dstType, kFpTemp2));
                Must(enc.Fcvt(result, value), "a conversion between precisions");
                StoreFpToSlot(result, instr.dst);
                break;
            }
            if (srcFloat) {
                // Toward zero, which is what a cast means in this language and
                // what the x86-64 CVTT pair does. The signedness of the
                // destination picks the instruction, so a `uint64` above 2^63
                // converts rather than saturating the way the x86-64 back end's
                // one signed conversion leaves it.
                const A64Reg value = ReadFloatOperand(instr.srcs[0], FpReg(srcType, kFpTemp));
                const A64Reg result = ResultRegister(instr.dst, A64::Xn(kTemp));
                Must(dstType.IsSigned() ? enc.Fcvtzs(result, value) : enc.Fcvtzu(result, value),
                     "a conversion to an integer");
                StoreToSlot(result, instr.dst, dstType);
                break;
            }
            if (!IsRegisterValue(srcType)) {
                NotImplemented(std::format("a cast from '{}' to '{}'", srcType.ToString(), dstType.ToString()));
                break;
            }
            // An integer to a float, read at the source's own signedness for
            // the same reason: the load has already extended a narrow value, so
            // only a 64-bit unsigned one is a question, and UCVTF answers it.
            const A64Reg value = ReadOperand(instr.srcs[0], srcType, A64::Xn(kTemp));
            const A64Reg result = FloatResultRegister(instr.dst, FpReg(dstType, kFpTemp));
            Must(srcType.IsSigned() ? enc.Scvtf(result, value) : enc.Ucvtf(result, value),
                 "a conversion from an integer");
            StoreFpToSlot(result, instr.dst);
            break;
        }
        case LirOpcode::Call: {
            // Four names the front end lowers as calls are not calls at all:
            // each moves a bit pattern between the register files and takes no
            // branch. They are matched whole, as the x86-64 back end matches
            // them, so that a program's own function whose name merely begins
            // the same way is a call.
            if (IsFloatBitsBuiltin(instr.strArg) && instr.srcs.size() == 1) {
                GenFloatBits(instr);
                break;
            }
            GenCall(instr, instr.srcs, false);
            break;
        }
        case LirOpcode::CallIndirect: {
            if (instr.srcs.empty()) {
                Report(std::format("AArch64 code generation reached an indirect call with no callee in '{}'",
                                   currentFunc));
                break;
            }
            GenCall(instr, {instr.srcs.begin() + 1, instr.srcs.end()}, true);
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
            const auto &registerTypes = FramePlan().RegisterTypes();
            const auto found = registerTypes.find(term.cond);
            LoadFromSlot(A64::Xn(kTemp), term.cond, found != registerTypes.end() ? found->second : TypeRef::MakeBool());
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
                // A value built in place is named by the alloca that holds it,
                // so the operand is a pointer to the result rather than the
                // result: it is read through that address, and its slot is the
                // address itself.
                const bool throughPointer = IsRegPointerTo(*term.retVal, term.retType);
                if (FramePlan().IndirectResultOffset() != 0) {
                    // The caller named the memory and the prologue kept the
                    // address; the value is copied there and no register carries
                    // any of it back.
                    const A64Reg destination = A64::Xn(kAddr);
                    LoadScalar(destination, A64::Fp, FramePlan().IndirectResultOffset(), 8, false);
                    A64Reg source = A64::Xn(kSrcAddr);
                    if (throughPointer) {
                        source = ReadPointerOperand(*term.retVal, source);
                    }
                    else {
                        SlotAddress(source, *term.retVal);
                    }
                    CopyBlock(destination, 0, source, 0, RuntimeSize(term.retType), RuntimeAlign(term.retType) >= 8);
                }
                else if (throughPointer) {
                    LoadResultFromAddress(callPlanner.PlanResult(term.retType),
                                          ReadPointerOperand(*term.retVal, A64::Xn(kSrcAddr)), term.retType);
                }
                else {
                    // X0 and V0 carry everything else, the load extending a
                    // narrow value the way AAPCS64 asks a callee to.
                    MoveRegisterArgument(callPlanner.PlanResult(term.retType), *term.retVal, term.retType, true);
                }
            }
            EmitEpilogue();
            break;
        }
        case LirTermKind::Switch: {
            // A chain of comparisons, which is what a jump table would be built
            // out of anyway and is all the x86-64 back end emits: the labels a
            // `match` produces are few and rarely contiguous.
            const auto &registerTypes = FramePlan().RegisterTypes();
            const auto found = registerTypes.find(term.cond);
            LoadFromSlot(A64::Xn(kTemp), term.cond,
                         found != registerTypes.end() ? found->second : TypeRef::MakeInt64());
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
    std::uint32_t ResolveAsmSymbol(const std::string &name) {
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
    void GenAsmFunc(const LirFunc &func) {
        const std::uint32_t symIdx = funcSyms.contains(func.name)
                                       ? funcSyms.at(func.name)
                                       : DeclareSymbol(func.name, func.returnType.ToString(), RcuSymKind::Func,
                                                       func.isPublic ? RcuSymVis::Global : RcuSymVis::Local);
        funcSyms[func.name] = symIdx;
        if (!moduleBuilder.BeginFunction(symIdx)) {
            return;
        }

        AsmAssembly assembled = AssembleAArch64AsmFunc(func.asmBody, mod.name, TextData());
        for (const auto &fixup : assembled.fixups) {
            (void)moduleBuilder.AddRelocation(RcuModuleSection::Text, fixup.offset, ResolveAsmSymbol(fixup.symbol),
                                              fixup.relType, fixup.addend);
        }
        for (auto &diagnostic : assembled.diagnostics) {
            diagnostics.push_back(std::move(diagnostic));
        }
        (void)moduleBuilder.EndFunction(symIdx);
    }

    void GenFunc(const LirFunc &func) {
        if (func.isExtern) {
            GetOrAddExtern(func.name, RcuSymKind::ExternFunc, func.dll);
            return;
        }
        currentFunc = func.name;
        if (func.isAsm) {
            GenAsmFunc(func);
            currentFunc.clear();
            return;
        }
        const AArch64FramePlan framePlan = PlanAArch64Frame(func, layouts, interfaceNames, structDecls, targetOs);
        activeFramePlan = &framePlan;
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
        const std::size_t relocCount = moduleBuilder.Relocations(RcuModuleSection::Text).size();
        const std::uint32_t symIdx = funcSyms.contains(func.name)
                                       ? funcSyms.at(func.name)
                                       : DeclareSymbol(func.name, func.returnType.ToString(), RcuSymKind::Func,
                                                       func.isPublic ? RcuSymVis::Global : RcuSymVis::Local);
        funcSyms[func.name] = symIdx;
        if (!moduleBuilder.BeginFunction(symIdx)) {
            activeFramePlan = nullptr;
            currentFunc.clear();
            return;
        }
        while (true) {
            jumpPatches.clear();
            EmitPrologue();
            if (FramePlan().IndirectResultOffset() != 0) {
                StoreScalar(A64::Xn(kIndirectResult), A64::Fp, FramePlan().IndirectResultOffset(), 8);
            }
            EmitParamSpills(func);
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
            (void)moduleBuilder.TruncateSection(RcuModuleSection::Text, funcStart, relocCount);
        }

        (void)moduleBuilder.EndFunction(symIdx);
        activeFramePlan = nullptr;
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
            const std::uint32_t vtableOff = AlignRodata(8);
            const std::uint32_t vtableSym = DeclareSymbol(vt.label, {}, RcuSymKind::Const, RcuSymVis::Global);
            dataSyms[vt.label] = vtableSym;

            for (const auto &method : vt.methods) {
                const auto slotOff = static_cast<std::uint32_t>(RodataData().size());
                RodataData().insert(RodataData().end(), 8, 0);
                const auto it = funcSyms.find(method);
                AddRodataReloc(slotOff,
                               it != funcSyms.end() ? it->second : GetOrAddExtern(method, RcuSymKind::ExternFunc),
                               RcuRelType::Abs64);
            }
            (void)moduleBuilder.DefineSymbol(vtableSym, RcuModuleSection::RoData, vtableOff,
                                             static_cast<std::uint32_t>(vt.methods.size() * 8));
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
            RodataData().push_back(static_cast<std::uint8_t>(bits >> (8 * i) & 0xFFU));
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
                RodataData().push_back(byte);
            }
            RodataData().push_back(0); // keep C interop's terminator
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
        RodataData().insert(RodataData().end(), 16, 0);
        AddRodataReloc(headerOff, elemsSym, RcuRelType::Abs64);
        for (int i = 0; i < 8; ++i) {
            RodataData()[headerOff + 8 + i] = static_cast<std::uint8_t>(length >> (8 * i) & 0xFFU);
        }

        dataSyms[c.name] = DefineDataSymbol(c.name, c.type.ToString(), RcuSymKind::Const,
                                            c.isPublic ? RcuSymVis::Global : RcuSymVis::Local, RcuModuleSection::RoData,
                                            headerOff, 16);
    }

    void EmitConstArray(const LirConstDecl &c) {
        const std::uint32_t arrayOff = AlignRodata(AlignOf(c.elementType));
        for (const auto &element : c.elements) {
            AppendConstElement(element, c.elementType);
        }

        dataSyms[c.name] = DefineDataSymbol(c.name, c.type.ToString(), RcuSymKind::Const,
                                            c.isPublic ? RcuSymVis::Global : RcuSymVis::Local, RcuModuleSection::RoData,
                                            arrayOff, static_cast<std::uint32_t>(RodataData().size()) - arrayOff);
    }

    // A scalar constant is inlined at every use, so its symbol exists only for
    // something to take the address of, and eight zeroed bytes in .data are
    // what stands behind it.
    void EmitScalarConst(const LirConstDecl &c) {
        const auto offset = static_cast<std::uint32_t>(DataData().size());
        DataData().insert(DataData().end(), 8, 0);
        dataSyms[c.name] =
            DefineDataSymbol(c.name, c.type.ToString(), RcuSymKind::Const,
                             c.isPublic ? RcuSymVis::Global : RcuSymVis::Local, RcuModuleSection::Data, offset, 8);
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
        runtimeHelpers.EmitRequested();
    }
};

RcuFile AArch64CodeGen::Generate() {
    GenModule();
    auto built = moduleBuilder.Finalize();
    diagnostics.insert(diagnostics.end(), std::make_move_iterator(built.diagnostics.begin()),
                       std::make_move_iterator(built.diagnostics.end()));
    return built.file ? std::move(*built.file) : RcuFile{};
}
} // namespace

AArch64RcuEmitter::AArch64RcuEmitter(const LirPackage &package, std::string inputPackageName,
                                     const Target::OS inputTargetOs, BuildInfo inputBuildInfo)
    : lir(package)
    , packageName(std::move(inputPackageName))
    , targetOs(inputTargetOs)
    , buildInfo(std::move(inputBuildInfo)) {
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
        AArch64CodeGen gen(module, structDecls, interfaceNames, packageName, targetOs, buildInfo, diagnostics);
        result.push_back(gen.Generate());
    }
    return result;
}
} // namespace Rux
