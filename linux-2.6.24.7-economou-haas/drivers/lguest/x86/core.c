/*
 * Copyright (C) 2006, Rusty Russell <rusty@rustcorp.com.au> IBM Corporation.
 * Copyright (C) 2007, Jes Sorensen <jes@sgi.com> SGI.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 * NON INFRINGEMENT.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <linux/kernel.h>
#include <linux/start_kernel.h>
#include <linux/string.h>
#include <linux/console.h>
#include <linux/screen_info.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/cpu.h>
#include <linux/lguest.h>
#include <linux/lguest_launcher.h>
#include <asm/paravirt.h>
#include <asm/param.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/desc.h>
#include <asm/setup.h>
#include <asm/lguest.h>
#include <asm/uaccess.h>
#include <asm/i387.h>
#include "../lg.h"

static int cpu_had_pge;

static struct {
	unsigned long offset;
	unsigned short segment;
} lguest_entry;

/* Offset from where switcher.S was compiled to where we've copied it */
static unsigned long switcher_offset(void)
{
	return SWITCHER_ADDR - (unsigned long)start_switcher_text;
}

/* This cpu's struct lguest_pages. */
static struct lguest_pages *lguest_pages(unsigned int cpu)
{
	return &(((struct lguest_pages *)
		  (SWITCHER_ADDR + SHARED_SWITCHER_PAGES*PAGE_SIZE))[cpu]);
}

static DEFINE_PER_CPU(struct lguest *, last_guest);

/*S:010
 * We approach the Switcher.
 *
 * Remember that each CPU has two pages which are visible to the Guest when it
 * runs on that CPU.  This has to contain the state for that Guest: we copy the
 * state in just before we run the Guest.
 *
 * Each Guest has "changed" flags which indicate what has changed in the Guest
 * since it last ran.  We saw this set in interrupts_and_traps.c and
 * segments.c.
 */
static void copy_in_guest_info(struct lguest *lg, struct lguest_pages *pages)
{
	/* Copying all this data can be quite expensive.  We usually run the
	 * same Guest we ran last time (and that Guest hasn't run anywhere else
	 * meanwhile).  If that's not the case, we pretend everything in the
	 * Guest has changed. */
	if (__get_cpu_var(last_guest) != lg || lg->last_pages != pages) {
		__get_cpu_var(last_guest) = lg;
		lg->last_pages = pages;
		lg->changed = CHANGED_ALL;
	}

	/* These copies are pretty cheap, so we do them unconditionally: */
	/* Save the current Host top-level page directory. */
	pages->state.host_cr3 = __pa(current->mm->pgd);
	/* Set up the Guest's page tables to see this CPU's pages (and no
	 * other CPU's pages). */
	map_switcher_in_guest(lg, pages);
	/* Set up the two "TSS" members which tell the CPU what stack to use
	 * for traps which do directly into the Guest (ie. traps at privilege
	 * level 1). */
	pages->state.guest_tss.esp1 = lg->esp1;
	pages->state.guest_tss.ss1 = lg->ss1;

	/* Copy direct-to-Guest trap entries. */
	if (lg->changed & CHANGED_IDT)
		copy_traps(lg, pages->state.guest_idt, default_idt_entries);

	/* Copy all GDT entries which the Guest can change. */
	if (lg->changed & CHANGED_GDT)
		copy_gdt(lg, pages->state.guest_gdt);
	/* If only the TLS entries have changed, copy them. */
	else if (lg->changed & CHANGED_GDT_TLS)
		copy_gdt_tls(lg, pages->state.guest_gdt);

	/* Mark the Guest as unchanged for next time. */
	lg->changed = 0;
}

