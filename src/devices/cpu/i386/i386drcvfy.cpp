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

    Compiled accesses use the page tables as they do without the verifier.  A
    store of a checked block that goes straight to memory does not pass the
    taps, so a function is called before and after it.  Before, the table
    entry and the index the store uses are compared with those computed afresh
    for the address, and the write is made through the program space, where
    the taps record it as one by the interpreter, and then taken back.  After,
    the memory is compared with what that write left.

    Compiled instructions are compared in the same way: the interpreter
    fetches them from memory, so a block that goes on using bytes which have
    been rewritten differs.  A block must also end with the instruction that
    writes to the bytes of a compiled instruction.  The opcode byte that the
    interpreter keeps while decoding (m_opcode) is not compared, as compiled
    instructions do not set it and it is only used within the instruction.

    Only accesses to plain memory can be repeated.  A block that accesses
    anything else, or any I/O port, is left as the recompiled code executed it
    and is counted as not checked.  Memory is recognised in the first 256 MiB
    and the last 1 MiB of the physical address space.  Reads from the system
    BIOS windows (0xe0000-0xfffff and the last 1 MiB) count as repeatable.
    The contents of a cache modelled by a subclass are not covered.

    Two paths of normal execution are not exercised under the verifier:
    - Blocks may be shaped differently.  Code in a BIOS window that has no
      read pointer is described as single-instruction blocks without the
      verifier, and as longer blocks with it.
    - Normally a block goes straight on to the next one with HASHJMP, while
      with the verifier generated code returns after every block.

    The replay also checks that the block did not execute an instruction
    which the interpreter loop would not have started, or which has to be
    left to the interpreter, or after the code cache had to be flushed.
    In every block, checked or not, each instruction must be started at the
    address it was compiled at, and the cached mode key must agree with the
    segment registers and CR0.  A block which ends by going on to the next must
    leave the CPU in a state in which that may run from the code cache.

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
        mov      inverts a bit of the register written by each compiled MOV
                 between registers, or of an immediate to one, of checked
                 blocks
        load     inverts a bit of the register written by each compiled load
                 of checked blocks
        store    inverts a bit of the value written by each compiled store
                 of checked blocks
        ea       moves the address of each compiled memory access of checked
                 blocks by a dword
        alu      inverts a bit of the register written by each compiled
                 arithmetic, logic, increment or lea instruction of checked
                 blocks
        flags    inverts one of the six arithmetic flags, chosen by the
                 address, after each compiled instruction of checked blocks
                 which sets flags
        carry    clears CF before each compiled adc and sbb of checked blocks
        stale    leaves the flags as they were before each compiled
                 instruction of checked blocks which sets them and is followed
                 by a stepped instruction
        cond     inverts the condition of the compiled conditional jumps of
                 checked blocks whose address is a multiple of three
        jcycles  charges each way of a compiled conditional jump the cycles of
                 the other, and in checked blocks one more for a taken jump,
                 as the two are the same on some CPUs
        rmw      writes back the value read by each compiled instruction of
                 checked blocks which writes its memory operand, other than MOV
        rmwmiss  lets such an instruction go straight to a page which has
                 only a read table entry, such as one holding compiled code.
                 This also applies in blocks that are not checked.
        mcycles  charges each compiled instruction with a memory operand,
                 other than MOV, the cycles of its register form, or one more
                 where the two are the same.  This also applies in blocks that
                 are not checked.
        target   adds one to the target of the direct jumps, calls and loops
                 of checked blocks whose address is a multiple of three
        retaddr  pushes a return address one byte further in the compiled
                 calls of checked blocks
        esp      moves ESP before the stack access of a compiled push or call
                 is known to be possible, in checked blocks, which shows
                 where the instruction is then executed with step()
        ecx      leaves out the decrement of the compiled loops of checked
                 blocks
        noentry  starts a block of more than one instruction without testing
                 for remaining cycles.  This also applies without the
                 verifier, and is only seen without it, as the verifier
                 returns after every block.
        chainirq stays in generated code after a called function has left an
                 interrupt pending or in the interrupt shadow.  This also
                 applies without the verifier, and is only seen without it
                 where execution goes on to another block.
        callcycle
                 goes on after a called function to the next instruction of
                 the block past its test for remaining cycles.  This also
                 applies without the verifier.  It is not seen after port
                 instructions, which end their block, so that the next test is
                 the first of a block and is always made.
        dirtyexit
                 stays in generated code after a called function has left the
                 code cache to be flushed.  This also applies without the
                 verifier, and is only seen without it, as a checked block is
                 left at its end, where the flush happens.
        faultext delivers the faults of compiled instructions as software
                 interrupts, with the external flag clear.  This also applies
                 without the verifier.  It is only seen where the fault is
                 taken above ring 0, as the privilege level of the gate is
                 then tested.
        staleexit
                 tests for an exit after a called function with the mode key as
                 it was before the call.  This also applies without the
                 verifier, which sees it where the block goes on to the next
                 (a change of PE or of a segment register).  A load of SS with
                 interrupts enabled ends generated code all the same, through
                 the interrupt shadow.
        noa20alias
                 with A20 gated off, does not discard the blocks 1 MB above a
                 write to compiled code, which fetch the written bytes.  This
                 also applies without the verifier.
        noa20flush
                 changes the A20 mask without flushing the code cache or the
                 page tables.  This also applies without the verifier.
        nodrop   leaves the write table entry of a page that starts being
                 watched for writes to compiled code.  This also applies
                 without the verifier.
        flushpages
                 keeps the entries of the data page tables when the code cache
                 is flushed.  This also applies without the verifier.
        flushtaps
                 keeps the watched pages and their taps when the code cache is
                 flushed.  This also applies without the verifier, and is only
                 run without it, as the verifier's handling of a change of the
                 memory map drops the taps.
        noretry  takes a block stopped by a full code cache as compiled,
                 without a flush.  This also applies without the verifier, and
                 is only seen where the cache fills.  It is caught only by a
                 crash, so it shows only that the path runs.
        nopostflush
                 loads a state without flushing the code cache, ignoring also
                 the changes of the memory map which other devices make then.
                 This also applies without the verifier, which flushes the
                 cache itself when the memory map changes.
        nomodekey
                 marks the mode key out of date neither when a state is loaded
                 nor when a timeslice starts.  This also applies without the
                 verifier.
        smc      ignores writes to the bytes of compiled instructions, which
                 leaves stale code running
        noexit   goes on with a block after it wrote to compiled code
        x87state inverts a bit of an x87 register after checked blocks
        x87val   inverts the low bit of the registers written by each compiled
                 x87 instruction of checked blocks which writes any; of C0 in
                 the status word after fcom and fcomp with a memory operand,
                 of the control word after fldcw, and of the value to be
                 written by fst, fstp, fistp and fnstcw to memory
        x87pe    clears the precision exception flag after each compiled x87
                 instruction of checked blocks other than fnstsw
        x87tag   undoes the tag word update of each such instruction; after
                 fxch, fchs, fabs and fst to memory, inverts a bit of the tags
                 of the registers named instead
        x87cycles
                 takes one more cycle for each compiled x87 instruction of
                 checked blocks
        x87mf    leaves out the test for a pending x87 exception before the
                 compiled x87 instructions of checked blocks
        x87nm    leaves out the test of CR0.TS and CR0.EM before them
        x87ax    makes the compiled fnstsw of checked blocks write all of EAX
        x87opc   gives the compiled x87 instructions of checked blocks the
                 wrong opcode byte
        x87ptr   inverts a bit of the x87 data pointer after each compiled
                 x87 instruction of checked blocks other than fnstsw (of the
                 instruction pointer after fxch, which leaves the data pointer)
        x87sf    leaves the SoftFloat inexact flag set after each compiled x87
                 instruction of checked blocks
        x87ea    moves the effective address of the compiled x87 loads and
                 stores of checked blocks by four bytes
        x87dsel  records the data segment selector for a compiled x87
                 instruction of checked blocks whose operand is in the stack
                 segment
        x87dptr  records the offset of the operand of a compiled x87
                 instruction of checked blocks plus one
        x87miss  treats a miss of a compiled x87 load of checked blocks as a
                 hit which reads zero, and one of a store as a hit which writes
                 nothing, instead of executing it with step().  This models a
                 miss taken for a hit; no unchecked access is made.
        x87store leaves out the write of the compiled x87 stores of checked
                 blocks
        x87sflag makes that write also where the instruction refuses it
        x87pop   undoes the pop of the compiled fstp and fistp to memory of
                 checked blocks
        x87size  writes only four bytes for a compiled store of a 64-bit
                 operand of checked blocks, and one byte for fnstcw
        x87high  reads the high half of the 64-bit operand of a compiled x87
                 instruction of checked blocks as zero
        x87rc    changes the SoftFloat rounding mode after the compiled fldcw
                 of checked blocks
        x87nopte records the x87 opcode, data pointer and data selector for
                 the compiled fnstcw of checked blocks
        x87wmf   makes a pending exception stop the compiled fnstcw of checked
                 blocks, as a test for #MF would, with nothing done

