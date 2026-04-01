#pragma once
/*
 * lexer.hpp  –  REXC C++ Lexer
 *
 * Tokenises a complete C++ source file into a flat vector<Token>.
 *
 * Handles:
 *   • All C++17 keywords (plus common C++20 context keywords)
 *   • Identifiers (ASCII + common Unicode continuation bytes)
 *   • Integer literals  – decimal, 0x hex, 0b binary, 0 octal,
 *                         suffixes u/U, l/L, ll/LL, and combinations
 *   • Float literals    – decimal, optional exponent (e/E), f/F suffix
 *   • String literals   – regular, L/u/U/u8 prefixed, raw R"delim(…)delim"
 *   • Char literals     – 'x', '\n', '\xFF', L'x', …
 *   • All operators including <<=, >>=, <=>, ->*, .*
 *   • // and slash-star comments (skipped, location preserved)
 *   • Preprocessor lines – tokenised as a single Preprocessor token
 *     carrying the entire source line as its value
 */

#include "token.hpp"
#include <vector>
#include <string>
#include <unordered_map>
#include <stdexcept>
#include <cassert>
#include <cstring>
#include <cerrno>

namespace rexc {

// ─────────────────────────────────────────────────────────────────
//  Keyword table  (initialised once via static local)
// ─────────────────────────────────────────────────────────────────
inline const std::unordered_map<std::string, TokenKind>& keyword_table() {
    static const std::unordered_map<std::string, TokenKind> kw = {
        {"alignas",          TokenKind::KwAlignas},
        {"alignof",          TokenKind::KwAlignof},
        {"and",              TokenKind::KwAnd},
        {"and_eq",           TokenKind::KwAndEq},
        {"asm",              TokenKind::KwAsm},
        {"auto",             TokenKind::KwAuto},
        {"bitand",           TokenKind::KwBitand},
        {"bitor",            TokenKind::KwBitor},
        {"bool",             TokenKind::KwBool},
        {"break",            TokenKind::KwBreak},
        {"case",             TokenKind::KwCase},
        {"catch",            TokenKind::KwCatch},
        {"char",             TokenKind::KwChar},
        {"char8_t",          TokenKind::KwChar8t},
        {"char16_t",         TokenKind::KwChar16t},
        {"char32_t",         TokenKind::KwChar32t},
        {"class",            TokenKind::KwClass},
        {"compl",            TokenKind::KwCompl},
        {"concept",          TokenKind::KwConcept},
        {"const",            TokenKind::KwConst},
        {"consteval",        TokenKind::KwConsteval},
        {"constexpr",        TokenKind::KwConstexpr},
        {"constinit",        TokenKind::KwConstinit},
        {"continue",         TokenKind::KwContinue},
        {"co_return",        TokenKind::KwCoReturn},
        {"co_await",         TokenKind::KwCoAwait},
        {"co_yield",         TokenKind::KwCoYield},
        {"decltype",         TokenKind::KwDecltype},
        {"default",          TokenKind::KwDefault},
        {"delete",           TokenKind::KwDelete},
        {"do",               TokenKind::KwDo},
        {"double",           TokenKind::KwDouble},
        {"dynamic_cast",     TokenKind::KwDynamicCast},
        {"else",             TokenKind::KwElse},
        {"enum",             TokenKind::KwEnum},
        {"explicit",         TokenKind::KwExplicit},
        {"export",           TokenKind::KwExport},
        {"extern",           TokenKind::KwExtern},
        {"false",            TokenKind::KwFalse},
        {"final",            TokenKind::KwFinal},
        {"float",            TokenKind::KwFloat},
        {"for",              TokenKind::KwFor},
        {"friend",           TokenKind::KwFriend},
        {"goto",             TokenKind::KwGoto},
        {"if",               TokenKind::KwIf},
        {"inline",           TokenKind::KwInline},
        {"int",              TokenKind::KwInt},
        {"long",             TokenKind::KwLong},
        {"mutable",          TokenKind::KwMutable},
        {"namespace",        TokenKind::KwNamespace},
        {"new",              TokenKind::KwNew},
        {"noexcept",         TokenKind::KwNoexcept},
        {"not",              TokenKind::KwNot},
        {"not_eq",           TokenKind::KwNotEq},
        {"nullptr",          TokenKind::KwNullptr},
        {"operator",         TokenKind::KwOperator},
        {"or",               TokenKind::KwOr},
        {"or_eq",            TokenKind::KwOrEq},
        {"override",         TokenKind::KwOverride},
        {"private",          TokenKind::KwPrivate},
        {"protected",        TokenKind::KwProtected},
        {"public",           TokenKind::KwPublic},
        {"register",         TokenKind::KwRegister},
        {"reinterpret_cast", TokenKind::KwReinterpretCast},
        {"const_cast",       TokenKind::KwConstCast},
        {"requires",         TokenKind::KwRequires},
        {"return",           TokenKind::KwReturn},
        {"short",            TokenKind::KwShort},
        {"signed",           TokenKind::KwSigned},
        {"sizeof",           TokenKind::KwSizeof},
        {"static",           TokenKind::KwStatic},
        {"static_assert",    TokenKind::KwStaticAssert},
        {"static_cast",      TokenKind::KwStaticCast},
        {"struct",           TokenKind::KwStruct},
        {"switch",           TokenKind::KwSwitch},
        {"template",         TokenKind::KwTemplate},
        {"this",             TokenKind::KwThis},
        {"thread_local",     TokenKind::KwThreadLocal},
        {"throw",            TokenKind::KwThrow},
        {"true",             TokenKind::KwTrue},
        {"try",              TokenKind::KwTry},
        {"typedef",          TokenKind::KwTypedef},
        {"typeid",           TokenKind::KwTypeid},
        {"typename",         TokenKind::KwTypename},
        {"union",            TokenKind::KwUnion},
        {"unsigned",         TokenKind::KwUnsigned},
        {"using",            TokenKind::KwUsing},
        {"virtual",          TokenKind::KwVirtual},
        {"void",             TokenKind::KwVoid},
        {"volatile",         TokenKind::KwVolatile},
        {"wchar_t",          TokenKind::KwWCharT},
        {"while",            TokenKind::KwWhile},
        {"xor",              TokenKind::KwXor},
        {"xor_eq",           TokenKind::KwXorEq},
    };
    return kw;
}

// ─────────────────────────────────────────────────────────────────
//  Lexer
// ─────────────────────────────────────────────────────────────────
class Lexer {
public:
    explicit Lexer(std::string source, std::string filename = "<input>")
        : src_(std::move(source)), filename_(std::move(filename)) {}

