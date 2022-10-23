#ifndef _SYS_SMP_H_
#define _SYS_SMP_H_

#include <sys/cdefs.h>
#include <sys/fdt.h>
#include <sys/types.h>
#include <machine/abi.h>

#define SMP_AP_INIT_STACK_SIZE 512

typedef struct smp_bootargs {
  paddr_t kernel_pd;
  vaddr_t stack_va;
  unsigned cpuid;
  uint8_t stack[SMP_AP_INIT_STACK_SIZE] __aligned(STACK_ALIGN);
} smp_bootargs_t;

bool smp_md_cpu_runnable(phandle_t node, unsigned hwid);
void smp_md_ap_configure(unsigned cpuid);
int smp_md_start_ap(unsigned hwid, paddr_t entry, register_t arg);

void smp_set_bp_hwid(unsigned hwid);
__noreturn void smp_ap_boot(unsigned cpuid);
void init_smp(void);

#endif /* !_SYS_SMP_H_ */
