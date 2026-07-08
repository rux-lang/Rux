#include "CodeGen/X86_64/AssemblyPrinter.h"

#include <cstring>
#include <format>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "CodeGen/Layout.h"
#include "CodeGen/PhiMoveResolver.h"
#include "Target/Platform.h"

namespace Rux {

using namespace Layout;

// Type utilities
namespace {
// x86-64 register names sized for the rax family
std::string_view GprA(int bytes) {
    switch (bytes) {
    case 1:
        return "al";
    case 2:
        return "ax";
    case 4:
        return "eax";
    default:
        return "rax";
    }
}

// r10 family (caller-saved scratch — primary operand)
[[maybe_unused]] std::string_view GprB(const int bytes) {
    switch (bytes) {
    case 1:
        return "r10b";
    case 2:
        return "r10w";
    case 4:
        return "r10d";
    default:
        return "r10";
    }
}

// r11 family (caller-saved scratch — secondary operand)
[[maybe_unused]] std::string_view GprC(const int bytes) {
    switch (bytes) {
    case 1:
        return "r11b";
    case 2:
        return "r11w";
    case 4:
        return "r11d";
    default:
        return "r11";
    }
}

std::string_view PtrSize(const int bytes) {
    switch (bytes) {
    case 1:
        return "byte";
    case 2:
        return "word";
    case 4:
        return "dword";
    default:
        return "qword";
    }
}

// The --dump-asm text output reflects the host ABI's default calling
// convention. (The RCU object path always emits Win64 and relies on the
// Unix linker's compat thunks to translate; this constant only affects the
// human-readable dump.)
constexpr bool kDefaultCallIsWin64 = RUX_OS_WINDOWS;

// Code generator
class AsmGen {
public:
    explicit AsmGen(const LirPackage &package)
        : pkg(package) {
    }

    std::string Generate();

private:
    const LirPackage &pkg;

    // Separate output streams assembled at the end
    std::ostringstream text;    // .text section
    std::ostringstream data;    // .data section (writable globals)
    std::ostringstream rodata;  // .rodata section (string/fp constants)
    std::ostringstream externs; // extern declarations
    std::ostringstream globals; // global declarations

    // Interned read-only constants
    std::unordered_map<std::string, std::string> strLabels;
    std::unordered_map<std::string, std::string> f32Labels;
    std::unordered_map<std::string, std::string> f64Labels;
    int constIdx = 0;

    // Struct field layouts (built once)
    LayoutMap layouts;
    std::unordered_set<std::string> interfaceNames;

    std::string curFunc;
    std::unordered_map<LirReg, int32_t> slotMap;    // vreg → rbp offset (positive, address = rbp - offset)
    std::unordered_map<LirReg, int32_t> allocaData; // alloca vreg → data region rbp offset
    std::unordered_map<LirReg, TypeRef> regTypes;   // vreg → value type (pointer for alloca)
    int32_t nextOff = 0;
    int32_t frameSize = 0;

    // phiMoves_[fromBlock][toBlock] = list of (dst, src, type)
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, std::vector<PhiMove>>> phiMoves;
    std::unordered_map<LirReg, int> physRegMap;
    std::vector<int> usedPhysRegs;

    std::unordered_set<std::string> declaredExterns;

    // Set when the unit uses the integer ** operator; emits __rux_ipow.
    bool usesIpow = false;

    // Set when the unit uses the float ** operator; emit the synthesized
    // pow helpers. The f32 helper wraps the f64 one.
    bool usesFpow = false;
    bool usesFpowF32 = false;

    // Low-level emit helpers
    void T(const std::string_view s) {
        text << s << '\n';
    }

    void TI(const std::string_view s) {
        text << "    " << s << '\n';
    }

    void TL(const std::string_view label) {
        text << label << ":\n";
    }

    void TC(const std::string_view comment) {
        text << "    ; " << comment << '\n';
    }

    void TB() {
        text << '\n';
    }

    // Constant interning
    std::string InternStr(const std::string &val) {
        if (const auto it = strLabels.find(val); it != strLabels.end()) {
            return it->second;
        }
        std::string lbl = std::format("__str{}", constIdx++);
        strLabels[val] = lbl;
        // Emit as NUL-terminated bytes in .rodata
        rodata << lbl << ":\n    db    ";
        for (const unsigned char c : val) {
            rodata << static_cast<int>(c) << ", ";
        }
        rodata << "0\n";
        return lbl;
    }

    std::string InternF32(const std::string &val) {
        auto it = f32Labels.find(val);
        if (it != f32Labels.end()) {
            return it->second;
        }
        std::string lbl = std::format("__f32_{}", constIdx++);
        f32Labels[val] = lbl;
        std::uint32_t bits;
        if (val.starts_with("0x")) {
            bits = static_cast<std::uint32_t>(std::stoull(val, nullptr, 16));
        }
        else {
            float fv = std::stof(val);
            std::memcpy(&bits, &fv, sizeof(bits));
        }
        rodata << lbl << ":\n    dd    0x" << std::hex << bits << std::dec << "\n";
        return lbl;
    }

    std::string InternF64(const std::string &val) {
        auto it = f64Labels.find(val);
        if (it != f64Labels.end()) {
            return it->second;
        }
        std::string lbl = std::format("__f64_{}", constIdx++);
        f64Labels[val] = lbl;
        std::uint64_t bits;
        if (val.starts_with("0x")) {
            bits = std::stoull(val, nullptr, 16);
        }
        else {
            double dv = std::stod(val);
            std::memcpy(&bits, &dv, sizeof(bits));
        }
        rodata << lbl << ":\n    dq    0x" << std::hex << bits << std::dec << "\n";
        return lbl;
    }

    void NeedExtern(const std::string &name) {
        if (declaredExterns.insert(name).second) {
            externs << "extern " << name << "\n";
        }
    }

    // Integer exponentiation helper: rax = rdi ** rsi (signed
    // exponent). Emitted once when the unit uses the integer **
    // operator so the output is self-contained (no libm/CRT
    // dependency). Mirrors the machine-code helper synthesized by the
    // RCU backend.
    void EmitIntPowHelper() {
        TB();
        TL("__rux_ipow");
        TI("test    rdx, rdx");  // exponent
        TI("js      .negative"); // negative exponent yields 0
        TI("mov     eax, 1");    // result = 1
        TL(".loop");
        TI("test    rdx, rdx");
        TI("jz      .done"); // exponent == 0
        TI("test    rdx, 1");
        TI("jz      .square");
        TI("imul    rax, rcx"); // result *= base
        TL(".square");
        TI("imul    rcx, rcx"); // base *= base
        TI("sar     rdx, 1");   // exponent >>= 1
        TI("jmp     .loop");
        TL(".negative");
        TI("xor     eax, eax");
        TL(".done");
        TI("ret");
    }

