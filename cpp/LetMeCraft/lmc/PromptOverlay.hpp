#pragma once

#include "Common.hpp"
#include "GameObjects.hpp"

namespace lmc
{
    // v1.2.0 floating prompt (approach B1 - reuse the game's own UI). When the player is in evict
    // range of a cook NPC (the same gate as pressing E), the game is already driving its floating
    // over-NPC name widget `W_DisplayName_GothicFont_C` via DisplayName(<npc name>) every frame.
    // We RIDE that call: a ProcessEvent pre-hook on DisplayName replaces the text argument with the
    // station's localized name + a key hint ("Сковорода  [E]"). Because it's the game's own per-frame
    // call, there is no fighting/flicker - it renders in the native font, at the native position
    // over the NPC. Pure UI: only a text argument is swapped; no NPC/spot/save state is touched.
    //
    // update() runs on the game thread (engine tick) and may call Conv_StringToText (building the
    // FText) - safe there. on_display_name() runs inside the ProcessEvent hook and must NOT call any
    // UFunction (re-entrancy); it only assigns the pre-built FText, so it is allocation-free.
    class PromptOverlay
    {
    public:
        explicit PromptOverlay(GameObjects& objects) : m_objects(objects)
        {
            // The floating name widget shown over the NPC (vs `W_ObjectInteraction_C`, the on-screen
            // interaction prompt). FNAME_Find: the BP class is already loaded.
            m_target_widget_class = FName(STR("W_DisplayName_GothicFont_C"), FNAME_Find);
        }

        // Refresh the prompt state from the current evict candidate (or none). gamepad selects the
        // key hint glyph text. Rebuilds the FText only when the displayed string actually changes.
        auto update(bool found, const CraftingCandidate& candidate, bool gamepad) -> void
        {
            if (!found)
            {
                m_active = false;
                return;
            }

            auto* item = read_object(read_object(read_object(candidate.ability, STR("m_InteractiveActor")),
                                                 STR("m_InteractiveComponent")), STR("m_InteractItem"));
            const auto name = read_ftext(item, STR("m_Name"));
            if (name.empty())
            {
                m_active = false;
                return;
            }

            const auto desired = name + (gamepad ? STR("  [Y]") : STR("  [E]"));
            if (desired != m_built)
            {
                m_prompt_text = FText(desired.c_str()); // Conv_StringToText - game thread only.
                m_built = desired;
                if (!m_logged_once)
                {
                    m_logged_once = true;
                    Output::send<LogLevel::Verbose>(
                        STR("[LetMeCraft] prompt active: over-NPC label -> '{}'.\n"), desired);
                }
            }
            m_active = true;
        }

        // Fed by the DisplayName ProcessEvent pre-hook. While the prompt is active, swap the text on
        // the over-NPC name widget for our station-name prompt. Allocation-free (no UFunction calls).
        auto on_display_name(UObject* context, void* parms) -> void
        {
            if (!m_active || !parms || !is_usable(context)) { return; }
            const auto* cls = context->GetClassPrivate();
            if (!cls || cls->GetNamePrivate() != m_target_widget_class) { return; }
            *reinterpret_cast<FText*>(parms) = m_prompt_text;
        }

        auto clear() -> void { m_active = false; }

    private:
        GameObjects& m_objects;
        FName m_target_widget_class{};
        FText m_prompt_text{};
        StringType m_built{};
        bool m_active{};
        bool m_logged_once{};
    };
}
