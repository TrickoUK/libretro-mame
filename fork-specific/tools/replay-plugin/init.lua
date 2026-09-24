-- license:BSD-3-Clause
-- Runs the Lua file named by the MAME_REPLAY_SCRIPT environment variable once the machine has
-- started. Used with fork-specific/tools/replay_inputs.py for frame-exact replays from a save
-- state; start it with MAME_EXTRA_PLUGIN=replay (see retro_init.cpp).
local exports = {
	name = 'replay',
	version = '0.1',
	description = 'Frame-exact replay script loader',
	license = 'BSD-3-Clause',
	author = { name = 'libretro-mame fork' } }

local replay = exports

function replay.startplugin()
	local path = os.getenv('MAME_REPLAY_SCRIPT')
	if not path then
		return
	end
	local started = false
	replay.reset_subscription = emu.add_machine_reset_notifier(function ()
		if not started then
			started = true
			dofile(path)
		end
	end)
end

return exports
