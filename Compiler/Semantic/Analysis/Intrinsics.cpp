#include "Semantic/Analysis/AnalysisContext.h"
#include "Semantic/Conditional/ConditionalCompilation.h"
#include "Semantic/Model/CompilerParameters.h"
#include "Types/PrimitiveCatalog.h"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>

namespace Rux::SemanticDetail {
TypeRef AnalysisContext::CheckAssociatedConstant(const ConstDecl &declaration) {
    if (const auto evaluated = evaluatedAssociatedConstants.find(&declaration);
        evaluated != evaluatedAssociatedConstants.end()) {
        return evaluated->second.type;
    }
    if (!checkingAssociatedConstants.insert(&declaration).second) {
        EmitError(declaration.location,
                  std::format("associated constant '{}' has a cyclic initializer", declaration.name));
        return TypeRef::MakeUnknown();
    }
    Scope *savedScope = currentScope;
    const std::string savedFile = currentFile;
    const std::string savedPackage = currentPackage;
    if (const auto owner = declarationInfos.find(&declaration); owner != declarationInfos.end()) {
        currentFile = owner->second.sourceName;
        currentPackage = owner->second.ownerPackage;
        const auto package = packageModuleScopes.find(currentPackage);
        if (package != packageModuleScopes.end()) {
            const auto scope = package->second.find(owner->second.modulePath);
            if (scope != package->second.end()) {
                currentScope = scope->second;
            }
        }
    }
    PushScope();
    Symbol symbol;
    symbol.kind = Symbol::Kind::Const;
    symbol.name = declaration.name;
    Define(symbol);
    CheckConstDecl(declaration);
    const TypeRef type = currentScope->Lookup(declaration.name)->type;
    if (!declaration.intrinsicName.empty() &&
        (!(type.kind == TypeRef::Kind::Float32 || type.kind == TypeRef::Kind::Float64) ||
         (declaration.name != "Infinity" && declaration.name != "NaN"))) {
        EmitError(declaration.location, "intrinsic associated constants support only floating-point Infinity and NaN");
    }
    if (!declaration.intrinsicName.empty()) {
        const auto separator = declaration.intrinsicName.find('.');
        const auto ownerType = PrimitiveTypeFromName(declaration.intrinsicName.substr(0, separator));
        if (!ownerType || *ownerType != type) {
            EmitError(declaration.location, "intrinsic associated constant must have its owning floating-point type");
        }
    }
    if (type.IsInteger() || type.IsFloat() || type.IsBool() || type.IsChar()) {
        std::string literal;
        if (!declaration.intrinsicName.empty()) {
            literal = declaration.name == "Infinity" ? "inf" : "nan";
        }
        else {
            const auto packageModules = [&](const std::string_view name) {
                std::vector<Module *> result;
                if (name == packageName) {
                    // ConditionalEvaluator reads modules without mutating them.
                    for (const Module *module : modules)
                        result.push_back(const_cast<Module *>(module));
                }
                else {
                    for (const auto &package : deps) {
                        if (package.name == name) {
                            for (const auto &entry : package.modules)
                                result.push_back(entry.module);
                        }
                    }
                }
                return result;
            };
            const auto ownerModules = packageModules(currentPackage);
            ConditionalEvaluator evaluator(
                context, ownerModules, [&](const std::string_view alias, const std::string_view source) {
                    return packageModules(
                        ResolvePackageImport(imports, PackageOwningSource(deps, packageName, source), alias));
                });
            evaluator.SetSourceContext(currentFile, {}, {});
            for (const Module *module : ownerModules) {
                if (module->name == currentFile)
                    evaluator.SetImports(*module);
            }
            evaluator.RegisterConstant(declaration);
            const auto evaluation = evaluator.EvaluateConstant(declaration.name);
            if (evaluation.value) {
                const auto &value = *evaluation.value;
                if (const auto *number = std::get_if<std::int64_t>(&value))
                    literal = std::to_string(*number);
                else if (const auto *unsignedNumber = std::get_if<std::uint64_t>(&value))
                    literal = std::to_string(*unsignedNumber);
                else if (const auto *floatingNumber = std::get_if<double>(&value))
                    literal = std::format("{:.17g}", *floatingNumber);
                else if (const auto *boolean = std::get_if<bool>(&value))
                    literal = *boolean ? "true" : "false";
                else if (const auto *wide = std::get_if<CompileTimeWideInteger>(&value)) {
                    literal = (wide->isSigned && wide->bits.IsNegative() ? "-" : "") +
                              wide->bits.Magnitude(wide->isSigned).ToDecimal();
                }
            }
            if (literal.empty()) {
                EmitError(declaration.location,
                          "associated constant initializer is not a supported compile-time value");
            }
        }
        if (!literal.empty())
            evaluatedAssociatedConstants[&declaration] = {type, std::move(literal)};
    }
    PopScope();
    currentScope = savedScope;
    currentFile = savedFile;
    currentPackage = savedPackage;
    checkingAssociatedConstants.erase(&declaration);
    return type;
}

namespace {
/// A parameter of a diagnostic intrinsic: the condition it tests, or the message it reports.
enum class IntrinsicParameter {
    Condition,
    Message,
};

/// An intrinsic function the compiler implements. A `signature` is the one the compiler emits calls for, so a
/// declaration must match it exactly; an empty `parameters` with `checked` false means another validator owns the
/// declaration's shape.
struct IntrinsicFunction {
    std::string_view name;
    bool checked = false;
    std::array<IntrinsicParameter, 2> parameters{};
    std::size_t parameterCount = 0;
    std::string_view signature;
};

constexpr std::array<IntrinsicFunction, 13> kIntrinsicFunctions{{
    {"CheckedAdd", false, {}, 0, {}},
    {"CheckedSub", false, {}, 0, {}},
    {"CheckedMul", false, {}, 0, {}},
    {"Zeroize", false, {}, 0, {}},
    {"Assert",
     true,
     {IntrinsicParameter::Condition, IntrinsicParameter::Message},
     2,
     "(condition: bool, message: char8[..])"},
    {"DebugAssert",
     true,
     {IntrinsicParameter::Condition, IntrinsicParameter::Message},
     2,
     "(condition: bool, message: char8[..])"},
    {"Panic", true, {IntrinsicParameter::Message}, 1, "(message: char8[..])"},
    {"#Error", true, {IntrinsicParameter::Message}, 1, "(message: char8[..])"},
    {"#Warn", true, {IntrinsicParameter::Message}, 1, "(message: char8[..])"},
    // Methods of compiler-supplied values, answered while compiling.
    {"Target.HasFeature", false, {}, 0, {}},
    {"Compiler.HasFeature", false, {}, 0, {}},
    {"Config.Get", false, {}, 0, {}},
    {"Config.Has", false, {}, 0, {}},
}};
} // namespace

void AnalysisContext::ValidateIntrinsicFunction(const FuncDecl &declaration) {
    if (declaration.intrinsicName.empty()) {
        return;
    }
    const auto known =
        std::ranges::find(kIntrinsicFunctions, std::string_view(declaration.intrinsicName), &IntrinsicFunction::name);
    if (known == kIntrinsicFunctions.end()) {
        EmitError(declaration.location,
                  std::format("'{}' is not a supported intrinsic function", declaration.intrinsicName), {},
                  "give the function a body, or declare a foreign function in an 'extern' block");
        return;
    }
    if (!known->checked) {
        return;
    }

    // The compiler emits the call itself, so the declaration is a contract it cannot adapt to: a different parameter
    // type would be passed in the shape the emitted call expects, not the one declared.
    const auto reject = [&](std::string reason) {
        EmitError(declaration.location,
                  std::format("intrinsic '{}' must be declared as 'func {}{}'", declaration.name, declaration.name,
                              known->signature),
                  {std::move(reason)});
    };
    if (declaration.params.size() != known->parameterCount) {
        reject(std::format("it declares {} parameter{}", declaration.params.size(),
                           declaration.params.size() == 1 ? "" : "s"));
        return;
    }
    for (std::size_t index = 0; index < known->parameterCount; ++index) {
        const Param &parameter = declaration.params[index];
        const TypeRef type = ResolveType(*parameter.type);
        const bool matches = known->parameters[index] == IntrinsicParameter::Condition
                               ? type.kind == TypeRef::Kind::Bool8
                               : type.IsSlice() && !type.IsWritableSlice() && !type.inner.empty() &&
                                     type.inner.front().kind == TypeRef::Kind::Char8;
        if (!matches || parameter.isVariadic) {
            reject(std::format("parameter '{}' has type '{}'", parameter.name, type.ToString()));
            return;
        }
    }
    if (declaration.returnType) {
        reject(std::format("it returns '{}'", ResolveType(*declaration.returnType->get()).ToString()));
    }
}

void AnalysisContext::CheckCompilerParameterDeclaration(const ConstDecl &declaration, const TypeRef &type) {
    const CompilerParameterRoot *root = FindCompilerParameterRoot(declaration.intrinsicName);
    if (!root) {
        std::string roots;
        for (const CompilerParameterRoot &candidate : CompilerParameterRoots) {
            roots += std::format("{}'{}'", roots.empty() ? "" : ", ", candidate.name);
        }
        EmitError(declaration.location,
                  std::format("'{}' is not a value the compiler supplies for '{}'", declaration.intrinsicName,
                              declaration.name),
                  {}, std::format("declare '{}' with one of {}", declaration.name, roots));
        return;
    }
    if (type.IsUnknown()) {
        return;
    }
    const Symbol *symbol = currentScope->Lookup(std::string(root->name));
    const auto *structure = symbol ? dynamic_cast<const StructDecl *>(symbol->declaration) : nullptr;
    if (!structure) {
        const auto indexed = structDecls.find(NominalTypeName(std::string(root->name)));
        structure = indexed == structDecls.end() ? nullptr : indexed->second;
    }
    if (!structure) {
        EmitError(declaration.location,
                  std::format("compiler-supplied value '{}' must have struct type '{}'", declaration.name, root->name));
        return;
    }
    for (const StructDecl::Field &field : structure->fields) {
        if (IsSuppliedCompilerParameterField(root->name, field.name)) {
            continue;
        }
        std::string supplied;
        for (const std::string_view name : root->fields) {
            supplied += std::format("{}'{}'", supplied.empty() ? "" : ", ", name);
        }
        EmitError(declaration.location,
                  std::format("field '{}' of '{}' is not one the compiler supplies for '{}'", field.name, root->name,
                              declaration.name),
                  {},
                  supplied.empty() ? std::format("remove the fields of '{}'; the compiler supplies none", root->name)
                                   : std::format("remove '{}'; the compiler supplies {}", field.name, supplied));
    }
}

} // namespace Rux::SemanticDetail
