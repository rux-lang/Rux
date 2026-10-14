#include "Lexer/Lexer.h"
#include "Numeric/IntegerLiteral.h"
#include "Semantic/Analysis/AnalysisContext.h"
#include "Semantic/Conditional/ConditionalCompilation.h"
#include "Target/Layout.h"
#include "Target/Target.h"
#include "Types/PrimitiveCatalog.h"
#include "Types/Type.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <charconv>
#include <format>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Rux::SemanticDetail {
using Layout::AlignUp;

void AnalysisContext::CheckFuncDecl(const FuncDecl &d, bool isMethod) {
    if (SkipsReusedFunction(d)) {
        return;
    }
    auto savedTypeParams = currentTypeParams;
    const FuncDecl *savedFunctionDecl = BeginTrackedFunction(d);
    if (!isMethod) {
        currentTypeParams.clear();
        if (IsSpecialOperationName(d.name)) {
            EmitError(d.location,
                      std::format("special operation '{}' may only be declared in an extend block", d.name));
        }
        else if (IsDestructorName(d.name)) {
            EmitError(d.location, std::format("destructor '{}' may only be declared in an extend block", d.name));
        }
    }
    AppendTypeParameterNames(currentTypeParams, d.typeParams);
    const ScopedTypeParameterBounds boundScope(*this, &d.typeParams, !isMethod);

    if (d.returnType) {
        ValidateArrayType(*d.returnType->get());
    }
    TypeRef retType = d.returnType ? ResolveType(*d.returnType->get()) : TypeRef::MakeOpaque();
    if (!retType.IsOpaque() && !retType.IsUnknown()) {
        ValidateStoredType(retType, d.returnType ? d.returnType->get()->location : d.location, "function return type");
    }
    // A function with an explicit '#Abi' is called from foreign code, so its signature is held to the C mapping too.
    const bool foreignAbi = d.callConv != CallingConvention::Default;
    if (foreignAbi && d.returnType) {
        ValidateCAbiType(retType, d.returnType->get()->location,
                         std::format("the return value of '#Abi' function '{}'", d.name));
    }

    auto savedRet = currentReturnType;
    currentReturnType = retType;
    const bool savedNoReturn = currentFunctionNoReturn;
    currentFunctionNoReturn = d.isNoReturn;

    PushScope();

    for (const auto &tp : d.typeParams) {
        Symbol sym;
        sym.kind = Symbol::Kind::Type;
        sym.name = tp.name;
        sym.type = TypeRef::MakeTypeParam(tp.name);
        Define(std::move(sym));
    }

    const TypeRef savedSelfType = DeclareReceiver(d, isMethod);

    bool seenDefault = false;
    for (const auto &param : d.params) {
        if (param.IsReceiver()) {
            continue;
        }
        ValidateArrayType(*param.type);
        if (param.isVariadic) {
            seenDefault = false; // variadic ends fixed params; reset
        }
        else if (param.defaultValue) {
            seenDefault = true;
        }
        else if (seenDefault) {
            EmitError(param.location, std::format("parameter '{}' without a default "
                                                  "value cannot follow a "
                                                  "parameter with a default value",
                                                  param.name));
        }
        Symbol sym;
        sym.kind = Symbol::Kind::Var;
        sym.name = param.name;
        sym.location = param.location;
        sym.type = param.isVariadic ? TypeRef::MakeSlice(ResolveType(*param.type)) : ResolveType(*param.type);
        if (sym.type.kind != TypeRef::Kind::Reference) {
            ValidateStoredType(sym.type, param.location, "function parameter");
        }
        if (foreignAbi) {
            ValidateCAbiType(sym.type, param.location,
                             std::format("parameter '{}' of '#Abi' function '{}'", param.name, d.name));
        }
        // A format string's placeholders are counted in the bytes of a UTF-8 literal, so the attribute belongs only
        // on the parameter type such a literal is passed as.
        if (param.isFormat && !param.isVariadic && !sym.type.IsUnknown() &&
            !(sym.type.IsSlice() && !sym.type.inner.empty() && sym.type.inner.front().kind == TypeRef::Kind::Char8 &&
              !sym.type.inner.front().isMut)) {
            EmitError(param.formatLocation,
                      std::format("'#Format' parameter '{}' has type '{}', but a format string must be 'char8[..]'",
                                  param.name, sym.type.ToString()));
        }
        sym.isMut = false;
        sym.isParameter = true;
        DefineTrackedLocal(std::move(sym), true);
        if (param.defaultValue) {
            TypeRef paramType = ResolveType(*param.type);
            TypeRef defaultType = CheckDefaultValue(**param.defaultValue, d.params);
            if (!defaultType.IsUnknown() && !paramType.IsUnknown() &&
                !CanAssignExprTo(**param.defaultValue, defaultType, paramType)) {
                EmitError(param.location,
                          AssignmentErrorMessage(**param.defaultValue, paramType,
                                                 std::format("default value type '{}' does not "
                                                             "match parameter type '{}'",
                                                             defaultType.ToString(), paramType.ToString())));
            }
        }
    }

    if (d.isAsm) {
        // An asm function's body is raw machine instructions, not Rux
        // statements, so it is validated when the assembler encodes it —
        // except for the one thing the assembler for the target cannot
        // say, which is that the body was written for the other one.
        CheckAsmBodyArchitecture(d);
    }
    else if (!d.body) {
        if (d.intrinsicName.empty() && !(isMethod && (IsSpecialOperationName(d.name) || IsDestructorName(d.name)))) {
            EmitError(d.location, std::format("function '{}' has no body", d.name));
        }
    }
    else if (!checkingDeclarationsOnly || !currentTypeParams.empty()) {
        // A generic body is checked even in a module whose code is reused: other modules instantiate it from what
        // checking it recorded.
        CheckFunctionBody(*d.body, d, retType);
    }

    PopScope();
    currentSelfType = savedSelfType;
    currentReturnType = savedRet;
    currentFunctionNoReturn = savedNoReturn;
    currentTypeParams = savedTypeParams;
    EndTrackedFunction(savedFunctionDecl);
}

