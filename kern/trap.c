/*
 * Processor trap handling.
 *
 * Copyright (C) 1997 Massachusetts Institute of Technology
 * See section "MIT License" in the file LICENSES for licensing terms.
 *
 * Derived from the MIT Exokernel and JOS.
 * Adapted for PIOS by Bryan Ford at Yale University.
 */

#include <inc/mmu.h>
#include <inc/x86.h>
#include <inc/assert.h>

#include <kern/cpu.h>
#include <kern/trap.h>
#include <kern/cons.h>
#include <kern/init.h>
#include <kern/proc.h>
#include <kern/syscall.h>
#include <kern/pmap.h>
#include <kern/net.h>

#include <dev/lapic.h>
#include <dev/kbd.h>
#include <dev/serial.h>
#include <dev/e100.h>


// Interrupt descriptor table.  Must be built at run time because
// shifted function addresses can't be represented in relocation records.
static struct gatedesc idt[256];

// This "pseudo-descriptor" is needed only by the LIDT instruction,
// to specify both the size and address of th IDT at once.
static struct pseudodesc idt_pd = {
	sizeof(idt) - 1, (uint32_t) idt
};


static void
trap_init_idt(void)
{
	extern segdesc gdt[];
    
  // All the trap handlers.
  extern char tdivide, tdebug, tnmi, tbrkpt, toflow, tbound, tillop, 
              tdivide, tdblflt, ttss, tsegnp, tstack, tgpflt, tpgflt, 
              tfperr, talign, tmchk, tsimd, tsecev,
              tirq0, tirqspur, tirqkbd, tirqser, tirq2, tirq3, 
              tirq5, tirq6, tirq8, tirq9, tirq10, tirq11, tirq12, 
              tirq13, tirq14, tirq15,
              tsystem, tltimer;
      
  SETGATE(idt[T_DIVIDE], 0, CPU_GDT_KCODE, &tdivide, 0);
  SETGATE(idt[T_DEBUG], 0, CPU_GDT_KCODE, &tdebug, 0);
  SETGATE(idt[T_NMI], 0, CPU_GDT_KCODE, &tnmi, 0);
  SETGATE(idt[T_BRKPT], 0, CPU_GDT_KCODE, &tbrkpt, 3);
  SETGATE(idt[T_OFLOW], 0, CPU_GDT_KCODE, &toflow, 3);
  SETGATE(idt[T_BOUND], 0, CPU_GDT_KCODE, &tbound, 0);
  SETGATE(idt[T_ILLOP], 0, CPU_GDT_KCODE, &tillop, 0);
  SETGATE(idt[T_DEVICE], 0, CPU_GDT_KCODE, &tdivide, 0);
  SETGATE(idt[T_DBLFLT], 0, CPU_GDT_KCODE, &tdblflt, 0);
  SETGATE(idt[T_TSS], 0, CPU_GDT_KCODE, &ttss, 0);
  SETGATE(idt[T_SEGNP], 0, CPU_GDT_KCODE, &tsegnp, 0);
  SETGATE(idt[T_STACK], 0, CPU_GDT_KCODE, &tstack, 0);
  SETGATE(idt[T_GPFLT], 0, CPU_GDT_KCODE, &tgpflt, 0);
  SETGATE(idt[T_PGFLT], 0, CPU_GDT_KCODE, &tpgflt, 0);
  SETGATE(idt[T_FPERR], 0, CPU_GDT_KCODE, &tfperr, 0);
  SETGATE(idt[T_ALIGN], 0, CPU_GDT_KCODE, &talign, 0);
  SETGATE(idt[T_MCHK], 0, CPU_GDT_KCODE, &tmchk, 0);
  SETGATE(idt[T_SIMD], 0, CPU_GDT_KCODE, &tsimd, 0);
  SETGATE(idt[T_SECEV], 0, CPU_GDT_KCODE, &tsecev, 0);

  // IRQ = 32
  SETGATE(idt[T_IRQ0], 0, CPU_GDT_KCODE, &tirq0, 0);                  // +0
  SETGATE(idt[T_IRQ0+IRQ_KBD], 0, CPU_GDT_KCODE, &tirqkbd,  0);        // +1
  SETGATE(idt[T_IRQ0+IRQ_SERIAL], 0, CPU_GDT_KCODE, &tirqser, 0);     // +4
  SETGATE(idt[T_IRQ0+IRQ_SPURIOUS], 0, CPU_GDT_KCODE, &tirqspur, 0);  // +7

  SETGATE(idt[T_IRQ0+2], 0, CPU_GDT_KCODE, &tirq2, 0);  // +2
  SETGATE(idt[T_IRQ0+3], 0, CPU_GDT_KCODE, &tirq3, 0);  // +3
  SETGATE(idt[T_IRQ0+5], 0, CPU_GDT_KCODE, &tirq5, 0);  // +5
  SETGATE(idt[T_IRQ0+6], 0, CPU_GDT_KCODE, &tirq6, 0);  // +6
  SETGATE(idt[T_IRQ0+8], 0, CPU_GDT_KCODE, &tirq8, 0);  // +8
  SETGATE(idt[T_IRQ0+9], 0, CPU_GDT_KCODE, &tirq9, 0);  // +9
  SETGATE(idt[T_IRQ0+10], 0, CPU_GDT_KCODE, &tirq10, 0); // +10
  SETGATE(idt[T_IRQ0+11], 0, CPU_GDT_KCODE, &tirq11, 0); // +11
  SETGATE(idt[T_IRQ0+12], 0, CPU_GDT_KCODE, &tirq12, 0); // +12
  SETGATE(idt[T_IRQ0+13], 0, CPU_GDT_KCODE, &tirq13, 0); // +13
  SETGATE(idt[T_IRQ0+14], 0, CPU_GDT_KCODE, &tirq14, 0); // +14 (IRQ_IDE)
  SETGATE(idt[T_IRQ0+15], 0, CPU_GDT_KCODE, &tirq15, 0); // +15

  SETGATE(idt[T_SYSCALL], 0, CPU_GDT_KCODE, &tsystem, 3);
  SETGATE(idt[T_LTIMER], 0, CPU_GDT_KCODE, &tltimer, 0);
}

