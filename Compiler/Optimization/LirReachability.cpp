#include "Optimization/LirReachability.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Rux::Optimization {
namespace {
using DeclarationList = std::vector<LirDeclarationId>;

/// Hashes a symbol however it is spelled, so a lookup by `std::string_view` builds no string.
struct SymbolHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(const std::string_view symbol) const noexcept {
        return std::hash<std::string_view>{}(symbol);
    }
};

using SymbolIndex = std::unordered_map<std::string, DeclarationList, SymbolHash, std::equal_to<>>;

struct PackageIndex {
    SymbolIndex functions;
    SymbolIndex data;
    SymbolIndex all;

    [[nodiscard]] const SymbolIndex &For(const LirSymbolReference::Scope scope) const {
        switch (scope) {
        case LirSymbolReference::Scope::Function:
            return functions;
        case LirSymbolReference::Scope::Data:
            return data;
        case LirSymbolReference::Scope::Any:
            break;
        }
        return all;
    }
};

/// The modules a walk covers, with the index each has in the package.
struct Graph {
    std::vector<const LirModuleReferences *> modules;
    std::vector<std::size_t> moduleIndices;
};

/// Index every declaration by the names it can be referenced under, so the walk can resolve a symbol string to
/// declarations without rescanning the modules for each reference.
PackageIndex BuildIndex(const Graph &graph) {
    PackageIndex index;
    for (std::size_t position = 0; position < graph.modules.size(); ++position) {
        const auto &module = *graph.modules[position];
        for (std::size_t kind = 0; kind < kLirDeclarationKinds; ++kind) {
            const auto declarationKind = static_cast<LirDeclarationKind>(kind);
            for (std::size_t declarationIndex = 0; declarationIndex < module.declarations[kind].size();
                 ++declarationIndex) {
                const LirDeclarationId declaration{declarationKind, graph.moduleIndices[position], declarationIndex};
                const std::string &name = module.declarations[kind][declarationIndex].name;
                (declarationKind == LirDeclarationKind::Function ? index.functions : index.data)[name].push_back(
                    declaration);
                index.all[name].push_back(declaration);
            }
        }
    }
    return index;
}

/// Strip the `&` an address-of reference carries, so taking a function's address and calling it resolve to the same
/// declaration.
std::string_view NormalizeSymbol(std::string_view symbol) {
    if (symbol.starts_with('&')) {
        symbol.remove_prefix(1);
    }
    return symbol;
}

/// Resolve a symbol reference to the declarations it could name, retrying without the module qualifier so a call
/// written as `Module::Func` still finds a declaration indexed under its bare name.
///
/// @return nullptr when nothing matches
const DeclarationList *FindSymbol(const SymbolIndex &index, std::string_view symbol) {
    symbol = NormalizeSymbol(symbol);
    if (const auto found = index.find(symbol); found != index.end()) {
        return &found->second;
    }
    if (const auto separator = symbol.rfind("::"); separator != std::string_view::npos) {
        if (const auto found = index.find(symbol.substr(separator + 2)); found != index.end()) {
            return &found->second;
        }
    }
    return nullptr;
}

class ReachabilityWalker {
public:
    explicit ReachabilityWalker(Graph inputGraph)
        : graph(std::move(inputGraph))
        , index(BuildIndex(graph)) {
        std::size_t moduleCount = 0;
        for (const std::size_t moduleIndex : graph.moduleIndices)
            moduleCount = std::max(moduleCount, moduleIndex + 1);
        positions.assign(moduleCount, std::nullopt);
        for (std::size_t position = 0; position < graph.modules.size(); ++position)
            positions[graph.moduleIndices[position]] = position;
        for (std::size_t kind = 0; kind < kLirDeclarationKinds; ++kind) {
            seen[kind].resize(moduleCount);
            for (std::size_t position = 0; position < graph.modules.size(); ++position)
                seen[kind][graph.moduleIndices[position]].resize(graph.modules[position]->declarations[kind].size());
        }
    }

    /// Record, per module, the declarations a walk enters it through from outside.
    void TrackRoots() {
        roots.emplace(positions.size());
    }

    /// Start at the artifact's roots: `Main` of an executable, every public declaration and vtable of a library.
    void AddArtifactRoots(const ArtifactKind artifactKind) {
        if (artifactKind == ArtifactKind::Executable) {
            Add(index.functions, "Main", std::nullopt);
            return;
        }
        for (std::size_t position = 0; position < graph.modules.size(); ++position) {
            const auto &module = *graph.modules[position];
            for (std::size_t kind = 0; kind < kLirDeclarationKinds; ++kind)
                for (std::size_t declarationIndex = 0; declarationIndex < module.declarations[kind].size();
                     ++declarationIndex)
                    if (module.declarations[kind][declarationIndex].libraryRoot)
                        Add({static_cast<LirDeclarationKind>(kind), graph.moduleIndices[position], declarationIndex},
                            std::nullopt);
        }
    }