***************************************************************************/

#include "emu.h"
#include "i386.h"
#include "i386priv.h"
#include "x87priv.h"

#include "softfloat3/source/include/softfloat.h"

#include <cstdlib>
#include <string_view>
#include <type_traits>


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
	int sabotage = DRC_BREAK_NONE;

	// violations seen while generated code was running
	uint32_t misplaced = 0;
	uint32_t misplaced_pc = 0;
	uint32_t misplaced_expected = 0;
	uint32_t stale_modes = 0;

	bool recording = false;
	bool unsafe = false;

	// the arithmetic flags as they were before an instruction, for a deliberate error
	uint8_t saved_flags[6] = { };

	// the store being made by generated code: the dwords it reaches and what they have to hold afterwards
	bool store_pending = false;
	unsigned store_dwords = 0;
	uint32_t store_address[3] = { };
	uint32_t store_result[3] = { };
	bool remap = true;
	// instructions executed when the block wrote to compiled code, zero if it did not
	uint32_t code_written = 0;

	std::vector<region> regions;
	std::vector<uint8_t> before;
	std::vector<uint8_t> after_block;
	std::vector<uint8_t> after_interpreter;
	std::vector<access> writes;
	std::vector<access> block_writes;

	// per page: bit 0 set if reads can be repeated, bit 1 set if writes go to memory, bit 2 set if reads can be repeated
	// but do not come from memory, bits 3 and 4 set if all of the page can be read, or written, through one pointer
	std::vector<uint8_t> pages;

	// per page: the host memory it is read from and written to, hidden by the taps once they are installed
	std::vector<uint8_t *> pointers;
	std::vector<uint8_t *> write_pointers;

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
	v.add("softfloat rounding", &softfloat_roundingMode, sizeof(softfloat_roundingMode));
	v.add("softfloat exceptions", &softfloat_exceptionFlags, sizeof(softfloat_exceptionFlags));
	v.add("softfloat precision", &extF80_roundingPrecision, sizeof(extF80_roundingPrecision));
	drc_verify_add_state([&v] (char const *name, void *base, size_t length) { v.add(name, base, length); });

	v.pages.resize(VERIFY_LOW_PAGES + VERIFY_HIGH_PAGES);
	v.pointers.resize(v.pages.size());
	v.write_pointers.resize(v.pages.size());
	v.program_notifier = m_program->add_change_notifier([this, &v] (read_or_write) { if (!m_drc_installing_taps) v.remap = true; });
	v.io_notifier = m_io->add_change_notifier([this, &v] (read_or_write) { if (!m_drc_installing_taps) v.remap = true; });

	osd_printf_info("%s: recompiler verifier checking one block in %u\n", tag(), v.interval);

	char const *const breakage = std::getenv("MAME_I386_DRC_VERIFY_BREAK");
	if (breakage)
	{
		static constexpr std::pair<std::string_view, int> NAMES[] = {
				{ "cf", DRC_BREAK_CF },
				{ "cycles", DRC_BREAK_CYCLES },
				{ "eip", DRC_BREAK_EIP },
				{ "write", DRC_BREAK_WRITE },
				{ "overrun", DRC_BREAK_OVERRUN },
				{ "address", DRC_BREAK_ADDRESS },
				{ "mode", DRC_BREAK_MODE },
				{ "throw", DRC_BREAK_THROW },
				{ "mov", DRC_BREAK_MOV },
				{ "smc", DRC_BREAK_SMC },
				{ "noexit", DRC_BREAK_NOEXIT },
				{ "load", DRC_BREAK_LOAD },
				{ "store", DRC_BREAK_STORE },
				{ "ea", DRC_BREAK_EA },
				{ "nodrop", DRC_BREAK_NODROP },
				{ "alu", DRC_BREAK_ALU },
				{ "flags", DRC_BREAK_FLAGS },
				{ "carry", DRC_BREAK_CARRY },
				{ "stale", DRC_BREAK_STALE },
				{ "cond", DRC_BREAK_COND },
				{ "jcycles", DRC_BREAK_JCYCLES },
				{ "rmw", DRC_BREAK_RMW },
				{ "rmwmiss", DRC_BREAK_RMWMISS },
				{ "mcycles", DRC_BREAK_MCYCLES },
				{ "target", DRC_BREAK_TARGET },
				{ "retaddr", DRC_BREAK_RETADDR },
				{ "esp", DRC_BREAK_ESP },
				{ "ecx", DRC_BREAK_ECX },
				{ "noentry", DRC_BREAK_NOENTRY },
				{ "chainirq", DRC_BREAK_CHAINIRQ },
				{ "x87state", DRC_BREAK_X87STATE },
				{ "x87val", DRC_BREAK_X87VAL },
				{ "x87pe", DRC_BREAK_X87PE },
				{ "x87tag", DRC_BREAK_X87TAG },
				{ "x87cycles", DRC_BREAK_X87CYCLES },
				{ "x87mf", DRC_BREAK_X87MF },
				{ "x87nm", DRC_BREAK_X87NM },
				{ "x87ax", DRC_BREAK_X87AX },
				{ "x87opc", DRC_BREAK_X87OPC },
				{ "x87ptr", DRC_BREAK_X87PTR },
				{ "x87sf", DRC_BREAK_X87SF },
				{ "x87ea", DRC_BREAK_X87EA },
				{ "x87dsel", DRC_BREAK_X87DSEL },
				{ "x87dptr", DRC_BREAK_X87DPTR },
				{ "x87miss", DRC_BREAK_X87MISS },
				{ "x87store", DRC_BREAK_X87STORE },
				{ "x87sflag", DRC_BREAK_X87SFLAG },
				{ "x87pop", DRC_BREAK_X87POP },
				{ "x87size", DRC_BREAK_X87SIZE },
				{ "x87rc", DRC_BREAK_X87RC },
				{ "x87nopte", DRC_BREAK_X87NOPTE },
				{ "x87wmf", DRC_BREAK_X87WMF },
				{ "x87high", DRC_BREAK_X87HIGH },
				{ "flushpages", DRC_BREAK_FLUSHPAGES },
				{ "flushtaps", DRC_BREAK_FLUSHTAPS },
				{ "noretry", DRC_BREAK_NORETRY },
				{ "nopostflush", DRC_BREAK_NOPOSTFLUSH },
				{ "callcycle", DRC_BREAK_CALLCYCLE },
				{ "dirtyexit", DRC_BREAK_DIRTYEXIT },
				{ "faultext", DRC_BREAK_FAULTEXT },
				{ "nomodekey", DRC_BREAK_NOMODEKEY },
				{ "staleexit", DRC_BREAK_STALEEXIT },
				{ "noa20alias", DRC_BREAK_NOA20ALIAS },
				{ "noa20flush", DRC_BREAK_NOA20FLUSH } };
		for (auto const &[name, kind] : NAMES)
		{
			if (name == breakage)
				v.sabotage = kind;
		}
		if (v.sabotage == DRC_BREAK_NONE)
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

	if (v.sabotage == DRC_BREAK_THROW)
		throw emu_fatalerror("%s: DRC verify: test exception thrown below generated code\n", tag());

	// an error in a block that is not compared would be left in the guest
	if (!v.recording || v.unsafe)
		return;

	switch (v.sabotage)
	{
	case DRC_BREAK_CF:
		core.CF ^= 1;
		break;

	case DRC_BREAK_CYCLES:
		core.cycles -= 1;
		break;

	case DRC_BREAK_EIP:
		core.eip += 1;
		break;

	case DRC_BREAK_WRITE:
		{
			uint32_t const address = (core.reg.d[ESP] - 4) & ~uint32_t(3);
			m_program->write_dword(address, m_program->read_dword(address));
		}
		break;

	case DRC_BREAK_OVERRUN:
		if (core.verify_count == 1)
		{
			step();
			core.verify_count++;
		}
		break;

	case DRC_BREAK_MODE:
		core.mode ^= 0x100;
		break;
	}
}


