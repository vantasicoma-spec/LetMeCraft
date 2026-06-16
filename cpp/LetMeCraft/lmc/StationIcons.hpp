#pragma once

#include "Common.hpp"

namespace lmc
{
    // Generic interaction icon: the fallback for every station without a dedicated texture, and the
    // safe default while the station name/icon have not been read yet.
    inline constexpr const TCHAR* kDefaultStationIcon =
        STR("/Game/UI/Textures/Common/Icons/T_Interaction_Use.T_Interaction_Use");

    // Maps the crafting station's root-task name to one of the game's interaction action icons. Paths
    // confirmed loaded by the glyph-tex sweep. NOTE the forge asset is "T_Interact_Forge" (prefix
    // "T_Interact_", NOT "T_Interaction_"). Anything else (whetstone/saw/...) -> generic "use" icon.
    // These are white silhouette masks; GlyphOverlay tints the station image to match the prompt text.
    inline auto pick_station_icon(UObject* root_task) -> const TCHAR*
    {
        const auto name = object_name(root_task);
        if (contains_any(name, {STR("Cook"), STR("Fry"), STR("Roast"), STR("Stove"), STR("Cauldron"), STR("Pan")}))
        {
            return STR("/Game/UI/Textures/Common/Icons/T_Interaction_Cooking.T_Interaction_Cooking");
        }
        if (contains_any(name, {STR("Alchemy")}))
        {
            return STR("/Game/UI/Textures/Common/Icons/T_Interaction_Alchemy.T_Interaction_Alchemy");
        }
        if (contains_any(name, {STR("Forge"), STR("Anvil"), STR("Smith")}))
        {
            return STR("/Game/UI/Textures/Common/Icons/T_Interact_Forge.T_Interact_Forge");
        }
        return kDefaultStationIcon;
    }
}
