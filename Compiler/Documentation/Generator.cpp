#include "Documentation/Generator.h"

#include "Diagnostics/Diagnostics.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <format>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

namespace Rux::Documentation {
namespace {
constexpr std::string_view MarkerName = ".rux-docs";

struct SearchEntry {
    std::string name;
    std::string kind;
    std::string module;
    std::string href;
};

/// Escape text for HTML. Documentation text comes from source comments, so it is author-controlled but not trusted
/// markup: everything is escaped and only the inline forms below are re-introduced deliberately.
std::string EscapeHtml(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char c : value) {
        switch (c) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        case '\'':
            result += "&#39;";
            break;
        default:
            result += c;
            break;
        }
    }
    return result;
}

/// Whether a link target may be emitted as an anchor. Restricted to schemes that cannot execute, so a doc comment
/// cannot turn into script when the generated page is opened.
bool SafeLink(const std::string_view link) {
    const auto colon = link.find(':');
    if (colon == std::string_view::npos)
        return true;
    std::string scheme(link.substr(0, colon));
    std::ranges::transform(scheme, scheme.begin(), [](const unsigned char c) { return std::tolower(c); });
    return scheme == "http" || scheme == "https" || scheme == "mailto";
}

/// Render the inline subset the generator supports — code spans, emphasis, links — over already-escaped text.
std::string InlineMarkdown(const std::string_view source) {
    std::string result;
    for (std::size_t i = 0; i < source.size();) {
        if (source[i] == '`') {
            if (const auto end = source.find('`', i + 1); end != std::string_view::npos) {
                result += "<code>" + EscapeHtml(source.substr(i + 1, end - i - 1)) + "</code>";
                i = end + 1;
                continue;
            }
        }
        if (source.substr(i).starts_with("**")) {
            if (const auto end = source.find("**", i + 2); end != std::string_view::npos) {
                result += "<strong>" + EscapeHtml(source.substr(i + 2, end - i - 2)) + "</strong>";
                i = end + 2;
                continue;
            }
        }
        if (source[i] == '*') {
            if (const auto end = source.find('*', i + 1); end != std::string_view::npos) {
                result += "<em>" + EscapeHtml(source.substr(i + 1, end - i - 1)) + "</em>";
                i = end + 1;
                continue;
            }
        }
        if (source[i] == '[') {
            const auto labelEnd = source.find(']', i + 1);
            if (labelEnd != std::string_view::npos && labelEnd + 1 < source.size() && source[labelEnd + 1] == '(') {
                const auto linkEnd = source.find(')', labelEnd + 2);
                if (linkEnd != std::string_view::npos) {
                    const auto label = source.substr(i + 1, labelEnd - i - 1);
                    const auto link = source.substr(labelEnd + 2, linkEnd - labelEnd - 2);
                    if (SafeLink(link)) {
                        result += "<a href=\"" + EscapeHtml(link) + "\">" + EscapeHtml(label) + "</a>";
                    }
                    else {
                        result += EscapeHtml(label);
                    }
                    i = linkEnd + 1;
                    continue;
                }
            }
        }
        result += EscapeHtml(source.substr(i, 1));
        ++i;
    }
    return result;
}

/// Render a doc comment's block structure: paragraphs, lists, and fenced code.
std::string RenderMarkdown(const std::string_view source) {
    std::istringstream input{std::string(source)};
    std::ostringstream output;
    std::string line;
    std::string paragraph;
    bool list = false;
    bool fence = false;
    auto flushParagraph = [&] {
        if (!paragraph.empty()) {
            output << "<p>" << InlineMarkdown(paragraph) << "</p>";
            paragraph.clear();
        }
    };
    while (std::getline(input, line)) {
        if (line.starts_with("```")) {
            flushParagraph();
            if (list) {
                output << "</ul>";
                list = false;
            }
            output << (fence ? "</code></pre>" : "<pre><code>");
            fence = !fence;
            continue;
        }
        if (fence) {
            output << EscapeHtml(line) << '\n';
            continue;
        }
        if (line.starts_with("- ") || line.starts_with("* ")) {
            flushParagraph();
            if (!list) {
                output << "<ul>";
                list = true;
            }
            output << "<li>" << InlineMarkdown(std::string_view(line).substr(2)) << "</li>";
            continue;
        }
        if (list) {
            output << "</ul>";
            list = false;
        }
        if (line.empty()) {
            flushParagraph();
        }
        else {
            if (!paragraph.empty())
                paragraph += ' ';
            paragraph += line;
        }
    }
    flushParagraph();
    if (list)
        output << "</ul>";
    if (fence)
        output << "</code></pre>";
    return output.str();
}

