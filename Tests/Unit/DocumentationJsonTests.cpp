#include "Documentation/JsonExport.h"
#include "Lexer/Lexer.h"
#include "Package/Manifest.h"
#include "Syntax/Parser/Parser.h"
#include "System/Os.h"

#include <array>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace Rux;

namespace {
// Every item kind and member kind the snapshot classifies, with the values and payloads it must keep as written.
constexpr std::string_view Fixture = R"rux(// Shapes measured in whole units.
// The header runs on across lines, as documentation prose does.

import Core::uint32;

/// The tag every shape file starts with.
/// @see https://rux-lang.dev/docs/api/shapes/magic
pub const Magic: uint64 = 0x5368_6170; // not part of the value

/// A separator kept as written, semicolon and all.
pub const Separator = "a;b";

/// A width and a height.
/// Both are whole units.
/// @see https://rux-lang.dev/docs/api/shapes/size
pub struct Size {
    /// How wide.
    pub width: uint32;
    /// Bookkeeping nobody outside reads.
    cached: uint32;
}

/// Something with a size.
pub interface Measured {
    /// The size it occupies.
    /// @returns the size
    func Measure() -> Size;
}

extend Size {
    /// The widest size there is.
    pub const Widest: uint32 = 0xFFFF_FFFF;

    /// Copying a size is not allowed.
    func =(self: &var Size, other: &Size);

    /// A size `width` wide.
    /// @param width how wide
    /// @returns the size
    /// @see https://rux-lang.dev/docs/api/shapes/size#size
    pub func Size(width: uint32) -> Size {
        return Size { width: width, cached: 0 };
    }

    /// Twice the width.
    /// @returns the doubled width
    pub func Doubled(self: &Size) -> uint32 {
        return self.width * 2;
    }

    /// Whether two sizes are as wide.
    /// @param other the size to compare with
    /// @returns whether the widths match
    pub func ==(self: &Size, other: Size) -> bool {
        return self.width == other.width;
    }

    /// Releases nothing.
    func ~Size(self: &var Size) {}

    /// Kept to the package.
    func Recompute(self: &var Size) {}
}

extend Size : Measured {
    /// The size itself.
    /// @returns the size
    pub func Measure(self: &Size) -> Size {
        return Size(self.width);
    }
}

/// How a shape is drawn.
pub enum Style: uint8 {
    /// Only the outline.
    Outline = 1,
    /// Filled in.
    Filled = 0x10
}

/// Why a shape could not be read.
pub variant ShapeError {
    /// Nothing was there.
    Empty,
    /// A byte that does not belong, at this offset.
    Invalid(uint32),
    /// A shape too large to hold.
    TooLarge { width: uint32; height: uint32; }
}

/// One value of `T`, owned.
/// @typeParam T the type of the value held
pub struct Box<T: Measured> {
    /// The value.
    pub value: T;
}

extend Box<T> {
    /// A box holding `value`.
    /// @param value the value to hold
    /// @returns the box
    pub func Create(value: T) -> Box<T> {
        return Box<T> { value: value };
    }
}

/// Draws a size.
/// @param size what to draw
/// @deprecated Use `Size::Doubled`.
pub func Draw(size: Size) {}

/// Not part of the package's surface.
func Hidden() {}
)rux";

