# SvR 2011 → PS5 port plan

Personal experiment: run the ReXGlue-recompiled SvR 2011 port as a native
homebrew app on a jailbroken PS5 (tested on 13.60). Bring-your-own-game: no
game files, XEX or generated code are ever stored in the repo.

## Verified on real hardware (probe payload, fw 13.600.007)

| Runtime needs | Result | Notes |
|---|---|---|
| shm_open (anon + named) mirrored mmaps | PASS | |
| Direct memory mapped twice (mirror) | PASS | even from a payload |
| ReXGlue's full 9-view 4.5 GB guest layout | PASS | 16 KB pages; MAP_FIXED into a PROT_NONE reserve (the Mac path) |
| getcontext/makecontext/swapcontext | **FAIL** | 0 switches; use own asm fiber switch instead (passes, ~0.03 µs) |
| 32 threads, 2 MB stacks (setstacksize or own), thread_local | PASS | default stack is only 64 KB → always set 2 MB |
| SIGSEGV/SIGBUS catch + resume | PASS | |
| Read/modify registers in the handler | PASS with fix | ucontext from the PS5 kernel = FreeBSD layout **+48 bytes**: `mcontext_t` starts at `(uint8_t*)uc + 64` (header says +16). rip at +224, rax +120, rdi +72 |

## Build status (Linux, as a stand-in for "not Windows")

- rexglue codegen built from source (clang 19), `default.xex` → 403 files / 210 MB C++.
- Runtime, rexgpu-xenos, plume: compile.
- Port's own 71 sources: compile after fixes below.

## Fixes made so far

1. `rexglue src/core/threading_posix.cpp`: `SystemTid()` uses the tid the thread
   records (Android path) everywhere; `pthread_gettid_np` needs glibc 2.42 and
   is absent on PS5.
2. `rexglue include/rex/chrono/chrono.h`: clock_cast fallback for any libc++ < 20.
3. `rexglue include/rex/string/numeric.h`: portable float from_chars for any libc++.
4. Build with `-fexperimental-library` (libc++ < 20 hides std::jthread/stop_token;
   PS5 SDK libc++ is 18.1 and ships libc++experimental.a).
5. `port/CMakeLists.txt`: launcher/ include dir for all non-Windows builds (ascii_fold.h).
6. CPU flags: `-march=znver2` (PS5 = Zen 2).

## PS5 build status (Oct 11)

- Runtime (librexruntime.a), Xenos GPU plugin (static), plume, all port sources and all
  403 generated files compile for x86_64-sie-ps5; the whole game links (CMake link) and
  links with RADV + the PS5 app CRT (tools/ps5_package.sh).
- Build: `cmake -S port -B port/out/ps5 -G Ninja -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_TOOLCHAIN_FILE=port/cmake/ps5-toolchain.cmake -DPS5_PAYLOAD_SDK=<sdk>
  -DREXSDK_DIR=<rexglue> -DREXGLUE_ENABLE_TRACY=OFF -DSDL_X11=OFF -DSDL_WAYLAND=OFF
  -DSDL_KMSDRM=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_OSS=OFF -DSDL_ALSA=OFF
  -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_HIDAPI=OFF
  -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_LIBUDEV=OFF -DSDL_VULKAN=OFF -DSDL_DUMMYVIDEO=ON
  -DSDL_DUMMYAUDIO=ON -DSDL_OFFSCREEN=OFF`, then `ninja -C port/out/ps5 svr2011`, then
  `tools/ps5_package.sh port/out/ps5 <ps5-plume-triangle> PPSA92011`.
- Codegen runs on the host (Linux, libstdc++ build of rexglue: the libc++ build hangs);
  then `tools/ps5_patch_generated.py generated/rexglue.cmake`.
- Title ID PPSA92011; app folder /data/homebrew/PPSA92011 = eboot.bin + sce_sys +
  sce_module + the installed PC game folder's data (pac, sound, movies, nxeart, plist360*.arc,
  pad_icons, native_shaders, DLC, Mods, default.xex). Saves/UserData go in the same folder.

