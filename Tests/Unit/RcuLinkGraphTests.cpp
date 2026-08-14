#include "Linker/RcuLinkGraph.h"

#include <algorithm>
#include <array>
#include <doctest.h>
#include <string_view>

using namespace Rux;

namespace {
RcuSection Text(std::vector<RcuReloc> relocations = {}) {
    RcuSection section;
    section.name = ".text";
    section.type = RcuSecType::Text;
    section.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    section.alignment = 16;
    section.data.resize(16);
    section.relocs = std::move(relocations);
    return section;
}

RcuSymbol Function(std::string name, const std::uint16_t section = RCU_TEXT_IDX,
                   const std::uint8_t visibility = RcuSymVis::Global) {
    return {std::move(name), "", 0, 4, section, RcuSymKind::Func, visibility};
}
} // namespace

TEST_CASE("RCU link graph distinguishes local, cross-object, external, and unresolved references") {
    RcuFile caller;
    caller.sections.push_back(
        Text({{0, 0, RcuRelType::Rel32, 0}, {4, 1, RcuRelType::Rel32, 0}, {8, 2, RcuRelType::Rel32, 0}}));
    caller.symbols = {Function("PrivateHelper", RCU_SEC_EXTERNAL),
                      {"puts", "libc", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global},
                      Function("Missing", RCU_SEC_EXTERNAL)};

    RcuFile definitions;
    definitions.sections.push_back(Text({{0, 0, RcuRelType::Rel32, 0}}));
    definitions.symbols = {Function("PrivateHelper", RCU_TEXT_IDX, RcuSymVis::Local)};

    const std::array objects = {caller, definitions};
    const RcuLinkGraph graph =
        RcuLinkGraph::Build(objects, "GraphTest", ArtifactKind::SharedLibrary, Target::Arch::X86_64);

    REQUIRE(graph.References().size() == 4);
    CHECK(graph.References()[0].resolution == RcuLinkResolution::CrossObjectDefinition);
    CHECK(graph.References()[0].definition == (RcuSymbolLocation{1, 0}));
    CHECK(graph.References()[1].resolution == RcuLinkResolution::External);
    CHECK(graph.References()[2].resolution == RcuLinkResolution::Unresolved);
    CHECK(graph.References()[3].resolution == RcuLinkResolution::LocalDefinition);
    REQUIRE(graph.ReferencedExternals().size() == 1);
    CHECK(graph.ReferencedExternals()[0].name == "puts");
    REQUIRE(graph.Diagnostics().size() == 1);
    CHECK(graph.Diagnostics()[0].kind == RcuLinkDiagnosticKind::UndefinedSymbol);
    CHECK(graph.Diagnostics()[0].symbol == "Missing");
}

TEST_CASE("RCU link graph keeps local data object-relative and diagnoses only duplicate strong definitions") {
    RcuFile first;
    first.sections.push_back(Text({{0, 0, RcuRelType::Abs64, 0}}));
    first.symbols = {{"local", "", 0, 8, RCU_TEXT_IDX, RcuSymKind::Data, RcuSymVis::Local},
                     Function("Weak", RCU_TEXT_IDX, RcuSymVis::Weak),
                     Function("Duplicate")};
    RcuFile second = first;

    const std::array objects = {first, second};
    const RcuLinkGraph graph =
        RcuLinkGraph::Build(objects, "GraphTest", ArtifactKind::SharedLibrary, Target::Arch::X86_64);

    REQUIRE(graph.References().size() == 2);
    CHECK(graph.References()[0].definition == (RcuSymbolLocation{0, 0}));
    CHECK(graph.References()[1].definition == (RcuSymbolLocation{1, 0}));
    REQUIRE(graph.Diagnostics().size() == 1);
    CHECK(graph.Diagnostics()[0].kind == RcuLinkDiagnosticKind::DuplicateDefinition);
    CHECK(graph.Diagnostics()[0].symbol == "Duplicate");
    const auto isLocation = [](const std::size_t objectIndex, const std::size_t symbolIndex) {
        return [=](const RcuSymbolLocation location) {
            return location.objectIndex == objectIndex && location.symbolIndex == symbolIndex;
        };
    };
    CHECK(std::ranges::any_of(graph.ExportRoots(), isLocation(0, 1)));
    CHECK(std::ranges::any_of(graph.ExportRoots(), isLocation(0, 2)));
    CHECK_FALSE(graph.FindDefinition("local").has_value());
    CHECK(graph.FindDefinition("Weak") == (RcuSymbolLocation{0, 1}));
}

TEST_CASE("RCU link graph applies executable and library root policy deterministically") {
    RcuFile object;
    object.sections.push_back(Text());
    object.symbols = {Function("Zulu"), Function("Main"), Function("Alpha")};
    const std::array objects = {object};

    const RcuLinkGraph executable =
        RcuLinkGraph::Build(objects, "GraphTest", ArtifactKind::Executable, Target::Arch::X86_64);
    CHECK(executable.EntryRoot() == (RcuSymbolLocation{0, 1}));
    CHECK(executable.ExportRoots().empty());
    CHECK_FALSE(executable.HasErrors());

    const RcuLinkGraph library =
        RcuLinkGraph::Build(objects, "GraphTest", ArtifactKind::StaticLibrary, Target::Arch::X86_64);
    CHECK_FALSE(library.EntryRoot().has_value());
    REQUIRE(library.ExportRoots().size() == 3);
    CHECK(library.ExportRoots()[0] == (RcuSymbolLocation{0, 2}));
    CHECK(library.ExportRoots()[1] == (RcuSymbolLocation{0, 1}));
    CHECK(library.ExportRoots()[2] == (RcuSymbolLocation{0, 0}));
}

TEST_CASE("RCU link graph returns architecture and missing-entry diagnostics as values") {
    RcuFile foreign;
    foreign.arch = RcuArch::AArch64;
    foreign.sourcePath = "Foreign.rux";
    const std::array objects = {foreign};
    const RcuLinkGraph mismatch =
        RcuLinkGraph::Build(objects, "GraphTest", ArtifactKind::Executable, Target::Arch::X86_64);
    REQUIRE(mismatch.Diagnostics().size() == 1);
    CHECK(mismatch.Diagnostics()[0].kind == RcuLinkDiagnosticKind::ArchitectureMismatch);
    CHECK(mismatch.Diagnostics()[0].objectName == "Foreign.rux");

    foreign.arch = RcuArch::X86_64;
    const std::array missingObjects = {foreign};
    const RcuLinkGraph missing =
        RcuLinkGraph::Build(missingObjects, "GraphTest", ArtifactKind::Executable, Target::Arch::X86_64);
    REQUIRE(missing.Diagnostics().size() == 1);
    CHECK(missing.Diagnostics()[0].kind == RcuLinkDiagnosticKind::MissingEntryPoint);
}
