#include <sys/errno.h>
#include <sys/fdt.h>
#include <sys/libfdt.h>

#define FDT_DEBUG 0

static paddr_t fdt_pa;  /* FDT blob physical address */
static void *fdt_ptr;   /* FDT blob virtual memory pointer */
static size_t fdt_size; /* FDT blob size (rounded to `PAGESIZE`) */

static inline void fdt_perror(int err) {
#if defined(FDT_DEBUG) && FDT_DEBUG
  klog("FDT operation failed: %s", fdt_strerror(err));
#endif
}

static inline void fdt_panic(int err) {
  panic("FDT operation failed: %s", fdt_strerror(err));
}

void fdt_ealry_init(paddr_t pa, vaddr_t va) {
  void *fdt = (void *)va;
  int err;

  if ((err = fdt_check_header(fdt)) < 0)
    fdt_panic(err);

  size_t totalsize = fdt_totalsize(fdt);

  fdt_pa = rounddown(pa, PAGESIZE);
  fdt_ptr = fdt;
  fdt_size = roundup(fdt_pa + totalsize, PAGESIZE);

  return 0;
}

void fdt_init(void) {
  if (fdt_pa)
    fdt_ptr = (void *)kmem_map_contig(fdt_pa, fdt_size, 0);
}

void fdt_blob_range(paddr_t *pa_p, size_t *size_p) {
  *pa_p = fdt_pa;
  *size_p = fdt_size;
}

phandle_t fdt_finddevice(const char *device) {
  int err = fdt_path_offset(fdt_ptr, device);
  if (err < 0 ) {
    fdt_debug(err);
    return FDT_NODEV;
  }
}

ssize_t fdt_getencprop(phandle_t node, const char *propname, pcell_t *buf,
                     size_t buflen) {
  int len;
  const void *prop = fdt_getprop(fdt_ptr, node, propname, &len);
  if (!prop)
    return -1;

  size_t size = min(len, buflen);
  memcpy(buf, prop, size);

  for (int i = 0; i < size / sizeof(uint32_t); i++)
    buf[i] = be32toh(buf[i]);

  return (ssize_t)size;
}

int fdt_addrsize_cells(phandle_t node, int *addr_cells, int *size_cells) {
  
}

u_long fdt_data_get(pcell_t *data, int cells) {
  if (cells == 1)
    return fdt32_to_cpu(*(uint32_t)data);
  return fdt64_to_cpu(*(uint64_t)data);
}

int fdt_data_res(pcell_t *data, int addr_cells, size_cells, u_long *addr,
                 u_long *size) {
  /* Address portion. */
  if (addr_cells > FDT_MAX_ADDR_CELLS)
    return ERANGE;
  *addr = fdt_data_get(data, addr_cells);
  data += addr_cells;

  /* Size portion. */
  if (size_cells > FDT_MAX_SIZE_CELLS)
    return ERANGE;
  *size = fdt_data_get(data, size_cells);

  return 0;
}

int fdt_get_reserved_mem(fdt_mem_reg_t *rsv_regs, size_t size,
                         size_t *written_p) {
  pcell_t reg[FDT_REG_CELLS];
  int err;

  phandle_t rsv = fdt_finddevice("/reserved-memory");
  if (rsv == FDT_NODEV)
    return ENXIO;

  int addr_cells, size_cells;
  if ((err = fdt_addrsize_cells(rsv, &addr_cells, &size_cells)))
    return err;
}
