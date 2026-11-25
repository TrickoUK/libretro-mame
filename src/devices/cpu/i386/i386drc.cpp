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
    end, and executes each of them with step(), the interpreter's
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
      and also when the code has been rewritten since it was described.
    - No C++ exception propagates through generated code, which has no unwind
      information.  A called function runs its body through drc_guarded_call(),
      and generated code calls it through generate_call().  A guest fault
      that generated code is to deliver is caught by the function itself and
      recorded for the fault handler.  Any other exception is kept, generated
      code is left at once, and it is rethrown from the execution loop.

***************************************************************************/

#include "emu.h"
#include "i386.h"
#include "i386priv.h"
#include "i386drcfe.h"

#include "cpu/drcumlsh.h"

#include <cstdlib>

using namespace uml;


namespace {

// size of the code cache; the two-level block lookup table takes 512 KiB for each 64 KiB of linear address space holding code
constexpr size_t DRC_CACHE_SIZE = 128 * 1024 * 1024;

// the front end describes instructions starting within this many bytes of the first, and at most this many in a block
constexpr uint32_t COMPILE_WINDOW_BYTES = 1024;
constexpr uint32_t COMPILE_MAX_SEQUENCE = 64;

// longest range of guest bytes a block may cover, used when invalidating blocks that begin before a written range
constexpr uint32_t COMPILE_MAX_SEQUENCE_BYTES = COMPILE_WINDOW_BYTES + 15;

// UML instructions allowed for each instruction of a block (up to 16 are generated, with the verifier and a
// deliberate error) and for the rest of the block
constexpr uint32_t COMPILE_UML_PER_INSTRUCTION = 24;
constexpr uint32_t COMPILE_UML_PER_BLOCK = 32;

// room for the UML instructions of the longest block
constexpr uint32_t COMPILE_MAX_UML = COMPILE_UML_PER_INSTRUCTION * COMPILE_MAX_SEQUENCE + COMPILE_UML_PER_BLOCK;

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


// Reads a byte of code at a linear address without side effects.  This fails for anything but memory.
bool i386_device::drc_read_code(offs_t address, uint8_t &value)
{
	offs_t const physical = address & m_core->a20_mask;
	unsigned const bytes = m_program->data_width() / 8;
	void const *const unit = m_program->get_read_ptr(physical & ~offs_t(bytes - 1));
	if (!unit)
		return m_verifier && drc_verify_read_code(physical, value);

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
	return (!drc_runnable() || m_drc_cache_dirty) ? DRC_EXIT_INTERPRET : DRC_EXIT_NONE;
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
			});
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
	generate_block_end(block);
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
	bool succeeded = false;
	while (!succeeded)
	{
		try
		{
			drcuml_block &block(m_drcuml->begin_block(COMPILE_MAX_UML));

			UML_HASH(block, mode, pc);                                      // hash    mode,pc
			for (opcode_desc const *desc = first; desc; desc = desc->next())
			{
				bool const last = desc->end_sequence() || !desc->next();
				assert(last || (desc->next()->pc == (desc->pc + desc->length)));
				generate_step_instruction(block, *desc, last, done, slow);
				if (last)
					break;
			}

			UML_LABEL(block, done);                                         // done:
			generate_block_end(block);
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
}


// calls a function that uses drc_guarded_call(), and leaves generated code at once if it caught an exception
void i386_device::generate_call(drcuml_block &block, void (*function)(void *))
{
	UML_CALLC(block, function, this);                                       // callc   function
	UML_CMP(block, mem(&m_core->drc_exit), DRC_EXIT_EXCEPTION);             // cmp     [drc_exit],exception
	block.append().exit(COND_E, EXECUTE_EXCEPTION);                         // exit    EXECUTE_EXCEPTION,e
}


// goes on to the block for the current mode and address
void i386_device::generate_block_end(drcuml_block &block)
{
	// the verifier compares the state after each block
	if (m_verifier)
		UML_EXIT(block, EXECUTE_BLOCK_DONE);                                // exit    EXECUTE_BLOCK_DONE
	else
		UML_HASHJMP(block, mem(&m_core->mode), mem(&m_core->pc), *m_nocode); // hashjmp <mode>,<pc>,nocode
}


// Executes one instruction with the interpreter.  Control goes on to the next instruction of the block only while
// the interpreter loop would run it next: there are cycles left, nothing needs the interpreter, and execution has
// arrived at the address the instruction was compiled at.
void i386_device::generate_step_instruction(drcuml_block &block, opcode_desc const &desc, bool last, uml::code_label done, uml::code_label slow)
{
	UML_CMP(block, mem(&m_core->cycles), 0);                                // cmp     [cycles],0
	UML_EXHc(block, COND_LE, *m_out_of_cycles, desc.pc);                    // exh     out_of_cycles,pc,le

	if (m_verifier)
	{
		UML_MOV(block, mem(&m_core->verify_pc), desc.pc);                   // mov     [verify_pc],pc
		generate_call(block, &i386_device::cfunc_verify_instruction);       // callc   cfunc_verify_instruction
	}

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

	while (m_core->cycles > 0)
	{
		if (!drc_runnable())
		{
			step();
			continue;
		}

		// a reset, a state load or an A20 change marks the cache dirty, and each can happen while an instruction executes
		if (m_drc_cache_dirty)
			drc_flush_cache();

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
