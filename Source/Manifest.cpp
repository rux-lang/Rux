/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#include "Rux/Manifest.h"

#include <algorithm>
#include <fstream>
#include <print>

namespace Rux {
    static std::string Trim(std::string_view s) {
        const auto ws = " \t\r\n";
        const auto start = s.find_first_not_of(ws);
        if (start == std::string_view::npos) return {};
        const auto end = s.find_last_not_of(ws);
        return std::string(s.substr(start, end - start + 1));
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
        const auto rawVal = Trim(inner.substr(eqPos + 1));
        if (rawVal.size() >= 2 && rawVal.front() == '"' && rawVal.back() == '"')
            return std::string(rawVal.substr(1, rawVal.size() - 2));
        return std::string(rawVal);
    }

    static std::string Unquote(std::string_view s) {
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return std::string(s.substr(1, s.size() - 2));
        return std::string(s);
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
        const auto at = spec.find('@');
        if (at == std::string_view::npos) return {std::string(spec), {}};
        return {std::string(spec.substr(0, at)), std::string(spec.substr(at + 1))};
    }

    std::optional<Manifest> Manifest::Load(const std::filesystem::path& path) {
        std::ifstream file(path);
        if (!file) return std::nullopt;
        Manifest m;
        std::string line;
        std::string section;
        while (std::getline(file, line)) {
            std::string trimmed = Trim(line);
            // Skip comments and blank lines
            if (trimmed.empty() || trimmed[0] == '#') continue;
            // Section header
            if (trimmed[0] == '[') {
                auto close = trimmed.find(']');
                if (close != std::string::npos) section = Trim(trimmed.substr(1, close - 1));
                continue;
            }
            // Key = value
            auto eq = trimmed.find('=');
            if (eq == std::string::npos) continue;
            std::string key = Trim(trimmed.substr(0, eq));
            std::string value = Unquote(Trim(trimmed.substr(eq + 1)));
            if (section == "Package") {
                if (key == "Name") m.package.name = value;
                if (key == "Version") m.package.version = value;
                if (key == "Type") m.package.type = value;
            }
            else if (section == "Build") {
                if (key == "Output") m.build.output = value;
            }
            else if (section == "Dependencies") {
                // Name = "version"  OR  Name = "*"  OR  Name = { Path = "..." }
                m.dependencies.push_back(ParseDependency(std::move(key), value));
            }
            else if (auto target = TargetNameFromDependenciesSection(section)) {
                m.targetDependencies[*target].push_back(ParseDependency(std::move(key), value));
            }
        }
        if (m.package.name.empty()) return std::nullopt;
        return m;
    }

    bool Manifest::Save(const std::filesystem::path& path) const {
        std::ofstream file(path);
        if (!file) return false;
        file << "[Package]\n";
        file << "Name    = \"" << package.name << "\"\n";
        file << "Version = \"" << package.version << "\"\n";
        file << "Type    = \"" << package.type << "\"\n";
        file << "\n[Build]\n";
        file << "Output = \"" << build.output << "\"\n";
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
                    std::string ver = dep.version.empty() ? "*" : dep.version;
                    file << dep.name << " = \"" << ver << "\"\n";
                }
            }
        }
        return file.good();
    }

    bool Manifest::AddDependency(const std::string& name, const std::string& version) {
        for (auto& dep : dependencies) {
            if (dep.name == name) {
                if (dep.version == version) return false; // already identical
                dep.version = version;
                return true;
            }
        }
        dependencies.push_back({name, {}, version, {}});
        return true;
    }

    bool Manifest::AddPathDependency(const std::string& name, const std::string& path) {
        for (auto& dep : dependencies) {
            if (dep.name == name) {
                if (dep.path == path) return false;
                dep.version = {};
                dep.path = path;
                return true;
            }
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
        auto it = std::ranges::find_if(dependencies, [&](const Dependency& d) { return d.name == name; });
        if (it == dependencies.end()) return false;
        dependencies.erase(it);
        return true;
    }

    std::optional<std::filesystem::path> Manifest::Find(const std::filesystem::path& start) {
        auto dir = std::filesystem::absolute(start);
        while (true) {
            if (auto candidate = dir / "Rux.toml"; std::filesystem::exists(candidate)) return candidate;
            auto parent = dir.parent_path();
            if (parent == dir) break; // filesystem root
            dir = parent;
        }
        return std::nullopt;
    }
} // namespace Rux
