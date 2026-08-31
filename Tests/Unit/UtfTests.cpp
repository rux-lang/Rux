#include "Unicode/Utf.h"

#include <doctest.h>
#include <string>
#include <string_view>

using namespace Rux;

namespace {
/// The four sequence widths, one sample each: ASCII, the two- and three-byte forms, and a supplementary code point
/// that needs a surrogate pair in UTF-16.
constexpr std::string_view Ascii = "A";
constexpr std::string_view Cent = "\xC2\xA2";           // U+00A2
constexpr std::string_view Euro = "\xE2\x82\xAC";       // U+20AC
constexpr std::string_view Rocket = "\xF0\x9F\x9A\x80"; // U+1F680
constexpr std::string_view Mixed = "A\xC2\xA2\xE2\x82\xAC\xF0\x9F\x9A\x80";

/// The bytes of a transcoding, rendered as a readable comparison value.
[[nodiscard]] std::string Bytes(const std::string_view text) {
    return std::string(text);
}
} // namespace

TEST_CASE("a code point decodes with the width its lead byte announces") {
    REQUIRE(DecodeUtf8(Ascii));
    CHECK_EQ(DecodeUtf8(Ascii)->codePoint, 0x41);
    CHECK_EQ(DecodeUtf8(Ascii)->width, 1);
    CHECK_EQ(DecodeUtf8(Cent)->codePoint, 0xA2);
    CHECK_EQ(DecodeUtf8(Cent)->width, 2);
    CHECK_EQ(DecodeUtf8(Euro)->codePoint, 0x20AC);
    CHECK_EQ(DecodeUtf8(Euro)->width, 3);
    CHECK_EQ(DecodeUtf8(Rocket)->codePoint, 0x1F680);
    CHECK_EQ(DecodeUtf8(Rocket)->width, 4);

    // The width belongs to the leading code point, not to the whole string.
    CHECK_EQ(DecodeUtf8(Mixed)->codePoint, 0x41);
    CHECK_EQ(DecodeUtf8(Mixed)->width, 1);
}

TEST_CASE("a single code point decodes only when it is the whole string") {
    CHECK_EQ(DecodeUtf8CodePoint(Euro), 0x20AC);
    CHECK_FALSE(DecodeUtf8CodePoint(Mixed).has_value());
    CHECK_FALSE(DecodeUtf8CodePoint("").has_value());
}

TEST_CASE("malformed UTF-8 is rejected rather than passed through") {
    // A continuation byte with no lead, a lead byte with no continuation, and a truncated four-byte sequence.
    for (const std::string_view text : {"\xA2", "\xC2", "\xE2\x82", "\xF0\x9F\x9A", "\xFF", "\xC2\x41"}) {
        CAPTURE(text);
        CHECK_FALSE(DecodeUtf8(text).has_value());
        CHECK_FALSE(IsValidUtf8(text));
        CHECK_FALSE(Utf16CodeUnitCount(text).has_value());
        CHECK_FALSE(Utf32CodeUnitCount(text).has_value());
        CHECK_FALSE(TranscodeUtf8ToUtf16LE(text).has_value());
        CHECK_FALSE(TranscodeUtf8ToUtf32LE(text).has_value());
    }
}

TEST_CASE("an overlong encoding is a second spelling and is rejected") {
    CHECK_FALSE(IsValidUtf8("\xC0\x80"));         // U+0000 spelled in two bytes
    CHECK_FALSE(IsValidUtf8("\xE0\x80\xAF"));     // U+002F spelled in three
    CHECK_FALSE(IsValidUtf8("\xF0\x80\x80\xAF")); // U+002F spelled in four
}

TEST_CASE("a lone surrogate and a code point past U+10FFFF are not scalar values") {
    CHECK_FALSE(IsValidUtf8("\xED\xA0\x80"));     // U+D800, the first high surrogate
    CHECK_FALSE(IsValidUtf8("\xED\xBF\xBF"));     // U+DFFF, the last low surrogate
    CHECK_FALSE(IsValidUtf8("\xF4\x90\x80\x80")); // U+110000
    CHECK(IsValidUtf8("\xF4\x8F\xBF\xBF"));       // U+10FFFF, the last code point
}

