/**
 * [ps5 port] getcwd: the PS5's (libSceLibcInternal) jumped into guest memory
 * - the game's thread crashed at a match's bell, when the match recordings
 * asked std::filesystem::current_path for their logs folder. An app's own
 * folder is its current one. (A file of its own: ps5_libc_extras.c's other
 * functions clash with the payload SDK's libc.a in the CMake link.)
 */
#if defined(__PROSPERO__)
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char* getcwd(char* buf, size_t size) {
  static const char kCwd[] = "/app0";
  if (!buf) {
    if (!size) size = sizeof kCwd;
    if (!(buf = malloc(size))) {
      errno = ENOMEM;
      return NULL;
    }
  } else if (!size) {
    errno = EINVAL;
    return NULL;
  }
  if (size < sizeof kCwd) {
    errno = ERANGE;
    return NULL;
  }
  memcpy(buf, kCwd, sizeof kCwd);
  return buf;
}
#endif
