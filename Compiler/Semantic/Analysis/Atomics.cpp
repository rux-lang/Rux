// Calls of the atomic intrinsics: the value type each acts on, and the memory orders each accepts.
// The compiler emits an atomic operation as one machine instruction, or one exclusive-access loop, on a value of 8 to
// 64 bits, so the type argument is checked against what those instructions move. A call inside a generic body whose
// type argument mentions a type parameter is checked again for every instantiation, when the type is known.
// The orders are chosen while compiling, so each is written at the call as a `MemoryOrder` case, and the combinations
// no architecture defines are refused: a load cannot release, a store cannot acquire, and a failed compare-exchange is
// only a load.

#include "Semantic/Analysis/AnalysisContext.h"
#include "Syntax/Ast/MemoryOrder.h"

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <string_view>

namespace Rux::SemanticDetail {
namespace {
constexpr std::array<std::string_view, 11> kAtomicOperations{
    "AtomicLoad",     "AtomicStore",    "AtomicExchange", "AtomicCompareExchange",
    "AtomicFetchAdd", "AtomicFetchSub", "AtomicFetchAnd", "AtomicFetchOr",
    "AtomicFetchXor", "Fence",          "SpinHint"};

[[nodiscard]] std::string_view OrderName(const MemoryOrderKind order) noexcept {
    switch (order) {
    case MemoryOrderKind::Relaxed:
        return "Relaxed";
    case MemoryOrderKind::Acquire:
        return "Acquire";
    case MemoryOrderKind::Release:
        return "Release";
    case MemoryOrderKind::AcquireRelease:
        return "AcquireRelease";
    case MemoryOrderKind::SequentiallyConsistent:
        return "SequentiallyConsistent";
    }
    return "?";
}

// How much an order acquires: nothing, acquire, or the sequentially consistent order as well.
[[nodiscard]] int AcquireStrength(const MemoryOrderKind order) noexcept {
    switch (order) {
    case MemoryOrderKind::Relaxed:
    case MemoryOrderKind::Release:
        return 0;
    case MemoryOrderKind::Acquire:
    case MemoryOrderKind::AcquireRelease:
        return 1;
    case MemoryOrderKind::SequentiallyConsistent:
        return 2;
    }
    return 0;
}
} // namespace

bool AnalysisContext::IsAtomicIntrinsic(const std::string_view intrinsicName) {
    return std::ranges::find(kAtomicOperations, intrinsicName) != kAtomicOperations.end();
}

void AnalysisContext::CheckAtomicValueType(const TypeRef &type, const std::string_view operation,
                                           const SourceLocation location) {
    if (type.IsUnknown()) {
        return;
    }
    const bool arithmetic = operation == "AtomicFetchAdd" || operation == "AtomicFetchSub";
    const bool bitwise = operation == "AtomicFetchAnd" || operation == "AtomicFetchOr" || operation == "AtomicFetchXor";
    const std::uint64_t size = type.SizeInBytes().value_or(0);
    const bool word = size == 1 || size == 2 || size == 4 || size == 8;
    bool accepted = false;
    std::string expected = "an integer of 8 to 64 bits, 'bool', or a pointer";
    if (arithmetic) {
        accepted = type.IsInteger() && word;
        expected = "an integer of 8 to 64 bits";
    }
    else if (bitwise) {
        accepted = (type.IsInteger() && word) || type->kind == TypeRef::Kind::Bool8;
        expected = "an integer of 8 to 64 bits, or 'bool'";
    }
    else {
        accepted =
            (type.IsInteger() && word) || type->kind == TypeRef::Kind::Bool8 || type->kind == TypeRef::Kind::Pointer;
    }
    if (!accepted) {
        EmitError(location, std::format("'{}' cannot act on type '{}'", operation, type.ToString()),
                  {std::format("its type argument must be {}", expected)});
    }
}

void AnalysisContext::CheckAtomicCall(const CallExpr &call, const FuncDecl &declaration,
                                      const std::unordered_map<std::string, TypeRef> &substitutions) {
    const std::string_view operation = declaration.intrinsicName;
    if (const auto value = substitutions.find("T"); value != substitutions.end()) {
        if (MentionsTypeParameter(value->second)) {
            if (currentFunctionDecl) {
                deferredAtomicChecks[currentFunctionDecl].push_back(
                    {value->second, std::string(operation), call.location});
            }
        }
        else {
            CheckAtomicValueType(value->second, operation, call.location);
        }
    }

    // Every `MemoryOrder` parameter takes a case written at the call.
    std::vector<MemoryOrderKind> orders;
    for (std::size_t index = 0; index < declaration.params.size() && index < call.args.size(); ++index) {
        // The orders are the parameters Core names for them; the declaration's own types mention `T`, which only its
        // scope can resolve.
        const std::string &name = declaration.params[index].name;
        if (name != "order" && name != "success" && name != "failure") {
            continue;
        }
        const auto order = MemoryOrderOf(*call.args[index]);
        if (!order) {
            EmitError(call.args[index]->location,
                      std::format("the memory order of '{}' must be written as a 'MemoryOrder' case", operation), {},
                      "write the order at the call, such as 'MemoryOrder::Acquire'");
            return;
        }
        orders.push_back(*order);
    }

    const auto refuse = [&](const MemoryOrderKind order, const std::string_view why) {
        EmitError(call.location, std::format("'{}' cannot use memory order '{}'", operation, OrderName(order)),
                  {std::string(why)});
    };
    if (operation == "AtomicLoad" && !orders.empty() &&
        (orders[0] == MemoryOrderKind::Release || orders[0] == MemoryOrderKind::AcquireRelease)) {
        refuse(orders[0], "a load only reads, so it cannot release");
    }
    if (operation == "AtomicStore" && !orders.empty() &&
        (orders[0] == MemoryOrderKind::Acquire || orders[0] == MemoryOrderKind::AcquireRelease)) {
        refuse(orders[0], "a store only writes, so it cannot acquire");
    }
    if (operation == "Fence" && !orders.empty() && orders[0] == MemoryOrderKind::Relaxed) {
        refuse(orders[0], "a relaxed fence would order nothing");
    }
    if (operation == "AtomicCompareExchange" && orders.size() == 2) {
        if (orders[1] == MemoryOrderKind::Release || orders[1] == MemoryOrderKind::AcquireRelease) {
            refuse(orders[1], "a compare-exchange that fails only reads, so its failure order cannot release");
        }
        else if (AcquireStrength(orders[1]) > AcquireStrength(orders[0])) {
            refuse(orders[1], "the failure order cannot acquire more than the success order");
        }
    }
}

void AnalysisContext::ValidateDeferredAtomicChecks(const FuncDecl &declaration,
                                                   const std::unordered_map<std::string, TypeRef> &substitutions) {
    if (const auto it = deferredAtomicChecks.find(&declaration); it != deferredAtomicChecks.end()) {
        for (const DeferredAtomicCheck &check : it->second) {
            CheckAtomicValueType(SubstituteTypeParameters(check.value, substitutions), check.operation, check.location);
        }
    }
}
} // namespace Rux::SemanticDetail
