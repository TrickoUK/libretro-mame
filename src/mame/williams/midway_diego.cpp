// license:BSD-3-Clause
// copyright-holders:TrickoUK
/**************************************************************************************************

Midway Diego I/O board (5770-15983-04), high level emulation

The board's CY7C63513 microcontroller isn't dumped. This device answers the host side of the
serial protocol as Hydro Thunder's DiegoIO library drives it (38400 baud 8N1 on COM1).

Each frame is 8 bytes, followed by two Fletcher-style check bytes (8-bit sums with end-around
carry; the receiver's sums over all 10 bytes are both 0xff) and SLIP framed (0xc0 delimiters,
0xdb escapes). Byte 0 bit 7 makes the XOR of the 8 frame bytes have even parity.

Host to board:
  0: bit 6 EEPROM request toggle, bit 5 security handshake, bit 4 coin counter request toggle,
     bit 3 random, bit 2 security key load, bits 1-0 security data
  1: bits 1-0 coin counter to pulse, bits 7-2 random
  2: DAC (force feedback)
  3: lamps
  4: LEDs
  5: EEPROM address, bit 7 set for a read
  6: EEPROM write data
  7: random

Board to host:
  0: bit 6 EEPROM acknowledge toggle, bit 5 security handshake, bit 4 coin counter acknowledge
     toggle, bit 2 security key load request, bits 1-0 security data
  1, 2: five 3-bit rolling coin drop counters (0 idle, then 1-7 wrapping back to 1):
     c0 = b2 bits 2-0, c1 = b2 bits 5-3, c2 = b2 bits 7-6 + b1 bit 0, c3 = b1 bits 3-1,
     c4 = b1 bits 6-4
  3: ADC channel 1
  4: ADC channel 0
  5: EEPROM read data
  6: switches, active low
  7: unused

TODO:
- The security channel (the board challenges the CPLD on the Voodoo 2 card through the host) is
  never started, so the board never asks for a key load.
- Verify the analog channel assignment and the DIP switch read (EEPROM address 0) against a board.
- Lamps, LEDs, coin meters and the force feedback DAC are not output.

**************************************************************************************************/

#include "emu.h"
#include "midway_diego.h"

#include <algorithm>
#include <bit>


DEFINE_DEVICE_TYPE(MIDWAY_DIEGO, midway_diego_device, "midway_diego", "Midway Diego I/O board (HLE)")

midway_diego_device::midway_diego_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: buffered_rs232_device<64>(mconfig, MIDWAY_DIEGO, tag, owner, clock)
	, device_nvram_interface(mconfig, *this)
	, m_switches(*this, "SWITCHES")
	, m_analog(*this, "ADC%u", 0U)
	, m_coins(*this, "COINS")
	, m_dsw(*this, "DSW")
{
}

