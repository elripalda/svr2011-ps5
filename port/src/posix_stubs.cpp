// WWE SmackDown vs. Raw 2011 - what the Windows-only parts of the port
// provide, for other platforms (Android, Linux) until they have their own:
// PC music for entrances (music.cpp, XAudio2 + Media Foundation). (Crash
// reports: crash_report_posix.cpp.)
#include "music.h"

#include <rex/logging.h>

namespace svr2011 {

#if !defined(__ANDROID__) && !defined(__PROSPERO__)  // (Android, PS5: music.cpp)
void InstallUserMusic(const std::filesystem::path&) {
  REXLOG_INFO("user music: not available on this platform yet");
}
std::filesystem::path UserMusicFolder() { return {}; }
std::filesystem::path UserMusicSong(const std::string&) { return {}; }
// [ps5 port] Host-side sounds (USER PLAYLIST songs, menu clips): none yet,
// so no songs are listed and every sound is "finished" at once.
std::vector<std::filesystem::path> UserMusicSongs() { return {}; }
void PlayClip(const std::filesystem::path&) {}
int HostSoundStart(const std::filesystem::path&, bool, float) { return 0; }
void HostSoundStop(int) {}
void HostSoundPause(int, bool) {}
void HostSoundVolume(int, float) {}
bool HostSoundActive(int) { return false; }
#endif

}  // namespace svr2011
