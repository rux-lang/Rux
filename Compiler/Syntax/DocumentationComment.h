#pragma once

#include "SourceModel/SourceLocation.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::Syntax {
enum class DocumentationTagKind : std::uint8_t {
    Parameter,
    TypeParameter,
    Returns,
    See,
    Deprecated,
};

struct DocumentationTag {
    DocumentationTagKind kind = DocumentationTagKind::Parameter;
    std::string subject;
    std::string markdown;
    SourceRange range;
};

enum class DocumentationIssueKind : std::uint8_t {
    Detached,
    Trailing,
};

struct DocumentationIssue {
    DocumentationIssueKind kind = DocumentationIssueKind::Detached;
    SourceRange range;
    std::string markdown;
    std::string message;
};

/// Normalized, source-aware documentation attached to one syntax item. Tags and issues are populated by tooling passes
/// after comment attachment; keeping them here gives completion and documentation generation one shared representation.
struct Documentation {
    std::string markdown;
    SourceRange range;
    std::vector<DocumentationTag> tags;
    std::vector<DocumentationIssue> issues;

    [[nodiscard]] bool Empty() const noexcept;
    [[nodiscard]] bool Present() const noexcept;
    [[nodiscard]] std::string_view Summary() const noexcept;
};

/// Normalize one exact line or block documentation comment into LF Markdown. The input retains its source delimiters;
/// unrecognized input is returned unchanged so recovery never destroys authored text.
[[nodiscard]] std::string NormalizeDocumentationComment(std::string_view raw);
} // namespace Rux::Syntax
