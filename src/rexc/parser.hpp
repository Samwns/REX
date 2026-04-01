#pragma once
/*
 * parser.hpp  –  REXC Recursive-Descent C++ Parser
 *
 * Builds an AST from the token stream produced by lexer.hpp.
 *
 * The parser is deliberately lenient: when it encounters constructs
 * it does not fully understand it skips forward to the next safe
 * synchronisation point (next ';' or '}') and records a diagnostic,
 * rather than aborting compilation.
 *
 * Operator precedences follow the standard C++ table.
 */

#include "token.hpp"
#include "lexer.hpp"
#include "ast.hpp"

#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <stdexcept>
#include <cassert>
#include <algorithm>

namespace rexc {

// ─────────────────────────────────────────────────────────────────
//  Diagnostic
// ─────────────────────────────────────────────────────────────────
struct Diagnostic {
    enum class Severity { Note, Warning, Error };
    Severity    severity;
    std::string message;
    SourceLocation loc;
    std::string str() const {
        std::string s = loc.str() + ": ";
        if (severity == Severity::Error)   s += "error: ";
        if (severity == Severity::Warning) s += "warning: ";
        return s + message;
    }
};

// ─────────────────────────────────────────────────────────────────
//  Parser
// ─────────────────────────────────────────────────────────────────
class Parser {
public:
    explicit Parser(std::vector<Token> tokens)
        : tokens_(std::move(tokens)), pos_(0) {}

    // Parse the entire translation unit
    std::unique_ptr<TranslationUnit> parse() {
        auto tu = std::make_unique<TranslationUnit>(cur().loc);
        while (!at_eof()) {
            if (auto d = parse_top_level_decl()) {
                tu->decls.push_back(std::move(d));
            } else {
                // recovery: skip a token and keep going
                if (!at_eof()) advance();
            }
        }
        return tu;
    }

    const std::vector<Diagnostic>& diagnostics() const { return diags_; }
    bool has_errors() const {
        return std::any_of(diags_.begin(), diags_.end(),
            [](const Diagnostic& d){ return d.severity == Diagnostic::Severity::Error; });
    }

private:
    std::vector<Token> tokens_;
    size_t             pos_;
    std::vector<Diagnostic> diags_;

    // ── Token navigation ─────────────────────────────────────────
    const Token& cur()  const { return tokens_[pos_]; }
    const Token& peek(size_t off = 1) const {
        size_t idx = pos_ + off;
        return (idx < tokens_.size()) ? tokens_[idx] : tokens_.back();
    }
    bool at_eof() const { return cur().kind == TokenKind::Eof; }

    Token advance() {
        Token t = tokens_[pos_];
        if (pos_ + 1 < tokens_.size()) pos_++;
        return t;
    }

    bool match(TokenKind k) {
        if (cur().kind == k) { advance(); return true; }
        return false;
    }

    // Consume if matches, otherwise record error and return false
    bool expect(TokenKind k, const std::string& ctx = "") {
        if (cur().kind == k) { advance(); return true; }
        std::string msg = "expected '" + Token(k,"",{}).kindName() + "'";
        if (!ctx.empty()) msg += " " + ctx;
        msg += ", got '" + cur().kindName() + "'";
        error(msg);
        return false;
    }

    bool check(TokenKind k) const { return cur().kind == k; }

    void error(const std::string& msg) {
        diags_.push_back({Diagnostic::Severity::Error, msg, cur().loc});
    }
    void warning(const std::string& msg) {
        diags_.push_back({Diagnostic::Severity::Warning, msg, cur().loc});
    }

    // Skip to next ';' or '}' for error recovery
    void synchronize() {
        while (!at_eof()) {
            if (cur().kind == TokenKind::Semicolon) { advance(); return; }
            if (cur().kind == TokenKind::RBrace)    { return; }
            advance();
        }
    }

    SourceLocation loc() const { return cur().loc; }

    // ── Type parsing helpers ─────────────────────────────────────

    // Returns true if the current token can start a type specifier
    bool is_type_start() const {
        auto k = cur().kind;
        if (cur().isTypeName()) return true;
        switch (k) {
            case TokenKind::KwConst:     case TokenKind::KwVolatile:
            case TokenKind::KwSigned:    case TokenKind::KwUnsigned:
            case TokenKind::KwStruct:    case TokenKind::KwClass:
            case TokenKind::KwUnion:     case TokenKind::KwEnum:
            case TokenKind::KwTypename:  case TokenKind::KwDecltype:
            case TokenKind::KwConstexpr: case TokenKind::KwStatic:
            case TokenKind::KwExtern:    case TokenKind::KwInline:
            case TokenKind::KwVirtual:   case TokenKind::KwExplicit:
            case TokenKind::KwFriend:    case TokenKind::KwMutable:
                return true;
            case TokenKind::Identifier:
                // Could be a user-defined type name – check context later
                return true;
            default:
                return false;
        }
    }

    // Parse base type + qualifiers (no *, &, or declarator)
    NodePtr parse_type_specifier() {
        bool is_const    = false;
        bool is_volatile = false;
        bool is_signed   = false;
        bool is_unsigned = false;
        int  is_long_cnt = 0;
        bool is_short    = false;

        // Storage class / function specifiers (we'll record but not emit separate nodes)
        while (true) {
            if (match(TokenKind::KwConst))     { is_const    = true; continue; }
            if (match(TokenKind::KwVolatile))  { is_volatile = true; continue; }
            if (match(TokenKind::KwSigned))    { is_signed   = true; continue; }
            if (match(TokenKind::KwUnsigned))  { is_unsigned = true; continue; }
            if (match(TokenKind::KwShort))     { is_short    = true; continue; }
            if (check(TokenKind::KwLong)) {
                advance(); is_long_cnt++; continue;
            }
            // Ignore storage class & function specifiers for type purposes
            if (match(TokenKind::KwStatic))   continue;
            if (match(TokenKind::KwExtern))   continue;
            if (match(TokenKind::KwInline))   continue;
            if (match(TokenKind::KwVirtual))  continue;
            if (match(TokenKind::KwExplicit)) continue;
            if (match(TokenKind::KwFriend))   continue;
            if (match(TokenKind::KwMutable))  continue;
            if (match(TokenKind::KwConstexpr)) continue;
            if (match(TokenKind::KwConsteval)) continue;
            if (match(TokenKind::KwConstinit)) continue;
            break;
        }

        NodePtr base;
        SourceLocation tl = loc();

        // Primitive types
        if (match(TokenKind::KwVoid))   base = make_prim(PrimitiveType::Kind::Void, tl);
        else if (match(TokenKind::KwBool))   base = make_prim(PrimitiveType::Kind::Bool, tl);
        else if (match(TokenKind::KwChar))   base = make_prim(PrimitiveType::Kind::Char, tl);
        else if (match(TokenKind::KwWCharT)) base = make_prim(PrimitiveType::Kind::WChar, tl);
        else if (match(TokenKind::KwInt))    base = make_prim(prim_int(is_unsigned, is_long_cnt, is_short), tl);
        else if (match(TokenKind::KwFloat))  base = make_prim(PrimitiveType::Kind::Float, tl);
        else if (match(TokenKind::KwDouble)) base = make_prim(is_long_cnt ? PrimitiveType::Kind::LongDouble
                                                                            : PrimitiveType::Kind::Double, tl);
        else if (match(TokenKind::KwAuto))   base = make_prim(PrimitiveType::Kind::Auto, tl);
        else if (check(TokenKind::KwStruct) || check(TokenKind::KwClass) ||
                 check(TokenKind::KwUnion)) {
            advance(); // consume struct/class/union
            if (check(TokenKind::Identifier)) {
                std::string name = advance().value;
                base = std::make_unique<NamedType>(std::vector<std::string>{}, name, tl);
            } else {
                base = make_prim(PrimitiveType::Kind::Int, tl); // fallback
            }
        }
        else if (check(TokenKind::KwEnum)) {
            advance();
            if (match(TokenKind::KwClass)) {} // enum class – ignore for type
            if (check(TokenKind::Identifier)) {
                std::string name = advance().value;
                base = std::make_unique<NamedType>(std::vector<std::string>{}, name, tl);
            } else {
                base = make_prim(PrimitiveType::Kind::Int, tl);
            }
        }
        else if (check(TokenKind::KwTypename)) {
            advance();
            base = parse_named_type();
        }
        else if (check(TokenKind::Identifier) || check(TokenKind::DoubleColon)) {
            base = parse_named_type();
        }
        else if (check(TokenKind::KwDecltype)) {
            // decltype(expr) – consume and return auto
            advance();
            expect(TokenKind::LParen);
            int depth = 1;
            while (!at_eof() && depth > 0) {
                if (check(TokenKind::LParen)) depth++;
                else if (check(TokenKind::RParen)) depth--;
                if (depth > 0) advance(); else advance();
            }
            base = make_prim(PrimitiveType::Kind::Auto, tl);
        }
        else {
            // No base type found – synthesise int
            base = make_prim(PrimitiveType::Kind::Int, tl);
        }

        // Handle  long  without an explicit int
        if (!base && is_long_cnt > 0) {
            base = make_prim(prim_int(is_unsigned, is_long_cnt, false), tl);
        }

        // Wrap with const/volatile qualifier
        if (is_const || is_volatile) {
            TypeQuals q;
            q.is_const    = is_const;
            q.is_volatile = is_volatile;
            if (base) base = std::make_unique<QualifiedType>(std::move(base), q, tl);
        }

        // Trailing const (e.g., int const)
        if (check(TokenKind::KwConst)) {
            advance();
            TypeQuals q; q.is_const = true;
            if (base) base = std::make_unique<QualifiedType>(std::move(base), q, tl);
        }

        return base;
    }

    // Parse *, &, && decorators on top of base type
    NodePtr parse_type_full() {
        NodePtr base = parse_type_specifier();
        return wrap_ptr_ref(std::move(base));
    }

