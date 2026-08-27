// Declaration and type formatting for the human-readable AST dump.

#include "Syntax/Parser/Detail/AstDumpWriter.h"

#include <cstdlib>
#include <ostream>
#include <string_view>
#include <utility>

namespace Rux::ParserDumpDetail {
DeclarationPrinter::DeclarationPrinter(AstDumpWriter &inputWriter, ExpressionCallback expressionCallback,
                                       BlockCallback blockCallback)
    : writer(inputWriter)
    , out(inputWriter.out)
    , indent(inputWriter.indent)
    , printExpression(std::move(expressionCallback))
    , printBlock(std::move(blockCallback)) {
}

void DeclarationPrinter::Pad() const {
    writer.Pad();
}

void DeclarationPrinter::PrintTypeParameters(const std::vector<TypeParameter> &parameters) {
    if (parameters.empty()) {
        return;
    }
    out << '<';
    for (std::size_t parameterIndex = 0; parameterIndex < parameters.size(); ++parameterIndex) {
        if (parameterIndex != 0) {
            out << ", ";
        }
        const TypeParameter &parameter = parameters[parameterIndex];
        out << parameter.name;
        if (parameter.bounds.empty()) {
            continue;
        }
        out << ": ";
        for (std::size_t boundIndex = 0; boundIndex < parameter.bounds.size(); ++boundIndex) {
            if (boundIndex != 0) {
                out << " + ";
            }
            out << TypeString(parameter.bounds[boundIndex].get());
        }
    }
    out << '>';
}

std::string DeclarationPrinter::TypeString(const TypeExpr *type) {
    if (!type) {
        return "<null>";
    }
    if (const auto *named = dynamic_cast<const NamedTypeExpr *>(type)) {
        std::string result = named->name;
        if (!named->typeArgs.empty()) {
            result += "<";
            for (std::size_t index = 0; index < named->typeArgs.size(); ++index) {
                if (index) {
                    result += ", ";
                }
                result += TypeString(named->typeArgs[index].get());
            }
            result += ">";
        }
        return result;
    }
    if (const auto *path = dynamic_cast<const PathTypeExpr *>(type)) {
        std::string result;
        for (std::size_t index = 0; index < path->segments.size(); ++index) {
            if (index) {
                result += "::";
            }
            result += path->segments[index];
        }
        return result;
    }
    if (const auto *array = dynamic_cast<const ArrayTypeExpr *>(type)) {
        std::string element = TypeString(array->element.get());
        if (dynamic_cast<const PointerTypeExpr *>(array->element.get()) ||
            dynamic_cast<const ReferenceTypeExpr *>(array->element.get())) {
            element = "(" + element + ")";
        }
        std::string result = element + "[";
        if (array->size) {
            result += "N"; // size is an Expr, not easily stringified
        }
        return result + "]";
    }
    if (const auto *pointer = dynamic_cast<const PointerTypeExpr *>(type)) {
        std::string pointee = TypeString(pointer->pointee.get());
        if (dynamic_cast<const ArrayTypeExpr *>(pointer->pointee.get())) {
            pointee = "(" + pointee + ")";
        }
        return (pointer->pointeeMut ? "*var " : "*") + pointee;
    }
    if (const auto *reference = dynamic_cast<const ReferenceTypeExpr *>(type)) {
        std::string pointee = TypeString(reference->pointee.get());
        if (dynamic_cast<const ArrayTypeExpr *>(reference->pointee.get())) {
            pointee = "(" + pointee + ")";
        }
        return (reference->pointeeMut ? "&var " : "&") + pointee;
    }
    if (const auto *tuple = dynamic_cast<const TupleTypeExpr *>(type)) {
        std::string result = "(";
        for (std::size_t index = 0; index < tuple->elements.size(); ++index) {
            if (index) {
                result += ", ";
            }
            result += TypeString(tuple->elements[index].get());
        }
        if (tuple->elements.size() == 1) {
            result += ",";
        }
        return result + ")";
    }
    if (dynamic_cast<const SelfTypeExpr *>(type)) {
        return "self";
    }
    return "<type>";
}

void DeclarationPrinter::Print(const Decl &decl) {
    if (const auto *function = dynamic_cast<const FuncDecl *>(&decl)) {
        PrintFuncDecl(*function);
    }
    else if (const auto *structure = dynamic_cast<const StructDecl *>(&decl)) {
        PrintStructDecl(*structure);
    }
    else if (const auto *enumeration = dynamic_cast<const EnumDecl *>(&decl)) {
        PrintEnumDecl(*enumeration);
    }
    else if (const auto *unionDecl = dynamic_cast<const UnionDecl *>(&decl)) {
        PrintUnionDecl(*unionDecl);
    }
    else if (const auto *interfaceDecl = dynamic_cast<const InterfaceDecl *>(&decl)) {
        PrintInterfaceDecl(*interfaceDecl);
    }
    else if (const auto *implementation = dynamic_cast<const ImplDecl *>(&decl)) {
        PrintImplDecl(*implementation);
    }
    else if (const auto *module = dynamic_cast<const ModuleDecl *>(&decl)) {
        PrintModuleDecl(*module);
    }
    else if (const auto *use = dynamic_cast<const UseDecl *>(&decl)) {
        PrintUseDecl(*use);
    }
    else if (const auto *constant = dynamic_cast<const ConstDecl *>(&decl)) {
        PrintConstDecl(*constant);
    }
    else if (const auto *alias = dynamic_cast<const TypeAliasDecl *>(&decl)) {
        PrintTypeAliasDecl(*alias);
    }
    else if (const auto *externalFunction = dynamic_cast<const ExternFuncDecl *>(&decl)) {
        PrintExternFuncDecl(*externalFunction);
    }
    else if (const auto *externalVariable = dynamic_cast<const ExternVarDecl *>(&decl)) {
        PrintExternVarDecl(*externalVariable);
    }
    else if (const auto *when = dynamic_cast<const WhenDecl *>(&decl)) {
        PrintWhenDecl(*when);
    }
}

void DeclarationPrinter::PrintWhenDecl(const WhenDecl &decl) {
    Pad();
    out << "WhenDecl\n";
    ++indent;
    for (const auto &branch : decl.branches) {
        Pad();
        out << (branch.condition ? "Branch\n" : "Else\n");
        ++indent;
        if (branch.condition) {
            Pad();
            out << "Condition\n";
            ++indent;
            printExpression(*branch.condition);
            --indent;
        }
        for (const auto &item : branch.items) {
            if (item) {
                Print(*item);
            }
        }
        --indent;
    }
    --indent;
}

void DeclarationPrinter::PrintFuncDecl(const FuncDecl &decl) {
    if (decl.isNoReturn) {
        Pad();
        out << "#NoReturn()\n";
    }
    Pad();
    if (decl.isPublic) {
        out << "pub ";
    }
    if (decl.isAsm) {
        out << "asm ";
    }
    out << "FuncDecl '" << decl.name << "'";
    PrintTypeParameters(decl.typeParams);
    out << " (";
    for (std::size_t index = 0; index < decl.params.size(); ++index) {
        if (index) {
            out << ", ";
        }
        const auto &parameter = decl.params[index];
        if (parameter.isVariadic) {
            out << "...";
            continue;
        }
        out << parameter.name << ": " << TypeString(parameter.type.get());
    }
    out << ')';
    if (decl.returnType) {
        out << " -> " << TypeString(decl.returnType->get());
    }
    if (decl.isAsm) {
        out << '\n';
        ++indent;
        for (const auto &instruction : decl.asmBody) {
            Pad();
            if (!instruction.labelDef.empty()) {
                out << instruction.labelDef << ":\n";
                continue;
            }
            out << instruction.mnemonic;
            for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
                out << (index == 0 ? " " : ", ");
                PrintAsmOperand(instruction.operands[index], instruction.arch);
            }
            out << '\n';
        }
        --indent;
        return;
    }
    out << (decl.body ? "" : " [signature]") << '\n';
    if (decl.body) {
        ++indent;
        printBlock(*decl.body);
        --indent;
    }
}

