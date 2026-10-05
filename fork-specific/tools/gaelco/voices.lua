-- print the software mixer state (operand cache RAM 0x7c001000..) of the main CPU at a few times
local main = manager.machine.devices[":maincpu"]
local sp = main.spaces["program"]
local times = {20, 45, 70, 95}
local done = {}
emu.register_frame_done(function()
  local t = manager.machine.time:as_double()
  for _, s in ipairs(times) do
    if not done[s] and t >= s then
      done[s] = true
      local function w(a) return string.format("%08x", sp:read_u32(a)) end
      local line = string.format("GAELCO voices t=%d mask[1288]=%s buf[1280]=%s [1284]=%s [128c]=%s", s, w(0x7c001288), w(0x7c001280), w(0x7c001284), w(0x7c00128c))
      print(line)
      for v = 0, 3 do
        local b = 0x7c001000 + v * 40
        local x = {}
        for k = 0, 9 do x[#x+1] = w(b + k*4) end
        print(string.format("GAELCO voice%d %s", v, table.concat(x, " ")))
      end
    end
  end
end)
