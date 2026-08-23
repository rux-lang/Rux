// Reference vectors for the wide float formatting in Packages/Format.
//
// The shortest decimal that reads back as a given bit pattern is computed here the way the Rux side computes it,
// with exact big-integer arithmetic -- but written separately, in another language, and checked against the
// compiler's own exact parser: every rendering must read back as the bits it came from, and a rendering with one
// digit fewer must not. The Rux side is then held to these strings byte for byte.

#include "Numeric/FloatEncoding.h"
#include "Numeric/FloatFormat.h"
#include "Numeric/FloatParsing.h"
#include "Numeric/WideInteger.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace Rux;

namespace {
/// A natural number of any size, least significant limb first. Only what the digit generation needs.
struct Big {
    std::vector<std::uint64_t> limbs;

    static Big Small(const std::uint64_t value) {
        Big result;
        if (value != 0) {
            result.limbs.push_back(value);
        }
        return result;
    }

    static Big FromWide(const WideInteger &value) {
        Big result;
        for (std::size_t word = 0; word < value.Width() / 64; ++word) {
            result.limbs.push_back(value.Word64(word));
        }
        result.Trim();
        return result;
    }

    void Trim() {
        while (!limbs.empty() && limbs.back() == 0) {
            limbs.pop_back();
        }
    }

    [[nodiscard]] bool IsZero() const {
        return limbs.empty();
    }

    [[nodiscard]] int Compare(const Big &other) const {
        if (limbs.size() != other.limbs.size()) {
            return limbs.size() < other.limbs.size() ? -1 : 1;
        }
        for (std::size_t index = limbs.size(); index > 0; --index) {
            if (limbs[index - 1] != other.limbs[index - 1]) {
                return limbs[index - 1] < other.limbs[index - 1] ? -1 : 1;
            }
        }
        return 0;
    }

    void ShiftLeft(const std::uint64_t bits) {
        if (IsZero() || bits == 0) {
            return;
        }
        const std::size_t limbShift = bits / 64;
        const unsigned bitShift = bits % 64;
        std::vector<std::uint64_t> shifted(limbs.size() + limbShift + 1, 0);
        for (std::size_t index = 0; index < limbs.size(); ++index) {
            shifted[index + limbShift] |= limbs[index] << bitShift;
            if (bitShift != 0) {
                shifted[index + limbShift + 1] |= limbs[index] >> (64 - bitShift);
            }
        }
        limbs = std::move(shifted);
        Trim();
    }

    void MulSmall(const std::uint64_t factor) {
        unsigned __int128 carry = 0;
        for (auto &limb : limbs) {
            const unsigned __int128 product = static_cast<unsigned __int128>(limb) * factor + carry;
            limb = static_cast<std::uint64_t>(product);
            carry = product >> 64;
        }
        if (carry != 0) {
            limbs.push_back(static_cast<std::uint64_t>(carry));
        }
        Trim();
    }

    /// Schoolbook product, one shifted partial per limb of the factor.
    void Mul(const Big &factor) {
        Big product;
        for (std::size_t index = 0; index < factor.limbs.size(); ++index) {
            Big partial = *this;
            partial.MulSmall(factor.limbs[index]);
            partial.ShiftLeft(64 * index);
            product.Add(partial);
        }
        limbs = std::move(product.limbs);
        Trim();
    }

    void Add(const Big &other) {
        if (other.limbs.size() > limbs.size()) {
            limbs.resize(other.limbs.size(), 0);
        }
        unsigned __int128 carry = 0;
        for (std::size_t index = 0; index < limbs.size(); ++index) {
            const std::uint64_t right = index < other.limbs.size() ? other.limbs[index] : 0;
            const unsigned __int128 sum = static_cast<unsigned __int128>(limbs[index]) + right + carry;
            limbs[index] = static_cast<std::uint64_t>(sum);
            carry = sum >> 64;
        }
        if (carry != 0) {
            limbs.push_back(static_cast<std::uint64_t>(carry));
        }
    }

    void Sub(const Big &other) {
        std::uint64_t borrow = 0;
        for (std::size_t index = 0; index < limbs.size(); ++index) {
            const std::uint64_t right = index < other.limbs.size() ? other.limbs[index] : 0;
            const unsigned __int128 taken = static_cast<unsigned __int128>(right) + borrow;
            if (limbs[index] >= taken) {
                limbs[index] = static_cast<std::uint64_t>(limbs[index] - taken);
                borrow = 0;
            }
            else {
                limbs[index] = static_cast<std::uint64_t>(
                    (static_cast<unsigned __int128>(limbs[index]) + (static_cast<unsigned __int128>(1) << 64)) - taken);
                borrow = 1;
            }
        }
        Trim();
    }

