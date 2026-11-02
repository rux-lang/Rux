#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>

using namespace Rux;

TEST_CASE("semantic model recursively classifies copy move-only and droppable types") {
    Lexer lexer(R"(
        struct Handle {
            value: int32;
        }
        extend Handle {
            func ~Handle(&var self) {}
        }

        struct Wrapper {
            handle: Handle;
        }

        struct Pair<T> {
            value: T;
            tag: int32;
        }

        struct GenericOwner<T> {
            value: T;
        }
        extend GenericOwner<T> {
            func ~GenericOwner(&var self) {}
        }

        variant Maybe<T> {
            None,
            Some(T)
        }

        union Bits {
            signed: int32,
            unsigned: uint32
        }

        func Identity<T>(value: T) {}

        func TransferGenericOwner<T>(owner: GenericOwner<T>) {}

        func Observe(copy: Pair<int32>, moved: Pair<Handle>, wrapped: Wrapper, choice: Maybe<Handle>,
                     bits: Bits, pointer: *Handle, array: Handle[2], tuple: (Handle, int32)) {}
    )",
                "type_properties.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "type_properties.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const FuncDecl *identity = nullptr;
    const FuncDecl *transferGenericOwner = nullptr;
    const FuncDecl *observe = nullptr;
    for (const auto &item : parsed.module.items) {
        const auto *function = dynamic_cast<const FuncDecl *>(item.get());
        if (function && function->name == "Identity") {
            identity = function;
        }
        if (function && function->name == "Observe") {
            observe = function;
        }
        if (function && function->name == "TransferGenericOwner") {
            transferGenericOwner = function;
        }
    }
    REQUIRE(identity != nullptr);
    REQUIRE(transferGenericOwner != nullptr);
    REQUIRE(observe != nullptr);

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    const auto propertiesOf = [&](const Param &parameter) -> const TypeProperties & {
        const TypeProperties *properties = model.TryGetProperties(*parameter.type);
        REQUIRE(properties != nullptr);
        return *properties;
    };

    REQUIRE_EQ(identity->params.size(), 1);
    const TypeProperties &unresolved = propertiesOf(identity->params[0]);
    CHECK_FALSE(unresolved.IsResolved());
    CHECK_FALSE(unresolved.IsDroppable());

    REQUIRE_EQ(transferGenericOwner->params.size(), 1);
    const TypeProperties &genericOwner = propertiesOf(transferGenericOwner->params[0]);
    CHECK(genericOwner.IsMoveOnly());
    CHECK(genericOwner.IsMovable());
    CHECK(genericOwner.IsDroppable());

    REQUIRE_EQ(observe->params.size(), 8);
    const TypeProperties &copy = propertiesOf(observe->params[0]);
    CHECK(copy.IsCopy());
    CHECK_FALSE(copy.IsDroppable());

    for (const std::size_t index : {1U, 2U, 3U, 6U, 7U}) {
        const TypeProperties &properties = propertiesOf(observe->params[index]);
        CHECK(properties.IsMoveOnly());
        CHECK(properties.IsDroppable());
    }

    for (const std::size_t index : {4U, 5U}) {
        const TypeProperties &properties = propertiesOf(observe->params[index]);
        CHECK(properties.IsCopy());
        CHECK_FALSE(properties.IsDroppable());
    }

    CHECK(model.TryGetProperties(TypeRef::MakeNamed("NeverObserved")) == nullptr);
}

