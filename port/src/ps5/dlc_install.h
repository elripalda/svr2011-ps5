// [ps5 port] A DLC package installed from the hub's DLC page, with progress,
// instead of while the game starts (see dlc_install.cpp).
#pragma once

#include <filesystem>
#include <string>

#include "../hub/hub_install.h"

namespace svr2011::ps5 {

// Extracts `package` (LIVE / PIRS / CON, as downloaded) into
// <user_data>/0000000000000000/5451085D/00000002/<name>/ with its .header,
// as the runtime's ContentManager::InstallContent does at the game's start.
// Progress in `p` (bytes of this package; `p.cancel` stops it); false with
// `error` saying why.
bool InstallDlcPackage(const std::filesystem::path& package, const std::filesystem::path& user_data,
                       const std::string& name, hub::install::Progress& p, std::string& error);

}  // namespace svr2011::ps5
