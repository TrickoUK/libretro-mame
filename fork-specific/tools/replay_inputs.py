#!/usr/bin/env python3
# Usage: replay_inputs.py OUT.lua [--gas 0xff] [--tap START:LEN:VALUE ...] [--dump FILE] [--dump-range LO:HI]
#                         [--frames N]
# Writes a Lua script for frame-exact input replay from a save state (gticlub2 port
# names). Frame 0 is the first frame after the state load, detected as machine time > 5s (a cold
# boot never gets there before the entry state is applied). From then on it holds the gas and sets
# the wheel every frame (centre 0x80 unless a --tap covers that frame), and optionally appends a
# raw memory snapshot per frame to --dump (big-endian, one --dump-range block per frame), and
# whole-work-RAM snapshots at chosen frames to --full-dump.
# Run with profile_run.py ... --state S --env MAME_EXTRA_PLUGIN=replay --env MAME_REPLAY_SCRIPT=OUT.lua (needs the retro_init.cpp
# MAME_EXTRA_PLUGIN hook and replay-plugin/ copied into <system>/mame/plugins/replay).
import argparse

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("--gas", default="0xff")
ap.add_argument("--tap", action="append", default=[], metavar="START:LEN:VALUE")
ap.add_argument("--dump")
ap.add_argument("--dump-range", default="0x560000:0x56ffff")
ap.add_argument("--frames", type=int, default=240)
ap.add_argument("--steering-response", type=int, help="gticlub2 Steering Response config value (0 = linear)")
ap.add_argument("--steering-smoothing", type=lambda x: int(x, 0), help="gticlub2 Steering Smoothing config value (0, 4, 8 or 0xc)")
ap.add_argument("--full-dump", help="prefix for whole-work-RAM snapshots (PREFIX_<frame>.bin)")
ap.add_argument("--full-dump-frames", default="", help="comma-separated frame numbers for --full-dump")
a = ap.parse_args()

taps = []
for t in a.tap:
    start, length, value = t.split(":")
    taps.append(f"{{{int(start, 0)}, {int(length, 0)}, {int(value, 0)}}}")
lo, hi = (int(x, 0) for x in a.dump_range.split(":"))

lua = f"""
local ports = manager.machine.ioport.ports
local gas = ports[':AN1'].fields['Gas Pedal']
local wheel = ports[':AN0'].fields['Steering Wheel']
local space = manager.machine.devices[':maincpu'].spaces['program']
local taps = {{ {", ".join(taps)} }}
local dumpfile = {f"io.open('{a.dump}', 'wb')" if a.dump else "nil"}
local frame = -1
{f"ports[':STEERING'].fields['Steering Response'].user_value = {a.steering_response}" if a.steering_response is not None else ""}
{f"ports[':STEERING'].fields['Steering Smoothing'].user_value = {a.steering_smoothing}" if a.steering_smoothing is not None else ""}
local full_prefix = {repr(a.full_dump) if a.full_dump else "nil"}
local full_frames = {{ {", ".join(f"[{int(x)}]=true" for x in a.full_dump_frames.split(",") if x)} }}
replay_sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if frame < 0 then
    if t < 5.0 then return end
    frame = 0
    print(string.format('REPLAY start t=%.6f', t))
  end
  if frame >= {a.frames} then
    if dumpfile then dumpfile:close(); dumpfile = nil end
    return
  end
  gas:set_value({int(a.gas, 0)})
  local w = 0x80
  for _, tap in ipairs(taps) do
    if frame >= tap[1] and frame < tap[1] + tap[2] then w = tap[3] end
  end
  wheel:set_value(w)
  if dumpfile then dumpfile:write(space:read_range({lo:#x}, {hi:#x}, 8)) end
  if full_prefix and full_frames[frame] then
    local f = io.open(full_prefix .. '_' .. frame .. '.bin', 'wb')
    f:write(space:read_range(0, 0xffffff, 8))
    f:close()
  end
  frame = frame + 1
end)
"""
open(a.out, "w").write(lua)
