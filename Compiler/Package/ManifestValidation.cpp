#include "Package/ManifestValidation.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <span>

namespace Rux::ManifestDetail {
namespace {
class Validator {
public:
    explicit Validator(Document input)
        : tables(std::move(input.tables)) {
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
        throw ValidationError{location, std::move(message)};
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
} // namespace

std::expected<Manifest, ValidationError> ValidateManifestV1(Document document) {
    try {
        return Validator(std::move(document)).Run();
    }
    catch (ValidationError &failure) {
        return std::unexpected(std::move(failure));
    }
}
} // namespace Rux::ManifestDetail
