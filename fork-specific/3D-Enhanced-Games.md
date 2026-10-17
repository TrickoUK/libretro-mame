# 3D Enhanced Games

A living record of the fork's **major 3D work**: the systems where we got 3D games running, made them
fast enough to play, or improved how they render. It is *not* a list of every fix. Small game fixes
(sprite glitches, boot errors, controls, light guns) belong in `custom-change-log.md`, not here.

Last updated: 2026-10-17.

**How this stays current.** `CLAUDE.md` ("3D Enhanced Games doc") makes updating this file part of any 3D
rendering work: add or edit the system's row in the summary table, its section below, the settings
table if an option changed, and the game list when games are verified. Update it in the same commit as
the work.

Bold game names in the lists were checked here (booted and looked at, or A/B compared). The rest are
on the same hardware, so the work applies to them, but they have not been looked at individually.

## Summary

| System / board | What we did | Settings | Status |
|---|---|---|---|
| **PS1-GPU boards**: Sony ZN, Taito G-NET, Namco System 11/12 (and 10), Konami GQ/GV | The PS1 GPU's polygons are drawn on the host GPU at 1x/2x/4x, with PGXP geometry/texture correction, MSAA and texture filtering | **Custom**: eight `mame_psx_gpu_*` options | Working, experimental, off by default |
| **3dfx Voodoo 1/2/3 boards**: Midway Seattle and Vegas, Konami Viper (GTI Club 2, Thrill Drive 2, Jurassic Park 3 and more), Hornet, NWK-TR, Hang Pilot, ITEagle | Voodoo 3 fixes (second texture unit, multi-base textures, 2D blit), PowerPC recompiler speed-ups, texture supersampling and depth-guided edge anti-aliasing | **Generic**: `mame_3d_supersample`, `mame_3d_edge_aa` | Working; options off by default |
| **Konami K001005 boards**: GTI Club, Operation Thunder Hurricane, Solar Assault, ZR107 games | The software 3D renderer draws at 2x/3x/4x and averages down, with optional bilinear texture filtering | **Generic**: `mame_3d_supersample` and `mame_3d_texfilter` (edge AA does not apply) | Working; option off by default |
| **SNK Hyper Neo Geo 64** (Fatal Fury: Wild Ambition, Samurai Shodown 64 and Warriors Rage, Buriki One, Roads Edge, Xtreme Rally, Beast Busters 2) | The software 3D layer renders at 2x/3x/4x, every sample is resolved through the palette and averaged down (smooths polygon edges and shrunk textures); also fixed the Xtreme Rally tunnel mist | **Generic**: `mame_3d_supersample` | Working (2x confirmed in a playtest); off by default. Speed not yet measured on an idle machine |
| **Sega Model 2** | The 3D layer's supersampling moved onto the generic option (it used to be fixed at 2x) | **Generic**: `mame_3d_supersample` | Working; native by default |
| **Konami M2** (Polystars, Total Vice, Evil Night, Heat of Eleven, Battle Tryst) | Triangle Engine rasterizer speed-up and threading, PowerPC recompiler and DSP fixes: playable at about full speed | None, always on | Working |
| **Tatsumi TZB215/315** (Apache 3, Round Up 5, Cycle Warriors, Big Fight) | Hardware-accurate ground, sky and sprite rotation/zoom for the pseudo-3D scenes | None, always on | Working; merged upstream |

## Settings reference

All options are in RetroArch's core options, category **Video**. The supersampling options take effect when content is
restarted.

