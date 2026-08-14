// Inline-assembly function body, instruction, and operand parsing.

#include "Syntax/Parser/Parser.h"

#include <cctype>
#include <charconv>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace Rux {
namespace {

// Inside an asm body, a language keyword may still name a register, symbol or
// label because the lexer has already classified it before reaching the parser.
bool IsAsmNameToken(const Token &token) {
    return token.Is(TokenKind::Ident) || token.IsKeyword();
}

std::string LowerAsmName(std::string name) {
    for (char &character : name) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return name;
}

// x86-64 mnemonics that never take an operand. They have to be listed because
// an identifier after one of them starts the next instruction rather than an
// operand, and x86-64 operand syntax gives no other way to tell. AArch64 needs
// no such list: its operands are registers, `#` immediates and brackets, so
// `CanStartAsmOperand` asks the mnemonic table instead.
bool IsZeroOperandAsmMnemonic(const std::string_view mnemonic) {
    return mnemonic == "ret" || mnemonic == "leave" || mnemonic == "nop" || mnemonic == "syscall" ||
           mnemonic == "cqo" || mnemonic == "cdq" || mnemonic == "cdqe";
}

// AArch64 shift and extend keywords, as written after a register or immediate
// operand and inside a register-offset memory operand.
AsmShiftKind AsmShiftFromName(const std::string_view name) noexcept {
    if (name == "lsl") {
        return AsmShiftKind::Lsl;
    }
    if (name == "lsr") {
        return AsmShiftKind::Lsr;
    }
    if (name == "asr") {
        return AsmShiftKind::Asr;
    }
    if (name == "ror") {
        return AsmShiftKind::Ror;
    }
    return AsmShiftKind::None;
}

AsmExtendKind AsmExtendFromName(const std::string_view name) noexcept {
    static constexpr std::pair<std::string_view, AsmExtendKind> table[8] = {
        {"uxtb", AsmExtendKind::Uxtb}, {"uxth", AsmExtendKind::Uxth}, {"uxtw", AsmExtendKind::Uxtw},
        {"uxtx", AsmExtendKind::Uxtx}, {"sxtb", AsmExtendKind::Sxtb}, {"sxth", AsmExtendKind::Sxth},
        {"sxtw", AsmExtendKind::Sxtw}, {"sxtx", AsmExtendKind::Sxtx},
    };
    for (const auto &[text, kind] : table) {
        if (name == text) {
            return kind;
        }
    }
    return AsmExtendKind::None;
}

} // namespace

// An asm body is a sequence of instructions and label definitions between the
// braces of an `asm func`. Newlines are not significant to the lexer, so an
// instruction's operand list simply ends at the first token that is not a
// comma — the next mnemonic, a label, or the closing brace.
std::vector<AsmInstr> Parser::ParseAsmBody() {
    std::vector<AsmInstr> instrs;
    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        // A label definition: `name:`.
        if (IsAsmNameToken(Peek()) && Peek(1).Is(TokenKind::Colon)) {
            AsmInstr label;
            label.arch = arch;
            label.location = CurrentLocation();
            label.labelDef = Advance().text; // name
            Advance();                       // ':'
            instrs.push_back(std::move(label));
            continue;
        }

        if (!IsAsmNameToken(Peek())) {
            EmitError(CurrentLocation(), std::format("expected an assembly mnemonic, found '{}'", Peek().text));
            Advance(); // skip the offending token to make progress
            continue;
        }

        AsmInstr instr;
        instr.arch = arch;
        instr.location = CurrentLocation();
        instr.mnemonic = LowerAsmName(Advance().text);

        // AArch64 writes a branch's condition into its name — `B.EQ` — and the
        // lexer hands the three pieces over separately. Nothing in x86-64
        // syntax joins a mnemonic to a name with a dot, so the form is read
        // there too, for the reason CanStartAsmOperand reads '#' there: a
        // `when #target.arch` arm for another architecture has to parse before
        // it can be discarded, and what survives is reported as the foreign
        // instruction it is rather than as a stray token.
        if (Check(TokenKind::Dot) && IsAsmNameToken(Peek(1))) {
            Advance(); // '.'
            instr.mnemonic += '.';
            instr.mnemonic += LowerAsmName(Advance().text);
        }

        // Operands, comma-separated. Stop when the operand is not followed
        // by a comma (i.e. the next token starts a new instruction).
        const bool zeroOperand = arch != Target::Arch::AArch64 && IsZeroOperandAsmMnemonic(instr.mnemonic);
        if (zeroOperand || (!Check(TokenKind::RightBrace) && !CanStartAsmOperand())) {
            instrs.push_back(std::move(instr));
            continue;
        }
        while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
            instr.operands.push_back(ParseAsmOperand());
            if (!Match(TokenKind::Comma)) {
                break;
            }
        }
        instrs.push_back(std::move(instr));
    }
    return instrs;
}

