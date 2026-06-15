#pragma once

#include "Common.hpp"
#include "GameObjects.hpp"
#include "MovementLock.hpp"
#include "PlayerSensor.hpp"
#include "SpotClaims.hpp"
#include "CraftingScanner.hpp"
#include "RoutineBlocker.hpp"

namespace lmc
{
    // The eviction state machine: on a manual request it finds the occupied
    // crafting station, cancels the NPC's ability, parks the NPC behind the
    // player and auto-walks the player in to take the station. Each live
    // displacement is one ActiveEviction in m_evictions, ticked every engine
    // frame. Holds references to the service components it orchestrates
    // (object cache, movement lock, sensor, spot claims, scanner, blocker).
    class EvictionManager
    {
    public:
        EvictionManager(GameObjects& objects, MovementLock& movement, PlayerSensor& sensor,
                        SpotClaims& claims, CraftingScanner& scanner, RoutineBlocker& blocker)
            : m_objects(objects), m_movement(movement), m_sensor(sensor),
              m_claims(claims), m_scanner(scanner), m_blocker(blocker)
        {
        }

        auto empty() const -> bool { return m_evictions.empty(); }

        // True when one of the active evictions is already displacing this avatar. Read-only;
        // used by the floating prompt to hide while an eviction on that NPC is in progress.
        auto is_evicting_avatar(UObject* avatar) const -> bool
        {
            if (!avatar) { return false; }
            for (const auto& eviction : m_evictions)
            {
                if (weak_get(eviction.avatar) == avatar) { return true; }
            }
            return false;
        }

        // Drop all evictions, removing each routine-blocker tag first (the world that
        // owned the ASCs is being torn down). Called from clear_transient_state on
        // map load / game-state init / SEH recovery.
        auto clear(const TCHAR* reason) -> void
        {
            // v0.8.16: best-effort tag removal before the evictions are dropped -
            // on LoadMap-pre the old ASCs are still alive; on InitGameState-post
            // weak_get resolves to null and this is a no-op.
            for (auto& eviction : m_evictions)
            {
                run_guarded(STR("routine blocker clear"), [&] {
                    m_blocker.remove_routine_blocker(eviction, reason);
                });
                // Map-load/teardown: clear the interrupt flag WITHOUT calling into the AI on a
                // possibly-dying world (allow_call=false). The live AI rebuilds from the save.
                run_guarded(STR("resume routine clear"), [&] { m_claims.resume_routine(eviction, false); });
            }
            m_evictions.clear();
        }

        // Re-enable interaction on any NPC left force-disabled by an aborted tick
        // (SEH crash recovery), so a crashed eviction never strands an NPC.
        auto reenable_disabled_npcs() -> void
        {
            for (auto& eviction : m_evictions)
            {
                // Crash recovery: clear any interrupt flag WITHOUT a ProcessEvent (allow_call=
                // false) - the tick already faulted, so do not re-enter the AI on this world.
                run_guarded(STR("crash recovery resume routine"), [&] { m_claims.resume_routine(eviction, false); });
                if (!eviction.npc_interaction_disabled) { continue; }
                run_guarded(STR("crash recovery npc re-enable"), [&] { set_npc_interaction_enabled(eviction, true); });
            }
        }
        auto request_end_nearest_crafter_once(const TCHAR* source) -> void
        {
            if (!Unreal::IsInGameThreadRaw())
            {
                Output::send<LogLevel::Error>(
                    STR("[LetMeCraft] {} request ignored: not running on game thread.\n"),
                    source);
                return;
            }

            // bShowMouseCursor is the cheapest available "UI is open" signal; whether the
            // Gothic menus actually set it is checklist item T9 of the test protocol.
            if (auto* player_controller = m_objects.find_player_controller_cached())
            {
                const auto cursor = read_bool(player_controller, STR("bShowMouseCursor"));
                if (cursor.found && cursor.value) { return; }
            }

            const auto now = Clock::now();
            if (now < m_next_manual_request_time) { return; }

            m_next_manual_request_time = now + kManualRequestCooldown;

            const auto result = m_scanner.request_end_matching_crafter(source);
            if (result.called)
            {
                // Own guard: a throw inside start_eviction (the v0.7.4 FuncMap bug
                // skipped the move lock) must not escape to the tick handler.
                run_guarded(STR("start eviction"), [&] { start_eviction(result.candidate, source, now); });
                return;
            }

            if (auto* eviction = find_nearby_eviction())
            {
                run_guarded(STR("refresh eviction"), [&] { refresh_eviction(*eviction, source, now); });
                return;
            }

            // Fires only on an actual press (cooldown-gated), so no spam: confirms the
            // v0.9.1 gates rejected the press - no occupied crafting station that the
            // player can use is in their view cone.
            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] {} press ignored: no usable crafting station in view.\n"),
                source);

