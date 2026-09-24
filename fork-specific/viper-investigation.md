# Konami Viper (gticlub2) investigation

Started 2026-09-24. Target: `gticlub2` (GTI Club: Corso Italiano, `src/mame/konami/viper.cpp`,
MPC8240 (603e core) + Voodoo 3). Symptoms at the start: slowdown in parts of attract mode, and
textures drawn as coloured noise. Examples: car windows, the "best laps" car, trees and chairs in
the town plaza, stair walls and the tunnel sign. The sea on the mountain course was dark green.

Tool: `fork-specific/tools/profile_run.py <romset> <tag> --warmup 60 --duration 60 --shot-every 4`
(boots cold, `perf record`s the process, polls `speed_percent` via the Lua console, and takes
core-framebuffer screenshots over UDP). gticlub2 needs ~50s from boot to reach 3D attract mode.

## Result

| | baseline | fixed |
|---|---|---|
| main-thread CPU | 91.5% of a core | 66.5% |
| main thread in the PPC recompiler | 22.5% | 0.3% |
| PPC block recompiles/s | ~8,000-12,000 | ~40 (genuinely new code) |
| `speed_percent` minimum over 60s | 0.72 | 0.99 |
| texture corruption | widespread | none seen in attract mode |

## Fix 1: PPC DRC recompile storm (performance)

`ppcdrc.cpp` `generate_sequence_instruction()`: code compiled while its page's vtlb entry was
empty got an *unconditional* `tlb_mismatch` exit. The first run refilled the entry and re-entered.
The second run saw a valid, fetchable stale entry, so the handler took the "mapping replaced"
path and recompiled. This fork's `af439229f3d` made that path recompile rather than flush. The
603e's software TLB evicts vtlb entries constantly, so this looped at ~10k recompiles/s,
almost all "override" recompiles of blocks whose code and TLB entry were unchanged. A variant
of the same bug: the entry was present but filled only by a data read (`...009`, no
FETCH), so the baked-in compare never matched a fetch-filled entry.

Fix: if the entry can't fetch at compile time, fill it for fetch first
(`ppc_device::compile_time_tlb_fill()`, the same translate + `vtlb_fill` the mismatch handler
does). The 603 translate path writes the IMISS/ICMP/HASH miss registers as a side effect, so
they're saved and restored in case the compile happens inside a guest TLB-miss handler. The
unconditional exit is still generated if the translation really fails, so the guest still
takes its ITLB miss.

Diagnosis technique: counters in `code_compile_block` (compile vs. "override" recompiles,
flushes, icbi/ICFI invalidations, top recompiled PCs), then logging the stale vtlb entry
seen by `ppc_cfunc_ppccom_mismatch` and the compile-time expected value. The translation-
generation checks and ICFI (`914dc7e06b8`) were ruled out; neither fired.

## Fix 2: Voodoo 3 had only one TMU (textures)

`voodoo_banshee.cpp` `device_start()` forced `m_chipmask = 0x03` (FBI + TMU0) for both
Banshee and Voodoo 3, but Voodoo 3 has two TMUs. gticlub2 broadcasts texture state to both
TMUs (chip field 0), then writes TMU0-only `textureMode`/`texBaseAddr3_8` (chip 2) and a
TMU1-only `tLOD` (chip 4). The chip-4 writes were silently dropped. TMU1 is the TMU that
samples the real texture; TMU0 is configured to pass TMU1's colour through. Now `0x07` for
Voodoo 3 only. Both TMUs already shared framebuffer RAM in the Banshee+ path.

## Fix 3: multibase texture addresses are base + owned-LOD offsets (textures)

`voodoo_render.cpp` `rasterizer_texture::recompute()` treated `texBaseAddr_1/2/3_8` as
absolute LOD addresses. That is the old upstream TODO, "viper expects relative offsets ...
0xff0000, 0xffc000, 0xfff000". The logged values are exactly `0 - (sizes of the LODs this
TMU owns below that LOD)`. For example, 0xff0000 is minus a 256x256 8bpp LOD0, 0xfef000 is
minus LOD0 plus LOD2, and the 16bpp values are 0xfe0000/0xfde000. The aspect-ratio
variants match too. So on Banshee+ the hardware still adds the cumulative
owned-LOD offsets in multibase mode, and Glide programs each base minus them (pointing
unused LODs at address 0). Applied only when `addrshift == 0` (Banshee/Voodoo 3); the
Voodoo 2 multibase path is unchanged.

Fixes 2 and 3 were needed together. With only fix 2, corruption remained on the env-mapped
surfaces (car windows, chrome car), and fix 3 cleared it.

## Fix 4: 2D screen-to-screen blit (jpark3 damage overlay)

Found via `jpark3`: when a dinosaur hits the player, a full-screen overlay was drawn as
rainbow noise. jpark3 grabs the rendered frame into textures with the 2D engine's
screen-to-screen blit: the command is `0xCC000801` (SRCCOPY). The source is the tiled colour
buffer (`srcBaseAddr` bit 31, stride 8 tiles). The destination is linear 16bpp textures at
0x4100/0x24100 (256 rows each) and 0x44100/0x54100 (128 rows each). It does one launch per
row: the launch data is the source X/Y, `dstSize` is 256x1, and `dstXY` is never rewritten.
That's ~200k launches in a couple of minutes. MAME's screen-to-screen blit was a `// TODO`
no-op, so the textures kept stale garbage.

