// Compile-time evaluation for `#if` conditions: the target, build and compiler
// values a condition can name, and the constants it can compare against.

#include "Numeric/IntegerLiteral.h"
#include "Semantic/ConditionalCompilation.h"
#include "Semantic/ConditionalEvaluatorInternal.h"
#include "Semantic/PrimitiveConstants.h"
#include "Semantic/SemanticVersion.h"
#include "Target/Target.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace Rux {
namespace {
/// The operating systems `#target.os` can name. Each is one a build can produce, so naming one is never a branch that
/// quietly never runs.
constexpr std::array OsVariants{"FreeBSD", "Linux", "macOS", "Windows"};

/// Spellings of an OS that are not the variant name: what the host reports, or what a target triple is called.
constexpr std::pair<std::string_view, std::string_view> OsAliases[] = {
    {"macos", "macOS"},
    {"osx", "macOS"},
    {"darwin", "macOS"},
};

using EnumValue = CompileTimeEnumValue;
using Value = CompileTimeValue;

/// Case-insensitive comparison for the names a `#if` condition can test, so a target or feature written in either case
/// matches.
bool EqualsIgnoringCase(const std::string_view a, const std::string_view b) {
    return std::ranges::equal(a, b, [](const char x, const char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

/// The `OS` variant an OS name denotes, however it is spelled ("macOS" and "Darwin" are both `.macOS`).
std::optional<std::string> OsVariantFor(const std::string_view name) {
    for (const std::string_view variant : OsVariants) {
        if (EqualsIgnoringCase(name, variant)) {
            return std::string(variant);
        }
    }
    for (const auto &[alias, variant] : OsAliases) {
        if (EqualsIgnoringCase(name, alias)) {
            return std::string(variant);
        }
    }
    return std::nullopt;
}

constexpr std::array ArchVariants{"AArch64", "X86_64"};
constexpr std::array AbiVariants{"AAPCS64", "SystemV", "WindowsX64"};
constexpr std::array EndianVariants{"Big", "Little"};
constexpr std::array DataModelVariants{"LLP64", "LP64"};
constexpr std::array ObjectFormatVariants{"COFF", "ELF", "MachO"};
constexpr std::array BuildModeVariants{"Debug", "Release"};
constexpr std::array OptimizationVariants{"None", "Size", "Speed"};
constexpr std::array OutputKindVariants{"Executable", "SharedLibrary", "StaticLibrary", "SourceLibrary"};
constexpr std::array TargetFeatureVariants{"AVX",  "AVX2",  "AVX512", "NEON",  "SSE2",
                                           "SSE3", "SSE41", "SSE42",  "SSSE3", "SVE"};
constexpr std::array CompilerFeatures{
    "conditional-compilation",    "namespaced-intrinsics",    "target-intrinsics",   "build-intrinsics",
    "compiler-feature-detection", "source-location-defaults", "extern-symbol-names", "link-attribute",
    "no-return-attribute"};

template <std::size_t N>
void RegisterVariants(std::unordered_map<std::string, std::vector<std::string>> &enums, const std::string &name,
                      const std::array<const char *, N> &variants) {
    auto &out = enums[name];
    for (const char *variant : variants) {
        out.emplace_back(variant);
    }
}

/// The spelling a compile-time condition matches an architecture by. Distinct from both the canonical target id and the
/// display name, because this one is part of the language's surface and cannot change with report wording.
std::string ArchVariant(const Target::Arch arch) {
    switch (arch) {
    case Target::Arch::AArch64:
        return "AArch64";
    case Target::Arch::X86_64:
        return "X86_64";
    default:
        return "Unknown";
    }
}

std::string AbiVariant(const Target::ABI abi) {
    switch (abi) {
    case Target::ABI::AAPCS64:
        return "AAPCS64";
    case Target::ABI::SystemV:
        return "SystemV";
    case Target::ABI::WindowsX64:
        return "WindowsX64";
    default:
        return "Unknown";
    }
}

std::string DataModelVariant(const Target::DataModel model) {
    switch (model) {
    case Target::DataModel::LLP64:
        return "LLP64";
    case Target::DataModel::LP64:
        return "LP64";
    default:
        return "Unknown";
    }
}

std::string ObjectFormatVariant(const Target::ObjectFormat format) {
    switch (format) {
    case Target::ObjectFormat::COFF:
        return "COFF";
    case Target::ObjectFormat::ELF:
        return "ELF";
    case Target::ObjectFormat::MachO:
        return "MachO";
    default:
        return "Unknown";
    }
}

/// A source path relative to the package root, so a `#if` testing the current file means the same thing wherever the
/// package was checked out.
std::string LogicalFilePath(const std::string &file, const std::filesystem::path &root) {
    const std::filesystem::path path(file);
    if (!root.empty()) {
        const auto relative = path.lexically_relative(root);
        if (!relative.empty() && *relative.begin() != "..") {
            return relative.generic_string();
        }
    }
    return path.generic_string();
}

bool UtcTime(const std::time_t time, std::tm &out) {
#if RUX_OS_WINDOWS
    return gmtime_s(&out, &time) == 0;
#else
    return gmtime_r(&time, &out) != nullptr;
#endif
}

std::string FormatTimestamp(const std::int64_t timestamp, const char *format) {
    const std::time_t value = static_cast<std::time_t>(timestamp);
    std::tm utc{};
    if (!UtcTime(value, utc)) {
        return {};
    }
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), format, &utc) == 0) {
        return {};
    }
    return buffer;
}

/// Parse an integer literal for compile-time evaluation, in every base and suffix the language accepts.
///
/// @return nullopt when the text is not a literal this evaluator can represent
std::optional<Value> ParseIntLiteral(std::string_view text) {
    // Strip a numeric suffix (12u8, 3i64) and the digit separators.
    std::string digits;
    digits.reserve(text.size());
    int base = 10;
    std::size_t i = 0;
    if (text.size() > 2 && text[0] == '0') {
        switch (text[1]) {
        case 'x':
        case 'X':
            base = 16;
            i = 2;
            break;
        case 'b':
        case 'B':
            base = 2;
            i = 2;
            break;
        case 'o':
        case 'O':
            base = 8;
            i = 2;
            break;
        default:
            break;
        }
    }
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '_') {
            continue;
        }
        const bool isDigit =
            (c >= '0' && c <= '9') || (base == 16 && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')));
        if (!isDigit) {
            break; // start of the type suffix
        }
        digits.push_back(c);
    }
    if (digits.empty()) {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    for (const char c : digits) {
        const std::uint64_t digit = (c >= '0' && c <= '9') ? static_cast<std::uint64_t>(c - '0')
                                  : (c >= 'a' && c <= 'f') ? static_cast<std::uint64_t>(c - 'a' + 10)
                                                           : static_cast<std::uint64_t>(c - 'A' + 10);
        if (digit >= static_cast<std::uint64_t>(base)) {
            return std::nullopt;
        }
        const auto limit = std::numeric_limits<std::uint64_t>::max();
        if (value > (limit - digit) / static_cast<std::uint64_t>(base)) {
            return std::nullopt;
        }
        value = value * static_cast<std::uint64_t>(base) + digit;
    }
    const std::string_view suffix = text.substr(i);
    const bool isUnsigned = suffix == "u" || suffix == "u8" || suffix == "u16" || suffix == "u32" || suffix == "u64";
    if (!isUnsigned && value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return Value{static_cast<std::int64_t>(value)};
    }
    return Value{value};
}

std::optional<double> ParseFloatValue(std::string_view text) {
    std::string value;
    value.reserve(text.size());
    for (const char c : text) {
        if (c != '_') {
            value.push_back(c);
        }
    }
    try {
        std::size_t consumed = 0;
        const double result = std::stod(value, &consumed);
        const std::string_view suffix{value.data() + consumed, value.size() - consumed};
        const NumericLiteralSuffixInfo *suffixInfo = FindNumericLiteralSuffix(suffix);
        if (!suffix.empty() && (!suffixInfo || !suffixInfo->isFloat)) {
            return std::nullopt;
        }
        return result;
    }
    catch (...) {
        return std::nullopt;
    }
}

/// The value of a built-in constant such as a type's limit, so a `#if` can compare against it.
std::optional<Value> PrimitiveValue(const PrimitiveConstant &constant) {
    if (constant.type.IsFloat()) {
        if (const auto value = ParseFloatValue(constant.value)) {
            return Value{*value};
        }
        return std::nullopt;
    }

    if (!constant.type.IsInteger() && constant.type.kind != TypeRef::Kind::Char8 &&
        constant.type.kind != TypeRef::Kind::Char16 && constant.type.kind != TypeRef::Kind::Char32) {
        return std::nullopt;
    }

    if (constant.value.starts_with('-')) {
        std::int64_t value = 0;
        const auto [end, error] =
            std::from_chars(constant.value.data(), constant.value.data() + constant.value.size(), value);
        if (error == std::errc{} && end == constant.value.data() + constant.value.size()) {
            return Value{value};
        }
        return std::nullopt;
    }

    std::uint64_t value = 0;
    const auto [end, error] =
        std::from_chars(constant.value.data(), constant.value.data() + constant.value.size(), value);
    if (error != std::errc{} || end != constant.value.data() + constant.value.size()) {
        return std::nullopt;
    }
    if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return Value{static_cast<std::int64_t>(value)};
    }
    return Value{value};
}

} // namespace

