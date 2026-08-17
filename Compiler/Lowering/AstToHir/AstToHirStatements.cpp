// Statement and block lowering, including the pattern bindings a `let` or a
// match arm introduces.

#include "Lowering/AstToHir/Detail/AstToHirContext.h"

#include <algorithm>
#include <format>
#include <utility>

namespace Rux::AstToHirDetail {
bool AstToHirContext::IsDiagnosticIntrinsicCall(const Expr &expression) const {
    const auto *call = dynamic_cast<const CallExpr *>(&expression);
    if (!call) {
        return false;
    }
    const auto *identifier = dynamic_cast<const IdentExpr *>(call->callee.get());
    if (!identifier) {
        return false;
    }
    const HirSymbol *symbol = currentScope->Lookup(identifier->name);
    return symbol && (symbol->intrinsicName == "#Error" || symbol->intrinsicName == "#Warn");
}

HirBlock AstToHirContext::LowerBlock(const Block &block) {
    HirBlock loweredBlock;
    loweredBlock.location = block.location;
    PushScope();
    for (const auto &statement : block.stmts) {
        if (const auto *expressionStatement = dynamic_cast<const ExprStmt *>(statement.get());
            expressionStatement && IsDiagnosticIntrinsicCall(*expressionStatement->expr)) {
            continue;
        }
        loweredBlock.stmts.push_back(LowerStmt(*statement));
    }
    PopScope();
    return loweredBlock;
}

HirStmtPtr AstToHirContext::LowerStmt(const Stmt &stmt) {
    if (const auto *statement = dynamic_cast<const ExprStmt *>(&stmt)) {
        auto lowered = std::make_unique<HirExprStmt>();
        lowered->location = statement->location;
        lowered->expr = LowerExpr(*statement->expr);
        return lowered;
    }

    if (const auto *statement = dynamic_cast<const LetStmt *>(&stmt)) {
        auto lowered = std::make_unique<HirLetStmt>();
        lowered->location = statement->location;
        lowered->isMut = statement->isMut;
        lowered->name = statement->name;
        const std::optional<TypeRef> explicitType =
            statement->type ? std::optional<TypeRef>(ResolveType(**statement->type)) : std::nullopt;
        if (statement->init) {
            lowered->init = explicitType ? LowerExprAs(*statement->init, *explicitType) : LowerExpr(*statement->init);
        }
        lowered->type = explicitType ? *explicitType : (lowered->init ? lowered->init->type : TypeRef::MakeUnknown());
        if (statement->pattern) {
            lowered->pattern = LowerLetPattern(*statement->pattern, lowered->type, statement->isMut);
            return lowered;
        }

        HirSymbol symbol;
        symbol.kind = HirSymbol::Kind::Var;
        symbol.name = statement->name;
        symbol.type = lowered->type;
        symbol.isMut = statement->isMut;
        Define(std::move(symbol));
        return lowered;
    }

    if (const auto *statement = dynamic_cast<const IfStmt *>(&stmt)) {
        auto lowered = std::make_unique<HirIfStmt>();
        lowered->location = statement->location;
        lowered->condition = LowerExpr(*statement->condition);
        lowered->thenBlock = LowerBlock(*statement->thenBlock);
        for (const auto &elseIf : statement->elseIfs) {
            HirIfStmt::ElseIf loweredElseIf;
            loweredElseIf.location = elseIf.location;
            loweredElseIf.condition = LowerExpr(*elseIf.condition);
            loweredElseIf.block = LowerBlock(*elseIf.block);
            lowered->elseIfs.push_back(std::move(loweredElseIf));
        }
        if (statement->elseBlock) {
            lowered->elseBlock = LowerBlock(*statement->elseBlock);
        }
        return lowered;
    }

    if (const auto *statement = dynamic_cast<const WhileStmt *>(&stmt)) {
        auto lowered = std::make_unique<HirWhileStmt>();
        lowered->location = statement->location;
        lowered->label = statement->label;
        lowered->condition = LowerExpr(*statement->condition);
        lowered->body = LowerBlock(*statement->body);
        return lowered;
    }

    if (const auto *statement = dynamic_cast<const DoWhileStmt *>(&stmt)) {
        auto lowered = std::make_unique<HirDoWhileStmt>();
        lowered->location = statement->location;
        lowered->label = statement->label;
        lowered->body = LowerBlock(*statement->body);
        lowered->condition = LowerExpr(*statement->condition);
        return lowered;
    }

    if (const auto *statement = dynamic_cast<const LoopStmt *>(&stmt)) {
        auto lowered = std::make_unique<HirLoopStmt>();
        lowered->location = statement->location;
        lowered->label = statement->label;
        lowered->body = LowerBlock(*statement->body);
        return lowered;
    }

    if (const auto *statement = dynamic_cast<const ForStmt *>(&stmt)) {
        auto lowered = std::make_unique<HirForStmt>();
        lowered->location = statement->location;
        lowered->label = statement->label;
        lowered->variable = statement->variable;
        lowered->iterable = LowerExpr(*statement->iterable);
        TypeRef elementType = TypeRef::MakeUnknown();
        if (lowered->iterable->type.IsIterableRange() && !lowered->iterable->type.inner.empty()) {
            elementType = lowered->iterable->type.inner[0];
        }
        else if (const auto sliceElement = IndexElementType(lowered->iterable->type)) {
            elementType = *sliceElement;
        }
        lowered->varType = elementType;
        HirSymbol *outer = currentScope->Lookup(statement->variable);
        lowered->reusesOuterVar =
            outer != nullptr && outer->kind == HirSymbol::Kind::Var && outer->isMut && outer->type == elementType;
        PushScope();
        if (!lowered->reusesOuterVar) {
            HirSymbol variable;
            variable.kind = HirSymbol::Kind::Var;
            variable.name = statement->variable;
            variable.type = elementType;
            Define(std::move(variable));
        }
        lowered->body = LowerBlock(*statement->body);
        PopScope();
        return lowered;
    }

    if (const auto *statement = dynamic_cast<const MatchStmt *>(&stmt)) {
        auto lowered = std::make_unique<HirMatchStmt>();
        lowered->location = statement->location;
        lowered->subject = LowerExpr(*statement->subject);
        for (const auto &arm : statement->arms) {
            HirMatchArm loweredArm;
            loweredArm.location = arm.location;
            PushScope();
            loweredArm.pattern = LowerPattern(*arm.pattern, lowered->subject->type);
            loweredArm.body = LowerExpr(*arm.body);
            PopScope();
            lowered->arms.push_back(std::move(loweredArm));
        }
        return lowered;
    }

    if (const auto *statement = dynamic_cast<const ReturnStmt *>(&stmt)) {
        auto lowered = std::make_unique<HirReturnStmt>();
        lowered->location = statement->location;
        if (statement->value) {
            lowered->value = LowerExprAs(**statement->value, currentReturnType);
        }
        return lowered;
    }

    if (const auto *statement = dynamic_cast<const BreakStmt *>(&stmt)) {
        auto lowered = std::make_unique<HirBreakStmt>();
        lowered->location = stmt.location;
        lowered->label = statement->label;
        return lowered;
    }

    if (const auto *statement = dynamic_cast<const ContinueStmt *>(&stmt)) {
        auto lowered = std::make_unique<HirContinueStmt>();
        lowered->location = stmt.location;
        lowered->label = statement->label;
        return lowered;
    }

    if (const auto *statement = dynamic_cast<const DeclStmt *>(&stmt)) {
        auto lowered = std::make_unique<HirLocalDecl>();
        lowered->location = statement->location;
        CollectDecl(*statement->decl);
        if (const auto *function = dynamic_cast<const FuncDecl *>(statement->decl.get())) {
            lowered->description = std::format("func {}", function->name);
        }
        else if (const auto *constant = dynamic_cast<const ConstDecl *>(statement->decl.get())) {
            lowered->description = std::format("const {}", constant->name);
            HirConst loweredConstant = LowerConst(*constant);
            lowered->hasConstant = true;
            lowered->constantName = std::move(loweredConstant.name);
            lowered->constantType = std::move(loweredConstant.type);
            lowered->constantValue = std::move(loweredConstant.value);
        }
        else if (const auto *alias = dynamic_cast<const TypeAliasDecl *>(statement->decl.get())) {
            lowered->description = std::format("type {}", alias->name);
        }
        else {
            lowered->description = "<local decl>";
        }
        return lowered;
    }

    auto lowered = std::make_unique<HirLocalDecl>();
    lowered->location = stmt.location;
    lowered->description = "<unknown stmt>";
    return lowered;
}

HirPatternPtr AstToHirContext::LowerLetPattern(const Pattern &pattern, const TypeRef &type, bool isMutable) {
    if (dynamic_cast<const WildcardPattern *>(&pattern)) {
        auto lowered = std::make_unique<HirWildcardPattern>();
        lowered->location = pattern.location;
        return lowered;
    }
    if (const auto *identifier = dynamic_cast<const IdentPattern *>(&pattern)) {
        auto lowered = std::make_unique<HirBindingPattern>();
        lowered->location = identifier->location;
        lowered->name = identifier->name;
        lowered->type = type;

        HirSymbol symbol;
        symbol.kind = HirSymbol::Kind::Var;
        symbol.name = identifier->name;
        symbol.type = type;
        symbol.isMut = isMutable;
        Define(std::move(symbol));
        return lowered;
    }
    if (const auto *tuple = dynamic_cast<const TuplePattern *>(&pattern)) {
        auto lowered = std::make_unique<HirTuplePattern>();
        lowered->location = tuple->location;
        for (std::size_t i = 0; i < tuple->elements.size(); ++i) {
            TypeRef elementType = TypeRef::MakeUnknown();
            if (type.kind == TypeRef::Kind::Tuple && i < type.inner.size()) {
                elementType = type.inner[i];
            }
            lowered->elements.push_back(LowerLetPattern(*tuple->elements[i], elementType, isMutable));
        }
        return lowered;
    }
    return LowerPattern(pattern);
}

HirPatternPtr AstToHirContext::LowerPattern(const Pattern &pattern, const TypeRef &subjectType) {
    if (dynamic_cast<const WildcardPattern *>(&pattern)) {
        auto lowered = std::make_unique<HirWildcardPattern>();
        lowered->location = pattern.location;
        return lowered;
    }
    if (const auto *literal = dynamic_cast<const LiteralPattern *>(&pattern)) {
        auto lowered = std::make_unique<HirLiteralPattern>();
        lowered->location = literal->location;
        lowered->type = LiteralType(literal->value);
        if (literal->value.kind == TokenKind::IntLiteral || literal->value.kind == TokenKind::FloatLiteral) {
            lowered->value = StripNumericLiteralSuffix(literal->value.text);
        }
        else {
            lowered->value = literal->value.text;
        }
        return lowered;
    }
    if (const auto *identifier = dynamic_cast<const IdentPattern *>(&pattern)) {
        auto lowered = std::make_unique<HirBindingPattern>();
        lowered->location = identifier->location;
        lowered->name = identifier->name;
        lowered->type = subjectType;
        HirSymbol symbol;
        symbol.kind = HirSymbol::Kind::Var;
        symbol.name = identifier->name;
        symbol.type = subjectType;
        Define(std::move(symbol));
        return lowered;
    }
    if (const auto *range = dynamic_cast<const RangePattern *>(&pattern)) {
        auto lowered = std::make_unique<HirRangePattern>();
        lowered->location = range->location;
        lowered->inclusive = range->inclusive;
        lowered->lo = LowerPattern(*range->lo);
        lowered->hi = LowerPattern(*range->hi);
        return lowered;
    }
    if (const auto *enumPattern = dynamic_cast<const EnumPattern *>(&pattern)) {
        auto lowered = std::make_unique<HirEnumPattern>();
        lowered->location = enumPattern->location;
        lowered->path = enumPattern->path;
        const EnumDecl::Variant *variant = nullptr;
        std::unordered_map<std::string, TypeRef> substitutions;
        std::string enumName;
        std::string variantName;
        if (enumPattern->path.size() == 1 && subjectType.kind == TypeRef::Kind::Named) {
            enumName = BaseTypeName(subjectType.name);
            variantName = enumPattern->path[0];
            lowered->path = {enumName, variantName};
            lowered->resolvedType = subjectType;
        }
        else if (enumPattern->path.size() >= 2) {
            enumName = enumPattern->path[0];
            variantName = enumPattern->path[1];
            if (HirSymbol *symbol = currentScope->Lookup(enumName)) {
                lowered->resolvedType = symbol->type;
            }
        }

        if (!enumName.empty() && !variantName.empty()) {
            lowered->discriminant = LookupEnumVariantDiscriminant(enumName, variantName);
            variant = LookupEnumVariant(enumName, variantName);
            if (variant) {
                lowered->hasPayload = !variant->fields.empty() || !variant->namedFields.empty();
            }
            if (const auto enumIt = enumDecls.find(enumName); enumIt != enumDecls.end()) {
                const auto typeArguments = ParseTypeArgsFromTypeName(subjectType.name);
                const auto &parameters = enumIt->second->typeParams;
                const std::size_t count = std::min(parameters.size(), typeArguments.size());
                for (std::size_t i = 0; i < count; ++i) {
                    substitutions.emplace(parameters[i], typeArguments[i]);
                }
                for (const auto &unitVariant : enumIt->second->variants) {
                    if (unitVariant.fields.empty() && unitVariant.namedFields.empty()) {
                        if (auto discriminant = LookupEnumVariantDiscriminant(enumName, unitVariant.name)) {
                            lowered->unitDiscriminants.push_back(*discriminant);
                        }
                    }
                }
            }
        }

        std::unordered_map<std::string, const Pattern *> namedArguments;
        for (const auto &argument : enumPattern->namedArgs) {
            namedArguments.emplace(argument.name, argument.pattern.get());
        }
        if (variant) {
            for (const auto &field : variant->namedFields) {
                if (const auto it = namedArguments.find(field.name); it != namedArguments.end()) {
                    lowered->argIndices.push_back(&field - variant->namedFields.data());
                    lowered->args.push_back(
                        LowerLetPattern(*it->second, ResolveTypeWithSubstitution(*field.type, substitutions), false));
                }
            }
        }
        else {
            for (const auto &argument : enumPattern->namedArgs) {
                lowered->args.push_back(LowerPattern(*argument.pattern));
            }
        }
        for (std::size_t i = 0; i < enumPattern->args.size(); ++i) {
            if (variant && i < variant->fields.size()) {
                lowered->argIndices.push_back(i);
                lowered->args.push_back(LowerLetPattern(
                    *enumPattern->args[i], ResolveTypeWithSubstitution(*variant->fields[i], substitutions), false));
            }
            else if (variant && i - variant->fields.size() < variant->namedFields.size()) {
                lowered->argIndices.push_back(i);
                lowered->args.push_back(LowerLetPattern(
                    *enumPattern->args[i],
                    ResolveTypeWithSubstitution(*variant->namedFields[i - variant->fields.size()].type, substitutions),
                    false));
            }
            else {
                lowered->args.push_back(LowerPattern(*enumPattern->args[i]));
            }
        }
        return lowered;
    }
    if (const auto *structPattern = dynamic_cast<const StructPattern *>(&pattern)) {
        auto lowered = std::make_unique<HirStructPattern>();
        lowered->location = structPattern->location;
        lowered->typeName = structPattern->typeName;
        if (HirSymbol *symbol = currentScope->Lookup(structPattern->typeName)) {
            lowered->resolvedType = symbol->type;
        }
        for (const auto &field : structPattern->fields) {
            HirStructPatternField loweredField;
            loweredField.name = field.name;
            loweredField.pattern = LowerPattern(*field.pattern);
            lowered->fields.push_back(std::move(loweredField));
        }
        return lowered;
    }
    if (const auto *tuple = dynamic_cast<const TuplePattern *>(&pattern)) {
        auto lowered = std::make_unique<HirTuplePattern>();
        lowered->location = tuple->location;
        for (std::size_t i = 0; i < tuple->elements.size(); ++i) {
            const TypeRef elementType = subjectType.kind == TypeRef::Kind::Tuple && i < subjectType.inner.size()
                                          ? subjectType.inner[i]
                                          : TypeRef::MakeUnknown();
            lowered->elements.push_back(LowerPattern(*tuple->elements[i], elementType));
        }
        return lowered;
    }
    if (const auto *guarded = dynamic_cast<const GuardedPattern *>(&pattern)) {
        auto lowered = std::make_unique<HirGuardedPattern>();
        lowered->location = guarded->location;
        lowered->inner = LowerPattern(*guarded->inner, subjectType);
        lowered->guard = LowerExpr(*guarded->guard);
        return lowered;
    }
    auto lowered = std::make_unique<HirWildcardPattern>();
    lowered->location = pattern.location;
    return lowered;
}
} // namespace Rux::AstToHirDetail