| Option | Values | Default | Used by |
|---|---|---|---|
| `mame_3d_supersample` | disabled, 2x, 3x, 4x | disabled | Voodoo boards (texture supersampling), K001005 boards, Sega Model 2, SNK Hyper Neo Geo 64 |
| `mame_3d_edge_aa` | disabled, enabled | disabled | Voodoo boards only |
| `mame_3d_texfilter` | disabled, bilinear | disabled | K001005 boards only (Voodoo boards already filter in hardware) |
| `mame_psx_gpu_hle` | disabled, 1x, 2x, 4x | disabled | PS1-GPU boards (master switch; the other PS1 options only matter when this is on) |
| `mame_psx_gpu_pgxp` | disabled, enabled | disabled | PS1-GPU boards: PGXP geometry and texture correction |
| `mame_psx_gpu_pgxp_tol` | legacy, disabled, 0px to 8px | 1px | PS1-GPU boards: how far a vertex without PGXP data may move before it is reverted |
| `mame_psx_gpu_pgxp_vcache` | disabled, enabled | enabled | PS1-GPU boards: recovers precise positions for vertices with no direct PGXP data |
| `mame_psx_gpu_msaa` | disabled, enabled | enabled | PS1-GPU boards: 4x MSAA on the GPU render target |
| `mame_psx_gpu_texfilter` | disabled, bilinear, trilinear, 3point | disabled | PS1-GPU boards |
| `mame_psx_gpu_filter_exclude_sprite` | disabled, opaque, all | all | PS1-GPU boards: keep sprites unfiltered |
| `mame_psx_gpu_filter_exclude_2d_polygon` | disabled, opaque, all | all | PS1-GPU boards: keep 2D polygons unfiltered |

The 3D options are **generic** on purpose: any 3D renderer with fork support reads the same three settings.
The PS1 GPU has its own set because it is a separate host-GPU renderer with PS1-specific behaviour.

## PS1-GPU boards (custom `mame_psx_gpu_*` options)

- **Device**: `psxgpu_device` (`src/devices/video/psx.cpp`), shared by every board below.
  Host side: `src/osd/libretro/libretro-internal/retro_gpu_target.cpp`, a private headless EGL/OpenGL
  context loaded with `dlopen`, independent of RetroArch's own video context. Built only with
  `HAVE_RETRO_GPU_TARGET=1`; without it the option does nothing and the software renderer runs.
- **What it does**: draws the PS1 GPU's triangles, rectangles, sprites, lines, fills and VRAM copies on the host
  GPU at 1x, 2x or 4x. Textures are decoded on the GPU. Screen contents persist between frames like
  real VRAM, and the draw-area clip, display-start double buffering and overscan crop match the
  software renderer.
- **Quality features**: PGXP (perspective-correct textures and less vertex wobble, tracked by memory
  address, with a vertex cache that recovers about 75% of vertices with no direct data), 4x MSAA,
  bilinear/trilinear/3-point filtering, a per-texel semi-transparency bit, texture windows,
  24-bit display mode for FMV, and interleaved texture pages for Konami GQ. Sprites and 2D polygons can
  be excluded from filtering so HUDs and text stay sharp.
- **Reference**: Beetle PSX HW is the gold standard; behaviour is ported from it where it applies.
- **Known gaps vs Beetle**: mask bits (needs framebuffer feedback), dithering, the line-render hack,
  PGXP nclip and the widescreen hack, extra filters (JINC2/SABR/xBR), SSAA.
- **Notes**: Taito G-NET games re-flash their card on the first boot (about 4 minutes); later boots are
  fast. Konami GV games need their CD image (CHD) to boot.
- **Docs**: `CLAUDE.md` (PS1 / ZN sections), `custom-change-log.md` 2026-09-16 to 2026-09-21.
- **Checked here**: Brave Blade, Star Sweep, G-Darius Ver.2, RayStorm, RayCrisis, Cool Boarders Arcade Jam,
  Tekken Tag Tournament, Crypt Killer. PGXP and the vertex cache were checked on Tekken Tag, RayStorm and Brave Blade.
  Nagano Winter Olympics '98 (Konami GV) could not be tested until its CHD was added.

<details>
<summary>Sony ZN (zn.cpp) (39 parent sets)</summary>

