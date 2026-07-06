<div align=center>

<img src="extras/banner.png" alt="Banner" width="40%">

</div>
<h1 align=center>Chrono Trigger — Nintendo Switch port</h1>

</div>

This is a wrapper/port of the Android version of *Chrono Trigger*
(`com.square_enix.android_googleplay.chrono`, v2.1.5). It loads the original game
binaries, patches them and runs them: we natively run the original Android `.so`
files inside a minimal emulated Android environment.

### Features

* **Extensive `config.ini`** — screen resolution (independent per handheld/docked,
  up to 4K supersampling), language, custom font loading, text shadows,
  performance tuning, and controller remapping, all editable without rebuilding.
* **Mod support** — drop ChronoMod-compatible `.ctp` patch archives or loose-file
  "folder mods" into a `mods/` folder and they're applied to `resources.bin` at
  startup, with no changes to your original file.
* **Custom font loading** — render the engine's system-font labels with a TTF of
  your choice (a matching pixel font, ChronoType, ships with the project) instead
  of the Switch's shared system font, with auto-fit scaling, horizontal squeeze,
  and an optional SNES-style drop shadow.
* **Visual & gameplay fixes** — root-cause fixes for the game's motion
  shimmer/scoot artifact, which turned out to have several independent
  causes: an uneven, non-square zoom baked into the field and world-map
  cameras, a mismatched global design resolution, and per-frame timing
  jitter. Also: crisp nearest-neighbour texture rendering, Equipment menu
  text alignment, smoother diagonal movement, and a corrected
  cursor/selection highlight.
* **Switch-native controller support** — real button prompts and glyphs instead
  of touch/keyboard prompts, optional right-stick-as-left-stick mirroring, and
  remapping of the otherwise-unused ZL/ZR/+/− buttons.
* **Touch UI removed** — the mobile-only on-screen button overlays (field menu,
  world-map menu/map/warp, menu system back/close buttons, minigame touch
  buttons) are hidden automatically.
* **Tidier install layout** — saves, mods, and settings each live in their own
  subfolder; upgrading from an older install migrates everything automatically.

### How to install

You're going to need the **`.apk`** for version 2.1.5. From it you need:
* `lib/arm64-v8a/libchrono.so` — the engine
* `lib/arm64-v8a/libc++_shared.so` — the C++ runtime it depends on
* the **entire `assets/` folder** — the game data (`resources.bin`, `001.dat`…
  `008.dat`, `007-en.dat`, `Shaders/`, `build_date.txt`)

