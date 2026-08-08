#pragma once

// Artifact digests.

#include <cstddef>
#include <string>
#include <string_view>

namespace Rux {
/// Number of hex characters in a SHA-256 digest.
inline constexpr std::size_t sha256HexLength = 64;

/**
 * @brief The SHA-256 digest of `data`, as lowercase hexadecimal.
 *
 * The registry publishes each artifact's digest beside its download route, so
 * an installed package can be checked against what the publisher uploaded
 * rather than trusted on the strength of the transport alone.
 */
[[nodiscard]] std::string Sha256Hex(std::string_view data);

/**
 * @brief Compare two digests without regard to case.
 *
 * The API documents lowercase hexadecimal, but a digest read from a manifest or
 * typed by a user may not be, and a case difference is never a mismatch.
 */
[[nodiscard]] bool DigestsEqual(std::string_view left, std::string_view right);
} // namespace Rux
