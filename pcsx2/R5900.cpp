// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "OrbisEEDiag.h" // vk-285-100
#include "OrbisEEHle.h" // vk-285-102

#include "common/Path.h"
#include "common/StringUtil.h"
#include "ps2/BiosTools.h"
#include "R5900.h"
#include "R3000A.h"
#include "ps2/pgif.h" // pgif init
#include "VUmicro.h"
#include "COP0.h"
#include "MTVU.h"
#include "VMManager.h"

#include "Hardware.h"
#include "IPU/IPUdma.h"

#include "Elfheader.h"
#include "CDVD/CDVD.h"
#include "Patch.h"
#include "GameDatabase.h"
#include "GSDumpReplayer.h"

#include "DebugTools/Breakpoints.h"
#include "DebugTools/MIPSAnalyst.h"
#include "DebugTools/SymbolGuardian.h"
#include "R5900OpcodeTables.h"

// Orbis: stdout is 1MB-buffered; neuter hot-path fflush in this TU (see
// iR5900.cpp). Crash handler flushes first thing (separate TU).
#undef fflush
#define fflush(f) ((void)0)

#include "fmt/format.h"

using namespace R5900;	// for R5900 disasm tools

s32 EEsCycle;		// used to sync the IOP to the EE
u64 EEoCycle;

alignas(16) cpuRegistersPack _cpuRegistersPack;
alignas(16) tlbs tlb[48];
cachedTlbs_t cachedTlbs;

R5900cpu *Cpu = NULL;

static constexpr uint eeWaitCycles = 3072;

bool eeEventTestIsActive = false;
EE_intProcessStatus eeRunInterruptScan = INT_NOT_RUNNING;
// PS5 port (vk-285-100): true inside _cpuEventTest_Shared's Instant DMA loop (see Vif1_Dma.cpp OrbisVif1Fuse).
bool g_orbis_instant_dma_loop = false;

u32 g_eeloadMain = 0, g_eeloadExec = 0, g_osdsys_str = 0;

// PS5 port (2026-11-16, AI-assisted): the PS2 logo before a fast-booted PS2 disc (R5900.h). On a real PS2, OSDSYS reads
// SYSTEM.CNF's BOOT2 and calls LoadExecPS2("rom0:PS2LOGO", [BOOT2]); PS2LOGO shows the logo stored on the disc, then execs the
// game. Fast boot skips OSDSYS, so here EELOAD's own first call is turned into that launch: "rom0:OSDSYS" in EELOAD is replaced by
// "rom0:PS2LOGO" and eeloadHook2() gives it the arguments {"rom0:PS2LOGO", BOOT2, <launch args>}. PS2LOGO's own LoadExecPS2 of the
// game then comes back through eeloadHook() as in a full boot. Whenever the logo can't be trusted to work (see
// Ps2LogoBootPath()) or PS2LOGO hands back to OSDSYS instead of the game, the game is started directly as before.
bool g_ps2logo_boot = false;
enum class Ps2LogoState : u8
{
	Off,      // not used for this boot (or not yet decided)
	Pending,  // EELOAD's default call was turned into rom0:PS2LOGO; eeloadHook2() still has to give it the arguments
	Running,  // PS2LOGO was started with the game's path
};
static Ps2LogoState s_ps2logo_state = Ps2LogoState::Off;
static bool s_ps2logo_tried = false; // once per boot: a second default EELOAD call (PS2LOGO gave up) starts the game directly
static std::string s_ps2logo_game; // BOOT2, the path PS2LOGO is given

/* I don't know how much space for args there is in the memory block used for args in full boot mode,
but in fast boot mode, the block we use can fit at least 16 argv pointers (varies with BIOS version).
The second EELOAD call during full boot has three built-in arguments ("EELOAD rom0:PS2LOGO <ELF>"),
meaning that only the first 13 game arguments supplied by the user can be added on and passed through.
In fast boot mode, 15 arguments can fit because the only call to EELOAD is "<ELF> <<args>>". */
const int kMaxArgs = 16;
uptr g_argPtrs[kMaxArgs];
#define DEBUG_LAUNCHARG 0 // show lots of helpful console messages as the launch arguments are passed to the game

void cpuReset()
{
	std::memset(&cpuRegs, 0, sizeof(cpuRegs));
	std::memset(&fpuRegs, 0, sizeof(fpuRegs));
	std::memset(&tlb, 0, sizeof(tlb));
	cachedTlbs.count = 0;
	OrbisTidInvalidate(); // 187: the GetThreadId cache was the previous run's (OrbisEEHle.h)

	cpuRegs.pc				= 0xbfc00000; //set pc reg to stack
	cpuRegs.CP0.n.Config	= 0x440;
	cpuRegs.CP0.n.Status.val= 0x70400004; //0x10900000 <-- wrong; // COP0 enabled | BEV = 1 | TS = 1
	cpuRegs.CP0.n.PRid		= 0x00002e20; // PRevID = Revision ID, same as R5900
	fpuRegs.fprc[0]			= 0x00002e30; // fpu Revision..
	fpuRegs.fprc[31]		= 0x01000001; // fpu Status/Control

	cpuRegs.nextEventCycle = cpuRegs.cycle + 4;
	EEsCycle = 0;
	EEoCycle = cpuRegs.cycle;

	psxReset();
	pgifInit();

	extern void Deci2Reset();		// lazy, no good header for it yet.
	Deci2Reset();

	AllowParams1 = !VMManager::Internal::IsFastBootInProgress();
	AllowParams2 = !VMManager::Internal::IsFastBootInProgress();
	ParamsRead = false;

	g_eeloadMain = 0;
	g_eeloadExec = 0;
	g_osdsys_str = 0;
	s_ps2logo_state = Ps2LogoState::Off;
	s_ps2logo_tried = false;
	s_ps2logo_game.clear();

	CBreakPoints::ClearSkipFirst();
}