    [[nodiscard]] unsigned TakeDigit(const Big &divisor) {
        unsigned digit = 0;
        while (digit < 9 && Compare(divisor) >= 0) {
            Sub(divisor);
            ++digit;
        }
        return digit;
    }

    static Big PowerOfTen(std::uint64_t power) {
        Big result = Small(1);
        while (power-- > 0) {
            result.MulSmall(10);
        }
        return result;
    }
};

struct Unpacked {
    FloatClass classification = FloatClass::Zero;
    bool negative = false;
    WideInteger significand = WideInteger::Zero(512);
    std::int64_t exponent = 0;
};

/// The integer significand and power of two of a finite encoding, leading bit included.
Unpacked Unpack(const FloatEncoding &encoding) {
    const FloatFormat &format = encoding.Format();
    Unpacked result;
    result.classification = encoding.Classify();
    result.negative = encoding.IsNegative();
    const std::uint32_t exponentField = encoding.ExponentField();
    WideInteger field = encoding.SignificandField().Extended(512, false);
    const std::int64_t scale = static_cast<std::int64_t>(format.precisionBits) - 1;
    if (exponentField == 0) {
        result.significand = field;
        result.exponent = 1 - format.exponentBias - scale;
    }
    else {
        if (!format.explicitIntegerBit) {
            field = field.BitwiseOr(WideInteger::FromUnsigned(1, 512).ShiftedLeft(format.precisionBits - 1));
        }
        result.significand = field;
        result.exponent = static_cast<std::int64_t>(exponentField) - format.exponentBias - scale;
    }
    return result;
}

/// Burger and Dybvig free-format digit generation: the shortest digits that read back as the same value.
std::string ShortestDigits(const Unpacked &value, const FloatFormat &format, std::int64_t &exponent10) {
    const WideInteger leading = WideInteger::FromUnsigned(1, 512).ShiftedLeft(format.precisionBits - 1);
    const std::int64_t minimumExponent =
        1 - format.exponentBias - (static_cast<std::int64_t>(format.precisionBits) - 1);
    const bool lonely = value.significand.Compare(leading, false) == 0 && value.exponent > minimumExponent;
    const bool even = !value.significand.BitSet(0);
    Big r = Big::FromWide(value.significand);
    Big s;
    Big high = Big::Small(1);
    Big low = Big::Small(1);
    if (value.exponent >= 0) {
        r.ShiftLeft(value.exponent + (lonely ? 2 : 1));
        s = Big::Small(lonely ? 4 : 2);
        high.ShiftLeft(value.exponent + (lonely ? 1 : 0));
        low.ShiftLeft(value.exponent);
    }
    else {
        r.ShiftLeft(lonely ? 2 : 1);
        s = Big::Small(1);
        s.ShiftLeft(-value.exponent + (lonely ? 2 : 1));
        if (lonely) {
            high.ShiftLeft(1);
        }
    }
    const std::int64_t magnitude =
        value.exponent + (512 - static_cast<std::int64_t>(value.significand.CountLeadingZeros())) - 1;
    std::int64_t k = (magnitude * 30103) / 100000 + 1;
    if (magnitude < 0) {
        --k;
    }
    // The scale s is only small when the binary exponent is non-negative; a value below one carries its whole
    // power of two in s, so the multiply has to be the general one.
    if (k >= 0) {
        s.Mul(Big::PowerOfTen(k));
    }
    else {
        const Big power = Big::PowerOfTen(-k);
        r.Mul(power);
        high.Mul(power);
        low.Mul(power);
    }
    for (;;) {
        Big edge = r;
        edge.Add(high);
        const bool reaches = even ? edge.Compare(s) >= 0 : edge.Compare(s) > 0;
        if (!reaches) {
            break;
        }
        s.MulSmall(10);
        ++k;
    }
    for (;;) {
        Big edge = r;
        edge.Add(high);
        edge.MulSmall(10);
        const bool shortOf = even ? edge.Compare(s) < 0 : edge.Compare(s) <= 0;
        if (!shortOf) {
            break;
        }
        r.MulSmall(10);
        high.MulSmall(10);
        low.MulSmall(10);
        --k;
    }
    std::string digits;
    for (;;) {
        if (digits.size() > 200) {
            std::fprintf(stderr, "digit generation did not terminate in %s (k=%lld)\n", format.name.data(),
                         static_cast<long long>(k));
            std::exit(2);
        }
        r.MulSmall(10);
        high.MulSmall(10);
        low.MulSmall(10);
        unsigned digit = r.TakeDigit(s);
        const bool withinLow = even ? r.Compare(low) <= 0 : r.Compare(low) < 0;
        Big edge = r;
        edge.Add(high);
        const bool withinHigh = even ? edge.Compare(s) >= 0 : edge.Compare(s) > 0;
        if (withinLow && withinHigh) {
            Big twice = r;
            twice.ShiftLeft(1);
            const int order = twice.Compare(s);
            if (order > 0 || (order == 0 && (digit & 1) == 1)) {
                ++digit;
            }
        }
        else if (withinHigh) {
            ++digit;
        }
        digits.push_back(static_cast<char>('0' + digit));
        if (withinLow || withinHigh) {
            break;
        }
    }
    exponent10 = k - 1;
    return digits;
}

/// The text Packages/Format writes for a finite value: fixed notation inside the digit capacity, scientific past it.
std::string Render(const std::string &digits, const std::int64_t exponent10, const bool negative,
                   const std::int64_t capacity) {
    std::string text = negative ? "-" : "";
    if (exponent10 < -4 || exponent10 >= capacity) {
        text += digits[0];
        text += '.';
        text += digits.size() == 1 ? "0" : digits.substr(1);
        const std::int64_t magnitude = exponent10 < 0 ? -exponent10 : exponent10;
        text += exponent10 < 0 ? "e-" : "e+";
        text += magnitude < 10 ? "0" + std::to_string(magnitude) : std::to_string(magnitude);
        return text;
    }
    if (exponent10 < 0) {
        text += "0.";
        text += std::string(static_cast<std::size_t>(-exponent10 - 1), '0');
        text += digits;
        return text;
    }
    const std::size_t integerLength = static_cast<std::size_t>(exponent10) + 1;
    for (std::size_t index = 0; index < integerLength; ++index) {
        text += index < digits.size() ? digits[index] : '0';
    }
    text += '.';
    text += digits.size() <= integerLength ? "0" : digits.substr(integerLength);
    return text;
}

struct Vector {
    WideInteger bits;
    std::string text;
};

struct Halfway {
    std::string text;
    WideInteger bits;
};

std::uint64_t NextRandom(std::uint64_t &state) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state ^ (state >> 29);
}