// An `asm func` body is written for one architecture, AnalysisContext::and nothing in the
// syntax says which: the mnemonics do. Report the first instruction that
// names an instruction of the architecture the compilation is not for,
// which is the whole body's mistake rather than that one line's.
//
// This runs after `when` folding, so a body a build never reaches is never
// reported, AnalysisContext::and it stops at the first offender so a body written for the
// wrong machine costs one diagnostic rather than one per line. A body that
// reaches this far is one the build needs AnalysisContext::and no assembler can encode, so
// it is an error: `when #target.arch` is how a function written twice
// reaches both machines, AnalysisContext::and every first-party body uses it.
void AnalysisContext::CheckAsmBodyArchitecture(const FuncDecl &d) const {
    const Target::Arch target = context.target.arch;
    for (const auto &instr : d.asmBody) {
        if (instr.mnemonic.empty()) {
            continue; // a label definition
        }
        const Target::Arch mnemonicArch = AsmMnemonicArch(instr.mnemonic);
        if (mnemonicArch == Target::Arch::Unknown || mnemonicArch == target) {
            continue;
        }
        EmitError(instr.location,
                  std::format("'{}' is an {} instruction, but asm func '{}' is compiled for {}", instr.mnemonic,
                              Target::ToDisplayString(mnemonicArch), d.name, Target::ToDisplayString(target)));
        return;
    }
}

void AnalysisContext::CheckStructDecl(const StructDecl &d) {
    auto savedTypeParams = currentTypeParams;
    currentTypeParams = TypeParameterNames(d.typeParams);
    const ScopedTypeParameterBounds boundScope(*this, &d.typeParams);

    PushScope();
    for (const auto &tp : d.typeParams) {
        Symbol sym;
        sym.kind = Symbol::Kind::Type;
        sym.name = tp.name;
        sym.type = TypeRef::MakeTypeParam(tp.name);
        Define(sym);
    }

    std::unordered_set<std::string> seen;
    for (std::size_t i = 0; i < d.fields.size(); ++i) {
        const auto &field = d.fields[i];
        if (!seen.insert(field.name).second) {
            EmitError(field.location, std::format("duplicate field '{}' in struct '{}'", field.name, d.name));
        }
        const auto *array = AstCast<const ArrayTypeExpr>(field.type.get());
        const bool isFlexibleTail = array && !array->size && i + 1 == d.fields.size();
        ValidateArrayType(*field.type, isFlexibleTail);
        ValidateStoredType(ResolveType(*field.type), field.location,
                           std::format("field '{}' in struct '{}'", field.name, d.name));
    }

    PopScope();
    currentTypeParams = savedTypeParams;
}

