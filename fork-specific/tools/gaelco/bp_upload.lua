-- log the sub CPU's GPU upload routine args (needs ATV_SUBFIRST=1 so the sub is the debugger default CPU)
local d = manager.machine.debugger
d:command('bpset 8c008442,1,{logerror "GAELCO UPLOAD pc=%08X dst=%08X src=%08X len=%08X r13=%08X pr=%08X r14=%08X\\n",pc,r12,r9,r7,r13,pr,r14; g}')
d:command('bpset 8c00ccd8,1,{logerror "GAELCO CREATE pc=%08X r4=%08X r5=%08X r6=%08X pr=%08X\\n",pc,r4,r5,r6,pr; g}')
print("GAELCO bp armed")
