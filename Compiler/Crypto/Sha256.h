#pragma once

// Narrow, host-independent SHA-256 primitive shared by package verification
// and binary-format writers.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Rux::Crypto {
inline constexpr std::size_t sha256DigestLength = 32;
using Sha256Digest = std::array<std::uint8_t, sha256DigestLength>;

/** @brief Compute the SHA-256 digest of an arbitrary byte range. */
[[nodiscard]] Sha256Digest Sha256(std::span<const std::uint8_t> data);
} // namespace Rux::Crypto
