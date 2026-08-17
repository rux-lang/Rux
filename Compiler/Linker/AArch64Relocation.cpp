#include "Linker/AArch64Relocation.h"

#include "Object/Rcu/Rcu.h"

#include <format>

namespace Rux {
namespace {
/// Reads back a little-endian instruction word a relocation is about to rewrite a field of.
[[nodiscard]] uint32_t ReadU32(const Buf &buf, const size_t offset) {
    return static_cast<uint32_t>(buf[offset]) | static_cast<uint32_t>(buf[offset + 1]) << 8U |
           static_cast<uint32_t>(buf[offset + 2]) << 16U | static_cast<uint32_t>(buf[offset + 3]) << 24U;
}

/// True when `value` fits in `bits` bits read as two's complement.
[[nodiscard]] bool FitsSigned(const int64_t value, const unsigned bits) {
    const int64_t limit = int64_t{1} << (bits - 1);
    return value >= -limit && value < limit;
}

/// Replaces `width` bits of `word` starting at `lsb` with `value`.
[[nodiscard]] uint32_t WithField(const uint32_t word, const unsigned lsb, const unsigned width, const uint32_t value) {
    const uint32_t mask = width == 32 ? ~0U : ((1U << width) - 1U) << lsb;
    return (word & ~mask) | ((value << lsb) & mask);
}
} // namespace

bool ApplyAArch64Relocation(Buf &buf, const size_t patchAt, const uint16_t type, const uint64_t targetVA,
                            const int64_t addend, const uint64_t siteVA, const std::string_view symbolName,
                            const std::string_view writerName, std::string &error) {
    const uint64_t value = targetVA + static_cast<uint64_t>(addend);
    const auto delta = static_cast<int64_t>(value - siteVA);
    const auto fail = [&] {
        std::string_view form = "address";
        if (type == RcuRelType::AArch64Call26 || type == RcuRelType::AArch64Jump26) {
            form = "branch";
        }
        else if (type == RcuRelType::AArch64CondBr19) {
            form = "conditional branch";
        }
        else if (type == RcuRelType::AArch64TstBr14) {
            form = "test-and-branch";
        }
        else if (type == RcuRelType::AArch64AdrPrelPgHi21) {
            form = "page-address";
        }
        else if (type == RcuRelType::AArch64Prel32) {
            form = "32-bit PC-relative";
        }
        error = std::format("AArch64 {} relocation to '{}' is out of range", form, symbolName);
        return false;
    };

    if (type == RcuRelType::Abs64 || type == RcuRelType::AArch64Prel64) {
        if (patchAt + 8 > buf.size()) {
            return true;
        }
        Patch64(buf, patchAt, type == RcuRelType::Abs64 ? value : static_cast<uint64_t>(delta));
        return true;
    }
    if (type == RcuRelType::Abs32 || type == RcuRelType::AArch64Prel32) {
        if (patchAt + 4 > buf.size()) {
            return true;
        }
        if (type == RcuRelType::AArch64Prel32 && !FitsSigned(delta, 32)) {
            return fail();
        }
        Patch32(buf, patchAt, type == RcuRelType::Abs32 ? static_cast<uint32_t>(value) : static_cast<uint32_t>(delta));
        return true;
    }

    if (patchAt + 4 > buf.size()) {
        return true;
    }
    const uint32_t word = ReadU32(buf, patchAt);
    const auto patch = [&](const uint32_t patched) {
        Patch32(buf, patchAt, patched);
        return true;
    };
    switch (type) {
    case RcuRelType::AArch64Call26:
    case RcuRelType::AArch64Jump26:
        if (delta % 4 != 0 || !FitsSigned(delta >> 2, 26)) {
            return fail();
        }
        return patch(WithField(word, 0, 26, static_cast<uint32_t>(delta >> 2)));
    case RcuRelType::AArch64CondBr19:
        if (delta % 4 != 0 || !FitsSigned(delta >> 2, 19)) {
            return fail();
        }
        return patch(WithField(word, 5, 19, static_cast<uint32_t>(delta >> 2)));
    case RcuRelType::AArch64TstBr14:
        if (delta % 4 != 0 || !FitsSigned(delta >> 2, 14)) {
            return fail();
        }
        return patch(WithField(word, 5, 14, static_cast<uint32_t>(delta >> 2)));
    case RcuRelType::AArch64AdrPrelPgHi21: {
        // ADRP names the 4 KB page the symbol sits on, relative to the page
        // the instruction itself sits on, and the 21-bit result is split with
        // its low two bits high in the word.
        const int64_t pages =
            (static_cast<int64_t>(value & ~0xFFFull) - static_cast<int64_t>(siteVA & ~0xFFFull)) >> 12;
        if (!FitsSigned(pages, 21)) {
            return fail();
        }
        const auto immediate = static_cast<uint32_t>(pages) & 0x1FFFFFU;
        return patch(WithField(WithField(word, 29, 2, immediate & 3U), 5, 19, immediate >> 2U));
    }
    case RcuRelType::AArch64AddAbsLo12Nc:
        return patch(WithField(word, 10, 12, static_cast<uint32_t>(value & 0xFFFU)));
    case RcuRelType::AArch64LdstAbsLo12Nc: {
        const unsigned scale = AArch64LoadStoreScale(word);
        if ((value & ((1ull << scale) - 1)) != 0) {
            error = std::format("AArch64 load/store relocation to '{}' is not aligned to its access width", symbolName);
            return false;
        }
        return patch(WithField(word, 10, 12, static_cast<uint32_t>((value & 0xFFFU) >> scale)));
    }
    case RcuRelType::AArch64MovwUabsG0:
    case RcuRelType::AArch64MovwUabsG1:
    case RcuRelType::AArch64MovwUabsG2:
    case RcuRelType::AArch64MovwUabsG3: {
        // The four together carry the whole 64-bit value, one halfword each,
        // so no single one checks for the bits the others take.
        const unsigned shift = 16U * (type - RcuRelType::AArch64MovwUabsG0);
        return patch(WithField(word, 5, 16, static_cast<uint32_t>(value >> shift & 0xFFFFU)));
    }
    default:
        error = std::format("relocation {} against '{}' is not supported by the {}", RcuRelTypeName(type), symbolName,
                            writerName);
        return false;
    }
}
} // namespace Rux
