// bar_watchdog.h -- captures all thread stacks if the SDL pump or game loop stalls.
#pragma once

namespace bar::watchdog {

// Start the monitor thread. Call once from main(), after the app config directory is usable (the
// report is written there) and before the first frame.
void install();

// One relaxed atomic increment. Call once per frame from update_gfx -- the callback that runs on the
// thread owning the window, which is the thread whose stall this watches for.
void heartbeat();

// One relaxed atomic update from the game's SI poll. A frozen game loop can leave the SDL pump
// and audio thread alive; while racing, that also needs to trigger a report.
void game_heartbeat(int game_state);

} // namespace bar::watchdog
