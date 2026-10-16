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

}  // namespace svr2011
