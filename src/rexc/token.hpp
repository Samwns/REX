#pragma once
/*
 * token.hpp  –  REXC Token definitions
 *
 * Defines the complete set of token kinds covering all C++17 lexical
 * categories, plus the Token and SourceLocation value types.
 */

#include <string>
#include <cstdint>

namespace rexc {

// ─────────────────────────────────────────────────────────────────
// TokenKind  –  every distinct lexical category
// ─────────────────────────────────────────────────────────────────
enum class TokenKind {
    // ── Literals ─────────────────────────────────────────────────
    IntLiteral,       // 42  0xFF  0b101  0777  1ULL
    FloatLiteral,     // 3.14  1.0f  2.5e-3
    StringLiteral,    // "hello"  R"(raw)"  L"wide"
    CharLiteral,      // 'a'  '\n'  L'x'
    BoolLiteral,      // true  false  (also keywords; duplicated for convenience)
    NullptrLiteral,   // nullptr

    // ── Identifier ───────────────────────────────────────────────
    Identifier,

    // ── Keywords (alphabetical) ──────────────────────────────────
    KwAlignas,
    KwAlignof,
    KwAnd,            // operator&&
    KwAndEq,          // operator&=
    KwAsm,
    KwAuto,
    KwBitand,
    KwBitor,
    KwBool,
    KwBreak,
    KwCase,
    KwCatch,
    KwChar,
    KwChar8t,         // char8_t
    KwChar16t,        // char16_t
    KwChar32t,        // char32_t
    KwClass,
    KwCompl,          // operator~
    KwConcept,        // C++20 (parse, ignore)
    KwConst,
    KwConsteval,      // C++20
    KwConstexpr,
    KwConstinit,      // C++20
    KwContinue,
    KwCoReturn,       // co_return
    KwCoAwait,        // co_await
    KwCoYield,        // co_yield
    KwDecltype,
    KwDefault,
    KwDelete,
    KwDo,
    KwDouble,
    KwDynamicCast,
    KwElse,
    KwEnum,
    KwExplicit,
    KwExport,
    KwExtern,
    KwFalse,
    KwFloat,
    KwFor,
    KwFriend,
    KwGoto,
    KwIf,
    KwInline,
    KwInt,
    KwLong,
    KwMutable,
    KwNamespace,
    KwNew,
    KwNoexcept,
    KwNot,            // operator!
    KwNotEq,          // operator!=
    KwNullptr,
    KwOperator,
    KwOr,             // operator||
    KwOrEq,           // operator|=
    KwOverride,       // context-sensitive keyword
    KwFinal,          // context-sensitive keyword
    KwPrivate,
    KwProtected,
    KwPublic,
    KwRegister,
    KwReinterpretCast,
    KwConstCast,
    KwRequires,       // C++20
    KwReturn,
    KwShort,
    KwSigned,
    KwSizeof,
    KwStatic,
    KwStaticAssert,
    KwStaticCast,
    KwStruct,
    KwSwitch,
    KwTemplate,
    KwThis,
    KwThreadLocal,
    KwThrow,
    KwTrue,
    KwTry,
    KwTypedef,
    KwTypeid,
    KwTypename,
    KwUnion,
    KwUnsigned,
    KwUsing,
    KwVirtual,
    KwVoid,
    KwVolatile,
    KwWCharT,
    KwWhile,
    KwXor,            // operator^
    KwXorEq,          // operator^=

    // ── Punctuation ──────────────────────────────────────────────
    LParen,           // (
    RParen,           // )
    LBrace,           // {
    RBrace,           // }
    LBracket,         // [
    RBracket,         // ]
    Semicolon,        // ;
    Colon,            // :
    DoubleColon,      // ::
    Comma,            // ,
    Dot,              // .
    DotStar,          // .*
    Ellipsis,         // ...
    Question,         // ?
    Hash,             // #  (inside preprocessor lines)
    HashHash,         // ##
    At,               // @  (extension)