    // Tokenise the entire input; never throws (errors stored as Error tokens)
    std::vector<Token> tokenize() {
        std::vector<Token> tokens;
        tokens.reserve(src_.size() / 4);  // rough estimate

        while (pos_ < src_.size()) {
            skip_whitespace_and_newlines();
            if (pos_ >= src_.size()) break;

            // ── Preprocessor directive ──────────────────────────
            if (cur() == '#' && at_line_start_) {
                tokens.push_back(read_preprocessor());
                continue;
            }

            // ── Line comment ────────────────────────────────────
            if (cur() == '/' && peek(1) == '/') {
                skip_line_comment();
                continue;
            }

            // ── Block comment ───────────────────────────────────
            if (cur() == '/' && peek(1) == '*') {
                skip_block_comment();
                continue;
            }

            tokens.push_back(read_token());
        }

        tokens.emplace_back(TokenKind::Eof, "", make_loc());
        return tokens;
    }

    // ── Error list ───────────────────────────────────────────────
    const std::vector<std::string>& errors() const { return errors_; }

private:
    std::string           src_;
    std::string           filename_;
    size_t                pos_           = 0;
    int                   line_          = 1;
    int                   column_        = 1;
    bool                  at_line_start_ = true;
    std::vector<std::string> errors_;

    // ── Source navigation ────────────────────────────────────────
    char cur()  const { return pos_ < src_.size() ? src_[pos_] : '\0'; }
    char peek(size_t offset = 1) const {
        return (pos_ + offset < src_.size()) ? src_[pos_ + offset] : '\0';
    }

    char advance() {
        char c = src_[pos_++];
        if (c == '\n') { ++line_; column_ = 1; at_line_start_ = true; }
        else           { ++column_; at_line_start_ = false; }
        return c;
    }

    bool match(char expected) {
        if (pos_ < src_.size() && src_[pos_] == expected) {
            advance(); return true;
        }
        return false;
    }

    SourceLocation make_loc() const { return { filename_, line_, column_ }; }

    // ── Skip helpers ─────────────────────────────────────────────
    void skip_whitespace_and_newlines() {
        while (pos_ < src_.size()) {
            char c = cur();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                advance();
            } else if (c == '\n') {
                advance();   // sets at_line_start_ = true
            } else {
                break;
            }
        }
    }

    void skip_line_comment() {
        while (pos_ < src_.size() && cur() != '\n') advance();
    }

