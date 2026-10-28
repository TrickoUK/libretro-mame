# CLAUDE.md

This file guides Claude Code when working in this repository. It holds only what an agent needs on every task.
History (sync log, per-phase PS1 GPU notes, old plans) lives in `my-docs/activity-log.md`: **do not read it at
the start of a session**, it is an archive that wastes context. `grep` it only when you need the history of one
specific system or sync, and append new history there rather than here.

## What this repo is

A fork of [libretro/mame](https://github.com/libretro/mame) (upstream MAME with a `libretro` OSD grafted on),
pushed to `TrickoUK/libretro-mame`. It's MAME itself (`src/mame`, `src/devices`, `src/emu`) plus a libretro
frontend layer in `src/osd/libretro`. Upstream syncs land as `Merge mamedev master ...` commits, so expect large,
unrelated diffs in history.

**Project goal**: improve 3D rendering for specific arcade drivers by offloading polygon rasterization to the host
GPU (Vulkan), instead of MAME's fully-software rendering, plus game fixes and light gun / steering work for
Batocera.

### Branches and upstream syncs

- **`arcade-focused`** is the long-term branch for all of this fork's work. **`master`** only tracks
  libretro/mame upstream. Short task branches get merged into `arcade-focused` and then deleted.
- Syncs use the `sync-mame` skill. Always a full `git merge` of the upstream tip (never cherry-picks), so SHAs stay
  aligned. The last merged upstream tip is kept at `refs/upstream-tmp/master` (fetched from the local checkout at
  `/var/home/bazzite/Projects/upstream/mame`). The per-sync record is in `my-docs/activity-log.md`, **not here**.
- After any sync that moves, renames or splits `src/mame/` driver files, regenerate `fork-specific/arcade.flt`
  (see "Arcade-only build filter"). genie fails with `Pattern "..." did not match any source files` otherwise. If
  any `scripts/**/*.lua` changed, run genie directly (see "Building"), then do a full build.
- Conflicts to expect again:
  - `src/devices/cpu/mips/mips3{,drc}.cpp`: Count writes must not arm the compare timer, or sfrush hangs at ~49 s.
  - `src/devices/cpu/powerpc`: our ICFI snapshots, compile-time fetch fill and reuse re-snapshot; upstream has its
    own HID0/ICFI handler, which we drop.
  - `src/devices/video/psx.{cpp,h}`: GPU HLE path. `src/devices/cpu/dspp`: idle-loop skip (Bulldog only).
  - `src/mame/konami/viper.cpp`: gamepad steering helper and gun reads. Upstream's 9-bit full-range pedal/handbrake
    ports replaced the fork's 8-bit path (keep upstream's).
  - `src/devices/video/voodoo_render.cpp` (fork split texturing into `fetch_texel()`/`sample_texel()`; mirror
    upstream LOD/sampling fixes there and in the GPU shader `voodoo_shader.h`, source `my-docs/tools/voodoo.glsl`),
    `voodoo_banshee.cpp`, `voodoo_2.cpp`: prefer upstream's fix where equivalent.
  - `scripts/genie.lua`, `scripts/src/main.lua`, `src/osd/modules/lib/osdobj_common.cpp`: libretro hooks.
  - `src/mame/namco/namcos22_v.cpp`/`namcos22.h` (host-GPU capture hooks: the `m_gpu_collecting` ternary, early
    returns in `poly3d_drawquad`/`poly3d_drawsprite`, the two `gpu_render_scene` calls in the screen updates; the
    rest is `namcos22_gpu.ipp`), `src/mame/namco/namcos23.cpp` (the `namcos23_shader.h`/`gpuengine.h` includes,
    `friend class namcos23_renderer;`, the renderer's `gpu_*` members and the `if (gpu_render(...)) return;` after
    the sort in `render_flush()`; the rest is `namcos23_gpu.ipp`/`namcos23_shader.h`), `src/mame/snk/hng64_3d.ipp`
    and `hng64_v.cpp` (GPU engine, submit/collect, palette-resolve composite). Fork-only, not for upstream.
    The software renderers are upstream again, so only these GPU-path hunks conflict.
  - `src/mame/williams/midzeus_v.cpp`/`midzeus.h` and `midvunit_v.cpp`/`midvunit.h` (host-GPU hooks: the
    `gpu_screen_update` call at the top of `screen_update`, the polygon capture and the quad path, the `gpu_*` members
    and the forced-software switches); the adapters are `midzeus_gpu.ipp` and `midvunit_gpu.ipp`, the shaders
    `midzeus_shader.h` and `midvunit_shader.h`. Fork-only, not for upstream.
