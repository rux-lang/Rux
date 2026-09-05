#include "SourceModel/SourceText.h"

#include <utility>

namespace Rux {
SourceText::SourceText(std::string text)
    : contents(std::move(text)) {
}

std::string_view SourceText::Text() const noexcept {
    return contents;
}

std::optional<std::string_view> SourceText::Line(const std::size_t number) const {
    if (number == 0) {
        return std::nullopt;
    }
    if (lineStarts.empty()) {
        lineStarts.push_back(0);
        for (std::size_t at = 0; at < contents.size(); ++at) {
            if (contents[at] == '\n') {
                lineStarts.push_back(at + 1);
            }
        }
    }
    if (number > lineStarts.size()) {
        return std::nullopt;
    }
    const auto begin = lineStarts[number - 1];
    auto end = number < lineStarts.size() ? lineStarts[number] - 1 : contents.size();
    if (end > begin && contents[end - 1] == '\r') {
        --end;
    }
    return Text().substr(begin, end - begin);
}
} // namespace Rux
