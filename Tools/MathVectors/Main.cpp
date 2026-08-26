// Reference vectors for the transcendental functions of Packages/Math.
//
// The problem this solves is that a test cannot check a function against itself. Somebody has to compute the
// true answer to more precision than the type being tested holds, and round it once -- which is what MPFR is
// normally reached for. Rather than take an external dependency for an offline tool, this computes in fixed
// point on the compiler's own arbitrary-width integers: 256 bits, of which 192 are fractional. That is about 58
// decimal digits, which leaves more than 130 bits below a double's 53 -- far past any double-rounding question,
// and small enough that the series below run in seconds rather than hours.
//
// Run by hand. The full sweep goes to Temp/ for inspection; the curated boundary set is written into the test
// package and committed, so CI builds this tool and never runs it.

#include "Numeric/WideInteger.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

namespace {
using Rux::WideInteger;

/// Bits of the fixed-point representation, and how many of them lie below the point.
constexpr std::uint32_t kWidth = 256;
constexpr std::uint32_t kFraction = 192;

/// A fixed-point real: a signed 512-bit integer scaled by 2^-448.
struct Fixed {
    WideInteger value = WideInteger::Zero(kWidth);

    static Fixed Zero() {
        return Fixed{};
    }

    static Fixed FromInt(const std::int64_t n) {
        const bool negative = n < 0;
        const auto magnitude = static_cast<std::uint64_t>(negative ? -(n + 1) + 1 : n);
        WideInteger scaled = WideInteger::FromUnsigned(magnitude, kWidth).ShiftedLeft(kFraction);
        return Fixed{negative ? scaled.Negated() : scaled};
    }

    [[nodiscard]] bool IsNegative() const {
        return value.Compare(WideInteger::Zero(kWidth), true) == std::strong_ordering::less;
    }

    [[nodiscard]] Fixed Negated() const {
        return Fixed{value.Negated()};
    }

    [[nodiscard]] Fixed Added(const Fixed &other) const {
        return Fixed{value.Added(other.value)};
    }

    [[nodiscard]] Fixed Subtracted(const Fixed &other) const {
        return Fixed{value.Subtracted(other.value)};
    }

    /// Multiplication is a full-width product shifted back down by the fractional bits. Doing it on magnitudes
    /// keeps the shift arithmetic-free: a right shift of a negative two's-complement value rounds toward
    /// negative infinity, which would bias every product involving a negative operand.
    [[nodiscard]] Fixed Multiplied(const Fixed &other) const {
        const bool negative = IsNegative() != other.IsNegative();
        const WideInteger left = IsNegative() ? value.Negated() : value;
        const WideInteger right = other.IsNegative() ? other.value.Negated() : other.value;
        const auto product = left.MultipliedFull(right);
        // The product is 512 bits wide as a low/high pair; shifting it down by the fractional bits means
        // taking the low half's top bits and the high half's bottom bits together.
        WideInteger low = product.low.ShiftedRight(kFraction, false);
        const WideInteger high = product.high.ShiftedLeft(kWidth - kFraction);
        WideInteger combined = low.BitwiseOr(high);
        return Fixed{negative ? combined.Negated() : combined};
    }

