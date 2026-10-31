// license:BSD-3-Clause
// copyright-holders:TrickoUK
#ifndef MAME_WILLIAMS_MIDWAY_DIEGO_H
#define MAME_WILLIAMS_MIDWAY_DIEGO_H

#pragma once

#include "bus/rs232/rs232.h"

class midway_diego_device : public buffered_rs232_device<64>, public device_nvram_interface
{
public:
	midway_diego_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock = 0);

protected:
	// device_t implementation
	virtual ioport_constructor device_input_ports() const override ATTR_COLD;
	virtual void device_start() override ATTR_COLD;
	virtual void device_reset() override ATTR_COLD;

	// device_nvram_interface implementation
	virtual void nvram_default() override;
	virtual bool nvram_read(util::read_stream &file) override;
	virtual bool nvram_write(util::write_stream &file) override;

	// buffered_rs232_device implementation
	virtual void received_byte(u8 data) override;

private:
	static constexpr unsigned FRAME_LENGTH = 8;

	void frame_received();
	void send_frame();
	static u8 add_carry(u8 a, u8 b);

	required_ioport m_switches;
	required_ioport_array<2> m_analog;
	required_ioport m_coins;

	u8 m_eeprom[128];

	u8 m_rx_buffer[FRAME_LENGTH + 2];
	u8 m_rx_count;
	bool m_rx_escape;
	bool m_rx_overflow;

	u8 m_host_control;
	u8 m_eeprom_data;
	u8 m_coin_last;
	u8 m_coin_count[5];
};

DECLARE_DEVICE_TYPE(MIDWAY_DIEGO, midway_diego_device)

#endif // MAME_WILLIAMS_MIDWAY_DIEGO_H
