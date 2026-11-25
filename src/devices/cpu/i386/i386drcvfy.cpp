// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    i386 family UML recompiler: lockstep verifier

    A debugging aid, selected by setting the environment variable
    MAME_I386_DRC_VERIFY to N, which checks one block in every N against the
    interpreter.  Generated code is built to return after each block.  For a
    checked block the CPU state is saved, the block is run, its memory writes
    are undone, the saved state is restored and the interpreter executes the
    same number of instructions.  CPU state and memory writes are then
    compared, and execution continues from the interpreter's result.

    Only accesses to plain memory can be repeated.  A block that accesses
    anything else, or any I/O port, is left as the recompiled code executed it
    and is counted as not checked.  Memory is recognised in the first 256 MiB
    and the last 1 MiB of the physical address space.  Reads from the system
    BIOS windows (0xe0000-0xfffff and the last 1 MiB) count as repeatable.

***************************************************************************/

#include "emu.h"
#include "i386.h"

#include "softfloat3/source/include/softfloat.h"

#include <cstdlib>


namespace {

constexpr uint32_t VERIFY_LOW_PAGES = 0x10000;      // 4 KiB pages from address zero
constexpr uint32_t VERIFY_HIGH_PAGES = 0x100;       // 4 KiB pages at the top of the address space
constexpr uint32_t VERIFY_HIGH_BASE = 0xfff00000;
constexpr unsigned VERIFY_REPORT_LIMIT = 20;
constexpr uint64_t VERIFY_PROGRESS_INTERVAL = 1'000'000;

} // anonymous namespace


struct i386_device::drc_verifier
{
	struct access
	{
		uint32_t address;
		uint32_t data;
		uint32_t mask;
		uint32_t old_data;
	};

	// a part of the CPU state outside or inside the state block
	struct region
	{
		char const *name;
		void *base;
		size_t length;
		size_t offset;
	};

	uint32_t interval = 1;
	uint64_t blocks = 0;
	uint64_t checked = 0;
	uint64_t unrepeatable = 0;
	uint64_t mismatched = 0;

	bool recording = false;
	bool unsafe = false;
	bool remap = true;
	bool installing = false;

	std::vector<region> regions;
	std::vector<uint8_t> before;
	std::vector<uint8_t> after_block;
	std::vector<uint8_t> after_interpreter;
	std::vector<access> writes;
	std::vector<access> block_writes;

	// per page: bit 0 set if reads come from memory, bit 1 set if writes go to memory
	std::vector<uint8_t> pages;

	memory_passthrough_handler program_tap;
	memory_passthrough_handler io_tap;
	util::notifier_subscription program_notifier;
	util::notifier_subscription io_notifier;

	void add(char const *name, void *base, size_t length)
	{
		size_t const offset = regions.empty() ? 0 : (regions.back().offset + regions.back().length);
		regions.emplace_back(region{ name, base, length, offset });
	}

	void capture(std::vector<uint8_t> &buffer) const
	{
		buffer.resize(regions.back().offset + regions.back().length);
		for (region const &r : regions)
			memcpy(&buffer[r.offset], r.base, r.length);
	}

	void restore(std::vector<uint8_t> const &buffer) const
	{
		for (region const &r : regions)
			memcpy(r.base, &buffer[r.offset], r.length);
	}

	uint8_t page_flags(uint32_t address) const
	{
		if (address < (VERIFY_LOW_PAGES << 12))
			return pages[address >> 12];
		if (address >= VERIFY_HIGH_BASE)
			return pages[VERIFY_LOW_PAGES + ((address - VERIFY_HIGH_BASE) >> 12)];
		return 0;
	}
};