WideInteger Compose(const FloatFormat &format, const bool negative, const std::uint64_t exponentField,
                    const WideInteger &field) {
    WideInteger bits = field.Truncated(512);
    bits = bits.BitwiseOr(WideInteger::FromUnsigned(exponentField, 512).ShiftedLeft(format.SignificandFieldBits()));
    if (negative) {
        bits = bits.BitwiseOr(WideInteger::FromUnsigned(1, 512).ShiftedLeft(format.valueBits - 1));
    }
    return bits;
}

WideInteger RandomField(std::uint64_t &state, const std::uint32_t width) {
    WideInteger field = WideInteger::Zero(512);
    for (std::uint32_t bit = 0; bit < width; bit += 64) {
        field = field.BitwiseOr(WideInteger::FromUnsigned(NextRandom(state), 512).ShiftedLeft(bit));
    }
    const WideInteger mask =
        WideInteger::FromUnsigned(1, 512).ShiftedLeft(width).Subtracted(WideInteger::FromUnsigned(1, 512));
    return field.BitwiseAnd(mask);
}

/// The bit patterns worth holding the Rux side to for one format.
std::vector<WideInteger> Patterns(const FloatFormat &format, const bool extremes) {
    std::vector<WideInteger> out;
    auto push = [&](const FloatEncoding &encoding) { out.push_back(encoding.Bits().Extended(512, false)); };
    push(FloatEncoding::Zero(format));
    push(FloatEncoding::Zero(format, true));
    push(FloatEncoding::Infinity(format));
    push(FloatEncoding::Infinity(format, true));
    push(FloatEncoding::QuietNaN(format));
    for (const char *literal : {"1",
                                "-1",
                                "0.5",
                                "2",
                                "10",
                                "100",
                                "1234.5678",
                                "0.1",
                                "0.2",
                                "0.3",
                                "1e-5",
                                "1e-4",
                                "0.0001234",
                                "123456789012345678901234567890",
                                "1e30",
                                "-1e30",
                                "1e-30",
                                "6.02214076e23",
                                "3.14159265358979323846264338327950288419716939937510582097494",
                                "0.333333333333333333333333333333333333333333333333333333333333333333333333333",
                                "1e300",
                                "1e-300",
                                "1e1000",
                                "1e-1000",
                                "7e-4000",
                                "9.9e4000"}) {
        if (const auto parsed = ParseFloatEncoding(literal, format)) {
            push(*parsed);
        }
    }
    if (extremes) {
        push(FloatEncoding::MaxFinite(format));
        push(FloatEncoding::MaxFinite(format, true));
        push(FloatEncoding::MinPositiveNormal(format));
        push(FloatEncoding::MinPositiveSubnormal(format));
        // The largest subnormal: exponent field zero and every fraction bit set. Composed rather than taken as the
        // smallest normal minus one, because with an explicit integer bit that subtraction lands on an unnormal --
        // exponent one and integer bit clear -- which is not a value the format means.
        out.push_back(Compose(format, false, 0,
                              WideInteger::FromUnsigned(1, 512)
                                  .ShiftedLeft(format.precisionBits - 1)
                                  .Subtracted(WideInteger::FromUnsigned(1, 512))));
    }
    std::uint64_t state = 0x9E3779B97F4A7C15ULL ^ format.valueBits;
    const std::uint32_t fieldBits = format.SignificandFieldBits();
    const WideInteger integerBit = WideInteger::FromUnsigned(1, 512).ShiftedLeft(format.precisionBits - 1);
    auto randomNormal = [&](const std::int64_t spread) {
        WideInteger field = RandomField(state, fieldBits);
        if (format.explicitIntegerBit) {
            field = field.BitwiseOr(integerBit);
        }
        const std::int64_t offset = static_cast<std::int64_t>(NextRandom(state) % (2 * spread + 1)) - spread;
        const std::uint64_t exponentField = static_cast<std::uint64_t>(format.exponentBias + offset);
        out.push_back(Compose(format, (NextRandom(state) & 1) == 1, exponentField, field));
    };
    for (int count = 0; count < 16; ++count) {
        randomNormal(300);
    }
    if (extremes) {
        for (int count = 0; count < 4; ++count) {
            randomNormal(format.exponentBias - 2);
        }
        for (int count = 0; count < 4; ++count) {
            WideInteger field = RandomField(state, format.precisionBits - 1);
            if (field.IsZero()) {
                field = WideInteger::FromUnsigned(1, 512);
            }
            out.push_back(Compose(format, false, 0, field));
        }
    }
    return out;
}

