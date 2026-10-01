# Custom change log

A dated record of this fork's own work on games and systems. Upstream MAME merges and
cherry-picks are left out unless we changed something to make them work. Each entry says what
changed and what you get from it. The technical detail is in `CLAUDE.md` and `fork-specific/*.md`.

Entries are oldest first. Add new work at the bottom, under its date.

---

## 2026-09-16

### Sony ZN / PS1 GPU (`psxgpu_device`): GPU-accelerated rendering
- The PS1 GPU used by Sony ZN, Taito G-NET, Namco System 10/11/12 and Konami GQ/GV/573 can now
  draw its polygons on the host GPU at a higher internal resolution. It uses a private EGL/OpenGL
  context and does not rely on RetroArch's hardware-render interface.
- Performance fixes: a texture page is uploaded once per frame instead of once per polygon, the
  whole frame is replayed in one GL context bind, and runs of triangles are batched into single
  draw calls.
- Screen contents now persist between frames the way real VRAM does. This removed black flicker
  frames. The draw-area clip is also respected, which removed edge flicker on large backdrops.
- New core option **`mame_psx_gpu_hle`** (Disabled/2x/4x, default Disabled). When it's off, the
  core behaves exactly as before.
- Textures are now decoded on the GPU in the fragment shader instead of on the CPU.
- Tested on Brave Blade (`brvblade`) and Star Sweep (`starswep`).

## 2026-09-17

### Sony ZN / PS1 GPU: crash and ghosting fixes (G-Darius Ver.2, Star Sweep, RayStorm)
- **gdarius2 crash**: when the game changed resolution, a scaled mode could go past the core's
  declared maximum size. RetroArch then restarted its video driver, which crashed. The maximum is
  now scaled up front, so this no longer happens.
- **Ghosting**: rectangles, sprites, dots, lines, VRAM fills, VRAM copies and CPU-to-VRAM image
  uploads were all still drawn only in software. Old pixels stayed on screen in the GPU image
  (gdarius2 in-game, starswep 2D attract). All of these now go through the GPU path as well.
- **RayStorm**: double buffering (the display-start address) and the overscan border crop now
  match the software renderer. This removed a duplicated strip at the bottom of the screen and a
  visible palette (CLUT) block in one corner.

### Taito G-NET: confirmed working with GPU rendering
- RayCrisis (`raycris`) runs on the GPU path with no code changes. The first boot re-flashes the
  card and takes about 4 minutes. Later boots are about 3x faster because the flash contents are
  kept in NVRAM.

### Sony ZN / PS1 GPU: PGXP geometry and texture correction, 4x MSAA
- PGXP-style tracking of the GTE's vertex output gives sub-pixel precision and real depth to
  vertices. This reduces PS1 polygon "wobble" and gives perspective-correct textures. Values are
  tracked through memory by address.
- Added 4x MSAA on the GPU render target for smoother polygon edges.

## 2026-09-18

### Sony ZN / PS1 GPU: texture filtering and 1x scale
- Added bilinear/trilinear and 3-point (N64-style) texture filtering to the GPU path. There are
  also options to leave sprites and 2D polygons unfiltered, so HUDs and text stay sharp.
- Added a 1x scale option: GPU rendering at native resolution.
- Fixed stale PGXP values being picked up. Each value is now checked again at every hop.

## 2026-09-21

### Sony ZN / PS1 GPU: matched to Beetle PSX HW (Cool Boarders Arcade Jam, Brave Blade J, Tekken Tag, Crypt Killer)
- **cbaj seams**: new option **`mame_psx_gpu_pgxp_tol`** (default 1px). It removes gaps between
  background polygons when only some of a polygon's vertices have PGXP data.
- **brvbladej blue screen**: the GPU path now drops oversized triangles the way the real GPU does,
  using Beetle's limits. Before this, huge off-screen quads covered the play area in flat colour.
- Semi-transparency now follows the per-texel STP bit, with the same filtered-opacity handling as
  Beetle.
- Added texture window support (GP0 E2) and a UV nudge for flipped 2D sprites.
- **Tekken Tag FMV**: 24-bit display mode (MDEC movies) was showing rainbow noise. It now decodes
  correctly.
- **PGXP vertex cache** (`mame_psx_gpu_pgxp_vcache`, default on): recovers precise positions for
  about 75% of vertices that had no direct PGXP data. The seams on Tekken Tag character limbs went
  from obvious to barely visible.
- **Crypt Killer (Konami GQ)**: interleaved texture pages are now supported. Before this, every
  texture was scrambled into stripes.

## 2026-09-22

