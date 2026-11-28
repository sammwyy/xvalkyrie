# Architecture

Quick tour of the code. Everything lives under `src/`, one static library per
directory, each with its own `CMakeLists.txt`. There are two executables.
`xvboot` runs both Xbox One and Xbox Series X|S titles: it looks at the import
table of the title and picks the console runtime to use. `xvalkyrie` is the
window, which starts `xvboot` for each title (see "Processes").

Two words are used with one meaning each. The **host** is the machine xvalkyrie
runs on, always 64-bit Windows. A **console** is what the title was built for:
Xbox One (`durango`) or Xbox Series X|S (`scarlett`).

```
src/
  core/            the Target enum (durango, scarlett), log, settings
  host/            OS wrappers: memory, files, threads, native modules, session log
  formats/         pe, gdk (MicrosoftGame.config), appx (AppxManifest.xml), xvd
  analysis/        import classification, console target detection
  loader/          image mapping
  backends/        what a title's output and input end up on, per host technology
    graphics/      backend selection; d3d11/, dxvk/ (Direct3D 11 on Vulkan), vulkan/
    audio/         wasapi/
    input/         the emulated gamepad; xinput/
    surface/       the window a title presents into; win32/
  runtime/         the console side: what a title's imports are bound to
    common/        import registry and resolver, guest execution
    modules/       console modules both consoles have
    durango/       Xbox One only
    scarlett/      Xbox Series X|S only
    console.*      picks durango or scarlett for a title
  launcher/        whether a title can start, game library, saved configuration
  app/             the two executables
    cli/           the commands that look at a title without running it
    bootstrap/     xvboot: starts a title, in a process and a window of its own
    gui/           xvalkyrie: the window
  tools/           developer executables
```

### Layers

A directory only includes from the ones above it in that list, with these
rules on top:

- `backends/` knows nothing about consoles. No `Target`, no console DLL names,
  no console object layouts.
- `runtime/` is the only place that knows what a console DLL looks like. It
  reaches the host through `host/` and `backends/`, never the other way round.
  The exceptions are the files that are the host side of running a guest:
  `runtime/common/{execute,com_forward,static_tls}`, `runtime/durango/winrt/` and
  `runtime/durango/d3d11x/` (see "Host objects" below).
- `runtime/durango` and `runtime/scarlett` do not include each other. What
  both need goes to `runtime/modules`.
- `launcher/` has no window and prints nothing. `app` uses `launcher`;
  `launcher` never includes `app`.
- `app/gui` never includes `app/bootstrap` or `app/cli`, nor the other way round:
  the two executables only meet at the command line of `xvboot`.
- Code in `backends/` sits in a subdirectory named after the technology it uses
  (`win32/`, `wasapi/`, `xinput/`, `d3d11/`, `dxvk/`, `vulkan/`).

### Shared

Used for every title, whatever console it was built for.

| Directory | Library | What it does |
|---|---|---|
| `core` | `xv_core` | `Target`, log and settings |
| `host` | `xv_host` | OS wrappers: memory, files, threads, TLS, native modules, session log |
| `formats/pe` | `xv_pe` | PE32+ parser. Takes bytes, returns a JSON report. Never loads anything. |
| `formats/gdk` | `xv_game_config` | Reads `MicrosoftGame.config` |
| `formats/appx` | `xv_appx` | Reads `AppxManifest.xml`, which names the executable of an unpacked ERA title |
| `formats/xvd` | `xv_xvd` | Reads the header of an XVD/XVC package |
| `analysis` | `xv_analysis` | Classifies imported DLLs and API names, detects the console target |
| `loader` | `xv_loader` | Maps an image in memory: sections, relocations, import slots |
| `backends/graphics` | `xv_graphics` | Which backend is selected, and the clear pass description |
| `backends/graphics/d3d11` | `xv_graphics_d3d11` | The host's Direct3D 11: device creation, frame dump |
| `backends/graphics/dxvk` | `xv_graphics_dxvk` | Direct3D 11 on Vulkan: loads `xv_dxgi.dll` and `xv_d3d11.dll` and creates the device |
| `backends/graphics/vulkan` | `xv_vulkan` | The offscreen Vulkan test backend |
| `backends/audio` | `xv_audio` | Sound output; so far only muting the process |
| `backends/input` | `xv_input` | The emulated gamepad: capture methods, key bindings |
| `backends/surface` | `xv_surface` | `TitleWindow`, the window a title presents into |
| `runtime/common` | `xv_runtime_common` | Import registry and resolver, and the code that actually runs a guest |
| `runtime/modules` | `xv_runtime_modules` | Console modules with the same interface on both consoles |

