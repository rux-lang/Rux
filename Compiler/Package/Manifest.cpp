#include "Package/Manifest.h"

#include "Package/ManifestSyntax.h"
#include "Package/ManifestValidation.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <ostream>
#include <ranges>
#include <sstream>

namespace Rux {
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
        return "MacOS";
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

namespace {
// ---- Serialization ----------------------------------------------------------

std::string Escape(const std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    for (const char c : value) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04X}", static_cast<unsigned>(c));
            }
            else {
                out.push_back(c);
            }
        }
    }
    return out;
}

std::string Quoted(const std::string_view value) {
    return std::format("\"{}\"", Escape(value));
}

void WriteStringArray(std::ostream &out, const std::string_view field, const std::vector<std::string> &values) {
    out << field << " = [";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << Quoted(values[i]);
    }
    out << "]\n";
}
} // namespace

std::string Manifest::Serialize() const {
    std::ostringstream out;
    out << "[Manifest]\n";
    out << "Version = " << header.schemaVersion << '\n';
    if (header.minRux) {
        out << "MinRux = " << Quoted(header.minRux->Text()) << '\n';
    }

    if (IsWorkspace()) {
        out << "\n[Workspace]\nPackages = [\n";
        for (const auto &member : workspace.packages) {
            out << "    " << Quoted(member) << ",\n";
        }
        out << "]\n";
        return out.str();
    }

    out << "\n[Package]\n";
    if (package.ns) {
        out << "Namespace = " << Quoted(package.ns->Text()) << '\n';
    }
    out << "Name = " << Quoted(package.name.Text()) << '\n';
    out << "Version = " << Quoted(package.version.Text()) << '\n';
    out << "Type = " << Quoted(ToString(package.type)) << '\n';
    if (!package.description.empty()) {
        out << "Description = " << Quoted(package.description) << '\n';
    }
    if (!package.authors.empty()) {
        WriteStringArray(out, "Authors", package.authors);
    }
    if (!package.keywords.empty()) {
        std::vector<std::string> spellings;
        spellings.reserve(package.keywords.size());
        for (const auto &keyword : package.keywords) {
            spellings.push_back(keyword.Text());
        }
        WriteStringArray(out, "Keywords", spellings);
    }
    if (!package.license.empty()) {
        out << "License = " << Quoted(package.license) << '\n';
    }
    if (!package.licenseFile.empty()) {
        out << "LicenseFile = " << Quoted(package.licenseFile) << '\n';
    }
    if (!package.repository.empty()) {
        out << "Repository = " << Quoted(package.repository) << '\n';
    }
    if (!package.homepage.empty()) {
        out << "Homepage = " << Quoted(package.homepage) << '\n';
    }
    if (!package.readmeFile.empty()) {
        out << "ReadmeFile = " << Quoted(package.readmeFile) << '\n';
    }

    if (!dependencies.empty()) {
        // Deterministic regardless of the order entries were added or read.
        std::vector<const ManifestDependency *> ordered;
        ordered.reserve(dependencies.size());
        for (const auto &dependency : dependencies) {
            ordered.push_back(&dependency);
        }
        std::ranges::sort(ordered, {}, [](const ManifestDependency *d) { return d->importName.Normalized(); });

        out << "\n[Dependencies]\n";
        for (const auto *dependency : ordered) {
            out << dependency->importName.Text() << " = { ";
            const bool aliased = dependency->package != dependency->importName;
            if (const auto *registry = dependency->Registry()) {
                out << "Namespace = " << Quoted(registry->ns.Text());
                if (aliased) {
                    out << ", Package = " << Quoted(dependency->package.Text());
                }
                out << ", Version = " << Quoted(registry->version.Text());
            }
            else {
                if (aliased) {
                    out << "Package = " << Quoted(dependency->package.Text()) << ", ";
                }
                out << "Path = " << Quoted(dependency->Path());
            }
            if (!dependency->targetOS.empty()) {
                out << ", TargetOS = [";
                for (std::size_t i = 0; i < dependency->targetOS.size(); ++i) {
                    if (i != 0) {
                        out << ", ";
                    }
                    out << Quoted(ManifestTargetOSName(dependency->targetOS[i]));
                }
                out << ']';
            }
            out << " }\n";
        }
    }

    // `[Build]` carries nothing but a default when the output is the default,
    // so canonical form leaves it out rather than restating it.
    if (build.output != "Bin") {
        out << "\n[Build]\n";
        out << "Output = " << Quoted(build.output) << '\n';
    }

    if (!build.defines.empty()) {
        out << "\n[Build.Defines]\n";
        for (const auto &[name, value] : build.defines) {
            out << name << " = ";
            switch (value.kind) {
            case DefineValue::Kind::String:
                out << Quoted(value.text);
                break;
            case DefineValue::Kind::Boolean:
            case DefineValue::Kind::Integer:
                out << value.text;
                break;
            }
            out << '\n';
        }
    }
    return out.str();
}