ConditionalEvaluator::Impl::Impl(const CompileTimeContext &inputContext, const std::vector<Module *> &modules)
    : context(inputContext) {
    // `OperatingSystem` is built in; the program's own enums are collected
    // from its declarations, so a `when` can compare against those too.
    auto &operatingSystem = enumVariants["OperatingSystem"];
    for (const char *variant : OsVariants) {
        operatingSystem.emplace_back(variant);
    }
    RegisterVariants(enumVariants, "Arch", ArchVariants);
    RegisterVariants(enumVariants, "Architecture", ArchVariants);
    RegisterVariants(enumVariants, "ABI", AbiVariants);
    RegisterVariants(enumVariants, "ApplicationBinaryInterface", AbiVariants);
    RegisterVariants(enumVariants, "Endian", EndianVariants);
    RegisterVariants(enumVariants, "Endianness", EndianVariants);
    RegisterVariants(enumVariants, "DataModel", DataModelVariants);
    RegisterVariants(enumVariants, "ObjectFormat", ObjectFormatVariants);
    RegisterVariants(enumVariants, "BuildMode", BuildModeVariants);
    RegisterVariants(enumVariants, "Optimization", OptimizationVariants);
    RegisterVariants(enumVariants, "OptimizationMode", OptimizationVariants);
    RegisterVariants(enumVariants, "OutputKind", OutputKindVariants);
    // Everything registered so far is a Rux built-in; naming one in a `when`
    // condition requires the file to have imported it from the intrinsics
    // package. The program's own enums, collected later, do not.
    for (const auto &[name, _] : enumVariants) {
        builtinEnumNames.insert(name);
    }
    for (const auto *module : modules) {
        CollectCompileTimeDecls(module->items);
    }
}