// inverts a bit of the register a compiled instruction wrote, at the byte offset left by generated code
void i386_device::drc_verify_break_mov()
{
	drc_verifier const &v = *m_verifier;
	if (v.recording && !v.unsafe)
		m_core->reg.b[m_core->ea_tmp] ^= 1;
}


// inverts one arithmetic flag after a compiled instruction, chosen by generated code
void i386_device::drc_verify_break_flags()
{
	drc_verifier const &v = *m_verifier;
	if (!v.recording || v.unsafe)
		return;
	uint8_t *const flags[6] = { &m_core->CF, &m_core->PF, &m_core->AF, &m_core->ZF, &m_core->SF, &m_core->OF };
	*flags[m_core->ea_tmp % 6] ^= 1;
}


// loses the carry into a compiled adc or sbb
void i386_device::drc_verify_break_carry()
{
	drc_verifier const &v = *m_verifier;
	if (v.recording && !v.unsafe)
		m_core->CF = 0;
}


// the two parts of leaving the flags as they were before a compiled instruction
void i386_device::drc_verify_break_save_flags()
{
	drc_verifier &v = *m_verifier;
	uint8_t const flags[6] = { m_core->CF, m_core->PF, m_core->AF, m_core->ZF, m_core->SF, m_core->OF };
	std::copy(std::begin(flags), std::end(flags), std::begin(v.saved_flags));
}


