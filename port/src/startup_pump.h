// Keeps the game window answering Windows while the port prepares mods at
// start (move packs, arena and superstar mods): that work runs on the UI
// thread before the app's event loop, and a window that reads no messages
// for 5 s is "Not Responding" - clicking it then lets Windows close the game
// (a slow first start on a hard drive with a cold file cache looked like a
// crash). SDL_PumpEvents reads the OS messages into SDL's queue; the event
// loop handles them once it runs.
#pragma once

#include <SDL3/SDL_events.h>

#include <chrono>
#include <string>
#include <thread>

#if defined(__PROSPERO__)
namespace svr2011::hub {  // [ps5 port] the loading screen (hub/ps5_hub.cpp)
void StartupStep(const std::string& step, const std::string& detail, float fraction);
void PaintStartupScreen();
void CloseStartupScreen();
}  // namespace svr2011::hub
#endif

namespace svr2011 {

// [ps5 port] What the loading screen after the hub says is being done; and
// that it's done (it fades out over the game). Nothing on the other platforms.
inline void StartupStep([[maybe_unused]] const char* step, [[maybe_unused]] const char* detail = "") {
#if defined(__PROSPERO__)
  hub::StartupStep(step, detail, -1);
#endif
}
inline void StartupDone() {
#if defined(__PROSPERO__)
  hub::CloseStartupScreen();
#endif
}

// The UI thread (the one that runs the Install* calls), set by the app.
inline std::thread::id& StartupThread() {
  static std::thread::id id;
  return id;
}

// Cheap enough to call per file: pumps at most every 100 ms, only on the
// startup thread.
inline void KeepWindowAlive() {
#if defined(__PROSPERO__)
  hub::PaintStartupScreen();  // [ps5 port] (only on the UI thread, a few times a second)
  return;
#endif
  if (std::this_thread::get_id() != StartupThread()) return;
  using clock = std::chrono::steady_clock;
  static clock::time_point last;
  const auto now = clock::now();
  if (now - last < std::chrono::milliseconds(100)) return;
  last = now;
  SDL_PumpEvents();
}

}  // namespace svr2011
