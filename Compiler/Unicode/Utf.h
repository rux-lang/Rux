#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Rux {
/// One decoded code point and the number of bytes it occupied.
struct DecodedUtf8 {
    std::uint32_t codePoint = 0;
    std::size_t width = 0;
};

/// Decode the leading UTF-8 code point of `text`.
///
/// An overlong encoding, a truncated sequence, a surrogate and a code point above U+10FFFF are all rejected rather
/// than passed through, so a caller that accepts the result holds a Unicode scalar value.
///
/// @return nullopt when `text` does not begin with a well-formed code point
[[nodiscard]] std::optional<DecodedUtf8> DecodeUtf8(std::string_view text) noexcept;

/// Decode `text` when it is exactly one code point.
///
/// @return nullopt when `text` is malformed, empty, or holds more than one code point
[[nodiscard]] std::optional<std::uint32_t> DecodeUtf8CodePoint(std::string_view text) noexcept;

/// Whether every byte of `text` belongs to a well-formed code point. An empty string is valid.
[[nodiscard]] bool IsValidUtf8(std::string_view text) noexcept;

/// How many UTF-16 code units `text` transcodes to. A code point outside the basic multilingual plane counts as the
/// two units of its surrogate pair.
///
/// @return nullopt when `text` is not valid UTF-8
[[nodiscard]] std::optional<std::size_t> Utf16CodeUnitCount(std::string_view text) noexcept;

/// How many UTF-32 code units `text` transcodes to, which is its number of code points.
///
/// @return nullopt when `text` is not valid UTF-8
[[nodiscard]] std::optional<std::size_t> Utf32CodeUnitCount(std::string_view text) noexcept;

/// How many code units of the encoding whose unit is `codeUnitBytes` wide `text` transcodes to. A width other
/// than 2 or 4 is UTF-8, whose units are the value's own bytes.
///
/// @return nullopt when `text` is not valid UTF-8
[[nodiscard]] std::optional<std::size_t> CodeUnitCount(std::string_view text, int codeUnitBytes) noexcept;

/// Transcode `text` to UTF-16, little-endian, returned as the raw bytes of the result: two bytes per code unit, so
/// the size is twice `Utf16CodeUnitCount`.
///
/// Little-endian because every supported target is, and the bytes are emitted into an image rather than exchanged.
///
/// @return nullopt when `text` is not valid UTF-8
[[nodiscard]] std::optional<std::string> TranscodeUtf8ToUtf16LE(std::string_view text);

/// Transcode `text` to UTF-32, little-endian, returned as the raw bytes of the result: four bytes per code unit.
///
/// @return nullopt when `text` is not valid UTF-8
[[nodiscard]] std::optional<std::string> TranscodeUtf8ToUtf32LE(std::string_view text);
} // namespace Rux
