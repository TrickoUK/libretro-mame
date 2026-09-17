# CLAUDE.md

This file guides Claude Code when working in this repository.

## What this repo is

A fork of [libretro/mame](https://github.com/libretro/mame) (upstream MAME with a
`libretro` OSD grafted on), pushed to `TrickoUK/mame`. It's MAME itself
(`src/mame`, `src/devices`, `src/emu`) plus a libretro frontend layer in
`src/osd/libretro`. Regular upstream MAME merges land as commits like
`Merge tag 'mame0289'...` — expect large, unrelated diffs from upstream syncs
mixed into history.

**Project goal for this fork**: improve 3D rendering for specific
arcade drivers by offloading polygon rasterization to the host GPU, instead of
MAME's normal fully-software rendering. This is a nontrivial architecture
change, not a shader tweak — see "The core problem" below before starting.

## Repo layout cheat sheet

- `src/mame/<manufacturer>/*.cpp` — per-driver code, organized by manufacturer
  (e.g. `sega/`, `namco/`, `nintendo/`, `konami/`, `williams/`), not by game
  name. A driver's video/rasterizer code is usually `<name>_v.cpp` or `<name>_video.cpp`.
- `src/devices/video/` — shared reusable video chip emulation (e.g. `voodoo*.cpp`).
- `src/emu/` — core emulation framework: `screen.cpp`/`screen.h` (per-driver
  update callbacks), `render.cpp`/`render.h`/`rendlay.cpp` (backend-agnostic
  render primitive compositor).
- `src/osd/` — platform frontends. `sdl/`, `windows/` are the standalone-MAME
  OSDs (with BGFX/HLSL/D3D/OpenGL draw backends). `src/osd/libretro/` is
  **the one this fork builds and the one relevant to this project.**
- `bgfx/`, `hlsl/` (top-level dirs) — shader chains for the *standalone* SDL/Windows
  OSD's post-processing (CRT effects, scanlines, etc). **Not used by the
  libretro build** — see below.
- `Makefile.libretro` — the build entry point for this fork.
- `scripts/src/osd/retro.lua`, `retro_modules.lua` — GENie build config for the
  libretro OSD; what gets compiled in/out of the libretro core.

## The core problem: this build is 100% software-rendered

Before writing any GPU-acceleration code, understand this pipeline — it explains
why "just enable BGFX/OpenGL" isn't a fix:

1. A driver's `screen_update_rgb32`/`ind16` callback (declared via
   `screen_device::set_screen_update`) fills an in-memory `bitmap_rgb32` purely
   in software. **This is where 3D drivers rasterize polygons** — Model 2, Model 3,
   Namco System 22/23, N64's RDP, Voodoo-based games, etc. all do this in C++ on
   the CPU, no GPU involved at any point.
2. `screen.cpp` hands that finished bitmap to `render.cpp`, which composites it
   (plus artwork/bezels/overlays) into a `render_primitive_list` — a flat,
   backend-agnostic list of textured quads.
3. An OSD draw module consumes that primitive list. In the standalone SDL/Windows
   OSD this can be `drawbgfx.cpp`/`drawd3d.cpp`/`drawogl.cpp`, which use the GPU
   — but **only to blit the already-rasterized 2D image and apply post-processing
   shaders** (CRT/scanline/HLSL effects from `bgfx/`, `hlsl/`). They never receive
   3D geometry from the driver; by the time they run, the 3D scene is already a
   flat bitmap.
4. **This fork's OSD (`src/osd/libretro/drawretro.cpp`) doesn't even do that much:
   it uses `rendersw.hxx`, a pure software blitter, to hand a raw RGB buffer to
   libretro's `video_cb`.** There is no GPU compositing step at all in the current
   libretro build, let alone GPU-side 3D rasterization.

So GPU acceleration of a driver's 3D rendering requires **replacing that
driver's software rasterizer** (e.g. `src/mame/sega/model2_v.cpp`) with one that
issues real GPU draw calls, *and* giving the libretro core a way to present a
GPU-rendered frame to RetroArch instead of a CPU pixel buffer. Both pieces are
currently missing/dead in this fork:

### Finding: dead OpenGL hardware-render scaffolding

`src/osd/libretro/libretro-internal/libretro.cpp` already has the skeleton of a
libretro `RETRO_HW_RENDER` integration, but it's inert:

```c
// retro_load_game(), ~line 938
//FIXME: re-add way to handle OGL
#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGL; // or OPENGLES2
   hw_render.context_reset = context_reset;
   hw_render.context_destroy = context_destroy;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
      return false;
#endif
```

`HAVE_OPENGL`/`HAVE_OPENGLES` are **never defined anywhere** in
`Makefile.libretro` or `scripts/src/osd/*.lua` — confirmed by grep — and the
`#include "retroogl.c"` at the top of the same file (line ~72) points to a file
that **does not exist** in this tree. So today the `#else` branch always runs:
every frame goes out via `video_cb(videoBuffer, ...)` as a plain software
buffer, and RetroArch is never asked for a GL context
(`RETRO_ENVIRONMENT_SET_HW_RENDER` is never called).

**This is the natural starting point**: reviving `context_reset`/`context_destroy`,
defining `HAVE_OPENGL`, writing a real `retroogl.c`/`.cpp`, and wiring
`RETRO_ENVIRONMENT_SET_HW_RENDER` + `RETRO_HW_RENDER_INTERFACE` is prerequisite
plumbing before any driver's rasterizer can actually hand triangles to the GPU
and get them composited into what RetroArch displays.

### Finding: the debugger is stubbed out in this build

`scripts/src/osd/retro_modules.lua` wires `src/osd/modules/debugger/none.cpp`
(not the Qt/Windows GUI debugger) and forces `USE_QTDEBUG=0`. **MAME's graphical
debugger is not available in libretro builds.** Instead, this fork has been
wired up (2026-09-16) to autostart MAME's Lua console/plugin system via
RetroArch core options — see "Debugging / monitoring progress" below.

## Candidate drivers for GPU-offloaded 3D rendering

### Chosen first target: Sony ZN / PS1 GPU (`psxgpu_device`) — Brave Blade

Decided 2026-09-16 after successfully running it as the baseline smoke test.
**Game**: Brave Blade (Eighting/Raizing, 2000), romset `brvblade` (World) —
verified booting and rendering in RetroArch via this fork's core (see the
2026-09-16 session: `Geometry: 640x480`, GL context created, no crash; one
non-fatal warning about a missing/undumped ROM chip `78081g503.ic655`,
unrelated to the core itself). Driver: `src/mame/sony/zn.cpp` (Sony ZN
hardware = arcade board wrapping the actual PS1 GPU chip, `CXD8561Q`/`CXD8654Q`
instantiated via `m_gpu` in `zn.cpp`).