TEST_CASE("an interface named Drop has no lifecycle semantics") {
    Lexer lexer(R"(
        interface Drop {
            func Drop(&var self);
        }

        struct Plain {
            value: int32;
        }
        extend Plain : Drop {
            func Drop(&var self) {}
        }

        func Observe(value: Plain) {}
    )",
                "ordinary_drop_interface.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "ordinary_drop_interface.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const FuncDecl *observe = nullptr;
    for (const auto &item : parsed.module.items) {
        if (const auto *function = dynamic_cast<const FuncDecl *>(item.get());
            function && function->name == "Observe") {
            observe = function;
        }
    }
    REQUIRE(observe != nullptr);

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    const TypeProperties *properties = model.TryGetProperties(*observe->params[0].type);
    REQUIRE(properties != nullptr);
    CHECK(properties->IsCopy());
    CHECK_FALSE(properties->IsDroppable());
    CHECK(model.TryGetDropGlue(TypeRef::MakeNamed("Plain")) == nullptr);
}

TEST_CASE("special operations and lifecycle attributes classify generated and prohibited capabilities") {
    Lexer lexer(R"(
        interface Assignable {
        }

        struct Handle {
            value: int32;
        }
        extend Handle {
            func ~Handle(&var self) {}
        }

        struct Generated {
            value: int32;
        }

        struct Wrapper<T> {
            value: T;
        }

        struct Owning {
            value: int32;
        }
        extend Owning {
            func ~Owning(&var self) {}
        }

        #NoCopy
        #NoMove
        struct Pinned {
            value: int32;
        }
        extend Pinned {
        }

        struct ContainsPinned {
            value: Pinned;
        }

        #NoCopy
        struct Raw {
            fd: int32;
        }

        #NoMove
        struct Anchored {
            next: *var Anchored;
        }

        struct ContainsAnchored {
            anchor: Anchored;
        }

        #NoMove
        #NoCopy
        variant Spelled { Only }

        func Observe(generated: Generated, genericCopy: Wrapper<int32>, genericMove: Wrapper<Handle>,
                     owning: Owning, pinned: Pinned, nested: ContainsPinned, raw: Raw, anchored: Anchored,
                     holdsAnchored: ContainsAnchored, spelled: Spelled) {}
    )",
                "special_operations.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "special_operations.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const FuncDecl *observe = nullptr;
    for (const auto &item : parsed.module.items) {
        if (const auto *function = dynamic_cast<const FuncDecl *>(item.get());
            function && function->name == "Observe") {
            observe = function;
        }
    }
    REQUIRE(observe != nullptr);

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    const auto propertiesOf = [&](const std::size_t parameter) -> const TypeProperties & {
        const TypeProperties *properties = model.TryGetProperties(*observe->params[parameter].type);
        REQUIRE(properties != nullptr);
        return *properties;
    };
    using Operation = TypeProperties::SpecialOperationState;

    for (const std::size_t parameter : {0U, 1U}) {
        const TypeProperties &properties = propertiesOf(parameter);
        CHECK(properties.IsCopy());
        CHECK(properties.IsMovable());
        CHECK_EQ(properties.copyOperation, Operation::Generated);
        CHECK_EQ(properties.moveOperation, Operation::Generated);
    }

    const TypeProperties &genericMove = propertiesOf(2);
    CHECK(genericMove.IsMoveOnly());
    CHECK(genericMove.IsMovable());
    CHECK_EQ(genericMove.copyOperation, Operation::Prohibited);
    CHECK_EQ(genericMove.moveOperation, Operation::Generated);

    // A destructor alone makes a type move-only: it owns resources, so it moves and never copies.
    const TypeProperties &owning = propertiesOf(3);
    CHECK(owning.IsMoveOnly());
    CHECK(owning.IsMovable());
    CHECK(owning.IsDroppable());
    CHECK(owning.OwnsResources());
    CHECK_EQ(owning.copyOperation, Operation::Prohibited);
    CHECK_EQ(owning.moveOperation, Operation::Generated);

    for (const std::size_t parameter : {4U, 5U}) {
        const TypeProperties &properties = propertiesOf(parameter);
        CHECK(properties.IsMoveOnly());
        CHECK_FALSE(properties.IsMovable());
        CHECK_EQ(properties.copyOperation, Operation::Prohibited);
        CHECK_EQ(properties.moveOperation, Operation::Prohibited);
    }

    // `#NoCopy` makes plain data move-only without a destructor; `#NoMove` takes the move away too, and implies
    // `#NoCopy`, for the type and anything holding it.
    const TypeProperties &raw = propertiesOf(6);
    CHECK(raw.IsMoveOnly());
    CHECK(raw.IsMovable());
    CHECK_FALSE(raw.IsDroppable());
    CHECK_FALSE(raw.OwnsResources());
    CHECK_EQ(raw.copyOperation, Operation::Prohibited);
    CHECK_EQ(raw.moveOperation, Operation::Generated);

    for (const std::size_t parameter : {7U, 8U, 9U}) {
        CAPTURE(parameter);
        const TypeProperties &properties = propertiesOf(parameter);
        CHECK(properties.IsMoveOnly());
        CHECK_FALSE(properties.IsMovable());
        CHECK_EQ(properties.copyOperation, Operation::Prohibited);
        CHECK_EQ(properties.moveOperation, Operation::Prohibited);
    }
}