/// A decimal exactly halfway between two neighbours, which must round to the even one.
std::vector<Halfway> HalfwayCases(const FloatFormat &format) {
    std::vector<Halfway> out;
    std::uint64_t state = 0xD1B54A32D192ED03ULL ^ format.valueBits;
    const std::uint32_t p = format.precisionBits;
    for (int count = 0; count < 3; ++count) {
        // A value in [1, 2) whose low bits are not all ones, so the neighbour above stays in the binade.
        WideInteger f = WideInteger::FromUnsigned(1, 512).ShiftedLeft(p - 1).BitwiseOr(RandomField(state, p - 1));
        f = f.BitwiseAnd(WideInteger::FromUnsigned(0xF, 512).ShiftedLeft(2).BitwiseNot());
        f = f.BitwiseOr(WideInteger::FromUnsigned(count == 0 ? 0 : (count == 1 ? 1 : 2), 512));
        // Midpoint = (2f + 1) / 2^p = (2f + 1) * 5^p / 10^p.
        Big mid = Big::FromWide(f);
        mid.ShiftLeft(1);
        mid.Add(Big::Small(1));
        for (std::uint32_t index = 0; index < p; ++index) {
            mid.MulSmall(5);
        }
        std::string digits;
        Big rest = mid;
        while (!rest.IsZero()) {
            unsigned __int128 remainder = 0;
            for (std::size_t index = rest.limbs.size(); index > 0; --index) {
                const unsigned __int128 current = (remainder << 64) | rest.limbs[index - 1];
                rest.limbs[index - 1] = static_cast<std::uint64_t>(current / 10);
                remainder = current % 10;
            }
            rest.Trim();
            digits.push_back(static_cast<char>('0' + remainder));
        }
        std::ranges::reverse(digits);
        while (digits.size() <= p) {
            digits.insert(digits.begin(), '0');
        }
        digits.insert(digits.end() - p, '.');
        const bool even = !f.BitSet(0);
        const WideInteger rounded = even ? f : f.Added(WideInteger::FromUnsigned(1, 512));
        const WideInteger field =
            format.explicitIntegerBit
                ? rounded
                : rounded.BitwiseAnd(WideInteger::FromUnsigned(1, 512).ShiftedLeft(p - 1).BitwiseNot());
        out.push_back({digits, Compose(format, false, static_cast<std::uint64_t>(format.exponentBias), field)});
    }
    return out;
}

