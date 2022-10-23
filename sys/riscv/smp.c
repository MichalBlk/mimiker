#include <sys/boot.h>
#include <sys/smp.h>
#include <riscv/pte.h>
#include <riscv/riscvreg.h>
#include <riscv/sbi.h>

__boot_data static volatile vaddr_t _smp_ap_boot = (vaddr_t)smp_ap_boot;

__boot_text __noreturn void smp_md_ap_init(smp_bootargs_t *ba) {
  unsigned cpuid = ba->cpuid;

  /* Move to VM boot stage. */
#if __riscv_xlen == 64
  const paddr_t satp = SATP_MODE_SV39 | (ba->kernel_pd >> PAGE_SHIFT);
#else
  const paddr_t satp = SATP_MODE_SV32 | (ba->kernel_pd >> PAGE_SHIFT);
#endif

  vaddr_t sp = (vaddr_t)ba->stack_va;

  /* Temporarily set the trap vector. */
  csr_write(stvec, _smp_ap_boot);

  __asm __volatile("mv a0, %0\n\t"
                   "mv sp, %1\n\t"
                   "csrw satp, %2\n\t"
                   "sfence.vma\n\t"
                   "1: j 1b" /* triggers instruction fetch page fault */
                   :
                   : "r"(cpuid), "r"(sp), "r"(satp)
                   : "a0");
  __unreachable();
}

bool smp_md_cpu_runnable(phandle_t node, unsigned hwid) {
  return FDT_hasprop(node, "mmu-type");
}

extern void cpu_exception_handler(void);

void smp_md_ap_configure(unsigned cpuid) {
  /* Set initial register values.
   * TODO: set TP to PCPU. */
  csr_write(sscratch, 0);

  /*
   * Set trap vector base address:
   *  - MODE = Direct - all exceptions set PC to specified BASE
   */
  csr_write(stvec, cpu_exception_handler);

  /*
   * NOTE: respective interrupts will be enabled by appropriate device drivers
   * while registering an interrupt handling routine.
   */
  csr_clear(sie, SIP_SEIP | SIP_STIP | SIP_SSIP);
  csr_clear(sie, SIE_SEIE | SIE_STIE | SIE_SSIE);
}

int smp_md_start_ap(unsigned hwid, paddr_t entry, register_t arg) {
  return sbi_hsm_hart_start(hwid, entry, arg) != SBI_SUCCESS;
}
