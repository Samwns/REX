#pragma once
/*
 * object_layout.hpp  –  REXC Object Memory Layout Calculator
 *
 * Computes Itanium ABI–compatible class memory layouts for Phase 2 (v0.6).
 * Given a ClassDecl and the semantic context, produces:
 *   • Field offsets with natural alignment padding
 *   • VTable slot assignments for virtual methods
 *   • Total size and alignment for the class
 *
 * Layout rules (Itanium C++ ABI):
 *   1. If the class has virtual methods, the first 8 bytes hold __vptr
 *   2. Base-class fields are placed first, then own fields
 *   3. Fields are laid out in declaration order with natural alignment
 *   4. Total size is rounded up to the maximum alignment of all fields
 *   5. VTable is an array of function pointers emitted in .rodata
 */

#include "ir.hpp"
#include "ast.hpp"
#include "semantic.hpp"

#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>

namespace rexc {

// ── Field layout descriptor ──────────────────────────────────────

struct FieldLayout {
    std::string name;       // field name as written in the source
    IRType      type;       // lowered IR type used for size/alignment
    uint32_t    offset;     // byte offset from the start of the object
    uint32_t    size;       // size in bytes
};

// ── VTable entry ─────────────────────────────────────────────────

struct VTableEntry {
    std::string mangled_name;   // mangled symbol for the implementation
    std::string method_name;    // source-level method name
    uint32_t    slot;           // index in the vtable pointer array
};

// ── Complete class layout ────────────────────────────────────────

struct ClassLayout {
    std::string              class_name;
    uint32_t                 total_size        = 0;   // sizeof the class
    uint32_t                 align             = 1;   // alignment requirement
    uint32_t                 vtable_ptr_offset = 0;   // offset of __vptr (usually 0)
    std::vector<FieldLayout> fields;
    std::vector<VTableEntry> vtable;
    std::string              vtable_symbol;            // rodata global name
    bool                     has_vtable        = false;
};

// ── Layout builder ───────────────────────────────────────────────
//
// Usage:
//   ObjectLayoutBuilder builder;
//   ClassLayout layout = builder.build(class_decl, semantic_ctx);

class ObjectLayoutBuilder {
public:
    /// Compute the full memory layout for a class declaration.
    ClassLayout build(const ClassDecl& cls, const SemanticContext& ctx) {
        cls_    = &cls;
        ctx_    = &ctx;
        layout_ = ClassLayout{};
        layout_.class_name = cls.name;

        // Look up the semantic ClassInfo (may be absent for forward decls)
        auto it = ctx.classes.find(cls.name);
        info_ = (it != ctx.classes.end()) ? &it->second : nullptr;

        // A vtable is needed when the semantic pass flagged has_vtable
        layout_.has_vtable = info_ && info_->has_vtable;

        // Itanium mangling: _ZTV<len><name>
        layout_.vtable_symbol = "_ZTV" + std::to_string(cls.name.size()) + cls.name;

        uint32_t offset    = 0;
        uint32_t max_align = 1;

        // Reserve the first 8 bytes for __vptr when a vtable is present
        if (layout_.has_vtable) {
            layout_.vtable_ptr_offset = 0;
            offset    = 8;   // pointer size on 64-bit targets
            max_align = 8;
        }

        layout_fields(offset, max_align);
        build_vtable();

        layout_.align      = max_align;
        layout_.total_size  = align_up(offset, max_align);

        // C++ mandates that every object occupies at least one byte
        if (layout_.total_size == 0)
            layout_.total_size = 1;

        return layout_;
    }

private:
    const ClassDecl*      cls_    = nullptr;
    const SemanticContext* ctx_    = nullptr;
    const ClassInfo*      info_   = nullptr;
    ClassLayout           layout_;

    // ── Field layout ─────────────────────────────────────────────

