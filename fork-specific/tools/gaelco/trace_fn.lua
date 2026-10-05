-- trace sub CPU instructions from the first entry of 0x8c0082d4 until the halt loop at 0x8c001e82
local sub = manager.machine.devices[":subcpu"]
local d = manager.machine.debugger
d.visible_cpu = sub
local out = "/var/home/bazzite/Projects/libretro/mame/fork-specific/out/gaelco3/trace_fn.txt"
d:command('bpset 8c0082d4,1,{trace ' .. out .. ',:subcpu,noloop; g}')
d:command('bpset 8c001e82,1,{trace off; g}')
print("GAELCO trace armed")
