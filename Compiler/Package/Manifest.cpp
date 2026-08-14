#include "Package/Manifest.h"

#include "Package/ManifestSyntax.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <ostream>
#include <ranges>
#include <span>
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
using ManifestDetail::KeyValue;
using ManifestDetail::Location;
using ManifestDetail::Table;
using ManifestDetail::Value;

struct ValidationFailure {
    Location location;
    std::string message;
};

// ---- Validation -------------------------------------------------------------

class Validator {
public:
    explicit Validator(std::vector<Table> input)
        : tables(std::move(input)) {
    }

    Manifest Run() {
        Manifest manifest;
        for (const auto &table : tables) {
            static constexpr std::string_view known[] = {"Manifest",     "Package", "Workspace",
                                                         "Dependencies", "Build",   "Build.Defines"};
            if (std::ranges::find(known, table.name) == std::ranges::end(known)) {
                FailAt(table.location, std::format("unknown section '[{}]'", table.name));
            }
            for (const auto &other : tables) {
                if (&other != &table && other.name == table.name && Before(other.location, table.location)) {
                    FailAt(table.location, std::format("duplicate section '[{}]'", table.name));
                }
            }
        }

        const Table *header = Find("Manifest");
        if (header == nullptr) {
            FailAt({1, 1}, "manifest must start with a '[Manifest]' section declaring 'Version = 1'");
        }
        ReadHeader(*header, manifest.header);

        const Table *packageTable = Find("Package");
        const Table *workspaceTable = Find("Workspace");
        if (packageTable != nullptr && workspaceTable != nullptr) {
            FailAt(workspaceTable->location, "'[Package]' and '[Workspace]' are mutually exclusive");
        }
        if (packageTable == nullptr && workspaceTable == nullptr) {
            FailAt(header->location, "manifest must declare either '[Package]' or '[Workspace]'");
        }

        if (workspaceTable != nullptr) {
            ReadWorkspace(*workspaceTable, manifest.workspace);
            if (const Table *deps = Find("Dependencies")) {
                FailAt(deps->location, "a workspace cannot declare dependencies");
            }
            if (const Table *build = Find("Build")) {
                FailAt(build->location, "a workspace cannot declare build settings");
            }
            if (const Table *defines = Find("Build.Defines")) {
                FailAt(defines->location, "a workspace cannot declare build settings");
            }
            return manifest;
        }

        ReadPackage(*packageTable, manifest.package);
        if (const Table *deps = Find("Dependencies")) {
            ReadDependencies(*deps, manifest.dependencies);
        }
        if (const Table *build = Find("Build")) {
            ReadBuild(*build, manifest.build);
        }
        if (const Table *defines = Find("Build.Defines")) {
            ReadDefines(*defines, manifest.build.defines);
        }
        return manifest;
    }

private:
    std::vector<Table> tables;

    static bool Before(const Location left, const Location right) noexcept {
        return left.line < right.line || (left.line == right.line && left.column < right.column);
    }

    [[noreturn]] static void FailAt(const Location location, std::string message) {
        throw ValidationFailure{location, std::move(message)};
    }

    [[nodiscard]] const Table *Find(const std::string_view name) const {
        const auto found = std::ranges::find(tables, name, &Table::name);
        return found == tables.end() ? nullptr : &*found;
    }

    static const Value *Lookup(const Table &table, const std::string_view key) {
        const auto found = std::ranges::find(table.entries, key, &KeyValue::key);
        return found == table.entries.end() ? nullptr : found->value.get();
    }

    static void RejectUnknownKeys(const Table &table, const std::span<const std::string_view> known) {
        for (const auto &entry : table.entries) {
            if (std::ranges::find(known, entry.key) == known.end()) {
                FailAt(entry.keyLocation, std::format("unknown field '{}' in '[{}]'", entry.key, table.name));
            }
        }
    }

    static const Value &Typed(const Value &value, const Value::Kind kind, const std::string_view field) {
        if (value.kind != kind) {
            static constexpr std::string_view names[] = {"a string", "an integer", "a boolean", "an array",
                                                         "an inline table"};
            FailAt(value.location, std::format("'{}' must be {}, found {}", field,
                                               names[static_cast<std::size_t>(kind)], value.KindName()));
        }
        return value;
    }

    static IdentitySegment ReadIdentity(const Value &value, const std::string_view field) {
        const std::string &text = Typed(value, Value::Kind::String, field).text;
        auto segment = IdentitySegment::Parse(text);
        if (!segment) {
            FailAt(value.location, std::format("'{}' is not a valid identity: {}", field, Describe(segment.error())));
        }
        return *segment;
    }

