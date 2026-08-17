#pragma once

#include "CodeGen/Layout.h"
#include "Ir/Lir/Lir.h"
#include "Target/Target.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux {
struct AArch64CallLayoutPolicy {
    bool compactStackArguments = false;
    bool alignWideGeneralArgumentsToEvenRegister = true;
    bool callerExtendsNarrowIntegers = false;

    [[nodiscard]] constexpr int StackAlignment(const int naturalAlignment) const noexcept {
        return compactStackArguments ? std::clamp(naturalAlignment, 1, 16) : std::clamp(naturalAlignment, 8, 16);
    }

    [[nodiscard]] constexpr int StackBytes(const int size) const noexcept {
        const int bytes = std::max(size, 1);
        return compactStackArguments ? bytes : (bytes + 7) / 8 * 8;
    }

    [[nodiscard]] constexpr unsigned FirstGeneralRegister(const unsigned next, const int alignment) const noexcept {
        if (alignment >= 16 && alignWideGeneralArgumentsToEvenRegister) {
            return next + next % 2;
        }
        return next;
    }
};

[[nodiscard]] constexpr AArch64CallLayoutPolicy AArch64CallPolicyFor(const Target::OS os) noexcept {
    if (os == Target::OS::MacOS) {
        return AArch64CallLayoutPolicy{
            .compactStackArguments = true,
            .alignWideGeneralArgumentsToEvenRegister = false,
            .callerExtendsNarrowIntegers = true,
        };
    }
    return {};
}

/// One argument's target location. A value larger than sixteen bytes travels as the address of a caller-owned copy;
/// copyOffset and copyBytes name that copy inside the same outgoing area.
struct AArch64ArgumentLocation {
    enum class Kind : std::uint8_t {
        General,
        Vector,
        Stack,
        Slots, // Windows C-variadic imaginary-stack slots
    };

    Kind kind = Kind::General;
    unsigned first = 0;
    unsigned count = 1;
    unsigned memberBytes = 8;
    bool byReference = false;
    std::int32_t offset = 0;
    std::int32_t bytes = 8;
    std::int32_t copyOffset = 0;
    std::int32_t copyBytes = 0;
};

/// A complete placement plan for one call's arguments and copies.
struct AArch64CallLayout {
    std::vector<AArch64ArgumentLocation> args;
    std::int32_t areaBytes = 0;
    bool windowsVariadic = false;
};

/// Pure target-driven AAPCS64 argument and result classification. The planner reads type metadata and produces
/// locations only; instruction emission owns no ABI placement decisions.
class AArch64CallPlanner {
public:
    AArch64CallPlanner(const Layout::LayoutMap &layouts, const std::unordered_set<std::string> &interfaceNames,
                       const std::vector<LirStructDecl> &structDecls, Target::OS targetOs);

    [[nodiscard]] AArch64CallLayout PlanArguments(const std::vector<TypeRef> &types,
                                                  std::optional<std::uint32_t> fixedParamCount = std::nullopt) const;

    [[nodiscard]] AArch64ArgumentLocation PlanResult(const TypeRef &type) const;

    [[nodiscard]] bool ReturnsInMemory(const TypeRef &type) const;

    [[nodiscard]] const AArch64CallLayoutPolicy &Policy() const {
        return policy;
    }

private:
    const Layout::LayoutMap &layouts;
    const std::unordered_set<std::string> &interfaceNames;
    std::unordered_map<std::string, const LirStructDecl *> structFields;
    Target::OS targetOs;
    AArch64CallLayoutPolicy policy;

    [[nodiscard]] int RuntimeSize(const TypeRef &type) const;
    [[nodiscard]] int RuntimeAlign(const TypeRef &type) const;
    [[nodiscard]] bool IsAggregate(const TypeRef &type) const;
    [[nodiscard]] bool CollectFloatMembers(const TypeRef &type, TypeRef::Kind &memberKind, unsigned &count) const;
    [[nodiscard]] unsigned FloatMembers(const TypeRef &type, unsigned &memberBytes) const;
    [[nodiscard]] AArch64CallLayout PlanWindowsVariadicArguments(const std::vector<TypeRef> &types) const;
    [[nodiscard]] static unsigned RegistersFor(int size);
};
} // namespace Rux
