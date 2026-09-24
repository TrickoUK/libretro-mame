# Idea: GPU-offloaded 3dfx Voodoo rendering (not started)

Status: **idea only** (2026-09-24). Nothing implemented. Captures a feasibility
assessment made after the Viper/Voodoo 3 fixes (see `viper-investigation.md`).

## Short answer

Conceptually easier than the PS1 GPU work, practically harder. The 3D part maps
almost 1:1 onto a modern GPU; the hard part is memory coherence, and the payoff
is mainly internal resolution rather than raw speed.

## Why it's a good fit

- **The Voodoo is a fixed-function, OpenGL-1.x-style rasteriser.**
  Perspective-correct texturing, Z/W buffering, bilinear filtering, mipmaps,
  blend factors, alpha test, fog and chroma key all have direct GPU equivalents
  or are a few lines of fragment shader. (PS1 was the awkward fit: we had to
  fake its quirks - affine warping, no depth.)
- **Precedent**: Glide wrappers (dgVoodoo, nGlide, OpenGlide) already run
  Voodoo-era rendering on modern GPUs.
- **One clean entry point**: every triangle goes through
  `voodoo_renderer::enqueue_triangle(poly_data&, vertex_t const*)`
  (`src/devices/video/voodoo_render.cpp`, ~line 1337) with all render state
  already decoded - the equivalent of the `gpu_submit_*` hooks in `psx.cpp`.
- **The PS1 infrastructure is reusable**: private EGL context, deferred
  per-frame command queue, batching, persistent FBO, N× readback, core-option
  gating (`osd::gpu_render_target`, `retro_gpu_target.cpp`).

## Where the work is

1. **Parameter conversion.** Voodoo receives per-triangle start values plus
   dx/dy gradients, not per-vertex values. Everything it interpolates (colour,
   Z, 1/W, S/W, T/W) is linear in screen space, so evaluating the gradients at
   the three vertices gives exact per-vertex attributes.
2. **The shader.** One "uber-shader" driven by `fbzColorPath`, `fbzMode`,
   `alphaMode`, `fogMode`, `textureMode`. The detail that makes it a real
   project:
   - chaining two TMUs through the combine unit (TMU1 feeds TMU0)
   - per-pixel LOD selection
   - NCC and palette texture formats
   - 16-bit W-buffer depth
   - 64-entry fog table
   - stipple
   - dithered RGB565 output
3. **Interface extension.** `osd::gpu_render_target` is position/colour/UV plus
   a few blend modes. Voodoo needs depth, W, two texture units, full
   blend-factor control and many more uniforms.
4. **Memory coherence - the real hard part.**
   - Games read and write the linear framebuffer (LFB) directly
     (`internal_lfb_r`/`internal_lfb_w` in `voodoo.cpp`). Writes must be
     pushed into the FBO; reads force a GPU flush + readback (slow if a game
     does it every frame).
   - Textures live in emulated RAM the CPU rewrites
     (`internal_texture_w`), so the GPU texture cache needs dirty tracking.
   - Banshee / Voodoo 3 are worse: framebuffer, textures and the 2D engine
     share one memory, so games render-to-texture and blit between 3D and 2D
     regions (e.g. the screen-to-screen blits added in `c87ab478763`). Each
     path needs a GPU-side version or a sync point - the same class of bug as
     PS1's MoveImage/CPU-to-VRAM ghosting, but with more paths.
5. **Timing.** Games poll the busy status (Vegas uses `set_status_cycles`).
   MAME models status from pixel work done; a GPU path would have to estimate
   it so games don't hang or run fast.
6. **Buffer swaps and scan-out.** Front/back/triple buffering, swap-on-vblank,
   the video 2×2 filter. Modest - similar to the PS1 display-start port.

## Would it help speed?

Probably not much. MAME's Voodoo renderer is already multithreaded
(`poly_manager`) with templated fast paths, and in the Viper profiling the
bottleneck was the emulated PowerPC, not rasterisation. Clear wins would be:

- higher internal resolution (2x/4x)
- cleaner filtering
- freeing the render threads' CPU

**Profile a Vegas or Seattle game first** to confirm how much time rendering
actually takes before committing to this.

## Suggested approach if picked up

1. Start with **Voodoo 1/2** (Seattle, early Vegas games) - separate
   framebuffer and texture memory, far fewer coherence paths.
2. Route triangles, fastfill, swap and LFB writes to the GPU target; make LFB
   reads a flush + readback.
3. Only then move to Banshee / Voodoo 3 (Viper, sf2049, iteagle) and add the
   2D engine and shared memory.

Gate it behind a core option like `mame_psx_gpu_hle`. Rough estimate: a
Voodoo 1/2 prototype is a few times the PS1 effort; Banshee/Voodoo 3
correctness is the long tail.

## Affected systems (for testing)

| Chip | Boards / games (local collection) |
|---|---|
| Voodoo 1/2 | Seattle (`sfrush`, `calspeed`, `carnevil`...), Vegas early (`gauntleg`, `gauntdl`, `tenthdeg`, `warfa`, `roadburn`) |
| Banshee | Vegas `vegasban` (`nbashowt`, `nbanfl`, `nbagold`), `xtom3d`, `quakeat` |
| Voodoo 3 | Konami Viper (all), Vegas `vegasv3`/`denver` (`cartfury`, `sf2049*`), iteagle (`carnking`, `bbh`...), `comebaby` |
