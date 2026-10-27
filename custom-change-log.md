# Custom change log

A dated record of this fork's own work on games and systems. Upstream MAME merges and
cherry-picks are left out unless we changed something to make them work. Each entry says what
changed and what you get from it. The technical detail is in `CLAUDE.md` and `my-docs/*.md`.

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

## 2026-09-29

### Build filter: video games only
- The arcade build no longer includes fruit machines, casino games (video slots, poker, bingo,
  roulette, medal games), mahjong and hanafuda, quiz games, pinball, mechanical games (cranes,
  coin pushers, redemption, darts) or console/computer systems. That's 12,549 romsets, down
  from about 32,300. The full list is in `fork-specific/arcade-games.txt`.
- 532 driver source files are no longer compiled, so builds are faster and the core is smaller.
- Video games with a physical control stay in, e.g. Sonic Blast Man, Real Puncher, Arm Champs II.

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

## 2026-10-04

### F-1 Super Battle: steering response and smoothing
- The same **Steering Response** and **Steering Smoothing** settings as the Final Laps and Road
  Blasters, from the shared helper (Machine Configuration menu). Default is arcade behaviour.
- With Squared, half stick turns the wheel a quarter of the way instead of half, and full stick
  still reaches full lock. Heavy smoothing takes about 1 s from full left to full right.

## 2026-10-07

### San Francisco Rush: no freeze in attract mode
- After the mame0289-1364 upstream sync, sfrush froze about 50 seconds after boot (picture stuck on
  the "Fast Times" table, music still playing). The new MIPS CPU code started the CPU's internal
  timer when the game set its clock counter, and the game hangs on that timer's interrupt.
- The timer now only starts when a game sets it explicitly, as before the sync. sfrush runs normally;
  gauntleg, calspeed and sfrushrk checked unaffected.

## 2026-10-11

### Wheel games: steering settings as core options (and Batocera ES)
- The gamepad steering settings (**Steering Response**, **Steering Smoothing**, and Pole Position's
  **Steering Input** / **Stick Steering Speed**) are now also RetroArch core options in the Input
  category, so Batocera can show them in ES (Controls > Driving Games) per system or per game.
- The default, **Game setting**, keeps what's set in MAME's Machine Configuration menu. Any other
  value replaces it when the game starts or the option changes. Games without these settings are
  unaffected.

## 2026-10-13

### Apache 3: ground, rotating sprites and full-width scenery
- The ground is now drawn: it banks with the helicopter, has the moving depth stripes, the city
  road, and the snow stage's river beside the mountains. The sky uses the game's own gradient.
- Sprites rotate: the splash logo spins and sweeps like the arcade, the rotor stays on the
  mast, and trees and buildings lean with the bank on both sides of the screen.
- Bush rows, tree lines and the splash backdrop span the full screen width, so the splash is
  solid blue and take-off shows dense vegetation.
- Mountain ranges and tree lines stay on the ground while banking: no blue gap under them and no
  shrinking on big turns.
- The attract rotor no longer jumps down for part of each turn, and the screen runs at the 55.8 Hz
  the game programs (was 57.4 Hz).

### Round Up 5: road visible in tunnels
- Tunnels showed only the wall lights on a black screen. The road is now drawn there too, with its
  lane markings and edges; the open road is unchanged.

### Taito TC0100SCN games (Aqua Jack, Chase H.Q. and others): background layer back in place
- Since the 2026-10-13 upstream sync, the layer these boards draw with line scroll was shifted by
  most of a screen: Aqua Jack's attract scenes were black behind the plane and the hovercraft, and
  the mountain backdrop showed its wrap seam on screen. Fixed (an upstream typo, not yet fixed there).

## 2026-10-14

### Round Up 5: sprites rotate and zoom, sea strips repeat
- The course-map intro now spins and zooms the "Round Up 5" logos out from the track locations, instead of
  flipping them through flat 90 degree turns.
- In-game, the motorbike riders lean into the bends rather than showing as dark upright blobs.
- Strips flagged to repeat across the screen (the sea beside the road) now run to the screen edge with
  their ripples, instead of stopping as a flat blue block.

### Cycle Warriors, Big Fight: sprites rotate, repeated strips run to the screen edge
- Cycle Warriors (and Big Fight, which shares its machine config) now use the sprite rotation and
  line repeat that Apache 3 and Round Up 5 already had, so rotated sprites and strips flagged to
  repeat across the screen draw as intended. Big Fight looks the same either way.

