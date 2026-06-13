#pragma once

#include "Common.hpp"
#include "GameObjects.hpp"

namespace lmc
{
    // Finds the crafting NPC to evict and cancels its craft ability. The full
    // FindAllOf scan runs only on a manual press / fallback; steady-state the
    // eviction re-evaluates its cached ability instance instead (the 10Hz/1Hz
    // sweeps were the v0.8.10 stutter).
    class CraftingScanner
    {
    public:
        explicit CraftingScanner(GameObjects& objects) : m_objects(objects) {}

        auto select_crafting_candidate(
            const TCHAR* source,
            const StringType* target_owner_name,
            const StringType* target_avatar_name,
            bool require_player_range = true) -> CraftingSearchResult
        {
            if (!Unreal::IsInGameThreadRaw())
            {
                Output::send<LogLevel::Error>(
                    STR("[LetMeCraft] {} request aborted: GAS scan attempted off game thread.\n"),
                    source);
                return {};
            }

            std::vector<UObject*> abilities{};
            UObjectGlobals::FindAllOf(STR("GameplayAbilityInteractFreePoint"), abilities);

            const auto player_location = read_actor_location(m_objects.find_player_actor_cached());
            const auto has_target = (target_owner_name && !target_owner_name->empty()) ||
                                    (target_avatar_name && !target_avatar_name->empty());

            CraftingCandidate best{};

            for (auto* ability : abilities)
            {
                if (!is_usable(ability)) { continue; }

                const auto active = read_bool(ability, STR("bIsActive"));
                if (!active.found || !active.value) { continue; }

                const auto cancelable = read_bool(ability, STR("bIsCancelable"));
                if (!cancelable.found || !cancelable.value) { continue; }

                const auto end_requested = read_bool(ability, STR("bEndRequested"));
                if (end_requested.found && end_requested.value) { continue; }

                auto* root_task = read_object(ability, STR("RootInteractionTask"));
                if (!root_task || !root_task_is_crafting(root_task)) { continue; }

                auto* asc = find_ability_system(ability, root_task);
                auto* avatar = find_avatar(ability, root_task);
                auto* owner = find_owner(ability, root_task);

                // GetFullName allocates a wide string per call; build the names only
                // when a target filter is set (fallback re-scans) or when stashing
                // the winning candidate below.
                StringType owner_name{};
                StringType avatar_name{};
                if (has_target)
                {
                    owner_name = object_name(owner);
                    avatar_name = object_name(avatar);
                    if (!matches_target(owner_name, avatar_name, target_owner_name, target_avatar_name))
                    {
                        continue;
                    }
                }

                auto* candidate_actor = find_candidate_actor(avatar, owner);
                const auto candidate_location = read_actor_location(candidate_actor);
                if (!candidate_location.found) { continue; }

                auto current_distance_squared = 0.0;
                if (player_location.found)
                {
                    current_distance_squared = distance_squared(player_location.value, candidate_location.value);
                    if (require_player_range && current_distance_squared > kMaxTargetDistanceSquared)
                    {
                        continue;
                    }
                }

                // The station window applies to the manual trigger only; the kill
                // guard and the combo re-find the SAME eviction target regardless of
                // where the player has wandered meanwhile. No minimum gate since
                // v0.8.7: the mod auto-walks the player in, so a repeat E pressed
                // point-blank must not be rejected.
                if (require_player_range && player_location.found)
                {
                    const auto station_location = read_actor_location(read_object(ability, STR("m_InteractiveActor")));
                    if (!station_location.found) { continue; }
                    if (distance_squared(player_location.value, station_location.value) >
                        kMaxStationDistanceSquared)
                    {
                        continue;
                    }
                }

                if (!best.ability || current_distance_squared < best.distance_squared)
                {
                    if (!has_target)
                    {
                        owner_name = object_name(owner);
                        avatar_name = object_name(avatar);
                    }
                    best = {ability, root_task, asc, owner, avatar, current_distance_squared,
                            std::move(owner_name), std::move(avatar_name)};
                }
            }

            if (!best.ability) { return {}; }

            return {true, best};
        }