/* Finally: the code to actually call into the Switcher to run the Guest. */
static void run_guest_once(struct lguest *lg, struct lguest_pages *pages)
{
	/* This is a dummy value we need for GCC's sake. */
	unsigned int clobber;

	/* Copy the guest-specific information into this CPU's "struct
	 * lguest_pages". */
	copy_in_guest_info(lg, pages);

	/* Set the trap number to 256 (impossible value).  If we fault while
	 * switching to the Guest (bad segment registers or bug), this will
	 * cause us to abort the Guest. */
	lg->regs->trapnum = 256;

	/* Now: we push the "eflags" register on the stack, then do an "lcall".
	 * This is how we change from using the kernel code segment to using
	 * the dedicated lguest code segment, as well as jumping into the
	 * Switcher.
	 *
	 * The lcall also pushes the old code segment (KERNEL_CS) onto the
	 * stack, then the address of this call.  This stack layout happens to
	 * exactly match the stack layout created by an interrupt... */
	asm volatile("pushf; lcall *lguest_entry"
		     /* This is how we tell GCC that %eax ("a") and %ebx ("b")
		      * are changed by this routine.  The "=" means output. */
		     : "=a"(clobber), "=b"(clobber)
		     /* %eax contains the pages pointer.  ("0" refers to the
		      * 0-th argument above, ie "a").  %ebx contains the
		      * physical address of the Guest's top-level page
		      * directory. */
		     : "0"(pages), "1"(__pa(lg->pgdirs[lg->pgdidx].pgdir))
		     /* We tell gcc that all these registers could change,
		      * which means we don't have to save and restore them in
		      * the Switcher. */
		     : "memory", "%edx", "%ecx", "%edi", "%esi");
}
/*:*/

/*M:002 There are hooks in the scheduler which we can register to tell when we
 * get kicked off the CPU (preempt_notifier_register()).  This would allow us
 * to lazily disable SYSENTER which would regain some performance, and should
 * also simplify copy_in_guest_info().  Note that we'd still need to restore
 * things when we exit to Launcher userspace, but that's fairly easy.
 *
 * The hooks were designed for KVM, but we can also put them to good use. :*/

/*H:040 This is the i386-specific code to setup and run the Guest.  Interrupts
 * are disabled: we own the CPU. */
void lguest_arch_run_guest(struct lguest *lg)
{
	/* Remember the awfully-named TS bit?  If the Guest has asked to set it
	 * we set it now, so we can trap and pass that trap to the Guest if it
	 * uses the FPU. */
	if (lg->ts)
		lguest_set_ts();

	/* SYSENTER is an optimized way of doing system calls.  We can't allow
	 * it because it always jumps to privilege level 0.  A normal Guest
	 * won't try it because we don't advertise it in CPUID, but a malicious
	 * Guest (or malicious Guest userspace program) could, so we tell the
	 * CPU to disable it before running the Guest. */
	if (boot_cpu_has(X86_FEATURE_SEP))
		wrmsr(MSR_IA32_SYSENTER_CS, 0, 0);

	/* Now we actually run the Guest.  It will return when something
	 * interesting happens, and we can examine its registers to see what it
	 * was doing. */
	run_guest_once(lg, lguest_pages(raw_smp_processor_id()));

	/* Note that the "regs" pointer contains two extra entries which are
	 * not really registers: a trap number which says what interrupt or
	 * trap made the switcher code come back, and an error code which some
	 * traps set.  */

	/* If the Guest page faulted, then the cr2 register will tell us the
	 * bad virtual address.  We have to grab this now, because once we
	 * re-enable interrupts an interrupt could fault and thus overwrite
	 * cr2, or we could even move off to a different CPU. */
	if (lg->regs->trapnum == 14)
		lg->arch.last_pagefault = read_cr2();
	/* Similarly, if we took a trap because the Guest used the FPU,
	 * we have to restore the FPU it expects to see. */
	else if (lg->regs->trapnum == 7)
		math_state_restore();

	/* Restore SYSENTER if it's supposed to be on. */
	if (boot_cpu_has(X86_FEATURE_SEP))
		wrmsr(MSR_IA32_SYSENTER_CS, __KERNEL_CS, 0);
}