### Apache 3: tall city buildings no longer lose their bodies
- In the city attract scene, big buildings close to the camera used to vanish apart from a thin strip of
  roof left floating in the sky. They now draw in full, in all three sets.

## 2026-10-15

### Apache 3: service switch toggles, throttle and coin-disable switches
- The service switch now toggles service mode on a single press instead of needing F2 held down.
- Two more SW2 switches are identified: "Use Throttle" (on by default; off makes speed automatic and
  the throttle shows as XX in service mode) and "Disable Coins" (only the service coin works).

### Round Up 5: tunnel exit opens onto the scenery
- Coming out of a tunnel, the far end now opens as a growing arch showing the houses, trees and then the
  sky, as on the real machine, instead of staying solid black until the car is out and then popping in.
  The tunnel mouth is a "shadow" sprite and the game keeps the scenery colours in the shadow half of the
  palette while it is in the tunnel; MAME used to just dim the normal colours under it.
- The gate and tunnel interior at the start of a game are now black behind the gate rather than a dimmed
  view of the sky and scenery (still shows a little grass on the left for a few frames, work in progress).

## 2026-10-16

### GTI Club: no more "DEVICE ERROR (STEERING WHEEL)" at boot
- The machine init checks the force feedback wheel by driving its motor against each stop and reading
  the position back. The emulated wheel never moved, so every boot ended on the steering error and
  needed the test switch to skip it. The wheel now reaches the stop when the motor is held on for half
  a second, so the init completes and the game goes straight to its title screen.
- Only the boot check moves the wheel, so racing is unaffected.
- The Steering Response and Steering Smoothing options now work in GTI Club, for steering with a
  gamepad stick.

### 3D Supersampling and 3D Edge Anti-Aliasing: one generic option each
- The Voodoo options are now generic core options that any 3D renderer with fork support can use,
  instead of one option per hardware. **`mame_3d_supersample`** (Disabled/2x/3x/4x) replaces
  `mame_voodoo_tex_supersample` and **`mame_3d_edge_aa`** (Disabled/Enabled) replaces
  `mame_voodoo_edge_aa`. The old option names are gone with no fallback, so set the new ones.
- GTI Club, Operation Thunder Hurricane, Solar Assault and the ZR107 games now use 3D Supersampling.
  Their 3D layer is rendered at 2x, 3x or 4x the native size and averaged down, which smooths polygon
  edges and the blocky texture shimmer. The HUD and text layers are untouched, and edge pixels blend
  over the background, never over the HUD. Takes effect when content is restarted.
- Cost grows with the square of the factor. On the dev machine GTI Club held full speed in a race at 3x
  and 4x (the attract demo dipped to 0.9x at 4x). On Voodoo games it still averages NxN texture
  samples as before.
- Sega Model 2 now uses 3D Supersampling too. Its 3D layer used to be always rendered at 2x; now it is
  native by default and the option picks 2x, 3x or 4x.
- Save states made with it on, on GTI Club hardware, load only with it on, since the big buffers
  aren't saved.

### Operation Thunder Hurricane: light gun aiming
- Player 1 can now aim with a light gun (tested path: the lightgun X/Y inputs a Sinden drives). The game
  was already firing from the trigger, but its aim point never moved. The gun position is now written
  into the aim point the game normally moves with the stick, so shots land where the gun points.
- Fork-only shortcut: the real I/O board reads two cameras and reports over a network chip that isn't
  emulated. The stick still works too. Player 2 is not done yet.

### Solar Assault (US): boots instead of "EEPROM PARAMETER IS ILLIGAL"
- The US set stopped at "24G BAD : EEPROM PARAMETER IS ILLIGAL" on a fresh boot, because its default
  EEPROM held only the magic number. It now starts from the populated default the Japanese set ships,
  so the machine initialises, the attract mode runs and a game starts.

## 2026-10-17

### Sega System 32 games (Dark Edge, Golden Axe 2, Title Fight, SegaSonic, Slip Stream, Hard Dunk and others): sprites no longer repeat
- Since the 2026-10-13 upstream sync, sprites that face the other way were drawn over and over across the
  line: rows of the same fighter in Dark Edge, Golden Axe: Death Adder and Title Fight, three Tails in
  SegaSonic, two cars in Slip Stream. They draw once again. Fixed (an upstream typo, not yet fixed there).

