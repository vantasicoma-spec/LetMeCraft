#include "lmc/EvictionManager.hpp"
#include "lmc/GlyphOverlay.hpp"

using namespace lmc;

class LetMeCraft : public CppUserModBase
{
public:
    LetMeCraft() : CppUserModBase()
    {
        ModName = STR("LetMeCraft");
        ModVersion = STR("1.2.0-glyphicon15");
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
            STR("[LetMeCraft] v1.2.0-glyphicon15 loaded. Eviction = v1.1.0. The icon14 log proved the native prompt's brush TintColors are all white (1,1,1,1) - its color is baked into the textures, not readable from the brush. Per the user, the icon should be the SAME earthy color as the prompt TEXT, so the mod now reads our OWN TextBlock's ColorAndOpacity at runtime and tints the ring + symbol with it (black fill untouched). That's a read of our own held widget child (run_guarded, no foreign scan). Until it resolves, an earthy-tan fallback (0.52,0.41,0.24) stands; then a one-shot upgrade swaps in the exact text color. Log: 'glyph text color: (r,g,b,a) rule=..' (the value read) and 'glyph text-color upgrade applied to ring+symbol: (r,g,b)'. Look at a cook/forge - ring + symbol should now be the earthy text color. If rule!=0 (color is in the font, not ColorAndOpacity), the earthy fallback shows - send the log and I'll pin the exact value.\n"));

