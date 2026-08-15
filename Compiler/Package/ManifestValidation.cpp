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

    ValidationResult Run() {
        Manifest manifest;
        for (const auto &table : tables) {
            static constexpr std::string_view known[] = {"Manifest",     "Package", "Workspace",
                                                         "Dependencies", "Build",   "Build.Defines"};
            if (std::ranges::find(known, table.name) == std::ranges::end(known)) {
                AddUnknown(table.location, "section", table.name, known, true);
            }
            for (const auto &other : tables) {
                if (&other != &table && other.name == table.name && Before(other.location, table.location)) {
                    Add(table.location, std::format("duplicate section '[{}]'", table.name),
                        "remove the repeated section or merge its fields into the first one");
                }
            }
        }

        const Table *header = Find("Manifest");
        if (header == nullptr) {
            Add({1, 1}, "manifest must start with a '[Manifest]' section declaring 'Version = 1'",
                "add '[Manifest]' followed by 'Version = 1'");
        }
        else {
            ReadHeader(*header, manifest.header);
        }

        const Table *packageTable = Find("Package");
        const Table *workspaceTable = Find("Workspace");
        if (packageTable != nullptr && workspaceTable != nullptr) {
            Add(workspaceTable->location, "'[Package]' and '[Workspace]' are mutually exclusive",
                "remove one section; a manifest describes either one package or one workspace");
        }
        if (packageTable == nullptr && workspaceTable == nullptr) {
            Add(header != nullptr ? header->location : Location{1, 1},
                "manifest must declare either '[Package]' or '[Workspace]'",
                "add the section that describes this manifest");
        }

        if (workspaceTable != nullptr && packageTable == nullptr) {
            ReadWorkspace(*workspaceTable, manifest.workspace);
            if (const Table *deps = Find("Dependencies")) {
                Add(deps->location, "a workspace cannot declare dependencies in '[Dependencies]'",
                    "move dependencies into a workspace member package");
            }
            if (const Table *build = Find("Build")) {
                Add(build->location, "a workspace cannot declare build settings in '[Build]'",
                    "move build settings into a workspace member package");
            }
            if (const Table *defines = Find("Build.Defines")) {
                Add(defines->location, "a workspace cannot declare build settings in '[Build.Defines]'",
                    "move build settings into a workspace member package");
            }
        }
        else if (packageTable != nullptr) {
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
        }

        std::ranges::stable_sort(errors, [](const ValidationError &left, const ValidationError &right) {
            return Before(left.location, right.location);
        });
        if (!errors.empty()) {
            return {std::nullopt, std::move(errors)};
        }
        return {std::move(manifest), {}};
    }

