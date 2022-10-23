#include <sys/klog.h>
#include <sys/malloc.h>
#include <sys/pmap.h>
#include <sys/smp.h>

static unsigned smp_bp_hwid;       /* BP hardware ID */
static unsigned smp_next_apid = 1; /* next AP ID to grant */
static unsigned smp_ncpus;         /* total count of runnable CPUs */

/* Set to 1 once we're ready to unleash APs. */
static volatile int smp_ap_ready;

static smp_bootargs_t *smp_bootargs;

__noreturn void smp_ap_boot(unsigned cpuid) {
  smp_md_ap_configure(cpuid);

  /* TODO: should be atomic. */
  while (!smp_ap_ready)
    continue;

  /* TODO: implement the remaining part. */
  for (;;)
    continue;

  __unreachable();
}

void smp_set_bp_hwid(unsigned hwid) {
  smp_bp_hwid = hwid;
}

extern paddr_t kernel_pd;

static __used bool smp_start_ap(phandle_t node, unsigned hwid) {
  if (hwid == smp_bp_hwid)
    return true;

  assert(smp_next_apid < smp_ncpus);
  unsigned cpuid = smp_next_apid;

  smp_bootargs_t *ba = &smp_bootargs[cpuid - 1];
  ba->kernel_pd = kernel_pd;
  ba->stack_va = (vaddr_t)ba->stack;
  ba->cpuid = cpuid;

  paddr_t ap_boot_pa;
  paddr_t ba_pa;

  if (!pmap_kextract((vaddr_t)smp_ap_boot, &ap_boot_pa))
    panic("Failed to obtain PA of smp_ap_boot");
  if (!pmap_kextract((vaddr_t)ba, &ba_pa))
    panic("Failed to obtain PA of an SMP bootargs struct for AP %u", hwid);

  klog("Starting AP %u", hwid);

  if (smp_md_start_ap(hwid, ap_boot_pa, (register_t)ba_pa)) {
    klog("Failed to start AP %u", hwid);
    smp_ncpus--;
    return false;
  }

  smp_next_apid++;
  return true;
}

void init_smp(void) {
#if SMP
  /* Count runnable CPUs. */
  int ncpus = FDT_cpu_foreach(smp_md_cpu_runnable);
  if (smp_ncpus <= 0)
    panic("No runnable CPU!");
  smp_ncpus = ncpus;
  klog("Detected %u runnable CPUs", smp_ncpus);

  assert(!smp_bootargs);
  smp_bootargs =
    kmalloc(M_TEMP, sizeof(smp_bootargs_t) * (smp_ncpus - 1), M_WAITOK);

  int nstarted = FDT_cpu_foreach(smp_start_ap);
  assert(nstarted > 0);
  klog("Started %d application processors", nstarted - 1);

  /* TODO: release `bootargs` after APs proceed to become idle threads. */
#endif /* !SMP */
}
