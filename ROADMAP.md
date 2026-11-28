# Roadmap

Xbox One (Durango) comes first, Xbox Series X|S (Scarlett) after it. Both are
x86-64 consoles running a trimmed-down Windows, so most of the work is shared
and the Series runtime is built on top of what the Xbox One one needs.

XValkyrie runs on 64-bit Windows. Nothing here counts as "a game boots" until a
real title shows its own output.

## Phase 1: PE loader

Load x64 executables: imports, relocations, memory and entry points. Shared by
both consoles (`formats/pe`, `loader`, `runtime/common`).

- [x] PE32+ headers, sections, imports, delay imports, exports, relocations, TLS, exception tables
- [x] Host memory, files, threads and TLS helpers
- [x] Image mapping: sections, DIR64 relocations, import slots, page protection
- [x] Import resolver: own modules, then allowed Windows APIs, then a stub
- [x] Entry point on a guest thread, unwind tables, exit codes (Windows)
- [x] Runs our own test programs (with and without the CRT, C++ exceptions, RTTI, threads)
- [x] Console target detection from the import table (Xbox One, Series, PC)
- [x] Unpacked title folders: executable taken from `AppxManifest.xml`
- [x] Parses and maps a real ERA title (74k relocations, 502 imports)
- [x] Static TLS, with index 0 for executables built with /GA
- [x] Title-local DLLs: mapped like the executable, `DllMain` called before the entry point
- [x] Delay-load imports, bound at map time like regular imports (the lazy helper path is out of scope)
- [x] `LoadLibrary` / `GetProcAddress` going through the resolver
- [ ] Guest command line

## Phase 2: Xbox One runtime

Implement the APIs a test program needs and reproduce its behavior
(`runtime/durango`).

The test program is an unpacked ERA title kept outside the repository, in
`samples/`. What it needs is listed in
[docs/research/era-title.md](docs/research/era-title.md).

- [x] Find out exactly what it imports (`xvboot bind`)
- [x] `kernelx.dll`: the Win32 subset goes to the host, plus `XMemAllocDefault` / `XMemFreeDefault`
- [x] `combase.dll` and `ws2_32.dll` go to the host, also by ordinal
- [x] `etwplus.dll` as no-ops
- [x] `exec --trace`: first call of every import, last calls and C++ exception type on a fault
- [x] The entry point runs up to the first WinRT class that is missing
- [x] `msvcr110.dll`, `msvcp110.dll`, `vccorlib110.dll` from the host (needs the Visual C++ 2012 redistributable)
- [x] Activation factories: our classes first, a short list of host namespaces, placeholders for the rest
- [x] Placeholders that report the class, interface, method number and caller of every call nobody implements
- [x] Inert console classes for what the title touches before its first frame (transport controls,
  store license, users, network status, downloadable content, party)
- [x] `CoreApplication::Run`, `CoreApplicationView`, `CoreWindow` and `CoreDispatcher` with a host window
- [x] The title's view is created, initialized, loaded, activated and run, up to `D3D11CreateDevice`
- [x] A signed-in user: one `Windows.Xbox.System.User`, the same object everywhere (user
  list, the gamepad's user, the storage space's user), signed in, not a guest, with an id
  and the gamer tag "Player"
- [x] Xbox Live, multiplayer session, presence and chat answer enough for the title to go
  from its main menu to the next screen; nothing goes online
- [x] WinRT classes of the title's own DLLs (in-process servers of the manifest), loaded
  when first asked for: Xbox Live and game chat run their own code
- [x] Achievements (`AchievementNotifier`), chat sessions without a microphone and the store
  statics (dialogs, privilege checks) of `Windows.Xbox.*`
- [x] Session logs: everything xvalkyrie and the title print also goes to
  `logs/yyyy-mm-dd_hh-mm-ss.log` next to the executable; lines start with `[Launcher]` or
  `[Runtime]`
