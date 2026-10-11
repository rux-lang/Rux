#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
/// Reads each field of a by-address parameter once, on entry, when the function only ever reads it.
///
/// A slice or interface parameter arrives as the address of the caller's value, but the parameter itself is a value:
/// a by-value parameter cannot be written, so its fields hold what they held at the call for the whole call. Every
/// load of a field then reads that same value, and one load in the entry block can serve them all. This keeps a
/// slice's data pointer and length in registers across stores the function makes through any other pointer, which
/// alias analysis alone could not allow, since the caller's value may be reachable through those pointers too.
/// A parameter whose address is used for anything but field reads and call arguments keeps its loads.
class LirViewParameters final : public LirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(LirPackage &package, const PassContext &context) override;
};
} // namespace Rux::Optimization
