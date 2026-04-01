#pragma once
/*
 * ast.hpp  –  REXC Abstract Syntax Tree
 *
 * Node hierarchy for C++17 programs.  Every node is heap-allocated
 * and owned via std::unique_ptr<Node>.
 *
 * Visitor pattern: derive from ASTVisitor and call node->accept(*this).
 *
 * Design choices:
 *   • Discriminated union via NodeKind enum + virtual dispatch
 *   • Leaf types stored by value in the appropriate subclass
 *   • Children owned by their parent via NodePtr (unique_ptr<Node>)
 *   • SourceLocation carried on every node for diagnostics
 */

#include "token.hpp"
#include <vector>
#include <string>
#include <memory>
#include <optional>

namespace rexc {

// ── Forward declarations ─────────────────────────────────────────
struct Node;
using NodePtr = std::unique_ptr<Node>;

// ─────────────────────────────────────────────────────────────────
//  NodeKind  –  discriminator for every AST node type
// ─────────────────────────────────────────────────────────────────
enum class NodeKind {
    // ── Translation unit ─────────────────────────────────────────
    TranslationUnit,

    // ── Declarations ─────────────────────────────────────────────
    PreprocessorDecl,   // #include / #define / ...
    NamespaceDecl,
    UsingDecl,          // using std::cout;  or  using T = int;
    TypedefDecl,
    ClassDecl,
    StructDecl,
    UnionDecl,
    EnumDecl,
    FunctionDecl,
    ConstructorDecl,
    DestructorDecl,
    VarDecl,
    FieldDecl,
    ParamDecl,
    TemplateDecl,
    FriendDecl,
    StaticAssertDecl,

    // ── Type nodes ───────────────────────────────────────────────
    PrimitiveType,      // int, float, void, bool, ...
    NamedType,          // user-defined name
    QualifiedType,      // const T, volatile T
    PointerType,        // T*
    ReferenceType,      // T&
    RValueRefType,      // T&&
    ArrayType,          // T[N]
    FunctionType,       // R(Args...)
    TemplateInstType,   // vector<int>

    // ── Statements ───────────────────────────────────────────────
    CompoundStmt,
    IfStmt,
    ForStmt,
    RangeForStmt,
    WhileStmt,
    DoWhileStmt,
    SwitchStmt,
    CaseStmt,
    DefaultStmt,
    ReturnStmt,
    BreakStmt,
    ContinueStmt,
    GotoStmt,
    LabelStmt,
    ExprStmt,
    DeclStmt,
    TryStmt,
    CatchClause,
    ThrowStmt,
    NullStmt,

    // ── Expressions ──────────────────────────────────────────────
    IntLiteralExpr,
    FloatLiteralExpr,
    StringLiteralExpr,
    CharLiteralExpr,
    BoolLiteralExpr,
    NullptrExpr,
    IdentifierExpr,
    BinaryExpr,
    UnaryExpr,
    TernaryExpr,
    CallExpr,
    MemberExpr,         // a.b  a->b
    ScopeExpr,          // A::b
    IndexExpr,          // a[i]
    CastExpr,           // static_cast<T>(e), (T)e
    SizeofExpr,
    NewExpr,
    DeleteExpr,
    AssignExpr,
    InitListExpr,       // { a, b, c }
    LambdaExpr,
    ThisExpr,
    ParenExpr,
    CommaExpr,

    // ── Access specifier (inside class body) ─────────────────────
    AccessSpecifier,
};

// ─────────────────────────────────────────────────────────────────
//  Base Node
// ─────────────────────────────────────────────────────────────────
struct ASTVisitor;

struct Node {
    NodeKind       kind;
    SourceLocation loc;

    explicit Node(NodeKind k, SourceLocation l = {})
        : kind(k), loc(std::move(l)) {}

    virtual ~Node() = default;
    virtual void accept(ASTVisitor&) = 0;

