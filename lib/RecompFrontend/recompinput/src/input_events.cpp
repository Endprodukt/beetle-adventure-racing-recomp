#include "recompinput/recompinput.h"
#include "recompinput/input_binding.h"
#include "recompinput/input_events.h"
#include "recompinput/profiles.h"
#include "recompui/config.h"
#include "ultramodern/ultramodern.hpp"
#include "librecomp/game.hpp"
#include <cmath>
#include <fstream>
#include <mutex>

static struct {
    std::list<std::filesystem::path> files_dropped;
} DropState;

namespace recompinput {

void wheel_debug_log(const std::string& line) {
    static std::mutex mutex;
    static bool first_line = true;
    std::lock_guard lock{mutex};
    const auto path = recomp::get_config_path() / "wheel-input.log";
    std::ofstream out(path, first_line ? std::ios::trunc : std::ios::app);
    first_line = false;
    if (out) out << line << '\n';
    std::fprintf(stderr, "%s\n", line.c_str());
}

void queue_if_enabled(SDL_Event* event) {
    if (!recompinput::all_input_disabled() && !binding::should_skip_events()) {
        recompui::queue_event(*event);
    }
}

// Controllers plugged in while in single player mode will create profiles after switching to multiplayer.
static std::unordered_map<uint64_t, ControllerGUID> deferred_controller_profiles;
static std::unordered_map<SDL_JoystickID, SDL_Joystick*> open_wheel_devices;

// All SDL haptic handles live on the same thread as the joystick event pump.
static struct {
    SDL_JoystickID id = -1;
    SDL_Haptic* handle = nullptr;
    int spring_effect = -1;
    int spring_strength = -1;
    int spring_uploaded_strength = -1;
    bool autocenter = false;
    bool rumble = false;
    bool open_failed = false;
} wheel_ffb;

static void close_wheel_ffb() {
    if (wheel_ffb.handle) {
        if (wheel_ffb.rumble) SDL_HapticRumbleStop(wheel_ffb.handle);
        if (wheel_ffb.autocenter) SDL_HapticSetAutocenter(wheel_ffb.handle, 0);
        if (wheel_ffb.spring_effect >= 0) SDL_HapticDestroyEffect(wheel_ffb.handle, wheel_ffb.spring_effect);
        SDL_HapticClose(wheel_ffb.handle);
    }
    wheel_ffb = {};
    wheel_ffb.id = -1;
    wheel_ffb.spring_effect = -1;
    wheel_ffb.spring_strength = -1;
    wheel_ffb.spring_uploaded_strength = -1;
}

static bool matches_wheel_binding(SDL_Joystick* joystick, const InputField& field) {
    if (!joystick || field.input_type != InputType::JoystickAxis || field.device_guid.empty()) return false;
    char guid[33]{};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), guid, sizeof(guid));
    if (field.device_guid != guid) return false;
    if (!field.device_serial.empty()) {
        const char* serial = SDL_JoystickGetSerial(joystick);
        return serial && field.device_serial == serial;
    }
    if (!field.device_path.empty()) {
        const char* path = SDL_JoystickPath(joystick);
        return path && field.device_path == path;
    }
    return true;
}

static SDL_HapticEffect center_spring_effect(int percent) {
    SDL_HapticEffect effect{};
    effect.type = SDL_HAPTIC_SPRING;
    effect.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
    effect.condition.length = SDL_HAPTIC_INFINITY;
    effect.condition.right_sat[0] = effect.condition.left_sat[0] = 0xFFFF;
    effect.condition.right_coeff[0] = effect.condition.left_coeff[0] =
        static_cast<Sint16>(0x7FFF * percent / 100);
    return effect;
}

static void set_center_spring(int percent) {
    if (wheel_ffb.spring_effect >= 0) {
        if (percent == 0) {
            SDL_HapticStopEffect(wheel_ffb.handle, wheel_ffb.spring_effect);
            return;
        }
        SDL_HapticEffect effect = center_spring_effect(percent);
        if ((percent == wheel_ffb.spring_uploaded_strength ||
             SDL_HapticUpdateEffect(wheel_ffb.handle, wheel_ffb.spring_effect, &effect) == 0) &&
            SDL_HapticRunEffect(wheel_ffb.handle, wheel_ffb.spring_effect, 1) == 0) {
            wheel_ffb.spring_uploaded_strength = percent;
            return;
        }
        std::fprintf(stderr, "[wheel-ffb] spring failed: %s\n", SDL_GetError());
        SDL_HapticDestroyEffect(wheel_ffb.handle, wheel_ffb.spring_effect);
        wheel_ffb.spring_effect = -1;
    }
    if (wheel_ffb.autocenter && SDL_HapticSetAutocenter(wheel_ffb.handle, percent) != 0) {
        std::fprintf(stderr, "[wheel-ffb] autocenter failed: %s\n", SDL_GetError());
        wheel_ffb.autocenter = false;
    }
}

