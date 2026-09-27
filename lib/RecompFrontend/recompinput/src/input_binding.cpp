#include "recompinput.h"
#include "input_binding.h"
#include "input_state.h"
#include "players.h"
#include "profiles.h"
#include <unordered_map>

namespace recompinput {
    static std::unordered_map<uint64_t, int> wheel_axis_baselines;

    static uint64_t axis_key(SDL_JoystickID id, int axis) {
        return (uint64_t)(uint32_t)id << 32 | (uint32_t)axis;
    }

    static struct {
        bool active = false;
        // Designates when binding has been cancelled or completed and the event queue should be purged/ignored.
        bool skip_events = false;
        int player_index = -1;
        recompinput::GameInput game_input = recompinput::GameInput::COUNT;
        int binding_index = -1;
        int profile_index = -1;
        recompinput::InputField new_binding = {};
        recompinput::InputDevice device = recompinput::InputDevice::COUNT;

        void reset() {
            active = false;
            skip_events = false;
            player_index = -1;
            game_input = recompinput::GameInput::COUNT;
            binding_index = -1;
            profile_index = -1;
            new_binding = {};
            device = recompinput::InputDevice::COUNT;
        }
    } BindingState;

    void binding::start_scanning(int player_index, recompinput::GameInput game_input, int binding_index, recompinput::InputDevice device, int profile_index) {
        BindingState.active = true;
        BindingState.skip_events = false;
        BindingState.player_index = player_index;
        BindingState.game_input = game_input;
        BindingState.binding_index = binding_index;
        BindingState.profile_index = profile_index;
        BindingState.device = device;
        wheel_axis_baselines.clear();
        if (profile_index == profiles::get_wheel_profile_index()) {
            for (int i = 0; i < SDL_NumJoysticks(); ++i) {
                SDL_Joystick* joystick = SDL_JoystickFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
                if (!joystick) continue;
                SDL_JoystickID id = SDL_JoystickInstanceID(joystick);
                for (int axis = 0; axis < SDL_JoystickNumAxes(joystick); ++axis) {
                    wheel_axis_baselines[axis_key(id, axis)] = SDL_JoystickGetAxis(joystick, axis);
                }
            }
        }
    }

    void binding::stop_scanning() {
        BindingState.reset();
        BindingState.skip_events = true;
    }

    bool binding::is_binding() {
        return BindingState.active;
    }

    bool binding::is_controller_being_bound(SDL_JoystickID joystick_id) {
        if (BindingState.device != InputDevice::Controller || binding::is_wheel_being_bound()) {
            return false;
        }

        if (players::is_single_player_mode()) {
            return true;
        }

        const auto& player = players::get_player(BindingState.player_index);
        if (player.controller != nullptr) {
            SDL_JoystickID assigned_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(player.controller));
            if (assigned_id == joystick_id) {
                return true;
            }
        }

        return false;
    }

    bool binding::is_wheel_being_bound() {
        return BindingState.active && BindingState.profile_index == profiles::get_wheel_profile_index();
    }

    int binding::wheel_axis_rest(SDL_JoystickID joystick_id, int axis) {
        auto it = wheel_axis_baselines.find(axis_key(joystick_id, axis));
        return it == wheel_axis_baselines.end() ? 0 : it->second;
    }

    void binding::set_scanned_input(recompinput::InputField value) {
        profiles::set_input_binding(
            BindingState.profile_index >= 0 ? BindingState.profile_index :
                profiles::get_input_profile_for_player(BindingState.player_index, BindingState.device),
            BindingState.game_input,
            BindingState.binding_index,
            value
        );
        binding::stop_scanning();
    }

    recompinput::InputDevice binding::get_scanning_device() {
        return BindingState.device;
    }

    bool binding::should_skip_events() {
        return BindingState.skip_events;
    }

    void binding::stop_skipping_events() {
        BindingState.skip_events = false;
    }

    recompinput::GameInput binding::get_scanning_game_input() {
        return BindingState.game_input;
    }
}
