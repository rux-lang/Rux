// The ad-hoc code signature a Mach-O image carries. Required on some Apple
// targets before an image will load at all, which is why it is emitted here
// rather than left to a separate tool.

#include "Linker/MachO/CodeSignature.h"

#include "Crypto/Sha256.h"

#include <algorithm>
#include <limits>

namespace Rux::MachO {
namespace {
constexpr std::uint32_t kEmbeddedSignatureMagic = 0xFADE'0CC0;
constexpr std::uint32_t kCodeDirectoryMagic = 0xFADE'0C02;
constexpr std::uint32_t kCodeDirectoryVersion = 0x0002'0400;
constexpr std::uint32_t kAdHocLinkerSigned = 0x0002'0002;
constexpr std::uint32_t kCodeDirectorySlot = 0;
constexpr std::uint32_t kBlobHeadersSize = 24;
constexpr std::uint32_t kCodeDirectoryHeaderSize = 88;
constexpr std::uint8_t kSha256Type = 2;
constexpr std::uint8_t kPageSizePower = 12;

/// Code signature structures are big-endian, unlike the rest of a Mach-O image, so these helpers exist separately from
/// the writer's native-order ones.
void WriteBig32(std::vector<std::uint8_t> &buffer, const std::uint32_t value) {
    buffer.push_back(static_cast<std::uint8_t>(value >> 24U));
    buffer.push_back(static_cast<std::uint8_t>(value >> 16U));
    buffer.push_back(static_cast<std::uint8_t>(value >> 8U));
    buffer.push_back(static_cast<std::uint8_t>(value));
}

void WriteBig64(std::vector<std::uint8_t> &buffer, const std::uint64_t value) {
    WriteBig32(buffer, static_cast<std::uint32_t>(value >> 32U));
    WriteBig32(buffer, static_cast<std::uint32_t>(value));
}

std::uint64_t AlignUp16(const std::uint64_t value) {
    return (value + 15U) & ~std::uint64_t{15};
}
} // namespace

std::uint64_t AdHocCodeSignatureSize(const std::uint64_t codeLimit, const std::string_view identifier,
                                     std::string &error) {
    error.clear();
    if (identifier.empty()) {
        error = "Mach-O code-signature identifier cannot be empty";
        return 0;
    }
    if (identifier.find('\0') != std::string_view::npos) {
        error = "Mach-O code-signature identifier cannot contain a null byte";
        return 0;
    }
    if (codeLimit > std::numeric_limits<std::uint32_t>::max()) {
        error = "Mach-O signed prefix does not fit in the CodeDirectory code-limit field";
        return 0;
    }
    if (identifier.size() > std::numeric_limits<std::uint32_t>::max() - kCodeDirectoryHeaderSize - 1U) {
        error = "Mach-O code-signature identifier is too long";
        return 0;
    }

    const std::uint64_t slotCount = (codeLimit + codeSignaturePageSize - 1U) / codeSignaturePageSize;
    const std::uint64_t allHeadersSize =
        AlignUp16(kBlobHeadersSize + kCodeDirectoryHeaderSize + identifier.size() + 1U);
    const std::uint64_t directorySize = allHeadersSize - kBlobHeadersSize + slotCount * Crypto::sha256DigestLength;
    const std::uint64_t totalSize = kBlobHeadersSize + directorySize;
    if (directorySize > std::numeric_limits<std::uint32_t>::max() ||
        totalSize > std::numeric_limits<std::uint32_t>::max()) {
        error = "Mach-O code signature does not fit in its 32-bit blob fields";
        return 0;
    }
    return totalSize;
}

bool BuildAdHocCodeSignature(const std::span<const std::uint8_t> signedPrefix, const std::string_view identifier,
                             const std::uint64_t executableSegmentBase, const std::uint64_t executableSegmentLimit,
                             const std::uint64_t executableSegmentFlags, std::vector<std::uint8_t> &signature,
                             std::string &error) {
    signature.clear();
    const std::uint64_t totalSize = AdHocCodeSignatureSize(signedPrefix.size(), identifier, error);
    if (totalSize == 0) {
        return false;
    }

    const std::uint32_t slotCount =
        static_cast<std::uint32_t>((signedPrefix.size() + codeSignaturePageSize - 1U) / codeSignaturePageSize);
    const std::uint32_t allHeadersSize =
        static_cast<std::uint32_t>(AlignUp16(kBlobHeadersSize + kCodeDirectoryHeaderSize + identifier.size() + 1U));
    const std::uint32_t hashOffset = allHeadersSize - kBlobHeadersSize;
    const std::uint32_t directorySize = hashOffset + slotCount * static_cast<std::uint32_t>(Crypto::sha256DigestLength);
    signature.reserve(static_cast<std::size_t>(totalSize));

    WriteBig32(signature, kEmbeddedSignatureMagic);
    WriteBig32(signature, static_cast<std::uint32_t>(totalSize));
    WriteBig32(signature, 1); // one CodeDirectory blob
    WriteBig32(signature, kCodeDirectorySlot);
    WriteBig32(signature, kBlobHeadersSize);
    while (signature.size() < kBlobHeadersSize) {
        signature.push_back(0);
    }

    WriteBig32(signature, kCodeDirectoryMagic);
    WriteBig32(signature, directorySize);
    WriteBig32(signature, kCodeDirectoryVersion);
    WriteBig32(signature, kAdHocLinkerSigned);
    WriteBig32(signature, hashOffset);
    WriteBig32(signature, kCodeDirectoryHeaderSize);
    WriteBig32(signature, 0); // special slots
    WriteBig32(signature, slotCount);
    WriteBig32(signature, static_cast<std::uint32_t>(signedPrefix.size()));
    signature.push_back(static_cast<std::uint8_t>(Crypto::sha256DigestLength));
    signature.push_back(kSha256Type);
    signature.push_back(0); // platform
    signature.push_back(kPageSizePower);
    WriteBig32(signature, 0); // spare2
    WriteBig32(signature, 0); // scatter offset
    WriteBig32(signature, 0); // team identifier offset
    WriteBig32(signature, 0); // spare3
    WriteBig64(signature, 0); // 64-bit code limit is unused for this version
    WriteBig64(signature, executableSegmentBase);
    WriteBig64(signature, executableSegmentLimit);
    WriteBig64(signature, executableSegmentFlags);

    signature.insert(signature.end(), identifier.begin(), identifier.end());
    signature.push_back(0);
    while (signature.size() < allHeadersSize) {
        signature.push_back(0);
    }

    for (std::size_t offset = 0; offset < signedPrefix.size(); offset += codeSignaturePageSize) {
        const std::size_t length = std::min<std::size_t>(codeSignaturePageSize, signedPrefix.size() - offset);
        const Crypto::Sha256Digest hash = Crypto::Sha256(signedPrefix.subspan(offset, length));
        signature.insert(signature.end(), hash.begin(), hash.end());
    }

    if (signature.size() != totalSize) {
        error = "internal: Mach-O code-signature size mismatch";
        signature.clear();
        return false;
    }
    return true;
}
} // namespace Rux::MachO
