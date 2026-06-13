#pragma once

#include "Common.hpp"
#include "GameObjects.hpp"

namespace lmc
{
    // The player's interaction sensor. Sensing stays live since v0.8.6; what still
    // needs cleanup when the take window closes is the m_FilterActor pin written
    // per attempt (it used to live behind the frozen flag). The m_sensor_frozen
    // branch survives as a belt for the triple-failsafed cleanup paths.
    class PlayerSensor
    {
    public:
        explicit PlayerSensor(GameObjects& objects) : m_objects(objects) {}

        auto is_frozen() const -> bool { return m_sensor_frozen; }

        // The take attempt pins the sensor's m_FilterActor at the target station;
        // record that so the next unfreeze clears it. (The write itself stays in the
        // eviction code next to the other sensor writes.)
        auto mark_pinned() -> void { m_sensor_pinned = true; }

        auto unfreeze_player_sensor(const TCHAR* reason) -> void
        {
            if (m_sensor_pinned)
            {
                run_guarded(STR("sensor unpin"), [&] {
                    if (auto* sensor = m_objects.find_player_sensor())
                    {
                        write_object_property(sensor, STR("m_FilterActor"), nullptr);
                    }
                });
                m_sensor_pinned = false;
            }

            if (!m_sensor_frozen) { return; }

            // Call first, clear the flag only on success: the engine-tick failsafe keeps
            // retrying while the flag is set, so sensing can never stay disabled.
            auto unfrozen = false;
            run_guarded(STR("sensor unfreeze"), [&] {
                auto* sensor = m_objects.find_player_sensor();
                write_object_property(sensor, STR("m_FilterActor"), nullptr);
                auto call = begin_call(sensor, STR("SetSensingUpdatesEnabled"), STR("sensor unfreeze"));
                set_bool_param(call, STR("bEnabled"), true, STR("sensor unfreeze"));
                unfrozen = invoke_call(call, STR("sensor unfreeze"));
            });
            if (!unfrozen)
            {
                Output::send<LogLevel::Warning>(
                    STR("[LetMeCraft] sensor unfreeze failed reason={}, will retry.\n"),
                    reason);
                return;
            }

            m_sensor_frozen = false;
        }

        auto clear() -> void
        {
            m_sensor_frozen = false;
            m_sensor_pinned = false;
        }

    private:
        GameObjects& m_objects;
        bool m_sensor_frozen{};
        bool m_sensor_pinned{};
    };
}
