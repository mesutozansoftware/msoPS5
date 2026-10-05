# msoPS5

[![Build msoPS5 (macOS Apple Silicon)](https://img.shields.io/github/check-runs/mesutozansoftware/msoPS5/main?nameFilter=Build%20msoPS5%20%28macOS%20Apple%20Silicon%29&label=Build%20msoPS5)](https://github.com/mesutozansoftware/msoPS5/actions/workflows/build.yml)
[![Platform](https://img.shields.io/badge/platform-macOS%20Apple%20Silicon-0078D4.svg)](#system-requirements)
[![Status](https://img.shields.io/badge/status-active%20development-orange.svg)](#current-status)
[![License](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](LICENSE)

msoPS5 is a free and open-source PlayStation 5 emulator for Macs with Apple silicon, written in
C++. It is a fork of [KytyPS5](https://github.com/KytyPS5/KytyPS5), which is itself based on a
heavily modified version of [Kyty](https://github.com/InoriRus/Kyty). The project is in active
development, and behavior can change significantly between builds.

> [!IMPORTANT]
> msoPS5 is not affiliated with Sony Interactive Entertainment or PlayStation, nor with the
> KytyPS5 project. The project does not distribute games or copyrighted system software. Use
> only game files that you have obtained legally.

## Current Status

msoPS5 can boot 2D games and a selection of 3D games, including titles built with Unreal Engine
4/5, Unity, and custom engines. External low-level emulation modules are neither required nor
planned.

Development is currently focused on expanding game compatibility and improving boot reliability.

msoPS5 runs only on Macs with Apple silicon (M1 or later). PS5 games are x86-64 programs that
the emulator executes directly, so msoPS5 itself is an x86-64 app that runs under Rosetta 2, with
Vulkan provided by MoltenVK. Intel Macs, Windows and Linux are not supported.

msoPS5 shares its emulation core with KytyPS5, so the community-maintained
[KytyPS5 Compatibility List](https://kytyps5.github.io/) is a good guide to which games work.

## Bugs and Issues

Compatibility, stability, and performance can vary between versions. You may encounter crashes
or graphical glitches, so please include the version you tested when reporting an issue.

## Screenshots

<table align="center">
  <tr>
    <td align="center">
      <strong>Astro Bot</strong><br>
      <img src="docs/screenshots/ps5-01.png" width="300" alt="Astro Bot running in KytyPS5">
    </td>
    <td align="center">
      <strong>Dreaming Sarah</strong><br>
      <img src="docs/screenshots/ps5-03.png" width="300" alt="Dreaming Sarah running in KytyPS5">
    </td>
  </tr>
  <tr>
    <td align="center">
      <strong>Neptunia ReVerse</strong><br>
      <img src="docs/screenshots/ps5-04.png" width="300" alt="Neptunia ReVerse running in KytyPS5">
    </td>
    <td align="center">
      <strong>SILENT HILL: The Short Message</strong><br>
      <img src="docs/screenshots/ps5-05.png" width="300" alt="SILENT HILL: The Short Message running in KytyPS5">
    </td>
  </tr>
  <tr>
    <td align="center">
      <strong>Demon's Souls</strong><br>
      <img src="docs/screenshots/ps5-02.png" width="300" alt="Demon's Souls running in KytyPS5">
    </td>
    <td align="center">
      <strong>UFC 6</strong><br>
      <img src="docs/screenshots/ps5-06.jpg" width="300" alt="UFC 6 running in KytyPS5">
    </td>
  </tr>
</table>

<p align="center"><em>And many more...</em></p>

## Contributing

Testing games and submitting detailed bug reports are useful ways to contribute. Search existing
issues first, then use the **Game Emulation Status Report** template and attach the complete log file.

Code contributions should be focused, build successfully on macOS, and include relevant tests
where practical. Because msoPS5 is still evolving quickly, consider opening an issue before
starting a large change.

### Formatting

Set up the clang-format hook after cloning:

Install `pre-commit` (`brew install pre-commit` or `python3 -m pip install pre-commit`), then
install the Git hook:

```bash
python -m pre_commit install --install-hooks
```

It formats staged `.cpp`, `.h`, and `.inc` files in `src`.

## Developer Information

The PS5 graphics architecture is based on AMD RDNA 2. Use AMD's
[RDNA 2 Instruction Set Architecture Reference Guide (document 70648)](https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture)
as the primary instruction-encoding reference when working on shader decoding and recompilation.

Important areas of the codebase:

- [`src/graphics/shader/recompiler`](src/graphics/shader/recompiler) — instruction decoding,
  intermediate representation, control flow, resource tracking, and SPIR-V emission
- [`src/graphics/guest_gpu`](src/graphics/guest_gpu) — PS5 (Prospero) GPU formats and command processing
- [`src/graphics/host_gpu`](src/graphics/host_gpu) — Vulkan host backend and resource management
- [`tests`](tests) — focused memory, shader, and resource-tracking regression tests

The renderer targets Vulkan 1.3. Keep shader changes aligned with both the RDNA 2 ISA semantics and
the Vulkan/SPIR-V validation rules.

## Installing

Download the latest `msoPS5-…-macOS-AppleSilicon.dmg` from the
[releases page](https://github.com/mesutozansoftware/msoPS5/releases), open it, and drag
**msoPS5** onto **Applications**. If Rosetta 2 is not installed yet, macOS offers to install it
the first time msoPS5 starts (or run `softwareupdate --install-rosetta`).

msoPS5 is not notarized by Apple, so the first launch is blocked by Gatekeeper. Either
right-click msoPS5 in Applications, choose **Open** and confirm, or remove the quarantine flag:

```bash
xattr -dr com.apple.quarantine /Applications/msoPS5.app
```

## Building

### System requirements

- A Mac with Apple silicon (M1 or later) running macOS 13 or newer
- Rosetta 2 (`softwareupdate --install-rosetta`)
- Vulkan is provided by the bundled MoltenVK

### Building from source

msoPS5 is always built for x86-64 so that the PS5's x86-64 game code runs through Rosetta 2
together with the emulator; CMake selects `x86_64` automatically and rejects other architectures.

Requirements:

- Xcode (or the Command Line Tools)
- Homebrew packages: `brew install cmake ninja glslang`
- Qt 6 (Concurrent, Network, Widgets) with x86-64 support. The official Qt installation is
  universal and works; Homebrew's Qt is arm64-only and will not link

```bash
git submodule update --init --recursive

cmake -S . -B _Build/macos -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH="$Qt6_DIR"

cmake --build _Build/macos --target launcher --parallel
cmake --install _Build/macos --prefix _Build/macos/install

# Bundle MoltenVK (Vulkan) into the install and the app, then build the disk image.
packaging/macos/bundle-moltenvk.sh _Build/macos/install
packaging/macos/make-dmg.sh _Build/macos/install/msoPS5.app _Build/msoPS5.dmg
```

The build re-signs `kyty_emulator` with the JIT entitlements it needs to execute translated
guest code; no manual signing step is required. The install produces
`_Build/macos/install/msoPS5.app`, and a flat `kyty_emulator` is kept next to it for CLI usage.
FFmpeg is linked statically from the pinned
[KytyPS5 FFmpeg core](https://github.com/KytyPS5/ext-ffmpeg-core) release.

### Regression tests

Build every regression executable and run the registered tests with:

```bash
cmake --build _Build/macos --target kyty_tests
ctest --test-dir _Build/macos --output-on-failure
```

## Running

To use the graphical launcher, open msoPS5 from Applications (or
`open _Build/macos/install/msoPS5.app` for a local build).

On first launch, add one or more game folders in the global settings. The launcher searches those
folders recursively for game directories containing `eboot.bin` and ZArchive (`.zar`) game dumps
whose archive root contains `eboot.bin`. Select a detected game and run it from the game list.
ZArchive dumps are mounted read-only and streamed directly; they do not need to be extracted first.

### Installing game packages

The **Install Package** button (download icon) in the game list toolbar unpacks a legally obtained
game package into one of your game folders, after which it shows up in the list. Supported inputs:

| Format | Notes |
| --- | --- |
| `.pkg` | Fake/debug PS5 packages (fPKG) with the default all-zero passcode, uncompressed or zlib-compressed |
| `.ffpfs`, `.ffpfsc` | PFS game images, optionally compressed, encrypted with the default key, or wrapping an exFAT image |
| `.exfat` | exFAT game images |

Not supported yet: retail packages (they are encrypted with console keys and cannot be
installed), packages that use Kraken compression or the network-install ("data-first") layout,
and UFS images (`.ffpkg`). The installer reports these cases instead of producing a broken game.

The emulator can also be started directly with a legally obtained game directory, ELF file, or
ZArchive dump. The adjacent flat or app-bundled `libMoltenVK.dylib` is found automatically:

```bash
/Applications/msoPS5.app/Contents/MacOS/kyty_emulator --game "/games/ExampleGame"
/Applications/msoPS5.app/Contents/MacOS/kyty_emulator --game "/games/ExampleGame.zar"
```

To override the Vulkan loader, set `SDL_VULKAN_LIBRARY`:

```bash
SDL_VULKAN_LIBRARY=/path/to/libMoltenVK.dylib ./kyty_emulator --game "/games/ExampleGame"
```

Run `kyty_emulator --help` to see the available graphics, logging, validation, profiling, and
debugging options.

### AI Use

AI tools may be used for research, reverse engineering, and development assistance. Contributors
must fully understand, review, and test all code they submit and remain responsible for its
correctness. Repository communication, including pull-request descriptions, code comments, and
issue comments, must come from the human contributor rather than an autonomous AI agent.

Pull requests that include AI-assisted or AI-generated work should disclose the scope of the AI
involvement and describe the human review and testing performed before submission. Unverified or
untested generated changes may be closed without review.

## License

msoPS5 is licensed under the [GNU General Public License version 2](LICENSE)
(`GPL-2.0-only`).

msoPS5 is a fork of [KytyPS5](https://github.com/KytyPS5/KytyPS5), which is licensed under the
same terms. KytyPS5 is based on the original [Kyty](https://github.com/InoriRus/Kyty), which was released
under the MIT License. Kyty's original copyright and license notice are preserved in
[`LICENSES/Kyty-MIT.txt`](LICENSES/Kyty-MIT.txt). Third-party components remain subject to the
licenses included with those components.

## Special Thanks

- [KytyPS5/KytyPS5](https://github.com/KytyPS5/KytyPS5) — msoPS5 is a fork of KytyPS5, and its
  emulation core comes from that project.
- [InoriRus/Kyty](https://github.com/InoriRus/Kyty) — KytyPS5 is based on a heavily modified version
  of the original Kyty project.
- [shadps4-emu/shadPS4](https://github.com/shadps4-emu/shadPS4) — reference for understanding PS4
  memory behavior, GPU resource aliasing and cache coherency,
  and the AVPlayer implementation.