void ConditionalEvaluator::Impl::SetSourceContext(const std::string_view file, const std::string_view modulePath,
                                                  const std::string_view function) {
    currentFile = file;
    currentModulePath = modulePath;
    currentFunction = function;
}

void ConditionalEvaluator::Impl::SetImports(const Module &module) {
    SetRuxImportsForModule(module);
}

void ConditionalEvaluator::Impl::RegisterDeclarations(const std::vector<DeclPtr> &decls) {
    CollectCompileTimeDecls(decls);
}

CompileTimeEvaluation ConditionalEvaluator::Impl::Evaluate(const Expr &expr) {
    BeginEvaluation();
    return {Eval(expr), TakeDiagnostics()};
}

CompileTimeEvaluation ConditionalEvaluator::Impl::EvaluateConstant(const std::string_view name) {
    BeginEvaluation();
    const auto found = constExprs.find(std::string(name));
    return {found == constExprs.end() ? std::nullopt : Eval(*found->second), TakeDiagnostics()};
}

CompileTimeConditionEvaluation ConditionalEvaluator::Impl::EvaluateCondition(const Expr *condition,
                                                                             const SourceLocation location) {
    BeginEvaluation();
    const bool value = EvalCondition(condition, location);
    return {value, TakeDiagnostics()};
}

CompileTimeMatchEvaluation
ConditionalEvaluator::Impl::SelectMatchArm(const Expr &subject, const std::vector<std::vector<const Expr *>> &arms,
                                           const SourceLocation location) {
    BeginEvaluation();
    const int arm = SelectMatchArmImpl(subject, arms, location);
    return {arm, TakeDiagnostics()};
}

void ConditionalEvaluator::Impl::RegisterConstant(const ConstDecl &decl) {
    RegisterConstantImpl(decl);
}

void ConditionalEvaluator::Impl::BeginEvaluation() {
    diags.clear();
    constsInProgress.clear();
    reportedError = false;
}

std::vector<Diagnostic> ConditionalEvaluator::Impl::TakeDiagnostics() {
    return std::exchange(diags, {});
}

void ConditionalEvaluator::Impl::EmitError(const SourceLocation location, std::string message) {
    diags.push_back({Diagnostic::Severity::Error, currentFile, location, std::move(message), {}, {}, {}});
}

void ConditionalEvaluator::Impl::EmitWarning(const SourceLocation location, std::string message) {
    diags.push_back({Diagnostic::Severity::Warning, currentFile, location, std::move(message), {}, {}, {}});
}

// What a condition can name: constants and enums

void ConditionalEvaluator::Impl::RegisterConstantImpl(const ConstDecl &decl) {
    if (decl.value) {
        constExprs.emplace(decl.name, decl.value.get());
    }
    if (!decl.type) {
        return;
    }
    const auto *named = dynamic_cast<const NamedTypeExpr *>(decl.type->get());
    if (!named) {
        return;
    }
    if (named->name == "int8")
        constSignedIntegerWidths.emplace(decl.name, 8);
    else if (named->name == "int16")
        constSignedIntegerWidths.emplace(decl.name, 16);
    else if (named->name == "int32")
        constSignedIntegerWidths.emplace(decl.name, 32);
    else if (named->name == "int64" || named->name == "int")
        constSignedIntegerWidths.emplace(decl.name, named->name == "int64" ? 64 : context.target.pointer_size * 8);
    else if (named->name == "uint8")
        constUnsignedIntegerWidths.emplace(decl.name, 8);
    else if (named->name == "uint16")
        constUnsignedIntegerWidths.emplace(decl.name, 16);
    else if (named->name == "uint32")
        constUnsignedIntegerWidths.emplace(decl.name, 32);
    else if (named->name == "uint64" || named->name == "uint")
        constUnsignedIntegerWidths.emplace(decl.name, named->name == "uint64" ? 64 : context.target.pointer_size * 8);
}

void ConditionalEvaluator::Impl::CollectCompileTimeDecls(const std::vector<DeclPtr> &decls) {
    for (const auto &decl : decls) {
        if (!decl) {
            continue;
        }
        if (const auto *constDecl = dynamic_cast<const ConstDecl *>(decl.get())) {
            // An `intrinsic #target: Target;` brings the intrinsic into
            // scope locally, just as importing it from Rux would.
            if (!constDecl->intrinsicName.empty()) {
                localIntrinsics.insert(constDecl->name);
            }
            RegisterConstantImpl(*constDecl);
        }
        else if (const auto *enumDecl = dynamic_cast<const EnumDecl *>(decl.get())) {
            if (enumDecl->form == EnumDecl::Form::Variant) {
                programVariantNames.insert(enumDecl->name);
            }
            else {
                programEnumNames.insert(enumDecl->name);
                auto &variants = enumVariants[enumDecl->name];
                for (const auto &variant : enumDecl->variants) {
                    variants.push_back(variant.name);
                }
            }
        }
        else if (const auto *module = dynamic_cast<const ModuleDecl *>(decl.get())) {
            CollectCompileTimeDecls(module->items);
        }
        // Constants inside an unresolved `when` are registered when its
        // branch is spliced in.
    }
}

/// Record which names the current file imports from the intrinsics package, so a `when` condition can require its build
/// intrinsics and enums to be imported. The import name is whatever the owning manifest bound the package to, so it is
/// matched against the aliases the driver resolved rather than against a fixed spelling.
void ConditionalEvaluator::Impl::SetRuxImportsForModule(const Module &module) {
    ruxImports.clear();
    ruxGlobImport = false;
    CollectRuxImports(module.items);
}

