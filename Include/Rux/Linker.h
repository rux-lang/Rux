/*
    Rux Compiler
    Copyright © 2026 Rux Contributors
    Licensed under the MIT License
*/

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
    // Windows hosts emit PE32+; Linux hosts emit ELF64; macOS hosts emit Mach-O.
    class Linker {
    public:
        explicit Linker(std::vector<RcuFile> objects,
                        std::string packageName,
                        std::vector<std::filesystem::path> importSearchDirs = {});

        // Produce the EXE at outputPath. Creates parent directories as needed.
        // Returns false if any errors occurred; call Errors() for details.
        [[nodiscard]] bool Link(const std::filesystem::path& outputPath);

        [[nodiscard]] const std::vector<LinkerError>& Errors() const {
            return errors;
        }

    private:
        std::vector<RcuFile> objects;
        std::string packageName;
        std::vector<std::filesystem::path> importSearchDirs;
        std::vector<LinkerError> errors;

        void Error(std::string msg);
#if defined(__linux__) || defined(__FreeBSD__)
        [[nodiscard]] bool LinkElf64(const std::filesystem::path& outputPath);
#elif defined(__APPLE__)
        [[nodiscard]] bool LinkMachO64(const std::filesystem::path& outputPath);
#endif
    };
} // namespace Rux