### GTI Club hardware (GTI Club, Operation Thunder Hurricane, Solar Assault, ZR107 games): 3D Texture Filtering option
- New core option **3D Texture Filtering** (Disabled/Bilinear, default Disabled). Bilinear smooths blocky
  magnified textures in GTI Club (walls, rock faces and road look much less pixelated) and lets very thin
  objects such as overhead wires show up, where plain sampling skips over them. Operation Thunder
  Hurricane and Solar Assault already filter on the real hardware, so for them it changes nothing.
- Works together with 3D Supersampling. Takes effect when content is restarted.

### Operation Tiger (Taito Power-JC): light gun aiming
- Operation Tiger now aims with a light gun (a Sinden or any libretro light gun) for both players. The aim axes
  were analog sticks, which a gun cannot drive; they are now light gun axes covering the whole screen.
- Tested by driving the aim values from a script: coin, start and a full stage play through, shots land where
  aimed. Not yet tried with a physical gun. Game runs at full speed; still no sound (the sound CPU ROM is undumped).

### GTI Club hardware: 3D Texture Filtering no longer draws dark lines on cut-out textures
- With bilinear on, thin cut-out objects (the masts in the title-screen window, palm leaf edges, banner edges)
  showed dark dashed lines. Transparent texels are black, and the filter blended them into the opaque ones.
  Cut-outs now keep their unfiltered outline and the filter only blends real colour, so the lines are gone while
  the textures stay smooth. Frames without cut-outs are unchanged.

### Xtreme Rally (Hyper Neo Geo 64): tunnel mist no longer draws rolling bars over the screen
- The mist that comes over the track after the tunnel was drawn as hard dark and grey bands across the whole
  screen, rolling as the game moved. It is now the light white tint over the top half of the screen that fades out
  towards the bottom and holds still, as on the real machine.
- Cause: the mist's per-line scroll table holds small offsets, not absolute positions. Half the lines read a
  negative row, wrapped to an empty row and drew nothing, and the rest sampled the same first rows. The offsets are
  now added to the first line's position while the layer scrolls one row per line. The Fatal Fury and Buriki One
  floors use absolute values and are unchanged (frame-exact checks identical).

### Hyper Neo Geo 64 (Fatal Fury, Samurai Shodown 64, Buriki One, Roads Edge, Xtreme Rally, Beast Busters 2): 3D Supersampling
- **3D Supersampling** (2x/3x/4x) now works on Hyper Neo Geo 64. The 3D layer (characters, cars, scenery) is
  drawn at the bigger size and averaged down, which smooths polygon edges and shrunk textures. The 2D layers, HUD and
  text are unchanged. Off by default and takes effect when content is restarted.
- Speed has not been measured on an idle machine yet: expect 2x to be the practical setting until it is.

## 2026-10-18

### Round Up 5: no green grass inside the start gate
- Inside the start-gate tunnel the left verge of the road showed bright green grass while the right side was
  black. Both sides are now black from the first frame, as in the tunnel itself.
- Cause: in tunnels the road is drawn with its second pixel set while the first set sits off to the right. The
  fill left of the road still used the first set's colours, which under the shadow sprite are the outdoor grass.
  Each side's fill now comes from the set that is really on that side. The open road is unchanged (frame-exact
  check); the one-frame flash on entering the tunnel is a separate issue and still there.

## 2026-10-19

### Round Up 5: service menu no longer freezes
- Tapping the service key opened the service/test screen, but its live input readout (accelerator, brake, turbo,
  shift, coins) then stopped responding. The service switch is now a latching test switch, like the cabinet's, so one
  press keeps it on and the readout updates; press it again to leave.

### Round Up 5: leaving the service menu restarts the game
- Pressing the service key a second time used to leave the game stuck on the test screen. The board's watchdog is
  now emulated, so the game resets itself a few seconds after the test screen is closed, as the cabinet does.

### Round Up 5: the two roads merge at a road fork
- Where the road forks, the two road halves now blend into one road widening into two, as on the real board,
  instead of the left road being drawn in full with its right red and white kerb and the right road appearing
  beside it. Checked against stills from a real cabinet; other road sections are unchanged.

## 2026-10-20

### Round Up 5: no longer flagged as not working
- The game starts without the "not working" warning screen. It is still marked as having imperfect graphics.

## 2026-10-22