void i386_device::drc_verify_start()
{
	char const *const setting = std::getenv("MAME_I386_DRC_VERIFY");
	if (!setting || (std::atoi(setting) <= 0))
		return;

	if ((m_program_config.data_width() != 32) || (m_io_config.data_width() != 32))
	{
		osd_printf_warning("%s: the recompiler verifier needs 32-bit address spaces\n", tag());
		return;
	}

	m_verifier = std::make_shared<drc_verifier>();
	drc_verifier &v = *m_verifier;
	v.interval = std::atoi(setting);

	// the state block must come first, as the comparison refers to offsets within it
	v.add("state block", m_core, sizeof(core_state));
	v.add("DR", m_dr, sizeof(m_dr));
	v.add("TR", m_tr, sizeof(m_tr));
	v.add("GDTR base", &m_gdtr.base, sizeof(m_gdtr.base));
	v.add("GDTR limit", &m_gdtr.limit, sizeof(m_gdtr.limit));
	v.add("IDTR base", &m_idtr.base, sizeof(m_idtr.base));
	v.add("IDTR limit", &m_idtr.limit, sizeof(m_idtr.limit));
	v.add("TR selector", &m_task.segment, sizeof(m_task.segment));
	v.add("TR flags", &m_task.flags, sizeof(m_task.flags));
	v.add("TR base", &m_task.base, sizeof(m_task.base));
	v.add("TR limit", &m_task.limit, sizeof(m_task.limit));
	v.add("LDTR selector", &m_ldtr.segment, sizeof(m_ldtr.segment));
	v.add("LDTR flags", &m_ldtr.flags, sizeof(m_ldtr.flags));
	v.add("LDTR base", &m_ldtr.base, sizeof(m_ldtr.base));
	v.add("LDTR limit", &m_ldtr.limit, sizeof(m_ldtr.limit));
	v.add("XMM", m_sse_reg, sizeof(m_sse_reg));
	v.add("MXCSR", &m_mxcsr, sizeof(m_mxcsr));
	v.add("TSC", &m_tsc, sizeof(m_tsc));
	v.add("performance counters", m_perfctr, sizeof(m_perfctr));
	v.add("SMBASE", &m_smbase, sizeof(m_smbase));
	v.add("opcode", &m_opcode, sizeof(m_opcode));
	v.add("softfloat rounding", &softfloat_roundingMode, sizeof(softfloat_roundingMode));
	v.add("softfloat exceptions", &softfloat_exceptionFlags, sizeof(softfloat_exceptionFlags));
	v.add("softfloat precision", &extF80_roundingPrecision, sizeof(extF80_roundingPrecision));

	v.pages.resize(VERIFY_LOW_PAGES + VERIFY_HIGH_PAGES);
	v.program_notifier = m_program->add_change_notifier([&v] (read_or_write) { if (!v.installing) v.remap = true; });
	v.io_notifier = m_io->add_change_notifier([&v] (read_or_write) { if (!v.installing) v.remap = true; });

	osd_printf_info("%s: recompiler verifier checking one block in %u\n", tag(), v.interval);
}


// classifies the pages of the program space and installs the taps that record accesses
void i386_device::drc_verify_map()
{
	drc_verifier &v = *m_verifier;
	v.installing = true;

	// pointers cannot be obtained through a tap
	v.program_tap.remove();
	v.io_tap.remove();

	for (uint32_t page = 0; page < v.pages.size(); page++)
	{
		uint32_t const base = (page < VERIFY_LOW_PAGES) ? (page << 12) : (VERIFY_HIGH_BASE + ((page - VERIFY_LOW_PAGES) << 12));
		auto const *const read = reinterpret_cast<uint8_t const *>(m_program->get_read_ptr(base));
		auto const *const write = reinterpret_cast<uint8_t const *>(m_program->get_write_ptr(base));
		uint8_t flags = 0;
		if (read && (m_program->get_read_ptr(base + 0xffc) == (read + 0xffc)))
			flags |= 1;
		if (write && (m_program->get_write_ptr(base + 0xffc) == (write + 0xffc)))
			flags |= 2;
		// the system BIOS is often a flash device behind a read handler, which reads without side effects
		if (((base >= 0x000e0000) && (base < 0x00100000)) || (base >= VERIFY_HIGH_BASE))
			flags |= 1;
		v.pages[page] = flags;
	}

	v.program_tap = m_program->install_readwrite_tap(
			0, 0xffffffff,
			"i386_drc_verify",
			[&v] (offs_t offset, u32 &data, u32 mem_mask)
			{
				if (v.recording && !(v.page_flags(offset) & 1))
					v.unsafe = true;
			},
			[this, &v] (offs_t offset, u32 &data, u32 mem_mask)
			{
				if (!v.recording)
					return;
				if (v.page_flags(offset) != 3)
				{
					v.unsafe = true;
					return;
				}
				v.recording = false;
				uint32_t const old_data = m_program->read_dword(offset & ~3, mem_mask);
				v.recording = true;
				v.writes.emplace_back(drc_verifier::access{ uint32_t(offset & ~3), data, uint32_t(mem_mask), old_data });
			},
			&v.program_tap);
	v.io_tap = m_io->install_readwrite_tap(
			0, 0xffff,
			"i386_drc_verify_io",
			[&v] (offs_t offset, u32 &data, u32 mem_mask) { if (v.recording) v.unsafe = true; },
			[&v] (offs_t offset, u32 &data, u32 mem_mask) { if (v.recording) v.unsafe = true; },
			&v.io_tap);

	v.installing = false;
	v.remap = false;
}


