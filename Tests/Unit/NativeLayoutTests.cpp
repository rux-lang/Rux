// The shared native layout: one tagged record per sum, optional, and fallible level, laid out from member layouts the
// same way by semantic analysis and code generation, with zero-sized members reserving no bytes.

#include "CodeGen/Layout.h"
#include "SemanticTestSupport.h"
#include "Types/NativeLayout.h"

#include <memory>

using namespace Rux;
using namespace Rux::Testing::SemanticTestSupport;

namespace {
/// Primitive and unit member layouts, the way both layout sources answer them.
std::optional<SizeAndAlignment> PrimitiveMember(const TypeRef &member) {
    const int size = Layout::SizeOf(member);
    return SizeAndAlignment{static_cast<std::uint64_t>(size), static_cast<std::uint64_t>(Layout::AlignOf(member))};
}

NativeLayout RequireLayout(const TypeRef &type) {
    const auto layout = ComputeNativeLayout(type, PrimitiveMember);
    REQUIRE(layout.has_value());
    return *layout;
}

struct Analyzed {
    explicit Analyzed(ParseResult result)
        : parsed(std::move(result))
        , model(SemanticAnalyzer({&parsed.module}, {}, "layouts", "Windows").Analyze()) {
    }

    ParseResult parsed;
    SemanticModel model;
};

std::unique_ptr<Analyzed> Analyze(const std::string &source) {
    Lexer lexer(source, "native_layouts.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "native_layouts.rux");
    ParseResult parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    return std::make_unique<Analyzed>(std::move(parsed));
}

const TypeRef Int32 = TypeRef::MakeInt32();
const TypeRef Optional = TypeRef::MakeOptional(TypeRef::MakeInt32());
} // namespace

TEST_CASE("each native level is a tag followed by its case's payload") {
    const NativeLayout optional = RequireLayout(Optional);
    CHECK_EQ(optional.size, 16);
    CHECK_EQ(optional.alignment, 8);
    CHECK_EQ(optional.tagOffset, 0);
    CHECK_EQ(optional.tagSize, 8);
    REQUIRE_EQ(optional.cases.size(), 2);
    CHECK_EQ(optional.cases[0].tag, NativeAbsentTag);
    CHECK_FALSE(optional.cases[0].payload.has_value());
    CHECK_EQ(optional.cases[1].tag, NativePresentTag);
    CHECK_EQ(*optional.cases[1].payload, Int32);
    CHECK_EQ(optional.cases[1].offset, 8);

    const NativeLayout fallible = RequireLayout(TypeRef::MakeFallible(TypeRef::MakeInt64(), TypeRef::MakeInt8()));
    CHECK_EQ(fallible.size, 16);
    CHECK_EQ(fallible.cases[0].tag, NativeSuccessTag);
    CHECK_EQ(fallible.cases[1].tag, NativeFailureTag);

    // A sum member takes its canonical index as its tag, and the widest member sizes the storage.
    const NativeLayout sum = RequireLayout(TypeRef::MakeSum({TypeRef::MakeInt8(), TypeRef::MakeInt64()}));
    REQUIRE_EQ(sum.cases.size(), 2);
    CHECK_EQ(sum.cases[0].tag, 0);
    CHECK_EQ(sum.cases[1].tag, 1);
    CHECK_EQ(sum.size, 16);
}

TEST_CASE("an over-aligned payload is placed at its own alignment") {
    const NativeLayout wide =
        RequireLayout(TypeRef::MakeSum({TypeRef::MakeInt8(), TypeRef::MakePrimitive(TypeRef::Kind::Int128)}));
    CHECK_EQ(wide.alignment, Layout::AlignOf(TypeRef::MakePrimitive(TypeRef::Kind::Int128)));
    for (const NativeLayout::Case &nativeCase : wide.cases) {
        CHECK_EQ(nativeCase.offset % nativeCase.layout.alignment, 0);
        CHECK_LE(nativeCase.offset + nativeCase.layout.size, wide.size);
    }
    CHECK_EQ(wide.size % wide.alignment, 0);
}

TEST_CASE("zero-sized members reserve no bytes") {
    const TypeRef unit = TypeRef::MakeUnit();
    CHECK_EQ(Layout::SizeOf(unit), 0);
    CHECK_EQ(Layout::AlignOf(unit), 1);
    // `! E` with a zero-sized error, and `()?`, are the tag alone.
    CHECK_EQ(RequireLayout(TypeRef::MakeFallible(unit, unit)).size, 8);
    CHECK_EQ(RequireLayout(TypeRef::MakeOptional(unit)).size, 8);
    CHECK_EQ(Layout::SizeOf(TypeRef::MakeTuple({unit, Int32})), 4);
    CHECK_EQ(Layout::RuntimeSizeOf(TypeRef::MakeTuple({unit, Int32}), {}, {}), 4);
    CHECK_EQ(Layout::RuntimeSizeOf(TypeRef::MakeTuple({unit, unit}), {}, {}), 0);
}

TEST_CASE("nested levels keep each inner value complete at its case offset") {
    // An optional of an optional stores the inner optional whole, with its own tag, so a borrowed inner value is read
    // in place: no level rewrites the tag of the level it holds.
    const TypeRef nested = TypeRef::MakeOptional(Optional);
    const NativeLayout outer = RequireLayout(nested);
    const NativeLayout inner = RequireLayout(Optional);
    REQUIRE(outer.cases[1].payload.has_value());
    CHECK_EQ(outer.cases[1].layout.size, inner.size);
    CHECK_EQ(outer.cases[1].layout.alignment, inner.alignment);
    CHECK_EQ(outer.size, outer.cases[1].offset + inner.size);

    const TypeRef nestedFallible = TypeRef::MakeFallible(TypeRef::MakeFallible(Int32, Int32), Optional);
    const NativeLayout channels = RequireLayout(nestedFallible);
    CHECK_EQ(channels.cases[0].layout.size, RequireLayout(TypeRef::MakeFallible(Int32, Int32)).size);
    CHECK_EQ(channels.cases[1].layout.size, inner.size);
}

TEST_CASE("only a complete native type has a layout") {
    CHECK_FALSE(ComputeNativeLayout(Int32, PrimitiveMember).has_value());
    CHECK_FALSE(ComputeNativeLayout(TypeRef::MakeNoneValue(), PrimitiveMember).has_value());
    CHECK_FALSE(ComputeNativeLayout(TypeRef::MakeFallible(Int32, TypeRef::MakeOpaque()), PrimitiveMember).has_value());
    // A sum of one repeated member is that member, which is not a native level at all.
    CHECK_FALSE(ComputeNativeLayout(TypeRef::MakeSum({Int32, Int32}), PrimitiveMember).has_value());
    // A member without a layout leaves the whole type without one.
    const auto unavailable =
        ComputeNativeLayout(Optional, [](const TypeRef &) -> std::optional<SizeAndAlignment> { return std::nullopt; });
    CHECK_FALSE(unavailable.has_value());
}

TEST_CASE("semantic layouts and runtime layouts agree for native types") {
    const auto analyzed = Analyze(R"(
        struct Done {}
        struct Holder { marker: (); value: int32; }
        func Use(optional: int32?, nested: int32??, completion: ! Done, unitOptional: ()?, sum: int8 | int64,
                 fallible: int64 ! int8, optionalError: int32 ! (int32?), items: int32?[3], pair: ((), int32),
                 holder: Holder) {}
    )");
    const std::vector<TypeRef> types = {
        Optional,
        TypeRef::MakeOptional(Optional),
        TypeRef::MakeOptional(TypeRef::MakeUnit()),
        TypeRef::MakeSum({TypeRef::MakeInt8(), TypeRef::MakeInt64()}),
        TypeRef::MakeFallible(TypeRef::MakeInt64(), TypeRef::MakeInt8()),
        TypeRef::MakeFallible(Int32, Optional),
        TypeRef::MakeArray(Optional, 3),
        TypeRef::MakeTuple({TypeRef::MakeUnit(), Int32}),
    };
    for (const TypeRef &type : types) {
        const ResolvedTypeLayout *semantic = analyzed->model.TryGetLayout(type);
        REQUIRE_MESSAGE(semantic != nullptr, "no semantic layout for ", type.ToString());
        CHECK_MESSAGE(semantic->size == static_cast<std::uint64_t>(Layout::RuntimeSizeOf(type, {}, {})),
                      type.ToString(), " sizes differ");
    }
    // Stride: an array of optionals is three whole optionals.
    CHECK_EQ(analyzed->model.TryGetLayout(TypeRef::MakeArray(Optional, 3))->size, 48);
    // A zero-sized error leaves the unit-success fallible as its tag alone.
    const ResolvedTypeLayout *completion =
        analyzed->model.TryGetLayout(TypeRef::MakeFallible(TypeRef::MakeUnit(), TypeRef::MakeNamed("Done")));
    REQUIRE(completion != nullptr);
    CHECK_EQ(completion->size, 8);
    const ResolvedTypeLayout *holder = analyzed->model.TryGetLayout(TypeRef::MakeNamed("Holder"));
    REQUIRE(holder != nullptr);
    CHECK_EQ(holder->size, 4);
}
