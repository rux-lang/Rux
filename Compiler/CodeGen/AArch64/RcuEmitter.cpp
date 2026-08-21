// AArch64 RCU code generation: lowers a LirModule to an in-memory RcuFile.

#include "CodeGen/AArch64/RcuEmitter.h"

#include "CodeGen/AArch64/Assembler.h"
#include "CodeGen/AArch64/CallAndTerminatorEmitter.h"
#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/FramePlan.h"
#include "CodeGen/AArch64/FunctionEmitter.h"
#include "CodeGen/AArch64/Registers.h"
#include "CodeGen/AArch64/RuntimeHelpers.h"
#include "CodeGen/BackendDiagnostics.h"
#include "CodeGen/ConstantData.h"
#include "CodeGen/FloatLiteral.h"
#include "CodeGen/IntegerLiteral.h"
#include "CodeGen/Layout.h"
#include "CodeGen/RcuModuleBuilder.h"
#include "CodeGen/RuntimeFailure.h"
#include "Object/Rcu/RcuMetadata.h"

#include <algorithm>
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
/// How a write is asked of a Unix kernel on this system: the number the call is known by, the register that number
/// travels in, and the immediate SVC carries. A failed assertion prints its message before it traps, and that print is
/// the one thing in this back end that is a property of the operating system rather than of the architecture — AAPCS64
/// settles everything else. Windows reaches the same operation through KERNEL32 imports below rather than a kernel
/// trap.
///
/// The three answers are the three families of AArch64 kernel: Linux keeps its own numbering and takes the number in
/// X8, the BSDs share the historical UNIX numbering and take it the same way, and Darwin takes it in X16 and is asked
/// through a trap of its own. An operating system with no entry has no assertion message, which is a report rather than
/// a wrong system call.
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

/// The file descriptor a failed assertion writes to on Unix.
constexpr std::uint64_t kStandardError = 2;

/// GetStdHandle's signed pseudo-handle for standard error, carried in X0 as the two's-complement value the Windows API
/// receives.
constexpr std::uint64_t kStandardErrorHandle = static_cast<std::uint64_t>(static_cast<std::int64_t>(-12));
constexpr std::string_view kKernel32 = "KERNEL32.DLL";

/// The frame record — the caller's frame pointer and the return address — that every prologue stores and every epilogue
/// restores.
constexpr std::int32_t kFrameRecordSize = 16;

/// The largest frame a single pre-indexed STP can open: its 7-bit immediate counts pairs of doublewords, so it reaches
/// 64 of them below the stack pointer. A frame past this opens with FrameAdjust instead.
constexpr std::int32_t kInlineFrameLimit = 512;

/// The register this generator computes in. AAPCS64 leaves X9 through X15 to the caller, so nothing that has to survive
/// a call lives here, and it is clear of both the argument registers and the X16/X17 pair the encoder's composite
/// sequences take as scratch.
constexpr unsigned kTemp = 9;

/// The register an address is computed in. A load reads through it and a store writes through it, so it is never the
/// register the value itself is in.
constexpr unsigned kAddr = 10;

/// The second value register. LDP and STP move two registers at a time, and a multiply by an element width needs
/// somewhere to put the width.
constexpr unsigned kTemp2 = 12;

/// The register a caller puts the address of an indirect result in. It is neither an argument register nor a result one
/// — a callee returning something too large for registers reads it and writes there, and returns nothing.
constexpr unsigned kIndirectResult = 8;

/// The vector register a floating-point value is computed in. AAPCS64 preserves only the low half of V8 through V15
/// across a call, so the caller-saved half of the file starts at V16 and nothing here has to be saved by a callee.
constexpr unsigned kFpTemp = 16;

/// The access width a scalar of `size` bytes is moved at: the four widths a load or store names, with anything else
/// rounded up to a whole register. A type with no size of its own — an opaque one — is a whole register too, since that
/// is what its stack slot was given.
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

/// Whether LDP and STP of two doublewords reach `offset`. Their 7-bit immediate counts pairs of doublewords, so it
/// reaches 64 of them either way and cannot name a displacement that is not a multiple of eight at all.
[[nodiscard]] bool InPairRange(const std::int64_t offset) {
    return offset % 8 == 0 && offset >= -512 && offset <= 504;
}

/// No symbol at all, for a lazily declared runtime helper nothing has reached yet. Index zero is a real symbol, so
/// absence needs a value of its own.
constexpr std::uint32_t kNoSymbol = ~0U;

