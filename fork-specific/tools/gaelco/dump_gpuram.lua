-- sparse dump of the GPU RAM window (sub CPU 0x18000000, 32MB) at emulated 10s and 18s:
-- file = repeated [u32 page_index][4096 bytes], only non-zero 4K pages
local sub = manager.machine.devices[":subcpu"]
local sp = sub.spaces["program"]
local out = "/var/home/bazzite/Projects/libretro/mame/fork-specific/out/gaelco3/"
local done = {}
local function dump(tag)
  local f = io.open(out .. "gpuram_" .. tag .. ".bin", "wb")
  local pages = 0
  for page = 0, 0x2000000 / 4096 - 1 do
    local base = 0x18000000 + page * 4096
    local words, nz = {}, false
    for i = 0, 1023 do
      local v = sp:read_u32(base + i * 4)
      if v ~= 0 then nz = true end
      words[#words+1] = string.pack("<I4", v)
    end
    if nz then f:write(string.pack("<I4", page), table.concat(words)); pages = pages + 1 end
  end
  f:close()
  print("GAELCO gpuram " .. tag .. " nonzero pages " .. pages)
end
emu.register_frame_done(function()
  local t = manager.machine.time:as_double()
  for _, s in ipairs({45, 80}) do
    if not done[s] and t >= s then
      done[s] = true
      dump(tostring(s))
      if s == 80 then manager.machine:exit() end
    end
  end
end)
