local done=false
emu.register_frame_done(function()
  if done then return end
  done=true
  for tag, p in pairs(manager.machine.ioport.ports) do
    for name, f in pairs(p.fields) do print(string.format("GAELCO port %s field '%s' mask=%x", tag, name, f.mask)) end
  end
end)
