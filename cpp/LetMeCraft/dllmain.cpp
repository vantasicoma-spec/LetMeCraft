#include "lmc/EvictionManager.hpp"

using namespace lmc;

class LetMeCraft : public CppUserModBase
{
public:
    LetMeCraft() : CppUserModBase()
    {
        ModName = STR("LetMeCraft");
        ModVersion = STR("0.9.0");
        ModDescription = STR("Moves the nearest NPC away from an occupied crafting station");
        ModAuthors = STR("Roma + Codex");
    }

    ~LetMeCraft() override
    {
        for (const auto id : {m_engine_tick_callback_id, m_load_map_callback_id, m_init_game_state_callback_id})
        {
            if (id != Unreal::Hook::ERROR_ID) { Unreal::Hook::UnregisterCallback(id); }
        }
    }

    auto on_unreal_init() -> void override
    {
        Output::send<LogLevel::Verbose>(
            STR("[LetMeCraft] v0.9.0 loaded. Press E or gamepad Y near an occupied crafting NPC. Release cleanup: dead code & diagnostics removed, hot paths cached; behavior identical to v0.8.18.\n"));

        register_keydown_event(Input::Key::E, [this]() {
            queue_manual_request(STR("keyboard E"));
        });

        m_engine_tick_callback_id = Unreal::Hook::RegisterEngineTickPreCallback(
            [this](auto&, UEngine*, float, bool) {
                on_engine_tick_game_thread();
            },
            {false, false, STR("LetMeCraft"), STR("GameThreadTick")});

        // Holds and pending displacements must not survive a map change or save-game
        // load: the actors they reference belong to the old world.
        m_load_map_callback_id = Unreal::Hook::RegisterLoadMapPreCallback(
            [this](auto&, auto&&...) {
                clear_transient_state(STR("LoadMap"));
            },
            {false, true, STR("LetMeCraft"), STR("ClearStateOnLoadMap")});

        m_init_game_state_callback_id = Unreal::Hook::RegisterInitGameStatePostCallback(
            [this](auto&, auto&&...) {
                clear_transient_state(STR("InitGameState"));
            },
            {false, true, STR("LetMeCraft"), STR("ClearStateOnInitGameState")});
    }

    auto on_update() -> void override
    {
        poll_gamepad_y();
    }

private:
    auto queue_manual_request(const TCHAR* source) -> void
    {
        std::lock_guard lock{m_pending_request_mutex};
        m_has_pending_manual_request = true;
        m_pending_manual_source = source;
    }

    auto take_manual_request(StringType& source) -> bool
    {
        std::lock_guard lock{m_pending_request_mutex};
        if (!m_has_pending_manual_request) { return false; }

        source = m_pending_manual_source;
        m_pending_manual_source.clear();
        m_has_pending_manual_request = false;
        return true;
    }

    auto clear_transient_state(const TCHAR* reason) -> void
    {
        // Claim tokens are dropped without Unclaim: the world that owned the claims is
        // being torn down together with the spots themselves.
        m_movement.unlock_player_movement(reason);
        m_sensor.unfreeze_player_sensor(reason);
        // The old world's controller and sensor die with the map and the new ones
        // spawn with default state: a failed unlock/unfreeze must not keep retrying
        // across worlds. The resolved function caches deliberately survive (permanent
        // /Script objects); clear() resets only the per-world flags + warm-up budget.
        m_movement.clear();
        m_sensor.clear();
        m_evictions_mgr.clear(reason);
        m_objects.clear();

        {
            std::lock_guard lock{m_pending_request_mutex};
            m_has_pending_manual_request = false;
            m_pending_manual_source.clear();
        }
    }

    auto on_engine_tick_game_thread() -> void
    {
        SehCrashInfo crash{};
        if (invoke_with_seh([this] { tick_game_thread_impl(); }, crash)) { return; }

        Output::send<LogLevel::Error>(
            STR("[LetMeCraft] engine tick caught SEH fault code=0x{:X} ip=0x{:X} access=0x{:X}; aborting all evictions.\n"),
            crash.code,
            reinterpret_cast<uintptr_t>(crash.instruction_address),
            static_cast<uintptr_t>(crash.fault_address));

        SehCrashInfo recovery_crash{};
        if (!invoke_with_seh([this] { recover_after_crash(); }, recovery_crash))
        {
            Output::send<LogLevel::Error>(
                STR("[LetMeCraft] crash recovery itself faulted code=0x{:X}; remaining state clears on next map load.\n"),
                recovery_crash.code);
        }
    }

