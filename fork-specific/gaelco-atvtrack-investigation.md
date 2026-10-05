# Gaelco ATV Track / Smashing Drive (`src/mame/gaelco/atvtrack.cpp`)

Two Hitachi SH-4 (SH7750S) CPUs + a NEC PowerVR "Neon 250" (PMX1) + Altera FPGA, Gaelco 2000-2002.
Smashing Drive (`smashdrv`) uses the SH-4 MMU, ATV Track (`atvtrack`) does not. Upstream marks both
`MACHINE_NOT_WORKING` (no video, no inputs, no sound). DEmul runs them. This file is the write-up of
getting the video side going in this fork (2026-10-01). Debug scaffolding is described at the end.

## Status (2026-10-01)

- **smashdrv** (3 sets): boots, runs attract mode and plays. Software PowerVR2-style list renderer
  (`neon250_renderer` in `atvtrack.cpp`, rows dealt to up to 8 threads), inputs (steering on ADC channel
  0, JP8/JP10/JP3 port banks, ADC0838 bit-banged), 32 kHz stereo DAC sound, PRG flash emulation. The SH-4
  DRC is on for both CPUs. The game's master volume float (main RAM `0x0c0733fc`, 0.0-1.0, shown as
  "Vol 00-99") starts at 0 and the original machine presumably kept it in its settings, so
  `smashdrv_state::default_volume` seeds 0.7 ten seconds after reset. The volume buttons work as normal.
- **atvtrack / atvtracka**: renders with the same code, coin and Start (JP8 bit 10) work, races start, sound
  plays at its default volume. Open: the "EMERGENCY STOP" overlay never clears (not JP8/JP10/F802 defaults,
  ADC or SCIF), test mode shows a monitor test pattern that doesn't respond, and the bottom of its test
  screen uses an unhandled texture format. Throttle/brake bits are not mapped.
- **gfootbal**: sub CPU programs the GPU registers (`0xC0` serial, `0xA4`) but never sends a render kick,
  so the screen stays black. Not investigated.
- **Renderer gaps**: pixel formats 3/4/7, fog, modifier volumes, culling modes, render-target-like
  buffers (motion blur wall, loading noise show as noise), pale squares in the RANKING table. Flash
  settings aren't persisted, the bass channels aren't used.

## Driver bugs found

1. `sdrc.ic20` is **not** interleaved with the unpopulated `ic21`: it is plain consecutive bytes at
   `0x02000000` (resource headers line up: `0x0a022565, 0x1d56, 128, 128`). The old
   `ROM_LOAD32_WORD` made every item that crossed the 32 MB boundary read garbage, and the sub CPU
   halted in a `bra self` error loop at `8C001D0E`/`8C001E82`.
2. The sub CPU reads the flash (`0x01820000`, ...) and the PRG ROM (`0x143B0000`) itself: both are
   mapped for it (`smashdrv_sub_map`), with the GPU registers overlaying the first 16 KB of the PRG
   window.

## Boot protocol

- The sub CPU boots from the 1 KB shared RAM. Its boot loader polls a mailbox at `+0x1F0`
  (`+0` command, `+4` address, `+8` length, `+0xC` ready, data at `+0x200`): 1 = copy, 2 = copy +
  flush, 3 = jump. The main CPU feeds it the sub program this way (already worked).
- Afterwards the sub CPU idles in a loop polling the same mailbox for commands from the main CPU
  (`8c001362`). The main CPU's frame loop is clocked by SH-4 TMU1 (INTEVT `0x420`, priority 13).
  The main CPU also installs an IRL level 10 (`0x2A0`) handler that times the frame with TCNT0:
  the video interrupt, which the driver does **not** raise yet.
- GPU registers (sub CPU, `0x14000000`): `0xB0 = 0x8001` starts the internal CPU, `0xB0 = 1` is the
  per-frame **render kick**, IRQ pending/mask at `0x70`/`0x74` (the game waits for bit 7 after the
  kick and acks with `W 0x70 = 0x80`; the driver raises it immediately), `0xC0` is a serial port
  (`0x44xxxxxx`/`0x46xxxxxx` = index byte + 16 bit data) to the video encoder/palette. PCI config
  space at `0x14004000` (BARs `0x18000000` memory, `0x14000000` registers).
- GPU RAM: 32 MB at `0x18000000`. First 16 MB is "parameter" memory (firmware, lists, vertices),
  last 16 MB is texture memory (filled with `0xdeadbeef` by the game at boot).
