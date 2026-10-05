-- print all state registers of both CPUs after 12s emulated
local done=false
emu.register_frame_done(function()
  if done or manager.machine.time:as_double() < 12 then return end
  done=true
  for _, tag in ipairs({":maincpu", ":subcpu"}) do
    local c = manager.machine.devices[tag]
    local names = {}
    for k, v in pairs(c.state) do names[#names+1] = k end
    table.sort(names)
    local s = tag .. ":"
    for _, k in ipairs(names) do
      if k ~= "FPR" and not k:match("^[FD]R%d") and not k:match("^XF") and not k:match("^XD") and not k:match("^R%d+_BANK") then
        s = s .. string.format(" %s=%x", k, c.state[k].value)
      end
    end
    print("GAELCO " .. s)
  end
  manager.machine:exit()
end)
