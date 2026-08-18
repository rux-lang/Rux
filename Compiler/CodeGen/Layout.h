#pragma once

// LIR-level type sizing, struct layout, and the x86-64 argument-register tables
// shared by the assembly and RCU-object code generators.

#include "Ir/Lir/Lir.h"
#include "Semantic/Type.h"
#include "Target/Layout.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux::Layout {
/// Size in bytes of a LIR-level type. Aggregates follow the shared layout rule; a named type resolves through its
/// attached inner type when present, otherwise Slice is a fat pointer (16) and anything else defaults to pointer-sized
/// (8).
[[nodiscard]] int SizeOf(const TypeRef &t);

/// Natural alignment of a LIR-level type. Arrays inherit their element's alignment rather than deriving it from their
/// total byte size.
[[nodiscard]] int AlignOf(const TypeRef &t);

[[nodiscard]] bool IsFloat(const TypeRef &t);

/// Alias that makes hardware-register decisions explicit at call sites.
[[nodiscard]] bool IsNativeFloat(const TypeRef &t);

/// Float widths lowered through software kernels and passed as raw integer/aggregate storage.
[[nodiscard]] bool IsSoftwareFloat(const TypeRef &t);

/// Multiword integer primitives travel through stack storage and aggregate ABI lanes rather than one machine register.
[[nodiscard]] bool IsWideInteger(const TypeRef &t);

/// The unsigned integer type of the same width, which is how a logical right shift reads its operand: `lshr` on a
/// signed type is the one place a value's bits are widened by its width rather than by its signedness. Anything that is
/// not a signed integer already reads that way and is returned unchanged.
[[nodiscard]] TypeRef UnsignedIntegerType(const TypeRef &type);

/// Strip a generic argument list: "Foo<int32>" -> "Foo".
[[nodiscard]] std::string BaseTypeName(const std::string &name);

/// Encode decoded string bytes as fixed-width elements. InternStr appends the final byte of the width-sized NUL
/// terminator.
[[nodiscard]] std::string EncodeStringLiteral(std::string_view value, int elementSize);

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

/// Compute the layout of `s`, resolving named field types through `known`.
[[nodiscard]] StructLayout ComputeStructLayout(const LirStructDecl &s, const LayoutMap &known);

/// Size of a LIR type as the running program lays it out, which is what a stack slot and a copy are measured in. SizeOf
/// alone cannot answer for a named type: an interface value is a fat pointer whether or not the module declaring it is
/// in hand, and a struct is whatever its computed layout came to. Both back ends size their frames by this, so the rule
/// is stated once here rather than beside each of them.
[[nodiscard]] int RuntimeSizeOf(const TypeRef &t, const LayoutMap &layouts,
                                const std::unordered_set<std::string> &interfaceNames);

/// Byte offset of `fieldName` inside the value `pointerType` addresses, which is what a FieldPtr adds to its base.
/// `pointerType` is the type the base register holds, so a pointer is expected and anything else has no field to find.
///
/// Four shapes answer, in the order a name is looked up in them: a range, whose bounds are its fields; a tuple, whose
/// field name is a decimal index; the two runtime fat pointers, an interface value and a slice, whose field names
/// belong to the runtime rather than to any declaration; and a struct, which answers from its computed layout. A name
/// none of them has is offset zero, which is the start of the value itself.
[[nodiscard]] int FieldOffsetOf(const TypeRef &pointerType, std::string_view fieldName, const LayoutMap &layouts,
                                const std::unordered_set<std::string> &interfaceNames);

// System V AMD64 integer argument registers (in order)
inline constexpr std::string_view kIntArgRegs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
// Microsoft x64 integer argument registers (in order)
inline constexpr std::string_view kWin64IntArgRegs[] = {"rcx", "rdx", "r8", "r9"};
// System V AMD64 float argument registers (in order)
inline constexpr std::string_view kFltArgRegs[] = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"};
} // namespace Rux::Layout
