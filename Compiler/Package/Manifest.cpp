#include "Package/Manifest.h"

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

// Parse a string value out of an inline table like { Path =
// "../../Packages/Std" }.
static std::string ParseInlineTableString(std::string_view val, std::string_view keyName) {
    const auto open = val.find('{');
    const auto close = val.rfind('}');
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open) {
        return {};
    }
    std::string_view inner = val.substr(open + 1, close - open - 1);
    const auto keyPos = inner.find(keyName);
    if (keyPos == std::string_view::npos) {
        return {};
    }
    const auto eqPos = inner.find('=', keyPos + keyName.size());
    if (eqPos == std::string_view::npos) {
        return {};
    }
    std::size_t valueEnd = eqPos + 1;
    bool inString = false;
    while (valueEnd < inner.size()) {
        const char c = inner[valueEnd];
        if (c == '"') {
            inString = !inString;
        }
        if (!inString && c == ',') {
            break;
        }
        ++valueEnd;
    }
    const auto rawVal = Trim(inner.substr(eqPos + 1, valueEnd - eqPos - 1));
    if (rawVal.size() >= 2 && rawVal.front() == '"' && rawVal.back() == '"') {
        return std::string(rawVal.substr(1, rawVal.size() - 2));
    }
    return std::string(rawVal);
}

