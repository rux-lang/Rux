#pragma once

#include <string>
#include <string_view>

namespace Rux {
/// The source spelling of an already uninstantiated nominal name, for constructor and destructor method names.
inline std::string UnqualifiedNominalName(const std::string_view name) {
    const auto separator = name.rfind("::");
    return std::string(separator == std::string_view::npos ? name : name.substr(separator + 2));
}
} // namespace Rux
