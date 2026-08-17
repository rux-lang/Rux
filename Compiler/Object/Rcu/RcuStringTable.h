#pragma once

// Interned, NUL-separated string table for RCU object files.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rux {
class RcuStringTable {
public:
    RcuStringTable() {
        data.push_back('\0');
    }

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

    [[nodiscard]] uint32_t Size() const {
        return static_cast<uint32_t>(data.size());
    }

    [[nodiscard]] const char *Data() const {
        return data.data();
    }

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
