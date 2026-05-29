/*
    Rux Compiler
    Copyright © 2026 Rux Contributors
    Licensed under the MIT License
*/

#include "Rux/Manifest.h"

#include <algorithm>
#include <fstream>
#include <ranges>

namespace Rux {
    static constexpr std::string_view whitespace = " \t\r\n";

    static constexpr std::string_view Trim(const std::string_view s) noexcept {
        const auto start = s.find_first_not_of(whitespace);
        return (start == std::string_view::npos) ? "" : s.substr(start, s.find_last_not_of(whitespace) - start + 1);
    }

    static constexpr std::string_view Unquote(std::string_view s) noexcept {
        s = Trim(s);
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
            return s.substr(1, s.size() - 2);
        }
        return s;
    }

    static std::string ParseInlineTablePath(std::string_view val) {
        const auto open = val.find('{');
        const auto close = val.rfind('}');

        if (open == std::string_view::npos || close <= open) return {};

        const std::string_view inner = Trim(val.substr(open + 1, close - open - 1));

        const auto keyPos = inner.find("Path");
        if (keyPos == std::string_view::npos ||
            (keyPos > 0 && std::isalnum(static_cast<unsigned char>(inner[keyPos - 1])))) {
            return {};
        }

        const auto eqPos = inner.find('=', keyPos);
        if (eqPos == std::string_view::npos) return {};

        return std::string(Unquote(Trim(inner.substr(eqPos + 1))));
    }

    std::pair<std::string, std::string> ParsePackageSpec(std::string_view spec) {
        if (const auto at = spec.find('@'); at != std::string_view::npos) {
            return {std::string(Trim(spec.substr(0, at))), std::string(Trim(spec.substr(at + 1)))};
        }
        return {std::string(Trim(spec)), {}};
    }

    std::optional<Manifest> Manifest::Load(const std::filesystem::path& path) {
        std::ifstream file(path);
        if (!file) return std::nullopt;

        Manifest m;
        std::string line;
        std::string section;

        while (std::getline(file, line)) {
            std::string_view trimmed = Trim(line);

            if (trimmed.empty() || trimmed.starts_with('#')) continue;

            if (trimmed.starts_with('[')) {
                if (const auto close = trimmed.find(']'); close != std::string_view::npos) {
                    section = Trim(trimmed.substr(1, close - 1));
                }
                continue;
            }

            const auto eq = trimmed.find('=');
            if (eq == std::string_view::npos) continue;

            const auto key = Trim(trimmed.substr(0, eq));
            const auto value = Unquote(Trim(trimmed.substr(eq + 1)));

            if (section == "Package") {
                if (key == "Name")
                    m.package.name = value;
                else if (key == "Version")
                    m.package.version = value;
                else if (key == "Type")
                    m.package.type = value;
            }
            else if (section == "Build") {
                if (key == "Output") m.build.output = value;
            }
            else if (section == "Dependencies") {
                auto& [name, version, path] = m.dependencies.emplace_back();
                name = key;

                if (std::string_view v = Trim(value); v.starts_with('{') && v.ends_with('}')) {
                    path = ParseInlineTablePath(v);
                }
                else {
                    version = (v == "*") ? "" : std::string(v);
                }
            }
        }

        if (m.package.name.empty()) return std::nullopt;
        return m;
    }

    bool Manifest::Save(const std::filesystem::path& path) const {
        std::ofstream file(path);
        if (!file) return false;

        file << "[Package]\n"
             << "Name    = \"" << package.name << "\"\n"
             << "Version = \"" << package.version << "\"\n"
             << "Type    = \"" << package.type << "\"\n"
             << "\n[Build]\n"
             << "Output  = \"" << build.output << "\"\n";

        if (!dependencies.empty()) {
            file << "\n[Dependencies]\n";
            for (const auto& [name, version, path] : dependencies) {
                if (!path.empty()) {
                    file << name << " = { Path = \"" << path << "\" }\n";
                }
                else {
                    file << name << " = \"" << (version.empty() ? "*" : version) << "\"\n";
                }
            }
        }

        return file.good();
    }

    bool Manifest::AddDependency(const std::string& name, const std::string& version) {
        if (const auto it = std::ranges::find(dependencies, name, &Dependency::name); it != dependencies.end()) {
            if (it->version == version) return false;
            it->version = version;
            it->path.clear();
            return true;
        }
        dependencies.push_back({name, version, {}});
        return true;
    }

    bool Manifest::AddPathDependency(const std::string& name, const std::string& path) {
        if (const auto it = std::ranges::find(dependencies, name, &Dependency::name); it != dependencies.end()) {
            if (it->path == path) return false;
            it->version.clear();
            it->path = path;
            return true;
        }
        dependencies.push_back({name, {}, path});
        return true;
    }

    bool Manifest::RemoveDependency(const std::string& name) {
        return std::erase_if(dependencies, [&](const Dependency& d) { return d.name == name; }) > 0;
    }

    std::optional<std::filesystem::path> Manifest::Find(const std::filesystem::path& start) {
        auto dir = std::filesystem::absolute(start);

        while (true) {
            if (auto candidate = dir / "Rux.toml"; std::filesystem::exists(candidate)) {
                return candidate;
            }
            auto parent = dir.parent_path();
            if (parent == dir) break;
            dir = std::move(parent);
        }

        return std::nullopt;
    }

} // namespace Rux
