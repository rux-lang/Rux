#include "Package/Version.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <utility>

namespace Rux {
namespace {
constexpr bool IsDigit(const char c) noexcept {
    return c >= '0' && c <= '9';
}

constexpr bool IsAlphanumeric(const char c) noexcept {
    return IsDigit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/// Whether a pre-release identifier is numeric, which decides how it orders: SemVer compares numeric identifiers
/// numerically and everything else lexically.
constexpr bool IsNumericIdentifier(const std::string_view value) noexcept {
    return std::ranges::all_of(value, IsDigit);
}

constexpr bool IsWildcardComponent(const std::string_view value) noexcept {
    return value == "*" || value == "x" || value == "X";
}

std::unexpected<VersionError> Fail(const VersionErrorKind kind, const std::size_t offset,
                                   const std::string_view section = {}) {
    return std::unexpected(VersionError{kind, offset, section});
}

/// A numeric component: ASCII digits only, no leading zero, no overflow.
std::expected<std::uint64_t, VersionError> ParseNumber(const std::string_view value, const std::string_view section,
                                                       const std::size_t index) {
    if (value.empty()) {
        return Fail(VersionErrorKind::MissingComponent, index, section);
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (!IsDigit(value[i])) {
            return Fail(VersionErrorKind::InvalidCharacter, index + i, section);
        }
    }
    if (value.size() > 1 && value.front() == '0') {
        return Fail(VersionErrorKind::LeadingZero, index, section);
    }
    std::uint64_t number = 0;
    if (const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
        error != std::errc{} || end != value.data() + value.size()) {
        return Fail(VersionErrorKind::NumericOverflow, index, section);
    }
    return number;
}

/// One dot-separated prerelease or build identifier.
std::expected<void, VersionError> ValidateIdentifier(const std::string_view value, const std::string_view section,
                                                     const std::size_t index, const bool rejectNumericLeadingZero) {
    if (value.empty()) {
        return Fail(VersionErrorKind::EmptyIdentifier, index, section);
    }
    if (rejectNumericLeadingZero && value.size() > 1 && value.front() == '0' && IsNumericIdentifier(value)) {
        return Fail(VersionErrorKind::LeadingZero, index, section);
    }
    return {};
}

/// A dot-separated identifier list. Identifiers are ASCII alphanumerics and `-`.
std::expected<void, VersionError> ValidateIdentifiers(const std::string_view value, const std::string_view section,
                                                      const std::size_t baseIndex,
                                                      const bool rejectNumericLeadingZero) {
    if (value.empty()) {
        return Fail(VersionErrorKind::EmptyIdentifier, baseIndex, section);
    }

    std::size_t segmentStart = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '.') {
            if (const auto result = ValidateIdentifier(value.substr(segmentStart, i - segmentStart), section,
                                                       baseIndex + segmentStart, rejectNumericLeadingZero);
                !result) {
                return result;
            }
            segmentStart = i + 1;
        }
        else if (!IsAlphanumeric(value[i]) && value[i] != '-') {
            return Fail(VersionErrorKind::InvalidCharacter, baseIndex + i, section);
        }
    }
    return ValidateIdentifier(value.substr(segmentStart), section, baseIndex + segmentStart, rejectNumericLeadingZero);
}

struct VersionSuffix {
    std::optional<std::string> prerelease;
    std::optional<std::string> build;
};

/// Everything from the first `-` or `+` onward: `-prerelease`, `+build`, or both.
///
/// @param suffixIndex Offset of the suffix within the whole version string, so a rejection points at the real column
std::expected<VersionSuffix, VersionError> ParseSuffix(const std::string_view value, const std::size_t suffixIndex) {
    if (suffixIndex == value.size()) {
        return VersionSuffix{};
    }

    const std::string_view suffix = value.substr(suffixIndex);
    if (suffix.front() != '-') {
        const std::string_view build = suffix.substr(1);
        if (const auto result = ValidateIdentifiers(build, "build", suffixIndex + 1, false); !result) {
            return std::unexpected(result.error());
        }
        return VersionSuffix{std::nullopt, std::string(build)};
    }

    const std::string_view rest = suffix.substr(1);
    const auto buildOffset = rest.find('+');
    const std::string_view prerelease = rest.substr(0, buildOffset);
    if (const auto result = ValidateIdentifiers(prerelease, "prerelease", suffixIndex + 1, true); !result) {
        return std::unexpected(result.error());
    }
    if (buildOffset == std::string_view::npos) {
        return VersionSuffix{std::string(prerelease), std::nullopt};
    }

    const std::string_view build = rest.substr(buildOffset + 1);
    const std::size_t buildIndex = suffixIndex + buildOffset + 2;
    if (const auto result = ValidateIdentifiers(build, "build", buildIndex, false); !result) {
        return std::unexpected(result.error());
    }
    return VersionSuffix{std::string(prerelease), std::string(build)};
}

/// Semantic Versioning 2.0.0 section 11: numeric identifiers compare numerically, alphanumeric identifiers compare in
/// ASCII order, and a numeric identifier always has lower precedence than an alphanumeric one.
std::strong_ordering CompareIdentifier(const std::string_view left, const std::string_view right) {
    const bool leftNumeric = IsNumericIdentifier(left);
    const bool rightNumeric = IsNumericIdentifier(right);
    if (leftNumeric && rightNumeric) {
        if (left.size() != right.size()) {
            return left.size() <=> right.size();
        }
        return left <=> right;
    }
    if (leftNumeric) {
        return std::strong_ordering::less;
    }
    if (rightNumeric) {
        return std::strong_ordering::greater;
    }
    return left <=> right;
}

/// Order two dot-separated pre-release lists, field by field.
///
/// A larger set of identifiers wins when every preceding one is equal, so `1.0.0-rc` precedes `1.0.0-rc.1`.
std::strong_ordering CompareIdentifierLists(const std::string_view left, const std::string_view right) {
    std::size_t leftPos = 0;
    std::size_t rightPos = 0;
    while (leftPos <= left.size() && rightPos <= right.size()) {
        const auto leftDot = left.find('.', leftPos);
        const auto rightDot = right.find('.', rightPos);
        const auto leftEnd = leftDot == std::string_view::npos ? left.size() : leftDot;
        const auto rightEnd = rightDot == std::string_view::npos ? right.size() : rightDot;

        if (const auto ordering =
                CompareIdentifier(left.substr(leftPos, leftEnd - leftPos), right.substr(rightPos, rightEnd - rightPos));
            ordering != std::strong_ordering::equal) {
            return ordering;
        }

        const bool leftDone = leftDot == std::string_view::npos;
        const bool rightDone = rightDot == std::string_view::npos;
        if (leftDone || rightDone) {
            if (leftDone && rightDone) {
                return std::strong_ordering::equal;
            }
            return leftDone ? std::strong_ordering::less : std::strong_ordering::greater;
        }
        leftPos = leftEnd + 1;
        rightPos = rightEnd + 1;
    }
    return std::strong_ordering::equal;
}

/// A version without a prerelease outranks one with it.
std::strong_ordering ComparePrerelease(const std::optional<std::string> &left,
                                       const std::optional<std::string> &right) {
    if (!left && !right) {
        return std::strong_ordering::equal;
    }
    if (!left) {
        return std::strong_ordering::greater;
    }
    if (!right) {
        return std::strong_ordering::less;
    }
    return CompareIdentifierLists(*left, *right);
}

struct PartialVersion {
    std::uint64_t major = 0;
    std::optional<std::uint64_t> minor;
    std::optional<std::uint64_t> patch;
    std::optional<std::string> prerelease;
    bool hasWildcard = false;
};

/// Byte offset of the numeric component at `position` within a dotted core.
std::size_t ComponentIndex(const std::string_view core, const std::size_t position) {
    if (position == 0) {
        return 0;
    }
    std::size_t found = 0;
    std::size_t pos = 0;
    while (found < position) {
        const auto dot = core.find('.', pos);
        if (dot == std::string_view::npos) {
            return core.size();
        }
        pos = dot + 1;
        ++found;
    }
    return pos;
}

std::expected<PartialVersion, VersionError> ParsePartialVersion(const std::string_view value,
                                                                const std::size_t baseIndex) {
    const auto suffixIndex = std::min(value.find('-'), value.find('+'));
    const std::string_view core = value.substr(0, suffixIndex == std::string_view::npos ? value.size() : suffixIndex);

    std::vector<std::string_view> parts;
    for (std::size_t pos = 0;;) {
        const auto dot = core.find('.', pos);
        if (dot == std::string_view::npos) {
            parts.push_back(core.substr(pos));
            break;
        }
        parts.push_back(core.substr(pos, dot - pos));
        pos = dot + 1;
    }
    if (parts.size() > 3) {
        return Fail(VersionErrorKind::TooManyComponents, baseIndex + ComponentIndex(core, 3), "patch");
    }
    if (parts.front().empty() || IsWildcardComponent(parts.front())) {
        return Fail(VersionErrorKind::MissingComponent, baseIndex, "major");
    }

    PartialVersion partial;
    const auto major = ParseNumber(parts.front(), "major", baseIndex);
    if (!major) {
        return std::unexpected(major.error());
    }
    partial.major = *major;

    constexpr std::string_view sections[] = {"major", "minor", "patch"};
    bool minorWildcard = false;
    for (std::size_t position = 1; position < parts.size(); ++position) {
        const std::size_t index = baseIndex + ComponentIndex(core, position);
        if (parts[position].empty()) {
            return Fail(VersionErrorKind::MissingComponent, index, sections[position]);
        }
        if (IsWildcardComponent(parts[position])) {
            partial.hasWildcard = true;
            minorWildcard = minorWildcard || position == 1;
            continue;
        }
        if (position == 2 && minorWildcard) {
            return Fail(VersionErrorKind::ComponentAfterWildcard, index, sections[position]);
        }
        const auto number = ParseNumber(parts[position], sections[position], index);
        if (!number) {
            return std::unexpected(number.error());
        }
        (position == 1 ? partial.minor : partial.patch) = *number;
    }

    if (suffixIndex == std::string_view::npos) {
        return partial;
    }
    if (!partial.patch || partial.hasWildcard) {
        return Fail(VersionErrorKind::SuffixRequiresCompleteVersion, baseIndex + suffixIndex, "prerelease");
    }
    auto suffix = ParseSuffix(value, suffixIndex);
    if (!suffix) {
        return std::unexpected(
            VersionError{suffix.error().kind, baseIndex + suffix.error().offset, suffix.error().section});
    }
    // Build metadata never participates in matching, so only the prerelease is
    // retained. It is accepted rather than rejected because the registry
    // accepts it, and a requirement the registry publishes must load locally.
    partial.prerelease = std::move(suffix->prerelease);
    return partial;
}

struct ParsedOperator {
    VersionOperator op = VersionOperator::Caret;
    bool explicitly = false;
    std::size_t length = 0;
};

/// Read the comparison operator a requirement opens with, defaulting to an exact match when none is written.
ParsedOperator ParseOperator(const std::string_view value) {
    constexpr std::pair<std::string_view, VersionOperator> spellings[] = {
        {">=", VersionOperator::GreaterOrEqual}, {"<=", VersionOperator::LessOrEqual}, {"=", VersionOperator::Exact},
        {">", VersionOperator::Greater},         {"<", VersionOperator::Less},         {"~", VersionOperator::Tilde},
        {"^", VersionOperator::Caret},
    };
    for (const auto &[spelling, op] : spellings) {
        if (value.starts_with(spelling)) {
            return {op, true, spelling.size()};
        }
    }
    return {VersionOperator::Caret, false, 0};
}

std::expected<VersionComparator, VersionError> ParseComparator(const std::string_view value,
                                                               const std::size_t baseIndex) {
    const auto parsed = ParseOperator(value);
    const std::string_view rest = value.substr(parsed.length);
    const auto spaces = rest.find_first_not_of(' ');
    if (spaces == std::string_view::npos) {
        return Fail(VersionErrorKind::MissingOperand, baseIndex + value.size(), "major");
    }
    const std::size_t operandIndex = baseIndex + parsed.length + spaces;

    const auto partial = ParsePartialVersion(rest.substr(spaces), operandIndex);
    if (!partial) {
        return std::unexpected(partial.error());
    }

    VersionComparator comparator;
    comparator.op = parsed.explicitly    ? parsed.op
                  : partial->hasWildcard ? VersionOperator::Wildcard
                                         : VersionOperator::Caret;
    comparator.major = partial->major;
    comparator.minor = partial->minor;
    comparator.patch = partial->patch;
    comparator.prerelease = partial->prerelease;
    return comparator;
}
} // namespace

