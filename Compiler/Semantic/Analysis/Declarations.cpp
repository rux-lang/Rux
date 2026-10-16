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

void AnalysisContext::CheckMainSignature(const FuncDecl &declaration, const TypeRef &returnType) {
    // Only the executable's own entry point: a `Main` of a dependency, or one inside a module, is an ordinary function.
    if (!context.checksEntryPoint || context.outputKind != OutputKind::Executable || currentPackage != packageName) {
        return;
    }
    if (const auto *info = programIndex.InfoFor(declaration); info == nullptr || !info->modulePath.empty()) {
        return;
    }
    const bool takesNothing = declaration.params.empty();
    // No result is a success that exits with 0, as finishing a fallible `Main` is.
    const bool integer = returnType.IsInteger() || returnType.IsOpaque();
    const bool fallible =
        returnType.IsFallible() && (returnType.FallibleSuccess().IsUnit() || returnType.FallibleSuccess().IsInteger());
    if (returnType.IsUnknown() || (takesNothing && declaration.typeParams.empty() && (integer || fallible))) {
        return;
    }
    const std::string written =
        declaration.returnType ? std::format("'{}'", returnType.DisplayString()) : std::string("no result");
    EmitError(
        declaration.location,
        std::format(
            "'Main' must take no parameters and return nothing, an integer, '! E' or an integer '! E', but it {}{}",
            takesNothing && declaration.typeParams.empty()
                ? ""
                : (declaration.typeParams.empty() ? "takes parameters and " : "is generic and "),
            declaration.returnType ? std::format("returns {}", written) : "has no result"),
        {"the result of 'Main' is the process's exit status, which no other form defines"},
        "declare it as 'func Main() -> int', 'func Main() -> ! E' or 'func Main() -> int ! E'");
}