    void skip_block_comment() {
        advance(); advance();   // consume /*
        while (pos_ < src_.size()) {
            if (cur() == '*' && peek(1) == '/') { advance(); advance(); return; }
            advance();
        }
        errors_.push_back(make_loc().str() + ": unterminated block comment");
    }

    // ── Preprocessor ────────────────────────────────────────────
    Token read_preprocessor() {
        SourceLocation loc = make_loc();
        std::string line;
        // Consume until end-of-logical-line (handle backslash continuation)
        while (pos_ < src_.size() && cur() != '\n') {
            if (cur() == '\\' && peek(1) == '\n') {
                line += advance();  // '\'
                line += advance();  // '\n'
            } else {
                line += advance();
            }
        }
        return Token(TokenKind::Preprocessor, line, loc);
    }

    // ── Main token dispatcher ────────────────────────────────────
    Token read_token() {
        SourceLocation loc = make_loc();
        char c = cur();

        // ── String / char prefix: u8"…" u"…" U"…" L"…" R"…" ──
        if ((c == 'u' || c == 'U' || c == 'L' || c == 'R') &&
            (peek(1) == '"' || peek(1) == '\'' ||
             (c != 'R' && peek(1) == '8' && peek(2) == '"'))) {
            return read_string_or_char_with_prefix(loc);
        }

        // ── Identifier / keyword ─────────────────────────────────
        if (is_ident_start(c)) return read_identifier(loc);

        // ── Numeric literal ──────────────────────────────────────
        if (std::isdigit(c) || (c == '.' && std::isdigit(peek(1))))
            return read_number(loc);

        // ── String literal ───────────────────────────────────────
        if (c == '"') return read_string(loc);

        // ── Char literal ─────────────────────────────────────────
        if (c == '\'') return read_char(loc);

        // ── Operators & punctuation ──────────────────────────────
        return read_operator(loc);
    }

    // ── Identifier / keyword reader ──────────────────────────────
    Token read_identifier(SourceLocation loc) {
        std::string text;
        while (pos_ < src_.size() && is_ident_cont(cur())) text += advance();

        const auto& kw = keyword_table();
        auto it = kw.find(text);
        if (it != kw.end()) {
            TokenKind k = it->second;
            // Produce specialised token kinds for literal keywords
            if (k == TokenKind::KwTrue || k == TokenKind::KwFalse) {
                Token t(TokenKind::BoolLiteral, text, loc);
                t.int_val = (k == TokenKind::KwTrue) ? 1 : 0;
                return t;
            }
            if (k == TokenKind::KwNullptr) {
                return Token(TokenKind::NullptrLiteral, text, loc);
            }
            return Token(k, text, loc);
        }
        return Token(TokenKind::Identifier, text, loc);
    }

    // ── Numeric literal reader ───────────────────────────────────
    Token read_number(SourceLocation loc) {
        std::string raw;
        bool is_float = false;

        // Hex prefix
        if (cur() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
            raw += advance(); raw += advance();
            while (pos_ < src_.size() && (std::isxdigit(cur()) || cur() == '\''))
                raw += advance();
            // Hex float (0x1.fp10)
            if (cur() == '.') { raw += advance(); is_float = true;
                while (std::isxdigit(cur()) || cur() == '\'') raw += advance();
            }
            if (cur() == 'p' || cur() == 'P') { raw += advance(); is_float = true;
                if (cur() == '+' || cur() == '-') raw += advance();
                while (std::isdigit(cur())) raw += advance();
            }
        }
        // Binary prefix
        else if (cur() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
            raw += advance(); raw += advance();
            while (cur() == '0' || cur() == '1' || cur() == '\'') raw += advance();
        }
        // Octal or decimal
        else {
            while (std::isdigit(cur()) || cur() == '\'') raw += advance();
            if (cur() == '.') {
                raw += advance(); is_float = true;
                while (std::isdigit(cur()) || cur() == '\'') raw += advance();
            }
            if (cur() == 'e' || cur() == 'E') {
                raw += advance(); is_float = true;
                if (cur() == '+' || cur() == '-') raw += advance();
                while (std::isdigit(cur())) raw += advance();
            }
        }

        Token t(is_float ? TokenKind::FloatLiteral : TokenKind::IntLiteral, raw, loc);

        // Suffixes
        std::string suffix;
        while (pos_ < src_.size() &&
               (cur() == 'u' || cur() == 'U' || cur() == 'l' || cur() == 'L' ||
                cur() == 'f' || cur() == 'F')) {
            suffix += std::tolower(cur());
            advance();
        }
        t.value = raw + suffix;
        t.is_unsigned   = (suffix.find('u') != std::string::npos);
        t.is_long_long  = (suffix.find("ll") != std::string::npos);
        t.is_long       = !t.is_long_long && (suffix.find('l') != std::string::npos);
        t.is_float_sfx  = (suffix.find('f') != std::string::npos);

        // Parse the numeric value (best effort)
        if (is_float) {
            // Remove digit separators before parsing
            std::string clean; for (char ch : raw) if (ch != '\'') clean += ch;
            try { t.float_val = std::stod(clean); } catch (...) {}
        } else {
            std::string clean; for (char ch : raw) if (ch != '\'') clean += ch;
            try { t.int_val = std::stoull(clean, nullptr, 0); } catch (...) {}
        }

        return t;
    }

