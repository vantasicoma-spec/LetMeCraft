#pragma once

#include "Common.hpp"
#include "GameObjects.hpp"

namespace lmc
{
    // The interaction-spot claim/token layer: query whether a spot is free, claim
    // it for the player, hand the token back, and force-release the NPC's claims +
    // use-tokens. The /Script library function and CDO pointers are resolve-once
    // weak-cached (the is_spot_unclaimed poll runs every engine tick); like the
    // move-lock function caches they deliberately survive a world change.
    class SpotClaims
    {
    public:
        explicit SpotClaims(GameObjects& objects) : m_objects(objects) {}

        auto is_spot_unclaimed(const FName& spot) -> bool
        {
            auto* subsystem = m_objects.find_interaction_subsystem();
            if (!subsystem) { return false; }

            // Cached resolve: this runs every engine tick during the claim poll
            // (v0.8.13 - the poll itself must stay) and used to pay two
            // StaticFindObject scans per call. /Script objects are permanent;
            // population happens at first use under the caller's run_guarded, in
            // ticks where these exact lookups already ran before v0.9.0.
            auto* function = static_cast<UFunction*>(weak_get(m_spot_unclaimed_function));
            if (!function)
            {
                function = UObjectGlobals::StaticFindObject<UFunction*>(
                    nullptr, nullptr, STR("/Script/G1R.InteractionSpotHandleLibrary:IsUnclaimed"));
                if (function) { m_spot_unclaimed_function = static_cast<UObject*>(function); }
            }
            auto* library = weak_get_cdo(m_spot_handle_library);
            if (!library)
            {
                library = UObjectGlobals::StaticFindObject<UObject*>(
                    nullptr, nullptr, STR("/Script/G1R.Default__InteractionSpotHandleLibrary"));
                if (library) { m_spot_handle_library = library; }
            }
            if (!function || !library) { return false; }

            auto call = begin_call_with_function(library, function, STR("spot free check"));
            auto handle = spot;
            set_param(call, STR("Handle"), &handle, 8, STR("spot free check"));
            set_param(call, STR("WorldContextObject"), &subsystem, 8, STR("spot free check"));
            if (!invoke_call(call, STR("spot free check"))) { return false; }

            return get_bool_param(call, STR("ReturnValue"), STR("spot free check"));
        }

        // Releases an interaction-spot claim token through the BlueprintFunctionLibrary CDO
        // (is_usable() rejects CDOs by design, hence the weak_get_cdo path). Function and
        // CDO are resolve-once cached like the spot-free pair above.
        auto call_token_library_unclaim(unsigned char (&token)[16], const TCHAR* context) -> bool
        {
            auto* function = static_cast<UFunction*>(weak_get(m_token_unclaim_function));
            if (!function)
            {
                function = UObjectGlobals::StaticFindObject<UFunction*>(
                    nullptr, nullptr, STR("/Script/G1R.InteractionSpotTokenLibrary:Unclaim"));
                if (function) { m_token_unclaim_function = static_cast<UObject*>(function); }
            }
            auto* library = weak_get_cdo(m_token_library);
            if (!library)
            {
                library = UObjectGlobals::StaticFindObject<UObject*>(
                    nullptr, nullptr, STR("/Script/G1R.Default__InteractionSpotTokenLibrary"));
                if (library) { m_token_library = library; }
            }
            if (!function || !library)
            {
                Output::send<LogLevel::Warning>(
                    STR("[LetMeCraft] {}: token library unavailable (function={} cdo={}).\n"),
                    context,
                    function ? STR("ok") : STR("missing"),
                    library ? STR("ok") : STR("missing"));
                return false;
            }

            auto call = begin_call_with_function(library, function, context);
            set_param(call, STR("Handle"), token, 16, context);
            if (!invoke_call(call, context)) { return false; }

            get_param(call, STR("Handle"), token, 16, context);
            return true;
        }

