<div align=center>

<img src="extras/banner.png" alt="Banner" width="40%">

</div>
<h1 align=center>Chrono Trigger — Nintendo Switch port</h1>

</div>

This is a wrapper/port of the Android version of *Chrono Trigger*
(`com.square_enix.android_googleplay.chrono`, v2.1.5). It loads the original game
binaries, patches them and runs them: we natively run the original Android `.so`
files inside a minimal emulated Android environment.

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
5. Copy **`ct_nx.nro`** into `/switch/ct/`.

### Notes

This will not work in applet/album mode. Use a game override (hold R on a title)
or a forwarder, so the homebrew gets the full memory and required syscalls.

Save data, the cocos2d-x `Cocos2dxPrefsFile.txt` settings store and the port's
`config.txt` are kept in `/switch/ct/`.

### Configuration

`config.txt` is created on first run:
* `screen_width` / `screen_height` — render resolution; `-1` picks 1280x720
  handheld and 1920x1080 docked.
* `language` — selects the in-game text/assets: one of `en fr de it es ja ko zh`.
  Anything other than `ja` uses the localized (`-en`) data. Defaults to English.

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

The included font is licensed under [CC BY-NC-SA 3.0]() and is not covered by this project's license.
