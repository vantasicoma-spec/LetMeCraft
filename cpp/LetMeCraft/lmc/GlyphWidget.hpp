#pragma once

#include "GlyphContext.hpp"
#include "UmgReflect.hpp"

namespace lmc
{
    // Creates the prompt widget (a proven-safe W_DisplayName_GothicFont_C clone) + adds it to the
    // viewport, and builds its content once: the key-glyph image and the 3-layer station icon (earthy
    // ring + black fill + earthy symbol) on the root CanvasPanel, plus the widget's own TextBlock
    // repositioned for the station name. The NPC's own name widget is never touched.
    class GlyphWidget
    {
    public:
        explicit GlyphWidget(GlyphContext& ctx) : m_ctx(ctx) {}

        auto ensure_widget(UObject* controller) -> UObject*
        {
            if (auto* existing = weak_get(m_ctx.widget)) { return existing; }

            auto* create_fn = static_cast<UFunction*>(weak_get(m_ctx.fn_create));
            auto* widget_class = static_cast<UClass*>(weak_get(m_ctx.widget_class));
            if (!create_fn || !widget_class) { return nullptr; }

            auto call = begin_call_with_function(controller, create_fn, STR("glyph create"));
            set_param(call, STR("WorldContextObject"), &controller, 8, STR("glyph create"));
            set_param(call, STR("WidgetType"), &widget_class, 8, STR("glyph create"));
            set_param(call, STR("OwningPlayer"), &controller, 8, STR("glyph create"));
            if (!invoke_call(call, STR("glyph create"))) { return nullptr; }

            auto* widget = get_object_param(call, STR("ReturnValue"), STR("glyph create"));
            if (!is_usable(widget)) { return nullptr; }

            m_ctx.widget = widget;
            m_ctx.image_key = static_cast<UObject*>(nullptr);
            m_ctx.image_station = static_cast<UObject*>(nullptr);
            m_ctx.image_station_bg = static_cast<UObject*>(nullptr);
            m_ctx.image_station_outline = static_cast<UObject*>(nullptr);
            m_ctx.text_block = static_cast<UObject*>(nullptr);
            m_ctx.content_built = false;
            m_ctx.added = false;
            m_ctx.visible = false;
            m_ctx.key_device = -1;
            m_ctx.name_built.clear();          // force rebuild_name to re-push the name onto the new TextBlock
            m_ctx.name_dirty = false;
            m_ctx.station_path_built.clear();
            // Re-apply the (world-stable) text color onto the freshly built content; keep the cached color.
            m_ctx.text_color_upgrade_done = false;
            LMC_DLOG(STR("[LetMeCraft] glyph created: {}.\n"), object_name(widget));

            if (auto* add_fn = static_cast<UFunction*>(weak_get(m_ctx.fn_add)))
            {
                auto add = begin_call_with_function(widget, add_fn, STR("glyph add"));
                int32_t z_order = 50;
                set_param(add, STR("ZOrder"), &z_order, 4, STR("glyph add"));
                invoke_call(add, STR("glyph add"));
                m_ctx.added = true;
            }
            if (auto* align_fn = static_cast<UFunction*>(weak_get(m_ctx.fn_set_align)))
            {
                FVector2D alignment{};
                alignment.SetX(0.5);
                alignment.SetY(1.0);
                auto al = begin_call_with_function(widget, align_fn, STR("glyph align"));
                set_param(al, STR("Alignment"), &alignment, umg::kFVector2DSize, STR("glyph align"));
                invoke_call(al, STR("glyph align"));
            }
            umg::apply_visibility(widget, static_cast<UFunction*>(weak_get(m_ctx.fn_set_vis)), false);
            return widget;
        }