void ConditionalEvaluator::Impl::CollectRuxImports(const std::vector<DeclPtr> &decls) {
    for (const auto &decl : decls) {
        if (!decl) {
            continue;
        }
        if (const auto *use = dynamic_cast<const UseDecl *>(decl.get())) {
            if (use->path.empty() || !context.intrinsicsAliases.contains(use->path.front())) {
                continue;
            }
            if (use->kind == UseDecl::Kind::Glob) {
                ruxGlobImport = true;
            }
            else if (use->kind == UseDecl::Kind::Multi) {
                for (const auto &name : use->names) {
                    ruxImports.insert(name);
                }
            }
            else if (use->path.size() >= 2) {
                ruxImports.insert(use->path.back());
            }
        }
        else if (const auto *module = dynamic_cast<const ModuleDecl *>(decl.get())) {
            CollectRuxImports(module->items);
        }
    }
}

/// A Rux build intrinsic or enum named in a condition must be imported.
bool ConditionalEvaluator::Impl::RequireRuxImport(const std::string &name, const SourceLocation location) {
    if (ruxGlobImport || ruxImports.contains(name) || localIntrinsics.contains(name)) {
        return true;
    }
    EmitError(location, std::format("unknown identifier '{}'", name));
    reportedError = true;
    return false;
}

// Evaluation

std::optional<std::string> ConditionalEvaluator::Impl::IntrinsicArgument(const IntrinsicExpr &expr,
                                                                         const bool allowEnum) {
    if (expr.args.size() != 1 || !expr.args[0]) {
        EmitError(expr.location, "compile-time intrinsic expects exactly one argument");
        reportedError = true;
        return std::nullopt;
    }
    const auto value = Eval(*expr.args[0]);
    if (value) {
        if (const auto *text = std::get_if<std::string>(&*value)) {
            return *text;
        }
        if (allowEnum) {
            if (const auto *enumerator = std::get_if<EnumValue>(&*value)) {
                return enumerator->variant;
            }
        }
    }
    EmitError(expr.args[0]->location, allowEnum ? "compile-time intrinsic argument must be a string or enum member"
                                                : "compile-time intrinsic argument must be a string");
    reportedError = true;
    return std::nullopt;
}

bool ConditionalEvaluator::Impl::TargetHasFeature(const std::string_view name) const {
    const Target::CpuFeatures features = context.target.cpu_features;
    if (name == "SSE2")
        return features.Has(Target::CpuFeature::SSE2);
    if (name == "SSE3")
        return features.Has(Target::CpuFeature::SSE3);
    if (name == "SSSE3")
        return features.Has(Target::CpuFeature::SSSE3);
    if (name == "SSE41")
        return features.Has(Target::CpuFeature::SSE41);
    if (name == "SSE42")
        return features.Has(Target::CpuFeature::SSE42);
    if (name == "AVX")
        return features.Has(Target::CpuFeature::AVX);
    if (name == "AVX2")
        return features.Has(Target::CpuFeature::AVX2);
    if (name == "AVX512")
        return features.Has(Target::CpuFeature::AVX512);
    if (name == "NEON")
        return features.Has(Target::CpuFeature::NEON);
    if (name == "SVE")
        return features.Has(Target::CpuFeature::SVE);
    return false;
}

std::optional<std::string_view> ConditionalEvaluator::Impl::CompilerParamRoot(const Expr &expr) {
    const auto *ident = dynamic_cast<const IdentExpr *>(&expr);
    if (!ident) {
        return std::nullopt;
    }
    if (ident->name == "#target")
        return "Target";
    if (ident->name == "#build")
        return "Build";
    if (ident->name == "#compiler")
        return "Compiler";
    if (ident->name == "#source")
        return "Source";
    if (ident->name == "#config")
        return "Config";
    return std::nullopt;
}

std::optional<CompileTimeValue> ConditionalEvaluator::Impl::EvalCompilerParamField(const std::string_view root,
                                                                                   const std::string_view field,
                                                                                   const SourceLocation location) {
    if (root == "Target") {
        if (field == "os") {
            if (const auto variant = OsVariantFor(ToString(context.target.os)))
                return Value{EnumValue{"OperatingSystem", *variant}};
        }
        if (field == "arch")
            return Value{EnumValue{"Architecture", ArchVariant(context.target.arch)}};
        if (field == "abi")
            return Value{EnumValue{"ApplicationBinaryInterface", AbiVariant(context.target.abi)}};
        if (field == "endian")
            return Value{EnumValue{"Endianness", context.target.endianness == Target::Endian::Big ? "Big" : "Little"}};
        if (field == "pointerBits")
            return Value{static_cast<std::int64_t>(context.target.pointer_size * 8)};
        if (field == "dataModel")
            return Value{EnumValue{"DataModel", DataModelVariant(context.target.data_model)}};
        if (field == "objectFormat")
            return Value{EnumValue{"ObjectFormat", ObjectFormatVariant(context.target.object_format)}};
        if (field == "triple")
            return Value{context.targetTriple};
    }
    if (root == "Build") {
        if (field == "profile")
            return Value{std::string(context.ProfileName())};
        if (field == "mode")
            return Value{
                EnumValue{"BuildMode", context.BuildMode() == Target::BuildMode::Release ? "Release" : "Debug"}};
        if (field == "optimization") {
            const auto optimization = context.Optimization();
            std::string variant = optimization == OptimizationMode::Size  ? "Size"
                                : optimization == OptimizationMode::Speed ? "Speed"
                                                                          : "None";
            return Value{EnumValue{"OptimizationMode", std::move(variant)}};
        }
        if (field == "debugAssertions")
            return Value{context.DebugAssertions()};
        if (field == "debugInfo")
            return Value{context.DebugInfo()};
        if (field == "isTest")
            return Value{context.isTest};
        if (field == "outputKind") {
            std::string variant = context.outputKind == OutputKind::StaticLibrary ? "StaticLibrary"
                                : context.outputKind == OutputKind::SharedLibrary ? "SharedLibrary"
                                : context.outputKind == OutputKind::SourceLibrary ? "SourceLibrary"
                                                                                  : "Executable";
            return Value{EnumValue{"OutputKind", std::move(variant)}};
        }
        if (field == "timestamp")
            return Value{context.buildInfo.Timestamp()};
        if (field == "date")
            return Value{FormatTimestamp(context.buildInfo.Timestamp(), "%Y-%m-%d")};
        if (field == "time")
            return Value{FormatTimestamp(context.buildInfo.Timestamp(), "%H:%M:%S")};
    }
    if (root == "Source") {
        if (field == "line")
            return Value{static_cast<std::int64_t>(location.line)};
        if (field == "column")
            return Value{static_cast<std::int64_t>(location.column)};
        if (field == "file" || field == "fileName")
            return Value{std::filesystem::path(currentFile).filename().generic_string()};
        if (field == "filePath")
            return Value{LogicalFilePath(currentFile, context.sourceRoot)};
        if (field == "function")
            return Value{currentFunction};
        if (field == "module")
            return Value{currentModulePath};
    }
    const std::string_view owner = root == "Target" ? "#target"
                                 : root == "Build"  ? "#build"
                                 : root == "Source" ? "#source"
                                                    : "#compiler";
    EmitError(location, std::format("unknown compile-time field '{}.{}'", owner, field));
    reportedError = true;
    return std::nullopt;
}