void AnalysisContext::CheckEnumDecl(const EnumDecl &d) {
    const auto savedTypeParams = currentTypeParams;
    AppendTypeParameterNames(currentTypeParams, d.typeParams);
    const ScopedTypeParameterBounds boundScope(*this, &d.typeParams, /*replaceEnclosing=*/false);
    const TypeRef baseType = EnumBaseType(d);
    if (!d.IsVariant() && !baseType.IsUnknown() && !baseType.IsInteger()) {
        EmitError(d.location, std::format("enum '{}' base type must be an integer type", d.name));
    }
    const std::string_view declarationName = d.IsVariant() ? "variant" : "enum";
    const std::string_view caseName = d.IsVariant() ? "case" : "enumerator";
    std::unordered_set<std::string> seen;
    for (const auto &variant : d.variants) {
        if (!seen.insert(variant.name).second) {
            EmitError(variant.location,
                      std::format("duplicate {} '{}' in {} '{}'", caseName, variant.name, declarationName, d.name));
        }
        if (variant.discriminant && (!variant.fields.empty() || !variant.namedFields.empty())) {
            EmitError(variant.location, std::format("{} '{}::{}' cannot have both fields and a discriminant", caseName,
                                                    d.name, variant.name));
        }
        for (const auto &f : variant.fields) {
            ValidateArrayType(*f);
            ValidateStoredType(
                ResolveType(*f), f->location,
                std::format("payload in {} {} '{}::{}'", declarationName, caseName, d.name, variant.name));
        }
        std::unordered_set<std::string> namedFields;
        for (const auto &f : variant.namedFields) {
            if (!namedFields.insert(f.name).second) {
                EmitError(f.location, std::format("duplicate field '{}' in {} {} '{}::{}'", f.name, declarationName,
                                                  caseName, d.name, variant.name));
            }
            ValidateArrayType(*f.type);
            ValidateStoredType(
                ResolveType(*f.type), f.location,
                std::format("field '{}' in {} {} '{}::{}'", f.name, declarationName, caseName, d.name, variant.name));
        }
    }
    currentTypeParams = savedTypeParams;
}

void AnalysisContext::CheckUnionDecl(const UnionDecl &d) {
    std::unordered_set<std::string> seen;
    for (const auto &field : d.fields) {
        if (!seen.insert(field.name).second) {
            EmitError(field.location, std::format("duplicate field '{}' in union '{}'", field.name, d.name));
        }
        ValidateArrayType(*field.type);
        ValidateStoredType(ResolveType(*field.type), field.location,
                           std::format("field '{}' in union '{}'", field.name, d.name));
    }
}

void AnalysisContext::CheckInterfaceDecl(const InterfaceDecl &d) {
    // `Self` stands for whichever type implements this interface, which is not known here, so it is checked as a
    // type parameter and bound to the implementing type wherever the interface is actually read.
    const auto savedTypeParams = currentTypeParams;
    currentTypeParams.emplace_back(SemanticDetail::SelfTypeName);
    // A generic interface's parameters are named like `Self`: the bound that uses the interface supplies them.
    std::unordered_set<std::string> parameterNames;
    for (const TypeParameter &parameter : d.typeParams) {
        if (!parameterNames.insert(parameter.name).second) {
            EmitError(parameter.location,
                      std::format("interface '{}' repeats type parameter '{}'", d.name, parameter.name));
        }
        if (!parameter.bounds.empty()) {
            EmitError(parameter.location,
                      std::format("type parameter '{}' of interface '{}' cannot have bounds", parameter.name, d.name));
        }
        currentTypeParams.push_back(parameter.name);
    }
    const auto restore = [&] { currentTypeParams = savedTypeParams; };

    std::unordered_set<std::string> seen;
    for (const auto &method : d.methods) {
        if (!seen.insert(method->name).second) {
            EmitError(method->location, std::format("duplicate method '{}' in interface '{}'", method->name, d.name));
        }
        ValidateRequirementReceiver(*method, d.name);
        if (method->returnType) {
            ValidateStoredType(ResolveType(**method->returnType), method->returnType->get()->location,
                               "interface method return type");
        }
        bool seenDefault = false;
        for (const auto &p : method->params) {
            const TypeRef parameterType = ResolveType(*p.type);
            if (parameterType.kind != TypeRef::Kind::Reference) {
                ValidateStoredType(parameterType, p.location, "interface method parameter");
            }
            if (p.defaultValue) {
                seenDefault = true;
                const TypeRef defaultType = CheckDefaultValue(**p.defaultValue, method->params);
                if (!defaultType.IsUnknown() && !parameterType.IsUnknown() &&
                    !CanAssignExprTo(**p.defaultValue, defaultType, parameterType)) {
                    EmitError(p.location, AssignmentErrorMessage(
                                              **p.defaultValue, parameterType,
                                              std::format("default value type '{}' does not match parameter type '{}'",
                                                          defaultType.ToString(), parameterType.ToString())));
                }
            }
            else if (seenDefault && !p.isVariadic) {
                EmitError(
                    p.location,
                    std::format("parameter '{}' without a default value cannot follow a parameter with a default value",
                                p.name));
            }
        }
    }
    restore();
}

TypeRef AnalysisContext::CheckDefaultValue(const Expr &value, const std::vector<Param> &parameters) {
    // A default is evaluated where the call is written, in the caller's scope, so no parameter of the callee exists
    // there to be read. Naming one is reported rather than resolved to whatever the caller has under that name.
    std::unordered_set<std::string> names;
    for (const Param &parameter : parameters) {
        names.insert(parameter.name);
    }
    const auto saved = std::exchange(defaultValueParameters, std::move(names));
    const TypeRef type = CheckExpr(value);
    defaultValueParameters = saved;
    return type;
}

