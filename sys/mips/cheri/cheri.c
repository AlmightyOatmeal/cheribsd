/*-
 * Copyright (c) 2011-2014 Robert N. M. Watson
 * All rights reserved.
 *
 * This software was developed by SRI International and the University of
 * Cambridge Computer Laboratory under DARPA/AFRL contract (FA8750-10-C-0237)
 * ("CTSRD"), as part of the DARPA CRASH research programme.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "opt_ddb.h"

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>

#include <ddb/ddb.h>
#include <sys/kdb.h>

#include <machine/atomic.h>
#include <machine/cheri.h>
#include <machine/pcb.h>
#include <machine/sysarch.h>

/*
 * Beginnings of a programming interface for explicitly managing capability
 * registers.  Convert back and forth between capability registers and
 * general-purpose registers/memory so that we can program the context,
 * save/restore application contexts, etc.
 *
 * In the future, we'd like the compiler to do this sort of stuff for us
 * based on language-level properties and annotations, but in the mean
 * time...
 *
 * XXXRW: Any manipulation of c0 should include a "memory" clobber for inline
 * assembler, so that the compiler will write back memory contents before the
 * call, and reload them afterwards.
 */

SYSCTL_NODE(_security, OID_AUTO, cheri, CTLFLAG_RD, 0,
    "CHERI settings and statistics");

SYSCTL_NODE(_security_cheri, OID_AUTO, stats, CTLFLAG_RD, 0,
    "CHERI statistics");

/* XXXRW: Should possibly be u_long. */
static u_int	security_cheri_syscall_violations;
SYSCTL_UINT(_security_cheri, OID_AUTO, syscall_violations, CTLFLAG_RD,
    &security_cheri_syscall_violations, 0, "Number of system calls blocked");

static u_int	security_cheri_sandboxed_signals;
SYSCTL_UINT(_security_cheri, OID_AUTO, sandboxed_signals, CTLFLAG_RD,
    &security_cheri_sandboxed_signals, 0, "Number of signals in sandboxes");

static u_int	security_cheri_debugger_on_exception;
SYSCTL_UINT(_security_cheri, OID_AUTO, debugger_on_exception, CTLFLAG_RW,
    &security_cheri_debugger_on_exception, 0,
    "Run debugger on CHERI exception");

#define	CHERI_CAP_PRINT(crn) do {					\
	uintmax_t c_perms, c_otype, c_base, c_length;			\
	u_int ctag, c_unsealed;						\
									\
	CHERI_CGETTAG(ctag, (crn));					\
	CHERI_CGETUNSEALED(c_unsealed, (crn));				\
	CHERI_CGETPERM(c_perms, (crn));					\
	CHERI_CGETTYPE(c_otype, (crn));					\
	CHERI_CGETBASE(c_base, (crn));					\
	CHERI_CGETLEN(c_length, (crn));					\
									\
	printf("t: %u u: %u perms 0x%08jx otype 0x%016jx\n", ctag,	\
	    c_unsealed, c_perms, c_otype);				\
	printf("\tbase 0x%016jx length 0x%016jx\n", c_base, c_length);	\
} while (0)

#define	CHERI_REG_PRINT(crn, num) do {					\
	printf("C%u ", num);						\
	CHERI_CAP_PRINT(crn);						\
} while (0)

static void	cheri_capability_set_user_c0(struct chericap *);
static void	cheri_capability_set_user_stack(struct chericap *);
static void	cheri_capability_set_user_pcc(struct chericap *);

/*
 * Given an existing more privileged capability (fromcrn), build a new
 * capability in tocrn with the contents of the passed flattened
 * representation.
 *
 * XXXRW: It's not yet clear how important ordering is here -- try to do the
 * privilege downgrade in a way that will work when doing an "in place"
 * downgrade, with permissions last.
 *
 * XXXRW: How about the unsealed bit?
 */