    NodePtr wrap_ptr_ref(NodePtr base) {
        while (true) {
            if (check(TokenKind::Star)) {
                SourceLocation l = loc(); advance();
                TypeQuals q;
                if (match(TokenKind::KwConst))    q.is_const    = true;
                if (match(TokenKind::KwVolatile))  q.is_volatile = true;
                base = std::make_unique<PointerType>(std::move(base), q, l);
            } else if (check(TokenKind::Amp) && peek().kind != TokenKind::Amp) {
                SourceLocation l = loc(); advance();
                base = std::make_unique<ReferenceType>(std::move(base), l);
            } else if (check(TokenKind::Amp) && peek().kind == TokenKind::Amp) {
                // Could be &&
                SourceLocation l = loc(); advance(); advance();
                base = std::make_unique<RValueRefType>(std::move(base), l);
            } else if (check(TokenKind::AmpAmp)) {
                SourceLocation l = loc(); advance();
                base = std::make_unique<RValueRefType>(std::move(base), l);
            } else {
                break;
            }
        }
        return base;
    }

    // Parse a named/qualified/template type like  std::vector<int>
    NodePtr parse_named_type() {
        SourceLocation tl = loc();
        std::vector<std::string> scope;
        std::string name;

        // Optional leading ::
        if (match(TokenKind::DoubleColon)) {}  // global scope – ignore

        if (!check(TokenKind::Identifier)) {
            error("expected type name");
            return make_prim(PrimitiveType::Kind::Int, tl);
        }
        name = advance().value;

        while (check(TokenKind::DoubleColon)) {
            advance();
            if (check(TokenKind::Identifier)) {
                scope.push_back(name);
                name = advance().value;
            } else {
                break;
            }
        }

        // Template arguments: vector<int>
        if (check(TokenKind::Lt)) {
            advance();  // consume <
            std::vector<NodePtr> args;
            int depth = 1;
            while (!at_eof() && depth > 0) {
                if (check(TokenKind::Lt)) { depth++; advance(); continue; }
                if (check(TokenKind::Gt)) {
                    depth--;
                    if (depth == 0) { advance(); break; }
                    advance(); continue;
                }
                if (check(TokenKind::RShift)) {
                    // >> closes two levels
                    if (depth <= 2) { depth = 0; advance(); break; }
                    depth -= 2; advance(); continue;
                }
                if (check(TokenKind::Comma) && depth == 1) {
                    advance(); continue;
                }
                // Try to parse a type argument
                if (is_type_start()) {
                    auto arg = parse_type_full();
                    if (arg) args.push_back(std::move(arg));
                } else if (!check(TokenKind::Gt) && !check(TokenKind::Comma)) {
                    auto arg = parse_assignment_expr();
                    if (arg) args.push_back(std::move(arg));
                } else {
                    advance();
                }
            }
            return std::make_unique<TemplateInstType>(std::move(scope), name, std::move(args), tl);
        }

        return std::make_unique<NamedType>(std::move(scope), name, tl);
    }

    static NodePtr make_prim(PrimitiveType::Kind k, SourceLocation l) {
        return std::make_unique<PrimitiveType>(k, l);
    }

    static PrimitiveType::Kind prim_int(bool uns, int longs, bool shrt) {
        if (uns) {
            if (longs >= 2) return PrimitiveType::Kind::ULongLong;
            if (longs == 1) return PrimitiveType::Kind::ULong;
            if (shrt)       return PrimitiveType::Kind::UShort;
            return PrimitiveType::Kind::UInt;
        }
        if (longs >= 2) return PrimitiveType::Kind::LongLong;
        if (longs == 1) return PrimitiveType::Kind::Long;
        if (shrt)       return PrimitiveType::Kind::Short;
        return PrimitiveType::Kind::Int;
    }

    // ── Top-level declaration dispatch ──────────────────────────
    NodePtr parse_top_level_decl() {
        // Preprocessor
        if (check(TokenKind::Preprocessor)) {
            Token t = advance();
            auto d = std::make_unique<PreprocessorDecl>(t.value, t.loc);
            return d;
        }
        // Namespace
        if (check(TokenKind::KwNamespace)) return parse_namespace_decl();
        // using
        if (check(TokenKind::KwUsing)) return parse_using_decl();
        // typedef
        if (check(TokenKind::KwTypedef)) return parse_typedef_decl();
        // template
        if (check(TokenKind::KwTemplate)) return parse_template_decl();
        // extern "C" linkage
        if (check(TokenKind::KwExtern)) {
            if (peek().kind == TokenKind::StringLiteral) {
                advance(); advance(); // skip extern "C"
                if (check(TokenKind::LBrace)) return parse_compound_as_decl_block();
                return parse_top_level_decl();
            }
        }
        // static_assert
        if (check(TokenKind::KwStaticAssert)) return parse_static_assert();
        // class / struct / union
        if (check(TokenKind::KwClass) || check(TokenKind::KwStruct) ||
            check(TokenKind::KwUnion)) {
            return parse_class_or_func_or_var();
        }
        // enum
        if (check(TokenKind::KwEnum)) return parse_enum_decl();
        // Everything else is a function or variable declaration
        if (is_type_start() || check(TokenKind::Tilde)) {
            return parse_func_or_var_decl(false);
        }
        return nullptr;
    }

    // Extern "C" { ... } – parse contents as declarations
    NodePtr parse_compound_as_decl_block() {
        expect(TokenKind::LBrace);
        // Return a namespace node with anonymous name
        auto ns = std::make_unique<NamespaceDecl>("", loc());
        while (!at_eof() && !check(TokenKind::RBrace)) {
            if (auto d = parse_top_level_decl()) ns->decls.push_back(std::move(d));
            else if (!at_eof()) advance();
        }
        expect(TokenKind::RBrace);
        return ns;
    }

    // ── Namespace ────────────────────────────────────────────────
    NodePtr parse_namespace_decl() {
        SourceLocation l = loc();
        advance();  // consume 'namespace'
        bool is_inline = false;
        if (check(TokenKind::KwInline)) { advance(); is_inline = true; }
        std::string name;
        if (check(TokenKind::Identifier)) name = advance().value;

        // Namespace alias: namespace A = B;
        if (check(TokenKind::Assign)) {
            advance();
            auto u = std::make_unique<UsingDecl>(l);
            u->form = UsingDecl::Form::Alias;
            u->name = name;
            if (check(TokenKind::Identifier)) u->scope_path.push_back(advance().value);
            expect(TokenKind::Semicolon);
            return u;
        }

        if (!expect(TokenKind::LBrace, "after namespace name")) {
            synchronize();
            return nullptr;
        }

        auto ns = std::make_unique<NamespaceDecl>(name, l);
        ns->is_inline = is_inline;
        while (!at_eof() && !check(TokenKind::RBrace)) {
            if (auto d = parse_top_level_decl()) ns->decls.push_back(std::move(d));
            else if (!at_eof() && !check(TokenKind::RBrace)) advance();
        }
        expect(TokenKind::RBrace);
        return ns;
    }

    // ── using ────────────────────────────────────────────────────
    NodePtr parse_using_decl() {
        SourceLocation l = loc();
        advance();  // consume 'using'
        auto u = std::make_unique<UsingDecl>(l);

        // using namespace X;
        if (check(TokenKind::KwNamespace)) {
            advance();
            u->form = UsingDecl::Form::Directive;
            while (check(TokenKind::Identifier)) {
                u->scope_path.push_back(advance().value);
                if (!match(TokenKind::DoubleColon)) break;
            }
            expect(TokenKind::Semicolon);
            return u;
        }

        // using T = SomeType;
        if (check(TokenKind::Identifier) && peek().kind == TokenKind::Assign) {
            u->name = advance().value;
            advance();  // =
            u->form = UsingDecl::Form::Alias;
            u->type_alias = parse_type_full();
            expect(TokenKind::Semicolon);
            return u;
        }

        // using std::cout;  or  using std::vector;
        u->form = UsingDecl::Form::Declaration;
        while (check(TokenKind::Identifier)) {
            u->scope_path.push_back(advance().value);
            if (check(TokenKind::DoubleColon)) advance();
            else break;
        }
        if (!u->scope_path.empty()) {
            u->name = u->scope_path.back();
            u->scope_path.pop_back();
        }
        expect(TokenKind::Semicolon);
        return u;
    }

    // ── typedef ──────────────────────────────────────────────────
    NodePtr parse_typedef_decl() {
        SourceLocation l = loc();
        advance();  // consume 'typedef'
        NodePtr type = parse_type_full();
        std::string name;
        if (check(TokenKind::Identifier)) name = advance().value;
        // Handle function pointer typedefs: typedef void (*fn)(int);
        else if (check(TokenKind::LParen)) {
            advance();  // (
            if (match(TokenKind::Star)) {
                if (check(TokenKind::Identifier)) name = advance().value;
            }
            expect(TokenKind::RParen);
            // consume parameter list
            if (check(TokenKind::LParen)) {
                advance(); int depth = 1;
                while (!at_eof() && depth > 0) {
                    if (check(TokenKind::LParen)) depth++;
                    else if (check(TokenKind::RParen)) depth--;
                    if (depth > 0) advance(); else advance();
                }
            }
        }
        expect(TokenKind::Semicolon);
        return std::make_unique<TypedefDecl>(name, std::move(type), l);
    }

