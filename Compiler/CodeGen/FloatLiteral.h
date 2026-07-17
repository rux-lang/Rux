#pragma once

#include <cstdlib>
#include <string>
#include <string_view>
#include <type_traits>

namespace Rux {
// The C conversions return the correctly rounded subnormal value even when
// they report underflow through errno. The throwing std::stof/std::stod
// wrappers discard that useful result by raising std::out_of_range.
template <typename T>
T ParseFloatLiteral(const std::string_view literal) {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);

    std::string normalized;
    normalized.reserve(literal.size());
    for (const char c : literal) {
        if (c != '_') {
            normalized.push_back(c);
        }
    }

    if constexpr (std::is_same_v<T, float>) {
        return std::strtof(normalized.c_str(), nullptr);
    }
    else {
        return std::strtod(normalized.c_str(), nullptr);
    }
}
} // namespace Rux