__ri void cpuException(u32 code, u32 bd)
{
	OrbisTidException(bd); // PS5 port (vk-285-102/103): every exception entry (OrbisEEHle.h)
	bool errLevel2, checkStatus;
	u32 offset = 0;

    cpuRegs.branch = 0;		// Tells the interpreter that an exception occurred during a branch.
	cpuRegs.CP0.n.Cause = code & 0xffff;

	if(cpuRegs.CP0.n.Status.b.ERL == 0)
	{
		//Error Level 0-1
		errLevel2 = false;
		checkStatus = (cpuRegs.CP0.n.Status.b.BEV == 0); //  for TLB/general exceptions

		if (((code & 0x7C) >= 0x8) && ((code & 0x7C) <= 0xC))
			offset = 0x0; //TLB Refill
		else if ((code & 0x7C) == 0x0)
			offset = 0x200; //Interrupt
		else
			offset = 0x180; // Everything else
	}
	else
	{
		//Error Level 2
		errLevel2 = true;
		checkStatus = (cpuRegs.CP0.n.Status.b.DEV == 0); // for perf/debug exceptions

		Console.Error("*PCSX2* FIX ME: Level 2 cpuException");
		if ((code & 0x38000) <= 0x8000 )
		{
			//Reset / NMI
			cpuRegs.pc = 0xBFC00000;
			Console.Warning("Reset request");
			cpuUpdateOperationMode();
			return;
		}
		else if ((code & 0x38000) == 0x10000)
			offset = 0x80; //Performance Counter
		else if ((code & 0x38000) == 0x18000)
			offset = 0x100; //Debug
		else
			Console.Error("Unknown Level 2 Exception!! Cause %x", code);
	}

	if (cpuRegs.CP0.n.Status.b.EXL == 0)
	{
		cpuRegs.CP0.n.Status.b.EXL = 1;
		if (bd)
		{
			Console.Warning("branch delay!!");
			cpuRegs.CP0.n.EPC = cpuRegs.pc - 4;
			cpuRegs.CP0.n.Cause |= 0x80000000;
		}
		else
		{
			cpuRegs.CP0.n.EPC = cpuRegs.pc;
			cpuRegs.CP0.n.Cause &= ~0x80000000;
		}
	}
	else
	{
		offset = 0x180; //Override the cause
		if (errLevel2) Console.Warning("cpuException: Status.EXL = 1 cause %x", code);
	}

	if (checkStatus)
		cpuRegs.pc = 0x80000000 + offset;
	else
		cpuRegs.pc = 0xBFC00200 + offset;

	cpuUpdateOperationMode();
}

void cpuTlbMiss(u32 addr, u32 bd, u32 excode)
{
	// Avoid too much spamming on the interpreter
	if (Cpu != &intCpu || IsDebugBuild) {
		Console.Error("cpuTlbMiss pc:%x, cycl:%llx, addr: %x, status=%x, code=%x",
				cpuRegs.pc, cpuRegs.cycle, addr, cpuRegs.CP0.n.Status.val, excode);
	}

	cpuRegs.CP0.n.BadVAddr = addr;
	cpuRegs.CP0.n.Context &= 0xFF80000F;
	cpuRegs.CP0.n.Context |= (addr >> 9) & 0x007FFFF0;
	cpuRegs.CP0.n.EntryHi = (addr & 0xFFFFE000) | (cpuRegs.CP0.n.EntryHi & 0x1FFF);

	cpuRegs.pc -= 4;
	cpuException(excode, bd);
}

void cpuTlbMissR(u32 addr, u32 bd) {
	cpuTlbMiss(addr, bd, EXC_CODE_TLBL);
}

void cpuTlbMissW(u32 addr, u32 bd) {
	cpuTlbMiss(addr, bd, EXC_CODE_TLBS);
}

// PS5 port (vk-285-90): the bring-up event-rate counters (a modulo and five stores per event test, and a
// counter in CPU_INT, cpuSetEvent and cpuSetNextEventDelta, all printed only under if (0)) are gone; event
// tests are counted once for the [eestat] line (GSRenderer.cpp reads it once a second).
std::atomic<u64> g_orbis_ee_evtests{0};

// sets a branch test to occur some time from an arbitrary starting point.
__fi void cpuSetNextEvent( u64 startCycle, s32 delta )
{
	// typecast the conditional to signed so that things don't blow up
	// if startCycle is greater than our next branch cycle.

	if( (int)(cpuRegs.nextEventCycle - startCycle) > delta )
	{
		cpuRegs.nextEventCycle = startCycle + delta;
	}
}

// sets a branch to occur some time from the current cycle
__fi void cpuSetNextEventDelta( s32 delta )
{
	cpuSetNextEvent( cpuRegs.cycle, delta );
}

__fi int cpuGetCycles(int interrupt)
{
	if(interrupt == VU_MTVU_BUSY && (!THREAD_VU1 || INSTANT_VU1))
		return 1;
	else
	{
		const int cycles = (cpuRegs.sCycle[interrupt] + cpuRegs.eCycle[interrupt]) - cpuRegs.cycle;
		return std::max(1, cycles);
	}

}

// tests the cpu cycle against the given start and delta values.
// Returns true if the delta time has passed.
__fi int cpuTestCycle( u64 startCycle, s32 delta )
{
	// typecast the conditional to signed so that things don't explode
	// if the startCycle is ahead of our current cpu cycle.

	return (int)(cpuRegs.cycle - startCycle) >= delta;
}

// tells the EE to run the branch test the next time it gets a chance.
__fi void cpuSetEvent()
{
	cpuRegs.nextEventCycle = cpuRegs.cycle;
}

// PS5 port (vk-285-99): dmastall changed with whole-word stores. With a constant bit the compiler narrowed
// these to byte ANDs/ORs, and CPU_INT's 32-bit load of the word right after (vif1Interrupt's CPU_INT follows
// TESTINT's clear) couldn't take its value from the byte store: it waited for the store to reach the cache,
// behind every older store in the queue. Same bits.
static __fi void OrbisDmaStallStore(u32 value)
{
	*reinterpret_cast<volatile u32*>(&cpuRegs.dmastall) = value;
}

__fi void cpuClearInt( uint i )
{
	pxAssume( i < 32 );
	cpuRegs.interrupt &= ~(1 << i);
	OrbisDmaStallStore(cpuRegs.dmastall & ~(1u << i)); // vk-285-99
}

static __fi void TESTINT( u8 n, void (*callback)() )
{
	if( !(cpuRegs.interrupt & (1 << n)) ) return;

	if(CHECK_INSTANTDMAHACK || cpuTestCycle( cpuRegs.sCycle[n], cpuRegs.eCycle[n] ) )
	{
		cpuClearInt( n );
		callback();
	}
	else
		cpuSetNextEvent( cpuRegs.sCycle[n], cpuRegs.eCycle[n] );
}