Psychic Force (`psyforce`), Super Football Champ (`sfchamp`), Battle Arena Toshinden 2 (`ts2`), Aero Fighters Special (`aerofgts`), Gallop Racer (`glpracr`), Heaven's Gate (`hvnsgate`), Judge Dredd (`jdredd`), NBA Jam Extreme (`nbajamex`), **RayStorm** (`raystorm`), Street Fighter EX (`sfex`), Star Gladiator Episode I: Final Crusade (`starglad`), Beastorizer (`beastrzr`), Fighters' Impact A (`ftimpcta`), **G-Darius Ver.2** (`gdarius2`), Gallop Racer 2 (`glpracr2`), Magical Date EX / Magical Date - Sotsugyou Kokuhaku Daisakusen (`mgcldtex`), Primal Rage II (`primrag2`), Rival Schools: United By Fate (`rvschool`), Street Fighter EX Plus (`sfexp`), Bloody Roar 2 (`bldyror2`), **Cool Boarders Arcade Jam** (`cbaj`), Dead Or Alive ++ (`doapp`), Plasma Sword: Nightmare of Bilstein (`plsmaswd`), Street Fighter EX2 (`sfex2`), Shanghai Matekibuyuu (`shngmtkb`), Tech Romancer (`techromn`), Tetris: The Grand Master (`tgmj`), Bust a Move 2 - Dance Tengoku Mix (`bam2`), Flame Gunner (`flamegun`), Gallop Racer 3 (`glpracr3`), Logic Pro Adventure (`lpadv`), Street Fighter EX2 Plus (`sfex2p`), Strider 2 (`strider2`), Tondemo Crisis (`tondemo`), 1 on 1 Government (`1on1gov`), **Brave Blade** (`brvblade`), The Block Kuzushi (`tblkkuzu`), Tecmo World Cup Millennium (`twcupmil`), Monster Farm Jump (`mfjump`)
</details>

<details>
<summary>Taito G-NET (taitogn.cpp) (29 parent sets)</summary>

Chaos Heat (`chaoshea`), **RayCrisis** (`raycris`), Flip Maze (`flipmaze`), Go By RC (`gobyrc`), Otenami Haiken (`otenamih`), Super Puzzle Bobble (`spuzbobl`), Mawasunda!! (`mawasunda`), Psyvariar -Medium Unit- (`psyvaria`), Psyvariar -Revision- (`psyvarrv`), Shanghai Shoryu Sairin (`shanghss`), Soutenryu (`soutenry`), Night Raid (`nightrai`), Otenki Kororin (`otenki`), Shikigami no Shiro (`shikigam`), Shanghai Sangokuhai Tougi (`shangtou`), XII Stag (`xiistag`), Kollon (`kollon`), Space Invaders Anniversary (`sianniv`), Zoku Otenamihaiken (`zokuoten`), Zooo (`zooo`), Otenami Haiken Final (`otenamhf`), Aero Fighters Special (`aerofgtsg`), Brave Blade (`brvbladeg`), Flame Gunner (`flamegung`), Fighters' Impact (`ftimpactg`), G-Darius (`gdariusg`), RayStorm (`raystormg`), Shanghai Matekibuyuu (`shngmtkbg`), The Block Kuzushi (`tblkkuzug`)
</details>

<details>
<summary>Namco System 11 (namcos11.cpp) (10 parent sets)</summary>

Tekken (`tekken`), Dunk Mania (`dunkmnia`), Xevious 3D/G (`xevi3dg`), Dancing Eyes (`danceyes`), Pocket Racer (`pocketrc`), Prime Goal EX (`primglex`), Soul Edge Ver. II (`souledge`), Tekken 2 Ver.B (`tekken2`), Family Bowl (`fambowl`), **Star Sweep** (`starswep`)
</details>

<details>
<summary>Namco System 12 (namcos12.cpp) (26 parent sets)</summary>