void
trap_init(void)
{
	// The first time we get called on the bootstrap processor,
	// initialize the IDT.  Other CPUs will share the same IDT.
	if (cpu_onboot())
		trap_init_idt();

	// Load the IDT into this processor's IDT register.
	asm volatile("lidt %0" : : "m" (idt_pd));

	// Check for the correct IDT and trap handler operation.
	if (cpu_onboot())
		trap_check_kernel();
}

const char *trap_name(int trapno)
{
	static const char * const excnames[] = {
		"Divide error",
		"Debug",
		"Non-Maskable Interrupt",
		"Breakpoint",
		"Overflow",
		"BOUND Range Exceeded",
		"Invalid Opcode",
		"Device Not Available",
		"Double Fault",
		"Coprocessor Segment Overrun",
		"Invalid TSS",
		"Segment Not Present",
		"Stack Fault",
		"General Protection",
		"Page Fault",
		"(unknown trap)",
		"x87 FPU Floating-Point Error",
		"Alignment Check",
		"Machine-Check",
		"SIMD Floating-Point Exception"
	};

	if (trapno < sizeof(excnames)/sizeof(excnames[0]))
		return excnames[trapno];
	if (trapno == T_SYSCALL)
		return "System call";
	if (trapno >= T_IRQ0 && trapno < T_IRQ0 + 16)
		return "Hardware Interrupt";
	return "(unknown trap)";
}

void
trap_print_regs(pushregs *regs)
{
	cprintf("  edi  0x%08x\n", regs->edi);
	cprintf("  esi  0x%08x\n", regs->esi);
	cprintf("  ebp  0x%08x\n", regs->ebp);
//	cprintf("  oesp 0x%08x\n", regs->oesp);	don't print - useless
	cprintf("  ebx  0x%08x\n", regs->ebx);
	cprintf("  edx  0x%08x\n", regs->edx);
	cprintf("  ecx  0x%08x\n", regs->ecx);
	cprintf("  eax  0x%08x\n", regs->eax);
}

void
trap_print(trapframe *tf)
{
	cprintf("TRAP frame at %p\n", tf);
	trap_print_regs(&tf->regs);
	cprintf("  es   0x----%04x\n", tf->es);
	cprintf("  ds   0x----%04x\n", tf->ds);
	cprintf("  trap 0x%08x %s\n", tf->trapno, trap_name(tf->trapno));
	cprintf("  err  0x%08x\n", tf->err);
	cprintf("  eip  0x%08x\n", tf->eip);
	cprintf("  cs   0x----%04x\n", tf->cs);
	cprintf("  flag 0x%08x\n", tf->eflags);
	cprintf("  esp  0x%08x\n", tf->esp);
	cprintf("  ss   0x----%04x\n", tf->ss);
}

void gcc_noreturn
trap(trapframe *tf)
{

  extern uint8_t e100_irq;
  int e100_irq_gate = T_IRQ0 + (int)e100_irq;
	// The user-level environment may have set the DF flag,
	// and some versions of GCC rely on DF being clear.
	asm volatile("cld" ::: "cc");

  // If it's a pagefault, check if it's one to blame on the user,
  // or this function will call trap_return itself.
  if(tf->trapno == T_PGFLT)
    pmap_pagefault(tf);

	// If this trap was anticipated, just use the designated handler.
	cpu *c = cpu_cur();
  proc *curr = proc_cur();

    // This is not this processe's home
  // cprintf("trap (%s -- %d) in %p (home %d)\n", 
  //   trap_name(tf->trapno), tf->trapno, curr, RRNODE(curr->home));

	if (c->recover)
		c->recover(tf, c->recoverdata);

  switch(tf->trapno) {
    case T_SYSCALL:
      syscall(tf);
      break;
    case T_LTIMER:
      net_tick();
      lapic_eoi();
      //cprintf("Timer Interrupt.\n");
      if(tf->cs & 3)
        proc_yield(tf);
      trap_return(tf);
    case T_IRQ0+IRQ_KBD:
      // cprintf("Keyboard interrupt\n");
      kbd_intr();
      lapic_eoi();
      trap_return(tf);
    case T_IRQ0+IRQ_SERIAL:
      // cprintf("Serial interrupt\n");
      lapic_eoi();
      serial_intr();
      trap_return(tf);

    case T_IRQ0+IRQ_SPURIOUS:
      cprintf("Spurious Interrupt. That's weird.\n");
      trap_return(tf);
  }
  
  if(tf->trapno == e100_irq_gate) { 
      e100_intr();
      lapic_eoi();
      trap_return(tf);
  }

  // USER MODE trap, reflect to parent
  if(tf->cs & 3) {
    // If we're on the right node, return here.
    if(RRNODE(curr->home) != net_node) {
      cprintf("trap on wrong node...%p returning to parent %d\n", 
        curr, RRNODE(curr->home));
      net_migrate(tf, RRNODE(curr->home), -1);
    }
    proc_ret(tf, -1);
  }

	// If we panic while holding the console lock,
	// release it so we don't get into a recursive panic that way.
	if (spinlock_holding(&cons_lock))
		spinlock_release(&cons_lock);

  char* msg = tf->cs & 3 ? "in user mode" : "in kernel";
	trap_print(tf);
	panic("unhandled trap %s", msg);
}