std::string HexLiteral(const WideInteger &bits, const std::uint32_t width) {
    std::string hex;
    for (std::size_t word = 0; word < width / 64; ++word) {
        hex.insert(0, std::format("{:016x}", bits.Word64(word)));
    }
    const auto first = hex.find_first_not_of('0');
    hex = first == std::string::npos ? "0" : hex.substr(first);
    return "0x" + hex + "u" + std::to_string(width);
}

/// A 512-bit literal written as two halves, because one of 128 hex digits does not fit a line; the narrower
/// widths fit as they are.
std::string WrappedHexLiteral(const WideInteger &bits, const std::uint32_t width) {
    if (width < 512) {
        return HexLiteral(bits, width);
    }
    const WideInteger low = bits.BitwiseAnd(
        WideInteger::FromUnsigned(1, 512).ShiftedLeft(256).Subtracted(WideInteger::FromUnsigned(1, 512)));
    const WideInteger high = bits.ShiftedRight(256, false);
    if (high.IsZero()) {
        return HexLiteral(low, 512);
    }
    return "(" + HexLiteral(high, 512) + " << 256u) |\n               " + HexLiteral(low, 512);
}

struct Emitted {
    std::string name;
    std::uint32_t storeBits;
    std::vector<Vector> vectors;
    std::vector<Halfway> halfway;
};

void EmitFunctions(std::ofstream &out, const Emitted &set) {
    const std::string store = "uint" + std::to_string(set.storeBits);
    out << "func FormatCount" << set.name << "() -> uint {\n    return " << set.vectors.size() << ";\n}\n\n";
    out << "func FormatBits" << set.name << "(index: uint) -> " << store << " {\n";
    for (std::size_t index = 0; index < set.vectors.size(); ++index) {
        out << "    if index == " << index << "u {\n        return "
            << WrappedHexLiteral(set.vectors[index].bits, set.storeBits) << ";\n    }\n";
    }
    out << "    return 0u" << set.storeBits << ";\n}\n\n";
    out << "func FormatText" << set.name << "(index: uint) -> Slice<char8> {\n";
    for (std::size_t index = 0; index < set.vectors.size(); ++index) {
        out << "    if index == " << index << "u {\n        return \"" << set.vectors[index].text << "\";\n    }\n";
    }
    out << "    return \"\";\n}\n\n";
    out << "func HalfwayCount" << set.name << "() -> uint {\n    return " << set.halfway.size() << ";\n}\n\n";
    out << "func HalfwayText" << set.name << "(index: uint) -> Slice<char8> {\n";
    for (std::size_t index = 0; index < set.halfway.size(); ++index) {
        out << "    if index == " << index << "u {\n        return \"" << set.halfway[index].text << "\";\n    }\n";
    }
    out << "    return \"\";\n}\n\n";
    out << "func HalfwayBits" << set.name << "(index: uint) -> " << store << " {\n";
    for (std::size_t index = 0; index < set.halfway.size(); ++index) {
        out << "    if index == " << index << "u {\n        return "
            << WrappedHexLiteral(set.halfway[index].bits, set.storeBits) << ";\n    }\n";
    }
    out << "    return 0u" << set.storeBits << ";\n}\n\n";
}

