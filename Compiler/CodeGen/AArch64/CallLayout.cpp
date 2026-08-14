#include "CodeGen/AArch64/CallLayout.h"

namespace Rux {
using namespace Layout;

namespace {
constexpr unsigned kGeneralArgumentRegisters = 8;
constexpr unsigned kVectorArgumentRegisters = 8;
constexpr unsigned kMaxHfaMembers = 4;
} // namespace

AArch64CallPlanner::AArch64CallPlanner(const LayoutMap &inputLayouts,
                                       const std::unordered_set<std::string> &inputInterfaceNames,
                                       const std::vector<LirStructDecl> &structDecls, const Target::OS inputTargetOs)
    : layouts(inputLayouts)
    , interfaceNames(inputInterfaceNames)
    , targetOs(inputTargetOs)
    , policy(AArch64CallPolicyFor(inputTargetOs)) {
    for (const auto &declaration : structDecls) {
        structFields[declaration.name] = &declaration;
    }
}

int AArch64CallPlanner::RuntimeSize(const TypeRef &type) const {
    return RuntimeSizeOf(type, layouts, interfaceNames);
}

int AArch64CallPlanner::RuntimeAlign(const TypeRef &type) const {
    if (!type.IsRange() && type.kind == TypeRef::Kind::Named) {
        const std::string base = BaseTypeName(type.name);
        if (interfaceNames.contains(base)) {
            return 8;
        }
        if (const auto found = layouts.find(base); found != layouts.end()) {
            return found->second.alignment;
        }
    }
    return AlignOf(type);
}

bool AArch64CallPlanner::IsAggregate(const TypeRef &type) const {
    if (type.IsRange()) {
        return true;
    }
    switch (type.kind) {
    case TypeRef::Kind::Tuple:
    case TypeRef::Kind::Array:
        return true;
    case TypeRef::Kind::Named: {
        const std::string base = BaseTypeName(type.name);
        return base == "Slice" || interfaceNames.contains(base) || layouts.contains(base) ||
               (!type.inner.empty() && SizeOf(type) > 8);
    }
    default:
        return false;
    }
}

bool AArch64CallPlanner::CollectFloatMembers(const TypeRef &type, TypeRef::Kind &memberKind, unsigned &count) const {
    if (count > kMaxHfaMembers) {
        return false;
    }
    if (IsFloat(type)) {
        if (count == 0) {
            memberKind = type.kind;
        }
        else if (type.kind != memberKind) {
            return false;
        }
        ++count;
        return true;
    }
    switch (type.kind) {
    case TypeRef::Kind::Tuple:
        for (const auto &element : type.inner) {
            if (!CollectFloatMembers(element, memberKind, count)) {
                return false;
            }
        }
        return !type.inner.empty();
    case TypeRef::Kind::Array:
        if (type.inner.empty() || !type.arrayLength || *type.arrayLength == 0) {
            return false;
        }
        for (std::uint64_t index = 0; index < *type.arrayLength; ++index) {
            if (!CollectFloatMembers(type.inner[0], memberKind, count)) {
                return false;
            }
        }
        return true;
    case TypeRef::Kind::Named: {
        const std::string base = BaseTypeName(type.name);
        if (interfaceNames.contains(base)) {
            return false;
        }
        const auto declaration = structFields.find(base);
        if (declaration == structFields.end()) {
            return false;
        }
        for (const auto &field : declaration->second->fields) {
            if (!CollectFloatMembers(field.type, memberKind, count)) {
                return false;
            }
        }
        return !declaration->second->fields.empty();
    }
    default:
        return false;
    }
}

unsigned AArch64CallPlanner::FloatMembers(const TypeRef &type, unsigned &memberBytes) const {
    TypeRef::Kind memberKind = TypeRef::Kind::Float64;
    unsigned count = 0;
    if (!CollectFloatMembers(type, memberKind, count) || count == 0 || count > kMaxHfaMembers) {
        return 0;
    }
    memberBytes = memberKind == TypeRef::Kind::Float32 ? 4 : 8;
    return count;
}

bool AArch64CallPlanner::ReturnsInMemory(const TypeRef &type) const {
    unsigned memberBytes = 8;
    return IsAggregate(type) && RuntimeSize(type) > 16 && FloatMembers(type, memberBytes) == 0;
}

AArch64ArgumentLocation AArch64CallPlanner::PlanResult(const TypeRef &type) const {
    AArch64ArgumentLocation location;
    unsigned memberBytes = 8;
    if (const unsigned members = FloatMembers(type, memberBytes); members > 0) {
        location.kind = AArch64ArgumentLocation::Kind::Vector;
        location.count = members;
        location.memberBytes = memberBytes;
        return location;
    }
    location.kind = AArch64ArgumentLocation::Kind::General;
    location.count = RegistersFor(RuntimeSize(type));
    return location;
}

unsigned AArch64CallPlanner::RegistersFor(const int size) {
    return size > 8 ? static_cast<unsigned>((size + 7) / 8) : 1;
}

AArch64CallLayout AArch64CallPlanner::PlanWindowsVariadicArguments(const std::vector<TypeRef> &types) const {
    AArch64CallLayout layout;
    layout.args.reserve(types.size());
    layout.windowsVariadic = true;
    std::int32_t nextSlot = 0;

    for (const TypeRef &type : types) {
        const int size = RuntimeSize(type);
        AArch64ArgumentLocation location;
        location.kind = AArch64ArgumentLocation::Kind::Slots;
        if (IsAggregate(type) && size > 16) {
            location.byReference = true;
            location.copyBytes = size;
            location.bytes = 8;
        }
        else {
            location.bytes = AlignUp(std::max(size, 1), 8);
        }
        const int alignment = location.byReference ? 8 : std::clamp(RuntimeAlign(type), 8, 16);
        nextSlot = AlignUp(nextSlot, alignment);
        location.offset = nextSlot;
        nextSlot += location.bytes;
        layout.args.push_back(location);
    }

    std::int32_t nextStack = std::max(nextSlot - static_cast<std::int32_t>(kGeneralArgumentRegisters * 8), 0);
    for (std::size_t index = 0; index < layout.args.size(); ++index) {
        if (!layout.args[index].byReference) {
            continue;
        }
        const int alignment = std::clamp(RuntimeAlign(types[index]), 8, 16);
        nextStack = AlignUp(nextStack, alignment);
        layout.args[index].copyOffset = nextStack;
        nextStack += AlignUp(std::max(layout.args[index].copyBytes, 1), 8);
    }
    layout.areaBytes = AlignUp(nextStack, 16);
    return layout;
}

AArch64CallLayout AArch64CallPlanner::PlanArguments(const std::vector<TypeRef> &types,
                                                    const std::optional<std::uint32_t> fixedParamCount) const {
    if (targetOs == Target::OS::Windows && fixedParamCount) {
        return PlanWindowsVariadicArguments(types);
    }

    AArch64CallLayout layout;
    layout.args.reserve(types.size());
    unsigned nextGeneral = 0;
    unsigned nextVector = 0;
    std::int32_t nextStack = 0;
    const bool appleVariadic = fixedParamCount && targetOs == Target::OS::MacOS;

    const auto onStack = [this, &nextStack](const int size, const int naturalAlignment) {
        AArch64ArgumentLocation location;
        location.kind = AArch64ArgumentLocation::Kind::Stack;
        const int alignment = policy.StackAlignment(naturalAlignment);
        nextStack = AlignUp(nextStack, alignment);
        location.offset = nextStack;
        location.bytes = policy.StackBytes(size);
        nextStack += location.bytes;
        return location;
    };

    const auto inGeneralFile = [&](const int size, const int alignment, const unsigned needed) {
        nextGeneral = policy.FirstGeneralRegister(nextGeneral, alignment);
        if (nextGeneral + needed <= kGeneralArgumentRegisters) {
            AArch64ArgumentLocation location;
            location.kind = AArch64ArgumentLocation::Kind::General;
            location.first = nextGeneral;
            location.count = needed;
            nextGeneral += needed;
            return location;
        }
        nextGeneral = kGeneralArgumentRegisters;
        return onStack(size, alignment);
    };

    for (std::size_t index = 0; index < types.size(); ++index) {
        const TypeRef &type = types[index];
        const int size = RuntimeSize(type);
        const int alignment = std::clamp(RuntimeAlign(type), 1, 16);

        if (appleVariadic && index >= *fixedParamCount) {
            if (index == *fixedParamCount) {
                nextStack = AlignUp(nextStack, 8);
            }
            AArch64ArgumentLocation location =
                onStack(IsAggregate(type) && size > 16 ? 8 : AlignUp(std::max(size, 1), 8), std::max(alignment, 8));
            if (IsAggregate(type) && size > 16) {
                location.byReference = true;
                location.copyBytes = size;
            }
            layout.args.push_back(location);
            continue;
        }

        unsigned memberBytes = 8;
        const unsigned members = FloatMembers(type, memberBytes);
        if (members > 0) {
            if (nextVector + members <= kVectorArgumentRegisters) {
                AArch64ArgumentLocation location;
                location.kind = AArch64ArgumentLocation::Kind::Vector;
                location.first = nextVector;
                location.count = members;
                location.memberBytes = memberBytes;
                nextVector += members;
                layout.args.push_back(location);
                continue;
            }
            nextVector = kVectorArgumentRegisters;
            layout.args.push_back(onStack(size, alignment));
            continue;
        }
        if (IsAggregate(type) && size > 16) {
            AArch64ArgumentLocation location = inGeneralFile(8, 8, 1);
            location.byReference = true;
            location.copyBytes = size;
            layout.args.push_back(location);
            continue;
        }
        layout.args.push_back(inGeneralFile(size, alignment, RegistersFor(size)));
    }

    for (std::size_t index = 0; index < layout.args.size(); ++index) {
        if (!layout.args[index].byReference) {
            continue;
        }
        const int alignment = policy.StackAlignment(RuntimeAlign(types[index]));
        nextStack = AlignUp(nextStack, alignment);
        layout.args[index].copyOffset = nextStack;
        nextStack += AlignUp(std::max(layout.args[index].copyBytes, 1), 8);
    }
    layout.areaBytes = AlignUp(nextStack, 16);
    return layout;
}
} // namespace Rux