static INPUT_PORTS_START( midway_diego )
	// names as listed by Hydro Thunder's switch test
	PORT_START("SWITCHES")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_BUTTON1 ) PORT_NAME("Boost")
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_BUTTON4 ) PORT_NAME("High View")
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_BUTTON3 ) PORT_NAME("Low View")
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_BUTTON2 ) PORT_NAME("Pilot View")
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_SERVICE1 ) PORT_NAME("Credit")
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_VOLUME_DOWN )
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_VOLUME_UP )
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_SERVICE ) PORT_NAME("Test")

	PORT_START("COINS")
	PORT_BIT( 0x01, IP_ACTIVE_HIGH, IPT_COIN1 )
	PORT_BIT( 0x02, IP_ACTIVE_HIGH, IPT_COIN2 )
	PORT_BIT( 0x04, IP_ACTIVE_HIGH, IPT_COIN3 )
	PORT_BIT( 0x08, IP_ACTIVE_HIGH, IPT_COIN4 )
	PORT_BIT( 0x10, IP_ACTIVE_HIGH, IPT_UNUSED )

	PORT_START("DSW")
	PORT_DIPUNUSED_DIPLOC( 0x01, 0x01, "S1:1" )
	PORT_DIPUNUSED_DIPLOC( 0x02, 0x02, "S1:2" )
	PORT_DIPNAME( 0x04, 0x04, "Test Mode" ) PORT_DIPLOCATION("S1:3")
	PORT_DIPSETTING(    0x04, DEF_STR( Off ) )
	PORT_DIPSETTING(    0x00, DEF_STR( On ) )
	PORT_DIPNAME( 0x08, 0x08, "Cabinet" ) PORT_DIPLOCATION("S1:4")
	PORT_DIPSETTING(    0x08, "25\"" )
	PORT_DIPSETTING(    0x00, "39\"" )
	PORT_DIPUNUSED_DIPLOC( 0x10, 0x10, "S1:5" )
	PORT_DIPUNUSED_DIPLOC( 0x20, 0x20, "S1:6" )
	PORT_DIPUNUSED_DIPLOC( 0x40, 0x40, "S1:7" )
	// the watchdog output isn't connected in this emulation
	PORT_DIPNAME( 0x80, 0x80, "Watchdog" ) PORT_DIPLOCATION("S1:8")
	PORT_DIPSETTING(    0x80, DEF_STR( Off ) )
	PORT_DIPSETTING(    0x00, DEF_STR( On ) )

	// wheel and throttle as named by the analog test (channel order assumed)
	PORT_START("ADC0")
	PORT_BIT( 0xff, 0x80, IPT_PADDLE ) PORT_SENSITIVITY(100) PORT_KEYDELTA(10) PORT_NAME("Wheel")

	PORT_START("ADC1")
	PORT_BIT( 0xff, 0x80, IPT_AD_STICK_Y ) PORT_SENSITIVITY(100) PORT_KEYDELTA(10) PORT_NAME("Throttle") PORT_REVERSE
INPUT_PORTS_END

ioport_constructor midway_diego_device::device_input_ports() const
{
	return INPUT_PORTS_NAME(midway_diego);
}

void midway_diego_device::device_start()
{
	buffered_rs232_device<64>::device_start();

	save_item(NAME(m_eeprom));
	save_item(NAME(m_rx_buffer));
	save_item(NAME(m_rx_count));
	save_item(NAME(m_rx_escape));
	save_item(NAME(m_rx_overflow));
	save_item(NAME(m_host_control));
	save_item(NAME(m_eeprom_data));
	save_item(NAME(m_coin_last));
	save_item(NAME(m_coin_count));
}