    static SemanticVersion ReadVersion(const Value &value, const std::string_view field) {
        const std::string &text = Typed(value, Value::Kind::String, field).text;
        auto version = SemanticVersion::Parse(text);
        if (!version) {
            FailAt(value.location, std::format("'{}' is not a valid version: {}", field, Describe(version.error())));
        }
        return *version;
    }

    // Manifest paths are relative, `/`-separated and free of `.` components.
    static std::string ReadPath(const Value &value, const std::string_view field, const bool allowParent) {
        const std::string &text = Typed(value, Value::Kind::String, field).text;
        if (text.empty()) {
            FailAt(value.location, std::format("'{}' cannot be empty", field));
        }
        if (text.find('\\') != std::string::npos) {
            FailAt(value.location, std::format("'{}' must use '/' separators", field));
        }
        if (text.front() == '/' || (text.size() > 1 && text[1] == ':')) {
            FailAt(value.location, std::format("'{}' must be a relative path", field));
        }
        bool sawNormal = false;
        for (const auto part : std::views::split(std::string_view(text), '/')) {
            const std::string_view component(part.begin(), part.end());
            if (component.empty()) {
                FailAt(value.location, std::format("'{}' has an empty path component", field));
            }
            if (component == ".") {
                FailAt(value.location, std::format("'{}' cannot contain a '.' component", field));
            }
            if (component == "..") {
                if (!allowParent) {
                    FailAt(value.location, std::format("'{}' cannot contain a '..' component", field));
                }
                if (sawNormal) {
                    FailAt(value.location, std::format("'{}' cannot use '..' after a normal component", field));
                }
                continue;
            }
            sawNormal = true;
        }
        return text;
    }

    // Metadata URLs are absolute `http`/`https`, carry a host and no credentials.
    static std::string ReadUrl(const Value &value, const std::string_view field) {
        const std::string &text = Typed(value, Value::Kind::String, field).text;
        if (text.size() > manifestMaxUrlBytes) {
            FailAt(value.location, std::format("'{}' exceeds the {}-byte URL limit", field, manifestMaxUrlBytes));
        }
        for (const unsigned char byte : text) {
            if (byte <= ' ' || byte == 0x7F) {
                FailAt(value.location, std::format("'{}' contains an invalid character", field));
            }
        }

        std::string_view rest(text);
        if (rest.starts_with("https://")) {
            rest.remove_prefix(std::string_view("https://").size());
        }
        else if (rest.starts_with("http://")) {
            rest.remove_prefix(std::string_view("http://").size());
        }
        else {
            FailAt(value.location, std::format("'{}' must be an absolute 'http' or 'https' URL", field));
        }

        const std::string_view authority = rest.substr(0, rest.find_first_of("/?#"));
        if (authority.empty()) {
            FailAt(value.location, std::format("'{}' must include a host", field));
        }
        if (authority.find('@') != std::string_view::npos) {
            FailAt(value.location, std::format("'{}' cannot contain credentials", field));
        }
        return text;
    }

    static void ReadHeader(const Table &table, ManifestHeader &header) {
        static constexpr std::string_view known[] = {"Version", "MinRux"};
        RejectUnknownKeys(table, known);

        const Value *version = Lookup(table, "Version");
        if (version == nullptr) {
            FailAt(table.location, "'[Manifest]' must declare 'Version'");
        }
        Typed(*version, Value::Kind::Integer, "Version");
        if (version->integer != manifestSchemaVersion) {
            FailAt(version->location, std::format("unsupported manifest version {}; this compiler accepts version {}",
                                                  version->integer, manifestSchemaVersion));
        }
        header.schemaVersion = manifestSchemaVersion;

        if (const Value *minRux = Lookup(table, "MinRux")) {
            header.minRux = ReadVersion(*minRux, "MinRux");
        }
    }