std::optional<ParsedSemanticVersion> ConditionalEvaluator::Impl::EvalSemanticVersion(const Expr &expr) {
    if (const auto *ident = dynamic_cast<const IdentExpr *>(&expr)) {
        const auto it = constExprs.find(ident->name);
        if (it == constExprs.end() || !constsInProgress.insert(ident->name).second) {
            return std::nullopt;
        }
        auto value = EvalSemanticVersion(*it->second);
        constsInProgress.erase(ident->name);
        return value;
    }

    if (const auto *field = dynamic_cast<const FieldExpr *>(&expr); field && field->field == "version") {
        if (const auto root = CompilerParamRoot(*field->object); root && *root == "Compiler") {
            const auto *ident = dynamic_cast<const IdentExpr *>(field->object.get());
            if (ident && !RequireRuxImport(ident->name, ident->location)) {
                return std::nullopt;
            }
            return ParseSemanticVersion(context.buildInfo.CompilerVersion());
        }
    }

    if (const auto *call = dynamic_cast<const CallExpr *>(&expr)) {
        const auto *identifier = dynamic_cast<const IdentExpr *>(call->callee.get());
        const bool typeConstructor = identifier && identifier->name == "SemanticVersion";
        if (typeConstructor && call->args.size() == 3) {
            ParsedSemanticVersion version;
            std::uint64_t *components[] = {&version.major, &version.minor, &version.patch};
            for (std::size_t index = 0; index < call->args.size(); ++index) {
                if (!call->args[index])
                    return std::nullopt;
                const auto value = Eval(*call->args[index]);
                if (!value)
                    return std::nullopt;
                if (const auto *unsignedValue = std::get_if<std::uint64_t>(&*value)) {
                    *components[index] = *unsignedValue;
                }
                else if (const auto *signedValue = std::get_if<std::int64_t>(&*value);
                         signedValue && *signedValue >= 0) {
                    *components[index] = static_cast<std::uint64_t>(*signedValue);
                }
                else {
                    return std::nullopt;
                }
            }
            return version;
        }
    }

    const auto *init = dynamic_cast<const StructInitExpr *>(&expr);
    if (!init || init->typeName != "SemanticVersion") {
        return std::nullopt;
    }

    ParsedSemanticVersion version;
    bool hasMajor = false;
    bool hasMinor = false;
    bool hasPatch = false;
    for (const StructInitExpr::Field &field : init->fields) {
        if (!field.value) {
            return std::nullopt;
        }
        const auto value = Eval(*field.value);
        if (!value) {
            return std::nullopt;
        }
        auto assignCore = [&](std::uint64_t &component, bool &present) {
            if (const auto *unsignedValue = std::get_if<std::uint64_t>(&*value)) {
                component = *unsignedValue;
                present = true;
                return true;
            }
            if (const auto *signedValue = std::get_if<std::int64_t>(&*value); signedValue && *signedValue >= 0) {
                component = static_cast<std::uint64_t>(*signedValue);
                present = true;
                return true;
            }
            return false;
        };
        if (field.name == "major") {
            if (!assignCore(version.major, hasMajor))
                return std::nullopt;
        }
        else if (field.name == "minor") {
            if (!assignCore(version.minor, hasMinor))
                return std::nullopt;
        }
        else if (field.name == "patch") {
            if (!assignCore(version.patch, hasPatch))
                return std::nullopt;
        }
    }
    if (!hasMajor || !hasMinor || !hasPatch) {
        return std::nullopt;
    }
    return version;
}