Libero Grande (`lbgrande`), Tekken 3 (`tekken3`), Shin Nihon Pro Wrestling Toukon Retsuden 3 Arcade Edition (`toukon3`), Attack Pla Rail (`aplarail`), Ehrgeiz (`ehrgeiz`), Fighting Layer (`fgtlayer`), Paca Paca Passion (`pacapp`), Soul Calibur (`soulclbr`), Super World Stadium '98 (`sws98`), Techno Drive (`technodr`), Tenkomori Shooting (`tenkomor`), Aqua Rush (`aquarush`), Ghoul Panic (`ghlpanic`), Golgo 13 (`golgo13`), Mr. Driller (`mrdrillr`), Paca Paca Passion 2 (`pacapp2`), Paca Paca Passion Special (`pacappsp`), Point Blank 2 (`ptblank2`), Super World Stadium '99 (`sws99`), **Tekken Tag Tournament** (`tektagt`), Um Jammer Lammy NOW! (`ujlnow`), Golgo 13 Kiseki no Dandou (`g13knd`), Kart Duel (`kartduel`), Super World Stadium 2000 (`sws2000`), Truck Kyosokyoku (`truckk`), Super World Stadium 2001 (`sws2001`)
</details>

<details>
<summary>Konami GQ (konamigq.cpp) (1 parent sets)</summary>

**Crypt Killer** (`cryptklr`)
</details>

<details>
<summary>Konami GV (konamigv.cpp) (11 parent sets)</summary>

Beat the Champ (`btchamp`), Hyper Athlete (`hyperath`), Dead Eye (`kdeadeye`), Let's Attack Crazy Cross (`lacrazyc`), Jikkyou Powerful Pro Yakyuu '96 (`powyak96`), Tokimeki Memorial Oshiete Your Heart (`tmosh`), Tokimeki Memorial Oshiete Your Heart Seal Version (`tmoshs`), Tokimeki Memorial Oshiete Your Heart Seal Version Plus (`tmoshsp`), Wedding Rhapsody (`weddingr`), Nagano Winter Olympics '98 (`nagano98`), The Simpsons Bowling (`simpbowl`)
</details>

Also on the same GPU, not listed: Namco System 10 (mostly 2D puzzle titles), Konami System 573 and Twinkle
(music games). The GPU path applies to them but nothing was looked at.

## 3dfx Voodoo boards (generic options)

- **Device**: `voodoo*.cpp` (`src/devices/video/`), used by Midway Seattle and Vegas, Konami Viper,
  Hornet, NWK-TR, Hang Pilot, ITEagle and the PC-based arcade boards.
- **Generic options**: `mame_3d_supersample` averages NxN texture samples per pixel to remove texture
  shimmer (only the colour is supersampled, so alpha-blended billboards get no fringes; 3x3 recommended).
  `mame_3d_edge_aa` smooths polygon edges using the depth buffer and never blends into HUDs or overlays
  (about 4% of the main thread). Both replaced the older `mame_voodoo_*` options.
- **Voodoo 3 fixes (Viper)**: second texture unit, multi-base texture addresses (cleared the coloured-noise
  textures on GTI Club 2), 2D screen-to-screen blit (the Jurassic Park 3 damage flash).
- **Speed (Viper)**: the PowerPC recompiler no longer recompiles about 10,000 blocks a second; GTI Club 2's
  attract minimum went from 0.72x to 0.99x. Save states now work.
- **Seattle**: San Francisco Rush no longer freezes in attract mode after the MIPS timer change.
- **Docs**: `viper-investigation.md`, `voodoo-texture-supersampling.md`, `voodoo-edge-aa.md`,
  `voodoo-gpu-idea.md` (host-GPU Voodoo rendering, not started).
- **Checked here**: GTI Club 2 and Jurassic Park 3 confirmed with supersampling by the user. Thrill Drive 2,
  Xtreme Trial, Code One Dispatch and San Francisco Rush boot and run. The other boards were not individually checked.

<details>
<summary>Konami Viper (viper.cpp, Voodoo 3) (14 parent sets)</summary>

**Code One Dispatch Ver 1.21** (`code1d`), **GTI Club: Corso Italiano** (`gticlub2`), The Keisatsukan: Shinjuku 24-ji (`p911`), Boxing Mania: Ashita no Joe (`boxingm`), **Jurassic Park III** (`jpark3`), Mocap Boxing (`mocapb`), Mocap Golf (`mocapglf`), Police 911 2 (`p9112`), Silent Scope EX (`sscopex`), **Thrill Drive 2** (`thrild2`), Silent Scope Fortune Hunter (`sscopefh`), Tsurugi (`tsurugi`), World Combat (`wcombat`), **Xtrial Racing** (`xtrial`)
</details>

