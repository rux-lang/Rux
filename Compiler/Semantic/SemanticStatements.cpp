#include "Semantic/Detail/SemanticAnalyzerContext.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <unordered_set>
#include <utility>

namespace Rux::SemanticDetail {
void SemanticAnalyzerContext::EmitError(const SourceLocation location, std::string message) const {
    diags.push_back({SemanticDiagnostic::Severity::Error, currentFile, location, std::move(message)});
}

void SemanticAnalyzerContext::EmitWarning(const SourceLocation location, std::string message) const {
    diags.push_back({SemanticDiagnostic::Severity::Warning, currentFile, location, std::move(message)});
}

void SemanticAnalyzerContext::PushScope() {
    currentScope = &programIndex.CreateScope(*currentScope);
}

void SemanticAnalyzerContext::PopScope() {
    assert(currentScope->Parent() != nullptr && "cannot pop global scope");
    currentScope = currentScope->Parent();
}

bool SemanticAnalyzerContext::Define(Symbol symbol) const {
    return currentScope->Define(std::move(symbol), diags, currentFile);
}

void SemanticAnalyzerContext::CheckBlock(const Block &block) {
    PushScope();
    for (const auto &statement : block.stmts) {
        CheckStatement(*statement);
    }
    PopScope();
}

void SemanticAnalyzerContext::CheckStatement(const Stmt &statement) {
    if (const auto *expressionStatement = dynamic_cast<const ExprStmt *>(&statement)) {
        CheckExpr(*expressionStatement->expr);
    }
    else if (const auto *letStatement = dynamic_cast<const LetStmt *>(&statement)) {
        if (letStatement->type) {
            ValidateArrayType(**letStatement->type, false);
        }
        TypeRef initializerType = letStatement->init ? CheckExpr(*letStatement->init) : TypeRef::MakeUnknown();
        TypeRef declarationType = letStatement->type ? ResolveType(**letStatement->type) : initializerType;

        if (!letStatement->init && !letStatement->type) {
            EmitError(letStatement->location, "uninitialized variable requires an explicit type");
        }

        if (!letStatement->init && !letStatement->isMut) {
            EmitError(letStatement->location, "immutable variable requires an initializer");
        }

        if (!letStatement->init && letStatement->pattern) {
            EmitError(letStatement->location, "destructuring declaration requires an initializer");
        }

        if (!letStatement->type && declarationType.IsUnknown() && !letStatement->pattern) {
            EmitWarning(letStatement->location, std::format("cannot infer type of '{}'", letStatement->name));
        }

        if (letStatement->init && letStatement->type && !initializerType.IsUnknown() && !declarationType.IsUnknown() &&
            !CanAssignExprTo(*letStatement->init, initializerType, declarationType)) {
            EmitError(letStatement->location,
                      AssignmentErrorMessage(*letStatement->init, declarationType,
                                             std::format("cannot assign '{}' to '{}'", initializerType.ToString(),
                                                         declarationType.ToString())));
        }

        if (letStatement->pattern) {
            CheckLetPattern(*letStatement->pattern, declarationType, letStatement->isMut);
            return;
        }

        Symbol symbol;
        symbol.kind = Symbol::Kind::Var;
        symbol.name = letStatement->name;
        symbol.location = letStatement->location;
        symbol.type = declarationType;
        symbol.isMut = letStatement->isMut;
        Define(std::move(symbol));
    }
    else if (const auto *ifStatement = dynamic_cast<const IfStmt *>(&statement)) {
        TypeRef condition = CheckExpr(*ifStatement->condition);
        if (!condition.IsUnknown() && !condition.IsBool()) {
            EmitError(ifStatement->condition->location, "if condition must be 'bool'");
        }
        CheckBlock(*ifStatement->thenBlock);
        for (const auto &elseIf : ifStatement->elseIfs) {
            TypeRef elseIfCondition = CheckExpr(*elseIf.condition);
            if (!elseIfCondition.IsUnknown() && !elseIfCondition.IsBool()) {
                EmitError(elseIf.condition->location, "if condition must be 'bool'");
            }
            CheckBlock(*elseIf.block);
        }
        if (ifStatement->elseBlock) {
            CheckBlock(*ifStatement->elseBlock);
        }
    }
    else if (const auto *whileStatement = dynamic_cast<const WhileStmt *>(&statement)) {
        if (!whileStatement->label.empty()) {
            activeLabels.insert(whileStatement->label);
        }

        TypeRef condition = CheckExpr(*whileStatement->condition);
        if (!condition.IsUnknown() && !condition.IsBool()) {
            EmitError(whileStatement->condition->location, "while condition must be 'bool'");
        }

        ++loopDepth;
        CheckBlock(*whileStatement->body);
        --loopDepth;
        if (!whileStatement->label.empty()) {
            activeLabels.erase(whileStatement->label);
        }
    }
    else if (const auto *doWhileStatement = dynamic_cast<const DoWhileStmt *>(&statement)) {
        if (!doWhileStatement->label.empty()) {
            activeLabels.insert(doWhileStatement->label);
        }

        ++loopDepth;
        CheckBlock(*doWhileStatement->body);
        --loopDepth;

        TypeRef condition = CheckExpr(*doWhileStatement->condition);
        if (!condition.IsUnknown() && !condition.IsBool()) {
            EmitError(doWhileStatement->condition->location, "do-while condition must be 'bool'");
        }

        if (!doWhileStatement->label.empty()) {
            activeLabels.erase(doWhileStatement->label);
        }
    }
    else if (const auto *loopStatement = dynamic_cast<const LoopStmt *>(&statement)) {
        if (!loopStatement->label.empty()) {
            activeLabels.insert(loopStatement->label);
        }
        ++loopDepth;
        CheckBlock(*loopStatement->body);
        --loopDepth;
        if (!loopStatement->label.empty()) {
            activeLabels.erase(loopStatement->label);
        }
    }
    else if (const auto *forStatement = dynamic_cast<const ForStmt *>(&statement)) {
        TypeRef iterableType = CheckExpr(*forStatement->iterable);
        TypeRef elementType;
        if (iterableType.IsIterableRange() && !iterableType.inner.empty()) {
            elementType = iterableType.inner[0];
        }
        else if (iterableType.IsRange()) {
            EmitError(forStatement->iterable->location,
                      std::format("range type '{}' has no initial value and is not iterable", iterableType.ToString()));
            elementType = TypeRef::MakeUnknown();
        }
        else if (auto sliceElement = IndexElementType(iterableType)) {
            elementType = *sliceElement;
        }
        else {
            elementType = TypeRef::MakeUnknown();
        }

        Symbol *outerVariable = currentScope->Lookup(forStatement->variable);
        const bool reuseOuterVariable = outerVariable != nullptr && outerVariable->kind == Symbol::Kind::Var &&
                                        outerVariable->isMut && !elementType.IsUnknown() &&
                                        outerVariable->type == elementType;
        PushScope();
        if (!reuseOuterVariable) {
            Symbol variable;
            variable.kind = Symbol::Kind::Var;
            variable.name = forStatement->variable;
            variable.location = forStatement->location;
            variable.type = elementType;
            variable.isMut = false;
            Define(std::move(variable));
        }
        if (!forStatement->label.empty()) {
            activeLabels.insert(forStatement->label);
        }
        ++loopDepth;
        CheckBlock(*forStatement->body);
        --loopDepth;
        if (!forStatement->label.empty()) {
            activeLabels.erase(forStatement->label);
        }
        PopScope();
    }
    else if (const auto *matchStatement = dynamic_cast<const MatchStmt *>(&statement)) {
        const TypeRef subjectType = CheckExpr(*matchStatement->subject);
        for (const auto &arm : matchStatement->arms) {
            PushScope();
            CheckPattern(*arm.pattern, subjectType);
            CheckExpr(*arm.body);
            PopScope();
        }
    }
    else if (const auto *returnStatement = dynamic_cast<const ReturnStmt *>(&statement)) {
        if (currentFunctionNoReturn) {
            EmitError(returnStatement->location, "return is not allowed in a '#NoReturn' function");
        }
        if (returnStatement->value) {
            if (TypeRef valueType = CheckExpr(**returnStatement->value);
                !valueType.IsUnknown() && !currentReturnType.IsUnknown() && !currentReturnType.IsOpaque() &&
                !CanAssignExprTo(**returnStatement->value, valueType, currentReturnType)) {
                EmitError(returnStatement->location,
                          AssignmentErrorMessage(**returnStatement->value, currentReturnType,
                                                 std::format("return type mismatch: expected '{}', found '{}'",
                                                             currentReturnType.ToString(), valueType.ToString())));
            }
        }
        else if (!currentReturnType.IsOpaque() && !currentReturnType.IsUnknown()) {
            EmitError(returnStatement->location,
                      std::format("missing return value; expected '{}'", currentReturnType.ToString()));
        }
    }
    else if (const auto *breakStatement = dynamic_cast<const BreakStmt *>(&statement)) {
        if (loopDepth == 0) {
            EmitError(statement.location, "'break' outside of a loop");
        }
        else if (!breakStatement->label.empty() && !activeLabels.contains(breakStatement->label)) {
            EmitError(statement.location, std::format("unknown loop label '{}'", breakStatement->label));
        }
    }
    else if (const auto *continueStatement = dynamic_cast<const ContinueStmt *>(&statement)) {
        if (loopDepth == 0) {
            EmitError(statement.location, "'continue' outside of a loop");
        }
        else if (!continueStatement->label.empty() && !activeLabels.contains(continueStatement->label)) {
            EmitError(statement.location, std::format("unknown loop label '{}'", continueStatement->label));
        }
    }
    else if (const auto *declarationStatement = dynamic_cast<const DeclStmt *>(&statement)) {
        programIndex.CollectDeclaration(*declarationStatement->decl, *currentScope, currentFile,
                                        [this](const TypeExpr &type) { return ResolveType(type); });
        CheckDecl(*declarationStatement->decl);
    }
}

void SemanticAnalyzerContext::CheckLetPattern(const Pattern &pattern, const TypeRef &type, const bool isMutable) {
    if (!type.IsUnknown()) {
        patternTypes.insert_or_assign(&pattern, type);
    }
    if (const auto *identifierPattern = dynamic_cast<const IdentPattern *>(&pattern)) {
        Symbol symbol;
        symbol.kind = Symbol::Kind::Var;
        symbol.name = identifierPattern->name;
        symbol.location = identifierPattern->location;
        symbol.type = type;
        symbol.isMut = isMutable;
        Define(std::move(symbol));
    }
    else if (dynamic_cast<const WildcardPattern *>(&pattern)) {}
    else if (const auto *tuplePattern = dynamic_cast<const TuplePattern *>(&pattern)) {
        if (type.kind != TypeRef::Kind::Tuple) {
            if (!type.IsUnknown()) {
                EmitError(tuplePattern->location,
                          std::format("cannot destructure non-tuple type '{}'", type.ToString()));
            }
            for (const auto &element : tuplePattern->elements) {
                CheckLetPattern(*element, TypeRef::MakeUnknown(), isMutable);
            }
            return;
        }

        if (tuplePattern->elements.size() != type.inner.size()) {
            EmitError(tuplePattern->location,
                      std::format("tuple pattern has {} elements but type '{}' has {}", tuplePattern->elements.size(),
                                  type.ToString(), type.inner.size()));
        }

        const std::size_t count = std::min(tuplePattern->elements.size(), type.inner.size());
        for (std::size_t index = 0; index < count; ++index) {
            CheckLetPattern(*tuplePattern->elements[index], type.inner[index], isMutable);
        }
        for (std::size_t index = count; index < tuplePattern->elements.size(); ++index) {
            CheckLetPattern(*tuplePattern->elements[index], TypeRef::MakeUnknown(), isMutable);
        }
    }
    else {
        EmitError(pattern.location, "unsupported pattern in let binding");
        CheckPattern(pattern, type);
    }
}

void SemanticAnalyzerContext::CheckPattern(const Pattern &pattern, const TypeRef &subjectType) {
    if (!subjectType.IsUnknown()) {
        patternTypes.insert_or_assign(&pattern, subjectType);
    }
    if (const auto *identifierPattern = dynamic_cast<const IdentPattern *>(&pattern)) {
        Symbol symbol;
        symbol.kind = Symbol::Kind::Var;
        symbol.name = identifierPattern->name;
        symbol.location = identifierPattern->location;
        symbol.type = subjectType;
        symbol.isMut = false;
        Define(std::move(symbol));
    }
    else if (const auto *guardedPattern = dynamic_cast<const GuardedPattern *>(&pattern)) {
        CheckPattern(*guardedPattern->inner, subjectType);
        CheckExpr(*guardedPattern->guard);
    }
    else if (const auto *rangePattern = dynamic_cast<const RangePattern *>(&pattern)) {
        CheckPattern(*rangePattern->lo);
        CheckPattern(*rangePattern->hi);
    }
    else if (const auto *tuplePattern = dynamic_cast<const TuplePattern *>(&pattern)) {
        for (std::size_t index = 0; index < tuplePattern->elements.size(); ++index) {
            const TypeRef elementType = subjectType.kind == TypeRef::Kind::Tuple && index < subjectType.inner.size()
                                          ? subjectType.inner[index]
                                          : TypeRef::MakeUnknown();
            CheckPattern(*tuplePattern->elements[index], elementType);
        }
    }
    else if (const auto *structPattern = dynamic_cast<const StructPattern *>(&pattern)) {
        if (!currentScope->Lookup(structPattern->typeName)) {
            EmitError(structPattern->location,
                      std::format("unknown type '{}' in struct pattern", structPattern->typeName));
        }
        for (const auto &field : structPattern->fields) {
            CheckPattern(*field.pattern);
        }
    }
    else if (const auto *enumPattern = dynamic_cast<const EnumPattern *>(&pattern)) {
        std::string enumName;
        std::string variantName;
        const EnumDecl *enumDeclaration = nullptr;

        if (enumPattern->path.size() == 1) {
            variantName = enumPattern->path[0];
            if (subjectType.kind != TypeRef::Kind::Named) {
                EmitError(enumPattern->location,
                          std::format("cannot infer enum type for shorthand pattern '.{}' from subject type '{}'",
                                      variantName, subjectType.ToString()));
            }
            else {
                enumName = BaseTypeName(subjectType.name);
                if (const auto enumIterator = enumDecls.find(enumName); enumIterator != enumDecls.end()) {
                    enumDeclaration = enumIterator->second;
                }
                else {
                    EmitError(enumPattern->location, std::format("type '{}' is not an enum in shorthand pattern '.{}'",
                                                                 subjectType.ToString(), variantName));
                }
            }
        }
        else if (enumPattern->path.size() >= 2) {
            enumName = enumPattern->path[0];
            variantName = enumPattern->path[1];
            if (!currentScope->Lookup(enumName)) {
                EmitError(enumPattern->location, std::format("unknown name '{}' in enum pattern", enumName));
            }
            if (const auto enumIterator = enumDecls.find(enumName); enumIterator != enumDecls.end()) {
                enumDeclaration = enumIterator->second;
            }
        }

        const EnumDecl::Variant *variant =
            enumName.empty() || variantName.empty() ? nullptr : LookupEnumVariant(enumName, variantName);
        if (enumDeclaration && !variant) {
            EmitError(enumPattern->location, std::format("enum '{}' has no variant '{}'", enumName, variantName));
        }

        std::unordered_map<std::string, TypeRef> substitutions;
        if (enumDeclaration) {
            const auto typeArguments = ParseTypeArgsFromTypeName(subjectType.name);
            const auto &parameters = enumDeclaration->typeParams;
            const std::size_t count = std::min(parameters.size(), typeArguments.size());
            for (std::size_t index = 0; index < count; ++index) {
                substitutions.emplace(parameters[index], typeArguments[index]);
            }
        }
        std::unordered_set<std::string> namedArguments;
        for (const auto &argument : enumPattern->namedArgs) {
            if (!namedArguments.insert(argument.name).second) {
                EmitError(argument.location, std::format("duplicate field '{}' in enum pattern", argument.name));
                continue;
            }

            const EnumDecl::Variant::NamedField *field = nullptr;
            if (variant) {
                for (const auto &candidate : variant->namedFields) {
                    if (candidate.name == argument.name) {
                        field = &candidate;
                        break;
                    }
                }
            }

            if (field) {
                CheckLetPattern(*argument.pattern, ResolveTypeWithSubstitution(*field->type, substitutions), false);
            }
            else {
                if (variant) {
                    EmitError(argument.location, std::format("unknown field '{}' in enum pattern", argument.name));
                }
                CheckPattern(*argument.pattern);
            }
        }
        for (std::size_t index = 0; index < enumPattern->args.size(); ++index) {
            if (variant && index < variant->fields.size()) {
                CheckLetPattern(*enumPattern->args[index],
                                ResolveTypeWithSubstitution(*variant->fields[index], substitutions), false);
            }
            else if (variant && index - variant->fields.size() < variant->namedFields.size()) {
                CheckLetPattern(*enumPattern->args[index],
                                ResolveTypeWithSubstitution(*variant->namedFields[index - variant->fields.size()].type,
                                                            substitutions),
                                false);
            }
            else {
                CheckPattern(*enumPattern->args[index]);
            }
        }
    }
}

const EnumDecl::Variant *SemanticAnalyzerContext::LookupEnumVariant(const std::string &enumName,
                                                                    const std::string &variantName) const {
    const auto enumIterator = enumDecls.find(enumName);
    if (enumIterator == enumDecls.end()) {
        return nullptr;
    }
    for (const auto &variant : enumIterator->second->variants) {
        if (variant.name == variantName) {
            return &variant;
        }
    }
    return nullptr;
}
} // namespace Rux::SemanticDetail
