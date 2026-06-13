#pragma once

#include "Common.hpp"
#include "GameObjects.hpp"

namespace lmc
{
    // The player presses E to take the station: their movement is locked for a few
    // seconds while the NPC steps aside, then the interaction starts automatically.
    // ResetIgnoreMoveInput clears the whole ignore-counter, so the unlock can never
    // leave the player stuck. Jumping is blocked separately (JumpMaxCount=0) because
    // SetIgnoreMoveInput only suppresses the movement axis.
    class MovementLock
    {
    public:
        explicit MovementLock(GameObjects& objects) : m_objects(objects) {}

        auto is_locked() const -> bool { return m_move_input_locked; }
        auto unlock_at() const -> Clock::time_point { return m_move_unlock_at; }

        // Both move-lock UFunctions are resolved ONCE on a quiet engine tick: any object
        // lookup (StaticFindObject's GUObjectArray scan included) throws "Array failed
        // invariants check" when it runs in the SAME tick as OnRequestEndQuick, tripping
        // over the half-destroyed GAS objects (proven by v0.7.4-0.7.6 logs: the identical
        // lookups succeed on every later tick). /Script functions are permanent, so the
        // warm-up happens once per session and lock/unlock then run cache-only.
        auto warm_up_controller_functions() -> void
        {
            if (m_controller_functions_warmed) { return; }

            const auto now = Clock::now();
            if (now < m_next_warmup_at) { return; }
            m_next_warmup_at = now + kWarmupRetryInterval;

            // The path lookup needs no controller; only the children-walk fallback does.
            auto* player_controller = m_objects.find_player_controller_cached();

            // Without a controller the children-walk has nothing to walk; do not burn
            // a logged attempt on the menu world.
            if (!player_controller) { return; }

            ++m_warmup_attempts;
            run_guarded(STR("controller function warm-up"), [&] {
                const auto* set_function = find_cached_function(
                    m_set_ignore_move_input_function,
                    player_controller,
                    STR("/Script/Engine.PlayerController:SetIgnoreMoveInput"),
                    STR("SetIgnoreMoveInput"),
                    STR("move lock warm-up"));
                const auto* reset_function = find_cached_function(
                    m_reset_ignore_move_input_function,
                    player_controller,
                    STR("/Script/Engine.PlayerController:ResetIgnoreMoveInput"),
                    STR("ResetIgnoreMoveInput"),
                    STR("move unlock warm-up"));
                m_controller_functions_warmed = set_function && reset_function;
            });

            if (!m_controller_functions_warmed && (m_warmup_attempts == 1 || m_warmup_attempts % 20 == 0))
            {
                Output::send<LogLevel::Warning>(
                    STR("[LetMeCraft] controller function warm-up attempt {} unresolved - retrying every {}ms.\n"),
                    m_warmup_attempts,
                    std::chrono::duration_cast<std::chrono::milliseconds>(kWarmupRetryInterval).count());
            }
        }

        auto lock_player_movement() -> void
        {
            auto* player_controller = m_objects.find_player_controller_cached();
            if (!player_controller) { return; }

            // Cache-only on purpose: no object lookups may run in the cancel tick.
            auto* function = static_cast<UFunction*>(weak_get(m_set_ignore_move_input_function));
            if (!function)
            {
                Output::send<LogLevel::Warning>(
                    STR("[LetMeCraft] move lock skipped: SetIgnoreMoveInput not warmed up yet.\n"));
                return;
            }

            auto call = begin_call_with_function(player_controller, function, STR("move lock"));
            set_bool_param(call, STR("bNewMoveInput"), true, STR("move lock"));
            if (!invoke_call(call, STR("move lock"))) { return; }

            m_move_input_locked = true;
            m_move_unlock_attempts = 0;
            m_move_unlock_at = Clock::now() + kMoveLockDuration;
            run_guarded(STR("jump block"), [&] { set_player_jump_enabled(false); });
        }

