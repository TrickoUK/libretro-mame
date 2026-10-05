-- disassemble sub CPU around PC and a few ranges listed in DASM_RANGES into fork-specific/out/gaelco3/
local sub = manager.machine.devices[":subcpu"]
local d = manager.machine.debugger
d.visible_cpu = sub
local pc = sub.state["PC"].value
print("GAELCO dasm pc=" .. string.format("%08x", pc))
d:command(string.format("dasm /var/home/bazzite/Projects/libretro/mame/fork-specific/out/gaelco3/halt.txt,%x,0x100,1", (pc - 0x80) & 0xfffffffe))
print("GAELCO dasm done")
