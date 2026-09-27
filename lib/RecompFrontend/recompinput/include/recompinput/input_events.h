#pragma once

#include "recompinput.h"

namespace recompinput {
    void handle_events();
    void purge_deferred_controller_profiles();
    int ensure_controller_profile(SDL_GameController* controller);
    // Called on the SDL event thread after pumping events and sampling BAR's motor.
    void update_wheel_force_feedback(uint16_t game_rumble, bool rumble_changed);
    // Stop and release any active wheel haptic effect during application shutdown.
    void shutdown_wheel_force_feedback();
    void wheel_debug_log(const std::string& line);
}