static Dependency ParseDependency(std::string key, const std::string &value) {
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

// Split a bracketed, comma-separated list of quoted strings (which may span
// several lines, already joined into `text`) into its entries. Package paths
// contain no commas, so a plain comma split is sufficient.
static void ParseStringArray(std::string_view text, std::vector<std::string> &out) {
    const auto open = text.find('[');
    const auto close = text.rfind(']');
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open) {
        return;
    }
    std::string_view inner = text.substr(open + 1, close - open - 1);
    std::size_t pos = 0;
    while (pos <= inner.size()) {
        const auto comma = inner.find(',', pos);
        const auto end = comma == std::string_view::npos ? inner.size() : comma;
        const auto entry = Unquote(inner.substr(pos, end - pos));
        if (!entry.empty()) {
            out.emplace_back(entry);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
}

std::pair<std::string, std::string> ParsePackageSpec(std::string_view spec) {
    if (const auto at = spec.find('@'); at != std::string_view::npos) {
        return {std::string(Trim(spec.substr(0, at))), std::string(Trim(spec.substr(at + 1)))};
    }
    return {std::string(Trim(spec)), {}};
}

std::optional<Manifest> Manifest::Load(const std::filesystem::path &path) {
    std::ifstream file(path);
    if (!file) {
        return std::nullopt;
    }

    Manifest m;
    std::string line;
    std::string section;

    while (std::getline(file, line)) {
        std::string_view trimmed = Trim(line);

        if (trimmed.empty() || trimmed.starts_with('#')) {
            continue;
        }

        if (trimmed.starts_with('[')) {
            if (const auto close = trimmed.find(']'); close != std::string_view::npos) {
                section = Trim(trimmed.substr(1, close - 1));
            }
            continue;
        }

        const auto eq = trimmed.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }

        const auto key = Trim(trimmed.substr(0, eq));
        const auto value = Unquote(Trim(trimmed.substr(eq + 1)));

        if (section == "Package") {
            if (key == "Name") {
                m.package.name = value;
            }
            else if (key == "Version") {
                m.package.version = value;
            }
            else if (key == "Type") {
                m.package.type = value;
            }
            else if (key == "Description") {
                m.package.description = value;
            }
            else if (key == "Authors") {
                m.package.authors = value;
            }
            else if (key == "License") {
                m.package.license = value;
            }
            else if (key == "Repository") {
                m.package.repository = value;
            }
            else if (key == "Homepage") {
                m.package.homepage = value;
            }
        }
        else if (section == "Build") {
            if (key == "Output") {
                m.build.output = value;
            }
        }
        else if (section == "Build.Defines") {
            m.build.defines[std::string(key)] = value.empty() ? "true" : value;
        }
        else if (section == "Dependencies") {
            m.dependencies.push_back(ParseDependency(std::string(key), std::string(value)));
        }
        else if (section == "Workspace") {
            if (key == "Packages") {
                // The array may span several lines; keep reading until the
                // closing bracket is seen, then split the joined text.
                std::string arrayText(Trim(trimmed.substr(eq + 1)));
                while (arrayText.find(']') == std::string::npos && std::getline(file, line)) {
                    arrayText.push_back(' ');
                    arrayText.append(Trim(line));
                }
                ParseStringArray(arrayText, m.workspace.packages);
            }
        }
    }

    // A valid manifest is either a package (has a name) or a workspace
    // (lists member packages).
    if (m.package.name.empty() && m.workspace.packages.empty()) {
        return std::nullopt;
    }
    return m;
}

bool Manifest::Save(const std::filesystem::path &path) const {
    std::ofstream file(path);
    if (!file) {
        return false;
    }

    file << "[Package]\n"
         << "Name    = \"" << package.name << "\"\n"
         << "Version = \"" << package.version << "\"\n"
         << "Type    = \"" << package.type << "\"\n";
    if (!package.description.empty()) {
        file << "Description = \"" << package.description << "\"\n";
    }
    if (!package.authors.empty()) {
        file << "Authors = \"" << package.authors << "\"\n";
    }
    if (!package.license.empty()) {
        file << "License = \"" << package.license << "\"\n";
    }
    if (!package.repository.empty()) {
        file << "Repository = \"" << package.repository << "\"\n";
    }
    if (!package.homepage.empty()) {
        file << "Homepage = \"" << package.homepage << "\"\n";
    }

    file << "\n[Build]\n"
         << "Output  = \"" << build.output << "\"\n";

    if (!build.defines.empty()) {
        file << "\n[Build.Defines]\n";
        for (const auto &[name, value] : build.defines) {
            file << name << " = \"" << value << "\"\n";
        }
    }

    if (!dependencies.empty()) {
        file << "\n[Dependencies]\n";
        for (const auto &dep : dependencies) {
            const bool hasPackageAlias = !dep.package.empty() && dep.package != dep.name;
            if (!dep.path.empty() || hasPackageAlias) {
                file << dep.name << " = { ";
                bool wrote = false;
                if (hasPackageAlias) {
                    file << "Package = \"" << dep.package << "\"";
                    wrote = true;
                }
                if (!dep.path.empty()) {
                    if (wrote) {
                        file << ", ";
                    }
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

bool Manifest::AddDependency(const std::string &name, const std::string &version) {
    if (const auto it = std::ranges::find(dependencies, name, &Dependency::name); it != dependencies.end()) {
        if (it->version == version) {
            return false;
        }
        it->version = version;
        it->path.clear();
        return true;
    }
    dependencies.push_back({name, {}, version, {}});
    return true;
}

bool Manifest::AddPathDependency(const std::string &name, const std::string &path) {
    if (const auto it = std::ranges::find(dependencies, name, &Dependency::name); it != dependencies.end()) {
        if (it->path == path) {
            return false;
        }
        it->version.clear();
        it->path = path;
        return true;
    }
    dependencies.push_back({name, {}, {}, path});
    return true;
}

bool Manifest::RemoveDependency(const std::string &name) {
    return std::erase_if(dependencies, [&](const Dependency &d) { return d.name == name; }) > 0;
}

std::optional<std::filesystem::path> Manifest::Find(const std::filesystem::path &start) {
    auto dir = std::filesystem::absolute(start);

    while (true) {
        if (auto candidate = dir / "Rux.toml"; std::filesystem::exists(candidate)) {
            return candidate;
        }
        auto parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = std::move(parent);
    }

    return std::nullopt;
}
} // namespace Rux