- Upstream API changes that have broken fork/libretro code (check these first when a build fails after a sync):
  - `screen_device` is split per type (`screen_svg_device`, vector screens). Use
    `video_output_interface_enumerator`/`is_vector()`, not `screen_type()`. `configure()` takes an `attotime`.
  - `emu.h` no longer pulls in `video.h`; include it wherever `machine().video()` is used.
  - PS1 GPU VRAM is a `ram_device` (`m_ram`) wired up with `set_cpu()`; `p_vram` is a raw pointer into it.
  - DSPP is split into Opera (runs the program once per frame) and Bulldog (free-running, used by M2).

### Game fixes carried in the fork

Keep these through upstream merges, and check whether upstream has fixed the same thing before re-porting one:
- `plugins/offscreenreload/init.lua`: lightgun reload auto-bind, skipped when BUTTON2 is already used by gameplay.
- `src/mame/sega/model2*`: supersample antialiasing of the 3D layer on the host-GPU path (generic
  `mame_3d_supersample` option, off by default; the software renderer is upstream and ignores it); `hotd` has no
  trigger flash (`init_hotd` ROM patch, Revision A only); `gunblade` gun Y gets `PORT_REVERSE` under `__LIBRETRO__`.
- `src/mame/taito/opwolf.cpp`: no trigger flash on `opwolf` only. It's a runtime ROM patch about 900 frames in,
  because the C-chip ROM check rejects a static patch.

### Light guns on Batocera (Sinden)

- The core side is `src/osd/modules/input/input_retro.cpp`. In "free" offscreen mode the reload button shoots off
  screen, and gun Start/Select also press pad Start/Select. jpark3's port fix: `my-docs/viper-investigation.md`
  Fix 8.
- It needs Batocera's RetroArch to use its `udev` input driver (Batocera patch
  `019-disable-wayland-events-by-wayland.patch` must cover `wayland_vk_ctx.c`, since the box runs Vulkan);
  otherwise every gun shares the same buttons and gun Start/Coin fail. **If gun buttons break after a new image,
  check RetroArch before the core**: `[udev] Mouse/Touch #N` lines in the box's `es_launch_stderr.log` mean udev
  input is live.

## Docs to keep updated

Update these in the same change as the work. Docs under `my-docs/` are committed in that nested repo.

- **`my-docs/custom-change-log.md`**: dated record of this fork's own game/system work. Whenever a fork change
  lands on `arcade-focused`, add an entry under that day's heading (oldest first, new work at the bottom): a `###`
  headline naming the game(s) or system, then a few short bullets on the end result a player would notice. Commit
  it in `my-docs` with the work. Leave out upstream merges and cherry-picks unless fork work was needed to make them run, and
  investigations that shipped no code unless they reached a conclusion worth keeping.
- **`my-docs/3D-Enhanced-Games.md`**: major 3D work only (a new 3D system made to run, a host-GPU or supersampling
  feature, a speed-up that makes a 3D system playable, a new or changed 3D core option): the system's row in the
  summary table, its section, the settings table and the game list (bold the games actually checked). Small fixes
  (sprite glitches, boot errors, controls, light guns) go in `custom-change-log.md` only. Also keep
  `my-docs/3d-options.md` current when a 3D option is added/changed or a system gains/loses option support.
- **`my-docs/Lightgun-improvements.md`**: every game or system where light guns work or work better (problem, fix,
  tested with a real gun or only by script). Update with every gun port change, trigger-flash patch, aim support
  or libretro gun input change, alongside the matching `custom-change-log.md` entry.
