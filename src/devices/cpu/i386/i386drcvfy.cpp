// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    i386 family UML recompiler: lockstep verifier

    A debugging aid, selected by setting the environment variable
    MAME_I386_DRC_VERIFY to N, which checks one block in every N against the
    interpreter.  Generated code is built to return after each block, and to
    call the verifier before each instruction.  For a checked block the CPU
    state is saved, the block is run, its memory writes are undone, the saved
    state is restored and the interpreter executes the same number of
    instructions.  CPU state and memory writes are then compared, and
    execution continues from the interpreter's result.

    Only accesses to plain memory can be repeated.  A block that accesses
    anything else, or any I/O port, is left as the recompiled code executed it
    and is counted as not checked.  Memory is recognised in the first 256 MiB
    and the last 1 MiB of the physical address space.  Reads from the system
    BIOS windows (0xe0000-0xfffff and the last 1 MiB) count as repeatable.
    The contents of a cache modelled by a subclass are not covered.

    The replay also checks that the block did not execute an instruction
    which the interpreter loop would not have started, or which has to be
    left to the interpreter, or after the code cache had to be flushed.
    In every block, checked or not, each instruction must be started at the
    address it was compiled at, and the cached mode key must agree with the
    segment registers and CR0.

    To show that errors are detected, MAME_I386_DRC_VERIFY_BREAK selects a
    deliberate error made by the generated code of every block:
        cf       inverts CF in checked blocks
        cycles   takes one more cycle in checked blocks
        eip      adds one to EIP in checked blocks
        write    writes the dword below the stack pointer back to itself
                 in checked blocks
        overrun  executes a second instruction after the first one of checked
                 blocks, whatever the cycles left and the state.  The block
                 is left if that does not end at the next compiled address,
                 so this is only reported when the extra instruction lands
                 at the end of a timeslice or in an ineligible state.
        address  leaves out the test of the address between the instructions
                 of a block
        mode     changes the cached mode key in checked blocks
        throw    throws a C++ exception in the first block run

***************************************************************************/

#include "emu.h"
#include "i386.h"
#include "i386priv.h"

#include "softfloat3/source/include/softfloat.h"

#include <cstdlib>
#include <string_view>


namespace {

constexpr uint32_t VERIFY_LOW_PAGES = 0x10000;      // 4 KiB pages from address zero
constexpr uint32_t VERIFY_HIGH_PAGES = 0x100;       // 4 KiB pages at the top of the address space
constexpr uint32_t VERIFY_HIGH_BASE = 0xfff00000;
constexpr unsigned VERIFY_REPORT_LIMIT = 20;
constexpr uint64_t VERIFY_PROGRESS_INTERVAL = 1'000'000;

// deliberate errors
enum : int
{
	BREAK_NONE,
	BREAK_CF,
	BREAK_CYCLES,
	BREAK_EIP,
	BREAK_WRITE,
	BREAK_OVERRUN,
	BREAK_ADDRESS,
	BREAK_MODE,
	BREAK_THROW
};

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
	int sabotage = BREAK_NONE;

	// violations seen while generated code was running
	uint32_t misplaced = 0;
	uint32_t misplaced_pc = 0;
	uint32_t misplaced_expected = 0;
	uint32_t stale_modes = 0;

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

	// per page: bit 0 set if reads come from memory, bit 1 set if writes go to memory, bit 2 set if the page has
	// a pointer for reads
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
	drc_verify_add_state([&v] (char const *name, void *base, size_t length) { v.add(name, base, length); });

	v.pages.resize(VERIFY_LOW_PAGES + VERIFY_HIGH_PAGES);
	v.program_notifier = m_program->add_change_notifier([&v] (read_or_write) { if (!v.installing) v.remap = true; });
	v.io_notifier = m_io->add_change_notifier([&v] (read_or_write) { if (!v.installing) v.remap = true; });

	osd_printf_info("%s: recompiler verifier checking one block in %u\n", tag(), v.interval);

	char const *const breakage = std::getenv("MAME_I386_DRC_VERIFY_BREAK");
	if (breakage)
	{
		static constexpr std::pair<std::string_view, int> NAMES[] = {
				{ "cf", BREAK_CF },
				{ "cycles", BREAK_CYCLES },
				{ "eip", BREAK_EIP },
				{ "write", BREAK_WRITE },
				{ "overrun", BREAK_OVERRUN },
				{ "address", BREAK_ADDRESS },
				{ "mode", BREAK_MODE },
				{ "throw", BREAK_THROW } };
		for (auto const &[name, kind] : NAMES)
		{
			if (name == breakage)
				v.sabotage = kind;
		}
		if (v.sabotage == BREAK_NONE)
			fatalerror("%s: unknown MAME_I386_DRC_VERIFY_BREAK value %s\n", tag(), breakage);
		m_drc_verify_break = true;
		osd_printf_warning("%s: recompiled code is deliberately wrong (%s)\n", tag(), breakage);
	}
}


