// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    i386 family UML recompiler

    Code is run from the code cache only in 32-bit protected mode with flat
    CS, DS, ES and SS segments (base zero, covering everything above at most
    the first page) and paging disabled.  Everything else, and any
    instruction that needs one of the checks the CPU makes between
    instructions (pending interrupt, SMI, single step, debug breakpoint,
    interrupt shadow, LOCK prefix, resume flag, halt), is executed by the
    interpreter loop.

    A block holds a straight sequence of instructions described by the front
    end.  MOV of 8 and 32 bits between registers, memory and immediates is
    compiled, and so are LEA, the 8 and 32-bit forms of ADD, ADC, SUB, SBB,
    CMP, AND, OR, XOR, TEST, INC, DEC, NEG and NOT with register, memory and
    immediate operands, MOVZX and MOVSX, XCHG of a register with memory, the
    conditional jumps with 8 and 32-bit displacements, the near forms of JMP,
    CALL and RET, LOOP, LOOPE, LOOPNE, JECXZ, PUSH of a register, memory or
    immediate, POP to a register or memory, and LEAVE, all without prefixes.
    Every other instruction is executed with step(), the interpreter's
    per-instruction function, which fetches the instruction from memory as it
    then is.  The block key is the linear address of the first instruction
    together with the mode bits.

    Rules the blocks follow:
    - An instruction is started only while cycles are left, so a timeslice
      ends after the same instruction as in the interpreter.  The remaining
      instructions of a timeslice are executed by the interpreter loop.
    - After every call that can run device code, an interrupt, a change to
      the mode or a need to flush the code cache ends generated code at the
      instruction boundary.
    - A block goes on to its next instruction only if execution has arrived at
      the address it was compiled at.  This ends the block at taken branches,
      and also when code executed by step() has been rewritten with a different
      length since it was described.
    - A sequence is cut short if the longest instruction might not fit in what
      is left of the UML instruction buffer.
    - No C++ exception propagates through generated code, which has no unwind
      information.  A called function runs its body through drc_guarded_call(),
      and generated code calls it through generate_call().  A guest fault
      that generated code is to deliver is caught by the function itself and
      recorded for the fault handler.  Any other exception is kept, generated
      code is left at once, and it is rethrown from the execution loop.

    Going from block to block:
    - A block ends with HASHJMP to the following block, with the target built
      in for a direct jump, call or loop and taken from the instruction
      pointer otherwise.  HASHJMP looks the target up in the block table when
      it is executed, so removing a block from the table is all that is needed
      to stop every other block reaching it.
    - The first instruction of a block tests for remaining cycles like any
      other.  This is the only test on the way round a loop which spans blocks,
      including a loop of one block.
    - Execution stays in generated code across blocks, so everything that the
      interpreter loop acts on before an instruction has to force an exit.
      Each of these changes only in a call made from generated code, after
      which the exit request is tested, or is covered by the cycle test:
      - The interrupt request, SMI, A20 and halt are changed by
        execute_set_input().  set_input_line() reaches it through a scheduler
        timer.  Setting that timer in a called function takes the remaining
        cycles away, so the cycle test before the next instruction ends
        generated code, and the timer expires before the next timeslice.
      - IF, TF, RF, the interrupt shadow of STI and of loading SS, halt, the
        LOCK flag, the debug registers, CR0 and the segment registers are
        written by instructions executed with step() and by the delivery of a
        fault.  drc_exit_request() tests them all, or the mode key.
      - A change to the memory map and a write to compiled code happen in a
        called function, which finds them in drc_exit_request().
      - The debugger is enabled for the whole of a run.
      This assumes that timers and devices other than those reached by a called
      function run only between timeslices.

    Flags:
    - A compiled instruction stores each arithmetic flag byte which its
      interpreter handler changes, with the same value, so the flags are
      current wherever generated code is left or calls a function.  The lazy
      flag record is not used.
    - The values follow the handlers: logic operations clear CF and OF and
      leave AF alone, except TEST with an immediate in the F6 and F7 groups,
      which also clears AF; INC and DEC leave CF alone.
    - 32-bit arithmetic takes CF, OF, ZF and SF from the host operation, and
      32-bit logic takes ZF and SF from a compare of the result.  PF, AF and
      all flags of 8-bit operations are computed from the operands and the
      result.
    - A compiled conditional jump tests the flag bytes as its interpreter
      handler does.  A taken jump ends the block, and execution goes on to the
      next instruction of the block otherwise.  Each of the two ways charges
      the cycles of the handler.

    Compiled control transfers and stack operations:
    - The code segment has base zero, so the instruction pointer is the linear
      address.  No target is checked against the segment limit, as in the
      interpreter.
    - LOOP, LOOPE and LOOPNE decrement ECX whether or not they jump, and change
      no flags.
    - The mode key requires a 32-bit stack segment with base zero, so the stack
      address is ESP, which wraps at 32 bits.  Generated stack code depends on
      this.
    - A push writes below ESP and then sets ESP.  PUSH ESP and a call through
      ESP use its value from before the push.  A pop reads, sets ESP and then
      writes its destination, so POP ESP leaves the value read, and the address
      of a POP to memory is computed with the new ESP.  A RET with an
      immediate adds it, sign extended, after the pop.
    - All the memory an instruction needs is looked up in the page tables
      before anything is changed.  If any of it cannot be accessed directly,
      including a stack in a page which holds compiled code, the whole
      instruction is executed with step(), which leaves ESP and EIP as the
      interpreter would on a fault.  These instructions are not compiled while
      the debugger is enabled.

    Compiled memory accesses:
    - The segment of an operand has base zero, so the effective address is the
      linear address.  Only DS and SS operands without prefixes are compiled.
    - An access straight to memory cannot fault only because of this: the
      segments are flat (see drc_mode()), page zero is never entered and an
      access does not cross a page.  Compiling code with paging or with other
      segments would require the access checks of GetEA(), which take whether
      the access writes and its size, to be made in generated code.
    - Two tables indexed by linear page, one for reads and one for writes,
      give the host memory of a page.  An access goes straight to that memory
      if it lies within one page that has an entry.  Every other access of a
      MOV calls a function which performs it as the interpreter does, with the
      segment checks, so that faults, devices, accesses crossing pages and
      write taps behave as there.  The function then enters the page in the
      table if all of it is plain memory.
    - Any other instruction with a memory operand which cannot be accessed
      directly is executed as a whole with step(), after the pages of the
      address have been entered where they can be.  The order of its accesses,
      the access checks of its handler, faults and device effects are then the
      interpreter's.  The debugger would be told of such an instruction twice,
      so it is not compiled while the debugger is enabled.
    - An instruction which writes its memory operand goes straight to memory
      only if the read and write entries of the page are the same, so that the
      value read and the value written are in the same host memory.  XCHG is
      locked on hardware when it has a memory operand; this is not modelled,
      as in the interpreter.  An instruction with a LOCK prefix is stepped.
    - A memory operand is read, the flags are stored, then memory is written,
      and a register destination of XCHG is written last.
    - Page zero never has an entry, as the expand-down segments exclude
      offsets in it.
    - An entry reflects the A20 mask and the memory map, and the tables are
      emptied whenever the code cache is flushed.  A page that holds bytes of
      compiled instructions has no entry for writes: it is removed when the
      page starts being watched.
    - Nothing is entered while the debugger is enabled, or on a host that is
      not little-endian.  Under the lockstep verifier, a store of a checked
      block that goes straight to memory is also examined by it.

    A compiled instruction has its bytes built into the code, so it must be
    compiled again once they change.  x86 code is not flushed explicitly: any
    write to memory can change instructions.
    - Only instructions whose bytes were all read from memory are compiled.
    - A write tap is installed on each 4 KiB page holding bytes of compiled
      instructions.  The tap sees the writes of every bus master that goes
      through the program space: the CPU, whether the interpreter or step()
      executes the store, DMA controllers and the debugger.  Without the
      recompiler no taps are installed.
    - The tap tests whether any byte written is one built into code; the values
      are not compared.  When such a byte is written, the blocks that can
      contain it are removed from the block lookup, and generated code is left
      after the instruction that wrote it.  Data kept in the same page as code
      costs only the test.
    - A change to the memory map, which includes selecting a memory view, can
      change the bytes at an address without a write, and drops taps, so it
      flushes the code cache, as do reset, a loaded state and a change of the
      A20 mask.
    - Changes that bypass the above are not seen: writes through a host pointer
      to the memory or through a mirror of the address, and switching the entry
      of a memory bank (memory_bank::set_entry), which would also leave the
      page tables pointing at the old entry.  TODO: handle these before
      enabling the recompiler for a system that does any of them.

***************************************************************************/

#include "emu.h"
#include "i386.h"
#include "i386priv.h"
#include "i386drcfe.h"

#include "cpu/drcumlsh.h"

#include <array>
#include <cstdlib>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

using namespace uml;


namespace {

// size of the code cache; the two-level block lookup table takes 512 KiB for each 64 KiB of linear address space holding code
constexpr size_t DRC_CACHE_SIZE = 128 * 1024 * 1024;

// the front end describes instructions starting within this many bytes of the first, and at most this many in a block
constexpr uint32_t COMPILE_WINDOW_BYTES = 1024;
constexpr uint32_t COMPILE_MAX_SEQUENCE = 64;

// longest range of guest bytes a block may cover, used when invalidating blocks that begin before a written range
constexpr uint32_t COMPILE_MAX_SEQUENCE_BYTES = COMPILE_WINDOW_BYTES + 15;

// UML instructions kept free for one instruction of a block (about 100 are generated for the longest, a read-modify-write
// of memory with the verifier and a deliberate error) and for the end of the block
constexpr uint32_t COMPILE_UML_PER_INSTRUCTION = 160;
constexpr uint32_t COMPILE_UML_PER_BLOCK = 32;

// size of the UML instruction buffer; a sequence which would not fit is ended early
constexpr uint32_t COMPILE_MAX_UML = 8192;

// pages of the data tables, and the pages from address zero searched for the largest range of memory
constexpr uint32_t DATA_PAGES = 0x100000;
constexpr uint32_t DATA_ANCHOR_PAGES = 0x10000;

// bits of the mode key
constexpr uint32_t MODE_CODE32 = 1 << 0;
constexpr uint32_t MODE_PROTECTED = 1 << 1;
constexpr uint32_t MODE_FLAT = 1 << 2;
constexpr uint32_t MODE_COUNT = 8;
constexpr uint32_t MODE_COMPILED = MODE_CODE32 | MODE_PROTECTED | MODE_FLAT;

// value of the pending fault field when no fault is pending
constexpr uint32_t NO_FAULT = 0xffffffff;

// values of the exit request that a called function leaves for generated code
constexpr uint32_t DRC_EXIT_NONE = 0;
constexpr uint32_t DRC_EXIT_INTERPRET = 1;
constexpr uint32_t DRC_EXIT_EXCEPTION = 2;
constexpr uint32_t DRC_EXIT_FAULT = 3;

// reasons for leaving generated code
constexpr int EXECUTE_OUT_OF_CYCLES = 0;
constexpr int EXECUTE_MISSING_CODE = 1;
constexpr int EXECUTE_RESET_CACHE = 2;
constexpr int EXECUTE_INTERPRET = 3;
constexpr int EXECUTE_BLOCK_DONE = 4;
constexpr int EXECUTE_EXCEPTION = 5;


inline void alloc_handle(drcuml_state &drcuml, uml::code_handle *&handleptr, const char *name)
{
	if (!handleptr)
		handleptr = drcuml.handle_alloc(name);
}

} // anonymous namespace


// the operation of a compiled arithmetic or logic instruction
struct i386_device::drc_alu_form
{
	enum operation : uint8_t { ADD, ADC, SUB, SBB, AND, OR, XOR, NOT, INC, DEC };

	// an operand is a register, as a byte or dword index, or one of these
	static constexpr int IMMEDIATE = -1;
	static constexpr int MEMORY = -2;

	operation op;
	bool bytes;         // operands of 8 rather than 32 bits
	bool write;         // the result is written to the destination
	bool clear_af;      // a logic operation which clears AF rather than leaving it
	int dest;           // register receiving the result, or MEMORY
	int first;          // first operand; the first immediate if IMMEDIATE
	int second;         // second operand; the second immediate if IMMEDIATE
	uint32_t imm_first;
	uint32_t imm_second;
	X86_CYCLES cycles;
	X86_CYCLES register_cycles; // cycles of the form with a register in place of the memory operand

	bool memory() const { return (dest == MEMORY) || (first == MEMORY) || (second == MEMORY); }
	bool sets_flags() const { return op != NOT; }
};


// the pages holding bytes of compiled instructions, and counts of what happens to them
struct i386_device::drc_code_watch
{
	struct page
	{
		std::array<uint32_t, 0x400> built{ };   // byte lanes of each dword that are built into compiled code
		uint16_t armed_regions = 0;             // 256-byte regions compiled from since they were last written
		bool armed = false;                     // compiled from since last written
	};

	std::unordered_map<uint32_t, std::unique_ptr<page> > pages;     // by physical page number
	memory_passthrough_handler tap;

	// Statistics.  The page and region counts are the invalidations that tracking whole 4 KiB pages or 256-byte regions
	// would have needed, for comparison with the exact byte tracking.
	uint64_t blocks = 0;
	uint64_t native = 0;
	uint64_t flushes = 0;
	uint64_t map_changes = 0;
	uint64_t page_installs = 0;
	uint64_t writes = 0;
	uint64_t page_hits = 0;
	uint64_t region_hits = 0;
	uint64_t invalidations = 0;
	int64_t report_interval = 0;
	int64_t next_report = 0;
};


// The host memory of linear pages, for compiled accesses.  A non-zero entry is one more than the difference between
// the host address of a byte and the anchor, less the byte's linear address, truncated to 32 bits.  Generated code
// adds the linear address and uses the sum as a signed index from one byte below the anchor.  Memory more than 2 GiB
// away from the anchor cannot be reached this way and has no entry.
struct i386_device::drc_data_pages
{
	std::unique_ptr<uint32_t []> read;
	std::unique_ptr<uint32_t []> write;
	uint8_t *anchor = nullptr;                  // start of the largest range of memory, built into generated code
	std::vector<uint32_t> filled;               // twice the page number, plus one for the write table
	std::unordered_set<uint32_t> refused;       // pages that cannot be entered, in the same form

	// statistics
	uint64_t slow_reads = 0;
	uint64_t slow_writes = 0;
	uint64_t fills = 0;
	uint64_t unreachable = 0;
};



/***************************************************************************
    SETUP
***************************************************************************/

