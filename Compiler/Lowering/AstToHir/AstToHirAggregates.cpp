// Lowering for struct, enum, union, array and slice expressions, and for the
// compile-time `Compiler` values whose fields are resolved rather than read.

#include "Lowering/AstToHir/Detail/AstToHirContext.h"
#include "Semantic/Model/CompilerParameters.h"
#include "Semantic/SemanticVersion.h"
#include "Target/Platform.h"

#include <algorithm>
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
    literal->type = type;
    literal->value = std::move(value);
    return literal;
}

HirExprPtr CompilerString(const SourceLocation location, std::string value) {
    return CompilerLiteral(location, TypeRef::MakeText(TypeRef::Kind::Char8), std::move(value));
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
std::string FormatBuildTime(const std::int64_t timestamp, const char *format) {
    const std::time_t value = static_cast<std::time_t>(timestamp);
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

} // namespace

HirExprPtr AstToHirContext::LowerMatchSubject(const Expr &expression, const ValueConsumptionKind kind) {
    HirExprPtr lowered = LowerExpr(expression);
    if (lowered->type->kind != TypeRef::Kind::Reference || lowered->type->inner.empty()) {
        // The arms own a subject analysis handed them. A transfer already says so; a temporary, or the copy a by-value
        // match makes of a named copyable value, is marked here, which is what gives the bindings their cleanups and
        // the arms the duty to destroy what they leave.
        if (lowered->consumption || !model.IsOwnedMatchSubject(expression)) {
            return lowered;
        }
        // A copy whose plan is a plain load would leave the subject reading the named storage itself, and the arms
        // would destroy parts its owner still holds. A droppable value gets storage of its own.
        if (const ValueCopy *copy = model.TryGetCopy(expression);
            copy && !HirCast<const HirCopyExpr>(lowered.get()) && model.TryGetDropGlue(lowered->type)) {
            auto copied = std::make_unique<HirCopyExpr>();
            copied->location = lowered->location;
            copied->type = lowered->type;
            copied->plan.type = lowered->type;
            copied->value = std::move(lowered);
            lowered = std::move(copied);
        }
        lowered->consumption = kind;
        return lowered;
    }

    auto dereference = std::make_unique<HirUnaryExpr>();
    dereference->location = expression.location;
    dereference->op = TokenKind::Star;
    dereference->type = lowered->type->inner.front();
    dereference->operand = std::move(lowered);
    return dereference;
}

std::uint64_t AstToHirContext::ResolvedTypeQuery(const TypeQueryExpr &expression) {
    if (const std::uint64_t *value = model.TryGetTypeQueryValue(expression)) {
        return *value;
    }

    const TypeRef type = ResolveTypeWithSubstitution(*expression.type, currentSubstitutions);
    if (type->kind == TypeRef::Kind::TypeParam) {
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
    std::string name = BaseTypeNameImpl(ResolvedExpressionType(expression)->name);
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
AstToHirContext::LookupEnumVariantInitializer(const StructInitExpr &expression) const {
    const std::string &typeName = expression.typeName;
    const std::size_t separator = typeName.rfind("::");
    if (separator == std::string::npos) {
        return {nullptr, nullptr};
    }

    const auto declaration = enumDecls.find(NamedBaseTypeName(ResolvedExpressionType(expression)));
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
    // Semantic analysis accepted only declarations whose fields the shared table lists, and nothing else is answered.
    if (!IsSuppliedCompilerParameterField(root, field)) {
        return nullptr;
    }
    auto compilerEnum = [&](const std::string &typeName, const std::int64_t value) {
        TypeRef type = TypeRef::MakeNamed(typeName);
        if (const auto declaration = enumDecls.find(typeName); declaration != enumDecls.end()) {
            type = EnumType(*declaration->second);
        }
        return CompilerLiteral(location, type, std::to_string(value));
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
            return CompilerLiteral(location, TypeRef::MakeUInt64(), std::to_string(BuildTimestamp()));
        if (field == "date")
            return CompilerString(location, FormatBuildTime(BuildTimestamp(), "%Y-%m-%d"));
        if (field == "time")
            return CompilerString(location, FormatBuildTime(BuildTimestamp(), "%H:%M:%S"));
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
        const SourceLocation site = sourceSiteOverride.value_or(location);
        if (field == "line")
            return CompilerLiteral(location, TypeRef::MakeUInt(), std::to_string(site.line));
        if (field == "column")
            return CompilerLiteral(location, TypeRef::MakeUInt(), std::to_string(site.column));
        if (field == "file" || field == "fileName")
            return CompilerString(location, std::filesystem::path(currentFile).filename().string());
        if (field == "filePath")
            return CompilerString(location, LogicalCurrentFilePath());
        if (field == "function")
            return CompilerString(location, currentFunctionName);
        if (field == "module")
            return CompilerString(location, declModulePath);
    }
    return nullptr;
}

HirExprPtr AstToHirContext::LowerCompilerParamObject(const std::string &root, const TypeRef &type,
                                                     const SourceLocation location) {
    if (!FindCompilerParameterRoot(root)) {
        return nullptr;
    }

    auto object = std::make_unique<HirStructInitExpr>();
    object->location = location;
    object->type = type;
    object->typeName = type->name.empty() ? root : type->name;
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
    if (const auto *variant = AstCast<const EnumShorthandExpr>(argumentExpression)) {
        argument = variant->variant;
    }
    else if (const auto *path = AstCast<const PathExpr>(argumentExpression); path && path->segments.size() == 2) {
        argument = path->segments[1];
    }
    else if (const auto *literal = AstCast<const LiteralExpr>(argumentExpression);
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
        return CompilerLiteral(call.location, TypeRef::MakeBool(),
                               IsSupportedCompilerFeature(argument) ? "true" : "false");
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
        // The reading expression starts at its `#` root, not at the `.` before the field, which is where `#source`
        // reports its column.
        return LowerCompilerParamField(*root, expression.field, expression.object->location);
    }
    return nullptr;
}

HirExprPtr AstToHirContext::LowerIntrinsicExpr(const IntrinsicExpr &expression) const {
    auto intrinsicArgument = [&]() -> std::string {
        if (expression.args.size() != 1 || !expression.args[0]) {
            return {};
        }
        if (const auto *variant = AstCast<const EnumShorthandExpr>(expression.args[0].get())) {
            return variant->variant;
        }
        if (const auto *literal = AstCast<const LiteralExpr>(expression.args[0].get());
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
        type = TypeRef::MakeText(TypeRef::Kind::Char8);
        value = std::filesystem::path(currentFile).filename().string();
        break;
    case Kind::FilePath:
        type = TypeRef::MakeText(TypeRef::Kind::Char8);
        value = LogicalCurrentFilePath();
        break;
    case Kind::Function:
        type = TypeRef::MakeText(TypeRef::Kind::Char8);
        value = currentFunctionName;
        break;
    case Kind::Date:
        type = TypeRef::MakeText(TypeRef::Kind::Char8);
        value = FormatBuildTime(BuildTimestamp(), "%Y-%m-%d");
        break;
    case Kind::Time:
        type = TypeRef::MakeText(TypeRef::Kind::Char8);
        value = FormatBuildTime(BuildTimestamp(), "%H:%M:%S");
        break;
    case Kind::Module:
        type = TypeRef::MakeText(TypeRef::Kind::Char8);
        value = declModulePath;
        break;
    case Kind::CompilerVersion:
        type = TypeRef::MakeText(TypeRef::Kind::Char8);
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
        type = TypeRef::MakeText(TypeRef::Kind::Char8);
        value = context.targetTriple;
        break;
    case Kind::TargetFeature:
        type = TypeRef::MakeBool();
        value = TargetHasFeature(context, intrinsicArgument()) ? "true" : "false";
        break;
    case Kind::BuildProfile:
        type = TypeRef::MakeText(TypeRef::Kind::Char8);
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
        value = std::to_string(BuildTimestamp());
        break;
    case Kind::CompilerHasFeature:
        type = TypeRef::MakeBool();
        value = IsSupportedCompilerFeature(intrinsicArgument()) ? "true" : "false";
        break;
    case Kind::Config: {
        type = TypeRef::MakeText(TypeRef::Kind::Char8);
        const auto config = context.config.find(intrinsicArgument());
        value = config == context.config.end() ? std::string{} : config->second;
        break;
    }
    case Kind::HasConfig:
        type = TypeRef::MakeBool();
        value = context.config.contains(intrinsicArgument()) ? "true" : "false";
        break;
    }
    return CompilerLiteral(expression.location, type, std::move(value));
}

TypeRef AstToHirContext::StructInitFieldType(const StructInitExpr &expression, const std::string &fieldName) {
    const auto structure = structDecls.find(NamedBaseTypeName(ResolvedExpressionType(expression)));
    if (structure == structDecls.end()) {
        if (const auto unionType = unionDecls.find(NamedBaseTypeName(ResolvedExpressionType(expression)));
            unionType != unionDecls.end()) {
            for (const auto &field : unionType->second->fields) {
                if (field.name == fieldName) {
                    return ResolveType(*field.type);
                }
            }
        }
        if (const auto [enumDecl, variant] = LookupEnumVariantInitializer(expression); enumDecl && variant) {
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

std::string AstToHirContext::InterfaceVtableLabel(const TypeRef &type, const std::string &interfaceName) {
    if (const auto known = typeInterfaceVtables.find(type.ToString()); known != typeInterfaceVtables.end()) {
        if (const auto label = known->second.find(interfaceName); label != known->second.end()) {
            return label->second;
        }
    }
    const std::string key = type.ToString() + "|" + interfaceName;
    if (const auto made = instanceVtableLabels.find(key); made != instanceVtableLabels.end()) {
        return made->second;
    }
    const std::string typeName = NamedBaseTypeName(type);
    const auto implementations = genericInterfaceImpls.find(typeName);
    const auto interface = interfaceDecls.find(interfaceName);
    if (implementations == genericInterfaceImpls.end() || interface == interfaceDecls.end()) {
        return {};
    }
    for (const ImplDecl *implementation : implementations->second) {
        if (BaseTypeNameImpl(*implementation->interfaceName) != interface->second->name) {
            continue;
        }
        // The instance's methods are emitted into this module as its other generic instances are, so the table is
        // the module's own too: its label carries the module, and two modules never define the same one.
        const std::string label = std::format("__vtable__{}__{}__{:016x}", MangleTypeName(type),
                                              interface->second->name, std::hash<std::string>{}(currentFile));
        HirImplBlock block;
        block.typeName = typeName;
        block.interfaceName = interface->second->name;
        block.location = implementation->location;
        block.vtableLabel = label;
        const TypeRef savedSelfType = currentSelfType;
        for (const auto &requirement : interface->second->methods) {
            const auto method = std::ranges::find_if(implementation->methods, [&](const auto &candidate) {
                return candidate->name == requirement->name && candidate->typeParams.empty();
            });
            if (method == implementation->methods.end()) {
                return {};
            }
            block.vtableEntries.push_back(ConcreteMethodCalleeName(typeName, type, **method));
        }
        currentSelfType = savedSelfType;
        instanceVtableLabels.emplace(key, label);
        instanceVtables.push_back(std::move(block));
        return label;
    }
    return {};
}

std::optional<TypeRef> AstToHirContext::InterfaceImplementationType(const TypeRef &expressionType,
                                                                    const TypeRef &targetType) const {
    TypeRef target = targetType->kind == TypeRef::Kind::Reference && !targetType->inner.empty()
                       ? targetType->inner.front()
                       : targetType;
    TypeRef expression = expressionType->kind == TypeRef::Kind::Reference && !expressionType->inner.empty()
                           ? expressionType->inner.front()
                           : expressionType;
    target.isMut = false;
    expression.isMut = false;
    if (target->kind != TypeRef::Kind::Named) {
        return std::nullopt;
    }
    const auto hasVtable = [&](const TypeRef &type) {
        const auto implementation = typeInterfaceVtables.find(type.ToString());
        if (implementation != typeInterfaceVtables.end() && implementation->second.contains(target->name)) {
            return true;
        }
        // A generic extension implements the interface for every instantiation it applies to.
        const auto generic = genericInterfaceImpls.find(NamedBaseTypeName(type));
        return type->kind == TypeRef::Kind::Named && generic != genericInterfaceImpls.end() &&
               interfaceDecls.contains(target->name);
    };
    if (hasVtable(expression)) {
        return expression;
    }
    // A writable slice is viewed through the read-only slice's implementation, as analysis accepted it.
    if (expression.IsWritableSlice()) {
        TypeRef readOnly = expression;
        readOnly = readOnly.WithWritableInner(false);
        if (hasVtable(readOnly)) {
            return readOnly;
        }
    }
    if (expression->kind == TypeRef::Kind::Int && hasVtable(TypeRef::MakeInt64())) {
        return TypeRef::MakeInt64();
    }
    if (expression->kind == TypeRef::Kind::Int64 && hasVtable(TypeRef::MakeInt())) {
        return TypeRef::MakeInt();
    }
    if (expression->kind == TypeRef::Kind::UInt && hasVtable(TypeRef::MakeUInt64())) {
        return TypeRef::MakeUInt64();
    }
    if (expression->kind == TypeRef::Kind::UInt64 && hasVtable(TypeRef::MakeUInt())) {
        return TypeRef::MakeUInt();
    }
    return std::nullopt;
}

HirExprPtr AstToHirContext::LowerExprAs(const Expr &expression, const TypeRef &targetType) {
    // A native destination builds its levels explicitly, by the route analysis accepted.
    if (IsNativeType(targetType) || AstCast<const NativeConstructExpr>(&expression) ||
        AstCast<const NoneExpr>(&expression)) {
        return LowerNativeAs(expression, targetType);
    }

    // A signed literal is one contextual value. Lowering its positive magnitude at the default int width before
    // negating it loses high words, and cannot represent the magnitude of the target's most negative value.
    if (const auto *unary = AstCast<const UnaryExpr>(&expression);
        unary && unary->op == TokenKind::Minus && UnsuffixedIntegerLiteralFits(expression, targetType)) {
        if (const auto *literal = AstCast<const LiteralExpr>(unary->operand.get());
            literal && literal->token.kind == TokenKind::IntLiteral) {
            return CompilerLiteral(expression.location, targetType, "-" + LowerLiteralValue(*literal));
        }
    }

    // Analysis has accepted both arms against this context. Materialize that width before the merge: converting
    // the merged value afterwards cannot recover high words already discarded by an untyped literal's default int.
    if (const auto *ternary = AstCast<const TernaryExpr>(&expression);
        ternary && (targetType.IsNumeric() || targetType.IsBool() || targetType.IsChar()) &&
        !model.HasBorrowedScalarRead(expression)) {
        auto lowered = std::make_unique<HirTernaryExpr>();
        lowered->location = expression.location;
        lowered->type = targetType;
        lowered->condition = LowerExpr(*ternary->condition);
        lowered->thenExpr = ConvertScalar(LowerExprAs(*ternary->thenExpr, targetType), targetType);
        lowered->elseExpr = ConvertScalar(LowerExprAs(*ternary->elseExpr, targetType), targetType);
        return lowered;
    }

    // Every bool width holds the same two values, so a bool literal is written at the width it is wanted at rather
    // than at `bool`'s and converted afterwards.
    if (const auto *literal = AstCast<const LiteralExpr>(&expression);
        literal && literal->token.kind == TokenKind::BoolLiteral && targetType.IsBool()) {
        return CompilerLiteral(expression.location, targetType, literal->token.text);
    }

    if (IsNullLiteral(expression) && targetType->kind == TypeRef::Kind::Pointer) {
        return CompilerLiteral(expression.location, targetType, "0");
    }

    // A range literal accepted against a range type builds each bound at that type's element type.
    if (const auto *range = AstCast<const RangeExpr>(&expression);
        range && targetType.IsRange() && !targetType->inner.empty()) {
        auto lowered = std::make_unique<HirRangeExpr>();
        lowered->location = range->location;
        lowered->inclusive = range->inclusive;
        if (range->lo) {
            lowered->lo = LowerExprAs(*range->lo, targetType->inner.front());
        }
        if (range->hi) {
            lowered->hi = LowerExprAs(*range->hi, targetType->inner.front());
        }
        lowered->type = targetType;
        return lowered;
    }

    // Analysis accepted an unprefixed character literal at the expected width, so it is that width's code unit.
    if (const auto *literal = AstCast<const LiteralExpr>(&expression);
        literal && literal->token.kind == TokenKind::CharLiteral && literal->token.text.starts_with('\'') &&
        targetType.IsChar()) {
        return CompilerLiteral(expression.location, targetType, DecodeCharLiteral(literal->token.text));
    }

    if (const auto *array = AstCast<const ArrayExpr>(&expression);
        array && targetType->kind == TypeRef::Kind::Array && targetType->arrayLength && !targetType->inner.empty()) {
        auto lowered = std::make_unique<HirArrayExpr>();
        lowered->location = array->location;
        lowered->elementType = targetType->inner[0];
        std::vector<HirPartialDropAction> completed;
        for (std::size_t index = 0; index < array->elements.size(); ++index) {
            const auto &element = array->elements[index];
            AppendFailureCleanup(lowered->failureCleanups, completed);
            lowered->elements.push_back(LowerExprAs(*element, targetType->inner[0]));
            if (auto cleanup = PartialCleanup(HirPartialDropAction::Kind::Element, targetType->inner[0], index, {},
                                              element->location)) {
                completed.push_back(std::move(*cleanup));
            }
        }
        lowered->type = targetType;
        return lowered;
    }

    if (const auto *repeat = AstCast<const ArrayRepeatExpr>(&expression);
        repeat && targetType->kind == TypeRef::Kind::Array && targetType->arrayLength && !targetType->inner.empty()) {
        return LowerArrayRepeatAs(*repeat, targetType->inner[0], targetType);
    }

    if (const auto *array = AstCast<const ArrayExpr>(&expression)) {
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

    if (const auto *repeat = AstCast<const ArrayRepeatExpr>(&expression)) {
        if (const auto sliceElement = SliceElementType(targetType)) {
            const TypeRef resolved = ResolvedExpressionType(*repeat);
            const std::uint64_t count = resolved->arrayLength.value_or(0);
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

    if (const auto *tuple = AstCast<const TupleExpr>(&expression);
        tuple && targetType->kind == TypeRef::Kind::Tuple && tuple->elements.size() == targetType->inner.size()) {
        auto lowered = std::make_unique<HirTupleExpr>();
        lowered->location = tuple->location;
        std::vector<HirPartialDropAction> completed;
        for (std::size_t i = 0; i < tuple->elements.size(); ++i) {
            AppendFailureCleanup(lowered->failureCleanups, completed);
            lowered->elements.push_back(
                ConvertScalar(LowerExprAs(*tuple->elements[i], targetType->inner[i]), targetType->inner[i]));
            if (auto cleanup = PartialCleanup(HirPartialDropAction::Kind::TupleElement, targetType->inner[i], i, {},
                                              tuple->elements[i]->location)) {
                completed.push_back(std::move(*cleanup));
            }
        }
        lowered->type = targetType;
        return lowered;
    }

    HirExprPtr lowered = LowerExpr(expression);
    if (targetType->kind == TypeRef::Kind::Reference) {
        const TypeRef &referent = targetType->inner.front();
        TypeRef expressionValueType = lowered->type->kind == TypeRef::Kind::Reference && !lowered->type->inner.empty()
                                        ? lowered->type->inner.front()
                                        : lowered->type;
        TypeRef referentValueType = referent;
        expressionValueType.isMut = false;
        referentValueType.isMut = false;
        if (referent->kind == TypeRef::Kind::Named) {
            if (interfaceDecls.contains(referent->name) && expressionValueType != referentValueType) {
                std::optional<TypeRef> implementationType = InterfaceImplementationType(lowered->type, targetType);
                if (!implementationType) {
                    implementationType =
                        lowered->type->kind == TypeRef::Kind::Reference && !lowered->type->inner.empty()
                            ? lowered->type->inner.front()
                            : lowered->type;
                    implementationType->isMut = false;
                }
                auto coercion = std::make_unique<HirCoerceToInterfaceExpr>();
                coercion->location = expression.location;
                coercion->type = targetType;
                coercion->borrowed = true;
                coercion->vtableLabel = InterfaceVtableLabel(*implementationType, referent->name);
                coercion->value = std::move(lowered);
                return coercion;
            }
        }
        // A concrete reference still needs interface coercion above. An already compatible reference keeps its
        // existing address and only weakens the accepted static borrow type.
        if (lowered->type->kind == TypeRef::Kind::Reference) {
            lowered->type = targetType;
            return lowered;
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
    else if (targetType->kind == TypeRef::Kind::Named) {
        if (interfaceDecls.contains(targetType->name) && lowered->type != targetType) {
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
            const auto interface = interfaceDecls.find(targetType->name);
            if (interface != interfaceDecls.end() && !interface->second->methods.empty()) {
                coercion->vtableLabel = InterfaceVtableLabel(*implementationType, targetType->name);
            }
            // A value stored in a local's interface value is kept in that local's holder for its place and type, which
            // lives as long as the local: storing it destroys the value the holder kept before, and the interface value
            // borrows the holder.
            if (const InterfaceHolderSite *site = model.TryGetInterfaceHolderSite(expression)) {
                if (const auto holder = interfaceHolderBindings.find(InterfaceHolderKey(*site));
                    holder != interfaceHolderBindings.end()) {
                    const auto &[holderName, holderBinding] = holder->second;
                    const TypeRef heldType = lowered->type;
                    const auto named = [&] {
                        auto reference = std::make_unique<HirVarExpr>();
                        reference->location = expression.location;
                        reference->name = holderName;
                        reference->type = heldType;
                        return reference;
                    };
                    auto store = std::make_unique<HirAssignExpr>();
                    store->location = expression.location;
                    store->op = TokenKind::Assign;
                    store->target = named();
                    store->value = std::move(lowered);
                    store->overwriteCleanup = cleanupPlanner.ActionFor(holderBinding);
                    store->type = heldType;
                    auto statement = std::make_unique<HirExprStmt>();
                    statement->location = expression.location;
                    statement->expr = std::move(store);
                    coercion->value = named();
                    coercion->borrowed = true;
                    auto scope = std::make_unique<HirBlockExpr>();
                    scope->location = expression.location;
                    scope->type = targetType;
                    scope->block.location = expression.location;
                    scope->block.stmts.push_back(std::move(statement));
                    scope->value = std::move(coercion);
                    return scope;
                }
            }
            // Any other value with a destructor that an interface value takes over lives in a hidden local of the scope
            // the coercion is written in, which destroys it when the scope ends, as it would a named local; the
            // interface value borrows it. Without one, nothing destroyed what the interface value held.
            const std::string hiddenName = std::format("$interface.{}", interfaceValueOrdinal);
            if (const std::uint64_t bindingId = RegisterCleanupBinding(hiddenName, lowered->type, expression.location);
                bindingId != 0) {
                ++interfaceValueOrdinal;
                auto hidden = std::make_unique<HirLetStmt>();
                hidden->location = expression.location;
                hidden->name = hiddenName;
                hidden->type = lowered->type;
                hidden->bindingId = bindingId;
                auto held = std::make_unique<HirVarExpr>();
                held->location = expression.location;
                held->name = hiddenName;
                held->type = lowered->type;
                hidden->init = std::move(lowered);
                coercion->value = std::move(held);
                coercion->borrowed = true;
                auto scope = std::make_unique<HirBlockExpr>();
                scope->location = expression.location;
                scope->type = targetType;
                scope->block.location = expression.location;
                scope->block.stmts.push_back(std::move(hidden));
                scope->value = std::move(coercion);
                return scope;
            }
            coercion->value = std::move(lowered);
            return coercion;
        }
    }
    if (lowered->type->kind == TypeRef::Kind::Array && lowered->type->arrayLength && !lowered->type->inner.empty() &&
        SliceElementType(targetType)) {
        auto coercion = std::make_unique<HirArrayToSliceExpr>();
        coercion->location = expression.location;
        coercion->type = targetType;
        coercion->elementType = lowered->type->inner[0];
        coercion->length = *lowered->type->arrayLength;
        coercion->value = std::move(lowered);
        return coercion;
    }
    // Semantic analysis has accepted this view conversion. Writability changes only the static element access
    // permission, not the descriptor; preserve its aggregate representation instead of emitting a scalar cast
    // when a writable view is returned through a read-only signature.
    if (lowered->type.IsSlice() && targetType.IsSlice()) {
        lowered->type = targetType;
    }
    return lowered;
}

HirExprPtr AstToHirContext::ConvertScalar(HirExprPtr value, const TypeRef &targetType) {
    // Analysis accepts a scalar where another is expected only when it is assignable there: a bool of any width, an
    // integer to a wider one of its signedness, float32 to float64. A store makes that conversion at the place it
    // writes, but an argument or an arm has no place of its own, and HIR-to-LIR reads it at the type it carries.
    const auto isScalar = [](const TypeRef &type) { return type.IsNumeric() || type.IsBool() || type.IsChar(); };
    if (!value || value->type == targetType || !isScalar(value->type) || !isScalar(targetType)) {
        return value;
    }
    auto cast = std::make_unique<HirCastExpr>();
    cast->location = value->location;
    cast->type = targetType;
    cast->targetType = targetType;
    cast->operand = std::move(value);
    return cast;
}

std::unique_ptr<HirArrayExpr> AstToHirContext::LowerArrayRepeatAs(const ArrayRepeatExpr &expression,
                                                                  const TypeRef &elementType,
                                                                  const TypeRef &arrayType) {
    auto lowered = std::make_unique<HirArrayExpr>();
    lowered->location = expression.location;
    lowered->type = arrayType;
    lowered->elementType = elementType;
    lowered->repeatCount = arrayType->arrayLength.value_or(0);
    lowered->repeatCopyPlan = BuildCopyPlan(elementType);
    lowered->repeatedElement = LowerExprAs(*expression.value, elementType);
    if (const DropGluePlan *glue = model.TryGetDropGlue(elementType)) {
        lowered->repeatElementDropGlue = glue->symbol;
    }
    return lowered;
}

HirExprPtr AstToHirContext::LowerAggregateExpr(const Expr &expression) {
    if (const auto *sizeOf = AstCast<const TypeQueryExpr>(&expression)) {
        return CompilerLiteral(sizeOf->location, ResolvedExpressionType(*sizeOf),
                               std::to_string(ResolvedTypeQuery(*sizeOf)));
    }
    if (const auto *intrinsic = AstCast<const IntrinsicExpr>(&expression)) {
        return LowerIntrinsicExpr(*intrinsic);
    }
    if (const auto *initializer = AstCast<const StructInitExpr>(&expression)) {
        if (const auto [enumDecl, variant] = LookupEnumVariantInitializer(*initializer); enumDecl && variant) {
            if (!variant->namedFields.empty()) {
                auto lowered = std::make_unique<HirEnumConstructExpr>();
                lowered->location = initializer->location;
                lowered->form = enumDecl->IsVariant() ? CaseTypeForm::Variant : CaseTypeForm::Enumeration;
                lowered->type = EnumType(*enumDecl);
                lowered->discriminant = LookupEnumVariantDiscriminant(NominalName(*enumDecl), variant->name).value();
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
            const std::string discriminant =
                LookupEnumVariantDiscriminant(NominalName(*enumDecl), variant->name).value_or("0");
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
            // A field is stored at the width its value carries, so a narrower value is widened to the field's here.
            loweredField.value = ConvertScalar(LowerExprAs(*field.value, fieldType), fieldType);
            lowered->fields.push_back(std::move(loweredField));
            if (auto cleanup =
                    PartialCleanup(HirPartialDropAction::Kind::Field, fieldType, index, field.name, field.location)) {
                completed.push_back(std::move(*cleanup));
            }
        }
        return lowered;
    }
    if (const auto *path = AstCast<const PathExpr>(&expression)) {
        // A module path naming a function, an overload among several included, is that function's address.
        if (const ResolvedCallableBinding *binding = model.TryGetFunctionReferenceBinding(expression)) {
            auto lowered = std::make_unique<HirVarExpr>();
            lowered->location = path->location;
            lowered->type = ResolvedExpressionType(expression);
            lowered->name = binding->linkerName;
            return lowered;
        }
        if (const ConstDecl *constant = model.TryGetAssociatedConstant(expression)) {
            if (const auto *value = model.TryGetConstantValue(*constant)) {
                return CompilerLiteral(path->location, value->type, value->literal);
            }
            if (constant->value) {
                auto value = LowerExpr(*constant->value);
                value->type = ResolvedExpressionType(expression);
                return value;
            }
            return nullptr;
        }
        if (path->segments.size() == 2) {
            TypeRef pathType = ResolvedExpressionType(expression);
            if (pathType->kind == TypeRef::Kind::Func && !pathType->inner.empty())
                pathType = pathType->inner.back();
            const std::string enumName = BaseTypeNameImpl(pathType->name);
            if (HirSymbol *first = currentScope->Lookup(path->segments[0]);
                first && (first->kind == HirSymbol::Kind::Type || first->kind == HirSymbol::Kind::Interface)) {
                if (first->kind == HirSymbol::Kind::Type) {
                    if (const auto discriminant = LookupEnumVariantDiscriminant(enumName, path->segments[1])) {
                        const auto *variant = LookupEnumVariant(enumName, path->segments[1]);
                        if (variant && (!variant->fields.empty() || !variant->namedFields.empty())) {
                            auto lowered = std::make_unique<HirPathExpr>();
                            lowered->location = path->location;
                            lowered->segments = path->segments;
                            lowered->type = EnumVariantConstructorType(*enumDecls.at(enumName), *variant);
                            return lowered;
                        }
                        const EnumDecl &declaration = *enumDecls.at(enumName);
                        if (!declaration.IsVariant()) {
                            return CompilerLiteral(path->location, EnumType(declaration), *discriminant);
                        }
                        auto lowered = std::make_unique<HirEnumConstructExpr>();
                        lowered->location = path->location;
                        lowered->form = CaseTypeForm::Variant;
                        // A generic unit case has the instantiation analysis gave it from its destination.
                        lowered->type = pathType->kind == TypeRef::Kind::Named ? pathType : EnumType(declaration);
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
    if (const auto *range = AstCast<const RangeExpr>(&expression)) {
        auto lowered = std::make_unique<HirRangeExpr>();
        lowered->location = range->location;
        lowered->inclusive = range->inclusive;
        if (range->lo) {
            lowered->lo = LowerExpr(*range->lo);
        }
        if (range->hi) {
            lowered->hi = LowerExpr(*range->hi);
        }
        lowered->type = ResolvedExpressionType(*range);
        // A literal bound takes the range's element type; any other bound is widened to it when the range is built.
        if (!lowered->type->inner.empty() && lowered->type->inner[0].IsInteger()) {
            const TypeRef &element = lowered->type->inner[0];
            if (range->lo && UnsuffixedIntegerLiteralFits(*range->lo, element)) {
                lowered->lo = LowerExprAs(*range->lo, element);
            }
            if (range->hi && UnsuffixedIntegerLiteralFits(*range->hi, element)) {
                lowered->hi = LowerExprAs(*range->hi, element);
            }
        }
        return lowered;
    }
    if (const auto *repeat = AstCast<const ArrayRepeatExpr>(&expression)) {
        const TypeRef type = ResolvedExpressionType(*repeat);
        const TypeRef elementType = type->kind == TypeRef::Kind::Array && !type->inner.empty()
                                      ? type->inner[0]
                                      : ResolvedExpressionType(*repeat->value);
        return LowerArrayRepeatAs(*repeat, elementType, type);
    }
    if (const auto *array = AstCast<const ArrayExpr>(&expression)) {
        auto lowered = std::make_unique<HirArrayExpr>();
        lowered->location = array->location;
        lowered->type = ResolvedExpressionType(*array);
        if (lowered->type->kind == TypeRef::Kind::Array && !lowered->type->inner.empty()) {
            lowered->elementType = lowered->type->inner[0];
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
    if (const auto *tuple = AstCast<const TupleExpr>(&expression)) {
        auto lowered = std::make_unique<HirTupleExpr>();
        lowered->location = tuple->location;
        lowered->type = ResolvedExpressionType(*tuple);
        std::vector<HirPartialDropAction> completed;
        for (std::size_t index = 0; index < tuple->elements.size(); ++index) {
            const auto &element = tuple->elements[index];
            AppendFailureCleanup(lowered->failureCleanups, completed);
            lowered->elements.push_back(LowerExpr(*element));
            const TypeRef elementType =
                index < lowered->type->inner.size() ? lowered->type->inner[index] : lowered->elements.back()->type;
            if (auto cleanup = PartialCleanup(HirPartialDropAction::Kind::TupleElement, elementType, index, {},
                                              element->location)) {
                completed.push_back(std::move(*cleanup));
            }
        }
        return lowered;
    }
    if (const auto *match = AstCast<const MatchExpr>(&expression)) {
        auto lowered = std::make_unique<HirMatchExpr>();
        lowered->location = match->location;
        lowered->type = ResolvedExpressionType(*match);
        lowered->exhaustive = model.IsExhaustiveMatch(*match);
        lowered->subject = LowerMatchSubject(*match->subject);
        // As in the statement form: an arm's bindings own what they took only if the match owns its subject.
        const bool armsOwnPayload = lowered->subject->consumption.has_value();
        for (const auto &arm : match->arms) {
            HirMatchArm loweredArm;
            loweredArm.location = arm.location;
            PushScope();
            const bool savedOwnership = patternBindingsOwnPayload;
            patternBindingsOwnPayload = armsOwnPayload;
            loweredArm.pattern = LowerPattern(*arm.pattern, lowered->subject->type);
            patternBindingsOwnPayload = savedOwnership;
            // An arm converts to the match's type the way analysis accepted it, which materializes a contextual
            // literal at that width; a diverging arm yields nothing.
            const bool scalar = lowered->type.IsNumeric() || lowered->type.IsBool() || lowered->type.IsChar();
            if ((IsNativeType(lowered->type) || scalar) && !AstCast<const DivergeExpr>(arm.body.get())) {
                loweredArm.body = ConvertScalar(LowerExprAs(*arm.body, lowered->type), lowered->type);
            }
            else {
                loweredArm.body = LowerExpr(*arm.body);
            }
            loweredArm.cleanups = CurrentScopeCleanups();
            PopScope();
            lowered->arms.push_back(std::move(loweredArm));
        }
        return lowered;
    }
    return nullptr;
}
} // namespace Rux::AstToHirDetail