    // ── String literal reader (no prefix) ───────────────────────
    Token read_string(SourceLocation loc) {
        std::string raw;
        raw += advance();  // opening "
        while (pos_ < src_.size() && cur() != '"') {
            if (cur() == '\\') { raw += advance(); if (pos_ < src_.size()) raw += advance(); }
            else                raw += advance();
        }
        if (cur() == '"') raw += advance();
        else errors_.push_back(loc.str() + ": unterminated string literal");
        return Token(TokenKind::StringLiteral, raw, loc);
    }

    // ── Raw string reader: R"delim(…)delim" ─────────────────────
    Token read_raw_string(SourceLocation loc, const std::string& prefix) {
        // We've already consumed the R" part; find the delimiter
        std::string delim;
        while (pos_ < src_.size() && cur() != '(') {
            if (cur() == ')' || cur() == '\\' || cur() == '\n') break;
            delim += advance();
        }
        if (cur() != '(') {
            errors_.push_back(loc.str() + ": malformed raw string literal");
            return Token(TokenKind::Error, prefix + "R\"" + delim, loc);
        }
        advance();  // consume '('

        std::string end = ")" + delim + "\"";
        std::string body;
        while (pos_ < src_.size()) {
            if (src_.substr(pos_, end.size()) == end) {
                pos_ += end.size(); column_ += (int)end.size();
                break;
            }
            body += advance();
        }
        return Token(TokenKind::StringLiteral,
                     prefix + "R\"" + delim + "(" + body + ")" + delim + "\"", loc);
    }

    // ── Char literal reader ──────────────────────────────────────
    Token read_char(SourceLocation loc) {
        std::string raw;
        raw += advance();  // opening '
        while (pos_ < src_.size() && cur() != '\'') {
            if (cur() == '\\') { raw += advance(); if (pos_ < src_.size()) raw += advance(); }
            else if (cur() == '\n') break;
            else raw += advance();
        }
        if (cur() == '\'') raw += advance();
        else errors_.push_back(loc.str() + ": unterminated char literal");
        return Token(TokenKind::CharLiteral, raw, loc);
    }

    // ── Prefixed string/char (u8"", L"", R"", u'', U'', ...) ────
    Token read_string_or_char_with_prefix(SourceLocation loc) {
        std::string prefix;
        // Collect prefix characters
        while (cur() == 'u' || cur() == 'U' || cur() == 'L' || cur() == 'R' || cur() == '8')
            prefix += advance();

        bool is_raw = (prefix.find('R') != std::string::npos);
        if (cur() == '"') {
            if (is_raw) {
                advance();  // consume opening "
                return read_raw_string(loc, prefix);
            }
            // Regular prefixed string
            Token t = read_string(loc);
            t.value = prefix + t.value;
            return t;
        }
        if (cur() == '\'') {
            Token t = read_char(loc);
            t.value = prefix + t.value;
            return t;
        }
        // Not actually a string/char – treat prefix as identifier
        const auto& kw = keyword_table();
        auto it = kw.find(prefix);
        if (it != kw.end()) return Token(it->second, prefix, loc);
        return Token(TokenKind::Identifier, prefix, loc);
    }

