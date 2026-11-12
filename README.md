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
| Video | Anisotropic Filtering | Game, 16x | 16x |
| Video | HDR | Off, On | Off |
| Video | Shadow Resolution (restart) | Game, 2x, 4x | Game |
| Video | MetalFX Upscaling | Off, On | Off |
| Video | Sharpening (CAS) | Off, Low, Medium, High | Off |
| Video | SMAA Anti-Aliasing | Off, On | Off |
| Controller | Button Icons | Game, Controller | Controller |
| Controller | Adaptive Triggers (DualSense) | Off, On | On |
| Controller | Enhanced Rumble | Off, On | On |

- Anisotropic Filtering sets 16x filtering on each texture that has mipmaps
  and linear filtering.
- HDR shows the game on an EDR display (extended dynamic range). Bright
  areas use the headroom of the display. A display without headroom shows
  the image as before.
- Shadow Resolution makes the shadow maps 2 or 4 times larger in each
  direction. The new value applies at the next start of the game.
- MetalFX Upscaling scales the image to the size of the window with the
  MetalFX spatial scaler. It has an effect only when the game resolution is
  smaller than the window.
- Sharpening is contrast-adaptive sharpening (CAS).
- SMAA is the SMAA 1x method with the "low" quality steps: edge detection
  on luma, orthogonal search of 32 pixels and blending. It has no diagonal
  or corner detection.
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
- The release package includes LLVM, lld, Z3, zstd and an LGPL build of
  FFmpeg. `tools/package_release.sh` copies their license files into
  `Contents/Resources/Licenses`.
- Prototype is a trademark of its owners. This project has no connection to
  the publisher or the developer of the game.
