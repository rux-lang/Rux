#include "CodeGen/AArch64/RuntimeHelpers.h"

#include "CodeGen/FloatLiteral.h"

#include <cstring>
#include <format>
#include <utility>
#include <vector>

namespace Rux {
namespace {
struct HelperBranch {
    std::uint32_t site = 0;
    unsigned lsb = 0;
    unsigned width = 0;
    unsigned label = 0;
};

constexpr std::int32_t kFrameRecordSize = 16;
} // namespace

AArch64RuntimeHelperEmitter::AArch64RuntimeHelperEmitter(RcuModuleBuilder &inputModuleBuilder,
                                                         unsigned &inputLiteralIndex,
                                                         DiagnosticReporter inputDiagnosticReporter)
    : moduleBuilder(inputModuleBuilder)
    , enc(moduleBuilder.SectionData(RcuModuleSection::Text))
    , literalIndex(inputLiteralIndex)
    , reportDiagnostic(std::move(inputDiagnosticReporter)) {
}

std::uint32_t AArch64RuntimeHelperEmitter::Declare(const std::string_view name) {
    return moduleBuilder
        .DeclareSymbol(
            {.name = std::string(name), .typeName = {}, .kind = RcuSymKind::Func, .visibility = RcuSymVis::Local})
        .value_or(NoSymbol);
}

std::uint32_t AArch64RuntimeHelperEmitter::Require(const AArch64RuntimeHelper helper) {
    switch (helper) {
    case AArch64RuntimeHelper::IntegerPower:
        if (integerPowerSymbol == NoSymbol) {
            integerPowerSymbol = Declare("__rux_ipow");
        }
        return integerPowerSymbol;
    case AArch64RuntimeHelper::FloatPower64:
        if (floatPower64Symbol == NoSymbol) {
            floatPower64Symbol = Declare("__rux_powf64");
        }
        return floatPower64Symbol;
    case AArch64RuntimeHelper::FloatPower32:
        (void)Require(AArch64RuntimeHelper::FloatPower64);
        if (floatPower32Symbol == NoSymbol) {
            floatPower32Symbol = Declare("__rux_powf32");
        }
        return floatPower32Symbol;
    }
    return NoSymbol;
}

void AArch64RuntimeHelperEmitter::AddCallRelocation(const std::uint32_t sectionOffset,
                                                    const AArch64RuntimeHelper helper) {
    const std::uint32_t symbol = Require(helper);
    if (symbol != NoSymbol) {
        (void)moduleBuilder.AddRelocation(RcuModuleSection::Text, sectionOffset, symbol, RcuRelType::AArch64Call26);
    }
}

void AArch64RuntimeHelperEmitter::EmitRequested() {
    EmitIntegerPower();
    EmitFloatPower32();
    EmitFloatPower64();
}

std::uint32_t AArch64RuntimeHelperEmitter::InternFloat64(const std::string_view literal) {
    if (const auto symbol = moduleBuilder.InternedLiteral("f64", literal)) {
        return *symbol;
    }

    const std::uint32_t offset = moduleBuilder.AlignSection(RcuModuleSection::RoData, 8);
    const double value = ParseFloatLiteral<double>(literal);
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    auto &data = moduleBuilder.SectionData(RcuModuleSection::RoData);
    for (int index = 0; index < 8; ++index) {
        data.push_back(static_cast<std::uint8_t>(bits >> (8 * index) & 0xFFU));
    }
    const std::uint32_t symbol =
        moduleBuilder
            .AddDefinition({.name = std::format("__f64_{}", literalIndex++),
                            .typeName = {},
                            .kind = RcuSymKind::Const,
                            .visibility = RcuSymVis::Local},
                           RcuModuleSection::RoData, offset, static_cast<std::uint32_t>(data.size()) - offset)
            .value_or(NoSymbol);
    if (symbol != NoSymbol) {
        (void)moduleBuilder.RecordInternedLiteral("f64", std::string(literal), symbol);
    }
    return symbol;
}

void AArch64RuntimeHelperEmitter::LoadFloat64Constant(const A64Reg destination, const std::string_view literal) {
    const double value = ParseFloatLiteral<double>(literal);
    if (TryEncodeFpImm8(value)) {
        Must(enc.FmovImm(destination, value), "a floating-point constant");
        return;
    }

    const std::uint32_t symbol = InternFloat64(literal);
    A64SymbolRef reference{};
    Must(enc.LoadFromSymbol(destination, reference), "a floating-point constant");
    (void)moduleBuilder.AddRelocation(RcuModuleSection::Text, reference.adrp, symbol, RcuRelType::AArch64AdrPrelPgHi21);
    (void)moduleBuilder.AddRelocation(RcuModuleSection::Text, reference.lo12, symbol, RcuRelType::AArch64LdstAbsLo12Nc);
}

void AArch64RuntimeHelperEmitter::Must(const A64Status status, const std::string_view what) {
    if (status != A64Status::Ok) {
        reportDiagnostic(std::format("AArch64 code generation could not encode {} in '{}': {}", what, currentFunction,
                                     A64StatusName(status)));
    }
}

void AArch64RuntimeHelperEmitter::EmitIntegerPower() {
    if (integerPowerSymbol == NoSymbol) {
        return;
    }
    currentFunction = "__rux_ipow";
    constexpr std::string_view what = "the exponentiation helper";

    const A64Reg result = A64::Xn(0);
    const A64Reg exponent = A64::Xn(1);
    const A64Reg base = A64::Xn(2);

    if (!moduleBuilder.BeginFunction(integerPowerSymbol)) {
        return;
    }

    Must(enc.Mov(base, result), what);
    Must(enc.LoadImm64(result, 0), what); // a negative exponent yields zero
    const std::uint32_t negativeBranch = enc.Size();
    Must(enc.Tbnz(exponent, 63, 0), what);
    Must(enc.LoadImm64(result, 1), what);

    const std::uint32_t loop = enc.Size();
    const std::uint32_t doneBranch = enc.Size();
    Must(enc.Cbz(exponent, 0), what);
    const std::uint32_t squareBranch = enc.Size();
    Must(enc.Tbz(exponent, 0, 0), what);
    Must(enc.Mul(result, result, base), what); // an odd exponent takes one base

    const std::uint32_t square = enc.Size();
    Must(enc.Mul(base, base, base), what);
    Must(enc.Asr(exponent, exponent, 1), what);
    Must(enc.B(static_cast<std::int64_t>(loop) - static_cast<std::int64_t>(enc.Size())), what);

    const std::uint32_t done = enc.Size();
    Must(enc.Ret(), what);

    // The three forward branches, each patched into the field its own form
    // keeps its immediate in: fourteen bits for the test-and-branch pair
    // and nineteen for the compare-and-branch one, both starting at bit 5.
    const auto instructions = [](const std::uint32_t from, const std::uint32_t to) {
        return (to - from) / A64Enc::InstrSize;
    };
    enc.PatchField(negativeBranch, 5, 14, instructions(negativeBranch, done));
    enc.PatchField(doneBranch, 5, 19, instructions(doneBranch, done));
    enc.PatchField(squareBranch, 5, 14, instructions(squareBranch, square));

    (void)moduleBuilder.EndFunction(integerPowerSymbol);
    currentFunction.clear();
}

void AArch64RuntimeHelperEmitter::EmitFloatPower64() {
    if (floatPower64Symbol == NoSymbol) {
        return;
    }
    currentFunction = "__rux_powf64";
    constexpr std::string_view what = "the floating-point exponentiation helper";

    enum : unsigned {
        LOne,
        LRet,
        LInf,
        LNonzero,
        LMagnitude,
        LNan,
        LNormal,
        LReduced,
        LOverflow,
        LUnderflow,
        LSign,
        LabelCount,
    };

    std::vector<std::uint32_t> labels(LabelCount, 0);
    std::vector<HelperBranch> branches;
    const auto place = [&](const unsigned label) { labels[label] = enc.Size(); };
    const auto branchIf = [&](const A64Condition cond, const unsigned label) {
        branches.push_back({enc.Size(), 5, 19, label});
        Must(enc.BCond(cond, 0), what);
    };
    const auto branchIfZero = [&](const A64Reg reg, const unsigned label) {
        branches.push_back({enc.Size(), 5, 19, label});
        Must(enc.Cbz(reg, 0), what);
    };
    const auto jump = [&](const unsigned label) {
        branches.push_back({enc.Size(), 0, 26, label});
        Must(enc.B(0), what);
    };
    // A constant into a vector register, through FMOV where the encoding
    // names the value and through the read-only pool where it does not.
    const auto constant = [&](const unsigned reg, const std::string &literal) {
        LoadFloat64Constant(A64::Dn(reg), literal);
    };
    // Horner over a coefficient list, highest degree first: the accumulator
    // starts at the leading coefficient and each step multiplies by `z` and
    // adds the next one, which FMADD does in one instruction and one
    // rounding.
    const auto horner = [&](const unsigned acc, const unsigned scratch, const unsigned z,
                            const std::vector<std::string> &coefficients) {
        constant(acc, coefficients.front());
        for (std::size_t i = 1; i < coefficients.size(); ++i) {
            constant(scratch, coefficients[i]);
            Must(enc.Fmadd(A64::Dn(acc), A64::Dn(acc), A64::Dn(z), A64::Dn(scratch)), what);
        }
    };

    if (!moduleBuilder.BeginFunction(floatPower64Symbol)) {
        return;
    }

    // The special cases, in the order the x86-64 helper takes them so that
    // both give C's answers and the same ones as each other: an exponent of
    // either zero answers one whatever the base is, a NaN base answers
    // itself, and a zero base answers zero or an infinity by the sign of
    // the exponent — ignoring the sign of the zero, as that helper does.
    Must(enc.FcmpZero(A64::Dn(1)), what);
    branchIf(A64Condition::Eq, LOne);
    Must(enc.Fcmp(A64::Dn(0), A64::Dn(0)), what);
    branchIf(A64Condition::Vs, LRet);
    Must(enc.FcmpZero(A64::Dn(0)), what);
    branchIf(A64Condition::Ne, LNonzero);
    Must(enc.FcmpZero(A64::Dn(1)), what);
    branchIf(A64Condition::Mi, LInf);
    Must(enc.Fmov(A64::Dn(0), A64::Xzr), what);
    Must(enc.Ret(), what);

    place(LInf);
    Must(enc.Movz(A64::Xn(0), 0x7FF0, 48), what);
    Must(enc.Fmov(A64::Dn(0), A64::Xn(0)), what);
    Must(enc.Ret(), what);

    // A negative base raised to a fractional power has no real answer, and
    // to an integral one it has the magnitude of its own absolute value
    // with the sign of the exponent's parity. Which of the two it is comes
    // from a round trip through a 64-bit integer, as it does on x86-64: an
    // exponent too large for one is treated as fractional there and here
    // alike, which is a NaN for an exponent that is mathematically an
    // even integer and the one place both helpers leave C behind.
    place(LNonzero);
    Must(enc.LoadImm64(A64::Xn(2), 0), what); // the sign to apply at the end
    Must(enc.FcmpZero(A64::Dn(0)), what);
    branchIf(A64Condition::Pl, LMagnitude);
    Must(enc.Fcvtzs(A64::Xn(0), A64::Dn(1)), what);
    Must(enc.Scvtf(A64::Dn(2), A64::Xn(0)), what);
    Must(enc.Fcmp(A64::Dn(2), A64::Dn(1)), what);
    branchIf(A64Condition::Ne, LNan);
    Must(enc.AndImm(A64::Xn(2), A64::Xn(0), 1), what);

    // |x| = 2^k * mm, with mm in [sqrt(0.5), sqrt(2)) so that the series
    // below converges at the same rate either side of one. A subnormal has
    // no exponent field to read, so it is scaled into the normal range
    // first and the scaling taken back out of k.
    place(LMagnitude);
    Must(enc.Fabs(A64::Dn(0), A64::Dn(0)), what);
    Must(enc.Fmov(A64::Xn(0), A64::Dn(0)), what);
    Must(enc.LoadImm64(A64::Xn(3), 0), what); // k
    Must(enc.Movz(A64::Xn(1), 0x0010, 48), what);
    Must(enc.Cmp(A64::Xn(0), A64::Xn(1)), what);
    branchIf(A64::Hs, LNormal);
    Must(enc.Movz(A64::Xn(4), 0x4350, 48), what); // 2^54
    Must(enc.Fmov(A64::Dn(2), A64::Xn(4)), what);
    Must(enc.Fmul(A64::Dn(0), A64::Dn(0), A64::Dn(2)), what);
    Must(enc.Fmov(A64::Xn(0), A64::Dn(0)), what);
    Must(enc.LoadImm64(A64::Xn(3), static_cast<std::uint64_t>(-54)), what);

    place(LNormal);
    Must(enc.Lsr(A64::Xn(4), A64::Xn(0), 52), what);
    Must(enc.Add(A64::Xn(3), A64::Xn(3), A64::Xn(4)), what);
    Must(enc.SubImm(A64::Xn(3), A64::Xn(3), 1023), what);
    Must(enc.AndImm(A64::Xn(4), A64::Xn(0), 0x000FFFFFFFFFFFFFULL), what);
    Must(enc.OrrImm(A64::Xn(4), A64::Xn(4), 0x3FF0000000000000ULL), what);
    Must(enc.Fmov(A64::Dn(2), A64::Xn(4)), what); // mm in [1, 2)
    constant(3, "1.4142135623730951");
    Must(enc.Fcmp(A64::Dn(2), A64::Dn(3)), what);
    branchIf(A64Condition::Ls, LReduced);
    constant(3, "0.5");
    Must(enc.Fmul(A64::Dn(2), A64::Dn(2), A64::Dn(3)), what);
    Must(enc.AddImm(A64::Xn(3), A64::Xn(3), 1), what);

    // log2(mm) = (2 / ln2) * s * (1 + s^2/3 + s^4/5 + ...) with
    // s = (mm - 1) / (mm + 1), which is the series that converges fastest
    // over this interval: |s| stays below 0.172, so its square is below
    // 0.03 and thirteen terms reach the precision the product needs.
    //
    // Both ends of the quotient are carried exactly. The numerator is
    // exact on its own — a difference of two numbers within a factor of two
    // of each other always is — and the denominator's lost low bit is
    // recovered as `dlo`, so that s is a pair of doubles rather than one
    // rounded to 53 bits, which is what a logarithm scaled by an exponent
    // of a thousand needs.
    place(LReduced);
    constant(4, "1.0");
    Must(enc.Fsub(A64::Dn(3), A64::Dn(2), A64::Dn(4)), what); // num = mm - 1
    Must(enc.Fadd(A64::Dn(4), A64::Dn(2), A64::Dn(4)), what); // dhi = mm + 1
    Must(enc.Fsub(A64::Dn(5), A64::Dn(4), A64::Dn(2)), what);
    Must(enc.Fsub(A64::Dn(6), A64::Dn(4), A64::Dn(5)), what);
    Must(enc.Fsub(A64::Dn(6), A64::Dn(2), A64::Dn(6)), what);
    constant(7, "1.0");
    Must(enc.Fsub(A64::Dn(7), A64::Dn(7), A64::Dn(5)), what);
    Must(enc.Fadd(A64::Dn(6), A64::Dn(6), A64::Dn(7)), what);  // dlo
    Must(enc.Fdiv(A64::Dn(16), A64::Dn(3), A64::Dn(4)), what); // q0
    Must(enc.Fmsub(A64::Dn(7), A64::Dn(16), A64::Dn(4), A64::Dn(3)), what);
    Must(enc.Fmul(A64::Dn(17), A64::Dn(16), A64::Dn(6)), what);
    Must(enc.Fsub(A64::Dn(7), A64::Dn(7), A64::Dn(17)), what);
    Must(enc.Fdiv(A64::Dn(17), A64::Dn(7), A64::Dn(4)), what); // q1
    Must(enc.Fmul(A64::Dn(18), A64::Dn(16), A64::Dn(16)), what);
    horner(19, 20, 18,
           {"0.037037037037037035", "0.04", "0.043478260869565216", "0.047619047619047616", "0.05263157894736842",
            "0.058823529411764705", "0.06666666666666667", "0.07692307692307693", "0.09090909090909091",
            "0.1111111111111111", "0.14285714285714285", "0.2", "0.3333333333333333"});
    Must(enc.Fmul(A64::Dn(19), A64::Dn(19), A64::Dn(18)), what);

    // The series correction is folded into s before the scaling, so that
    // the pair carrying the result stays normalized: it is a hundredth of
    // the value at most, which is far above one unit in the last place, and
    // leaving it in the low half would put it into the exponent's rounding
    // rather than into the mantissa's.
    Must(enc.Fmul(A64::Dn(21), A64::Dn(16), A64::Dn(19)), what);
    Must(enc.Fnmsub(A64::Dn(22), A64::Dn(16), A64::Dn(19), A64::Dn(21)), what);
    Must(enc.Fadd(A64::Dn(23), A64::Dn(16), A64::Dn(21)), what); // shi
    Must(enc.Fsub(A64::Dn(24), A64::Dn(16), A64::Dn(23)), what);
    Must(enc.Fadd(A64::Dn(24), A64::Dn(24), A64::Dn(21)), what);
    Must(enc.Fadd(A64::Dn(24), A64::Dn(24), A64::Dn(17)), what);
    Must(enc.Fadd(A64::Dn(24), A64::Dn(24), A64::Dn(22)), what); // slo
    constant(25, "2.8853900817779268");                          // 2 / ln2, high half
    Must(enc.Fmul(A64::Dn(26), A64::Dn(25), A64::Dn(23)), what);
    Must(enc.Fnmsub(A64::Dn(27), A64::Dn(25), A64::Dn(23), A64::Dn(26)), what);
    Must(enc.Fmadd(A64::Dn(27), A64::Dn(25), A64::Dn(24), A64::Dn(27)), what);
    constant(28, "4.0710547481862066e-17"); // 2 / ln2, low half
    Must(enc.Fmadd(A64::Dn(27), A64::Dn(28), A64::Dn(23), A64::Dn(27)), what);

    // log2 |x| = k + that, and then w = y * log2 |x|, both as pairs. The
    // integer k is the larger of the two terms wherever it is not zero, so
    // the remainder of the addition is exact and needs no test.
    Must(enc.Scvtf(A64::Dn(29), A64::Xn(3)), what);
    Must(enc.Fadd(A64::Dn(30), A64::Dn(29), A64::Dn(26)), what);
    Must(enc.Fsub(A64::Dn(31), A64::Dn(29), A64::Dn(30)), what);
    Must(enc.Fadd(A64::Dn(31), A64::Dn(31), A64::Dn(26)), what);
    Must(enc.Fadd(A64::Dn(27), A64::Dn(31), A64::Dn(27)), what);
    Must(enc.Fmul(A64::Dn(2), A64::Dn(1), A64::Dn(30)), what);
    Must(enc.Fnmsub(A64::Dn(3), A64::Dn(1), A64::Dn(30), A64::Dn(2)), what);
    Must(enc.Fmadd(A64::Dn(3), A64::Dn(1), A64::Dn(27), A64::Dn(3)), what);

    // A product past either end of the exponent range is an infinity or a
    // zero outright. Testing here rather than clamping keeps the reduction
    // below honest: its argument is a difference from a rounded integer,
    // which means nothing once the value it came from has been changed.
    constant(4, "1025.0");
    Must(enc.Fcmp(A64::Dn(2), A64::Dn(4)), what);
    branchIf(A64Condition::Gt, LOverflow);
    constant(4, "-1100.0");
    Must(enc.Fcmp(A64::Dn(2), A64::Dn(4)), what);
    branchIf(A64Condition::Mi, LUnderflow);

    // 2^w = 2^n * 2^r with n the nearest integer, so that |r| is at most a
    // half and the polynomial below is asked for nothing further out. The
    // exponential itself is fdlibm's: with the argument in natural units as
    // a pair `ph + plo`, exp(x) = 1 + x + x*c/(2 - c) where c is x less an
    // odd polynomial in x squared, which needs five coefficients where a
    // direct series would need fourteen.
    Must(enc.Frintn(A64::Dn(4), A64::Dn(2)), what);
    Must(enc.Fsub(A64::Dn(5), A64::Dn(2), A64::Dn(4)), what);
    Must(enc.Fadd(A64::Dn(5), A64::Dn(5), A64::Dn(3)), what);
    constant(6, "0.6931471805599453"); // ln2, high half
    Must(enc.Fmul(A64::Dn(7), A64::Dn(5), A64::Dn(6)), what);
    Must(enc.Fnmsub(A64::Dn(16), A64::Dn(5), A64::Dn(6), A64::Dn(7)), what);
    constant(6, "2.3190468138462996e-17"); // ln2, low half
    Must(enc.Fmadd(A64::Dn(16), A64::Dn(5), A64::Dn(6), A64::Dn(16)), what);
    Must(enc.Fadd(A64::Dn(17), A64::Dn(7), A64::Dn(16)), what);
    Must(enc.Fmul(A64::Dn(18), A64::Dn(17), A64::Dn(17)), what);
    horner(19, 20, 18,
           {"4.13813679705723846039e-08", "-1.65339022054652515390e-06", "6.61375632143793436117e-05",
            "-2.77777777770155933842e-03", "1.66666666666666019037e-01"});
    Must(enc.Fmsub(A64::Dn(19), A64::Dn(19), A64::Dn(18), A64::Dn(17)), what); // c
    Must(enc.Fmul(A64::Dn(20), A64::Dn(17), A64::Dn(19)), what);
    constant(21, "2.0");
    Must(enc.Fsub(A64::Dn(21), A64::Dn(21), A64::Dn(19)), what);
    Must(enc.Fdiv(A64::Dn(20), A64::Dn(20), A64::Dn(21)), what);
    Must(enc.Fneg(A64::Dn(21), A64::Dn(16)), what);
    Must(enc.Fsub(A64::Dn(20), A64::Dn(21), A64::Dn(20)), what);
    Must(enc.Fsub(A64::Dn(20), A64::Dn(20), A64::Dn(7)), what);
    constant(21, "1.0");
    Must(enc.Fsub(A64::Dn(21), A64::Dn(21), A64::Dn(20)), what); // 2^r

    // 2^n is its exponent field written down, in two halves: n reaches a
    // thousand either way, which no single exponent field holds once the
    // bias is added, and the product of the two halves underflows through
    // the subnormals the way the answer itself should.
    Must(enc.Fcvtzs(A64::Xn(0), A64::Dn(4)), what);
    Must(enc.Asr(A64::Xn(1), A64::Xn(0), 1), what);
    Must(enc.Sub(A64::Xn(0), A64::Xn(0), A64::Xn(1)), what);
    Must(enc.AddImm(A64::Xn(1), A64::Xn(1), 1023), what);
    Must(enc.AddImm(A64::Xn(0), A64::Xn(0), 1023), what);
    Must(enc.Lsl(A64::Xn(1), A64::Xn(1), 52), what);
    Must(enc.Lsl(A64::Xn(0), A64::Xn(0), 52), what);
    Must(enc.Fmov(A64::Dn(22), A64::Xn(1)), what);
    Must(enc.Fmov(A64::Dn(23), A64::Xn(0)), what);
    Must(enc.Fmul(A64::Dn(21), A64::Dn(21), A64::Dn(22)), what);
    Must(enc.Fmul(A64::Dn(0), A64::Dn(21), A64::Dn(23)), what);

    place(LSign);
    branchIfZero(A64::Xn(2), LRet);
    Must(enc.Fneg(A64::Dn(0), A64::Dn(0)), what);
    place(LRet);
    Must(enc.Ret(), what);

    place(LOne);
    constant(0, "1.0");
    Must(enc.Ret(), what);

    place(LNan);
    Must(enc.Movz(A64::Xn(0), 0x7FF8, 48), what);
    Must(enc.Fmov(A64::Dn(0), A64::Xn(0)), what);
    Must(enc.Ret(), what);

    place(LOverflow);
    Must(enc.Movz(A64::Xn(0), 0x7FF0, 48), what);
    Must(enc.Fmov(A64::Dn(0), A64::Xn(0)), what);
    jump(LSign);

    place(LUnderflow);
    Must(enc.Fmov(A64::Dn(0), A64::Xzr), what);
    jump(LSign);

    for (const auto &branch : branches) {
        const std::uint32_t target = (labels[branch.label] - branch.site) / A64Enc::InstrSize;
        enc.PatchField(branch.site, branch.lsb, branch.width, target);
    }

    (void)moduleBuilder.EndFunction(floatPower64Symbol);
    currentFunction.clear();
}

void AArch64RuntimeHelperEmitter::EmitFloatPower32() {
    if (floatPower32Symbol == NoSymbol) {
        return;
    }
    currentFunction = "__rux_powf32";
    constexpr std::string_view what = "the single-precision exponentiation helper";

    if (!moduleBuilder.BeginFunction(floatPower32Symbol)) {
        return;
    }

    Must(enc.Stp(A64::Fp, A64::Lr, A64::Sp, -kFrameRecordSize, A64IndexMode::PreIndex), what);
    Must(enc.Mov(A64::Fp, A64::Sp), what);
    Must(enc.Fcvt(A64::Dn(0), A64::Sn(0)), what);
    Must(enc.Fcvt(A64::Dn(1), A64::Sn(1)), what);
    const std::uint32_t site = enc.Size();
    Must(enc.Bl(0), what);
    (void)moduleBuilder.AddRelocation(RcuModuleSection::Text, site, Require(AArch64RuntimeHelper::FloatPower64),
                                      RcuRelType::AArch64Call26);
    Must(enc.Fcvt(A64::Sn(0), A64::Dn(0)), what);
    Must(enc.Ldp(A64::Fp, A64::Lr, A64::Sp, kFrameRecordSize, A64IndexMode::PostIndex), what);
    Must(enc.Ret(), what);

    (void)moduleBuilder.EndFunction(floatPower32Symbol);
    currentFunction.clear();
}

} // namespace Rux
