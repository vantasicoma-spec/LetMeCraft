#pragma once

#include "Common.hpp"
#include "GameObjects.hpp"
#include "GlyphContext.hpp"
#include "GlyphWarmup.hpp"
#include "GlyphWidget.hpp"
#include "GlyphPositioner.hpp"
#include "GlyphColorizer.hpp"
#include "GlyphIcons.hpp"

namespace lmc
{
    // v1.2.0 floating eviction prompt over the evictable cook NPC - thin orchestrator.
    //
    // A custom UMG widget (a proven-safe W_DisplayName_GothicFont_C clone) is created via reflection
    // and hosts our own content: a key glyph (E / round Y), a 3-layer station icon (earthy ring + black
    // fill + earthy symbol, tinted to match the prompt text), and the widget's TextBlock reused for the
    // station name. Positioned every frame over the NPC head bone. Shown only while the player can evict
    // that NPC. Pure UI: no NPC/spot/save writes.
    //
    // The work is split by responsibility into GlyphWarmup (resolve UFunctions), GlyphWidget (create +
    // build content), GlyphIcons (key/station textures), GlyphColorizer (text-color tint), and
    // GlyphPositioner (project/track + name). All shared state lives in GlyphContext. Per-frame
    // positioning is wrapped in its OWN SEH guard so a transient fault skips one icon frame instead of
    // taking the tick (and the evictions) down with it.
    class GlyphOverlay
    {
    public:
        explicit GlyphOverlay(GameObjects& objects) : m_objects(objects) {}

        // active: the player can evict this NPC right now. avatar: the NPC, ALREADY validated live by
        // the caller via weak_get this frame. station_name: the localized station name to print.
        // station_icon_path: full object path of the action icon texture. Both are computed by the
        // caller at scan time off the fresh candidate and only re-applied when they actually change.
        auto update(bool active, UObject* avatar, bool gamepad,
                    const StringType& station_name, const TCHAR* station_icon_path) -> void
        {
            m_warmup.warm_up();
            if (!active || !m_ctx.warmed || !is_usable(avatar)) { m_positioner.hide(); return; }

            auto* controller = m_objects.find_player_controller_cached();
            if (!is_usable(controller)) { m_positioner.hide(); return; }

            auto* widget = m_widget.ensure_widget(controller);
            if (!is_usable(widget)) { m_positioner.hide(); return; }

            m_icons.resolve_key_textures_lazy();
            m_widget.build_content_once(widget);
            m_icons.set_key_icon(gamepad);
            m_icons.set_station_icon(station_icon_path);
            m_positioner.rebuild_name(station_name);

            // Read the prompt TEXT color (now that build_content_once has found the TextBlock); if the
            // content was tinted with the earthy FALLBACK first, re-tint the ring + symbol ONCE to match it.
            m_colorizer.resolve_text_color();
            m_colorizer.apply_text_color_upgrade();

            // Nothing to show until the REAL key icon is actually on the image (no placeholder).
            if (m_ctx.key_device < 0) { m_positioner.hide(); return; }

            // Project the head to screen, position the widget, (re)assert the station name. Wrapped in
            // its OWN SEH guard: during a station-take/camera transition the projection occasionally
            // faults on a half-torn-down avatar; with this guard that just skips a single icon frame
            // instead of aborting all evictions and clearing the widget (which made the icon vanish).
            SehCrashInfo crash{};
            if (!invoke_with_seh([&] { m_positioner.position_and_label(controller, widget, avatar); }, crash))
            {
                if (m_pos_fault_logged < 3)
                {
                    ++m_pos_fault_logged;
                    LMC_DLOG(
                        STR("[LetMeCraft] glyph position skipped one frame (SEH code=0x{:X} ip=0x{:X}); eviction unaffected.\n"),
                        crash.code,
                        reinterpret_cast<uintptr_t>(crash.instruction_address));
                }
            }
        }

        auto clear() -> void
        {
            // Re-warm + re-read color after a map load: the BP widget class may unload with the old
            // world. The /Script function caches + textures deliberately survive (permanent objects).
            m_warmup.clear();
            m_colorizer.clear();
            m_ctx.widget = static_cast<UObject*>(nullptr);
            m_ctx.image_key = static_cast<UObject*>(nullptr);
            m_ctx.image_station = static_cast<UObject*>(nullptr);
            m_ctx.image_station_bg = static_cast<UObject*>(nullptr);
            m_ctx.image_station_outline = static_cast<UObject*>(nullptr);
            m_ctx.text_block = static_cast<UObject*>(nullptr);
            m_ctx.added = false;
            m_ctx.visible = false;
            m_ctx.content_built = false;
            m_ctx.key_device = -1;
            m_ctx.name_built.clear();
            m_ctx.name_dirty = false;
            m_ctx.station_path_built.clear();
            m_ctx.text_color_valid = false;
            m_ctx.text_color_upgrade_done = false;
        }

    private:
        GameObjects& m_objects;
        GlyphContext m_ctx{};
        GlyphWarmup m_warmup{m_ctx};
        GlyphWidget m_widget{m_ctx};
        GlyphPositioner m_positioner{m_ctx};
        GlyphColorizer m_colorizer{m_ctx};
        GlyphIcons m_icons{m_ctx};
        int m_pos_fault_logged{};
    };
}
