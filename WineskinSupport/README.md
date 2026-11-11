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
what winemac.drv's window drawing depends on.

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

On 10.9 winemac's content views have no layer (`wineskin_layer_views` in `cocoa_window.m`): a view
draws its window surface in `-drawRect:`, with the window's shape as a clip and letterbox bars drawn
over it, and an `NSOpenGLContext` attaches to it as to any view without a layer, a window-server
surface that AppKit keeps over the view. That surface is in front of the window's other content, so
OpenGL drawing covers sibling and child windows over it. 10.9's AppKit gives a context attached to a
layer-backed view no drawable (its default framebuffer is `GL_FRAMEBUFFER_UNDEFINED`); with
`WINESKIN_LAYER_VIEWS=1` the views are layer-backed, for comparison, and the AppKit polyfill
(`polyfill/src/objc/AppKit.m`) then has `-setView:` attach the context the way it attaches to a view
without a layer. The same file finishes `-[NSOpenGLContext dealloc]` as an `NSOpenGLContext`, since
10.9's dealloc messages `-clearDrawable` to the half-freed subclass.

A program can draw with OpenGL, and so with Direct3D, into a child window whose top-level window
belongs to another process, as a launcher that runs its game in a window of its own does (Rez
Infinite). The drawing process has no Cocoa view of that window, so winemac gives the context an
off-screen window as its drawable. The pixels of a window that isn't on screen fail OpenGL's pixel
ownership test, so for such a window win32u gives the program a framebuffer surface (framebuffer
objects standing in for the default framebuffer), and at each buffer swap winemac copies the frame from
it into one of three IOSurfaces shown by a layer of a `CAContext`; the process that owns the top-level window hosts that
context at the child's place in its content view (a `CALayerHost`), once a `WM_MACDRV_CREATE_REMOTE_LAYER`
message reaches it (`winemac.drv/remote_gl.c`, `cocoa_remote_gl.m`). Frames go from the drawing process
to the window server directly.

winemac probes OpenGL with accelerated renderers only, as upstream does. A Mac with no accelerated
renderer, such as a virtual machine, falls back to the software renderer automatically
(`opengl.c: has_accelerated_renderer`), so no machine-specific setting lives in a wrapper's prefix.
Don't set `AllowSoftwareRendering` in a prefix that will be copied to a Mac with a GPU.

### Direct3D 8 and 9 shaders: zero and infinity

Direct3D 8 and 9 define `rsq(0)` and `rcp(0)` as the largest float and `log(0)` as its negative. GLSL
gives infinities, and the shader's next multiplication by zero makes a NaN, which Apple's drivers draw
as black or white blocks in bloom, specular and fog passes. For shader model 1 to 3, wined3d's GLSL
backend clamps the results of `rsq`, `rcp`, `log` and `pow` to ±`FLT_MAX` (`shader_glsl_guard_inf`).
This is wine-staging's `wined3d-zero-inf-shaders` patch set by Paul Gofman (Wine bugs 34266 and 45375),
which CrossOver ships switched on. `HKCU\Software\Wine\Direct3D\multiply_special` (a DWORD, or
`WINE_D3D_CONFIG=multiply_special=…`) selects 1 (the default, the clamp), 2 (`mul`, `mad` and `dp2/3/4`
take 0 × x as 0 instead, with slower shaders) or 0 (neither).

### Direct3D 10 and 11 on OpenGL 4.1

macOS 10.9 stops at OpenGL 4.1: no compute shaders, image load/store, atomic counters, cull distances,
derivative control, half-float packing, integer `mix()`, polygon offset clamp, base instance, timestamp
counters or pipeline statistics. wined3d (`dlls/wined3d`) still offers feature level 11_0 there:

- Shader model 4 and 5 graphics shaders translate; the GLSL they need and 10.9 lacks (integer `mix()`,
  `packHalf2x16()`, coarse/fine derivatives) is declared in each shader
  (`shader_glsl_declare_missing_builtins`). Cull distances are dropped: primitives are drawn, not culled.
- Compute shaders run on OpenCL 1.2 (see below).
- There is no base instance (`ARB_base_instance`): a draw with a start instance points each per-instance
  vertex attribute array start instance elements further for the draw (`base_instance_gl.c`);
  Direct3D's `SV_InstanceID` starts at 0 either way. Indirect draws (`ARB_draw_indirect`) read their
  arguments on the GPU, where OpenGL 4.1 reserves the start instance field; NVIDIA's driver honours it
  (`~/WineskinTests/d3d11tools/probe/baseinst.c`). An indirect draw whose arguments are current in system
  memory with a start instance other than 0 is drawn as a direct draw with them, whatever the driver.
- There is no BPTC texture compression. BC6H and BC7 textures are decoded when they are uploaded
  (`bptc_decompress.c`), through wined3d's own decompress-on-upload path (`WINED3D_FORMAT_CAP_DECOMPRESS`):
  the texture keeps its blocks in system memory, where `Map`, `UpdateSubresource` and
  `CopySubresourceRegion` work on them, and is B8G8R8A8 (sRGB for `BC7_UNORM_SRGB`) or R16G16B16A16_FLOAT
  in OpenGL, mip levels, array layers and cube faces included. The decoder is bcdec by Sergii Kudlai
  (`dlls/wined3d/bcdec.h`, MIT or Unlicense, the same file Wine has in `dlls/d3dx9_36`).
- A pixel shader that reads `SV_RenderTargetArrayIndex` takes it from the varying the geometry shader
  writes for that register, with constant interpolation (`shader_glsl_input_pack`): OpenGL 4.1 has no
  `gl_Layer` in fragment shaders. Unreal Engine renders its colour grading table one layer at a time and
  takes the blue coordinate from the layer. The centroid and sample interpolation modes interpolate at
  the pixel centre.