std::optional<CompileTimeValue> ConditionalEvaluator::Impl::EvalCompilerParamCall(const std::string_view root,
                                                                                  const std::string_view member,
                                                                                  const CallExpr &call) {
    if (call.args.size() != 1 || !call.args[0]) {
        EmitError(call.location, "compiler parameter query expects exactly one argument");
        reportedError = true;
        return std::nullopt;
    }
    const auto argument = Eval(*call.args[0]);
    if (!argument) {
        return std::nullopt;
    }
    std::string name;
    if (const auto *text = std::get_if<std::string>(&*argument))
        name = *text;
    else if (const auto *enumerator = std::get_if<EnumValue>(&*argument))
        name = enumerator->variant;
    else
        return std::nullopt;

    // These match the method names the Rux package declares, which are
    // functions and so PascalCase; only the fields are lowerCamelCase.
    if (root == "Target" && member == "HasFeature") {
        if (!std::ranges::contains(TargetFeatureVariants, name)) {
            EmitError(call.location, "unknown target feature '." + name + "'");
            reportedError = true;
            return std::nullopt;
        }
        return Value{TargetHasFeature(name)};
    }
    if (root == "Compiler" && member == "HasFeature")
        return Value{std::ranges::contains(CompilerFeatures, name)};
    if (root == "Config" && member == "Get") {
        const auto it = context.config.find(name);
        return Value{it == context.config.end() ? std::string{} : it->second};
    }
    if (root == "Config" && member == "Has")
        return Value{context.config.contains(name)};
    return std::nullopt;
}