TEST_CASE("only plain data is copyable, and a destructor anywhere inside makes a type move-only") {
    Lexer lexer(R"(
        struct Owner { value: int32; }
        extend Owner {
            func ~Owner(&var self) {}
        }

        struct Point { x: int32; y: int32; }
        struct Holds { owner: Owner; tag: int32; }
        variant Shape { Dot(Point), Line(Point, Point), Empty }
        variant Carries { Held(Owner), Empty }
        union Word { signed: int32, unsigned: uint32 }

        #NoCopy
        struct Handle { fd: int32; }

        func Observe(number: int64, flag: bool, letter: char32, point: Point, points: Point[4], pair: (Point, bool),
                     shape: Shape, word: Word, view: Owner[..], pointer: *var Owner, callback: func(int32) -> int32,
                     maybe: Point?, owner: Owner, holds: Holds, owners: Owner[2], tuple: (Owner, int32),
                     carries: Carries, optional: Owner?, handle: Handle) {}
    )",
                "copyability.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "copyability.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const FuncDecl *observe = nullptr;
    for (const auto &item : parsed.module.items) {
        if (const auto *function = dynamic_cast<const FuncDecl *>(item.get());
            function && function->name == "Observe") {
            observe = function;
        }
    }
    REQUIRE(observe != nullptr);
    REQUIRE_EQ(observe->params.size(), 19);

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    const auto propertiesOf = [&](const std::size_t parameter) -> const TypeProperties & {
        const TypeProperties *properties = model.TryGetProperties(*observe->params[parameter].type);
        REQUIRE(properties != nullptr);
        return *properties;
    };

    // Numbers, bool, characters, plain structures, fixed arrays, tuples, variants and unions of them, views, raw
    // pointers, function values and optionals of plain data copy their bytes. A view or a pointer to an owning value
    // owns nothing itself.
    for (std::size_t parameter = 0; parameter < 12; ++parameter) {
        CAPTURE(parameter);
        const TypeProperties &properties = propertiesOf(parameter);
        CHECK(properties.IsCopy());
        CHECK_FALSE(properties.IsDroppable());
        CHECK_EQ(properties.copyOperation, TypeProperties::SpecialOperationState::Generated);
    }
    // A destructor, on the type or on anything it holds, makes it move-only because it owns resources.
    for (std::size_t parameter = 12; parameter < 18; ++parameter) {
        CAPTURE(parameter);
        const TypeProperties &properties = propertiesOf(parameter);
        CHECK(properties.IsMoveOnly());
        CHECK(properties.IsMovable());
        CHECK(properties.OwnsResources());
    }
    // `#NoCopy` plain data is move-only without owning anything.
    const TypeProperties &handle = propertiesOf(18);
    CHECK(handle.IsMoveOnly());
    CHECK_FALSE(handle.OwnsResources());
}

