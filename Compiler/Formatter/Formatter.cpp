#include "Formatter/Formatter.h"

#include "Lexer/Lexer.h"
#include "Syntax/DocumentationComment.h"

#include <algorithm>
#include <string>
#include <vector>

namespace Rux::Formatting {
namespace {
struct Edit {
    std::size_t start = 0;
    std::size_t end = 0;
    std::string replacement;
};

std::string_view TrimLeft(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    return text;
}

std::string_view Trim(std::string_view text) {
    text = TrimLeft(text);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

std::string_view TakeWord(std::string_view &text) {
    text = TrimLeft(text);
    const std::size_t end = text.find_first_of(" \t");
    const std::string_view word = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end);
    return word;
}

bool StartsTag(const std::string_view line, const std::string_view spelling) {
    return line.starts_with(spelling) &&
           (line.size() == spelling.size() || line[spelling.size()] == ' ' || line[spelling.size()] == '\t');
}

std::string CanonicalTagLine(const std::string_view line) {
    static constexpr std::string_view subjectTags[] = {"@param", "@typeParam"};
    for (const std::string_view spelling : subjectTags) {
        if (!StartsTag(line, spelling)) {
            continue;
        }
        std::string_view rest = line.substr(spelling.size());
        const std::string_view subject = TakeWord(rest);
        rest = TrimLeft(rest);
        if (subject.empty() || rest.empty()) {
            return std::string(line);
        }
        return std::string(spelling) + " " + std::string(subject) + " " + std::string(rest);
    }

    static constexpr std::string_view textTags[] = {"@returns", "@deprecated"};
    for (const std::string_view spelling : textTags) {
        if (!StartsTag(line, spelling)) {
            continue;
        }
        const std::string_view text = TrimLeft(line.substr(spelling.size()));
        return text.empty() ? std::string(line) : std::string(spelling) + " " + std::string(text);
    }

    if (StartsTag(line, "@see")) {
        std::string_view rest = line.substr(4);
        const std::string_view target = TakeWord(rest);
        rest = TrimLeft(rest);
        if (target.empty()) {
            return std::string(line);
        }
        return "@see " + std::string(target) + (rest.empty() ? "" : " " + std::string(rest));
    }
    return std::string(line);
}

bool FenceMarker(const std::string_view line, char &marker) {
    const std::string_view trimmed = TrimLeft(line);
    if (trimmed.starts_with("```") || trimmed.starts_with("~~~")) {
        marker = trimmed.front();
        return true;
    }
    return false;
}

std::vector<std::string> SplitLines(const std::string_view markdown) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= markdown.size()) {
        const std::size_t end = markdown.find('\n', start);
        lines.emplace_back(
            markdown.substr(start, end == std::string_view::npos ? markdown.size() - start : end - start));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

void CanonicalizeTagBlock(std::vector<std::string> &lines) {
    bool fenced = false;
    char openMarker = '\0';
    std::size_t firstTag = lines.size();
    for (std::size_t index = 0; index < lines.size(); ++index) {
        char marker = '\0';
        if (FenceMarker(lines[index], marker)) {
            if (!fenced) {
                fenced = true;
                openMarker = marker;
            }
            else if (marker == openMarker) {
                fenced = false;
            }
            continue;
        }
        if (!fenced && lines[index].starts_with('@')) {
            if (firstTag == lines.size()) {
                firstTag = index;
            }
            lines[index] = CanonicalTagLine(lines[index]);
        }
    }
    if (firstTag > 0 && firstTag < lines.size() && !lines[firstTag - 1].empty()) {
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(firstTag), "");
    }
}

std::string LineDocumentation(const std::string_view content) {
    return content.empty() ? "///" : "/// " + std::string(content);
}

std::string BlockDocumentation(const CommentTrivia &comment) {
    std::vector<std::string> lines = SplitLines(Syntax::NormalizeDocumentationComment(comment.raw));
    CanonicalizeTagBlock(lines);
    const bool multiline = comment.raw.find_first_of("\r\n") != std::string::npos || lines.size() > 1;
    if (!multiline) {
        const std::string_view content = Trim(lines.front());
        return content.empty() ? "/** */" : "/** " + std::string(content) + " */";
    }

    const std::string indent(comment.range.start.column - 1, ' ');
    std::string result = "/**";
    for (const std::string &line : lines) {
        result += '\n';
        result += indent;
        result += line.empty() ? " *" : " * " + line;
    }
    result += '\n';
    result += indent;
    result += " */";
    return result;
}

bool AdjacentDocumentationLines(const CommentTrivia &left, const CommentTrivia &right, const std::string_view source) {
    if (left.kind != CommentKind::DocumentationLine || right.kind != CommentKind::DocumentationLine ||
        right.range.start.line != left.range.end.line + 1) {
        return false;
    }
    return std::ranges::all_of(
        source.substr(left.range.end.offset, right.range.start.offset - left.range.end.offset),
        [](const char value) { return value == ' ' || value == '\t' || value == '\r' || value == '\n'; });
}

std::vector<Edit> DocumentationEdits(const std::string_view source, const std::vector<CommentTrivia> &comments) {
    std::vector<Edit> edits;
    for (std::size_t index = 0; index < comments.size();) {
        const CommentTrivia &comment = comments[index];
        if (!IsDocumentationComment(comment.kind) || !comment.terminated) {
            ++index;
            continue;
        }
        if (comment.kind == CommentKind::DocumentationBlock) {
            edits.push_back({comment.range.start.offset, comment.range.end.offset, BlockDocumentation(comment)});
            ++index;
            continue;
        }

        std::size_t groupEnd = index + 1;
        while (groupEnd < comments.size() &&
               AdjacentDocumentationLines(comments[groupEnd - 1], comments[groupEnd], source)) {
            ++groupEnd;
        }
        std::vector<std::string> lines;
        lines.reserve(groupEnd - index);
        for (std::size_t line = index; line < groupEnd; ++line) {
            lines.push_back(Syntax::NormalizeDocumentationComment(comments[line].raw));
        }
        CanonicalizeTagBlock(lines);

        std::size_t rendered = 0;
        for (std::size_t line = index; line < groupEnd; ++line) {
            std::string replacement;
            while (rendered < lines.size() - (groupEnd - line - 1)) {
                if (!replacement.empty()) {
                    replacement += '\n';
                    replacement.append(comments[line].range.start.column - 1, ' ');
                }
                replacement += LineDocumentation(lines[rendered++]);
            }
            edits.push_back(
                {comments[line].range.start.offset, comments[line].range.end.offset, std::move(replacement)});
        }
        index = groupEnd;
    }
    return edits;
}

std::string ApplyEdits(const std::string_view source, const std::vector<Edit> &edits) {
    std::string result;
    result.reserve(source.size() + edits.size() * 4);
    std::size_t offset = 0;
    for (const Edit &edit : edits) {
        result.append(source.substr(offset, edit.start - offset));
        result += edit.replacement;
        offset = edit.end;
    }
    result.append(source.substr(offset));
    return result;
}

bool ProtectedWhitespace(const std::vector<CommentTrivia> &comments, const std::size_t offset) {
    return std::ranges::any_of(comments, [&](const CommentTrivia &comment) {
        const bool preserve = !IsDocumentationComment(comment.kind) || !comment.terminated;
        return preserve && offset >= comment.range.start.offset && offset < comment.range.end.offset;
    });
}

std::string NormalizeLines(const std::string_view source) {
    const auto lexed = Lexer(std::string(source), "<formatter>").Tokenize();
    std::string result;
    result.reserve(source.size() + 1);
    std::size_t offset = 0;
    while (offset < source.size()) {
        const std::size_t ending = source.find_first_of("\r\n", offset);
        const std::size_t lineEnd = ending == std::string_view::npos ? source.size() : ending;
        std::size_t contentEnd = lineEnd;
        while (contentEnd > offset && (source[contentEnd - 1] == ' ' || source[contentEnd - 1] == '\t') &&
               !ProtectedWhitespace(lexed.comments, contentEnd - 1)) {
            --contentEnd;
        }
        result.append(source.substr(offset, contentEnd - offset));
        if (ending == std::string_view::npos) {
            break;
        }
        result.push_back('\n');
        offset = ending + 1;
        if (source[ending] == '\r' && offset < source.size() && source[offset] == '\n') {
            ++offset;
        }
    }
    if (!result.empty() && result.back() != '\n') {
        result.push_back('\n');
    }
    return result;
}
} // namespace

FormatResult Format(const std::string_view source) {
    const LexerResult lexed = Lexer(std::string(source), "<formatter>").Tokenize();
    const std::string edited = ApplyEdits(source, DocumentationEdits(source, lexed.comments));
    std::string formatted = NormalizeLines(edited);
    const bool changed = formatted != source;
    return {std::move(formatted), changed};
}
} // namespace Rux::Formatting
