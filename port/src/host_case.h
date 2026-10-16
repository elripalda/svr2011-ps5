// [ps5 port] A game file by its name in any letter case. The disc's names are
// mixed (pac\evt\Nyujyo5.pac, pac\bg\bg06.pac) and the port asks for them as
// Windows lets it ("nyujyo5.pac", "BG06.PAC"); the PS5's file system minds the
// case, so those opens failed (a move pack merge that gave up, arena slots
// that couldn't be put back).
#pragma once

#include <filesystem>

namespace svr2011 {

// `p` when it exists; else `p` with each missing part matched to the folder's
// entries without regard to case; else `p` as it was. Windows: `p`.
std::filesystem::path HostCase(const std::filesystem::path& p);

// `p` relative to `base`, worked out from the names (std::filesystem::relative
// goes through the real paths first, and on the PS5 it gave nothing for the
// app folder's files: the DLC pacs and catalogs served from the move packs'
// overlay pointed at the overlay folder itself - the game waited on its
// loading screen for ever). The file system's own answer only when the names
// don't lead there.
std::filesystem::path RelativePath(const std::filesystem::path& p, const std::filesystem::path& base);

}  // namespace svr2011