void midway_diego_device::device_reset()
{
	buffered_rs232_device<64>::device_reset();

	set_data_frame(1, 8, PARITY_NONE, STOP_BITS_1);
	set_rate(38'400);

	output_rxd(1);
	output_dcd(0);
	output_dsr(0);
	output_cts(0);
	output_ri(1);

	m_rx_count = 0;
	m_rx_escape = false;
	m_rx_overflow = false;
	m_host_control = 0;
	m_eeprom_data = 0xff;
	m_coin_last = 0;
	std::fill(std::begin(m_coin_count), std::end(m_coin_count), 0);
}

void midway_diego_device::nvram_default()
{
	std::fill(std::begin(m_eeprom), std::end(m_eeprom), 0xff);
}

bool midway_diego_device::nvram_read(util::read_stream &file)
{
	auto const [err, actual] = util::read(file, m_eeprom, sizeof(m_eeprom));
	return !err && (actual == sizeof(m_eeprom));
}

bool midway_diego_device::nvram_write(util::write_stream &file)
{
	auto const [err, actual] = util::write(file, m_eeprom, sizeof(m_eeprom));
	return !err;
}

u8 midway_diego_device::add_carry(u8 a, u8 b)
{
	const unsigned sum = a + b;
	return (sum > 0xff) ? ((sum & 0xff) + 1) : sum;
}

void midway_diego_device::received_byte(u8 data)
{
	if (data == 0xc0)
	{
		if ((m_rx_count == FRAME_LENGTH + 2) && !m_rx_overflow)
			frame_received();
		m_rx_count = 0;
		m_rx_escape = false;
		m_rx_overflow = false;
		return;
	}

	if (m_rx_escape)
	{
		m_rx_escape = false;
		if (data == 0xdc)
			data = 0xc0;
		else if (data == 0xdd)
			data = 0xdb;
	}
	else if (data == 0xdb)
	{
		m_rx_escape = true;
		return;
	}

	if (m_rx_count < std::size(m_rx_buffer))
		m_rx_buffer[m_rx_count++] = data;
	else
		m_rx_overflow = true;
}

void midway_diego_device::frame_received()
{
	u8 s1 = 0, s2 = 0;
	for (u8 const b : m_rx_buffer)
	{
		s1 = add_carry(s1, b);
		s2 = add_carry(s2, s1);
	}
	if ((s1 != 0xff) || (s2 != 0xff))
	{
		logerror("checksum error\n");
		return;
	}

	u8 const *const frame = m_rx_buffer;
	u8 const changed = frame[0] ^ m_host_control;

	// a new EEPROM request is signalled by toggling bit 6
	if (BIT(changed, 6))
	{
		u8 const address = frame[5] & 0x7f;
		if (BIT(frame[5], 7))
		{
			// the host reads its DIP switch settings from address 0 (assumed to be answered from S1
			// rather than the EEPROM; the game caches the complement)
			m_eeprom_data = address ? m_eeprom[address] : m_dsw->read();
		}
		else
		{
			m_eeprom[address] = frame[6];
			m_eeprom_data = frame[6];
		}
	}

	m_host_control = frame[0];
	send_frame();
}

void midway_diego_device::send_frame()
{
	// coin drops step the rolling counters
	u8 const coins = m_coins->read();
	u8 const pressed = coins & ~m_coin_last;
	m_coin_last = coins;
	for (int i = 0; i < 5; i++)
	{
		if (BIT(pressed, i))
			m_coin_count[i] = (m_coin_count[i] >= 7) ? 1 : (m_coin_count[i] + 1);
	}

	u8 frame[FRAME_LENGTH + 2];
	// acknowledge the EEPROM and coin counter requests by echoing their toggles
	frame[0] = m_host_control & 0x50;
	frame[1] = BIT(m_coin_count[2], 2) | (m_coin_count[3] << 1) | (m_coin_count[4] << 4);
	frame[2] = m_coin_count[0] | (m_coin_count[1] << 3) | ((m_coin_count[2] & 0x03) << 6);
	frame[3] = m_analog[1]->read();
	frame[4] = m_analog[0]->read();
	frame[5] = m_eeprom_data;
	frame[6] = m_switches->read();
	frame[7] = 0;

	u8 parity = 0;
	for (unsigned i = 0; i < FRAME_LENGTH; i++)
		parity ^= frame[i];
	if (std::popcount(parity) & 1)
		frame[0] |= 0x80;

	u8 s1 = 0, s2 = 0;
	for (unsigned i = 0; i < FRAME_LENGTH; i++)
	{
		s1 = add_carry(s1, frame[i]);
		s2 = add_carry(s2, s1);
	}
	frame[FRAME_LENGTH] = ~add_carry(s1, s2);
	frame[FRAME_LENGTH + 1] = ~add_carry(s1, frame[FRAME_LENGTH]);

	transmit_byte(0xc0);
	for (u8 const b : frame)
	{
		if (b == 0xc0)
		{
			transmit_byte(0xdb);
			transmit_byte(0xdc);
		}
		else if (b == 0xdb)
		{
			transmit_byte(0xdb);
			transmit_byte(0xdd);
		}
		else
		{
			transmit_byte(b);
		}
	}
	transmit_byte(0xc0);
}