ManifestResult Manifest::Parse(const std::string_view text, const std::filesystem::path &path) {
    ManifestResult result;
    if (text.size() > manifestMaxBytes) {
        result.diagnostics.push_back({path, 1, 1, std::format("manifest is larger than {} bytes", manifestMaxBytes)});
        return result;
    }

    auto syntax = ManifestDetail::ParseManifestSyntax(text);
    if (!syntax) {
        result.diagnostics.push_back(
            {path, syntax.error().location.line, syntax.error().location.column, syntax.error().message});
        return result;
    }

    auto validation = ManifestDetail::ValidateManifestV1(std::move(*syntax));
    if (!validation) {
        result.diagnostics.push_back(
            {path, validation.error().location.line, validation.error().location.column, validation.error().message});
        return result;
    }
    result.manifest = std::move(*validation);
    return result;
}

ManifestResult Manifest::Load(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ManifestResult result;
        result.diagnostics.push_back({path, 1, 1, "could not open the manifest"});
        return result;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return Parse(contents.str(), path);
}

bool Manifest::Save(const std::filesystem::path &path) const {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    const std::string text = Serialize();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return file.good();
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

std::vector<std::string> ValidateForPublication(const Manifest &manifest) {
    std::vector<std::string> rejections;

    if (manifest.IsWorkspace()) {
        rejections.emplace_back("a workspace cannot be published; publish a member package instead");
        return rejections;
    }

    if (manifest.package.type != ManifestPackageType::SourceLibrary) {
        rejections.emplace_back(std::format("[Package].Type = \"{}\" cannot be published by Rux 0.4.0; this release "
                                            "publishes only Type = \"SourceLibrary\"",
                                            ToString(manifest.package.type)));
    }

    if (!manifest.package.ns) {
        rejections.emplace_back("publication requires [Package].Namespace; a namespace-free package is local-only");
    }

    if (!manifest.header.minRux) {
        rejections.emplace_back(
            std::format("publication requires [Manifest].MinRux, the oldest compiler release that can build the "
                        "package; it must be at least {}",
                        publicationMinRuxFloor));
    }
    else if (const auto floor = SemanticVersion::Parse(publicationMinRuxFloor);
             floor && SemanticVersion::ComparePrecedence(*manifest.header.minRux, *floor) < 0) {
        rejections.emplace_back(std::format("[Manifest].MinRux is '{}' but publication requires at least {}",
                                            manifest.header.minRux->Text(), publicationMinRuxFloor));
    }

    // A path dependency names a directory that exists only in the publishing
    // tree, so a consumer resolving from the registry could never satisfy it.
    for (const auto &dependency : manifest.dependencies) {
        if (dependency.IsPath()) {
            rejections.emplace_back(
                std::format("dependency '{}' uses Path = \"{}\"; publication requires registry dependencies",
                            dependency.importName.Text(), dependency.Path()));
        }
    }

    return rejections;
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

std::expected<PackageSpec, std::string> ParsePackageSpec(const std::string_view spec) {
    std::string_view rest = spec;
    PackageSpec parsed;

    if (const auto at = rest.find('@'); at != std::string_view::npos) {
        const std::string_view requirement = rest.substr(at + 1);
        if (requirement.empty()) {
            return std::unexpected("missing version requirement after '@'");
        }
        auto range = VersionRange::Parse(requirement);
        if (!range) {
            return std::unexpected(
                std::format("invalid version requirement '{}': {}", requirement, Describe(range.error())));
        }
        parsed.version = std::move(*range);
        rest = rest.substr(0, at);
    }

    std::string_view nameText = rest;
    if (const auto slash = rest.find('/'); slash != std::string_view::npos) {
        auto ns = IdentitySegment::Parse(rest.substr(0, slash));
        if (!ns) {
            return std::unexpected(
                std::format("invalid namespace '{}': {}", rest.substr(0, slash), Describe(ns.error())));
        }
        parsed.ns = std::move(*ns);
        nameText = rest.substr(slash + 1);
    }

    auto name = IdentitySegment::Parse(nameText);
    if (!name) {
        return std::unexpected(std::format("invalid package name '{}': {}", nameText, Describe(name.error())));
    }
    parsed.name = std::move(*name);
    return parsed;
}
} // namespace Rux
