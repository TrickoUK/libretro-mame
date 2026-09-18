// license:BSD-3-Clause
// copyright-holders:smf
/*
 * PlayStation DMA emulator
 *
 * Copyright 2003-2011 smf
 *
 */

#ifndef MAME_CPU_PSX_DMA_H
#define MAME_CPU_PSX_DMA_H

#pragma once


DECLARE_DEVICE_TYPE(PSX_DMA, psxdma_device)

class psxdma_device : public device_t
{
public:
	typedef delegate<void (uint32_t *, uint32_t, int32_t)> read_delegate;
	typedef delegate<void (uint32_t *, uint32_t, int32_t)> write_delegate;
	// PGXP (CLAUDE.md Phase 4c): any DMA transfer that writes device data
	// INTO m_ram (a "read block" transfer below, plus the channel-6
	// reverse-clear's direct m_ram[] stores) bypasses the CPU's OP_SW/
	// SWC2 shadow-propagation entirely - it never executes those
	// instructions, so psxcpu_device's m_pgxp_ram_shadow never learns
	// those RAM words changed. Left unfixed, a RAM address that once held
	// a shadowed vertex word keeps reporting that stale shadow as valid
	// even after DMA overwrites it with unrelated data (image/audio/CD
	// payload, or later a *different* vertex at the same scratch
	// address), corrupting whatever later reads it via LW. Bound (see
	// psxcpu_device::device_reset()) to invalidate the shadow range a DMA
	// write just touched.
	typedef delegate<void (uint32_t, uint32_t)> invalidate_delegate;
	void set_ram_write_callback( invalidate_delegate cb ) { m_ram_write_cb = cb; }

	psxdma_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock = 0);

	//configuration helpers
	auto irq() { return m_irq_handler.bind(); }

	void install_read_handler( int n_channel, read_delegate p_fn_dma_read );
	void install_write_handler( int n_channel, write_delegate p_fn_dma_write );

	void write(offs_t offset, uint32_t data, uint32_t mem_mask = ~0);
	uint32_t read(offs_t offset, uint32_t mem_mask = ~0);

	uint32_t *m_ram;
	size_t m_ramsize;
	invalidate_delegate m_ram_write_cb;

protected:
	virtual void device_start() override ATTR_COLD;
	virtual void device_reset() override ATTR_COLD;

private:
	struct psx_dma_channel
	{
		uint32_t n_base;
		uint32_t n_blockcontrol;
		uint32_t n_channelcontrol;
		emu_timer *timer;
		read_delegate fn_read;
		write_delegate fn_write;
		uint32_t n_ticks;
		uint32_t b_running;
	};

	void dma_start_timer( int n_channel, uint32_t n_ticks );
	void dma_stop_timer( int n_channel );
	void dma_timer_adjust( int n_channel );
	void dma_interrupt_update();
	TIMER_CALLBACK_MEMBER( dma_finished );

	psx_dma_channel m_channel[7];
	uint32_t m_dpcp;
	uint32_t m_dicr;

	devcb_write_line m_irq_handler;
};

#endif