// creates the recompiler and places the CPU state in the part of the code cache that generated code can address directly
void i386_device::drc_start()
{
	// the near area of the code cache has a fixed size of 128 KiB, shared with the recompiler's own near data
	static_assert(sizeof(core_state) <= 4 * 1024);

	m_drcuml = std::make_unique<drcuml_state>(*this, DRC_CACHE_SIZE);
	m_drcuml->initialize(0, MODE_COUNT, 32, 0, COMPILE_MAX_SEQUENCE_BYTES);

	m_core = m_drcuml->cache_alloc_near<core_state>();
	if (!m_core)
		fatalerror("%s: cannot allocate the CPU state in the recompiler cache\n", tag());

	m_drcuml->symbol_add(&m_core->pc, sizeof(m_core->pc), "pc");
	m_drcuml->symbol_add(&m_core->cycles, sizeof(m_core->cycles), "cycles");
	m_drcuml->symbol_add(&m_core->mode, sizeof(m_core->mode), "mode");

	m_drcfe = std::make_shared<frontend>(*this, 0, COMPILE_WINDOW_BYTES, COMPILE_MAX_SEQUENCE);
	m_drcfe->set_check(std::getenv("MAME_I386_DRC_FE_CHECK") != nullptr);
}


void i386_device::drc_flush_cache()
{
	m_drcuml->reset();
	drc_code_clear();
	drc_pages_flush();
	drc_pages_choose_anchor();
	m_drc_code->flushes++;

	try
	{
		drcuml_block &block(m_drcuml->begin_block(64));

		static_generate_entry_point(block);
		static_generate_nocode_handler(block);
		static_generate_out_of_cycles(block);
		static_generate_fault_handler(block);

		block.end();
	}
	catch (drcuml_block::abort_compilation const &)
	{
		fatalerror("%s: unrecoverable error generating static code\n", tag());
	}

	m_drc_cache_dirty = false;
}



/***************************************************************************
    STATE TESTS
***************************************************************************/

uint32_t i386_device::drc_mode() const
{
	core_state const &core = *m_core;
	uint32_t mode = core.sreg[CS].d ? MODE_CODE32 : 0;
	if (!(core.cr[0] & CR0_PE))
		return mode;
	mode |= MODE_PROTECTED;

	// A code segment is flat if it covers the whole address space.  A data or stack segment may also be expand-down
	// with the 32-bit upper bound and a limit within the first page, as used to trap null pointer accesses; only
	// offsets in that page are outside it.
	I386_SREG const &cs = core.sreg[CS];
	bool const flat_code = cs.valid && !cs.base && (cs.limit == 0xffffffff) && BIT(cs.flags, 3);
	auto const flat_data =
			[&core] (int segment)
			{
				I386_SREG const &sreg = core.sreg[segment];
				if (!sreg.valid || sreg.base)
					return false;
				switch (sreg.flags & 0x000e)
				{
				case 0x0002: // writable, expand-up data
					return sreg.limit == 0xffffffff;
				case 0x0006: // writable, expand-down data
					return (sreg.limit <= 0xfff) && sreg.d;
				default:
					return false;
				}
			};

	// NOTE: compiled memory accesses make no segment check.  They cannot fault only because these segments are flat,
	// page zero is never entered in the page tables and an access does not cross a page.  Generated stack code also
	// depends on SS being 32-bit, with ESP wrapping at 32 bits.
	// a 16-bit stack segment uses SP rather than ESP, so it is not flat
	if (flat_code && flat_data(DS) && flat_data(ES) && flat_data(SS) && core.sreg[SS].d)
		mode |= MODE_FLAT;

	return mode;
}


// True if the next instruction can be run from the code cache in the given mode.  The conditions are those that the
// CPU tests between instructions, paging and virtual 8086 mode, all combined into one value.
bool i386_device::drc_eligible(uint32_t mode) const
{
	core_state const &core = *m_core;
	return !((mode ^ MODE_COMPILED)
			| (core.cr[0] & CR0_PG)
			| core.VM
			| core.TF
			| core.RF
			| (m_dr[7] & 0xff)
			| uint32_t(core.smi)
			| uint32_t(core.halted)
			| core.delayed_interrupt_enable
			| uint32_t(core.lock)
			| uint32_t(core.irq_state ? core.IF : 0));
}


// brings the mode key up to date and tests whether the next instruction can be run from the code cache
bool i386_device::drc_runnable()
{
	core_state &core = *m_core;

	// the mode only changes with the segment registers and CR0, whose writers mark it, except for edits made
	// through the debugger
	if (core.mode_dirty || debugger_enabled())
	{
		core.mode = drc_mode();
		core.mode_dirty = 0;
	}
	assert(core.mode == drc_mode());

	return drc_eligible(core.mode);
}


// Reads a byte of code at a linear address without side effects.  This fails for anything but memory, except under
// the verifier, which also reads code that it may not compile; the memory flag tells the two apart.
bool i386_device::drc_read_code(offs_t address, uint8_t &value, bool &memory)
{
	offs_t const physical = address & m_core->a20_mask;
	unsigned const bytes = m_program->data_width() / 8;
	void const *const unit = m_program->get_read_ptr(physical & ~offs_t(bytes - 1));
	if (!unit)
		return m_verifier && drc_verify_read_code(physical, value, memory);

	memory = true;
	unsigned const shift = (physical & (bytes - 1)) * 8;
	switch (bytes)
	{
	case 2:
		value = uint8_t(*reinterpret_cast<uint16_t const *>(unit) >> shift);
		return true;
	case 4:
		value = uint8_t(*reinterpret_cast<uint32_t const *>(unit) >> shift);
		return true;
	case 8:
		value = uint8_t(*reinterpret_cast<uint64_t const *>(unit) >> shift);
		return true;
	default:
		return false;
	}
}



/***************************************************************************
    DATA PAGES
***************************************************************************/

void i386_device::drc_pages_start()
{
	m_drc_data = std::make_shared<drc_data_pages>();
	m_drc_data->read = make_unique_clear<uint32_t []>(DATA_PAGES);
	m_drc_data->write = make_unique_clear<uint32_t []>(DATA_PAGES);
}


// Chooses the host address that table entries are relative to.  It is built into generated code, so it is only chosen
// while the code cache is empty, and is kept from then on.
void i386_device::drc_pages_choose_anchor()
{
	drc_data_pages &data = *m_drc_data;
	if (data.anchor || (m_program->data_width() != 32))
		return;

	uint8_t *start = nullptr;
	uint32_t length = 0;
	uint32_t longest = 0;
	for (uint32_t page = 0; page < DATA_ANCHOR_PAGES; page++)
	{
		offs_t const base = page << 12;
		auto *const pointer = m_verifier
				? drc_verify_host_page(base, false)
				: reinterpret_cast<uint8_t *>(m_program->get_read_ptr(base));
		if (pointer && length && (pointer == (start + (size_t(length) << 12))))
		{
			length++;
		}
		else
		{
			start = pointer;
			length = pointer ? 1 : 0;
		}
		if (length > longest)
		{
			longest = length;
			data.anchor = start;
		}
	}
}


void i386_device::drc_pages_flush()
{
	drc_data_pages &data = *m_drc_data;
	for (uint32_t const key : data.filled)
		(BIT(key, 0) ? data.write : data.read)[key >> 1] = 0;
	data.filled.clear();
	data.refused.clear();
}


// returns the host memory of a physical page if all of it can be read, or written, directly
uint8_t *i386_device::drc_host_page(offs_t address, bool write)
{
	// the verifier's taps hide the pointers
	if (m_verifier)
		return drc_verify_host_page(address, write);

	auto const pointer =
			[this, write] (offs_t a)
			{
				return reinterpret_cast<uint8_t *>(write ? m_program->get_write_ptr(a) : m_program->get_read_ptr(a));
			};
	uint8_t *const result = pointer(address);
	if (!result)
		return nullptr;
	for (offs_t offset = 4; offset < 0x1000; offset += 4)
	{
		if (pointer(address + offset) != (result + offset))
			return nullptr;
	}
	return result;
}


// enters the page of a linear address in a data table, if it can be
void i386_device::drc_page_fill(uint32_t address, bool write)
{
	drc_data_pages &data = *m_drc_data;
	uint32_t const page = address >> 12;
	uint32_t *const table = (write ? data.write : data.read).get();
	if ((ENDIANNESS_NATIVE != ENDIANNESS_LITTLE) || !data.anchor || !page || table[page] || debugger_enabled())
		return;

	offs_t const physical = (address & ~uint32_t(0xfff)) & m_core->a20_mask;

	// writes to a page holding compiled instructions have to go through its tap
	if (write && m_drc_code->pages.count(physical >> 12))
		return;

	uint32_t const key = (page << 1) | (write ? 1 : 0);
	if (data.refused.count(key))
		return;

	uint8_t *const host = drc_host_page(physical, write);
	intptr_t const distance = host ? (reinterpret_cast<intptr_t>(host) - reinterpret_cast<intptr_t>(data.anchor)) : 0;
	if (!host || (distance < INT32_MIN) || (distance > (INT32_MAX - 0x1000)) || (distance & 3))
	{
		if (host)
			data.unreachable++;
		data.refused.insert(key);
		return;
	}

	table[page] = (uint32_t(distance) - (page << 12)) + 1;
	data.filled.push_back(key);
	data.fills++;
}


// For the verifier: tests that the write table holds the entry which the page of a linear address would be given now,
// and that the index is the sum of that entry and the address.
bool i386_device::drc_page_write_is_current(uint32_t address, uint32_t index)
{
	uint32_t const page = address >> 12;
	offs_t const physical = (address & ~uint32_t(0xfff)) & m_core->a20_mask;
	uint8_t *const host = (page && !m_drc_code->pages.count(physical >> 12) && !debugger_enabled())
			? drc_host_page(physical, true)
			: nullptr;
	if (!host)
		return false;
	uint32_t const distance = uint32_t(reinterpret_cast<intptr_t>(host) - reinterpret_cast<intptr_t>(m_drc_data->anchor));
	uint32_t const entry = (distance - (page << 12)) + 1;
	return (m_drc_data->write[page] == entry) && (index == uint32_t(entry + address));
}


// removes the write entries of the linear pages that reach a physical page
void i386_device::drc_page_unmap_write(uint32_t physical_page)
{
	drc_data_pages &data = *m_drc_data;
	data.write[physical_page] = 0;
	if (!BIT(m_core->a20_mask, 20))
		data.write[physical_page ^ 0x100] = 0;
}



/***************************************************************************
    CODE WRITES
***************************************************************************/

void i386_device::drc_code_start()
{
	m_drc_code = std::make_shared<drc_code_watch>();
	char const *const interval = std::getenv("MAME_I386_DRC_STATS");
	if (interval)
		m_drc_code->report_interval = m_drc_code->next_report = std::max(std::atoi(interval), 1);

	// these deliberate errors apply with and without the verifier
	char const *const breakage = std::getenv("MAME_I386_DRC_VERIFY_BREAK");
	m_drc_break_nodrop = breakage && (std::string_view(breakage) == "nodrop");
	m_drc_break_noentry = breakage && (std::string_view(breakage) == "noentry");
	m_drc_break_chainirq = breakage && (std::string_view(breakage) == "chainirq");

	drc_pages_start();

	m_drc_map_notifier = m_program->add_change_notifier(
			[this] (read_or_write)
			{
				if (!m_drc_installing_taps)
				{
					m_drc_cache_dirty = true;
					m_drc_code->map_changes++;
				}
			});
}


// removes the taps, which must be done whenever the code that they protect is discarded; true if there were any
bool i386_device::drc_code_clear()
{
	drc_code_watch &watch = *m_drc_code;
	if (watch.pages.empty())
		return false;

	m_drc_installing_taps = true;
	watch.tap.remove();
	m_drc_installing_taps = false;
	watch.pages.clear();
	return true;
}


// watches the bytes of an instruction that has been built into compiled code
void i386_device::drc_code_protect(opcode_desc const &desc)
{
	drc_code_watch &watch = *m_drc_code;
	watch.native++;
	for (unsigned i = 0; i < desc.length; i++)
	{
		offs_t const physical = (desc.pc + i) & m_core->a20_mask;
		std::unique_ptr<drc_code_watch::page> &entry = watch.pages[physical >> 12];
		if (!entry)
		{
			entry = std::make_unique<drc_code_watch::page>();
			drc_code_watch::page *const page = entry.get();
			offs_t const base = physical & ~offs_t(0xfff);
			watch.page_installs++;
			if (!m_drc_break_nodrop)
				drc_page_unmap_write(physical >> 12);
			m_drc_installing_taps = true;
			watch.tap = m_program->install_write_tap(
					base, base | 0xfff,
					"i386_drc_code",
					[this, page] (offs_t offset, u32 &data, u32 mem_mask)
					{
						drc_code_watch &watch = *m_drc_code;
						unsigned const dword = (offset & 0xfff) >> 2;
						unsigned const region = (offset & 0xfff) >> 8;
						watch.writes++;
						if (page->armed)
						{
							page->armed = false;
							watch.page_hits++;
						}
						if (BIT(page->armed_regions, region))
						{
							page->armed_regions &= ~(1 << region);
							watch.region_hits++;
						}
						uint32_t const lanes = page->built[dword] & mem_mask;
						if (lanes && !drc_verify_breaks(DRC_BREAK_SMC))
						{
							// no block that remains in the lookup contains these bytes
							page->built[dword] &= ~lanes;
							drc_code_written(offset & ~offs_t(3), lanes);
						}
					},
					&watch.tap);
			m_drc_installing_taps = false;
		}
		entry->built[(physical & 0xfff) >> 2] |= uint32_t(0xff) << ((physical & 3) * 8);
		entry->armed_regions |= 1 << ((physical & 0xfff) >> 8);
		entry->armed = true;
	}
}


// removes the blocks that can contain the written bytes of a dword, given by their byte lanes
void i386_device::drc_code_written(uint32_t address, uint32_t lanes)
{
	uint32_t first = address;
	while (!(lanes & 0xff))
	{
		lanes >>= 8;
		first++;
	}
	uint32_t last = first;
	while (lanes >>= 8)
		last++;

	// blocks are looked up by linear address, which equals the physical address apart from the A20 mask; this also
	// removes the blocks that begin up to the longest block before the first byte
	m_drcuml->hash_invalidate_range(first, last);
	if (!BIT(m_core->a20_mask, 20) && !BIT(first, 20))
		m_drcuml->hash_invalidate_range(first | 0x100000, last | 0x100000);

	m_drc_code->invalidations++;
	m_drc_code_modified = true;
	if (m_verifier)
		drc_verify_code_written();
}


void i386_device::drc_code_report(bool periodic)
{
	drc_code_watch const &watch = *m_drc_code;
	drc_data_pages const &data = *m_drc_data;
	auto const line = util::string_format(
			"%s: recompiler at %.3f s: %u blocks, %u compiled instructions, %u flushes, %u map changes, %u pages tapped, "
			"%u writes to tapped pages, %u invalidations (%u by page, %u by 256-byte region), %u data pages entered, "
			"%u out of reach, %u called reads, %u called writes\n",
			tag(), machine().time().as_double(), watch.blocks, watch.native, watch.flushes, watch.map_changes, watch.page_installs,
			watch.writes, watch.invalidations, watch.page_hits, watch.region_hits, data.fills,
			data.unreachable, data.slow_reads, data.slow_writes);
	if (periodic)
		osd_printf_info("%s", line);
	else
		osd_printf_verbose("%s", line);
}



