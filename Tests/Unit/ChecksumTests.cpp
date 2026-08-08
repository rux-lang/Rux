#include "Package/Checksum.h"

#include <doctest.h>
#include <string>

using namespace Rux;

// Vectors from FIPS 180-4 and the NIST example set.
TEST_CASE("Sha256Hex matches the published vectors") {
    CHECK(Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(Sha256Hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrs"
                    "mnopqrstnopqrstu") == "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST_CASE("Sha256Hex handles the padding boundaries") {
    // 55 bytes leaves exactly room for the terminator and length in one block;
    // 56 and 64 force a second block.
    CHECK(Sha256Hex(std::string(55, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(Sha256Hex(std::string(56, 'a')) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(Sha256Hex(std::string(63, 'a')) == "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
    CHECK(Sha256Hex(std::string(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    CHECK(Sha256Hex(std::string(1'000'000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("Sha256Hex is defined over arbitrary bytes, not text") {
    const std::string binary("\x00\x01\xFF\x7F", 4);
    CHECK(Sha256Hex(binary).size() == sha256HexLength);
    CHECK(Sha256Hex(binary) != Sha256Hex(""));
}

TEST_CASE("DigestsEqual ignores hexadecimal case but not content") {
    CHECK(DigestsEqual("ABCDEF0123", "abcdef0123"));
    CHECK(DigestsEqual("", ""));
    CHECK_FALSE(DigestsEqual("abcdef0123", "abcdef0124"));
    CHECK_FALSE(DigestsEqual("abcdef", "abcdef00"));
}
