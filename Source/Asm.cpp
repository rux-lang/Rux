/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#include "Rux/Asm.h"

#include <cassert>
#include <format>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux {

// ─────────────────────────────────────────────────────────────────────────────
// Type utilities
// ─────────────────────────────────────────────────────────────────────────────

namespace {

int SizeOf(const TypeRef& t) {
    switch (t.kind) {
        case TypeRef::Kind::Bool:
        case TypeRef::Kind::Int8:
        case TypeRef::Kind::UInt8:   return 1;
        case TypeRef::Kind::Int16:
        case TypeRef::Kind::UInt16:  return 2;
        case TypeRef::Kind::Char:
        case TypeRef::Kind::Int32:
        case TypeRef::Kind::UInt32:
        case TypeRef::Kind::Float32: return 4;
        case TypeRef::Kind::Void:    return 0;
        default:                     return 8; // int64, uint64, float64, pointer, str, named, …
    }
}

bool IsFloat(const TypeRef& t) {
    return t.kind == TypeRef::Kind::Float32 || t.kind == TypeRef::Kind::Float64;
}

int AlignUp(int v, int a) { return (v + a - 1) & ~(a - 1); }

// x86-64 register names sized for the rax family
std::string_view GprA(int bytes) {
    switch (bytes) { case 1: return "al";  case 2: return "ax";  case 4: return "eax"; default: return "rax"; }
}
// r10 family (caller-saved scratch — primary operand)
std::string_view GprB(int bytes) {
    switch (bytes) { case 1: return "r10b"; case 2: return "r10w"; case 4: return "r10d"; default: return "r10"; }
}
// r11 family (caller-saved scratch — secondary operand)
std::string_view GprC(int bytes) {
    switch (bytes) { case 1: return "r11b"; case 2: return "r11w"; case 4: return "r11d"; default: return "r11"; }
}

std::string_view PtrSize(int bytes) {
    switch (bytes) { case 1: return "byte"; case 2: return "word"; case 4: return "dword"; default: return "qword"; }
}

// System V AMD64 integer argument registers (in order)
constexpr std::string_view kIntArgRegs[] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };
// System V AMD64 float argument registers (in order)
constexpr std::string_view kFltArgRegs[] = { "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7" };

// ─────────────────────────────────────────────────────────────────────────────
// Struct field layout
// ─────────────────────────────────────────────────────────────────────────────

struct FieldLayout {
    std::string name;
    int         offset = 0;
    int         size   = 0;
};

struct StructLayout {
    std::vector<FieldLayout> fields;
    int totalSize  = 0;
    int alignment  = 1;
};

using LayoutMap = std::unordered_map<std::string, StructLayout>;