        // Build the prompt content once: a key-glyph UImage + the 3-layer station icon on the root
        // CanvasPanel, and the widget's own TextBlock repositioned to the right of them for the name.
        auto build_content_once(UObject* widget) -> void
        {
            if (m_ctx.content_built) { return; }

            auto* tree = read_object(widget, STR("WidgetTree"));
            auto* root = read_object(tree, STR("RootWidget"));
            auto* image_class = static_cast<UClass*>(weak_get(m_ctx.image_class));
            auto* add_fn = static_cast<UFunction*>(weak_get(m_ctx.fn_add_to_canvas));
            if (!is_usable(tree) || !is_usable(root) || !image_class || !add_fn)
            {
                // Tree not ready yet: do NOT latch the flag, retry next frame (otherwise the prompt
                // would stay forever empty if the widget tree wasn't built on the first update).
                LMC_DLOG(STR("[LetMeCraft] glyph: content not ready (tree/root/class/fn missing), will retry.\n"));
                return;
            }

            // Row order, left -> right: KEY glyph, then the STATION icon. The station icon is three
            // concentric layers and children draw in ADD order (later = on top), so add them outline ->
            // fill -> symbol: the earthy ring sits at the bottom, the black fill covers its centre (leaving
            // a thin ring), the earthy symbol sits on top. Text comes after (right of the icon).
            m_ctx.image_key = add_canvas_image(tree, root, image_class, add_fn, kKeyCenterX);
            m_ctx.image_station_outline = add_canvas_image(tree, root, image_class, add_fn, kStationCenterX, kStationOutlineSize);
            m_ctx.image_station_bg = add_canvas_image(tree, root, image_class, add_fn, kStationCenterX, kStationFillSize);
            m_ctx.image_station = add_canvas_image(tree, root, image_class, add_fn, kStationCenterX, kStationSymbolSize);

            // Both circle layers draw the same T_BaseCircle disc; the tint makes one an earthy ring and the
            // other a black fill. Set the brush once (never changes per station), then tint. The symbol's
            // texture+tint are (re)applied per station by GlyphIcons (no symbol texture exists yet).
            if (auto* circle = umg::resolve_texture(m_ctx.circle_tex, kCircleTexture))
            {
                auto* brush_fn = static_cast<UFunction*>(weak_get(m_ctx.fn_set_brush));
                auto* tint_fn = static_cast<UFunction*>(weak_get(m_ctx.fn_set_brush_tint));
                if (brush_fn)
                {
                    if (auto* o = weak_get(m_ctx.image_station_outline)) { umg::set_brush(o, brush_fn, circle, STR("glyph circ-out")); }
                    if (auto* f = weak_get(m_ctx.image_station_bg)) { umg::set_brush(f, brush_fn, circle, STR("glyph circ-fill")); }
                }
                // Outline: the resolved earthy text color if we have it, else the earthy fallback; the
                // post-build upgrade re-applies the text color if it resolves later. Fill: ALWAYS black.
                float oc[4]; umg::pick_color(m_ctx.text_color, m_ctx.text_color_valid, kTintEarthy, oc);
                umg::apply_brush_tint(weak_get(m_ctx.image_station_outline), oc, brush_fn, tint_fn, circle, STR("glyph tint-out"));
                umg::apply_brush_tint(weak_get(m_ctx.image_station_bg), kTintFill, brush_fn, tint_fn, circle, STR("glyph tint-fill"));
            }

            // Reuse the widget's TextBlock for the station name: place it to the RIGHT of the icons,
            // vertically centred on the row (NOT below - that collided with the game's NPC name plate).
            if (auto* text_block = find_text_block(widget))
            {
                m_ctx.text_block = text_block;
                if (auto* slot = read_object(text_block, STR("Slot")))
                {
                    // The reused TextBlock comes from the BP with its own anchors (centre/right), which
                    // made SetPosition land it far off. Reset its anchors to the top-left point (0,0) so
                    // our absolute coords share the same space as the freshly-added image slots.
                    umg::set_topleft_anchor(slot, static_cast<UFunction*>(weak_get(m_ctx.fn_slot_anchors)));
                    FVector2D pos{}; pos.SetX(kTextLeftX); pos.SetY(0.0);
                    FVector2D align{}; align.SetX(0.0); align.SetY(0.5);
                    umg::call_slot(slot, static_cast<UFunction*>(weak_get(m_ctx.fn_slot_pos)), STR("InPosition"), pos, STR("glyph textpos"));
                    umg::call_slot(slot, static_cast<UFunction*>(weak_get(m_ctx.fn_slot_align)), STR("InAlignment"), align, STR("glyph textalign"));
                }
            }

            // Latch only once the key + all three station layers are real; a half-built attempt retries.
            const bool ok = is_usable(weak_get(m_ctx.image_key)) && is_usable(weak_get(m_ctx.image_station_outline)) &&
                            is_usable(weak_get(m_ctx.image_station_bg)) && is_usable(weak_get(m_ctx.image_station));
            m_ctx.content_built = ok;
            LMC_DLOG(
                STR("[LetMeCraft] glyph content built={} (key={} outline={} fill={} symbol={} text={}).\n"),
                ok ? STR("ok") : STR("retry"),
                weak_get(m_ctx.image_key) ? STR("ok") : STR("-"),
                weak_get(m_ctx.image_station_outline) ? STR("ok") : STR("-"),
                weak_get(m_ctx.image_station_bg) ? STR("ok") : STR("-"),
                weak_get(m_ctx.image_station) ? STR("ok") : STR("-"),
                weak_get(m_ctx.text_block) ? STR("ok") : STR("-"));
        }