### Per console

| Directory | Library | What it does |
|---|---|---|
| `runtime/durango` | `xv_runtime_durango` | Xbox One modules (`d3d12_x.dll` and friends) |
| `runtime/scarlett` | `xv_runtime_scarlett` | Xbox Series X\|S modules (`d3d12_xs.dll` and friends) |

Each one has a single entry point, `install(Registry &)`, that adds its
modules to the import registry. The Series one only installs shared modules under their
Series names.

The two consoles differ less than the two directories suggest. There are
three kinds of console module:

- ERA / XDK only, which is Xbox One only: `kernelx.dll`, `d3d11_x.dll`, the
  WinRT classes, `etwplus.dll`. These live in `runtime/durango`.
- The same interface on both consoles under two names (`xg_x.dll` /
  `xg_xs.dll`, later `d3d12_x.dll` / `d3d12_xs.dll` and the Gaming Runtime).
  These live in `runtime/modules`, with an `install_*(Registry &, name)` that
  each console calls with the name the DLL has there. Where the two consoles
  differ inside such a module, it takes the `Target`.
- Series only: `runtime/scarlett`.

`runtime/modules` so far:

- `xg.cpp`: `xg_x.dll` and `xg_xs.dll`, which tell titles how a texture is laid out in
  memory.
- `kernel.cpp`: the Win32 functions a console answers differently (libraries of ours and
  of the title's, the console's drives, debug output, thread affinity, the registry value
  that names the console). ERA titles get them from `kernelx.dll`; for GDK titles they are
  registered with `Registry::add_api`, which answers a function whatever API set it is
  imported from.
- `memory.cpp`: console memory, for `kernelx.dll` and `xmem.dll`. `VirtualAlloc` with large or 4 MiB pages
  reserves placeholders, and the title's physical pages
  (`AllocateTitlePhysicalPages`, `MapTitlePhysicalPages`) are 64 KiB views of
  one 16 GiB section committed as it is used. Also XMem and its
  hooks, `D3DAllocateGraphicsMemory` and the `Apu*` memory of `acphal.dll`.
- `window.cpp`: a GDK title's own windows stay hidden; its frames go to the title window.
- `d3d12x/`: `d3d12_x.dll` and `d3d12_xs.dll` on the host's Direct3D 12 (`device.cpp`),
  and `PresentX` with the frame events (`present.cpp`).
- `placement.cpp`: resources on memory the title owns.
- `gameruntime/`: `xgameruntime.dll`, the system side of the GDK's Gaming Runtime.
- `gameinput.cpp`, `system.cpp` (`xaudio2_9.dll` by ordinal, the shader compiler),
  `storage.cpp` (saved games in files).

`runtime/durango` so far:

- `kernelx.cpp`: `kernelx.dll` is forwarded to the host's `kernel32.dll`, except
  for the functions that only exist on the console.
- `system.cpp`: `mmdevapi.dll`, `mfplat.dll` and `toolhelpx.dll` go to the host,
  plus the console-only functions of those and of `pixevt.dll`. Functions whose
  arguments nobody has documented only ever return an HRESULT.
- `winrt/system_ui.cpp`: the system keyboard (`ShowVirtualKeyboardAsync` and
  `KeyboardOptions`), answered by a window of the host
  (`backends/surface/win32/prompt.cpp`) on a thread of its own, and what
  Options > Console says about the user: gamertag, user ID, gamerscore, age
  group, and a headset that is the host's communications output.
