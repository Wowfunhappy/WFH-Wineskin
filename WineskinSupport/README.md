# WFH Wineskin

`WineskinSupport/` holds everything that makes this Wine fork build and run on **OS X 10.9.5 Mavericks**,
and that packages it as a **WFH Wineskin wrapper**: a single app that holds a Wine engine, a Windows environment and the program it
runs. A blank wrapper is copied once per program; there is no separate wrapper-creation app.

The build runs on a 10.9 host with the in-tree clang 22 against the **running system's own headers and
libraries** (the Command Line Tools' `/usr/include` and the frameworks under `/System`), deployment target 10.9.
No Xcode or SDK is needed.
Wine is built as a classic WoW64 pair: an x86_64 host tree with x86_64 PE modules, and an i386 host tree
(10.9 runs i386 processes natively) with i386 PE modules. `wine` runs either kind of program and switches
to the other loader as needed.

## The one rule: source vs. artifact

If a script can regenerate it, it is an artifact: it lives in a gitignored `build/` (or `deps/work/`) and
is never committed. Everything else is committed. Wine's own tree is the exception: its build reads some
generated files from the source tree, so those are committed beside their generators, as upstream commits
`aero.msstyles`' bitmaps (`dlls/aqua.msstyles/*.bmp` and `aqua.rc`, from `generate.py`).

## Layout

```
WineskinSupport/
├── README.md               this file
├── build.sh                configure (first run) and build build/wine64 and build/wine32, then stage the engine
├── toolchain/
│   ├── bootstrap.sh        assembles toolchain/build: clang 22 from vendor/, and m4, autoconf, bison,
│   │                       GNU make, pkgconf and nasm from source
│   ├── vendor/clang/       COMMITTED: clang 22 + lld (bzip2), llvm-ar, resource headers, compiler-rt
│   └── scripts/            build_gnu.sh (one GNU-style tool from a release tarball)
├── deps/
│   ├── build_deps.sh       third-party libraries Wine's Unix side loads (FreeType, GnuTLS, and the media
│   │                       runtime through build_media.sh), universal i386 + x86_64, and the wine-mono tarball
│   ├── build_media.sh      the media runtime: GLib, GStreamer and FFmpeg (see "Video and compressed audio")
│   └── patches/            patches build_media.sh applies to upstream sources
├── polyfill/               the 10.9 gap layer: libwineskin_polyfill.a + a header overlay (see below)
├── scripts/
│   ├── env.sh              (sourced) toolchain PATH, sysroot, deployment target, per-arch host flags
│   ├── build-log.sh        (sourced) the one build log, /tmp/wineskin_build.log
│   ├── configure-wine.sh   configure one tree
│   ├── track-polyfill.sh   make one tree depend on the polyfill layer (its GNUmakefile)
│   └── stage-engine.sh     install both trees into build/engine, the skeleton of a wrapper app
└── wrapper/
    ├── src/main.m          the launcher and configuration window
    ├── Info.plist
    └── build-wrapper.sh    assemble build/WFH Wineskin.app
```

## Building

1. Install the Command Line Tools (`xcode-select --install`), which put the C headers in `/usr/include`.
   `WINESKIN_SYSROOT` names another root, such as a `MacOSX10.9.sdk`.
2. `WineskinSupport/toolchain/bootstrap.sh`
3. `WineskinSupport/deps/build_deps.sh` — FreeType, GnuTLS, the media runtime, and the wine-mono release
   `dlls/mscoree` expects, which staging unpacks into the engine's `share/wine/mono` so .NET programs run
   offline. The media runtime takes about a quarter of an hour the first time.
4. `WineskinSupport/build.sh` — wait for `REBUILD DONE (rc=0)` in `/tmp/wineskin_build.log`. It builds the
   polyfill layer first, on every run.
5. `WineskinSupport/wrapper/build-wrapper.sh` — produces `build/WFH Wineskin.app`, a blank wrapper.

## Compilers and linkers

- Host (Mach-O) code: `toolchain/build/host/bin/clang`, the in-tree clang with ld64 as its linker (Apple's
  clang has it so): a link gets `-fuse-ld=/usr/bin/ld -mlinker-version=241.9`, a compile nothing extra.
  lld's Mach-O port has no i386, and the Wine loader and preloader rely on ld64's segment options.
  Compiles use `--no-default-config -isysroot / -mmacosx-version-min=10.9`.
- PE code: the in-tree clang as Wine's cross compiler (`-target <arch>-windows`), linked by lld-link.
- Every compile goes through ccache (`build/ccache`). A configure starts a fresh build tree; unchanged
  objects come back from the cache.

## The preloader on 10.9

On 10.9, CoreFoundation decides which compatibility behaviours a process gets from the libraries its main
executable links (later systems read the SDK version instead). The main executable of a Wine process is
the preloader, so it links libSystem: AppKit then behaves as for any app built with the 10.9 SDK, which is
what winemac.drv's drawing (`-updateLayer`) depends on.

libSystem initializes before the preloader runs, so the address ranges Wine needs are taken before it can
allocate there: the x86_64 low 8GB and, on i386, the low 64k, the DOS area, the low memory below
0x20000000 and the top-down area are zero-fill segments of the preloader, mapped by the kernel at exec.
The i386 range 0x20000000-0x68000000, which ntdll gives back to the system once the process is running,
is no segment of any image; the preloader reserves its free parts at startup.

## The polyfill layer

`polyfill/src/*.c` implement libc and framework entry points Wine uses that 10.9 lacks (the `*at()`
family, `clock_gettime`, `getattrlistat`, `os_unfair_lock`, …), compiled for both architectures with
hidden visibility into `polyfill/build/libwineskin_polyfill.a`. Every host link takes the archive, so an
image that calls a gap function holds a private copy. `polyfill/include/` is on every host compile's
system include path ahead of the system's: wrapper headers that `#include_next` the system header and add the
declarations later SDKs carry. Configure sees both, so Wine detects the functions as present.

A polyfill honors the real API's contract. Most of `src/` comes from macports-legacy-support by way of
the Aqua WebKit port; `polyfill/LICENSE` has the terms.

Wine's makedep sees neither the overlay nor the archive, so `build.sh` tracks them itself. It rebuilds
the polyfill layer on every run (ccache makes that about a second); a member, generated header, stub or
rsp keeps its modification time unless its bytes change. Before each make, `scripts/track-polyfill.sh`
writes the tree's `GNUmakefile`, which make reads ahead of `Makefile`:

- Every host compile also writes `<object>.d` (`-MD -MP`), the compiler's list of every header it read,
  overlay and generated ones included, and make reads the lists: an overlay header change recompiles
  the objects that include it.
- Every host image depends on `link-<arch>.rsp`, the stub Metal, and each archive member that defines a
  symbol the image names, defined or imported: a member change relinks the images that hold it or
  would now take it from the archive. A member whose bytes did not change relinks nothing.

An object compiled before its tree had a `GNUmakefile` gets its `.d` from a syntax-only pass. `make -C
build/wine64` honors the dependencies too; `make -f Makefile` does not. An overlay header the tools
include (`stdio.h`, `unistd.h`, …) or an rsp change relinks the tools, and with them every PE module.

## OpenGL on 10.9

winemac's views are layer-backed, and 10.9's AppKit gives an `NSOpenGLContext` attached to a
layer-backed view no drawable (its default framebuffer is `GL_FRAMEBUFFER_UNDEFINED`). The AppKit
polyfill (`polyfill/src/objc/AppKit.m`) has `-setView:` attach the context the way it attaches to a view
without a layer, a window-server surface over the view. The same file finishes `-[NSOpenGLContext
dealloc]` as an `NSOpenGLContext`, since 10.9's dealloc messages `-clearDrawable` to the half-freed
subclass.

winemac probes OpenGL with accelerated renderers only, as upstream does. A Mac with no accelerated
renderer, such as a virtual machine, falls back to the software renderer automatically
(`opengl.c: has_accelerated_renderer`), so no machine-specific setting lives in a wrapper's prefix.
Don't set `AllowSoftwareRendering` in a prefix that will be copied to a Mac with a GPU.

### Direct3D 10 and 11 on OpenGL 4.1

macOS 10.9 stops at OpenGL 4.1: no compute shaders, image load/store, atomic counters, cull distances,
derivative control, half-float packing, integer `mix()`, polygon offset clamp, base instance, timestamp
counters or pipeline statistics. wined3d (`dlls/wined3d`) still offers feature level 11_0 there:

- Shader model 4 and 5 graphics shaders translate; the GLSL they need and 10.9 lacks (integer `mix()`,
  `packHalf2x16()`, coarse/fine derivatives) is declared in each shader
  (`shader_glsl_declare_missing_builtins`). Cull distances are dropped: primitives are drawn, not culled.
- Compute shaders run on OpenCL 1.2 (see below). Pixel shaders that use unordered access views, and
  indirect draws, do nothing. BC6H/BC7 are unsupported formats.
- Constant (stride 0) vertex attributes of any format of 8, 16 or 32-bit components are set from their
  element (`wined3d_context_gl_load_stride0_attrib`).
- Vertex shaders whose input registers run past 10.9's 16 attributes have the inputs they read packed into
  consecutive locations; inputs they declare but never read, and vertex/instance IDs, take no attribute
  (`wined3d_vs_attrib_location`).
- Timestamp queries read the CPU clock; pipeline statistics queries count nothing.
- Multisample textures exist without immutable storage: they are allocated with `glTexImage2DMultisample`
  and `glTexImage3DMultisample`, so games can resolve and sample MSAA render targets.
- 10.9's driver drops a buffer texture's storage when its buffer is deleted, and then faults on the next
  buffer change while that texture is bound, leaving an internal lock held so the next GL call hangs.
  The dummy buffer texture keeps its buffer, and units holding a buffer texture get the dummy before any
  buffer is deleted (`wined3d_context_gl_unbind_buffer_textures`).

#### Compute shaders on OpenCL

Where OpenGL lacks `ARB_compute_shader`, a dispatch runs the compute shader as an OpenCL 1.2 kernel on the
GPU, on an OpenCL context that shares the current OpenGL context's share group
(`CL_CONTEXT_PROPERTY_USE_CGL_SHAREGROUP_APPLE`), so kernels read and write wined3d's buffers and textures
in place.

- `dlls/wined3d/glsl_cl.h` (included by `glsl_shader.c`) generates the kernel with the GLSL backend in an
  OpenCL C dialect: the arithmetic and control-flow handlers emit their GLSL unchanged, and a prelude of
  typedefs, constructor macros and overloaded functions makes it OpenCL C (`vec4` is `float4`, `bvec4` an
  `int4` of 1s and 0s, `mix()` with a boolean selects, and so on). Resources, thread group shared memory
  (`__local` arrays), atomics, UAV counters and barriers have handlers of their own. A kernel is generated
  per combination of OpenCL image kinds bound to the shader's texture views.
- Kernel arguments: constant buffers and buffer views are OpenGL buffers with a byte offset (typed buffer
  views decode their format in the kernel); texture views are images of the view's first level, with
  out-of-bounds reads returning 0 and writes dropped; samplers take the view's address mode and filter.
  A texture UAV the shader reads is bound twice, as a write-only and a read-only image.
- `dlls/wined3d/cl_compute.c` loads the bound resources into their OpenGL locations, describes each
  argument, reads indirect arguments back, and dispatches. It also clears unordered access views (10.9 has
  neither `ARB_clear_texture` nor `ARB_clear_buffer_object`). Formats OpenCL can't share (R11G11B10F,
  RGB10A2, sRGB, SNORM, 24-bit depth, compressed) get a shadow texture of a shareable format, blitted on
  the GPU (or copied through a pixel buffer) before a dispatch and back after one that writes it.
