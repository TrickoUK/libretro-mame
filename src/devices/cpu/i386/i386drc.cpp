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

    Each block holds a single instruction and executes it with step(), the
    interpreter's per-instruction function.  The block key is the linear
    address of the instruction together with the mode bits.

    Rules the blocks follow:
    - A block is entered only while the cycles left cover everything before its
      last instruction, so a timeslice ends after the same instruction as in
      the interpreter.  The remaining instructions of a timeslice are executed
      by the interpreter loop.
    - After every call that can run device code, an interrupt or a change to
      the mode ends generated code at the instruction boundary.
    - No C++ exception propagates through generated code.  Called functions
      catch a guest fault and record it, and the fault handler delivers it.

***************************************************************************/

#include "emu.h"
#include "i386.h"
#include "i386priv.h"

#include "cpu/drcumlsh.h"

using namespace uml;


namespace {

// size of the code cache; the two-level block lookup table takes 512 KiB for each 64 KiB of linear address space holding code
constexpr size_t DRC_CACHE_SIZE = 128 * 1024 * 1024;

// longest range of guest bytes a block may cover, used when invalidating blocks that begin before a written range
constexpr uint32_t COMPILE_MAX_SEQUENCE_BYTES = 1024 + 15;

// bits of the mode key
constexpr uint32_t MODE_CODE32 = 1 << 0;
constexpr uint32_t MODE_PROTECTED = 1 << 1;
constexpr uint32_t MODE_FLAT = 1 << 2;
constexpr uint32_t MODE_COUNT = 8;
constexpr uint32_t MODE_COMPILED = MODE_CODE32 | MODE_PROTECTED | MODE_FLAT;

// value of the pending fault field when no fault is pending
constexpr uint32_t NO_FAULT = 0xffffffff;

// reasons for leaving generated code
constexpr int EXECUTE_OUT_OF_CYCLES = 0;
constexpr int EXECUTE_MISSING_CODE = 1;
constexpr int EXECUTE_RESET_CACHE = 2;
constexpr int EXECUTE_INTERPRET = 3;


inline void alloc_handle(drcuml_state &drcuml, uml::code_handle *&handleptr, const char *name)
{
	if (!handleptr)
		handleptr = drcuml.handle_alloc(name);
}

} // anonymous namespace



/***************************************************************************
    SETUP
***************************************************************************/

// creates the recompiler and places the CPU state in the part of the code cache that generated code can address directly
void i386_device::drc_start()
{
	m_drcuml = std::make_unique<drcuml_state>(*this, DRC_CACHE_SIZE + sizeof(core_state));
	m_drcuml->initialize(0, MODE_COUNT, 32, 0, COMPILE_MAX_SEQUENCE_BYTES);

	m_core = m_drcuml->cache_alloc_near<core_state>();
	if (!m_core)
		fatalerror("%s: cannot allocate the CPU state in the recompiler cache\n", tag());

	m_drcuml->symbol_add(&m_core->pc, sizeof(m_core->pc), "pc");
	m_drcuml->symbol_add(&m_core->cycles, sizeof(m_core->cycles), "cycles");
	m_drcuml->symbol_add(&m_core->mode, sizeof(m_core->mode), "mode");
}


void i386_device::drc_flush_cache()
{
	m_drcuml->reset();

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

	// a 16-bit stack segment uses SP rather than ESP, so it is not flat
	if (flat_code && flat_data(DS) && flat_data(ES) && flat_data(SS) && core.sreg[SS].d)
		mode |= MODE_FLAT;

	return mode;
}


// true if the next instruction can be run from the code cache; updates the mode key
bool i386_device::drc_runnable()
{
	core_state &core = *m_core;
	core.mode = drc_mode();
	if (core.mode != MODE_COMPILED)
		return false;
	if ((core.cr[0] & CR0_PG) || core.VM)
		return false;
	if (core.TF || core.RF || (m_dr[7] & 0xff) || core.smi || core.halted || core.delayed_interrupt_enable || core.lock)
		return false;
	return !(core.irq_state && core.IF);
}