### More fixes made for the PS5 build
- rexglue platform: `REX_PLATFORM_PS5` (+ LINUX); `rex/ps5_compat.h` (macros only: PCH-safe).
- Static rexruntime / rexgpu-xenos on PS5; gpu_plugin_loader calls rex_gpu_create directly.
- No `-mcmodel=large` on PS5 (94k .ltext sections broke the FSELF signer).
- FFmpeg: config_ps5_x86_64.h (from the Android x86-64 one); xma_decoder.cpp without PCH.
- One VMA (rexglue's 3.3.0) for plume too.
- libc gaps: ffs, timegm, getresuid/getresgid (src/core/ps5_libc_extras.c).
- Platform layer: Ps5DisplaySurface + VK_KHR_display in the Vulkan presenter; RADV's
  vk_icdGetInstanceProcAddr as the loader; SDL3 dummy video for windows/events;
  Ps5InputDriver (scePad); Ps5AudioDriver (sceAudioOut, s16 stereo, own thread).
- Game folder = /app0; guest memory via shm_open(SHM_ANON) + reserved MAP_FIXED views.
- Music/host sounds: stubs for now (posix_stubs.cpp).

## Status (Oct 11, test 19): playable

Boots in seconds, menus and matches at 60 fps at native 4K (render scale 3x),
DualSense with PlayStation button pictures, sound, DLC, saves kept between
starts. Fixes that got it there, in order:
- 0xE0000000 guest addresses sit 0x1000 higher in host memory with 16 KB pages
  (as on Windows / arm64 macOS): the runtime knew, the generated code didn't
  (REX_PHYS_HOST_OFFSET; codegen template + tools/ps5_patch_generated.py).
- 16 MiB host thread stacks from direct memory (libkernel's come from ~400 MiB
  of flexible memory: thread 18 failed).
- Imports only libkernel_sys exports (link, symlink, readlink, pathconf, fork,
  vfork, setsid) are stubbed: they were calls to address 0 (the save's
  create_hard_link hung the title screen).
- No keyboard/mouse driver (the game showed "A / Cross" pairs); the button
  picture copies get a command pair each and fresh records.
- DLC "<name> (1)" packages are recognised as installed (no 26 s reinstall).

## PS5 work list

### Core (rexglue)
- [ ] `REX_PLATFORM_PS5` (`__PROSPERO__`) platform define; FreeBSD-ish POSIX paths.
- [ ] fiber_posix → asm fiber switch (probe's `my_fiber_switch`).
- [ ] exception_handler_posix → PS5 branch reading mcontext at uc+64.
- [ ] threading_posix: replace `SYS_gettid`/`SYS_tgkill` (thr_self/thr_kill), SIGRTMIN
      suspend signal (pick SIGUSR2?), affinity (cpuset / no-op), pthread_setname_np.
- [ ] memory_posix: `/proc/self/maps` → no-op / sceKernelVirtualQuery; named shm path rules.
- [ ] filesystem_posix: `/proc/self/exe` → `/app0`.
- [ ] dbg_posix: `/proc/self/status` → no-op.
- [ ] GPU plugin: link rexgpu-xenos statically (no dlopen of our own .so).
- [ ] Every thread created with 2 MB+ stacks.

### Platform layer (new, replaces SDL on PS5)
- [ ] window_ps5 / windowed_app_context_ps5 / main: fullscreen, event loop.
- [ ] Vulkan surface via `VK_KHR_display` (PS5_Mesa winsys), like plume-ps5.
- [ ] Audio driver: sceAudioOut 48 kHz (Sonic port: 256-sample frames).
- [ ] Input driver: scePad (layout verified in ps5-plume-triangle).
- [ ] Port's own SDL users: music.cpp (disable at first), pad_types.cpp, keyboard_typing.cpp
      (sceImeDialog later), online_net.cpp, touch_controls.cpp (off).

### Toolchain / packaging (reuse UnleashedRecomp-Prospero's tools)
- [x] PS5 SDK fork (mihawk-99/PS5_PayloadSDK) via tools/setup-native-dependencies.sh
- [ ] PS5_Mesa RADV static lib via tools/build-radv.sh (wrapdb blocked here → patch from GitHub mirror)
- [ ] PacBrew libs (if any are needed; SDL not needed)
- [ ] CMake toolchain file for x86_64-sie-ps5 (+ `-femulated-tls`, emutls/thread_local destructor fix from Sonic port)
- [ ] FFmpeg (XMA decode subset) cross-compiled via the SDK's CMake
- [ ] eboot.bin FSELF + sce_module/libc.prx + sce_sys via Sonic's ps5-native-tool
- [ ] Title ID + data folder (TBD with Ruben)

### Milestones on the console
1. ps5-plume-triangle builds here and runs on Ruben's PS5 (pipeline check).
2. SvR boots to the runtime log (no graphics).
3. First frame / title screen.
4. Input + audio.
5. In-ring gameplay.
