#include "Lowering/AstToHir/Detail/AstToHirContext.h"
#include "Semantic/SemanticVersion.h"
#include "Target/Platform.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string_view>

namespace Rux::AstToHirDetail {
namespace {
HirExprPtr CompilerLiteral(const SourceLocation location, TypeRef type, std::string value) {
    auto literal = std::make_unique<HirLiteralExpr>();
    literal->location = location;
    literal->type = std::move(type);
    literal->value = std::move(value);
    return literal;
}

HirExprPtr CompilerString(const SourceLocation location, std::string value) {
    return CompilerLiteral(location, TypeRef::MakeNamed("Slice<char8>"), std::move(value));
}

bool UtcTime(const std::time_t time, std::tm &out) {
#if RUX_OS_WINDOWS
    return gmtime_s(&out, &time) == 0;
#else
    return gmtime_r(&time, &out) != nullptr;
#endif
}

std::string FormatBuildTime(const CompileTimeContext &context, const char *format) {
    const std::time_t value = static_cast<std::time_t>(context.buildInfo.Timestamp());
    std::tm utc{};
    if (!UtcTime(value, utc)) {
        return {};
    }
    char buffer[32]{};
    return std::strftime(buffer, sizeof(buffer), format, &utc) == 0 ? std::string{} : std::string(buffer);
}

bool TargetHasFeature(const CompileTimeContext &context, const std::string_view name) {
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

bool CompilerHasFeature(const std::string_view feature) {
    static constexpr std::array features{
        "conditional-compilation", "namespaced-intrinsics",      "target-intrinsics",
        "build-intrinsics",        "compiler-feature-detection", "source-location-defaults",
        "extern-symbol-names",     "no-return-attribute",        "when-attribute",
        "link-attribute"};
    return std::ranges::contains(features, feature);
}
} // namespace

std::uint64_t AstToHirContext::ResolvedSizeOf(const SizeOfExpr &expression) {
    if (const std::uint64_t *value = model.TryGetSizeOfValue(expression)) {
        return *value;
    }

    const TypeRef type = ResolveTypeWithSubstitution(*expression.type, currentSubstitutions);
    if (type.kind == TypeRef::Kind::TypeParam) {
        assert(currentSubstitutions.empty());
        return 0;
    }
    const ResolvedTypeLayout *layout = model.TryGetLayout(type);
    assert(layout != nullptr && "accepted sizeof expression is missing a resolved layout");
    if (!layout) {
        std::abort();
    }
    return layout->size;
}

std::string AstToHirContext::GenericStructInitName(const StructInitExpr &expression) {
    std::string name = expression.typeName;
    if (!expression.typeArgs.empty()) {
        name += "<";
        for (std::size_t i = 0; i < expression.typeArgs.size(); ++i) {
            if (i) {
                name += ", ";
            }
            name += ResolveType(*expression.typeArgs[i]).ToString();
        }
        name += ">";
    }
    return name;
}

std::pair<const EnumDecl *, const EnumDecl::Variant *>
AstToHirContext::LookupEnumVariantInitializer(const std::string &typeName) const {
    const std::size_t separator = typeName.find("::");
    if (separator == std::string::npos || typeName.find("::", separator + 2) != std::string::npos) {
        return {nullptr, nullptr};
    }

    const auto declaration = enumDecls.find(typeName.substr(0, separator));
    if (declaration == enumDecls.end()) {
        return {nullptr, nullptr};
    }
    const std::string variantName = typeName.substr(separator + 2);
    for (const auto &variant : declaration->second->variants) {
        if (variant.name == variantName) {
            return {declaration->second, &variant};
        }
    }
    return {declaration->second, nullptr};
}

HirExprPtr AstToHirContext::LowerCompilerParamField(const std::string &root, const std::string &field,
                                                    const SourceLocation location) {
    auto compilerEnum = [&](const std::string &typeName, const std::int64_t value) {
        TypeRef type = TypeRef::MakeNamed(typeName);
        if (const auto declaration = enumDecls.find(typeName); declaration != enumDecls.end()) {
            type = EnumType(*declaration->second);
        }
        return CompilerLiteral(location, std::move(type), std::to_string(value));
    };

    if (root == "Target") {
        if (field == "os")
            return compilerEnum("OperatingSystem", static_cast<std::int64_t>(context.target.os));
        if (field == "arch")
            return compilerEnum("Architecture", static_cast<std::int64_t>(context.target.arch));
        if (field == "abi")
            return compilerEnum("ApplicationBinaryInterface", static_cast<std::int64_t>(context.target.abi));
        if (field == "endian")
            return compilerEnum("Endianness", static_cast<std::int64_t>(context.target.endianness));
        if (field == "pointerBits")
            return CompilerLiteral(location, TypeRef::MakeUInt(), std::to_string(context.target.pointer_size * 8));
        if (field == "dataModel")
            return compilerEnum("DataModel", static_cast<std::int64_t>(context.target.data_model));
        if (field == "objectFormat")
            return compilerEnum("ObjectFormat", static_cast<std::int64_t>(context.target.object_format));
        if (field == "triple")
            return CompilerString(location, context.targetTriple);
    }
    if (root == "Build") {
        if (field == "profile")
            return CompilerString(location, std::string(context.ProfileName()));
        if (field == "mode")
            return compilerEnum("BuildMode", static_cast<std::int64_t>(context.BuildMode()));
        if (field == "optimization")
            return compilerEnum("OptimizationMode", static_cast<std::int64_t>(context.Optimization()));
        if (field == "debugAssertions")
            return CompilerLiteral(location, TypeRef::MakeBool(), context.DebugAssertions() ? "true" : "false");
        if (field == "debugInfo")
            return CompilerLiteral(location, TypeRef::MakeBool(), context.DebugInfo() ? "true" : "false");
        if (field == "isTest")
            return CompilerLiteral(location, TypeRef::MakeBool(), context.isTest ? "true" : "false");
        if (field == "outputKind")
            return compilerEnum("OutputKind", static_cast<std::int64_t>(context.outputKind));
        if (field == "timestamp")
            return CompilerLiteral(location, TypeRef::MakeUInt64(), std::to_string(context.buildInfo.Timestamp()));
        if (field == "date")
            return CompilerString(location, FormatBuildTime(context, "%Y-%m-%d"));
        if (field == "time")
            return CompilerString(location, FormatBuildTime(context, "%H:%M:%S"));
    }
    if (root == "Compiler" && field == "version") {
        const ParsedSemanticVersion version =
            ParseSemanticVersion(context.buildInfo.CompilerVersion()).value_or(ParsedSemanticVersion{});
        auto object = std::make_unique<HirStructInitExpr>();
        object->location = location;
        object->type = TypeRef::MakeNamed("SemanticVersion");
        object->typeName = "SemanticVersion";
        auto addField = [&](std::string name, const std::uint64_t value) {
            HirStructInitField fieldValue;
            fieldValue.name = std::move(name);
            fieldValue.value = CompilerLiteral(location, TypeRef::MakeUInt(), std::to_string(value));
            object->fields.push_back(std::move(fieldValue));
        };
        addField("major", version.major);
        addField("minor", version.minor);
        addField("patch", version.patch);
        return object;
    }
    if (root == "Source") {
        if (field == "line")
            return CompilerLiteral(location, TypeRef::MakeUInt(), std::to_string(location.line));
        if (field == "column")
            return CompilerLiteral(location, TypeRef::MakeUInt(), std::to_string(location.column));
        if (field == "file" || field == "fileName")
            return CompilerString(location, std::filesystem::path(currentFile).filename().string());
        if (field == "filePath")
            return CompilerString(location, LogicalCurrentFilePath());
        if (field == "function")
            return CompilerString(location, currentFunctionName);
        if (field == "module")
            return CompilerString(location, currentModulePath);
    }
    return nullptr;
}

HirExprPtr AstToHirContext::LowerCompilerParamObject(const std::string &root, const TypeRef &type,
                                                     const SourceLocation location) {
    if (root != "Target" && root != "Build" && root != "Compiler" && root != "Source" && root != "Config") {
        return nullptr;
    }

    auto object = std::make_unique<HirStructInitExpr>();
    object->location = location;
    object->type = type;
    object->typeName = type.name.empty() ? root : type.name;
    const auto declaration = structDecls.find(object->typeName);
    if (declaration == structDecls.end()) {
        return nullptr;
    }
    for (const StructDecl::Field &field : declaration->second->fields) {
        HirStructInitField value;
        value.name = field.name;
        value.value = LowerCompilerParamField(root, field.name, location);
        if (!value.value) {
            return nullptr;
        }
        object->fields.push_back(std::move(value));
    }
    return object;
}

HirExprPtr AstToHirContext::LowerCompilerParamCall(const std::string &root, const std::string &member,
                                                   const CallExpr &call) const {
    if (call.args.size() != 1 || !call.args[0]) {
        return nullptr;
    }
    const auto *argumentExpression = call.args[0].get();
    std::string argument;
    if (const auto *variant = dynamic_cast<const EnumShorthandExpr *>(argumentExpression)) {
        argument = variant->variant;
    }
    else if (const auto *path = dynamic_cast<const PathExpr *>(argumentExpression);
             path && path->segments.size() == 2) {
        argument = path->segments[1];
    }
    else if (const auto *literal = dynamic_cast<const LiteralExpr *>(argumentExpression);
             literal && literal->token.kind == TokenKind::StringLiteral) {
        argument = LowerLiteralValue(*literal);
    }
    else {
        return nullptr;
    }

    if (root == "Target" && member == "HasFeature")
        return CompilerLiteral(call.location, TypeRef::MakeBool(),
                               TargetHasFeature(context, argument) ? "true" : "false");
    if (root == "Compiler" && member == "HasFeature")
        return CompilerLiteral(call.location, TypeRef::MakeBool(), CompilerHasFeature(argument) ? "true" : "false");
    if (root == "Config" && member == "Get") {
        const auto value = context.config.find(argument);
        return CompilerString(call.location, value == context.config.end() ? std::string{} : value->second);
    }
    if (root == "Config" && member == "Has")
        return CompilerLiteral(call.location, TypeRef::MakeBool(),
                               context.config.contains(argument) ? "true" : "false");
    return nullptr;
}

HirExprPtr AstToHirContext::LowerCompilerParamIdentifier(const IdentExpr &expression) {
    if (HirSymbol *symbol = currentScope->Lookup(expression.name);
        symbol && symbol->kind == HirSymbol::Kind::Const && !symbol->intrinsicName.empty()) {
        return LowerCompilerParamObject(symbol->intrinsicName, symbol->type, expression.location);
    }
    return nullptr;
}

HirExprPtr AstToHirContext::LowerCompilerParamFieldExpression(const FieldExpr &expression) {
    if (const auto root = CompilerParamRoot(*expression.object)) {
        return LowerCompilerParamField(*root, expression.field, expression.location);
    }
    return nullptr;
}

HirExprPtr AstToHirContext::LowerIntrinsicExpr(const IntrinsicExpr &expression) const {
    auto intrinsicArgument = [&]() -> std::string {
        if (expression.args.size() != 1 || !expression.args[0]) {
            return {};
        }
        if (const auto *variant = dynamic_cast<const EnumShorthandExpr *>(expression.args[0].get())) {
            return variant->variant;
        }
        if (const auto *literal = dynamic_cast<const LiteralExpr *>(expression.args[0].get());
            literal && literal->token.kind == TokenKind::StringLiteral) {
            return LowerLiteralValue(*literal);
        }
        return {};
    };

    TypeRef type;
    std::string value;
    using Kind = IntrinsicExpr::Kind;
    switch (expression.kind) {
    case Kind::Line:
        type = TypeRef::MakeUInt();
        value = std::to_string(expression.location.line);
        break;
    case Kind::Column:
        type = TypeRef::MakeUInt();
        value = std::to_string(expression.location.column);
        break;
    case Kind::File:
    case Kind::FileName:
        type = TypeRef::MakeNamed("Slice<char8>");
        value = std::filesystem::path(currentFile).filename().string();
        break;
    case Kind::FilePath:
        type = TypeRef::MakeNamed("Slice<char8>");
        value = LogicalCurrentFilePath();
        break;
    case Kind::Function:
        type = TypeRef::MakeNamed("Slice<char8>");
        value = currentFunctionName;
        break;
    case Kind::Date:
        type = TypeRef::MakeNamed("Slice<char8>");
        value = FormatBuildTime(context, "%Y-%m-%d");
        break;
    case Kind::Time:
        type = TypeRef::MakeNamed("Slice<char8>");
        value = FormatBuildTime(context, "%H:%M:%S");
        break;
    case Kind::Module:
        type = TypeRef::MakeNamed("Slice<char8>");
        value = currentModulePath;
        break;
    case Kind::CompilerVersion:
        type = TypeRef::MakeNamed("Slice<char8>");
        value = context.buildInfo.CompilerVersion();
        break;
    case Kind::Os:
    case Kind::Arch:
    case Kind::Abi:
    case Kind::Endian:
    case Kind::DataModel:
    case Kind::ObjectFormat:
    case Kind::BuildMode:
    case Kind::Optimization:
    case Kind::OutputKind:
        type = TypeRef::MakeUnknown();
        break;
    case Kind::PointerBits:
        type = TypeRef::MakeUInt();
        value = std::to_string(context.target.pointer_size * 8);
        break;
    case Kind::TargetTriple:
        type = TypeRef::MakeNamed("Slice<char8>");
        value = context.targetTriple;
        break;
    case Kind::TargetFeature:
        type = TypeRef::MakeBool();
        value = TargetHasFeature(context, intrinsicArgument()) ? "true" : "false";
        break;
    case Kind::BuildProfile:
        type = TypeRef::MakeNamed("Slice<char8>");
        value = context.ProfileName();
        break;
    case Kind::DebugAssertions:
        type = TypeRef::MakeBool();
        value = context.DebugAssertions() ? "true" : "false";
        break;
    case Kind::DebugInfo:
        type = TypeRef::MakeBool();
        value = context.DebugInfo() ? "true" : "false";
        break;
    case Kind::IsTest:
        type = TypeRef::MakeBool();
        value = context.isTest ? "true" : "false";
        break;
    case Kind::BuildTimestamp:
        type = TypeRef::MakeUInt64();
        value = std::to_string(context.buildInfo.Timestamp());
        break;
    case Kind::CompilerHasFeature:
        type = TypeRef::MakeBool();
        value = CompilerHasFeature(intrinsicArgument()) ? "true" : "false";
        break;
    case Kind::Config: {
        type = TypeRef::MakeNamed("Slice<char8>");
        const auto config = context.config.find(intrinsicArgument());
        value = config == context.config.end() ? std::string{} : config->second;
        break;
    }
    case Kind::HasConfig:
        type = TypeRef::MakeBool();
        value = context.config.contains(intrinsicArgument()) ? "true" : "false";
        break;
    }
    return CompilerLiteral(expression.location, std::move(type), std::move(value));
}

TypeRef AstToHirContext::StructInitFieldType(const StructInitExpr &expression, const std::string &fieldName) {
    const auto structure = structDecls.find(expression.typeName);
    if (structure == structDecls.end()) {
        if (const auto [enumDecl, variant] = LookupEnumVariantInitializer(expression.typeName); enumDecl && variant) {
            for (const auto &field : variant->namedFields) {
                if (field.name == fieldName) {
                    return ResolveType(*field.type);
                }
            }
        }
        return TypeRef::MakeUnknown();
    }

    std::unordered_map<std::string, TypeRef> substitutions;
    const std::size_t count = std::min(structure->second->typeParams.size(), expression.typeArgs.size());
    for (std::size_t i = 0; i < count; ++i) {
        substitutions.emplace(structure->second->typeParams[i], ResolveType(*expression.typeArgs[i]));
    }
    for (const auto &field : structure->second->fields) {
        if (field.name == fieldName) {
            return ResolveTypeWithSubstitution(*field.type, substitutions);
        }
    }
    return TypeRef::MakeUnknown();
}

HirExprPtr AstToHirContext::LowerAggregateExpr(const Expr &expression) {
    if (const auto *sizeOf = dynamic_cast<const SizeOfExpr *>(&expression)) {
        return CompilerLiteral(sizeOf->location, ResolvedExpressionType(*sizeOf),
                               std::to_string(ResolvedSizeOf(*sizeOf)));
    }
    if (const auto *intrinsic = dynamic_cast<const IntrinsicExpr *>(&expression)) {
        return LowerIntrinsicExpr(*intrinsic);
    }
    if (const auto *initializer = dynamic_cast<const StructInitExpr *>(&expression)) {
        if (const auto [enumDecl, variant] = LookupEnumVariantInitializer(initializer->typeName); enumDecl && variant) {
            if (!variant->namedFields.empty()) {
                auto lowered = std::make_unique<HirEnumConstructExpr>();
                lowered->location = initializer->location;
                lowered->type = EnumType(*enumDecl);
                const std::size_t separator = initializer->typeName.find("::");
                lowered->discriminant = LookupEnumVariantDiscriminant(initializer->typeName.substr(0, separator),
                                                                      initializer->typeName.substr(separator + 2))
                                            .value_or("0");
                for (const auto &field : variant->namedFields) {
                    const auto initialized = std::ranges::find_if(
                        initializer->fields, [&](const auto &candidate) { return candidate.name == field.name; });
                    if (initialized != initializer->fields.end()) {
                        lowered->payloads.push_back(LowerExprAs(*initialized->value, ResolveType(*field.type)));
                    }
                }
                return lowered;
            }
            return CompilerLiteral(initializer->location, EnumType(*enumDecl),
                                   LookupEnumVariantDiscriminant(enumDecl->name, variant->name).value_or("0"));
        }

        auto lowered = std::make_unique<HirStructInitExpr>();
        lowered->location = initializer->location;
        lowered->typeName = GenericStructInitName(*initializer);
        lowered->type = ResolvedExpressionType(*initializer);
        for (const auto &field : initializer->fields) {
            HirStructInitField loweredField;
            loweredField.name = field.name;
            loweredField.value = LowerExprAs(*field.value, StructInitFieldType(*initializer, field.name));
            lowered->fields.push_back(std::move(loweredField));
        }
        return lowered;
    }
    return nullptr;
}
} // namespace Rux::AstToHirDetail