// [TODO] move this function to Dmac.cpp, and remove most of the DMAC-related headers from
// being included into R5900.cpp.
static __fi bool _cpuTestInterrupts()
{

	if (!dmacRegs.ctrl.DMAE || (psHu8(DMAC_ENABLER+2) & 1))
	{
		//Console.Write("DMAC Disabled or suspended");
		return false;
	}

	eeRunInterruptScan = INT_RUNNING;

	while (eeRunInterruptScan == INT_RUNNING)
	{
		/* These are 'pcsx2 interrupts', they handle asynchronous stuff
		   that depends on the cycle timings */
		TESTINT(VU_MTVU_BUSY, MTVUInterrupt);
		TESTINT(DMAC_VIF1, vif1Interrupt);
		TESTINT(DMAC_GIF, gifInterrupt);
		TESTINT(DMAC_SIF0, EEsif0Interrupt);
		TESTINT(DMAC_SIF1, EEsif1Interrupt);
		// Profile-guided Optimization (sorta)
		// The following ints are rarely called.  Encasing them in a conditional
		// as follows helps speed up most games.

		if (cpuRegs.interrupt & ((1 << DMAC_VIF0) | (1 << DMAC_FROM_IPU) | (1 << DMAC_TO_IPU)
			| (1 << DMAC_FROM_SPR) | (1 << DMAC_TO_SPR) | (1 << DMAC_MFIFO_VIF) | (1 << DMAC_MFIFO_GIF)
			| (1 << VIF_VU0_FINISH) | (1 << VIF_VU1_FINISH) | (1 << IPU_PROCESS)))
		{
			TESTINT(DMAC_VIF0, vif0Interrupt);

			TESTINT(DMAC_FROM_IPU, ipu0Interrupt);
			TESTINT(DMAC_TO_IPU, ipu1Interrupt);
			TESTINT(IPU_PROCESS, ipuCMDProcess);

			TESTINT(DMAC_FROM_SPR, SPRFROMinterrupt);
			TESTINT(DMAC_TO_SPR, SPRTOinterrupt);

			TESTINT(DMAC_MFIFO_VIF, vifMFIFOInterrupt);
			TESTINT(DMAC_MFIFO_GIF, gifMFIFOInterrupt);

			TESTINT(VIF_VU0_FINISH, vif0VUFinish);
			TESTINT(VIF_VU1_FINISH, vif1VUFinish);
		}

		if (eeRunInterruptScan == INT_REQ_LOOP)
			eeRunInterruptScan = INT_RUNNING;
		else
			break;
	}

	eeRunInterruptScan = INT_NOT_RUNNING;

	if ((cpuRegs.interrupt & 0x1FFFF) & ~cpuRegs.dmastall)
		return true;
	else
		return false;
}

static __fi void _cpuTestTIMR()
{
	cpuRegs.CP0.n.Count += cpuRegs.cycle - cpuRegs.lastCOP0Cycle;
	cpuRegs.lastCOP0Cycle = cpuRegs.cycle;

	// fixme: this looks like a hack to make up for the fact that the TIMR
	// doesn't yet have a proper mechanism for setting itself up on a nextEventCycle.
	// A proper fix would schedule the TIMR to trigger at a specific cycle anytime
	// the Count or Compare registers are modified.

	if ( (cpuRegs.CP0.n.Status.val & 0x8000) &&
		cpuRegs.CP0.n.Count >= cpuRegs.CP0.n.Compare && cpuRegs.CP0.n.Count < cpuRegs.CP0.n.Compare+1000 )
	{
		Console.WriteLn( Color_Magenta, "timr intr: %x, %x", cpuRegs.CP0.n.Count, cpuRegs.CP0.n.Compare);
		cpuException(0x808000, cpuRegs.branch);
	}
}

static __fi void _cpuTestPERF()
{
	// Perfs are updated when read by games (COP0's MFC0/MTC0 instructions), so we need
	// only update them at semi-regular intervals to keep cpuRegs.cycle from wrapping
	// around twice on us btween updates.  Hence this function is called from the cpu's
	// Counters update.

	COP0_UpdatePCCR();
}

// Checks the COP0.Status for exception enablings.
// Exception handling for certain modes is *not* currently supported, this function filters
// them out.  Exceptions while the exception handler is active (EIE), or exceptions of any
// level other than 0 are ignored here.

static bool cpuIntsEnabled(int Interrupt)
{
	bool IntType = !!(cpuRegs.CP0.n.Status.val & Interrupt); //Choose either INTC or DMAC, depending on what called it

	return IntType && cpuRegs.CP0.n.Status.b.EIE && cpuRegs.CP0.n.Status.b.IE &&
		!cpuRegs.CP0.n.Status.b.EXL && (cpuRegs.CP0.n.Status.b.ERL == 0);
}

// PS5 port (vk-285-98): the lines an event test's VIF1, GIF and DMA handlers read, asked for at its start,
// so their misses overlap each other and the IOP's block instead of queueing one behind the other.
// vk-285-97's Shadow of the Colossus EE profile had ~3% of the thread right after loads of these (VIF1_STAT,
// GIF_STAT, D1_CHCR, the VIF1 state) at ~0.7 million event tests and ~2.3 million VIF1 interrupts a
// second. Only with a DMA or MTVU interrupt pending. Needs proper testing.
static __fi void OrbisPrefetchEventLines()
{
	if (!(cpuRegs.interrupt & ((1u << DMAC_VIF1) | (1u << DMAC_GIF) | (1u << VU_MTVU_BUSY))))
		return;
	const char* const hw = reinterpret_cast<const char*>(eeHw);
	_mm_prefetch(hw + 0x3c00, _MM_HINT_T0); // VIF1 STAT, FBRST, ERR, MARK
	_mm_prefetch(hw + 0x3c40, _MM_HINT_T0); // VIF1 CYCLE, MODE, NUM, MASK
	_mm_prefetch(hw + 0x3c80, _MM_HINT_T0); // VIF1 CODE, ITOPS, BASE, OFST
	_mm_prefetch(hw + 0x3cc0, _MM_HINT_T0); // VIF1 TOPS, ITOP, TOP
	_mm_prefetch(hw + 0x9000, _MM_HINT_T0); // D1 CHCR, MADR, QWC, TADR
	_mm_prefetch(hw + 0x3000, _MM_HINT_T0); // GIF CTRL, MODE, STAT
	_mm_prefetch(hw + 0xe000, _MM_HINT_T0); // D_CTRL, D_STAT, D_PCR, D_SQWC
	_mm_prefetch(hw + 0xf500, _MM_HINT_T0); // D_ENABLER (0xf520)
	const char* const v = reinterpret_cast<const char*>(&vif1);
	_mm_prefetch(v, _MM_HINT_T0);
	_mm_prefetch(v + 64, _MM_HINT_T0);
	_mm_prefetch(v + 128, _MM_HINT_T0);
}