<details>
<summary>Midway Seattle (seattle.cpp, Voodoo 1) (12 parent sets)</summary>

Mace: The Dark Age (`mace`), **San Francisco Rush** (`sfrush`), Wayne Gretzky's 3D Hockey (`wg3dh`), Bio F.R.E.A.K.S (`biofreak`), NFL Blitz (`blitz`), San Francisco Rush: The Rock (`sfrushrk`), NFL Blitz '99 (`blitz99`), California Speed (`calspeed`), CarnEvil (`carnevil`), Hyperdrive (`hyprdriv`), Vapor TRX (`vaportrx`), NFL Blitz 2000 Gold Edition (`blitz2k`)
</details>

<details>
<summary>Midway Vegas (vegas.cpp, Voodoo 2) (12 parent sets)</summary>

Gauntlet Legends (`gauntleg`), Tenth Degree (`tenthdeg`), Gauntlet Dark Legacy (`gauntdl`), SportStation: NBA Showtime NBA on NBC (`nbanfl`), NBA Showtime NBA on NBC (`nbashowt`), Road Burners (`roadburn`), San Francisco Rush 2049 (`sf2049`), War: Final Assault (`warfa`), CART Fury Championship Racing (`cartfury`), SportStation: NBA Showtime NBA on NBC Gold Edition (`nbagold`), San Francisco Rush 2049: Tournament Edition (`sf2049te`), San Francisco Rush 2049: Special Edition (`sf2049se`)
</details>

<details>
<summary>Konami Hornet (hornet.cpp) (5 parent sets)</summary>

Gradius IV: Fukkatsu (`gradius4`), NBA Play By Play (`nbapbp`), Teraburst (`terabrst`), Silent Scope (`sscope`), Silent Scope 2 : Dark Silhouette (`sscope2`)
</details>

<details>
<summary>Konami NWK-TR (nwk-tr.cpp) (3 parent sets)</summary>

Racing Jam (`racingj`), Racing Jam: Chapter II (`racingj2`), Thrill Drive (`thrilld`)
</details>

<details>
<summary>ITEagle (iteagle.cpp, Voodoo 3) (12 parent sets)</summary>

Virtual Pool (`virtpool`), Big Buck Hunter (`bbh`), Golden Tee Fore! (`gtfore01`), Golden Tee Fore! 2002 (`gtfore02`), Big Buck Hunter - Shooter's Challenge (`bbhsc`), Carnival King (`carnking`), Golden Tee Fore! 2003 (`gtfore03`), Golden Tee Fore! 2004 Extra (`gtfore04`), Big Buck Hunter II - Sportsman's Paradise (`bbh2sp`), Golden Tee Fore! 2005 Extra (`gtfore05`), Golden Tee Fore! 2006 Complete (`gtfore06`), Big Buck Hunter Call of the Wild (`bbhcotw`)
</details>

Also Voodoo-based: Hang Pilot (`hangplt`, in `gticlub.cpp`) and the PC-based arcade boards (Pump It Up and others).
Not listed: ParaParaParadise 2nd Mix (a Viper music game).

## Konami K001005 boards (generic `mame_3d_supersample`, `mame_3d_texfilter`)

- **Device**: `k001005.cpp`, the software 3D renderer in GTI Club, Operation Thunder Hurricane, Solar Assault and
  the ZR107 games. With `mame_3d_supersample` on, the 3D layer is rendered at 2x/3x/4x the native size and
  averaged down. The HUD and text layers are untouched, and edge pixels blend over the background, never over the HUD.
- **Texture filtering**: `mame_3d_texfilter = bilinear` forces the renderer's own bilinear path (Operation Thunder Hurricane
  and Solar Assault already enable it on the hardware). On GTI Club it smooths walls, rock and road, and thin
  objects like overhead wires appear that unfiltered sampling skips. Cut-out (transparent) textures keep their
  unfiltered outline and never blend in the black of transparent texels, so they get no dark edge lines. No speed cost measured yet.
