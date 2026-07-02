#pragma once

// LIR-level type sizing, struct layout, and the x64 argument-register tables
// shared by the assembly and RCU-object code generators.

#include "Frontend/Sema/Type.h"
#include "Ir/Lir/Lir.h"
#include "Support/Layout.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Rux::Layout {

// Size in bytes of a LIR-level type. Aggregates follow the shared layout
// rule; a named type resolves through its attached inner type when present,
// otherwise Slice is a fat pointer (16) and anything else defaults to
// pointer-sized (8).
[[nodiscard]] int SizeOf(const TypeRef &t);

[[nodiscard]] bool IsFloat(const TypeRef &t);

// Strip a generic argument list: "Foo<int32>" -> "Foo".
[[nodiscard]] std::string BaseTypeName(const std::string &name);

// Struct field layout
struct FieldLayout {
    std::string name;
    int offset = 0;
    int size = 0;
};

struct StructLayout {
    std::vector<FieldLayout> fields;
    int totalSize = 0;
    int alignment = 1;
};

using LayoutMap = std::unordered_map<std::string, StructLayout>;

// Compute the layout of `s`, resolving named field types through `known`.
[[nodiscard]] StructLayout ComputeStructLayout(const LirStructDecl &s, const LayoutMap &known);

// System V AMD64 integer argument registers (in order)
inline constexpr std::string_view kIntArgRegs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
// Microsoft x64 integer argument registers (in order)
inline constexpr std::string_view kWin64IntArgRegs[] = {"rcx", "rdx", "r8", "r9"};
// System V AMD64 float argument registers (in order)
inline constexpr std::string_view kFltArgRegs[] = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"};

} // namespace Rux::Layout
