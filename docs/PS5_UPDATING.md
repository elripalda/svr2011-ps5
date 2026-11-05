# Updating the PS5 port to a new PC release

Online only works between the same versions, so the PS5 port follows every release of
KaikoClanworth1's PC port. The PS5 parts are kept apart so a new release merges in cleanly:

- the hub is `port/src/hub/` and the PS5 sound and DLC code is `port/src/ps5/`: PS5-only, the PC
  release never touches them;
- PS5 changes inside the game's code are marked `[ps5 port]` or sit under `__PROSPERO__`.

## The round

1. Bring in the release and build it:

   ```
   REXGLUE=<the rexglue tool> port/tools/ps5_update_upstream.sh v2.2.0 ../rexglue-ps5 port/out/ps5 ../ps5-plume-triangle
   ```

   It merges the PC port's code, puts its SDK patch into the PS5 runtime, generates the game's
   code again when the release needs it, then builds and packages `eboot.bin`.

2. If a merge stops: both sides changed those files. Keep the PS5 parts and the PC port's new
   code, `git add` them, `git commit --no-edit`, and run the same command again. It carries on.

3. From the PC release's zips, into the app folder:
   - `native_shaders/shaders.spv.pak`
   - `native_shaders/pipelines.list`: the PC list with the PS5's own lines added (`port/dist/pipelines.list`
     shows which)
   - `pad_icons/`
   - `Bundled Mods/`: the `.svrmod` files of `SvR2011-Mods-<tag>.zip`. The app installs the changed ones
     at its next start, switched off.

4. Look over what's new in the PC code:
   - a new read of a game file goes through `svr2011::HostCase` (the PS5's file system minds letter
     case);
   - a new feature that's Windows-only stays out of the PS5's menus.

5. Play an online match against the PC release before it goes out.
