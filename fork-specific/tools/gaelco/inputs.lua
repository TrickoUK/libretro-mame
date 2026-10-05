-- timed input script for smashdrv/atvtrack (autoboot script). Edit SCHEDULE: {emu_seconds, port, field_name, value}
local SCHEDULE = {
  {30, ":JP10", "Coin 1", 1}, {30.5, ":JP10", "Coin 1", 0},
}
local env = os.getenv("ATV_INPUTS")
if env then SCHEDULE = load("return " .. env)() end
local ports = manager.machine.ioport.ports
local done = {}
emu.register_frame_done(function()
  local t = manager.machine.time:as_double()
  for i, e in ipairs(SCHEDULE) do
    if not done[i] and t >= e[1] then
      done[i] = true
      local p = ports[e[2]]
      local f = p and p.fields[e[3]]
      if f then f:set_value(e[4]) print(string.format("GAELCO input t=%.1f %s %s=%d", t, e[2], e[3], e[4]))
      else print("GAELCO input field not found: " .. e[2] .. " " .. e[3]) end
    end
  end
end)
