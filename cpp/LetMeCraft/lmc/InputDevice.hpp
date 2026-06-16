#pragma once

#include "Common.hpp"

namespace lmc
{
    // Owns all platform input polling for the mod: the gamepad-Y evict trigger (XInput) and the
    // live "which device is the player using right now" detection (XInput sticks/buttons/triggers vs
    // user32 keyboard/mouse). The glyph prompt shows E or a round Y based on is_gamepad(); the main
    // mod consumes consume_y_press() to queue an eviction.
    //
    // PERFORMANCE: this is polled from on_update() which runs at the UE4SS event-loop rate (hundreds
    // of Hz). XInputGetState on a DISCONNECTED slot does an internal device enumeration (~ms each), so
    // polling all 4 slots every loop iteration froze the game (v1.2.0 stutter). Fixes: (1) the whole
    // poll is throttled to ~30Hz; (2) a connected-slot cache means empty slots are touched only by a
    // 1Hz re-scan (<=4 empty polls/sec instead of thousands) and the steady poll hits only connected
    // slots (cheap). A keyboard-only player thus pays ~4 empty XInput polls/sec total.
    class InputDevice
    {
    public:
        // Run from on_update() every UE4SS loop tick; internally throttled to ~30Hz.
        auto update() -> void
        {
            const auto now = Clock::now();
            if (now < m_next_input_at) { return; }
            m_next_input_at = now + kInputPollInterval;
            poll(now);
        }

        // True while the last active device was the gamepad (selects the round Y glyph over E).
        auto is_gamepad() const -> bool { return m_last_input_gamepad; }

        // The keyboard E handler calls this so the glyph immediately switches back to the E glyph.
        auto note_keyboard() -> void { m_last_input_gamepad = false; }

        // Returns true exactly once per gamepad-Y press (rising edge); the caller queues the evict.
        auto consume_y_press() -> bool
        {
            const auto pressed = m_y_pressed_edge;
            m_y_pressed_edge = false;
            return pressed;
        }

    private:
        static inline const Clock::duration kInputPollInterval = std::chrono::milliseconds(30);  // ~30Hz
        static inline const Clock::duration kSlotRescanInterval = std::chrono::milliseconds(1000); // re-check empty slots 1Hz

        // One combined poll: read each CONNECTED gamepad slot ONCE for both the Y rising-edge and the
        // active-device detection, then the keyboard/mouse. Empty slots are touched only on the 1Hz
        // re-scan. "Last active device wins"; the gamepad wins ties.
        auto poll(Clock::time_point now) -> void
        {
            auto gamepad_active = false;
            auto y_is_down = false;
            if (ensure_xinput())
            {
                const bool rescan = now >= m_next_slot_rescan;
                if (rescan) { m_next_slot_rescan = now + kSlotRescanInterval; }

                for (DWORD user_index = 0; user_index < 4; ++user_index)
                {
                    // Steady state: skip slots known to be empty. Only the 1Hz re-scan probes them
                    // (the expensive XInputGetState-on-empty-slot enumeration happens at most 4x/sec).
                    if (!rescan && !m_slot_connected[user_index]) { continue; }

                    XInputState state{};
                    const bool connected = (m_xinput_get_state(user_index, &state) == 0);
                    if (rescan) { m_slot_connected[user_index] = connected; }
                    if (!connected) { continue; }

                    const auto& pad = state.Gamepad;
                    if (pad.wButtons & kXInputGamepadY) { y_is_down = true; }

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
                    }
                }
            }

            // Y rising edge -> queue the evict + switch to the gamepad glyph.
            if (y_is_down && !m_gamepad_y_was_down)
            {
                m_last_input_gamepad = true;
                m_y_pressed_edge = true;
            }
            m_gamepad_y_was_down = y_is_down;

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

            // Gamepad wins ties (if somehow both moved this tick): the user just grabbed the pad.
            if (gamepad_active) { m_last_input_gamepad = true; }
            else if (kbm_active) { m_last_input_gamepad = false; }
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
                    LMC_DLOG(STR("[LetMeCraft] XInput loaded for gamepad Y support.\n"));
                    return true;
                }
            }

            // Rare, one-shot, actionable: gamepad evict is genuinely off. Kept as a real Warning.
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
                // Rare, one-shot, actionable: the glyph then follows the last evict key instead of
                // the live device. Kept as a real Warning.
                Output::send<LogLevel::Warning>(
                    STR("[LetMeCraft] user32 input polling unavailable; the E/Y glyph will follow the last evict key pressed instead of the live device.\n"));
            }
            return m_get_async_key_state != nullptr;
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
        bool m_xinput_checked{};
        bool m_gamepad_y_was_down{};
        bool m_last_input_gamepad{};
        bool m_y_pressed_edge{};
        bool m_slot_connected[4]{};        // which XInput slots are physically present (refreshed 1Hz)
        Clock::time_point m_next_input_at{};
        Clock::time_point m_next_slot_rescan{};
    };
}
