// Human-readable semantic-analysis dump.

#include "Semantic/SemanticPrinter.h"

#include <format>
#include <fstream>
#include <string>
#include <string_view>

namespace Rux {
namespace {
std::string TypeText(const TypeExpr *type) {
    if (!type) {
        return "?";
    }
    if (const auto *named = dynamic_cast<const NamedTypeExpr *>(type)) {
        std::string text = named->name;
        if (!named->typeArgs.empty()) {
            text += '<';
            for (std::size_t index = 0; index < named->typeArgs.size(); ++index) {
                if (index != 0) {
                    text += ", ";
                }
                text += TypeText(named->typeArgs[index].get());
            }
            text += '>';
        }
        return text;
    }
    if (const auto *path = dynamic_cast<const PathTypeExpr *>(type)) {
        std::string text;
        for (std::size_t index = 0; index < path->segments.size(); ++index) {
            if (index != 0) {
                text += "::";
            }
            text += path->segments[index];
        }
        return text;
    }
    if (const auto *pointer = dynamic_cast<const PointerTypeExpr *>(type)) {
        return std::string(pointer->pointeeMut ? "*var " : "*") + TypeText(pointer->pointee.get());
    }
    if (const auto *reference = dynamic_cast<const ReferenceTypeExpr *>(type)) {
        return std::string(reference->pointeeMut ? "&var " : "&") + TypeText(reference->pointee.get());
    }
    if (const auto *array = dynamic_cast<const ArrayTypeExpr *>(type)) {
        return TypeText(array->element.get()) + (array->size ? "[N]" : "[]");
    }
    if (const auto *tuple = dynamic_cast<const TupleTypeExpr *>(type)) {
        std::string text = "(";
        for (std::size_t index = 0; index < tuple->elements.size(); ++index) {
            if (index != 0) {
                text += ", ";
            }
            text += TypeText(tuple->elements[index].get());
        }
        return text + (tuple->elements.size() == 1 ? ",)" : ")");
    }
    if (dynamic_cast<const SelfTypeExpr *>(type)) {
        return "self";
    }
    return "?";
}

std::string FunctionSignature(const FuncDecl &function) {
    std::string text = "func " + function.name + "(";
    for (std::size_t index = 0; index < function.params.size(); ++index) {
        if (index != 0) {
            text += ", ";
        }
        const auto &parameter = function.params[index];
        text += parameter.isVariadic ? "..." : parameter.name + ": " + TypeText(parameter.type.get());
    }
    text += ')';
    if (function.returnType) {
        text += " -> " + TypeText(function.returnType->get());
    }
    return text;
}
} // namespace

bool SemanticPrinter::Dump(const SemanticModel &result, const std::filesystem::path &path) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }

    static constexpr auto kindName = [](SemanticSymbol::Kind k) -> std::string_view {
        switch (k) {
        case SemanticSymbol::Kind::Var:
            return "var";
        case SemanticSymbol::Kind::Func:
            return "func";
        case SemanticSymbol::Kind::Type:
            return "type";
        case SemanticSymbol::Kind::Const:
            return "const";
        case SemanticSymbol::Kind::Module:
            return "module";
        case SemanticSymbol::Kind::Interface:
            return "interface";
        }
        return "?";
    };

    out << "=== Semantic Analysis Results ===\n\n";

    // Symbols
    out << std::format("Symbols ({} total)\n", result.symbols.size());
    out << std::string(40, '-') << '\n';

    if (result.symbols.empty()) {
        out << "(none)\n";
    }
    else {
        for (const auto &sym : result.symbols) {
            std::string tag = std::format("{:<10}", kindName(sym.kind));
            std::string qname = sym.name;
            if (sym.isMut) {
                qname += " (var)";
            }
            std::string typeStr = sym.resolvedType.empty() ? "" : "  " + sym.resolvedType;
            out << std::format("{}  {:<28}{}  [{}:{}:{}]\n", tag, qname, typeStr, sym.sourceName, sym.location.line,
                               sym.location.column);
        }
    }

    out << '\n';

    std::size_t functionCount = 0;
    for (const Module *module : result.modules) {
        if (!module) {
            continue;
        }
        for (const auto &item : module->items) {
            functionCount += dynamic_cast<const FuncDecl *>(item.get()) != nullptr ? 1 : 0;
        }
    }
    out << std::format("Function signatures ({} total)\n", functionCount);
    out << std::string(40, '-') << '\n';
    if (functionCount == 0) {
        out << "(none)\n";
    }
    else {
        for (const Module *module : result.modules) {
            if (!module) {
                continue;
            }
            for (const auto &item : module->items) {
                if (const auto *function = dynamic_cast<const FuncDecl *>(item.get())) {
                    out << FunctionSignature(*function) << '\n';
                }
            }
        }
    }

    out << '\n';

    static constexpr auto operationName = [](const TypeProperties::SpecialOperationState state) {
        switch (state) {
        case TypeProperties::SpecialOperationState::Generated:
            return "generated";
        case TypeProperties::SpecialOperationState::Custom:
            return "custom";
        case TypeProperties::SpecialOperationState::Prohibited:
            return "prohibited";
        case TypeProperties::SpecialOperationState::Unresolved:
            return "unresolved";
        }
        return "?";
    };

    std::size_t capabilityCount = 0;
    for (const auto &symbol : result.symbols) {
        if (symbol.kind == SemanticSymbol::Kind::Type && result.TryGetProperties(TypeRef::MakeNamed(symbol.name))) {
            ++capabilityCount;
        }
    }
    out << std::format("Type capabilities ({} total)\n", capabilityCount);
    out << std::string(40, '-') << '\n';
    if (capabilityCount == 0) {
        out << "(none)\n";
    }
    else {
        for (const auto &symbol : result.symbols) {
            if (symbol.kind != SemanticSymbol::Kind::Type) {
                continue;
            }
            const TypeProperties *properties = result.TryGetProperties(TypeRef::MakeNamed(symbol.name));
            if (!properties) {
                continue;
            }
            out << std::format("{:<28}  copy={} move={} drop={}\n", symbol.name,
                               operationName(properties->copyOperation), operationName(properties->moveOperation),
                               properties->droppable ? "yes" : "no");
        }
    }

    out << '\n';

    // Diagnostics
    out << std::format("Diagnostics ({} total)\n", result.diagnostics.size());
    out << std::string(40, '-') << '\n';

    if (result.diagnostics.empty()) {
        out << "(none)\n";
    }
    else {
        for (const auto &diag : result.diagnostics) {
            const char *sev = diag.severity == SemanticDiagnostic::Severity::Error ? "error" : "warning";
            out << std::format("{}:{}:{}: {}: {}\n", diag.sourceName, diag.location.line, diag.location.column, sev,
                               diag.message);
        }
    }

    return out.good();
}
} // namespace Rux
