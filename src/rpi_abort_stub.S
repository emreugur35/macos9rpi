/*
 * rpi_abort_stub.S - Low level helpers for macos9rpi
 *
 *  - Data abort stub that lets C code "skip" a faulting load/store that
 *    the PowerPC emulator issued to an unmapped / read-only Mac address
 *    (the bare-metal equivalent of SheepShaver's SIGSEGV skip logic).
 *  - Helper to run a function on a different stack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

	.text
	.arm

/*
 * Frame layout (pointer passed in r0 to rpi_data_abort_handler):
 *   [sp+0]  spsr
 *   [sp+4]  r0 ... [sp+52] r12
 *   [sp+56] pc of faulting instruction
 */
	.globl	rpi_data_abort_stub
rpi_data_abort_stub:
	sub	lr, lr, #8			/* lr = address of faulting instruction */
	sub	sp, sp, #4			/* keep 8 byte stack alignment (15 words pushed) */
	stmfd	sp!, {r0-r12, lr}
	mrs	r0, spsr
	stmfd	sp!, {r0}
	mov	r0, sp
	bl	rpi_data_abort_handler		/* returns != 0 if handled */
	cmp	r0, #0
	beq	1f

	ldmfd	sp!, {r0}
	msr	spsr_cxsf, r0
	ldmfd	sp!, {r0-r12, lr}
	add	sp, sp, #4
	movs	pc, lr				/* return, restores CPSR from SPSR */

1:	/* not handled: pass on to Circle's handler with the original state */
	ldmfd	sp!, {r0}
	msr	spsr_cxsf, r0
	ldmfd	sp!, {r0-r12, lr}
	add	sp, sp, #4
	add	lr, lr, #8
	ldr	pc, =DataAbortStub

/*
 * void rpi_call_on_stack (int (*func) (void), void *stack_top)
 */
	.globl	rpi_call_on_stack
rpi_call_on_stack:
	push	{r4, lr}
	mov	r4, sp
	mov	sp, r1
	blx	r0
	mov	sp, r4
	pop	{r4, pc}

	.ltorg
