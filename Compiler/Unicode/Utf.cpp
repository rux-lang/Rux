#include "Unicode/Utf.h"

namespace Rux {
namespace {
/// The last code point Unicode defines, and the surrogate range, which is half of a UTF-16 pair rather than a
/// character and so encodes nothing on its own.
constexpr std::uint32_t MaxScalarValue = 0x10FFFF;
constexpr std::uint32_t FirstSurrogate = 0xD800;
constexpr std::uint32_t LastSurrogate = 0xDFFF;

/// The first code point that needs a surrogate pair in UTF-16.
constexpr std::uint32_t FirstSupplementary = 0x10000;

/// Walk every code point of `text`, calling `visit` with each. Counting, validating and transcoding are the same
/// walk over three different bodies.
template <typename Visit>
[[nodiscard]] bool ForEachCodePoint(const std::string_view text, Visit visit) {
    std::string_view rest = text;
    while (!rest.empty()) {
        const auto decoded = DecodeUtf8(rest);
        if (!decoded) {
            return false;
        }
        visit(decoded->codePoint);
        rest.remove_prefix(decoded->width);
    }
    return true;
}

void AppendLittleEndian16(std::string &bytes, const std::uint32_t unit) {
    bytes.push_back(static_cast<char>(unit & 0xFF));
    bytes.push_back(static_cast<char>((unit >> 8) & 0xFF));
}

void AppendLittleEndian32(std::string &bytes, const std::uint32_t unit) {
    AppendLittleEndian16(bytes, unit & 0xFFFF);
    AppendLittleEndian16(bytes, unit >> 16);
}
} // namespace

std::optional<DecodedUtf8> DecodeUtf8(const std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }

    const auto continuation = [&](const std::size_t index) -> std::optional<std::uint32_t> {
        if (index >= text.size()) {
            return std::nullopt;
        }
        const auto byte = static_cast<unsigned char>(text[index]);
        if ((byte & 0xC0) != 0x80) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(byte & 0x3F);
    };

    const auto lead = static_cast<unsigned char>(text[0]);
    std::uint32_t codePoint = 0;
    std::size_t width = 0;
    // The smallest code point this width is allowed to carry. Checking it is what rejects an overlong encoding,
    // which would otherwise be a second spelling of a code point that already has one.
    std::uint32_t minValue = 0;
    if (lead <= 0x7F) {
        codePoint = lead;
        width = 1;
        minValue = 0;
    }
    else if ((lead & 0xE0) == 0xC0) {
        codePoint = lead & 0x1F;
        width = 2;
        minValue = 0x80;
    }
    else if ((lead & 0xF0) == 0xE0) {
        codePoint = lead & 0x0F;
        width = 3;
        minValue = 0x800;
    }
    else if ((lead & 0xF8) == 0xF0) {
        codePoint = lead & 0x07;
        width = 4;
        minValue = FirstSupplementary;
    }
    else {
        return std::nullopt;
    }

    if (text.size() < width) {
        return std::nullopt;
    }
    for (std::size_t index = 1; index < width; ++index) {
        const auto byte = continuation(index);
        if (!byte) {
            return std::nullopt;
        }
        codePoint = (codePoint << 6) | *byte;
    }
    if (codePoint < minValue || codePoint > MaxScalarValue) {
        return std::nullopt;
    }
    if (codePoint >= FirstSurrogate && codePoint <= LastSurrogate) {
        return std::nullopt;
    }
    return DecodedUtf8{codePoint, width};
}

std::optional<std::uint32_t> DecodeUtf8CodePoint(const std::string_view text) noexcept {
    const auto decoded = DecodeUtf8(text);
    if (!decoded || decoded->width != text.size()) {
        return std::nullopt;
    }
    return decoded->codePoint;
}

bool IsValidUtf8(const std::string_view text) noexcept {
    return ForEachCodePoint(text, [](std::uint32_t) {});
}

std::optional<std::size_t> Utf16CodeUnitCount(const std::string_view text) noexcept {
    std::size_t units = 0;
    const bool valid = ForEachCodePoint(
        text, [&](const std::uint32_t codePoint) { units += codePoint >= FirstSupplementary ? 2 : 1; });
    return valid ? std::optional{units} : std::nullopt;
}

std::optional<std::size_t> Utf32CodeUnitCount(const std::string_view text) noexcept {
    std::size_t units = 0;
    const bool valid = ForEachCodePoint(text, [&](std::uint32_t) { ++units; });
    return valid ? std::optional{units} : std::nullopt;
}

std::optional<std::string> TranscodeUtf8ToUtf16LE(const std::string_view text) {
    std::string bytes;
    bytes.reserve(text.size() * 2);
    const bool valid = ForEachCodePoint(text, [&](const std::uint32_t codePoint) {
        if (codePoint < FirstSupplementary) {
            AppendLittleEndian16(bytes, codePoint);
            return;
        }
        // A supplementary code point is carried by a surrogate pair: the value above the plane boundary is split
        // into two ten-bit halves, the high one biased to 0xD800 and the low one to 0xDC00.
        const std::uint32_t offset = codePoint - FirstSupplementary;
        AppendLittleEndian16(bytes, 0xD800 + (offset >> 10));
        AppendLittleEndian16(bytes, 0xDC00 + (offset & 0x3FF));
    });
    return valid ? std::optional{std::move(bytes)} : std::nullopt;
}

std::optional<std::string> TranscodeUtf8ToUtf32LE(const std::string_view text) {
    std::string bytes;
    bytes.reserve(text.size() * 4);
    const bool valid =
        ForEachCodePoint(text, [&](const std::uint32_t codePoint) { AppendLittleEndian32(bytes, codePoint); });
    return valid ? std::optional{std::move(bytes)} : std::nullopt;
}
} // namespace Rux
