#include "Syntax/DocumentationComment.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Rux::Syntax {
namespace {
struct LogicalLine {
    std::string_view text;
    SourceRange range;
};

std::vector<std::string_view> SplitLines(const std::string_view markdown) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= markdown.size()) {
        const std::size_t end = markdown.find('\n', start);
        if (end == std::string_view::npos) {
            lines.push_back(markdown.substr(start));
            break;
        }
        lines.push_back(markdown.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

std::string JoinLines(const std::vector<LogicalLine> &lines, const std::size_t end) {
    std::string result;
    for (std::size_t index = 0; index < end; ++index) {
        if (index != 0) {
            result += '\n';
        }
        result += lines[index].text;
    }
    return result;
}

bool IsHorizontalWhitespace(const char value) noexcept {
    return value == ' ' || value == '\t';
}

std::string_view TrimLeading(std::string_view text) noexcept {
    while (!text.empty() && IsHorizontalWhitespace(text.front())) {
        text.remove_prefix(1);
    }
    return text;
}

bool IsIdentifier(const std::string_view text) noexcept {
    if (text.empty() || (!std::isalpha(static_cast<unsigned char>(text.front())) && text.front() != '_')) {
        return false;
    }
    return std::ranges::all_of(text.substr(1), [](const char value) {
        return std::isalnum(static_cast<unsigned char>(value)) || value == '_';
    });
}

bool IsRuxPath(std::string_view text) noexcept {
    while (!text.empty()) {
        const std::size_t separator = text.find("::");
        const std::string_view segment = text.substr(0, separator);
        if (!IsIdentifier(segment)) {
            return false;
        }
        if (separator == std::string_view::npos) {
            return true;
        }
        text.remove_prefix(separator + 2);
    }
    return false;
}

bool IsFence(const std::string_view line, char &marker) noexcept {
    std::string_view content = line;
    std::size_t indentation = 0;
    while (indentation < 3 && content.starts_with(' ')) {
        content.remove_prefix(1);
        ++indentation;
    }
    if (content.size() < 3 || (content[0] != '`' && content[0] != '~') || content[1] != content[0] ||
        content[2] != content[0]) {
        return false;
    }
    marker = content[0];
    return true;
}

SourceRange RangeForLine(const Documentation &documentation, const std::size_t index) noexcept {
    return index < documentation.lineRanges.size() ? documentation.lineRanges[index] : documentation.range;
}

void AddIssue(Documentation &documentation, const DocumentationIssueKind kind, const SourceRange range,
              std::string message) {
    documentation.issues.push_back(DocumentationIssue{kind, range, {}, std::move(message)});
}

std::string_view TakeWord(std::string_view &text) noexcept {
    text = TrimLeading(text);
    const std::size_t end = text.find_first_of(" \t");
    const std::string_view word = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end);
    return word;
}

bool ParseReference(std::string_view &text, std::string &target, bool &unsafe) {
    text = TrimLeading(text);
    if (text.empty()) {
        return false;
    }

    bool quoted = false;
    if (text.starts_with('`')) {
        const std::size_t closing = text.find('`', 1);
        if (closing == std::string_view::npos || closing == 1) {
            return false;
        }
        target = std::string(text.substr(1, closing - 1));
        text.remove_prefix(closing + 1);
        quoted = true;
    }
    else {
        target = std::string(TakeWord(text));
    }

    if (quoted || target.starts_with("http://") || target.starts_with("https://") || IsRuxPath(target)) {
        return true;
    }
    const std::size_t colon = target.find(':');
    unsafe = colon != std::string::npos || target.contains("://");
    return false;
}

bool IsDuplicate(const DocumentationTag &tag, std::unordered_set<std::string> &seen) {
    std::string key;
    switch (tag.kind) {
    case DocumentationTagKind::Parameter:
        key = "param:" + tag.subject;
        break;
    case DocumentationTagKind::TypeParameter:
        key = "typeParam:" + tag.subject;
        break;
    case DocumentationTagKind::Returns:
        key = "returns";
        break;
    case DocumentationTagKind::See:
        key = "see:" + tag.subject;
        break;
    case DocumentationTagKind::Deprecated:
        key = "deprecated";
        break;
    }
    return !seen.insert(std::move(key)).second;
}

DocumentationTag *ParseTagLine(Documentation &documentation, const LogicalLine &line,
                               std::unordered_set<std::string> &seen) {
    std::string_view remainder = line.text;
    const std::string_view spelling = TakeWord(remainder);
    DocumentationTag tag;
    tag.range = line.range;

    const bool takesSubject = spelling == "@param" || spelling == "@typeParam";
    if (takesSubject) {
        tag.kind = spelling == "@param" ? DocumentationTagKind::Parameter : DocumentationTagKind::TypeParameter;
        const std::string_view subject = TakeWord(remainder);
        remainder = TrimLeading(remainder);
        if (!IsIdentifier(subject) || remainder.empty()) {
            AddIssue(documentation, DocumentationIssueKind::MalformedTag, line.range,
                     std::string(spelling) + " requires an identifier and Markdown description");
            return nullptr;
        }
        tag.subject = subject;
        tag.markdown = remainder;
    }
    else if (spelling == "@returns" || spelling == "@deprecated") {
        tag.kind = spelling == "@returns" ? DocumentationTagKind::Returns : DocumentationTagKind::Deprecated;
        remainder = TrimLeading(remainder);
        if (remainder.empty()) {
            AddIssue(documentation, DocumentationIssueKind::MalformedTag, line.range,
                     std::string(spelling) + " requires a Markdown description");
            return nullptr;
        }
        tag.markdown = remainder;
    }
    else if (spelling == "@see") {
        tag.kind = DocumentationTagKind::See;
        bool unsafe = false;
        if (!ParseReference(remainder, tag.subject, unsafe)) {
            AddIssue(documentation,
                     unsafe ? DocumentationIssueKind::UnsafeReference : DocumentationIssueKind::MalformedTag,
                     line.range,
                     unsafe ? "documentation reference uses an unsafe or unsupported scheme"
                            : "@see requires an HTTP URL, Rux path, or backtick-delimited reference");
            return nullptr;
        }
        tag.markdown = TrimLeading(remainder);
    }
    else {
        AddIssue(documentation, DocumentationIssueKind::UnknownTag, line.range,
                 "unknown documentation tag '" + std::string(spelling) + "'");
        return nullptr;
    }

    if (IsDuplicate(tag, seen)) {
        AddIssue(documentation, DocumentationIssueKind::DuplicateTag, line.range,
                 "duplicate documentation tag '" + std::string(spelling) +
                     (tag.subject.empty() ? "'" : " " + tag.subject + "'"));
    }
    documentation.tags.push_back(std::move(tag));
    return &documentation.tags.back();
}
} // namespace

