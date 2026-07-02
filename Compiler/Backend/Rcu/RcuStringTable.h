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
        data_.push_back('\0');
    }

    uint32_t Intern(const std::string &s) {
        if (s.empty()) {
            return 0;
        }
        auto it = map_.find(s);
        if (it != map_.end()) {
            return it->second;
        }
        const auto off = static_cast<uint32_t>(data_.size());
        map_[s] = off;
        data_.insert(data_.end(), s.begin(), s.end());
        data_.push_back('\0');
        return off;
    }

    [[nodiscard]] uint32_t Size() const {
        return static_cast<uint32_t>(data_.size());
    }

    [[nodiscard]] const char *Data() const {
        return data_.data();
    }

    [[nodiscard]] std::string Get(const uint32_t off) const {
        if (off >= data_.size()) {
            return {};
        }
        return {data_.data() + off};
    }

private:
    std::vector<char> data_;
    std::unordered_map<std::string, uint32_t> map_;
};

} // namespace Rux
