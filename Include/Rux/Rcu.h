/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#pragma once

#include "Rux/Lir.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Rux {

// ── Format constants ──────────────────────────────────────────────────────────

namespace RcuSecType {
    constexpr uint32_t Null   = 0;
    constexpr uint32_t Text   = 1;
    constexpr uint32_t Data   = 2;
    constexpr uint32_t RoData = 3;
    constexpr uint32_t Bss    = 4;
    constexpr uint32_t Meta   = 5;
}

namespace RcuSecFlag {
    constexpr uint32_t Alloc   = 0x01;
    constexpr uint32_t Exec    = 0x02;
    constexpr uint32_t Read    = 0x04;
    constexpr uint32_t Write   = 0x08;
    constexpr uint32_t Merge   = 0x10;
    constexpr uint32_t Strings = 0x20;
}

namespace RcuSymKind {
    constexpr uint8_t Unknown    = 0;
    constexpr uint8_t Func       = 1;
    constexpr uint8_t Data       = 2;
    constexpr uint8_t Const      = 3;
    constexpr uint8_t Section    = 4;
    constexpr uint8_t File       = 5;
    constexpr uint8_t ExternFunc = 6;
    constexpr uint8_t ExternData = 7;
}

namespace RcuSymVis {
    constexpr uint8_t Local  = 0;
    constexpr uint8_t Global = 1;
    constexpr uint8_t Weak   = 2;
}

namespace RcuRelType {
    constexpr uint16_t None  = 0;
    constexpr uint16_t Abs64 = 1;
    constexpr uint16_t Abs32 = 2;
    constexpr uint16_t Rel32 = 3;
}

constexpr uint16_t RCU_SEC_EXTERNAL = 0xFFFF;
constexpr uint16_t RCU_SEC_ABSOLUTE = 0xFFFE;

// Fixed section indices used by the generator
constexpr uint16_t RCU_TEXT_IDX   = 0;
constexpr uint16_t RCU_RODATA_IDX = 1;
constexpr uint16_t RCU_DATA_IDX   = 2;

// ── In-memory structures ──────────────────────────────────────────────────────

struct RcuReloc {
    uint32_t sectionOffset = 0;
    uint32_t symbolIndex   = 0;
    uint16_t type          = RcuRelType::None;
    int32_t  addend        = 0;
};

struct RcuSection {
    std::string          name;
    uint32_t             type      = RcuSecType::Null;
    uint32_t             flags     = 0;
    uint16_t             alignment = 1;
    std::vector<uint8_t> data;
    std::vector<RcuReloc> relocs;
};

struct RcuSymbol {
    std::string  name;
    std::string  typeName;
    uint32_t     value      = 0;
    uint32_t     size       = 0;
    uint16_t     sectionIdx = RCU_SEC_EXTERNAL;
    uint8_t      kind       = RcuSymKind::Unknown;
    uint8_t      visibility = RcuSymVis::Local;
};

struct RcuFile {
    uint8_t  arch           = 0x01; // x86-64
    uint8_t  flags          = 0x00;
    bool     hasMetadata    = false;
    std::string sourcePath;
    std::string packageName;
    uint64_t buildTimestamp  = 0;
    uint32_t ruxVersion      = 0;
    uint32_t compilerFlags   = 0;
    std::array<uint8_t, 32> sourceHash = {};
    std::vector<RcuSection> sections;
    std::vector<RcuSymbol>  symbols;
};

// ── Public API ────────────────────────────────────────────────────────────────

// Generates RCU binary object files from a LIR package.
// One RcuFile is produced per LirModule (one per source file).
class Rcu {
public:
    explicit Rcu(LirPackage package, std::string packageName = {});

    // Generate one RcuFile per module in the package.
    [[nodiscard]] std::vector<RcuFile> Generate();

    // Write a binary RCU file. Returns false on I/O error.
    static bool Emit(const RcuFile& file, const std::filesystem::path& path);

    // Write a human-readable text dump. Returns false on I/O error.
    static bool Dump(const RcuFile& file, const std::filesystem::path& path);

private:
    LirPackage  lir_;
    std::string packageName_;
};

} // namespace Rux