std::optional<CompileTimeValue> ConditionalEvaluator::Impl::Eval(const Expr &expr) {
    if (const auto *e = dynamic_cast<const LiteralExpr *>(&expr)) {
        switch (e->token.kind) {
        case TokenKind::BoolLiteral:
            return Value{e->token.text == "true"};
        case TokenKind::IntLiteral:
            if (const auto value = ParseIntLiteral(e->token.text)) {
                return *value;
            }
            EmitError(
                e->location,
                std::format("compile-time integer literal '{}' is outside the supported 64-bit range", e->token.text));
            reportedError = true;
            return std::nullopt;
        case TokenKind::FloatLiteral:
            if (const auto value = ParseFloatValue(e->token.text)) {
                return Value{*value};
            }
            return std::nullopt;
        case TokenKind::StringLiteral:
            if (auto value = ConditionalEvaluation::ParseStringLiteral(e->token.text)) {
                return Value{*std::move(value)};
            }
            return std::nullopt;
        default:
            return std::nullopt;
        }
    }

    if (const auto *e = dynamic_cast<const IdentExpr *>(&expr)) {
        return EvalConstantReference(*e);
    }

    if (const auto *e = dynamic_cast<const FieldExpr *>(&expr)) {
        if (const auto *version = dynamic_cast<const FieldExpr *>(e->object.get());
            version && version->field == "version") {
            if (const auto root = CompilerParamRoot(*version->object); root && *root == "Compiler") {
                const auto *ident = dynamic_cast<const IdentExpr *>(version->object.get());
                if (ident && !RequireRuxImport(ident->name, ident->location)) {
                    return std::nullopt;
                }
                const ParsedSemanticVersion parsed =
                    ParseSemanticVersion(context.buildInfo.CompilerVersion()).value_or(ParsedSemanticVersion{});
                if (e->field == "major")
                    return Value{parsed.major};
                if (e->field == "minor")
                    return Value{parsed.minor};
                if (e->field == "patch")
                    return Value{parsed.patch};
            }
        }
        if (const auto root = CompilerParamRoot(*e->object)) {
            const auto *ident = dynamic_cast<const IdentExpr *>(e->object.get());
            if (ident && !RequireRuxImport(ident->name, ident->location)) {
                return std::nullopt;
            }
            return EvalCompilerParamField(*root, e->field, e->location);
        }
        return std::nullopt;
    }

    if (const auto *e = dynamic_cast<const CallExpr *>(&expr)) {
        if (const auto *field = dynamic_cast<const FieldExpr *>(e->callee.get())) {
            if (const auto left = EvalSemanticVersion(*field->object)) {
                if (e->args.size() != 1 || !e->args[0]) {
                    EmitError(e->location, "semantic version comparison expects exactly one argument");
                    reportedError = true;
                    return std::nullopt;
                }
                const auto right = EvalSemanticVersion(*e->args[0]);
                if (!right) {
                    return std::nullopt;
                }
                const int comparison = CompareSemanticVersions(*left, *right);
                if (field->field == "Compare")
                    return Value{static_cast<std::int64_t>(comparison)};
                if (field->field == "IsEqualTo")
                    return Value{comparison == 0};
                if (field->field == "IsLessThan")
                    return Value{comparison < 0};
                if (field->field == "IsAtMost")
                    return Value{comparison <= 0};
                if (field->field == "IsGreaterThan")
                    return Value{comparison > 0};
                if (field->field == "IsAtLeast")
                    return Value{comparison >= 0};
            }
            if (const auto root = CompilerParamRoot(*field->object)) {
                const auto *ident = dynamic_cast<const IdentExpr *>(field->object.get());
                if (ident && !RequireRuxImport(ident->name, ident->location)) {
                    return std::nullopt;
                }
                return EvalCompilerParamCall(*root, field->field, *e);
            }
        }
        return std::nullopt;
    }

    if (const auto *e = dynamic_cast<const IntrinsicExpr *>(&expr)) {
        using K = IntrinsicExpr::Kind;
        switch (e->kind) {
        case K::Line:
            return Value{static_cast<std::int64_t>(e->location.line)};
        case K::Column:
            return Value{static_cast<std::int64_t>(e->location.column)};
        case K::File:
        case K::FileName:
            return Value{std::filesystem::path(currentFile).filename().generic_string()};
        case K::FilePath:
            return Value{LogicalFilePath(currentFile, context.sourceRoot)};
        case K::Function:
            return Value{currentFunction};
        case K::Date:
            return Value{FormatTimestamp(context.buildInfo.Timestamp(), "%Y-%m-%d")};
        case K::Time:
            return Value{FormatTimestamp(context.buildInfo.Timestamp(), "%H:%M:%S")};
        case K::Module:
            return Value{currentModulePath};
        case K::CompilerVersion:
            return Value{context.buildInfo.CompilerVersion()};
        case K::Os:
            if (const auto variant = OsVariantFor(ToString(context.target.os))) {
                return Value{EnumValue{"OperatingSystem", *variant}};
            }
            return std::nullopt;
        case K::Arch:
            return Value{EnumValue{"Arch", ArchVariant(context.target.arch)}};
        case K::Abi:
            return Value{EnumValue{"ABI", AbiVariant(context.target.abi)}};
        case K::Endian:
            return Value{EnumValue{"Endian", context.target.endianness == Target::Endian::Big ? "Big" : "Little"}};
        case K::PointerBits:
            return Value{static_cast<std::int64_t>(context.target.pointer_size * 8)};
        case K::DataModel:
            return Value{EnumValue{"DataModel", DataModelVariant(context.target.data_model)}};
        case K::ObjectFormat:
            return Value{EnumValue{"ObjectFormat", ObjectFormatVariant(context.target.object_format)}};
        case K::TargetTriple:
            return Value{context.targetTriple};
        case K::TargetFeature: {
            const auto name = IntrinsicArgument(*e, true);
            if (!name) {
                return std::nullopt;
            }
            if (!std::ranges::contains(TargetFeatureVariants, *name)) {
                EmitError(e->location, "unknown target feature '." + *name + "'");
                reportedError = true;
                return std::nullopt;
            }
            return Value{TargetHasFeature(*name)};
        }
        case K::BuildProfile:
            return Value{std::string(context.ProfileName())};
        case K::BuildMode:
            return Value{
                EnumValue{"BuildMode", context.BuildMode() == Target::BuildMode::Release ? "Release" : "Debug"}};
        case K::Optimization: {
            std::string variant = "None";
            if (context.Optimization() == OptimizationMode::Size)
                variant = "Size";
            else if (context.Optimization() == OptimizationMode::Speed)
                variant = "Speed";
            return Value{EnumValue{"Optimization", std::move(variant)}};
        }
        case K::DebugAssertions:
            return Value{context.DebugAssertions()};
        case K::DebugInfo:
            return Value{context.DebugInfo()};
        case K::IsTest:
            return Value{context.isTest};
        case K::OutputKind: {
            std::string variant = "Executable";
            if (context.outputKind == OutputKind::StaticLibrary)
                variant = "StaticLibrary";
            else if (context.outputKind == OutputKind::SharedLibrary)
                variant = "SharedLibrary";
            else if (context.outputKind == OutputKind::SourceLibrary)
                variant = "SourceLibrary";
            return Value{EnumValue{"OutputKind", std::move(variant)}};
        }
        case K::BuildTimestamp:
            return Value{context.buildInfo.Timestamp()};
        case K::CompilerHasFeature: {
            const auto feature = IntrinsicArgument(*e);
            return feature ? std::optional<Value>{Value{std::ranges::contains(CompilerFeatures, *feature)}}
                           : std::nullopt;
        }
        case K::Config: {
            const auto key = IntrinsicArgument(*e);
            if (!key)
                return std::nullopt;
            const auto it = context.config.find(*key);
            return Value{it == context.config.end() ? std::string{} : it->second};
        }
        case K::HasConfig: {
            const auto key = IntrinsicArgument(*e);
            return key ? std::optional<Value>{Value{context.config.contains(*key)}} : std::nullopt;
        }
        }
    }

    if (const auto *e = dynamic_cast<const EnumShorthandExpr *>(&expr)) {
        // An empty case type marks a shorthand; the comparison then rejects
        // it and asks for the fully-qualified form.
        return Value{EnumValue{"", e->variant}};
    }

    if (const auto *e = dynamic_cast<const PathExpr *>(&expr)) {
        if (e->segments.size() == 2) {
            if (const auto constant = LookupPrimitiveConstant(e->segments[0], e->segments[1], context)) {
                return PrimitiveValue(*constant);
            }
        }
        // The long form of an enum member: OperatingSystem::Windows. A
        // built-in Rux enum must be imported; the program's own need not be.
        if (e->segments.size() == 2) {
            const std::string &enumName = e->segments[0];
            if (programVariantNames.contains(enumName)) {
                EmitError(e->location, std::format("compile-time 'when' cannot evaluate variant type '{}'", enumName));
                reportedError = true;
                return std::nullopt;
            }
            if (builtinEnumNames.contains(enumName) && !programEnumNames.contains(enumName) &&
                !RequireRuxImport(enumName, e->location)) {
                return std::nullopt;
            }
            return Value{EnumValue{e->segments[0], e->segments[1]}};
        }
        return std::nullopt;
    }

    if (const auto *e = dynamic_cast<const UnaryExpr *>(&expr)) {
        const auto operand = Eval(*e->operand);
        if (!operand) {
            return std::nullopt;
        }
        switch (e->op) {
        case TokenKind::Bang:
            if (const auto *b = std::get_if<bool>(&*operand)) {
                return Value{!*b};
            }
            EmitError(e->location, "compile-time operator '!' requires an operand of type 'bool'");
            reportedError = true;
            return std::nullopt;
        case TokenKind::Minus:
            if (const auto *i = std::get_if<std::int64_t>(&*operand)) {
                return Value{static_cast<std::int64_t>(0u - static_cast<std::uint64_t>(*i))};
            }
            if (const auto *u = std::get_if<std::uint64_t>(&*operand); u && *u <= (std::uint64_t{1} << 63)) {
                if (*u == (std::uint64_t{1} << 63)) {
                    return Value{std::numeric_limits<std::int64_t>::min()};
                }
                return Value{-static_cast<std::int64_t>(*u)};
            }
            if (const auto *f = std::get_if<double>(&*operand)) {
                return Value{-*f};
            }
            EmitError(e->location, "compile-time unary operator '-' requires a signed numeric operand");
            reportedError = true;
            return std::nullopt;
        case TokenKind::Plus:
            return operand;
        case TokenKind::Tilde:
            if (const auto *i = std::get_if<std::int64_t>(&*operand)) {
                return Value{~*i};
            }
            if (const auto *u = std::get_if<std::uint64_t>(&*operand)) {
                return Value{~*u};
            }
            EmitError(e->location, "compile-time operator '~' requires an integer operand");
            reportedError = true;
            return std::nullopt;
        default:
            return std::nullopt;
        }
    }

    if (const auto *e = dynamic_cast<const BinaryExpr *>(&expr)) {
        return EvalBinary(*e);
    }

    return std::nullopt;
}