// PS5 port (vk-285-98): the EE-to-IOP clock ratio below, divided once per PSXCLK (it changes only with the
// PS1 mode) instead of once per event test.
static u32 s_orbis_ratio_psxclk = 0;
static float s_orbis_ratio = 8.0f;

// Shared portion of the branch test, called from both the Interpreter
// and the recompiler.  (moved here to help alleviate redundant code)
__fi void _cpuEventTest_Shared()
{
	OrbisPrefetchEventLines(); // vk-285-98
	eeEventTestIsActive = true;
	cpuRegs.nextEventCycle = cpuRegs.cycle + eeWaitCycles;
	cpuRegs.lastEventCycle = cpuRegs.cycle;
	// ---- INTC / DMAC (CPU-level Exceptions) -----------------
	// Done first because exceptions raised during event tests need to be postponed a few
	// cycles (fixes Grandia II [PAL], which does a spin loop on a vsync and expects to
	// be able to read the value before the exception handler clears it).

	uint mask = intcInterrupt() | dmacInterrupt();
	ORBIS_EEDIAG(if (cpuRegs.interrupt & ((1u << DMAC_VIF1) | (1u << DMAC_GIF) | (1u << VU_MTVU_BUSY))) OrbisEEDiag::ev_dma++); // vk-285-100
	if (cpuIntsEnabled(mask))
	{
		ORBIS_EEDIAG(OrbisEEDiag::exc_intc += (mask & 0x400) != 0; OrbisEEDiag::exc_dmac += (mask & 0x800) != 0); // vk-285-100
		cpuException(mask, cpuRegs.branch);
	}

	// ---- IOP -------------
	// * It's important to run a iopEventTest before calling ExecuteBlock. This
	//   is because the IOP does not always perform branch tests before returning
	//   (during the prev branch) and also so it can act on the state the EE has
	//   given it before executing any code.
	//
	// * The IOP cannot always be run.  If we run IOP code every time through the
	//   cpuEventTest, the IOP generally starts to run way ahead of the EE.

	// It's also important to sync up the IOP before updating the timers, since gates will depend on starting/stopping in the right place!
	EEsCycle += cpuRegs.cycle - EEoCycle;
	EEoCycle = cpuRegs.cycle;

	if (EEsCycle > 0)
		iopEventAction = true;

	if (iopEventAction)
	{
		//if( EEsCycle < -450 )
		//	Console.WriteLn( " IOP ahead by: %d cycles", -EEsCycle );

		EEsCycle = psxCpu->ExecuteBlock(EEsCycle);

		iopEventAction = false;
	}

	iopEventTest();

	if (cpuTestCycle(nextStartCounter, nextDeltaCounter))
	{
		ORBIS_EEDIAG(OrbisEEDiag::rcnt++); // vk-285-100
		rcntUpdate();
		_cpuTestPERF();
	}

	_cpuTestTIMR();

	// ---- Interrupts -------------
	// These are basically just DMAC-related events, which also piggy-back the same bits as
	// the PS2's own DMA channel IRQs and IRQ Masks.

	if (cpuRegs.interrupt)
	{
		// This is a BIOS hack because the coding in the BIOS is terrible but the bug is masked by Data Cache
		// where a DMA buffer is overwritten without waiting for the transfer to end, which causes the fonts to get all messed up
		// so to fix it, we run all the DMA's instantly when in the BIOS.
		// Only use the lower 17 bits of the cpuRegs.interrupt as the upper bits are for VU0/1 sync which can't be done in a tight loop
		if (CHECK_INSTANTDMAHACK && dmacRegs.ctrl.DMAE && !(psHu8(DMAC_ENABLER + 2) & 1) && (cpuRegs.interrupt & 0x1FFFF))
		{
			g_orbis_instant_dma_loop = true; // vk-285-100 (Vif1_Dma.cpp OrbisVif1Fuse)
			while ((cpuRegs.interrupt & 0x1FFFF) && _cpuTestInterrupts())
				;
			g_orbis_instant_dma_loop = false;
		}
		else
			_cpuTestInterrupts();
	}

	// ---- VU Sync -------------
	// We're in a EventTest.  All dynarec registers are flushed
	// so there is no need to freeze registers here.
	CpuVU0->ExecuteBlock();
	CpuVU1->ExecuteBlock();

	// ---- Schedule Next Event Test --------------
	if (s_orbis_ratio_psxclk != PSXCLK) // vk-285-98
	{
		s_orbis_ratio_psxclk = PSXCLK;
		s_orbis_ratio = static_cast<float>(PS2CLK) / static_cast<float>(PSXCLK);
	}
	const float mutiplier = s_orbis_ratio;
	const int nextIopEventDeta = ((psxRegs.iopNextEventCycle - psxRegs.cycle) * mutiplier);
	// 8 or more cycles behind and there's an event scheduled
	g_orbis_ee_evtests.store(g_orbis_ee_evtests.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); // vk-285-90
	if (EEsCycle >= nextIopEventDeta)
	{
		// EE's running way ahead of the IOP still, so we should branch quickly to give the
		// IOP extra timeslices in short order.

		ORBIS_EEDIAG(OrbisEEDiag::iop_rapid++); // vk-285-100
		cpuSetNextEventDelta(48);
		//Console.Warning( "EE ahead of the IOP -- Rapid Event!  %d", EEsCycle );
	}
	else
	{
		// Otherwise IOP is caught up/not doing anything so we can wait for the next event.
		cpuSetNextEventDelta(((psxRegs.iopNextEventCycle - psxRegs.cycle) * mutiplier) - EEsCycle);
	}

	// Apply vsync and other counter nextCycles
	cpuSetNextEvent(nextStartCounter, nextDeltaCounter);

	/* Ratchet & Clank SCES-50916 has a pure guest-side wait at 0x00118cc0:
	 * lw v0,0(sp); beq v0,zero,loop. Fast-forward only while the flag is
	 * zero; the next scheduled event still runs before checking it again. */
	if (EmuConfig.Speedhacks.WaitLoop &&
		(cpuRegs.pc == 0x00118cc0 || cpuRegs.pc == 0x00118cb0) &&
		cpuRegs.GPR.r[2].UL[0] == 0 && cpuRegs.nextEventCycle > cpuRegs.cycle)
	{
		cpuRegs.cycle = cpuRegs.nextEventCycle;
	}

	eeEventTestIsActive = false;
}

