/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  S390 version
 *    Copyright IBM Corp. 1999
 *    Author(s): Hartmut Penner (hp@de.ibm.com),
 *               Martin Schwidefsky (schwidefsky@de.ibm.com)
 *
 *  Derived from "include/asm-i386/processor.h"
 *    Copyright (C) 1994, Linus Torvalds
 */

#ifndef __ASM_S390_PROCESSOR_H
#define __ASM_S390_PROCESSOR_H

#include <linux/bits.h>

#define CIF_NOHZ_DELAY		2	/* delay HZ disable for a tick */
#define CIF_ENABLED_WAIT	5	/* in enabled wait state */
#define CIF_MCCK_GUEST		6	/* machine check happening in guest */
#define CIF_DEDICATED_CPU	7	/* this CPU is dedicated */

#define _CIF_NOHZ_DELAY		BIT(CIF_NOHZ_DELAY)
#define _CIF_ENABLED_WAIT	BIT(CIF_ENABLED_WAIT)
#define _CIF_MCCK_GUEST		BIT(CIF_MCCK_GUEST)
#define _CIF_DEDICATED_CPU	BIT(CIF_DEDICATED_CPU)

#define RESTART_FLAG_CTLREGS	_AC(1 << 0, U)

#ifndef __ASSEMBLER__

#include <linux/cpumask.h>
#include <linux/linkage.h>
#include <linux/irqflags.h>
#include <linux/bitops.h>
#include <asm/fpu-types.h>
#include <asm/cpu.h>
#include <asm/page.h>
#include <asm/ptrace.h>
#include <asm/setup.h>
#include <asm/runtime_instr.h>
#include <asm/irqflags.h>
#include <asm/alternative.h>
#include <asm/fault.h>

struct pcpu {
	unsigned long ec_mask;		/* bit mask for ec_xxx functions */
	unsigned long ec_clk;		/* sigp timestamp for ec_xxx */
	unsigned long flags;		/* per CPU flags */
	unsigned long capacity;		/* cpu capacity for scheduler */
	signed char state;		/* physical cpu state */
	signed char polarization;	/* physical polarization */
	u16 address;			/* physical cpu address */
};

DECLARE_PER_CPU(struct pcpu, pcpu_devices);

typedef long (*sys_call_ptr_t)(struct pt_regs *regs);

static __always_inline struct pcpu *this_pcpu(void)
{
	return (struct pcpu *)(get_lowcore()->pcpu);
}

static __always_inline void set_cpu_flag(int flag)
{
	set_bit(flag, &this_pcpu()->flags);
}

static __always_inline void clear_cpu_flag(int flag)
{
	clear_bit(flag, &this_pcpu()->flags);
}

static __always_inline bool test_cpu_flag(int flag)
{
	return test_bit(flag, &this_pcpu()->flags);
}

static __always_inline bool test_and_set_cpu_flag(int flag)
{
	return test_and_set_bit(flag, &this_pcpu()->flags);
}

static __always_inline bool test_and_clear_cpu_flag(int flag)
{
	return test_and_clear_bit(flag, &this_pcpu()->flags);
}

/*
 * Test CIF flag of another CPU. The caller needs to ensure that
 * CPU hotplug can not happen, e.g. by disabling preemption.
 */
static __always_inline bool test_cpu_flag_of(int flag, int cpu)
{
	return test_bit(flag, &per_cpu(pcpu_devices, cpu).flags);
}

#define arch_needs_cpu() test_cpu_flag(CIF_NOHZ_DELAY)

static inline void get_cpu_id(struct cpuid *ptr)
{
	asm volatile("stidp %0" : "=Q" (*ptr));
}

static __always_inline unsigned long get_cpu_timer(void)
{
	unsigned long timer;

	asm volatile("stpt	%[timer]" : [timer] "=Q" (timer));
	return timer;
}

void s390_adjust_jiffies(void);
void s390_update_cpu_mhz(void);
void cpu_detect_mhz_feature(void);

extern const struct seq_operations cpuinfo_op;
extern void execve_tail(void);
unsigned long vdso_text_size(void);
unsigned long vdso_size(void);

/*
 * User space process size: 2GB for 31 bit, 4TB or 8PT for 64 bit.
 */

#define TASK_SIZE		(test_thread_flag(TIF_31BIT) ? \
					_REGION3_SIZE : TASK_SIZE_MAX)
#define TASK_UNMAPPED_BASE	(test_thread_flag(TIF_31BIT) ? \
					(_REGION3_SIZE >> 1) : (_REGION2_SIZE >> 1))
#define TASK_SIZE_MAX		(-PAGE_SIZE)

#define VDSO_BASE		(STACK_TOP + PAGE_SIZE)
#define VDSO_LIMIT		(test_thread_flag(TIF_31BIT) ? _REGION3_SIZE : _REGION2_SIZE)
#define STACK_TOP		(VDSO_LIMIT - vdso_size() - PAGE_SIZE)
#define STACK_TOP_MAX		(_REGION2_SIZE - vdso_size() - PAGE_SIZE)