- The game uploads a command/firmware program for the GPU's internal CPU at `0x18000040...`
  (words like `0300c004 53002000 f1000000`, `f1000000` = NOP). Control block at `0x18814800`:
  `[0]` entry `0x18000e00`, `[1]` ack (the driver writes 1), `[7]` `0x18815000`, `[8]` `0x18800000`,
  `[9]` `0x18811000` (job ring page). We do **not** emulate that CPU: the renderer is high level.

## Frame description (all in GPU RAM, double buffered `+0x800000`)

- **Job ring**: write pointer (bytes, +0x40 per job) at `0x18811020`; the job record is at
  `0x18811f80 + (wp & 0x3fff) - 0x40`, `[0] = 0x40`, `[3]` = context block pointer
  (`0x18040000` / `0x18840000` alternating).
- **Context block**: `+0x18 + 0x10*n` (n = 0..2) = `{flags, ?, size in words, page table}` for the three
  object lists (opaque, ?, translucent). A page table is a list of `address >> 12` words
  (`0x00018047` = page `0x18047000`), 1024 list words per page.
- **Object list entry** (bit-for-bit the PowerVR2 triangle strip entry): bit 31 = "new state
  follows" (5 words: vertex base, vertex limit, ISP, TSP, texture control), bits 30-25 triangle
  mask (bit 30 = triangle 0), bits 23-21 = vertex words - 3, bits 20-0 = first vertex in words from
  the base. Strips of up to 6 triangles; `6060xxxx` = 2 triangles = a quad.
- **Vertices** (24 bytes): `x, y, 1/w, u, v, ARGB` floats (screen space already, u/v normalised).
- **ISP/TSP/texture words** use the Dreamcast PVR2 layout. TSP "use alpha" is clear on the board's
  translucent polygons yet they blend on source alpha: always use alpha.
- **Texture address = `0x18000000 + (field << 5)`**, 32 byte units (the Dreamcast uses 8). This was the
  hard one: with `<< 3` everything overlapped and half the textures read `0xdeadbeef` or other
  textures' data. VQ (256 x 4 x u16 codebook then twiddled index bytes), twiddling, mipmap level
  offsets are exactly the PVR2 ones (MAME `powervr2.cpp`).
- The sub CPU's texture objects (`r5` at the upload function `8c008442`) carry `[4]` = upload
  destination and `[7]` = texture control word; `TEX address field * 32 == upload dst - 0x18000000`.

## Debugging methods that worked (this core, RetroArch only)

- `MAME_EXTRA_OPTS="-oslog -debug -autoboot_delay 0 -autoboot_script X.lua"` (new env var in
  `retro_init.cpp`): logerror to stderr and the debugger engine. **Debugger expressions
  (`pc`, `r12`...) always evaluate against the main CPU**, never the sub CPU, so breakpoints can't
  print sub registers. Use the temporary hooks below.
- `fork-specific/tools/lua_run.py <rom> <tag> <secs> [--lua T:file] [--shot T] [--env K=V]`:
  runs the core with the Lua console, screenshots through RetroArch.
- `fork-specific/tools/gaelco/`: Lua dumpers (`dumpram.lua`, `dump_gpuram.lua` sparse 32 MB dump),
  `sh4dis.py` (capstone SH-4 disassembly with literal pools resolved), `gpulist.py` (list wireframe),
  `texsheet.py` / `texscore.py` / `texvariants.py` (texture decoding experiments).
- Lua memory taps can't be used on this 64-bit bus (u64 > int64 overflows the binding): use C++
  `install_write_tap` in the driver.
- The temporary scaffolding (`ATV_TAP` write/read taps, `ATV_HOOK` per-instruction sub CPU register log via
  `g_sh4_debug_pc_hook` in `sh4.cpp`, `ATV_REGLOG`/`ATV_IOLOG`, `ATV_SUBFIRST`, `ATV_NODRC`, `ATV_PROBE`,
  `ATV_SAVE*`, `ATV_ONLYTEX`/`ATV_SKIPTEX`, `ATV_LODBIAS`, `ATV_THREADS`, `ATV_RENDERPROF`) was removed from
  the tree. `fork-specific/tools/gaelco/debug-scaffolding.patch` re-adds it (`patch -p1 < ...`); the
  hook only works with DRC off (the `ATV_NODRC` line is part of the patch). The `lua_run.py`/`gaelco/`
  scripts still work without it, apart from the tap/hook ones.
- `MAME_EXTRA_OPTS` in `retro_init.cpp` stays: `-wavwrite file` captures the sound for checking.
