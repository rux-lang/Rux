#include "Documentation/JsonExport.h"

#include "Diagnostics/Diagnostics.h"
#include "Documentation/Signatures.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Rux::Documentation {
namespace {
/// Writes JSON with two-space indentation, one member per line and `[]`/`{}` for empty containers, the layout
/// `JSON.stringify(value, null, 2)` produces, so a snapshot diffs cleanly and reads the same to both tools.
class JsonWriter {
public:
    void BeginObject() {
        Open('{');
    }

    void EndObject() {
        Close('}');
    }

    void BeginArray() {
        Open('[');
    }

    void EndArray() {
        Close(']');
    }

    void Key(const std::string_view key) {
        Separate();
        text += '"';
        text += EscapeJson(key);
        text += "\": ";
        afterKey = true;
    }

    void String(const std::string_view value) {
        Separate();
        text += '"';
        text += EscapeJson(value);
        text += '"';
    }

    void Number(const std::uint64_t value) {
        Separate();
        text += std::to_string(value);
    }

    void Boolean(const bool value) {
        Separate();
        text += value ? "true" : "false";
    }

    void Null() {
        Separate();
        text += "null";
    }

    void StringOrNull(const std::optional<std::string_view> value) {
        if (value) {
            String(*value);
        }
        else {
            Null();
        }
    }

    /// An empty string stands for a value that was never written.
    void TextOrNull(const std::string_view value) {
        StringOrNull(value.empty() ? std::nullopt : std::optional{value});
    }

    [[nodiscard]] std::string Finish() && {
        text += '\n';
        return std::move(text);
    }

private:
    void Separate() {
        if (afterKey) {
            afterKey = false;
            return;
        }
        if (open.empty()) {
            return;
        }
        if (open.back()) {
            text += ',';
        }
        open.back() = true;
        text += '\n';
        text.append(open.size() * 2, ' ');
    }

    void Open(const char bracket) {
        Separate();
        text += bracket;
        open.push_back(false);
    }

    void Close(const char bracket) {
        const bool populated = open.back();
        open.pop_back();
        if (populated) {
            text += '\n';
            text.append(open.size() * 2, ' ');
        }
        text += bracket;
    }