static SDL_Joystick* steering_joystick() {
    const int profile = profiles::get_wheel_profile_index();
    if (profile < 0 || !profiles::is_wheel_selected(0)) return nullptr;
    for (GameInput direction : {GameInput::X_AXIS_NEG, GameInput::X_AXIS_POS}) {
        for (size_t slot = 0; slot < num_bindings_per_input; ++slot) {
            const InputField& field = profiles::get_input_binding(profile, direction, slot);
            for (int i = 0, count = SDL_NumJoysticks(); i < count; ++i) {
                SDL_Joystick* joystick = SDL_JoystickFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
                if (matches_wheel_binding(joystick, field)) return joystick;
            }
        }
    }
    return nullptr;
}

void update_wheel_force_feedback(uint16_t game_rumble, bool rumble_changed) {
    SDL_Joystick* joystick = steering_joystick();
    const SDL_JoystickID id = joystick ? SDL_JoystickInstanceID(joystick) : -1;
    if (wheel_ffb.id != id) close_wheel_ffb();
    if (id < 0) return;
    if (wheel_ffb.open_failed) return;

    if (!wheel_ffb.handle) {
        static bool haptics_unavailable = false;
        if (haptics_unavailable) return;
        if (SDL_WasInit(SDL_INIT_HAPTIC) == 0 && SDL_InitSubSystem(SDL_INIT_HAPTIC) != 0) {
            std::fprintf(stderr, "[wheel-ffb] SDL haptics unavailable: %s\n", SDL_GetError());
            haptics_unavailable = true;
            return;
        }
        wheel_ffb.id = id;
        if (!SDL_JoystickIsHaptic(joystick)) {
            std::fprintf(stderr, "[wheel-ffb] %s: no haptic support\n", SDL_JoystickName(joystick));
            wheel_ffb.open_failed = true;
            return;
        }
        wheel_ffb.handle = SDL_HapticOpenFromJoystick(joystick);
        if (!wheel_ffb.handle) {
            std::fprintf(stderr, "[wheel-ffb] cannot open %s: %s\n", SDL_JoystickName(joystick), SDL_GetError());
            wheel_ffb.open_failed = true;
            return;
        }
        const unsigned int features = SDL_HapticQuery(wheel_ffb.handle);
        wheel_ffb.autocenter = (features & SDL_HAPTIC_AUTOCENTER) != 0;
        // A wheel's position-dependent spring is preferable to a driver's global
        // autocenter setting, which some drivers claim but silently ignore.
        if (features & SDL_HAPTIC_SPRING) {
            SDL_HapticEffect effect = center_spring_effect(profiles::get_wheel_center_strength());
            wheel_ffb.spring_effect = SDL_HapticNewEffect(wheel_ffb.handle, &effect);
            if (wheel_ffb.spring_effect >= 0)
                wheel_ffb.spring_uploaded_strength = profiles::get_wheel_center_strength();
            else
                std::fprintf(stderr, "[wheel-ffb] spring upload failed: %s\n", SDL_GetError());
        }
        wheel_ffb.rumble = SDL_HapticRumbleSupported(wheel_ffb.handle) > 0 &&
                           SDL_HapticRumbleInit(wheel_ffb.handle) == 0;
        std::fprintf(stderr, "[wheel-ffb] %s: features=0x%X spring=%s, rumble=%s\n", SDL_JoystickName(joystick),
                     features, wheel_ffb.spring_effect >= 0 ? "condition" : wheel_ffb.autocenter ? "autocenter" : "unsupported",
                     wheel_ffb.rumble ? "available" : "unsupported");
    }

    const int center = profiles::get_wheel_center_strength();
    if (center != wheel_ffb.spring_strength) {
        wheel_ffb.spring_strength = center;
        set_center_spring(center);
    }
    if (rumble_changed && wheel_ffb.rumble) {
        // A wheel's force motor responds poorly to the tiny duty-cycle levels
        // of an N64 Rumble Pak. Preserve zero and full scale, lift the quiet hits.
        const float level = game_rumble / 65535.0f;
        const float strength = std::min(1.0f, 1.5f * std::pow(level, 0.4f)) *
                               (profiles::get_wheel_rumble_strength() / 100.0f);
        if (strength > 0.0f) {
            if (SDL_HapticRumblePlay(wheel_ffb.handle, strength, 250) != 0) {
                std::fprintf(stderr, "[wheel-ffb] rumble playback failed: %s\n", SDL_GetError());
                wheel_ffb.rumble = false;
            }
        }
        else SDL_HapticRumbleStop(wheel_ffb.handle);
    }
}

