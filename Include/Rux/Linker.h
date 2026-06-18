// Copyright (c) Rux contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include "Rux/Rcu.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Rux {
struct LinkerError {
    std::string message;
};

// Links one or more RcuFile objects into a native x86-64 executable.
// Windows hosts emit PE32+; Unix-like hosts emit ELF64; macOS hosts emit
// Mach-O.
class Linker {
public:
    explicit Linker(std::vector<RcuFile> objects, std::string packageName,
                    std::vector<std::filesystem::path> importSearchDirs = {}, bool isDll = false);

    // Produce the EXE or DLL at outputPath. Creates parent directories as
    // needed. Returns false if any errors occurred; call Errors() for
    // details.
    [[nodiscard]] bool Link(std::filesystem::path const &outputPath);

    [[nodiscard]] std::vector<LinkerError> const &Errors() const {
        return errors;
    }

private:
    std::vector<RcuFile> objects;
    std::string packageName;
    std::vector<std::filesystem::path> importSearchDirs;
    std::vector<LinkerError> errors;
    bool isDll = false;

    void Error(std::string msg);
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||                          \
    defined(__DragonFly__) || defined(__NetBSD__) || defined(__illumos__) ||                       \
    (defined(__sun) && defined(__SVR4))
    [[nodiscard]] bool LinkElf64(std::filesystem::path const &outputPath);
#elif defined(__APPLE__)
    [[nodiscard]] bool LinkMachO64(std::filesystem::path const &outputPath);
#endif
};
} // namespace Rux
