local main = manager.machine.devices[":maincpu"]
local sp = main.spaces["program"]
local done=false
emu.register_frame_done(function()
  if done or manager.machine.time:as_double() < 40 then return end
  done=true
  local f = io.open("/var/home/bazzite/Projects/libretro/mame/fork-specific/out/gaelco64/flash_top.bin","wb")
  local t = {}
  for a = 0x143e0000, 0x143fffff do t[#t+1] = string.char(sp:read_u8(a)) end
  f:write(table.concat(t)); f:close()
  print("GAELCO flash dumped")
  manager.machine:exit()
end)