To install:
1. Create a folder called `ct` in the `switch` folder on your SD card.
2. From your `.apk`, extract **`lib/arm64-v8a/libchrono.so`** to `/switch/ct/`.
3. From your `.apk`, extract **`lib/arm64-v8a/libc++_shared.so`** to `/switch/ct/`.
4. From your `.apk`, copy the **whole `assets/` directory** to `/switch/ct/assets/`.
5. Copy this project's **`font/`** folder to `/switch/ct/font/` (bundles
   ChronoType, used by the [custom font](#configuration) feature by default).
6. Copy **`ct_nx.nro`** into `/switch/ct/`.
7. *(Optional)* Create `/switch/ct/mods/` and drop in any `.ctp` mods or folder
   mods you want loaded — see [Mods](#mods) below.

If you're upgrading an existing install, just overwrite `ct_nx.nro` (and
`libchrono.so`/`libc++_shared.so`/`assets/` if they changed) — your settings and
save data are migrated automatically on first launch, see **Notes** below.

### Notes

This will not work in applet/album mode. Use a game override (hold R on a title)
or a forwarder, so the homebrew gets the full memory and required syscalls.

Everything the port reads or writes lives under `/switch/ct/`:
* `config.ini` — all settings (see **Configuration**)
* `saves/` — save data (`common.bin`, `meta.bin`, `Chrono_sp_*.dat`) and the
  cocos2d-x `Cocos2dxPrefsFile.txt` settings store, kept in their own subfolder
  so the top-level directory stays tidy
* `mods/` — optional `.ctp` / folder mods (see **Mods**)
* `font/` — optional TTFs for the custom font feature
* `debug.log` — diagnostic log

**Upgrading from an older version:** if you previously used the old flat
`config.txt`, it's read once, migrated into the new `config.ini` format, and
renamed to `config.txt.bak` so it's never read again. Likewise, if your saves
were still sitting directly in `/switch/ct/` from an older install, they're
moved into `/switch/ct/saves/` automatically the first time you launch the
updated build — nothing else in the top-level folder is touched.

### Configuration

`config.ini` is created (or migrated from an older `config.txt`) on first run.
It's grouped into sections; here's what each key does and its default.

**Screen**
* `screen_width_handheld` / `screen_height_handheld`, `screen_width_docked` /
  `screen_height_docked` *(default 1280x720 / 1920x1080)* — render resolution
  per mode. `-1` (or anything out of range) auto-picks 1280x720 handheld /
  1920x1080 docked. An explicit value is honoured up to 4K, so you can also
  render **above** native for supersampling (e.g. 2560x1440 docked) — keep a
  16:9 ratio to avoid stretching. Applied live: changing dock state mid-session
  resizes the render target with no restart needed.

**Language**
* `language` *(default `en`)* — selects the in-game text/assets. One of
  `en fr de it es ja ko zh zh-Hant` (`zh`/`zh-Hans` = Simplified Chinese,
  `zh-Hant` = Traditional Chinese). Anything other than `ja` uses the
  localized (`-en` etc.) data.

**Font**
* `game_font` *(default 1)* — render the engine's system-font labels with a TTF
  instead of the Switch's shared font, so they match the in-game look. The
  game's own TTF-rendered labels are unaffected either way. Glyphs the game
  font lacks fall back to the shared font.
* `game_font_path` *(default `/switch/ct/font/ChronoType/ChronoType.ttf`)* —
  path to the TTF to use. If empty, a few common asset locations are probed;
  if none load, falls back to the shared font.
* `game_font_scale` *(default 0.90)* — fine-tune multiplier on the game font's
  auto-fitted size. `1.0` = auto (cap height matched to the shared font); lower
  if it still looks too big, higher if too small.
* `game_font_xscale` *(default 1.0)* — horizontal-only squeeze, for condensing a
  wide/monospaced font so the game's pre-broken lines stay inside their window.
  A proportional font like ChronoType usually needs no squeeze.

**Text shadow**
* `text_shadow` *(default 1)* — draw a crisp, hard-edged drop shadow behind the
  system-font labels, classic CT/SNES style.
* `text_shadow_scale` *(default 1.0)* — multiplier on the shadow's auto offset.
* `text_shadow_alpha` *(default 200, 0-255)* — shadow opacity; also scales with
  each label's own alpha, so fades stay consistent.

**Performance**
* `gl_threaded` *(default 0)* — run mesa's GL command submission on a second CPU
  core (glthread), offloading work the single-threaded engine spends inside the
  GPU driver. No-op if mesa lacks it; disable if it causes trouble.
* `gl_no_error` *(default 1)* — skip mesa's internal validation of GL calls,
  removing CPU overhead on a path that fires constantly. The engine's calls are
  already well-formed, so this is safe to leave on.

**Runtime binary patches** — applied to `libchrono.so` at boot; each is
independent and can be disabled if it ever causes trouble. Most of these
require a relaunch to take effect after changing.

The game's "everything shimmers/scoots while moving" artifact isn't one bug —
it comes from at least three independent causes, each fixed by a different
flag below: the field/world-map camera's own **uneven, non-square zoom**
(`field_zoom_fix`/`map_zoom_fix`), a **mismatched global design resolution**
that scales every layer by a non-integer factor (`ui_scale_fix`), and
**per-frame timing jitter** unrelated to scaling or filtering at all
(`fixed_timestep`). Nearest-neighbour sampling (`remove_bilinear_filter`/
`force_nearest`) is separate again: it controls filtering sharpness, not
shimmer, and by itself doesn't make anything "pixel-perfect" — sampling a
still-uneven or non-integer scale with nearest-neighbour is part of what
produces the shimmer, not a fix for it.

* `remove_mobile_ui` *(default 1)* — hide the on-screen touch-overlay buttons
  (field/world-map/title-screen buttons, menu back/close buttons, minigame
  touch buttons).
* `cursor_fix` *(default 1)* — keep selected-item text white with a dark
  semi-transparent highlight, instead of the original cream colour-inversion.
* `remove_bilinear_filter` *(default 1)* — force nearest-neighbour (point)
  texture sampling instead of bilinear filtering, so edges are crisp rather
  than blended/blurred. Doesn't by itself guarantee square or evenly-spaced
  pixels — that's what the zoom/scale fixes below are for.
* `fixed_timestep` *(default 1)* — force a constant 1/60s frame delta instead
  of the measured wall-clock gap, removing dt-jitter drift (keeps the intro
  movie's audio in sync, smooths anything timed off delta-time). This is a
  timing fix, confirmed unrelated to filtering or scaling.
* `ui_scale_fix` *(default 1)* — fixes the game's leftover iPhone-5 design
  resolution (568x320), which doesn't divide evenly into any Switch panel, so
  every layer gets stretched by a fractional, per-frame-shifting scale.
  Corrects it to an exact 16:9 640x360, which divides every Switch resolution
  cleanly, at the cost of showing a small amount (~12%) more of the world per
  screen edge.
* `force_nearest` *(default 0)* — total-coverage version of
  `remove_bilinear_filter` that rewrites every GL texture filter at the
  wrapper level, including buffers the binary patch alone can't reach.
* `game_area_width_fix` *(default 1)* — makes the game's UI-layer width
  calculation adaptive instead of hardcoded to the iPhone-5 width; fixes a
  horizontal-only UI stretch. No-op at stock design resolution.
* `menu_alignment_fix` *(default 1)* — fixes the Equipment menu's stat/HP
  block/button text sitting slightly higher than the item list rows.
* `field_zoom_fix` *(default 1)* / `field_zoom` *(default 2.0)* — the field
  camera has its own baked-in, uneven zoom (X and Y scaled by different
  amounts, independent of `ui_scale_fix`'s design-resolution issue above),
  which is the actual root cause of the field screen's shimmer: it stretches
  art pixels and produces an uneven tight/loose pixel-width pattern as the
  camera scrolls. This flag squares the zoom to a uniform, integer value on
  both axes. `field_zoom` sets the zoom level (lower = see more map, higher =
  bigger on-screen tiles); whole-number values stay shimmer-free in both
  handheld and docked.
* `map_zoom_fix` *(default 1)* / `map_zoom` *(default 2.0)* — the same fix and
  zoom control, for the overworld map screen's own uneven zoom.

**Input / controller**
* `native_controller` *(default 1)* — drive the engine purely through its
  native controller path, so on-screen prompts and dialogue button tags show
  Switch buttons. Turn off only for legacy keyboard-style compatibility.
* `controller_glyphs` *(default 1)* — force in-message button tags to the
  controller glyph set (honours any A/B swap) regardless of the engine's own
  last-input-device detection.
* `fix_diagonal_movement` *(default 1)* — smooths diagonal field movement to
  match the speed of cardinal movement (purely visual; no save-data impact).
* `right_stick_mirror` *(default 1)* — lets the right stick also drive
  movement, as if it were the left stick. Right-stick tilt takes priority when
  both sticks are active.
* `key_zl` / `key_zr` / `key_plus` / `key_minus` *(default: unremapped)* —
  remap these otherwise-unused buttons to any of `key_a key_b key_x key_y
  key_l key_r`. The physical button then behaves exactly as if the named
  button were pressed, glyph included.

### Mods

Drop ChronoMod-compatible `.ctp` patch archives (standard ZIP files, entry
paths matching `resources.bin` internal paths) into the mods folder — `mods/`
by default, configurable via `mods_dir` in `config.ini` — and they're applied
to a patched, in-memory copy of `resources.bin` at startup. **Your original
`resources.bin` is never modified.**

You can also use "folder mods": a subfolder inside the mods folder containing
loose files at their `resources.bin`-relative path (e.g.
`mods/MyMod/Game/common/foo.dat`) is treated the same way. Folder mods are
applied after `.ctp` files, so they take priority if both touch the same file.

The first time a mod set is applied it's cached to disk (`mods/.modcache`), so
subsequent boots load instantly instead of rebuilding — you'll only see the
brief "Caching mods…" splash after adding, removing, or changing a mod.

**Included tool — `tools/generate_text_ctp.py`:** the field-message text ct_nx
loads from your `resources.bin` was written for a touchscreen ("Touch blue,
yellow, then red…"), in every shipped language. This script reads your own,
legally-extracted `resources.bin`, rewrites those lines to reference actual
Switch buttons using the same tags the controller-glyphs patch already
understands, and packages the result into a `.ctp` for your mods folder:

```
python3 tools/generate_text_ctp.py /path/to/resources.bin
python3 tools/generate_text_ctp.py /path/to/resources.bin --lang en,ja
```

Because the fix is generated from your own file, none of Square Enix's text is
redistributed — only the substitution logic in the script is ours.

### Advanced: offline binary patch tools

`tools/` also includes the standalone Python patchers the boot-time
`config.ini` patches above are based on (`remove_bilinear.py`,
`remove_cursor_invert.py`, `remove_mobile_ui.py`, `remove_menu_buttons.py`).
They edit a `libchrono.so` on disk directly and are kept mainly for reference
and for anyone who wants a pre-patched `.so` outside of ct_nx's own runtime
patching — for normal use, the equivalent `config.ini` flags are simpler,
reversible, and require no re-patching. Each script is self-contained
(Python 3 standard library only) and supports `--dry-run`/`--in-place`/`-o`;
run one with `-h` for details.

### How to build

You're going to need devkitA64 and the following devkitPro packages:
* `switch-mesa`
* `switch-libdrm_nouveau`
* `switch-sdl2`
* `switch-freetype`
* `switch-libpng`
* `switch-harfbuzz`
* `switch-ffmpeg` (+ its codec deps `switch-dav1d`, `switch-libopus`,
  `switch-libvorbisidec`, `switch-libwebp`, `switch-libogg`)

### Credits

* fgsfds for [max_nx](https://github.com/fgsfdsfgs/max_nx), which this loader is
  based on
* TheOfficialFloW for the original Vita ports that pioneered this technique
* [ppkantorski](https://github.com/ppkantorski/) for the `config.ini` revision,
  mod support, font loading, all of the graphical and UI fixes, controller remapping,
  save/mods/config directory reorganization, and the tools.

### Support

If you enjoy my work and want to support me :

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)

### Legal

This project has no direct affiliation with Square Enix. "Chrono Trigger" is a
trademark of its respective owner. All Rights Reserved.

No assets or program code from the original game or its Android port are included
in this project. We do not condone piracy in any way, shape or form and encourage
users to legally own the original game.

Unless specified otherwise, the source code provided in this repository is
licensed under the [MIT License](/LICENSE).

The included font is licensed under [CC BY-NC-SA 3.0](https://github.com/ppkantorski/ct_nx/blob/main/font/ChronoType/license.txt) and is not covered by this project's license.