TEST_CASE("Copyable is decided by the compiler and bounds what generic code may copy") {
    Lexer lexer(R"(
        interface Copyable {}

        struct Owner { value: int32; }
        extend Owner {
            func ~Owner(&var self) {}
        }
        struct Point { x: int32; y: int32; }
        #NoCopy
        struct Handle { fd: int32; }

        func Twice<T: Copyable>(value: T) -> (T, T) {
            return (value, value);
        }
        func Forward<U: Copyable>(value: U) -> (U, U) {
            return Twice<U>(value);
        }
        func Unbound<V>(value: V) -> (V, V) {
            return Twice<V>(value);
        }

        func Uses() -> int32 {
            let numbers = Twice<int32>(4);
            let points = Twice<Point>(Point { x: 1, y: 2 });
            let forwarded = Forward<(Point, bool)>((Point { x: 3, y: 4 }, true));
            let owners = Twice<Owner>(Owner { value: 1 });
            let handles = Twice<Handle>(Handle { fd: 2 });
            return numbers.0 + points.1.y;
        }
    )",
                "copyable_bound.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "copyable_bound.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    std::vector<const Diagnostic *> errors;
    for (const Diagnostic &diagnostic : model.diagnostics) {
        if (diagnostic.IsError()) {
            errors.push_back(&diagnostic);
        }
    }
    REQUIRE_EQ(errors.size(), 3);
    CHECK_EQ(errors[0]->message, "type argument 'V' does not satisfy interface bound 'Copyable' on type parameter 'T'");
    CHECK_EQ(errors[1]->message,
             "type argument 'Owner' does not satisfy interface bound 'Copyable' on type parameter 'T'");
    REQUIRE_FALSE(errors[1]->notes.empty());
    CHECK_EQ(errors[1]->notes.front(), "'Owner' owns resources, so it moves instead of copying");
    CHECK_EQ(errors[2]->message,
             "type argument 'Handle' does not satisfy interface bound 'Copyable' on type parameter 'T'");
    REQUIRE_FALSE(errors[2]->notes.empty());
    CHECK_EQ(errors[2]->notes.front(), "'Handle' prohibits copying");
}

TEST_CASE("bodyless lifecycle operators are replaced by attributes") {
    Lexer lexer(R"(
        struct Bad {
            value: int32;
        }

        extend Bad {
            func =(&var self, other: &Bad);
            func <-(&var self, other: Bad);
            func Ordinary();
        }

        func =(left: Bad, right: Bad) {}
    )",
                "invalid_special_operations.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "invalid_special_operations.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE(model.HasErrors());

    const auto hasMessage = [&](const std::string_view message) {
        return std::ranges::any_of(model.diagnostics,
                                   [&](const Diagnostic &diagnostic) { return diagnostic.message == message; });
    };
    CHECK(hasMessage("a bodyless 'func =' does not prohibit copying"));
    CHECK(hasMessage("a bodyless 'func <-' does not prohibit moving"));
    CHECK(hasMessage("function 'Ordinary' has no body"));
    CHECK(hasMessage("special operation '=' may only be declared in an extend block"));
}

TEST_CASE("type-named destructors require the canonical owning signature") {
    Lexer lexer(R"(
        struct Bad { value: int32; }
        extend Bad {
            func ~Other(&var self) {}
            func ~Bad(&self);
        }
        func ~Bad(value: &var Bad) {}
    )",
                "invalid_destructors.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "invalid_destructors.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const auto *implementation = dynamic_cast<const ImplDecl *>(parsed.module.items[1].get());
    REQUIRE(implementation != nullptr);
    REQUIRE_EQ(implementation->methods.size(), 2);
    CHECK_EQ(implementation->methods[0]->name, "~Other");
    CHECK_EQ(implementation->methods[1]->name, "~Bad");

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE(model.HasErrors());
    const auto hasMessage = [&](const std::string_view message) {
        return std::ranges::any_of(model.diagnostics,
                                   [&](const Diagnostic &diagnostic) { return diagnostic.message == message; });
    };
    CHECK(hasMessage("destructor '~Other' must be named '~Bad' for type 'Bad'"));
    CHECK(hasMessage("destructor for type 'Bad' must have signature 'func ~Bad(&var self)'"));
    CHECK(hasMessage("destructor '~Bad' must have a body"));
    CHECK(hasMessage("destructor '~Bad' may only be declared in an extend block"));
}

