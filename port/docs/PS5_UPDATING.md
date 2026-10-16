# Updating to a new PC port release

Online only matches games on the same version with the same gameplay mods, so the PS5 port has to follow each release of [wwe-svr2011-pc](https://github.com/KaikoClanworth1/wwe-svr2011-pc).

This repository shares the PC port's git history, and the runtime has a `rexglue-pc` branch: the stock ReXGlue SDK with the PC port's SDK patch. So an update is a merge, not a re-port.

```sh
port/tools/ps5_update_upstream.sh v2.1.4 ../rexglue-ps5 port/out/ps5 ../ps5-plume-triangle
```

It:

1. merges the PC release into this branch (`README.md` stays the PS5 one, `README-PC.md` gets theirs),
2. applies its `rexglue-sdk-svr2011.patch` to the stock SDK as a new `rexglue-pc` commit and merges that into the runtime,
3. says if the game's code has to be generated again, or plume rebuilt,
4. builds and packages (with the last two arguments).

If both sides changed the same lines, it stops and lists the files. Keep the PS5 parts (`[ps5 port]` comments, `__PROSPERO__` blocks) and the PC port's new code, commit, and run it again.

## The release

- `eboot.bin` from `port/out/ps5/ps5-app/PPSA92011/`.
- The bundled mods: the `.svrmod` files from that release's `SvR2011-Mods-<version>.zip` go in the app's `Bundled Mods` folder. The app installs the new and changed ones at its next start and keeps players' on/off choices.
- `native_shaders/pipelines.list` if it changed.

The PC version shows in the log's first line and decides who you can play online.