/*H:130 Now we've examined the hypercall code; our Guest can make requests.
 * Our Guest is usually so well behaved; it never tries to do things it isn't
 * allowed to, and uses hypercalls instead.  Unfortunately, Linux's paravirtual
 * infrastructure isn't quite complete, because it doesn't contain replacements
 * for the Intel I/O instructions.  As a result, the Guest sometimes fumbles
 * across one during the boot process as it probes for various things which are
 * usually attached to a PC.
 *
 * When the Guest uses one of these instructions, we get a trap (General
 * Protection Fault) and come here.  We see if it's one of those troublesome
 * instructions and skip over it.  We return true if we did. */
static int emulate_insn(struct lguest *lg)
{
	u8 insn;
	unsigned int insnlen = 0, in = 0, shift = 0;
	/* The eip contains the *virtual* address of the Guest's instruction:
	 * guest_pa just subtracts the Guest's page_offset. */
	unsigned long physaddr = guest_pa(lg, lg->regs->eip);

	/* This must be the Guest kernel trying to do something, not userspace!
	 * The bottom two bits of the CS segment register are the privilege
	 * level. */
	if ((lg->regs->cs & 3) != GUEST_PL)
		return 0;

	/* Decoding x86 instructions is icky. */
	insn = lgread(lg, physaddr, u8);

	/* 0x66 is an "operand prefix".  It means it's using the upper 16 bits
	   of the eax register. */
	if (insn == 0x66) {
		shift = 16;
		/* The instruction is 1 byte so far, read the next byte. */
		insnlen = 1;
		insn = lgread(lg, physaddr + insnlen, u8);
	}

	/* We can ignore the lower bit for the moment and decode the 4 opcodes
	 * we need to emulate. */
	switch (insn & 0xFE) {
	case 0xE4: /* in     <next byte>,%al */
		insnlen += 2;
		in = 1;
		break;
	case 0xEC: /* in     (%dx),%al */
		insnlen += 1;
		in = 1;
		break;
	case 0xE6: /* out    %al,<next byte> */
		insnlen += 2;
		break;
	case 0xEE: /* out    %al,(%dx) */
		insnlen += 1;
		break;
	default:
		/* OK, we don't know what this is, can't emulate. */
		return 0;
	}

	/* If it was an "IN" instruction, they expect the result to be read
	 * into %eax, so we change %eax.  We always return all-ones, which
	 * traditionally means "there's nothing there". */
	if (in) {
		/* Lower bit tells is whether it's a 16 or 32 bit access */
		if (insn & 0x1)
			lg->regs->eax = 0xFFFFFFFF;
		else
			lg->regs->eax |= (0xFFFF << shift);
	}
	/* Finally, we've "done" the instruction, so move past it. */
	lg->regs->eip += insnlen;
	/* Success! */
	return 1;
}

