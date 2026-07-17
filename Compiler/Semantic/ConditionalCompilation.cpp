#include "Semantic/ConditionalCompilation.h"

#include "Driver/Version.h"
#include "Semantic/PrimitiveConstants.h"
#include "Target/Target.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace Rux {
namespace {
// The operating systems `target.os` can name. Each is a real system, not a
// family, so FreeBSD and OpenBSD remain distinct. `buildable` marks the ones a build can currently produce;
// the rest are accepted so that code can name them, and comparing `target.os` against
// one warns rather than quietly never running.
struct OsVariant {
    std::string_view name;
    bool buildable;
};

constexpr OsVariant OsVariants[] = {
    {"AIX", false},     {"Android", false}, {"DragonFlyBSD", true}, {"FreeBSD", true},
    {"Fuchsia", false}, {"Haiku", false},   {"Illumos", true},      {"iOS", false},
    {"Linux", true},    {"MacOS", true},    {"NetBSD", true},       {"OpenBSD", true},
    {"QNX", false},     {"Redox", false},   {"Solaris", true},      {"Windows", true},
};

// Spellings of an OS that are not the variant name: what the host reports, or
// what a target triple is called.
constexpr std::pair<std::string_view, std::string_view> OsAliases[] = {
    {"macos", "MacOS"},     {"osx", "MacOS"}, {"darwin", "MacOS"}, {"dragonfly", "DragonFlyBSD"},
    {"illumos", "Illumos"}, {"ios", "iOS"},
};

// An enum value. `variant` is the name after the dot; `type` is the enum it
// belongs to, and is empty for a shorthand such as `.Windows` until the other
// side of a comparison says which enum was meant.
struct EnumValue {
    std::string type;
    std::string variant;

