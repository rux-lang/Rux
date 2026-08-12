#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::MachO {
inline constexpr std::uint64_t codeSignaturePageSize = 4096;

/**
 * @brief Return the deterministic embedded-signature size for a signed prefix.
 *
 * Returns zero and describes the malformed input in `error` when the
 * CodeDirectory cannot represent the prefix or identifier.
 */
[[nodiscard]] std::uint64_t AdHocCodeSignatureSize(std::uint64_t codeLimit, std::string_view identifier,
                                                   std::string &error);

/**
 * @brief Build an Apple embedded-signature superblob for `signedPrefix`.
 *
 * Every complete or partial 4 KiB page in the prefix contributes one SHA-256
 * code slot. All multi-byte signature fields use the big-endian encoding
 * required by Apple's code-signing blob format.
 */
[[nodiscard]] bool BuildAdHocCodeSignature(std::span<const std::uint8_t> signedPrefix, std::string_view identifier,
                                           std::uint64_t executableSegmentBase, std::uint64_t executableSegmentLimit,
                                           std::uint64_t executableSegmentFlags, std::vector<std::uint8_t> &signature,
                                           std::string &error);
} // namespace Rux::MachO
