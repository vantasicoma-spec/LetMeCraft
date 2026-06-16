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
                // v0.9.5: the CLAIM ledger can key on the avatar, not only the owner-state.
                // PAN_08/Shadow11 held its spot claim under the avatar, so the owner-only
                // HandleClaimerDestroyed above never freed it and the player-use combo read
                // unclaimed=0 for all 35 attempts ("spot stuck", take failed). Release the
                // avatar's claims too, mirroring the use-token release just below.
                auto release_avatar_claims = begin_call(subsystem, STR("HandleClaimerDestroyed"), context);
                set_param(release_avatar_claims, STR("Actor"), &avatar_actor, 8, context);
                invoke_call(release_avatar_claims, context);

                auto release_avatar_uses = begin_call(subsystem, STR("HandleUserDestroyed"), context);
                set_param(release_avatar_uses, STR("Actor"), &avatar_actor, 8, context);
                invoke_call(release_avatar_uses, context);
            }

            return true;
        }

        // v1.1.0 PAN_08 TRANSIENT interrupt. SWITCHES the cook NPC's active AI state to the
        // engine's EMPTY daily routine via SwitchAIStateImmediatelyToClass on the ABILITY, so an
        // empty routine becomes the live state - nothing is scheduled, the cook re-claim stops
        // and the NPC idles (exactly the runtime effect the rc5 swap produced and the user
        // confirmed worked, but WITHOUT writing the persistent DailyRoutineClass). rc6 confirmed
        // DoInterruptStateOfClass(Empty) only pushes a temporary sub-state that completes
        // instantly, so the hard schedule re-asserted (NPC kept twitching) - "switch the active
        // state" is the right lever, "interrupt" is not. resume_routine restores the real routine
        // via SwitchToDailyRoutine(). STRICTLY transient: never touches the saved DailyRoutineClass.
        // All reflection via begin_native_call (dodges the IncludeInterfaces throw). FAIL-SAFE:
        // any unresolved link -> no-op, routine_interrupted stays false, so resume stays a no-op.
        auto interrupt_routine(ActiveEviction& eviction) -> void
        {
            if (eviction.routine_interrupted) { return; }  // one-shot

            auto* npc_state = weak_get(eviction.owner);
            if (!is_usable(npc_state)) { return; }
            auto* ai_ability = read_object(npc_state, STR("AIAbility"));
            if (!is_usable(ai_ability)) { return; }

            // Verified benign target (rc5: resolves; rc6b: CharacterAIState subclass). As the
            // ACTIVE state an empty routine idles the NPC; the swap proved it holds (does not
            // complete) for the whole hold.
            static constexpr const TCHAR* kInterruptStateClassPath = STR("/Script/Angelscript.DailyRoutine_Empty");
            auto* state_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, kInterruptStateClassPath);
            if (!is_usable(state_class))
            {
                Output::send<LogLevel::Warning>(
                    STR("[LetMeCraft] interrupt skipped: state class {} unresolved owner={}.\n"),
                    kInterruptStateClassPath, eviction.owner_name);
                return;
            }

            // SwitchAIStateImmediatelyToClass(StateClass) on the ability. parmsSize=16 = the
            // 8-byte StateClass input + an 8-byte return value (the created state ptr, ignored);
            // find_call_property locates StateClass by its own offset, so the layout is handled.
            auto call = begin_native_call(
                ai_ability,
                STR("/Script/G1R.GameplayAbility_CharacterAI:SwitchAIStateImmediatelyToClass"),
                STR("SwitchAIStateImmediatelyToClass"),
                STR("interrupt routine"));
            UObject* state_class_obj = static_cast<UObject*>(state_class);
            set_param(call, STR("StateClass"), &state_class_obj, 8, STR("interrupt routine"));
            if (!invoke_call(call, STR("interrupt routine"))) { return; }  // fail-safe: flag stays false

            eviction.routine_interrupted = true;
            LMC_DLOG(
                STR("[LetMeCraft] routine interrupted (SwitchAIState) owner={} spot={} state={} - active state now empty.\n"),
                eviction.owner_name, fname_to_text(eviction.spot), kInterruptStateClassPath);
        }

        // Restore normal planning. Called on EVERY eviction exit path. One-shot (clears the flag
        // up front so a teardown can never re-fire or strand it). allow_call=false on the teardown
        // paths (map-load clear / avatar-gone / SEH recovery): there the world/ability may be mid-
        // destruction, so we ONLY clear the flag and do NOT issue the SwitchToDailyRoutine
        // ProcessEvent (the interrupt is transient and the live AI is rebuilt from the save on
        // reload, so skipping the call there is correct AND avoids a teardown-time GAS re-entry -
        // the v0.8.11 crash class). allow_call=true ONLY on the healthy finish path.
        auto resume_routine(ActiveEviction& eviction, bool allow_call) -> void
        {
            if (!eviction.routine_interrupted) { return; }
            eviction.routine_interrupted = false;
            if (!allow_call) { return; }

            auto* npc_state = weak_get(eviction.owner);
            if (!is_usable(npc_state)) { return; }
            auto* ai_ability = read_object(npc_state, STR("AIAbility"));
            if (!is_usable(ai_ability)) { return; }

            auto call = begin_native_call(
                ai_ability,
                STR("/Script/G1R.GameplayAbility_CharacterAI:SwitchToDailyRoutine"),
                STR("SwitchToDailyRoutine"),
                STR("resume routine"));
            if (invoke_call(call, STR("resume routine")))
            {
                LMC_DLOG(
                    STR("[LetMeCraft] routine resumed owner={}.\n"), eviction.owner_name);
            }
            else
            {
                Output::send<LogLevel::Warning>(
                    STR("[LetMeCraft] routine resume call failed owner={} - planner self-recovers via its own scheduling.\n"),
                    eviction.owner_name);
            }
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
