# Upstream MAME changes worth pulling into this fork (investigation only)

Written 2026-09-24 against upstream `mame0289-1112-g7a134a7b767` and `arcade-focused` at `7c1fa0f62bb`. Commit hashes are upstream ones (`/var/home/bazzite/Projects/upstream/mame`).

## Context
The user asked, read-only: which of upstream's changes since our base (mame0289; upstream checkout
is `mame0289-1112-g7a134a7b767`, 2026-09-24, no mame0290 tag yet) would improve this fork's
drivers. Method: I filtered upstream's 1,110 non-merge commits to arcade and 3D areas, read the
relevant messages and diffs, checked the affected romsets against the local ROM collection, and
ran `git apply --check` on each candidate against `arcade-focused`. Nothing was changed.

## Tier 1: directly affects hardware we're actively working on

### PPC DRC (Viper gticlub2/jpark3, M2, Model 3, Hornet)
**Status 2026-09-25: pulled on branch `ppc-upstream-sync`**. Taken: `6e65bf15368`,
`764de9b5b48`, `8f158369e6a`, `1a8267fa579`. Conflict notes are in each commit message.
- `8f158369e6a` replaces our `af439229f3d` compare. A reused block now also re-snapshots its code
  pages so our ICFI handling still works.
- **`0ee5c47faa7` was dropped.** With it, daytona2 and spikeout (Model 3) hang at boot. User-mode
  code reads ~0x1500 through a page with SR Kp=1 and PP=00. The new check turns that into a DSI,
  and the game's handler treats the DSI as fatal. It's architecturally "correct", so it's probably
  an existing Model 3 inaccuracy that the old, permissive check hid. The other eight local Model 3
  sets tested showed no protection faults. Model 3 is low priority, so this could be retaken if a
  Viper/M2 game needs it.
- Speed: no measurable change on gticlub2 or polystar (the recompile storms were already fixed by
  our own commits). The value is TLB accuracy. Save states are not compatible across this change,
  because the vTLB tables are saved.

- **`8f158369e6a`**: the vTLB grows from **128 to 4096 entries**, adds a sequence cache, and adds
  the masked TLB compare. Our gticlub2 recompile storm was caused by the 603e's TLB constantly
  evicting vTLB entries, so a 32× larger vTLB may reduce it on its own. Upstream's message also
  says it "sped up a few of the PPC arcade games". Conflicts with our `af439229f3d` (same masked
  compare, done independently), so ours should be dropped in favour of it.
- **603 TLB correctness**: `6e65bf15368` (SRR1 store-bit polarity, applies cleanly),
  `1a8267fa579` (invalidate by VSID/index), `0ee5c47faa7` (KEY bit, vTLB permissions,
  user-mode fetch permissions), `764de9b5b48` (divtlb, applies cleanly). These are real accuracy
  fixes for the 602/603e in M2 and Viper. `0ee5c47faa7` conflicts with our
  `6a0cf6b4aaf`/mismatch handler; ours has to be reworked to use the per-mode
  `TR_FETCH`/`TR_UFETCH` permissions.
- `89bd53eb70a` (ICFI full invalidate) duplicates our `914dc7e06b8`. Keep ours: it only discards
  pages whose code actually changed.

### DRC floating-point rounding (every x64 DRC)
- **`0e3c9c6cd14`**, a one-line `drcbex64.cpp` fix for the rounding mode not being set in some
  cases. The `drcbex64` hunk on its own **applies cleanly**. **Backported 2026-09-24 as `da0e7a6c62a`.** It affects PPC (Model 3/Viper/M2),
  MIPS (Seattle/Vegas), SH and others.
- **`fac12419a82`** sets a rounding mode for the SHARC and TGPx4 (MB86235) DRCs ("resolves weird
  behavior on x64"). **Applies cleanly.** SHARC matters for Hornet/NWK (gradius4, thrilld,
  hangplt) and Model 2B. TGPx4 matters for Model 2C. Model 2 is our next candidate.

### PS1 / ZN sound: `aab5dcadb60` + `2d84d8a39b0`
- An SPU overhaul: register/DMA endian fixes, master volume, correctly timed SPU IRQ, FM
  chaining, repeat-address fix, CD/XA decoded-data writes. It improves audio on **every
  `psxgpu_device` board** we target (ZN1/ZN2, System 10/11/12, GV/GQ/573, Taito G-NET).
- It also moves PS1 GPU VRAM to a `ram_device`: `p_vram` becomes `m_ram->pointer()`, and the GPU
  constructors change to `set_cpu()`. That collides with our GPU HLE code in `video/psx.cpp`.
  11 hunks fail as-is. It would have to be ported by hand, but the SPU part (`spu.cpp`) is
  independent of our changes.
- Our `master` also carries an old libretro "MAMEFX" left-edge hack in `video/psx.cpp` that
  upstream never took. Worth rechecking whether it still matters when merging.

## Tier 2: other candidate 3D drivers with ROMs in the local collection (low conflict)
- **Status 2026-09-25**: the System 22 set below was pulled into `arcade-focused` (`01989a166b6`,
  `cd0a41e0a21`, `cbf7becbc9a`, `f47e20b46fc`, `9bfb7941715`, `a0813ada663`, `42040098b84`).
  All applied cleanly. `cd0a41e0a21` also changes the shared `poly.h` `render_polygon` clipping.
  10 S22/SS22 sets plus daytona, crusnusa and mk4 cold-boot clean. Konami GX
  (`af5376797f1`, `f10cb860392`, `45d303a983e`) was pulled too: Racin' Force now draws its road and
  takes pedal input, and it plays well (its music is louder than its sound effects; not
  investigated).
