#pragma once

// Structured representation of a single x86-64 instruction inside an
// `asm func` body. Produced by the parser, threaded unchanged through HIR and
// LIR, and encoded to machine code by the x86-64 assembler in
// CodeGen/X86_64/Assembler.cpp.

#include "Source/SourceLocation.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Rux {
// Decoded x86-64 register: its number (0..15), byte width, and the quirks that
// affect REX encoding. Covers both the general-purpose and the XMM (SSE) files.
// Shared by the parser (to classify an identifier as a register) and the
// assembler (to encode it).
struct X64RegInfo {
    bool valid = false;
    int code = 0;             // 0..15
    int size = 0;             // GP: 1/2/4/8 bytes. XMM: 16.
    bool rexRequired = false; // spl/bpl/sil/dil: need a REX prefix to select
    bool high8 = false;       // ah/ch/dh/bh: legacy high-byte registers
    bool isXmm = false;       // xmm0..xmm15: an SSE register, not a GP register
};

inline const std::unordered_map<std::string_view, X64RegInfo> &X64RegisterTable() {
    static const std::unordered_map<std::string_view, X64RegInfo> table = [] {
        std::unordered_map<std::string_view, X64RegInfo> m;
        static constexpr const char *r64[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                                "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
        static constexpr const char *r32[16] = {"eax", "ecx", "edx",  "ebx",  "esp",  "ebp",  "esi",  "edi",
                                                "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
        static constexpr const char *r16[16] = {"ax",  "cx",  "dx",   "bx",   "sp",   "bp",   "si",   "di",
                                                "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
        static constexpr const char *r8l[16] = {"al",  "cl",  "dl",   "bl",   "spl",  "bpl",  "sil",  "dil",
                                                "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};
        for (int i = 0; i < 16; ++i) {
            m[r64[i]] = X64RegInfo{true, i, 8, false, false};
            m[r32[i]] = X64RegInfo{true, i, 4, false, false};
            m[r16[i]] = X64RegInfo{true, i, 2, false, false};
            // spl/bpl/sil/dil (codes 4..7) require a REX prefix to be reachable.
            m[r8l[i]] = X64RegInfo{true, i, 1, i >= 4 && i <= 7, false};
        }
        m["ah"] = X64RegInfo{true, 4, 1, false, true};
        m["ch"] = X64RegInfo{true, 5, 1, false, true};
        m["dh"] = X64RegInfo{true, 6, 1, false, true};
        m["bh"] = X64RegInfo{true, 7, 1, false, true};
        // XMM (SSE) registers: xmm0..xmm15.
        static constexpr const char *xmm[16] = {"xmm0", "xmm1", "xmm2",  "xmm3",  "xmm4",  "xmm5",  "xmm6",  "xmm7",
                                                "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"};
        for (int i = 0; i < 16; ++i) {
            m[xmm[i]] = X64RegInfo{true, i, 16, false, false, true};
        }
        return m;
    }();
    return table;
}

inline X64RegInfo LookupX64Reg(std::string_view name) {
    const auto &t = X64RegisterTable();
    if (const auto it = t.find(name); it != t.end()) {
        return it->second;
    }
    return {};
}

inline bool IsX64RegisterName(std::string_view name) {
    return LookupX64Reg(name).valid;
}

// A single operand of an inline-assembly instruction.
struct AsmOperand {
    enum class Kind : std::uint8_t {
        None,
        Reg, // a register:            rax, ecx, r8, ...
        Imm, // an integer immediate:  42, -1, 0xFF
        Mem, // a memory reference:    [rbp + rax*4 - 8]
        Sym, // a symbol / label name: some_func, .loop
    };

    Kind kind = Kind::None;
    SourceLocation location;

    // Reg: register name (lower-cased). Sym: symbol / label name.
    std::string name;

    // Imm: the immediate value. Mem: the displacement.
    std::int64_t imm = 0;

    // Memory operand: [memBase + memIndex*memScale + imm (+ memSym)].
    std::string memBase;  // may be empty
    std::string memIndex; // may be empty
    int memScale = 1;     // 1, 2, 4 or 8
    int memSize = 0;      // size hint from byte/word/dword/qword (1/2/4/8); 0 = unspecified
    std::string memSym;   // symbol referenced inside the brackets (rip-relative)
};

// A single instruction (or, when `labelDef` is set, a label definition).
struct AsmInstr {
    SourceLocation location;
    std::string mnemonic; // lower-cased; empty when this entry is a label
    std::vector<AsmOperand> operands;
    std::string labelDef; // non-empty => this entry defines a label
};
} // namespace Rux
