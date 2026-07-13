#pragma once

#include <map>
#include <string>
#include <string_view>

namespace Rux::CliSupport {

inline bool AddCompileTimeDefine(const std::string_view spec, std::map<std::string, std::string> &defines,
                                 std::string &error) {
    const auto equals = spec.find('=');
    const std::string_view name = spec.substr(0, equals);
    if (name.empty()) {
        error = "--define expects NAME or NAME=VALUE";
        return false;
    }
    const std::string_view value =
        equals == std::string_view::npos ? std::string_view("true") : spec.substr(equals + 1);
    defines[std::string(name)] = std::string(value);
    return true;
}

} // namespace Rux::CliSupport