        // HandleClaimerDestroyed drops every spot claim the NPC holds. Proven safe and
        // idempotent (releasing claims of an owner with none is a no-op), and it cannot
        // touch a claim held by the player's G1RPlayerState - so the claim phase, the
        // kill guard and the player-use combo can all call it freely. The name-based
        // begin_call is intentional: the FuncMap throw is specific to the PlayerController,
        // lookups on the InteractionSubsystem are proven working since v0.7.0.
        auto force_release_owner_claims(
            UObject* subsystem, ActiveEviction& eviction, const TCHAR* context) -> bool
        {
            auto* owner_actor = weak_get(eviction.owner);
            if (!subsystem || !owner_actor) { return false; }

            auto release = begin_call(subsystem, STR("HandleClaimerDestroyed"), context);
            set_param(release, STR("Actor"), &owner_actor, 8, context);
            if (!invoke_call(release, context)) { return false; }

            // v0.8.14: the spot keeps a SECOND ledger - active USES (UseSpot tokens),
            // separate from claims. A crafting NPC holds a use token continuously
            // across task restarts, which is why ClaimSpot kept failing with "No
            // tokens left" while the claims list read empty (the v0.8.13 Kharim
            // wars: zero spotFree=true across five full windows - no release gap
            // exists for claims alone). HandleUserDestroyed is the game's own
            // despawn-cleanup for that ledger (actors despawn mid-use routinely,
            // e.g. the observed mid-session NPC recreations), called here for both
            // identities the ledger could key on.
            auto release_owner_uses = begin_call(subsystem, STR("HandleUserDestroyed"), context);
            set_param(release_owner_uses, STR("Actor"), &owner_actor, 8, context);
            invoke_call(release_owner_uses, context);
            if (auto* avatar_actor = weak_get(eviction.avatar))
            {
                auto release_avatar_uses = begin_call(subsystem, STR("HandleUserDestroyed"), context);
                set_param(release_avatar_uses, STR("Actor"), &avatar_actor, 8, context);
                invoke_call(release_avatar_uses, context);
            }

            return true;
        }

        auto try_claim_spot(ActiveEviction& eviction, bool allow_force_release) -> bool
        {
            auto* subsystem = m_objects.find_interaction_subsystem();
            auto* player_state = m_objects.find_player_state();
            if (!subsystem || !player_state) { return false; }

            if (claim_spot_once(subsystem, player_state, eviction)) { return true; }
            if (!allow_force_release) { return false; }

            // Deterministic fallback for the v0.7.0 Kharim case ("No tokens left, spot was
            // already claimed by State_..."): some NPCs never release their token, or
            // re-claim it within the same frame, so polling can never win the race.
            // Release every claim the NPC holds and take the spot in the same game-thread
            // tick. Done only once per eviction: if the claim still fails afterwards, the
            // blocker is a requirement check that no amount of retries can fix.
            if (!force_release_owner_claims(subsystem, eviction, STR("claim"))) { return false; }

            return claim_spot_once(subsystem, player_state, eviction);
        }

    private:
        auto claim_spot_once(UObject* subsystem, UObject* player_state, ActiveEviction& eviction) -> bool
        {
            auto claim = begin_call(subsystem, STR("ClaimSpot"), STR("claim"));
            set_param(claim, STR("Spot"), &eviction.spot, 8, STR("claim"));
            set_param(claim, STR("User"), &player_state, 8, STR("claim"));
            set_param(claim, STR("Action"), &eviction.action, 8, STR("claim"));
            if (!invoke_call(claim, STR("claim"))) { return false; }
            get_param(claim, STR("ReturnValue"), eviction.claim_token, 16, STR("claim"));

            auto verify = begin_call(subsystem, STR("IsSpotClaimedBy"), STR("claim verify"));
            set_param(verify, STR("Spot"), &eviction.spot, 8, STR("claim verify"));
            set_param(verify, STR("User"), &player_state, 8, STR("claim verify"));
            if (!invoke_call(verify, STR("claim verify"))) { return false; }
            return get_bool_param(verify, STR("ReturnValue"), STR("claim verify"));
        }

        GameObjects& m_objects;
        FWeakObjectPtr m_spot_unclaimed_function{};
        FWeakObjectPtr m_spot_handle_library{};
        FWeakObjectPtr m_token_unclaim_function{};
        FWeakObjectPtr m_token_library{};
    };
}