- `dlls/wined3d/cl.c` (`wined3d.so`) builds the kernels, keeps an OpenCL memory object per OpenGL buffer
  and texture level until wined3d deletes the OpenGL object, acquires them, runs the NDRange and flushes;
  the share group orders the OpenCL work with the OpenGL commands around it.
- Uncompressed, non-sRGB color formats report unordered access support.
- OpenCL 1.2 has no image atomics: atomics on texture UAVs run as a read and a write. Sampling reads the
  view's first level only; cube textures are read one face at a time; subroutines (`call`) don't
  translate, and the shader's dispatches then do nothing.
- `HKCU\Software\Wine\Direct3D\OpenCLCompute` = `n` (or `WINE_D3D_CONFIG=OpenCLCompute=n`) turns it off;
  dispatches then do nothing. `WINEDEBUG=+d3d_cl` logs each kernel's source and build log.

An OpenGL context made current before its window is on screen waits for the view's next `-viewWillDraw`,
which a layer-backed view that shows only OpenGL may never get on 10.9; such a context attaches the next
time it is made current with its window visible (`macdrv_attach_pending_view_opengl_context`).

## Native look and feel

- **Menu bar.** A top-level window's Win32 menu bar is shown in the Mac menu bar, between the app menu and
  the Window menu, instead of inside the window (`winemac.drv/menu.c`, `cocoa_menu.m`; win32u reserves no
  menu bar space when the driver hosts it). Menus are refreshed with `WM_INITMENUPOPUP` as they open, and
  `Ctrl` accelerators become Command key equivalents. GTK (2 and 3) and Qt (4, 5 and 6) programs, which draw their
  menu bars themselves, get the same: when such a window is activated, `winemac.drv/toolkit_menu.c` reads
  the toolkit's menu bar through the functions its DLLs export, gives the window a Win32 menu bar made
  from it, hides the toolkit's, refills a menu from the toolkit's as it opens, and activates the
  toolkit's item for a command. `HKCU\Software\Wine\Mac Driver\NativeMenuBar` = `n` turns all of it off.
