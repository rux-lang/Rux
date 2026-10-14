// Interned types: one node per structure, identity and hashing as `operator==` defines them, and cached spellings.

#include "Types/PrimitiveCatalog.h"
#include "Types/Type.h"

#include <doctest.h>
#include <functional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace Rux;

static_assert(std::is_trivially_copyable_v<TypeRef>, "a type is a handle, copied without allocating");
static_assert(sizeof(TypeRef) <= 2 * sizeof(void *), "a type is a pointer and two flags");

TEST_CASE("one structure is one interned node") {
    const TypeRef first = TypeRef::MakePointer(TypeRef::MakeNamed("Vec<int32>"));
    const TypeRef second = TypeRef::MakePointer(TypeRef::MakeNamed("Vec<int32>"));
    CHECK(first.IsSameNode(second));
    CHECK_EQ(&first.ToString(), &second.ToString());
    CHECK_EQ(first.ToString(), "*Vec<int32>");
    CHECK_FALSE(first.IsSameNode(TypeRef::MakePointer(TypeRef::MakeNamed("Vec<int64>"))));
}

TEST_CASE("leaf kinds are built in and never interned again") {
    CHECK(TypeRef{}.IsUnknown());
    CHECK_EQ(TypeRef{}.ToString(), "?");
    CHECK(TypeRef{}.IsSameNode(TypeRef::MakeUnknown()));
    CHECK(TypeRef::MakeOpaque().IsSameNode(TypeRef::Intern(TypeRef::Kind::Opaque, {}, {})));
    CHECK_EQ(TypeRef::MakeRangeFull().ToString(), "..");
    for (const PrimitiveInfo &primitive : PrimitiveCatalog()) {
        const TypeRef type = TypeRef::MakePrimitive(primitive.kind);
        CHECK_EQ(type->kind, primitive.kind);
        CHECK_EQ(type.ToString(), primitive.name);
        CHECK(type.IsSameNode(TypeRef::Intern(primitive.kind, {}, {})));
    }
}

TEST_CASE("identity leaves out what operator== ignores") {
    const TypeRef int32 = TypeRef::MakeInt32();
    TypeRef writable = int32;
    writable.isMut = true;

    // Writability behind a pointer is spelled, but is not part of the pointer's identity.
    const TypeRef pointer = TypeRef::MakePointer(int32);
    const TypeRef writablePointer = TypeRef::MakePointer(writable);
    CHECK_FALSE(pointer.IsSameNode(writablePointer));
    CHECK_EQ(writablePointer.ToString(), "*var int32");
    CHECK_EQ(pointer, writablePointer);
    CHECK_EQ(pointer.Hash(), writablePointer.Hash());

    // A reference's and a slice's writability is.
    CHECK_NE(TypeRef::MakeReference(int32), TypeRef::MakeReference(writable));
    CHECK_NE(TypeRef::MakeSlice(int32), TypeRef::MakeSlice(int32, true));

    // The handle's own flags never are.
    TypeRef variadic = TypeRef::MakeFunc({int32}, int32);
    variadic.isVariadic = true;
    variadic.isMut = true;
    CHECK_EQ(variadic, TypeRef::MakeFunc({int32}, int32));

    // A named type is its name; the layout marker it carries is not part of it.
    const TypeRef plain = TypeRef::MakeNamed("Choice");
    const TypeRef marker = TypeRef::MakeStorageMarker(16, 8);
    const TypeRef marked = plain.WithInner({&marker, 1});
    CHECK_FALSE(plain.IsSameNode(marked));
    CHECK_EQ(plain, marked);
    CHECK_EQ(plain.Hash(), marked.Hash());
    CHECK_EQ(marked.ToString(), "Choice");
    CHECK(marked.Identity().IsSameNode(plain));
    CHECK_EQ(std::hash<TypeRef>{}(marked), plain.Hash());
}

TEST_CASE("rebuilding keeps the handle's flags and reuses an unchanged node") {
    TypeRef pointer = TypeRef::MakePointer(TypeRef::MakeInt32());
    pointer.isMut = true;
    const TypeRef writable = pointer.WithWritableInner(true);
    CHECK(writable.isMut);
    CHECK_EQ(writable.ToString(), "*var int32");
    CHECK(writable.WithWritableInner(true).IsSameNode(writable));
    CHECK(writable.WithWritableInner(false).IsSameNode(TypeRef::MakePointer(TypeRef::MakeInt32())));
    CHECK(pointer.WithInner(pointer->inner).IsSameNode(pointer));
}

TEST_CASE("threads interning the same structures meet on the same nodes") {
    constexpr int threadCount = 8;
    std::vector<std::vector<TypeRef>> results(threadCount);
    std::vector<std::thread> threads;
    for (int index = 0; index < threadCount; ++index) {
        threads.emplace_back([&results, index] {
            for (int length = 0; length < 200; ++length) {
                const TypeRef element = TypeRef::MakeNamed("Concurrent" + std::to_string(length % 17));
                results[index].push_back(TypeRef::MakeOptional(TypeRef::MakeArray(element, length)));
            }
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    for (int index = 1; index < threadCount; ++index) {
        for (std::size_t position = 0; position < results[0].size(); ++position) {
            CHECK(results[index][position].IsSameNode(results[0][position]));
        }
    }
}
