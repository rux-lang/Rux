// Lowering for struct, enum, union, array and slice expressions, and for the
// compile-time `Compiler` values whose fields are resolved rather than read.

#include "Lowering/AstToHir/Detail/AstToHirContext.h"
#include "Semantic/Model/PrimitiveConstants.h"
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
    return CompilerLiteral(location, TypeRef::MakeString8(), std::move(value));
}

/// Convert to UTC, not local time, so the same source and the same build timestamp produce identical output wherever
/// the compiler runs.
bool UtcTime(const std::time_t time, std::tm &out) {
#if RUX_OS_WINDOWS
    return gmtime_s(&out, &time) == 0;
#else
    return gmtime_r(&time, &out) != nullptr;
#endif
}

/// Render the build timestamp for a compile-time `Build` value. Reads the timestamp from the context rather than the
/// clock, which is what lets a reproducible build pin it.
std::string FormatBuildTime(const CompileTimeContext &context, const char *format) {
    const std::time_t value = static_cast<std::time_t>(context.buildInfo.Timestamp());
    std::tm utc{};
    if (!UtcTime(value, utc)) {
        return {};
    }
    char buffer[32]{};
    return std::strftime(buffer, sizeof(buffer), format, &utc) == 0 ? std::string{} : std::string(buffer);
}

/// Whether the target being compiled for has a named CPU feature. Answered from target data, never from the host, so a
/// cross build does not inherit the building machine's capabilities.
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

/// Whether this compiler implements a named language feature, letting source guard a construct that older releases
/// would not accept.
bool CompilerHasFeature(const std::string_view feature) {
    static constexpr std::array features{
        "conditional-compilation", "namespaced-intrinsics",      "target-intrinsics",
        "build-intrinsics",        "compiler-feature-detection", "source-location-defaults",
        "extern-symbol-names",     "no-return-attribute",        "when-attribute",
        "link-attribute"};
    return std::ranges::contains(features, feature);
}
} // namespace

HirExprPtr AstToHirContext::LowerMatchSubject(const Expr &expression) {
    HirExprPtr lowered = LowerExpr(expression);
    if (lowered->type.kind != TypeRef::Kind::Reference || lowered->type.inner.empty()) {
        return lowered;
    }

    auto dereference = std::make_unique<HirUnaryExpr>();
    dereference->location = expression.location;
    dereference->op = TokenKind::Star;
    dereference->type = lowered->type.inner.front();
    dereference->operand = std::move(lowered);
    return dereference;
}

std::uint64_t AstToHirContext::ResolvedTypeQuery(const TypeQueryExpr &expression) {
    if (const std::uint64_t *value = model.TryGetTypeQueryValue(expression)) {
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
    // Which of the two was asked for. Analysis answers this for a type that is concrete where it is written; this
    // path is what answers it for a type parameter, once an instantiation says what the parameter stands for.
    return expression.query == TypeQueryExpr::Query::Size ? layout->size : layout->alignment;
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
        type = TypeRef::MakeString8();
        value = std::filesystem::path(currentFile).filename().string();
        break;
    case Kind::FilePath:
        type = TypeRef::MakeString8();
        value = LogicalCurrentFilePath();
        break;
    case Kind::Function:
        type = TypeRef::MakeString8();
        value = currentFunctionName;
        break;
    case Kind::Date:
        type = TypeRef::MakeString8();
        value = FormatBuildTime(context, "%Y-%m-%d");
        break;
    case Kind::Time:
        type = TypeRef::MakeString8();
        value = FormatBuildTime(context, "%H:%M:%S");
        break;
    case Kind::Module:
        type = TypeRef::MakeString8();
        value = currentModulePath;
        break;
    case Kind::CompilerVersion:
        type = TypeRef::MakeString8();
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
        type = TypeRef::MakeString8();
        value = context.targetTriple;
        break;
    case Kind::TargetFeature:
        type = TypeRef::MakeBool();
        value = TargetHasFeature(context, intrinsicArgument()) ? "true" : "false";
        break;
    case Kind::BuildProfile:
        type = TypeRef::MakeString8();
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
        type = TypeRef::MakeString8();
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
        if (const auto unionType = unionDecls.find(expression.typeName); unionType != unionDecls.end()) {
            for (const auto &field : unionType->second->fields) {
                if (field.name == fieldName) {
                    return ResolveType(*field.type);
                }
            }
        }
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
        substitutions.emplace(structure->second->typeParams[i].name, ResolveType(*expression.typeArgs[i]));
    }
    for (const auto &field : structure->second->fields) {
        if (field.name == fieldName) {
            return ResolveTypeWithSubstitution(*field.type, substitutions);
        }
    }
    return TypeRef::MakeUnknown();
}