static void open_unmapped_joysticks() {
    // The initial JOYDEVICEADDED events can be consumed during early SDL/renderer setup.
    // Enumeration remains available, so open every currently connected raw device here too.
    for (int i = 0, count = SDL_NumJoysticks(); i < count; ++i) {
        if (SDL_IsGameController(i)) continue;
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        if (id < 0 || open_wheel_devices.contains(id)) continue;
        if (SDL_Joystick* joystick = SDL_JoystickOpen(i)) {
            open_wheel_devices.emplace(id, joystick);
            std::fprintf(stderr, "[recompinput] raw joystick opened: %s (instance %d)\n",
                         SDL_JoystickName(joystick), id);
        }
    }
    for (auto it = open_wheel_devices.begin(); it != open_wheel_devices.end();) {
        if (SDL_JoystickGetAttached(it->second)) {
            ++it;
        } else {
            if (wheel_ffb.id == it->first) close_wheel_ffb();
            SDL_JoystickClose(it->second);
            it = open_wheel_devices.erase(it);
        }
    }
}

static InputField wheel_input(SDL_JoystickID id, InputType type, int input_id) {
    InputField field{type, input_id};
    SDL_Joystick* joystick = SDL_JoystickFromInstanceID(id);
    if (joystick != nullptr) {
        char guid[33]{};
        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), guid, sizeof(guid));
        field.device_guid = guid;
        if (const char* serial = SDL_JoystickGetSerial(joystick)) field.device_serial = serial;
        if (const char* path = SDL_JoystickPath(joystick)) field.device_path = path;
        if (const char* name = SDL_JoystickName(joystick)) field.device_name = name;
    }
    return field;
}

static int get_or_create_controller_profile_index(ControllerGUID guid) {
    std::string default_profile_key = profiles::get_string_from_controller_guid(guid);
    int profile_index = profiles::get_input_profile_by_key(default_profile_key);
    if (profile_index < 0) {
        profile_index = profiles::add_input_profile(default_profile_key, "Controller", InputDevice::Controller, false);
        profiles::reset_profile_bindings(profile_index, InputDevice::Controller);
    }
    return profile_index;
}

int ensure_controller_profile(SDL_GameController* controller) {
    if (!controller) return -1;
    const ControllerGUID guid = profiles::get_guid_from_sdl_controller(controller);
    if (guid.hash == 0) return -1;
    int profile_index = profiles::get_controller_profile_index_from_sdl_controller(controller);
    if (profile_index < 0) {
        profile_index = get_or_create_controller_profile_index(guid);
        profiles::add_controller(guid, profile_index);
    }
    return profile_index;
}

void purge_deferred_controller_profiles() {
    for (auto &guid_pair : deferred_controller_profiles) {
        int profile_index = get_or_create_controller_profile_index(guid_pair.second);
        profiles::add_controller(guid_pair.second, profile_index);
    }
    deferred_controller_profiles.clear();
}