### Konami M2 (Polystars, Total Vice, Evil Night): Triangle Engine speed-up, part 1
- The DSPP audio DSP clock was corrected to the separate audio oscillator documented for the
  board. This was an accuracy fix, with no speed change.
- The Triangle Engine rasterizer now skips blend maths it doesn't use, which made non-blended
  draws about 30-35% faster.
- Triangle Engine scanlines are now drawn on worker threads.
- Triangle Engine register writes no longer make the emulation thread wait. Each queued job keeps
  a snapshot of the settings it needs. After this, no single CPU core was stuck at 100% on
  Polystars.

## 2026-09-23

### Konami M2: full speed (Polystars benchmark ~0.54x -> ~1.0x)
- The PowerPC recompiler (DRC) no longer recompiles code blocks when only data-permission bits of
  a TLB entry change. This took Polystars from ~0.54x to ~0.86x speed.
- Triangle Engine work is now spread over 12 worker threads instead of 3, which reached full speed
  (~1.0x).
- Texture uploads to TRAM are copied 64 bits at a time, which cut main-thread texture loading
  from ~5.5% to ~0.5%.
- The DSPP audio DSP now skips idle polling loops that provably do nothing. The output is
  bit-identical, and its CPU share dropped from 13% to 7.5%.
- Per-pixel Triangle Engine work uses 43% less CPU, with bit-identical output.
- The user confirmed Polystars and Evil Night are playable.

### PowerPC 602/603 (Konami M2, Viper): code cache flush (Polystars black stage)
- **Polystars "STAGE1 START!" black screen** (a regression from the DRC fix above): the game's
  instruction-cache flash invalidate (HID0[ICFI]) was ignored, so code loaded for the stage never
  ran. Compiled code whose memory has changed is now thrown away when the game flushes the cache.

## 2026-09-24

### Konami Viper (GTI Club 2, Thrill Drive 2, Jurassic Park 3)
- **Speed**: the PowerPC recompiler was recompiling about 10,000 blocks a second. It now fills the
  fetch TLB entry at compile time instead. The gticlub2 attract minimum went from 0.72x to 0.99x.
- **Voodoo 3 textures**: Voodoo 3 now has its second texture unit, and multi-base texture
  addresses are fixed. This cleared the coloured-noise textures on gticlub2 (car windows, trees,
  signs, a dark-green sea).
- **jpark3 damage overlay**: added the Voodoo 2D screen-to-screen blit. The red damage flash now
  shows correctly instead of rainbow noise.
- **gticlub2 analog controls**: steering and pedals didn't work at all, because the I2C ADC read
  was wrong. Fixed.
- **thrild2 steering**: fixed the 8-bit wheel port. Added optional steering-curve and gamepad
  **Steering Smoothing** settings.
- **Save states**: Viper save states now load and run. Missing EPIC/I2C, K056230 and Banshee state
  is now saved, and the recompiler cache is flushed on load. Also backported an upstream DRC fix
  that sets the floating-point mode state on entry.
- Found that gticlub2 cars rolling at speed is a rule in the game itself, not an emulation bug.
  Steering Smoothing makes it easier to live with on a gamepad.

## 2026-09-25

### Light guns: Jurassic Park 3, and Sinden on Batocera
- **jpark3**: gun shots were always read as reloads. The gun port layout is fixed.
- **Libretro light gun input**: in "free" offscreen mode, the reload button now fires off screen.
  Gun Start/Select also press pad Start/Select, so coin and start work from the gun.
- Tested on a Batocera box with Total Vice, Evil Night and jpark3. Gun Start/Coin also needed a
  fix in Batocera's RetroArch (patch 019, Vulkan hunk). That fix is outside this repo.

## 2026-09-26

### Game fixes moved from Batocera patches into the fork
These used to be Batocera `libretro-mame` patches 003-007. They now live in the fork, so Batocera
builds get them straight from here.
- **Light gun reload auto-bind plugin** (`offscreenreload`): binds reload to BUTTON2 for light
  guns, except when the game uses BUTTON2 for gameplay.
- **Sega Model 2**: the 3D layer is rendered at 2x and scaled down, giving smoother polygon edges.
- **House of the Dead (`hotd`)**: no full-screen flash on every trigger pull.
- **Operation Wolf (`opwolf`)**: no full-screen flash on every trigger pull. This is a runtime
  patch, because the C-chip ROM check rejects a static one.
- **Gunblade NY (`gunblade`)**: the gun Y axis is reversed for the libretro light gun device.
- The arcade-only build filter (`fork-specific/arcade.flt`) is now tracked in the fork.

## 2026-09-27