### Sega Model 2: 3D output scale option
- New core option "3D Output Scale". With 3D Supersampling on and this set to "Match supersampling", Model 2 games show
  the 3D layer at the supersampled size (992x768 at 2x) instead of averaging it down, so the picture keeps the extra
  detail. The tilemaps, sprites and text are enlarged with sharp pixels. Off by default.

### PS1 GPU path (Sony ZN, Namco System 10/11/12, Konami GQ/GV, Taito G-NET): loading a save state
- With the GPU option on, loading a state no longer leaves the picture from before the load on screen. The screen goes
  blank and fills in as the game draws its next frames.

### PS1 GPU path (Sony ZN, Namco System 10/11/12, Konami GQ/GV, Taito G-NET): faster at high resolution
- The 4x setting runs much faster: a Brave Blade attract scene went from about 28 fps to about 90 fps on the dev PC
  (uncapped). 2x is also a little faster. The picture is the same as before.

### Sega Model 2: GPU rendering option
- New core option "3D GPU Rendering" (off by default). The 3D layer is drawn on the host GPU at the 3D Supersampling
  resolution and matches the software picture closely. It falls back to the software renderer if no GPU context can be
  created. Checked on Daytona USA, House of the Dead and Virtua Fighter 2 (attract mode).

### Konami GTI Club hardware: GPU rendering option
- "3D GPU Rendering" now also covers the K001005 boards (GTI Club, Thunder Hurricane, Hang Pilot, Winding Heat, Midnight Run).
  The 3D layer is drawn on the host GPU at the 3D Supersampling resolution with the same lighting, fog, texture mirroring
  and filtering as the software renderer. Checked on Thunder Hurricane (attract mode), where it looks the same as software.

### SNK Hyper Neo Geo 64: GPU rendering option
- "3D GPU Rendering" now also covers Hyper Neo Geo 64 (Fatal Fury: Wild Ambition, Xtreme Rally, Beast Busters 2 and the rest).
  The 3D polygons are drawn on the host GPU at the 3D Supersampling resolution; the palette, fade and blend handling is unchanged.
  Checked on Fatal Fury: Wild Ambition, Xtreme Rally and Beast Busters 2 (attract mode). Off by default.

### Midway Seattle and Vegas (3dfx Voodoo 1/2): GPU rendering option
- "3D GPU Rendering" now also covers the Voodoo 1/2 boards (San Francisco Rush, California Speed, Gauntlet Legends,
  Rush 2049 and the rest). Triangles are drawn on the host GPU at the 3D Supersampling resolution, with the texture
  formats, fog, alpha blending and depth tests done in a shader. With 3D Output Scale on, the screen is presented at
  that size instead of being averaged down. The software renderer runs alongside for the first seconds so the boot
  self-tests pass, then stops; any feature the GPU path does not cover (alpha planes, rotating stipple, depth-tested
  framebuffer writes) hands the game back to software without a glitch. Voodoo 3 / Banshee boards (Konami Viper) are
  unchanged. Checked on San Francisco Rush and California Speed (attract mode); off by default.

### Konami Viper (3dfx Voodoo 3 / Banshee): GPU rendering option
- "3D GPU Rendering" now also covers the Viper boards (GTI Club 2, X-Trial Racing, Thrill Drive 2, Jurassic Park 3 and
  the rest). The frame buffers and textures share one video memory on these chips, so the GPU path follows the buffers
  by address, keeps the textures in step with every kind of write the game uses, and applies the video output gamma
  table. Alpha-blended and cut-out textures (billboards, explosions, car reflections, water) use the ARGB4444 and
  ARGB1555 formats and show correctly. Checked on GTI Club 2 over a 90 s attract run (the GPU picture stays within
  about 8/765 of the software one), plus X-Trial Racing, Thrill Drive 2 and Jurassic Park 3 (attract mode). Off by default.

### Konami Viper: GPU path after a save-state load, faster picture copy
- The host-GPU path now works after a save state is loaded (before, it silently did nothing until the game reset the video mode).
- The picture copy to the screen is cached between new GPU frames and uses a tighter loop; GTI Club 2 at 2x runs about
  6-8% faster with the GPU path on (still about 85-91% of software speed).

## 2026-10-23

### Konami Viper: GPU path no longer waits for the GPU draw
- With 3D GPU Rendering on, the emulation no longer stalls while the GPU draws a frame; the finished picture is read
  first and the new frame's drawing runs on the GPU while the game keeps going. The picture is unchanged and shown at
  the same time as before.
