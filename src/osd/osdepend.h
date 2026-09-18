// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    osdepend.h

    OS-dependent code interface.

***************************************************************************/
#ifndef MAME_OSD_OSDEPEND_H
#define MAME_OSD_OSDEPEND_H

#pragma once


#include "emufwd.h"

#include "bitmap.h"
#include "interface/audio.h"
#include "interface/gpurender.h"
#include "interface/midiport.h"
#include "interface/nethandler.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <array>

// forward references
class input_type_entry;
namespace osd { class midi_input_port; class midi_output_port; }
namespace ui { class menu_item; }


//============================================================
//  TYPE DEFINITIONS
//============================================================

// ======================> osd_font interface

class osd_font
{
public:
	typedef std::unique_ptr<osd_font> ptr;

	virtual ~osd_font() { }

	/** attempt to "open" a handle to the font with the given name */
	virtual bool open(std::string const &font_path, std::string const &name, int &height) = 0;

	/** release resources associated with a given OSD font */
	virtual void close() = 0;

	/*!
	 * allocate and populate a BITMAP_FORMAT_ARGB32 bitmap containing
	 * the pixel values rgb_t(0xff,0xff,0xff,0xff) or
	 * rgb_t(0x00,0xff,0xff,0xff) for each pixel of a black & white font
	 */
	virtual bool get_bitmap(char32_t chnum, bitmap_argb32 &bitmap, std::int32_t &width, std::int32_t &xoffs, std::int32_t &yoffs) = 0;
};

// ======================> osd_interface

// description of the currently-running machine
class osd_interface
{
public:
	// general overridables
	virtual void init(running_machine &machine) = 0;
	virtual void update(bool skip_redraw) = 0;
	virtual void input_update(bool relative_reset) = 0;
	virtual void check_osd_inputs() = 0;
	virtual void set_verbose(bool print_verbose) = 0;

	// debugger overridables
	virtual void init_debugger() = 0;
	virtual void wait_for_debugger(device_t &device, bool firststop) = 0;

	// audio overridables
	virtual bool no_sound() = 0;
	virtual bool sound_external_per_channel_volume() = 0;
	virtual bool sound_split_streams_per_source() = 0;
	virtual uint32_t sound_get_generation() = 0;
	virtual osd::audio_info sound_get_information() = 0;
	virtual uint32_t sound_stream_sink_open(uint32_t node, std::string name, uint32_t rate) = 0;
	virtual uint32_t sound_stream_source_open(uint32_t node, std::string name, uint32_t rate) = 0;
	virtual void sound_stream_close(uint32_t id) = 0;
	virtual void sound_stream_sink_update(uint32_t id, const int16_t *buffer, int samples_this_frame) = 0;
	virtual void sound_stream_source_update(uint32_t id, int16_t *buffer, int samples_this_frame) = 0;
	virtual void sound_stream_set_volumes(uint32_t id, const std::vector<float> &db) = 0;
	virtual void sound_begin_update() = 0;
	virtual void sound_end_update() = 0;

	// input overridables
	virtual void customize_input_type_list(std::vector<input_type_entry> &typelist) = 0;

	// video overridables
	virtual void add_audio_to_recording(const int16_t *buffer, int samples_this_frame) = 0;
	virtual std::vector<ui::menu_item> get_slider_list() = 0;

	// font interface
	virtual osd_font::ptr font_alloc() = 0;
	virtual bool get_font_families(std::string const &font_path, std::vector<std::pair<std::string, std::string> > &result) = 0;

	// command option overrides
	virtual bool execute_command(const char *command) = 0;

	// MIDI interface
	virtual std::unique_ptr<osd::midi_input_port> create_midi_input(std::string_view name) = 0;
	virtual std::unique_ptr<osd::midi_output_port> create_midi_output(std::string_view name) = 0;
	virtual std::vector<osd::midi_port_info> list_midi_ports() = 0;

	// network interface
	virtual std::unique_ptr<osd::network_device> open_network_device(int id, osd::network_handler &handler) = 0;
	virtual std::vector<osd::network_device_info> list_network_devices() = 0;

	// GPU render interface - get_gpu_render_target() returns nullptr if
	// this OSD/build doesn't support it, or if it does but the user has
	// disabled it (e.g. via a frontend core option); callers must always
	// have a software fallback. gpu_render_available() is a cheap query -
	// no runtime GPU context creation - safe to call early (e.g. to decide
	// a screen's declared resolution before any actual rendering happens)
	// without the ordering hazards that come with eagerly constructing a
	// real GPU context too early relative to the host frontend's own
	// graphics setup (see psxgpu_device::updatevisiblearea()). It reflects
	// both build-time support AND the user's current on/off choice, so a
	// caller only needs this one check.
	virtual bool gpu_render_available() const { return false; }
	virtual osd::gpu_render_target *get_gpu_render_target() = 0;

	// The resolution multiplier a caller should scale by when
	// gpu_render_available() is true (e.g. 2 or 4 for 2x/4x internal
	// resolution) - user-selectable where the OSD supports it (see the
	// retro OSD's mame_psx_gpu_hle core option). Meaningless/unspecified
	// when gpu_render_available() is false; default of 1 here covers that
	// case safely for any caller that queries it regardless.
	virtual int gpu_render_scale() const { return 1; }

	// Whether GTE-sourced GPU polygon vertices should be corrected using
	// PGXP-style geometry correction (see the retro OSD's
	// mame_psx_gpu_pgxp core option and CLAUDE.md Phase 4) - meaningless/
	// unspecified when gpu_render_available() is false, same as
	// gpu_render_scale(); default of false covers that case safely.
	virtual bool gpu_render_pgxp_enabled() const { return false; }

	// MSAA sample count the GPU render target should use (e.g. 4 for 4x
	// MSAA, 0 to disable) - user-selectable where the OSD supports it (see
	// the retro OSD's mame_psx_gpu_msaa core option). Read once, at
	// get_gpu_render_target()'s first (lazy) construction of the target -
	// changing it takes effect on the next restart, same as
	// gpu_render_scale()/gpu_render_pgxp_enabled(). Meaningless/
	// unspecified when gpu_render_available() is false; default of 0
	// covers that case safely.
	virtual int gpu_render_msaa_samples() const { return 0; }

	// Texture filtering mode for the GPU render target (0 = disabled/
	// nearest, PS1-accurate; 1 = bilinear; 2 = trilinear) - user-selectable
	// where the OSD supports it (see the retro OSD's mame_psx_gpu_texfilter
	// core option). Read once, at get_gpu_render_target()'s first (lazy)
	// construction of the target, same as gpu_render_msaa_samples().
	// Meaningless/unspecified when gpu_render_available() is false;
	// default of 0 covers that case safely.
	virtual int gpu_render_texfilter() const { return 0; }

protected:
	virtual ~osd_interface() { }
};

#endif  // MAME_OSD_OSDEPEND_H