    // ── template ─────────────────────────────────────────────────
    NodePtr parse_template_decl() {
        SourceLocation l = loc();
        advance();  // consume 'template'
        auto tmpl = std::make_unique<TemplateDecl>(l);

        expect(TokenKind::Lt);
        while (!at_eof() && !check(TokenKind::Gt)) {
            TemplateDecl::TypeParam p;
            if (check(TokenKind::KwTypename) || check(TokenKind::KwClass)) {
                advance();
                p.is_typename = true;
                if (check(TokenKind::Identifier)) p.name = advance().value;
                if (match(TokenKind::Assign))     p.default_type = parse_type_full();
            } else {
                // Non-type parameter
                p.is_typename   = false;
                p.non_type_type = parse_type_full();
                if (check(TokenKind::Identifier)) p.name = advance().value;
                if (match(TokenKind::Assign))     p.default_type = parse_assignment_expr();
            }
            tmpl->params.push_back(std::move(p));
            if (!match(TokenKind::Comma)) break;
        }
        expect(TokenKind::Gt);

        // The templated entity: class or function
        if (check(TokenKind::KwClass) || check(TokenKind::KwStruct)) {
            tmpl->body = parse_class_or_func_or_var();
        } else {
            tmpl->body = parse_func_or_var_decl(false);
        }
        return tmpl;
    }

    // ── static_assert ────────────────────────────────────────────
    NodePtr parse_static_assert() {
        SourceLocation l = loc();
        advance();
        expect(TokenKind::LParen);
        auto cond = parse_assignment_expr();
        std::string msg;
        if (match(TokenKind::Comma)) {
            if (check(TokenKind::StringLiteral)) msg = advance().value;
        }
        expect(TokenKind::RParen);
        expect(TokenKind::Semicolon);
        return std::make_unique<StaticAssertDecl>(std::move(cond), msg, l);
    }

    // ── enum ─────────────────────────────────────────────────────
    NodePtr parse_enum_decl() {
        SourceLocation l = loc();
        advance();  // consume 'enum'
        bool is_class = (check(TokenKind::KwClass) || check(TokenKind::KwStruct));
        if (is_class) advance();

        std::string name;
        if (check(TokenKind::Identifier)) name = advance().value;

        auto en = std::make_unique<EnumDecl>(name, l);
        en->is_class = is_class;

        // Optional : base type
        if (check(TokenKind::Colon)) {
            advance();
            en->base_type = parse_type_specifier();
        }

        // Forward declaration
        if (check(TokenKind::Semicolon)) { advance(); return en; }

        expect(TokenKind::LBrace);
        while (!at_eof() && !check(TokenKind::RBrace)) {
            if (!check(TokenKind::Identifier)) { advance(); continue; }
            EnumDecl::Enumerator e;
            e.name = advance().value;
            if (match(TokenKind::Assign)) e.value = parse_assignment_expr();
            en->enumerators.push_back(std::move(e));
            if (!match(TokenKind::Comma)) break;
            // allow trailing comma
            if (check(TokenKind::RBrace)) break;
        }
        expect(TokenKind::RBrace);
        expect(TokenKind::Semicolon);
        return en;
    }

    // ── class/struct/union – or a function/variable starting with them ──
    NodePtr parse_class_or_func_or_var() {
        // Could be:  struct Foo { ... };
        //            struct Foo f;  (variable with tag-type)
        //            class Foo : Base { ... };
        SourceLocation l = loc();
        bool is_struct = check(TokenKind::KwStruct);
        bool is_union  = check(TokenKind::KwUnion);
        advance();  // consume class/struct/union

        std::string name;
        if (check(TokenKind::Identifier)) name = advance().value;

        // class body
        if (check(TokenKind::Colon) || check(TokenKind::LBrace)) {
            return parse_class_body(name, is_struct, is_union, l);
        }

        // No body – this is a variable declaration using the tag
        // e.g.  struct Foo* ptr;  – reconstruct a NamedType and continue
        auto named_t = std::make_unique<NamedType>(std::vector<std::string>{}, name, l);
        auto full_t  = wrap_ptr_ref(std::move(named_t));
        return parse_var_decl_rest(std::move(full_t), l);
    }

    // ── Class body ───────────────────────────────────────────────
    NodePtr parse_class_body(const std::string& name, bool is_struct,
                              bool is_union, SourceLocation l) {
        auto cls = std::make_unique<ClassDecl>(name, l);
        cls->is_struct = is_struct;
        cls->is_union  = is_union;

        // Base classes
        if (check(TokenKind::Colon)) {
            advance();
            do {
                BaseClass base;
                if (check(TokenKind::KwVirtual)) { advance(); base.is_virtual = true; }
                if (check(TokenKind::KwPublic))    { base.access = TokenKind::KwPublic;    advance(); }
                else if (check(TokenKind::KwProtected)) { base.access = TokenKind::KwProtected; advance(); }
                else if (check(TokenKind::KwPrivate))   { base.access = TokenKind::KwPrivate;   advance(); }
                if (check(TokenKind::KwVirtual)) { advance(); base.is_virtual = true; }

                // Qualified base name
                while (check(TokenKind::Identifier)) {
                    std::string part = advance().value;
                    if (check(TokenKind::DoubleColon)) { advance(); base.scope.push_back(part); }
                    else { base.name = part; break; }
                }
                cls->bases.push_back(std::move(base));
            } while (match(TokenKind::Comma));
        }

        if (!expect(TokenKind::LBrace, "opening class body")) {
            synchronize();
            return cls;
        }

        // Default access
        TokenKind cur_access = is_struct ? TokenKind::KwPublic : TokenKind::KwPrivate;

        while (!at_eof() && !check(TokenKind::RBrace)) {
            SourceLocation ml = loc();

            // Access specifier
            if (check(TokenKind::KwPublic) || check(TokenKind::KwProtected) ||
                check(TokenKind::KwPrivate)) {
                cur_access = cur().kind;
                advance();
                expect(TokenKind::Colon);
                cls->members.push_back(
                    std::make_unique<AccessSpecifier>(cur_access, ml));
                continue;
            }

            // friend
            if (check(TokenKind::KwFriend)) {
                advance();
                auto fd = std::make_unique<FriendDecl>(parse_func_or_var_decl(true), ml);
                cls->members.push_back(std::move(fd));
                continue;
            }

            // static_assert
            if (check(TokenKind::KwStaticAssert)) {
                cls->members.push_back(parse_static_assert());
                continue;
            }

            // Destructor
            if (check(TokenKind::Tilde)) {
                cls->members.push_back(parse_destructor(name));
                continue;
            }

            // Constructor: name(...)
            if (check(TokenKind::Identifier) && cur().value == name &&
                peek().kind == TokenKind::LParen) {
                cls->members.push_back(parse_constructor(name));
                continue;
            }

            // Normal member
            if (is_type_start()) {
                auto member = parse_func_or_var_decl(true);
                if (member) cls->members.push_back(std::move(member));
            } else if (!check(TokenKind::RBrace)) {
                advance();  // error recovery
            }
        }

        expect(TokenKind::RBrace);
        // Optional variable declarator after class body: struct Foo { } bar;
        if (check(TokenKind::Identifier)) {
            // ignored – parse as VarDecl but return the class
            while (!check(TokenKind::Semicolon) && !at_eof()) advance();
        }
        expect(TokenKind::Semicolon);
        cls->is_complete = true;
        return cls;
    }

    // ── Constructor ──────────────────────────────────────────────
    NodePtr parse_constructor(const std::string& class_name) {
        SourceLocation l = loc();
        advance();  // consume name
        auto ctor = std::make_unique<ConstructorDecl>(class_name, l);
        ctor->is_explicit = false;

        // Parameters
        expect(TokenKind::LParen);
        parse_param_list(ctor->params);
        expect(TokenKind::RParen);

        // Qualifiers
        while (match(TokenKind::KwNoexcept)) ctor->is_noexcept = true;
        if (match(TokenKind::Assign)) {
            if (check(TokenKind::KwDefault)) { advance(); ctor->is_defaulted = true; }
            else if (check(TokenKind::KwDelete)) { advance(); ctor->is_deleted = true; }
            else advance();
            expect(TokenKind::Semicolon);
            return ctor;
        }

        // Initialiser list
        if (check(TokenKind::Colon)) {
            advance();
            while (!check(TokenKind::LBrace) && !at_eof()) {
                FunctionDecl::MemberInit mi;
                if (check(TokenKind::Identifier)) mi.name = advance().value;
                else if (check(TokenKind::KwThis)) { advance(); mi.name = "this"; }
                if (check(TokenKind::LParen)) {
                    advance();
                    while (!check(TokenKind::RParen) && !at_eof()) {
                        mi.args.push_back(parse_assignment_expr());
                        if (!match(TokenKind::Comma)) break;
                    }
                    expect(TokenKind::RParen);
                } else if (check(TokenKind::LBrace)) {
                    // brace-init
                    advance();
                    while (!check(TokenKind::RBrace) && !at_eof()) {
                        mi.args.push_back(parse_assignment_expr());
                        if (!match(TokenKind::Comma)) break;
                    }
                    expect(TokenKind::RBrace);
                }
                ctor->init_list.push_back(std::move(mi));
                if (!match(TokenKind::Comma)) break;
            }
        }

        if (check(TokenKind::LBrace)) ctor->body = parse_compound_stmt();
        else expect(TokenKind::Semicolon);
        return ctor;
    }

    // ── Destructor ───────────────────────────────────────────────
    NodePtr parse_destructor(const std::string& class_name) {
        SourceLocation l = loc();
        advance();  // ~
        if (check(TokenKind::Identifier)) advance();  // class name
        auto dtor = std::make_unique<DestructorDecl>(class_name, l);
        dtor->is_virtual = false;  // set by caller if preceded by 'virtual'
        expect(TokenKind::LParen);
        expect(TokenKind::RParen);
        if (match(TokenKind::KwNoexcept)) dtor->is_noexcept = true;
        if (match(TokenKind::Assign)) {
            if (check(TokenKind::KwDefault)) { advance(); dtor->is_defaulted = true; }
            else if (check(TokenKind::KwDelete)) { advance(); dtor->is_deleted = true; }
            else advance();
            expect(TokenKind::Semicolon);
            return dtor;
        }
        if (check(TokenKind::LBrace)) dtor->body = parse_compound_stmt();
        else expect(TokenKind::Semicolon);
        return dtor;
    }