void DeclarationPrinter::PrintAsmShift(const AsmOperand &operand) {
    static constexpr std::string_view shiftNames[5] = {"", "lsl", "lsr", "asr", "ror"};
    static constexpr std::string_view extendNames[9] = {"",     "uxtb", "uxth", "uxtw", "uxtx",
                                                        "sxtb", "sxth", "sxtw", "sxtx"};
    if (operand.shift != AsmShiftKind::None) {
        out << ", " << shiftNames[static_cast<std::size_t>(operand.shift)] << " #" << operand.shiftAmount;
    }
    else if (operand.extend != AsmExtendKind::None) {
        out << ", " << extendNames[static_cast<std::size_t>(operand.extend)];
        if (operand.shiftAmount != 0) {
            out << " #" << operand.shiftAmount;
        }
    }
}

void DeclarationPrinter::PrintAsmOperand(const AsmOperand &operand, const Target::Arch arch) {
    switch (operand.kind) {
    case AsmOperand::Kind::Reg:
    case AsmOperand::Kind::Sym:
        out << operand.name;
        PrintAsmShift(operand);
        break;
    case AsmOperand::Kind::Imm:
        if (arch == Target::Arch::AArch64) {
            out << '#';
        }
        out << operand.imm;
        PrintAsmShift(operand);
        break;
    case AsmOperand::Kind::Mem: {
        if (arch == Target::Arch::AArch64) {
            out << '[' << operand.memBase;
            if (!operand.memIndex.empty()) {
                out << ", " << operand.memIndex;
                PrintAsmShift(operand);
            }
            else if (operand.imm != 0 && operand.indexMode != AsmIndexMode::PostIndex) {
                out << ", #" << operand.imm;
            }
            if (!operand.memSym.empty()) {
                out << ", " << operand.memSym;
            }
            out << ']';
            if (operand.indexMode == AsmIndexMode::PreIndex) {
                out << '!';
            }
            else if (operand.indexMode == AsmIndexMode::PostIndex) {
                out << ", #" << operand.imm;
            }
            break;
        }
        out << '[';
        bool wrote = false;
        if (!operand.memBase.empty()) {
            out << operand.memBase;
            wrote = true;
        }
        if (!operand.memIndex.empty()) {
            out << (wrote ? " + " : "") << operand.memIndex << '*' << operand.memScale;
            wrote = true;
        }
        if (!operand.memSym.empty()) {
            out << (wrote ? " + " : "") << operand.memSym;
            wrote = true;
        }
        if (operand.imm != 0 || !wrote) {
            out << (wrote && operand.imm >= 0 ? " + " : (wrote ? " - " : ""))
                << (wrote ? std::abs(operand.imm) : operand.imm);
        }
        out << ']';
        break;
    }
    case AsmOperand::Kind::None:
        break;
    }
}