    // Double-precision exponentiation helper: xmm0 = xmm0 ** xmm1
    // (base ** exponent). Computed as |base|**exp = 2**(exp*log2(|base|))
    // on the x87 FPU, so the output stays self-contained (no libm/CRT
    // dependency). Mirrors the machine-code helper synthesized by the RCU
    // backend; see EmitFloatPowHelper there for the full rationale.
    void EmitFloatPowHelper() {
        TB();
        TL("__rux_powf64");
        TI("sub     rsp, 16");
        TI("movsd   [rsp], xmm0");     // base
        TI("movsd   [rsp + 8], xmm1"); // exponent
        // exp == 0 -> 1.0
        TI("mov     rax, [rsp + 8]");
        TI("add     rax, rax"); // drop sign bit
        TI("jnz     .not_exp0");
        TI("mov     rax, 0x3FF0000000000000");
        TI("mov     [rsp], rax");
        TI("movsd   xmm0, [rsp]");
        TI("add     rsp, 16");
        TI("ret");
        TL(".not_exp0");
        // |base| == 0 -> 0.0 (exp>0) or +inf (exp<0)
        TI("mov     rax, [rsp]");
        TI("add     rax, rax");
        TI("jnz     .base_nonzero");
        TI("mov     rax, [rsp + 8]");
        TI("test    rax, rax");
        TI("js      .base0_neg");
        TI("xor     eax, eax");
        TI("mov     [rsp], rax");
        TI("movsd   xmm0, [rsp]");
        TI("add     rsp, 16");
        TI("ret");
        TL(".base0_neg");
        TI("mov     rax, 0x7FF0000000000000");
        TI("mov     [rsp], rax");
        TI("movsd   xmm0, [rsp]");
        TI("add     rsp, 16");
        TI("ret");
        TL(".base_nonzero");
        // Sign decision -> edx: 0 keep, 1 negate, 2 NaN.
        TI("xor     edx, edx");
        TI("mov     rax, [rsp]");
        TI("test    rax, rax");
        TI("jns     .magnitude"); // base > 0
        TI("movsd   xmm2, [rsp + 8]");
        TI("cvttsd2si rax, xmm2");
        TI("cvtsi2sd xmm3, rax");
        TI("ucomisd xmm2, xmm3");
        TI("jne     .nonint"); // non-integer exponent -> NaN
        TI("and     eax, 1");
        TI("mov     edx, eax"); // 1 if odd
        TI("jmp     .magnitude");
        TL(".nonint");
        TI("mov     edx, 2");
        TL(".magnitude");
        TI("fld     qword [rsp + 8]"); // exp
        TI("fld     qword [rsp]");     // base
        TI("fabs");                    // |base|
        TI("fyl2x");                   // w = exp*log2(|base|)
        TI("fld     st0");
        TI("frndint");          // i = round(w)
        TI("fsub    st1, st0"); // frac = w - i in [-1,1]
        TI("fxch");
        TI("f2xm1"); // 2^frac - 1
        TI("fld1");
        TI("faddp   st1, st0"); // 2^frac
        TI("fscale");           // 2^frac * 2^i
        TI("fstp    st1");      // drop i -> st0 = magnitude
        TI("test    edx, edx");
        TI("jz      .store");
        TI("cmp     edx, 2");
        TI("jz      .nan");
        TI("fchs"); // negate (odd exponent)
        TL(".store");
        TI("fstp    qword [rsp]");
        TI("movsd   xmm0, [rsp]");
        TI("add     rsp, 16");
        TI("ret");
        TL(".nan");
        TI("fstp    st0"); // drop magnitude
        TI("mov     rax, 0x7FF8000000000000");
        TI("mov     [rsp], rax");
        TI("movsd   xmm0, [rsp]");
        TI("add     rsp, 16");
        TI("ret");
    }

    // Single-precision exponentiation helper: xmm0 = xmm0 ** xmm1. Widens
    // the arguments to f64, defers to __rux_powf64, then narrows the result.
    void EmitFloatPowF32Helper() {
        TB();
        TL("__rux_powf32");
        TI("cvtss2sd xmm0, xmm0");
        TI("cvtss2sd xmm1, xmm1");
        TI("sub     rsp, 8"); // keep 16-byte alignment at the inner call
        TI("call    __rux_powf64");
        TI("add     rsp, 8");
        TI("cvtsd2ss xmm0, xmm0");
        TI("ret");
    }

    static std::string BaseTypeName(const std::string &name) {
        const std::size_t pos = name.find('<');
        return pos == std::string::npos ? name : name.substr(0, pos);
    }