- **Aqua theme.** `dlls/aqua.msstyles`, the default theme, draws common controls as OS X 10.9 draws them,
  with Lucida Grande for menus, captions, message boxes and status bars. `generate.py` in that directory
  draws every bitmap. Dialogs use Lucida Grande too: `wine.inf` substitutes it for "MS Shell Dlg" and
  "MS Shell Dlg 2", and dialog templates size their controls from it.
- **Text smoothing.** The wrapper sets the prefix's Windows text smoothing to match the Mac's: ClearType
  (subpixel) when the Mac uses LCD font smoothing, standard (grayscale) when "Use LCD font smoothing when
  available" is off. `MatchFontSmoothing` writes `HKCU\Control Panel\Desktop` only when the Mac's setting
  differs from the one recorded in `Wineskin.plist` (`LCDFontSmoothing`). Notepad's default font is Menlo.
- **Open and Save panels.** GetOpenFileName/GetSaveFileName, IFileOpenDialog/IFileSaveDialog and
  SHBrowseForFolder show NSOpenPanel or NSSavePanel, as a sheet on the owner window where there is one
  (`comdlg32/nativedlg.c`, `winemac.drv/cocoa_file_panel.m`, reached through winemac.drv's
  `wineskin_file_panel` export). A dialog with a hook, a custom template or custom controls keeps Wine's
  own. `HKCU\Software\Wine\Mac Driver\NativeFileDialogs` = `n` turns it off.
