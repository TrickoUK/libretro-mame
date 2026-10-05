-- dump sub CPU RAM (0x0c000000, DUMP_SIZE bytes) to fork-specific/out/gaelco3/subram.bin after 6s
local sub = manager.machine.devices[":subcpu"]
local sp = sub.spaces["program"]
local main = manager.machine.devices[":maincpu"]
local mp = main.spaces["program"]
local done=false
emu.register_frame_done(function()
  if done or manager.machine.time:as_double() < 8 then return end
  done=true
  local f = io.open("/var/home/bazzite/Projects/libretro/mame/fork-specific/out/gaelco3/subram.bin","wb")
  local t = {}
  for a=0x0c000000, 0x0c000000+0x400000-4, 4 do
    t[#t+1] = string.pack("<I4", sp:read_u32(a))
    if #t >= 4096 then f:write(table.concat(t)); t = {} end
  end
  f:write(table.concat(t)); f:close()
  local f2 = io.open("/var/home/bazzite/Projects/libretro/mame/fork-specific/out/gaelco3/mainram.bin","wb")
  t = {}
  for a=0x0c000000, 0x0c000000+0x800000-4, 4 do
    t[#t+1] = string.pack("<I4", mp:read_u32(a))
    if #t >= 4096 then f2:write(table.concat(t)); t = {} end
  end
  f2:write(table.concat(t)); f2:close()
  print("GAELCO dumped")
  manager.machine:exit()
end)