/*H:050 Once we've re-enabled interrupts, we look at why the Guest exited. */
void lguest_arch_handle_trap(struct lguest *lg)
{
	switch (lg->regs->trapnum) {
	case 13: /* We've intercepted a General Protection Fault. */
		/* Check if this was one of those annoying IN or OUT
		 * instructions which we need to emulate.  If so, we just go
		 * back into the Guest after we've done it. */
		if (lg->regs->errcode == 0) {
			if (emulate_insn(lg))
				return;
		}
		break;
	case 14: /* We've intercepted a Page Fault. */
		/* The Guest accessed a virtual address that wasn't mapped.
		 * This happens a lot: we don't actually set up most of the
		 * page tables for the Guest at all when we start: as it runs
		 * it asks for more and more, and we set them up as
		 * required. In this case, we don't even tell the Guest that
		 * the fault happened.
		 *
		 * The errcode tells whether this was a read or a write, and
		 * whether kernel or userspace code. */
		if (demand_page(lg, lg->arch.last_pagefault, lg->regs->errcode))
			return;

		/* OK, it's really not there (or not OK): the Guest needs to
		 * know.  We write out the cr2 value so it knows where the
		 * fault occurred.
		 *
		 * Note that if the Guest were really messed up, this could
		 * happen before it's done the LHCALL_LGUEST_INIT hypercall, so
		 * lg->lguest_data could be NULL */
		if (lg->lguest_data &&
		    put_user(lg->arch.last_pagefault, &lg->lguest_data->cr2))
			kill_guest(lg, "Writing cr2");
		break;
	case 7: /* We've intercepted a Device Not Available fault. */
		/* If the Guest doesn't want to know, we already restored the
		 * Floating Point Unit, so we just continue without telling
		 * it. */
		if (!lg->ts)
			return;
		break;
	case 32 ... 255:
		/* These values mean a real interrupt occurred, in which case
		 * the Host handler has already been run.  We just do a
		 * friendly check if another process should now be run, then
		 * return to run the Guest again */
		cond_resched();
		return;
	case LGUEST_TRAP_ENTRY:
		/* Our 'struct hcall_args' maps directly over our regs: we set
		 * up the pointer now to indicate a hypercall is pending. */
		lg->hcall = (struct hcall_args *)lg->regs;
		return;
	}

	/* We didn't handle the trap, so it needs to go to the Guest. */
	if (!deliver_trap(lg, lg->regs->trapnum))
		/* If the Guest doesn't have a handler (either it hasn't
		 * registered any yet, or it's one of the faults we don't let
		 * it handle), it dies with a cryptic error message. */
		kill_guest(lg, "unhandled trap %li at %#lx (%#lx)",
			   lg->regs->trapnum, lg->regs->eip,
			   lg->regs->trapnum == 14 ? lg->arch.last_pagefault
			   : lg->regs->errcode);
}

/* Now we can look at each of the routines this calls, in increasing order of
 * complexity: do_hypercalls(), emulate_insn(), maybe_do_interrupt(),
 * deliver_trap() and demand_page().  After all those, we'll be ready to
 * examine the Switcher, and our philosophical understanding of the Host/Guest
 * duality will be complete. :*/
static void adjust_pge(void *on)
{
	if (on)
		write_cr4(read_cr4() | X86_CR4_PGE);
	else
		write_cr4(read_cr4() & ~X86_CR4_PGE);
}

/*H:020 Now the Switcher is mapped and every thing else is ready, we need to do
 * some more i386-specific initialization. */
