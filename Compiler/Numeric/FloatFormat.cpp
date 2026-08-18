#include "Numeric/FloatFormat.h"

#include <algorithm>
#include <array>

namespace Rux {
namespace {
constexpr std::array<FloatFormat, 8> Formats{{
    {"binary8-e4m3", 8, 1, 4, 4, 7, false},
    {"binary16", 16, 2, 5, 11, 15, false},
    {"binary32", 32, 4, 8, 24, 127, false},
    {"binary64", 64, 8, 11, 53, 1023, false},
    {"extended80", 80, 16, 15, 64, 16383, true},
    {"binary128", 128, 16, 15, 113, 16383, false},
    {"binary256", 256, 32, 19, 237, 262143, false},
    {"binary512", 512, 64, 23, 489, 4194303, false},
}};
} // namespace

std::span<const FloatFormat> FloatFormats() noexcept {
    return Formats;
}

const FloatFormat *FindFloatFormat(const std::uint32_t valueBits) noexcept {
    const auto found = std::ranges::find(Formats, valueBits, &FloatFormat::valueBits);
    return found == Formats.end() ? nullptr : &*found;
}
} // namespace Rux
