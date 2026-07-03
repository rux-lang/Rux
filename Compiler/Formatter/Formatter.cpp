#include "Formatter/Formatter.h"

namespace Rux::Formatting {

FormatResult Format(const std::string_view source) {
    std::string formatted;
    formatted.reserve(source.size() + 1);

    std::size_t offset = 0;
    while (offset < source.size()) {
        const std::size_t end = source.find_first_of("\r\n", offset);
        const std::size_t lineEnd = end == std::string_view::npos ? source.size() : end;
        std::size_t contentEnd = lineEnd;
        while (contentEnd > offset && (source[contentEnd - 1] == ' ' || source[contentEnd - 1] == '\t')) {
            --contentEnd;
        }
        formatted.append(source.substr(offset, contentEnd - offset));
        if (end == std::string_view::npos) {
            break;
        }
        formatted.push_back('\n');
        offset = end + 1;
        if (source[end] == '\r' && offset < source.size() && source[offset] == '\n') {
            ++offset;
        }
    }

    if (!formatted.empty() && formatted.back() != '\n') {
        formatted.push_back('\n');
    }
    return {formatted, formatted != source};
}

} // namespace Rux::Formatting
