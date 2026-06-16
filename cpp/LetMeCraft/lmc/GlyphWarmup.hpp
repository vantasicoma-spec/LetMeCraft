#pragma once

#include "GlyphContext.hpp"
#include "UmgReflect.hpp"

namespace lmc
{
    // Resolves every UFunction/UClass the prompt needs, once, on a quiet 500ms-retry cadence (resolving
    // in a hot/ProcessEvent tick threw "Array failed invariants check"). Sets ctx.warmed when the
    // gating set is all resolved; the two tint functions are non-gating (the primary tint path is a
    // direct memory write that needs no UFunction).
    class GlyphWarmup
    {
    public:
        explicit GlyphWarmup(GlyphContext& ctx) : m_ctx(ctx) {}

        auto warm_up() -> void
        {
            if (m_ctx.warmed) { return; }
            const auto now = Clock::now();
            if (now < m_next_warmup_at) { return; }
            m_next_warmup_at = now + kWarmupRetryInterval;
            ++m_warmup_attempts;

            run_guarded(STR("glyph warm-up"), [&] {
                const auto create = umg::resolve_fn(m_ctx.fn_create, STR("/Script/UMG.WidgetBlueprintLibrary:Create"));
                const auto add = umg::resolve_fn(m_ctx.fn_add, STR("/Script/UMG.UserWidget:AddToViewport"));
                const auto setpos = umg::resolve_fn(m_ctx.fn_set_pos, STR("/Script/UMG.UserWidget:SetPositionInViewport"));
                const auto setalign = umg::resolve_fn(m_ctx.fn_set_align, STR("/Script/UMG.UserWidget:SetAlignmentInViewport"));
                const auto setvis = umg::resolve_fn(m_ctx.fn_set_vis, STR("/Script/UMG.Widget:SetVisibility"));
                const auto project = umg::resolve_fn(m_ctx.fn_project, STR("/Script/Engine.PlayerController:ProjectWorldLocationToScreen"));
                const auto socket = umg::resolve_fn(m_ctx.fn_socket, STR("/Script/Engine.SceneComponent:GetSocketLocation"));
                const auto brush = umg::resolve_fn(m_ctx.fn_set_brush, STR("/Script/UMG.Image:SetBrushFromTexture"));
                const auto addcanvas = umg::resolve_fn(m_ctx.fn_add_to_canvas, STR("/Script/UMG.CanvasPanel:AddChildToCanvas"));
                const auto slotpos = umg::resolve_fn(m_ctx.fn_slot_pos, STR("/Script/UMG.CanvasPanelSlot:SetPosition"));
                const auto slotsize = umg::resolve_fn(m_ctx.fn_slot_size, STR("/Script/UMG.CanvasPanelSlot:SetSize"));
                const auto slotalign = umg::resolve_fn(m_ctx.fn_slot_align, STR("/Script/UMG.CanvasPanelSlot:SetAlignment"));
                umg::resolve_fn(m_ctx.fn_slot_anchors, STR("/Script/UMG.CanvasPanelSlot:SetAnchors"));
                // Tint mechanisms (non-gating). PRIMARY is a direct Brush.TintColor memory write (needs no
                // UFunction); SetBrushTintColor is a fallback. SetColorAndOpacity is the old path that
                // never visibly tinted - kept resolved only for the diagnostic log.
                const auto tint_bt = umg::resolve_fn(m_ctx.fn_set_brush_tint, STR("/Script/UMG.Image:SetBrushTintColor"));
                const auto tint_co = umg::resolve_fn(m_ctx.fn_tint, STR("/Script/UMG.Image:SetColorAndOpacity"));
                const auto settext = umg::resolve_fn(m_ctx.fn_set_text, STR("/Script/UMG.TextBlock:SetText"));
                const auto wcls = umg::resolve_class_by_path(m_ctx.widget_class, STR("/Game/UI/Common/W_DisplayName_GothicFont.W_DisplayName_GothicFont_C"), STR("W_DisplayName_GothicFont_C"));
                const auto icls = umg::resolve_class_by_path(m_ctx.image_class, STR("/Script/UMG.Image"), STR("Image"));
                m_ctx.warmed = create && add && setpos && setvis && project && brush && addcanvas &&
                               slotpos && slotsize && slotalign && settext && wcls && icls;

                if (m_ctx.warmed && !m_logged_warm)
                {
                    m_logged_warm = true;
                    LMC_DLOG(
                        STR("[LetMeCraft] glyph warm-up OK (align={} socket={} settext={} | tint: direct=proven setbrushtint={} colorandopacity={}).\n"),
                        setalign ? STR("ok") : STR("-"),
                        socket ? STR("ok") : STR("-"),
                        settext ? STR("ok") : STR("-"),
                        tint_bt ? STR("ok") : STR("-"),
                        tint_co ? STR("ok") : STR("-"));
                }
            });

            if (!m_ctx.warmed && (m_warmup_attempts == 1 || m_warmup_attempts % 20 == 0))
            {
                LMC_DLOG(
                    STR("[LetMeCraft] glyph warm-up attempt {} unresolved (create={} addcanvas={} settext={} wclass={} iclass={}).\n"),
                    m_warmup_attempts,
                    weak_get(m_ctx.fn_create) ? STR("ok") : STR("-"), weak_get(m_ctx.fn_add_to_canvas) ? STR("ok") : STR("-"),
                    weak_get(m_ctx.fn_set_text) ? STR("ok") : STR("-"), weak_get(m_ctx.widget_class) ? STR("ok") : STR("-"),
                    weak_get(m_ctx.image_class) ? STR("ok") : STR("-"));
            }
        }

        // Re-warm after a map load: the BP widget class may unload with the old world. The /Script
        // function caches are permanent and re-resolve instantly on the next quiet tick.
        auto clear() -> void
        {
            m_ctx.warmed = false;
            m_next_warmup_at = {};
        }

    private:
        GlyphContext& m_ctx;
        Clock::time_point m_next_warmup_at{};
        int m_warmup_attempts{};
        bool m_logged_warm{};
    };
}