    // Convenience cast (no run-time type-check overhead for release builds)
    template<typename T> T* as() { return static_cast<T*>(this); }
    template<typename T> const T* as() const { return static_cast<const T*>(this); }
};

// ─────────────────────────────────────────────────────────────────
//  Visitor base (forward-declared; implementations in codegen etc.)
// ─────────────────────────────────────────────────────────────────
struct ASTVisitor {
    virtual ~ASTVisitor() = default;
#define VISIT(T) virtual void visit(T&) {}
    // (All concrete visit overloads default to no-op so subclasses
    //  only override what they care about.)
    VISIT(struct TranslationUnit)
    VISIT(struct PreprocessorDecl)
    VISIT(struct NamespaceDecl)
    VISIT(struct UsingDecl)
    VISIT(struct TypedefDecl)
    VISIT(struct ClassDecl)
    VISIT(struct EnumDecl)
    VISIT(struct FunctionDecl)
    VISIT(struct ConstructorDecl)
    VISIT(struct DestructorDecl)
    VISIT(struct VarDecl)
    VISIT(struct FieldDecl)
    VISIT(struct ParamDecl)
    VISIT(struct TemplateDecl)
    VISIT(struct StaticAssertDecl)
    VISIT(struct PrimitiveType)
    VISIT(struct NamedType)
    VISIT(struct QualifiedType)
    VISIT(struct PointerType)
    VISIT(struct ReferenceType)
    VISIT(struct RValueRefType)
    VISIT(struct ArrayType)
    VISIT(struct FunctionType)
    VISIT(struct TemplateInstType)
    VISIT(struct FriendDecl)
    VISIT(struct CompoundStmt)
    VISIT(struct IfStmt)
    VISIT(struct ForStmt)
    VISIT(struct RangeForStmt)
    VISIT(struct WhileStmt)
    VISIT(struct DoWhileStmt)
    VISIT(struct SwitchStmt)
    VISIT(struct CaseStmt)
    VISIT(struct DefaultStmt)
    VISIT(struct ReturnStmt)
    VISIT(struct BreakStmt)
    VISIT(struct ContinueStmt)
    VISIT(struct GotoStmt)
    VISIT(struct LabelStmt)
    VISIT(struct ExprStmt)
    VISIT(struct DeclStmt)
    VISIT(struct TryStmt)
    VISIT(struct CatchClause)
    VISIT(struct ThrowStmt)
    VISIT(struct NullStmt)
    VISIT(struct IntLiteralExpr)
    VISIT(struct FloatLiteralExpr)
    VISIT(struct StringLiteralExpr)
    VISIT(struct CharLiteralExpr)
    VISIT(struct BoolLiteralExpr)
    VISIT(struct NullptrExpr)
    VISIT(struct IdentifierExpr)
    VISIT(struct BinaryExpr)
    VISIT(struct UnaryExpr)
    VISIT(struct TernaryExpr)
    VISIT(struct CallExpr)
    VISIT(struct MemberExpr)
    VISIT(struct ScopeExpr)
    VISIT(struct IndexExpr)
    VISIT(struct CastExpr)
    VISIT(struct SizeofExpr)
    VISIT(struct NewExpr)
    VISIT(struct DeleteExpr)
    VISIT(struct AssignExpr)
    VISIT(struct InitListExpr)
    VISIT(struct LambdaExpr)
    VISIT(struct ThisExpr)
    VISIT(struct ParenExpr)
    VISIT(struct CommaExpr)
    VISIT(struct AccessSpecifier)
#undef VISIT
};

// ─────────────────────────────────────────────────────────────────
//  Helper macro to emit accept() in every concrete node
// ─────────────────────────────────────────────────────────────────
#define REXC_ACCEPT() void accept(ASTVisitor& v) override { v.visit(*this); }

// ─────────────────────────────────────────────────────────────────
//  Type qualifiers  (bit-field)
// ─────────────────────────────────────────────────────────────────
struct TypeQuals {
    bool is_const    = false;
    bool is_volatile = false;
    bool is_restrict = false;
    bool is_mutable  = false;
};

// ─────────────────────────────────────────────────────────────────
//  Type nodes
// ─────────────────────────────────────────────────────────────────

struct PrimitiveType : Node {
    enum class Kind {
        Void, Bool,
        Char, SChar, UChar, WChar,
        Short, UShort,
        Int, UInt,
        Long, ULong,
        LongLong, ULongLong,
        Float, Double, LongDouble,
        Auto, SizeT,
        Nullptr_t,
    };
    Kind prim_kind;
    explicit PrimitiveType(Kind k, SourceLocation l = {})
        : Node(NodeKind::PrimitiveType, l), prim_kind(k) {}
    REXC_ACCEPT()
};

struct NamedType : Node {
    std::string name;                       // e.g. "Animal"
    std::vector<std::string> scope;         // e.g. ["std"] for std::string
    explicit NamedType(std::vector<std::string> sc, std::string n, SourceLocation l = {})
        : Node(NodeKind::NamedType, l), name(std::move(n)), scope(std::move(sc)) {}
    REXC_ACCEPT()

