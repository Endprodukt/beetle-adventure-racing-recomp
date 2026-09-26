#pragma once

#include "recompinput.h"

namespace recompinput {
    namespace binding {
        void start_scanning(int player_index, recompinput::GameInput game_input, int binding_index, recompinput::InputDevice device, int profile_index = -1);
        void stop_scanning();
        bool is_binding();
        bool is_controller_being_bound(SDL_JoystickID joystick_id);
        bool is_wheel_being_bound();
        int wheel_axis_rest(SDL_JoystickID joystick_id, int axis);
        void set_scanned_input(recompinput::InputField value);
        recompinput::InputDevice get_scanning_device();
        bool should_skip_events();
        void stop_skipping_events();
        recompinput::GameInput get_scanning_game_input();
    }
}