    // ── Operators ────────────────────────────────────────────────
    // Arithmetic
    Plus,             // +
    Minus,            // -
    Star,             // *
    Slash,            // /
    Percent,          // %
    // Bitwise
    Amp,              // &
    Pipe,             // |
    Caret,            // ^
    Tilde,            // ~
    LShift,           // <<
    RShift,           // >>
    // Comparison
    Eq,               // ==
    NotEq,            // !=
    Lt,               // <
    Gt,               // >
    LtEq,             // <=
    GtEq,             // >=
    Spaceship,        // <=>
    // Logical
    AmpAmp,           // &&
    PipePipe,         // ||
    Bang,             // !
    // Member access / pointer
    Arrow,            // ->
    ArrowStar,        // ->*
    // Assignment
    Assign,           // =
    PlusAssign,       // +=
    MinusAssign,      // -=
    StarAssign,       // *=
    SlashAssign,      // /=
    PercentAssign,    // %=
    AmpAssign,        // &=
    PipeAssign,       // |=
    CaretAssign,      // ^=
    LShiftAssign,     // <<=
    RShiftAssign,     // >>=
    // Increment / Decrement
    PlusPlus,         // ++
    MinusMinus,       // --

    // ── Preprocessor directive ───────────────────────────────────
    Preprocessor,     // # include / # define / ...  (entire line as value)

    // ── Special ──────────────────────────────────────────────────
    Eof,
    Error             // lexer error; value contains description
};

// ─────────────────────────────────────────────────────────────────
// SourceLocation  –  file + line + column
// ─────────────────────────────────────────────────────────────────
struct SourceLocation {
    std::string file;
    int line   = 1;
    int column = 1;

    std::string str() const {
        return file + ":" + std::to_string(line) + ":" + std::to_string(column);
    }
};

// ─────────────────────────────────────────────────────────────────
// Token
// ─────────────────────────────────────────────────────────────────
struct Token {
    TokenKind      kind  = TokenKind::Error;
    std::string    value;           // raw source text of the token
    SourceLocation loc;

    // ── Numeric caches (filled by the lexer) ─────────────────────
    uint64_t int_val       = 0;     // integer literal value
    double   float_val     = 0.0;   // float literal value
    bool     is_unsigned   = false; // u / U suffix
    bool     is_long       = false; // l / L suffix
    bool     is_long_long  = false; // ll / LL suffix
    bool     is_float_sfx  = false; // f / F suffix (float, not double)

    // ── Constructors ─────────────────────────────────────────────
    Token() = default;

    Token(TokenKind k, std::string v, SourceLocation l)
        : kind(k), value(std::move(v)), loc(std::move(l)) {}

    // ── Convenience predicates ───────────────────────────────────
    bool is(TokenKind k) const { return kind == k; }
    bool isNot(TokenKind k) const { return kind != k; }
    bool isEof()  const { return kind == TokenKind::Eof; }
    bool isError() const { return kind == TokenKind::Error; }

    bool isKeyword()    const;
    bool isLiteral()    const;
    bool isAssignOp()   const;
    bool isBinaryOp()   const;
    bool isUnaryOp()    const;
    bool isTypeName()   const;   // one of the built-in type keywords
    bool isAccessSpec() const;   // public/protected/private