void AnalysisContext::CheckImplDecl(const ImplDecl &d) {
    std::unordered_set<std::string> constantNames;
    const auto owner = declarationInfos.find(&d);
    for (const ImplDecl *previous : implDecls) {
        if (previous == &d)
            break;
        const auto previousOwner = declarationInfos.find(previous);
        if (previous->typeName == d.typeName && owner != declarationInfos.end() &&
            previousOwner != declarationInfos.end() &&
            previousOwner->second.ownerPackage == owner->second.ownerPackage) {
            for (const auto &constant : previous->constants)
                constantNames.insert(constant->name);
        }
    }
    for (const auto &constant : d.constants) {
        if (!constantNames.insert(constant->name).second) {
            EmitError(constant->location, std::format("associated constant '{}' is already declared", constant->name));
        }
        if (!precheckedDeclarations.contains(constant.get()))
            (void)CheckAssociatedConstant(*constant);
    }
    const auto savedTypeParams = currentTypeParams;
    currentTypeParams = ImplTypeParams(d);

    // A compound receiver (e.g. `int[]`) resolves through the type
    // expression rather than a named symbol.
    const std::string typeName = BaseTypeName(d.typeName);
    // An extend block borrows the extended type's parameters, so it borrows their bounds too: a method body passing
    // `T` on to a constrained generic is checked against what the struct declared rather than left unconstrained.
    const ScopedTypeParameterBounds boundScope(*this, AggregateTypeParams(typeName));
    const Symbol *extendedSymbol = currentScope->Lookup(typeName);
    if (d.extendedType) {
        ValidateArrayType(*d.extendedType);
    }
    if (const std::string element = UndefinedSliceElement(d); !element.empty()) {
        EmitError(
            d.location,
            std::format("cannot extend slice type '{}' because element type '{}' is not defined", d.typeName, element),
            {}, "extend a slice of one concrete element type, for example 'extend int[..]'");
        currentTypeParams = savedTypeParams;
        return;
    }
    // A native form or the unit has no declaring package to own a method set or an interface implementation.
    if (const TypeExpr *target = d.extendedType.get();
        target && (IsNativeTypeExpr(*target) || (AstCast<const TupleTypeExpr>(target) &&
                                                 static_cast<const TupleTypeExpr *>(target)->elements.empty()))) {
        EmitError(d.location, std::format("cannot extend native type '{}'", d.typeName),
                  {"a sum, optional, fallible, or unit type has no declaring package to own methods or interface "
                   "implementations"},
                  "write a generic function that takes the native type as a parameter");
        currentTypeParams = savedTypeParams;
        return;
    }
    const bool receiverMayResolve = extendedSymbol != nullptr || !AstCast<const NamedTypeExpr>(d.extendedType.get());
    TypeRef extendedType = d.extendedType && receiverMayResolve ? ResolveType(*d.extendedType) : TypeRef::MakeUnknown();
    const bool isSliceReceiver = extendedType.kind == TypeRef::Kind::Array || extendedType.IsSlice();
    if (!isSliceReceiver && !extendedSymbol) {
        std::optional<std::string> help;
        if (const Symbol *suggestion = currentScope->Suggest(typeName)) {
            help = std::format("did you mean '{}'?", suggestion->name);
        }
        EmitError(d.location, std::format("cannot extend type '{}' because it is not defined", d.typeName), {},
                  std::move(help));
    }
    else if (extendedSymbol && extendedSymbol->kind != Symbol::Kind::Type) {
        EmitError(d.location,
                  std::format("cannot extend '{}' because it is a {}, not a type", d.typeName,
                              SymbolKindName(extendedSymbol->kind)),
                  {DeclarationNote(*extendedSymbol)});
    }

    if (d.interfaceName) {
        Symbol *ifaceSym = currentScope->Lookup(*d.interfaceName);
        if (!ifaceSym) {
            std::optional<std::string> help;
            if (const Symbol *suggestion = currentScope->Suggest(*d.interfaceName)) {
                help = std::format("did you mean '{}'?", suggestion->name);
            }
            EmitError(d.location, std::format("interface '{}' is not defined", *d.interfaceName), {}, std::move(help));
        }
        else if (ifaceSym->kind != Symbol::Kind::Interface) {
            EmitError(
                d.location,
                std::format("name '{}' is a {}, not an interface", *d.interfaceName, SymbolKindName(ifaceSym->kind)),
                {DeclarationNote(*ifaceSym)});
        }
        else {
            // A generic interface is implemented for particular type arguments, and any other is named bare.
            if (const auto *interface = AstCast<const InterfaceDecl>(ifaceSym->declaration)) {
                const std::size_t expected = interface->typeParams.size();
                if (d.interfaceTypeArgs.size() != expected) {
                    EmitError(d.location, expected == 0
                                              ? std::format("interface '{}' takes no type arguments", *d.interfaceName)
                                              : std::format("interface '{}' takes {} type argument{}, but {} {} given",
                                                            *d.interfaceName, expected, expected == 1 ? "" : "s",
                                                            d.interfaceTypeArgs.size(),
                                                            d.interfaceTypeArgs.size() == 1 ? "was" : "were"));
                }
                const auto savedParams = currentTypeParams;
                currentTypeParams = ImplTypeParams(d);
                for (const TypeExprPtr &argument : d.interfaceTypeArgs) {
                    static_cast<void>(ResolveType(*argument));
                }
                currentTypeParams = savedParams;
            }
            std::unordered_set<std::string> implNames;
            for (const auto &m : d.methods) {
                implNames.insert(m->name);
            }
            for (const auto &required : ifaceSym->interfaceMethods) {
                if (!implNames.count(required)) {
                    EmitError(d.location,
                              std::format("implementation of interface '{}' for type '{}' is missing method '{}'",
                                          *d.interfaceName, d.typeName, required),
                              {std::format("interface '{}' requires method '{}'", *d.interfaceName, required)});
                }
            }
            if (const auto *interface = AstCast<const InterfaceDecl>(ifaceSym->declaration)) {
                for (const auto &requirement : interface->methods) {
                    for (const auto &m : d.methods) {
                        if (m->name == requirement->name) {
                            CheckImplementationReceiver(*m, *requirement, *d.interfaceName, d.typeName);
                        }
                    }
                }
            }
        }
    }

    bool savedInImpl = inImpl;
    TypeRef savedSelfType = currentSelfType;
    const ImplDecl *savedImpl = currentImpl;
    TypeRef savedExtendedType = currentExtendedType;
    inImpl = true;
    currentImpl = &d;
    currentExtendedType = extendedType.IsUnknown() ? TypeRef::MakeNamed(d.typeName) : extendedType;
    // Each method replaces this with what its own receiver declares. It stands for the block as a whole: what
    // `self` written as a type resolves to, and what a method that declares no receiver at all would see.
    if (isSliceReceiver) {
        // A slice is a fat pointer already; `self` is the slice value, so
        // `for x in self` and `self[i]` work directly.
        currentSelfType = extendedType;
    }
    else {
        TypeRef selfBase = extendedType.IsUnknown() ? TypeRef::MakeNamed(d.typeName) : extendedType;
        currentSelfType = TypeRef::MakePointer(selfBase);
    }
    for (const auto &m : d.methods) {
        if (const auto typeIt =
                methodsByType.find(extendedType.IsUnknown() ? typeName : NamedBaseTypeName(extendedType));
            typeIt != methodsByType.end()) {
            if (const auto methodIt = typeIt->second.find(m->name); methodIt != typeIt->second.end()) {
                ValidateFunctionSignature(*m, methodIt->second, /*isMethod=*/true);
            }
        }
        ValidateSpecialOperation(*m, currentExtendedType);
        ValidateIndexOperator(*m, currentExtendedType);
        ValidateDestructor(*m, currentExtendedType);
        ValidateConstructor(*m, currentExtendedType);
        CheckFuncDecl(*m, /*isMethod=*/true);
        ValidatePublicFunction(*m, std::format("public method '{}.{}'", typeName, m->name), d.extendedType.get());
    }
    currentSelfType = savedSelfType;
    currentExtendedType = savedExtendedType;
    currentImpl = savedImpl;
    inImpl = savedInImpl;
    currentTypeParams = savedTypeParams;
}

