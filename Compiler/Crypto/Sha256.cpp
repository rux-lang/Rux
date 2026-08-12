#include "Crypto/Sha256.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace Rux::Crypto {
namespace {
// FIPS 180-4 section 4.2.2: the first 32 bits of the fractional parts of the
// cube roots of the first 64 primes.
constexpr std::array<std::uint32_t, 64> roundConstants = {
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5, 0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
    0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3, 0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
    0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC, 0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7, 0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
    0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13, 0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
    0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3, 0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5, 0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
    0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208, 0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2,
};

void CompressBlock(const std::uint8_t *block, std::array<std::uint32_t, 8> &state) {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t i = 0; i < 16; ++i) {
        schedule[i] = static_cast<std::uint32_t>(block[i * 4]) << 24U |
                      static_cast<std::uint32_t>(block[i * 4 + 1]) << 16U |
                      static_cast<std::uint32_t>(block[i * 4 + 2]) << 8U | static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            std::rotr(schedule[i - 15], 7) ^ std::rotr(schedule[i - 15], 18) ^ schedule[i - 15] >> 3U;
        const std::uint32_t s1 =
            std::rotr(schedule[i - 2], 17) ^ std::rotr(schedule[i - 2], 19) ^ schedule[i - 2] >> 10U;
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }

    auto [a, b, c, d, e, f, g, h] = state;
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t sigma1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + sigma1 + choose + roundConstants[i] + schedule[i];
        const std::uint32_t sigma0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = sigma0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    const std::array<std::uint32_t, 8> working = {a, b, c, d, e, f, g, h};
    for (std::size_t i = 0; i < state.size(); ++i) {
        state[i] += working[i];
    }
}
} // namespace

Sha256Digest Sha256(const std::span<const std::uint8_t> data) {
    std::array<std::uint32_t, 8> state = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
                                          0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};

    const std::size_t wholeBlocks = data.size() / 64;
    for (std::size_t i = 0; i < wholeBlocks; ++i) {
        CompressBlock(data.data() + i * 64, state);
    }

    std::array<std::uint8_t, 128> tail{};
    const std::size_t remainder = data.size() - wholeBlocks * 64;
    for (std::size_t i = 0; i < remainder; ++i) {
        tail[i] = data[wholeBlocks * 64 + i];
    }
    tail[remainder] = 0x80;
    const std::size_t tailBlocks = remainder < 56 ? 1 : 2;
    const std::size_t tailSize = tailBlocks * 64;
    const std::uint64_t bitCount = static_cast<std::uint64_t>(data.size()) * 8;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[tailSize - 1 - i] = static_cast<std::uint8_t>(bitCount >> (8 * i));
    }
    for (std::size_t i = 0; i < tailBlocks; ++i) {
        CompressBlock(tail.data() + i * 64, state);
    }

    Sha256Digest digest{};
    for (std::size_t word = 0; word < state.size(); ++word) {
        for (std::size_t byte = 0; byte < 4; ++byte) {
            digest[word * 4 + byte] = static_cast<std::uint8_t>(state[word] >> (24U - byte * 8U));
        }
    }
    return digest;
}
} // namespace Rux::Crypto