// The whole snapshot, so a change to any key, its order or its spelling is a deliberate schema change.
constexpr std::string_view Expected = R"json({
  "schema": 1,
  "target": "linux-x86_64",
  "package": {
    "name": "Shapes",
    "namespace": "Rux",
    "version": "1.2.0",
    "description": "Shapes and their sizes",
    "license": "MIT",
    "minRux": "0.4.0",
    "repository": null,
    "homepage": "https://rux-lang.dev/docs/api/shapes",
    "dependencies": [
      {
        "name": "Core",
        "namespace": "Rux",
        "version": "0.1.0",
        "targetOS": []
      },
      {
        "name": "Native",
        "namespace": null,
        "version": null,
        "targetOS": [
          "Linux"
        ]
      }
    ]
  },
  "modules": [
    {
      "name": "Shapes",
      "source": "Src/Shapes.rux",
      "header": "Shapes measured in whole units.\nThe header runs on across lines, as documentation prose does."
    }
  ],
  "items": [
    {
      "kind": "constant",
      "name": "Magic",
      "displayName": "Magic",
      "module": "Shapes",
      "source": "Src/Shapes.rux",
      "line": 8,
      "signature": "pub const Magic: uint64",
      "typeParams": [],
      "doc": {
        "summary": "The tag every shape file starts with.",
        "markdown": "The tag every shape file starts with.",
        "typeParams": [],
        "params": [],
        "returns": null,
        "see": [
          "https://rux-lang.dev/docs/api/shapes/magic"
        ],
        "deprecated": null
      },
      "value": "0x5368_6170",
      "fields": [],
      "baseType": null,
      "cases": [],
      "members": [],
      "implements": []
    },
    {
      "kind": "constant",
      "name": "Separator",
      "displayName": "Separator",
      "module": "Shapes",
      "source": "Src/Shapes.rux",
      "line": 11,
      "signature": "pub const Separator",
      "typeParams": [],
      "doc": {
        "summary": "A separator kept as written, semicolon and all.",
        "markdown": "A separator kept as written, semicolon and all.",
        "typeParams": [],
        "params": [],
        "returns": null,
        "see": [],
        "deprecated": null
      },
      "value": "\"a;b\"",
      "fields": [],
      "baseType": null,
      "cases": [],
      "members": [],
      "implements": []
    },
    {
      "kind": "struct",
      "name": "Size",
      "displayName": "Size",
      "module": "Shapes",
      "source": "Src/Shapes.rux",
      "line": 16,
      "signature": "pub struct Size",
      "typeParams": [],
      "doc": {
        "summary": "A width and a height.",
        "markdown": "A width and a height.\nBoth are whole units.",
        "typeParams": [],
        "params": [],
        "returns": null,
        "see": [
          "https://rux-lang.dev/docs/api/shapes/size"
        ],
        "deprecated": null
      },
      "value": null,
      "fields": [
        {
          "name": "width",
          "type": "uint32",
          "public": true,
          "line": 18,
          "doc": "How wide."
        },
        {
          "name": "cached",
          "type": "uint32",
          "public": false,
          "line": 20,
          "doc": "Bookkeeping nobody outside reads."
        }
      ],
      "baseType": null,
      "cases": [],
      "members": [
        {
          "kind": "constant",
          "name": "Widest",
          "line": 32,
          "signature": "pub const Widest: uint32",
          "typeParams": [],
          "params": [],
          "receiver": null,
          "returnType": null,
          "value": "0xFFFF_FFFF",
          "conformance": null,
          "public": true,
          "doc": {
            "summary": "The widest size there is.",
            "markdown": "The widest size there is.",
            "typeParams": [],
            "params": [],
            "returns": null,
            "see": [],
            "deprecated": null
          }
        },
        {
          "kind": "constructor",
          "name": "Size",
          "line": 41,
          "signature": "pub func Size(width: uint32) -> Size",
          "typeParams": [],
          "params": [
            {
              "name": "width",
              "type": "uint32"
            }
          ],
          "receiver": null,
          "returnType": "Size",
          "value": null,
          "conformance": null,
          "public": true,
          "doc": {
            "summary": "A size `width` wide.",
            "markdown": "A size `width` wide.",
            "typeParams": [],
            "params": [
              {
                "name": "width",
                "markdown": "how wide"
              }
            ],
            "returns": "the size",
            "see": [
              "https://rux-lang.dev/docs/api/shapes/size#size"
            ],
            "deprecated": null
          }
        },
        {
          "kind": "method",
          "name": "Doubled",
          "line": 47,
          "signature": "pub func Doubled(self: &Size) -> uint32",
          "typeParams": [],
          "params": [],
          "receiver": "&Size",
          "returnType": "uint32",
          "value": null,
          "conformance": null,
          "public": true,
          "doc": {
            "summary": "Twice the width.",
            "markdown": "Twice the width.",
            "typeParams": [],
            "params": [],
            "returns": "the doubled width",
            "see": [],
            "deprecated": null
          }
        },
        {
          "kind": "operator",
          "name": "==",
          "line": 54,
          "signature": "pub func ==(self: &Size, other: Size) -> bool",
          "typeParams": [],
          "params": [
            {
              "name": "other",
              "type": "Size"
            }
          ],
          "receiver": "&Size",
          "returnType": "bool",
          "value": null,
          "conformance": null,
          "public": true,
          "doc": {
            "summary": "Whether two sizes are as wide.",
            "markdown": "Whether two sizes are as wide.",
            "typeParams": [],
            "params": [
              {
                "name": "other",
                "markdown": "the size to compare with"
              }
            ],
            "returns": "whether the widths match",
            "see": [],
            "deprecated": null
          }
        },
        {
          "kind": "destructor",
          "name": "~Size",
          "line": 59,
          "signature": "func ~Size(self: &var Size)",
          "typeParams": [],
          "params": [],
          "receiver": "&var Size",
          "returnType": null,
          "value": null,
          "conformance": null,
          "public": true,
          "doc": {
            "summary": "Releases nothing.",
            "markdown": "Releases nothing.",
            "typeParams": [],
            "params": [],
            "returns": null,
            "see": [],
            "deprecated": null
          }
        },
        {
          "kind": "method",
          "name": "Measure",
          "line": 68,
          "signature": "pub func Measure(self: &Size) -> Size",
          "typeParams": [],
          "params": [],
          "receiver": "&Size",
          "returnType": "Size",
          "value": null,
          "conformance": "Measured",
          "public": true,
          "doc": {
            "summary": "The size itself.",
            "markdown": "The size itself.",
            "typeParams": [],
            "params": [],
            "returns": "the size",
            "see": [],
            "deprecated": null
          }
        }
      ],
      "implements": [
        "Measured"
      ]
    },
    {
      "kind": "interface",
      "name": "Measured",
      "displayName": "Measured",
      "module": "Shapes",
      "source": "Src/Shapes.rux",
      "line": 24,
      "signature": "pub interface Measured",
      "typeParams": [],
      "doc": {
        "summary": "Something with a size.",
        "markdown": "Something with a size.",
        "typeParams": [],
        "params": [],
        "returns": null,
        "see": [],
        "deprecated": null
      },
      "value": null,
      "fields": [],
      "baseType": null,
      "cases": [],
      "members": [
        {
          "kind": "requirement",
          "name": "Measure",
          "line": 27,
          "signature": "func Measure() -> Size",
          "typeParams": [],
          "params": [],
          "receiver": null,
          "returnType": "Size",
          "value": null,
          "conformance": null,
          "public": true,
          "doc": {
            "summary": "The size it occupies.",
            "markdown": "The size it occupies.",
            "typeParams": [],
            "params": [],
            "returns": "the size",
            "see": [],
            "deprecated": null
          }
        }
      ],
      "implements": []
    },
    {
      "kind": "enum",
      "name": "Style",
      "displayName": "Style",
      "module": "Shapes",
      "source": "Src/Shapes.rux",
      "line": 74,
      "signature": "pub enum Style",
      "typeParams": [],
      "doc": {
        "summary": "How a shape is drawn.",
        "markdown": "How a shape is drawn.",
        "typeParams": [],
        "params": [],
        "returns": null,
        "see": [],
        "deprecated": null
      },
      "value": null,
      "fields": [],
      "baseType": "uint8",
      "cases": [
        {
          "name": "Outline",
          "value": "1",
          "payload": null,
          "line": 76,
          "doc": "Only the outline."
        },
        {
          "name": "Filled",
          "value": "0x10",
          "payload": null,
          "line": 78,
          "doc": "Filled in."
        }
      ],
      "members": [],
      "implements": []
    },
    {
      "kind": "variant",
      "name": "ShapeError",
      "displayName": "ShapeError",
      "module": "Shapes",
      "source": "Src/Shapes.rux",
      "line": 82,
      "signature": "pub variant ShapeError",
      "typeParams": [],
      "doc": {
        "summary": "Why a shape could not be read.",
        "markdown": "Why a shape could not be read.",
        "typeParams": [],
        "params": [],
        "returns": null,
        "see": [],
        "deprecated": null
      },
      "value": null,
      "fields": [],
      "baseType": null,
      "cases": [
        {
          "name": "Empty",
          "value": null,
          "payload": null,
          "line": 84,
          "doc": "Nothing was there."
        },
        {
          "name": "Invalid",
          "value": null,
          "payload": "(uint32)",
          "line": 86,
          "doc": "A byte that does not belong, at this offset."
        },
        {
          "name": "TooLarge",
          "value": null,
          "payload": "{ width: uint32; height: uint32; }",
          "line": 88,
          "doc": "A shape too large to hold."
        }
      ],
      "members": [],
      "implements": []
    },
    {
      "kind": "struct",
      "name": "Box",
      "displayName": "Box<T>",
      "module": "Shapes",
      "source": "Src/Shapes.rux",
      "line": 93,
      "signature": "pub struct Box<T: Measured>",
      "typeParams": [
        {
          "name": "T",
          "bounds": [
            "Measured"
          ]
        }
      ],
      "doc": {
        "summary": "One value of `T`, owned.",
        "markdown": "One value of `T`, owned.",
        "typeParams": [
          {
            "name": "T",
            "markdown": "the type of the value held"
          }
        ],
        "params": [],
        "returns": null,
        "see": [],
        "deprecated": null
      },
      "value": null,
      "fields": [
        {
          "name": "value",
          "type": "T",
          "public": true,
          "line": 95,
          "doc": "The value."
        }
      ],
      "baseType": null,
      "cases": [],
      "members": [
        {
          "kind": "associated",
          "name": "Create",
          "line": 102,
          "signature": "pub func Create(value: T) -> Box<T>",
          "typeParams": [],
          "params": [
            {
              "name": "value",
              "type": "T"
            }
          ],
          "receiver": null,
          "returnType": "Box<T>",
          "value": null,
          "conformance": null,
          "public": true,
          "doc": {
            "summary": "A box holding `value`.",
            "markdown": "A box holding `value`.",
            "typeParams": [],
            "params": [
              {
                "name": "value",
                "markdown": "the value to hold"
              }
            ],
            "returns": "the box",
            "see": [],
            "deprecated": null
          }
        }
      ],
      "implements": []
    },
    {
      "kind": "function",
      "name": "Draw",
      "displayName": "Draw",
      "module": "Shapes",
      "source": "Src/Shapes.rux",
      "line": 110,
      "signature": "pub func Draw(size: Size)",
      "typeParams": [],
      "doc": {
        "summary": "Draws a size.",
        "markdown": "Draws a size.",
        "typeParams": [],
        "params": [
          {
            "name": "size",
            "markdown": "what to draw"
          }
        ],
        "returns": null,
        "see": [],
        "deprecated": "Use `Size::Doubled`."
      },
      "value": null,
      "fields": [],
      "baseType": null,
      "cases": [],
      "members": [],
      "implements": []
    }
  ]
}
)json";