- **`my-docs/activity-log.md`**: archive of history (sync record, PS1 GPU phases). Append there; never read it
  routinely.

## Repo layout cheat sheet

- `my-docs/` — fork investigation docs and test tools. **Its own private git repo**
  (`git@github.com:TrickoUK/mame-working-docs.git`, branch `main`), nested here and hidden from this repo by
  `/my-docs/` in `.git/info/exclude`. **Commit docs inside it (`cd my-docs && git ...`), never into this repo; ask
  before pushing.** Documents plus reusable scripts and tool source only; never generated output (screenshots,
  video, binaries, run results), scratch files, secrets or session info (its `.gitignore` and pre-commit hook
  enforce that). `my-docs/out/` holds tool results (ignored). See `my-docs/README.md`; tools are in
  `my-docs/tools/` (save-state profiling, boot smoke tests, bit-exact render checks). Run tools from this repo's
  root.
- `fork-specific/` — **only the files the build needs**: `arcade.flt`, `arcade-games.txt` and
  `tools/prune_arcade_filter.py` (Batocera's `libretro-mame.mk` uses `arcade.flt`). Docs and other tools do not go
  here.
- `src/mame/<manufacturer>/*.cpp` — per-driver code by manufacturer. A driver's video code is usually
  `<name>_v.cpp` or `<name>_video.cpp`.
- `src/devices/video/` — shared video chips (`voodoo*.cpp`, `psx.cpp`). `src/emu/` — core framework (`screen.cpp`,
  `render.cpp`).
- `src/osd/libretro/` — **the OSD this fork builds**. `sdl/`, `windows/`, `bgfx/`, `hlsl/` are for standalone MAME
  and unused here; do not touch them.
- `Makefile.libretro` is the build entry; `scripts/src/osd/retro.lua`, `retro_modules.lua` configure GENie for
  the libretro OSD.

## Rendering architecture

Software path: a driver's `screen_update_*` callback fills a `bitmap_rgb32` on the CPU, `render.cpp` composites it,
and `src/osd/libretro/drawretro.cpp` (via `rendersw.hxx`) hands the raw buffer to libretro's `video_cb`. BGFX/HLSL
shader chains are not used. So GPU acceleration means replacing a driver's rasterizer with one that issues real
GPU draws, and returning the result as a bigger normal bitmap. MAME's compositor and the OSD's per-frame geometry
reporting (`window.cpp`, from the screen's live `visible_area()`) handle a screen larger than its native size
without core changes.

- **Host-GPU engine: Vulkan only** (`src/osd/libretro/libretro-internal/retro_vk_device`, `retro_vk_engine`,
  `retro_vk_shader` `.{h,cpp}`). It uses a **private Vulkan instance and device independent of RetroArch's**; the
  libretro `RETRO_HW_RENDER` scaffolding in `libretro.cpp` (guarded by `HAVE_OPENGL`, includes a nonexistent
  `retroogl.c`) is dead and must stay unused. The Vulkan loader is `dlopen()`ed, so there are no build-time
  Vulkan dependencies. Without a Vulkan 1.3 device the GPU path is off and the system renders in software. The
  old EGL/OpenGL engine was removed (2026-10-26). Plans/status: `my-docs/gpu-vulkan-only-plan.md`,
  `my-docs/plans-archived/gpu-vulkan-plan.md`, `my-docs/plans-archived/gpu-3d-renderer-plan.md`.
- Gated by the **`HAVE_RETRO_GPU_TARGET`** define (on by default on Linux, set in `Makefile.libretro`; pass it
  empty to leave it out). Never use `HAVE_OPENGL` for this feature. `retromain.cpp`/`libretro.cpp` are compiled
  twice (once into `libosd_retro.a` via `retro.lua`, once directly into the `mame` project via
  `scripts/src/main.lua` ~line 414); only the second copy reaches `mame_libretro.so`, so define/file changes must
  be made in both places. glslang (bgfx's copy, `glslang` project in `scripts/src/3rdparty.lua`) is built in for
  run-time shader compilation.
- Generic interface: `src/osd/interface/gpurender.h`; `osd_interface::get_gpu_render_target()` returns nullptr by
  default so other OSDs need no changes.
- Every host-GPU path needs a core option (or switch) to fall back to software; keep it working. Generic options
  are `mame_3d_gpu`, `mame_3d_supersample`, `mame_3d_output_scale`, `mame_3d_msaa`, `mame_3d_texfilter` and
  `mame_3d_aniso`; PS1 has the `mame_psx_gpu_*` set.
  `rendersw`-path drivers you have not converted must keep producing output through `bitmap_rgb32`.
- Treat `src/mame/<mfg>/*_v.cpp` rasterizer rewrites as the highest-risk changes in this codebase.

### Sony ZN / PS1 GPU (`psxgpu_device`, `src/devices/video/psx.cpp`)

The first and most shared GPU-offloaded target, used by Sony ZN (`zn.cpp`, Brave Blade), Taito G-NET
(`taitogn.cpp`), Konami GQ/GV/573/Twinkle, Namco System 10/11/12 and the PS1 console driver. A regression
affects every board, so test more than one (`brvblade`, `starswep`, `gdarius2`, `raystorm`, `raycris`; `nagano98`
needs its CHD). Core option `mame_psx_gpu_hle` (disabled/2x/4x). Invariants learned the hard way:
- The GPU target is a **persistent framebuffer like real VRAM**: clear only on allocation, never per frame.
- **Every** primitive that lands on screen must reach the GPU target (polygons, rectangles, sprites, dots, lines,
  GP0 `0x02` fill, `0x80` VRAM copy, `0xA0` CPU-to-VRAM stamp), or stale pixels ghost. `0x02`/`0xA0` ignore the
  draw-area clip and draw offset (`gpu_force_no_clip()`); `0x80` is a GPU-side copy because its source may exist
  only on the GPU.
- Respect the draw-area clip, the **oversized-triangle cull** (>=1024 in x, >=512 in y, on raw native coordinates),
  display-start windowing (double buffering), overscan border cropping, 24-bit display mode and interleaved
  texture pages, exactly as the software `update_screen()` does. The target is the full VRAM height.
- Pre-scale the libretro OSD's `max_width`/`max_height` by the GPU scale in `retro_load_game()`; crossing the
  ceiling forces a full `SET_SYSTEM_AV_INFO` and RetroArch's threaded video driver crashes on that reinit.
- PGXP lives in `src/devices/cpu/psx/gte.cpp` (RTPS/RTPT) with options `mame_psx_gpu_pgxp*`
  (`_tol`, `_vcache`). **Beetle PSX HW (`libretro/beetle-psx-libretro`) is the gold standard: read its source and
  port its behaviour rather than inventing a fix.**
- G-NET titles are CHD-based and the first boot reflashes for ~4 minutes (real hardware behaviour); let it finish,
  later boots take under 90 s. Point RetroArch at the containing directory, not the `.chd`.

## Building

```sh
# Preferred: -j10 in a scope that systemd-oomd leaves alone
systemd-run --user --scope -p ManagedOOMPreference=omit -- \
  env MAME_DYNAMIC_LIBSTDCXX=1 make -f Makefile.libretro -j10 PREMAKE=0 SOURCEFILTER=fork-specific/arcade.flt
make -f Makefile.libretro -j4          # fallback without the scope
```

- Output: `mame_libretro.so` at the repo root. Use `-j10` only inside the `systemd-run` scope (earlier `-j8`/`-j16`
  builds were killed, probably by systemd-oomd); without the scope use `-j4`. If a build still dies, check
  `journalctl -u systemd-oomd` and `journalctl -k | grep -i oom`.
- **Run the build with the Bash tool's `run_in_background`**, output to a log ending in a `BUILD-EXIT=$?` marker.
  Never write `pgrep -f` wait loops or `pkill -f` (the pattern matches the waiting shell itself).
- **`MAME_DYNAMIC_LIBSTDCXX=1`** is required on this machine (only the 32-bit static `libstdc++.a` is installed;
  upstream's `-static-libstdc++` link fails otherwise).
- `DEBUG=1 make -f Makefile.libretro ...` builds with symbols (`CONFIG=libretrodbg`) for gdb/perf.
- No `SUBTARGET` is set, so a full build compiles every driver (~4,600 files); always use the arcade filter below.
- **The make wrapper intermittently skips genie regeneration** after a `.lua` edit. Run genie directly, confirm
  `Generating "..."` output, grep the generated `.make` file for your change, then build with `PREMAKE=0`:
  ```sh
  MAME_DYNAMIC_LIBSTDCXX=1 3rdparty/genie/bin/linux/genie --LIBRETRO_CPU=x86_64 --with-emulator --OPTIMIZE=3 \
    --NOWERROR='1' --target='mame' --subtarget='mame' --build-dir='build' --NO_USE_MIDI='1' --NO_USE_PORTAUDIO='1' \
    --PYTHON_EXECUTABLE='python3' --SOURCEFILTER='fork-specific/arcade.flt' --HAVE_RETRO_GPU_TARGET='1' \
    --osd='retro' --targetos='linux' --PLATFORM='x86' --gcc=linux-gcc --gcc_version=16.2.1 gmake
  ```
- Generated object rules depend on `$(MAKEFILE)`, which is never defined, so **changing only a define/flag never
  recompiles an object**: `rm` the specific stale `.o` files.
- **After force-killing a build**, run `find build/libretro/obj -name "*.o" -size 0` and delete any hits. Killing
  just the top-level `make` is not enough; kill the whole process group. A truncated `.o` links "successfully"
  but gives undefined symbols at `dlopen()` time.

### Arcade-only build filter

`fork-specific/arcade.flt` is a `SOURCEFILTER=` list of `src/mame/<mfg>/<driver>.cpp` paths (relative to the repo
root) restricting the build to video-game driver files. It is tracked in git and used by Batocera builds, so it is
the only copy. Every GPU candidate driver is arcade and stays in. `fork-specific/tools/prune_arcade_filter.py`
strips fruit machines, slots/poker/bingo, mahjong, quiz, pinball, mechanical games and consoles/computers (wrongly
caught video games go in its `KEEP_GAMES`, missed files in `EXCLUDE_FILES`/`EXCLUDE_DIRS`); the remaining romsets
are listed in `fork-specific/arcade-games.txt`.

**Regenerating** (after any upstream merge that renames, moves or splits `src/mame/` driver files; commit it with
the merge):
```sh
python3 /var/home/bazzite/Projects/libretro/speed-mame/make_arcade_filter.py src/mame fork-specific/arcade.flt
python3 fork-specific/tools/prune_arcade_filter.py    # also rewrites fork-specific/arcade-games.txt
python3 scripts/build/makedep.py -r . filterproject -t mame_arcade -f fork-specific/arcade.flt src/mame/mame.lst > /dev/null
# expect: exit 0, "N source file(s) found" on stderr, no errors
```
The static check does not catch everything; a real build through to link is the only full proof (watch for
`undefined reference`). Background: `/var/home/bazzite/Projects/libretro/speed-mame/README.md`.

## Test ROMs

**Always launch games (play or testing) from `/var/home/bazzite/Projects/mame-roms/roms/`**: the current WIP set
with valid CHDs. Some drivers need a separate BIOS zip there; check the driver's `ROM_START` parent relationship
if a set fails to load. Check a CHD exists before choosing a CHD game. Test ROMs are disposable: delete stale
`.dif`/nvram/state files that block a test boot without asking.

## Running/testing in RetroArch

The Flatpak RetroArch can't load a host-built core (host glibc 2.43 vs the Flatpak runtime's 2.42). Use the
**`mame-dev` Distrobox** (Fedora rawhide, RetroArch installed inside, shares `$HOME` and the display):

```sh
distrobox enter mame-dev -- retroarch -v -L /var/home/bazzite/Projects/libretro/mame/mame_libretro.so /var/home/bazzite/Projects/mame-roms/roms/daytona.zip
```

- After any driver-rasterizer or HW-render change, do a real smoke test like this against a romset for that
  driver. This is accuracy-sensitive code; a clean compile means nothing.
- **Stall check**: if a single-game run goes past 4 minutes (except a first G-NET boot, or a run meant to be long),
  look at the log tail and one screenshot; if it is not making progress (repeating shader/GL error, black or
  frozen frame, hung boot, missing ROM/CHD, blocked dialog), `pkill -x retroarch` and verify `pgrep -x retroarch`
  is empty. A `timeout` around `distrobox enter` leaves MAME running inside; use `timeout` inside and check
  `pgrep` after.
- RetroArch config is `~/.config/retroarch/` (same path inside the container). Per-core option overrides are in
  `~/.config/retroarch/config/MAME/MAME.opt` (`key = "value"` lines; safe to hand-edit between runs). Core options
  are defined in `src/osd/libretro/libretro-internal/libretro_core_options.h`.
- `-v` gives verbose frontend logging.

## Debugging / monitoring progress

The graphical debugger is compiled out (`none.cpp`, `USE_QTDEBUG=0`). Use MAME's Lua console/plugins through two
core options (category **Debug**, or edit `MAME.opt`):
- **`mame_lua_console`** (`disabled`/`enabled`): passes `-console` (REPL on stdin/stdout; only usable when
  RetroArch is launched from a real terminal).
- **`mame_debug_plugin`** (`none`/`gdbstub`/`cheatfind`/`hiscore`/`timer`): passes `-plugin <name>`. `gdbstub` only
  implements the i386 register map. Plugins are read from `<retroarch system dir>/mame/plugins`
  (`~/.config/retroarch/system/mame/plugins/`); copy this repo's `plugins/` there. To add a plugin, add its name to
  the option's value list in `libretro_core_options.h`.
- Wiring: `check_variables()` in `libretro.cpp` reads them; `Set_Default_Option()` in `retro_init.cpp` turns them
  into `-console` / `-plugin`. Mirror `mouse_enable` when adding options.
- **gdb** on the Distrobox hits an internal bug on `mame_libretro.so`'s debug info; workaround: `set
  auto-solib-add off` before `run`, then `sharedlibrary mame_libretro` after the crash.
- Temporary `fprintf(stderr, ...)` at the suspect site beats `LOGMASKED` (no rebuild-and-filter) for "GPU path
  differs from software" bugs; remove it afterwards. To screenshot a deterministic state: copy a `.state` to
  `~/.config/retroarch/states/MAME/<rom>.state`, launch with `-e 0 --appendconfig <file>` (see below), wait for
  the log line, send `PAUSE_TOGGLE` then `SCREENSHOT` over UDP to 127.0.0.1:55355 (python3 socket; no `nc`).
- For performance, prefer host tools (`mangohud`, `radeontop`, RenderDoc) over MAME's own frame timing.

### Screenshots

**Always use RetroArch's own screenshot** (UDP `SCREENSHOT` with `network_cmd_enable = "true"` and
`screenshot_directory` in an `--appendconfig` file; `profile_run.py --shot-every` and `smoke.py` do this). **Never
use a desktop capture tool** (`spectacle`, grim): it grabs the active window and has captured the user's terminal.
For frame-exact captures use `my-docs/tools/snap_run.sh` (MAME's own `video:snapshot()`).

## Working conventions for this project

- **Never `pkill -f` or `pgrep -f` with a pattern that appears in the command itself.** The shell running the
  command matches and kills itself (exit 144). Use `pkill -x retroarch`, or kill by PID.

- **Fix games the way the real hardware works (upstream-PR-able work).** First try a hardware-appropriate solution
  that keeps to the spirit of the original design. Example: Apache 3's sprite rotation first worked with a large
  off-screen buffer, but the scanline-at-a-time rotation (`tatsumi/tzbx15_sprites.cpp`) matches the chip and is
  fit for an upstream PR. Making an imperfect or broken game correct is PR-able, so write it to upstream
  standards: hardware-accurate, no libretro/fork-only hooks in shared code, separable from fork-only changes.
  **Exception**: fork-only features upstream would never take (the PS1 GPU HLE/PGXP path, GPU-path Voodoo
  supersampling, gamepad steering and similar) don't need to follow the hardware; make them work and efficient.

- **Code comments describe how the hardware works (or is assumed to work), never the edit.** This applies to all
  work. Write comments about what the chip, board or code does, as if it had always been written that way. No
  change-describing wording ("now", "no longer", "previously", "instead of", "fixes", "was wrong", "not a ... any
  more"). Mark inferred hardware behaviour as such ("appears to", "assumed"). What changed and why belongs in the
  commit message. From upstream review (mamedev PR #16437): capitalise `TODO`/`FIXME`/`NOTE`; write TODO items as
  actions ("verify X against hardware"); yes/no feature DIPs use `DEF_STR( No )`/`DEF_STR( Yes )`; use
  `auto *const` for obvious pointer types; parenthesise conditional-operator operands that contain binary
  operators.

- **Follow the MAME C++ coding standards** (https://docs.mamedev.org/contributing/cxx.html) in all new or changed
  code, fork-only or upstream-bound:
  - **Existing style wins**: in an existing file, match the style already there.
  - **Format**: UTF-8, files end with a newline, printable ASCII only outside comments and strings. Tabs (one per
    scope, tab stops of 4; continuation lines get two tabs; spaces only for alignment within a line). K&R or
    Allman bracing, consistent within an `if`/`else` chain, no blank lines between `if`/`else if`/`else` blocks. A
    space between a control keyword and its `(`, single spacing around binary operators. Run `srcclean` on
    modified files before a PR.
  - **Naming**: macros, constants, enumerators and device types are `SCREAMING_SNAKE_CASE`. Functions, classes,
    enum type names and public members are `snake_case`. Template parameters are `LlamaCase`. Abstract bases end
    in `_base`; concrete `driver_device`s end in `_state`; other devices in `_device`; interfaces are
    `device_..._interface`. Include guards are `MAME_` plus the capitalised path. Data members may use `m_`/`s_`
    (not on nested structs). Avoid double underscores, `_` + capital, leading underscores and `_t` suffixes.
  - **Literals and variables**: avoid octal; lowercase hex; 64-bit literals suffixed `LL`; group digits where it
    reads better. Save-state members need a fixed-size type, never plain `int`.
  - **Scope and const**: declare variables at first use in the narrowest scope; `const` where possible. Constant
    arrays are `constexpr` in screaming snake case. Enums/structs used by one device go inside its class.
  - **Comments**: English, `//` preferred. No commented-out code in a PR (`if (false)` for code kept for later,
    not `#if 0`).
  - **Helpers**: use `BIT(value, bit)` / `BIT(value, bit, width)` and `eminline.h` functions instead of hand-written
    masks and shifts.
  - **Logging**: `logerror` for developer messages, `osd_printf_verbose` for user diagnostics. Use `logmacro.h`
    (`VERBOSE`, `LOG`, `LOGMASKED`, masks from bit 1). Set `VERBOSE` to 0 and drop any `LOG_OUTPUT_FUNC` override
    before a PR.
  - **File structure**: start with `// license:` (SPDX) and `// copyright-holders:`. `emu.h` first; then
    same-directory headers, `src/devices`, `src/emu`, `src/lib/formats`, `src/lib/util`, OSD, C++ standard
    library, C standard library, OS-specific, layout (`.lh`) headers, then `logmacro.h` last; alphabetical within
    each group. Driver classes are declared in the source file inside an anonymous namespace, with only the
    `GAME`/`COMP`/`CONS` macros outside it.
  - **Class layout**: one `public`, `protected` and `private` section each where practical, least access needed;
    group constants, types, data members, instance functions, then static functions; use constructor delegation.

- Changes to `src/devices/video/psx.cpp` or other shared devices have a large blast radius; test more than one
  driver that uses them. When touching the HW-render block in `libretro.cpp`, keep changes scoped to that file
  and the `retro_modules.lua`/`Makefile.libretro` build-flag plumbing.