**The actual rasterizer to replace is not in `zn.cpp`** — it's the shared
device `src/devices/video/psx.cpp` (`psxgpu_device`, ~3559 lines + 358-line
header). This is the real Sony PlayStation 1 GPU emulated in software: a
single self-contained class with clearly separated primitive-drawing
functions — `FlatPolygon`, `FlatTexturedPolygon`, `GouraudPolygon`,
`GouraudTexturedPolygon`, `MonochromeLine`, `GouraudLine`, `FlatRectangle`
(+8x8/16x16 variants), `Sprite8x8`/`Sprite16x16`, `Dot`/`TexturedDot` — all
writing into an internally-simulated VRAM buffer, which `update_screen()`
(the `screen_update_rgb32`-style callback, registered at
`psx.cpp:3558`) then presents each frame. This matches the general pipeline
described above exactly, just packaged as a reusable device instead of
being embedded directly in one driver file.

**Why this is high-leverage**: `psxgpu_device` is shared by far more drivers
than any other candidate here — `src/mame/konami/konamigq.cpp`, `konamigv.cpp`
(`nagano98`, in the ROM collection), `ksys573.cpp`, `twinkle.cpp`,
`src/mame/namco/namcos10.cpp`, `namcos11.cpp` (`starswep`, in the ROM
collection), `namcos12.cpp`, `src/mame/sony/psx.cpp` (the plain PS1 console
driver itself), `src/mame/sony/taitogn.cpp`, `src/mame/skeleton/pap2.cpp`,
`src/mame/tomy/kisssite.cpp`. A working GPU path here benefits every game on
every one of these boards, not just Brave Blade — easily the biggest
multiplier of any candidate in this list.

**Test/verify command** (established working baseline):
```sh
distrobox enter mame-dev -- retroarch -v -L /var/home/bazzite/Projects/libretro/mame/mame_libretro.so /var/home/bazzite/Projects/mame-roms/roms/brvblade.zip
```

### Architecture decision: private EGL context, not `RETRO_HW_RENDER` (Phase 1 done, 2026-09-16)

The dead `RETRO_HW_RENDER` scaffolding above turned out to be a dead end
worth avoiding, not reviving: investigation for the implementation plan
found (a) `hw_render`/`context_reset`/`context_destroy`/`do_glflush` are
referenced *nowhere else* in the tree — this was never functional, building
it out means writing a full negotiated-context integration with RetroArch's
own frontend from scratch — and (b) MAME's own render pipeline already
tolerates a screen reporting a bitmap/resolution bigger than its declared
native size with **no core changes needed** (`render_texture::set_bitmap()`
only requires the bitmap contain the visible area; the libretro OSD's own
`compute_minimum_size()`-driven geometry reporting in `window.cpp` already
re-derives `fb_width`/`fb_height` from the screen's live `visible_area()`
every frame and tells RetroArch automatically). So a shared/negotiated GL
context with RetroArch isn't needed at all.