void
cheri_capability_set(struct chericap *cp, uint32_t perms,
    void *otypep /* eaddr */, void *basep, uint64_t length)
{

	CHERI_CINCBASE(CHERI_CR_CTEMP0, CHERI_CR_KDC, (register_t)basep);
	CHERI_CSETLEN(CHERI_CR_CTEMP0, CHERI_CR_CTEMP0, (register_t)length);
	CHERI_CANDPERM(CHERI_CR_CTEMP0, CHERI_CR_CTEMP0, (register_t)perms);
	CHERI_CSETTYPE(CHERI_CR_CTEMP0, CHERI_CR_CTEMP0, (register_t)otypep);
	CHERI_CSC(CHERI_CR_CTEMP0, CHERI_CR_KDC, (register_t)cp, 0);
}

static void
cheri_capability_clear(struct chericap *cp)
{

	/*
	 * While we could construct a non-capability and write it out, simply
	 * bzero'ing memory is sufficient to clear the tag bit, and easier to
	 * spell.
	 */
	bzero(cp, sizeof(*cp));

}

/*
 * Functions to store a common set of capability values to in-memory
 * capabilities used in various aspects of user context.
 * contexts.
 */
#ifdef _UNUSED
static void
cheri_capability_set_priv(struct chericap *cp)
{

	cheri_capability_set(cp, CHERI_CAP_PRIV_PERMS, CHERI_CAP_PRIV_OTYPE,
	    CHERI_CAP_PRIV_BASE, CHERI_CAP_PRIV_LENGTH);
}
#endif

static void
cheri_capability_set_user_c0(struct chericap *cp)
{

	cheri_capability_set(cp, CHERI_CAP_USER_PERMS, CHERI_CAP_USER_OTYPE,
	    CHERI_CAP_USER_BASE, CHERI_CAP_USER_LENGTH);
}

static void
cheri_capability_set_user_stack(struct chericap *cp)
{

	/*
	 * For now, initialise stack as ambient with identical rights as $c0.
	 * In the future, we will likely want to change this to be ephemeral.
	 */
	cheri_capability_set(cp, CHERI_CAP_USER_PERMS, CHERI_CAP_USER_OTYPE,
	    CHERI_CAP_USER_BASE, CHERI_CAP_USER_LENGTH);
}

static void
cheri_capability_set_user_idc(struct chericap *cp)
{

	/*
	 * The default invoked data capability is also identical to $c0.
	 */
	cheri_capability_set(cp, CHERI_CAP_USER_PERMS, CHERI_CAP_USER_OTYPE,
	    CHERI_CAP_USER_BASE, CHERI_CAP_USER_LENGTH);
}

static void
cheri_capability_set_user_pcc(struct chericap *cp)
{

	cheri_capability_set(cp, CHERI_CAP_USER_PERMS, CHERI_CAP_USER_OTYPE,
	    CHERI_CAP_USER_BASE, CHERI_CAP_USER_LENGTH);
}

void
cheri_capability_set_null(struct chericap *cp)
{

	cheri_capability_clear(cp);
}

/*
 * Because contexts contain tagged capabilities, we can't just use memcpy()
 * on the data structure.  Once the C compiler knows about capabilities, then
 * direct structure assignment should be plausible.  In the mean time, an
 * explicit capability context copy routine is required.
 *
 * XXXRW: Compiler should know how to do copies of tagged capabilities.
 *
 * XXXRW: Compiler should be providing us with the temporary register.
 */
void
cheri_capability_copy(struct chericap *cp_to, struct chericap *cp_from)
{

	cheri_capability_load(CHERI_CR_CTEMP0, cp_from);
	cheri_capability_store(CHERI_CR_CTEMP0, cp_to);
}

void
cheri_context_copy(struct pcb *dst, struct pcb *src)
{

	cheri_memcpy(&dst->pcb_cheriframe, &src->pcb_cheriframe,
	    sizeof(dst->pcb_cheriframe));
}

