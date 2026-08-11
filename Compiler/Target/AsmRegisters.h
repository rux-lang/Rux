#pragma once

// Register names an `asm func` body may spell, one table per architecture.
//
// Two consumers share these tables: the parser, which only asks whether an
// identifier names a register at all so it can tell a register operand from a
// symbol, and the per-architecture assembler, which reads the decoded fields to
// encode it. The table is selected by the architecture being compiled for, so
// the same identifier may be a register in one body and a symbol in another —
// `sp` is a register on both architectures, `rax` only on x86-64.

#include "Target/Target.h"

#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Rux {
// Which register file a name belongs to. `Vector` covers the x86-64 XMM file
// and the AArch64 SIMD and floating-point file alike: on both, a name from it
// is spelled where an instruction expects a floating-point operand.
enum class AsmRegFile : std::uint8_t {
    None,
    Gpr,
    Vector,
};

// A decoded register name: which file it belongs to, its number, the width the
// name selects, and the per-architecture quirks that affect encoding.
struct AsmRegInfo {
    bool valid = false;
    Target::Arch arch = Target::Arch::Unknown;
    AsmRegFile file = AsmRegFile::None;

    // x86-64: 0..15. AArch64: 0..31, where 31 is the zero register unless
    // `stackPointer` says the name is the other reading of that code.
    int code = 0;

    // Width in bytes the name selects. General purpose: 1/2/4/8. Vector:
    // x86-64 XMM is always 16; AArch64 B/H/S/D/Q views are 1/2/4/8/16.
    int size = 0;

    // x86-64 only: spl/bpl/sil/dil need a REX prefix to be reachable, and
    // ah/ch/dh/bh are the legacy high-byte registers.
    bool rexRequired = false;
    bool high8 = false;

    // AArch64 only: SP and WSP, the reading of code 31 that is not the zero
    // register. Nothing in the encoding distinguishes the two, so the reading
    // has to travel with the name.
    bool stackPointer = false;

    [[nodiscard]] bool IsVector() const noexcept {
        return file == AsmRegFile::Vector;
    }
};

// Transparent hashing so a table keyed by owned names — the AArch64 one builds
// `x0`, `w0`, `d0` and their kin rather than listing them — is still looked up
// with the `std::string_view` the parser and the assembler hold.
struct AsmNameHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(const std::string_view name) const noexcept {
        return std::hash<std::string_view>{}(name);
    }
};

using AsmRegisterMap = std::unordered_map<std::string, AsmRegInfo, AsmNameHash, std::equal_to<>>;