private:
    std::vector<Table> tables;
    std::vector<ValidationError> errors;

    static bool Before(const Location left, const Location right) noexcept {
        return left.line < right.line || (left.line == right.line && left.column < right.column);
    }

    [[noreturn]] static void FailAt(const Location location, std::string message,
                                    std::optional<std::string> help = {}) {
        throw ValidationError{location, std::move(message), std::move(help), std::string(manifestDocumentationUrl)};
    }

    void Add(const Location location, std::string message, std::optional<std::string> help = {}) {
        errors.push_back({location, std::move(message), std::move(help), std::string(manifestDocumentationUrl)});
    }

    template <typename Function>
    void Try(Function &&function) {
        try {
            function();
        }
        catch (ValidationError &failure) {
            errors.push_back(std::move(failure));
        }
    }

    static bool EqualIgnoringCase(const std::string_view left, const std::string_view right) {
        return left.size() == right.size() && std::ranges::equal(left, right, [](const char a, const char b) {
                   const auto Lower = [](const char value) {
                       return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
                   };
                   return Lower(a) == Lower(b);
               });
    }

    void AddUnknown(const Location location, const std::string_view kind, const std::string_view actual,
                    const std::span<const std::string_view> known, const bool section = false) {
        std::optional<std::string> help;
        for (const auto expected : known) {
            if (EqualIgnoringCase(actual, expected)) {
                help = section ? std::format("section names are case-sensitive; use '[{}]'", expected)
                               : std::format("field names are case-sensitive; use '{}'", expected);
                break;
            }
        }
        Add(location,
            section ? std::format("unknown section '[{}]'", actual) : std::format("unknown {} '{}'", kind, actual),
            std::move(help));
    }

    [[nodiscard]] const Table *Find(const std::string_view name) const {
        const auto found = std::ranges::find(tables, name, &Table::name);
        return found == tables.end() ? nullptr : &*found;
    }

    static const Value *Lookup(const Table &table, const std::string_view key) {
        const auto found = std::ranges::find(table.entries, key, &KeyValue::key);
        return found == table.entries.end() ? nullptr : found->value.get();
    }

    void RejectUnknownKeys(const Table &table, const std::span<const std::string_view> known) {
        for (const auto &entry : table.entries) {
            if (std::ranges::find(known, entry.key) == known.end()) {
                std::optional<std::string> help;
                for (const auto expected : known) {
                    if (EqualIgnoringCase(entry.key, expected)) {
                        help = std::format("field names are case-sensitive; use '{}'", expected);
                        break;
                    }
                }
                Add(entry.keyLocation, std::format("unknown field '{}' in [{}]", entry.key, table.name),
                    std::move(help));
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

    void ReadHeader(const Table &table, ManifestHeader &header) {
        static constexpr std::string_view known[] = {"Version", "MinRux"};
        RejectUnknownKeys(table, known);

        const Value *version = Lookup(table, "Version");
        if (version == nullptr) {
            Add(table.location, "[Manifest] must declare 'Version'", "add 'Version = 1' to [Manifest]");
        }
        else {
            Try([&] {
                Typed(*version, Value::Kind::Integer, "[Manifest].Version");
                if (version->integer != manifestSchemaVersion) {
                    FailAt(version->location,
                           std::format("unsupported manifest version {} in [Manifest].Version; this compiler accepts "
                                       "version {}",
                                       version->integer, manifestSchemaVersion),
                           "set '[Manifest].Version = 1'");
                }
                header.schemaVersion = manifestSchemaVersion;
            });
        }

        if (const Value *minRux = Lookup(table, "MinRux")) {
            Try([&] { header.minRux = ReadVersion(*minRux, "[Manifest].MinRux"); });
        }
    }

    void ReadPackage(const Table &table, Package &package) {
        static constexpr std::string_view known[] = {"Namespace",   "Name",       "Version",  "Type",
                                                     "Description", "Authors",    "Keywords", "License",
                                                     "LicenseFile", "Repository", "Homepage", "ReadmeFile"};
        RejectUnknownKeys(table, known);

        if (const Value *ns = Lookup(table, "Namespace")) {
            Try([&] { package.ns = ReadIdentity(*ns, "[Package].Namespace"); });
        }
        const Value *name = Lookup(table, "Name");
        if (name == nullptr) {
            Add(table.location, "[Package] must declare 'Name'", "add a package identity such as 'Name = \"App\"'");
        }
        else {
            Try([&] { package.name = ReadIdentity(*name, "[Package].Name"); });
        }

        const Value *version = Lookup(table, "Version");
        if (version == nullptr) {
            Add(table.location, "[Package] must declare 'Version'",
                "add a semantic version such as 'Version = \"0.1.0\"'");
        }
        else {
            Try([&] { package.version = ReadVersion(*version, "[Package].Version"); });
        }

        const Value *type = Lookup(table, "Type");
        if (type == nullptr) {
            Add(table.location, "[Package] must declare 'Type'",
                "use 'Executable', 'SharedLibrary', 'StaticLibrary' or 'SourceLibrary'");
        }
        else {
            Try([&] {
                const auto parsedType =
                    ParseManifestPackageType(Typed(*type, Value::Kind::String, "[Package].Type").text);
                if (!parsedType) {
                    std::optional<std::string> help;
                    static constexpr std::string_view allowed[] = {"Executable", "SharedLibrary", "StaticLibrary",
                                                                   "SourceLibrary"};
                    for (const auto expected : allowed) {
                        if (EqualIgnoringCase(type->text, expected)) {
                            help = std::format("enum values are case-sensitive; use '{}'", expected);
                            break;
                        }
                    }
                    FailAt(type->location,
                           std::format("[Package].Type must be 'Executable', 'SharedLibrary', 'StaticLibrary' or "
                                       "'SourceLibrary', found '{}'",
                                       type->text),
                           std::move(help));
                }
                package.type = *parsedType;
            });
        }

        if (const Value *description = Lookup(table, "Description")) {
            Try([&] { package.description = Typed(*description, Value::Kind::String, "[Package].Description").text; });
        }
        if (const Value *authors = Lookup(table, "Authors")) {
            Try([&] {
                Typed(*authors, Value::Kind::Array, "[Package].Authors");
                if (authors->array.size() > manifestMaxAuthors) {
                    FailAt(authors->location,
                           std::format("[Package].Authors allows at most {} entries", manifestMaxAuthors));
                }
                for (const auto &author : authors->array) {
                    package.authors.push_back(Typed(*author, Value::Kind::String, "[Package].Authors item").text);
                }
            });
        }
        if (const Value *keywords = Lookup(table, "Keywords")) {
            Try([&] {
                Typed(*keywords, Value::Kind::Array, "[Package].Keywords");
                if (keywords->array.size() > manifestMaxKeywords) {
                    FailAt(keywords->location,
                           std::format("[Package].Keywords allows at most {} entries", manifestMaxKeywords));
                }
                for (const auto &keyword : keywords->array) {
                    auto segment = ReadIdentity(*keyword, "[Package].Keywords item");
                    for (const auto &existing : package.keywords) {
                        if (existing == segment) {
                            FailAt(keyword->location,
                                   std::format("[Package].Keywords entry '{}' collides with '{}' after normalization",
                                               segment.Text(), existing.Text()));
                        }
                    }
                    package.keywords.push_back(std::move(segment));
                }
            });
        }
        if (const Value *license = Lookup(table, "License")) {
            Try([&] { package.license = Typed(*license, Value::Kind::String, "[Package].License").text; });
        }
        if (const Value *licenseFile = Lookup(table, "LicenseFile")) {
            Try([&] { package.licenseFile = ReadPath(*licenseFile, "[Package].LicenseFile", false); });
        }
        if (const Value *repository = Lookup(table, "Repository")) {
            Try([&] { package.repository = ReadUrl(*repository, "[Package].Repository"); });
        }
        if (const Value *homepage = Lookup(table, "Homepage")) {
            Try([&] { package.homepage = ReadUrl(*homepage, "[Package].Homepage"); });
        }
        if (const Value *readmeFile = Lookup(table, "ReadmeFile")) {
            Try([&] { package.readmeFile = ReadPath(*readmeFile, "[Package].ReadmeFile", false); });
        }
    }

    void ReadWorkspace(const Table &table, Workspace &workspace) {
        static constexpr std::string_view known[] = {"Packages"};
        RejectUnknownKeys(table, known);
        const Value *packages = Lookup(table, "Packages");
        if (packages == nullptr) {
            Add(table.location, "[Workspace] must declare 'Packages'", "add a non-empty array of package paths");
            return;
        }
        Try([&] { Typed(*packages, Value::Kind::Array, "[Workspace].Packages"); });
        if (packages->kind != Value::Kind::Array) {
            return;
        }
        if (packages->array.empty()) {
            Add(packages->location, "[Workspace].Packages cannot be empty", "list at least one package path");
        }
        if (packages->array.size() > manifestMaxWorkspacePackages) {
            Add(packages->location,
                std::format("[Workspace].Packages allows at most {} entries", manifestMaxWorkspacePackages));
        }
        for (const auto &member : packages->array) {
            Try([&] {
                auto path = ReadPath(*member, "[Workspace].Packages item", false);
                if (std::ranges::find(workspace.packages, path) != workspace.packages.end()) {
                    FailAt(member->location,
                           std::format("duplicate workspace package '{}' in [Workspace].Packages", path));
                }
                workspace.packages.push_back(std::move(path));
            });
        }
    }

    const KeyValue *DependencyField(const Value &value, const std::string_view name) const {
        const auto found = std::ranges::find(value.table, name, &KeyValue::key);
        return found == value.table.end() ? nullptr : &*found;
    }

    void ReadDependency(const KeyValue &entry, std::vector<ManifestDependency> &dependencies) {
        ManifestDependency dependency;
        auto importName = IdentitySegment::Parse(entry.key);
        if (!importName) {
            FailAt(entry.keyLocation, std::format("[Dependencies] import name '{}' is invalid: {}", entry.key,
                                                  Describe(importName.error())));
        }
        dependency.importName = *importName;

        const Value &value = *entry.value;
        Typed(value, Value::Kind::InlineTable, std::format("[Dependencies].{}", entry.key));
        static constexpr std::string_view known[] = {"Namespace", "Package", "Version", "Path", "TargetOS"};
        for (const auto &field : value.table) {
            if (std::ranges::find(known, field.key) == std::ranges::end(known)) {
                std::optional<std::string> help;
                for (const auto expected : known) {
                    if (EqualIgnoringCase(field.key, expected)) {
                        help = std::format("field names are case-sensitive; use '{}'", expected);
                        break;
                    }
                }
                Add(field.keyLocation, std::format("unknown dependency field '{}' in '{}'", field.key, entry.key),
                    std::move(help));
            }
        }

        const KeyValue *ns = DependencyField(value, "Namespace");
        const KeyValue *requirement = DependencyField(value, "Version");
        const KeyValue *path = DependencyField(value, "Path");
        const KeyValue *targetOS = DependencyField(value, "TargetOS");

        dependency.package = dependency.importName;
        if (const KeyValue *alias = DependencyField(value, "Package")) {
            dependency.package = ReadIdentity(*alias->value, std::format("dependency '{}'.Package", entry.key));
        }
        if (path != nullptr) {
            if (ns != nullptr || requirement != nullptr) {
                FailAt(path->keyLocation,
                       std::format("path dependency '{}' cannot also declare 'Namespace' or 'Version'", entry.key),
                       "remove 'Path' or remove both registry source fields");
            }
            dependency.source =
                PathDependencySource{ReadPath(*path->value, std::format("dependency '{}'.Path", entry.key), true)};
        }
        else {
            if (ns == nullptr) {
                FailAt(entry.keyLocation, std::format("registry dependency '{}' must declare 'Namespace'", entry.key));
            }
            if (requirement == nullptr) {
                FailAt(entry.keyLocation, std::format("registry dependency '{}' must declare 'Version'", entry.key));
            }
            const std::string &text =
                Typed(*requirement->value, Value::Kind::String, std::format("dependency '{}'.Version", entry.key)).text;
            auto range = VersionRange::Parse(text);
            if (!range) {
                FailAt(requirement->value->location,
                       std::format("dependency '{}'.Version is not a valid requirement: {}", entry.key,
                                   Describe(range.error())));
            }
            dependency.source = RegistryDependencySource{
                ReadIdentity(*ns->value, std::format("dependency '{}'.Namespace", entry.key)), *range};
        }

        if (targetOS != nullptr) {
            Typed(*targetOS->value, Value::Kind::Array, std::format("dependency '{}'.TargetOS", entry.key));
            if (targetOS->value->array.empty()) {
                FailAt(targetOS->value->location, std::format("dependency '{}'.TargetOS cannot be empty", entry.key),
                       "omit 'TargetOS' to match every target");
            }
            for (const auto &item : targetOS->value->array) {
                const std::string &name =
                    Typed(*item, Value::Kind::String, std::format("dependency '{}'.TargetOS item", entry.key)).text;
                const auto os = ParseManifestTargetOS(name);
                if (!os) {
                    std::optional<std::string> help;
                    static constexpr std::string_view allowed[] = {"FreeBSD", "Linux", "macOS", "Windows"};
                    for (const auto expected : allowed) {
                        if (EqualIgnoringCase(name, expected)) {
                            help = std::format("enum values are case-sensitive; use '{}'", expected);
                            break;
                        }
                    }
                    FailAt(item->location,
                           std::format("'{}' is not a supported TargetOS for dependency '{}'; allowed values are "
                                       "'FreeBSD', 'Linux', 'macOS' and 'Windows'",
                                       name, entry.key),
                           std::move(help));
                }
                if (std::ranges::contains(dependency.targetOS, *os)) {
                    FailAt(item->location, std::format("duplicate TargetOS '{}' in dependency '{}'", name, entry.key));
                }
                dependency.targetOS.push_back(*os);
            }
        }

        for (const auto &existing : dependencies) {
            if (existing.importName == dependency.importName) {
                FailAt(entry.keyLocation,
                       std::format("[Dependencies] import name '{}' collides with '{}' after normalization", entry.key,
                                   existing.importName.Text()));
            }
        }
        dependencies.push_back(std::move(dependency));
    }

    void ReadDependencies(const Table &table, std::vector<ManifestDependency> &dependencies) {
        if (table.entries.size() > manifestMaxDependencies) {
            Add(table.location, std::format("[Dependencies] allows at most {} entries", manifestMaxDependencies));
        }
        for (const auto &entry : table.entries) {
            Try([&] { ReadDependency(entry, dependencies); });
        }
    }

    void ReadBuild(const Table &table, Build &build) {
        static constexpr std::string_view known[] = {"Output"};
        RejectUnknownKeys(table, known);
        if (const Value *output = Lookup(table, "Output")) {
            Try([&] { build.output = ReadPath(*output, "[Build].Output", true); });
        }
    }

    void ReadDefines(const Table &table, std::map<std::string, DefineValue> &defines) {
        if (table.entries.size() > manifestMaxDefines) {
            Add(table.location, std::format("[Build.Defines] allows at most {} entries", manifestMaxDefines));
        }
        for (const auto &entry : table.entries) {
            Try([&] {
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
                           std::format("[Build.Defines].{} must be a string, boolean or integer", entry.key));
                }
                defines.emplace(entry.key, std::move(define));
            });
        }
    }
};
} // namespace

std::expected<Manifest, ValidationError> ValidateManifestV1(Document document) {
    auto result = ValidateManifestV1All(std::move(document));
    if (!result.Ok()) {
        return std::unexpected(std::move(result.diagnostics.front()));
    }
    return std::move(*result.manifest);
}

ValidationResult ValidateManifestV1All(Document document) {
    return Validator(std::move(document)).Run();
}
} // namespace Rux::ManifestDetail
