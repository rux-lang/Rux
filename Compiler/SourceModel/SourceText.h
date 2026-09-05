#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Rux {
/// Immutable source contents. Diagnostic offsets are indexed only when a line is requested; returned views remain
/// valid until this object is moved or destroyed. A compilation owns each text and its diagnostic index together.
class SourceText {
public:
    explicit SourceText(std::string contents);
    [[nodiscard]] std::string_view Text() const noexcept;
    [[nodiscard]] std::optional<std::string_view> Line(std::size_t number) const;

private:
    std::string contents;
    mutable std::vector<std::size_t> lineStarts;
};
} // namespace Rux