- **Cost** of supersampling: grows with the square of the factor. GTI Club held full speed in a race at 3x and 4x on the dev
  machine; the attract demo dipped to 0.9x at 4x.
- **Save states** made with it on load only with it on, since the big buffers aren't saved.
- **Also made to work here**: GTI Club's boot steering check, Solar Assault's default EEPROM, Operation Thunder
  Hurricane player 1 light gun aim (see `custom-change-log.md`).
- **Checked here**: GTI Club (speed at 3x/4x; texture filtering at 1x and 3x, frame-exact snapshots).

<details>
<summary>Konami GTI Club hardware (gticlub.cpp, excluding Hang Pilot) (3 parent sets)</summary>

**GTI Club: Rally Cote D'Azur** (`gticlub`), Solar Assault (`slrasslt`), Operation Thunder Hurricane (`thunderh`)
</details>

<details>
<summary>Konami ZR107 (zr107.cpp) (3 parent sets)</summary>

Midnight Run: Road Fighter 2 (`midnrun`), Jet Wave (`jetwave`), Winding Heat (`windheat`)
</details>

## Sega Model 2 (generic `mame_3d_supersample`)

- **Change**: Model 2's 3D polygon layer used to be rendered at a fixed 2x. It is now native by default and
  `mame_3d_supersample` picks 2x, 3x or 4x. The layer is box-filtered down to the native size.
- **Checked here**: the switch was recorded on 2026-10-16; per-game A/B checks are not recorded in the change log.

<details>
<summary>Sega Model 2 (model2.cpp) (36 parent sets)</summary>

Daytona USA (`daytona`), Desert Tank (`desert`), Rail Chase 2 (`rchase2`), Virtua Cop (`vcop`), Virtua Fighter 2 (`vf2`), Virtua Striker (`vstriker`), Fighting Vipers (`fvipers`), Gunblade NY (`gunblade`), INDY 500 Twin (`indy500`), Manx TT Superbike - DX/Twin (`manxtt`), Sky Target (`skytargt`), Sega Rally Championship - Twin/DX (`srallyc`), Virtua Cop 2 (`vcop2`), Cyber Troopers Virtual-On - Twin (`von`), Dead or Alive (`doa`), Dynamite Baseball (`dynabb`), Last Bronx (`lastbrnx`), Power Sled (`powsled`), Sonic Championship (`schamp`), Super GT 24h (`sgt24h`), Sega Ski Super G (`skisuprg`), Sega Touring Car Championship (`stcc`), Wave Runner (`waverunr`), Air Walkers (`airwlkrs`), Behind Enemy Lines (`bel`), Dynamite Baseball 97 (`dynabb97`), The House of the Dead (`hotd`), Motor Raid - Twin (`motoraid`), Over Rev (`overrev`), Royal Ascot II (`rascot2`), Sega Water Ski (`segawski`), Top Skater (`topskatr`), Zero Gunner (`zerogun`), Dynamite Cop (`dynamcop`), Hanguk Pro Yagu 98 (`hpyagu98`), Pilot Kids (`pltkids`)
</details>

## SNK Hyper Neo Geo 64 (generic `mame_3d_supersample`)

- **Device**: `hng64_3d.ipp` / `hng64_v.cpp`, a software rasterizer that draws palette indices (11-bit palette index, 4-bit
  light, blend flag) into a 512x512 buffer. The compositor resolves them through a palette that holds 16 brightness
  copies and stretches the buffer to the 448-line screen.
- **Supersampling**: with `mame_3d_supersample` on, the colour and depth buffers are N times bigger in each direction and
  the polygon coordinates are scaled to match. When the layer is composited, every sample under an output pixel is
  looked up through the palette (so the blend flag and transparent texels are handled per sample) and the results are
  averaged. The 512 to 448 line squash becomes a box filter over the true span instead of picking one row, which also
  removes row-skip shimmer. Tilemaps, sprites and the HUD are drawn separately and stay sharp. With the option off the
  original path is untouched (frame-exact identical on 9 frames each of Fatal Fury and Xtreme Rally).
