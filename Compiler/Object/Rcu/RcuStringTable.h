#pragma once

// Interned, NUL-separated string table for RCU object files.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rux {
/// Interns the strings an RCU object refers to, so a name repeated across symbols, sections, and metadata is stored
/// once and referenced by offset.
///
/// Offset 0 is always the empty string. That is why the table starts with a NUL: it lets a zero offset mean "no name"
/// without needing a separate flag anywhere in the format.
class RcuStringTable {
public:
    RcuStringTable() {
        data.push_back('\0');
    }

    /// Add `s` if new, and return its byte offset. The empty string is always offset 0.
    uint32_t Intern(const std::string &s) {
        if (s.empty()) {
            return 0;
        }
        auto it = map.find(s);
        if (it != map.end()) {
            return it->second;
        }
        const auto off = static_cast<uint32_t>(data.size());
        map[s] = off;
        data.insert(data.end(), s.begin(), s.end());
        data.push_back('\0');
        return off;
    }

    /// Total encoded size in bytes, which is what the object header records.
    [[nodiscard]] uint32_t Size() const {
        return static_cast<uint32_t>(data.size());
    }

    /// The raw NUL-separated block, written to the object verbatim.
    [[nodiscard]] const char *Data() const {
        return data.data();
    }

    /// The string at `off`, or empty when the offset is past the end.
    [[nodiscard]] std::string Get(const uint32_t off) const {
        if (off >= data.size()) {
            return {};
        }
        return {data.data() + off};
    }

private:
    std::vector<char> data;
    std::unordered_map<std::string, uint32_t> map;
};
} // namespace Rux
