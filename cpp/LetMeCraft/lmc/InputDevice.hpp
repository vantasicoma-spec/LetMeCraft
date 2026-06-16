#pragma once

#include "Common.hpp"

namespace lmc
{
    // Owns all platform input polling for the mod: the gamepad-Y evict trigger (XInput) and the
    // live "which device is the player using right now" detection (XInput sticks/buttons/triggers vs
    // user32 keyboard/mouse). The glyph prompt shows E or a round Y based on is_gamepad(); the main
    // mod consumes consume_y_press() to queue an eviction. Pulled out of dllmain so the main class
    // stays about orchestration. Polled from on_update() on the UE4SS loop thread.
    class InputDevice
    {
    public:
        // Run every UE4SS loop tick: refresh the active-device flag and edge-detect the gamepad Y.
        auto update() -> void
        {
            detect_current_input_device();
            poll_gamepad_y();
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
                m_y_pressed_edge = true;
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

        // Pick the glyph (E vs round Y) by the device the player is using RIGHT NOW, not by the last
        // evict key pressed. "Last active device wins": gamepad button/stick/trigger -> Y; keyboard
        // key / mouse button / mouse movement -> E; no input -> keep the previous choice.
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
        bool m_xinput_checked{};
        bool m_gamepad_y_was_down{};
        bool m_last_input_gamepad{};
        bool m_y_pressed_edge{};
    };
}
