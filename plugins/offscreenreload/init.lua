-- license:BSD-3-Clause
-- copyright-holders:Vas Crabb
local exports = {
	name = 'offscreenreload',
	version = '0.0.1',
	description = 'Off-screen reload helper plugin',
	license = 'BSD-3-Clause',
	author = { name = 'Vas Crabb' } }


local offscreenreload = exports

local frame_subscription, stop_subscription

function offscreenreload.startplugin()
	--[[
	  Configuration data:
	  * binding: activation sequence (input sequence)
	  * bindingcfg: activation sequence configuration (string)
	  * axis:
	    * port: port tag (string)
	    * mask: port field mask (integer)
	    * type: port field type (integer)
	    * field: field (I/O port field)
	  * button:
	    * port: port tag (string)
	    * mask: port field mask (integer)
	    * type: port field type (integer)
	    * field: field (I/O port field)

	  Live state:
	  * pressed: currently active (Boolean or nil)
	]]
	local helpers = { }
	local menu
	local input

	local function process_frame()
		for index, helper in ipairs(helpers) do
			if input:seq_pressed(helper.binding) then
				if not helper.pressed then
					if helper.axis.field then
						helper.axis.field:set_value(helper.axis.field.minvalue)
					end
					if helper.button.field then
						helper.button.field:set_value(1)
					end
					helper.pressed = true
				end
			else
				if helper.pressed then
					if helper.axis.field then
						helper.axis.field:clear_value()
					end
					if helper.button.field then
						helper.button.field:clear_value()
					end
					helper.pressed = nil
				end
			end
		end
	end

	local function auto_configure()
		local ioport = manager.machine.ioport
		local result = {}
		local players = {}

		for tag, port in pairs(ioport.ports) do
			for fname, field in pairs(port.fields) do
				local type_token = ioport:input_type_to_token(field.type)
				if type_token then
					local p = field.player + 1
					if not players[p] then
						players[p] = {}
					end
					local info = {
						port = field.port.tag,
						mask = field.mask,
						type = field.type,
						field = field
					}

					if (type_token:match('_LIGHTGUN_Y$') or type_token:match('_AD_STICK_Y$'))
					   and field.is_analog and not field.analog_wraps then
						players[p].y = info
					elseif (type_token:match('_LIGHTGUN_X$') or type_token:match('_AD_STICK_X$'))
					   and field.is_analog and not field.analog_wraps then
						players[p].x = info
					elseif type_token:match('_BUTTON1$')
					   and not field.is_analog and not field.is_toggle then
						if not players[p].trigger then
							players[p].trigger = info
						end
					elseif type_token:match('_BUTTON2$')
					   and not field.is_analog and not field.is_toggle then
						-- already used for real gameplay (e.g. "foot pedal"),
						-- which shares the same physical gun button as reload
						players[p].button2_used = true
					end
				end
			end
		end

		for player_num, pdata in pairs(players) do
			if pdata.y and pdata.trigger and not pdata.button2_used then
				local token = 'GUNCODE_' .. player_num .. '_BUTTON2'
				local seq = manager.machine.input:seq_from_tokens(token)
				if seq then
					table.insert(result, {
						binding = seq,
						bindingcfg = token,
						axis = pdata.y,
						button = pdata.trigger
					})
					if pdata.x then
						table.insert(result, {
							binding = seq,
							bindingcfg = token,
							axis = pdata.x,
							button = pdata.trigger
						})
					end
				end
			end
		end

		return result
	end

	local function start()
		input = manager.machine.input
		local persister = require('offscreenreload/offscreenreload_persist')
		helpers = persister.load_settings()
		if #helpers == 0 then
			helpers = auto_configure()
			if #helpers > 0 then
				persister:save_settings(helpers)
			end
		end
	end

	local function stop()
		local persister = require('offscreenreload/offscreenreload_persist')
		persister:save_settings(helpers)

		helpers = { }
		menu = nil
	end

	local function menu_callback(index, event)
		return menu:handle_event(index, event)
	end

	local function menu_populate()
		if not menu then
			menu = require('offscreenreload/offscreenreload_menu')
			menu:init(helpers)
		end
		return menu:populate()
	end

	frame_subscription = emu.add_machine_frame_notifier(process_frame)
	emu.register_prestart(start)
	stop_subscription = emu.add_machine_stop_notifier(stop)
	emu.register_menu(menu_callback, menu_populate, _p('plugin-offscreenreload', 'Off-Screen Reload Helper'))
end

return exports