- The finished picture is now averaged to the screen size and mapped through the video gamma table on the GPU and read
  straight into the screen image, instead of being converted pixel by pixel on the CPU (identical pixels). GTI Club 2 at
  2x with the screen at native size now runs at about 91-97% of software speed on this machine (was 85-91%).

### Model 2, K001005 boards, Hyper Neo Geo 64: GPU path overlaps with the game
- With 3D GPU Rendering on, the frame is started on the host GPU earlier and picked up later in the same frame, so the
  emulation no longer waits for the GPU. Model 2 starts the 3D layer before the tilemaps are drawn, K001005 boards at the
  buffer swap, and Hyper Neo Geo 64 before the background tilemaps; the picture appears at the same time as before.
  Hyper Neo Geo 64 now reads its 16-bit colour words straight from the GPU. Measured with the screen at native size and
  4x supersampling: Model 2 (Daytona USA) about 3% faster, Operation Thunder Hurricane about 1% faster (23% faster than software),
  Xtreme Rally unchanged (limited by the CPU composite, same speed as software).

### Model 2: GPU path averages the supersampled layer on the GPU
- With 3D GPU Rendering on and the screen at native size, the supersampled 3D layer is averaged down to the screen size on
  the GPU (only the pixels a polygon covers count, as before) and read at screen size, instead of reading the full
  supersampled image and averaging it on the CPU. The picture is pixel-identical (checked on Daytona USA at 4x over
  four frames). Daytona USA at 4x supersampling runs at about 1.50x real time instead of 1.04x on this machine.

### Seattle and Vegas Voodoo boards: GPU picture read straight into the screen
- With 3D GPU Rendering on, San Francisco Rush, California Speed and the other Voodoo 1/2 boards now get the finished picture
  from the GPU already averaged to the screen size and written into the screen image at its position, as the Viper boards
  already did, instead of converting it pixel by pixel on the CPU. The picture is pixel-identical (checked on San Francisco
  Rush and California Speed, four frames each, against the CPU conversion).

### Hyper Neo Geo 64: faster 3D buffer clear with GPU rendering
- With 3D GPU Rendering on, the 3D buffers are no longer cleared on the CPU every frame (about 5.6 ms at 4x supersampling);
  the unused software depth buffer is only cleared if the game falls back to software rendering. Xtreme Rally at 4x
  supersampling runs about 11% faster (0.78x against 0.70x real time, same-core comparison). The picture is unchanged.

### Viper, GTI Club boards and Hyper Neo Geo 64: 3D Output Scale
- 3D Output Scale ("match") now works on every system that has 3D Supersampling, not only Model 2 and the Seattle/Vegas
  Voodoo boards: Viper games (GTI Club 2 and others, on the GPU path), the GTI Club boards (GTI Club, Operation Thunder
  Hurricane, Solar Assault, Midnight Run, Jet Wave) and Hyper Neo Geo 64. The screen is shown at the supersample size, so
  the 3D layer keeps its extra detail while tilemaps, sprites and text are enlarged with sharp pixels.
- Fixed the vertical-blank position of the Voodoo boards at an output scale above 1: it sat halfway down the scaled
  frame, which tore the picture in the middle of the screen.

### Round Up 5: road scenery on the right of the screen
- Where the second road pixel set is parked off the right of the screen, the area right of the first set took the "between the
  sets" pen and showed the wrong colour (the right verge on the beach stage). A set that reaches no on-screen pixel on a line
  now draws nothing and the first set's edge pen fills the rest. Forked roads and tunnels are unchanged.

### Viper, Seattle/Vegas, GTI Club boards, Model 2: 3D Multisample Anti-Aliasing (new option)
- New **3D Multisample Anti-Aliasing** option (off by default, 2x/4x/8x), used with 3D GPU Rendering. It smooths the stair-stepped
  edges of 3D objects on the GPU without raising the render resolution, and it combines with 3D Supersampling and 3D Output Scale.
  On GTI Club 2, 4x MSAA at 1x supersampling adds 0.03 ms of GPU time per frame where 2x supersampling adds 0.47 ms, and the frame rate
  stayed the same within measurement noise. The multisampled 3D buffers do need video memory: the 3D target grows from 6 MB to 27 MB with
  4x MSAA at 1x, about the same as 2x supersampling (24 MB), and from 24 MB to 108 MB at 2x supersampling.