- **Command keys.** A wrapper maps both Command keys to Control (`CommandAsControl` in `Wineskin.plist`, on
  unless set off), so Command-C copies and Command-S saves; the app menu then takes Command-H and
  Command-Q.

## Full screen and display modes

A program's display mode changes are emulated: `ChangeDisplaySettings`, DirectDraw's `SetDisplayMode` and
Direct3D's fullscreen modes succeed and Windows reports the new mode, but the display keeps its own. Each
display offers its current mode, read from the display at run time (in pixels in Retina mode), as its
largest and default mode, and win32u's usual list of smaller sizes below it (`get_virtual_modes`); the
native mode draws 1:1. When a display is reconfigured or replaced while Wine runs, its mode follows,
unless a program set a mode the display still holds. Windows' coordinates in a smaller mode map to the
display's (win32u's monitor scaling, as for DPI), centered and scaled to fit, so a window that covers the
emulated screen covers the display, with black beside a 4:3 picture on a 16:9 display, and the pointer,
cursor clipping and window rects all stay in the program's coordinates. winemac gives a window's OpenGL
surface the window's size in Windows' coordinates and the window server scales it to the window
(`kCGLCPSurfaceBackingSize`), so OpenGL, Direct3D 8 to 11 and DirectDraw, which draw through it, need
no framebuffer copy. Emulation is `HKCU\Software\Wine\Mac Driver\EmulateModeset` (on unless set `n`;
`win32u/sysparams.c`); gamma ramps do nothing while it is on.