void
cheri_exec_setregs(struct thread *td)
{
	struct cheri_frame *cfp;
	struct cheri_signal *csigp;

	/*
	 * XXXRW: Experimental CHERI ABI initialises $c0 with full user
	 * privilege, and all other user-accessible capability registers with
	 * no rights at all.  The runtime linker/compiler/application can
	 * propagate around rights as required.
	 */
	cfp = &td->td_pcb->pcb_cheriframe;
	bzero(cfp, sizeof(*cfp));
	cheri_capability_set_user_c0(&cfp->cf_c0);
	cheri_capability_set_user_stack(&cfp->cf_c11);
	cheri_capability_set_user_idc(&cfp->cf_idc);
	cheri_capability_set_user_pcc(&cfp->cf_pcc);

	/*
	 * Also initialise signal-handling state; this can't yet be modified
	 * by userspace, but the principle is that signal handlers should run
	 * with ambient authority unless given up by the userspace runtime
	 * explicitly.
	 */
	csigp = &td->td_pcb->pcb_cherisignal;
	bzero(csigp, sizeof(*csigp));
	cheri_capability_set_user_c0(&csigp->csig_c0);
	cheri_capability_set_user_stack(&csigp->csig_c11);
	cheri_capability_set_user_idc(&csigp->csig_idc);
	cheri_capability_set_user_pcc(&csigp->csig_pcc);
}

/*
 * When new threads are forked, by default simply replicate the parent
 * thread's CHERI-related signal-handling state.
 *
 * XXXRW: Is this, in fact, the right thing?
 */
void
cheri_signal_copy(struct pcb *dst, struct pcb *src)
{

	cheri_memcpy(&dst->pcb_cherisignal, &src->pcb_cherisignal,
	    sizeof(dst->pcb_cherisignal));
}

/*
 * Configure CHERI register state for a thread about to resume in a signal
 * handler.  Eventually, csigp should contain configurable values, but for
 * now, this ensures handlers run with ambient authority in a useful way.
 * Note that this doesn't touch the already copied-out CHERI register frame
 * (see sendsig()), and hence when sigreturn() is called, the previous CHERI
 * state will be restored by default.
 */
void
cheri_sendsig(struct thread *td)
{
	struct cheri_frame *cfp;
	struct cheri_signal *csigp;

	cfp = &td->td_pcb->pcb_cheriframe;
	csigp = &td->td_pcb->pcb_cherisignal;
	cheri_capability_copy(&cfp->cf_c0, &csigp->csig_c0);
	cheri_capability_copy(&cfp->cf_c11, &csigp->csig_c11);
	cheri_capability_copy(&cfp->cf_idc, &csigp->csig_idc);
	cheri_capability_copy(&cfp->cf_pcc, &csigp->csig_pcc);
}