// Helper function for trap_check_recover(), below:
// handles "anticipated" traps by simply resuming at a new EIP.
static void gcc_noreturn
trap_check_recover(trapframe *tf, void *recoverdata)
{
	trap_check_args *args = recoverdata;
	tf->eip = (uint32_t) args->reip;	// Use recovery EIP on return
	args->trapno = tf->trapno;		// Return trap number
	trap_return(tf);
}

// Check for correct handling of traps from kernel mode.
// Called on the boot CPU after trap_init() and trap_setup().
void
trap_check_kernel(void)
{
	assert((read_cs() & 3) == 0);	// better be in kernel mode!

	cpu *c = cpu_cur();
	c->recover = trap_check_recover;
	trap_check(&c->recoverdata);
	c->recover = NULL;	// No more mr. nice-guy; traps are real again

	cprintf("trap_check_kernel() succeeded!\n");
}

// Check for correct handling of traps from user mode.
// Called from user() in kern/init.c, only in lab 1.
// We assume the "current cpu" is always the boot cpu;
// this true only because lab 1 doesn't start any other CPUs.
void
trap_check_user(void)
{
	assert((read_cs() & 3) == 3);	// better be in user mode!

	cpu *c = &cpu_boot;	// cpu_cur doesn't work from user mode!
	c->recover = trap_check_recover;
	trap_check(&c->recoverdata);
	c->recover = NULL;	// No more mr. nice-guy; traps are real again

	cprintf("trap_check_user() succeeded!\n");
}

void after_div0();
void after_breakpoint();
void after_overflow();
void after_bound();
void after_illegal();
void after_gpfault();
void after_priv();

// Multi-purpose trap checking function.
void
trap_check(void **argsp)
{
	volatile int cookie = 0xfeedface;
	volatile trap_check_args args;
	*argsp = (void*)&args;	// provide args needed for trap recovery

	// Try a divide by zero trap.
	// Be careful when using && to take the address of a label:
	// some versions of GCC (4.4.2 at least) will incorrectly try to
	// eliminate code it thinks is _only_ reachable via such a pointer.
	args.reip = after_div0;
	asm volatile("div %0,%0; after_div0:" : : "r" (0));
	assert(args.trapno == T_DIVIDE);

	// Make sure we got our correct stack back with us.
	// The asm ensures gcc uses ebp/esp to get the cookie.
	asm volatile("" : : : "eax","ebx","ecx","edx","esi","edi");
	assert(cookie == 0xfeedface);

	// Breakpoint trap
	args.reip = after_breakpoint;
	asm volatile("int3; after_breakpoint:");
	assert(args.trapno == T_BRKPT);

	// Overflow trap
	args.reip = after_overflow;
	asm volatile("addl %0,%0; into; after_overflow:" : : "r" (0x70000000));
	assert(args.trapno == T_OFLOW);

	// Bounds trap
	args.reip = after_bound;
	int bounds[2] = { 1, 3 };
	asm volatile("boundl %0,%1; after_bound:" : : "r" (0), "m" (bounds[0]));
	assert(args.trapno == T_BOUND);

	// Illegal instruction trap
	args.reip = after_illegal;
	asm volatile("ud2; after_illegal:");	// guaranteed to be undefined
	assert(args.trapno == T_ILLOP);

	// General protection fault due to invalid segment load
	args.reip = after_gpfault;
	asm volatile("movl %0,%%fs; after_gpfault:" : : "r" (-1));
	assert(args.trapno == T_GPFLT);

	// General protection fault due to privilege violation
	if (read_cs() & 3) {
		args.reip = after_priv;
		asm volatile("lidt %0; after_priv:" : : "m" (idt_pd));
		assert(args.trapno == T_GPFLT);
	}

	// Make sure our stack cookie is still with us
	assert(cookie == 0xfeedface);

	*argsp = NULL;	// recovery mechanism not needed anymore
}