TEST_CASE("drop glue expands concrete aggregates in reverse construction order") {
    Lexer lexer(R"(
        struct Leaf { value: int32; }
        extend Leaf {
            func ~Leaf(&var self) {}
        }

        struct Pair<T> {
            first: T;
            copy: int32;
            second: T;
        }

        struct Owner { leaf: Leaf; }
        extend Owner {
            func ~Owner(&var self) {}
        }

        struct Unused { leaf: Leaf; }

        variant Choice<T> {
            None,
            Both(T, int32, T)
        }

        func Observe(leaf: Leaf, pair: Pair<Leaf>, owner: Owner, values: Leaf[3], choice: Choice<Leaf>,
                     tuple: (Leaf, int32, Leaf)) {}
    )",
                "drop_glue.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "drop_glue.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const FuncDecl *observe = nullptr;
    for (const auto &item : parsed.module.items) {
        const auto *function = dynamic_cast<const FuncDecl *>(item.get());
        if (function && function->name == "Observe") {
            observe = function;
        }
    }
    REQUIRE(observe != nullptr);

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    const auto glueFor = [&](const std::size_t parameter) -> const DropGluePlan & {
        const TypeRef *type = model.TryGetType(*observe->params[parameter].type);
        REQUIRE(type != nullptr);
        const DropGluePlan *glue = model.TryGetDropGlue(*type);
        REQUIRE(glue != nullptr);
        return *glue;
    };
    const auto requireDirectDrop = [](const DropGlueStep &step, const std::string &type) {
        CHECK_EQ(step.kind, DropGlueStep::Kind::InvokeDrop);
        CHECK_EQ(step.type, TypeRef::MakeNamed(type));
    };

    const DropGluePlan &leaf = glueFor(0);
    CHECK_EQ(leaf.symbol, "__rux_drop__Leaf");
    REQUIRE_EQ(leaf.steps.size(), 1);
    requireDirectDrop(leaf.steps[0], "Leaf");

    const DropGluePlan &pair = glueFor(1);
    REQUIRE_EQ(pair.steps.size(), 2);
    CHECK_EQ(pair.steps[0].kind, DropGlueStep::Kind::Field);
    CHECK_EQ(pair.steps[0].name, "second");
    CHECK_EQ(pair.steps[0].ordinal, 2);
    REQUIRE_EQ(pair.steps[0].children.size(), 1);
    requireDirectDrop(pair.steps[0].children[0], "Leaf");
    CHECK_EQ(pair.steps[1].kind, DropGlueStep::Kind::Field);
    CHECK_EQ(pair.steps[1].name, "first");
    CHECK_EQ(pair.steps[1].ordinal, 0);

    const DropGluePlan &owner = glueFor(2);
    REQUIRE_EQ(owner.steps.size(), 2);
    requireDirectDrop(owner.steps[0], "Owner");
    CHECK_EQ(owner.steps[1].kind, DropGlueStep::Kind::Field);
    CHECK_EQ(owner.steps[1].name, "leaf");

    const DropGluePlan &array = glueFor(3);
    REQUIRE_EQ(array.steps.size(), 1);
    CHECK_EQ(array.steps[0].kind, DropGlueStep::Kind::ArrayElements);
    CHECK_EQ(array.steps[0].count, 3);
    CHECK(array.steps[0].reverse);
    REQUIRE_EQ(array.steps[0].children.size(), 1);
    requireDirectDrop(array.steps[0].children[0], "Leaf");

    const DropGluePlan &choice = glueFor(4);
    REQUIRE_EQ(choice.steps.size(), 1);
    CHECK_EQ(choice.steps[0].kind, DropGlueStep::Kind::EnumVariant);
    CHECK_EQ(choice.steps[0].name, "Both");
    REQUIRE_EQ(choice.steps[0].children.size(), 2);
    CHECK_EQ(choice.steps[0].children[0].ordinal, 2);
    CHECK_EQ(choice.steps[0].children[1].ordinal, 0);

    const DropGluePlan &tuple = glueFor(5);
    REQUIRE_EQ(tuple.steps.size(), 2);
    CHECK_EQ(tuple.steps[0].kind, DropGlueStep::Kind::TupleElement);
    CHECK_EQ(tuple.steps[0].ordinal, 2);
    CHECK_EQ(tuple.steps[1].kind, DropGlueStep::Kind::TupleElement);
    CHECK_EQ(tuple.steps[1].ordinal, 0);

    CHECK(model.TryGetDropGlue(TypeRef::MakeInt32()) == nullptr);
    CHECK(model.TryGetDropGlue(TypeRef::MakeNamed("Pair<int32>")) == nullptr);
    const DropGluePlan *unused = model.TryGetDropGlue(TypeRef::MakeNamed("Unused"));
    REQUIRE(unused != nullptr);
    REQUIRE_EQ(unused->steps.size(), 1);
    CHECK_EQ(unused->steps[0].name, "leaf");

    const HirPackage hir = AstToHirLowering(model).Generate();
    REQUIRE_FALSE(hir.dropGlues.empty());
    for (std::size_t index = 1; index < hir.dropGlues.size(); ++index) {
        CHECK(hir.dropGlues[index - 1].type.ToString() < hir.dropGlues[index].type.ToString());
    }
    const auto loweredPair = std::ranges::find(hir.dropGlues, TypeRef::MakeNamed("Pair<Leaf>"), &DropGluePlan::type);
    REQUIRE(loweredPair != hir.dropGlues.end());
    CHECK_EQ(loweredPair->steps.size(), pair.steps.size());
}