            // Diagnostic: log nearby crafting abilities + their gate values so a stuck station (the
            // Snaf cauldron where E does nothing) reveals which gate rejected it. SEH-guarded because
            // object_name on a recycled ability can fault - and there is no active eviction to protect
            // here, so a fault must not reach the top-level guard.
            SehCrashInfo diag_crash{};
            invoke_with_seh([&] { m_scanner.diagnose_no_candidate(source); }, diag_crash);
        }





        auto find_nearby_eviction() -> ActiveEviction*
        {
            auto* player = m_objects.find_player_actor_cached();
            const auto player_location = read_actor_location(player);
            if (!player_location.found) { return nullptr; }

            ActiveEviction* best{};
            auto best_distance_squared = std::numeric_limits<double>::max();
            for (auto& eviction : m_evictions)
            {
                auto* actor = weak_get(eviction.avatar);
                if (!actor) { continue; }

                const auto actor_location = read_actor_location(actor);
                if (!actor_location.found) { continue; }

                const auto current_distance_squared = distance_squared(player_location.value, actor_location.value);
                if (current_distance_squared > kHeldFallbackDistanceSquared) { continue; }
                if (current_distance_squared >= best_distance_squared) { continue; }

                best = &eviction;
                best_distance_squared = current_distance_squared;
            }

            return best;
        }

        auto refresh_eviction(ActiveEviction& eviction, const TCHAR* source, Clock::time_point now) -> void
        {
            eviction.resume_at = now + kHoldDuration;
            // Repeat E re-arms the auto-take too: a fresh interaction window and a
            // fresh move lock, exactly like a new eviction would get. The approach
            // restarts as well - the player may have wandered off since the last
            // window, and the arrival gate must hold attempts until they are back.
            eviction.player_use_until = now + kMoveLockDuration;
            eviction.player_use_done = false;
            // Repeat E re-arms the bridge, so the kill guard must run again - and
            // the attempt counter restarts so the v0.8.12 cap measures THIS window.
            eviction.player_took_station = false;
            eviction.player_use_attempts = 0;
            eviction.approach_done = false;
            eviction.approach_stop_distance = kApproachStopDistance;
            eviction.approach_grace_until = now + kApproachActivationGrace;
            run_guarded(STR("move lock"), [&] { m_movement.lock_player_movement(); });

            // v0.8.16: the tag normally survives a repeat-E (the eviction object is
            // reused) - never double-add; re-add only when it verifiably vanished.
            run_guarded(STR("routine blocker refresh"), [&] {
                if (!eviction.blocker_known) { return; }
                if (!eviction.blocker_applied)
                {
                    m_blocker.apply_routine_blocker(eviction);
                    return;
                }
                auto* asc = weak_get(eviction.asc);
                if (!asc) { return; }
                auto check = begin_call(asc, STR("HasTagExact"), STR("routine blocker refresh"));
                set_param(check, STR("GameplayTag"), &eviction.blocker_tag, 8, STR("routine blocker refresh"));
                if (invoke_call(check, STR("routine blocker refresh")) &&
                    !get_bool_param(check, STR("ReturnValue"), STR("routine blocker refresh")))
                {
                    eviction.blocker_applied = false;
                    m_blocker.apply_routine_blocker(eviction);
                }
            });

            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] {} eviction refreshed owner={} avatar={} spot={} holdMs={}.\n"),
                source,
                eviction.owner_name,
                eviction.avatar_name,
                fname_to_text(eviction.spot),
                std::chrono::duration_cast<std::chrono::milliseconds>(kHoldDuration).count());
        }

        auto start_eviction(const CraftingCandidate& candidate, const TCHAR* source, Clock::time_point now) -> void
        {
            // Capture everything suppression needs while the transient GAS objects are
            // still alive: spot name, action tag, station actor, AI controller.
            const auto root_spot = read_fname_at(candidate.root_task, STR("Spot"));
            const auto ability_spot = read_fname_at(candidate.ability, STR("m_InteractionSpot"));
            const auto action = read_fname_at(candidate.root_task, STR("Action"));
            auto* station = read_object(candidate.ability, STR("m_InteractiveActor"));
            auto* controller = m_scanner.find_ai_controller_from_candidate(candidate);

            for (auto& eviction : m_evictions)
            {
                if (!eviction.owner_name.empty() && eviction.owner_name == candidate.owner_name)
                {
                    refresh_eviction(eviction, source, now);
                    return;
                }
            }

            ActiveEviction eviction{};
            eviction.move_at = now + kRetreatDelay;
            eviction.stand_still_from = now + kStandStillDelay;
            eviction.next_enforce_at = now + kStandStillDelay;
            eviction.next_claim_at = now + kClaimRetryInterval;
            eviction.player_use_at = now + kPlayerUseDelay;
            eviction.approach_grace_until = now + kApproachActivationGrace;
            eviction.player_use_until = now + kMoveLockDuration;
            eviction.resume_at = now + kHoldDuration;
            eviction.owner = candidate.owner;
            eviction.avatar = candidate.avatar;
            eviction.controller = controller;
            eviction.station = station;
            eviction.ability = candidate.ability;
            eviction.asc = candidate.asc;
            eviction.spot = root_spot.found ? root_spot.value : ability_spot.value;
            eviction.spot_known = root_spot.found || ability_spot.found;
            eviction.action = action.value;
            eviction.owner_name = candidate.owner_name;
            eviction.avatar_name = candidate.avatar_name;
            run_guarded(STR("routine blocker select"), [&] { m_blocker.select_routine_blocker(eviction, candidate); });
            m_evictions.push_back(eviction);

            // Free the spot right away: in v0.8.0 the first force-release came only
            // with the claim phase (~2s in), so the station stayed locked by the
            // NPC's claim for the first seconds of every eviction.
            run_guarded(STR("initial spot release"), [&] {
                if (auto* subsystem = m_objects.find_interaction_subsystem())
                {
                    m_claims.force_release_owner_claims(subsystem, m_evictions.back(), STR("eviction start"));
                }
            });

            // v0.8.16: block re-activation at the source - the routine restarts its
            // interaction task within the SAME world tick as our force-release,
            // which the per-tick claim poll can never win (the v0.8.15 35-cancel
            // wars on Kharim/Snaf).
            run_guarded(STR("routine blocker apply"), [&] { m_blocker.apply_routine_blocker(m_evictions.back()); });

            // v1.1.0: TRANSIENT interrupt for the firmly-scheduled campfire-cook case
            // (PAN_08/Shadow11). Pushes an empty routine onto the NPC's LIVE routine instance so
            // its planner stops re-claiming the pan; the retreat + claim-poll then take it. Cook-
            // only (forge/anvil/whetstone take cleanly on the proven path, untouched). Does NOT
            // touch the saved DailyRoutineClass; resumed on every exit path. Fail-safe inside.
            if (action.found && contains_any(fname_to_text(eviction.action), {STR("Cook")}))
            {
                run_guarded(STR("interrupt routine"), [&] { m_claims.interrupt_routine(m_evictions.back()); });
            }

            run_guarded(STR("move lock"), [&] { m_movement.lock_player_movement(); });

            // Snap the view at the station right away: the sensor's sensed set only
            // admits a station while it is usable AND inside the view cone, and the
            // ~1s between eviction start and the first take attempt is the window
            // where both finally hold (v0.8.5 log: component healthy, sensor null -
            // the player simply was not looking at the small station while its set
            // could still update).
            run_guarded(STR("face station early"), [&] { face_station(station); });

            run_guarded(STR("eviction started log"), [&] {
                Output::send<LogLevel::Verbose>(
                    STR("[LetMeCraft] {} eviction started owner={} avatar={} spot={} action={} controller={} station={} holdMs={} activeEvictions={}.\n"),
                    source,
                    candidate.owner_name,
                    candidate.avatar_name,
                    eviction.spot_known ? fname_to_text(eviction.spot) : StringType{STR("<unknown>")},
                    action.found ? fname_to_text(eviction.action) : StringType{STR("<none>")},
                    object_name(controller),
                    object_name(station),
                    std::chrono::duration_cast<std::chrono::milliseconds>(kHoldDuration).count(),
                    m_evictions.size());
            });
        }


        // The activation targets the interactive nearest to the player and ignores both
        // the frozen sensor and m_FilterActor (v0.7.10-0.7.12 logs), and fighting the
        // NPC's movement lost every time (the routine overrides MoveToLocation within a
        // tick - v0.7.12: the NPC hovered at 0.7-1.7m for the whole window). So the NPC
        // itself is made non-interactive for the eviction window instead:
        // AGothicCharacter::m_InteractiveComponent (@0x9B0) exposes
        // SetForceDisableInteraction(bool) - the game's own temporary hide-from-
        // interaction switch. Deliberately NOT SetCanBeUsed: that one takes a 'save'
        // parameter and persists into savegames.
        auto set_npc_interaction_enabled(ActiveEviction& eviction, bool enabled) -> bool
        {
            auto* avatar = weak_get(eviction.avatar);
            auto* component = read_object(avatar, STR("m_InteractiveComponent"));
            if (!component) { return false; }

            auto call = begin_call(component, STR("SetForceDisableInteraction"), STR("npc interaction toggle"));
            set_bool_param(call, STR("Value"), !enabled, STR("npc interaction toggle"));
            const auto called = invoke_call(call, STR("npc interaction toggle"));
            if (!called)
            {
                // Fallback belt: write the flag directly - the property exists even if
                // the function-call path fails.
                auto* property = CastField<FBoolProperty>(
                    component->GetPropertyByNameInChain(STR("m_ForceDisableInteraction")));
                if (!property) { return false; }
                property->SetPropertyValueInContainer(component, !enabled);
            }

            eviction.npc_interaction_disabled = !enabled;
            return true;
        }

        auto issue_retreat(ActiveEviction& eviction) -> void
        {
            auto* avatar = weak_get(eviction.avatar);
            auto* controller = weak_get(eviction.controller);
            const auto avatar_location = read_actor_location(avatar);
            if (!avatar || !controller || !avatar_location.found) { return; }

            // Destination: behind the player's back (user redesign v0.8.0). The player
            // faces the station when pressing E, so the point on the station->player
            // ray extended kBehindPlayerDistance past the player is behind them. The
            // point is captured once per eviction and reused by every re-issue.
            if (!eviction.retreat_dest_known)
            {
                const auto player_location = read_actor_location(m_objects.find_player_actor_cached());
                if (!player_location.found) { return; }

                auto dx = 0.0;
                auto dy = 0.0;
                const auto station_location = read_actor_location(weak_get(eviction.station));
                if (station_location.found)
                {
                    dx = player_location.value.X() - station_location.value.X();
                    dy = player_location.value.Y() - station_location.value.Y();
                }
                auto length = std::sqrt(dx * dx + dy * dy);
                if (length < 1.0)
                {
                    // No station actor: walk the NPC->player direction past the player.
                    dx = player_location.value.X() - avatar_location.value.X();
                    dy = player_location.value.Y() - avatar_location.value.Y();
                    length = std::sqrt(dx * dx + dy * dy);
                }
                if (length < 1.0)
                {
                    dx = 1.0;
                    dy = 0.0;
                    length = 1.0;
                }

                eviction.retreat_dest = FVector{
                    player_location.value.X() + (dx / length) * kBehindPlayerDistance,
                    player_location.value.Y() + (dy / length) * kBehindPlayerDistance,
                    player_location.value.Z()};
                eviction.retreat_dest_known = true;
            }

            issue_move_to_location(controller, eviction.retreat_dest, STR("retreat"));
        }





        // The sensor's live target query filters by UInteractiveComponent::IsUsable().
        // The NPC's crafting marks the station's component m_IsBeingUsed=true and our
        // hard cancel (OnRequestEndQuick + claim force-release) skips the vanilla
        // teardown that resets it - the v0.8.4 diag showed nearest=<null> at the
        // whetstone/cauldron even with the view snapped at them from 1.9m, while the
        // multi-spot forge survived. Read the component's flags (diagnostics) and
        // clear the transient ones; SetCanBeUsed is NEVER called - its 'save' param
        // persists into savegames (closed track).
        auto make_station_usable(ActiveEviction& eviction, UObject* station) -> void
        {
            auto* component = read_object(station, STR("m_InteractiveComponent"));
            if (!component)
            {
                if (eviction.player_use_attempts <= 1)
                {
                    Output::send<LogLevel::Warning>(
                        STR("[LetMeCraft] station usable skipped: no m_InteractiveComponent on station spot={}.\n"),
                        fname_to_text(eviction.spot));
                }
                return;
            }

            const auto is_being_used = read_bool(component, STR("m_IsBeingUsed"));
            const auto can_be_used = read_bool(component, STR("m_CanBeUsed"));
            const auto force_disabled = read_bool(component, STR("m_ForceDisableInteraction"));

            if (is_being_used.found && is_being_used.value)
            {
                auto call = begin_call(component, STR("SetIsBeingUsed"), STR("station usable"));
                set_bool_param(call, STR("Value"), false, STR("station usable"));
                if (!invoke_call(call, STR("station usable")))
                {
                    auto* property = CastField<FBoolProperty>(
                        component->GetPropertyByNameInChain(STR("m_IsBeingUsed")));
                    if (property) { property->SetPropertyValueInContainer(component, false); }
                }
            }

            if (force_disabled.found && force_disabled.value)
            {
                auto call = begin_call(component, STR("SetForceDisableInteraction"), STR("station usable"));
                set_bool_param(call, STR("Value"), false, STR("station usable"));
                if (!invoke_call(call, STR("station usable")))
                {
                    auto* property = CastField<FBoolProperty>(
                        component->GetPropertyByNameInChain(STR("m_ForceDisableInteraction")));
                    if (property) { property->SetPropertyValueInContainer(component, false); }
                }
            }

            if (can_be_used.found && !can_be_used.value && eviction.player_use_attempts <= 2)
            {
                Output::send<LogLevel::Warning>(
                    STR("[LetMeCraft] station canBeUsed=false spot={} - not touching (SetCanBeUsed persists to saves).\n"),
                    fname_to_text(eviction.spot));
            }
        }

        // The interact ability resolves its target through the player's LOOK-based
        // live sensor query (UInteractSensor carries player/object peripheral-vision
        // cosines): with the view turned away the query returns null and TryActivate
        // fails - the v0.8.3 diag showed canInteract=false nearest=<null> for whole
        // 9s windows. Turn the controller toward the station before every take
        // attempt; the user pressed E on this station, so snapping the view to it
        // during the short locked window is the expected outcome anyway.
        auto face_station(UObject* station) -> void
        {
            auto* controller = m_objects.find_player_controller_cached();
            const auto player_location = read_actor_location(m_objects.find_player_actor_cached());
            const auto station_location = read_actor_location(station);
            if (!controller || !player_location.found || !station_location.found) { return; }

            const auto dx = station_location.value.X() - player_location.value.X();
            const auto dy = station_location.value.Y() - player_location.value.Y();
            if (dx * dx + dy * dy < 1.0) { return; }

            struct FRotatorMirror
            {
                double pitch;
                double yaw;
                double roll;
            } rotation{0.0, std::atan2(dy, dx) * 180.0 / 3.14159265358979323846, 0.0};

            auto call = begin_native_call(
                controller,
                STR("/Script/Engine.Controller:SetControlRotation"),
                STR("SetControlRotation"),
                STR("face station"));
            set_param(call, STR("NewRotation"), &rotation, sizeof(rotation), STR("face station"));
            invoke_call(call, STR("face station"));
        }

        // Walk the player up to the station: every station's component reports
        // interactDist=250 (v0.8.6 diag), so a player standing beyond 2.5m can
        // never be sensed - both v0.8.6 misses started at 3.0-3.2m. Movement input
        // is fed every engine tick while the take window is open; bForce=true
        // bypasses our own SetIgnoreMoveInput lock (which keeps blocking the
        // user's input), so the mod alone steers the character.
        auto drive_player_toward_station(ActiveEviction& eviction) -> void
        {
            auto* station = weak_get(eviction.station);
            auto* pawn = m_objects.find_player_actor_cached();
            const auto player_location = read_actor_location(pawn);
            const auto station_location = read_actor_location(station);
            if (!pawn || !station || !player_location.found || !station_location.found) { return; }

            const auto dx = station_location.value.X() - player_location.value.X();
            const auto dy = station_location.value.Y() - player_location.value.Y();
            const auto distance_2d_squared = dx * dx + dy * dy;
            const auto stop_squared =
                eviction.approach_stop_distance * eviction.approach_stop_distance;
            if (distance_2d_squared <= stop_squared)
            {
                // Mark done even when no walking was needed (E pressed up close) -
                // the arrival gate keys off this flag and must not sit out the
                // whole grace in that case.
                eviction.approach_done = true;
                return;
            }

            const auto length = std::sqrt(distance_2d_squared);
            struct FVectorMirror
            {
                double x;
                double y;
                double z;
            } direction{dx / length, dy / length, 0.0};

            auto call = begin_native_call(
                pawn,
                STR("/Script/Engine.Pawn:AddMovementInput"),
                STR("AddMovementInput"),
                STR("approach station"));
            set_param(call, STR("WorldDirection"), &direction, sizeof(direction), STR("approach station"));
            float scale = 1.0f;
            set_param(call, STR("ScaleValue"), &scale, sizeof(scale), STR("approach station"));
            set_bool_param(call, STR("bForce"), true, STR("approach station"));
            invoke_call(call, STR("approach station"));
        }

        // "The player steps up to the station": wait until the interaction spot is
        // actually free (or held by our own claim), then start the player's interact
        // ability with the station as the explicit target.
        auto try_player_use_station(ActiveEviction& eviction) -> bool
        {
            auto* station = weak_get(eviction.station);
            auto* player_state = m_objects.find_player_state();
            auto* asc = read_object(player_state, STR("AbilitySystemComponent"));
            auto* ability_instance = m_objects.find_player_interact_ability_instance(player_state);
            auto* ability_class = ability_instance ? ability_instance->GetClassPrivate() : nullptr;
            if (!station || !asc || !ability_class)
            {
                return false;
            }

            // Same-tick combo: cancel the NPC's reacquired crafting ability, drop its spot
            // claims and activate the player's interaction inside ONE game-thread tick.
            // The AI routine can only react on a later tick, so the re-claim race that
            // v0.7.4 lost on WhetStone/Cauldron cannot interleave.
            // v0.8.3: the cancel half runs on EVERY attempt - even when the spot
            // reads free and even while we hold our own claim. The v0.8.2 log
            // proved the routine re-activates its crafting ability within
            // ~50-250ms of a cancel, and that ACTIVE (not yet claiming) ability
            // blocks the player's TryActivate: every success across v0.8.1/0.8.2
            // landed within ~150ms of a cancel, while burst windows where the
            // free spot made the combo skip its cancels produced 10-80 straight
            // activated=false over 1-9 seconds.
            // v0.9.5: hoisted above the combo block so the stuck-claim claim-over below
            // can flag the claim as acquired-this-tick for the handoff guard.
            auto claimed_this_tick = false;
            {
                // Evaluate the cached ability instead of a full-object scan; the
                // scan remains only as a fallback for a dead weak ptr (unseen so
                // far - the instance survives the whole eviction in every log).
                auto search = m_scanner.evaluate_eviction_ability(eviction);
                if (!search.found && !weak_get(eviction.ability))
                {
                    search = m_scanner.select_crafting_candidate(
                        STR("player use combo"),
                        &eviction.owner_name,
                        &eviction.avatar_name,
                        false);
                    if (search.found)
                    {
                        eviction.ability = search.candidate.ability;
                        Output::send<LogLevel::Warning>(
                            STR("[LetMeCraft] cached eviction ability died, re-found via scan owner={}.\n"),
                            eviction.owner_name);
                    }
                }
                if (search.found)
                {
                    // Full 10Hz cancel cadence (the v0.8.13 1-in-3 throttle is
                    // reverted: it eased the pressure that made sticky routines
                    // give up - Kharim went 0-for-5 windows under it after winning
                    // on the 3rd window without it).
                    m_scanner.call_request_end_quick(search.candidate);
                }

                if (!eviction.has_claim)
                {
                    m_claims.force_release_owner_claims(
                        m_objects.find_interaction_subsystem(), eviction, STR("player use combo"));

                    // With an unknown spot freeness cannot be verified - proceed
                    // anyway, same as the v0.7.4 gate did. Still held: retry. After
                    // a cancel the fast (100ms) recheck lands inside the release
                    // window (the claim frees ~100ms after a cancel, the routine
                    // re-claims in ~250ms - v0.8.1 log).
                    if (eviction.spot_known && !m_claims.is_spot_unclaimed(eviction.spot))
                    {
                        // The spot still reads claimed after the force-release. A few NPCs
                        // (PAN_08/Shadow11 while FIRMLY scheduled to cook there) hold a
                        // persistent daily-routine reservation that no SAFE API yields: the
                        // routine re-issues its interaction task ~every 250ms. Tried and
                        // rejected: subsystem release, NotifyDoneWithAction/Idle,
                        // CancelAllExceptRootState (all execute, claim stays); raising the
                        // spot's NumAllowedUsers (template field, no effect); and swapping the
                        // NPC's daily routine to Empty (worked in-session but rewrote the saved
                        // routine class -> NPC loaded broken after a reload; withdrawn). So
                        // PAN_08-when-firmly-scheduled is a documented known limitation. Still
                        // try the player's OWN ClaimSpot - it grabs the spot the moment the
                        // routine itself releases it, bounded by the 70-attempt out-wait window.
                        run_guarded(STR("claim over stuck"), [&] {
                            if (m_claims.try_claim_spot(eviction, true))
                            {
                                eviction.has_claim = true;
                                claimed_this_tick = true;
                            }
                        });
                        if (!eviction.has_claim)
                        {
                            return false;
                        }
                    }
                }
            }

            // Claim the spot for the player in the SAME tick as the cancel above -
            // the routine can only react next tick (the v0.8.3 combo principle).
            // The standalone claim phase ran on its own 500ms timer and missed the
            // ~100-250ms post-cancel free window 2-3 times per eviction; the user
            // saw that as the station "unlocking for milliseconds" until claim ok.
            // The v0.8.9 log: the successful activation always lands on the attempt
            // right AFTER claim ok - the claim has to survive one world tick before
            // the game reads the spot as available to the player.
            if (eviction.spot_known && !eviction.has_claim)
            {
                run_guarded(STR("inline claim"), [&] {
                    if (m_claims.try_claim_spot(eviction, false))
                    {
                        eviction.has_claim = true;
                        claimed_this_tick = true;
                    }
                });
            }

            // Activation gate (v0.8.15): the activation runs its own live
            // nearest-interactive query that the pins below cannot override
            // (v0.7.10), and in a station cluster a NEIGHBOR can win it - Huno's
            // anvil lost to the adjacent forge at the 2.0m stop and the attempt=1
            // activation walked the player to the WRONG station (v0.8.14 log).
            // No activation while a DIFFERENT non-null interactive is nearest; while a
            // neighbor holds it, shrink the approach stop so the drive creeps the player
            // in until the station is the closest pick. Placed BEFORE the handoff so a
            // gated attempt keeps holding our claim (no release/re-claim churn).
            // v0.9.5: only gate on a non-null neighbor. A small station seen from the
            // front/side returns nearest=<null> (PAN_08/Shadow11 - log 2026-06-14): there
            // is no wrong target to grab, so let the pinned activation try; the
            // target-mismatch verify below still ends a wrong grab. This is the
            // "works from behind, breaks from front/side" case the user reported.
            if (auto* station_actor = weak_get(eviction.station))
            {
                if (auto* sensor = m_objects.find_player_sensor())
                {
                    auto nearest_call = begin_call(sensor, STR("GetNearestInteraction"), STR("activation gate"));
                    UObject* nearest = nullptr;
                    if (invoke_call(nearest_call, STR("activation gate")))
                    {
                        nearest = get_object_param(nearest_call, STR("ReturnValue"), STR("activation gate"));
                    }
                    if (nearest && nearest != station_actor)
                    {
                        // Keep the view aimed at the station - the live query is
                        // look-based and the pin block below does not run on a
                        // gated attempt.
                        run_guarded(STR("face station (gated)"), [&] { face_station(station_actor); });
                        if (nearest && eviction.approach_stop_distance > kApproachMinStopDistance)
                        {
                            eviction.approach_stop_distance = std::max(
                                kApproachMinStopDistance,
                                eviction.approach_stop_distance - kApproachStopShrinkStep);
                            eviction.approach_done = false;
                        }
                        return false;
                    }
                }
            }

            // Hand our claim over right before the player's own interaction claims
            // it - but never release a claim acquired THIS tick (it has not been
            // seen by a world tick yet; releasing it here would just re-open the
            // routine's re-claim race the inline claim exists to close).
            if (eviction.has_claim && !claimed_this_tick &&
                m_claims.call_token_library_unclaim(eviction.claim_token, STR("unclaim (handoff)")))
            {
                eviction.has_claim = false;
            }

            // Fallback gate, active only when the NPC's interactivity could NOT be
            // force-disabled: never activate while the evicted NPC stands next to the
            // player - the activation targets the nearest interactive and would grab
            // the NPC (v0.7.11). With the disable in effect (v0.7.13) the NPC is
            // invisible to the interaction search and no waiting is needed.
            if (auto* avatar = eviction.npc_interaction_disabled ? nullptr : weak_get(eviction.avatar))
            {
                const auto avatar_location = read_actor_location(avatar);
                const auto player_location = read_actor_location(m_objects.find_player_actor_cached());
                if (avatar_location.found && player_location.found)
                {
                    const auto npc_distance_squared =
                        distance_squared(avatar_location.value, player_location.value);
                    if (npc_distance_squared < kNpcClearDistanceSquared)
                    {
                        const auto now = Clock::now();
                        if (now >= eviction.re_retreat_at)
                        {
                            eviction.re_retreat_at = now + kReRetreatInterval;
                            run_guarded(STR("re-retreat"), [&] { issue_retreat(eviction); });
                        }
                        return false;
                    }
                }
            }

            // Pin the target: clear the station's stale "busy" flags, turn the player's
            // view at the station (the live target query is look-based), then aim both
            // the sensor and the ability instance at the station explicitly. Sensing
            // stays LIVE for the whole window (v0.8.6): the sensor's sensed set only
            // picks up the just-freed station on its own sensing ticks, and freezing
            // locked the set in its pre-eviction state - empty for small stations
            // (v0.8.5 log: component healthy, nearest=<null> all window). The old
            // target-steal reason for the freeze is covered now: the NPC is
            // force-disabled and the mismatch-guard ends wrong grabs (proven on the
            // water barrel).
            make_station_usable(eviction, station);
            face_station(station);
            if (auto* sensor = m_objects.find_player_sensor())
            {
                write_object_property(sensor, STR("m_CurrentInteractionActor"), station);
                // v0.7.10 proved the activation ignores the written/frozen sensor field
                // (sensorTarget held OUR station, the ability still targeted the NPC) -
                // it does its own live nearest-interactive query. m_FilterActor is the
                // sensor's only filter knob; pin it to the station for the use window
                // (cleared in unfreeze_player_sensor). Semantics are undocumented - if
                // it is an EXCLUDE filter the verify+EndAndCall net below still holds.
                write_object_property(sensor, STR("m_FilterActor"), station);
                m_sensor.mark_pinned();
            }
            write_object_property(ability_instance, STR("m_InteractionActor"), station);
            write_object_property(ability_instance, STR("m_InteractiveActor"), station);

            auto call = begin_native_call(
                asc,
                STR("/Script/GameplayAbilities.AbilitySystemComponent:TryActivateAbilityByClass"),
                STR("TryActivateAbilityByClass"),
                STR("player use"));
            set_param(call, STR("InAbilityToActivate"), &ability_class, 8, STR("player use"));
            set_bool_param(call, STR("bAllowRemoteActivation"), false, STR("player use"));
            if (!invoke_call(call, STR("player use")))
            {
                return false;
            }

            const auto activated = get_bool_param(call, STR("ReturnValue"), STR("player use"));
            if (!activated)
            {
                return false;
            }

            // Verify what the interaction actually targeted: if the activation re-picked
            // its own target (the NPC -> dialogue, or a closer station), end it right
            // away and retry on the cadence instead of reporting a false success.
            auto* actual_target = read_object(ability_instance, STR("m_InteractionActor"));
            if (actual_target != station)
            {
                // Diagnostic: if the sensor still holds OUR station here, the activation
                // reads its target elsewhere (live nearest-interactive compute) - that is
                // the plan-C trigger; if it holds something else, the freeze is leaky.
                auto* sensor = m_objects.find_player_sensor();
                auto* sensor_target = sensor ? read_object(sensor, STR("m_CurrentInteractionActor")) : nullptr;
                auto* filter_actor = sensor ? read_object(sensor, STR("m_FilterActor")) : nullptr;
                Output::send<LogLevel::Warning>(
                    STR("[LetMeCraft] player use target mismatch attempt={} target={} station={} sensorTarget={} filterActor={} - ending wrong interaction.\n"),
                    eviction.player_use_attempts,
                    object_name(actual_target),
                    object_name(station),
                    object_name(sensor_target),
                    object_name(filter_actor));
                call_no_params_safely(ability_instance, STR("EndAndCall"), STR("player use target mismatch"));

                // The evicted NPC standing next to the player keeps stealing the target:
                // send it away again (it drifts back between kill-guard cancels).
                const auto now = Clock::now();
                if (actual_target == weak_get(eviction.avatar) && now >= eviction.re_retreat_at)
                {
                    eviction.re_retreat_at = now + kReRetreatInterval;
                    run_guarded(STR("re-retreat"), [&] { issue_retreat(eviction); });
                }
                return false;
            }

            return true;
        }

        auto finish_eviction(ActiveEviction& eviction, const TCHAR* reason) -> void
        {
            // First, before the unclaim: a throw later in this function must not
            // leave the routine-blocker tag on the NPC.
            run_guarded(STR("routine blocker remove"), [&] {
                m_blocker.remove_routine_blocker(eviction, reason);
            });
            // Resume the NPC's daily routine. Issue the SwitchToDailyRoutine ProcessEvent ONLY
            // if the avatar is still live: a dead avatar means the world is tearing down (finish
            // raced a map-load), so we then just clear the flag instead of re-entering the AI on
            // a dying world (the v0.8.11 crash class). resume_routine also is_usable-gates the
            // ability itself, so this is belt-and-suspenders.
            const auto avatar_live = weak_get(eviction.avatar) != nullptr;
            run_guarded(STR("resume routine"), [&] { m_claims.resume_routine(eviction, avatar_live); });

            auto unclaimed = false;
            if (eviction.has_claim)
            {
                unclaimed = m_claims.call_token_library_unclaim(eviction.claim_token, STR("unclaim"));
            }
            if (eviction.npc_interaction_disabled)
            {
                run_guarded(STR("npc interaction re-enable"), [&] {
                    set_npc_interaction_enabled(eviction, true);
                });
            }
            m_movement.unlock_player_movement(reason);
            m_sensor.unfreeze_player_sensor(reason);

            Output::send<LogLevel::Verbose>(
                STR("[LetMeCraft] eviction finished reason={} owner={} avatar={} spot={} unclaim={}.\n"),
                reason,
                eviction.owner_name,
                eviction.avatar_name,
                fname_to_text(eviction.spot),
                eviction.has_claim ? (unclaimed ? STR("ok") : STR("failed")) : STR("<none>"));
        }

        // Returns false when the eviction entry should be removed by the caller.
        // Every phase runs under its own guard: one throwing phase neither kills the
        // others nor escapes to the engine-tick handler anonymously.
        auto tick_eviction(ActiveEviction& eviction, Clock::time_point now) -> bool
        {
            auto* avatar = weak_get(eviction.avatar);
            if (!avatar)
            {
                Output::send<LogLevel::Verbose>(
                    STR("[LetMeCraft] eviction dropped: avatar gone owner={} avatar={}.\n"),
                    eviction.owner_name,
                    eviction.avatar_name);
                // Separate guard: an unclaim throw inside the drop cleanup must not
                // skip the tag removal (the ASC may outlive the avatar).
                run_guarded(STR("routine blocker remove"), [&] {
                    m_blocker.remove_routine_blocker(eviction, STR("avatar gone"));
                });
                // Avatar is gone -> world is tearing this NPC down; clear the interrupt flag
                // WITHOUT a SwitchToDailyRoutine ProcessEvent (allow_call=false).
                run_guarded(STR("resume routine"), [&] { m_claims.resume_routine(eviction, false); });
                run_guarded(STR("drop cleanup"), [&] {
                    if (eviction.has_claim)
                    {
                        m_claims.call_token_library_unclaim(eviction.claim_token, STR("unclaim (avatar gone)"));
                    }
                    if (eviction.npc_interaction_disabled)
                    {
                        set_npc_interaction_enabled(eviction, true);
                    }
                    m_movement.unlock_player_movement(STR("avatar gone"));
                    m_sensor.unfreeze_player_sensor(STR("avatar gone"));
                });
                return false;
            }

            if (!eviction.move_issued && now >= eviction.move_at)
            {
                eviction.move_issued = true;
                run_guarded(STR("retreat"), [&] { issue_retreat(eviction); });
                // Quiet tick (+250ms after the cancel): safe to resolve the component.
                run_guarded(STR("npc interaction disable"), [&] {
                    set_npc_interaction_enabled(eviction, false);
                });
            }

            if (eviction.spot_known && !eviction.has_claim && !eviction.claim_gave_up &&
                !eviction.player_use_done && now >= eviction.next_claim_at)
            {
                // The NPC releases its own claim shortly after OnRequestEndQuick; a few
                // retries, force-release once. No cooldown fallback: a spot cooldown
                // locks the station for the player too (v0.7.3 lesson). The attempts
                // counter also acts as the failsafe if the claim phase keeps throwing.
                eviction.next_claim_at = now + kClaimRetryInterval;
                ++eviction.claim_attempts;
                if (eviction.claim_attempts > kClaimMaxAttempts + 2)
                {
                    eviction.claim_gave_up = true;
                }
                else
                {
                    run_guarded(STR("claim"), [&] {
                        if (m_claims.try_claim_spot(eviction, eviction.claim_attempts == 2))
                        {
                            eviction.has_claim = true;
                        }
                        else if (eviction.claim_attempts >= kClaimMaxAttempts)
                        {
                            eviction.claim_gave_up = true;
                        }
                    });
                }
            }

            // The auto-take window is the move-lock span (10s), shorter than the 17s
            // parking hold. Once it closes, stop poking the interact ability and give
            // the player back full control - late activations while the player walks
            // around would grab whatever interactive they pass (v0.8.1 risk).
            // v0.8.12: the attempt cap closes it early - a window that ran past
            // every historical success point is a stuck spot, not a slow one.
            if (!eviction.player_use_done &&
                (now >= eviction.player_use_until ||
                 eviction.player_use_attempts >= kPlayerUseMaxAttempts))
            {
                const auto capped = eviction.player_use_attempts >= kPlayerUseMaxAttempts;
                eviction.player_use_done = true;
                run_guarded(STR("interaction window close"), [&] {
                    m_movement.unlock_player_movement(STR("interaction window over"));
                    m_sensor.unfreeze_player_sensor(STR("interaction window over"));
                });
                // The take FAILED - the spot stays with the NPC. End the parking
                // (and with it the 1Hz kill-guard fight against the routine) soon
                // instead of churning for the rest of the 17s hold.
                if (now + kFailedTakeHoldTail < eviction.resume_at)
                {
                    eviction.resume_at = now + kFailedTakeHoldTail;
                }
                Output::send<LogLevel::Verbose>(
                    STR("[LetMeCraft] auto-take window closed (reason={}) after {} attempts owner={} spot={} - take failed, parking shortened to {}s.\n"),
                    capped ? STR("attempt cap, spot stuck") : STR("timeout"),
                    eviction.player_use_attempts,
                    eviction.owner_name,
                    eviction.spot_known ? fname_to_text(eviction.spot) : StringType{STR("<unknown>")},
                    std::chrono::duration_cast<std::chrono::seconds>(kFailedTakeHoldTail).count());
            }

            // Feed the approach every engine tick (AddMovementInput is per-frame
            // input) from eviction start until the take succeeds or the window
            // closes - the ~1s pre-take pause is already walking time.
            if (!eviction.player_use_done)
            {
                run_guarded(STR("approach station"), [&] { drive_player_toward_station(eviction); });
            }

            // Arrival gate: no activation while the player is still walking - the
            // v0.8.8 mid-walk activations grabbed bystander interactives (water
            // barrel) and paid seconds of mismatch recovery. The grace is the
            // blocked-path fallback.
            if (!eviction.player_use_done && now >= eviction.player_use_at &&
                (eviction.approach_done || now >= eviction.approach_grace_until))
            {
                ++eviction.player_use_attempts;

                auto activated = false;
                run_guarded(STR("player use"), [&] { activated = try_player_use_station(eviction); });
                if (activated)
                {
                    // The player occupies the station now - release the player right
                    // away, but KEEP the eviction alive: the NPC stays parked behind
                    // the player for the full hold (user request v0.8.2) instead of
                    // wandering back to the station area while the player works.
                    eviction.player_use_done = true;
                    eviction.player_took_station = true;
                    run_guarded(STR("bridge cleanup"), [&] {
                        if (eviction.npc_interaction_disabled)
                        {
                            set_npc_interaction_enabled(eviction, true);
                        }
                        m_movement.unlock_player_movement(STR("player took station"));
                        m_sensor.unfreeze_player_sensor(STR("player took station"));
                    });
                    Output::send<LogLevel::Verbose>(
                        STR("[LetMeCraft] player took station owner={} spot={} - NPC keeps parking until the hold expires.\n"),
                        eviction.owner_name,
                        eviction.spot_known ? fname_to_text(eviction.spot) : StringType{STR("<unknown>")});
                }
                else
                {
                    eviction.player_use_at = now + kPlayerUseBurstInterval;
                }
            }

            // v0.8.13: per-engine-tick claim poll. The routine re-claims the token
            // within 0-1 world ticks of its task releasing it; the 100ms attempt
            // cadence saw ~1 in 6 of those gaps and lost the race on sticky spots
            // (whetstone/cauldron wars in the v0.8.12 log). This prehook runs
            // BEFORE each world tick, so a poll every tick catches every >=1-tick
            // gap. Placed AFTER the take attempt on purpose: a claim acquired here
            // is one world tick old when the next attempt's handoff releases it
            // (the v0.8.9 survive-one-tick rule) - same-tick acquisition inside an
            // attempt stays with the inline claim and its claimed_this_tick guard.
            // The IsUnclaimed pre-gate keeps a held token to one quiet read per
            // tick instead of a game-side "No tokens left" warning at 60Hz.
            // claim_gave_up is deliberately ignored: it only ends the 500ms
            // diagnostics phase, not the race.
            if (!eviction.player_use_done && eviction.spot_known && !eviction.has_claim)
            {
                run_guarded(STR("claim poll"), [&] {
                    if (m_claims.is_spot_unclaimed(eviction.spot) && m_claims.try_claim_spot(eviction, false))
                    {
                        eviction.has_claim = true;
                    }
                });
            }

            // Park the NPC behind the player for the whole hold: the routine drags it
            // back toward the station between cancels, so when it drifts off the
            // behind-point the walk order is gently re-issued (no teleports).
            if (eviction.move_issued && eviction.retreat_dest_known &&
                now >= eviction.stand_still_from && now >= eviction.re_retreat_at)
            {
                const auto avatar_location = read_actor_location(avatar);
                if (avatar_location.found &&
                    distance_squared(avatar_location.value, eviction.retreat_dest) >
                        kBehindHoldToleranceSquared)
                {
                    eviction.re_retreat_at = now + kReRetreatInterval;
                    run_guarded(STR("behind-hold re-retreat"), [&] { issue_retreat(eviction); });
                }
            }

            // v0.8.12: the guard stops once the player RUNS the station - the routine
            // cannot reclaim an occupied spot, so post-success cancels were pure GAS
            // teardown churn on the NPC ability (the v0.8.11 crash followed exactly
            // that churn against an NPC whose actor the game had recreated mid-fight).
            if (!eviction.player_took_station &&
                now >= eviction.stand_still_from && now >= eviction.next_enforce_at)
            {
                eviction.next_enforce_at = now + kHoldEnforceInterval;
                run_guarded(STR("kill guard"), [&] {
                    // Bridge guard only: if the routine re-grabs the station while the
                    // player is taking over, end it again. No pausing, no repositioning -
                    // the NPC's behavior stays natural. Evaluates the cached ability
                    // (scan fallback only on a dead weak ptr).
                    auto search = m_scanner.evaluate_eviction_ability(eviction);
                    if (!search.found && !weak_get(eviction.ability))
                    {
                        search = m_scanner.select_crafting_candidate(
                            STR("kill guard"),
                            &eviction.owner_name,
                            &eviction.avatar_name,
                            false);
                        if (search.found) { eviction.ability = search.candidate.ability; }
                    }
                    if (search.found)
                    {
                        m_scanner.call_request_end_quick(search.candidate);
                        // Also drop the claims the reacquired ability took: cancelling the
                        // ability alone leaves the token held, which is what starved the
                        // player-use poll on Kharim/Snaf in v0.7.4.
                        m_claims.force_release_owner_claims(
                            m_objects.find_interaction_subsystem(), eviction, STR("kill guard"));
                    }
                });
            }

            if (now >= eviction.resume_at)
            {
                run_guarded(STR("finish"), [&] { finish_eviction(eviction, STR("bridge window expired")); });
                return false;
            }

            return true;
        }

        auto tick_evictions() -> void
        {
            if (m_evictions.empty()) { return; }

            const auto now = Clock::now();
            for (auto index = m_evictions.size(); index > 0; --index)
            {
                if (!tick_eviction(m_evictions[index - 1], now))
                {
                    m_evictions.erase(m_evictions.begin() + static_cast<std::ptrdiff_t>(index - 1));
                }
            }
        }

    private:
        GameObjects& m_objects;
        MovementLock& m_movement;
        PlayerSensor& m_sensor;
        SpotClaims& m_claims;
        CraftingScanner& m_scanner;
        RoutineBlocker& m_blocker;
        std::vector<ActiveEviction> m_evictions{};
        Clock::time_point m_next_manual_request_time{};
    };
}
