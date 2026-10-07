# Provenance of libopenage/ui/agesxr (XR fork)

These files are **copies** of the GL-free user interface code of the XR superproject
(XR-openage, `native/xr/` and `native/app/`): CPU canvas with the vector look of the VR
menu (`xr_canvas.*`), text rasterizer on top of stb_truetype (`text_raster.*`), the HUD bar
(`xr_hud*`), the game menu / context menu / match board (`xr_game_ui.*`), the HUD data flow
(`hud_feed.h`, `engine_status.h`) and the log macros (`xr_log.h`).

- License: MIT, own code of the XR ports (no engine or game sources, no game graphics;
  the stone/wood/grass look is an own vector drawing). MIT is compatible with the GPL-3+
  of openage; the copies are used here under the GPL-3+ of the fork.
- Origin chain: XR-openage `native/xr` <- XR-StardewValley `native/xr` <- XR-PvZPortable
  <- XR-CorsixTH <- XRShell (`vr_menu.cpp`, `text_raster.*`), see `native/xr/PROVENANCE.md`
  of the superproject.
- **Source of truth is the superproject.** Do not edit the copies here; change `native/xr`
  or `native/app` and run `bash scripts/sync-ui-to-fork.sh` (`--check` compares). The
  Android build of this fork (`scripts/wsl/android/30-openage.sh`) builds the fork alone,
  which is why the files are copied instead of referenced.
- Fork-only files in this directory: `stb_truetype.h` (public domain, v1.26, same commit
  `2c980bb5` as the superproject's FetchContent), `stb_truetype_impl.cpp`,
  `CMakeLists.txt`, this file.
- Engine side (fork only): `libopenage/ui/game_ui_controller.*` (data flow and input),
  `libopenage/renderer/stages/ui/` (textures, render pass), shaders `ui_overlay.*.glsl`.