/// Spell a type the way it was written in source, since documentation should show the reader the syntax they would type
/// rather than an internal normal form.
std::string TypeText(const TypeExpr *type) {
    if (!type)
        return "?";
    if (const auto *named = dynamic_cast<const NamedTypeExpr *>(type)) {
        std::string text = named->name;
        if (!named->typeArgs.empty()) {
            text += '<';
            for (std::size_t i = 0; i < named->typeArgs.size(); ++i) {
                if (i != 0)
                    text += ", ";
                text += TypeText(named->typeArgs[i].get());
            }
            text += '>';
        }
        return text;
    }
    if (const auto *path = dynamic_cast<const PathTypeExpr *>(type)) {
        std::string text;
        for (std::size_t i = 0; i < path->segments.size(); ++i) {
            if (i != 0)
                text += "::";
            text += path->segments[i];
        }
        return text;
    }
    if (const auto *array = dynamic_cast<const ArrayTypeExpr *>(type)) {
        return TypeText(array->element.get()) + (array->size ? "[N]" : "[]");
    }
    if (const auto *pointer = dynamic_cast<const PointerTypeExpr *>(type)) {
        return std::string("*") + (pointer->pointeeMut ? "var " : "") + TypeText(pointer->pointee.get());
    }
    if (const auto *tuple = dynamic_cast<const TupleTypeExpr *>(type)) {
        std::string text = "(";
        for (std::size_t i = 0; i < tuple->elements.size(); ++i) {
            if (i != 0)
                text += ", ";
            text += TypeText(tuple->elements[i].get());
        }
        return text + ")";
    }
    if (dynamic_cast<const SelfTypeExpr *>(type))
        return "self";
    if (const auto *function = dynamic_cast<const FunctionTypeExpr *>(type)) {
        std::string text = "func(";
        for (std::size_t i = 0; i < function->params.size(); ++i) {
            if (i != 0)
                text += ", ";
            text += TypeText(function->params[i].get());
        }
        text += ')';
        if (function->returnType)
            text += " -> " + TypeText(function->returnType->get());
        return text;
    }
    return "?";
}

std::string TypeParams(const std::vector<TypeParameter> &parameters) {
    if (parameters.empty())
        return {};
    std::string text = "<";
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        if (i != 0)
            text += ", ";
        text += parameters[i].name;
        if (!parameters[i].bounds.empty()) {
            text += ": ";
            for (std::size_t bound = 0; bound < parameters[i].bounds.size(); ++bound) {
                if (bound != 0)
                    text += " + ";
                text += TypeText(parameters[i].bounds[bound].get());
            }
        }
    }
    return text + ">";
}

std::string FunctionSignature(const FuncDecl &function) {
    std::string text = function.isPublic ? "pub " : "";
    text += "func " + function.name + TypeParams(function.typeParams) + "(";
    for (std::size_t i = 0; i < function.params.size(); ++i) {
        if (i != 0)
            text += ", ";
        const auto &parameter = function.params[i];
        if (parameter.isVariadic)
            text += "...";
        else
            text += (parameter.isMut ? "var " : "") + parameter.name + ": " + TypeText(parameter.type.get());
    }
    text += ')';
    if (function.returnType)
        text += " -> " + TypeText(function.returnType->get());
    return text;
}

std::string DeclName(const Decl &decl) {
    if (const auto *value = dynamic_cast<const FuncDecl *>(&decl))
        return value->name;
    if (const auto *value = dynamic_cast<const StructDecl *>(&decl))
        return value->name;
    if (const auto *value = dynamic_cast<const EnumDecl *>(&decl))
        return value->name;
    if (const auto *value = dynamic_cast<const UnionDecl *>(&decl))
        return value->name;
    if (const auto *value = dynamic_cast<const InterfaceDecl *>(&decl))
        return value->name;
    if (const auto *value = dynamic_cast<const ModuleDecl *>(&decl))
        return value->name;
    if (const auto *value = dynamic_cast<const ConstDecl *>(&decl))
        return value->name;
    if (const auto *value = dynamic_cast<const TypeAliasDecl *>(&decl))
        return value->name;
    if (const auto *value = dynamic_cast<const ExternFuncDecl *>(&decl))
        return value->name;
    if (const auto *value = dynamic_cast<const ExternVarDecl *>(&decl))
        return value->name;
    if (const auto *value = dynamic_cast<const ImplDecl *>(&decl))
        return "extend-" + value->typeName;
    return {};
}