__ri void cpuTestINTCInts()
{
	// Check the COP0's Status register for general interrupt disables, and the 0x400
	// bit (which is INTC master toggle).
	if (!cpuIntsEnabled(0x400))
		return;

	if ((psHu32(INTC_STAT) & psHu32(INTC_MASK)) == 0)
		return;

	cpuSetNextEventDelta(4);
	if (eeEventTestIsActive && (psxRegs.iopCycleEE > 0))
	{
		psxRegs.iopBreak += psxRegs.iopCycleEE; // record the number of cycles the IOP didn't run.
		psxRegs.iopCycleEE = 0;
	}
}

__fi void cpuTestDMACInts()
{
	// Check the COP0's Status register for general interrupt disables, and the 0x800
	// bit (which is the DMAC master toggle).
	if (!cpuIntsEnabled(0x800))
		return;

	if (((psHu16(0xe012) & psHu16(0xe010)) == 0) &&
		((psHu16(0xe010) & 0x8000) == 0))
		return;

	cpuSetNextEventDelta(4);
	if (eeEventTestIsActive && (psxRegs.iopCycleEE > 0))
	{
		psxRegs.iopBreak += psxRegs.iopCycleEE; // record the number of cycles the IOP didn't run.
		psxRegs.iopCycleEE = 0;
	}
}

__fi void cpuTestTIMRInts()
{
	if ((cpuRegs.CP0.n.Status.val & 0x10007) == 0x10001)
	{
		_cpuTestPERF();
		_cpuTestTIMR();
	}
}

__fi void cpuTestHwInts()
{
	cpuTestINTCInts();
	cpuTestDMACInts();
	cpuTestTIMRInts();
}

__fi void CPU_SET_DMASTALL(EE_EventType n, bool set)
{
	// vk-285-99: whole-word stores (see OrbisDmaStallStore).
	if (set)
		OrbisDmaStallStore(cpuRegs.dmastall | (1u << n));
	else
		OrbisDmaStallStore(cpuRegs.dmastall & ~(1u << n));
}

__fi void CPU_INT( EE_EventType n, s32 ecycle)
{
	ORBIS_EEDIAG(OrbisEEDiag::cpu_int[n & 31]++); // vk-285-100
	// If it's retunning too quick, just rerun the DMA, there's no point in running the EE for < 4 cycles.
	// This causes a huge uplift in performance for ONI FMV's.
	if (ecycle < 4 && !(cpuRegs.dmastall & (1 << n)) && eeRunInterruptScan != INT_NOT_RUNNING)
	{
		eeRunInterruptScan = INT_REQ_LOOP;
		cpuRegs.interrupt |= 1 << n;
		cpuRegs.sCycle[n] = cpuRegs.cycle;
		cpuRegs.eCycle[n] = 0;
		return;
	}

	// EE events happen 8 cycles in the future instead of whatever was requested.
	// This can be used on games with PATH3 masking issues for example, or when
	// some FMV look bad.
	if (CHECK_EETIMINGHACK && n < VIF_VU0_FINISH)
		ecycle = 8;

	cpuRegs.interrupt |= 1 << n;
	cpuRegs.sCycle[n] = cpuRegs.cycle;
	cpuRegs.eCycle[n] = ecycle;

	// Interrupt is happening soon: make sure both EE and IOP are aware.

	if (ecycle <= 28 && psxRegs.iopCycleEE > 0)
	{
		// If running in the IOP, force it to break immediately into the EE.
		// the EE's branch test is due to run.

		psxRegs.iopBreak += psxRegs.iopCycleEE; // record the number of cycles the IOP didn't run.
		psxRegs.iopCycleEE = 0;
	}

	cpuSetNextEventDelta(cpuRegs.eCycle[n]);
}

// Count arguments, save their starting locations, and replace the space separators with null terminators so they're separate strings
int ParseArgumentString(u32 arg_block)
{
	if (!arg_block)
		return 0;

	int argc = 0;
	bool wasSpace = true; // status of last char. scanned
	int args_len = strlen((char *)PSM(arg_block));
	for (int i = 0; i < args_len; i++)
	{
		char curchar = *(char *)PSM(arg_block + i);
		if (curchar == '\0')
			break; // should never reach this

		bool isSpace = (curchar == ' ');
		if (isSpace)
			memset(PSM(arg_block + i), 0, 1);
		else if (wasSpace) // then we're at a new arg
		{
			if (argc < kMaxArgs)
			{
				g_argPtrs[argc] = arg_block + i;
				argc++;
			}
			else
			{
				Console.WriteLn("ParseArgumentString: Discarded additional arguments beyond the maximum of %d.", kMaxArgs);
				break;
			}
		}
		wasSpace = isSpace;
	}
#if DEBUG_LAUNCHARG
	// Check our args block
	Console.WriteLn("ParseArgumentString: Saving these strings:");
	for (int a = 0; a < argc; a++)
		Console.WriteLn("%p -> '%s'.", g_argPtrs[a], (char *)PSM(g_argPtrs[a]));
#endif
	return argc;
}

// PS5 port (2026-11-16, AI-assisted): the BIOS image holds a file of this name (its ROMDIR, as BiosTools.cpp reads it).
// OpenPS2BIOS has no PS2LOGO, so it is never used with that BIOS.
static bool BiosHasRomFile(const char* name)
{
	const size_t size = BiosRom.size();
	size_t dir = size;
	for (size_t i = 0; i + 16 <= size && i < 512 * 1024; i += 16)
	{
		if (std::memcmp(&BiosRom[i], "RESET\0", 6) == 0)
		{
			dir = i;
			break;
		}
	}
	for (size_t i = dir; i + 16 <= size; i += 16)
	{
		const char* entry = reinterpret_cast<const char*>(&BiosRom[i]);
		if (entry[0] == '\0' || strnlen(entry, 10) == 10)
			break;
		if (std::strncmp(entry, name, 10) == 0)
			return true;
	}
	return false;
}

// The address of EELOAD's call to ExecPS2(), as the recompilers find it (iR5900.cpp recRecompile); 0 for an EELOAD they don't know.
static u32 EeloadExecAddress()
{
	const u32 typeAexecjump = memRead32(EELOAD_START + 0x470);
	const u32 typeBexecjump = memRead32(EELOAD_START + 0x5B0);
	const u32 typeCexecjump = memRead32(EELOAD_START + 0x618);
	const u32 typeDexecjump = memRead32(EELOAD_START + 0x600);
	if ((typeBexecjump >> 26 == 3) || (typeCexecjump >> 26 == 3) || (typeDexecjump >> 26 == 3))
		return EELOAD_START + 0x2B8;
	if (typeAexecjump >> 26 == 3)
		return EELOAD_START + 0x170;
	return 0;
}