- A vertex or domain shader that writes `SV_RenderTargetArrayIndex` or `SV_ViewportArrayIndex` (OpenGL 4.1
  has `gl_Layer` and `gl_ViewportIndex` only in geometry shaders) comes with a geometry shader without a
  function, made from its output signature (`wined3d_shader.layer_gs`). The command stream binds it where the
  application binds no geometry shader, and it emits each primitive as it comes (adjacency primitives without
  their adjacent vertices, tessellated draws as the tessellator's primitives) with the layer and viewport
  index set. The device reports `VPAndRTArrayIndexFromAnyShaderFeedingRasterizer`. Unity's Sayonara Wild
  Hearts draws into texture arrays this way.
- A draw without a pixel shader (a depth prepass, a shadow map) links an empty fragment shader
  (`shader_glsl_get_empty_fragment_shader`): 10.9's core profile fails every draw of a program that has
  none with `GL_INVALID_OPERATION`.
- Graphics shaders that use unordered access views translate without image load/store
  (`shader_glsl_declare_image_emulation`): each view is a sampler on one of the stage's last texture
  units, so loads and size queries read the view as it is before the draw. Stores do nothing, atomic
  operations and counters return what they read without changing it, and `early_fragment_tests` is not
  declared. The shader's color and depth output is drawn as usual. A view of some layers or of a level
  other than the first is read through a texture of its own (see the next item). Stores have no
  emulation: an OpenGL 4.1 fragment shader writes only its framebuffer attachments, at its own pixel,
  and a store writes any texel or buffer element (there are no shader storage buffers either).
- `resinfo`'s level count comes from a uniform array loaded with the level count of the view bound to
  each sampler (`shader_glsl_load_level_counts`): `textureQueryLevels()` is OpenGL 4.3.
- There are no texture views (`ARB_texture_view`). A shader resource view is bound as its texture, with
  the view's level range set as the texture's base and maximum level
  (`wined3d_shader_resource_view_gl_apply_view`). Typeless textures and swap chain buffers of a format
  with an sRGB variant are allocated with the sRGB internal format (`srgb_view_internal`): sRGB views
  decode and encode (`EXT_texture_sRGB_decode`, `GL_FRAMEBUFFER_SRGB`), other views skip decoding
  through a second sampler object and draw with `GL_FRAMEBUFFER_SRGB` off; the GLSL backend sets
  `GL_FRAMEBUFFER_SRGB` from the render target view of each draw (`shader_glsl_apply_draw_state`).
  A typeless format is allocated as the last of its float and normalised formats (`init_typeless_formats`):
  R32 as R32F, R8 as R8 UNORM, R8G8 as RG8 UNORM, which is how games view them (Unity's ambient occlusion
  and depth targets are typeless with float views).
- A shader resource view its texture can't stand for is a texture of its own (`view_copy_gl.c`): a view
  that reads a typeless texture's bits as another kind of number than it is stored as (an R32_UINT view
  of an R32 typeless texture), a view of some of the texture's layers, and a view of another type than
  the texture (a cube view of an array, a 2D view of one layer). The view's texture is brought up to date
  where a draw loads its resources, and only when the texture has been written since
  (`wined3d_texture.content_serial` changes with every write). The copy is a draw with a fragment shader
  that fetches the source texel and writes its value or, for the first kind, its bits
  (`floatBitsToUint()` and the like; `texture_copy_gl.c`), so nothing is read back from the GPU. Views
  of compressed textures hold decoded texels, depth views hold depth without stencil. Render target and
  unordered access views that would write a typeless texture as another kind of number use the texture as
  it was allocated.
- 10.9's driver keeps the converted colour of the previous `glClear` when the same colour is cleared again
  with `GL_FRAMEBUFFER_SRGB` toggled, so clears of sRGB views convert the colour themselves and run with
  it off (`ffp_blitter_clear_rendertargets`). Its `glBlitFramebuffer` never converts, so copies, resolves
  and presents keep the texture's bits. A multisample resolve between an sRGB internal format and its linear
  one is allowed, so a resolve into a typeless texture goes straight into it (`wined3d_gl_linear_internal`);
  resolves between other formats go through an intermediate texture of the resolve format, which the CPU
  blitter copies (there is no `ARB_copy_image`). The driver averages sRGB samples as stored, not in linear
  space, so an unscaled sRGB resolve from a multisample texture into a texture is a draw instead
  (`resolve_gl.c`): it fetches the samples, decodes them (the fetch does for an sRGB internal format),
  averages and encodes. Resolves through renderbuffers still average the stored bytes, so their edge pixels
  come out a little darker than on Windows.
- A rasterizer state's `DepthBiasClamp` is ignored (there is no polygon offset clamp): the depth bias is
  applied unclamped. Clamping it would mean writing depth from the fragment shader, which turns early depth
  testing off for those draws; shadow passes, which use depth bias, mostly leave the clamp at 0.
- R11G11B10_FLOAT vertex elements can be declared; constant (stride 0) ones are decoded, arrays of them
  read as (0, 0, 0, 1) (there is no `ARB_vertex_type_10f_11f_11f_rev`).
- A fragment shader gets 16 texture units, and shader model 5 shaders can sample more (resource, sampler)
  pairs than that. For such a shader, pairs bound to the same view and sampler for a draw, and pairs with
  no view, share a unit: the shader is compiled per sharing pattern with the duplicates `#define`d to the
  sampler they share (`wined3d_shader_get_sampler_aliases`). When more than 16 distinct pairs remain, the
  last ones read an earlier sampler's texture, and a FIXME names them.
- A buffer view of part of a buffer (no `ARB_texture_buffer_range`) attaches its buffer texture to a
  buffer of its own, and copies the viewed range into it on the GPU when the view is used after the
  buffer was written (`wined3d_gl_view_load_buffer_range`; `wined3d_bo.content_serial` changes with every
  write to a buffer object).
