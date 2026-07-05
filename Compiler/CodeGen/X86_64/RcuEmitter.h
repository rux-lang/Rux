#pragma once

#include "Ir/Lir/Lir.h"

#include <string>
#include <vector>

#include "Object/Rcu/Rcu.h"

namespace Rux {

class RcuEmitter {
public:
    explicit RcuEmitter(const LirPackage &package, std::string inputPackageName = {});
    [[nodiscard]] std::vector<RcuFile> Generate() const;

private:
    const LirPackage &lir;
    std::string packageName;
};

} // namespace Rux