/// Fails loudly when a rendering does not read back as its bits, or when one digit fewer would have.
void Verify(const FloatFormat &format, const WideInteger &bits, const std::string &digits, const std::int64_t e10,
            const std::string &text) {
    const auto parsed = ParseFloatEncoding(text, format);
    if (!parsed || parsed->Bits().Extended(512, false).Compare(bits, false) != 0) {
        std::fprintf(stderr, "rendering '%s' does not read back in %s\n", text.c_str(), format.name.data());
        std::fprintf(stderr, "  wanted %s\n", HexLiteral(bits, 512).c_str());
        if (parsed) {
            std::fprintf(stderr, "  parsed %s\n", HexLiteral(parsed->Bits().Extended(512, false), 512).c_str());
        }
        std::exit(1);
    }
    if (digits.size() <= 1) {
        return;
    }
    for (const bool up : {false, true}) {
        std::string shorter = digits.substr(0, digits.size() - 1);
        std::int64_t exponent = e10;
        if (up) {
            std::size_t index = shorter.size();
            while (index > 0 && shorter[index - 1] == '9') {
                shorter[--index] = '0';
            }
            if (index == 0) {
                shorter.insert(shorter.begin(), '1');
                ++exponent;
            }
            else {
                ++shorter[index - 1];
            }
        }
        const std::string candidate = shorter.substr(0, 1) + "." + (shorter.size() > 1 ? shorter.substr(1) : "0") +
                                      "e" + std::to_string(exponent);
        const auto again = ParseFloatEncoding(candidate, format);
        if (again && again->Bits().Extended(512, false).Compare(bits, false) == 0) {
            std::fprintf(stderr, "rendering '%s' is not shortest: '%s' reads back too\n", text.c_str(),
                         candidate.c_str());
            std::exit(1);
        }
    }
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto started = std::chrono::steady_clock::now();
    auto Elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(); };
    std::ofstream out("Tests/Packages/Format/WideFloat/Src/Vectors.rux", std::ios::binary | std::ios::trunc);
    out << "// Generated by Tools/FloatVectors from the compiler's exact float arithmetic. Do not edit by hand.\n"
        << "//\n// Every text here is the shortest decimal that reads back as its bits, checked against the\n"
        << "// compiler's exact parser when it was written; every halfway text is exactly between two neighbouring\n"
        << "// values and must round to the even one.\n\nimport Core::Slice;\n\n";

    const struct {
        std::uint32_t valueBits;
        std::uint32_t storeBits;
        const char *name;
        bool extremes;
    } sets[] = {{80, 128, "80", true}, {128, 128, "128", true}, {256, 256, "256", true}, {512, 512, "512", false}};

    for (const auto &set : sets) {
        const FloatFormat &format = *FindFloatFormat(set.valueBits);
        Emitted emitted{set.name, set.storeBits, {}, HalfwayCases(format)};
        const std::int64_t capacity = (static_cast<std::int64_t>(format.precisionBits) * 30103) / 100000 + 2;
        std::size_t progress = 0;
        for (const WideInteger &bits : Patterns(format, set.extremes)) {
            std::printf("  %s pattern %zu at %.1fs\n", format.name.data(), progress++, Elapsed());
            const FloatEncoding encoding = FloatEncoding::FromBits(format, bits.Truncated(format.valueBits));
            const Unpacked value = Unpack(encoding);
            std::string text;
            if (value.classification == FloatClass::QuietNaN || value.classification == FloatClass::SignalingNaN) {
                text = "NaN";
            }
            else if (value.classification == FloatClass::Infinity) {
                text = value.negative ? "-Inf" : "Inf";
            }
            else if (value.classification == FloatClass::Zero) {
                text = value.negative ? "-0.0" : "0.0";
            }
            else {
                std::int64_t exponent10 = 0;
                const std::string digits = ShortestDigits(value, format, exponent10);
                text = Render(digits, exponent10, value.negative, capacity);
                Verify(format, bits, digits, exponent10, text);
            }
            emitted.vectors.push_back({bits, text});
        }
        for (const Halfway &half : emitted.halfway) {
            const auto parsed = ParseFloatEncoding(half.text, format);
            if (!parsed || parsed->Bits().Extended(512, false).Compare(half.bits, false) != 0) {
                std::fprintf(stderr, "halfway '%s' does not round to even in %s\n", half.text.c_str(),
                             format.name.data());
                return 1;
            }
        }
        EmitFunctions(out, emitted);
        std::printf("%s: %zu format vectors, %zu halfway cases\n", format.name.data(), emitted.vectors.size(),
                    emitted.halfway.size());
    }
    return 0;
}
