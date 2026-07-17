// RCU code generation: lowers a LirModule to an in-memory RcuFile.

#include "CodeGen/X86_64/RcuEmitter.h"

#include "CodeGen/FloatLiteral.h"
#include "CodeGen/Layout.h"
#include "CodeGen/PhiMoveResolver.h"
#include "CodeGen/X86_64/Assembler.h"
#include "CodeGen/X86_64/Encoder.h"
#include "Driver/Version.h"
#include "Object/Rcu/RcuStringTable.h"

#include <charconv>
#include <chrono>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Rux {
using namespace Layout;

namespace {
std::string_view NumericLiteralSuffix(std::string_view text) {
    static constexpr std::string_view suffixes[] = {
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "i", "u",
    };
    for (const auto suffix : suffixes) {
        if (text.size() > suffix.size() && text.substr(text.size() - suffix.size()) == suffix) {
            return suffix;
        }
    }
    return {};
}

std::optional<std::uint64_t> ParseIntegerLiteralBits(std::string_view text) {
    const std::string_view suffix = NumericLiteralSuffix(text);
    if (!suffix.empty()) {
        text.remove_suffix(suffix.size());
    }

    bool negative = false;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }

    std::string cleaned;
    cleaned.reserve(text.size());
    for (const char c : text) {
        if (c != '_') {
            cleaned.push_back(c);
        }
    }

    int base = 10;
    std::string_view digits(cleaned);
    if (digits.size() > 2 && digits[0] == '0') {
        switch (digits[1]) {
        case 'x':
        case 'X':
            base = 16;
            digits.remove_prefix(2);
            break;
        case 'b':
        case 'B':
            base = 2;
            digits.remove_prefix(2);
            break;
        case 'o':
        case 'O':
            base = 8;
            digits.remove_prefix(2);
            break;
        default:
            break;
        }
    }
    if (digits.empty()) {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    const auto *first = digits.data();
    const auto *last = first + digits.size();
    const auto [ptr, ec] = std::from_chars(first, last, value, base);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    if (!negative) {
        return value;
    }

    constexpr std::uint64_t maxNegativeMagnitude =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
    if (value > maxNegativeMagnitude) {
        return std::nullopt;
    }
    return std::uint64_t{0} - value;
}

CallingConvention EffectiveConv(const CallingConvention c) {
    const CallingConvention resolved = ResolveCConvention(c);
    if (resolved != CallingConvention::Default) {
        return resolved;
    }
    return PlatformDefaultConvention();
}

// RCU Code Generator: LirModule → RcuFile
struct JumpPatch {
    uint32_t patchOff;
    uint32_t targetBlock;
};

class RcuCodeGen {
public:
    explicit RcuCodeGen(const LirModule &module, const std::vector<LirStructDecl> &inputStructDecls,
                        const std::vector<std::string> &inputPackageInterfaceNames, std::string packageName,
                        const Target::OS inputTargetOs, std::vector<Diagnostic> &inputDiagnostics)
        : mod(module)
        , structDecls(inputStructDecls)
        , packageInterfaceNames(inputPackageInterfaceNames)
        , pkgName(std::move(packageName))
        , targetOs(inputTargetOs)
        , diagnostics(inputDiagnostics)
        , enc(textData) {
    }

    RcuFile Generate();

private:
    const LirModule &mod;
    const std::vector<LirStructDecl> &structDecls;
    const std::vector<std::string> &packageInterfaceNames;
    std::string pkgName;
    Target::OS targetOs;
    std::vector<Diagnostic> &diagnostics;

    // Section data buffers
    std::vector<uint8_t> textData;
    std::vector<uint8_t> rodataData;
    std::vector<uint8_t> dataData;

    // Per-section relocations
    std::vector<RcuReloc> textRelocs;
    std::vector<RcuReloc> rodataRelocs;

    // Symbol table and string table
    std::vector<RcuSymbol> symbols;
    RcuStringTable strings;

    // Encoder writing into textData
    X64Enc enc;

    // Interned rodata constants: key → symbol index
    std::unordered_map<std::string, uint32_t> strSyms;
    std::unordered_map<std::string, uint32_t> f32Syms;
    std::unordered_map<std::string, uint32_t> f64Syms;
    int constIdx = 0;
    uint32_t f32SignMaskSym = ~0u;
    uint32_t f64SignMaskSym = ~0u;

    // Declared extern symbols (by name → symbol index)
    std::unordered_map<std::string, uint32_t> externSyms;
    std::unordered_map<std::string, uint32_t> funcSyms;
    std::unordered_map<std::string, uint32_t> dataSyms;

    // Symbol index of the synthesized integer-pow helper (~0u until
    // used).
    uint32_t ipowSym = ~0u;

    // Symbol indices of the synthesized floating-point pow helpers (~0u
    // until used). The f32 helper is a thin wrapper around the f64 one.
    uint32_t fpowSym = ~0u;
    uint32_t fpowf32Sym = ~0u;

    // Struct field layouts
    LayoutMap layouts;
    std::unordered_set<std::string> interfaceNames;

    std::unordered_map<LirReg, int32_t> slotMap;
    std::unordered_map<LirReg, int32_t> allocaData;
    std::unordered_map<LirReg, TypeRef> regTypes;
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, std::vector<PhiMove>>> phiMoves;
    int32_t nextOff = 0;
    int32_t frameSize = 0;
    int32_t hiddenReturnOff = 0;
    std::unordered_map<LirReg, int> physRegMap;
    std::vector<int> usedPhysRegs;

    std::vector<uint32_t> blockOffsets;
    std::vector<JumpPatch> jumpPatches;

    // Helpers
    [[nodiscard]] int32_t Disp(const LirReg r) const {
        return -static_cast<int32_t>(slotMap.at(r));
    }

    [[nodiscard]] int SizeOfRuntime(const TypeRef &t) const {
        if (t.kind == TypeRef::Kind::Range) {
            const TypeRef &elemType = t.inner.empty() ? TypeRef::MakeInt64() : t.inner[0];
            int elemSize = SizeOf(elemType);
            return AlignUp(2 * elemSize + 1, elemSize > 0 ? elemSize : 1);
        }
        if (t.kind == TypeRef::Kind::Named) {
            const std::string base = BaseTypeName(t.name);
            if (interfaceNames.count(base)) {
                return 16;
            }
            if (base == "Slice") {
                return 16;
            }
            if (base == "String" || base == "StringArray" || base == "SystemTime") {
                return 16;
            }
            if (base == "StringBuilder") {
                return 24;
            }
            auto it = layouts.find(base);
            if (it != layouts.end()) {
                return it->second.totalSize;
            }
        }
        return SizeOf(t);
    }

    [[nodiscard]] int StackValueSize(const TypeRef &t) const {
        return SizeOfRuntime(t);
    }

    [[nodiscard]] bool IsAggregate(const TypeRef &t) const {
        switch (t.kind) {
        case TypeRef::Kind::Tuple:
        case TypeRef::Kind::Slice:
        case TypeRef::Kind::Range:
            return true;
        case TypeRef::Kind::Named: {
            const std::string base = BaseTypeName(t.name);
            return base == "Slice" || interfaceNames.count(base) > 0 || layouts.contains(base);
        }
        default:
            return false;
        }
    }

    [[nodiscard]] bool IsWin64ByRefAggregate(const TypeRef &t) const {
        if (!IsAggregate(t)) {
            return false;
        }
        const int size = SizeOfRuntime(t);
        return size > 0 && size != 1 && size != 2 && size != 4 && size != 8;
    }

    [[nodiscard]] bool IsSysVMemoryAggregate(const TypeRef &t) const {
        return IsAggregate(t) && SizeOfRuntime(t) > 16;
    }

    [[nodiscard]] bool IsWin64AddressParam(const TypeRef &t) const {
        if (t.kind == TypeRef::Kind::Slice) {
            return true;
        }
        if (t.kind != TypeRef::Kind::Named) {
            return false;
        }
        const std::string base = BaseTypeName(t.name);
        return base == "Slice" || interfaceNames.count(base) > 0;
    }

    [[nodiscard]] bool IsPointerToWin64ByRefAggregate(const TypeRef &t) const {
        return t.kind == TypeRef::Kind::Pointer && !t.inner.empty() && IsWin64ByRefAggregate(t.inner[0]);
    }

    [[nodiscard]] bool IsRegPointerTo(const LirReg reg, const TypeRef &pointee) const {
        const auto it = regTypes.find(reg);
        return it != regTypes.end() && it->second.kind == TypeRef::Kind::Pointer && !it->second.inner.empty() &&
               it->second.inner[0] == pointee;
    }

    static int Win64CallFrameSize(const std::size_t argCount) {
        const std::size_t stackArgs = argCount > 4 ? argCount - 4 : 0;
        return AlignUp(static_cast<int>(32 + stackArgs * 8), 16);
    }

    [[nodiscard]] std::size_t SysVStackArgCount(const std::vector<LirReg> &args, const int startIntIdx = 0) const {
        int intIdx = startIntIdx;
        int fltIdx = 0;
        std::size_t stackArgs = 0;
        for (const LirReg arg : args) {
            const TypeRef type = regTypes.contains(arg) ? regTypes.at(arg) : TypeRef::MakeInt64();
            if (IsFloat(type)) {
                if (fltIdx < 8) {
                    ++fltIdx;
                }
                else {
                    ++stackArgs;
                }
            }
            else if (IsSysVMemoryAggregate(type)) {
                stackArgs += static_cast<std::size_t>(AlignUp(SizeOfRuntime(type), 8) / 8);
            }
            else if (IsAggregate(type) && SizeOfRuntime(type) == 16) {
                if (intIdx <= 4) {
                    intIdx += 2;
                }
                else {
                    stackArgs += 2;
                }
            }
            else if (intIdx < 6) {
                ++intIdx;
            }
            else {
                ++stackArgs;
            }
        }
        return stackArgs;
    }

    [[nodiscard]] int SysVCallFrameSize(const std::vector<LirReg> &args, const int startIntIdx = 0) const {
        return AlignUp(static_cast<int>(SysVStackArgCount(args, startIntIdx) * 8), 16);
    }

    void StoreSysVStackArgs(const std::vector<LirReg> &args, const int startIntIdx = 0) const {
        int intIdx = startIntIdx;
        int fltIdx = 0;
        int stackIdx = 0;
        for (const LirReg arg : args) {
            const TypeRef type = regTypes.contains(arg) ? regTypes.at(arg) : TypeRef::MakeInt64();
            if (IsSysVMemoryAggregate(type)) {
                const int size = AlignUp(SizeOfRuntime(type), 8);
                for (int offset = 0; offset < size; offset += 8) {
                    enc.MovRaxLoad(Disp(arg) + offset);
                    enc.MovRaxStoreRsp(stackIdx++ * 8);
                }
                continue;
            }
            const bool twoWordAggregate = IsAggregate(type) && SizeOfRuntime(type) == 16;
            if (twoWordAggregate) {
                if (intIdx <= 4) {
                    intIdx += 2;
                }
                else {
                    enc.MovRaxLoad(Disp(arg));
                    enc.MovRaxStoreRsp(stackIdx++ * 8);
                    enc.MovRaxLoad(Disp(arg) + 8);
                    enc.MovRaxStoreRsp(stackIdx++ * 8);
                }
                continue;
            }

            bool onStack = false;
            if (IsFloat(type)) {
                if (fltIdx < 8) {
                    ++fltIdx;
                }
                else {
                    onStack = true;
                }
            }
            else if (intIdx < 6) {
                ++intIdx;
            }
            else {
                onStack = true;
            }
            if (!onStack) {
                continue;
            }

            LoadA(arg, type);
            const int32_t offset = stackIdx++ * 8;
            if (IsFloat(type)) {
                if (SizeOf(type) == 4) {
                    enc.MovssXmm0StoreRsp(offset);
                }
                else {
                    enc.MovsdXmm0StoreRsp(offset);
                }
            }
            else {
                enc.MovRaxStoreRsp(offset);
            }
        }
    }

    uint32_t AddSymbol(RcuSymbol s) {
        auto idx = static_cast<uint32_t>(symbols.size());
        symbols.push_back(std::move(s));
        return idx;
    }

    uint32_t GetOrAddExtern(const std::string &name, uint8_t kind, const std::string &dll = {}) {
        auto it = externSyms.find(name);
        if (it != externSyms.end()) {
            return it->second;
        }
        RcuSymbol s;
        s.name = name;
        s.typeName = dll;
        s.kind = kind;
        s.visibility = RcuSymVis::Global;
        s.sectionIdx = RCU_SEC_EXTERNAL;
        uint32_t idx = AddSymbol(s);
        externSyms[name] = idx;
        return idx;
    }

    // Lazily declares the synthesized integer exponentiation helper and
    // returns its symbol index. The body is emitted once per object by
    // EmitIntPowHelper(), after all user functions, so forward calls to
    // it resolve like any other local text symbol.
    uint32_t EnsureIntPowHelper() {
        if (ipowSym == ~0u) {
            RcuSymbol s;
            s.name = "__rux_ipow";
            s.sectionIdx = RCU_TEXT_IDX;
            s.value = 0; // patched to the real offset in EmitIntPowHelper
            s.kind = RcuSymKind::Func;
            s.visibility = RcuSymVis::Local;
            ipowSym = AddSymbol(s);
        }
        return ipowSym;
    }

