#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace Rux {
/// Maximum number of ASCII bytes in one identity segment.
inline constexpr std::size_t identitySegmentMaxLength = 64;

/**
 * @brief Why an identity segment was rejected.
 */
enum class IdentityErrorKind {
    Empty,
    TooLong,
    InvalidCharacter,
    LeadingSeparator,
    TrailingSeparator,
    AdjacentSeparators,
};

/**
 * @brief A rejected identity segment, located inside the offending text.
 */
struct IdentityError {
    IdentityErrorKind kind = IdentityErrorKind::Empty;

    /// Byte offset of the offending character, or 0 when the whole value is at fault.
    std::size_t offset = 0;
};

/**
 * @brief Explain a rejected identity segment for a manifest diagnostic.
 */
[[nodiscard]] std::string Describe(const IdentityError &error);

/**
 * @brief Describe an identity rejection while naming its role and value.
 * @param role User-facing role such as `package name` or `namespace`
 */
[[nodiscard]] std::string DescribeIdentity(std::string_view role, std::string_view value, const IdentityError &error);

/// The permitted spelling shared by identity diagnostics.
inline constexpr std::string_view identitySegmentConstraint =
    "use 1 to 64 ASCII letters or digits separated by single '-' or '_' characters";

/**
 * @brief A display-preserving segment of a package identity.
 *
 * A segment is 1 to 64 bytes of ASCII alphanumeric characters separated by
 * single `-` or `_` characters. It cannot start or end with a separator and
 * cannot contain two adjacent separators.
 *
 * Equality, ordering and hashing use a normalized form that lowercases ASCII
 * letters and folds `_` to `-`, so `Rux/My_Pkg` and `rux/my-pkg` identify the
 * same package while each manifest keeps its own spelling for display.
 */
class IdentitySegment {
public:
    IdentitySegment() = default;

    /**
     * @brief Validate and construct an identity segment.
     * @param value Candidate segment text
     * @return The segment, or the reason it was rejected
     */
    [[nodiscard]] static std::expected<IdentitySegment, IdentityError> Parse(std::string_view value);

    /// The original, display-preserved spelling.
    [[nodiscard]] const std::string &Text() const noexcept {
        return text;
    }

    /// The lowercase, underscore-folded lookup key.
    [[nodiscard]] const std::string &Normalized() const noexcept {
        return normalized;
    }

    [[nodiscard]] bool Empty() const noexcept {
        return text.empty();
    }

    [[nodiscard]] friend bool operator==(const IdentitySegment &left, const IdentitySegment &right) noexcept {
        return left.normalized == right.normalized;
    }

    [[nodiscard]] friend auto operator<=>(const IdentitySegment &left, const IdentitySegment &right) noexcept {
        return left.normalized <=> right.normalized;
    }

private:
    std::string text;
    std::string normalized;
};

/**
 * @brief Lowercase ASCII letters and fold `_` to `-`.
 *
 * Exposed so callers can normalize a candidate name without first constructing
 * a segment, for example when reporting a collision between rejected values.
 */
[[nodiscard]] std::string NormalizeIdentity(std::string_view value);
} // namespace Rux
