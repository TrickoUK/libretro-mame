# Voodoo texture supersampling (jpark3 shimmer)

Core option `mame_voodoo_tex_supersample` (Video: Disabled/2x2/3x3/4x4, default Disabled).
Developed on branch `voodoo-texture-supersample`, merged into `arcade-focused` 2026-09-27.

## Problem

jpark3 (Konami Viper, Voodoo 3) shows shimmering/sparkling pixels in most scenes, even with a
good CRT shader on Batocera (less, but still noticeable).

## Cause (register trace, 2026-09-26)

A temporary trace in `voodoo_1_device` (pixel counts per render state at triangle enqueue,
dumped every 300 swaps, plus `vidProcCfg` at scanout; reverted after) over 3 minutes of
cold-boot attract mode showed:

- **Dithering off** on 100% of pixels (`fbzMode` bit 8 = 0). So the 16-bit dither and a
  "V3 scanout de-dither filter" idea are irrelevant here.
- **LOD dither never enabled**; bilinear min/mag on almost everything (point sampling only on a
  small HUD format).
- **Mipmapping effectively off**: nearly every triangle clamps `lodmin`/`lodmax` to a 0.25-wide
  window (raw 2-3, 6-7, 22-23), i.e. one mip level chosen per object. Only a two-TMU trilinear
  mode (`lod 8-35`, `tc_mselect` 5) has a real range, and it is a large share only in a few
  scenes.
- The 3D renders at **512x384** (`SET_SYSTEM_AV_INFO: 512x384`). The real V3 overlay scaled it
  to 1024x768 (`vidProcCfg = 02e24101`; not decoded against the spec).

So the shimmer is texture aliasing: detailed textures shrunk onto a low-res screen with one
bilinear tap from a fixed LOD. The real cabinet very likely looked the same.

## Prototype (this branch)

`rasterizer_texture::fetch_texel()` (`src/devices/video/voodoo_render.cpp`) is split into
`compute_st()` (iterated S/T/W to 24.8 S/T) and `sample_texel()` (point/bilinear at a LOD).
When the pixel's unclamped, unbiased LOD exceeds the LOD actually sampled by >= 0.5
(footprint > ~1.4 texels), it averages an NxN grid of samples across the pixel
(N = 2 below 1.0 LOD excess, 3 below ~1.58, else 4, capped by the setting). Sample positions
come from `texture_footprint` (per-pixel dS/dT/dW in X and Y, filled in
`voodoo_renderer::rasterizer()`), and each goes through the normal perspective divide. Pixels at
1:1 or magnified take the original path unchanged.

Switch: core option `mame_voodoo_tex_supersample` -> `check_variables()` (`libretro.cpp`) ->
`voodoo_tex_supersample_max` -> `osd_interface::voodoo_tex_supersample()` (default 0 for other
OSDs) -> read in `voodoo_1_device::device_start()` -> `voodoo_renderer::set_tex_supersample()`.
The env var `MAME_VOODOO_TEXSS=0/2/3/4` overrides it (used by the test tools).

```sh
distrobox enter mame-dev -- env MAME_VOODOO_TEXSS=4 retroarch -v -L mame_libretro.so .../jpark3.zip
python3 fork-specific/tools/profile_run.py jpark3 texss4 --warmup 20 --duration 150 --shot-every 10 --no-perf --env MAME_VOODOO_TEXSS=4
```

## Results so far

- jpark3, 150 s attract, off vs 4: speed ~1.0x both. Voodoo worker threads 18% -> 28.5% of a
  core each, main thread 50% -> 58%.
- Island scene: hillside foliage goes from per-pixel sparkle noise to a coherent texture.
  User confirmed live: "much better image".
- Initial prototype had **fringing along layer boundaries** (user report; worst in jpark3,
  which builds foliage and the island hills from stacked 2D billboards). Fixed 2026-09-27, see
  below.

## Fringing fix (2026-09-27)

