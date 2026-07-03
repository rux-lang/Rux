#pragma once

// Byte-buffer helpers shared by the PE, ELF, and Mach-O object writers.

#include <cstdint>
#include <cstring>
#include <vector>

#include "Linker/Linker.h"

namespace Rux {

using Buf = std::vector<uint8_t>;

inline void WriteU8(Buf &b, uint8_t v) {
    b.push_back(v);
}

inline void WriteU16(Buf &b, uint16_t v) {
    b.push_back(v & 0xFF);
    b.push_back(v >> 8);
}

inline void WriteU32(Buf &b, uint32_t v) {
    b.push_back(v & 0xFF);
    b.push_back((v >> 8) & 0xFF);
    b.push_back((v >> 16) & 0xFF);
    b.push_back((v >> 24) & 0xFF);
}

inline void WriteU64(Buf &b, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        b.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
}

inline void WriteZeros(Buf &b, size_t n) {
    b.insert(b.end(), n, 0);
}

inline void WriteCStr(Buf &b, const char *s) {
    while (*s) {
        b.push_back(*s++);
    }
    b.push_back(0);
}

inline void WriteName8(Buf &b, const char *s) {
    size_t len = std::strlen(s);
    for (size_t i = 0; i < 8; ++i) {
        b.push_back(i < len ? static_cast<uint8_t>(s[i]) : 0);
    }
}

inline void PadTo(Buf &b, size_t align, uint8_t fill = 0) {
    while (b.size() % align) {
        b.push_back(fill);
    }
}

inline uint32_t AlignUp(uint32_t v, uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}

inline void Patch32(Buf &b, size_t off, uint32_t v) {
    b[off] = v & 0xFF;
    b[off + 1] = (v >> 8) & 0xFF;
    b[off + 2] = (v >> 16) & 0xFF;
    b[off + 3] = (v >> 24) & 0xFF;
}

inline void Patch64(Buf &b, size_t off, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        b[off + i] = static_cast<uint8_t>(v >> (i * 8));
    }
}

} // namespace Rux
