#pragma once

#include "Linker/NativeObjectWriter.h"
#include "Target/Target.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace Rux {
[[nodiscard]] bool WriteNativeArchive(std::span<const NativeObject> objects, Target::OS targetOs,
                                      const std::filesystem::path &outputPath, std::string &error);
[[nodiscard]] bool WriteWindowsImportLibrary(std::string_view libraryName, std::span<const std::string> exports,
                                             const std::filesystem::path &outputPath, std::string &error);
} // namespace Rux