    // Human-readable name (for diagnostics)
    std::string kindName() const;
};

// ─────────────────────────────────────────────────────────────────
// Token member implementations  (inline, header-only)
// ─────────────────────────────────────────────────────────────────

inline bool Token::isKeyword() const {
    return kind >= TokenKind::KwAlignas && kind <= TokenKind::KwXorEq;
}

inline bool Token::isLiteral() const {
    switch (kind) {
        case TokenKind::IntLiteral:
        case TokenKind::FloatLiteral:
        case TokenKind::StringLiteral:
        case TokenKind::CharLiteral:
        case TokenKind::BoolLiteral:
        case TokenKind::NullptrLiteral:
            return true;
        default:
            return false;
    }
}

inline bool Token::isAssignOp() const {
    switch (kind) {
        case TokenKind::Assign:
        case TokenKind::PlusAssign:   case TokenKind::MinusAssign:
        case TokenKind::StarAssign:   case TokenKind::SlashAssign:
        case TokenKind::PercentAssign:
        case TokenKind::AmpAssign:    case TokenKind::PipeAssign:
        case TokenKind::CaretAssign:
        case TokenKind::LShiftAssign: case TokenKind::RShiftAssign:
            return true;
        default:
            return false;
    }
}

inline bool Token::isBinaryOp() const {
    switch (kind) {
        case TokenKind::Plus: case TokenKind::Minus:
        case TokenKind::Star: case TokenKind::Slash: case TokenKind::Percent:
        case TokenKind::Amp:  case TokenKind::Pipe:  case TokenKind::Caret:
        case TokenKind::LShift: case TokenKind::RShift:
        case TokenKind::Eq:   case TokenKind::NotEq:
        case TokenKind::Lt:   case TokenKind::Gt:
        case TokenKind::LtEq: case TokenKind::GtEq: case TokenKind::Spaceship:
        case TokenKind::AmpAmp: case TokenKind::PipePipe:
        case TokenKind::Dot: case TokenKind::Arrow:
        case TokenKind::DoubleColon:
            return true;
        default:
            return isAssignOp();
    }
}

inline bool Token::isUnaryOp() const {
    switch (kind) {
        case TokenKind::Plus: case TokenKind::Minus:
        case TokenKind::Bang: case TokenKind::Tilde:
        case TokenKind::Star: case TokenKind::Amp:
        case TokenKind::PlusPlus: case TokenKind::MinusMinus:
            return true;
        default:
            return false;
    }
}

inline bool Token::isTypeName() const {
    switch (kind) {
        case TokenKind::KwVoid:  case TokenKind::KwBool:
        case TokenKind::KwChar:  case TokenKind::KwWCharT:
        case TokenKind::KwChar8t: case TokenKind::KwChar16t: case TokenKind::KwChar32t:
        case TokenKind::KwShort: case TokenKind::KwInt:
        case TokenKind::KwLong:  case TokenKind::KwFloat:    case TokenKind::KwDouble:
        case TokenKind::KwSigned: case TokenKind::KwUnsigned:
        case TokenKind::KwAuto:
            return true;
        default:
            return false;
    }
}

inline bool Token::isAccessSpec() const {
    return kind == TokenKind::KwPublic ||
           kind == TokenKind::KwProtected ||
           kind == TokenKind::KwPrivate;
}

inline std::string Token::kindName() const {
    switch (kind) {
        case TokenKind::IntLiteral:     return "integer-literal";
        case TokenKind::FloatLiteral:   return "float-literal";
        case TokenKind::StringLiteral:  return "string-literal";
        case TokenKind::CharLiteral:    return "char-literal";
        case TokenKind::BoolLiteral:    return "bool-literal";
        case TokenKind::NullptrLiteral: return "nullptr";
        case TokenKind::Identifier:     return "identifier";
        case TokenKind::LParen:         return "(";
        case TokenKind::RParen:         return ")";
        case TokenKind::LBrace:         return "{";
        case TokenKind::RBrace:         return "}";
        case TokenKind::LBracket:       return "[";
        case TokenKind::RBracket:       return "]";
        case TokenKind::Semicolon:      return ";";
        case TokenKind::Colon:          return ":";
        case TokenKind::DoubleColon:    return "::";
        case TokenKind::Comma:          return ",";
        case TokenKind::Dot:            return ".";
        case TokenKind::Arrow:          return "->";
        case TokenKind::Ellipsis:       return "...";
        case TokenKind::Question:       return "?";
        case TokenKind::Plus:           return "+";
        case TokenKind::Minus:          return "-";
        case TokenKind::Star:           return "*";
        case TokenKind::Slash:          return "/";
        case TokenKind::Percent:        return "%";
        case TokenKind::Amp:            return "&";
        case TokenKind::Pipe:           return "|";
        case TokenKind::Caret:          return "^";
        case TokenKind::Tilde:          return "~";
        case TokenKind::LShift:         return "<<";
        case TokenKind::RShift:         return ">>";
        case TokenKind::Eq:             return "==";
        case TokenKind::NotEq:          return "!=";
        case TokenKind::Lt:             return "<";
        case TokenKind::Gt:             return ">";
        case TokenKind::LtEq:           return "<=";
        case TokenKind::GtEq:           return ">=";
        case TokenKind::Spaceship:      return "<=>";
        case TokenKind::AmpAmp:         return "&&";
        case TokenKind::PipePipe:       return "||";
        case TokenKind::Bang:           return "!";
        case TokenKind::Assign:         return "=";
        case TokenKind::PlusAssign:     return "+=";
        case TokenKind::MinusAssign:    return "-=";
        case TokenKind::StarAssign:     return "*=";
        case TokenKind::SlashAssign:    return "/=";
        case TokenKind::PercentAssign:  return "%=";
        case TokenKind::AmpAssign:      return "&=";
        case TokenKind::PipeAssign:     return "|=";
        case TokenKind::CaretAssign:    return "^=";
        case TokenKind::LShiftAssign:   return "<<=";
        case TokenKind::RShiftAssign:   return ">>=";
        case TokenKind::PlusPlus:       return "++";
        case TokenKind::MinusMinus:     return "--";
        case TokenKind::Preprocessor:   return "preprocessor-directive";
        case TokenKind::Eof:            return "<EOF>";
        case TokenKind::Error:          return "<ERROR>";
        default:
            return value.empty() ? "keyword" : value;
    }
}

} // namespace rexc
