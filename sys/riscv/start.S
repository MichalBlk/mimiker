#include <riscv/abi.h>
#include <riscv/asm.h>
#include <riscv/vm_param.h>

#include "assym.h"

#define INIT_STACK_SIZE 512

	.option nopic
	.section .boot,"ax",@progbits

/*
 * The following entry point is used when booting via SBI firmware.
 * It must be placed at the beginning of the .boot section.
 * Hart start register state:
 *  - satp = 0 (i.e. MMU's disabled)
 *  - sstatus.SIE = 0 (i.e. supervisor interrupts are disabled)
 *  - a0 = hart ID
 *  - a1 = dtbp
 *  - all other registers remain in an undefined state
 */
_ENTRY(_start)
	PTR_L	gp, _global_pointer

	/*
	 * NOTE: We assume that an SMP system supports HSM SBI extension.
	 * Thereby, we will have a single hart (so called Boot Processor (BP))
	 * entering the kernel via `_start`.
	 */

	/* Move to the initial stack. */
	PTR_LA	sp, _init_stack_end

	tail	riscv_init
_END(_start)

/*
 * Application Processor (AP) entry point.
 * Hart start register state:
 *  - satp = 0 (i.e. MMU's disabled)
 *  - sstatus.SIE = 0 (i.e. supervisor interrupts are disabled)
 *  - a0 = hart ID
 *  - a1 = `smp_boot_t` pointer
 *  - all other registers remain in an undefined state
 */
_ENTRY(smp_md_ap_entry)
	PTR_L	gp, _global_pointer

	/* Move to the initial stack. */
	REG_LI	t0, SMP_BOOTARGS_SIZE
	PTR_ADD	sp, a1, t0

	mv	a0, a1
	tail smp_md_ap_init
_END(smp_md_ap_entry)

	.section .boot.data,"aw",@progbits
_global_pointer:
#if __riscv_xlen == 32
	.long	__global_pointer$
#else
	.quad	__global_pointer$
#endif

	.section .boot.bss,"aw",@nobits

	.align STACK_ALIGN
_init_stack:
	.space INIT_STACK_SIZE
_init_stack_end:

# vim: sw=8 ts=8 et