**Chosen instead**: `psxgpu_device` (and any future driver) can request a
**private, headless EGL/OpenGL context**, completely independent of
whatever context RetroArch's own frontend holds — verified safe to create
and repeatedly recreate *inside the same process* as a live RetroArch
session without any interference (15/15 iterations over 30s while Brave
Blade ran, see the plan's Phase 0 status). Render real geometry into an
off-screen FBO at N× resolution, read it back into a normal (now-larger)
`bitmap_rgb32`, let MAME's existing compositor and the OSD's existing
geometry reporting handle the rest unmodified.

**What was built** (Phase 1 of
`/home/bazzite/.claude/plans/glowing-conjuring-raven.md` — read that file
for full phase-by-phase detail):
- `src/osd/interface/gpurender.h` — the generic interface
  (`osd::gpu_render_target`: `begin_frame`/`upload_texture`/
  `submit_triangle`/`end_frame_and_readback`), vertex-format-agnostic
  (position/color/UV triangles only).
- `osd_interface::get_gpu_render_target()` — new pure-virtual accessor
  (`src/osd/osdepend.h`), with a shared nullptr-returning default in
  `osd_common_t` (`src/osd/modules/lib/osdobj_common.h`) so **every other
  OSD (SDL/Windows) needed zero changes**.
- `src/osd/libretro/libretro-internal/retro_gpu_target.{h,cpp}` — the real
  EGL-backed implementation, only for the retro OSD. **EGL/GL are loaded
  entirely via `dlopen()`/`dlsym()` at runtime, not linked at build time** —
  this build host has no EGL/GL `-devel` packages (layering them on this
  atomic/immutable desktop needs a reboot), so there are no new build-time
  header/library dependencies at all; the feature just degrades to
  unavailable (`is_valid() == false`) on any system without a working
  OpenGL/EGL install.
- Gated behind a **`HAVE_RETRO_GPU_TARGET`** build define (opt-in only,
  default build unaffected) — **not** `HAVE_OPENGL`: that name collides
  with the pre-existing dead `RETRO_HW_RENDER` code above, which shares the
  same guard and includes a nonexistent `retroogl.c` — defining
  `HAVE_OPENGL` for real activates that broken dead code and breaks the
  build. Use `HAVE_RETRO_GPU_TARGET` for this feature specifically.
- Build wiring needed touching **two** places for the define/files to
  actually reach the final linked binary — `retromain.cpp`/`libretro.cpp`
  are compiled *twice* in this build system (once into `libosd_retro.a` via
  `retro.lua`'s `osd_retro` project, once more directly into the final
  `mame` project via `scripts/src/main.lua` ~line 414 — only the second
  copy's object ends up in `mame_libretro.so`). Missed this once during
  implementation; a define/files change in only one of the two silently
  had no effect on the actual binary.

**Build command** (verified working):
```sh
MAME_DYNAMIC_LIBSTDCXX=1 HAVE_RETRO_GPU_TARGET=1 make -f Makefile.libretro -j4 SOURCEFILTER=arcade.flt PREMAKE=0
```
Two build-system quirks discovered/worked around this session, both
independent of this feature and likely to bite future work too:
- **The `Makefile.libretro`/`makefile` wrapper intermittently skips genie
  regeneration** even with `REGENIE=1` (default) after editing a `.lua`
  build script — no `Generating "..."` output, stale generated Makefiles
  silently reused. Root cause not fully found. **Workaround**: after any
  `.lua` script edit, invoke genie directly to force it —
  `MAME_DYNAMIC_LIBSTDCXX=1 3rdparty/genie/bin/linux/genie
  --LIBRETRO_CPU=x86_64 --with-emulator --OPTIMIZE=3 --NOWERROR='1'
  --target='mame' --subtarget='mame' --build-dir='build'
  --NO_USE_MIDI='1' --NO_USE_PORTAUDIO='1' --PYTHON_EXECUTABLE='python3'
  --SOURCEFILTER='arcade.flt' --HAVE_RETRO_GPU_TARGET='1' --osd='retro'
  --targetos='linux' --PLATFORM='x86' --gcc=linux-gcc --gcc_version=16.2.1
  gmake` (confirm `Generating "..."` output appears, then `grep` the
  relevant generated `.make` file for your change) — then build with
  `PREMAKE=0`.
- **Generated object rules reference `$(MAKEFILE)` as a prerequisite, but
  it's never actually defined anywhere** in this build — so it's silently
  empty, meaning **plain incremental `make` never recompiles an object
  just because compiler flags/defines changed**, only if the source
  `.cpp`'s own mtime changed. If you change a `.lua` script's `defines`/
  flags without touching the affected `.cpp` files, `rm` the specific
  stale `.o` file(s) before rebuilding or the change silently won't apply.
- Also: `-j16`/`-j8` full-ish rebuilds twice hit this harness's low-memory
  guard and got killed mid-build during this session (not a real OOM — RAM
  was free again immediately after); `-j4` was reliable. Prefer `-j4` for
  this project's builds.

### Phase 2 status: working, correctness/stability solid, performance acceptable but host-limited (2026-09-16)

`psxgpu_device` now routes its four polygon primitives to the Phase 1 GPU
service (see the plan file for full detail on each). **Confirmed by live
visual testing: zero on-screen issues** (no crash, no black-frame flicker,
no ground/sky edge flicker) at 2x internal resolution, with performance
"a lot better" after the batching work below, though not a rock-solid
60fps on this specific dev machine (see "Performance: final state" below
for why that's believed to be host/environment-specific, not a core bug).
**Every correctness bug below was invisible to a clean compile and even to
a crash-free soak test — they only showed up from watching actual
gameplay.** Reinforces: always do a real visual `retroarch -v -L ... <rom>`
smoke test after touching this code.

1. **Crash** (SIGSEGV in `end_frame_and_readback`, deep in Mesa): caused by
   leaving `retro_gpu_target`'s private EGL context current on the shared
   thread across multiple *separate MAME/RetroArch scheduler-visible calls*
   — RetroArch's frontend can synchronously do its own EGL work (context
   reinit) *inside* a MAME core call (e.g. when our scaled-resolution
   `screen.configure()` propagates to `SET_SYSTEM_AV_INFO`), and if our
   context was still bound at that moment it corrupted RetroArch's own EGL
   state. Two designs that held the context across multiple *separate*
   calls (even lazily-reacquired ones) both eventually crashed this way,
   consistent with a Mesa/driver-level issue - not something fixable via
   EGL bookkeeping alone - with RetroArch recreating its own context while
   ours coexists on the GPU. **Fixed, final architecture**: every
   `retro_gpu_target` call still defaults to acquiring/releasing its own
   context individually (`scoped_context` in `retro_gpu_target.cpp`, safe
   on its own) - see item 2 for how this coexists with good performance.
2. **Performance** ("terribly slow", later "a lot better...but still not
   full speed"), addressed in three real, independent steps:
   - `upload_texture()` was decoding+uploading the full 256×256 texture
     page on *every textured polygon*, not once per frame. **Fixed** via
     `gpu_maybe_upload_texture_page()` (`psx.cpp`), caching the
     last-uploaded page/CLUT and skipping re-upload when unchanged.
   - Per-call EGL context acquisition (~1 `eglMakeCurrent` pair per
     triangle) was then the dominant cost. **Fixed** by deferring all of a
     frame's `upload_texture`/`set_clip_rect`/triangle calls into a
     structured queue (`psxgpu_device::m_gpu_queue`, a `gpu_queued_cmd`
     list, not opaque closures) and replaying the whole queue in gpu_
     update_screen() as one tight, synchronous C++ loop bracketed by new
     `osd::gpu_render_target::begin_batch()`/`end_batch()` calls - one
     context acquisition per *frame* instead of per *call*. This is safe
     where holding the context across multiple frame's worth of
     *separately-scheduled* calls (item 1) was not: control never returns
     to MAME's scheduler or RetroArch during the replay loop, so the risky
     interleaving that caused item 1's crash is structurally impossible
     here, not just unlikely. `scoped_context` checks a `m_batch_active`
     flag and skips its own acquire/release when a batch already holds the
     context (see its 4th constructor arg).
   - Per-*triangle* GL draw-call overhead (one `glBufferData`+
     `glDrawArrays` each) was still significant even with EGL switching
     fixed. **Fixed** by having the queue replay loop merge consecutive
     `TRIANGLE` commands sharing the same textured/blend state into one
     larger vertex buffer and a single `osd::gpu_render_target::
     submit_triangles()` call (new method, default implementation loops
     `submit_triangle()` for any backend that doesn't override it) instead
     of one draw call per triangle - a run only breaks on an actual state
     change or an intervening `TEXTURE`/`CLUT` command.
3. **Correctness** (periodic near-black frames, "game frame → black frame →
   game frame" with only a few edge/HUD shapes visible on the black ones):
   `begin_frame()`/`gpu_update_screen()` were unconditionally clearing the
   GPU target every frame, wrongly assuming every game fully redraws 100%
   of the screen every refresh. Real PS1 VRAM (and the original
   VRAM-scanout path) is persistent — a game only erases what it explicitly
   overdraws. **Fixed** by moving the one-time clear from `begin_frame()`
   into `resize_target()` (only on actual FBO (re)allocation), so the
   target now persists content across frames like real VRAM. **Confirmed
   fixed by live visual testing.**
4. **Correctness** (flicker specifically on large, edge-of-screen polygons
   — ground/sky backdrops): the GPU path ignored PS1's draw-area clip
   rectangle entirely (`n_drawarea_x1/y1/x2/y2`, used extensively by the
   CPU rasterizer's clipping logic but never passed to the GPU path).
   Large backdrop polygons are the geometry most likely to extend beyond
   the current draw area, so as it changed frame-to-frame, unclipped
   polygons bled into/overwrote regions they shouldn't on the now-
   persistent framebuffer. **Fixed** via `osd::gpu_render_target::
   set_clip_rect()` (implemented with `GL_SCISSOR_TEST`/`glScissor` in
   `retro_gpu_target`), called from `psxgpu_device::
   gpu_maybe_set_clip_rect()` (cached like the texture-page cache above).
   Note: `glReadPixels` is itself affected by the scissor box, so
   `end_frame_and_readback()` temporarily disables `GL_SCISSOR_TEST`
   around its own readback and restores it after. **Confirmed fixed by
   live visual testing.**

**Performance: final state (2026-09-16).** After the three fixes in item 2,
Brave Blade runs noticeably better but still isn't a locked 60fps on this
dev machine - live testing (`btop`) showed short bursts of a single CPU
core near 100%, consistent with the still-mostly-single-threaded
CPU-emulation + GPU-submission pipeline. Investigated and ruled out as the
cause: the `mame-dev` Distrobox container itself (identical Mesa 26.2.2
inside and outside, hardware `radeonsi` acceleration confirmed active - not
falling back to software rendering, no cgroup CPU quota limit, and the host
CPU does reach its ~4.8GHz max under load rather than being hard-capped -
Podman/Distrobox containers share the host kernel directly, not a VM, so
there's little room for a "container tax" here). RetroArch's `video_threaded`
option was also tried (now left enabled in `~/.config/retroarch/
retroarch.cfg` - harmless, no measurable difference either way). The
**one concrete, host-level factor found**: this machine's CPU governor is
`powersave` **system-wide** (not container-specific - `cat /sys/devices/
system/cpu/cpu*/cpufreq/scaling_governor`), typically running ~2.2GHz
rather than its ~4.8GHz max, and Feral GameMode - which would normally push
games to the `performance` governor - isn't actually registered as a
running service on this system (`systemctl --user status gamemoded` finds
nothing), so nothing is currently correcting for this during testing.
**User's assessment (2026-09-16), after trying the above**: likely
acceptable in a real/production emulator environment (i.e. this specific
gap is believed to be a dev-machine power-management artifact, not a flaw
in the core's rendering path) - deprioritized for now rather than chased
further. If revisited, the CPU governor is the next concrete thing to
test (switch to `performance` and re-measure) before looking for further
code-level optimizations.

See the plan file's Phase 2 status note for full root-cause detail on each,
including the full crash/stability investigation behind item 1's final
architecture.

### Phase 3 status: core option done, regression testing partially done (2026-09-16)

- **`mame_psx_gpu_hle` core option** (category "video": disabled/2x/4x,
  default disabled) toggles the GPU path and its resolution multiplier
  without a rebuild - added following the exact `mame_lua_console`/
  `mame_debug_plugin` pattern. `GPU_RES_SCALE` moved from a compile-time
  constant to a runtime `psxgpu_device::gpu_scale()` query
  (`osd_interface::gpu_render_scale()`). Verified both states: default
  (disabled) boots at native resolution with no GPU context created at
  all (identical to pre-feature behavior); `2x` reproduces the previously
  verified GPU-rendered behavior exactly.
- **Regression testing**: `starswep` (Namco System 11) **passed** - clean
  boot, correct 512x480 scaling with that board's own 1.333 aspect ratio
  (confirms the scaling isn't accidentally Brave-Blade-specific), stable
  through an extended soak test. `nagano98` (Konami GV) **could not be
  tested** - the romset in this project's ROM collection is a 128-byte
  stub, not a real dump (confirmed via `unzip -l`), and no other
  `konamigv.cpp` game in the collection has a real dump either. This is a
  ROM-collection availability gap, not a code issue - revisit if a real
  dump for any `konamigv.cpp` game becomes available.

### Post-Phase-3 fixes: gdarius2 crash + ghosting (2026-09-17)

Found via `gdarius2` (G-Darius Ver.2, `src/mame/sony/zn.cpp`, Sony ZN) at 2x
GPU scale - two independent bugs, both now fixed and verified (90s soak,
no crash, no ghosting; `brvblade` re-verified unaffected by either fix):

1. **Crash**: `double free or corruption (!prev)` inside RetroArch's own
   `video_thread_free()`/`driver_uninit()`, not this core's code (confirmed
   via `gdb` backtrace - see "Debugging" below for the GDB workaround
   needed to get one). Root cause: `gdarius2` writes the GP1 display-mode
   register several times during boot; at 2x scale the resulting
   resolution exceeded the libretro OSD's hardcoded 720x720
   `max_width`/`max_height` (`src/osd/libretro/libretro-internal/
   libretro.cpp`), and crossing that ceiling forces a full
   `RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO` (`window.cpp`'s
   `VIDEO_CHANGED_AV_INFO`) instead of the cheap `SET_GEOMETRY` - RetroArch
   handles that by tearing down and reinitializing its whole video driver,
   and its threaded video driver has a double-free bug when hit that way.
   **Fixed** (not by patching RetroArch) by pre-scaling `max_width`/
   `max_height` by the `mame_psx_gpu_hle` multiplier in `retro_load_game()`,
   right after `check_variables()`, before the machine starts or any AV-info
   is ever sent - scaled resolutions then never exceed the declared max, so
   in-session mode changes stay on the cheap path and never re-trigger
   RetroArch's fragile reinit.
2. **Correctness** (frame ghosting: new content draws correctly each frame,
   e.g. ships moving, but stale previous-frame pixels remain everywhere
   nothing new was drawn, until a full screen change happens to overwrite
   them): only the four polygon primitives (`FlatPolygon`,
   `FlatTexturedPolygon`, `GouraudPolygon`, `GouraudTexturedPolygon`) were
   ever routed to the GPU target - `FlatRectangle`(+`8x8`/`16x16`),
   `FlatTexturedRectangle`, and `Sprite8x8`/`Sprite16x16` still only wrote
   to the software `p_vram` buffer. Games that use a solid `FlatRectangle`
   to erase/clear regions of the screen each frame (distinct from a full
   VRAM clear - a common PS1 technique `gdarius2` apparently relies on
   more than `brvblade` does) had that erase applied only to software VRAM,
   never reflected in the GPU target's deliberately-persistent framebuffer
   (see Phase 2 item 3 above), so old GPU-rendered pixels from prior frames
   stuck around as ghosting. **Fixed** by giving all six of these
   primitives the same GPU-submission path as the four polygon primitives
   already had (`gpu_submit_flat_rectangle()`/`gpu_submit_textured_
   rectangle()` in `psx.cpp`, each rectangle/sprite becoming two triangles).
   Since `psxgpu_device` is shared across every PS1-based board (see
   "Active target" above), this was a latent bug for any game on any of
   those boards that leans on rectangle/sprite primitives, not just
   `gdarius2` - worth re-testing `starswep`/other already-verified romsets
   if picking this back up.
3. **Debugging note**: `gdb` on this `mame-dev` Distrobox hits an internal
   GDB bug (`ext_lang_guard: Assertion 'is_main_thread()' failed`) when it
   tries to canonicalize a deeply-templated type name from
   `mame_libretro.so`'s debug info (a `make_cosine_table<0,1,2,...>` audio
   DSP table) as soon as any new thread appears - crashes GDB itself, not
   the target. Workaround that got a real backtrace: `set auto-solib-add
   off` before `run`, then `sharedlibrary mame_libretro` after the crash to
   load symbols only for the one library actually needed, avoiding
   whatever triggers the bad type lookup in the other libraries.
4. **Correctness, round 2** (same-shaped ghosting bug, found via `starswep`
   regression-testing item 2 above: 2D-only attract-mode content - no 3D
   geometry involved at all - visibly ghosted the same way, confirmed via
   live testing to not happen with the GPU path disabled). The item-2 fix
   only covered rectangle/sprite primitives; every *other* primitive that
   can land on screen was still software-only: `Dot`/`TexturedDot`,
   `MonochromeLine`/`GouraudLine`, GP0 `0x02` "Fill Rectangle in VRAM", GP0
   `0x80` "Move Image in Frame Buffer" (VRAM-to-VRAM copy), and GP0 `0xA0`
   "Copy Rectangle (CPU to VRAM)" - the standard way PS1 games DMA
   pre-rendered 2D art (logos, backgrounds, UI) straight into VRAM, and the
   dominant cause of this particular bug given the "2D-only, no 3D" shape.
   **Fixed** by routing all of these to the GPU target too:
   - `0x02`/`0xA0` are absolute-VRAM-address, no-draw-area-clip commands on
     real hardware (matching their software implementations, which skip
     both `n_drawoffset_x/y` and the `n_drawarea_*` clip check) - new
     `gpu_force_no_clip()` pushes a full-target CLIP command and
     invalidates the clip cache so the *next* real primitive unconditionally
     restores the game's actual draw area afterward.
   - `0xA0` specifically is implemented as a textured-quad "stamp" sampling
     the whole-VRAM texture (`gpu_submit_image_stamp()`) rather than
     re-uploading the transferred bytes as a second texture - the transfer
     still writes `p_vram` exactly as before (now serving only as this
     frame's `upload_vram()` source, not the visible framebuffer), and
     since this target's local coordinate space already equals VRAM-
     absolute address space (every offset-relative primitive ends up at
     `coord + n_drawoffset` == the VRAM address it's specified relative
     to), the stamp's destination and its source UV in the VRAM texture
     are literally the same numbers.
   - `0x80` (MoveImage) is different: its source may be GPU-rendered
     content (a polygon/rectangle/sprite from earlier this frame or a
     prior one) that was *never* written back to `p_vram`, so sampling the
     VRAM texture would be wrong. Implemented as a real GPU-side
     framebuffer-to-itself blit instead - new `osd::gpu_render_target::
     copy_rect()` (default no-op in the interface, `retro_gpu_target`'s
     implementation uses `glBlitFramebuffer` with the same FBO bound as
     both read and draw, temporarily disabling the scissor test) - and a
     new `COPY` kind in `psxgpu_device::m_gpu_queue` so it replays at the
     correct point in the frame's command order, same as `TEXPARAM`/`CLIP`.
   - `Dot`/`TexturedDot`/lines *do* respect draw offset and the draw-area
     clip on real hardware (confirmed in their software implementations),
     so these go through the normal `gpu_submit_triangle_pair()` path
     rather than the no-clip one - a 1x1 quad for dots, a thin
     (half-scaled-pixel-wide) quad along the segment for lines, reusing
     the existing triangle/blend infrastructure rather than adding a new
     `GL_LINES` draw path.
   Verified: `starswep` attract mode no longer ghosts; `gdarius2` and
   `brvblade` re-confirmed unaffected (both still crash-free, ghosting-free).
5. **Correctness, round 3 - display-start/double-buffering and overscan
   border cropping** (found via `raystorm`/RayStorm, Taito, same
   `src/mame/sony/zn.cpp` board as `gdarius2`): a duplicated strip of the
   top of the screen reappeared near the bottom, and separately a small
   block of what turned out to be raw CLUT (palette) data was visible in
   a corner - neither is a primitive-routing gap like items 2/4 above;
   both come from `gpu_update_screen()`'s final readback/compositing step
   never having replicated two things the software display path
   (`update_screen()`, further down in `psx.cpp`) already does correctly:
   - **Display-start windowing**: GP1 0x05 lets a game point "display
     start" anywhere in VRAM - `raystorm` flips `n_displaystarty` between
     `0` and `240` every frame, real PS1 double buffering (draw into
     whichever half isn't currently selected, then flip). The GPU path
     always read back target-local `(0,0)`, ignoring `n_displaystarty`/
     `m_n_displaystartx` entirely, so the *fixed* window it read from a
     *persistent* (never-fully-cleared, see Phase 2 item 3) target could
     miss whichever half was actually current. Also required enlarging
     the GPU target itself from `n_screenheight` to full `m_vram_height`
     (previously draw-offset-relative geometry aimed at the "hidden" half
     of a double-buffered scene landed outside the target and was
     silently discarded - `gpu_force_no_clip()`'s bounds needed the same
     enlargement, or it would incorrectly scissor absolute-VRAM commands
     aimed at that hidden half).
   - **Overscan border cropping**: the software path only ever shows the
     `n_vert_disstart`/`n_vert_disend`/`n_horiz_disstart`/`n_horiz_disend`-
     bounded window and paints solid black everywhere else (with PAL/NTSC
     `n_overscantop`/`n_overscanleft` constants) - real PS1 games rely on
     that crop, routinely stashing non-image scratch data (CLUT tables,
     work buffers) in the border, counting on real hardware never
     scanning it out. The GPU path showed the *raw* `n_screenwidth` x
     `n_screenheight` canvas verbatim, so `raystorm`'s CLUT swatch - safely
     invisible on real hardware and under the software path - became a
     visible artifact.
   **Fixed** by porting both algorithms from `update_screen()` into
   `gpu_update_screen()`: same PAL/NTSC/interlace/`b_reverseflag` branches,
   same border-fill-then-windowed-copy structure, with the copy step
   reading from `n_displaystarty`/`m_n_displaystartx` (wrapped modulo the
   now-larger target) instead of `(0,0)`. Debugging note: got the exact
   `n_displaystarty`/`n_drawoffset_y` values and target dimensions in play
   via temporary `fprintf(stderr, ...)` calls at the GP1 0x05 write site
   and the top of `gpu_update_screen()` - removed once confirmed; worth
   the same trick again for any future "GPU path shows something
   different from what should be visible" bug in this device, since
   VERBOSE-gated `LOGMASKED` calls need a rebuild to toggle and are easy
   to drown in unrelated log lines otherwise. Verified via screenshot
   (`spectacle`, since RetroArch's window is a native Wayland client
   invisible to X11-only capture tools like `xdotool`/`wmctrl` on this
   KDE/Wayland desktop) across three passes - fixed the vertical jump the
   display-start fix alone introduced, then fixed the still-remaining
   duplicate band and CLUT swatch with the border-crop fix; `gdarius2`,
   `starswep`, `brvblade` re-confirmed unaffected.

### Next planned work: rendering quality (PGXP-style correction) - not started

**User's intent (2026-09-16): this is the next thing to work on, in a
future session** (explicitly not the same day Phase 3 finished). Phases
1-3 above are done and working; this is new, not-yet-started work.

Current GPU path deliberately/inherently reproduces two classic PS1
visual characteristics: affine texture warping (the vertex shader hardcodes
`w=1.0`, disabling perspective-correct UV interpolation - see
`retro_gpu_target.cpp`'s `vertex_shader_src`) and geometry wobble (the
vertices we render are the same integer-truncated screen coordinates the
real GPU always received - the CPU-side GTE's low-precision fixed-point
math is what causes this, and `psxgpu_device`/`psx.cpp` - everything
touched so far - has never had access to real per-vertex depth to do
anything about it). Fixing either one for real needs the same missing
ingredient (real per-vertex depth), which means the same underlying
technique - **PGXP-style GTE interception** (`src/devices/cpu/psx/gte.cpp`,
opcodes **RTPS**/**RTPT** - completely different, untouched code from
Phase 1-3, belonging to `psxcpu_device` not `psxgpu_device`) - would
address both at once, not two separate projects. Full technical grounding,
including why running Brave Blade in a console-only PS1 core (e.g. Beetle
PSX HW) instead was considered and rejected (Brave Blade is Sony ZN-2
**arcade** hardware, not a retail PS1 game - MAME already solved the hard
"arcade platform compatibility" part; a console core has none of that),
is in `/home/bazzite/.claude/plans/glowing-conjuring-raven.md`'s "Phase 4"
section - **read that before starting**, nothing below is a
substitute for it.

### Other candidates (not currently being worked, kept for reference)

Ranked by how self-contained/impactful their software rasterizer is. Line
counts are for the whole file at investigation time — expect drift after
upstream merges.

| Driver | Video/rasterizer file(s) | Test romset(s) available | Notes |
|---|---|---|---|
| Sega Model 2 | `src/mame/sega/model2_v.cpp` (~2450 lines) | `daytona`, `vf2`, `fvipers`, `lastbrnx`, `von` | Custom quad/polygon rasterizer, well-isolated from the rest of `model2.cpp`. Good first target. |
| Sega Model 3 | `src/mame/sega/model3_v.cpp` (~2500 lines) | `daytona2` | Texture-mapped Gouraud triangles, more complex than Model 2. |
| Namco System 22 | `src/mame/namco/namcos22_v.cpp` (~2700 lines) | `ridgerac`, `timecris`, `acedrive`, `cybrcomm` | Uses the shared legacy software polygon helper (`src/mame/ausnz/poly.h` / `src/devices/video/poly.h`). |
| Namco System 11 (3D) | `src/mame/namco/namcos11.cpp` | `starswep` | PS1-derived 3D hardware, simpler than System 22/23. |
| Konami GTI Club-class | `src/mame/konami/gticlub.cpp`, `nwk-tr.cpp`, `hornet.cpp` | `gticlub`, `hangplt`, `thrilld`, `gradius4` | Custom 3D chip (K001005/K001006) does the real rasterizing. **Correction (2026-09-17)**: these files also wire a `generic_voodoo_device` into their memory map (via `konppc_device`), but it's bus/register glue only, not the renderer — confirmed no `K001005`/`K001006` config exists without it and the Voodoo device has no framebuffer/screen of its own here. Not a Voodoo rasterizer target. |
| Konami (3D, other) | `src/mame/konami/konamigv.cpp` | `nagano98` | PS1-derived GV system (uses `psxgpu_device`, same device already GPU-accelerated for Sony ZN — see "Active target" above). `nagano98`'s romset in this collection is a stub, untestable currently. |
| Midway "Vegas Flavor" | `src/mame/williams/midvunit.cpp` | `crusnusa`, `crusnwld`, `offroadc` | **Correction (2026-09-17)**: does **not** use `voodoo*.cpp` — has its own self-contained rasterizer, `midvunit_renderer : poly_manager<...>` in `midvunit_v.cpp`. Custom TMS34010-driven polygon rasterizer, not Voodoo-based. Kept here as a candidate in its own right, not as a Voodoo target. |
| Midway "Seattle" (Voodoo 1) | `src/mame/williams/seattle.cpp` | `sfrush`, `sfrushrk`, `mace`, `calspeed`, `vaportrx`, `carnevil`, `hyprdriv` | True Voodoo-rasterizer target (`voodoo_render.cpp`, ~2900 lines total). Entry point: `enqueue_triangle()` in `src/devices/video/voodoo.cpp:3015`. Best-covered board in the local ROM collection. |
| Midway "Vegas" (Voodoo 2) | `src/mame/williams/vegas.cpp` | `gauntleg`, `tenthdeg`, `gauntdl`, `warfa`, `roadburn`, `sf2049`, `cartfury` | Also a true Voodoo target, same device/entry point as Seattle above. Second-best-covered board locally. |
| Midway "Quicksilver" (Voodoo 2, PCI) | `src/mame/williams/midqslvr.cpp` | none locally (`hydrthnd`, `offrthnd`, `arctthnd` not in collection) | True Voodoo target, PCI-attached like the PC-based systems below rather than a fixed on-board device. |
| Konami Viper (Voodoo 3/Banshee) | `src/mame/konami/viper.cpp` | `kviper`, `code1d`, `gticlub2`, `jpark3`, `thrild2`, `wcombat`, `xtrial` | True Voodoo target (`voodoo_banshee.cpp`/`voodoo_3_device`). Note: despite the name, `gticlub2` here is unrelated to the K001005-based original `gticlub` above — Viper-era GTI Club 2 really is Voodoo-rendered. |
| ITEagle (Voodoo 3) | `src/mame/itech/iteagle.cpp` | `iteagle`, `virtpool`, `carnking`, `bbh` | True Voodoo target. |
| PC-based arcade systems (Voodoo as a real PCI card) | `src/mame/misc/comebaby.cpp`, `funkball.cpp`, `gammagic.cpp`, `magictg.cpp`, `savquest.cpp`, `xtom3d.cpp`, `src/mame/pc/quakeat.cpp`, `src/mame/taito/taitowlf.cpp` | `pumpitup` (+ ~18 variants, `xtom3d.cpp`) is the only one locally available | Embedded-PC arcade systems with genuine Voodoo 1/2/Banshee PCI cards; same `voodoo_render.cpp` pipeline as the dedicated boards above but reached through `voodoo_pci.cpp`. |
| Midway Zeus | `src/mame/williams/midzeus.cpp` | `mk4`, `invasnab` | Custom "Zeus" 3D chip, not Voodoo. |
| Nintendo 64 (RDP) / Aleck64 | `src/mame/nintendo/n64_v.h` (~4200 lines), `src/mame/nintendo/aleck64.cpp` | `aleck64` | N64's actual 3D pipeline (RDP) — most complex/most faithfully-emulated candidate, high risk/reward. |

Romset → driver mappings above were confirmed by grepping `GAME(...)` macros
in `src/mame` against what's actually on disk (see "Test ROMs" below) — re-run
that check if retargeting to a romset not listed here.

The generic legacy rasterizer helper `poly.h` is used by ~22 files across
`src/mame` — worth checking as a single integration point if a driver uses it,
rather than hand-writing GPU replacement code per driver.

There is **no existing GPU-passthrough precedent** in any of these — all are
pure software rasterizers today. This is genuinely green-field work in this
codebase, not a matter of flipping on a hidden fast path.

## Building

```sh
make -f Makefile.libretro -j4          # full build (see -j note below)
make -f Makefile.libretro -j4 PREMAKE=0  # faster incremental rebuild after the first build
```

- **Prefer `-j4` over `-j16`/`-j8`**: full-ish rebuilds at higher parallelism
  twice triggered this harness's low-memory guard and got killed mid-build
  (not a real host OOM — 16 cores are available and plenty of RAM is free
  again immediately after); `-j4` completed reliably every time.

- Output: `mame_libretro.so` at the repo root (GENie appends the
  `_libretro` target suffix; see `scripts/src/main.lua`).
- `DEBUG=1 make -f Makefile.libretro ...` switches to `CONFIG=libretrodbg`
  (`SYMBOLS=1 SYMLEVEL=1 OPTIMIZE=g`) — use this when you need gdb/perf
  symbols on rasterizer code you're rewriting.
- No `SUBTARGET` is set by default, so **a full build compiles every driver in
  MAME** (~4,600 source files) — this genuinely takes a long time and pegs all
  cores; a full `-j16` build was aborted mid-run during initial setup of this
  project rather than let it run to completion unattended. Prefer the
  arcade-only filter below while iterating.
- **`MAME_DYNAMIC_LIBSTDCXX=1`** — set this env var when invoking `make` on
  this machine. Upstream unconditionally links `-static-libgcc -static-libstdc++`
  on Linux (`scripts/genie.lua`, "See #137" — for portable prebuilt-core
  distribution across distros), but this host only has the 32-bit static
  `libstdc++.a` installed, not 64-bit, so the link fails with `cannot find
  -lstdc++` without this flag. The opt-out (added to `scripts/genie.lua`) falls
  back to dynamically linking the system's `libstdc++.so`, which is fine when
  building and running on the same machine. Full invocation:
  `MAME_DYNAMIC_LIBSTDCXX=1 make -f Makefile.libretro -j4 SOURCEFILTER=arcade.flt`.
- **If you ever force-kill a build mid-compile** (e.g. `kill -9 -<pgid>` on a
  runaway job — note killing just the top-level `make` PID is not enough, its
  per-directory child `make` processes keep spawning compiler jobs; kill the
  whole process group), **check for zero-byte object files before trusting the
  next incremental build**: `find build/libretro/obj -name "*.o" -size 0`.
  GCC can leave a truncated/empty `.o` on disk when killed mid-write, and
  incremental `make` won't recompile it (source `.cpp` mtime hasn't changed),
  silently producing a `.so` that links "successfully" but has undefined
  symbols at `dlopen()` time in RetroArch (hit this once with
  `src/mame/atlus/cave.cpp` → `undefined symbol: driver_agallet`). Fix: `rm`
  the zero-byte `.o` files, rebuild.

### Arcade-only build filter (much faster iteration)

`arcade.flt` at the repo root is a `SOURCEFILTER=` file — a plain list of
`src/mame/<mfg>/<driver>.cpp` paths, one per line — that restricts the build to
files declaring at least one `GAME()`/`GAMEL()` (arcade) system, dropping
console/computer/gambling/mechanical (`CONS`/`COMP`/`SYST`)-only driver files
entirely. **Every GPU-offload candidate driver in this project (Model 2/3,
Namco System 22/23, N64/Aleck64, Voodoo-based games) is arcade and stays in.**
Reference numbers (from `/var/home/bazzite/Projects/libretro/speed-mame/README.md`,
verified to match this exact checkout byte-for-byte): 2,639 of 4,628 driver
files kept, ~28% smaller `mame_libretro.so`, dramatically fewer files to
recompile on a full build.

```sh
make -f Makefile.libretro -j4 SOURCEFILTER=arcade.flt
```

`Makefile.libretro` was patched (mirroring its existing `SOURCES=` passthrough)
to forward `SOURCEFILTER=` to the underlying GENie build — see the
`TARGETFLAGS` block. `arcade.flt` is covered by this repo's root `.gitignore`
(`/*` blanket rule) so it won't show up in `git status`/be committed, but it's
real on disk and required for this to work; regenerate it if it ever goes
missing (see below).

**Regenerating** (only needed if `src/mame/` driver files get renamed/moved/
split by a future upstream merge — verified unnecessary as of the mame0289
merge currently checked out):
```sh
python3 /var/home/bazzite/Projects/libretro/speed-mame/make_arcade_filter.py src/mame arcade.flt
# then validate statically before trusting it:
python3 scripts/build/makedep.py -r . filterproject -t mame_arcade -f arcade.flt src/mame/mame.lst > /dev/null
# expect: exit 0, "N source file(s) found" on stderr, no errors.
```
The static check alone doesn't catch everything — see
`/var/home/bazzite/Projects/libretro/speed-mame/README.md` ("Bug found and
fixed") for a past link-stage failure the generator now avoids (zero-macro
helper files with no driver macro of their own, pulled in per-directory). A
real build through to link is the only full proof; watch for `undefined
reference` warnings even on a `.so` link that exits 0.

## Test ROMs

`/var/home/bazzite/Projects/mame-roms/roms/` contains a working collection of
~600 romsets and BIOS files (read permission granted for this directory). Use
it as the source of test content — e.g. `daytona.zip` for Model 2,
`crusnusa.zip` for a Voodoo-based Midway game. Some drivers need a separate
BIOS zip alongside the game zip (e.g. `*_bios.zip` files in that directory) —
check the driver's `ROM_START`/parent-set relationship if a romset fails to
load. See the candidate driver table above for confirmed available romsets
per driver.

## Running/testing in RetroArch

### Finding: the Flatpak RetroArch can't load a host-built core on this machine

RetroArch is installed as a **Flatpak** (`org.libretro.RetroArch`), with the
`host` filesystem permission — it can read a freshly built `.so` straight out
of this repo, no need to copy it into its own cores directory. **But this
host runs glibc 2.43** (bleeding-edge/rolling bazzite), while the Flatpak's
bundled runtime (`org.kde.Platform` 6.11) only ships **glibc 2.42** — one
version behind. A core built with this host's toolchain requires symbols
(`sqrtf`, `acosf`, `atan2f`, `log10f` got a new version in glibc 2.43) the
Flatpak runtime doesn't have, so it fails to load:
`libm.so.6: version 'GLIBC_2.43' not found`. No Flatpak runtime update fixes
this (checked — already latest), and it's not likely to for a while since
bazzite here is ahead of the Flatpak/Fedora-stable ecosystem.

**Fix in use: a Distrobox container.** A Distrobox named `mame-dev` was
created from `registry.fedoraproject.org/fedora:rawhide` (glibc 2.44 at time
of creation — comfortably ahead), with `retroarch` installed inside via
`sudo dnf install -y retroarch`. Distrobox shares the host's display/audio
sockets directly (no portal/sandbox runtime mismatch like Flatpak), and
`/var/home/...` paths are visible inside unchanged since Distrobox shares the
host `$HOME`.

```sh
distrobox enter mame-dev -- retroarch -v -L /var/home/bazzite/Projects/libretro/mame/mame_libretro.so /var/home/bazzite/Projects/mame-roms/roms/daytona.zip
```

- `-v` enables verbose frontend logging — useful while iterating on a HW-render path.
- This RetroArch's config lives at `~/.config/retroarch/` (native install
  inside the container, shared `$HOME` — so this path is the same whether you
  read it from inside or outside the container). Per-core option overrides
  live at `~/.config/retroarch/config/MAME/MAME.opt` (plain `key = "value"`
  lines, RetroArch rewrites this file, safe to hand-edit between runs).
- If the Flatpak RetroArch ever becomes usable again on this host (its
  runtime catches up to glibc 2.43+), its invocation is:
  `flatpak run org.libretro.RetroArch -v -L <core> <rom>`, config at
  `~/.var/app/org.libretro.RetroArch/config/retroarch/`.
- Core options relevant to video are defined in
  `src/osd/libretro/libretro-internal/libretro_core_options.h` (e.g.
  `*_alternate_renderer`, `*_altres`, `*_rotation_mode`) — these only affect
  the software-framebuffer path (scaling/cropping/resolution), not a GPU
  backend; there's currently nothing here to toggle a hardware renderer on.

## Debugging / monitoring progress

Since the GUI debugger is compiled out (`none.cpp`, see above), MAME's Lua
console/plugin system is the way to inspect state at runtime. This required
plumbing (added 2026-09-16) because the libretro OSD didn't wire any of it up
by default — see "What was added" below if extending it further.

### Using it

Two RetroArch core options control this (category **Debug** in RetroArch's
Core Options menu, or set directly in
`~/.config/retroarch/config/MAME/MAME.opt` for the Distrobox RetroArch):

- **`mame_lua_console`** (`disabled`/`enabled`) — passes `-console` to MAME,
  starting its interactive Lua REPL on stdin/stdout. Only usable when
  RetroArch itself is launched attached to a real terminal (i.e. run it from
  a shell, not RetroArch's own GUI launcher) — the console reads/writes that
  terminal's stdin/stdout directly.
- **`mame_debug_plugin`** (`none`/`gdbstub`/`cheatfind`/`hiscore`/`timer`) —
  passes `-plugin <name>`, autostarting one Lua plugin non-interactively (no
  terminal needed). `gdbstub` (`plugins/gdbstub/`) exposes CPU state via the
  GDB remote protocol, but **only implements the i386/i486/pentium register
  map today** — not directly useful for Model 2 (V60/V70), Model 3 (PowerPC),
  or Namco System 22/23's CPUs without extending its `regmaps` table first.

Both were verified end-to-end against Daytona USA in the `mame-dev` Distrobox:
clean boot with `mame_debug_plugin = "hiscore"` and separately with
`mame_lua_console = "enabled"`, matching the same `Geometry:`/no-fatal-error
signature as a plain run. (RetroArch's own combo-option validation makes a
true negative test — e.g. a nonexistent plugin name — impossible to construct
this way: it silently falls back to the option's default rather than passing
through an invalid string, so absence of a crash there proves RetroArch's
validation, not MAME's. MAME's own `fatalerror("Could not load plugin: ...")`
in `src/frontend/mame/mame.cpp` is the real enforcement, confirmed by reading
the source rather than by triggering it live.)

### What was added (the plumbing itself)

MAME's plugin loading (`src/frontend/mame/mame.cpp::start_luaengine`,
`pluginopts.cpp`) is core/OSD-agnostic — it was never libretro-specific code
missing, just nothing telling it to activate. Three things were needed:

1. **Path resolution already worked** — `src/osd/libretro/libretro-internal/retro_init.cpp`'s
   `Set_Path_Option()` already had a `"-pluginspath"` entry (`opt_name`/`dir_name`
   arrays, `NB_OPTPATH`) resolving to `<retro_system_directory>/mame/plugins`.
   Just needed the directory to actually exist there — copied this repo's
   `plugins/` into `~/.config/retroarch/system/mame/plugins/` (the Distrobox
   RetroArch's system directory) via `cp -r`. Do the same for any other
   RetroArch install/environment you test against.
2. **Two new RetroArch core options** added to
   `src/osd/libretro/libretro-internal/libretro_core_options.h`
   (`mame_lua_console`, `mame_debug_plugin`, new "debug" category) — v1/legacy
   option tables are auto-derived from this at runtime, no need to touch them.
3. **Wiring**: `check_variables()` in `libretro.cpp` reads the two new RetroArch
   variables into `lua_console_enable`/`debug_plugin` (declared in
   `libretro_shared.h`, defined in `retro_init.cpp`); `Set_Default_Option()` in
   `retro_init.cpp` turns them into `Add_Option("-console")` /
   `Add_Option("-plugin"); Add_Option(debug_plugin)` — the same pattern every
   other boolean/string core option in this OSD already uses (mirror
   `mouse_enable` if adding more).

To add another autostart-able plugin, just add its name to the
`mame_debug_plugin` value list in `libretro_core_options.h` — no other code
changes needed, since `-plugin <name>` resolution is generic.

### Other monitoring options

- **`-v` / verbose RetroArch logging** for frontend-side issues (context
  creation failures, video driver mismatches).
- For measuring actual rendering performance changes, prefer host GPU tools
  (`mangohud`, `nvidia-smi`/`radeontop`, RenderDoc if a real GL/Vulkan context
  ever gets created) over anything MAME-internal, since MAME's own frame-timing
  instrumentation assumes/measures the software path.
- When first reviving the `HAVE_OPENGL` path, a well-placed `fprintf(stderr, ...)`
  or `osd_printf_verbose` in `context_reset`/`retro_load_game` is the fastest way
  to confirm whether RetroArch actually granted the HW render context before
  chasing rendering bugs further downstream.

## Working conventions for this project

- Treat `src/mame/<manufacturer>/*_v.cpp` rasterizer rewrites as the highest-risk
  changes in this codebase — they must still produce output through the existing
  `bitmap_rgb32`/render-primitive path for any driver you *haven't* converted,
  so don't break the software fallback while adding a GPU path. A per-driver or
  global switch (core option or compile-time) to fall back to software rendering
  is expected, not optional, until a GPU path is proven correct.
- **Active target is `src/devices/video/psx.cpp` (`psxgpu_device`)**, exercised
  via Brave Blade (`brvblade.zip`) on `src/mame/sony/zn.cpp` — see "Chosen
  first target" above. Since this device is shared by many drivers, changes
  here have a much bigger blast radius than a single-driver rasterizer: a
  regression affects every PS1-based board (Konami GQ/GV/573/Twinkle, Namco
  System 10/11/12, Taito GNET, plain PS1 console), not just Brave Blade. Test
  against more than one of these before considering a change solid — `nagano98`
  (`konamigv.cpp`) and `starswep` (`namcos11.cpp`) are both already in the ROM
  collection and use this same device.
- If moving on from Sony ZN/PS1, the other candidates below are Model 2
  (`model2_v.cpp`) as the next-smallest single-driver rasterizer, then Model 3,
  System 22/23, N64, or Voodoo — those are progressively larger and more
  historically-accuracy-sensitive.
- When modifying `src/osd/libretro/libretro-internal/libretro.cpp`'s HW-render
  block, keep changes scoped to that file and `retro_modules.lua`/`Makefile.libretro`
  build-flag plumbing — avoid touching the SDL/Windows OSD's BGFX/HLSL code,
  which is unrelated to this build target and a much larger surface.
- After any driver-rasterizer or HW-render change, always do a real
  `distrobox enter mame-dev -- retroarch -v -L mame_libretro.so <rom>` smoke
  test against a romset from `/var/home/bazzite/Projects/mame-roms/roms/` for
  that driver — this is emulator accuracy-sensitive code; a clean compile
  means nothing here.
