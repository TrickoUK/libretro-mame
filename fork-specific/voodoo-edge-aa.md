# Voodoo edge anti-aliasing

Core option `mame_voodoo_edge_aa` (Video: Disabled/Enabled, default Disabled), via
`osd_interface::voodoo_edge_aa()`. The env var `MAME_VOODOO_EDGEAA` overrides it for testing
(0 = off, 1 = on, 2 = debug view: every smoothed pixel painted red). Developed on branch
`voodoo-edge-aa`.

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
   indexed by address) on every colour write: the current frame's stamp if the triangle is 3D,
   0 otherwise. 3D = depth test enabled with a real compare, not ALWAYS (7): jpark3's HUD has no
   depth test, calspeed draws its logo/text with ALWAYS plus depth writes. A rolling stamp avoids
   clearing the mask.
2. `voodoo_1_device::compute_edge_aa()` runs in `swap_buffers()` after `rotate_buffers()`: the new
   front buffer is the finished frame and the aux buffer still holds its depth. Candidates are
   3D pixels with a depth jump (> 0x200 raw) to a valid partner: other 3D geometry, or non-3D
   content clearly *behind* (a backdrop). Non-3D content not behind is an overlay (HUD/logo/text
   drawn on top, leaving the 3D depth or writing a nearer one) and is never blended toward; the
   final FXAA blend direction must also point at a valid partner. Both games use LESS/LEQUAL
   depth tests, so larger values are farther. On those it runs an FXAA-style
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
- Speed was measured later, once the machine was idle (see below).

## Live check, speed, core option, regression (2026-09-27)

- User live look on jpark3: "a nice improvement".
- Speed (jpark3, 150 s attract, machine idle): main thread 49% -> 53% of a core, render threads
  unchanged, full speed throughout with no dips. `compute_edge_aa()` stays single-threaded.
- Core option `mame_voodoo_edge_aa` added; option alone gives pixel-identical output to the env var.
- calspeed (Seattle, Voodoo 1) first showed its logo and "CREDITS" text outlined: they're drawn
  with depth test ALWAYS + depth writes, and 3D pixels next to them were blended toward them.
  Fixed by the ALWAYS rule and the "partner" rule above; logo/text now untouched, track/car/
  pillar edges smoothed.
- gauntdl (Vegas): table/geometry edges smoothed. Its HUD ("0 CREDITS", logo) is depth-tested
  so it counts as 3D; a few pixels around it change (20 px around the text, max 50/255), not
  visible in practice. Known limitation.
- A frame whose 3D scene starts one row below a black row 0 (gauntdl) gets that top row blended
  slightly toward black: a real scene edge, harmless.
- jpark3 after the rule changes: detection 5-30% lower per frame, HUD still untouched, 2D
  screens still pixel-identical.

## Possible follow-ups

- Tuning constants (top of `compute_edge_aa()`), maybe a strength setting.
- Exclude gauntdl-style depth-tested HUDs (e.g. by depth near the near plane) if it ever shows.