std::optional<TypeRef> AstToHirContext::InterfaceImplementationType(const TypeRef &expressionType,
                                                                    const TypeRef &targetType) const {
    TypeRef target = targetType.kind == TypeRef::Kind::Reference && !targetType.inner.empty() ? targetType.inner.front()
                                                                                              : targetType;
    TypeRef expression = expressionType.kind == TypeRef::Kind::Reference && !expressionType.inner.empty()
                           ? expressionType.inner.front()
                           : expressionType;
    target.isMut = false;
    expression.isMut = false;
    if (target.kind != TypeRef::Kind::Named) {
        return std::nullopt;
    }
    const auto hasVtable = [&](const TypeRef &type) {
        const auto implementation = typeInterfaceVtables.find(type.ToString());
        return implementation != typeInterfaceVtables.end() && implementation->second.contains(target.name);
    };
    if (hasVtable(expression)) {
        return expression;
    }
    if (expression.kind == TypeRef::Kind::Int && hasVtable(TypeRef::MakeInt64())) {
        return TypeRef::MakeInt64();
    }
    if (expression.kind == TypeRef::Kind::Int64 && hasVtable(TypeRef::MakeInt())) {
        return TypeRef::MakeInt();
    }
    if (expression.kind == TypeRef::Kind::UInt && hasVtable(TypeRef::MakeUInt64())) {
        return TypeRef::MakeUInt64();
    }
    if (expression.kind == TypeRef::Kind::UInt64 && hasVtable(TypeRef::MakeUInt())) {
        return TypeRef::MakeUInt();
    }
    return std::nullopt;
}

