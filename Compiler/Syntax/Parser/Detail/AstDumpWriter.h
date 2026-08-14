#pragma once

#include "Syntax/Ast/Ast.h"

#include <functional>
#include <iosfwd>
#include <string>

namespace Rux::ParserDumpDetail {
class DeclarationPrinter;
class ExpressionPrinter;
class StatementPrinter;

// Private output and indentation state shared by the focused AST printers.
// Parser::DumpAst remains the only public facade.
class AstDumpWriter {
public:
    explicit AstDumpWriter(std::ostream &output);

protected:
    std::ostream &out;
    int indent = 0;

    void Pad() const;

private:
    friend class DeclarationPrinter;
    friend class ExpressionPrinter;
    friend class StatementPrinter;
};

class ExpressionPrinter {
public:
    using BlockCallback = std::function<void(const Block &)>;
    using PatternCallback = std::function<void(const Pattern &)>;

    ExpressionPrinter(AstDumpWriter &writer, BlockCallback blockCallback, PatternCallback patternCallback);

    void Print(const Expr &expression);

private:
    AstDumpWriter &writer;
    std::ostream &out;
    int &indent;
    BlockCallback printBlock;
    PatternCallback printPattern;

    void Pad() const;
    void PrintLiteralExpr(const LiteralExpr &expression) const;
};

class DeclarationPrinter {
public:
    using ExpressionCallback = std::function<void(const Expr &)>;
    using BlockCallback = std::function<void(const Block &)>;

    DeclarationPrinter(AstDumpWriter &writer, ExpressionCallback expressionCallback, BlockCallback blockCallback);

    void Print(const Decl &decl);
    [[nodiscard]] static std::string TypeString(const TypeExpr *type);

private:
    AstDumpWriter &writer;
    std::ostream &out;
    int &indent;
    ExpressionCallback printExpression;
    BlockCallback printBlock;

    void Pad() const;
    void PrintWhenDecl(const WhenDecl &decl);
    void PrintFuncDecl(const FuncDecl &decl);
    void PrintAsmShift(const AsmOperand &operand);
    void PrintAsmOperand(const AsmOperand &operand, Target::Arch arch);
    void PrintStructDecl(const StructDecl &decl);
    void PrintEnumDecl(const EnumDecl &decl);
    void PrintUnionDecl(const UnionDecl &decl);
    void PrintInterfaceDecl(const InterfaceDecl &decl);
    void PrintImplDecl(const ImplDecl &decl);
    void PrintModuleDecl(const ModuleDecl &decl);
    void PrintUseDecl(const UseDecl &decl) const;
    void PrintConstDecl(const ConstDecl &decl);
    void PrintTypeAliasDecl(const TypeAliasDecl &decl) const;
    void PrintExternFuncDecl(const ExternFuncDecl &decl) const;
    void PrintExternVarDecl(const ExternVarDecl &decl) const;
};

class StatementPrinter {
public:
    using ExpressionCallback = std::function<void(const Expr &)>;
    using DeclarationCallback = std::function<void(const Decl &)>;

    StatementPrinter(AstDumpWriter &writer, ExpressionCallback expressionCallback,
                     DeclarationCallback declarationCallback);

    void PrintBlock(const Block &block);
    void Print(const Stmt &statement);
    void PrintPattern(const Pattern &pattern);

private:
    AstDumpWriter &writer;
    std::ostream &out;
    int &indent;
    ExpressionCallback printExpression;
    DeclarationCallback printDeclaration;

    void Pad() const;
    void PrintLetStmt(const LetStmt &statement);
    void PrintIfStmt(const IfStmt &statement);
    void PrintWhileStmt(const WhileStmt &statement);
    void PrintForStmt(const ForStmt &statement);
    void PrintMatchStmt(const MatchStmt &statement);
    void PrintReturnStmt(const ReturnStmt &statement);
};
} // namespace Rux::ParserDumpDetail