        auto unlock_player_movement(const TCHAR* reason) -> void
        {
            if (!m_move_input_locked) { return; }

            // Call first, clear the flag only on success: a silently failed
            // ResetIgnoreMoveInput must never strand the player move-locked.
            auto unlocked = false;
            run_guarded(STR("move unlock"), [&] {
                auto* player_controller = m_objects.find_player_controller_cached();
                // Cache-only, same as the lock: filled by the warm-up tick.
                auto* function = static_cast<UFunction*>(weak_get(m_reset_ignore_move_input_function));
                if (!player_controller || !function) { return; }

                auto call = begin_call_with_function(player_controller, function, STR("move unlock"));
                unlocked = invoke_call(call, STR("move unlock"));
            });

            if (!unlocked)
            {
                ++m_move_unlock_attempts;
                if (m_move_unlock_attempts < kMoveUnlockMaxAttempts)
                {
                    // Re-arm the engine-tick failsafe as the retry driver.
                    m_move_unlock_at = Clock::now() + kMoveUnlockRetryInterval;
                    Output::send<LogLevel::Warning>(
                        STR("[LetMeCraft] move unlock failed reason={} attempt={}, retrying.\n"),
                        reason,
                        m_move_unlock_attempts);
                    return;
                }
                Output::send<LogLevel::Error>(
                    STR("[LetMeCraft] move unlock abandoned after {} attempts reason={}.\n"),
                    m_move_unlock_attempts,
                    reason);
            }

            m_move_input_locked = false;
            m_move_unlock_attempts = 0;
            run_guarded(STR("jump unblock"), [&] { set_player_jump_enabled(true); });
        }

        // Reset only the per-world flags + warm-up budget. The resolved function
        // caches and the "warmed" latch deliberately survive across worlds
        // (permanent /Script objects).
        auto clear() -> void
        {
            m_move_input_locked = false;
            m_move_unlock_attempts = 0;
            m_warmup_attempts = 0;
            m_next_warmup_at = {};
        }

    private:
        // Resolve a UFunction once and keep it in a weak cache (/Script function objects
        // live for the whole session). Tries the children-walk first (FuncMap-independent,
        // the only route that ever resolved the controller functions in-game), then the
        // full-path lookup; each route under its own guard so one throwing route cannot
        // eat the other.
        auto find_cached_function(
            FWeakObjectPtr& cache,
            UObject* chain_object,
            const TCHAR* function_path,
            const TCHAR* function_name,
            const TCHAR* context) -> UFunction*
        {
            if (auto* cached = weak_get(cache)) { return static_cast<UFunction*>(cached); }

            // Children-walk FIRST: it only touches the target's class chain and is the
            // only route that has ever resolved the controller functions in-game
            // (v0.7.7). The global StaticFindObject scan can throw for MINUTES straight
            // (v0.7.9 log) - each route runs under its own guard so one throwing route
            // cannot eat the other.
            UFunction* function = nullptr;
            try
            {
                function = find_function_via_children(chain_object, function_name);
            }
            catch (...)
            {
            }
            if (!function)
            {
                try
                {
                    function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, function_path);
                }
                catch (...)
                {
                }
            }
            // No warning here: the warm-up retries on a schedule and does its own
            // throttled failure logging.
            if (!function) { return nullptr; }

            cache = static_cast<UObject*>(function);
            return function;
        }

        // SetIgnoreMoveInput only suppresses the movement AXIS - jumping is an action
        // input and stays live (v0.7.13 user report: the player can jump while
        // move-locked). ACharacter::JumpMaxCount=0 makes CanJump fail; the previous
        // value is restored on unlock. Property chain walks are safe in any tick.
        auto set_player_jump_enabled(bool enabled) -> void
        {
            auto* player = m_objects.find_player_actor_cached();
            if (!player) { return; }
            auto* property = CastField<FIntProperty>(player->GetPropertyByNameInChain(STR("JumpMaxCount")));
            if (!property) { return; }
            if (!enabled)
            {
                const auto current = property->GetPropertyValueInContainer(player);
                if (current > 0) { m_saved_jump_max_count = current; }
                property->SetPropertyValueInContainer(player, 0);
            }
            else
            {
                property->SetPropertyValueInContainer(
                    player, m_saved_jump_max_count > 0 ? m_saved_jump_max_count : 1);
            }
        }

        GameObjects& m_objects;
        Clock::time_point m_move_unlock_at{};
        bool m_move_input_locked{};
        int m_move_unlock_attempts{};
        int32_t m_saved_jump_max_count{1};
        bool m_controller_functions_warmed{};
        int m_warmup_attempts{};
        Clock::time_point m_next_warmup_at{};
        FWeakObjectPtr m_set_ignore_move_input_function{};
        FWeakObjectPtr m_reset_ignore_move_input_function{};
    };
}
