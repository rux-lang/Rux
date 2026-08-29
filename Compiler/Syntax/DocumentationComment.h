#pragma once

#include <string>
#include <string_view>

namespace Rux {
/// Normalize one exact line or block documentation comment into LF Markdown. The input retains its source delimiters;
/// unrecognized input is returned unchanged so recovery never destroys authored text.
[[nodiscard]] std::string NormalizeDocumentationComment(std::string_view raw);
} // namespace Rux
