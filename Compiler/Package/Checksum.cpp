#include "Package/Checksum.h"

#include "Crypto/Sha256.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace Rux {
namespace {
[[nodiscard]] char LowerHexDigit(const std::uint32_t nibble) {
    return static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
}

[[nodiscard]] char FoldHexCase(const char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}
} // namespace

std::string Sha256Hex(const std::string_view data) {
    const auto bytes = std::span(reinterpret_cast<const std::uint8_t *>(data.data()), data.size());
    const Crypto::Sha256Digest hash = Crypto::Sha256(bytes);
    std::string digest;
    digest.reserve(sha256HexLength);
    for (const std::uint8_t byte : hash) {
        digest.push_back(LowerHexDigit(byte >> 4U));
        digest.push_back(LowerHexDigit(byte & 0xFU));
    }
    return digest;
}

bool DigestsEqual(const std::string_view left, const std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (FoldHexCase(left[i]) != FoldHexCase(right[i])) {
            return false;
        }
    }
    return true;
}
} // namespace Rux