HirExprPtr AstToHirContext::LowerExprAs(const Expr &expression, const TypeRef &targetType) {
    if (IsNullLiteral(expression) && targetType.kind == TypeRef::Kind::Pointer) {
        return CompilerLiteral(expression.location, targetType, "0");
    }

    if (const auto *array = dynamic_cast<const ArrayExpr *>(&expression);
        array && targetType.kind == TypeRef::Kind::Array && targetType.arrayLength && !targetType.inner.empty()) {
        auto lowered = std::make_unique<HirArrayExpr>();
        lowered->location = array->location;
        lowered->elementType = targetType.inner[0];
        std::vector<HirPartialDropAction> completed;
        for (std::size_t index = 0; index < array->elements.size(); ++index) {
            const auto &element = array->elements[index];
            AppendFailureCleanup(lowered->failureCleanups, completed);
            lowered->elements.push_back(LowerExprAs(*element, targetType.inner[0]));
            if (auto cleanup = PartialCleanup(HirPartialDropAction::Kind::Element, targetType.inner[0], index, {},
                                              element->location)) {
                completed.push_back(std::move(*cleanup));
            }
        }
        lowered->type = targetType;
        return lowered;
    }

    if (const auto *repeat = dynamic_cast<const ArrayRepeatExpr *>(&expression);
        repeat && targetType.kind == TypeRef::Kind::Array && targetType.arrayLength && !targetType.inner.empty()) {
        return LowerArrayRepeatAs(*repeat, targetType.inner[0], targetType);
    }

    if (const auto *array = dynamic_cast<const ArrayExpr *>(&expression)) {
        if (const auto sliceElement = SliceElementType(targetType)) {
            auto lowered = std::make_unique<HirArrayExpr>();
            lowered->location = array->location;
            lowered->elementType = *sliceElement;
            std::vector<HirPartialDropAction> completed;
            for (std::size_t index = 0; index < array->elements.size(); ++index) {
                const auto &element = array->elements[index];
                AppendFailureCleanup(lowered->failureCleanups, completed);
                lowered->elements.push_back(LowerExprAs(*element, *sliceElement));
                if (auto cleanup = PartialCleanup(HirPartialDropAction::Kind::Element, *sliceElement, index, {},
                                                  element->location)) {
                    completed.push_back(std::move(*cleanup));
                }
            }
            lowered->type = TypeRef::MakeArray(*sliceElement, array->elements.size());

            auto view = std::make_unique<HirArrayToSliceExpr>();
            view->location = array->location;
            view->type = targetType;
            view->elementType = *sliceElement;
            view->length = array->elements.size();
            view->value = std::move(lowered);
            return view;
        }
    }

    if (const auto *repeat = dynamic_cast<const ArrayRepeatExpr *>(&expression)) {
        if (const auto sliceElement = SliceElementType(targetType)) {
            const TypeRef resolved = ResolvedExpressionType(*repeat);
            const std::uint64_t count = resolved.arrayLength.value_or(0);
            auto lowered = LowerArrayRepeatAs(*repeat, *sliceElement, TypeRef::MakeArray(*sliceElement, count));

            auto view = std::make_unique<HirArrayToSliceExpr>();
            view->location = repeat->location;
            view->type = targetType;
            view->elementType = *sliceElement;
            view->length = count;
            view->value = std::move(lowered);
            return view;
        }
    }

    if (const auto *tuple = dynamic_cast<const TupleExpr *>(&expression);
        tuple && targetType.kind == TypeRef::Kind::Tuple && tuple->elements.size() == targetType.inner.size()) {
        auto lowered = std::make_unique<HirTupleExpr>();
        lowered->location = tuple->location;
        std::vector<HirPartialDropAction> completed;
        for (std::size_t i = 0; i < tuple->elements.size(); ++i) {
            AppendFailureCleanup(lowered->failureCleanups, completed);
            lowered->elements.push_back(LowerExprAs(*tuple->elements[i], targetType.inner[i]));
            if (auto cleanup = PartialCleanup(HirPartialDropAction::Kind::TupleElement, targetType.inner[i], i, {},
                                              tuple->elements[i]->location)) {
                completed.push_back(std::move(*cleanup));
            }
        }
        lowered->type = targetType;
        return lowered;
    }

    HirExprPtr lowered = LowerExpr(expression);
    if (targetType.kind == TypeRef::Kind::Reference) {
        if (lowered->type.kind == TypeRef::Kind::Reference) {
            lowered->type = targetType;
            return lowered;
        }
        const TypeRef &referent = targetType.inner.front();
        TypeRef expressionValueType = lowered->type;
        TypeRef referentValueType = referent;
        expressionValueType.isMut = false;
        referentValueType.isMut = false;
        if (referent.kind == TypeRef::Kind::Named) {
            if (HirSymbol *symbol = currentScope->Lookup(referent.name);
                symbol && symbol->kind == HirSymbol::Kind::Interface && expressionValueType != referentValueType) {
                std::optional<TypeRef> implementationType = InterfaceImplementationType(lowered->type, targetType);
                if (!implementationType) {
                    implementationType = lowered->type.kind == TypeRef::Kind::Reference && !lowered->type.inner.empty()
                                           ? lowered->type.inner.front()
                                           : lowered->type;
                    implementationType->isMut = false;
                }
                auto coercion = std::make_unique<HirCoerceToInterfaceExpr>();
                coercion->location = expression.location;
                coercion->type = targetType;
                coercion->borrowed = true;
                const auto type = typeInterfaceVtables.find(implementationType->ToString());
                if (type != typeInterfaceVtables.end()) {
                    const auto label = type->second.find(referent.name);
                    if (label != type->second.end()) {
                        coercion->vtableLabel = label->second;
                    }
                }
                coercion->value = std::move(lowered);
                return coercion;
            }
        }
        auto borrow = std::make_unique<HirUnaryExpr>();
        borrow->location = expression.location;
        borrow->op = TokenKind::At;
        borrow->type = targetType;
        borrow->operand = std::move(lowered);
        return borrow;
    }
    if (UnsuffixedIntegerLiteralFits(expression, targetType)) {
        lowered->type = targetType;
    }
    else if (targetType.kind == TypeRef::Kind::Named) {
        if (HirSymbol *symbol = currentScope->Lookup(targetType.name);
            symbol && symbol->kind == HirSymbol::Kind::Interface && lowered->type != targetType) {
            std::optional<TypeRef> implementationType = InterfaceImplementationType(lowered->type, targetType);
            if (!implementationType) {
                implementationType = lowered->type;
            }
            const std::string typeName = implementationType->ToString();
            if (UnsuffixedIntegerLiteralFits(expression, *implementationType)) {
                lowered->type = *implementationType;
            }
            auto coercion = std::make_unique<HirCoerceToInterfaceExpr>();
            coercion->location = expression.location;
            coercion->type = targetType;
            const auto interface = interfaceDecls.find(targetType.name);
            if (interface != interfaceDecls.end() && !interface->second->methods.empty()) {
                if (const auto type = typeInterfaceVtables.find(typeName); type != typeInterfaceVtables.end()) {
                    if (const auto label = type->second.find(targetType.name); label != type->second.end()) {
                        coercion->vtableLabel = label->second;
                    }
                }
            }
            coercion->value = std::move(lowered);
            return coercion;
        }
    }
    if (lowered->type.kind == TypeRef::Kind::Array && lowered->type.arrayLength && !lowered->type.inner.empty() &&
        SliceElementType(targetType)) {
        auto coercion = std::make_unique<HirArrayToSliceExpr>();
        coercion->location = expression.location;
        coercion->type = targetType;
        coercion->elementType = lowered->type.inner[0];
        coercion->length = *lowered->type.arrayLength;
        coercion->value = std::move(lowered);
        return coercion;
    }
    return lowered;
}