- HUD, text, logos and 2D layers stay sharp (only depth-tested 3D geometry is smoothed), and edges over the 2D background show no dark
  fringe on the GTI Club boards and Model 2. It does nothing with the software renderer, and Hyper Neo Geo 64 is not covered.
  Checked on GTI Club 2, San Francisco Rush, Daytona USA and Operation Thunder Hurricane. Cut-out textures (fences, foliage) are not smoothed.

## 2026-10-24

### Ridge Racer, Time Crisis, Prop Cycle and the other Namco System 22 / Super System 22 games: 3D GPU Rendering, Supersampling, MSAA and Texture Filtering
- The polygons and sprites of every System 22 and Super System 22 game can now be drawn on the host GPU (3D GPU Rendering). The picture
  matches the software renderer closely (Ridge Racer, Time Crisis and Prop Cycle checked frame for frame), and it runs about 1.3 times
  faster than software at normal resolution on this machine.
- 3D Supersampling (2x to 4x) and 3D Multisample Anti-Aliasing now work on these games: polygon edges and shrunk textures smooth out, while
  the HUD, text and logos stay sharp. 3D Texture Filtering (bilinear) smooths polygon textures. All are off by default.
- 3D Output Scale "match" does not apply to these games; the picture is always averaged to the normal screen size.


### Hang Pilot, Thrill Drive, Racing Jam and the other two-Voodoo boards: 3D GPU Rendering no longer mixes the two chips
- On boards with two Voodoo chips (Hang Pilot, Thrill Drive, Racing Jam and the Hornet pairs) both chips drew into one shared
  3D buffer, so Hang Pilot showed the same picture on both screens with patterned blocks across them. Each chip now has its own 3D
  buffer, and the two Hang Pilot screens show their own pictures again.

### Operation Thunder Hurricane, GTI Club, Solar Assault: faster 3D Supersampling on the GPU
- With 3D GPU Rendering on and 3D Output Scale at "native", the GPU now averages the supersampled 3D layer itself and sends back only the
  screen-size picture. Operation Thunder Hurricane at 4x supersampling went from about 1.5x to about 2.0x real time on this machine.
  The picture is the same to within one colour level.

### Operation Thunder Hurricane, Solar Assault: cut-out texture edges
- Cut-out parts of textures on Operation Thunder Hurricane and Solar Assault keep a clean edge with
  every option off, as they have since the 2026-10-17 filtering change: transparent texels beside a visible one borrow its colour
  instead of bleeding dark lines. This is assumed hardware behaviour; it has not been compared with a real board.

### Hang Pilot, Thrill Drive, Gradius 4 and the other Konami Voodoo boards: 2D layers at 3D Output Scale "match"
- With 3D Output Scale set to "Match supersampling" the Konami boards that draw their text and HUD tile layer themselves (Hang Pilot,
  Thrill Drive, Gradius 4, Silent Scope and the other Hornet boards) drew that layer at the normal size on the enlarged screen. Hang Pilot
  showed a red patterned block over the lower part of both screens, Thrill Drive repeated its "INSERT COIN" box and showed misplaced text,
  and Gradius 4's score and credit text was tiny and scattered. The layer is now drawn at the normal size and enlarged with sharp pixels, so
  it sits where it does at native output scale.

### All Voodoo games: see-through textures on the 3D GPU Rendering path
- With 3D GPU Rendering on, textures that are meant to be see-through (trees, fences, car windows and similar cut-outs) were drawn as
  solid blocks on every Voodoo board: Thrill Drive's trees had flat green backgrounds and its car windows were opaque. The GPU path now
  rejects those texels as the software renderer does. Checked against software on Thrill Drive (trees, windows), Jurassic Park III
  (large differences from software mostly gone) and Thrill Drive 2.

### Hyper Neo Geo 64: always native output size
- 3D Output Scale ("Match supersampling") no longer applies to Hyper Neo Geo 64; the screen is always shown at its normal size and
  3D Supersampling is averaged down to it, as on Namco System 22. At 2x with "match" the Xtreme Rally race scene ran at about
  0.59x real time (resolving the whole 3D layer through the palette at the larger size on the CPU); at 2x with native output it
  runs at about 0.87x on this machine, against about 1.0x with supersampling off.