Manifest SnapshotManifest() {
    auto parsed = Manifest::Parse("[Manifest]\nVersion = 1\nMinRux = \"0.4.0\"\n\n[Package]\nNamespace = \"Rux\"\n"
                                  "Name = \"Shapes\"\nVersion = \"1.2.0\"\nType = \"SourceLibrary\"\n"
                                  "Description = \"Shapes and their sizes\"\nLicense = \"MIT\"\n"
                                  "Homepage = \"https://rux-lang.dev/docs/api/shapes\"\n\n[Dependencies]\n"
                                  "Core = { Namespace = \"Rux\", Version = \"0.1.0\" }\n"
                                  "Native = { Path = \"../Native\", TargetOS = [\"Linux\"] }\n",
                                  "Rux.toml");
    REQUIRE(parsed.Ok());
    return std::move(*parsed.manifest);
}

std::filesystem::path SnapshotRoot(const std::string &name) {
    const auto root = System::TempDirectory() / name;
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "Src", error);
    REQUIRE_FALSE(error);
    return root;
}

/// Write the fixture where a package keeps it and parse it from there, since a snapshot reads the file again for its
/// header and its constants' values.
ParseResult ParseFixture(const std::filesystem::path &root, const std::string_view source = Fixture,
                         const std::string_view file = "Shapes.rux") {
    const auto path = root / "Src" / file;
    std::ofstream(path, std::ios::binary) << source;
    auto lexed = Lexer(std::string(source), path.string()).Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    auto parsed = Parser(std::move(lexed.tokens), path.string()).Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    return parsed;
}