// True when the current token can begin an operand of the instruction whose
// mnemonic was just consumed. Used to tell a zero-operand instruction (ret,
// syscall) followed by another mnemonic apart from one that takes operands.
bool Parser::CanStartAsmOperand() const noexcept {
    switch (Peek().kind) {
    case TokenKind::IntLiteral:
    case TokenKind::LeftBracket:
    case TokenKind::Minus:
    case TokenKind::Plus:
        return true;
    case TokenKind::Hash:
        // AArch64 marks an immediate with '#'. Nothing in x86-64 syntax starts
        // with one, so it is accepted there too and reported as the foreign
        // instruction it belongs to rather than as a stray token.
        return true;
    default:
        // An identifier-like token begins a new instruction if it is itself a
        // label definition (`name :`); otherwise it is a register / symbol.
        if (!IsAsmNameToken(Peek()) || Peek(1).Is(TokenKind::Colon)) {
            return false;
        }
        if (arch == Target::Arch::AArch64) {
            // `RET` takes an optional register and `B` a label, so whether an
            // identifier continues this instruction or starts the next one is
            // decided by what the identifier is: a register or a symbol
            // continues, an instruction name starts.
            const std::string lowered = LowerAsmName(Peek().text);
            return IsRegisterName(arch, lowered) || !IsAsmMnemonic(arch, lowered);
        }
        return true;
    }
}

// Parse one operand: a register, an immediate, a `[...]` memory reference, a
// size-prefixed memory reference (qword [...]), or a symbol / label name.
AsmOperand Parser::ParseAsmOperand() {
    AsmOperand op;
    op.location = CurrentLocation();

    // Optional size specifier before a memory operand: byte/word/dword/qword.
    int sizeHint = 0;
    if (Check(TokenKind::Ident)) {
        const std::string &text = Peek().text;
        if (text == "byte") {
            sizeHint = 1;
        }
        else if (text == "word") {
            sizeHint = 2;
        }
        else if (text == "dword") {
            sizeHint = 4;
        }
        else if (text == "qword") {
            sizeHint = 8;
        }
        if (sizeHint != 0 && Peek(1).Is(TokenKind::LeftBracket)) {
            Advance();               // size keyword
            Match(TokenKind::Ident); // optional 'ptr'
        }
        else {
            sizeHint = 0;
        }
    }

    if (Check(TokenKind::LeftBracket)) {
        ParseAsmMemory(op);
        op.memSize = sizeHint;
        return op;
    }

    // AArch64 marks an immediate with '#'; both architectures accept a bare
    // integer, which is the only spelling x86-64 has.
    const bool hashed = Match(TokenKind::Hash);
    if (hashed || CheckAny({TokenKind::IntLiteral, TokenKind::Minus, TokenKind::Plus})) {
        op.kind = AsmOperand::Kind::Imm;
        op.imm = ParseAsmInt();
        ParseAsmShift(op); // `#1, LSL #12`
        return op;
    }

    // An identifier-like token: either a register or a symbol / label reference.
    if (!IsAsmNameToken(Peek())) {
        EmitError(CurrentLocation(), std::format("expected an assembly operand, found '{}'", Peek().text));
        return op;
    }
    const Token &token = Advance();
    std::string name = token.text;
    std::string lowered = LowerAsmName(name);
    if (IsRegisterName(arch, lowered)) {
        op.kind = AsmOperand::Kind::Reg;
        op.name = std::move(lowered);
        ParseAsmShift(op); // `X1, LSL #3`
    }
    else {
        // A condition (`CSEL X0, X1, X2, EQ`) arrives here too: which of the
        // two an instruction wanted is the assembler's to say.
        op.kind = AsmOperand::Kind::Sym;
        op.name = std::move(name);
    }
    return op;
}

// AArch64: the shift or extend a register or immediate operand may carry, as
// the tail of the operand rather than as an operand of its own. Recognized
// only after a comma, so an instruction whose name is a shift keyword — `LSL`
// is an alias of `UBFM` — still starts a new instruction when one follows.
//
// Read for every architecture rather than only for AArch64: an AArch64 body
// compiled for x86-64 is diagnosed by its mnemonics, and reading it as far as
// the assembler keeps that one diagnostic from arriving behind a pile of
// syntax errors about a syntax the target simply does not have.
void Parser::ParseAsmShift(AsmOperand &op) {
    if (!Check(TokenKind::Comma) || !IsAsmNameToken(Peek(1))) {
        return;
    }
    const std::string keyword = LowerAsmName(Peek(1).text);
    const AsmShiftKind shift = AsmShiftFromName(keyword);
    const AsmExtendKind extend = AsmExtendFromName(keyword);
    if (shift == AsmShiftKind::None && extend == AsmExtendKind::None) {
        return;
    }
    Advance(); // ','
    Advance(); // the keyword
    op.shift = shift;
    op.extend = extend;
    // An extend without an amount shifts by nothing; a shift always says how far.
    if (Match(TokenKind::Hash) || CheckAny({TokenKind::IntLiteral, TokenKind::Minus, TokenKind::Plus})) {
        op.shiftAmount = static_cast<int>(ParseAsmInt());
    }
    else if (shift != AsmShiftKind::None) {
        EmitError(CurrentLocation(), std::format("expected a shift amount after '{}'", keyword));
    }
}