class AArch64CodeGen final : private AArch64FunctionEmitterHooks, private AArch64CallAndTerminatorHooks {
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
        Report(ErrorDiagnostic(std::move(message)));
    }

    void Report(Diagnostic diagnostic) {
        if (reported.insert(diagnostic.message).second) {
            diagnostics.push_back(std::move(diagnostic));
        }
    }

    void ReportFunctionDiagnostic(std::string message) override {
        Report(std::move(message));
    }

    void ReportFunctionDiagnostic(Diagnostic diagnostic) override {
        Report(std::move(diagnostic));
    }

    void ReportCallAndTerminatorDiagnostic(std::string message) override {
        Report(std::move(message));
    }

    void NotImplemented(const std::string_view what) {
        Report(UnsupportedBackendConstructDiagnostic(what, targetOs, Target::Arch::AArch64, currentFunc));
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
    std::uint32_t InternStringLiteral(const std::string &bytes) override {
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

    void AddCallRelocation(const std::uint32_t sectionOffset, const std::uint32_t symbol) override {
        AddTextReloc(sectionOffset, symbol, RcuRelType::AArch64Call26);
    }

    [[nodiscard]] std::uint32_t ResolveCallSymbol(const std::string &name) override {
        if (const auto it = funcSyms.find(name); it != funcSyms.end()) {
            return it->second;
        }
        return GetOrAddExtern(name, RcuSymKind::ExternFunc);
    }

    void AddRodataReloc(const std::uint32_t sectionOff, const std::uint32_t symIdx, const std::uint16_t type,
                        const std::int32_t addend = 0) {
        (void)moduleBuilder.AddRelocation(RcuModuleSection::RoData, sectionOff, symIdx, type, addend);
    }

    // The address of a symbol: the page it sits on, then its offset within that
    // page. Both immediates are emitted as zero and belong to the two
    // relocations hung on them, so the sequence is the same two instructions
    // whatever the symbol turns out to be and wherever the linker puts it.
    void LoadSymbolAddress(const A64Reg rd, const std::uint32_t symIdx) override {
        A64SymbolRef ref{};
        Must(enc.LoadAddress(rd, ref), "the address of a symbol");
        AddTextReloc(ref.adrp, symIdx, RcuRelType::AArch64AdrPrelPgHi21);
        AddTextReloc(ref.lo12, symIdx, RcuRelType::AArch64AddAbsLo12Nc);
    }

    [[nodiscard]] std::uint32_t ResolveGlobalSymbol(const std::string &name) override {
        if (const auto data = dataSyms.find(name); data != dataSyms.end()) {
            return data->second;
        }
        if (const auto func = funcSyms.find(name); func != funcSyms.end()) {
            return func->second;
        }
        return GetOrAddExtern(name, RcuSymKind::ExternData);
    }

    void LoadNamedDataSymbol(const A64Reg destination, const std::string &name) override {
        A64SymbolRef ref{};
        Must(enc.LoadFromSymbol(destination, ref), "a load from a symbol");
        const std::uint32_t symbol = GetOrAddExtern(name, RcuSymKind::ExternData);
        AddTextReloc(ref.adrp, symbol, RcuRelType::AArch64AdrPrelPgHi21);
        AddTextReloc(ref.lo12, symbol, RcuRelType::AArch64LdstAbsLo12Nc);
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

    [[nodiscard]] std::int32_t Disp(const LirReg reg) override {
        if (const auto it = FramePlan().SlotOffsets().find(reg); it != FramePlan().SlotOffsets().end()) {
            return it->second;
        }
        Report(
            std::format("AArch64 code generation reached register %{} with no stack slot in '{}'", reg, currentFunc));
        return kFrameRecordSize;
    }

    [[nodiscard]] int RuntimeSize(const TypeRef &t) const override {
        return RuntimeSizeOf(t, layouts, interfaceNames);
    }

    // The alignment the running program gives a value of this type, which is
    // what decides whether a block copy of it may use the pair forms. AlignOf
    // can only derive an alignment from a size, so a named type answers from
    // its computed layout wherever the package declared one.
    [[nodiscard]] int RuntimeAlign(const TypeRef &t) const override {
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
        return !IsFloat(t) && !IsAggregate(t);
    }

    // Whether a value of this type moves as a block of bytes rather than in one
    // register. What counts as an aggregate is a property of the LIR type
    // rather than of the machine, so this is the x86-64 rule unchanged.
    [[nodiscard]] bool IsAggregate(const TypeRef &t) const override {
        if (IsWideInteger(t) || (IsSoftwareFloat(t) && RuntimeSize(t) > 8)) {
            return true;
        }
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
    void OpenStackArea(const std::int32_t bytes, const std::string_view what) override {
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

    void EmitEpilogue() override {
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

    void StoreScalar(const A64Reg value, const A64Reg base, const std::int64_t offset, const unsigned width) override {
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
                    const bool sign) override {
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

    void StoreWidthToSlot(const A64Reg value, const LirReg reg, const unsigned width) override {
        if (const std::optional<A64Reg> home = GeneralHome(reg)) {
            if (home->code != value.code) {
                Must(enc.Mov(*home, A64::Xn(value.code)), "a value into its register");
            }
            return;
        }
        StoreScalar(value, A64::Fp, Disp(reg), width);
    }

    void LoadWidthFromSlot(const A64Reg dst, const LirReg reg, const unsigned width, const bool sign) override {
        if (const std::optional<A64Reg> home = GeneralHome(reg)) {
            EmitExtendFromHome(dst, *home, width, sign);
            return;
        }
        LoadScalar(dst, A64::Fp, Disp(reg), width, sign);
    }

    void StoreToSlot(const A64Reg value, const LirReg reg, const TypeRef &type) override {
        StoreWidthToSlot(value, reg, AccessWidth(RuntimeSize(type)));
    }

    void StoreFpToSlot(const A64Reg value, const LirReg reg) override {
        if (const std::optional<A64Reg> home = VectorHome(reg, value.bits)) {
            if (home->code != value.code) {
                Must(enc.Fmov(*home, value), "a float into its register");
            }
            return;
        }
        StoreScalar(value, A64::Fp, Disp(reg), value.bits / 8U);
    }

    void LoadFromSlot(const A64Reg dst, const LirReg reg, const TypeRef &type) override {
        LoadWidthFromSlot(dst, reg, AccessWidth(RuntimeSize(type)), type.IsSigned());
    }

    void LoadFpFromSlot(const A64Reg dst, const LirReg reg) override {
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
    void LoadPointer(const A64Reg dst, const LirReg reg) override {
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

    [[nodiscard]] A64Reg ReadOperand(const LirReg reg, const TypeRef &type, const A64Reg scratch) override {
        return ReadWidthOperand(reg, AccessWidth(RuntimeSize(type)), type.IsSigned(), scratch);
    }

    // The same where only the low bytes of the value matter — a store writes
    // the width its type occupies and reads nothing above it — so the
    // extension every other mention needs is not emitted at all.
    [[nodiscard]] A64Reg ReadRawOperand(const LirReg reg, const unsigned width, const A64Reg scratch) override {
        if (const std::optional<A64Reg> home = GeneralHome(reg)) {
            return *home;
        }
        LoadScalar(scratch, A64::Fp, Disp(reg), width, false);
        return scratch;
    }

    // The same for an address, which is a doubleword whatever it points at and
    // so is never the narrow case.
    [[nodiscard]] A64Reg ReadPointerOperand(const LirReg reg, const A64Reg scratch) override {
        return ReadWidthOperand(reg, 8, false, scratch);
    }

    [[nodiscard]] A64Reg ReadFloatOperand(const LirReg reg, const A64Reg scratch) override {
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
    [[nodiscard]] A64Reg ResultRegister(const LirReg reg, const A64Reg scratch) const override {
        if (const std::optional<A64Reg> home = GeneralHome(reg)) {
            return A64::Gpr(home->code, scratch.bits);
        }
        return scratch;
    }

    [[nodiscard]] A64Reg FloatResultRegister(const LirReg reg, const A64Reg scratch) const override {
        if (const std::optional<A64Reg> home = VectorHome(reg, scratch.bits)) {
            return *home;
        }
        return scratch;
    }

    // The address of a value's own slot, for the cases where an aggregate is
    // held in the frame rather than behind a pointer.
    void SlotAddress(const A64Reg dst, const LirReg reg) override {
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
                   const int size, const bool paired) override {
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
    void CopyFrameValue(const std::int32_t dstOff, const std::int32_t srcOff, const TypeRef &type) override {
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

    // Instruction selection

    // Bring a floating-point constant into `dst`. FMOV names 256 values
    // outright, which covers most of what a program writes down; anything else
    // — an exact fraction the encoding misses, or a value with more precision
    // than it carries — is a word or a doubleword in the read-only pool,
    // reached in two instructions rather than the four or five a MOVZ chain
    // through a general-purpose register would take.
    void LoadFloatConstant(const A64Reg dst, const TypeRef &type, const std::string &literal) override {
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
        LoadSymbolAddress(A64::Xn(1), InternStringLiteral(text));
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
        LoadSymbolAddress(A64::Xn(1), InternStringLiteral(text));
        Must(enc.LoadImm64(A64::Xn(2), text.size()), "the length of an assertion message");
        EmitWindowsCall(writeFile, "a call to WriteFile");
    }

    void GenAssert(AArch64TerminatorEmitter &terminatorEmitter, const LirInstr &instr) {
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
            heldBranch = terminatorEmitter.EmitBranchOverNonZero(A64::Xn(kTemp));
        }

        const RuntimeFailureLayout layout =
            BuildRuntimeFailureLayout(isAssertion ? RuntimeFailureKind::Assertion : RuntimeFailureKind::Panic,
                                      instr.sourceFunction, instr.sourceFile, instr.sourceLine, instr.sourceColumn);

        if (isWindows) {
            const std::uint32_t getStdHandle =
                GetOrAddExtern("GetStdHandle", RcuSymKind::ExternFunc, std::string(kKernel32));
            const std::uint32_t writeFile = GetOrAddExtern("WriteFile", RcuSymKind::ExternFunc, std::string(kKernel32));

            // One aligned doubleword pair gives WriteFile a writable DWORD for
            // lpNumberOfBytesWritten. The failure path ends at BRK, so it never
            // has to close this area; a held assertion branches over it.
            Must(enc.FrameAdjust(-16), "the assertion write area");
            EmitWindowsWriteStatic(getStdHandle, writeFile, layout.prefix);

            const LirReg messageReg = instr.srcs[isAssertion ? 1 : 0];
            PrepareWindowsWrite(getStdHandle);
            LoadPointer(A64::Xn(kAddr), messageReg);
            Must(enc.Ldp(A64::Xn(1), A64::Xn(2), A64::Xn(kAddr), 0), "an assertion message");
            EmitWindowsCall(writeFile, "a call to WriteFile");

            EmitWindowsWriteStatic(getStdHandle, writeFile, layout.location);
        }
        else {
            EmitWriteStatic(*syscall, layout.prefix);

            // The message is a `Slice<char8>` the caller built, so what the
            // operand holds is its address and the two doublewords behind it
            // are what the system call takes.
            const LirReg messageReg = instr.srcs[isAssertion ? 1 : 0];
            LoadPointer(A64::Xn(kAddr), messageReg);
            Must(enc.Ldp(A64::Xn(1), A64::Xn(2), A64::Xn(kAddr), 0), "an assertion message");
            Must(enc.LoadImm64(A64::Xn(0), kStandardError), "the standard error descriptor");
            EmitWriteSyscall(*syscall);

            EmitWriteStatic(*syscall, layout.location);
        }

        Must(enc.Brk(1), "an assertion trap");
        if (isAssertion) {
            terminatorEmitter.PatchBranchOver(heldBranch);
        }
    }

    void GenInstr(AArch64FunctionEmitter &functionEmitter, AArch64CallEmitter &callEmitter,
                  AArch64TerminatorEmitter &terminatorEmitter, const LirInstr &instr) {
        if (functionEmitter.EmitArithmetic(instr) || functionEmitter.EmitMemory(instr) || callEmitter.Emit(instr)) {
            return;
        }
        switch (instr.op) {
        case LirOpcode::Assert:
        case LirOpcode::Panic:
            GenAssert(terminatorEmitter, instr);
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
        AArch64FunctionEmitter functionEmitter(enc, framePlan, runtimeHelpers, layouts, interfaceNames, currentFunc,
                                               targetOs, *this);
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
                                                       func.isPublic ? RcuSymVis::Global : RcuSymVis::Local);
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

    void AppendConstElement(const std::string &literal, const TypeRef &type) {
        AppendScalarConstant(RodataData(), literal, type);
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
            layouts[s.name] = ComputeStructLayout(s, layouts, interfaceNames);
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
