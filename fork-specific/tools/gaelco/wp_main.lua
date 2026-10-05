-- watch writes to the flag the main loop waits on (0x8c0766f4), plus entries to the IRL10 / TMU1 handlers
local main = manager.machine.devices[":maincpu"]
local d = manager.machine.debugger
d.visible_cpu = main
d:command('wpset 0c0766f4,4,w,1,{logerror "GAELCO FLAGW pc=%08X data=%08X\\n",pc,wpdata; g}')
d:command('bpset 8c0644e0,1,{logerror "GAELCO IRL10 handler entered\\n"; g}')
d:command('bpset 8c057c3c,1,{logerror "GAELCO TMU1 handler entered\\n"; g}')
print("GAELCO wp armed")
