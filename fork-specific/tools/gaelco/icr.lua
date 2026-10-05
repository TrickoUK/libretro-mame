local main = manager.machine.devices[":maincpu"]
local done=false
emu.register_frame_done(function()
  if done or manager.machine.time:as_double() < 15 then return end
  done=true
  for _, k in ipairs({"ICR","IPRA","IPRB","IPRC","SR"}) do
    local ok, v = pcall(function() return main.state[k].value end)
    print("GAELCO", k, ok and string.format("%x", v) or "n/a")
  end
  manager.machine:exit()
end)