        auto call_request_end_quick(const CraftingCandidate& candidate) -> bool
        {
            if (!is_usable(candidate.ability)) { return false; }

            auto* request_end_quick = candidate.ability->GetFunctionByNameInChain(STR("OnRequestEndQuick"));
            if (!request_end_quick)
            {
                Output::send<LogLevel::Error>(
                    STR("[LetMeCraft] OnRequestEndQuick not found on {}; no call made.\n"),
                    object_name(candidate.ability));
                return false;
            }

            struct EmptyParams
            {
            } params{};

            candidate.ability->ProcessEvent(request_end_quick, &params);
            return true;
        }

        auto request_end_matching_crafter(const TCHAR* source) -> RequestResult
        {
            const auto search = select_crafting_candidate(source, nullptr, nullptr);
            if (!search.found) { return {}; }

            const auto called = call_request_end_quick(search.candidate);
            return {called, search.candidate};
        }

        // The steady-state replacement for select_crafting_candidate inside an
        // eviction: evaluate the CACHED ability instance directly (~10 property
        // reads) instead of FindAllOf-sweeping the whole object array. The 10Hz
        // combo + 1Hz kill guard sweeps were the v0.8.10 stutter. Same per-ability
        // checks as the scan loop; found=false when the routine's ability is
        // currently idle (nothing to cancel) - exactly like a scan miss.
        auto evaluate_eviction_ability(ActiveEviction& eviction) -> CraftingSearchResult
        {
            auto* ability = weak_get(eviction.ability);
            if (!ability || !is_usable(ability)) { return {}; }

            const auto active = read_bool(ability, STR("bIsActive"));
            const auto cancelable = read_bool(ability, STR("bIsCancelable"));
            const auto end_requested = read_bool(ability, STR("bEndRequested"));
            if (!active.found || !active.value) { return {}; }
            if (!cancelable.found || !cancelable.value) { return {}; }
            if (end_requested.found && end_requested.value) { return {}; }

            auto* root_task = read_object(ability, STR("RootInteractionTask"));
            if (!root_task || !root_task_is_crafting(root_task)) { return {}; }

            auto* asc = find_ability_system(ability, root_task);
            auto* owner = find_owner(ability, root_task);
            auto* avatar = find_avatar(ability, root_task);

            // No distance: its only consumers were diagnostic logs. The two
            // K2_GetActorLocation calls per 10Hz attempt + 1Hz kill-guard check
            // bought nothing.
            return {true,
                    {ability, root_task, asc, owner, avatar,
                     std::numeric_limits<double>::max(),
                     eviction.owner_name, eviction.avatar_name}};
        }

        auto find_ai_controller_from_candidate(const CraftingCandidate& candidate) -> UObject*
        {
            if (auto* controller = read_object(candidate.avatar, STR("Controller"))) { return controller; }
            if (auto* controller = read_object(candidate.owner, STR("Controller"))) { return controller; }

            auto* ai_ability = find_ai_ability_from_candidate(candidate);
            if (auto* controller = call_object_return(ai_ability, STR("GetAIController"))) { return controller; }

            return nullptr;
        }

    private:
        auto find_ai_ability_from_candidate(const CraftingCandidate& candidate) -> UObject*
        {
            if (auto* ai_ability = read_object(candidate.owner, STR("AIAbility"))) { return ai_ability; }
            if (auto* ai_ability = read_object(candidate.avatar, STR("AIAbility"))) { return ai_ability; }
            if (auto* ai_ability = call_object_return(candidate.owner, STR("GetAIAbility"))) { return ai_ability; }
            if (auto* ai_ability = call_object_return(candidate.avatar, STR("GetAIAbility"))) { return ai_ability; }

            for (const auto* class_name : {STR("GameplayAbility_CharacterAI"), STR("GameplayAbility_CharacterAI_Gothic")})
            {
                std::vector<UObject*> ai_abilities{};
                UObjectGlobals::FindAllOf(class_name, ai_abilities);
                for (auto* ai_ability : ai_abilities)
                {
                    if (!is_usable(ai_ability)) { continue; }

                    const auto owner_name = object_name(find_owner(ai_ability, nullptr));
                    const auto avatar_name = object_name(find_avatar(ai_ability, nullptr));
                    if (owner_name == candidate.owner_name || avatar_name == candidate.avatar_name)
                    {
                        return ai_ability;
                    }
                }
            }

            return nullptr;
        }

        GameObjects& m_objects;
    };
}
