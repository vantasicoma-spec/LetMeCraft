#pragma once

#include "Common.hpp"

namespace lmc
{
    // Weak-cached resolution of the stable game-side singletons (interaction
    // subsystem, the player's controller/state/pawn/sensor and interact ability
    // instance). FindAllOf walks the entire object array, so each is discovered
    // once and re-found only when it actually dies. Cleared on map load /
    // game-state init (the old world's objects belong to the old world).
    class GameObjects
    {
    public:
        // FindAllOf walks the entire object array (expensive); the stable singletons are
        // cached in weak pointers and only re-discovered when they actually die.
        auto find_interaction_subsystem() -> UObject*
        {
            if (auto* cached = weak_get(m_cached_subsystem)) { return cached; }

            std::vector<UObject*> subsystems{};
            UObjectGlobals::FindAllOf(STR("InteractionSubsystem"), subsystems);
            for (auto* subsystem : subsystems)
            {
                if (is_usable(subsystem))
                {
                    m_cached_subsystem = subsystem;
                    return subsystem;
                }
            }
            return nullptr;
        }

        auto find_player_actor_cached() -> UObject*
        {
            if (auto* player_controller = find_player_controller_cached())
            {
                if (auto* pawn = read_object(player_controller, STR("Pawn"))) { return pawn; }
                if (auto* pawn = read_object(player_controller, STR("AcknowledgedPawn"))) { return pawn; }
            }
            return find_player_actor();
        }

        auto find_player_controller_cached() -> UObject*
        {
            if (auto* cached = weak_get(m_cached_player_controller)) { return cached; }

            if (auto* player_controller = find_player_controller())
            {
                m_cached_player_controller = player_controller;
                return player_controller;
            }
            return nullptr;
        }

        auto find_player_state() -> UObject*
        {
            if (auto* cached = weak_get(m_cached_player_state)) { return cached; }

            if (auto* player_controller = find_player_controller_cached())
            {
                if (auto* player_state = read_object(player_controller, STR("PlayerState")))
                {
                    m_cached_player_state = player_state;
                    return player_state;
                }
            }
            return nullptr;
        }

        // The player's UInteractSensor decides what the interact key would target; its
        // m_CurrentInteractionActor gates the handoff and the auto-use (otherwise the
        // interact ability fires at whatever is focused - e.g. starts a dialogue with
        // the evicted NPC).
        auto find_player_sensor() -> UObject*
        {
            if (auto* cached = weak_get(m_cached_player_sensor)) { return cached; }

            auto* pawn = find_player_actor_cached();
            if (!pawn) { return nullptr; }

            std::vector<UObject*> sensors{};
            UObjectGlobals::FindAllOf(STR("InteractSensor"), sensors);
            for (auto* sensor : sensors)
            {
                if (!is_usable(sensor)) { continue; }
                for (auto* outer = sensor->GetOuterPrivate(); outer; outer = outer->GetOuterPrivate())
                {
                    if (outer == pawn)
                    {
                        m_cached_player_sensor = sensor;
                        return sensor;
                    }
                }
            }
            return nullptr;
        }

        // Returns the player's own GameplayAbilityInteract INSTANCE (not the class): the
        // instance carries m_InteractionActor, which is both pre-aimed at the station and
        // read back after activation to verify what the interaction actually targeted.
        // Weak-cached: this runs on every 10Hz take attempt and the FindAllOf sweep
        // was part of the v0.8.10 stutter.
        auto find_player_interact_ability_instance(UObject* player_state) -> UObject*
        {
            const auto outer_chain_reaches = [](UObject* object, UObject* target) {
                for (auto* outer = object->GetOuterPrivate(); outer; outer = outer->GetOuterPrivate())
                {
                    if (outer == target) { return true; }
                }
                return false;
            };

            if (auto* cached = weak_get(m_cached_player_interact_ability))
            {
                if (is_usable(cached) && outer_chain_reaches(cached, player_state)) { return cached; }
            }

            std::vector<UObject*> abilities{};
            UObjectGlobals::FindAllOf(STR("GameplayAbilityInteract"), abilities);
            for (auto* ability : abilities)
            {
                if (!is_usable(ability)) { continue; }

                // Pointer-walk the outer chain instead of building full-name strings.
                if (outer_chain_reaches(ability, player_state))
                {
                    m_cached_player_interact_ability = ability;
                    return ability;
                }
            }

            return nullptr;
        }

        // Drop the cached singletons: the old world that owned them is being torn down.
        auto clear() -> void
        {
            m_cached_subsystem = static_cast<UObject*>(nullptr);
            m_cached_player_controller = static_cast<UObject*>(nullptr);
            m_cached_player_state = static_cast<UObject*>(nullptr);
            m_cached_player_sensor = static_cast<UObject*>(nullptr);
            m_cached_player_interact_ability = static_cast<UObject*>(nullptr);
        }

    private:
        FWeakObjectPtr m_cached_subsystem{};
        FWeakObjectPtr m_cached_player_controller{};
        FWeakObjectPtr m_cached_player_state{};
        FWeakObjectPtr m_cached_player_sensor{};
        FWeakObjectPtr m_cached_player_interact_ability{};
    };
}