    /// Division by shifting the numerator up before dividing, which keeps the fractional bits.
    [[nodiscard]] Fixed Divided(const Fixed &other) const {
        const bool negative = IsNegative() != other.IsNegative();
        const WideInteger left = IsNegative() ? value.Negated() : value;
        const WideInteger right = other.IsNegative() ? other.value.Negated() : other.value;
        // Widen so the shift cannot lose the top of the numerator.
        const WideInteger wideLeft = left.Extended(512, false).ShiftedLeft(kFraction);
        const WideInteger wideRight = right.Extended(512, false);
        const auto division = wideRight.Compare(WideInteger::Zero(512), false) == std::strong_ordering::equal
                                ? Rux::WideIntegerDivision{}
                                : wideLeft.Divided(wideRight, false);
        WideInteger quotient = division.quotient.Truncated(kWidth);
        return Fixed{negative ? quotient.Negated() : quotient};
    }
};

/// The fixed-point value nearest `x`, which is exact for every double since 448 fractional bits far exceed a
/// double's 52 plus its exponent range in this direction.
Fixed FromDouble(const double x) {
    const bool negative = x < 0.0;
    const double magnitude = negative ? -x : x;
    const auto bits = std::bit_cast<std::uint64_t>(magnitude);
    const auto rawExponent = static_cast<std::int32_t>((bits >> 52) & 0x7FF);
    std::uint64_t significand = bits & 0xF'FFFF'FFFF'FFFFULL;
    std::int32_t exponent = 0;
    if (rawExponent == 0) {
        exponent = -1074;
    }
    else {
        significand |= 0x10'0000'0000'0000ULL;
        exponent = rawExponent - 1075;
    }
    WideInteger held = WideInteger::FromUnsigned(significand, kWidth);
    const std::int32_t shift = exponent + static_cast<std::int32_t>(kFraction);
    held = shift >= 0 ? held.ShiftedLeft(static_cast<std::uint32_t>(shift))
                      : held.ShiftedRight(static_cast<std::uint32_t>(-shift), false);
    return Fixed{negative ? held.Negated() : held};
}

/// The double nearest a fixed-point value, rounded once to nearest with ties to even -- which is what makes this
/// a reference rather than another approximation.
double ToDouble(const Fixed &x) {
    const bool negative = x.IsNegative();
    WideInteger magnitude = negative ? x.value.Negated() : x.value;
    if (magnitude.Compare(WideInteger::Zero(kWidth), false) == std::strong_ordering::equal) {
        return negative ? -0.0 : 0.0;
    }
    // The position of the highest set bit gives the exponent directly.
    std::int32_t highest = -1;
    for (std::int32_t bit = kWidth - 1; bit >= 0; --bit) {
        if (magnitude.ShiftedRight(static_cast<std::uint32_t>(bit), false).Word64(0) & 1ULL) {
            highest = bit;
            break;
        }
    }
    std::int32_t exponent = highest - static_cast<std::int32_t>(kFraction);
    // Take the 53 bits of the significand, with the bit below them to round on and everything under that as a
    // sticky flag -- which together are exactly what round-to-nearest-ties-to-even needs.
    const std::int32_t dropped = highest - 52;
    std::uint64_t significand = 0;
    bool roundBit = false;
    bool sticky = false;
    if (dropped <= 0) {
        significand = magnitude.ShiftedLeft(static_cast<std::uint32_t>(-dropped)).Word64(0);
    }
    else {
        significand = magnitude.ShiftedRight(static_cast<std::uint32_t>(dropped), false).Word64(0);
        roundBit = (magnitude.ShiftedRight(static_cast<std::uint32_t>(dropped - 1), false).Word64(0) & 1ULL) != 0;
        const WideInteger low = magnitude.BitwiseAnd(
            WideInteger::AllOnes(kWidth).ShiftedRight(kWidth - static_cast<std::uint32_t>(dropped - 1), false));
        sticky = low.Compare(WideInteger::Zero(kWidth), false) != std::strong_ordering::equal;
    }
    if (roundBit && (sticky || (significand & 1ULL) != 0)) {
        ++significand;
        if ((significand >> 53) != 0) {
            // The increment carried out of the significand, which moves the point one place.
            significand >>= 1;
            ++exponent;
        }
    }
    double result = static_cast<double>(significand);
    result = std::ldexp(result, exponent - 52);
    return negative ? -result : result;
}

/// exp(x) by the Taylor series, after halving the argument until it is small enough for the series to converge
/// quickly, then squaring back. Each halving costs one squaring and buys a much faster series.
Fixed Exp(const Fixed &x) {
    int halvings = 0;
    Fixed reduced = x;
    const Fixed quarter = Fixed::FromInt(1).Divided(Fixed::FromInt(4));
    while (reduced.value.Compare(quarter.value, true) == std::strong_ordering::greater ||
           reduced.Negated().value.Compare(quarter.value, true) == std::strong_ordering::greater) {
        reduced = reduced.Divided(Fixed::FromInt(2));
        ++halvings;
    }
    Fixed term = Fixed::FromInt(1);
    Fixed sum = Fixed::FromInt(1);
    for (int n = 1; n <= 60; ++n) {
        term = term.Multiplied(reduced).Divided(Fixed::FromInt(n));
        sum = sum.Added(term);
    }
    for (int i = 0; i < halvings; ++i) {
        sum = sum.Multiplied(sum);
    }
    return sum;
}

/// log(x) by Newton's method on exp, which converges quadratically from a double-precision seed.
Fixed Log(const Fixed &x) {
    Fixed guess = FromDouble(std::log(ToDouble(x)));
    for (int i = 0; i < 8; ++i) {
        // guess -= 1 - x / exp(guess)
        const Fixed ratio = x.Divided(Exp(guess));
        guess = guess.Added(ratio).Subtracted(Fixed::FromInt(1));
    }
    return guess;
}

/// sin(x) and cos(x) by the Taylor series on an argument already reduced into a small interval by the caller.
void SinCosSeries(const Fixed &x, Fixed &sine, Fixed &cosine) {
    Fixed term = x;
    Fixed sum = x;
    Fixed squared = x.Multiplied(x);
    for (int n = 1; n <= 45; ++n) {
        const auto a = static_cast<std::int64_t>(n) * 2;
        const auto b = static_cast<std::int64_t>(n) * 2 + 1;
        term = term.Multiplied(squared).Divided(Fixed::FromInt(a)).Divided(Fixed::FromInt(b)).Negated();
        sum = sum.Added(term);
    }
    sine = sum;

    term = Fixed::FromInt(1);
    Fixed cosineSum = Fixed::FromInt(1);
    for (int n = 1; n <= 45; ++n) {
        const auto a = static_cast<std::int64_t>(n) * 2 - 1;
        const auto b = static_cast<std::int64_t>(n) * 2;
        term = term.Multiplied(squared).Divided(Fixed::FromInt(a)).Divided(Fixed::FromInt(b)).Negated();
        cosineSum = cosineSum.Added(term);
    }
    cosine = cosineSum;
}

/// Pi, from the Machin-like formula 4*atan(1/5) - atan(1/239) scaled by four, using the arctangent series which
/// converges quickly for these small arguments.
Fixed Pi() {
    const auto arcTangentOfReciprocal = [](const std::int64_t denominator) {
        const Fixed x = Fixed::FromInt(1).Divided(Fixed::FromInt(denominator));
        const Fixed squared = x.Multiplied(x);
        Fixed term = x;
        Fixed sum = x;
        for (int n = 1; n <= 90; ++n) {
            term = term.Multiplied(squared).Negated();
            sum = sum.Added(term.Divided(Fixed::FromInt(2 * n + 1)));
        }
        return sum;
    };
    const Fixed first = arcTangentOfReciprocal(5).Multiplied(Fixed::FromInt(4));
    const Fixed second = arcTangentOfReciprocal(239);
    return first.Subtracted(second).Multiplied(Fixed::FromInt(4));
}

/// sin and cos of any argument, reducing modulo a high-precision Pi/2 rather than a rounded one -- which is the
/// whole point of computing the reference here rather than in double.
void SinCosOf(const Fixed &x, Fixed &sine, Fixed &cosine) {
    const Fixed pi = Pi();
    const Fixed halfPi = pi.Divided(Fixed::FromInt(2));
    // Reduce to a quadrant by repeated subtraction, which is fine for the modest arguments the vectors use.
    Fixed reduced = x;
    std::int64_t quadrant = 0;
    const bool negative = reduced.IsNegative();
    if (negative) {
        reduced = reduced.Negated();
    }
    while (reduced.value.Compare(halfPi.value, true) == std::strong_ordering::greater) {
        reduced = reduced.Subtracted(halfPi);
        quadrant = (quadrant + 1) % 4;
    }
    Fixed s = Fixed::Zero();
    Fixed c = Fixed::Zero();
    SinCosSeries(reduced, s, c);
    Fixed outSine = s;
    Fixed outCosine = c;
    if (quadrant == 1) {
        outSine = c;
        outCosine = s.Negated();
    }
    else if (quadrant == 2) {
        outSine = s.Negated();
        outCosine = c.Negated();
    }
    else if (quadrant == 3) {
        outSine = c.Negated();
        outCosine = s;
    }
    sine = negative ? outSine.Negated() : outSine;
    cosine = outCosine;
}

/// One row of the table: the argument and the correctly-rounded true value of each function at it.
struct Row {
    double argument;
    double exp;
    double log;
    double sine;
    double cosine;
};

std::string DoubleLiteral(const double value) {
    // Seventeen significant digits round-trip every double exactly.
    std::string text = std::format("{:.17g}", value);
    if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
        text.find("inf") == std::string::npos && text.find("nan") == std::string::npos) {
        text += ".0";
    }
    return text;
}

