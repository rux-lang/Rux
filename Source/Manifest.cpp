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

    // Parse a string value out of an inline table like { Path = "../../Packages/Std" }.
    static std::string ParseInlineTableString(std::string_view val, std::string_view keyName) {
        const auto open = val.find('{');
        const auto close = val.rfind('}');
        if (open == std::string_view::npos || close == std::string_view::npos || close <= open) return {};
        std::string_view inner = val.substr(open + 1, close - open - 1);
        const auto keyPos = inner.find(keyName);
        if (keyPos == std::string_view::npos) return {};
        const auto eqPos = inner.find('=', keyPos + keyName.size());
        if (eqPos == std::string_view::npos) return {};
        std::string_view rawVal = inner.substr(eqPos + 1);
        // Stop at comma if present (for multi-field inline tables)
        const auto commaPos = rawVal.find(',');
        if (commaPos != std::string_view::npos)
            rawVal = rawVal.substr(0, commaPos);
        rawVal = Trim(rawVal);
        if (rawVal.size() >= 2 && rawVal.front() == '"' && rawVal.back() == '"')
            return std::string(rawVal.substr(1, rawVal.size() - 2));
        return std::string(rawVal);
    }

    static Dependency ParseDependency(std::string key, const std::string& value) {
        Dependency dep;
        dep.name = std::move(key);
        if (!value.empty() && value.front() == '{') {
            dep.package = ParseInlineTableString(value, "Package");
            dep.path = ParseInlineTableString(value, "Path");
            dep.version = ParseInlineTableString(value, "Version");
        }
        else {
            dep.version = value == "*" ? "" : value;
        }
        return dep;
    }

    static std::optional<std::string> TargetNameFromDependenciesSection(const std::string& section) {
        constexpr std::string_view prefix = "Target.";
        constexpr std::string_view suffix = ".Dependencies";
        if (!section.starts_with(prefix) || !section.ends_with(suffix)) return std::nullopt;
        const auto begin = prefix.size();
        const auto len = section.size() - prefix.size() - suffix.size();
        if (len == 0) return std::nullopt;
        return section.substr(begin, len);
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
                m.dependencies.push_back(ParseDependency(std::string(key), std::string(value)));
            }
            else if (const auto target = TargetNameFromDependenciesSection(section)) {
                m.targetDependencies[*target].push_back(ParseDependency(std::string(key), std::string(value)));
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
            for (const auto& dep : dependencies) {
                const bool hasPackageAlias = !dep.package.empty() && dep.package != dep.name;
                if (!dep.path.empty() || hasPackageAlias) {
                    file << dep.name << " = { ";
                    bool wrote = false;
                    if (hasPackageAlias) {
                        file << "Package = \"" << dep.package << "\"";
                        wrote = true;
                    }
                    if (!dep.path.empty()) {
                        if (wrote) file << ", ";
                        file << "Path = \"" << dep.path << "\"";
                    }
                    file << " }\n";
                }
                else {
                    std::string ver = dep.version.empty() ? "*" : dep.version;
                    file << dep.name << " = \"" << ver << "\"\n";
                }
            }
        }
        for (const auto& [target, deps] : targetDependencies) {
            if (deps.empty()) continue;
            file << "\n[Target." << target << ".Dependencies]\n";
            for (const auto& dep : deps) {
                const bool hasPackageAlias = !dep.package.empty() && dep.package != dep.name;
                if (!dep.path.empty() || hasPackageAlias) {
                    file << dep.name << " = { ";
                    bool wrote = false;
                    if (hasPackageAlias) {
                        file << "Package = \"" << dep.package << "\"";
                        wrote = true;
                    }
                    if (!dep.path.empty()) {
                        if (wrote) file << ", ";
                        file << "Path = \"" << dep.path << "\"";
                    }
                    file << " }\n";
                }
                else {
                    file << dep.name << " = \"" << (dep.version.empty() ? "*" : dep.version) << "\"\n";
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
        dependencies.push_back({name, {}, version, {}});
        return true;
    }

    bool Manifest::AddPathDependency(const std::string& name, const std::string& path) {
        if (const auto it = std::ranges::find(dependencies, name, &Dependency::name); it != dependencies.end()) {
            if (it->path == path) return false;
            it->version.clear();
            it->path = path;
            return true;
        }
        dependencies.push_back({name, {}, {}, path});
        return true;
    }

    std::vector<Dependency> Manifest::EffectiveDependencies(const std::string& target) const {
        std::vector<Dependency> result = dependencies;
        auto it = targetDependencies.find(target);
        if (it == targetDependencies.end()) return result;

        for (const auto& targetDep : it->second) {
            auto existing =
                std::ranges::find_if(result, [&](const Dependency& dep) { return dep.name == targetDep.name; });
            if (existing == result.end())
                result.push_back(targetDep);
            else
                *existing = targetDep;
        }
        return result;
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
