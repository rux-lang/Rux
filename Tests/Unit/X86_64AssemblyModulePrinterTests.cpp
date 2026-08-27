#include "CodeGen/X86_64/AssemblyModulePrinter.h"
#include "Ir/Lir/Lir.h"

#include <doctest.h>
#include <string>
#include <utility>

using namespace Rux;

TEST_CASE("x86-64 assembly module printing owns deterministic sections") {
    AssemblyModulePrinter printer(Target::OS::Linux);

    LirModule module;
    LirExternVar externalVariable;
    externalVariable.name = "ExternalValue";
    externalVariable.type = TypeRef::MakeInt64();
    module.externVars.push_back(std::move(externalVariable));
    LirConstDecl constant;
    constant.name = "PublishedValue";
    constant.isPublic = true;
    constant.type = TypeRef::MakeInt64();
    constant.value = "42";
    module.consts.push_back(std::move(constant));
    module.vtables.push_back({.label = "Widget_vtable", .methods = {"Widget_Draw", "Widget_Drop"}});
    printer.EmitModuleData(module);
    printer.DeclareExtern("ExternalValue");
    LirFunc function;
    function.name = "Main";
    function.isPublic = true;
    CHECK(printer.DeclareFunction(function));
    LirFunc externalFunction;
    externalFunction.name = "ExternalFunction";
    externalFunction.isExtern = true;
    CHECK_FALSE(printer.DeclareFunction(externalFunction));

    CHECK(printer.InternString("shared") == "__str0");
    CHECK(printer.InternString("shared") == "__str0");
    CHECK(printer.InternFloat32("1.25") == "__f32_1");
    CHECK(printer.InternFloat64("2.5") == "__f64_2");
    printer.TextLabel("Main");
    printer.TextInstruction("ret");
    const std::string output = printer.Finalize();
    const std::size_t rodata = output.find("section .rodata");
    const std::size_t data = output.find("section .data");
    const std::size_t text = output.find("section .text");
    REQUIRE(rodata != std::string::npos);
    REQUIRE(data != std::string::npos);
    REQUIRE(text != std::string::npos);
    CHECK(rodata < data);
    CHECK(data < text);
    CHECK(output.find("extern ExternalValue\nextern ExternalValue") == std::string::npos);
    CHECK(output.find("extern ExternalFunction\n") != std::string::npos);
    CHECK(output.find("global Main\n") != std::string::npos);
    CHECK(output.find("global PublishedValue\nPublishedValue:  ; int64 = 42") != std::string::npos);
    CHECK(output.find("Widget_vtable:\n    dq Widget_Draw\n    dq Widget_Drop") != std::string::npos);
    CHECK(output.find("__str0:\n    db    115, 104, 97, 114, 101, 100, 0") != std::string::npos);
}
