#include "Optimization/LirReachability.h"

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Rux::Optimization {
namespace {
using DeclarationList = std::vector<LirDeclarationId>;
using SymbolIndex = std::unordered_map<std::string, DeclarationList>;

struct PackageIndex {
    SymbolIndex functions;
    SymbolIndex data;
    SymbolIndex all;
};

void IndexSymbol(SymbolIndex &index, const std::string &name, const LirDeclarationId declaration) {
    index[name].push_back(declaration);
}

PackageIndex BuildIndex(const LirPackage &package) {
    PackageIndex index;
    for (std::size_t moduleIndex = 0; moduleIndex < package.modules.size(); ++moduleIndex) {
        const auto &module = package.modules[moduleIndex];
        for (std::size_t functionIndex = 0; functionIndex < module.funcs.size(); ++functionIndex) {
            const LirDeclarationId declaration{LirDeclarationKind::Function, moduleIndex, functionIndex};
            IndexSymbol(index.functions, module.funcs[functionIndex].name, declaration);
            IndexSymbol(index.all, module.funcs[functionIndex].name, declaration);
        }
        for (std::size_t constantIndex = 0; constantIndex < module.consts.size(); ++constantIndex) {
            const LirDeclarationId declaration{LirDeclarationKind::Constant, moduleIndex, constantIndex};
            IndexSymbol(index.data, module.consts[constantIndex].name, declaration);
            IndexSymbol(index.all, module.consts[constantIndex].name, declaration);
        }
        for (std::size_t vtableIndex = 0; vtableIndex < module.vtables.size(); ++vtableIndex) {
            const LirDeclarationId declaration{LirDeclarationKind::Vtable, moduleIndex, vtableIndex};
            IndexSymbol(index.data, module.vtables[vtableIndex].label, declaration);
            IndexSymbol(index.all, module.vtables[vtableIndex].label, declaration);
        }
        for (std::size_t variableIndex = 0; variableIndex < module.externVars.size(); ++variableIndex) {
            const LirDeclarationId declaration{LirDeclarationKind::ExternVariable, moduleIndex, variableIndex};
            IndexSymbol(index.data, module.externVars[variableIndex].name, declaration);
            IndexSymbol(index.all, module.externVars[variableIndex].name, declaration);
        }
    }
    return index;
}

std::string_view NormalizeSymbol(std::string_view symbol) {
    if (symbol.starts_with('&')) {
        symbol.remove_prefix(1);
    }
    return symbol;
}

const DeclarationList *FindSymbol(const SymbolIndex &index, std::string_view symbol) {
    symbol = NormalizeSymbol(symbol);
    if (const auto found = index.find(std::string(symbol)); found != index.end()) {
        return &found->second;
    }
    if (const auto separator = symbol.rfind("::"); separator != std::string_view::npos) {
        if (const auto found = index.find(std::string(symbol.substr(separator + 2))); found != index.end()) {
            return &found->second;
        }
    }
    return nullptr;
}

class ReachabilityWalker {
public:
    ReachabilityWalker(const LirPackage &package, const ArtifactKind artifactKind)
        : package_(package)
        , artifactKind_(artifactKind)
        , index_(BuildIndex(package)) {
    }

    DeclarationList Run() {
        AddRoots();
        while (next_ < worklist_.size()) {
            Visit(worklist_[next_++]);
        }
        return {seen_.begin(), seen_.end()};
    }

private:
    void Add(const LirDeclarationId declaration) {
        if (seen_.insert(declaration).second) {
            worklist_.push_back(declaration);
        }
    }

    void Add(const SymbolIndex &index, const std::string_view symbol) {
        if (const DeclarationList *declarations = FindSymbol(index, symbol)) {
            for (const LirDeclarationId declaration : *declarations) {
                Add(declaration);
            }
        }
    }

    void AddRoots() {
        if (artifactKind_ == ArtifactKind::Executable) {
            Add(index_.functions, "Main");
            return;
        }

        for (std::size_t moduleIndex = 0; moduleIndex < package_.modules.size(); ++moduleIndex) {
            const auto &module = package_.modules[moduleIndex];
            for (std::size_t functionIndex = 0; functionIndex < module.funcs.size(); ++functionIndex) {
                if (module.funcs[functionIndex].isPublic) {
                    Add({LirDeclarationKind::Function, moduleIndex, functionIndex});
                }
            }
            for (std::size_t constantIndex = 0; constantIndex < module.consts.size(); ++constantIndex) {
                if (module.consts[constantIndex].isPublic) {
                    Add({LirDeclarationKind::Constant, moduleIndex, constantIndex});
                }
            }
            for (std::size_t vtableIndex = 0; vtableIndex < module.vtables.size(); ++vtableIndex) {
                Add({LirDeclarationKind::Vtable, moduleIndex, vtableIndex});
            }
            for (std::size_t variableIndex = 0; variableIndex < module.externVars.size(); ++variableIndex) {
                if (module.externVars[variableIndex].isPublic) {
                    Add({LirDeclarationKind::ExternVariable, moduleIndex, variableIndex});
                }
            }
        }
    }

    void Visit(const LirDeclarationId declaration) {
        const auto &module = package_.modules[declaration.moduleIndex];
        switch (declaration.kind) {
        case LirDeclarationKind::Function:
            VisitFunction(module.funcs[declaration.declarationIndex]);
            break;
        case LirDeclarationKind::Constant:
            VisitConstant(module.consts[declaration.declarationIndex]);
            break;
        case LirDeclarationKind::Vtable:
            VisitVtable(module.vtables[declaration.declarationIndex]);
            break;
        case LirDeclarationKind::ExternVariable:
            break;
        }
    }

    void VisitFunction(const LirFunc &function) {
        // An unresolved external declaration has no package-owned body to
        // traverse. It remains reachable so a later pruning pass keeps the
        // declaration needed by the call or address reference.
        if (function.isExtern) {
            return;
        }
        for (const auto &block : function.blocks) {
            for (const auto &instruction : block.instrs) {
                if (instruction.op == LirOpcode::Call) {
                    Add(index_.functions, instruction.strArg);
                }
                else if (instruction.op == LirOpcode::GlobalAddr) {
                    Add(index_.all, instruction.strArg);
                }
                else if (instruction.op == LirOpcode::Load && instruction.srcs.empty()) {
                    Add(index_.data, instruction.strArg);
                }
            }
        }
        for (const auto &instruction : function.asmBody) {
            for (const auto &operand : instruction.operands) {
                if (operand.kind == AsmOperand::Kind::Sym) {
                    Add(index_.all, operand.name);
                }
                if (!operand.memSym.empty()) {
                    Add(index_.data, operand.memSym);
                }
            }
        }
    }

    void VisitConstant(const LirConstDecl &constant) {
        Add(index_.data, constant.value);
        for (const auto &element : constant.elements) {
            Add(index_.data, element);
        }
    }

    void VisitVtable(const LirVtable &vtable) {
        for (const auto &method : vtable.methods) {
            Add(index_.functions, method);
        }
    }

    const LirPackage &package_;
    ArtifactKind artifactKind_;
    PackageIndex index_;
    DeclarationList worklist_;
    std::set<LirDeclarationId> seen_;
    std::size_t next_ = 0;
};
} // namespace

bool LirReachabilityResult::IsReachable(const LirDeclarationId declaration) const {
    return std::ranges::binary_search(reachableDeclarations_, declaration);
}

const std::vector<LirDeclarationId> &LirReachabilityResult::ReachableDeclarations() const noexcept {
    return reachableDeclarations_;
}

LirReachabilityResult LirReachabilityAnalysis::Run(const LirPackage &package, const ArtifactKind artifactKind) {
    LirReachabilityResult result;
    result.reachableDeclarations_ = ReachabilityWalker(package, artifactKind).Run();
    return result;
}
} // namespace Rux::Optimization
