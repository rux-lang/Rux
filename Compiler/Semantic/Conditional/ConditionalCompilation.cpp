// The conditional-compilation entry point: evaluates every `#if` and folds each
// to its taken branch before any name is resolved.

#include "Semantic/Conditional/ConditionalCompilation.h"

#include "Semantic/Conditional/ConditionalFolding.h"
#include "Target/Target.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Rux {
namespace ConditionalFolding {
std::string FilePathToModulePath(const std::string &filePath) {
    std::filesystem::path path(filePath);
    std::vector<std::string> parts;
    for (const auto &part : path) {
        parts.push_back(part.generic_string());
    }
    std::size_t start = parts.size() > 1 ? parts.size() - 1 : 0;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (parts[i] == "Src" || parts[i] == "src") {
            start = i + 1;
        }
    }
    if (!parts.empty()) {
        parts.back() = std::filesystem::path(parts.back()).stem().generic_string();
    }
    std::string result;
    for (std::size_t i = start; i < parts.size(); ++i) {
        if (!result.empty()) {
            result += "::";
        }
        result += parts[i];
    }
    return result;
}
} // namespace ConditionalFolding

namespace {
bool EqualsIgnoringCase(const std::string_view left, const std::string_view right) {
    return std::ranges::equal(left, right, [](const char x, const char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

/// Map a target system name written in source to the target model.
///
/// @return nullopt for a name this compiler does not know, which the caller reports rather than treating as false
std::optional<Target::OS> ParseTargetSystem(const std::string_view name) {
    if (name.empty()) {
        return Target::HostOS;
    }
    if (EqualsIgnoringCase(name, "FreeBSD")) {
        return Target::OS::FreeBSD;
    }
    if (EqualsIgnoringCase(name, "Linux")) {
        return Target::OS::Linux;
    }
    if (EqualsIgnoringCase(name, "MacOS") || EqualsIgnoringCase(name, "osx") || EqualsIgnoringCase(name, "darwin")) {
        return Target::OS::MacOS;
    }
    if (EqualsIgnoringCase(name, "Windows")) {
        return Target::OS::Windows;
    }
    return std::nullopt;
}

/// Evaluate every `#if` in the given modules and fold each to its taken branch, rewriting the AST in place so later
/// stages only ever see one version of the program.
void Resolve(const std::vector<Module *> &modules, const CompileTimeContext &context, std::vector<Diagnostic> &diags,
             ConditionalImportResolver imports = {}) {
    ConditionalEvaluator evaluator(context, modules, std::move(imports));
    ConditionalFolding::FoldDeclarations(modules, evaluator, diags);
    ConditionalFolding::FoldStatements(modules, evaluator, diags);
}
} // namespace

void ResolveConditionalCompilation(const std::vector<Module *> &modules, const CompileTimeContext &context,
                                   std::vector<Diagnostic> &diags, ConditionalImportResolver imports) {
    Resolve(modules, context, diags, std::move(imports));
}

void ResolveConditionalCompilation(const std::vector<Module *> &modules, const std::string_view targetSystem,
                                   std::vector<Diagnostic> &diags) {
    CompileTimeContext context;
    if (const auto os = ParseTargetSystem(targetSystem)) {
        context.target.os = *os;
        context.target.object_format = Target::GetObjectFormat(*os);
    }
    Resolve(modules, context, diags);
}
} // namespace Rux