- **Texture filtering**: not implemented, and not planned. The buffer holds palette indices, which can't be blended, so it
  would need a parallel RGB buffer filled at raster time. The textures already look good enough that it isn't worth it.
- **Also fixed here**: the Xtreme Rally tunnel mist (see `custom-change-log.md`), which was drawing rolling bars.
- **Speed**: unmeasured. A first run on a busy machine (a Batocera build was running) gave unusable numbers; 3x and 4x
  looked much slower than 2x. Re-measure on an idle machine before recommending a setting.
- **Checked here**: **Fatal Fury: Wild Ambition**, **Xtreme Rally** (2x against native, frame-exact snapshots). 2x also
  confirmed good in a playtest of Xtreme Rally by the user (2026-10-17); 3x and 4x not played.

<details>
<summary>SNK Hyper Neo Geo 64 (hng64.cpp) (7 parent sets)</summary>

Roads Edge / Round Trip RV (`roadedge`), Samurai Shodown 64 (`sams64`), **Xtreme Rally** (`xrally`), Beast Busters: Second Nightmare (`bbust2`), Samurai Shodown 64: Warriors Rage (`sams64_2`), **Fatal Fury: Wild Ambition** (`fatfurwa`), Buriki One (`buriki`)
</details>

## Konami M2 (no setting, always on)

- **Device**: the 3DO M2 hardware (`konamim2.cpp`, `3dom2*.cpp`), a PowerPC 602 with the Triangle Engine rasterizer.
- **What we did**: the Triangle Engine skips blend maths it doesn't use (30-35% faster for non-blended draws),
  draws scanlines on 12 worker threads, queues register writes with a snapshot per job, and copies texture uploads
  64 bits at a time. The PowerPC recompiler stopped recompiling blocks when only TLB permission bits change, and
  flushes compiled code when the game flushes its instruction cache (that fixed the Polystars stage 1 black screen).
  The DSPP audio DSP skips idle loops (bit-identical output).
- **Result**: Polystars benchmark about 0.54x to about 1.0x. Polystars and Evil Night confirmed playable.
- **Docs**: `m2-fix-investigation.md`, `polystar-performance.md`.
- **Checked here**: **Polystars**, **Evil Night**.

<details>
<summary>Konami M2 (konamim2.cpp) (5 parent sets)</summary>

Tobe! Polystars (`polystar`), Total Vice (`totlvice`), Battle Tryst (`btltryst`), Evil Night (`evilngt`), Heat of Eleven '98 (`heatof11`)
</details>

## Tatsumi TZB215/315 (no setting, always on; merged upstream)

- **What we did**: hardware-accurate ground and sky, scanline sprite rotation and zoom, and the Round Up 5 tunnel and
  sea strips. These are pseudo-3D scenes (scaled sprites and roads), so this is the one non-polygon system in the list.
- **Upstream**: merged into mamedev as `04d45091693` (PR #16437), so it is no longer fork-only.
- **Checked here**: Apache 3 and Round Up 5 in detail (see `roundup5-tunnel-investigation.md`).

<details>
<summary>Tatsumi (3 drivers, 4 parent sets)</summary>

**Apache 3** (`apache3`), **Round Up 5 - Super Delta Force** (`roundup5`), Cycle Warriors (`cyclwarr`), Big Fight - Big Trouble In The Atlantic Ocean (`bigfight`)
</details>

## Not started

Candidates from the planning notes in `CLAUDE.md`, none touched yet: Sega Model 3 (`daytona2`), Namco System 22
and 23, Nintendo 64 / Aleck64, Midway Zeus, and Midway Quicksilver. A host-GPU Voodoo renderer is only an idea
(`voodoo-gpu-idea.md`).

## Adding to this document

When 3D rendering work lands on a system:
1. Add or update its row in **Summary**, saying what was done and whether it uses a generic or custom setting.
2. Add or update its section, with the device or files, the effect, speed numbers if measured, and what was checked.
3. Update **Settings reference** if an option was added, renamed or changed default.
4. Update the game list when more games are checked, and bold the ones that were.
5. Leave small fixes (sprites, boot errors, controls) to `custom-change-log.md`.