std::string Describe(const VersionError &error) {
    const std::string section = error.section.empty() ? std::string() : std::format(" in the {}", error.section);
    switch (error.kind) {
    case VersionErrorKind::Empty:
        return "version is empty";
    case VersionErrorKind::MissingComponent:
        return std::format("missing or empty numeric component{}", section);
    case VersionErrorKind::LeadingZero:
        return std::format("leading zero{}", section);
    case VersionErrorKind::NumericOverflow:
        return std::format("numeric component is too large{}", section);
    case VersionErrorKind::InvalidCharacter:
        return std::format("invalid character{}", section);
    case VersionErrorKind::EmptyIdentifier:
        return std::format("empty identifier{}", section);
    case VersionErrorKind::TooManyComponents:
        return "version has more than three numeric components";
    case VersionErrorKind::ComponentAfterWildcard:
        return "a numeric component cannot follow a wildcard";
    case VersionErrorKind::SuffixRequiresCompleteVersion:
        return "prerelease and build metadata require a complete major.minor.patch operand";
    case VersionErrorKind::MissingOperand:
        return "comparator has no version operand";
    case VersionErrorKind::EmptyComparator:
        return "empty comparator";
    case VersionErrorKind::WildcardMustStandAlone:
        return "'*' cannot be combined with another comparator";
    case VersionErrorKind::TooManyComparators:
        return std::format("requirement has more than {} comparators", versionRangeMaxComparators);
    }
    return "invalid version";
}