#define HAVE_ARCH_PICK_MMAP_LAYOUT

#define __stackleak_poison __stackleak_poison
static __always_inline void __stackleak_poison(unsigned long erase_low,
					       unsigned long erase_high,
					       unsigned long poison)
{
	unsigned long tmp, count;

	count = erase_high - erase_low;
	if (!count)
		return;
	asm volatile(
		"	cghi	%[count],8\n"
		"	je	2f\n"
		"	aghi	%[count],-(8+1)\n"
		"	srlg	%[tmp],%[count],8\n"
		"	ltgr	%[tmp],%[tmp]\n"
		"	jz	1f\n"
		"0:	stg	%[poison],0(%[addr])\n"
		"	mvc	8(256-8,%[addr]),0(%[addr])\n"
		"	la	%[addr],256(%[addr])\n"
		"	brctg	%[tmp],0b\n"
		"1:	stg	%[poison],0(%[addr])\n"
		"	exrl	%[count],3f\n"
		"	j	4f\n"
		"2:	stg	%[poison],0(%[addr])\n"
		"	j	4f\n"
		"3:	mvc	8(1,%[addr]),0(%[addr])\n"
		"4:\n"
		: [addr] "+&a" (erase_low), [count] "+&d" (count), [tmp] "=&a" (tmp)
		: [poison] "d" (poison)
		: "memory", "cc"
		);
}

/*
 * Thread structure
 */
struct thread_struct {
	unsigned int  acrs[NUM_ACRS];
	unsigned long ksp;			/* kernel stack pointer */
	unsigned long user_timer;		/* task cputime in user space */
	unsigned long guest_timer;		/* task cputime in kvm guest */
	unsigned long system_timer;		/* task cputime in kernel space */
	unsigned long hardirq_timer;		/* task cputime in hardirq context */
	unsigned long softirq_timer;		/* task cputime in softirq context */
	const sys_call_ptr_t *sys_call_table;	/* system call table address */
	union teid gmap_teid;			/* address and flags of last gmap fault */
	unsigned int gmap_int_code;		/* int code of last gmap fault */
	int ufpu_flags;				/* user fpu flags */
	int kfpu_flags;				/* kernel fpu flags */

	/* Per-thread information related to debugging */
	struct per_regs per_user;		/* User specified PER registers */
	struct per_event per_event;		/* Cause of the last PER trap */
	unsigned long per_flags;		/* Flags to control debug behavior */
	unsigned int system_call;		/* system call number in signal */
	unsigned long last_break;		/* last breaking-event-address. */
	/* pfault_wait is used to block the process on a pfault event */
	unsigned long pfault_wait;
	struct list_head list;
	/* cpu runtime instrumentation */
	struct runtime_instr_cb *ri_cb;
	struct gs_cb *gs_cb;			/* Current guarded storage cb */
	struct gs_cb *gs_bc_cb;			/* Broadcast guarded storage cb */
	struct pgm_tdb trap_tdb;		/* Transaction abort diagnose block */
	struct fpu ufpu;			/* User FP and VX register save area */
	struct fpu kfpu;			/* Kernel FP and VX register save area */
};

/* Flag to disable transactions. */
#define PER_FLAG_NO_TE			1UL
/* Flag to enable random transaction aborts. */
#define PER_FLAG_TE_ABORT_RAND		2UL
/* Flag to specify random transaction abort mode:
 * - abort each transaction at a random instruction before TEND if set.
 * - abort random transactions at a random instruction if cleared.
 */
#define PER_FLAG_TE_ABORT_RAND_TEND	4UL

typedef struct thread_struct thread_struct;

#define ARCH_MIN_TASKALIGN	8

#define INIT_THREAD {							\
	.ksp = sizeof(init_stack) + (unsigned long) &init_stack,	\
	.last_break = 1,						\
}

/*
 * Do necessary setup to start up a new thread.
 */
#define start_thread(regs, new_psw, new_stackp) do {			\
	regs->psw.mask	= PSW_USER_BITS | PSW_MASK_EA | PSW_MASK_BA;	\
	regs->psw.addr	= new_psw;					\
	regs->gprs[15]	= new_stackp;					\
	execve_tail();							\
} while (0)

#define start_thread31(regs, new_psw, new_stackp) do {			\
	regs->psw.mask	= PSW_USER_BITS | PSW_MASK_BA;			\
	regs->psw.addr	= new_psw;					\
	regs->gprs[15]	= new_stackp;					\
	execve_tail();							\
} while (0)

struct task_struct;
struct mm_struct;
struct seq_file;
struct pt_regs;

void show_registers(struct pt_regs *regs);
void show_cacheinfo(struct seq_file *m);

/* Free guarded storage control block */
void guarded_storage_release(struct task_struct *tsk);
void gs_load_bc_cb(struct pt_regs *regs);