void DeclarationPrinter::PrintStructDecl(const StructDecl &decl) {
    Pad();
    if (decl.isPublic) {
        out << "pub ";
    }
    out << "StructDecl '" << decl.name << "'";
    PrintTypeParameters(decl.typeParams);
    out << '\n';
    ++indent;
    for (const auto &field : decl.fields) {
        Pad();
        if (field.isPublic) {
            out << "pub ";
        }
        out << "Field '" << field.name << "' : " << TypeString(field.type.get()) << '\n';
    }
    --indent;
}

void DeclarationPrinter::PrintEnumDecl(const EnumDecl &decl) {
    Pad();
    if (decl.isPublic) {
        out << "pub ";
    }
    out << "EnumDecl '" << decl.name << "'";
    PrintTypeParameters(decl.typeParams);
    if (decl.baseType) {
        out << " : " << TypeString(decl.baseType.get());
    }
    out << '\n';
    ++indent;
    for (const auto &variant : decl.variants) {
        Pad();
        out << "Variant '" << variant.name << "'";
        if (!variant.fields.empty()) {
            out << " (";
            for (std::size_t index = 0; index < variant.fields.size(); ++index) {
                if (index) {
                    out << ", ";
                }
                out << TypeString(variant.fields[index].get());
            }
            out << ')';
        }
        if (!variant.namedFields.empty()) {
            out << " { ";
            for (std::size_t index = 0; index < variant.namedFields.size(); ++index) {
                if (index) {
                    out << " ";
                }
                out << variant.namedFields[index].name << ": " << TypeString(variant.namedFields[index].type.get())
                    << ";";
            }
            out << " }";
        }
        if (variant.discriminant) {
            out << " = " << *variant.discriminant;
        }
        out << '\n';
    }
    --indent;
}

void DeclarationPrinter::PrintUnionDecl(const UnionDecl &decl) {
    Pad();
    if (decl.isPublic) {
        out << "pub ";
    }
    out << "UnionDecl '" << decl.name << "'\n";
    ++indent;
    for (const auto &field : decl.fields) {
        Pad();
        if (field.isPublic) {
            out << "pub ";
        }
        out << "Field '" << field.name << "' : " << TypeString(field.type.get()) << '\n';
    }
    --indent;
}