/// The element name of a slice receiver that names nothing in scope, or empty. A slice receiver extends one concrete
/// element type: an element that names nothing would have to be a type parameter, and a slice declares none to
/// borrow, so such a block is reported as what it is rather than as a typo, and only once.
std::string AnalysisContext::UndefinedSliceElement(const ImplDecl &d) const {
    const auto *slice = AstCast<const SliceTypeExpr>(d.extendedType.get());
    const auto *element = slice ? AstCast<const NamedTypeExpr>(slice->element.get()) : nullptr;
    if (!element || !element->typeArgs.empty() || currentScope->Lookup(element->name) ||
        PrimitiveTypeFromName(element->name)) {
        return {};
    }
    return element->name;
}

void AnalysisContext::CheckModuleDecl(const ModuleDecl &d) {
    Scope *savedScope = currentScope;
    currentScope = &ModuleScopeFor(d.name, *currentScope);
    for (const auto &item : d.items) {
        CheckDecl(*item);
    }
    currentScope = savedScope;
}

// An element of a constant array must reduce to a literal, since the array
// is laid out in read-only data rather than evaluated at each use.
bool AnalysisContext::IsConstArrayElement(const Expr &e) const {
    if (const auto *cast = AstCast<const CastExpr>(&e)) {
        return IsConstArrayElement(*cast->operand);
    }
    if (AstCast<const LiteralExpr>(&e)) {
        return true;
    }
    if (const auto *u = AstCast<const UnaryExpr>(&e)) {
        return u->op == TokenKind::Minus && IsConstArrayElement(*u->operand);
    }
    if (const auto *ident = AstCast<const IdentExpr>(&e)) {
        const Symbol *sym = currentScope->Lookup(ident->name);
        return sym && sym->kind == Symbol::Kind::Const;
    }
    return false;
}

