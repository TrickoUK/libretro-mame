// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************************************

Realtek RTL8029AS based Network cards

List of OEM cards:
- D-Link SN-3200 (ibm5170:s3200pci)
- ...

TODO:
- Understand how it manages "config pages" (-> 4 views);
- Bridge with NE2000;
- 93C46 EEPROM;
- Optional ROMs for RTL8029AS extended modes;

***************************************************************************************************/

#include "emu.h"
#include "rtl8029as_pci.h"

#include "multibyte.h"

#define LOG_WARN      (1U << 1)

#define VERBOSE (LOG_GENERAL | LOG_WARN)
//#define LOG_OUTPUT_FUNC osd_printf_info
#include "logmacro.h"

#define LOGWARN(...)            LOGMASKED(LOG_WARN, __VA_ARGS__)


DEFINE_DEVICE_TYPE(RTL8029AS_PCI, rtl8029as_pci_device,   "rtl8029as_pci",   "Realtek RTL8029AS Full-Duplex Ethernet")



rtl8029as_pci_device::rtl8029as_pci_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, uint32_t clock)
	: pci_card_device(mconfig, type, tag, owner, clock)
	, m_dp8390(*this, "dp8390d")
{
	set_ids(0x10ec8029, 0x00, 0x020000, 0x10ec8029);
}

rtl8029as_pci_device::rtl8029as_pci_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: rtl8029as_pci_device(mconfig, RTL8029AS_PCI, tag, owner, clock)
{
}

void rtl8029as_pci_device::device_add_mconfig(machine_config &config)
{
	DP8390D(config, m_dp8390);
	m_dp8390->irq_callback().set([this] (int state) { irq_pin_w(0, state); });
	m_dp8390->mem_read_callback().set(FUNC(rtl8029as_pci_device::mem_read));
	m_dp8390->mem_write_callback().set(FUNC(rtl8029as_pci_device::mem_write));
}

// NE2000 compatible I/O window
void rtl8029as_pci_device::map(address_map &map)
{
	map(0x00, 0x0f).lrw8(
		NAME([this] (offs_t offset) { return m_dp8390->cs_read(offset); }),
		NAME([this] (offs_t offset, u8 data) { m_dp8390->cs_write(offset, data); }));
	// remote DMA data port
	map(0x10, 0x11).lrw16(
		NAME([this] () { return m_dp8390->remote_read(); }),
		NAME([this] (u16 data) { m_dp8390->remote_write(data); }));
	// reading the reset port resets the controller, writing completes the reset
	map(0x18, 0x1f).lrw8(
		NAME([this] () { if (!machine().side_effects_disabled()) m_dp8390->dp8390_reset(ASSERT_LINE); return 0; }),
		NAME([this] (u8 data) { m_dp8390->dp8390_reset(CLEAR_LINE); }));
}

// buffer RAM at $4000-$7fff in byte mode, $4000-$bfff (16K x 16) in word mode, station address PROM below
u8 rtl8029as_pci_device::mem_read(offs_t offset)
{
	offset &= 0xffff;
	if (offset < 0x20)
		return m_prom[offset >> 1];
	if ((offset < 0x4000) || (offset >= 0xc000))
		return 0xff;
	return m_board_ram[offset - 0x4000];
}

void rtl8029as_pci_device::mem_write(offs_t offset, u8 data)
{
	offset &= 0xffff;
	if ((offset >= 0x4000) && (offset < 0xc000))
		m_board_ram[offset - 0x4000] = data;
}

void rtl8029as_pci_device::device_start()
{
	pci_card_device::device_start();

	// TODO: verify 16 being of the right size
	// documentation puts 3 bits in BAR0 as size but then all the "pages" are 16,
	// then one note also claims 32 bytes wtf
	add_map(32, M_IO, FUNC(rtl8029as_pci_device::map));

	u8 mac[6] = { 0x00, 0x00, 0xe8, 0x00, 0x00, 0x00 };
	put_u24be(mac + 3, machine().rand());
	m_dp8390->set_mac(mac);
	std::fill(std::begin(m_prom), std::end(m_prom), 0x57);
	std::copy_n(mac, 6, m_prom);

	save_item(NAME(m_prom));
	save_item(NAME(m_board_ram));

	// INTA#
	intr_pin = 1;
}

void rtl8029as_pci_device::device_reset()
{
	pci_card_device::device_reset();

	// Not indicated, assume zeroed
	command = 0x0000;
	// medium DEVSELB
	status = 0x0200;

	remap_cb();
}

void rtl8029as_pci_device::config_map(address_map &map)
{
	pci_card_device::config_map(map);
}