int Run() {
    // The arguments: a spread across the ordinary range, plus the awkward neighbourhoods -- near zero, near one,
    // and near the multiples of a quarter turn where the trigonometric functions cancel.
    std::vector<double> arguments;
    for (int i = 1; i <= 40; ++i) {
        arguments.push_back(static_cast<double>(i) * 0.25);
    }
    for (int i = 1; i <= 12; ++i) {
        arguments.push_back(std::ldexp(1.0, -i));
        arguments.push_back(1.0 + std::ldexp(1.0, -i));
    }
    const double piApproximation = 3.14159265358979311600;
    for (int i = 1; i <= 8; ++i) {
        arguments.push_back(piApproximation * 0.5 * static_cast<double>(i));
    }

    std::vector<Row> rows;
    rows.reserve(arguments.size());
    for (const double argument : arguments) {
        const Fixed x = FromDouble(argument);
        Fixed sine = Fixed::Zero();
        Fixed cosine = Fixed::Zero();
        SinCosOf(x, sine, cosine);
        rows.push_back(Row{argument, ToDouble(Exp(x)), ToDouble(Log(x)), ToDouble(sine), ToDouble(cosine)});
    }

    std::filesystem::create_directories("Temp");
    {
        std::ofstream sweep("Temp/MathVectors.txt");
        sweep << "argument exp log sin cos\n";
        for (const Row &row : rows) {
            sweep << std::format("{} {} {} {} {}\n", DoubleLiteral(row.argument), DoubleLiteral(row.exp),
                                 DoubleLiteral(row.log), DoubleLiteral(row.sine), DoubleLiteral(row.cosine));
        }
    }

    const std::filesystem::path generated{"Tests/Packages/Math/Vectors/Src/Generated.rux"};
    std::filesystem::create_directories(generated.parent_path());
    std::ofstream out(generated, std::ios::binary);
    out << "// Generated by Tools/MathVectors. Do not edit by hand.\n"
           "//\n"
           "// Each row is an argument and the correctly-rounded true value of a function at it, computed in\n"
           "// fixed point with 192 fractional bits -- about 58 decimal digits -- and rounded once to nearest.\n"
           "// A test cannot check a function against itself, so the reference has to come from somewhere with\n"
           "// far more precision than the type being checked; this is that somewhere.\n\n";
    out << std::format("/// How many rows the table holds.\n"
                       "///\n"
                       "/// https://rux-lang.dev/docs/api/math/vectors/row-count\n"
                       "const RowCount: uint = {};\n\n",
                       rows.size());
    const auto column = [&](const char *name, const auto projection) {
        out << std::format("/// The {} column of the reference table.\n"
                           "///\n"
                           "/// https://rux-lang.dev/docs/api/math/vectors/{}\n"
                           "const {}: float64[{}] = [\n",
                           name, name, name, rows.size());
        for (std::size_t index = 0; index < rows.size(); ++index) {
            out << "    " << DoubleLiteral(projection(rows[index]));
            if (index + 1 != rows.size()) {
                out << ",";
            }
            out << "\n";
        }
        out << "];\n\n";
    };
    column("Arguments", [](const Row &row) { return row.argument; });
    column("Exponentials", [](const Row &row) { return row.exp; });
    column("Logarithms", [](const Row &row) { return row.log; });
    column("Sines", [](const Row &row) { return row.sine; });
    column("Cosines", [](const Row &row) { return row.cosine; });
    std::printf("wrote %zu rows to %s and Temp/MathVectors.txt\n", rows.size(), generated.string().c_str());
    return 0;
}
} // namespace

int main() {
    return Run();
}