    // ── Function or variable declaration ─────────────────────────
    NodePtr parse_func_or_var_decl(bool inside_class) {
        SourceLocation l = loc();
        // Gather prefix specifiers (static, inline, virtual, override, const, ...)
        bool is_static    = false;
        bool is_inline    = false;
        bool is_virtual   = false;
        bool is_constexpr = false;
        bool is_extern    = false;
        bool is_explicit  = false;
        bool is_friend    = false;
        bool is_mutable   = false;

        while (true) {
            if (match(TokenKind::KwStatic))    { is_static    = true; continue; }
            if (match(TokenKind::KwInline))    { is_inline    = true; continue; }
            if (match(TokenKind::KwVirtual))   { is_virtual   = true; continue; }
            if (match(TokenKind::KwConstexpr)) { is_constexpr = true; continue; }
            if (match(TokenKind::KwExtern))    { is_extern    = true; continue; }
            if (match(TokenKind::KwExplicit))  { is_explicit  = true; continue; }
            if (match(TokenKind::KwFriend))    { is_friend    = true; continue; }
            if (match(TokenKind::KwMutable))   { is_mutable   = true; continue; }
            break;
        }

        // Destructor inside class
        if (check(TokenKind::Tilde)) {
            // The class name was already passed to parse_class_body
            auto dtor = parse_destructor("");
            dtor->as<DestructorDecl>()->is_virtual = is_virtual;
            return dtor;
        }

        // operator overload (return type may be missing for user-defined literals)
        if (check(TokenKind::KwOperator)) {
            return parse_operator_func(is_static, is_inline, is_virtual,
                                       is_const_member(), l);
        }

        // Parse return/variable type
        NodePtr type = parse_type_specifier_with_specifiers(
            is_static, is_inline, is_virtual, is_constexpr, is_extern);

        if (!type) {
            synchronize();
            return nullptr;
        }

        // Pointer/reference decoration
        type = wrap_ptr_ref(std::move(type));

        // Qualified name or operator
        std::vector<std::string> scope;
        std::string name;

        if (check(TokenKind::KwOperator)) {
            // e.g.  bool operator==(...)
            return parse_operator_func_with_return(std::move(type),
                scope, is_static, is_inline, is_virtual, is_const_member(), l);
        }

        // Parse (possibly qualified) name
        while (check(TokenKind::Identifier)) {
            name = advance().value;
            if (check(TokenKind::DoubleColon)) {
                advance();
                scope.push_back(name);
            } else {
                break;
            }
        }

        if (name.empty() && check(TokenKind::Tilde)) {
            // ~ClassName()
            advance();
            std::string cn;
            if (check(TokenKind::Identifier)) cn = advance().value;
            if (!scope.empty()) cn = scope.back();
            auto dtor = std::make_unique<DestructorDecl>(cn, l);
            dtor->is_virtual = is_virtual;
            expect(TokenKind::LParen);
            expect(TokenKind::RParen);
            if (match(TokenKind::KwNoexcept)) dtor->is_noexcept = true;
            if (check(TokenKind::LBrace)) dtor->body = parse_compound_stmt();
            else expect(TokenKind::Semicolon);
            return dtor;
        }

        // Function?
        if (check(TokenKind::LParen) || check(TokenKind::Lt)) {
            return parse_function_decl_rest(std::move(type), scope, name,
                is_static, is_inline, is_virtual, is_explicit,
                is_constexpr, is_extern, inside_class, l);
        }

        // Variable / field declaration
        if (name.empty() && check(TokenKind::Identifier)) name = advance().value;
        return parse_var_decl_rest_named(std::move(type), name, is_static,
                                         is_constexpr, is_extern, l);
    }

    bool is_const_member() { return false; } // will be set post-parse

    NodePtr parse_type_specifier_with_specifiers(bool& is_static, bool& is_inline,
                                                   bool& is_virtual, bool& is_constexpr,
                                                   bool& is_extern) {
        // Re-gather (they may have been pushed back)
        while (true) {
            if (match(TokenKind::KwStatic))    { is_static    = true; continue; }
            if (match(TokenKind::KwInline))    { is_inline    = true; continue; }
            if (match(TokenKind::KwVirtual))   { is_virtual   = true; continue; }
            if (match(TokenKind::KwConstexpr)) { is_constexpr = true; continue; }
            if (match(TokenKind::KwExtern))    { is_extern    = true; continue; }
            break;
        }
        return parse_type_specifier();
    }

    // ── Function declaration (rest) ──────────────────────────────
    NodePtr parse_function_decl_rest(NodePtr ret_type,
        std::vector<std::string> scope, std::string name,
        bool is_static, bool is_inline, bool is_virtual, bool is_explicit,
        bool is_constexpr, bool is_extern, bool inside_class,
        SourceLocation l) {

        // Template args on the function (ignore for now)
        if (check(TokenKind::Lt)) {
            advance(); int depth = 1;
            while (!at_eof() && depth > 0) {
                if (check(TokenKind::Lt)) depth++;
                else if (check(TokenKind::Gt)) depth--;
                if (depth > 0) advance(); else advance();
            }
        }

        auto fn = std::make_unique<FunctionDecl>(name, l);
        fn->return_type = std::move(ret_type);
        fn->scope       = std::move(scope);
        fn->is_static   = is_static;
        fn->is_inline   = is_inline;
        fn->is_virtual  = is_virtual;
        fn->is_constexpr= is_constexpr;
        fn->is_explicit = is_explicit;
        fn->is_extern   = is_extern;

        expect(TokenKind::LParen);
        parse_param_list(fn->params);
        expect(TokenKind::RParen);

        // Post-parameter qualifiers
        if (match(TokenKind::KwConst))    fn->is_const    = true;
        if (match(TokenKind::KwVolatile)) fn->is_volatile = true;
        if (match(TokenKind::KwNoexcept)) {
            fn->is_noexcept = true;
            if (match(TokenKind::LParen)) { parse_assignment_expr(); expect(TokenKind::RParen); }
        }
        if (match(TokenKind::KwConst))    fn->is_const    = true;
        if (check(TokenKind::KwOverride)) { advance(); fn->is_override = true; }
        if (check(TokenKind::KwFinal))    { advance(); fn->is_final    = true; }

        // Trailing return type: -> T
        if (check(TokenKind::Arrow)) {
            advance();
            fn->return_type = parse_type_full();
        }

        // = 0 / = default / = delete
        if (match(TokenKind::Assign)) {
            if (check(TokenKind::IntLiteral) && cur().value == "0") {
                advance(); fn->is_pure_virtual = true;
            } else if (check(TokenKind::KwDefault)) {
                advance(); fn->is_defaulted = true;
            } else if (check(TokenKind::KwDelete)) {
                advance(); fn->is_deleted = true;
            } else {
                advance();
            }
            expect(TokenKind::Semicolon);
            return fn;
        }

        if (check(TokenKind::LBrace)) {
            fn->body = parse_compound_stmt();
        } else {
            expect(TokenKind::Semicolon);
        }
        return fn;
    }

    // ── Operator overload function ───────────────────────────────
    NodePtr parse_operator_func(bool is_static, bool is_inline,
                                 bool is_virtual, bool is_const,
                                 SourceLocation l) {
        // Return type was not yet parsed – parse it now if present
        // Actually we get here without a return type; let's re-parse
        auto type = parse_type_specifier();
        type = wrap_ptr_ref(std::move(type));
        return parse_operator_func_with_return(std::move(type), {},
            is_static, is_inline, is_virtual, is_const, l);
    }

    NodePtr parse_operator_func_with_return(NodePtr ret_type,
        std::vector<std::string> scope, bool is_static, bool is_inline,
        bool is_virtual, bool is_const, SourceLocation l) {

        expect(TokenKind::KwOperator);
        std::string op_tok = parse_operator_symbol();
        auto fn = std::make_unique<FunctionDecl>("operator" + op_tok, l);
        fn->return_type = std::move(ret_type);
        fn->scope       = std::move(scope);
        fn->is_static   = is_static;
        fn->is_inline   = is_inline;
        fn->is_virtual  = is_virtual;
        fn->is_const    = is_const;
        fn->is_operator = true;
        fn->operator_token = op_tok;

        expect(TokenKind::LParen);
        parse_param_list(fn->params);
        expect(TokenKind::RParen);
        if (match(TokenKind::KwConst)) fn->is_const = true;
        if (match(TokenKind::KwNoexcept)) fn->is_noexcept = true;
        if (check(TokenKind::KwOverride)) { advance(); fn->is_override = true; }

        if (match(TokenKind::Assign)) {
            if (check(TokenKind::KwDefault)) { advance(); fn->is_defaulted = true; }
            else if (check(TokenKind::KwDelete)) { advance(); fn->is_deleted = true; }
            else advance();
            expect(TokenKind::Semicolon);
            return fn;
        }

        if (check(TokenKind::LBrace)) fn->body = parse_compound_stmt();
        else expect(TokenKind::Semicolon);
        return fn;
    }

    std::string parse_operator_symbol() {
        // Consume the operator token(s) and return the symbolic text
        if (at_eof()) return "?";
        auto k = cur().kind;
        // Special: operator()  operator[]  operator new  operator delete
        if (k == TokenKind::LParen) {
            advance(); expect(TokenKind::RParen); return "()";
        }
        if (k == TokenKind::LBracket) {
            advance(); expect(TokenKind::RBracket); return "[]";
        }
        if (k == TokenKind::KwNew) {
            advance();
            if (check(TokenKind::LBracket)) { advance(); expect(TokenKind::RBracket); return "new[]"; }
            return "new";
        }
        if (k == TokenKind::KwDelete) {
            advance();
            if (check(TokenKind::LBracket)) { advance(); expect(TokenKind::RBracket); return "delete[]"; }
            return "delete";
        }
        // Conversion operators: operator int(), operator std::string()
        if (cur().isTypeName() || cur().kind == TokenKind::Identifier) {
            // Gather until '('
            std::string s;
            while (!check(TokenKind::LParen) && !at_eof()) s += advance().value;
            return s;
        }
        std::string s = advance().value;
        // Two-char operators that might have been split
        return s;
    }