    std::string qualified_name() const {
        std::string r;
        for (auto& s : scope) r += s + "::";
        return r + name;
    }
};

struct QualifiedType : Node {
    NodePtr inner;
    TypeQuals quals;
    explicit QualifiedType(NodePtr inner_, TypeQuals q, SourceLocation l = {})
        : Node(NodeKind::QualifiedType, l), inner(std::move(inner_)), quals(q) {}
    REXC_ACCEPT()
};

struct PointerType : Node {
    NodePtr pointee;
    TypeQuals quals;    // qualifiers on the pointer itself (T* const)
    explicit PointerType(NodePtr pt, TypeQuals q = {}, SourceLocation l = {})
        : Node(NodeKind::PointerType, l), pointee(std::move(pt)), quals(q) {}
    REXC_ACCEPT()
};

struct ReferenceType : Node {
    NodePtr referee;
    explicit ReferenceType(NodePtr r, SourceLocation l = {})
        : Node(NodeKind::ReferenceType, l), referee(std::move(r)) {}
    REXC_ACCEPT()
};

struct RValueRefType : Node {
    NodePtr referee;
    explicit RValueRefType(NodePtr r, SourceLocation l = {})
        : Node(NodeKind::RValueRefType, l), referee(std::move(r)) {}
    REXC_ACCEPT()
};

struct ArrayType : Node {
    NodePtr element;
    NodePtr size_expr;      // may be nullptr (unknown size)
    explicit ArrayType(NodePtr el, NodePtr sz, SourceLocation l = {})
        : Node(NodeKind::ArrayType, l), element(std::move(el)), size_expr(std::move(sz)) {}
    REXC_ACCEPT()
};

struct TemplateInstType : Node {
    std::vector<std::string> scope;
    std::string              name;           // e.g. "vector"
    std::vector<NodePtr>     args;           // type / expression arguments
    explicit TemplateInstType(std::vector<std::string> sc, std::string n,
                               std::vector<NodePtr> a, SourceLocation l = {})
        : Node(NodeKind::TemplateInstType, l),
          scope(std::move(sc)), name(std::move(n)), args(std::move(a)) {}
    REXC_ACCEPT()

    std::string qualified_name() const {
        std::string r;
        for (auto& s : scope) r += s + "::";
        return r + name;
    }
};

// ─────────────────────────────────────────────────────────────────
//  Declaration helpers
// ─────────────────────────────────────────────────────────────────

// A single declarator: name + optional initialiser
struct Declarator {
    std::string name;
    NodePtr     init;       // initialiser expression (may be nullptr)
};

// Base class/interface in inheritance list
struct BaseClass {
    std::string    name;
    std::vector<std::string> scope;
    bool           is_virtual = false;
    TokenKind      access = TokenKind::KwPublic;  // public/protected/private
};

// Function parameter
struct ParamDecl : Node {
    NodePtr     type;
    std::string name;           // may be empty (unnamed parameter)
    NodePtr     default_val;    // may be nullptr
    bool        is_variadic = false;  // for '...'
    explicit ParamDecl(NodePtr t, std::string n, NodePtr dv, SourceLocation l = {})
        : Node(NodeKind::ParamDecl, l),
          type(std::move(t)), name(std::move(n)), default_val(std::move(dv)) {}
    REXC_ACCEPT()
};

// ─────────────────────────────────────────────────────────────────
//  Top-level declarations
// ─────────────────────────────────────────────────────────────────

struct TranslationUnit : Node {
    std::vector<NodePtr> decls;
    explicit TranslationUnit(SourceLocation l = {})
        : Node(NodeKind::TranslationUnit, l) {}
    REXC_ACCEPT()
};

struct PreprocessorDecl : Node {
    std::string directive;   // entire #... line (without leading #)
    explicit PreprocessorDecl(std::string d, SourceLocation l = {})
        : Node(NodeKind::PreprocessorDecl, l), directive(std::move(d)) {}
    REXC_ACCEPT()
};

struct NamespaceDecl : Node {
    std::string          name;       // may be empty for anonymous namespace
    bool                 is_inline = false;
    std::vector<NodePtr> decls;
    explicit NamespaceDecl(std::string n, SourceLocation l = {})
        : Node(NodeKind::NamespaceDecl, l), name(std::move(n)) {}
    REXC_ACCEPT()
};

struct UsingDecl : Node {
    enum class Form { Directive, Declaration, Alias };
    Form        form       = Form::Declaration;
    std::string name;                    // alias name for Form::Alias
    std::vector<std::string> scope_path; // namespace path
    NodePtr     type_alias;              // for  using T = SomeType;
    explicit UsingDecl(SourceLocation l = {})
        : Node(NodeKind::UsingDecl, l) {}
    REXC_ACCEPT()
};

struct TypedefDecl : Node {
    std::string name;
    NodePtr     type;
    explicit TypedefDecl(std::string n, NodePtr t, SourceLocation l = {})
        : Node(NodeKind::TypedefDecl, l), name(std::move(n)), type(std::move(t)) {}
    REXC_ACCEPT()
};

struct EnumDecl : Node {
    std::string name;
    bool        is_class   = false;    // enum class
    NodePtr     base_type;             // : int  (may be nullptr)
    struct Enumerator {
        std::string name;
        NodePtr     value;  // may be nullptr
    };
    std::vector<Enumerator> enumerators;
    explicit EnumDecl(std::string n, SourceLocation l = {})
        : Node(NodeKind::EnumDecl, l), name(std::move(n)) {}
    REXC_ACCEPT()
};

struct FunctionDecl : Node {
    std::string              name;
    NodePtr                  return_type;
    std::vector<NodePtr>     params;      // ParamDecl nodes
    NodePtr                  body;        // CompoundStmt or nullptr (declaration only)
    std::vector<std::string> scope;       // qualified name prefix

