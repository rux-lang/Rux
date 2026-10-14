#pragma once

#include "Ir/Lir/Lir.h"

#include <unordered_set>

namespace Rux::Optimization {
/// Thin references to concrete structures are scalar addresses. Unknown names and interface references are not
/// inferred to be thin: interfaces carry a data/vtable pair. Rebuild this snapshot when declarations change.
class ConcreteReferenceTypes {
public:
    explicit ConcreteReferenceTypes(const LirPackage &package) {
        for (const auto &module : package.modules)
            for (const auto &structure : module.structs)
                if (structure.typeParams.empty())
                    names.insert(structure.name);
        for (const auto &module : package.modules)
            for (const auto &name : module.interfaceNames)
                names.erase(name);
    }

    [[nodiscard]] bool Contains(const TypeRef &type) const {
        return type->kind == TypeRef::Kind::Reference && type->inner.size() == 1 &&
               type->inner[0]->kind == TypeRef::Kind::Named && names.contains(type->inner[0]->name);
    }

private:
    std::unordered_set<std::string> names;
};
} // namespace Rux::Optimization
