#pragma once

#include "GlyphContext.hpp"
#include "UmgReflect.hpp"

namespace lmc
{
    // Tracks the prompt over the NPC head every frame: anchors to the Head bone (follows the
    // animation, so it stays put when the cook sits), projects to screen, positions the widget, and
    // (re)asserts the station name on the reused TextBlock. Also owns show/hide. The caller wraps
    // position_and_label in its own SEH guard so a transient fault skips one frame, not the tick.
    class GlyphPositioner
    {
    public:
        explicit GlyphPositioner(GlyphContext& ctx) : m_ctx(ctx) {}

        // Build the FText once per change (not every frame): vendored FText is POD-ish (copy duplicates
        // Data without AddRef, no releasing dtor), so re-asserting 10-60x/sec risks a refcount desync.
        auto rebuild_name(const StringType& name) -> void
        {
            if (name == m_ctx.name_built) { return; }
            m_ctx.name_built = name;
            m_ctx.name_text = FText(name.c_str()); // Conv_StringToText - game thread only.
            m_ctx.name_dirty = true;
        }

        auto position_and_label(UObject* controller, UObject* widget, UObject* avatar) -> void
        {
            FVector head{};
            if (!head_anchor(avatar, head)) { hide(); return; }

            auto* project_fn = static_cast<UFunction*>(weak_get(m_ctx.fn_project));
            if (!project_fn) { hide(); return; }
            auto proj = begin_call_with_function(controller, project_fn, STR("glyph project"));
            set_param(proj, STR("WorldLocation"), &head, static_cast<int32_t>(sizeof(FVector)), STR("glyph project"));
            set_bool_param(proj, STR("bPlayerViewportRelative"), false, STR("glyph project"));
            if (!invoke_call(proj, STR("glyph project"))) { hide(); return; }
            if (!get_bool_param(proj, STR("ReturnValue"), STR("glyph project"))) { hide(); return; }

            FVector2D screen{};
            get_param(proj, STR("ScreenLocation"), &screen, umg::kFVector2DSize, STR("glyph project"));
            screen.SetY(screen.GetY() - kAboveHeadPx);

            show();
            if (auto* pos_fn = static_cast<UFunction*>(weak_get(m_ctx.fn_set_pos)))
            {
                auto pc = begin_call_with_function(widget, pos_fn, STR("glyph setpos"));
                set_param(pc, STR("Position"), &screen, umg::kFVector2DSize, STR("glyph setpos"));
                set_bool_param(pc, STR("bRemoveDPIScale"), true, STR("glyph setpos"));
                invoke_call(pc, STR("glyph setpos"));
            }
            apply_name();
        }

        auto show() -> void { if (!m_ctx.visible) { set_visible(true); } }
        auto hide() -> void { if (m_ctx.visible) { set_visible(false); } }

    private:
        auto set_visible(bool visible) -> void
        {
            if (umg::apply_visibility(weak_get(m_ctx.widget), static_cast<UFunction*>(weak_get(m_ctx.fn_set_vis)), visible))
            {
                m_ctx.visible = visible;
            }
        }

        // Push the station name onto the TextBlock ONLY when it changed (dirty), cleared on success.
        auto apply_name() -> void
        {
            if (!m_ctx.name_dirty) { return; }
            auto* text_block = weak_get(m_ctx.text_block);
            auto* set_text_fn = static_cast<UFunction*>(weak_get(m_ctx.fn_set_text));
            if (!is_usable(text_block) || !set_text_fn) { return; }

            auto call = begin_call_with_function(text_block, set_text_fn, STR("glyph settext"));
            if (auto* prop = find_call_property(call, STR("InText"), -1, STR("glyph settext")))
            {
                *reinterpret_cast<FText*>(call.params.data() + prop->GetOffset_Internal()) = m_ctx.name_text;
                if (invoke_call(call, STR("glyph settext"))) { m_ctx.name_dirty = false; }
            }
        }

        // The skeleton bone dump confirmed the head bone is "Head"; use it directly (it follows the
        // animation, so the icon stays over the head even when the cook is seated). Capsule top is the
        // fallback if the socket call ever fails.
        auto head_anchor(UObject* avatar, FVector& out) -> bool
        {
            if (!is_usable(avatar)) { return false; }
            const auto base = read_actor_location(avatar);
            if (!base.found) { return false; }

            auto* mesh = read_object(avatar, STR("Mesh"));
            auto* socket_fn = static_cast<UFunction*>(weak_get(m_ctx.fn_socket));
            if (is_usable(mesh) && socket_fn)
            {
                FVector socket{};
                if (socket_location(mesh, socket_fn, STR("Head"), socket) &&
                    socket.Z() > base.value.Z() - 140.0 && socket.Z() < base.value.Z() + 400.0)
                {
                    out = FVector{socket.X(), socket.Y(), socket.Z() + kHeadExtraZ};
                    return true;
                }
            }

            // Fallback: actor location is the capsule centre; top of capsule ~ above the head.
            double up = 96.0;
            if (auto* capsule = read_object(avatar, STR("CapsuleComponent")))
            {
                if (is_usable(capsule))
                {
                    if (auto* half = capsule->GetValuePtrByPropertyNameInChain<float>(STR("CapsuleHalfHeight")))
                    {
                        up = static_cast<double>(*half) + kHeadExtraZ;
                    }
                }
            }
            out = FVector{base.value.X(), base.value.Y(), base.value.Z() + up};
            return true;
        }

        auto socket_location(UObject* mesh, UFunction* socket_fn, const TCHAR* socket_name, FVector& out) -> bool
        {
            FName name{socket_name, FNAME_Find};
            auto call = begin_call_with_function(mesh, socket_fn, STR("glyph socket"));
            set_param(call, STR("InSocketName"), &name, static_cast<int32_t>(sizeof(FName)), STR("glyph socket"));
            if (!invoke_call(call, STR("glyph socket"))) { return false; }
            return get_param(call, STR("ReturnValue"), &out, static_cast<int32_t>(sizeof(FVector)), STR("glyph socket"));
        }

        GlyphContext& m_ctx;
    };
}