    [[nodiscard]] int SizeOfRuntime(const TypeRef &t) const {
        if (t.kind == TypeRef::Kind::Named) {
            const std::string base = BaseTypeName(t.name);
            if (interfaceNames.contains(base)) {
                return 16; // {data: *opaque, vtable: *opaque}
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
            if (const auto it = layouts.find(base); it != layouts.end()) {
                return it->second.totalSize;
            }
        }
        return SizeOf(t);
    }

    [[nodiscard]] bool IsAggregate(const TypeRef &t) const {
        switch (t.kind) {
        case TypeRef::Kind::Tuple:
        case TypeRef::Kind::Slice:
        case TypeRef::Kind::Range:
            return true;
        case TypeRef::Kind::Named: {
            const std::string base = BaseTypeName(t.name);
            return base == "Slice" || interfaceNames.contains(base) || layouts.contains(base);
        }
        default:
            return false;
        }
    }

    [[nodiscard]] bool IsWin64AddressParam(const TypeRef &t) const {
        if (t.kind == TypeRef::Kind::Slice) {
            return true;
        }
        if (t.kind != TypeRef::Kind::Named) {
            return false;
        }
        const std::string base = BaseTypeName(t.name);
        return base == "Slice" || interfaceNames.contains(base);
    }

    [[nodiscard]] int StackValueSize(const TypeRef &t) const {
        return SizeOfRuntime(t);
    }

    [[nodiscard]] bool IsRegPointerTo(const LirReg reg, const TypeRef &pointee) const {
        const auto it = regTypes.find(reg);
        return it != regTypes.end() && it->second.kind == TypeRef::Kind::Pointer && !it->second.inner.empty() &&
               it->second.inner[0] == pointee;
    }

    // Stack slot allocation
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

    std::string_view PhysRegName(int rIdx) const {
        switch (rIdx) {
        case 0:
            return "rbx";
        case 1:
            return "r12";
        case 2:
            return "r13";
        case 3:
            return "r14";
        case 4:
            return "r15";
        default:
            return "rbx";
        }
    }

    // Load/store helpers
    // Integer loads promote to 64-bit (sign- or zero-extend as needed).
    // Float loads use xmm0 (primary) or xmm1 (secondary).
    // Load vreg into rax (integer) or xmm0 (float)
    void LoadA(LirReg reg, const TypeRef &t) {
        auto it = physRegMap.find(reg);
        if (it != physRegMap.end()) {
            TI(std::format("{:<8}rax, {}", "mov", PhysRegName(it->second)));
            int sz = SizeOfRuntime(t);
            if (sz > 0 && sz < 8) {
                if (t.IsSigned()) {
                    if (sz == 4)
                        TI("movsxd  rax, eax");
                    else if (sz == 2)
                        TI("movsx   rax, ax");
                    else
                        TI("movsx   rax, al");
                }
                else {
                    if (sz == 4)
                        TI("mov     eax, eax");
                    else if (sz == 2)
                        TI("movzx   rax, ax");
                    else
                        TI("movzx   rax, al");
                }
            }
            return;
        }
        int sz = SizeOfRuntime(t);
        int runtimeSz = SizeOfRuntime(t);
        int off = slotMap.at(reg);
        if (runtimeSz == 16) {
            TI(std::format("{:<8}rax, qword [rbp - {}]", "mov", off));
            TI(std::format("{:<8}rdx, qword [rbp - {}]", "mov", off - 8));
        }
        else if (IsFloat(t)) {
            TI(std::format("{:<8}xmm0, {} [rbp - {}]", sz == 4 ? "movss" : "movsd", PtrSize(sz), off));
        }
        else if (sz == 8 || sz == 0) {
            TI(std::format("{:<8}rax, qword [rbp - {}]", "mov", off));
        }
        else if (t.IsSigned()) {
            TI(std::format("{:<8}rax, {} [rbp - {}]", sz == 4 ? "movsxd" : "movsx", PtrSize(sz), off));
        }
        else {
            if (sz == 4) {
                TI(std::format("{:<8}eax, dword [rbp - {}]", "mov", off));
            }
            else {
                TI(std::format("{:<8}rax, {} [rbp - {}]", "movzx", PtrSize(sz), off));
            }
        }
    }

    // Load vreg into r10 (integer) or xmm1 (float)
    void LoadB(LirReg reg, const TypeRef &t) {
        auto it = physRegMap.find(reg);
        if (it != physRegMap.end()) {
            TI(std::format("{:<8}r10, {}", "mov", PhysRegName(it->second)));
            int sz = SizeOfRuntime(t);
            if (sz > 0 && sz < 8) {
                if (t.IsSigned()) {
                    if (sz == 4)
                        TI("movsxd  r10, r10d");
                    else if (sz == 2)
                        TI("movsx   r10, r10w");
                    else
                        TI("movsx   r10, r10b");
                }
                else {
                    if (sz == 4)
                        TI("mov     r10d, r10d");
                    else if (sz == 2)
                        TI("movzx   r10, r10w");
                    else
                        TI("movzx   r10, r10b");
                }
            }
            return;
        }
        int sz = SizeOfRuntime(t);
        int32_t off = slotMap.at(reg);
        if (IsFloat(t)) {
            TI(std::format("{:<8}xmm1, {} [rbp - {}]", sz == 4 ? "movss" : "movsd", PtrSize(sz), off));
        }
        else if (sz == 8 || sz == 0) {
            TI(std::format("{:<8}r10, qword [rbp - {}]", "mov", off));
        }
        else if (t.IsSigned()) {
            TI(std::format("{:<8}r10, {} [rbp - {}]", sz == 4 ? "movsxd" : "movsx", PtrSize(sz), off));
        }
        else {
            if (sz == 4) {
                TI(std::format("{:<8}r10d, dword [rbp - {}]", "mov", off));
            }
            else {
                TI(std::format("{:<8}r10, {} [rbp - {}]", "movzx", PtrSize(sz), off));
            }
        }
    }

    // Store rax (integer) or xmm0 (float) into dst's slot
    void StoreA(LirReg dst, const TypeRef &t) {
        auto it = physRegMap.find(dst);
        if (it != physRegMap.end()) {
            TI(std::format("{:<8}{}, rax", "mov", PhysRegName(it->second)));
            return;
        }
        int sz = SizeOfRuntime(t);
        int runtimeSz = SizeOfRuntime(t);
        int off = slotMap.at(dst);
        if (runtimeSz == 16) {
            TI(std::format("{:<8}qword [rbp - {}], rax", "mov", off));
            TI(std::format("{:<8}qword [rbp - {}], rdx", "mov", off - 8));
        }
        else if (IsFloat(t)) {
            TI(std::format("{:<8}{} [rbp - {}], xmm0", sz == 4 ? "movss" : "movsd", PtrSize(sz), off));
        }
        else {
            int store_sz = (sz > 0) ? sz : 8;
            TI(std::format("{:<8}{} [rbp - {}], {}", "mov", PtrSize(store_sz), off, GprA(store_sz)));
        }
    }

    void LoadReturnValue(LirReg reg, const TypeRef &t) {
        if (SizeOfRuntime(t) == 16 && IsRegPointerTo(reg, t)) {
            auto it = physRegMap.find(reg);
            if (it != physRegMap.end()) {
                TI(std::format("{:<8}r10, {}", "mov", PhysRegName(it->second)));
            }
            else {
                TI(std::format("{:<8}r10, qword [rbp - {}]", "mov", slotMap.at(reg)));
            }
            TI(std::format("{:<8}rax, qword [r10]", "mov"));
            TI(std::format("{:<8}rdx, qword [r10 + 8]", "mov"));
            return;
        }
        LoadA(reg, t);
    }

    // Block labels
    [[nodiscard]] std::string BlockLabel(uint32_t idx, const std::string &label) const {
        if (idx == 0) {
            return curFunc;
        }
        return "." + curFunc + "_" + label;
    }

    // Phi move emission
    void EmitPhiMoves(uint32_t fromBlock, uint32_t toBlock) {
        auto it1 = phiMoves.find(fromBlock);
        if (it1 == phiMoves.end()) {
            return;
        }
        auto it2 = it1->second.find(toBlock);
        if (it2 == it1->second.end()) {
            return;
        }

        std::vector<Rux::PhiMove> rawMoves;
        rawMoves.reserve(it2->second.size());
        for (const auto &m : it2->second) {
            rawMoves.push_back({m.dst, m.src, m.type});
        }

        std::vector<PhiMoveStep> steps = ResolvePhiMoves(std::move(rawMoves));
        int32_t tempOff = frameSize + 8;

        for (const auto &step : steps) {
            if (step.kind == PhiMoveStep::Kind::SaveDestination) {
                int sz = SizeOfRuntime(step.type);
                if (sz == 16) {
                    LoadA(step.dst, step.type);
                    TI(std::format("{:<8}qword [rbp - {}], rax", "mov", tempOff));
                    TI(std::format("{:<8}qword [rbp - {}], rdx", "mov", tempOff - 8));
                }
                else if (IsFloat(step.type)) {
                    LoadA(step.dst, step.type);
                    TI(std::format("{:<8}{} [rbp - {}], xmm0", sz == 4 ? "movss" : "movsd", PtrSize(sz), tempOff));
                }
                else {
                    LoadA(step.dst, step.type);
                    TI(std::format("{:<8}qword [rbp - {}], rax", "mov", tempOff));
                }
            }
            else {
                if (step.sourceIsTemporary) {
                    int sz = SizeOfRuntime(step.type);
                    if (sz == 16) {
                        TI(std::format("{:<8}rax, qword [rbp - {}]", "mov", tempOff));
                        TI(std::format("{:<8}rdx, qword [rbp - {}]", "mov", tempOff - 8));
                        StoreA(step.dst, step.type);
                    }
                    else if (IsFloat(step.type)) {
                        TI(std::format("{:<8}xmm0, {} [rbp - {}]", sz == 4 ? "movss" : "movsd", PtrSize(sz), tempOff));
                        StoreA(step.dst, step.type);
                    }
                    else {
                        TI(std::format("{:<8}rax, qword [rbp - {}]", "mov", tempOff));
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

    // Pre-pass
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

        for (const auto &p : func.params) {
            regTypes[p.reg] = IsWin64AddressParam(p.type) ? TypeRef::MakePointer(p.type) : p.type;
            AllocSlot(p.reg, std::max(8, SizeOfRuntime(regTypes[p.reg])));
        }

        for (uint32_t bi = 0; bi < func.blocks.size(); bi++) {
            const auto &block = func.blocks[bi];
            for (const auto &instr : block.instrs) {
                if (instr.op == LirOpcode::Phi) {
                    for (const auto &[src, pred] : instr.phiPreds) {
                        phiMoves[pred][bi].push_back({instr.dst, src, instr.type});
                    }
                }
                if (instr.dst == LirNoReg) {
                    continue;
                }

                if (instr.op == LirOpcode::Alloca) {
                    int dataSz;
                    if (!instr.strArg.empty()) {
                        int count = std::stoi(instr.strArg);
                        const TypeRef &et = instr.type.inner.empty() ? instr.type : instr.type.inner[0];
                        int elemSz = SizeOfRuntime(et);
                        dataSz = count * (elemSz > 0 ? elemSz : 8);
                    }
                    else {
                        dataSz = StackValueSize(instr.type);
                    }

                    AllocSlot(instr.dst, 8);
                    allocaData[instr.dst] = AllocRegion(dataSz > 0 ? dataSz : 8);
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

    // Module / function generation
    void BuildLayouts() {
        for (const auto &mod : pkg.modules) {
            for (const auto &name : mod.interfaceNames) {
                interfaceNames.insert(name);
            }
            for (const auto &s : mod.structs) {
                layouts[s.name] = ComputeStructLayout(s, layouts);
            }
        }
    }

    void GenModule(const LirModule &mod) {
        // Extern variable declarations
        for (const auto &ev : mod.externVars) {
            NeedExtern(ev.name);
            // Emit a comment in .data noting the extern
            data << "; extern var: " << ev.name << "\n";
        }

        // Module-level constants in .rodata
        for (const auto &c : mod.consts) {
            std::string vis = c.isPublic ? "global " + c.name + "\n" : "";
            data << vis;
            data << c.name << ":  ; " << c.type.ToString() << " = " << c.value << "\n";
            data << "    ; (constant — initialized at link time)\n";
        }

        // Vtables
        for (const auto &vt : mod.vtables) {
            rodata << vt.label << ":\n";
            for (const auto &m : vt.methods) {
                rodata << "    dq " << m << "\n";
            }
        }

        // Functions
        for (const auto &func : mod.funcs) {
            GenFunc(func);
        }
    }

    void GenFunc(const LirFunc &func) {
        if (func.isExtern) {
            NeedExtern(func.name);
            return;
        }

        curFunc = func.name;
        PrepassFunc(func);

        // Global / visibility declaration
        if (func.isPublic) {
            globals << "global " << func.name << "\n";
        }

        TB();
        TC(std::format("── {} ─", func.name));
        TL(func.name);

        // Prologue
        TI("push    rbp");
        TI("mov     rbp, rsp");
        for (int rIdx : usedPhysRegs) {
            TI(std::format("push    {}", PhysRegName(rIdx)));
        }
        int32_t remainingFrame = frameSize - static_cast<int32_t>(usedPhysRegs.size() * 8);
        if (remainingFrame > 0) {
            TI(std::format("sub     rsp, {}", remainingFrame));
        }

        // Spill parameter ABI registers to their stack slots
        if (kDefaultCallIsWin64) {
            int argIdx = 0;
            for (const auto &p : func.params) {
                int sz = SizeOf(p.type);
                int off = slotMap.at(p.reg);
                if (argIdx < 4) {
                    if (IsWin64AddressParam(p.type)) {
                        TI(std::format("mov     qword [rbp - {}], {}", off, kWin64IntArgRegs[argIdx]));
                    }
                    else if (IsFloat(p.type)) {
                        TI(std::format("{:<8}{} [rbp - {}], {}", sz == 4 ? "movss" : "movsd", PtrSize(sz), off,
                                       kFltArgRegs[argIdx]));
                    }
                    else {
                        TI(std::format("{:<8}{} [rbp - {}], {}", "mov", PtrSize(std::max(sz, 1)), off,
                                       kWin64IntArgRegs[argIdx]));
                    }
                    argIdx++;
                }
                else {
                    // Remaining arguments arrive on the stack at [rbp + 16 + 32 + (argIdx - 4)*8]
                    int32_t stackArgOff = 48 + (argIdx - 4) * 8;
                    if (IsWin64AddressParam(p.type)) {
                        TI(std::format("mov     rax, qword [rbp + {}]", stackArgOff));
                        TI(std::format("mov     qword [rbp - {}], rax", off));
                    }
                    else if (IsFloat(p.type)) {
                        TI(std::format("{:<8}xmm0, {} [rbp + {}]", sz == 4 ? "movss" : "movsd", PtrSize(sz),
                                       stackArgOff));
                        TI(std::format("{:<8}{} [rbp - {}], xmm0", sz == 4 ? "movss" : "movsd", PtrSize(sz), off));
                    }
                    else {
                        TI(std::format("mov     rax, {} [rbp + {}]", PtrSize(std::max(sz, 1)), stackArgOff));
                        TI(std::format("mov     {} [rbp - {}], rax", PtrSize(std::max(sz, 1)), off));
                    }
                    argIdx++;
                }
            }
        }
        else {
            int intArgIdx = 0, fltArgIdx = 0;
            for (const auto &p : func.params) {
                int sz = SizeOf(p.type);
                int off = slotMap.at(p.reg);
                if (IsFloat(p.type)) {
                    if (fltArgIdx < 8) {
                        TI(std::format("{:<8}{} [rbp - {}], {}", sz == 4 ? "movss" : "movsd", PtrSize(sz), off,
                                       kFltArgRegs[fltArgIdx]));
                        fltArgIdx++;
                    }
                    // Remaining float params arrive on the stack (System
                    // V): skip for now
                }
                else {
                    if (intArgIdx < 6) {
                        TI(std::format("{:<8}{} [rbp - {}], {}", "mov", PtrSize(std::max(sz, 1)), off,
                                       kIntArgRegs[intArgIdx]));
                        intArgIdx++;
                    }
                    // Stack params beyond the 6th: at [rbp + 16 + 8*n]
                }
            }
        }
        // Load params into their allocated physical registers
        for (const auto &p : func.params) {
            auto it = physRegMap.find(p.reg);
            if (it != physRegMap.end()) {
                int sz = IsWin64AddressParam(p.type) ? 8 : SizeOfRuntime(p.type);
                int off = slotMap.at(p.reg);
                if (sz == 8 || sz == 0) {
                    TI(std::format("{:<8}rax, qword [rbp - {}]", "mov", off));
                }
                else if (p.type.IsSigned()) {
                    TI(std::format("{:<8}rax, {} [rbp - {}]", sz == 4 ? "movsxd" : "movsx", PtrSize(sz), off));
                }
                else {
                    if (sz == 4) {
                        TI(std::format("{:<8}eax, dword [rbp - {}]", "mov", off));
                    }
                    else {
                        TI(std::format("{:<8}rax, {} [rbp - {}]", "movzx", PtrSize(sz), off));
                    }
                }
                TI(std::format("{:<8}{}, rax", "mov", PhysRegName(it->second)));
            }
        }

        // Basic blocks
        for (uint32_t bi = 0; bi < func.blocks.size(); bi++) {
            GenBlock(bi, func.blocks[bi], func);
        }

        TB();
    }

    void GenBlock(uint32_t idx, const LirBlock &block, const LirFunc &func) {
        // Emit label for every block after entry
        std::string lbl = BlockLabel(idx, block.label);
        if (idx != 0) {
            TB();
            TL(lbl);
        }

        for (const auto &instr : block.instrs) {
            GenInstr(instr, func);
        }

        if (block.term) {
            GenTerminator(idx, *block.term, func);
        }
        else {
            TI("nop    ; missing terminator");
        }
    }

    // Instruction generation
    void GenInstr(const LirInstr &instr, const LirFunc & /*func*/) {
        switch (instr.op) {
        case LirOpcode::Const: {
            if (instr.dst == LirNoReg) {
                break;
            }
            const TypeRef &t = instr.type;
            int sz = SizeOf(t);
            if (t.kind == TypeRef::Kind::Str) {
                std::string lbl = InternStr(instr.strArg);
                TI(std::format("{:<8}rax, {}", "lea", lbl));
            }
            else if (t.kind == TypeRef::Kind::Float32) {
                std::string lbl = InternF32(instr.strArg);
                TI(std::format("{:<8}xmm0, dword [rel {}]", "movss", lbl));
                TI(std::format("{:<8}dword [rbp - {}], xmm0", "movss", slotMap.at(instr.dst)));
                break;
            }
            else if (t.kind == TypeRef::Kind::Float64) {
                std::string lbl = InternF64(instr.strArg);
                TI(std::format("{:<8}xmm0, qword [rel {}]", "movsd", lbl));
                TI(std::format("{:<8}qword [rbp - {}], xmm0", "movsd", slotMap.at(instr.dst)));
                break;
            }
            else if (t.kind == TypeRef::Kind::Bool) {
                std::string v = (instr.strArg == "true" || instr.strArg == "1") ? "1" : "0";
                TI(std::format("{:<8}rax, {}", "mov", v));
            }
            else {
                // Integer / char: the literal is the numeric value
                TI(std::format("{:<8}rax, {}", "mov", instr.strArg.empty() ? "0" : instr.strArg));
            }
            StoreA(instr.dst, sz > 0 ? t : TypeRef::MakeInt64());
            break;
        }

        case LirOpcode::Alloca: {
            int32_t dataOff = allocaData.at(instr.dst);
            TI(std::format("{:<8}rax, [rbp - {}]", "lea", dataOff));
            StoreA(instr.dst, TypeRef::MakePointer(instr.type));
            break;
        }

        case LirOpcode::Load: {
            const TypeRef &t = instr.type;
            int sz = SizeOfRuntime(t);
            int runtimeSz = SizeOfRuntime(t);
            if (!instr.strArg.empty()) {
                // Named global / constant
                TI(std::format("{:<8}rax, [rel {}]", "mov", instr.strArg));
            }
            else {
                // Load through pointer in srcs[0]
                LirReg ptr = instr.srcs[0];
                auto it = physRegMap.find(ptr);
                if (it != physRegMap.end()) {
                    TI(std::format("{:<8}r10, {}", "mov", PhysRegName(it->second)));
                }
                else {
                    TI(std::format("{:<8}r10, qword [rbp - {}]", "mov", slotMap.at(ptr)));
                }
                if (runtimeSz == 16) {
                    TI(std::format("{:<8}rax, qword [r10]", "mov"));
                    TI(std::format("{:<8}qword [rbp - {}], rax", "mov", slotMap.at(instr.dst)));
                    TI(std::format("{:<8}rax, qword [r10 + 8]", "mov"));
                    TI(std::format("{:<8}qword [rbp - {}], rax", "mov", slotMap.at(instr.dst) - 8));
                    break;
                }
                if (IsFloat(t)) {
                    TI(std::format("{:<8}xmm0, {} [r10]", sz == 4 ? "movss" : "movsd", PtrSize(sz)));
                    TI(std::format("{:<8}{} [rbp - {}], xmm0", sz == 4 ? "movss" : "movsd", PtrSize(sz),
                                   slotMap.at(instr.dst)));
                    break;
                }
                else if (sz == 8 || sz == 0) {
                    TI(std::format("{:<8}rax, qword [r10]", "mov"));
                }
                else if (t.IsSigned()) {
                    TI(std::format("{:<8}rax, {} [r10]", sz == 4 ? "movsxd" : "movsx", PtrSize(sz)));
                }
                else {
                    if (sz == 4) {
                        TI(std::format("{:<8}eax, dword [r10]", "mov"));
                    }
                    else {
                        TI(std::format("{:<8}rax, {} [r10]", "movzx", PtrSize(sz)));
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

            // Load pointer
            auto itPtr = physRegMap.find(ptr);
            if (itPtr != physRegMap.end()) {
                TI(std::format("{:<8}r11, {}", "mov", PhysRegName(itPtr->second)));
            }
            else {
                TI(std::format("{:<8}r11, qword [rbp - {}]", "mov", slotMap.at(ptr)));
            }

            if (runtimeSz == 16) {
                TI(std::format("{:<8}rax, qword [rbp - {}]", "mov", slotMap.at(val)));
                TI(std::format("{:<8}qword [r11], rax", "mov"));
                TI(std::format("{:<8}rax, qword [rbp - {}]", "mov", slotMap.at(val) - 8));
                TI(std::format("{:<8}qword [r11 + 8], rax", "mov"));
            }
            else if (IsFloat(t)) {
                TI(std::format("{:<8}xmm0, {} [rbp - {}]", sz == 4 ? "movss" : "movsd", PtrSize(sz), slotMap.at(val)));
                TI(std::format("{:<8}{} [r11], xmm0", sz == 4 ? "movss" : "movsd", PtrSize(sz)));
            }
            else {
                int store_sz = (sz > 0) ? sz : 8;
                LoadA(val, t);
                TI(std::format("{:<8}{} [r11], {}", "mov", PtrSize(store_sz), GprA(store_sz)));
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
                std::string_view op;
                bool f32 = (t.kind == TypeRef::Kind::Float32);
                switch (instr.op) {
                case LirOpcode::Add:
                    op = f32 ? "addss" : "addsd";
                    break;
                case LirOpcode::Sub:
                    op = f32 ? "subss" : "subsd";
                    break;
                default:
                    op = f32 ? "addss" : "addsd";
                    break;
                }
                TI(std::format("{:<8}xmm0, xmm1", op));
                StoreA(instr.dst, t);
            }
            else {
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                std::string_view op;
                switch (instr.op) {
                case LirOpcode::Add:
                    op = "add";
                    break;
                case LirOpcode::Sub:
                    op = "sub";
                    break;
                case LirOpcode::And:
                    op = "and";
                    break;
                case LirOpcode::Or:
                    op = "or";
                    break;
                case LirOpcode::Xor:
                    op = "xor";
                    break;
                default:
                    op = "add";
                    break;
                }
                TI(std::format("{:<8}rax, r10", op));
                StoreA(instr.dst, t);
            }
            break;
        }

        case LirOpcode::Mul: {
            const TypeRef &t = instr.type;
            if (IsFloat(t)) {
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                TI(t.kind == TypeRef::Kind::Float32 ? "mulss   xmm0, xmm1" : "mulsd   xmm0, xmm1");
                StoreA(instr.dst, t);
            }
            else {
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                TI("imul    rax, r10");
                StoreA(instr.dst, t);
            }
            break;
        }

        case LirOpcode::Div:
        case LirOpcode::Mod: {
            const TypeRef &t = instr.type;
            if (IsFloat(t)) {
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                TI(t.kind == TypeRef::Kind::Float32 ? "divss   xmm0, xmm1" : "divsd   xmm0, xmm1");
                StoreA(instr.dst, t);
            }
            else {
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                // idiv uses rdx:rax; result in rax (quotient) or rdx
                // (remainder)
                if (t.IsSigned()) {
                    TI("cqo"); // sign-extend rax → rdx:rax
                    TI("idiv    r10");
                }
                else {
                    TI("xor     rdx, rdx"); // zero-extend rax
                    TI("div     r10");
                }
                if (instr.op == LirOpcode::Mod) {
                    TI("mov     rax, rdx"); // remainder is in rdx
                }
                StoreA(instr.dst, t);
            }
            break;
        }

        case LirOpcode::Pow: {
            // Integer ** calls the in-unit __rux_ipow helper; float ** calls libm pow.
            const TypeRef &t = instr.type;
            constexpr int shadowSpace = kDefaultCallIsWin64 ? 32 : 0;
            if constexpr (shadowSpace > 0) {
                TI(std::format("sub     rsp, {}", shadowSpace));
            }
            if (IsFloat(t)) {
                // Float ** uses a synthesized x87 helper, not libm pow, so
                // no CRT/math dependency is introduced.
                const bool isF32 = t.kind == TypeRef::Kind::Float32;
                usesFpow = true;
                usesFpowF32 = usesFpowF32 || isF32;
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                TI(isF32 ? "call    __rux_powf32" : "call    __rux_powf64");
            }
            else {
                usesIpow = true;
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                TI("mov     rcx, rax");
                TI("mov     rdx, r10");
                TI("call    __rux_ipow");
            }
            if constexpr (shadowSpace > 0) {
                TI(std::format("add     rsp, {}", shadowSpace));
            }
            StoreA(instr.dst, t);
            break;
        }

        case LirOpcode::Shl:
        case LirOpcode::Shr: {
            const TypeRef &t = instr.type;
            LoadA(instr.srcs[0], t);
            // Shift count must be in cl
            TI(std::format("{:<8}r11, qword [rbp - {}]", "mov", slotMap.at(instr.srcs[1])));
            TI("mov     rcx, r11");
            if (bool isShr = (instr.op == LirOpcode::Shr); isShr && t.IsSigned()) {
                TI("sar     rax, cl");
            }
            else if (isShr) {
                TI("shr     rax, cl");
            }
            else {
                TI("shl     rax, cl");
            }
            StoreA(instr.dst, t);
            break;
        }

        case LirOpcode::Neg: {
            const TypeRef &t = instr.type;
            if (IsFloat(t)) {
                LoadA(instr.srcs[0], t);
                // Negate by XOR with sign bit
                if (t.kind == TypeRef::Kind::Float32) {
                    std::string lbl = InternF32("0x80000000");
                    TI(std::format("movss   xmm1, dword [rel {}]", lbl));
                    TI("xorps   xmm0, xmm1");
                }
                else {
                    std::string lbl = InternF64("0x8000000000000000");
                    TI(std::format("movsd   xmm1, qword [rel {}]", lbl));
                    TI("xorpd   xmm0, xmm1");
                }
                StoreA(instr.dst, t);
            }
            else {
                LoadA(instr.srcs[0], t);
                TI("neg     rax");
                StoreA(instr.dst, t);
            }
            break;
        }

        case LirOpcode::Not: {
            // Logical not: result = (operand == 0) ? 1 : 0
            const TypeRef &t = instr.type;
            LoadA(instr.srcs[0], t);
            TI("test    rax, rax");
            TI("setz    al");
            TI("movzx   rax, al");
            StoreA(instr.dst, TypeRef::MakeBool());
            break;
        }

        case LirOpcode::BitNot: {
            const TypeRef &t = instr.type;
            LoadA(instr.srcs[0], t);
            TI("not     rax");
            StoreA(instr.dst, t);
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
                TI(lhsT.kind == TypeRef::Kind::Float32 ? "ucomiss xmm0, xmm1" : "ucomisd xmm0, xmm1");
                std::string_view set;
                switch (instr.op) {
                case LirOpcode::CmpEq:
                    set = "sete";
                    break;
                case LirOpcode::CmpNe:
                    set = "setne";
                    break;
                case LirOpcode::CmpLt:
                    set = "setb";
                    break;
                case LirOpcode::CmpLe:
                    set = "setbe";
                    break;
                case LirOpcode::CmpGt:
                    set = "seta";
                    break;
                default:
                    set = "setae";
                    break;
                }
                TI(std::format("{:<8}al", set));
            }
            else {
                TI("cmp     rax, r10");
                std::string_view set;
                bool sig = lhsT.IsSigned();
                switch (instr.op) {
                case LirOpcode::CmpEq:
                    set = "sete";
                    break;
                case LirOpcode::CmpNe:
                    set = "setne";
                    break;
                case LirOpcode::CmpLt:
                    set = sig ? "setl" : "setb";
                    break;
                case LirOpcode::CmpLe:
                    set = sig ? "setle" : "setbe";
                    break;
                case LirOpcode::CmpGt:
                    set = sig ? "setg" : "seta";
                    break;
                default:
                    set = sig ? "setge" : "setae";
                    break;
                }
                TI(std::format("{:<8}al", set));
            }
            TI("movzx   rax, al");
            StoreA(instr.dst, TypeRef::MakeBool());
            break;
        }

        case LirOpcode::Cast: {
            const TypeRef &dst_t = instr.type;
            TypeRef src_t;
            // strArg holds the source type string; try to reconstruct
            // enough info by looking up the register type
            if (regTypes.contains(instr.srcs[0])) {
                src_t = regTypes.at(instr.srcs[0]);
            }
            else {
                src_t = dst_t;
            }

            bool srcFloat = IsFloat(src_t);
            bool dstFloat = IsFloat(dst_t);

            LoadA(instr.srcs[0], src_t);

            if (!srcFloat && !dstFloat) {
                // int → int: sign/zero extend is already done by LoadA;
                // for narrowing, the lower bits in rax are already
                // correct. Nothing extra needed for most cases.
            }
            else if (srcFloat && !dstFloat) {
                // float → int
                bool f32src = (src_t.kind == TypeRef::Kind::Float32);
                if (bool signed_ = dst_t.IsSigned(); signed_) {
                    TI(f32src ? "cvttss2si rax, xmm0" : "cvttsd2si rax, xmm0");
                }
                else {
                    // Unsigned: no direct instruction; use signed then
                    // re-interpret
                    TI(f32src ? "cvttss2si rax, xmm0" : "cvttsd2si rax, xmm0");
                }
            }
            else if (!srcFloat && dstFloat) {
                // int → float
                bool f32dst = (dst_t.kind == TypeRef::Kind::Float32);
                TI(f32dst ? "cvtsi2ss  xmm0, rax" : "cvtsi2sd  xmm0, rax");
            }
            else {
                // float → float
                bool f32src = (src_t.kind == TypeRef::Kind::Float32);
                bool f32dst = (dst_t.kind == TypeRef::Kind::Float32);
                if (f32src && !f32dst) {
                    TI("cvtss2sd  xmm0, xmm0");
                }
                else if (!f32src && f32dst) {
                    TI("cvtsd2ss  xmm0, xmm0");
                }
            }
            StoreA(instr.dst, dst_t);
            break;
        }

        case LirOpcode::Call: {
            EmitCall(instr.strArg, instr.srcs, instr.dst, instr.type, instr.callConv);
            break;
        }

        case LirOpcode::CallIndirect: {
            // strArg empty; srcs[0] = callee, srcs[1..] = args
            EmitCallIndirect(instr.srcs, instr.dst, instr.type, instr.callConv);
            break;
        }

        case LirOpcode::FieldPtr: {
            LirReg base = instr.srcs[0];
            LoadA(base, regTypes.at(base));

            // Compute field offset using struct layout
            int fieldOff = ResolveFieldOffset(base, instr.strArg);
            if (fieldOff != 0) {
                TI(std::format("{:<8}rax, [rax + {}]", "lea", fieldOff));
            }
            // else pointer is already at the field start
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
            TI(std::format("{:<8}r11, r10, {}", "imul", elemSz));
            TI("add     rax, r11");
            StoreA(instr.dst, TypeRef::MakePointer(instr.type));
            break;
        }

        case LirOpcode::Phi:
            // Phi moves are emitted by predecessors before their
            // terminators. The phi dst slot is already allocated;
            // nothing to emit here.
            break;

        case LirOpcode::GlobalAddr:
            TI(std::format("{:<8}rax, [rel {}]", "lea", instr.strArg));
            StoreA(instr.dst, TypeRef::MakePointer(instr.type));
            break;

        case LirOpcode::StringAddr: {
            const TypeRef elemType = instr.type.inner.empty() ? TypeRef::MakeChar8() : instr.type.inner[0];
            const std::string lbl = InternStr(EncodeStringLiteral(instr.strArg, SizeOfRuntime(elemType)));
            TI(std::format("{:<8}rax, [rel {}]", "lea", lbl));
            StoreA(instr.dst, instr.type);
            break;
        }

        default:
            TC(std::format("TODO: opcode {}", static_cast<int>(instr.op)));
            break;
        }
    }

    // Field offset resolution via regTypes_ + layouts_
    int ResolveFieldOffset(LirReg base, const std::string &fieldName) {
        auto typeIt = regTypes.find(base);
        if (typeIt == regTypes.end()) {
            return 0;
        }
        const TypeRef &ptrType = typeIt->second;
        if (ptrType.kind != TypeRef::Kind::Pointer || ptrType.inner.empty()) {
            return 0;
        }
        const TypeRef &inner = ptrType.inner[0];
        if (inner.kind == TypeRef::Kind::Tuple) {
            std::size_t idx = 0;
            try {
                idx = std::stoul(fieldName);
            }
            catch (...) {
                return 0;
            }
            if (idx >= inner.inner.size()) {
                return 0;
            }
            int offset = 0;
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
        if (interfaceNames.contains(baseName)) {
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
        for (const auto &f : layIt->second.fields) {
            if (f.name == fieldName) {
                return f.offset;
            }
        }
        return 0;
    }

    // Call emission
    void EmitCall(const std::string &callee, const std::vector<LirReg> &args, LirReg dst, const TypeRef &retType,
                  CallingConvention callConv) {
        const bool win64 =
            callConv == CallingConvention::Win64 || (callConv == CallingConvention::Default && kDefaultCallIsWin64);
        const auto *intRegs = win64 ? kWin64IntArgRegs : kIntArgRegs;
        const int maxIntRegs = win64 ? 4 : 6;
        std::vector<LirReg> stackArgs;
        // Set up arguments into ABI registers
        if (win64) {
            for (int i = 0; i < static_cast<int>(args.size()); ++i) {
                LirReg arg = args[i];
                TypeRef at = regTypes.contains(arg) ? regTypes.at(arg) : TypeRef::MakeInt64();
                if (i < 4) {
                    if (IsFloat(at)) {
                        int sz = SizeOf(at);
                        TI(std::format("{:<8}{}, {} [rbp - {}]", sz == 4 ? "movss" : "movsd", kFltArgRegs[i],
                                       PtrSize(sz), slotMap.at(arg)));
                    }
                    else {
                        LoadA(arg, at);
                        TI(std::format("{:<8}{}, rax", "mov", intRegs[i]));
                    }
                }
                else {
                    stackArgs.push_back(arg);
                }
            }
        }
        else {
            int intIdx = 0, fltIdx = 0;
            for (LirReg arg : args) {
                TypeRef at = regTypes.contains(arg) ? regTypes.at(arg) : TypeRef::MakeInt64();
                if (IsFloat(at)) {
                    if (fltIdx < 8) {
                        int sz = SizeOf(at);
                        TI(std::format("{:<8}{}, {} [rbp - {}]", sz == 4 ? "movss" : "movsd", kFltArgRegs[fltIdx],
                                       PtrSize(sz), slotMap.at(arg)));
                        fltIdx++;
                    }
                    else {
                        stackArgs.push_back(arg);
                    }
                }
                else {
                    if (intIdx < maxIntRegs) {
                        LoadA(arg, at);
                        TI(std::format("{:<8}{}, rax", "mov", intRegs[intIdx]));
                        intIdx++;
                    }
                    else {
                        stackArgs.push_back(arg);
                    }
                }
            }
        }
        const int stackBytes = win64 ? 32 + AlignUp(static_cast<int>(stackArgs.size()) * 8, 16)
                                     : AlignUp(static_cast<int>(stackArgs.size()) * 8, 16);
        if (stackBytes > 0) {
            TI(std::format("sub     rsp, {}", stackBytes));
            StoreStackArgs(stackArgs, win64);
        }
        // Stack is already 16-byte aligned: prologue sub rsp,frameSize
        // ensures rsp ≡ 8 (mod 16) which the ABI requires before a call
        // instruction.
        TI(std::format("{:<8}{}", "call", callee));
        if (stackBytes > 0) {
            TI(std::format("add     rsp, {}", stackBytes));
        }
        if (dst != LirNoReg && !retType.IsOpaque()) {
            StoreA(dst, retType);
        }
    }

    void EmitCallIndirect(const std::vector<LirReg> &srcs, LirReg dst, const TypeRef &retType,
                          CallingConvention callConv) {
        if (srcs.empty()) {
            return;
        }
        LirReg callee = srcs[0];
        std::vector<LirReg> args(srcs.begin() + 1, srcs.end());
        const std::vector<LirReg> stackArgs = EmitCallArgs(args, callConv);
        const bool win64 =
            callConv == CallingConvention::Win64 || (callConv == CallingConvention::Default && kDefaultCallIsWin64);
        const int stackBytes = win64 ? 32 + AlignUp(static_cast<int>(stackArgs.size()) * 8, 16)
                                     : AlignUp(static_cast<int>(stackArgs.size()) * 8, 16);
        if (stackBytes > 0) {
            TI(std::format("sub     rsp, {}", stackBytes));
            StoreStackArgs(stackArgs, win64);
        }
        // Load the callee after preparing args because arg setup uses
        // r10.
        TI(std::format("{:<8}r10, qword [rbp - {}]", "mov", slotMap.at(callee)));
        TI("call    r10");
        if (stackBytes > 0) {
            TI(std::format("add     rsp, {}", stackBytes));
        }
        if (dst != LirNoReg && !retType.IsOpaque()) {
            StoreA(dst, retType);
        }
    }

    void StoreStackArgs(const std::vector<LirReg> &stackArgs, bool win64) {
        for (std::size_t i = 0; i < stackArgs.size(); ++i) {
            TypeRef at = regTypes.contains(stackArgs[i]) ? regTypes.at(stackArgs[i]) : TypeRef::MakeInt64();
            LoadA(stackArgs[i], at);
            const int offset = win64 ? (32 + i * 8) : (i * 8);
            TI(std::format("{:<8}qword [rsp + {}], rax", "mov", offset));
        }
    }

    std::vector<LirReg> EmitCallArgs(const std::vector<LirReg> &args, CallingConvention callConv) {
        const bool win64 =
            callConv == CallingConvention::Win64 || (callConv == CallingConvention::Default && kDefaultCallIsWin64);
        const auto *intRegs = win64 ? kWin64IntArgRegs : kIntArgRegs;
        const int maxIntRegs = win64 ? 4 : 6;
        std::vector<LirReg> stackArgs;
        if (win64) {
            for (int i = 0; i < static_cast<int>(args.size()); ++i) {
                LirReg arg = args[i];
                TypeRef at = regTypes.contains(arg) ? regTypes.at(arg) : TypeRef::MakeInt64();
                if (i < 4) {
                    if (IsFloat(at)) {
                        const int sz = SizeOf(at);
                        TI(std::format("{:<8}{}, {} [rbp - {}]", sz == 4 ? "movss" : "movsd", kFltArgRegs[i],
                                       PtrSize(sz), slotMap.at(arg)));
                    }
                    else {
                        LoadA(arg, at);
                        TI(std::format("{:<8}{}, rax", "mov", intRegs[i]));
                    }
                }
                else {
                    stackArgs.push_back(arg);
                }
            }
        }
        else {
            int intIdx = 0, fltIdx = 0;
            for (LirReg arg : args) {
                if (TypeRef at = regTypes.contains(arg) ? regTypes.at(arg) : TypeRef::MakeInt64(); IsFloat(at)) {
                    if (fltIdx < 8) {
                        const int sz = SizeOf(at);
                        TI(std::format("{:<8}{}, {} [rbp - {}]", sz == 4 ? "movss" : "movsd", kFltArgRegs[fltIdx],
                                       PtrSize(sz), slotMap.at(arg)));
                        fltIdx++;
                    }
                    else {
                        stackArgs.push_back(arg);
                    }
                }
                else {
                    if (intIdx < maxIntRegs) {
                        LoadA(arg, at);
                        TI(std::format("{:<8}{}, rax", "mov", intRegs[intIdx]));
                        intIdx++;
                    }
                    else {
                        stackArgs.push_back(arg);
                    }
                }
            }
        }
        return stackArgs;
    }

    // Terminator generation
    void GenTerminator(uint32_t blockIdx, const LirTerminator &term, const LirFunc &func) {
        switch (term.kind) {
        case LirTermKind::Jump: {
            EmitPhiMoves(blockIdx, term.trueTarget);
            TI(std::format("{:<8}{}", "jmp", BlockLabel(term.trueTarget, func.blocks[term.trueTarget].label)));
            break;
        }

        case LirTermKind::Branch: {
            // Load condition
            TypeRef condT = regTypes.contains(term.cond) ? regTypes.at(term.cond) : TypeRef::MakeBool();
            LoadA(term.cond, condT);
            TI("test    rax, rax");

            std::string trueLabel = BlockLabel(term.trueTarget, func.blocks[term.trueTarget].label);
            std::string falseLabel = BlockLabel(term.falseTarget, func.blocks[term.falseTarget].label);

            // Check whether either branch needs phi moves
            bool truePhi = HasPhiMoves(blockIdx, term.trueTarget);
            bool falsePhi = HasPhiMoves(blockIdx, term.falseTarget);

            if (!truePhi && !falsePhi) {
                TI(std::format("{:<8}{}", "jz", falseLabel));
                TI(std::format("{:<8}{}", "jmp", trueLabel));
            }
            else {
                // Use intermediate labels so each path has room for phi
                // moves
                std::string trampTrue = std::format(".{}_br{}_t", curFunc, blockIdx);
                std::string trampFalse = std::format(".{}_br{}_f", curFunc, blockIdx);
                TI(std::format("{:<8}{}", "jz", trampFalse));
                TL(trampTrue);
                EmitPhiMoves(blockIdx, term.trueTarget);
                TI(std::format("{:<8}{}", "jmp", trueLabel));
                TL(trampFalse);
                EmitPhiMoves(blockIdx, term.falseTarget);
                TI(std::format("{:<8}{}", "jmp", falseLabel));
            }
            break;
        }

        case LirTermKind::Return: {
            if (term.retVal && *term.retVal != LirNoReg) {
                LoadReturnValue(*term.retVal, term.retType);
                // Result already in rax or xmm0 — do not overwrite
            }
            EmitEpilogue();
            break;
        }

        case LirTermKind::Switch: {
            TypeRef condT = regTypes.contains(term.cond) ? regTypes.at(term.cond) : TypeRef::MakeInt64();
            LoadA(term.cond, condT);
            for (const auto &c : term.cases) {
                TI(std::format("{:<8}rax, {}", "cmp", c.value));
                std::string lbl = BlockLabel(c.target, func.blocks[c.target].label);
                TI(std::format("{:<8}{}", "je", lbl));
            }
            EmitPhiMoves(blockIdx, term.defaultTarget);
            TI(std::format("{:<8}{}", "jmp", BlockLabel(term.defaultTarget, func.blocks[term.defaultTarget].label)));
            break;
        }
        }
    }

    [[nodiscard]] bool HasPhiMoves(uint32_t from, uint32_t to) const {
        const auto it = phiMoves.find(from);
        if (it == phiMoves.end()) {
            return false;
        }
        return it->second.contains(to);
    }

    void EmitEpilogue() {
        if (!usedPhysRegs.empty()) {
            int32_t remainingFrame = frameSize - static_cast<int32_t>(usedPhysRegs.size() * 8);
            if (remainingFrame > 0) {
                TI(std::format("add     rsp, {}", remainingFrame));
            }
            for (auto it = usedPhysRegs.rbegin(); it != usedPhysRegs.rend(); ++it) {
                TI(std::format("pop     {}", PhysRegName(*it)));
            }
            TI("pop     rbp");
        }
        else {
            TI("leave");
        }
        TI("ret");
    }
};

// AsmGen::Generate
std::string AsmGen::Generate() {
    BuildLayouts();
    for (const auto &mod : pkg.modules) {
        GenModule(mod);
    }
    if (usesIpow) {
        EmitIntPowHelper();
    }
    // The f32 pow helper calls the f64 one, so emit f64 first.
    if (usesFpow) {
        EmitFloatPowHelper();
    }
    if (usesFpowF32) {
        EmitFloatPowF32Helper();
    }
    std::ostringstream out;
    out << "; Generated by Rux Compiler\n";
    if (kDefaultCallIsWin64) {
        out << "; Target:  x86-64  (Windows x64 ABI, NASM syntax)\n";
        out << "; Calling: rcx/rdx/r8/r9 (int args), xmm0-3 (float args)\n";
    }
    else {
        out << "; Target:  x86-64  (System V AMD64 ABI, NASM syntax)\n";
        out << "; Calling: rdi/rsi/rdx/rcx/r8/r9 (int args), xmm0-7 (float args)\n";
    }
    out << "; Scratch: r10, r11 (caller-saved)\n";
    out << "\n";
    out << "bits 64\n\n";
    if (const std::string ext = externs.str(); !ext.empty()) {
        out << ext << "\n";
    }
    if (const std::string glb = globals.str(); !glb.empty()) {
        out << glb << "\n";
    }
    if (const std::string rod = rodata.str(); !rod.empty()) {
        out << "section .rodata\n" << rod << "\n";
    }
    if (const std::string dat = data.str(); !dat.empty()) {
        out << "section .data\n" << dat << "\n";
    }
    if (const std::string cod = text.str(); !cod.empty()) {
        out << "section .text\n" << cod;
    }
    return out.str();
}
} // namespace

// Public API
AssemblyPrinter::AssemblyPrinter(LirPackage package)
    : lir(std::move(package)) {
}

std::string AssemblyPrinter::Generate() const {
    AsmGen gen(lir);
    return gen.Generate();
}

bool AssemblyPrinter::Emit(const LirPackage &package, const std::filesystem::path &path) {
    AsmGen gen(package);
    std::string text = gen.Generate();
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f) {
        return false;
    }
    f << text;
    return f.good();
}
} // namespace Rux