void __init lguest_arch_host_init(void)
{
	int i;

	/* Most of the i386/switcher.S doesn't care that it's been moved; on
	 * Intel, jumps are relative, and it doesn't access any references to
	 * external code or data.
	 *
	 * The only exception is the interrupt handlers in switcher.S: their
	 * addresses are placed in a table (default_idt_entries), so we need to
	 * update the table with the new addresses.  switcher_offset() is a
	 * convenience function which returns the distance between the builtin
	 * switcher code and the high-mapped copy we just made. */
	for (i = 0; i < IDT_ENTRIES; i++)
		default_idt_entries[i] += switcher_offset();

	/*
	 * Set up the Switcher's per-cpu areas.
	 *
	 * Each CPU gets two pages of its own within the high-mapped region
	 * (aka. "struct lguest_pages").  Much of this can be initialized now,
	 * but some depends on what Guest we are running (which is set up in
	 * copy_in_guest_info()).
	 */
	for_each_possible_cpu(i) {
		/* lguest_pages() returns this CPU's two pages. */
		struct lguest_pages *pages = lguest_pages(i);
		/* This is a convenience pointer to make the code fit one
		 * statement to a line. */
		struct lguest_ro_state *state = &pages->state;

		/* The Global Descriptor Table: the Host has a different one
		 * for each CPU.  We keep a descriptor for the GDT which says
		 * where it is and how big it is (the size is actually the last
		 * byte, not the size, hence the "-1"). */
		state->host_gdt_desc.size = GDT_SIZE-1;
		state->host_gdt_desc.address = (long)get_cpu_gdt_table(i);

		/* All CPUs on the Host use the same Interrupt Descriptor
		 * Table, so we just use store_idt(), which gets this CPU's IDT
		 * descriptor. */
		store_idt(&state->host_idt_desc);

		/* The descriptors for the Guest's GDT and IDT can be filled
		 * out now, too.  We copy the GDT & IDT into ->guest_gdt and
		 * ->guest_idt before actually running the Guest. */
		state->guest_idt_desc.size = sizeof(state->guest_idt)-1;
		state->guest_idt_desc.address = (long)&state->guest_idt;
		state->guest_gdt_desc.size = sizeof(state->guest_gdt)-1;
		state->guest_gdt_desc.address = (long)&state->guest_gdt;

		/* We know where we want the stack to be when the Guest enters
		 * the switcher: in pages->regs.  The stack grows upwards, so
		 * we start it at the end of that structure. */
		state->guest_tss.esp0 = (long)(&pages->regs + 1);
		/* And this is the GDT entry to use for the stack: we keep a
		 * couple of special LGUEST entries. */
		state->guest_tss.ss0 = LGUEST_DS;

		/* x86 can have a finegrained bitmap which indicates what I/O
		 * ports the process can use.  We set it to the end of our
		 * structure, meaning "none". */
		state->guest_tss.io_bitmap_base = sizeof(state->guest_tss);

		/* Some GDT entries are the same across all Guests, so we can
		 * set them up now. */
		setup_default_gdt_entries(state);
		/* Most IDT entries are the same for all Guests, too.*/
		setup_default_idt_entries(state, default_idt_entries);

		/* The Host needs to be able to use the LGUEST segments on this
		 * CPU, too, so put them in the Host GDT. */
		get_cpu_gdt_table(i)[GDT_ENTRY_LGUEST_CS] = FULL_EXEC_SEGMENT;
		get_cpu_gdt_table(i)[GDT_ENTRY_LGUEST_DS] = FULL_SEGMENT;
	}

	/* In the Switcher, we want the %cs segment register to use the
	 * LGUEST_CS GDT entry: we've put that in the Host and Guest GDTs, so
	 * it will be undisturbed when we switch.  To change %cs and jump we
	 * need this structure to feed to Intel's "lcall" instruction. */
	lguest_entry.offset = (long)switch_to_guest + switcher_offset();
	lguest_entry.segment = LGUEST_CS;

	/* Finally, we need to turn off "Page Global Enable".  PGE is an
	 * optimization where page table entries are specially marked to show
	 * they never change.  The Host kernel marks all the kernel pages this
	 * way because it's always present, even when userspace is running.
	 *
	 * Lguest breaks this: unbeknownst to the rest of the Host kernel, we
	 * switch to the Guest kernel.  If you don't disable this on all CPUs,
	 * you'll get really weird bugs that you'll chase for two days.
	 *
	 * I used to turn PGE off every time we switched to the Guest and back
	 * on when we return, but that slowed the Switcher down noticibly. */

	/* We don't need the complexity of CPUs coming and going while we're
	 * doing this. */
	lock_cpu_hotplug();
	if (cpu_has_pge) { /* We have a broader idea of "global". */
		/* Remember that this was originally set (for cleanup). */
		cpu_had_pge = 1;
		/* adjust_pge is a helper function which sets or unsets the PGE
		 * bit on its CPU, depending on the argument (0 == unset). */
		on_each_cpu(adjust_pge, (void *)0, 0, 1);
		/* Turn off the feature in the global feature set. */
		clear_bit(X86_FEATURE_PGE, boot_cpu_data.x86_capability);
	}
	unlock_cpu_hotplug();
};
/*:*/

void __exit lguest_arch_host_fini(void)
{
	/* If we had PGE before we started, turn it back on now. */
	lock_cpu_hotplug();
	if (cpu_had_pge) {
		set_bit(X86_FEATURE_PGE, boot_cpu_data.x86_capability);
		/* adjust_pge's argument "1" means set PGE. */
		on_each_cpu(adjust_pge, (void *)1, 0, 1);
	}
	unlock_cpu_hotplug();
}


