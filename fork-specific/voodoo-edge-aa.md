# Voodoo edge anti-aliasing (prototype)

Branch: `voodoo-edge-aa` (not merged). Switch: env var `MAME_VOODOO_EDGEAA` (0 = off, 1 = on,
2 = debug view: every smoothed pixel painted red). No core option yet.

## Goal

Soften the stair-stepped outlines of 3D geometry (the main visible benefit of 2x rendering)
without 2x rendering's frame-buffer sync work and ~5x rasteriser cost.

## Why not coverage AA in the rasteriser

Blending partially covered edge pixels needs back-to-front sorted geometry. Otherwise internal
mesh edges show hairline cracks and partial pixels write depth that blocks later geometry (the
same mechanism as the billboard lines fixed in `voodoo-texture-supersampling.md`). Voodoo 1-3
hardware had no full-scene AA either.

## Depth trace (jpark3 attract, 2026-09-27)

Pixel share per triangle state:
- perspective 3D, depth test LEQUAL, depth write on: 40-80% (main geometry, W-buffer)
- perspective 3D, depth test ALWAYS, no depth write: 16-37% (backdrops)
- non-perspective 2D, no depth test/write: 1.5-21% (HUD, text)
- non-perspective with depth write: a few %
- fastfill clears depth every frame (sometimes without colour)

So depth discontinuities find geometry silhouettes, but the HUD doesn't write depth and would
inherit the 3D depth under it. Hence the per-pixel 3D mask below.

## Design

1. `voodoo_renderer::write_pixel()` stamps a per-pixel mask (one byte per frame-buffer pixel,
   indexed by address) on every colour write: the current frame's stamp if the triangle is
   depth-tested (3D), 0 otherwise (2D/HUD). A rolling stamp avoids clearing the mask.
2. `voodoo_1_device::compute_edge_aa()` runs in `swap_buffers()` after `rotate_buffers()`: the new
   front buffer is the finished frame and the aux buffer still holds its depth. Candidates are
   3D pixels next to a depth jump (> 0x200 raw) or a non-3D pixel. On those it runs an FXAA-style
   analysis (local contrast gate, edge orientation, search along the edge up to 8 pixels, sub-pixel
   term) and stores per screen pixel a blend direction (N/S/W/E) and weight in `m_aa_map`.
3. `update_common()` applies the blends at scan-out when the front buffer matches the buffer the
   map was computed for. Game-visible frame-buffer memory is never modified, so LFB reads,
   blits, render-to-texture and save states are unaffected. The map isn't saved; after a state
   load it's rebuilt at the next swap.

## Prototype results (jpark3, frame-exact `snap_run.sh`)

- Debug view: detects the island ridge and island outlines, the edges between hillside layers,
  jungle tree trunks, the raptor and cut-out foliage. The HUD text is never marked. Background
  trees drawn without depth writes aren't detected (no depth edges).
- On: smooths ~4.5k-18k edge pixels per 3D frame, mean change 10-14/255 per pixel. The
  spinosaurus silhouette against the sky and the tree tops lose their hard stair-steps; texture
  interiors and HUD icons unchanged. Noisy foliage cut-outs (island ridge) change little.
- 2D-only screens (attract title/ranking/copyright): pixel-identical with it on.
- Off (0): pixel-identical to the pre-feature baseline on the frames checked.
- **No performance numbers yet**: a Batocera build was using the CPU during this work.

## Next steps

1. User live look (`MAME_VOODOO_EDGEAA=1`), including motion.
2. Performance measurement once the machine is idle. `compute_edge_aa()` is single-threaded
   and uses floats over the visible area; if it's too slow, restrict the luma pass to candidate
   rows or thread it.
3. Tuning: `DEPTH_EDGE`, `EDGE_MIN`/`EDGE_REL`, `SUBPIX` (constants at the top of
   `compute_edge_aa()`), maybe a strength setting.
4. Core option (e.g. `mame_voodoo_edge_aa`: disabled/enabled), plumbed like
   `mame_voodoo_tex_supersample`.
5. Regression on gticlub2, gauntdl, calspeed (Voodoo 1/2 have separate frame-buffer/aux layout;
   check the aux buffer is valid at swap there too).