std::unique_ptr<HirArrayExpr> AstToHirContext::LowerArrayRepeatAs(const ArrayRepeatExpr &expression,
                                                                  const TypeRef &elementType,
                                                                  const TypeRef &arrayType) {
    auto lowered = std::make_unique<HirArrayExpr>();
    lowered->location = expression.location;
    lowered->type = arrayType;
    lowered->elementType = elementType;
    lowered->repeatCount = arrayType.arrayLength.value_or(0);
    lowered->repeatCopyPlan = BuildCopyPlan(elementType);
    lowered->repeatedElement = LowerExprAs(*expression.value, elementType);
    if (const DropGluePlan *glue = model.TryGetDropGlue(elementType)) {
        lowered->repeatElementDropGlue = glue->symbol;
    }
    return lowered;
}

HirExprPtr AstToHirContext::LowerAggregateExpr(const Expr &expression) {
    if (const auto *sizeOf = dynamic_cast<const TypeQueryExpr *>(&expression)) {
        return CompilerLiteral(sizeOf->location, ResolvedExpressionType(*sizeOf),
                               std::to_string(ResolvedTypeQuery(*sizeOf)));
    }
    if (const auto *intrinsic = dynamic_cast<const IntrinsicExpr *>(&expression)) {
        return LowerIntrinsicExpr(*intrinsic);
    }
    if (const auto *initializer = dynamic_cast<const StructInitExpr *>(&expression)) {
        if (const auto [enumDecl, variant] = LookupEnumVariantInitializer(initializer->typeName); enumDecl && variant) {
            if (!variant->namedFields.empty()) {
                auto lowered = std::make_unique<HirEnumConstructExpr>();
                lowered->location = initializer->location;
                lowered->form = enumDecl->IsVariant() ? CaseTypeForm::Variant : CaseTypeForm::Enumeration;
                lowered->type = EnumType(*enumDecl);
                const std::size_t separator = initializer->typeName.find("::");
                lowered->discriminant = LookupEnumVariantDiscriminant(initializer->typeName.substr(0, separator),
                                                                      initializer->typeName.substr(separator + 2))
                                            .value_or("0");
                std::vector<HirPartialDropAction> completed;
                for (std::size_t index = 0; index < variant->namedFields.size(); ++index) {
                    const auto &field = variant->namedFields[index];
                    const auto initialized = std::ranges::find_if(
                        initializer->fields, [&](const auto &candidate) { return candidate.name == field.name; });
                    if (initialized != initializer->fields.end()) {
                        const TypeRef fieldType = ResolveType(*field.type);
                        AppendFailureCleanup(lowered->failureCleanups, completed);
                        lowered->payloads.push_back(LowerExprAs(*initialized->value, fieldType));
                        if (auto cleanup = PartialCleanup(HirPartialDropAction::Kind::EnumPayload, fieldType, index,
                                                          field.name, initialized->location, CaseTypeForm::Variant)) {
                            completed.push_back(std::move(*cleanup));
                        }
                    }
                }
                return lowered;
            }
            const std::string discriminant = LookupEnumVariantDiscriminant(enumDecl->name, variant->name).value_or("0");
            if (!enumDecl->IsVariant()) {
                return CompilerLiteral(initializer->location, EnumType(*enumDecl), discriminant);
            }
            auto lowered = std::make_unique<HirEnumConstructExpr>();
            lowered->location = initializer->location;
            lowered->form = CaseTypeForm::Variant;
            lowered->type = EnumType(*enumDecl);
            lowered->discriminant = discriminant;
            return lowered;
        }

        auto lowered = std::make_unique<HirStructInitExpr>();
        lowered->location = initializer->location;
        lowered->typeName = GenericStructInitName(*initializer);
        lowered->type = ResolvedExpressionType(*initializer);
        std::vector<HirPartialDropAction> completed;
        for (std::size_t index = 0; index < initializer->fields.size(); ++index) {
            const auto &field = initializer->fields[index];
            const TypeRef fieldType = StructInitFieldType(*initializer, field.name);
            AppendFailureCleanup(lowered->failureCleanups, completed);
            HirStructInitField loweredField;
            loweredField.name = field.name;
            loweredField.value = LowerExprAs(*field.value, fieldType);
            lowered->fields.push_back(std::move(loweredField));
            if (auto cleanup =
                    PartialCleanup(HirPartialDropAction::Kind::Field, fieldType, index, field.name, field.location)) {
                completed.push_back(std::move(*cleanup));
            }
        }
        return lowered;
    }
    if (const auto *path = dynamic_cast<const PathExpr *>(&expression)) {
        if (path->segments.size() == 2) {
            if (HirSymbol *first = currentScope->Lookup(path->segments[0]);
                first && (first->kind == HirSymbol::Kind::Type || first->kind == HirSymbol::Kind::Interface)) {
                if (first->kind == HirSymbol::Kind::Type) {
                    if (const auto constant = LookupPrimitiveConstant(first->type, path->segments[1], context)) {
                        return CompilerLiteral(path->location, constant->type, constant->value);
                    }
                    if (const auto discriminant = LookupEnumVariantDiscriminant(path->segments[0], path->segments[1])) {
                        const auto *variant = LookupEnumVariant(path->segments[0], path->segments[1]);
                        if (variant && (!variant->fields.empty() || !variant->namedFields.empty())) {
                            auto lowered = std::make_unique<HirPathExpr>();
                            lowered->location = path->location;
                            lowered->segments = path->segments;
                            lowered->type = EnumVariantConstructorType(*enumDecls.at(path->segments[0]), *variant);
                            return lowered;
                        }
                        const EnumDecl &declaration = *enumDecls.at(path->segments[0]);
                        if (!declaration.IsVariant()) {
                            return CompilerLiteral(path->location, EnumType(declaration), *discriminant);
                        }
                        auto lowered = std::make_unique<HirEnumConstructExpr>();
                        lowered->location = path->location;
                        lowered->form = CaseTypeForm::Variant;
                        lowered->type = EnumType(declaration);
                        lowered->discriminant = *discriminant;
                        return lowered;
                    }
                }
                const TypeRef receiverType = first->type.IsUnknown() ? TypeRef::MakeNamed(first->name) : first->type;
                if (const FuncDecl *method = LookupMethod(receiverType, path->segments[1])) {
                    auto lowered = std::make_unique<HirVarExpr>();
                    lowered->location = path->location;
                    if (const auto *identity = model.TryGetSymbolIdentity(*method)) {
                        lowered->name = identity->linkerName;
                    }
                    else {
                        lowered->name = CalleeName(path->segments[0], path->segments[1], receiverType, *method);
                    }
                    lowered->type = AssociatedFunctionType(receiverType, *method);
                    return lowered;
                }
            }
        }

        auto lowered = std::make_unique<HirPathExpr>();
        lowered->location = path->location;
        lowered->segments = path->segments;
        lowered->type = ResolvedExpressionType(*path);
        return lowered;
    }
    if (const auto *range = dynamic_cast<const RangeExpr *>(&expression)) {
        auto lowered = std::make_unique<HirRangeExpr>();
        lowered->location = range->location;
        lowered->inclusive = range->inclusive;
        if (range->lo) {
            lowered->lo = LowerExpr(*range->lo);
        }
        if (range->hi) {
            lowered->hi = LowerExpr(*range->hi);
        }
        if (range->lo && lowered->hi && lowered->hi->type.IsInteger() &&
            UnsuffixedIntegerLiteralFits(*range->lo, lowered->hi->type)) {
            lowered->lo = LowerExprAs(*range->lo, lowered->hi->type);
        }
        else if (range->hi && lowered->lo && lowered->lo->type.IsInteger() &&
                 UnsuffixedIntegerLiteralFits(*range->hi, lowered->lo->type)) {
            lowered->hi = LowerExprAs(*range->hi, lowered->lo->type);
        }
        lowered->type = ResolvedExpressionType(*range);
        return lowered;
    }
    if (const auto *repeat = dynamic_cast<const ArrayRepeatExpr *>(&expression)) {
        const TypeRef type = ResolvedExpressionType(*repeat);
        const TypeRef elementType = type.kind == TypeRef::Kind::Array && !type.inner.empty()
                                      ? type.inner[0]
                                      : ResolvedExpressionType(*repeat->value);
        return LowerArrayRepeatAs(*repeat, elementType, type);
    }
    if (const auto *array = dynamic_cast<const ArrayExpr *>(&expression)) {
        auto lowered = std::make_unique<HirArrayExpr>();
        lowered->location = array->location;
        lowered->type = ResolvedExpressionType(*array);
        if (lowered->type.kind == TypeRef::Kind::Array && !lowered->type.inner.empty()) {
            lowered->elementType = lowered->type.inner[0];
        }
        std::vector<HirPartialDropAction> completed;
        for (std::size_t index = 0; index < array->elements.size(); ++index) {
            const auto &element = array->elements[index];
            AppendFailureCleanup(lowered->failureCleanups, completed);
            lowered->elements.push_back(LowerExpr(*element));
            const TypeRef elementType =
                lowered->elementType.IsUnknown() ? lowered->elements.back()->type : lowered->elementType;
            if (auto cleanup =
                    PartialCleanup(HirPartialDropAction::Kind::Element, elementType, index, {}, element->location)) {
                completed.push_back(std::move(*cleanup));
            }
        }
        return lowered;
    }
    if (const auto *tuple = dynamic_cast<const TupleExpr *>(&expression)) {
        auto lowered = std::make_unique<HirTupleExpr>();
        lowered->location = tuple->location;
        lowered->type = ResolvedExpressionType(*tuple);
        std::vector<HirPartialDropAction> completed;
        for (std::size_t index = 0; index < tuple->elements.size(); ++index) {
            const auto &element = tuple->elements[index];
            AppendFailureCleanup(lowered->failureCleanups, completed);
            lowered->elements.push_back(LowerExpr(*element));
            const TypeRef elementType =
                index < lowered->type.inner.size() ? lowered->type.inner[index] : lowered->elements.back()->type;
            if (auto cleanup = PartialCleanup(HirPartialDropAction::Kind::TupleElement, elementType, index, {},
                                              element->location)) {
                completed.push_back(std::move(*cleanup));
            }
        }
        return lowered;
    }
    if (const auto *match = dynamic_cast<const MatchExpr *>(&expression)) {
        auto lowered = std::make_unique<HirMatchExpr>();
        lowered->location = match->location;
        lowered->type = ResolvedExpressionType(*match);
        lowered->subject = LowerMatchSubject(*match->subject);
        // As in the statement form: an arm's bindings own what they took only if the subject was handed over.
        const bool armsOwnPayload = lowered->subject->consumption.has_value();
        for (const auto &arm : match->arms) {
            HirMatchArm loweredArm;
            loweredArm.location = arm.location;
            PushScope();
            const bool savedOwnership = patternBindingsOwnPayload;
            patternBindingsOwnPayload = armsOwnPayload;
            loweredArm.pattern = LowerPattern(*arm.pattern, lowered->subject->type);
            patternBindingsOwnPayload = savedOwnership;
            loweredArm.body = LowerExpr(*arm.body);
            loweredArm.cleanups = CurrentScopeCleanups();
            PopScope();
            lowered->arms.push_back(std::move(loweredArm));
        }
        return lowered;
    }
    return nullptr;
}
} // namespace Rux::AstToHirDetail
