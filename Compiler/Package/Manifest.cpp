#include "Package/Manifest.h"

#include "Package/ManifestSyntax.h"
#include "Package/ManifestValidation.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <ranges>
#include <sstream>

namespace Rux {
namespace {
std::optional<ManifestDetail::Location> InvalidUtf8Location(const std::string_view text) {
    ManifestDetail::Location location;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto lead = static_cast<unsigned char>(text[offset]);
        std::size_t width = 1;
        std::uint32_t codePoint = lead;
        std::uint32_t minimum = 0;
        if (lead < 0x80) {
            if (lead == '\n') {
                ++location.line;
                location.column = 1;
            }
            else {
                ++location.column;
            }
            ++offset;
            continue;
        }
        if ((lead & 0xE0U) == 0xC0U) {
            width = 2;
            codePoint = lead & 0x1FU;
            minimum = 0x80;
        }
        else if ((lead & 0xF0U) == 0xE0U) {
            width = 3;
            codePoint = lead & 0x0FU;
            minimum = 0x800;
        }
        else if ((lead & 0xF8U) == 0xF0U) {
            width = 4;
            codePoint = lead & 0x07U;
            minimum = 0x10000;
        }
        else {
            return location;
        }
        if (offset + width > text.size()) {
            return location;
        }
        for (std::size_t index = 1; index < width; ++index) {
            const auto continuation = static_cast<unsigned char>(text[offset + index]);
            if ((continuation & 0xC0U) != 0x80U) {
                return location;
            }
            codePoint = (codePoint << 6U) | (continuation & 0x3FU);
        }
        if (codePoint < minimum || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
            return location;
        }
        offset += width;
        location.column += static_cast<std::uint32_t>(width);
    }
    return std::nullopt;
}

std::optional<std::string> SourceLineAt(const std::string_view text, const std::uint32_t wantedLine) {
    std::uint32_t line = 1;
    std::size_t begin = 0;
    while (line < wantedLine) {
        const auto newline = text.find('\n', begin);
        if (newline == std::string_view::npos) {
            return std::nullopt;
        }
        begin = newline + 1;
        ++line;
    }
    auto end = text.find('\n', begin);
    if (end == std::string_view::npos) {
        end = text.size();
    }
    if (end > begin && text[end - 1] == '\r') {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}
} // namespace

std::string_view ToString(const ManifestPackageType type) noexcept {
    switch (type) {
    case ManifestPackageType::Executable:
        return "Executable";
    case ManifestPackageType::SharedLibrary:
        return "SharedLibrary";
    case ManifestPackageType::StaticLibrary:
        return "StaticLibrary";
    case ManifestPackageType::SourceLibrary:
        return "SourceLibrary";
    }
    return "Executable";
}

std::optional<ManifestPackageType> ParseManifestPackageType(const std::string_view value) noexcept {
    if (value == "Executable") {
        return ManifestPackageType::Executable;
    }
    if (value == "SharedLibrary") {
        return ManifestPackageType::SharedLibrary;
    }
    if (value == "StaticLibrary") {
        return ManifestPackageType::StaticLibrary;
    }
    if (value == "SourceLibrary") {
        return ManifestPackageType::SourceLibrary;
    }
    return std::nullopt;
}

std::string_view ManifestTargetOSName(const Target::OS os) noexcept {
    switch (os) {
    case Target::OS::FreeBSD:
        return "FreeBSD";
    case Target::OS::Linux:
        return "Linux";
    case Target::OS::MacOS:
        return "macOS";
    case Target::OS::Windows:
        return "Windows";
    default:
        return "";
    }
}

std::optional<Target::OS> ParseManifestTargetOS(const std::string_view value) noexcept {
    constexpr Target::OS supported[] = {Target::OS::FreeBSD, Target::OS::Linux, Target::OS::MacOS, Target::OS::Windows};
    for (const Target::OS os : supported) {
        if (ManifestTargetOSName(os) == value) {
            return os;
        }
    }
    return std::nullopt;
}

std::string ManifestDiagnostic::Format() const {
    return std::format("{}:{}:{}: {}", path.string(), line, column, message);
}

std::string ManifestDiagnostic::Render() const {
    std::string rendered = std::format("{}:{}:{}: error: {}\n", path.string(), line, column, message);
    if (sourceLine) {
        const std::size_t caret = std::min<std::size_t>(column > 0 ? column - 1 : 0, sourceLine->size());
        rendered += std::format("  {} | {}\n", line, *sourceLine);
        rendered += std::string(std::to_string(line).size() + 5 + caret, ' ') + "^\n";
    }
    for (const auto &note : notes) {
        rendered += "  note: " + note + '\n';
    }
    if (help) {
        rendered += "  help: " + *help + '\n';
    }
    if (documentationUrl) {
        rendered += "  docs: " + *documentationUrl + '\n';
    }
    return rendered;
}

const std::string &ManifestDependency::Path() const noexcept {
    static const std::string none;
    if (const auto *local = std::get_if<PathDependencySource>(&source)) {
        return local->path;
    }
    return none;
}

bool ManifestDependency::MatchesTarget(const Target::OS os) const noexcept {
    return targetOS.empty() || std::ranges::contains(targetOS, os);
}

std::map<std::string, std::string> Build::ConfigValues() const {
    std::map<std::string, std::string> values;
    for (const auto &[name, value] : defines) {
        values.emplace(name, value.text);
    }
    return values;
}

ManifestResult Manifest::Parse(const std::string_view text, const std::filesystem::path &path) {
    ManifestResult result;
    if (text.size() > manifestMaxBytes) {
        result.diagnostics.push_back({path,
                                      1,
                                      1,
                                      std::format("manifest is larger than {} bytes", manifestMaxBytes),
                                      {},
                                      "reduce the manifest size",
                                      std::string(manifestDocumentationUrl),
                                      {}});
        return result;
    }

    if (const auto invalid = InvalidUtf8Location(text)) {
        result.diagnostics.push_back({path,
                                      invalid->line,
                                      invalid->column,
                                      "manifest contains invalid UTF-8",
                                      {},
                                      "save the manifest as valid UTF-8",
                                      std::string(manifestDocumentationUrl),
                                      SourceLineAt(text, invalid->line)});
        return result;
    }

    auto syntax = ManifestDetail::ParseManifestSyntax(text);
    if (!syntax) {
        result.diagnostics.push_back({path,
                                      syntax.error().location.line,
                                      syntax.error().location.column,
                                      syntax.error().message,
                                      {},
                                      "use the supported TOML syntax described in the manifest reference",
                                      std::string(manifestDocumentationUrl),
                                      SourceLineAt(text, syntax.error().location.line)});
        return result;
    }

    auto validation = ManifestDetail::ValidateManifestV1All(std::move(*syntax));
    if (!validation.Ok()) {
        for (auto &diagnostic : validation.diagnostics) {
            result.diagnostics.push_back({path,
                                          diagnostic.location.line,
                                          diagnostic.location.column,
                                          std::move(diagnostic.message),
                                          {},
                                          std::move(diagnostic.help),
                                          std::move(diagnostic.documentationUrl),
                                          SourceLineAt(text, diagnostic.location.line)});
        }
        return result;
    }
    result.manifest = std::move(*validation.manifest);
    return result;
}

ManifestResult Manifest::Load(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ManifestResult result;
        result.diagnostics.push_back(
            {path, 1, 1, "could not open the manifest", {}, "check that the path exists and is readable", {}, {}});
        return result;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return Parse(contents.str(), path);
}

const ManifestDependency *Manifest::FindDependency(const IdentitySegment &importName) const {
    const auto found = std::ranges::find(dependencies, importName, &ManifestDependency::importName);
    return found == dependencies.end() ? nullptr : &*found;
}

bool Manifest::AddRegistryDependency(IdentitySegment importName, IdentitySegment ns, VersionRange version) {
    RegistryDependencySource source{std::move(ns), std::move(version)};
    const auto found = std::ranges::find(dependencies, importName, &ManifestDependency::importName);
    if (found != dependencies.end()) {
        const auto *existing = found->Registry();
        if (existing != nullptr && existing->ns == source.ns && existing->version.Text() == source.version.Text() &&
            found->package == importName) {
            return false;
        }
        found->package = importName;
        found->source = std::move(source);
        return true;
    }
    dependencies.push_back({importName, importName, std::move(source), {}});
    return true;
}

bool Manifest::AddPathDependency(IdentitySegment importName, std::string path) {
    const auto found = std::ranges::find(dependencies, importName, &ManifestDependency::importName);
    if (found != dependencies.end()) {
        if (found->IsPath() && found->Path() == path && found->package == importName) {
            return false;
        }
        found->package = importName;
        found->source = PathDependencySource{std::move(path)};
        return true;
    }
    dependencies.push_back({importName, importName, PathDependencySource{std::move(path)}, {}});
    return true;
}

bool Manifest::RemoveDependency(const IdentitySegment &importName) {
    return std::erase_if(dependencies,
                         [&](const ManifestDependency &dependency) { return dependency.importName == importName; }) > 0;
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

bool IsIntrinsicsPackage(const Manifest &manifest) {
    return !manifest.IsWorkspace() && manifest.package.ns &&
           manifest.package.ns->Normalized() == NormalizeIdentity(intrinsicsPackageNamespace) &&
           manifest.package.name.Normalized() == NormalizeIdentity(intrinsicsPackageName);
}

std::vector<std::filesystem::path> DiscoverManifestlessWorkspaceManifests(const std::filesystem::path &root) {
    std::vector<std::filesystem::path> manifests;

    // Match `rux test`: directories without a manifest are grouping
    // directories, searched only a few levels deep so output trees and other
    // unrelated directory hierarchies are not walked indefinitely.
    auto DiscoverTests = [&](const std::filesystem::path &testsRoot) {
        std::error_code ec;
        if (!std::filesystem::exists(testsRoot, ec)) {
            return;
        }
        constexpr int maxGroupDepth = 3;
        std::vector<std::pair<std::filesystem::path, int>> pending;
        pending.emplace_back(testsRoot, 0);
        while (!pending.empty()) {
            const auto [dir, depth] = std::move(pending.back());
            pending.pop_back();
            for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
                if (!entry.is_directory()) {
                    continue;
                }
                const auto manifestPath = entry.path() / "Rux.toml";
                if (std::filesystem::exists(manifestPath, ec)) {
                    manifests.push_back(manifestPath.lexically_normal());
                }
                else if (depth + 1 < maxGroupDepth) {
                    pending.emplace_back(entry.path(), depth + 1);
                }
            }
        }
    };

    const auto workspaceRoot = std::filesystem::absolute(root).lexically_normal();
    DiscoverTests(workspaceRoot / "Tests");

    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(workspaceRoot, ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        const auto memberManifest = entry.path() / "Rux.toml";
        if (!std::filesystem::exists(memberManifest, ec)) {
            continue;
        }
        manifests.push_back(memberManifest.lexically_normal());
        DiscoverTests(entry.path() / "Tests");
    }

    std::ranges::sort(manifests);
    const auto duplicates = std::ranges::unique(manifests);
    manifests.erase(duplicates.begin(), duplicates.end());
    return manifests;
}

std::expected<PackageSpec, PackageProblem> ParsePackageSpec(const std::string_view spec) {
    std::string_view rest = spec;
    PackageSpec parsed;

    const auto identityProblem = [](const std::string_view role, const std::string_view value,
                                    const IdentityError &error) {
        return PackageProblem{DescribeIdentity(role, value, error), {std::string(identitySegmentConstraint)}, {}};
    };

    if (const auto at = rest.find('@'); at != std::string_view::npos) {
        if (rest.find('@', at + 1) != std::string_view::npos) {
            return std::unexpected(
                PackageProblem{std::format("package specification '{}' contains more than one '@' separator", spec),
                               {"a package specification has at most one version requirement"},
                               {}});
        }
        const std::string_view requirement = rest.substr(at + 1);
        if (requirement.empty()) {
            return std::unexpected(
                PackageProblem{std::format("package specification '{}' has no requirement after '@'", spec),
                               {"version requirements use Semantic Versioning ranges"},
                               {}});
        }
        auto range = VersionRange::Parse(requirement);
        if (!range) {
            return std::unexpected(PackageProblem{DescribeVersion("version requirement", requirement, range.error()),
                                                  {"requirements use forms such as '^1.2.0' or '>=1.0.0, <2.0.0'"},
                                                  {}});
        }
        parsed.version = std::move(*range);
        rest = rest.substr(0, at);
    }

    std::string_view nameText = rest;
    if (const auto slash = rest.find('/'); slash != std::string_view::npos) {
        if (rest.find('/', slash + 1) != std::string_view::npos) {
            return std::unexpected(
                PackageProblem{std::format("package specification '{}' contains more than one '/' separator", spec),
                               {"registry identities use exactly 'namespace/package'"},
                               {}});
        }
        auto ns = IdentitySegment::Parse(rest.substr(0, slash));
        if (!ns) {
            return std::unexpected(identityProblem("namespace", rest.substr(0, slash), ns.error()));
        }
        parsed.ns = std::move(*ns);
        nameText = rest.substr(slash + 1);
    }

    auto name = IdentitySegment::Parse(nameText);
    if (!name) {
        return std::unexpected(identityProblem("package name", nameText, name.error()));
    }
    parsed.name = std::move(*name);
    return parsed;
}
} // namespace Rux
