-- prints sub/main PC once per emulated second (autoboot script: MAME_EXTRA_OPTS="-autoboot_delay 0 -autoboot_script ...")
local sub = manager.machine.devices[":subcpu"]
local main = manager.machine.devices[":maincpu"]
local last = -1
emu.register_frame_done(function()
  local t = math.floor(manager.machine.time:as_double())
  if t ~= last then
    last = t
    print(string.format("GAELCO t=%d sub=%08x main=%08x", t, sub.state["PC"].value, main.state["PC"].value))
  end
end)