std::string DescribeVersion(const std::string_view role, const std::string_view value, const VersionError &error) {
    if (error.kind == VersionErrorKind::InvalidCharacter && error.offset < value.size()) {
        return std::format("{} '{}' contains invalid character '{}' at byte {}{}", role, value, value[error.offset],
                           error.offset + 1,
                           error.section.empty() ? std::string() : std::format(" in the {}", error.section));
    }
    return std::format("{} '{}': {}", role, value, Describe(error));
}

std::expected<SemanticVersion, VersionError> SemanticVersion::Parse(const std::string_view value) {
    if (value.empty()) {
        return Fail(VersionErrorKind::Empty, 0);
    }

    const auto suffixIndex = std::min(value.find('-'), value.find('+'));
    const std::string_view core = value.substr(0, suffixIndex == std::string_view::npos ? value.size() : suffixIndex);

    const auto firstDot = core.find('.');
    if (firstDot == std::string_view::npos) {
        return Fail(VersionErrorKind::MissingComponent, core.size(), "minor");
    }
    const auto secondDot = core.find('.', firstDot + 1);
    if (secondDot == std::string_view::npos) {
        return Fail(VersionErrorKind::MissingComponent, core.size(), "patch");
    }
    if (core.find('.', secondDot + 1) != std::string_view::npos) {
        return Fail(VersionErrorKind::TooManyComponents, secondDot + 1, "patch");
    }

    const auto majorValue = ParseNumber(core.substr(0, firstDot), "major", 0);
    if (!majorValue) {
        return std::unexpected(majorValue.error());
    }
    const auto minorValue = ParseNumber(core.substr(firstDot + 1, secondDot - firstDot - 1), "minor", firstDot + 1);
    if (!minorValue) {
        return std::unexpected(minorValue.error());
    }
    const auto patchValue = ParseNumber(core.substr(secondDot + 1), "patch", secondDot + 1);
    if (!patchValue) {
        return std::unexpected(patchValue.error());
    }

    auto suffix = ParseSuffix(value, suffixIndex == std::string_view::npos ? value.size() : suffixIndex);
    if (!suffix) {
        return std::unexpected(suffix.error());
    }

    SemanticVersion version;
    version.text.assign(value);
    version.major = *majorValue;
    version.minor = *minorValue;
    version.patch = *patchValue;
    version.prerelease = std::move(suffix->prerelease);
    version.build = std::move(suffix->build);
    return version;
}