// disturbs the state as a faulty compiled instruction would
void i386_device::drc_verify_break()
{
	drc_verifier const &v = *m_verifier;
	core_state &core = *m_core;

	if (v.sabotage == BREAK_THROW)
		throw emu_fatalerror("%s: DRC verify: test exception thrown below generated code\n", tag());

	// an error in a block that is not compared would be left in the guest
	if (!v.recording || v.unsafe)
		return;

	switch (v.sabotage)
	{
	case BREAK_CF:
		core.CF ^= 1;
		break;

	case BREAK_CYCLES:
		core.cycles -= 1;
		break;

	case BREAK_EIP:
		core.eip += 1;
		break;

	case BREAK_WRITE:
		{
			uint32_t const address = (core.reg.d[ESP] - 4) & ~uint32_t(3);
			m_program->write_dword(address, m_program->read_dword(address));
		}
		break;

	case BREAK_OVERRUN:
		if (core.verify_count == 1)
		{
			step();
			core.verify_count++;
		}
		break;

	case BREAK_MODE:
		core.mode ^= 0x100;
		break;
	}
}


bool i386_device::drc_verify_ignores_address() const
{
	return m_verifier && (m_verifier->sabotage == BREAK_ADDRESS);
}


// called before each instruction of a block
void i386_device::drc_verify_instruction()
{
	drc_verifier &v = *m_verifier;
	core_state &core = *m_core;

	core.verify_count++;
	if (core.pc != core.verify_pc)
	{
		v.misplaced++;
		v.misplaced_pc = core.pc;
		v.misplaced_expected = core.verify_pc;
	}
	if (!core.mode_dirty && (core.mode != drc_mode()))
		v.stale_modes++;
}


// reads code from a page that is memory while the taps hide its pointer
bool i386_device::drc_verify_read_code(offs_t address, uint8_t &value)
{
	drc_verifier &v = *m_verifier;
	if (v.remap)
		drc_verify_map();
	if (!(v.page_flags(address) & 4))
		return false;
	value = m_program->read_byte(address);
	return true;
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
			flags |= 1 | 4;
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
				if ((v.page_flags(offset) & 3) != 3)
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
	drc_rethrow_exception();
	uint32_t const count = m_core->verify_count;

	// the cached mode key, as the block left it
	if (!m_core->mode_dirty && (m_core->mode != drc_mode()))
		v.stale_modes++;
	if (v.misplaced || v.stale_modes)
	{
		if (v.mismatched < VERIFY_REPORT_LIMIT)
		{
			if (v.misplaced)
				osd_printf_error("%s: DRC verify at %08X: an instruction compiled at %08X was started at %08X\n", tag(), pc, v.misplaced_expected, v.misplaced_pc);
			if (v.stale_modes)
				osd_printf_error("%s: DRC verify at %08X: the cached mode key is out of date\n", tag(), pc);
		}
		v.mismatched++;
		v.misplaced = 0;
		v.stale_modes = 0;
	}
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
	char const *overrun = nullptr;

	// a block must also be left once the code cache has to be flushed
	bool const cache_dirty = m_drc_cache_dirty;
	m_drc_cache_dirty = false;
	for (uint32_t i = 0; (i < count) && !overrun; i++)
	{
		// the first instruction was tested before the block was entered
		if (i && (m_core->cycles <= 0))
			overrun = "block ran past the timeslice";
		else if (i && (m_drc_cache_dirty || !drc_eligible(drc_mode())))
			overrun = "block continued where it must exit";
		else
			step();
	}
	v.recording = false;
	m_drc_cache_dirty = m_drc_cache_dirty || cache_dirty;
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
				clear(offsetof(core_state, verify_pc), sizeof(core_state::verify_pc));
				clear(offsetof(core_state, mode_dirty), sizeof(core_state::mode_dirty));
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

	if (overrun)
	{
		failed = true;
		if (report)
			osd_printf_error("%s: DRC verify at %08X: %s\n", tag(), pc, overrun);
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
			"%s: DRC verify: %u blocks checked, %u not repeatable (%.5f per checked block), %u mismatched (of %u run)\n",
			tag(), v.checked, v.unrepeatable, v.checked ? (double(v.unrepeatable) / double(v.checked)) : 0.0, v.mismatched, v.blocks);
}