StructLayout ComputeLayout(const LirStructDecl& s, const LayoutMap& known) {
    StructLayout result;
    int offset   = 0;
    int maxAlign = 1;

    for (const auto& f : s.fields) {
        int sz = SizeOf(f.type);
        int al = sz > 0 ? std::min(sz, 8) : 1;

        if (f.type.kind == TypeRef::Kind::Named) {
            if (auto it = known.find(f.type.name); it != known.end()) {
                sz = it->second.totalSize;
                al = it->second.alignment;
            }
        }

        if (al > 1) offset = AlignUp(offset, al);
        result.fields.push_back({ f.name, offset, sz });
        offset += sz;
        maxAlign = std::max(maxAlign, al);
    }

    result.totalSize = AlignUp(offset, maxAlign);
    result.alignment = maxAlign;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Code generator
// ─────────────────────────────────────────────────────────────────────────────

class AsmGen {
public:
    explicit AsmGen(const LirPackage& pkg) : pkg_(pkg) {}
    std::string Generate();

private:
    const LirPackage& pkg_;

    // Separate output streams assembled at the end
    std::ostringstream text_;     // .text section
    std::ostringstream data_;     // .data section (writable globals)
    std::ostringstream rodata_;   // .rodata section (string/fp constants)
    std::ostringstream externs_;  // extern declarations
    std::ostringstream globals_;  // global declarations

    // Interned read-only constants
    std::unordered_map<std::string, std::string> strLabels_;
    std::unordered_map<std::string, std::string> f32Labels_;
    std::unordered_map<std::string, std::string> f64Labels_;
    int constIdx_ = 0;

    // Struct field layouts (built once)
    LayoutMap layouts_;

    // ── Per-function state ────────────────────────────────────────────────────

    struct PhiMove { LirReg dst; LirReg src; TypeRef type; };

    std::string curFunc_;
    std::unordered_map<LirReg, int32_t> slotMap_;   // vreg → rbp offset (positive, address = rbp - offset)
    std::unordered_map<LirReg, int32_t> allocaData_; // alloca vreg → data region rbp offset
    std::unordered_map<LirReg, TypeRef> regTypes_;   // vreg → value type (pointer for alloca)
    int32_t nextOff_   = 0;
    int32_t frameSize_ = 0;

    // phiMoves_[fromBlock][toBlock] = list of (dst, src, type)
    std::unordered_map<uint32_t,
        std::unordered_map<uint32_t, std::vector<PhiMove>>> phiMoves_;

    std::unordered_set<std::string> declaredExterns_;

    // ── Low-level emit helpers ────────────────────────────────────────────────

    void T(std::string_view s)         { text_ << s << '\n'; }
    void TI(std::string_view s)        { text_ << "    " << s << '\n'; }
    void TL(std::string_view label)    { text_ << label << ":\n"; }
    void TC(std::string_view comment)  { text_ << "    ; " << comment << '\n'; }
    void TB()                          { text_ << '\n'; }

    // ── Constant interning ────────────────────────────────────────────────────

    std::string InternStr(const std::string& val) {
        auto it = strLabels_.find(val);
        if (it != strLabels_.end()) return it->second;
        std::string lbl = std::format("__str{}", constIdx_++);
        strLabels_[val] = lbl;
        // Emit as NUL-terminated bytes in .rodata
        rodata_ << lbl << ":\n    db    ";
        for (unsigned char c : val) rodata_ << static_cast<int>(c) << ", ";
        rodata_ << "0\n";
        return lbl;
    }

    std::string InternF32(const std::string& val) {
        auto it = f32Labels_.find(val);
        if (it != f32Labels_.end()) return it->second;
        std::string lbl = std::format("__f32_{}", constIdx_++);
        f32Labels_[val] = lbl;
        rodata_ << lbl << ":\n    dd    " << val << "\n";
        return lbl;
    }

    std::string InternF64(const std::string& val) {
        auto it = f64Labels_.find(val);
        if (it != f64Labels_.end()) return it->second;
        std::string lbl = std::format("__f64_{}", constIdx_++);
        f64Labels_[val] = lbl;
        rodata_ << lbl << ":\n    dq    " << val << "\n";
        return lbl;
    }

    void NeedExtern(const std::string& name) {
        if (declaredExterns_.insert(name).second)
            externs_ << "extern " << name << "\n";
    }

    // ── Stack slot allocation ─────────────────────────────────────────────────

    int32_t AllocSlot(LirReg reg, int bytes) {
        if (auto it = slotMap_.find(reg); it != slotMap_.end()) return it->second;
        int al = (bytes > 0) ? std::min(bytes, 8) : 1;
        nextOff_ = AlignUp(nextOff_, al);
        nextOff_ += (bytes > 0 ? bytes : 8);
        slotMap_[reg] = nextOff_;
        return nextOff_;
    }

    int32_t AllocRegion(int bytes) {
        int al = (bytes > 0) ? std::min(bytes, 8) : 1;
        nextOff_ = AlignUp(nextOff_, al);
        nextOff_ += (bytes > 0 ? bytes : 8);
        return nextOff_;
    }

    // ── Load/store helpers ────────────────────────────────────────────────────
    //
    // Integer loads promote to 64-bit (sign- or zero-extend as needed).
    // Float loads use xmm0 (primary) or xmm1 (secondary).

    // Load vreg into rax (integer) or xmm0 (float)
    void LoadA(LirReg reg, const TypeRef& t) {
        int sz  = SizeOf(t);
        int off = slotMap_.at(reg);
        if (IsFloat(t)) {
            TI(std::format("{:<8}xmm0, {} [rbp - {}]",
                sz == 4 ? "movss" : "movsd", PtrSize(sz), off));
        } else if (sz == 8 || sz == 0) {
            TI(std::format("{:<8}rax, qword [rbp - {}]", "mov", off));
        } else if (t.IsSigned()) {
            TI(std::format("{:<8}rax, {} [rbp - {}]",
                sz == 4 ? "movsxd" : "movsx", PtrSize(sz), off));
        } else {
            if (sz == 4)
                TI(std::format("{:<8}eax, dword [rbp - {}]", "mov", off));
            else
                TI(std::format("{:<8}rax, {} [rbp - {}]", "movzx", PtrSize(sz), off));
        }
    }

    // Load vreg into r10 (integer) or xmm1 (float)
    void LoadB(LirReg reg, const TypeRef& t) {
        int sz  = SizeOf(t);
        int off = slotMap_.at(reg);
        if (IsFloat(t)) {
            TI(std::format("{:<8}xmm1, {} [rbp - {}]",
                sz == 4 ? "movss" : "movsd", PtrSize(sz), off));
        } else if (sz == 8 || sz == 0) {
            TI(std::format("{:<8}r10, qword [rbp - {}]", "mov", off));
        } else if (t.IsSigned()) {
            TI(std::format("{:<8}r10, {} [rbp - {}]",
                sz == 4 ? "movsxd" : "movsx", PtrSize(sz), off));
        } else {
            if (sz == 4)
                TI(std::format("{:<8}r10d, dword [rbp - {}]", "mov", off));
            else
                TI(std::format("{:<8}r10, {} [rbp - {}]", "movzx", PtrSize(sz), off));
        }
    }

    // Store rax (integer) or xmm0 (float) into dst's slot
    void StoreA(LirReg dst, const TypeRef& t) {
        int sz  = SizeOf(t);
        int off = slotMap_.at(dst);
        if (IsFloat(t)) {
            TI(std::format("{:<8}{} [rbp - {}], xmm0",
                sz == 4 ? "movss" : "movsd", PtrSize(sz), off));
        } else {
            int store_sz = (sz > 0) ? sz : 8;
            TI(std::format("{:<8}{} [rbp - {}], {}",
                "mov", PtrSize(store_sz), off, GprA(store_sz)));
        }
    }

    // ── Block labels ──────────────────────────────────────────────────────────

    std::string BlockLabel(uint32_t idx, const std::string& label) const {
        if (idx == 0) return curFunc_;
        return "." + curFunc_ + "_" + label;
    }

    // ── Phi move emission ─────────────────────────────────────────────────────

    void EmitPhiMoves(uint32_t fromBlock, uint32_t toBlock) {
        auto it1 = phiMoves_.find(fromBlock);
        if (it1 == phiMoves_.end()) return;
        auto it2 = it1->second.find(toBlock);
        if (it2 == it1->second.end()) return;

        for (const auto& m : it2->second) {
            if (!slotMap_.count(m.src)) continue;
            LoadA(m.src, m.type);
            StoreA(m.dst, m.type);
        }
    }

    // ── Pre-pass ──────────────────────────────────────────────────────────────

    void PrepassFunc(const LirFunc& func) {
        nextOff_   = 0;
        frameSize_ = 0;
        slotMap_.clear();
        allocaData_.clear();
        regTypes_.clear();
        phiMoves_.clear();

        // Parameters
        for (const auto& p : func.params) {
            AllocSlot(p.reg, 8);  // always 8 bytes — value enters via ABI reg
            regTypes_[p.reg] = p.type;
        }

        // Instructions
        for (uint32_t bi = 0; bi < func.blocks.size(); bi++) {
            const auto& block = func.blocks[bi];
            for (const auto& instr : block.instrs) {
                if (instr.op == LirOpcode::Phi) {
                    for (const auto& [src, pred] : instr.phiPreds)
                        phiMoves_[pred][bi].push_back({ instr.dst, src, instr.type });
                }
                if (instr.dst == LirNoReg) continue;

                if (instr.op == LirOpcode::Alloca) {
                    AllocSlot(instr.dst, 8);  // pointer slot
                    int dataSz = SizeOf(instr.type);
                    allocaData_[instr.dst] = AllocRegion(dataSz > 0 ? dataSz : 8);
                    regTypes_[instr.dst] = TypeRef::MakePointer(instr.type);
                } else {
                    int sz = SizeOf(instr.type);
                    AllocSlot(instr.dst, sz > 0 ? sz : 8);
                    regTypes_[instr.dst] = instr.type;
                }
            }
        }

        frameSize_ = AlignUp(nextOff_, 16);
        if (frameSize_ == 0) frameSize_ = 16; // always reserve at least one slot for alignment
    }

    // ── Module / function generation ──────────────────────────────────────────

    void BuildLayouts() {
        for (const auto& mod : pkg_.modules)
            for (const auto& s : mod.structs)
                layouts_[s.name] = ComputeLayout(s, layouts_);
    }

    void GenModule(const LirModule& mod) {
        // Extern variable declarations
        for (const auto& ev : mod.externVars) {
            NeedExtern(ev.name);
            // Emit a comment in .data noting the extern
            data_ << "; extern var: " << ev.name << "\n";
        }

        // Module-level constants in .rodata
        for (const auto& c : mod.consts) {
            std::string vis = c.isPublic ? "global " + c.name + "\n" : "";
            data_ << vis;
            data_ << c.name << ":  ; " << c.type.ToString() << " = " << c.value << "\n";
            data_ << "    ; (constant — initialized at link time)\n";
        }

        // Functions
        for (const auto& func : mod.funcs)
            GenFunc(func);
    }

    void GenFunc(const LirFunc& func) {
        if (func.isExtern) {
            NeedExtern(func.name);
            return;
        }

        curFunc_ = func.name;
        PrepassFunc(func);

        // Global / visibility declaration
        if (func.isPublic)
            globals_ << "global " << func.name << "\n";

        TB();
        TC(std::format("── {} ─", func.name));
        TL(func.name);

        // Prologue
        TI("push    rbp");
        TI("mov     rbp, rsp");
        if (frameSize_ > 0)
            TI(std::format("sub     rsp, {}", frameSize_));

        // Spill integer parameter ABI registers to their stack slots
        int intArgIdx = 0, fltArgIdx = 0;
        for (const auto& p : func.params) {
            int sz  = SizeOf(p.type);
            int off = slotMap_.at(p.reg);
            if (IsFloat(p.type)) {
                if (fltArgIdx < 8) {
                    TI(std::format("{:<8}{} [rbp - {}], {}",
                        sz == 4 ? "movss" : "movsd",
                        PtrSize(sz), off,
                        kFltArgRegs[fltArgIdx]));
                    fltArgIdx++;
                }
                // Remaining float params arrive on the stack (System V): skip for now
            } else {
                if (intArgIdx < 6) {
                    TI(std::format("{:<8}{} [rbp - {}], {}",
                        "mov", PtrSize(std::max(sz, 1)), off,
                        kIntArgRegs[intArgIdx]));
                    intArgIdx++;
                }
                // Stack params beyond the 6th: at [rbp + 16 + 8*n]
            }
        }

        // Basic blocks
        for (uint32_t bi = 0; bi < func.blocks.size(); bi++)
            GenBlock(bi, func.blocks[bi], func);

        TB();
    }

    void GenBlock(uint32_t idx, const LirBlock& block, const LirFunc& func) {
        // Emit label for every block after entry
        std::string lbl = BlockLabel(idx, block.label);
        if (idx != 0) {
            TB();
            TL(lbl);
        }

        for (const auto& instr : block.instrs)
            GenInstr(instr, func);

        if (block.term)
            GenTerminator(idx, *block.term, func);
        else
            TI("nop    ; missing terminator");
    }

    // ── Instruction generation ────────────────────────────────────────────────

    void GenInstr(const LirInstr& instr, const LirFunc& /*func*/) {
        switch (instr.op) {

        // ── Const ────────────────────────────────────────────────────────────
        case LirOpcode::Const: {
            if (instr.dst == LirNoReg) break;
            const TypeRef& t = instr.type;
            int sz = SizeOf(t);
            if (t.kind == TypeRef::Kind::Str) {
                std::string lbl = InternStr(instr.strArg);
                TI(std::format("{:<8}rax, {}", "lea", lbl));
            } else if (t.kind == TypeRef::Kind::Float32) {
                std::string lbl = InternF32(instr.strArg);
                TI(std::format("{:<8}xmm0, dword [rel {}]", "movss", lbl));
                TI(std::format("{:<8}dword [rbp - {}], xmm0", "movss", slotMap_.at(instr.dst)));
                break;
            } else if (t.kind == TypeRef::Kind::Float64) {
                std::string lbl = InternF64(instr.strArg);
                TI(std::format("{:<8}xmm0, qword [rel {}]", "movsd", lbl));
                TI(std::format("{:<8}qword [rbp - {}], xmm0", "movsd", slotMap_.at(instr.dst)));
                break;
            } else if (t.kind == TypeRef::Kind::Bool) {
                std::string v = (instr.strArg == "true") ? "1" : "0";
                TI(std::format("{:<8}rax, {}", "mov", v));
            } else {
                // Integer / char: the literal is the numeric value
                TI(std::format("{:<8}rax, {}", "mov", instr.strArg.empty() ? "0" : instr.strArg));
            }
            StoreA(instr.dst, sz > 0 ? t : TypeRef::MakeInt64());
            break;
        }

        // ── Alloca ───────────────────────────────────────────────────────────
        case LirOpcode::Alloca: {
            int32_t dataOff = allocaData_.at(instr.dst);
            TI(std::format("{:<8}rax, [rbp - {}]", "lea", dataOff));
            TI(std::format("{:<8}qword [rbp - {}], rax", "mov", slotMap_.at(instr.dst)));
            break;
        }

        // ── Load ─────────────────────────────────────────────────────────────
        case LirOpcode::Load: {
            const TypeRef& t = instr.type;
            int sz = SizeOf(t);
            if (!instr.strArg.empty()) {
                // Named global / constant
                TI(std::format("{:<8}rax, [rel {}]", "mov", instr.strArg));
            } else {
                // Load through pointer in srcs[0]
                LirReg ptr = instr.srcs[0];
                TI(std::format("{:<8}r10, qword [rbp - {}]", "mov", slotMap_.at(ptr)));
                if (IsFloat(t)) {
                    TI(std::format("{:<8}xmm0, {} [r10]",
                        sz == 4 ? "movss" : "movsd", PtrSize(sz)));
                    TI(std::format("{:<8}{} [rbp - {}], xmm0",
                        sz == 4 ? "movss" : "movsd", PtrSize(sz), slotMap_.at(instr.dst)));
                    break;
                } else if (sz == 8 || sz == 0) {
                    TI(std::format("{:<8}rax, qword [r10]", "mov"));
                } else if (t.IsSigned()) {
                    TI(std::format("{:<8}rax, {} [r10]",
                        sz == 4 ? "movsxd" : "movsx", PtrSize(sz)));
                } else {
                    if (sz == 4)
                        TI(std::format("{:<8}eax, dword [r10]", "mov"));
                    else
                        TI(std::format("{:<8}rax, {} [r10]", "movzx", PtrSize(sz)));
                }
            }
            StoreA(instr.dst, sz > 0 ? t : TypeRef::MakeInt64());
            break;
        }

        // ── Store ────────────────────────────────────────────────────────────
        case LirOpcode::Store: {
            LirReg val = instr.srcs[0];
            LirReg ptr = instr.srcs[1];
            const TypeRef& t = instr.type;
            int sz = SizeOf(t);

            // Load pointer
            TI(std::format("{:<8}r11, qword [rbp - {}]", "mov", slotMap_.at(ptr)));

            if (IsFloat(t)) {
                TI(std::format("{:<8}xmm0, {} [rbp - {}]",
                    sz == 4 ? "movss" : "movsd", PtrSize(sz), slotMap_.at(val)));
                TI(std::format("{:<8}{} [r11], xmm0",
                    sz == 4 ? "movss" : "movsd", PtrSize(sz)));
            } else {
                int store_sz = (sz > 0) ? sz : 8;
                TI(std::format("{:<8}{}, {} [rbp - {}]", "mov",
                    GprA(store_sz), PtrSize(store_sz), slotMap_.at(val)));
                TI(std::format("{:<8}{} [r11], {}", "mov", PtrSize(store_sz), GprA(store_sz)));
            }
            break;
        }

        // ── Arithmetic ───────────────────────────────────────────────────────
        case LirOpcode::Add:
        case LirOpcode::Sub:
        case LirOpcode::And:
        case LirOpcode::Or:
        case LirOpcode::Xor: {
            const TypeRef& t = instr.type;
            if (IsFloat(t)) {
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                std::string_view op;
                bool f32 = (t.kind == TypeRef::Kind::Float32);
                switch (instr.op) {
                    case LirOpcode::Add: op = f32 ? "addss" : "addsd"; break;
                    case LirOpcode::Sub: op = f32 ? "subss" : "subsd"; break;
                    default:             op = f32 ? "addss" : "addsd"; break;
                }
                TI(std::format("{:<8}xmm0, xmm1", op));
                StoreA(instr.dst, t);
            } else {
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                std::string_view op;
                switch (instr.op) {
                    case LirOpcode::Add: op = "add"; break;
                    case LirOpcode::Sub: op = "sub"; break;
                    case LirOpcode::And: op = "and"; break;
                    case LirOpcode::Or:  op = "or";  break;
                    case LirOpcode::Xor: op = "xor"; break;
                    default:             op = "add"; break;
                }
                TI(std::format("{:<8}rax, r10", op));
                StoreA(instr.dst, t);
            }
            break;
        }

        case LirOpcode::Mul: {
            const TypeRef& t = instr.type;
            if (IsFloat(t)) {
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                TI(t.kind == TypeRef::Kind::Float32 ? "mulss   xmm0, xmm1" : "mulsd   xmm0, xmm1");
                StoreA(instr.dst, t);
            } else {
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                TI("imul    rax, r10");
                StoreA(instr.dst, t);
            }
            break;
        }

        case LirOpcode::Div:
        case LirOpcode::Mod: {
            const TypeRef& t = instr.type;
            if (IsFloat(t)) {
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                TI(t.kind == TypeRef::Kind::Float32 ? "divss   xmm0, xmm1" : "divsd   xmm0, xmm1");
                StoreA(instr.dst, t);
            } else {
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                // idiv uses rdx:rax; result in rax (quotient) or rdx (remainder)
                if (t.IsSigned()) {
                    TI("cqo");               // sign-extend rax → rdx:rax
                    TI("idiv    r10");
                } else {
                    TI("xor     rdx, rdx");  // zero-extend rax
                    TI("div     r10");
                }
                if (instr.op == LirOpcode::Mod)
                    TI("mov     rax, rdx");  // remainder is in rdx
                StoreA(instr.dst, t);
            }
            break;
        }

        case LirOpcode::Pow: {
            // Emit a call to __rux_pow (integer) or pow (float from libm)
            const TypeRef& t = instr.type;
            if (IsFloat(t)) {
                NeedExtern("pow");
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                TI("movsd   xmm0, xmm0");
                TI("movsd   xmm1, xmm1");
                TI("call    pow");
            } else {
                NeedExtern("__rux_ipow");
                LoadA(instr.srcs[0], t);
                LoadB(instr.srcs[1], t);
                TI("mov     rdi, rax");
                TI("mov     rsi, r10");
                TI("call    __rux_ipow");
            }
            StoreA(instr.dst, t);
            break;
        }

        // ── Shift ────────────────────────────────────────────────────────────
        case LirOpcode::Shl:
        case LirOpcode::Shr: {
            const TypeRef& t = instr.type;
            LoadA(instr.srcs[0], t);
            // Shift count must be in cl
            TI(std::format("{:<8}r11, qword [rbp - {}]", "mov", slotMap_.at(instr.srcs[1])));
            TI("mov     rcx, r11");
            bool isShr = (instr.op == LirOpcode::Shr);
            if (isShr && t.IsSigned())
                TI("sar     rax, cl");
            else if (isShr)
                TI("shr     rax, cl");
            else
                TI("shl     rax, cl");
            StoreA(instr.dst, t);
            break;
        }

        // ── Unary ────────────────────────────────────────────────────────────
        case LirOpcode::Neg: {
            const TypeRef& t = instr.type;
            if (IsFloat(t)) {
                LoadA(instr.srcs[0], t);
                // Negate by XOR with sign bit
                if (t.kind == TypeRef::Kind::Float32) {
                    std::string lbl = InternF32("0x80000000");
                    TI(std::format("movss   xmm1, dword [rel {}]", lbl));
                    TI("xorps   xmm0, xmm1");
                } else {
                    std::string lbl = InternF64("0x8000000000000000");
                    TI(std::format("movsd   xmm1, qword [rel {}]", lbl));
                    TI("xorpd   xmm0, xmm1");
                }
                StoreA(instr.dst, t);
            } else {
                LoadA(instr.srcs[0], t);
                TI("neg     rax");
                StoreA(instr.dst, t);
            }
            break;
        }

        case LirOpcode::Not: {
            // Logical not: result = (operand == 0) ? 1 : 0
            const TypeRef& t = instr.type;
            LoadA(instr.srcs[0], t);
            TI("test    rax, rax");
            TI("setz    al");
            TI("movzx   rax, al");
            StoreA(instr.dst, TypeRef::MakeBool());
            break;
        }

        case LirOpcode::BitNot: {
            const TypeRef& t = instr.type;
            LoadA(instr.srcs[0], t);
            TI("not     rax");
            StoreA(instr.dst, t);
            break;
        }

        // ── Comparisons ──────────────────────────────────────────────────────
        case LirOpcode::CmpEq: case LirOpcode::CmpNe:
        case LirOpcode::CmpLt: case LirOpcode::CmpLe:
        case LirOpcode::CmpGt: case LirOpcode::CmpGe: {
            const TypeRef& lhsT = regTypes_.count(instr.srcs[0])
                                   ? regTypes_.at(instr.srcs[0]) : instr.type;
            LoadA(instr.srcs[0], lhsT);
            LoadB(instr.srcs[1], lhsT);
            if (IsFloat(lhsT)) {
                TI(lhsT.kind == TypeRef::Kind::Float32
                    ? "ucomiss xmm0, xmm1" : "ucomisd xmm0, xmm1");
                std::string_view set;
                switch (instr.op) {
                    case LirOpcode::CmpEq: set = "sete";  break;
                    case LirOpcode::CmpNe: set = "setne"; break;
                    case LirOpcode::CmpLt: set = "setb";  break;
                    case LirOpcode::CmpLe: set = "setbe"; break;
                    case LirOpcode::CmpGt: set = "seta";  break;
                    default:               set = "setae"; break;
                }
                TI(std::format("{:<8}al", set));
            } else {
                TI("cmp     rax, r10");
                std::string_view set;
                bool sig = lhsT.IsSigned();
                switch (instr.op) {
                    case LirOpcode::CmpEq: set = "sete";                    break;
                    case LirOpcode::CmpNe: set = "setne";                   break;
                    case LirOpcode::CmpLt: set = sig ? "setl"  : "setb";   break;
                    case LirOpcode::CmpLe: set = sig ? "setle" : "setbe";  break;
                    case LirOpcode::CmpGt: set = sig ? "setg"  : "seta";   break;
                    default:               set = sig ? "setge" : "setae";  break;
                }
                TI(std::format("{:<8}al", set));
            }
            TI("movzx   rax, al");
            StoreA(instr.dst, TypeRef::MakeBool());
            break;
        }

        // ── Cast ─────────────────────────────────────────────────────────────
        case LirOpcode::Cast: {
            const TypeRef& dst_t = instr.type;
            TypeRef src_t;
            // strArg holds the source type string; try to reconstruct enough info
            // by looking up the register type
            if (regTypes_.count(instr.srcs[0]))
                src_t = regTypes_.at(instr.srcs[0]);
            else
                src_t = dst_t;

            bool srcFloat = IsFloat(src_t);
            bool dstFloat = IsFloat(dst_t);

            LoadA(instr.srcs[0], src_t);

            if (!srcFloat && !dstFloat) {
                // int → int: sign/zero extend is already done by LoadA;
                // for narrowing, the lower bits in rax are already correct.
                // Nothing extra needed for most cases.
            } else if (srcFloat && !dstFloat) {
                // float → int
                bool f32src = (src_t.kind == TypeRef::Kind::Float32);
                bool signed_ = dst_t.IsSigned();
                if (signed_)
                    TI(f32src ? "cvttss2si rax, xmm0" : "cvttsd2si rax, xmm0");
                else {
                    // Unsigned: no direct instruction; use signed then re-interpret
                    TI(f32src ? "cvttss2si rax, xmm0" : "cvttsd2si rax, xmm0");
                }
            } else if (!srcFloat && dstFloat) {
                // int → float
                bool f32dst = (dst_t.kind == TypeRef::Kind::Float32);
                TI(f32dst ? "cvtsi2ss  xmm0, rax" : "cvtsi2sd  xmm0, rax");
            } else {
                // float → float
                bool f32src = (src_t.kind == TypeRef::Kind::Float32);
                bool f32dst = (dst_t.kind == TypeRef::Kind::Float32);
                if (f32src && !f32dst)
                    TI("cvtss2sd  xmm0, xmm0");
                else if (!f32src && f32dst)
                    TI("cvtsd2ss  xmm0, xmm0");
            }
            StoreA(instr.dst, dst_t);
            break;
        }

        // ── Call ─────────────────────────────────────────────────────────────
        case LirOpcode::Call: {
            EmitCall(instr.strArg, instr.srcs, instr.dst, instr.type, /*indirect=*/false);
            break;
        }

        case LirOpcode::CallIndirect: {
            // strArg empty; srcs[0] = callee, srcs[1..] = args
            EmitCallIndirect(instr.srcs, instr.dst, instr.type);
            break;
        }

        // ── FieldPtr ─────────────────────────────────────────────────────────
        case LirOpcode::FieldPtr: {
            LirReg base = instr.srcs[0];
            // Load base pointer
            TI(std::format("{:<8}rax, qword [rbp - {}]", "mov", slotMap_.at(base)));

            // Compute field offset using struct layout
            int fieldOff = ResolveFieldOffset(base, instr.strArg);
            if (fieldOff != 0)
                TI(std::format("{:<8}rax, [rax + {}]", "lea", fieldOff));
            // else pointer is already at the field start
            TI(std::format("{:<8}qword [rbp - {}], rax", "mov", slotMap_.at(instr.dst)));
            break;
        }

        // ── IndexPtr ─────────────────────────────────────────────────────────
        case LirOpcode::IndexPtr: {
            LirReg base = instr.srcs[0];
            LirReg idx  = instr.srcs[1];
            int elemSz = (instr.type.kind == TypeRef::Kind::Pointer && !instr.type.inner.empty())
                         ? SizeOf(instr.type.inner[0]) : 8;
            if (elemSz < 1) elemSz = 1;

            TI(std::format("{:<8}rax, qword [rbp - {}]", "mov", slotMap_.at(base)));
            TI(std::format("{:<8}r10, qword [rbp - {}]", "mov", slotMap_.at(idx)));
            TI(std::format("{:<8}r11, r10, {}", "imul", elemSz));
            TI("add     rax, r11");
            TI(std::format("{:<8}qword [rbp - {}], rax", "mov", slotMap_.at(instr.dst)));
            break;
        }

        // ── Phi ──────────────────────────────────────────────────────────────
        case LirOpcode::Phi:
            // Phi moves are emitted by predecessors before their terminators.
            // The phi dst slot is already allocated; nothing to emit here.
            break;

        default:
            TC(std::format("TODO: opcode {}", static_cast<int>(instr.op)));
            break;
        }
    }

    // Field offset resolution via regTypes_ + layouts_
    int ResolveFieldOffset(LirReg base, const std::string& fieldName) {
        auto typeIt = regTypes_.find(base);
        if (typeIt == regTypes_.end()) return 0;
        const TypeRef& ptrType = typeIt->second;
        if (ptrType.kind != TypeRef::Kind::Pointer || ptrType.inner.empty()) return 0;
        const TypeRef& inner = ptrType.inner[0];
        if (inner.kind != TypeRef::Kind::Named) return 0;
        auto layIt = layouts_.find(inner.name);
        if (layIt == layouts_.end()) return 0;
        for (const auto& f : layIt->second.fields)
            if (f.name == fieldName) return f.offset;
        return 0;
    }

    // ── Call emission ─────────────────────────────────────────────────────────

    void EmitCall(const std::string& callee, const std::vector<LirReg>& args,
                  LirReg dst, const TypeRef& retType, bool /*indirect*/) {
        // Set up arguments into ABI registers
        int intIdx = 0, fltIdx = 0;
        for (LirReg arg : args) {
            TypeRef at = regTypes_.count(arg) ? regTypes_.at(arg) : TypeRef::MakeInt64();
            if (IsFloat(at)) {
                if (fltIdx < 8) {
                    int sz = SizeOf(at);
                    TI(std::format("{:<8}{}, {} [rbp - {}]",
                        sz == 4 ? "movss" : "movsd",
                        kFltArgRegs[fltIdx], PtrSize(sz), slotMap_.at(arg)));
                    fltIdx++;
                }
            } else {
                if (intIdx < 6) {
                    int sz = std::max(SizeOf(at), 1);
                    TI(std::format("{:<8}{}, {} [rbp - {}]",
                        "mov", kIntArgRegs[intIdx], PtrSize(sz), slotMap_.at(arg)));
                    intIdx++;
                }
            }
        }
        // Stack is already 16-byte aligned: prologue sub rsp,frameSize ensures
        // rsp ≡ 8 (mod 16) which the ABI requires before a call instruction.
        TI(std::format("{:<8}{}", "call", callee));
        if (dst != LirNoReg && !retType.IsVoid())
            StoreA(dst, retType);
    }

    void EmitCallIndirect(const std::vector<LirReg>& srcs, LirReg dst, const TypeRef& retType) {
        if (srcs.empty()) return;
        LirReg callee = srcs[0];
        std::vector<LirReg> args(srcs.begin() + 1, srcs.end());
        // Load callee pointer into r10 before setting up args
        TI(std::format("{:<8}r10, qword [rbp - {}]", "mov", slotMap_.at(callee)));
        EmitCallArgs(args);
        TI("call    r10");
        if (dst != LirNoReg && !retType.IsVoid())
            StoreA(dst, retType);
    }

    void EmitCallArgs(const std::vector<LirReg>& args) {
        int intIdx = 0, fltIdx = 0;
        for (LirReg arg : args) {
            TypeRef at = regTypes_.count(arg) ? regTypes_.at(arg) : TypeRef::MakeInt64();
            if (IsFloat(at)) {
                if (fltIdx < 8) {
                    int sz = SizeOf(at);
                    TI(std::format("{:<8}{}, {} [rbp - {}]",
                        sz == 4 ? "movss" : "movsd",
                        kFltArgRegs[fltIdx], PtrSize(sz), slotMap_.at(arg)));
                    fltIdx++;
                }
            } else {
                if (intIdx < 6) {
                    int sz = std::max(SizeOf(at), 1);
                    TI(std::format("{:<8}{}, {} [rbp - {}]",
                        "mov", kIntArgRegs[intIdx], PtrSize(sz), slotMap_.at(arg)));
                    intIdx++;
                }
            }
        }
    }

    // ── Terminator generation ─────────────────────────────────────────────────

    void GenTerminator(uint32_t blockIdx, const LirTerminator& term, const LirFunc& func) {
        switch (term.kind) {

        case LirTermKind::Jump: {
            EmitPhiMoves(blockIdx, term.trueTarget);
            TI(std::format("{:<8}{}", "jmp",
                BlockLabel(term.trueTarget, func.blocks[term.trueTarget].label)));
            break;
        }

        case LirTermKind::Branch: {
            // Load condition
            TypeRef condT = regTypes_.count(term.cond) ? regTypes_.at(term.cond) : TypeRef::MakeBool();
            TI(std::format("{:<8}rax, qword [rbp - {}]", "mov", slotMap_.at(term.cond)));
            TI("test    rax, rax");

            std::string trueLabel  = BlockLabel(term.trueTarget,  func.blocks[term.trueTarget].label);
            std::string falseLabel = BlockLabel(term.falseTarget, func.blocks[term.falseTarget].label);

            // Check whether either branch needs phi moves
            bool truePhi  = HasPhiMoves(blockIdx, term.trueTarget);
            bool falsePhi = HasPhiMoves(blockIdx, term.falseTarget);

            if (!truePhi && !falsePhi) {
                TI(std::format("{:<8}{}", "jz", falseLabel));
                TI(std::format("{:<8}{}", "jmp", trueLabel));
            } else {
                // Use intermediate labels so each path has room for phi moves
                std::string trampTrue  = std::format(".{}_br{}_t", curFunc_, blockIdx);
                std::string trampFalse = std::format(".{}_br{}_f", curFunc_, blockIdx);
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
                LoadA(*term.retVal, term.retType);
                // Result already in rax or xmm0 — do not overwrite
            }
            EmitEpilogue();
            break;
        }

        case LirTermKind::Switch: {
            TypeRef condT = regTypes_.count(term.cond) ? regTypes_.at(term.cond) : TypeRef::MakeInt64();
            TI(std::format("{:<8}rax, qword [rbp - {}]", "mov", slotMap_.at(term.cond)));
            for (const auto& c : term.cases) {
                TI(std::format("{:<8}rax, {}", "cmp", c.value));
                std::string lbl = BlockLabel(c.target, func.blocks[c.target].label);
                TI(std::format("{:<8}{}", "je", lbl));
            }
            EmitPhiMoves(blockIdx, term.defaultTarget);
            TI(std::format("{:<8}{}",
                "jmp", BlockLabel(term.defaultTarget, func.blocks[term.defaultTarget].label)));
            break;
        }
        }
    }

    bool HasPhiMoves(uint32_t from, uint32_t to) const {
        auto it = phiMoves_.find(from);
        if (it == phiMoves_.end()) return false;
        return it->second.count(to) != 0;
    }

    void EmitEpilogue() {
        TI("leave");
        TI("ret");
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// AsmGen::Generate
// ─────────────────────────────────────────────────────────────────────────────

std::string AsmGen::Generate() {
    BuildLayouts();

    for (const auto& mod : pkg_.modules)
        GenModule(mod);

    std::ostringstream out;
    out << "; Generated by Rux Compiler\n";
    out << "; Target:  x86-64  (System V AMD64 ABI, NASM syntax)\n";
    out << "; Calling: rdi/rsi/rdx/rcx/r8/r9 (int args), xmm0-7 (float args)\n";
    out << "; Scratch: r10, r11 (caller-saved)\n";
    out << "\n";
    out << "bits 64\n\n";

    std::string ext = externs_.str();
    if (!ext.empty()) { out << ext << "\n"; }

    std::string glb = globals_.str();
    if (!glb.empty()) { out << glb << "\n"; }

    std::string rod = rodata_.str();
    if (!rod.empty()) { out << "section .rodata\n" << rod << "\n"; }

    std::string dat = data_.str();
    if (!dat.empty()) { out << "section .data\n" << dat << "\n"; }

    std::string cod = text_.str();
    if (!cod.empty()) { out << "section .text\n" << cod; }

    return out.str();
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

Asm::Asm(LirPackage package) : lir_(std::move(package)) {}

std::string Asm::Generate() {
    AsmGen gen(lir_);
    return gen.Generate();
}

bool Asm::Emit(const LirPackage& package, const std::filesystem::path& path) {
    AsmGen gen(package);
    std::string text = gen.Generate();
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f) return false;
    f << text;
    return f.good();
}

} // namespace Rux