// runs one block from the code cache and checks it against the interpreter
int i386_device::drc_verify_execute()
{
	drc_verifier &v = *m_verifier;
	if (++v.blocks % v.interval)
		return m_drcuml->execute(*m_entry);

	if (v.remap)
		drc_verify_map();

	uint32_t const pc = m_core->pc;
	m_core->verify_count = 0;
	v.capture(v.before);
	v.writes.clear();
	v.unsafe = false;
	v.recording = true;

	int const result = m_drcuml->execute(*m_entry);

	v.recording = false;
	uint32_t const count = m_core->verify_count;
	if (!count)
		return result;
	if (v.unsafe)
	{
		v.unrepeatable++;
		return result;
	}

	// undo the block's writes, restore the state and run the same instructions in the interpreter
	v.capture(v.after_block);
	v.block_writes.swap(v.writes);
	for (auto it = v.block_writes.rbegin(); it != v.block_writes.rend(); ++it)
		m_program->write_dword(it->address, it->old_data, it->mask);
	v.restore(v.before);

	v.writes.clear();
	v.recording = true;
	for (uint32_t i = 0; i < count; i++)
		step();
	v.recording = false;
	v.capture(v.after_interpreter);
	v.checked++;

	// fields that only carry information between generated code and the functions it calls
	auto const clear_scratch =
			[] (std::vector<uint8_t> &buffer)
			{
				auto const clear = [&buffer] (size_t offset, size_t length) { std::fill_n(&buffer[offset], length, 0); };
				clear(offsetof(core_state, fault), sizeof(core_state::fault));
				clear(offsetof(core_state, fault_code), sizeof(core_state::fault_code));
				clear(offsetof(core_state, mode), sizeof(core_state::mode));
				clear(offsetof(core_state, ea_tmp), sizeof(core_state::ea_tmp));
				clear(offsetof(core_state, arg0), sizeof(core_state::arg0));
				clear(offsetof(core_state, arg1), sizeof(core_state::arg1));
				clear(offsetof(core_state, drc_exit), sizeof(core_state::drc_exit));
				clear(offsetof(core_state, verify_count), sizeof(core_state::verify_count));
			};
	clear_scratch(v.after_block);
	clear_scratch(v.after_interpreter);

	bool const report = v.mismatched < VERIFY_REPORT_LIMIT;
	bool failed = false;

	if (v.unsafe)
	{
		failed = true;
		if (report)
			osd_printf_error("%s: DRC verify at %08X: the interpreter accessed something the recompiled code did not\n", tag(), pc);
	}

	for (auto const &r : v.regions)
	{
		for (size_t i = 0; i < r.length; i++)
		{
			uint8_t const a = v.after_block[r.offset + i];
			uint8_t const b = v.after_interpreter[r.offset + i];
			if (a != b)
			{
				failed = true;
				if (report)
					osd_printf_error("%s: DRC verify at %08X: %s offset %x is %02X, interpreter %02X\n", tag(), pc, r.name, i, a, b);
				break;
			}
		}
	}

	bool const same_writes = std::equal(
			v.block_writes.begin(), v.block_writes.end(),
			v.writes.begin(), v.writes.end(),
			[] (drc_verifier::access const &a, drc_verifier::access const &b)
			{
				return (a.address == b.address) && (a.mask == b.mask) && !((a.data ^ b.data) & a.mask);
			});
	if (!same_writes)
	{
		failed = true;
		if (report)
			osd_printf_error("%s: DRC verify at %08X: memory writes differ (%u, interpreter %u)\n", tag(), pc, v.block_writes.size(), v.writes.size());
	}

	if (failed)
		v.mismatched++;
	if (!(v.checked % VERIFY_PROGRESS_INTERVAL))
		drc_verify_report();

	return result;
}


void i386_device::drc_verify_report()
{
	drc_verifier const &v = *m_verifier;
	osd_printf_info(
			"%s: DRC verify: %u blocks checked, %u not repeatable, %u mismatched (of %u run)\n",
			tag(), v.checked, v.unrepeatable, v.mismatched, v.blocks);
}
