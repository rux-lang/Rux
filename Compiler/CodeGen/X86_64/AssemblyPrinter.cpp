// Textual assembly output for x86-64, the entry point shared by the module,
// instruction and control-flow printers beside it. The text is for reading;
// nothing assembles it back.

#include "CodeGen/X86_64/AssemblyPrinter.h"

#include "CodeGen/Layout.h"
#include "CodeGen/X86_64/AssemblyControlFlowPrinter.h"
#include "CodeGen/X86_64/AssemblyModulePrinter.h"

#include <fstream>
#include <unordered_set>
#include <utility>

namespace Rux {
namespace {
[[nodiscard]] std::string GenerateAssembly(const LirPackage &package, const Target::OS targetOs,
                                           const bool machineSelection) {
    Layout::LayoutMap layouts;
    std::unordered_set<std::string> interfaceNames;
    std::vector<LirStructDecl> allStructs;
    for (const auto &module : package.modules) {
        for (const auto &name : module.interfaceNames) {
            interfaceNames.insert(name);
        }
        allStructs.insert(allStructs.end(), module.structs.begin(), module.structs.end());
    }
    Layout::BuildStructLayouts(allStructs, layouts, interfaceNames);

    AssemblyModulePrinter modulePrinter(targetOs);
    AssemblyControlFlowPrinter functionPrinter(modulePrinter, layouts, interfaceNames, targetOs);
    functionPrinter.machineSelection = machineSelection;
    for (const auto &module : package.modules) {
        for (const auto &function : module.funcs) {
            functionPrinter.tracesPanics = functionPrinter.tracesPanics || function.name == kBacktraceFunctionName;
        }
    }
    for (const auto &module : package.modules) {
        modulePrinter.EmitModuleData(module, layouts, interfaceNames);
        for (const auto &function : module.funcs) {
            if (modulePrinter.DeclareFunction(function)) {
                functionPrinter.EmitFunction(function);
            }
        }
    }
    return modulePrinter.Finalize();
}
} // namespace

AssemblyPrinter::AssemblyPrinter(LirPackage package, const Target::OS inputTargetOs)
    : lir(std::move(package))
    , targetOs(inputTargetOs) {
}

std::string AssemblyPrinter::Generate() const {
    return GenerateAssembly(lir, targetOs, false);
}

bool AssemblyPrinter::Emit(const LirPackage &package, const std::filesystem::path &path, const Target::OS targetOs,
                           const bool machineSelection) {
    const std::string text = GenerateAssembly(package, targetOs, machineSelection);
    std::ofstream file(path, std::ios::out | std::ios::trunc);
    if (!file) {
        return false;
    }
    file << text;
    return file.good();
}
} // namespace Rux