/// Whether two enum values name the same variant. A shorthand such as `.Windows` takes its enum from the value on the
/// other side (`#target.os`, which is `OperatingSystem`), and the variant is validated against it. Returns nullopt when
/// the comparison is ill-formed (an error is reported).
std::optional<bool> ConditionalEvaluator::Impl::EnumEquals(const EnumValue &left, const EnumValue &right,
                                                           const SourceLocation location) {
    if (!left.type.empty() && !right.type.empty() && left.type != right.type) {
        EmitError(location, std::format("cannot compare '{}' with '{}'", left.type, right.type));
        reportedError = true;
        return std::nullopt;
    }
    const std::string &type = left.type.empty() ? right.type : left.type;
    if (type.empty()) {
        return std::nullopt; // two shorthands: nothing says which enum
    }
    const auto variants = enumVariants.find(type);
    if (variants == enumVariants.end()) {
        return std::nullopt;
    }
    for (const auto *side : {&left, &right}) {
        if (std::ranges::find(variants->second, side->variant) == variants->second.end()) {
            EmitError(location, std::format("'.{}' is not a variant of '{}'; the variants are: .{}", side->variant,
                                            type, JoinVariants(variants->second)));
            reportedError = true;
            return std::nullopt;
        }
    }
    return left.variant == right.variant;
}

/// `#target.os == .Windows`. Enum values compare by variant, and only for equality; a shorthand takes its enum from the
/// value on the other side.
std::optional<CompileTimeValue>
ConditionalEvaluator::Impl::EvalEnumComparison(const BinaryExpr &e, const EnumValue &left, const EnumValue &right) {
    if (e.op != TokenKind::Equal && e.op != TokenKind::BangEqual) {
        return std::nullopt;
    }
    const auto equal = EnumEquals(left, right, e.location);
    if (!equal) {
        return std::nullopt;
    }
    return Value{e.op == TokenKind::Equal ? *equal : !*equal};
}

/// Alphabetical, so a reader scanning the list for the name they meant to type can find it. Comparison is
/// case-insensitive so technical initialisms sort with the same rule as other names.
std::string ConditionalEvaluator::Impl::JoinVariants(const std::vector<std::string> &variants) {
    std::vector<std::string> sorted = variants;
    std::ranges::sort(sorted, [](const std::string_view a, const std::string_view b) {
        return std::ranges::lexicographical_compare(a, b, [](const char x, const char y) {
            return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
        });
    });

    std::string joined;
    for (const auto &variant : sorted) {
        if (!joined.empty()) {
            joined += ", .";
        }
        joined += variant;
    }
    return joined;
}

ConditionalEvaluator::ConditionalEvaluator(const CompileTimeContext &context, const std::vector<Module *> &modules)
    : impl(std::make_unique<Impl>(context, modules)) {
}

ConditionalEvaluator::~ConditionalEvaluator() = default;
ConditionalEvaluator::ConditionalEvaluator(ConditionalEvaluator &&) noexcept = default;
ConditionalEvaluator &ConditionalEvaluator::operator=(ConditionalEvaluator &&) noexcept = default;

void ConditionalEvaluator::SetSourceContext(const std::string_view file, const std::string_view modulePath,
                                            const std::string_view function) {
    impl->SetSourceContext(file, modulePath, function);
}

void ConditionalEvaluator::SetImports(const Module &module) {
    impl->SetImports(module);
}

void ConditionalEvaluator::RegisterDeclarations(const std::vector<DeclPtr> &decls) {
    impl->RegisterDeclarations(decls);
}

void ConditionalEvaluator::RegisterConstant(const ConstDecl &decl) {
    impl->RegisterConstant(decl);
}

CompileTimeEvaluation ConditionalEvaluator::Evaluate(const Expr &expr) {
    return impl->Evaluate(expr);
}

CompileTimeEvaluation ConditionalEvaluator::EvaluateConstant(const std::string_view name) {
    return impl->EvaluateConstant(name);
}

CompileTimeConditionEvaluation ConditionalEvaluator::EvaluateCondition(const Expr *condition,
                                                                       const SourceLocation location) {
    return impl->EvaluateCondition(condition, location);
}

CompileTimeMatchEvaluation ConditionalEvaluator::SelectMatchArm(const Expr &subject,
                                                                const std::vector<std::vector<const Expr *>> &arms,
                                                                const SourceLocation location) {
    return impl->SelectMatchArm(subject, arms, location);
}

} // namespace Rux