        register_keydown_event(Input::Key::E, [this]() {
            m_last_input_gamepad = false;
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
        detect_current_input_device();
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
        m_glyph.clear();
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

            // Floating glyph prompt, FULLY ISOLATED under its own SEH guard. NOTHING in the prompt
            // path (the 100ms scan that walks recycled GAS abilities, the station-name FText read, the
            // UMG calls, the head projection) may ever reach the top-level tick guard and abort an
            // eviction - that abort dropped a live eviction's spot claim WITHOUT Unclaim and left the
            // NPC's routine swapped to DailyRoutine_Empty, i.e. the "station stays locked / E does
            // nothing" bug. A glyph fault now just skips one prompt frame; evictions are untouched.
            {
                SehCrashInfo glyph_crash{};
                if (!invoke_with_seh([this] { update_glyph(); }, glyph_crash) && m_glyph_frame_faults < 5)
                {
                    ++m_glyph_frame_faults;
                    Output::send<LogLevel::Warning>(
                        STR("[LetMeCraft] glyph frame skipped (SEH code=0x{:X} ip=0x{:X}); evictions untouched.\n"),
                        glyph_crash.code,
                        reinterpret_cast<uintptr_t>(glyph_crash.instruction_address));
                }
            }

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


    // Whole glyph-prompt frame: scan the evict candidate on a 100ms throttle, read its station
    // name/icon once when the candidate changes (and not mid-eviction), then drive the overlay. Runs
    // entirely inside the caller's invoke_with_seh, so any fault here just skips a prompt frame.
    auto update_glyph() -> void
    {
        const auto now = Clock::now();
        if (now - m_last_prompt_scan >= std::chrono::milliseconds(100))
        {
            m_last_prompt_scan = now;
            auto probe = m_scanner.select_crafting_candidate(STR("glyph"), nullptr, nullptr, true);
            if (probe.found)
            {
                auto* candidate_avatar = probe.candidate.avatar;
                const bool avatar_changed = candidate_avatar && candidate_avatar != weak_get(m_glyph_avatar);
                // Cache the avatar FIRST: if the name read below faults, the outer SEH skips the rest
                // of this frame, but the avatar is already recorded so we never re-read (and re-fault)
                // the same candidate at 10Hz.
                m_glyph_avatar = candidate_avatar;
                if (avatar_changed && !m_evictions_mgr.is_evicting_avatar(candidate_avatar))
                {
                    auto* item = read_object(read_object(read_object(probe.candidate.ability,
                                                                     STR("m_InteractiveActor")),
                                                         STR("m_InteractiveComponent")),
                                             STR("m_InteractItem"));
                    m_glyph_name = read_ftext(item, STR("m_Name"));
                    m_glyph_icon = pick_station_icon(probe.candidate.root_task);
                }
            }
            else
            {
                m_glyph_avatar = static_cast<UObject*>(nullptr);
            }
        }

        auto* glyph_avatar = weak_get(m_glyph_avatar);
        const bool glyph_active = glyph_avatar && !m_evictions_mgr.is_evicting_avatar(glyph_avatar);
        m_glyph.update(glyph_active, glyph_avatar, m_last_input_gamepad, m_glyph_name, m_glyph_icon);
    }

    // Generic interaction icon: the fallback for every station without a dedicated texture, and the
    // safe default while the station name/icon have not been read yet.
    static constexpr const TCHAR* kDefaultStationIcon = STR("/Game/UI/Textures/Common/Icons/T_Interaction_Use.T_Interaction_Use");

    // Maps the crafting station's root-task name to one of the game's interaction action icons. Paths
    // confirmed loaded by the glyph-tex sweep. NOTE the forge asset is "T_Interact_Forge" (prefix
    // "T_Interact_", NOT "T_Interaction_"). Anything else (whetstone/saw/...) -> generic "use" icon.
    // These are white silhouette masks; GlyphOverlay tints the station image orange to match the native.
    static auto pick_station_icon(UObject* root_task) -> const TCHAR*
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
            m_last_input_gamepad = true;
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

    auto ensure_user32() -> bool
    {
        if (m_user32_checked) { return m_get_async_key_state != nullptr; }
        m_user32_checked = true;

        m_user32_module = Windows::LoadLibraryW(L"user32.dll");
        if (m_user32_module)
        {
            m_get_async_key_state = reinterpret_cast<GetAsyncKeyStateFn>(Windows::GetProcAddress(m_user32_module, "GetAsyncKeyState"));
            m_get_cursor_pos = reinterpret_cast<GetCursorPosFn>(Windows::GetProcAddress(m_user32_module, "GetCursorPos"));
        }
        if (!m_get_async_key_state)
        {
            Output::send<LogLevel::Warning>(
                STR("[LetMeCraft] user32 input polling unavailable; the E/Y glyph will follow the last evict key pressed instead of the live device.\n"));
        }
        return m_get_async_key_state != nullptr;
    }

    // Pick the glyph (E vs round Y) by the device the player is using RIGHT NOW, not by the last evict
    // key pressed. "Last active device wins": gamepad button/stick/trigger -> Y; keyboard key / mouse
    // button / mouse movement -> E; no input -> keep the previous choice. Runs every UE4SS loop tick.
    auto detect_current_input_device() -> void
    {
        auto gamepad_active = false;
        if (ensure_xinput())
        {
            for (DWORD user_index = 0; user_index < 4; ++user_index)
            {
                XInputState state{};
                if (m_xinput_get_state(user_index, &state) != 0) { continue; }
                const auto& pad = state.Gamepad;
                const auto stick = [](SHORT v) {
                    return static_cast<int>(v) > kStickActivity || static_cast<int>(v) < -kStickActivity;
                };
                if (pad.wButtons != 0 ||
                    stick(pad.sThumbLX) || stick(pad.sThumbLY) ||
                    stick(pad.sThumbRX) || stick(pad.sThumbRY) ||
                    static_cast<int>(pad.bLeftTrigger) > kTriggerActivity ||
                    static_cast<int>(pad.bRightTrigger) > kTriggerActivity)
                {
                    gamepad_active = true;
                    break;
                }
            }
        }

        auto kbm_active = false;
        if (ensure_user32())
        {
            const auto down = [this](int vk) {
                return (static_cast<unsigned short>(m_get_async_key_state(vk)) & 0x8000u) != 0;
            };
            if (down(kVkLButton) || down(kVkRButton) || down(kVkMButton) ||
                down(kVkW) || down(kVkA) || down(kVkS) || down(kVkD) ||
                down(kVkSpace) || down(kVkShift) || down(kVkControl))
            {
                kbm_active = true;
            }
            if (!kbm_active && m_get_cursor_pos)
            {
                CursorPoint point{};
                if (m_get_cursor_pos(&point) != 0)
                {
                    if (m_have_last_mouse && (point.x != m_last_mouse_x || point.y != m_last_mouse_y))
                    {
                        kbm_active = true;
                    }
                    m_last_mouse_x = point.x;
                    m_last_mouse_y = point.y;
                    m_have_last_mouse = true;
                }
            }
        }

        // Gamepad wins ties (if somehow both moved this frame): the user just grabbed the pad.
        if (gamepad_active) { m_last_input_gamepad = true; }
        else if (kbm_active) { m_last_input_gamepad = false; }
    }

    HMODULE m_xinput_module{};
    XInputGetStateFn m_xinput_get_state{};
    HMODULE m_user32_module{};
    GetAsyncKeyStateFn m_get_async_key_state{};
    GetCursorPosFn m_get_cursor_pos{};
    long m_last_mouse_x{};
    long m_last_mouse_y{};
    bool m_have_last_mouse{};
    bool m_user32_checked{};
    GameObjects m_objects{};
    StringType m_pending_manual_source{};
    std::mutex m_pending_request_mutex{};
    Unreal::Hook::GlobalCallbackId m_engine_tick_callback_id{};
    Unreal::Hook::GlobalCallbackId m_load_map_callback_id{};
    Unreal::Hook::GlobalCallbackId m_init_game_state_callback_id{};
    bool m_last_input_gamepad{};
    Clock::time_point m_last_prompt_scan{};
    FWeakObjectPtr m_glyph_avatar{};
    StringType m_glyph_name{};
    const TCHAR* m_glyph_icon{kDefaultStationIcon};
    int m_glyph_frame_faults{};
    MovementLock m_movement{m_objects};
    PlayerSensor m_sensor{m_objects};
    SpotClaims m_claims{m_objects};
    RoutineBlocker m_blocker{};
    CraftingScanner m_scanner{m_objects};
    EvictionManager m_evictions_mgr{m_objects, m_movement, m_sensor, m_claims, m_scanner, m_blocker};
    GlyphOverlay m_glyph{m_objects};
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
