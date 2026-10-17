# Light Gun Improvements

A living record of the games and systems where this fork made **light guns work or work better**, mainly
for a Sinden on Batocera. Small fixes still go in `custom-change-log.md` too; this file is the one place
that lists every gun game, what was wrong and what was done.

Last updated: 2026-10-17.

**How this stays current.** `CLAUDE.md` ("Light gun doc") makes updating this file part of any light gun work:
add or edit the game's row in the summary table and its section, and update the shared-plumbing section if
the libretro input side changed. Update it in the same commit as the work.

Status words: **Tested with a gun** means it was tried with a real light gun (a Sinden on Batocera).
**Tested by script** means the aim values were driven from a Lua script, which proves the game reads them and
the range and direction are right, but not the gun path itself.

## Summary

| Game / system | Problem | What we did | Status |
|---|---|---|---|
| **Jurassic Park III** (`jpark3`, Konami Viper) | Every shot was read as a reload | Gun port layout fixed (`viper.cpp`) | Tested with a gun |
| **Operation Thunder Hurricane** (`thunderh`, GTI Club hardware) | Trigger fired but the aim point never moved | Gun position written into the aim point the game normally moves with the stick (player 1 only) | Working for player 1; player 2 not done |
| **Operation Tiger** (`optiger`, Taito Power-JC) | Aim axes were analog sticks, which a gun cannot drive | Aim axes changed to light gun axes for both players, full screen range | Tested by script; not yet with a gun |
| **House of the Dead** (`hotd`, Sega Model 2) | Full-screen flash on every trigger pull | ROM patch removes the flash (Revision A only) | Working |
| **Operation Wolf** (`opwolf`, Taito) | Full-screen flash on every trigger pull | Runtime ROM patch about 900 frames in (the C-chip ROM check rejects a static patch) | Working |
| **Gunblade NY** (`gunblade`, Sega Model 2) | Gun Y axis was upside down on the libretro gun device | `PORT_REVERSE` on gun Y under `__LIBRETRO__` | Working |
| **Total Vice, Evil Night** (Konami M2) | None found | Used as test games for the shared gun input below | Tested with a gun |
| **All games** (libretro gun input) | Reload could not shoot off screen; gun Start/Select did nothing for coin and start | See the shared plumbing section | Tested with a gun |

## Shared plumbing

These apply to every light gun game, not one driver.

- **Libretro input** (`src/osd/modules/input/input_retro.cpp`, commit `d8f3c392f0f`): in "free" offscreen mode the
  reload button now shoots off screen. A gun's Start and Select also press that player's pad Start and Select, so
  coin and start work from the gun alone.
- **Reload auto-bind plugin** (`plugins/offscreenreload/init.lua`): binds reload to BUTTON2 for light guns, and
  skips the bind when a game already uses BUTTON2 for gameplay. This was Batocera patch 003 before it moved
  into the fork.
- **Batocera's RetroArch** (outside this repo): it must use its `udev` input driver. Batocera patch
  `019-disable-wayland-events-by-wayland.patch` has to cover `wayland_vk_ctx.c` (the box runs Vulkan), or every
  gun shares the same buttons and gun Start and Coin fail. **If gun buttons break after a new image, check
  RetroArch before the core**: `[udev] Mouse/Touch #N` lines in the box's `es_launch_stderr.log` mean udev input
  is live. An image on 2026-09-26 shipped a stale RetroArch without the fix.
- **Port type matters.** A gun can only drive `IPT_LIGHTGUN_X/Y` fields. Games that read the aim from analog
  sticks (`IPT_AD_STICK_X/Y`) get no gun aim until their aim ports are switched, as was done for Operation Tiger.
  Light gun fields have no gamepad stick in MAME's default bindings, so switching them can cost pad aiming.

## Per-game notes

### Jurassic Park III (`viper.cpp`)
- **Cause**: the gun port layout made every shot read as a reload.
- **Fix**: `c79b34ab13b`. Written up in `viper-investigation.md` (Fix 8).
- **Checked**: on a Batocera box with a Sinden.

### Operation Thunder Hurricane (`gticlub.cpp`)
- **Cause**: the real I/O board reads two cameras and reports over a network chip that isn't emulated, so the
  aim point had nothing to move it.
- **Fix**: `13b22dc7830`. The lightgun X/Y position (the inputs a Sinden drives) is written into the aim point the
  game normally moves with the stick, so shots land where the gun points. The stick still works too.
- **Fork-only shortcut**: this does not model the hardware, so it is not upstream material.
- **Not done**: player 2.

### Operation Tiger (`taitopjc.cpp`)
- **Cause**: the four aim ports (P1 and P2, X and Y) were `IPT_AD_STICK_X/Y` with crosshairs.
- **Fix**: they are now `IPT_LIGHTGUN_X/Y`, keeping the 10-bit range, the X reversal and the crosshair scale.
  Measured on the running game: aim value 0 to 1023 spans the whole screen, X=1023 is the left edge and
  Y=1023 is the bottom edge, which matches the existing reversal.
- **Checked**: by script only (coin, start, a stage played through, shots land where aimed). Not yet with a
  physical gun.
- **Known gaps**: no sound (the sound CPU's internal ROM is undumped), and the driver is still flagged not
  working upstream. A gamepad stick no longer aims.

### House of the Dead, Operation Wolf, Gunblade NY
- **`hotd`** (`src/mame/sega/model2*`): no trigger flash, via an `init_hotd` ROM patch. Revision A only.
- **`opwolf`** (`src/mame/taito/opwolf.cpp`): no trigger flash, via a runtime ROM patch about 900 frames in.
- **`gunblade`**: gun Y gets `PORT_REVERSE` under `__LIBRETRO__`, because the libretro gun device reports Y the
  other way round.
- These came from Batocera patches 004-007 and moved into the fork on 2026-09-26.

## Not done

- **Operation Thunder Hurricane player 2** aim.
- **Operation Tiger with a physical Sinden**: confirm the aim lines up, and decide whether to restore gamepad aiming.
- **Games not looked at yet**: any other game whose aim comes from analog sticks instead of light gun ports.

## Adding to this document

When light gun work lands:
1. Add or update the game's row in **Summary** (problem, fix, and whether it was tested with a gun or by script).
2. Add a per-game note if the fix needs more than one line.
3. Update **Shared plumbing** if the libretro input side, the reload plugin or the Batocera requirements changed.
4. Keep the matching entry in `custom-change-log.md`.
