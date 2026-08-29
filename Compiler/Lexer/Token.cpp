#include "Lexer/Token.h"

#include <unordered_map>

namespace Rux {
TokenKind KeywordKind(const std::string_view text) noexcept {
    // Static table built once; string_view keys are fine because the
    // string literals they point to have static storage duration.
    static const std::unordered_map<std::string_view, TokenKind> kTable = {
        {"as", TokenKind::AsKeyword},
        {"break", TokenKind::BreakKeyword},
        {"const", TokenKind::ConstKeyword},
        {"continue", TokenKind::ContinueKeyword},
        {"defer", TokenKind::DeferKeyword},
        {"do", TokenKind::DoKeyword},
        {"else", TokenKind::ElseKeyword},
        {"enum", TokenKind::EnumKeyword},
        {"extend", TokenKind::ExtendKeyword},
        {"extern", TokenKind::ExternKeyword},
        {"false", TokenKind::BoolLiteral},
        {"for", TokenKind::ForKeyword},
        {"func", TokenKind::FuncKeyword},
        {"if", TokenKind::IfKeyword},
        {"import", TokenKind::ImportKeyword},
        {"in", TokenKind::InKeyword},
        {"interface", TokenKind::InterfaceKeyword},
        {"intrinsic", TokenKind::IntrinsicKeyword},
        {"is", TokenKind::IsKeyword},
        {"let", TokenKind::LetKeyword},
        {"loop", TokenKind::LoopKeyword},
        {"match", TokenKind::MatchKeyword},
        {"module", TokenKind::ModuleKeyword},
        {"null", TokenKind::NullKeyword},
        {"pub", TokenKind::PubKeyword},
        {"return", TokenKind::ReturnKeyword},
        {"self", TokenKind::SelfKeyword},
        {"struct", TokenKind::StructKeyword},
        {"true", TokenKind::BoolLiteral},
        {"type", TokenKind::TypeKeyword},
        {"union", TokenKind::UnionKeyword},
        {"var", TokenKind::VarKeyword},
        {"variant", TokenKind::VariantKeyword},
        {"when", TokenKind::WhenKeyword},
        {"while", TokenKind::WhileKeyword},
    };
    if (const auto it = kTable.find(text); it != kTable.end()) {
        return it->second;
    }
    return TokenKind::Ident;
}

std::string_view TokenKindName(const TokenKind kind) noexcept {
    switch (kind) {
    case TokenKind::IntLiteral:
        return "IntLiteral";
    case TokenKind::FloatLiteral:
        return "FloatLiteral";
    case TokenKind::StringLiteral:
        return "StringLiteral";
    case TokenKind::CharLiteral:
        return "CharLiteral";
    case TokenKind::BoolLiteral:
        return "BoolLiteral";
    case TokenKind::Ident:
        return "Identifier";
    case TokenKind::IfKeyword:
        return "IfKeyword";
    case TokenKind::WhenKeyword:
        return "WhenKeyword";
    case TokenKind::ElseKeyword:
        return "ElseKeyword";
    case TokenKind::WhileKeyword:
        return "WhileKeyword";
    case TokenKind::DoKeyword:
        return "DoKeyword";
    case TokenKind::LoopKeyword:
        return "LoopKeyword";
    case TokenKind::ForKeyword:
        return "ForKeyword";
    case TokenKind::InKeyword:
        return "InKeyword";
    case TokenKind::BreakKeyword:
        return "BreakKeyword";
    case TokenKind::ContinueKeyword:
        return "ContinueKeyword";
    case TokenKind::ReturnKeyword:
        return "ReturnKeyword";
    case TokenKind::MatchKeyword:
        return "MatchKeyword";
    case TokenKind::DeferKeyword:
        return "DeferKeyword";
    case TokenKind::IntrinsicKeyword:
        return "IntrinsicKeyword";
    case TokenKind::FuncKeyword:
        return "FuncKeyword";
    case TokenKind::LetKeyword:
        return "LetKeyword";
    case TokenKind::VarKeyword:
        return "VarKeyword";
    case TokenKind::ConstKeyword:
        return "ConstKeyword";
    case TokenKind::TypeKeyword:
        return "TypeKeyword";
    case TokenKind::StructKeyword:
        return "StructKeyword";
    case TokenKind::EnumKeyword:
        return "EnumKeyword";
    case TokenKind::VariantKeyword:
        return "VariantKeyword";
    case TokenKind::UnionKeyword:
        return "UnionKeyword";
    case TokenKind::InterfaceKeyword:
        return "InterfaceKeyword";
    case TokenKind::ExtendKeyword:
        return "ExtendKeyword";
    case TokenKind::ModuleKeyword:
        return "ModuleKeyword";
    case TokenKind::ImportKeyword:
        return "ImportKeyword";
    case TokenKind::PubKeyword:
        return "PubKeyword";
    case TokenKind::ExternKeyword:
        return "ExternKeyword";
    case TokenKind::AsKeyword:
        return "AsKeyword";
    case TokenKind::IsKeyword:
        return "IsKeyword";
    case TokenKind::NullKeyword:
        return "NullKeyword";
    case TokenKind::SelfKeyword:
        return "SelfKeyword";
    case TokenKind::LeftParen:
        return "LeftParen";
    case TokenKind::RightParen:
        return "RightParen";
    case TokenKind::LeftBrace:
        return "LeftBrace";
    case TokenKind::RightBrace:
        return "RightBrace";
    case TokenKind::LeftBracket:
        return "LeftBracket";
    case TokenKind::RightBracket:
        return "RightBracket";
    case TokenKind::Comma:
        return "Comma";
    case TokenKind::Semicolon:
        return "Semicolon";
    case TokenKind::Colon:
        return "Colon";
    case TokenKind::ColonColon:
        return "ColonColon";
    case TokenKind::Dot:
        return "Dot";
    case TokenKind::DotDot:
        return "DotDot";
    case TokenKind::DotDotDot:
        return "DotDotDot";
    case TokenKind::DotDotEqual:
        return "DotDotEqual";
    case TokenKind::Arrow:
        return "Arrow";
    case TokenKind::MoveArrow:
        return "MoveArrow";
    case TokenKind::FatArrow:
        return "FatArrow";
    case TokenKind::At:
        return "At";
    case TokenKind::Hash:
        return "Hash";
    case TokenKind::Question:
        return "Question";
    case TokenKind::Plus:
        return "Plus";
    case TokenKind::Minus:
        return "Minus";
    case TokenKind::Star:
        return "Star";
    case TokenKind::Slash:
        return "Slash";
    case TokenKind::Percent:
        return "Percent";
    case TokenKind::PlusPlus:
        return "PlusPlus";
    case TokenKind::MinusMinus:
        return "MinusMinus";
    case TokenKind::Amp:
        return "Amp";
    case TokenKind::Pipe:
        return "Pipe";
    case TokenKind::Caret:
        return "Caret";
    case TokenKind::Tilde:
        return "Tilde";
    case TokenKind::LessLess:
        return "LessLess";
    case TokenKind::GreaterGreater:
        return "GreaterGreater";
    case TokenKind::GreaterGreaterGreater:
        return "GreaterGreaterGreater";
    case TokenKind::AmpAmp:
        return "AmpAmp";
    case TokenKind::PipePipe:
        return "PipePipe";
    case TokenKind::Bang:
        return "Bang";
    case TokenKind::Equal:
        return "Equal";
    case TokenKind::BangEqual:
        return "BangEqual";
    case TokenKind::Less:
        return "Less";
    case TokenKind::LessEqual:
        return "LessEqual";
    case TokenKind::Greater:
        return "Greater";
    case TokenKind::GreaterEqual:
        return "GreaterEqual";
    case TokenKind::Assign:
        return "Assign";
    case TokenKind::PlusAssign:
        return "PlusAssign";
    case TokenKind::MinusAssign:
        return "MinusAssign";
    case TokenKind::StarAssign:
        return "StarAssign";
    case TokenKind::SlashAssign:
        return "SlashAssign";
    case TokenKind::PercentAssign:
        return "PercentAssign";
    case TokenKind::AmpAssign:
        return "AmpAssign";
    case TokenKind::PipeAssign:
        return "PipeAssign";
    case TokenKind::CaretAssign:
        return "CaretAssign";
    case TokenKind::LessLessAssign:
        return "LessLessAssign";
    case TokenKind::GreaterGreaterAssign:
        return "GreaterGreaterAssign";
    case TokenKind::GreaterGreaterGreaterAssign:
        return "GreaterGreaterGreaterAssign";
    case TokenKind::DocComment:
        return "DocComment";
    case TokenKind::NewLine:
        return "NewLine";
    case TokenKind::EndOfFile:
        return "EndOfFile";
    case TokenKind::Unknown:
        return "Unknown";
    }
    return "<unknown>";
}

bool Token::IsKeyword() const noexcept {
    return kind >= TokenKind::AsKeyword && kind <= TokenKind::WhileKeyword;
}

bool Token::IsLiteral() const noexcept {
    return kind >= TokenKind::IntLiteral && kind <= TokenKind::BoolLiteral;
}

bool Token::IsOperator() const noexcept {
    return kind >= TokenKind::Plus && kind <= TokenKind::GreaterGreaterGreaterAssign;
}

std::string Token::Describe() const {
    std::string d(TokenKindName(kind));
    if (kind == TokenKind::Ident || IsLiteral()) {
        d += " `" + text + "`";
    }
    return d;
}
} // namespace Rux