unsigned long __get_wchan(struct task_struct *p);
#define task_pt_regs(tsk) ((struct pt_regs *) \
        (task_stack_page(tsk) + THREAD_SIZE) - 1)
#define KSTK_EIP(tsk)	(task_pt_regs(tsk)->psw.addr)
#define KSTK_ESP(tsk)	(task_pt_regs(tsk)->gprs[15])

/* Has task runtime instrumentation enabled ? */
#define is_ri_task(tsk) (!!(tsk)->thread.ri_cb)

/* avoid using global register due to gcc bug in versions < 8.4 */
#define current_stack_pointer (__current_stack_pointer())

static __always_inline unsigned long __current_stack_pointer(void)
{
	unsigned long sp;

	asm volatile("lgr %0,15" : "=d" (sp));
	return sp;
}

static __always_inline bool on_thread_stack(void)
{
	unsigned long ksp = get_lowcore()->kernel_stack;

	return !((ksp ^ current_stack_pointer) & ~(THREAD_SIZE - 1));
}

static __always_inline unsigned short stap(void)
{
	unsigned short cpu_address;

	asm volatile("stap %0" : "=Q" (cpu_address));
	return cpu_address;
}

#define cpu_relax() barrier()

#define ECAG_CACHE_ATTRIBUTE	0
#define ECAG_CPU_ATTRIBUTE	1

static inline unsigned long __ecag(unsigned int asi, unsigned char parm)
{
	unsigned long val;

	asm volatile("ecag %0,0,0(%1)" : "=d" (val) : "a" (asi << 8 | parm));
	return val;
}

static inline void psw_set_key(unsigned int key)
{
	asm volatile("spka 0(%0)" : : "d" (key));
}

/*
 * Set PSW to specified value.
 */
static inline void __load_psw(psw_t psw)
{
	asm volatile("lpswe %0" : : "Q" (psw) : "cc");
}

/*
 * Set PSW mask to specified value, while leaving the
 * PSW addr pointing to the next instruction.
 */
static __always_inline void __load_psw_mask(unsigned long mask)
{
	psw_t psw __uninitialized;
	unsigned long addr;

	psw.mask = mask;

	asm volatile(
		"	larl	%0,1f\n"
		"	stg	%0,%1\n"
		"	lpswe	%2\n"
		"1:"
		: "=&d" (addr), "=Q" (psw.addr) : "Q" (psw) : "memory", "cc");
}

/*
 * Extract current PSW mask
 */
static inline unsigned long __extract_psw(void)
{
	unsigned int reg1, reg2;

	asm volatile("epsw %0,%1" : "=d" (reg1), "=a" (reg2));
	return (((unsigned long) reg1) << 32) | ((unsigned long) reg2);
}

static inline unsigned long __local_mcck_save(void)
{
	unsigned long mask = __extract_psw();

	__load_psw_mask(mask & ~PSW_MASK_MCHECK);
	return mask & PSW_MASK_MCHECK;
}

#define local_mcck_save(mflags)			\
do {						\
	typecheck(unsigned long, mflags);	\
	mflags = __local_mcck_save();		\
} while (0)

static inline void local_mcck_restore(unsigned long mflags)
{
	unsigned long mask = __extract_psw();

	mask &= ~PSW_MASK_MCHECK;
	__load_psw_mask(mask | mflags);
}

static inline void local_mcck_disable(void)
{
	__local_mcck_save();
}

static inline void local_mcck_enable(void)
{
	__load_psw_mask(__extract_psw() | PSW_MASK_MCHECK);
}

/*
 * Rewind PSW instruction address by specified number of bytes.
 */
static inline unsigned long __rewind_psw(psw_t psw, unsigned long ilc)
{
	unsigned long mask;

	mask = (psw.mask & PSW_MASK_EA) ? -1UL :
	       (psw.mask & PSW_MASK_BA) ? (1UL << 31) - 1 :
					  (1UL << 24) - 1;
	return (psw.addr - ilc) & mask;
}

/*
 * Function to drop a processor into disabled wait state
 */
static __always_inline void __noreturn disabled_wait(void)
{
	psw_t psw;

	psw.mask = PSW_MASK_BASE | PSW_MASK_WAIT | PSW_MASK_BA | PSW_MASK_EA;
	psw.addr = _THIS_IP_;
	__load_psw(psw);
	while (1);
}

#define ARCH_LOW_ADDRESS_LIMIT	0x7fffffffUL

static __always_inline bool regs_irqs_disabled(struct pt_regs *regs)
{
	return arch_irqs_disabled_flags(regs->psw.mask);
}

static __always_inline void bpon(void)
{
	asm_inline volatile(
		ALTERNATIVE("	nop\n",
			    "	.insn	rrf,0xb2e80000,0,0,13,0\n",
			    ALT_SPEC(82))
		);
}

#endif /* __ASSEMBLER__ */

#endif /* __ASM_S390_PROCESSOR_H */
