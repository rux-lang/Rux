#include "Linter/Linter.h"

#include <doctest.h>

using namespace Rux;

TEST_CASE("linter accepts syntactically valid source") {
    auto result = Rux::Linting::Lint("func Main() -> int { return 0; }", "valid.rux");
    CHECK_FALSE(result.HasErrors());
}

TEST_CASE("linter reports syntax errors") {
    auto result = Rux::Linting::Lint("func Main() -> int { let value = 1 return value; }", "invalid.rux");
    CHECK(result.HasErrors());
}

TEST_CASE("linter accepts correct naming conventions") {
    const std::string source = R"(
        module ValidModule {
            const MyConstant = 42;

            struct MyStruct {
                someField: int32;
            }

            enum MyEnum {
                FirstVariant,
                SecondVariant{innerValue: int32;}
            }

            interface MyInterface {
                func SomeMethod(someParam: int32);
            }

            func SomeFunction(firstParam: int32, secondParam: float64) -> int32 {
                let localVariable = 10;
                return localVariable;
            }
        }
    )";

    auto result = Rux::Linting::Lint(source, "naming_valid.rux");
    CHECK_FALSE(result.HasErrors());
    CHECK(result.diagnostics.empty());
}

TEST_CASE("linter warns on bad function name") {
    auto result = Rux::Linting::Lint("func bad_name() {}", "func_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "function name 'bad_name' should be PascalCase");
}

TEST_CASE("linter accepts symbolic operator function names") {
    auto result = Rux::Linting::Lint(R"(
        extend Number {
            func +(self, other: Number) -> Number;
            func ==(self, other: Number) -> bool;
        }
    )",
                                     "operators.rux");
    CHECK_FALSE(result.HasErrors());
    CHECK(result.diagnostics.empty());
}

TEST_CASE("linter warns on bad function parameter name") {
    auto result = Rux::Linting::Lint("func Test(BadParam: int) {}", "param_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "parameter name 'BadParam' should be camelCase");
}

TEST_CASE("linter warns on bad struct name") {
    auto result = Rux::Linting::Lint("struct badStruct {}", "struct_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "struct name 'badStruct' should be PascalCase");
}

TEST_CASE("linter warns on bad struct field name") {
    auto result = Rux::Linting::Lint("struct Test { BadField: int; }", "field_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "struct field name 'BadField' should be camelCase");
}

TEST_CASE("linter warns on bad enum name") {
    auto result = Rux::Linting::Lint("enum badEnum { Variant }", "enum_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "enum name 'badEnum' should be PascalCase");
}

TEST_CASE("linter warns on bad enum variant name") {
    auto result = Rux::Linting::Lint("enum Test { badVariant }", "variant_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "enum variant name 'badVariant' should be PascalCase");
}

TEST_CASE("linter warns on bad enum variant field name") {
    auto result = Rux::Linting::Lint("enum Test { Variant{BadField: int;} }", "var_field_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "enum variant field name 'BadField' should be camelCase");
}

TEST_CASE("linter warns on bad union name") {
    auto result = Rux::Linting::Lint("union badUnion { x: int }", "union_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "union name 'badUnion' should be PascalCase");
}

TEST_CASE("linter warns on bad union field name") {
    auto result = Rux::Linting::Lint("union Test { BadField: int }", "union_field_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "union field name 'BadField' should be camelCase");
}

TEST_CASE("linter warns on bad interface name") {
    auto result = Rux::Linting::Lint("interface badInterface {}", "interface_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "interface name 'badInterface' should be PascalCase");
}

TEST_CASE("linter warns on bad module name") {
    auto result = Rux::Linting::Lint("module bad_module {}", "module_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "module name 'bad_module' should be PascalCase");
}

TEST_CASE("linter warns on bad constant name") {
    auto result = Rux::Linting::Lint("const badConst = 42;", "const_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "constant name 'badConst' should be PascalCase");
}

TEST_CASE("linter warns on bad type alias name") {
    auto result = Rux::Linting::Lint("type badAlias = int;", "alias_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "type alias name 'badAlias' should be PascalCase");
}

TEST_CASE("naming.type allows foreign type and member names") {
    auto result = Rux::Linting::Lint(R"(
        #Allow("naming.type")
        type time_t = int64;

        #Allow("naming.type")
        struct timespec {
            tv_sec: time_t;
            tv_nsec: int64;
        }
    )",
                                     "foreign_types.rux");
    CHECK_FALSE(result.HasErrors());
    CHECK(result.diagnostics.empty());
}

TEST_CASE("naming.type rejects unknown rules and non-type declarations") {
    auto unknown = Rux::Linting::Lint(R"(
        #Allow("naming.unknown")
        type time_t = int64;
    )",
                                      "unknown_allow.rux");
    CHECK(unknown.HasErrors());

    auto nonType = Rux::Linting::Lint(R"(
        #Allow("naming.type")
        func bad_name() {}
    )",
                                      "invalid_allow_target.rux");
    CHECK(nonType.HasErrors());
}

TEST_CASE("linter warns on bad local variable name") {
    auto result = Rux::Linting::Lint("func Test() { let BadVar = 10; }", "local_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "variable name 'BadVar' should be camelCase");
}

TEST_CASE("linter warns on bad loop variable name") {
    auto result = Rux::Linting::Lint("func Test() { for BadVar in 0..10 {} }", "loop_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "loop variable name 'BadVar' should be camelCase");
}

TEST_CASE("linter warns on bad pattern binding name") {
    auto result = Rux::Linting::Lint("func Test() { let (x, BadVar) = (1, 2); }", "pattern_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "variable binding 'BadVar' should be camelCase");
}
