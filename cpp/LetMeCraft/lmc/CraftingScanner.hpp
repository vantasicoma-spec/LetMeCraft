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

        // The expensive primitive: FindAllOf walks the ENTIRE GUObjectArray and, for nearly every
        // object, its whole super-struct chain (UObjectGlobals.cpp). Pulled out so callers can cache it.
        // The manual E/Y path scans fresh per press (correctness); the continuous glyph-prompt path
        // caches the result and refreshes it on a ~1.5s timer (see dllmain update_glyph), which removes
        // the per-250ms full walk that was the residual stutter.
        auto find_crafting_abilities(std::vector<UObject*>& out) -> void
        {
            UObjectGlobals::FindAllOf(STR("GameplayAbilityInteractFreePoint"), out);
        }

        // Display-only fast path for the floating prompt (refreshed ~1.5s). FindAllOf is costly because
        // it walks each object's whole super-struct chain comparing names; an EXACT class-pointer compare
        // is one comparison per object (no chain walk) - ~5-10x cheaper per call. We keep the manual E
        // path on find_crafting_abilities (FindAllOf) so eviction correctness never depends on this; the
        // worst case here is the prompt not showing for an exotic subclass (E still works). Falls back to
        // FindAllOf if the class can't be resolved, making it identical to the old behavior.
        auto find_crafting_abilities_for_prompt(std::vector<UObject*>& out) -> void
        {
            auto* cls = resolve_interact_ability_class();
            if (!cls) { find_crafting_abilities(out); return; }
            UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) {
                if (obj && obj->GetClassPrivate() == cls && is_usable(obj)) { out.emplace_back(obj); }
                return LoopAction::Continue;
            });
        }

        // prefetched: if non-null, evaluate THIS list instead of doing a fresh FindAllOf (the glyph
        // prompt passes its ~1.5s-refreshed cache). Stale pointers in it are safely dropped by the
        // per-ability is_usable() gate below.
        auto select_crafting_candidate(
            const TCHAR* source,
            const StringType* target_owner_name,
            const StringType* target_avatar_name,
            bool require_player_range = true,
            const std::vector<UObject*>* prefetched = nullptr) -> CraftingSearchResult
        {
            if (!Unreal::IsInGameThreadRaw())
            {
                Output::send<LogLevel::Error>(
                    STR("[LetMeCraft] {} request aborted: GAS scan attempted off game thread.\n"),
                    source);
                return {};
            }

            std::vector<UObject*> scanned{};
            if (!prefetched) { find_crafting_abilities(scanned); }
            const std::vector<UObject*>& abilities = prefetched ? *prefetched : scanned;

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

        // Diagnostic only (no state change): when a manual press finds NO candidate, enumerate the
        // active crafting abilities and the gate values for each, so a stuck station (e.g. the Snaf
        // cauldron that "won't give up" and where E does nothing) reveals WHY it was rejected. The
        // caller wraps this in invoke_with_seh - object_name on a recycled ability can fault.
        auto diagnose_no_candidate(const TCHAR* source) -> void
        {
            if (!Unreal::IsInGameThreadRaw()) { return; }

            std::vector<UObject*> abilities{};
            UObjectGlobals::FindAllOf(STR("GameplayAbilityInteractFreePoint"), abilities);
            const auto player_location = read_actor_location(m_objects.find_player_actor_cached());

            const auto yn = [](bool found, bool value) -> const TCHAR* {
                return found ? (value ? STR("Y") : STR("n")) : STR("?");
            };

            int logged = 0;
            for (auto* ability : abilities)
            {
                if (logged >= 12 || !is_usable(ability)) { continue; }
                auto* root_task = read_object(ability, STR("RootInteractionTask"));
                if (!root_task || !root_task_is_crafting(root_task)) { continue; }

                auto* avatar = find_avatar(ability, root_task);
                auto* owner = find_owner(ability, root_task);
                auto* candidate_actor = find_candidate_actor(avatar, owner);
                const auto npc_loc = read_actor_location(candidate_actor);
                int npc_dist = -1;
                if (player_location.found && npc_loc.found)
                {
                    npc_dist = static_cast<int>(std::sqrt(distance_squared(player_location.value, npc_loc.value)));
                }

                const auto active = read_bool(ability, STR("bIsActive"));
                const auto cancelable = read_bool(ability, STR("bIsCancelable"));
                const auto end_requested = read_bool(ability, STR("bEndRequested"));
                const auto action = read_fname_at(root_task, STR("Action"));

                ++logged;
                LMC_DLOG(
                    STR("[LetMeCraft] {} no-candidate diag: owner={} active={} cancelable={} endReq={} action={} npcDist={}m.\n"),
                    source,
                    object_name(owner),
                    yn(active.found, active.value),
                    yn(cancelable.found, cancelable.value),
                    yn(end_requested.found, end_requested.value),
                    action.found ? fname_to_text(action.value) : StringType{STR("?")},
                    npc_dist);
            }

            if (logged == 0)
            {
                LMC_DLOG(
                    STR("[LetMeCraft] {} no-candidate diag: NO active+crafting interact ability exists right now (the NPC is likely idle / not crafting - e.g. a bricked or unstarted routine).\n"),
                    source);
            }
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
        // Resolve the interaction-ability UClass once (weak-cached). One-time name scan; thereafter a
        // pointer in the cache. Used only by the display-only prompt fast path.
        auto resolve_interact_ability_class() -> UClass*
        {
            if (auto* cached = weak_get(m_interact_ability_class)) { return static_cast<UClass*>(cached); }
            UClass* found = nullptr;
            const FName target{STR("GameplayAbilityInteractFreePoint"), FNAME_Find};
            UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) {
                if (auto* cls = Cast<UClass>(obj))
                {
                    if (cls->GetNamePrivate() == target) { found = cls; return LoopAction::Break; }
                }
                return LoopAction::Continue;
            });
            if (found) { m_interact_ability_class = static_cast<UObject*>(found); }
            return found;
        }

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
        FWeakObjectPtr m_interact_ability_class{}; // cached UClass for the prompt's exact-class fast scan
    };
}