// Parse a memory operand. x86-64 writes `[base + index*scale +/- disp]`, where
// any of the three may be omitted; AArch64 writes `[Xn]`, `[Xn, #off]`,
// `[Xn, Xm]`, `[Xn, Xm, LSL #3]` and `[Xn, Wm, UXTW #2]`, followed by `!` for
// pre-index or, outside the brackets, `, #off` for post-index. One loop reads
// both: the separators an architecture does not use simply never appear.
void Parser::ParseAsmMemory(AsmOperand &op) {
    op.kind = AsmOperand::Kind::Mem;
    Expect(TokenKind::LeftBracket, "expected '['");
    bool negateNext = false;
    while (!Check(TokenKind::RightBracket) && !IsAtEnd()) {
        if (Match(TokenKind::Plus)) {
            negateNext = false;
            continue;
        }
        if (Match(TokenKind::Minus)) {
            negateNext = true;
            continue;
        }
        if (Match(TokenKind::Comma) || Match(TokenKind::Hash)) {
            continue;
        }
        if (Check(TokenKind::IntLiteral)) {
            std::int64_t value = ParseAsmInt();
            op.imm += negateNext ? -value : value;
            negateNext = false;
            continue;
        }
        if (IsAsmNameToken(Peek())) {
            std::string name = Advance().text;
            std::string lowered = LowerAsmName(name);
            // Scaled index: reg * scale.
            if (Match(TokenKind::Star)) {
                op.memIndex = std::move(lowered);
                op.memScale = static_cast<int>(ParseAsmInt());
            }
            else if (lowered == "rip") {
                op.memBase = "rip";
            }
            else if (AsmShiftFromName(lowered) != AsmShiftKind::None) {
                op.shift = AsmShiftFromName(lowered);
                Match(TokenKind::Hash);
                op.shiftAmount = static_cast<int>(ParseAsmInt());
            }
            else if (AsmExtendFromName(lowered) != AsmExtendKind::None) {
                op.extend = AsmExtendFromName(lowered);
                // The amount is optional: `[X0, W1, UXTW]` scales by nothing.
                if (Match(TokenKind::Hash) || Check(TokenKind::IntLiteral)) {
                    op.shiftAmount = static_cast<int>(ParseAsmInt());
                }
            }
            else if (IsRegisterName(arch, lowered)) {
                if (op.memBase.empty()) {
                    op.memBase = std::move(lowered);
                }
                else {
                    op.memIndex = std::move(lowered);
                }
            }
            else {
                op.memSym = std::move(name);
            }
            continue;
        }
        EmitError(CurrentLocation(), std::format("unexpected token '{}' in memory operand", Peek().text));
        Advance();
    }
    Expect(TokenKind::RightBracket, "expected ']'");

    // Writeback: `[X0, #8]!` updates the base before the access, `[X0], #8`
    // after it. The post-index offset sits outside the brackets, so it has to
    // be taken here — the operand list would otherwise read it as an operand
    // of its own. It is recognized by the '#', which is what keeps the x86-64
    // `mov qword [rsp - 8], 5` from being read as one.
    if (Match(TokenKind::Bang)) {
        op.indexMode = AsmIndexMode::PreIndex;
    }
    else if (Check(TokenKind::Comma) && Peek(1).Is(TokenKind::Hash)) {
        Advance(); // ','
        Advance(); // '#'
        op.imm = ParseAsmInt();
        op.indexMode = AsmIndexMode::PostIndex;
    }
}

// Parse an optionally-signed integer literal (decimal, hex, octal, binary).
std::int64_t Parser::ParseAsmInt() {
    bool negative = false;
    if (Match(TokenKind::Minus)) {
        negative = true;
    }
    else {
        Match(TokenKind::Plus);
    }
    const Token &token = Expect(TokenKind::IntLiteral, "expected an integer");
    std::string text;
    for (const char character : token.text) {
        if (character != '_') {
            text.push_back(character);
        }
    }
    int base = 10;
    std::string_view digits(text);
    if (digits.size() > 2 && digits[0] == '0') {
        switch (digits[1]) {
        case 'x':
        case 'X':
            base = 16;
            digits.remove_prefix(2);
            break;
        case 'b':
        case 'B':
            base = 2;
            digits.remove_prefix(2);
            break;
        case 'o':
        case 'O':
            base = 8;
            digits.remove_prefix(2);
            break;
        default:
            break;
        }
    }
    std::uint64_t value = 0;
    std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
    auto result = static_cast<std::int64_t>(value);
    return negative ? -result : result;
}

} // namespace Rux