- [x] Options > Console: gamertag, user ID, gamerscore, age group, headset and how the system
  keyboard answers, saved in `config.ini`
- [x] Options > Console > Console model: the console a title is told it runs on, for each
  generation (Series X or S; Xbox One X, One S or One), through the registry value the GDK
  reads and `GetConsoleType`, with the memory a title has on it
- [x] `xvboot exec` takes any setting for one run (`--console`, `--profile`, `--set
  SECTION.KEY=VALUE`...), or none of `config.ini` (`--no-config`); nothing of it is saved
- [x] System keyboard (`ShowVirtualKeyboardAsync`, `KeyboardOptions`) as a window on the host
- [x] Party, party chat, secure device associations, quality of service, game DVR and speech
  recognizers report that they are not there
- [x] Title physical pages, large page reservations, XMem hooks, graphics and APU memory
- [x] `mmdevapi`, `mfplat`, `pixevt` and `toolhelpx` with their console-only functions
- [x] `acphal.dll`: SHAPE contexts and a HAL that acknowledges commands and ticks frames; nothing is decoded
- [ ] `XGCreateTextureComputer`: the layout of `XG_RESOURCE_DESC` is not known
- [ ] `D3D11XCreateDeviceX` and `DXGIXPresentArray` (Unity titles)
- [ ] Connected storage that stores: reads say "no such blob", writes succeed and are dropped
- [x] A title runs in a process and a window of its own (`xvboot`), started by the
  xvalkyrie window; its caption shows the title, the video driver, the frame rate and the
  version. F11 is full screen, borderless or native (Options > Graphics); the xvalkyrie
  window shows what the title's process prints
- [x] Opening a title does not block the window: it is read on a worker thread, with a
  loading screen until its first frame
- [x] Closing a running title without closing xvalkyrie
- [x] DLLs shipped with the title (`xcompress.dll`), loaded from its folder
- [x] Finished async operations and actions (`winrt/async.cpp`)
- [x] `CoreWindow` in the console layout (bounds, dispatcher, activate; the rest unknown)
- [x] Audio engine: `xaudio2_9.dll` from the host, with the console's creation function
- [x] The title's window messages are handled on another thread than the title's: moving
  or resizing the window does not stop it, and a busy title does not freeze the window
- [x] Input (`src/backends/input`): one gamepad for the title, fed by the keyboard or by the host's
  gamepad. Two capture methods, chosen in Options > Input together with the key bindings:
  from the window's events, or read directly from the system (XInput and the keyboard
  state). Saved in the per-user `config.ini`
- [x] `Windows.Xbox.Input.Gamepad`: the list of gamepads and the raw reading
- [ ] The controller interface of the gamepad (user pairing, id), vibration
- [ ] More than one gamepad, binding the host gamepad's buttons, mouse
- [ ] Window events (keys, size, visibility): registered, never raised

## Phase 3: Graphics

Two backends behind the console Direct3D modules, chosen in Options > Graphics or
with `--backend`:

- `d3d11`: the host's own Direct3D 11, and the default. This is the one that gets
  titles on screen first.
- `vulkan`: Direct3D 11 on Vulkan through DXVK with the changes console titles need
  (`cmake/dxvk.patch`).

ERA titles like the test program use `d3d11_x.dll` (Direct3D 11.X).

- [x] Backend selection (`backends/graphics/backend`)
- [x] Offscreen Vulkan clear + readback test (host only, not connected to anything)
- [x] `D3D11CreateDevice` from `d3d11_x.dll` on the host's Direct3D 11
- [x] Swap chain for the title's `CoreWindow`, created on the host window
- [x] Direct3D 11.X objects in the console layout in front of the host's: device, context
  (vtable and in-object method table), swap chain, resources, views, states, shaders
- [x] `xg_x.dll`: tile modes and texture sizes, with one plain layout
- [x] The test program creates its renderer and its UI renderer (targets, shaders, states,
  buffers, samplers)
