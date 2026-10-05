-- prints both SH4 PCs and a few regs; run via lua_run.py --lua T:probe.lua
local function dump(tag, c)
  local s = tag .. " pc=" .. string.format("%08x", c.state["PC"].value)
  for _, r in ipairs({"R4","R5","R13","R14","PR"}) do s = s .. " " .. r .. "=" .. string.format("%08x", c.state[r].value) end
  print("GAELCO " .. s)
end
dump("SUB", manager.machine.devices[":subcpu"])
dump("MAIN", manager.machine.devices[":maincpu"])
print("GAELCO frame " .. manager.machine.screens[":screen"]:frame_number())