    /// Start at the named declarations of the one module the walk covers.
    void AddModuleRoots(const std::span<const LirModuleRoot> moduleRoots) {
        const std::size_t moduleIndex = graph.moduleIndices.front();
        const auto &module = *graph.modules.front();
        for (const LirModuleRoot &root : moduleRoots) {
            const auto &declarations = module.declarations[static_cast<std::size_t>(root.kind)];
            for (std::size_t declarationIndex = 0; declarationIndex < declarations.size(); ++declarationIndex)
                if (declarations[declarationIndex].name == root.name)
                    Add({root.kind, moduleIndex, declarationIndex}, std::nullopt);
        }
    }

    /// The reachable declarations in ascending order: by kind, then module, then position.
    DeclarationList Run() {
        while (next < worklist.size()) {
            Visit(worklist[next++]);
        }
        DeclarationList reachable;
        reachable.reserve(worklist.size());
        for (std::size_t kind = 0; kind < kLirDeclarationKinds; ++kind)
            for (std::size_t moduleIndex = 0; moduleIndex < seen[kind].size(); ++moduleIndex)
                for (std::size_t declarationIndex = 0; declarationIndex < seen[kind][moduleIndex].size();
                     ++declarationIndex)
                    if (seen[kind][moduleIndex][declarationIndex])
                        reachable.push_back({static_cast<LirDeclarationKind>(kind), moduleIndex, declarationIndex});
        return reachable;
    }

    [[nodiscard]] std::vector<std::vector<LirModuleRoot>> TakeRoots() {
        std::vector<std::vector<LirModuleRoot>> result(positions.size());
        for (std::size_t moduleIndex = 0; moduleIndex < positions.size(); ++moduleIndex)
            result[moduleIndex].assign((*roots)[moduleIndex].begin(), (*roots)[moduleIndex].end());
        return result;
    }

private:
    [[nodiscard]] const LirDeclarationReferences &Declaration(const LirDeclarationId declaration) const {
        return graph.modules[*positions[declaration.moduleIndex]]
            ->declarations[static_cast<std::size_t>(declaration.kind)][declaration.declarationIndex];
    }

    /// Reach `declaration` from module `from`, or from the artifact's roots when there is none.
    void Add(const LirDeclarationId declaration, const std::optional<std::size_t> from) {
        if (roots && from != declaration.moduleIndex)
            (*roots)[declaration.moduleIndex].insert({declaration.kind, Declaration(declaration).name});
        auto flag = seen[static_cast<std::size_t>(declaration.kind)][declaration.moduleIndex][declaration.declarationIndex];
        if (!flag) {
            flag = true;
            worklist.push_back(declaration);
        }
    }

    void Add(const SymbolIndex &symbolIndex, const std::string_view symbol, const std::optional<std::size_t> from) {
        if (const DeclarationList *declarations = FindSymbol(symbolIndex, symbol)) {
            for (const LirDeclarationId declaration : *declarations) {
                Add(declaration, from);
            }
        }
    }

    void Visit(const LirDeclarationId declaration) {
        for (const LirSymbolReference &reference : Declaration(declaration).references)
            Add(index.For(reference.scope), reference.symbol, declaration.moduleIndex);
    }

    Graph graph;
    PackageIndex index;
    std::vector<std::optional<std::size_t>> positions;
    DeclarationList worklist;
    // Indexed by kind, module and position; the kinds are the enumerators of LirDeclarationKind in order.
    std::array<std::vector<std::vector<bool>>, kLirDeclarationKinds> seen;
    std::optional<std::vector<std::set<LirModuleRoot>>> roots;
    std::size_t next = 0;
};

void Reference(LirDeclarationReferences &declaration, const LirSymbolReference::Scope scope, std::string symbol) {
    declaration.references.push_back({scope, std::move(symbol)});
}

LirDeclarationReferences FunctionReferences(const LirFunc &function) {
    LirDeclarationReferences declaration{function.name, function.isExported, {}};
    // An unresolved external declaration has no package-owned body to traverse. It remains reachable so a later
    // pruning pass keeps the declaration needed by the call or address reference.
    if (function.isExtern) {
        return declaration;
    }
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            if (instruction.op == LirOpcode::Call) {
                Reference(declaration, LirSymbolReference::Scope::Function, instruction.strArg);
            }
            // Code generation calls the stack-trace printer from every panic and failed assertion.
            else if (instruction.op == LirOpcode::Panic || instruction.op == LirOpcode::Assert) {
                Reference(declaration, LirSymbolReference::Scope::Function, std::string(kBacktraceFunctionName));
            }
            else if (instruction.op == LirOpcode::GlobalAddr || instruction.op == LirOpcode::ThreadLocalAddr) {
                Reference(declaration, LirSymbolReference::Scope::Any, instruction.strArg);
            }
            else if (instruction.op == LirOpcode::Load && instruction.srcs.empty()) {
                Reference(declaration, LirSymbolReference::Scope::Data, instruction.strArg);
            }
        }
    }
    for (const auto &instruction : function.asmBody) {
        for (const auto &operand : instruction.operands) {
            if (operand.kind == AsmOperand::Kind::Sym) {
                Reference(declaration, LirSymbolReference::Scope::Any, operand.name);
            }
            if (!operand.memSym.empty()) {
                Reference(declaration, LirSymbolReference::Scope::Data, operand.memSym);
            }
        }
    }
    return declaration;
}

