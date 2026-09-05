#pragma once

#include <string_view>

namespace Rux::Testing {
// These fixtures test representations and operations with a local declaration provider.
// Import availability and dependency-free behavior have separate intrinsic declaration tests.
inline constexpr std::string_view StringDeclarations = R"(
intrinsic struct string8 { pub data: *char8; pub length: uint; }
intrinsic struct string16 { pub data: *char16; pub length: uint; }
intrinsic struct string32 { pub data: *char32; pub length: uint; }
type string = string8;
)";
} // namespace Rux::Testing