A visible top-level window whose content covers a screen, as a fullscreen game's does, goes full screen
in a Space of its own, as with the green button (`-[WineWindow wine_updateFullScreenSpace]`,
`HKCU\Software\Wine\Mac Driver\FullScreenSpace`, on unless set `n`):

- It enters once it has covered the screen for 0.4 seconds, since engines resize and recreate their
  windows while they start, and only while the app is active on that screen's Space. It leaves when
  Windows makes it stop covering the screen (the game switches to windowed), when it is hidden, or when
  it gains an owner window it moves with. A window the user takes out of its Space stays out until it
  stops covering the screen.
- Its Win32 rect stays what the program set; frame changes Cocoa makes for the Space are not posted.
- The menu bar and Dock stay hidden in the Space, even with the pointer at the top or bottom edge.
- Hot corners act even in a full-screen Space, and no presentation option stops them, so while the
  window is in front in its Space, the hot corners that need no modifier key are turned off with the
  Dock's own SPI (`CoreDockSetExposeCornerActionWithModifier`, as System Preferences sets them) and
  turned back on when it leaves the Space, the app is deactivated or the process exits normally
  (`suppress_hot_corners` in `cocoa_app.m`). A corner the user sets in the meantime keeps their choice.
  A process killed while its game is in front leaves the corners off.