    // ── Parameter list ───────────────────────────────────────────
    void parse_param_list(std::vector<NodePtr>& params) {
        // () or (void) → empty
        if (check(TokenKind::RParen)) return;
        if (check(TokenKind::KwVoid) && peek().kind == TokenKind::RParen) {
            advance(); return;
        }
        while (!at_eof() && !check(TokenKind::RParen)) {
            if (check(TokenKind::Ellipsis)) {
                advance();
                auto p = std::make_unique<ParamDecl>(
                    make_prim(PrimitiveType::Kind::Int, loc()), "...", nullptr, loc());
                p->is_variadic = true;
                params.push_back(std::move(p));
                break;
            }

            SourceLocation pl = loc();
            NodePtr ptype = parse_type_full();
            std::string  pname;
            NodePtr      pdefault;

            // Optional name
            if (check(TokenKind::Identifier)) pname = advance().value;

            // Optional array declarator: int arr[]
            if (check(TokenKind::LBracket)) {
                advance();
                NodePtr sz;
                if (!check(TokenKind::RBracket)) sz = parse_assignment_expr();
                expect(TokenKind::RBracket);
                ptype = std::make_unique<ArrayType>(std::move(ptype), std::move(sz), pl);
            }

            // Default value
            if (match(TokenKind::Assign)) pdefault = parse_assignment_expr();

            params.push_back(std::make_unique<ParamDecl>(
                std::move(ptype), pname, std::move(pdefault), pl));

            if (!match(TokenKind::Comma)) break;
        }
    }

    // ── Variable declaration (rest) ──────────────────────────────
    NodePtr parse_var_decl_rest(NodePtr type, SourceLocation l) {
        std::string name;
        if (check(TokenKind::Identifier)) name = advance().value;
        return parse_var_decl_rest_named(std::move(type), name, false, false, false, l);
    }

    NodePtr parse_var_decl_rest_named(NodePtr type, std::string name,
        bool is_static, bool is_constexpr, bool is_extern, SourceLocation l) {
        // Build a DeclStmt that may contain multiple declarators
        auto ds = std::make_unique<DeclStmt>(l);

        auto make_var = [&](std::string n, NodePtr t, NodePtr init_expr) {
            auto v = std::make_unique<VarDecl>(n, std::move(t), std::move(init_expr), l);
            v->is_static    = is_static;
            v->is_constexpr = is_constexpr;
            v->is_extern    = is_extern;
            return v;
        };

        // Array declarator:  type name[N] or type name[]
        if (check(TokenKind::LBracket)) {
            advance();
            NodePtr sz;
            if (!check(TokenKind::RBracket)) sz = parse_assignment_expr();
            expect(TokenKind::RBracket);
            type = std::make_unique<ArrayType>(std::move(type), std::move(sz), l);
        }

        NodePtr init_expr;
        // Check for function-like initialisation: Foo f(args)
        if (check(TokenKind::LParen)) {
            advance();
            auto call_args = std::make_unique<InitListExpr>(l);
            while (!check(TokenKind::RParen) && !at_eof()) {
                call_args->elements.push_back(parse_assignment_expr());
                if (!match(TokenKind::Comma)) break;
            }
            expect(TokenKind::RParen);
            init_expr = std::move(call_args);
        } else if (check(TokenKind::LBrace)) {
            init_expr = parse_initialiser_list_expr();
        } else if (match(TokenKind::Assign)) {
            init_expr = parse_assignment_expr();
        }

        ds->decls.push_back(make_var(name, clone_type(type.get()), std::move(init_expr)));

        // Multiple declarators: int a, *b = nullptr, c = 3;
        while (match(TokenKind::Comma)) {
            NodePtr extra_type = clone_type(type.get());
            extra_type = wrap_ptr_ref(std::move(extra_type));
            std::string extra_name;
            if (check(TokenKind::Identifier)) extra_name = advance().value;

            NodePtr extra_init;
            if (check(TokenKind::LParen)) {
                advance();
                auto call_args = std::make_unique<InitListExpr>(l);
                while (!check(TokenKind::RParen) && !at_eof()) {
                    call_args->elements.push_back(parse_assignment_expr());
                    if (!match(TokenKind::Comma)) break;
                }
                expect(TokenKind::RParen);
                extra_init = std::move(call_args);
            } else if (match(TokenKind::Assign)) {
                extra_init = parse_assignment_expr();
            } else if (check(TokenKind::LBrace)) {
                extra_init = parse_initialiser_list_expr();
            }
            ds->decls.push_back(make_var(extra_name, std::move(extra_type), std::move(extra_init)));
        }

        expect(TokenKind::Semicolon);
        if (ds->decls.size() == 1) return std::move(ds->decls[0]);
        return ds;
    }

    // ── Statements ───────────────────────────────────────────────
    NodePtr parse_compound_stmt() {
        SourceLocation l = loc();
        expect(TokenKind::LBrace);
        auto cs = std::make_unique<CompoundStmt>(l);
        while (!at_eof() && !check(TokenKind::RBrace)) {
            if (auto s = parse_stmt()) cs->stmts.push_back(std::move(s));
            else if (!check(TokenKind::RBrace) && !at_eof()) advance();
        }
        expect(TokenKind::RBrace);
        return cs;
    }

    NodePtr parse_stmt() {
        SourceLocation l = loc();
        switch (cur().kind) {
            case TokenKind::Semicolon:  { advance(); return std::make_unique<NullStmt>(l); }
            case TokenKind::LBrace:     return parse_compound_stmt();
            case TokenKind::KwIf:       return parse_if_stmt();
            case TokenKind::KwFor:      return parse_for_stmt();
            case TokenKind::KwWhile:    return parse_while_stmt();
            case TokenKind::KwDo:       return parse_do_while_stmt();
            case TokenKind::KwSwitch:   return parse_switch_stmt();
            case TokenKind::KwReturn:   return parse_return_stmt();
            case TokenKind::KwBreak:    { advance(); expect(TokenKind::Semicolon); return std::make_unique<BreakStmt>(l); }
            case TokenKind::KwContinue: { advance(); expect(TokenKind::Semicolon); return std::make_unique<ContinueStmt>(l); }
            case TokenKind::KwGoto:     return parse_goto_stmt();
            case TokenKind::KwTry:      return parse_try_stmt();
            case TokenKind::KwThrow:    return parse_throw_stmt();
            case TokenKind::KwStaticAssert: return parse_static_assert();
            default: break;
        }

        // Label statement
        if (check(TokenKind::Identifier) && peek().kind == TokenKind::Colon) {
            std::string lbl = advance().value;
            advance(); // :
            auto s = parse_stmt();
            return std::make_unique<LabelStmt>(lbl, std::move(s), l);
        }

        // Declaration statement (variable declaration)
        if (is_type_start_strict()) {
            return parse_decl_stmt();
        }

        // Expression statement
        auto expr = parse_expr();
        if (!expr) return nullptr;
        expect(TokenKind::Semicolon);
        return std::make_unique<ExprStmt>(std::move(expr), l);
    }

    // Stricter type-start check (avoids ambiguities with identifiers used as expressions)
    bool is_type_start_strict() const {
        auto k = cur().kind;
        // Definite type keywords
        if (k == TokenKind::KwAuto)      return true;
        if (k == TokenKind::KwConst)     return true;
        if (k == TokenKind::KwVolatile)  return true;
        if (k == TokenKind::KwStatic)    return true;
        if (k == TokenKind::KwConstexpr) return true;
        if (k == TokenKind::KwExtern)    return true;
        if (cur().isTypeName())          return true;
        if (k == TokenKind::KwStruct || k == TokenKind::KwClass ||
            k == TokenKind::KwEnum  || k == TokenKind::KwUnion)  return true;
        if (k != TokenKind::Identifier) return false;

        // Identifier followed by another identifier → type name
        if (peek().kind == TokenKind::Identifier) return true;

        // Identifier followed by * or & → could be type if followed by identifier/semicolon
        if (peek().kind == TokenKind::Star || peek().kind == TokenKind::Amp ||
            peek().kind == TokenKind::AmpAmp) {
            // Look two tokens ahead: T* name or T* = ...
            auto k2 = peek(2).kind;
            return k2 == TokenKind::Identifier || k2 == TokenKind::Semicolon;
        }

        // Identifier :: → need to look much further ahead
        if (peek().kind == TokenKind::DoubleColon) {
            // Find where the qualified name ends, then check what follows
            size_t i = pos_;
            while (i + 1 < tokens_.size() &&
                   tokens_[i].kind == TokenKind::Identifier &&
                   tokens_[i+1].kind == TokenKind::DoubleColon) {
                i += 2;  // skip "Ident ::"
            }
            // Now i points at the final Identifier in the qualified name
            if (i < tokens_.size() && tokens_[i].kind == TokenKind::Identifier) {
                i++;  // skip last identifier
            }
            // Skip template args if present
            if (i < tokens_.size() && tokens_[i].kind == TokenKind::Lt) {
                i++;
                int depth = 1;
                while (i < tokens_.size() && depth > 0) {
                    if (tokens_[i].kind == TokenKind::Lt)      depth++;
                    else if (tokens_[i].kind == TokenKind::Gt) depth--;
                    else if (tokens_[i].kind == TokenKind::RShift) { depth -= 2; }
                    i++;
                }
            }
            // Skip pointer/ref decorators
            while (i < tokens_.size() &&
                   (tokens_[i].kind == TokenKind::Star  ||
                    tokens_[i].kind == TokenKind::Amp   ||
                    tokens_[i].kind == TokenKind::AmpAmp ||
                    tokens_[i].kind == TokenKind::KwConst)) {
                i++;
            }
            // If next token is an identifier, it's a declaration
            if (i < tokens_.size() && tokens_[i].kind == TokenKind::Identifier)
                return true;
            // If next is '(' it might be a function call (expression) or a function decl
            // For now treat it as expression
            return false;
        }

        // Identifier followed by < → template instantiation as expression OR type
        // Use heuristic: if followed by a type keyword inside <>, treat as type
        if (peek().kind == TokenKind::Lt) {
            // Look for > then identifier → declaration; > then operator → expression
            size_t i = pos_ + 2;
            int depth = 1;
            while (i < tokens_.size() && depth > 0) {
                if (tokens_[i].kind == TokenKind::Lt)      depth++;
                else if (tokens_[i].kind == TokenKind::Gt) depth--;
                else if (tokens_[i].kind == TokenKind::RShift) depth -= 2;
                i++;
            }
            // After template args: pointer/ref decorators then identifier?
            while (i < tokens_.size() &&
                   (tokens_[i].kind == TokenKind::Star  ||
                    tokens_[i].kind == TokenKind::Amp   ||
                    tokens_[i].kind == TokenKind::AmpAmp)) {
                i++;
            }
            if (i < tokens_.size() && tokens_[i].kind == TokenKind::Identifier)
                return true;
            return false;
        }

        return false;
    }