void i386_device::drc_verify_break_restore_flags()
{
	drc_verifier const &v = *m_verifier;
	if (!v.recording || v.unsafe)
		return;
	uint8_t *const flags[6] = { &m_core->CF, &m_core->PF, &m_core->AF, &m_core->ZF, &m_core->SF, &m_core->OF };
	for (int i = 0; i < 6; i++)
		*flags[i] = v.saved_flags[i];
}


// replaces the value a compiled instruction writes to its memory operand, in arg0, with the value it read, in arg1
void i386_device::drc_verify_break_rmw()
{
	drc_verifier const &v = *m_verifier;
	if (v.recording && !v.unsafe)
		m_core->arg0 = m_core->arg1;
}


// disturbs what a compiled x87 instruction left, given the status and tag words from before it
void i386_device::drc_verify_break_x87(uint8_t opcode, uint8_t modrm, uint16_t status, uint16_t tags)
{
	drc_verifier const &v = *m_verifier;
	if (!v.recording || v.unsafe)
		return;
	core_state &core = *m_core;
	bool const fnstsw = (opcode == 0xdf) && (modrm == 0xe0);
	uint8_t const group = modrm & 0xf8;
	int const sti = modrm & 7;
	bool const fxch = (opcode == 0xd9) && (group == 0xc8);
	bool const sign = (opcode == 0xd9) && ((modrm == 0xe0) || (modrm == 0xe1));   // fchs, fabs
	// the physical numbers of the top of stack before and after the instruction
	int const top = BIT(status, X87_SW_TOP_SHIFT, 3);
	int const new_top = BIT(core.x87_sw, X87_SW_TOP_SHIFT, 3);
	// the forms with a memory operand
	bool const memory = modrm < 0xc0;
	unsigned const reg = BIT(modrm, 3, 3);
	bool const store = drc_x87_memory(opcode, modrm).store;
	bool const store_pop = store && ((opcode == 0xdf) || (reg == 3));
	bool const arithmetic = memory && ((opcode == 0xd8) || (opcode == 0xdc));
	bool const fldcw = memory && (opcode == 0xd9) && (reg == 5);

	switch (v.sabotage)
	{
	case DRC_BREAK_X87VAL:
		{
			auto const invert = [&core] (int reg) { core.x87_reg[reg & 7].signif ^= 1; };
			if (store)
			{
				core.x87_store_value ^= 1;      // the value to be written
			}
			else if (arithmetic && ((reg == 2) || (reg == 3)))
			{
				core.x87_sw ^= X87_SW_C0;       // fcom and fcomp write no register
			}
			else if (arithmetic || ((opcode == 0xd8) || sign))
			{
				invert(top);                    // arithmetic, fchs and fabs write ST(0)
			}
			else if (fldcw)
			{
				core.x87_cw ^= 1;
			}
			else if (memory)
			{
				invert(new_top);                // the loads write the new ST(0)
			}
			else if (((opcode == 0xde) && (modrm != 0xd9)) || (opcode == 0xdd))
			{
				invert(top + sti);              // faddp, fstp and the like write ST(i) as numbered before the pop
			}
			else if (fxch)
			{
				invert(top);
				if (sti)
					invert(top + sti);
			}
			else if (opcode == 0xd9)
			{
				invert(new_top);                // fld, fld1 and fldz write the new ST(0)
			}
		}
		break;

	case DRC_BREAK_X87PE:
		if (!fnstsw)
			core.x87_sw &= ~X87_SW_PE;
		break;

	case DRC_BREAK_X87TAG:
		if (fxch || sign || (store && !store_pop))
		{
			core.x87_tw ^= 1 << X87_TW_FIELD_SHIFT(top);
			if (fxch && sti)
				core.x87_tw ^= 1 << X87_TW_FIELD_SHIFT((top + sti) & 7);
		}
		else if (!fnstsw)
		{
			core.x87_tw = tags;
		}
		break;

	case DRC_BREAK_X87POP:
		if (store_pop)
		{
			core.x87_tw = tags;
			core.x87_sw = (core.x87_sw & ~(X87_SW_TOP_MASK << X87_SW_TOP_SHIFT)) | (status & (X87_SW_TOP_MASK << X87_SW_TOP_SHIFT));
		}
		break;

	case DRC_BREAK_X87CYCLES:
		core.cycles -= 1;
		break;

	case DRC_BREAK_X87AX:
		if (fnstsw)
			core.reg.d[EAX] = core.x87_sw;
		break;

	case DRC_BREAK_X87PTR:
		if (fxch)
			core.x87_inst_ptr ^= 1;
		else if (!fnstsw)
			core.x87_data_ptr ^= 1;
		break;

	case DRC_BREAK_X87SF:
		softfloat_exceptionFlags |= softfloat_flag_inexact;
		break;
	}
}