// A constant is computed by the compiler and stands for that one value at every use, so nothing in its initializer may
// depend on the running program: a call runs code, and a variable holds a value only once the program runs. A module
// variable's initial value is the bytes its storage starts with, which the compiler writes, so the same rule applies.
bool AnalysisContext::CheckCompileTimeInitializer(const Expr &expression, const Decl &owner) {
    const auto reject = [&](const Expr &part, std::string message) {
        std::string help;
        if (const auto *global = AstCast<const GlobalDecl>(&owner)) {
            help = std::format("initialize '{}' from literals, operators, casts, and constants, and assign it from a "
                               "function at run time",
                               global->name);
        }
        else if (const auto *constant = AstCast<const ConstDecl>(&owner);
                 constant && localConstants.contains(constant)) {
            help = std::format("declare '{}' with 'let' to compute it at run time", constant->name);
        }
        else {
            help = "initialize a constant from literals, operators, casts, and other constants";
        }
        EmitError(part.location, std::move(message), {}, std::move(help));
        return false;
    };
    const auto check = [&](const Expr *part) { return !part || CheckCompileTimeInitializer(*part, owner); };
    const auto checkAll = [&](const std::vector<ExprPtr> &parts) {
        return std::ranges::all_of(parts, [&](const ExprPtr &part) { return check(part.get()); });
    };
    const auto isRuntimeSymbol = [](const Symbol *symbol) { return symbol && symbol->kind == Symbol::Kind::Var; };

    if (AstCast<const LiteralExpr>(&expression) || AstCast<const TypeQueryExpr>(&expression) ||
        AstCast<const IntrinsicExpr>(&expression) || AstCast<const EnumShorthandExpr>(&expression) ||
        AstCast<const NoneExpr>(&expression)) {
        return true;
    }
    if (const auto *identifier = AstCast<const IdentExpr>(&expression)) {
        if (isRuntimeSymbol(currentScope->Lookup(identifier->name))) {
            return reject(expression, std::format("'{}' is not a compile-time constant", identifier->name));
        }
        return true;
    }
    if (const auto *path = AstCast<const PathExpr>(&expression)) {
        // Associated constants, cases, and functions are all compile-time; only a module path can reach a variable.
        const Symbol *current = path->segments.empty() ? nullptr : currentScope->Lookup(path->segments[0]);
        for (std::size_t i = 1; current && i < path->segments.size(); ++i) {
            current = current->kind == Symbol::Kind::Module && current->moduleScope
                        ? current->moduleScope->LookupLocal(path->segments[i])
                        : nullptr;
        }
        if (isRuntimeSymbol(current)) {
            return reject(expression, std::format("'{}' is not a compile-time constant",
                                                  JoinPathSegments(path->segments, 0, path->segments.size())));
        }
        return true;
    }
    if (const auto *unary = AstCast<const UnaryExpr>(&expression)) {
        if (unary->op != TokenKind::Minus && unary->op != TokenKind::Plus && unary->op != TokenKind::Tilde &&
            unary->op != TokenKind::Bang) {
            return reject(expression, "a reference or an address is not a compile-time value");
        }
        return check(unary->operand.get());
    }
    if (const auto *binary = AstCast<const BinaryExpr>(&expression)) {
        return check(binary->left.get()) && check(binary->right.get());
    }
    if (const auto *ternary = AstCast<const TernaryExpr>(&expression)) {
        return check(ternary->condition.get()) && check(ternary->thenExpr.get()) && check(ternary->elseExpr.get());
    }
    if (const auto *cast = AstCast<const CastExpr>(&expression)) {
        return check(cast->operand.get());
    }
    if (const auto *test = AstCast<const IsExpr>(&expression)) {
        return check(test->operand.get());
    }
    if (const auto *range = AstCast<const RangeExpr>(&expression)) {
        return check(range->lo.get()) && check(range->hi.get());
    }
    if (const auto *field = AstCast<const FieldExpr>(&expression)) {
        return check(field->object.get());
    }
    if (const auto *index = AstCast<const IndexExpr>(&expression)) {
        if (IsIndexOperatorCall(*index)) {
            return reject(expression, "a call to an index operator is not a compile-time value");
        }
        return check(index->object.get()) && check(index->index.get());
    }
    if (const auto *initializer = AstCast<const StructInitExpr>(&expression)) {
        return std::ranges::all_of(initializer->fields,
                                   [&](const StructInitExpr::Field &field) { return check(field.value.get()); });
    }
    if (const auto *array = AstCast<const ArrayExpr>(&expression)) {
        return checkAll(array->elements);
    }
    if (const auto *repeat = AstCast<const ArrayRepeatExpr>(&expression)) {
        return check(repeat->value.get());
    }
    if (const auto *tuple = AstCast<const TupleExpr>(&expression)) {
        return checkAll(tuple->elements);
    }
    if (const auto *construct = AstCast<const NativeConstructExpr>(&expression)) {
        return check(construct->operand.get());
    }
    if (const auto *call = AstCast<const CallExpr>(&expression)) {
        // A case with a payload is constructed, not called, and a method of a compiler-supplied value such as
        // `#config.Get` is answered while compiling.
        bool compileTime = AstCast<const EnumShorthandExpr>(call->callee.get()) != nullptr;
        std::string callee = "the function";
        if (const auto *path = AstCast<const PathExpr>(call->callee.get())) {
            const Symbol *first = path->segments.empty() ? nullptr : currentScope->Lookup(path->segments[0]);
            compileTime = path->segments.size() == 2 && first && first->kind == Symbol::Kind::Type &&
                          LookupCase(first->name, path->segments[1]).has_value();
            callee = std::format("'{}'", JoinPathSegments(path->segments, 0, path->segments.size()));
        }
        else if (const auto *identifier = AstCast<const IdentExpr>(call->callee.get())) {
            callee = std::format("'{}'", identifier->name);
        }
        else if (const auto *method = AstCast<const FieldExpr>(call->callee.get())) {
            const auto *object = AstCast<const IdentExpr>(method->object.get());
            const Symbol *root = object ? currentScope->Lookup(object->name) : nullptr;
            compileTime = root && root->kind == Symbol::Kind::Const && !root->intrinsicName.empty();
            callee = std::format("'{}'", method->field);
        }
        if (!compileTime) {
            return reject(*call->callee, std::format("call to {} is not a compile-time value", callee));
        }
        return checkAll(call->args);
    }
    return reject(expression, "expression is not a compile-time value");
}