    // ── Operator reader ──────────────────────────────────────────
    Token read_operator(SourceLocation loc) {
        char c = advance();
        switch (c) {
            case '(': return Token(TokenKind::LParen,     "(", loc);
            case ')': return Token(TokenKind::RParen,     ")", loc);
            case '{': return Token(TokenKind::LBrace,     "{", loc);
            case '}': return Token(TokenKind::RBrace,     "}", loc);
            case '[': return Token(TokenKind::LBracket,   "[", loc);
            case ']': return Token(TokenKind::RBracket,   "]", loc);
            case ';': return Token(TokenKind::Semicolon,  ";", loc);
            case ',': return Token(TokenKind::Comma,      ",", loc);
            case '?': return Token(TokenKind::Question,   "?", loc);
            case '~': return Token(TokenKind::Tilde,      "~", loc);
            case '@': return Token(TokenKind::At,         "@", loc);

            case ':':
                if (match(':')) return Token(TokenKind::DoubleColon, "::", loc);
                return Token(TokenKind::Colon, ":", loc);

            case '.':
                if (cur() == '*') { advance(); return Token(TokenKind::DotStar,  ".*",  loc); }
                if (cur() == '.' && peek(1) == '.') { advance(); advance();
                    return Token(TokenKind::Ellipsis, "...", loc); }
                return Token(TokenKind::Dot, ".", loc);

            case '+':
                if (match('+')) return Token(TokenKind::PlusPlus,   "++", loc);
                if (match('=')) return Token(TokenKind::PlusAssign,  "+=", loc);
                return Token(TokenKind::Plus,  "+", loc);

            case '-':
                if (match('-')) return Token(TokenKind::MinusMinus,  "--", loc);
                if (cur() == '>' && peek(1) == '*') { advance(); advance();
                    return Token(TokenKind::ArrowStar, "->*", loc); }
                if (match('>')) return Token(TokenKind::Arrow,       "->", loc);
                if (match('=')) return Token(TokenKind::MinusAssign, "-=", loc);
                return Token(TokenKind::Minus, "-", loc);

            case '*':
                if (match('=')) return Token(TokenKind::StarAssign,  "*=", loc);
                return Token(TokenKind::Star, "*", loc);

            case '/':
                if (match('=')) return Token(TokenKind::SlashAssign, "/=", loc);
                return Token(TokenKind::Slash, "/", loc);

            case '%':
                if (match('=')) return Token(TokenKind::PercentAssign, "%=", loc);
                return Token(TokenKind::Percent, "%", loc);

            case '&':
                if (match('&')) return Token(TokenKind::AmpAmp,     "&&", loc);
                if (match('=')) return Token(TokenKind::AmpAssign,   "&=", loc);
                return Token(TokenKind::Amp, "&", loc);

            case '|':
                if (match('|')) return Token(TokenKind::PipePipe,   "||", loc);
                if (match('=')) return Token(TokenKind::PipeAssign,  "|=", loc);
                return Token(TokenKind::Pipe, "|", loc);

            case '^':
                if (match('=')) return Token(TokenKind::CaretAssign, "^=", loc);
                return Token(TokenKind::Caret, "^", loc);

            case '!':
                if (match('=')) return Token(TokenKind::NotEq, "!=", loc);
                return Token(TokenKind::Bang, "!", loc);

            case '=':
                if (match('=')) return Token(TokenKind::Eq,     "==", loc);
                return Token(TokenKind::Assign, "=", loc);

            case '<':
                if (cur() == '<') {
                    advance();
                    if (match('=')) return Token(TokenKind::LShiftAssign, "<<=", loc);
                    return Token(TokenKind::LShift, "<<", loc);
                }
                if (cur() == '=' && peek(1) == '>') {
                    advance(); advance();
                    return Token(TokenKind::Spaceship, "<=>", loc);
                }
                if (match('=')) return Token(TokenKind::LtEq, "<=", loc);
                return Token(TokenKind::Lt, "<", loc);

            case '>':
                if (cur() == '>') {
                    advance();
                    if (match('=')) return Token(TokenKind::RShiftAssign, ">>=", loc);
                    return Token(TokenKind::RShift, ">>", loc);
                }
                if (match('=')) return Token(TokenKind::GtEq, ">=", loc);
                return Token(TokenKind::Gt, ">", loc);

            case '#':
                if (match('#')) return Token(TokenKind::HashHash, "##", loc);
                return Token(TokenKind::Hash, "#", loc);

            default: {
                std::string msg = loc.str() + ": unexpected character '";
                msg += c; msg += '\'';
                errors_.push_back(msg);
                return Token(TokenKind::Error, std::string(1, c), loc);
            }
        }
    }

    // ── Character classification ─────────────────────────────────
    static bool is_ident_start(char c) {
        return std::isalpha(c) || c == '_' || (unsigned char)c > 127;
    }
    static bool is_ident_cont(char c) {
        return std::isalnum(c) || c == '_' || (unsigned char)c > 127;
    }
};

// ─────────────────────────────────────────────────────────────────
//  Free-function convenience wrapper
// ─────────────────────────────────────────────────────────────────
inline std::vector<Token> tokenize(const std::string& source,
                                   const std::string& filename = "<input>") {
    Lexer lex(source, filename);
    return lex.tokenize();
}

} // namespace rexc
