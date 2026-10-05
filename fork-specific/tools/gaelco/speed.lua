-- prints emulated time, render-free speed and screen frame number every 5 emulated seconds
local last = -1
emu.register_frame_done(function()
  local t = math.floor(manager.machine.time:as_double() / 5)
  if t ~= last then
    last = t
    print(string.format("GAELCO speed t=%d speed=%.1f%%", t * 5, manager.machine.video.speed_percent))
  end
end)