    // Qualifiers
    bool is_static   = false;
    bool is_inline   = false;
    bool is_virtual  = false;
    bool is_override = false;
    bool is_final    = false;
    bool is_const    = false;   // member function const
    bool is_volatile = false;
    bool is_noexcept = false;
    bool is_explicit = false;
    bool is_pure_virtual = false;  // = 0
    bool is_deleted  = false;      // = delete
    bool is_defaulted= false;      // = default
    bool is_constexpr= false;
    bool is_extern   = false;
    bool is_operator = false;      // operator+  etc.
    std::string operator_token;    // "+", "<<", etc. for operator overloads

    // Constructor initialiser list (for constructors)
    struct MemberInit {
        std::string  name;
        std::vector<NodePtr> args;
    };
    std::vector<MemberInit> init_list;

    explicit FunctionDecl(std::string n, SourceLocation l = {})
        : Node(NodeKind::FunctionDecl, l), name(std::move(n)) {}
    REXC_ACCEPT()
};

// Constructor is a FunctionDecl with special NodeKind for easy identification
struct ConstructorDecl : FunctionDecl {
    explicit ConstructorDecl(std::string class_name, SourceLocation l = {})
        : FunctionDecl(class_name, l) { kind = NodeKind::ConstructorDecl; }
    REXC_ACCEPT()
};

struct DestructorDecl : FunctionDecl {
    explicit DestructorDecl(std::string class_name, SourceLocation l = {})
        : FunctionDecl("~" + class_name, l) { kind = NodeKind::DestructorDecl; }
    REXC_ACCEPT()
};

struct ClassDecl : Node {
    std::string              name;
    bool                     is_struct   = false;   // struct vs class
    bool                     is_union    = false;
    std::vector<BaseClass>   bases;
    std::vector<NodePtr>     members;    // FieldDecl / FunctionDecl / AccessSpecifier / ...
    bool                     is_complete = false;   // true if body was parsed

