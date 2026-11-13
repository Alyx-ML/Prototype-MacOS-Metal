# Prototype for Mac (Apple silicon)

Play the Steam version of Prototype (2009) natively on a Mac with Apple
silicon. The game runs as native ARM64 code with Metal graphics. It does not
use Rosetta, Wine or an emulator.

This project holds no game files and no game code. You need your own copy of
the game from Steam.

## Requirements

- A Mac with Apple silicon.
- The installed game folder of the Steam version of Prototype, copied to
  your Mac.
- 8 GB of free disk space for the first start.

## Install and play

1. Download `Prototype-Mac-ARM64.zip` from the Releases page and open it.
2. Move `Prototype.app` to your Applications folder.
3. Open `Prototype.app`. If macOS does not open it, open System Settings >
   Privacy & Security and click "Open Anyway".
4. In the Prototype window, click "Choose Folder…" and select your game
   folder (the folder that holds `prototypef.exe`). A green mark shows that
   the folder can be used.
5. Change the graphics and controller settings if you want to.
6. Click Play.

On the first start, the application prepares the game for your Mac. This
takes several minutes and is done one time. Later starts go directly to the
game.

The window opens each time you start the application. It remembers your game
folder and your settings. To quit without playing, close the window.

## Supported game files

Only the Steam version is supported. The application checks these two files
and tells you if they are different:

| File | SHA-256 |
| --- | --- |
| `prototypef.exe` | `ac1542512ab5eddffcbf949919f3dcc4b26e38f07b57cb6a094def2a621b0b82` |
| `prototypeenginef.dll` | `d331be205bdde99897952ee88261ab7e63c5c65e36a4660e2a367886a3537071` |

Use the original Steam files. Mods (for example an ASI loader in
`binkw32.dll`) are not necessary.

## Settings

You can change these settings in the Prototype window, and in the Video and
Controller screens of the game's options menu.

| Setting | Values | What it does |
| --- | --- | --- |
| HDR | Off, On | Brighter highlights on an HDR display. |
| Shadow Resolution | Game, 2x, 4x | Sharper shadows. Applies at the next start of the game. |
| MetalFX Upscaling | Off, On | Scales a lower game resolution up to the window. |
| Sharpening | Off, Low, Medium, High | Makes textures look crisper. |
| Anti-Aliasing (SMAA) | Off, 1x, T2x | Smooths jagged edges. |
| Field of View | Game, +5° to +30° | Shows more of the city around the camera. |
| Ambient Occlusion | Off, Low, Medium, High | Darker corners and contact areas. |
| Soft Shadows | Off, Low, High | Shadow edges get softer with distance. |
| Contact Shadows | Off, On | Small shadows below objects and at feet. |
| Bloom | Game, Kawase | A smoother glow around lights. |
| Tone Mapping | Game, GT7 | Softer highlights and truer colors. |
| Button Icons | Game, Controller | PlayStation or Xbox buttons for your controller. |
| Adaptive Triggers | Off, On | Light trigger resistance on a DualSense. |
| Enhanced Rumble | Off, On | Richer vibration on supported controllers. |

## Where your files are

Your settings, saved games and the prepared game code are in
`~/Library/Application Support/Prototype Mac Port/`. Your game folder is not
changed.

## Build from source

This is only necessary if you want to make the release package yourself.

You need Xcode or the Xcode Command Line Tools, the Homebrew packages `llvm`,
`lld` and `ldc` (`brew install llvm lld ldc`), Python 3, an FFmpeg source
archive from <https://ffmpeg.org/download.html>, and the two supported game
files.

1. Build the recompiler tools:

   ```bash
   sh tools/build_toolchain.sh
   ```

2. Build the FFmpeg libraries for the game videos:

   ```bash
   sh tools/build_ffmpeg_lgpl.sh ~/Downloads/ffmpeg-9.0.2.tar.xz
   ```

3. Copy `prototypef.exe` into `work/launcher/` and `prototypeenginef.dll`
   into `work/engine/`.
4. Translate the game modules:

   ```bash
   sh tools/translate_launcher.sh
   ```

   ```bash
   sh tools/translate_engine.sh
   ```

5. Make the release package:

   ```bash
   FFMPEG_PREFIX="$PWD/build/ffmpeg-lgpl" sh tools/package_release.sh
   ```

   The result is `build/release/Prototype-Mac-ARM64.zip`. It holds no game
   code: the application converts the game files of each player on their
   Mac.

> **WARNING:** The files in `build/` and the `.llasm`, `.llinc`, `.ll`,
> `.bc` and `.o` files in `work/` hold the translated code of the game. Do
> not upload, share or commit them. The `.gitignore` file excludes them.

## Licenses

- The code of this project: MIT license (see `LICENSE`).
- The SR static recompiler by M-HT (<https://github.com/M-HT/SR>): MIT
  license (see `third_party/SR/README.md`).
- `runtime/src/rt_d3d9_ao.m` is a port of XeGTAO, Copyright (C) 2016-2021
  Intel Corporation, MIT license. The license text is in that file.
- `runtime/src/rt_d3d9_sss.m` is a port of the screen-space shadows of Bend
  Studio, Copyright 2023 Sony Interactive Entertainment, Apache License 2.0.
  That file has the license notice and a list of the changes.
- The GT7 tone mapping in `runtime/src/rt_d3d9_draw.m` is a port of the
  sample code of Polyphony Digital, Copyright (c) 2025 Polyphony Digital
  Inc., MIT license. The license text is in that file.
- The SMAA code in `runtime/src/rt_d3d9_post.m` is a port of the SMAA
  reference implementation, Copyright (C) 2013 Jorge Jimenez, Jose I.
  Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez, MIT license.
  The license text is in that file.
- The release package includes LLVM, lld, Z3, zstd and an LGPL build of
  FFmpeg. Their license files are in `Prototype.app/Contents/Resources/Licenses`.
- Prototype is a trademark of its owners. This project has no connection to
  the publisher or the developer of the game.
