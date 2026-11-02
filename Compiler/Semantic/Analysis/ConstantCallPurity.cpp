// What a function a constant calls may not use: raw pointers, `unsafe`, extern functions and variables, assembly,
// thread-local and module variables. Allocators are reached only through raw pointers and interfaces, so they are
// covered by the same rules. The check reads the whole body and every function it calls, not only the path one
// evaluation takes, so whether a function may be called from a constant never depends on its arguments.

#include "Semantic/Analysis/ConstantCallEvaluator.h"

#include <format>

namespace Rux::SemanticDetail {
void AnalysisContext::CheckReusedModuleBodies(const std::string &sourceName) {
    if (declarationOnlyModules == nullptr || !recheckedReusedModules.insert(sourceName).second)
        return;
    const Module *module = nullptr;
    const std::string *owner = nullptr;
    for (const auto &package : deps)
        for (const auto &entry : package.modules)
            if (entry.module->name == sourceName) {
                module = entry.module;
                owner = &package.name;
            }
    for (const Module *candidate : modules)
        if (candidate->name == sourceName) {
            module = candidate;
            owner = &packageName;
        }
    if (module == nullptr || !declarationOnlyModules->contains(module))
        return;
    // A reused module compiled cleanly when its code was recorded, so checking its bodies again only records the
    // facts evaluation reads; anything it would report was reported then.
    const std::size_t reported = diags.size();
    const std::string savedFile = currentFile;
    const std::string savedPackage = currentPackage;
    Scope *const savedScope = currentScope;
    const FuncDecl *const savedFunction = currentFunctionDecl;
    currentFunctionDecl = nullptr;
    currentPackage = *owner;
    checkingReusedBodies = true;
    if (owner == &packageName) {
        currentScope = &globalScope;
        CheckModule(*module);
    }
    else {
        CheckModuleInScope(*module, *packageModuleScopes.at(*owner).at(""));
    }
    checkingReusedBodies = false;
    diags.resize(reported);
    currentFunctionDecl = savedFunction;
    currentScope = savedScope;
    currentPackage = savedPackage;
    currentFile = savedFile;
}

void ConstantCallEvaluator::EnsureBodyChecked(const FuncDecl &function) {
    if (context.declarationOnlyModules == nullptr || !function.typeParams.empty())
        return;
    if (const auto info = context.declarationInfos.find(&function); info != context.declarationInfos.end())
        context.CheckReusedModuleBodies(info->second.sourceName);
}

const std::optional<ConstantCallEvaluator::Offense> &ConstantCallEvaluator::PurityOf(const FuncDecl &function) {
    static const std::optional<Offense> none;
    if (const auto known = purity.find(&function); known != purity.end())
        return known->second;
    // A function that reaches itself is judged by the rest of its body.
    if (!scanning.insert(&function).second)
        return none;
    EnsureBodyChecked(function);
    std::optional<Offense> offense = ScanFunction(function);
    scanning.erase(&function);
    return purity.insert_or_assign(&function, std::move(offense)).first->second;
}

ConstantCallEvaluator::Offense ConstantCallEvaluator::OffenseAt(const FuncDecl &owner, const SourceLocation location,
                                                                std::string description) const {
    Offense offense;
    offense.description = std::format("'{}' {}", FunctionName(owner), description);
    if (const auto info = context.declarationInfos.find(&owner); info != context.declarationInfos.end())
        offense.file = info->second.sourceName;
    offense.location = location;
    offense.chain.push_back(FunctionName(owner));
    return offense;
}

std::optional<ConstantCallEvaluator::Offense> ConstantCallEvaluator::ScanFunction(const FuncDecl &function) {
    if (function.isAsm)
        return OffenseAt(function, function.location, "is written in assembly");
    if (function.isUnsafe)
        return OffenseAt(function, function.unsafeLocation, "is an 'unsafe func'");
    for (const Param &parameter : function.params) {
        const auto type = context.typeNodeTypes.find(parameter.type.get());
        if (type != context.typeNodeTypes.end() && type->second->kind == TypeRef::Kind::Pointer)
            return OffenseAt(function, parameter.location,
                             std::format("takes raw pointer parameter '{}'", parameter.name));
    }
    if (function.returnType) {
        const auto type = context.typeNodeTypes.find(function.returnType->get());
        if (type != context.typeNodeTypes.end() && type->second->kind == TypeRef::Kind::Pointer)
            return OffenseAt(function, (*function.returnType)->location, "returns a raw pointer");
    }
    return function.body ? ScanBlock(*function.body, function) : std::nullopt;
}

std::optional<ConstantCallEvaluator::Offense> ConstantCallEvaluator::ScanBlock(const Block &block,
                                                                               const FuncDecl &owner) {
    for (const StmtPtr &statement : block.stmts)
        if (auto offense = ScanStatement(*statement, owner))
            return offense;
    return std::nullopt;
}

std::optional<ConstantCallEvaluator::Offense> ConstantCallEvaluator::ScanCallee(const Decl *callee, const Expr &site,
                                                                                const FuncDecl &owner) {
    if (const auto *external = AstCast<const ExternFuncDecl>(callee))
        return OffenseAt(owner, site.location, std::format("calls extern function '{}'", external->name));
    const auto *function = AstCast<const FuncDecl>(callee);
    if (function == nullptr)
        return std::nullopt;
    if (function->isUnsafe)
        return OffenseAt(owner, site.location, std::format("calls 'unsafe func' '{}'", function->name));
    if (function->isAsm)
        return OffenseAt(owner, site.location, std::format("calls assembly function '{}'", function->name));
    const auto &inner = PurityOf(*function);
    if (!inner)
        return std::nullopt;
    Offense offense = *inner;
    offense.chain.insert(offense.chain.begin(), FunctionName(owner));
    return offense;
}

std::optional<ConstantCallEvaluator::Offense> ConstantCallEvaluator::ScanStatement(const Stmt &statement,
                                                                                   const FuncDecl &owner) {
    const auto expression = [&](const Expr *part) -> std::optional<Offense> {
        return part ? ScanExpression(*part, owner) : std::nullopt;
    };
    const auto block = [&](const Block *part) -> std::optional<Offense> {
        return part ? ScanBlock(*part, owner) : std::nullopt;
    };
    if (const auto *simple = AstCast<const ExprStmt>(&statement))
        return expression(simple->expr.get());
    if (const auto *let = AstCast<const LetStmt>(&statement)) {
        if (let->type) {
            const auto type = context.typeNodeTypes.find(let->type->get());
            if (type != context.typeNodeTypes.end() && type->second->kind == TypeRef::Kind::Pointer)
                return OffenseAt(owner, let->location, std::format("declares raw pointer '{}'", let->name));
        }
        return expression(let->init.get());
    }
    if (const auto *conditional = AstCast<const IfStmt>(&statement)) {
        if (auto offense = expression(conditional->condition.get()))
            return offense;
        if (auto offense = block(conditional->thenBlock.get()))
            return offense;
        for (const auto &alternative : conditional->elseIfs) {
            if (auto offense = expression(alternative.condition.get()))
                return offense;
            if (auto offense = block(alternative.block.get()))
                return offense;
        }
        return block(conditional->elseBlock.get());
    }
    if (const auto *loop = AstCast<const WhileStmt>(&statement)) {
        if (auto offense = expression(loop->condition.get()))
            return offense;
        return block(loop->body.get());
    }
    if (const auto *loop = AstCast<const DoWhileStmt>(&statement)) {
        if (auto offense = block(loop->body.get()))
            return offense;
        return expression(loop->condition.get());
    }
    if (const auto *loop = AstCast<const LoopStmt>(&statement))
        return block(loop->body.get());
    if (const auto *loop = AstCast<const ForStmt>(&statement)) {
        if (auto offense = expression(loop->iterable.get()))
            return offense;
        if (const auto iteration = context.iterations.find(loop); iteration != context.iterations.end()) {
            for (const FuncDecl *driver : {iteration->second.entry, iteration->second.advance})
                if (driver)
                    if (auto offense = ScanCallee(driver, *loop->iterable, owner))
                        return offense;
        }
        return block(loop->body.get());
    }
    if (const auto *match = AstCast<const MatchStmt>(&statement)) {
        if (auto offense = expression(match->subject.get()))
            return offense;
        for (const auto &arm : match->arms) {
            if (auto offense = ScanPattern(*arm.pattern, owner))
                return offense;
            if (auto offense = expression(arm.body.get()))
                return offense;
        }
        return std::nullopt;
    }
    if (const auto *deferred = AstCast<const DeferStmt>(&statement))
        return deferred->deferredStmt ? ScanStatement(*deferred->deferredStmt, owner) : std::nullopt;
    if (const auto *thrown = AstCast<const FailStmt>(&statement))
        return expression(thrown->value.get());
    if (const auto *attempt = AstCast<const TryStmt>(&statement)) {
        if (auto offense = block(attempt->body.get()))
            return offense;
        return ScanArms(attempt->arms, owner);
    }
    if (const auto *result = AstCast<const ReturnStmt>(&statement))
        return result->value ? expression(result->value->get()) : std::nullopt;
    if (const auto *declaration = AstCast<const DeclStmt>(&statement))
        if (const auto *constant = AstCast<const ConstDecl>(declaration->decl.get()))
            return expression(constant->value.get());
    return std::nullopt;
}

std::optional<ConstantCallEvaluator::Offense> ConstantCallEvaluator::ScanArms(const std::vector<MatchExpr::Arm> &arms,
                                                                              const FuncDecl &owner) {
    for (const auto &arm : arms) {
        if (arm.pattern)
            if (auto offense = ScanPattern(*arm.pattern, owner))
                return offense;
        if (arm.body)
            if (auto offense = ScanExpression(*arm.body, owner))
                return offense;
    }
    return std::nullopt;
}

std::optional<ConstantCallEvaluator::Offense> ConstantCallEvaluator::ScanPattern(const Pattern &pattern,
                                                                                 const FuncDecl &owner) {
    if (const auto *guarded = AstCast<const GuardedPattern>(&pattern)) {
        if (auto offense = ScanPattern(*guarded->inner, owner))
            return offense;
        return ScanExpression(*guarded->guard, owner);
    }
    return std::nullopt;
}

std::optional<ConstantCallEvaluator::Offense> ConstantCallEvaluator::ScanExpression(const Expr &expression,
                                                                                    const FuncDecl &owner) {
    if (const auto type = context.expressionTypes.find(&expression);
        type != context.expressionTypes.end() && type->second->kind == TypeRef::Kind::Pointer)
        return OffenseAt(owner, expression.location,
                         std::format("uses a raw pointer of type '{}'", type->second.ToString()));
    if (const auto global = context.globalReferences.find(&expression); global != context.globalReferences.end()) {
        if (const auto *variable = AstCast<const GlobalDecl>(global->second))
            return OffenseAt(owner, expression.location,
                             std::format("reads {} variable '{}'", variable->isThreadLocal ? "thread-local" : "module",
                                         variable->name));
        const auto *external = AstCast<const ExternVarDecl>(global->second);
        return OffenseAt(owner, expression.location,
                         std::format("reads extern variable '{}'", external ? external->name : "?"));
    }
    const auto scan = [&](const Expr *part) -> std::optional<Offense> {
        return part ? ScanExpression(*part, owner) : std::nullopt;
    };
    const auto scanAll = [&](const std::vector<ExprPtr> &parts) -> std::optional<Offense> {
        for (const ExprPtr &part : parts)
            if (auto offense = scan(part.get()))
                return offense;
        return std::nullopt;
    };
    if (const auto *block = AstCast<const BlockExpr>(&expression)) {
        if (block->isUnsafe)
            return OffenseAt(owner, block->location, "uses an 'unsafe' block");
        return block->block ? ScanBlock(*block->block, owner) : std::nullopt;
    }
    if (const auto *call = AstCast<const CallExpr>(&expression)) {
        if (const auto binding = context.callableBindings.find(call); binding != context.callableBindings.end())
            if (auto offense = ScanCallee(binding->second.selectedDeclaration, *call->callee, owner))
                return offense;
        if (auto offense = scan(call->callee.get()))
            return offense;
        return scanAll(call->args);
    }
    if (const auto *index = AstCast<const IndexExpr>(&expression)) {
        if (const auto method = context.indexOperators.find(index); method != context.indexOperators.end())
            if (auto offense = ScanCallee(method->second.method, expression, owner))
                return offense;
        if (const auto method = context.indexAssignments.find(index); method != context.indexAssignments.end())
            if (auto offense = ScanCallee(method->second.method, expression, owner))
                return offense;
        if (auto offense = scan(index->object.get()))
            return offense;
        return scan(index->index.get());
    }
    if (const auto *unary = AstCast<const UnaryExpr>(&expression))
        return scan(unary->operand.get());
    if (const auto *move = AstCast<const MoveExpr>(&expression))
        return scan(move->operand.get());
    if (const auto *attempt = AstCast<const TryExpr>(&expression))
        return scan(attempt->operand.get());
    if (const auto *postfix = AstCast<const PostfixExpr>(&expression))
        return scan(postfix->operand.get());
    if (const auto *binary = AstCast<const BinaryExpr>(&expression)) {
        if (auto offense = scan(binary->left.get()))
            return offense;
        return scan(binary->right.get());
    }
    if (const auto *assignment = AstCast<const AssignExpr>(&expression)) {
        if (auto offense = scan(assignment->target.get()))
            return offense;
        return scan(assignment->value.get());
    }
    if (const auto *range = AstCast<const RangeExpr>(&expression)) {
        if (auto offense = scan(range->lo.get()))
            return offense;
        return scan(range->hi.get());
    }
    if (const auto *field = AstCast<const FieldExpr>(&expression))
        return scan(field->object.get());
    if (const auto *initializer = AstCast<const StructInitExpr>(&expression)) {
        for (const auto &field : initializer->fields)
            if (auto offense = scan(field.value.get()))
                return offense;
        return std::nullopt;
    }
    if (const auto *array = AstCast<const ArrayExpr>(&expression))
        return scanAll(array->elements);
    if (const auto *repeat = AstCast<const ArrayRepeatExpr>(&expression)) {
        if (auto offense = scan(repeat->value.get()))
            return offense;
        return scan(repeat->count.get());
    }
    if (const auto *spread = AstCast<const SpreadExpr>(&expression))
        return scan(spread->operand.get());
    if (const auto *tuple = AstCast<const TupleExpr>(&expression))
        return scanAll(tuple->elements);
    if (const auto *cast = AstCast<const CastExpr>(&expression))
        return scan(cast->operand.get());
    if (const auto *test = AstCast<const IsExpr>(&expression)) {
        if (test->pattern)
            if (auto offense = ScanPattern(*test->pattern, owner))
                return offense;
        return scan(test->operand.get());
    }
    if (const auto *match = AstCast<const MatchExpr>(&expression)) {
        if (auto offense = scan(match->subject.get()))
            return offense;
        return ScanArms(match->arms, owner);
    }
    if (const auto *construct = AstCast<const NativeConstructExpr>(&expression))
        return scan(construct->operand.get());
    if (const auto *marked = AstCast<const TryMarkExpr>(&expression)) {
        if (auto offense = scan(marked->operand.get()))
            return offense;
        return ScanArms(marked->arms, owner);
    }
    if (const auto *diverge = AstCast<const DivergeExpr>(&expression))
        return scan(diverge->value.get());
    if (const auto *intrinsic = AstCast<const IntrinsicExpr>(&expression))
        return scanAll(intrinsic->args);
    return std::nullopt;
}
} // namespace Rux::SemanticDetail