std::string ReadSnapshotFile(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void RemoveSnapshotRoot(const std::filesystem::path &root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
}
} // namespace

TEST_CASE("a JSON snapshot records every public item, member, field and case as written") {
    const auto root = SnapshotRoot("rux-documentation-json-unit");
    const std::array modules{ParseFixture(root)};
    const auto snapshot = Documentation::RenderJson(
        SnapshotManifest(), modules,
        {.packageRoot = root, .outputDirectory = root / "site", .includePrivate = false, .target = "linux-x86_64"});
    CHECK(snapshot.diagnostics.empty());
    CHECK(snapshot.file.name == "Shapes.json");
    CHECK(snapshot.file.content == Expected);
    RemoveSnapshotRoot(root);
}

TEST_CASE("a JSON snapshot installs as one file per package and documents private items on request") {
    const auto root = SnapshotRoot("rux-documentation-json-install");
    const std::array modules{ParseFixture(root)};
    const auto output = root / "site";
    const auto generated = Documentation::GenerateJson(
        SnapshotManifest(), modules, {.packageRoot = root, .outputDirectory = output, .includePrivate = true});
    REQUIRE(generated.ok);
    CHECK(generated.diagnostics.empty());
    CHECK(std::filesystem::exists(output / ".rux-docs"));
    CHECK_FALSE(std::filesystem::exists(output / "index.html"));
    const auto content = ReadSnapshotFile(output / "Shapes.json");
    CHECK(content.contains("\"name\": \"Hidden\""));
    CHECK(content.contains("\"name\": \"Recompute\""));
    CHECK(content.contains("\"signature\": \"func =(self: &var Size, other: &Size)\""));
    CHECK(content.contains("\"public\": false"));

    // Generating again over the managed directory produces the same bytes.
    const auto again = Documentation::GenerateJson(
        SnapshotManifest(), modules, {.packageRoot = root, .outputDirectory = output, .includePrivate = true});
    REQUIRE(again.ok);
    CHECK(ReadSnapshotFile(output / "Shapes.json") == content);

    const auto unmanaged = root / "unmanaged";
    std::filesystem::create_directories(unmanaged);
    std::ofstream(unmanaged / "keep.txt") << "keep";
    const auto refused =
        Documentation::GenerateJson(SnapshotManifest(), modules, {.packageRoot = root, .outputDirectory = unmanaged});
    CHECK_FALSE(refused.ok);
    REQUIRE(refused.diagnostics.size() == 1);
    CHECK(refused.diagnostics.front().message.contains("unmarked"));
    CHECK(ReadSnapshotFile(unmanaged / "keep.txt") == "keep");
    RemoveSnapshotRoot(root);
}