/***************************************************************************
    FUNCTIONS CALLED FROM GENERATED CODE
***************************************************************************/

// Runs the body of a function called from generated code, and tells generated code whether it may continue.
// Nothing thrown by the body leaves this function.
template <typename Body>
void i386_device::drc_guarded_call(Body &&body)
{
	try
	{
		body();
		m_core->drc_exit = drc_exit_request();
	}
	catch (...)
	{
		// a fatal error, an allocation failure or a guest fault that the body does not handle
		m_drc_exception = std::current_exception();
		m_core->drc_exit = DRC_EXIT_EXCEPTION;
	}
}


// what generated code has to do after a called function
uint32_t i386_device::drc_exit_request()
{
	if (m_core->fault != NO_FAULT)
		return DRC_EXIT_FAULT;
	bool const code_modified = m_drc_code_modified && !drc_verify_breaks(DRC_BREAK_NOEXIT);
	bool runnable = drc_runnable();
	if (!runnable && m_drc_break_chainirq)
	{
		// a deliberate error: a pending interrupt and the interrupt shadow do not end generated code
		core_state &core = *m_core;
		uint8_t const interrupt_flag = core.IF;
		auto const shadow = core.delayed_interrupt_enable;
		core.IF = 0;
		core.delayed_interrupt_enable = 0;
		runnable = drc_eligible(core.mode);
		core.IF = interrupt_flag;
		core.delayed_interrupt_enable = shadow;
	}
	return (!runnable || m_drc_cache_dirty || code_modified) ? DRC_EXIT_INTERPRET : DRC_EXIT_NONE;
}


// passes on an exception caught while generated code was running
void i386_device::drc_rethrow_exception()
{
	if (m_drc_exception)
		std::rethrow_exception(std::exchange(m_drc_exception, nullptr));
}


void i386_device::cfunc_step(void *param)
{
	i386_device &cpu = *static_cast<i386_device *>(param);
	cpu.drc_guarded_call(
			[&cpu] ()
			{
				// step() delivers the faults raised while an instruction executes.  The interrupt and breakpoint checks
				// that precede an instruction can raise one only with an interrupt, SMI or breakpoint pending, and no
				// block is entered then: the execution loop calls step() itself, so such a fault leaves execute_run()
				// with or without the recompiler, and the catch below is not reached by it.
				try
				{
					cpu.step();
				}
				catch (uint64_t e)
				{
					cpu.m_core->fault = uint32_t(e);
					cpu.m_core->fault_code = uint32_t(e >> 32);
				}
			});
}


void i386_device::cfunc_deliver_fault(void *param)
{
	i386_device &cpu = *static_cast<i386_device *>(param);
	cpu.drc_guarded_call(
			[&cpu] ()
			{
				uint32_t const fault = cpu.m_core->fault;
				uint32_t const error = cpu.m_core->fault_code;

				cpu.m_core->fault = NO_FAULT;
				cpu.m_core->ext = 1;
				cpu.i386_trap_with_error(fault, 0, 0, error);

				// as at the end of an instruction
				if (cpu.m_core->RF && cpu.m_core->auto_clear_RF)
					cpu.m_core->RF = 0;
				if (!cpu.m_core->auto_clear_RF)
					cpu.m_core->auto_clear_RF = true;
			});
}


// Performs a memory access for a compiled instruction whose page has no entry, as the interpreter does.  The offset
// is in ea_tmp, the segment in arg1, and the value written or read in arg0.
template <typename Data, bool Write>
void i386_device::cfunc_access(void *param)
{
	static_assert((sizeof(Data) == 1) || (sizeof(Data) == 4));
	i386_device &cpu = *static_cast<i386_device *>(param);
	cpu.drc_guarded_call(
			[&cpu] ()
			{
				core_state &core = *cpu.m_core;
				try
				{
					uint32_t const address = cpu.i386_translate(int(core.arg1), core.ea_tmp, Write ? 1 : 0, sizeof(Data));
					if constexpr (Write)
					{
						cpu.m_drc_data->slow_writes++;
						uint32_t value = core.arg0;
						if (cpu.drc_verify_breaks(DRC_BREAK_STORE))
							cpu.drc_verify_break_store(value);
						if constexpr (sizeof(Data) == 1)
							cpu.WRITE8(address, uint8_t(value));
						else
							cpu.WRITE32(address, value);
					}
					else
					{
						cpu.m_drc_data->slow_reads++;
						if constexpr (sizeof(Data) == 1)
							core.arg0 = cpu.READ8(address);
						else
							core.arg0 = cpu.READ32(address);
					}
					cpu.drc_page_fill(address, Write);
				}
				catch (uint64_t e)
				{
					core.fault = uint32_t(e);
					core.fault_code = uint32_t(e >> 32);
				}
			});
}


// Enters the pages of an address missed by a compiled instruction which the interpreter is to execute.  The linear
// address is in ea_tmp, and arg1 is set if the instruction writes to it.
void i386_device::cfunc_page_miss(void *param)
{
	i386_device &cpu = *static_cast<i386_device *>(param);
	cpu.drc_guarded_call(
			[&cpu] ()
			{
				core_state const &core = *cpu.m_core;
				cpu.drc_page_fill(core.ea_tmp, false);
				if (core.arg1)
					cpu.drc_page_fill(core.ea_tmp, true);
			});
}


template <typename Data>
void i386_device::cfunc_verify_store_start(void *param)
{
	i386_device &cpu = *static_cast<i386_device *>(param);
	cpu.drc_guarded_call([&cpu] () { cpu.drc_verify_store_start<Data>(); });
}


void i386_device::cfunc_verify_store_end(void *param)
{
	i386_device &cpu = *static_cast<i386_device *>(param);
	cpu.drc_guarded_call([&cpu] () { cpu.drc_verify_store_end(); });
}


void i386_device::cfunc_verify_instruction(void *param)
{
	i386_device &cpu = *static_cast<i386_device *>(param);
	cpu.drc_guarded_call([&cpu] () { cpu.drc_verify_instruction(); });
}


void i386_device::cfunc_verify_break(void *param)
{
	i386_device &cpu = *static_cast<i386_device *>(param);
	cpu.drc_guarded_call([&cpu] () { cpu.drc_verify_break(); });
}


void i386_device::cfunc_verify_break_rmw(void *param)
{
	i386_device &cpu = *static_cast<i386_device *>(param);
	cpu.drc_guarded_call([&cpu] () { cpu.drc_verify_break_rmw(); });
}


void i386_device::cfunc_verify_break_ea(void *param)
{
	i386_device &cpu = *static_cast<i386_device *>(param);
	cpu.drc_guarded_call([&cpu] () { cpu.drc_verify_break_ea(); });
}


void i386_device::cfunc_verify_break_mov(void *param)
{
	i386_device &cpu = *static_cast<i386_device *>(param);
	cpu.drc_guarded_call([&cpu] () { cpu.drc_verify_break_mov(); });
}


void i386_device::cfunc_verify_break_flags(void *param)
{
	i386_device &cpu = *static_cast<i386_device *>(param);
	cpu.drc_guarded_call([&cpu] () { cpu.drc_verify_break_flags(); });
}


void i386_device::cfunc_verify_break_carry(void *param)
{
	i386_device &cpu = *static_cast<i386_device *>(param);
	cpu.drc_guarded_call([&cpu] () { cpu.drc_verify_break_carry(); });
}


void i386_device::cfunc_verify_break_save_flags(void *param)
{
	i386_device &cpu = *static_cast<i386_device *>(param);
	cpu.drc_guarded_call([&cpu] () { cpu.drc_verify_break_save_flags(); });
}


void i386_device::cfunc_verify_break_restore_flags(void *param)
{
	i386_device &cpu = *static_cast<i386_device *>(param);
	cpu.drc_guarded_call([&cpu] () { cpu.drc_verify_break_restore_flags(); });
}



/***************************************************************************
    STATIC CODE
***************************************************************************/

void i386_device::static_generate_entry_point(drcuml_block &block)
{
	alloc_handle(*m_drcuml, m_nocode, "nocode");
	alloc_handle(*m_drcuml, m_entry, "entry");

	// interrupts and the other conditions that need the interpreter are tested before entering
	UML_HANDLE(block, *m_entry);                                            // handle  entry
	UML_HASHJMP(block, mem(&m_core->mode), mem(&m_core->pc), *m_nocode);    // hashjmp <mode>,<pc>,nocode
}


void i386_device::static_generate_nocode_handler(drcuml_block &block)
{
	alloc_handle(*m_drcuml, m_nocode, "nocode");

	UML_HANDLE(block, *m_nocode);                                           // handle  nocode
	UML_GETEXP(block, I0);                                                  // getexp  i0
	UML_MOV(block, mem(&m_core->pc), I0);                                   // mov     [pc],i0
	UML_EXIT(block, EXECUTE_MISSING_CODE);                                  // exit    EXECUTE_MISSING_CODE
}


void i386_device::static_generate_out_of_cycles(drcuml_block &block)
{
	alloc_handle(*m_drcuml, m_out_of_cycles, "out_of_cycles");

	// the CPU state is already complete, and the interpreter finishes the timeslice
	UML_HANDLE(block, *m_out_of_cycles);                                    // handle  out_of_cycles
	UML_EXIT(block, EXECUTE_OUT_OF_CYCLES);                                 // exit    EXECUTE_OUT_OF_CYCLES
}


void i386_device::static_generate_fault_handler(drcuml_block &block)
{
	alloc_handle(*m_drcuml, m_nocode, "nocode");
	alloc_handle(*m_drcuml, m_fault_handler, "fault");

	UML_HANDLE(block, *m_fault_handler);                                    // handle  fault
	generate_call(block, &i386_device::cfunc_deliver_fault);                // callc   cfunc_deliver_fault
	UML_CMP(block, mem(&m_core->drc_exit), DRC_EXIT_NONE);                  // cmp     [drc_exit],none
	block.append().exit(COND_NE, EXECUTE_INTERPRET);                        // exit    EXECUTE_INTERPRET,ne
	generate_block_end(block, mem(&m_core->pc));
}



/***************************************************************************
    COMPILATION
***************************************************************************/

void i386_device::drc_compile_block(uint32_t mode, offs_t pc)
{
	auto profile = g_profiler.start(PROFILER_DRC_COMPILE);

	// the first sequence of the list starts at the address; its instructions follow each other
	opcode_desc const *const first = m_drcfe->describe_code(pc);
	assert(first && (first->pc == pc));

	uml::code_label const done = 1;
	uml::code_label const slow = 2;

	// A full code cache aborts the compilation, which is repeated after a flush.  A block that needs more UML
	// instructions than it has room for raises a fatal error instead, which is not caught here.
	opcode_desc const *final = first;
	bool succeeded = false;
	while (!succeeded)
	{
		try
		{
			drcuml_block &block(m_drcuml->begin_block(COMPILE_MAX_UML));

			UML_HASH(block, mode, pc);                                      // hash    mode,pc
			bool decode_state_set = false;
			uml::code_label label = slow + 1;

			// instructions which do nothing mark places in the buffer, to measure how much of it is used
			uml::instruction *const start = &block.append();
			start->nop();
			for (opcode_desc const *desc = first; desc; desc = desc->next())
			{
				uml::instruction *const mark = &block.append();
				mark->nop();
				bool const full = (uint32_t(mark - start) + (2 * COMPILE_UML_PER_INSTRUCTION) + COMPILE_UML_PER_BLOCK) > COMPILE_MAX_UML;
				bool const last = full || desc->end_sequence() || !desc->next();
				assert(last || (desc->next()->pc == (desc->pc + desc->length)));
				generate_instruction(block, *desc, desc == first, last, done, slow, label, decode_state_set);
				final = desc;
				if (last)
					break;
			}

			UML_LABEL(block, done);                                         // done:
			generate_block_end(block, mem(&m_core->pc));
			UML_LABEL(block, slow);                                         // slow:
			generate_slow_exit(block);

			block.end();
			succeeded = true;
		}
		catch (drcuml_block::abort_compilation const &)
		{
			drc_flush_cache();
		}
	}

	// watch the bytes that are now part of the code
	m_drc_code->blocks++;
	for (opcode_desc const *desc = first; desc; desc = desc->next())
	{
		if (drc_compiles_natively(*desc))
			drc_code_protect(*desc);
		if (desc == final)
			break;
	}
}