Implemented in `voodoo_banshee.cpp` `execute_blit()` case 1: copy the `dstSize` rectangle
from the launch source X/Y to the current destination, then advance the destination Y by the
height. A tiled surface's stride is in 128-byte tiles; the 3D engine renders it linearly with
that row pitch, so it's addressed the same way. Only ROP 0xCC between surfaces of the same
depth is handled; anything else is logged. After the fix, spit-damage frames render cleanly.
Confirmed in live play (2026-09-24): the intended red damage tint shows for a few frames. The
periodic screenshots missed it because it only lasts a few frames.
carnking (Voodoo 3) is unchanged. gticlub2 never issues screen-to-screen blits.

Also: unlike gticlub2 (CMDFIFO packet 5), jpark3 uploads its textures with 2D host-to-screen
blits (cmd 3, srcFormat 0x0041xxxx 8bpp and 0x0073xxxx 16bpp with host byte/word swizzle).
These already render correctly, but MAME's host blit ignores the srcFormat swizzle bits and
hard-codes the byte order. Worth remembering if another Viper game shows swapped texels.

Repro with no gun needed: `profile_run.py jpark3 <tag> --warmup 60 --duration 120
--shot-every 2 --lua "70:manager.machine.ioport.ports[':IN3'].fields['Coin 1']:set_value(1)"
...` Send the coin twice and `1 Player Start` once, 0.5s set/clear apart. If nobody shoots,
the Dilophosaurus in Area 1 Stage 1 spits within ~90s of the game starting.

## Fix 5: analog controls (ADC over I2C)

Symptom: steering and pedals did nothing. The game's I/O CHECK showed every analog channel
pinned (ADC FFFF, steering F87C) whatever MAME sent, and the car sat at 0 km/h with gas held.

The analog chip is reached over the MPC8240 I2C controller, and the I2C interrupt handler
(vector table entry 0x3a0 -> 0x3d2b4) runs a small state machine per channel. It writes the
address byte `0x21 + 2*channel + 8*half`, does a dummy read, then reads one data byte. `half` is a
per-channel bit kept at RAM 0x84b. If the byte is zero, it flips that channel's bit and reads the
other half. The sample is 9-bit sign/magnitude around 0x100: latch 0x10-0x13 returns the distance
above 0x100, and 0x14-0x17 the distance below it. The game rebuilds 0x100 +/- byte, and the I/O
CHECK's ADC field shows that value * 128.25. Found by logging PC/LR at the I2C data register
(the LR pointed into the EPIC dispatcher at 0xf0a0), dumping main RAM via Lua
`read_range` and disassembling with capstone.

MAME's stub returned `port >> 8` for 0x10-0x13. gticlub2's ports are 8-bit, so that was the
base `viper` port's unused active-low high byte (always 0xff). Fix: channels whose port only
uses bits 0-7 (detected from the fields at machine_start) now scale the 8-bit value to 9 bits
and answer in the split-half format. Channels with wider ports (thrild2's 12-bit steering) keep
the old path. The PORT_REVERSE flags on the pedals, handbrake and K-type wheel were backwards
(verified in I/O CHECK: raw 0xff is full RIGHT / pedal MAX), so they are removed.

Result: I/O CHECK shows centre/MIN at rest and full LEFT/RIGHT and MIN/MAX at the extremes, and
the car drives (89 km/h with gas held). Pedals saturate before full raw travel with the NVRAM
calibration used here; the test menu's CALIBRATION page adjusts that. Smoke-booted gticlub2,
thrild2 and jpark3, with no errors. thrild2's pedals now go through the same path; its gameplay
is untested.

Lua gotchas: an I2C tap on this 64-bit bus aborts the core ("integer value will be
misrepresented in lua", the byte-lane mask has bit 63 set). `ioport_field:set_value()` on an
analog field writes the unshifted override into the whole port, so it only behaves for fields
at bit 0.

## Checked and ruled out

- Texture download apertures (`map_texture_w`, which is a logerror stub on Banshee) and the 2D
  blitter: unused by gticlub2. Textures arrive via CMDFIFO packet type 5 (linear FB, 1KB
  chunks). The direct `write_lfb` path is only a boot-time VRAM clear.
- Palette/NCC writes don't set `tmu_state::m_regdirty`, so a palette-only change keeps
  sampling the old palette copy until a texture register changes. This looks like a real
  latent bug by reading, but fixing it made no visible difference here, so it isn't applied.
- Some packet-5 upload runs skip 0x100 bytes every 0x900 (a strided surface); a few textures
  overlap those holes by 1 block. No visible effect seen.

## Regression check (post-fix, 50s cold boots)

carnking (iteagle, Voodoo 3): attract renders correctly. sf2049 (Vegas, Voodoo 3), daytona2
and scud (Model 3 PPC), polystar (M2, 602), kviper: boot clean, no errors. virtpool, bbh,
cartfury and pumpitup are missing files in the local ROM collection, so they're untested.

## Remaining ideas (not done)

- Main thread is now ~52% JIT guest code, ~10% memory handlers (`handler_entry_read_memory`),
  ~10% Voodoo setup. The Voodoo rasterizer already runs on 3 worker threads (~26% each).
- `voodoo_1_device::update_common` + software render compositing: ~3%.