void ParseDocumentationTags(Documentation &documentation) {
    const std::vector<std::string_view> textLines = SplitLines(documentation.markdown);
    std::vector<LogicalLine> lines;
    lines.reserve(textLines.size());
    for (std::size_t index = 0; index < textLines.size(); ++index) {
        lines.push_back(LogicalLine{textLines[index], RangeForLine(documentation, index)});
    }

    bool fenced = false;
    char fenceMarker = '\0';
    std::size_t firstTag = lines.size();
    for (std::size_t index = 0; index < lines.size(); ++index) {
        char marker = '\0';
        if (IsFence(lines[index].text, marker)) {
            if (!fenced) {
                fenced = true;
                fenceMarker = marker;
            }
            else if (marker == fenceMarker) {
                fenced = false;
            }
            continue;
        }
        if (!fenced && lines[index].text.starts_with('@')) {
            firstTag = index;
            break;
        }
    }
    if (firstTag == lines.size()) {
        return;
    }

    std::unordered_set<std::string> seen;
    DocumentationTag *current = nullptr;
    for (std::size_t index = firstTag; index < lines.size(); ++index) {
        const LogicalLine &line = lines[index];
        if (line.text.starts_with('@')) {
            current = ParseTagLine(documentation, line, seen);
            continue;
        }
        if (line.text.empty()) {
            if (current != nullptr) {
                current->markdown += '\n';
                current->range.end = line.range.end;
            }
            continue;
        }
        if (line.text.starts_with("  ")) {
            if (current == nullptr) {
                AddIssue(documentation, DocumentationIssueKind::MalformedContinuation, line.range,
                         "documentation tag continuation has no preceding tag");
                continue;
            }
            current->markdown += '\n';
            current->markdown += line.text.substr(2);
            current->range.end = line.range.end;
            continue;
        }
        if (line.text.starts_with(' ')) {
            AddIssue(documentation, DocumentationIssueKind::MalformedContinuation, line.range,
                     "documentation tag continuation must begin with two spaces");
        }
        else {
            AddIssue(documentation, DocumentationIssueKind::ProseAfterTags, line.range,
                     "unindented prose cannot follow documentation tags");
        }
        if (current != nullptr) {
            current->markdown += '\n';
            current->markdown += line.text;
            current->range.end = line.range.end;
        }
    }

    std::size_t proseEnd = firstTag;
    while (proseEnd > 0 && lines[proseEnd - 1].text.empty()) {
        --proseEnd;
    }
    documentation.markdown = JoinLines(lines, proseEnd);
    documentation.lineRanges.resize(std::min(proseEnd, documentation.lineRanges.size()));
}
} // namespace Rux::Syntax