    // Best-effort rollback after a hardware fault aborted the tick mid-flight:
    // a crashed eviction must not leave its NPC permanently non-interactive or
    // the player locked in place. Every step is guarded independently.
    auto recover_after_crash() -> void
    {
        m_evictions_mgr.reenable_disabled_npcs();
        run_guarded(STR("crash recovery clear"), [&] { clear_transient_state(STR("SEH fault")); });
    }

    auto tick_game_thread_impl() -> void
    {
        try
        {
            StringType source{};
            if (take_manual_request(source))
            {
                m_evictions_mgr.request_end_nearest_crafter_once(source.c_str());
            }

            m_evictions_mgr.tick_evictions();

            // Hard failsafe: the movement lock can never outlive its time budget.
            if (m_movement.is_locked() && Clock::now() >= m_movement.unlock_at())
            {
                m_movement.unlock_player_movement(STR("timeout"));
            }

            // Sensor failsafe: sensing must never stay frozen without a live bridge.
            if (m_sensor.is_frozen() && m_evictions_mgr.empty())
            {
                m_sensor.unfreeze_player_sensor(STR("failsafe"));
            }

            m_movement.warm_up_controller_functions();
        }
        catch (const std::exception& exception)
        {
            Output::send<LogLevel::Error>(
                STR("[LetMeCraft] engine tick recovered from exception: {}.\n"),
                ensure_str(exception.what()));
        }
        catch (...)
        {
            Output::send<LogLevel::Error>(
                STR("[LetMeCraft] engine tick recovered from unknown exception.\n"));
        }
    }


    auto poll_gamepad_y() -> void
    {
        if (!ensure_xinput()) { return; }

        auto y_is_down = false;
        for (DWORD user_index = 0; user_index < 4; ++user_index)
        {
            XInputState state{};
            if (m_xinput_get_state(user_index, &state) == 0 && (state.Gamepad.wButtons & kXInputGamepadY))
            {
                y_is_down = true;
                break;
            }
        }

        if (y_is_down && !m_gamepad_y_was_down)
        {
            queue_manual_request(STR("gamepad Y"));
        }

        m_gamepad_y_was_down = y_is_down;
    }

    auto ensure_xinput() -> bool
    {
        if (m_xinput_checked) { return m_xinput_get_state != nullptr; }
        m_xinput_checked = true;

        for (const auto* dll_name : {L"xinput1_4.dll", L"xinput9_1_0.dll", L"xinput1_3.dll"})
        {
            m_xinput_module = Windows::LoadLibraryW(dll_name);
            if (!m_xinput_module) { continue; }

            m_xinput_get_state = reinterpret_cast<XInputGetStateFn>(Windows::GetProcAddress(m_xinput_module, "XInputGetState"));
            if (m_xinput_get_state)
            {
                Output::send<LogLevel::Verbose>(STR("[LetMeCraft] XInput loaded for gamepad Y support.\n"));
                return true;
            }
        }

        Output::send<LogLevel::Warning>(
            STR("[LetMeCraft] XInput was not available; keyboard E still works, gamepad Y is disabled.\n"));
        return false;
    }

    HMODULE m_xinput_module{};
    XInputGetStateFn m_xinput_get_state{};
    GameObjects m_objects{};
    StringType m_pending_manual_source{};
    std::mutex m_pending_request_mutex{};
    Unreal::Hook::GlobalCallbackId m_engine_tick_callback_id{};
    Unreal::Hook::GlobalCallbackId m_load_map_callback_id{};
    Unreal::Hook::GlobalCallbackId m_init_game_state_callback_id{};
    MovementLock m_movement{m_objects};
    PlayerSensor m_sensor{m_objects};
    SpotClaims m_claims{m_objects};
    RoutineBlocker m_blocker{};
    CraftingScanner m_scanner{m_objects};
    EvictionManager m_evictions_mgr{m_objects, m_movement, m_sensor, m_claims, m_scanner, m_blocker};
    bool m_xinput_checked{};
    bool m_gamepad_y_was_down{};
    bool m_has_pending_manual_request{};
};

#define LET_ME_CRAFT_API __declspec(dllexport)

extern "C"
{
    LET_ME_CRAFT_API CppUserModBase* start_mod()
    {
        return new LetMeCraft();
    }

    LET_ME_CRAFT_API void uninstall_mod(CppUserModBase* mod)
    {
        delete mod;
    }
}
