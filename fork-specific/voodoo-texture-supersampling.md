# Voodoo texture supersampling (jpark3 shimmer) - WIP

Branch: `voodoo-texture-supersample` (not merged into `arcade-focused` yet).

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

Switch (prototype only): env var `MAME_VOODOO_TEXSS=0/2/3/4` (max N), read in
`voodoo_1_device::device_start()` in `voodoo.cpp` -> `voodoo_renderer::set_tex_supersample()`.
Default off.

```sh
distrobox enter mame-dev -- env MAME_VOODOO_TEXSS=4 retroarch -v -L mame_libretro.so .../jpark3.zip
python3 fork-specific/tools/profile_run.py jpark3 texss4 --warmup 20 --duration 150 --shot-every 10 --no-perf --env MAME_VOODOO_TEXSS=4
```

## Results so far

- jpark3, 150 s attract, off vs 4: speed ~1.0x both. Voodoo worker threads 18% -> 28.5% of a
  core each, main thread 50% -> 58%.
- Island scene: hillside foliage goes from per-pixel sparkle noise to a coherent texture.
  User confirmed live: "much better image".
- **Known artifact**: visible fringing along the boundaries of layers (cut-out / overlapping
  layer edges). User judged it less noticeable than the shimmer, but it should be fixed.
  Likely cause: the average is taken *before* the chroma-key and alpha tests, so texels from
  the keyed-out/transparent side bleed into the edge colour. Faint horizontal cyan lines on the
  hillside are pre-existing (present with the option off).

## Plan for the next session

1. **Fix the fringing.** Options, in order to try:
   - When chroma key is enabled (`fbzcp`/`fbzmode` chromakey) or the texture format has alpha
     used for alpha test, exclude keyed/zero-alpha samples from the average (average only the
     opaque samples, keep alpha as coverage), or skip supersampling for those triangles.
   - Compare against a simple "skip SS when chromakey/alphatest is on" build to see which
     layers are affected.
2. **Core option** replacing the env var: e.g. `mame_voodoo_tex_supersample`
   (disabled/2x2/4x4, default disabled), plumbed through the OSD like the `mame_psx_gpu_*`
   options (`libretro_core_options.h` -> `check_variables()` -> an `osd_interface` query ->
   `voodoo_renderer::set_tex_supersample()`), so Batocera can use it.
3. **Regression/visual pass** on other Voodoo boards (the renderer is shared): gticlub2 and
   thrild2 (Viper), a Seattle game (`sfrush`/`calspeed`) and a Vegas game (`gauntleg`). Check the
   off path is bit-identical (same-frame screenshots or frame hashes).
4. Performance on slower Batocera hardware: consider an anisotropic grid (more samples along
   the major axis only) if 4x4 is too heavy.
5. Then document in `viper-investigation.md`, merge to `arcade-focused`.

Longer term (not planned): full 2x supersampling of the Voodoo framebuffer would also fix edge
crawl, but it needs a shadow high-res buffer kept in sync with LFB access and 2D blits.