    static void ReadPackage(const Table &table, Package &package) {
        static constexpr std::string_view known[] = {"Namespace",   "Name",       "Version",  "Type",
                                                     "Description", "Authors",    "Keywords", "License",
                                                     "LicenseFile", "Repository", "Homepage", "ReadmeFile"};
        RejectUnknownKeys(table, known);

        if (const Value *ns = Lookup(table, "Namespace")) {
            package.ns = ReadIdentity(*ns, "Namespace");
        }

        const Value *name = Lookup(table, "Name");
        if (name == nullptr) {
            FailAt(table.location, "'[Package]' must declare 'Name'");
        }
        package.name = ReadIdentity(*name, "Name");

        const Value *version = Lookup(table, "Version");
        if (version == nullptr) {
            FailAt(table.location, "'[Package]' must declare 'Version'");
        }
        package.version = ReadVersion(*version, "Version");

        const Value *type = Lookup(table, "Type");
        if (type == nullptr) {
            FailAt(table.location, "'[Package]' must declare 'Type'");
        }
        const auto parsedType = ParseManifestPackageType(Typed(*type, Value::Kind::String, "Type").text);
        if (!parsedType) {
            FailAt(type->location,
                   std::format("'Type' must be 'Executable', 'SharedLibrary', 'StaticLibrary' or 'SourceLibrary', "
                               "found '{}'",
                               type->text));
        }
        package.type = *parsedType;

        if (const Value *description = Lookup(table, "Description")) {
            package.description = Typed(*description, Value::Kind::String, "Description").text;
        }
        if (const Value *authors = Lookup(table, "Authors")) {
            Typed(*authors, Value::Kind::Array, "Authors");
            if (authors->array.size() > manifestMaxAuthors) {
                FailAt(authors->location, std::format("at most {} authors are allowed", manifestMaxAuthors));
            }
            for (const auto &author : authors->array) {
                package.authors.push_back(Typed(*author, Value::Kind::String, "Authors").text);
            }
        }
        if (const Value *keywords = Lookup(table, "Keywords")) {
            Typed(*keywords, Value::Kind::Array, "Keywords");
            if (keywords->array.size() > manifestMaxKeywords) {
                FailAt(keywords->location, std::format("at most {} keywords are allowed", manifestMaxKeywords));
            }
            for (const auto &keyword : keywords->array) {
                auto segment = ReadIdentity(*keyword, "Keywords");
                for (const auto &existing : package.keywords) {
                    if (existing == segment) {
                        FailAt(keyword->location, std::format("keyword '{}' collides with '{}' after normalization",
                                                              segment.Text(), existing.Text()));
                    }
                }
                package.keywords.push_back(std::move(segment));
            }
        }
        if (const Value *license = Lookup(table, "License")) {
            package.license = Typed(*license, Value::Kind::String, "License").text;
        }
        // A field named '*File' is a path into the package, never a URL, so it
        // is read with the same rules as any other archive entry.
        if (const Value *licenseFile = Lookup(table, "LicenseFile")) {
            package.licenseFile = ReadPath(*licenseFile, "LicenseFile", false);
        }
        if (const Value *repository = Lookup(table, "Repository")) {
            package.repository = ReadUrl(*repository, "Repository");
        }
        if (const Value *homepage = Lookup(table, "Homepage")) {
            package.homepage = ReadUrl(*homepage, "Homepage");
        }
        if (const Value *readmeFile = Lookup(table, "ReadmeFile")) {
            package.readmeFile = ReadPath(*readmeFile, "ReadmeFile", false);
        }
    }

    static void ReadWorkspace(const Table &table, Workspace &workspace) {
        static constexpr std::string_view known[] = {"Packages"};
        RejectUnknownKeys(table, known);

        const Value *packages = Lookup(table, "Packages");
        if (packages == nullptr) {
            FailAt(table.location, "'[Workspace]' must declare 'Packages'");
        }
        Typed(*packages, Value::Kind::Array, "Packages");
        if (packages->array.empty()) {
            FailAt(packages->location, "'Packages' cannot be empty");
        }
        if (packages->array.size() > manifestMaxWorkspacePackages) {
            FailAt(packages->location,
                   std::format("at most {} workspace packages are allowed", manifestMaxWorkspacePackages));
        }
        for (const auto &member : packages->array) {
            auto path = ReadPath(*member, "Packages", false);
            if (std::ranges::find(workspace.packages, path) != workspace.packages.end()) {
                FailAt(member->location, std::format("duplicate workspace package '{}'", path));
            }
            workspace.packages.push_back(std::move(path));
        }
    }