std::string DeclKind(const Decl &decl) {
    if (dynamic_cast<const FuncDecl *>(&decl))
        return "function";
    if (dynamic_cast<const StructDecl *>(&decl))
        return "struct";
    if (dynamic_cast<const EnumDecl *>(&decl))
        return "enum";
    if (dynamic_cast<const UnionDecl *>(&decl))
        return "union";
    if (dynamic_cast<const InterfaceDecl *>(&decl))
        return "interface";
    if (dynamic_cast<const ModuleDecl *>(&decl))
        return "module";
    if (dynamic_cast<const ConstDecl *>(&decl))
        return "constant";
    if (dynamic_cast<const TypeAliasDecl *>(&decl))
        return "type alias";
    if (dynamic_cast<const ExternFuncDecl *>(&decl) || dynamic_cast<const ExternVarDecl *>(&decl))
        return "extern";
    if (dynamic_cast<const ImplDecl *>(&decl))
        return "extension";
    return "declaration";
}

std::string DeclSignature(const Decl &decl) {
    if (const auto *function = dynamic_cast<const FuncDecl *>(&decl))
        return FunctionSignature(*function);
    if (const auto *value = dynamic_cast<const StructDecl *>(&decl))
        return std::string(value->isPublic ? "pub " : "") + "struct " + value->name + TypeParams(value->typeParams);
    if (const auto *value = dynamic_cast<const EnumDecl *>(&decl))
        return std::string(value->isPublic ? "pub " : "") + "enum " + value->name + TypeParams(value->typeParams);
    if (const auto *value = dynamic_cast<const UnionDecl *>(&decl))
        return std::string(value->isPublic ? "pub " : "") + "union " + value->name;
    if (const auto *value = dynamic_cast<const InterfaceDecl *>(&decl))
        return std::string(value->isPublic ? "pub " : "") + "interface " + value->name;
    if (const auto *value = dynamic_cast<const ModuleDecl *>(&decl))
        return std::string(value->isPublic ? "pub " : "") + "module " + value->name;
    if (const auto *value = dynamic_cast<const ConstDecl *>(&decl))
        return std::string(value->isPublic ? "pub " : "") + "const " + value->name +
               (value->type ? ": " + TypeText(value->type->get()) : "");
    if (const auto *value = dynamic_cast<const TypeAliasDecl *>(&decl))
        return std::string(value->isPublic ? "pub " : "") + "type " + value->name + " = " + TypeText(value->type.get());
    if (const auto *value = dynamic_cast<const ImplDecl *>(&decl))
        return "extend " + value->typeName;
    if (const auto *value = dynamic_cast<const ExternVarDecl *>(&decl))
        return std::string(value->isPublic ? "pub " : "") + "extern " + value->name + ": " +
               TypeText(value->type.get());
    if (const auto *value = dynamic_cast<const ExternFuncDecl *>(&decl)) {
        FuncDecl function;
        function.isPublic = value->isPublic;
        function.name = value->name;
        return "extern " + FunctionSignature(function);
    }
    return DeclName(decl);
}