    /// Walk base-class fields (placed first), then own ClassDecl.members,
    /// computing byte offsets with natural alignment padding.
    void layout_fields(uint32_t& offset, uint32_t& max_align) {
        // 1. Inherited fields from each direct base class
        if (info_) {
            for (const auto& base_name : info_->bases) {
                auto base_it = ctx_->classes.find(base_name);
                if (base_it == ctx_->classes.end()) continue;
                const ClassInfo& base = base_it->second;

                for (const auto& fname : base.fields) {
                    auto ft = base.field_types.find(fname);
                    if (ft == base.field_types.end()) continue;

                    IRType   ir_ty = map_field_type(ft->second);
                    uint32_t sz    = static_cast<uint32_t>(ir_type_size(ir_ty));
                    uint32_t al    = sz ? sz : 1;

                    offset = align_up(offset, al);
                    layout_.fields.push_back({fname, ir_ty, offset, sz});
                    offset   += sz;
                    max_align = std::max(max_align, al);
                }
            }
        }

        // 2. Own fields — iterate ClassDecl.members in declaration order
        //    The parser may emit FieldDecl or VarDecl for member fields.
        for (const auto& member : cls_->members) {
            std::string field_name;
            bool        field_is_static = false;

            if (member->kind == NodeKind::FieldDecl) {
                const auto* fd = member->as<const FieldDecl>();
                field_name      = fd->name;
                field_is_static = fd->is_static;
            } else if (member->kind == NodeKind::VarDecl) {
                const auto* vd = member->as<const VarDecl>();
                field_name      = vd->name;
                field_is_static = vd->is_static;
            } else {
                continue;
            }

            if (field_is_static) continue;   // statics have no instance layout

            IRType   ir_ty = IRType::Int32;   // conservative fallback
            uint32_t sz    = 4;

            // Prefer resolved type from the semantic pass
            if (info_) {
                auto ft = info_->field_types.find(field_name);
                if (ft != info_->field_types.end()) {
                    ir_ty = map_field_type(ft->second);
                    sz    = static_cast<uint32_t>(ir_type_size(ir_ty));
                }
            }

            uint32_t al = sz ? sz : 1;
            offset = align_up(offset, al);
            layout_.fields.push_back({field_name, ir_ty, offset, sz});
            offset   += sz;
            max_align = std::max(max_align, al);
        }
    }

    // ── VTable construction ──────────────────────────────────────

    /// Walk virtual methods from base classes then own class, assigning
    /// consecutive slot indices.  Overriding methods reuse the base slot.
    void build_vtable() {
        if (!layout_.has_vtable || !info_) return;

        uint32_t slot = 0;

        // Slots inherited from base classes (override → derived impl)
        for (const auto& base_name : info_->bases) {
            auto base_it = ctx_->classes.find(base_name);
            if (base_it == ctx_->classes.end()) continue;
            const ClassInfo& base = base_it->second;

            for (const auto& vm : base.virtual_methods) {
                // If the derived class overrides this method, use its symbol
                bool overridden = info_->methods.count(vm) != 0;
                std::string mangled = overridden
                    ? mangle_method(cls_->name, vm)
                    : mangle_method(base_name, vm);
                layout_.vtable.push_back({mangled, vm, slot++});
            }
        }

        // New virtual methods introduced by this class (not already slotted)
        for (const auto& vm : info_->virtual_methods) {
            bool in_base = false;
            for (const auto& base_name : info_->bases) {
                auto base_it = ctx_->classes.find(base_name);
                if (base_it != ctx_->classes.end()) {
                    const auto& bvms = base_it->second.virtual_methods;
                    if (std::find(bvms.begin(), bvms.end(), vm) != bvms.end()) {
                        in_base = true;
                        break;
                    }
                }
            }
            if (!in_base) {
                layout_.vtable.push_back(
                    {mangle_method(cls_->name, vm), vm, slot++});
            }
        }
    }

    // ── Helpers ──────────────────────────────────────────────────

    /// Round `offset` up to the next multiple of `al` (power of two).
    static uint32_t align_up(uint32_t offset, uint32_t al) {
        if (al == 0) return offset;
        return (offset + al - 1) & ~(al - 1);
    }

    /// Convert a semantic TypeDesc to the corresponding IRType so that
    /// ir_type_size() can provide the field width.
    static IRType map_field_type(const TypeDesc& td) {
        switch (td.tag) {
            case TypeTag::Bool:                         return IRType::Bool;
            case TypeTag::Int8:
            case TypeTag::UInt8:
            case TypeTag::Char:                         return IRType::Int8;
            case TypeTag::Int16:
            case TypeTag::UInt16:
            case TypeTag::WChar:                        return IRType::Int16;
            case TypeTag::Int32:
            case TypeTag::UInt32:                       return IRType::Int32;
            case TypeTag::Int64:
            case TypeTag::UInt64:                       return IRType::Int64;
            case TypeTag::Float:                        return IRType::Float32;
            case TypeTag::Double:
            case TypeTag::LongDouble:                   return IRType::Float64;
            case TypeTag::Pointer:
            case TypeTag::Reference:
            case TypeTag::RValueRef:
            case TypeTag::Nullptr_t:
            case TypeTag::Class:                        return IRType::Ptr;
            default:                                    return IRType::Int32;
        }
    }

    /// Produce a simplified Itanium-style mangled name:  _ZN<len><cls><len><method>Ev
    static std::string mangle_method(const std::string& cls,
                                     const std::string& method) {
        return "_ZN" + std::to_string(cls.size()) + cls
                     + std::to_string(method.size()) + method + "Ev";
    }
};

} // namespace rexc