// The BIOS region (BiosTools.cpp numbering) a disc serial's prefix asks for, as ps5/coreorbis/main-boot.cpp's BIOS-by-region
// choice reads it; -1 unknown.
static int Ps2LogoSerialRegion(const std::string& serial)
{
	if (serial.size() < 4)
		return -1;
	const std::string p = StringUtil::toUpper(serial.substr(0, 4));
	if (p == "SLUS" || p == "SCUS" || p == "PBPX" || p == "LSP-" || p == "LSP_")
		return 1;
	if (p == "SLES" || p == "SCES" || p == "SCED" || p == "SLED")
		return 2;
	if (p == "SCAJ" || p == "SLAJ" || p == "SCKA" || p == "SLKA")
		return 4;
	if (p == "SLPM" || p == "SLPS" || p == "SCPS" || p == "SCPM")
		return 0;
	return -1;
}

// The game path to give PS2LOGO for this fast boot, or empty when the game should start directly. The logo is decoded by
// PS2LOGO with the BIOS's region (a PAL disc on a USA BIOS comes out scrambled), it is read from the disc's first 12
// sectors (a homebrew image or an HDD-loader rip often has none), and it can't be skipped or timed out from here, so it is
// only used when all of this holds.
static std::string Ps2LogoBootPath(const std::string& boot2, const std::string& serial)
{
	const char* why = nullptr;
	if (!g_ps2logo_boot)
		return {};
	if (s_ps2logo_tried)
		why = "PS2LOGO already ran once this boot without starting the game";
	else if (!VMManager::Internal::GetELFOverride().empty())
		why = "an ELF is being started";
	else if (EmuConfig.EnableFastBootFastForward)
		why = "fast boot fast-forward is on";
	else if (!EmuConfig.CurrentGameArgs.empty() && EmuConfig.CurrentGameArgs.size() > 64)
		why = "the launch arguments are too long";
	else if (!StringUtil::StartsWithNoCase(boot2, "cdrom0:") || boot2.find(' ') != std::string::npos || boot2.size() > 64)
		why = "SYSTEM.CNF's BOOT2 is not a plain cdrom0: path";
	else if (!BiosHasRomFile("PS2LOGO"))
		why = "the BIOS has no PS2LOGO";
	else if (EeloadExecAddress() == 0)
		why = "this BIOS's EELOAD is not one the launch hook knows";
	else
	{
		int bios_region = static_cast<int>(BiosRegion);
		if (bios_region == 3 || bios_region == 5) // Oceania, Russia: European discs
			bios_region = 2;
		else if (bios_region == 7) // Mexico: American discs
			bios_region = 1;
		const int disc_region = Ps2LogoSerialRegion(serial);
		if (disc_region < 0)
			why = "the disc's region is not known from its serial";
		else if (disc_region != bios_region)
			why = "the disc's region is not the BIOS's";
		else
		{
			// The logo: sectors 0-11 of the disc. All zero = no logo to show.
			bool any = false;
			u8 sector[2048];
			u8 first = 0;
			for (u32 lsn = 0; lsn < 12 && !any; lsn++)
			{
				if (DoCDVDreadSector(sector, lsn, CDVD_MODE_2048) != 0)
				{
					why = "the disc's logo sectors can't be read";
					break;
				}
				for (u8 b : sector)
					any = any || (b != 0);
				if (lsn == 0)
					first = sector[0];
			}
			if (!why && any)
			{
				u8 key[16];
				cdvdReadKey(0, 0, 75, key);
				Console.WriteLn(fmt::format("PS2 logo: disc key byte {:02x}, logo's first byte {:02x}", key[4], first));
			}
			if (!why && !any)
				why = "the disc has no logo (its first sectors are empty)";
		}
	}
	if (why)
	{
		Console.WriteLn(Color_StrongOrange, fmt::format("PS2 logo: not shown, the game starts directly ({})", why));
		return {};
	}
	return boot2;
}

// Called from recompilers; define is mandatory.
void eeloadHook()
{
	std::string elfname;
	int argc = cpuRegs.GPR.n.a0.SD[0];
	if (argc) // calls to EELOAD *after* the first one during the startup process will come here
	{
#if DEBUG_LAUNCHARG
		Console.WriteLn("eeloadHook: EELOAD was called with %d arguments according to $a0 and %d according to vargs block:",
			argc, memRead32(cpuRegs.GPR.n.a1.UD[0] - 4));
		for (int a = 0; a < argc; a++)
			Console.WriteLn("argv[%d]: %p -> %p -> '%s'", a, cpuRegs.GPR.n.a1.UL[0] + (a * 4),
				memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4)), (char *)PSM(memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4))));
#endif
		if (argc > 1)
			elfname = (char*)PSM(memRead32(cpuRegs.GPR.n.a1.UD[0] + 4)); // argv[1] in OSDSYS's invocation "EELOAD <game ELF>"

		// This code fires if the user chooses "full boot". First the Sony Computer Entertainment screen appears. This is the result
		// of an EELOAD call that does not want to accept launch arguments (but we patch it to do so in eeloadHook2() in fast boot
		// mode). Then EELOAD is called with the argument "rom0:PS2LOGO". At this point, we do not need any additional tricks
		// because EELOAD is now ready to accept launch arguments. So in full-boot mode, we simply wait for PS2LOGO to be called,
		// then we add the desired launch arguments. PS2LOGO passes those on to the game itself as it calls EELOAD a third time.
		if (!EmuConfig.CurrentGameArgs.empty() && elfname == "rom0:PS2LOGO")
		{
			const char *argString = EmuConfig.CurrentGameArgs.c_str();
			Console.WriteLn("eeloadHook: Supplying launch argument(s) '%s' to module '%s'...", argString, elfname.c_str());

			// Join all arguments by space characters so they can be processed as one string by ParseArgumentString(), then add the
			// user's launch arguments onto the end
			u32 arg_ptr = 0;
			int arg_len = 0;
			for (int a = 0; a < argc; a++)
			{
				arg_ptr = memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4));
				arg_len = strlen((char *)PSM(arg_ptr));
				memset(PSM(arg_ptr + arg_len), 0x20, 1);
			}
			strcpy((char *)PSM(arg_ptr + arg_len + 1), EmuConfig.CurrentGameArgs.c_str());
			u32 first_arg_ptr = memRead32(cpuRegs.GPR.n.a1.UD[0]);