    NodePtr parse_decl_stmt() {
        SourceLocation l = loc();
        bool is_static   = false;
        bool is_constexpr= false;
        bool is_extern   = false;

        while (true) {
            if (match(TokenKind::KwStatic))    { is_static    = true; continue; }
            if (match(TokenKind::KwConstexpr)) { is_constexpr = true; continue; }
            if (match(TokenKind::KwExtern))    { is_extern    = true; continue; }
            if (match(TokenKind::KwInline))    continue;
            break;
        }

        NodePtr type = parse_type_full();
        std::string name;
        if (check(TokenKind::Identifier)) name = advance().value;

        // Could be a function call used as a statement if no name follows
        // In that case we fall through to expression parsing
        auto ds = parse_var_decl_rest_named(std::move(type), name,
                                             is_static, is_constexpr, is_extern, l);
        // parse_var_decl_rest_named returns a DeclStmt for multi-declarators
        // (e.g. int a = 10, b = 3;) — return it directly to avoid double-wrapping.
        if (ds && ds->kind == NodeKind::DeclStmt) return ds;
        auto wrapper = std::make_unique<DeclStmt>(l);
        wrapper->decls.push_back(std::move(ds));
        return wrapper;
    }

    NodePtr parse_if_stmt() {
        SourceLocation l = loc();
        advance();  // if
        bool is_ce = (check(TokenKind::KwConstexpr));
        if (is_ce) advance();

        expect(TokenKind::LParen);
        // C++17: if (init; cond)
        NodePtr init_stmt;
        auto cond = parse_if_condition(init_stmt);
        expect(TokenKind::RParen);

        auto s = std::make_unique<IfStmt>(l);
        s->is_constexpr = is_ce;
        s->init_stmt    = std::move(init_stmt);
        s->condition    = std::move(cond);
        s->then_branch  = parse_stmt();
        if (match(TokenKind::KwElse)) s->else_branch = parse_stmt();
        return s;
    }

    NodePtr parse_if_condition(NodePtr& init_stmt_out) {
        // Check for init-statement:  if (int x = 0; x < 10) 
        // Heuristic: if we see a type start and `;` will appear, it's init-stmt
        // Simple approach: always try as expression first
        // If it's a declaration, parse as such
        if (is_type_start_strict()) {
            // Try to detect init-statement by saving position
            size_t saved = pos_;
            bool has_semi = false;
            int depth = 0;
            for (size_t i = pos_; i < tokens_.size(); i++) {
                auto tk = tokens_[i].kind;
                if (tk == TokenKind::LParen || tk == TokenKind::LBrace) depth++;
                else if (tk == TokenKind::RParen || tk == TokenKind::RBrace) {
                    if (depth == 0) break; depth--;
                } else if (tk == TokenKind::Semicolon && depth == 0) {
                    has_semi = true; break;
                }
            }
            if (has_semi) {
                init_stmt_out = parse_decl_stmt();
                return parse_assignment_expr();
            }
        }
        return parse_assignment_expr();
    }

    NodePtr parse_for_stmt() {
        SourceLocation l = loc();
        advance();  // for
        expect(TokenKind::LParen);

        // Range-based for detection: look for ':' at depth 0 in the for-header
        // after any init expression
        bool is_range_for = detect_range_for();

        if (is_range_for) return parse_range_for_stmt_rest(l);

        // Traditional for
        auto s = std::make_unique<ForStmt>(l);
        // Init
        if (!check(TokenKind::Semicolon)) {
            if (is_type_start_strict()) {
                auto ds = parse_decl_stmt();
                // parse_decl_stmt already consumed the ';'
                s->init = std::move(ds);
            } else {
                s->init = std::make_unique<ExprStmt>(parse_expr(), l);
                expect(TokenKind::Semicolon);
            }
        } else {
            advance();
        }
        // Condition
        if (!check(TokenKind::Semicolon)) s->condition = parse_assignment_expr();
        expect(TokenKind::Semicolon);
        // Increment
        if (!check(TokenKind::RParen)) s->increment = parse_expr();
        expect(TokenKind::RParen);
        s->body = parse_stmt();
        return s;
    }

    bool detect_range_for() {
        int depth = 0;
        for (size_t i = pos_; i < tokens_.size(); i++) {
            auto tk = tokens_[i].kind;
            if (tk == TokenKind::LParen || tk == TokenKind::LBrace) { depth++; continue; }
            if (tk == TokenKind::RParen || tk == TokenKind::RBrace) {
                if (depth == 0) break; depth--; continue;
            }
            if (tk == TokenKind::Semicolon && depth == 0) return false;
            if (tk == TokenKind::Colon     && depth == 0) return true;
        }
        return false;
    }

    NodePtr parse_range_for_stmt_rest(SourceLocation l) {
        // We're right after '(' of for(
        auto s = std::make_unique<RangeForStmt>(l);
        // Range variable declaration
        NodePtr decl_type = parse_type_full();
        std::string decl_name;
        if (check(TokenKind::Identifier)) decl_name = advance().value;

        auto var = std::make_unique<VarDecl>(decl_name, std::move(decl_type), nullptr, l);
        s->range_decl = std::move(var);

        expect(TokenKind::Colon);
        s->range_expr = parse_assignment_expr();
        expect(TokenKind::RParen);
        s->body = parse_stmt();
        return s;
    }

    NodePtr parse_while_stmt() {
        SourceLocation l = loc();
        advance();
        expect(TokenKind::LParen);
        auto s = std::make_unique<WhileStmt>(l);
        s->condition = parse_assignment_expr();
        expect(TokenKind::RParen);
        s->body = parse_stmt();
        return s;
    }

    NodePtr parse_do_while_stmt() {
        SourceLocation l = loc();
        advance();
        auto s = std::make_unique<DoWhileStmt>(l);
        s->body = parse_stmt();
        expect(TokenKind::KwWhile);
        expect(TokenKind::LParen);
        s->condition = parse_assignment_expr();
        expect(TokenKind::RParen);
        expect(TokenKind::Semicolon);
        return s;
    }

    NodePtr parse_switch_stmt() {
        SourceLocation l = loc();
        advance();
        expect(TokenKind::LParen);
        auto s = std::make_unique<SwitchStmt>(l);
        s->control = parse_assignment_expr();
        expect(TokenKind::RParen);
        expect(TokenKind::LBrace);
        while (!at_eof() && !check(TokenKind::RBrace)) {
            if (check(TokenKind::KwCase)) {
                advance();
                auto cs = std::make_unique<CaseStmt>(parse_assignment_expr(), loc());
                expect(TokenKind::Colon);
                while (!at_eof() && !check(TokenKind::KwCase) &&
                       !check(TokenKind::KwDefault) && !check(TokenKind::RBrace)) {
                    cs->stmts.push_back(parse_stmt());
                }
                s->cases.push_back(std::move(cs));
            } else if (check(TokenKind::KwDefault)) {
                advance(); expect(TokenKind::Colon);
                auto ds = std::make_unique<DefaultStmt>(l);
                while (!at_eof() && !check(TokenKind::KwCase) &&
                       !check(TokenKind::KwDefault) && !check(TokenKind::RBrace)) {
                    ds->stmts.push_back(parse_stmt());
                }
                s->cases.push_back(std::move(ds));
            } else {
                s->cases.push_back(parse_stmt());
            }
        }
        expect(TokenKind::RBrace);
        return s;
    }

    NodePtr parse_return_stmt() {
        SourceLocation l = loc();
        advance();
        NodePtr val;
        if (!check(TokenKind::Semicolon)) val = parse_expr();
        expect(TokenKind::Semicolon);
        return std::make_unique<ReturnStmt>(std::move(val), l);
    }

    NodePtr parse_goto_stmt() {
        SourceLocation l = loc();
        advance();
        std::string lbl;
        if (check(TokenKind::Identifier)) lbl = advance().value;
        expect(TokenKind::Semicolon);
        return std::make_unique<GotoStmt>(lbl, l);
    }

