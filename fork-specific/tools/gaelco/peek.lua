local sub = manager.machine.devices[":subcpu"]
local sp = sub.spaces["program"]
local done=false
emu.register_frame_done(function()
  if done or manager.machine.time:as_double() < 6 then return end
  done=true
  local function w(a) return string.format("%08x", sp:read_u32(a)) end
  print("GAELCO peek 020013e4: "..w(0x020013e4).." "..w(0x020013e8).." "..w(0x020013ec).." "..w(0x020013f0))
  print("GAELCO peek 01820000: "..w(0x01820000).." "..w(0x01820004))
  print("GAELCO peek 8c012da4 ctx ptr: "..w(0x0c012da4))
  -- file table at 8c006d5c, entries 0x10 bytes each; show fd 0..5
  for fd=0,5 do
    local base = 0x0c006d5c + fd*16
    print(string.format("GAELCO fd%d: %s %s %s %s", fd, w(base), w(base+4), w(base+8), w(base+12)))
  end
  manager.machine:exit()
end)