// Decodes the compiled forms of the arithmetic and logic instructions.  Each takes the cycle table entry that its
// interpreter handler takes, which does not always follow the operand kinds: the immediate forms with a memory operand
// take the entries of the register to memory forms.  The length shows that a form without a memory operand has no
// prefix; the caller tests the prefixes of the forms with one.
bool i386_device::drc_decode_alu(opcode_desc const &desc, drc_alu_form &form) const
{
	static constexpr drc_alu_form::operation ARITHMETIC[8] = {
			drc_alu_form::ADD, drc_alu_form::OR, drc_alu_form::ADC, drc_alu_form::SBB,
			drc_alu_form::AND, drc_alu_form::SUB, drc_alu_form::XOR, drc_alu_form::SUB };
	constexpr unsigned ARITHMETIC_CMP = 7;

	uint16_t const op = desc.opcode;
	bool const register_rm = desc.modrm >= 0xc0;
	unsigned const group = BIT(desc.modrm, 3, 3);
	MODRM_TABLE const &fields = i386_MODRM_table[desc.modrm];

	form.write = true;
	form.clear_af = false;
	form.imm_first = 0;
	form.imm_second = 0;
	form.dest = form.first = form.second = drc_alu_form::IMMEDIATE;
	form.cycles = form.register_cycles = CYCLES_ALU_REG_REG;

	auto const rm =
			[&form, &fields, register_rm] ()
			{
				if (!register_rm)
					return drc_alu_form::MEMORY;
				return form.bytes ? int(fields.rm.b) : int(fields.rm.d);
			};
	// the entries of a form with a ModRM operand, according to whether that is a register or memory
	auto const take =
			[&form, register_rm] (X86_CYCLES register_entry, X86_CYCLES memory_entry)
			{
				form.register_cycles = register_entry;
				form.cycles = register_rm ? register_entry : memory_entry;
			};
	auto const length_is = [&desc, register_rm] (unsigned length) { return !register_rm || (desc.length == length); };
	auto const reg = [&form, &fields] () { return form.bytes ? int(fields.reg.b) : int(fields.reg.d); };
	auto const accumulator = [&form] () { return form.bytes ? int(AL) : int(EAX); };

	if ((op < 0x40) && ((op & 7) < 6))
	{
		unsigned const operation = op >> 3;
		form.op = ARITHMETIC[operation];
		form.bytes = !(op & 1);
		form.write = operation != ARITHMETIC_CMP;
		switch (op & 7)
		{
		case 0:
		case 1:
			form.dest = form.first = rm();
			form.second = reg();
			break;
		case 2:
		case 3:
			form.dest = form.first = reg();
			form.second = rm();
			break;
		default:
			form.dest = form.first = accumulator();
			form.imm_second = desc.immediate;
			form.cycles = form.write ? CYCLES_ALU_IMM_ACC : CYCLES_CMP_IMM_ACC;
			return desc.length == (form.bytes ? 2 : 5);
		}
		if (!form.write)
			take(CYCLES_CMP_REG_REG, BIT(op, 1) ? CYCLES_CMP_MEM_REG : CYCLES_CMP_REG_MEM);
		else
			take(CYCLES_ALU_REG_REG, BIT(op, 1) ? CYCLES_ALU_MEM_REG : CYCLES_ALU_REG_MEM);
		return length_is(2);
	}
	else if ((op >= 0x40) && (op <= 0x4f))
	{
		form.op = (op < 0x48) ? drc_alu_form::INC : drc_alu_form::DEC;
		form.bytes = false;
		form.dest = form.first = op & 7;
		form.imm_second = 1;
		form.cycles = (op < 0x48) ? CYCLES_INC_REG : CYCLES_DEC_REG;
		return desc.length == 1;
	}

	switch (op)
	{
	case 0x80:
	case 0x81:
	case 0x82:
	case 0x83:
		form.op = ARITHMETIC[group];
		form.bytes = (op == 0x80) || (op == 0x82);
		form.write = group != ARITHMETIC_CMP;
		form.dest = form.first = rm();
		form.imm_second = (op == 0x83) ? uint32_t(int32_t(int8_t(desc.immediate))) : desc.immediate;
		if (form.write)
			take(CYCLES_ALU_REG_REG, CYCLES_ALU_REG_MEM);
		else
			take(CYCLES_CMP_REG_REG, CYCLES_CMP_REG_MEM);
		return length_is((op == 0x81) ? 6 : 3);

	case 0x84:
	case 0x85:
		form.op = drc_alu_form::AND;
		form.bytes = op == 0x84;
		form.write = false;
		form.dest = form.first = rm();
		form.second = reg();
		take(CYCLES_TEST_REG_REG, CYCLES_TEST_REG_MEM);
		return length_is(2);

	case 0xa8:
	case 0xa9:
		form.op = drc_alu_form::AND;
		form.bytes = op == 0xa8;
		form.write = false;
		form.dest = form.first = accumulator();
		form.imm_second = desc.immediate;
		form.cycles = (op == 0xa8) ? CYCLES_ALU_IMM_ACC : CYCLES_TEST_IMM_ACC;
		return desc.length == (form.bytes ? 2 : 5);

	case 0xf6:
	case 0xf7:
		form.bytes = op == 0xf6;
		form.dest = form.first = rm();
		switch (group)
		{
		case 0:     // test with an immediate
			form.op = drc_alu_form::AND;
			form.write = false;
			form.clear_af = true;
			form.imm_second = desc.immediate;
			take(CYCLES_TEST_IMM_REG, CYCLES_TEST_IMM_MEM);
			return length_is(form.bytes ? 3 : 6);
		case 2:
			form.op = drc_alu_form::NOT;
			take(CYCLES_NOT_REG, CYCLES_NOT_MEM);
			return length_is(2);
		case 3:     // neg subtracts from zero
			form.op = drc_alu_form::SUB;
			form.first = drc_alu_form::IMMEDIATE;
			form.second = rm();
			take(CYCLES_NEG_REG, CYCLES_NEG_MEM);
			return length_is(2);
		default:
			return false;
		}

	case 0xfe:
	case 0xff:
		form.bytes = op == 0xfe;
		form.op = group ? drc_alu_form::DEC : drc_alu_form::INC;
		form.dest = form.first = rm();
		form.imm_second = 1;
		if (group)
			take(CYCLES_DEC_REG, CYCLES_DEC_MEM);
		else
			take(CYCLES_INC_REG, CYCLES_INC_MEM);
		return (group <= 1) && length_is(2);

	default:
		return false;
	}
}


// The compiled instructions are MOV of 8 and 32 bits between registers, memory and immediates, LEA, arithmetic and
// logic on registers, memory and immediates, MOVZX, MOVSX, XCHG with memory, the conditional jumps, the near jumps,
// calls and returns, the loops, PUSH, POP and LEAVE, all without prefixes.
bool i386_device::drc_compiles_natively(opcode_desc const &desc) const
{
	if (!desc.in_memory || (m_program->data_width() != 32) || !drc_can_bake_code())
		return false;

	// a prefix is recorded in one of these two fields
	bool const unprefixed = !desc.prefixes && (desc.override_segment == opcode_desc::NO_SEGMENT);

	// a memory operand in the data or stack segment
	bool const memory_operand = unprefixed && ((desc.segment == DS) || (desc.segment == SS));

	// A miss of a memory operand of anything but MOV steps the whole instruction, which the debugger would then be
	// told of twice; the same goes for an instruction which uses the stack
	bool const steppable_operand = memory_operand && !debugger_enabled();
	bool const steppable_stack = unprefixed && !debugger_enabled();
	bool const register_rm = desc.modrm >= 0xc0;
	unsigned const group = BIT(desc.modrm, 3, 3);

	drc_alu_form form;
	switch (desc.kind)
	{
	case opcode_desc::family::MOV:
		break;
	case opcode_desc::family::LEA:
		return unprefixed && (desc.opcode == 0x8d) && (desc.modrm < 0xc0);
	case opcode_desc::family::ALU:
	case opcode_desc::family::TEST:
	case opcode_desc::family::INCDEC:
		return unprefixed && drc_decode_alu(desc, form) && (!form.memory() || steppable_operand);
	case opcode_desc::family::EXTEND:
		return (desc.modrm >= 0xc0) ? (unprefixed && (desc.length == 3)) : steppable_operand;
	case opcode_desc::family::XCHG:
		return ((desc.opcode == 0x86) || (desc.opcode == 0x87)) && (desc.modrm < 0xc0) && steppable_operand;
	case opcode_desc::family::JCC:
		// the length shows that there is no prefix, such as a 16-bit operand size or a branch hint
		return unprefixed && (desc.length == ((desc.opcode < 0x80) ? 2 : 6));
	case opcode_desc::family::LOOP:
		return unprefixed && (desc.length == 2);
	case opcode_desc::family::JMP:
		if (desc.opcode == 0xff)
			return (group == 4) && (register_rm ? (unprefixed && (desc.length == 2)) : steppable_operand);
		return unprefixed && (desc.length == ((desc.opcode == 0xeb) ? 2 : 5));
	case opcode_desc::family::CALL:
		if (desc.opcode == 0xff)
			return (group == 2) && (register_rm ? (steppable_stack && (desc.length == 2)) : steppable_operand);
		return steppable_stack && (desc.length == 5);
	case opcode_desc::family::RET:
		return steppable_stack && (desc.length == ((desc.opcode == 0xc2) ? 3 : 1));
	case opcode_desc::family::PUSHPOP:
		switch (desc.opcode)
		{
		case 0x68:
			return steppable_stack && (desc.length == 5);
		case 0x6a:
			return steppable_stack && (desc.length == 2);
		case 0x8f:
		case 0xff:
			return (group == ((desc.opcode == 0xff) ? 6 : 0))
					&& (register_rm ? (steppable_stack && (desc.length == 2)) : steppable_operand);
		default:    // 50-5f and c9
			return steppable_stack && (desc.length == 1);
		}
	default:
		return false;
	}

	switch (desc.opcode)
	{
	case 0x88:
	case 0x89:
	case 0x8a:
	case 0x8b:
		return (desc.modrm >= 0xc0) ? (desc.length == 2) : memory_operand;
	case 0xa0:
	case 0xa1:
	case 0xa2:
	case 0xa3:
		return memory_operand;
	case 0xc6:
		if (desc.modrm >= 0xc0)
			return (desc.length == 3) && ((desc.modrm & 0xf8) == 0xc0);
		return memory_operand && !(desc.modrm & 0x38);
	case 0xc7:
		if (desc.modrm >= 0xc0)
			return (desc.length == 6) && ((desc.modrm & 0xf8) == 0xc0);
		return memory_operand && !(desc.modrm & 0x38);
	default:
		// the length shows that there is no prefix
		if ((desc.opcode >= 0xb0) && (desc.opcode <= 0xb7))
			return desc.length == 2;
		return (desc.opcode >= 0xb8) && (desc.opcode <= 0xbf) && (desc.length == 5);
	}
}


// calls a function that uses drc_guarded_call(), and leaves generated code at once if it caught an exception
void i386_device::generate_call(drcuml_block &block, void (*function)(void *))
{
	UML_CALLC(block, function, this);                                       // callc   function
	UML_CMP(block, mem(&m_core->drc_exit), DRC_EXIT_EXCEPTION);             // cmp     [drc_exit],exception
	block.append().exit(COND_E, EXECUTE_EXCEPTION);                         // exit    EXECUTE_EXCEPTION,e
}


// Goes on to the block at an address, which the instruction pointer must already hold.  Code is only generated and run
// in one mode.
void i386_device::generate_block_end(drcuml_block &block, uml::parameter address)
{
	// the verifier compares the state after each block
	if (m_verifier)
		UML_EXIT(block, EXECUTE_BLOCK_DONE);                                // exit    EXECUTE_BLOCK_DONE
	else
		UML_HASHJMP(block, MODE_COMPILED, address, *m_nocode);              // hashjmp <mode>,address,nocode
}


// leaves the block for the target of a direct jump, call or loop
void i386_device::generate_direct_transfer(drcuml_block &block, opcode_desc const &desc)
{
	// the address and the instruction pointer are the same with a flat code segment
	uint32_t const target = desc.targetpc;
	if (drc_verify_breaks(DRC_BREAK_TARGET) && !(desc.pc % 3))
	{
		// a deliberate error: the target of some transfers, chosen by their address, is one byte further in checked blocks
		UML_ADD(block, mem(&m_core->eip), mem(&m_core->verify_checking), target);   // add     [eip],[verify_checking],target
		UML_MOV(block, mem(&m_core->pc), mem(&m_core->eip));                // mov     [pc],[eip]
	}
	else
	{
		UML_MOV(block, mem(&m_core->eip), target);                          // mov     [eip],target
		UML_MOV(block, mem(&m_core->pc), target);                           // mov     [pc],target
	}
	generate_block_end(block, target);
}


// Executes one instruction, which is started only while cycles are left.
void i386_device::generate_instruction(drcuml_block &block, opcode_desc const &desc, bool first, bool last, uml::code_label done, uml::code_label slow, uml::code_label &label, bool &decode_state_set)
{
	// a deliberate error: a block starts without cycles left.  Blocks of one instruction keep the test, as without it a
	// loop of one would never end.
	if (!m_drc_break_noentry || !first || last)
	{
		UML_CMP(block, mem(&m_core->cycles), 0);                            // cmp     [cycles],0
		UML_EXHc(block, COND_LE, *m_out_of_cycles, desc.pc);                // exh     out_of_cycles,pc,le
	}

	if (m_verifier)
	{
		UML_MOV(block, mem(&m_core->verify_pc), desc.pc);                   // mov     [verify_pc],pc
		generate_call(block, &i386_device::cfunc_verify_instruction);       // callc   cfunc_verify_instruction
	}

	if (drc_compiles_natively(desc))
	{
		generate_native_start(block, desc, decode_state_set);
		if (desc.kind == opcode_desc::family::LEA)
		{
			generate_lea(block, desc);
			return;
		}
		if (desc.kind == opcode_desc::family::JCC)
		{
			generate_jcc(block, desc, label);
			return;
		}
		if (desc.kind == opcode_desc::family::EXTEND)
		{
			generate_extend(block, desc, last, done, slow, label);
			return;
		}
		if (desc.kind == opcode_desc::family::XCHG)
		{
			generate_xchg(block, desc, last, done, slow, label);
			return;
		}
		if (desc.kind == opcode_desc::family::LOOP)
		{
			generate_loop(block, desc, label);
			return;
		}
		if (desc.kind == opcode_desc::family::JMP)
		{
			generate_near_jump(block, desc, last, done, slow, label);
			return;
		}
		if (desc.kind == opcode_desc::family::CALL)
		{
			generate_near_call(block, desc, last, done, slow, label);
			return;
		}
		if (desc.kind == opcode_desc::family::RET)
		{
			generate_near_return(block, desc, last, done, slow, label);
			return;
		}
		if (desc.kind == opcode_desc::family::PUSHPOP)
		{
			if (((desc.opcode >= 0x58) && (desc.opcode <= 0x5f)) || (desc.opcode == 0x8f) || (desc.opcode == 0xc9))
				generate_pop(block, desc, last, done, slow, label);
			else
				generate_push(block, desc, last, done, slow, label);
			return;
		}
		if (desc.kind != opcode_desc::family::MOV)
		{
			// a deliberate error: the flags of an instruction before a stepped one are lost
			drc_alu_form form;
			bool const lose_flags = drc_verify_breaks(DRC_BREAK_STALE)
					&& drc_decode_alu(desc, form) && form.sets_flags()
					&& !last && !drc_compiles_natively(*desc.next());
			if (lose_flags)
				generate_call(block, &i386_device::cfunc_verify_break_save_flags);     // callc   cfunc_verify_break_save_flags
			generate_alu(block, desc, last, done, slow, label);
			if (lose_flags)
				generate_call(block, &i386_device::cfunc_verify_break_restore_flags);  // callc   cfunc_verify_break_restore_flags
			return;
		}
		bool const moffs = (desc.opcode >= 0xa0) && (desc.opcode <= 0xa3);
		bool const modrm = ((desc.opcode >= 0x88) && (desc.opcode <= 0x8b)) || (desc.opcode == 0xc6) || (desc.opcode == 0xc7);
		if (moffs || (modrm && (desc.modrm < 0xc0)))
			generate_memory_mov(block, desc, label, slow);
		else
			generate_register_mov(block, desc);
	}
	else
	{
		generate_step(block, desc, last, done, slow);
		decode_state_set = false;
	}
}


