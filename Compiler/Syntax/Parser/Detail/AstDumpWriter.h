#pragma once

#include "Syntax/Ast/Ast.h"

#include <functional>
#include <iosfwd>
#include <string>

namespace Rux::ParserDumpDetail {
class DeclarationPrinter;

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
} // namespace Rux::ParserDumpDetail
