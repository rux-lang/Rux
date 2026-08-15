#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Rux {
/// Maximum number of comparators accepted in one version requirement.
inline constexpr std::size_t versionRangeMaxComparators = 32;

/**
 * @brief Why a semantic version or version requirement was rejected.
 */
enum class VersionErrorKind {
    Empty,
    MissingComponent,
    LeadingZero,
    NumericOverflow,
    InvalidCharacter,
    EmptyIdentifier,
    TooManyComponents,
    ComponentAfterWildcard,
    SuffixRequiresCompleteVersion,
    MissingOperand,
    EmptyComparator,
    WildcardMustStandAlone,
    TooManyComparators,
};

/**
 * @brief A rejected version or requirement, located inside the offending text.
 */
struct VersionError {
    VersionErrorKind kind = VersionErrorKind::Empty;

    /// Byte offset of the offending character within the parsed text.
    std::size_t offset = 0;

    /// The component or section at fault, for example "minor" or "prerelease".
    std::string_view section;
};

/**
 * @brief Explain a rejected version or requirement for a manifest diagnostic.
 */
[[nodiscard]] std::string Describe(const VersionError &error);

/// Explain a version rejection while naming the rejected value and character.
[[nodiscard]] std::string DescribeVersion(std::string_view role, std::string_view value, const VersionError &error);

/**
 * @brief A strict Semantic Versioning 2.0.0 value.
 *
 * The complete source text is retained because it forms publication identity:
 * `1.0.0+linux` and `1.0.0+windows` are distinct releases. Semantic Versioning
 * precedence ignores build metadata, so ordering candidates for a requirement
 * uses ComparePrecedence rather than equality.
 */
class SemanticVersion {
public:
    SemanticVersion() = default;

    /**
     * @brief Parse a complete `major.minor.patch` version.
     * @param value Candidate version text, without a leading `v`
     * @return The version, or the reason it was rejected
     */
    [[nodiscard]] static std::expected<SemanticVersion, VersionError> Parse(std::string_view value);

    /// The complete source text, including any build metadata.
    [[nodiscard]] const std::string &Text() const noexcept {
        return text;
    }

    [[nodiscard]] std::uint64_t Major() const noexcept {
        return major;
    }

    [[nodiscard]] std::uint64_t Minor() const noexcept {
        return minor;
    }

    [[nodiscard]] std::uint64_t Patch() const noexcept {
        return patch;
    }

    [[nodiscard]] const std::optional<std::string> &Prerelease() const noexcept {
        return prerelease;
    }

    [[nodiscard]] const std::optional<std::string> &Build() const noexcept {
        return build;
    }

    [[nodiscard]] bool IsPrerelease() const noexcept {
        return prerelease.has_value();
    }

    [[nodiscard]] bool Empty() const noexcept {
        return text.empty();
    }

    /**
     * @brief Order two versions by Semantic Versioning precedence.
     *
     * Build metadata is ignored, so `1.0.0+a` and `1.0.0+b` compare equal even
     * though they are different releases.
     *
     * @return A negative value, zero, or a positive value
     */
    [[nodiscard]] static int ComparePrecedence(const SemanticVersion &left, const SemanticVersion &right);

    /// Identity comparison over the complete source text, build metadata included.
    [[nodiscard]] friend bool operator==(const SemanticVersion &left, const SemanticVersion &right) noexcept {
        return left.text == right.text;
    }

private:
    std::string text;
    std::uint64_t major = 0;
    std::uint64_t minor = 0;
    std::uint64_t patch = 0;
    std::optional<std::string> prerelease;
    std::optional<std::string> build;
};

/**
 * @brief The operator of one requirement comparator.
 */
enum class VersionOperator {
    Exact,
    Greater,
    GreaterOrEqual,
    Less,
    LessOrEqual,
    Tilde,
    Caret,
    Wildcard,
};

/**
 * @brief One comparator of a version requirement.
 *
 * Operands may be partial: `^1.0` leaves `patch` unset and `1.*` leaves both
 * `minor` and `patch` unset. An operand with no explicit operator is a caret
 * requirement, so `1.2.3` means `^1.2.3`.
 */
struct VersionComparator {
    VersionOperator op = VersionOperator::Caret;
    std::uint64_t major = 0;
    std::optional<std::uint64_t> minor;
    std::optional<std::uint64_t> patch;
    std::optional<std::string> prerelease;

    [[nodiscard]] bool Matches(const SemanticVersion &version) const;
};

/**
 * @brief An intersection of version comparators.
 *
 * A requirement is satisfied when every comparator matches. A standalone `*`,
 * `x` or `X` produces no comparators and matches any stable version.
 */
class VersionRange {
public:
    VersionRange() = default;

    /**
     * @brief Parse a comma-separated intersection of comparators.
     * @param value Candidate requirement text
     * @return The requirement, or the reason it was rejected
     */
    [[nodiscard]] static std::expected<VersionRange, VersionError> Parse(std::string_view value);

    /// The original valid spelling, preserved for canonical serialization.
    [[nodiscard]] const std::string &Text() const noexcept {
        return text;
    }

    [[nodiscard]] const std::vector<VersionComparator> &Comparators() const noexcept {
        return comparators;
    }

    /// Whether this requirement accepts any stable version.
    [[nodiscard]] bool IsWildcard() const noexcept {
        return comparators.empty();
    }

    /**
     * @brief Whether @p version satisfies every comparator.
     *
     * A prerelease is eligible only when some comparator names the same
     * `major.minor.patch` and carries a prerelease operand of its own, so
     * `^1.2.3` never accepts `1.4.0-beta` while `^1.2.3-alpha` can.
     */
    [[nodiscard]] bool Matches(const SemanticVersion &version) const;

private:
    std::string text;
    std::vector<VersionComparator> comparators;
};
} // namespace Rux
