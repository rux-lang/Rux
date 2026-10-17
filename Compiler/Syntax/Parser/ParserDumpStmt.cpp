// Block, statement, and pattern formatting for the human-readable AST dump.

#include "Syntax/Parser/Detail/AstDumpWriter.h"

#include <ostream>
#include <utility>

namespace Rux::ParserDumpDetail {
StatementPrinter::StatementPrinter(AstDumpWriter &inputWriter, ExpressionCallback expressionCallback,
                                   DeclarationCallback declarationCallback)
    : writer(inputWriter)
    , out(inputWriter.out)
    , indent(inputWriter.indent)
    , printExpression(std::move(expressionCallback))
    , printDeclaration(std::move(declarationCallback)) {
}

void StatementPrinter::Pad() const {
    writer.Pad();
}

void StatementPrinter::PrintBlock(const Block &block) {
    Pad();
    out << "Block [" << block.stmts.size() << " stmt" << (block.stmts.size() == 1 ? "" : "s") << "]\n";
    ++indent;
    for (const auto &statement : block.stmts) {
        if (statement) {
            Print(*statement);
        }
    }
    --indent;
}

void StatementPrinter::Print(const Stmt &statement) {
    if (const auto *let = AstCast<const LetStmt>(&statement)) {
        PrintLetStmt(*let);
    }
    else if (const auto *ifStatement = AstCast<const IfStmt>(&statement)) {
        PrintIfStmt(*ifStatement);
    }
    else if (const auto *whileStatement = AstCast<const WhileStmt>(&statement)) {
        PrintWhileStmt(*whileStatement);
    }
    else if (const auto *forStatement = AstCast<const ForStmt>(&statement)) {
        PrintForStmt(*forStatement);
    }
    else if (const auto *matchStatement = AstCast<const MatchStmt>(&statement)) {
        PrintMatchStmt(*matchStatement);
    }
    else if (const auto *returnStatement = AstCast<const ReturnStmt>(&statement)) {
        PrintReturnStmt(*returnStatement);
    }
    else if (const auto *failStatement = AstCast<const FailStmt>(&statement)) {
        Pad();
        out << "FailStmt\n";
        if (failStatement->value) {
            ++indent;
            printExpression(*failStatement->value);
            --indent;
        }
    }
    else if (const auto *deferStatement = AstCast<const DeferStmt>(&statement)) {
        PrintDeferStmt(*deferStatement);
    }
    else if (AstCast<const BreakStmt>(&statement)) {
        Pad();
        out << "BreakStmt\n";
    }
    else if (AstCast<const ContinueStmt>(&statement)) {
        Pad();
        out << "ContinueStmt\n";
    }
    else if (const auto *expressionStatement = AstCast<const ExprStmt>(&statement)) {
        Pad();
        out << "ExprStmt\n";
        ++indent;
        if (expressionStatement->expr) {
            printExpression(*expressionStatement->expr);
        }
        --indent;
    }
    else if (const auto *declarationStatement = AstCast<const DeclStmt>(&statement)) {
        if (declarationStatement->decl) {
            printDeclaration(*declarationStatement->decl);
        }
    }
}

void StatementPrinter::PrintLetStmt(const LetStmt &statement) {
    Pad();
    out << "LetStmt '";
    if (statement.pattern) {
        out << "<pattern>";
    }
    else {
        out << statement.name;
    }
    out << "' (" << (statement.isMut ? "var" : "let") << ")";
    if (statement.type) {
        out << " : " << DeclarationPrinter::TypeString(statement.type->get());
    }
    out << '\n';
    ++indent;
    if (statement.pattern) {
        PrintPattern(*statement.pattern);
    }
    if (statement.init) {
        printExpression(*statement.init);
    }
    --indent;
}

void StatementPrinter::PrintIfStmt(const IfStmt &statement) {
    Pad();
    out << (statement.isCompileTime ? "WhenStmt\n" : "IfStmt\n");
    ++indent;

    Pad();
    out << "Condition\n";
    ++indent;
    if (statement.condition) {
        printExpression(*statement.condition);
    }
    --indent;

    Pad();
    out << "Then\n";
    ++indent;
    if (statement.thenBlock) {
        PrintBlock(*statement.thenBlock);
    }
    --indent;

    for (const auto &elseIf : statement.elseIfs) {
        Pad();
        out << "ElseIf\n";
        ++indent;
        Pad();
        out << "Condition\n";
        ++indent;
        if (elseIf.condition) {
            printExpression(*elseIf.condition);
        }
        --indent;
        if (elseIf.block) {
            PrintBlock(*elseIf.block);
        }
        --indent;
    }

    if (statement.elseBlock) {
        Pad();
        out << "Else\n";
        ++indent;
        PrintBlock(*statement.elseBlock);
        --indent;
    }
    --indent;
}

void StatementPrinter::PrintWhileStmt(const WhileStmt &statement) {
    Pad();
    out << "WhileStmt\n";
    ++indent;
    Pad();
    out << "Condition\n";
    ++indent;
    if (statement.condition) {
        printExpression(*statement.condition);
    }
    --indent;
    if (statement.body) {
        PrintBlock(*statement.body);
    }
    --indent;
}

void StatementPrinter::PrintForStmt(const ForStmt &statement) {
    Pad();
    out << "ForStmt '" << statement.variable << "' in\n";
    ++indent;
    if (statement.iterable) {
        printExpression(*statement.iterable);
    }
    if (statement.body) {
        PrintBlock(*statement.body);
    }
    --indent;
}

void StatementPrinter::PrintMatchStmt(const MatchStmt &statement) {
    Pad();
    out << "MatchStmt\n";
    ++indent;
    Pad();
    out << "Subject\n";
    ++indent;
    if (statement.subject) {
        printExpression(*statement.subject);
    }
    --indent;
    for (const auto &arm : statement.arms) {
        Pad();
        out << "Arm\n";
        ++indent;
        if (arm.pattern) {
            PrintPattern(*arm.pattern);
        }
        if (arm.body) {
            printExpression(*arm.body);
        }
        --indent;
    }
    --indent;
}

void StatementPrinter::PrintReturnStmt(const ReturnStmt &statement) {
    Pad();
    out << "ReturnStmt\n";
    if (statement.value) {
        ++indent;
        printExpression(**statement.value);
        --indent;
    }
}

void StatementPrinter::PrintDeferStmt(const DeferStmt &statement) {
    Pad();
    out << "DeferStmt\n";
    if (statement.deferredStmt) {
        ++indent;
        Print(*statement.deferredStmt);
        --indent;
    }
}

void StatementPrinter::PrintPattern(const Pattern &pattern) {
    if (AstCast<const WildcardPattern>(&pattern)) {
        Pad();
        out << "WildcardPattern\n";
    }
    else if (const auto *literal = AstCast<const LiteralPattern>(&pattern)) {
        Pad();
        out << "LiteralPattern '" << literal->value.text << "'\n";
    }
    else if (const auto *identifier = AstCast<const IdentPattern>(&pattern)) {
        Pad();
        out << "IdentPattern '" << identifier->name << "'\n";
    }
    else if (const auto *range = AstCast<const RangePattern>(&pattern)) {
        Pad();
        out << "RangePattern " << (range->inclusive ? "..." : "..") << '\n';
        ++indent;
        if (range->lo) {
            PrintPattern(*range->lo);
        }
        if (range->hi) {
            PrintPattern(*range->hi);
        }
        --indent;
    }
    else if (const auto *enumeration = AstCast<const EnumPattern>(&pattern)) {
        Pad();
        out << "EnumPattern '";
        if (enumeration->path.size() == 1) {
            out << '.';
        }
        for (std::size_t index = 0; index < enumeration->path.size(); ++index) {
            if (index) {
                out << '.';
            }
            out << enumeration->path[index];
        }
        out << "'";
        if (!enumeration->args.empty()) {
            out << " [" << enumeration->args.size() << " bindings]";
        }
        if (!enumeration->namedArgs.empty()) {
            out << " [" << enumeration->namedArgs.size() << " fields]";
        }
        out << '\n';
        if (!enumeration->args.empty() || !enumeration->namedArgs.empty()) {
            ++indent;
            for (const auto &argument : enumeration->args) {
                if (argument) {
                    PrintPattern(*argument);
                }
            }
            for (const auto &argument : enumeration->namedArgs) {
                Pad();
                out << "." << argument.name << ":\n";
                ++indent;
                if (argument.pattern) {
                    PrintPattern(*argument.pattern);
                }
                --indent;
            }
            --indent;
        }
    }
    else if (const auto *structure = AstCast<const StructPattern>(&pattern)) {
        Pad();
        out << "StructPattern '" << structure->typeName << "'\n";
        ++indent;
        for (const auto &field : structure->fields) {
            Pad();
            out << "." << field.name << ":\n";
            ++indent;
            if (field.pattern) {
                PrintPattern(*field.pattern);
            }
            --indent;
        }
        --indent;
    }
    else if (const auto *tuple = AstCast<const TuplePattern>(&pattern)) {
        Pad();
        out << "TuplePattern [" << tuple->elements.size() << "]\n";
        ++indent;
        for (const auto &element : tuple->elements) {
            if (element) {
                PrintPattern(*element);
            }
        }
        --indent;
    }
    else if (const auto *typed = AstCast<const TypedPattern>(&pattern)) {
        Pad();
        out << "TypedPattern '" << (typed->name.empty() ? "_" : typed->name)
            << "' : " << DeclarationPrinter::TypeString(typed->type.get()) << "\n";
    }
    else if (const auto *presence = AstCast<const PresencePattern>(&pattern)) {
        Pad();
        out << "PresencePattern\n";
        ++indent;
        if (presence->inner) {
            PrintPattern(*presence->inner);
        }
        --indent;
    }
    else if (AstCast<const NonePattern>(&pattern)) {
        Pad();
        out << "NonePattern\n";
    }
    else if (const auto *guarded = AstCast<const GuardedPattern>(&pattern)) {
        Pad();
        out << "GuardedPattern\n";
        ++indent;
        if (guarded->inner) {
            PrintPattern(*guarded->inner);
        }
        Pad();
        out << "Guard\n";
        ++indent;
        if (guarded->guard) {
            printExpression(*guarded->guard);
        }
        --indent;
        --indent;
    }
}
} // namespace Rux::ParserDumpDetail
