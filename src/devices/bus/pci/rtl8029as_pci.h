// license:BSD-3-Clause
// copyright-holders:

#ifndef MAME_BUS_PCI_RTL8029AS_PCI_H
#define MAME_BUS_PCI_RTL8029AS_PCI_H

#pragma once

#include "pci_slot.h"

#include "machine/dp8390.h"

class rtl8029as_pci_device : public pci_card_device
{
public:
	rtl8029as_pci_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);


protected:
	rtl8029as_pci_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, uint32_t clock);

	virtual void device_start() override ATTR_COLD;
	virtual void device_reset() override ATTR_COLD;
	virtual void device_add_mconfig(machine_config &config) override ATTR_COLD;

//  virtual const tiny_rom_entry *device_rom_region() const override ATTR_COLD;

//  virtual void map_extra(uint64_t memory_window_start, uint64_t memory_window_end, uint64_t memory_offset, address_space *memory_space,
//                         uint64_t io_window_start, uint64_t io_window_end, uint64_t io_offset, address_space *io_space) override;

	virtual void config_map(address_map &map) override ATTR_COLD;

private:
	required_device<dp8390d_device> m_dp8390;

	u8 m_prom[16];
	u8 m_board_ram[32 * 1024];

	void map(address_map &map) ATTR_COLD;

	u8 mem_read(offs_t offset);
	void mem_write(offs_t offset, u8 data);
};

DECLARE_DEVICE_TYPE(RTL8029AS_PCI, rtl8029as_pci_device)

#endif // MAME_NETWORK_RTL8029AS_PCI_H