- `winrt/activation.cpp`: ERA titles are C++/CX applications that reach the
  system through WinRT classes, all of them looked up by name through one
  function of `vccorlib110.dll`. That function is ours, and so is combase's
  `RoGetActivationFactory`, which WRL code in the title's DLLs uses instead. It
  answers with our own class, with the title's (below), with the host's for a
  few namespaces that work in any process (`Windows.Foundation`,
  `Windows.Storage.Streams`...), or with a placeholder. The title's classes
  are the in-process servers of its `AppxManifest.xml` (Xbox Live in
  `Microsoft.Xbox.Services.dll`, game chat...): the DLL is loaded the first
  time one of its classes is asked for, and its `DllGetActivationFactory`
  makes the factory. Those DLLs then reach the console classes the same way
  the executable does.
- `winrt/placeholder.cpp`: an object that answers every `QueryInterface`. A
  call to one of its methods prints the class, interface, method number and
  caller, and fails. That is how the next missing piece is found.
- `winrt/inert.cpp`: table of methods of console classes whose feature is
  absent (no store, no party, no achievements, no microphone, no network, no
  game DVR, no speech recognizer): accept the
  arguments, return zero, hand out a token for an event that is never raised.
  Method numbers and interface IDs come from the title's call sites and from
  public interface descriptions of the console's WinRT classes.
- `winrt/storage.cpp`: connected storage (`Windows.Xbox.Storage`), the way
  ERA titles save their games. The spaces, containers and container queries
  are ours and keep everything in files through `modules/storage.cpp` (`SaveSpace`):
  `profiles/<profile>/saves/<package name>/{machine,user}/<container>/<blob>` in the per-user
  configuration folder, with every name escaped into a portable file name.
  `launcher` picks the folder and hands it over in
  `ExecuteOptions::save_directory`. Only the methods seen in titles are
  implemented; the layout of `ContainerInfo2` is a guess.
- `winrt/application.cpp`: real classes, written with WRL against the public
  Windows SDK headers: `CoreApplication`, `CoreApplicationView`, `CoreWindow`
  and `CoreDispatcher`. `CoreApplication::Run` drives the title's view on the
  calling thread, and the window is a `TitleWindow` from `backends/surface`.

### Executables

| Directory | Target | What it does |
|---|---|---|
| `runtime` | `xv_runtime` | `console_registry(Target)`: the registry of one console |
| `launcher` | `xv_launcher` | `launch_title`, the game library, `config.ini` |
| `app/cli` | `xv_cli` | `inspect`, `assess`, `map`, `bind`...: JSON on stdout, nothing runs |
| `app/bootstrap` | `xvboot` | `exec`: from a path to a running title (`bootstrap::run`), and the CLI |
| `app/gui` | `xv_gui`, `xvalkyrie` | GLFW + ImGui main window, native file dialog |

### Processes

`xvalkyrie` is a window program and never runs a title. For each title it starts
`xvboot exec <title>`, the executable next to it (`host::run_beside`), waits for
its exit code on a thread of its own, and can end it (File > Close App). The
child is in a job that ends it if the window goes first. Closing the title's
window ends the title too: a console has no such event, so `xvboot` ends itself
two seconds later if the title has not left by then. What `xvboot` and the
title print comes back through a pipe: the window shows it while the title runs
and prints it itself, so it is in the window's log and console too. The two
share nothing but that command line, that pipe and `config.ini`, which `xvboot`
reads when it starts: no window, no graphics device and no input state
is in both. `xvboot` links none of the interface, so what it loads, where its
image sits and how it is built can follow what titles need.

`bootstrap::run` is the one way a title starts, from the window or from a
person's `xvboot exec`: it picks the saved games folder, the shader cache and
the console settings, and gives the title window its caption,
`<title> | <video driver> | <n> FPS | XValkyrie <version> <commit>`. The caption
is asked for again every second on the window's thread
(`surface::set_title_caption`); the driver is what the device was created on
(`graphics::drawing_with`), and the frame rate comes from
`graphics::frames_presented`.

F11 in the title window is full screen, handled by `backends/surface`. Borderless
is the window's own doing: no frame, the size of its display. Native
(`[graphics] fullscreen = native`, Options > Graphics) needs whoever owns the
swap chain: it offers with `surface::offer_exclusive_fullscreen`, looks at
`exclusive_fullscreen_wanted` at every frame on the thread that presents, and
calls `SetFullscreenState` and `ResizeBuffers` to match, so the window's thread
and the presenting one never wait for each other. Only `d3d12x/present.cpp`
offers, since it owns its chain's buffers; a title that holds its own back
buffers (Direct3D 11.X) cannot have them resized under it and gets borderless,
as does a host that refuses.