### Voodoo (Viper, Seattle, Vegas and others): texture supersampling
- New core option **`mame_voodoo_tex_supersample`** (Disabled/2x2/3x3/4x4, default Disabled). It
  removes texture shimmer, such as on jpark3's scenery. Only the colour is supersampled, so
  alpha-blended billboards don't get fringes. 3x3 is the recommended setting.
- The user confirmed it on jpark3 and gticlub2.

### Voodoo: depth-guided edge anti-aliasing
- New core option **`mame_voodoo_edge_aa`** (Disabled/Enabled, default Disabled). It smooths
  jagged 3D polygon edges using the depth buffer, and never blends into HUDs or overlays. It costs
  about 4% of the main thread, with no speed dips. The user chose it over 2x rendering.

### Jurassic Park 3: 60 fps experiment (not adopted)
- Found the game's 30 fps frame lock and a patch that removes it. The game logic runs a fixed step
  per frame, so unlocking it makes the whole game run at double speed. The patch was written up
  and not shipped.

## 2026-09-28

### Pole Position / Pole Position II: gamepad stick steering
- The real wheel spins freely, and the game steers by how fast it turns. MAME's default stick
  mapping spun it about 5x too fast, so most of the stick's travel was full lock.
- New Machine Configuration setting **Steering Input: Gamepad stick** (default is the arcade
  wheel/spinner). The stick sets the spin speed, with a **Stick Steering Speed**
  (Slow/Medium/Fast/Very fast) and the gticlub2-style **Steering Response** curve. Holding the stick
  holds a steady turn, and letting go straightens up.
- The user confirmed it's now playable (Squared curve, Slow speed).

### Final Lap 1/2/3, Four Trax, Suzuka 8 Hours, Dirt Fox: steering response and smoothing
- The same **Steering Response** and **Steering Smoothing** settings as gticlub2, now in a shared
  helper (`src/mame/shared/gamepad_steering.h`). Default is arcade behaviour.
- The user confirmed Final Lap 3 is much better (Squared curve, Heavy smoothing). Final Lap 1/2
  need `lh5762.6n`, a ROM added upstream that 0.289 romsets don't have, so they're untested until
  a 0.290 set is available.

### Final Lap R: steering response and smoothing
- The same **Steering Response** and **Steering Smoothing** settings as the other Final Laps, using
  the shared helper. Default is arcade behaviour.
- The user tested it (Squared curve, Firm smoothing) and approved it.

### GTI Club 2 / Thrill Drive 2: steering code moved to the shared helper
- gticlub2/thrild2's Steering Response and Steering Smoothing now use the shared
  `gamepad_steering.h`, the same code as Pole Position and the Final Laps. The settings and saved
  cfg values are unchanged. Smoothing can now differ by at most 1 of 255 from the old code, which
  can't be felt.

### Speed Racer: steering response and smoothing
- The same **Steering Response** and **Steering Smoothing** settings as Final Lap R, from the shared
  helper. Default is arcade behaviour.
- The user tested it (Squared curve, Medium+ smoothing) and approved it.

### Road Blasters: steering response and smoothing
- The game reads the yoke as a position (centre 0x40, range 0x00-0x7f) and turns the offset into a
  sideways speed, rounded to only about 10 steps each way. On a linear stick the first step comes
  at about 10% deflection, so it felt jerky, and MAME's sensitivity setting has no effect on it.
- Added the shared **Steering Response** and **Steering Smoothing** settings. With Squared, half
  stick gives 2 steps instead of 5. Default is arcade behaviour, and the default path is
  byte-identical (700 frames of RAM compared).
- The user tested it (Squared curve, Medium smoothing) and approved it.

## 2026-10-01

### Viper driving games: pedals kept full range through the upstream merge
- MAME's latest code changed how GTI Club 2, Thrill Drive 2, Xtreme Trial and Code One Dispatch
  read their wheel and pedals. Taken as-is, the gas and brake would only reach about 74% with
  the stored calibration, so the fork keeps the pedals spanning their full travel. The I/O CHECK
  screen shows the same values as before.
- Xtreme Trial's gas, brake and handbrake read as fully pressed with nothing pressed. They now
  read MIN at rest and MAX at full.
- Steering keeps the Steering Response and Smoothing settings and works as before.

### Code One Dispatch: boots and steers
- The game no longer stops at boot with "DEVICE ERROR / STEERING WHEEL". At power-on it checks its
  motorised steering wheel by turning it left and right, and in MAME the wheel never moved.
  It now follows the motor during that check, so the game calibrates its controls.
- Steering, gas and brake now work in play. Before, they read zero whenever the check was
  skipped.
- After the check, steering is your input only: the in-game force feedback rumble doesn't move it.