TEST_CASE("drop glue covers a generic type spelled only inside an instantiated generic body") {
    Lexer lexer(R"(
        struct Leaf { value: int32; }
        extend Leaf {
            func ~Leaf(&var self) {}
        }

        struct Pair<T> {
            first: T;
            second: T;
        }

        struct Holder<T> { pair: *var Pair<T>; }
        extend Holder<T> {
            func ~Holder(&var self) {
                // Safety: `pair` addresses the `Pair<T>` this holder owns
                unsafe { Take<T>(self.pair); }
            }
        }

        func Make<T>(first: T, second: T) {
            let pair = Pair<T> { first: move first, second: move second };
        }

        // Safety: `pair` must address a live `Pair<T>` that the caller gives up
        unsafe func Take<T>(pair: *var Pair<T>) {
            let taken = move pair[0];
        }

        struct Triple<T> { value: T; }

        func Keep<T>(value: T) {
            let triple = Triple<T> { value: move value };
        }

        func Main() -> int {
            Make<Leaf>(Leaf { value: 1 }, Leaf { value: 2 });
            let holder = Holder<int32> { pair: null };
            return 0;
        }
    )",
                "generic_drop_glue.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "generic_drop_glue.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    CAPTURE(model.diagnostics.empty() ? std::string{} : model.diagnostics.front().message);
    REQUIRE_FALSE(model.HasErrors());

    // Nothing outside `Make` spells `Pair<Leaf>`, yet its instantiation holds one by value.
    const DropGluePlan *pair = model.TryGetDropGlue(TypeRef::MakeNamed("Pair<Leaf>"));
    REQUIRE(pair != nullptr);
    CHECK_EQ(pair->steps.size(), 2);
    // A body that is never instantiated with a droppable argument asks for nothing.
    CHECK(model.TryGetDropGlue(TypeRef::MakeNamed("Triple<Leaf>")) == nullptr);
    CHECK(model.TryGetDropGlue(TypeRef::MakeNamed("Pair<int32>")) == nullptr);
}