### Where new code goes

- A new way to produce pictures, sound or input on a host (Vulkan, Direct3D
  12, another audio API): `backends/<kind>/<technology>/`.
- Works the same on both consoles and is not a console DLL (a translation
  layer, file access): a shared library next to `loader`, or under `backends/`
  if it is about the host.
- A console DLL only one console has: that console's runtime.
- A console DLL both have: `runtime/modules`, installed by each console.
- Console runtimes do not include each other.

### Host objects

Titles are Windows programs, and most of what they import is handed to the
real system DLLs. These give the title host objects:

- `runtime/common/execute.cpp`, `static_tls.cpp` and `com_forward.cpp`: running
  the guest, and the COM forwarder.
- `runtime/durango/winrt/` and `runtime/durango/d3d11x/`: they give the title
  host COM objects, wrapped or as they are.

The line through Direct3D is drawn at the API, not below it: `d3d11x` turns
console Direct3D 11.X calls into desktop Direct3D 11 calls on an
`ID3D11Device`, and `backends/graphics/d3d11` or `backends/graphics/dxvk` is
where that device comes from. Another backend is therefore something that
provides Direct3D 11 (or 12) interfaces, not a second copy of `d3d11x`.

### The window

`app/gui` is Dear ImGui, in four layers that only use the ones before them:

- `theme`: fonts, colors, the pixel-art wordmark and the backdrop. The only
  place with color values and font sizes.
- `widgets`: stateless pieces built on the theme (badges, key hints, the
  segmented control, the rows of a settings page, `animate`).
- `settings`: the settings window, a list of pages and one function per page.
  A new page is an entry in `pages` and a function. It asks its owner for
  anything outside itself through `Settings::Host`.
- `desktop`: the window itself: what is loaded or running, and the library,
  loading and running screens. The running one is the log of the title's process,
  which stays after the title ends, closed by a line that says how it did.
- `properties`: the Properties window of a title, which draws what
  `title_properties` of the launcher reports (`xvboot properties` prints it).

The library is scanned on a worker thread. `discover_library` keeps what it
found in `cache/library.json` in the configuration folder, with the size and
time of each title's manifest and executable: a title whose files did not
change is not read again. The window shows `cached_library` (the last scan,
without reading anything) the moment it opens, and replaces it when the new
scan is done. Library pictures are decoded off the window thread too.

A manifest says what each picture is for, not what it looks like, so the files
decide (`pictures` in `launcher/library.cpp`): of the logos the visual elements
name, the cover is the largest that is about square and the icon the smallest of
at least 32 pixels, by their PNG headers. The store's logo is used only by a
title with nothing else. `xvboot` gives the icon to the title's window
(`surface::set_title_icon`).

## Target detection

`analysis::import_profile` classifies every imported DLL with the tables of
Microsoft's `xbdepends` tool. `d3d12_x.dll`, `dxcompiler_x.dll`, `xg_x.dll`
and the ERA modules `kernelx.dll`, `d3d11_x.dll` and `etwplus.dll` mean Xbox
One (`Target::durango`). The last two are not in the `xbdepends` tables, which
only cover the GDK.
`d3d12_xs.dll`, `dxcompiler_xs.dll` and `xg_xs.dll` mean Series
(`Target::scarlett`). Stock Direct3D DLLs mean a PC build and are refused, as
are images that mix targets. An image with none of them has an unknown target
and gets no console modules.

## Parsing

The PE parser works on a byte span and checks every range before reading it.
RVAs are only resolved to bytes that exist in the file, so a table that points
into a zero-filled tail or into alignment padding is an error. There are limits
on the number of records and string sizes to keep reports bounded.

Parsing and running are separate on purpose. The inspection commands can be
pointed at any file without risk of executing it.

## Loading

`loader::MappedImage` reserves `SizeOfImage` bytes, copies headers and sections
to their RVAs, applies DIR64 relocations and fills the import address table by
calling a resolver for each `(module, symbol)` pair.

By default the image is rebased to wherever the host put it, which is what
running it needs. You can also ask for a fixed guest base (`map --base`), in
which case the result is only good for looking at.