    explicit ClassDecl(std::string n, SourceLocation l = {})
        : Node(NodeKind::ClassDecl, l), name(std::move(n)) {}
    REXC_ACCEPT()
};

struct FieldDecl : Node {
    std::string  name;
    NodePtr      type;
    NodePtr      bit_width;    // for bitfields; nullptr otherwise
    NodePtr      init;         // in-class initialiser; nullptr otherwise
    bool         is_static   = false;
    bool         is_mutable  = false;
    bool         is_constexpr= false;
    explicit FieldDecl(std::string n, NodePtr t, SourceLocation l = {})
        : Node(NodeKind::FieldDecl, l), name(std::move(n)), type(std::move(t)) {}
    REXC_ACCEPT()
};

struct VarDecl : Node {
    std::string name;
    NodePtr     type;
    NodePtr     init;
    bool        is_const    = false;
    bool        is_constexpr= false;
    bool        is_static   = false;
    bool        is_extern   = false;
    bool        is_auto     = false;    // type was 'auto'
    explicit VarDecl(std::string n, NodePtr t, NodePtr iv, SourceLocation l = {})
        : Node(NodeKind::VarDecl, l),
          name(std::move(n)), type(std::move(t)), init(std::move(iv)) {}
    REXC_ACCEPT()
};

struct TemplateDecl : Node {
    struct TypeParam {
        std::string name;
        NodePtr     default_type;  // may be nullptr
        bool        is_typename = true;  // true: typename T, false: non-type
        NodePtr     non_type_type;       // for non-type params
    };
    std::vector<TypeParam> params;
    NodePtr                body;        // ClassDecl or FunctionDecl
    explicit TemplateDecl(SourceLocation l = {}) : Node(NodeKind::TemplateDecl, l) {}
    REXC_ACCEPT()
};

struct StaticAssertDecl : Node {
    NodePtr     condition;
    std::string message;
    explicit StaticAssertDecl(NodePtr cond, std::string msg, SourceLocation l = {})
        : Node(NodeKind::StaticAssertDecl, l),
          condition(std::move(cond)), message(std::move(msg)) {}
    REXC_ACCEPT()
};

struct AccessSpecifier : Node {
    TokenKind spec;  // KwPublic / KwProtected / KwPrivate
    explicit AccessSpecifier(TokenKind s, SourceLocation l = {})
        : Node(NodeKind::AccessSpecifier, l), spec(s) {}
    REXC_ACCEPT()
};

struct FriendDecl : Node {
    NodePtr decl;
    explicit FriendDecl(NodePtr d, SourceLocation l = {})
        : Node(NodeKind::FriendDecl, l), decl(std::move(d)) {}
    REXC_ACCEPT()
};

// ─────────────────────────────────────────────────────────────────
//  Statement nodes
// ─────────────────────────────────────────────────────────────────

struct CompoundStmt : Node {
    std::vector<NodePtr> stmts;
    explicit CompoundStmt(SourceLocation l = {}) : Node(NodeKind::CompoundStmt, l) {}
    REXC_ACCEPT()
};

struct IfStmt : Node {
    NodePtr condition;
    NodePtr then_branch;
    NodePtr else_branch;    // may be nullptr
    NodePtr init_stmt;      // C++17: if (init; cond) – may be nullptr
    bool    is_constexpr = false;
    explicit IfStmt(SourceLocation l = {}) : Node(NodeKind::IfStmt, l) {}
    REXC_ACCEPT()
};

struct ForStmt : Node {
    NodePtr init;       // VarDecl or ExprStmt; may be nullptr
    NodePtr condition;  // may be nullptr
    NodePtr increment;  // may be nullptr
    NodePtr body;
    explicit ForStmt(SourceLocation l = {}) : Node(NodeKind::ForStmt, l) {}
    REXC_ACCEPT()
};

struct RangeForStmt : Node {
    // for (T var : range) body
    NodePtr range_decl;     // VarDecl node (type + name)
    NodePtr range_expr;     // the range expression
    NodePtr body;
    explicit RangeForStmt(SourceLocation l = {}) : Node(NodeKind::RangeForStmt, l) {}
    REXC_ACCEPT()
};

struct WhileStmt : Node {
    NodePtr condition;
    NodePtr body;
    explicit WhileStmt(SourceLocation l = {}) : Node(NodeKind::WhileStmt, l) {}
    REXC_ACCEPT()
};

struct DoWhileStmt : Node {
    NodePtr body;
    NodePtr condition;
    explicit DoWhileStmt(SourceLocation l = {}) : Node(NodeKind::DoWhileStmt, l) {}
    REXC_ACCEPT()
};

struct SwitchStmt : Node {
    NodePtr              control;
    std::vector<NodePtr> cases;
    explicit SwitchStmt(SourceLocation l = {}) : Node(NodeKind::SwitchStmt, l) {}
    REXC_ACCEPT()
};

struct CaseStmt : Node {
    NodePtr              value;    // expression
    std::vector<NodePtr> stmts;
    explicit CaseStmt(NodePtr v, SourceLocation l = {})
        : Node(NodeKind::CaseStmt, l), value(std::move(v)) {}
    REXC_ACCEPT()
};

struct DefaultStmt : Node {
    std::vector<NodePtr> stmts;
    explicit DefaultStmt(SourceLocation l = {}) : Node(NodeKind::DefaultStmt, l) {}
    REXC_ACCEPT()
};

struct ReturnStmt : Node {
    NodePtr value;   // may be nullptr (void return)
    explicit ReturnStmt(NodePtr v, SourceLocation l = {})
        : Node(NodeKind::ReturnStmt, l), value(std::move(v)) {}
    REXC_ACCEPT()
};

struct BreakStmt    : Node { explicit BreakStmt(SourceLocation l={}) : Node(NodeKind::BreakStmt,l){} REXC_ACCEPT() };
struct ContinueStmt : Node { explicit ContinueStmt(SourceLocation l={}) : Node(NodeKind::ContinueStmt,l){} REXC_ACCEPT() };
struct NullStmt     : Node { explicit NullStmt(SourceLocation l={}) : Node(NodeKind::NullStmt,l){} REXC_ACCEPT() };

struct GotoStmt : Node {
    std::string label;
    explicit GotoStmt(std::string lbl, SourceLocation l = {})
        : Node(NodeKind::GotoStmt, l), label(std::move(lbl)) {}
    REXC_ACCEPT()
};

struct LabelStmt : Node {
    std::string label;
    NodePtr     stmt;
    explicit LabelStmt(std::string lbl, NodePtr s, SourceLocation l = {})
        : Node(NodeKind::LabelStmt, l), label(std::move(lbl)), stmt(std::move(s)) {}
    REXC_ACCEPT()
};

struct ExprStmt : Node {
    NodePtr expr;
    explicit ExprStmt(NodePtr e, SourceLocation l = {})
        : Node(NodeKind::ExprStmt, l), expr(std::move(e)) {}
    REXC_ACCEPT()
};

struct DeclStmt : Node {
    std::vector<NodePtr> decls;
    explicit DeclStmt(SourceLocation l = {}) : Node(NodeKind::DeclStmt, l) {}
    REXC_ACCEPT()
};

struct TryStmt : Node {
    NodePtr              try_body;
    std::vector<NodePtr> catches;    // CatchClause nodes
    explicit TryStmt(SourceLocation l = {}) : Node(NodeKind::TryStmt, l) {}
    REXC_ACCEPT()
};

struct CatchClause : Node {
    NodePtr param;    // ParamDecl or nullptr (catch(...))
    NodePtr body;
    bool    is_ellipsis = false;
    explicit CatchClause(SourceLocation l = {}) : Node(NodeKind::CatchClause, l) {}
    REXC_ACCEPT()
};

struct ThrowStmt : Node {
    NodePtr expr;    // may be nullptr (rethrow)
    explicit ThrowStmt(NodePtr e, SourceLocation l = {})
        : Node(NodeKind::ThrowStmt, l), expr(std::move(e)) {}
    REXC_ACCEPT()
};

// ─────────────────────────────────────────────────────────────────
//  Expression nodes
// ─────────────────────────────────────────────────────────────────

struct IntLiteralExpr : Node {
    uint64_t value;
    bool     is_unsigned  = false;
    bool     is_long      = false;
    bool     is_long_long = false;
    std::string raw;
    explicit IntLiteralExpr(uint64_t v, std::string r, SourceLocation l = {})
        : Node(NodeKind::IntLiteralExpr, l), value(v), raw(std::move(r)) {}
    REXC_ACCEPT()
};

struct FloatLiteralExpr : Node {
    double      value;
    bool        is_float = false;
    std::string raw;
    explicit FloatLiteralExpr(double v, std::string r, bool f, SourceLocation l = {})
        : Node(NodeKind::FloatLiteralExpr, l), value(v), raw(std::move(r)), is_float(f) {}
    REXC_ACCEPT()
};

struct StringLiteralExpr : Node {
    std::string value;    // raw source (including quotes and prefix)
    std::string cooked;   // decoded string content
    explicit StringLiteralExpr(std::string raw, std::string cooked_val, SourceLocation l = {})
        : Node(NodeKind::StringLiteralExpr, l),
          value(std::move(raw)), cooked(std::move(cooked_val)) {}
    REXC_ACCEPT()
};

struct CharLiteralExpr : Node {
    std::string raw;
    int32_t     char_val = 0;
    explicit CharLiteralExpr(std::string r, int32_t v, SourceLocation l = {})
        : Node(NodeKind::CharLiteralExpr, l), raw(std::move(r)), char_val(v) {}
    REXC_ACCEPT()
};

struct BoolLiteralExpr : Node {
    bool value;
    explicit BoolLiteralExpr(bool v, SourceLocation l = {})
        : Node(NodeKind::BoolLiteralExpr, l), value(v) {}
    REXC_ACCEPT()
};

struct NullptrExpr : Node {
    explicit NullptrExpr(SourceLocation l = {}) : Node(NodeKind::NullptrExpr, l) {}
    REXC_ACCEPT()
};

struct ThisExpr : Node {
    explicit ThisExpr(SourceLocation l = {}) : Node(NodeKind::ThisExpr, l) {}
    REXC_ACCEPT()
};

struct IdentifierExpr : Node {
    std::string name;
    explicit IdentifierExpr(std::string n, SourceLocation l = {})
        : Node(NodeKind::IdentifierExpr, l), name(std::move(n)) {}
    REXC_ACCEPT()
};

struct BinaryExpr : Node {
    TokenKind op;
    NodePtr   left;
    NodePtr   right;
    explicit BinaryExpr(TokenKind o, NodePtr l, NodePtr r, SourceLocation loc = {})
        : Node(NodeKind::BinaryExpr, loc), op(o),
          left(std::move(l)), right(std::move(r)) {}
    REXC_ACCEPT()
};

struct AssignExpr : Node {
    TokenKind op;     // Assign, PlusAssign, etc.
    NodePtr   target;
    NodePtr   value;
    explicit AssignExpr(TokenKind o, NodePtr tgt, NodePtr val, SourceLocation loc = {})
        : Node(NodeKind::AssignExpr, loc), op(o),
          target(std::move(tgt)), value(std::move(val)) {}
    REXC_ACCEPT()
};

struct UnaryExpr : Node {
    TokenKind op;
    NodePtr   operand;
    bool      is_postfix = false;
    explicit UnaryExpr(TokenKind o, NodePtr e, bool post, SourceLocation loc = {})
        : Node(NodeKind::UnaryExpr, loc), op(o),
          operand(std::move(e)), is_postfix(post) {}
    REXC_ACCEPT()
};

struct TernaryExpr : Node {
    NodePtr condition;
    NodePtr then_expr;
    NodePtr else_expr;
    explicit TernaryExpr(NodePtr c, NodePtr t, NodePtr e, SourceLocation loc = {})
        : Node(NodeKind::TernaryExpr, loc),
          condition(std::move(c)), then_expr(std::move(t)), else_expr(std::move(e)) {}
    REXC_ACCEPT()
};

struct CallExpr : Node {
    NodePtr              callee;
    std::vector<NodePtr> args;
    std::vector<NodePtr> template_args;  // may be empty
    explicit CallExpr(NodePtr callee_, SourceLocation loc = {})
        : Node(NodeKind::CallExpr, loc), callee(std::move(callee_)) {}
    REXC_ACCEPT()
};

struct MemberExpr : Node {
    NodePtr     object;
    std::string member;
    bool        is_arrow = false;   // true: obj->member, false: obj.member
    explicit MemberExpr(NodePtr obj, std::string m, bool arrow, SourceLocation loc = {})
        : Node(NodeKind::MemberExpr, loc),
          object(std::move(obj)), member(std::move(m)), is_arrow(arrow) {}
    REXC_ACCEPT()
};

struct ScopeExpr : Node {
    std::vector<std::string> scope;   // e.g. {"std"}
    std::string              name;    // e.g. "cout"
    explicit ScopeExpr(std::vector<std::string> sc, std::string n, SourceLocation loc = {})
        : Node(NodeKind::ScopeExpr, loc), scope(std::move(sc)), name(std::move(n)) {}
    REXC_ACCEPT()