#if DEBUG_LAUNCHARG
			Console.WriteLn("eeloadHook: arg block is '%s'.", (char *)PSM(first_arg_ptr));
#endif
			argc = ParseArgumentString(first_arg_ptr);

			// Write pointer to next slot in $a1
			for (int a = 0; a < argc; a++)
				memWrite32(cpuRegs.GPR.n.a1.UD[0] + (a * 4), g_argPtrs[a]);
			cpuRegs.GPR.n.a0.SD[0] = argc;
#if DEBUG_LAUNCHARG
			// Check our work
			Console.WriteLn("eeloadHook: New arguments are:");
			for (int a = 0; a < argc; a++)
				Console.WriteLn("argv[%d]: %p -> '%s'", a, memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4)),
				(char *)PSM(memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4))));
#endif
		}
		// else it's presumed that the invocation is "EELOAD <game ELF> <<launch args>>", coming from PS2LOGO, and we needn't do
		// anything more

		// PS5 port (2026-11-16, AI-assisted): PS2LOGO (started by our fast boot, see Ps2LogoBootPath) handed over to OSDSYS
		// instead of the game (it couldn't take the disc's logo): the game is started directly, as fast boot would have.
		if (s_ps2logo_state == Ps2LogoState::Running && argc > 1 && elfname.starts_with("rom0:OSDSYS"))
		{
			Console.Warning(fmt::format("PS2 logo: PS2LOGO went to {} {} instead of the game; starting {} directly", elfname,
				argc > 2 ? (const char*)PSM(memRead32(cpuRegs.GPR.n.a1.UD[0] + 8)) : "", s_ps2logo_game));
			s_ps2logo_state = Ps2LogoState::Off;
			u32 str = 0;
			for (u32 a = EELOAD_START; a < EELOAD_START + EELOAD_SIZE; a += 8)
			{
				if (!strcmp((char*)PSM(a), "rom0:OSDSYS"))
				{
					str = a;
					break;
				}
			}
			if (str)
			{
				strcpy((char*)PSM(str), s_ps2logo_game.c_str());
				memWrite32(cpuRegs.GPR.n.a1.UD[0] + 4, str);
				cpuRegs.GPR.n.a0.SD[0] = 2;
				elfname = s_ps2logo_game;
			}
		}
		else if (s_ps2logo_state == Ps2LogoState::Running && argc > 1)
		{
			// PS2LOGO's launch of the game (anything later, such as the game going back to OSDSYS, is left alone)
			Console.WriteLn(Color_StrongGreen, fmt::format("PS2 logo: PS2LOGO is starting {}", elfname));
			s_ps2logo_state = Ps2LogoState::Off;
		}
	}
#if DEBUG_LAUNCHARG
	// This code fires in full/fast boot mode when EELOAD is called the first/only time. When EELOAD is not given any arguments,
	// it calls rom0:OSDSYS by default, which displays the Sony Computer Entertainment screen. OSDSYS then calls "EELOAD
	// rom0:PS2LOGO" and we end up above.
	else
		Console.WriteLn("eeloadHook: EELOAD was called with no arguments.");
#endif

	// If "fast boot" was chosen, then on EELOAD's first call we won't yet know what the game's ELF is. Find the name and write it
	// into EELOAD's memory.
	if (VMManager::Internal::IsFastBootInProgress() && elfname.empty())
	{
		const std::string& elf_override = VMManager::Internal::GetELFOverride();
		if (!elf_override.empty())
		{
			// The host: root should be directory containg the elf, so get only the filename part
			elfname = fmt::format("host:{}", Path::GetFileName(elf_override));
		}
		else
		{
			s_ps2logo_state = Ps2LogoState::Off; // a default EELOAD call again (PS2LOGO gave up): not again (s_ps2logo_tried)
			CDVDDiscType disc_type;
			std::string disc_elf, disc_serial;
			cdvdGetDiscInfo(&disc_serial, &disc_elf, nullptr, nullptr, nullptr, &disc_type);
			if (disc_type == CDVDDiscType::PS2Disc)
			{
				// only allow fast boot for PS2 games
				elfname = std::move(disc_elf);
				// PS5 port (2026-11-16, AI-assisted): through rom0:PS2LOGO when it can be (g_ps2logo_boot)
				s_ps2logo_game = Ps2LogoBootPath(elfname, disc_serial);
			}
			else
			{
				Console.Warning(fmt::format("Not allowing fast boot for non-PS2 ELF {}", disc_elf));
			}
		}

		// When fast-booting, we insert the game's ELF name into EELOAD so that the game is called instead of the default call of
		// "rom0:OSDSYS"; any launch arguments supplied by the user will be inserted into EELOAD later by eeloadHook2()
		if (!elfname.empty())
		{
			// Find and save location of default/fallback call "rom0:OSDSYS"; to be used later by eeloadHook2()
			for (g_osdsys_str = EELOAD_START; g_osdsys_str < EELOAD_START + EELOAD_SIZE; g_osdsys_str += 8) // strings are 64-bit aligned
			{
				if (!strcmp((char*)PSM(g_osdsys_str), "rom0:OSDSYS"))
				{
					if (!s_ps2logo_game.empty())
					{
						// PS5 port (2026-11-16, AI-assisted): rom0:PS2LOGO in its place; eeloadHook2() adds the game's path.
						// PS2LOGO's own launch of the game is the next ELF loaded (eeloadHook's argc branch above).
						strcpy((char*)PSM(g_osdsys_str), "rom0:PS2LOGO");
						// The logo on the disc comes through the drive's decryption (CDVD DEC-SET: XOR with key byte 4,
						// then a rotate), and PS2LOGO doesn't read the disc key itself: on a PS2 the key is in the drive
						// from OSDSYS's own disc check before it starts PS2LOGO. Fast boot skips OSDSYS, so the key
						// was never read and PS2LOGO got the logo still encrypted, refused it and went to OSDSYS
						// "BootBrowser" (console logs 2026-11-16: one ReadCD of sectors 0-11, then Exit). The key is
						// read here as that disc check reads it (CdReadKey, cdvdReadKey: from the disc's serial; the game's
						// own ELF load reads it again later).
						cdvdReadKey(0, 0, 75, &cdvd.Key[0]);
						cdvd.KeyXor = 0x00;
						s_ps2logo_state = Ps2LogoState::Pending;
						s_ps2logo_tried = true;
						Console.WriteLn(Color_StrongGreen, fmt::format("PS2 logo: starting rom0:PS2LOGO {}", s_ps2logo_game));
						elfname.clear(); // the game's ELF is loaded later, from PS2LOGO (as in a full boot)
						break;
					}
					// Overwrite OSDSYS with game's ELF name
					strcpy((char*)PSM(g_osdsys_str), elfname.c_str());
					break;
				}
			}
			if (s_ps2logo_state != Ps2LogoState::Pending)
				s_ps2logo_game.clear();
		}
		else
		{
			// Stop fast forwarding if we're doing that for boot.
			VMManager::Internal::DisableFastBoot();
			AllowParams1 = true;
			AllowParams2 = true;
		}
	}

	VMManager::Internal::ELFLoadingOnCPUThread(std::move(elfname));

	if (CHECK_EXTRAMEM)
	{
		// Map extra memory.
		vtlb_VMap(Ps2MemSize::MainRam, Ps2MemSize::MainRam, Ps2MemSize::ExtraRam);

		// Map RAM mirrors for extra memory.
		vtlb_VMap(0x20000000 | Ps2MemSize::MainRam, Ps2MemSize::MainRam, Ps2MemSize::ExtraRam);
		vtlb_VMap(0x30000000 | Ps2MemSize::MainRam, Ps2MemSize::MainRam, Ps2MemSize::ExtraRam);
	}
}