    NodePtr parse_try_stmt() {
        SourceLocation l = loc();
        advance();
        auto s = std::make_unique<TryStmt>(l);
        s->try_body = parse_compound_stmt();
        while (check(TokenKind::KwCatch)) {
            advance();
            auto cl = std::make_unique<CatchClause>(l);
            expect(TokenKind::LParen);
            if (check(TokenKind::Ellipsis)) {
                advance(); cl->is_ellipsis = true;
            } else {
                SourceLocation pl = loc();
                NodePtr pt = parse_type_full();
                std::string pn;
                if (check(TokenKind::Identifier)) pn = advance().value;
                cl->param = std::make_unique<ParamDecl>(std::move(pt), pn, nullptr, pl);
            }
            expect(TokenKind::RParen);
            cl->body = parse_compound_stmt();
            s->catches.push_back(std::move(cl));
        }
        return s;
    }

    NodePtr parse_throw_stmt() {
        SourceLocation l = loc();
        advance();
        NodePtr expr;
        if (!check(TokenKind::Semicolon)) expr = parse_assignment_expr();
        expect(TokenKind::Semicolon);
        return std::make_unique<ThrowStmt>(std::move(expr), l);
    }

    // ── Expression parsing (precedence climbing) ─────────────────

    NodePtr parse_expr() {
        SourceLocation l = loc();
        auto lhs = parse_assignment_expr();
        if (match(TokenKind::Comma)) {
            auto rhs = parse_expr();
            return std::make_unique<CommaExpr>(std::move(lhs), std::move(rhs), l);
        }
        return lhs;
    }

    NodePtr parse_assignment_expr() {
        SourceLocation l = loc();
        auto lhs = parse_ternary_expr();
        if (cur().isAssignOp()) {
            TokenKind op = advance().kind;
            auto rhs = parse_assignment_expr();
            return std::make_unique<AssignExpr>(op, std::move(lhs), std::move(rhs), l);
        }
        return lhs;
    }

    NodePtr parse_ternary_expr() {
        SourceLocation l = loc();
        auto cond = parse_logical_or_expr();
        if (match(TokenKind::Question)) {
            auto then_e = parse_assignment_expr();
            expect(TokenKind::Colon);
            auto else_e = parse_assignment_expr();
            return std::make_unique<TernaryExpr>(
                std::move(cond), std::move(then_e), std::move(else_e), l);
        }
        return cond;
    }

    // Precedence levels 4–12 via recursive calls
    NodePtr parse_logical_or_expr()  { return parse_binop(0); }

    // Operator precedence table
    struct OpInfo { int prec; bool right_assoc; };
    static OpInfo op_info(TokenKind k) {
        switch (k) {
            case TokenKind::PipePipe:    return {3,  false};
            case TokenKind::AmpAmp:      return {4,  false};
            case TokenKind::KwOr:        return {3,  false};
            case TokenKind::KwAnd:       return {4,  false};
            case TokenKind::Pipe:        return {5,  false};
            case TokenKind::Caret:       return {6,  false};
            case TokenKind::Amp:         return {7,  false};
            case TokenKind::Eq:          return {8,  false};
            case TokenKind::NotEq:       return {8,  false};
            case TokenKind::KwNotEq:     return {8,  false};
            case TokenKind::Lt:          return {9,  false};
            case TokenKind::Gt:          return {9,  false};
            case TokenKind::LtEq:        return {9,  false};
            case TokenKind::GtEq:        return {9,  false};
            case TokenKind::Spaceship:   return {9,  false};
            case TokenKind::LShift:      return {10, false};
            case TokenKind::RShift:      return {10, false};
            case TokenKind::Plus:        return {11, false};
            case TokenKind::Minus:       return {11, false};
            case TokenKind::Star:        return {12, false};
            case TokenKind::Slash:       return {12, false};
            case TokenKind::Percent:     return {12, false};
            case TokenKind::DotStar:     return {13, false};
            case TokenKind::ArrowStar:   return {13, false};
            default:                     return {-1, false};
        }
    }

    NodePtr parse_binop(int min_prec) {
        SourceLocation l = loc();
        auto lhs = parse_unary_expr();

        while (true) {
            auto info = op_info(cur().kind);
            if (info.prec < min_prec) break;
            TokenKind op = advance().kind;
            int next_prec = info.right_assoc ? info.prec : info.prec + 1;
            auto rhs = parse_binop(next_prec);
            lhs = std::make_unique<BinaryExpr>(op, std::move(lhs), std::move(rhs), l);
            l = loc();
        }
        return lhs;
    }

    NodePtr parse_unary_expr() {
        SourceLocation l = loc();
        switch (cur().kind) {
            case TokenKind::PlusPlus: {
                advance();
                auto e = parse_unary_expr();
                return std::make_unique<UnaryExpr>(TokenKind::PlusPlus, std::move(e), false, l);
            }
            case TokenKind::MinusMinus: {
                advance();
                auto e = parse_unary_expr();
                return std::make_unique<UnaryExpr>(TokenKind::MinusMinus, std::move(e), false, l);
            }
            case TokenKind::Bang: case TokenKind::KwNot: {
                advance();
                auto e = parse_unary_expr();
                return std::make_unique<UnaryExpr>(TokenKind::Bang, std::move(e), false, l);
            }
            case TokenKind::Tilde: case TokenKind::KwCompl: {
                advance();
                auto e = parse_unary_expr();
                return std::make_unique<UnaryExpr>(TokenKind::Tilde, std::move(e), false, l);
            }
            case TokenKind::Minus: {
                advance();
                auto e = parse_unary_expr();
                return std::make_unique<UnaryExpr>(TokenKind::Minus, std::move(e), false, l);
            }
            case TokenKind::Plus: {
                advance();
                return parse_unary_expr();  // unary + is identity
            }
            case TokenKind::Amp: {
                // address-of
                advance();
                auto e = parse_unary_expr();
                return std::make_unique<UnaryExpr>(TokenKind::Amp, std::move(e), false, l);
            }
            case TokenKind::Star: {
                // dereference
                advance();
                auto e = parse_unary_expr();
                return std::make_unique<UnaryExpr>(TokenKind::Star, std::move(e), false, l);
            }
            case TokenKind::KwSizeof: {
                advance();
                if (check(TokenKind::LParen)) {
                    advance();
                    // sizeof(type) or sizeof(expr)
                    NodePtr type_arg, expr_arg;
                    if (is_type_start_strict()) type_arg = parse_type_full();
                    else                        expr_arg = parse_assignment_expr();
                    expect(TokenKind::RParen);
                    return std::make_unique<SizeofExpr>(std::move(expr_arg), std::move(type_arg), l);
                }
                auto e = parse_unary_expr();
                return std::make_unique<SizeofExpr>(std::move(e), nullptr, l);
            }
            case TokenKind::KwNew: return parse_new_expr(l);
            case TokenKind::KwDelete: return parse_delete_expr(l);
            case TokenKind::KwThrow: {
                advance();
                NodePtr e;
                if (!check(TokenKind::Semicolon) && !check(TokenKind::RParen) &&
                    !check(TokenKind::RBrace)) {
                    e = parse_assignment_expr();
                }
                return std::make_unique<ThrowStmt>(std::move(e), l);
            }
            // Casts
            case TokenKind::KwStaticCast: case TokenKind::KwDynamicCast:
            case TokenKind::KwReinterpretCast: case TokenKind::KwConstCast:
                return parse_named_cast(l);

            default: break;
        }
        return parse_postfix_expr();
    }

    NodePtr parse_new_expr(SourceLocation l) {
        advance();  // new
        auto ne = std::make_unique<NewExpr>(nullptr, l);

        // Placement new: new (place) Type
        if (check(TokenKind::LParen)) {
            advance();
            ne->placement = parse_assignment_expr();
            expect(TokenKind::RParen);
        }

        // Type
        if (check(TokenKind::LParen)) {
            // new (Type)  functional form
            advance();
            ne->type = parse_type_full();
            expect(TokenKind::RParen);
        } else {
            ne->type = parse_type_full();
        }

        // Array dimension
        if (check(TokenKind::LBracket)) {
            ne->is_array = true;
            advance();
            if (!check(TokenKind::RBracket)) ne->array_size = parse_assignment_expr();
            expect(TokenKind::RBracket);
        }

        // Constructor args
        if (check(TokenKind::LParen)) {
            advance();
            while (!check(TokenKind::RParen) && !at_eof()) {
                ne->args.push_back(parse_assignment_expr());
                if (!match(TokenKind::Comma)) break;
            }
            expect(TokenKind::RParen);
        }
        return ne;
    }

    NodePtr parse_delete_expr(SourceLocation l) {
        advance();  // delete
        bool is_array = false;
        if (check(TokenKind::LBracket)) { advance(); expect(TokenKind::RBracket); is_array = true; }
        auto e = parse_unary_expr();
        return std::make_unique<DeleteExpr>(std::move(e), is_array, l);
    }

    NodePtr parse_named_cast(SourceLocation l) {
        CastKind ck;
        switch (cur().kind) {
            case TokenKind::KwStaticCast:      ck = CastKind::Static;       break;
            case TokenKind::KwDynamicCast:     ck = CastKind::Dynamic;      break;
            case TokenKind::KwReinterpretCast: ck = CastKind::Reinterpret;  break;
            default:                           ck = CastKind::Const;        break;
        }
        advance();
        expect(TokenKind::Lt);
        auto type = parse_type_full();
        expect(TokenKind::Gt);
        expect(TokenKind::LParen);
        auto expr = parse_assignment_expr();
        expect(TokenKind::RParen);
        return std::make_unique<CastExpr>(ck, std::move(type), std::move(expr), l);
    }

