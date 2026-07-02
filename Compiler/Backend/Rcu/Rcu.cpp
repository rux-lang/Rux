// Public RCU API: drives code generation, binary emission, and dumping.

#include "Backend/Rcu/Rcu.h"

#include "Backend/Rcu/RcuInternal.h"

#include <fstream>
#include <utility>

namespace Rux {

// Public API
Rcu::Rcu(LirPackage package, std::string packageName)
    : lir(std::move(package))
    , packageName(std::move(packageName)) {
}

std::vector<RcuFile> Rcu::Generate() const {
    std::vector<RcuFile> result;
    result.reserve(lir.modules.size());
    std::vector<LirStructDecl> structDecls;
    std::vector<std::string> interfaceNames;
    for (const auto &mod : lir.modules) {
        for (const auto &s : mod.structs) {
            structDecls.push_back(s);
        }
        for (const auto &name : mod.interfaceNames) {
            interfaceNames.push_back(name);
        }
    }
    for (const auto &mod : lir.modules) {
        result.push_back(GenerateRcuModule(mod, structDecls, interfaceNames, packageName));
    }
    return result;
}

bool Rcu::Emit(const RcuFile &file, const std::filesystem::path &path) {
    const auto bytes = SerializeRcuFile(file);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return f.good();
}

bool Rcu::Dump(const RcuFile &file, const std::filesystem::path &path) {
    const std::string text = DumpRcuFileText(file);
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f) {
        return false;
    }
    f << text;
    return f.good();
}
} // namespace Rux