- **Namco System 22** (ridgerac, timecris, acedrive, cybrcomm, propcycl, alpinerd, tokyowar,
  airco22b): `cbf7becbc9a` lighting with shared normals, `a0813ada663` Super System 22 per-poly
  fog, `9bfb7941715` quantum fix (acedrive/victlap video test), `f47e20b46fc` double-precision
  extents, `cd0a41e0a21`/`01989a166b6` cleanups. Most apply cleanly, and the rest fail only
  because they depend on each other.
- **Midway Zeus / Zeus 2** (mk4, invasnab, crusnexo, thegrid): `d1e9f7db995` (mk4/invasn
  frozen screen edges, green colour-key bleed on the timer; applies cleanly), `2778d32e480`,
  `00c056a8edc`, `c34397f5b63` (depth, blending, solid fill, darkened cars), `64ee7b37da0`
  (Cruis'n Exotica game speed and depth clear), `f42098cc1dc` (vsync interrupt). They should be
  taken as an ordered series.
- **Sega Model 3**: `2b5c770ba76` fixes an out-of-bounds texture read crash. **Applies cleanly.**
- **Taito JC** (dendego, dendego2): `f2b8d3b5f22` adds 68040 FPU exceptions, which fixes the
  train not moving in Densha de GO! attract mode. Applies cleanly.
- **Sega System 32** (jpark, ga2, titlef): `736defc1b85` per-line clipping. Applies cleanly.
- **Cave CV1000** (espgal2, ddpdfk, mushisam, futari15): `69fb86bd250` (applies cleanly), then
  `5948b7f6d63`. SH-3 cycle accounting and blitter bus contention give more accurate slowdown.
- **YMF271** (Seibu SPI, Jaleco and others): `03761e46766` core rewrite (applies cleanly), then
  `783e8a2efc2`.
- **V25** (Irem sound CPUs): `964966cf3d0` cycle counts and REP resume after interrupt. Applies
  cleanly.
- **Namco System 10**: `1ff7d66e66a` decrypts three more games (kd2001 is local). Applies cleanly.
- Many ST-V / Saturn SCU/DCC fixes (`df6bf669e35`, `6e08706e9d1`, `bd8d65f4640`, ...). This
  is a large series on a platform outside our focus.

## Not useful to us
- The DSPP changes (`0d098f67d20` AUDLOCK/39 kHz fix, semaphore, RMAP) target the **original
  3DO's CLIO DSPP**. The Bulldog (M2) runs its own free-running model, so this is **not** a fix
  for the slow evilngt cutscene speech.
- `0218c4ba9ff` (VR5500, jnero): the game is still MACHINE_NOT_WORKING.
- Most of the rest is computers, consoles or buses (cbmiec, a2bus, Amiga, 3DO MADAM, RiscPC, ...).

## Cherry-pick vs. merging mame0290
- There's also core API churn: the `screen_device` split (`774a180df2b`), `frame_period` as
  `attotime` (`4873b416e7e`), `rotl_32` replaced by `std::rotl`, ATA `mem_mask` removal,
  `sound.h`/`video.h` no longer in `emu.h`. Later driver commits depend on these (that's why
  some hunks fail), so cherry-picking gets harder with every step down the list.
- The libretro OSD barely uses the changed APIs (`window.cpp` already uses
  `frame_period().as_hz()`), so a full merge is mostly driver-level conflicts.
- Expected conflict hotspots on a merge: `video/psx.cpp` (ram_device vs our GPU HLE, the
  biggest), PPC DRC (our three fixes vs the TLB rework), `dspp.cpp` (our idle-skip vs RMAP),
  `viper.cpp` and `voodoo_*` (trivial).

## Recommendation
- **Cherry-pick now** (clean, low risk, high relevance): `0e3c9c6cd14` (drcbex64 hunk only),
  `fac12419a82`, `6e65bf15368`, `764de9b5b48`, `2b5c770ba76`, `d1e9f7db995`, plus the
  clean-applying namcos22 fixes.
- **Port by hand, one at a time**: `8f158369e6a`, then `1a8267fa579` and `0ee5c47faa7`,
  dropping our `af439229f3d` and reworking `6a0cf6b4aaf` on top. Re-measure the gticlub2
  recompile rate and M2 speed after each step.
- **The PSX SPU overhaul** is the biggest audible win for our main target, but it's tangled with
  the GPU ram_device change. Either port only `spu.cpp` plus the driver RAM wiring, or wait for
  mame0290 and resolve `video/psx.cpp` once.

## Verification (for any later pick)
Build with the usual command (`-j4`, `PREMAKE=0`). Cold-boot smoke tests via the `mame-dev`
Distrobox:
- DRC/PPC: gticlub2, jpark3, polystar, evilngt, daytona2, scud, gradius4. Compare speed and
  recompile rate with `fork-specific/tools/profile_run.py`. Cold boots are mandatory for DRC
  changes.
- PS1: brvblade, raystorm, starswep, gdarius2 with the GPU HLE on and off.
- Driver fixes: the affected romset listed with each one above.
