#pragma once

#include "Linker/NativeObjectWriter.h"
#include "Target/Target.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace Rux {
/// Writes a static library in the target's native container: a COFF library on Windows, a BSD archive on macOS, a GNU
/// archive elsewhere. The container itself is architecture-neutral, so `targetArch` only names the architecture its
/// members were compiled for and rejects one with no object encoding.
[[nodiscard]] bool WriteNativeArchive(std::span<const NativeObject> objects, Target::OS targetOs,
                                      Target::Arch targetArch, const std::filesystem::path &outputPath,
                                      std::string &error);

/// Writes the import library that accompanies a Windows shared library. Each member carries the COFF machine identifier
/// for `targetArch`.
[[nodiscard]] bool WriteWindowsImportLibrary(std::string_view libraryName, std::span<const std::string> exports,
                                             Target::Arch targetArch, const std::filesystem::path &outputPath,
                                             std::string &error);
} // namespace Rux