TEST_CASE("scalar enums stay trivial while variants combine every payload ownership property") {
    Lexer lexer(R"(
        enum Status: uint8 { Ready = 1, Busy = 2 }

        struct Handle { value: int32; }
        extend Handle {
            func ~Handle(&var self) {}
        }

        struct Copier { value: int32; }
        extend Copier {
            func ~Copier(&var self) {}
        }

        variant Units { Empty, Ready, Busy }
        variant Maybe<T> { None, Some(T) }
        variant Nested { Empty, Direct(Handle), Wrapped(Maybe<Handle>) }

        func Observe(status: Status, units: Units, number: Maybe<int32>, owner: Maybe<Handle>,
                     copier: Maybe<Copier>, nested: Nested) {}
    )",
                "variant_type_properties.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "variant_type_properties.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const auto observe = std::ranges::find_if(parsed.module.items, [](const DeclPtr &item) {
        const auto *function = dynamic_cast<const FuncDecl *>(item.get());
        return function && function->name == "Observe";
    });
    REQUIRE(observe != parsed.module.items.end());
    const auto *function = dynamic_cast<const FuncDecl *>(observe->get());
    REQUIRE(function != nullptr);
    REQUIRE_EQ(function->params.size(), 6);

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    const auto properties = [&](const std::size_t index) -> const TypeProperties & {
        const TypeProperties *value = model.TryGetProperties(*function->params[index].type);
        REQUIRE(value != nullptr);
        return *value;
    };

    for (const std::size_t index : {0U, 1U, 2U}) {
        CHECK(properties(index).IsCopy());
        CHECK(properties(index).IsMovable());
        CHECK_FALSE(properties(index).IsDroppable());
        CHECK_EQ(properties(index).copyOperation, TypeProperties::SpecialOperationState::Generated);
        CHECK_EQ(properties(index).moveOperation, TypeProperties::SpecialOperationState::Generated);
    }
    const TypeProperties &owner = properties(3);
    CHECK(owner.IsMoveOnly());
    CHECK(owner.IsMovable());
    CHECK(owner.IsDroppable());
    CHECK_EQ(owner.copyOperation, TypeProperties::SpecialOperationState::Prohibited);
    CHECK_EQ(owner.moveOperation, TypeProperties::SpecialOperationState::Generated);
    // A destructor alone makes the payload, and so the variant, move-only.
    const TypeProperties &copier = properties(4);
    CHECK(copier.IsMoveOnly());
    CHECK(copier.IsMovable());
    CHECK(copier.IsDroppable());
    CHECK(copier.OwnsResources());
    CHECK_EQ(copier.copyOperation, TypeProperties::SpecialOperationState::Prohibited);
    CHECK_EQ(copier.moveOperation, TypeProperties::SpecialOperationState::Generated);
    const TypeProperties &nested = properties(5);
    CHECK(nested.IsMoveOnly());
    CHECK(nested.IsMovable());
    CHECK(nested.IsDroppable());
    CHECK_EQ(nested.copyOperation, TypeProperties::SpecialOperationState::Prohibited);
    CHECK_EQ(nested.moveOperation, TypeProperties::SpecialOperationState::Generated);
    const TypeProperties *status = model.TryGetProperties(TypeRef::MakeNamed("Status"));
    const TypeProperties *units = model.TryGetProperties(TypeRef::MakeNamed("Units"));
    REQUIRE(status != nullptr);
    REQUIRE(units != nullptr);
    CHECK(status->IsCopy());
    CHECK_EQ(status->copyOperation, TypeProperties::SpecialOperationState::Generated);
    CHECK(units->IsCopy());
}