bool sdl_event_filter(void* userdata, SDL_Event* event) {
    switch (event->type) {
    case SDL_EventType::SDL_KEYDOWN:
    {
        SDL_KeyboardEvent* keyevent = &event->key;

        // Skip repeated events when not in the menu
        if (!recompui::is_context_capturing_input() &&
            event->key.repeat) {
            break;
        }

        if ((keyevent->keysym.scancode == SDL_Scancode::SDL_SCANCODE_RETURN && (keyevent->keysym.mod & SDL_Keymod::KMOD_ALT)) ||
            keyevent->keysym.scancode == SDL_Scancode::SDL_SCANCODE_F11
            ) {
            recompui::config::graphics::toggle_fullscreen();
        }
        if (binding::is_binding()) {
            if (keyevent->keysym.scancode == SDL_Scancode::SDL_SCANCODE_ESCAPE) {
                binding::stop_scanning();
            }
            else if (!keyevent->repeat &&
                     (binding::get_scanning_device() == InputDevice::Keyboard || binding::is_wheel_being_bound())) {
                binding::set_scanned_input({ InputType::Keyboard, keyevent->keysym.scancode });
            }
        }
        else {
            if (!should_override_keystate(keyevent->keysym.scancode, static_cast<SDL_Keymod>(keyevent->keysym.mod))) {
                queue_if_enabled(event);
            }
        }
    }
    break;
    case SDL_EventType::SDL_CONTROLLERDEVICEADDED:
    {
        SDL_ControllerDeviceEvent* controller_event = &event->cdevice;
        SDL_GameController* controller = SDL_GameControllerOpen(controller_event->which);
        printf("Controller added: %d\n", controller_event->which);
        if (controller != nullptr) {
            printf("  Instance ID: %d\n", SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)));
            printf("  Path: %s\n", SDL_JoystickPath(SDL_GameControllerGetJoystick(controller)));
            recompinput::add_controller_state(SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)), controller);

            if (SDL_GameControllerHasSensor(controller, SDL_SensorType::SDL_SENSOR_GYRO) && SDL_GameControllerHasSensor(controller, SDL_SensorType::SDL_SENSOR_ACCEL)) {
                SDL_GameControllerSetSensorEnabled(controller, SDL_SensorType::SDL_SENSOR_GYRO, SDL_TRUE);
                SDL_GameControllerSetSensorEnabled(controller, SDL_SensorType::SDL_SENSOR_ACCEL, SDL_TRUE);
            }
        }
        
        ControllerGUID guid = profiles::get_guid_from_sdl_controller(controller);
        if (profiles::get_controller_by_guid(guid) < 0) {
            if (players::is_single_player_mode()) {
                deferred_controller_profiles[guid.hash] = guid;
            } else {
                int profile_index = get_or_create_controller_profile_index(guid);
                profiles::add_controller(guid, profile_index);
            }
        }
    }
    break;
    case SDL_EventType::SDL_CONTROLLERDEVICEREMOVED:
    {
        SDL_ControllerDeviceEvent* controller_event = &event->cdevice;
        printf("Controller removed: %d\n", controller_event->which);
        recompinput::remove_controller_state(controller_event->which);
    }
    break;
    case SDL_EventType::SDL_JOYDEVICEADDED:
        open_unmapped_joysticks();
        break;
    case SDL_EventType::SDL_JOYDEVICEREMOVED:
        if (auto it = open_wheel_devices.find(event->jdevice.which); it != open_wheel_devices.end()) {
            if (wheel_ffb.id == it->first) close_wheel_ffb();
            SDL_JoystickClose(it->second);
            open_wheel_devices.erase(it);
        }
        break;
    case SDL_EventType::SDL_JOYBUTTONDOWN:
        if (profiles::is_wheel_selected(0) && !binding::is_binding()) {
            // Temporary field diagnostic: the binding trace in profiles.cpp then shows
            // whether this physical press became N64 B. Limit menu noise per launch.
            static int wheel_button_logs = 0;
            if (wheel_button_logs++ < 100) {
                SDL_Joystick* source = SDL_JoystickFromInstanceID(event->jbutton.which);
                const char* name = source ? SDL_JoystickName(source) : nullptr;
                wheel_debug_log("[wheel-button] " + std::string(name ? name : "disconnected") +
                                " button " + std::to_string(event->jbutton.button));
            }
        }
        if (binding::is_wheel_being_bound()) {
            binding::set_scanned_input(wheel_input(event->jbutton.which, InputType::JoystickButton, event->jbutton.button));
        } else {
            queue_if_enabled(event);
        }
        break;
    case SDL_EventType::SDL_JOYBUTTONUP:
        queue_if_enabled(event);
        break;
    case SDL_EventType::SDL_JOYHATMOTION:
        if (binding::is_wheel_being_bound() && event->jhat.value != SDL_HAT_CENTERED) {
            binding::set_scanned_input(wheel_input(event->jhat.which, InputType::JoystickHat,
                event->jhat.hat * 16 + event->jhat.value));
        } else if (!binding::is_wheel_being_bound()) {
            queue_if_enabled(event);
        }
        break;
    case SDL_EventType::SDL_JOYAXISMOTION:
        if (binding::is_wheel_being_bound()) {
            const int rest = binding::wheel_axis_rest(event->jaxis.which, event->jaxis.axis);
            const int movement = (int)event->jaxis.value - rest;
            if (std::abs(movement) < 16384) break;
            InputField field = wheel_input(event->jaxis.which, InputType::JoystickAxis,
                movement > 0 ? event->jaxis.axis + 1 : -event->jaxis.axis - 1);
            field.axis_rest = rest;
            binding::set_scanned_input(field);
        } else {
            queue_if_enabled(event);
        }
        break;
    case SDL_EventType::SDL_QUIT: {
        if (!ultramodern::is_game_started()) {
            ultramodern::quit();
            return true;
        }

        recompui::open_quit_game_prompt();
        recompui::activate_mouse();
        break;
    }
    case SDL_EventType::SDL_MOUSEWHEEL:
    {
        SDL_MouseWheelEvent* wheel_event = &event->wheel;
        recompinput::add_mouse_wheel_delta(wheel_event->y * (wheel_event->direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1));
    }
    queue_if_enabled(event);
    break;
    case SDL_EventType::SDL_CONTROLLERBUTTONDOWN:
        if (binding::is_binding() && binding::is_controller_being_bound(event->cbutton.which)) {
            // TODO: Needs the controller profile index.
            auto menuToggleBinding0 = profiles::get_input_binding(0, GameInput::TOGGLE_MENU, 0);
            auto menuToggleBinding1 = profiles::get_input_binding(0, GameInput::TOGGLE_MENU, 1);
            // note - magic number: 0 is InputType::None
            if ((menuToggleBinding0.input_type != InputType::None && event->cbutton.button == menuToggleBinding0.input_id) ||
                (menuToggleBinding1.input_type != InputType::None && event->cbutton.button == menuToggleBinding1.input_id)) {
                binding::stop_scanning();
            }
            else if (binding::get_scanning_device() == InputDevice::Controller) {
                SDL_ControllerButtonEvent* button_event = &event->cbutton;
                GameInput scanning_game_input = binding::get_scanning_game_input();
                if ((scanning_game_input == GameInput::TOGGLE_MENU ||
                    scanning_game_input == GameInput::ACCEPT_MENU ||
                    scanning_game_input == GameInput::APPLY_MENU) && (
                    button_event->button == SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_DPAD_UP ||
                    button_event->button == SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_DPAD_DOWN ||
                    button_event->button == SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_DPAD_LEFT ||
                    button_event->button == SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
                    break;
                }

                binding::set_scanned_input({ InputType::ControllerDigital, button_event->button });
            }
        }
        else {
            queue_if_enabled(event);
        }
        break;
    case SDL_EventType::SDL_CONTROLLERAXISMOTION:
        if (binding::is_controller_being_bound(event->caxis.which)) {
            GameInput scanning_game_input = binding::get_scanning_game_input();
            if (scanning_game_input == GameInput::TOGGLE_MENU ||
                scanning_game_input == GameInput::ACCEPT_MENU ||
                scanning_game_input == GameInput::APPLY_MENU) {
                break;
            }

            SDL_ControllerAxisEvent* axis_event = &event->caxis;
            float axis_value = axis_event->value * (1/32768.0f);
            if (axis_value > recompinput::axis_digital_threshold) {
                SDL_Event set_stick_return_event;
                set_stick_return_event.type = SDL_USEREVENT;
                set_stick_return_event.user.code = axis_event->axis;
                set_stick_return_event.user.data1 = nullptr;
                set_stick_return_event.user.data2 = nullptr;
                recompui::queue_event(set_stick_return_event);

                binding::set_scanned_input({ InputType::ControllerAnalog, axis_event->axis + 1 });
            }
            else if (axis_value < -recompinput::axis_digital_threshold) {
                SDL_Event set_stick_return_event;
                set_stick_return_event.type = SDL_USEREVENT;
                set_stick_return_event.user.code = axis_event->axis;
                set_stick_return_event.user.data1 = nullptr;
                set_stick_return_event.user.data2 = nullptr;
                recompui::queue_event(set_stick_return_event);

                binding::set_scanned_input({ InputType::ControllerAnalog, -axis_event->axis - 1 });
            }
        }
        else {
            queue_if_enabled(event);
        }
        break;
    case SDL_EventType::SDL_CONTROLLERSENSORUPDATE:
        if (event->csensor.sensor == SDL_SensorType::SDL_SENSOR_ACCEL) {
            // Convert acceleration to g's.
            float x = event->csensor.data[0] / SDL_STANDARD_GRAVITY;
            float y = event->csensor.data[1] / SDL_STANDARD_GRAVITY;
            float z = event->csensor.data[2] / SDL_STANDARD_GRAVITY;
            ControllerState& state = recompinput::get_controller_state(event->csensor.which);
            state.latest_accelerometer[0] = x;
            state.latest_accelerometer[1] = y;
            state.latest_accelerometer[2] = z;
        }
        else if (event->csensor.sensor == SDL_SensorType::SDL_SENSOR_GYRO) {
            // constexpr float gyro_threshold = 0.05f;
            // Convert rotational velocity to degrees per second.
            constexpr float rad_to_deg = 180.0f / M_PI;
            float x = event->csensor.data[0] * rad_to_deg;
            float y = event->csensor.data[1] * rad_to_deg;
            float z = event->csensor.data[2] * rad_to_deg;
            ControllerState& state = recompinput::get_controller_state(event->csensor.which);
            uint64_t cur_timestamp = event->csensor.timestamp;
            uint32_t delta_ms = cur_timestamp - state.prev_gyro_timestamp;
            state.motion.ProcessMotion(x, y, z, state.latest_accelerometer[0], state.latest_accelerometer[1], state.latest_accelerometer[2], delta_ms * 0.001f);
            state.prev_gyro_timestamp = cur_timestamp;

            float rot_x = 0.0f;
            float rot_y = 0.0f;
            state.motion.GetPlayerSpaceGyro(rot_x, rot_y);
            recompinput::add_rotation_deltas(event->csensor.which, rot_x, rot_y);
        }
        break;
    case SDL_EventType::SDL_MOUSEMOTION:
        if (!recompinput::game_input_disabled()) {
            SDL_MouseMotionEvent* motion_event = &event->motion;
            recompinput::add_mouse_deltas(motion_event->xrel, motion_event->yrel);
        }
        queue_if_enabled(event);
        break;
    case SDL_EventType::SDL_DROPBEGIN:
        DropState.files_dropped.clear();
        break;
    case SDL_EventType::SDL_DROPFILE:
        DropState.files_dropped.emplace_back(std::filesystem::path(std::u8string_view((const char8_t*)(event->drop.file))));
        SDL_free(event->drop.file);
        break;
    case SDL_EventType::SDL_DROPCOMPLETE:
        recompui::drop_files(DropState.files_dropped);
        break;
    case SDL_EventType::SDL_CONTROLLERBUTTONUP:
        // Always queue button up events to avoid missing them during binding.
        recompui::queue_event(*event);
        break;
    default:
        queue_if_enabled(event);
        break;
    }
    playerassignment::process_sdl_event(event);
    return false;
}

void handle_events() {
    SDL_Event cur_event;
    static bool started = false;
    static bool exited = false;
    open_unmapped_joysticks();
    while (SDL_PollEvent(&cur_event) && !exited) {
        exited = sdl_event_filter(nullptr, &cur_event);

        bool has_mouse_sensitivity = recompui::config::general::has_mouse_sensitivity_option();

        // Lock the cursor if all three conditions are true: mouse aiming is enabled, game input is not disabled, and the game has been started. 
        bool cursor_locked = (has_mouse_sensitivity && recompui::config::general::get_mouse_sensitivity() != 0) && !recompinput::game_input_disabled() && ultramodern::is_game_started();

        // Hide the cursor based on its enable state, but override visibility to false if the cursor is locked.
        bool cursor_visible = recompui::get_cursor_visible();
        if (cursor_locked) {
            cursor_visible = false;
        }

        SDL_ShowCursor(cursor_visible ? SDL_ENABLE : SDL_DISABLE);
        SDL_SetRelativeMouseMode(cursor_locked ? SDL_TRUE : SDL_FALSE);
    }

    binding::stop_skipping_events();

    if (!started && ultramodern::is_game_started()) {
        started = true;
        recompui::process_game_started();
    }
}

}