// changes the rounding mode of SoftFloat: to the nearest even and towards zero change places, and down and up
void i386_device::drc_verify_break_rounding()
{
	drc_verifier const &v = *m_verifier;
	if (v.recording && !v.unsafe)
		softfloat_roundingMode ^= 1;
}


// moves the effective address of a compiled memory access
void i386_device::drc_verify_break_ea()
{
	drc_verifier const &v = *m_verifier;
	if (v.recording && !v.unsafe)
		m_core->ea_tmp += 4;
}


// inverts a bit of the value a compiled instruction stores
void i386_device::drc_verify_break_store(uint32_t &value) const
{
	drc_verifier const &v = *m_verifier;
	if (v.recording && !v.unsafe)
		value ^= 1;
}


// the host memory which compiled instructions may read, or write, directly
uint8_t *i386_device::drc_verify_host_page(offs_t address, bool write) const
{
	drc_verifier const &v = *m_verifier;
	uint8_t const flags = v.page_flags(address);

	// a store is only examined where its reads and writes can both be repeated
	if (write ? ((flags & (1 | 2 | 4 | 16)) != (1 | 2 | 16)) : ((flags & (1 | 4 | 8)) != (1 | 8)))
		return nullptr;
	size_t const page = (address < (VERIFY_LOW_PAGES << 12)) ? (address >> 12) : (VERIFY_LOW_PAGES + ((address - VERIFY_HIGH_BASE) >> 12));
	return write ? v.write_pointers[page] : v.pointers[page];
}


