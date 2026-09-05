#include "Lexer/Lexer.h"
#include "Numeric/IntegerLiteral.h"
#include "Semantic/Analysis/AnalysisContext.h"
#include "Semantic/Conditional/ConditionalCompilation.h"
#include "Target/Layout.h"
#include "Target/Target.h"
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
        sym.isMut = false;
        DefineTrackedLocal(std::move(sym), true);
        if (param.defaultValue) {
            TypeRef paramType = ResolveType(*param.type);
            TypeRef defaultType = CheckExpr(**param.defaultValue);
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
    else {
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
    if (!d.intrinsicName.empty()) {
        CheckIntrinsicType(d);
    }
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
        const auto *array = dynamic_cast<const ArrayTypeExpr *>(field.type.get());
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
    const auto restore = [&] { currentTypeParams = savedTypeParams; };

    std::unordered_set<std::string> seen;
    for (const auto &method : d.methods) {
        if (!seen.insert(method->name).second) {
            EmitError(method->location, std::format("duplicate method '{}' in interface '{}'", method->name, d.name));
        }
        if (method->returnType) {
            ValidateStoredType(ResolveType(**method->returnType), method->returnType->get()->location,
                               "interface method return type");
        }
        for (const auto &p : method->params) {
            if (!p.isVariadic) {
                const TypeRef parameterType = ResolveType(*p.type);
                if (parameterType.kind != TypeRef::Kind::Reference) {
                    ValidateStoredType(parameterType, p.location, "interface method parameter");
                }
            }
        }
    }
    restore();
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
    const bool receiverMayResolve =
        extendedSymbol != nullptr || !dynamic_cast<const NamedTypeExpr *>(d.extendedType.get());
    TypeRef extendedType = d.extendedType && receiverMayResolve ? ResolveType(*d.extendedType) : TypeRef::MakeUnknown();
    const bool isSliceReceiver = extendedType.kind == TypeRef::Kind::Array || (extendedType.isIntrinsicSlice);
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
        if (const auto typeIt = methodsByType.find(
                extendedType.isIntrinsicSlice && currentTypeParams.empty() ? extendedType.name : typeName);
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

void AnalysisContext::CheckModuleDecl(const ModuleDecl &d) {
    Scope *savedScope = currentScope;
    currentScope = &ModuleScopeFor(d.name, *currentScope);
    for (const auto &item : d.items) {
        CheckDecl(*item);
    }
    currentScope = savedScope;
}

bool AnalysisContext::IsSliceTypeRef(const TypeRef &type) {
    return type.isIntrinsicSlice;
}

// An element of a constant array must reduce to a literal, since the array
// is laid out in read-only data rather than evaluated at each use.
bool AnalysisContext::IsConstArrayElement(const Expr &e) const {
    if (dynamic_cast<const LiteralExpr *>(&e)) {
        return true;
    }
    if (const auto *u = dynamic_cast<const UnaryExpr *>(&e)) {
        return u->op == TokenKind::Minus && IsConstArrayElement(*u->operand);
    }
    if (const auto *ident = dynamic_cast<const IdentExpr *>(&e)) {
        const Symbol *sym = currentScope->Lookup(ident->name);
        return sym && sym->kind == Symbol::Kind::Const;
    }
    return false;
}

void AnalysisContext::CheckConstDecl(const ConstDecl &d) {
    if (!d.intrinsicName.empty()) {
        if (!d.type) {
            EmitError(d.location, std::format("'intrinsic' constant '{}' requires a type", d.name));
            return;
        }
        const TypeRef constType = ResolveType(**d.type);
        ValidateStoredType(constType, d.location, "intrinsic constant");
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
        EmitError(d.value->location, AssignmentErrorMessage(*d.value, constType,
                                                            std::format("cannot assign '{}' to constant of type '{}'",
                                                                        valueType.ToString(), constType.ToString())));
    }
    if (IsSliceTypeRef(constType) || constType.kind == TypeRef::Kind::Array) {
        const auto *array = dynamic_cast<const ArrayExpr *>(d.value.get());
        const auto *repeat = dynamic_cast<const ArrayRepeatExpr *>(d.value.get());
        const bool isText = dynamic_cast<const LiteralExpr *>(d.value.get()) != nullptr;
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
