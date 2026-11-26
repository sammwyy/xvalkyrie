<div align="center">

<img src="docs/img/logo.svg" alt="XValkyrie" width="380">

# XValkyrie

**Xbox One and Xbox Series X|S software on Windows, by translation.**

[![build](https://github.com/sammwyy/xvalkyrie/actions/workflows/build.yml/badge.svg)](https://github.com/sammwyy/xvalkyrie/actions/workflows/build.yml)
[![license](https://img.shields.io/badge/license-GPL--3.0-ec3630.svg)](LICENSE)
[![language](https://img.shields.io/badge/C%2B%2B-20-ec3630.svg)](CMakeLists.txt)
[![platform](https://img.shields.io/badge/platform-Windows%20x64-ec3630.svg)](#building)
[![discord](https://img.shields.io/badge/discord-join-5865F2.svg?logo=discord&logoColor=white)](https://discord.gg/pTehpajhXF)

[Compatibility](#compatibility) · [Status](#status) · [How it works](#how-it-works) · [Building](#building) · [Usage](#usage) · [Roadmap](ROADMAP.md)

</div>

XValkyrie runs Xbox One and Xbox Series X|S software on 64-bit Windows. It is written in
C++20.

It is a translation layer, not an emulator. Both consoles are x86-64 machines that run a
cut-down Windows, so there is no CPU to emulate and no system to boot: a title's executable is
mapped into a process, its code runs natively, and the console libraries it imports
(`d3d12_xs.dll`, `xgameruntime.dll`, `kernelx.dll` and the rest) are answered by
implementations written here on top of the host's Direct3D, audio and input. The closest
relatives are Wine and Proton, not console emulators.

What has to be translated is what differs from a PC:

- the console builds of Direct3D 11 and 12, whose objects a title reads and writes directly;
- shaders, which Series titles ship as machine code for the console's GPU instead of
  portable bytecode;
- memory, where a title owns physical pages and places resources on them itself;
- the system services: users, saved games, the Gaming Runtime, WinRT on Xbox One.

## Compatibility

How far titles get when they are started. Every title that was tried, its state and what
the states mean are in [COMPATIBILITY.md](COMPATIBILITY.md).

**Xbox One**

<table>
  <tr>
    <th>UNDERTALE</th>
    <th>DRAGON BALL FighterZ</th>
    <th>Terraria</th>
    <th>Puyo Puyo Champions</th>
    <th>Rayman Legends</th>
  </tr>
  <tr>
    <td><img src="docs/img/compat/undertale.webp" alt="UNDERTALE" width="170"></td>
    <td><img src="docs/img/compat/dragon-ball-fighterz.webp" alt="DRAGON BALL FighterZ" width="170"></td>
    <td><img src="docs/img/compat/terraria.webp" alt="Terraria" width="170"></td>
    <td><img src="docs/img/compat/puyo-puyo-champions.webp" alt="Puyo Puyo Champions" width="170"></td>
    <td><img src="docs/img/compat/rayman-legends.webp" alt="Rayman Legends" width="170"></td>
  </tr>
  <tr>
    <td align="center">Playable</td>
    <td align="center">Menu</td>
    <td align="center">Playable</td>
    <td align="center">Menu</td>
    <td align="center">Menu</td>
  </tr>
</table>

[+36 titles](COMPATIBILITY.md#xbox-one)

**Xbox Series X|S**

<table>
  <tr>
    <th>Fortnite</th>
    <th>Unicorn Overlord</th>
    <th>Minecraft Preview</th>
    <th>Psychonauts 2</th>
    <th>"The Game"</th>
  </tr>
  <tr>
    <td><img src="docs/img/compat/fortnite.webp" alt="Fortnite" width="170"></td>
    <td><img src="docs/img/compat/unicorn-overlord.webp" alt="Unicorn Overlord" width="170"></td>
    <td><img src="docs/img/compat/minecraft.webp" alt="Minecraft Preview" width="170"></td>
    <td><img src="docs/img/compat/psychonauts-2.webp" alt="Psychonauts 2" width="170"></td>
    <td><img src="docs/img/compat/the-game.webp" alt="&quot;The Game&quot;" width="170"></td>
  </tr>
  <tr>
    <td align="center">Menu</td>
    <td align="center">Playable</td>
    <td align="center">Playable</td>
    <td align="center">Intro</td>
    <td align="center">Menu</td>
  </tr>
</table>

[+3 titles](COMPATIBILITY.md#xbox-series-xs)

49 titles tried: **8** playable · **12** menu · **3** intro · **19** boots · **7** nothing

## Status

The project is experimental. What it implements of each console, counted from the code:

<!-- status:begin -->
[![status map](docs/img/status.svg)](docs/STATUS.md)

Every function and instruction behind these numbers is listed in [docs/STATUS.md](docs/STATUS.md).
<!-- status:end -->

The block above is generated: `python tools/status.py` after a build. The
[roadmap](ROADMAP.md) has what is planned, and [docs/research](docs/research) what was found
out on the way, with the evidence for it.

## How it works

1. **Inspect.** The executable is parsed without being loaded. Its import table tells the
   console: `d3d12_x.dll` and `kernelx.dll` mean Xbox One, `d3d12_xs.dll` means Series, the
   stock Direct3D DLLs mean a PC build, which is refused.
2. **Map.** The image is mapped and relocated, and every import is bound through one
   resolver: to an implementation of this project, to the title's own DLL, to the console
   system DLL the title shipped with when that is pure computation, to the host's function
   when it is one console titles may call, and otherwise to a stub that names itself when
   called.
3. **Run.** The entry point runs on a thread of its own, inside a process of its own
   (`xvboot`), with the window, the graphics device and the input coming from the host.
4. **Translate.** Direct3D calls go to the host's Direct3D 11 or 12 through objects with the
   console's layout. Console shaders are translated to HLSL once and kept in a cache.

[ARCHITECTURE.md](ARCHITECTURE.md) is the tour of the code: what each directory is for, how
the layers depend on each other and where new code goes.

## Building

Windows 10 or 11, 64-bit, with Visual Studio 2022 ("Desktop development with C++"),
CMake 3.25 or later and Python 3. CMake fetches the dependencies on the first configure.

The shaders made for Series titles are compiled with `dxcompiler.dll` when it and `dxil.dll`
are next to `xvalkyrie.exe` or in a Windows SDK, and with the system's older compiler
otherwise, which takes minutes over the largest of them.

```bat
build.cmd --test
```

This leaves `xvalkyrie.exe` and `xvboot.exe` in `build\win\Release`.

| Argument        | Effect                                                                                  |
| --------------- | --------------------------------------------------------------------------------------- |
| `Debug`         | A debug build instead of Release                                                        |
| `--test`        | Runs CTest after building                                                               |
| `--vulkan`      | Also builds Direct3D 11 on Vulkan (needs [Meson](https://mesonbuild.com) and `glslang`) |
| `--target NAME` | Builds one target                                                                       |
| `--reconfigure` | Runs CMake's configure step again                                                       |

`JOBS` in the environment sets the number of parallel compilations.

## Usage

`xvalkyrie` is the window: the game library, the options, and the log of the title that is
running. It starts `xvboot` for each title, which is also the command line.

```
xvalkyrie [FILE|FOLDER]              open the window, and a title if one is given
xvboot exec TITLE [--allow-missing] [--trace] [--mute] [--backend d3d11|vulkan]
                  [--no-config] [--profile NAME] [--console MODEL] [--language TAG]
                  [--fullscreen MODE] [--set SECTION.KEY=VALUE]
```

A title is an executable or the folder of an unpacked title (the one with
`MicrosoftGame.config` or `AppxManifest.xml`). Packages (`.xvc`) are listed but cannot be
opened: XValkyrie decrypts nothing.

`exec` runs the title's code in the `xvboot` process with no sandbox. Only run what you
trust.

`exec` takes the settings of the window (`config.ini` and the profile). The other options
change them for that run alone, and nothing of it is saved:

| Option                    | Effect                                                                                                   |
| ------------------------- | -------------------------------------------------------------------------------------------------------- |
| `--no-config`             | None of the saved settings is read: every one is its default                                             |
| `--profile NAME`          | The profile `NAME`, as saved                                                                             |
| `--console MODEL`         | The console the title is told it runs on: `series_x`, `series_s`, `xbox_one_x`, `xbox_one_s`, `xbox_one` |
| `--language TAG`          | The console's language and region, such as `es-AR`                                                       |
| `--fullscreen MODE`       | What F11 does: `borderless` or `native`                                                                  |
| `--set SECTION.KEY=VALUE` | Any setting by its name in `config.ini` (`console.headset=true`), or of the profile (`profile.gamertag`) |

The commands that look at a title without running it print JSON:

| Command                                   | Prints                                                                                               |
| ----------------------------------------- | ---------------------------------------------------------------------------------------------------- |
| `xvboot inspect FILE`                     | PE headers and directories                                                                           |
| `xvboot assess FILE`                      | The imports by kind, and the console the build is for                                                |
| `xvboot bind FILE`                        | Where each import would be bound                                                                     |
| `xvboot map FILE [--base HEX]`            | The layout of the mapped image                                                                       |
| `xvboot properties FILE`                  | All of the above in one report                                                                       |
| `xvboot inspect-config FILE`              | A `MicrosoftGame.config`                                                                             |
| `xvboot inspect-manifest FILE`            | An `AppxManifest.xml`                                                                                |
| `xvboot inspect-package FILE`             | The header of an XVD/XVC package                                                                     |
| `xvboot modules durango\|scarlett`        | The console modules and functions implemented                                                        |
| `xvboot shader-probe [--compile]`         | For each line of machine code words on the input, what stops its translation                         |
| `xvboot shader-corpus FOLDER [--compile]` | How many of the shaders a run left in a folder (`XV_SHADER_CODE`) translate, and what stops the rest |
| `xvboot shader-source FILE`               | The HLSL made for one shader of such a folder                                                        |

In the window, Options > Input binds the gamepad to keys or to the host's gamepad,
Options > Graphics picks the backend and what F11 does, Options > Console sets the signed-in
user, the console model titles are told they run on (Series X or S, Xbox One X, One S or
One, for each generation) and, optionally, the system storage: a folder with the console's own apps, each
unpacked in a folder of its own. With one, apps find the packages they depend on there and
File > Boot from Menu starts the dashboard, as far as it gets (see the roadmap). Options >
Game Libraries sets the folders that are scanned for titles. Settings are kept
in `%APPDATA%\xvalkyrie\config.ini`, logs in `logs` next to it, one file per session and per
title.

Development switches are environment variables, described where they are used in
[docs/research](docs/research): `XV_D3D_TRACE`, `XV_D3D_DEBUG`, `XV_D3D_PROBE`,
`XV_SHADER_WHY`, `XV_SHADER_CODE`, `XV_D3D_DRED` and others.

## Tools

| Script                                 | What it writes                                                                           |
| -------------------------------------- | ---------------------------------------------------------------------------------------- |
| `tools/status.py`                      | The status block above, `docs/STATUS.md` and `docs/img/status.svg`                       |
| `tools/rdna_ops.py`                    | The instruction table of the shader translator, from LLVM's disassembler                 |
| `tools/import_xbdepends.py`            | The lists of system DLLs and APIs a console title may use                                |
| `tools/compat_images.py check\|fixall` | Nothing, or the screenshots of the table above made alike: 640x360 WebP, 100 KiB at most |

## FAQ

**Is this an emulator?**
No. Nothing is emulated at the level of the hardware. A title's code runs on the host's CPU
as it is, and the console's libraries are reimplemented.

**Can I play games with it?**
[COMPATIBILITY.md](COMPATIBILITY.md) says how far each title that was tried gets.

**Does it run on Linux?**
No. It relies on Windows for the part of the console's system that is Windows.

**Where do I get games or keys?**
Not here. XValkyrie includes no games, keys, firmware or SDK files, links to none, and cannot
decrypt retail packages.

## Contributing

Issues and pull requests are welcome; [CONTRIBUTING.md](CONTRIBUTING.md) has the ground
rules. Say where information comes from: guessed offsets, ordinals or ABIs are not merged.

## Acknowledgements

The method order of some console WinRT classes (`Windows.Kinect`,
`Windows.Xbox.Management.Deployment.PackageTransferManager`, `Windows.Xbox.Input.Controller`)
was checked against the interface descriptions of
[WinDurango](https://github.com/WinDurango/WinDurango). No code of it is used here.
The consoles' own DXGI formats go by the names
[DirectXTex](https://github.com/microsoft/DirectXTex) (MIT) gives them in `DirectXTexP.h`. No
code of it is used here.

## Legal

XValkyrie is not affiliated with or endorsed by Microsoft. Xbox, Xbox One and Xbox Series X|S
are trademarks of Microsoft Corporation. The project is written from public documentation,
public source code and the observed behaviour of software its authors own.

## License

Copyright (C) 2026 Sammwy.

XValkyrie is free software under the [GNU General Public License, version 3](LICENSE): you may
use, change and share it, and what you build from it has to stay under the same license and
keep its notices. It comes with no warranty. Third-party components keep their own licenses,
listed in [THIRD-PARTY.md](THIRD-PARTY.md).