/*H:122 The i386-specific hypercalls simply farm out to the right functions. */
int lguest_arch_do_hcall(struct lguest *lg, struct hcall_args *args)
{
	switch (args->arg0) {
	case LHCALL_LOAD_GDT:
		load_guest_gdt(lg, args->arg1, args->arg2);
		break;
	case LHCALL_LOAD_IDT_ENTRY:
		load_guest_idt_entry(lg, args->arg1, args->arg2, args->arg3);
		break;
	case LHCALL_LOAD_TLS:
		guest_load_tls(lg, args->arg1);
		break;
	default:
		/* Bad Guest.  Bad! */
		return -EIO;
	}
	return 0;
}

/*H:126 i386-specific hypercall initialization: */
int lguest_arch_init_hypercalls(struct lguest *lg)
{
	u32 tsc_speed;

	/* The pointer to the Guest's "struct lguest_data" is the only
	 * argument.  We check that address now. */
	if (!lguest_address_ok(lg, lg->hcall->arg1, sizeof(*lg->lguest_data)))
		return -EFAULT;

	/* Having checked it, we simply set lg->lguest_data to point straight
	 * into the Launcher's memory at the right place and then use
	 * copy_to_user/from_user from now on, instead of lgread/write.  I put
	 * this in to show that I'm not immune to writing stupid
	 * optimizations. */
	lg->lguest_data = lg->mem_base + lg->hcall->arg1;

	/* We insist that the Time Stamp Counter exist and doesn't change with
	 * cpu frequency.  Some devious chip manufacturers decided that TSC
	 * changes could be handled in software.  I decided that time going
	 * backwards might be good for benchmarks, but it's bad for users.
	 *
	 * We also insist that the TSC be stable: the kernel detects unreliable
	 * TSCs for its own purposes, and we use that here. */
	if (boot_cpu_has(X86_FEATURE_CONSTANT_TSC) && !check_tsc_unstable())
		tsc_speed = tsc_khz;
	else
		tsc_speed = 0;
	if (put_user(tsc_speed, &lg->lguest_data->tsc_khz))
		return -EFAULT;

	/* The interrupt code might not like the system call vector. */
	if (!check_syscall_vector(lg))
		kill_guest(lg, "bad syscall vector");

	return 0;
}

/*L:030 lguest_arch_setup_regs()
 *
 * Most of the Guest's registers are left alone: we used get_zeroed_page() to
 * allocate the structure, so they will be 0. */
void lguest_arch_setup_regs(struct lguest *lg, unsigned long start)
{
	struct lguest_regs *regs = lg->regs;

	/* There are four "segment" registers which the Guest needs to boot:
	 * The "code segment" register (cs) refers to the kernel code segment
	 * __KERNEL_CS, and the "data", "extra" and "stack" segment registers
	 * refer to the kernel data segment __KERNEL_DS.
	 *
	 * The privilege level is packed into the lower bits.  The Guest runs
	 * at privilege level 1 (GUEST_PL).*/
	regs->ds = regs->es = regs->ss = __KERNEL_DS|GUEST_PL;
	regs->cs = __KERNEL_CS|GUEST_PL;

	/* The "eflags" register contains miscellaneous flags.  Bit 1 (0x002)
	 * is supposed to always be "1".  Bit 9 (0x200) controls whether
	 * interrupts are enabled.  We always leave interrupts enabled while
	 * running the Guest. */
	regs->eflags = X86_EFLAGS_IF | 0x2;

	/* The "Extended Instruction Pointer" register says where the Guest is
	 * running. */
	regs->eip = start;

	/* %esi points to our boot information, at physical address 0, so don't
	 * touch it. */

	/* There are a couple of GDT entries the Guest expects when first
	 * booting. */
	setup_guest_gdt(lg);
}