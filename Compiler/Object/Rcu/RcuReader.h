#pragma once

#include "Object/Rcu/Rcu.h"

#include <filesystem>
#include <optional>

namespace Rux {
/// Reads an RCU object back from disk: the inverse of `RcuWriter`, which is what lets a build link an object an
/// earlier build compiled instead of compiling its module again.
class RcuReader {
public:
    /// @return nullopt when the file cannot be read or is not a well-formed RCU object
    [[nodiscard]] static std::optional<RcuFile> Read(const std::filesystem::path &path);
};
} // namespace Rux