// x86-64: the general-purpose file in all four widths, its legacy high-byte
// names, and the XMM (SSE) file.
inline const AsmRegisterMap &X86_64RegisterTable() {
    static const AsmRegisterMap table = [] {
        AsmRegisterMap m;
        constexpr auto arch = Target::Arch::X86_64;
        static constexpr const char *r64[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                                "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
        static constexpr const char *r32[16] = {"eax", "ecx", "edx",  "ebx",  "esp",  "ebp",  "esi",  "edi",
                                                "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
        static constexpr const char *r16[16] = {"ax",  "cx",  "dx",   "bx",   "sp",   "bp",   "si",   "di",
                                                "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
        static constexpr const char *r8l[16] = {"al",  "cl",  "dl",   "bl",   "spl",  "bpl",  "sil",  "dil",
                                                "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};
        for (int i = 0; i < 16; ++i) {
            m[r64[i]] = AsmRegInfo{.valid = true, .arch = arch, .file = AsmRegFile::Gpr, .code = i, .size = 8};
            m[r32[i]] = AsmRegInfo{.valid = true, .arch = arch, .file = AsmRegFile::Gpr, .code = i, .size = 4};
            m[r16[i]] = AsmRegInfo{.valid = true, .arch = arch, .file = AsmRegFile::Gpr, .code = i, .size = 2};
            // spl/bpl/sil/dil (codes 4..7) require a REX prefix to be reachable.
            m[r8l[i]] = AsmRegInfo{.valid = true,
                                   .arch = arch,
                                   .file = AsmRegFile::Gpr,
                                   .code = i,
                                   .size = 1,
                                   .rexRequired = i >= 4 && i <= 7};
            m[std::format("xmm{}", i)] =
                AsmRegInfo{.valid = true, .arch = arch, .file = AsmRegFile::Vector, .code = i, .size = 16};
        }
        static constexpr const char *high8[4] = {"ah", "ch", "dh", "bh"};
        for (int i = 0; i < 4; ++i) {
            m[high8[i]] = AsmRegInfo{
                .valid = true, .arch = arch, .file = AsmRegFile::Gpr, .code = i + 4, .size = 1, .high8 = true};
        }
        return m;
    }();
    return table;
}

// AArch64: X0-X30 and W0-W30, both readings of code 31, and the scalar views of
// the SIMD file. The bare `V` names are deliberately absent: they are only ever
// written with an element qualifier (`V0.16B`), which the scalar subset the
// encoder covers has no use for, so a body naming one is read as a symbol here
// and reported by the assembler rather than silently becoming a register.
inline const AsmRegisterMap &AArch64RegisterTable() {
    static const AsmRegisterMap table = [] {
        AsmRegisterMap m;
        constexpr auto arch = Target::Arch::AArch64;
        const auto gpr = [&m](std::string name, const int code, const int size, const bool stackPointer = false) {
            m[std::move(name)] = AsmRegInfo{.valid = true,
                                            .arch = arch,
                                            .file = AsmRegFile::Gpr,
                                            .code = code,
                                            .size = size,
                                            .stackPointer = stackPointer};
        };
        for (int i = 0; i <= 30; ++i) {
            gpr(std::format("x{}", i), i, 8);
            gpr(std::format("w{}", i), i, 4);
        }
        // The two readings of code 31. Which one a name means is not in the
        // encoding, so it travels here; an instruction accepts one or the other.
        gpr("xzr", 31, 8);
        gpr("wzr", 31, 4);
        gpr("sp", 31, 8, true);
        gpr("wsp", 31, 4, true);
        // The two register names AAPCS64 gives a role rather than a number.
        gpr("fp", 29, 8);
        gpr("lr", 30, 8);

        // Scalar views of V0-V31: the letter selects how many bytes of the
        // register the instruction reads and writes.
        static constexpr struct {
            char letter;
            int size;
        } views[5] = {{'b', 1}, {'h', 2}, {'s', 4}, {'d', 8}, {'q', 16}};
        for (const auto &[letter, size] : views) {
            for (int i = 0; i <= 31; ++i) {
                m[std::format("{}{}", letter, i)] =
                    AsmRegInfo{.valid = true, .arch = arch, .file = AsmRegFile::Vector, .code = i, .size = size};
            }
        }
        return m;
    }();
    return table;
}

// The table for `arch`, or an empty one for an architecture with no inline
// assembly support.
[[nodiscard]] inline const AsmRegisterMap &RegisterTable(const Target::Arch arch) {
    static const AsmRegisterMap none;
    switch (arch) {
    case Target::Arch::X86_64:
        return X86_64RegisterTable();
    case Target::Arch::AArch64:
        return AArch64RegisterTable();
    default:
        return none;
    }
}

// Decode `name` — already lower-cased — as a register of `arch`. An
// unrecognized name comes back invalid rather than as a diagnostic: the parser
// reads it as a symbol, and only the assembler knows which of the two the
// instruction wanted.
[[nodiscard]] inline AsmRegInfo LookupRegister(const Target::Arch arch, const std::string_view name) {
    const auto &table = RegisterTable(arch);
    if (const auto it = table.find(name); it != table.end()) {
        return it->second;
    }
    return {};
}

[[nodiscard]] inline bool IsRegisterName(const Target::Arch arch, const std::string_view name) {
    return LookupRegister(arch, name).valid;
}
} // namespace Rux