int SemanticVersion::ComparePrecedence(const SemanticVersion &left, const SemanticVersion &right) {
    auto ordering = left.major <=> right.major;
    if (ordering == std::strong_ordering::equal) {
        ordering = left.minor <=> right.minor;
    }
    if (ordering == std::strong_ordering::equal) {
        ordering = left.patch <=> right.patch;
    }
    if (ordering == std::strong_ordering::equal) {
        ordering = ComparePrerelease(left.prerelease, right.prerelease);
    }
    if (ordering == std::strong_ordering::less) {
        return -1;
    }
    return ordering == std::strong_ordering::greater ? 1 : 0;
}

bool VersionComparator::Matches(const SemanticVersion &version) const {
    const auto prereleaseOrdering = ComparePrerelease(version.Prerelease(), prerelease);

    const auto matchesExact = [&] {
        return version.Major() == major && (!minor || version.Minor() == *minor) &&
               (!patch || version.Patch() == *patch) && prereleaseOrdering == std::strong_ordering::equal;
    };

    // A partial operand has no fixed boundary below the components it omits, so
    // `>1` and `<1` never match a 1.x version.
    const auto matchesGreater = [&] {
        if (version.Major() != major) {
            return version.Major() > major;
        }
        if (!minor) {
            return false;
        }
        if (version.Minor() != *minor) {
            return version.Minor() > *minor;
        }
        if (!patch) {
            return false;
        }
        if (version.Patch() != *patch) {
            return version.Patch() > *patch;
        }
        return prereleaseOrdering == std::strong_ordering::greater;
    };

    const auto matchesLess = [&] {
        if (version.Major() != major) {
            return version.Major() < major;
        }
        if (!minor) {
            return false;
        }
        if (version.Minor() != *minor) {
            return version.Minor() < *minor;
        }
        if (!patch) {
            return false;
        }
        if (version.Patch() != *patch) {
            return version.Patch() < *patch;
        }
        return prereleaseOrdering == std::strong_ordering::less;
    };

    switch (op) {
    case VersionOperator::Exact:
    case VersionOperator::Wildcard:
        return matchesExact();
    case VersionOperator::Greater:
        return matchesGreater();
    case VersionOperator::GreaterOrEqual:
        return matchesExact() || matchesGreater();
    case VersionOperator::Less:
        return matchesLess();
    case VersionOperator::LessOrEqual:
        return matchesExact() || matchesLess();
    case VersionOperator::Tilde:
        // `~1.2.3` allows patch updates: >=1.2.3, <1.3.0.
        return version.Major() == major && (!minor || version.Minor() == *minor) &&
               (!patch || version.Patch() >= *patch) && prereleaseOrdering != std::strong_ordering::less;
    case VersionOperator::Caret:
        break;
    }

    // `^1.2.3` allows minor and patch updates below 2.0.0, `^0.2.3` allows patch
    // updates below 0.3.0, and `^0.0.3` allows nothing but 0.0.3.
    if (version.Major() != major) {
        return false;
    }
    if (!minor) {
        return true;
    }
    if (!patch) {
        return major > 0 ? version.Minor() >= *minor : version.Minor() == *minor;
    }
    // The leftmost non-zero component is the one held fixed.
    bool coreMatches = false;
    if (major > 0) {
        coreMatches = version.Minor() > *minor || (version.Minor() == *minor && version.Patch() >= *patch);
    }
    else if (*minor > 0) {
        coreMatches = version.Minor() == *minor && version.Patch() >= *patch;
    }
    else {
        coreMatches = version.Minor() == *minor && version.Patch() == *patch;
    }
    return coreMatches && prereleaseOrdering != std::strong_ordering::less;
}