void DeclarationPrinter::PrintInterfaceDecl(const InterfaceDecl &decl) {
    Pad();
    if (decl.isPublic) {
        out << "pub ";
    }
    out << "InterfaceDecl '" << decl.name << "'\n";
    ++indent;
    for (const auto &method : decl.methods) {
        if (method) {
            PrintFuncDecl(*method);
        }
    }
    --indent;
}

void DeclarationPrinter::PrintImplDecl(const ImplDecl &decl) {
    Pad();
    out << "ImplDecl ";
    if (decl.interfaceName) {
        out << *decl.interfaceName << " for ";
    }
    out << decl.typeName << '\n';
    ++indent;
    for (const auto &method : decl.methods) {
        if (method) {
            PrintFuncDecl(*method);
        }
    }
    for (const auto &conditional : decl.conditionals) {
        if (conditional) {
            PrintWhenDecl(*conditional);
        }
    }
    --indent;
}

void DeclarationPrinter::PrintModuleDecl(const ModuleDecl &decl) {
    Pad();
    if (decl.isPublic) {
        out << "pub ";
    }
    out << "ModuleDecl '" << decl.name << "'\n";
    ++indent;
    for (const auto &item : decl.items) {
        if (item) {
            Print(*item);
        }
    }
    --indent;
}

void DeclarationPrinter::PrintUseDecl(const UseDecl &decl) const {
    Pad();
    out << "ImportDecl '";
    for (std::size_t index = 0; index < decl.path.size(); ++index) {
        if (index) {
            out << '.';
        }
        out << decl.path[index];
    }
    switch (decl.kind) {
    case UseDecl::Kind::Glob:
        out << ".*";
        break;
    case UseDecl::Kind::Multi:
        out << "::{";
        for (std::size_t index = 0; index < decl.names.size(); ++index) {
            if (index) {
                out << ", ";
            }
            out << decl.names[index];
        }
        out << '}';
        break;
    default:
        break;
    }
    out << "'\n";
}

void DeclarationPrinter::PrintConstDecl(const ConstDecl &decl) {
    Pad();
    if (decl.isPublic) {
        out << "pub ";
    }
    out << "ConstDecl '" << decl.name << "'";
    if (decl.type) {
        out << " : " << TypeString(decl.type->get());
    }
    out << '\n';
    ++indent;
    if (decl.value) {
        printExpression(*decl.value);
    }
    --indent;
}

void DeclarationPrinter::PrintTypeAliasDecl(const TypeAliasDecl &decl) const {
    Pad();
    if (decl.isPublic) {
        out << "pub ";
    }
    out << "TypeAliasDecl '" << decl.name << "' = " << TypeString(decl.type.get()) << '\n';
}

void DeclarationPrinter::PrintExternFuncDecl(const ExternFuncDecl &decl) const {
    if (decl.isNoReturn) {
        Pad();
        out << "#NoReturn()\n";
    }
    if (!decl.dll.empty()) {
        Pad();
        out << "#Link(\"" << decl.dll << "\"";
        if (!decl.symbolName.empty()) {
            out << ", \"" << decl.symbolName << "\"";
        }
        out << ")\n";
    }
    if (decl.callConv != CallingConvention::Default) {
        Pad();
        out << "#Abi(" << ConventionName(decl.callConv) << ")\n";
    }
    Pad();
    if (decl.isPublic) {
        out << "pub ";
    }
    out << "ExternFuncDecl '" << decl.name << "' (";
    for (std::size_t index = 0; index < decl.params.size(); ++index) {
        if (index) {
            out << ", ";
        }
        out << decl.params[index].name << ": " << TypeString(decl.params[index].type.get());
    }
    if (decl.isVariadic) {
        out << (decl.params.empty() ? "..." : ", ...");
    }
    out << ')';
    if (decl.returnType) {
        out << " -> " << TypeString(decl.returnType->get());
    }
    out << '\n';
}

void DeclarationPrinter::PrintExternVarDecl(const ExternVarDecl &decl) const {
    Pad();
    if (decl.isPublic) {
        out << "pub ";
    }
    out << "ExternVarDecl '" << decl.name << "' : " << TypeString(decl.type.get()) << '\n';
}
} // namespace Rux::ParserDumpDetail