Pages are read-only or read-write according to the section flags. Code
sections become executable only when the runtime asks for it, and a section
that wants to be writable and executable at the same time is rejected.

## Resolving imports

Every import goes through one place (`runtime/common`). The order is:

1. `Registry`: our own implementations, keyed by DLL name and symbol, filled
   by the runtime of the title's console.
2. Runtime overrides, see below.
3. The real Windows export with the same name. This is only allowed for CRT
   DLLs and for system DLLs when the function name is in the list of APIs that
   console titles can use. Ordinals, console DLLs and unknown DLLs are never
   forwarded. DLLs are loaded by name from System32 only.
4. A small generated stub. If the game calls it, the process prints the name
   of the import and exits with `0xE0585301`.

`xvboot bind` prints which of these each import would get.

## Threads

Nothing a title does runs on the thread that handles a window, and the other
way round:

- in `xvalkyrie`, the window thread runs the interface, a worker thread reads
  and checks a title when it is opened, and another waits for the `xvboot`
  that runs it;
- in `xvboot`, the guest thread runs the title, and the title's window gets a
  thread of its own for its messages and its caption.

So dragging or resizing a window does not stop the title, and a title that is
busy or stuck does not freeze the window. `CoreDispatcher::ProcessEvents` has
nothing to pump for that reason.

## Input

`src/backends/input` is the one place a console runtime asks what the gamepad reads
(`input::poll()`), and it does not depend on the window or on the runtime.
Where the reading comes from is the capture method:

- `window`: the title's window reports its key events, and
  the bindings turn the keys that are down into buttons, triggers and stick
  directions.
- `direct`: the state of the keyboard and of the host's first gamepad
  (XInput) is read from the system at the moment the title asks, without any
  window or message loop involved. It only counts while a window
  of the title's process is in the foreground.

Bindings are Windows virtual-key codes. Options > Input in the
window changes the method and the bindings, which are saved by
`launcher/config` to `config.ini` in the per-user configuration folder and
read by `xvboot` for the next title.
`runtime/durango/winrt/gamepad.cpp` is the console side: one
`Windows.Xbox.Input.Gamepad`, always connected, whose raw reading is
`input::poll()`.

## Title DLLs

A DLL that sits next to the executable and is neither ours nor forwarded is
the title's own (`xcompress.dll` in the test program). It is mapped like the
executable, its imports go through the same resolver, its exception table is
registered and its entry point is called before the title's. A title DLL that
nothing imports, like the WinRT servers of the manifest, is mapped while the
title runs (`detail::title_export`) and its `DllMain` is called then. C++
exceptions, `GetModuleHandleEx` by address and `GetModuleFileName` know
about every title DLL, not only the executable.

## Running

The guest runs inside the xvboot process on its own thread. Since guest and
host use the same calling convention, a bound import is just a
function pointer.

Running in-process means a few things have to be faked:

- `GetModuleHandle(NULL)` and `GetModuleFileName` return the guest image
  instead of `xvboot.exe`.
- `_CxxThrowException` is reimplemented. The CRT looks up the module that owns
  a thrown type in the loader's module list, and a manually mapped image is
  not in it, so `catch` would never match.
- The image's unwind table is registered with `RtlAddFunctionTable`.

- The command line (`GetCommandLine`, `__p___argv`, `Platform::GetCmdArguments`)
  is the title's path alone, not xvboot's.
- Static TLS: the system loader only gives TLS to what it loads itself, so
  `runtime/common/static_tls.cpp` writes a small DLL with the image's
  TLS template (and one import, without which the loader skips its TLS) and
  loads it; the image's `_tls_index` is set to the index that DLL got.
- A title's own DLLs are looked for from the executable's folder up to its
  package root, the folder with the `AppxManifest.xml`.

Things that do not work yet: the lazy `__delayLoadHelper` path. Delay-load
import slots are bound at map time like regular imports, so a delayed import
gets its address when the image is mapped, not on first use. The title has a
process of its own, away from the window, but no isolation from the host of
any kind.

## Graphics

`backends/graphics/backend` names the two backends a console Direct3D module can sit on
and holds the one selected for the process (Options > Graphics, saved in
`config.ini`; `exec --backend` overrides it):