void AnalysisContext::CheckFuncDecl(const FuncDecl &d, bool isMethod) {
    if (SkipsReusedFunction(d)) {
        ResolveReusedParameters(d);
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
    CheckAbiForTarget(d.callConv, d.abiLocation);
    if (foreignAbi && d.returnType) {
        ValidateCAbiType(retType, d.returnType->get()->location,
                         std::format("the return value of '#Abi' function '{}'", d.name));
    }

    if (!isMethod && d.name == "Main") {
        CheckMainSignature(d, retType);
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
    for (std::size_t index = 0; index < d.params.size(); ++index) {
        const Param &param = d.params[index];
        if (param.IsReceiver()) {
            continue;
        }
        ValidateArrayType(*param.type);
        if (param.isVariadic) {
            seenDefault = false; // variadic ends fixed params; reset
            // The arguments past the fixed ones all fill the variadic parameter, so nothing can follow it.
            if (index + 1 < d.params.size()) {
                EmitError(param.location,
                          std::format("variadic parameter '{}' of '{}' must be its last parameter", param.name, d.name),
                          {"a variadic parameter takes every argument after the fixed ones, so none is left for a "
                           "parameter after it"},
                          std::format("move '{}' to the end of the parameter list", param.name));
            }
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
        if (sym.type->kind != TypeRef::Kind::Reference) {
            ValidateStoredType(sym.type, param.location, "function parameter");
        }
        if (foreignAbi) {
            ValidateCAbiType(sym.type, param.location,
                             std::format("parameter '{}' of '#Abi' function '{}'", param.name, d.name));
        }
        // A format string's placeholders are counted in the bytes of a UTF-8 literal, so the attribute belongs only
        // on the parameter type such a literal is passed as.
        if (param.isFormat && !param.isVariadic && !sym.type.IsUnknown() &&
            !(sym.type.IsSlice() && !sym.type->inner.empty() && sym.type->inner.front()->kind == TypeRef::Kind::Char8 &&
              !sym.type->inner.front().isMut)) {
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
        // A body for the other machine reads its registers as names, so its names are checked only once it is not.
        if (CheckAsmBodyArchitecture(d)) {
            CheckAsmBodySymbols(d);
        }
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
bool AnalysisContext::CheckAsmBodyArchitecture(const FuncDecl &d) const {
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
        return false;
    }
    return true;
}

// A name in an asm body becomes a relocation the linker resolves, so one that names nothing would fail only when the
// program links, or, against a shared library, when it starts. A body's names are its own labels, functions and
// module variables: those have one address the code can reach. A constant, a parameter or a local has none, a
// thread-local variable is reached through the thread pointer, and an extern variable through its import slot.
void AnalysisContext::CheckAsmBodySymbols(const FuncDecl &d) {
    std::unordered_set<std::string> labels;
    for (const auto &instr : d.asmBody) {
        if (!instr.labelDef.empty()) {
            labels.insert(instr.labelDef);
        }
    }
    std::unordered_set<std::string> reported;
    const auto check = [&](const std::string &name, const SourceLocation &location) {
        if (name.empty() || labels.contains(name) || reported.contains(name)) {
            return;
        }
        std::optional<std::string> note;
        std::string help = "define the label in the body, or declare the function or module variable it names";
        if (Symbol *symbol = currentScope->Lookup(name)) {
            const auto *global = AstCast<const GlobalDecl>(symbol->declaration);
            if (symbol->kind == Symbol::Kind::Func || (global && !global->isThreadLocal)) {
                return;
            }
            if (symbol->kind == Symbol::Kind::Const) {
                note = std::format("'{}' is a constant, which has no storage", name);
                help = "write its value as an immediate";
            }
            else if (global) {
                note = std::format("'{}' is a thread-local variable, reached through the thread pointer", name);
                help = "pass its address to the function as an argument";
            }
            else if (AstCast<const ExternVarDecl>(symbol->declaration)) {
                note = std::format("'{}' is an extern variable, reached through the slot the loader fills", name);
                help = "pass its address to the function as an argument";
            }
            else if (symbol->kind == Symbol::Kind::Var) {
                note = std::format("'{}' is a parameter or a local, which has no address of its own", name);
                help = "read a parameter from the register or stack slot the calling convention puts it in";
            }
        }
        reported.insert(name);
        EmitError(location,
                  std::format("'{}' in asm func '{}' is not a label, a function or a module variable", name, d.name),
                  note ? std::vector<std::string>{*note} : std::vector<std::string>{}, help);
    };
    for (const auto &instr : d.asmBody) {
        for (const auto &operand : instr.operands) {
            // An AArch64 condition is written where a symbol could be, as the last operand of `csel`.
            if (operand.kind == AsmOperand::Kind::Sym &&
                !(instr.arch == Target::Arch::AArch64 && IsAArch64ConditionName(operand.name))) {
                check(operand.name, operand.location);
            }
            if (operand.kind == AsmOperand::Kind::Mem) {
                check(operand.memSym, operand.location);
            }
        }
    }
}

void AnalysisContext::CheckAbiForTarget(const CallingConvention convention,
                                        const std::optional<SourceLocation> &location) {
    // AArch64 has one calling convention, so an x86-64 one named on an AArch64 build would be silently ignored.
    if (!location || context.target.arch != Target::Arch::AArch64 ||
        (convention != CallingConvention::Win64 && convention != CallingConvention::SysV)) {
        return;
    }
    const std::string_view name = convention == CallingConvention::Win64 ? "Win64" : "SysV";
    EmitError(*location,
              std::format("'#Abi(.{})' names an x86-64 calling convention, which a build for {} cannot use", name,
                          Target::ToDisplayString(context.target.arch)),
              {"AArch64 has one calling convention, which '#Abi(.C)' names"},
              "write '#Abi(.C)', or keep the x86-64 declaration in a 'when #target.arch' arm");
}

std::optional<std::string> AnalysisContext::FindByValueCycle(const TypeRef &type, const std::string_view target,
                                                             std::unordered_set<std::string> &visiting) {
    const auto inside = [](std::optional<std::string> found, const std::string_view member) {
        if (found) {
            found = found->empty() ? std::string(member) : std::format("{}.{}", member, *found);
        }
        return found;
    };
    // An optional, a fallible and a sum hold their members in place, as a tuple and a fixed array do.
    if (type->kind == TypeRef::Kind::Tuple || type.IsOptional() || type.IsFallible() || type.IsSum() ||
        (type->kind == TypeRef::Kind::Array && type->arrayLength)) {
        for (const TypeRef &member : type->inner) {
            if (auto found = FindByValueCycle(member, target, visiting)) {
                return found;
            }
        }
        return std::nullopt;
    }
    if (type->kind != TypeRef::Kind::Named) {
        return std::nullopt;
    }
    const std::string baseName = BaseTypeName(type->name);
    if (baseName == target) {
        return std::string{};
    }
    if (!visiting.insert(type->name).second) {
        return std::nullopt;
    }
    std::optional<std::string> found;
    std::unordered_map<std::string, TypeRef> substitutions;
    const std::vector<TypeRef> typeArgs = ParseTypeArgsFromTypeName(type->name);
    const auto substitute = [&](const std::vector<TypeParameter> &parameters) {
        for (std::size_t index = 0; index < std::min(parameters.size(), typeArgs.size()); ++index) {
            substitutions[parameters[index].name] = typeArgs[index];
        }
    };
    if (const auto structure = structDecls.find(baseName); structure != structDecls.end()) {
        substitute(structure->second->typeParams);
        for (const auto &field : structure->second->fields) {
            if ((found =
                     inside(FindByValueCycle(ResolveTypeWithSubstitution(*field.type, substitutions), target, visiting),
                            field.name))) {
                break;
            }
        }
    }
    else if (const EnumDecl *enumeration = EnumNamed(baseName)) {
        substitute(enumeration->typeParams);
        for (const auto &variant : enumeration->variants) {
            for (const auto &field : variant.fields) {
                if ((found =
                         inside(FindByValueCycle(ResolveTypeWithSubstitution(*field, substitutions), target, visiting),
                                variant.name))) {
                    break;
                }
            }
            for (const auto &field : variant.namedFields) {
                if (found) {
                    break;
                }
                found =
                    inside(FindByValueCycle(ResolveTypeWithSubstitution(*field.type, substitutions), target, visiting),
                           std::format("{}.{}", variant.name, field.name));
            }
            if (found) {
                break;
            }
        }
    }
    else if (const auto unionType = unionDecls.find(baseName); unionType != unionDecls.end()) {
        for (const auto &field : unionType->second->fields) {
            if ((found = inside(FindByValueCycle(ResolveTypeWithSubstitution(*field.type, {}), target, visiting),
                                field.name))) {
                break;
            }
        }
    }
    visiting.erase(type->name);
    return found;
}

void AnalysisContext::CheckByValueCycle(const Decl &declaration, const std::string_view kind, const std::string &name) {
    const std::string identity = NominalTypeName(name);
    std::unordered_set<std::string> visiting;
    std::optional<std::string> path;
    // The fields are resolved in the declaration's own terms, its type parameters included; what resolving them
    // reports is reported where the declaration itself is checked, so only the cycle is kept from here.
    const std::size_t diagnosticCount = diags.size();
    const auto savedTypeParams = currentTypeParams;
    if (const auto *structure = AstCast<const StructDecl>(&declaration)) {
        currentTypeParams = TypeParameterNames(structure->typeParams);
    }
    else if (const auto *enumeration = AstCast<const EnumDecl>(&declaration)) {
        currentTypeParams = TypeParameterNames(enumeration->typeParams);
    }
    const auto check = [&](const TypeExpr &field, const std::string_view member) {
        if (!path) {
            if (auto found = FindByValueCycle(ResolveType(field), BaseTypeName(identity), visiting)) {
                path = found->empty() ? std::string(member) : std::format("{}.{}", member, *found);
            }
        }
    };
    if (const auto *structure = AstCast<const StructDecl>(&declaration)) {
        for (const auto &field : structure->fields) {
            check(*field.type, field.name);
        }
    }
    else if (const auto *enumeration = AstCast<const EnumDecl>(&declaration)) {
        for (const auto &variant : enumeration->variants) {
            for (const auto &field : variant.fields) {
                check(*field, variant.name);
            }
            for (const auto &field : variant.namedFields) {
                check(*field.type, std::format("{}.{}", variant.name, field.name));
            }
        }
    }
    else if (const auto *unionType = AstCast<const UnionDecl>(&declaration)) {
        for (const auto &field : unionType->fields) {
            check(*field.type, field.name);
        }
    }
    currentTypeParams = savedTypeParams;
    diags.resize(diagnosticCount);
    if (path) {
        EmitError(declaration.location, std::format("{} '{}' contains itself by value through '{}'", kind, name, *path),
                  {"a value that holds itself in place would have no finite size"},
                  std::format("hold it behind a pointer, as '*{0}' or '(*{0})?'", name));
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
    CheckByValueCycle(d, "struct", d.name);
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
    // Every enumerator's value, written or the one after the previous, must be one the backing type holds: a value
    // cut to fit would make two enumerators equal, or name a number no program wrote.
    if (!d.IsVariant() && baseType.IsInteger()) {
        std::optional<std::int64_t> next = 0;
        for (const auto &enumerator : d.variants) {
            std::optional<std::int64_t> value = next;
            // A 64-bit unsigned value past the signed range is checked by its width alone, which it fits.
            bool wideUnsigned = false;
            if (enumerator.discriminant) {
                value = std::nullopt;
                if (const auto parts = SplitIntegerLiteral(*enumerator.discriminant)) {
                    std::string digits;
                    for (const char c : parts->digits) {
                        if (c != '_') {
                            digits.push_back(c);
                        }
                    }
                    std::uint64_t magnitude = 0;
                    const char *end = digits.data() + digits.size();
                    if (const auto [stop, error] =
                            std::from_chars(digits.data(), end, magnitude, static_cast<int>(parts->base));
                        error == std::errc{} && stop == end) {
                        constexpr auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
                        if (!parts->negative && magnitude <= limit) {
                            value = static_cast<std::int64_t>(magnitude);
                        }
                        else if (!parts->negative && !baseType.IsSigned() &&
                                 PrimitiveBits(baseType->kind, 64).value_or(0) >= 64) {
                            wideUnsigned = true;
                        }
                        else if (parts->negative && magnitude <= limit + 1) {
                            value = magnitude == limit + 1 ? std::numeric_limits<std::int64_t>::min()
                                                           : -static_cast<std::int64_t>(magnitude);
                        }
                    }
                }
            }
            const bool fits = value && ConstantFitsTarget(*value, baseType);
            if (!fits && !wideUnsigned && (value || enumerator.discriminant)) {
                EmitError(enumerator.location,
                          value ? std::format("enumerator '{}::{}' has the value {}, which the backing type '{}' "
                                              "cannot hold",
                                              d.name, enumerator.name, *value, baseType.DisplayString())
                                : std::format("enumerator '{}::{}' has a value the backing type '{}' cannot hold",
                                              d.name, enumerator.name, baseType.DisplayString()),
                          {enumerator.discriminant
                               ? "a value outside the backing type's range would be cut to fit"
                               : "an enumerator without '=' takes the value after the previous one"},
                          "give it a value inside the backing type's range, or declare a wider backing type");
            }
            next =
                value && *value < std::numeric_limits<std::int64_t>::max() ? std::optional(*value + 1) : std::nullopt;
        }
    }
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
    if (d.IsVariant()) {
        CheckByValueCycle(d, "variant", d.name);
    }
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
    CheckByValueCycle(d, "union", d.name);
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
        // A requirement names what an implementation provides; there is no default to inherit, so a body would be
        // dropped without a word.
        if (method->body) {
            EmitError(
                method->location,
                std::format("requirement '{}' of interface '{}' cannot have a body", method->name, d.name),
                {"an interface declares what its implementations provide; it supplies no default"},
                std::format("end the requirement with ';', and write the body in each 'extend Type : {}'", d.name));
        }
        if (method->returnType) {
            ValidateStoredType(ResolveType(**method->returnType), method->returnType->get()->location,
                               "interface method return type");
        }
        bool seenDefault = false;
        for (const auto &p : method->params) {
            const TypeRef parameterType = ResolveType(*p.type);
            if (parameterType->kind != TypeRef::Kind::Reference) {
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
    const bool isSliceReceiver = extendedType->kind == TypeRef::Kind::Array || extendedType.IsSlice();
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
                // Every call through the interface is typed by the requirement and reaches this method, so the
                // method has to take and return exactly what the requirement says. A generic block is checked in
                // its instances, through the bounds that use it.
                if (ImplTypeParams(d).empty() && d.extendedType) {
                    const TypeRef receiver = ResolveType(*d.extendedType);
                    std::vector<TypeRef> interfaceArguments;
                    for (const TypeExprPtr &argument : d.interfaceTypeArgs) {
                        interfaceArguments.push_back(ResolveType(*argument));
                    }
                    for (const auto &requirement : interface->methods) {
                        std::optional<SignatureMismatch> mismatch;
                        const FuncDecl *mismatched = nullptr;
                        bool matched = false;
                        for (const auto &m : d.methods) {
                            if (m->name != requirement->name || receiver.IsUnknown()) {
                                continue;
                            }
                            if (auto found = RequirementSignatureMismatch(receiver, *m, *requirement, *interface,
                                                                          interfaceArguments)) {
                                mismatch = std::move(found);
                                mismatched = m.get();
                            }
                            else {
                                matched = true;
                            }
                        }
                        if (!matched && mismatch) {
                            EmitError(mismatched->location,
                                      std::format("method '{}' does not match the requirement of interface '{}'",
                                                  requirement->name, *d.interfaceName),
                                      {std::format("'{}' requires '{}{}'", *d.interfaceName, requirement->name,
                                                   mismatch->required),
                                       std::format("'{}' declares '{}{}'", d.typeName, requirement->name,
                                                   mismatch->declared)},
                                      "declare the method with the parameters and result the interface requires");
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
    if (compileTime && (constType.IsSlice() || constType->kind == TypeRef::Kind::Array)) {
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
    if (compileTime && globalType->kind == TypeRef::Kind::Array) {
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