// Called before a store of a checked block that goes straight to memory.  The linear address is in ea_tmp, the value
// in arg0 (for a quadword, in x87_store_value) and the index which the store adds to the anchor in arg1.
template <typename Data>
void i386_device::drc_verify_store_start()
{
	drc_verifier &v = *m_verifier;
	core_state &core = *m_core;
	v.store_pending = false;
	if (!v.recording || v.unsafe)
		return;

	uint32_t const address = core.ea_tmp;
	offs_t const physical = address & core.a20_mask;
	if (!drc_page_write_is_current(address, core.arg1))
	{
		if (v.mismatched < VERIFY_REPORT_LIMIT)
			osd_printf_error("%s: DRC verify at %08X: store to %08X through a wrong page table entry\n", tag(), core.prev_eip, address);
		v.mismatched++;
	}

	if (v.sabotage == DRC_BREAK_STORE)
	{
		if constexpr (sizeof(Data) == 8)
			core.x87_store_value ^= 1;
		else
			core.arg0 ^= 1;
	}

	// make the write where the taps record it, note the result and take it back
	offs_t const first = physical & ~offs_t(3);
	v.store_dwords = ((((physical + sizeof(Data) - 1) & ~offs_t(3)) - first) >> 2) + 1;
	uint32_t old[3];
	v.recording = false;
	for (unsigned i = 0; i < v.store_dwords; i++)
	{
		v.store_address[i] = first + (i * 4);
		old[i] = m_program->read_dword(v.store_address[i]);
	}
	v.recording = true;
	if constexpr (sizeof(Data) == 1)
		WRITE8(address, uint8_t(core.arg0));
	else if constexpr (sizeof(Data) == 2)
		WRITE16(address, uint16_t(core.arg0));
	else if constexpr (sizeof(Data) == 8)
		WRITE64(address, core.x87_store_value);
	else
		WRITE32(address, core.arg0);
	v.recording = false;
	for (unsigned i = 0; i < v.store_dwords; i++)
		v.store_result[i] = m_program->read_dword(v.store_address[i]);
	for (unsigned i = 0; i < v.store_dwords; i++)
		m_program->write_dword(v.store_address[i], old[i]);
	v.recording = true;
	v.store_pending = true;
}

