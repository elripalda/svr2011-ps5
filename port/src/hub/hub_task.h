// WWE SmackDown vs. Raw 2011 - the PS5 hub's jobs on a thread (installs, checks, mods, DLC and
// Discovery downloads): one at a time (hub_setup.cpp, hub_discover.cpp).
#pragma once

#include <string>
#include <thread>
#include <vector>

#include "hub_ui.h"

namespace svr2011::hub {

// An installer job on its thread (one at a time).
struct Hub::Task {
  std::thread thread;
  install::Progress p;
  bool ok = false;  // (a mod: installed)
  std::string message;
  std::vector<std::string> failed;  // (DLC: the package files that didn't install)
  void Cancel() { p.cancel = true; }
  ~Task() {
    p.cancel = true;  // (the hub closing: stops at the next piece)
    if (thread.joinable()) thread.join();
  }
};

}  // namespace svr2011::hub
