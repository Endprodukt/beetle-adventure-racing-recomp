#pragma once

#include "recompinput.h"

namespace recompinput {
    void handle_events();
    void purge_deferred_controller_profiles();
    int ensure_controller_profile(SDL_GameController* controller);
}