TypeRef AnalysisContext::CheckNamedConstant(const ConstDecl &declaration) {
    if (const auto checked = checkedConstantTypes.find(&declaration); checked != checkedConstantTypes.end()) {
        return checked->second;
    }
    if (!checkingConstants.insert(&declaration).second) {
        EmitError(declaration.location, std::format("constant '{}' has a cyclic initializer", declaration.name));
        return TypeRef::MakeUnknown();
    }

    // Imports copy the indexed symbol before inference. Read its declaration, not that provisional copy, and
    // resolve the initializer in its owning scope even when a caller reaches it before its declaration is checked.
    Scope *savedScope = currentScope;
    const std::string savedFile = currentFile;
    const std::string savedPackage = currentPackage;
    const auto savedTypeParams = currentTypeParams;
    const FuncDecl *savedFunction = currentFunctionDecl;
    const auto &owner = declarationInfos.at(&declaration);
    currentFile = owner.sourceName;
    currentPackage = owner.ownerPackage;
    currentScope = owner.scope;
    currentTypeParams.clear();
    currentFunctionDecl = nullptr;
    CheckConstDecl(declaration);
    const TypeRef type = currentScope->LookupLocal(declaration.name)->type;
    checkedConstantTypes.emplace(&declaration, type);
    checkingConstants.erase(&declaration);
    currentFunctionDecl = savedFunction;
    currentTypeParams = savedTypeParams;
    currentPackage = savedPackage;
    currentFile = savedFile;
    currentScope = savedScope;
    return type;
}

void AnalysisContext::CheckConstDecl(const ConstDecl &d) {
    // Nothing reads the temporaries of an initializer once it is checked. A statement's are discarded after it, and
    // so are a constant's, or the constants checked outside every function would pile theirs up in one tracker.
    const auto temporaries = moveStates.Mark();
    CheckConstDeclValue(d);
    moveStates.DiscardTemporariesSince(temporaries);
}

