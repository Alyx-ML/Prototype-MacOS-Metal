# Prototype for Mac (Apple silicon)

This project is a static recompilation of the Windows game Prototype (2009,
Steam version) for macOS on Apple silicon. The game runs as native ARM64 code.
Rendering uses Metal. The project does not use Rosetta, Wine or an emulator.

This repository holds no game files and no game code. You must have your own
legal copy of the game. The application converts your game files on your Mac.

## What the repository holds

| Path | Contents |
| --- | --- |
| `runtime/` | The Windows API runtime in C and Objective-C: kernel32, user32, the msvcr80 C runtime, COM, Direct3D 9 on Metal, XAudio2 on Core Audio, DirectInput, XInput, Bink video on FFmpeg. |
| `runtime/loader/` | The application executable and the converter. |
| `tools/` | Scripts that translate, compile, build and package the game. |
| `tests/` | Unit tests for the runtime. |
| `work/*/*.sci`, `work/*/SR.cfg`, `work/launcher/relocations.csv` | Analysis files for the static recompiler: code addresses and flag values. These files hold no code bytes. |
| `third_party/SR/` | The SR static recompiler by M-HT (<https://github.com/M-HT/SR>, MIT license), commit `ac690ddf3010bc3d2c875cb1b5e8da4f53a8d5b3`, with local changes. |
| `PORTING_LOG.md` | The dated log of the port. |

## Supported game files

The application accepts only these files from the Steam version:

| File | SHA-256 |
| --- | --- |
| `prototypef.exe` | `ac1542512ab5eddffcbf949919f3dcc4b26e38f07b57cb6a094def2a621b0b82` |
| `prototypeenginef.dll` | `d331be205bdde99897952ee88261ab7e63c5c65e36a4660e2a367886a3537071` |

The application refuses other versions with an error message. The converter
output is tested only for these files.

To get the SHA-256 value of a file, run this command in Terminal:

```bash
shasum -a 256 prototypeenginef.dll
```

Use the original Steam files. Mod files (for example an ASI loader in
`binkw32.dll`) are not necessary. The runtime supplies its own Bink decoder.

## How the conversion works

1. The application finds the game folder. It looks in these locations in
   this sequence:
   1. The folder given with `--game-dir <path>`.
   2. The folder `Put game files here` next to the application.
   3. The folder that holds the application.
   4. The folder that you selected before.
2. If no location holds the game files, the application shows a window to
   select the folder. Hold the Option key at start to select a different
   folder.
3. The application checks the SHA-256 values of the two game files.
4. On the first start, the converter translates the game code. The SRW
   recompiler makes llasm code, llasm makes LLVM IR, and clang compiles the
   IR to ARM64. The result goes into
   `~/Library/Application Support/Prototype Mac Port/GameCode/`.
   This occurs one time. It can take several minutes, and it needs 8 GB of
   free disk space for temporary files.
5. The game starts. Later starts use the converted code.

## Build from source

### Prerequisites

- A Mac with Apple silicon.
- Xcode or the Xcode Command Line Tools.
- Homebrew packages `llvm`, `lld` and `ldc`:

  ```bash
  brew install llvm lld ldc
  ```

- Python 3 (the system `python3` is sufficient).
- An FFmpeg source archive (for example `ffmpeg-9.0.2.tar.xz` from
  <https://ffmpeg.org/download.html>).
- Your own copy of the two supported game files.

The scripts expect the Homebrew LLVM in `/opt/homebrew/opt/llvm` and lld in
`/opt/homebrew/opt/lld`.

### Procedure

1. Build the recompiler tools SRW and llasm:

   ```bash
   sh tools/build_toolchain.sh
   ```

2. Build the LGPL FFmpeg libraries (only the Bink decoders):

   ```bash
   sh tools/build_ffmpeg_lgpl.sh ~/Downloads/ffmpeg-9.0.2.tar.xz
   ```

3. Copy `prototypef.exe` into `work/launcher/`.
4. Copy `prototypeenginef.dll` into `work/engine/`.
5. Translate the launcher:

   ```bash
   sh tools/translate_launcher.sh
   ```

6. Translate the engine:

   ```bash
   sh tools/translate_engine.sh
   ```

7. Make the release package:

   ```bash
   FFMPEG_PREFIX="$PWD/build/ffmpeg-lgpl" sh tools/package_release.sh
   ```

   The result is `build/release/Prototype-Mac-ARM64.zip`. This package holds
   no game code. It holds the loader, the runtime and the converter. The
   script stops with an error if the package holds a file or a symbol of the
   translated game code.

To make a development application that holds the translated game code, do
steps 1 to 6, then run these commands:

```bash
sh tools/compile_engine.sh
```

```bash
sh tools/build_app.sh
```

The result is `build/app/Prototype.app`.

> **WARNING:** The development application, the files in `build/`, and the
> `.llasm`, `.llinc`, `.ll`, `.bc` and `.o` files in `work/` hold the
> translated code of the game. If you share these files, you distribute the
> game code. Do not upload, share or commit these files. The `.gitignore`
> file of this repository excludes them.

### Run the development application

```bash
build/app/Prototype.app/Contents/MacOS/Prototype --game-dir "/path/to/Prototype"
```

The game folder must hold the complete installed game (the data files and the
two supported files).

### Run the unit tests

```bash
sh tools/run_tests.sh
```

Some tests use Metal or open a local network port. Run them outside a sandbox.

## Mac enhancements

The port adds rows to the options screens of the game. The rows are in the
main menu and in the pause menu. The runtime adds the rows when the game
reads `art.rcf`. The file on disk does not change.

| Screen | Row | Values | Default |
| --- | --- | --- | --- |
| Video | HDR | Off, On | Off |
| Video | Shadow Resolution (restart) | Game, 2x, 4x | Game |
| Video | MetalFX Upscaling | Off, On | Off |
| Video | Sharpening (CAS) | Off, Low, Medium, High | Off |
| Video | SMAA Anti-Aliasing | Off, 1x, T2x | Off |
| Video | Field of View | Game, +5 to +30 (degrees, steps of 5) | Game |
| Video | Ambient Occlusion (XeGTAO) | Off, Low, Medium, High | Off |
| Video | Soft Shadows (PCSS) | Off, Low, High | Off |
| Video | Contact Shadows | Off, On | Off |
| Video | Bloom | Game, Kawase | Game |
| Video | Tone Mapping | Game, GT7 | Game |
| Controller | Button Icons | Game, Controller | Controller |
| Controller | Adaptive Triggers (DualSense) | Off, On | On |
| Controller | Enhanced Rumble | Off, On | On |

- Anisotropic filtering is always 16x (no menu row). It applies to each
  texture that has mipmaps and linear filtering.
- HDR shows the game on an EDR display (extended dynamic range). Bright
  areas use the headroom of the display. A display without headroom shows
  the image as before.
- Shadow Resolution makes the shadow maps 2 or 4 times larger in each
  direction. The new value applies at the next start of the game. The
  shadow pass does not save its unused color target or the stencil of the
  shadow map, so the larger maps use less memory bandwidth.
- MetalFX Upscaling scales the image to the size of the window with the
  MetalFX spatial scaler. It has an effect only when the game resolution is
  smaller than the window.
- Sharpening is contrast-adaptive sharpening (CAS).
- SMAA is the SMAA 1x method with the settings of the "ultra" preset:
  edge detection on color with depth predication, diagonal detection,
  orthogonal search of 32 pixels and diagonal search of 16 pixels, corner
  detection and the blending of the SMAA reference. Depth predication finds more edges where the
  depth changes and fewer edges in textures. Diagonal detection makes
  sloped edges smoother.
- SMAA T2x adds a temporal step to SMAA 1x. The scene moves by a quarter
  pixel in alternate frames. After the scene and before the HUD, the
  SMAA passes use the area values for the offset of the frame. Then the
  image is mixed with the last frame at the position that the camera
  motion gives. The mix decreases where the motion changes, and the last
  frame is limited to the colors of the 3 x 3 pixels around each pixel,
  so moving objects have less ghosting. A frame without a 3D scene (for
  example a menu) gets SMAA 1x.
- Field of View adds degrees to the field of view of the game camera.
- Ambient Occlusion is the XeGTAO method of Intel. It makes corners and
  contact areas darker. Low, Medium and High use 1, 2 and 3 directions for
  each pixel. The multi-bounce approximation of GTAO keeps light surfaces
  from going grey in corners.
- Soft Shadows is percentage-closer soft shadows (PCSS). A shadow edge
  gets softer as the distance from the object that casts the shadow
  increases.
- Contact Shadows is the screen-space shadow method of Bend Studio. It
  adds short sun shadows from the depth buffer where the shadow map has
  too little detail, for example below small objects and at the feet of
  characters. The shadows fade out between 40 and 80 meters from the
  camera.
- Bloom Kawase replaces the blur of the game's bloom with a dual Kawase
  blur (five half-size passes down and five passes up). The first pass
  uses the 13-tap filter with the Karis average, so small bright points
  do not flicker. The up passes add the levels together, so the glow
  falls off gradually around a light. The game's bloom strength stays the
  same.
- Tone Mapping GT7 applies the tone mapping of Gran Turismo 7 to the scene
  before the HUD: a soft shoulder for bright areas and colors that keep
  their hue near white. The HUD and the menus keep their colors. The
  highlights are expanded before the curve, so white stays at 98 percent.
  With HDR on, the tone mapping applies after the expansion to the EDR
  headroom, with the peak of the curve at the headroom. The back buffer is
  then a 16-bit float image, so the scene keeps its highlights and the HUD
  stays at SDR white. Menus without the 3D scene get the expansion and the
  curve at the end of the frame.
- Button Icons shows the PlayStation or the Xbox button icons for the
  controller that has the first player slot.
- Adaptive Triggers gives light resistance on L2 and R2 of a DualSense
  controller. While a motor runs, the trigger on the same side vibrates.
- Enhanced Rumble gives the left motor a soft haptic and the right motor a
  sharp haptic, and makes weak vibration stronger.

The runtime keeps these values in
`~/Library/Application Support/Prototype Mac Port/enhancements.txt`.

The menu patch supports only the movies of the Steam release. If
`art.rcf` has different movies, the game stops with a message that names
the movie.

## Settings and saved games

The game writes its settings, saved games and registry values into
`~/Library/Application Support/Prototype Mac Port/User/`.

## Licenses

- The code of this port (`runtime/`, `tools/`, `tests/`): MIT license (see
  `LICENSE`).
- The SR recompiler: MIT license (see `third_party/SR/README.md`).
- `runtime/src/rt_d3d9_ao.m` is a port of XeGTAO, Copyright (C) 2016-2021
  Intel Corporation, MIT license. The license text is in that file.
- `runtime/src/rt_d3d9_sss.m` is a port of the screen-space shadows of Bend
  Studio, Copyright 2023 Sony Interactive Entertainment, Apache License
  2.0. That file has the license notice and a list of the changes.
- The GT7 tone mapping in `runtime/src/rt_d3d9_draw.m` (built-in shader
  library) is a port of the sample code of Polyphony Digital, Copyright (c) 2025
  Polyphony Digital Inc., MIT license. The license text is in that file.
- The SMAA diagonal detection in `runtime/src/rt_d3d9_post.m` is a port of
  the SMAA reference implementation (SMAA.hlsl and AreaTex.py), Copyright
  (C) 2013 Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro
  and Diego Gutierrez, MIT license. The license text is in that file.
- The release package includes LLVM, lld, Z3, zstd and an LGPL build of
  FFmpeg. `tools/package_release.sh` copies their license files into
  `Contents/Resources/Licenses`.
- Prototype is a trademark of its owners. This project has no connection to
  the publisher or the developer of the game.
