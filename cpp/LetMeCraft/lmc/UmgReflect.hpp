#pragma once

#include "Common.hpp"

// Generic UMG-via-reflection primitives, free of any glyph-specific state. Every function takes the
// already-resolved UFunction*/UClass*/textures (or a weak cache to resolve into) so it can be reused
// by any future UI feature. The glyph components (warmup/widget/positioner/colorizer/icons) hold the
// caches in GlyphContext and pass them in. No logging here - callers decide what to report.
namespace lmc::umg
{
    inline constexpr int32_t kFVector2DSize = 16;  // UE5.4 LWC: two doubles live (NOT sizeof=24)
    inline constexpr uint8_t kVisCollapsed = 1;
    inline constexpr uint8_t kVisSelfHitTestInvisible = 4;

    // Resolve a UFunction by full object path, caching into a weak ptr that survives GC.
    inline auto resolve_fn(FWeakObjectPtr& cache, const TCHAR* path) -> UFunction*
    {
        if (auto* cached = weak_get(cache)) { return static_cast<UFunction*>(cached); }
        UFunction* fn = nullptr;
        try { fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path); } catch (...) {}
        if (fn) { cache = static_cast<UObject*>(fn); }
        return fn;
    }

    // Resolve any UObject (texture/asset) by full object path, weak-cached.
    inline auto resolve_texture(FWeakObjectPtr& cache, const TCHAR* path) -> UObject*
    {
        if (auto* cached = weak_get(cache)) { return cached; }
        UObject* tex = nullptr;
        try { tex = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path); } catch (...) {}
        if (tex) { cache = tex; }
        return tex;
    }

    // Resolve a UClass by full object path (works for /Script classes and loaded /Game BP classes);
    // falls back to a global name scan if the path is not directly findable.
    inline auto resolve_class_by_path(FWeakObjectPtr& cache, const TCHAR* path, const TCHAR* short_name) -> UClass*
    {
        if (auto* cached = weak_get(cache)) { return static_cast<UClass*>(cached); }
        UClass* found = nullptr;
        try { found = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, path); } catch (...) {}
        if (!found)
        {
            const FName target{short_name, FNAME_Find};
            UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) {
                if (auto* cls = Cast<UClass>(obj))
                {
                    if (cls->GetNamePrivate() == target) { found = cls; return LoopAction::Break; }
                }
                return LoopAction::Continue;
            });
        }
        if (found) { cache = static_cast<UObject*>(found); }
        return found;
    }

    // Image:SetBrushFromTexture(Texture, bMatchSize=false).
    inline auto set_brush(UObject* image, UFunction* brush_fn, UObject* tex, const TCHAR* ctx) -> bool
    {
        auto call = begin_call_with_function(image, brush_fn, ctx);
        set_param(call, STR("Texture"), &tex, 8, ctx);
        set_bool_param(call, STR("bMatchSize"), false, ctx);
        return invoke_call(call, ctx);
    }

    // CanvasPanelSlot:SetPosition/SetSize/SetAlignment - one FVector2D param (16 bytes live on LWC).
    inline auto call_slot(UObject* slot, UFunction* fn, const TCHAR* param, const FVector2D& value, const TCHAR* ctx) -> void
    {
        if (!fn) { return; }
        auto call = begin_call_with_function(slot, fn, ctx);
        auto local = value;
        set_param(call, param, &local, kFVector2DSize, ctx);
        invoke_call(call, ctx);
    }

    // SetAnchors(InAnchors) with a zero-initialized param buffer => InAnchors = {Min(0,0),Max(0,0)},
    // a top-left point anchor. Size-agnostic (we don't memcpy a value), so it works whatever the
    // FAnchors layout/size is on this build - the zeroed params slot already encodes (0,0,0,0).
    inline auto set_topleft_anchor(UObject* slot, UFunction* fn) -> void
    {
        if (!is_usable(slot) || !fn) { return; }
        auto call = begin_call_with_function(slot, fn, STR("glyph anchors"));
        invoke_call(call, STR("glyph anchors"));
    }

    // Widget:SetVisibility(InVisibility). visible -> SelfHitTestInvisible (drawn, no input); else Collapsed.
    inline auto apply_visibility(UObject* widget, UFunction* fn, bool visible) -> bool
    {
        if (!widget || !fn) { return false; }
        auto call = begin_call_with_function(widget, fn, STR("glyph vis"));
        uint8_t value = visible ? kVisSelfHitTestInvisible : kVisCollapsed;
        set_param(call, STR("InVisibility"), &value, 1, STR("glyph vis"));
        return invoke_call(call, STR("glyph vis"));
    }

    // Read an FSlateColor property (e.g. UTextBlock.ColorAndOpacity) -> its SpecifiedColor (FLinearColor,
    // 16 bytes) + ColorUseRule byte. out_rule defaults to 0xFF so a missing rule reads as "not specified".
    inline auto read_slate_color(UObject* obj, const TCHAR* prop_name, float (&out_rgba)[4], uint8_t& out_rule) -> bool
    {
        out_rule = 0xFF;
        if (!is_usable(obj)) { return false; }
        auto* col_prop = CastField<FStructProperty>(obj->GetPropertyByNameInChain(prop_name));
        if (!col_prop) { return false; }
        void* col_ptr = col_prop->ContainerPtrToValuePtr<void>(obj);
        if (!col_ptr) { return false; }
        auto col_struct = col_prop->GetStruct(); // TObjectPtr<UScriptStruct> (FSlateColor)
        if (!col_struct) { return false; }
        auto* spec_prop = CastField<FStructProperty>(col_struct->FindProperty(FName(STR("SpecifiedColor"), FNAME_Find)));
        if (!spec_prop) { return false; }
        const int32_t spec_off = spec_prop->GetOffset_Internal();
        if (spec_off < 0 || spec_prop->GetSize() < 16) { return false; }
        std::memcpy(&out_rgba[0], static_cast<uint8_t*>(col_ptr) + spec_off, 16);
        if (auto* rule_prop = col_struct->FindProperty(FName(STR("ColorUseRule"), FNAME_Find)))
        {
            const int32_t ro = rule_prop->GetOffset_Internal();
            if (ro >= 0 && rule_prop->GetSize() >= 1) { std::memcpy(&out_rule, static_cast<uint8_t*>(col_ptr) + ro, 1); }
        }
        return true;
    }

    // Copy the best color for a layer into out[4]: primary if valid, else fallback. Returns true if primary used.
    inline auto pick_color(const float* primary, bool valid, const float* fallback, float (&out)[4]) -> bool
    {
        const float* s = valid ? primary : fallback;
        out[0] = s[0]; out[1] = s[1]; out[2] = s[2]; out[3] = s[3];
        return valid;
    }

    // Tint a UImage's brush to an RGBA color. SetColorAndOpacity never visibly recolored a
    // SetBrushFromTexture image on this build, so we use only PROVEN primitives:
    //  PRIMARY (A): write the 16-byte FLinearColor straight into the live Brush.TintColor.SpecifiedColor
    //    via property traversal, then re-set the brush texture (set_brush) to force Slate to repaint - it
    //    only touches ResourceObject so the TintColor we just wrote is preserved. No new UFunction needed.
    //  FALLBACK (B): Image:SetBrushTintColor(FSlateColor) - size discovered at call time.
    // retexture = the texture the image currently carries (re-set to repaint). Degrades to untinted; never throws.
    inline auto apply_brush_tint(UObject* image, const float (&rgba)[4], UFunction* set_brush_fn,
                                 UFunction* set_brush_tint_fn, UObject* retexture, const TCHAR* ctx) -> void
    {
        if (!is_usable(image)) { return; }

        // ---- PRIMARY (A): direct Brush.TintColor.SpecifiedColor write. ----
        bool wrote_direct = false;
        run_guarded(STR("glyph tint-direct"), [&] {
            auto* brush_prop = CastField<FStructProperty>(image->GetPropertyByNameInChain(STR("Brush")));
            if (!brush_prop) { return; }
            void* brush_ptr = brush_prop->ContainerPtrToValuePtr<void>(image);
            if (!brush_ptr) { return; }
            auto brush_struct = brush_prop->GetStruct(); // TObjectPtr<UScriptStruct>
            if (!brush_struct) { return; }

            auto* tint_prop = CastField<FStructProperty>(brush_struct->FindProperty(FName(STR("TintColor"), FNAME_Find)));
            if (!tint_prop) { return; }
            void* tint_ptr = tint_prop->ContainerPtrToValuePtr<void>(brush_ptr);
            if (!tint_ptr) { return; }
            auto tint_struct = tint_prop->GetStruct();
            if (!tint_struct) { return; }

            auto* spec_prop = CastField<FStructProperty>(tint_struct->FindProperty(FName(STR("SpecifiedColor"), FNAME_Find)));
            if (!spec_prop) { return; }
            const int32_t spec_off = spec_prop->GetOffset_Internal();
            if (spec_off < 0 || spec_prop->GetSize() < 16) { return; } // FLinearColor = 16 bytes
            std::memcpy(static_cast<uint8_t*>(tint_ptr) + spec_off, &rgba[0], 16);

            // ColorUseRule = 0 (ESlateColorStylingMode::UseColor_Specified) so SpecifiedColor is used.
            if (auto* rule_prop = tint_struct->FindProperty(FName(STR("ColorUseRule"), FNAME_Find)))
            {
                const int32_t ro = rule_prop->GetOffset_Internal();
                if (ro >= 0 && rule_prop->GetSize() >= 1)
                {
                    uint8_t v = 0;
                    std::memcpy(static_cast<uint8_t*>(tint_ptr) + ro, &v, 1);
                }
            }
            wrote_direct = true;
        });

        if (wrote_direct)
        {
            if (set_brush_fn && is_usable(retexture)) { set_brush(image, set_brush_fn, retexture, ctx); } // repaint; keeps TintColor
            return;
        }

        // ---- FALLBACK (B): SetBrushTintColor(FSlateColor TintColor). ----
        if (set_brush_tint_fn)
        {
            auto call = begin_call_with_function(image, set_brush_tint_fn, ctx);
            if (auto* prop = find_call_property(call, STR("TintColor"), -1, ctx)) // -1: discover size only
            {
                const int32_t sz = prop->GetSize();
                if (sz >= 16 && static_cast<size_t>(prop->GetOffset_Internal()) + static_cast<size_t>(sz) <= call.params.size())
                {
                    std::vector<uint8_t> buf(static_cast<size_t>(sz), 0); // zero => ColorUseRule = 0
                    std::memcpy(buf.data(), &rgba[0], 16);                // FLinearColor at offset 0
                    set_param(call, STR("TintColor"), buf.data(), sz, ctx); // real size, never -1
                    invoke_call(call, ctx);
                }
            }
        }
    }
}