static const char *cheri_exccode_array[] = {
	"none",					/* CHERI_EXCCODE_NONE */
	"length violation",			/* CHERI_EXCCODE_LENGTH */
	"tag violation",			/* CHERI_EXCCODE_TAG */
	"seal violation",			/* CHERI_EXCCODE_SEAL */
	"type violation",			/* CHERI_EXCCODE_TYPE */
	"call trap",				/* CHERI_EXCCODE_CALL */
	"return trap",				/* CHERI_EXCCODE_RETURN */
	"underflow of trusted system stack",	/* CHERI_EXCCODE_UNDERFLOW */
	"user-defined permission exception",	/* CHERI_EXCCODE_USER */
	"reserved",				/* TBD */
	"reserved",				/* TBD */
	"reserved",				/* TBD */
	"reserved",				/* TBD */
	"reserved",				/* TBD */
	"reserved",				/* TBD */
	"reserved",				/* TBD */
	"non-ephemeral violation",		/* CHERI_EXCCODE_NON_EPHEM */
	"permit execute violation",		/* CHERI_EXCCODE_PERM_EXECUTE */
	"permit load violation",		/* CHERI_EXCCODE_PERM_LOAD */
	"permit store violation",		/* CHERI_EXCCODE_PERM_STORE */
	"permit load capability violation",	/* CHERI_EXCCODE_PERM_LOADCAP */
	"permit store capability violation",   /* CHERI_EXCCODE_PERM_STORECAP */
 "permit store ephemeral capability violation", /* CHERI_EXCCODE_STORE_EPHEM */
	"permit seal violation",		/* CHERI_EXCCODE_PERM_SEAL */
	"permit set type violation",		/* CHERI_EXCCODE_PERM_SETTYPE */
	"reserved",				/* TBD */
	"access EPCC violation",		/* CHERI_EXCCODE_ACCESS_EPCC */
	"access KDC violation",			/* CHERI_EXCCODE_ACCESS_KDC */
	"access KCC violation",			/* CHERI_EXCCODE_ACCESS_KCC */
	"access KR1C violation",		/* CHERI_EXCCODE_ACCESS_KR1C */
	"access KR2C violation",		/* CHERI_EXCCODE_ACCESS_KR2C */
};
static const int cheri_exccode_array_length = sizeof(cheri_exccode_array) /
    sizeof(cheri_exccode_array[0]);

static const char *
cheri_exccode_string(uint8_t exccode)
{

	if (exccode >= cheri_exccode_array_length)
		return ("unknown exception");
	return (cheri_exccode_array[exccode]);
}

void
cheri_log_exception(struct trapframe *frame, int trap_type)
{
	struct cheri_frame *cheriframe;
	register_t cause;
	uint8_t exccode, regnum;

#ifdef SMP
	printf("cpuid = %d\n", PCPU_GET(cpuid));
#endif
	/* XXXRW: awkward and unmaintainable pointer construction. */
	cheriframe = &(((struct pcb *)frame)->pcb_cheriframe);
	cause = cheriframe->cf_capcause;
	exccode = (cause & CHERI_CAPCAUSE_EXCCODE_MASK) >>
	    CHERI_CAPCAUSE_EXCCODE_SHIFT;
	regnum = cause & CHERI_CAPCAUSE_REGNUM_MASK;
	printf("CHERI cause: ExcCode: 0x%02x RegNum: 0x%02x (%s)\n", exccode,
	    regnum, cheri_exccode_string(exccode));


	/* C0 */
	CHERI_CLC(CHERI_CR_CTEMP0, CHERI_CR_KDC, &cheriframe->cf_c0, 0);
	CHERI_REG_PRINT(CHERI_CR_CTEMP0, 0);

	/* C1 */
	CHERI_CLC(CHERI_CR_CTEMP0, CHERI_CR_KDC, &cheriframe->cf_c1, 0);
	CHERI_REG_PRINT(CHERI_CR_CTEMP0, 1);

	/* C2 */
	CHERI_CLC(CHERI_CR_CTEMP0, CHERI_CR_KDC, &cheriframe->cf_c2, 0);
	CHERI_REG_PRINT(CHERI_CR_CTEMP0, 2);

	/* C3 */
	CHERI_CLC(CHERI_CR_CTEMP0, CHERI_CR_KDC, &cheriframe->cf_c3, 0);
	CHERI_REG_PRINT(CHERI_CR_CTEMP0, 3);

	/* C4 */
	CHERI_CLC(CHERI_CR_CTEMP0, CHERI_CR_KDC, &cheriframe->cf_c4, 0);
	CHERI_REG_PRINT(CHERI_CR_CTEMP0, 4);

	/* C11 */
	CHERI_CLC(CHERI_CR_CTEMP0, CHERI_CR_KDC, &cheriframe->cf_c11, 0);
	CHERI_REG_PRINT(CHERI_CR_CTEMP0, 11);

	/* C24 - RCC */
	CHERI_CLC(CHERI_CR_CTEMP0, CHERI_CR_KDC, &cheriframe->cf_rcc, 0);
	CHERI_REG_PRINT(CHERI_CR_CTEMP0, 24);

	/* C26 - IDC */
	CHERI_CLC(CHERI_CR_CTEMP0, CHERI_CR_KDC, &cheriframe->cf_idc, 0);
	CHERI_REG_PRINT(CHERI_CR_CTEMP0, 26);

	/* C31 - saved PCC */
	CHERI_CLC(CHERI_CR_CTEMP0, CHERI_CR_KDC, &cheriframe->cf_pcc, 0);
	CHERI_REG_PRINT(CHERI_CR_CTEMP0, 31);

#if DDB
	if (security_cheri_debugger_on_exception)
		kdb_enter(KDB_WHY_CHERI, "CHERI exception");
#endif
}

