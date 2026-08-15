#include "Package/Identity.h"

#include <format>

namespace Rux {
static constexpr bool IsSeparator(const char c) noexcept {
    return c == '-' || c == '_';
}

static constexpr bool IsAlphanumeric(const char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

std::string NormalizeIdentity(const std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const char c : value) {
        if (c == '_') {
            normalized.push_back('-');
        }
        else if (c >= 'A' && c <= 'Z') {
            normalized.push_back(static_cast<char>(c - 'A' + 'a'));
        }
        else {
            normalized.push_back(c);
        }
    }
    return normalized;
}

std::string Describe(const IdentityError &error) {
    switch (error.kind) {
    case IdentityErrorKind::Empty:
        return "identity segment is empty";
    case IdentityErrorKind::TooLong:
        return std::format("identity segment is longer than {} bytes", identitySegmentMaxLength);
    case IdentityErrorKind::InvalidCharacter:
        return "identity segment allows only ASCII letters, digits, '-' and '_'";
    case IdentityErrorKind::LeadingSeparator:
        return "identity segment cannot start with '-' or '_'";
    case IdentityErrorKind::TrailingSeparator:
        return "identity segment cannot end with '-' or '_'";
    case IdentityErrorKind::AdjacentSeparators:
        return "identity segment cannot contain two adjacent separators";
    }
    return "invalid identity segment";
}

std::string DescribeIdentity(const std::string_view role, const std::string_view value, const IdentityError &error) {
    switch (error.kind) {
    case IdentityErrorKind::Empty:
        return std::format("{} is empty", role);
    case IdentityErrorKind::TooLong:
        return std::format("{} '{}' is {} bytes, exceeding the {}-byte limit", role, value, value.size(),
                           identitySegmentMaxLength);
    case IdentityErrorKind::InvalidCharacter:
        return std::format("{} '{}' contains '{}' at byte {}", role, value, value[error.offset], error.offset + 1);
    case IdentityErrorKind::LeadingSeparator:
        return std::format("{} '{}' starts with separator '{}'", role, value, value.front());
    case IdentityErrorKind::TrailingSeparator:
        return std::format("{} '{}' ends with separator '{}'", role, value, value.back());
    case IdentityErrorKind::AdjacentSeparators:
        return std::format("{} '{}' has adjacent separators at bytes {} and {}", role, value, error.offset,
                           error.offset + 1);
    }
    return std::format("{} '{}' is invalid", role, value);
}

std::expected<IdentitySegment, IdentityError> IdentitySegment::Parse(const std::string_view value) {
    if (value.empty()) {
        return std::unexpected(IdentityError{IdentityErrorKind::Empty, 0});
    }
    if (value.size() > identitySegmentMaxLength) {
        return std::unexpected(IdentityError{IdentityErrorKind::TooLong, identitySegmentMaxLength});
    }

    for (std::size_t i = 0; i < value.size(); ++i) {
        if (!IsAlphanumeric(value[i]) && !IsSeparator(value[i])) {
            return std::unexpected(IdentityError{IdentityErrorKind::InvalidCharacter, i});
        }
    }

    if (IsSeparator(value.front())) {
        return std::unexpected(IdentityError{IdentityErrorKind::LeadingSeparator, 0});
    }
    if (IsSeparator(value.back())) {
        return std::unexpected(IdentityError{IdentityErrorKind::TrailingSeparator, value.size() - 1});
    }

    for (std::size_t i = 1; i < value.size(); ++i) {
        if (IsSeparator(value[i - 1]) && IsSeparator(value[i])) {
            return std::unexpected(IdentityError{IdentityErrorKind::AdjacentSeparators, i});
        }
    }

    IdentitySegment segment;
    segment.text.assign(value);
    segment.normalized = NormalizeIdentity(value);
    return segment;
}
} // namespace Rux
