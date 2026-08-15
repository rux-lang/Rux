#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace Rux {

enum class RuntimeFailureKind {
    Assertion,
    Panic
};

// Runtime failures are emitted as three writes because the message is a
// runtime Slice<char8>. Keeping the two static fragments together here makes
// their logical byte layout independent of the target's writing mechanism.
struct RuntimeFailureLayout {
    std::string prefix;
    std::string location;

    [[nodiscard]] std::string Join(std::string_view message) const;
};

[[nodiscard]] RuntimeFailureLayout BuildRuntimeFailureLayout(RuntimeFailureKind kind, std::string_view function,
                                                             std::string_view file, std::size_t line,
                                                             std::size_t column);

} // namespace Rux