### Time Crisis II, Crisis Zone, 500 GP and the other Namco System 23 / Super System 23 games: 3D GPU Rendering
- With 3D GPU Rendering on, the 3D scene of these games is drawn by the graphics card. 3D Supersampling (2x-4x, shown at the normal
  screen size), 3D Multisample Anti-Aliasing and 3D Texture Filtering now apply to them: polygon edges and textures are smoother while
  the HUD, text and light-gun aim stay as they were. Time Crisis II, Crisis Zone and 500 GP matched the software picture to within
  rounding on every frame checked; Motocross Go!, Final Furlong 2, Panic Park and Race On! were checked the same way.
- It is a little faster than software (about 10-15% on this machine at the normal size) and 2x supersampling costs about what the
  software renderer costs without it. These sets are all marked as not working in MAME, so the picture has the same glitches as before.
  Rapid River and Final Furlong (the System 22.5 boards) still use the software renderer.

### Anisotropic filtering of 3D textures (all GPU-rendered 3D systems except PS1 and Hyper Neo Geo 64)
- New core option "3D Anisotropic Filtering" (off, 2x to 16x). With 3D GPU Rendering on, roads, streets and building fronts seen at a
  steep angle stay steady and sharp in the distance instead of shimmering. Seen most on Gunblade NY; also applies to the Voodoo
  boards (GTI Club 2 and others), GTI Club hardware, System 22 and System 23 games.
- It costs graphics-card time only on the pixels that need it and works with 3D Supersampling and Multisample Anti-Aliasing.
  Off, the picture is identical to before.

### Anisotropic filtering: distant face-on surfaces on GTI Club, Ridge Racer and 500 GP style hardware
- With 3D Anisotropic Filtering on, the distant scenery on Konami GTI Club hardware and Namco System 22 / System 23 games is
  smoothed in both directions, not only along angled surfaces, so far-off buildings, cars and track edges shimmer less.

## 2026-10-26

### Host-GPU 3D rendering: Vulkan engine, now the default
- Every host-GPU 3D system can draw through Vulkan: the PS1 boards, Model 2, K001005, HNG64, the Voodoo boards, System 22 and System 23.
  `mame_3d_gpu_api` picks Vulkan (the default) or OpenGL. OpenGL is used when no Vulkan 1.3 device is found.
- Output matches the OpenGL path frame for frame, except a few edge pixels on Model 2, HNG64, System 22 and System 23.
- Speed is the same as OpenGL on the games measured. MSAA works on Vulkan.
- PS1 frames are sent to the GPU in chunks while emulation runs.
- Namco System 22 needs dualSrcBlend; on a Vulkan device without it, that system's 3D layer is drawn in software.

### Host-GPU engine: Vulkan only
- The OpenGL engine and its loader are gone, along with the `mame_3d_gpu_api` option. Vulkan 1.3 is the only host-GPU
  API; without a Vulkan device the 3D and PS1 GPU paths render in software.

### Software 3D renderers match upstream again (Voodoo, K001005, Model 2, HNG64); PS1 PGXP costs nothing when off
- With 3D GPU Rendering off, Voodoo, K001005, Model 2 and HNG64 draw exactly like upstream at native size. The software-only
  texture supersampling and the Voodoo software edge AA are gone (`mame_3d_edge_aa` was removed).
- The 3D options (supersampling, output scale, MSAA, texture filtering, anisotropic) only act on the host-GPU path, and the
  core option texts say so.
- PS1 with PGXP on but the GPU HLE off, and with PGXP off, does no shadow work on the CPU.

## 2026-10-27

### thrild2: no crash with 3D GPU Rendering on
- Thrild2 no longer crashes about 5800 frames in (around 1.5 minutes) when the GPU path hands a triangle to the software
  rasterizer. The game runs past that point with the Vulkan path on.

### Cartfury: stays on the host-GPU path past its direct-write point (Voodoo 2, Vegas)
- Direct writes to the colour buffer go to the GPU target instead of dropping the whole game to software. Cartfury keeps
  drawing on the host GPU after the frame where it used to switch.

### Voodoo: 3x is the highest 3D supersampling, and colour buffers can be added up to eight
- `mame_3d_supersample` offers 2x and 3x only. A saved 4x setting runs at 3x. 4x is not supported on every system: the
  colour-target stack would not fit the GPU image limit on tall modes.
- The Voodoo GPU target can hold up to eight colour buffers (was four). Games whose 3D draws reach a fifth buffer stay on
  the host-GPU path instead of dropping to software there.
