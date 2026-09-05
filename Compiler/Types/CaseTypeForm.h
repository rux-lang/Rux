#pragma once

#include <cstdint>
#include <string_view>

namespace Rux {
/// The source declaration form of a scoped case-bearing type. Payload shape is deliberately absent: an all-unit
/// variant remains a variant, and a legacy payload enum remains an enum until source migration is complete.
enum class CaseTypeForm : std::uint8_t {
    Enumeration,
    Variant,
};

[[nodiscard]] constexpr std::string_view CaseTypeKeyword(const CaseTypeForm form) noexcept {
    return form == CaseTypeForm::Variant ? "variant" : "enum";
}
} // namespace Rux