    // Emits the integer exponentiation helper into .text:
    //   rax = rdi ** rsi   (signed exponent)
    // Exponentiation by squaring; a negative exponent yields 0 and a
    // zero exponent yields 1. Works for every integer width because the
    // low bits of a two's-complement product are width-independent, so
    // no libm/CRT dependency is needed on any backend.
    void EmitIntPowHelper() {
        if (ipowSym == ~0u) {
            return;
        }
        symbols[ipowSym].value = enc.Size();
        if (EffectiveConv(CallingConvention::Default) == CallingConvention::SysV) {
            // Keep the compact helper body below in its historical rcx/rdx
            // form after accepting native SysV rdi/rsi arguments.
            enc.Byte(0x48);
            enc.Byte(0x89);
            enc.Byte(0xF9); // mov rcx, rdi
            enc.Byte(0x48);
            enc.Byte(0x89);
            enc.Byte(0xF2); // mov rdx, rsi
        }
        // clang-format off
                static constexpr std::uint8_t kThunk[] = {
                    0x48, 0x85, 0xD2,                         // test rdx, rdx    ; exponent
                    0x78, 0x20,                               // js   .negative   ; exp < 0 -> 0
                    0xB8, 0x01, 0x00, 0x00, 0x00,             // mov  eax, 1      ; result = 1
                    // .loop:
                    0x48, 0x85, 0xD2,                         // test rdx, rdx
                    0x74, 0x18,                               // jz   .done       ; exp == 0
                    0x48, 0xF7, 0xC2, 0x01, 0x00, 0x00, 0x00, // test rdx, 1
                    0x74, 0x04,                               // jz   .square
                    0x48, 0x0F, 0xAF, 0xC1,                   // imul rax, rcx    ; result *= base
                    // .square:
                    0x48, 0x0F, 0xAF, 0xC9,                   // imul rcx, rcx    ; base *= base
                    0x48, 0xD1, 0xFA,                         // sar  rdx, 1      ; exp >>= 1
                    0xEB, 0xE5,                               // jmp  .loop
                    // .negative:
                    0x31, 0xC0,                               // xor  eax, eax    ; result = 0
                    // .done:
                    0xC3,                                     // ret
                };
        // clang-format on
        for (const std::uint8_t b : kThunk) {
            enc.Byte(b);
        }
    }

    // Lazily declares the synthesized double-precision pow helper and
    // returns its symbol index. The body is emitted once per object by
    // EmitFloatPowHelper(), alongside the other runtime helpers.
    uint32_t EnsureFloatPowHelper() {
        if (fpowSym == ~0u) {
            RcuSymbol s;
            s.name = "__rux_powf64";
            s.sectionIdx = RCU_TEXT_IDX;
            s.value = 0; // patched to the real offset in EmitFloatPowHelper
            s.kind = RcuSymKind::Func;
            s.visibility = RcuSymVis::Local;
            fpowSym = AddSymbol(s);
        }
        return fpowSym;
    }

    // Lazily declares the synthesized single-precision pow helper. It is a
    // thin wrapper around the double helper, so ensure that one exists too.
    uint32_t EnsureFloatPowF32Helper() {
        EnsureFloatPowHelper();
        if (fpowf32Sym == ~0u) {
            RcuSymbol s;
            s.name = "__rux_powf32";
            s.sectionIdx = RCU_TEXT_IDX;
            s.value = 0; // patched to the real offset in EmitFloatPowF32Helper
            s.kind = RcuSymKind::Func;
            s.visibility = RcuSymVis::Local;
            fpowf32Sym = AddSymbol(s);
        }
        return fpowf32Sym;
    }

    // Emits the double-precision exponentiation helper into .text:
    //   xmm0 = xmm0 ** xmm1   (base ** exponent, both f64)
    //
    // Computed as |base|**exp = 2**(exp * log2(|base|)) on the x87 FPU
    // (fyl2x + f2xm1 + fscale), so no libm/CRT dependency is needed on any
    // backend. The sign of a negative base is restored for integer
    // exponents (odd -> negated result, even -> positive); a negative base
    // with a non-integer exponent yields NaN, matching C pow(). Special
    // cases handled up front: exp == 0 -> 1, base == 0 -> 0 (or +inf for a
    // negative exponent). Only volatile registers (rax/rdx, xmm0-xmm3) are
    // touched, so the helper is ABI-safe under both SysV and Win64.
    void EmitFloatPowHelper() {
        if (fpowSym == ~0u) {
            return;
        }
        symbols[fpowSym].value = enc.Size();

        auto b = [&](std::initializer_list<uint8_t> bytes) {
            for (uint8_t x : bytes) {
                enc.Byte(x);
            }
        };
        // Emits a two-byte Jcc rel32 and returns the offset of its
        // displacement field for later patching.
        auto jcc = [&](uint8_t cc) -> uint32_t {
            enc.Byte(0x0F);
            enc.Byte(cc);
            uint32_t off = enc.Size();
            enc.Dword(0);
            return off;
        };
        // Patches a previously emitted Jcc/Jmp to target the current offset.
        auto patch = [&](uint32_t patchOff) {
            int32_t rel = static_cast<int32_t>(enc.Size()) - static_cast<int32_t>(patchOff + 4);
            enc.Patch32(patchOff, rel);
        };

        // clang-format off
        // Prologue: reserve 16 bytes of scratch; [rsp]=base, [rsp+8]=exp.
        b({0x48, 0x83, 0xEC, 0x10});             // sub  rsp, 16
        b({0xF2, 0x0F, 0x11, 0x04, 0x24});       // movsd [rsp], xmm0      ; base
        b({0xF2, 0x0F, 0x11, 0x4C, 0x24, 0x08}); // movsd [rsp+8], xmm1    ; exp

        // exp == 0  ->  return 1.0  (covers base==0/inf/NaN per C pow).
        b({0x48, 0x8B, 0x44, 0x24, 0x08});       // mov  rax, [rsp+8]
        b({0x48, 0x01, 0xC0});                   // add  rax, rax          ; drop sign bit
        uint32_t jExpNonZero = jcc(0x85);        // jnz  .not_exp0
        b({0x48, 0xB8}); enc.Qword(0x3FF0000000000000ull); // mov rax, 1.0 bits
        b({0x48, 0x89, 0x04, 0x24});             // mov  [rsp], rax
        b({0xF2, 0x0F, 0x10, 0x04, 0x24});       // movsd xmm0, [rsp]
        b({0x48, 0x83, 0xC4, 0x10});             // add  rsp, 16
        b({0xC3});                               // ret
        patch(jExpNonZero);                      // .not_exp0:

        // |base| == 0  ->  0.0 (exp>0) or +inf (exp<0).
        b({0x48, 0x8B, 0x04, 0x24});             // mov  rax, [rsp]
        b({0x48, 0x01, 0xC0});                   // add  rax, rax          ; drop sign bit
        uint32_t jBaseNonZero = jcc(0x85);       // jnz  .base_nonzero
        b({0x48, 0x8B, 0x44, 0x24, 0x08});       // mov  rax, [rsp+8]
        b({0x48, 0x85, 0xC0});                   // test rax, rax          ; exp sign
        uint32_t jBase0NegExp = jcc(0x88);       // js   .base0_neg_exp
        b({0x31, 0xC0});                         // xor  eax, eax          ; +0.0
        b({0x48, 0x89, 0x04, 0x24});             // mov  [rsp], rax
        b({0xF2, 0x0F, 0x10, 0x04, 0x24});       // movsd xmm0, [rsp]
        b({0x48, 0x83, 0xC4, 0x10});             // add  rsp, 16
        b({0xC3});                               // ret
        patch(jBase0NegExp);                     // .base0_neg_exp:
        b({0x48, 0xB8}); enc.Qword(0x7FF0000000000000ull); // mov rax, +inf bits
        b({0x48, 0x89, 0x04, 0x24});             // mov  [rsp], rax
        b({0xF2, 0x0F, 0x10, 0x04, 0x24});       // movsd xmm0, [rsp]
        b({0x48, 0x83, 0xC4, 0x10});             // add  rsp, 16
        b({0xC3});                               // ret
        patch(jBaseNonZero);                     // .base_nonzero:

        // Sign decision -> edx: 0 keep, 1 negate, 2 NaN.
        b({0x31, 0xD2});                         // xor  edx, edx
        b({0x48, 0x8B, 0x04, 0x24});             // mov  rax, [rsp]        ; base bits
        b({0x48, 0x85, 0xC0});                   // test rax, rax
        uint32_t jBasePos = jcc(0x89);           // jns  .magnitude        ; base > 0
        // base < 0: the exponent must be an integer, else the result is NaN.
        b({0xF2, 0x0F, 0x10, 0x54, 0x24, 0x08}); // movsd xmm2, [rsp+8]
        b({0xF2, 0x48, 0x0F, 0x2C, 0xC2});       // cvttsd2si rax, xmm2
        b({0xF2, 0x48, 0x0F, 0x2A, 0xD8});       // cvtsi2sd  xmm3, rax
        b({0x66, 0x0F, 0x2E, 0xD3});             // ucomisd xmm2, xmm3
        uint32_t jNonInt = jcc(0x85);            // jne  .nonint
        b({0x83, 0xE0, 0x01});                   // and  eax, 1            ; parity
        b({0x89, 0xC2});                         // mov  edx, eax          ; 1 if odd
        enc.Byte(0xE9); uint32_t jToMag = enc.Size(); enc.Dword(0); // jmp .magnitude
        patch(jNonInt);                          // .nonint:
        b({0xBA, 0x02, 0x00, 0x00, 0x00});       // mov  edx, 2
        patch(jBasePos);                         // .magnitude:
        patch(jToMag);

        // magnitude = |base| ** exp  =  2 ** (exp * log2(|base|)).
        b({0xDD, 0x44, 0x24, 0x08});             // fld  qword [rsp+8]     ; exp
        b({0xDD, 0x04, 0x24});                   // fld  qword [rsp]       ; base
        b({0xD9, 0xE1});                         // fabs                   ; |base|
        b({0xD9, 0xF1});                         // fyl2x                  ; w = exp*log2(|base|)
        b({0xD9, 0xC0});                         // fld  st0
        b({0xD9, 0xFC});                         // frndint                ; i = round(w)
        b({0xDC, 0xE9});                         // fsub st1, st0          ; frac = w - i in [-1,1]
        b({0xD9, 0xC9});                         // fxch                   ; st0=frac st1=i
        b({0xD9, 0xF0});                         // f2xm1                  ; 2^frac - 1
        b({0xD9, 0xE8});                         // fld1
        b({0xDE, 0xC1});                         // faddp st1, st0         ; 2^frac
        b({0xD9, 0xFD});                         // fscale                 ; 2^frac * 2^i
        b({0xDD, 0xD9});                         // fstp st1               ; drop i -> st0=magnitude

        b({0x85, 0xD2});                         // test edx, edx
        uint32_t jStore = jcc(0x84);             // jz   .store
        b({0x83, 0xFA, 0x02});                   // cmp  edx, 2
        uint32_t jNan = jcc(0x84);               // jz   .nan
        b({0xD9, 0xE0});                         // fchs                   ; negate (odd exponent)
        patch(jStore);                           // .store:
        b({0xDD, 0x1C, 0x24});                   // fstp qword [rsp]
        b({0xF2, 0x0F, 0x10, 0x04, 0x24});       // movsd xmm0, [rsp]
        b({0x48, 0x83, 0xC4, 0x10});             // add  rsp, 16
        b({0xC3});                               // ret
        patch(jNan);                             // .nan:
        b({0xDD, 0xD8});                         // fstp st0               ; drop magnitude
        b({0x48, 0xB8}); enc.Qword(0x7FF8000000000000ull); // mov rax, qNaN bits
        b({0x48, 0x89, 0x04, 0x24});             // mov  [rsp], rax
        b({0xF2, 0x0F, 0x10, 0x04, 0x24});       // movsd xmm0, [rsp]
        b({0x48, 0x83, 0xC4, 0x10});             // add  rsp, 16
        b({0xC3});                               // ret
                   // clang-format on
    }

