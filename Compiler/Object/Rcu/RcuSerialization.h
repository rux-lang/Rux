#pragma once

#include "Object/Rcu/Rcu.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Rux {
/// Encode an RCU object to its on-disk bytes. Split from `RcuWriter` so the same encoding can be produced in memory,
/// which is what the unit tests round-trip against without touching the filesystem.
[[nodiscard]] std::vector<std::uint8_t> SerializeRcuFile(const RcuFile &file);

/// Decode the bytes `SerializeRcuFile` produced. Nullopt when they are not a well-formed RCU object of this version or
/// their checksum does not match, so a damaged or foreign file is never mistaken for an object.
[[nodiscard]] std::optional<RcuFile> DeserializeRcuFile(std::span<const std::uint8_t> bytes);

/// The CRC-32C an RCU object's header carries, over its bytes with the checksum field itself zero.
[[nodiscard]] std::uint32_t RcuChecksum(std::span<const std::uint8_t> bytes);

/// Render an RCU object as readable text, the form behind `RcuDumper`.
[[nodiscard]] std::string DumpRcuFileText(const RcuFile &file);
} // namespace Rux