Graph WholePackage(const std::vector<LirModuleReferences> &references) {
    Graph graph;
    for (std::size_t moduleIndex = 0; moduleIndex < references.size(); ++moduleIndex) {
        graph.modules.push_back(&references[moduleIndex]);
        graph.moduleIndices.push_back(moduleIndex);
    }
    return graph;
}
} // namespace

LirModuleReferences CollectModuleReferences(const LirModule &module) {
    LirModuleReferences result;
    auto &functions = result.declarations[static_cast<std::size_t>(LirDeclarationKind::Function)];
    for (const auto &function : module.funcs)
        functions.push_back(FunctionReferences(function));
    auto &constants = result.declarations[static_cast<std::size_t>(LirDeclarationKind::Constant)];
    for (const auto &constant : module.consts) {
        auto &declaration = constants.emplace_back(LirDeclarationReferences{constant.name, constant.isExported, {}});
        Reference(declaration, LirSymbolReference::Scope::Data, constant.value);
        for (const auto &element : constant.elements)
            Reference(declaration, LirSymbolReference::Scope::Data, element);
    }
    auto &vtables = result.declarations[static_cast<std::size_t>(LirDeclarationKind::Vtable)];
    for (const auto &vtable : module.vtables) {
        auto &declaration = vtables.emplace_back(LirDeclarationReferences{vtable.label, true, {}});
        for (const auto &method : vtable.methods)
            Reference(declaration, LirSymbolReference::Scope::Function, method);
    }
    // Storage refers to nothing: its initial value is bytes, never an address.
    auto &externVariables = result.declarations[static_cast<std::size_t>(LirDeclarationKind::ExternVariable)];
    for (const auto &variable : module.externVars)
        externVariables.push_back({variable.name, variable.isExported, {}});
    auto &globals = result.declarations[static_cast<std::size_t>(LirDeclarationKind::Global)];
    for (const auto &global : module.globals)
        globals.push_back({global.name, global.isExported, {}});
    // Which reference came first never matters to a walk, and a sorted set is smaller to keep.
    for (auto &kind : result.declarations)
        for (auto &declaration : kind) {
            std::ranges::sort(declaration.references);
            const auto duplicates = std::ranges::unique(declaration.references);
            declaration.references.erase(duplicates.begin(), duplicates.end());
        }
    return result;
}

std::vector<std::vector<LirModuleRoot>> ModuleRoots(const std::span<const LirModuleReferences> modules,
                                                    const ArtifactKind artifactKind) {
    std::vector<const LirModuleReferences *> pointers;
    pointers.reserve(modules.size());
    for (const auto &module : modules)
        pointers.push_back(&module);
    return ModuleRoots(pointers, artifactKind);
}

std::vector<std::vector<LirModuleRoot>> ModuleRoots(const std::span<const LirModuleReferences *const> modules,
                                                    const ArtifactKind artifactKind) {
    Graph graph;
    for (std::size_t moduleIndex = 0; moduleIndex < modules.size(); ++moduleIndex) {
        graph.modules.push_back(modules[moduleIndex]);
        graph.moduleIndices.push_back(moduleIndex);
    }
    ReachabilityWalker walker(std::move(graph));
    walker.TrackRoots();
    walker.AddArtifactRoots(artifactKind);
    static_cast<void>(walker.Run());
    return walker.TakeRoots();
}

bool LirReachabilityResult::IsReachable(const LirDeclarationId declaration) const {
    return std::ranges::binary_search(reachableDeclarations, declaration);
}

const std::vector<LirDeclarationId> &LirReachabilityResult::ReachableDeclarations() const noexcept {
    return reachableDeclarations;
}

LirReachabilityResult LirReachabilityAnalysis::Run(const LirPackage &package, const ArtifactKind artifactKind) {
    std::vector<LirModuleReferences> references;
    references.reserve(package.modules.size());
    for (const auto &module : package.modules)
        references.push_back(CollectModuleReferences(module));
    ReachabilityWalker walker(WholePackage(references));
    walker.AddArtifactRoots(artifactKind);
    LirReachabilityResult result;
    result.reachableDeclarations = walker.Run();
    return result;
}

LirReachabilityResult LirReachabilityAnalysis::RunModule(const LirPackage &package, const std::size_t moduleIndex,
                                                         const std::span<const LirModuleRoot> roots) {
    const LirModuleReferences references = CollectModuleReferences(package.modules[moduleIndex]);
    ReachabilityWalker walker(Graph{{&references}, {moduleIndex}});
    walker.AddModuleRoots(roots);
    LirReachabilityResult result;
    result.reachableDeclarations = walker.Run();
    return result;
}
} // namespace Rux::Optimization