    NodePtr parse_postfix_expr() {
        SourceLocation l = loc();
        auto e = parse_primary_expr();

        while (true) {
            if (check(TokenKind::Dot)) {
                advance();
                std::string m;
                if (check(TokenKind::Identifier)) m = advance().value;
                else if (check(TokenKind::KwTemplate)) { advance(); if (check(TokenKind::Identifier)) m = advance().value; }
                e = std::make_unique<MemberExpr>(std::move(e), m, false, l);
            } else if (check(TokenKind::Arrow)) {
                advance();
                std::string m;
                if (check(TokenKind::Identifier)) m = advance().value;
                else if (check(TokenKind::KwTemplate)) { advance(); if (check(TokenKind::Identifier)) m = advance().value; }
                e = std::make_unique<MemberExpr>(std::move(e), m, true, l);
            } else if (check(TokenKind::LBracket)) {
                advance();
                auto idx = parse_assignment_expr();
                expect(TokenKind::RBracket);
                e = std::make_unique<IndexExpr>(std::move(e), std::move(idx), l);
            } else if (check(TokenKind::LParen)) {
                advance();
                auto call = std::make_unique<CallExpr>(std::move(e), l);
                while (!check(TokenKind::RParen) && !at_eof()) {
                    call->args.push_back(parse_assignment_expr());
                    if (!match(TokenKind::Comma)) break;
                }
                expect(TokenKind::RParen);
                e = std::move(call);
            } else if (check(TokenKind::PlusPlus)) {
                advance();
                e = std::make_unique<UnaryExpr>(TokenKind::PlusPlus, std::move(e), true, l);
            } else if (check(TokenKind::MinusMinus)) {
                advance();
                e = std::make_unique<UnaryExpr>(TokenKind::MinusMinus, std::move(e), true, l);
            } else {
                break;
            }
            l = loc();
        }
        return e;
    }

    NodePtr parse_primary_expr() {
        SourceLocation l = loc();
        switch (cur().kind) {
            case TokenKind::IntLiteral: {
                auto t = advance();
                auto n = std::make_unique<IntLiteralExpr>(t.int_val, t.value, t.loc);
                n->is_unsigned  = t.is_unsigned;
                n->is_long      = t.is_long;
                n->is_long_long = t.is_long_long;
                return n;
            }
            case TokenKind::FloatLiteral: {
                auto t = advance();
                return std::make_unique<FloatLiteralExpr>(t.float_val, t.value, t.is_float_sfx, t.loc);
            }
            case TokenKind::StringLiteral: {
                // Adjacent string literals are concatenated
                std::string raw = cur().value;
                std::string cooked = decode_string(cur().value);
                advance();
                while (check(TokenKind::StringLiteral)) {
                    raw   += " " + cur().value;
                    cooked += decode_string(cur().value);
                    advance();
                }
                return std::make_unique<StringLiteralExpr>(raw, cooked, l);
            }
            case TokenKind::CharLiteral: {
                auto t = advance();
                int32_t cv = (t.value.size() >= 3) ? (int32_t)t.value[1] : 0;
                return std::make_unique<CharLiteralExpr>(t.value, cv, l);
            }
            case TokenKind::BoolLiteral: {
                auto t = advance();
                return std::make_unique<BoolLiteralExpr>(t.int_val != 0, l);
            }
            case TokenKind::NullptrLiteral:
                advance();
                return std::make_unique<NullptrExpr>(l);
            case TokenKind::KwThis:
                advance();
                return std::make_unique<ThisExpr>(l);

            case TokenKind::LParen: {
                // Either (expr) or C-style cast (type)expr
                advance();
                if (is_type_start_strict()) {
                    // Peek ahead: if we see ) followed by non-operator, it's a cast
                    NodePtr type = parse_type_full();
                    if (check(TokenKind::RParen)) {
                        advance();
                        // C-style cast
                        auto operand = parse_unary_expr();
                        return std::make_unique<CastExpr>(
                            CastKind::C_Style, std::move(type), std::move(operand), l);
                    }
                    // Not a cast – put the type node back somehow
                    // Fallback: emit 0 literal (best effort)
                    expect(TokenKind::RParen);
                    return std::make_unique<IntLiteralExpr>(0, "0", l);
                }
                auto inner = parse_expr();
                expect(TokenKind::RParen);
                return std::make_unique<ParenExpr>(std::move(inner), l);
            }

            case TokenKind::LBrace:
                return parse_initialiser_list_expr();

            case TokenKind::KwTrue:
                advance();
                return std::make_unique<BoolLiteralExpr>(true, l);
            case TokenKind::KwFalse:
                advance();
                return std::make_unique<BoolLiteralExpr>(false, l);
            case TokenKind::KwNullptr:
                advance();
                return std::make_unique<NullptrExpr>(l);

            case TokenKind::KwStaticCast: case TokenKind::KwDynamicCast:
            case TokenKind::KwReinterpretCast: case TokenKind::KwConstCast:
                return parse_named_cast(l);

            // Lambda
            case TokenKind::LBracket:
                return parse_lambda_expr(l);

            // Qualified name / template instantiation call
            case TokenKind::DoubleColon:
            case TokenKind::Identifier: {
                std::vector<std::string> scope;
                match(TokenKind::DoubleColon);  // global ::
                std::string name = "";
                if (check(TokenKind::Identifier)) name = advance().value;

                while (check(TokenKind::DoubleColon)) {
                    advance();
                    scope.push_back(name);
                    if (check(TokenKind::Identifier)) name = advance().value;
                    else break;
                }

                if (!scope.empty()) {
                    // Check for template arguments on the last name
                    if (check(TokenKind::Lt)) {
                        // might be template args – try to parse
                        size_t saved = pos_;
                        advance();
                        bool ok = try_parse_template_args();
                        if (!ok) { pos_ = saved; }
                    }
                    return std::make_unique<ScopeExpr>(std::move(scope), name, l);
                }

                // Plain identifier
                // Could be followed by template arguments for a free function call
                return std::make_unique<IdentifierExpr>(name, l);
            }

            // Keyword types used as function names (e.g., void()): skip
            default: {
                // Unknown – produce an error and return dummy
                if (!at_eof()) {
                    error("unexpected token '" + cur().kindName() + "' in expression");
                    advance();
                }
                return std::make_unique<IntLiteralExpr>(0, "0", l);
            }
        }
    }

    // Try to skip template argument list <...>; returns true if balanced
    bool try_parse_template_args() {
        int depth = 1;
        while (!at_eof() && depth > 0) {
            if (check(TokenKind::Lt)) depth++;
            else if (check(TokenKind::Gt)) { depth--; if (depth == 0) { advance(); return true; } }
            else if (check(TokenKind::RShift)) { depth -= 2; if (depth <= 0) { advance(); return true; } }
            else if (check(TokenKind::Semicolon) || check(TokenKind::LBrace)) return false;
            advance();
        }
        return depth == 0;
    }

    NodePtr parse_initialiser_list_expr() {
        SourceLocation l = loc();
        expect(TokenKind::LBrace);
        auto il = std::make_unique<InitListExpr>(l);
        while (!check(TokenKind::RBrace) && !at_eof()) {
            il->elements.push_back(parse_assignment_expr());
            if (!match(TokenKind::Comma)) break;
            if (check(TokenKind::RBrace)) break;  // trailing comma
        }
        expect(TokenKind::RBrace);
        return il;
    }

    NodePtr parse_lambda_expr(SourceLocation l) {
        auto lam = std::make_unique<LambdaExpr>(l);
        expect(TokenKind::LBracket);

        // Capture list
        if (check(TokenKind::Assign))      { advance(); lam->capture_default = LambdaExpr::CaptureDefault::ByCopy; }
        else if (check(TokenKind::Amp))    { advance(); lam->capture_default = LambdaExpr::CaptureDefault::ByRef; }
        while (!check(TokenKind::RBracket) && !at_eof()) {
            LambdaExpr::Capture cap;
            if (check(TokenKind::Comma)) { advance(); continue; }
            if (check(TokenKind::Amp)) { advance(); cap.by_ref = true; }
            if (check(TokenKind::KwThis)) { advance(); cap.is_this = true; }
            else if (check(TokenKind::Identifier)) cap.name = advance().value;
            lam->captures.push_back(std::move(cap));
        }
        expect(TokenKind::RBracket);

        // Parameters (optional)
        if (check(TokenKind::LParen)) {
            advance();
            parse_param_list(lam->params);
            expect(TokenKind::RParen);
        }

        if (match(TokenKind::KwMutable)) lam->is_mutable = true;

        // Trailing return type
        if (check(TokenKind::Arrow)) {
            advance();
            lam->return_type = parse_type_full();
        }

        lam->body = parse_compound_stmt();
        return lam;
    }

    // ── String decoding ──────────────────────────────────────────
    static std::string decode_string(const std::string& raw) {
        // Strip prefix and quotes; unescape escape sequences
        size_t start = raw.find('"');
        if (start == std::string::npos) return raw;
        size_t end = raw.rfind('"');
        if (end == start) return "";
        std::string s;
        for (size_t i = start + 1; i < end; i++) {
            if (raw[i] == '\\' && i + 1 < end) {
                i++;
                switch (raw[i]) {
                    case 'n':  s += '\n'; break;
                    case 't':  s += '\t'; break;
                    case 'r':  s += '\r'; break;
                    case '\\': s += '\\'; break;
                    case '"':  s += '"';  break;
                    case '\'': s += '\''; break;
                    case '0':  s += '\0'; break;
                    default:   s += raw[i]; break;
                }
            } else {
                s += raw[i];
            }
        }
        return s;
    }
};

// ─────────────────────────────────────────────────────────────────
//  Convenience wrapper
// ─────────────────────────────────────────────────────────────────
inline std::unique_ptr<TranslationUnit>
parse_translation_unit(const std::string& source,
                        const std::string& filename,
                        std::vector<Diagnostic>& out_diags) {
    Lexer lexer(source, filename);
    auto  tokens = lexer.tokenize();
    // Forward lexer errors as diagnostics
    for (auto& e : lexer.errors())
        out_diags.push_back({Diagnostic::Severity::Error, e, {}});

    Parser parser(std::move(tokens));
    auto   tu = parser.parse();
    for (auto& d : parser.diagnostics()) out_diags.push_back(d);
    return tu;
}

} // namespace rexc