TEST_CASE("an empty string is valid and transcodes to nothing") {
    CHECK(IsValidUtf8(""));
    CHECK_EQ(Utf16CodeUnitCount(""), 0);
    CHECK_EQ(Utf32CodeUnitCount(""), 0);
    CHECK_EQ(TranscodeUtf8ToUtf16LE(""), std::string());
    CHECK_EQ(TranscodeUtf8ToUtf32LE(""), std::string());
}

TEST_CASE("a code unit count is counted in units of its own encoding") {
    // Only the UTF-16 count of a supplementary code point differs from its code-point count, and that is exactly the
    // surrogate pair.
    CHECK_EQ(Utf16CodeUnitCount(Ascii), 1);
    CHECK_EQ(Utf16CodeUnitCount(Cent), 1);
    CHECK_EQ(Utf16CodeUnitCount(Euro), 1);
    CHECK_EQ(Utf16CodeUnitCount(Rocket), 2);
    CHECK_EQ(Utf16CodeUnitCount(Mixed), 5);

    CHECK_EQ(Utf32CodeUnitCount(Ascii), 1);
    CHECK_EQ(Utf32CodeUnitCount(Cent), 1);
    CHECK_EQ(Utf32CodeUnitCount(Euro), 1);
    CHECK_EQ(Utf32CodeUnitCount(Rocket), 1);
    CHECK_EQ(Utf32CodeUnitCount(Mixed), 4);
}

TEST_CASE("UTF-16 transcoding widens each code point and splits the supplementary ones") {
    CHECK_EQ(TranscodeUtf8ToUtf16LE(Ascii), Bytes(std::string_view("\x41\x00", 2)));
    CHECK_EQ(TranscodeUtf8ToUtf16LE(Cent), Bytes(std::string_view("\xA2\x00", 2)));
    CHECK_EQ(TranscodeUtf8ToUtf16LE(Euro), Bytes(std::string_view("\xAC\x20", 2)));
    // U+1F680 - 0x10000 = 0xF680, so the pair is 0xD83D 0xDE80, little-endian byte by byte.
    CHECK_EQ(TranscodeUtf8ToUtf16LE(Rocket), Bytes(std::string_view("\x3D\xD8\x80\xDE", 4)));

    const auto mixed = TranscodeUtf8ToUtf16LE(Mixed);
    REQUIRE(mixed);
    CHECK_EQ(mixed->size(), 2 * *Utf16CodeUnitCount(Mixed));
    CHECK_EQ(*mixed, Bytes(std::string_view("\x41\x00\xA2\x00\xAC\x20\x3D\xD8\x80\xDE", 10)));
}

TEST_CASE("UTF-32 transcoding carries each code point whole") {
    CHECK_EQ(TranscodeUtf8ToUtf32LE(Ascii), Bytes(std::string_view("\x41\x00\x00\x00", 4)));
    CHECK_EQ(TranscodeUtf8ToUtf32LE(Euro), Bytes(std::string_view("\xAC\x20\x00\x00", 4)));
    CHECK_EQ(TranscodeUtf8ToUtf32LE(Rocket), Bytes(std::string_view("\x80\xF6\x01\x00", 4)));

    const auto mixed = TranscodeUtf8ToUtf32LE(Mixed);
    REQUIRE(mixed);
    CHECK_EQ(mixed->size(), 4 * *Utf32CodeUnitCount(Mixed));
    CHECK_EQ(*mixed, Bytes(std::string_view("\x41\x00\x00\x00"
                                            "\xA2\x00\x00\x00"
                                            "\xAC\x20\x00\x00"
                                            "\x80\xF6\x01\x00",
                                            16)));
}