    // Emits the single-precision pow helper into .text:
    //   xmm0 = xmm0 ** xmm1   (base ** exponent, both f32)
    //
    // Widens the arguments to f64, defers to __rux_powf64, then narrows the
    // result. Computing in double precision keeps the f32 result correctly
    // rounded and avoids duplicating the x87 sequence.
    void EmitFloatPowF32Helper() {
        if (fpowf32Sym == ~0u) {
            return;
        }
        symbols[fpowf32Sym].value = enc.Size();

        auto b = [&](std::initializer_list<uint8_t> bytes) {
            for (uint8_t x : bytes) {
                enc.Byte(x);
            }
        };
        // clang-format off
        b({0xF3, 0x0F, 0x5A, 0xC0}); // cvtss2sd xmm0, xmm0
        b({0xF3, 0x0F, 0x5A, 0xC9}); // cvtss2sd xmm1, xmm1
        b({0x48, 0x83, 0xEC, 0x08}); // sub  rsp, 8            ; keep 16-byte align at call
        uint32_t ro;
        enc.Call(ro);
        AddTextReloc(ro, fpowSym);   // call __rux_powf64
        b({0x48, 0x83, 0xC4, 0x08}); // add  rsp, 8
        b({0xF2, 0x0F, 0x5A, 0xC0}); // cvtsd2ss xmm0, xmm0
        b({0xC3});                   // ret
                   // clang-format on
    }

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
            funcSyms[func.name] = AddSymbol(sym);
        }
    }

    // Align rodataData_ to `align` bytes (zero-fill), return current
    // offset.
    uint32_t AlignRodata(int align) {
        while (rodataData.size() % align) {
            rodataData.push_back(0);
        }
        return static_cast<uint32_t>(rodataData.size());
    }

    uint32_t InternStr(const std::string &val) {
        auto it = strSyms.find(val);
        if (it != strSyms.end()) {
            return it->second;
        }
        auto off = static_cast<uint32_t>(rodataData.size());
        for (unsigned char c : val) {
            rodataData.push_back(c);
        }
        rodataData.push_back(0);
        std::string lbl = std::format("__str{}", constIdx++);
        RcuSymbol s;
        s.name = lbl;
        s.sectionIdx = RCU_RODATA_IDX;
        s.value = off;
        s.size = static_cast<uint32_t>(val.size() + 1);
        s.kind = RcuSymKind::Const;
        s.visibility = RcuSymVis::Local;
        uint32_t idx = AddSymbol(s);
        strSyms[val] = idx;
        return idx;
    }

    uint32_t InternF32(const std::string &val) {
        auto it = f32Syms.find(val);
        if (it != f32Syms.end()) {
            return it->second;
        }
        uint32_t off = AlignRodata(4);
        const float fv = ParseFloatLiteral<float>(val);
        uint32_t bits;
        std::memcpy(&bits, &fv, 4);
        for (int i = 0; i < 4; ++i) {
            rodataData.push_back(bits & 0xFF);
            bits >>= 8;
        }
        std::string lbl = std::format("__f32_{}", constIdx++);
        RcuSymbol s;
        s.name = lbl;
        s.sectionIdx = RCU_RODATA_IDX;
        s.value = off;
        s.size = 4;
        s.kind = RcuSymKind::Const;
        s.visibility = RcuSymVis::Local;
        uint32_t idx = AddSymbol(s);
        f32Syms[val] = idx;
        return idx;
    }

    uint32_t InternF64(const std::string &val) {
        auto it = f64Syms.find(val);
        if (it != f64Syms.end()) {
            return it->second;
        }
        uint32_t off = AlignRodata(8);
        const double dv = ParseFloatLiteral<double>(val);
        uint64_t bits;
        std::memcpy(&bits, &dv, 8);
        for (int i = 0; i < 8; ++i) {
            rodataData.push_back(bits & 0xFF);
            bits >>= 8;
        }
        std::string lbl = std::format("__f64_{}", constIdx++);
        RcuSymbol s;
        s.name = lbl;
        s.sectionIdx = RCU_RODATA_IDX;
        s.value = off;
        s.size = 8;
        s.kind = RcuSymKind::Const;
        s.visibility = RcuSymVis::Local;
        uint32_t idx = AddSymbol(s);
        f64Syms[val] = idx;
        return idx;
    }

    uint32_t InternF32SignMask() {
        if (f32SignMaskSym != ~0u) {
            return f32SignMaskSym;
        }
        uint32_t off = AlignRodata(4);
        // 0x80000000 — sign bit of f32
        rodataData.push_back(0x00);
        rodataData.push_back(0x00);
        rodataData.push_back(0x00);
        rodataData.push_back(0x80);
        RcuSymbol s;
        s.name = "__f32_sign_mask";
        s.sectionIdx = RCU_RODATA_IDX;
        s.value = off;
        s.size = 4;
        s.kind = RcuSymKind::Const;
        s.visibility = RcuSymVis::Local;
        f32SignMaskSym = AddSymbol(s);
        return f32SignMaskSym;
    }

    uint32_t InternF64SignMask() {
        if (f64SignMaskSym != ~0u) {
            return f64SignMaskSym;
        }
        uint32_t off = AlignRodata(8);
        // 0x8000000000000000 — sign bit of f64
        for (int i = 0; i < 7; ++i) {
            rodataData.push_back(0x00);
        }
        rodataData.push_back(0x80);
        RcuSymbol s;
        s.name = "__f64_sign_mask";
        s.sectionIdx = RCU_RODATA_IDX;
        s.value = off;
        s.size = 8;
        s.kind = RcuSymKind::Const;
        s.visibility = RcuSymVis::Local;
        f64SignMaskSym = AddSymbol(s);
        return f64SignMaskSym;
    }

    void AddTextReloc(uint32_t sectionOff, uint32_t symIdx, int32_t addend = 0) {
        textRelocs.push_back({sectionOff, symIdx, RcuRelType::Rel32, addend});
    }

    void AddRodataReloc(uint32_t sectionOff, uint32_t symIdx, uint16_t type, int32_t addend = 0) {
        rodataRelocs.push_back({sectionOff, symIdx, type, addend});
    }

    void PatchJumps() {
        for (const auto &p : jumpPatches) {
            auto target = static_cast<int32_t>(blockOffsets[p.targetBlock]);
            int32_t rel32 = target - static_cast<int32_t>(p.patchOff + 4);
            enc.Patch32(p.patchOff, rel32);
        }
        jumpPatches.clear();
    }

    // Load A (rax / xmm0) and B (r10 / xmm1)
    void LoadA(const LirReg reg, const TypeRef &t) const {
        auto it = physRegMap.find(reg);
        if (it != physRegMap.end()) {
            enc.MovRaxPhysReg(it->second);
            int sz = SizeOfRuntime(t);
            if (sz > 0 && sz < 8) {
                if (t.IsSigned()) {
                    if (sz == 4)
                        enc.MovsxdRaxEax();
                    else if (sz == 2)
                        enc.MovsxRaxAx();
                    else
                        enc.MovsxRaxAl();
                }
                else {
                    if (sz == 4)
                        enc.MovEaxEax();
                    else if (sz == 2)
                        enc.MovzxRaxAx();
                    else
                        enc.MovzxRaxAl();
                }
            }
            return;
        }
        const int sz = SizeOfRuntime(t);
        const int runtimeSz = SizeOfRuntime(t);
        const int32_t d = Disp(reg);
        if (runtimeSz == 16) {
            enc.MovRaxLoad(d);
            enc.MovR10Load(d + 8);
            enc.Byte(0x4C);
            enc.Byte(0x89);
            enc.Byte(0xD2); // mov rdx, r10
        }
        else if (IsFloat(t)) {
            if (t.kind == TypeRef::Kind::Float32) {
                enc.MovssXmm0Load(d);
            }
            else {
                enc.MovsdXmm0Load(d);
            }
        }
        else if (sz == 8 || sz == 0) {
            enc.MovRaxLoad(d);
        }
        else if (t.IsSigned()) {
            if (sz == 4) {
                enc.MovsxdRaxDword(d);
            }
            else if (sz == 2) {
                enc.MovsxRaxWord(d);
            }
            else {
                enc.MovsxRaxByte(d);
            }
        }
        else {
            if (sz == 4) {
                enc.MovEaxLoad(d);
            }
            else if (sz == 2) {
                enc.MovzxRaxWord(d);
            }
            else {
                enc.MovzxRaxByte(d);
            }
        }
    }

    void LoadB(LirReg reg, const TypeRef &t) const {
        auto it = physRegMap.find(reg);
        if (it != physRegMap.end()) {
            enc.MovR10PhysReg(it->second);
            int sz = SizeOfRuntime(t);
            if (sz > 0 && sz < 8) {
                if (t.IsSigned()) {
                    if (sz == 4)
                        enc.MovsxdR10r10d();
                    else if (sz == 2)
                        enc.MovsxR10r10w();
                    else
                        enc.MovsxR10r10b();
                }
                else {
                    if (sz == 4)
                        enc.MovR10dR10d();
                    else if (sz == 2)
                        enc.MovzxR10r10w();
                    else
                        enc.MovzxR10r10b();
                }
            }
            return;
        }
        int sz = SizeOfRuntime(t);
        int32_t d = Disp(reg);
        if (IsFloat(t)) {
            if (t.kind == TypeRef::Kind::Float32) {
                enc.MovssXmm1Load(d);
            }
            else {
                enc.MovsdXmm1Load(d);
            }
        }
        else if (sz == 8 || sz == 0) {
            enc.MovR10Load(d);
        }
        else if (t.IsSigned()) {
            if (sz == 4) {
                enc.MovsxdR10Dword(d);
            }
            else if (sz == 2) {
                enc.MovsxR10Word(d);
            }
            else {
                enc.MovsxR10Byte(d);
            }
        }
        else {
            if (sz == 4) {
                enc.MovR10dLoad(d);
            }
            else if (sz == 2) {
                enc.MovzxR10Word(d);
            }
            else {
                enc.MovzxR10Byte(d);
            }
        }
    }

    void StoreStack(LirReg dst, const TypeRef &t) const {
        int sz = SizeOfRuntime(t);
        int runtimeSz = SizeOfRuntime(t);
        int32_t d = Disp(dst);
        if (runtimeSz == 16) {
            enc.MovRaxStore(d);
            enc.Byte(0x48);
            enc.Byte(0x89);
            enc.Byte(0x95);
            enc.Dword(static_cast<uint32_t>(d + 8)); // mov [rbp+disp+8], rdx
        }
        else if (IsFloat(t)) {
            if (t.kind == TypeRef::Kind::Float32) {
                enc.MovssXmm0Store(d);
            }
            else {
                enc.MovsdXmm0Store(d);
            }
        }
        else {
            int ss = (sz > 0) ? sz : 8;
            if (ss == 8) {
                enc.MovRaxStore(d);
            }
            else if (ss == 4) {
                enc.MovEaxStore(d);
            }
            else if (ss == 2) {
                enc.MovAxStore(d);
            }
            else {
                enc.MovAlStore(d);
            }
        }
    }

    void StoreA(LirReg dst, const TypeRef &t) const {
        auto it = physRegMap.find(dst);
        if (it != physRegMap.end()) {
            enc.MovPhysRegRax(it->second);
            return;
        }
        StoreStack(dst, t);
    }

    void LoadReturnValue(const LirReg reg, const TypeRef &t) const {
        const int size = SizeOfRuntime(t);
        if (IsRegPointerTo(reg, t)) {
            if (size == 16) {
                auto it = physRegMap.find(reg);
                if (it != physRegMap.end()) {
                    enc.MovR10PhysReg(it->second);
                }
                else {
                    enc.MovR10Load(Disp(reg));
                }
                enc.Byte(0x49);
                enc.Byte(0x8B);
                enc.Byte(0x02); // mov rax, [r10]
                enc.Byte(0x49);
                enc.Byte(0x8B);
                enc.Byte(0x52);
                enc.Byte(0x08); // mov rdx, [r10 + 8]
                return;
            }

            if (size == 1 || size == 2 || size == 4 || size == 8) {
                auto it = physRegMap.find(reg);
                if (it != physRegMap.end()) {
                    enc.MovR10PhysReg(it->second);
                }
                else {
                    enc.MovR10Load(Disp(reg));
                }
                LoadChunkFromR10(0, size);
                return;
            }
        }

        if (size == 16) {
            enc.MovRaxLoad(Disp(reg));
            enc.MovR10Load(Disp(reg) + 8);
            enc.Byte(0x4C);
            enc.Byte(0x89);
            enc.Byte(0xD2); // mov rdx, r10
            return;
        }
        LoadA(reg, t);
    }

    void StoreReturnValue(const LirReg dst, const TypeRef &t) const {
        if (SizeOfRuntime(t) == 16) {
            enc.MovRaxStore(Disp(dst));
            enc.Byte(0x48);
            enc.Byte(0x89);
            enc.Byte(0x95);
            enc.Dword(static_cast<uint32_t>(Disp(dst) + 8)); // mov [rbp+disp], rdx
            return;
        }
        StoreA(dst, t);
    }

    void LoadChunkFromR10(const int32_t offset, const int size) const {
        if (size == 8) {
            enc.Byte(0x49);
            enc.Byte(0x8B);
            enc.Byte(0x82); // mov rax, [r10 + disp32]
        }
        else if (size == 4) {
            enc.Byte(0x41);
            enc.Byte(0x8B);
            enc.Byte(0x82); // mov eax, [r10 + disp32]
        }
        else if (size == 2) {
            enc.Byte(0x41);
            enc.Byte(0x0F);
            enc.Byte(0xB7);
            enc.Byte(0x82); // movzx eax, word [r10 + disp32]
        }
        else {
            enc.Byte(0x41);
            enc.Byte(0x0F);
            enc.Byte(0xB6);
            enc.Byte(0x82); // movzx eax, byte [r10 + disp32]
        }
        enc.Dword(static_cast<uint32_t>(offset));
    }

    void StoreChunkToR11(const int32_t offset, const int size) const {
        if (size == 8) {
            enc.Byte(0x49);
            enc.Byte(0x89);
            enc.Byte(0x83); // mov [r11 + disp32], rax
        }
        else if (size == 4) {
            enc.Byte(0x41);
            enc.Byte(0x89);
            enc.Byte(0x83); // mov [r11 + disp32], eax
        }
        else if (size == 2) {
            enc.Byte(0x66);
            enc.Byte(0x41);
            enc.Byte(0x89);
            enc.Byte(0x83); // mov [r11 + disp32], ax
        }
        else {
            enc.Byte(0x41);
            enc.Byte(0x88);
            enc.Byte(0x83); // mov [r11 + disp32], al
        }
        enc.Dword(static_cast<uint32_t>(offset));
    }

    template <typename StoreChunk>
    void CopyAggregateFromR10(const int size, StoreChunk storeChunk) const {
        int32_t offset = 0;
        for (const int chunkSize : {8, 4, 2, 1}) {
            while (offset + chunkSize <= size) {
                LoadChunkFromR10(offset, chunkSize);
                storeChunk(offset, chunkSize);
                offset += chunkSize;
            }
        }
    }

    void CopyAggregateFromR10ToStack(const int32_t dstDisp, const int size) const {
        CopyAggregateFromR10(size, [&](const int32_t offset, const int chunkSize) {
            if (chunkSize == 8) {
                enc.MovRaxStore(dstDisp + offset);
            }
            else if (chunkSize == 4) {
                enc.MovEaxStore(dstDisp + offset);
            }
            else if (chunkSize == 2) {
                enc.MovAxStore(dstDisp + offset);
            }
            else {
                enc.MovAlStore(dstDisp + offset);
            }
        });
    }

    void StoreHiddenReturnValue(const LirReg src, const TypeRef &t) const {
        if (hiddenReturnOff == 0) {
            LoadReturnValue(src, t);
            return;
        }
        enc.MovR11Load(-hiddenReturnOff);
        if (IsRegPointerTo(src, t)) {
            auto it = physRegMap.find(src);
            if (it != physRegMap.end()) {
                enc.MovR10PhysReg(it->second);
            }
            else {
                enc.MovR10Load(Disp(src));
            }
        }
        else {
            enc.Byte(0x4C);
            enc.Byte(0x8D);
            enc.Byte(0x95);
            enc.Dword(static_cast<uint32_t>(Disp(src))); // lea r10, [rbp + disp32]
        }
        CopyAggregateFromR10(SizeOfRuntime(t),
                             [&](const int32_t offset, const int size) { StoreChunkToR11(offset, size); });
        enc.Byte(0x4C);
        enc.Byte(0x89);
        enc.Byte(0xD8); // mov rax, r11
    }

    // Struct field lookup
    int FieldOffset(LirReg base, const std::string &fieldName) {
        auto typeIt = regTypes.find(base);
        if (typeIt == regTypes.end()) {
            return 0;
        }
        const TypeRef &pt = typeIt->second;
        if (pt.kind != TypeRef::Kind::Pointer || pt.inner.empty()) {
            return 0;
        }
        const TypeRef &inner = pt.inner[0];
        if (inner.kind == TypeRef::Kind::Range) {
            const TypeRef &elemType = inner.inner.empty() ? TypeRef::MakeInt64() : inner.inner[0];
            int elemSize = SizeOf(elemType);
            if (fieldName == "lo") {
                return 0;
            }
            if (fieldName == "hi") {
                return elemSize;
            }
            if (fieldName == "inclusive") {
                return 2 * elemSize;
            }
            return 0;
        }
        if (inner.kind == TypeRef::Kind::Tuple) {
            std::size_t idx = 0;
            try {
                idx = std::stoul(fieldName);
            }
            catch (...) {
                return 0;
            }
            int offset = 0;
            if (idx >= inner.inner.size()) {
                return 0;
            }
            for (std::size_t i = 0; i < idx && i < inner.inner.size(); ++i) {
                const int sz = SizeOf(inner.inner[i]);
                const int al = sz > 0 ? std::min(sz, 8) : 1;
                if (al > 1) {
                    offset = AlignUp(offset, al);
                }
                offset += sz > 0 ? sz : 8;
            }
            const int fieldSize = SizeOf(inner.inner[idx]);
            const int fieldAlign = fieldSize > 0 ? std::min(fieldSize, 8) : 1;
            if (fieldAlign > 1) {
                offset = AlignUp(offset, fieldAlign);
            }
            return offset;
        }
        if (inner.kind != TypeRef::Kind::Named) {
            return 0;
        }
        const std::string baseName = BaseTypeName(inner.name);
        if (interfaceNames.count(baseName)) {
            if (fieldName == "data") {
                return 0;
            }
            if (fieldName == "vtable") {
                return 8;
            }
            return 0;
        }
        if (baseName == "Slice") {
            if (fieldName == "data") {
                return 0;
            }
            if (fieldName == "length") {
                return 8;
            }
            return 0;
        }
        auto layIt = layouts.find(baseName);
        if (layIt == layouts.end()) {
            return 0;
        }
        for (const auto &field : layIt->second.fields) {
            if (field.name == fieldName) {
                return field.offset;
            }
        }
        return 0;
    }

    // Pre-pass: allocate stack slots
    int32_t AllocSlot(LirReg reg, int bytes) {
        if (auto it = slotMap.find(reg); it != slotMap.end()) {
            return it->second;
        }
        int al = (bytes > 0) ? std::min(bytes, 8) : 1;
        nextOff = AlignUp(nextOff, al);
        nextOff += (bytes > 0 ? bytes : 8);
        slotMap[reg] = nextOff;
        return nextOff;
    }

    int32_t AllocRegion(int bytes) {
        int al = (bytes > 0) ? std::min(bytes, 8) : 1;
        nextOff = AlignUp(nextOff, al);
        nextOff += (bytes > 0 ? bytes : 8);
        return nextOff;
    }

    void PrepassFunc(const LirFunc &func) {
        slotMap.clear();
        allocaData.clear();
        regTypes.clear();
        phiMoves.clear();
        physRegMap.clear();
        usedPhysRegs.clear();

        // 1. Collect type information and calculate intervals for virtual registers
        struct LiveInterval {
            LirReg reg;
            int start = -1;
            int end = -1;
            TypeRef type;
        };

        std::unordered_map<LirReg, LiveInterval> intervals;
        int instIdx = 0;

        std::unordered_map<LirReg, TypeRef> tempRegTypes;

        // Params
        for (const auto &p : func.params) {
            tempRegTypes[p.reg] = IsWin64AddressParam(p.type) ? TypeRef::MakePointer(p.type) : p.type;
            intervals[p.reg] = {p.reg, 0, 0, tempRegTypes[p.reg]};
        }

        // Instructions
        for (uint32_t bi = 0; bi < func.blocks.size(); ++bi) {
            for (const auto &instr : func.blocks[bi].instrs) {
                if (instr.dst != LirNoReg) {
                    if (instr.op == LirOpcode::Alloca) {
                        tempRegTypes[instr.dst] = TypeRef::MakePointer(instr.type);
                    }
                    else {
                        tempRegTypes[instr.dst] = instr.type;
                    }
                }

                for (LirReg src : instr.srcs) {
                    TypeRef srcT = tempRegTypes.contains(src) ? tempRegTypes[src] : TypeRef::MakeInt64();
                    if (intervals.find(src) == intervals.end()) {
                        intervals[src] = {src, instIdx, instIdx, srcT};
                    }
                    else {
                        intervals[src].end = instIdx;
                    }
                }

                if (instr.dst != LirNoReg) {
                    if (intervals.find(instr.dst) == intervals.end()) {
                        intervals[instr.dst] = {instr.dst, instIdx, instIdx, tempRegTypes[instr.dst]};
                    }
                    else {
                        intervals[instr.dst].end = instIdx;
                    }
                }

                if (instr.op == LirOpcode::Phi) {
                    for (const auto &[src, pred] : instr.phiPreds) {
                        TypeRef srcT = tempRegTypes.contains(src) ? tempRegTypes[src] : TypeRef::MakeInt64();
                        if (intervals.find(src) == intervals.end()) {
                            intervals[src] = {src, instIdx, instIdx, srcT};
                        }
                        else {
                            intervals[src].end = instIdx;
                        }
                    }
                }

                instIdx++;
            }

            if (func.blocks[bi].term) {
                const auto &term = *func.blocks[bi].term;
                if (term.cond != LirNoReg) {
                    TypeRef condT = tempRegTypes.contains(term.cond) ? tempRegTypes[term.cond] : TypeRef::MakeInt64();
                    if (intervals.find(term.cond) == intervals.end()) {
                        intervals[term.cond] = {term.cond, instIdx, instIdx, condT};
                    }
                    else {
                        intervals[term.cond].end = instIdx;
                    }
                }
                if (term.retVal && *term.retVal != LirNoReg) {
                    TypeRef retT =
                        tempRegTypes.contains(*term.retVal) ? tempRegTypes[*term.retVal] : TypeRef::MakeInt64();
                    if (intervals.find(*term.retVal) == intervals.end()) {
                        intervals[*term.retVal] = {*term.retVal, instIdx, instIdx, retT};
                    }
                    else {
                        intervals[*term.retVal].end = instIdx;
                    }
                }
                instIdx++;
            }
        }

        // 2. Perform Register Allocation (Linear Scan)
        std::vector<LiveInterval> candidates;
        if (func.blocks.size() == 1) {
            for (const auto &[reg, iv] : intervals) {
                if (IsFloat(iv.type) || IsAggregate(iv.type) || SizeOfRuntime(iv.type) > 8) {
                    continue;
                }
                candidates.push_back(iv);
            }
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const LiveInterval &a, const LiveInterval &b) { return a.start < b.start; });

        constexpr int numPhysRegs = 5;
        std::vector<int> regEndTimes(numPhysRegs, -1);

        for (const auto &iv : candidates) {
            int allocatedIdx = -1;
            for (int r = 0; r < numPhysRegs; ++r) {
                if (regEndTimes[r] < iv.start) {
                    allocatedIdx = r;
                    break;
                }
            }

            if (allocatedIdx != -1) {
                physRegMap[iv.reg] = allocatedIdx;
                regEndTimes[allocatedIdx] = iv.end;
                if (std::find(usedPhysRegs.begin(), usedPhysRegs.end(), allocatedIdx) == usedPhysRegs.end()) {
                    usedPhysRegs.push_back(allocatedIdx);
                }
            }
        }

        std::sort(usedPhysRegs.begin(), usedPhysRegs.end());

        // 3. Allocate Stack Slots
        nextOff = static_cast<int32_t>(usedPhysRegs.size() * 8);
        hiddenReturnOff = 0;

        const CallingConvention conv = EffectiveConv(func.callConv);
        if ((conv == CallingConvention::Win64 && IsWin64ByRefAggregate(func.returnType)) ||
            (conv == CallingConvention::SysV && IsSysVMemoryAggregate(func.returnType))) {
            hiddenReturnOff = AllocRegion(8);
        }

        for (const auto &p : func.params) {
            regTypes[p.reg] = IsWin64AddressParam(p.type) ? TypeRef::MakePointer(p.type) : p.type;
            AllocSlot(p.reg, std::max(8, SizeOfRuntime(regTypes[p.reg])));
        }

        for (uint32_t bi = 0; bi < func.blocks.size(); ++bi) {
            for (const auto &instr : func.blocks[bi].instrs) {
                if (instr.op == LirOpcode::Phi) {
                    for (const auto &[src, pred] : instr.phiPreds) {
                        phiMoves[pred][bi].push_back({instr.dst, src, instr.type});
                    }
                }
                if (instr.dst == LirNoReg) {
                    continue;
                }

                if (instr.op == LirOpcode::Alloca) {
                    int dsz;
                    if (!instr.strArg.empty()) {
                        int count = 0;
                        try {
                            count = std::stoi(instr.strArg);
                        }
                        catch (...) {
                        }
                        const TypeRef &elemType = instr.type.inner.empty() ? instr.type : instr.type.inner[0];
                        int elemSize = SizeOfRuntime(elemType);
                        dsz = count * (elemSize > 0 ? elemSize : 8);
                    }
                    else {
                        dsz = StackValueSize(instr.type);
                    }

                    AllocSlot(instr.dst, 8);
                    allocaData[instr.dst] = AllocRegion(dsz > 0 ? dsz : 8);
                    regTypes[instr.dst] = TypeRef::MakePointer(instr.type);
                }
                else {
                    regTypes[instr.dst] = instr.type;
                    int sz = StackValueSize(instr.type);
                    AllocSlot(instr.dst, sz > 0 ? sz : 8);
                }
            }
        }
        frameSize = AlignUp(nextOff, 16);
        if (frameSize == 0) {
            frameSize = 16;
        }
    }

    // Build struct layouts
    void BuildLayouts() {
        for (const auto &name : packageInterfaceNames) {
            interfaceNames.insert(name);
        }
        for (const auto &s : structDecls) {
            StructLayout layout;
            int offset = 0, maxAlign = 1;
            for (const auto &f : s.fields) {
                int sz = SizeOfRuntime(f.type);
                int al = sz > 0 ? std::min(sz, 8) : 1;
                if (f.type.kind == TypeRef::Kind::Named) {
                    auto it = layouts.find(BaseTypeName(f.type.name));
                    if (it != layouts.end()) {
                        sz = it->second.totalSize;
                        al = it->second.alignment;
                    }
                }
                if (al > 1) {
                    offset = AlignUp(offset, al);
                }
                layout.fields.push_back({f.name, offset, sz});
                offset += (sz > 0 ? sz : 8);
                maxAlign = std::max(maxAlign, al);
            }
            layout.totalSize = AlignUp(offset, maxAlign);
            layout.alignment = maxAlign;
            layouts[s.name] = std::move(layout);
        }
    }

    // Phi move emission
    bool HasPhiMoves(const uint32_t from, uint32_t to) const {
        auto it = phiMoves.find(from);
        if (it == phiMoves.end()) {
            return false;
        }
        return it->second.contains(to);
    }

    void EmitPhiMoves(const uint32_t from, uint32_t to) {
        auto it1 = phiMoves.find(from);
        if (it1 == phiMoves.end()) {
            return;
        }
        auto it2 = it1->second.find(to);
        if (it2 == it1->second.end()) {
            return;
        }

        std::vector<Rux::PhiMove> rawMoves;
        rawMoves.reserve(it2->second.size());
        for (const auto &m : it2->second) {
            rawMoves.push_back({m.dst, m.src, m.type});
        }

        std::vector<PhiMoveStep> steps = ResolvePhiMoves(std::move(rawMoves));
        int32_t tempDisp = -static_cast<int32_t>(frameSize + 8);

        for (const auto &step : steps) {
            if (step.kind == PhiMoveStep::Kind::SaveDestination) {
                int sz = SizeOfRuntime(step.type);
                if (sz == 16) {
                    LoadA(step.dst, step.type);
                    enc.MovRaxStore(tempDisp);
                    enc.Byte(0x48);
                    enc.Byte(0x89);
                    enc.Byte(0x95);
                    enc.Dword(static_cast<uint32_t>(tempDisp + 8));
                }
                else if (IsFloat(step.type)) {
                    LoadA(step.dst, step.type);
                    if (step.type.kind == TypeRef::Kind::Float32) {
                        enc.MovssXmm0Store(tempDisp);
                    }
                    else {
                        enc.MovsdXmm0Store(tempDisp);
                    }
                }
                else {
                    LoadA(step.dst, step.type);
                    int ss = (sz > 0) ? sz : 8;
                    if (ss == 8) {
                        enc.MovRaxStore(tempDisp);
                    }
                    else if (ss == 4) {
                        enc.MovEaxStore(tempDisp);
                    }
                    else if (ss == 2) {
                        enc.MovAxStore(tempDisp);
                    }
                    else {
                        enc.MovAlStore(tempDisp);
                    }
                }
            }
            else {
                if (step.sourceIsTemporary) {
                    int sz = SizeOfRuntime(step.type);
                    if (sz == 16) {
                        enc.MovRaxLoad(tempDisp);
                        enc.MovR10Load(tempDisp + 8);
                        enc.Byte(0x4C);
                        enc.Byte(0x89);
                        enc.Byte(0xD2);
                        StoreA(step.dst, step.type);
                    }
                    else if (IsFloat(step.type)) {
                        if (step.type.kind == TypeRef::Kind::Float32) {
                            enc.MovssXmm0Load(tempDisp);
                        }
                        else {
                            enc.MovsdXmm0Load(tempDisp);
                        }
                        StoreA(step.dst, step.type);
                    }
                    else {
                        int ss = (sz > 0) ? sz : 8;
                        if (ss == 8) {
                            enc.MovRaxLoad(tempDisp);
                        }
                        else if (step.type.IsSigned()) {
                            if (ss == 4) {
                                enc.MovsxdRaxDword(tempDisp);
                            }
                            else if (ss == 2) {
                                enc.MovsxRaxWord(tempDisp);
                            }
                            else {
                                enc.MovsxRaxByte(tempDisp);
                            }
                        }
                        else {
                            if (ss == 4) {
                                enc.MovEaxLoad(tempDisp);
                            }
                            else if (ss == 2) {
                                enc.MovzxRaxWord(tempDisp);
                            }
                            else {
                                enc.MovzxRaxByte(tempDisp);
                            }
                        }
                        StoreA(step.dst, step.type);
                    }
                }
                else {
                    LoadA(step.src, step.type);
                    StoreA(step.dst, step.type);
                }
            }
        }
    }

    void EmitStackAlloc(int32_t bytes) const {
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

    // Call argument setup
    void EmitCallArgs(const std::vector<LirReg> &args, CallingConvention conv = CallingConvention::Default,
                      int startIdx = 0) const {
        if (EffectiveConv(conv) == CallingConvention::Win64) {
            // Unified index: rcx/xmm0=0, rdx/xmm1=1, r8/xmm2=2,
            // r9/xmm3=3
            int idx = startIdx;
            for (LirReg arg : args) {
                TypeRef at = regTypes.contains(arg) ? regTypes.at(arg) : TypeRef::MakeInt64();
                int32_t d = Disp(arg);
                if (idx >= 4) {
                    const int32_t stackArgOff = 32 + (idx - 4) * 8;
                    if (IsFloat(at)) {
                        LoadA(arg, at);
                        if (SizeOf(at) == 4) {
                            enc.MovssXmm0StoreRsp(stackArgOff);
                        }
                        else {
                            enc.MovsdXmm0StoreRsp(stackArgOff);
                        }
                    }
                    else if (IsWin64ByRefAggregate(at)) {
                        enc.LeaRaxStack(d);
                        enc.MovRaxStoreRsp(stackArgOff);
                    }
                    else {
                        LoadA(arg, at);
                        enc.MovRaxStoreRsp(stackArgOff);
                    }
                    ++idx;
                    continue;
                }

                if (IsFloat(at)) {
                    int sz = SizeOf(at);
                    if (sz == 4) {
                        enc.MovssXmmNLoad(idx, d);
                    }
                    else {
                        enc.MovsdXmmNLoad(idx, d);
                    }
                }
                else if (IsWin64ByRefAggregate(at)) {
                    enc.LeaArgStackWin64(idx, d);
                }
                else if (IsPointerToWin64ByRefAggregate(at)) {
                    auto it = physRegMap.find(arg);
                    if (it != physRegMap.end()) {
                        LoadA(arg, at);
                        enc.MovArgWin64Rax(idx);
                    }
                    else {
                        enc.MovArgLoadWin64(idx, d);
                    }
                }
                else {
                    LoadA(arg, at);
                    enc.MovArgWin64Rax(idx);
                }
                ++idx;
            }
        }
        else {
            int intIdx = startIdx, fltIdx = 0;
            for (LirReg arg : args) {
                TypeRef at = regTypes.contains(arg) ? regTypes.at(arg) : TypeRef::MakeInt64();
                int32_t d = Disp(arg);
                if (IsFloat(at)) {
                    if (fltIdx < 8) {
                        int sz = SizeOf(at);
                        if (sz == 4) {
                            enc.MovssXmmNLoad(fltIdx, d);
                        }
                        else {
                            enc.MovsdXmmNLoad(fltIdx, d);
                        }
                        ++fltIdx;
                    }
                }
                else if (IsAggregate(at) && SizeOfRuntime(at) == 16) {
                    if (intIdx <= 4) {
                        enc.MovRaxLoad(d);
                        enc.MovArgRax(intIdx++);
                        enc.MovRaxLoad(d + 8);
                        enc.MovArgRax(intIdx++);
                    }
                }
                else {
                    if (intIdx < 6) {
                        LoadA(arg, at);
                        enc.MovArgRax(intIdx);
                        ++intIdx;
                    }
                }
            }
            // System V AMD64 requires AL to hold the number of vector
            // registers used when calling a variadic function; it is an
            // ignored scratch register for non-variadic callees, so setting
            // it unconditionally is always safe and lets printf-style
            // functions decide whether to spill XMM registers.
            enc.MovEaxImm32(fltIdx);
        }
    }

    // Instruction code generation
    void GenInstr(const LirInstr &instr) {
        switch (instr.op) {
        case LirOpcode::Const: {
            if (instr.dst == LirNoReg) {
                break;
            }
            const TypeRef &t = instr.type;
            int sz = SizeOf(t);
            if (t.kind == TypeRef::Kind::Str) {
                uint32_t symIdx = InternStr(instr.strArg);
                uint32_t relocOff;
                enc.LeaRaxRip(relocOff);
                AddTextReloc(relocOff, symIdx);
                StoreA(instr.dst, t);
            }
            else if (t.kind == TypeRef::Kind::Float32) {
                uint32_t symIdx = InternF32(instr.strArg);
                uint32_t relocOff;
                enc.MovssXmm0Rip(relocOff);
                AddTextReloc(relocOff, symIdx);
                enc.MovssXmm0Store(Disp(instr.dst));
            }
            else if (t.kind == TypeRef::Kind::Float64) {
                uint32_t symIdx = InternF64(instr.strArg);
                uint32_t relocOff;
                enc.MovsdXmm0Rip(relocOff);
                AddTextReloc(relocOff, symIdx);
                enc.MovsdXmm0Store(Disp(instr.dst));
            }
            else if (t.IsBool()) {
                enc.MovEaxImm32((instr.strArg == "true" || instr.strArg == "1") ? 1 : 0);
                StoreA(instr.dst, t);
            }
            else {
                const std::string &sv = instr.strArg.empty() ? "0" : instr.strArg;
                const std::uint64_t bits = ParseIntegerLiteralBits(sv).value_or(0);
                if (bits <= 0x7FFF'FFFF) {
                    enc.MovEaxImm32(static_cast<int32_t>(bits));
                }
                else {
                    enc.MovRaxImm64(static_cast<std::int64_t>(bits));
                }
                StoreA(instr.dst, sz > 0 ? t : TypeRef::MakeInt64());
            }
            break;
        }
        case LirOpcode::Alloca: {
            int32_t dataOff = allocaData.at(instr.dst);
            enc.LeaRaxStack(-dataOff);
            StoreA(instr.dst, TypeRef::MakePointer(instr.type));
            break;
        }
        case LirOpcode::Assert:
        case LirOpcode::Panic: {
            const bool isAssertion = instr.op == LirOpcode::Assert;
            if (instr.srcs.size() < (isAssertion ? 2 : 1)) {
                break;
            }
            uint32_t okPatch = 0;
            if (isAssertion) {
                LoadA(instr.srcs[0], TypeRef::MakeBool());
                enc.TestRaxRax();
                enc.Jnz(okPatch);
            }

            const LirReg messageReg = instr.srcs[isAssertion ? 1 : 0];
            const std::string prefix = isAssertion ? "Assertion failed: " : "Panic: ";
            const std::string function = instr.sourceFunction.empty() ? "<unknown>" : instr.sourceFunction;
            const std::string file = instr.sourceFile.empty() ? "<unknown>" : instr.sourceFile;
            const std::string suffix =
                std::format("\n  at {} ({}:{}:{})\n", function, file, instr.sourceLine, instr.sourceColumn);

            if (targetOs == Target::OS::Windows) {
                const uint32_t getStdHandle = GetOrAddExtern("GetStdHandle", RcuSymKind::ExternFunc, "KERNEL32.DLL");
                const uint32_t writeFile = GetOrAddExtern("WriteFile", RcuSymKind::ExternFunc, "KERNEL32.DLL");

                // Shadow space, the fifth WriteFile argument, and a DWORD for
                // lpNumberOfBytesWritten. The failure path never returns.
                enc.SubRspImm32(48);
                enc.MovQwordRspImm32(32, 0);

                const auto prepareWrite = [&]() {
                    enc.MovEaxImm32(-12); // STD_ERROR_HANDLE
                    enc.MovArgWin64Rax(0);
                    uint32_t getHandleReloc;
                    enc.Call(getHandleReloc);
                    AddTextReloc(getHandleReloc, getStdHandle);
                    enc.MovArgWin64Rax(0);
                    enc.LeaR9Rsp(40);
                };
                const auto writeStatic = [&](const std::string &text) {
                    prepareWrite();
                    const uint32_t textSymbol = InternStr(text);
                    uint32_t textReloc;
                    enc.LeaRaxRip(textReloc);
                    AddTextReloc(textReloc, textSymbol);
                    enc.MovArgWin64Rax(1);
                    enc.MovEaxImm32(static_cast<int32_t>(text.size()));
                    enc.MovArgWin64Rax(2);
                    uint32_t writeReloc;
                    enc.Call(writeReloc);
                    AddTextReloc(writeReloc, writeFile);
                };

                writeStatic(prefix);

                prepareWrite();
                LoadA(messageReg, TypeRef::MakePointer(TypeRef::MakeNamed("Slice<char8>")));
                enc.MovR10Rax();
                enc.MovRdxR10Load();
                enc.MovR8R10Load(8);
                uint32_t messageWriteReloc;
                enc.Call(messageWriteReloc);
                AddTextReloc(messageWriteReloc, writeFile);

                writeStatic(suffix);
            }
            else {
                const int syscallNumber = targetOs == Target::OS::Linux ? 1
                                        : targetOs == Target::OS::MacOS ? 0x0200'0004
                                                                        : 4;
                const auto writeStatic = [&](const std::string &text) {
                    const uint32_t textSymbol = InternStr(text);
                    uint32_t textReloc;
                    enc.LeaRaxRip(textReloc);
                    AddTextReloc(textReloc, textSymbol);
                    enc.MovRsiRax();
                    enc.MovEdxImm32(static_cast<int32_t>(text.size()));
                    enc.MovEdiImm32(2);
                    enc.MovEaxImm32(syscallNumber);
                    enc.Syscall();
                };

                writeStatic(prefix);
                LoadA(messageReg, TypeRef::MakePointer(TypeRef::MakeNamed("Slice<char8>")));
                enc.MovR10Rax();
                enc.MovRsiR10Load();
                enc.MovRdxR10Load(8);
                enc.MovEdiImm32(2);
                enc.MovEaxImm32(syscallNumber);
                enc.Syscall();
                writeStatic(suffix);
            }

            enc.Ud2();
            if (isAssertion) {
                const auto here = static_cast<int32_t>(enc.Size());
                enc.Patch32(okPatch, here - static_cast<int32_t>(okPatch + 4));
            }
            break;
        }
        case LirOpcode::Load: {
            const TypeRef &t = instr.type;
            int sz = SizeOfRuntime(t);
            int runtimeSz = SizeOfRuntime(t);
            if (!instr.strArg.empty()) {
                // Named global — load via RIP-relative
                uint32_t symIdx = GetOrAddExtern(instr.strArg, RcuSymKind::ExternData);
                uint32_t relocOff;
                enc.MovRaxRip(relocOff);
                AddTextReloc(relocOff, symIdx);
            }
            else {
                LirReg ptr = instr.srcs[0];
                auto it = physRegMap.find(ptr);
                if (it != physRegMap.end()) {
                    enc.MovR10PhysReg(it->second);
                }
                else {
                    enc.MovR10Load(Disp(ptr));
                }
                if (IsAggregate(t) && runtimeSz > 8) {
                    CopyAggregateFromR10ToStack(Disp(instr.dst), runtimeSz);
                    break;
                }
                // Load through pointer: use r10 as base
                // Emit: mov rax, [r10]  (49 8B 02)
                if (IsFloat(t)) {
                    // movss/movsd xmm0, [r10]
                    if (sz == 4) {
                        enc.Byte(0xF3);
                        enc.Byte(0x41);
                        enc.Byte(0x0F);
                        enc.Byte(0x10);
                        enc.Byte(0x02);
                    }
                    else {
                        enc.Byte(0xF2);
                        enc.Byte(0x41);
                        enc.Byte(0x0F);
                        enc.Byte(0x10);
                        enc.Byte(0x02);
                    }
                    StoreA(instr.dst, t);
                    break;
                }
                else if (sz == 8 || sz == 0) {
                    enc.Byte(0x49);
                    enc.Byte(0x8B);
                    enc.Byte(0x02); // mov rax, [r10]
                }
                else if (t.IsSigned()) {
                    if (sz == 4) {
                        enc.Byte(0x49);
                        enc.Byte(0x63);
                        enc.Byte(0x02); // movsxd rax,[r10]
                    }
                    else if (sz == 2) {
                        enc.Byte(0x49);
                        enc.Byte(0x0F);
                        enc.Byte(0xBF);
                        enc.Byte(0x02);
                    }
                    else {
                        enc.Byte(0x49);
                        enc.Byte(0x0F);
                        enc.Byte(0xBE);
                        enc.Byte(0x02);
                    }
                }
                else {
                    if (sz == 4) {
                        enc.Byte(0x41);
                        enc.Byte(0x8B);
                        enc.Byte(0x02); // mov eax, [r10]  (zero-extends
                        // to rax)
                    }
                    else if (sz == 2) {
                        enc.Byte(0x49);
                        enc.Byte(0x0F);
                        enc.Byte(0xB7);
                        enc.Byte(0x02); // movzx rax, word [r10]
                    }
                    else {
                        enc.Byte(0x49);
                        enc.Byte(0x0F);
                        enc.Byte(0xB6);
                        enc.Byte(0x02); // movzx rax, byte [r10]
                    }
                }
            }
            StoreA(instr.dst, sz > 0 ? t : TypeRef::MakeInt64());
            break;
        }
        case LirOpcode::Store: {
            LirReg val = instr.srcs[0];
            LirReg ptr = instr.srcs[1];
            const TypeRef &t = instr.type;
            int sz = SizeOfRuntime(t);
            int runtimeSz = SizeOfRuntime(t);
            auto itPtr = physRegMap.find(ptr);
            if (itPtr != physRegMap.end()) {
                enc.MovR11PhysReg(itPtr->second);
            }
            else {
                enc.MovR11Load(Disp(ptr));
            }
            if (IsAggregate(t) && runtimeSz > 8) {
                if (IsRegPointerTo(val, t)) {
                    auto it = physRegMap.find(val);
                    if (it != physRegMap.end()) {
                        enc.MovR10PhysReg(it->second);
                    }
                    else {
                        enc.MovR10Load(Disp(val));
                    }
                }
                else {
                    enc.Byte(0x4C);
                    enc.Byte(0x8D);
                    enc.Byte(0x95);
                    enc.Dword(static_cast<uint32_t>(Disp(val))); // lea r10, [rbp + disp32]
                }
                CopyAggregateFromR10(runtimeSz,
                                     [&](const int32_t offset, const int size) { StoreChunkToR11(offset, size); });
                break;
            }
            if (IsFloat(t)) {
                LoadA(val, t);
                // movss/movsd [r11], xmm0
                if (sz == 4) {
                    enc.Byte(0xF3);
                    enc.Byte(0x41);
                    enc.Byte(0x0F);
                    enc.Byte(0x11);
                    enc.Byte(0x03);
                }
                else {
                    enc.Byte(0xF2);
                    enc.Byte(0x41);
                    enc.Byte(0x0F);
                    enc.Byte(0x11);
                    enc.Byte(0x03);
                }
            }
            else {
                const int ss = (sz > 0) ? sz : 8;
                LoadA(val, t);
                // mov [r11], rax/eax/ax/al
                if (ss == 8) {
                    enc.Byte(0x49);
                    enc.Byte(0x89);
                    enc.Byte(0x03);
                }
                else if (ss == 4) {
                    enc.Byte(0x41);
                    enc.Byte(0x89);
                    enc.Byte(0x03);
                }
                else if (ss == 2) {
                    enc.Byte(0x66);
                    enc.Byte(0x41);
                    enc.Byte(0x89);
                    enc.Byte(0x03);
                }
                else {
                    enc.Byte(0x41);
                    enc.Byte(0x88);
                    enc.Byte(0x03);
                }
            }
            break;
        }
        case LirOpcode::Add:
        case LirOpcode::Sub:
        case LirOpcode::And:
        case LirOpcode::Or:
        case LirOpcode::Xor: {
            const TypeRef &t = instr.type;
            if (IsFloat(t)) {
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                const bool f32 = (t.kind == TypeRef::Kind::Float32);
                if (instr.op == LirOpcode::Add) {
                    if (f32) {
                        enc.AddssXmm01();
                    }
                    else {
                        enc.AddsdXmm01();
                    }
                }
                else if (instr.op == LirOpcode::Sub) {
                    if (f32) {
                        enc.SubssXmm01();
                    }
                    else {
                        enc.SubsdXmm01();
                    }
                }
                else {
                    if (f32) {
                        enc.AddssXmm01();
                    }
                    else {
                        enc.AddsdXmm01();
                    }
                } // bitwise on float: fallback
                StoreA(instr.dst, t);
            }
            else {
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                if (instr.op == LirOpcode::Add) {
                    enc.AddRaxR10();
                }
                else if (instr.op == LirOpcode::Sub) {
                    enc.SubRaxR10();
                }
                else if (instr.op == LirOpcode::And) {
                    enc.AndRaxR10();
                }
                else if (instr.op == LirOpcode::Or) {
                    enc.OrRaxR10();
                }
                else {
                    enc.XorRaxR10();
                }
                StoreA(instr.dst, t);
            }
            break;
        }
        case LirOpcode::Mul: {
            const TypeRef &t = instr.type;
            if (IsFloat(t)) {
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                if (t.kind == TypeRef::Kind::Float32) {
                    enc.MulssXmm01();
                }
                else {
                    enc.MulsdXmm01();
                }
            }
            else {
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                enc.ImulRaxR10();
            }
            StoreA(instr.dst, t);
            break;
        }
        case LirOpcode::Div:
        case LirOpcode::Mod: {
            if (const TypeRef &t = instr.type; IsFloat(t)) {
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                if (instr.op == LirOpcode::Div) {
                    if (t.kind == TypeRef::Kind::Float32) {
                        enc.DivssXmm01();
                    }
                    else {
                        enc.DivsdXmm01();
                    }
                }
                else {
                    // Mod
                    if (t.kind == TypeRef::Kind::Float32) {
                        enc.FmodssXmm01();
                    }
                    else {
                        enc.FmodsdXmm01();
                    }
                }
                StoreA(instr.dst, t);
            }
            else {
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                if (t.IsSigned()) {
                    enc.Cqo();
                    enc.IdivR10();
                }
                else {
                    enc.XorRdxRdx();
                    enc.DivR10();
                }
                if (instr.op == LirOpcode::Mod) {
                    enc.MovRaxRdx();
                }
                StoreA(instr.dst, t);
            }
            break;
        }
        case LirOpcode::Pow: {
            const TypeRef &t = instr.type;
            const bool win64Call = EffectiveConv(CallingConvention::Default) == CallingConvention::Win64;
            const int callFrameSize = win64Call ? Win64CallFrameSize(instr.srcs.size()) : 0;
            if (win64Call) {
                enc.SubRspImm32(callFrameSize);
            }
            if (IsFloat(t)) {
                // Float exponentiation uses a synthesized x87 helper rather
                // than libm pow, so no CRT/math DLL import is required.
                uint32_t sym = t.kind == TypeRef::Kind::Float32 ? EnsureFloatPowF32Helper() : EnsureFloatPowHelper();
                EmitCallArgs(instr.srcs, CallingConvention::Default);
                uint32_t ro;
                enc.Call(ro);
                AddTextReloc(ro, sym);
            }
            else {
                uint32_t sym = EnsureIntPowHelper();
                EmitCallArgs(instr.srcs, CallingConvention::Default);
                uint32_t ro;
                enc.Call(ro);
                AddTextReloc(ro, sym);
            }
            if (win64Call) {
                enc.AddRspImm32(callFrameSize);
            }
            StoreA(instr.dst, t);
            break;
        }
        case LirOpcode::Shl:
        case LirOpcode::Shr: {
            const TypeRef &t = instr.type;
            LoadA(instr.srcs[0], t);
            auto it = physRegMap.find(instr.srcs[1]);
            if (it != physRegMap.end()) {
                enc.MovR11PhysReg(it->second);
            }
            else {
                enc.MovR11Load(Disp(instr.srcs[1]));
            }
            enc.MovRcxR11();
            bool isShr = (instr.op == LirOpcode::Shr);
            if (isShr && t.IsSigned()) {
                enc.SarRaxCl();
            }
            else if (isShr) {
                enc.ShrRaxCl();
            }
            else {
                enc.ShlRaxCl();
            }
            StoreA(instr.dst, t);
            break;
        }

        case LirOpcode::Neg: {
            const TypeRef &t = instr.type;
            if (IsFloat(t)) {
                LoadA(instr.srcs[0], t);
                const bool f32 = (t.kind == TypeRef::Kind::Float32);
                const uint32_t maskSym = f32 ? InternF32SignMask() : InternF64SignMask();
                uint32_t ro;
                if (f32) {
                    enc.MovssXmm1Rip(ro);
                }
                else {
                    enc.MovsdXmm1Rip(ro);
                }
                AddTextReloc(ro, maskSym);
                if (f32) {
                    enc.XorpsXmm01();
                }
                else {
                    enc.XorpdXmm01();
                }
                StoreA(instr.dst, t);
            }
            else {
                LoadA(instr.srcs[0], t);
                enc.NegRax();
                StoreA(instr.dst, t);
            }
            break;
        }

        case LirOpcode::Not: {
            LoadA(instr.srcs[0], instr.type);
            enc.TestRaxRax();
            enc.SeteAl();
            enc.MovzxRaxAl();
            StoreA(instr.dst, TypeRef::MakeBool());
            break;
        }
        case LirOpcode::BitNot: {
            LoadA(instr.srcs[0], instr.type);
            if (instr.type.IsBool()) {
                // For bools, ~ is logical NOT (xor with 1) so that
                // ~true == false and ~false == true, matching the
                // docs at Web/src/docs/types/bool.md (issue #95).
                // A plain bitwise NOT would leave 0xFE in the low
                // byte for `~true`, which the bool loads back as
                // truthy.
                enc.XorRaxImmediate(1);
            }
            else {
                enc.NotRax();
            }
            StoreA(instr.dst, instr.type);
            break;
        }
        case LirOpcode::CmpEq:
        case LirOpcode::CmpNe:
        case LirOpcode::CmpLt:
        case LirOpcode::CmpLe:
        case LirOpcode::CmpGt:
        case LirOpcode::CmpGe: {
            const TypeRef &lhsT = regTypes.contains(instr.srcs[0]) ? regTypes.at(instr.srcs[0]) : instr.type;
            LoadA(instr.srcs[0], lhsT);
            LoadB(instr.srcs[1], lhsT);
            if (IsFloat(lhsT)) {
                if (lhsT.kind == TypeRef::Kind::Float32) {
                    enc.UcomissXmm01();
                }
                else {
                    enc.UcomisdXmm01();
                }

                switch (instr.op) {
                case LirOpcode::CmpEq:
                    // ordered && equal
                    enc.SeteAl();  // AL = ZF
                    enc.SetnpDl(); // DL = !PF
                    enc.AndAlDl();
                    break;

                case LirOpcode::CmpNe:
                    // unordered || unequal
                    enc.SetneAl(); // AL = !ZF
                    enc.SetpDl();  // DL = PF
                    enc.OrAlDl();
                    break;

                case LirOpcode::CmpLt:
                    // ordered && CF
                    enc.SetbAl();
                    enc.SetnpDl();
                    enc.AndAlDl();
                    break;

                case LirOpcode::CmpLe:
                    // ordered && (CF || ZF)
                    enc.SetbeAl();
                    enc.SetnpDl();
                    enc.AndAlDl();
                    break;

                case LirOpcode::CmpGt:
                    // ordered && (!CF && !ZF)
                    enc.SetaAl();
                    enc.SetnpDl();
                    enc.AndAlDl();
                    break;

                case LirOpcode::CmpGe:
                    // ordered && !CF
                    enc.SetaeAl();
                    enc.SetnpDl();
                    enc.AndAlDl();
                    break;

                default:
                    break;
                }
            }
            else {
                enc.CmpRaxR10();
                bool sig = lhsT.IsSigned();
                switch (instr.op) {
                case LirOpcode::CmpEq:
                    enc.SeteAl();
                    break;
                case LirOpcode::CmpNe:
                    enc.SetneAl();
                    break;
                case LirOpcode::CmpLt:
                    sig ? enc.SetlAl() : enc.SetbAl();
                    break;
                case LirOpcode::CmpLe:
                    sig ? enc.SetleAl() : enc.SetbeAl();
                    break;
                case LirOpcode::CmpGt:
                    sig ? enc.SetgAl() : enc.SetaAl();
                    break;
                default:
                    sig ? enc.SetgeAl() : enc.SetaeAl();
                    break;
                }
            }
            enc.MovzxRaxAl();
            StoreA(instr.dst, TypeRef::MakeBool());
            break;
        }
        case LirOpcode::Cast: {
            const TypeRef &dstT = instr.type;
            TypeRef srcT = regTypes.contains(instr.srcs[0]) ? regTypes.at(instr.srcs[0]) : dstT;
            LoadA(instr.srcs[0], srcT);
            bool srcFl = IsFloat(srcT), dstFl = IsFloat(dstT);
            if (srcFl && !dstFl) {
                if (srcT.kind == TypeRef::Kind::Float32) {
                    enc.CvttsssiRaxXmm0();
                }
                else {
                    enc.CvttsdsiRaxXmm0();
                }
            }
            else if (!srcFl && dstFl) {
                if (dstT.kind == TypeRef::Kind::Float32) {
                    enc.Cvtsi2ssXmm0Rax();
                }
                else {
                    enc.Cvtsi2sdXmm0Rax();
                }
            }
            else if (srcFl && dstFl) {
                if (srcT.kind == TypeRef::Kind::Float32 && dstT.kind == TypeRef::Kind::Float64) {
                    enc.CvtsssdXmm0();
                }
                else if (srcT.kind == TypeRef::Kind::Float64 && dstT.kind == TypeRef::Kind::Float32) {
                    enc.CvtsdssXmm0();
                }
            }
            StoreA(instr.dst, dstT);
            break;
        }
        case LirOpcode::Call: {
            // Built-in: FloatBits64 — reinterpret float64 bits as uint64
            if (instr.strArg == "FloatBits64" && instr.srcs.size() == 1) {
                enc.MovsdXmm0Load(Disp(instr.srcs[0]));
                enc.Byte(0x66);
                enc.Byte(0x48);
                enc.Byte(0x0F);
                enc.Byte(0x7E);
                enc.Byte(0xC0); // movq rax, xmm0
                if (instr.dst != LirNoReg && !instr.type.IsOpaque()) {
                    StoreReturnValue(instr.dst, instr.type);
                }
                break;
            }
            // Built-in: FloatFromBits64 — reinterpret uint64 bits as float64
            if (instr.strArg == "FloatFromBits64" && instr.srcs.size() == 1) {
                LoadA(instr.srcs[0], TypeRef::MakeInt64());
                enc.Byte(0x66);
                enc.Byte(0x48);
                enc.Byte(0x0F);
                enc.Byte(0x6E);
                enc.Byte(0xC0); // movq xmm0, rax
                if (instr.dst != LirNoReg && !instr.type.IsOpaque()) {
                    StoreReturnValue(instr.dst, instr.type);
                }
                break;
            }
            // Built-in: FloatBits32 — reinterpret float32 bits as uint32
            if (instr.strArg == "FloatBits32" && instr.srcs.size() == 1) {
                enc.MovssXmm0Load(Disp(instr.srcs[0]));
                enc.Byte(0x66);
                enc.Byte(0x0F);
                enc.Byte(0x7E);
                enc.Byte(0xC0); // movd eax, xmm0
                if (instr.dst != LirNoReg && !instr.type.IsOpaque()) {
                    StoreReturnValue(instr.dst, instr.type);
                }
                break;
            }
            // Built-in: FloatFromBits32 — reinterpret uint32 bits as float32
            if (instr.strArg == "FloatFromBits32" && instr.srcs.size() == 1) {
                LoadA(instr.srcs[0], TypeRef::MakeInt32());
                enc.Byte(0x66);
                enc.Byte(0x0F);
                enc.Byte(0x6E);
                enc.Byte(0xC0); // movd xmm0, eax
                if (instr.dst != LirNoReg && !instr.type.IsOpaque()) {
                    StoreReturnValue(instr.dst, instr.type);
                }
                break;
            }
            bool win64Call = EffectiveConv(instr.callConv) == CallingConvention::Win64;
            const bool hiddenReturn = instr.dst != LirNoReg && (win64Call ? IsWin64ByRefAggregate(instr.type)
                                                                          : IsSysVMemoryAggregate(instr.type));
            // Win64 reserves 32-byte shadow space. SysV reserves only enough
            // space for overflow arguments; both frame sizes preserve the
            // required 16-byte alignment at the call instruction.
            const int callFrameSize = win64Call ? Win64CallFrameSize(instr.srcs.size() + (hiddenReturn ? 1 : 0))
                                                : SysVCallFrameSize(instr.srcs, hiddenReturn ? 1 : 0);
            if (callFrameSize > 0) {
                enc.SubRspImm32(callFrameSize);
            }
            if (hiddenReturn) {
                if (win64Call) {
                    enc.LeaArgStackWin64(0, Disp(instr.dst));
                }
                else {
                    enc.LeaRaxStack(Disp(instr.dst));
                    enc.MovArgRax(0);
                    StoreSysVStackArgs(instr.srcs, 1);
                }
                EmitCallArgs(instr.srcs, instr.callConv, 1);
            }
            else {
                if (!win64Call) {
                    StoreSysVStackArgs(instr.srcs);
                }
                EmitCallArgs(instr.srcs, instr.callConv);
            }
            uint32_t symIdx;
            if (const auto it = funcSyms.find(instr.strArg); it != funcSyms.end()) {
                symIdx = it->second;
            }
            else {
                symIdx = GetOrAddExtern(instr.strArg, RcuSymKind::ExternFunc);
            }
            uint32_t ro;
            enc.Call(ro);
            AddTextReloc(ro, symIdx);
            if (callFrameSize > 0) {
                enc.AddRspImm32(callFrameSize);
            }
            if (instr.dst != LirNoReg && !instr.type.IsOpaque() && !hiddenReturn) {
                StoreReturnValue(instr.dst, instr.type);
            }
            break;
        }
        case LirOpcode::CallIndirect: {
            if (instr.srcs.empty()) {
                break;
            }
            LirReg callee = instr.srcs[0];
            std::vector<LirReg> args(instr.srcs.begin() + 1, instr.srcs.end());
            bool win64Call = EffectiveConv(instr.callConv) == CallingConvention::Win64;
            const bool hiddenReturn = instr.dst != LirNoReg && (win64Call ? IsWin64ByRefAggregate(instr.type)
                                                                          : IsSysVMemoryAggregate(instr.type));
            const int callFrameSize = win64Call ? Win64CallFrameSize(args.size() + (hiddenReturn ? 1 : 0))
                                                : SysVCallFrameSize(args, hiddenReturn ? 1 : 0);
            if (callFrameSize > 0) {
                enc.SubRspImm32(callFrameSize);
            }
            if (hiddenReturn) {
                if (win64Call) {
                    enc.LeaArgStackWin64(0, Disp(instr.dst));
                }
                else {
                    enc.LeaRaxStack(Disp(instr.dst));
                    enc.MovArgRax(0);
                    StoreSysVStackArgs(args, 1);
                }
                EmitCallArgs(args, instr.callConv, 1);
            }
            else {
                if (!win64Call) {
                    StoreSysVStackArgs(args);
                }
                EmitCallArgs(args, instr.callConv);
            }
            auto it = physRegMap.find(callee);
            if (it != physRegMap.end()) {
                enc.MovR10PhysReg(it->second);
            }
            else {
                enc.MovR10Load(Disp(callee));
            }
            enc.CallR10();
            if (callFrameSize > 0) {
                enc.AddRspImm32(callFrameSize);
            }
            if (instr.dst != LirNoReg && !instr.type.IsOpaque() && !hiddenReturn) {
                StoreReturnValue(instr.dst, instr.type);
            }
            break;
        }
        case LirOpcode::GlobalAddr: {
            uint32_t symIdx;
            if (const auto dataIt = dataSyms.find(instr.strArg); dataIt != dataSyms.end()) {
                symIdx = dataIt->second;
            }
            else if (const auto funcIt = funcSyms.find(instr.strArg); funcIt != funcSyms.end()) {
                symIdx = funcIt->second;
            }
            else {
                symIdx = GetOrAddExtern(instr.strArg, RcuSymKind::ExternData);
            }
            uint32_t relocOff;
            enc.LeaRaxRip(relocOff);
            AddTextReloc(relocOff, symIdx);
            StoreA(instr.dst, TypeRef::MakePointer(instr.type));
            break;
        }
        case LirOpcode::StringAddr: {
            const TypeRef elemType = instr.type.inner.empty() ? TypeRef::MakeChar8() : instr.type.inner[0];
            const uint32_t symIdx = InternStr(EncodeStringLiteral(instr.strArg, SizeOfRuntime(elemType)));
            uint32_t relocOff;
            enc.LeaRaxRip(relocOff);
            AddTextReloc(relocOff, symIdx);
            StoreA(instr.dst, instr.type);
            break;
        }
        case LirOpcode::FieldPtr: {
            LirReg base = instr.srcs[0];
            LoadA(base, regTypes.at(base));
            int off = FieldOffset(base, instr.strArg);
            if (off != 0) {
                enc.LeaRaxRaxDisp(off);
            }
            StoreA(instr.dst, TypeRef::MakePointer(instr.type));
            break;
        }
        case LirOpcode::IndexPtr: {
            LirReg base = instr.srcs[0];
            LirReg idx = instr.srcs[1];
            int elemSz = (instr.type.kind == TypeRef::Kind::Pointer && !instr.type.inner.empty())
                           ? SizeOfRuntime(instr.type.inner[0])
                           : 8;
            if (elemSz < 1) {
                elemSz = 1;
            }
            LoadA(base, regTypes.at(base));
            LoadB(idx, regTypes.at(idx));
            enc.ImulR11R10Imm32(elemSz);
            enc.AddRaxR11();
            StoreA(instr.dst, TypeRef::MakePointer(instr.type));
            break;
        }
        case LirOpcode::Phi:
            break; // handled by phi-move pre-emission
        default:
            break;
        }
    }

    // Terminator
    void GenTerm(uint32_t blockIdx, const LirTerminator &term, const LirFunc &func) {
        (void)func;
        switch (term.kind) {
        case LirTermKind::Jump: {
            EmitPhiMoves(blockIdx, term.trueTarget);
            uint32_t po;
            enc.Jmp(po);
            jumpPatches.push_back({po, term.trueTarget});
            break;
        }
        case LirTermKind::Branch: {
            // Load condition with correct width to avoid reading stack
            // garbage
            {
                const TypeRef condT = regTypes.contains(term.cond) ? regTypes.at(term.cond) : TypeRef::MakeBool();
                LoadA(term.cond, condT);
            }
            enc.TestRaxRax();
            const bool truePhi = HasPhiMoves(blockIdx, term.trueTarget);
            if (const bool falsePhi = HasPhiMoves(blockIdx, term.falseTarget); !truePhi && !falsePhi) {
                uint32_t po;
                enc.Jz(po);
                jumpPatches.push_back({po, term.falseTarget});
                uint32_t po2;
                enc.Jmp(po2);
                jumpPatches.push_back({po2, term.trueTarget});
            }
            else {
                uint32_t jzOff;
                enc.Jz(jzOff);
                // true trampoline
                EmitPhiMoves(blockIdx, term.trueTarget);
                uint32_t jmpTrue;
                enc.Jmp(jmpTrue);
                jumpPatches.push_back({jmpTrue, term.trueTarget});
                // patch jz to here (false trampoline)
                auto here = static_cast<int32_t>(enc.Size());
                enc.Patch32(jzOff, here - static_cast<int32_t>(jzOff + 4));
                EmitPhiMoves(blockIdx, term.falseTarget);
                uint32_t jmpFalse;
                enc.Jmp(jmpFalse);
                jumpPatches.push_back({jmpFalse, term.falseTarget});
            }
            break;
        }
        case LirTermKind::Return: {
            if (term.retVal && *term.retVal != LirNoReg) {
                if (hiddenReturnOff != 0) {
                    StoreHiddenReturnValue(*term.retVal, term.retType);
                }
                else {
                    LoadReturnValue(*term.retVal, term.retType);
                }
            }
            if (!usedPhysRegs.empty()) {
                int32_t remainingFrame = frameSize - static_cast<int32_t>(usedPhysRegs.size() * 8);
                if (remainingFrame > 0) {
                    enc.AddRspImm32(remainingFrame);
                }
                for (auto it = usedPhysRegs.rbegin(); it != usedPhysRegs.rend(); ++it) {
                    enc.PopReg(*it);
                }
                enc.PopRbp();
            }
            else {
                enc.Leave();
            }
            enc.Ret();
            break;
        }
        case LirTermKind::Switch: {
            LoadA(term.cond, regTypes.contains(term.cond) ? regTypes.at(term.cond) : TypeRef::MakeInt64());
            for (const auto &c : term.cases) {
                const std::uint64_t bits = ParseIntegerLiteralBits(c.value).value_or(0);
                enc.CmpRaxImm32(static_cast<int32_t>(bits));
                uint32_t po;
                enc.Je(po);
                jumpPatches.push_back({po, c.target});
            }
            EmitPhiMoves(blockIdx, term.defaultTarget);
            uint32_t po;
            enc.Jmp(po);
            jumpPatches.push_back({po, term.defaultTarget});
            break;
        }
        case LirTermKind::Unreachable:
            enc.Ud2();
            break;
        }
    }

    // Resolve a symbol referenced from inline assembly to its symbol-table
    // index: a local text symbol, module data/const, an already-declared
    // extern, or a newly declared extern function.
    uint32_t ResolveAsmSymbol(const std::string &name) {
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
    void GenAsmFunc(const LirFunc &func) {
        const uint32_t funcStart = enc.Size();
        RcuSymbol sym;
        sym.name = func.name;
        sym.sectionIdx = RCU_TEXT_IDX;
        sym.value = funcStart;
        sym.kind = RcuSymKind::Func;
        sym.visibility = func.isPublic ? RcuSymVis::Global : RcuSymVis::Local;
        sym.typeName = func.returnType.ToString();
        uint32_t symIdx;
        if (const auto it = funcSyms.find(func.name); it != funcSyms.end()) {
            symbols[it->second] = std::move(sym);
            symIdx = it->second;
        }
        else {
            symIdx = AddSymbol(std::move(sym));
            funcSyms[func.name] = symIdx;
        }

        AsmAssembly asmResult = AssembleAsmFunc(func.asmBody, mod.name, textData);
        for (const auto &fixup : asmResult.fixups) {
            textRelocs.push_back({fixup.offset, ResolveAsmSymbol(fixup.symbol), fixup.relType, fixup.addend});
        }
        for (auto &diag : asmResult.diagnostics) {
            diagnostics.push_back(std::move(diag));
        }
        symbols[symIdx].size = enc.Size() - funcStart;
    }

    // Function generation
    void GenFunc(const LirFunc &func) {
        if (func.isExtern) {
            GetOrAddExtern(func.name, RcuSymKind::ExternFunc, func.dll);
            return;
        }
        if (func.isAsm) {
            GenAsmFunc(func);
            return;
        }
        PrepassFunc(func);
        jumpPatches.clear();
        uint32_t funcStart = enc.Size();
        // Function symbol
        RcuSymbol sym;
        sym.name = func.name;
        sym.sectionIdx = RCU_TEXT_IDX;
        sym.value = funcStart;
        sym.kind = RcuSymKind::Func;
        sym.visibility = func.isPublic ? RcuSymVis::Global : RcuSymVis::Local;
        sym.typeName = func.returnType.ToString();
        if (const auto it = funcSyms.find(func.name); it != funcSyms.end()) {
            symbols[it->second] = std::move(sym);
        }
        else {
            funcSyms[func.name] = AddSymbol(std::move(sym));
        }
        // Prologue
        enc.PushRbp();
        enc.MovRbpRsp();
        for (int rIdx : usedPhysRegs) {
            enc.PushReg(rIdx);
        }
        int32_t remainingFrame = frameSize - static_cast<int32_t>(usedPhysRegs.size() * 8);
        EmitStackAlloc(remainingFrame);
        // Spill ABI param registers to stack slots
        bool win64Func = EffectiveConv(func.callConv) == CallingConvention::Win64;
        int intIdx = 0, fltIdx = 0, sysvStackIdx = 0, win64Idx = 0;
        if (hiddenReturnOff != 0) {
            if (win64Func) {
                enc.MovArgStoreWin64(0, -hiddenReturnOff);
                win64Idx = 1;
            }
            else {
                enc.MovArgStore(0, -hiddenReturnOff);
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
                else if (IsAggregate(p.type) && sz == 16) {
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
            auto it = physRegMap.find(p.reg);
            if (it != physRegMap.end()) {
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
        blockOffsets.assign(func.blocks.size(), 0);
        for (uint32_t bi = 0; bi < func.blocks.size(); ++bi) {
            blockOffsets[bi] = enc.Size();
            const auto &block = func.blocks[bi];
            for (const auto &instr : block.instrs) {
                GenInstr(instr);
            }
            if (block.term) {
                GenTerm(bi, *block.term, func);
            }
        }
        PatchJumps();
        // Update symbol size
        for (auto &s : symbols) {
            if (s.name == func.name && s.sectionIdx == RCU_TEXT_IDX && s.value == funcStart) {
                s.size = enc.Size() - funcStart;
                break;
            }
        }
    }

    // Module generation
    void EmitVtables() {
        for (const auto &vt : mod.vtables) {
            AlignRodata(8);

            RcuSymbol sym;
            sym.name = vt.label;
            sym.sectionIdx = RCU_RODATA_IDX;
            sym.value = static_cast<uint32_t>(rodataData.size());
            sym.size = static_cast<uint32_t>(vt.methods.size() * 8);
            sym.kind = RcuSymKind::Const;
            sym.visibility = RcuSymVis::Global;
            const uint32_t vtSym = AddSymbol(std::move(sym));
            dataSyms[vt.label] = vtSym;

            for (const auto &method : vt.methods) {
                const uint32_t slotOff = static_cast<uint32_t>(rodataData.size());
                for (int i = 0; i < 8; ++i) {
                    rodataData.push_back(0);
                }

                uint32_t methodSym;
                if (const auto it = funcSyms.find(method); it != funcSyms.end()) {
                    methodSym = it->second;
                }
                else {
                    methodSym = GetOrAddExtern(method, RcuSymKind::ExternFunc);
                }
                AddRodataReloc(slotOff, methodSym, RcuRelType::Abs64);
            }
        }
    }

    // Appends one element of a constant array to .rodata, little-endian.
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
            bits = (literal == "true" || literal == "1") ? 1 : 0;
        }
        else if (literal.starts_with('-')) {
            const std::uint64_t magnitude = ParseIntegerLiteralBits(literal.substr(1)).value_or(0);
            bits = static_cast<std::uint64_t>(-static_cast<std::int64_t>(magnitude));
        }
        else {
            bits = ParseIntegerLiteralBits(literal).value_or(0);
        }
        for (int i = 0; i < size; ++i) {
            rodataData.push_back(bits & 0xFF);
            bits >>= 8;
        }
    }

    // A slice constant becomes two read-only symbols: its elements, and a
    // {data, length} header under the constant's own name whose data field is
    // relocated to point at them. Code then reaches the elements the same way
    // it reaches those of any other slice.
    void EmitConstSlice(const LirConstDecl &c) {
        const int elemSize = std::max(SizeOf(c.elementType), 1);
        const uint32_t elemsOff = AlignRodata(std::min(elemSize, 8));
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

        RcuSymbol elems;
        elems.name = c.name + "$elements";
        elems.sectionIdx = RCU_RODATA_IDX;
        elems.value = elemsOff;
        elems.size = static_cast<uint32_t>(rodataData.size() - elemsOff);
        elems.kind = RcuSymKind::Const;
        elems.visibility = RcuSymVis::Local;
        const uint32_t elemsSym = AddSymbol(std::move(elems));

        const uint32_t headerOff = AlignRodata(8);
        for (int i = 0; i < 16; ++i) {
            rodataData.push_back(0);
        }
        AddRodataReloc(headerOff, elemsSym, RcuRelType::Abs64);
        for (int i = 0; i < 8; ++i) {
            rodataData[headerOff + 8 + i] = static_cast<std::uint8_t>((length >> (8 * i)) & 0xFF);
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

    void GenModule() {
        BuildLayouts();
        PredeclareFunctions();
        // Extern vars
        for (const auto &ev : mod.externVars) {
            GetOrAddExtern(ev.name, RcuSymKind::ExternData);
        }
        // Module constants → .data symbols
        for (const auto &c : mod.consts) {
            // A constant of slice type is addressed, not inlined, so it needs
            // real contents behind its name rather than a placeholder.
            if (c.isTextSlice || !c.elements.empty()) {
                EmitConstSlice(c);
                continue;
            }
            RcuSymbol s;
            s.name = c.name;
            s.sectionIdx = RCU_DATA_IDX;
            s.value = static_cast<uint32_t>(dataData.size());
            s.kind = RcuSymKind::Const;
            s.visibility = c.isPublic ? RcuSymVis::Global : RcuSymVis::Local;
            s.typeName = c.type.ToString();
            // Emit 8 placeholder bytes in .data
            for (int i = 0; i < 8; ++i) {
                dataData.push_back(0);
            }
            s.size = 8;
            dataSyms[c.name] = AddSymbol(s);
        }
        EmitVtables();
        // Functions
        for (const auto &func : mod.funcs) {
            GenFunc(func);
        }
        // Runtime helpers referenced by the generated code, emitted
        // once. The f32 pow helper calls the f64 one, so emit f64 first.
        EmitIntPowHelper();
        EmitFloatPowHelper();
        EmitFloatPowF32Helper();
    }
};

// RcuCodeGen::Generate
RcuFile RcuCodeGen::Generate() {
    GenModule();
    RcuFile file;
    file.sourcePath = mod.name;
    file.packageName = pkgName;
    file.buildTimestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    // Parse rux version from RUX_VERSION string "M.m.p"
    {
        std::string ver = RUX_VERSION;
        unsigned M = 0, mi = 0, p = 0;
        auto parseNum = [](const char *s, unsigned &out) -> const char * {
            while (*s && (*s < '0' || *s > '9')) {
                ++s;
            }
            while (*s >= '0' && *s <= '9') {
                out = out * 10 + static_cast<unsigned>(*s - '0');
                ++s;
            }
            return s;
        };
        const char *c1 = parseNum(ver.c_str(), M);
        const char *c2 = parseNum(c1, mi);
        parseNum(c2, p);
        file.ruxVersion = (M << 16) | (mi << 8) | p;
    }

    // Build sections (always 3: .text, .rodata, .data)
    {
        RcuSection text;
        text.name = ".text";
        text.type = RcuSecType::Text;
        text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
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
    // Build string table offsets (intern all names into the file's
    // string table) (done during Emit/Dump)
    file.flags = 0x01; // F_HAS_METADATA
    file.hasMetadata = true;
    return file;
}
} // namespace

RcuFile GenerateRcuModule(const LirModule &mod, const std::vector<LirStructDecl> &structDecls,
                          const std::vector<std::string> &interfaceNames, const std::string &packageName,
                          const Target::OS targetOs, std::vector<Diagnostic> &diagnostics) {
    RcuCodeGen gen(mod, structDecls, interfaceNames, packageName, targetOs, diagnostics);
    return gen.Generate();
}

RcuEmitter::RcuEmitter(const LirPackage &package, std::string inputPackageName, const Target::OS inputTargetOs)
    : lir(package)
    , packageName(std::move(inputPackageName))
    , targetOs(inputTargetOs) {
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
        result.push_back(GenerateRcuModule(module, structDecls, interfaceNames, packageName, targetOs, diagnostics));
    }
    return result;
}
} // namespace Rux