- [x] State the title writes into the context object instead of calling a method (input
  layout, shaders, topology), applied at each draw
- [x] The test program presents frames: splash screens and its main menu scene
- [x] `XV_FRAME_DUMP` writes a presented frame to a file; `XV_D3D_DEBUG` prints what the
  host's debug layer says after each call
- [ ] The methods of the context whose place is still a guess (see `d3d11x/layout.cpp`)
- [ ] Check the picture against the console's, frame by frame
- [x] Vulkan backend: Direct3D 11 on a patched DXVK (`-DXV_DXVK=ON`), chosen in
  Options > Graphics and saved in `config.ini`
- [ ] The test program on the Vulkan backend, checked against the `d3d11` one
- [ ] `d3d12_x.dll`, and a D3D12 to Vulkan translation for it

## Phase 4: Xbox One compatibility

Filesystem, audio, input and the runtime services.

- [x] XVD/XVC header identification (read-only, layout from third-party research)
- [ ] Filesystem and storage paths; save games on disk
- [ ] Audio: the engine starts on the host's XAudio2; menu music does not stop in game
- [ ] Input: one gamepad from keyboard or XInput works; vibration, more gamepads, mouse
- [ ] Runtime services (Gaming Runtime, users, app lifecycle)
- [ ] More test programs, regression tracking

## GDK titles and Scarlett

Same executable, second runtime (`runtime/scarlett`). What GDK titles of both consoles
share is in `runtime/modules`. See [docs/research/gdk-titles.md](docs/research/gdk-titles.md).

- [x] A Series test program (a public GDK sample built with the local kit) and its imports
- [x] Titles described by `MicrosoftGame.config`: found, named and labelled in the library
- [x] `xgameruntime.dll`: task queues, async blocks, one user, saved games, package and system
- [x] `d3d12_xs.dll` on the Direct3D 12 layer `d3d12_x.dll` has, with the Series slots
- [x] `PresentX` and the frame events on a swap chain of the title surface
- [x] `GameInput.dll` with the emulated gamepad, `xmem.dll`, `xaudio2_9.dll` by ordinal
- [x] The Series sample draws its picture and reads the gamepad
- [x] All 40 public samples that build for Series present frames
- [x] Rectangle lists: the fourth corner of each comes from a geometry shader made for the
  pipeline's vertex shader
- [x] A sampler feedback map read back by copying it to a buffer, as whole mip levels
- [ ] Indexed quad lists, the fraction of a level in a sampler feedback map read back
- [ ] Shaders that only carry the console's GPU code (the retail GDK title looked at)
- [ ] `xg_xs.dll` texture layouts, `dstorage_xs.dll`
- [x] XStore without a store (the title is owned, nothing is for sale), XGameUi's dialogs and
  keyboard on the host, XAccessibility with the host's voice, installation monitors
- [ ] Keyboards and mice through GameInput
- [ ] Shaders that share memory in a thread group are translated, but their threads only
  wait for each other outside branches and loops
- [ ] Check the XVD layout against a real Series package

## System apps

The console's dashboard and its other apps are Windows apps (XAML, .NET Native) that use
the console's internal WinRT classes. They are started like a title, with the host's WinRT
classes where the host has them.

- [x] Options > Console > System storage, and File > Boot from Menu
- [x] An app's framework packages are found next to it or in the system storage
- [x] The dashboard's executable, its .NET Native runtime and its own DLL load and start
- [ ] `Windows.Xbox.System.Internal.Interop.AppModelUtil`, the first console class the
  dashboard asks for: it is not documented, and the dashboard ends there
- [ ] A XAML host for an app's views

## Other

- [x] C++20 project, CMake, tests, CI on Windows
- [x] `xvalkyrie <file>` entry point with a window that explains why launch failed
- [ ] Window test on Windows
- [ ] Retail titles are out of scope until packages can be handled legitimately