// Executes one instruction with the interpreter.  Control goes on to the next instruction of the block only while
// the interpreter loop would run it next: there are cycles left, nothing needs the interpreter, no compiled
// instruction has been written to, and execution has arrived at the address the instruction was compiled at.
void i386_device::generate_step(drcuml_block &block, opcode_desc const &desc, bool last, uml::code_label done, uml::code_label slow)
{
	UML_CALLC(block, &i386_device::cfunc_step, this);                       // callc   cfunc_step
	if (m_drc_verify_break)
	{
		UML_CMP(block, mem(&m_core->drc_exit), DRC_EXIT_EXCEPTION);         // cmp     [drc_exit],exception
		block.append().exit(COND_E, EXECUTE_EXCEPTION);                     // exit    EXECUTE_EXCEPTION,e
		generate_call(block, &i386_device::cfunc_verify_break);             // callc   cfunc_verify_break
	}
	UML_CMP(block, mem(&m_core->drc_exit), DRC_EXIT_NONE);                  // cmp     [drc_exit],none
	UML_JMPc(block, COND_NE, slow);                                         // jmp     slow,ne

	if (!last && !drc_verify_ignores_address())
	{
		UML_CMP(block, mem(&m_core->pc), uint32_t(desc.pc + desc.length));  // cmp     [pc],next
		UML_JMPc(block, COND_NE, done);                                     // jmp     done,ne
	}
}


// The part of a compiled instruction that leaves behind the state step() would have while executing the instruction: the
// per-instruction decode fields and the addresses.
void i386_device::generate_native_start(drcuml_block &block, opcode_desc const &desc, bool &decode_state_set)
{
	// the decode fields of a 32-bit instruction without prefixes stay as set until an instruction is stepped
	if (!decode_state_set)
	{
		UML_MOV(block, mem(&m_core->operand_size), 1);                      // mov     [operand_size],1
		UML_MOV(block, mem(&m_core->xmm_operand_size), 0);                  // mov     [xmm_operand_size],0
		UML_MOV(block, mem(&m_core->address_size), 1);                      // mov     [address_size],1
		UML_MOV(block, mem(&m_core->operand_prefix), 0);                    // mov     [operand_prefix],0
		UML_MOV(block, mem(&m_core->address_prefix), 0);                    // mov     [address_prefix],0
		UML_MOV(block, mem(&m_core->segment_prefix), 0);                    // mov     [segment_prefix],0
		UML_STORE(block, &m_core->ext, 0, 1, SIZE_BYTE, SCALE_x1);          // store   [ext],1
		UML_STORE(block, &m_core->auto_clear_RF, 0, 1, SIZE_BYTE, SCALE_x1);    // store   [auto_clear_RF],1
		decode_state_set = true;
	}
	UML_MOV(block, mem(&m_core->prev_eip), desc.pc);                        // mov     [prev_eip],pc
	if (debugger_enabled())
		UML_DEBUG(block, desc.pc);                                          // debug   pc

	uint32_t const next = desc.pc + desc.length;
	UML_MOV(block, mem(&m_core->eip), next);                                // mov     [eip],next
	UML_MOV(block, mem(&m_core->pc), next);                                 // mov     [pc],next
}


// Compiled MOV to a register from a register or an immediate.  Nothing here can fault, call out or change the mode or
// the interrupt state.
void i386_device::generate_register_mov(drcuml_block &block, opcode_desc const &desc)
{
	// the register encodings of the instruction: for opcodes B0-BF in the low three bits of the opcode, otherwise in
	// the ModRM byte, where the reg field is the source of 88 and 89 and the destination of 8A and 8B
	bool const immediate = desc.opcode >= 0xb0;
	bool const bytes = (desc.opcode == 0x88) || (desc.opcode == 0x8a) || (desc.opcode == 0xc6) || ((desc.opcode >= 0xb0) && (desc.opcode <= 0xb7));
	uint8_t const modrm = ((desc.opcode >= 0xb0) && (desc.opcode <= 0xbf)) ? (0xc0 | (desc.opcode & 7)) : desc.modrm;
	bool const reg_is_destination = (desc.opcode == 0x8a) || (desc.opcode == 0x8b);
	MODRM_TABLE const &fields = i386_MODRM_table[modrm];
	int const destination = reg_is_destination
			? (bytes ? fields.reg.b : fields.reg.d)
			: (bytes ? fields.rm.b : fields.rm.d);
	int const source = reg_is_destination
			? (bytes ? fields.rm.b : fields.rm.d)
			: (bytes ? fields.reg.b : fields.reg.d);

	if (bytes)
	{
		if (immediate || (desc.opcode == 0xc6))
		{
			UML_STORE(block, &m_core->reg.b[0], destination, uint8_t(desc.immediate), SIZE_BYTE, SCALE_x1); // store   reg8,imm
		}
		else
		{
			UML_LOAD(block, I0, &m_core->reg.b[0], source, SIZE_BYTE, SCALE_x1);        // load    i0,reg8
			UML_STORE(block, &m_core->reg.b[0], destination, I0, SIZE_BYTE, SCALE_x1);  // store   reg8,i0
		}
	}
	else if (immediate || (desc.opcode == 0xc7))
	{
		UML_MOV(block, mem(&m_core->reg.d[destination]), desc.immediate);   // mov     [reg],imm
	}
	else
	{
		UML_MOV(block, mem(&m_core->reg.d[destination]), mem(&m_core->reg.d[source]));  // mov     [reg],[reg]
	}

	X86_CYCLES const cycles = (immediate || (desc.opcode == 0xc6) || (desc.opcode == 0xc7)) ? CYCLES_MOV_IMM_REG : CYCLES_MOV_REG_REG;
	UML_SUB(block, mem(&m_core->cycles), mem(&m_core->cycles), m_cycle_table_pm[cycles]);   // sub     [cycles],[cycles],n

	if (drc_verify_breaks(DRC_BREAK_MOV))
	{
		UML_MOV(block, mem(&m_core->ea_tmp), bytes ? destination : (destination * 4));  // mov     [ea_tmp],offset
		generate_call(block, &i386_device::cfunc_verify_break_mov);         // callc   cfunc_verify_break_mov
	}
}


// the effective address of a 32-bit ModRM memory operand, in i0
void i386_device::generate_effective_address(drcuml_block &block, opcode_desc const &desc)
{
	unsigned const mod = BIT(desc.modrm, 6, 2);
	unsigned base = BIT(desc.modrm, 0, 3);
	int index = -1;
	unsigned scale = 0;
	if (base == 4)
	{
		base = BIT(desc.sib, 0, 3);
		scale = BIT(desc.sib, 6, 2);
		if (BIT(desc.sib, 3, 3) != 4)
			index = BIT(desc.sib, 3, 3);
	}
	bool const has_base = mod || (base != 5);
	if (index >= 0)
	{
		if (scale)
			UML_SHL(block, I0, mem(&m_core->reg.d[index]), scale);      // shl     i0,[index],scale
		else
			UML_MOV(block, I0, mem(&m_core->reg.d[index]));             // mov     i0,[index]
		if (has_base)
			UML_ADD(block, I0, I0, mem(&m_core->reg.d[base]));          // add     i0,i0,[base]
		if (desc.displacement)
			UML_ADD(block, I0, I0, desc.displacement);                  // add     i0,i0,displacement
	}
	else if (!has_base)
	{
		UML_MOV(block, I0, desc.displacement);                          // mov     i0,displacement
	}
	else if (desc.displacement)
	{
		UML_ADD(block, I0, mem(&m_core->reg.d[base]), desc.displacement);   // add     i0,[base],displacement
	}
	else
	{
		UML_MOV(block, I0, mem(&m_core->reg.d[base]));                  // mov     i0,[base]
	}
}


// Compiled LEA, which writes the effective address to a register and sets no flags.
void i386_device::generate_lea(drcuml_block &block, opcode_desc const &desc)
{
	int const reg = i386_MODRM_table[desc.modrm].reg.d;
	generate_effective_address(block, desc);
	UML_MOV(block, mem(&m_core->reg.d[reg]), I0);                           // mov     [reg],i0
	UML_SUB(block, mem(&m_core->cycles), mem(&m_core->cycles), m_cycle_table_pm[CYCLES_LEA]);  // sub     [cycles],[cycles],n

	if (drc_verify_breaks(DRC_BREAK_ALU))
	{
		UML_MOV(block, mem(&m_core->ea_tmp), reg * 4);                      // mov     [ea_tmp],offset
		generate_call(block, &i386_device::cfunc_verify_break_mov);         // callc   cfunc_verify_break_mov
	}
}


// Compiled arithmetic and logic.  The operands are in i0 and i1 and the result in i2; i3 and i4 are used to work out
// flags.  A memory operand is read into i6 first, so that every form shares the flag path.  Each flag byte is stored with
// the value the interpreter's handler gives it, before a result goes to memory.  A form without a memory operand cannot
// fault, call out or change the mode or the interrupt state.
void i386_device::generate_alu(drcuml_block &block, opcode_desc const &desc, bool last, uml::code_label done, uml::code_label slow, uml::code_label &label)
{
	drc_alu_form form;
	bool const decoded = drc_decode_alu(desc, form);
	assert(decoded);
	if (!decoded)
		return;

	bool const arithmetic = (form.op == drc_alu_form::ADD) || (form.op == drc_alu_form::ADC) || (form.op == drc_alu_form::INC)
			|| (form.op == drc_alu_form::SUB) || (form.op == drc_alu_form::SBB) || (form.op == drc_alu_form::DEC);
	bool const logic = (form.op == drc_alu_form::AND) || (form.op == drc_alu_form::OR) || (form.op == drc_alu_form::XOR);
	bool const subtract = (form.op == drc_alu_form::SUB) || (form.op == drc_alu_form::SBB) || (form.op == drc_alu_form::DEC);
	bool const carry_in = (form.op == drc_alu_form::ADC) || (form.op == drc_alu_form::SBB);
	bool const carry_out = arithmetic && (form.op != drc_alu_form::INC) && (form.op != drc_alu_form::DEC);
	bool const to_memory = form.write && (form.dest == drc_alu_form::MEMORY);
	operand_size const size = form.bytes ? SIZE_BYTE : SIZE_DWORD;

	uml::code_label const miss = form.memory() ? label++ : 0;
	if (form.memory())
		generate_operand_load(block, desc, size, false, to_memory, miss);

	if (carry_in && drc_verify_breaks(DRC_BREAK_CARRY))
		generate_call(block, &i386_device::cfunc_verify_break_carry);       // callc   cfunc_verify_break_carry

	auto const load_operand =
			[this, &block, &form] (uml::parameter dest, int reg, uint32_t immediate)
			{
				if (reg == drc_alu_form::MEMORY)
					UML_MOV(block, dest, I6);                                           // mov     dest,i6
				else if (reg == drc_alu_form::IMMEDIATE)
					UML_MOV(block, dest, form.bytes ? (immediate & 0xff) : immediate);  // mov     dest,imm
				else if (form.bytes)
					UML_LOAD(block, dest, &m_core->reg.b[0], reg, SIZE_BYTE, SCALE_x1); // load    dest,reg8
				else
					UML_MOV(block, dest, mem(&m_core->reg.d[reg]));                     // mov     dest,[reg]
			};
	auto const store_flag =
			[&block] (uint8_t &flag, uml::parameter value)
			{
				UML_STORE(block, &flag, 0, value, SIZE_BYTE, SCALE_x1);                 // store   flag,value
			};

	load_operand(I0, form.first, form.imm_first);
	if (form.op != drc_alu_form::NOT)
		load_operand(I1, form.second, form.imm_second);

	// CF as a value in i3, for the operations that read it
	if (carry_in)
		UML_LOAD(block, I3, &m_core->CF, 0, SIZE_BYTE, SCALE_x1);           // load    i3,CF

	switch (form.op)
	{
	case drc_alu_form::ADD:
	case drc_alu_form::INC:
		UML_ADD(block, I2, I0, I1);                                         // add     i2,i0,i1
		break;
	case drc_alu_form::SUB:
	case drc_alu_form::DEC:
		UML_SUB(block, I2, I0, I1);                                         // sub     i2,i0,i1
		break;
	case drc_alu_form::ADC:
		if (form.bytes)
		{
			UML_ADD(block, I2, I0, I1);                                     // add     i2,i0,i1
			UML_ADD(block, I2, I2, I3);                                     // add     i2,i2,i3
		}
		else
		{
			UML_CARRY(block, I3, 0);                                        // carry   i3,0
			UML_ADDC(block, I2, I0, I1);                                    // addc    i2,i0,i1
		}
		break;
	case drc_alu_form::SBB:
		if (form.bytes)
		{
			UML_SUB(block, I2, I0, I1);                                     // sub     i2,i0,i1
			UML_SUB(block, I2, I2, I3);                                     // sub     i2,i2,i3
		}
		else
		{
			UML_CARRY(block, I3, 0);                                        // carry   i3,0
			UML_SUBB(block, I2, I0, I1);                                    // subb    i2,i0,i1
		}
		break;
	case drc_alu_form::AND:
		UML_AND(block, I2, I0, I1);                                         // and     i2,i0,i1
		break;
	case drc_alu_form::OR:
		UML_OR(block, I2, I0, I1);                                          // or      i2,i0,i1
		break;
	case drc_alu_form::XOR:
		UML_XOR(block, I2, I0, I1);                                         // xor     i2,i0,i1
		break;
	case drc_alu_form::NOT:
		UML_XOR(block, I2, I0, 0xffffffff);                                 // xor     i2,i0,0xffffffff
		break;
	}

	if (form.sets_flags())
	{
		if (!form.bytes && arithmetic)
		{
			// the host flags of the operation, none of the operations in between changing them
			if (carry_out)
			{
				UML_SETc(block, COND_C, I3);                                // set     i3,c
				store_flag(m_core->CF, I3);
			}
			UML_SETc(block, COND_V, I3);                                    // set     i3,v
			store_flag(m_core->OF, I3);
			UML_SETc(block, COND_Z, I3);                                    // set     i3,z
			store_flag(m_core->ZF, I3);
			UML_SETc(block, COND_S, I3);                                    // set     i3,s
			store_flag(m_core->SF, I3);
		}
		else
		{
			if (form.bytes)
			{
				UML_TEST(block, I2, 0xff);                                  // test    i2,0xff
				UML_SETc(block, COND_Z, I3);                                // set     i3,z
				store_flag(m_core->ZF, I3);
				UML_ROLAND(block, I3, I2, 25, 1);                           // roland  i3,i2,25,1
				store_flag(m_core->SF, I3);
			}
			else
			{
				UML_CMP(block, I2, 0);                                      // cmp     i2,0
				UML_SETc(block, COND_Z, I3);                                // set     i3,z
				store_flag(m_core->ZF, I3);
				UML_ROLAND(block, I3, I2, 1, 1);                            // roland  i3,i2,1,1
				store_flag(m_core->SF, I3);
			}

			if (logic)
			{
				store_flag(m_core->CF, 0);
				store_flag(m_core->OF, 0);
				if (form.clear_af)
					store_flag(m_core->AF, 0);
			}
			else
			{
				// the 8-bit result keeps the carry or borrow out in bit 8
				if (carry_out)
				{
					UML_ROLAND(block, I3, I2, 24, 1);                       // roland  i3,i2,24,1
					store_flag(m_core->CF, I3);
				}
				// signed overflow: the result differs in sign from both operands of an addition, or from the first of a
				// subtraction whose operands differ in sign
				if (subtract)
				{
					UML_XOR(block, I3, I0, I1);                             // xor     i3,i0,i1
					UML_XOR(block, I4, I0, I2);                             // xor     i4,i0,i2
				}
				else
				{
					UML_XOR(block, I3, I2, I1);                             // xor     i3,i2,i1
					UML_XOR(block, I4, I2, I0);                             // xor     i4,i2,i0
				}
				UML_AND(block, I3, I3, I4);                                 // and     i3,i3,i4
				UML_ROLAND(block, I3, I3, 25, 1);                           // roland  i3,i3,25,1
				store_flag(m_core->OF, I3);
			}
		}

		if (arithmetic)
		{
			UML_XOR(block, I3, I0, I1);                                     // xor     i3,i0,i1
			UML_XOR(block, I3, I3, I2);                                     // xor     i3,i3,i2
			UML_ROLAND(block, I3, I3, 28, 1);                               // roland  i3,i3,28,1
			store_flag(m_core->AF, I3);
		}

		static_assert(sizeof(i386_parity_table[0]) == 4);
		UML_AND(block, I3, I2, 0xff);                                       // and     i3,i2,0xff
		UML_LOAD(block, I3, &i386_parity_table[0], I3, SIZE_DWORD, SCALE_x4);  // load    i3,parity[i3]
		store_flag(m_core->PF, I3);
	}

	if (to_memory)
	{
		generate_operand_store(block, size, label);
	}
	else if (form.write)
	{
		if (form.bytes)
			UML_STORE(block, &m_core->reg.b[0], form.dest, I2, SIZE_BYTE, SCALE_x1);   // store   reg8,i2
		else
			UML_MOV(block, mem(&m_core->reg.d[form.dest]), I2);                        // mov     [reg],i2
	}
	uint32_t const cost = form.memory() ? drc_operand_cycles(form.cycles, form.register_cycles) : m_cycle_table_pm[form.cycles];
	UML_SUB(block, mem(&m_core->cycles), mem(&m_core->cycles), cost);      // sub     [cycles],[cycles],n

	if (form.write && !to_memory && drc_verify_breaks(DRC_BREAK_ALU))
	{
		UML_MOV(block, mem(&m_core->ea_tmp), form.bytes ? form.dest : (form.dest * 4));  // mov     [ea_tmp],offset
		generate_call(block, &i386_device::cfunc_verify_break_mov);         // callc   cfunc_verify_break_mov
	}
	if (form.sets_flags() && drc_verify_breaks(DRC_BREAK_FLAGS))
	{
		UML_MOV(block, mem(&m_core->ea_tmp), desc.pc % 6);                  // mov     [ea_tmp],flag
		generate_call(block, &i386_device::cfunc_verify_break_flags);       // callc   cfunc_verify_break_flags
	}

	if (form.memory())
		generate_operand_miss(block, desc, to_memory, last, done, slow, miss, label);
}