/*
 * Only allow most system calls from either ambient authority, or from
 * sandboxes that have been explicitly delegated CHERI_PERM_SYSCALL via their
 * code capability.  Note that CHERI_PERM_SYSCALL effectively implies ambient
 * authority, as the kernel does not [currently] interpret pointers/lengths
 * via userspace $c0.
 */
int
cheri_syscall_authorize(struct thread *td, u_int code, int nargs,
    register_t *args)
{
	uintmax_t c_perms;

	/*
	 * Allow the cycle counter to be read via sysarch.
	 *
	 * XXXRW: Now that we support a userspace cycle counter, we should
	 * remove this.
	 */
	if (code == SYS_sysarch && args[0] == MIPS_GET_COUNT)
		return (0);

	/*
	 * Check whether userspace holds the rights defined in
	 * cheri_capability_set_user() in $PCC.  Note that object type doesn't
	 * come into play here.
	 *
	 * XXXRW: Possibly ECAPMODE should be EPROT or ESANDBOX?
	 */
	CHERI_CLC(CHERI_CR_CTEMP0, CHERI_CR_KDC,
	    &td->td_pcb->pcb_cheriframe.cf_pcc, 0);
	CHERI_CGETPERM(c_perms, CHERI_CR_CTEMP0);
	if ((c_perms & CHERI_PERM_SYSCALL) == 0) {
		atomic_add_int(&security_cheri_syscall_violations, 1);

#if DDB
		if (security_cheri_debugger_on_exception)
			kdb_enter(KDB_WHY_CHERI,
			    "blocked system call within sandbox");
#endif
		return (ECAPMODE);
	}
	return (0);
}

/*
 * As with system calls, handling signal delivery connotes special authority
 * in the runtime environment.  In the signal delivery code, we need to
 * determine whether to trust the executing thread to have valid stack state,
 * and use this function to query whether the execution environment is
 * suitable for direct handler execution, or if (in effect) a security-domain
 * transition is required first.
 */
int
cheri_signal_sandboxed(struct thread *td)
{
	uintmax_t c_perms;

	CHERI_CLC(CHERI_CR_CTEMP0, CHERI_CR_KDC,
	    &td->td_pcb->pcb_cheriframe.cf_pcc, 0);
	CHERI_CGETPERM(c_perms, CHERI_CR_CTEMP0);
	if ((c_perms & CHERI_PERM_SYSCALL) == 0) {
		atomic_add_int(&security_cheri_sandboxed_signals, 1);
		return (ECAPMODE);
	}
	return (0);
}

#ifdef DDB
#define	DB_CHERI_REG_PRINT_NUM(crn, num) do {				\
	uintmax_t c_perms, c_otype, c_base, c_length;			\
	u_int ctag, c_unsealed;						\
									\
	CHERI_CGETTAG(ctag, (crn));					\
	CHERI_CGETUNSEALED(c_unsealed, (crn));				\
	CHERI_CGETPERM(c_perms, (crn));					\
	CHERI_CGETTYPE(c_otype, (crn));					\
	CHERI_CGETBASE(c_base, (crn));					\
	CHERI_CGETLEN(c_length, (crn));					\
									\
	db_printf("C%u t: %u u: %u perms 0x%08jx otype 0x%016jx\n",	\
	    num, ctag, c_unsealed, c_perms, c_otype);			\
	db_printf("\tbase 0x%016jx length 0x%016jx\n", c_base,		\
	    c_length);							\
} while (0)