- OpenGL 4.1 makes buffer textures of a short list of formats: no SNORM, BGRA or 10/10/10/2 ones. A buffer
  view of another format (Unreal Engine fetches vertex tangents from R8G8B8A8_SNORM views) reads a buffer
  of its own that holds the viewed elements as RGBA32F. The conversion is on the GPU
  (`buffer_convert_gl.c`): the source is fetched as a vertex attribute of the view's format and captured
  with transform feedback, again only after the source was written. Integer formats outside the list
  and R11G11B10_FLOAT have no conversion.
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
- Kernel arguments: constant buffers and buffer views are OpenGL buffers, or copies of them (below), with
  a byte offset (typed buffer views decode their format in the kernel); texture views are images of the view's first level, with
  out-of-bounds reads returning 0 and writes dropped; samplers take the view's address mode and filter.
  OpenCL 1.2 images have one level: a view the kernel reads with `sample_l`, `sample_d` or `ld` at a mip
  level gets one image per level (at most 12), the kernel picks the level from the level of detail
  (clamped by the sampler's minimum and maximum, plus its bias) and, with a linear mip filter, blends the
  two nearest levels itself (`shader_cl_append_level_read` in `glsl_cl.h`; checked by
  `~/WineskinTests/d3d11tools/probe/csmip.c`).
  A texture UAV the shader reads is bound twice, as a write-only and a read-only image.
- Wine's opengl32 gives each group of contexts that share objects (a wined3d device) object names of its
  own and keeps every context of the process in one share group of the Mac's OpenGL
  (`dlls/opengl32/wgl.c`). OpenCL takes the Mac's names, so `cl_compute.c` binds an object the Unix side
  has not seen and `unix_cl_register` asks the Mac's OpenGL which name is bound there; memory objects
  are kept per device and name. A second device's names differ from the Mac's: a kernel given wined3d's
  names writes to another device's objects, which the NVIDIA driver answers with "GPU hang occurred".
- `dlls/wined3d/cl_compute.c` loads the bound resources into their OpenGL locations, describes each
  argument, reads indirect arguments back, and dispatches. It also clears unordered access views (10.9 has
  neither `ARB_clear_texture` nor `ARB_clear_buffer_object`). Formats OpenCL can't share get a shadow
  texture of a shareable format: which formats share is tried at initialisation (`test_shareable_formats`
  in `cl.c`); with NVIDIA's and Intel's drivers R11G11B10F, RGB10A2, sRGB, SNORM, 24-bit depth and
  compressed formats don't. A dispatch
  copies the texture to its shadow when the texture has been written since they were last in step, and
  copies a shadow it wrote back; both copies are draws (`texture_copy_gl.c`). Only a texture OpenGL
  can't draw to gets its shadow back through a pixel buffer, which waits for the GPU.
- `dlls/wined3d/cl.c` (`wined3d.so`) builds the kernels, keeps an OpenCL memory object per OpenGL buffer
  and texture level until wined3d deletes the OpenGL object, acquires them, runs the NDRange and flushes;
  the device has `cl_khr_gl_event`, under which acquiring OpenGL objects waits for the OpenGL commands
  of the context bound on the calling thread, and OpenGL commands issued after the release see the
  kernel's results; both calls run on wined3d's command stream thread with its OpenGL context current.
  A device without the extension finishes OpenGL before and OpenCL after each dispatch.
- One OpenCL context serves the process's share group. When a device deletes its OpenGL contexts, its
  kernels in flight are finished and its shadows, kernels and memory objects are released first
  (`wined3d_cl_release_device`); the OpenCL context goes with the last device, since an OpenCL context
  must never outlive its share group.
- Uncompressed, non-sRGB color formats report unordered access support.
- OpenCL 1.2 has no image atomics: atomics on texture UAVs run as a read and a write. `sample`, `sample_b`
  and the comparison samples read the view's first level; cube textures are read one face at a time;
  subroutines (`call`) don't translate, and the shader's dispatches then do nothing.
- A memory object made with `clCreateFromGLBuffer` reads what the buffer holds when it is acquired, however
  the buffer was written since (`glBufferSubData`, `glBufferData`, an invalidating map), so it lives as long
  as the buffer (`get_gl_mem`; `~/WineskinTests/d3d11tools/probe/clrename.c`, and in Bit.Trip Rerunner
  15800 buffers read through their memory object and a new one gave the same data). Making one costs a
  search through all of the share group's buffers, about 0.2 ms with Bit.Trip Rerunner's 30000.
- Constant buffers, and buffer views of buffers the CPU writes, reach kernels as copies, not as OpenGL
  buffers: Unreal Engine rewrites a constant buffer before nearly every dispatch, and dynamic buffers of
  64 KiB every frame. A buffer object of at most 64 KiB that a dispatch reads gets a mirror of its contents
  in system memory (`wined3d_bo.cl_mirror`), which `wined3d_context_gl_copy_bo_address` keeps current
  from the next write of the whole buffer on, and which a write by the GPU (an unordered access view,
  stream output) makes stale; the dispatch passes the mirror (`WINED3D_CL_ARG_HOST_DATA`) and `cl.c`
  copies it into a ring of host memory and writes that to a ring buffer OpenCL owns. A copy's memory stays untouched until a marker enqueued
  after its write has completed: a non-blocking `clEnqueueWriteBuffer` reads its source late, and from
  memory that changes meanwhile it gives wrong data in about one dispatch in twenty
  (`~/WineskinTests/d3d11tools/probe/clwrite.c`). Constants as a by-value kernel argument are a hundred
  times slower on the GPU (`clbyval2.c`).
- Every command buffer the driver submits (a `glFlush`, an OpenCL write, an acquire or release of OpenGL
  objects) is a call into the kernel, which in a frame of Rerunner costs 0.1-0.2 ms. Dispatches
  therefore go to `cl.c` as a batch (`wined3d_cl_dispatch_params.batch`): their copies, arguments and
  OpenGL objects are recorded, and one write, one `glFlush`, one acquire and one release serve the whole
  batch when it is enqueued (`unix_cl_flush`). OpenGL must not touch an object of an open batch, so the
  command stream enqueues it before any operation but those that set state, dispatch, issue a query, or
  draw with, clear, copy, write or map resources the batch does not use (`wined3d_cs_cl_before_op` in
  `cs.c`, with the resources in `wined3d_device.cl_batch_resources`; a draw's are its targets, vertex,
  index and stream output buffers and the views and constant buffers its shaders declare,
  `wined3d_cl_batch_uses_draw_resources`); a dispatch enqueues it before loading a resource
  through OpenGL or copying a texture the batch uses to its shadow, and the command stream thread before
  it waits for work. The copy of a shadow a batched kernel wrote back to its texture is made when the
  batch is enqueued (`cl_copy_back_shadows`); until then the texture is among the batch's resources.
  `WINESKIN_CL_NOBATCH=1` enqueues each dispatch on its own. Waits for the queue (`cl_finish_bounded`) look
  every few microseconds instead of blocking in `clFinish`; after `WINESKIN_CL_WAIT_MS` (2000) they log
  what they wait for, and go on, except before memory objects are released, where they keep waiting.
- NVIDIA's OpenCL on 10.9 has one coordinate mode per image in a kernel: once a kernel samples an image
  with normalised coordinates, a load from that image with integer coordinates, or without a sampler,
  returns texel (0, 0) at (0, 0) and zero elsewhere. Loads from resource views therefore address texel
  centres with normalised coordinates and a nearest filter (`wsmp_ldn` in `shader_cl_ld`). Unreal's
  motion blur loads and samples one texture, and draws black where it only loads.
- NVIDIA's OpenCL on 10.9 reads an `image2d_depth_t` at the wrong texel through a sampler with normalised
  coordinates (it reads them as texel indices); an `image2d_t` of the same OpenGL depth texture reads the
  depth into x at the right texel. Sampled depth views are therefore declared as colour images
  (`shader_cl_image_type`). Unreal builds its hierarchical Z buffer with such reads. A depth format is
  shared only if a kernel reads back, this way, the depth OpenGL cleared a texture to
  (`test_depth_read`); Intel's OpenCL returns 0 for every shared depth texture, and there depth reaches
  kernels through an R32F shadow. `WINESKIN_CL_NO_DEPTH=1` shares no depth format.
- A typed buffer view of a format of 32-bit components is read and written as words: the kernel key
  holds its component count (`WINED3D_CL_BUFFER_KEY_32`). The path for any format (`wfmt_ld`, `wfmt_st`)
  is large, and the driver's compiler gives up on a kernel that has hundreds of them ("Error getting
  function data from server", after two minutes during which the game stands still).
- The driver's compile server caches builds by source text, so a kernel's source is the same in every
  run; its first line names the shader by a hash of its byte code. A first run compiles for about half a
  minute in total, spread over the first minutes of play. A kernel builds on a thread of its own, and the
  command stream waits for it at most `WINESKIN_CL_BUILD_WAIT` seconds (10; 0 waits for every build).
  A slower build goes on in the background, and the kernel's dispatches do nothing until it has finished;
  a failed build is logged with its build log, and its dispatches do nothing.
- Debugging aids: `WINESKIN_CL_DUMP=<directory>` receives every kernel's source;
  `WINESKIN_D3D_FRAME_DUMP=<Windows path of a directory>` captures a frame when a file named `go` (or
  `go-indirect`, for the frame after an indirect dispatch) appears there: `frame.txt` lists the draws and
  dispatches with shaders, bound textures, samples passed, OpenGL state and errors, and each pass's render
  targets, each dispatch's unordered access views, shadow textures and buffers are written as images and
  files (`frame_dump_gl.c`). `~/WineskinTests/d3d11tools/probe/clrun.c` runs a dumped kernel natively with
  those files as inputs, and `clbuild.c` times a kernel's build. `WINESKIN_CL_STATS=1` prints the time
  each part of a dispatch takes, the batches, memory objects made, the operations that end batches and
  what draws that end one share with it. `WINESKIN_CS_STATS=1` prints, every 300 presents, the command
  stream thread's milliseconds per frame in each kind of operation, in enqueuing batches, in polling
  queries and in waiting for work, the kilobytes of map shadows uploaded, and names copies that take
  over half a millisecond.
- `HKCU\Software\Wine\Direct3D\OpenCLCompute` = `n` (or `WINE_D3D_CONFIG=OpenCLCompute=n`) turns it off;
  dispatches then do nothing. `WINEDEBUG=+d3d_cl` logs each kernel's source and build log.

#### Dynamic buffers without persistent mapping

wined3d's command stream thread makes every OpenGL call; the thread a game renders from only queues
commands. With `ARB_buffer_storage` wined3d keeps buffers mapped and the game's thread writes into them
directly. 10.9 has no persistent mapping, so upstream's code makes each `Map(DISCARD)` or
`Map(NOOVERWRITE)` wait for the command stream to reach the buffer's last use and map it there, and
replaces a buffer the GPU still reads with a new OpenGL buffer. Two files change that:

- `dlls/wined3d/map_shadow.c`: a `DISCARD` or `NOOVERWRITE` map of a dynamic buffer of at most 16 MiB
  returns system memory at once, the buffer's *shadow*, and the unmap queues its contents. The command
  stream writes them with `glBufferSubData` into the same OpenGL buffer, which 10.9's driver copies into
  its own command stream: per update and draw that takes 0.2 microseconds, against 1 for mapping a spare
  buffer and 7 for creating a buffer and deleting the old one (`~/WineskinTests/perf/tools/ubobench.c`
  measures it). A map's writes are not known (Direct3D 10 and later map whole buffers), so the shadow
  of a buffer of at most 64 KiB, a constant buffer typically, is written whole. The shadow of a larger
  buffer, such as an engine's vertex or index scratch buffer to which each map appends a few kilobytes,
  is write-watched memory (`VirtualAlloc` with `MEM_WRITE_WATCH`), and the unmap queues the pages written
  since the previous one. A page's first write costs a fault, about 7 microseconds
  (`~/WineskinTests/perf/tools/wwbench.c`); a map through the command stream costs the wait for the
  draws queued before it, and Ori and the Will of the Wisps makes 32 such maps a frame.
  `HKCU\Software\Wine\Direct3D\MapShadows` = `n` (or `WINE_D3D_CONFIG=MapShadows=n`) turns shadows
  off, `small` the write-watched ones.
- `dlls/wined3d/bo_pool_gl.c`: an OpenGL buffer that is replaced or destroyed goes to a pool, and a new
  buffer of the same size and usage takes one from it once the GPU has finished with it, instead of
  `glGenBuffers`/`glDeleteBuffers` each time. A reused buffer keeps its name and its OpenCL memory
  object.

#### Constant buffers as uniform arrays

On 10.9's NVIDIA driver a program switch costs much more when the programs read uniform blocks. Switching
between two of Ori and the Will of the Wisps' programs costs 5.4 microseconds of CPU time per draw and stalls
the GPU for 9.7; with the same programs reading plain uniform arrays it costs 3.3 and 1.0
(`~/WineskinTests/perf/tools/drawcost2.c`, which replays a game's GLSL natively). One block is enough. The
blocks' size and number, the buffers' usage and binding ranges, and program pipelines change nothing.

So a shader model 4 or 5 vertex or pixel shader declares a constant buffer as `uniform uvec4 vs_cb0[n]` and
reads it through `uintBitsToFloat()` (`dlls/wined3d/cb_uniforms_gl.c`): unsigned uniforms keep every bit, where
float ones may lose NaN payloads and the denormals that integer constants are as floats. This applies while the
stage's constant buffers fit its share of a program's uniform storage: a program whose vertex and fragment
shaders' plain uniforms come to more than 512 vectors together has its vertex processing run on the CPU
(`~/WineskinTests/perf/tools/vpfallback.c`; one stage alone may use 1024), so each stage takes 256, less a
reserve for wined3d's own uniforms. A constant buffer that the shader indexes dynamically stays a uniform block,
because Direct3D reads past its declared size as far as the bound buffer goes; so do those that don't fit.
Before each draw, the program loads each such constant buffer whose binding, range or contents changed since it
last loaded it, with `glProgramUniform4uiv`. The contents come from the system memory copy of the buffer object
(`struct wined3d_bo`'s `cl_mirror`, which OpenCL dispatches use too). A buffer object bound as a constant buffer
gets one when it is created, and every write from the CPU keeps it current: updates from system memory, maps
(their written ranges are copied from the mapping at the unmap), copies from a buffer object whose copy is
current, and the new buffer object that takes a busy one's place on a DISCARD map. Only after a GPU write is the
buffer read back. A bound range shorter than the declared array is padded with zeros. The stream output emulation
program loads its vertex shader's constant buffers the same way. It is off unless
`HKCU\Software\Wine\Direct3D\ConstantBufferUniforms` = `y` (or `WINE_D3D_CONFIG=ConstantBufferUniforms=y`).

With it, Bit.Trip Rerunner's title runs at 71 frames a second against 61; without the copies kept through those
writes it reads 44,000 buffers back (59.5), and without the per-stage share its vertex processing runs on the CPU
(6 frames a second, black). Neon White, Thumper and Ori and the Will of the Wisps' title show no difference; Ori's
gameplay has not been measured reliably with it yet, so it stays off by default.

#### The cost of an OpenGL call, and the state filter

A 64-bit Windows thread keeps its TEB in `%gs`, which macOS's libraries use for their own thread data.
Upstream Wine switches the `%gs` base on every call from Windows code into a Unix library and back; 10.9
has no user-mode `wrgsbase`, so each switch is a kernel trap, and a call costs about 0.5 microseconds
before it does anything (`~/WineskinTests/perf/tools/ucall.c` measures it). wined3d makes one such call
per OpenGL function, tens per draw. 32-bit programs keep their TEB in `%fs` and pay nothing.

Here `%gs` keeps the pthread base while Windows code runs, as in CrossOver and in marzent/winecx
(commit 0a3e723e47, the source of the change), and a call costs 0.15 to 0.2 microseconds. Compiled
Windows code reads three things straight from `%gs`, and each thread's pthread data holds them at the
same offsets (`wineskin_init_teb_slots` in `dlls/ntdll/unix/signal_x86_64.c`): the TEB at `%gs:0x30`,
the TLS pointer at `%gs:0x58`, the PEB at `%gs:0x60`. Code that inlines `TlsGetValue()` reads
`%gs:0x1780`, the TLS expansion slots, which every thread has from the start; the 64 TLS slots inside
the TEB are never handed out, so `TlsAlloc()` returns indices from 64 up. What this rests on in 10.9's
libpthread and libc, checked in their disassembly: a thread's `%gs` base is its array of 768 pointers;
slot 6 (`0x30`) belongs to nobody; slots 11 and 12 (`0x58`, `0x60`) are libc's keys for the buffers of
`ttyname()` and `localtime()`, which the Unix loader replaces with versions that use no key, and which
are cleared before a thread exits, since libc frees what it finds there; slot 752 (`0x1780`) is an
ordinary pthread key, which the loader reserves at startup. A program that reads any other TEB field
straight from `%gs` (the stack bounds at `0x8` and `0x10`, the last error at `0x68`, a TLS slot at
`0x1480`) reads pthread data instead; `WINESKIN_GSBASE_SWITCH=1` in the environment gives such a
program upstream's switching. A program that loads the `%gs` selector loses the base until its next
fault, where the signal handler restores it.

`QueryPerformanceCounter` makes no such call in a 64-bit program: `RtlQueryPerformanceCounter`
(`dlls/ntdll/time.c`) scales the TSC with the nanotime data of macOS's commpage, as
`mach_absolute_time()` does, once the Unix side (`wineskin_qpc_params` in `unix/sync.c`) has checked
that this gives the same clock. It takes 19 ns.

`dlls/wined3d/gl_filter.c` replaces the state setting entries of wined3d's OpenGL function table
(`glEnable`/`glDisable`, texture, sampler and uniform buffer bindings, vertex attribute divisors, face,
depth and colour mask state) with functions that remember, per OpenGL context, what was last set and
return without calling OpenGL when nothing changes. wined3d applies Direct3D state an object at a time,
so a game that sets the same state before every draw otherwise has it reissued: on Ori and the Will of
the Wisps' title screen the filter takes 71 OpenGL calls per draw down to 31. Deleting an object forgets
the bindings that name it, in every context. `HKCU\Software\Wine\Direct3D\GLStateFilter` = `n` (or
`WINE_D3D_CONFIG=GLStateFilter=n`) turns the filter off.

A disabled vertex attribute array keeps pointing into the buffer it last read, and 10.9's OpenGL brings
every buffer a vertex array object names up to date before each draw. When the GPU has written that
buffer since (stream output, an OpenCL kernel), each draw submits the command buffer to the kernel, used
or not: a game that skins meshes on the GPU and draws its full-screen passes without vertex buffers paid
that 77 times a frame. wined3d points disabled arrays at no buffer
(`wined3d_context_gl_drop_stale_attrib_pointers`).

`glEndTransformFeedback` and `glPauseTransformFeedback` wait until the GPU has finished every command
submitted before them, in any context of the share group (`~/WineskinTests/perf/tools/xfbstall.c`), so a game
that uses stream output once a frame runs its CPU and GPU work one after the other. `dlls/wined3d/so_emu_gl.c`
runs the common case without transform feedback: a draw of a list of points, lines or triangles whose vertex
shader outputs go to one stream output buffer, with no geometry shader of the game's and no rasterization
(Unity's GPU skinning). A geometry shader writes the bits of each captured component to a pixel of a
4096-wide `R32UI` renderbuffer, in the order they take in the buffer, and `glReadPixels` copies the rows
into the stream output buffer bound as the pixel pack buffer; both are queued, and nothing waits. Other
stream output uses transform feedback. `HKCU\Software\Wine\Direct3D\StreamOutputEmulation` = `n` (or
`WINE_D3D_CONFIG=StreamOutputEmulation=n`) turns it off.

The multithreaded OpenGL engine (`kCGLCEMPEngine`) is on for every context: the driver's work for each
call runs on a thread of its own, and the thread that makes the calls, wined3d's command stream, only
queues them. It speeds up programs bound by the driver's work (Ori and the Will of the Wisps' title
79 -> 93 fps, Bit.Trip Rerunner's 41 -> 50) and costs a few percent in light ones running at hundreds
of frames a second. `HKCU\Software\Wine\Mac Driver\MultithreadedOpenGL` = `n` turns it off. Calls
that ask OpenGL for something wait for that thread, which is why winemac remembers whether the surface
backing size is enabled instead of asking at every buffer swap (`backingSizeEnabled`).

10.9's OpenGL offers no program binary formats (`GL_NUM_PROGRAM_BINARY_FORMATS` is 0), so linked shader
programs can't be kept on disk: a shader is compiled the first time each run draws with it.

An OpenGL context made current before its window is on screen waits for the view's next `-viewWillDraw`;
such a context also attaches the next time it is made current with its window visible
(`macdrv_attach_pending_view_opengl_context`).

### Other GPUs

The work above was done on an NVIDIA GeForce GTX Titan Black. `~/WineskinTests/probe-air` holds a
probe that measures what it relies on with another Mac's drivers (a native program, run by double-
clicking "Run Probe.command"; it writes a report to the Desktop), and reports from the Titan and a 2014
MacBook Air with Intel HD Graphics 5000 on 10.9. What wined3d checks, and what it does without:

- OpenCL compute needs the OpenCL device CGL names for the OpenGL context to be a GPU with images,
  OpenCL C 1.2 and OpenGL sharing (`device_usable` in `cl.c`; a GPU without OpenCL, such as Intel HD
  Graphics 3000, can have the CPU named), and the shadow formats (R32F, RGBA16F, RGBA32F, RGBA32UI,
  RGBA32I) to share. Without them one message says so and compute dispatches do nothing; the feature
  level stays 11_0, as with `OpenCLCompute` = `n`. Shared formats, depth reads and build times are
  above. Kernels that write 3D images need `cl_khr_3d_image_writes` (Intel's OpenCL lacks it), and
  kernels with multisample images `cl_khr_gl_msaa_sharing` (NVIDIA's only); without the extension the
  shader's dispatches do nothing, with a FIXME (`shader_cl_kinds_supported` in `glsl_cl.h`).
- What the OpenCL path does for NVIDIA's quirks is valid OpenCL on any device and runs everywhere:
  loads at normalised texel centres (exact with Intel's OpenCL too), and 32-bit typed buffer views read
  as words. So is the conversion of `glClear` colours for sRGB views: Intel's driver clears either way.
- Intel's OpenGL ignores `GL_TEXTURE_SRGB_DECODE_EXT` on a sampler object; the texture's parameter
  decides. A draw at adapter initialisation finds this (`wined3d_gl_sampler_srgb_decode_works` in
  `texture_copy_gl.c`, quirk `WINED3D_QUIRK_SAMPLER_SRGB_DECODE_IGNORED`), and Direct3D 9's sRGB
  sampler state then goes into the texture's parameter. Views of Direct3D 10 and later set the
  texture's parameter in any case, and copies read with `texelFetch()`, which the texture's parameter
  governs.
- macOS has no `ARB_internalformat_query`: a format's multisample counts are the powers of two up to
  `GL_MAX_INTEGER_SAMPLES` for integer formats (1 with NVIDIA's and Intel's drivers: none),
  `GL_MAX_DEPTH_TEXTURE_SAMPLES` for depth and `GL_MAX_COLOR_TEXTURE_SAMPLES` for the rest
  (`query_internal_format` in `utils.c`).
- Stream output emulation needs a geometry shader and an `R32UI` render target of 4096 texels a row;
  when its program doesn't build or its target is incomplete, the draw uses transform feedback. Intel's
  `glReadPixels` into the buffer waits for the GPU work queued before it (6.5 ms, against 9.8 ms for
  `glEndTransformFeedback`), so the emulation gains less there.
- View copies and texture copies need GLSL 3.30 and sampler objects (OpenGL 3.3), buffer conversions
  also `ARB_transform_feedback2`; without them wined3d's own paths run. Buffer textures lack RGBA8_SNORM and RGB10A2 with both drivers,
  which `buffer_convert_gl.c` covers.
- Memory: the pool of retired OpenGL buffers holds at most an eighth of the video memory (Intel HD
  Graphics 5000 reports 1536 MB, shared with the system), and no more than 256 MiB. Map shadows are
  system memory, at most 16 MiB a buffer; shadows and view copies are textures of their texture's size,
  made only for textures a dispatch or a view needs.
- Timer queries read the CPU clock (Intel's `GL_TIME_ELAPSED` reads 0 anyway). The per-draw cost of
  Intel's driver is higher: 3.3 microseconds for a draw after 8 texture changes, 4.0 after a program
  change (Titan: 1.5 and 2.6).

The checks at initialisation (the sRGB decode draw on the capability context, the OpenCL format and depth
checks on the command stream's context) compare the OpenGL state they could touch before and after, and
an error names any state they leave changed: the format checks that follow the sRGB check draw with the
capability context's vertex array. `WINESKIN_NO_DRIVER_CHECKS=1` skips them (every format then counts
as shared with OpenCL), and `WINEDEBUG=+d3d_formats` prints every format's capabilities;
`~/WineskinTests/probe-air/capsdiff.sh` runs a d3d11 test both ways and compares the two, which must be
the same on a Mac whose drivers need none of the workarounds.

Only seen on other hardware: AMD GPUs (none probed yet); whether games' compute shaders read depth
correctly through the shadow on Intel (the probe's copy does); Intel HD Graphics 3000 (OpenGL 3.3, so
shader model 4 and feature level 10_0 at most, and no OpenCL on its GPU).

## Synchronisation

`RtlWaitOnAddress`, on which SRW locks, condition variables and most engines' own locks sleep, hashes
addresses into 1024 queues and its queue lock yields after 64 spins (`dlls/ntdll/sync.c`, from
marzent/winecx).

### In-process synchronisation (msync)

Every wait on, and every release of, a semaphore, event or mutex is otherwise a wineserver round trip,
and the server is one thread for the whole prefix. With `WINEMSYNC=1` in the environment the wineserver
starts in (off unless set), these objects live in memory the server shares with every process, and a
release or an uncontended wait is a few atomic operations in the calling thread. The server decides: a
process uses msync exactly when its server does, whatever its own environment says.

- The code is CrossOver 26.3.0's msync (`server/msync.c`, `dlls/ntdll/unix/msync.c` and their headers;
  CodeWeavers, LGPL 2.1+, the successor of https://github.com/marzent/wine-msync), which plugs into
  Wine's in-process synchronisation (`server/inproc_sync.c`, `dlls/ntdll/unix/sync.c`) where Linux has
  ntsync; `msync_inproc.h` on each side is that glue. The object types are ntsync's: semaphores, events,
  mutexes, and the internal events through which the server signals everything else a thread can wait
  on (threads, processes, timers, queues, a thread's user APCs).
- CrossOver's msync sleeps and wakes through the ulock system calls of macOS 10.12. 10.9 has none, so a
  thread that has to sleep waits on a Mach semaphore of its own: it sends the semaphore to the server's
  Mach port once (a bootstrap service named after the prefix), registers each wait there with one
  message, and the server's message thread signals the semaphore when a registered object is signaled
  or already is. A release with no sleeper sends nothing; one with sleepers sends one message and does
  not wait for it. The ulock paths remain for systems that have the calls; they are untested here.
- The protocol is upstream's: an object's index in the shared memory travels in the type field of the
  `get_inproc_sync_fd` reply and in the handle of the `get_inproc_alert_fd` reply.
- Differences from CrossOver's files are marked `WINESKIN` in them: `NtReleaseMutant` reports NT's
  previous count; a signaled object comes before a pending user APC, as in the server; a mutex whose
  handles are closed while a thread waits on it is still abandoned when its owner dies; long timeouts
  don't overflow; thread exit doesn't close the alert object's index as a file descriptor.
- Known limits, inherited: a process that dies leaves its references to objects' shared memory entries
  allocated; `PulseEvent` can miss a waiter; relative timeouts follow the wall clock.

`~/WineskinTests/perf/tools/synctest.c` exercises timeouts, mixed and wait-all waits, APCs, named,
inherited and duplicated objects across processes, abandoned mutexes and killed threads, with a time
limit on every wait.

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
  "MS Shell Dlg 2", and dialog templates size their controls from it. Its TaskDialog panels are the plain
  window background of a Mac alert; Qt's Windows style paints message boxes and input dialogs with them,
  and a theme without them leaves those dialogs black.
- **Message boxes.** `MessageBox` lays its dialog out as OS X 10.9 lays out an alert (`user32/msgbox_aqua.c`):
  a 64-pixel icon at the left (the alert type's, a `MessageBoxIndirect` custom icon, or the program's own;
  a program's icon at its own size, never scaled up),
  the text's first paragraph in bold 13-point type and the rest in 11-point type, the default button
  rightmost with Cancel or No to its left and any other buttons at the left, and no title or close button
  in the title bar (`__wine_mac_untitled`, honoured by winemac; the window text is still the caption). It
  stays a Win32 dialog: its controls, IDs, hooks, keyboard handling and Help button are Windows'. It runs
  in an activation context that asks for comctl32 6 (`msgbox_aqua.manifest`), so its buttons are the themed
  ones, the default one blue, in any program.
- **Alert icons.** Windows' stock alert icons (`IDI_HAND`/`IDI_ERROR`, `IDI_EXCLAMATION`/`IDI_WARNING`,
  `IDI_ASTERISK`/`IDI_INFORMATION`, `IDI_QUESTION`) are the Mac's stop, caution and note icons wherever
  user32 loads them (`LoadIcon`, `LoadImage`, message boxes, task dialogs): user32 asks winemac.drv's
  `wineskin_system_icon()` (`include/wine/wineskin_system_icon.h`, `winemac.drv/cocoa_system_icon.m`) to draw
  the system's own icns from CoreTypes.bundle at the size it loads, so they are sharp at any size. OS X has
  no question icon; `IDI_QUESTION` is the note icon.
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
- **Access keys.** Controls and in-window menus hide their access key underlines ("&High"), as Windows
  does with its default keyboard cues setting (`SPI_GETKEYBOARDCUES` off), and show them while the Alt key
  is held (Option, with "Option keys act as Alt"). win32u keeps each window's UI state
  (`WM_CHANGEUISTATE`, `WM_UPDATEUISTATE`, `WM_QUERYUISTATE`, in `win32u/defwnd.c`): a top-level window
  starts with underlines and focus rectangles hidden, a child takes its parent's state, and Tab or an
  arrow key in a dialog shows focus rectangles. Pressing Alt shows the underlines and releasing it hides
  them (`process_keyboard_message` in `win32u/message.c`); a menu bar the keyboard drives shows them until
  it closes, and a bare Alt or F10 opens no system menu, which a Mac title bar has no button for. Buttons,
  check boxes, radio buttons, group boxes, static text, tabs and menus draw with `DT_HIDEPREFIX` (which
  `user32/text.c` honours) while the state hides them, and repaint when it changes.

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
`win32u/sysparams.c`).

A program's gamma ramp (`SetDeviceGammaRamp`, the gamma controls of DirectDraw and Direct3D, a game's
brightness slider) applies to the main display while the program is the active app, emulated modes or
not; when another app is active, when the ramp is the identity and when the program quits, the display
has its ColorSync settings (`show_program_gamma` in `winemac.drv/cocoa_display.m`).

A visible top-level window whose content covers a screen, as a fullscreen game's does, goes full screen
in a Space of its own, as with the green button (`-[WineWindow wine_updateFullScreenSpace]`,
`HKCU\Software\Wine\Mac Driver\FullScreenSpace`, on unless set `n`):

- It enters once it has covered the screen for 0.4 seconds, since engines resize and recreate their
  windows while they start, and only while the app is active on that screen's Space. It leaves once
  Windows has made it stop covering the screen for 0.4 seconds (the game switches to windowed), and at
  once when it is hidden or gains an owner window it moves with. A window the user takes out of its Space stays out until it
  stops covering the screen.
- Its Win32 rect stays what the program set; frame changes Cocoa makes for the Space are not posted, and
  the minimum and maximum sizes a program gives its window don't limit it there.
- The menu bar and Dock stay hidden in the Space, even with the pointer at the top or bottom edge.
  Hot corners still act there.
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

DirectShow's default video window (quartz's VMR-7 and its default presenter) shows each frame by a
blit to a DirectDraw primary surface clipped to the window, which wined3d draws into the window's
OpenGL surface. When winemac attaches an OpenGL context to a view again after 10.9 dropped the view's
surface (the window or view was hidden and shown), it moves the context to an off-screen view and back
(`-[WineOpenGLContext wine_reattach]`): a context whose drawable was cleared and then set to the same
view draws into a surface that doesn't follow the view, at the screen's top-left corner and scaled down.

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

A wrapper is named by its file name, and identified by its program. The blank wrapper has no
`CFBundleIdentifier`; once a program is chosen, the launcher writes one made from the program's file name
and a hash of its path in the prefix (`com.wowfunhappy.wfhwineskin.<program>-<hash>`), so no two wrappers
share an identifier and LaunchServices never opens one wrapper for another. When the program changes,
the identifier follows: the launcher rewrites Info.plist, registers the bundle, says "<name> was updated.
Please open it again." and quits. On the first launch after a copy or rename, it writes the new
`CFBundleName`, registers the bundle and relaunches once, so the menu bar, Cmd-Tab, Keep in Dock and
Recent Items show the wrapper's name.

Each user's Desktop folder in the prefix is a directory of its own, not Wine's link to the Mac's
Desktop, so the shortcuts installers leave there stay out of Finder. The launcher makes it so when it
creates the prefix and before each run.

A program's AppData folder lives in the wrapper's own folder in `~/Library/Application Support` (named
after the wrapper the first time, kept in `Wineskin.plist` as `DataFolder`), where settings and saved games
outlast the wrapper; the prefix's `AppData` is a link to it. Documents can live there too, instead of being
Wine's link to the Mac's Documents. "AppData in Application Support" (on) and "Documents in Application
Support" (off) in the configuration window choose; turning AppData off moves it back into the prefix, and
turning Documents off links the Mac's Documents again, leaving what the program saved where it is.

Each wrapper carries its own engine and is independent of every other. `wrapper/Update Existing
Wrappers.command` brings wrappers up to this build: it replaces a wrapper's engine and launcher with
those of `build/WFH Wineskin.app`, keeps its Windows environment, settings, name and icon, and has Wine
update the prefix (`wineboot --update`), which refreshes the files a prefix copies from its engine: the
visual style, placeholder DLLs and registry defaults. Run it with no arguments to choose wrappers in a
dialog, with wrapper paths, or with `--update-apps-folder` for every wrapper in /Applications.

The bundle's icon is the program's own, so Finder shows what the Dock shows while it runs. When the
program changes (chosen in the configuration window, or found changed at launch; `IconProgram` in
`Wineskin.plist` records which program the icon came from), `MatchBundleIcon` reads the program's first
icon group from its resources and writes it as `Contents/Resources/Wineskin.icns`. A program without an
icon gets `Blank.icns`, the blank wrapper's.

Opening a wrapper with no program, or with Option held, shows the configuration window: install
software, choose the program and its arguments, Wine's tools (configuration, registry, command prompt,
task manager), and the Mac driver options, among them "Full screen in its own Space" (see "Full screen
and display modes").