template void i386_device::drc_verify_store_start<uint8_t>();
template void i386_device::drc_verify_store_start<uint16_t>();
template void i386_device::drc_verify_store_start<uint32_t>();
template void i386_device::drc_verify_store_start<uint64_t>();


// called after the store
void i386_device::drc_verify_store_end()
{
	drc_verifier &v = *m_verifier;
	if (!v.store_pending)
		return;
	v.store_pending = false;

	bool const recording = v.recording;
	v.recording = false;
	for (unsigned i = 0; i < v.store_dwords; i++)
	{
		if (m_program->read_dword(v.store_address[i]) != v.store_result[i])
		{
			if (v.mismatched < VERIFY_REPORT_LIMIT)
				osd_printf_error("%s: DRC verify at %08X: store to %08X left the wrong value in memory\n", tag(), m_core->prev_eip, v.store_address[i]);
			v.mismatched++;
			break;
		}
	}
	v.recording = recording;
}


bool i386_device::drc_verify_breaks(int kind) const
{
	return m_verifier && (m_verifier->sabotage == kind);
}


bool i386_device::drc_verify_ignores_address() const
{
	return drc_verify_breaks(DRC_BREAK_ADDRESS);
}


// notes the instruction of a checked block that wrote to compiled code
void i386_device::drc_verify_code_written()
{
	drc_verifier &v = *m_verifier;
	if (v.recording && !v.code_written)
		v.code_written = m_core->verify_count;
}