/// A stable URL fragment for a declaration, so a link into the generated page keeps working across rebuilds.
std::string Anchor(std::string value) {
    for (char &c : value) {
        if (!std::isalnum(static_cast<unsigned char>(c)))
            c = '-';
        else
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

/// Whether a declaration belongs in the generated output, which by default is the package's public surface only.
bool Visible(const Decl &decl, const bool includePrivate) {
    if (includePrivate || decl.isPublic)
        return true;
    if (const auto *extension = dynamic_cast<const ImplDecl *>(&decl)) {
        return std::ranges::any_of(extension->methods, [](const auto &method) { return method->isPublic; });
    }
    return false;
}

void RenderDecl(std::ostringstream &html, const Decl &decl, const std::string &moduleName, const std::string &source,
                const bool includePrivate, std::vector<SearchEntry> &search, const std::string &prefix = {}) {
    if (!Visible(decl, includePrivate))
        return;
    const std::string name = DeclName(decl);
    const std::string qualified = prefix.empty() ? name : prefix + "::" + name;
    const std::string id = Anchor(moduleName + "-" + qualified);
    search.push_back({qualified, DeclKind(decl), moduleName, "#" + id});
    html << "<article class=\"item\" id=\"" << EscapeHtml(id) << "\"><div class=\"kind\">" << EscapeHtml(DeclKind(decl))
         << "</div><h3>" << EscapeHtml(name) << "</h3><pre><code>" << EscapeHtml(DeclSignature(decl))
         << "</code></pre><div class=\"location\">" << EscapeHtml(source) << ':' << decl.location.line << "</div>"
         << RenderMarkdown(decl.documentation);

    if (const auto *structure = dynamic_cast<const StructDecl *>(&decl)) {
        for (const auto &field : structure->fields) {
            if (!includePrivate && !field.isPublic)
                continue;
            html << "<section class=\"member\"><h4>" << EscapeHtml(field.name) << "</h4><code>"
                 << (field.isPublic ? "pub " : "") << EscapeHtml(field.name + ": " + TypeText(field.type.get()))
                 << "</code>" << RenderMarkdown(field.documentation) << "</section>";
        }
    }
    else if (const auto *enumeration = dynamic_cast<const EnumDecl *>(&decl)) {
        for (const auto &variant : enumeration->variants) {
            html << "<section class=\"member\"><h4>" << EscapeHtml(variant.name) << "</h4>"
                 << RenderMarkdown(variant.documentation) << "</section>";
        }
    }
    else if (const auto *interface = dynamic_cast<const InterfaceDecl *>(&decl)) {
        for (const auto &method : interface->methods) {
            html << "<section class=\"member\"><h4>" << EscapeHtml(method->name) << "</h4><code>"
                 << EscapeHtml(FunctionSignature(*method)) << "</code>" << RenderMarkdown(method->documentation)
                 << "</section>";
        }
    }
    else if (const auto *extension = dynamic_cast<const ImplDecl *>(&decl)) {
        for (const auto &method : extension->methods) {
            if (!includePrivate && !method->isPublic)
                continue;
            html << "<section class=\"member\"><h4>" << EscapeHtml(method->name) << "</h4><code>"
                 << EscapeHtml(FunctionSignature(*method)) << "</code>" << RenderMarkdown(method->documentation)
                 << "</section>";
        }
    }
    html << "</article>";
    if (const auto *module = dynamic_cast<const ModuleDecl *>(&decl)) {
        for (const auto &item : module->items) {
            RenderDecl(html, *item, moduleName, source, includePrivate, search, qualified);
        }
    }
}

/// Build an operational diagnostic that carries the system error, so a permission or disk-full failure says what
/// actually went wrong rather than only that generation failed.
Diagnostic FilesystemFailure(std::string message, const std::error_code error, std::optional<std::string> help = {}) {
    return ErrorDiagnostic(std::move(message), {std::format("filesystem error {}: {}", error.value(), error.message())},
                           std::move(help));
}

bool WriteFile(const std::filesystem::path &path, const std::string_view value, Diagnostic &error) {
    errno = 0;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value;
    if (!output) {
        std::error_code writeError(errno, std::generic_category());
        if (!writeError) {
            writeError = std::make_error_code(std::errc::io_error);
        }
        error = FilesystemFailure(std::format("could not write generated documentation file '{}'", path.string()),
                                  writeError, "check that the output path is writable and has enough free space");
        return false;
    }
    return true;
}
} // namespace

bool Generate(const Manifest &manifest, const std::span<const ParseResult> modules, const GenerateOptions &options,
              Diagnostic &error) {
    std::error_code ec;
    const auto output = std::filesystem::absolute(options.outputDirectory, ec).lexically_normal();
    if (ec) {
        error = FilesystemFailure(
            std::format("could not resolve documentation output directory '{}'", options.outputDirectory.string()), ec,
            "choose a valid path with '--output <dir>'");
        return false;
    }
    const bool outputExists = std::filesystem::exists(output, ec);
    if (ec) {
        error = FilesystemFailure(std::format("could not inspect documentation output directory '{}'", output.string()),
                                  ec);
        return false;
    }
    if (outputExists) {
        const bool outputEmpty = std::filesystem::is_empty(output, ec);
        if (ec) {
            error = FilesystemFailure(
                std::format("could not inspect documentation output directory '{}'", output.string()), ec);
            return false;
        }
        const bool managed = outputEmpty || std::filesystem::exists(output / MarkerName, ec);
        if (ec) {
            error = FilesystemFailure(
                std::format("could not inspect documentation marker '{}'", (output / MarkerName).string()), ec);
            return false;
        }
        if (!managed) {
            error =
                ErrorDiagnostic(std::format("refusing to replace non-empty unmarked directory '{}'", output.string()),
                                {}, "choose an empty output directory or remove its contents");
            return false;
        }
    }

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temporary = output.parent_path() / std::format(".rux-docs-tmp-{}", nonce);
    std::filesystem::create_directories(temporary, ec);
    if (ec) {
        error = FilesystemFailure(
            std::format("could not create temporary documentation directory '{}'", temporary.string()), ec,
            "check that the output directory's parent is writable");
        return false;
    }

    std::ostringstream content;
    std::vector<SearchEntry> search;
    for (const auto &module : modules) {
        const std::filesystem::path sourcePath(module.module.name);
        auto relative = std::filesystem::relative(sourcePath, options.packageRoot, ec);
        const std::string source = ec ? sourcePath.filename().generic_string() : relative.generic_string();
        ec.clear();
        const std::string moduleName = sourcePath.stem().string();
        content << "<section class=\"module\"><h2>Module " << EscapeHtml(moduleName) << "</h2>";
        for (const auto &declaration : module.module.items) {
            RenderDecl(content, *declaration, moduleName, source, options.includePrivate, search);
        }
        content << "</section>";
    }
    std::ranges::sort(search, {}, &SearchEntry::name);

    const std::string title = manifest.package.name.Text() + " " + manifest.package.version.Text();
    const std::string html =
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" "
        "content=\"width=device-width,initial-scale=1\"><title>" +
        EscapeHtml(title) +
        " API documentation</title><link rel=\"stylesheet\" href=\"style.css\"></head><body><header><h1>" +
        EscapeHtml(title) + "</h1><p>" + EscapeHtml(manifest.package.description) +
        "</p><input id=\"search\" type=\"search\" placeholder=\"Search public API\" autocomplete=\"off\"><div "
        "id=\"results\"></div></header><main>" +
        content.str() + "</main><script src=\"search.js\"></script></body></html>";
    constexpr std::string_view css =
        ":root{color-scheme:light dark;font:16px/1.55 "
        "system-ui,sans-serif;--bg:#fff;--fg:#18212f;--muted:#667085;--card:#f5f7fa;--line:#d8dee8}"
        "@media(prefers-color-scheme:dark){:root{--bg:#11151c;--fg:#e7ebf2;--muted:#9ca7b8;--card:#1a202b;--line:#"
        "303948}}"
        "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg)}header,main{max-width:1100px;"
        "margin:auto;padding:2rem}"
        "header{border-bottom:1px solid var(--line)}input{width:100%;padding:.75rem;border:1px solid "
        "var(--line);border-radius:.5rem;background:var(--card);color:inherit}"
        ".module{margin:2rem 0}.item{padding:1.25rem;margin:1rem 0;border:1px solid "
        "var(--line);border-radius:.75rem}.kind,.location{color:var(--muted);font-size:.85rem}"
        "pre{overflow:auto;padding:1rem;background:var(--card);border-radius:.5rem}.member{margin:1rem 0 0 "
        "1rem;padding-left:1rem;border-left:3px solid var(--line)}a{color:#4d7cfe}";
    constexpr std::string_view script =
        "fetch('search-index.json').then(r=>r.json()).then(items=>{const "
        "q=document.querySelector('#search'),out=document.querySelector('#results');"
        "q.addEventListener('input',()=>{const "
        "s=q.value.trim().toLowerCase();out.innerHTML=s?items.filter(x=>(x.name+' '+x.kind+' "
        "'+x.module).toLowerCase().includes(s)).slice(0,20)"
        ".map(x=>`<a href=\"${x.href}\">${x.name}</a> <small>${x.kind} in ${x.module}</small>`).join('<br>'):''})})";
    std::ostringstream searchJson;
    searchJson << '[';
    for (std::size_t i = 0; i < search.size(); ++i) {
        if (i != 0)
            searchJson << ',';
        searchJson << "{\"name\":\"" << EscapeJson(search[i].name) << "\",\"kind\":\"" << EscapeJson(search[i].kind)
                   << "\",\"module\":\"" << EscapeJson(search[i].module) << "\",\"href\":\""
                   << EscapeJson(search[i].href) << "\"}";
    }
    searchJson << "]\n";

    bool written = WriteFile(temporary / MarkerName, "rux-docs-v1\n", error) &&
                   WriteFile(temporary / "index.html", html, error) && WriteFile(temporary / "style.css", css, error) &&
                   WriteFile(temporary / "search.js", script, error) &&
                   WriteFile(temporary / "search-index.json", searchJson.str(), error);
    if (!written) {
        std::filesystem::remove_all(temporary, ec);
        return false;
    }
    if (std::filesystem::exists(output, ec)) {
        std::filesystem::remove_all(output, ec);
    }
    if (ec) {
        error =
            FilesystemFailure(std::format("could not replace managed documentation directory '{}'", output.string()),
                              ec, "close programs using the generated documentation and try again");
        return false;
    }
    std::filesystem::rename(temporary, output, ec);
    if (ec) {
        error = FilesystemFailure(std::format("could not install generated documentation at '{}'", output.string()), ec,
                                  "check that the output directory's parent is writable");
        return false;
    }
    return true;
}
} // namespace Rux::Documentation