A trace of per-triangle state (chroma key, alpha test, blend, texture format) during attract
mode showed chroma key is never used. Nearly every textured triangle uses alpha blend
(src alpha / 1-src alpha) plus alpha test >= 0x7f or >= 0x0c, mostly with alpha-carrying formats
(6 = palette+alpha, 12 = ARGB4444, 2 = A8). Frame-exact snapshots (`tools/snap_run.sh`, frames
2400-4200 step 60) of off / plain-average / variants showed thin pale lines along the top edge of
each hillside billboard with supersampling on. The same lines exist faintly with it off.

Experiments, each on the same frame:
- Alpha-weighted averaging between samples: no visible change.
- Keeping samples inside the centre's texture repeat (wrap bleed): changed <200 pixels.
- Alpha-weighted bilinear taps, and even opaque-only taps: lines unchanged.
- Skipping samples in the texture's edge rows: lines unchanged (and it opened tile seams).
- Debug colour for supersampled pixels: the line pixels are on the supersampled path.

Conclusion: the lines come from **averaging alpha**. The cards are drawn with alpha blend and
depth writes at a low alpha-test threshold. Without supersampling, edge texels are mostly fully
opaque or transparent, so only a scattered few pixels are semi-transparent (blend with sky,
write depth, block the farther card = the faint dotted lines in the original). Averaged alpha
makes a continuous band of semi-transparent pixels along every card edge, which shows as a line.

**Fix**: supersample colour only. Alpha is the normal single sample at the pixel centre, so alpha
test, blending and depth writes cover exactly the pixels they did before. Kept as well:
alpha-weighted colour (between samples and inside each bilinear tap, so transparent texels'
RGB doesn't tint edges) and the wrap-repeat clamp (cheap, correct for billboards).
Result: the extra pale lines are gone. What remains (e.g. dark blue streaks) is identical to the
original render. Off path verified pixel-identical to the committed baseline (31 frames).

Cost (jpark3, 150 s attract, 4x4): full speed on average; Voodoo worker threads ~36% each
(plain average was 28.5%, off 18%); one 0.67x speed sample. Easy optimisation if needed:
accumulate premultiplied sums across all taps and divide once per pixel instead of per tap.

## Core option, optimisation and regression check (2026-09-27)

- User confirmed the colour-only version looks good live.
- Core option added (see top). Verified the option alone (no env var) gives pixel-identical
  output to `MAME_VOODOO_TEXSS=4`.
- The supersample path accumulates premultiplied sums (`premultiplied_sum`) over every bilinear
  tap and divides once per pixel, instead of per tap. Rounding-only change (mean 0.03/channel).
- Regression, off vs 4x4 snapshots in attract (`snap_run.sh`, frames 1800-7200 step 900):
  gticlub2 (Viper; user also checked it live), gauntdl (Vegas), calspeed (Seattle, Voodoo 1 with
  its coarser 4-bit bilinear). All clean: distant floors/tracks smoother, HUD/logos/alpha effects
  unchanged. `sfrush` can't be tested: its local CHD fails with "DIFF CHD ERROR: Invalid parent".
  Off path pixel-identical to the pre-feature baseline on jpark3.
- jpark3 speed (150 s attract; Voodoo worker thread CPU each, worst speed sample):
  off 18% / 0.92x, 2x2 24% / 0.91x, 3x3 29.5% / 0.94x, 4x4 34% / 0.70x (one or two dips per run
  in a heavy scene). **3x3 is the safe recommendation**; 4x4 for fast machines.

## Possible follow-ups

- Anisotropic grid (more samples along the major axis only) to make 4x4-quality cheaper.
- Skip the alpha weighting for formats without alpha (RGB565 etc.).

Longer term (not planned): full 2x supersampling of the Voodoo framebuffer would also fix edge
crawl, but it needs a shadow high-res buffer kept in sync with LFB access and 2D blits.