- The app's other windows, such as message boxes, appear on the game's Space above it.
- Command-Tab leaves the Space where it is, as for a Mac app. A game Windows minimizes
  when it loses the foreground (Direct3D's fullscreen devices do) stays in its Space and is restored when
  the user comes back; minimized while it is in front, it leaves the Space for the Dock.

The wrapper's "Full screen in its own Space" (on by default) sets both values; off, a covering window is
raised over the current Space and display mode changes are real, as in upstream winemac. They go
together because real mode changes resize every Space and move the desktop's windows around a game's
Space, and a mode left behind by a game that quits or crashes is the user's to fix.

### Retina mode

In Retina mode (the wrapper's "Retina mode", `RetinaMode`), Windows sees two pixels per point each way on
a display in the mode it had when Wine started: a Retina display's real pixels, and on a 1x display
twice its pixels, which the window server draws at half size. Each display's DPI is
96 per pixel per point, 192 for those (`display_dpi` in `winemac.drv/display.c`), so DPI-aware programs
draw at 200% and win32u scales DPI-unaware windows up, as on Windows at 200%. The wrapper sets the system
DPI (`HKCU\Control Panel\Desktop\LogPixels`) to 192 when Retina mode is turned on and 96 when it is
turned off. Out of Retina mode, displays are in points at the system DPI. A game's native mode in Retina
mode is the display's pixel size.

## Audio

Devices offer no spatial audio client (`ISpatialAudioClient`), as on Windows without spatial sound:
programs use an ordinary stream, which a Mac mixes to its speakers either way. Wwise's spatial sink
corrupted its heap through Wine's (Ori and the Will of the Wisps).

## Video and compressed audio

Media Foundation's and DirectShow's media sources and decoders (H.264, MPEG-1/2/4, WMV, WMA and the
xWMA XAudio2 plays through `wmadmod`, AAC, MP3, ...) are winegstreamer's, which runs GStreamer;
winedmo's demuxers run FFmpeg. `deps/build_media.sh` builds both, each architecture into
`deps/work/media/<arch>/prefix` and merged with lipo into `deps/build`:

- GLib 2.80.5, with libffi 3.4.6 (10.9's lacks the closure API), pcre2 and proxy-libintl built in.
- GStreamer 1.28.7 with the plugins Wine's pipeline uses and no others: core elements; from base app,
  audioconvert, audioresample, pbtypes, playback (decodebin), rawparse, typefind, videoconvertscale;
  from good apetag, audioparsers, avi, deinterlace, id3demux, isomp4, matroska, videofilter, wavparse;
  from bad mpegdemux, mpegtsdemux, videoparsers; from ugly asfdemux; and gst-libav, whose decoders are
  FFmpeg's. orc 0.4.41 compiles the SIMD code of the converters.
- FFmpeg 8.1.3 with decoders, demuxers, parsers and bitstream filters only: no encoders, muxers,
  filters, devices, network, or Apple frameworks.

Everything compiles against 10.9's own headers. `deps/patches/gstreamer-macos-10.9.patch` gives
`gst_macos_main()` (the tools') AppKit's 10.9 constant names and synthesized properties i386's
Objective-C runtime needs. Meson's links go through `deps/work/media/bin/cc`, which drops two arguments
10.9's linker tools mishandle (see the script).

Every library's install name is `@rpath/<name>`. The engine holds the libraries in `lib/`, the plugins
in `lib/gstreamer-1.0` (rpath `@loader_path/..`) and the plugin scanner in `libexec/gstreamer-1.0`, where
a relocated libgstreamer looks for it; `lib/media-runtime.txt` lists the libraries staging copies.
winegstreamer.so and winedmo.so carry the rpath `@loader_path/../..`, the engine's `lib/`. Before it
initializes GStreamer, winegstreamer (`unixlib.c`, `wineskin_set_gstreamer_paths`) sets
`GST_PLUGIN_SYSTEM_PATH_1_0` to the engine's `lib/gstreamer-1.0` and `GST_REGISTRY_1_0` to
`<prefix>/gstreamer-1.0/registry.<arch>.bin`, unless the environment sets them, so a wrapper and a
plain `bin/wine` use the bundled plugins and never a system or home-folder GStreamer. Wine loads
plugins in its own process to build the registry (`--gst-disable-registry-fork`), once per prefix and
architecture.

The media session (`dlls/mf/session.c`) asks a transform's upstream node for one sample at a time,
so decoded frames don't pile up ahead of the renderer; a 32-bit program playing 4K video, such as
Tempest 4000's 3840x2160 intro, holds only the frames it is showing.

## TLS

Schannel (`secur32`), and with it WinHTTP, WinINet and every program that speaks TLS through Windows,
runs on GnuTLS 3.8.13. `deps/build_deps.sh` builds it with nettle 3.10.2 (and hogweed) and GMP 6.3.0
linked in statically, GnuTLS's own copies of libtasn1 and libunistring, and no PKCS #11, IDN, TPM,
compression or tools: `libgnutls.30.dylib`, install name `@rpath/libgnutls.30.dylib`, needs only libSystem,
CoreFoundation and Security. Wine dlopens it by that name (`SONAME_LIBGNUTLS`), and the engine's `lib`
holds it, which the wrapper and the test scripts put on `DYLD_FALLBACK_LIBRARY_PATH`.

GnuTLS checks no certificates, and neither does Wine's Schannel: a program that asks Schannel to
validate the server (`SCH_CRED_AUTO_CRED_VALIDATION`, such as libcurl's Schannel backend) gets any
certificate accepted. WinHTTP and WinINet check the chain themselves with crypt32, against the roots
`SecTrustSettingsCopyCertificates` returns for the system, admin and user domains
(`dlls/crypt32/unixlib.c`): the roots this Mac has, plus any an administrator added. 10.9's own set
predates ISRG Root X1 and other current roots, so sites issued from those verify only on a Mac that has
them installed. Wine's Schannel offers TLS 1.0 to 1.2; a server that speaks only TLS 1.3 fails the
handshake.

## The wrapper

```
Name.app/Contents/
├── MacOS/Wineskin                the launcher
├── Resources/Engine/             the Wine installation (bin, lib/wine, share/wine) + FreeType, GnuTLS,
│                                 the media runtime (lib, lib/gstreamer-1.0, libexec/gstreamer-1.0)
└── SharedSupport/
    ├── prefix/                   the Windows environment (WINEPREFIX)
    ├── Wineskin.plist            the program and options
    └── Logs/
```

A configured wrapper's launcher execs Wine before AppKit starts, so the program runs in the process
LaunchServices launched. A 64-bit program gets the 64-bit preloader and loader directly, with
`WINELOADERNOEXEC=1`: Wine's default loader is the 32-bit one, which would start a 64-bit program in a
new process through start.exe. Every Wine process carries `CFProcessPath`, naming the launcher, from which
CoreFoundation finds its main bundle: the program is the wrapper app to the system, under the wrapper's
one Dock icon, and Keep in Dock and Recent Items open the wrapper.

The other Wine processes (explorer, services, a program's helpers) take the bundle as theirs too. A
bundle's Info.plist makes a process a Dock app the moment AppKit starts, so winemac (`cocoa_main.m`)
gives every Wine process but the launched one (`WINESKIN_APP_PID`) an in-memory `LSUIElement`: they
start as agents, as unbundled Wine processes do, and become Dock apps only when they show a window. The
bundle on disk stays an ordinary application. The Run button in the configuration window execs the
program the same way.

A wrapper is named by its file name. On the first launch after a copy or rename, the launcher writes
`CFBundleName` and a `CFBundleIdentifier` of its own (`com.wowfunhappy.wfhwineskin.<name>`) into
Info.plist, registers the bundle, and relaunches once through LaunchServices, so the menu bar, Cmd-Tab,
Keep in Dock and Recent Items all name and open this wrapper, never another copy.

Each user's Desktop folder in the prefix is a directory of its own, not Wine's link to the Mac's
Desktop, so the shortcuts installers leave there stay out of Finder. The launcher makes it so when it
creates the prefix and before each run.

A program's AppData folder lives in the wrapper's own folder in `~/Library/Application Support` (named
after the wrapper the first time, kept in `Wineskin.plist` as `DataFolder`), where settings and saved games
outlast the wrapper; the prefix's `AppData` is a link to it. Documents can live there too, instead of being
Wine's link to the Mac's Documents. "AppData in Application Support" (on) and "Documents in Application
Support" (off) in the configuration window choose; turning AppData off moves it back into the prefix, and
turning Documents off links the Mac's Documents again, leaving what the program saved where it is.

Opening a wrapper with no program, or with Option held, shows the configuration window: install
software, choose the program and its arguments, Wine's tools (configuration, registry, command prompt,
task manager), and the Mac driver options, among them "Full screen in its own Space" (see "Full screen
and display modes").
