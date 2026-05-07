/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
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

    // Links one or more RcuFile objects into a Windows PE32+ executable (.exe).
    // Target: Windows x86-64 (AMD64), console subsystem.
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
    };
}
