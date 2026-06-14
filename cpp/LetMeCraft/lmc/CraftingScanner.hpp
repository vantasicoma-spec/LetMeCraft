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

            // #1: the player's look direction, read once. Only for the manual trigger -
            // kill-guard / combo re-finds (require_player_range==false) must keep
            // targeting the same eviction no matter where the player is looking.
            const auto view_forward = require_player_range
                ? read_view_forward_2d(m_objects.find_player_controller_cached())
                : ViewForwardRead{};

            CraftingCandidate best{};
            double best_score = -std::numeric_limits<double>::max();

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

                // Manual-trigger gates (require_player_range only). The kill guard and
                // the combo re-find the SAME eviction target regardless of where the
                // player has wandered, so they skip the station window, the usability
                // gate and the view-cone gate entirely and keep the legacy nearest pick.
                auto candidate_score = -current_distance_squared;  // legacy: nearest wins
                if (require_player_range && player_location.found)
                {
                    auto* station_actor = read_object(ability, STR("m_InteractiveActor"));
                    const auto station_location = read_actor_location(station_actor);
                    if (!station_location.found) { continue; }
                    // No minimum gate since v0.8.7: the mod auto-walks the player in, so
                    // a repeat E pressed point-blank must not be rejected.
                    if (distance_squared(player_location.value, station_location.value) >
                        kMaxStationDistanceSquared)
                    {
                        continue;
                    }

                    // #2: skip stations the player cannot actually use (NPC-only ambient
                    // activities, by Action tag) - otherwise the mod drags the player over
                    // and burns 35 failed take attempts.
                    if (!player_can_use_candidate(root_task)) { continue; }

                    // #1: behave like the native Interact key - only engage when the NPC
                    // is in the player's view cone, and #3: among several (a pan cluster)
                    // prefer the one most centered in view. The cone is tested against the
                    // NPC (candidate_location), NOT the station actor: the station pivot
                    // can sit behind the working spot (cauldron logged dot=-0.995 while
                    // correctly used). A read failure -> no cone gate, legacy nearest pick.
                    if (view_forward.found)
                    {
                        const auto dot = view_dot_2d(view_forward, player_location.value, candidate_location.value);
                        if (dot.valid)
                        {
                            if (dot.dot < kViewConeMinDot) { continue; }
                            candidate_score = dot.dot;  // most-centered wins
                        }
                        else
                        {
                            candidate_score = 1.0;  // NPC on top of the player: treat as centered
                        }
                    }
                }

                if (!best.ability || candidate_score > best_score)
                {
                    if (!has_target)
                    {
                        owner_name = object_name(owner);
                        avatar_name = object_name(avatar);
                    }
                    best_score = candidate_score;
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
        // True if the player can actually perform this station's action - the gate that
        // stops the mod dragging the player to NPC-only "ambient" stations it can never
        // use (which otherwise burned 35 failed take attempts and raised crash exposure).
        // Field-verified discriminator (calibration log 2026-06-14): every player-usable
        // station carries an Action.Crafting.* tag (Forge / WhetStone / Cook.Cauldron /
        // Cook.Pan), while NPC-only activities carry Action.Ambient.* (RoastScavenger /
        // Leatherworking). The subsystem HasPawnAbilityToUseSpot and the static
        // m_UsableByPlayer flag BOTH read false for every station on this build, so the
        // action tag is the only reliable signal. Unknown action -> allow (never block a
        // possibly-usable station on a read failure).
        auto player_can_use_candidate(UObject* root_task) -> bool
        {
            const auto action = read_fname_at(root_task, STR("Action"));
            if (!action.found) { return true; }
            return contains_any(fname_to_text(action.value), {STR("Crafting")});
        }

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
