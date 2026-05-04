/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#include "Rux/Manifest.h"

#include <fstream>
#include <sstream>
#include <print>
#include <algorithm>

namespace Rux
{
    static std::string Trim(std::string_view s)
    {
        const char* ws = " \t\r\n";
        auto start = s.find_first_not_of(ws);
        if (start == std::string_view::npos) return {};
        auto end = s.find_last_not_of(ws);
        return std::string(s.substr(start, end - start + 1));
    }

    static std::string Unquote(std::string_view s)
    {
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            return std::string(s.substr(1, s.size() - 2));
        return std::string(s);
    }

    std::pair<std::string, std::string> ParsePackageSpec(std::string_view spec)
    {
        auto at = spec.find('@');
        if (at == std::string_view::npos)
            return {std::string(spec), {}};
        return {std::string(spec.substr(0, at)), std::string(spec.substr(at + 1))};
    }

    std::optional<Manifest> Manifest::Load(const std::filesystem::path& path)
    {
        std::ifstream file(path);
        if (!file) return std::nullopt;
        Manifest m;
        std::string line;
        std::string section;
        while (std::getline(file, line))
        {
            std::string trimmed = Trim(line);
            // Skip comments and blank lines
            if (trimmed.empty() || trimmed[0] == '#') continue;
            // Section header
            if (trimmed[0] == '[')
            {
                auto close = trimmed.find(']');
                if (close != std::string::npos)
                    section = Trim(trimmed.substr(1, close - 1));
                continue;
            }
            // Key = value
            auto eq = trimmed.find('=');
            if (eq == std::string::npos) continue;
            std::string key = Trim(trimmed.substr(0, eq));
            std::string value = Unquote(Trim(trimmed.substr(eq + 1)));
            if (section == "Package")
            {
                if (key == "Name") m.package.name = value;
                if (key == "Version") m.package.version = value;
                if (key == "Type") m.package.type = value;
            }
            else if (section == "Build")
            {
                if (key == "Output") m.build.output = value;
            }
            else if (section == "Dependencies")
            {
                // Each line: Name = "version"  OR  Name = "*"
                m.dependencies.push_back({key, value == "*" ? "" : value});
            }
        }
        if (m.package.name.empty()) return std::nullopt;
        return m;
    }

    bool Manifest::Save(const std::filesystem::path& path) const
    {
        std::ofstream file(path);
        if (!file) return false;
        file << "[Package]\n";
        file << "Name    = \"" << package.name << "\"\n";
        file << "Version = \"" << package.version << "\"\n";
        file << "Type    = \"" << package.type << "\"\n";
        file << "\n[Build]\n";
        file << "Output = \"" << build.output << "\"\n";
        if (!dependencies.empty())
        {
            file << "\n[Dependencies]\n";
            for (const auto& dep : dependencies)
            {
                std::string ver = dep.version.empty() ? "*" : dep.version;
                file << dep.name << " = \"" << ver << "\"\n";
            }
        }
        return file.good();
    }

    bool Manifest::AddDependency(const std::string& name, const std::string& version)
    {
        for (auto& dep : dependencies)
        {
            if (dep.name == name)
            {
                if (dep.version == version) return false; // already identical
                dep.version = version;
                return true;
            }
        }
        dependencies.push_back({name, version});
        return true;
    }

    bool Manifest::RemoveDependency(const std::string& name)
    {
        auto it = std::ranges::find_if(dependencies,
                                       [&](const Dependency& d) { return d.name == name; });
        if (it == dependencies.end()) return false;
        dependencies.erase(it);
        return true;
    }

    std::optional<std::filesystem::path> Manifest::Find(const std::filesystem::path& start)
    {
        auto dir = std::filesystem::absolute(start);
        while (true)
        {
            auto candidate = dir / "Rux.toml";
            if (std::filesystem::exists(candidate))
                return candidate;
            auto parent = dir.parent_path();
            if (parent == dir) break; // filesystem root
            dir = parent;
        }
        return std::nullopt;
    }
}