- `d3d11`, the host's Direct3D 11. The default.
- `vulkan`, Direct3D 11 translated to Vulkan by DXVK. `backends/graphics/dxvk`
  loads `xv_dxgi.dll` and `xv_d3d11.dll` from the folder of `xvboot.exe`,
  takes the first adapter of the DXVK factory and creates the device on it, so
  DXVK never goes looking for the system's `dxgi.dll`. The DLLs are DXVK's
  `dxgi.dll` and `d3d11.dll` renamed, so they never clash with the system ones
  already in the process.

`cmake/Dxvk.cmake` (`-DXV_DXVK=ON`) fetches DXVK at a fixed commit, applies
`cmake/dxvk.patch`, builds it with Meson and copies the two DLLs next to
`xvboot.exe`. The patch makes DXVK behave the way console titles expect:

- `SetResourceMinLOD` and `GetResourceMinLOD` work for textures created with
  `D3D11_RESOURCE_MISC_RESOURCE_CLAMP`, through `VK_EXT_image_view_min_lod`.
- A compute shader may have the same resource bound as a shader resource and as
  an unordered access view.
- A stencil-only shader resource view has the stencil value in red, not green.

`xv_vulkan` is a separate standalone test that clears an offscreen image and
reads it back.

`runtime/durango/d3d11x` is `d3d11_x.dll`. Its `D3D11CreateDevice` creates the
device on the selected backend and wraps it; the rest of `d3d11x` does not know
which one it is.

Direct3D 11.X interfaces have the desktop interface IDs but not the desktop
layout, so the title never gets a host object:

- `wrap.cpp`: every host object is given to the title inside an object of ours
  with the console layout. One generic entry per method slot looks the method
  up in the layout of the object's kind, replaces the arguments that are our
  objects by the host's, calls the host method, and wraps what comes back.
  The same host object always gets the same wrapper.
- `layout.cpp`: the knowledge. For each kind (device, context, swap chain,
  resource, view, state...) where every method is, which host method it is,
  and which arguments are objects. Methods with float arguments, or whose
  console signature differs from the desktop one, are written out by hand
  there. Positions that are still guesses are marked as such.
- The device context is special: titles call most of its methods through
  function pointers stored inside the object, not through the vtable, so the
  context object carries that table too.
- `device.cpp`: `D3D11CreateDevice` and the way to the swap chain. The host
  device itself is created by `backends/graphics/d3d11`. The DXGI
  device, adapter and factory are the same as on the desktop and go through
  a `Forwarder` (`runtime/common/com_forward`), a COM object that
  sends every method to the host except the ones it is told to answer;
  the only one of ours is `CreateSwapChainForCoreWindow`, which makes the
  swap chain on the `TitleWindow`.

- State without calls: the console headers inline some setters. The title
  writes the input layout, the vertex and pixel shaders and the topology (as a
  hardware register value) into fixed offsets of the context object and sets
  dirty bits; `layout.cpp` reads them back and applies them before each draw.

Development switches, all environment variables: `XV_D3D_TRACE=1` prints the
first call of every Direct3D method with its arguments and caller,
`XV_D3D_DEBUG=1` creates the host device with the debug layer and prints its
messages after the call that caused them, `XV_FRAME_DUMP=file.bmp` (with
`XV_FRAME_DUMP_AT=n`) writes the n-th presented frame to a file.

`runtime/modules/xg.cpp` is `xg_x.dll`, which tells titles how a texture is
laid out in memory. There is one layout here, the plain one.

The window (`xv_gui`) uses OpenGL through GLFW for its own interface and
gets everything else from `app/gui/main.cpp` as callbacks. Opening a title reads
and checks it on a worker thread while the window shows a loading screen, then
starts `xvboot` for it. The title does not draw through the interface, nor in
its process: `backends/surface` gives it a `TitleWindow`, a top-level window of
`xvboot` that the title's swap chain presents into and that is shown at its
first frame.

## Tests

Only the parsers of files that come from outside (PE, XVD) are tested, with broken input
included: `tests/*.cpp` on the synthetic PE files of `tests/corpus/generate.py`. The rest
is checked by running titles.