    std::string qualified() const {
        std::string r;
        for (auto& s : scope) r += s + "::";
        return r + name;
    }
};

struct IndexExpr : Node {
    NodePtr object;
    NodePtr index;
    explicit IndexExpr(NodePtr obj, NodePtr idx, SourceLocation loc = {})
        : Node(NodeKind::IndexExpr, loc), object(std::move(obj)), index(std::move(idx)) {}
    REXC_ACCEPT()
};

enum class CastKind { C_Style, Static, Dynamic, Reinterpret, Const, Functional };

struct CastExpr : Node {
    CastKind cast_kind;
    NodePtr  type;
    NodePtr  expr;
    explicit CastExpr(CastKind ck, NodePtr t, NodePtr e, SourceLocation loc = {})
        : Node(NodeKind::CastExpr, loc), cast_kind(ck),
          type(std::move(t)), expr(std::move(e)) {}
    REXC_ACCEPT()
};

struct SizeofExpr : Node {
    NodePtr  expr;      // sizeof(expr) – may be nullptr if type form
    NodePtr  type;      // sizeof(type) – may be nullptr if expr form
    explicit SizeofExpr(NodePtr e, NodePtr t, SourceLocation loc = {})
        : Node(NodeKind::SizeofExpr, loc), expr(std::move(e)), type(std::move(t)) {}
    REXC_ACCEPT()
};

struct NewExpr : Node {
    NodePtr              type;
    std::vector<NodePtr> args;    // constructor arguments
    NodePtr              placement; // placement new expression; may be nullptr
    bool                 is_array = false;
    NodePtr              array_size;
    explicit NewExpr(NodePtr t, SourceLocation loc = {})
        : Node(NodeKind::NewExpr, loc), type(std::move(t)) {}
    REXC_ACCEPT()
};

struct DeleteExpr : Node {
    NodePtr expr;
    bool    is_array = false;
    explicit DeleteExpr(NodePtr e, bool arr, SourceLocation loc = {})
        : Node(NodeKind::DeleteExpr, loc), expr(std::move(e)), is_array(arr) {}
    REXC_ACCEPT()
};

struct InitListExpr : Node {
    std::vector<NodePtr> elements;
    explicit InitListExpr(SourceLocation loc = {}) : Node(NodeKind::InitListExpr, loc) {}
    REXC_ACCEPT()
};

struct LambdaExpr : Node {
    enum class CaptureDefault { None, ByCopy, ByRef };
    struct Capture {
        std::string name;
        bool        by_ref = false;
        bool        is_this = false;
    };
    CaptureDefault         capture_default = CaptureDefault::None;
    std::vector<Capture>   captures;
    std::vector<NodePtr>   params;
    NodePtr                return_type;   // may be nullptr (deduced)
    NodePtr                body;
    bool                   is_mutable = false;
    explicit LambdaExpr(SourceLocation loc = {}) : Node(NodeKind::LambdaExpr, loc) {}
    REXC_ACCEPT()
};

struct ParenExpr : Node {
    NodePtr inner;
    explicit ParenExpr(NodePtr e, SourceLocation loc = {})
        : Node(NodeKind::ParenExpr, loc), inner(std::move(e)) {}
    REXC_ACCEPT()
};

struct CommaExpr : Node {
    NodePtr left;
    NodePtr right;
    explicit CommaExpr(NodePtr l, NodePtr r, SourceLocation loc = {})
        : Node(NodeKind::CommaExpr, loc), left(std::move(l)), right(std::move(r)) {}
    REXC_ACCEPT()
};

// ─────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────

// Clone a type node (shallow structural copy)
inline NodePtr clone_type(const Node* n) {
    if (!n) return nullptr;
    switch (n->kind) {
        case NodeKind::PrimitiveType: {
            auto* t = static_cast<const PrimitiveType*>(n);
            return std::make_unique<PrimitiveType>(t->prim_kind, t->loc);
        }
        case NodeKind::NamedType: {
            auto* t = static_cast<const NamedType*>(n);
            return std::make_unique<NamedType>(t->scope, t->name, t->loc);
        }
        case NodeKind::PointerType: {
            auto* t = static_cast<const PointerType*>(n);
            return std::make_unique<PointerType>(clone_type(t->pointee.get()), t->quals, t->loc);
        }
        case NodeKind::ReferenceType: {
            auto* t = static_cast<const ReferenceType*>(n);
            return std::make_unique<ReferenceType>(clone_type(t->referee.get()), t->loc);
        }
        case NodeKind::TemplateInstType: {
            auto* t = static_cast<const TemplateInstType*>(n);
            // shallow – don't deep-clone template args for now
            return std::make_unique<TemplateInstType>(t->scope, t->name,
                                                       std::vector<NodePtr>{}, t->loc);
        }
        default:
            return nullptr;  // unsupported clone
    }
}

#undef REXC_ACCEPT

} // namespace rexc