void AnalysisContext::CheckConstDeclValue(const ConstDecl &d) {
    if (!d.intrinsicName.empty()) {
        if (!d.type) {
            EmitError(d.location, std::format("'intrinsic' constant '{}' requires a type", d.name));
            return;
        }
        const TypeRef constType = ResolveType(**d.type);
        ValidateStoredType(constType, d.location, "intrinsic constant");
        if (d.name.starts_with('#')) {
            CheckCompilerParameterDeclaration(d, constType);
        }
        if (Symbol *sym = currentScope->Lookup(d.name)) {
            sym->type = constType;
            sym->intrinsicName = d.intrinsicName;
        }
        return;
    }
    if (!d.value) {
        EmitError(d.location, std::format("constant '{}' requires an initializer", d.name));
        return;
    }
    if (d.type) {
        ValidateArrayType(**d.type);
    }
    TypeRef valueType = CheckExpr(*d.value);
    TypeRef constType = d.type ? ResolveType(*d.type->get()) : valueType;
    ValidateStoredType(constType, d.location, "constant");
    if (d.type && !valueType.IsUnknown() && !constType.IsUnknown() &&
        !CanAssignExprTo(*d.value, valueType, constType)) {
        EmitError(d.value->location,
                  AssignmentErrorMessage(*d.value, constType,
                                         std::format("cannot assign '{}' to constant of type '{}'",
                                                     valueType.DisplayString(), constType.ToString())));
    }
    const bool compileTime = valueType.IsUnknown() || CheckCompileTimeInitializer(*d.value, d);
    if (compileTime && (constType.IsSlice() || constType.kind == TypeRef::Kind::Array)) {
        const auto *array = AstCast<const ArrayExpr>(d.value.get());
        const auto *repeat = AstCast<const ArrayRepeatExpr>(d.value.get());
        const bool isText = AstCast<const LiteralExpr>(d.value.get()) != nullptr;
        if (!isText && !array && !repeat) {
            EmitError(d.value->location,
                      "a constant sequence must be initialized with an array literal or a string literal");
        }
        else if (array) {
            for (const auto &element : array->elements) {
                if (!IsConstArrayElement(*element)) {
                    EmitError(element->location, "element of a constant array must be a literal or a "
                                                 "named constant");
                    break;
                }
            }
        }
        else if (repeat && !IsConstArrayElement(*repeat->value)) {
            EmitError(repeat->value->location, "element of a constant array must be a literal or a named constant");
        }
    }
    if (Symbol *sym = currentScope->Lookup(d.name)) {
        sym->type = constType;
    }
}

void AnalysisContext::CheckGlobalDecl(const GlobalDecl &d) {
    ValidateArrayType(*d.type);
    const TypeRef globalType = ResolveType(*d.type);
    ValidateStoredType(globalType, d.location, "module variable");
    // Module storage lives until the process exits and no destructor ever runs on it, so a type whose value owns
    // something that must be released would leak it or, worse, be released by nobody while code still believes it
    // will be.
    if (!globalType.IsUnknown() && MayNeedDestruction(globalType)) {
        EmitError(
            d.type->location,
            std::format("module variable '{}' has type '{}', which needs destruction", d.name, globalType.ToString()),
            {}, "a module variable lives until the program exits and is never destroyed");
    }
    if (globalType.IsSlice()) {
        EmitError(d.type->location,
                  std::format("module variable '{}' cannot have slice type '{}'", d.name, globalType.ToString()), {},
                  "store a fixed array, or a pointer and a length, in the module variable");
    }
    const TypeRef valueType = CheckExpr(*d.value);
    if (!valueType.IsUnknown() && !globalType.IsUnknown() && !CanAssignExprTo(*d.value, valueType, globalType)) {
        EmitError(d.value->location,
                  AssignmentErrorMessage(*d.value, globalType,
                                         std::format("cannot assign '{}' to module variable of type '{}'",
                                                     valueType.DisplayString(), globalType.ToString())));
    }
    const bool compileTime = valueType.IsUnknown() || CheckCompileTimeInitializer(*d.value, d);
    if (compileTime && globalType.kind == TypeRef::Kind::Array) {
        const auto *array = AstCast<const ArrayExpr>(d.value.get());
        const auto *repeat = AstCast<const ArrayRepeatExpr>(d.value.get());
        if (!array && !repeat) {
            EmitError(d.value->location, "a module variable array must be initialized with an array literal");
        }
        else if (array) {
            for (const auto &element : array->elements) {
                if (!IsConstArrayElement(*element)) {
                    EmitError(element->location, "element of a module variable array must be a literal or a "
                                                 "named constant");
                    break;
                }
            }
        }
        else if (!IsConstArrayElement(*repeat->value)) {
            EmitError(repeat->value->location,
                      "element of a module variable array must be a literal or a named constant");
        }
    }
    if (Symbol *symbol = currentScope->LookupLocal(d.name); symbol && symbol->declaration == &d) {
        symbol->type = globalType;
    }
    checkedGlobalTypes.emplace(&d, globalType);
}

std::string AnalysisContext::JoinPathSegments(const std::vector<std::string> &path, std::size_t first,
                                              std::size_t lastExclusive) {
    std::string result;
    for (std::size_t i = first; i < lastExclusive; ++i) {
        if (!result.empty()) {
            result += "::";
        }
        result += path[i];
    }
    return result;
}
} // namespace Rux::SemanticDetail