// Compiled MOVZX and MOVSX, from a register or memory of 8 or 16 bits to a 32-bit register.
void i386_device::generate_extend(drcuml_block &block, opcode_desc const &desc, bool last, uml::code_label done, uml::code_label slow, uml::code_label &label)
{
	bool const sign = BIT(desc.opcode, 3);
	bool const words = BIT(desc.opcode, 0);
	bool const memory = desc.modrm < 0xc0;
	operand_size const size = words ? SIZE_WORD : SIZE_BYTE;
	MODRM_TABLE const &fields = i386_MODRM_table[desc.modrm];
	int const dest = fields.reg.d;

	uml::code_label const miss = memory ? label++ : 0;
	if (memory)
	{
		generate_operand_load(block, desc, size, sign, false, miss);
	}
	else
	{
		void const *const base = words ? static_cast<void const *>(&m_core->reg.w[0]) : static_cast<void const *>(&m_core->reg.b[0]);
		int const source = words ? fields.rm.w : fields.rm.b;
		memory_scale const scale = words ? SCALE_x2 : SCALE_x1;
		if (sign)
			UML_LOADS(block, I6, base, source, size, scale);                // loads   i6,reg
		else
			UML_LOAD(block, I6, base, source, size, scale);                 // load    i6,reg
	}
	UML_MOV(block, mem(&m_core->reg.d[dest]), I6);                          // mov     [reg],i6

	X86_CYCLES const register_cycles = sign ? CYCLES_MOVSX_REG_REG : CYCLES_MOVZX_REG_REG;
	uint32_t const cost = memory
			? drc_operand_cycles(sign ? CYCLES_MOVSX_MEM_REG : CYCLES_MOVZX_MEM_REG, register_cycles)
			: m_cycle_table_pm[register_cycles];
	UML_SUB(block, mem(&m_core->cycles), mem(&m_core->cycles), cost);      // sub     [cycles],[cycles],n

	if (drc_verify_breaks(memory ? DRC_BREAK_LOAD : DRC_BREAK_MOV))
	{
		UML_MOV(block, mem(&m_core->ea_tmp), dest * 4);                     // mov     [ea_tmp],offset
		generate_call(block, &i386_device::cfunc_verify_break_mov);         // callc   cfunc_verify_break_mov
	}

	if (memory)
		generate_operand_miss(block, desc, false, last, done, slow, miss, label);
}


// Compiled XCHG of a register with memory: memory is read, then written, and the register is written last.
void i386_device::generate_xchg(drcuml_block &block, opcode_desc const &desc, bool last, uml::code_label done, uml::code_label slow, uml::code_label &label)
{
	bool const bytes = desc.opcode == 0x86;
	operand_size const size = bytes ? SIZE_BYTE : SIZE_DWORD;
	int const reg = bytes ? i386_MODRM_table[desc.modrm].reg.b : i386_MODRM_table[desc.modrm].reg.d;

	uml::code_label const miss = label++;
	generate_operand_load(block, desc, size, false, true, miss);
	if (bytes)
		UML_LOAD(block, I2, &m_core->reg.b[0], reg, SIZE_BYTE, SCALE_x1);   // load    i2,reg8
	else
		UML_MOV(block, I2, mem(&m_core->reg.d[reg]));                       // mov     i2,[reg]
	generate_operand_store(block, size, label);
	if (bytes)
		UML_STORE(block, &m_core->reg.b[0], reg, I6, SIZE_BYTE, SCALE_x1);  // store   reg8,i6
	else
		UML_MOV(block, mem(&m_core->reg.d[reg]), I6);                       // mov     [reg],i6
	UML_SUB(block, mem(&m_core->cycles), mem(&m_core->cycles), drc_operand_cycles(CYCLES_XCHG_REG_MEM, CYCLES_XCHG_REG_REG));  // sub     [cycles],[cycles],n

	if (drc_verify_breaks(DRC_BREAK_LOAD))
	{
		UML_MOV(block, mem(&m_core->ea_tmp), bytes ? reg : (reg * 4));      // mov     [ea_tmp],offset
		generate_call(block, &i386_device::cfunc_verify_break_mov);         // callc   cfunc_verify_break_mov
	}

	generate_operand_miss(block, desc, true, last, done, slow, miss, label);
}


// The cycles taken by an instruction with a memory operand.  The deliberate error charges those of the register form
// instead, or one more where the two are the same.
uint32_t i386_device::drc_operand_cycles(unsigned memory_entry, unsigned register_entry) const
{
	uint32_t const cost = m_cycle_table_pm[memory_entry];
	if (!drc_verify_breaks(DRC_BREAK_MCYCLES))
		return cost;
	uint32_t const other = m_cycle_table_pm[register_entry];
	return (other != cost) ? other : (cost + 1);
}


// Reads the memory operand of a compiled instruction other than MOV.  The linear address is left in i0 and i7, the
// index which reaches the host memory from the anchor in i5 and the value, extended to 32 bits, in i6.  Control goes to
// the miss label, with the address in i0 and nothing changed, unless the operand lies within one page that can be
// accessed directly: for an operand which is also written, the read and write entries of the page have to be the same,
// which they are only where both reach the same host memory.
void i386_device::generate_operand_load(drcuml_block &block, opcode_desc const &desc, uml::operand_size size, bool sign, bool write, uml::code_label miss)
{
	drc_data_pages const &data = *m_drc_data;

	generate_effective_address(block, desc);
	if (drc_verify_breaks(DRC_BREAK_EA))
	{
		UML_MOV(block, mem(&m_core->ea_tmp), I0);                           // mov     [ea_tmp],i0
		generate_call(block, &i386_device::cfunc_verify_break_ea);          // callc   cfunc_verify_break_ea
		UML_MOV(block, I0, mem(&m_core->ea_tmp));                           // mov     i0,[ea_tmp]
	}
	UML_MOV(block, I7, I0);                                                 // mov     i7,i0

	if (size != SIZE_BYTE)
	{
		UML_AND(block, I1, I0, 0xfff);                                      // and     i1,i0,0xfff
		UML_CMP(block, I1, 0x1000 - ((size == SIZE_WORD) ? 2 : 4));         // cmp     i1,0x1000-size
		UML_JMPc(block, COND_A, miss);                                      // jmp     miss,a
	}
	UML_SHR(block, I1, I0, 12);                                             // shr     i1,i0,12

	// a deliberate error: a page that may only be read is written
	bool const test_write = write && !drc_verify_breaks(DRC_BREAK_RMWMISS);
	uint32_t const *const table = (test_write ? data.write : data.read).get();
	UML_LOAD(block, I5, table, I1, SIZE_DWORD, SCALE_x4);                   // load    i5,table[i1]
	UML_CMP(block, I5, 0);                                                  // cmp     i5,0
	UML_JMPc(block, COND_E, miss);                                          // jmp     miss,e
	if (test_write)
	{
		UML_LOAD(block, I1, data.read.get(), I1, SIZE_DWORD, SCALE_x4);     // load    i1,read[i1]
		UML_CMP(block, I1, I5);                                             // cmp     i1,i5
		UML_JMPc(block, COND_NE, miss);                                     // jmp     miss,ne
	}
	UML_ADD(block, I5, I5, I0);                                             // add     i5,i5,i0
	if (sign)
		UML_LOADS(block, I6, data.anchor - 1, I5, size, SCALE_x1);          // loads   i6,[host]
	else
		UML_LOAD(block, I6, data.anchor - 1, I5, size, SCALE_x1);           // load    i6,[host]
}


// writes i2 to the memory operand read by generate_operand_load()
void i386_device::generate_operand_store(drcuml_block &block, uml::operand_size size, uml::code_label &label)
{
	// a deliberate error: the value read is written back
	if (drc_verify_breaks(DRC_BREAK_RMW))
	{
		UML_MOV(block, mem(&m_core->arg0), I2);                             // mov     [arg0],i2
		UML_MOV(block, mem(&m_core->arg1), I6);                             // mov     [arg1],i6
		generate_call(block, &i386_device::cfunc_verify_break_rmw);         // callc   cfunc_verify_break_rmw
		UML_MOV(block, I2, mem(&m_core->arg0));                             // mov     i2,[arg0]
	}
	generate_direct_store(block, size, I7, I5, label);
}


// Writes i2 to host memory, given the linear address and the index which reaches the host memory from the anchor.  The
// verifier examines the entry and records the write before and after the store of a checked block.
void i386_device::generate_direct_store(drcuml_block &block, uml::operand_size size, uml::parameter address, uml::parameter index, uml::code_label &label)
{
	drc_data_pages const &data = *m_drc_data;
	uml::code_label const unrecorded = m_verifier ? label++ : 0;
	if (m_verifier)
	{
		UML_CMP(block, mem(&m_core->verify_checking), 0);                   // cmp     [verify_checking],0
		UML_JMPc(block, COND_E, unrecorded);                                // jmp     unrecorded,e
		UML_MOV(block, mem(&m_core->ea_tmp), address);                      // mov     [ea_tmp],address
		UML_MOV(block, mem(&m_core->arg0), I2);                             // mov     [arg0],i2
		UML_MOV(block, mem(&m_core->arg1), index);                          // mov     [arg1],index
		generate_call(block, (size == SIZE_BYTE) ? &i386_device::cfunc_verify_store_start<uint8_t> : &i386_device::cfunc_verify_store_start<uint32_t>);
		UML_MOV(block, I2, mem(&m_core->arg0));                             // mov     i2,[arg0]
		UML_LABEL(block, unrecorded);                                       // unrecorded:
	}
	UML_STORE(block, data.anchor - 1, index, I2, size, SCALE_x1);           // store   [host],i2
	if (m_verifier)
	{
		uml::code_label const unchecked = label++;
		UML_CMP(block, mem(&m_core->verify_checking), 0);                   // cmp     [verify_checking],0
		UML_JMPc(block, COND_E, unchecked);                                 // jmp     unchecked,e
		generate_call(block, &i386_device::cfunc_verify_store_end);         // callc   cfunc_verify_store_end
		UML_LABEL(block, unchecked);                                        // unchecked:
	}
}


// Ends a compiled instruction with a memory operand.  On a miss the pages of the address are entered in the tables if
// they can be, and the interpreter executes the whole instruction, which keeps its order of accesses, its faults and
// its effects on devices and compiled code.
void i386_device::generate_operand_miss(drcuml_block &block, opcode_desc const &desc, bool write, bool last, uml::code_label done, uml::code_label slow, uml::code_label miss, uml::code_label &label)
{
	uml::code_label const complete = label++;
	UML_JMP(block, complete);                                               // jmp     complete

	UML_LABEL(block, miss);                                                 // miss:
	UML_MOV(block, mem(&m_core->ea_tmp), I0);                               // mov     [ea_tmp],i0
	UML_MOV(block, mem(&m_core->arg1), write ? 1 : 0);                      // mov     [arg1],write
	generate_call(block, &i386_device::cfunc_page_miss);                    // callc   cfunc_page_miss
	UML_MOV(block, mem(&m_core->eip), desc.pc);                             // mov     [eip],pc
	UML_MOV(block, mem(&m_core->pc), desc.pc);                              // mov     [pc],pc
	generate_step(block, desc, last, done, slow);

	UML_LABEL(block, complete);                                             // complete:
}


