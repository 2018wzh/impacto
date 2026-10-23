# impacto [![impacto](https://github.com/CommitteeOfZero/impacto/actions/workflows/impacto.yml/badge.svg)](https://github.com/CommitteeOfZero/impacto/actions/workflows/impacto.yml)

**impacto** is an open-source **reimplementation** of the **"MAGES." visual novel engine** in C++ and OpenGL. Using the original data files, impacto can run any supported game on any supported platform.

## Status

**impacto is in active development**. At current stage it should be possible to reach all endings in at least one supported game - "Memories Off 6 \~T-Wave\~." However, some functionality and architectural design are still missing and **no games are fully complete yet**. Refer to [#1](https://github.com/CommitteeOfZero/impacto/issues/1) for supported games and remaining work.

impacto is currently being developed for **64-bit Windows 10 and desktop Linux PCs** but we've successfully experimented on macOS, Android (ARM), Switch (homebrew and Linux) and HTML5/WebAssembly (via Emscripten) and full official Android support is planned. OpenGL is used for the graphics renderer with experimental Vulkan and DirectX 9 options available.

## Building

For building on Windows with Visual Studio 2019 or newer, please refer to the [building instructions](doc/windows_build.md).

For building on Linux, see the [instructions for Ubuntu Desktop](doc/ubuntu_build.md) and adapt to your distribution if necessary.

For building for the Nintendo Switch (Horizon / devkitPro), see the [Switch build instructions](doc/switch_build.md).

More platforms and toolchains are known to work.

## Contributing

**We are looking for contributors!** Check out the [Getting Started guide](doc/getting_started.md) for pointers on setting up games for testing, finding your way around the codebase and adding functionality. Also check out the [Contributor guide](doc/contributor_guide.md) for important information on the code style you should follow.

There is work to be done for C++ programmers of any skill level in a wide range of subjects, on game engine architecture (design and implementation), reverse-engineering the original (looking at just the game's outside behaviour as a *black box* or inspecting the internals through a *white box*), replicating it, improving on it with new functionality, documenting our efforts, fixing bugs and polish. 

If you're interested, [come join our Discord](https://discord.gg/rq4GGCh) to discuss ideas and help you get into it.

## Legal stuff

impacto source code as a whole is released under the liberal [ISC license](LICENSE). Some parts are based on or copied from third-party code under various licenses. Binary distributions or parts thereof may fall under more restrictive licensing terms. See [THIRDPARTY.md](THIRDPARTY.md) for details and attribution.

## AstraEMU fork

This fork adds `IMPACTO_ASTRA_EMU` on native repair baseline
`f12b4b799f1024eaca9a4e48e0371c2f3835576e`, derived from upstream
`914fae7cbb9583f8aa231569a03aab27d8c042f8`. The baseline isolates DDS
decoder state for concurrent image loads and fixes ownership of unallocated
movie-audio planes and their pointer array. The shared core exports the current
AstraEMU C ABI v3 directly: plugin `impacto`, engine family `mages`.
`vendor/astra-emu-abi/include/astra_emu.h` is an unchanged byte copy of the
cbindgen-generated header from AstraEngine
`6d9d691dcc8f3d768e40b5be1d6f37366cca62d8`,
`Emulator/Source/FamilyApi/astra-emu-abi/include/astra_emu.h`.
It is licensed `MIT OR Apache-2.0`; see `THIRDPARTY.md` for attribution and
license texts. CMake uses this fixed copy by default. An explicit
`ASTRA_EMU_ABI_INCLUDE_DIR` override supports local integration; no second
interface or generated-header edits are maintained.

The core worker owns the original SC3 VM, native menus, OpenGL renderer,
OpenAL audio and FFmpeg players. It renders into a hidden hardware GL context,
reads the final 1920x1080 backbuffer and publishes immutable RGBA CPU leases.
Manager performs the visible presentation. No host advance loop, second visible
window, null audio backend, generated black frame or Steam API replacement is
used. Input goes through the native SDL queue. Closing joins the worker and
media threads, tears down native resources and resets session state; CPU leases
retain only CPU storage and can be released after close.
The four CPU frame slots are allocated during open; acquiring or releasing a
lease only changes the frame's intrusive reference count. Host suspension is
independent of explicit pause: either freezes VM updates and native audio/movie
clocks and playback, while focus loss alone retains the native focus policy.

The adapter selects `profiles/cc-steam/game.lua`, which includes the original
CC profile and describes the Steam font's two native DDS strips. Native
spritesheet `AppendPath` decodes both strips directly into one RGBA allocation;
no font pixels are extracted into the core package or copied into the source.
Single-sheet font `CellSize` separates glyph spacing from atlas tail padding.
Profiles without that field keep their original grid-derived spacing.
Native title groups own their button widgets; session teardown does not delete
those buttons again before deleting their groups.

### Windows Release build

Use the upstream `vcpkg.json` baseline and `vcpkg-configuration.json` overlay.
The manifest includes SDL3 3.4.0, OpenAL Soft 1.25.1, FFmpeg 7.1.2, avcpp,
Ogg/Vorbis/Opus, WebP, zlib, fmt, GLM, pugixml, utfcpp, concurrentqueue,
readerwriterqueue, unordered-dense, boost-circular-buffer, toml11, FreeType,
HarfBuzz and libass. CMake additionally fetches LibAtrac9 and magic_enum.
Desktop OpenGL 3.3 and a working native audio device are required. ImGui,
Vulkan and DirectX9 are excluded from this target.

Open an **x64 Native Tools Command Prompt for Visual Studio**, set
`VCPKG_ROOT` to a vcpkg checkout, and run from this fork's root. Use a short
build directory; CMake installs the pinned manifest dependencies before building.

```bat
set BUILD_ROOT=C:/b/impacto-astra
cmake -S . -B "%BUILD_ROOT%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DIMPACTO_ASTRA_EMU=ON -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake"
cmake --build "%BUILD_ROOT%" --target impacto
cmake --install "%BUILD_ROOT%" --prefix "%BUILD_ROOT%/package"
```

The output is `astra_emu_impacto.dll`. Keep its native dependency DLLs beside
it. `cmake --install` creates `Resources/profiles`, `Resources/resources` and
`Resources/src/shaders` beside the DLL. On Windows an empty `resources_root`
resolves only to that dynamic core's `Resources` directory. Source-tree
acceptance passes this checkout explicitly as `resources_root`; no development
checkout is compiled into the runtime. Static or non-Windows hosts must provide
an explicit resource root. ISC `LICENSE` and the upstream `THIRDPARTY.md`
remain part of the distribution.

### Typed configuration and acceptance

* `profile`: enum, currently exactly `cc`.
* `assets_root`: directory containing Steam CC's MPK files; an empty value uses
  `game_path/USRDIR`, or `game_path` when it already names USRDIR.
* `resources_root`: directory containing `profiles/cc`, `resources/cc` and
  `src/shaders/opengl`; empty selects the installed Windows core's `Resources`.
* `save_root`: required isolated writable directory, outside game assets and
  core resources. It supplies native save, configuration and base paths.
* `use_backup_archives`: boolean, defaults to true. This explicitly requires
  `script_bak.mpk` and `system_bak.mpk`; false selects the installed MPKs.
  Backup filenames alone do not establish patch compatibility.
* `movie_order`: optional path to a verified native CLS file, one filename per
  movie ID starting at zero. Blank lines retain their IDs. The supplied bytes
  are mounted in memory against `USRDIR/movie/1920x1080`; no sidecar is written
  beside the game. Without a mapping, a movie request fails explicitly. The
  adapter never guesses IDs from a directory sort.

Unknown/duplicate fields, mismatched types and unsupported render extents are
rejected. Neither game assets nor pre-existing user saves/configuration are
modified. The unofficial patch executables are not run.

For an ABI driver, configure `profile=cc`, the source checkout as
`resources_root`, the real Steam `USRDIR` as `assets_root`, and a private
directory as `save_root`. Native bindings are Enter/Space for confirm,
arrow keys for selection, Backspace/KeyX for cancel, Digit1 or secondary mouse
button for the native Start/menu action, and ControlLeft for held skip.
Use real frames to decide when to confirm New Game; blind timed key presses
are not evidence of a reached title or advancing story.

The upstream CC profile is partial. It does not implement CC save capture,
restore or a save format (`SaveDataType.None`). The adapter preserves the
native title controls; requesting unsupported save/load raises an explicit
native error that is reported through the ABI failed state.
The generic save functions only forward to a game-specific implementation;
the CCLCC snapshot implementation is not a CC save backend. The native CC
system-menu profile is also unfinished. A real Steam sample contains Bink 2
movies, which require a decoder beyond FFmpeg's available Bink support.
These are native-engine acceptance blockers, not completed features.
The build continues to use open-source dependencies. It does not load a Bink
runtime from this or another game, and it does not substitute a no-op movie.
On 2026-10-23 the integrating host built the Release DLL and exercised native
startup frames, same-DLL close/reopen, cancellation and retained CPU leases.
The reached native screen reports a system-data loading failure, so those
diagnostics are not evidence of playable story progression. The user then
explicitly skipped CHAOS;CHILD acceptance; no gameplay pass is claimed.

