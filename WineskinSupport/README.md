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
├── deps/build_deps.sh      third-party libraries Wine's Unix side loads (FreeType, GnuTLS), universal
│                           i386 + x86_64, and the wine-mono tarball
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
3. `WineskinSupport/deps/build_deps.sh` — FreeType, GnuTLS, and the wine-mono release `dlls/mscoree`
   expects, which staging unpacks into the engine's `share/wine/mono` so .NET programs run offline.
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
- Compute shaders can be created; a dispatch, like an indirect draw, does nothing. Unordered access views
  have no GL objects. BC6H/BC7 are unsupported formats.
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
- **Open and Save panels.** GetOpenFileName/GetSaveFileName, IFileOpenDialog/IFileSaveDialog and
  SHBrowseForFolder show NSOpenPanel or NSSavePanel, as a sheet on the owner window where there is one
  (`comdlg32/nativedlg.c`, `winemac.drv/cocoa_file_panel.m`, reached through winemac.drv's
  `wineskin_file_panel` export). A dialog with a hook, a custom template or custom controls keeps Wine's
  own. `HKCU\Software\Wine\Mac Driver\NativeFileDialogs` = `n` turns it off.
- **Command keys.** A wrapper maps both Command keys to Control (`CommandAsControl` in `Wineskin.plist`, on
  unless set off), so Command-C copies and Command-S saves; the app menu then takes Command-H and
  Command-Q.

## Audio

Devices offer no spatial audio client (`ISpatialAudioClient`), as on Windows without spatial sound:
programs use an ordinary stream, which a Mac mixes to its speakers either way. Wwise's spatial sink
corrupted its heap through Wine's (Ori and the Will of the Wisps). Media Foundation decoders (H.264
video, WMA) come from GStreamer, which the engine is built without, so in-game videos and xWMA music
do not play.

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
├── Resources/Engine/             the Wine installation (bin, lib/wine, share/wine) + FreeType, GnuTLS
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
task manager), and the Mac driver options.