// Compiled conditional jumps.  The condition is that of the interpreter's handler, worked out from the flag bytes: the
// low bit of the opcode inverts the condition given by the other three bits of the low half.  A taken jump leaves the
// block for its target.  Nothing here can fault, call out or change the mode or the interrupt state.
void i386_device::generate_jcc(drcuml_block &block, opcode_desc const &desc, uml::code_label &label)
{
	bool const short_form = desc.opcode < 0x80;
	unsigned const condition = BIT(desc.opcode, 0, 4);

	auto const load_flag =
			[&block] (uml::parameter dest, uint8_t &flag)
			{
				UML_LOAD(block, dest, &flag, 0, SIZE_BYTE, SCALE_x1);               // load    dest,flag
			};

	// i0 is non-zero if the condition of the even opcode holds
	switch (condition >> 1)
	{
	case 0: // o
		load_flag(I0, m_core->OF);
		break;
	case 1: // b
		load_flag(I0, m_core->CF);
		break;
	case 2: // z
		load_flag(I0, m_core->ZF);
		break;
	case 3: // be
		load_flag(I0, m_core->CF);
		load_flag(I1, m_core->ZF);
		UML_OR(block, I0, I0, I1);                                          // or      i0,i0,i1
		break;
	case 4: // s
		load_flag(I0, m_core->SF);
		break;
	case 5: // p
		load_flag(I0, m_core->PF);
		break;
	case 6: // l
		load_flag(I0, m_core->SF);
		load_flag(I1, m_core->OF);
		UML_XOR(block, I0, I0, I1);                                         // xor     i0,i0,i1
		break;
	case 7: // le
		load_flag(I0, m_core->SF);
		load_flag(I1, m_core->OF);
		UML_XOR(block, I0, I0, I1);                                         // xor     i0,i0,i1
		load_flag(I1, m_core->ZF);
		UML_OR(block, I0, I0, I1);                                          // or      i0,i0,i1
		break;
	}

	// a deliberate error: the condition of some jumps, chosen by their address, is inverted in checked blocks
	if (drc_verify_breaks(DRC_BREAK_COND) && !(desc.pc % 3))
	{
		UML_CMP(block, I0, 0);                                              // cmp     i0,0
		UML_SETc(block, COND_NZ, I0);                                       // set     i0,nz
		UML_XOR(block, I0, I0, mem(&m_core->verify_checking));              // xor     i0,i0,[verify_checking]
	}

	uint32_t taken = m_cycle_table_pm[short_form ? CYCLES_JCC_DISP8 : CYCLES_JCC_FULL_DISP];
	uint32_t not_taken = m_cycle_table_pm[short_form ? CYCLES_JCC_DISP8_NOBRANCH : CYCLES_JCC_FULL_DISP_NOBRANCH];
	bool const swap_cycles = drc_verify_breaks(DRC_BREAK_JCYCLES);
	if (swap_cycles)
		std::swap(taken, not_taken);

	uml::code_label const untaken = label++;
	UML_CMP(block, I0, 0);                                                  // cmp     i0,0
	UML_JMPc(block, BIT(condition, 0) ? COND_NZ : COND_Z, untaken);         // jmp     untaken,z/nz

	UML_SUB(block, mem(&m_core->cycles), mem(&m_core->cycles), taken);      // sub     [cycles],[cycles],n
	// a deliberate error: one more cycle, so that the swapped costs show on a CPU where both are the same
	if (swap_cycles)
		UML_SUB(block, mem(&m_core->cycles), mem(&m_core->cycles), mem(&m_core->verify_checking));
	generate_direct_transfer(block, desc);

	UML_LABEL(block, untaken);                                              // untaken:
	UML_SUB(block, mem(&m_core->cycles), mem(&m_core->cycles), not_taken);  // sub     [cycles],[cycles],n
}


// Compiled LOOP, LOOPE, LOOPNE and JECXZ.  The loops decrement ECX and jump if it is not zero, LOOPE and LOOPNE also
// testing ZF; no flag changes.  Nothing here can fault, call out or change the mode or the interrupt state.
void i386_device::generate_loop(drcuml_block &block, opcode_desc const &desc, uml::code_label &label)
{
	uml::code_label const untaken = label++;
	uint32_t taken, not_taken;
	if (desc.opcode == 0xe3)
	{
		taken = m_cycle_table_pm[CYCLES_JCXZ];
		not_taken = m_cycle_table_pm[CYCLES_JCXZ_NOBRANCH];
		UML_CMP(block, mem(&m_core->reg.d[ECX]), 0);                        // cmp     [ecx],0
		UML_JMPc(block, COND_NZ, untaken);                                  // jmp     untaken,nz
	}
	else
	{
		X86_CYCLES const cycles = (desc.opcode == 0xe0) ? CYCLES_LOOPNZ : (desc.opcode == 0xe1) ? CYCLES_LOOPZ : CYCLES_LOOP;
		taken = not_taken = m_cycle_table_pm[cycles];
		if (drc_verify_breaks(DRC_BREAK_ECX))
		{
			// a deliberate error: ECX is not decremented in checked blocks
			UML_XOR(block, I1, mem(&m_core->verify_checking), 1);           // xor     i1,[verify_checking],1
			UML_SUB(block, mem(&m_core->reg.d[ECX]), mem(&m_core->reg.d[ECX]), I1);    // sub     [ecx],[ecx],i1
		}
		else
		{
			UML_SUB(block, mem(&m_core->reg.d[ECX]), mem(&m_core->reg.d[ECX]), 1);     // sub     [ecx],[ecx],1
		}
		UML_CMP(block, mem(&m_core->reg.d[ECX]), 0);                        // cmp     [ecx],0
		UML_JMPc(block, COND_Z, untaken);                                   // jmp     untaken,z
		if (desc.opcode != 0xe2)
		{
			UML_LOAD(block, I0, &m_core->ZF, 0, SIZE_BYTE, SCALE_x1);       // load    i0,ZF
			UML_CMP(block, I0, 0);                                          // cmp     i0,0
			UML_JMPc(block, (desc.opcode == 0xe0) ? COND_NZ : COND_Z, untaken);    // jmp     untaken,nz/z
		}
	}

	UML_SUB(block, mem(&m_core->cycles), mem(&m_core->cycles), taken);      // sub     [cycles],[cycles],n
	generate_direct_transfer(block, desc);

	UML_LABEL(block, untaken);                                              // untaken:
	UML_SUB(block, mem(&m_core->cycles), mem(&m_core->cycles), not_taken);  // sub     [cycles],[cycles],n
}


// Looks up the dword at the linear address in a register, which must not be i1.  The index which reaches its host memory
// from the anchor is left in the index register.  Control goes to the miss label unless the dword lies within one page
// which has an entry.
void i386_device::generate_page_lookup(drcuml_block &block, uml::parameter address, uml::parameter index, bool write, uml::code_label miss)
{
	drc_data_pages const &data = *m_drc_data;
	uint32_t const *const table = (write ? data.write : data.read).get();

	UML_AND(block, I1, address, 0xfff);                                     // and     i1,address,0xfff
	UML_CMP(block, I1, 0x1000 - 4);                                         // cmp     i1,0xffc
	UML_JMPc(block, COND_A, miss);                                          // jmp     miss,a
	UML_SHR(block, I1, address, 12);                                        // shr     i1,address,12
	UML_LOAD(block, index, table, I1, SIZE_DWORD, SCALE_x4);                // load    index,table[i1]
	UML_CMP(block, index, 0);                                               // cmp     index,0
	UML_JMPc(block, COND_E, miss);                                          // jmp     miss,e
	UML_ADD(block, index, index, address);                                  // add     index,index,address
}


// The address pushed to is left in i7 and its index in i5.  Nothing is changed.
void i386_device::generate_push_lookup(drcuml_block &block, uml::code_label miss)
{
	UML_SUB(block, I7, mem(&m_core->reg.d[ESP]), 4);                        // sub     i7,[esp],4
	if (drc_verify_breaks(DRC_BREAK_ESP))
	{
		// a deliberate error: ESP is moved before the access is known to be possible, in checked blocks
		UML_SHL(block, I1, mem(&m_core->verify_checking), 2);               // shl     i1,[verify_checking],2
		UML_SUB(block, mem(&m_core->reg.d[ESP]), mem(&m_core->reg.d[ESP]), I1);    // sub     [esp],[esp],i1
	}
	generate_page_lookup(block, I7, I5, true, miss);
}


// the linear address of the memory operand of a control transfer or stack operation, in i6, and its index, in i3
void i386_device::generate_stack_operand(drcuml_block &block, opcode_desc const &desc, bool write, uint32_t adjust, uml::code_label miss)
{
	generate_effective_address(block, desc);
	if (adjust)
		UML_ADD(block, I0, I0, adjust);                                     // add     i0,i0,adjust
	if (drc_verify_breaks(DRC_BREAK_EA))
	{
		UML_MOV(block, mem(&m_core->ea_tmp), I0);                           // mov     [ea_tmp],i0
		generate_call(block, &i386_device::cfunc_verify_break_ea);          // callc   cfunc_verify_break_ea
		UML_MOV(block, I0, mem(&m_core->ea_tmp));                           // mov     i0,[ea_tmp]
	}
	UML_MOV(block, I6, I0);                                                 // mov     i6,i0
	generate_page_lookup(block, I6, I3, write, miss);
}


// Where a control transfer or stack operation goes if the stack, or its memory operand, cannot be accessed directly: the
// pages of the address in i7 or i6 are entered if they can be, and the interpreter executes the whole instruction.  A
// label of zero means that the instruction has no such access.
void i386_device::generate_stack_miss(drcuml_block &block, opcode_desc const &desc, uml::code_label stack_miss, bool stack_write, uml::code_label operand_miss, bool operand_write, bool last, uml::code_label done, uml::code_label slow, uml::code_label &label)
{
	uml::code_label const execute = label++;
	auto const enter =
			[this, &block, execute] (uml::code_label miss, uml::parameter address, bool write)
			{
				if (!miss)
					return;
				UML_LABEL(block, miss);                                             // miss:
				UML_MOV(block, mem(&m_core->ea_tmp), address);                      // mov     [ea_tmp],address
				UML_MOV(block, mem(&m_core->arg1), write ? 1 : 0);                  // mov     [arg1],write
				generate_call(block, &i386_device::cfunc_page_miss);                // callc   cfunc_page_miss
				UML_JMP(block, execute);                                            // jmp     execute
			};
	enter(operand_miss, I6, operand_write);
	enter(stack_miss, I7, stack_write);

	UML_LABEL(block, execute);                                              // execute:
	UML_MOV(block, mem(&m_core->eip), desc.pc);                             // mov     [eip],pc
	UML_MOV(block, mem(&m_core->pc), desc.pc);                              // mov     [pc],pc
	generate_step(block, desc, last, done, slow);
}


// Compiled near jumps: direct, or to an address from a register or memory.
void i386_device::generate_near_jump(drcuml_block &block, opcode_desc const &desc, bool last, uml::code_label done, uml::code_label slow, uml::code_label &label)
{
	if (desc.opcode != 0xff)
	{
		UML_SUB(block, mem(&m_core->cycles), mem(&m_core->cycles), m_cycle_table_pm[(desc.opcode == 0xeb) ? CYCLES_JMP_SHORT : CYCLES_JMP]);  // sub     [cycles],[cycles],n
		generate_direct_transfer(block, desc);
		return;
	}

	bool const memory = desc.modrm < 0xc0;
	uml::code_label const operand_miss = memory ? label++ : 0;
	if (memory)
	{
		generate_stack_operand(block, desc, false, 0, operand_miss);
		UML_LOAD(block, I4, m_drc_data->anchor - 1, I3, SIZE_DWORD, SCALE_x1);  // load    i4,[operand]
	}
	else
	{
		UML_MOV(block, I4, mem(&m_core->reg.d[i386_MODRM_table[desc.modrm].rm.d]));  // mov     i4,[reg]
	}
	UML_MOV(block, mem(&m_core->eip), I4);                                  // mov     [eip],i4
	UML_MOV(block, mem(&m_core->pc), I4);                                   // mov     [pc],i4
	uint32_t const cost = memory ? drc_operand_cycles(CYCLES_JMP_MEM, CYCLES_JMP_REG) : m_cycle_table_pm[CYCLES_JMP_REG];
	UML_SUB(block, mem(&m_core->cycles), mem(&m_core->cycles), cost);      // sub     [cycles],[cycles],n
	generate_block_end(block, mem(&m_core->pc));

	if (memory)
		generate_stack_miss(block, desc, 0, false, operand_miss, false, last, done, slow, label);
}


// Compiled near calls: direct, or to an address from a register or memory.  The target is read before the return
// address is pushed.
void i386_device::generate_near_call(drcuml_block &block, opcode_desc const &desc, bool last, uml::code_label done, uml::code_label slow, uml::code_label &label)
{
	bool const direct = desc.opcode == 0xe8;
	bool const memory = !direct && (desc.modrm < 0xc0);
	uml::code_label const operand_miss = memory ? label++ : 0;
	uml::code_label const stack_miss = label++;

	if (memory)
		generate_stack_operand(block, desc, false, 0, operand_miss);
	else if (!direct)
		UML_MOV(block, I4, mem(&m_core->reg.d[i386_MODRM_table[desc.modrm].rm.d]));  // mov     i4,[reg]
	generate_push_lookup(block, stack_miss);
	if (memory)
		UML_LOAD(block, I4, m_drc_data->anchor - 1, I3, SIZE_DWORD, SCALE_x1);  // load    i4,[operand]

	uint32_t const next = desc.pc + desc.length;
	if (drc_verify_breaks(DRC_BREAK_RETADDR))
		UML_ADD(block, I2, mem(&m_core->verify_checking), next);            // add     i2,[verify_checking],next
	else
		UML_MOV(block, I2, next);                                           // mov     i2,next
	generate_direct_store(block, SIZE_DWORD, I7, I5, label);
	UML_MOV(block, mem(&m_core->reg.d[ESP]), I7);                           // mov     [esp],i7

	if (direct)
	{
		UML_SUB(block, mem(&m_core->cycles), mem(&m_core->cycles), m_cycle_table_pm[CYCLES_CALL]);  // sub     [cycles],[cycles],n
		generate_direct_transfer(block, desc);
	}
	else
	{
		UML_MOV(block, mem(&m_core->eip), I4);                              // mov     [eip],i4
		UML_MOV(block, mem(&m_core->pc), I4);                               // mov     [pc],i4
		uint32_t const cost = memory ? drc_operand_cycles(CYCLES_CALL_MEM, CYCLES_CALL_REG) : m_cycle_table_pm[CYCLES_CALL_REG];
		UML_SUB(block, mem(&m_core->cycles), mem(&m_core->cycles), cost);  // sub     [cycles],[cycles],n
		generate_block_end(block, mem(&m_core->pc));
	}

	generate_stack_miss(block, desc, stack_miss, true, operand_miss, false, last, done, slow, label);
}


// Compiled near returns.  The immediate of C2 is added to ESP, sign extended, after the pop.
void i386_device::generate_near_return(drcuml_block &block, opcode_desc const &desc, bool last, uml::code_label done, uml::code_label slow, uml::code_label &label)
{
	bool const immediate = desc.opcode == 0xc2;
	uml::code_label const stack_miss = label++;

	UML_MOV(block, I7, mem(&m_core->reg.d[ESP]));                           // mov     i7,[esp]
	generate_page_lookup(block, I7, I5, false, stack_miss);
	UML_LOAD(block, I4, m_drc_data->anchor - 1, I5, SIZE_DWORD, SCALE_x1);  // load    i4,[stack]
	uint32_t const release = 4 + (immediate ? uint32_t(int32_t(int16_t(desc.immediate))) : 0);
	UML_ADD(block, mem(&m_core->reg.d[ESP]), I7, release);                  // add     [esp],i7,release
	UML_MOV(block, mem(&m_core->eip), I4);                                  // mov     [eip],i4
	UML_MOV(block, mem(&m_core->pc), I4);                                   // mov     [pc],i4
	UML_SUB(block, mem(&m_core->cycles), mem(&m_core->cycles), m_cycle_table_pm[immediate ? CYCLES_RET_IMM : CYCLES_RET]);  // sub     [cycles],[cycles],n
	generate_block_end(block, mem(&m_core->pc));

	generate_stack_miss(block, desc, stack_miss, false, 0, false, last, done, slow, label);
}


