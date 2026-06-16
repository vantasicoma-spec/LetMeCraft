#pragma once

#include "GlyphContext.hpp"
#include "UmgReflect.hpp"

namespace lmc
{
    // The native prompt's color is baked into its TEXTURES (every native brush TintColor reads white),
    // so we tint the ring + symbol the SAME earthy color as the prompt TEXT: read our OWN TextBlock's
    // ColorAndOpacity (a read of our held widget child -> run_guarded suffices), then a one-shot upgrade
    // re-tints the ring + symbol once it resolves. Until then the earthy fallback (kTintEarthy) stands.
    class GlyphColorizer
    {
    public:
        explicit GlyphColorizer(GlyphContext& ctx) : m_ctx(ctx) {}

        // Throttled until a usable color resolves or the attempt cap; then stops.
        auto resolve_text_color() -> void
        {
            if (m_text_color_ready || m_text_color_attempts >= kTextColorMaxAttempts) { return; }
            auto* tb = weak_get(m_ctx.text_block);
            if (!is_usable(tb)) { return; } // TextBlock not built yet; retry next frame (no attempt spent)
            const auto now = Clock::now();
            if (now < m_next_text_color_at) { return; }
            m_next_text_color_at = now + kTextColorRetryInterval;
            ++m_text_color_attempts;

            float rgba[4]{}; uint8_t rule = 0xFF; bool read = false;
            run_guarded(STR("glyph text-color"), [&] { read = umg::read_slate_color(tb, STR("ColorAndOpacity"), rgba, rule); });

            if (read && !m_logged_text_color_values)
            {
                m_logged_text_color_values = true;
                LMC_DLOG(
                    STR("[LetMeCraft] glyph text color: ({:.3f},{:.3f},{:.3f},{:.3f}) rule={}.\n"),
                    rgba[0], rgba[1], rgba[2], rgba[3], static_cast<int>(rule));
            }
            // Accept only a specified (rule==0), non-pure-white color. Pure white => the real color lives
            // in the font/material, not ColorAndOpacity -> keep the earthy fallback.
            if (read && rule == 0 &&
                !(rgba[0] > kNearWhiteThreshold && rgba[1] > kNearWhiteThreshold && rgba[2] > kNearWhiteThreshold))
            {
                std::memcpy(m_ctx.text_color, rgba, sizeof(m_ctx.text_color));
                m_ctx.text_color_valid = true;
                m_text_color_ready = true; // got a usable color -> stop polling
            }
            else if (m_text_color_attempts >= kTextColorMaxAttempts)
            {
                m_text_color_ready = true; // give up; the earthy fallback stands
            }
        }

        // One-shot: once the text color resolves AFTER the content was built with the earthy fallback,
        // re-tint the ring + symbol with it (GlyphIcons::set_station_icon early-returns once the icon is
        // set, so it won't re-tint on its own). The BLACK FILL is never touched. Latched only after a
        // successful outline re-tint, so a momentarily unusable widget retries next frame.
        auto apply_text_color_upgrade() -> void
        {
            if (m_ctx.text_color_upgrade_done || !m_ctx.text_color_valid || !m_ctx.content_built) { return; }
            auto* outline = weak_get(m_ctx.image_station_outline);
            if (!is_usable(outline)) { return; }

            auto* circle = umg::resolve_texture(m_ctx.circle_tex, kCircleTexture); // re-resolve so the repaint fires
            auto* brush_fn = static_cast<UFunction*>(weak_get(m_ctx.fn_set_brush));
            auto* tint_fn = static_cast<UFunction*>(weak_get(m_ctx.fn_set_brush_tint));
            float c[4]; umg::pick_color(m_ctx.text_color, m_ctx.text_color_valid, kTintEarthy, c);
            umg::apply_brush_tint(outline, c, brush_fn, tint_fn, circle, STR("glyph tint-out"));
            if (auto* sym = weak_get(m_ctx.image_station))
            {
                if (auto* tex = weak_get(m_ctx.station_tex)) { umg::apply_brush_tint(sym, c, brush_fn, tint_fn, tex, STR("glyph tint-sym")); }
            }
            m_ctx.text_color_upgrade_done = true;
            LMC_DLOG(
                STR("[LetMeCraft] glyph text-color upgrade applied to ring+symbol: ({:.3f},{:.3f},{:.3f}).\n"),
                c[0], c[1], c[2]);
        }

        // A new world reloads the prompt BP: re-read the text color from scratch.
        auto clear() -> void
        {
            m_text_color_ready = false;
            m_text_color_attempts = 0;
            m_next_text_color_at = {};
            m_logged_text_color_values = false;
        }

    private:
        GlyphContext& m_ctx;
        bool m_text_color_ready{};        // got a usable color, or gave up -> stop polling
        int m_text_color_attempts{};
        Clock::time_point m_next_text_color_at{};
        bool m_logged_text_color_values{};
    };
}