/***************************************************************************
    FUNCTIONS CALLED FROM GENERATED CODE
***************************************************************************/

void i386_device::cfunc_step(void *param)
{
	i386_device &cpu = *static_cast<i386_device *>(param);

	// step() handles the faults raised while an instruction executes, but not any raised by the checks that precede it
	try
	{
		cpu.step();
	}
	catch (uint64_t e)
	{
		cpu.m_core->fault = uint32_t(e);
		cpu.m_core->fault_code = uint32_t(e >> 32);
	}

	cpu.m_core->drc_exit = cpu.drc_runnable() ? 0 : 1;
}


void i386_device::cfunc_deliver_fault(void *param)
{
	i386_device &cpu = *static_cast<i386_device *>(param);
	uint32_t const fault = cpu.m_core->fault;
	uint32_t const error = cpu.m_core->fault_code;

	cpu.m_core->fault = NO_FAULT;
	cpu.m_core->ext = 1;
	cpu.i386_trap_with_error(fault, 0, 0, error);

	cpu.m_core->drc_exit = cpu.drc_runnable() ? 0 : 1;
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
	UML_CALLC(block, &i386_device::cfunc_deliver_fault, this);              // callc   cfunc_deliver_fault
	UML_CMP(block, mem(&m_core->drc_exit), 0);                              // cmp     [drc_exit],0
	block.append().exit(COND_NE, EXECUTE_INTERPRET);                        // exit    EXECUTE_INTERPRET,ne
	UML_HASHJMP(block, mem(&m_core->mode), mem(&m_core->pc), *m_nocode);    // hashjmp <mode>,<pc>,nocode
}



/***************************************************************************
    COMPILATION
***************************************************************************/

void i386_device::drc_compile_block(uint32_t mode, offs_t pc)
{
	auto profile = g_profiler.start(PROFILER_DRC_COMPILE);

	bool succeeded = false;
	while (!succeeded)
	{
		try
		{
			drcuml_block &block(m_drcuml->begin_block(32));

			UML_HASH(block, mode, pc);                                      // hash    mode,pc
			generate_step_instruction(block, pc);
			UML_HASHJMP(block, mem(&m_core->mode), mem(&m_core->pc), *m_nocode); // hashjmp <mode>,<pc>,nocode

			block.end();
			succeeded = true;
		}
		catch (drcuml_block::abort_compilation const &)
		{
			drc_flush_cache();
		}
	}
}


// executes one instruction with the interpreter
void i386_device::generate_step_instruction(drcuml_block &block, offs_t pc)
{
	// the block is entered only if the interpreter would start every instruction in it; for a single instruction
	// the cycle cost before the last instruction is zero
	UML_CMP(block, mem(&m_core->cycles), 0);                                // cmp     [cycles],0
	UML_EXHc(block, COND_LE, *m_out_of_cycles, pc);                         // exh     out_of_cycles,pc,le

	UML_CALLC(block, &i386_device::cfunc_step, this);                       // callc   cfunc_step

	UML_CMP(block, mem(&m_core->fault), NO_FAULT);                          // cmp     [fault],none
	UML_EXHc(block, COND_NE, *m_fault_handler, pc);                         // exh     fault,pc,ne
	UML_CMP(block, mem(&m_core->drc_exit), 0);                              // cmp     [drc_exit],0
	block.append().exit(COND_NE, EXECUTE_INTERPRET);                        // exit    EXECUTE_INTERPRET,ne
}



/***************************************************************************
    EXECUTION
***************************************************************************/

void i386_device::execute_run_drc()
{
	core_state *const m_core = this->m_core;

	while (m_core->cycles > 0)
	{
		if (!drc_runnable())
		{
			step();
			continue;
		}

		// a reset or state load marks the cache dirty, and either can happen while an instruction executes
		if (m_drc_cache_dirty)
			drc_flush_cache();

		switch (m_drcuml->execute(*m_entry))
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
			break;
		}
	}
}