    private:
        // NewObject a UImage, add it to the root CanvasPanel, and size/centre its slot at (x, 0).
        auto add_canvas_image(UObject* tree, UObject* root, UClass* image_class, UFunction* add_fn, double x, double px = kIconSize) -> UObject*
        {
            UObject* image = nullptr;
            try { image = UObjectGlobals::NewObject<UObject>(tree, image_class); } catch (...) {}
            if (!is_usable(image))
            {
                LMC_DLOG(STR("[LetMeCraft] glyph: NewObject(Image) failed.\n"));
                return nullptr;
            }

            auto add = begin_call_with_function(root, add_fn, STR("glyph addcanvas"));
            set_param(add, STR("Content"), &image, 8, STR("glyph addcanvas"));
            if (!invoke_call(add, STR("glyph addcanvas"))) { return nullptr; }
            auto* slot = get_object_param(add, STR("ReturnValue"), STR("glyph addcanvas"));

            if (is_usable(slot))
            {
                auto* pos_fn = static_cast<UFunction*>(weak_get(m_ctx.fn_slot_pos));
                auto* size_fn = static_cast<UFunction*>(weak_get(m_ctx.fn_slot_size));
                auto* align_fn = static_cast<UFunction*>(weak_get(m_ctx.fn_slot_align));
                FVector2D pos{}; pos.SetX(x); pos.SetY(0.0);
                FVector2D size{}; size.SetX(px); size.SetY(px);
                FVector2D mid{}; mid.SetX(0.5); mid.SetY(0.5);
                umg::call_slot(slot, pos_fn, STR("InPosition"), pos, STR("glyph slotpos"));
                umg::call_slot(slot, size_fn, STR("InSize"), size, STR("glyph slotsize"));
                umg::call_slot(slot, align_fn, STR("InAlignment"), mid, STR("glyph slotalign"));
            }
            return image;
        }

        // First TextBlock found under the widget (W_DisplayName_GothicFont_C has exactly one,
        // Text_CharacterName, directly in the root CanvasPanel).
        auto find_text_block(UObject* widget) -> UObject*
        {
            UObject* found = nullptr;
            UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) {
                if (!obj) { return LoopAction::Continue; }
                auto* cls = obj->GetClassPrivate();
                if (!cls || cls->GetName().find(STR("TextBlock")) == StringType::npos) { return LoopAction::Continue; }
                for (auto* outer = obj->GetOuterPrivate(); outer; outer = outer->GetOuterPrivate())
                {
                    if (outer == widget) { found = obj; return LoopAction::Break; }
                }
                return LoopAction::Continue;
            });
            return found;
        }

        GlyphContext& m_ctx;
    };
}