    static void ReadDependencies(const Table &table, std::vector<ManifestDependency> &dependencies) {
        if (table.entries.size() > manifestMaxDependencies) {
            FailAt(table.location, std::format("at most {} dependencies are allowed", manifestMaxDependencies));
        }
        for (const auto &entry : table.entries) {
            ManifestDependency dependency;
            auto importName = IdentitySegment::Parse(entry.key);
            if (!importName) {
                FailAt(entry.keyLocation,
                       std::format("'{}' is not a valid import name: {}", entry.key, Describe(importName.error())));
            }
            dependency.importName = *importName;

            const Value &value = *entry.value;
            Typed(value, Value::Kind::InlineTable, entry.key);

            static constexpr std::string_view known[] = {"Namespace", "Package", "Version", "Path", "TargetOS"};
            for (const auto &field : value.table) {
                if (std::ranges::find(known, field.key) == std::ranges::end(known)) {
                    FailAt(field.keyLocation,
                           std::format("unknown dependency field '{}' in '{}'", field.key, entry.key));
                }
            }

            const auto field = [&](const std::string_view name) -> const KeyValue * {
                const auto found = std::ranges::find(value.table, name, &KeyValue::key);
                return found == value.table.end() ? nullptr : &*found;
            };

            const KeyValue *ns = field("Namespace");
            const KeyValue *requirement = field("Version");
            const KeyValue *path = field("Path");
            const KeyValue *targetOS = field("TargetOS");

            dependency.package = dependency.importName;
            if (const KeyValue *alias = field("Package")) {
                dependency.package = ReadIdentity(*alias->value, "Package");
            }

            if (path != nullptr) {
                if (ns != nullptr || requirement != nullptr) {
                    FailAt(path->keyLocation,
                           std::format("path dependency '{}' cannot also declare 'Namespace' or 'Version'", entry.key));
                }
                dependency.source = PathDependencySource{ReadPath(*path->value, "Path", true)};
            }
            else {
                if (ns == nullptr) {
                    FailAt(entry.keyLocation,
                           std::format("registry dependency '{}' must declare 'Namespace'", entry.key));
                }
                if (requirement == nullptr) {
                    FailAt(entry.keyLocation,
                           std::format("registry dependency '{}' must declare 'Version'", entry.key));
                }
                const std::string &text = Typed(*requirement->value, Value::Kind::String, "Version").text;
                auto range = VersionRange::Parse(text);
                if (!range) {
                    FailAt(requirement->value->location,
                           std::format("'Version' is not a valid requirement: {}", Describe(range.error())));
                }
                dependency.source = RegistryDependencySource{ReadIdentity(*ns->value, "Namespace"), *range};
            }

            if (targetOS != nullptr) {
                Typed(*targetOS->value, Value::Kind::Array, "TargetOS");
                if (targetOS->value->array.empty()) {
                    FailAt(targetOS->value->location, "'TargetOS' cannot be empty; omit it to match every target");
                }
                for (const auto &item : targetOS->value->array) {
                    const std::string &name = Typed(*item, Value::Kind::String, "TargetOS").text;
                    const auto os = ParseManifestTargetOS(name);
                    if (!os) {
                        FailAt(item->location, std::format("'{}' is not a supported TargetOS", name));
                    }
                    if (std::ranges::contains(dependency.targetOS, *os)) {
                        FailAt(item->location, std::format("duplicate TargetOS '{}'", name));
                    }
                    dependency.targetOS.push_back(*os);
                }
            }

            for (const auto &existing : dependencies) {
                if (existing.importName == dependency.importName) {
                    FailAt(entry.keyLocation, std::format("import name '{}' collides with '{}' after normalization",
                                                          entry.key, existing.importName.Text()));
                }
            }
            dependencies.push_back(std::move(dependency));
        }
    }

    static void ReadBuild(const Table &table, Build &build) {
        static constexpr std::string_view known[] = {"Output"};
        RejectUnknownKeys(table, known);
        if (const Value *output = Lookup(table, "Output")) {
            build.output = ReadPath(*output, "Output", true);
        }
    }

    static void ReadDefines(const Table &table, std::map<std::string, DefineValue> &defines) {
        if (table.entries.size() > manifestMaxDefines) {
            FailAt(table.location, std::format("at most {} defines are allowed", manifestMaxDefines));
        }
        for (const auto &entry : table.entries) {
            DefineValue define;
            switch (entry.value->kind) {
            case Value::Kind::String:
                define.kind = DefineValue::Kind::String;
                define.text = entry.value->text;
                break;
            case Value::Kind::Boolean:
                define.kind = DefineValue::Kind::Boolean;
                define.text = entry.value->boolean ? "true" : "false";
                break;
            case Value::Kind::Integer:
                define.kind = DefineValue::Kind::Integer;
                define.text = std::to_string(entry.value->integer);
                break;
            default:
                FailAt(entry.value->location,
                       std::format("define '{}' must be a string, boolean or integer", entry.key));
            }
            defines.emplace(entry.key, std::move(define));
        }
    }
};

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

    try {
        Validator validator(std::move(syntax->tables));
        result.manifest = validator.Run();
    }
    catch (const ValidationFailure &failure) {
        result.diagnostics.push_back({path, failure.location.line, failure.location.column, failure.message});
    }
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