#define	DB_CHERI_REG_PRINT(crn)	 DB_CHERI_REG_PRINT_NUM(crn, crn)

/*
 * Variation that prints live register state from the capability coprocessor.
 */
DB_SHOW_COMMAND(cheri, ddb_dump_cheri)
{
	register_t cause;

	db_printf("CHERI registers\n");
	DB_CHERI_REG_PRINT(0);
	DB_CHERI_REG_PRINT(1);
	DB_CHERI_REG_PRINT(2);
	DB_CHERI_REG_PRINT(3);
	DB_CHERI_REG_PRINT(4);
	DB_CHERI_REG_PRINT(5);
	DB_CHERI_REG_PRINT(6);
	DB_CHERI_REG_PRINT(7);
	DB_CHERI_REG_PRINT(8);
	DB_CHERI_REG_PRINT(9);
	DB_CHERI_REG_PRINT(10);
	DB_CHERI_REG_PRINT(11);
	DB_CHERI_REG_PRINT(12);
	DB_CHERI_REG_PRINT(13);
	DB_CHERI_REG_PRINT(14);
	DB_CHERI_REG_PRINT(15);
	DB_CHERI_REG_PRINT(16);
	DB_CHERI_REG_PRINT(17);
	DB_CHERI_REG_PRINT(18);
	DB_CHERI_REG_PRINT(19);
	DB_CHERI_REG_PRINT(20);
	DB_CHERI_REG_PRINT(21);
	DB_CHERI_REG_PRINT(22);
	DB_CHERI_REG_PRINT(23);
	DB_CHERI_REG_PRINT(24);
	DB_CHERI_REG_PRINT(25);
	DB_CHERI_REG_PRINT(26);
	DB_CHERI_REG_PRINT(27);
	DB_CHERI_REG_PRINT(28);
	DB_CHERI_REG_PRINT(29);
	DB_CHERI_REG_PRINT(30);
	DB_CHERI_REG_PRINT(31);
	CHERI_CGETCAUSE(cause);
	db_printf("CHERI cause: ExcCode: 0x%02x RegNum: 0x%02x\n",
	    (uint8_t)((cause >> 8) & 0xff), (uint8_t)(cause & 0x1f));
}

/*
 * Variation that prints the saved userspace CHERI register frame for a
 * thread.
 */
DB_SHOW_COMMAND(cheriframe, ddb_dump_cheriframe)
{
	struct thread *td;
	struct cheri_frame *cfp;
	u_int i;

	if (have_addr)
		td = db_lookup_thread(addr, TRUE);
	else
		td = curthread;

	cfp = &td->td_pcb->pcb_cheriframe;
	db_printf("Thread %d at %p\n", td->td_tid, td);
	db_printf("CHERI frame at %p\n", cfp);

	/* Laboriously load and print each user capability. */
	for (i = 0; i < 27; i++) {
		cheri_capability_load(CHERI_CR_CTEMP0,
		    (struct chericap *)&cfp->cf_c0 + i);
		DB_CHERI_REG_PRINT_NUM(CHERI_CR_CTEMP0, i);
	}
	db_printf("\nPCC:\n");
	cheri_capability_load(CHERI_CR_CTEMP0,
	    (struct chericap *)&cfp->cf_c0 + CHERIFRAME_OFF_PCC);
	DB_CHERI_REG_PRINT_NUM(CHERI_CR_CTEMP0, CHERI_CR_EPCC);
}
#endif