    bool operator==(const EnumValue &) const = default;
};

// A compile-time value. `when` conditions must evaluate to a bool; the other
// alternatives exist so that comparisons such as `Version >= 2`, `Name == "x"`
// and `target.os == .Windows` can produce one.
using Value = std::variant<bool, std::int64_t, std::uint64_t, double, std::string, EnumValue>;

bool EqualsIgnoringCase(const std::string_view a, const std::string_view b) {
    return std::ranges::equal(a, b, [](const char x, const char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

// The `OS` variant an OS name denotes, however it is spelled ("macOS", "Darwin"
// and "MacOS" are all `.MacOS`).
std::optional<std::string> OsVariantFor(const std::string_view name) {
    for (const auto &variant : OsVariants) {
        if (EqualsIgnoringCase(name, variant.name)) {
            return std::string(variant.name);
        }
    }
    for (const auto &[alias, variant] : OsAliases) {
        if (EqualsIgnoringCase(name, alias)) {
            return std::string(variant);
        }
    }
    return std::nullopt;
}

bool IsBuildableOs(const std::string_view variant) {
    const auto it = std::ranges::find(OsVariants, variant, &OsVariant::name);
    return it != std::end(OsVariants) && it->buildable;
}

constexpr std::array ArchVariants{"ARM32", "ARM64", "RISCV32", "RISCV64", "X86_32", "X86_64"};
constexpr std::array AbiVariants{"AAPCS",   "AAPCS64",    "RISCV_ILP32", "RISCV_LP64",
                                 "SystemV", "WindowsX64", "WindowsX86"};
constexpr std::array EndianVariants{"Big", "Little"};
constexpr std::array DataModelVariants{"ILP32", "LLP64", "LP64"};
constexpr std::array ObjectFormatVariants{"COFF", "ELF", "MachO", "Wasm"};
constexpr std::array BuildModeVariants{"Debug", "Release"};
constexpr std::array OptimizationVariants{"None", "Size", "Speed"};
constexpr std::array OutputKindVariants{"Executable", "SharedLibrary", "StaticLibrary"};
constexpr std::array TargetFeatureVariants{"AVX",  "AVX2",  "AVX512", "NEON",  "RVV", "SSE2",
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

std::string ArchVariant(const Target::Arch arch) {
    switch (arch) {
    case Target::Arch::X86_32:
        return "X86_32";
    case Target::Arch::X86_64:
        return "X86_64";
    case Target::Arch::ARM32:
        return "ARM32";
    case Target::Arch::ARM64:
        return "ARM64";
    case Target::Arch::RISCV32:
        return "RISCV32";
    case Target::Arch::RISCV64:
        return "RISCV64";
    default:
        return "Unknown";
    }
}

std::string AbiVariant(const Target::ABI abi) {
    switch (abi) {
    case Target::ABI::SystemV:
        return "SystemV";
    case Target::ABI::WindowsX86:
        return "WindowsX86";
    case Target::ABI::WindowsX64:
        return "WindowsX64";
    case Target::ABI::AAPCS:
        return "AAPCS";
    case Target::ABI::AAPCS64:
        return "AAPCS64";
    case Target::ABI::RISCV_ILP32:
        return "RISCV_ILP32";
    case Target::ABI::RISCV_LP64:
        return "RISCV_LP64";
    default:
        return "Unknown";
    }
}

std::string DataModelVariant(const Target::DataModel model) {
    switch (model) {
    case Target::DataModel::ILP32:
        return "ILP32";
    case Target::DataModel::LP64:
        return "LP64";
    case Target::DataModel::LLP64:
        return "LLP64";
    default:
        return "Unknown";
    }
}

std::string ObjectFormatVariant(const Target::ObjectFormat format) {
    switch (format) {
    case Target::ObjectFormat::ELF:
        return "ELF";
    case Target::ObjectFormat::COFF:
        return "COFF";
    case Target::ObjectFormat::MachO:
        return "MachO";
    case Target::ObjectFormat::Wasm:
        return "Wasm";
    default:
        return "Unknown";
    }
}

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
    if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
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
        if (!suffix.empty() && suffix != "f32" && suffix != "f64") {
            return std::nullopt;
        }
        return result;
    }
    catch (...) {
        return std::nullopt;
    }
}

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

// "hello" / c8"hello" -> hello. Only the escapes that can plausibly appear in a
// `when` comparison are decoded; anything else is kept verbatim.
std::optional<std::string> ParseStringLiteral(std::string_view text) {
    const auto open = text.find('"');
    if (open == std::string_view::npos || text.back() != '"' || text.size() < open + 2) {
        return std::nullopt;
    }
    const std::string_view body = text.substr(open + 1, text.size() - open - 2);

    std::string value;
    value.reserve(body.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '\\' || i + 1 == body.size()) {
            value.push_back(body[i]);
            continue;
        }
        switch (body[++i]) {
        case 'n':
            value.push_back('\n');
            break;
        case 't':
            value.push_back('\t');
            break;
        case 'r':
            value.push_back('\r');
            break;
        case '0':
            value.push_back('\0');
            break;
        case '\\':
            value.push_back('\\');
            break;
        case '"':
            value.push_back('"');
            break;
        default:
            value.push_back('\\');
            value.push_back(body[i]);
            break;
        }
    }
    return value;
}

class Resolver {
public:
    Resolver(const CompileTimeContext &inputContext, std::vector<Diagnostic> &inputDiags)
        : context(inputContext)
        , diags(inputDiags) {
        // `OperatingSystem` is built in; the program's own enums are collected
        // from its declarations, so a `when` can compare against those too.
        auto &operatingSystem = enumVariants["OperatingSystem"];
        for (const auto &variant : OsVariants) {
            operatingSystem.emplace_back(variant.name);
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
        // condition requires an explicit `import Rux::{...}` in the file. The
        // program's own enums, collected later, do not.
        for (const auto &[name, _] : enumVariants) {
            builtinEnumNames.insert(name);
        }
    }

    void Run(const std::vector<Module *> &modules) {
        for (const auto *module : modules) {
            CollectCompileTimeDecls(module->items);
        }
        // Declarations first: a `when` branch can define constants that a
        // condition inside a function body then tests.
        for (auto *module : modules) {
            currentFile = module->name;
            currentModulePath = FilePathToModulePath(module->name);
            currentDeclModulePath.clear();
            SetRuxImportsForModule(*module);
            ResolveDecls(module->items);
        }
        for (auto *module : modules) {
            currentFile = module->name;
            currentModulePath = FilePathToModulePath(module->name);
            currentDeclModulePath.clear();
            SetRuxImportsForModule(*module);
            for (const auto &decl : module->items) {
                ResolveDeclBodies(*decl);
            }
        }
    }

private:
    const CompileTimeContext &context;
    std::vector<Diagnostic> &diags;
    std::string currentFile;
    std::string currentFunction;
    std::string currentModulePath;
    std::string currentDeclModulePath;
    std::unordered_map<std::string, const Expr *> constExprs;
    std::unordered_map<std::string, std::vector<std::string>> enumVariants;
    std::unordered_set<std::string> constsInProgress;
    // Rux built-in enum type names, and the program's own enum names. The former
    // must be imported to be named in a condition; the latter never are.
    std::unordered_set<std::string> builtinEnumNames;
    std::unordered_set<std::string> programEnumNames;
    // Intrinsics the program declares itself with `intrinsic`, which count as
    // in scope without an import.
    std::unordered_set<std::string> localIntrinsics;
    // Names imported from the `Rux` package in the file currently being folded.
    std::unordered_set<std::string> ruxImports;
    bool ruxGlobImport = false;
    // Set when evaluation already explained why a condition failed, so the
    // generic "not a compile-time constant" is not piled on top of it.
    bool reportedError = false;

    void EmitError(const SourceLocation location, std::string message) {
        diags.push_back({Diagnostic::Severity::Error, currentFile, location, std::move(message)});
    }

    void EmitWarning(const SourceLocation location, std::string message) {
        diags.push_back({Diagnostic::Severity::Warning, currentFile, location, std::move(message)});
    }

    // What a condition can name: constants and enums

    void CollectCompileTimeDecls(const std::vector<DeclPtr> &decls) {
        for (const auto &decl : decls) {
            if (!decl) {
                continue;
            }
            if (const auto *constDecl = dynamic_cast<const ConstDecl *>(decl.get())) {
                // An `intrinsic const target: Target;` brings the intrinsic into
                // scope locally, just as importing it from Rux would.
                if (!constDecl->intrinsicName.empty()) {
                    localIntrinsics.insert(constDecl->name);
                }
                if (constDecl->value) {
                    constExprs.emplace(constDecl->name, constDecl->value.get());
                }
            }
            else if (const auto *enumDecl = dynamic_cast<const EnumDecl *>(decl.get())) {
                programEnumNames.insert(enumDecl->name);
                auto &variants = enumVariants[enumDecl->name];
                for (const auto &variant : enumDecl->variants) {
                    variants.push_back(variant.name);
                }
            }
            else if (const auto *module = dynamic_cast<const ModuleDecl *>(decl.get())) {
                CollectCompileTimeDecls(module->items);
            }
            // Constants inside an unresolved `when` are registered when its
            // branch is spliced in.
        }
    }

    // Record which names the current file imports from the `Rux` package, so a
    // `when` condition can require its build intrinsics and enums to be imported.
    void SetRuxImportsForModule(const Module &module) {
        ruxImports.clear();
        ruxGlobImport = false;
        CollectRuxImports(module.items);
    }

    void CollectRuxImports(const std::vector<DeclPtr> &decls) {
        for (const auto &decl : decls) {
            if (!decl) {
                continue;
            }
            if (const auto *use = dynamic_cast<const UseDecl *>(decl.get())) {
                if (use->path.empty() || use->path.front() != "Rux") {
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

    // A Rux build intrinsic or enum named in a condition must be imported.
    bool RequireRuxImport(const std::string &name, const SourceLocation location) {
        if (ruxGlobImport || ruxImports.contains(name) || localIntrinsics.contains(name)) {
            return true;
        }
        EmitError(location, std::format("unknown identifier '{}'", name));
        reportedError = true;
        return false;
    }

    // Evaluation

    std::optional<std::string> IntrinsicArgument(const IntrinsicExpr &expr, const bool allowEnum = false) {
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
        EmitError(expr.args[0]->location, allowEnum ? "compile-time intrinsic argument must be a string or enum variant"
                                                    : "compile-time intrinsic argument must be a string");
        reportedError = true;
        return std::nullopt;
    }

    bool TargetHasFeature(const std::string_view name) const {
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
        if (name == "RVV")
            return features.Has(Target::CpuFeature::RVV);
        return false;
    }

    static std::optional<std::string_view> CompilerParamRoot(const Expr &expr) {
        const auto *ident = dynamic_cast<const IdentExpr *>(&expr);
        if (!ident) {
            return std::nullopt;
        }
        if (ident->name == "target")
            return "Target";
        if (ident->name == "build")
            return "Build";
        if (ident->name == "compiler")
            return "Compiler";
        if (ident->name == "source")
            return "Source";
        if (ident->name == "config")
            return "Config";
        return std::nullopt;
    }

    std::optional<Value> EvalCompilerParamField(const std::string_view root, const std::string_view field,
                                                const SourceLocation location) const {
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
                return Value{
                    EnumValue{"Endianness", context.target.endianness == Target::Endian::Big ? "Big" : "Little"}};
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
                return Value{context.profileName};
            if (field == "mode")
                return Value{
                    EnumValue{"BuildMode", context.buildMode == Target::BuildMode::Release ? "Release" : "Debug"}};
            if (field == "optimization") {
                std::string variant = context.optimization == OptimizationMode::Size  ? "Size"
                                    : context.optimization == OptimizationMode::Speed ? "Speed"
                                                                                      : "None";
                return Value{EnumValue{"OptimizationMode", std::move(variant)}};
            }
            if (field == "debugAssertions")
                return Value{context.debugAssertions};
            if (field == "debugInfo")
                return Value{context.debugInfo};
            if (field == "isTest")
                return Value{context.isTest};
            if (field == "outputKind") {
                std::string variant = context.outputKind == OutputKind::StaticLibrary ? "StaticLibrary"
                                    : context.outputKind == OutputKind::SharedLibrary ? "SharedLibrary"
                                                                                      : "Executable";
                return Value{EnumValue{"OutputKind", std::move(variant)}};
            }
            if (field == "timestamp")
                return Value{context.buildTimestamp};
            if (field == "date")
                return Value{FormatTimestamp(context.buildTimestamp, "%Y-%m-%d")};
            if (field == "time")
                return Value{FormatTimestamp(context.buildTimestamp, "%H:%M:%S")};
        }
        if (root == "Compiler" && field == "version")
            return Value{context.compilerVersion};
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
        return std::nullopt;
    }

    std::optional<Value> EvalCompilerParamCall(const std::string_view root, const std::string_view member,
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

    std::optional<Value> Eval(const Expr &expr) {
        if (const auto *e = dynamic_cast<const LiteralExpr *>(&expr)) {
            switch (e->token.kind) {
            case TokenKind::BoolLiteral:
                return Value{e->token.text == "true"};
            case TokenKind::IntLiteral:
                if (const auto value = ParseIntLiteral(e->token.text)) {
                    return *value;
                }
                return std::nullopt;
            case TokenKind::FloatLiteral:
                if (const auto value = ParseFloatValue(e->token.text)) {
                    return Value{*value};
                }
                return std::nullopt;
            case TokenKind::StringLiteral:
                if (auto value = ParseStringLiteral(e->token.text)) {
                    return Value{*std::move(value)};
                }
                return std::nullopt;
            default:
                return std::nullopt;
            }
        }

        if (const auto *e = dynamic_cast<const IdentExpr *>(&expr)) {
            const auto it = constExprs.find(e->name);
            if (it == constExprs.end()) {
                return std::nullopt;
            }
            // A constant defined in terms of itself has no value; refuse to
            // recurse forever.
            if (!constsInProgress.insert(e->name).second) {
                return std::nullopt;
            }
            auto value = Eval(*it->second);
            constsInProgress.erase(e->name);
            return value;
        }

        if (const auto *e = dynamic_cast<const FieldExpr *>(&expr)) {
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
                return Value{FormatTimestamp(context.buildTimestamp, "%Y-%m-%d")};
            case K::Time:
                return Value{FormatTimestamp(context.buildTimestamp, "%H:%M:%S")};
            case K::Module:
                return Value{currentModulePath};
            case K::CompilerVersion:
                return Value{context.compilerVersion};
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
                return Value{context.profileName};
            case K::BuildMode:
                return Value{
                    EnumValue{"BuildMode", context.buildMode == Target::BuildMode::Release ? "Release" : "Debug"}};
            case K::Optimization: {
                std::string variant = "None";
                if (context.optimization == OptimizationMode::Size)
                    variant = "Size";
                else if (context.optimization == OptimizationMode::Speed)
                    variant = "Speed";
                return Value{EnumValue{"Optimization", std::move(variant)}};
            }
            case K::DebugAssertions:
                return Value{context.debugAssertions};
            case K::DebugInfo:
                return Value{context.debugInfo};
            case K::IsTest:
                return Value{context.isTest};
            case K::OutputKind: {
                std::string variant = "Executable";
                if (context.outputKind == OutputKind::StaticLibrary)
                    variant = "StaticLibrary";
                else if (context.outputKind == OutputKind::SharedLibrary)
                    variant = "SharedLibrary";
                return Value{EnumValue{"OutputKind", std::move(variant)}};
            }
            case K::BuildTimestamp:
                return Value{context.buildTimestamp};
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
            // An empty enum type marks a shorthand; the comparison then rejects
            // it and asks for the fully-qualified form.
            return Value{EnumValue{"", e->variant}};
        }

        if (const auto *e = dynamic_cast<const PathExpr *>(&expr)) {
            if (e->segments.size() == 2) {
                if (const auto constant = LookupPrimitiveConstant(e->segments[0], e->segments[1], context)) {
                    return PrimitiveValue(*constant);
                }
            }
            // The long form of an enum variant: OperatingSystem::Windows. A
            // built-in Rux enum must be imported; the program's own need not be.
            if (e->segments.size() == 2) {
                const std::string &enumName = e->segments[0];
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

    // `target.os == .Windows`. Enum values compare by variant, and only for equality;
    // a shorthand takes its enum from the value on the other side.
    std::optional<Value> EvalEnumComparison(const BinaryExpr &e, const EnumValue &left, const EnumValue &right) {
        if (e.op != TokenKind::Equal && e.op != TokenKind::BangEqual) {
            return std::nullopt;
        }
        if (!left.type.empty() && !right.type.empty() && left.type != right.type) {
            EmitError(e.location, std::format("cannot compare '{}' with '{}'", left.type, right.type));
            reportedError = true;
            return std::nullopt;
        }

        const std::string &type = left.type.empty() ? right.type : left.type;
        if (type.empty()) {
            return std::nullopt; // two shorthands: nothing says which enum
        }
        // The enum shorthand is not accepted in a condition; the variant must be
        // written in full, as in `OperatingSystem::Windows`.
        for (const auto *side : {&left, &right}) {
            if (side->type.empty()) {
                EmitError(e.location, std::format("enum shorthand '.{}' is not allowed in a 'when' condition; "
                                                  "write it in full, as in '{}::{}'",
                                                  side->variant, type, side->variant));
                reportedError = true;
                return std::nullopt;
            }
        }
        const auto variants = enumVariants.find(type);
        if (variants == enumVariants.end()) {
            return std::nullopt;
        }
        for (const auto *side : {&left, &right}) {
            if (std::ranges::find(variants->second, side->variant) == variants->second.end()) {
                EmitError(e.location, std::format("'.{}' is not a variant of '{}'; the variants are: .{}",
                                                  side->variant, type, JoinVariants(variants->second)));
                reportedError = true;
                return std::nullopt;
            }
            if (type == "OperatingSystem" && !IsBuildableOs(side->variant)) {
                EmitWarning(e.location, std::format("no build target produces '.{}', so this branch is never taken",
                                                    side->variant));
            }
        }

        const bool equal = left.variant == right.variant;
        return Value{e.op == TokenKind::Equal ? equal : !equal};
    }

    // Alphabetical, so a reader scanning the list for the name they meant to
    // type can find it. Case-insensitive, so `.iOS` sorts with the letter it
    // starts with rather than ahead of every capitalized variant.
    static std::string JoinVariants(const std::vector<std::string> &variants) {
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

    std::optional<Value> EvalBinary(const BinaryExpr &e) {
        const auto left = Eval(*e.left);
        if (!left) {
            return std::nullopt;
        }

        // Short-circuit, so `Debug && DebugLevel > 1` does not require the
        // right-hand side to be evaluable when the left is false.
        if (e.op == TokenKind::AmpAmp || e.op == TokenKind::PipePipe) {
            const auto *lb = std::get_if<bool>(&*left);
            if (!lb) {
                return std::nullopt;
            }
            if (e.op == TokenKind::AmpAmp && !*lb) {
                return Value{false};
            }
            if (e.op == TokenKind::PipePipe && *lb) {
                return Value{true};
            }
            const auto right = Eval(*e.right);
            if (!right || !std::holds_alternative<bool>(*right)) {
                return std::nullopt;
            }
            return right;
        }

        const auto right = Eval(*e.right);
        if (!right) {
            return std::nullopt;
        }

        const bool leftInteger =
            std::holds_alternative<std::int64_t>(*left) || std::holds_alternative<std::uint64_t>(*left);
        const bool rightInteger =
            std::holds_alternative<std::int64_t>(*right) || std::holds_alternative<std::uint64_t>(*right);
        if (leftInteger && rightInteger) {
            const auto equal = [&] {
                if (const auto *l = std::get_if<std::int64_t>(&*left)) {
                    if (const auto *r = std::get_if<std::int64_t>(&*right))
                        return *l == *r;
                    return *l >= 0 && static_cast<std::uint64_t>(*l) == std::get<std::uint64_t>(*right);
                }
                const auto l = std::get<std::uint64_t>(*left);
                if (const auto *r = std::get_if<std::uint64_t>(&*right))
                    return l == *r;
                const auto r = std::get<std::int64_t>(*right);
                return r >= 0 && l == static_cast<std::uint64_t>(r);
            };
            const auto less = [&] {
                if (const auto *l = std::get_if<std::int64_t>(&*left)) {
                    if (const auto *r = std::get_if<std::int64_t>(&*right))
                        return *l < *r;
                    return *l < 0 || static_cast<std::uint64_t>(*l) < std::get<std::uint64_t>(*right);
                }
                const auto l = std::get<std::uint64_t>(*left);
                if (const auto *r = std::get_if<std::uint64_t>(&*right))
                    return l < *r;
                const auto r = std::get<std::int64_t>(*right);
                return r >= 0 && l < static_cast<std::uint64_t>(r);
            };

            switch (e.op) {
            case TokenKind::Equal:
                return Value{equal()};
            case TokenKind::BangEqual:
                return Value{!equal()};
            case TokenKind::Less:
                return Value{less()};
            case TokenKind::LessEqual:
                return Value{less() || equal()};
            case TokenKind::Greater:
                return Value{!less() && !equal()};
            case TokenKind::GreaterEqual:
                return Value{!less()};
            default:
                break;
            }

            if (const auto *l = std::get_if<std::int64_t>(&*left)) {
                const auto *r = std::get_if<std::int64_t>(&*right);
                if (!r) {
                    return std::nullopt;
                }
                const auto lu = static_cast<std::uint64_t>(*l);
                const auto ru = static_cast<std::uint64_t>(*r);
                switch (e.op) {
                case TokenKind::Plus:
                    return Value{static_cast<std::int64_t>(lu + ru)};
                case TokenKind::Minus:
                    return Value{static_cast<std::int64_t>(lu - ru)};
                case TokenKind::Star:
                    return Value{static_cast<std::int64_t>(lu * ru)};
                case TokenKind::Slash:
                case TokenKind::Percent:
                    if (*r == 0 || (*l == std::numeric_limits<std::int64_t>::min() && *r == -1))
                        return std::nullopt;
                    return Value{e.op == TokenKind::Slash ? *l / *r : *l % *r};
                case TokenKind::Amp:
                    return Value{*l & *r};
                case TokenKind::Pipe:
                    return Value{*l | *r};
                case TokenKind::Caret:
                    return Value{*l ^ *r};
                case TokenKind::LessLess:
                case TokenKind::GreaterGreater:
                    if (*r < 0 || *r >= 64)
                        return std::nullopt;
                    return Value{e.op == TokenKind::LessLess
                                     ? static_cast<std::int64_t>(lu << static_cast<std::uint64_t>(*r))
                                     : *l >> *r};
                default:
                    return std::nullopt;
                }
            }

            const auto l = std::get<std::uint64_t>(*left);
            const auto *r = std::get_if<std::uint64_t>(&*right);
            if (!r) {
                return std::nullopt;
            }
            switch (e.op) {
            case TokenKind::Plus:
                return Value{l + *r};
            case TokenKind::Minus:
                return Value{l - *r};
            case TokenKind::Star:
                return Value{l * *r};
            case TokenKind::Slash:
            case TokenKind::Percent:
                if (*r == 0)
                    return std::nullopt;
                return Value{e.op == TokenKind::Slash ? l / *r : l % *r};
            case TokenKind::Amp:
                return Value{l & *r};
            case TokenKind::Pipe:
                return Value{l | *r};
            case TokenKind::Caret:
                return Value{l ^ *r};
            case TokenKind::LessLess:
            case TokenKind::GreaterGreater:
                if (*r >= 64)
                    return std::nullopt;
                return Value{e.op == TokenKind::LessLess ? l << *r : l >> *r};
            default:
                return std::nullopt;
            }
        }

        if (left->index() != right->index()) {
            return std::nullopt;
        }

        if (const auto *le = std::get_if<EnumValue>(&*left)) {
            return EvalEnumComparison(e, *le, std::get<EnumValue>(*right));
        }

        if (e.op == TokenKind::Equal) {
            return Value{*left == *right};
        }
        if (e.op == TokenKind::BangEqual) {
            return Value{*left != *right};
        }

        if (const auto *ls = std::get_if<std::string>(&*left)) {
            const auto &rs = std::get<std::string>(*right);
            switch (e.op) {
            case TokenKind::Less:
                return Value{*ls < rs};
            case TokenKind::LessEqual:
                return Value{*ls <= rs};
            case TokenKind::Greater:
                return Value{*ls > rs};
            case TokenKind::GreaterEqual:
                return Value{*ls >= rs};
            default:
                return std::nullopt;
            }
        }

        if (const auto *lb = std::get_if<bool>(&*left)) {
            const bool rb = std::get<bool>(*right);
            switch (e.op) {
            case TokenKind::Amp:
                return Value{*lb && rb};
            case TokenKind::Pipe:
                return Value{*lb || rb};
            case TokenKind::Caret:
                return Value{*lb != rb};
            default:
                return std::nullopt;
            }
        }

        if (const auto *lf = std::get_if<double>(&*left)) {
            const double rf = std::get<double>(*right);
            switch (e.op) {
            case TokenKind::Less:
                return Value{*lf < rf};
            case TokenKind::LessEqual:
                return Value{*lf <= rf};
            case TokenKind::Greater:
                return Value{*lf > rf};
            case TokenKind::GreaterEqual:
                return Value{*lf >= rf};
            case TokenKind::Plus:
                return Value{*lf + rf};
            case TokenKind::Minus:
                return Value{*lf - rf};
            case TokenKind::Star:
                return Value{*lf * rf};
            case TokenKind::Slash:
                return Value{*lf / rf};
            default:
                return std::nullopt;
            }
        }

        return std::nullopt;
    }

    // Evaluates a conditional-compilation condition, reporting why it cannot
    // be used if it fails.
    bool EvalCondition(const Expr *condition, const SourceLocation location) {
        if (!condition) {
            return false;
        }
        reportedError = false;
        const auto value = Eval(*condition);
        if (!value) {
            if (!reportedError) {
                EmitError(location, "'when' condition must be a compile-time constant expression");
            }
            return false;
        }
        if (const auto *b = std::get_if<bool>(&*value)) {
            return *b;
        }
        EmitError(location, "'when' condition must be of type 'bool'");
        return false;
    }

    // Declaration-level `when`

    void ResolveDecls(std::vector<DeclPtr> &decls) {
        std::vector<DeclPtr> resolved;
        resolved.reserve(decls.size());

        for (auto &decl : decls) {
            if (!decl) {
                continue;
            }
            auto *when = dynamic_cast<WhenDecl *>(decl.get());
            if (!when) {
                if (auto *module = dynamic_cast<ModuleDecl *>(decl.get())) {
                    const std::string savedModule = currentModulePath;
                    const std::string savedDeclModule = currentDeclModulePath;
                    currentModulePath =
                        currentModulePath.empty() ? module->name : currentModulePath + "::" + module->name;
                    currentDeclModulePath =
                        currentDeclModulePath.empty() ? module->name : currentDeclModulePath + "::" + module->name;
                    ResolveDecls(module->items);
                    currentModulePath = savedModule;
                    currentDeclModulePath = savedDeclModule;
                }
                else if (auto *impl = dynamic_cast<ImplDecl *>(decl.get())) {
                    ResolveImplConditionals(*impl);
                }
                resolved.push_back(std::move(decl));
                continue;
            }

            for (auto &branch : when->branches) {
                // A branch with no condition is the trailing `else`.
                if (branch.condition && !EvalCondition(branch.condition.get(), when->location)) {
                    continue;
                }
                ResolveDecls(branch.items);
                CollectCompileTimeDecls(branch.items);
                for (auto &item : branch.items) {
                    resolved.push_back(std::move(item));
                }
                break;
            }
        }

        decls = std::move(resolved);
    }

    // An `extend` body holds methods, not a declaration list, so the `when`
    // chains written between them are folded separately: the methods of the
    // taken branch join the ones written unconditionally.
    void ResolveImplConditionals(ImplDecl &impl) {
        for (auto &conditional : impl.conditionals) {
            if (!conditional) {
                continue;
            }
            for (auto &branch : conditional->branches) {
                if (branch.condition && !EvalCondition(branch.condition.get(), conditional->location)) {
                    continue;
                }
                ResolveDecls(branch.items); // a nested `when` resolves first
                for (auto &item : branch.items) {
                    if (!item) {
                        continue;
                    }
                    auto *method = dynamic_cast<FuncDecl *>(item.get());
                    if (!method) {
                        EmitError(item->location, "only methods can be declared inside an 'extend' block");
                        continue;
                    }
                    item.release();
                    impl.methods.emplace_back(method);
                }
                break;
            }
        }
        impl.conditionals.clear();
    }

    // Statement-level `when`

    void ResolveDeclBodies(Decl &decl) {
        if (auto *func = dynamic_cast<FuncDecl *>(&decl)) {
            const std::string savedFunction = currentFunction;
            currentFunction = currentDeclModulePath.empty() ? func->name : currentDeclModulePath + "::" + func->name;
            if (func->body) {
                ResolveBlock(*func->body);
            }
            currentFunction = savedFunction;
        }
        else if (auto *impl = dynamic_cast<ImplDecl *>(&decl)) {
            for (const auto &method : impl->methods) {
                if (method && method->body) {
                    const std::string savedFunction = currentFunction;
                    currentFunction = impl->typeName + "::" + method->name;
                    ResolveBlock(*method->body);
                    currentFunction = savedFunction;
                }
            }
        }
        else if (auto *module = dynamic_cast<ModuleDecl *>(&decl)) {
            const std::string savedModule = currentModulePath;
            const std::string savedDeclModule = currentDeclModulePath;
            currentModulePath = currentModulePath.empty() ? module->name : currentModulePath + "::" + module->name;
            currentDeclModulePath =
                currentDeclModulePath.empty() ? module->name : currentDeclModulePath + "::" + module->name;
            for (const auto &item : module->items) {
                if (item) {
                    ResolveDeclBodies(*item);
                }
            }
            currentModulePath = savedModule;
            currentDeclModulePath = savedDeclModule;
        }
    }

    void ResolveBlock(Block &block) {
        std::vector<StmtPtr> resolved;
        resolved.reserve(block.stmts.size());
        for (auto &stmt : block.stmts) {
            if (stmt) {
                ResolveStmt(std::move(stmt), resolved);
            }
        }
        block.stmts = std::move(resolved);
    }

    // Appends `stmt` to `out`, or — for a `when` — the statements of its taken
    // branch, which are spliced into the enclosing block rather than nested in
    // one, so a `when` introduces no scope of its own.
    void ResolveStmt(StmtPtr stmt, std::vector<StmtPtr> &out) {
        auto *ifStmt = dynamic_cast<IfStmt *>(stmt.get());
        if (ifStmt && ifStmt->isCompileTime) {
            Block *taken = nullptr;
            if (EvalCondition(ifStmt->condition.get(), ifStmt->location)) {
                taken = ifStmt->thenBlock.get();
            }
            else {
                for (auto &elseIf : ifStmt->elseIfs) {
                    if (EvalCondition(elseIf.condition.get(), elseIf.location)) {
                        taken = elseIf.block.get();
                        break;
                    }
                }
                if (!taken) {
                    taken = ifStmt->elseBlock.get();
                }
            }
            if (taken) {
                for (auto &inner : taken->stmts) {
                    if (inner) {
                        ResolveStmt(std::move(inner), out);
                    }
                }
            }
            return;
        }

        ResolveNestedBlocks(*stmt);
        out.push_back(std::move(stmt));
    }

    void ResolveNestedBlocks(Stmt &stmt) {
        if (auto *ifStmt = dynamic_cast<IfStmt *>(&stmt)) {
            ResolveExpr(ifStmt->condition.get());
            if (ifStmt->thenBlock) {
                ResolveBlock(*ifStmt->thenBlock);
            }
            for (auto &elseIf : ifStmt->elseIfs) {
                ResolveExpr(elseIf.condition.get());
                if (elseIf.block) {
                    ResolveBlock(*elseIf.block);
                }
            }
            if (ifStmt->elseBlock) {
                ResolveBlock(*ifStmt->elseBlock);
            }
        }
        else if (auto *whileStmt = dynamic_cast<WhileStmt *>(&stmt)) {
            ResolveExpr(whileStmt->condition.get());
            if (whileStmt->body) {
                ResolveBlock(*whileStmt->body);
            }
        }
        else if (auto *doWhileStmt = dynamic_cast<DoWhileStmt *>(&stmt)) {
            if (doWhileStmt->body) {
                ResolveBlock(*doWhileStmt->body);
            }
            ResolveExpr(doWhileStmt->condition.get());
        }
        else if (auto *loopStmt = dynamic_cast<LoopStmt *>(&stmt)) {
            if (loopStmt->body) {
                ResolveBlock(*loopStmt->body);
            }
        }
        else if (auto *forStmt = dynamic_cast<ForStmt *>(&stmt)) {
            ResolveExpr(forStmt->iterable.get());
            if (forStmt->body) {
                ResolveBlock(*forStmt->body);
            }
        }
        else if (auto *matchStmt = dynamic_cast<MatchStmt *>(&stmt)) {
            ResolveExpr(matchStmt->subject.get());
            for (auto &arm : matchStmt->arms) {
                ResolveExpr(arm.body.get());
            }
        }
        else if (auto *letStmt = dynamic_cast<LetStmt *>(&stmt)) {
            ResolveExpr(letStmt->init.get());
        }
        else if (auto *returnStmt = dynamic_cast<ReturnStmt *>(&stmt)) {
            if (returnStmt->value) {
                ResolveExpr(returnStmt->value->get());
            }
        }
        else if (auto *exprStmt = dynamic_cast<ExprStmt *>(&stmt)) {
            ResolveExpr(exprStmt->expr.get());
        }
        else if (auto *declStmt = dynamic_cast<DeclStmt *>(&stmt)) {
            if (declStmt->decl) {
                if (const auto *constDecl = dynamic_cast<const ConstDecl *>(declStmt->decl.get())) {
                    if (constDecl->value) {
                        constExprs.emplace(constDecl->name, constDecl->value.get());
                    }
                }
                ResolveDeclBodies(*declStmt->decl);
            }
        }
    }

    // Blocks can hide inside expressions (a match arm body), so expressions are
    // walked too.
    void ResolveExpr(Expr *expr) {
        if (!expr) {
            return;
        }
        if (auto *blockExpr = dynamic_cast<BlockExpr *>(expr)) {
            if (blockExpr->block) {
                ResolveBlock(*blockExpr->block);
            }
        }
        else if (auto *matchExpr = dynamic_cast<MatchExpr *>(expr)) {
            ResolveExpr(matchExpr->subject.get());
            for (auto &arm : matchExpr->arms) {
                ResolveExpr(arm.body.get());
            }
        }
        else if (auto *binary = dynamic_cast<BinaryExpr *>(expr)) {
            ResolveExpr(binary->left.get());
            ResolveExpr(binary->right.get());
        }
        else if (auto *assign = dynamic_cast<AssignExpr *>(expr)) {
            ResolveExpr(assign->target.get());
            ResolveExpr(assign->value.get());
        }
        else if (auto *unary = dynamic_cast<UnaryExpr *>(expr)) {
            ResolveExpr(unary->operand.get());
        }
        else if (auto *ternary = dynamic_cast<TernaryExpr *>(expr)) {
            ResolveExpr(ternary->condition.get());
            ResolveExpr(ternary->thenExpr.get());
            ResolveExpr(ternary->elseExpr.get());
        }
        else if (auto *call = dynamic_cast<CallExpr *>(expr)) {
            ResolveExpr(call->callee.get());
            for (auto &arg : call->args) {
                ResolveExpr(arg.get());
            }
        }
        else if (auto *index = dynamic_cast<IndexExpr *>(expr)) {
            ResolveExpr(index->object.get());
            ResolveExpr(index->index.get());
        }
        else if (auto *field = dynamic_cast<FieldExpr *>(expr)) {
            ResolveExpr(field->object.get());
        }
        else if (auto *cast = dynamic_cast<CastExpr *>(expr)) {
            ResolveExpr(cast->operand.get());
        }
        else if (auto *intrinsic = dynamic_cast<IntrinsicExpr *>(expr)) {
            for (auto &arg : intrinsic->args) {
                ResolveExpr(arg.get());
            }
        }
    }
};
} // namespace

void ResolveConditionalCompilation(const std::vector<Module *> &modules, const CompileTimeContext &context,
                                   std::vector<Diagnostic> &diags) {
    Resolver resolver(context, diags);
    resolver.Run(modules);
}

void ResolveConditionalCompilation(const std::vector<Module *> &modules, const std::string_view targetSystem,
                                   std::vector<Diagnostic> &diags) {
    CompileTimeContext context;
    if (const auto variant = OsVariantFor(targetSystem.empty() ? ToString(Target::HostOS) : targetSystem)) {
        for (int value = static_cast<int>(Target::OS::Unknown); value <= static_cast<int>(Target::OS::Windows);
             ++value) {
            const auto os = static_cast<Target::OS>(value);
            if (const auto candidate = OsVariantFor(ToString(os)); candidate && *candidate == *variant) {
                context.target.os = os;
                context.target.object_format = Target::GetObjectFormat(os);
                break;
            }
        }
    }
    context.compilerVersion = RUX_VERSION;
    Resolver resolver(context, diags);
    resolver.Run(modules);
}
} // namespace Rux