TEST_CASE("a JSON snapshot lists a documented primitive extension as the primitive's item") {
    constexpr std::string_view Primitives = R"rux(/// A signed integer in 16 bits.
/// @see https://rux-lang.dev/docs/api/shapes/int16
extend int16 {
    /// The number of bits in the representation.
    pub const Bits: uint = 16u;
}

extend int16 {
    /// The largest representable value.
    pub const Max: int16 = 32767i16;
}

extend byte {
    /// The largest representable value.
    pub const Max: uint8 = 255u8;
}
)rux";
    const auto root = SnapshotRoot("rux-documentation-json-primitives");
    const std::array modules{ParseFixture(root, Primitives, "Limits.rux")};
    const auto snapshot = Documentation::RenderJson(
        SnapshotManifest(), modules,
        {.packageRoot = root, .outputDirectory = root / "site", .includePrivate = false, .target = "linux-x86_64"});
    CHECK(snapshot.diagnostics.empty());
    const std::string &content = snapshot.file.content;
    CHECK(content.contains(R"("kind": "primitive",
      "name": "int16",
      "displayName": "int16",
      "module": "Limits",)"));
    CHECK(content.contains(R"("signature": "extend int16")"));
    CHECK(content.contains(R"("summary": "A signed integer in 16 bits.")"));
    CHECK(content.contains(R"("https://rux-lang.dev/docs/api/shapes/int16")"));
    // Every block's members gather under the one item; an undocumented extension stands for no item.
    CHECK(content.contains(R"("signature": "pub const Bits: uint")"));
    CHECK(content.contains(R"("signature": "pub const Max: int16")"));
    CHECK_FALSE(content.contains(R"("name": "uint8")"));
    CHECK_FALSE(content.contains(R"("signature": "pub const Max: uint8")"));
    RemoveSnapshotRoot(root);
}