// Compiled PUSH of a register, a memory operand or an immediate.  The value is that from before ESP changes.
void i386_device::generate_push(drcuml_block &block, opcode_desc const &desc, bool last, uml::code_label done, uml::code_label slow, uml::code_label &label)
{
	bool const immediate = (desc.opcode == 0x68) || (desc.opcode == 0x6a);
	bool const memory = (desc.opcode == 0xff) && (desc.modrm < 0xc0);
	uml::code_label const operand_miss = memory ? label++ : 0;
	uml::code_label const stack_miss = label++;
	uml::code_label const complete = label++;

	if (memory)
		generate_stack_operand(block, desc, false, 0, operand_miss);
	generate_push_lookup(block, stack_miss);

	// nothing above writes a register, apart from a deliberate error which moves ESP
	if (memory)
		UML_LOAD(block, I2, m_drc_data->anchor - 1, I3, SIZE_DWORD, SCALE_x1);  // load    i2,[operand]
	else if (immediate)
		UML_MOV(block, I2, (desc.opcode == 0x6a) ? uint32_t(int32_t(int8_t(desc.immediate))) : desc.immediate);  // mov     i2,imm
	else
	{
		int const reg = (desc.opcode == 0xff) ? int(i386_MODRM_table[desc.modrm].rm.d) : int(desc.opcode & 7);
		if (reg == ESP)
			UML_ADD(block, I2, I7, 4);                                      // add     i2,i7,4
		else
			UML_MOV(block, I2, mem(&m_core->reg.d[reg]));                   // mov     i2,[reg]
	}
	generate_direct_store(block, SIZE_DWORD, I7, I5, label);
	UML_MOV(block, mem(&m_core->reg.d[ESP]), I7);                           // mov     [esp],i7

	uint32_t const cost = memory
			? drc_operand_cycles(CYCLES_PUSH_RM, CYCLES_PUSH_RM)
			: m_cycle_table_pm[immediate ? CYCLES_PUSH_IMM : (desc.opcode == 0xff) ? CYCLES_PUSH_RM : CYCLES_PUSH_REG_SHORT];
	UML_SUB(block, mem(&m_core->cycles), mem(&m_core->cycles), cost);      // sub     [cycles],[cycles],n
	UML_JMP(block, complete);                                               // jmp     complete

	generate_stack_miss(block, desc, stack_miss, true, operand_miss, false, last, done, slow, label);
	UML_LABEL(block, complete);                                             // complete:
}


// Compiled POP to a register or memory, and LEAVE, which sets ESP to EBP and pops EBP.  ESP is written before the
// destination, so a destination of ESP keeps the value read, and the address of a memory destination is computed with
// the new ESP.
void i386_device::generate_pop(drcuml_block &block, opcode_desc const &desc, bool last, uml::code_label done, uml::code_label slow, uml::code_label &label)
{
	bool const leave = desc.opcode == 0xc9;
	bool const memory = (desc.opcode == 0x8f) && (desc.modrm < 0xc0);
	int const reg = leave ? int(EBP) : (desc.opcode == 0x8f) ? int(i386_MODRM_table[desc.modrm].rm.d) : int(desc.opcode & 7);
	uml::code_label const operand_miss = memory ? label++ : 0;
	uml::code_label const stack_miss = label++;
	uml::code_label const complete = label++;

	UML_MOV(block, I7, mem(&m_core->reg.d[leave ? EBP : ESP]));             // mov     i7,[esp]
	generate_page_lookup(block, I7, I5, false, stack_miss);
	if (memory)
	{
		// ESP can only be the base of an address, which takes a SIB byte
		bool const stack_based = (BIT(desc.modrm, 0, 3) == 4) && (BIT(desc.sib, 0, 3) == ESP);
		generate_stack_operand(block, desc, true, stack_based ? 4 : 0, operand_miss);
	}
	UML_LOAD(block, I2, m_drc_data->anchor - 1, I5, SIZE_DWORD, SCALE_x1);  // load    i2,[stack]
	if (memory)
		generate_direct_store(block, SIZE_DWORD, I6, I3, label);
	UML_ADD(block, mem(&m_core->reg.d[ESP]), I7, 4);                        // add     [esp],i7,4
	if (!memory)
		UML_MOV(block, mem(&m_core->reg.d[reg]), I2);                       // mov     [reg],i2

	uint32_t const cost = memory
			? drc_operand_cycles(CYCLES_POP_RM, CYCLES_POP_RM)
			: m_cycle_table_pm[leave ? CYCLES_LEAVE : (desc.opcode == 0x8f) ? CYCLES_POP_RM : CYCLES_POP_REG_SHORT];
	UML_SUB(block, mem(&m_core->cycles), mem(&m_core->cycles), cost);      // sub     [cycles],[cycles],n
	if (!memory && drc_verify_breaks(DRC_BREAK_LOAD))
	{
		UML_MOV(block, mem(&m_core->ea_tmp), reg * 4);                      // mov     [ea_tmp],offset
		generate_call(block, &i386_device::cfunc_verify_break_mov);         // callc   cfunc_verify_break_mov
	}
	UML_JMP(block, complete);                                               // jmp     complete

	generate_stack_miss(block, desc, stack_miss, false, operand_miss, true, last, done, slow, label);
	UML_LABEL(block, complete);                                             // complete:
}


// Compiled MOV between memory and a register, or of an immediate to memory.  A page with a table entry is accessed
// directly; otherwise a function performs the access, which can fault, run device code and ask for the block to be
// left.  A faulting instruction changes no register and takes no cycles, and an access that was performed completes
// the instruction before the block is left.
void i386_device::generate_memory_mov(drcuml_block &block, opcode_desc const &desc, uml::code_label &label, uml::code_label slow)
{
	drc_data_pages const &data = *m_drc_data;
	bool const moffs = (desc.opcode >= 0xa0) && (desc.opcode <= 0xa3);
	bool const immediate = (desc.opcode == 0xc6) || (desc.opcode == 0xc7);
	bool const store = immediate || (desc.opcode == 0x88) || (desc.opcode == 0x89) || (desc.opcode == 0xa2) || (desc.opcode == 0xa3);
	bool const bytes = (desc.opcode == 0x88) || (desc.opcode == 0x8a) || (desc.opcode == 0xa0) || (desc.opcode == 0xa2) || (desc.opcode == 0xc6);
	operand_size const size = bytes ? SIZE_BYTE : SIZE_DWORD;

	// the register operand, as a byte or dword index into the registers
	int reg = EAX;
	if (!moffs && !immediate)
		reg = bytes ? i386_MODRM_table[desc.modrm].reg.b : i386_MODRM_table[desc.modrm].reg.d;
	else if (bytes)
		reg = AL;

	X86_CYCLES cycles;
	switch (desc.opcode)
	{
	case 0x88:
	case 0x89:
		cycles = CYCLES_MOV_REG_MEM;
		break;
	case 0x8a:
	case 0x8b:
		cycles = CYCLES_MOV_MEM_REG;
		break;
	case 0xa1:
	case 0xa2:
		cycles = CYCLES_MOV_MEM_ACC;
		break;
	case 0xa3:
		cycles = CYCLES_MOV_ACC_MEM;
		break;
	default:    // a0, c6 and c7
		cycles = CYCLES_MOV_IMM_MEM;
		break;
	}

	// effective address in i0
	if (moffs)
		UML_MOV(block, I0, desc.displacement);                              // mov     i0,offset
	else
		generate_effective_address(block, desc);
	if (drc_verify_breaks(DRC_BREAK_EA))
	{
		UML_MOV(block, mem(&m_core->ea_tmp), I0);                           // mov     [ea_tmp],i0
		generate_call(block, &i386_device::cfunc_verify_break_ea);          // callc   cfunc_verify_break_ea
		UML_MOV(block, I0, mem(&m_core->ea_tmp));                           // mov     i0,[ea_tmp]
	}

	// the value of a store, in i2
	auto const load_value =
			[this, &block, &desc, immediate, bytes, reg] ()
			{
				if (immediate)
					UML_MOV(block, I2, bytes ? uint8_t(desc.immediate) : desc.immediate);  // mov     i2,imm
				else if (bytes)
					UML_LOAD(block, I2, &m_core->reg.b[0], reg, SIZE_BYTE, SCALE_x1);      // load    i2,reg8
				else
					UML_MOV(block, I2, mem(&m_core->reg.d[reg]));                          // mov     i2,[reg]
			};
	auto const store_register =
			[this, &block, bytes, reg] ()
			{
				if (bytes)
					UML_STORE(block, &m_core->reg.b[0], reg, I2, SIZE_BYTE, SCALE_x1);     // store   reg8,i2
				else
					UML_MOV(block, mem(&m_core->reg.d[reg]), I2);                          // mov     [reg],i2
			};

	uml::code_label const miss = label++;
	uml::code_label const complete = label++;
	uint32_t const *const table = (store ? data.write : data.read).get();

	// access directly if within one page that has an entry
	if (!bytes)
	{
		UML_AND(block, I1, I0, 0xfff);                                      // and     i1,i0,0xfff
		UML_CMP(block, I1, 0x1000 - 4);                                     // cmp     i1,0xffc
		UML_JMPc(block, COND_A, miss);                                      // jmp     miss,a
	}
	UML_SHR(block, I1, I0, 12);                                             // shr     i1,i0,12
	UML_LOAD(block, I1, table, I1, SIZE_DWORD, SCALE_x4);                   // load    i1,table[i1]
	UML_CMP(block, I1, 0);                                                  // cmp     i1,0
	UML_JMPc(block, COND_E, miss);                                          // jmp     miss,e
	UML_ADD(block, I1, I1, I0);                                             // add     i1,i1,i0
	if (store)
	{
		load_value();
		generate_direct_store(block, size, I0, I1, label);
	}
	else
	{
		UML_LOAD(block, I2, data.anchor - 1, I1, size, SCALE_x1);           // load    i2,[host]
		store_register();
	}
	UML_SUB(block, mem(&m_core->cycles), mem(&m_core->cycles), m_cycle_table_pm[cycles]);   // sub     [cycles],[cycles],n
	UML_JMP(block, complete);                                               // jmp     complete

	UML_LABEL(block, miss);                                                 // miss:
	UML_MOV(block, mem(&m_core->ea_tmp), I0);                               // mov     [ea_tmp],i0
	UML_MOV(block, mem(&m_core->arg1), desc.segment);                       // mov     [arg1],segment
	if (store)
	{
		load_value();
		UML_MOV(block, mem(&m_core->arg0), I2);                             // mov     [arg0],i2
		generate_call(block, bytes ? &i386_device::cfunc_access<uint8_t, true> : &i386_device::cfunc_access<uint32_t, true>);
	}
	else
	{
		generate_call(block, bytes ? &i386_device::cfunc_access<uint8_t, false> : &i386_device::cfunc_access<uint32_t, false>);
	}
	UML_CMP(block, mem(&m_core->drc_exit), DRC_EXIT_FAULT);                 // cmp     [drc_exit],fault
	UML_JMPc(block, COND_E, slow);                                          // jmp     slow,e
	if (!store)
	{
		UML_MOV(block, I2, mem(&m_core->arg0));                             // mov     i2,[arg0]
		store_register();
	}
	UML_SUB(block, mem(&m_core->cycles), mem(&m_core->cycles), m_cycle_table_pm[cycles]);   // sub     [cycles],[cycles],n
	UML_CMP(block, mem(&m_core->drc_exit), DRC_EXIT_NONE);                  // cmp     [drc_exit],none
	UML_JMPc(block, COND_NE, slow);                                         // jmp     slow,ne

	UML_LABEL(block, complete);                                             // complete:
	if (!store && drc_verify_breaks(DRC_BREAK_LOAD))
	{
		UML_MOV(block, mem(&m_core->ea_tmp), bytes ? reg : (reg * 4));      // mov     [ea_tmp],offset
		generate_call(block, &i386_device::cfunc_verify_break_mov);         // callc   cfunc_verify_break_mov
	}
}


// leaves a block after a called function has asked for it
void i386_device::generate_slow_exit(drcuml_block &block)
{
	UML_CMP(block, mem(&m_core->drc_exit), DRC_EXIT_EXCEPTION);             // cmp     [drc_exit],exception
	block.append().exit(COND_E, EXECUTE_EXCEPTION);                         // exit    EXECUTE_EXCEPTION,e
	UML_CMP(block, mem(&m_core->drc_exit), DRC_EXIT_FAULT);                 // cmp     [drc_exit],fault
	UML_EXHc(block, COND_E, *m_fault_handler, 0);                           // exh     fault,0,e
	UML_EXIT(block, EXECUTE_INTERPRET);                                     // exit    EXECUTE_INTERPRET
}



/***************************************************************************
    EXECUTION
***************************************************************************/

void i386_device::execute_run_drc()
{
	core_state *const m_core = this->m_core;

	// state may have been edited since the last timeslice
	m_core->mode_dirty = 1;

	if (m_drc_code->report_interval && (machine().time().seconds() >= m_drc_code->next_report))
	{
		m_drc_code->next_report += m_drc_code->report_interval;
		drc_code_report(true);
	}

	while (m_core->cycles > 0)
	{
		if (!drc_runnable())
		{
			step();
			continue;
		}

		if (m_verifier)
			drc_verify_prepare();

		// a reset, a state load, an A20 change or a memory map change marks the cache dirty, and each can happen while
		// an instruction executes
		if (m_drc_cache_dirty)
			drc_flush_cache();

		// blocks containing rewritten bytes have been removed from the lookup, so new entry into the cache is safe
		m_drc_code_modified = false;

		int const result = m_verifier ? drc_verify_execute() : m_drcuml->execute(*m_entry);
		drc_rethrow_exception();

		switch (result)
		{
		case EXECUTE_MISSING_CODE:
			drc_compile_block(m_core->mode, m_core->pc);
			break;

		case EXECUTE_OUT_OF_CYCLES:
			while (m_core->cycles > 0)
				step();
			break;

		case EXECUTE_RESET_CACHE:
			drc_flush_cache();
			break;

		case EXECUTE_INTERPRET:
		case EXECUTE_BLOCK_DONE:
		case EXECUTE_EXCEPTION:
			break;
		}
	}
}
