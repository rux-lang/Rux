// RCU code generation: lowers a LirModule to an in-memory RcuFile.

#include "CodeGen/X86_64/RcuEmitter.h"

#include "Driver/Version.h"

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

#include "CodeGen/Layout.h"
#include "CodeGen/X86_64/Assembler.h"
#include "CodeGen/X86_64/Encoder.h"
#include "Object/Rcu/RcuStringTable.h"

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
    if (c != CallingConvention::Default) {
        return c;
    }
    return CallingConvention::Win64;
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
                        std::vector<Diagnostic> &inputDiagnostics)
        : mod(module)
        , structDecls(inputStructDecls)
        , packageInterfaceNames(inputPackageInterfaceNames)
        , pkgName(std::move(packageName))
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

    // Struct field layouts
    LayoutMap layouts;
    std::unordered_set<std::string> interfaceNames;

    // Per-function state
    struct PhiMove {
        LirReg dst;
        LirReg src;
        TypeRef type;
    };

    std::unordered_map<LirReg, int32_t> slotMap;
    std::unordered_map<LirReg, int32_t> allocaData;
    std::unordered_map<LirReg, TypeRef> regTypes;
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, std::vector<PhiMove>>> phiMoves;
    int32_t nextOff = 0;
    int32_t frameSize = 0;
    int32_t hiddenReturnOff = 0;

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
            auto it = layouts.find(base);
            if (it != layouts.end()) {
                return it->second.totalSize;
            }
        }
        return SizeOf(t);
    }

    [[nodiscard]] int StackValueSize(const TypeRef &t) const {
        return std::max(SizeOf(t), SizeOfRuntime(t));
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

    [[nodiscard]] bool IsWin64AddressParam(const TypeRef &t) const {
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
        float fv = std::stof(val);
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
        double dv = std::stod(val);
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
        const int sz = SizeOf(t);
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
        int sz = SizeOf(t);
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

    void StoreA(LirReg dst, const TypeRef &t) const {
        int sz = SizeOf(t);
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

    void LoadReturnValue(const LirReg reg, const TypeRef &t) const {
        if (SizeOfRuntime(t) == 16) {
            if (IsRegPointerTo(reg, t)) {
                enc.MovR10Load(Disp(reg));
                enc.Byte(0x49);
                enc.Byte(0x8B);
                enc.Byte(0x02); // mov rax, [r10]
                enc.Byte(0x49);
                enc.Byte(0x8B);
                enc.Byte(0x52);
                enc.Byte(0x08); // mov rdx, [r10 + 8]
            }
            else {
                enc.MovRaxLoad(Disp(reg));
                enc.MovR10Load(Disp(reg) + 8);
                enc.Byte(0x4C);
                enc.Byte(0x89);
                enc.Byte(0xD2); // mov rdx, r10
            }
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
        if (hiddenReturnOff == 0 || !IsWin64ByRefAggregate(t)) {
            LoadReturnValue(src, t);
            return;
        }
        enc.MovR11Load(-hiddenReturnOff);
        if (IsRegPointerTo(src, t)) {
            enc.MovR10Load(Disp(src));
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
        nextOff = 0;
        frameSize = 0;
        hiddenReturnOff = 0;
        slotMap.clear();
        allocaData.clear();
        regTypes.clear();
        phiMoves.clear();
        if (EffectiveConv(func.callConv) == CallingConvention::Win64 && IsWin64ByRefAggregate(func.returnType)) {
            hiddenReturnOff = AllocRegion(8);
        }
        for (const auto &p : func.params) {
            int sz = IsWin64AddressParam(p.type) ? 8 : SizeOfRuntime(p.type);
            AllocSlot(p.reg, sz > 0 ? sz : 8);
            regTypes[p.reg] = IsWin64AddressParam(p.type) ? TypeRef::MakePointer(p.type) : p.type;
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
                    AllocSlot(instr.dst, 8);
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
                    allocaData[instr.dst] = AllocRegion(dsz > 0 ? dsz : 8);
                    regTypes[instr.dst] = TypeRef::MakePointer(instr.type);
                }
                else {
                    int sz = StackValueSize(instr.type);
                    AllocSlot(instr.dst, sz > 0 ? sz : 8);
                    regTypes[instr.dst] = instr.type;
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
        for (const auto &m : it2->second) {
            if (!slotMap.contains(m.src)) {
                continue;
            }
            LoadA(m.src, m.type);
            StoreA(m.dst, m.type);
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
                    enc.MovArgLoadWin64(idx, d);
                }
                else {
                    LoadA(arg, at);
                    enc.MovArgWin64Rax(idx);
                }
                ++idx;
            }
        }
        else {
            int intIdx = 0, fltIdx = 0;
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
                else {
                    if (intIdx < 6) {
                        enc.MovArgLoad(intIdx, d);
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
                enc.MovRaxStore(Disp(instr.dst));
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
            enc.MovRaxStore(Disp(instr.dst));
            break;
        }
        case LirOpcode::Load: {
            const TypeRef &t = instr.type;
            int sz = SizeOf(t);
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
                enc.MovR10Load(Disp(ptr));
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
            int sz = SizeOf(t);
            int runtimeSz = SizeOfRuntime(t);
            enc.MovR11Load(Disp(ptr));
            if (IsAggregate(t) && runtimeSz > 8) {
                if (IsRegPointerTo(val, t)) {
                    enc.MovR10Load(Disp(val));
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
                uint32_t sym = GetOrAddExtern("pow", RcuSymKind::ExternFunc);
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
            enc.MovR11Load(Disp(instr.srcs[1]));
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
                enc.MovRaxLoad(Disp(instr.srcs[0]));
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
                enc.MovRaxLoad(Disp(instr.srcs[0]));
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
            const bool hiddenReturn = win64Call && instr.dst != LirNoReg && IsWin64ByRefAggregate(instr.type);
            // Win64 reserves 32-byte shadow space plus any stack args. System V
            // needs no shadow space, but Rux enters every function with rsp
            // 16-byte aligned (its body invariant is rsp % 16 == 8), whereas
            // the SysV ABI requires rsp % 16 == 0 at the call site; the 8-byte
            // pad restores that so libc's aligned SSE prologues do not fault.
            const int callFrameSize = win64Call ? Win64CallFrameSize(instr.srcs.size() + (hiddenReturn ? 1 : 0)) : 8;
            enc.SubRspImm32(callFrameSize);
            if (hiddenReturn) {
                enc.LeaArgStackWin64(0, Disp(instr.dst));
                EmitCallArgs(instr.srcs, instr.callConv, 1);
            }
            else {
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
            enc.AddRspImm32(callFrameSize);
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
            const bool hiddenReturn = win64Call && instr.dst != LirNoReg && IsWin64ByRefAggregate(instr.type);
            const int callFrameSize = win64Call ? Win64CallFrameSize(args.size() + (hiddenReturn ? 1 : 0)) : 0;
            if (win64Call) {
                enc.SubRspImm32(callFrameSize);
            }
            if (hiddenReturn) {
                enc.LeaArgStackWin64(0, Disp(instr.dst));
                EmitCallArgs(args, instr.callConv, 1);
            }
            else {
                EmitCallArgs(args, instr.callConv);
            }
            enc.MovR10Load(Disp(callee));
            enc.CallR10();
            if (win64Call) {
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
            enc.MovRaxStore(Disp(instr.dst));
            break;
        }
        case LirOpcode::FieldPtr: {
            LirReg base = instr.srcs[0];
            enc.MovRaxLoad(Disp(base));
            int off = FieldOffset(base, instr.strArg);
            if (off != 0) {
                enc.LeaRaxRaxDisp(off);
            }
            enc.MovRaxStore(Disp(instr.dst));
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
            enc.MovRaxLoad(Disp(base));
            LoadB(idx, regTypes.at(idx));
            enc.ImulR11R10Imm32(elemSz);
            enc.AddRaxR11();
            enc.MovRaxStore(Disp(instr.dst));
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
                const int condSz = SizeOf(condT);
                if (condSz <= 1) {
                    enc.MovzxRaxByte(Disp(term.cond));
                }
                else if (condSz == 2) {
                    enc.MovzxRaxWord(Disp(term.cond));
                }
                else if (condSz == 4) {
                    enc.MovEaxLoad(Disp(term.cond));
                }
                else {
                    enc.MovRaxLoad(Disp(term.cond));
                }
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
                if (hiddenReturnOff != 0 && IsWin64ByRefAggregate(term.retType)) {
                    StoreHiddenReturnValue(*term.retVal, term.retType);
                }
                else {
                    LoadReturnValue(*term.retVal, term.retType);
                }
            }
            enc.Leave();
            enc.Ret();
            break;
        }
        case LirTermKind::Switch: {
            enc.MovRaxLoad(Disp(term.cond));
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
        EmitStackAlloc(frameSize);
        // Spill ABI param registers to stack slots
        bool win64Func = EffectiveConv(func.callConv) == CallingConvention::Win64;
        int intIdx = 0, fltIdx = 0, win64Idx = 0;
        if (win64Func && hiddenReturnOff != 0) {
            enc.MovArgStoreWin64(0, -hiddenReturnOff);
            win64Idx = 1;
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
                        StoreA(p.reg, p.type);
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
                    StoreA(p.reg, p.type);
                }
                ++win64Idx;
            }
            else {
                if (IsFloat(p.type)) {
                    if (fltIdx < 8) {
                        // MOVSS/MOVSD [rbp+d], xmmN
                        enc.Byte(sz == 4 ? 0xF3 : 0xF2);
                        enc.Byte(0x0F);
                        enc.Byte(0x11);
                        enc.Byte(static_cast<uint8_t>(0x80 | (fltIdx << 3) | 5));
                        enc.Dword(static_cast<uint32_t>(d));
                        ++fltIdx;
                    }
                }
                else {
                    if (intIdx < 6) {
                        enc.MovArgStore(intIdx, d);
                        ++intIdx;
                    }
                }
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

    void GenModule() {
        BuildLayouts();
        PredeclareFunctions();
        // Extern vars
        for (const auto &ev : mod.externVars) {
            GetOrAddExtern(ev.name, RcuSymKind::ExternData);
        }
        // Module constants → .data symbols
        for (const auto &c : mod.consts) {
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
            AddSymbol(s);
        }
        EmitVtables();
        // Functions
        for (const auto &func : mod.funcs) {
            GenFunc(func);
        }
        // Runtime helpers referenced by the generated code, emitted
        // once.
        EmitIntPowHelper();
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
                          std::vector<Diagnostic> &diagnostics) {
    RcuCodeGen gen(mod, structDecls, interfaceNames, packageName, diagnostics);
    return gen.Generate();
}

RcuEmitter::RcuEmitter(const LirPackage &package, std::string inputPackageName)
    : lir(package)
    , packageName(std::move(inputPackageName)) {
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
        result.push_back(GenerateRcuModule(module, structDecls, interfaceNames, packageName, diagnostics));
    }
    return result;
}

} // namespace Rux
