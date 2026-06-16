#pragma once

#include "GlyphContext.hpp"
#include "UmgReflect.hpp"

namespace lmc
{
    // Drives the two icon brushes: the key glyph (E for keyboard / round Y for gamepad) and the station
    // action symbol. The station symbol is tinted to match the prompt text (GlyphColorizer's color, else
    // the earthy fallback) right after its texture is set, so it never flashes untinted.
    class GlyphIcons
    {
    public:
        explicit GlyphIcons(GlyphContext& ctx) : m_ctx(ctx) {}

        auto resolve_key_textures_lazy() -> void
        {
            if (weak_get(m_ctx.tex_e) && weak_get(m_ctx.tex_y)) { return; }
            const auto now = Clock::now();
            if (now < m_next_tex_retry) { return; }
            m_next_tex_retry = now + std::chrono::milliseconds(1000);
            umg::resolve_texture(m_ctx.tex_e, STR("/Game/UI/Textures/Common/HotkeysIcons/T_Icon_PC_E.T_Icon_PC_E"));
            // NOTE the asset is spelled "XboX" (capital X at BOTH ends) - confirmed by the glyph-tex
            // sweep. "T_Icon_Xbox_Y" does not resolve, which is why the gamepad prompt fell back to E.
            umg::resolve_texture(m_ctx.tex_y, STR("/Game/UI/Textures/Common/HotkeysIcons/T_Icon_XboX_Y.T_Icon_XboX_Y"));
            if (weak_get(m_ctx.tex_e) && !m_logged_tex)
            {
                m_logged_tex = true;
                LMC_DLOG(STR("[LetMeCraft] glyph key textures resolved.\n"));
            }
        }

        auto set_key_icon(bool gamepad) -> void
        {
            const int device = gamepad ? 1 : 0;
            if (device == m_ctx.key_device) { return; }
            auto* image = weak_get(m_ctx.image_key);
            auto* brush_fn = static_cast<UFunction*>(weak_get(m_ctx.fn_set_brush));
            if (!is_usable(image) || !brush_fn) { return; }

            // Prefer the requested device's glyph; if it didn't resolve (the gamepad path may be wrong
            // on this build) fall back to the other so the WHOLE prompt still shows instead of
            // vanishing - the key_device<0 gate in the orchestrator otherwise hides everything on a gamepad.
            auto* tex = gamepad ? weak_get(m_ctx.tex_y) : weak_get(m_ctx.tex_e);
            if (!tex)
            {
                if (!m_logged_key_fallback)
                {
                    m_logged_key_fallback = true;
                    LMC_DLOG(
                        STR("[LetMeCraft] glyph key texture for device={} not resolved; using fallback so the prompt still shows.\n"), device);
                }
                tex = gamepad ? weak_get(m_ctx.tex_e) : weak_get(m_ctx.tex_y);
            }
            if (!tex) { return; }

            if (umg::set_brush(image, brush_fn, tex, STR("glyph keyicon")))
            {
                m_ctx.key_device = device;
                if (!m_logged_icon)
                {
                    m_logged_icon = true;
                    LMC_DLOG(STR("[LetMeCraft] glyph key icon set (device={}).\n"), device);
                }
            }
        }

        auto set_station_icon(const TCHAR* path) -> void
        {
            if (!path) { return; }
            if (m_ctx.station_path_built == path && weak_get(m_ctx.station_tex)) { return; }

            UObject* tex = nullptr;
            try { tex = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path); } catch (...) {}
            bool fell_back = false;
            if (!tex)
            {
                try { tex = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, kFallbackStationTex); } catch (...) {}
                fell_back = true;
            }

            auto* image = weak_get(m_ctx.image_station);
            auto* brush_fn = static_cast<UFunction*>(weak_get(m_ctx.fn_set_brush));
            if (!tex || !is_usable(image) || !brush_fn) { return; }

            if (umg::set_brush(image, brush_fn, tex, STR("glyph staticon")))
            {
                m_ctx.station_tex = tex;
                m_ctx.station_path_built = path;
                // The symbol's brush just changed (and the first station was never tinted), so (re)apply
                // the symbol tint now: the resolved earthy text color if we have it, else the earthy
                // fallback. apply_brush_tint re-sets the brush internally to repaint, preserving the texture.
                auto* tint_fn = static_cast<UFunction*>(weak_get(m_ctx.fn_set_brush_tint));
                float sc[4]; umg::pick_color(m_ctx.text_color, m_ctx.text_color_valid, kTintEarthy, sc);
                umg::apply_brush_tint(image, sc, brush_fn, tint_fn, tex, STR("glyph tint-sym"));
                if (fell_back && !m_logged_station_fallback)
                {
                    m_logged_station_fallback = true;
                    LMC_DLOG(
                        STR("[LetMeCraft] glyph station icon '{}' not loaded; using generic Execute icon for now.\n"), path);
                }
            }
        }

    private:
        GlyphContext& m_ctx;
        Clock::time_point m_next_tex_retry{};
        bool m_logged_tex{};
        bool m_logged_icon{};
        bool m_logged_key_fallback{};
        bool m_logged_station_fallback{};
    };
}