// Called where a block has ended by going on to the next, which normal execution does without a test: the CPU must
// still be in a state in which it may run from the code cache.
void i386_device::drc_verify_block_end()
{
	drc_verifier &v = *m_verifier;
	if (drc_eligible(drc_mode()))
		return;
	if (v.mismatched < VERIFY_REPORT_LIMIT)
		osd_printf_error("%s: DRC verify at %08X: the block before went on where it must exit\n", tag(), m_core->pc);
	v.mismatched++;
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


// Reads code from a page whose reads repeat: memory while the taps hide its pointer, and the system BIOS windows, so
// that the verifier also sees blocks of several instructions there.
bool i386_device::drc_verify_read_code(offs_t address, uint8_t &value, bool &memory)
{
	drc_verifier &v = *m_verifier;
	if (v.remap)
		drc_verify_map();
	uint8_t const flags = v.page_flags(address);
	if (!(flags & 1))
		return false;
	memory = !(flags & 4);
	value = m_program->read_byte(address);
	return true;
}


// classifies the pages of the program space and installs the taps that record accesses
void i386_device::drc_verify_map()
{
	drc_verifier &v = *m_verifier;

	// pointers cannot be obtained through a tap, including those protecting compiled code, which is then discarded
	if (drc_code_clear())
		m_drc_cache_dirty = true;
	drc_pages_flush();
	m_drc_installing_taps = true;

	v.program_tap.remove();
	v.io_tap.remove();

	for (uint32_t page = 0; page < v.pages.size(); page++)
	{
		uint32_t const base = (page < VERIFY_LOW_PAGES) ? (page << 12) : (VERIFY_HIGH_BASE + ((page - VERIFY_LOW_PAGES) << 12));
		auto *const read = reinterpret_cast<uint8_t *>(m_program->get_read_ptr(base));
		auto *const write = reinterpret_cast<uint8_t *>(m_program->get_write_ptr(base));
		uint8_t flags = 0;
		if (read && (m_program->get_read_ptr(base + 0xffc) == (read + 0xffc)))
			flags |= 1;
		if (write && (m_program->get_write_ptr(base + 0xffc) == (write + 0xffc)))
			flags |= 2;
		// the system BIOS is often a flash device behind a read handler, which reads without side effects
		if (!(flags & 1) && (((base >= 0x000e0000) && (base < 0x00100000)) || (base >= VERIFY_HIGH_BASE)))
			flags |= 1 | 4;
		// compiled accesses need the same pointer test of every dword as without the verifier
		bool whole_read = (flags & 1) && read;
		bool whole_write = (flags & 2) != 0;
		for (uint32_t offset = 4; (offset < 0x1000) && (whole_read || whole_write); offset += 4)
		{
			whole_read = whole_read && (m_program->get_read_ptr(base + offset) == (read + offset));
			whole_write = whole_write && (m_program->get_write_ptr(base + offset) == (write + offset));
		}
		v.pages[page] = flags | (whole_read ? 8 : 0) | (whole_write ? 16 : 0);
		v.pointers[page] = read;
		v.write_pointers[page] = write;
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

	m_drc_installing_taps = false;
	v.remap = false;
}


// classifies the program space again if it has changed, which discards compiled code
void i386_device::drc_verify_prepare()
{
	if (m_verifier->remap)
		drc_verify_map();
}


// runs one block from the code cache and checks it against the interpreter
int i386_device::drc_verify_execute()
{
	drc_verifier &v = *m_verifier;
	if (++v.blocks % v.interval)
		return m_drcuml->execute(*m_entry);

	uint32_t const pc = m_core->pc;
	m_core->verify_count = 0;
	v.code_written = 0;
	v.capture(v.before);
	v.writes.clear();
	v.unsafe = false;
	v.recording = true;
	m_core->verify_checking = 1;

	int const result = m_drcuml->execute(*m_entry);

	m_core->verify_checking = 0;
	v.recording = false;
	drc_rethrow_exception();
	uint32_t const count = m_core->verify_count;

	// the cached mode key, as the block left it
	if (!m_core->mode_dirty && (m_core->mode != drc_mode()))
		v.stale_modes++;
	if (v.code_written && (v.code_written != count))
	{
		if (v.mismatched < VERIFY_REPORT_LIMIT)
			osd_printf_error("%s: DRC verify at %08X: block continued after writing to compiled code\n", tag(), pc);
		v.mismatched++;
	}
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

	// a deliberate error in the x87 state, whatever the instructions of the block
	if (v.sabotage == DRC_BREAK_X87STATE)
		m_core->x87_reg[7].signif ^= 1;

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
				clear(offsetof(core_state, x87_store_value), sizeof(core_state::x87_store_value));
				clear(offsetof(core_state, x87_operand), sizeof(core_state::x87_operand));
				clear(offsetof(core_state, x87_store), sizeof(core_state::x87_store));
				clear(offsetof(core_state, ea_tmp), sizeof(core_state::ea_tmp));
				clear(offsetof(core_state, arg0), sizeof(core_state::arg0));
				clear(offsetof(core_state, arg1), sizeof(core_state::arg1));
				clear(offsetof(core_state, drc_exit), sizeof(core_state::drc_exit));
				clear(offsetof(core_state, verify_count), sizeof(core_state::verify_count));
				clear(offsetof(core_state, verify_pc), sizeof(core_state::verify_pc));
				clear(offsetof(core_state, verify_checking), sizeof(core_state::verify_checking));
				clear(offsetof(core_state, mode_dirty), sizeof(core_state::mode_dirty));

				// the bytes which pad each 80-bit x87 register hold nothing
				for (unsigned i = 0; i < std::extent_v<decltype(core_state::x87_reg)>; i++)
				{
					for (size_t b = 0; b < sizeof(extFloat80_t); b++)
					{
						bool const signif = (b - offsetof(extFloat80_t, signif)) < sizeof(extFloat80_t::signif);
						bool const exponent = (b - offsetof(extFloat80_t, signExp)) < sizeof(extFloat80_t::signExp);
						if (!signif && !exponent)
							clear(offsetof(core_state, x87_reg) + (i * sizeof(extFloat80_t)) + b, 1);
					}
				}
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
