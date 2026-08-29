#include "Syntax/DocumentationComment.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::Syntax {
namespace {
bool IsHorizontalWhitespace(const char value) noexcept {
    return value == ' ' || value == '\t';
}

bool IsBlank(const std::string_view line) noexcept {
    return std::ranges::all_of(line, IsHorizontalWhitespace);
}

std::string NormalizeLineEndings(const std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '\r') {
            normalized += text[index];
            continue;
        }

        normalized += '\n';
        if (index + 1 < text.size() && text[index + 1] == '\n') {
            ++index;
        }
    }
    return normalized;
}

std::vector<std::string_view> SplitLines(const std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

std::size_t Indentation(const std::string_view line) noexcept {
    std::size_t width = 0;
    while (width < line.size() && IsHorizontalWhitespace(line[width])) {
        ++width;
    }
    return width;
}

void RemoveBoundaryLines(std::vector<std::string_view> &lines) {
    if (!lines.empty() && IsBlank(lines.front())) {
        lines.erase(lines.begin());
    }
    if (!lines.empty() && IsBlank(lines.back())) {
        lines.pop_back();
    }
}

void RemoveCommonIndentation(std::vector<std::string_view> &lines) {
    std::size_t common = std::numeric_limits<std::size_t>::max();
    for (const std::string_view line : lines) {
        if (!IsBlank(line)) {
            common = std::min(common, Indentation(line));
        }
    }
    if (common == std::numeric_limits<std::size_t>::max() || common == 0) {
        return;
    }

    for (std::string_view &line : lines) {
        line.remove_prefix(std::min(common, Indentation(line)));
    }
}

bool HasAlignedStarMargin(const std::vector<std::string_view> &lines) noexcept {
    bool found = false;
    for (const std::string_view line : lines) {
        if (IsBlank(line)) {
            continue;
        }
        if (!line.starts_with('*') || (line.size() > 1 && line[1] != ' ')) {
            return false;
        }
        found = true;
    }
    return found;
}

void RemoveAlignedStarMargin(std::vector<std::string_view> &lines) {
    if (!HasAlignedStarMargin(lines)) {
        return;
    }

    for (std::string_view &line : lines) {
        if (line.starts_with('*')) {
            line.remove_prefix(1);
            if (line.starts_with(' ')) {
                line.remove_prefix(1);
            }
        }
    }
}

std::string JoinLines(const std::vector<std::string_view> &lines) {
    std::size_t size = lines.empty() ? 0 : lines.size() - 1;
    for (const std::string_view line : lines) {
        size += line.size();
    }

    std::string result;
    result.reserve(size);
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index != 0) {
            result += '\n';
        }
        result += lines[index];
    }
    return result;
}

std::string NormalizeLineComment(const std::string_view raw) {
    std::string_view content = raw;
    content.remove_prefix(3);
    if (content.starts_with(' ')) {
        content.remove_prefix(1);
    }
    return std::string(content);
}

std::string NormalizeBlockComment(const std::string_view raw) {
    if (!raw.ends_with("*/")) {
        return std::string(raw);
    }

    std::string body = NormalizeLineEndings(raw.substr(3, raw.size() - 5));
    if (!body.contains('\n')) {
        std::string_view content = body;
        if (content.starts_with(' ')) {
            content.remove_prefix(1);
        }
        if (content.ends_with(' ')) {
            content.remove_suffix(1);
        }
        return std::string(content);
    }

    std::vector<std::string_view> lines = SplitLines(body);
    RemoveBoundaryLines(lines);
    RemoveCommonIndentation(lines);
    RemoveAlignedStarMargin(lines);
    return JoinLines(lines);
}
} // namespace

std::string NormalizeDocumentationComment(const std::string_view raw) {
    if (raw.starts_with("///") && !raw.starts_with("////")) {
        return NormalizeLineComment(raw);
    }
    if (raw.starts_with("/**") && !raw.starts_with("/***") && !raw.starts_with("/**/")) {
        return NormalizeBlockComment(raw);
    }
    return std::string(raw);
}

bool Documentation::Empty() const noexcept {
    return markdown.empty() && tags.empty();
}

bool Documentation::Present() const noexcept {
    return !range.Empty();
}

std::string_view Documentation::Summary() const noexcept {
    const std::size_t paragraphEnd = markdown.find("\n\n");
    return std::string_view(markdown).substr(0, paragraphEnd);
}
} // namespace Rux::Syntax