    std::string text;
    /// One entry per open container: whether it holds anything yet.
    std::vector<bool> open;
    bool afterKey = false;
};

/// One module's source, read once for its header and its constants' values.
struct SourceFile {
    std::string display;
    std::string text;
};

/// An `extend` block, with the source file its constants are sliced from.
struct Extension {
    const ImplDecl *declaration = nullptr;
    const SourceFile *source = nullptr;
};

struct Context {
    const GenerateOptions &options;
    /// Every `extend` block in the package, keyed by the qualified name of the type it extends, in source order.
    std::map<std::string, std::vector<Extension>> extensions;
    std::vector<Diagnostic> diagnostics;
};

std::string Qualify(const std::string &prefix, const std::string &name) {
    return prefix.empty() ? name : prefix + "::" + name;
}

/// The leading `//` header of a file, one Markdown string with its lines joined by newlines, as documentation prose
/// is. A `///` line is documentation for the first item, not a header.
std::optional<std::string> FileHeader(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    std::optional<std::string> header;
    while (!text.empty()) {
        const auto end = text.find('\n');
        std::string_view line = text.substr(0, end);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (!line.starts_with("//") || line.starts_with("///")) {
            break;
        }
        line.remove_prefix(2);
        if (line.starts_with(' ')) {
            line.remove_prefix(1);
        }
        if (header) {
            *header += '\n';
            *header += line;
        }
        else {
            header = std::string(line);
        }
        if (end == std::string_view::npos) {
            break;
        }
        text.remove_prefix(end + 1);
    }
    return header;
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

/// A constant's initializer exactly as written, so `0x52757841_6C6C6F63` stays the hexadecimal the author chose rather
/// than becoming the decimal the parser folded it to.
///
/// The value starts after the first `=` that follows the name and is not part of a longer operator: a type between
/// them can hold `=` only in a range type's `..=`. It ends at the first `;` outside brackets, strings and comments.
std::optional<std::string> InitializerText(const std::string_view text, const ConstDecl &constant) {
    std::size_t index = text.find(constant.name, constant.location.offset);
    if (index == std::string_view::npos) {
        return std::nullopt;
    }
    for (index += constant.name.size(); index < text.size(); ++index) {
        if (text[index] != '=') {
            continue;
        }
        const char after = index + 1 < text.size() ? text[index + 1] : '\0';
        if (text[index - 1] != '.' && after != '=' && after != '>') {
            break;
        }
    }
    const std::size_t start = index + 1;
    int depth = 0;
    for (std::size_t cursor = start; cursor < text.size(); ++cursor) {
        const char c = text[cursor];
        const char next = cursor + 1 < text.size() ? text[cursor + 1] : '\0';
        if (c == '"' || c == '\'') {
            for (++cursor; cursor < text.size() && text[cursor] != c; ++cursor) {
                if (text[cursor] == '\\') {
                    ++cursor;
                }
            }
        }
        else if (c == '/' && next == '/') {
            cursor = std::min(text.find('\n', cursor), text.size());
        }
        else if (c == '/' && next == '*') {
            cursor = std::min(text.find("*/", cursor + 2), text.size()) + 1;
        }
        else if (c == '(' || c == '[' || c == '{') {
            ++depth;
        }
        else if (c == ')' || c == ']' || c == '}') {
            --depth;
        }
        else if (c == ';' && depth <= 0) {
            return std::string(Trim(text.substr(start, cursor - start)));
        }
    }
    return std::nullopt;
}

void RecordDocumentationIssues(Context &context, const Syntax::Documentation &documentation,
                               const std::string &source) {
    for (const auto &issue : documentation.issues) {
        Diagnostic problem = ErrorDiagnostic("invalid documentation: " + issue.message, {},
                                             "fix the documentation comment before generating this item");
        problem.sourceName = source;
        problem.location = issue.range.start;
        context.diagnostics.push_back(std::move(problem));
    }
}

const Syntax::DocumentationTag *FindTag(const Syntax::Documentation &documentation,
                                        const Syntax::DocumentationTagKind kind) {
    const auto tag = std::ranges::find(documentation.tags, kind, &Syntax::DocumentationTag::kind);
    return tag == documentation.tags.end() ? nullptr : &*tag;
}

void WriteNamedTags(JsonWriter &json, const Syntax::Documentation &documentation,
                    const Syntax::DocumentationTagKind kind) {
    json.BeginArray();
    for (const auto &tag : documentation.tags) {
        if (tag.kind != kind) {
            continue;
        }
        json.BeginObject();
        json.Key("name");
        json.String(tag.subject);
        json.Key("markdown");
        json.String(tag.markdown);
        json.EndObject();
    }
    json.EndArray();
}

void WriteDoc(JsonWriter &json, Context &context, const Syntax::Documentation &documentation,
              const std::string &source) {
    using Syntax::DocumentationTagKind;
    RecordDocumentationIssues(context, documentation, source);
    json.BeginObject();
    json.Key("summary");
    json.TextOrNull(documentation.Summary());
    json.Key("markdown");
    json.TextOrNull(documentation.markdown);
    json.Key("typeParams");
    WriteNamedTags(json, documentation, DocumentationTagKind::TypeParameter);
    json.Key("params");
    WriteNamedTags(json, documentation, DocumentationTagKind::Parameter);
    json.Key("returns");
    const auto *returns = FindTag(documentation, DocumentationTagKind::Returns);
    json.StringOrNull(returns ? std::optional<std::string_view>(returns->markdown) : std::nullopt);
    json.Key("see");
    json.BeginArray();
    for (const auto &tag : documentation.tags) {
        if (tag.kind == DocumentationTagKind::See) {
            json.String(tag.subject);
        }
    }
    json.EndArray();
    json.Key("deprecated");
    const auto *deprecated = FindTag(documentation, DocumentationTagKind::Deprecated);
    json.StringOrNull(deprecated ? std::optional<std::string_view>(deprecated->markdown) : std::nullopt);
    json.EndObject();
}

/// A field's or case's documentation, which carries no tags: its prose, or null.
void WriteProse(JsonWriter &json, Context &context, const Syntax::Documentation &documentation,
                const std::string &source) {
    RecordDocumentationIssues(context, documentation, source);
    json.TextOrNull(documentation.markdown);
}

void WriteTypeParams(JsonWriter &json, const std::vector<TypeParameter> &parameters) {
    json.BeginArray();
    for (const auto &parameter : parameters) {
        json.BeginObject();
        json.Key("name");
        json.String(parameter.name);
        json.Key("bounds");
        json.BeginArray();
        for (const auto &bound : parameter.bounds) {
            json.String(TypeText(bound.get()));
        }
        json.EndArray();
        json.EndObject();
    }
    json.EndArray();
}

void WriteValue(JsonWriter &json, Context &context, const ConstDecl &constant, const SourceFile &source) {
    if (!constant.value) {
        json.Null();
        return;
    }
    const auto value = InitializerText(source.text, constant);
    if (!value) {
        Diagnostic problem = ErrorDiagnostic(std::format("could not read the value of constant '{}'", constant.name),
                                             {}, "check that the source file has not changed since it was compiled");
        problem.sourceName = source.display;
        problem.location = constant.location;
        context.diagnostics.push_back(std::move(problem));
    }
    json.StringOrNull(value);
}

const std::vector<TypeParameter> &DeclTypeParams(const Decl &decl) {
    static const std::vector<TypeParameter> none;
    if (const auto *function = dynamic_cast<const FuncDecl *>(&decl)) {
        return function->typeParams;
    }
    if (const auto *structure = dynamic_cast<const StructDecl *>(&decl)) {
        return structure->typeParams;
    }
    if (const auto *enumeration = dynamic_cast<const EnumDecl *>(&decl)) {
        return enumeration->typeParams;
    }
    return none;
}

std::string ItemKind(const Decl &decl) {
    if (const auto *alias = dynamic_cast<const TypeAliasDecl *>(&decl)) {
        return alias->intrinsicName.empty() ? "type" : "intrinsic-type";
    }
    return DeclKind(decl);
}

bool IsOperatorName(const std::string_view name) {
    const unsigned char first = name.empty() ? '\0' : static_cast<unsigned char>(name.front());
    return first != '\0' && !std::isalpha(first) && first != '_' && first != '#' && first != '~';
}

/// One function of an `extend` block or an interface, classified the way a reader looks for it.
std::string_view MemberKind(const FuncDecl &function, const std::string &typeName, const bool requirement) {
    if (requirement) {
        return "requirement";
    }
    if (function.name.starts_with('~')) {
        return "destructor";
    }
    if (IsOperatorName(function.name)) {
        return "operator";
    }
    if (function.Receiver() != nullptr) {
        return "method";
    }
    return function.name == typeName ? "constructor" : "associated";
}

/// Whether a member is part of the type's surface. A destructor runs wherever the type is dropped and an interface
/// requirement is as visible as its interface, so neither is ever written `pub`.
bool MemberPublic(const FuncDecl &function, const bool requirement) {
    return requirement || function.name.starts_with('~') || function.isPublic;
}

void WriteFunctionMember(JsonWriter &json, Context &context, const FuncDecl &function, const std::string &typeName,
                         const std::optional<std::string> &conformance, const bool requirement,
                         const std::string &source) {
    json.BeginObject();
    json.Key("kind");
    json.String(MemberKind(function, typeName, requirement));
    json.Key("name");
    json.String(function.name);
    json.Key("line");
    json.Number(function.location.line);
    json.Key("signature");
    json.String(FunctionSignature(function));
    json.Key("typeParams");
    WriteTypeParams(json, function.typeParams);
    json.Key("params");
    json.BeginArray();
    for (const auto &parameter : function.params) {
        if (parameter.IsReceiver()) {
            continue;
        }
        json.BeginObject();
        json.Key("name");
        json.String(parameter.isVariadic ? "..." : parameter.name);
        json.Key("type");
        json.String(parameter.isVariadic ? "..." : TypeText(parameter.type.get()));
        json.EndObject();
    }
    json.EndArray();
    json.Key("receiver");
    const Param *receiver = function.Receiver();
    json.StringOrNull(receiver ? std::optional<std::string>(TypeText(receiver->type.get())) : std::nullopt);
    json.Key("returnType");
    json.StringOrNull(function.returnType ? std::optional<std::string>(TypeText(function.returnType->get()))
                                          : std::nullopt);
    json.Key("value");
    json.Null();
    json.Key("conformance");
    json.StringOrNull(conformance);
    json.Key("public");
    json.Boolean(MemberPublic(function, requirement));
    json.Key("doc");
    WriteDoc(json, context, function.documentation, source);
    json.EndObject();
}

void WriteConstantMember(JsonWriter &json, Context &context, const ConstDecl &constant,
                         const std::optional<std::string> &conformance, const SourceFile &source) {
    json.BeginObject();
    json.Key("kind");
    json.String("constant");
    json.Key("name");
    json.String(constant.name);
    json.Key("line");
    json.Number(constant.location.line);
    json.Key("signature");
    json.String(DeclSignature(constant));
    json.Key("typeParams");
    json.BeginArray();
    json.EndArray();
    json.Key("params");
    json.BeginArray();
    json.EndArray();
    json.Key("receiver");
    json.Null();
    json.Key("returnType");
    json.Null();
    json.Key("value");
    WriteValue(json, context, constant, source);
    json.Key("conformance");
    json.StringOrNull(conformance);
    json.Key("public");
    json.Boolean(constant.isPublic);
    json.Key("doc");
    WriteDoc(json, context, constant.documentation, source.display);
    json.EndObject();
}

/// The members of a type: an interface's requirements, then every function and constant of every `extend` block of
/// the type in source order. A block keeps its constants and functions apart, so they are merged back by position.
void WriteMembers(JsonWriter &json, Context &context, const Decl &decl, const std::string &qualified,
                  const SourceFile &source) {
    const std::string typeName = DeclName(decl);
    const bool includePrivate = context.options.includePrivate;
    json.BeginArray();
    if (const auto *interface = dynamic_cast<const InterfaceDecl *>(&decl)) {
        for (const auto &method : interface->methods) {
            WriteFunctionMember(json, context, *method, typeName, std::nullopt, true, source.display);
        }
    }
    const auto found = context.extensions.find(qualified);
    if (found != context.extensions.end()) {
        for (const auto &[extension, extensionSource] : found->second) {
            std::vector<const Decl *> members;
            for (const auto &method : extension->methods) {
                if (includePrivate || MemberPublic(*method, false)) {
                    members.push_back(method.get());
                }
            }
            for (const auto &constant : extension->constants) {
                if (includePrivate || constant->isPublic) {
                    members.push_back(constant.get());
                }
            }
            std::ranges::stable_sort(members, {}, [](const Decl *member) { return member->location.offset; });
            for (const Decl *member : members) {
                if (const auto *function = dynamic_cast<const FuncDecl *>(member)) {
                    WriteFunctionMember(json, context, *function, typeName, extension->interfaceName, false,
                                        extensionSource->display);
                }
                else {
                    WriteConstantMember(json, context, *dynamic_cast<const ConstDecl *>(member),
                                        extension->interfaceName, *extensionSource);
                }
            }
        }
    }
    json.EndArray();
}

void WriteImplements(JsonWriter &json, const Context &context, const std::string &qualified) {
    std::vector<std::string> interfaces;
    if (const auto found = context.extensions.find(qualified); found != context.extensions.end()) {
        for (const auto &extension : found->second) {
            const auto &name = extension.declaration->interfaceName;
            if (name && !std::ranges::contains(interfaces, *name)) {
                interfaces.push_back(*name);
            }
        }
    }
    json.BeginArray();
    for (const auto &name : interfaces) {
        json.String(name);
    }
    json.EndArray();
}

template <typename Field>
void WriteFields(JsonWriter &json, Context &context, const std::vector<Field> &fields, const std::string &source) {
    json.BeginArray();
    for (const auto &field : fields) {
        json.BeginObject();
        json.Key("name");
        json.String(field.name);
        json.Key("type");
        json.String(TypeText(field.type.get()));
        json.Key("public");
        json.Boolean(field.isPublic);
        json.Key("line");
        json.Number(field.location.line);
        json.Key("doc");
        WriteProse(json, context, field.documentation, source);
        json.EndObject();
    }
    json.EndArray();
}

/// A variant case's payload as source spells it after the name, or nothing for a unit case.
std::optional<std::string> CasePayload(const EnumDecl &declaration, const EnumDecl::Variant &variant) {
    if (!declaration.IsVariant() || (variant.fields.empty() && variant.namedFields.empty())) {
        return std::nullopt;
    }
    std::string payload = CaseSignature(declaration, variant).substr(variant.name.size());
    return std::string(Trim(payload));
}

void WriteCases(JsonWriter &json, Context &context, const EnumDecl &declaration, const std::string &source) {
    json.BeginArray();
    for (const auto &variant : declaration.variants) {
        json.BeginObject();
        json.Key("name");
        json.String(variant.name);
        json.Key("value");
        json.StringOrNull(declaration.IsVariant() ? std::nullopt : variant.discriminant);
        json.Key("payload");
        json.StringOrNull(CasePayload(declaration, variant));
        json.Key("line");
        json.Number(variant.location.line);
        json.Key("doc");
        WriteProse(json, context, variant.documentation, source);
        json.EndObject();
    }
    json.EndArray();
}

void WriteItem(JsonWriter &json, Context &context, const Decl &decl, const std::string &moduleName,
               const SourceFile &source, const std::string &prefix) {
    const std::string qualified = Qualify(prefix, DeclName(decl));
    const auto &typeParams = DeclTypeParams(decl);
    std::string displayName = qualified;
    if (!typeParams.empty()) {
        displayName += '<';
        for (std::size_t index = 0; index < typeParams.size(); ++index) {
            displayName += (index == 0 ? "" : ", ") + typeParams[index].name;
        }
        displayName += '>';
    }
    const auto *structure = dynamic_cast<const StructDecl *>(&decl);
    const auto *unionType = dynamic_cast<const UnionDecl *>(&decl);
    const auto *enumeration = dynamic_cast<const EnumDecl *>(&decl);
    const auto *constant = dynamic_cast<const ConstDecl *>(&decl);
    const bool type = structure || unionType || enumeration || dynamic_cast<const InterfaceDecl *>(&decl) ||
                      dynamic_cast<const TypeAliasDecl *>(&decl);

    json.BeginObject();
    json.Key("kind");
    json.String(ItemKind(decl));
    json.Key("name");
    json.String(qualified);
    json.Key("displayName");
    json.String(displayName);
    json.Key("module");
    json.String(moduleName);
    json.Key("source");
    json.String(source.display);
    json.Key("line");
    json.Number(decl.location.line);
    json.Key("signature");
    json.String(DeclSignature(decl));
    json.Key("typeParams");
    WriteTypeParams(json, typeParams);
    json.Key("doc");
    WriteDoc(json, context, decl.documentation, source.display);
    json.Key("value");
    if (constant) {
        WriteValue(json, context, *constant, source);
    }
    else {
        json.Null();
    }
    json.Key("fields");
    if (structure) {
        WriteFields(json, context, structure->fields, source.display);
    }
    else if (unionType) {
        WriteFields(json, context, unionType->fields, source.display);
    }
    else {
        json.BeginArray();
        json.EndArray();
    }
    json.Key("baseType");
    json.StringOrNull(enumeration && !enumeration->IsVariant() && enumeration->baseType
                          ? std::optional<std::string>(TypeText(enumeration->baseType.get()))
                          : std::nullopt);
    json.Key("cases");
    if (enumeration) {
        WriteCases(json, context, *enumeration, source.display);
    }
    else {
        json.BeginArray();
        json.EndArray();
    }
    json.Key("members");
    if (type) {
        WriteMembers(json, context, decl, qualified, source);
    }
    else {
        json.BeginArray();
        json.EndArray();
    }
    json.Key("implements");
    if (type) {
        WriteImplements(json, context, qualified);
    }
    else {
        json.BeginArray();
        json.EndArray();
    }
    json.EndObject();
}

/// Write every visible item `decl` stands for. A module contributes its items under qualified names rather than an
/// item of its own, an extern block contributes each of its declarations, and an `extend` block contributes members
/// to the type it extends rather than an item.
void WriteItems(JsonWriter &json, Context &context, const Decl &decl, const std::string &moduleName,
                const SourceFile &source, const std::string &prefix = {}, const bool containingModulesPublic = true) {
    // The public type names only decide whether an `extend` block is shown, and a block never becomes an item.
    static const std::unordered_set<std::string> unconsulted;
    if (dynamic_cast<const ImplDecl *>(&decl) ||
        !Visible(decl, context.options.includePrivate, containingModulesPublic, unconsulted)) {
        return;
    }
    if (const auto *block = dynamic_cast<const ExternBlockDecl *>(&decl)) {
        for (const auto &item : block->items) {
            WriteItems(json, context, *item, moduleName, source, prefix, containingModulesPublic);
        }
        return;
    }
    if (const auto *module = dynamic_cast<const ModuleDecl *>(&decl)) {
        for (const auto &item : module->items) {
            WriteItems(json, context, *item, moduleName, source, Qualify(prefix, module->name),
                       containingModulesPublic && module->isPublic);
        }
        return;
    }
    if (DeclName(decl).empty()) {
        return;
    }
    WriteItem(json, context, decl, moduleName, source, prefix);
}

void CollectExtensions(Context &context, const Decl &decl, const SourceFile &source, const std::string &prefix = {}) {
    if (const auto *module = dynamic_cast<const ModuleDecl *>(&decl)) {
        for (const auto &item : module->items) {
            CollectExtensions(context, *item, source, Qualify(prefix, module->name));
        }
        return;
    }
    if (const auto *extension = dynamic_cast<const ImplDecl *>(&decl)) {
        const std::string typeName = extension->typeName.substr(0, extension->typeName.find('<'));
        context.extensions[Qualify(prefix, typeName)].push_back({extension, &source});
    }
}

void WritePackage(JsonWriter &json, const Manifest &manifest) {
    const auto &package = manifest.package;
    json.BeginObject();
    json.Key("name");
    json.String(package.name.Text());
    json.Key("namespace");
    json.StringOrNull(package.ns ? std::optional<std::string_view>(package.ns->Text()) : std::nullopt);
    json.Key("version");
    json.String(package.version.Text());
    json.Key("description");
    json.TextOrNull(package.description);
    json.Key("license");
    json.TextOrNull(package.license);
    json.Key("minRux");
    json.StringOrNull(manifest.header.minRux ? std::optional<std::string_view>(manifest.header.minRux->Text())
                                             : std::nullopt);
    json.Key("repository");
    json.TextOrNull(package.repository);
    json.Key("homepage");
    json.TextOrNull(package.homepage);
    json.Key("dependencies");
    json.BeginArray();
    for (const auto &dependency : manifest.dependencies) {
        const auto *registry = dependency.Registry();
        json.BeginObject();
        json.Key("name");
        json.String(dependency.package.Text());
        json.Key("namespace");
        json.StringOrNull(registry ? std::optional<std::string_view>(registry->ns.Text()) : std::nullopt);
        json.Key("version");
        json.StringOrNull(registry ? std::optional<std::string_view>(registry->version.Text()) : std::nullopt);
        json.Key("targetOS");
        json.BeginArray();
        for (const auto os : dependency.targetOS) {
            json.String(ManifestTargetOSName(os));
        }
        json.EndArray();
        json.EndObject();
    }
    json.EndArray();
    json.EndObject();
}

SourceFile ReadSource(Context &context, const std::filesystem::path &sourcePath) {
    SourceFile source{.display = SourceDisplayName(sourcePath, context.options.packageRoot), .text = {}};
    std::filesystem::path path = sourcePath;
    if (std::error_code ec; !path.is_absolute() && std::filesystem::exists(context.options.packageRoot / path, ec)) {
        path = context.options.packageRoot / path;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        context.diagnostics.push_back(
            ErrorDiagnostic(std::format("could not read source file '{}' for its documentation", path.string()), {},
                            "check that the package's sources are readable"));
        return source;
    }
    source.text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return source;
}
} // namespace

bool JsonSnapshot::HasErrors() const {
    return std::ranges::any_of(diagnostics,
                               [](const Diagnostic &item) { return item.severity == Diagnostic::Severity::Error; });
}

JsonSnapshot RenderJson(const Manifest &manifest, const std::span<const ParseResult> modules,
                        const GenerateOptions &options) {
    Context context{.options = options, .extensions = {}, .diagnostics = {}};
    std::vector<SourceFile> sources;
    sources.reserve(modules.size());
    for (const auto &module : modules) {
        sources.push_back(ReadSource(context, std::filesystem::path(module.module.name)));
    }
    for (std::size_t index = 0; index < modules.size(); ++index) {
        for (const auto &declaration : modules[index].module.items) {
            CollectExtensions(context, *declaration, sources[index]);
        }
    }

    JsonWriter json;
    json.BeginObject();
    json.Key("schema");
    json.Number(JsonSchemaVersion);
    json.Key("target");
    json.String(options.target);
    json.Key("package");
    WritePackage(json, manifest);
    json.Key("modules");
    json.BeginArray();
    for (std::size_t index = 0; index < modules.size(); ++index) {
        json.BeginObject();
        json.Key("name");
        json.String(std::filesystem::path(modules[index].module.name).stem().string());
        json.Key("source");
        json.String(sources[index].display);
        json.Key("header");
        json.StringOrNull(FileHeader(sources[index].text));
        json.EndObject();
    }
    json.EndArray();
    json.Key("items");
    json.BeginArray();
    for (std::size_t index = 0; index < modules.size(); ++index) {
        const std::string moduleName = std::filesystem::path(modules[index].module.name).stem().string();
        for (const auto &declaration : modules[index].module.items) {
            WriteItems(json, context, *declaration, moduleName, sources[index]);
        }
    }
    json.EndArray();
    json.EndObject();

    return {.file = {.name = manifest.package.name.Text() + ".json", .content = std::move(json).Finish()},
            .diagnostics = std::move(context.diagnostics)};
}

GenerateResult GenerateJson(const Manifest &manifest, const std::span<const ParseResult> modules,
                            const GenerateOptions &options) {
    auto snapshot = RenderJson(manifest, modules, options);
    if (snapshot.HasErrors()) {
        return {.ok = false, .diagnostics = std::move(snapshot.diagnostics)};
    }
    auto result = InstallManagedDirectory(options.outputDirectory, std::span(&snapshot.file, 1));
    result.diagnostics.insert(result.diagnostics.begin(), snapshot.diagnostics.begin(), snapshot.diagnostics.end());
    return result;
}
} // namespace Rux::Documentation