// Called from recompilers; define is mandatory.
// Only called if g_SkipBiosHack is true
void eeloadHook2()
{
	// PS5 port (2026-11-16, AI-assisted): the logo boot's PS2LOGO call (eeloadHook): {"rom0:PS2LOGO", BOOT2, <launch args>}, as
	// OSDSYS's LoadExecPS2("rom0:PS2LOGO", [BOOT2]) gives it; PS2LOGO passes the launch arguments on to the game.
	if (s_ps2logo_state == Ps2LogoState::Pending)
	{
		s_ps2logo_state = Ps2LogoState::Running;
		std::string line = "rom0:PS2LOGO " + s_ps2logo_game;
		if (!EmuConfig.CurrentGameArgs.empty())
			line += " " + EmuConfig.CurrentGameArgs;
		strcpy((char*)PSM(g_osdsys_str), line.c_str());
		const int argc = ParseArgumentString(g_osdsys_str);
		const uptr block_start = g_osdsys_str - (argc * 4);
		for (int a = 0; a < argc; a++)
			memWrite32(block_start + (a * 4), g_argPtrs[a]);
		cpuRegs.GPR.n.a0.SD[0] = argc;
		cpuRegs.GPR.n.a1.UD[0] = block_start;
		// This EELOAD's strings are not used again: a later EELOAD (PS2LOGO's launch of the game) is a fresh copy, and its
		// call here must not be given these arguments.
		g_osdsys_str = 0;
		return;
	}

	if (EmuConfig.CurrentGameArgs.empty())
		return;

	if (!g_osdsys_str)
	{
		Console.WriteLn("eeloadHook2: Called before \"rom0:OSDSYS\" was found by eeloadHook()!");
		return;
	}

	const char *argString = EmuConfig.CurrentGameArgs.c_str();
	Console.WriteLn("eeloadHook2: Supplying launch argument(s) '%s' to ELF '%s'.", argString, (char *)PSM(g_osdsys_str));

	// Add args string after game's ELF name that was written over "rom0:OSDSYS" by eeloadHook(). In between the ELF name and args
	// string we insert a space character so that ParseArgumentString() has one continuous string to process.
	int game_len = strlen((char *)PSM(g_osdsys_str));
	memset(PSM(g_osdsys_str + game_len), 0x20, 1);
	strcpy((char *)PSM(g_osdsys_str + game_len + 1), EmuConfig.CurrentGameArgs.c_str());
#if DEBUG_LAUNCHARG
	Console.WriteLn("eeloadHook2: arg block is '%s'.", (char *)PSM(g_osdsys_str));
#endif
	int argc = ParseArgumentString(g_osdsys_str);

	// Back up 4 bytes from start of args block for every arg + 4 bytes for start of argv pointer block, write pointers
	uptr block_start = g_osdsys_str - (argc * 4);
	for (int a = 0; a < argc; a++)
	{
#if DEBUG_LAUNCHARG
		Console.WriteLn("eeloadHook2: Writing address %p to location %p.", g_argPtrs[a], block_start + (a * 4));
#endif
		memWrite32(block_start + (a * 4), g_argPtrs[a]);
	}

	// Save argc and argv as incoming arguments for EELOAD function which calls ExecPS2()
#if DEBUG_LAUNCHARG
	Console.WriteLn("eeloadHook2: Saving %d and %p in $a0 and $a1.", argc, block_start);
#endif
	cpuRegs.GPR.n.a0.SD[0] = argc;
	cpuRegs.GPR.n.a1.UD[0] = block_start;
}

inline bool isBranchOrJump(u32 addr)
{
	u32 op = memRead32(addr);
	const OPCODE& opcode = GetInstruction(op);

	// Return false for eret & syscall as they are branch type in pcsx2 debugging tools,
	// but shouldn't have delay slot in isBreakpointNeeded/isMemcheckNeeded.
	if ((opcode.flags == (IS_BRANCH | BRANCHTYPE_SYSCALL)) || (opcode.flags == (IS_BRANCH | BRANCHTYPE_ERET)))
		return false;

	return (opcode.flags & IS_BRANCH) != 0;
}

// The next two functions return 0 if no breakpoint is needed,
// 1 if it's needed on the current pc, 2 if it's needed in the delay slot
// 3 if needed in both

int isBreakpointNeeded(u32 addr)
{
	int bpFlags = 0;
	if (CBreakPoints::IsAddressBreakPoint(BREAKPOINT_EE, addr))
		bpFlags += 1;

	// there may be a breakpoint in the delay slot
	if (isBranchOrJump(addr) && CBreakPoints::IsAddressBreakPoint(BREAKPOINT_EE, addr+4))
		bpFlags += 2;

	return bpFlags;
}

int isMemcheckNeeded(u32 pc)
{
	if (CBreakPoints::GetNumMemchecks() == 0)
		return 0;

	u32 addr = pc;
	if (isBranchOrJump(addr))
		addr += 4;

	u32 op = memRead32(addr);
	const OPCODE& opcode = GetInstruction(op);

	if (opcode.flags & IS_MEMORY)
		return addr == pc ? 1 : 2;

	return 0;
}