std::expected<VersionRange, VersionError> VersionRange::Parse(const std::string_view value) {
    const auto firstNonSpace = value.find_first_not_of(' ');
    if (firstNonSpace == std::string_view::npos) {
        return Fail(VersionErrorKind::Empty, 0);
    }
    const auto lastNonSpace = value.find_last_not_of(' ');
    const std::string_view trimmed = value.substr(firstNonSpace, lastNonSpace - firstNonSpace + 1);

    VersionRange range;
    range.text.assign(value);
    if (IsWildcardComponent(trimmed)) {
        return range;
    }

    for (std::size_t start = 0;;) {
        const auto comma = trimmed.find(',', start);
        const auto end = comma == std::string_view::npos ? trimmed.size() : comma;
        const std::string_view raw = trimmed.substr(start, end - start);

        const auto partStart = raw.find_first_not_of(' ');
        if (partStart == std::string_view::npos) {
            return Fail(VersionErrorKind::EmptyComparator, firstNonSpace + start);
        }
        const auto partEnd = raw.find_last_not_of(' ');
        const std::string_view part = raw.substr(partStart, partEnd - partStart + 1);
        const std::size_t partIndex = firstNonSpace + start + partStart;

        if (IsWildcardComponent(part)) {
            return Fail(VersionErrorKind::WildcardMustStandAlone, partIndex);
        }
        if (range.comparators.size() == versionRangeMaxComparators) {
            return Fail(VersionErrorKind::TooManyComparators, partIndex);
        }

        auto comparator = ParseComparator(part, partIndex);
        if (!comparator) {
            return std::unexpected(comparator.error());
        }
        range.comparators.push_back(std::move(*comparator));

        if (comma == std::string_view::npos) {
            break;
        }
        start = end + 1;
        if (start == trimmed.size()) {
            return Fail(VersionErrorKind::EmptyComparator, firstNonSpace + start);
        }
    }
    return range;
}

bool VersionRange::Matches(const SemanticVersion &version) const {
    if (!std::ranges::all_of(comparators, [&](const VersionComparator &c) { return c.Matches(version); })) {
        return false;
    }
    // A prerelease is only in play when a comparator opts into that exact
    // release line, so `>=1.2.0, <2.0.0` never silently selects `1.9.0-rc.1`.
    if (!version.IsPrerelease()) {
        return true;
    }
    return std::ranges::any_of(comparators, [&](const VersionComparator &c) {
        return c.major == version.Major() && c.minor == version.Minor() && c.patch == version.Patch() &&
               c.prerelease.has_value();
    });
}
} // namespace Rux
